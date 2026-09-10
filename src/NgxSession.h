#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace ngx_session_detail {

// NGX initialization is shared by overlapping renderers which use the same
// canonical D3D12 device. Feature handles and parameter blocks remain per backend.
class Registry {
public:
    template <typename Initialize>
    bool Acquire(const void* device, Initialize&& initialize)
    {
        if (!device) return false;
        std::scoped_lock lock(m_mutex);
        auto active = m_leases.find(device);
        if (active != m_leases.end()) {
            ++active->second;
            return true;
        }
        if (!initialize()) return false;
        m_leases.emplace(device, 1);
        return true;
    }

    template <typename Shutdown>
    void Release(const void* device, Shutdown&& shutdown)
    {
        if (!device) return;
        std::scoped_lock lock(m_mutex);
        auto active = m_leases.find(device);
        if (active == m_leases.end()) return;
        if (--active->second != 0) return;
        shutdown();
        m_leases.erase(active);
    }

    size_t LeaseCount(const void* device) const
    {
        std::scoped_lock lock(m_mutex);
        const auto active = m_leases.find(device);
        return active == m_leases.end() ? 0 : active->second;
    }

private:
    mutable std::mutex m_mutex;
    std::unordered_map<const void*, size_t> m_leases;
};

inline Registry& ProcessRegistry()
{
    static Registry registry;
    return registry;
}

class FeatureCreateGate {
public:
    bool ShouldAttempt() const { return !m_failed; }
    void RecordFailure() { m_failed = true; }
    void Reset() { m_failed = false; }

private:
    bool m_failed = false;
};

struct FeatureSetupResult {
    bool selected = false;
    bool needsFlush = false;
};

// The frame count the feature lifetime is keyed on, named here so the renderer
// and the receipt gate that reads it cannot drift apart. There is deliberately
// no second, automatic count that releases a live feature later in a job:
// NVIDIA's DLSS Programming Guide 310.6.0 restricts recreation to display
// resolution, RTX and buffer-format changes (S3.2 step 6) and requires that
// every command list which referenced the feature in Evaluate has retired
// before ReleaseFeature (S5.5). A release mid-job also makes the RenoDX add-on
// tear down the inline neural worksets it had already armed, which wedged the
// pass on an RTX 4070 Ti and an RTX PRO 6000 while it survived on an RTX 4080
// SUPER. Both sibling projects that drive the same add-on reached the same
// place: the warm-up re-create is a workaround for older builds that latch
// STANDBY on a create they missed, it is one-shot, and it is disabled outright
// for the v4.5+ builds that rescan every present and adopt features lazily.
// So a re-create here is only ever an explicit request, made by a caller that
// has evidence the add-on missed the first create, and never on a timer.
inline constexpr uint64_t FeatureCreateFrame = 2;

template <typename EnsureFeature, typename RecreateFeature>
FeatureSetupResult PrepareFeatureForFrame(
    bool enabled,
    bool featureCreated,
    uint64_t framesPresented,
    bool& recreateRequested,
    EnsureFeature&& ensureFeature,
    RecreateFeature&& recreateFeature,
    bool immediateCreate = false)
{
    if (!enabled) return {};

    // A user-requested re-hook must reset a failed automatic-create gate before
    // the ordinary missing-feature path gets another chance to observe it.
    if (recreateRequested) {
        const bool needsFlush = recreateFeature();
        recreateRequested = false;
        return {true, needsFlush};
    }
    if (!featureCreated && (immediateCreate || framesPresented >= FeatureCreateFrame)) {
        return {true, ensureFeature()};
    }
    return {};
}

} // namespace ngx_session_detail
