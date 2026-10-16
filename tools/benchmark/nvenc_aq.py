"""NVENC adaptive quantisation and lookahead against the cache encode (P2.50).

Reproduces the cache encode's hevc_nvenc options (src/MediaPipeline.cpp
BuildEncoderArguments, which NvencDirectPolicy.h copies field by field):

    -preset p5 -tune hq -rc vbr -cq Q -b:v 0 -maxrate 800M -bufsize 800M -split_encode_mode auto
    Standard: -pix_fmt nv12, CQ 16          High: -profile:v main10 -pix_fmt p010le, CQ 14

and adds, one arm at a time, ``-spatial-aq 1`` and ``-spatial-aq 1 -temporal-aq 1
-rc-lookahead 20``. Every arm runs at four CQ values so the arms can be compared at
matched size (Bjontegaard delta over log rate, piecewise-cubic interpolation).

The encoder reads a pre-decoded raw file, so the encode fps excludes decoding; each
encode is scored against exactly the frames it was given (8-bit against the 8-bit
input, 10-bit against the 10-bit input) with libvmaf: vmaf_v0.6.1, vmaf_v0.6.1neg and
CAMBI on the encode.

    python tools/benchmark/nvenc_aq.py --inputs name=path.mkv ... --work DIR --out DIR
"""
from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from scipy.interpolate import PchipInterpolator

from common import FFMPEG, FFPROBE, FLAGS

BASE = ["-c:v", "hevc_nvenc", "-preset", "p5", "-tune", "hq", "-rc", "vbr", "-b:v", "0", "-maxrate", "800M",
        "-bufsize", "800M", "-split_encode_mode", "auto"]
ARMS = {
    "base": [],
    "saq": ["-spatial-aq", "1"],
    "saq-taq-la20": ["-spatial-aq", "1", "-temporal-aq", "1", "-rc-lookahead", "20"],
}
RUNGS = {
    "standard": dict(pix_fmt="nv12", extra=[], cqs=[16, 19, 22, 25], shipped=16),
    "high": dict(pix_fmt="p010le", extra=["-profile:v", "main10"], cqs=[14, 17, 20, 23], shipped=14),
}


def probe(path: Path) -> dict:
    data = json.loads(subprocess.check_output(
        [str(FFPROBE), "-v", "error", "-select_streams", "v:0", "-count_frames", "-show_entries",
         "stream=width,height,pix_fmt,avg_frame_rate,nb_read_frames", "-of", "json", str(path)],
        creationflags=FLAGS))["streams"][0]
    n, d = map(int, data["avg_frame_rate"].split("/"))
    return dict(width=data["width"], height=data["height"], pix_fmt=data["pix_fmt"], fps=n / d,
                frames=int(data["nb_read_frames"]))


def raw_input(src: Path, pix_fmt: str, dest: Path) -> Path:
    if not dest.exists():
        subprocess.check_call([str(FFMPEG), "-v", "error", "-y", "-i", str(src), "-map", "0:v:0", "-pix_fmt", pix_fmt,
                               "-f", "rawvideo", str(dest)], creationflags=FLAGS)
    return dest


def raw_args(info: dict, pix_fmt: str, path: Path, loops: int = 0) -> list[str]:
    args = ["-f", "rawvideo", "-pix_fmt", pix_fmt, "-s", f"{info['width']}x{info['height']}", "-r",
            f"{info['fps']}"]
    if loops:
        args += ["-stream_loop", str(loops)]
    return args + ["-i", str(path)]


def encode(raw: Path, info: dict, rung: str, arm: str, cq: int, out: Path, loops: int = 0) -> dict:
    r = RUNGS[rung]
    cmd = [str(FFMPEG), "-v", "error", "-y", *raw_args(info, r["pix_fmt"], raw, loops), "-map", "0:v:0", *BASE,
           "-cq", str(cq), *r["extra"], *ARMS[arm], "-pix_fmt", r["pix_fmt"], "-color_primaries", "bt709",
           "-color_trc", "bt709", "-colorspace", "bt709", "-color_range", "tv", str(out)]
    start = time.perf_counter()
    subprocess.check_call(cmd, creationflags=FLAGS)
    wall = time.perf_counter() - start
    return dict(wall_seconds=wall, bytes=out.stat().st_size)


def score(encoded: Path, raw: Path, info: dict, pix_fmt: str, log: Path) -> dict:
    fmt = "yuv420p10le" if pix_fmt == "p010le" else "yuv420p"
    graph = (f"[0:v]settb=1/30,setpts=N,format={fmt}[d];[1:v]settb=1/30,setpts=N,format={fmt}[r];"
             f"[d][r]libvmaf=model='version=vmaf_v0.6.1\\:name=vmaf|version=vmaf_v0.6.1neg\\:name=neg':"
             f"feature=name=cambi:n_threads=16:log_fmt=json:log_path={log.name}")
    subprocess.check_call([str(FFMPEG), "-v", "error", "-i", str(encoded), *raw_args(info, pix_fmt, raw),
                           "-lavfi", graph, "-f", "null", "-"], cwd=log.parent, creationflags=FLAGS)
    data = json.loads(log.read_text())
    pooled = data["pooled_metrics"]
    frames = data["frames"]
    vmaf = [f["metrics"]["vmaf"] for f in frames]
    return dict(vmaf=pooled["vmaf"]["mean"], vmaf_p5=float(np.percentile(vmaf, 5)), vmaf_min=min(vmaf),
                vmaf_neg=pooled["neg"]["mean"], cambi=pooled["cambi"]["mean"], scored_frames=len(frames))


