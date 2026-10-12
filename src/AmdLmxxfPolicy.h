#pragma once

#include "TemporalStabilityPolicy.h"
#include "Utf8Text.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace amd_lmxxf {

inline constexpr uint32_t kAmdVendorId = 0x1002;
inline constexpr std::wstring_view kRuntimeDirectoryName = L"amd-neural-runtime";
inline constexpr std::wstring_view kRuntimeDllName = L"LmxxfNrRuntime.dll";
inline constexpr std::wstring_view kAssetsDirectoryName = L"native-game-tiled-assets";
inline constexpr std::wstring_view kLockFileName = L"amd-runtime-lock.json";

// The rung an AMD render takes when nothing asked for one.
//
// The runtime relights every frame on its own and this backend has no history
// for it to fuse with - its own temporal model needs guides, and the estimator
// that would produce them is the expensive half this backend does not have - so
// what a viewer sees between frames is the model's own frame-to-frame jitter.
// The host-side accumulation (TemporalStabilityPolicy.h) is what takes that out,
// which makes Off here not a neutral setting but the arm that flickers: measured
// on a still source, frame-to-frame Y PSNR is 49.1 dB at the model's own output
// against the source's own 96.6 dB, and Low takes the added jitter down 28.5%
// for 0.1 dB of distance from the source
// (docs/AMD-TEMPORAL-ACCUMULATION-zh.md).
//
// Low rather than Medium or High because this is a default and not a choice: the
// rungs trade steadiness for detail monotonically, Low is the one that moves the
// picture least, and a viewer who wants more can ask for it.
inline constexpr TemporalStability kAmdDefaultStability = TemporalStability::Low;

enum class GpuFamily
{
    Unsupported,
    Rdna4,
};

enum class KernelTarget
{
    Unsupported,
    Gfx1200,
    Gfx1201,
};

constexpr std::string_view GpuFamilyName(GpuFamily family) noexcept
{
    switch (family) {
        case GpuFamily::Unsupported: return "unsupported";
        case GpuFamily::Rdna4: return "amd-rdna4";
    }
    return "unsupported";
}

constexpr std::string_view KernelTargetName(KernelTarget target) noexcept
{
    switch (target) {
        case KernelTarget::Unsupported: return "unsupported";
        case KernelTarget::Gfx1200: return "gfx1200";
        case KernelTarget::Gfx1201: return "gfx1201";
    }
    return "unsupported";
}

constexpr GpuFamily ClassifyGpu(uint32_t vendorId, std::wstring_view description) noexcept
{
    if (vendorId != kAmdVendorId) return GpuFamily::Unsupported;
    if (description.find(L"RX 9070") != std::wstring_view::npos ||
        description.find(L"RX 9060") != std::wstring_view::npos ||
        description.find(L"Radeon AI PRO R9700") != std::wstring_view::npos)
        return GpuFamily::Rdna4;
    return GpuFamily::Unsupported;
}

constexpr KernelTarget SelectKernelTarget(GpuFamily family,
                                          std::wstring_view description) noexcept
{
    if (family != GpuFamily::Rdna4) return KernelTarget::Unsupported;
    if (description.find(L"RX 9060") != std::wstring_view::npos)
        return KernelTarget::Gfx1200;
    if (description.find(L"RX 9070") != std::wstring_view::npos ||
        description.find(L"Radeon AI PRO R9700") != std::wstring_view::npos)
        return KernelTarget::Gfx1201;
    return KernelTarget::Unsupported;
}

struct RuntimeLayout
{
    bool runtimeDll{};
    bool assetsDirectory{};
    bool shaders{};
    bool modules{};
    bool weights{};
    bool lockFile{};

    constexpr bool Complete() const noexcept
    {
        return runtimeDll && assetsDirectory && shaders && modules && weights && lockFile;
    }
    constexpr bool Absent() const noexcept
    {
        return !runtimeDll && !assetsDirectory && !shaders && !modules && !weights && !lockFile;
    }
};

enum class LayoutState
{
    Absent,
    Complete,
    Incomplete,
};

constexpr LayoutState ClassifyLayout(const RuntimeLayout& layout) noexcept
{
    if (layout.Absent()) return LayoutState::Absent;
    return layout.Complete() ? LayoutState::Complete : LayoutState::Incomplete;
}

