#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "RuntimePolicy.h"
#include "UiLayout.h"

// The idle window as a start screen: can this machine do what the player is
// for, and what can be opened in one click. Before, it offered Open file and
// Open YouTube URL and nothing else, and the first question every reporter
// asked (issue #1: "what resolution do I have to give to this?") had its
// answer in a log file.
//
// Pure: the facts are gathered off the UI thread by main.cpp, and what is
// said about them and where it goes is decided here.
namespace start_screen {

enum class RuntimeState {
    Checking,     // the worker has not answered yet
    Absent,       // no neural-runtime folder: DLSS SR only, by design
    Incomplete,   // some of the runtime's files are missing
    Verified,     // every locked file matches packaging/runtime-lock.json
    Drifted,      // present, but a file differs from the lock
};

struct Facts {
    std::wstring gpu;
    GpuGeneration generation{GpuGeneration::Unsupported};
    std::wstring driverVersion;   // DXGI's form, "32.0.16.1047"
    bool safeMode{};
    RuntimeState runtime{RuntimeState::Checking};
    std::wstring runtimeVersion;  // the lock's, when verified
    // Frames per second the live render is forecast to manage, only when a
    // measurement or a measured prior backs it. Nothing is printed otherwise:
    // an invented number here is the question the screen exists to answer.
    std::optional<double> fps1080;
    std::optional<double> fps1440;
};

enum class Mark { Pass, Fail, Info, Pending };

struct Line {
    std::wstring label;
    std::wstring value;
    Mark mark{Mark::Info};

    friend bool operator==(const Line&, const Line&) = default;
};

inline std::wstring FpsText(double fps)
{
    return L"about " + std::to_wstring(static_cast<long long>(std::lround(fps))) + L" fps";
}

inline std::vector<Line> CapabilityLines(const Facts& facts)
{
    std::vector<Line> lines;
    const bool rtx = facts.generation != GpuGeneration::Unsupported && facts.generation != GpuGeneration::OtherNvidia;
    lines.push_back(Line{L"GPU", facts.gpu.empty() ? L"No NVIDIA GPU found" :
                         rtx ? facts.gpu : facts.gpu + L" · not an RTX GPU, so no DLSS",
                         rtx ? Mark::Pass : Mark::Fail});
    const std::wstring floor = FormatNvidiaDriverVersion(kNeuralDriverFloor);
    if (const auto version = ParseNvidiaDriverVersion(facts.driverVersion)) {
        const bool meets = !(*version < kNeuralDriverFloor);
        lines.push_back(Line{L"Driver", FormatNvidiaDriverVersion(*version) +
                             (meets ? L" · meets the " + floor + L" minimum"
                                    : L" · neural rendering needs " + floor + L" or newer"),
                             meets ? Mark::Pass : Mark::Fail});
    } else {
        lines.push_back(Line{L"Driver", L"Version not reported · " + floor + L" or newer is needed", Mark::Info});
    }
    switch (facts.runtime) {
    case RuntimeState::Checking: lines.push_back(Line{L"Neural runtime", L"Checking…", Mark::Pending}); break;
    case RuntimeState::Absent: lines.push_back(Line{L"Neural runtime", L"Not installed · DLSS Super Resolution only", Mark::Info}); break;
    case RuntimeState::Incomplete: lines.push_back(Line{L"Neural runtime", L"Incomplete · reinstall the package", Mark::Fail}); break;
    case RuntimeState::Verified:
        lines.push_back(Line{L"Neural runtime", L"Present · matches the lock" +
                             (facts.runtimeVersion.empty() ? std::wstring{} : L" (" + facts.runtimeVersion + L")"), Mark::Pass});
        break;
    case RuntimeState::Drifted: lines.push_back(Line{L"Neural runtime", L"Present · a file differs from the lock", Mark::Fail}); break;
    }
    if (facts.safeMode) lines.push_back(Line{L"Mode", L"Safe mode · neural add-on off for this launch", Mark::Info});
    if (facts.fps1080 || facts.fps1440) {
        std::wstring value;
        if (facts.fps1080) value = FpsText(*facts.fps1080) + L" at 1080p";
        if (facts.fps1440) value += (value.empty() ? L"" : L" · ") + FpsText(*facts.fps1440) + L" at 1440p";
        lines.push_back(Line{L"Neural render", value, Mark::Info});
    }
    return lines;
}

// Safe mode is offered only where it helps: a check failed, and the player
// is not already in it.
inline bool OfferSafeMode(const std::vector<Line>& lines, bool safeMode)
{
    return !safeMode && std::any_of(lines.begin(), lines.end(), [](const Line& line) { return line.mark == Mark::Fail; });
}

// "Rendered 100%" for a whole-video render; a share when the source's length
// is known; the rendered span otherwise, which is still a real fact.
inline std::wstring CoverageBadge(int64_t rangeStart100ns, int64_t rangeEnd100ns, int64_t renderDuration100ns,
                                  std::optional<int64_t> sourceDuration100ns)
{
    if (rangeStart100ns == 0 && rangeEnd100ns == 0) return L"Rendered 100%";
    const int64_t span = rangeEnd100ns > rangeStart100ns ? rangeEnd100ns - rangeStart100ns : renderDuration100ns;
    if (span <= 0) return {};
    if (sourceDuration100ns && *sourceDuration100ns > 0) {
        const double share = std::clamp(double(span) / double(*sourceDuration100ns), 0.0, 1.0);
        const long long percent = share >= 1.0 ? 100 : std::min<long long>(99, std::max<long long>(1, std::llround(share * 100.0)));
        return L"Rendered " + std::to_wstring(percent) + L"%";
    }
    return L"Rendered " + std::to_wstring(static_cast<long long>(std::llround(double(span) * 1e-7))) + L" s";
}

// ---- Layout ------------------------------------------------------------------

inline constexpr int kTileWidthDip = 192;
inline constexpr int kTileThumbHeightDip = 108;   // 16:9 of the width
// Smaller tiles, tried before a row is dropped: at 175% the default window is
// 503 dip tall, and with 192 dip tiles a fresh profile's one row of trailers
// never fitted beside the capability panel, so the start screen showed none.
inline constexpr int kMediumTileWidthDip = 160;
inline constexpr int kSmallTileWidthDip = 128;
inline constexpr int kTileTextDip = 42;           // title and detail lines
inline constexpr int kTileGapDip = 16;
inline constexpr int kLineHeightDip = 22;
inline constexpr int kLabelWidthDip = 132;
inline constexpr int kPanelWidthDip = 640;
inline constexpr int kSectionGapDip = 22;
inline constexpr int kHeadingDip = 26;

struct Layout {
    IdleSurfaceLayout core;
    // False: the window is too small for anything past the two buttons, and
    // the core is the plain idle surface it always was.
    bool full{};
    std::vector<RECT> lines;       // one per capability line
    int labelWidth{};              // the label column inside a line
    RECT safeMode{};               // empty when not offered
    RECT recentHeading{}, trailersHeading{};
    std::vector<RECT> recentTiles, trailerTiles;
    RECT hint{};
};

// The capability panel's two columns as the caller measured them, in pixels:
// the widest mark-and-label, and the widest value. With both, the panel is
// exactly as wide as its text and centred as one block under the centred
// title; without them it keeps the fixed width it had.
struct PanelText {
    int labelColumn{};
    int valueColumn{};
};

inline int ScaleDip(int value, UINT dpi)
{
    return MulDiv(value, static_cast<int>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi), USER_DEFAULT_SCREEN_DPI);
}

