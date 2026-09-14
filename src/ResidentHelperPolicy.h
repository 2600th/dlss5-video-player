#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Whether the next neural job can be handed to the helper process that is
// already running, and what to do when it cannot. Kept out of the launcher so
// the decision can be exercised without a process, a pipe or a GPU, the same
// way LiveSessionPolicy keeps an active session's decisions out of the player.
//
// A resident helper is a loaded ReShade proxy, a loaded neural add-on, an
// initialized D3D12 device and a created feature 18. Two of those cost 1.415 s
// (neuralInit) and 0.689 s (featureArm) per render on an RTX 4080 SUPER, which
// is 2.10 s of the 4.88-5.16 s a warm toggle took when every job started its
// own process. Keeping the process is keeping those 2.10 s; everything here
// exists to decide when keeping it would be wrong.
namespace resident_helper {

// Everything a running helper has already committed to and cannot be told to
// change. The directory and its digest are the files it loaded; the settings
// digest is the ReShade.ini it read - ReShade and RenoDX read theirs when the
// proxy loads, so a settings change is a new process and not a new job, however
// cheap re-reading would look from here.
struct HelperKey {
    std::wstring runtimeDirectory;
    std::string runtimeDigest;
    std::string settingsDigest;

    bool operator==(const HelperKey&) const = default;
    // A key missing any part identifies nothing: two jobs whose settings differ
    // would compare equal, and the second would reuse a helper still holding
    // the first one's INI while its cache entry claimed the new settings.
    bool Complete() const
    {
        return !runtimeDirectory.empty() && !runtimeDigest.empty() && !settingsDigest.empty();
    }
};

// Keys compare exactly, so the one part of the key that is a path is folded
// here instead of at every comparison: the same directory spelled two ways must
// not cost a relaunch. Matches how NeuralRuntimeLease derives its mutex name
// from the same directory - one runtime, one identity, whoever spells it.
inline HelperKey MakeHelperKey(std::wstring_view runtimeDirectory, std::string runtimeDigest,
                               std::string settingsDigest)
{
    std::wstring directory(runtimeDirectory);
    for (wchar_t& character : directory) {
        if (character == L'/') character = L'\\';
        if (character >= L'A' && character <= L'Z') character = wchar_t(character - L'A' + L'a');
    }
    while (!directory.empty() && directory.back() == L'\\') directory.pop_back();
    return HelperKey{std::move(directory), std::move(runtimeDigest), std::move(settingsDigest)};
}

// What a resident helper does with its feature-18 workset between jobs.
//
// DLSS does not give feature memory back on an ordinary ReleaseFeature, so a
// parked helper keeps holding it; the documented escape is to ask for it with
// NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, which trades the re-allocation
// back onto the next job. Both arms are real, so both are implemented and the
// choice is made once, at launch: switching mid-life would leave the two VRAM
// samples describing different processes and neither number would mean
// anything.
//
// Deliberately NOT part of HelperKey and deliberately not in the neural
// settings the cache key hashes. It changes when memory is handed back, never
// what the pass computes, so a render made under either arm is the same render
// and must stay reusable across a change of it.
enum class IdleVramPolicy : uint8_t {
    KeepFeature,  // A: hold the workset, so the next job pays neither init nor arm
    FreeFeature,  // B: hand it back while idle, so the next job re-arms
};

// A, because reuse latency is the whole point of keeping the process: the arm
// this policy would give back is 0.689 s of the 2.10 s residency saves.
inline constexpr IdleVramPolicy kDefaultIdleVramPolicy = IdleVramPolicy::KeepFeature;

constexpr std::wstring_view IdleVramPolicyName(IdleVramPolicy policy) noexcept
{
    switch (policy) {
        case IdleVramPolicy::KeepFeature: return L"keep";
        case IdleVramPolicy::FreeFeature: return L"free";
    }
    return L"unknown";
}

// Exactly the two names above, nothing else. An unreadable value is refused
// rather than defaulted: a run whose policy nobody can name produces VRAM
// numbers nobody can attribute.
constexpr std::optional<IdleVramPolicy> ParseIdleVramPolicy(std::wstring_view name) noexcept
{
    if (name == L"keep") return IdleVramPolicy::KeepFeature;
    if (name == L"free") return IdleVramPolicy::FreeFeature;
    return std::nullopt;
}

enum class HelperPlan {
    Reuse,      // hand the job to the running helper over its command channel
    Relaunch,   // end the running helper first: it holds the device this job's would need
    Launch,     // nothing is running; start a helper and keep it for the next job
    SingleShot, // one process, one job, exit: the path that predates residency
};

constexpr std::string_view HelperPlanName(HelperPlan plan) noexcept
{
    switch (plan) {
        case HelperPlan::Reuse: return "reuse";
        case HelperPlan::Relaunch: return "relaunch";
        case HelperPlan::Launch: return "launch";
        case HelperPlan::SingleShot: return "single-shot";
    }
    return "unknown";
}

// What the player is holding when a job arrives. `running` is false both for a
// helper that was never started and for one that is gone: a resident helper
// exits by itself after 30 s idle, and the player finds that out by looking at
// the process, so "gone" is an ordinary Launch rather than a failure to report.
struct ResidentState {
    bool running = false;
    HelperKey key;
};

// The whole decision. Reuse and Relaunch are not two shades of the same answer:
// Reuse skips neuralInit and featureArm entirely, while Relaunch must end the
// old process before starting the new one, because both would otherwise hold
// the same adapter and the same runtime directory at once.
inline HelperPlan PlanForJob(const ResidentState& resident, const HelperKey& job)
{
    // An unidentified job may never be given a helper that outlives it: with
    // nothing to compare, every later job would reuse this one's process.
    if (!job.Complete()) return HelperPlan::SingleShot;
    if (!resident.running) return HelperPlan::Launch;
    return resident.key == job ? HelperPlan::Reuse : HelperPlan::Relaunch;
}

} // namespace resident_helper
