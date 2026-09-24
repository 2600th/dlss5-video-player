"""Generates the repeatable benchmark corpus (lossless FFV1 MKV, 1920x1080, 30 fps).

Every synthetic clip is produced by deterministic FFmpeg sources with explicit
seeds; the ``real`` clips are cut from this repository's own 12 September demo capture
(``tools/benchmark/fixtures/demo-capture-20260912.mp4``), which is tracked in git, so they are
reproducible from a clean checkout too. ``manifest.json`` records per-clip
category, hard-cut frame indices, burned-in ground-truth text and an rgb24
frame-hash digest so a regenerated corpus can be proven identical (``--check``).

    python tools/benchmark/corpus.py [--corpus DIR] [--check]
"""
from __future__ import annotations

import argparse
import datetime as dt
import subprocess
import sys
from pathlib import Path

from common import BUILD, CORPUS, FFMPEG, FLAGS, REPO, framemd5, probe, sequence_digest, write_json

WIDTH, HEIGHT, FPS = 1920, 1080, 30
ENCODE = ["-c:v", "ffv1", "-level", "3", "-coder", "1", "-context", "1", "-g", "1", "-slices", "4",
          "-pix_fmt", "yuv420p", "-r", str(FPS), "-an"]
# Camera-original clips keep their publisher frame rate: re-timing 23.976 to 30
# duplicates one frame in five, and a duplicate pair is motionless, which is what
# makes real-film-cuts 37 % motionless and its false-motion level incomparable.
ENCODE_NATIVE_RATE = ["-c:v", "ffv1", "-level", "3", "-coder", "1", "-context", "1", "-g", "1",
                      "-slices", "4", "-pix_fmt", "yuv420p", "-an"]
FONT_MONO = "C\\\\:/Windows/Fonts/consola.ttf"
FONT_UI = "C\\\\:/Windows/Fonts/segoeui.ttf"
FACE_FIXTURE = BUILD / "runtime-comparison-20260907" / "fixtures" / "mafia-60s.mkv"
# The 12 September screen capture, kept byte-identical (the same git blob) when the
# README demonstration was replaced on 22 September: the real-* labels, cut frames and
# digests below are indices into this file, not into whatever the README shows.
DEMO = REPO / "tools" / "benchmark" / "fixtures" / "demo-capture-20260912.mp4"
DEMO_RELATIVE = "tools/benchmark/fixtures/demo-capture-20260912.mp4"
# The demo is a 1920x1080 screen capture of the player, so only part of each frame is
# footage. This rectangle is that part: the columns and rows whose temporal standard
# deviation is nonzero while the capture plays back video (501-1863 x 126-892, trimmed
# to even sides), which is the player's video surface with its pillarbox and its
# chrome excluded. Cropping is not cosmetic - the static chrome is 50 % of the frame,
# and left in it would dominate the residual the cut test reads and inflate the
# static-cell denominator false motion is measured against.
DEMO_SURFACE = "crop=1362:766:502:126"

# Ground truth for the OCR clip. Static lines drift slowly; subtitle cues are
# burned from an SRT so the benchmark exercises real subtitle rendering.
STATIC_TEXT = [
    dict(text="QUICK BROWN FOX 0123456789", px=14, y=120),
    dict(text="Neural rendering benchmark clip", px=18, y=200),
    dict(text="ISO 12233 Rev 4 lanes 88 gain 0.75 dB", px=22, y=290),
    dict(text="serial WX7 4Q9 KL2 zebra 31", px=28, y=400),
]
SUBTITLES = [
    dict(text="The lighthouse keeper counted seven ships.", start=0.5, end=2.5),
    dict(text="Nobody answered the radio after midnight.", start=2.8, end=4.8),
    dict(text="Sector 41 reports clear skies until dawn.", start=5.1, end=7.0),
]


def srt_time(seconds: float) -> str:
    ms = round(seconds * 1000)
    return f"{ms // 3600000:02d}:{ms // 60000 % 60:02d}:{ms // 1000 % 60:02d},{ms % 1000:03d}"


def ffmpeg(args: list[str], cwd: Path) -> None:
    subprocess.run([str(FFMPEG), "-v", "error", "-y", *args], check=True, cwd=cwd, creationflags=FLAGS)


def build_text(corpus: Path) -> dict:
    srt = corpus / "text-subtitles.srt"
    srt.write_text("".join(f"{i + 1}\n{srt_time(c['start'])} --> {srt_time(c['end'])}\n{c['text']}\n\n"
                           for i, c in enumerate(SUBTITLES)), encoding="utf-8")
    draws = ",".join(
        f"drawtext=fontfile={FONT_MONO if i % 2 == 0 else FONT_UI}:text='{t['text']}':fontsize={t['px']}"
        f":fontcolor=white:borderw=1:bordercolor=black@0.6:x=140+t*12:y={t['y']}"
        for i, t in enumerate(STATIC_TEXT))
    graph = (f"gradients=s={WIDTH}x{HEIGHT}:r={FPS}:d=7:seed=7:speed=0.01:nb_colors=3"
             f":c0=#1c2740:c1=#3a2f4a:c2=#123d3a,format=yuv420p,{draws},"
             f"subtitles=filename={srt.name}:force_style='FontName=Arial,FontSize=20,Outline=1'")
    ffmpeg(["-f", "lavfi", "-i", graph, "-t", "7", *ENCODE, "text-subtitles.mkv"], corpus)
    text = [dict(kind="static", text=t["text"], font_px=t["px"], start_s=0.0, end_s=7.0) for t in STATIC_TEXT]
    text += [dict(kind="subtitle", text=c["text"], font_px=20, start_s=c["start"], end_s=c["end"])
             for c in SUBTITLES]
    return dict(name="text-subtitles", category="text", synthetic=True, cuts=[], text=text,
                notes="Slow-drifting small text at 14-28 px plus three burned SRT cues on a slow gradient.")


