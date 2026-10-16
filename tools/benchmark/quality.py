"""Perceptual scores for analyze.py (P1.42): VMAF v1 through FFmpeg's libvmaf and
ColorVideoVDP on the GPU, plus the fetch-and-verify step for the VMAF v1 models.

Both are full-reference scores of the output against the lossless source, so they are
only computed when the two files have the same frame count (analyze.py checks).

VMAF v1 (Netflix vmaf ``resource/doc/models_v1.md``): chroma-aware (``speed_chroma``),
CAMBI banding built in, separate 1080p@3H / phone@5H / 4K models, HFR variants for
~50/60 fps. libvmaf in ``external/ffmpeg`` (BtbN n9.0.2) is 86da14d, eight commits past
v3.2.1, and loads the v1 JSON models as they are. Both inputs are scored at 10 bits as
the model notes ask; v0.6.1 runs in the same pass for continuity with older numbers.

ColorVideoVDP (gfxdisp, MIT): JOD, 10 = indistinguishable from the source, ~0.1 JOD
is about the smallest difference a viewer would notice. Besides the overall JOD, the
per-channel JODs pool one channel at a time (the others zeroed) through the metric's
own pooling: ``transient`` is the visibility of temporal differences (flicker, but
also freezing - it is symmetric), ``rg``/``yv`` are the two chroma channels.

    python tools/benchmark/quality.py fetch      # download + verify the pinned models
"""
from __future__ import annotations

import hashlib
import json
import shutil
import statistics
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

import numpy as np

from common import FFMPEG, FLAGS, FrameReader, WORK

# The v3.2.1 tag (2026-09-14). The models are tiny JSON files under BSD-2-Clause-Patent;
# they are fetched into the gitignored work tree and verified, never committed.
VMAF_COMMIT = "f85a853692a8c730d0270cd733c8bb30b5b93b7c"
VMAF_MODELS = {
    "vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json": "e4cf8c147e1368b35497d772920bc92f98c1ad7853c1033d8a836947f427140e",
    "vmaf_v1.0.16/vmaf_v1.0.16_5d0h.json": "64e12d9c1e4ed5d79da7fa515b82fac7247a9e2ccc91e0ebce31d05489500385",
    "vmaf_v1.0.16/vmaf_v1.0.16_1d5h_2160.json": "634d36e0502cb797384726cdd1a106a2c207827595a9e4958d3dae1bd517b139",
    "vmaf_v1.0.16/vmaf_v1.0.16_3d0h_2160.json": "77f552fd8d5b6c6a2166bfc4470c9368ec68d2425ed40f1cc235b0cc500f33c2",
    "vmaf_v1.0.16_hfr/vmaf_v1.0.16_hfr_3d0h.json": "dcfdfab1c00685c1e5f75b6f1078bb4a44c722654e4de090fa5c66784c75241b",
    "vmaf_v1.0.16_hfr/vmaf_v1.0.16_hfr_5d0h.json": "590f612c87df94badca58e563b318fa9958eac631ab8aadd73bff8cc1adfc515",
    "vmaf_v1.0.16_hfr/vmaf_v1.0.16_hfr_1d5h_2160.json": "0371db55d3de81a760125f6d65eb4386f1d38af6cf1d00c2f48158e3596246d4",
    "vmaf_v1.0.16_hfr/vmaf_v1.0.16_hfr_3d0h_2160.json": "1bc502f00aeee11433e2d09468186bbf1bbce56ee650c9bade06c7435ebdd4db",
}
VMAF_MODELS_DIR = WORK / "vmaf-models" / VMAF_COMMIT[:12]
CVVDP_GPU_MEM_GB = 8  # cap, so a concurrent render keeps its VRAM


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fetch_vmaf_models(dest: Path = VMAF_MODELS_DIR) -> Path:
    """Downloads the pinned v1 models once and verifies every file against its SHA-256."""
    for rel, digest in VMAF_MODELS.items():
        path = dest / Path(rel).name
        if path.exists() and _sha256(path) == digest:
            continue
        dest.mkdir(parents=True, exist_ok=True)
        url = f"https://raw.githubusercontent.com/Netflix/vmaf/{VMAF_COMMIT}/model/{rel}"
        data = urllib.request.urlopen(url, timeout=60).read()
        got = hashlib.sha256(data).hexdigest()
        if got != digest:
            raise RuntimeError(f"{url}: sha256 {got} does not match the pinned {digest}")
        path.write_bytes(data)
    return dest


def vmaf_model_for(height: int, fps: float, view: str = "3d0h") -> str:
    """The model file models_v1.md recommends: 1080p at 3H (or the 5H phone model),
    the 1.5H 4K model above 1080 lines, the _hfr variant above 45 fps."""
    uhd = height > 1080
    name = ("1d5h_2160" if view == "3d0h" else f"{view}_2160") if uhd else view
    return f"vmaf_v1.0.16{'_hfr' if fps > 45 else ''}_{name}.json"


