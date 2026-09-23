#pragma once

#include "GuideControls.h"
#include "TemporalGuides.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Guides read from files instead of the estimator: the A/B harness P2.4 asks for,
// so depth or flow computed offline (in Python, by a model this player does not
// ship) can be rendered through the unchanged neural pass and scored by
// tools/benchmark against the built-in guides.
//
// This is a benchmark path and never a player one. The player cannot ask for it:
// the helper takes it off its own command line (`--guides mv=file:<dir>,...`,
// `--guide-dump <dir>`) before the shared worker parser runs, so the canonical
// `mv=0|1,depth=0|1` string the cache key and the receipt are built from is the
// only thing that parser ever sees, and no cached render can be made from a file.
//
// The layout, which tools/benchmark/guidefiles.py writes and reads:
//  - one directory per guide, one file per source frame, named by the frame's
//    number - round(pts * fps), which is the decoder's own frameNumber and is the
//    zero-based decoded index for a file whose first frame is at pts 0 - as six
//    or more digits: `000000.pfm`, `000001.pfm`, ...
//  - each file a Portable Float Map: `Pf` (one channel) for depth, `PF` (three)
//    for motion, a negative scale for little-endian floats, and rows stored
//    BOTTOM to top as the format specifies.
//  - depth in [0,1], 0 = near and 1 = far, the convention DLSSBackend sets
//    (DepthInverted off); values outside are clamped when the GPU writes them.
//  - motion as (x, y, unused) in DLSS input pixels, pointing from the current
//    frame to where the content was in the previous one - the same unit and
//    direction as TemporalGuides' grid.
//  - any size. A file at the analysis grid's size (TemporalGuideGenerator::
//    AnalysisGrid: 160x90 for a 30-fps 1080p source) is taken exactly, which is
//    what makes a dump of the estimator's own guides render byte-identically when
//    it is fed back; any other size is area-averaged onto that grid, because the
//    grid is the resolution the renderer's expansion pass reads from.
namespace guide_files {

// Where the motion field comes from.
//  Estimator - the built-in path: hardware optical flow where the engine comes
//              up, the CPU block matcher otherwise (mv=1).
//  Cpu       - the CPU block matcher even where the engine is present (mv=cpu),
//              which is the only estimator a grid file can reproduce and so the
//              reference the round-trip proof is taken against.
//  File      - the file's field (mv=file:<dir>).
enum class MotionSource { Estimator, Cpu, File };

struct Sources {
    MotionSource motion = MotionSource::Estimator;
    std::filesystem::path motionDirectory;
    bool depthFromFile = false;
    std::filesystem::path depthDirectory;
    // Writes the guides each submitted frame was rendered with, in the layout
    // above, under <dump>/mv and <dump>/depth.
    std::filesystem::path dumpDirectory;