def build_detail(corpus: Path) -> dict:
    graph = (
        f"mandelbrot=s={WIDTH}x{HEIGHT}:r={FPS}:maxiter=512:start_scale=2.5:end_scale=0.05[base];"
        f"color=c=black@0:s={WIDTH + 32}x{HEIGHT + 32}:r={FPS},drawgrid=w=16:h=16:t=1:c=white@0.55:replace=1,"
        f"format=yuva420p[grid];"
        f"testsrc2=s=960x540:r={FPS},zoompan=z='1.5+0.5*sin(in/20)':x='iw/2-(iw/zoom/2)'"
        f":y='ih/2-(ih/zoom/2)':d=1:s=960x540:fps={FPS},format=yuv420p[inset];"
        f"[base][grid]overlay=x='-16+mod(t*45\\,16)':y='-16+mod(t*30\\,16)':eval=frame:shortest=1[g];"
        f"[g][inset]overlay=x=960:y=540:shortest=1,format=yuv420p")
    ffmpeg(["-filter_complex", graph, "-t", "7", *ENCODE, "fine-detail.mkv"], corpus)
    return dict(name="fine-detail", category="detail", synthetic=True, cuts=[], text=[],
                notes="Continuous Mandelbrot zoom, a sub-pixel-drifting 16 px grid, and a zoompan test pattern inset.")


def build_highlights(corpus: Path) -> dict:
    lum = ("clip(lum(X\\,Y)+230*exp(-(pow(X-(200+1520*(T/7))\\,2)+pow(Y-(540+300*sin(T*1.5))\\,2))"
           "/(2*pow(140\\,2)))+90*exp(-(pow(X-(1700-1400*(T/7))\\,2)+pow(Y-260\\,2))/(2*pow(60\\,2)))\\,0\\,255)")
    graph = (f"gradients=s={WIDTH}x{HEIGHT}:r={FPS}:d=7:seed=11:speed=0.02:nb_colors=4:type=linear"
             f":c0=#5a3d2b:c1=#2f5d4a:c2=#1f2a55:c3=#8a7a3a,"
             f"format=yuv444p,geq=lum='{lum}':cb='cb(X\\,Y)':cr='cr(X\\,Y)',format=yuv420p")
    ffmpeg(["-f", "lavfi", "-i", graph, "-t", "7", *ENCODE, "highlights-gradients.mkv"], corpus)
    return dict(name="highlights-gradients", category="tone", synthetic=True, cuts=[], text=[],
                notes="Animated smooth gradients with two radial highlight sweeps that clip to white.")


def concat(segments: list[str]) -> str:
    """Hard-cuts `segments` together: every boundary is one frame of discontinuity."""
    graph = ";".join(f"{s},setpts=PTS-STARTPTS,format=yuv420p[s{i}]" for i, s in enumerate(segments))
    return graph + ";" + "".join(f"[s{i}]" for i in range(len(segments))) + f"concat=n={len(segments)}:v=1:a=0"


def frozen(source: str, seconds: float) -> str:
    """One frame of a generator, held for `seconds`, so the only motion is what follows it."""
    return f"{source},trim=end_frame=1,loop=loop=-1:size=1:start=0,trim=duration={seconds}"


STILL = f"mandelbrot=s=%dx%d:r={FPS}:maxiter=300:start_scale=1.6:end_scale=1.6"


def build_cuts(corpus: Path) -> dict:
    seg = 1.5
    segments = [
        f"testsrc2=s={WIDTH}x{HEIGHT}:r={FPS},trim=duration={seg},"
        f"crop=960:540:x='(iw-960)*(0.5+0.5*sin(t*5))':y='(ih-540)*(0.5+0.5*cos(t*4))',scale={WIDTH}:{HEIGHT}",
        f"mandelbrot=s={WIDTH}x{HEIGHT}:r={FPS}:start_scale=1.0:end_scale=0.2,trim=duration={seg}",
        f"life=s={WIDTH}x{HEIGHT}:r={FPS}:seed=5:mold=10:life_color=#ffcc66:death_color=#221100,"
        f"trim=duration={seg}",
        f"smptehdbars=s={WIDTH}x{HEIGHT}:r={FPS},rotate=a=t*3:fillcolor=black,trim=duration={seg}",
        f"gradients=s={WIDTH}x{HEIGHT}:r={FPS}:seed=3:speed=0.15:nb_colors=5"
        f":c0=#d04020:c1=#20a0d0:c2=#f0e040:c3=#3020a0:c4=#20c060,trim=duration={seg}",
    ]
    ffmpeg(["-filter_complex", concat(segments), *ENCODE, "cuts-motion.mkv"], corpus)
    cuts = [round(seg * FPS * i) for i in range(1, len(segments))]
    return dict(name="cuts-motion", category="cuts", synthetic=True, cuts=cuts, text=[],
                notes="Five dissimilar 1.5 s segments (fast pan, fractal zoom, cellular automaton, fast rotation, "
                      "fast gradients) hard-cut together.")


def build_similar_cuts(corpus: Path) -> dict:
    seg = 1.0
    # One deep fractal still, circled by a pan whose period is exactly the shot length,
    # mirrored a different way in each shot. Because the pan returns to where it
    # started, the frames either side of a cut show the same window of the same still
    # under different mirrors - a permutation of the pixels - so the two shots have the
    # same luma histogram by construction and the weak arm's histogram gate is blind
    # here. Measured across the cuts: residual 0.19-0.25 at overlap 0.98, which is the
    # gap between both shipped arms.
    segments = [frozen(f"mandelbrot=s=2560x1440:r={FPS}:maxiter=512:start_scale=0.35:end_scale=0.35", seg) +
                f",{flip},crop={WIDTH}:{HEIGHT}:x='320+80*sin(2*PI*t/{seg})':y='180+80*cos(2*PI*t/{seg})'"
                for flip in ("null", "hflip", "vflip", "hflip,vflip")]
    ffmpeg(["-filter_complex", concat(segments), *ENCODE, "cuts-similar.mkv"], corpus)
    return dict(name="cuts-similar", category="cuts", synthetic=True,
                cuts=[round(seg * FPS * i) for i in range(1, len(segments))], text=[],
                notes="Four 1.0 s shots of the same deep fractal still, gently panned and mirrored a "
                      "different way in each, hard-cut together: identical histogram, no correspondence.")


def build_pan(corpus: Path) -> dict:
    seconds = 1.0
    # The window crosses the whole spare width and height of a 4K still in one second:
    # 64 px per frame horizontally and 36 vertically, which is 6.1 analysis cells of
    # travel against the global search's +-7. As fast as a pan can be and still be
    # findable, which is what makes it a negative worth having.
    graph = (frozen(STILL % (3840, 2160), seconds) +
             f",crop={WIDTH}:{HEIGHT}:x='min(1920,1920*t)':y='min(1080,1080*t)',format=yuv420p")
    ffmpeg(["-filter_complex", graph, *ENCODE, "pan-fast.mkv"], corpus)
    return dict(name="pan-fast", category="cuts", synthetic=True, cuts=[], text=[],
                notes="1.0 s diagonal pan across a 4K fractal still at 64x36 px per frame (6.1 analysis "
                      "cells). Nothing here is a cut; every frame corresponds to the last.")


