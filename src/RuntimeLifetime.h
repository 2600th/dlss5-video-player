#pragma once

#include <utility>

// Own the player entirely inside runPlayer so every Media Foundation object is
// destroyed before MFShutdown unloads source-reader and codec modules.
template <typename RunPlayer, typename ShutdownMediaFoundation, typename UninitializeCom>
decltype(auto) RunPlayerRuntime(
    RunPlayer&& runPlayer,
    ShutdownMediaFoundation&& shutdownMediaFoundation,
    UninitializeCom&& uninitializeCom)
{
    decltype(auto) result = std::forward<RunPlayer>(runPlayer)();
    std::forward<ShutdownMediaFoundation>(shutdownMediaFoundation)();
    std::forward<UninitializeCom>(uninitializeCom)();
    return result;
}

