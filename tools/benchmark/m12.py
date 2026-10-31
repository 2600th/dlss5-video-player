"""M1.2 measurement session (2026-10-31): DLSS-NR 310.9.16, its two networks, RenoDX 10.8.0 (P3.14).

Renders through ``run.py``'s worker driver at the Lossless rung, as ``m1.py`` does, and
swaps files in each profile's runtime clone only: the add-on (6.5.3 pinned, 10.8.0), the
NR DLL (310.8.SF-v2 pinned, 310.9.16) and, for the ``-sl`` profiles, Streamline 2.14.16
and DLSS SR 310.9.16. ``external/runtime`` and the snapshot are untouched. The archives
are verified by SHA-256 before extraction (``tools/fetch_neural_runtime.ps1`` pins them);
point ``--staging`` at the directory holding ``<tag>/extracted/``.

    python tools/benchmark/m12.py fade                       # builds the fade-to-black clips
    python tools/benchmark/m12.py render --corpus build-upscaling/benchmark-corpus-m1
    python tools/benchmark/m12.py render --corpus build-upscaling/benchmark-corpus-m12-fade --profiles ...
    python tools/benchmark/m12.py looks  --corpus build-upscaling/benchmark-corpus-m1
    python tools/benchmark/m12.py fadecheck --corpus build-upscaling/benchmark-corpus-m12-fade
    python tools/benchmark/m12.py pace --corpus build-upscaling/benchmark-corpus-4k --repeats 3
    python tools/benchmark/m12.py timing
"""
from __future__ import annotations

import argparse
import csv
import json
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np

import m1
import run
from common import ANALYSIS, BUILD, FFMPEG, FLAGS, RUNS, framemd5, load_manifest, sequence_digest, write_json

REPO = Path(__file__).resolve().parents[2]
STAGING = REPO / "external" / "runtime-staging"


def staged(tag: str, name: str) -> Path:
    return STAGING / tag / "extracted" / name


# Clone file -> source, per runtime variant. The pinned set is the snapshot itself.
VARIANTS = {
    "653-308": {},
    "108-308": {"renodx-dlss5.addon64": lambda: staged("renodx-dlss5-10.8.0", "renodx-dlss5.addon64")},
    "108-916": {"renodx-dlss5.addon64": lambda: staged("renodx-dlss5-10.8.0", "renodx-dlss5.addon64"),
                "nvngx_dlssnr.dll": lambda: staged("dlssnr-310.9.16", "nvngx_dlssnr.dll")},
}
# Streamline 2.14.16 and DLSS SR 310.9.16 on top: 2.14.16-v2 ships no sl.dlss_nr.dll, so the
# pinned 2.13 one stays (the player runs EnableHooks=2 and loads no Streamline plugin).
SL = {f"sl.{n}.dll": (lambda n=n: staged("streamline-2.14.16.0-v2", f"sl.{n}.dll"))
      for n in ("common", "dlss", "dlss_g", "interposer", "nis", "pcl", "reflex")}
SL["nvngx_dlss.dll"] = lambda: staged("dlss-310.9.16", "nvngx_dlss.dll")
VARIANTS["108-916-sl"] = {**VARIANTS["108-916"], **SL}
VARIANTS["108-308-sl"] = {**VARIANTS["108-308"], **SL}

# What 10.8.0 needs written beside the player's keys (decided by the discovery profiles):
# its own schema stamp, Detail stability off and Independent skin off as on 8.5.
FIXED_108 = {"NRDetailStability": "1", "NRSkinIndependent": "0"}
GENTLE = {"NRIntensity": "0.500000", "NRLocalStructure": "0.500000", "NRSkinStructure": "0.000000",
          "NRPass2Intensity": "0.500000", "NRPass3Intensity": "0.500000", "NRPass4Intensity": "0.500000"}
