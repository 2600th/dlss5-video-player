// dlss5-convert: the planner (ConvertCommandLine.h) on its own, and the real
// dlss5-convert.exe driving this executable as a stand-in player.
//
//   ConvertTests.exe <directory holding dlss5-convert.exe>
//
// Run with --render or --probe, this executable is the player: it records the
// command line it was given and answers by the input's name - "refuse" is
// refused (3), "fail" fails (4), "slow" renders until Ctrl+Break and is then
// cancelled (5), anything else writes its output and is done.

#include "ConvertCommandLine.h"
#include "InheritedHandles.h"
#include "KillOnCloseJob.h"
#include "TestSupport.h"
#include "TestEnvironment.h"
#include "Utf8Text.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace convert_command;
namespace fs = std::filesystem;

// ---- the stand-in player ------------------------------------------------

std::atomic<bool> g_broken{false};
BOOL WINAPI OnBreak(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    g_broken = true;
    return TRUE;
}

int FakePlayer(const std::vector<std::wstring>& arguments)
{
    wchar_t log[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"CONVERT_TEST_LOG", log, MAX_PATH)) {
        std::wofstream out(log, std::ios::app);
        for (const auto& argument : arguments) out << argument << L'|';
        out << L'\n';
    }
    if (arguments[0] == L"--probe") { std::cout << "kind=video\n"; return kExitOk; }
    std::wstring input, output;
    for (size_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == L"--render") input = arguments[index + 1];
        if (arguments[index] == L"--out") output = arguments[index + 1];
    }
    const std::wstring name = fs::path(input).filename().wstring();
    if (name.find(L"refuse") != std::wstring::npos) { std::cerr << "refused: the fake runtime is busy\n"; return kExitRefused; }
    if (name.find(L"fail") != std::wstring::npos) { std::cerr << "failed: the fake pass broke\n"; return kExitFailed; }
    if (name.find(L"slow") != std::wstring::npos) {
        SetConsoleCtrlHandler(OnBreak, TRUE);
        std::cout << "1/1 Neural rendering: 0 frames\n" << std::flush;
        for (int wait = 0; wait < 200 && !g_broken; ++wait) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (g_broken) { std::cerr << "cancelled\n"; return kExitCancelled; }
    }
    std::cout << "1/1 Neural rendering: 30/30 frames (100%)\n";
    std::ofstream(fs::path(output), std::ios::binary) << "rendered";
    std::cout << "done: " << utf8_text::FromWide(output) << "\n";
    return kExitOk;
}

// ---- the planner --------------------------------------------------------

Options ParseLine(std::vector<std::wstring> arguments) { return Parse(arguments); }

