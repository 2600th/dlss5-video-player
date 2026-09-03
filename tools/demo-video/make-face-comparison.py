"""Make the unscaled same-frame figure: Python + Pillow + FFmpeg."""
import argparse
import subprocess
import tempfile
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

parser = argparse.ArgumentParser()
parser.add_argument('original', type=Path)
parser.add_argument('neural', type=Path)
parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[2] / 'docs/screenshots/current/face-comparison.png')
args = parser.parse_args()
figure = Image.new('RGB', (1460, 980), '#0b1117')
draw = ImageDraw.Draw(figure)
font = ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf', 24)
with tempfile.TemporaryDirectory() as temporary:
    for column, (path, label) in enumerate(((args.original, 'Original'), (args.neural, 'Cached neural'))):
        frame = Path(temporary) / f'{column}.png'
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-i', str(path), '-vf', "select='eq(n,3375)'", '-fps_mode', 'vfr', '-frames:v', '1', str(frame)], check=True)
        # Identical source-pixel crop; no resampling or tonal adjustment.
        figure.paste(Image.open(frame).crop((670, 0, 1370, 880)), (20 + column * 720, 80))
        draw.text((20 + column * 720, 25), label, fill='#f0f4f8', font=font)
args.output.parent.mkdir(parents=True, exist_ok=True)
figure.save(args.output)
print(args.output)