STRONG = {"NRLocalTone": "2.000000", "NRLocalStructure": "1.500000"}
DETAIL = {"NRColorStrength": "0.000000", "NRPass2Color": "0.000000", "NRPass3Color": "0.000000",
          "NRPass4Color": "0.000000"}
KNOBS = {"std": {}, "gentle": GENTLE, "strong": STRONG, "detail": DETAIL, "tone200": {"NRLocalTone": "2.000000"},
         "int000": {"NRIntensity": "0.000000", "NRPass2Intensity": "0.000000", "NRPass3Intensity": "0.000000",
                    "NRPass4Intensity": "0.000000"},
         "natural": {"NRStyle": "1"}, "cinematic": {"NRStyle": "2"},
         # 10.8.0 refuses colour strength 0 as "every pass at zero strength (exact passthrough)",
         # so Detail only fails there; 0.01 is the nearest value it renders.
         "detail001": {"NRColorStrength": "0.010000", "NRPass2Color": "0.010000", "NRPass3Color": "0.010000",
                       "NRPass4Color": "0.010000"}}

# name -> (overrides on top of m1.PLAYER, repeats, variant, ConfigVersion or None for run.py's "6")
PROFILES: dict[str, tuple[dict, int, str, str | None]] = {
    # The pinned runtime, as 0.28.3 renders it.
    "m12-base": ({}, 2, "653-308", None),
    # 10.8.0 with the player's config written unchanged (ConfigVersion=6, NRPreset=0).
    "m12-r108-308-asw": ({}, 1, "108-308", None),
    "m12-r108-916-asw": ({}, 1, "108-916", None),
}
for _variant, _tag in (("108-308", "308"), ("108-916", "916")):
    for _preset in ("0", "1", "2", "3"):
        PROFILES[f"m12-{_tag}-p{_preset}"] = ({**FIXED_108, "NRPreset": _preset}, 2 if _preset in "12" else 1,
                                             _variant, "CFG")
for _knob, _over in KNOBS.items():
    if _knob == "std":
        continue
    PROFILES[f"m12-base-{_knob}"] = (_over, 1, "653-308", None)
    PROFILES[f"m12-308-a-{_knob}"] = ({**FIXED_108, "NRPreset": "1", **_over}, 1, "108-308", "CFG")
    PROFILES[f"m12-916-a-{_knob}"] = ({**FIXED_108, "NRPreset": "1", **_over}, 1, "108-916", "CFG")
    PROFILES[f"m12-916-b-{_knob}"] = ({**FIXED_108, "NRPreset": "2", **_over}, 1, "108-916", "CFG")
PROFILES["m12-916-a-sl"] = ({**FIXED_108, "NRPreset": "1"}, 1, "108-916-sl", "CFG")
PROFILES["m12-916-b-sl"] = ({**FIXED_108, "NRPreset": "2"}, 1, "108-916-sl", "CFG")
PROFILES["m12-308-a-sl"] = ({**FIXED_108, "NRPreset": "1"}, 1, "108-308-sl", "CFG")

# The schema stamp 10.8.0 writes on a config it has loaded once: the asw profiles' copied
# ReShade.ini reads ConfigVersion=10 and ConfigRevision=1 after it migrated the player's 6
# (and left ReShade.ini.v10-backup). "CFG" in a profile resolves to it.
CONFIG_VERSION_108 = "10"


def spec(name: str, flags: list[str] | None = None) -> dict:
    overrides, _, variant, _ = PROFILES[name]
    return run.profile("mv=1,depth=1", {**m1.PLAYER, **overrides}, description=f"M1.2 {name} ({variant})",
                       worker_flags=m1.LOSSLESS if flags is None else flags)


