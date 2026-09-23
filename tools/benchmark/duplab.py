"""Measures the duplicate-frame test in ``src/SceneCut.h`` against animation on twos.

Frame generation holds instead of interpolating when a decoded pair is the same picture
twice (``IsDuplicateDecodedPair``). This asks whether that test separates the
populations it has to separate, before any GPU is involved:

  * repeats - each corpus clip is re-timed onto twos (every second source frame shown
    twice at the clip's own rate) and re-encoded with libx264 at a streaming CRF, so
    every second pair is a repeat whose decode differs from its original by codec
    noise only;
  * real pairs - the other half of the same file, which is ordinary motion that frame
    generation must keep interpolating; and
  * small objects - the clip's first frame held still with one square of 16, 32 or
    64 px moving 8 px a frame across it, through the same encode. A mean cannot see
    such a pair; it is what the changed-sample test exists for, and no corpus clip
    has one.

The evidence is computed exactly as ``MeasureDecodedPair`` does it: BGRA Rec.709 luma
on one regular stride that keeps at most 320x180 samples. The shipped thresholds are
copied below, so a change there must be made in both places; ``--grid`` also prints
how many pairs of each population a neighbourhood of other thresholds would hold.

    python tools/benchmark/duplab.py [--corpus DIR] [--clips NAME ...] [--crf N] [--grid] [--json OUT]
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

import common
from common import CORPUS, FLAGS, FrameReader, load_manifest, write_json

# src/SceneCut.h
MAX_PAIR_SAMPLES = 320 * 180
DUPLICATE_RESIDUAL = 0.5 / 255.0
DUPLICATE_SAMPLE_CHANGE = 32.0 / 255.0
DUPLICATE_CHANGED_FRACTION = 1.0e-4
REC709 = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32) / 255  # LumaFromBgra, on RGB

GRID_RESIDUAL = (0.25, 0.5, 1.0)                  # 8-bit codes
GRID_SAMPLE_CHANGE = (8, 16, 24, 32)              # 8-bit codes
GRID_CHANGED_FRACTION = (1e-4, 5e-4, 1e-3, 2e-3, 4e-3)
OBJECT_SIZES, OBJECT_STEP, OBJECT_FRAMES = (16, 32, 64), 8, 24


def sample_luma(rgb: np.ndarray) -> np.ndarray:
    """MeasureDecodedPair's sample grid: one stride for both axes, (w/step+1)*(h/step+1) <= cap."""
    height, width = rgb.shape[:2]
    step = 1
    while (width // step + 1) * (height // step + 1) > MAX_PAIR_SAMPLES:
        step += 1
    return rgb[::step, ::step].astype(np.float32) @ REC709


def evidence(previous: np.ndarray, current: np.ndarray) -> dict:
    """The mean change and, per candidate threshold, the fraction of samples that moved more."""
    difference = np.abs(previous.astype(np.float64) - current.astype(np.float64))
    return dict(residual=float(difference.mean()),
                changed={code: float((difference > code / 255.0).mean()) for code in GRID_SAMPLE_CHANGE})


def held(e: dict, residual: float = DUPLICATE_RESIDUAL, code: int = round(DUPLICATE_SAMPLE_CHANGE * 255),
         fraction: float = DUPLICATE_CHANGED_FRACTION) -> bool:
    return e["residual"] < residual and e["changed"][code] <= fraction


def encode(frames, clip: dict, crf: int, out: Path) -> None:
    width, height = clip["width"], clip["height"]
    encoder = subprocess.Popen([str(common.FFMPEG), "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
                                "-s", f"{width}x{height}", "-r", repr(clip["fps"]), "-i", "-", "-c:v", "libx264",
                                "-preset", "medium", "-crf", str(crf), "-pix_fmt", "yuv420p", "-an", str(out)],
                               stdin=subprocess.PIPE, creationflags=FLAGS)
    for rgb in frames:
        encoder.stdin.write(rgb.tobytes())
    encoder.stdin.close()
    if encoder.wait() != 0:
        raise RuntimeError(f"libx264 encode of {out.name} failed")


def pairs(path: Path, clip: dict) -> list[dict]:
    rows, previous = [], None
    for rgb in FrameReader(path, clip["width"], clip["height"]):
        luma = sample_luma(rgb)
        if previous is not None:
            rows.append(evidence(previous, luma))
        previous = luma
    return rows


def on_twos(source: Path, clip: dict):
    """Every second source frame, yielded twice.

    Duplicated here rather than re-timed by an FFmpeg filter, so which output pairs are
    repeats is known by construction instead of depending on how a rate filter rounds:
    output frames 2k and 2k+1 are source frame 2k, so pair i (frame i+1 against frame i)
    is a repeat when i is even.
    """
    for index, rgb in enumerate(FrameReader(source, clip["width"], clip["height"])):
        if index % 2 == 0:
            yield rgb
            yield rgb


def small_object(source: Path, clip: dict, size: int):
    """The clip's first frame held still, with one flat square moving across it."""
    reader = FrameReader(source, clip["width"], clip["height"], count=1)
    still = next(reader)
    reader.close()
    y = (clip["height"] - size) // 2
    for index in range(OBJECT_FRAMES):
        frame = still.copy()
        x = 64 + index * OBJECT_STEP
        frame[y:y + size, x:x + size] = 128 if still[y:y + size, x:x + size].mean() < 96 else 32
        yield frame


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", type=Path, default=CORPUS)
    parser.add_argument("--clips", nargs="*")
    parser.add_argument("--crf", type=int, default=23, help="libx264 CRF of the re-encode (default 23)")
    parser.add_argument("--grid", action="store_true", help="also print the held counts around the shipped point")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    clips = [c for c in load_manifest(args.corpus)["clips"] if not args.clips or c["name"] in args.clips]
    population = dict(repeat=[], real=[], object=[])
    lines = ["| clip | repeats held | real pairs held | small-object pairs held | largest repeat mean | "
             "smallest real mean |", "|---|---:|---:|---:|---:|---:|"]
    with tempfile.TemporaryDirectory() as scratch:
        for clip in clips:
            source = args.corpus / clip["file"]
            twos = Path(scratch) / "twos.mkv"
            encode(on_twos(source, clip), clip, args.crf, twos)
            rows = pairs(twos, clip)
            repeats, real = rows[0::2], rows[1::2]
            objects = []
            for size in OBJECT_SIZES:
                moving = Path(scratch) / f"object-{size}.mkv"
                encode(small_object(source, clip, size), clip, args.crf, moving)
                objects += pairs(moving, clip)
            population["repeat"] += repeats
            population["real"] += real
            population["object"] += objects
            lines.append(f"| {clip['name']} | {sum(map(held, repeats))}/{len(repeats)} | "
                         f"{sum(map(held, real))}/{len(real)} | {sum(map(held, objects))}/{len(objects)} | "
                         f"{max(e['residual'] for e in repeats) * 255:.3f} | "
                         f"{min(e['residual'] for e in real) * 255:.3f} |")
    count = lambda name, **k: sum(held(e, **k) for e in population[name])
    lines += ["", f"Shipped point (mean < {DUPLICATE_RESIDUAL * 255:g} code, at most "
                  f"{DUPLICATE_CHANGED_FRACTION:g} of samples past {DUPLICATE_SAMPLE_CHANGE * 255:g} codes), "
                  f"libx264 CRF {args.crf}: repeats held {count('repeat')}/{len(population['repeat'])}, real pairs "
                  f"held {count('real')}/{len(population['real'])}, small-object pairs held "
                  f"{count('object')}/{len(population['object'])}."]
    grid = []
    if args.grid:
        lines += ["", "| mean < codes | changed past codes | changed fraction <= | repeats held | real held | "
                      "small-object held |", "|---:|---:|---:|---:|---:|---:|"]
        for residual in GRID_RESIDUAL:
            for code in GRID_SAMPLE_CHANGE:
                for fraction in GRID_CHANGED_FRACTION:
                    point = dict(residual=residual / 255.0, code=code, fraction=fraction)
                    counts = {name: count(name, **point) for name in population}
                    grid.append(dict(point, **counts))
                    lines.append(f"| {residual:g} | {code} | {fraction:g} | {counts['repeat']} | {counts['real']} | "
                                 f"{counts['object']} |")
    print("\n".join(lines))
    if args.json:
        write_json(args.json, dict(crf=args.crf, sizes={k: len(v) for k, v in population.items()}, grid=grid))
    return 0


if __name__ == "__main__":
    sys.exit(main())
