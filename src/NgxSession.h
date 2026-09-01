#pragma once

#include <cstddef>
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

} // namespace ngx_session_detail
