"""Make an unscaled same-frame original/DLSS 5 figure: Python + Pillow + FFmpeg.

Both crops come from the identical source pixels of the identical frame; there is
no resampling, retouching or tonal adjustment. The DLSS 5 side is a real render
produced by the shipping worker, so `--neural-frame` is the frame index inside
that render, which usually starts at the render range's first frame rather than
at the source's.

    python make-face-comparison.py original.mp4 neural.mkv \
        --frame 3375 --crop 670,0,700,880 --output docs/screenshots/current/face-comparison.png

The second argument can also be a render folder copied out of the player's cache
(renders/<key>/ with neural.mkv, manifest.json and neural-settings.ini). Then the
frame offset comes from the manifest, the pair is checked by picture (it must
differ least at that offset), and `--provenance` writes a JSON record beside the
figure: the source and render digests, the frame and its timestamp, the neural
settings and their digest, the runtime and the player that made it.
"""
import argparse
import datetime
import hashlib
import json
import subprocess
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import numpy as np

REPO = Path(__file__).resolve().parents[2]
BIN = REPO / 'external' / 'ffmpeg' / 'bin'
parser = argparse.ArgumentParser()
parser.add_argument('original', type=Path)
parser.add_argument('neural', type=Path, help='neural.mkv, or the render folder that holds it')
parser.add_argument('--frame', type=int, default=3375, help='zero-based frame in the original')
parser.add_argument('--neural-frame', type=int, default=None,
                    help='zero-based frame in the render; defaults to --frame, or the manifest offset for a folder')
parser.add_argument('--crop', default='670,0,700,880', help='x,y,width,height in source pixels')
parser.add_argument('--caption', default='', help='line drawn under the two crops')
parser.add_argument('--ffmpeg-bin', type=Path, default=BIN, help='folder with ffmpeg.exe and ffprobe.exe')
parser.add_argument('--provenance', type=Path, default=None, help='write a JSON record here (render folder only)')
parser.add_argument('--source-url', default='', help='where the source came from, for the record')
parser.add_argument('--title', default='', help='what the source is, for the record')
parser.add_argument('--gpu', default='', help='the GPU that rendered it, for the record')
parser.add_argument('--player-commit', default='', help='the commit the rendering player was built from; defaults to HEAD')
parser.add_argument('--cache-key', default='', help="the render's folder name in the cache, when it was copied out under another")
parser.add_argument('--output', type=Path,
                    default=REPO / 'docs/screenshots/current/face-comparison.png')
args = parser.parse_args()
FF = str(args.ffmpeg_bin / 'ffmpeg.exe') if (args.ffmpeg_bin / 'ffmpeg.exe').exists() else 'ffmpeg'
PROBE = str(args.ffmpeg_bin / 'ffprobe.exe') if (args.ffmpeg_bin / 'ffprobe.exe').exists() else 'ffprobe'

x, y, width, height = (int(value) for value in args.crop.split(','))
render_dir = args.neural if args.neural.is_dir() else None
neural_path = render_dir / 'neural.mkv' if render_dir else args.neural
manifest = json.loads((render_dir / 'manifest.json').read_text()) if render_dir else None
if manifest and (manifest.get('state') != 'complete' or manifest['verifiedNeuralFrames'] != manifest['frameCount']):
    raise SystemExit('the render is not a complete, fully verified cache entry')
rate = subprocess.run([PROBE, '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=r_frame_rate',
                       '-of', 'csv=p=0', str(args.original)], capture_output=True, text=True, check=True).stdout.strip()
fps = float(eval(rate))  # "30/1", "60000/1001": ffprobe's own fraction
offset = round(manifest['rangeStart100ns'] / 1e7 * fps) if manifest else 0
neural_frame = args.neural_frame if args.neural_frame is not None else args.frame - offset