void planner_test()
{
    // Render options pass through; a stage option given twice is refused.
    const auto plain = ParseLine({L"a.mp4", L"--stages", L"sr,nr", L"--height", L"2160"});
    CHECK(plain.mode == Mode::Convert);
    CHECK((plain.renderOptions == std::vector<std::wstring>{L"--stages", L"sr,nr", L"--height", L"2160"}));
    CHECK(ParseLine({L"a.mp4", L"--height", L"1080", L"--height", L"1440"}).mode == Mode::BadArguments);
    // Neural settings, the encoder ladder and SR history reach the player as given.
    const auto tuned = ParseLine({L"a.mp4", L"--passes", L"3", L"--intensity", L"1.5", L"--encode", L"high",
                                  L"--color-strength", L"0.8"});
    CHECK((tuned.renderOptions == std::vector<std::wstring>{L"--passes", L"3", L"--intensity", L"1.5",
                                                           L"--encode", L"high", L"--color-strength", L"0.8"}));
    CHECK(ParseLine({L"a.mp4", L"--stages", L"sr", L"--history", L"per-frame"}).renderOptions.back() == L"per-frame");
    CHECK(ParseLine({}).mode == Mode::BadArguments);
    CHECK(ParseLine({L"a.mp4", L"--bogus"}).mode == Mode::BadArguments);
    CHECK(ParseLine({L"a.mp4", L"--format", L"avi"}).mode == Mode::BadArguments);
    CHECK(ParseLine({L"a.mp4", L"--format", L"JPEG"}).format == L"jpg");
    CHECK(ParseLine({L"a.mp4", L"--out", L"x.mkv", L"--out-dir", L"d"}).mode == Mode::BadArguments);
    CHECK(ParseLine({L"a.mp4", L"--suffix", L""}).mode == Mode::BadArguments);
    CHECK(ParseLine({L"a.mp4", L"--suffix", L"a/b"}).mode == Mode::BadArguments);
    CHECK(ParseLine({L"a.mp4", L"--suffix", L"", L"--out-dir", L"d"}).mode == Mode::Convert);
    CHECK(ParseLine({L"--", L"-odd.mp4"}).inputs == std::vector<std::wstring>{L"-odd.mp4"});
    CHECK(ParseLine({L"--help"}).mode == Mode::Help);
    const auto probe = ParseLine({L"probe", L"a.mp4", L"--json", L"--capabilities"});
    CHECK(probe.mode == Mode::Probe && probe.json && probe.capabilities);
    CHECK(ParseLine({L"probe", L"a.mp4", L"b.mp4"}).mode == Mode::BadArguments);
    CHECK(ParseLine({L"probe", L"a.mp4", L"--stages", L"sr"}).mode == Mode::BadArguments);

    // Where results go.
    Options options = ParseLine({L"x"});
    CHECK(OutputFor(L"C:/v/clip.mp4", {}, options) == fs::path(L"C:/v/clip-dlss.mkv"));
    CHECK(OutputFor(L"C:/v/anim.gif", {}, options) == fs::path(L"C:/v/anim-dlss.gif"));
    CHECK(OutputFor(L"C:/v/photo.JPG", {}, options) == fs::path(L"C:/v/photo-dlss.png"));
    options.format = L"mp4";
    options.outDir = L"D:/out";
    CHECK(OutputFor(L"C:/v/a/b/clip.mkv", L"C:/v", options) == fs::path(L"D:/out/a/b/clip-dlss.mp4"));
    CHECK(OutputFor(L"C:/v/clip.mkv", {}, options) == fs::path(L"D:/out/clip-dlss.mp4"));
    CHECK(LooksLikeOutput(L"C:/v/clip-DLSS.mkv", L"-dlss"));
    CHECK(!LooksLikeOutput(L"C:/v/-dlss.mkv", L"-dlss"));
    CHECK(!LooksLikeOutput(L"C:/v/clip.mkv", L"-dlss"));

    // What the player is handed.
    Options quiet = ParseLine({L"x", L"--preset", L"gentle", L"-q"});
    CHECK((PlayerArguments(quiet, L"C:/v/a.mp4", L"C:/v/a-dlss.mkv") ==
           std::vector<std::wstring>{L"--render", L"C:/v/a.mp4", L"--preset", L"gentle", L"--out", L"C:/v/a-dlss.mkv", L"--quiet"}));

    // One exit code for a batch.
    using O = Outcome;
    CHECK_EQ(kExitOk, BatchExitCode(std::vector<O>{O::Done, O::Skipped}));
    CHECK_EQ(kExitRefused, BatchExitCode(std::vector<O>{O::Done, O::Refused, O::BadArguments}));
    CHECK_EQ(kExitFailed, BatchExitCode(std::vector<O>{O::Refused, O::Failed}));
    CHECK_EQ(kExitCancelled, BatchExitCode(std::vector<O>{O::Failed, O::Cancelled}));
    CHECK_EQ(kExitBadArguments, BatchExitCode(std::vector<O>{O::BadArguments}));
    CHECK_EQ(kExitRefused, BatchExitCode(std::vector<O>{O::Refused, O::NotStarted}));
    CHECK_EQ(kExitOk, BatchExitCode(std::vector<O>{}));
}

