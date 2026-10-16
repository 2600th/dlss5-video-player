"""Scores every run under benchmark-work/runs against its lossless source.

Per run (all frames decoded to rgb24 through FFmpeg, streamed, never cached in RAM):
  determinism   sha256 over the rgb24 framemd5 sequence; identical across repeats
                of the same clip/profile => deterministic rerender
  flicker       mean |Y_t - Y_(t-1)| of the output minus the same statistic of
                the source, excluding manifest cut frames (temporal instability
                the neural pass added; negative = smoother than source)
  color shift   mean CIE76 dE in CIELAB (cv2 BGR->Lab) and mean per-channel
                (R,G,B) delta output-source over sampled frames
  fidelity      PSNR (RGB, 8 bit) and grayscale SSIM (Gaussian 11x11, sigma 1.5)
  OCR           rapidocr on sampled frames of text clips; character-level
                SequenceMatcher ratio against the manifest strings active at
                that timestamp, source and output side by side
  faces         cv2 Haar frontal+profile boxes on the source; resnet18
                (ImageNet, penultimate layer) cosine similarity of the output
                crop vs the source crop, and frame-to-frame drift of the
                output-crop embedding versus the source's own drift
  sigma         per-pixel temporal standard deviation of luma inside a shot
                (the frames between manifest cuts), meaned over pixels, output
                and source side by side: the localized shimmer a frame-global
                mean averages away
  motion field  on the guide generator's own analysis grid, the fraction of
                cells the source held static that the output moved anyway
                (motion the pass invented) and the fraction of cells whose
                moving/static verdict flips between consecutive pairs
  cuts          precision/recall/F1 of the generator's own cut test against the
                manifest hard-cut indices at +/-1 frame, on both streams
  two-pass      metric deltas of pass-2 profiles versus their single-pass
                counterpart on the same clip

Opt-in (P1.42; the default run's output is unchanged without them):
  --lost-change the frozen-output guard paired with flicker+/sigma+/false mv: per
                consecutive pair, the change the output added where the source did not
                change, and the share of the source's change the output lost (source
                moved, output did not), per pixel and per guide-grid cell
  --vmaf-v1     VMAF v1 (chroma, built-in CAMBI, 4K and HFR models; pinned, fetched and
                hash-verified by quality.py) plus v0.6.1 in the same pass
  --cvvdp       ColorVideoVDP JOD on the GPU, overall and per channel (transient = visible
                temporal difference, rg/yv = chroma)
  --yardstick   all three

--sample-every strides the dE/PSNR/SSIM/OCR/face block alone. Sigma, the motion field
and the cut test always see every consecutive pair, and all three are withheld whole
when the output frame count does not match the source, because frame i of one file is
then not frame i of the other.

Writes analysis/analysis.json, analysis/report.md and per-run metrics.json.

    python tools/benchmark/analyze.py [--corpus DIR] [--runs DIR] [--sample-every N] [--no-ocr] [--no-faces]
                                      [--match GLOB] [--lost-change] [--vmaf-v1] [--cvvdp] [--yardstick]
"""
from __future__ import annotations

import argparse
import difflib
import fnmatch
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import cv2
import numpy as np

from common import (ANALYSIS, CORPUS, FrameReader, RUNS, framemd5, load_manifest, run_dirs, sequence_digest,
                    write_json)
from cutmirror import CUT_MATCH_FRAMES, CutRun, GuideGrid, cut_scores

SAMPLE_EVERY = 15

# The grid metrics mirror src/TemporalGuides.cpp instead of inventing a geometry: the
# analysis grid, the stratified downsample, the normalized Rec.709 luma, the cut
# thresholds and the weak-arm debounce all come from cutmirror, which is the
# generator's own. A cell here is the cell the worker solves a vector for, so a
# threshold swept there transfers unchanged.
CELL_TOLERANCE = 2 / 255  # "unchanged": two 8-bit levels of a cell's normalized luma
MIN_SHOT_FRAMES = 3
# The pixel half of the frozen-output guard uses the same two levels, in 8-bit luma, as
# the line between "the source changed here" and noise; a pair where less than this share
# of the source's pixels changed is a held/duplicate source frame and is skipped whole,
# so a motionless stretch cannot drive the lost-change ratio through a near-zero denominator.
PIXEL_TOLERANCE = 2.0
STILL_PAIR_SHARE = 0.001


def ssim_gray(a: np.ndarray, b: np.ndarray) -> float:
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    a, b = a.astype(np.float64), b.astype(np.float64)
    mu_a, mu_b = cv2.GaussianBlur(a, (11, 11), 1.5), cv2.GaussianBlur(b, (11, 11), 1.5)
    s_aa = cv2.GaussianBlur(a * a, (11, 11), 1.5) - mu_a * mu_a
    s_bb = cv2.GaussianBlur(b * b, (11, 11), 1.5) - mu_b * mu_b
    s_ab = cv2.GaussianBlur(a * b, (11, 11), 1.5) - mu_a * mu_b
    num = (2 * mu_a * mu_b + c1) * (2 * s_ab + c2)
    den = (mu_a ** 2 + mu_b ** 2 + c1) * (s_aa + s_bb + c2)
    return float((num / den).mean())


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))
    return 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)


def luma(rgb: np.ndarray) -> np.ndarray:
    return cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY).astype(np.float32)


