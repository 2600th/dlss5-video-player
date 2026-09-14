"""Shared paths, FFmpeg helpers and the NeuralWorker metadata-pipe decoder.

Every benchmark script writes only below ``build-upscaling/`` (gitignored).
"""
from __future__ import annotations

import hashlib
import json
import os
import struct
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "build-upscaling"
WORK = BUILD / "benchmark-work"
CORPUS = BUILD / "benchmark-corpus"
RUNS = WORK / "runs"
PROFILES = WORK / "profiles"
ANALYSIS = WORK / "analysis"
BLIND = WORK / "blind"
RUNTIME_SNAPSHOT = WORK / "runtime-snapshot"
RELEASE_RUNTIME = BUILD / "Release" / "neural-runtime"
FFMPEG = REPO / "external" / "ffmpeg" / "bin" / "ffmpeg.exe"
FFPROBE = REPO / "external" / "ffmpeg" / "bin" / "ffprobe.exe"
FLAGS = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0

# src/NeuralWorkerProtocol.h
WIRE_MAGIC = 0x3152574E  # NWR1
WIRE_VERSION = 6
KIND_PROGRESS, KIND_RESULT, KIND_PREFLIGHT, KIND_SEGMENT, KIND_TIMELINE = 1, 2, 3, 4, 5
PHASES = ["Idle", "Decoding", "Priming", "Rendering", "Encoding", "Validating", "Completed", "Failed",
          "Cancelled", "Ready", "Preflight", "Paused", "Recovering"]
FAILURES = ["None", "Source", "Encoder", "Neural", "GpuStall", "DeviceRemoved", "WorkerCrashed",
            "RetryExhausted", "Cancelled", "Preflight", "Identity", "Protocol"]
# src/NeuralRenderTypes.h NeuralColdStartPhase, in wire bit order.
COLD_START_PHASES = ["request", "preflight", "launch", "helperStart", "runtimeReady", "neuralInit",
                     "featureArm", "firstOutput", "attach"]
ENCODERS = {0: "hevc_nvenc", 1: "h264_software"}  # src/MediaPipeline.h EncoderKind
CONFIGURATION_CHANGED_EXIT = 75
PROGRESS_STRUCT = struct.Struct("<IQQQqqII")  # 52 bytes
RESULT_STRUCT = struct.Struct("<10B6xQqQQQQIIqdddddQQIIII")  # 152 bytes
PREFLIGHT_STRUCT = struct.Struct("<B3xI")  # 8 bytes
SEGMENT_STRUCT = struct.Struct("<QQqQqI")  # 44 bytes, then nameBytes of UTF-16LE
TIMELINE_STRUCT = struct.Struct("<I4x9q")  # 80 bytes
assert PROGRESS_STRUCT.size == 52 and RESULT_STRUCT.size == 152 and PREFLIGHT_STRUCT.size == 8
assert SEGMENT_STRUCT.size == 44 and TIMELINE_STRUCT.size == 80
assert len(COLD_START_PHASES) == 9


def decode_metadata(data: bytes) -> list[dict]:
    """Decodes every complete NWR1 v6 message in ``data``.

    Progress records carry ``kind='progress'``, the final record ``kind='result'``,
    a preflight probe ``kind='preflight'`` with the parsed JSON receipt, and the
    helper's cold-start breakdown ``kind='timeline'``. A timeline phase the helper
    did not measure is None rather than 0.
    """
    records: list[dict] = []
    pos = 0
    while pos + 12 <= len(data):
        magic, version, kind, count = struct.unpack_from("<IHHI", data, pos)
        if magic != WIRE_MAGIC or version != WIRE_VERSION:
            raise ValueError(f"invalid worker metadata at offset {pos}: magic={magic:#x} version={version}")
        pos += 12
        payload = data[pos:pos + count]
        if len(payload) < count:
            break
        pos += count
        if kind == KIND_PROGRESS and len(payload) == PROGRESS_STRUCT.size:
            phase, done, total, nbytes, elapsed, remaining, recovering, retries = PROGRESS_STRUCT.unpack(payload)
            records.append(dict(kind="progress", phase=PHASES[phase] if phase < len(PHASES) else phase,
                                completed_frames=done, total_frames=total, bytes=nbytes, elapsed_ms=elapsed,
                                remaining_ms=remaining, recovering=_name(FAILURES, recovering), retries=retries))
        elif kind == KIND_RESULT and len(payload) >= RESULT_STRUCT.size:
            v = RESULT_STRUCT.unpack_from(payload)
            detail_bytes = v[-1]
            detail = payload[RESULT_STRUCT.size:RESULT_STRUCT.size + detail_bytes].decode("utf-16le", "replace")
            records.append(dict(
                kind="result", ok=bool(v[0]), cancelled=bool(v[1]), encoder=ENCODERS.get(v[2], v[2]),
                armed_before_capture=bool(v[3]), upscaling_off=bool(v[4]), inline_contract=bool(v[5]),
                feature18_created=bool(v[6]), feature18_evaluated=bool(v[7]), later_failure=bool(v[8]),
                failure=_name(FAILURES, v[9]), frame_count=v[10], duration_100ns=v[11],
                native_evaluations=v[12], verified_neural_frames=v[13], highest_evaluation=v[14],
                job_id=v[15], history_resets=v[16], frame_retries=v[17], first_timestamp_100ns=v[18],
                neural_gpu_ms_p50=v[19], neural_gpu_ms_p95=v[20], neural_gpu_ms_max=v[21],
                guide_ms_mean=v[22], capture_ms_mean=v[23], peak_local_vram_mib=v[24],
                timing_samples=v[25], accepted_strong_cuts=v[26], accepted_weak_cuts=v[27],
                suppressed_cuts=v[28], detail=detail))
        elif kind == KIND_PREFLIGHT and len(payload) >= PREFLIGHT_STRUCT.size:
            ok, json_bytes = PREFLIGHT_STRUCT.unpack_from(payload)
            text = payload[PREFLIGHT_STRUCT.size:PREFLIGHT_STRUCT.size + json_bytes].decode("utf-8", "replace")
            try:
                parsed = json.loads(text)
            except json.JSONDecodeError:
                parsed = None
            records.append(dict(kind="preflight", ok=bool(ok), json=parsed, raw=text))
        elif kind == KIND_SEGMENT and len(payload) >= SEGMENT_STRUCT.size:
            index, first_frame, first_pts, frames, frame_duration, name_bytes = SEGMENT_STRUCT.unpack_from(payload)
            name = payload[SEGMENT_STRUCT.size:SEGMENT_STRUCT.size + name_bytes].decode("utf-16le", "replace")
            records.append(dict(kind="segment", index=index, first_frame_number=first_frame,
                                first_timestamp_100ns=first_pts, frame_count=frames,
                                frame_duration_100ns=frame_duration, name=name))
        elif kind == KIND_TIMELINE and len(payload) == TIMELINE_STRUCT.size:
            values = TIMELINE_STRUCT.unpack(payload)
            present = values[0]
            records.append(dict(kind="timeline", microseconds={
                name: (values[1 + index] if present & (1 << index) else None)
                for index, name in enumerate(COLD_START_PHASES)}))
    return records


