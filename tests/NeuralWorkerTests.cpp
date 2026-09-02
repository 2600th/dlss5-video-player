#include "NeuralWorker.h"
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

constexpr uint32_t kProtocolMagic = 0x3152574Eu; // NWR1
constexpr uint16_t kProtocolVersion = 1;

enum class WireKind : uint16_t { Progress = 1, Result = 2 };

#pragma pack(push, 1)
struct WireHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t payloadBytes;
};

struct WireProgress {
    uint32_t phase;
    uint64_t completedFrames;
    uint64_t totalFrames;
    uint64_t bytes;
    int64_t elapsedMilliseconds;
    int64_t estimatedRemainingMilliseconds;
};

struct WireResult {
    uint8_t ok;
    uint8_t cancelled;
    uint8_t encoder;
    uint8_t feature18ArmedBeforeCapture;
    uint8_t upscalingOff;
    uint8_t inlineInterceptionContract;
    uint8_t feature18Created;
    uint8_t feature18Evaluated;
    uint8_t laterFailure;
    uint8_t reserved[7];
    uint64_t frameCount;
    int64_t duration100ns;
    uint64_t nativeEvaluations;
    uint64_t verifiedNeuralFrames;
    uint64_t highestObservedEvaluation;
    uint32_t detailBytes;
};
#pragma pack(pop)

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

bool ParseUnsigned(std::wstring_view value, uintptr_t& result)
{
    if (value.empty()) return false;
    result = 0;
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9') return false;
        const uintptr_t next = result * 10 + static_cast<uintptr_t>(character - L'0');
        if (next < result) return false;
        result = next;
    }
    return true;
}

bool WriteAll(HANDLE handle, const void* data, size_t bytes)
{
    const auto* cursor = static_cast<const std::byte*>(data);
    while (bytes != 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes, MAXDWORD));
        DWORD written = 0;
        if (!WriteFile(handle, cursor, chunk, &written, nullptr) || written == 0) return false;
        cursor += written;
        bytes -= written;
    }
    return true;
}

bool WriteMessage(HANDLE handle, WireKind kind, const void* payload, uint32_t payloadBytes)
{
    const WireHeader header{kProtocolMagic, kProtocolVersion, static_cast<uint16_t>(kind), payloadBytes};
    return WriteAll(handle, &header, sizeof(header)) &&
        (!payloadBytes || WriteAll(handle, payload, payloadBytes));
}

int RunFakeWorker(int argc, wchar_t** argv)
{
    if ((argc != 16 && argc != 17) || std::wstring_view(argv[1]) != L"--neural-worker") return 9;
    std::wstring source;
    uintptr_t rawHandle = 0;
    for (int index = 2; index + 1 < argc; index += 2) {
        const std::wstring_view key(argv[index]);
        const std::wstring_view value(argv[index + 1]);
        if (key == L"--metadata-handle") {
            if (!ParseUnsigned(value, rawHandle)) return 10;
        } else if (key == L"--source") {
            source.assign(value);
        }
    }
    if (!rawHandle) return 11;
    const HANDLE handle = reinterpret_cast<HANDLE>(rawHandle);
    if (source == L"configure-once-source.mkv" && argc == 16) return 75;
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

    const WireProgress progress{static_cast<uint32_t>(NeuralRenderPhase::NeuralRendering),
        17, 60, 4096, 125, 200};
    if (!WriteMessage(handle, WireKind::Progress, &progress, sizeof(progress))) return 13;
    const std::wstring detail = L"validated fake helper result";
    WireResult result{};
    result.ok = 1;
    result.encoder = static_cast<uint8_t>(EncoderKind::H264Software);
    result.feature18ArmedBeforeCapture = 1;
    result.upscalingOff = 1;
    result.inlineInterceptionContract = 1;
    result.feature18Created = 1;
    result.feature18Evaluated = 1;
    result.frameCount = 60;
    result.duration100ns = 10'000'000;
    result.nativeEvaluations = 60;
    result.verifiedNeuralFrames = 60;
    result.highestObservedEvaluation = 75;
    result.detailBytes = static_cast<uint32_t>(detail.size() * sizeof(wchar_t));
    std::vector<std::byte> payload(sizeof(result) + result.detailBytes);
    std::memcpy(payload.data(), &result, sizeof(result));
    std::memcpy(payload.data() + sizeof(result), detail.data(), result.detailBytes);
    if (!WriteMessage(handle, WireKind::Result, payload.data(), static_cast<uint32_t>(payload.size()))) return 14;
    return 0;
}

