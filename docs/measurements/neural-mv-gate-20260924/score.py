"""Scores the zero-motion-test A/B (gate.profile.json) on top of analyze.py.

analyze.py has already written metrics.json beside every run (flicker+, sigma+,
false mv, dE, PSNR, SSIM). This adds what the A/B needs and analyze.py does not
carry: sharpness against the source over time, luma PSNR at the start and end of
each clip (a held frame that decays shows there), the worker's own warp error, and
how far the two arms' outputs are from each other. It prints the report's tables.

    set DLSS_BENCHMARK_BUILD=... & set DLSS_BENCHMARK_FFMPEG=...
    python docs/measurements/neural-mv-gate-20260924/score.py
"""
from __future__ import annotations

import json
import statistics
import sys
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools" / "benchmark"))
from common import CORPUS, RUNS, FrameReader, load_manifest  # noqa: E402

PAIRS = [("gate-off", "gate-on", "governor 0"), ("shipped-gate-off", "shipped-gate-on", "shipped")]
EDGE = 10  # frames averaged at each end of a clip


def luma(rgb: np.ndarray) -> np.ndarray:
    return cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY).astype(np.float32)


def sharpness(y: np.ndarray) -> float:
    # Mean absolute Laplacian: high-frequency energy. A history re-sampled by a
    # sub-pixel field every frame loses it; a sharpening model gains it.
    return float(np.abs(cv2.Laplacian(y, cv2.CV_32F, ksize=3)).mean())


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)


def score_run(run: Path, clip: dict) -> dict:
    cached = run / "mvgate.json"
    if cached.exists():
        return json.loads(cached.read_text(encoding="utf-8"))
    result = json.loads((run / "result.json").read_text(encoding="utf-8"))
    w, h = clip["width"], clip["height"]
    sharp, psnrs = [], []
    for s, o in zip(FrameReader(Path(result["source"]), w, h), FrameReader(run / "output.mkv", w, h)):
        sy, oy = luma(s), luma(o)
        sharp.append(sharpness(oy) / max(sharpness(sy), 1e-6))
        psnrs.append(psnr(sy, oy))
    worker = {}
    for attempt in result["pass_results"][-1]["attempts"]:
        for record in attempt["records"]:
            if record["kind"] == "metrics":
                worker = record
    metrics = json.loads((run / "metrics.json").read_text(encoding="utf-8"))
    row = dict(
        run=run.name, frames=len(psnrs), digest=metrics.get("output_digest"),
        sharp=statistics.fmean(sharp), sharp_first=statistics.fmean(sharp[:EDGE]),
        sharp_last=statistics.fmean(sharp[-EDGE:]), psnr_y=statistics.fmean(psnrs),
        psnr_y_first=statistics.fmean(psnrs[:EDGE]), psnr_y_last=statistics.fmean(psnrs[-EDGE:]),
        flicker_added=metrics.get("flicker_added"), sigma_added=metrics.get("temporal_sigma_added"),
        sigma_p99_output=metrics.get("temporal_sigma_p99_output"),
        false_mv=(metrics.get("motion_field") or {}).get("false_motion_rate"),
        delta_e=metrics.get("delta_e_mean"), psnr=metrics.get("psnr_mean"), ssim=metrics.get("ssim_mean"),
        warp_added=worker.get("flicker_added"), warp_output=worker.get("output_warp_error"),
        worker_color_delta=worker.get("color_delta"),
        gpu_ms_p50=result["timing"].get("neural_gpu_ms_p50"), guide_ms=result["timing"].get("guide_ms_mean"))
    cached.write_text(json.dumps(row, indent=2), encoding="utf-8")
    return row


def difference(a: Path, b: Path, clip: dict) -> tuple[float, float]:
    """Mean absolute byte difference and share of bytes that differ, over every frame."""
    w, h = clip["width"], clip["height"]
    total = changed = 0.0
    count = 0
    for x, y in zip(FrameReader(a / "output.mkv", w, h), FrameReader(b / "output.mkv", w, h)):
        d = np.abs(x.astype(np.int16) - y.astype(np.int16))
        total += float(d.mean())
        changed += float((d > 0).mean())
        count += 1
    return total / count, changed / count


def fmt(v, digits=3):
    return "-" if v is None else f"{v:.{digits}f}"


def main() -> int:
    clips = {c["name"]: c for c in load_manifest(CORPUS)["clips"]}
    runs = {p.name: p for p in RUNS.iterdir() if (p / "metrics.json").exists()}
    rows = {name: score_run(path, clips[name.split("__")[0]]) for name, path in sorted(runs.items())}
    print("| clip | arms | digest rep1=rep2 (off/on) | bytes differing off vs on | sharp off -> on | sharp last10 off -> on "
          "| PSNR-Y first10 -> last10 off | on | flicker+ off -> on | sigma+ off -> on | false mv off -> on "
          "| warp+ off -> on | dE off -> on | PSNR off -> on | SSIM off -> on |")
    print("|" + "---|" * 15)
    for off, on, label in PAIRS:
        for clip in clips:
            a, b = rows.get(f"{clip}__{off}__1"), rows.get(f"{clip}__{on}__1")
            if not a or not b:
                continue
            a2, b2 = rows.get(f"{clip}__{off}__2"), rows.get(f"{clip}__{on}__2")
            det = "/".join("-" if not r2 else ("yes" if r1["digest"] == r2["digest"] else "no")
                           for r1, r2 in ((a, a2), (b, b2)))
            mad, share = difference(runs[a["run"]], runs[b["run"]], clips[clip])
            print(f"| {clip} | {label} | {det} "
                  f"| {share * 100:.1f} % (mean {mad:.2f}) "
                  f"| {fmt(a['sharp'])} -> {fmt(b['sharp'])} | {fmt(a['sharp_last'])} -> {fmt(b['sharp_last'])} "
                  f"| {fmt(a['psnr_y_first'], 2)} -> {fmt(a['psnr_y_last'], 2)} "
                  f"| {fmt(b['psnr_y_first'], 2)} -> {fmt(b['psnr_y_last'], 2)} "
                  f"| {fmt(a['flicker_added'])} -> {fmt(b['flicker_added'])} "
                  f"| {fmt(a['sigma_added'])} -> {fmt(b['sigma_added'])} "
                  f"| {fmt(a['false_mv'], 4)} -> {fmt(b['false_mv'], 4)} "
                  f"| {fmt(a['warp_added'])} -> {fmt(b['warp_added'])} "
                  f"| {fmt(a['delta_e'], 2)} -> {fmt(b['delta_e'], 2)} "
                  f"| {fmt(a['psnr'], 2)} -> {fmt(b['psnr'], 2)} | {fmt(a['ssim'], 4)} -> {fmt(b['ssim'], 4)} |")
    # The still's exact answer: with nothing moving, the test must hand NGX the same
    # all-zero field the motion guide switched off does.
    for arm in ("gate-off", "gate-on"):
        r, z = rows.get(f"still-hold__{arm}__1"), rows.get("still-hold__mv-zero__1")
        if r and z:
            print(f"still-hold {arm} vs mv-zero: {'byte-identical' if r['digest'] == z['digest'] else 'different'}")
    (RUNS.parent / "mvgate-rows.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
