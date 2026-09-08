"""Generates the repeatable benchmark corpus (lossless FFV1 MKV, 1920x1080, 30 fps).

Every synthetic clip is produced by deterministic FFmpeg sources with explicit
seeds; ``manifest.json`` records per-clip category, hard-cut frame indices,
burned-in ground-truth text and an rgb24 frame-hash digest so a regenerated
corpus can be proven identical (``--check``).

    python tools/benchmark/corpus.py [--corpus DIR] [--check]
"""
from __future__ import annotations

import argparse
import datetime as dt
import subprocess
import sys
from pathlib import Path

from common import BUILD, CORPUS, FFMPEG, FLAGS, framemd5, probe, sequence_digest, write_json

WIDTH, HEIGHT, FPS = 1920, 1080, 30
ENCODE = ["-c:v", "ffv1", "-level", "3", "-coder", "1", "-context", "1", "-g", "1", "-slices", "4",
          "-pix_fmt", "yuv420p", "-r", str(FPS), "-an"]
FONT_MONO = "C\\\\:/Windows/Fonts/consola.ttf"
FONT_UI = "C\\\\:/Windows/Fonts/segoeui.ttf"
FACE_FIXTURE = BUILD / "runtime-comparison-20260907" / "fixtures" / "mafia-60s.mkv"

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
    graph = ";".join(f"{s},setpts=PTS-STARTPTS,format=yuv420p[s{i}]" for i, s in enumerate(segments))
    graph += ";" + "".join(f"[s{i}]" for i in range(len(segments))) + f"concat=n={len(segments)}:v=1:a=0"
    ffmpeg(["-filter_complex", graph, *ENCODE, "cuts-motion.mkv"], corpus)
    cuts = [round(seg * FPS * i) for i in range(1, len(segments))]
    return dict(name="cuts-motion", category="cuts", synthetic=True, cuts=cuts, text=[],
                notes="Five dissimilar 1.5 s segments (fast pan, fractal zoom, cellular automaton, fast rotation, "
                      "fast gradients) hard-cut together.")


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


def describe(corpus: Path, clip: dict) -> dict:
    path = corpus / f"{clip['name']}.mkv"
    info = probe(path)
    hashes = framemd5(path)
    clip.update(file=path.name, width=info["width"], height=info["height"], fps=info["fps"], frames=len(hashes),
                duration_s=len(hashes) / info["fps"], digest=sequence_digest(hashes))
    return clip


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", type=Path, default=CORPUS)
    parser.add_argument("--check", action="store_true", help="recompute digests of an existing corpus")
    args = parser.parse_args()
    corpus: Path = args.corpus
    if args.check:
        manifest = __import__("json").loads((corpus / "manifest.json").read_text(encoding="utf-8"))
        drift = 0
        for clip in manifest["clips"]:
            digest = sequence_digest(framemd5(corpus / clip["file"]))
            same = digest == clip["digest"]
            drift += not same
            print(f"{clip['name']}: {'ok' if same else 'DIGEST DRIFT'} {digest}")
        return 0 if drift == 0 else 1
    corpus.mkdir(parents=True, exist_ok=True)
    version = subprocess.check_output([str(FFMPEG), "-version"], creationflags=FLAGS).decode().splitlines()[0]
    clips = []
    for builder in (build_text, build_detail, build_highlights, build_cuts, build_faces):
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