def build_zoom(corpus: Path) -> dict:
    seconds, frames = 1.5, round(1.5 * FPS)
    # 2.2x in 1.5 s. The frame edge moves 25.6 px (2.1 cells) in the first frame and
    # less afterwards, so the per-cell +-3 search can follow it while the single global
    # translation the residual is measured on cannot - which is the point of the clip.
    graph = (frozen(STILL % (WIDTH, HEIGHT), seconds) +
             f",zoompan=z='1+1.2*on/{frames - 1}':x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)'"
             f":d=1:s={WIDTH}x{HEIGHT}:fps={FPS},format=yuv420p")
    ffmpeg(["-filter_complex", graph, *ENCODE, "zoom-fast.mkv"], corpus)
    return dict(name="zoom-fast", category="cuts", synthetic=True, cuts=[], text=[],
                notes="1.5 s centre zoom from 1.0x to 2.2x on a fractal still. No cut; a global translation "
                      "cannot model it, so it is the residual arm's hardest honest negative.")


def build_dissolve(corpus: Path) -> dict:
    hold, fade = 1.7, 0.7
    graph = (f"testsrc2=s={WIDTH}x{HEIGHT}:r={FPS},trim=duration={hold},setpts=PTS-STARTPTS,format=yuv420p[a];"
             f"mandelbrot=s={WIDTH}x{HEIGHT}:r={FPS}:maxiter=300:start_scale=1.6:end_scale=0.9,"
             f"trim=duration={hold},setpts=PTS-STARTPTS,format=yuv420p[b];"
             f"[a][b]xfade=transition=fade:duration={fade}:offset={hold - fade}")
    ffmpeg(["-filter_complex", graph, *ENCODE, "dissolve.mkv"], corpus)
    first = round((hold - fade) * FPS)
    # A dissolve has no frame at which history stops being valid: every pair inside it
    # still corresponds, and by the end the content is a different shot. One reset
    # anywhere in the fade is defensible either way, so it is scored as tolerated rather
    # than as a hit or a miss; a second reset inside the same fade is the over-reset
    # DLSS PG 310.6.0 S3.13 warns about and stays a false positive.
    return dict(name="dissolve", category="cuts", synthetic=True, cuts=[],
                soft_cuts=[[first, first + round(fade * FPS) + 1]], text=[],
                notes="0.7 s cross-fade from a test pattern to a fractal, held 1.0 s either side. At most one "
                      "history reset inside the fade is correct; two are not.")


def build_flash(corpus: Path) -> dict:
    seconds = 3.0
    # A four-frame flash and, a second later, a sustained exposure step. Both collapse
    # the luma histogram without touching correspondence, which is exactly the shape the
    # weak arm's histogram gate cannot tell apart from a cut. +0.26 rather than +0.30
    # deliberately: at +0.30 the frame that ends the flash measures residual 0.3003
    # against a 0.30 threshold, and a labelled case must not turn on the fourth decimal.
    graph = (frozen(STILL % (2560, 1440), seconds) +
             f",crop={WIDTH}:{HEIGHT}:x='320+300*sin(t*1.1)':y='180+170*cos(t*0.9)',"
             f"eq=brightness=0.26:enable='between(t,1.0,1.1)',"
             f"eq=brightness=0.18:enable='gte(t,2.0)',format=yuv420p")
    ffmpeg(["-filter_complex", graph, *ENCODE, "flash-exposure.mkv"], corpus)
    return dict(name="flash-exposure", category="cuts", synthetic=True, cuts=[], text=[],
                notes="3 s slow pan with a 4-frame flash at t=1.0 and a sustained +0.18 exposure step at "
                      "t=2.0. Neither is a cut; the scene never changes.")


def build_faces(corpus: Path) -> dict | None:
    if not FACE_FIXTURE.exists():
        print(f"face fixture missing: {FACE_FIXTURE}; skipping faces clip", file=sys.stderr)
        return None
    ffmpeg(["-ss", "12", "-t", "8", "-i", str(FACE_FIXTURE), "-vf", f"scale={WIDTH}:{HEIGHT},format=yuv420p",
            *ENCODE, "faces.mkv"], corpus)
    return dict(name="faces", category="faces", synthetic=False, cuts=[], text=[],
                source=str(FACE_FIXTURE),
                notes="NOT synthetic: seconds 12-20 of the mafia-60s fixture (frontal and profile faces, skin, "
                      "hair). Re-encoded losslessly; the fixture itself is not redistributed.")


def demo() -> str:
    """The demo capture, which is tracked in git, so a missing file is a broken checkout."""
    if not DEMO.exists():
        raise SystemExit(f"{DEMO} is tracked in this repository and the real clips are cut from it; "
                         f"restore it (git checkout -- {DEMO_RELATIVE}) and build again")
    return str(DEMO)


def real_segment(first: int, count: int) -> str:
    """`count` frames of the demo's video surface, starting at decoded source frame `first`.

    Selecting on the decoded frame index rather than seeking by time is deliberate: the
    labelled cut indices below are decoded-frame indices, and `-ss` on an open-GOP h264
    file resolves to a keyframe, which would shift every one of them.
    """
    return (f"select='between(n\\,{first}\\,{first + count - 1})',setpts=N/{FPS}/TB,"
            f"{DEMO_SURFACE},scale={WIDTH}:{HEIGHT}:flags=lanczos,format=yuv420p")


def build_real_film_cuts(corpus: Path) -> dict:
    first, count = 15, 102
    ffmpeg(["-i", demo(), "-vf", real_segment(first, count), "-frames:v", str(count),
            *ENCODE, "real-film-cuts.mkv"], corpus)
    return dict(name="real-film-cuts", category="real", synthetic=False, cuts=[20, 47, 70, 87], text=[],
                source=f"{DEMO_RELATIVE} frames {first}-{first + count - 1}",
                notes="NOT synthetic: five film shots from the demo capture (hands over a bedspread, a car on a "
                      "road, a man in a crowd, a revolver firing, a portrait) with grain, motion blur and real "
                      "camera motion. Cuts 20/47/70/87 (source 35/62/85/102) were proposed by FFmpeg scene "
                      "detection on the cropped surface, which scored them 0.72/0.66/0.52/0.53, and then "
                      "confirmed by eye: all 102 frames were extracted and inspected, those four pairs are the "
                      "only ones where the shot changes, and the frame at each index is the first frame of the "
                      "new shot. The eight smaller detector flags inside the clip (locals 2, 4, 7, 13, 15, 18, "
                      "84 and 86, scoring 0.032-0.076) were checked the same way and are hand motion, a pan or "
                      "the muzzle flash, so none is labelled: the flash at locals 84-85 is a real two-frame "
                      "exposure spike inside one shot. Note the 70->87 gap is 17 frames, inside cutmirror's "
                      "0.6 s weak-arm debounce - the editing rhythm no synthetic clip here has.")


