#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
// Export with any combination of the three neural stages, as against
// IDM_EXPORT_CACHED_VIDEO, which writes out the render already in the cache.
inline constexpr UINT IDM_EXPORT_STAGES = 142;
// The composed comparison as shown, with a provenance footer, as a PNG.
inline constexpr UINT IDM_SAVE_COMPARISON_IMAGE = 143;
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
// One contiguous radio block for the source's audio streams, like the
// upscaling rungs: CheckMenuRadioItem clears every command in the range it is
// given, so exactly one track carries the mark. Sixteen is far past what any
// consumer container carries and keeps the block clear of IDM_NEURAL_RENDERING.
inline constexpr UINT IDM_AUDIO_TRACK_FIRST = 210;
inline constexpr UINT IDM_AUDIO_TRACK_COUNT = 16;
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
// Fit, Fill and 1:1 pixels are one radio group by POSITION (see CheckRadioCommand):
// 1:1 sits under Fill in the menu with an id past the two that were already taken.
inline constexpr UINT IDM_ASPECT_FIT = 400;
inline constexpr UINT IDM_ASPECT_FILL = 401;
inline constexpr UINT IDM_ASPECT_ONE_TO_ONE = 404;
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
// The modes are one radio group in the Compare popup. CheckMenuRadioItem works on
// the POSITIONS between its first and last command, so the group is contiguous in
// the popup - IDM_COMPARE_NEURAL first, kLastComparisonModeCommand last - whatever
// the ids are. IDM_COMPARE_BLEND keeps its number but has no row: Blend became the
// Mix, which [ and ] still step.
inline constexpr UINT IDM_COMPARE_ORIGINAL = 427;
inline constexpr UINT IDM_COMPARE_SWAP = 428;
inline constexpr UINT IDM_COMPARE_NEXT_MODE = 429;
inline constexpr UINT IDM_COMPARE_PREVIOUS_MODE = 430;
inline constexpr UINT IDM_COMPARE_ZOOM_OUT = 431;
inline constexpr UINT IDM_COMPARE_ZOOM_FIT = 432;
inline constexpr UINT IDM_COMPARE_LOUPE = 433;
inline constexpr UINT IDM_COMPARE_DIFFERENCE = 434;
inline constexpr UINT IDM_COMPARE_DIFFERENCE_LESS = 435;
inline constexpr UINT IDM_COMPARE_DIFFERENCE_MORE = 436;
inline constexpr UINT IDM_COMPARE_DIFFERENCE_LUMA = 437;
inline constexpr UINT IDM_COMPARE_SIDE_BY_SIDE = 447;
inline constexpr UINT IDM_COMPARE_QUAD = 448;
inline constexpr UINT kLastComparisonModeCommand = IDM_COMPARE_QUAD;
// Quad's second Mix, one command per compare_settings::kSecondMixes entry, in order.
// A block of its own past every other command (500-599 stay unused, see the menu
// tests): 449 would have run into the Advanced items at 450-452, and HandleCommand
// routes the block before its switch.
inline constexpr UINT IDM_COMPARE_SECOND_MIX_FIRST = 610;
inline constexpr UINT IDM_COMPARE_SECOND_MIX_COUNT = 5;
// The spatial mask on the Mix. The feather choices are their own radio block, one
// command per compare_mask::kFeathers entry, in its order.
inline constexpr UINT IDM_COMPARE_MASK_LOAD = 438;
inline constexpr UINT IDM_COMPARE_MASK_INVERT = 439;
inline constexpr UINT IDM_COMPARE_MASK_CLEAR = 440;
inline constexpr UINT IDM_COMPARE_MASK_FEATHER_FIRST = 441;
inline constexpr UINT IDM_COMPARE_MASK_FEATHER_COUNT = 6;
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
// The processing-scale rungs, one contiguous radio block in the order of
// kProcessingScaleRungs (UpscalingPolicy.h): Source 100% first, then 75, 50.
inline constexpr UINT IDM_PROCESSING_SCALE_FIRST = 480;
inline constexpr UINT IDM_PROCESSING_SCALE_LAST = 482;
// Right-justified affordance appended to the menu bar itself, not a submenu.
inline constexpr UINT IDM_UPDATE_AVAILABLE = 461;
// Help > Keyboard shortcuts, the menu route to the ? / F1 cheat sheet.
inline constexpr UINT IDM_KEYBOARD_SHORTCUTS = 490;
// Not on any menu: the taskbar thumbnail's side-by-side switch, which flips
// between the Compare menu's Neural and Split rather than naming a mode.
inline constexpr UINT IDM_COMPARE_TOGGLE = 491;

enum class PlayerCommandRoute {
    KeyDown,
    NativeMenu,
};

