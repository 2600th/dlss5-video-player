#include "NeuralWorker.h"
#include "NeuralWorkerProtocol.h"
#include "ResidentWorkerLoop.h"
#include "TestSupport.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

using namespace neural_worker_protocol;

constexpr uint16_t kProtocolVersion = kVersion;
constexpr uint32_t kProtocolMagic = kMagic;

std::filesystem::path CurrentExecutable()
{
    std::wstring value(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    CHECK(length > 0 && length < value.size());
    value.resize(length);
    return value;
}

NeuralRenderRequest TestRequest(std::wstring_view sourceName)
{
    NeuralRenderRequest request;
    request.sourcePath = std::filesystem::path(sourceName);
    request.stagingVideoPath = L"staging-output.mkv";
    request.width = 1920;
    request.height = 1080;
    request.fps = 60.0;
    request.durationSeconds = 1.0;
    return request;
}

// 60 fps: the duration the helper derives from the request's frame rate.
constexpr int64_t kTestFrameDuration100ns = 166'667;

std::wstring SegmentFileName(uint64_t index)
{
    std::wstring digits = std::to_wstring(index);
    if (digits.size() < 5) digits.insert(0, 5 - digits.size(), L'0');
    return L"neural-" + digits + L".mkv";
}

NeuralRenderSegment TestSegment(uint64_t index, uint64_t frames)
{
    NeuralRenderSegment segment;
    segment.index = index;
    segment.firstFrameNumber = index * frames;
    segment.firstTimestamp100ns = static_cast<int64_t>(index * frames) * kTestFrameDuration100ns;
    segment.frameCount = frames;
    segment.end100ns = segment.firstTimestamp100ns + static_cast<int64_t>(frames) * kTestFrameDuration100ns;
    segment.fileName = SegmentFileName(index);
    return segment;
}

// A name one character past the cap, which EncodeSegment would truncate.
std::vector<std::byte> OversizedSegmentPayload(const NeuralRenderSegment& segment)
{
    const std::wstring name(kMaximumSegmentNameBytes / sizeof(wchar_t) + 1, L'n');
    WireSegment wire{};
    wire.index = segment.index;
    wire.firstFrameNumber = segment.firstFrameNumber;
    wire.firstTimestamp100ns = segment.firstTimestamp100ns;
    wire.frameCount = segment.frameCount;
    wire.frameDuration100ns = kTestFrameDuration100ns;
    wire.nameBytes = static_cast<uint32_t>(name.size() * sizeof(wchar_t));
    std::vector<std::byte> payload(sizeof(wire) + wire.nameBytes);
    std::memcpy(payload.data(), &wire, sizeof(wire));
    std::memcpy(payload.data() + sizeof(wire), name.data(), wire.nameBytes);
    return payload;
}

template <class T>
std::span<const std::byte> AsBytes(const T& value)
{
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(&value), sizeof(value));
}

void AppendMessage(std::vector<std::byte>& stream, uint16_t version, WireKind kind,
                   std::span<const std::byte> payload)
{
    const WireHeader header{kProtocolMagic, version, static_cast<uint16_t>(kind),
                            static_cast<uint32_t>(payload.size())};
    const auto* first = reinterpret_cast<const std::byte*>(&header);
    stream.insert(stream.end(), first, first + sizeof(header));
    stream.insert(stream.end(), payload.begin(), payload.end());
}

void AppendMessage(std::vector<std::byte>& stream, WireKind kind, std::span<const std::byte> payload)
{
    AppendMessage(stream, kProtocolVersion, kind, payload);
}

NeuralRenderResult ValidFakeResult(uint64_t jobId)
{
    NeuralRenderResult result;
    result.ok = true;
    result.encoder = EncoderKind::H264Software;
    result.feature18ArmedBeforeCapture = true;
    result.evidence = {true, true, true, true, false, 75};
    result.frameCount = 48;
    result.duration100ns = 8'000'000;
    result.nativeEvaluations = 48;
    result.verifiedNeuralFrames = 48;
    result.jobId = jobId;
    return result;
}

// The five phases only a helper can see, with the orders of magnitude the
// handoff's cold-start table records so a transposed field is obvious.
NeuralColdStartTimeline FakeHelperTimeline()
{
    NeuralColdStartTimeline timeline;
    timeline.Record(NeuralColdStartPhase::HelperStart, 712000us);
    timeline.Record(NeuralColdStartPhase::RuntimeReady, 94000us);
    timeline.Record(NeuralColdStartPhase::NeuralInit, 1511000us);
    timeline.Record(NeuralColdStartPhase::FeatureArm, 631000us);
    timeline.Record(NeuralColdStartPhase::FirstOutput, 1780000us);
    return timeline;
}

int RunFakeWorker(int argc, wchar_t** argv)
{
    std::vector<std::wstring_view> values;
    for (int index = 0; index < argc; ++index) values.emplace_back(argv[index]);
    const auto parsed = neural_worker_detail::ParseWorkerArguments(values);
    if (!parsed) return 9;
    const HANDLE handle = parsed->metadata;
    if (parsed->preflight) {
        // The parent picks the receipt shape: a refusal carrying a diagnosis,
        // a refusal without one, or the normal success.
        std::array<wchar_t, 16> mode{};
        const DWORD length = GetEnvironmentVariableW(L"DLSSVIDEOPLAYER_FAKE_PREFLIGHT", mode.data(),
                                                     static_cast<DWORD>(mode.size()));
        const std::wstring_view shape(mode.data(), length < mode.size() ? length : 0);
        PreflightPayload payload;
        if (shape == L"refused") {
            payload.json = "{\"schema\":2,\"ok\":false,\"feature18\":{\"carrierCreateResult\":\"0x00000001\","
                           "\"createResult\":\"0xbad00002\"},\"diagnosis\":{\"cause\":\"driverBelowFloor\","
                           "\"detail\":\"NVIDIA driver 566.14 is below the 610.47 minimum for neural rendering. "
                           "Update to 616.64 or newer, then try again.\"}}";
        } else if (shape == L"bare") {
            payload.json = "{\"schema\":2,\"ok\":false}";
        } else {
            payload.ok = !parsed->configurationRestarted;
            payload.json = parsed->configurationRestarted ? "{\"ok\":false,\"restarted\":true}" : "{\"ok\":true,\"gpu\":\"fake\"}";
        }
        const auto bytes = EncodePreflight(payload);
        return WriteMessage(handle, WireKind::Preflight, bytes.data(), static_cast<uint32_t>(bytes.size())) ? 0 : 15;
    }
    const std::wstring source = parsed->request.sourcePath.wstring();
    if (source == L"configure-once-source.mkv" && !parsed->configurationRestarted) return 75;
    if (source == L"configure-always-source.mkv") return 75;
    if (source == L"hang-source.mkv") {
        std::this_thread::sleep_for(20s);
        return 0;
    }
    if (source == L"truncated-source.mkv") {
        const WireHeader truncated{kProtocolMagic, kProtocolVersion,
            static_cast<uint16_t>(WireKind::Result), sizeof(WireResult)};
        return WriteAll(handle, &truncated, sizeof(truncated)) ? 0 : 12;
    }
    if (source == L"crash-source.mkv") {
        // Report progress, then die without a result: the parent must treat
        // this as a crash and relaunch from zero at most once.
        NeuralRenderProgress progress;
        progress.phase = NeuralRenderPhase::NeuralRendering;
        progress.completedFrames = 3;
        progress.totalFrames = 60;
        const WireProgress wire = EncodeProgress(progress);
        WriteMessage(handle, WireKind::Progress, &wire, sizeof(wire));
        return 0xC0000005u & 0xFF;
    }
    if (source == L"device-removed-source.mkv") {
        NeuralRenderResult failed;
        failed.failure = NeuralRenderFailure::DeviceRemoved;
        failed.jobId = parsed->request.jobId;
        failed.detail = L"fake device removal";
        const auto bytes = EncodeResult(failed);
        return WriteMessage(handle, WireKind::Result, bytes.data(), static_cast<uint32_t>(bytes.size())) ? 0 : 14;
    }
    if (source == L"wrong-job-source.mkv") {
        NeuralRenderResult failed;
        failed.failure = NeuralRenderFailure::Source;
        failed.jobId = parsed->request.jobId + 1;
        const auto bytes = EncodeResult(failed);
        return WriteMessage(handle, WireKind::Result, bytes.data(), static_cast<uint32_t>(bytes.size())) ? 0 : 14;
    }

    NeuralRenderProgress progress;
    progress.phase = NeuralRenderPhase::NeuralRendering;
    progress.completedFrames = 17;
    progress.totalFrames = 60;
    progress.bytes = 4096;
    progress.elapsed = 125ms;
    progress.estimatedRemaining = 200ms;
    const WireProgress wireProgress = EncodeProgress(progress);
    if (!WriteMessage(handle, WireKind::Progress, &wireProgress, sizeof(wireProgress))) return 13;
    if (source == L"segment-source.mkv") {
        // Two finalized files announced while the job is still running.
        for (uint64_t index = 0; index < 2; ++index) {
            const auto payload = EncodeSegment(TestSegment(index, 24), kTestFrameDuration100ns);
            if (!WriteMessage(handle, WireKind::Segment, payload.data(),
                              static_cast<uint32_t>(payload.size()))) return 16;
        }
    }
    if (source == L"timeline-source.mkv") {
        // Non-terminal and ahead of the result, the way the real helper sends it.
        const WireTimeline wire = EncodeTimeline(FakeHelperTimeline());
        if (!WriteMessage(handle, WireKind::Timeline, &wire, sizeof(wire))) return 17;
    }
    NeuralRenderResult result;
    result.ok = true;
    result.encoder = EncoderKind::H264Software;
    result.feature18ArmedBeforeCapture = true;
    result.evidence = {true, true, true, true, false, 75};
    result.frameCount = 60;
    result.duration100ns = 10'000'000;
    result.nativeEvaluations = 60;
    result.verifiedNeuralFrames = 60;
    result.jobId = parsed->request.jobId;
    result.historyResets = 2;
    result.frameRetries = 1;
    result.sceneCuts = {.acceptedStrong = 3, .acceptedWeak = 5, .suppressed = 7};
    result.firstTimestamp100ns = parsed->request.range.start100ns;
    result.timing = {60, 1.5, 2.5, 4.0, 0.7, 0.3, 2048};
    result.detail = L"validated fake helper result";
    if (source == L"echo-request-source.mkv") {
        // Encode the parsed request in the detail so the round trip is provable.
        const std::string guides = CanonicalGuideControls(parsed->request.guides);
        result.detail = L"guides=" + std::wstring(guides.begin(), guides.end()) +
                        L" preroll=" + std::to_wstring(parsed->request.prerollFrames) +
                        L" retry=" + std::to_wstring(parsed->request.frameRetryLimit) +
                        L" segments=" + std::to_wstring(parsed->request.segmentFrames) +
                        L" range=" + std::to_wstring(parsed->request.range.start100ns) + L"-" +
                        std::to_wstring(parsed->request.range.end100ns) +
                        L" pause=" + (parsed->request.pauseEvent ? L"1" : L"0");
    }
    const auto bytes = EncodeResult(result);
    if (!WriteMessage(handle, WireKind::Result, bytes.data(), static_cast<uint32_t>(bytes.size()))) return 14;
    return 0;
}