def _libvmaf(distorted: Path, reference: Path, model: Path) -> dict:
    """One FFmpeg pass, v1 and v0.6.1 side by side, JSON log. Runs in a temp directory
    holding a copy of the model so no Windows drive colon reaches the filter graph."""
    with tempfile.TemporaryDirectory() as tmp:
        shutil.copy(model, Path(tmp) / "v1.json")
        graph = ("[0:v]setpts=PTS-STARTPTS,format=yuv420p10le[d];"
                 "[1:v]setpts=PTS-STARTPTS,format=yuv420p10le[r];"
                 "[d][r]libvmaf=model='path=v1.json\\:name=vmaf_v1|version=vmaf_v0.6.1\\:name=vmaf_v061'"
                 ":log_fmt=json:log_path=vmaf.json:n_threads=16:shortest=1")
        subprocess.run([str(FFMPEG), "-v", "error", "-nostdin", "-i", str(distorted), "-i", str(reference),
                        "-lavfi", graph, "-f", "null", "-"], cwd=tmp, check=True, creationflags=FLAGS)
        return json.loads((Path(tmp) / "vmaf.json").read_text(encoding="utf-8"))


def _feature(frames: list[dict], prefix: str) -> list[float]:
    key = next(k for k in frames[0]["metrics"] if k.startswith(prefix))
    return [f["metrics"][key] for f in frames]


def vmaf_v1(source: Path, output: Path, height: int, fps: float) -> dict:
    """VMAF v1 (and v0.6.1) of the output against the source, CAMBI of both."""
    models = fetch_vmaf_models()
    model = vmaf_model_for(height, fps)
    log = _libvmaf(output, source, models / model)
    frames = log["frames"]
    v1, v061 = [f["metrics"]["vmaf_v1"] for f in frames], [f["metrics"]["vmaf_v061"] for f in frames]
    # CAMBI is no-reference on the distorted input, so the source's own banding needs a
    # second pass with the source in that seat.
    cambi_out = _feature(frames, "cambi")
    cambi_src = _feature(_libvmaf(source, source, models / model)["frames"], "cambi")
    chroma = _feature(frames, "speed_chroma_uv")
    return dict(model=model, model_commit=VMAF_COMMIT, libvmaf=log.get("version"), frames=len(frames),
                vmaf_v1_mean=statistics.fmean(v1), vmaf_v1_p5=float(np.percentile(v1, 5)), vmaf_v1_min=min(v1),
                vmaf_v061_mean=statistics.fmean(v061),
                cambi_output=statistics.fmean(cambi_out), cambi_source=statistics.fmean(cambi_src),
                cambi_added=statistics.fmean(cambi_out) - statistics.fmean(cambi_src),
                speed_chroma_uv=statistics.fmean(chroma))


class _PairStream:
    """A pycvvdp video source that decodes both files in lockstep instead of holding the
    clip in memory. cvvdp with replicate padding asks for frames strictly in order
    (test then reference for each index), which is all this supports."""

    def __init__(self, output: Path, source: Path, width: int, height: int, frames: int, fps: float,
                 display: str):
        import torch
        self.torch = torch
        super().__init__(display_photometry=display)  # pycvvdp.video_source_dm, mixed in by cvvdp()
        self.readers = (FrameReader(output, width, height), FrameReader(source, width, height))
        self.size, self.fps, self.index, self.pair = (height, width, frames), fps, -1, None

    def get_video_size(self):
        return self.size

    def get_frames_per_second(self):
        return self.fps

    def get_batch_size(self):
        return 1

    def _frame(self, which: int, frame: int, device, colorspace):
        while self.index < frame:
            self.pair = (next(self.readers[0]), next(self.readers[1]))
            self.index += 1
        if frame != self.index:
            raise RuntimeError(f"cvvdp asked for frame {frame} after {self.index}; the stream only moves forward")
        t = self.torch.from_numpy(self.pair[which].copy()).to(device)
        t = t.permute(2, 0, 1)[None, :, None].float() / 255  # BCFHW
        return self.apply_dm_and_color_transform(t, colorspace)

    def get_test_frame(self, frame, device, colorspace):
        return self._frame(0, frame, device, colorspace)

    def get_reference_frame(self, frame, device, colorspace):
        return self._frame(1, frame, device, colorspace)

    def close(self):
        for r in self.readers:
            r.close()


def cvvdp(source: Path, output: Path, width: int, height: int, frames: int, fps: float) -> dict:
    import pycvvdp
    import torch
    from importlib.metadata import version
    display = "standard_4k" if height > 1080 else "standard_fhd"
    from pycvvdp.video_source import video_source_dm
    stream_cls = type("PairStream", (_PairStream, video_source_dm), {})
    stream = stream_cls(output, source, width, height, frames, fps, display)
    metric = pycvvdp.cvvdp(display_name=display, quiet=True, device=torch.device("cuda"),
                           gpu_mem=CVVDP_GPU_MEM_GB)
    try:
        with torch.no_grad():
            jod, stats = metric.predict_video_source(stream)
            per_ch = torch.as_tensor(stats["Q_per_ch"], device=metric.device)
            names = ("sustained", "rg", "yv", "transient")
            channel = {}
            for c in range(per_ch.shape[1]):
                only = torch.zeros_like(per_ch)
                only[:, c] = per_ch[:, c]
                channel[names[c]] = float(metric.do_pooling_and_jods(only))
    finally:
        stream.close()
    return dict(version=version("cvvdp"), display=display, fps=fps, frames=frames, jod=float(jod),
                jod_channel=channel)


def main() -> int:
    if sys.argv[1:] == ["fetch"]:
        dest = fetch_vmaf_models()
        for rel, digest in VMAF_MODELS.items():
            print(f"{digest}  {dest / Path(rel).name}")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