def build_real_game_cuts(corpus: Path) -> dict:
    first, count = 438, 68
    ffmpeg(["-i", demo(), "-vf", real_segment(first, count), "-frames:v", str(count),
            *ENCODE, "real-game-cuts.mkv"], corpus)
    return dict(name="real-game-cuts", category="real", synthetic=False, cuts=[32], text=[],
                source=f"{DEMO_RELATIVE} frames {first}-{first + count - 1}",
                notes="NOT synthetic: the demo's game-footage playback section - a dirt-bike race exterior "
                      "(locals 0-31) hard-cut to a convenience-store interior (locals 32-67), both with the "
                      "game's own static HUD over fast camera motion, which is where invented motion on static "
                      "cells shows. The single cut at local 32 (source 470) was proposed at scene score 0.37 "
                      "and confirmed by inspecting all 68 frames: 469 is the last race frame and 470 the first "
                      "interior frame. The detector's neighbouring flags at locals 29 and 33 (0.16, 0.22) are "
                      "camera motion and dust inside a shot - inspected, not cuts, so not labelled.")


def build_real_game_motion(corpus: Path) -> dict:
    first, count = 506, 76
    ffmpeg(["-i", demo(), "-vf", real_segment(first, count), "-frames:v", str(count),
            *ENCODE, "real-game-motion.mkv"], corpus)
    return dict(name="real-game-motion", category="real", synthetic=False, cuts=[], text=[],
                source=f"{DEMO_RELATIVE} frames {first}-{first + count - 1}",
                notes="NOT synthetic: one continuous 2.5 s shot, a character running off a rooftop and falling "
                      "towards a city, so the camera translates while the subject occludes and disoccludes "
                      "background throughout - the real-footage negative. Nothing here is a cut: all 76 frames "
                      "were inspected, and the strongest scene score of any pair inside the clip is 0.055 (at "
                      "local 1, the second frame of the new shot; then 0.046, 0.045, 0.041) - far under the "
                      "0.37-0.60 the confirmed cuts in the other real clips measure.")


def build_still_hold(corpus: Path) -> dict:
    first, count = 30, 60
    # Decoded frame 30 of the demo held for two seconds: every pair is two identical
    # frames, so any motion a guide reports is invented. It is the clip the flow
    # engine's fixed field on identical frames was found on (the w4-sr still is frame
    # 0 of the same capture) and the one the zero-motion test exists for.
    graph = (f"select='eq(n\\,{first})',setpts=N/{FPS}/TB,{DEMO_SURFACE},scale={WIDTH}:{HEIGHT}:flags=lanczos,"
             f"loop=loop={count - 1}:size=1:start=0,setpts=N/{FPS}/TB,format=yuv420p")
    ffmpeg(["-i", demo(), "-vf", graph, "-frames:v", str(count), *ENCODE, "still-hold.mkv"], corpus)
    return dict(name="still-hold", category="still", synthetic=False, cuts=[], text=[],
                source=f"{DEMO_RELATIVE} frame {first}, held",
                notes="NR-processed capture: one film frame (a revolver on a patterned bedspread, grain) held "
                      "for 60 frames. Nothing moves; every pair is identical.")


def slow_pan(still: str, frames: int) -> str:
    """A 1920x1080 view sliding right by exactly 0.5 px per frame across `still`.

    The window moves one pixel per frame across a 2x enlargement and is then
    area-reduced by two, so the content moves half a source pixel per frame: the
    sub-pixel speed the flow engine's own fixed field is the size of. The crop runs
    on RGB because a 4:2:0 crop rounds its offset to even, which would move the
    view one pixel every other frame and hold it in between.
    """
    return (f"{still},scale={2 * WIDTH + frames}:{2 * HEIGHT}:flags=lanczos,format=gbrp,loop=loop={frames - 1}:size=1:start=0,"
            f"setpts=N/{FPS}/TB,crop={2 * WIDTH}:{2 * HEIGHT}:x='n':y=0,"
            f"scale={WIDTH}:{HEIGHT}:flags=area,format=yuv420p")


def build_pan_slow(corpus: Path) -> dict:
    first, count = 506, 90
    still = f"select='eq(n\\,{first})',setpts=N/{FPS}/TB,{DEMO_SURFACE},scale={WIDTH}:{HEIGHT}:flags=lanczos"
    ffmpeg(["-i", demo(), "-vf", slow_pan(still, count), "-frames:v", str(count), *ENCODE, "pan-slow.mkv"], corpus)
    return dict(name="pan-slow", category="still", synthetic=False, cuts=[], text=[],
                source=f"{DEMO_RELATIVE} frame {first}, panned",
                notes="NR-processed capture: one frame (a city skyline from a rooftop) panned right at exactly "
                      "0.5 px per frame for 90 frames. True motion is (-0.5, 0) everywhere; no cut.")


def build_pan_slow_fractal(corpus: Path) -> dict:
    count = 90
    graph = slow_pan(f"{STILL % (WIDTH, HEIGHT)},trim=end_frame=1", count)
    ffmpeg(["-filter_complex", graph, "-frames:v", str(count), *ENCODE, "pan-slow-fractal.mkv"], corpus)
    return dict(name="pan-slow-fractal", category="still", synthetic=True, cuts=[], text=[],
                notes="A fractal still panned right at exactly 0.5 px per frame for 90 frames: detail at every "
                      "scale, no noise. True motion is (-0.5, 0) everywhere; no cut.")


