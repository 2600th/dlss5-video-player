"""Build public/ for the composition from two trailers, their renders and player captures.

Every source/render input is a matched pair cut from a source file and the render
the player wrote for that file (a cache entry: renders/<key>/neural.mkv beside its
manifest.json). A render starts at its range's first frame, so source frame n is
render frame n - offset, offset = round(rangeStart * fps) from the manifest. The
offset is then checked by picture: the pair must differ least at that offset
than one frame either side, or the script stops.

Both sides get the same frame indices and the same crop. Stills are lossless
PNGs of the whole 2560x1440 frame; clips are one H.264 CRF 10 encode each. No
scaling, retouching or tonal adjustment happens here.

The player captures (the Difference, Side by side and loupe views, the PNG the player's Save
comparison image wrote for its 2 x 2 view, and the
render band filling during a live session) are window captures made with
tools/verification/capture-window.ps1. They are copied as they are: PNG in,
PNG out.

    python prepare-inputs.py --bond SOURCE RENDER_DIR --re SOURCE RENDER_DIR \
                             --gta6 SOURCE RENDER_DIR --captures CAPTURE_DIR

Needs Python with numpy and Pillow, and FFmpeg (the repository's
external/ffmpeg/bin is used when present, or --ffmpeg-bin).
"""
import argparse, json, shutil, subprocess
from pathlib import Path
import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
FPS = {'bond': 60.0, 'gta6': 30.0}

# name, source frame: whole-frame stills
STILLS = [('bond', 1122)]
# name, clip, first frame, count, crop x,y,w,h
CLIPS = [('gta6', 'lucia', 1896, 84, (130, 150, 1920, 1080))]
# The captures the composition uses, by the names capture-window gave them.
CAPTURES = ['difference.png', 'side-by-side.png', 'loupe.png', 'saved-2x2.png']
BAND_FRAMES = 'band-*.png'

parser = argparse.ArgumentParser()
for name in FPS:
    parser.add_argument(f'--{name}', nargs=2, type=Path, required=True, metavar=('SOURCE', 'RENDER_DIR'))
parser.add_argument('--captures', type=Path, required=True)
parser.add_argument('--ffmpeg-bin', type=Path, default=REPO / 'external' / 'ffmpeg' / 'bin')
args = parser.parse_args()
FF = str(args.ffmpeg_bin / 'ffmpeg.exe') if (args.ffmpeg_bin / 'ffmpeg.exe').exists() else 'ffmpeg'


def frame(path, index):
    raw = subprocess.run([FF, '-v', 'error', '-i', str(path), '-vf', f"select='eq(n,{index})'", '-fps_mode', 'vfr',
                          '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(1440, 2560, 3)


def offset_of(name, source, render, probe):
    manifest = json.loads((render / 'manifest.json').read_text())
    if manifest.get('state') != 'complete' or manifest['verifiedNeuralFrames'] != manifest['frameCount']:
        raise SystemExit(f'{name}: the render is not a complete, fully verified entry')
    offset = round(manifest['rangeStart100ns'] / 1e7 * FPS[name])
    a = frame(source, probe).astype(np.int16)
    mad = {d: np.abs(a - frame(render / 'neural.mkv', probe - offset + d).astype(np.int16)).mean() for d in (-1, 0, 1)}
    if min(mad, key=mad.get) != 0:
        raise SystemExit(f'{name}: pair is not aligned at offset {offset}: {mad}')
    print(f'{name}: offset {offset}, aligned ({", ".join(f"{d:+d}={v:.2f}" for d, v in mad.items())})')
    return offset


inputs = {name: getattr(args, name) for name in FPS}
out = HERE / 'public'
if out.exists():
    shutil.rmtree(out)
out.mkdir()
for font in (REPO / 'site' / 'src' / 'assets' / 'fonts').glob('*.woff2'):
    shutil.copy2(font, out / font.name)

probes = {name: n for name, n in STILLS} | {name: first for name, _, first, _, _ in CLIPS}
offsets = {name: offset_of(name, *inputs[name], probes[name]) for name in inputs}
for name, n in STILLS:
    source, render = inputs[name]
    Image.fromarray(frame(source, n)).save(out / f'{name}-{n}-original.png')
    Image.fromarray(frame(render / 'neural.mkv', n - offsets[name])).save(out / f'{name}-{n}-neural.png')
for name, clip, first, count, (x, y, w, h) in CLIPS:
    source, render = inputs[name]
    for path, start, side in ((source, first, 'original'), (render / 'neural.mkv', first - offsets[name], 'neural')):
        subprocess.run([FF, '-v', 'error', '-y', '-i', str(path), '-vf',
                        f"select='between(n,{start},{start + count - 1})',setpts=N/FRAME_RATE/TB,crop={w}:{h}:{x}:{y}",
                        '-an', '-c:v', 'libx264', '-preset', 'slow', '-crf', '10', '-pix_fmt', 'yuv420p',
                        '-r', str(FPS[name]), str(out / f'{clip}-{side}.mp4')], check=True)
for capture in CAPTURES + sorted(p.name for p in args.captures.glob(BAND_FRAMES)):
    shutil.copy2(args.captures / capture, out / capture)
print('public/ ready:', len(list(out.iterdir())), 'files')
