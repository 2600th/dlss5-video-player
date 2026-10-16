"""What moving one decoded frame through the decoder pipe costs (P2.57).

The worker's decoder child (src/VideoDecoder.cpp) writes rawvideo NV12 (or BGRA) to an
anonymous pipe created with 2 frames of buffer (clamped to 4-64 MiB), and the worker
reads it with ReadFile into a frame-sized buffer, at most 16 MiB per call. This
reproduces that transport with nothing else in it:

* ``ffmpeg-raw``  - ffmpeg reads a pre-decoded raw file (page cache) and writes it to
  the pipe unchanged; the consumer reads each frame into one preallocated buffer.
  Transport plus ffmpeg's own rawvideo copy.
* ``thread``      - an in-process writer thread writes a preallocated frame into the
  same kind of pipe: the kernel pipe copy alone, both ends.
* ``decode-pipe`` / ``decode-null`` - ffmpeg decoding the 4K H.264 clip into the pipe,
  against the same decode into ``-f null``: what the pipe adds to a real decode.

    python tools/benchmark/pipe_transport.py --clip build-upscaling/benchmark-corpus-4k/real-game-motion-4k.mkv
"""
from __future__ import annotations

import _winapi
import argparse
import json
import msvcrt
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

from common import FFMPEG, FLAGS

LAYOUT_BYTES = {"nv12": lambda w, h: w * h * 3 // 2, "bgra": lambda w, h: w * h * 4}


def pipe_pair(frame_bytes: int):
    size = min(max(2 * frame_bytes, 4 << 20), 64 << 20)
    read_handle, write_handle = _winapi.CreatePipe(None, size)
    return read_handle, write_handle, size


def consume(read_handle: int, frame_bytes: int, frames_expected: int | None) -> tuple[int, float, float]:
    """Reads whole frames, at most 16 MiB per call; returns (frames, seconds, seconds blocked in reads)."""
    fd = msvcrt.open_osfhandle(read_handle, os.O_RDONLY)
    buf = bytearray(frame_bytes)
    view = memoryview(buf)
    frames, blocked = 0, 0.0
    start = None
    with open(fd, "rb", buffering=0) as f:
        while True:
            got = 0
            while got < frame_bytes:
                t = time.perf_counter()
                n = f.readinto(view[got:got + min(frame_bytes - got, 16 << 20)])
                blocked += time.perf_counter() - t
                if not n:
                    return frames, (time.perf_counter() - start) if start else 0.0, blocked
                if start is None:
                    start = time.perf_counter()
                    blocked = 0.0
                got += n
            frames += 1
            if frames_expected and frames >= frames_expected:
                return frames, time.perf_counter() - start, blocked


def run_ffmpeg_into_pipe(args: list[str], frame_bytes: int) -> dict:
    read_handle, write_handle, size = pipe_pair(frame_bytes)
    os.set_handle_inheritable(write_handle, True)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESTDHANDLES
    startup.hStdOutput = write_handle
    startup.hStdInput = _winapi.GetStdHandle(_winapi.STD_INPUT_HANDLE)
    startup.hStdError = _winapi.GetStdHandle(_winapi.STD_ERROR_HANDLE)
    startup.lpAttributeList = {"handle_list": [write_handle]}
    launched = time.perf_counter()
    proc = subprocess.Popen([str(FFMPEG), "-v", "error", *args, "pipe:1"], startupinfo=startup, close_fds=True,
                            creationflags=FLAGS)
    _winapi.CloseHandle(write_handle)
    frames, seconds, blocked = consume(read_handle, frame_bytes, None)
    proc.wait()
    return dict(pipe_bytes=size, frames=frames, seconds=seconds, blocked_seconds=blocked,
                wall_from_launch=time.perf_counter() - launched)


def run_thread(frame_bytes: int, frames: int) -> dict:
    read_handle, write_handle, size = pipe_pair(frame_bytes)
    payload = os.urandom(1 << 20) * (frame_bytes // (1 << 20)) + os.urandom(frame_bytes % (1 << 20))

    def writer():
        fd = msvcrt.open_osfhandle(write_handle, 0)
        with open(fd, "wb", buffering=0) as w:
            for _ in range(frames):
                view = memoryview(payload)
                while view:
                    n = w.write(view)
                    view = view[n:]

    t = threading.Thread(target=writer)
    t.start()
    got, seconds, blocked = consume(read_handle, frame_bytes, frames)
    t.join()
    return dict(pipe_bytes=size, frames=got, seconds=seconds, blocked_seconds=blocked)


def summarise(name: str, layout: str, w: int, h: int, r: dict) -> dict:
    fb = LAYOUT_BYTES[layout](w, h)
    ms = 1000 * r["seconds"] / r["frames"] if r["frames"] else None
    return dict(mode=name, layout=layout, width=w, height=h, frame_bytes=fb, **r, ms_per_frame=ms,
                gb_per_s=fb * r["frames"] / r["seconds"] / 1e9 if r["seconds"] else None,
                blocked_ms_per_frame=1000 * r["blocked_seconds"] / r["frames"] if r["frames"] else None)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--clip", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--raw-frames", type=int, default=60)
    parser.add_argument("--loops", type=int, default=4)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    w, h = 3840, 2160
    args.work.mkdir(parents=True, exist_ok=True)
    rows = []
    raws = {}
    for layout in ("nv12", "bgra"):
        raw = args.work / f"4k-{args.raw_frames}.{layout}.yuv"
        if not raw.exists():
            subprocess.check_call([str(FFMPEG), "-v", "error", "-y", "-i", str(args.clip), "-frames:v",
                                   str(args.raw_frames), "-pix_fmt", layout, "-f", "rawvideo", str(raw)],
                                  creationflags=FLAGS)
        raws[layout] = raw
    for rep in range(args.repeats):
        for layout in ("nv12", "bgra"):
            fb = LAYOUT_BYTES[layout](w, h)
            r = run_ffmpeg_into_pipe(["-f", "rawvideo", "-pix_fmt", layout, "-s", f"{w}x{h}", "-r", "30",
                                      "-stream_loop", str(args.loops), "-i", str(raws[layout]), "-f", "rawvideo",
                                      "-pix_fmt", layout], fb)
            rows.append(dict(rep=rep, **summarise("ffmpeg-raw", layout, w, h, r)))
            print(json.dumps(rows[-1]), flush=True)
            r = run_thread(fb, args.raw_frames * (args.loops + 1))
            rows.append(dict(rep=rep, **summarise("thread", layout, w, h, r)))
            print(json.dumps(rows[-1]), flush=True)
            # A real decode of the clip into the pipe, against the same decode into nothing.
            r = run_ffmpeg_into_pipe(["-i", str(args.clip), "-f", "rawvideo", "-pix_fmt", layout], fb)
            rows.append(dict(rep=rep, **summarise("decode-pipe", layout, w, h, r)))
            print(json.dumps(rows[-1]), flush=True)
            t = time.perf_counter()
            subprocess.check_call([str(FFMPEG), "-v", "error", "-i", str(args.clip), "-pix_fmt", layout, "-f",
                                   "null", "-"], creationflags=FLAGS)
            seconds = time.perf_counter() - t
            n = rows[-1]["frames"]
            rows.append(dict(rep=rep, **summarise("decode-null", layout, w, h,
                                                  dict(pipe_bytes=0, frames=n, seconds=seconds,
                                                       blocked_seconds=0.0, wall_from_launch=seconds))))
            print(json.dumps(rows[-1]), flush=True)
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "pipe_transport.json").write_text(json.dumps(rows, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