def prepare(name: str, flags: list[str] | None = None, tag: str | None = None) -> Path:
    """Clones the runtime for the profile, swaps the variant's files in the clone only and stamps the schema."""
    _, _, variant, version = PROFILES[name]
    runtime = run.prepare(tag or name, spec(name, flags), 1, False)
    for target, source in VARIANTS[variant].items():
        src = source()
        dst = runtime / target
        if not dst.exists() or dst.stat().st_size != src.stat().st_size or dst.read_bytes() != src.read_bytes():
            shutil.copy2(src, dst)
    if version:
        ini = runtime / "ReShade.ini"
        text = ini.read_text(encoding="utf-8")
        stamp = CONFIG_VERSION_108 if version == "CFG" else version
        if "ConfigVersion=6\n" in text:
            ini.write_text(text.replace("ConfigVersion=6\n", f"ConfigVersion={stamp}\n"), encoding="utf-8")
    return runtime


def run_dir(clip: str, name: str, rep: int = 1) -> Path:
    return RUNS / f"{clip}__{name}__{rep}"


def render(args) -> int:
    manifest = {c["name"]: c for c in load_manifest(args.corpus)["clips"]}
    names = args.profiles or list(PROFILES)
    failures = 0
    for name in names:
        prepare(name)
        for rep in range(1, PROFILES[name][1] + 1):
            for clip in args.clips or list(manifest):
                result = run.run_one(manifest[clip], name, spec(name), rep, args.timeout, args.corpus, False)
                failures += not result["result"].get("ok")
    print(f"{failures} failed")
    return 1 if failures else 0


def pace(args) -> int:
    """Interleaved pace runs at the player's default cache rung (NVENC), one clone per profile."""
    manifest = {c["name"]: c for c in load_manifest(args.corpus)["clips"]}
    names = args.profiles or ["m12-base", "m12-916-p1", "m12-916-p2"]
    failures = 0
    for rep in range(1, args.repeats + 1):
        for clip in args.clips or list(manifest):
            for name in names:
                tag = f"{name}-pace"
                prepare(name, flags=[], tag=tag)
                result = run.run_one(manifest[clip], tag, spec(name, flags=[]), rep, args.timeout, args.corpus,
                                     False)
                failures += not result["result"].get("ok")
    print(f"{failures} failed")
    return 1 if failures else 0


# ---- the fade-to-black clips --------------------------------------------------------

def fade(args) -> int:
    """Builds the fade clips from the M1 corpus with the bundled FFmpeg (FFV1, yuv420p, 30 fps, 1080p).

    A is real-game-motion (76 frames), B is orig-game-motion played forward then backward
    (132 frames, continuous motion with no cut). Each clip ends in B; `fade-control` is B
    alone, so B's frames can be compared between a render that went through a fade to
    black and one that never saw black.
    """
    src = args.source_corpus
    out = args.corpus
    out.mkdir(parents=True, exist_ok=True)
    a, b = src / "real-game-motion.mkv", src / "orig-game-motion.mkv"
    common = ["-v", "error", "-y"]
    enc = ["-c:v", "ffv1", "-level", "3", "-pix_fmt", "yuv420p", "-r", "30"]
    # B ping-pong, the shared tail.
    bb = out / "_b.mkv"
    subprocess.check_call([str(FFMPEG), *common, "-i", str(b), "-filter_complex",
                           "[0:v]split[f][r];[r]reverse[rv];[f][rv]concat=n=2:v=1[o]", "-map", "[o]", *enc,
                           str(bb)], creationflags=FLAGS)
    black = "color=c=black:s=1920x1080:r=30:d=2,format=yuv420p,setsar=1"
    clips = {
        # A fades out over its last 30 frames, 60 frames of black, then a hard cut into B.
        "fade-hard": f"[0:v]fade=t=out:st=1.5333:d=1,setsar=1[a];{black}[k];[a][k][1:v]concat=n=3:v=1[o]",
        # The same, with B fading in over 15 frames instead of the cut.
        "fade-in": f"[0:v]fade=t=out:st=1.5333:d=1,setsar=1[a];{black}[k];[1:v]fade=t=in:st=0:d=0.5,setsar=1[bi];"
                   f"[a][k][bi]concat=n=3:v=1[o]",
        "fade-control": "[1:v]setsar=1[o]",
    }
    entries = []
    for name, graph in clips.items():
        path = out / f"{name}.mkv"
        subprocess.check_call([str(FFMPEG), *common, "-i", str(a), "-i", str(bb), "-filter_complex", graph,
                               "-map", "[o]", *enc, str(path)], creationflags=FLAGS)
        frames = int(subprocess.check_output([str(FFMPEG).replace("ffmpeg.exe", "ffprobe.exe"), "-v", "error",
                                              "-count_frames", "-select_streams", "v:0", "-show_entries",
                                              "stream=nb_read_frames", "-of", "csv=p=0", str(path)],
                                             creationflags=FLAGS).decode().strip())
        tail = 132  # B's frames are the last 132 of every clip
        entries.append(dict(name=name, category="fade", file=path.name, width=1920, height=1080, fps=30.0,
                            frames=frames, cuts=[frames - tail] if name == "fade-hard" else [],
                            soft_cuts=[[46, 75]] + ([[frames - tail, frames - tail + 14]] if name == "fade-in" else []),
                            b_first=frames - tail, synthetic=True,
                            notes="M1.2 fade-to-black clip: real-game-motion fading to black, 2 s of black, then "
                                  "orig-game-motion forward and backward (B)." if name != "fade-control" else
                                  "B alone: orig-game-motion forward and backward."))
    bb.unlink()
    write_json(out / "manifest.json", dict(schema=1, clips=entries))
    print(json.dumps(entries, indent=1))
    return 0


