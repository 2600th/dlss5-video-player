#pragma once

#include "NeuralPreflight.h"
#include "OfflineNeuralRenderer.h"
#include "ResidentHelperPolicy.h"

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
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

// The callbacks and limits one job hands the launcher. Collected into a struct
// because a resident helper takes the same set as a single-shot one and the
// eighth positional argument had stopped being readable.
struct NeuralJobHooks {
    OfflineNeuralRenderer::ProgressCallback progress;
    NeuralSegmentSink segments;
    // Fires the moment the helper holds this job: the created process for a
    // launch, the written command frame for a reuse. Both end the player's
    // Launch phase at the same boundary - the last thing the parent did before
    // the helper's own clock starts - and the plan names which one it was,
    // early enough for a caller that reports when the first frame reaches the
    // screen rather than when the job ends. A crash relaunch fires it again,
    // reporting Launch for the replacement process.
    std::function<void(resident_helper::HelperPlan)> accepted;
    NeuralColdStartCallback helperTimeline;
    uint32_t crashRelaunchLimit = kDefaultCrashRelaunchLimit;
};

// The advanced setting that selects the idle-VRAM arm, read out of the
// player's own `DLSSVideoPlayer.ini` (not the neural runtime's ReShade.ini -
// putting it there would fold a resource policy into the settings digest the
// render cache key hashes):
//
//   [NeuralHelper]
//   IdleVramPolicy=keep    ; A, the default: hold the feature workset
//   IdleVramPolicy=free    ; B: hand it back while idle, re-arm on the next job
//
// An absent file, an absent key or a name this build does not know all give
// the default. A typo in a settings file is not worth refusing to render over,
// and the arm that actually ran is on the helper's launch line and in every
// receipt it produces, so nothing is attributed to a policy it did not run
// under. `settingsIni` empty also gives the default.
resident_helper::IdleVramPolicy ReadIdleVramPolicy(const std::filesystem::path& settingsIni);
// `DLSSVideoPlayer.ini` beside the running executable, which is where the
// player keeps its own settings.
std::filesystem::path PlayerSettingsPath();