def build_real_dissolve(corpus: Path) -> dict:
    # The demo capture contains no dissolve: across all 678 frames every transition is a
    # single-frame jump, so there is no real fade to label. This clip therefore has real
    # material and a synthesised transition - the honest half of what the gate question
    # asks for - built from the two adjacent shots real-game-cuts and real-game-motion
    # use, with the same 0.7 s fade the synthetic `dissolve` clip uses so the two are
    # directly comparable.
    fade, a_first, a_count, b_first, b_count = 0.7, 470, 36, 506, 51
    fade_frames = round(fade * FPS)
    graph = (f"[0:v]split=2[a0][b0];"
             f"[a0]{real_segment(a_first, a_count)}[a];"
             f"[b0]{real_segment(b_first, b_count)}[b];"
             f"[a][b]xfade=transition=fade:duration={fade}:offset={(a_count - fade_frames) / FPS}")
    ffmpeg(["-i", demo(), "-filter_complex", graph, "-frames:v", str(a_count + b_count - fade_frames),
            *ENCODE, "real-dissolve.mkv"], corpus)
    first = a_count - fade_frames
    return dict(name="real-dissolve", category="real", synthetic=False, cuts=[],
                soft_cuts=[[first, first + fade_frames + 1]], text=[],
                source=f"{DEMO_RELATIVE} frames {a_first}-{a_first + a_count - 1} over "
                       f"{b_first}-{b_first + b_count - 1}",
                notes="Real material, synthesised transition, and the distinction matters: the demo capture "
                      "has no dissolve anywhere in it (all 678 frames were differenced; every transition is a "
                      "single-frame jump), so this is the store interior cross-faded into the rooftop fall over "
                      "0.7 s, held 0.5 s before and 1.0 s after. The fade occupies frames 15-35 and is "
                      "deliberately NOT in `cuts`: no single frame in it is where history stops being valid. At "
                      "most one reset inside the span is correct; a second is the over-reset artifact.")


# ---------------------------------------------------------------------------
# Camera-original clips.
#
# Every `real-*` clip above is an NR-processed capture: the player's own DLSS-NR
# output, screen-captured, h264-encoded twice and lanczos-upscaled before the pass
# under test renders it again. Sound for an A/B where both arms see identical
# input, and not a statement about original footage. These clips close that gap by
# starting from the publisher's own release of the same two titles the demo capture
# filmed, plus one trailer acquired for the transition the others do not contain.
#
# The sources are copyrighted trailers, so they are NOT committed and NOT
# redistributed: `tools/benchmark/fetch_camera_original.ps1` downloads them by
# video id and format id into `build-upscaling/camera-original/`, and each builder
# below skips itself when its source is absent, exactly as `faces` does. What is
# committed is the label set and the per-clip frame digest, so a rebuild is
# provable against this manifest.
#
# Geometry: crop to the active picture, scale to 1080 height with lanczos, then
# centre-crop to 1920 wide. No padding, so no synthetic black row enters the
# static-cell population that false motion is measured against. Native frame rate
# is preserved - forcing 30 fps would duplicate frames, which is what makes
# `real-film-cuts` 37 % motionless and its false-motion level incomparable.
#
# That scale is a DOWNscale for the two 1440p sources (1384 active rows -> 1080)
# and an UPscale for the letterboxed 1920x1038 trailer (884 active rows -> 1080,
# 1.22x). An upscale adds no information, so `orig-dissolve` is the one clip here
# whose fine detail is partly resampled rather than camera-native. It is kept
# because what that clip exists to carry is a transition's temporal structure,
# which the resample does not touch, and because padding to 1080 instead would put
# 196 static black rows into the static-cell denominator. Do not cite it for
# sharpness, grain or any per-pixel fidelity claim.
CAMERA_ORIGINAL = BUILD / "camera-original"
# Tracked expectations for the clips built from those sources: the sources and the
# built clips are both untracked, so this file is the only thing a clean checkout
# can verify a rebuild against.
CAMERA_ORIGINAL_DIGESTS = Path(__file__).resolve().parent / "camera-original.digests.json"
CAMERA_SOURCES = {
    # tag: (file, youtube id, format id, active-picture crop or None, human description)
    "godfather": (CAMERA_ORIGINAL / "godfather-50th.webm", "UaVTIH8mujA", "271", "crop=2560:1384:0:28",
                  "THE GODFATHER 50th Anniversary Trailer (Paramount Pictures), 2560x1440 VP9, 23.976 fps"),
    "gtavi": (CAMERA_ORIGINAL / "gtavi-extended.webm", "uphThaa97ig", "271", None,
              "Grand Theft Auto VI: An Extended Look (Netflix/Now Playing), 2560x1440 VP9, 30 fps"),
    "lawrence": (CAMERA_ORIGINAL / "cand-lawrence-arabia.mp4", "HFAkWNiETrg", "399", "crop=1920:884:0:76",
                 "Lawrence of Arabia - official HD trailer for the new restoration (Park Circus), "
                 "1920x1038 AV1 (format 399), 23.976 fps, 122 s"),
}


def camera_segment(crop: str | None, first: int, count: int) -> str:
    """`count` frames from source frame `first`, active picture only, at native fps."""
    chain = [f"select='between(n\\,{first}\\,{first + count - 1})'"]
    if crop:
        chain.append(crop)
    chain += [f"scale=-2:{HEIGHT}:flags=lanczos", f"crop={WIDTH}:{HEIGHT}", "format=yuv420p"]
    return ",".join(chain)


def build_camera_clip(corpus: Path, name: str, tag: str, first: int, count: int, clip: dict) -> dict | None:
    source, video_id, fmt, crop, described = CAMERA_SOURCES[tag]
    if not source.exists():
        print(f"camera-original source missing: {source}; skipping {name} "
              f"(run tools/benchmark/fetch_camera_original.ps1)", file=sys.stderr)
        return None
    ffmpeg(["-i", str(source), "-vf", camera_segment(crop, first, count), "-frames:v", str(count),
            *ENCODE_NATIVE_RATE, f"{name}.mkv"], corpus)
    clip.update(name=name, category=clip.get("category", "camera-original"), synthetic=False,
                camera_original=True, text=clip.get("text", []),
                source=f"youtube:{video_id} format {fmt} frames {first}-{first + count - 1} ({described})")
    return clip


def build_orig_film_cuts_a(corpus: Path) -> dict | None:
    return build_camera_clip(corpus, "orig-film-cuts-a", "godfather", 2130, 131, dict(
        cuts=[4, 14, 24, 39, 51, 81, 105, 120],
        notes="Camera-original, not a capture: the publisher's 1440p release of the same film the demo capture "
              "filmed off the player's window. Eight hard cuts in 131 frames - the densest real editing rhythm "
              "in this corpus, and the point of the clip. Every one of the eight was proposed by mean |dY| >= 25 "
              "with histogram overlap <= 0.55 on the built 1920x1080 clip and then confirmed by inspecting the "
              "frame pair either side; locals 4 and 51 needed the inspection, because their histograms overlap "
              "0.65 and 0.75 and a threshold alone would have missed them. The 20 other pairs above |dY| 12 "
              "(locals 15-17, 40-50, 121-130) were inspected the same way and are camera motion inside a shot, "
              "so none is labelled. The frame at each index is the first frame of the new shot."))


