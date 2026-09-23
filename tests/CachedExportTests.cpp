#include "MediaPipeline.h"
#include "NeuralSegmentIndex.h"
#include "RuntimePolicy.h"
#include "SynchronizedPlayback.h"
#include "VideoDecoder.h"
#include "TestSupport.h"
#include "TestEnvironment.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
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

size_t IndexOf(const std::vector<std::wstring>& arguments, std::wstring_view value, size_t from = 0)
{
    for (size_t i = from; i < arguments.size(); ++i)
        if (arguments[i] == value) return i;
    return arguments.size();
}

void ExportArgumentTests()
{
    // The seek belongs to the source input: it must follow the neural video's
    // -i and precede the source's, so the rendered range is never seeked. The
    // zero start and duration are output options placed after every input.
    const std::filesystem::path neural = L"C:/cache/neural.mkv", source = L"C:/media/source.mkv", staging = L"C:/out/.stage.tmp";
    for (const auto* extension : {L"clip.mkv", L"clip.mp4"}) {
        const auto whole = BuildCachedExportArguments({neural, source, extension}, staging, false);
        CHECK_EQ(whole.size(), IndexOf(whole, L"-ss"));
        CHECK_EQ(whole.size(), IndexOf(whole, L"-t"));
        CHECK_EQ(staging.wstring(), whole.back());

        const auto ranged = BuildCachedExportArguments({neural, source, extension, 12.5, 3.25}, staging, false);
        const size_t neuralInput = IndexOf(ranged, L"-i");
        const size_t seek = IndexOf(ranged, L"-ss");
        const size_t sourceInput = IndexOf(ranged, L"-i", neuralInput + 1);
        const size_t outputStart = IndexOf(ranged, L"-ss", seek + 1);
        const size_t duration = IndexOf(ranged, L"-t");
        CHECK(neuralInput < ranged.size() && sourceInput < ranged.size());
        CHECK(seek < ranged.size() && outputStart < ranged.size() && duration < ranged.size());
        CHECK_EQ(neural.wstring(), ranged[neuralInput + 1]);
        CHECK_EQ(source.wstring(), ranged[sourceInput + 1]);
        CHECK(neuralInput < seek);
        CHECK(seek < sourceInput);
        CHECK(sourceInput < outputStart);
        CHECK(outputStart < duration);
        CHECK_EQ(std::wstring(L"12.5"), ranged[seek + 1]);
        CHECK_EQ(std::wstring(L"0"), ranged[outputStart + 1]);
        CHECK_EQ(std::wstring(L"3.25"), ranged[duration + 1]);
        CHECK_EQ(ranged.size(), IndexOf(ranged, L"-i", sourceInput + 1));
        CHECK_EQ(ranged.size(), IndexOf(ranged, L"-ss", outputStart + 1));
        CHECK_EQ(ranged.size(), IndexOf(ranged, L"-t", duration + 1));
        // Removing the trim yields exactly the untrimmed command.
        auto stripped = ranged;
        stripped.erase(stripped.begin() + static_cast<std::ptrdiff_t>(outputStart), stripped.begin() + static_cast<std::ptrdiff_t>(outputStart) + 4);
        stripped.erase(stripped.begin() + static_cast<std::ptrdiff_t>(seek), stripped.begin() + static_cast<std::ptrdiff_t>(seek) + 2);
        CHECK(stripped == whole);

        // A start without a duration runs to the source end.
        const auto openEnded = BuildCachedExportArguments({neural, source, extension, 2.0, 0.0}, staging, false);
        CHECK(IndexOf(openEnded, L"-ss") < IndexOf(openEnded, L"-i", IndexOf(openEnded, L"-i") + 1));
        CHECK_EQ(openEnded.size(), IndexOf(openEnded, L"-t"));
        // A duration from the very start needs no seek.
        const auto head = BuildCachedExportArguments({neural, source, extension, 0.0, 1.5}, staging, false);
        CHECK_EQ(head.size(), IndexOf(head, L"-ss"));
        CHECK(IndexOf(head, L"-i", IndexOf(head, L"-i") + 1) < IndexOf(head, L"-t"));
        CHECK_EQ(std::wstring(L"1.5"), head[IndexOf(head, L"-t") + 1]);
    }
    // GIF and still exports read the neural video only; nothing to trim.
    for (const auto* extension : {L"clip.gif", L"clip.png", L"clip.jpg"}) {
        const auto ranged = BuildCachedExportArguments({neural, source, extension, 12.5, 3.25}, staging, false);
        CHECK_EQ(ranged.size(), IndexOf(ranged, L"-ss"));
        CHECK_EQ(ranged.size(), IndexOf(ranged, L"-t"));
        CHECK_EQ(std::ptrdiff_t{1}, std::count(ranged.begin(), ranged.end(), L"-i"));
        CHECK_EQ(ranged.size(), IndexOf(ranged, source.wstring()));
    }
}

