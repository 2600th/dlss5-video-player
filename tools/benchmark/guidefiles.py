"""Per-frame guide files for the worker's ``depth=file:`` / ``mv=file:`` guide modes (P2.4).

The layout is the one ``src/GuideFiles.h`` reads: one directory per guide, one
Portable Float Map per source frame named by its zero-padded frame number
(``000000.pfm``), depth as a one-channel ``Pf`` map in [0,1] with 0 = near,
motion as a three-channel ``PF`` map of (x, y, 0) in DLSS input pixels pointing
from the current frame to where the content was in the previous one. PFM rows are
stored bottom-up; ``read_pfm``/``write_pfm`` take and return top-down arrays.
Any size is accepted by the worker, which area-averages onto its analysis grid;
files written here are already at that grid (160x90 for a 30-fps 1080p clip).

    python tools/benchmark/guidefiles.py truth                       # true guides of the depth-* clips
    python tools/benchmark/guidefiles.py convert --clip depth-pan --depth-dir vda/ --inverse
    python tools/benchmark/guidefiles.py compare A/depth B/depth     # max |difference| per frame
    python tools/benchmark/guidefiles.py prove                       # renders the round-trip proof

``prove`` renders ``roundtrip.profile.json``'s arms in order through run.py and
checks the two equalities the harness rests on: the CPU estimator's dumped guides
fed back through files give byte-identical output, and so does the shipped
estimator's dumped depth with its hardware motion left alone.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np

from common import CORPUS, RUNS, WORK, framemd5, load_manifest, sequence_digest
from cutmirror import analysis_grid

HERE = Path(__file__).resolve().parent


def read_pfm(path: Path) -> np.ndarray:
    """Top-down float32 array, (h, w) for Pf and (h, w, 3) for PF."""
    data = path.read_bytes()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end].decode("ascii"))
        pos = end
    pos += 1
    magic, width, height, scale = parts[0], int(parts[1]), int(parts[2]), float(parts[3])
    channels = {"Pf": 1, "PF": 3}[magic]
    values = np.frombuffer(data, dtype="<f4" if scale < 0 else ">f4", offset=pos, count=width * height * channels)
    shape = (height, width) if channels == 1 else (height, width, 3)
    return np.flipud(values.reshape(shape)).astype(np.float32)


def write_pfm(path: Path, array: np.ndarray) -> None:
    array = np.asarray(array, dtype="<f4")
    if array.ndim == 2:
        magic = "Pf"
    elif array.ndim == 3 and array.shape[2] == 3:
        magic = "PF"
    else:
        raise ValueError(f"PFM holds (h, w) or (h, w, 3), not {array.shape}")
    path.parent.mkdir(parents=True, exist_ok=True)
    header = f"{magic}\n{array.shape[1]} {array.shape[0]}\n-1.0\n".encode("ascii")
    path.write_bytes(header + np.ascontiguousarray(np.flipud(array)).tobytes())


def frame_path(directory: Path, frame: int) -> Path:
    return directory / f"{frame:06d}.pfm"


def area_average(array: np.ndarray, gw: int, gh: int) -> np.ndarray:
    """The worker's own ResampleArea: each grid cell is the mean of the source rectangle it covers."""
    h, w = array.shape[:2]
    if (w, h) == (gw, gh):
        return array.astype(np.float32)
    out = np.empty((gh, gw) + array.shape[2:], dtype=np.float64)
    for gy in range(gh):
        y0 = gy * h // gh
        y1 = max(y0 + 1, (gy + 1) * h // gh)
        for gx in range(gw):
            x0 = gx * w // gw
            x1 = max(x0 + 1, (gx + 1) * w // gw)
            out[gy, gx] = array[y0:y1, x0:x1].mean(axis=(0, 1))
    return out.astype(np.float32)


def write_frame(root: Path, frame: int, depth: np.ndarray | None, motion: np.ndarray | None) -> None:
    if depth is not None:
        write_pfm(frame_path(root / "depth", frame), depth)
    if motion is not None:
        write_pfm(frame_path(root / "mv", frame), np.dstack([motion[..., 0], motion[..., 1],
                                                              np.zeros(motion.shape[:2], np.float32)]))


# ---------------------------------------------------------------------------
# True guides of the synthetic near/far clips, from corpus.py's own constants.

def truth_fields(clip: str, frame: int, width: int, height: int) -> tuple[np.ndarray, np.ndarray]:
    """Full-resolution (depth, motion) of `clip` at `frame`. Motion is the renderer's
    convention: current -> previous, so content that moved left by v px carries +v."""
    import corpus
    y, x = np.mgrid[0:height, 0:width].astype(np.float64)
    motion = np.zeros((height, width, 2), np.float32)
    if clip == "depth-pan":
        depth, speed = corpus.depth_pan_layers(frame, x, y)
        if frame:
            motion[..., 0] = speed
        return depth.astype(np.float32), motion
    if clip == "depth-subject":
        p = corpus.DEPTH_SUBJECT
        inside = corpus.depth_subject_mask(frame, x, y)
        depth = np.where(inside, p["subject"]["depth"], p["room"]["depth"]).astype(np.float32)
        if frame:
            (x1, y1), (x0, y0) = corpus.depth_subject_position(frame), corpus.depth_subject_position(frame - 1)
            motion[..., 0] = np.where(inside, -(x1 - x0), p["room"]["speed"])
            motion[..., 1] = np.where(inside, -(y1 - y0), 0.0)
        return depth, motion
    raise ValueError(f"{clip} has no analytic truth")


def cmd_truth(args) -> int:
    manifest = load_manifest(args.corpus)
    clips = [c for c in manifest["clips"] if c.get("depth_truth") and (not args.clips or c["name"] in args.clips)]
    if not clips:
        print("no depth-truth clip in the manifest; build depth-pan / depth-subject with corpus.py first")
        return 1
    for clip in clips:
        gw, gh = analysis_grid(clip["width"], clip["height"], clip["fps"])
        root = args.out / clip["name"]
        for frame in range(clip["frames"]):
            depth, motion = truth_fields(clip["name"], frame, clip["width"], clip["height"])
            write_frame(root, frame, area_average(depth, gw, gh), area_average(motion, gw, gh))
        print(f"{clip['name']}: {clip['frames']} frames of true depth and motion at {gw}x{gh} -> {root}")
    return 0


# ---------------------------------------------------------------------------
# Offline depth (Video Depth Anything or anything else) into the layout.

def load_map(path: Path) -> np.ndarray:
    if path.suffix.lower() == ".npy":
        return np.load(path).astype(np.float64)
    if path.suffix.lower() == ".pfm":
        return read_pfm(path).astype(np.float64)
    import cv2
    image = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    if image is None:
        raise ValueError(f"cannot read {path}")
    if image.ndim == 3:
        image = image[..., 0]
    return image.astype(np.float64)


def cmd_convert(args) -> int:
    manifest = {c["name"]: c for c in load_manifest(args.corpus)["clips"]}
    clip = manifest[args.clip]
    gw, gh = analysis_grid(clip["width"], clip["height"], clip["fps"])
    files = sorted(p for p in args.depth_dir.iterdir() if p.suffix.lower() in (".npy", ".png", ".pfm", ".tif",
                                                                               ".tiff"))
    if len(files) != clip["frames"]:
        print(f"{len(files)} depth maps for {clip['frames']} frames of {args.clip}; they must match one to one "
              f"in sorted name order")
        return 1
    maps = [area_average(load_map(p), gw, gh).astype(np.float64) for p in files]
    stack = np.stack(maps)
    # One scale for the whole clip, never per frame: a per-frame min/max makes the
    # depth of a static object pump whenever something nearer enters the shot.
    low, high = np.percentile(stack, [args.low_percentile, 100.0 - args.low_percentile])
    if high <= low:
        print("the clip's depth range is empty")
        return 1
    normalised = np.clip((stack - low) / (high - low), 0.0, 1.0)
    if args.inverse:
        # Relative inverse depth (disparity): large is near. The renderer wants 0 near.
        normalised = 1.0 - normalised
    root = args.out / args.clip
    for frame, depth in enumerate(normalised):
        write_frame(root, frame, depth.astype(np.float32), None)
    print(f"{args.clip}: {len(files)} frames -> {root / 'depth'} at {gw}x{gh}, clip range "
          f"[{low:.4g}, {high:.4g}] ({'inverse' if args.inverse else 'direct'} depth)")
    return 0


def cmd_compare(args) -> int:
    worst = 0.0
    names = sorted(p.name for p in args.a.glob("*.pfm"))
    missing = [n for n in names if not (args.b / n).exists()]
    for name in names:
        if name in missing:
            continue
        a, b = read_pfm(args.a / name), read_pfm(args.b / name)
        difference = float(np.max(np.abs(a - b))) if a.shape == b.shape else math.inf
        worst = max(worst, difference)
    print(f"{len(names)} frames, {len(missing)} missing in {args.b}, max |difference| {worst}")
    return 0 if worst == 0.0 and not missing else 1


# ---------------------------------------------------------------------------
# The round-trip proof.

def cmd_prove(args) -> int:
    import run
    manifest = {c["name"]: c for c in load_manifest(args.corpus)["clips"]}
    profiles = run.load_profiles(args.profile_file)
    order = list(profiles)
    for clip in args.clips:
        for name in order:
            run.run_one(manifest[clip], name, profiles[name], 1, args.timeout, args.corpus, False)
    pairs = [("rt-cpu-dump", "rt-cpu-replay"), ("rt-shipped-dump", "rt-shipped-depth-replay")]
    failures = 0
    for clip in args.clips:
        digests = {}
        for name in order:
            directory = RUNS / f"{clip}__{name}__1"
            result = json.loads((directory / "result.json").read_text(encoding="utf-8"))
            if not result["result"].get("ok"):
                print(f"{clip} {name}: render failed: {result['result'].get('detail', '')[:300]}")
                failures += 1
                continue
            digests[name] = sequence_digest(framemd5(directory / "output.mkv", directory / "frames.md5"))
        for built_in, replay in pairs:
            if built_in in digests and replay in digests:
                same = digests[built_in] == digests[replay]
                failures += not same
                print(f"{clip}: {built_in} {digests[built_in][:16]} vs {replay} {digests[replay][:16]} -> "
                      f"{'BYTE-IDENTICAL' if same else 'DIFFERENT'}")
        if "rt-cpu-dump" in digests and "rt-shipped-dump" in digests:
            print(f"{clip}: CPU estimator vs shipped estimator (hardware flow where present): "
                  f"{'identical' if digests['rt-cpu-dump'] == digests['rt-shipped-dump'] else 'different'}")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", type=Path, default=CORPUS)
    sub = parser.add_subparsers(dest="command", required=True)
    truth = sub.add_parser("truth", help="write the true depth/motion of the depth-* clips")
    truth.add_argument("--clips", nargs="*")
    truth.add_argument("--out", type=Path, default=WORK / "truth")
    convert = sub.add_parser("convert", help="normalise per-frame depth maps over the clip into the layout")
    convert.add_argument("--clip", required=True)
    convert.add_argument("--depth-dir", type=Path, required=True, help=".npy, 16-bit .png, .tif or .pfm per frame")
    convert.add_argument("--inverse", action="store_true", help="maps are inverse depth (large = near)")
    convert.add_argument("--low-percentile", type=float, default=1.0,
                         help="clip-wide range is [p, 100-p] percentiles, robust to a few outliers")
    convert.add_argument("--out", type=Path, default=WORK / "offline-depth")
    compare = sub.add_parser("compare", help="max |difference| between two directories of PFMs")
    compare.add_argument("a", type=Path)
    compare.add_argument("b", type=Path)
    prove = sub.add_parser("prove", help="render the round-trip proof and compare digests")
    prove.add_argument("--clips", nargs="+", default=["depth-pan", "depth-subject", "real-game-motion"])
    prove.add_argument("--profile-file", type=Path, default=HERE / "roundtrip.profile.json")
    prove.add_argument("--timeout", type=float, default=900.0)
    args = parser.parse_args()
    return {"truth": cmd_truth, "convert": cmd_convert, "compare": cmd_compare, "prove": cmd_prove}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
