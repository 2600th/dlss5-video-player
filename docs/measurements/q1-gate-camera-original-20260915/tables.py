"""Re-derives every number in REPORT.md from the two arms' run directories.

Reads only what the harness already wrote - per-run `metrics.json` (written by
`tools/benchmark/analyze.py --no-ocr --no-faces`) and the rendered `output.mkv`
of each run - and prints three blocks:

  digests   per-arm rgb24 frame-sequence digest per repeat, the within-arm
            determinism check, and how many frames differ between the arms
            (the per-arm assertion that the gate engaged on this material)
  tables    the A/B table, the shipped-intensity-0 carrier floor and the share
            of the neural pass's own false motion the gate removes
  cutdist   false motion recomputed over pairs at least N frames from any
            labelled cut, which is what answers "is this a cut artifact"

    python tables.py --on <arm dir> --off <arm dir> --corpus <corpus dir>

The arm directories are `<worktree>/build-upscaling/benchmark-work/runs`. The
false-motion recomputation mirrors `analyze.py`'s MotionField exactly (same
GuideGrid, same 2/255 cell tolerance, same exclusion of pairs spanning a cut)
and adds only the distance filter, so block three is comparable with block two.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np

CELL_TOLERANCE = 2 / 255
CLIPS = ["orig-film-cuts-a", "orig-film-cuts-b", "orig-film-fade", "orig-faces",
         "orig-game-cuts", "orig-game-motion", "orig-dissolve"]
PROFILE = "shipped-defaults"
CONTROL = "shipped-intensity-0"
DEPTH_OFF = "shipped-depth-constant"
DEPTH_OFF_CLIPS = ["orig-faces", "orig-game-cuts"]


def metrics(runs: Path, clip: str, profile: str, rep: int) -> dict:
    return json.loads((runs / f"{clip}__{profile}__{rep}" / "metrics.json").read_text(encoding="utf-8"))


def pick(m: dict, *path):
    for key in path:
        m = m[key] if m is not None else None
    return m


def median_of_identical_repeats(runs: Path, clip: str, profile: str, *path):
    """Both repeats are bit-identical renders, so a disagreement here is a bug, not noise."""
    values = [pick(metrics(runs, clip, profile, rep), *path) for rep in (1, 2)]
    if values[0] != values[1]:
        raise SystemExit(f"{clip}/{profile}{path}: repeats disagree {values}")
    return values[0]


def frame_hashes(ffmpeg: Path, path: Path) -> list[str]:
    import subprocess
    out = subprocess.run([str(ffmpeg), "-v", "error", "-i", str(path), "-map", "0:v:0",
                          "-pix_fmt", "rgb24", "-f", "framemd5", "-"],
                         capture_output=True, text=True, check=True).stdout
    return [line.split(",")[-1].strip() for line in out.splitlines() if line and not line.startswith("#")]


def false_motion(source, output, cuts: set[int], min_distance: int):
    static = invented = pairs = 0
    for i in range(1, len(source)):
        if i in cuts:
            continue
        if min_distance and cuts and min(abs(i - c) for c in cuts) < min_distance:
            continue
        moving_source = np.abs(source[i] - source[i - 1]) > CELL_TOLERANCE
        moving_output = np.abs(output[i] - output[i - 1]) > CELL_TOLERANCE
        held_static = ~moving_source
        static += int(held_static.sum())
        invented += int((held_static & moving_output).sum())
        pairs += 1
    return (invented / static if static else None), pairs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--on", type=Path, required=True, help="gate-on arm runs directory")
    parser.add_argument("--off", type=Path, required=True, help="gate-off arm runs directory")
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--benchmark", type=Path, required=True,
                        help="tools/benchmark of the tree whose GuideGrid/FrameReader to use")
    parser.add_argument("--ffmpeg", type=Path, required=True)
    parser.add_argument("--blocks", nargs="*", default=["digests", "tables", "cutdist", "depthoff"])
    args = parser.parse_args()
    sys.path.insert(0, str(args.benchmark))
    from cutmirror import GuideGrid            # noqa: E402
    from common import FrameReader             # noqa: E402

    manifest = json.loads((args.corpus / "manifest.json").read_text(encoding="utf-8"))
    clips = {c["name"]: c for c in manifest["clips"]}
    arms = {"on": args.on, "off": args.off}

    if "digests" in args.blocks:
        print("## digests")
        print(f"{'clip':18s} {'fr':>4s} {'on rep1':>17s} {'on rep2':>17s} {'off rep1':>17s} {'off rep2':>17s} "
              f"{'det':>5s} {'differ':>8s} identical frames")
        for clip in CLIPS:
            hs = {}
            for tag, runs in arms.items():
                for rep in (1, 2):
                    hs[(tag, rep)] = frame_hashes(args.ffmpeg, runs / f"{clip}__{PROFILE}__{rep}" / "output.mkv")
            digest = lambda h: hashlib.sha256("\n".join(h).encode()).hexdigest()[:16]
            same = [i for i, (a, b) in enumerate(zip(hs[("on", 1)], hs[("off", 1)])) if a == b]
            differ = len(hs[("on", 1)]) - len(same)
            det = hs[("on", 1)] == hs[("on", 2)] and hs[("off", 1)] == hs[("off", 2)]
            print(f"{clip:18s} {len(hs[('on', 1)]):4d} {digest(hs[('on', 1)]):>17s} {digest(hs[('on', 2)]):>17s} "
                  f"{digest(hs[('off', 1)]):>17s} {digest(hs[('off', 2)]):>17s} {str(det):>5s} "
                  f"{differ:>4d}/{len(hs[('on', 1)]):<3d} {same}")

    if "tables" in args.blocks:
        print("\n## tables")
        head = (f"{'clip':18s} {'fm on':>9s} {'fm off':>9s} {'delta':>9s} {'rel %':>7s} | "
                f"{'flip on':>8s} {'flip off':>8s} {'delta':>9s} | {'sig+ on':>8s} {'sig+ off':>8s} {'delta':>8s} | "
                f"{'psnr on':>8s} {'psnr off':>8s}")
        print(head)
        for clip in CLIPS:
            get = lambda tag, *p: median_of_identical_repeats(arms[tag], clip, PROFILE, *p)
            fm_on, fm_off = get("on", "motion_field", "false_motion_rate"), get("off", "motion_field", "false_motion_rate")
            fl_on, fl_off = get("on", "motion_field", "flip_rate_output"), get("off", "motion_field", "flip_rate_output")
            sg_on, sg_off = get("on", "temporal_sigma_added"), get("off", "temporal_sigma_added")
            ps_on, ps_off = get("on", "psnr_mean"), get("off", "psnr_mean")
            print(f"{clip:18s} {fm_on:9.5f} {fm_off:9.5f} {fm_on - fm_off:+9.5f} {100 * (fm_on - fm_off) / fm_off:+7.2f} | "
                  f"{fl_on:8.5f} {fl_off:8.5f} {fl_on - fl_off:+9.5f} | {sg_on:8.4f} {sg_off:8.4f} {sg_on - sg_off:+8.4f} | "
                  f"{ps_on:8.3f} {ps_off:8.3f}")
        print(f"\n{'clip':18s} {'flick+ on':>10s} {'flick+ off':>10s} {'delta':>9s} | {'flips+ on':>10s} "
              f"{'flips+ off':>10s} {'delta':>9s} | {'flip src':>9s} | {'ssim on':>8s} {'ssim off':>8s} {'delta':>9s} | "
              f"{'sig src':>8s} {'sig out on':>10s} {'sig out off':>11s}")
        for clip in CLIPS:
            get = lambda tag, *p: median_of_identical_repeats(arms[tag], clip, PROFILE, *p)
            fk_on, fk_off = get("on", "flicker_added"), get("off", "flicker_added")
            fa_on, fa_off = get("on", "motion_field", "flip_rate_added"), get("off", "motion_field", "flip_rate_added")
            src = get("on", "motion_field", "flip_rate_source")
            ss_on, ss_off = get("on", "ssim_mean"), get("off", "ssim_mean")
            sigsrc = get("on", "temporal_sigma_source")
            so_on, so_off = get("on", "temporal_sigma_output"), get("off", "temporal_sigma_output")
            print(f"{clip:18s} {fk_on:10.4f} {fk_off:10.4f} {fk_on - fk_off:+9.4f} | {fa_on:10.5f} {fa_off:10.5f} "
                  f"{fa_on - fa_off:+9.5f} | {src:9.5f} | {ss_on:8.5f} {ss_off:8.5f} {ss_on - ss_off:+9.5f} | "
                  f"{sigsrc:8.4f} {so_on:10.4f} {so_off:11.4f}")
        print(f"\n{'clip':18s} {'floor':>9s} {'fm on':>9s} {'fm off':>9s} {'NR on':>9s} {'NR off':>9s} "
              f"{'gate removes':>13s}")
        for clip in CLIPS:
            floor = median_of_identical_repeats(arms["on"], clip, CONTROL, "motion_field", "false_motion_rate")
            fm_on = median_of_identical_repeats(arms["on"], clip, PROFILE, "motion_field", "false_motion_rate")
            fm_off = median_of_identical_repeats(arms["off"], clip, PROFILE, "motion_field", "false_motion_rate")
            nr_on, nr_off = fm_on - floor, fm_off - floor
            print(f"{clip:18s} {floor:9.5f} {fm_on:9.5f} {fm_off:9.5f} {nr_on:+9.5f} {nr_off:+9.5f} "
                  f"{100 * (1 - nr_on / nr_off):+12.1f} %")

    if "cutdist" in args.blocks:
        print("\n## cutdist")

        def cell_sequence(path: Path, clip: dict):
            grid = GuideGrid(clip["width"], clip["height"], clip["fps"])
            reader = FrameReader(path, clip["width"], clip["height"])
            try:
                return [grid.cells(frame) for frame in reader]
            finally:
                reader.close()

        for name in [c for c in CLIPS if clips[c]["cuts"]]:
            clip = clips[name]
            cuts = set(clip["cuts"])
            source = cell_sequence(args.corpus / clip["file"], clip)
            outputs = {tag: cell_sequence(runs / f"{name}__{PROFILE}__1" / "output.mkv", clip)
                       for tag, runs in arms.items()}
            print(f"{name} ({len(clip['cuts'])} labelled cuts)")
            for distance in (0, 3, 6):
                on, pairs = false_motion(source, outputs["on"], cuts, distance)
                off, _ = false_motion(source, outputs["off"], cuts, distance)
                print(f"   >= {distance} frames from a cut: pairs={pairs:3d} on={on:.5f} off={off:.5f} "
                      f"delta={on - off:+.5f} rel={100 * (on - off) / off:+.2f} %")

    if "depthoff" in args.blocks:
        # mv=1,depth=0 pins the depth channel to a uniform 0.75, so the CPU estimator's
        # flow field reaches the output on no frame where NVOFA produced one: what is left
        # between the arms is the resolve pass's gate alone.
        print("\n## depthoff")
        for clip in DEPTH_OFF_CLIPS:
            get = lambda tag, *p: median_of_identical_repeats(arms[tag], clip, DEPTH_OFF, *p)
            fm_on, fm_off = get("on", "motion_field", "false_motion_rate"), get("off", "motion_field", "false_motion_rate")
            fl_on, fl_off = get("on", "motion_field", "flip_rate_output"), get("off", "motion_field", "flip_rate_output")
            sg_on, sg_off = get("on", "temporal_sigma_added"), get("off", "temporal_sigma_added")
            hs = {tag: [frame_hashes(args.ffmpeg, runs / f"{clip}__{DEPTH_OFF}__{rep}" / "output.mkv")
                        for rep in (1, 2)] for tag, runs in arms.items()}
            digest = lambda h: hashlib.sha256("\n".join(h).encode()).hexdigest()[:16]
            differ = sum(1 for a, b in zip(hs["on"][0], hs["off"][0]) if a != b)
            print(f"{clip:18s} fm on={fm_on:.5f} off={fm_off:.5f} delta={fm_on - fm_off:+.5f} "
                  f"rel={100 * (fm_on - fm_off) / fm_off:+.2f} % | flip {fl_on:.5f}/{fl_off:.5f} "
                  f"delta={fl_on - fl_off:+.5f} | sig+ {sg_on:.4f}/{sg_off:.4f} delta={sg_on - sg_off:+.4f}")
            print(f"{'':18s} digest on={digest(hs['on'][0])} (repeats identical: {hs['on'][0] == hs['on'][1]}) "
                  f"off={digest(hs['off'][0])} (repeats identical: {hs['off'][0] == hs['off'][1]}) "
                  f"frames differing={differ}/{len(hs['on'][0])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