def bd_rate(r1, q1, r2, q2) -> float | None:
    """Bjontegaard delta rate of arm 2 against arm 1, in percent (negative = smaller at the same quality)."""
    def curve(rates, quals):
        order = np.argsort(quals)
        q = np.asarray(quals, float)[order]
        lr = np.log10(np.asarray(rates, float))[order]
        if np.any(np.diff(q) <= 0):
            return None, None
        return q, PchipInterpolator(q, lr)
    qa, fa = curve(r1, q1)
    qb, fb = curve(r2, q2)
    if fa is None or fb is None:
        return None
    lo, hi = max(qa.min(), qb.min()), min(qa.max(), qb.max())
    if hi <= lo:
        return None
    grid = np.linspace(lo, hi, 200)
    avg = np.trapezoid(fb(grid) - fa(grid), grid) / (hi - lo)
    return float((10 ** avg - 1) * 100)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--inputs", nargs="+", required=True, help="name=path[:rungs], rungs comma-separated")
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--speed-loops", type=int, default=9, help="input repeats for the encode-fps runs")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    args.out.mkdir(parents=True, exist_ok=True)
    rows = []
    for item in args.inputs:
        name, rest = item.split("=", 1)
        path, _, rungs = rest.partition("|")
        src = Path(path)
        info = probe(src)
        for rung in (rungs.split(",") if rungs else list(RUNGS)):
            r = RUNGS[rung]
            raw = raw_input(src, r["pix_fmt"], args.work / f"{name}.{r['pix_fmt']}.yuv")
            for arm in ARMS:
                for cq in r["cqs"]:
                    out = args.work / f"{name}__{rung}__{arm}__cq{cq}.mkv"
                    enc = encode(raw, info, rung, arm, cq, out)
                    sc = score(out, raw, info, r["pix_fmt"], args.work / f"{out.stem}.vmaf.json")
                    kbps = enc["bytes"] * 8 / 1000 / (info["frames"] / info["fps"])
                    row = dict(input=name, rung=rung, arm=arm, cq=cq, frames=info["frames"], bytes=enc["bytes"],
                               kbps=kbps, kib_per_frame=enc["bytes"] / 1024 / info["frames"], **sc)
                    if cq == r["shipped"]:
                        speed_out = args.work / f"{name}__{rung}__{arm}__speed.mkv"
                        sp = encode(raw, info, rung, arm, cq, speed_out, loops=args.speed_loops)
                        row["encode_fps"] = info["frames"] * (args.speed_loops + 1) / sp["wall_seconds"]
                        speed_out.unlink()
                    rows.append(row)
                    print(json.dumps(row), flush=True)
                    out.unlink()
    fields = ["input", "rung", "arm", "cq", "frames", "bytes", "kbps", "kib_per_frame", "vmaf", "vmaf_p5",
              "vmaf_min", "vmaf_neg", "cambi", "scored_frames", "encode_fps"]
    with (args.out / "nvenc_aq_points.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for row in rows:
            w.writerow({k: (round(v, 4) if isinstance(v, float) else v) for k, v in row.items()})
    # BD-rate per input and rung, each arm against base, on VMAF and on VMAF NEG.
    bd = []
    for name in sorted({r["input"] for r in rows}):
        for rung in RUNGS:
            base = [r for r in rows if r["input"] == name and r["rung"] == rung and r["arm"] == "base"]
            if not base:
                continue
            for arm in ARMS:
                if arm == "base":
                    continue
                cand = [r for r in rows if r["input"] == name and r["rung"] == rung and r["arm"] == arm]
                entry = dict(input=name, rung=rung, arm=arm)
                for metric in ("vmaf", "vmaf_neg"):
                    entry[f"bd_rate_{metric}"] = bd_rate([r["kbps"] for r in base], [r[metric] for r in base],
                                                         [r["kbps"] for r in cand], [r[metric] for r in cand])
                shipped = RUNGS[rung]["shipped"]
                b0 = next(r for r in base if r["cq"] == shipped)
                c0 = next(r for r in cand if r["cq"] == shipped)
                entry.update(size_ratio_at_shipped_cq=c0["bytes"] / b0["bytes"],
                             vmaf_delta_at_shipped_cq=c0["vmaf"] - b0["vmaf"],
                             cambi_delta_at_shipped_cq=c0["cambi"] - b0["cambi"],
                             encode_fps_base=b0.get("encode_fps"), encode_fps_arm=c0.get("encode_fps"))
                bd.append(entry)
                print(json.dumps(entry), flush=True)
    with (args.out / "nvenc_aq_bdrate.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(bd[0]))
        w.writeheader()
        for row in bd:
            w.writerow({k: (round(v, 4) if isinstance(v, float) else v) for k, v in row.items()})
    return 0


if __name__ == "__main__":
    sys.exit(main())
