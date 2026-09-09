"""Make an unscaled same-frame original/neural figure: Python + Pillow + FFmpeg.

Both crops come from the identical source pixels of the identical frame; there is
no resampling, retouching or tonal adjustment. The neural side is a real render
produced by the shipping worker, so `--neural-frame` is the frame index inside
that render, which usually starts at the render range's first frame rather than
at the source's.

    python make-face-comparison.py original.mp4 neural.mkv \
        --frame 3375 --crop 670,0,700,880 --output docs/screenshots/current/face-comparison.png
"""
import argparse
import subprocess
import tempfile
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

parser = argparse.ArgumentParser()
parser.add_argument('original', type=Path)
parser.add_argument('neural', type=Path)
parser.add_argument('--frame', type=int, default=3375, help='zero-based frame in the original')
parser.add_argument('--neural-frame', type=int, default=None,
                    help='zero-based frame in the render; defaults to --frame')
parser.add_argument('--crop', default='670,0,700,880', help='x,y,width,height in source pixels')
parser.add_argument('--caption', default='', help='line drawn under the two crops')
parser.add_argument('--output', type=Path,
                    default=Path(__file__).resolve().parents[2] / 'docs/screenshots/current/face-comparison.png')
args = parser.parse_args()

x, y, width, height = (int(value) for value in args.crop.split(','))
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
frames = (args.frame, args.frame if args.neural_frame is None else args.neural_frame)
with tempfile.TemporaryDirectory() as temporary:
    for column, (path, label, index) in enumerate(((args.original, 'Original', frames[0]),
                                                   (args.neural, 'Neural rendered', frames[1]))):
        frame = Path(temporary) / f'{column}.png'
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-i', str(path),
                        '-vf', f"select='eq(n,{index})'", '-fps_mode', 'vfr', '-frames:v', '1', str(frame)],
                       check=True)
        # Identical source-pixel crop; no resampling or tonal adjustment.
        figure.paste(Image.open(frame).crop((x, y, x + width, y + height)), (20 + column * (width + 20), 80))
        draw.text((20 + column * (width + 20), 25), label, fill='#f0f4f8', font=font)
for row, line in enumerate(caption_lines):
    draw.text((20, height + 92 + row * 24), line, fill='#93a1b1', font=small)
args.output.parent.mkdir(parents=True, exist_ok=True)
figure.save(args.output)
print(args.output)
