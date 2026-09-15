"""Scores the scene-cut criterion itself against the corpus manifest's labelled cuts.

``analyze.py`` asks what a rendered run did; this asks whether the test the generator
applies is the right test. It replays ``cutmirror`` - the mirror of
``src/TemporalGuides.cpp`` - over the corpus clips, matches every accepted history reset
against the manifest ground truth, and sweeps the shipped criterion (an absolute
residual with a histogram gate) against the scale-free candidate of roadmap survey item
3 (the fraction of cells whose best displacement failed to beat no prediction).

The labelled set is every clip in the manifest: ``cuts`` are the frames a reset must
land on, ``soft_cuts`` are gradual transitions where one reset is tolerated and a second
inside the same transition is not, and a clip with neither must never reset at all.

    python tools/benchmark/cutlab.py [--corpus DIR] [--cache DIR] [--clips NAME ...]
                                     [--sweep] [--debounce S ...] [--json OUT]

Cell features are expensive to extract and independent of every threshold, so they are
cached under ``--cache`` (default ``<corpus>/cutlab-cache``) keyed by the clip's frame
digest; a sweep after the first run costs nothing but the replay. ``--cache`` exists
because a shared labelled corpus is often mounted read-only.

``--debounce`` sweeps the minimum-interval window instead of the thresholds. The window
is the one decision a threshold sweep cannot reach: it adjudicates only weak-arm fires
that land close behind an accepted cut, so it is invisible in every clip that has no
such pair.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

import cutmirror as cm
from common import CORPUS, FrameReader, load_manifest, write_json


def extract(clip: dict, corpus: Path, cache: Path) -> list[dict]:
    """Per-consecutive-pair features for one clip, cached on the clip's own digest."""
    store = cache / f"{clip['name']}-{clip['digest'][:16]}.npz"
    if store.exists():
        data = np.load(store)
        return [dict(residual=float(data["residual"][i]), histogram_overlap=float(data["overlap"][i]),
                     cell_best=data["best"][i], cell_zero=data["zero"][i])
                for i in range(data["residual"].size)]
    grid = cm.GuideGrid(clip["width"], clip["height"], clip["fps"])
    reader = FrameReader(corpus / clip["file"], clip["width"], clip["height"])
    features, previous = [], None
    for rgb in reader:
        cells = grid.cells(rgb)
        if previous is not None:
            features.append(cm.pair_features(cells, previous, cells=True))
        previous = cells
    cache.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(store,
                        residual=np.array([f["residual"] for f in features], dtype=np.float64),
                        overlap=np.array([f["histogram_overlap"] for f in features], dtype=np.float64),
                        best=np.stack([f["cell_best"] for f in features]),
                        zero=np.stack([f["cell_zero"] for f in features]))
    return features


def replay(clip: dict, features: list[dict], criterion: cm.Criterion,
           seconds_between_cuts: float = cm.MIN_SECONDS_BETWEEN_CUTS) -> cm.CutRun:
    run = cm.CutRun(clip["fps"], criterion, seconds_between_cuts)
    for offset, feature in enumerate(features):
        run.decide(offset + 1, feature)  # feature i compares frame i+1 against frame i
    return run


def judge(clip: dict, run: cm.CutRun,
          seconds_between_cuts: float = cm.MIN_SECONDS_BETWEEN_CUTS) -> dict:
    """Score one replay: the manifest's verdict plus the over-reset count.

    Over-resetting is measured against the window in force, not against a fixed
    distance: a second reset is only "inside one transition" if the generator would
    have had the chance to withhold it.
    """
    truth, soft = clip.get("cuts", []), clip.get("soft_cuts", [])
    scores = cm.cut_scores(run.cuts, truth, soft_cuts=soft)
    # The clip's own rate, not 30: the camera-original clips run at 23.976, where
    # the shipped 0.3 s debounce is 7 frames rather than 9. Scoring them against a
    # 30 fps window counts a second fire the mirror would actually have accepted.
    window = cm.min_frames_between_cuts(clip["fps"], seconds_between_cuts)
    multi = sum(1 for cut in truth
                for fire in run.cuts if 0 < fire - cut < window)
    multi += sum(max(0, sum(1 for fire in run.cuts if first <= fire <= last) - 1)
                 for first, last in soft)
    return dict(clip=clip["name"], expected=truth, soft=soft, fired=run.cuts,
                suppressed=run.suppressed, multi_fire=multi, **scores)