def build_orig_film_cuts_b(corpus: Path) -> dict | None:
    return build_camera_clip(corpus, "orig-film-cuts-b", "godfather", 2271, 70, dict(
        cuts=[13, 41, 60],
        notes="Camera-original. Three hard cuts in 70 frames, all confirmed by frame-pair inspection, and the "
              "span starts one frame later than its neighbour clip on purpose: at source 2270 the first frame "
              "was the tail of the previous shot, which would have put a one-frame shot at the head of the clip "
              "and given the cut test a boundary no reset policy can serve. No pair between the labelled cuts "
              "exceeds |dY| 8.7."))


def build_orig_film_fade(corpus: Path) -> dict | None:
    return build_camera_clip(corpus, "orig-film-fade", "godfather", 275, 31, dict(
        cuts=[], soft_cuts=[[6, 27]],
        notes="Camera-original, and a REAL gradual transition rather than a synthesised one: a fade to black "
              "cut by the trailer's own editor. Luma falls 77 -> 0 monotonically across locals 6-27 and no pair "
              "in the clip reaches |dY| 6, so there is no frame where history stops being valid - the same "
              "property `real-dissolve` has to synthesise. Frames 27-30 are the black hold, kept short on "
              "purpose: a long black tail is static and would inflate the static-cell denominator the way "
              "`real-film-cuts`' frozen tail does."))


def build_orig_faces(corpus: Path) -> dict | None:
    return build_camera_clip(corpus, "orig-faces", "godfather", 1160, 101, dict(
        category="faces", cuts=[19, 34, 56, 83],
        notes="Camera-original faces: frontal and three-quarter close-ups with skin, hair and film grain, "
              "replacing the `faces` fixture (`mafia-60s.mkv`) that is not in this repository and cannot be "
              "redistributed. Four hard cuts, each confirmed by frame-pair inspection, and a face is clearly "
              "visible on at least one side of every one of them; locals 19, 34 and 83 have histogram overlap "
              "0.67-0.71, so they are exactly the 'similar cut' case a threshold misses. This is the only clip "
              "here whose category drives face metrics, so `analyze.py --faces` has real faces to embed for "
              "the first time."))


def build_orig_game_cuts(corpus: Path) -> dict | None:
    return build_camera_clip(corpus, "orig-game-cuts", "gtavi", 40, 91, dict(
        cuts=[20, 70],
        notes="Camera-original game footage, the publisher's 1440p release of the same material the demo "
              "capture filmed. Two hard cuts at locals 20 and 70, both confirmed by frame-pair inspection "
              "(street chase -> jet skis -> armoured truck), histogram overlap 0.34 and 0.38. No other pair in "
              "the clip exceeds |dY| 9.3."))


def build_orig_game_motion(corpus: Path) -> dict | None:
    return build_camera_clip(corpus, "orig-game-motion", "gtavi", 492, 66, dict(
        cuts=[],
        notes="Camera-original, one continuous moving shot and the hardest such span to find in this source: a "
              "sweep of every span in the 26 s release with no pair above |dY| 12 returned exactly one that is "
              "also actually moving (median |dY| 3.9) - this one. The only other continuous spans are the "
              "static end card. Its strongest internal pair, local 65 at |dY| 12.4 with histogram overlap 0.99, "
              "was inspected and is the same shot. The false-motion negative for camera-original material."))


# The two clips below exist because a ballot about a per-frame effect needs stills
# that carry temporal history, and this corpus could not supply them: every other
# camera-original clip is 1.3-5.5 s, so a guard of 1 s past the opening cut leaves
# five of the seven with no usable candidate frame at all. Both spans were found the
# same way - every span in the registered sources with no pair proposed as a cut
# (mean |dY| >= 25 or histogram overlap <= 0.55), at least 240 frames long, and a
# median |dY| of 1.5 or more so the span is actually moving. Three qualified; the
# third (lawrence 1694-2159, 19.4 s) was rejected because 179 of its 465 pairs are
# near-static, the same defect that makes `real-film-cuts` false-motion level
# incomparable.
def build_orig_film_motion_a(corpus: Path) -> dict | None:
    return build_camera_clip(corpus, "orig-film-motion-a", "godfather", 205, 272, dict(
        cuts=[],
        notes="Camera-original, one continuous 11.3 s interior shot - the longest cut-free moving span in "
              "this source. Subject motion and a slow camera in a dim room with faces and skin, which is "
              "where a tone operator is judged by eye rather than by a metric. Motion is gentle: median "
              "|dY| 1.81, and the largest internal pair is local 248 at |dY| 4.9 with histogram overlap "
              "0.983, inspected and the same shot (a man at a table, a second figure behind him). 33 of "
              "its 271 pairs are near-static, so quote its false-motion level only against itself."))


def build_orig_film_motion_b(corpus: Path) -> dict | None:
    return build_camera_clip(corpus, "orig-film-motion-b", "lawrence", 562, 258, dict(
        cuts=[],
        notes="Camera-original, one continuous 10.8 s exterior tracking shot and the strongest sustained "
              "motion in any camera-original clip here: median |dY| 4.72 against 3.9 for the 2.2 s "
              "`orig-game-motion`. A rider tracked along a road with foliage and grass streaming past, so "
              "real motion blur and fine detail in the same frame. Its five largest internal pairs (locals "
              "144-183, |dY| 14.5-17.4) were inspected: histogram overlap stays 0.82-0.97 across all of "
              "them and each is the same shot in fast motion, not a cut. Two caveats travel with it: the "
              "letterbox geometry noted above for `orig-dissolve` - 884 active rows scaled UP to 1080, so "
              "fine detail is partly resampled - and a burned-in title card over the first half."))


def build_orig_dissolve(corpus: Path) -> dict | None:
    return build_camera_clip(corpus, "orig-dissolve", "lawrence", 2510, 71, dict(
        cuts=[], soft_cuts=[[21, 48]],
        notes="The real cross-dissolve this corpus did not have. Note the geometry caveat above: this "
              "source is letterboxed 1920x1038, so its 884 active rows are scaled UP to 1080 (1.22x) "
              "where the other camera-original clips are scaled down - fine detail here is partly "
              "resampled, and only the transition's temporal structure is camera-native. Neither title the demo capture filmed contains "
              "one - both were scanned end to end and every transition in them is a hard cut, a fade through "
              "black or a fade from white - so this trailer was acquired for the transition alone. Verified as "
              "a dissolve rather than a fade: across locals 21-48 the frame is a linear blend of the shots "
              "either side with alpha sliding monotonically 1 -> 0 (residual 0.063 of the endpoint difference), "
              "both endpoints are bright real shots rather than black, and mid-transition gradient energy dips "
              "below both - the double-exposure signature. Confirmed by eye at locals 10/33/56: a title card "
              "over white desert dissolving into a cliff shot, both visible at once in the middle. No hard cut "
              "and no pair above |dY| 5.2 in the whole clip."))


