#pragma once

#include <string>
#include <unordered_map>

class Localizer {
public:
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
            {L"menu.youtube_quality", L"YouTube source quality"}, {L"menu.youtube_quality_auto", L"Auto (up to 1440p, highest bitrate)"},
            {L"menu.youtube_quality_2160", L"2160p"}, {L"menu.youtube_quality_1440", L"1440p"}, {L"menu.youtube_quality_1080", L"1080p"},
            {L"menu.adjustments", L"Image adjustments...\tCtrl+E   (Overlay: Ctrl+Alt+C)"},
            {L"menu.compare", L"Compare"}, {L"menu.compare_neural", L"Neural"}, {L"menu.compare_blend", L"Blend"}, {L"menu.compare_split", L"Split"}, {L"menu.compare_wipe", L"Wipe"},
            {L"menu.compare_blend_less", L"Blend less\t["}, {L"menu.compare_blend_more", L"Blend more\t]"}, {L"menu.compare_zoom", L"Zoom 2x\tZ"},
            {L"menu.final", L"Final image\t1"}, {L"menu.input", L"DLSS input\t2"}, {L"menu.mv", L"Motion vectors\t3"},
            {L"menu.depth", L"Depth\t4"}, {L"menu.fullscreen", L"Fullscreen\tF11"},
            {L"menu.dlss", L"DLSS"}, {L"menu.neural_rendering", L"Neural Rendering\tD"},
        {L"menu.dlss_upscaling", L"DLSS Upscaling"},
            {L"menu.frame_generation_unavailable", L"Frame Generation\tUnavailable in this build"},
            {L"menu.rehook", L"Recreate NGX / re-hook DLSS 5\tF6"},
            {L"menu.preview_frame", L"Preview this frame (neural)\tF"}, {L"menu.preview_clip", L"Preview 4 s clip (neural)\tShift+F"},
            {L"menu.convert", L"Convert && save"},
            {L"menu.render_range", L"Convert marked clip to neural video\tCtrl+R"}, {L"menu.render_whole", L"Convert whole video to neural video"},
            {L"menu.export_cached", L"Save converted video..."}, {L"menu.cancel_export", L"Cancel saving"},
            {L"menu.neural_settings", L"Neural settings...\tCtrl+N"},
            {L"menu.encoder_settings", L"Encoder settings..."},
            {L"menu.advanced", L"Advanced"}, {L"menu.safe_mode", L"Restart in DLSS SR safe mode"},
            {L"menu.clear_neural_cache", L"Clear Neural Cache"}, {L"menu.open_receipt", L"Open render receipt"},
            {L"menu.check_updates", L"Check for updates"},
            // The badge sits right-justified in the menu bar; the arrow reads as
            // "newer than this build" without needing colour, which a native
            // menu bar does not give us.
            {L"update.badge", L"\u2191 Update "},
            {L"update.available", L"A newer release is available: "},
            {L"update.open_question", L"\n\nOpen the GitHub releases page?"},
            {L"update.up_to_date", L"This is the latest release: "},
            {L"update.unreachable", L"The update check could not reach GitHub. Check the connection and try again."},
            {L"driver.below_floor", L"Neural rendering needs a newer NVIDIA driver."},
            {L"driver.detected", L"Detected driver: "},
            {L"driver.minimum", L"Minimum for neural rendering: "},
            {L"driver.verified", L"Verified for this build: "},
            {L"neural.phase.cache", L"Checking saved video"},
            {L"neural.cache.checking", L"Verifying cache; no re-encoding"},
            // The cause is appended to the staging sentence, and the
            // filesystem error number with it when there was one; the log line
            // beside it names the exact directory that was refused.
            {L"cache.staging_failed", L"Neural cache staging could not be created in %s: %s"},
            {L"cache.not_writable", L"The neural cache is not writable at %s. Move the player to a folder you can write to, or free space in LocalAppData."},
            {L"cache.cause.unwritable", L"the folder cannot be written to"},
            {L"cache.cause.invalid_key", L"the cache key was rejected"},
            {L"cache.cause.create_failed", L"the folder could not be created"},
            {L"cache.cause.exists", L"a staging folder of that name is already there"},
            {L"cache.cause.outside_root", L"the folder resolved outside the cache"},
            {L"neural.phase.acquiring", L"Acquiring"}, {L"neural.phase.rendering", L"Neural rendering"},
            {L"neural.phase.encoding", L"Encoding"}, {L"neural.phase.validating", L"Validating"},
            {L"neural.phase.ready", L"Ready"}, {L"neural.cancel", L"Cancel"},
            {L"neural.phase.preflight", L"Probing neural runtime"}, {L"neural.phase.paused", L"Paused"},
            {L"neural.phase.recovering", L"Recovering"},
            {L"neural.live.title", L"Neural rendering from here"},
            {L"neural.live.buffering", L"Buffering neural frames"},
            {L"neural.live.lead", L"buffered"},
            // Which way the play press that arrived during buffering points: the
            // button state alone was invisible, so a second press cancelled it.
            {L"neural.live.stalled", L"Live rendering stopped: the render never reached this frame. Playing the original."},
            {L"neural.live.will_play", L"starts when the buffer fills"},
            {L"neural.live.will_stay_paused", L"stays paused when the buffer fills"},
            {L"neural.live.slow", L"This video is %ux%u at %.6g fps. On this GPU neural rendering runs at about %.3g frames per second, which is %.2gx real time, so watching it live would pause to buffer almost continuously.\n\nConvert the clip instead (DLSS > Convert & save) to watch it smoothly afterwards.\n\nStart the live session anyway?"},
            {L"neural.preview.title", L"Previewing neural settings"},
            {L"neural.preview.detail", L"Rendering this frame with the new settings"},
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
            // The toolbar builds its own labels from state (Play/Pause, Mute/Sound,
            // Fit/Fill); only the idle surface's Open button reads a key.
            {L"button.open", L"Open"},
            {L"adjustments.title", L"Image adjustments"}, {L"adjustments.brightness", L"Brightness"}, {L"adjustments.contrast", L"Contrast"},
            {L"adjustments.saturation", L"Saturation"}, {L"adjustments.gamma", L"Gamma"}, {L"adjustments.temperature", L"Temperature"},
            {L"adjustments.tint", L"Tint"},
            {L"adjustments.neural_strength", L"Neural strength"},
            {L"adjustments.neural_strength.tip", L"How much of the neural result you see: below 100 percent mixes back toward the original, above 100 percent extends the model's own luminance change, and either way it only re-composes the frame already on screen, so it costs no re-render."},
            {L"adjustments.note", L"Adjustments are applied after DLSS to the final video image. Debug views remain unmodified."},
            {L"adjustments.reset", L"Reset"}, {L"adjustments.close", L"Close"},
            {L"neural.settings.title", L"Neural settings"}, {L"neural.settings.intensity", L"Intensity"}, {L"neural.settings.structure", L"Local structure"},
            {L"neural.settings.tone", L"Local tone"}, {L"neural.settings.skin", L"Skin structure"},
            {L"neural.settings.style", L"Style"}, {L"neural.settings.automask", L"Automatic mask"},
            {L"neural.settings.guides", L"Guides"}, {L"neural.settings.guide_mv", L"Motion vectors"}, {L"neural.settings.guide_depth", L"Depth"},
            {L"neural.settings.note", L"These change the neural model: they apply to the paused preview and to the next conversion; playback color adjustments are instant."},
            {L"encoder.settings.title", L"Encoder settings"},
            {L"encoder.settings.gpu_convert", L"GPU color conversion"},
            {L"encoder.settings.gpu_source", L"GPU source conversion"},
            {L"encoder.settings.nvenc_preset", L"NVENC preset"},
            {L"encoder.settings.note", L"These only affect how the next neural render is decoded and encoded; the rendered image itself is unchanged. Nothing has to be re-rendered."},
            {L"encoder.settings.reset", L"Reset"}, {L"encoder.settings.close", L"Close"},
            // Tooltips. They describe the control and, where it was measured,
            // what it demonstrably does; they never promise a direction that has
            // not been verified on hardware.
            {L"neural.tip.intensity", L"Overall strength of the neural effect. 0.00 leaves the decoded frame alone, 1.00 is the model default, 2.00 is the maximum.\nBy far the strongest control here: it moves several times more of the picture than the others."},
            {L"neural.tip.structure", L"Strength of the model's local-detail term (default 1.00).\nIt measurably changes the frame; the direction depends on the material, so judge it on the preview."},
            {L"neural.tip.tone", L"Strength of the model's local brightness and shading term (default 1.00).\nSecond-largest effect after Intensity."},
            {L"neural.tip.skin", L"Separate structure term for skin, from -1.00 to +1.00 (default -1.00).\nChanges the frame; NVIDIA documents no scale, so compare faces in the preview."},
            {L"neural.tip.style", L"Which look the model is asked for. Changing it rebuilds the neural feature, so the next render starts a fresh temporal history."},
            {L"neural.tip.automask", L"Lets the model choose regions to leave untouched.\nTurning it off measurably changes the frame."},
            {L"encoder.tip.gpu_convert", L"Converts each rendered frame to NV12 on the GPU instead of letting the encoder convert on the CPU. Measured with the neural pass running, the GPU is the scarce resource and the CPU path was slightly faster, so this is off by default; turn it on for a GPU with headroom."},
            {L"encoder.tip.gpu_source", L"Decodes the source to NV12 and converts it to BGRA on the GPU instead of letting ffmpeg convert on the CPU. Turn it off to compare when the GPU is the bottleneck. Applies to the next render."},
            {L"encoder.tip.nvenc_preset", L"Speed/quality preset for the hevc_nvenc encoder used by neural renders. p7 is the slowest and best; p5 roughly doubles encoder throughput at a small quality cost. Applies to the next render."},
            {L"neural.tip.guide_mv", L"Sends this player's estimated motion to the model so it can reuse the previous frame.\nVideo carries no real motion vectors, so these are estimated per frame and rejected where the estimate is not trustworthy."},
            {L"neural.tip.guide_depth", L"Sends this player's estimated depth proxy.\nMeasured: it only changes the image while Motion vectors is on - with motion off, depth makes no difference."},
            {L"neural.tip.apply", L"Applies these settings to what is on screen: an active session restarts at the playhead, a paused frame is rendered again.\nEvery distinct combination is rendered from scratch - about 10 s for one 1080p frame - and repeats come back from cache."},
            {L"neural.tip.reset", L"Returns every control to the model defaults and previews the paused frame again."},
            {L"neural.settings.reset", L"Reset"}, {L"neural.settings.apply", L"Apply"}, {L"neural.settings.close", L"Close"},
            {L"export.settings_changed", L"The converted video on disk was rendered with different neural settings.\n\nConvert this range again with the settings you selected?"},
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
            // What the resolver actually got, said out loud. A silent 360p
            // fallback on an age-restricted video is indistinguishable from a
            // bad render otherwise.
            {L"youtube.source.low", L"Source is only %dp at %.1f Mbps"},
            {L"youtube.source.low.signin", L"Source is only %dp at %.1f Mbps: this video is age-restricted, and YouTube serves higher quality only to a signed-in session"},
            {L"youtube.source.low_height", L"Source is only %dp: this copy was acquired at that height, so pick a YouTube source quality to fetch it again"},
            {L"error.decode", L"Could not open this media. Check the file and bundled FFmpeg files, then try again."},
            {L"error.renderer", L"DLSS is unavailable. Update the NVIDIA driver or use safe mode, then try again."},
            {L"error.frame", L"No video frame could be decoded. Try another file."},
            {L"status.muted", L"Muted"}, {L"status.volume", L"Vol"}, {L"status.seeking", L"Seeking\u2026"},
            {L"status.render_hint", L"Mark I/O, then Ctrl+R renders the marked range"},
            {L"status.preparing_source", L"Downloading this source for rendering\u2026"}
        };
        return strings;
    }
};
