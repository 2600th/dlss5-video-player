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

// Whether `line` sets `key` exactly, under the reader's own rule: a line counts
// when it starts with DLSS5_, holds '=' and is long enough, and the key is
// everything before the first '='. Matching the key rather than the prefix is
// what keeps the runtime's own switches apart - DLSS5_MULTI_PASS_SKIN_PROTECT
// and DLSS5_MULTI_PASS_PREDICT share the prefix, and a prefix match swallowed
// the first of them when this writer began as a single-key one (measured, and
// the reason the tests below exist). Same key twice in one file is the last
// line that wins, so this returns true for every occurrence and the writer
// keeps the last one, which is where the reader looks.
inline bool IsAmdConfigLine(std::string_view line, std::string_view key) noexcept
{
    if (line.size() < 8 || line.substr(0, 6) != "DLSS5_") return false;
    const size_t eq = line.find('=');
    return eq != std::string_view::npos && line.substr(0, eq) == key;
}

inline bool IsAmdPassesConfigLine(std::string_view line) noexcept
{
    return IsAmdConfigLine(line, "DLSS5_MULTI_PASS");
}

// What the player owns inside the runtime's configuration file: the pass count
// and, on this runtime, whether later passes keep skin-coloured pixels pinned to
// the first pass (DLSS5_MULTI_PASS_SKIN_PROTECT). Both are written on every AMD
// job, the skin switch included when it is off - a key the panel has released has
// to be able to switch off one a hand edit turned on, and a runtime that reads
// its configuration once cannot be told again.
inline bool WriteAmdMultiPassConfig(const std::filesystem::path& runtimeDirectory, uint32_t passes,
                                    bool skinProtect, bool multiPassPredict, bool laterPassSkip,
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
    // Passes first, then the skin switch: the order they are written in is the
    // order they appear in when neither was in the file.
    std::string keys[4];
    std::string values[4];
    keys[0] = "DLSS5_MULTI_PASS";
    values[0] = AmdPassesConfigLine(passes).substr(keys[0].size() + 1);
    keys[1] = "DLSS5_MULTI_PASS_SKIN_PROTECT";
    values[1] = skinProtect ? "1" : "0";
    keys[2] = "DLSS5_MULTI_PASS_PREDICT";
    values[2] = multiPassPredict ? "1" : "0";
    // Off is an EMPTY value, not "0": the runtime treats an empty list as its
    // built-in default (run every block), which is the only spelling that also
    // switches off a list a hand edit left behind. The list itself is the ViT
    // and C512-uplink group the porting project measured - it costs the later
    // passes most of their reshaping, which is where the stacking's ghosting
    // comes from.
    keys[3] = "DLSS5_MULTI_PASS_SKIP_BLOCKS";
    values[3] = laterPassSkip ? "31,32,33,34,35,36,37,38,40,41,42,43,44,45,46,47" : "";
    constexpr size_t kKeyCount = 4;
    // Where each key's last line sits, or npos when the file has none yet.
    size_t keepAt[kKeyCount] = {std::string::npos, std::string::npos, std::string::npos, std::string::npos};
    std::vector<std::string> lines;
    {
        std::ifstream input(path, std::ios::binary);
        std::ostringstream buffer;
        if (input) buffer << input.rdbuf();
        const std::string text = buffer.str();
        // Two passes over the text: which lines are ours, and where the reader
        // would read each key. Only the LAST occurrence of each key is rewritten
        // and the earlier ones are dropped, so the file the runtime parses agrees
        // with the file it would have parsed before this call.
        {
            size_t start = 0;
            while (start < text.size()) {
                const size_t end = text.find('\n', start);
                std::string_view line(text.data() + start,
                                      (end == std::string::npos ? text.size() : end) - start);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                for (size_t key = 0; key < kKeyCount; ++key)
                    if (IsAmdConfigLine(line, keys[key])) keepAt[key] = start;
                if (end == std::string::npos) break;
                start = end + 1;
            }
        }
        size_t start = 0;
        while (start < text.size()) {
            const size_t end = text.find('\n', start);
            std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            size_t ours = kKeyCount;
            for (size_t key = 0; key < kKeyCount; ++key)
                if (IsAmdConfigLine(line, keys[key])) ours = key;
            if (ours == kKeyCount) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(std::move(line));
            } else if (keepAt[ours] == start) {
                lines.push_back(keys[ours] + "=" + values[ours]);
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    for (size_t key = 0; key < kKeyCount; ++key)
        if (keepAt[key] == std::string::npos) lines.push_back(keys[key] + "=" + values[key]);
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
