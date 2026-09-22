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
            {L"menu.back10", L"Back 10 s\tLeft"}, {L"menu.forward10", L"Forward 10 s\tRight"}, {L"menu.mute", L"Mute\tM"}, {L"menu.audio_track", L"Audio track"},
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
            {L"menu.upscale_output", L"Upscaling output"},
            {L"menu.upscale_auto", L"Auto (match display)"},
            {L"menu.upscale_1080", L"1080p"}, {L"menu.upscale_1440", L"1440p"}, {L"menu.upscale_2160", L"2160p (4K)"},
            {L"menu.frame_generation", L"Generate frames (higher frame rate)..."},
            {L"menu.cancel_frame_generation", L"Cancel frame generation"},
            {L"menu.show_framegen_output", L"Show converted file"},
            // 2x is the default because a conversion costs minutes of GPU work
            // and a large file; the rest are opt-in, and "as many as the
            // display allows" is what the first version did automatically.
            {L"menu.framegen_multiple", L"Generated frames"},
            {L"menu.framegen_2x", L"2\u00d7 the frame rate (default)"},
            {L"menu.framegen_3x", L"3\u00d7 the frame rate"},
            {L"menu.framegen_4x", L"4\u00d7 the frame rate"},
            {L"menu.framegen_5x", L"5\u00d7 the frame rate"},
            {L"menu.framegen_max", L"As many as the display allows"},
            // A checkbox under the multiples. Off by default; FrameRatePolicy.h
            // carries the measurement that made it a setting instead of a rule.
            {L"menu.framegen_even_only", L"Even cadence only"},
            {L"framegen.refusal.preference", L"This display cannot show %u\u00d7 this video's frame rate evenly, but it can show %u\u00d7.\n\nRaise DLSS > Generated frames to convert it."},
            {L"framegen.refusal.preference.short", L"set to %u\u00d7, needs %u\u00d7"},
            // Frame generation names ONE feature on every surface. Before this
            // block the menu called it "Generate frames", the toolbar pill
            // called it "Frame Generation" and the status line called it "FG" -
            // three names for one feature in a single screenshot, four while it
            // ran. The control is "Frame Generation"; "Generate frames" is only
            // the verb on the action item.
            {L"framegen.title", L"Frame Generation"},
            {L"framegen.pill.generate", L"Frame Generation \u00b7 Generate"},
            {L"framegen.pill.cancel", L"Frame Generation \u00b7 Cancel"},
            {L"framegen.pill.busy", L"Frame Generation \u00b7 Busy"},
            {L"framegen.pill.unavailable", L"Frame Generation \u00b7 Unavailable"},
            {L"framegen.status.ready", L"Frame Generation ready "},
            // Ready in every way the player can check without the GPU; the
            // runtime's own admission is measured inside the first conversion,
            // because a second NGX feature create beside the renderer's own
            // froze presentation when it was tried in the background. No
            // multiple is promised here, because the cap could still narrow it.
            {L"framegen.status.ready_unchecked", L"Frame Generation ready"},
            {L"framegen.status.busy", L"Frame Generation waits for the current job"},
            // Printed with the refusal's short form in brackets, so the line
            // reads "Frame Generation off (already lands evenly)".
            {L"framegen.status.off", L"Frame Generation off"},
            {L"framegen.status.generating", L"Generating frames"},
            {L"framegen.status.stopping", L"Stopping frame generation"},
            // The streaming case. It is an OFFER, not a refusal: the pass
            // converts a file, the player can keep one with the same
            // acquisition a render of a stream uses, and playback continues
            // while it downloads.
            {L"framegen.pill.get_copy", L"Frame Generation \u00b7 Get a copy"},
            {L"framegen.pill.copying", L"Frame Generation \u00b7 Copying"},
            {L"framegen.status.needs_copy", L"Frame Generation needs a local copy of this stream"},
            {L"framegen.status.copying", L"Frame Generation is keeping a local copy of this stream"},
            {L"framegen.needs_copy", L"This video is streaming, and frame generation converts a file.\n\nKeep a local copy of it now? It goes into the player's cache, playback carries on while it downloads, and Generate frames becomes available as soon as it finishes."},
            {L"framegen.copy_started", L"Keeping a local copy of this video. The status line shows it while it runs; Generate frames becomes available when it finishes."},
            {L"framegen.copy_failed", L"A local copy of this video could not be started. Try again once playback is running, or open a local file."},
            {L"framegen.status.cancel_hint", L"Cancel: DLSS > Cancel frame generation"},
            // Refusals. The long form is what a dialog shows; the short form is
            // what fits the status line. The status line used to print the log
            // slug ("no-even-multiple"), which is a developer identifier in a
            // user's face while a written sentence sat unused beside it.
            {L"framegen.refusal.unknown_rate", L"This video does not report a frame rate, so there is no interval to generate inside."},
            {L"framegen.refusal.unknown_rate.short", L"no frame rate"},
            {L"framegen.refusal.still_image", L"A still image has no second frame to generate between."},
            {L"framegen.refusal.still_image.short", L"still image"},
            {L"framegen.refusal.variable_rate", L"This video's frame rate varies, and generated frames need a constant one."},
            {L"framegen.refusal.variable_rate.short", L"variable frame rate"},
            {L"framegen.refusal.unknown_refresh", L"Windows did not report a refresh rate for this display."},
            {L"framegen.refusal.unknown_refresh.short", L"no display refresh"},
            {L"framegen.refusal.meets_refresh", L"This video already runs at or above what this display can show, so generated frames would never be presented."},
            {L"framegen.refusal.meets_refresh.short", L"already matches the display"},
            {L"framegen.refusal.below_double", L"This display cannot show even twice this video's frame rate, so there is no multiple to generate."},
            {L"framegen.refusal.below_double.short", L"display cannot double it"},
            // The one cadence trade this player refuses: the video is already
            // scanned out the same number of times per frame, and no multiple
            // that fits divides this refresh, so generating would land the
            // frames unevenly where the video does not.
            {L"framegen.refusal.source_even", L"This video already lands evenly on this display - every frame is scanned out the same number of times - and no higher multiple divides this refresh, so generated frames would land unevenly where this video does not."},
            {L"framegen.refusal.source_even.short", L"already lands evenly"},
            // Uneven, admissible, and withheld by the setting: the generated
            // frames would land in the grid the video is already playing in,
            // which is why this is an offer rather than a rule.
            {L"framegen.refusal.even_only", L"No whole multiple of this video's frame rate divides this display's refresh, and Even cadence only is on.\n\nTurning it off in DLSS > Generated frames converts it anyway: the generated frames land in the same uneven grid this video already plays in, and arrive twice as often."},
            {L"framegen.refusal.even_only.short", L"even cadence only"},
            // The display-side answer, offered as the refusal dialog's Yes.
            // %s Hz, %u the multiple, %s the rate it reaches, %s Hz again.
            {L"framegen.mode_switch", L"\n\nThis monitor also has a %s Hz mode, where %u\u00d7 this video's frame rate lands on the refresh exactly: %s fps.\n\nSwitch this display to %s Hz? Nothing is converted yet - ask for Generate frames again once the display has changed. Windows keeps the new mode until you change it back."},
            {L"framegen.mode_switch_failed", L"Windows refused the display mode change. Nothing was changed and your video is still playing."},
            {L"framegen.refusal.runtime", L"This GPU and driver admit no generated frames."},
            {L"framegen.refusal.runtime.short", L"refused by the driver"},
            {L"framegen.refusal.no_local_copy.short", L"needs a local copy"},
            {L"framegen.refusal.unchanged", L"\n\nNothing was changed and your video is still playing."},
            {L"framegen.driver_next_step", L"\n\nUpdate the NVIDIA driver and try again."},
            // %u is the multiplier, then the source and target rates as text so
            // one formatter renders every rate the user sees.
            {L"framegen.confirm", L"Generate %u\u00d7 the frames of this video: %s fps \u2192 %s fps.\n\nThe converted file is kept with the player's converted videos; DLSS > Show converted file opens it. Playback switches to it when the conversion finishes.\n\nStart the conversion?"},
            // Appended when the generated rate does not divide the refresh. The
            // two holds are one scan-out apart, which is the same unevenness
            // the source is already being shown with - see FrameRatePolicy.h.
            {L"framegen.confirm.uneven", L"\n\nEach frame is shown for %u or %u refreshes of this display, which is the unevenness this video already plays with."},
            {L"framegen.confirm.neural", L"\n\nThe neural render on screen is what will be converted, not the original - upscale first, generate frames on the result, which is the order NVIDIA’s own pipeline uses. Turn Neural Rendering off first to generate frames from the original instead."},
            {L"framegen.exists", L"This video has already been converted:\n%s\n\nPlay that file instead of converting again?\n\nYes plays it. No converts again and replaces it."},
            {L"framegen.worker_failed", L"The conversion could not start. Try again."},
            {L"export.worker_failed", L"Saving could not start. Try again."},
            {L"framegen.cache_failed", L"The converted file could not be created where the player keeps its converted videos. Check that the cache folder is writable."},
            {L"framegen.failed", L"The conversion did not finish, so nothing was written and your video is unchanged.\n\nSee DLSSVideoPlayer.log for what the runtime reported."},
            {L"framegen.finished_elsewhere", L"The conversion finished for a video you have since left, so playback was left alone.\n\nDLSS > Show converted file opens the new file."},
            {L"framegen.reveal_failed", L"The converted file could not be shown. It may have been moved or deleted."},
            {L"export.title.failed", L"Save failed"},
            {L"export.title.complete", L"Save complete"},
            {L"menu.rehook", L"Recreate NGX / re-hook DLSS 5\tF6"},
            {L"menu.preview_frame", L"Preview this frame (neural)\tF"}, {L"menu.preview_clip", L"Preview 4 s clip (neural)\tShift+F"},
            {L"menu.convert", L"Convert && export"},
            {L"menu.render_range", L"Convert marked clip to neural video\tCtrl+R"}, {L"menu.render_whole", L"Convert whole video to neural video"},
            {L"menu.export_cached", L"Save converted video..."},
            {L"menu.export_stages", L"Export with DLSS stages...\tCtrl+S"},
            {L"export.stages.title", L"Export with DLSS stages"},
            {L"export.stages.upscale", L"DLSS Super Resolution"},
            {L"export.stages.neural", L"Neural rendering"},
            {L"export.stages.framegen", L"Frame generation"},
            {L"export.stages.resolution", L"Output height"},
            {L"export.stages.multiplier", L"Frame rate"},
            {L"export.stages.run", L"Export..."},
            {L"export.stages.close", L"Close"},
            {L"export.stages.note", L"Stages run in NVIDIA's order: Super Resolution, then neural rendering on the upscaled frame, then frame generation on the result."},
            {L"export.stages.group_stages", L"Stages, in the order they run"},
            {L"export.stages.group_result", L"Result"},
            {L"export.progress.title", L"Exporting with DLSS stages"},
            {L"export.progress.pass_sr_neural", L"Super Resolution and neural rendering"},
            {L"export.progress.pass_sr", L"Super Resolution"},
            {L"export.progress.pass_neural", L"Neural rendering"},
            {L"export.progress.pass_framegen", L"Frame generation"},
            {L"export.progress.writing", L"Writing the finished file"},
            {L"export.stages.refusal.nothing", L"Choose at least one stage."},
            {L"export.stages.refusal.geometry", L"This source reports no usable geometry."},
            {L"export.stages.refusal.target", L"This source is already at or above that height, so there is nothing to upscale."},
            {L"export.stages.refusal.multiplier", L"This GPU's runtime does not admit that frame rate."},
            {L"export.stages.refusal.still", L"A still image has no successor frame to generate toward."},
            {L"export.stages.refusal.upscale_needs_neural", L"Super Resolution on its own is not available yet - the neural pass runs with it either way. Tick Neural rendering as well."},
            {L"export.stages.busy", L"Another conversion is running."},
            {L"export.stages.no_source", L"Open a local video first. A stream has to finish copying before it can be exported."},
            {L"export.tip.upscale", L"Runs DLSS Super Resolution into the file. Off, the render stays at the source resolution."},
            {L"toolbar.tip.neural", L"Neural Rendering\nReplays the video through NVIDIA's neural model, which has to render the frames before you can watch them. Off plays the original."},
            {L"toolbar.tip.upscaling", L"DLSS Super Resolution\nRenders the picture larger than the source. Unavailable when the source already fills the panel - there is nothing to upscale then."},
            {L"toolbar.tip.framegen", L"Frame Generation\nWrites a new file with generated frames between the real ones. This is a conversion that takes minutes, not a switch."},
            {L"toolbar.tip.open", L"Open a video file, or paste a YouTube link."},
            {L"toolbar.tip.playpause", L"Play or pause. Space does the same."},
            {L"toolbar.tip.mute", L"Mute or unmute. The slider beside it sets the level."},
            {L"toolbar.tip.aspect", L"Fit the whole picture in the window, or fill the window and crop."},
            {L"toolbar.tip.color", L"Brightness, contrast, saturation and white balance. Applied after rendering, so they are instant."},
            {L"toolbar.tip.debug", L"Show the motion vectors and depth the model is given, instead of the picture."},
            {L"toolbar.tip.fullscreen", L"Fill the screen. Escape or F leaves."},
            {L"export.tip.neural", L"Runs the neural model with the settings from Neural settings. On an upscaled export it runs on the upscaled frame, which is NVIDIA's own DLSS 5 order."},
            {L"export.tip.framegen", L"Generates frames between the rendered ones. Always last, because DLSS-G consumes the finished picture."},
            {L"menu.neural_presets", L"Neural presets"},
            {L"menu.neural_preset_custom", L"Custom (a control was changed)"},
            {L"menu.neural_settings", L"Neural settings\tCtrl+N"},
            {L"menu.encoder_settings", L"Encoder settings"},
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
            {L"neural.cadence.cannot_follow", L"This video plays faster than the neural view can be assembled on this machine, so playback kept falling behind. Showing the rendered file on its own instead."},
            {L"neural.order.generated_source", L"This video is a frame-generation conversion, so this render upscales generated frames and costs as many times the work as the conversion multiplied. Rendering first and converting the result is the faster order."},
            {L"neural.live.will_play", L"starts when the buffer fills"},
            {L"neural.live.will_stay_paused", L"stays paused when the buffer fills"},
            {L"neural.live.slow", L"This video is %ux%u at %.6g fps. On this GPU neural rendering runs at about %.3g frames per second, which is %.2gx real time, so watching it live would pause to buffer almost continuously.\n\nConvert the clip instead (DLSS > Convert & save) to watch it smoothly afterwards.\n\nStart the live session anyway?"},
            {L"neural.live.declined", L"Live neural rendering declined for this video; convert it instead (DLSS > Convert & save)"},
            {L"neural.live.directory_failed", L"The neural segment folder could not be created; check the cache location"},
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
            // A renderer that latched itself unusable is rebuilt once with the
            // frame that was on screen; only a rebuild that fails unloads.
            {L"renderer.removed.rebuilt", L"The graphics device was removed or reset; playback stopped and the renderer was rebuilt"},
            {L"renderer.removed.lost", L"The graphics device was removed or reset, and the renderer could not be rebuilt. Open the video again."},
            {L"renderer.stalled.rebuilt", L"The GPU stopped responding; playback stopped and the renderer was rebuilt"},
            {L"renderer.stalled.lost", L"The GPU stopped responding, and the renderer could not be rebuilt. Open the video again."},
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
            {L"neural.settings.passes", L"Neural passes"}, {L"neural.settings.chained", L"Keep temporal history per pass"},
            {L"neural.settings.group_look", L"Look"},
            {L"neural.settings.group_cost", L"Quality and render time"},
            {L"neural.settings.group_guides", L"Guides sent to the model"},
            {L"neural.settings.note", L"These change the neural model: they apply to the paused preview and to the next conversion; playback color adjustments are instant."},
            {L"neural.settings.ahead", L"Settings changed - this is the previous render; pause to preview them, or convert again"},
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
            {L"encoder.tip.nvenc_preset", L"Speed/quality preset for the hevc_nvenc encoder that writes neural renders and frame-generation conversions. p5 is the default. Measured at 2560x1440 on an RTX 5090: p7 takes twice the encode time of p5 and buys 0.12 VMAF on ordinary content, 0.53 on noise-heavy content, at 95-98 VMAF. The encoder is the long pole on both paths, so whole conversions ran 8.8 s against 6.3 s. Applies to the next render."},
            {L"neural.tip.guide_mv", L"Sends this player's estimated motion to the model so it can reuse the previous frame.\nVideo carries no real motion vectors, so these are estimated per frame and rejected where the estimate is not trustworthy."},
            {L"neural.tip.guide_depth", L"Sends this player's estimated depth proxy.\nMeasured: it only changes the image while Motion vectors is on - with motion off, depth makes no difference."},
            {L"neural.tip.apply", L"Applies these settings to what is on screen: an active session restarts at the playhead, a paused frame is rendered again.\nEvery distinct combination is rendered from scratch - about 10 s for one 1080p frame - and repeats come back from cache."},
            {L"neural.tip.passes", L"Runs the model over each frame more than once.\nThe add-on warns games away from this because every extra pass is another full neural evaluate against a frame budget; a conversion has no frame budget, so the cost lands on render time instead.\nMeasured on the 72-frame range clip: two passes took 9.81 s against 8.01 s and wrote 932,019 bytes against 780,048."},
            {L"neural.tip.chained", L"Gives every stacked pass its own temporal history, reset only on a scene cut.\nTurning it off makes passes 2 and up stateless - NVIDIA documents a per-frame reset as a flicker and aliasing risk, so this is an A/B switch for suspected ghosting rather than a setting to leave off.\nNo effect at one pass."},
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
            {L"status.preparing_source", L"Downloading this source for rendering\u2026"},
            {L"recent.missing", L"This local video has moved or is no longer available."}
        };
        return strings;
    }
};