def fadecheck(args) -> int:
    """B's frames after the fade against B rendered with no fade before it: mean luma, chroma and |delta|."""
    manifest = {c["name"]: c for c in load_manifest(args.corpus)["clips"]}
    control = manifest["fade-control"]
    rows = []
    for name in args.profiles or ["m12-base", "m12-916-p1", "m12-916-p2", "m12-308-p1"]:
        ref = run_dir("fade-control", name)
        if not (ref / "output.mkv").exists():
            continue
        ref_frames = list(m1.planes(ref / "output.mkv", 1920, 1080, 1))
        src_frames = list(m1.planes(args.corpus / control["file"], 1920, 1080, 1))
        for clip in ("fade-hard", "fade-in"):
            d = run_dir(clip, name)
            if not (d / "output.mkv").exists():
                continue
            first = manifest[clip]["b_first"]
            frames = list(m1.planes(d / "output.mkv", 1920, 1080, 1))[first:]
            src_tail = list(m1.planes(args.corpus / manifest[clip]["file"], 1920, 1080, 1))[first:]
            for lo, hi in ((0, 30), (30, 66), (66, 132)):
                dy, dc, ad, sdy, sad = [], [], [], [], []
                for i in range(lo, min(hi, len(frames), len(ref_frames))):
                    (y, u, v), (ry, ru, rv) = frames[i], ref_frames[i]
                    (sy, su, sv), (cy, cu, cv) = src_tail[i], src_frames[i]
                    dy.append(y.mean() - ry.mean())
                    dc.append(np.hypot(u - 128, v - 128).mean() - np.hypot(ru - 128, rv - 128).mean())
                    ad.append(np.abs(y - ry).mean())
                    sdy.append(sy.mean() - cy.mean())  # the sources' own difference (0 for fade-hard)
                    sad.append(np.abs(sy - cy).mean())
                rows.append(dict(profile=name, clip=clip, b_frames=f"{lo}-{hi - 1}", luma_delta=float(np.mean(dy)),
                                 chroma_delta=float(np.mean(dc)), mean_abs_luma=float(np.mean(ad)),
                                 source_luma_delta=float(np.mean(sdy)), source_mean_abs_luma=float(np.mean(sad))))
                print(json.dumps(rows[-1]), flush=True)
    out = args.out / "fade"
    out.mkdir(parents=True, exist_ok=True)
    with (out / "fade.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        for r in rows:
            w.writerow({k: (round(v, 4) if isinstance(v, float) else v) for k, v in r.items()})
    return 0


# ---- distances (m1.py's knobs method) --------------------------------------------------

LOOKS = [  # (row, reference run, candidate run)
    ("Pinned rendered again", ("m12-base", 1), ("m12-base", 2)),
    ("10.8.0 + 310.8 as written vs pinned", ("m12-base", 1), ("m12-r108-308-asw", 1)),
    ("10.8.0 + 310.8 fixed, preset 1 vs pinned", ("m12-base", 1), ("m12-308-p1", 1)),
    ("10.8.0 + 310.8 fixed, preset 1 rendered again", ("m12-308-p1", 1), ("m12-308-p1", 2)),
    ("10.8.0 + 310.8 preset 0 vs 1", ("m12-308-p1", 1), ("m12-308-p0", 1)),
    ("10.8.0 + 310.8 preset 2 vs 1", ("m12-308-p1", 1), ("m12-308-p2", 1)),
    ("10.8.0 + 310.8 preset 3 vs 1", ("m12-308-p1", 1), ("m12-308-p3", 1)),
    ("310.9.16 A vs 310.8 (both 10.8.0, preset 1)", ("m12-308-p1", 1), ("m12-916-p1", 1)),
    ("310.9.16 A vs pinned", ("m12-base", 1), ("m12-916-p1", 1)),
    ("310.9.16 A rendered again", ("m12-916-p1", 1), ("m12-916-p1", 2)),
    ("310.9.16 B vs A", ("m12-916-p1", 1), ("m12-916-p2", 1)),
    ("310.9.16 B vs pinned", ("m12-base", 1), ("m12-916-p2", 1)),
    ("310.9.16 B rendered again", ("m12-916-p2", 1), ("m12-916-p2", 2)),
    ("310.9.16 preset 0 vs A", ("m12-916-p1", 1), ("m12-916-p0", 1)),
    ("310.9.16 preset 0 vs B", ("m12-916-p2", 1), ("m12-916-p0", 1)),
    ("310.9.16 preset 3 vs A", ("m12-916-p1", 1), ("m12-916-p3", 1)),
    ("310.9.16 preset 3 vs B", ("m12-916-p2", 1), ("m12-916-p3", 1)),
    ("310.9.16 as written vs B", ("m12-916-p2", 1), ("m12-r108-916-asw", 1)),
    ("310.9.16 A, SL 2.14.16 + SR 310.9.16 vs A", ("m12-916-p1", 1), ("m12-916-a-sl", 1)),
    ("310.9.16 B, SL 2.14.16 + SR 310.9.16 vs B", ("m12-916-p2", 1), ("m12-916-b-sl", 1)),
    ("310.8, SL 2.14.16 + SR 310.9.16 vs 310.8", ("m12-308-p1", 1), ("m12-308-a-sl", 1)),
]
for _knob in KNOBS:
    if _knob == "std":
        continue
    LOOKS += [(f"pinned {_knob} vs Standard", ("m12-base", 1), (f"m12-base-{_knob}", 1)),
              (f"310.8 (10.8.0) {_knob} vs Standard", ("m12-308-p1", 1), (f"m12-308-a-{_knob}", 1)),
              (f"A {_knob} vs Standard", ("m12-916-p1", 1), (f"m12-916-a-{_knob}", 1)),
              (f"B {_knob} vs Standard", ("m12-916-p2", 1), (f"m12-916-b-{_knob}", 1)),
              (f"310.8 (10.8.0) {_knob} vs pinned {_knob}", (f"m12-base-{_knob}", 1), (f"m12-308-a-{_knob}", 1)),
              (f"A {_knob} vs 310.8 (10.8.0) {_knob}", (f"m12-308-a-{_knob}", 1), (f"m12-916-a-{_knob}", 1))]
LOOKS += [("pinned colour 0.01 vs pinned colour 0 (Detail only)", ("m12-base-detail", 1), ("m12-base-detail001", 1)),
          ("310.8 (10.8.0) colour 0.01 vs pinned colour 0", ("m12-base-detail", 1), ("m12-308-a-detail001", 1))]


def looks(args) -> int:
    manifest = {c["name"]: c for c in load_manifest(args.corpus)["clips"]}
    out = args.out / "looks"
    rows = []
    for clip in args.clips or list(manifest):
        info = manifest[clip]
        for label, (rn, rr), (cn, cr) in LOOKS:
            a, b = run_dir(clip, rn, rr), run_dir(clip, cn, cr)
            if not ((a / "output.mkv").exists() and (b / "output.mkv").exists()):
                continue
            da = sequence_digest(framemd5(a / "output.mkv", a / "frames.md5"))
            db = sequence_digest(framemd5(b / "output.mkv", b / "frames.md5"))
            d = m1.distances(args.corpus / info["file"], a / "output.mkv", b / "output.mkv", info["width"],
                             info["height"], args.every_rgb)
            rows.append(dict(clip=clip, row=label, reference=f"{rn}#{rr}", candidate=f"{cn}#{cr}",
                             identical=da == db, **d))
            print(json.dumps(rows[-1]), flush=True)
    write_json(out / "looks.json", rows)
    with (out / "looks.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        for r in rows:
            w.writerow({k: (round(v, 4) if isinstance(v, float) else v) for k, v in r.items()})
    return 0


def timing(args) -> int:
    """m1.py's pace and memory table, over this session's runs."""
    rows = []
    for d in sorted(RUNS.glob("*__m12-*__*")):
        clip, name, rep = d.name.split("__")
        if not (d / "result.json").exists() or (args.profiles and name not in args.profiles):
            continue
        r = json.loads((d / "result.json").read_text(encoding="utf-8"))
        res = r["result"]
        baseline = peak = None
        if (d / "gpu.csv").exists():
            samples = list(csv.DictReader((d / "gpu.csv").open()))
            if samples:
                baseline = float(samples[0]["total_used_mib"])
                peak = max(float(x["total_used_mib"]) for x in samples)
        rows.append(dict(clip=clip, profile=name, rep=int(rep), ok=res.get("ok"), frames=res.get("frame_count"),
                         encoder=res.get("encoder"), processing_fps=r.get("processing_fps"),
                         wall_seconds=r.get("wall_seconds"), first_frame_seconds=r.get("first_frame_seconds"),
                         neural_gpu_ms_p50=res.get("neural_gpu_ms_p50"), neural_gpu_ms_p95=res.get("neural_gpu_ms_p95"),
                         peak_local_vram_mib=res.get("peak_local_vram_mib"), nvml_baseline_mib=baseline,
                         nvml_peak_mib=peak, nvml_delta_mib=(peak - baseline) if peak is not None else None))
    out = args.out / "timing"
    out.mkdir(parents=True, exist_ok=True)
    with (out / "timing.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        for row in rows:
            w.writerow({k: (round(v, 4) if isinstance(v, float) else v) for k, v in row.items()})
    print(f"{len(rows)} runs -> {out / 'timing.csv'}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["render", "pace", "fade", "fadecheck", "looks", "timing"])
    parser.add_argument("--corpus", type=Path, default=BUILD / "benchmark-corpus-m1")
    parser.add_argument("--source-corpus", type=Path, default=BUILD / "benchmark-corpus-m1")
    parser.add_argument("--clips", nargs="*")
    parser.add_argument("--profiles", nargs="*")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--every-rgb", type=int, default=4, help="frame sampling for the distances")
    parser.add_argument("--out", type=Path, default=ANALYSIS / "m12")
    args = parser.parse_args()
    return {"render": render, "pace": pace, "fade": fade, "fadecheck": fadecheck, "looks": looks,
            "timing": timing}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
