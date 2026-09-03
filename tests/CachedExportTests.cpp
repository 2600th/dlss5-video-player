#include "MediaPipeline.h"
#include "TestSupport.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::filesystem::path ExecutableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

struct FixtureDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        (L"DLSS-CachedExportTests-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    FixtureDirectory() { CHECK(std::filesystem::create_directory(path)); }
    ~FixtureDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

void Write(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream file(path, std::ios::binary);
    file << text;
    CHECK(file.good());
}

std::string Read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    CHECK(file.is_open());
    return {std::istreambuf_iterator<char>(file), {}};
}

bool RunTool(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
             const std::filesystem::path& log)
{
    // Fixtures have no quotes or trailing directory separators in their arguments.
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    for (const auto& arg : args) command += L" \"" + arg + L"\"";
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return false;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = output;
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
    CloseHandle(output);
    if (!started) return false;
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 2000); }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (code != 0) std::cerr << Read(log) << '\n';
    return code == 0;
}

std::string Probe(const std::filesystem::path& helpers, const std::filesystem::path& file,
                  const std::filesystem::path& log, std::vector<std::wstring> args)
{
    args.insert(args.begin(), {L"-v", L"error"});
    args.push_back(file.wstring());
    CHECK(RunTool(helpers / L"ffprobe.exe", args, log));
    return Read(log);
}