def pooled(verdicts: list[dict]) -> dict:
    """Micro-averaged P/R/F1 over the labelled set: one clip must not outvote the rest."""
    matched = sum(v["matched"] for v in verdicts)
    scored = sum(len(v["fired"]) - v["tolerated"] for v in verdicts)
    wanted = sum(len(v["expected"]) for v in verdicts)
    precision = matched / scored if scored else None
    recall = matched / wanted if wanted else None
    f1 = None if precision is None or recall is None else \
        (0.0 if precision + recall == 0 else 2 * precision * recall / (precision + recall))
    return dict(precision=precision, recall=recall, f1=f1, matched=matched, detections=scored,
                truth=wanted, false_positives=sum(len(v["false_positives"]) for v in verdicts),
                missed=sum(v["missed"] for v in verdicts), multi_fire=sum(v["multi_fire"] for v in verdicts),
                wrong=[v["clip"] for v in verdicts if v["missed"] or v["false_positives"] or v["multi_fire"]])


def fmt(value, digits=3) -> str:
    if value is None:
        return "-"
    return f"{value:.{digits}f}" if isinstance(value, float) else str(value)


def firing_table(clip: dict, run: cm.CutRun, features: list[dict]) -> list[str]:
    lines = [f"### {clip['name']}  ({len(run.evidence)} decisions, {len(run.cuts)} accepted, "
             f"{len(run.suppressed)} suppressed; truth {clip.get('cuts', [])})",
             "| frame | arm | residual | overlap | failed frac | verdict |",
             "|---:|---|---:|---:|---:|---|"]
    for row in run.evidence:
        feature = features[row["frame"] - 1]
        lines.append(f"| {row['frame']} | {row['arm']} | {row['residual']:.4f} | "
                     f"{row['histogram_overlap']:.4f} | "
                     f"{cm.failed_fraction(feature['cell_best'], feature['cell_zero']):.4f} | "
                     f"{'suppressed' if row['suppressed'] else 'RESET'} |")
    return lines


def margin_table(clips: list[dict], features: dict) -> list[str]:
    """What separates the labelled cuts from everything else, before any threshold.

    A negative case only earns its place if it comes close to firing, so the worst
    non-cut frame of each clip is reported next to its cut frames.
    """
    lines = ["| clip | frames | at the cuts (residual / overlap / failed) | worst other frame |",
             "|---|---:|---|---|"]
    for clip in clips:
        rows = features[clip["name"]]
        fraction = [cm.failed_fraction(f["cell_best"], f["cell_zero"]) for f in rows]
        cuts = {c - 1 for c in clip.get("cuts", [])}
        at = " ".join(f"{rows[i]['residual']:.3f}/{rows[i]['histogram_overlap']:.3f}/{fraction[i]:.3f}"
                      for i in sorted(cuts)) or "-"
        other = [i for i in range(len(rows)) if i not in cuts]
        peak = max(other, key=lambda i: rows[i]["residual"]) if other else None
        worst = "-" if peak is None else (f"{peak + 1}: {rows[peak]['residual']:.3f}/"
                                          f"{rows[peak]['histogram_overlap']:.3f}/"
                                          f"{max(fraction[i] for i in other):.3f} max failed")
        lines.append(f"| {clip['name']} | {len(rows) + 1} | {at} | {worst} |")
    return lines


def ordering_table(clips: list[dict], features: dict, criteria: list[cm.Criterion]) -> list[str]:
    """Where the labelled cuts sit in each score's own ordering, thresholds aside.

    Two arms and a debounce can rescue a score whose ordering is wrong, but only so far:
    if a labelled non-cut outranks a labelled cut, no single threshold can tell them
    apart and the arms are carrying the decision. Soft-cut spans are in neither column.
    """
    lines = ["| score | three weakest true cuts | five highest non-cut frames | separable |",
             "|---|---|---|---|"]
    for criterion in criteria:
        cuts, others = [], []
        for clip in clips:
            truth = {c - 1 for c in clip.get("cuts", [])}
            soft = {i for first, last in clip.get("soft_cuts", []) for i in range(first - 1, last)}
            for index, feature in enumerate(features[clip["name"]]):
                if index in soft:
                    continue
                bucket = cuts if index in truth else others
                bucket.append((criterion.score(feature), clip["name"], index + 1))
        cuts.sort()
        others.sort(reverse=True)
        show = lambda rows: ", ".join(f"{v:.4f} {name}@{frame}" for v, name, frame in rows)
        lines.append(f"| {criterion.name} | {show(cuts[:3])} | {show(others[:5])} | "
                     f"{'yes' if cuts and others and cuts[0][0] > others[0][0] else 'no'} |")
    return lines