class ShotSigma:
    """Per-pixel temporal standard deviation of luma, accumulated one shot at a time.

    A shot is the run of frames between manifest cuts: across a cut a pixel's spread is the
    edit and not the pass, so the sums are flushed at every cut and never span one. Only a
    running sum and sum of squares are held, in float64 because at float32 a shot's variance
    disappears into the rounding of E[x^2] - E[x]^2.
    """

    def __init__(self):
        self.total = self.squares = None
        self.frames = 0
        self.shots: list[tuple[int, float, float]] = []

    def add(self, y: np.ndarray) -> None:
        if self.total is None:
            self.total, self.squares = np.zeros(y.shape, np.float64), np.zeros(y.shape, np.float64)
        self.total += y
        self.squares += np.square(y, dtype=np.float64)
        self.frames += 1

    def cut(self) -> None:
        """Ends the current shot. Shots too short for a standard deviation to mean anything
        are dropped rather than reported as suspiciously low sigma."""
        if self.frames >= MIN_SHOT_FRAMES:
            mean = self.total / self.frames
            sigma = np.sqrt(np.maximum(self.squares / self.frames - mean * mean, 0.0))
            self.shots.append((self.frames, float(sigma.mean()), float(np.percentile(sigma, 99))))
        if self.total is not None:
            self.total.fill(0.0)
            self.squares.fill(0.0)
        self.frames = 0

    def result(self) -> tuple[float, float] | None:
        """Frame-weighted mean over shots of the sigma map's mean and of its p99."""
        self.cut()
        if not self.shots:
            return None
        weight = sum(n for n, _, _ in self.shots)
        return (sum(n * mean for n, mean, _ in self.shots) / weight,
                sum(n * p99 for n, _, p99 in self.shots) / weight)


class MotionField:
    """Per-cell motion verdicts of the output against the source, on the guide grid.

    A cell is moving when its cell luma changed by more than CELL_TOLERANCE across a
    consecutive frame pair. Two numbers come out of that which a frame-global mean cannot
    produce: the fraction of the cells the source held static that the output moved anyway,
    which is motion the pass invented rather than carried, and the fraction of cells whose
    verdict changes from one pair to the next, which is the instability of the motion field
    itself rather than of the pixels - a threshold only becomes visible once it oscillates.
    Pairs that span a manifest cut are excluded from both; the cut test is not, because
    those pairs are what it exists to find.
    """

    def __init__(self, width: int, height: int, fps: float, cuts: set[int], soft_cuts=()):
        self.grid = GuideGrid(width, height, fps)
        self.cuts = cuts
        self.soft_cuts = [tuple(span) for span in soft_cuts or ()]
        self.detect_source, self.detect_output = CutRun(fps), CutRun(fps)
        self.static = self.invented = self.pairs = 0
        self.moving_source = self.lost = 0
        self.flips_source = self.flips_output = self.transitions = 0
        self.previous = self.verdicts = None

    def add(self, index: int, source: np.ndarray, output: np.ndarray) -> None:
        cells = (self.grid.cells(source), self.grid.cells(output))
        if self.previous is None:
            self.previous = cells
            return
        self.detect_source.feed(index, cells[0], self.previous[0])
        self.detect_output.feed(index, cells[1], self.previous[1])
        if index in self.cuts:
            self.previous, self.verdicts = cells, None
            return
        moving = tuple(np.abs(now - was) > CELL_TOLERANCE for now, was in zip(cells, self.previous))
        static = ~moving[0]
        self.static += int(static.sum())
        self.invented += int((static & moving[1]).sum())
        if not any(first < index <= last for first, last in self.soft_cuts):
            self.moving_source += int(moving[0].sum())
            self.lost += int((moving[0] & ~moving[1]).sum())
        self.pairs += 1
        if self.verdicts is not None:
            self.flips_source += int((moving[0] != self.verdicts[0]).sum())
            self.flips_output += int((moving[1] != self.verdicts[1]).sum())
            self.transitions += 1
        self.previous, self.verdicts = cells, moving

    def result(self) -> dict:
        cells = self.grid.gw * self.grid.gh
        rate = lambda flips: flips / (self.transitions * cells) if self.transitions else None
        return dict(grid=[self.grid.gw, self.grid.gh],
                    cell_pixels=[round(self.grid.cell_w, 2), round(self.grid.cell_h, 2)],
                    tolerance=CELL_TOLERANCE, pairs=self.pairs, cells=cells, static_cells=self.static,
                    invented_cells=self.invented,
                    false_motion_rate=self.invented / self.static if self.static else None,
                    transitions=self.transitions, flip_rate_source=rate(self.flips_source),
                    flip_rate_output=rate(self.flips_output),
                    flip_rate_added=rate(self.flips_output - self.flips_source))

    def lost_result(self) -> dict:
        """The mirror of false_motion_rate: of the cells the source moved, the fraction the
        output held static. A frozen or over-smoothed render scores high here while its
        false mv and flips+ look perfect. Pairs inside soft-cut spans are left out too."""
        return dict(moving_cells=self.moving_source, lost_cells=self.lost,
                    lost_motion_rate=self.lost / self.moving_source if self.moving_source else None)