// Stacked and centred: the title and the two buttons, the capability panel,
// a row of recent videos, a row of game trailers, the D hint. Each row holds
// as many whole tiles as fit. When the window is too short for all of it the
// trailers go first, then the recent row, then the panel; with nothing left
// to show, the plain idle surface is returned exactly as LayoutIdleSurface
// makes it, so small windows keep the layout the idle tests pin.
// `youtubeReason` says whether the idle surface's line under the buttons
// (why YouTube is unavailable) has anything to say; when it does not, its
// 46 dip are given to the rows below instead of left blank.
inline Layout LayoutStartScreen(int clientWidth, int clientHeight, UINT dpi, size_t lineCount, bool safeModeLink,
                                size_t recentCount, size_t trailerCount, PanelText panelText = {}, bool youtubeReason = true)
{
    const auto D = [dpi](int value) { return ScaleDip(value, dpi); };
    Layout layout{};
    const int width = std::max(1, clientWidth), height = std::max(1, clientHeight);
    const int gutter = D(16);
    // The core measured from the idle surface itself, at a height where it
    // is not squeezed.
    const IdleSurfaceLayout natural = LayoutIdleSurface(width, D(400), dpi);
    const int coreHeight = static_cast<int>((youtubeReason ? natural.youtubeReason.bottom : natural.actions[1].bounds.bottom) - natural.title.top);
    const int panelHeight = static_cast<int>(lineCount + (safeModeLink ? 1 : 0)) * D(kLineHeightDip);

    // Rows go before tiles shrink: every row that fits at the largest tile
    // size that fits it. The order rows give way in is unchanged - trailers,
    // then the recent row, then the panel.
    bool showPanel = lineCount > 0, showRecent = false, showTrailers = false;
    int tileWidth = D(kTileWidthDip), thumbHeight = D(kTileThumbHeightDip);
    size_t recentShown = 0, trailersShown = 0;
    const int available = height - 2 * gutter;
    const auto fits = [&](bool panel, bool recent, bool trailers, int tileDip) {
        const int tile = D(tileDip), thumb = MulDiv(tile, 9, 16);
        const int perRow = std::max(0, (width - 2 * gutter + D(kTileGapDip)) / (tile + D(kTileGapDip)));
        const size_t recents = recent ? std::min<size_t>(recentCount, static_cast<size_t>(perRow)) : 0;
        const size_t trailerTiles = trailers ? std::min<size_t>(trailerCount, static_cast<size_t>(perRow)) : 0;
        if ((recent && recents == 0) || (trailers && trailerTiles == 0)) return false;
        const int rowHeight = D(kHeadingDip) + thumb + D(kTileTextDip);
        int sum = coreHeight + D(kSectionGapDip) + D(kLineHeightDip);   // the hint
        if (panel) sum += D(kSectionGapDip) + panelHeight;
        if (recent) sum += D(kSectionGapDip) + rowHeight;
        if (trailers) sum += D(kSectionGapDip) + rowHeight;
        if (sum > available) return false;
        showPanel = panel; showRecent = recent; showTrailers = trailers;
        tileWidth = tile; thumbHeight = thumb; recentShown = recents; trailersShown = trailerTiles;
        return true;
    };
    const bool anyRecent = recentCount > 0, anyTrailer = trailerCount > 0, anyPanel = lineCount > 0;
    const auto rows = [&](bool panel, bool recent, bool trailers) {
        for (const int tileDip : {kTileWidthDip, kMediumTileWidthDip, kSmallTileWidthDip})
            if (fits(panel, recent, trailers, tileDip)) return true;
        return false;
    };
    const bool placed = rows(anyPanel, anyRecent, anyTrailer) || rows(anyPanel, anyRecent, false) ||
                        (anyPanel && fits(true, false, false, kTileWidthDip));
    if (!placed || !(showPanel || showRecent || showTrailers)) {
        layout.core = LayoutIdleSurface(clientWidth, clientHeight, dpi);
        return layout;
    }
    const int tileHeight = thumbHeight + D(kTileTextDip);
    const auto total = [&] {
        int sum = coreHeight + D(kSectionGapDip) + D(kLineHeightDip);
        if (showPanel) sum += D(kSectionGapDip) + panelHeight;
        if (showRecent) sum += D(kSectionGapDip) + D(kHeadingDip) + tileHeight;
        if (showTrailers) sum += D(kSectionGapDip) + D(kHeadingDip) + tileHeight;
        return sum;
    };
    layout.full = true;
    int y = gutter + std::max(0, (available - total()) / 2);
    const LONG shift = y - natural.title.top;
    layout.core = natural;
    for (RECT* rect : {&layout.core.title, &layout.core.subtitle, &layout.core.youtubeReason}) OffsetRect(rect, 0, shift);
    for (auto& action : layout.core.actions) OffsetRect(&action.bounds, 0, shift);
    y += coreHeight;
    if (showPanel) {
        y += D(kSectionGapDip);
        const bool measured = panelText.labelColumn > 0 && panelText.valueColumn > 0;
        const int panelWidth = std::min(measured ? panelText.labelColumn + panelText.valueColumn : D(kPanelWidthDip), width - 2 * gutter);
        const int left = (width - panelWidth) / 2;
        layout.labelWidth = measured ? std::min(panelText.labelColumn, panelWidth / 2) : std::min(D(kLabelWidthDip), panelWidth / 3);
        for (size_t index = 0; index < lineCount; ++index, y += D(kLineHeightDip))
            layout.lines.push_back(RECT{left, y, left + panelWidth, y + D(kLineHeightDip)});
        if (safeModeLink) {
            layout.safeMode = RECT{left + layout.labelWidth, y, left + panelWidth, y + D(kLineHeightDip)};
            y += D(kLineHeightDip);
        }
    }
    const auto row = [&](size_t count, RECT& heading, std::vector<RECT>& tiles) {
        y += D(kSectionGapDip);
        const int rowWidth = static_cast<int>(count) * tileWidth + (static_cast<int>(count) - 1) * D(kTileGapDip);
        const int left = (width - rowWidth) / 2;
        heading = RECT{left, y, left + rowWidth, y + D(kHeadingDip)};
        y += D(kHeadingDip);
        for (size_t index = 0; index < count; ++index) {
            const int tileLeft = left + static_cast<int>(index) * (tileWidth + D(kTileGapDip));
            tiles.push_back(RECT{tileLeft, y, tileLeft + tileWidth, y + tileHeight});
        }
        y += tileHeight;
    };
    if (showRecent) row(recentShown, layout.recentHeading, layout.recentTiles);
    if (showTrailers) row(trailersShown, layout.trailersHeading, layout.trailerTiles);
    y += D(kSectionGapDip);
    layout.hint = RECT{gutter, y, width - gutter, y + D(kLineHeightDip)};
    return layout;
}

// The thumbnail part of a tile: 16:9 of whatever width the layout gave it.
inline RECT TileThumbnail(RECT tile, UINT dpi)
{
    const int height = std::max(0, static_cast<int>(tile.bottom - tile.top) - ScaleDip(kTileTextDip, dpi));
    return RECT{tile.left, tile.top, tile.right, tile.top + height};
}

} // namespace start_screen
