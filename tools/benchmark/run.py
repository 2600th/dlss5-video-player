"""Renders corpus clips through NeuralWorker.exe under named profiles.

A profile is a named set of {guides canonical string, RenoDX [RenoDX.DLSS5]
INI overrides, passes 1|2}. Each profile gets its own isolated copy of the
neural runtime (``benchmark-work/profiles/<profile>/neural-runtime``) with the
overrides written into ReShade.ini, one preflight probe (receipt saved as
``preflight.json``) and then one worker launch per clip and repeat. Results are
decoded from the protocol-v2 metadata pipe and stored as
``runs/<clip>__<profile>__<rep>/result.json`` beside ``output.mkv``.

Two-pass profiles render pass 1, then feed the pass-1 MKV (an NVENC HEVC or
software H.264 re-encode) as the source of pass 2 at reduced strength. The
result flags ``pass2_reencoded_input=True`` because the second pass never sees
the lossless corpus frames.

    python tools/benchmark/run.py --clips text-subtitles cuts-motion --profiles baseline mv-off --repeats 1
    python tools/benchmark/run.py --ablation            # every profile in ABLATION
    python tools/benchmark/run.py --list-profiles
"""
from __future__ import annotations

import argparse
import csv
import ctypes
import json
import msvcrt
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

import psutil

from common import (CONFIGURATION_CHANGED_EXIT, CORPUS, FFMPEG, FFPROBE, FLAGS, PROFILES, RELEASE_RUNTIME,
                    RUNS, RUNTIME_SNAPSHOT, decode_metadata, load_manifest, probe, write_json)

DEFAULT_GUIDES = "mv=1,depth=1,mask=1"
PASS2_OVERRIDES = {"NRIntensity": "0.750000"}


def profile(guides=DEFAULT_GUIDES, overrides=None, passes=1, description=""):
    return dict(guides=guides, overrides=dict(overrides or {}), passes=passes, description=description)


# Ablation matrix: one factor changes per profile relative to baseline so a
# difference in the analysis attributes to that factor alone.
ABLATION = {
    "baseline": profile(description="all guides, RenoDX defaults"),
    "mv-off": profile("mv=0,depth=1,mask=1", description="zero motion vectors"),
    "depth-off": profile("mv=1,depth=0,mask=1", description="constant depth 0.75"),
    "mask-off": profile("mv=1,depth=1,mask=0", description="zero temporal mask"),
    "automask-off": profile(overrides={"NRAutoMask": "0"}, description="RenoDX automatic mask disabled"),
    "structure-0": profile(overrides={"NRLocalStructure": "0.000000"}, description="local structure strength 0"),
    "tone-0": profile(overrides={"NRLocalTone": "0.000000"}, description="local tone strength 0"),
    "intensity-0": profile(overrides={"NRIntensity": "0.000000"}, description="relighting intensity 0 (control)"),
    "preset-1": profile(overrides={"NRPreset": "1"}, description="render preset #1"),
    "preset-2": profile(overrides={"NRPreset": "2"}, description="render preset #2"),
    "preset-3": profile(overrides={"NRPreset": "3"}, description="render preset #3"),
    "style-natural": profile(overrides={"NRStyle": "1"}, description="style Natural"),
    "style-cinematic": profile(overrides={"NRStyle": "2"}, description="style Cinematic"),
    "two-pass": profile(passes=2, description="baseline, then pass 2 at NRIntensity 0.75 over the pass-1 output"),
}


