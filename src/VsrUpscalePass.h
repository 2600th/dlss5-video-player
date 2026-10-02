#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

#include "MediaPipeline.h"
#include "NeuralRenderTypes.h"
#include "VsrPolicy.h"

// RTX Video Super Resolution as an export's upscale (ExportPlan::vsrStage). The
// same path playback upscaling draws and docs/measurements/vsr-quality-20261002
// scored: the decoded frame through the player's renderer in a window of the
// output's size, VSR at the size it is shown, the composed view read back and
// encoded. It runs in the player process, where nvngx_vsr.dll ships and the NGX
// session it joins is DLSSBackend's - not in the helper, whose directory the
// runtime lock keeps to the files it names. It writes video only, as every pass
// does; the export's last step attaches the source's audio, subtitles and
// chapters (MuxStageExport).
struct VsrUpscaleRequest {
    std::filesystem::path source;
    std::filesystem::path output;
    uint32_t outputWidth{}, outputHeight{};
    vsr_policy::Quality quality{vsr_policy::kDefaultQuality};
    // Whole, or the marked range: frames whose timestamp falls inside it.
    NeuralRenderRange range{};
    uint32_t nvencPreset{5};
    EncoderQuality encode{EncoderQuality::Standard};
};

struct VsrUpscaleProgress {
    uint64_t framesWritten{};
    uint64_t framesTotal{};  // 0 when the length is unknown
};

struct VsrUpscaleResult {
    bool ok{};
    bool cancelled{};
    std::wstring detail;
    uint64_t framesWritten{};
    uint64_t evaluations{};
};

VsrUpscaleResult RunVsrUpscalePass(const std::filesystem::path& helperDirectory, const VsrUpscaleRequest& request,
                                   std::stop_token stop, const std::function<void(const VsrUpscaleProgress&)>& progress = {});
