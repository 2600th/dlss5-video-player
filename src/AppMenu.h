#pragma once

#include <windows.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ExampleVideos.h"
#include "UiLayout.h"

class Localizer;
enum class YouTubeSourceQuality;

namespace app_menu {

inline constexpr UINT IDM_OPEN = 100;
inline constexpr UINT IDM_EXIT = 101;
inline constexpr UINT IDM_OPEN_YOUTUBE = 102;
inline constexpr UINT IDM_EXAMPLE_VIDEO_FIRST = 110;
inline constexpr UINT IDM_RECENT_VIDEO_FIRST = 130;
inline constexpr UINT IDM_EXPORT_CACHED_VIDEO = 140;
inline constexpr UINT IDM_CANCEL_EXPORT = 141;
inline constexpr UINT IDM_PLAY = 200;
inline constexpr UINT IDM_STOP = 201;
inline constexpr UINT IDM_BACK10 = 202;
inline constexpr UINT IDM_FWD10 = 203;
inline constexpr UINT IDM_MUTE = 204;
inline constexpr UINT IDM_MARK_IN = 205;
inline constexpr UINT IDM_MARK_OUT = 206;
inline constexpr UINT IDM_CLEAR_MARKS = 207;
inline constexpr UINT IDM_GOTO_TIMECODE = 208;
inline constexpr UINT IDM_PAUSE_NEURAL_RENDER = 209;
inline constexpr UINT IDM_NEURAL_RENDERING = 300;
inline constexpr UINT IDM_REHOOK = 301;
inline constexpr UINT IDM_VIEW_FINAL = 302;
inline constexpr UINT IDM_VIEW_INPUT = 303;
inline constexpr UINT IDM_VIEW_MV = 304;
inline constexpr UINT IDM_VIEW_DEPTH = 305;
inline constexpr UINT IDM_DLSS_UPSCALING = 308;
inline constexpr UINT IDM_FRAME_GENERATION = 309;
inline constexpr UINT IDM_PREVIEW_FRAME = 310;
inline constexpr UINT IDM_PREVIEW_CLIP = 311;
inline constexpr UINT IDM_RENDER_RANGE = 312;
inline constexpr UINT IDM_RENDER_WHOLE = 313;
inline constexpr UINT IDM_NEURAL_SETTINGS = 314;
inline constexpr UINT IDM_ENCODER_SETTINGS = 315;
// One contiguous block: CheckMenuRadioItem clears every command in the range it
// is given, so a rung outside it would keep a stale check.
inline constexpr UINT IDM_UPSCALE_AUTO = 334;
inline constexpr UINT IDM_UPSCALE_1080 = 335;
inline constexpr UINT IDM_UPSCALE_1440 = 336;
inline constexpr UINT IDM_UPSCALE_2160 = 337;
inline constexpr UINT IDM_CANCEL_FRAME_GENERATION = 338;
// Outside 334..337 for the same reason, and the only surface that can reach a
// converted file once its confirmation dialog is gone.
inline constexpr UINT IDM_SHOW_FRAMEGEN_OUTPUT = 339;
// The generated-frame preference, its own contiguous radio block for the same
// reason as the upscaling rungs above: 2x is the default and the rest are
// opt-in, so one of these five is always checked.
inline constexpr UINT IDM_FRAMEGEN_2X = 340;
inline constexpr UINT IDM_FRAMEGEN_3X = 341;
inline constexpr UINT IDM_FRAMEGEN_4X = 342;
inline constexpr UINT IDM_FRAMEGEN_5X = 343;
inline constexpr UINT IDM_FRAMEGEN_MAX = 344;
// A checkbox, and deliberately OUTSIDE 340..344: CheckMenuRadioItem clears
// every command in the range it is given, and this one is a constraint on the
// multiple rather than one of the choices of multiple.
inline constexpr UINT IDM_FRAMEGEN_EVEN_ONLY = 345;
inline constexpr UINT IDM_ASPECT_FIT = 400;
inline constexpr UINT IDM_ASPECT_FILL = 401;
inline constexpr UINT IDM_FULLSCREEN = 402;
inline constexpr UINT IDM_VIDEO_ADJUSTMENTS = 403;
inline constexpr UINT IDM_YOUTUBE_QUALITY_AUTO = 410;
inline constexpr UINT IDM_YOUTUBE_QUALITY_2160 = 411;
inline constexpr UINT IDM_YOUTUBE_QUALITY_1440 = 412;
inline constexpr UINT IDM_YOUTUBE_QUALITY_1080 = 413;
inline constexpr UINT IDM_COMPARE_NEURAL = 420;
inline constexpr UINT IDM_COMPARE_BLEND = 421;
inline constexpr UINT IDM_COMPARE_SPLIT = 422;
inline constexpr UINT IDM_COMPARE_WIPE = 423;
inline constexpr UINT IDM_COMPARE_ZOOM = 424;
inline constexpr UINT IDM_COMPARE_BLEND_LESS = 425;
inline constexpr UINT IDM_COMPARE_BLEND_MORE = 426;
inline constexpr UINT IDM_ADVANCED_SAFE_MODE = 450;
inline constexpr UINT IDM_CLEAR_NEURAL_CACHE = 451;
inline constexpr UINT IDM_OPEN_RENDER_RECEIPT = 452;
inline constexpr UINT IDM_CHECK_FOR_UPDATES = 460;
// One contiguous radio block, like the upscaling rungs: CheckMenuRadioItem
// clears every command in the range it is given, so exactly one preset - or
// the Custom entry that ends the block - is checked at a time. Custom is not
// selectable; it reports that a control has been moved off every preset.
inline constexpr UINT IDM_NEURAL_PRESET_FIRST = 470;
inline constexpr UINT IDM_NEURAL_PRESET_CUSTOM = 479;
// Right-justified affordance appended to the menu bar itself, not a submenu.
inline constexpr UINT IDM_UPDATE_AVAILABLE = 461;

enum class PlayerCommandRoute {
    KeyDown,
    NativeMenu,
};

HMENU CreateMenuBar(const Localizer& localizer, bool youtubeAvailable);
void UpdateRecentVideos(HMENU menuBar, std::span<const std::wstring> titles, bool enabled);
HMENU CreateDebugViewMenu(UINT selectedCommand);
bool RoutesToRehook(PlayerCommandRoute route, UINT value);
// Shows, relabels or removes the right-justified update item in the menu bar.
// An empty label removes it; the caller redraws the bar.
bool SetUpdateBadge(HMENU menuBar, std::wstring_view label);
bool RoutesToOpenYouTube(PlayerCommandRoute route, UINT value, bool controlDown);
const ExampleVideo* ExampleVideoForCommand(UINT command);
bool UpdateSourceActionAvailability(HMENU menuBar, bool openEnabled,
                                    bool youtubeEnabled);
std::optional<YouTubeSourceQuality> YouTubeQualityForCommand(UINT command);
UINT CommandForYouTubeQuality(YouTubeSourceQuality quality);
bool UpdateYouTubeQualitySelection(HMENU menuBar, YouTubeSourceQuality quality);
// The two toggles carry a checkmark for their active state; frame generation
// does not, so frameGenerationActive is accepted and ignored - the item is a
// one-shot conversion, and a check on it stated a mode the player never has.
bool UpdateFeatureAvailability(HMENU menuBar, bool neuralRequested,
                               bool neuralAvailable, bool neuralActive,
                               bool upscalingAvailable, bool upscalingActive,
                               bool frameGenerationAvailable, bool frameGenerationActive);
// Range, preview and render commands, the render pause item and the receipt
// item. markersAvailable: a source is loaded; rangeRenderAvailable: that source
// can be range-rendered now (local/cached source, no active job).
bool UpdateRenderActionAvailability(HMENU menuBar, bool markersAvailable, bool rangeRenderAvailable,
                                    bool jobActive, bool jobPaused, bool receiptAvailable);
// Video > Compare: the mode radio group is enabled only while modesAvailable;
// selectedMode is one of IDM_COMPARE_NEURAL..IDM_COMPARE_WIPE.
bool UpdateComparisonMenu(HMENU menuBar, bool modesAvailable, bool zoomAvailable,
                          UINT selectedMode, bool zoomed);
// Menu command for a plain-key accelerator of the range, preview, neural
// settings and comparison items (I, O, Shift+I/O, Ctrl+G, F, Shift+F, Ctrl+R,
// Ctrl+N, Z, [ and ]); nullopt when the key is not one of them.
std::optional<UINT> CommandForPlayerKey(UINT key, bool controlDown, bool shiftDown);

} // namespace app_menu