class GpuMonitor:
    """Samples NVML device-wide memory/utilisation/temperature/power at ~2 Hz.

    ``total_used_mib`` is the whole GPU (every process), not the worker's own
    local budget; compare it with the receipt's ``peak_local_vram_mib``.
    """

    class Memory(ctypes.Structure):
        _fields_ = [("total", ctypes.c_ulonglong), ("free", ctypes.c_ulonglong), ("used", ctypes.c_ulonglong)]

    class Util(ctypes.Structure):
        _fields_ = [("gpu", ctypes.c_uint), ("memory", ctypes.c_uint)]

    def __init__(self):
        self.samples = []
        self.api = None
        try:
            api = ctypes.WinDLL("nvml.dll")
            if api.nvmlInit_v2() != 0:
                return
            self.handle = ctypes.c_void_p()
            if api.nvmlDeviceGetHandleByIndex_v2(0, ctypes.byref(self.handle)) != 0:
                api.nvmlShutdown()
                return
            self.api = api
        except OSError:
            return

    def sample(self, elapsed):
        if not self.api:
            return
        memory, util = self.Memory(), self.Util()
        temperature, power = ctypes.c_uint(), ctypes.c_uint()
        errors = [self.api.nvmlDeviceGetMemoryInfo(self.handle, ctypes.byref(memory)),
                  self.api.nvmlDeviceGetUtilizationRates(self.handle, ctypes.byref(util)),
                  self.api.nvmlDeviceGetTemperature(self.handle, 0, ctypes.byref(temperature)),
                  self.api.nvmlDeviceGetPowerUsage(self.handle, ctypes.byref(power))]
        if any(errors):
            return
        self.samples.append(dict(elapsed_seconds=round(elapsed, 3), gpu_util_percent=util.gpu,
                                 memory_util_percent=util.memory, total_used_mib=round(memory.used / 1048576, 1),
                                 temperature_c=temperature.value, power_w=power.value / 1000))

    def finish(self, path: Path) -> dict:
        if self.api:
            self.api.nvmlShutdown()
        if self.samples:
            with path.open("w", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=list(self.samples[0]))
                writer.writeheader()
                writer.writerows(self.samples)
            return dict(nvml_samples=len(self.samples),
                        nvml_total_used_peak_mib=max(s["total_used_mib"] for s in self.samples),
                        nvml_gpu_util_mean_percent=sum(s["gpu_util_percent"] for s in self.samples) / len(self.samples),
                        nvml_gpu_util_peak_percent=max(s["gpu_util_percent"] for s in self.samples),
                        nvml_power_peak_w=max(s["power_w"] for s in self.samples),
                        nvml_temperature_peak_c=max(s["temperature_c"] for s in self.samples))
        return dict(nvml_samples=0)


def runtime_source() -> Path:
    """Prefers the frozen snapshot so concurrent rebuilds of Release/ cannot change the worker mid-run."""
    if (RUNTIME_SNAPSHOT / "NeuralWorker.exe").exists():
        return RUNTIME_SNAPSHOT
    return RELEASE_RUNTIME


def wait_for_stable(exe: Path, attempts: int = 30) -> None:
    """A sibling build may be relinking NeuralWorker.exe; wait until it is openable and stable."""
    last = None
    for _ in range(attempts):
        try:
            with exe.open("rb"):
                pass
            size = exe.stat().st_size
            if size and size == last:
                return
            last = size
        except OSError:
            last = None
        time.sleep(2)
    raise RuntimeError(f"{exe} never became stable/openable")


def write_overrides(ini: Path, overrides: dict[str, str]) -> None:
    """Writes [RenoDX.DLSS5] with the managed keys plus profile overrides (exact case, one key each)."""
    lines = ini.read_text(encoding="utf-8").splitlines() if ini.exists() else []
    kept, section, skipping = [], False, False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("["):
            skipping = stripped.lower() == "[renodx.dlss5]"
            section = section or skipping
        if not skipping:
            kept.append(line)
    while kept and not kept[-1].strip():
        kept.pop()
    managed = {"EnableHooks": "2", "NeuralUplift": "1", "NREnableUpscaling": "0"}
    body = ["", "[RenoDX.DLSS5]"] + [f"{k}={v}" for k, v in managed.items()] + \
           [f"{k}={v}" for k, v in overrides.items() if k not in managed]
    ini.write_text("\n".join(kept + body) + "\n", encoding="utf-8")