// One neural helper process kept across jobs, so a warm toggle stops paying
// for the NGX initialization and feature creation it already paid for. The
// residency key is `(runtime directory, runtime digest, neural-settings
// digest)`: the first two are the files the helper loaded, the third is the INI
// it read at process start, which is why a settings change relaunches instead
// of being re-read.
//
// The helper exits by itself after 30 s idle, and again after any job it could
// not finish, so its absence is normal and never surfaces as an error: a
// vanished helper is an ordinary Launch. Its process stays assigned to a job
// object holding JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE for its whole life, so it
// cannot outlive this object - or the player, however the player ends.
//
// NOT thread-safe, and does not need to be: the player runs one neural job at
// a time on one job thread, and the UI thread only touches this once that
// thread has been joined.
//
// The idle-VRAM policy every helper this object launches is given is fixed at
// construction, because it is fixed for a helper process: switching it
// mid-session would leave two idle samples describing two different arms and
// neither number would mean anything. It is deliberately absent from the
// residency key - it decides when memory is handed back, never what the pass
// computes, so flipping it must not retire a cache entry or force a relaunch.
class ResidentNeuralHelper {
public:
    ResidentNeuralHelper();
    explicit ResidentNeuralHelper(resident_helper::IdleVramPolicy idleVramPolicy);
    ~ResidentNeuralHelper();
    ResidentNeuralHelper(const ResidentNeuralHelper&) = delete;
    ResidentNeuralHelper& operator=(const ResidentNeuralHelper&) = delete;
    // Runs one job, launching, relaunching or reusing the helper as the policy
    // decides, and reports the decision through `plan` for the caller's log.
    // Everything the single-shot launcher accepts is accepted here, including
    // its bounded crash relaunch and its refusal to splice frames across
    // helper instances.
    NeuralRenderResult RunJob(const std::filesystem::path& executable,
                              const resident_helper::HelperKey& key,
                              const NeuralRenderRequest& request,
                              const NeuralJobHooks& hooks,
                              std::stop_token stop = {},
                              resident_helper::HelperPlan* plan = nullptr);
    // Asks a resident helper to exit, waits briefly for it, then takes the job
    // object down over whatever is left. Idempotent, and safe with no helper.
    void Release();
    // True while a helper process is alive. Looks at the process, so an idle
    // timeout or a self-invalidating exit is observed rather than assumed.
    bool Resident() const;
    // The arm every helper this object launches runs under, for the caller's
    // log. Named for the invariant rather than for the type it returns:
    // exactly one policy is in force per helper process, for this object's
    // whole life, so there is nothing here a caller could set.
    resident_helper::IdleVramPolicy IdleVramPolicyInForce() const noexcept { return idleVramPolicy_; }

private:
    struct Session;
    // One helper instance being given this job: launch or reuse, dispatch,
    // pump, judge. The crash relaunch calls it again, and `accepted` is told
    // whether the instance that took the job was launched or reused.
    NeuralRenderResult RunAttempt(const std::filesystem::path& executable,
                                  const resident_helper::HelperKey& key,
                                  const NeuralRenderRequest& request,
                                  const NeuralJobHooks& hooks, std::stop_token stop,
                                  const std::function<void(bool launched)>& accepted);
    std::unique_ptr<Session> session_;
    resident_helper::IdleVramPolicy idleVramPolicy_{resident_helper::kDefaultIdleVramPolicy};
};

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
// this from the settings write until the job has finished - not until the
// helper has exited, because a resident helper outlives the job on purpose.
// An idle helper writes nothing and reads nothing, so releasing the lease
// between jobs is what keeps a resident helper from locking a second player
// instance out of a runtime it is not using. The helper re-reads the lock and
// the parent rewrites the INI under the lease for every job, resident or not.
class NeuralRuntimeLease {
public:
    explicit NeuralRuntimeLease(const std::filesystem::path& runtimeDirectory,
                                std::chrono::milliseconds wait = std::chrono::seconds{5});
    ~NeuralRuntimeLease();
    NeuralRuntimeLease(const NeuralRuntimeLease&) = delete;
    NeuralRuntimeLease& operator=(const NeuralRuntimeLease&) = delete;

    bool Held() const noexcept { return held_; }
    // Machine-wide name derived from the directory; stable across processes
    // and sessions.
    static std::wstring MutexName(const std::filesystem::path& runtimeDirectory);

private:
    void* mutex_{};
    bool held_{};
};

namespace neural_worker_detail {

// The helper exits completely after updating the proxy's startup settings.
// Only its hook-free parent may launch the replacement, at most once.
inline constexpr unsigned long kConfigurationChangedExitCode = 75;

// The exit code of a helper the parent has seen go, or nothing when there is
// no code to read: `query` is GetExitCodeProcess, which can fail, and which
// answers STILL_ACTIVE for a process that has not actually gone. Neither is an
// exit code. The resident path ignored the call's result and kept its 0, so a
// helper that died that way was judged a clean exit that walked away from its
// job - a Protocol failure, which is never relaunched - instead of a crash.
template <class Query>
std::optional<DWORD> ReadHelperExitCode(Query&& query)
{
    DWORD code = 0;
    if (!query(&code) || code == STILL_ACTIVE) return std::nullopt;
    return code;
}

struct WorkerArguments {
    HANDLE metadata{};
    // Parent to helper. Present only in resident mode, where the helper takes
    // its jobs off this pipe instead of off its own command line; a job's
    // argument vector never carries one, because nested residency would be
    // meaningless. Its presence is what resident mode is.
    HANDLE command{};
    // The parent's own process handle, duplicated with SYNCHRONIZE. A resident
    // helper waits on it and exits when it signals: the kill-on-close job
    // object already covers a parent that dies, and this covers it a second
    // time, because the process this guards is holding the GPU.
    HANDLE parentProcess{};
    bool preflight{};
    bool configurationRestarted{};
    // What this helper does with its feature memory between jobs. Meaningful
    // only in resident mode - a single-shot helper exits instead of idling -
    // and fixed for the process, because two idle samples taken under
    // different policies would describe nothing.
    resident_helper::IdleVramPolicy idleVramPolicy{resident_helper::kDefaultIdleVramPolicy};
    NeuralRenderRequest request;

