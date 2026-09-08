#include "NeuralWorker.h"
#include "NeuralWorkerProtocol.h"
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


int RunFakeWorker(int argc, wchar_t** argv)
{
    std::vector<std::wstring_view> values;
    for (int index = 0; index < argc; ++index) values.emplace_back(argv[index]);
    const auto parsed = neural_worker_detail::ParseWorkerArguments(values);
    if (!parsed) return 9;
    const HANDLE handle = parsed->metadata;
    if (parsed->preflight) {
        PreflightPayload payload;
        payload.ok = !parsed->configurationRestarted;
        payload.json = parsed->configurationRestarted ? "{\"ok\":false,\"restarted\":true}" : "{\"ok\":true,\"gpu\":\"fake\"}";
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
    result.firstTimestamp100ns = parsed->request.range.start100ns;
    result.timing = {60, 1.5, 2.5, 4.0, 0.7, 0.3, 2048};
    result.detail = L"validated fake helper result";
    if (source == L"echo-request-source.mkv") {
        // Encode the parsed request in the detail so the round trip is provable.
        const std::string guides = CanonicalGuideControls(parsed->request.guides);
        result.detail = L"guides=" + std::wstring(guides.begin(), guides.end()) +
                        L" preroll=" + std::to_wstring(parsed->request.prerollFrames) +
                        L" retry=" + std::to_wstring(parsed->request.frameRetryLimit) +
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
    if (argc != 9 && argc != 12) {
        std::wcerr << L"Usage: NeuralWorkerTests --real-worker <workerexe> <sourcevideo> <outputvideo> <width> <height> <fps> <seconds> [rangeStartSec rangeEndSec mv=1,depth=1,mask=1]\n";
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
    if (argc == 12) {
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
    NeuralRenderPhase lastPhase = NeuralRenderPhase::CheckingCache;
    uint64_t lastReportedFrames = 0;
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
        });
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
    request.guides = {true, false, true};
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
    }
    const auto restarted = neural_worker_detail::BuildWorkerArguments(request, metadata, nullptr, true);
    const auto restartedView = view(restarted);
    const auto parsedRestarted = neural_worker_detail::ParseWorkerArguments(restartedView);
    CHECK(parsedRestarted.has_value());
    if (parsedRestarted) {
        CHECK(parsedRestarted->configurationRestarted);
        CHECK(parsedRestarted->request.pauseEvent == nullptr);
    }
    auto malformed = restarted;
    malformed.back() = L"--unexpected";
    const auto malformedView = view(malformed);
    CHECK(!neural_worker_detail::ParseWorkerArguments(malformedView).has_value());
    auto duplicated = normal;
    duplicated.emplace_back(L"--width");
    duplicated.emplace_back(L"1920");
    const auto duplicatedView = view(duplicated);
    CHECK(!neural_worker_detail::ParseWorkerArguments(duplicatedView).has_value());
    auto badGuides = normal;
    for (size_t index = 0; index + 1 < badGuides.size(); ++index) {
        if (badGuides[index] == L"--guides") badGuides[index + 1] = L"mv=2,depth=1,mask=1";
    }
    const auto badGuidesView = view(badGuides);
    CHECK(!neural_worker_detail::ParseWorkerArguments(badGuidesView).has_value());
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
    request.guides = {false, true, false};
    request.prerollFrames = 7;
    request.frameRetryLimit = 2;
    request.range = {30'000'000, 50'000'000};
    HANDLE pause = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    request.pauseEvent = pause;
    const NeuralRenderResult result = RunNeuralWorker(CurrentExecutable(), request);
    CloseHandle(pause);
    CHECK(result.ok);
    if (!result.ok) std::wcerr << L"detail: " << result.detail << L" failure=" << static_cast<int>(result.failure) << L'\n';
    CHECK(result.detail == L"guides=mv=0,depth=1,mask=0 preroll=7 retry=2 range=30000000-50000000 pause=1");
}

void crashed_helper_is_relaunched_at_most_once_test()
{
    std::vector<NeuralRenderProgress> progress;
    const NeuralRenderResult crashed = RunNeuralWorker(CurrentExecutable(), TestRequest(L"crash-source.mkv"),
        [&](const NeuralRenderProgress& update) { progress.push_back(update); });
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

    const NeuralRenderResult removed = RunNeuralWorker(CurrentExecutable(),
        TestRequest(L"device-removed-source.mkv"), {}, {}, 0);
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
    NeuralRenderProgress recoveringWithoutKind;
    recoveringWithoutKind.phase = NeuralRenderPhase::Recovering;
    const WireProgress wire = EncodeProgress(recoveringWithoutKind);
    CHECK(!DecodeProgress(std::span<const std::byte>(reinterpret_cast<const std::byte*>(&wire), sizeof(wire))).has_value());
}

void configuration_retry_is_sequential_and_bounded_test()
{
    size_t progressCount = 0;
    const auto configured = RunNeuralWorker(CurrentExecutable(), TestRequest(L"configure-once-source.mkv"),
        [&](const NeuralRenderProgress&) { ++progressCount; });
    CHECK(configured.ok);
    CHECK(configured.frameCount == 60);
    CHECK(progressCount == 1);
    const auto repeated = RunNeuralWorker(CurrentExecutable(), TestRequest(L"configure-always-source.mkv"));
    CHECK(!repeated.ok);
    CHECK(!repeated.detail.empty());
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc > 1 && (std::wstring_view(argv[1]) == L"--neural-worker" ||
                     std::wstring_view(argv[1]) == L"--neural-preflight")) return RunFakeWorker(argc, argv);
    if (argc > 1 && std::wstring_view(argv[1]) == L"--real-worker") return RunRealWorker(argc, argv);
    if (argc > 1 && std::wstring_view(argv[1]) == L"--real-preflight") return RunRealPreflight(argc, argv);
    nonexistent_helper_fails_test();
    helper_main_parser_accepts_normal_and_restarted_contracts_test();
    cancellation_of_running_child_is_bounded_test();
    malformed_or_truncated_results_are_rejected_test();
    valid_result_preserves_all_verification_fields_test();
    request_fields_reach_the_helper_intact_test();
    crashed_helper_is_relaunched_at_most_once_test();
    preflight_receipt_round_trips_and_restarts_once_test();
    protocol_rejects_inconsistent_results_test();
    configuration_retry_is_sequential_and_bounded_test();
    return test_support::failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
