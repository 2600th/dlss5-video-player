#pragma once

#include <type_traits>
#include <utility>

template <typename ShutdownMediaFoundation, typename UninitializeCom>
class RuntimeCleanup final {
public:
    RuntimeCleanup(ShutdownMediaFoundation& shutdownMediaFoundation,
                   UninitializeCom& uninitializeCom) noexcept
        : m_shutdownMediaFoundation(shutdownMediaFoundation),
          m_uninitializeCom(uninitializeCom)
    {
    }

    ~RuntimeCleanup() noexcept
    {
        m_shutdownMediaFoundation();
        m_uninitializeCom();
    }

    RuntimeCleanup(const RuntimeCleanup&) = delete;
    RuntimeCleanup& operator=(const RuntimeCleanup&) = delete;

private:
    ShutdownMediaFoundation& m_shutdownMediaFoundation;
    UninitializeCom& m_uninitializeCom;
};

// Own the player entirely inside runPlayer so every Media Foundation object is
// destroyed before MFShutdown unloads source-reader and codec modules.
template <typename RunPlayer, typename ShutdownMediaFoundation, typename UninitializeCom>
decltype(auto) RunPlayerRuntime(
    RunPlayer&& runPlayer,
    ShutdownMediaFoundation&& shutdownMediaFoundation,
    UninitializeCom&& uninitializeCom)
{
    RuntimeCleanup<std::remove_reference_t<ShutdownMediaFoundation>,
                   std::remove_reference_t<UninitializeCom>> cleanup{
        shutdownMediaFoundation, uninitializeCom};
    return std::forward<RunPlayer>(runPlayer)();
}
