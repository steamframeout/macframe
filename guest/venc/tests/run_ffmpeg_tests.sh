#!/bin/bash
# Encodes test patterns with FFmpeg's Vulkan encoders through the layer (lavapipe), decodes, checks PSNR.
#   FF: an ffmpeg >= 7.1 with Vulkan encoders; VENC_LAYER_DIR: a directory with the layer's json (make check).
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
FF=${FF:-/home/claude/dl/ffmpeg-inst/bin/ffmpeg}
OUT=${OUT:-/tmp/venc-tests}
MIN_PSNR=${MIN_PSNR:-40}
mkdir -p "$OUT"
export VK_LAYER_PATH=${VENC_LAYER_DIR:-$HERE/build} VK_INSTANCE_LAYERS=VK_LAYER_STEAMAC_video_encode
export VENC_BACKEND=${VENC_BACKEND:-x26x}
export STEAMAC_VENC=1   # the layer serves SteamVR's processes by default; tests opt in
# FFmpeg 7.1 uploads from imported host memory without waiting for the copy before reusing the source frame;
# visible here because the layer's encode runs asynchronously. Not a layer issue: hide the extension.
export VENC_HIDE_EXT=VK_EXT_external_memory_host
fail=0
t() {   # name codec size min_psnr ffmpeg-args...
    local name=$1 codec=$2 size=$3 minp=$4; shift 4
    local ext=h264; [ "$codec" = hevc_vulkan ] && ext=h265
    timeout -s KILL 180 "$FF" -hide_banner -y -loglevel warning -init_hw_device vulkan=vk:0 -filter_hw_device vk \
        -f lavfi -i testsrc2=size=$size:rate=30 -frames:v 45 -vf format=nv12,hwupload -c:v $codec -bf 0 "$@" \
        "$OUT/$name.$ext" > "$OUT/$name.log" 2>&1
    local rc=$?
    local warn; warn=$("$FF" -hide_banner -loglevel warning -i "$OUT/$name.$ext" -f null - 2>&1 | wc -l)
    "$FF" -hide_banner -loglevel error -i "$OUT/$name.$ext" -f lavfi -i testsrc2=size=$size:rate=30 -lavfi \
        "[0:v]format=yuv420p,settb=1/30,setpts=N[a];[1:v]format=yuv420p,trim=end_frame=45,settb=1/30,setpts=N[b];[a][b]psnr=stats_file=$OUT/$name.ps" \
        -f null - 2>/dev/null
    local res; res=$(awk -v m="$minp" '{split($6,a,":"); s+=a[2]; if(min==""||a[2]<min)min=a[2]} END {printf "%d %.2f %.2f %s", NR, s/(NR?NR:1), min, (NR==45 && min>=m)?"ok":"FAIL"}' "$OUT/$name.ps" 2>/dev/null)
    local verdict=${res##* }
    [ $rc -ne 0 ] || [ "$warn" != 0 ] && verdict=FAIL
    printf '%-14s exit=%s decoder-warnings=%s frames/avg/min PSNR: %s\n' "$name" "$rc" "$warn" "$res"
    [ "$verdict" = ok ] || fail=1
}
t h264-cabac  h264_vulkan 640x480 $MIN_PSNR -g 20
t h264-cavlc  h264_vulkan 640x480 $MIN_PSNR -coder vlc
t h264-cbr    h264_vulkan 640x480 35 -rc_mode cbr -b:v 1M
t h264-vbr    h264_vulkan 640x480 $MIN_PSNR -rc_mode vbr -b:v 2M -maxrate 4M
t h264-1080p  h264_vulkan 1920x1080 $MIN_PSNR
t h264-odd    h264_vulkan 1000x562 $MIN_PSNR -profile:v main
t h264-cbase  h264_vulkan 640x480 $MIN_PSNR -profile:v constrained_baseline
t h265        hevc_vulkan 640x480 $MIN_PSNR -g 20
t h265-cbr    hevc_vulkan 640x480 35 -rc_mode cbr -b:v 1M
t h265-1080p  hevc_vulkan 1920x1080 $MIN_PSNR
t h265-odd    hevc_vulkan 1000x562 $MIN_PSNR
# a process that did not opt in sees the driver unchanged: no video encode
if STEAMAC_VENC= timeout -s KILL 60 "$FF" -hide_banner -loglevel error -init_hw_device vulkan=vk:0 -filter_hw_device vk \
    -f lavfi -i testsrc2=size=320x240:rate=30 -frames:v 2 -vf format=nv12,hwupload -c:v h264_vulkan -f null - \
    > "$OUT/optin.log" 2>&1; then
    echo "opt-in          FAIL: a process without STEAMAC_VENC got video encode"; fail=1
elif grep -q "does not support the VK_KHR_video_encode_queue" "$OUT/optin.log"; then
    echo "opt-in          ok (other processes see the driver unchanged)"
else
    echo "opt-in          FAIL: unexpected error"; cat "$OUT/optin.log"; fail=1
fi
exit $fail