size_t Count(std::string_view value, std::string_view needle)
{
    size_t count = 0, position = 0;
    while ((position = value.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

std::set<std::filesystem::path> Files(const std::filesystem::path& directory)
{
    std::set<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) files.insert(entry.path());
    return files;
}

void MaterializationPreservesFullVideoTest(const std::filesystem::path& helpers)
{
    FixtureDirectory fixture;
    const auto video = fixture.path / L"video.mkv";
    const auto audio = fixture.path / L"audio.wav";
    const auto output = fixture.path / L"source.mkv";
    const auto log = fixture.path / L"tool.log";
    CHECK(RunTool(helpers / L"ffmpeg.exe", {L"-v", L"error", L"-nostdin", L"-n",
        L"-f", L"lavfi", L"-i", L"color=red:s=64x48:r=5:d=3", L"-c:v", L"ffv1",
        video.wstring()}, log));
    CHECK(RunTool(helpers / L"ffmpeg.exe", {L"-v", L"error", L"-nostdin", L"-n",
        L"-f", L"lavfi", L"-i", L"sine=duration=1", audio.wstring()}, log));
    const auto materialized = MediaMaterializer(helpers).Run({video.wstring(), audio.wstring(), output, 3.0}, {});
    CHECK(materialized.ok);
    const auto frames = Probe(helpers, output, log, {L"-count_frames", L"-select_streams", L"v:0",
        L"-show_entries", L"stream=nb_read_frames", L"-of", L"default=noprint_wrappers=1"});
    CHECK(frames.find("nb_read_frames=15") != std::string::npos);
    const auto measured = ProbeMedia(helpers, output, {});
    CHECK(measured.ok);
    CHECK_EQ(int64_t{30000000}, measured.videoDuration100ns);
}

void MaterializationRejectsShortVideoWithLongAudioTest(const std::filesystem::path& helpers)
{
    FixtureDirectory fixture;
    const auto video = fixture.path / L"video.mkv";
    const auto audio = fixture.path / L"audio.wav";
    const auto output = fixture.path / L"source.mkv";
    const auto log = fixture.path / L"tool.log";
    CHECK(RunTool(helpers / L"ffmpeg.exe", {L"-v", L"error", L"-nostdin", L"-n",
        L"-f", L"lavfi", L"-i", L"color=red:s=64x48:r=5:d=1", L"-c:v", L"ffv1",
        video.wstring()}, log));
    CHECK(RunTool(helpers / L"ffmpeg.exe", {L"-v", L"error", L"-nostdin", L"-n",
        L"-f", L"lavfi", L"-i", L"sine=duration=3", audio.wstring()}, log));
    const auto materialized = MediaMaterializer(helpers).Run({video.wstring(), audio.wstring(), output, 3.0}, {});
    CHECK(!materialized.ok);
    CHECK_EQ(MaterializeError::ProcessFailed, materialized.error);
    CHECK(materialized.detail.find(L"incomplete") != std::wstring::npos);
    const auto decoded = ProbeMedia(helpers, output, {});
    CHECK(decoded.ok);
    CHECK_EQ(int64_t{10000000}, decoded.videoDuration100ns);
}

void ExportTests(const std::filesystem::path& helpers)
{
    FixtureDirectory fixture;
    const auto source = fixture.path / L"source.mkv";
    const auto cached = fixture.path / L"cached.mkv";
    const auto output = fixture.path / L"export.mkv";
    const auto log = fixture.path / L"tool.log";
    const auto subtitle = fixture.path / L"captions.srt";
    const auto metadata = fixture.path / L"chapters.txt";
    Write(subtitle, "1\n00:00:00,000 --> 00:00:00,750\nSource subtitle\n");
    Write(metadata, ";FFMETADATA1\ntitle=Source title\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=1000\ntitle=Opening\n");
    const auto ffmpeg = helpers / L"ffmpeg.exe";
    CHECK(RunTool(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n",
        L"-f", L"lavfi", L"-i", L"color=red:s=64x48:r=5:d=1",
        L"-f", L"lavfi", L"-i", L"sine=frequency=440:duration=1",
        L"-i", subtitle.wstring(), L"-f", L"ffmetadata", L"-i", metadata.wstring(),
        L"-map", L"0:v", L"-map", L"1:a", L"-map", L"1:a", L"-map", L"2:s", L"-map", L"2:s",
        L"-map_metadata", L"3", L"-map_chapters", L"3", L"-c:v", L"ffv1", L"-c:a", L"pcm_s16le", L"-c:s", L"srt",
        L"-metadata:s:a:0", L"language=eng", L"-metadata:s:a:1", L"language=fra",
        L"-metadata:s:s:0", L"language=eng", L"-metadata:s:s:1", L"language=fra", source.wstring()}, log));
    CHECK(RunTool(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi",
        L"-i", L"color=blue:s=64x48:r=5:d=1", L"-c:v", L"ffv1", cached.wstring()}, log));
    if (!std::filesystem::exists(source) || !std::filesystem::exists(cached)) return;
    const auto sourceBefore = Read(source);
    const auto cachedBefore = Read(cached);
    CachedVideoExporter exporter(helpers);
    const auto result = exporter.Run({cached, source, output}, {});
    if (!result.ok) std::wcerr << result.detail << '\n';
    CHECK(result.ok);
    if (result.ok) {
        const auto info = Probe(helpers, output, log, {L"-show_streams", L"-show_chapters", L"-show_format"});
        CHECK_EQ(size_t{1}, Count(info, "codec_type=video"));
        CHECK_EQ(size_t{2}, Count(info, "codec_type=audio"));
        CHECK_EQ(size_t{2}, Count(info, "codec_type=subtitle"));
        CHECK_EQ(size_t{1}, Count(info, "[CHAPTER]"));
        CHECK(info.find("TAG:title=Source title") != std::string::npos);
        CHECK(info.find("TAG:title=Opening") != std::string::npos);
        CHECK_EQ(size_t{2}, Count(info, "TAG:language=eng"));
        CHECK_EQ(size_t{2}, Count(info, "TAG:language=fra"));
        const std::vector<std::wstring> hashes{L"-select_streams", L"v:0", L"-show_packets",
            L"-show_entries", L"packet=data_hash", L"-show_data_hash", L"sha256", L"-of", L"csv=p=0"};
        const auto cachedHashes = Probe(helpers, cached, log, hashes);
        CHECK(!cachedHashes.empty());
        CHECK_EQ(cachedHashes, Probe(helpers, output, log, hashes));
        CHECK(cachedHashes != Probe(helpers, source, log, hashes));
        for (const auto* selector : {L"a", L"s"}) {
            auto streamHashes = hashes;
            streamHashes[1] = selector;
            const auto original = Probe(helpers, source, log, streamHashes);
            CHECK(!original.empty());
            CHECK_EQ(original, Probe(helpers, output, log, streamHashes));
        }
    }

    // A source without audio/subtitles/chapters must still export successfully.
    const auto silentOutput = fixture.path / L"silent.MKV";
    CHECK(exporter.Run({cached, cached, silentOutput}, {}).ok);
    if (std::filesystem::exists(silentOutput)) {
        const auto info = Probe(helpers, silentOutput, log, {L"-show_streams", L"-show_chapters"});
        CHECK_EQ(size_t{1}, Count(info, "codec_type=video"));
        CHECK_EQ(size_t{0}, Count(info, "codec_type=audio"));
        CHECK_EQ(size_t{0}, Count(info, "codec_type=subtitle"));
    }

    // Inputs are relative to the caller, even though FFmpeg runs in its own folder.
    const auto originalDirectory = std::filesystem::current_path();
    std::filesystem::current_path(fixture.path);
    const auto relativeResult = exporter.Run({cached.filename(), source.filename(), L"relative.mkv"}, {});
    std::filesystem::current_path(originalDirectory);
    CHECK(relativeResult.ok);
    CHECK(std::filesystem::is_regular_file(fixture.path / L"relative.mkv"));

    // Existing files, including either input and a hard-link alias, are never replaced.
    const auto existing = fixture.path / L"existing.mkv";
    Write(existing, "keep this file");
    const auto alias = fixture.path / L"source-alias.mkv";
    std::filesystem::create_hard_link(source, alias);
    const auto filesBefore = Files(fixture.path);
    for (const auto& destination : {existing, cached, source, alias, fixture.path / L"wrong.mp4"}) {
        const auto rejected = exporter.Run({cached, source, destination}, {});
        CHECK(!rejected.ok);
        CHECK_EQ(MaterializeError::InvalidRequest, rejected.error);
    }
    CHECK_EQ(std::string("keep this file"), Read(existing));
    CHECK_EQ(filesBefore, Files(fixture.path));

    // Invalid media can produce a partial mux; it must never become the final file.
    const auto invalid = fixture.path / L"invalid.mkv";
    Write(invalid, "not media");
    const auto filesWithInvalid = Files(fixture.path);
    const auto failed = exporter.Run({cached, invalid, fixture.path / L"failed.mkv"}, {});
    CHECK(!failed.ok);
    CHECK_EQ(MaterializeError::ProcessFailed, failed.error);
    CHECK(!failed.detail.empty());
    CHECK_EQ(filesWithInvalid, Files(fixture.path));

    // MP4 timed-text subtitles cannot be stream-copied into MKV. Fail visibly
    // rather than dropping that track or silently converting it.
    const auto timedTextSource = fixture.path / L"timed-text.mp4";
    CHECK(RunTool(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-i", source.wstring(),
        L"-map", L"0:v:0", L"-map", L"0:s:0", L"-c:v", L"libx264", L"-c:s", L"mov_text",
        timedTextSource.wstring()}, log));
    const auto filesWithTimedText = Files(fixture.path);
    const auto unsupported = exporter.Run({cached, timedTextSource, fixture.path / L"unsupported.mkv"}, {});
    CHECK(!unsupported.ok);
    CHECK_EQ(MaterializeError::ProcessFailed, unsupported.error);
    CHECK(!unsupported.detail.empty());
    CHECK_EQ(filesWithTimedText, Files(fixture.path));

    CHECK_EQ(MaterializeError::HelperMissing, CachedVideoExporter(fixture.path).Run(
        {cached, source, fixture.path / L"missing-helper.mkv"}, {}).error);
    CHECK_EQ(filesWithTimedText, Files(fixture.path));
    std::stop_source stopped;
    stopped.request_stop();
    CHECK_EQ(MaterializeError::Cancelled,
        exporter.Run({cached, source, fixture.path / L"cancelled.mkv"}, stopped.get_token()).error);
    CHECK_EQ(filesWithTimedText, Files(fixture.path));

    // Cancel after staging begins. No final output or staging file may survive.
    std::stop_source activeStop;
    auto active = std::async(std::launch::async, [&] {
        return exporter.Run({cached, source, fixture.path / L"active-cancel.mkv"}, activeStop.get_token());
    });
    bool sawStaging = false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline && active.wait_for(0ms) != std::future_status::ready) {
        if (Files(fixture.path) != filesWithTimedText) {
            sawStaging = true;
            activeStop.request_stop();
            break;
        }
        std::this_thread::yield();
    }
    CHECK(sawStaging);
    CHECK_EQ(MaterializeError::Cancelled, active.get().error);
    CHECK_EQ(filesWithTimedText, Files(fixture.path));

    // A file appearing after the initial destination check must not be replaced
    // at publication, even though the FFmpeg remux itself completed successfully.
    const auto contested = fixture.path / L"contested.mkv";
    auto racingExport = std::async(std::launch::async, [&] {
        return exporter.Run({cached, source, contested}, {});
    });
    bool createdDuringExport = false;
    const auto raceDeadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < raceDeadline &&
           racingExport.wait_for(0ms) != std::future_status::ready) {
        if (Files(fixture.path) != filesWithTimedText) {
            Write(contested, "another writer owns this destination");
            createdDuringExport = true;
            break;
        }
        std::this_thread::yield();
    }
    CHECK(createdDuringExport);
    CHECK_EQ(MaterializeError::ProcessFailed, racingExport.get().error);
    CHECK_EQ(std::string("another writer owns this destination"), Read(contested));
    auto expectedFiles = filesWithTimedText;
    expectedFiles.insert(contested);
    CHECK_EQ(expectedFiles, Files(fixture.path));
    CHECK_EQ(sourceBefore, Read(source));
    CHECK_EQ(cachedBefore, Read(cached));
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const auto helpers = std::filesystem::absolute(argc > 1 ? std::filesystem::path(argv[1]) : ExecutableDirectory());
    if (!std::filesystem::is_regular_file(helpers / L"ffmpeg.exe") ||
        !std::filesystem::is_regular_file(helpers / L"ffprobe.exe")) {
        std::cerr << "FFmpeg and FFprobe are required; pass their directory as the first argument.\n";
        return 1;
    }
    MaterializationPreservesFullVideoTest(helpers);
    MaterializationRejectsShortVideoWithLongAudioTest(helpers);
    ExportTests(helpers);
    if (test_support::failure_count != 0) return 1;
    std::cout << "Cached export real-media tests passed.\n";
    return 0;
}
