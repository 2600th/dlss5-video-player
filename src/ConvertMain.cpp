// dlss5-convert.exe: the console front end for DLSSVideoPlayer.exe --render
// and --probe. What it decides is ConvertCommandLine.h; this is the part that
// touches the system - finding files, running the player, relaying its output
// and Ctrl+C, and writing the summary.

#include "ConvertCommandLine.h"
#include "InheritedHandles.h"
#include "JsonEscape.h"
#include "KillOnCloseJob.h"
#include "Utf8Text.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace convert_command;
namespace fs = std::filesystem;

// Writes lines to one of this process's streams: UTF-16 to a console, UTF-8
// to a file or a pipe, which is what the player itself does.
class Stream {
public:
    explicit Stream(DWORD which) : handle_(GetStdHandle(which)) {}
    void Line(std::wstring_view line)
    {
        std::lock_guard lock(mutex_);
        if (!handle_ || handle_ == INVALID_HANDLE_VALUE) return;
        std::wstring text(line);
        text += L"\r\n";
        DWORD mode = 0, written = 0;
        if (GetConsoleMode(handle_, &mode)) {
            WriteConsoleW(handle_, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
            return;
        }
        const std::string bytes = utf8_text::FromWide(text);
        WriteFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    }

private:
    HANDLE handle_;
    std::mutex mutex_;
};

Stream& Out() { static Stream stream(STD_OUTPUT_HANDLE); return stream; }
Stream& Err() { static Stream stream(STD_ERROR_HANDLE); return stream; }

// Ctrl+C reaches the player itself - it attaches to this console and stops its
// render as its own Cancel does - so here it only ends the batch after the
// current file. A second press ends the player too, for one that does not.
std::atomic<int> g_interrupts{0};
std::atomic<HANDLE> g_currentJob{nullptr};
BOOL WINAPI OnConsoleControl(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    if (g_interrupts.fetch_add(1) >= 1)
        if (const HANDLE job = g_currentJob.load()) TerminateJobObject(job, kExitCancelled);
    return TRUE;
}

fs::path ThisExecutable()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return fs::path(path);
}

std::wstring Quote(const std::wstring& argument)
{
    // CommandLineToArgvW's rules: backslashes are literal except before a quote.
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) return argument;
    std::wstring quoted(1, L'"');
    size_t slashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') { ++slashes; continue; }
        quoted.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        quoted.push_back(c);
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

// Relays one of the player's streams line by line, with `prefix` in front,
// and keeps the last line it saw: the player ends with done:, refused:,
// failed: or cancelled.
void Relay(HANDLE pipe, Stream& to, const std::wstring& prefix, std::wstring& last)
{
    std::string pending;
    char buffer[4096];
    DWORD read = 0;
    const auto emit = [&](std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::wstring wide = utf8_text::ToWide(line);
        if (!wide.empty()) last = wide;
        to.Line(prefix + wide);
    };
    while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read) {
        pending.append(buffer, read);
        for (size_t end; (end = pending.find('\n')) != std::string::npos;) {
            emit(pending.substr(0, end));
            pending.erase(0, end + 1);
        }
    }
    if (!pending.empty()) emit(pending);
}

struct Run {
    int exitCode{kExitFailed};
    std::wstring lastOut, lastErr;
};

