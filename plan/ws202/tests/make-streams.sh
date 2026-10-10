#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Independently encoded media and reference outputs for the unfinished native decoder WS.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws202-streams"
work=$fresh_dir
for pair in stereo:44100:2 mono:48000:1 low:22050:2; do
    name=${pair%%:*}
    rest=${pair#*:}
    rate=${rest%%:*}
    channels=${rest##*:}
    ffmpeg -nostdin -v error -f lavfi -i "aevalsrc=0.12*sin(2*PI*997*t)+0.06*sin(2*PI*431*t):s=$rate:d=1.2" \
        -ac "$channels" -c:a aac -profile:a aac_low -b:a 128k "$work/aac-$name.m4a"
    ffmpeg -nostdin -v error -i "$work/aac-$name.m4a" -c:a copy -f adts "$work/aac-$name.aac"
    ffmpeg -nostdin -v error -i "$work/aac-$name.aac" -c:a pcm_f32le -f f32le "$work/aac-$name.f32"
done

# Abrupt attacks, high-frequency stereo and disabled PNS expose exact transform/tool errors.
for kind in transient intensity; do
    expr='0.4*sin(2*PI*2500*t)*lt(mod(t,0.09),0.007)|0.3*sin(2*PI*3100*t)*lt(mod(t,0.11),0.006)'
    bitrate=128k
    if [ "$kind" = intensity ]; then
        expr='0.3*sin(2*PI*7000*t)+0.12*sin(2*PI*990*t)|0.18*sin(2*PI*7000*t)+0.12*sin(2*PI*990*t)'
        bitrate=48k
    fi
    ffmpeg -nostdin -v error -f lavfi -i "aevalsrc='$expr':s=48000:d=1.2" \
        -c:a aac -profile:a aac_low -b:a "$bitrate" -aac_tns 1 -aac_pns 0 -aac_is 1 -f adts "$work/aac-$kind.aac"
    ffmpeg -nostdin -v error -i "$work/aac-$kind.aac" -c:a pcm_f32le -f f32le "$work/aac-$kind.f32"
done
ffmpeg -nostdin -v error -f lavfi -i 'testsrc2=size=320x180:rate=25:duration=2' \
    -f lavfi -i 'sine=frequency=997:sample_rate=48000:duration=2' \
    -c:v libx264 -profile:v high -pix_fmt yuv420p -x264-params 'bframes=3:ref=3:keyint=25:threads=1' \
    -c:a aac -profile:a aac_low -movflags +faststart -shortest "$work/h264-high-b-aac.mp4"
ffmpeg -nostdin -v error -i "$work/h264-high-b-aac.mp4" -an -c:v copy -bsf:v h264_mp4toannexb -f h264 "$work/h264-high-b.h264"
ffmpeg -nostdin -v error -i "$work/h264-high-b-aac.mp4" -an -pix_fmt nv12 -f rawvideo "$work/h264-high-b.nv12"
ffprobe -v error -select_streams v -show_entries frame=pts,pkt_dts,pict_type,coded_picture_number -of json "$work/h264-high-b-aac.mp4" > "$work/h264-high-b-frames.json"
printf '%s\n' "$work"