def decode(path, index):
    raw = subprocess.run([FF, '-v', 'error', '-i', str(path), '-vf', f"select='eq(n,{index})'", '-fps_mode', 'vfr',
                          '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], capture_output=True, check=True).stdout
    probe = subprocess.run([PROBE, '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=width,height',
                            '-of', 'csv=p=0', str(path)], capture_output=True, text=True, check=True).stdout.strip()
    w, h = (int(v) for v in probe.split(','))
    return np.frombuffer(raw, np.uint8).reshape(h, w, 3)


source = decode(args.original, args.frame)
neural = decode(neural_path, neural_frame)
alignment = None
if render_dir:
    # The pair must differ least at the offset the manifest implies, or it is not one frame.
    mad = {d: float(np.abs(source.astype(np.int16) - (neural if d == 0 else decode(neural_path, neural_frame + d)).astype(np.int16)).mean())
           for d in (-1, 0, 1)}
    if min(mad, key=mad.get) != 0:
        raise SystemExit(f'pair is not aligned at render frame {neural_frame}: {mad}')
    alignment = {f'{d:+d}': round(v, 3) for d, v in mad.items()}

figure_width = width * 2 + 60
font = ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf', 24)
small = ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf', 17)


def wrap(text, cap_font, limit):
    """Greedy word wrap, so a long provenance line is never clipped."""
    lines, current = [], ''
    for word in text.split():
        candidate = f'{current} {word}'.strip()
        if current and cap_font.getlength(candidate) > limit:
            lines.append(current)
            current = word
        else:
            current = candidate
    if current:
        lines.append(current)
    return lines


caption_lines = wrap(args.caption, small, figure_width - 40) if args.caption else []
caption_height = 12 + 24 * len(caption_lines)
figure = Image.new('RGB', (figure_width, height + 100 + caption_height), '#0b1117')
draw = ImageDraw.Draw(figure)
for column, (picture, label) in enumerate(((source, 'Original'), (neural, 'DLSS 5'))):
    # Identical source-pixel crop; no resampling or tonal adjustment.
    figure.paste(Image.fromarray(picture[y:y + height, x:x + width]), (20 + column * (width + 20), 80))
    draw.text((20 + column * (width + 20), 25), label, fill='#f0f4f8', font=font)
for row, line in enumerate(caption_lines):
    draw.text((20, height + 92 + row * 24), line, fill='#93a1b1', font=small)
args.output.parent.mkdir(parents=True, exist_ok=True)
figure.save(args.output)
print(args.output)

if args.provenance:
    if not render_dir:
        raise SystemExit('--provenance needs the render folder, not a bare neural.mkv')

    def sha256(path):
        digest = hashlib.sha256()
        with open(path, 'rb') as handle:
            for block in iter(lambda: handle.read(1 << 22), b''):
                digest.update(block)
        return digest.hexdigest()

    settings = {}
    for line in (render_dir / 'neural-settings.ini').read_text().splitlines():
        key, _, value = line.partition('=')
        if key in ('NRIntensity', 'NRLocalTone', 'NRLocalStructure', 'NRSkinStructure', 'NRColorStrength', 'NRPreset',
                   'NRStyle', 'NRAutoMask', 'NRPasses', 'NRChainedHistory', 'NRPreUpscale'):
            settings[key] = value
    lock = json.loads((REPO / 'packaging' / 'runtime-lock.json').read_text())
    seconds = args.frame / fps
    commit = args.player_commit or subprocess.run(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    record = {
        'figure': args.output.name,
        'source': {'title': args.title, 'url': args.source_url, 'file': args.original.name,
                   'sha256': sha256(args.original), 'size': f'{source.shape[1]}x{source.shape[0]}', 'fps': rate},
        'frame': args.frame,
        'timestampApprox': f'{int(seconds // 3600)}:{int(seconds % 3600 // 60):02d}:{seconds % 60:06.3f}',
        'crop': {'x': x, 'y': y, 'width': width, 'height': height, 'scaled': False},
        'render': {'cacheKey': args.cache_key or render_dir.name, 'frame': neural_frame, 'neuralSha256': sha256(neural_path),
                   'verifiedFrames': f"{manifest['verifiedNeuralFrames']} / {manifest['frameCount']}",
                   'encoder': manifest['encoder'], 'rangeStart100ns': manifest['rangeStart100ns'],
                   'alignmentMad': alignment},
        'settingsDigest': manifest['settingsDigest'],
        'settings': settings,
        'runtime': {'version': lock['runtimeVersion'], 'digest': manifest['runtimeDigest'],
                    'models': manifest['environment']['models']},
        'player': {'version': manifest['environment']['application'], 'commit': commit},
        'machine': {'gpu': args.gpu, 'driver': manifest['environment']['driver']},
        'made': datetime.date.today().isoformat(),
        'processing': 'Both crops are the same source pixels of the same frame, unscaled. Nothing retouched, sharpened or colour-corrected.',
    }
    args.provenance.parent.mkdir(parents=True, exist_ok=True)
    args.provenance.write_text(json.dumps(record, indent=2) + '\n')
    print(args.provenance)
