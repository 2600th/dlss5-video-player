#pragma once

#include "NeuralRenderTypes.h"

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Every RTX-branded NVIDIA part has tensor cores and an NGX-capable driver.
// The generation only chooses cache identity, the render-pace prior and the
// receipt label; whether feature 18 actually runs is decided by the runtime's
// own capability check and the strict evidence chain, which fail closed.
enum class GpuGeneration {
    Rtx20Turing,
    Rtx30Ampere,
    Rtx40Ada,
    Rtx50Blackwell,
    // RTX-branded workstation/laptop parts ("RTX A4000", "RTX 6000 Ada
    // Generation", "RTX PRO 6000 Blackwell") outside the GeForce naming.
    OtherRtx,
    OtherNvidia,
    Unsupported,
};

// Stable short label for the cache identity and receipt ("rtx40").
const char* GpuGenerationPathName(GpuGeneration generation) noexcept;

// Scale on the reference render cost (RTX 5090, see PlaybackTiming.h) a fresh
// install assumes for a generation before this machine has measured its own
// pace. 0 means unknown: the live-session forecast then stays silent instead
// of guessing, and the session simply buffers when it cannot keep up.
double RenderPacePrior(GpuGeneration generation) noexcept;

struct DetectedGpu {
    GpuGeneration generation{GpuGeneration::Unsupported};
    std::wstring description;
    uint32_t vendorId{};
    uint32_t deviceId{};
    uint64_t dedicatedVideoMemoryBytes{};
    // User-mode driver version reported by DXGI ("32.0.15.6164"); empty when
    // the adapter does not report one.
    std::wstring driverVersion;
};

// NVIDIA publishes a driver as "566.14"; DXGI reports the user-mode driver as
// four 16-bit parts and hides that number in the last two. Verified against
// three machines this project has run on:
//   32.0.15.6614 -> 566.14  RTX 3060 Laptop, feature 18 refused (0xbad00002)
//   32.0.16.1047 -> 610.47  RTX 4080 SUPER, feature 18 runs
//   32.0.16.1664 -> 616.64  RTX 5090, feature 18 runs
// number = (part3 % 10) * 10000 + part4, then major = number / 100.
struct NvidiaDriverVersion {
    uint32_t major{};
    uint32_t minor{};
    auto operator<=>(const NvidiaDriverVersion&) const = default;
};

// Defined here rather than in RuntimePolicy.cpp because the preflight
// diagnosis needs them in translation units that link no DXGI: unlike the
// rest of this header's implementation they are pure text.
inline std::optional<NvidiaDriverVersion> ParseNvidiaDriverVersion(std::wstring_view dxgiVersion)
{
    uint32_t parts[4]{};
    size_t offset = 0;
    for (size_t index = 0; index < 4; ++index) {
        const size_t dot = dxgiVersion.find(L'.', offset);
        const bool last = index == 3;
        // Exactly four parts: a separator is required before the last one and
        // forbidden after it.
        if (last != (dot == std::wstring_view::npos)) return std::nullopt;
        const std::wstring_view part =
            dxgiVersion.substr(offset, last ? std::wstring_view::npos : dot - offset);
        if (part.empty() || part.size() > 5) return std::nullopt;
        uint32_t value = 0;
        for (const wchar_t character : part) {
            if (character < L'0' || character > L'9') return std::nullopt;
            value = value * 10u + static_cast<uint32_t>(character - L'0');
        }
        parts[index] = value;
        offset = dot + 1;
    }
    if (parts[3] > 9999) return std::nullopt;
    const uint32_t number = (parts[2] % 10u) * 10000u + parts[3];
    const NvidiaDriverVersion version{number / 100u, number % 100u};
    if (version.major == 0) return std::nullopt;
    return version;
}

inline std::wstring FormatNvidiaDriverVersion(NvidiaDriverVersion version)
{
    std::wstring text = std::to_wstring(version.major);
    text += L'.';
    if (version.minor < 10) text += L'0';
    text += std::to_wstring(version.minor);
    return text;
}

// 610.47 is the lowest driver this project has actually run feature 18 on;
// 616.64 is the driver the locked runtime is verified against. The floor is
// deliberately the measured one - the community-published floor for this
// runtime is 616.56, which no machine here has bracketed.
inline constexpr NvidiaDriverVersion kNeuralDriverFloor{610, 47};
inline constexpr NvidiaDriverVersion kNeuralDriverRecommended{616, 64};

enum class NeuralDriverSupport { Unknown, BelowFloor, Supported };