// The panel's pass count, written where the runtime will read it: the
// `DLSS5_MULTI_PASS` key of the runtime's own custom-config.txt, under
// `DLSS5-AMD\` next to the DLL.
//
// Why a file and not the process environment. The runtime reads its
// configuration exactly once per process, when the network is created
// (LoadFlagsFileOnce is a std::call_once inside the module), and its DLL is a
// static-CRT build with no option setter in its API table - so an environment
// variable set by this process cannot reach it: this program is /MT too, and
// two static CRTs have two private copies of the environment. Measured, not
// assumed: with the count injected into this process's environment, renders at
// one pass and at two came out byte-identical. The file is the channel that
// does work, and rewriting the key is idempotent, so every AMD job states the
// count again before the network is built.
//
// Everything else in the file is left alone - the reader's rules are
// reproduced here rather than replaced (a line counts when it starts with
// DLSS5_, holds '=', and is at least 8 characters; anything else, comments
// included, passes through), because a file rewritten wholesale would take the
// user's other switches with it.
inline std::string AmdPassesConfigLine(uint32_t passes)
{
    return "DLSS5_MULTI_PASS=" + std::to_string(passes < 1 ? 1u : (passes > 4 ? 4u : passes));
}

// Whether `line` sets DLSS5_MULTI_PASS, matched on the KEY rather than on the
// prefix: the reader counts any line that starts with DLSS5_ and holds '=', and
// the runtime's own switches share that prefix - DLSS5_MULTI_PASS_SKIN_PROTECT
// is a different key that a prefix match would swallow. Same key twice in one
// file is the last line that wins, so this returns true for every occurrence and
// the writer keeps the last one, which is where the reader would look.
inline bool IsAmdPassesConfigLine(std::string_view line) noexcept
{
    if (line.size() < 8 || line.substr(0, 6) != "DLSS5_") return false;
    const size_t eq = line.find('=');
    return eq != std::string_view::npos && line.substr(0, eq) == "DLSS5_MULTI_PASS";
}

inline bool WriteAmdPassesConfig(const std::filesystem::path& runtimeDirectory, uint32_t passes,
                                 std::string& error)
{
    namespace fs = std::filesystem;
    const fs::path directory = runtimeDirectory / L"DLSS5-AMD";
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        error = "the runtime's configuration folder could not be created (" + directory.string() + ")";
        return false;
    }
    const fs::path path = directory / L"custom-config.txt";
    const std::string wanted = AmdPassesConfigLine(passes);
    std::vector<std::string> lines;
    bool replaced = false;
    {
        std::ifstream input(path, std::ios::binary);
        std::ostringstream buffer;
        if (input) buffer << input.rdbuf();
        const std::string text = buffer.str();
        // Two passes over the text: which lines are ours, and where the reader
        // would read the key. Only the LAST occurrence is rewritten and the
        // earlier ones are dropped, so the file the runtime parses agrees with
        // the file it would have parsed before this call.
        std::vector<size_t> ours;
        {
            size_t start = 0;
            while (start < text.size()) {
                const size_t end = text.find('\n', start);
                std::string_view line(text.data() + start,
                                      (end == std::string::npos ? text.size() : end) - start);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                if (IsAmdPassesConfigLine(line)) ours.push_back(start);
                if (end == std::string::npos) break;
                start = end + 1;
            }
        }
        const size_t keep = ours.empty() ? std::string::npos : ours.back();
        size_t start = 0;
        while (start < text.size()) {
            const size_t end = text.find('\n', start);
            std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            const bool isOurs = IsAmdPassesConfigLine(line);
            if (!isOurs) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(std::move(line));
            } else if (start == keep) {
                lines.push_back(wanted);
                replaced = true;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    if (!replaced) lines.push_back(wanted);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "the runtime's configuration file could not be written (" + path.string() + ")";
        return false;
    }
    for (const std::string& line : lines) output << line << "\n";
    if (!output) {
        error = "the runtime's configuration file was not written whole (" + path.string() + ")";
        return false;
    }
    return true;
}

} // namespace amd_lmxxf
