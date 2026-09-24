"""The add-on's normalization governor: does turning it off bring back brightness pumping?

RenoDX 6.5.3's `NRNormGovernor` (0 off, 1 slew, 2 stable; the add-on default is 2 and
the player does not write the key) damps how fast the neural pass's normalization
divisor moves. docs/measurements/knobs-653-20260924 found that at 2 a repeat of one
configuration differs from itself on saturated synthetic clips, and at 0 it does not.
This renders the player's shipped settings at each governor value, twice, and reports
what off costs in the terms a governor exists for:

  repeat      whether the two renders of one configuration are byte-identical, and
              how many frames differ when they are not
  flicker+    analyze.py's added mean |dY| between consecutive frames (cuts excluded),
              computed the same way (analyze.luma, analyze.ShotSigma) in one pass that
              skips analyze.py's motion field, which is not what a governor moves
  sigma+      analyze.py's added per-pixel temporal standard deviation inside a shot
  pump+       added frame-to-frame change of the frame's MEAN luma: mean |dYbar| of the
              output minus the source's, over non-cut pairs. Pumping is a whole-frame
              brightness swing, which per-pixel flicker dilutes into texture noise
  wander      standard deviation, within shots, of Ybar(output) - Ybar(source): how far
              the pass's brightness offset moves while the scene's does not
  step        at a labelled exposure change, the output's mean-luma step over the
              source's (1.00 follows it exactly), and the largest |dYbar| of the output
              in the 12 frames after it against the source's (overshoot or lag)

Beside the corpus clips it builds `real-lighting`: `real-game-motion` played forward,
backward and forward again (7.6 s with no cut) under an exposure schedule - a step
up, a hold, a ramp down below the start, a step back - so a governor has attack,
release and slow drift to act on in real footage. The luminance-over-time plot of
each clip is written beside the table.

    set DLSS_BENCHMARK_BUILD=C:\\t\\bb & set DLSS_BENCHMARK_FFMPEG=<dir with ffmpeg.exe>
    python tools/benchmark/corpus.py --corpus C:\\t\\bb\\benchmark-corpus --clips flash-exposure depth-pan ...
    python tools/benchmark/governor.py --clips flash-exposure depth-pan real-lighting --out <report dir>
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path

import numpy as np

import analyze
import corpus as corpus_module
import run
from common import CORPUS, RUNS, FrameReader, framemd5, load_manifest, write_json
from knobs import SHIPPED

GOVERNORS = {"gov-0": "0", "gov-1": "1", "gov-2": "2"}

# real-lighting's exposure schedule, in seconds of the 30 fps clip: +0.15 at 1.5 s,
# held to 3.5 s, ramped to -0.12 by 5.0 s, held, and back to 0 at 6.5 s.
LIGHTING = ("if(lt(t,1.5),0,if(lt(t,3.5),0.15,if(lt(t,5.0),0.15-0.27*(t-3.5)/1.5,"
            "if(lt(t,6.5),-0.12,0))))")
LIGHTING_STEPS = {"real-lighting": [45, 195], "flash-exposure": [30, 34, 60]}


def build_lighting(corpus: Path) -> dict:
    first, count = 506, 76  # corpus.build_real_game_motion's span
    segment = corpus_module.real_segment(first, count)
    graph = (f"[0:v]{segment},split=3[a][b][c];[b]reverse[r];[a][r][c]concat=n=3:v=1:a=0,"
             f"setpts=N/{corpus_module.FPS}/TB,eq=brightness='{LIGHTING}':eval=frame,format=yuv420p")
    corpus_module.ffmpeg(["-i", corpus_module.demo(), "-filter_complex", graph, "-frames:v", str(3 * count),
                          *corpus_module.ENCODE, "real-lighting.mkv"], corpus)
    return corpus_module.describe(corpus, dict(
        name="real-lighting", category="real", synthetic=False, cuts=[], text=[],
        source=f"{corpus_module.DEMO_RELATIVE} frames {first}-{first + count - 1}, forward/back/forward",
        notes="NR-processed capture (real-game-motion) played forward, reversed and forward again, under "
              "eq brightness +0.15 from 1.5 s, a ramp to -0.12 over 3.5-5.0 s and 0 again from 6.5 s. "
              "The two direction reversals are motion reversals, not cuts."))


def clips_for(names: list[str], corpus: Path) -> list[dict]:
    manifest = load_manifest(corpus)
    known = {c["name"]: c for c in manifest["clips"]}
    if "real-lighting" in names and "real-lighting" not in known:
        known["real-lighting"] = build_lighting(corpus)
        manifest["clips"].append(known["real-lighting"])
        write_json(corpus / "manifest.json", manifest)
    missing = [n for n in names if n not in known]
    if missing:
        raise SystemExit(f"not in {corpus / 'manifest.json'}: {', '.join(missing)}; build them with corpus.py")
    return [known[n] for n in names]


def luma_pass(path: Path, width: int, height: int, cuts: set[int]) -> dict:
    """One decode: per-frame mean luma, per-pair mean |dY| and shot sigma (analyze.py's)."""
    reader = FrameReader(path, width, height)
    means, pair_flicker, sigma, previous = [], {}, analyze.ShotSigma(), None
    for index, frame in enumerate(reader):
        y = analyze.luma(frame)
        if index in cuts:
            sigma.cut()
        sigma.add(y)
        if previous is not None and index not in cuts:
            pair_flicker[index] = float(np.abs(y - previous).mean())
        previous = y
        means.append(float(y.mean()))
    reader.close()
    return dict(means=means, flicker=pair_flicker, sigma=sigma.result())


def mean_luma(path: Path, width: int, height: int) -> list[float]:
    return luma_pass(path, width, height, set())["means"]


def pairs(n: int, cuts: set[int]) -> list[int]:
    return [i for i in range(1, n) if i not in cuts]


def shot_std(values: list[float], cuts: set[int]) -> float:
    shots, current = [], []
    for i, v in enumerate(values):
        if i in cuts and current:
            shots.append(current)
            current = []
        current.append(v)
    shots.append(current)
    weighted = [(len(s), statistics.pstdev(s)) for s in shots if len(s) > 1]
    return sum(n * s for n, s in weighted) / sum(n for n, _ in weighted)


def score(clip: dict, name: str, src: dict) -> dict:
    cuts = set(clip["cuts"])
    source = src["means"]
    reps = sorted(RUNS.glob(f"{clip['name']}__{name}__*"))
    passes = [luma_pass(r / "output.mkv", clip["width"], clip["height"], cuts) for r in reps]
    outputs = [p["means"] for p in passes]
    hashes = [framemd5(r / "output.mkv", r / "frames.md5") for r in reps]
    idx = pairs(len(source), cuts)
    d_src = statistics.fmean(abs(source[i] - source[i - 1]) for i in idx)
    flicker_src = statistics.fmean(src["flicker"].values())
    rows = [dict(run=r.name, ok=len(p["means"]) == clip["frames"],
                 flicker_added=statistics.fmean(p["flicker"].values()) - flicker_src,
                 temporal_sigma_added=(p["sigma"][0] - src["sigma"][0]) if p["sigma"] and src["sigma"] else None)
            for r, p in zip(reps, passes)]
    per_rep = []
    for row, out in zip(rows, outputs):
        gain = [o - s for o, s in zip(out, source)]
        steps = []
        for at in LIGHTING_STEPS.get(clip["name"], []):
            ds, do = source[at] - source[at - 1], out[at] - out[at - 1]
            after = range(at + 1, min(at + 13, len(out)))
            steps.append(dict(frame=at, source_step=ds, output_step=do, follow=do / ds if abs(ds) > 1e-6 else None,
                              after_max_output=max((abs(out[i] - out[i - 1]) for i in after), default=0.0),
                              after_max_source=max((abs(source[i] - source[i - 1]) for i in after), default=0.0)))
        per_rep.append(dict(run=row["run"], ok=row["ok"], flicker_added=row.get("flicker_added"),
                            sigma_added=row.get("temporal_sigma_added"),
                            pump_added=statistics.fmean(abs(out[i] - out[i - 1]) for i in idx) - d_src,
                            wander=shot_std(gain, cuts), mean_gain=statistics.fmean(gain), steps=steps))
    differing = [sum(a != b for a, b in zip(hashes[0], h)) for h in hashes[1:]]
    first_diff = [next((i for i, (a, b) in enumerate(zip(hashes[0], h)) if a != b), None) for h in hashes[1:]]
    return dict(clip=clip["name"], profile=name, governor=GOVERNORS[name], repeats=len(reps),
                repeat_identical=all(d == 0 for d in differing), repeat_frames_differing=differing,
                repeat_first_differing_frame=first_diff, frames=len(hashes[0]) if hashes else 0,
                runs=per_rep, luma=outputs[0] if outputs else [])


def plot(clip: dict, source: list[float], scored: list[dict], out: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, (top, bottom) = plt.subplots(2, 1, figsize=(10, 6), sharex=True)
    top.plot(source, color="#888888", label="source", linewidth=1.2)
    colours = {"gov-0": "#1f77b4", "gov-1": "#2ca02c", "gov-2": "#d62728"}
    for s in scored:
        if not s["luma"]:
            continue
        top.plot(s["luma"], color=colours[s["profile"]], label=f"output, governor {s['governor']}", linewidth=1)
        bottom.plot([o - v for o, v in zip(s["luma"], source)], color=colours[s["profile"]],
                    label=f"governor {s['governor']}", linewidth=1)
    for c in clip["cuts"]:
        for axis in (top, bottom):
            axis.axvline(c, color="#bbbbbb", linestyle=":", linewidth=0.8)
    top.set_ylabel("mean luma (0-255)")
    bottom.set_ylabel("output - source")
    bottom.set_xlabel("frame")
    top.set_title(f"{clip['name']}: frame mean luma, shipped settings, by NRNormGovernor")
    top.legend(loc="best", fontsize=8)
    bottom.legend(loc="best", fontsize=8)
    fig.tight_layout()
    fig.savefig(out, dpi=110)
    plt.close(fig)


def fmt(v, digits=3):
    return "-" if v is None else f"{v:.{digits}f}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", type=Path, default=CORPUS)
    parser.add_argument("--clips", nargs="+", required=True)
    parser.add_argument("--profiles", nargs="*", default=list(GOVERNORS))
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--out", type=Path, required=True, help="directory for governor.json and the plots")
    parser.add_argument("--compare-only", action="store_true")
    args = parser.parse_args()
    clips = clips_for(args.clips, args.corpus)
    specs = {name: run.profile(overrides={**SHIPPED, "NRNormGovernor": GOVERNORS[name]},
                               description=f"shipped settings, NRNormGovernor={GOVERNORS[name]}")
             for name in args.profiles}
    if not args.compare_only:
        for clip in clips:
            for name, spec in specs.items():
                for rep in range(1, args.repeats + 1):
                    run.run_one(clip, name, spec, rep, args.timeout, args.corpus, False)
    args.out.mkdir(parents=True, exist_ok=True)
    table = []
    for clip in clips:
        src = luma_pass(args.corpus / clip["file"], clip["width"], clip["height"], set(clip["cuts"]))
        scored = [score(clip, name, src) for name in specs]
        plot(clip, src["means"], scored, args.out / f"luma-{clip['name']}.png")
        table += scored
        print(f"scored {clip['name']}", flush=True)
    write_json(args.out / "governor.json", table)
    print("| clip | governor | repeat | flicker+ | sigma+ | pump+ | wander | step follow / after (out vs src) |")
    print("|---|---|---|---:|---:|---:|---:|---|")
    for s in table:
        r = s["runs"][0] if s["runs"] else {}
        repeat = "identical" if s["repeat_identical"] else \
            f"{s['repeat_frames_differing']} of {s['frames']} differ from {s['repeat_first_differing_frame']}"
        steps = "; ".join(f"f{x['frame']} {fmt(x['follow'], 2)} / {fmt(x['after_max_output'], 2)} vs "
                          f"{fmt(x['after_max_source'], 2)}" for x in r.get("steps", []))
        print(f"| {s['clip']} | {s['governor']} | {repeat} | {fmt(r.get('flicker_added'))} | "
              f"{fmt(r.get('sigma_added'))} | {fmt(r.get('pump_added'))} | {fmt(r.get('wander'))} | {steps or '-'} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