# The residual arm cannot express "no histogram gate", so the sweep gives it an
# overlap of 1.01: every overlap is below that. The candidate takes None for the same
# thing, because the point of a scale-free score is that it may not need the gate.
NO_HISTOGRAM_GATE = 1.01


def sweep_grid(kind: str) -> list[cm.Criterion]:
    if kind == "residual":
        return [cm.ResidualCriterion(strong, weak, overlap)
                for strong in (0.15, 0.20, 0.25, 0.30, 0.35, 0.40)
                for weak in (0.06, 0.08, 0.10, 0.13, 0.16, 0.20)
                for overlap in (0.75, 0.80, 0.85, 0.90, 0.95, NO_HISTOGRAM_GATE)
                if weak < strong]
    return [cm.FailedFractionCriterion(strong, weak, ratio, floor, overlap)
            for ratio in (0.50, 0.60, 0.70, 0.80, 0.90)
            for floor in (0.005, 0.010, 0.020, 0.040, 0.080)
            for strong in (0.30, 0.40, 0.50, 0.60, 0.70, 0.80)
            for weak in (0.10, 0.15, 0.20, 0.30, 0.40)
            for overlap in (0.85, None)
            if weak < strong]


def rank(result: dict) -> tuple:
    """Best first: most correct clips, then F1, then fewest over-resets."""
    return (-len(result["pooled"]["wrong"]), result["pooled"]["f1"] or 0.0,
            -result["pooled"]["multi_fire"], -result["pooled"]["false_positives"])


def evaluate(clips: list[dict], features: dict, criterion: cm.Criterion,
             seconds_between_cuts: float = cm.MIN_SECONDS_BETWEEN_CUTS) -> dict:
    verdicts = [judge(clip, replay(clip, features[clip["name"]], criterion, seconds_between_cuts),
                      seconds_between_cuts) for clip in clips]
    return dict(criterion=str(criterion), kind=criterion.name, window=seconds_between_cuts,
                verdicts=verdicts, pooled=pooled(verdicts))