class ChangeBalance:
    """flicker+ split into its two halves, per pixel, so they cannot cancel.

    flicker+ is mean |dY_out| - mean |dY_src|: a render that adds shimmer and one that stops
    following the source move it in opposite directions, and a frozen render scores as
    "smoother than the source". Per consecutive pair (manifest cuts, soft-cut spans and
    held/duplicate source pairs excluded), on 8-bit luma:

      added  mean over all pixels of max(|dY_out| - |dY_src|, 0): change the output has that
             the source does not, in luma levels per pair
      lost   over the pixels where the source changed by more than PIXEL_TOLERANCE, the sum
             of max(|dY_src| - |dY_out|, 0) over the sum of |dY_src|: the share of the
             source's own change the output did not carry. 0 = every change carried,
             1 = frozen. Pooled over pairs (a ratio of sums, never a mean of per-pair ratios)
    """

    def __init__(self, cuts: set[int], soft_cuts=()):
        self.cuts, self.soft_cuts = cuts, [tuple(span) for span in soft_cuts or ()]
        self.pairs = self.still_pairs = 0
        self.added = self.lost = self.source_change = 0.0

    def add(self, index: int, ds: np.ndarray, do: np.ndarray) -> None:
        """ds, do: |Y_t - Y_(t-1)| of source and output for the pair ending at index."""
        if index in self.cuts or any(first < index <= last for first, last in self.soft_cuts):
            return
        moving = ds > PIXEL_TOLERANCE
        if moving.mean() < STILL_PAIR_SHARE:
            self.still_pairs += 1
            return
        self.pairs += 1
        gap = do - ds
        self.added += float(np.maximum(gap, 0).mean())
        self.lost += float(np.maximum(-gap[moving], 0).sum(dtype=np.float64))
        self.source_change += float(ds[moving].sum(dtype=np.float64))

    def result(self) -> dict:
        return dict(pairs=self.pairs, still_pairs_skipped=self.still_pairs, tolerance=PIXEL_TOLERANCE,
                    added=self.added / self.pairs if self.pairs else None,
                    lost_share=self.lost / self.source_change if self.source_change else None)


def delta_e(a: np.ndarray, b: np.ndarray) -> float:
    la = cv2.cvtColor(a, cv2.COLOR_RGB2LAB).astype(np.float32)
    lb = cv2.cvtColor(b, cv2.COLOR_RGB2LAB).astype(np.float32)
    # OpenCV 8-bit Lab: L*255/100, a+128, b+128 -> rescale to CIE units.
    la[..., 0] *= 100 / 255
    lb[..., 0] *= 100 / 255
    return float(np.sqrt(((la - lb) ** 2).sum(-1)).mean())


class Ocr:
    def __init__(self):
        from rapidocr_onnxruntime import RapidOCR
        self.engine = RapidOCR()

    def read(self, rgb: np.ndarray) -> str:
        result, _ = self.engine(cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR))
        return " ".join(item[1] for item in result or [])


def text_ratio(expected: list[str], recognized: str) -> float:
    """Character-level similarity of the concatenated expected strings to the OCR output."""
    want = " ".join(expected).lower()
    got = recognized.lower()
    if not want:
        return 1.0
    matcher = difflib.SequenceMatcher(None, want, got, autojunk=False)
    matched = sum(block.size for block in matcher.get_matching_blocks())
    return matched / len(want)


class FaceEmbedder:
    def __init__(self):
        import torch
        import torchvision
        self.torch = torch
        weights = torchvision.models.ResNet18_Weights.IMAGENET1K_V1
        model = torchvision.models.resnet18(weights=weights)
        model.fc = torch.nn.Identity()
        self.device = "cuda" if torch.cuda.is_available() else "cpu"
        self.model = model.eval().to(self.device)
        self.mean = torch.tensor([0.485, 0.456, 0.406], device=self.device).view(1, 3, 1, 1)
        self.std = torch.tensor([0.229, 0.224, 0.225], device=self.device).view(1, 3, 1, 1)
        base = cv2.data.haarcascades
        self.frontal = cv2.CascadeClassifier(base + "haarcascade_frontalface_default.xml")
        self.profile = cv2.CascadeClassifier(base + "haarcascade_profileface.xml")

    def boxes(self, rgb: np.ndarray) -> list[tuple[int, int, int, int, str]]:
        gray = cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY)
        found = [(*b, "frontal") for b in self.frontal.detectMultiScale(gray, 1.15, 6, minSize=(72, 72))]
        found += [(*b, "profile") for b in self.profile.detectMultiScale(gray, 1.15, 6, minSize=(72, 72))]
        return sorted(found, key=lambda b: -b[2] * b[3])[:2]

    def embed(self, crops: list[np.ndarray]) -> np.ndarray:
        with self.torch.no_grad():
            batch = np.stack([cv2.resize(c, (224, 224), interpolation=cv2.INTER_AREA) for c in crops])
            tensor = self.torch.from_numpy(batch).to(self.device).permute(0, 3, 1, 2).float() / 255
            features = self.model((tensor - self.mean) / self.std)
            features = features / features.norm(dim=1, keepdim=True)
            return features.cpu().numpy()


def crop(rgb: np.ndarray, box) -> np.ndarray:
    x, y, w, h, _ = box
    pad = int(0.15 * w)
    return rgb[max(0, y - pad):y + h + pad, max(0, x - pad):x + w + pad]


def active_text(clip: dict, seconds: float) -> list[str]:
    return [t["text"] for t in clip["text"] if t["start_s"] <= seconds < t["end_s"]]