    bool Active() const
    {
        return motion != MotionSource::Estimator || depthFromFile || !dumpDirectory.empty();
    }
};

struct ParsedSpec {
    GuideControls controls;
    Sources sources;
};

// Accepts the canonical `mv=0|1,depth=0|1` and the benchmark forms
// `mv=cpu`, `mv=file:<dir>` and `depth=file:<dir>`, in that order. The motion
// value runs to the last `,depth=` and the depth value to the end of the text, so
// a depth path is the one place that sequence cannot appear. The controls a file
// mode reports are "on": the generator still runs, so the job's scene-cut
// decisions are the estimator's own whatever the guides are.
inline std::optional<ParsedSpec> ParseSpec(std::wstring_view text)
{
    constexpr std::wstring_view mvKey = L"mv=", depthKey = L",depth=", file = L"file:";
    if (text.substr(0, mvKey.size()) != mvKey) return std::nullopt;
    text.remove_prefix(mvKey.size());
    const size_t split = text.rfind(depthKey);
    if (split == std::wstring_view::npos) return std::nullopt;
    const std::wstring_view mv = text.substr(0, split), depth = text.substr(split + depthKey.size());
    ParsedSpec parsed;
    if (mv == L"0" || mv == L"1") {
        parsed.controls.motionVectors = mv == L"1";
    } else if (mv == L"cpu") {
        parsed.sources.motion = MotionSource::Cpu;
    } else if (mv.substr(0, file.size()) == file && mv.size() > file.size()) {
        parsed.sources.motion = MotionSource::File;
        parsed.sources.motionDirectory = std::filesystem::path(mv.substr(file.size()));
    } else {
        return std::nullopt;
    }
    if (depth == L"0" || depth == L"1") {
        parsed.controls.depth = depth == L"1";
    } else if (depth.substr(0, file.size()) == file && depth.size() > file.size()) {
        parsed.sources.depthFromFile = true;
        parsed.sources.depthDirectory = std::filesystem::path(depth.substr(file.size()));
    } else {
        return std::nullopt;
    }
    return parsed;
}

// Takes the benchmark forms off a helper command line. `rewritten` receives every
// argument with `--guide-dump <dir>` removed and the `--guides` value replaced by
// its canonical form, so the shared parser validates the job exactly as it would
// the player's. False for a malformed spec, a dump flag without a value, or
// either one given twice. A line without them comes back unchanged with inactive
// sources. The pair search starts at index 2, where the helper's key/value pairs
// start; a trailing lone flag is left where it is.
inline bool ExtractBenchmarkArguments(std::span<const std::wstring_view> arguments,
                                      std::vector<std::wstring>& rewritten, Sources& sources)
{
    rewritten.assign(arguments.begin(), arguments.end());
    sources = {};
    bool sawGuides = false, sawDump = false;
    for (size_t index = 2; index < rewritten.size(); ++index) {
        if (rewritten[index] == L"--guide-dump") {
            if (sawDump || index + 1 >= rewritten.size() || rewritten[index + 1].empty()) return false;
            sawDump = true;
            const std::filesystem::path dump = rewritten[index + 1];
            rewritten.erase(rewritten.begin() + static_cast<std::ptrdiff_t>(index),
                            rewritten.begin() + static_cast<std::ptrdiff_t>(index + 2));
            sources.dumpDirectory = dump;
            --index;
            continue;
        }
        if (rewritten[index] == L"--guides" && index + 1 < rewritten.size()) {
            if (sawGuides) return false;
            sawGuides = true;
            const auto spec = ParseSpec(rewritten[index + 1]);
            if (!spec) return false;
            const std::filesystem::path dump = sources.dumpDirectory;
            sources = spec->sources;
            sources.dumpDirectory = dump;
            const std::string canonical = CanonicalGuideControls(spec->controls);
            rewritten[index + 1].assign(canonical.begin(), canonical.end());
            ++index;
        }
    }
    return true;
}

inline std::filesystem::path FramePath(const std::filesystem::path& directory, uint64_t frameNumber)
{
    wchar_t name[32]{};
    std::swprintf(name, std::size(name), L"%06llu.pfm", static_cast<unsigned long long>(frameNumber));
    return directory / name;
}

// A decoded map, rows TOP to bottom (the order a grid is in), channel-interleaved.
struct Plane {
    uint32_t width = 0, height = 0, channels = 0;
    std::vector<float> values;
};

// Parses a PFM. Big-endian files are accepted and swapped; anything whose header
// or size does not add up is refused rather than read short.
inline std::optional<Plane> ParsePfm(std::span<const uint8_t> bytes, std::string* error = nullptr)
{
    auto fail = [&](const char* why) -> std::optional<Plane> { if (error) *error = why; return std::nullopt; };
    size_t pos = 0;
    auto token = [&]() -> std::string {
        while (pos < bytes.size() && std::isspace(bytes[pos])) ++pos;
        std::string out;
        while (pos < bytes.size() && !std::isspace(bytes[pos])) out.push_back(static_cast<char>(bytes[pos++]));
        return out;
    };
    const std::string magic = token();
    Plane plane;
    if (magic == "Pf") plane.channels = 1;
    else if (magic == "PF") plane.channels = 3;
    else return fail("not a PFM (magic is neither Pf nor PF)");
    const std::string w = token(), h = token(), s = token();
    char* end = nullptr;
    const unsigned long width = std::strtoul(w.c_str(), &end, 10);
    if (w.empty() || *end) return fail("PFM width is not a number");
    const unsigned long height = std::strtoul(h.c_str(), &end, 10);
    if (h.empty() || *end) return fail("PFM height is not a number");
    const double scale = std::strtod(s.c_str(), &end);
    if (s.empty() || *end || scale == 0.0 || !std::isfinite(scale)) return fail("PFM scale is not a nonzero number");
    if (!width || !height || width > 16384 || height > 16384) return fail("PFM size is outside 1..16384");
    // Exactly one whitespace byte separates the header from the data.
    if (pos >= bytes.size() || !std::isspace(bytes[pos])) return fail("PFM header is not terminated");
    ++pos;
    plane.width = static_cast<uint32_t>(width);
    plane.height = static_cast<uint32_t>(height);
    const size_t count = size_t(plane.width) * plane.height * plane.channels;
    if (bytes.size() - pos != count * sizeof(float)) return fail("PFM data size does not match its header");
    plane.values.resize(count);
    const bool bigEndian = scale > 0.0;
    const size_t row = size_t(plane.width) * plane.channels;
    for (uint32_t y = 0; y < plane.height; ++y) {
        // Stored bottom row first.
        const uint8_t* source = bytes.data() + pos + size_t(plane.height - 1 - y) * row * sizeof(float);
        for (size_t i = 0; i < row; ++i) {
            uint8_t raw[4];
            std::memcpy(raw, source + i * sizeof(float), 4);
            if (bigEndian) { std::swap(raw[0], raw[3]); std::swap(raw[1], raw[2]); }
            std::memcpy(&plane.values[size_t(y) * row + i], raw, 4);
        }
    }
    return plane;
}

inline std::vector<uint8_t> EncodePfm(const Plane& plane)
{
    const std::string header = std::string(plane.channels == 1 ? "Pf" : "PF") + "\n" +
        std::to_string(plane.width) + " " + std::to_string(plane.height) + "\n-1.0\n";
    std::vector<uint8_t> bytes(header.begin(), header.end());
    const size_t row = size_t(plane.width) * plane.channels;
    bytes.reserve(bytes.size() + plane.values.size() * sizeof(float));
    for (uint32_t y = plane.height; y-- > 0;) {
        const auto* source = reinterpret_cast<const uint8_t*>(plane.values.data() + size_t(y) * row);
        bytes.insert(bytes.end(), source, source + row * sizeof(float));
    }
    return bytes;
}

// Area average onto a gw x gh grid, each cell covering the same source rectangle
// DownsampleLuma gives it. At equal size every cell is exactly one source value,
// copied rather than averaged, so a grid-sized file passes through bit for bit.
inline Plane ResampleArea(const Plane& source, uint32_t gw, uint32_t gh)
{
    if (source.width == gw && source.height == gh) return source;
    Plane out{gw, gh, source.channels, std::vector<float>(size_t(gw) * gh * source.channels)};
    for (uint32_t gy = 0; gy < gh; ++gy) {
        const uint32_t y0 = uint32_t((uint64_t(gy) * source.height) / gh);
        const uint32_t y1 = std::max(y0 + 1, uint32_t((uint64_t(gy + 1) * source.height) / gh));
        for (uint32_t gx = 0; gx < gw; ++gx) {
            const uint32_t x0 = uint32_t((uint64_t(gx) * source.width) / gw);
            const uint32_t x1 = std::max(x0 + 1, uint32_t((uint64_t(gx + 1) * source.width) / gw));
            for (uint32_t c = 0; c < source.channels; ++c) {
                double sum = 0.0;
                for (uint32_t y = y0; y < y1; ++y)
                    for (uint32_t x = x0; x < x1; ++x)
                        sum += source.values[(size_t(y) * source.width + x) * source.channels + c];
                out.values[(size_t(gy) * gw + gx) * source.channels + c] =
                    static_cast<float>(sum / double(size_t(y1 - y0) * (x1 - x0)));
            }
        }
    }
    return out;
}

inline std::optional<Plane> ReadPfmFile(const std::filesystem::path& path, std::string* error = nullptr)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (error) *error = "cannot open " + path.string(); return std::nullopt; }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string why;
    auto plane = ParsePfm(bytes, &why);
    if (!plane && error) *error = path.string() + ": " + why;
    return plane;
}

