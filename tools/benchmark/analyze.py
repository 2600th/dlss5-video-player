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
  two-pass      metric deltas of pass-2 profiles versus their single-pass
                counterpart on the same clip

Writes analysis/analysis.json, analysis/report.md and per-run metrics.json.

    python tools/benchmark/analyze.py [--sample-every N] [--no-ocr] [--no-faces]
"""
from __future__ import annotations

import argparse
import difflib
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import cv2
import numpy as np

from common import ANALYSIS, FrameReader, RUNS, framemd5, load_manifest, run_dirs, sequence_digest, write_json

SAMPLE_EVERY = 15


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


def analyze_run(run: Path, clip: dict, ocr: Ocr | None, faces: FaceEmbedder | None, sample_every: int) -> dict:
    result = json.loads((run / "result.json").read_text(encoding="utf-8"))
    metrics = dict(run=run.name, clip=result["clip"], category=result["category"], profile=result["profile"],
                   repeat=result["repeat"], passes=result["passes"], ok=bool(result["result"].get("ok")),
                   failure=result["result"].get("failure"), encoder=result["result"].get("encoder"),
                   wall_seconds=result["wall_seconds"], end_to_end_fps=result["end_to_end_fps"],
                   processing_fps=result["processing_fps"], timing=result["timing"], nvml=result["nvml"],
                   history_resets=result["result"].get("history_resets"),
                   frame_retries=result["result"].get("frame_retries"),
                   pass2_reencoded_input=result.get("pass2_reencoded_input", False))
    if not metrics["ok"]:
        return metrics
    output = Path(result["output"])
    hashes = framemd5(output, run / "frames.md5")
    metrics.update(output_frames=len(hashes), output_digest=sequence_digest(hashes),
                   unique_output_frames=len(set(hashes)),
                   frame_count_matches_source=len(hashes) == clip["frames"])
    w, h, cuts = clip["width"], clip["height"], set(clip["cuts"])
    src, out = FrameReader(Path(result["source"]), w, h), FrameReader(output, w, h)
    flick_src, flick_out, de, rgb_delta, psnrs, ssims = [], [], [], [], [], []
    ocr_rows, face_rows = [], []
    prev_src_y = prev_out_y = None
    prev_src_emb = prev_out_emb = None
    for index, (s, o) in enumerate(zip(src, out)):
        sy, oy = luma(s), luma(o)
        if prev_src_y is not None and index not in cuts:
            flick_src.append(float(np.abs(sy - prev_src_y).mean()))
            flick_out.append(float(np.abs(oy - prev_out_y).mean()))
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
    write_json(run / "metrics.json", metrics)
    return metrics


def fmt(value, digits=3):
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def summarize(rows: list[dict]) -> tuple[list[dict], list[dict]]:
    groups = defaultdict(list)
    for r in rows:
        if r["ok"]:
            groups[(r["clip"], r["profile"])].append(r)
    summary = []
    for (clip, prof), runs in sorted(groups.items()):
        med = lambda key: statistics.median(r[key] for r in runs if r.get(key) is not None) \
            if any(r.get(key) is not None for r in runs) else None
        timing = lambda key: statistics.median(r["timing"][key] for r in runs if r["timing"].get(key) is not None) \
            if any(r["timing"].get(key) is not None for r in runs) else None
        summary.append(dict(
            clip=clip, profile=prof, repeats=len(runs), passes=runs[0]["passes"],
            deterministic=len({r["output_digest"] for r in runs}) == 1 if len(runs) > 1 else None,
            wall_seconds=med("wall_seconds"), end_to_end_fps=med("end_to_end_fps"),
            processing_fps=med("processing_fps"),
            neural_gpu_ms_p50=timing("neural_gpu_ms_p50"), neural_gpu_ms_p95=timing("neural_gpu_ms_p95"),
            neural_gpu_ms_max=timing("neural_gpu_ms_max"), guide_ms_mean=timing("guide_ms_mean"),
            capture_ms_mean=timing("capture_ms_mean"), peak_local_vram_mib=timing("peak_local_vram_mib"),
            nvml_total_used_peak_mib=statistics.median(r["nvml"].get("nvml_total_used_peak_mib") or 0 for r in runs),
            flicker_added=med("flicker_added"), delta_e_mean=med("delta_e_mean"), psnr_mean=med("psnr_mean"),
            ssim_mean=med("ssim_mean"),
            ocr_output=statistics.median(r["ocr"]["output_ratio_mean"] for r in runs) if runs[0].get("ocr") else None,
            ocr_source=statistics.median(r["ocr"]["source_ratio_mean"] for r in runs) if runs[0].get("ocr") else None,
            face_cosine=statistics.median(r["faces"]["source_vs_output_cosine_mean"] for r in runs)
            if runs[0].get("faces") else None,
            face_drift_output=statistics.median(r["faces"]["output_drift_mean"] or 0 for r in runs)
            if runs[0].get("faces") else None,
            history_resets=med("history_resets")))
    by_key = {(s["clip"], s["profile"]): s for s in summary}
    two_pass = []
    for s in summary:
        if s["passes"] != 2:
            continue
        base = by_key.get((s["clip"], "baseline"))
        if not base:
            continue
        two_pass.append(dict(clip=s["clip"], profile=s["profile"], versus="baseline", **{
            f"delta_{k}": (s[k] - base[k]) if s[k] is not None and base[k] is not None else None
            for k in ("flicker_added", "delta_e_mean", "psnr_mean", "ssim_mean", "ocr_output", "face_cosine",
                      "end_to_end_fps")}))
    return summary, two_pass


def report(summary: list[dict], two_pass: list[dict], rows: list[dict]) -> str:
    lines = ["# Neural benchmark report", "",
             "Per clip/profile medians over repeats. `flicker_added` = output minus source mean |dY| "
             "(cut frames excluded); `dE` = mean CIE76 in CIELAB vs source; PSNR/SSIM vs the lossless source "
             "(low values on a strong relight are expected, not failures); OCR = char-level ratio vs manifest "
             "strings (output | source); VRAM = worker peak local budget (receipt) | NVML whole-GPU peak; "
             "wall s = all passes, e2e fps = final pass only.", "",
             "| clip | profile | n | det | wall s | e2e fps | proc fps | gpu ms p50/p95/max | guide ms | capture ms "
             "| VRAM MiB local|total | flicker+ | dE | PSNR | SSIM | OCR out|src | face cos | resets |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for s in summary:
        lines.append("| " + " | ".join([
            s["clip"], s["profile"], str(s["repeats"]), fmt(s["deterministic"]), fmt(s["wall_seconds"], 1),
            fmt(s["end_to_end_fps"], 2),
            fmt(s["processing_fps"], 2),
            f"{fmt(s['neural_gpu_ms_p50'], 2)}/{fmt(s['neural_gpu_ms_p95'], 2)}/{fmt(s['neural_gpu_ms_max'], 2)}",
            fmt(s["guide_ms_mean"], 2), fmt(s["capture_ms_mean"], 2),
            f"{fmt(s['peak_local_vram_mib'], 0)}|{fmt(s['nvml_total_used_peak_mib'], 0)}",
            fmt(s["flicker_added"]), fmt(s["delta_e_mean"], 2), fmt(s["psnr_mean"], 2), fmt(s["ssim_mean"], 4),
            f"{fmt(s['ocr_output'])}|{fmt(s['ocr_source'])}" if s["ocr_output"] is not None else "-",
            fmt(s["face_cosine"]), fmt(s["history_resets"], 0)]) + " |")
    failed = [r for r in rows if not r["ok"]]
    if failed:
        lines += ["", "## Failed runs", ""] + [f"- {r['run']}: {r['failure']}" for r in failed]
    if two_pass:
        lines += ["", "## Two-pass versus single pass", "",
                  "Pass 2 re-renders the pass-1 *encoded* output (NVENC HEVC or software H.264), so its input "
                  "already carries one lossy generation; deltas mix model effect with re-encode loss.", "",
                  "| clip | profile | d flicker+ | d dE | d PSNR | d SSIM | d OCR | d face cos | d e2e fps |",
                  "|---|---|---|---|---|---|---|---|---|"]
        for t in two_pass:
            lines.append("| " + " | ".join([t["clip"], t["profile"], fmt(t["delta_flicker_added"]),
                                             fmt(t["delta_delta_e_mean"], 2), fmt(t["delta_psnr_mean"], 2),
                                             fmt(t["delta_ssim_mean"], 4), fmt(t["delta_ocr_output"]),
                                             fmt(t["delta_face_cosine"]), fmt(t["delta_end_to_end_fps"], 2)]) + " |")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runs", type=Path, default=RUNS)
    parser.add_argument("--sample-every", type=int, default=SAMPLE_EVERY, help="frame stride for dE/PSNR/SSIM/OCR/faces")
    parser.add_argument("--no-ocr", action="store_true")
    parser.add_argument("--no-faces", action="store_true")
    parser.add_argument("--force", action="store_true", help="recompute runs that already have metrics.json")
    args = parser.parse_args()
    manifest = load_manifest()
    clips = {c["name"]: c for c in manifest["clips"]}
    dirs = run_dirs(args.runs)
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
        cached = run / "metrics.json"
        if cached.exists() and not args.force:
            rows.append(json.loads(cached.read_text(encoding="utf-8")))
            print(f"cached {run.name}", flush=True)
            continue
        clip = clips[json.loads((run / "result.json").read_text(encoding="utf-8"))["clip"]]
        m = analyze_run(run, clip, ocr, faces, args.sample_every)
        rows.append(m)
        print(f"{run.name}: ok={m['ok']} psnr={fmt(m.get('psnr_mean'), 2)} dE={fmt(m.get('delta_e_mean'), 2)} "
              f"flicker+={fmt(m.get('flicker_added'))} ocr={fmt((m.get('ocr') or {}).get('output_ratio_mean'))}",
              flush=True)
    summary, two_pass = summarize(rows)
    ANALYSIS.mkdir(parents=True, exist_ok=True)
    write_json(ANALYSIS / "analysis.json", dict(runs=rows, summary=summary, two_pass=two_pass))
    text = report(summary, two_pass, rows)
    (ANALYSIS / "report.md").write_text(text, encoding="utf-8")
    print(text)
    print(f"analysis: {ANALYSIS}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
