"""Builds randomized, sealed A/B pairs for blind one-pass versus two-pass judgement.

For every clip that has both a single-pass profile run (default ``baseline``)
and a two-pass run (default ``two-pass``), emits under benchmark-work/blind:

  pairs/<id>-A.png, <id>-B.png       matched still (same frame index) from each
  pairs/<id>-A.mp4, <id>-B.mp4       3 s H.264 excerpts starting at that frame
  pairs/<id>.txt                     what to judge (no identities)
  key.json                           sealed mapping id -> {A: run, B: run, frame}
  ballot.csv                         one row per pair for the judge to fill in
  score.py-compatible: python blind.py --score ballot.csv

Left/right (A/B) assignment is shuffled with the supplied seed; the key is
written once and never printed. Judges should look only at ``pairs/``.

    python tools/benchmark/blind.py --pairs-per-clip 3 --seed 1
    python tools/benchmark/blind.py --score build-upscaling/benchmark-work/blind/ballot.csv
"""
from __future__ import annotations

import argparse
import csv
import json
import random
import secrets
import subprocess
import sys
from pathlib import Path

from common import BLIND, FFMPEG, FLAGS, RUNS, load_manifest, run_dirs, write_json


def load_runs(single: str, double: str) -> dict[str, dict[str, Path]]:
    """clip -> {'single': output.mkv, 'double': output.mkv} using repeat 1 of each profile."""
    found: dict[str, dict[str, Path]] = {}
    for run in run_dirs(RUNS):
        result = json.loads((run / "result.json").read_text(encoding="utf-8"))
        if not result["result"].get("ok") or result["repeat"] != 1:
            continue
        role = "single" if result["profile"] == single else "double" if result["profile"] == double else None
        if role:
            found.setdefault(result["clip"], {})[role] = Path(result["output"])
    return {clip: v for clip, v in found.items() if "single" in v and "double" in v}


def still(source: Path, frame: int, dest: Path) -> None:
    subprocess.run([str(FFMPEG), "-v", "error", "-y", "-i", str(source), "-vf", f"select=eq(n\\,{frame})",
                    "-frames:v", "1", str(dest)], check=True, creationflags=FLAGS)


def excerpt(source: Path, start_seconds: float, seconds: float, dest: Path) -> None:
    subprocess.run([str(FFMPEG), "-v", "error", "-y", "-ss", f"{start_seconds:.3f}", "-i", str(source), "-t",
                    f"{seconds:.3f}", "-an", "-c:v", "libx264", "-preset", "slow", "-crf", "14", "-pix_fmt",
                    "yuv420p", "-movflags", "+faststart", str(dest)], check=True, creationflags=FLAGS)


def build(args) -> int:
    manifest = load_manifest()
    clips = {c["name"]: c for c in manifest["clips"]}
    runs = load_runs(args.single, args.double)
    if not runs:
        print(f"no clip has both '{args.single}' and '{args.double}' runs under {RUNS}", file=sys.stderr)
        return 1
    pairs_dir = BLIND / "pairs"
    pairs_dir.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)
    key, ballot = [], []
    ids = [secrets.token_hex(3) for _ in range(len(runs) * args.pairs_per_clip)]
    rng.shuffle(ids)
    cursor = 0
    for clip_name, members in sorted(runs.items()):
        clip = clips[clip_name]
        fps, frames = clip["fps"], clip["frames"]
        window = int(args.seconds * fps)
        candidates = [f for f in range(0, max(1, frames - window)) if
                      all(abs(f - c) > window for c in clip["cuts"]) or not clip["cuts"]]
        if not candidates:
            candidates = [0]
        for _ in range(args.pairs_per_clip):
            pair_id = ids[cursor]
            cursor += 1
            frame = rng.choice(candidates)
            order = ["single", "double"]
            rng.shuffle(order)
            for label, role in zip("AB", order):
                still(members[role], frame, pairs_dir / f"{pair_id}-{label}.png")
                excerpt(members[role], frame / fps, args.seconds, pairs_dir / f"{pair_id}-{label}.mp4")
            (pairs_dir / f"{pair_id}.txt").write_text(
                f"Pair {pair_id}: compare {pair_id}-A and {pair_id}-B (still and {args.seconds:.0f} s clip). "
                "Judge sharpness of fine detail/text, skin/hair naturalness, temporal stability (flicker, "
                "crawling), halos/over-smoothing and colour. Record the preferred side (A, B or tie) and a "
                "1-5 confidence in ballot.csv.\n", encoding="utf-8")
            key.append(dict(id=pair_id, clip=clip_name, frame=frame, A=order[0], B=order[1],
                            A_run=str(members[order[0]]), B_run=str(members[order[1]])))
            ballot.append(dict(id=pair_id, preferred="", confidence=""))
            print(f"pair {pair_id}: {clip_name} frame {frame}", flush=True)
    write_json(BLIND / "key.json", dict(seed=args.seed, single=args.single, double=args.double, pairs=key))
    with (BLIND / "ballot.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["id", "preferred", "confidence"])
        writer.writeheader()
        writer.writerows(ballot)
    print(f"{len(key)} pairs in {pairs_dir}; sealed key at {BLIND / 'key.json'}; ballot at {BLIND / 'ballot.csv'}")
    return 0


def score(ballot_path: Path) -> int:
    key = {p["id"]: p for p in json.loads((BLIND / "key.json").read_text(encoding="utf-8"))["pairs"]}
    votes = {"single": 0, "double": 0, "tie": 0}
    weighted = {"single": 0.0, "double": 0.0}
    per_clip: dict[str, dict[str, int]] = {}
    with ballot_path.open(encoding="utf-8") as f:
        for row in csv.DictReader(f):
            pair = key.get(row["id"])
            side = row["preferred"].strip().upper()
            if not pair or side not in ("A", "B", "TIE"):
                continue
            role = "tie" if side == "TIE" else pair[side]
            votes[role] += 1
            if role != "tie":
                weighted[role] += float(row["confidence"] or 1)
            per_clip.setdefault(pair["clip"], {"single": 0, "double": 0, "tie": 0})[role] += 1
    print(json.dumps(dict(votes=votes, confidence_weighted=weighted, per_clip=per_clip), indent=2))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--single", default="baseline", help="single-pass profile name")
    parser.add_argument("--double", default="two-pass", help="two-pass profile name")
    parser.add_argument("--pairs-per-clip", type=int, default=3)
    parser.add_argument("--seconds", type=float, default=3.0)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--score", type=Path, help="score a filled ballot.csv against key.json")
    args = parser.parse_args()
    return score(args.score) if args.score else build(args)


if __name__ == "__main__":
    sys.exit(main())
