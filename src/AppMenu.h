#pragma once

#include <windows.h>

#include <optional>
#include <span>
#include <string>

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
inline constexpr UINT IDM_NEURAL_RENDERING = 300;
inline constexpr UINT IDM_REHOOK = 301;
inline constexpr UINT IDM_VIEW_FINAL = 302;
inline constexpr UINT IDM_VIEW_INPUT = 303;
inline constexpr UINT IDM_VIEW_MV = 304;
inline constexpr UINT IDM_VIEW_DEPTH = 305;
inline constexpr UINT IDM_VIEW_MASK = 306;
inline constexpr UINT IDM_DEPTH_MODE = 307;
inline constexpr UINT IDM_DLSS_UPSCALING = 308;
inline constexpr UINT IDM_FRAME_GENERATION = 309;
inline constexpr UINT IDM_UPSCALE_1440 = 336;
inline constexpr UINT IDM_UPSCALE_2160 = 337;
inline constexpr UINT IDM_ASPECT_FIT = 400;
inline constexpr UINT IDM_ASPECT_FILL = 401;
inline constexpr UINT IDM_FULLSCREEN = 402;
inline constexpr UINT IDM_VIDEO_ADJUSTMENTS = 403;
inline constexpr UINT IDM_YOUTUBE_QUALITY_AUTO = 410;
inline constexpr UINT IDM_YOUTUBE_QUALITY_2160 = 411;
inline constexpr UINT IDM_YOUTUBE_QUALITY_1440 = 412;
inline constexpr UINT IDM_YOUTUBE_QUALITY_1080 = 413;
inline constexpr UINT IDM_ADVANCED_SAFE_MODE = 450;
inline constexpr UINT IDM_CLEAR_NEURAL_CACHE = 451;

enum class PlayerCommandRoute {
    KeyDown,
    NativeMenu,
};

HMENU CreateMenuBar(const Localizer& localizer, bool youtubeAvailable);
void UpdateRecentVideos(HMENU menuBar, std::span<const std::wstring> titles, bool enabled);
HMENU CreateDebugViewMenu(UINT selectedCommand);
bool RoutesToRehook(PlayerCommandRoute route, UINT value);
bool RoutesToOpenYouTube(PlayerCommandRoute route, UINT value, bool controlDown);
const ExampleVideo* ExampleVideoForCommand(UINT command);
bool UpdateSourceActionAvailability(HMENU menuBar, bool openEnabled,
                                    bool youtubeEnabled);
std::optional<YouTubeSourceQuality> YouTubeQualityForCommand(UINT command);
UINT CommandForYouTubeQuality(YouTubeSourceQuality quality);
bool UpdateYouTubeQualitySelection(HMENU menuBar, YouTubeSourceQuality quality);
bool UpdateFeatureAvailability(HMENU menuBar, bool neuralRequested,
                               bool neuralAvailable, bool neuralActive,
                               bool upscalingAvailable, bool upscalingActive,
                               bool frameGenerationAvailable, bool frameGenerationActive);

} // namespace app_menu
