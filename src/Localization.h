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
            {L"menu.mark_in", L"Mark In\tI"}, {L"menu.mark_out", L"Mark Out\tO"}, {L"menu.clear_marks", L"Clear Marks\tShift+I / Shift+O"},
            {L"menu.goto_timecode", L"Go to timecode...\tCtrl+G"}, {L"menu.pause_neural_render", L"Pause neural render\tSpace"},
            {L"menu.video", L"Video"}, {L"menu.aspectfit", L"Original aspect ratio (Fit)\tA"}, {L"menu.aspectfill", L"Fill without stretching (Crop)"},
            {L"menu.youtube_quality", L"YouTube source quality"}, {L"menu.youtube_quality_auto", L"Auto (1080p preferred)"},
            {L"menu.youtube_quality_2160", L"2160p"}, {L"menu.youtube_quality_1440", L"1440p"}, {L"menu.youtube_quality_1080", L"1080p"},
            {L"menu.adjustments", L"Image adjustments...\tCtrl+E   (Overlay: Ctrl+Alt+C)"},
            {L"menu.compare", L"Compare"}, {L"menu.compare_neural", L"Neural"}, {L"menu.compare_blend", L"Blend"}, {L"menu.compare_split", L"Split"}, {L"menu.compare_wipe", L"Wipe"},
            {L"menu.compare_blend_less", L"Blend less\t["}, {L"menu.compare_blend_more", L"Blend more\t]"}, {L"menu.compare_zoom", L"Zoom 2x\tZ"},
            {L"menu.final", L"Final image\t1"}, {L"menu.input", L"DLSS input\t2"}, {L"menu.mv", L"Motion vectors\t3"},
            {L"menu.depth", L"Depth\t4"}, {L"menu.mask", L"BiasCurrent mask\t5"}, {L"menu.fullscreen", L"Fullscreen\tF11"},
            {L"menu.dlss", L"DLSS"}, {L"menu.neural_rendering", L"Neural Rendering\tD"},
        {L"menu.dlss_upscaling", L"DLSS Upscaling"},
            {L"menu.frame_generation_unavailable", L"Frame Generation\tUnavailable in this build"},
            {L"menu.rehook", L"Recreate NGX / re-hook DLSS 5\tF6"},
            {L"menu.preview_frame", L"Preview this frame (neural)\tF"}, {L"menu.preview_clip", L"Preview 4 s clip (neural)\tShift+F"},
            {L"menu.render_range", L"Render marked range (neural)\tCtrl+R"}, {L"menu.render_whole", L"Render whole video (neural)"},
            {L"menu.neural_settings", L"Neural settings...\tCtrl+N"},
            {L"menu.advanced", L"Advanced"}, {L"menu.safe_mode", L"Restart in DLSS SR safe mode"},
            {L"menu.clear_neural_cache", L"Clear Neural Cache"}, {L"menu.open_receipt", L"Open render receipt"},
            {L"neural.phase.cache", L"Checking saved video"},
            {L"neural.cache.checking", L"Verifying cache; no re-encoding"},
            {L"neural.phase.acquiring", L"Acquiring"}, {L"neural.phase.rendering", L"Neural rendering"},
            {L"neural.phase.encoding", L"Encoding"}, {L"neural.phase.validating", L"Validating"},
            {L"neural.phase.ready", L"Ready"}, {L"neural.cancel", L"Cancel"},
            {L"neural.phase.preflight", L"Probing neural runtime"}, {L"neural.phase.paused", L"Paused"},
            {L"neural.phase.recovering", L"Recovering"},
            {L"neural.failure.gpu-stall", L"The GPU stalled during neural rendering."},
            {L"neural.failure.device-removed", L"The graphics device was removed or reset during neural rendering."},
            {L"neural.failure.worker-crashed", L"The neural render helper crashed."},
            {L"neural.failure.retry-exhausted", L"Neural rendering gave up after retrying the same frame."},
            {L"neural.failure.preflight", L"The neural runtime preflight failed."},
            {L"neural.failure.identity", L"A frame identity mismatch stopped the neural render."},
            {L"neural.failure.protocol", L"The neural render helper stopped responding correctly."},
            {L"neural.view.original", L"Original"}, {L"neural.view.rendered", L"Neural rendered"},
            {L"neural.sync.warning", L"The original and neural-rendered streams are out of sync."},
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
            {L"neural.settings.title", L"Neural settings"}, {L"neural.settings.intensity", L"Intensity"}, {L"neural.settings.structure", L"Local structure"},
            {L"neural.settings.tone", L"Local tone"}, {L"neural.settings.skin", L"Skin structure"}, {L"neural.settings.color", L"Color strength"},
            {L"neural.settings.preset", L"Preset"}, {L"neural.settings.style", L"Style"}, {L"neural.settings.automask", L"Automatic mask"},
            {L"neural.settings.guides", L"Guides"}, {L"neural.settings.guide_mv", L"Motion vectors"}, {L"neural.settings.guide_depth", L"Depth"}, {L"neural.settings.guide_mask", L"Mask"},
            {L"neural.settings.note", L"These change the neural model and require a re-render; playback color adjustments are instant."},
            {L"neural.settings.reset", L"Reset"}, {L"neural.settings.apply", L"Apply && re-render"}, {L"neural.settings.close", L"Close"},
            {L"timecode.title", L"Go to timecode"}, {L"timecode.label", L"Timecode (hh:mm:ss:ff, h:mm:ss.mmm or f<frame>)"},
            {L"timecode.go", L"Go"}, {L"timecode.set_in", L"Set In"}, {L"timecode.set_out", L"Set Out"}, {L"timecode.cancel", L"Cancel"},
            {L"timecode.invalid", L"Enter a timecode such as 00:01:23:04, 1:23.500 or f2000."},
            {L"range.invalid", L"Mark In (I) and Out (O) with In before Out, then render the marked range."},
            {L"idle.title", L"Play a video with DLSS"}, {L"idle.subtitle", L"Drop a file here or choose a source"}, {L"idle.open", L"Open file"},
            {L"idle.youtube", L"Open YouTube URL"}, {L"idle.youtube_unavailable", L"Unavailable in this build; YouTube support is not installed yet."},
            {L"idle.youtube_unavailable_compact", L"YouTube unavailable in this build."},
            {L"dialog.title", L"Open photo, GIF or video"}, {L"dialog.all_ffmpeg", L"All files (FFmpeg auto-detect)"}, {L"dialog.supported", L"Photos, GIFs and videos"}, {L"dialog.all", L"All files"},
            {L"youtube.dialog.title", L"Open YouTube video"}, {L"youtube.dialog.url", L"YouTube URL"},
            {L"youtube.dialog.paste", L"Paste"}, {L"youtube.dialog.play", L"Play"}, {L"youtube.dialog.cancel", L"Cancel"},
            {L"youtube.dialog.note", L"Public, non-DRM videos only."},
            {L"youtube.dialog.invalid", L"Enter a supported youtube.com or youtu.be video URL."},
            {L"youtube.error.invalid", L"That is not a supported YouTube video URL. Check the address and try again."},
            {L"youtube.error.helper_missing", L"YouTube support files are missing beside the app. Reinstall the complete package and try again."},
            {L"youtube.error.start_failed", L"The YouTube resolver could not start. Close other copies of the player and try again."},
            {L"youtube.error.extraction", L"A playable stream could not be extracted. The video may be private, DRM-protected, or temporarily unavailable."},
            {L"youtube.error.timeout", L"YouTube took too long to respond. Check your connection and try again."},
            {L"youtube.error.cancelled", L"YouTube resolution was cancelled."},
            {L"youtube.error.ffmpeg", L"FFmpeg could not open this YouTube stream. Try another public, non-DRM video."},
            {L"youtube.error.media_timeout", L"The YouTube stream did not become ready within 20 seconds. Check your connection and try again."},
            {L"youtube.error.media_stalled", L"The YouTube stream stopped delivering video for 15 seconds. Check your connection and try again."},
            {L"error.decode", L"Could not open this media. Check the file and bundled FFmpeg files, then try again."},
            {L"error.renderer", L"DLSS is unavailable. Update the NVIDIA driver or use safe mode, then try again."},
            {L"error.frame", L"No video frame could be decoded. Try another file."},
            {L"error.seek", L"Could not seek. Try restarting playback."},
            {L"status.muted", L"Muted"}, {L"status.volume", L"Vol"}, {L"status.seeking", L"Seeking\u2026"},
            {L"status.render_hint", L"Mark I/O, then Ctrl+R renders the marked range"},
            {L"status.preparing_source", L"Downloading this source for rendering\u2026"}
        };
        return strings;
    }
};