void RangeExportTests(const std::filesystem::path& helpers)
{
    FixtureDirectory fixture;
    const auto source = fixture.path / L"source.mkv";
    const auto cached = fixture.path / L"cached.mkv";
    const auto log = fixture.path / L"tool.log";
    const auto ffmpeg = helpers / L"ffmpeg.exe";
    // A 4 s source whose audio is a tone only inside [1 s, 3 s); the neural
    // render covers exactly that range. Any pre-roll or shift shows up as
    // silence at an edge of the exported audio or a non-zero video start.
    CHECK(RunTool(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n",
        L"-f", L"lavfi", L"-i", L"color=red:s=64x48:r=5:d=4",
        L"-f", L"lavfi", L"-i", L"aevalsrc=0.5*sin(440*2*PI*t)*gte(t\\,1)*lt(t\\,3):s=44100:d=4",
        L"-map", L"0:v", L"-map", L"1:a", L"-c:v", L"ffv1", L"-c:a", L"pcm_s16le", source.wstring()}, log));
    CHECK(RunTool(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi",
        L"-i", L"color=blue:s=64x48:r=5:d=2", L"-c:v", L"ffv1", cached.wstring()}, log));
    if (!std::filesystem::exists(source) || !std::filesystem::exists(cached)) return;
    CachedVideoExporter exporter(helpers);
    const auto peak = [](std::string_view pcm) {
        int maximum = 0;
        for (size_t i = 0; i + 1 < pcm.size(); i += 2)
            maximum = std::max(maximum, std::abs(static_cast<int16_t>(static_cast<uint8_t>(pcm[i]) | (static_cast<uint8_t>(pcm[i + 1]) << 8))));
        return maximum;
    };
    for (const auto* name : {L"range.mkv", L"range.mp4"}) {
        const auto output = fixture.path / name;
        const auto result = exporter.Run({cached, source, output, 1.0, 2.0}, {});
        if (!result.ok) std::wcerr << result.detail << '\n';
        CHECK(result.ok);
        if (!result.ok) continue;
        const auto video = Probe(helpers, output, log, {L"-count_frames", L"-select_streams", L"v:0",
            L"-show_entries", L"stream=start_time,nb_read_frames", L"-of", L"default=noprint_wrappers=1"});
        CHECK(video.find("nb_read_frames=10") != std::string::npos);
        CHECK(video.find("start_time=0.000000") != std::string::npos);
        const auto pcm = fixture.path / L"exported.pcm";
        CHECK(RunTool(ffmpeg, {L"-v", L"error", L"-y", L"-i", output.wstring(), L"-map", L"0:a:0",
            L"-ac", L"1", L"-ar", L"44100", L"-f", L"s16le", pcm.wstring()}, log));
        const auto samples = Read(pcm);
        std::filesystem::remove(pcm);
        constexpr size_t bytesPerSecond = 44100 * 2, window = bytesPerSecond / 10;
        CHECK(samples.size() > bytesPerSecond * 19 / 10 && samples.size() < bytesPerSecond * 21 / 10);
        if (samples.size() < 2 * window) continue;
        CHECK(peak(std::string_view(samples).substr(0, window)) > 8000);
        CHECK(peak(std::string_view(samples).substr(samples.size() - window)) > 8000);
    }
    const auto files = Files(fixture.path);
    for (const auto [start, duration] : {std::pair{-1.0, 2.0}, std::pair{1.0, -2.0},
             std::pair{std::nan(""), 2.0}, std::pair{1.0, std::numeric_limits<double>::infinity()}}) {
        const auto rejected = exporter.Run({cached, source, fixture.path / L"rejected.mkv", start, duration}, {});
        CHECK(!rejected.ok);
        CHECK_EQ(MaterializeError::InvalidRequest, rejected.error);
    }
    CHECK_EQ(files, Files(fixture.path));
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

    // MP4 uses compatible encodings while preserving selected tracks and chapters.
    const auto mp4Output = fixture.path / L"with-streams.mp4";
    CHECK(exporter.Run({cached, source, mp4Output}, {}).ok);
    CHECK(std::filesystem::is_regular_file(mp4Output));
    {
        const auto info = Probe(helpers, mp4Output, log, {L"-show_streams", L"-show_chapters"});
        CHECK_EQ(size_t{1}, Count(info, "codec_name=h264"));
        CHECK_EQ(size_t{2}, Count(info, "codec_name=aac"));
        CHECK_EQ(size_t{2}, Count(info, "codec_name=mov_text"));
        CHECK_EQ(size_t{1}, Count(info, "[CHAPTER]"));
        // MP4 can also carry a language tag on its chapter data track.
        for (const auto* selector : {L"a", L"s"}) {
            const auto languages = Probe(helpers, mp4Output, log, {L"-select_streams", selector, L"-show_streams"});
            CHECK_EQ(size_t{1}, Count(languages, "TAG:language=eng"));
            CHECK_EQ(size_t{1}, Count(languages, "TAG:language=fra"));
        }
    }

    // A source without audio/subtitles/chapters must still export successfully.
    const auto silentOutput = fixture.path / L"silent.MKV";
    CHECK(exporter.Run({cached, cached, silentOutput}, {}).ok);
    CHECK(std::filesystem::is_regular_file(silentOutput));
    {
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
    for (const auto& destination : {existing, cached, source, alias, fixture.path / L"wrong.txt"}) {
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

void PhotoAndAnimationTests(const std::filesystem::path& helpers)
{
    // Catch image metadata rejecting neural jobs, accidental image looping,
    // flattened GIFs, and exports that silently use original instead of processed pixels.
    FixtureDirectory fixture;
    const auto log = fixture.path / L"media.log";
    const auto ffmpeg = helpers / L"ffmpeg.exe";
    CachedVideoExporter exporter(helpers);
    for (const auto* extension : {L".png", L".jpg", L".bmp", L".tiff", L".webp"}) {
        const auto photo = fixture.path / (std::wstring(L"photo") + extension);
        CHECK(RunTool(ffmpeg, {L"-v", L"error", L"-f", L"lavfi", L"-i",
            L"color=blue:s=96x64", L"-frames:v", L"1", photo.wstring()}, log));
        VideoDecoder decoder;
        CHECK(decoder.OpenSequential(photo.wstring()));
        CHECK_EQ(decoder.Width(), 96u);
        CHECK_EQ(decoder.Height(), 64u);
        CHECK_EQ(decoder.FrameRate(), 1.0);
        CHECK_EQ(decoder.DurationSeconds(), 1.0);
        VideoFrame frame;
        CHECK(decoder.ReadNext(frame));
        // A sequential open takes NV12 only for a source that DECLARES a colour
        // description the GPU conversion implements (see MediaSource.h), so the
        // layout is a per-format answer here rather than a constant: measured on
        // this ffmpeg, `color=blue` gives png `pc/gbr` and bmp/tiff
        // `unknown/unknown` (all three refused, BGRA), while jpg is `pc/bt470bg`
        // and webp `tv/bt470bg` (BT.601 full and limited, both converted on the
        // GPU as NV12). Asserting the layout follows the gate is what survives a
        // codec changing its mind; asserting a fixed layout did not.
        //
        // What the gate is worth, measured: a JPEG is BT.601 full range by
        // convention (`pc/bt470bg` above), and the pre-probe shader was
        // hard-coded to BT.709 limited. Decoding this file's NV12 with those
        // coefficients lands mean 5.89 / max 33.0 eight-bit levels away from its
        // true RGB, against 0.40 / max 2.0 for the BT.601-full program the probe
        // now selects. Reachable only with `GpuSourceConversion=1`, since the one
        // production caller of OpenSequential passes that flag as its NV12
        // request (`src/OfflineNeuralRenderer.cpp:1661`) - which is precisely the
        // colour hazard that kept the flag off by default.
        const bool convertible =
            SourceNv12ConversionFor(decoder.ColorDescription()) != SourceNv12Conversion::Unsupported;
        CHECK((decoder.PixelLayout() == PixelLayout::Nv12) == convertible);
        CHECK_EQ(frame.bgra.size(), FrameBytes(decoder.PixelLayout(), 96, 64));
        CHECK(!decoder.ReadNext(frame));
        CHECK(decoder.SeekSeconds(0));
        CHECK(decoder.ReadNext(frame));
    }
    // Smartphone JPEG orientation lives on decoded-frame EXIF side data.
    // Encoded96x64 + orientation6 must be exposed as upright64x96 BGRA rows.
    const auto rotatedPhoto = fixture.path / L"rotated.jpg";
    auto jpegBytes = Read(fixture.path / L"photo.jpg");
    const unsigned char orientation[]{0xff,0xe1,0,34,'E','x','i','f',0,0,
        'I','I',42,0,8,0,0,0,1,0,0x12,1,3,0,1,0,0,0,6,0,0,0,0,0,0,0};
    jpegBytes.insert(2, reinterpret_cast<const char*>(orientation), sizeof(orientation));
    Write(rotatedPhoto, jpegBytes);
    VideoDecoder rotatedDecoder;
    CHECK(rotatedDecoder.OpenSequential(rotatedPhoto.wstring()));
    CHECK_EQ(rotatedDecoder.Width(), 64u);
    CHECK_EQ(rotatedDecoder.Height(), 96u);
    CHECK_EQ(rotatedDecoder.NativeWidth(), 64u);
    CHECK(std::abs(rotatedDecoder.DisplayAspectRatio() - 2.0 / 3.0) < 0.001);
    VideoFrame rotatedFrame;
    CHECK(rotatedDecoder.ReadNext(rotatedFrame));
    CHECK_EQ(rotatedFrame.bgra.size(), FrameBytes(rotatedDecoder.PixelLayout(), 64, 96));

    // Photos may have odd dimensions; software fallback must preserve them.
    const auto oddPhoto = fixture.path / L"odd.png";
    CHECK(RunTool(ffmpeg, {L"-v", L"error", L"-f", L"lavfi", L"-i",
        L"testsrc=s=95x65", L"-frames:v", L"1", oddPhoto.wstring()}, log));
    VideoDecoder photoDecoder;
    CHECK(photoDecoder.OpenSequential(oddPhoto.wstring()));
    VideoFrame photoFrame;
    CHECK(photoDecoder.ReadNext(photoFrame));
    const auto photoCache = fixture.path / L"photo-cache.mkv";
    RawVideoEncoder photoEncoder(helpers);
    CHECK_EQ(photoEncoder.Start({95, 65, 1.0, EncoderKind::H264Software}, photoCache), EncodeError::None);
    CHECK_EQ(photoEncoder.WriteFrame(photoFrame.bgra), EncodeError::None);
    CHECK_EQ(photoEncoder.Finish(), EncodeError::None);
    const auto photoOutput = fixture.path / L"photo-export.png";
    CHECK(exporter.Run({photoCache, oddPhoto, photoOutput}, {}).ok);
    CHECK(std::filesystem::is_regular_file(photoOutput));
    {
        const auto info = Probe(helpers, photoOutput, log, {L"-count_frames", L"-show_streams"});
        CHECK(info.find("width=95") != std::string::npos);
        CHECK(info.find("height=65") != std::string::npos);
        CHECK(info.find("nb_read_frames=1") != std::string::npos);
    }
    const auto photoMp4 = fixture.path / L"photo-export.mp4";
    CHECK(exporter.Run({photoCache, oddPhoto, photoMp4}, {}).ok);
    CHECK(std::filesystem::is_regular_file(photoMp4));
    {
        const auto info = Probe(helpers, photoMp4, log, {L"-count_frames", L"-show_streams"});
        CHECK(info.find("width=95") != std::string::npos);
        CHECK(info.find("height=65") != std::string::npos);
        CHECK(info.find("nb_read_frames=1") != std::string::npos);
    }
    const auto animation = fixture.path / L"animation.gif";
    CHECK(RunTool(ffmpeg, {L"-v", L"error", L"-f", L"lavfi", L"-i",
        L"testsrc2=s=96x64:r=10:d=1", L"-vf", L"select='eq(n,0)+eq(n,1)+eq(n,4)'",
        L"-fps_mode", L"vfr", L"-final_delay", L"60", animation.wstring()}, log));
    VideoDecoder decoder;
    CHECK(decoder.OpenSequential(animation.wstring()));
    CHECK(std::abs(decoder.DurationSeconds() - 1.0) < 0.011);
    const auto processed = fixture.path / L"processed.mkv";
    RawVideoEncoder encoder(helpers);
    const bool nv12 = decoder.PixelLayout() == PixelLayout::Nv12;
    CHECK_EQ(encoder.Start({96, 64, decoder.FrameRate(), EncoderKind::H264Software,
                            nv12 ? EncoderPixelFormat::Nv12 : EncoderPixelFormat::Bgra}, processed),
             EncodeError::None);
    size_t frames = 0;
    VideoFrame frame;
    while (decoder.ReadNext(frame) && frames < 200) {
        // A visibly different processed result (pure blue), encoded through the real cache
        // encoder. In NV12 that is BT.709 limited-range Y=32, U=240, V=118.
        if (nv12) {
            const size_t luma = size_t{96} * 64;
            std::fill(frame.bgra.begin(), frame.bgra.begin() + luma, uint8_t{32});
            for (size_t i = luma; i + 1 < frame.bgra.size(); i += 2) { frame.bgra[i] = 240; frame.bgra[i + 1] = 118; }
        } else {
            for (size_t i = 0; i < frame.bgra.size(); i += 4) {
                frame.bgra[i] = 255; frame.bgra[i + 1] = 0; frame.bgra[i + 2] = 0;
            }
        }
        CHECK_EQ(encoder.WriteFrame(frame.bgra), EncodeError::None);
        ++frames;
    }
    CHECK_EQ(frames, size_t{100});
    CHECK_EQ(encoder.Finish(), EncodeError::None);
    for (const auto* extension : {L".png", L".jpg", L".gif", L".mp4", L".mkv"}) {
        const auto output = fixture.path / (std::wstring(L"export") + extension);
        const auto result = exporter.Run({processed, animation, output}, {});
        if (!result.ok) std::wcerr << result.detail << '\n';
        CHECK(result.ok);
        if (!result.ok) continue;
        const auto info = Probe(helpers, output, log, {L"-show_streams", L"-show_format"});
        CHECK(info.find("width=96") != std::string::npos);
        CHECK(info.find("height=64") != std::string::npos);
        const auto pixels = fixture.path / L"pixels.bgra";
        CHECK(RunTool(ffmpeg, {L"-v", L"error", L"-y", L"-i", output.wstring(),
            L"-frames:v", L"1", L"-pix_fmt", L"bgra", L"-f", L"rawvideo", pixels.wstring()}, log));
        const auto bytes = Read(pixels);
        CHECK_EQ(bytes.size(), size_t{96 * 64 * 4});
        if (bytes.size() >= 4) { CHECK(static_cast<unsigned char>(bytes[0]) > 220); CHECK(static_cast<unsigned char>(bytes[2]) < 30); }
        if (std::wstring_view(extension) == L".gif" || std::wstring_view(extension) == L".mp4")
            CHECK(info.find("duration=1.000000") != std::string::npos);
    }
    // A GIF exported directly from an animation must retain distinct visual frames.
    const auto animatedOutput = fixture.path / L"animated-export.gif";
    CHECK(exporter.Run({animation, animation, animatedOutput}, {}).ok);
    CHECK(std::filesystem::is_regular_file(animatedOutput));
    {
        const auto info = Probe(helpers, animatedOutput, log, {L"-count_frames", L"-show_streams", L"-show_format"});
        CHECK(info.find("nb_read_frames=50") != std::string::npos);
        CHECK(info.find("duration=1.000000") != std::string::npos);
        VideoDecoder exportedAnimation;
        CHECK(exportedAnimation.OpenSequential(animatedOutput.wstring()));
        std::set<std::vector<uint8_t>> distinctFrames;
        VideoFrame animatedFrame;
        while (exportedAnimation.ReadNext(animatedFrame) && distinctFrames.size() < 3)
            distinctFrames.insert(animatedFrame.bgra);
        CHECK_EQ(distinctFrames.size(), size_t{3});
    }
}

// The publish gate compares the joined render's frame count and video span
// against what the renderer reported. Both are read by demuxing rather than
// decoding, which is 400x cheaper and used to hold the next hole's render back
// by 18 s - so they have to agree with a decode on a stream where they could
// disagree. These parts carry B-frames, so coded and presentation order differ
// and the last packet's duration is derived rather than stored: Matroska has no
// per-packet duration, and a tail that came back unknown would leave the span
// one frame short of the file. The joined length is deliberately an odd number
// of frames, so an off-by-one cannot hide in a round total.
void JoinedFrameCountMatchesDecodedCountTest(const std::filesystem::path& helpers)
{
    FixtureDirectory fixture;
    const auto first = fixture.path / L"part-0.mkv";
    const auto second = fixture.path / L"part-1.mkv";
    const auto joined = fixture.path / L"joined.mkv";
    const auto shortJoin = fixture.path / L"short.mkv";
    const auto log = fixture.path / L"tool.log";
    CHECK(RunTool(helpers / L"ffmpeg.exe", {L"-v", L"error", L"-nostdin", L"-n",
        L"-f", L"lavfi", L"-i", L"testsrc=s=64x48:r=10:d=2", L"-c:v", L"libx264",
        L"-bf", L"2", L"-pix_fmt", L"yuv420p", first.wstring()}, log));
    CHECK(RunTool(helpers / L"ffmpeg.exe", {L"-v", L"error", L"-nostdin", L"-n",
        L"-f", L"lavfi", L"-i", L"testsrc=s=64x48:r=10:d=1.1", L"-c:v", L"libx264",
        L"-bf", L"2", L"-pix_fmt", L"yuv420p", second.wstring()}, log));

    const std::vector<std::filesystem::path> parts{first, second};
    CHECK(ConcatenateMedia(helpers, parts, joined, {}) == EncodeError::None);

    // Ground truth: every frame actually decoded out of the joined file.
    const auto decoded = Probe(helpers, joined, log, {L"-count_frames", L"-select_streams", L"v:0",
        L"-show_entries", L"stream=nb_read_frames", L"-of", L"default=noprint_wrappers=1"});
    CHECK(decoded.find("nb_read_frames=31") != std::string::npos);

    const auto measured = ProbeMedia(helpers, joined, {});
    CHECK(measured.ok);
    CHECK_EQ(uint64_t{31}, measured.frameCount);
    CHECK_EQ(uint32_t{64}, measured.width);
    CHECK_EQ(uint32_t{48}, measured.height);
    // 31 frames at 10 fps, so the last frame's derived duration is inside this:
    // drop it and the span reads 3.0 s. This is the field the publish gate's
    // duration comparison comes from.
    CHECK_EQ(int64_t{31000000}, measured.videoDuration100ns);
    CHECK(measured.decodedFinalFrame);

    // What the gate is for: a join that lost a part has to read short, or a
    // truncated render would be published as a complete cache entry.
    const std::vector<std::filesystem::path> onePart{first};
    CHECK(ConcatenateMedia(helpers, onePart, shortJoin, {}) == EncodeError::None);
    const auto shortMeasured = ProbeMedia(helpers, shortJoin, {});
    CHECK(shortMeasured.ok);
    CHECK_EQ(uint64_t{20}, shortMeasured.frameCount);
    CHECK_EQ(int64_t{20000000}, shortMeasured.videoDuration100ns);

    // The two probes through the gate itself, composed the way the player's
    // publish composes it (src/main.cpp, the promote tail of the job lambda):
    // the renderer reported 31 frames over 3.1 s for a two-part join at
    // 10 fps. The full join is publishable; the join that lost its second
    // part is refused on its frame count and on its span alike.
    const uint64_t renderedFrames = 31;
    const int64_t renderedDuration100ns = 31000000, expectedDuration100ns = 31000000;
    const int64_t tolerance100ns = JoinedMediaDurationTolerance100ns(10.0, parts.size());
    const auto probeMatches = [&](const ProbeResult& probe) {
        return probe.ok && probe.width == 64u && probe.height == 48u && probe.frameCount == renderedFrames &&
               NeuralPublishDurationsMatch(probe.duration100ns, renderedDuration100ns, expectedDuration100ns,
                                           tolerance100ns);
    };
    CHECK(probeMatches(measured));
    CHECK(CanPublishNeuralCompletion(true, probeMatches(measured), true));
    CHECK(!probeMatches(shortMeasured));
    CHECK(!NeuralPublishDurationsMatch(shortMeasured.duration100ns, renderedDuration100ns, expectedDuration100ns,
                                       tolerance100ns));
    CHECK(!CanPublishNeuralCompletion(true, probeMatches(shortMeasured), true));
}

// Cached playback builds its own decoders: nothing injects the two sources,
// so this is the one place the decoder-backed frame source pairs real files.
// A red original and a blue render, lossless, so a pair is checked by its
// pixels and a frame is identified by its number on the 10 fps grid.
void SynchronizedPlaybackPairsRealMediaTest(const std::filesystem::path& helpers)
{
    FixtureDirectory fixture;
    const auto original = fixture.path / L"original.mkv";
    const auto neural = fixture.path / L"neural.mkv";
    const auto rangeRender = fixture.path / L"range-render.mkv";
    const auto log = fixture.path / L"tool.log";
    const auto ffmpeg = helpers / L"ffmpeg.exe";
    CHECK(RunTool(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi",
        L"-i", L"color=red:s=64x48:r=10:d=3", L"-c:v", L"ffv1", original.wstring()}, log));
    CHECK(RunTool(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi",
        L"-i", L"color=blue:s=64x48:r=10:d=3", L"-c:v", L"ffv1", neural.wstring()}, log));
    CHECK(RunTool(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi",
        L"-i", L"color=blue:s=64x48:r=10:d=1", L"-c:v", L"ffv1", rangeRender.wstring()}, log));
    if (!std::filesystem::exists(original) || !std::filesystem::exists(neural) ||
        !std::filesystem::exists(rangeRender)) return;

    // The decoders restart FFmpeg asynchronously, so NotReady is a wait, not an answer.
    const auto next = [](SynchronizedPlayback& playback) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            const auto result = playback.ReadNextAvailable();
            if (result != SynchronizedReadResult::NotReady || std::chrono::steady_clock::now() >= deadline)
                return result;
            std::this_thread::sleep_for(5ms);
        }
    };
    const auto solid = [](const VideoFrame& frame, size_t channel) {
        if (frame.bgra.size() != FrameBytes(PixelLayout::Bgra, 64, 48)) return false;
        for (size_t i = 0; i < frame.bgra.size(); i += 4)
            if (frame.bgra[i + channel] < 220 || frame.bgra[i + (2 - channel)] > 30) return false;
        return true;
    };
    const auto red = [&](const VideoFrame& frame) { return solid(frame, 2); };
    const auto blue = [&](const VideoFrame& frame) { return solid(frame, 0); };

    SynchronizedPlayback playback;
    CHECK(playback.Open(original, neural));
    CHECK(playback.NeuralAvailable());
    size_t pairs = 0;
    SynchronizedReadResult result = SynchronizedReadResult::NotReady;
    while ((result = next(playback)) == SynchronizedReadResult::PairReady) {
        const SynchronizedFramePair* pair = playback.CurrentPair();
        CHECK(pair != nullptr);
        if (!pair) break;
        CHECK_EQ(uint64_t{pairs}, pair->frameNumber);
        CHECK_EQ(int64_t{1000000} * static_cast<int64_t>(pairs), pair->timestamp100ns);
        CHECK(red(pair->original));
        CHECK(blue(pair->neural));
        if (++pairs > 40) break;
    }
    CHECK_EQ(SynchronizedReadResult::EndOfStream, result);
    CHECK_EQ(size_t{30}, pairs);

    // A seek lands both members on the same frame, the view switch shows the
    // render's pixels for it, and playback continues from the frame after.
    CHECK(playback.SeekSeconds(1.5));
    const SynchronizedFramePair* seeked = playback.CurrentPair();
    CHECK(seeked != nullptr);
    if (seeked) {
        CHECK_EQ(uint64_t{15}, seeked->frameNumber);
        CHECK(red(seeked->original));
        CHECK(blue(seeked->neural));
    }
    CHECK(playback.SetView(ComparisonView::Neural));
    CHECK(playback.VisibleFrame() && blue(*playback.VisibleFrame()));
    CHECK(playback.SetView(ComparisonView::Original));
    CHECK(playback.VisibleFrame() && red(*playback.VisibleFrame()));
    CHECK_EQ(SynchronizedReadResult::PairReady, next(playback));
    CHECK(playback.CurrentPair() && playback.CurrentPair()->frameNumber == 16u);
    // Past the end lands on the last frame, and the stream ends after it.
    CHECK(playback.SeekSeconds(10.0));
    CHECK(playback.CurrentPair() && playback.CurrentPair()->frameNumber == 29u);
    CHECK_EQ(SynchronizedReadResult::EndOfStream, next(playback));

    // A render of [1 s, 2 s) pairs with the original's frames 10-19 and the
    // stream ends at the range, not at the original's end.
    SynchronizedPlayback ranged;
    CHECK(ranged.Open(original, rangeRender, {}, SynchronizedRange{10000000, 20000000}));
    pairs = 0;
    while ((result = next(ranged)) == SynchronizedReadResult::PairReady) {
        const SynchronizedFramePair* pair = ranged.CurrentPair();
        CHECK(pair != nullptr);
        if (!pair) break;
        CHECK_EQ(uint64_t{10 + pairs}, pair->frameNumber);
        CHECK(red(pair->original));
        CHECK(blue(pair->neural));
        if (++pairs > 40) break;
    }
    CHECK_EQ(SynchronizedReadResult::EndOfStream, result);
    CHECK_EQ(size_t{10}, pairs);

    // A render whose length disagrees with the span it claims is refused at Open.
    SynchronizedPlayback mismatched;
    CHECK(!mismatched.Open(original, rangeRender));
    CHECK(!mismatched.NeuralAvailable());
}