# ---------------------------------------------------------------------------
# Near/far clips (P2.4). The depth guide is only worth questioning on footage whose
# depth is known, and nothing else here knows it: every other clip is flat - a
# fractal, a pattern, a capture - and the camera-original ones have depth nobody
# measured. These two are built from layers at known distances moving at known
# integer speeds, so their true depth and true motion can be written per frame by
# `guidefiles.py truth` from the same numbers the graph below uses, and fed through
# `depth=file:`/`mv=file:` beside the estimator's own guides.
#
# Layer speeds are content displacement in whole pixels per frame, leftward for a
# positive number (the window over each wide still advances by that much), so every
# layer's motion is exact and constant. Depth follows the convention the renderer
# hands DLSS: 0 = near, 1 = far.
DEPTH_PAN = dict(
    frames=90,
    # A landscape seen from a moving car: sky and distant ridge, rolling hills, and
    # posts right beside the road. Speeds 3 / 9 / 24 px per frame: 0.25, 0.75 and 2.0
    # analysis cells, so the far layer is slow enough to look still to a coarse test
    # and the near one is fast.
    far=dict(speed=3, depth=0.9),
    mid=dict(speed=9, depth=0.5, horizon=600, amplitude=120, period=700),
    near=dict(speed=24, depth=0.15, pitch=400, width=90),
)
DEPTH_SUBJECT = dict(
    frames=90,
    # An interior with a person-sized subject crossing close to the camera while the
    # camera itself drifts: the background moves 1 px per frame, the subject 14 px
    # right and bobs +-18 px vertically, so it occludes and disoccludes the room.
    room=dict(speed=1, depth=0.85),
    subject=dict(depth=0.2, rx=260, ry=420, x0=160, dx=14, y0=150, bob=18, bob_period=45),
)


def depth_pan_layers(n: int, x, y):
    """(depth, content speed) of the layer visible at (x, y) in frame n, vectorised."""
    import numpy as np
    p = DEPTH_PAN
    mid, near = p["mid"], p["near"]
    xm = x + mid["speed"] * n
    xn = x + near["speed"] * n
    in_near = np.mod(xn, near["pitch"]) < near["width"]
    in_mid = y > mid["horizon"] + mid["amplitude"] * np.sin(xm * 2 * np.pi / mid["period"])
    depth = np.where(in_near, near["depth"], np.where(in_mid, mid["depth"], p["far"]["depth"]))
    speed = np.where(in_near, near["speed"], np.where(in_mid, mid["speed"], p["far"]["speed"]))
    return depth, speed


def depth_subject_position(n: int) -> tuple[int, int]:
    """Top-left of the subject's box in frame n; the graph's overlay uses the same expression."""
    import math
    s = DEPTH_SUBJECT["subject"]
    return s["x0"] + s["dx"] * n, s["y0"] + math.trunc(s["bob"] * math.sin(2 * math.pi * n / s["bob_period"]))


def depth_subject_mask(n: int, x, y):
    """True where the subject covers (x, y) in frame n."""
    s = DEPTH_SUBJECT["subject"]
    left, top = depth_subject_position(n)
    return ((x - left - s["rx"]) / s["rx"]) ** 2 + ((y - top - s["ry"]) / s["ry"]) ** 2 < 1.0


def build_depth_pan(corpus: Path) -> dict:
    p = DEPTH_PAN
    frames, seconds = p["frames"], p["frames"] / FPS
    far, mid, near = p["far"], p["mid"], p["near"]
    wide = lambda speed: WIDTH + speed * frames + 16  # noqa: E731 - the still each window slides over
    mid_alpha = f"if(gt(Y\\,{mid['horizon']}+{mid['amplitude']}*sin(X*2*PI/{mid['period']}))\\,255\\,0)"
    near_alpha = f"if(lt(mod(X\\,{near['pitch']})\\,{near['width']})\\,255\\,0)"
    rgba = "format=rgba,geq=r='r(X\\,Y)':g='g(X\\,Y)':b='b(X\\,Y)':a='{alpha}'"
    graph = (
        frozen(f"mandelbrot=s={wide(far['speed'])}x{HEIGHT}:r={FPS}:maxiter=300:start_scale=1.4:end_scale=1.4",
               seconds) + f",crop={WIDTH}:{HEIGHT}:x='{far['speed']}*n':y=0,format=rgba[far];"
        f"sierpinski=s={wide(mid['speed'])}x{HEIGHT}:r={FPS}:seed=3:jump=100:type=carpet,trim=end_frame=1,"
        + rgba.format(alpha=mid_alpha) + f",loop=loop=-1:size=1:start=0,trim=duration={seconds},"
        f"crop={WIDTH}:{HEIGHT}:x='{mid['speed']}*n':y=0[mid];"
        f"testsrc2=s={wide(near['speed'])}x{HEIGHT}:r={FPS},trim=end_frame=1,"
        + rgba.format(alpha=near_alpha) + f",loop=loop=-1:size=1:start=0,trim=duration={seconds},"
        f"crop={WIDTH}:{HEIGHT}:x='{near['speed']}*n':y=0[near];"
        f"[far][mid]overlay=shortest=1[fm];[fm][near]overlay=shortest=1,format=yuv420p")
    ffmpeg(["-filter_complex", graph, "-frames:v", str(frames), *ENCODE, "depth-pan.mkv"], corpus)
    return dict(name="depth-pan", category="depth", synthetic=True, cuts=[], text=[], depth_truth=True,
                notes="Near/far landscape pan built from three layers at known depth and known whole-pixel "
                      "speeds: a fractal sky/ridge at depth 0.9 moving 3 px/frame, sinusoidal hills at 0.5 "
                      "moving 9, and roadside posts at 0.15 moving 24. `guidefiles.py truth` writes its true "
                      "depth and motion from the same constants (corpus.DEPTH_PAN). No cut.")