// Runs the player with `arguments`, its output relayed with `prefix`, and
// waits for it. The player is a Windows program: it writes to the pipes it is
// handed, and attaches to this console for Ctrl+C.
Run RunPlayer(const fs::path& player, const std::vector<std::wstring>& arguments, const std::wstring& prefix)
{
    Run run;
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &inherit, 0) || !CreatePipe(&errRead, &errWrite, &inherit, 0)) {
        run.lastErr = L"failed: the pipes to the player could not be created.";
        return run;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    const HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul == INVALID_HANDLE_VALUE ? nullptr : nul;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;
    const InheritedHandles handles{si.hStdInput, si.hStdOutput, si.hStdError};
    STARTUPINFOEXW startup{};
    startup.StartupInfo = si;
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = handles.AttributeList();
    std::wstring command = Quote(player.wstring());
    for (const std::wstring& argument : arguments) command += L" " + Quote(argument);
    const HANDLE job = CreateKillOnCloseJob();
    PROCESS_INFORMATION info{};
    const BOOL started = job && handles.Ready() &&
        CreateProcessW(player.c_str(), command.data(), nullptr, nullptr, handles.InheritHandles(),
                       CREATE_SUSPENDED | handles.CreationFlags(), nullptr, nullptr, &startup.StartupInfo, &info);
    CloseHandle(outWrite);
    CloseHandle(errWrite);
    if (si.hStdInput) CloseHandle(si.hStdInput);
    if (!started) {
        run.lastErr = L"failed: " + player.wstring() + L" could not be started (error " + std::to_wstring(GetLastError()) + L").";
        CloseHandle(outRead);
        CloseHandle(errRead);
        if (job) CloseHandle(job);
        return run;
    }
    AssignProcessToJobObject(job, info.hProcess);
    g_currentJob.store(job);
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    std::thread errors([&] { Relay(errRead, Err(), prefix, run.lastErr); });
    Relay(outRead, Out(), prefix, run.lastOut);
    errors.join();
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = kExitFailed;
    GetExitCodeProcess(info.hProcess, &code);
    run.exitCode = static_cast<int>(code);
    g_currentJob.store(nullptr);
    CloseHandle(info.hProcess);
    CloseHandle(outRead);
    CloseHandle(errRead);
    CloseHandle(job);
    return run;
}

struct Job {
    fs::path input, root, output;
    Outcome outcome{Outcome::Planned};
    int exitCode{kExitOk};
    std::wstring detail;
};

// Files in `folder` a scan converts, in name order so a batch is repeatable.
void Scan(const fs::path& folder, const Options& options, std::vector<Job>& jobs)
{
    std::vector<fs::path> found;
    std::error_code error;
    const auto consider = [&](const fs::directory_entry& entry) {
        std::error_code ignored;
        if (!entry.is_regular_file(ignored)) return;
        const fs::path& file = entry.path();
        if (!IsVideoExtension(file.extension().wstring()) || LooksLikeOutput(file, options.suffix)) return;
        found.push_back(file);
    };
    if (options.recursive) {
        for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error))
            consider(*it);
    } else {
        for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) consider(*it);
    }
    std::sort(found.begin(), found.end());
    for (const fs::path& file : found) jobs.push_back({file, folder, {}});
}

std::vector<Job> Expand(const Options& options, std::vector<std::wstring>& problems)
{
    std::vector<Job> jobs;
    for (const std::wstring& given : options.inputs) {
        std::error_code error;
        if (HasWildcard(given)) {
            const fs::path pattern(given);
            const fs::path folder = pattern.has_parent_path() ? pattern.parent_path() : fs::path(L".");
            std::vector<fs::path> matched;
            WIN32_FIND_DATAW data{};
            const HANDLE find = FindFirstFileW(pattern.c_str(), &data);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) matched.push_back(folder / data.cFileName);
                } while (FindNextFileW(find, &data));
                FindClose(find);
            }
            std::sort(matched.begin(), matched.end());
            if (matched.empty()) problems.push_back(L"nothing matches " + given);
            for (const fs::path& file : matched) jobs.push_back({fs::absolute(file, error), {}, {}});
        } else if (fs::is_directory(given, error)) {
            const size_t before = jobs.size();
            Scan(fs::absolute(given, error), options, jobs);
            if (jobs.size() == before) problems.push_back(L"no videos in " + given);
        } else if (fs::is_regular_file(given, error)) {
            jobs.push_back({fs::absolute(given, error), {}, {}});
        } else {
            problems.push_back(L"not found: " + given);
        }
    }
    return jobs;
}