// A live run is served from its segments until it is published, then from the
// joined cache entry (NeuralSegmentIndex::ReplaceRun, P1.14). The entry is a
// stream copy of the same segments, so after the switch every frame has to be
// byte-for-byte the frame the segments decode to, at the same number and
// timestamp - including where playback crosses into the entry mid-way and
// where a seek lands inside it. Long-GOP H.264 with B-frames, one keyframe per
// segment like the renderer's, so entering the entry at a segment boundary and
// seeking inside it both have to find the right frame from a keyframe behind.
void LivePlaybackSwitchesOntoTheJoinedRunTest(const std::filesystem::path& helpers)
{
    FixtureDirectory fixture;
    const auto original = fixture.path / L"original.mkv";
    const auto joined = fixture.path / L"joined.mkv";
    const auto log = fixture.path / L"tool.log";
    const auto ffmpeg = helpers / L"ffmpeg.exe";
    CHECK(RunTool(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-f", L"lavfi",
        L"-i", L"testsrc=s=64x48:r=10:d=4", L"-c:v", L"ffv1", original.wstring()}, log));
    // A short first segment and longer ones behind it, as a session renders.
    struct Cut { uint64_t first, count; };
    const std::vector<Cut> cuts{{0, 5}, {5, 20}, {25, 15}};
    std::vector<std::filesystem::path> parts;
    auto segments = std::make_shared<NeuralSegmentIndex>();
    for (size_t index = 0; index < cuts.size(); ++index) {
        const auto part = fixture.path / (L"neural-0000" + std::to_wstring(index) + L".mkv");
        const std::wstring trim = L"trim=start_frame=" + std::to_wstring(cuts[index].first) + L":end_frame=" +
                                  std::to_wstring(cuts[index].first + cuts[index].count) + L",setpts=PTS-STARTPTS";
        CHECK(RunTool(ffmpeg, {L"-v", L"error", L"-nostdin", L"-n", L"-i", original.wstring(), L"-vf", trim,
            L"-c:v", L"libx264", L"-g", L"1000", L"-bf", L"2", L"-pix_fmt", L"yuv420p", part.wstring()}, log));
        parts.push_back(part);
        NeuralSegment record;
        record.path = part;record.runId = 1;record.index = index;
        record.firstFrameNumber = cuts[index].first;record.frameCount = cuts[index].count;
        record.firstTimestamp100ns = int64_t(cuts[index].first) * 1000000;
        record.end100ns = int64_t(cuts[index].first + cuts[index].count) * 1000000;
        segments->Append(record);
    }
    CHECK(ConcatenateMedia(helpers, parts, joined, {}) == EncodeError::None);
    if (!std::filesystem::exists(joined)) return;

    const auto next = [](SynchronizedPlayback& playback) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            const auto result = playback.ReadNextAvailable();
            if (result != SynchronizedReadResult::NotReady || std::chrono::steady_clock::now() >= deadline)
                return result;
            std::this_thread::sleep_for(5ms);
        }
    };
    // Every neural frame of one whole pass, by frame number.
    const auto play = [&](const std::shared_ptr<NeuralSegmentIndex>& index, bool publishAfterFirstFrames) {
        std::vector<std::vector<uint8_t>> frames;
        SynchronizedPlayback playback;
        CHECK(playback.OpenLive(original, index, SynchronizedRange{}));
        SynchronizedReadResult result = SynchronizedReadResult::NotReady;
        while ((result = next(playback)) == SynchronizedReadResult::PairReady) {
            const SynchronizedFramePair* pair = playback.CurrentPair();
            CHECK(pair != nullptr);
            if (!pair || frames.size() > 50) break;
            CHECK_EQ(uint64_t{frames.size()}, pair->frameNumber);
            CHECK_EQ(pair->original.frameNumber, pair->neural.frameNumber);
            CHECK_EQ(int64_t{1000000} * static_cast<int64_t>(frames.size()), pair->neural.timestamp100ns);
            frames.push_back(pair->neural.bgra);
            if (publishAfterFirstFrames && frames.size() == 2) CHECK(index->ReplaceRun(1, joined));
        }
        CHECK_EQ(SynchronizedReadResult::EndOfStream, result);
        CHECK(playback.LastFault().empty());
        if (publishAfterFirstFrames) {
            // Everything retired is free: playback is inside the entry now.
            for (const auto& part : parts) CHECK(!playback.HoldsFile(part));
            CHECK(playback.HoldsFile(joined));
            // A seek inside the entry lands on the frame the segment held.
            CHECK(playback.SeekSeconds(3.3));
            const SynchronizedFramePair* seeked = playback.CurrentPair();
            CHECK(seeked != nullptr);
            if (seeked && frames.size() > 33) {
                CHECK_EQ(uint64_t{33}, seeked->frameNumber);
                CHECK(seeked->neural.bgra == frames[33]);
            }
        }
        return frames;
    };
    const auto fromSegments = play(segments, false);
    auto republished = std::make_shared<NeuralSegmentIndex>();
    for (size_t index = 0; index < segments->Count(); ++index)
        if (const auto record = segments->At(index)) republished->Append(*record);
    const auto fromEntry = play(republished, true);
    CHECK_EQ(size_t{40}, fromSegments.size());
    CHECK_EQ(fromSegments.size(), fromEntry.size());
    size_t differing = 0;
    for (size_t frame = 0; frame < std::min(fromSegments.size(), fromEntry.size()); ++frame)
        if (fromSegments[frame] != fromEntry[frame]) ++differing;
    CHECK_EQ(size_t{0}, differing);
    CHECK_EQ(size_t{parts.size()}, republished->RetiredFiles().size());
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    test_support::ContainChildProcesses();
    const auto helpers = std::filesystem::absolute(argc > 1 ? std::filesystem::path(argv[1]) : ExecutableDirectory());
    if (!std::filesystem::is_regular_file(helpers / L"ffmpeg.exe") ||
        !std::filesystem::is_regular_file(helpers / L"ffprobe.exe")) {
        // Staging FFmpeg is optional at configure time, so a tree without it
        // skips this suite (ctest's SKIP_RETURN_CODE) rather than failing it.
        std::cerr << "FFmpeg and FFprobe are required; pass their directory as the first argument.\n";
        return 125;
    }
    // Decoders this suite constructs itself - the photo cases' and the ones
    // cached playback owns - look for FFmpeg beside this executable and then on
    // PATH, so the staged helpers go first on PATH for their sake.
    std::wstring path(32768, L'\0');
    path.resize(GetEnvironmentVariableW(L"PATH", path.data(), static_cast<DWORD>(path.size())));
    CHECK(SetEnvironmentVariableW(L"PATH", (helpers.wstring() + L';' + path).c_str()) != FALSE);
    MaterializationPreservesFullVideoTest(helpers);
    MaterializationRejectsShortVideoWithLongAudioTest(helpers);
    ExportArgumentTests();
    ExportTests(helpers);
    RangeExportTests(helpers);
    PhotoAndAnimationTests(helpers);
    JoinedFrameCountMatchesDecodedCountTest(helpers);
    SynchronizedPlaybackPairsRealMediaTest(helpers);
    LivePlaybackSwitchesOntoTheJoinedRunTest(helpers);
    if (test_support::failure_count != 0) return 1;
    std::cout << "Cached export real-media tests passed.\n";
    return 0;
}