bool ParseUnsigned32(std::wstring_view text, uint32_t& value)
{
    if (text.empty()) return false;
    uint64_t parsed = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') return false;
        parsed = parsed * 10 + static_cast<uint64_t>(character - L'0');
        if (parsed > UINT32_MAX) return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool ParsePositiveDouble(std::wstring_view text, double& value)
{
    if (text.empty() || text.size() >= 128) return false;
    std::wstring copy(text);
    wchar_t* end = nullptr;
    value = std::wcstod(copy.c_str(), &end);
    return end == copy.c_str() + copy.size() && std::isfinite(value) && value > 0.0;
}

int RunRealPreflight(int argc, wchar_t** argv)
{
    if (argc != 3) {
        std::wcerr << L"Usage: NeuralWorkerTests --real-preflight <workerexe>\n";
        return EXIT_FAILURE;
    }
    const NeuralPreflightResult result = RunNeuralPreflight(argv[2]);
    std::wcout << L"ok=" << result.ok << L" cancelled=" << result.cancelled << L" detail=" << result.detail << L'\n';
    std::cout << result.json << '\n';
    return result.ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

int RunRealWorker(int argc, wchar_t** argv)
{
    if (argc != 9 && argc != 12 && argc != 13) {
        std::wcerr << L"Usage: NeuralWorkerTests --real-worker <workerexe> <sourcevideo> <outputvideo> <width> <height> <fps> <seconds> [rangeStartSec rangeEndSec mv=1,depth=1 [segmentFrames]]\n";
        return EXIT_FAILURE;
    }
    NeuralRenderRequest request;
    request.sourcePath = argv[3];
    request.stagingVideoPath = argv[4];
    request.jobId = static_cast<uint64_t>(GetTickCount64());
    if (!ParseUnsigned32(argv[5], request.width) || !ParseUnsigned32(argv[6], request.height) ||
        !request.width || !request.height || !ParsePositiveDouble(argv[7], request.fps) ||
        !ParsePositiveDouble(argv[8], request.durationSeconds)) {
        std::wcerr << L"Invalid --real-worker dimensions, FPS, or duration.\n";
        return EXIT_FAILURE;
    }
    if (argc >= 12) {
        double start = 0.0, end = 0.0;
        std::wstring copy(argv[9]);
        wchar_t* stop = nullptr;
        start = std::wcstod(copy.c_str(), &stop);
        if (!ParsePositiveDouble(argv[10], end) || start < 0.0 || end <= start) {
            std::wcerr << L"Invalid --real-worker range.\n";
            return EXIT_FAILURE;
        }
        request.range = {static_cast<int64_t>(std::llround(start * 1e7)), static_cast<int64_t>(std::llround(end * 1e7))};
        std::string guidesText;
        for (const wchar_t character : std::wstring_view(argv[11])) {
            if (character > 0x7F) { std::wcerr << L"Invalid --real-worker guides.\n"; return EXIT_FAILURE; }
            guidesText.push_back(static_cast<char>(character));
        }
        const auto guides = ParseGuideControls(guidesText);
        if (!guides) {
            std::wcerr << L"Invalid --real-worker guides.\n";
            return EXIT_FAILURE;
        }
        request.guides = *guides;
    }
    if (argc == 13) {
        uint32_t segmentFrames = 0;
        if (!ParseUnsigned32(argv[12], segmentFrames)) {
            std::wcerr << L"Invalid --real-worker segment frames.\n";
            return EXIT_FAILURE;
        }
        request.segmentFrames = segmentFrames;
    }
    NeuralRenderPhase lastPhase = NeuralRenderPhase::CheckingCache;
    uint64_t lastReportedFrames = 0;
    std::vector<std::chrono::steady_clock::time_point> segmentTimes;
    const auto jobStart = std::chrono::steady_clock::now();
    NeuralSegmentSink segmentSink;
    segmentSink.onSegment = [&](const NeuralRenderSegment& segment) {
        segmentTimes.push_back(std::chrono::steady_clock::now());
        std::wcout << L"segment index=" << segment.index << L" frames=" << segment.frameCount
            << L" atMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(segmentTimes.back() - jobStart).count() << L'\n';
    };
    const NeuralRenderResult result = RunNeuralWorker(argv[2], request,
        [&](const NeuralRenderProgress& progress) {
            if (progress.phase != lastPhase || progress.completedFrames == progress.totalFrames ||
                progress.completedFrames >= lastReportedFrames + 120) {
                std::wcout << L"progress phase=" << static_cast<unsigned>(progress.phase)
                    << L" frames=" << progress.completedFrames << L'/' << progress.totalFrames
                    << L" bytes=" << progress.bytes << L" elapsedMs=" << progress.elapsed.count()
                    << L" remainingMs=" << progress.estimatedRemaining.count() << L'\n';
                lastPhase = progress.phase;
                lastReportedFrames = progress.completedFrames;
            }
        }, {}, segmentSink);
    std::wcout << L"result ok=" << result.ok << L" cancelled=" << result.cancelled
        << L" encoder=" << static_cast<unsigned>(result.encoder) << L" frames=" << result.frameCount
        << L" duration100ns=" << result.duration100ns << L" nativeEvaluations=" << result.nativeEvaluations
        << L" verifiedNeuralFrames=" << result.verifiedNeuralFrames
        << L" armed=" << result.feature18ArmedBeforeCapture
        << L" evidence={upscalingOff=" << result.evidence.upscalingOff
        << L", inline=" << result.evidence.inlineInterceptionContract
        << L", created=" << result.evidence.feature18Created
        << L", evaluated=" << result.evidence.feature18Evaluated
        << L", laterFailure=" << result.evidence.laterFailure
        << L", highest=" << result.evidence.highestObservedEvaluation << L"}"
        << L" failure=" << std::string(NeuralRenderFailureName(result.failure)).c_str()
        << L" jobId=" << result.jobId << L" historyResets=" << result.historyResets << L" frameRetries=" << result.frameRetries
        << L" firstTimestamp100ns=" << result.firstTimestamp100ns
        << L" timing={samples=" << result.timing.samples << L", neuralGpuMsP50=" << result.timing.neuralGpuMsP50
        << L", p95=" << result.timing.neuralGpuMsP95 << L", max=" << result.timing.neuralGpuMsMax
        << L", guideMsMean=" << result.timing.guideMsMean << L", captureMsMean=" << result.timing.captureMsMean
        << L", peakLocalVramMiB=" << result.timing.peakLocalVramMiB << L"} detail=" << result.detail << L'\n';
    // The helper's own share of the cold start, in microseconds, as it arrived
    // over the pipe. An absent phase prints "-": this harness launches the
    // helper directly, so the player's four phases never exist here.
    std::wcout << L"coldStart={";
    for (uint32_t index = 0; index < kNeuralColdStartPhaseCount; ++index) {
        const auto phase = static_cast<NeuralColdStartPhase>(index);
        const auto elapsed = result.coldStart.Phase(phase);
        const std::string name(NeuralColdStartPhaseName(phase));
        if (index) std::wcout << L", ";
        std::wcout << name.c_str() << L'=';
        if (elapsed) std::wcout << elapsed->count() << L"us";
        else std::wcout << L'-';
    }
    std::wcout << L"}\n";
    return result.ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

void nonexistent_helper_fails_test()
{
    size_t progressCount = 0;
    const NeuralRenderResult result = RunNeuralWorker(L"definitely-not-a-neural-helper.exe",
        TestRequest(L"valid-source.mkv"), [&](const NeuralRenderProgress&) { ++progressCount; });
    CHECK(!result.ok);
    CHECK(!result.cancelled);
    CHECK(progressCount == 0);
}

void helper_main_parser_accepts_normal_and_restarted_contracts_test()
{
    NeuralRenderRequest request = TestRequest(L"source.mkv");
    request.jobId = 77;
    request.range = {10'000'000, 30'000'000};
    request.prerollFrames = 12;
    request.frameRetryLimit = 5;
    request.guides = {true, false};
    request.segmentFrames = 96;
    const HANDLE metadata = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(123));
    const HANDLE pause = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(456));
    auto view = [](const std::vector<std::wstring>& arguments) {
        std::vector<std::wstring_view> values{L"NeuralWorker.exe"};
        for (const auto& argument : arguments) values.emplace_back(argument);
        return values;
    };
    const auto normal = neural_worker_detail::BuildWorkerArguments(request, metadata, pause, false);
    const auto normalView = view(normal);
    const auto parsedNormal = neural_worker_detail::ParseWorkerArguments(normalView);
    CHECK(parsedNormal.has_value());
    if (parsedNormal) {
        CHECK(!parsedNormal->preflight);
        CHECK(!parsedNormal->configurationRestarted);
        CHECK(parsedNormal->metadata == metadata);
        CHECK(parsedNormal->request.pauseEvent == pause);
        CHECK(parsedNormal->request.sourcePath == request.sourcePath);
        CHECK(parsedNormal->request.stagingVideoPath == request.stagingVideoPath);
        CHECK(parsedNormal->request.width == 1920);
        CHECK(parsedNormal->request.height == 1080);
        CHECK(parsedNormal->request.fps == 60.0);
        CHECK(parsedNormal->request.durationSeconds == 1.0);
        CHECK(parsedNormal->request.jobId == 77);
        CHECK(parsedNormal->request.range == request.range);
        CHECK(parsedNormal->request.prerollFrames == 12);
        CHECK(parsedNormal->request.frameRetryLimit == 5);
        CHECK(parsedNormal->request.guides == request.guides);
        CHECK(parsedNormal->request.segmentFrames == 96);
    }
    const auto restarted = neural_worker_detail::BuildWorkerArguments(request, metadata, nullptr, true);
    const auto restartedView = view(restarted);
    const auto parsedRestarted = neural_worker_detail::ParseWorkerArguments(restartedView);
    CHECK(parsedRestarted.has_value());
    if (parsedRestarted) {
        CHECK(parsedRestarted->configurationRestarted);
        CHECK(parsedRestarted->request.pauseEvent == nullptr);
    }
    // Absent --segment-frames means one output file for the whole range.
    NeuralRenderRequest single = request;
    single.segmentFrames = 0;
    const auto unsegmented = neural_worker_detail::BuildWorkerArguments(single, metadata, nullptr, false);
    for (const auto& argument : unsegmented) CHECK(argument != L"--segment-frames");
    const auto unsegmentedView = view(unsegmented);
    const auto parsedSingle = neural_worker_detail::ParseWorkerArguments(unsegmentedView);
    CHECK(parsedSingle.has_value());
    if (parsedSingle) {
        CHECK(parsedSingle->request.segmentFrames == 0);
    }
    auto malformed = restarted;
    malformed.back() = L"--unexpected";
    const auto malformedView = view(malformed);
    CHECK(!neural_worker_detail::ParseWorkerArguments(malformedView).has_value());
    // The GPU colour conversion is opt-in and travels as its own optional pair, so an
    // older parent that never sends it still parses into the CPU-conversion default.
    for (const auto& argument : normal) CHECK(argument != L"--gpu-color-conversion");
    if (parsedNormal) CHECK(!parsedNormal->request.gpuColorConversion);
    NeuralRenderRequest converted = request;
    converted.gpuColorConversion = true;
    const auto convertedArguments =
        neural_worker_detail::BuildWorkerArguments(converted, metadata, pause, false);
    const auto convertedView = view(convertedArguments);
    const auto parsedConverted = neural_worker_detail::ParseWorkerArguments(convertedView);
    CHECK(parsedConverted.has_value());
    if (parsedConverted) {
        CHECK(parsedConverted->request.gpuColorConversion);
        CHECK(parsedConverted->request.segmentFrames == 96);
        CHECK(parsedConverted->request.pauseEvent == pause);
    }
    auto badConversion = convertedArguments;
    for (size_t index = 0; index + 1 < badConversion.size(); ++index) {
        if (badConversion[index] == L"--gpu-color-conversion") badConversion[index + 1] = L"2";
    }
    const auto badConversionView = view(badConversion);
    CHECK(!neural_worker_detail::ParseWorkerArguments(badConversionView).has_value());
    // The NVENC preset is opt-in and travels as its own optional pair, so an older
    // parent that never sends it still parses into the preset-7 default.
    for (const auto& argument : normal) CHECK(argument != L"--nvenc-preset");
    if (parsedNormal) CHECK(parsedNormal->request.nvencPreset == 7);
    NeuralRenderRequest presetRequest = request;
    presetRequest.nvencPreset = 5;
    const auto presetArguments =
        neural_worker_detail::BuildWorkerArguments(presetRequest, metadata, pause, false);
    const auto presetView = view(presetArguments);
    const auto parsedPreset = neural_worker_detail::ParseWorkerArguments(presetView);
    CHECK(parsedPreset.has_value());
    if (parsedPreset) {
        CHECK(parsedPreset->request.nvencPreset == 5);
    }
    auto badPresetHigh = presetArguments;
    for (size_t index = 0; index + 1 < badPresetHigh.size(); ++index) {
        if (badPresetHigh[index] == L"--nvenc-preset") badPresetHigh[index + 1] = L"8";
    }
    const auto badPresetHighView = view(badPresetHigh);
    CHECK(!neural_worker_detail::ParseWorkerArguments(badPresetHighView).has_value());
    auto badPresetZero = presetArguments;
    for (size_t index = 0; index + 1 < badPresetZero.size(); ++index) {
        if (badPresetZero[index] == L"--nvenc-preset") badPresetZero[index + 1] = L"0";
    }
    const auto badPresetZeroView = view(badPresetZero);
    CHECK(!neural_worker_detail::ParseWorkerArguments(badPresetZeroView).has_value());
    // GPU source conversion is opt-in and travels as its own optional pair, so an
    // older parent that never sends it still parses into the CPU conversion every
    // earlier helper performed - a missing flag can never switch the decode path.
    for (const auto& argument : normal) CHECK(argument != L"--gpu-source-conversion");
    if (parsedNormal) CHECK(!parsedNormal->request.gpuSourceConversion);
    NeuralRenderRequest gpuSource = request;
    gpuSource.gpuSourceConversion = true;
    const auto gpuSourceArguments =
        neural_worker_detail::BuildWorkerArguments(gpuSource, metadata, pause, false);
    const auto gpuSourceView = view(gpuSourceArguments);
    const auto parsedGpuSource = neural_worker_detail::ParseWorkerArguments(gpuSourceView);
    CHECK(parsedGpuSource.has_value());
    if (parsedGpuSource) {
        CHECK(parsedGpuSource->request.gpuSourceConversion);
        CHECK(parsedGpuSource->request.segmentFrames == 96);
        CHECK(parsedGpuSource->request.pauseEvent == pause);
    }
    auto badSourceConversion = gpuSourceArguments;
    for (size_t index = 0; index + 1 < badSourceConversion.size(); ++index) {
        if (badSourceConversion[index] == L"--gpu-source-conversion") badSourceConversion[index + 1] = L"2";
    }
    const auto badSourceConversionView = view(badSourceConversion);
    CHECK(!neural_worker_detail::ParseWorkerArguments(badSourceConversionView).has_value());
    auto duplicated = normal;
    duplicated.emplace_back(L"--width");
    duplicated.emplace_back(L"1920");
    const auto duplicatedView = view(duplicated);
    CHECK(!neural_worker_detail::ParseWorkerArguments(duplicatedView).has_value());
    auto badGuides = normal;
    for (size_t index = 0; index + 1 < badGuides.size(); ++index) {
        if (badGuides[index] == L"--guides") badGuides[index + 1] = L"mv=2,depth=1";
    }
    const auto badGuidesView = view(badGuides);
    CHECK(!neural_worker_detail::ParseWorkerArguments(badGuidesView).has_value());
    auto retiredGuides = normal;
    for (size_t index = 0; index + 1 < retiredGuides.size(); ++index) {
        if (retiredGuides[index] == L"--guides") retiredGuides[index + 1] = L"mv=1,depth=1,mask=1";
    }
    const auto retiredGuidesView = view(retiredGuides);
    // The mask field was deleted: a stale argv carrying it is not a valid contract.
    CHECK(!neural_worker_detail::ParseWorkerArguments(retiredGuidesView).has_value());
    auto badRange = normal;
    for (size_t index = 0; index + 1 < badRange.size(); ++index) {
        if (badRange[index] == L"--range-end-100ns") badRange[index + 1] = L"5000000";
    }
    const auto badRangeView = view(badRange);
    CHECK(!neural_worker_detail::ParseWorkerArguments(badRangeView).has_value());

    const auto preflight = neural_worker_detail::BuildPreflightArguments(metadata, true);
    const auto preflightView = view(preflight);
    const auto parsedPreflight = neural_worker_detail::ParseWorkerArguments(preflightView);
    CHECK(parsedPreflight.has_value());
    if (parsedPreflight) {
        CHECK(parsedPreflight->preflight);
        CHECK(parsedPreflight->configurationRestarted);
        CHECK(parsedPreflight->metadata == metadata);
    }
    auto preflightWithSource = preflight;
    preflightWithSource.insert(preflightWithSource.end() - 1, {L"--source", L"x.mkv"});
    const auto preflightWithSourceView = view(preflightWithSource);
    CHECK(!neural_worker_detail::ParseWorkerArguments(preflightWithSourceView).has_value());
}

void cancellation_of_running_child_is_bounded_test()
{
    std::stop_source stop;
    std::jthread cancel([&] {
        std::this_thread::sleep_for(100ms);
        stop.request_stop();
    });
    const auto started = std::chrono::steady_clock::now();
    const NeuralRenderResult result = RunNeuralWorker(CurrentExecutable(), TestRequest(L"hang-source.mkv"),
        {}, stop.get_token());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(!result.ok);
    CHECK(result.cancelled);
    CHECK(elapsed < 3s);
}

void malformed_or_truncated_results_are_rejected_test()
{
    const NeuralRenderResult result = RunNeuralWorker(CurrentExecutable(),
        TestRequest(L"truncated-source.mkv"));
    CHECK(!result.ok);
    CHECK(!result.cancelled);
    CHECK(!result.detail.empty());
}

void valid_result_preserves_all_verification_fields_test()
{
    NeuralRenderProgress observed{};
    size_t progressCount = 0;
    NeuralRenderRequest request = TestRequest(L"valid-source.mkv");
    request.jobId = 9001;
    request.range = {20'000'000, 0};
    const NeuralRenderResult result = RunNeuralWorker(CurrentExecutable(), request,
        [&](const NeuralRenderProgress& progress) { observed = progress; ++progressCount; });
    CHECK(result.ok);
    CHECK(!result.cancelled);
    CHECK(result.failure == NeuralRenderFailure::None);
    CHECK(result.encoder == EncoderKind::H264Software);
    CHECK(result.frameCount == 60);
    CHECK(result.duration100ns == 10'000'000);
    CHECK(result.nativeEvaluations == 60);
    CHECK(result.verifiedNeuralFrames == 60);
    CHECK(result.feature18ArmedBeforeCapture);
    CHECK(result.evidence.upscalingOff);
    CHECK(result.evidence.inlineInterceptionContract);
    CHECK(result.evidence.feature18Created);
    CHECK(result.evidence.feature18Evaluated);
    CHECK(!result.evidence.laterFailure);
    CHECK(result.evidence.highestObservedEvaluation == 75);
    CHECK(result.evidence.Valid());
    CHECK(result.jobId == 9001);
    CHECK(result.historyResets == 2);
    CHECK(result.frameRetries == 1);
    const SceneCutAccounting expectedCuts{.acceptedStrong = 3, .acceptedWeak = 5, .suppressed = 7};
    CHECK(result.sceneCuts == expectedCuts);
    CHECK(result.firstTimestamp100ns == 20'000'000);
    const NeuralRenderTiming expectedTiming{60, 1.5, 2.5, 4.0, 0.7, 0.3, 2048};
    CHECK(result.timing == expectedTiming);
    CHECK(progressCount == 1);
    CHECK(observed.phase == NeuralRenderPhase::NeuralRendering);
    CHECK(observed.completedFrames == 17);
    CHECK(observed.totalFrames == 60);
}

void request_fields_reach_the_helper_intact_test()
{
    NeuralRenderRequest request = TestRequest(L"echo-request-source.mkv");
    request.guides = {false, true};
    request.prerollFrames = 7;
    request.frameRetryLimit = 2;
    request.segmentFrames = 90;
    request.range = {30'000'000, 50'000'000};
    HANDLE pause = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    request.pauseEvent = pause;
    const NeuralRenderResult result = RunNeuralWorker(CurrentExecutable(), request);
    CloseHandle(pause);
    CHECK(result.ok);
    if (!result.ok) std::wcerr << L"detail: " << result.detail << L" failure=" << static_cast<int>(result.failure) << L'\n';
    CHECK(result.detail == L"guides=mv=0,depth=1 preroll=7 retry=2 segments=90 range=30000000-50000000 pause=1");
}

void crashed_helper_is_relaunched_at_most_once_test()
{
    std::vector<NeuralRenderProgress> progress;
    size_t restarts = 0;
    NeuralSegmentSink sink;
    sink.onRestart = [&] { ++restarts; };
    const NeuralRenderResult crashed = RunNeuralWorker(CurrentExecutable(), TestRequest(L"crash-source.mkv"),
        [&](const NeuralRenderProgress& update) { progress.push_back(update); }, {}, sink);
    CHECK(!crashed.ok);
    CHECK(!crashed.cancelled);
    CHECK(crashed.failure == NeuralRenderFailure::RetryExhausted);
    CHECK(!crashed.detail.empty());
    // progress(3/60), Recovering(1), progress(3/60): one relaunch, then exhaustion.
    size_t recovering = 0, rendering = 0;
    for (const auto& update : progress) {
        if (update.phase == NeuralRenderPhase::Recovering) {
            ++recovering;
            CHECK(update.recovering == NeuralRenderFailure::WorkerCrashed);
            CHECK(update.retries == 1);
        } else if (update.phase == NeuralRenderPhase::NeuralRendering) {
            ++rendering;
        }
    }
    CHECK(recovering == 1);
    CHECK(rendering == 2);
    // The relaunch renders from frame zero: any published segment is invalid.
    CHECK(restarts == 1);

    const NeuralRenderResult removed = RunNeuralWorker(CurrentExecutable(),
        TestRequest(L"device-removed-source.mkv"), {}, {}, {}, 0);
    CHECK(!removed.ok);
    CHECK(removed.failure == NeuralRenderFailure::RetryExhausted);
    CHECK(removed.detail.find(L"fake device removal") != std::wstring::npos);

    const NeuralRenderResult wrongJob = RunNeuralWorker(CurrentExecutable(), TestRequest(L"wrong-job-source.mkv"));
    CHECK(!wrongJob.ok);
    CHECK(wrongJob.failure == NeuralRenderFailure::Identity);
}

void preflight_receipt_round_trips_and_restarts_once_test()
{
    const NeuralPreflightResult preflight = RunNeuralPreflight(CurrentExecutable());
    CHECK(preflight.ok);
    CHECK(!preflight.cancelled);
    CHECK(preflight.json == "{\"ok\":true,\"gpu\":\"fake\"}");
    const NeuralPreflightResult missing = RunNeuralPreflight(L"definitely-not-a-neural-helper.exe");
    CHECK(!missing.ok);
    CHECK(!missing.detail.empty());
}

void preflight_failure_detail_comes_from_the_receipt_diagnosis_test()
{
    const auto withShape = [](const wchar_t* shape) {
        CHECK(SetEnvironmentVariableW(L"DLSSVIDEOPLAYER_FAKE_PREFLIGHT", shape) != FALSE);
        const NeuralPreflightResult result = RunNeuralPreflight(CurrentExecutable());
        CHECK(SetEnvironmentVariableW(L"DLSSVIDEOPLAYER_FAKE_PREFLIGHT", nullptr) != FALSE);
        return result;
    };

    // The classified sentence and its cause survive the receipt round trip.
    const NeuralPreflightResult refused = withShape(L"refused");
    CHECK(!refused.ok);
    CHECK(!refused.cancelled);
    CHECK(refused.cause == NeuralPreflightCause::DriverBelowFloor);
    CHECK(refused.detail.find(L"566.14") != std::wstring::npos);
    CHECK(refused.detail.find(L"610.47") != std::wstring::npos);
    CHECK(refused.detail.find(L"616.64") != std::wstring::npos);

    // A receipt without a diagnosis still says something, unclassified.
    const NeuralPreflightResult bare = withShape(L"bare");
    CHECK(!bare.ok);
    CHECK(bare.cause == NeuralPreflightCause::None);
    CHECK_EQ(std::wstring(L"The neural runtime preflight did not arm feature 18."), bare.detail);
}

void preflight_latch_holds_one_verdict_per_runtime_identity_test()
{
    const NeuralPreflightKey key{L"NVIDIA GeForce RTX 3060 Laptop GPU", L"32.0.15.6614", "runtime-digest-a"};
    constexpr std::wstring_view detail =
        L"NVIDIA driver 566.14 is below the 610.47 minimum for neural rendering.";
    NeuralPreflightLatch latch;
    // A non-empty detail is the whole predicate: it is both the reason to skip
    // the probe and the sentence the render fails with.
    CHECK(latch.LatchedFailureDetail(key).empty());

    latch.RecordFailure(key, std::wstring(detail));
    CHECK_EQ(std::wstring(detail), latch.LatchedFailureDetail(key));

    // Anything that can change the answer carries no verdict, so it probes.
    const NeuralPreflightKey newDriver{key.gpu, L"32.0.16.1664", key.runtimeDigest};
    const NeuralPreflightKey otherGpu{L"NVIDIA GeForce RTX 5090", key.driver, key.runtimeDigest};
    const NeuralPreflightKey newRuntime{key.gpu, key.driver, "runtime-digest-b"};
    for (const NeuralPreflightKey& other : {newDriver, otherGpu, newRuntime})
        CHECK(latch.LatchedFailureDetail(other).empty());
    CHECK(!latch.LatchedFailureDetail(key).empty());

    // A pass is remembered too, with its receipt: skipping the probe is only
    // allowed when the render's own receipt can still carry the evidence.
    latch.RecordSuccess(key, "{\"schema\":2,\"ok\":true}");
    CHECK(latch.LatchedFailureDetail(key).empty());
    CHECK_EQ(std::string("{\"schema\":2,\"ok\":true}"), latch.LatchedSuccessJson(key));
    for (const NeuralPreflightKey& other : {newDriver, otherGpu, newRuntime})
        CHECK(latch.LatchedSuccessJson(other).empty());
    // A pass without a receipt cannot stand in for one.
    latch.RecordSuccess(key, std::string{});
    CHECK(latch.LatchedSuccessJson(key).empty());

    latch.RecordSuccess(key, "{\"ok\":true}");
    latch.RecordFailure(key, std::wstring(detail));
    CHECK(latch.LatchedSuccessJson(key).empty());
    latch.Invalidate();
    CHECK(latch.LatchedFailureDetail(key).empty());
    CHECK(latch.LatchedSuccessJson(key).empty());

    // The field storm: three playback starts on the same doomed machine ran
    // three ~5 s probes in 65 s. One probe is the whole point of the latch.
    NeuralPreflightLatch storm;
    int probes = 0;
    for (int playbackStart = 0; playbackStart < 3; ++playbackStart) {
        if (!storm.LatchedFailureDetail(key).empty()) continue;
        ++probes;
        storm.RecordFailure(key, std::wstring(detail));
    }
    CHECK_EQ(1, probes);
}

void runtime_lease_admits_one_holder_per_directory_test()
{
    const std::filesystem::path runtime = L"D:/example/neural-runtime";
    // The name identifies the directory, not the process or the path spelling.
    CHECK(NeuralRuntimeLease::MutexName(runtime) == NeuralRuntimeLease::MutexName(L"D:\\Example\\neural-runtime\\"));
    CHECK(NeuralRuntimeLease::MutexName(runtime) != NeuralRuntimeLease::MutexName(L"D:/example/other-runtime"));
    CHECK(NeuralRuntimeLease::MutexName(runtime).starts_with(L"Local\\DLSSVideoPlayer.neural-runtime."));

    auto heldElsewhere = [&](const std::filesystem::path& directory) {
        // Ownership is per thread, so a competing holder must be another thread.
        bool held = false;
        std::jthread other([&] { NeuralRuntimeLease lease(directory, 0ms); held = lease.Held(); });
        other.join();
        return held;
    };
    {
        NeuralRuntimeLease lease(runtime);
        CHECK(lease.Held());
        CHECK(!heldElsewhere(runtime));
        // A different runtime directory is unaffected.
        CHECK(heldElsewhere(L"D:/example/other-runtime"));
    }
    // Releasing lets the next render in.
    CHECK(heldElsewhere(runtime));
    // An empty directory is not a shared resource and never blocks.
    NeuralRuntimeLease none(L"");
    CHECK(!none.Held());
}

void protocol_rejects_inconsistent_results_test()
{
    NeuralRenderResult okWithFailure;
    okWithFailure.ok = true;
    okWithFailure.feature18ArmedBeforeCapture = true;
    okWithFailure.evidence = {true, true, true, true, false, 1};
    okWithFailure.frameCount = okWithFailure.nativeEvaluations = okWithFailure.verifiedNeuralFrames = 1;
    okWithFailure.duration100ns = 1;
    CHECK(DecodeResult(EncodeResult(okWithFailure)).has_value());
    okWithFailure.failure = NeuralRenderFailure::GpuStall;
    CHECK(!DecodeResult(EncodeResult(okWithFailure)).has_value());
    NeuralRenderResult failedWithoutKind;
    CHECK(!DecodeResult(EncodeResult(failedWithoutKind)).has_value());
    NeuralRenderResult cancelledWrongKind;
    cancelledWrongKind.cancelled = true;
    cancelledWrongKind.failure = NeuralRenderFailure::Source;
    CHECK(!DecodeResult(EncodeResult(cancelledWrongKind)).has_value());
    cancelledWrongKind.failure = NeuralRenderFailure::Cancelled;
    CHECK(DecodeResult(EncodeResult(cancelledWrongKind)).has_value());
    // The reserved bytes are the only growth room left in WireResult; a helper that
    // writes anything there is not speaking this version of the protocol.
    auto reservedInUse = EncodeResult(cancelledWrongKind);
    reservedInUse[10] = std::byte{1};
    CHECK(!DecodeResult(reservedInUse).has_value());
    NeuralRenderProgress recoveringWithoutKind;
    recoveringWithoutKind.phase = NeuralRenderPhase::Recovering;
    const WireProgress wire = EncodeProgress(recoveringWithoutKind);
    CHECK(!DecodeProgress(std::span<const std::byte>(reinterpret_cast<const std::byte*>(&wire), sizeof(wire))).has_value());
}

void segment_messages_round_trip_and_reject_malformed_test()
{
    const NeuralRenderSegment segment = TestSegment(3, 48);
    const auto decoded = DecodeSegment(EncodeSegment(segment, kTestFrameDuration100ns));
    CHECK(decoded.has_value());
    if (decoded) {
        CHECK(decoded->index == 3);
        CHECK(decoded->firstFrameNumber == segment.firstFrameNumber);
        CHECK(decoded->firstTimestamp100ns == segment.firstTimestamp100ns);
        CHECK(decoded->frameCount == 48);
        CHECK(decoded->end100ns == segment.end100ns);
        CHECK(decoded->fileName == L"neural-00003.mkv");
    }
    NeuralRenderSegment empty = segment;
    empty.frameCount = 0;
    CHECK(!DecodeSegment(EncodeSegment(empty, kTestFrameDuration100ns)).has_value());
    CHECK(!DecodeSegment(EncodeSegment(segment, 0)).has_value());
    CHECK(!DecodeSegment(EncodeSegment(segment, -1)).has_value());
    NeuralRenderSegment nested = segment;
    nested.fileName = L"sub/neural-00003.mkv";
    CHECK(!DecodeSegment(EncodeSegment(nested, kTestFrameDuration100ns)).has_value());
    nested.fileName = L"sub\\neural-00003.mkv";
    CHECK(!DecodeSegment(EncodeSegment(nested, kTestFrameDuration100ns)).has_value());
    NeuralRenderSegment traversal = segment;
    traversal.fileName = L"neural..mkv";
    CHECK(!DecodeSegment(EncodeSegment(traversal, kTestFrameDuration100ns)).has_value());
    NeuralRenderSegment unnamed = segment;
    unnamed.fileName.clear();
    CHECK(!DecodeSegment(EncodeSegment(unnamed, kTestFrameDuration100ns)).has_value());
    // The encoder caps the name; a hand-built oversized payload is refused.
    NeuralRenderSegment longName = segment;
    longName.fileName.assign(400, L'n');
    const auto capped = DecodeSegment(EncodeSegment(longName, kTestFrameDuration100ns));
    CHECK(capped.has_value());
    if (capped) CHECK(capped->fileName.size() == kMaximumSegmentNameBytes / sizeof(wchar_t));
    CHECK(!DecodeSegment(OversizedSegmentPayload(segment)).has_value());
    const auto payload = EncodeSegment(segment, kTestFrameDuration100ns);
    CHECK(!DecodeSegment(std::span<const std::byte>(payload.data(), payload.size() - 2)).has_value());
}

void metadata_reader_accepts_segments_before_the_result_test()
{
    const WireProgress progress = EncodeProgress([] {
        NeuralRenderProgress value;
        value.phase = NeuralRenderPhase::NeuralRendering;
        value.completedFrames = 24;
        value.totalFrames = 48;
        return value;
    }());
    const auto resultPayload = EncodeResult(ValidFakeResult(4242));
    const auto segmentPayload = [](uint64_t index) {
        return EncodeSegment(TestSegment(index, 24), kTestFrameDuration100ns);
    };
    std::vector<std::byte> stream;
    AppendMessage(stream, WireKind::Progress, AsBytes(progress));
    AppendMessage(stream, WireKind::Segment, segmentPayload(0));
    AppendMessage(stream, WireKind::Segment, segmentPayload(1));
    AppendMessage(stream, WireKind::Result, resultPayload);
    const auto accepted = neural_worker_detail::DecodeMetadataStream(stream);
    CHECK(!accepted.malformed);
    CHECK(accepted.complete);
    CHECK(accepted.progressUpdates == 1);
    CHECK(accepted.restarts == 0);
    CHECK(accepted.segments.size() == 2);
    if (accepted.segments.size() == 2) {
        CHECK(accepted.segments[0].index == 0);
        CHECK(accepted.segments[0].fileName == L"neural-00000.mkv");
        CHECK(accepted.segments[1].index == 1);
        CHECK(accepted.segments[1].firstTimestamp100ns == accepted.segments[0].end100ns);
    }

    // A segment can never follow the terminal result.
    std::vector<std::byte> afterResult = stream;
    AppendMessage(afterResult, WireKind::Segment, segmentPayload(2));
    const auto rejected = neural_worker_detail::DecodeMetadataStream(afterResult);
    CHECK(rejected.malformed);
    CHECK(!rejected.complete);

    // A skipped index is malformed; a repeated 0 restarts the sequence.
    std::vector<std::byte> gap;
    AppendMessage(gap, WireKind::Segment, segmentPayload(0));
    AppendMessage(gap, WireKind::Segment, segmentPayload(2));
    CHECK(neural_worker_detail::DecodeMetadataStream(gap).malformed);
    std::vector<std::byte> replay;
    AppendMessage(replay, WireKind::Segment, segmentPayload(0));
    AppendMessage(replay, WireKind::Segment, segmentPayload(1));
    AppendMessage(replay, WireKind::Segment, segmentPayload(0));
    AppendMessage(replay, WireKind::Result, resultPayload);
    const auto restarted = neural_worker_detail::DecodeMetadataStream(replay);
    CHECK(!restarted.malformed);
    CHECK(restarted.complete);
    CHECK(restarted.restarts == 1);
    CHECK(restarted.segments.size() == 1);

    // A helper speaking the previous protocol version is refused outright.
    std::vector<std::byte> older;
    AppendMessage(older, static_cast<uint16_t>(kProtocolVersion - 1), WireKind::Segment, segmentPayload(0));
    CHECK(neural_worker_detail::DecodeMetadataStream(older).malformed);
    std::vector<std::byte> broken;
    AppendMessage(broken, WireKind::Segment, OversizedSegmentPayload(TestSegment(0, 24)));
    CHECK(neural_worker_detail::DecodeMetadataStream(broken).malformed);
}

void running_helper_publishes_segments_before_its_result_test()
{
    NeuralRenderRequest request = TestRequest(L"segment-source.mkv");
    request.jobId = 5150;
    request.segmentFrames = 24;
    std::vector<NeuralRenderSegment> segments;
    size_t restarts = 0;
    NeuralSegmentSink sink;
    sink.onSegment = [&](const NeuralRenderSegment& segment) { segments.push_back(segment); };
    sink.onRestart = [&] { ++restarts; };
    const NeuralRenderResult result = RunNeuralWorker(CurrentExecutable(), request, {}, {}, sink);
    CHECK(result.ok);
    CHECK(result.jobId == 5150);
    CHECK(restarts == 0);
    CHECK(segments.size() == 2);
    if (segments.size() == 2) {
        CHECK(segments[0].index == 0);
        CHECK(segments[0].frameCount == 24);
        CHECK(segments[0].firstFrameNumber == 0);
        CHECK(segments[0].fileName == L"neural-00000.mkv");
        CHECK(segments[1].index == 1);
        CHECK(segments[1].firstFrameNumber == 24);
        CHECK(segments[1].fileName == L"neural-00001.mkv");
        CHECK(segments[1].firstTimestamp100ns == segments[0].end100ns);
    }
}

void configuration_retry_is_sequential_and_bounded_test()
{
    size_t progressCount = 0, launches = 0;
    const auto configured = RunNeuralWorker(CurrentExecutable(), TestRequest(L"configure-once-source.mkv"),
        [&](const NeuralRenderProgress&) { ++progressCount; }, {}, {}, kDefaultCrashRelaunchLimit,
        [&] { ++launches; });
    CHECK(configured.ok);
    CHECK(configured.frameCount == 60);
    CHECK(progressCount == 1);
    // The repaired helper and its replacement are both launch cost, so both
    // processes are reported rather than only the one that rendered.
    CHECK(launches == 2);
    const auto repeated = RunNeuralWorker(CurrentExecutable(), TestRequest(L"configure-always-source.mkv"));
    CHECK(!repeated.ok);
    CHECK(!repeated.detail.empty());
}

void helper_cold_start_timeline_reaches_the_parent_test()
{
    size_t launches = 0;
    const NeuralRenderResult result = RunNeuralWorker(CurrentExecutable(),
        TestRequest(L"timeline-source.mkv"), {}, {}, {}, kDefaultCrashRelaunchLimit,
        [&] { ++launches; });
    CHECK(result.ok);
    CHECK(launches == 1);
    CHECK(result.coldStart == FakeHelperTimeline());
    // The helper measures its own five phases and claims nothing else: the
    // player's preparation and its request-to-picture total are not its to report.
    CHECK(!result.coldStart.Phase(NeuralColdStartPhase::Request).has_value());
    CHECK(!result.coldStart.Phase(NeuralColdStartPhase::Attach).has_value());
    CHECK(!result.coldStart.Total().has_value());
    // A helper that never reports one leaves every phase absent rather than zero.
    const NeuralRenderResult silent = RunNeuralWorker(CurrentExecutable(), TestRequest(L"valid-source.mkv"));
    CHECK(silent.ok);
    CHECK(silent.coldStart == NeuralColdStartTimeline{});
}

void cold_start_timeline_messages_are_validated_test()
{
    const auto resultPayload = EncodeResult(ValidFakeResult(9001));
    const WireTimeline valid = EncodeTimeline(FakeHelperTimeline());
    std::vector<std::byte> stream;
    AppendMessage(stream, WireKind::Timeline, AsBytes(valid));
    AppendMessage(stream, WireKind::Result, resultPayload);
    const auto accepted = neural_worker_detail::DecodeMetadataStream(stream);
    CHECK(!accepted.malformed);
    CHECK(accepted.complete);
    CHECK(accepted.timeline == FakeHelperTimeline());

    // A helper speaking the previous protocol version is refused outright, so a
    // stale NeuralWorker.exe cannot have its shorter messages read as v5 ones.
    std::vector<std::byte> older;
    AppendMessage(older, static_cast<uint16_t>(kProtocolVersion - 1), WireKind::Timeline, AsBytes(valid));
    CHECK(neural_worker_detail::DecodeMetadataStream(older).malformed);

    // Reserved bytes are the only room a later version has; a helper that
    // writes there is not the helper this parent validated.
    WireTimeline reserved = valid;
    reserved.reserved[2] = 1;
    std::vector<std::byte> dirty;
    AppendMessage(dirty, WireKind::Timeline, AsBytes(reserved));
    CHECK(neural_worker_detail::DecodeMetadataStream(dirty).malformed);

    // A phase the player owns, a duration in a slot with no present bit, a
    // negative duration, an empty timeline and a second timeline are all
    // helpers that measured something they could not have measured.
    for (const WireTimeline broken : {
             [&] { WireTimeline value = valid;
                   value.present |= 1u << static_cast<uint32_t>(NeuralColdStartPhase::Attach);
                   return value; }(),
             [&] { WireTimeline value = valid;
                   value.microseconds[static_cast<size_t>(NeuralColdStartPhase::Request)] = 5;
                   return value; }(),
             [&] { WireTimeline value = valid;
                   value.microseconds[static_cast<size_t>(NeuralColdStartPhase::NeuralInit)] = -1;
                   return value; }(),
             WireTimeline{}}) {
        std::vector<std::byte> rejected;
        AppendMessage(rejected, WireKind::Timeline, AsBytes(broken));
        CHECK(neural_worker_detail::DecodeMetadataStream(rejected).malformed);
    }
    std::vector<std::byte> twice;
    AppendMessage(twice, WireKind::Timeline, AsBytes(valid));
    AppendMessage(twice, WireKind::Timeline, AsBytes(valid));
    CHECK(neural_worker_detail::DecodeMetadataStream(twice).malformed);

    // The payload is fixed size: a short or padded one is not a timeline.
    std::vector<std::byte> truncated;
    AppendMessage(truncated, WireKind::Timeline, AsBytes(valid).first(sizeof(WireTimeline) - 8));
    CHECK(neural_worker_detail::DecodeMetadataStream(truncated).malformed);
}

// --- The resident command channel and its job loop -------------------------

using resident_worker::CommandChannel;
using resident_worker::JobOutcome;
using resident_worker::ResidentExit;

// An anonymous pipe the test writes command frames into. The read end is handed
// to the channel, which owns it from then on; the write end is closed here so
// the channel's reader sees the end of the stream when the test means it to and
// not when this process exits.
class CommandPipe {
public:
    CommandPipe() { CHECK(CreatePipe(&read_, &write_, nullptr, 0) != 0); }
    ~CommandPipe()
    {
        CloseWrite();
        if (read_) CloseHandle(read_);
    }
    CommandPipe(const CommandPipe&) = delete;
    CommandPipe& operator=(const CommandPipe&) = delete;

    // Transfers the read end to the caller, which is what CommandChannel wants.
    HANDLE TakeRead()
    {
        HANDLE handle = read_;
        read_ = nullptr;
        return handle;
    }

    void Send(CommandKind kind, std::span<const std::byte> payload = {})
    {
        CHECK(WriteCommand(write_, kind, payload.data(), static_cast<uint32_t>(payload.size())));
    }

    // A frame built without the protocol's own writer, so the header fields the
    // helper is supposed to refuse can actually be produced.
    void SendRaw(uint32_t magic, uint16_t version, uint16_t kind,
                 std::span<const std::byte> payload = {})
    {
        const WireHeader header{magic, version, kind, static_cast<uint32_t>(payload.size())};
        CHECK(WriteAll(write_, &header, sizeof(header)));
        if (!payload.empty()) CHECK(WriteAll(write_, payload.data(), payload.size()));
    }

    void CloseWrite()
    {
        if (!write_) return;
        CloseHandle(write_);
        write_ = nullptr;
    }

private:
    HANDLE read_{};
    HANDLE write_{};
};

// Stands in for the runner that renders. Every observation the loop's contract
// is written against is recorded here, and a job can be made to block until it
// is stopped so a cancel has something to interrupt.
struct RecordingRunner {
    size_t readyCalls{};
    std::vector<std::vector<std::wstring>> jobs;
    std::vector<std::wstring> refusals;
    JobOutcome outcome{JobOutcome::Completed};
    bool blockUntilStopped{};
    bool sawStop{};
    bool readyFails{};
    std::chrono::milliseconds jobDuration{0};

    bool Ready()
    {
        ++readyCalls;
        return !readyFails;
    }

    bool Refuse(std::wstring_view detail)
    {
        refusals.emplace_back(detail);
        return true;
    }

    JobOutcome Job(std::span<const std::wstring> argv, std::stop_token stop)
    {
        jobs.emplace_back(argv.begin(), argv.end());
        if (jobDuration.count()) std::this_thread::sleep_for(jobDuration);
        while (blockUntilStopped && !stop.stop_requested()) std::this_thread::sleep_for(1ms);
        sawStop = stop.stop_requested();
        return sawStop ? JobOutcome::Cancelled : outcome;
    }
};

std::vector<std::wstring> ResidentJobArguments()
{
    NeuralRenderRequest request = TestRequest(L"resident-source.mkv");
    request.jobId = 4242;
    std::vector<std::wstring> argv{L"NeuralWorker.exe"};
    const auto tail = neural_worker_detail::BuildWorkerArguments(
        request, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(9)), nullptr, false);
    argv.insert(argv.end(), tail.begin(), tail.end());
    return argv;
}