def analyze_run(run: Path, clip: dict, ocr: Ocr | None, faces: FaceEmbedder | None, sample_every: int,
                lost_change: bool = False, vmaf: bool = False, cvvdp: bool = False) -> dict:
    result = json.loads((run / "result.json").read_text(encoding="utf-8"))
    metrics = dict(run=run.name, clip=result["clip"], category=result["category"], profile=result["profile"],
                   repeat=result["repeat"], passes=result["passes"], ok=bool(result["result"].get("ok")),
                   failure=result["result"].get("failure"), encoder=result["result"].get("encoder"),
                   wall_seconds=result["wall_seconds"], end_to_end_fps=result["end_to_end_fps"],
                   processing_fps=result["processing_fps"], timing=result["timing"], nvml=result["nvml"],
                   history_resets=result["result"].get("history_resets"),
                   frame_retries=result["result"].get("frame_retries"),
                   pass2_reencoded_input=result.get("pass2_reencoded_input", False),
                   guides=result.get("guides", ""))
    if not metrics["ok"]:
        return metrics
    # The render always writes output.mkv beside result.json, so prefer the path
    # relative to this run directory and fall back to the absolute one recorded at
    # render time. Moving a run between trees then just works, while a genuinely
    # missing artifact still raises from FrameReader rather than scoring nothing.
    beside = run / "output.mkv"
    output = beside if beside.exists() else Path(result["output"])
    hashes = framemd5(output, run / "frames.md5")
    metrics.update(output_frames=len(hashes), output_digest=sequence_digest(hashes),
                   unique_output_frames=len(set(hashes)),
                   frame_count_matches_source=len(hashes) == clip["frames"])
    w, h, cuts = clip["width"], clip["height"], set(clip["cuts"])
    src, out = FrameReader(Path(result["source"]), w, h), FrameReader(output, w, h)
    flick_src, flick_out, de, rgb_delta, psnrs, ssims = [], [], [], [], [], []
    ocr_rows, face_rows = [], []
    # The pair metrics ignore sample_every on purpose: a stride would measure a different
    # clip's worth of motion under the same name. Sampling stays inside the block below.
    field = MotionField(w, h, clip["fps"], cuts, clip.get("soft_cuts"))
    balance = ChangeBalance(cuts, clip.get("soft_cuts")) if lost_change else None
    sigma_src, sigma_out = ShotSigma(), ShotSigma()
    prev_src_y = prev_out_y = None
    prev_src_emb = prev_out_emb = None
    index = -1
    for index, (s, o) in enumerate(zip(src, out)):
        sy, oy = luma(s), luma(o)
        if index in cuts:
            sigma_src.cut()
            sigma_out.cut()
        sigma_src.add(sy)
        sigma_out.add(oy)
        field.add(index, s, o)
        if prev_src_y is not None and index not in cuts:
            ds, do = np.abs(sy - prev_src_y), np.abs(oy - prev_out_y)
            flick_src.append(float(ds.mean()))
            flick_out.append(float(do.mean()))
            if balance:
                balance.add(index, ds, do)
        prev_src_y, prev_out_y = sy, oy
        if index % sample_every == 0:
            de.append(delta_e(s, o))
            rgb_delta.append((o.astype(np.float32) - s.astype(np.float32)).reshape(-1, 3).mean(0))
            psnrs.append(psnr(s, o))
            ssims.append(ssim_gray(luma(s), luma(o)))
            seconds = index / clip["fps"]
            if ocr and clip["text"]:
                expected = active_text(clip, seconds)
                got_src, got_out = ocr.read(s), ocr.read(o)
                ocr_rows.append(dict(frame=index, expected=expected, source_text=got_src, output_text=got_out,
                                     source_ratio=text_ratio(expected, got_src),
                                     output_ratio=text_ratio(expected, got_out)))
            if faces and clip["category"] == "faces":
                boxes = faces.boxes(s)
                if boxes:
                    emb = faces.embed([crop(s, b) for b in boxes] + [crop(o, b) for b in boxes])
                    n = len(boxes)
                    sims = [float(emb[i] @ emb[n + i]) for i in range(n)]
                    row = dict(frame=index, faces=[b[4] for b in boxes], source_vs_output_cosine=sims)
                    if prev_src_emb is not None:
                        row["source_drift"] = 1 - float(emb[0] @ prev_src_emb)
                        row["output_drift"] = 1 - float(emb[n] @ prev_out_emb)
                    prev_src_emb, prev_out_emb = emb[0], emb[n]
                    face_rows.append(row)
    src.close()
    out.close()
    if index < 0:
        raise RuntimeError(f"{run.name}: compared no frame pairs - source {result['source']} "
                           f"and output {output} did not both yield frames")
    rgb = np.mean(rgb_delta, axis=0) if rgb_delta else np.zeros(3)
    metrics.update(
        compared_frames=index + 1,
        flicker_source=statistics.fmean(flick_src) if flick_src else None,
        flicker_output=statistics.fmean(flick_out) if flick_out else None,
        flicker_added=(statistics.fmean(flick_out) - statistics.fmean(flick_src)) if flick_src else None,
        flicker_p95_output=float(np.percentile(flick_out, 95)) if flick_out else None,
        delta_e_mean=statistics.fmean(de) if de else None, delta_e_max=max(de) if de else None,
        rgb_shift=dict(r=float(rgb[0]), g=float(rgb[1]), b=float(rgb[2])),
        psnr_mean=statistics.fmean(psnrs) if psnrs else None, psnr_min=min(psnrs) if psnrs else None,
        ssim_mean=statistics.fmean(ssims) if ssims else None, ssim_min=min(ssims) if ssims else None,
        sampled_frames=len(psnrs))
    sigma_source, sigma_output = sigma_src.result(), sigma_out.result()
    if not metrics["frame_count_matches_source"]:
        metrics.update(temporal_sigma_source=None, temporal_sigma_output=None, temporal_sigma_added=None,
                       temporal_sigma_p99_source=None, temporal_sigma_p99_output=None, temporal_sigma_shots=0,
                       motion_field=None, cuts=None,
                       temporal_withheld=f"output has {len(hashes)} frames against the source's "
                                         f"{clip['frames']}, so consecutive pairs are not aligned")
    else:
        worker = result["result"]
        metrics.update(
            temporal_sigma_source=sigma_source[0] if sigma_source else None,
            temporal_sigma_output=sigma_output[0] if sigma_output else None,
            temporal_sigma_added=(sigma_output[0] - sigma_source[0]) if sigma_source and sigma_output else None,
            temporal_sigma_p99_source=sigma_source[1] if sigma_source else None,
            temporal_sigma_p99_output=sigma_output[1] if sigma_output else None,
            temporal_sigma_shots=len(sigma_src.shots), motion_field=field.result(),
            cuts=dict(ground_truth=sorted(cuts), tolerance_frames=CUT_MATCH_FRAMES,
                      tolerated_spans=clip.get("soft_cuts", []),
                      source=cut_scores(field.detect_source.cuts, sorted(cuts), soft_cuts=clip.get("soft_cuts")),
                      output=cut_scores(field.detect_output.cuts, sorted(cuts), soft_cuts=clip.get("soft_cuts")),
                      source_evidence=field.detect_source.evidence,
                      output_evidence=field.detect_output.evidence,
                      worker_accepted_strong=worker.get("accepted_strong_cuts"),
                      worker_accepted_weak=worker.get("accepted_weak_cuts"),
                      worker_suppressed=worker.get("suppressed_cuts")))
    if ocr_rows:
        metrics.update(ocr=dict(frames=ocr_rows,
                                source_ratio_mean=statistics.fmean(r["source_ratio"] for r in ocr_rows),
                                output_ratio_mean=statistics.fmean(r["output_ratio"] for r in ocr_rows)))
    if face_rows:
        sims = [s for r in face_rows for s in r["source_vs_output_cosine"]]
        drift_s = [r["source_drift"] for r in face_rows if "source_drift" in r]
        drift_o = [r["output_drift"] for r in face_rows if "output_drift" in r]
        metrics.update(faces=dict(frames=face_rows, sampled=len(face_rows),
                                  source_vs_output_cosine_mean=statistics.fmean(sims),
                                  source_vs_output_cosine_min=min(sims),
                                  source_drift_mean=statistics.fmean(drift_s) if drift_s else None,
                                  output_drift_mean=statistics.fmean(drift_o) if drift_o else None))
    if balance:
        metrics["change"] = dict(balance.result(), **(field.lost_result() if metrics["frame_count_matches_source"]
                                                      else dict(moving_cells=None, lost_cells=None,
                                                                lost_motion_rate=None)))
    perceptual(metrics, Path(result["source"]), output, clip, vmaf, cvvdp)
    write_json(run / "metrics.json", metrics)
    return metrics