HMENU CreateMenuBar(const Localizer& localizer, bool youtubeAvailable);
void UpdateRecentVideos(HMENU menuBar, std::span<const std::wstring> titles, bool enabled);
// Playback > Audio track. Empty labels leave a disabled placeholder rather
// than an empty popup; `selected` is the index of the track playing and
// carries the only radio mark.
void UpdateAudioTracks(HMENU menuBar, std::span<const std::wstring> labels, int selected);
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
// The processing-scale percentage a rung's command selects, and back.
std::optional<uint32_t> ProcessingScaleForCommand(UINT command);
UINT CommandForProcessingScale(uint32_t percent);
UINT CommandForYouTubeQuality(YouTubeSourceQuality quality);
bool UpdateYouTubeQualitySelection(HMENU menuBar, YouTubeSourceQuality quality);
// The two toggles carry a checkmark for their active state; frame generation
// does not - the item is a one-shot conversion, and a check on it stated a mode
// the player never has. `frameGenerationRunning` keeps its row live while a
// conversion runs, because the row IS the cancel command then.
bool UpdateFeatureAvailability(HMENU menuBar, bool neuralRequested,
                               bool neuralAvailable, bool neuralActive,
                               bool upscalingAvailable, bool upscalingActive,
                               bool frameGenerationAvailable, bool frameGenerationRunning);
// Range, preview and render commands, the render pause item and the receipt
// item. markersAvailable: a source is loaded; rangeRenderAvailable: that source
// can be range-rendered now (local/cached source, no active job).
// The popup that holds `command`, or null. Exported because a caller that
// relabels a command needs the menu the command lives in.
HMENU FindMenuContainingCommand(HMENU menuBar, UINT command);
// Replaces one command's text in place, keeping its id, position and state.
// A command that reports what it will do next - "Generate frames..." becoming
// "Cancel frame generation" - is one live row where a separate cancel item was
// a permanently greyed one.
bool SetMenuCommandText(HMENU menu, UINT command, const std::wstring& text);
bool UpdateRenderActionAvailability(HMENU menuBar, bool markersAvailable, bool rangeRenderAvailable,
                                    bool jobActive, bool jobPaused, bool receiptAvailable);
// Video > Compare: the mode radio group is enabled only while modesAvailable;
// selectedMode is one of the mode commands (IDM_COMPARE_NEURAL, _ORIGINAL, _SPLIT,
// _WIPE); anything else checks IDM_COMPARE_NEURAL.
// zoomed enables Zoom out and Fit; the zoom steps themselves carry no check.
bool UpdateComparisonMenu(HMENU menuBar, bool modesAvailable, bool zoomAvailable,
                          UINT selectedMode, bool zoomed, bool swapped = false, bool loupe = false,
                          bool differenceLuma = true);
// Video > Compare's mask rows: Load needs a source; Invert, Clear and the feather need
// a mask. featherIndex is the checked position in the feather block.
bool UpdateMaskMenu(HMENU menuBar, bool loadAvailable, bool maskLoaded, bool inverted, UINT featherIndex);
// The second-Mix block: enabled while comparing, `index` checked.
bool UpdateSecondMixMenu(HMENU menuBar, bool available, UINT index);
// CheckMenuRadioItem over the popup that holds `first`, by the POSITIONS of `first`
// and `last`: with MF_BYCOMMAND Windows wants the checked id numerically between
// them, which a group that grew an item past its original ids cannot promise.
bool CheckRadioCommand(HMENU menuBar, UINT first, UINT last, UINT chosen);
// Menu command for a plain-key accelerator of the range, preview, neural
// settings and comparison items (I, O, Shift+I/O, Ctrl+G, F, Shift+F, Ctrl+R,
// Ctrl+N, Ctrl+Shift+S, Z, Shift+Z, [ and ], Shift+[ and Shift+], X, C, Shift+C
// and L); nullopt when the key is not one of them.
std::optional<UINT> CommandForPlayerKey(UINT key, bool controlDown, bool shiftDown);

// One row of the keyboard cheat sheet: the menu it lives in (or "Keyboard"
// for a key no menu names), what it does and the keys, as the menu prints them.
struct ShortcutRow {
    std::wstring group;
    std::wstring action;
    std::wstring keys;

    friend bool operator==(const ShortcutRow&, const ShortcutRow&) = default;
};
// Read from the menu bar itself - every command whose text carries a tab and
// an accelerator - so the sheet cannot say something the menus do not; then
// the keys no menu command names (Tab, Enter, '.', Esc, the wheel...).
std::vector<ShortcutRow> CollectShortcuts(HMENU menuBar, const Localizer& localizer);

} // namespace app_menu