def _name(names: list[str], value: int):
    return names[value] if value < len(names) else value


def probe(source: Path) -> dict:
    """Width, height, fps and 100 ns duration of the first video stream."""
    data = json.loads(subprocess.check_output(
        [str(FFPROBE), "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height,avg_frame_rate,r_frame_rate,codec_name,pix_fmt:format=duration", "-of", "json",
         str(source)], creationflags=FLAGS))
    stream = data["streams"][0]
    n, d = map(int, stream["avg_frame_rate"].split("/"))
    return dict(width=stream["width"], height=stream["height"], fps=n / d, codec=stream["codec_name"],
                pix_fmt=stream.get("pix_fmt"), duration_100ns=round(float(data["format"]["duration"]) * 1e7))


def framemd5(path: Path, cache: Path | None = None) -> list[str]:
    """Per-frame MD5 of the rgb24-decoded video stream (cached beside the run)."""
    if cache and cache.exists():
        text = cache.read_text()
    else:
        text = subprocess.check_output(
            [str(FFMPEG), "-v", "error", "-xerror", "-i", str(path), "-map", "0:v:0", "-pix_fmt", "rgb24",
             "-f", "framemd5", "-"], creationflags=FLAGS).decode()
        if cache:
            cache.write_text(text)
    return [line.split(",")[-1].strip() for line in text.splitlines() if line and not line.startswith("#")]


def sequence_digest(hashes: list[str]) -> str:
    return hashlib.sha256("\n".join(hashes).encode()).hexdigest()


class FrameReader:
    """Streams rgb24 frames from FFmpeg one at a time (never whole clips in memory)."""

    def __init__(self, path: Path, width: int, height: int, start_frame: int = 0, count: int | None = None):
        import numpy as np
        self.np = np
        self.width, self.height = width, height
        self.path = Path(path)
        # A missing input is a caller bug, not an empty clip. FFmpeg would exit with
        # nothing on stdout, which is byte-for-byte what end-of-stream looks like here,
        # so a run whose recorded source has moved would silently score zero frames.
        if not self.path.exists():
            raise FileNotFoundError(f"frame source does not exist: {self.path}")
        select = []
        if start_frame:
            select.append(f"select=gte(n\\,{start_frame})")
        args = [str(FFMPEG), "-v", "error", "-i", str(self.path), "-map", "0:v:0"]
        if select:
            args += ["-vf", ",".join(select)]
        if count is not None:
            args += ["-frames:v", str(count)]
        args += ["-pix_fmt", "rgb24", "-f", "rawvideo", "-"]
        self.process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                        creationflags=FLAGS, bufsize=0)
        self.frame_bytes = width * height * 3
        self.delivered = 0

    def __iter__(self):
        return self

    def __next__(self):
        buffer = bytearray()
        while len(buffer) < self.frame_bytes:
            chunk = self.process.stdout.read(self.frame_bytes - len(buffer))
            if not chunk:
                self.close()
                # Distinguish "the clip ended" from "FFmpeg could not decode it": a
                # decoder that failed before the first frame must not read as an empty clip.
                if self.delivered == 0 and self.process.returncode not in (0, None):
                    raise RuntimeError(f"decoded no frames from {self.path} "
                                       f"(ffmpeg exit {self.process.returncode})")
                raise StopIteration
            buffer.extend(chunk)
        self.delivered += 1
        return self.np.frombuffer(bytes(buffer), dtype=self.np.uint8).reshape(self.height, self.width, 3)

    def close(self):
        if self.process.stdout:
            self.process.stdout.close()
        self.process.wait()


def read_frame(path: Path, index: int, width: int, height: int):
    reader = FrameReader(path, width, height, start_frame=index, count=1)
    try:
        return next(reader)
    finally:
        reader.close()


def load_manifest(corpus: Path = CORPUS) -> dict:
    return json.loads((corpus / "manifest.json").read_text(encoding="utf-8"))


def run_dirs(runs: Path = RUNS) -> list[Path]:
    return sorted(p.parent for p in runs.glob("*/result.json"))


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2), encoding="utf-8")
