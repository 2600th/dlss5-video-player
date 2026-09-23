"""Which neural settings change the image: one control at a time from the shipped state.

Renders every row of ``KNOBS`` through ``run.py``'s own worker driver and compares
each output to the reference, decoded to rgb24, byte for byte. The reference writes
exactly the ten keys the player writes (``NeuralAddonOverridesFor`` over a default
``NeuralSettings``, src/NeuralSettings.cpp) plus ``NRNormGovernor=0`` - see
REFERENCE_EXTRA for why; every other row differs from it in one key or one guide, and
the reference is rendered twice so a zero is a measured zero rather than an assumed
one.

Beside the byte counts, every row reports where its change landed: the share of its
absolute difference that falls inside face boxes found on the source (OpenCV's Haar
frontal-face cascade, sampled every ``--face-every`` frames) against the share of the
area those boxes cover. A control that works on skin concentrates there; a global
one does not.

    python tools/benchmark/knobs.py --clips real-film-cuts real-game-cuts          # render + table
    python tools/benchmark/knobs.py --clips real-film-cuts --rows skin-plus1 automask-off
    python tools/benchmark/knobs.py --list
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import cv2
import numpy as np

import run
from common import ANALYSIS, CORPUS, FrameReader, RUNS, framemd5, load_manifest, sequence_digest, write_json

# What the player writes on every render, in its order (NeuralAddonOverridesFor).
SHIPPED = {
    "NRIntensity": "1.000000", "NRLocalTone": "1.000000", "NRLocalStructure": "1.000000",
    "NRSkinStructure": "-1.000000", "NRColorStrength": "1.000000", "NRPreset": "0", "NRStyle": "0",
    "NRAutoMask": "1", "NRPasses": "1", "NRChainedHistory": "1",
}

# The reference is the shipped state with the add-on's normalization governor off.
# Its default governor (2, "Stable") settles at rates per SECOND, and a repeat of one
# configuration then differs from itself on the pinned runtime - 21 of 90 frames of
# depth-pan, from frame 69 on - so a byte count against it would measure the clock as
# well as the control. Governor 0 renders reproducibly; the `governor-*` rows put a
# number on what the default does, and `governor-2-repeat` on its own noise.
REFERENCE_EXTRA = {"NRNormGovernor": "0"}

# (row, control, change, overrides on top of the reference, guides). Controls the
# player does not write are here too: NRGlobalTone ("Global Tone Intensity" in the
# add-on's overlay, default 1), NRUICorrection ("NR UI Correction", default 0) and
# NRNormGovernor ("Normalization Governor": 0 off, 1 slew, 2 stable, default 2).
KNOBS = [
    ("shipped-repeat", "(reference, rendered again)", "none", {}, "mv=1,depth=1"),
    ("governor-2", "Normalization governor (hidden)", "0 -> 2 (the add-on default the player ships)",
     {"NRNormGovernor": "2"}, "mv=1,depth=1"),
    ("governor-2-repeat", "Normalization governor (hidden)", "2, rendered again (its own noise)",
     {"NRNormGovernor": "2"}, "mv=1,depth=1"),
    ("governor-1", "Normalization governor (hidden)", "0 -> 1 (slew)", {"NRNormGovernor": "1"}, "mv=1,depth=1"),
    ("intensity-040", "Intensity", "1.00 -> 0.40", {"NRIntensity": "0.400000"}, "mv=1,depth=1"),
    ("tone-030", "Local tone", "1.00 -> 0.30", {"NRLocalTone": "0.300000"}, "mv=1,depth=1"),
    ("structure-030", "Local structure", "1.00 -> 0.30", {"NRLocalStructure": "0.300000"}, "mv=1,depth=1"),
    ("globaltone-030", "Global tone (hidden)", "1.00 -> 0.30", {"NRGlobalTone": "0.300000"}, "mv=1,depth=1"),
    ("globaltone-105", "Global tone (hidden)", "1.00 -> 1.05", {"NRGlobalTone": "1.050000"}, "mv=1,depth=1"),
    ("globaltone-0", "Global tone (hidden)", "1.00 -> 0.00", {"NRGlobalTone": "0.000000"}, "mv=1,depth=1"),
    ("globaltone-2", "Global tone (hidden)", "1.00 -> 2.00", {"NRGlobalTone": "2.000000"}, "mv=1,depth=1"),
    ("style-1", "Style", "0 -> 1 (Natural)", {"NRStyle": "1"}, "mv=1,depth=1"),
    ("style-2", "Style", "0 -> 2 (Cinematic)", {"NRStyle": "2"}, "mv=1,depth=1"),
    ("mv-off", "Motion-vector guide", "on -> off", {}, "mv=0,depth=1"),
    ("depth-off", "Depth guide", "on -> off (motion on)", {}, "mv=1,depth=0"),
    ("skin-0", "Skin structure", "-1.00 -> 0.00", {"NRSkinStructure": "0.000000"}, "mv=1,depth=1"),
    ("skin-plus1", "Skin structure", "-1.00 -> +1.00", {"NRSkinStructure": "1.000000"}, "mv=1,depth=1"),
    ("skin-m050", "Skin structure", "-1.00 -> -0.50", {"NRSkinStructure": "-0.500000"}, "mv=1,depth=1"),
    ("skin-m001", "Skin structure", "-1.00 -> -0.01", {"NRSkinStructure": "-0.010000"}, "mv=1,depth=1"),
    ("skin-p025", "Skin structure", "-1.00 -> +0.25", {"NRSkinStructure": "0.250000"}, "mv=1,depth=1"),
    ("skin-p050", "Skin structure", "-1.00 -> +0.50", {"NRSkinStructure": "0.500000"}, "mv=1,depth=1"),
    ("skin-p099", "Skin structure", "-1.00 -> +0.99", {"NRSkinStructure": "0.990000"}, "mv=1,depth=1"),
    ("automask-off", "Automatic mask", "on -> off", {"NRAutoMask": "0"}, "mv=1,depth=1"),
    ("skin-0-nomask", "Skin structure, mask off", "-1.00 -> 0.00 with NRAutoMask=0",
     {"NRSkinStructure": "0.000000", "NRAutoMask": "0"}, "mv=1,depth=1"),
    ("skin-plus1-nomask", "Skin structure, mask off", "-1.00 -> +1.00 with NRAutoMask=0",
     {"NRSkinStructure": "1.000000", "NRAutoMask": "0"}, "mv=1,depth=1"),
    ("uicorrection-1", "UI correction (hidden)", "0 -> 1", {"NRUICorrection": "1"}, "mv=1,depth=1"),
    ("uicorrection-05", "UI correction (hidden)", "0 -> 0.5", {"NRUICorrection": "0.500000"}, "mv=1,depth=1"),
    ("colorstrength-020", "Color strength", "1.00 -> 0.20", {"NRColorStrength": "0.200000"},
     "mv=1,depth=1"),
    ("colorstrength-060", "Color strength", "1.00 -> 0.60", {"NRColorStrength": "0.600000"},
     "mv=1,depth=1"),
    ("preset-1", "Render preset (hidden)", "0 -> 1", {"NRPreset": "1"}, "mv=1,depth=1"),
    ("preset-3", "Render preset (hidden)", "0 -> 3", {"NRPreset": "3"}, "mv=1,depth=1"),
    ("passes-2", "Passes", "1 -> 2", {"NRPasses": "2"}, "mv=1,depth=1"),
    ("passes-2-unchained", "Chained history", "on -> off (2 passes)",
     {"NRPasses": "2", "NRChainedHistory": "0"}, "mv=1,depth=1"),
]
REFERENCE = "knob-shipped"


# Rows that are a second render of another row's profile, scored against its first.
REPEATS = {"shipped-repeat": REFERENCE, "governor-2-repeat": "knob-governor-2"}


def profile_of(row: str) -> str:
    return REPEATS.get(row, f"knob-{row}")


def profiles() -> dict:
    base = {**SHIPPED, **REFERENCE_EXTRA}
    out = {REFERENCE: run.profile("mv=1,depth=1", base,
                                  description="the ten player keys, normalization governor off")}
    for row, control, change, overrides, guides in KNOBS:
        if row in REPEATS:
            continue
        out[f"knob-{row}"] = run.profile(guides, {**base, **overrides},
                                         description=f"{control}: {change} (reference otherwise)")
    return out


def run_dir(clip: str, profile: str, rep: int = 1) -> Path:
    return RUNS / f"{clip}__{profile}__{rep}"


FACE_CASCADE = None


def faces(frame: np.ndarray) -> list[tuple[int, int, int, int]]:
    global FACE_CASCADE
    if FACE_CASCADE is None:
        FACE_CASCADE = cv2.CascadeClassifier(cv2.data.haarcascades + "haarcascade_frontalface_default.xml")
    gray = cv2.cvtColor(frame, cv2.COLOR_RGB2GRAY)
    return [tuple(map(int, box)) for box in
            FACE_CASCADE.detectMultiScale(gray, scaleFactor=1.1, minNeighbors=6, minSize=(48, 48))]


def face_masks(source: Path, width: int, height: int, every: int) -> dict[int, np.ndarray]:
    """Per sampled frame index, a boolean map of the Haar face boxes on the source."""
    masks = {}
    for index, frame in enumerate(FrameReader(source, width, height)):
        if index % every:
            continue
        mask = np.zeros((height, width), dtype=bool)
        for x, y, w, h in faces(frame):
            mask[y:y + h, x:x + w] = True
        masks[index] = mask
    return masks


def compare(reference: Path, candidate: Path, width: int, height: int, masks: dict[int, np.ndarray]) -> dict:
    differing = total = 0
    absolute = 0.0
    peak = 0
    frames_differing = frames = 0
    face_delta = face_area = sampled_delta = sampled_area = 0.0
    for index, (a, b) in enumerate(zip(FrameReader(reference, width, height), FrameReader(candidate, width, height))):
        delta = np.abs(a.astype(np.int16) - b.astype(np.int16))
        changed = int(np.count_nonzero(delta))
        differing += changed
        total += delta.size
        absolute += float(delta.sum())
        peak = max(peak, int(delta.max()))
        frames += 1
        frames_differing += changed > 0
        mask = masks.get(index)
        if mask is not None and mask.any():
            per_pixel = delta.sum(axis=2)
            face_delta += float(per_pixel[mask].sum())
            sampled_delta += float(per_pixel.sum())
            face_area += float(mask.sum())
            sampled_area += float(mask.size)
    result = dict(frames=frames, frames_differing=frames_differing,
                  bytes_differing_percent=100.0 * differing / total if total else None,
                  mean_abs_delta=absolute / total if total else None, max_abs_delta=peak)
    if sampled_area and sampled_delta:
        area_share = face_area / sampled_area
        delta_share = face_delta / sampled_delta
        result.update(face_area_share=area_share, face_delta_share=delta_share,
                      face_enrichment=delta_share / area_share if area_share else None)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", type=Path, default=CORPUS)
    parser.add_argument("--clips", nargs="+", default=["real-film-cuts", "real-game-cuts"])
    parser.add_argument("--rows", nargs="*", help="KNOBS rows to run (default: all)")
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--face-every", type=int, default=5)
    parser.add_argument("--compare-only", action="store_true", help="score existing runs, render nothing")
    parser.add_argument("--render-only", action="store_true", help="render, score nothing (frees the GPU sooner)")
    parser.add_argument("--out", type=Path, default=ANALYSIS / "knobs")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()
    specs = profiles()
    rows = [k for k in KNOBS if not args.rows or k[0] in args.rows]
    if args.list:
        for row, control, change, overrides, guides in KNOBS:
            print(f"{row:20s} {control}: {change}  guides={guides} overrides={overrides}")
        return 0
    manifest = {c["name"]: c for c in load_manifest(args.corpus)["clips"]}
    unknown = [c for c in args.clips if c not in manifest]
    if unknown:
        parser.error(f"unknown clips {unknown}")
    RUNS.mkdir(parents=True, exist_ok=True)
    if not args.compare_only:
        for clip in args.clips:
            run.run_one(manifest[clip], REFERENCE, specs[REFERENCE], 1, args.timeout, args.corpus, False)
            for row in rows:
                name = profile_of(row[0])
                run.run_one(manifest[clip], name, specs[name], 2 if row[0] in REPEATS else 1, args.timeout,
                            args.corpus, False)
    if args.render_only:
        return 0
    table = []
    for clip in args.clips:
        info = manifest[clip]
        masks = face_masks(args.corpus / info["file"], info["width"], info["height"], args.face_every)
        faced = sum(1 for m in masks.values() if m.any())
        print(f"{clip}: faces found on {faced} of {len(masks)} sampled source frames", flush=True)
        for row, control, change, overrides, guides in rows:
            against = run_dir(clip, profile_of(row)) if row in REPEATS else run_dir(clip, REFERENCE)
            reference = against / "output.mkv"
            ref_digest = sequence_digest(framemd5(reference, against / "frames.md5"))
            directory = run_dir(clip, profile_of(row), 2 if row in REPEATS else 1)
            result = json.loads((directory / "result.json").read_text(encoding="utf-8"))
            if not result["result"].get("ok"):
                table.append(dict(clip=clip, row=row, control=control, change=change, failed=True,
                                  detail=result["result"].get("detail", "")))
                continue
            digest = sequence_digest(framemd5(directory / "output.mkv", directory / "frames.md5"))
            scored = compare(reference, directory / "output.mkv", info["width"], info["height"], masks)
            table.append(dict(clip=clip, row=row, control=control, change=change, guides=guides,
                              overrides=overrides, digest=digest, identical=digest == ref_digest,
                              faces_sampled=faced, **scored))
            print(json.dumps(table[-1]), flush=True)
    write_json(args.out / "knobs.json", dict(reference=REFERENCE, shipped=SHIPPED, rows=table))
    lines = ["| clip | control | change | bytes differing | mean abs delta | max | face delta / area share |",
             "|---|---|---|---:|---:|---:|---:|"]
    for r in table:
        if r.get("failed"):
            lines.append(f"| {r['clip']} | {r['control']} | {r['change']} | failed | | | |")
            continue
        enrich = (f"{r['face_delta_share'] * 100:.1f} % / {r['face_area_share'] * 100:.1f} %"
                  if r.get("face_area_share") else "-")
        lines.append(f"| {r['clip']} | {r['control']} | {r['change']} | {r['bytes_differing_percent']:.2f} % | "
                     f"{r['mean_abs_delta']:.3f} | {r['max_abs_delta']} | {enrich} |")
    (args.out / "knobs.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
