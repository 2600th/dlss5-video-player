#!/usr/bin/env python3
"""RTX VSR against DLSS Super Resolution and plain scalers on video, 960x540 -> 1920x1080.

    python docs/measurements/vsr-quality-20261002/run.py <work dir> [--frames N] [--clips a,b]

Everything is rebuilt from what the repository holds: the two committed clips,
lavfi sources, the bundled FFmpeg (external/ffmpeg/bin, libvmaf) and the
Release UpscalingGpuSmoke in build-upscaling. The sr-quality-20260924 report
described its clips in prose only; the recipes are code here.

Each clip is a 1080p30 BT.709 reference (FFV1) and two 960x540 inputs:
  clean  - area-reduced, lossless (the earlier report's input)
  coded  - the same, then H.264 CRF 30 (what a 540p stream actually carries)
VMAF and VMAF NEG (no enhancement gain: plain VMAF rewards sharpening, and
RTX VSR sharpens), luma PSNR, and error flicker. Every upscale is scored against the 1080p reference through the same
8-bit RGB -> BT.709 limited YUV 4:2:0 path the player's capture takes, so no
arm is handed the 3 VMAF the round trip costs (see sr-quality-20260924).
"""
import argparse, csv, json, os, subprocess, sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[3]
FF = REPO / 'external' / 'ffmpeg' / 'bin' / 'ffmpeg.exe'
SMOKE = REPO / 'build-upscaling' / 'Release' / 'UpscalingGpuSmoke.exe'
TAG = ['-color_primaries', 'bt709', '-color_trc', 'bt709', '-colorspace', 'bt709', '-color_range', 'tv']
BT709 = 'setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv'
W, H = 1920, 1080


def ff(*args, cwd):
    subprocess.run([str(FF), '-v', 'error', '-nostdin', '-y', *args], cwd=cwd, check=True)


def build_clips(work, frames):
    demo = str(REPO / 'tools' / 'benchmark' / 'fixtures' / 'demo-capture-20260912.mp4')
    ncd = str(REPO / 'docs' / 'media' / 'neural-comparison-demo.mp4')
    held = f'select=eq(n\\,0),loop=loop={frames - 1}:size=1:start=0,setpts=N/30/TB'
    pan = lambda src: ['-i', src, '-vf', f'select=eq(n\\,0),scale=2400:1350:flags=lanczos,loop=loop={frames - 1}:size=1:start=0,'
                       f"crop=1920:1080:n:135,setpts=N/30/TB,{BT709}"]
    recipes = {
        'demo': ['-i', demo, '-vf', f'scale={W}:{H}:flags=lanczos,{BT709}'],
        'ncd': ['-i', ncd, '-vf', f'scale={W}:{H}:flags=lanczos,{BT709}'],
        'still': ['-i', demo, '-vf', f'{held},scale={W}:{H}:flags=lanczos,{BT709}'],
        'pan': pan(ncd),
        'mpan': ['-f', 'lavfi', '-i', 'mandelbrot=size=2400x1350:rate=30', *pan('pipe:')[2:]],
        'ts': ['-f', 'lavfi', '-i', f'testsrc2=size={W}x{H}:rate=30', '-vf', BT709],
    }
    for name, recipe in recipes.items():
        ref = work / f'{name}_ref.mkv'
        if not ref.exists():
            if name == 'mpan':   # the mandelbrot still, then the same pan
                ff('-f', 'lavfi', '-i', 'mandelbrot=size=2400x1350:rate=30', '-frames:v', '1', 'mpan_still.png', cwd=work)
                recipe = pan('mpan_still.png')
            ff(*recipe, '-frames:v', str(frames), '-r', '30', *TAG, '-c:v', 'ffv1', ref.name, cwd=work)
        clean, coded = work / f'{name}_540.mkv', work / f'{name}_540c.mkv'
        if not clean.exists():
            ff('-i', ref.name, '-vf', f'scale=960:540:flags=area,{BT709}', *TAG, '-c:v', 'ffv1', clean.name, cwd=work)
        if not coded.exists():
            ff('-i', clean.name, *TAG, '-c:v', 'libx264', '-preset', 'medium', '-crf', '30', '-pix_fmt', 'yuv420p', coded.name, cwd=work)
    return list(recipes)


def scaler(work, src, out, flags):
    ff('-i', src, '-vf', f'scale=in_color_matrix=bt709:in_range=tv,format=bgra,scale={W}:{H}:flags={flags}',
       '-f', 'rawvideo', out, cwd=work)
    return 'bgra'


def dlss_sr(work, src, out, frames):
    r = subprocess.run([str(SMOKE), str(work / src), '1080', str(work / out), 'mv=1,depth=1', str(frames), 'per-frame'],
                       cwd=work, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f'DLSS SR failed ({r.returncode}): {r.stdout[-400:]}')
    return 'bgra'


