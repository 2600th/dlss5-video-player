#!/bin/bash
# vmaf.sh <distorted: .mkv or .raw (BGRA 1920x1080)> <reference.mkv> <out.json>
# Prints: mean VMAF, first/last-10-frame means, PSNR-Y mean.
FF=/c/Users/User/Documents/GitHub/dlss5-video-player/external/ffmpeg/bin
dist=$1; ref=$2; out=$3
if [[ "$dist" == *.raw ]]; then
  size=${SIZE:-1920x1080}
  in=(-f rawvideo -pix_fmt bgra -s $size -r 30 -i "$dist")
  conv="scale=out_color_matrix=bt709:out_range=tv,format=yuv420p"
else
  in=(-i "$dist")
  conv="format=yuv420p"
fi
logpath=$(cygpath -m "$out")
$FF/ffmpeg -v error -nostdin "${in[@]}" -i "$ref" -lavfi "[0:v]$conv,setpts=N/30/TB[d];[1:v]format=yuv420p,setpts=N/30/TB[r];[d][r]libvmaf=log_fmt=json:log_path='${logpath}':n_threads=16:feature=name=psnr" -f null - || exit 1
python - "$out" <<'EOF'
import json,sys
d=json.load(open(sys.argv[1]))
v=[f['metrics']['vmaf'] for f in d['frames']]
p=[f['metrics'].get('psnr_y',0) for f in d['frames']]
n=len(v)
print(f"frames={n} vmaf={sum(v)/n:.2f} first10={sum(v[:10])/min(10,n):.2f} last10={sum(v[-10:])/min(10,n):.2f} f0={v[0]:.2f} psnr_y={sum(p)/n:.2f}")
EOF