void resident_loop_answers_hello_and_serves_jobs_test()
{
    const std::vector<std::wstring> argv = ResidentJobArguments();
    const std::vector<std::byte> payload = EncodeJobArguments(argv);
    CommandPipe pipe;
    pipe.Send(CommandKind::Hello);
    pipe.Send(CommandKind::Job, payload);
    pipe.Send(CommandKind::Job, payload);
    pipe.Send(CommandKind::Shutdown);
    RecordingRunner runner;
    CommandChannel channel(pipe.TakeRead(), nullptr);
    CHECK(RunResidentLoop(channel, runner, 10s) == ResidentExit::Shutdown);
    // Hello is answered exactly once, and the two jobs arrive with the argument
    // vector the parent encoded, byte for byte: the helper's own parser is the
    // only thing that decides what those arguments mean.
    CHECK_EQ(size_t{1}, runner.readyCalls);
    CHECK_EQ(size_t{2}, runner.jobs.size());
    CHECK(runner.refusals.empty());
    if (runner.jobs.size() == 2) {
        CHECK(runner.jobs[0] == argv);
        CHECK(runner.jobs[1] == argv);
    }
}

void resident_loop_refuses_frames_it_cannot_trust_test()
{
    const std::vector<std::byte> payload = EncodeJobArguments(ResidentJobArguments());
    const std::array<std::byte, 4> fourBytes{};

    // A header this version cannot vouch for leaves the stream at an unknown
    // offset, so the session ends rather than guessing where the next frame
    // starts. Every case here must end the session WITHOUT running a job.
    struct Framing {
        uint32_t magic;
        uint16_t version;
        uint16_t kind;
        bool payload;
    };
    for (const Framing broken : {
             Framing{kProtocolMagic, static_cast<uint16_t>(kProtocolVersion - 1),
                     static_cast<uint16_t>(CommandKind::Job), true},
             Framing{kProtocolMagic, static_cast<uint16_t>(kProtocolVersion + 1),
                     static_cast<uint16_t>(CommandKind::Job), true},
             Framing{kProtocolMagic ^ 1u, kProtocolVersion,
                     static_cast<uint16_t>(CommandKind::Job), true},
             Framing{kProtocolMagic, kProtocolVersion, 0, false},
             Framing{kProtocolMagic, kProtocolVersion, 5, false},
             // Only Job carries a payload; anything else with one is not this
             // protocol's frame whatever its kind says.
             Framing{kProtocolMagic, kProtocolVersion,
                     static_cast<uint16_t>(CommandKind::Hello), true}}) {
        CommandPipe pipe;
        pipe.SendRaw(broken.magic, broken.version, broken.kind,
                     broken.payload ? std::span<const std::byte>(payload)
                                    : std::span<const std::byte>());
        pipe.Send(CommandKind::Job, payload);
        RecordingRunner runner;
        CommandChannel channel(pipe.TakeRead(), nullptr);
        CHECK(RunResidentLoop(channel, runner, 10s) == ResidentExit::Malformed);
        CHECK(runner.jobs.empty());
        CHECK(runner.readyCalls == 0);
    }

    // A Job payload the decoder refuses is different in kind: the frame's
    // length was known, so the stream is still in sync. That job is refused and
    // the helper stays, which the Shutdown behind it proves.
    std::vector<std::byte> trailing = payload;
    trailing.push_back(std::byte{0});
    uint32_t zeroCount = 0;
    std::vector<std::byte> emptyVector(sizeof(zeroCount));
    std::memcpy(emptyVector.data(), &zeroCount, sizeof(zeroCount));
    for (const std::vector<std::byte>& refused : {trailing, emptyVector, std::vector<std::byte>{}}) {
        CommandPipe pipe;
        pipe.Send(CommandKind::Job, refused);
        pipe.Send(CommandKind::Shutdown);
        RecordingRunner runner;
        CommandChannel channel(pipe.TakeRead(), nullptr);
        CHECK(RunResidentLoop(channel, runner, 10s) == ResidentExit::Shutdown);
        CHECK(runner.jobs.empty());
        CHECK_EQ(size_t{1}, runner.refusals.size());
    }
}