def vsr(work, src, out, frames, quality):
    r = subprocess.run([str(SMOKE), 'vsr-capture', str(work / src), str(W), str(H), str(quality), str(work / out), str(frames)],
                       cwd=work, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f'VSR q{quality} failed ({r.returncode}): {r.stdout[-400:]}')
    ms = [t.split('=')[1] for t in r.stdout.split() if t.startswith('meanGpuMs=')]
    return 'rgba', float(ms[-1]) if ms else float('nan')


def luma(work, args, out):
    ff(*args, '-vf', 'scale=out_color_matrix=bt709:out_range=tv,format=yuv420p,extractplanes=y', '-f', 'rawvideo', out, cwd=work)
    return np.fromfile(work / out, np.uint8).reshape(-1, H, W)


def score(work, raw, pix, ref):
    log = raw + '.json'
    inp = ['-f', 'rawvideo', '-pix_fmt', pix, '-s', f'{W}x{H}', '-r', '30', '-i', raw]
    ff(*inp, '-i', ref, '-lavfi',
       '[0:v]scale=out_color_matrix=bt709:out_range=tv,format=yuv420p,setpts=N/30/TB[d];'
       f"[1:v]format=yuv420p,setpts=N/30/TB[r];[d][r]libvmaf=log_fmt=json:log_path={log}:n_threads=16:feature=name=psnr:"
       r"model='version=vmaf_v0.6.1\:name=vmaf|version=vmaf_v0.6.1neg\:name=vmaf_neg'",
       '-f', 'null', '-', cwd=work)
    frames = json.load(open(work / log))['frames']
    v = [f['metrics']['vmaf'] for f in frames]
    neg = [f['metrics']['vmaf_neg'] for f in frames]
    p = [f['metrics'].get('psnr_y', 0.0) for f in frames]
    yd = luma(work, inp, raw + '.y')
    yr_path = work / (ref + '.y')
    yr = np.fromfile(yr_path, np.uint8).reshape(-1, H, W) if yr_path.exists() else luma(work, ['-i', ref], ref + '.y')
    m = min(len(yd), len(yr))
    # Error flicker: mean |e_t - e_(t-1)|, e = upscale - original luma. Zero for an
    # upscale whose error moves with the picture; shimmer and crawling edges raise it.
    ef = float(np.mean([np.abs((yd[t].astype(np.float32) - yr[t]) - (yd[t - 1].astype(np.float32) - yr[t - 1])).mean()
                        for t in range(1, m)])) if m > 1 else 0.0
    os.remove(work / (raw + '.y'))
    return sum(v) / len(v), sum(neg) / len(neg), v[0], sum(p) / len(p), ef


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('work')
    ap.add_argument('--frames', type=int, default=60)
    ap.add_argument('--clips', default='')
    a = ap.parse_args()
    work = Path(a.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    clips = build_clips(work, a.frames)
    if a.clips:
        clips = [c for c in clips if c in a.clips.split(',')]
    rows = []
    for clip in clips:
        ref = f'{clip}_ref.mkv'
        for variant, src in (('clean', f'{clip}_540.mkv'), ('coded', f'{clip}_540c.mkv')):
            arms = [('bicubic', lambda o: (scaler(work, src, o, 'bicubic'), None)),
                    ('lanczos', lambda o: (scaler(work, src, o, 'lanczos'), None)),
                    ('dlss-sr', lambda o: (dlss_sr(work, src, o, a.frames), None))]
            arms += [(f'vsr-q{q}', (lambda q: lambda o: vsr(work, src, o, a.frames, q))(q)) for q in (1, 2, 3, 4)]
            for arm, run in arms:
                raw = f'{clip}_{variant}_{arm}.raw'
                pix, ms = run(raw)
                vmaf, neg, f0, psnr, ef = score(work, raw, pix, ref)
                os.remove(work / raw)
                rows.append(dict(clip=clip, input=variant, arm=arm, vmaf=round(vmaf, 2), vmaf_neg=round(neg, 2), f0=round(f0, 2),
                                 psnr_y=round(psnr, 2), err_flicker=round(ef, 3), gpu_ms='' if ms is None else round(ms, 3)))
                print(f'{clip:6} {variant:5} {arm:8} vmaf={vmaf:6.2f} neg={neg:6.2f} f0={f0:6.2f} psnr_y={psnr:6.2f} err_flicker={ef:.3f}'
                      + ('' if ms is None else f' gpu_ms={ms:.3f}'), flush=True)
    with open(work / 'results.csv', 'w', newline='') as out:
        writer = csv.DictWriter(out, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(f'wrote {work / "results.csv"}')


if __name__ == '__main__':
    sys.exit(main())
