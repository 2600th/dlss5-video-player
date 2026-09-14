#pragma once

#include "NeuralPreflight.h"
#include "OfflineNeuralRenderer.h"

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// Starts the isolated neural-cache helper and accepts only a complete, validated
// helper result. The caller retains ownership of cache staging/promotion.
// Worker crashes and device removal are retried from frame zero at most
// `crashRelaunchLimit` times; exhaustion reports NeuralRenderFailure::RetryExhausted.
// `segments` receives the files a segmented job finalizes while it renders:
// onSegment for each finalized file, onRestart whenever the sequence begins
// again from index zero and every earlier file must be discarded.
// `processCreated` fires once per launch, as soon as the helper's process
// exists; a repair or crash relaunch fires it again, so the cold-start Launch
// phase ends at the process that actually reported and a discarded first
// process counts as launch cost instead of vanishing. The helper's own share of
// the cold-start timeline arrives on the returned result, whether that result
// is the helper's or one this launcher synthesized for a crash or a cancel.
// `helperTimeline` reports that same share the moment it arrives on the pipe,
// which is when the helper's first output file exists - seconds before an
// active session's playback attaches to it, and seconds before this function
// returns. A caller that only reports at the end of the job can ignore it; one
// that reports when the first frame reaches the screen cannot, because the
// returned result comes far too late for it. It runs on the calling thread,
// from inside the metadata decode, exactly like `segments.onSegment`.
inline constexpr uint32_t kDefaultCrashRelaunchLimit = 1;
NeuralRenderResult RunNeuralWorker(
    const std::filesystem::path& executable,
    const NeuralRenderRequest& request,
    OfflineNeuralRenderer::ProgressCallback progress = {},
    std::stop_token stop = {},
    const NeuralSegmentSink& segments = {},
    uint32_t crashRelaunchLimit = kDefaultCrashRelaunchLimit,
    const std::function<void()>& processCreated = {},
    const NeuralColdStartCallback& helperTimeline = {});

// Short Feature-18 probe run in the same isolated helper before a render. The
// JSON receipt names GPU, driver, runtime/consumer versions and every feature
// creation result observed; `ok` is false when the probe could not arm the
// neural contract. Never renders or touches the cache.
struct NeuralPreflightResult {
    bool ok{};
    bool cancelled{};
    std::string json;
    std::wstring detail;
    // Classified reason the probe failed, scanned back out of the receipt so
    // the caller can branch (offer a driver update, stop offering a retry).
    NeuralPreflightCause cause{NeuralPreflightCause::None};
};
NeuralPreflightResult RunNeuralPreflight(
    const std::filesystem::path& executable,
    std::stop_token stop = {});

// Identity a preflight verdict belongs to. Anything that can change the
// answer - the adapter, its driver, the runtime files - is in the key.
struct NeuralPreflightKey {
    std::wstring gpu;
    std::wstring driver;
    std::string runtimeDigest;
    bool operator==(const NeuralPreflightKey&) const = default;
};

// One remembered verdict, shared by the UI thread and job threads.
// A doomed preflight costs ~5 s and ends the running session: the field log
// shows three of them in 65 s, one per playback start, each dropping frames.
// A passing one costs the same ~5 s - a whole helper process that loads
// ReShade, the add-on and NGX, creates feature 18, and exits - and only the
// failures were remembered, so every session after the first paid again for an
// answer already in hand. The verdict stands until the identity changes or the
// runtime is replaced.
class NeuralPreflightLatch {
public:
    void RecordFailure(const NeuralPreflightKey& key, std::wstring detail);
    void RecordSuccess(const NeuralPreflightKey& key, std::string json);
    // Empty unless this exact key is latched as failed.
    std::wstring LatchedFailureDetail(const NeuralPreflightKey& key) const;
    // The receipt of a probe that already passed for this exact key, so the
    // render's receipt still carries the evidence the probe produced. Empty
    // when this key has not passed.
    std::string LatchedSuccessJson(const NeuralPreflightKey& key) const;
    void Invalidate();

private:
    mutable std::mutex mutex_;
    NeuralPreflightKey key_;
    std::wstring detail_;
    std::string json_;
    bool failed_{};
    bool passed_{};
};

// A passing verdict also survives the process, because the probe's answer is a
// property of the machine, not of the run: same adapter, same driver, same
// twelve runtime files means the same answer. The store lives beside the render
// cache and is keyed by that identity; a render whose runtime was tampered with
// behind the key still fails on its own armed-evidence check, so the worst a
// stale entry costs is a less specific message.
std::filesystem::path NeuralPreflightReceiptPath(const std::filesystem::path& cacheRoot,
                                                 const NeuralPreflightKey& key);
std::string LoadNeuralPreflightReceipt(const std::filesystem::path& cacheRoot,
                                       const NeuralPreflightKey& key);
bool StoreNeuralPreflightReceipt(const std::filesystem::path& cacheRoot,
                                 const NeuralPreflightKey& key, std::string_view json);
// Stamps a receipt that was reused rather than produced by this session, so the
// render receipt does not claim a probe it never ran.
std::string MarkReusedNeuralPreflight(std::string_view json);

// Serializes every use of the shared neural-runtime directory. The parent
// rewrites ReShade.ini with the render's neural settings per job and the
// helper's proxy owns ReShade.log, so two concurrent renders would swap each
// other's settings and evidence under cache keys that claim otherwise. Hold
// this from the settings write until the helper has exited.
class NeuralRuntimeLease {
public:
    explicit NeuralRuntimeLease(const std::filesystem::path& runtimeDirectory,
                                std::chrono::milliseconds wait = std::chrono::seconds{5});
    ~NeuralRuntimeLease();
    NeuralRuntimeLease(const NeuralRuntimeLease&) = delete;
    NeuralRuntimeLease& operator=(const NeuralRuntimeLease&) = delete;

    bool Held() const noexcept { return held_; }
    // Session-scoped name derived from the directory; stable across processes.
    static std::wstring MutexName(const std::filesystem::path& runtimeDirectory);

private:
    void* mutex_{};
    bool held_{};
};

namespace neural_worker_detail {

// The helper exits completely after updating the proxy's startup settings.
// Only its hook-free parent may launch the replacement, at most once.
inline constexpr unsigned long kConfigurationChangedExitCode = 75;

struct WorkerArguments {
    HANDLE metadata{};
    bool preflight{};
    bool configurationRestarted{};
    NeuralRenderRequest request;
};

// Exact argument envelope accepted by the helper executable. Shared with
// NeuralWorkerMain so the launcher and the parser cannot drift apart.
std::vector<std::wstring> BuildWorkerArguments(const NeuralRenderRequest& request, HANDLE metadata,
                                               HANDLE pauseEvent, bool configurationRestarted);
std::vector<std::wstring> BuildPreflightArguments(HANDLE metadata, bool configurationRestarted);
std::optional<WorkerArguments> ParseWorkerArguments(std::span<const std::wstring_view> arguments);

// Outcome of running one metadata byte stream through the parent's decoder.
// Exposed so the accept/reject rules can be exercised without a live helper:
// the pipe reader inside RunNeuralWorker is this same decoder fed from a pipe.
struct MetadataStreamOutcome {
    bool malformed{};
    bool complete{};                    // a valid result terminated the stream
    size_t progressUpdates{};
    size_t restarts{};                  // sequences that began again at index 0
    std::vector<NeuralRenderSegment> segments;
    NeuralColdStartTimeline timeline;  // empty unless the helper reported one
};
MetadataStreamOutcome DecodeMetadataStream(std::span<const std::byte> bytes);

} // namespace neural_worker_detail