void resident_loop_exits_on_shutdown_and_on_idle_test()
{
    // The shipped budget, stated here because it is the number that bounds
    // parked VRAM: a change to it is a change to that promise.
    static_assert(resident_worker::kIdleTimeout == std::chrono::seconds{30});

    {
        CommandPipe pipe;
        pipe.Send(CommandKind::Shutdown);
        RecordingRunner runner;
        CommandChannel channel(pipe.TakeRead(), nullptr);
        CHECK(RunResidentLoop(channel, runner, 10s) == ResidentExit::Shutdown);
    }
    {
        // Nothing arrives and the parent keeps its end of the pipe open, so the
        // only thing that can end this session is the idle budget - and it must
        // actually wait for it rather than falling out of an empty queue.
        CommandPipe pipe;
        RecordingRunner runner;
        CommandChannel channel(pipe.TakeRead(), nullptr);
        const auto started = std::chrono::steady_clock::now();
        CHECK(RunResidentLoop(channel, runner, 200ms) == ResidentExit::Idle);
        CHECK(std::chrono::steady_clock::now() - started >= 180ms);
    }
    {
        // "No job for this long" is measured from the end of the last job, so a
        // job that took longer than the budget does not itself expire it.
        CommandPipe pipe;
        const std::vector<std::byte> payload = EncodeJobArguments(ResidentJobArguments());
        pipe.Send(CommandKind::Job, payload);
        RecordingRunner runner;
        runner.jobDuration = 300ms;
        CommandChannel channel(pipe.TakeRead(), nullptr);
        const auto started = std::chrono::steady_clock::now();
        CHECK(RunResidentLoop(channel, runner, 200ms) == ResidentExit::Idle);
        CHECK(std::chrono::steady_clock::now() - started >= 480ms);
        CHECK_EQ(size_t{1}, runner.jobs.size());
    }
}