def perceptual(metrics: dict, source: Path, output: Path, clip: dict, vmaf: bool, cvvdp: bool) -> None:
    """Full-reference perceptual scores (quality.py), only on frame-aligned outputs."""
    if not (vmaf or cvvdp):
        return
    import quality
    if not metrics.get("frame_count_matches_source"):
        reason = "output frame count does not match the source, so frame i is not frame i"
        if vmaf:
            metrics["vmaf_v1"] = dict(withheld=reason)
        if cvvdp:
            metrics["cvvdp"] = dict(withheld=reason)
        return
    if vmaf:
        metrics["vmaf_v1"] = quality.vmaf_v1(source, output, clip["height"], clip["fps"])
    if cvvdp:
        metrics["cvvdp"] = quality.cvvdp(source, output, clip["width"], clip["height"], clip["frames"], clip["fps"])


def fmt(value, digits=3):
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def median_of(runs: list[dict], *path) -> float | None:
    """Median over repeats of a metric named by a path through possibly absent dicts."""
    values = []
    for run in runs:
        value = run
        for key in path:
            value = value.get(key) if isinstance(value, dict) else None
        if value is not None:
            values.append(value)
    return statistics.median(values) if values else None


def summarize(rows: list[dict]) -> tuple[list[dict], list[dict], list[dict]]:
    groups = defaultdict(list)
    for r in rows:
        if r["ok"]:
            groups[(r["clip"], r["profile"])].append(r)
    summary = []
    for (clip, prof), runs in sorted(groups.items()):
        med = lambda *path: median_of(runs, *path)
        row = dict(
            clip=clip, profile=prof, guides=runs[0].get("guides", ""), repeats=len(runs),
            passes=runs[0]["passes"],
            deterministic=len({r["output_digest"] for r in runs}) == 1 if len(runs) > 1 else None,
            wall_seconds=med("wall_seconds"), end_to_end_fps=med("end_to_end_fps"),
            processing_fps=med("processing_fps"),
            neural_gpu_ms_p50=med("timing", "neural_gpu_ms_p50"),
            neural_gpu_ms_p95=med("timing", "neural_gpu_ms_p95"),
            neural_gpu_ms_max=med("timing", "neural_gpu_ms_max"), guide_ms_mean=med("timing", "guide_ms_mean"),
            capture_ms_mean=med("timing", "capture_ms_mean"),
            peak_local_vram_mib=med("timing", "peak_local_vram_mib"),
            nvml_total_used_peak_mib=statistics.median(r["nvml"].get("nvml_total_used_peak_mib") or 0 for r in runs),
            flicker_added=med("flicker_added"), delta_e_mean=med("delta_e_mean"), psnr_mean=med("psnr_mean"),
            ssim_mean=med("ssim_mean"),
            temporal_sigma_source=med("temporal_sigma_source"),
            temporal_sigma_output=med("temporal_sigma_output"), temporal_sigma_added=med("temporal_sigma_added"),
            temporal_sigma_p99_output=med("temporal_sigma_p99_output"),
            false_motion_rate=med("motion_field", "false_motion_rate"),
            cell_flip_output=med("motion_field", "flip_rate_output"),
            cell_flip_added=med("motion_field", "flip_rate_added"),
            cut_f1_source=med("cuts", "source", "f1"), cut_f1_output=med("cuts", "output", "f1"),
            ocr_output=statistics.median(r["ocr"]["output_ratio_mean"] for r in runs) if runs[0].get("ocr") else None,
            ocr_source=statistics.median(r["ocr"]["source_ratio_mean"] for r in runs) if runs[0].get("ocr") else None,
            face_cosine=statistics.median(r["faces"]["source_vs_output_cosine_mean"] for r in runs)
            if runs[0].get("faces") else None,
            face_drift_output=statistics.median(r["faces"]["output_drift_mean"] or 0 for r in runs)
            if runs[0].get("faces") else None,
            history_resets=med("history_resets"))
        # The P1.42 columns exist only when a run carries them, so a default analysis
        # writes the same analysis.json it always did.
        if any(r.get("change") for r in runs):
            row.update(change_added=med("change", "added"), change_lost_share=med("change", "lost_share"),
                       lost_motion_rate=med("change", "lost_motion_rate"))
        if any("vmaf_v1_mean" in (r.get("vmaf_v1") or {}) for r in runs):
            row.update(vmaf_v1=med("vmaf_v1", "vmaf_v1_mean"), vmaf_v1_p5=med("vmaf_v1", "vmaf_v1_p5"),
                       vmaf_v061=med("vmaf_v1", "vmaf_v061_mean"), cambi_added=med("vmaf_v1", "cambi_added"),
                       speed_chroma_uv=med("vmaf_v1", "speed_chroma_uv"))
        if any("jod" in (r.get("cvvdp") or {}) for r in runs):
            row.update(jod=med("cvvdp", "jod"), jod_transient=med("cvvdp", "jod_channel", "transient"),
                       jod_rg=med("cvvdp", "jod_channel", "rg"), jod_yv=med("cvvdp", "jod_channel", "yv"))
        summary.append(row)
    by_key = {(s["clip"], s["profile"]): s for s in summary}
    keys = ("flicker_added", "temporal_sigma_added", "temporal_sigma_output", "false_motion_rate",
            "cell_flip_added", "cut_f1_output", "delta_e_mean", "psnr_mean", "ssim_mean", "ocr_output",
            "face_cosine", "end_to_end_fps")
    two_pass, guide_ab = [], []
    for s in summary:
        base = by_key.get((s["clip"], "baseline"))
        if not base or s["profile"] == "baseline":
            continue
        delta = {f"delta_{k}": (s[k] - base[k]) if s[k] is not None and base[k] is not None else None
                 for k in keys}
        row = dict(clip=s["clip"], profile=s["profile"], versus="baseline", **delta)
        if s["passes"] == 2:
            two_pass.append(row)
        elif s["guides"] != base["guides"]:
            guide_ab.append(dict(row, guides=s["guides"]))
    return summary, two_pass, guide_ab