def prepare(name: str, spec: dict, pass_index: int, fresh: bool) -> Path:
    """Clones the runtime for ``name`` (pass 2 gets its own clone with pass-2 overrides)."""
    tag = name if pass_index == 1 else f"{name}--pass2"
    root = PROFILES / tag
    runtime = root / "neural-runtime"
    if fresh and root.exists():
        shutil.rmtree(root)
    if not (runtime / "NeuralWorker.exe").exists():
        source = runtime_source()
        wait_for_stable(source / "NeuralWorker.exe")
        shutil.copytree(source, runtime, ignore=shutil.ignore_patterns("*.log", "*.log1", "ngx_logs",
                                                                       "DLSSVideoPlayer.log"))
        # VideoDecoder::FindTool: a neural-runtime helper only looks in its parent directory.
        for helper in (FFMPEG, FFPROBE):
            dest = root / helper.name
            if not dest.exists():
                try:
                    os.link(helper, dest)
                except OSError:
                    shutil.copy2(helper, dest)
        overrides = dict(spec["overrides"])
        if pass_index == 2:
            overrides.update(PASS2_OVERRIDES)
        write_overrides(runtime / "ReShade.ini", overrides)
        write_json(root / "profile.json", dict(name=name, pass_index=pass_index, guides=spec["guides"],
                                               overrides=overrides, description=spec["description"],
                                               runtime_source=str(source)))
    return runtime


def launch(runtime: Path, args: list[str], metadata: Path, log: Path, timeout: float, sample: bool):
    """Runs one worker invocation with an inheritable metadata pipe handle; returns (attempt dict, records)."""
    startup = subprocess.STARTUPINFO()
    monitor = GpuMonitor() if sample else None
    with metadata.open("wb") as f, log.open("ab") as out:
        handle = msvcrt.get_osfhandle(f.fileno())
        os.set_handle_inheritable(handle, True)
        startup.lpAttributeList = {"handle_list": [handle]}
        argv = [str(runtime / "NeuralWorker.exe"), args[0], "--metadata-handle", str(handle), *args[1:]]
        start = time.perf_counter()
        process = subprocess.Popen(argv, cwd=runtime, stdout=out, stderr=out, startupinfo=startup,
                                   close_fds=True, creationflags=FLAGS)
        timed_out, modules, sampled_at, first_frame_at = False, [], -1.0, None
        try:
            while process.poll() is None:
                elapsed = time.perf_counter() - start
                if elapsed > timeout:
                    raise subprocess.TimeoutExpired(argv, timeout)
                if monitor and elapsed - sampled_at >= 0.5:
                    monitor.sample(elapsed)
                    sampled_at = elapsed
                if first_frame_at is None or not modules:
                    live = decode_metadata(metadata.read_bytes())
                    if first_frame_at is None and any(r.get("completed_frames", 0) >= 1 for r in live):
                        first_frame_at = elapsed
                    if not modules and any(r.get("completed_frames", 0) >= 10 for r in live):
                        try:
                            modules = sorted({m.path for m in psutil.Process(process.pid).memory_maps()})
                        except psutil.Error:
                            pass
                time.sleep(0.1)
            code = process.returncode
        except subprocess.TimeoutExpired:
            timed_out = True
            subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True,
                           creationflags=FLAGS)
            code = process.wait(timeout=10)
        wall = time.perf_counter() - start
    records = decode_metadata(metadata.read_bytes())
    attempt = dict(returncode=code, timed_out=timed_out, wall_seconds=wall, first_frame_seconds=first_frame_at,
                   records=records, loaded_modules=[m for m in modules if "neural-runtime" in m.lower()])
    if monitor:
        attempt.update(monitor.finish(metadata.with_name("gpu.csv")))
    return attempt


def with_restart(runtime: Path, args: list[str], dest: Path, stem: str, timeout: float, sample: bool):
    """Worker exit 75 means it repaired ReShade.ini; the parent relaunches exactly once with the flag."""
    attempts = []
    for restarted in (False, True):
        suffix = "-restart" if restarted else ""
        attempt = launch(runtime, args + ([f"--configuration-restarted"] if restarted else []),
                         dest / f"{stem}{suffix}.bin", dest / f"{stem}-stdout{suffix}.txt", timeout, sample)
        attempts.append(attempt)
        for filename in ("ReShade.log", "ReShade.ini", "ReShadePreset.ini"):
            if (runtime / filename).exists():
                shutil.copy2(runtime / filename, dest / f"{stem}{suffix}-{filename}")
        if attempt["returncode"] != CONFIGURATION_CHANGED_EXIT:
            break
    return attempts