void resident_loop_cancel_stops_the_job_and_keeps_the_helper_test()
{
    const std::vector<std::byte> payload = EncodeJobArguments(ResidentJobArguments());
    {
        CommandPipe pipe;
        pipe.Send(CommandKind::Job, payload);
        pipe.Send(CommandKind::Cancel);
        pipe.Send(CommandKind::Shutdown);
        RecordingRunner runner;
        runner.blockUntilStopped = true;
        CommandChannel channel(pipe.TakeRead(), nullptr);
        // The job only ends because the cancel reached it while it was running,
        // and the session only ends on the Shutdown behind it: a cancel leaves
        // the helper resident and idle.
        CHECK(RunResidentLoop(channel, runner, 10s) == ResidentExit::Shutdown);
        CHECK_EQ(size_t{1}, runner.jobs.size());
        CHECK(runner.sawStop);
    }
    {
        // A cancel arriving with no job to cancel is a no-op, not an exit.
        CommandPipe pipe;
        pipe.Send(CommandKind::Cancel);
        RecordingRunner runner;
        CommandChannel channel(pipe.TakeRead(), nullptr);
        CHECK(RunResidentLoop(channel, runner, 200ms) == ResidentExit::Idle);
    }
    {
        // Shutdown during a job stops it too, and the job still gets to report
        // before the session ends.
        CommandPipe pipe;
        pipe.Send(CommandKind::Job, payload);
        pipe.Send(CommandKind::Shutdown);
        RecordingRunner runner;
        runner.blockUntilStopped = true;
        CommandChannel channel(pipe.TakeRead(), nullptr);
        CHECK(RunResidentLoop(channel, runner, 10s) == ResidentExit::Shutdown);
        CHECK_EQ(size_t{1}, runner.jobs.size());
        CHECK(runner.sawStop);
    }
}