def build_depth_subject(corpus: Path) -> dict:
    p = DEPTH_SUBJECT
    frames, seconds = p["frames"], p["frames"] / FPS
    room, s = p["room"], p["subject"]
    ellipse = (f"if(lt(pow((X-{s['rx']})/{s['rx']}\\,2)+pow((Y-{s['ry']})/{s['ry']}\\,2)\\,1)\\,255\\,0)")
    graph = (
        frozen(f"mandelbrot=s={WIDTH + room['speed'] * frames + 16}x{HEIGHT}:r={FPS}:maxiter=400"
               f":start_scale=0.55:end_scale=0.55", seconds) +
        f",crop={WIDTH}:{HEIGHT}:x='{room['speed']}*n':y=0,format=rgba[room];"
        f"testsrc2=s={2 * s['rx']}x{2 * s['ry']}:r={FPS},trim=end_frame=1,format=rgba,"
        f"geq=r='r(X\\,Y)':g='g(X\\,Y)':b='b(X\\,Y)':a='{ellipse}',"
        f"loop=loop=-1:size=1:start=0,trim=duration={seconds}[subject];"
        f"[room][subject]overlay=shortest=1:eval=frame:x='{s['x0']}+{s['dx']}*n'"
        f":y='{s['y0']}+trunc({s['bob']}*sin(2*PI*n/{s['bob_period']}))',format=yuv420p")
    ffmpeg(["-filter_complex", graph, "-frames:v", str(frames), *ENCODE, "depth-subject.mkv"], corpus)
    return dict(name="depth-subject", category="depth", synthetic=True, cuts=[], text=[], depth_truth=True,
                notes="Near/far interior: a fractal 'room' at depth 0.85 drifting 1 px/frame behind a "
                      "person-sized textured ellipse at depth 0.2 that crosses 14 px/frame and bobs +-18 px, "
                      "occluding and disoccluding the background. True depth and motion are written by "
                      "`guidefiles.py truth` from corpus.DEPTH_SUBJECT. No cut.")


def describe(corpus: Path, clip: dict) -> dict:
    path = corpus / f"{clip['name']}.mkv"
    info = probe(path)
    hashes = framemd5(path)
    clip.update(file=path.name, width=info["width"], height=info["height"], fps=info["fps"], frames=len(hashes),
                duration_s=len(hashes) / info["fps"], digest=sequence_digest(hashes))
    return clip


BUILDERS = {"text-subtitles": build_text, "fine-detail": build_detail,
            "highlights-gradients": build_highlights, "cuts-motion": build_cuts,
            "cuts-similar": build_similar_cuts, "pan-fast": build_pan, "zoom-fast": build_zoom,
            "dissolve": build_dissolve, "flash-exposure": build_flash, "faces": build_faces,
            "real-film-cuts": build_real_film_cuts, "real-game-cuts": build_real_game_cuts,
            "real-game-motion": build_real_game_motion, "real-dissolve": build_real_dissolve,
            "orig-film-cuts-a": build_orig_film_cuts_a, "orig-film-cuts-b": build_orig_film_cuts_b,
            "orig-film-fade": build_orig_film_fade, "orig-faces": build_orig_faces,
            "orig-game-cuts": build_orig_game_cuts, "orig-game-motion": build_orig_game_motion,
            "orig-dissolve": build_orig_dissolve,
            "orig-film-motion-a": build_orig_film_motion_a,
            "orig-film-motion-b": build_orig_film_motion_b,
            "depth-pan": build_depth_pan, "depth-subject": build_depth_subject,
            "still-hold": build_still_hold, "pan-slow": build_pan_slow,
            "pan-slow-fractal": build_pan_slow_fractal}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", type=Path, default=CORPUS)
    parser.add_argument("--clips", nargs="*", help="build only these clips; the manifest then describes "
                                                   "only them, so point --corpus somewhere of its own")
    parser.add_argument("--check", action="store_true", help="recompute digests of an existing corpus")
    args = parser.parse_args()
    corpus: Path = args.corpus
    unknown = sorted(set(args.clips or ()) - set(BUILDERS))
    if unknown:
        print(f"unknown clips: {', '.join(unknown)}; known: {', '.join(BUILDERS)}", file=sys.stderr)
        return 1
    if args.check:
        import json as _json
        manifest = _json.loads((corpus / "manifest.json").read_text(encoding="utf-8"))
        drift = 0
        for clip in manifest["clips"]:
            digest = sequence_digest(framemd5(corpus / clip["file"]))
            same = digest == clip["digest"]
            drift += not same
            print(f"{clip['name']}: {'ok' if same else 'DIGEST DRIFT'} {digest}")
        # The camera-original clips are also checked against the digests tracked in
        # git, not only against the manifest that was written beside them. A local
        # manifest agrees with itself by construction; only the tracked file can
        # tell a clean checkout that a rebuild produced the pixels the committed
        # labels were verified against.
        expected = CAMERA_ORIGINAL_DIGESTS
        if expected.exists():
            tracked = _json.loads(expected.read_text(encoding="utf-8"))["clips"]
            built = {c["name"]: c for c in manifest["clips"]}
            for row in tracked:
                have = built.get(row["name"])
                if not have:
                    print(f"{row['name']}: not built (source absent), tracked digest not checked")
                    continue
                path = corpus / row["file"]
                digest = sequence_digest(framemd5(path))
                if digest != row["digest"]:
                    drift += 1
                    print(f"{row['name']}: DRIFT AGAINST TRACKED DIGEST - the upstream re-encoded or the "
                          f"span moved; re-verify the labels before citing them ({digest})")
                elif have["frames"] != row["frames"] or have["cuts"] != row["cuts"]:
                    drift += 1
                    print(f"{row['name']}: pixels match the tracked digest but the labels do not "
                          f"({have['frames']}/{have['cuts']} vs {row['frames']}/{row['cuts']})")
                else:
                    print(f"{row['name']}: matches the tracked digest and labels")
        return 0 if drift == 0 else 1
    corpus.mkdir(parents=True, exist_ok=True)
    version = subprocess.check_output([str(FFMPEG), "-version"], creationflags=FLAGS).decode().splitlines()[0]
    clips = []
    for name, builder in BUILDERS.items():
        if args.clips and name not in args.clips:
            continue
        clip = builder(corpus)
        if clip:
            clips.append(describe(corpus, clip))
            print(f"{clip['name']}: {clip['frames']} frames {clip['digest'][:16]}", flush=True)
    write_json(corpus / "manifest.json", dict(
        schema=1, generated=dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"), ffmpeg=version,
        width=WIDTH, height=HEIGHT, fps=FPS, container="matroska/ffv1 level 3 yuv420p", clips=clips))
    print(f"manifest: {corpus / 'manifest.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