def preflight(name: str, runtime: Path, timeout: float) -> dict:
    root = runtime.parent
    receipt = root / "preflight.json"
    if receipt.exists():
        return json.loads(receipt.read_text(encoding="utf-8"))
    attempts = with_restart(runtime, ["--neural-preflight"], root, "preflight", timeout, sample=False)
    final = next((r for r in reversed(attempts[-1]["records"]) if r["kind"] == "preflight"), None)
    payload = dict(profile=name, ok=bool(final and final["ok"]), receipt=final["json"] if final else None,
                   returncode=attempts[-1]["returncode"], wall_seconds=attempts[-1]["wall_seconds"],
                   attempts=len(attempts))
    write_json(receipt, payload)
    gpu = (payload["receipt"] or {}).get("gpu", {})
    print(f"preflight {name}: ok={payload['ok']} gpu={gpu.get('description')} driver={gpu.get('driverVersion')}",
          flush=True)
    return payload


def render(runtime: Path, source: Path, output: Path, dest: Path, stem: str, guides: str, job_id: int,
           timeout: float) -> dict:
    info = probe(source)
    args = ["--neural-worker", "--source", str(source), "--staging", str(output), "--width", str(info["width"]),
            "--height", str(info["height"]), "--fps", repr(info["fps"]), "--duration-100ns",
            str(info["duration_100ns"]), "--job-id", str(job_id), "--range-start-100ns", "0",
            "--range-end-100ns", "0", "--preroll-frames", "60", "--frame-retry-limit", "3", "--guides", guides]
    attempts = with_restart(runtime, args, dest, stem, timeout, sample=True)
    last = attempts[-1]
    final = next((r for r in reversed(last["records"]) if r["kind"] == "result"), {})
    progress = [r for r in last["records"] if r["kind"] == "progress" and r["phase"] == "Rendering"]
    processing_fps = None
    if len(progress) > 1 and progress[-1]["elapsed_ms"] > progress[0]["elapsed_ms"]:
        processing_fps = (progress[-1]["completed_frames"] - progress[0]["completed_frames"]) * 1000 / \
                         (progress[-1]["elapsed_ms"] - progress[0]["elapsed_ms"])
    return dict(source=str(source), output=str(output), probe=info, arguments=args[1:], attempts=attempts,
                result=final, wall_seconds=last["wall_seconds"], first_frame_seconds=last["first_frame_seconds"],
                end_to_end_fps=final.get("frame_count", 0) / last["wall_seconds"] if last["wall_seconds"] else None,
                processing_fps=processing_fps, nvml={k: v for k, v in last.items() if k.startswith("nvml_")})