bool ParseUnsigned32(std::wstring_view text, uint32_t& value)
{
    uintptr_t parsed = 0;
    if (!ParseUnsigned(text, parsed) || parsed > UINT32_MAX) return false;
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

int RunRealWorker(int argc, wchar_t** argv)
{
    if (argc != 9) {
        std::wcerr << L"Usage: NeuralWorkerTests --real-worker <workerexe> <sourcevideo> <outputvideo> <width> <height> <fps> <seconds>\n";
        return EXIT_FAILURE;
    }
    NeuralRenderRequest request;
    request.sourcePath = argv[3];
    request.stagingVideoPath = argv[4];
    if (!ParseUnsigned32(argv[5], request.width) || !ParseUnsigned32(argv[6], request.height) ||
        !request.width || !request.height || !ParsePositiveDouble(argv[7], request.fps) ||
        !ParsePositiveDouble(argv[8], request.durationSeconds)) {
        std::wcerr << L"Invalid --real-worker dimensions, FPS, or duration.\n";
        return EXIT_FAILURE;
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
        << L", highest=" << result.evidence.highestObservedEvaluation << L"} detail=" << result.detail << L'\n';
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
    constexpr std::array<std::wstring_view, 16> normal{
        L"NeuralWorker.exe", L"--neural-worker", L"--metadata-handle", L"123", L"--source", L"source.mkv",
        L"--staging", L"staging.mkv", L"--width", L"1920", L"--height", L"1080", L"--fps", L"60",
        L"--duration-100ns", L"10000000"};
    CHECK(neural_worker_detail::HasValidWorkerArgumentShape(normal));
    constexpr std::array<std::wstring_view, 17> restarted{
        L"NeuralWorker.exe", L"--neural-worker", L"--metadata-handle", L"123", L"--source", L"source.mkv",
        L"--staging", L"staging.mkv", L"--width", L"1920", L"--height", L"1080", L"--fps", L"60",
        L"--duration-100ns", L"10000000", L"--configuration-restarted"};
    CHECK(neural_worker_detail::HasValidWorkerArgumentShape(restarted));
    auto malformed = restarted;
    malformed[16] = L"--unexpected";
    CHECK(!neural_worker_detail::HasValidWorkerArgumentShape(malformed));
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
    const NeuralRenderResult result = RunNeuralWorker(CurrentExecutable(), TestRequest(L"valid-source.mkv"),
        [&](const NeuralRenderProgress& progress) { observed = progress; ++progressCount; });
    CHECK(result.ok);
    CHECK(!result.cancelled);
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
    CHECK(progressCount == 1);
    CHECK(observed.phase == NeuralRenderPhase::NeuralRendering);
    CHECK(observed.completedFrames == 17);
    CHECK(observed.totalFrames == 60);
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
    if (argc > 1 && std::wstring_view(argv[1]) == L"--neural-worker") return RunFakeWorker(argc, argv);
    if (argc > 1 && std::wstring_view(argv[1]) == L"--real-worker") return RunRealWorker(argc, argv);
    nonexistent_helper_fails_test();
    helper_main_parser_accepts_normal_and_restarted_contracts_test();
    cancellation_of_running_child_is_bounded_test();
    malformed_or_truncated_results_are_rejected_test();
    valid_result_preserves_all_verification_fields_test();
    configuration_retry_is_sequential_and_bounded_test();
    return test_support::failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
