"""M1 measurement session (2026-10-16): stacked-pass drift, NVIDIA's NR defaults, RenoDX 8.5.

Renders through ``run.py``'s own worker driver, every profile at the Lossless rung
(``--cache-quality lossless``: FFV1 10-bit from the P010 capture) so nothing measured
here carries an NVENC loss, and the lossless renders double as the reference for the
NVENC AQ measurement (``nvenc_aq.py``).

Profiles start from exactly what the player writes for a default ``NeuralSettings``
(``NeuralAddonOverridesFor``: the eleven keys plus ``NRPass{2..4}Intensity/Color``),
with the shipped ``NRNormGovernor=1``; the ``-g0`` profiles set it to 0 as knobs.py does.

    python tools/benchmark/m1.py render --corpus build-upscaling/benchmark-corpus-m1
    python tools/benchmark/m1.py drift  --corpus build-upscaling/benchmark-corpus-m1
    python tools/benchmark/m1.py looks  --corpus build-upscaling/benchmark-corpus-m1
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

import run
from common import ANALYSIS, CORPUS, FFMPEG, FLAGS, RUNS, framemd5, load_manifest, sequence_digest, write_json

# NeuralAddonOverridesFor(NeuralSettings{}) - src/NeuralSettings.cpp, release/0.29.0.
PLAYER = {
    "NRIntensity": "1.000000", "NRLocalTone": "1.000000", "NRLocalStructure": "1.000000",
    "NRSkinStructure": "-1.000000", "NRColorStrength": "1.000000", "NRPreset": "0", "NRStyle": "0",
    "NRAutoMask": "1", "NRPasses": "1", "NRChainedHistory": "1", "NRNormGovernor": "1",
    "NRPass2Intensity": "1.000000", "NRPass2Color": "1.000000", "NRPass3Intensity": "1.000000",
    "NRPass3Color": "1.000000", "NRPass4Intensity": "1.000000", "NRPass4Color": "1.000000",
}
# NVIDIA's RTX Remix defaults (dxvk-remix ecf646d): intensity 1.0, local structure 0.7,
# local tone 0.3, skin 0.5; automatic mask left on as the player ships it.
REMIX = {"NRIntensity": "1.000000", "NRLocalStructure": "0.700000", "NRLocalTone": "0.300000",
         "NRSkinStructure": "0.500000"}
LOSSLESS = ["--cache-quality", "lossless"]
ADDON_85 = Path(__file__).resolve().parents[2] / "build-upscaling" / "benchmark-work" / "renodx-8.5.0-rc10" / \
    "renodx-dlss5.addon64"

# name -> (overrides on top of PLAYER, repeats, add-on swap)
PROFILES = {
    "m1-p1": ({}, 2, None),
    "m1-p2": ({"NRPasses": "2"}, 1, None),
    "m1-p3": ({"NRPasses": "3"}, 1, None),
    "m1-p4": ({"NRPasses": "4"}, 2, None),
    "m1-remix": (REMIX, 1, None),
    # Neighbours of the Remix look: the shipped Gentle preset (NeuralPresets.h; the
    # player writes NRPass{2..4}Intensity with it, inert at one pass) and tone 0.30 alone.
    "m1-gentle": ({"NRIntensity": "0.500000", "NRLocalStructure": "0.500000", "NRSkinStructure": "0.000000",
                   "NRPass2Intensity": "0.500000", "NRPass3Intensity": "0.500000", "NRPass4Intensity": "0.500000"},
                  1, None),
    "m1-tone030": ({"NRLocalTone": "0.300000"}, 1, None),
    "m1-g0-p1": ({"NRNormGovernor": "0"}, 2, None),
    "m1-g0-p4": ({"NRNormGovernor": "0", "NRPasses": "4"}, 1, None),
    "m1-g0-remix": ({"NRNormGovernor": "0", **REMIX}, 1, None),
    # RenoDX 8.5.0-rc10 in place of 6.5.3, the player's config written unchanged.
    "m1-r85-p1": ({}, 2, ADDON_85),
    "m1-r85-g0-p1": ({"NRNormGovernor": "0"}, 2, ADDON_85),
    "m1-r85-remix": (REMIX, 1, ADDON_85),
    # 8.5's Detail stability (NRDetailStability: 0 Auto - on in the player's after-upscale
    # path - 1 Off, 2 Always) is a temporal filter on the model's edit that 6.5.3 does not have.
    "m1-r85-ds1": ({"NRDetailStability": "1"}, 2, ADDON_85),
    # 8.5's Independent skin (NRSkinIndependent, default 1) sends Local structure at most 1:
    # the Strong preset's structure 1.50 against 6.5.3, with and without it (orig-faces only).
    "m1-s150": ({"NRLocalStructure": "1.500000"}, 1, None),
    "m1-r85-s150": ({"NRLocalStructure": "1.500000", "NRDetailStability": "1"}, 1, ADDON_85),
    "m1-r85-s150-si0": ({"NRLocalStructure": "1.500000", "NRDetailStability": "1", "NRSkinIndependent": "0"}, 1,
                        ADDON_85),
}


def spec(name: str) -> dict:
    overrides, _, _ = PROFILES[name]
    return run.profile("mv=1,depth=1", {**PLAYER, **overrides}, description=f"M1 {name}", worker_flags=LOSSLESS)


def prepare(name: str) -> None:
    """Clones the runtime for the profile and, for an 8.5 profile, swaps the add-on in the clone only."""
    _, _, addon = PROFILES[name]
    runtime = run.prepare(name, spec(name), 1, False)
    if addon:
        target = runtime / "renodx-dlss5.addon64"
        if target.read_bytes() != addon.read_bytes():
            shutil.copy2(addon, target)


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


# ---- frame statistics -----------------------------------------------------------------

def planes(path: Path, width: int, height: int, every: int):
    """Yields (Y, U, V) of every ``every``-th frame as uint16 arrays at the file's own depth.

    Read as planar YUV without any colour conversion: the worker's FFV1 is yuv420p10le
    (limited range, BT.709 as written by the capture) and the corpus is yuv420p.
    """
    probe = subprocess.check_output([str(FFMPEG).replace("ffmpeg.exe", "ffprobe.exe"), "-v", "error",
                                     "-select_streams", "v:0", "-show_entries", "stream=pix_fmt", "-of",
                                     "csv=p=0", str(path)], creationflags=FLAGS).decode().strip()
    ten = "10" in probe
    fmt = "yuv420p10le" if ten else "yuv420p"
    bpp = 2 if ten else 1
    y_bytes, c_bytes = width * height * bpp, (width // 2) * (height // 2) * bpp
    proc = subprocess.Popen([str(FFMPEG), "-v", "error", "-i", str(path), "-map", "0:v:0", "-f", "rawvideo",
                             "-pix_fmt", fmt, "-"], stdout=subprocess.PIPE, creationflags=FLAGS, bufsize=0)
    index = 0
    frame_bytes = y_bytes + 2 * c_bytes
    try:
        while True:
            buf = bytearray()
            while len(buf) < frame_bytes:
                chunk = proc.stdout.read(frame_bytes - len(buf))
                if not chunk:
                    return
                buf.extend(chunk)
            if index % every == 0:
                dt = np.uint16 if ten else np.uint8
                arr = np.frombuffer(bytes(buf), dtype=dt)
                y = arr[:width * height].reshape(height, width).astype(np.float64)
                u = arr[width * height:width * height + c_bytes // bpp].reshape(height // 2, width // 2)
                v = arr[width * height + c_bytes // bpp:].reshape(height // 2, width // 2)
                scale = 4.0 if ten else 1.0  # 10-bit codes to the 8-bit scale
                yield y / scale, u.astype(np.float64) / scale, v.astype(np.float64) / scale
            index += 1
    finally:
        proc.stdout.close()
        proc.wait()


def rgb_from_yuv(y, u, v):
    """BT.709 limited-range to full-range R'G'B' on the 8-bit scale (chroma upsampled by repetition)."""
    u = np.repeat(np.repeat(u, 2, axis=0), 2, axis=1)[:y.shape[0], :y.shape[1]] - 128.0
    v = np.repeat(np.repeat(v, 2, axis=0), 2, axis=1)[:y.shape[0], :y.shape[1]] - 128.0
    yy = (y - 16.0) * (255.0 / 219.0)
    cb, cr = u * (255.0 / 224.0), v * (255.0 / 224.0)
    r = yy + 1.5748 * cr
    g = yy - 0.1873 * cb - 0.4681 * cr
    b = yy + 1.8556 * cb
    return np.clip(r, 0, 255), np.clip(g, 0, 255), np.clip(b, 0, 255)


