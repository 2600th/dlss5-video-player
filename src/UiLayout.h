#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class ToolbarAction {
    Open, OpenYouTube, Back10, PlayPause, Stop, Forward10, Mute,
    ToggleDlss, Aspect, Adjustments, DebugView, Fullscreen, None
};

struct ToolbarItem {
    ToolbarAction action{ToolbarAction::None};
    RECT bounds{};
    bool compact{false};
};

struct ToolbarAvailability {
    bool mediaLoaded{false};
    bool seeking{false};
    bool rendererReady{false};
    bool youtubeAvailable{false};
};

struct IdleSurfaceLayout {
    RECT title{};
    RECT subtitle{};
    std::array<ToolbarItem, 2> actions{};
    RECT youtubeReason{};
    bool stacked{false};
};

enum class PlayerRuntimeConfiguration {
    NeuralAddonExperimental,
    DlssSrSafeMode,
    NeuralAddonUnavailable,
};

enum class PlayerDlssState {
    Active,
    ScalerFallback,
};

struct PlayerRuntimeStatus {
    PlayerRuntimeConfiguration configuration{PlayerRuntimeConfiguration::NeuralAddonUnavailable};
    PlayerDlssState dlssState{PlayerDlssState::ScalerFallback};
};

enum class PlayerStatusActivity {
    None,
    ResolvingYouTube,
};

struct PlayerStatusSnapshot {
    bool mediaLoaded{false};
    PlayerStatusActivity activity{PlayerStatusActivity::None};
    PlayerRuntimeConfiguration runtimeConfiguration{PlayerRuntimeConfiguration::NeuralAddonUnavailable};
    PlayerDlssState dlssState{PlayerDlssState::ScalerFallback};
    uint32_t sourceWidth{};
    uint32_t sourceHeight{};
    uint32_t inputWidth{};
    uint32_t inputHeight{};
    uint32_t outputWidth{};
    uint32_t outputHeight{};
    std::wstring quality;
    double renderedFps{};
    double sourceFps{};
    uint64_t droppedFrames{};
};

struct PaintBufferLayout {
    RECT paintBounds{};
    int width{};
    int height{};
    POINT viewportOrigin{};
};

inline constexpr int kToolbarSpacingDip = 4;
inline constexpr int kToolbarMinHitHeightDip = 36;
inline constexpr int kToolbarCornerRadiusDip = 8;
inline constexpr int kToolbarOuterGutterDip = 16;
inline constexpr int kToolbarGroupGapDip = 12;

std::vector<ToolbarItem> LayoutToolbar(int clientWidth, int clientHeight, UINT dpi);
IdleSurfaceLayout LayoutIdleSurface(int clientWidth, int clientHeight, UINT dpi);
ToolbarAction HitTestToolbar(std::span<const ToolbarItem> items, POINT point);
int MinimumToolbarClientWidth(UINT dpi);
int MinimumIdleClientHeight(UINT dpi);
RECT ClampWindowRectToMinimumTrackSize(RECT suggested, POINT minimumTrackSize);
std::optional<RECT> LayoutVolumeSlider(int clientWidth, int clientHeight, UINT dpi,
                                      std::span<const ToolbarItem> toolbarItems);
bool IsToolbarActionEnabled(ToolbarAction action, ToolbarAvailability availability);
ToolbarAction NextFocusableToolbarAction(std::span<const ToolbarItem> items,
                                         ToolbarAction current, bool reverse,
                                         ToolbarAvailability availability);
std::vector<RECT> HoverDirtyRectangles(std::span<const ToolbarItem> items,
                                       ToolbarAction oldAction,
                                       ToolbarAction newAction);
ToolbarAction ResolveToolbarHover(std::span<const ToolbarItem> items,
                                  POINT point,
                                  ToolbarAvailability availability);
ToolbarAction ResolveToolbarHoverForCursor(std::span<const ToolbarItem> items,
                                           std::optional<POINT> clientPoint,
                                           ToolbarAvailability availability);
std::optional<PaintBufferLayout> LayoutPaintBuffer(RECT clientBounds, RECT paintBounds);
std::wstring BuildPlayerStatusText(const PlayerStatusSnapshot& status);
std::wstring BuildPlayerWindowTitle(std::wstring_view appTitle,
                                    std::wstring_view mediaTitle,
                                    size_t maxCharacters);
PlayerRuntimeStatus ResolvePlayerRuntimeStatus(bool safeMode,
                                               bool neuralAddonConfigured,
                                               bool dlssEnabled,
                                               bool dlssFeatureCreated);
bool ExecuteGuardedRehook(int dialogResult, const std::function<void()>& requestRecreate);
bool YouTubePlaybackAvailable();
