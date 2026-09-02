#pragma once

#include "OfflineNeuralRenderer.h"

#include <filesystem>
#include <span>
#include <stop_token>
#include <string_view>

// Starts the isolated neural-cache helper and accepts only a complete, validated
// helper result. The caller retains ownership of cache staging/promotion.
NeuralRenderResult RunNeuralWorker(
    const std::filesystem::path& executable,
    const NeuralRenderRequest& request,
    OfflineNeuralRenderer::ProgressCallback progress = {},
    std::stop_token stop = {});

namespace neural_worker_detail {

// The helper exits completely after updating the proxy's startup settings.
// Only its hook-free parent may launch the replacement, at most once.
inline constexpr unsigned long kConfigurationChangedExitCode = 75;

// Kept shared with NeuralWorkerMain so focused tests cover the exact argument
// envelope accepted by the executable before it parses individual values.
bool HasValidWorkerArgumentShape(std::span<const std::wstring_view> arguments);

} // namespace neural_worker_detail