def run_one(clip: dict, name: str, spec: dict, rep: int, timeout: float, corpus: Path, fresh: bool) -> dict:
    dest = RUNS / f"{clip['name']}__{name}__{rep}"
    if (dest / "result.json").exists():
        print(f"skip {dest.name}: result.json exists", flush=True)
        return json.loads((dest / "result.json").read_text(encoding="utf-8"))
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    runtime1 = prepare(name, spec, 1, fresh)
    receipt1 = preflight(name, runtime1, timeout)
    source = corpus / clip["file"]
    job_base = (hash((clip["name"], name, rep)) & 0xFFFF_FFFF) + 1
    passes = [render(runtime1, source, dest / "output.mkv", dest, "pass1", spec["guides"], job_base, timeout)]
    passes[0]["pass"] = 1
    if spec["passes"] == 2 and passes[0]["result"].get("ok"):
        shutil.move(dest / "output.mkv", dest / "pass1.mkv")
        passes[0]["output"] = str(dest / "pass1.mkv")
        runtime2 = prepare(name, spec, 2, fresh)
        preflight(f"{name}--pass2", runtime2, timeout)
        passes.append(render(runtime2, dest / "pass1.mkv", dest / "output.mkv", dest, "pass2", spec["guides"],
                             job_base + 1, timeout))
        passes[-1]["pass"] = 2
    final = passes[-1]
    result = dict(
        schema=1, run=dest.name, clip=clip["name"], category=clip["category"], profile=name, repeat=rep,
        guides=spec["guides"], overrides=spec["overrides"], passes=spec["passes"],
        pass2_reencoded_input=spec["passes"] == 2, pass2_overrides=PASS2_OVERRIDES if spec["passes"] == 2 else {},
        preflight_ok=receipt1["ok"], preflight=receipt1["receipt"], source=str(source),
        output=str(dest / "output.mkv"), width=clip["width"], height=clip["height"], fps=clip["fps"],
        source_frames=clip["frames"], cuts=clip["cuts"], pass_results=passes, result=final["result"],
        wall_seconds=sum(p["wall_seconds"] for p in passes), wall_seconds_final_pass=final["wall_seconds"],
        first_frame_seconds=final["first_frame_seconds"], end_to_end_fps=final["end_to_end_fps"],
        processing_fps=final["processing_fps"], nvml=final["nvml"],
        timing={k: final["result"].get(k) for k in ("neural_gpu_ms_p50", "neural_gpu_ms_p95", "neural_gpu_ms_max",
                                                   "guide_ms_mean", "capture_ms_mean", "peak_local_vram_mib",
                                                   "timing_samples")})
    write_json(dest / "result.json", result)
    r = final["result"]
    print(json.dumps(dict(run=dest.name, ok=r.get("ok"), failure=r.get("failure"), frames=r.get("frame_count"),
                          encoder=r.get("encoder"), wall_s=round(result["wall_seconds"], 2),
                          e2e_fps=round(result["end_to_end_fps"] or 0, 2),
                          proc_fps=round(result["processing_fps"] or 0, 2),
                          gpu_ms_p50=r.get("neural_gpu_ms_p50"), peak_vram_mib=r.get("peak_local_vram_mib"),
                          detail=r.get("detail", "")[:200])), flush=True)
    return result


def load_profiles(path: Path | None) -> dict:
    if not path:
        return ABLATION
    custom = json.loads(path.read_text(encoding="utf-8"))
    return {k: profile(v.get("guides", DEFAULT_GUIDES), v.get("overrides"), v.get("passes", 1), v.get("description", ""))
            for k, v in custom.items()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", type=Path, default=CORPUS)
    parser.add_argument("--clips", nargs="*", help="clip names from manifest.json (default: all)")
    parser.add_argument("--profiles", nargs="*", help="profile names (default: baseline)")
    parser.add_argument("--profile-file", type=Path, help="JSON {name:{guides,overrides,passes,description}}")
    parser.add_argument("--ablation", action="store_true", help="run every built-in ablation profile")
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=900.0, help="seconds per worker launch")
    parser.add_argument("--fresh-profiles", action="store_true", help="rebuild profile runtime clones")
    parser.add_argument("--list-profiles", action="store_true")
    args = parser.parse_args()
    profiles = load_profiles(args.profile_file)
    if args.list_profiles:
        for name, spec in profiles.items():
            print(f"{name:16s} guides={spec['guides']} passes={spec['passes']} overrides={spec['overrides']} "
                  f"- {spec['description']}")
        return 0
    manifest = load_manifest(args.corpus)
    clips = {c["name"]: c for c in manifest["clips"]}
    wanted = args.clips or list(clips)
    unknown = [c for c in wanted if c not in clips]
    if unknown:
        parser.error(f"unknown clips {unknown}; available: {list(clips)}")
    names = list(profiles) if args.ablation else (args.profiles or ["baseline"])
    unknown = [n for n in names if n not in profiles]
    if unknown:
        parser.error(f"unknown profiles {unknown}; available: {list(profiles)}")
    RUNS.mkdir(parents=True, exist_ok=True)
    failures = 0
    for rep in range(1, args.repeats + 1):
        for name in names:
            for clip in wanted:
                result = run_one(clips[clip], name, profiles[name], rep, args.timeout, args.corpus,
                                 args.fresh_profiles)
                failures += not result["result"].get("ok")
    print(f"runs: {RUNS} ({failures} failed)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
