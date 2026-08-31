#pragma once

#include <string>
#include <unordered_map>

class Localizer {
public:
    void Initialize() {}

    std::wstring Get(const wchar_t* key) const {
        const auto& strings = EnglishDefaults();
        const auto it = strings.find(key);
        if (it != strings.end()) return it->second;
        return key ? std::wstring(key) : std::wstring();
    }

private:
    using Map = std::unordered_map<std::wstring, std::wstring>;

    static const Map& EnglishDefaults() {
        static const Map strings = {
            {L"app.title", L"DLSS Video Player"},
            {L"menu.file", L"File"}, {L"menu.open", L"Open file\tCtrl+O"}, {L"menu.open_youtube", L"Open YouTube URL\u2026\tCtrl+L"}, {L"menu.exit", L"Exit"},
            {L"menu.playback", L"Playback"}, {L"menu.playpause", L"Play / Pause\tSpace   (Overlay: Ctrl+Alt+Space)"}, {L"menu.stop", L"Stop\tS"},
            {L"menu.back10", L"Back 10 s\tLeft"}, {L"menu.forward10", L"Forward 10 s\tRight"}, {L"menu.mute", L"Mute\tM"},
            {L"menu.video", L"Video"}, {L"menu.aspectfit", L"Original aspect ratio (Fit)\tA"}, {L"menu.aspectfill", L"Fill without stretching (Crop)"},
            {L"menu.adjustments", L"Image adjustments...\tCtrl+E   (Overlay: Ctrl+Alt+C)"},
            {L"menu.final", L"Final image\t1"}, {L"menu.input", L"DLSS input\t2"}, {L"menu.mv", L"Motion vectors\t3"},
            {L"menu.depth", L"Depth\t4"}, {L"menu.mask", L"BiasCurrent mask\t5"}, {L"menu.fullscreen", L"Fullscreen\tF11"},
            {L"menu.dlss", L"DLSS"}, {L"menu.dlss_toggle", L"Enable DLSS\tD"}, {L"menu.rehook", L"Recreate NGX / re-hook DLSS 5\tF6"},
            {L"menu.depthmode", L"Estimated / flat depth proxy\tG"}, {L"menu.quality", L"Mode / quality"}, {L"menu.quality_auto", L"Auto (realtime recommended)"},
            {L"menu.advanced", L"Advanced"}, {L"menu.safe_mode", L"Restart in DLSS SR safe mode"},
            {L"safe_mode.confirm", L"Restart the player in DLSS SR safe mode?\n\nThis disables the experimental neural add-on for this launch."},
            {L"safe_mode.launch_failed", L"The player could not restart in DLSS SR safe mode.\n\nSee DLSSVideoPlayer.log for details."},
            {L"rehook.title", L"Recreate DLSS"}, {L"rehook.confirm", L"Recreate DLSS now?\n\nRenderer recreation can reset playback or hang while the experimental neural add-on is active."},
            {L"button.open", L"Open"}, {L"button.pause", L"Pause"}, {L"button.play", L"Play"}, {L"button.stop", L"Stop"},
            {L"button.mute", L"Mute"}, {L"button.sound", L"Sound"}, {L"button.aspect", L"Aspect"}, {L"button.crop", L"Crop"},
            {L"button.rehook", L"Re-hook"}, {L"button.color", L"Color"}, {L"button.full", L"Full"},
            {L"adjustments.title", L"Image adjustments"}, {L"adjustments.brightness", L"Brightness"}, {L"adjustments.contrast", L"Contrast"},
            {L"adjustments.saturation", L"Saturation"}, {L"adjustments.gamma", L"Gamma"}, {L"adjustments.temperature", L"Temperature"},
            {L"adjustments.tint", L"Tint"}, {L"adjustments.note", L"Adjustments are applied after DLSS to the final video image. Debug views remain unmodified."},
            {L"adjustments.reset", L"Reset"}, {L"adjustments.close", L"Close"},
            {L"idle.title", L"Play a video with DLSS"}, {L"idle.subtitle", L"Drop a file here or choose a source"}, {L"idle.open", L"Open file"},
            {L"idle.youtube", L"Open YouTube URL"}, {L"idle.youtube_unavailable", L"Unavailable in this build; YouTube support is not installed yet."},
            {L"dialog.title", L"Open video"}, {L"dialog.all_ffmpeg", L"All files (FFmpeg auto-detect)"}, {L"dialog.supported", L"Common video files"}, {L"dialog.all", L"All files"},
            {L"error.decode", L"Could not open this video. Check the file and bundled FFmpeg files, then try again."},
            {L"error.renderer", L"DLSS is unavailable. Update the NVIDIA driver or use safe mode, then try again."},
            {L"error.frame", L"No video frame could be decoded. Try another file."},
            {L"error.seek", L"Could not seek. Try restarting playback."},
            {L"status.muted", L"Muted"}, {L"status.volume", L"Vol"}, {L"status.seeking", L"Seeking\u2026"}
        };
        return strings;
    }
};
