#pragma once

#include <windows.h>

#include <optional>

#include "ExampleVideos.h"

class Localizer;
enum class YouTubeSourceQuality;

namespace app_menu {

inline constexpr UINT IDM_OPEN = 100;
inline constexpr UINT IDM_EXIT = 101;
inline constexpr UINT IDM_OPEN_YOUTUBE = 102;
inline constexpr UINT IDM_EXAMPLE_VIDEO_FIRST = 110;
inline constexpr UINT IDM_PLAY = 200;
inline constexpr UINT IDM_STOP = 201;
inline constexpr UINT IDM_BACK10 = 202;
inline constexpr UINT IDM_FWD10 = 203;
inline constexpr UINT IDM_MUTE = 204;
inline constexpr UINT IDM_DLSS = 300;
inline constexpr UINT IDM_REHOOK = 301;
inline constexpr UINT IDM_VIEW_FINAL = 302;
inline constexpr UINT IDM_VIEW_INPUT = 303;
inline constexpr UINT IDM_VIEW_MV = 304;
inline constexpr UINT IDM_VIEW_DEPTH = 305;
inline constexpr UINT IDM_VIEW_MASK = 306;
inline constexpr UINT IDM_DEPTH_MODE = 307;
inline constexpr UINT IDM_QUALITY_AUTO = 330;
inline constexpr UINT IDM_QUALITY_QUALITY = 331;
inline constexpr UINT IDM_QUALITY_BALANCED = 332;
inline constexpr UINT IDM_QUALITY_PERFORMANCE = 333;
inline constexpr UINT IDM_QUALITY_ULTRAPERF = 334;
inline constexpr UINT IDM_QUALITY_DLAA = 335;
inline constexpr UINT IDM_ASPECT_FIT = 400;
inline constexpr UINT IDM_ASPECT_FILL = 401;
inline constexpr UINT IDM_FULLSCREEN = 402;
inline constexpr UINT IDM_VIDEO_ADJUSTMENTS = 403;
inline constexpr UINT IDM_YOUTUBE_QUALITY_AUTO = 410;
inline constexpr UINT IDM_YOUTUBE_QUALITY_2160 = 411;
inline constexpr UINT IDM_YOUTUBE_QUALITY_1440 = 412;
inline constexpr UINT IDM_YOUTUBE_QUALITY_1080 = 413;
inline constexpr UINT IDM_YOUTUBE_QUALITY_720 = 414;
inline constexpr UINT IDM_YOUTUBE_QUALITY_480 = 415;
inline constexpr UINT IDM_ADVANCED_SAFE_MODE = 450;

enum class PlayerCommandRoute {
    KeyDown,
    NativeMenu,
};

HMENU CreateMenuBar(const Localizer& localizer, bool youtubeAvailable);
HMENU CreateDebugViewMenu(UINT selectedCommand);
bool RoutesToRehook(PlayerCommandRoute route, UINT value);
bool RoutesToOpenYouTube(PlayerCommandRoute route, UINT value, bool controlDown);
const ExampleVideo* ExampleVideoForCommand(UINT command);
bool UpdateSourceActionAvailability(HMENU menuBar, bool openEnabled,
                                    bool youtubeEnabled);
std::optional<YouTubeSourceQuality> YouTubeQualityForCommand(UINT command);
UINT CommandForYouTubeQuality(YouTubeSourceQuality quality);
bool UpdateYouTubeQualitySelection(HMENU menuBar, YouTubeSourceQuality quality);

} // namespace app_menu