def report(summary: list[dict], two_pass: list[dict], guide_ab: list[dict], rows: list[dict]) -> str:
    lines = ["# Neural benchmark report", "",
             "Per clip/profile medians over repeats. `flicker+` = output minus source mean |dY| "
             "(cut frames excluded); `sigma+` = output minus source per-pixel temporal standard deviation "
             "inside a shot, in 8-bit luma levels; `false mv` = fraction of the cells the source held static "
             "that the output moved anyway; `flips+` = output minus source fraction of cells whose "
             "moving/static verdict changes from one pair to the next; `cut F1` = the generator's own cut test "
             "on the output against the manifest's hard cuts at +/-1 frame; `dE` = mean CIE76 in CIELAB vs "
             "source; PSNR/SSIM vs the lossless source "
             "(low values on a strong relight are expected, not failures); OCR = char-level ratio vs manifest "
             "strings (output | source); VRAM = worker peak local budget (receipt) | NVML whole-GPU peak; "
             "wall s = all passes, e2e fps = final pass only.", "",
             "| clip | profile | n | det | wall s | e2e fps | proc fps | gpu ms p50/p95/max | guide ms | capture ms "
             "| VRAM MiB local|total | flicker+ | sigma+ | false mv | flips+ | cut F1 | dE | PSNR | SSIM "
             "| OCR out|src | face cos | resets |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for s in summary:
        lines.append("| " + " | ".join([
            s["clip"], s["profile"], str(s["repeats"]), fmt(s["deterministic"]), fmt(s["wall_seconds"], 1),
            fmt(s["end_to_end_fps"], 2),
            fmt(s["processing_fps"], 2),
            f"{fmt(s['neural_gpu_ms_p50'], 2)}/{fmt(s['neural_gpu_ms_p95'], 2)}/{fmt(s['neural_gpu_ms_max'], 2)}",
            fmt(s["guide_ms_mean"], 2), fmt(s["capture_ms_mean"], 2),
            f"{fmt(s['peak_local_vram_mib'], 0)}|{fmt(s['nvml_total_used_peak_mib'], 0)}",
            fmt(s["flicker_added"]), fmt(s["temporal_sigma_added"]), fmt(s["false_motion_rate"], 4),
            fmt(s["cell_flip_added"], 4), fmt(s["cut_f1_output"], 2),
            fmt(s["delta_e_mean"], 2), fmt(s["psnr_mean"], 2), fmt(s["ssim_mean"], 4),
            f"{fmt(s['ocr_output'])}|{fmt(s['ocr_source'])}" if s["ocr_output"] is not None else "-",
            fmt(s["face_cosine"]), fmt(s["history_resets"], 0)]) + " |")
    yard = [s for s in summary if any(k in s for k in ("change_added", "vmaf_v1", "jod"))]
    if yard:
        g = lambda s, k, d=3: fmt(s.get(k), d)
        lines += ["", "## Yardstick (P1.42)", "",
                  "Every temporal number next to its frozen-output guard. `added` = mean per pixel of "
                  "max(|dY_out| - |dY_src|, 0), luma levels per pair; `lost` = share of the source's own change "
                  "(pixels where it moved > 2 levels) the output did not carry, 0 = all carried, 1 = frozen; "
                  "`lost mv` = fraction of the cells the source moved that the output held static (the mirror of "
                  "`false mv`). Cuts, soft-cut spans and held source pairs are excluded. A render that only got "
                  "calmer lowers flicker+ and raises `lost`; one that got better lowers flicker+ and leaves `lost` "
                  "alone. VMAF v1 = mean (p5) of the v1 model for the clip's size and rate, at 10 bits; `CAMBI+` = "
                  "banding of the output minus the source's; `chroma` = v1's speed_chroma_uv feature (0 = none). "
                  "JOD = ColorVideoVDP, 10 = indistinguishable, ~0.1 is about the smallest visible step; "
                  "`trans` and `rg|yv` pool one channel alone (temporal, and the two chroma channels).", "",
                  "| clip | profile | flicker+ | sigma+ | added | lost | false mv | lost mv | VMAF v1 (p5) | v0.6.1 "
                  "| CAMBI+ | chroma | JOD | JOD trans | JOD rg|yv |",
                  "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for s in yard:
            lines.append("| " + " | ".join([
                s["clip"], s["profile"], g(s, "flicker_added"), g(s, "temporal_sigma_added"), g(s, "change_added"),
                g(s, "change_lost_share"), g(s, "false_motion_rate", 4), g(s, "lost_motion_rate", 4),
                f"{g(s, 'vmaf_v1', 2)} ({g(s, 'vmaf_v1_p5', 2)})" if s.get("vmaf_v1") is not None else "-",
                g(s, "vmaf_v061", 2), g(s, "cambi_added", 2), g(s, "speed_chroma_uv", 2), g(s, "jod", 2),
                g(s, "jod_transient", 2),
                f"{g(s, 'jod_rg', 2)}|{g(s, 'jod_yv', 2)}" if s.get("jod") is not None else "-"]) + " |")
    failed = [r for r in rows if not r["ok"]]
    if failed:
        lines += ["", "## Failed runs", ""] + [f"- {r['run']}: {r['failure']}" for r in failed]
    withheld = [r for r in rows if r.get("temporal_withheld")]
    if withheld:
        lines += ["", "## Temporal metrics withheld", ""] + \
                 [f"- {r['run']}: {r['temporal_withheld']}" for r in withheld]
    scored = [r for r in rows if r.get("cuts")]
    if scored:
        lines += ["", "## Cut detection against the manifest", "",
                  "The generator's own test (residual > 0.30, or residual > 0.10 with histogram overlap < 0.85, "
                  "debounced over 0.6 s) run frame by frame over the cell grids of both files and matched to the "
                  "corpus's hard-cut indices at +/-1 frame. The source column is a threshold check and is the "
                  "same for every profile of a clip; the output column is what a consumer of the rendered file "
                  "would detect. `worker` is the render's own strong/weak/suppressed counters where the run "
                  "reported them.", "",
                  "| run | GT | source P/R/F1 | output P/R/F1 | detected | false+ | worker |",
                  "|---|---|---|---|---|---|---|"]
        for r in scored:
            c = r["cuts"]
            trio = lambda side: "/".join(fmt(c[side][k], 2) for k in ("precision", "recall", "f1"))
            worker = "/".join(fmt(c[f"worker_{k}"], 0)
                              for k in ("accepted_strong", "accepted_weak", "suppressed"))
            lines.append("| " + " | ".join([r["run"], str(len(c["ground_truth"])), trio("source"), trio("output"),
                                            str(len(c["output"]["detected"])),
                                            str(len(c["output"]["false_positives"])), worker]) + " |")
    if guide_ab:
        lines += ["", "## Guide A/B versus baseline", "",
                  "Same clip, same settings, one guide string changed. This is the table the motion-vector and "
                  "depth claims stand or fall on: a guide that is doing its job lowers sigma+, false motion and "
                  "the flip rate together. The roadmap's `depth-constant` and `depth-proxy` are `depth-off` and "
                  "`baseline`, which are the two depth guide strings the worker can be asked for.", "",
                  "| clip | profile | guides | d sigma+ | d false mv | d flips+ | d flicker+ | d cut F1 | d PSNR "
                  "| d e2e fps |",
                  "|---|---|---|---|---|---|---|---|---|---|"]
        for g in guide_ab:
            lines.append("| " + " | ".join([g["clip"], g["profile"], g["guides"],
                                            fmt(g["delta_temporal_sigma_added"]),
                                            fmt(g["delta_false_motion_rate"], 4),
                                            fmt(g["delta_cell_flip_added"], 4), fmt(g["delta_flicker_added"]),
                                            fmt(g["delta_cut_f1_output"], 2), fmt(g["delta_psnr_mean"], 2),
                                            fmt(g["delta_end_to_end_fps"], 2)]) + " |")
    if two_pass:
        lines += ["", "## Two-pass versus single pass", "",
                  "Pass 2 re-renders the pass-1 *encoded* output (NVENC HEVC or software H.264), so its input "
                  "already carries one lossy generation; deltas mix model effect with re-encode loss.", "",
                  "| clip | profile | d flicker+ | d sigma+ | d false mv | d flips+ | d dE | d PSNR | d SSIM "
                  "| d OCR | d face cos | d e2e fps |",
                  "|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for t in two_pass:
            lines.append("| " + " | ".join([t["clip"], t["profile"], fmt(t["delta_flicker_added"]),
                                             fmt(t["delta_temporal_sigma_added"]),
                                             fmt(t["delta_false_motion_rate"], 4),
                                             fmt(t["delta_cell_flip_added"], 4),
                                             fmt(t["delta_delta_e_mean"], 2), fmt(t["delta_psnr_mean"], 2),
                                             fmt(t["delta_ssim_mean"], 4), fmt(t["delta_ocr_output"]),
                                             fmt(t["delta_face_cosine"]), fmt(t["delta_end_to_end_fps"], 2)]) + " |")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runs", type=Path, default=RUNS)
    parser.add_argument("--corpus", type=Path, default=CORPUS, help="corpus holding manifest.json")
    parser.add_argument("--sample-every", type=int, default=SAMPLE_EVERY,
                        help="frame stride for dE/PSNR/SSIM/OCR/faces; pair metrics always use every frame")
    parser.add_argument("--no-ocr", action="store_true")
    parser.add_argument("--no-faces", action="store_true")
    parser.add_argument("--force", action="store_true", help="recompute runs that already have metrics.json")
    parser.add_argument("--match", help="only runs whose directory name matches this glob, e.g. 'real-*__knob-*'")
    parser.add_argument("--lost-change", action="store_true",
                        help="frozen-output guard: added change and lost change per pixel and per cell")
    parser.add_argument("--vmaf-v1", action="store_true", help="VMAF v1 + v0.6.1 + CAMBI through libvmaf")
    parser.add_argument("--cvvdp", action="store_true", help="ColorVideoVDP JOD on the GPU")
    parser.add_argument("--yardstick", action="store_true", help="--lost-change --vmaf-v1 --cvvdp")
    args = parser.parse_args()
    if args.yardstick:
        args.lost_change = args.vmaf_v1 = args.cvvdp = True
    wanted = [key for key, on in (("change", args.lost_change), ("vmaf_v1", args.vmaf_v1), ("cvvdp", args.cvvdp))
              if on]
    manifest = load_manifest(args.corpus)
    clips = {c["name"]: c for c in manifest["clips"]}
    dirs = [d for d in run_dirs(args.runs) if not args.match or fnmatch.fnmatch(d.name, args.match)]
    if not dirs:
        print(f"no runs under {args.runs}", file=sys.stderr)
        return 1
    need_ocr = not args.no_ocr and any(clips[json.loads((d / "result.json").read_text())["clip"]]["text"] for d in dirs)
    need_faces = not args.no_faces and any(
        json.loads((d / "result.json").read_text())["category"] == "faces" for d in dirs)
    ocr = Ocr() if need_ocr else None
    faces = FaceEmbedder() if need_faces else None
    rows = []
    for run in dirs:
        # Membership first, before the cache short-circuit: one runs directory can
        # hold runs from several corpora, and a cached metrics.json from a foreign
        # one would otherwise be pulled into this analysis while an uncached one
        # was skipped - the same tree scoring differently depending on --force.
        name = json.loads((run / "result.json").read_text(encoding="utf-8"))["clip"]
        if name not in clips:
            print(f"skipping {run.name}: clip {name!r} is not in this corpus manifest "
                  f"(wrong --corpus, or a runs directory shared with another corpus)", flush=True)
            continue
        clip = clips[name]
        cached = run / "metrics.json"
        previous = json.loads(cached.read_text(encoding="utf-8")) if cached.exists() else None
        missing = [k for k in wanted if previous is not None and previous.get("ok") and k not in previous]
        if previous is not None and not args.force and not missing:
            rows.append(previous)
            print(f"cached {run.name}", flush=True)
            continue
        if previous is not None and not args.force and "change" not in missing:
            # Only perceptual scores were asked for: add them to the cached metrics
            # instead of decoding the whole run again.
            recorded = json.loads((run / "result.json").read_text(encoding="utf-8"))
            output = run / "output.mkv" if (run / "output.mkv").exists() else Path(recorded["output"])
            perceptual(previous, Path(recorded["source"]), output, clip, "vmaf_v1" in missing, "cvvdp" in missing)
            write_json(cached, previous)
            rows.append(previous)
            print(f"cached {run.name} + {', '.join(missing)}", flush=True)
            continue
        m = analyze_run(run, clip, ocr, faces, args.sample_every, args.lost_change, args.vmaf_v1, args.cvvdp)
        # Without --force, blocks an earlier opt-in run computed and this one did not are kept.
        for key in ("change", "vmaf_v1", "cvvdp"):
            if previous and not args.force and key in previous and key not in m:
                m[key] = previous[key]
        if any(k in m for k in ("change", "vmaf_v1", "cvvdp")):
            write_json(cached, m)
        rows.append(m)
        print(f"{run.name}: ok={m['ok']} psnr={fmt(m.get('psnr_mean'), 2)} dE={fmt(m.get('delta_e_mean'), 2)} "
              f"flicker+={fmt(m.get('flicker_added'))} sigma+={fmt(m.get('temporal_sigma_added'))} "
              f"false-mv={fmt((m.get('motion_field') or {}).get('false_motion_rate'), 4)} "
              f"cut-f1={fmt(((m.get('cuts') or {}).get('output') or {}).get('f1'), 2)} "
              f"ocr={fmt((m.get('ocr') or {}).get('output_ratio_mean'))}",
              flush=True)
    summary, two_pass, guide_ab = summarize(rows)
    ANALYSIS.mkdir(parents=True, exist_ok=True)
    write_json(ANALYSIS / "analysis.json",
               dict(runs=rows, summary=summary, two_pass=two_pass, guide_ab=guide_ab))
    text = report(summary, two_pass, guide_ab, rows)
    (ANALYSIS / "report.md").write_text(text, encoding="utf-8")
    print(text)
    print(f"analysis: {ANALYSIS}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