def means(path: Path, width: int, height: int, every: int) -> dict:
    ys, rs, gs, bs, frames = [], [], [], [], 0
    for y, u, v in planes(path, width, height, every):
        r, g, b = rgb_from_yuv(y, u, v)
        ys.append(y.mean()); rs.append(r.mean()); gs.append(g.mean()); bs.append(b.mean())
        frames += 1
    return dict(frames=frames, Y=float(np.mean(ys)), R=float(np.mean(rs)), G=float(np.mean(gs)),
                B=float(np.mean(bs)), Y_per_frame=[float(x) for x in ys])


def drift(args) -> int:
    manifest = {c["name"]: c for c in load_manifest(args.corpus)["clips"]}
    out = args.out / "drift"
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    sets = [("m1-p1", 1), ("m1-p1", 2), ("m1-p2", 1), ("m1-p3", 1), ("m1-p4", 1), ("m1-p4", 2),
            ("m1-g0-p1", 1), ("m1-g0-p1", 2), ("m1-g0-p4", 1)]
    for clip in args.clips or list(manifest):
        info = manifest[clip]
        src = means(args.corpus / info["file"], info["width"], info["height"], args.every)
        rows.append(dict(clip=clip, run="source", **{k: src[k] for k in ("frames", "Y", "R", "G", "B")}))
        for name, rep in sets:
            d = run_dir(clip, name, rep)
            if not (d / "output.mkv").exists():
                continue
            m = means(d / "output.mkv", info["width"], info["height"], args.every)
            digest = sequence_digest(framemd5(d / "output.mkv", d / "frames.md5"))
            rows.append(dict(clip=clip, run=f"{name}#{rep}", digest=digest[:16],
                             **{k: m[k] for k in ("frames", "Y", "R", "G", "B")}))
            print(json.dumps(rows[-1]), flush=True)
    with (out / "drift.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["clip", "run", "frames", "Y", "R", "G", "B", "digest"])
        w.writeheader()
        for r in rows:
            w.writerow({k: (round(v, 4) if isinstance(v, float) else v) for k, v in r.items()})
    write_json(out / "drift.json", rows)
    return 0


# ---- knobs-style distances (8-bit rgb24, every 4th frame, as knobs-upper-20261003) -----

def rgb_frames(path: Path, width: int, height: int, every: int):
    from common import FrameReader
    for i, f in enumerate(FrameReader(path, width, height)):
        if i % every == 0:
            yield f


def distances(source: Path, ref: Path, cand: Path, width: int, height: int, every: int) -> dict:
    to_ref = to_src = ref_src = 0.0
    n = 0
    for s, a, b in zip(rgb_frames(source, width, height, every), rgb_frames(ref, width, height, every),
                       rgb_frames(cand, width, height, every)):
        s16, a16, b16 = s.astype(np.int16), a.astype(np.int16), b.astype(np.int16)
        to_ref += float(np.abs(b16 - a16).mean())
        to_src += float(np.abs(b16 - s16).mean())
        ref_src += float(np.abs(a16 - s16).mean())
        n += 1
    return dict(frames=n, change_to_reference=to_ref / n, distance_to_source=to_src / n,
                reference_to_source=ref_src / n, distance_share=to_src / ref_src if ref_src else None)


LOOKS = [  # (row, reference run, candidate run)
    ("Standard rendered again (gov 1)", ("m1-p1", 1), ("m1-p1", 2)),
    ("Standard rendered again (gov 0)", ("m1-g0-p1", 1), ("m1-g0-p1", 2)),
    ("Remix defaults vs Standard (gov 1)", ("m1-p1", 1), ("m1-remix", 1)),
    ("Remix defaults vs Standard (gov 0)", ("m1-g0-p1", 1), ("m1-g0-remix", 1)),
    ("Gentle preset vs Standard", ("m1-p1", 1), ("m1-gentle", 1)),
    ("Local tone 0.30 alone vs Standard", ("m1-p1", 1), ("m1-tone030", 1)),
    ("Remix defaults vs Gentle", ("m1-gentle", 1), ("m1-remix", 1)),
    ("Remix defaults vs Local tone 0.30 alone", ("m1-tone030", 1), ("m1-remix", 1)),
    ("2 passes vs 1", ("m1-p1", 1), ("m1-p2", 1)),
    ("3 passes vs 1", ("m1-p1", 1), ("m1-p3", 1)),
    ("4 passes vs 1", ("m1-p1", 1), ("m1-p4", 1)),
    ("4 passes rendered again", ("m1-p4", 1), ("m1-p4", 2)),
    ("RenoDX 8.5 vs 6.5.3 (gov 1, as written)", ("m1-p1", 1), ("m1-r85-p1", 1)),
    ("RenoDX 8.5 rendered again (gov 1)", ("m1-r85-p1", 1), ("m1-r85-p1", 2)),
    ("RenoDX 8.5 vs 6.5.3 (gov 0)", ("m1-g0-p1", 1), ("m1-r85-g0-p1", 1)),
    ("RenoDX 8.5 rendered again (gov 0)", ("m1-r85-g0-p1", 1), ("m1-r85-g0-p1", 2)),
    ("RenoDX 8.5, Detail stability off, vs 6.5.3", ("m1-p1", 1), ("m1-r85-ds1", 1)),
    ("RenoDX 8.5, Detail stability off, rendered again", ("m1-r85-ds1", 1), ("m1-r85-ds1", 2)),
    ("RenoDX 8.5 Detail stability Auto vs Off", ("m1-r85-ds1", 1), ("m1-r85-p1", 1)),
    ("RenoDX 8.5 Remix vs 8.5 Standard", ("m1-r85-p1", 1), ("m1-r85-remix", 1)),
    ("RenoDX 8.5 Remix vs 6.5.3 Remix", ("m1-remix", 1), ("m1-r85-remix", 1)),
]


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
            d = distances(args.corpus / info["file"], a / "output.mkv", b / "output.mkv", info["width"],
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
    """Pace and memory per run: the worker's own numbers and NVML device memory over an idle baseline."""
    manifest = {c["name"]: c for c in load_manifest(args.corpus)["clips"]}
    rows = []
    for d in sorted(RUNS.glob("*__m1-*__*")):
        clip, name, rep = d.name.split("__")
        if clip not in manifest or not (d / "result.json").exists():
            continue
        if args.profiles and name not in args.profiles:
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
                         processing_fps=r.get("processing_fps"), ms_per_frame=1000 / r["processing_fps"]
                         if r.get("processing_fps") else None, wall_seconds=r.get("wall_seconds"),
                         first_frame_seconds=r.get("first_frame_seconds"),
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
    parser.add_argument("command", choices=["render", "drift", "looks", "timing"])
    parser.add_argument("--corpus", type=Path, default=CORPUS)
    parser.add_argument("--clips", nargs="*")
    parser.add_argument("--profiles", nargs="*")
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--every", type=int, default=2, help="frame sampling for the drift means")
    parser.add_argument("--every-rgb", type=int, default=4, help="frame sampling for the distances")
    parser.add_argument("--out", type=Path, default=ANALYSIS / "m1")
    args = parser.parse_args()
    return {"render": render, "drift": drift, "looks": looks, "timing": timing}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