void resident_loop_ends_when_the_parent_dies_test()
{
    // The job object already kills a helper whose player died. This is the
    // second mechanism, because the failure it prevents is a process holding
    // the GPU and the runtime lease with nobody left to reap it.
    std::wstring commandLine = L"\"" + CurrentExecutable().wstring() + L"\" --exit-now";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    CHECK(CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &startup, &process) != 0);
    if (!process.hProcess) return;
    CloseHandle(process.hThread);
    CommandPipe pipe;
    RecordingRunner runner;
    {
        CommandChannel channel(pipe.TakeRead(), process.hProcess);
        // The command pipe stays open and empty, and the idle budget is far
        // longer than the child lives, so the only thing that can end this
        // session is the parent handle signalling.
        CHECK(RunResidentLoop(channel, runner, 30s) == ResidentExit::ParentExited);
    }
    CloseHandle(process.hProcess);
    CHECK(runner.jobs.empty());
}

void resident_loop_stops_serving_after_an_invalidating_job_test()
{
    const std::vector<std::byte> payload = EncodeJobArguments(ResidentJobArguments());
    {
        // A failed job retires the process: the session log's failure lines are
        // scanned session-wide, so the second job queued behind it must never
        // run in this helper.
        CommandPipe pipe;
        pipe.Send(CommandKind::Job, payload);
        pipe.Send(CommandKind::Job, payload);
        RecordingRunner runner;
        runner.outcome = JobOutcome::Invalidated;
        CommandChannel channel(pipe.TakeRead(), nullptr);
        CHECK(RunResidentLoop(channel, runner, 10s) == ResidentExit::JobInvalidated);
        CHECK_EQ(size_t{1}, runner.jobs.size());
    }
    {
        // Nothing left to report to: the session ends instead of rendering into
        // a pipe nobody is reading.
        CommandPipe pipe;
        pipe.Send(CommandKind::Hello);
        RecordingRunner runner;
        runner.readyFails = true;
        CommandChannel channel(pipe.TakeRead(), nullptr);
        CHECK(RunResidentLoop(channel, runner, 10s) == ResidentExit::WriteFailed);
    }
    {
        // The parent closed its end without saying goodbye.
        CommandPipe pipe;
        pipe.CloseWrite();
        RecordingRunner runner;
        CommandChannel channel(pipe.TakeRead(), nullptr);
        CHECK(RunResidentLoop(channel, runner, 10s) == ResidentExit::Closed);
    }
}

