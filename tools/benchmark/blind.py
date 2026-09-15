"""Builds randomized, sealed A/B pairs for blind one-pass versus two-pass judgement.

For every clip that has both a single-pass profile run (default ``baseline``)
and a two-pass run (default ``two-pass``), emits under benchmark-work/blind:

  pairs/<id>-A.png, <id>-B.png       matched still (same frame index) from each
  pairs/<id>-A.mp4, <id>-B.mp4       H.264 excerpts from that frame, clipped to
                                     the shot the frame sits in (``--seconds``
                                     is the cap, not the length)
  pairs/<id>.txt                     what to judge (no identities)
  key.json                           sealed mapping id -> {A: run, B: run, frame, shot}
  ballot.csv                         one row per pair for the judge to fill in
  score.py-compatible: python blind.py --score ballot.csv

Left/right (A/B) assignment is shuffled with the supplied seed; the key is
written once and never printed. Judges should look only at ``pairs/``.

Candidate stills come from the manifest's own cut labels rather than from a
heuristic: ``cuts`` bound the shots, an inclusive ``soft_cuts`` span belongs to
neither of the shots it joins and is removed, a still must sit at least
``GUARD_SECONDS`` past the cut that opened its shot, and its excerpt is clipped
to that shot so it never spans an edit. This replaces a fallback to frame 0,
which fired whenever a clip's cuts left no frame a full excerpt clear of one -
that is, on every cut-bearing clip and on any clip shorter than the excerpt -
and frame 0 is the least useful still in a clip to judge: the guide generator
resets its history on it, so neither arm has any temporal history there and a
pair built on it cannot show the temporal behaviour the ballot asks about,
while the excerpt starting there runs straight through every cut that follows.
A clip that yields no candidate is now named and skipped with its reason
instead of degrading to frame 0, and a ballot with no pairs left in it exits
non-zero. Frames per clip still come only from ``--seed``, so one seed still
reproduces one ballot.

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


# A still is worth judging only where the manifest can prove which shot it
# belongs to. The frames straight after a hard cut are where the guide
# generator resets history, so both arms render them from nothing and the pair
# shows the reset instead of the setting under test; GUARD_SECONDS keeps the
# still clear of that. MIN_EXCERPT_SECONDS is what has to be left of the shot
# afterwards for the excerpt to say anything about temporal stability, and it
# is set to keep the corpus's 20-frame shots usable - real-film-cuts' opening
# shot offers three candidates at 30 fps - while anything shorter is reported
# as unusable instead of being judged on a handful of frames.
GUARD_SECONDS = 0.1
MIN_EXCERPT_SECONDS = 0.5


def shots(clip: dict) -> list[tuple[int, int]]:
    """Half-open frame spans the manifest labels as one continuous shot.

    ``cuts`` are the frames a hard cut lands on, so each one opens a shot.
    ``soft_cuts`` are inclusive transition spans - a dissolve has no single cut
    frame - whose frames belong to neither neighbour, so the span is cut out
    rather than split at. Ground truth only; nothing here is detected.
    """
    frames = clip["frames"]
    edges = sorted({0, frames, *(c for c in clip["cuts"] if 0 < c < frames)})
    fades = sorted((max(0, first), min(frames - 1, last)) for first, last in clip.get("soft_cuts") or [])
    spans: list[tuple[int, int]] = []
    for first, last in zip(edges, edges[1:]):
        cursor = first
        for fade_first, fade_last in fades:
            if fade_last < cursor or fade_first >= last:
                continue
            if fade_first > cursor:
                spans.append((cursor, fade_first))
            cursor = fade_last + 1
        if cursor < last:
            spans.append((cursor, last))
    return spans


def candidate_stills(clip: dict, seconds: float) -> tuple[list[tuple[int, int, range]], list[str]]:
    """``(first, last, candidate frames)`` per usable shot, longest shot first, and why the rest were dropped.

    Longest first so the earliest pairs of a clip get the longest excerpts. A
    shot too short to hold the guard plus a minimum excerpt is not usable, and
    says so rather than contributing a frame nobody can judge. A ``seconds``
    below MIN_EXCERPT_SECONDS becomes the minimum itself: asking for shorter
    excerpts must not reject the shots that can serve them.
    """
    fps = clip["fps"]
    guard = max(1, round(GUARD_SECONDS * fps))
    shortest = max(2, round(min(seconds, MIN_EXCERPT_SECONDS) * fps))
    pools: list[tuple[int, int, range]] = []
    rejected: list[str] = []
    for first, last in shots(clip):
        pool = range(first + guard, last - shortest + 1)
        if pool:
            pools.append((first, last, pool))
        else:
            rejected.append(f"shot {first}-{last - 1} is {last - first} frames, short of the {guard} guard "
                            f"plus {shortest} excerpt frames it would need")
    pools.sort(key=lambda shot: (-(shot[1] - shot[0]), shot[0]))
    return pools, rejected


def build(args) -> int:
    manifest = load_manifest()
    clips = {c["name"]: c for c in manifest["clips"]}
    runs = load_runs(args.single, args.double)
    if not runs:
        print(f"no clip has both '{args.single}' and '{args.double}' runs under {RUNS}", file=sys.stderr)
        return 1
    plans: dict[str, list[tuple[int, int, range]]] = {}
    dropped_shots: list[dict] = []
    skipped_clips: list[dict] = []
    shortest = min(args.seconds, MIN_EXCERPT_SECONDS)
    for clip_name in sorted(runs):
        pools, rejected = candidate_stills(clips[clip_name], args.seconds)
        # A shot the ballot cannot use is named, not quietly passed over: it is
        # coverage the judgement does not have.
        for reason in rejected:
            print(f"{clip_name}: {reason}", file=sys.stderr)
            dropped_shots.append(dict(clip=clip_name, reason=reason))
        if pools:
            plans[clip_name] = pools
            continue
        reason = f"no frame of {clip_name} is provably inside a shot with {shortest:.2f} s of it left"
        print(f"skipping {clip_name}: {reason}", file=sys.stderr)
        skipped_clips.append(dict(clip=clip_name, reason=reason))
    if not plans:
        print(f"every candidate clip was excluded, so there is nothing to judge; lower "
              f"MIN_EXCERPT_SECONDS ({MIN_EXCERPT_SECONDS:.2f}) or use clips with shots longer than "
              f"{shortest:.2f} s",
              file=sys.stderr)
        return 1
    # Every pair from a previous ballot is removed first. `key.json` below is
    # overwritten, so those files are already unscoreable - `--score` only accepts
    # ids the current key holds - and leaving them in the one directory the judge is
    # told to look at invites scoring a retired question's images. Counted out loud
    # rather than deleted quietly.
    pairs_dir = BLIND / "pairs"
    pairs_dir.mkdir(parents=True, exist_ok=True)
    stale = sorted(p for p in pairs_dir.iterdir() if p.is_file())
    for path in stale:
        path.unlink()
    if stale:
        print(f"removed {len(stale)} file(s) from a previous ballot in {pairs_dir}")
    rng = random.Random(args.seed)
    key, ballot = [], []
    ids = [secrets.token_hex(3) for _ in range(len(plans) * args.pairs_per_clip)]
    rng.shuffle(ids)
    cursor = 0
    for clip_name, pools in sorted(plans.items()):
        members = runs[clip_name]
        fps = clips[clip_name]["fps"]
        for index in range(args.pairs_per_clip):
            pair_id = ids[cursor]
            cursor += 1
            # One shot per pair, round-robin, so a cut-bearing clip spreads its
            # pairs over different shots before it repeats one.
            first, last, pool = pools[index % len(pools)]
            frame = rng.choice(pool)
            length = min(args.seconds, (last - frame) / fps)
            order = ["single", "double"]
            rng.shuffle(order)
            for label, role in zip("AB", order):
                still(members[role], frame, pairs_dir / f"{pair_id}-{label}.png")
                excerpt(members[role], frame / fps, length, pairs_dir / f"{pair_id}-{label}.mp4")
            (pairs_dir / f"{pair_id}.txt").write_text(
                f"Pair {pair_id}: compare {pair_id}-A and {pair_id}-B (still and {length:.1f} s clip). "
                "Judge sharpness of fine detail/text, skin/hair naturalness, temporal stability (flicker, "
                "crawling), halos/over-smoothing and colour. Record the preferred side (A, B or tie) and a "
                "1-5 confidence in ballot.csv.\n", encoding="utf-8")
            key.append(dict(id=pair_id, clip=clip_name, frame=frame, shot=[first, last - 1],
                            seconds=round(length, 3), A=order[0], B=order[1],
                            A_run=str(members[order[0]]), B_run=str(members[order[1]])))
            ballot.append(dict(id=pair_id, preferred="", confidence=""))
            print(f"pair {pair_id}: {clip_name} frame {frame} in shot {first}-{last - 1}, {length:.2f} s",
                  flush=True)
    write_json(BLIND / "key.json", dict(seed=args.seed, single=args.single, double=args.double,
                                        guard_seconds=GUARD_SECONDS, min_excerpt_seconds=MIN_EXCERPT_SECONDS,
                                        skipped_clips=skipped_clips, dropped_shots=dropped_shots, pairs=key))
    with (BLIND / "ballot.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["id", "preferred", "confidence"])
        writer.writeheader()
        writer.writerows(ballot)
    print(f"{len(key)} pairs in {pairs_dir}; sealed key at {BLIND / 'key.json'}; ballot at "
          f"{BLIND / 'ballot.csv'}; {len(skipped_clips)} clip(s) skipped, {len(dropped_shots)} shot(s) too short")
    return 0


def score(ballot_path: Path, key_path: Path | None = None) -> int:
    # An archived ballot is scored against the key archived beside it; BLIND/key.json
    # is whatever the last build wrote, and re-scoring an old ballot against it would
    # report every row as unknown. Defaults to the live one.
    if key_path is None:
        beside = ballot_path.parent / "key.json"
        key_path = beside if beside.exists() else BLIND / "key.json"
    sealed = json.loads(key_path.read_text(encoding="utf-8"))
    key = {p["id"]: p for p in sealed["pairs"]}
    # `single` and `double` are this script's role names, from the one-pass versus
    # two-pass question it was written for. Any two profiles can be handed to
    # --single/--double, and a reader of the votes below cannot know which arm a
    # role was unless the build says so - a tone ballot scored as "double wins"
    # reads backwards to anyone who assumes the shipped arm is always `single`.
    # The names the ballot was built with are printed with the result.
    profiles = {"single": sealed.get("single"), "double": sealed.get("double")}
    votes = {"single": 0, "double": 0, "tie": 0}
    weighted = {"single": 0.0, "double": 0.0}
    per_clip: dict[str, dict[str, int]] = {}
    unknown, unscored, no_confidence, bad_confidence = [], [], [], []
    with ballot_path.open(encoding="utf-8") as f:
        for row in csv.DictReader(f):
            pair = key.get(row["id"])
            side = row["preferred"].strip().upper()
            if not pair:
                unknown.append(row["id"])
                continue
            if side not in ("A", "B", "TIE"):
                unscored.append(row["id"])
                continue
            role = "tie" if side == "TIE" else pair[side]
            votes[role] += 1
            # A blank confidence used to become 1.0 here, so a ballot with the column
            # left empty printed a confidence-weighted total that looked measured and
            # was arithmetic on a default. The rows are named and the weighting is
            # withheld instead: a rule written as "N pairs at confidence >= 3" cannot
            # be evaluated against numbers nobody supplied.
            #
            # A value that is not an integer in 1-5 is as unscoreable as a blank and
            # worse than one, because `float()` accepted "0" and "9" as if the scale
            # had them and raised on anything non-numeric, taking the whole ballot
            # down with it. Blank is an omission and scores preference-only; a value
            # out of range or unparseable is a data error and fails the run.
            confidence = (row.get("confidence") or "").strip()
            if role != "tie":
                if not confidence:
                    no_confidence.append(row["id"])
                elif confidence.isdigit() and 1 <= int(confidence) <= 5:
                    weighted[role] += float(int(confidence))
                else:
                    bad_confidence.append(f"{row['id']}={confidence!r}")
            per_clip.setdefault(pair["clip"], {"single": 0, "double": 0, "tie": 0})[role] += 1
    weighting_measured = not no_confidence and not bad_confidence
    # A row this key does not know, or one with no preference in it, is reported
    # rather than skipped. Scoring an unfilled ballot used to print a clean sweep of
    # zeros and exit 0, which reads exactly like a measured tie; a ballot filled in
    # against a since-rebuilt key would have read the same way. Neither is a result.
    for ballot_id in unknown:
        print(f"ballot row '{ballot_id}' is not in key.json - the ballot and the key are from "
              f"different builds", file=sys.stderr)
    if unscored:
        print(f"{len(unscored)} of {len(unscored) + sum(votes.values())} pair(s) carry no "
              f"preference: {', '.join(unscored)}", file=sys.stderr)
    if no_confidence:
        print(f"{len(no_confidence)} pair(s) carry a preference with no confidence: "
              f"{', '.join(no_confidence)}. The weighting is withheld, so a threshold "
              f"written in terms of confidence cannot be checked against this ballot.",
              file=sys.stderr)
    if bad_confidence:
        print(f"{len(bad_confidence)} pair(s) carry a confidence that is not an integer "
              f"in 1-5: {', '.join(bad_confidence)}. The scale has five points; a value "
              f"outside it is a data error, not a weak preference.", file=sys.stderr)
    print(json.dumps(dict(profiles=profiles, votes=votes,
                          confidence_weighted=(weighted if weighting_measured else None),
                          confidence_missing=no_confidence,
                          confidence_invalid=bad_confidence,
                          per_clip=per_clip, unscored=unscored, unknown=unknown), indent=2))
    # Spelled out in prose too, because the JSON above is the part that gets pasted
    # into a report and the roles are meaningless without their profiles.
    if sum(votes.values()):
        for role in ("single", "double"):
            tail = f", confidence-weighted {weighted[role]:.1f}" if weighting_measured else \
                   " (confidence not usable)"
            print(f"{votes[role]} of {sum(votes.values())} pair(s) preferred "
                  f"{profiles[role] or role} (role '{role}'){tail}", file=sys.stderr)
        print(f"{votes['tie']} tie(s)", file=sys.stderr)
    if unknown or bad_confidence or not sum(votes.values()):
        print("nothing was scored" if not sum(votes.values()) else
              "the ballot does not match the key" if unknown else
              "the ballot carries an unusable confidence value", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--single", default="baseline", help="single-pass profile name")
    parser.add_argument("--double", default="two-pass", help="two-pass profile name")
    parser.add_argument("--pairs-per-clip", type=int, default=3)
    parser.add_argument("--seconds", type=float, default=3.0,
                        help="longest excerpt to cut; a shot shorter than this caps its own pairs")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--score", type=Path, help="score a filled ballot.csv against key.json")
    parser.add_argument("--key", type=Path, help="key.json to score against; defaults to one "
                                                 "beside the ballot, else the live blind/ key")
    args = parser.parse_args()
    return score(args.score, args.key) if args.score else build(args)


if __name__ == "__main__":
    sys.exit(main())