inline bool WritePfmFile(const std::filesystem::path& path, const Plane& plane)
{
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const auto bytes = EncodePfm(plane);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// The grid's motion (R, G) or depth (B) as a plane.
inline Plane GridChannels(const GuideFrame& guide, bool motion)
{
    Plane plane{guide.gridW, guide.gridH, motion ? 3u : 1u, {}};
    const size_t cells = size_t(guide.gridW) * guide.gridH;
    plane.values.resize(cells * plane.channels);
    if (guide.guideGridRGBA32F.size() < cells * 4) return {};
    for (size_t cell = 0; cell < cells; ++cell) {
        if (motion) {
            plane.values[cell * 3] = guide.guideGridRGBA32F[cell * 4];
            plane.values[cell * 3 + 1] = guide.guideGridRGBA32F[cell * 4 + 1];
            plane.values[cell * 3 + 2] = 0.0f;
        } else {
            plane.values[cell] = guide.guideGridRGBA32F[cell * 4 + 2];
        }
    }
    return plane;
}

// Replaces the guide grid's channels from the files for `frameNumber` and writes
// the dump, per `sources`. A missing or unreadable file fails the frame: a render
// that quietly fell back to the estimator for some frames would be scored as the
// file's. Motion from anywhere but the estimator also clears
// `guide.motionVectors`, because that flag is what lets the renderer's hardware
// optical flow overwrite the grid's R and G - on a card with the engine, leaving
// it set would render the engine's field and report the file's.
inline bool Apply(const Sources& sources, uint64_t frameNumber, GuideFrame& guide, std::string* error = nullptr)
{
    const size_t cells = size_t(guide.gridW) * guide.gridH;
    if (!cells || guide.guideGridRGBA32F.size() < cells * 4) {
        if (error) *error = "the guide grid is empty";
        return false;
    }
    auto load = [&](const std::filesystem::path& directory, uint32_t channels) -> std::optional<Plane> {
        auto plane = ReadPfmFile(FramePath(directory, frameNumber), error);
        if (!plane) return std::nullopt;
        if (plane->channels != channels) {
            if (error) *error = FramePath(directory, frameNumber).string() +
                (channels == 1 ? ": depth must be a one-channel Pf map" : ": motion must be a three-channel PF map");
            return std::nullopt;
        }
        return ResampleArea(*plane, guide.gridW, guide.gridH);
    };
    if (sources.motion == MotionSource::File) {
        const auto motion = load(sources.motionDirectory, 3);
        if (!motion) return false;
        for (size_t cell = 0; cell < cells; ++cell) {
            guide.guideGridRGBA32F[cell * 4] = motion->values[cell * 3];
            guide.guideGridRGBA32F[cell * 4 + 1] = motion->values[cell * 3 + 1];
        }
    }
    if (sources.motion != MotionSource::Estimator) guide.motionVectors = false;
    if (sources.depthFromFile) {
        const auto depth = load(sources.depthDirectory, 1);
        if (!depth) return false;
        for (size_t cell = 0; cell < cells; ++cell) guide.guideGridRGBA32F[cell * 4 + 2] = depth->values[cell];
    }
    if (!sources.dumpDirectory.empty()) {
        if (!WritePfmFile(FramePath(sources.dumpDirectory / L"mv", frameNumber), GridChannels(guide, true)) ||
            !WritePfmFile(FramePath(sources.dumpDirectory / L"depth", frameNumber), GridChannels(guide, false))) {
            if (error) *error = "cannot write the guide dump under " + sources.dumpDirectory.string();
            return false;
        }
    }
    return true;
}

} // namespace guide_files