    // Resident mode is not a flag to keep in step with the handle; it is the
    // handle. `request` is unset here - the first job arrives as a command.
    bool Resident() const noexcept { return command != nullptr; }
};

// Exact argument envelope accepted by the helper executable. Shared with
// NeuralWorkerMain so the launcher and the parser cannot drift apart.
std::vector<std::wstring> BuildWorkerArguments(const NeuralRenderRequest& request, HANDLE metadata,
                                               HANDLE pauseEvent, bool configurationRestarted);
std::vector<std::wstring> BuildPreflightArguments(HANDLE metadata, bool configurationRestarted);
// A resident helper's launch line: handles only, no job. Jobs arrive as command
// frames carrying the vector BuildWorkerArguments produces, so ParseWorkerArguments
// stays the one definition and the one validator of what a job is.
std::vector<std::wstring> BuildResidentArguments(HANDLE metadata, HANDLE command, HANDLE pauseEvent,
                                                 HANDLE parentProcess,
                                                 resident_helper::IdleVramPolicy idleVramPolicy);
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

// How many bytes one pass of the parent's metadata drain will absorb before
// handing control back.
//
// The drain used to run until the pipe was dry, and it is the FIRST statement
// of every pump iteration - ahead of the stop check. A helper that writes
// faster than the parent reads therefore made a render uncancellable: the user
// pressed stop and nothing happened until the helper chose to go quiet. One
// mebibyte is far more than a well-behaved helper emits between iterations, so
// the budget is invisible in normal operation and only bites a runaway. The
// pipe is now read by a thread of its own, which runs at most this far ahead
// of the pump, and each pass takes at most this much of what it queued.
inline constexpr size_t kMetadataDrainByteBudget = 1u << 20;

// What one bounded pass did.
struct MetadataDrainPass {
    bool malformed{};
    // Stopped because the budget ran out rather than because the pipe was
    // empty. The pump checks its stop token and comes straight back.
    bool budgetSpent{};
    size_t bytesRead{};
};

// Drains `pipe` once, exactly as the pump does between stop-token checks:
// through the same reader thread, once it has caught up with what the pipe
// already holds. Exposed for the same reason DecodeMetadataStream is: the
// bound that keeps a cancel responsive is worth asserting against a real pipe
// rather than hoped for. Decoded messages are discarded - this entry point is
// about the bound. The caller keeps `pipe`; a duplicate is read.
MetadataDrainPass DrainMetadataPipeOnce(HANDLE pipe);

// How long the shutdown frame may take before the helper is killed instead of
// asked.
//
// WriteCommand is a synchronous WriteFile on an anonymous pipe. A helper that
// has stopped reading - which is precisely when it needs ending - lets the
// pipe fill at around 16 KiB, and the write then never returns. Running that
// inline ahead of TerminateJobObject hung the shutdown it was there to
// perform.
inline constexpr std::chrono::milliseconds kShutdownWriteBudget{250};

// Sends the shutdown frame and takes ownership of `command`, closing it on
// every path - possibly from the writer thread, if the write outlives the
// budget. Returns true when the helper took the frame.
//
// A false return means the helper is not reading its commands, so the caller
// must kill rather than wait. The kill breaks the pipe, which is what
// releases any writer still blocked here.
bool SendShutdownAndCloseBounded(HANDLE command, std::chrono::milliseconds budget);

} // namespace neural_worker_detail