// ---- the real front end -------------------------------------------------

struct Result {
    int exitCode{-1};
    std::string output;
};

// Runs dlss5-convert.exe with this executable as its player, stdout and
// stderr captured together.
Result RunConvert(const fs::path& convert, std::vector<std::wstring> arguments, DWORD extraFlags = 0,
                  const std::function<void(DWORD)>& whileRunning = {})
{
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    arguments.insert(arguments.end(), {L"--player", self});
    std::wstring command = L"\"" + convert.wstring() + L"\"";
    for (const auto& argument : arguments) command += L" \"" + argument + L"\"";
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    CHECK(CreatePipe(&read, &write, &inherit, 0) != FALSE);
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = write;
    const InheritedHandles handles{write};
    STARTUPINFOEXW startup{};
    startup.StartupInfo = si;
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = handles.AttributeList();
    PROCESS_INFORMATION info{};
    const BOOL started = CreateProcessW(convert.c_str(), command.data(), nullptr, nullptr, handles.InheritHandles(),
                                        extraFlags | handles.CreationFlags(), nullptr, nullptr, &startup.StartupInfo, &info);
    CloseHandle(write);
    Result result;
    CHECK(started != FALSE);
    if (!started) { CloseHandle(read); return result; }
    CloseHandle(info.hThread);
    std::thread reader([&] {
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(read, buffer, sizeof(buffer), &got, nullptr) && got) result.output.append(buffer, got);
    });
    if (whileRunning) whileRunning(info.dwProcessId);
    if (WaitForSingleObject(info.hProcess, 60000) != WAIT_OBJECT_0) TerminateProcess(info.hProcess, 99);
    reader.join();
    DWORD code = 0;
    GetExitCodeProcess(info.hProcess, &code);
    result.exitCode = static_cast<int>(code);
    CloseHandle(info.hProcess);
    CloseHandle(read);
    return result;
}

