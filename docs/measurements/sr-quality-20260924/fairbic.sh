#!/bin/bash
# fairbic.sh <clip>: bicubic of the decoder's BGRA frames, scored through the same
# BGRA->YUV path the SR capture takes.
cd /c/t/w4-sr/sr
FF=/c/Users/User/Documents/GitHub/dlss5-video-player/external/ffmpeg/bin
c=$1
$FF/ffmpeg -v error -y -i ${c}_540.mkv -vf "scale=in_color_matrix=bt709:in_range=tv,format=bgra,scale=1920:1080:flags=bicubic" -f rawvideo fb_$c.raw
echo -n "fair bicubic $c: "; bash vmaf.sh fb_$c.raw ${c}_ref.mkv fairbic_$c.json 2>/dev/null
rm -f fb_$c.raw