def debounce_clip_table(clips: list[dict], results: list[dict]) -> list[str]:
    """Precision and recall per clip, one column per swept window.

    A clip whose weak arm never fires close behind an accepted cut is constant across
    the whole sweep; the ``varies`` column says which clips the window actually decides,
    so a reader can see how narrow the evidence for a window length really is.
    """
    head = " | ".join(f"{r['window']:g} s / {cm.min_frames_between_cuts(30.0, r['window'])}f"
                      for r in results)
    lines = [f"| clip | truth | {head} | varies |", "|---|---|" + "---:|" * len(results) + "---|"]
    for index, clip in enumerate(clips):
        cells = [f"{fmt(r['verdicts'][index]['precision'], 2)}/"
                 f"{fmt(r['verdicts'][index]['recall'], 2)}" for r in results]
        fired = [str(r["verdicts"][index]["fired"]) for r in results]
        lines.append(f"| {clip['name']} | {clip.get('cuts') or clip.get('soft_cuts') or '-'} | "
                     f"{' | '.join(cells)} | {'yes' if len(set(fired)) > 1 else 'no'} |")
    lines += ["", f"| pooled | P | R | F1 | FP | missed | multi | clips wrong |",
              "|---|---:|---:|---:|---:|---:|---:|---|"]
    for result in results:
        p = result["pooled"]
        lines.append(f"| {result['window']:g} s "
                     f"({cm.min_frames_between_cuts(30.0, result['window'])}f @30) | "
                     f"{fmt(p['precision'])} | {fmt(p['recall'])} | {fmt(p['f1'])} | "
                     f"{p['false_positives']} | {p['missed']} | {p['multi_fire']} | "
                     f"{', '.join(p['wrong']) or 'none'} |")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", type=Path, default=CORPUS)
    parser.add_argument("--clips", nargs="*", help="clip names; default every clip in the manifest")
    parser.add_argument("--cache", type=Path, help="feature cache; default <corpus>/cutlab-cache")
    parser.add_argument("--sweep", action="store_true", help="also sweep both criteria over the labelled set")
    parser.add_argument("--debounce", type=float, nargs="*", metavar="SECONDS",
                        help="sweep the minimum-interval window instead of the thresholds; "
                             "omit the values for a default bracket around the shipped one")
    parser.add_argument("--json", type=Path, help="write the full result, sweep included, here")
    args = parser.parse_args()

    manifest = load_manifest(args.corpus)
    clips = [c for c in manifest["clips"] if not args.clips or c["name"] in args.clips]
    if not clips:
        print("no clips selected", file=sys.stderr)
        return 1
    features = {c["name"]: extract(c, args.corpus, args.cache or args.corpus / "cutlab-cache")
                for c in clips}

    shipped, candidate = cm.ResidualCriterion(), cm.FailedFractionCriterion()
    lines = ["# Scene-cut criterion lab", "",
             f"Corpus `{args.corpus}`, {len(clips)} clips, "
             f"{sum(len(f) for f in features.values())} consecutive pairs.", "",
             "## Separation before any threshold", ""]
    lines += margin_table(clips, features)
    lines += ["", "## Where the labelled cuts sit in each score's ordering", ""]
    lines += ordering_table(clips, features, [shipped, candidate])
    lines += ["", "## Shipped criterion, every decision", ""]
    for clip in clips:
        lines += firing_table(clip, replay(clip, features[clip["name"]], shipped),
                              features[clip["name"]]) + [""]

    results = [evaluate(clips, features, shipped), evaluate(clips, features, candidate)]
    lines += ["## Per-clip verdicts", "",
              "| criterion | clip | truth | fired | suppressed | missed | false pos | multi |",
              "|---|---|---|---|---|---:|---|---:|"]
    for result in results:
        for v in result["verdicts"]:
            lines.append(f"| {result['kind']} | {v['clip']} | {v['expected'] or v['soft'] or '-'} | "
                         f"{v['fired'] or '-'} | {v['suppressed'] or '-'} | {v['missed']} | "
                         f"{v['false_positives'] or '-'} | {v['multi_fire']} |")
    lines += ["", "| criterion | P | R | F1 | FP | missed | multi | clips wrong |", "|---|---:|---:|---:|---:|---:|---:|---|"]
    for result in results:
        p = result["pooled"]
        lines.append(f"| {result['criterion']} | {fmt(p['precision'])} | {fmt(p['recall'])} | {fmt(p['f1'])} | "
                     f"{p['false_positives']} | {p['missed']} | {p['multi_fire']} | {', '.join(p['wrong']) or 'none'} |")

    sweep: dict = {}
    if args.sweep:
        for kind in ("residual", "failed-fraction"):
            ranked = sorted((evaluate(clips, features, c) for c in sweep_grid(kind)), key=rank, reverse=True)
            sweep[kind] = ranked
            lines += ["", f"## Sweep: {kind} ({len(ranked)} points, best 8)", "",
                      "| thresholds | P | R | F1 | FP | missed | multi | clips wrong |",
                      "|---|---:|---:|---:|---:|---:|---:|---|"]
            for result in ranked[:8]:
                p = result["pooled"]
                lines.append(f"| {result['criterion']} | {fmt(p['precision'])} | {fmt(p['recall'])} | "
                             f"{fmt(p['f1'])} | {p['false_positives']} | {p['missed']} | {p['multi_fire']} | "
                             f"{', '.join(p['wrong']) or 'none'} |")
        lines += ["", "## Verdict", "",
                  "The comparison that decides whether the candidate is worth shipping is the best "
                  "operating point each family can reach with no missed cut and no over-reset.", "",
                  "| family | points | reachable with 0 missed and 0 multi | fewest false positives there | "
                  "best F1 anywhere |", "|---|---:|---:|---:|---:|"]
        for kind, ranked in sweep.items():
            clean = [r for r in ranked if r["pooled"]["missed"] == 0 and r["pooled"]["multi_fire"] == 0]
            fewest = min((r["pooled"]["false_positives"] for r in clean), default=None)
            lines.append(f"| {kind} | {len(ranked)} | {len(clean)} | {fmt(fewest)} | "
                         f"{fmt(max((r['pooled']['f1'] or 0.0) for r in ranked))} |")

    debounce: list[dict] = []
    if args.debounce is not None:
        windows = args.debounce or [0.0, 0.1, 0.133, 0.167, 0.2, 0.3, 0.4, 0.5, 0.567, 0.6, 0.8]
        debounce = [evaluate(clips, features, shipped, w) for w in sorted(set(windows))]
        lines += ["", f"## Sweep: debounce window ({len(debounce)} windows, shipped criterion)", ""]
        lines += debounce_clip_table(clips, debounce)

    print("\n".join(lines))
    if args.json:
        write_json(args.json, dict(corpus=str(args.corpus), results=results,
                                   sweep={k: [dict(criterion=r["criterion"], pooled=r["pooled"]) for r in v]
                                          for k, v in sweep.items()},
                                   debounce=[dict(window=r["window"], verdicts=r["verdicts"],
                                                  pooled=r["pooled"]) for r in debounce]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