void resident_launch_line_is_a_helper_and_not_a_job_test()
{
    const HANDLE metadata = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(11));
    const HANDLE command = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(22));
    const HANDLE pause = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(33));
    const HANDLE parent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(44));
    auto parse = [](std::vector<std::wstring_view> values) {
        return neural_worker_detail::ParseWorkerArguments(values);
    };
    const auto resident = parse({L"NeuralWorker.exe", L"--neural-worker", L"--metadata-handle", L"11",
                                 L"--command-handle", L"22", L"--pause-event", L"33",
                                 L"--parent-process", L"44"});
    CHECK(resident.has_value());
    if (resident) {
        CHECK(resident->Resident());
        CHECK(resident->metadata == metadata);
        CHECK(resident->command == command);
        CHECK(resident->request.pauseEvent == pause);
        CHECK(resident->parentProcess == parent);
        // The launch line describes a helper: it carries no job, and the first
        // job arrives as a command frame.
        CHECK(resident->request.sourcePath.empty());
        CHECK(resident->request.width == 0);
    }
    // A job field on a resident launch line is a job nobody asked for, arriving
    // beside a channel that is about to deliver one.
    CHECK(!parse({L"NeuralWorker.exe", L"--neural-worker", L"--metadata-handle", L"11",
                  L"--command-handle", L"22", L"--width", L"1920"}).has_value());
    // A probe never goes resident, and a zero handle is not a handle.
    CHECK(!parse({L"NeuralWorker.exe", L"--neural-preflight", L"--metadata-handle", L"11",
                  L"--command-handle", L"22"}).has_value());
    CHECK(!parse({L"NeuralWorker.exe", L"--neural-worker", L"--metadata-handle", L"11",
                  L"--command-handle", L"0"}).has_value());
    // The parent handle is only meaningful to a helper that outlives one job.
    const auto single = neural_worker_detail::BuildWorkerArguments(
        TestRequest(L"single.mkv"), metadata, nullptr, false);
    std::vector<std::wstring_view> withParent{L"NeuralWorker.exe"};
    for (const auto& argument : single) withParent.emplace_back(argument);
    withParent.emplace_back(L"--parent-process");
    withParent.emplace_back(L"44");
    CHECK(!neural_worker_detail::ParseWorkerArguments(withParent).has_value());
    // And the single-shot line the benchmark harness drives is still exactly
    // what it was: no command handle, no residency.
    std::vector<std::wstring_view> plain{L"NeuralWorker.exe"};
    for (const auto& argument : single) plain.emplace_back(argument);
    const auto parsed = neural_worker_detail::ParseWorkerArguments(plain);
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK(!parsed->Resident());
        CHECK(parsed->command == nullptr);
        CHECK(parsed->parentProcess == nullptr);
        CHECK(parsed->request.width == 1920);
    }
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc > 1 && (std::wstring_view(argv[1]) == L"--neural-worker" ||
                     std::wstring_view(argv[1]) == L"--neural-preflight")) return RunFakeWorker(argc, argv);
    if (argc > 1 && std::wstring_view(argv[1]) == L"--real-worker") return RunRealWorker(argc, argv);
    if (argc > 1 && std::wstring_view(argv[1]) == L"--real-preflight") return RunRealPreflight(argc, argv);
    // A parent that dies immediately, for the resident helper's second
    // orphan-safety mechanism. It has to be a real process handle: the wait it
    // exercises is a wait on one.
    if (argc > 1 && std::wstring_view(argv[1]) == L"--exit-now") return 0;
    nonexistent_helper_fails_test();
    helper_main_parser_accepts_normal_and_restarted_contracts_test();
    cancellation_of_running_child_is_bounded_test();
    malformed_or_truncated_results_are_rejected_test();
    valid_result_preserves_all_verification_fields_test();
    request_fields_reach_the_helper_intact_test();
    crashed_helper_is_relaunched_at_most_once_test();
    preflight_receipt_round_trips_and_restarts_once_test();
    preflight_failure_detail_comes_from_the_receipt_diagnosis_test();
    preflight_latch_holds_one_verdict_per_runtime_identity_test();
    runtime_lease_admits_one_holder_per_directory_test();
    protocol_rejects_inconsistent_results_test();
    segment_messages_round_trip_and_reject_malformed_test();
    metadata_reader_accepts_segments_before_the_result_test();
    running_helper_publishes_segments_before_its_result_test();
    configuration_retry_is_sequential_and_bounded_test();
    helper_cold_start_timeline_reaches_the_parent_test();
    cold_start_timeline_messages_are_validated_test();
    resident_loop_answers_hello_and_serves_jobs_test();
    resident_loop_refuses_frames_it_cannot_trust_test();
    resident_loop_exits_on_shutdown_and_on_idle_test();
    resident_loop_cancel_stops_the_job_and_keeps_the_helper_test();
    resident_loop_ends_when_the_parent_dies_test();
    resident_loop_stops_serving_after_an_invalidating_job_test();
    resident_launch_line_is_a_helper_and_not_a_job_test();
    return test_support::failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