void WriteReport(const fs::path& path, const std::vector<Job>& jobs, int exitCode)
{
    std::string json = "{\"exitCode\":" + std::to_string(exitCode) + ",\"files\":[";
    for (size_t i = 0; i < jobs.size(); ++i) {
        const Job& job = jobs[i];
        json += (i ? "," : "") + std::string("{\"input\":\"") + JsonEscape(utf8_text::FromWide(job.input.wstring())) +
                "\",\"output\":\"" + JsonEscape(utf8_text::FromWide(job.output.wstring())) +
                "\",\"result\":\"" + utf8_text::FromWide(OutcomeName(job.outcome)) +
                "\",\"exitCode\":" + std::to_string(job.exitCode) +
                ",\"detail\":\"" + JsonEscape(utf8_text::FromWide(job.detail)) + "\"}";
    }
    json += "]}\n";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!out) Err().Line(L"warning: the report could not be written to " + path.wstring());
}

int Convert(const Options& options, const fs::path& player)
{
    std::vector<std::wstring> problems;
    std::vector<Job> jobs = Expand(options, problems);
    for (const std::wstring& problem : problems) Err().Line(L"error: " + problem);
    if (jobs.empty()) {
        Err().Line(L"error: there is nothing to convert.");
        return kExitBadArguments;
    }
    if (!options.out.empty() && jobs.size() != 1) {
        Err().Line(L"error: --out names one file, and " + std::to_wstring(jobs.size()) + L" were found.");
        return kExitBadArguments;
    }

    // Decide every output before anything runs, so a clash or an existing
    // result is known up front rather than after an hour of rendering.
    std::map<std::wstring, size_t> claimed;
    for (size_t index = 0; index < jobs.size(); ++index) {
        Job& job = jobs[index];
        std::error_code error;
        job.output = fs::absolute(OutputFor(job.input, job.root, options), error);
        const std::wstring key = Lower(job.output.lexically_normal().wstring());
        if (Lower(job.input.lexically_normal().wstring()) == key) {
            job.outcome = Outcome::BadArguments;
            job.exitCode = kExitBadArguments;
            job.detail = L"the output would replace the input";
        } else if (const auto [at, inserted] = claimed.emplace(key, index); !inserted) {
            job.outcome = Outcome::BadArguments;
            job.exitCode = kExitBadArguments;
            job.detail = L"the same output as " + jobs[at->second].input.filename().wstring();
        } else if (!options.overwrite && fs::exists(job.output, error)) {
            job.outcome = Outcome::Skipped;
            job.detail = L"already converted: " + job.output.wstring() + L" (--overwrite replaces it)";
        }
    }

    const size_t total = jobs.size();
    const auto prefixOf = [&](size_t index) {
        return total > 1 ? L"[" + std::to_wstring(index + 1) + L"/" + std::to_wstring(total) + L"] " : std::wstring();
    };
    bool stopped = false;
    for (size_t index = 0; index < total; ++index) {
        Job& job = jobs[index];
        const std::wstring prefix = prefixOf(index);
        if (stopped) {
            job.outcome = Outcome::NotStarted;
            job.exitCode = kExitOk;
            job.detail = L"the batch was stopped before it";
            Out().Line(prefix + L"not started: " + job.input.filename().wstring() + L" - " + job.detail);
            continue;
        }
        if (job.outcome != Outcome::Planned) {
            (job.outcome == Outcome::Skipped ? Out() : Err()).Line(prefix + OutcomeName(job.outcome) + L": " +
                job.input.filename().wstring() + L" - " + job.detail);
            if (job.outcome != Outcome::Skipped && options.failFast) stopped = true;
            continue;
        }
        const std::vector<std::wstring> arguments = PlayerArguments(options, job.input, job.output);
        if (options.dryRun) {
            std::wstring line = prefix + L"would run: " + Quote(player.wstring());
            for (const std::wstring& argument : arguments) line += L" " + Quote(argument);
            Out().Line(line);
            continue;
        }
        if (!options.outDir.empty()) {
            std::error_code error;
            fs::create_directories(job.output.parent_path(), error);
            if (error) {
                job.outcome = Outcome::Failed;
                job.exitCode = kExitFailed;
                job.detail = L"the output folder could not be created: " + job.output.parent_path().wstring();
                Err().Line(prefix + L"failed: " + job.detail);
                if (options.failFast) stopped = true;
                continue;
            }
        }
        if (!options.quiet) Out().Line(prefix + job.input.wstring() + L" -> " + job.output.wstring());
        const Run run = RunPlayer(player, arguments, prefix);
        job.exitCode = run.exitCode;
        job.outcome = OutcomeOf(run.exitCode);
        job.detail = job.outcome == Outcome::Done ? run.lastOut : (!run.lastErr.empty() ? run.lastErr : run.lastOut);
        // The player's own verdict word, which the summary already names.
        for (const std::wstring_view word : {L"done: ", L"refused: ", L"failed: ", L"error: "})
            if (job.detail.starts_with(word)) { job.detail.erase(0, word.size()); break; }
        if (job.outcome == Outcome::Cancelled || g_interrupts.load()) stopped = true;
        else if (job.outcome != Outcome::Done && options.failFast) stopped = true;
    }

    std::vector<Outcome> outcomes;
    for (const Job& job : jobs) outcomes.push_back(job.outcome);
    const int exitCode = options.dryRun ? kExitOk : BatchExitCode(outcomes);
    if (total > 1 && !options.dryRun) {
        std::map<Outcome, size_t> counts;
        for (const Outcome outcome : outcomes) ++counts[outcome];
        std::wstring line = L"summary: " + std::to_wstring(total) + L" files";
        for (const auto& [outcome, count] : counts) line += L", " + std::to_wstring(count) + L" " + OutcomeName(outcome);
        Out().Line(line);
        for (size_t index = 0; index < total; ++index)
            if (jobs[index].outcome != Outcome::Done && jobs[index].outcome != Outcome::Skipped)
                Out().Line(L"  " + std::wstring(OutcomeName(jobs[index].outcome)) + L": " + jobs[index].input.wstring() +
                           (jobs[index].detail.empty() ? L"" : L" - " + jobs[index].detail));
    }
    if (!options.report.empty()) WriteReport(options.report, jobs, exitCode);
    return exitCode;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::vector<std::wstring> arguments(argv + 1, argv + argc);
    const Options options = Parse(arguments);
    switch (options.mode) {
        case Mode::Help: Out().Line(Usage()); return kExitOk;
        case Mode::Version: Out().Line(L"dlss5-convert " + utf8_text::ToWide(DLSS_VIDEO_PLAYER_VERSION)); return kExitOk;
        case Mode::BadArguments:
            Err().Line(L"error: " + options.error);
            Err().Line(L"Run dlss5-convert --help for the options.");
            return kExitBadArguments;
        default: break;
    }
    const fs::path player = options.player.empty() ? ThisExecutable().parent_path() / L"DLSSVideoPlayer.exe"
                                                   : fs::absolute(options.player);
    std::error_code error;
    if (!fs::is_regular_file(player, error)) {
        Err().Line(L"error: the player was not found at " + player.wstring() +
                   L". dlss5-convert runs DLSSVideoPlayer.exe from its own folder.");
        return kExitFailed;
    }
    SetConsoleCtrlHandler(OnConsoleControl, TRUE);
    if (options.mode == Mode::Probe) {
        std::vector<std::wstring> probe{L"--probe", fs::absolute(options.inputs.front(), error).wstring()};
        if (options.json) probe.push_back(L"--json");
        if (options.capabilities) probe.push_back(L"--capabilities");
        return RunPlayer(player, probe, {}).exitCode;
    }
    return Convert(options, player);
}
