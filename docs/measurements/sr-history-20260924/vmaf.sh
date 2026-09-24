#!/bin/bash
# vmaf.sh <distorted: .mkv or .raw (BGRA 1920x1080)> <reference.mkv> <out.json>
# Prints: mean VMAF, first/last-10-frame means, PSNR-Y mean, and the added
# temporal change: mean |Y_t - Y_(t-1)| of the distorted minus the reference's
# (8-bit luma levels; positive = the upscale added frame-to-frame change), and
# err_flicker: mean |e_t - e_(t-1)| where e = distorted - reference luma, the
# part of the frame-to-frame change that is error rather than content. A steady
# upscale's error moves with the picture and scores near 0 on a still; shimmer,
# crawling edges and re-sampled history score above it.
FF=${FF:-/c/Users/User/Documents/GitHub/dlss5-video-player/external/ffmpeg/bin}
dist=$1; ref=$2; out=$3
if [[ "$dist" == *.raw ]]; then
  in=(-f rawvideo -pix_fmt bgra -s ${SIZE:-1920x1080} -r 30 -i "$dist")
  conv="scale=out_color_matrix=bt709:out_range=tv,format=yuv420p"
else
  in=(-i "$dist"); conv="format=yuv420p"
fi
logpath=$(cygpath -m "$out")
$FF/ffmpeg -v error -nostdin "${in[@]}" -i "$ref" -lavfi "[0:v]$conv,setpts=N/30/TB[d];[1:v]format=yuv420p,setpts=N/30/TB[r];[d][r]libvmaf=log_fmt=json:log_path='${logpath}':n_threads=8:feature=name=psnr" -f null - || exit 1
yd="${out%.json}.y"; yr="${ref%.mkv}.y"
$FF/ffmpeg -v error -nostdin "${in[@]}" -vf "$conv,extractplanes=y" -f rawvideo "$yd" || exit 1
[ -f "$yr" ] || $FF/ffmpeg -v error -nostdin -i "$ref" -vf "format=yuv420p,extractplanes=y" -f rawvideo "$yr"
python - "$out" "$yd" "$yr" <<'PY'
import json,sys,numpy as np
d=json.load(open(sys.argv[1]))
v=[f['metrics']['vmaf'] for f in d['frames']]
p=[f['metrics'].get('psnr_y',0) for f in d['frames']]
n=len(v)
yd=np.fromfile(sys.argv[2],np.uint8).reshape(-1,1080,1920)
yr=np.fromfile(sys.argv[3],np.uint8).reshape(-1,1080,1920)
m=min(len(yd),len(yr))
fd=fr=ef=0.0
for t in range(1,m):
    a0,a1=yd[t-1].astype(np.float32),yd[t].astype(np.float32)
    r0,r1=yr[t-1].astype(np.float32),yr[t].astype(np.float32)
    fd+=float(np.abs(a1-a0).mean()); fr+=float(np.abs(r1-r0).mean())
    ef+=float(np.abs((a1-r1)-(a0-r0)).mean())
k=max(1,m-1); fd/=k; fr/=k; ef/=k
print(f"frames={n} vmaf={sum(v)/n:.2f} first10={sum(v[:10])/min(10,n):.2f} last10={sum(v[-10:])/min(10,n):.2f} f0={v[0]:.2f} psnr_y={sum(p)/n:.2f} flicker_added={fd-fr:+.3f} err_flicker={ef:.3f}")
PY
rm -f "$yd"