bool Has(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

void Touch(const fs::path& file, std::string_view text = "source")
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

void front_end_test(const fs::path& convert)
{
    const fs::path root = fs::temp_directory_path() / (L"ConvertTests-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    const fs::path media = root / L"media";
    Touch(media / L"a.mp4");
    Touch(media / L"a-dlss.mkv", "an earlier result");  // a's result, already there
    Touch(media / L"b.mkv");
    Touch(media / L"refuse.mp4");
    Touch(media / L"notes.txt");
    Touch(media / L"sub" / L"c.mov");
    const fs::path log = root / L"player.log";
    SetEnvironmentVariableW(L"CONVERT_TEST_LOG", log.c_str());

    // A folder: a's result is there and skipped, b is converted, the refused
    // one is reported, the text file and the earlier result are not videos
    // to convert. The worst outcome, refused, is the exit code.
    const Result folder = RunConvert(convert, {media.wstring()});
    CHECK_EQ(kExitRefused, folder.exitCode);
    CHECK(Has(folder.output, "skipped: a.mp4"));
    CHECK(Has(folder.output, "done: "));
    CHECK(Has(folder.output, "refused: the fake runtime is busy"));
    CHECK(Has(folder.output, "summary: 3 files, 1 done, 1 skipped, 1 refused"));
    CHECK(Has(folder.output, "[2/3]"));
    CHECK(!Has(folder.output, "notes.txt") && !Has(folder.output, "c.mov"));
    CHECK(fs::exists(media / L"b-dlss.mkv"));
    std::ifstream earlier(media / L"a-dlss.mkv");
    CHECK_EQ(std::string("an earlier result"), std::string(std::istreambuf_iterator<char>(earlier), {}));

    // Recursive into an output folder that keeps the subfolders, as MP4, with
    // --overwrite and a JSON report; --fail-fast stops at the refusal.
    const fs::path out = root / L"out";
    const fs::path report = root / L"report.json";
    const Result deep = RunConvert(convert, {media.wstring(), L"-r", L"--out-dir", out.wstring(), L"--format", L"mp4",
                                             L"--report", report.wstring(), L"--stages", L"sr", L"--height", L"2160"});
    CHECK_EQ(kExitRefused, deep.exitCode);
    CHECK(fs::exists(out / L"a-dlss.mp4") && fs::exists(out / L"b-dlss.mp4") && fs::exists(out / L"sub" / L"c-dlss.mp4"));
    std::ifstream json(report);
    const std::string reported((std::istreambuf_iterator<char>(json)), {});
    CHECK(Has(reported, "\"exitCode\":3"));
    CHECK(Has(reported, "\"result\":\"refused\""));
    CHECK(Has(reported, "c-dlss.mp4"));
    std::ifstream calls(log);
    const std::string recorded((std::istreambuf_iterator<char>(calls)), {});
    CHECK(Has(recorded, "--stages|sr|--height|2160|--out|"));
    calls.close();

    const Result fast = RunConvert(convert, {(media / L"refuse.mp4").wstring(), (media / L"sub" / L"c.mov").wstring(),
                                             L"--fail-fast", L"--overwrite"});
    CHECK_EQ(kExitRefused, fast.exitCode);
    CHECK(Has(fast.output, "not started: c.mov"));

    // A dry run runs nothing, and names what it would.
    fs::remove(log, ignored);
    const Result dry = RunConvert(convert, {(media / L"b.mkv").wstring(), L"--overwrite", L"--dry-run"});
    CHECK_EQ(kExitOk, dry.exitCode);
    CHECK(Has(dry.output, "would run: "));
    if (fs::exists(log)) { std::ifstream stray(log); std::cerr << "player ran during a dry run: " << std::string(std::istreambuf_iterator<char>(stray), {}) << '\n'; }
    CHECK(!fs::exists(log));

    // Problems with the command line are the script's, before anything runs.
    CHECK_EQ(kExitBadArguments, RunConvert(convert, {(root / L"missing.mp4").wstring()}).exitCode);
    CHECK_EQ(kExitBadArguments, RunConvert(convert, {media.wstring(), L"--out", (root / L"x.mkv").wstring()}).exitCode);
    const Result probe = RunConvert(convert, {L"probe", (media / L"b.mkv").wstring()});
    CHECK_EQ(kExitOk, probe.exitCode);
    CHECK(Has(probe.output, "kind=video"));

    // Ctrl+Break cancels the file being converted and stops the batch. Sent to
    // the front end's own process group, which the stand-in shares, and only
    // where this test runs with a console to send it through.
    Touch(media / L"slow.mp4");
    if (GetConsoleWindow() || GetConsoleCP()) {
        const Result cancelled = RunConvert(convert, {(media / L"slow.mp4").wstring(), (media / L"b.mkv").wstring(), L"--overwrite"},
            CREATE_NEW_PROCESS_GROUP, [](DWORD group) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, group);
            });
        CHECK_EQ(kExitCancelled, cancelled.exitCode);
        CHECK(Has(cancelled.output, "cancelled"));
        CHECK(Has(cancelled.output, "not started: b.mkv"));
    } else {
        std::cout << "No console: the Ctrl+Break case was not run.\n";
    }
    fs::remove_all(root, ignored);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::vector<std::wstring> arguments(argv + 1, argv + argc);
    if (!arguments.empty() && (arguments[0] == L"--render" || arguments[0] == L"--probe")) return FakePlayer(arguments);
    test_support::ContainChildProcesses();
    planner_test();
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path convert = (argc > 1 ? fs::path(argv[1]) : fs::path(self).parent_path()) / L"dlss5-convert.exe";
    if (fs::exists(convert)) front_end_test(convert);
    else { std::cerr << "dlss5-convert.exe was not found beside the tests.\n"; ++test_support::failure_count; }
    if (test_support::failure_count) return 1;
    std::cout << "ConvertTests passed.\n";
    return 0;
}