inline NeuralDriverSupport ClassifyNeuralDriver(std::wstring_view dxgiVersion)
{
    const std::optional<NvidiaDriverVersion> version = ParseNvidiaDriverVersion(dxgiVersion);
    if (!version) return NeuralDriverSupport::Unknown;
    return *version < kNeuralDriverFloor ? NeuralDriverSupport::BelowFloor : NeuralDriverSupport::Supported;
}

struct NeuralRenderDefaults {
    uint32_t width{};
    uint32_t height{};
};

struct RuntimeArguments {
    bool ok{false};
    bool safeMode{false};
    std::vector<std::wstring> userArguments;
    std::wstring error;
};

enum class NeuralRuntimeLayout {
    Absent,
    Complete,
    Incomplete,
};

enum class SafeModeRestartOutcome {
    Cancelled,
    LaunchFailed,
    CloseCurrent,
};

enum class NeuralPlaybackState {
    Idle,
    Acquiring,
    Rendering,
    Validating,
    Ready,
    OriginalOnly,
    Cancelling,
    Failed,
    // Render job suspended by the user; resumes to Rendering.
    Paused,
    // Job retrying the same frame / relaunching the worker; resolves to
    // Rendering, Failed, RetryExhausted or Cancelling.
    Recovering,
    // Bounded retries spent. Terminal like Failed.
    RetryExhausted,
};

const wchar_t* NeuralPlaybackStateName(NeuralPlaybackState state) noexcept;

// Lifecycle state a job failure lands in: RetryExhausted stays distinct so
// the UI can say the retries were spent, Cancelled is the user's choice and
// offers the original, every other failure is Failed.
NeuralPlaybackState StateForFailure(NeuralRenderFailure failure) noexcept;

// Lifecycle state a worker progress phase drives the job into. Phases that
// carry no state of their own (CheckingCache, Ready) keep `current`; the
// decode/render/encode phases return to Rendering from Paused or Recovering.
NeuralPlaybackState StateForProgressPhase(NeuralRenderPhase phase, NeuralPlaybackState current) noexcept;

struct NeuralPlaybackLifecycle {
    NeuralPlaybackState state{NeuralPlaybackState::Idle};
    uint64_t generation{};
    uint64_t Begin();
    bool Accept(uint64_t candidateGeneration) const;
    bool Transition(NeuralPlaybackState next);
    void Invalidate();
};

bool CanPublishNeuralCompletion(bool renderOk, bool probeOk, bool manifestValid);

// How far a joined media file's duration may legitimately differ from the
// duration the render itself reported: one frame of playback jitter plus one
// 1 ms muxer rounding per joined file. A live session publishes N separately
// muxed segments concatenated into one cache entry, and Matroska's default
// timecode scale quantises each file's timestamps to 1 ms, so those roundings
// accumulate with the part count while a single-file render carries only one.
int64_t JoinedMediaDurationTolerance100ns(double fps, size_t parts);

// The three pairwise duration agreements the publish gate requires: the
// probed file, the render result and the requested range must all describe
// the same span.
bool NeuralPublishDurationsMatch(
    int64_t probeDuration100ns,
    int64_t resultDuration100ns,
    int64_t expectedDuration100ns,
    int64_t tolerance100ns);

// Whether a render range has nothing renderable left. The render head is an
// integer multiple of a frame duration built from per-frame segment ends,
// while the range end is a probed source duration: a residual shorter than
// one frame is coverage, not work, and handing it to a worker only earns a
// range refusal. An unreadable frame rate falls back to the plain comparison.
bool RenderRangeIsCovered(int64_t renderFrom100ns, int64_t rangeEnd100ns, double fps);

GpuGeneration ClassifyGpu(uint32_t vendorId, std::wstring_view description);
bool NeuralAddonDesired(GpuGeneration gpu, bool safeMode);
NeuralRenderDefaults ResolveNeuralRenderDefaults(
    bool neuralAddonConfigured,
    bool outputExplicit,
    uint32_t requestedWidth,
    uint32_t requestedHeight);
NeuralRuntimeLayout ClassifyNeuralRuntimeLayout(
    bool hasReShadeConfig,
    bool hasReShadeProxy,
    bool hasNeuralAddon,
    bool hasNeuralRuntime);
DetectedGpu DetectHighPerformanceGpu();
RuntimeArguments ParseRuntimeArguments(int argc, const wchar_t* const* argv);
std::vector<std::wstring> BuildSafeModeRestartArguments(
    const std::vector<std::wstring>& userArguments);
SafeModeRestartOutcome ExecuteAdvancedSafeModeRestart(
    bool confirmed,
    const std::vector<std::wstring>& userArguments,
    const std::function<bool(const std::vector<std::wstring>&)>& launch);
std::wstring BuildWindowsCommandLine(
    std::wstring_view executable,
    const std::vector<std::wstring>& arguments);
