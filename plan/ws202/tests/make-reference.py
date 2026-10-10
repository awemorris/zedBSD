#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Make independent visible-NV12 hash and normalized-PCM RMS reference rows."""
import argparse
from decimal import Decimal
import hashlib
import json
from pathlib import Path
import struct
import math

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directory", type=Path)
parser.add_argument("--name", default="h264-high-b")
parser.add_argument("--width", type=int, default=320)
parser.add_argument("--height", type=int, default=180)
parser.add_argument("--audio-name", default="h264-high-b-aac")
parser.add_argument("--nocts-name", default="h264-nocts")
args = parser.parse_args()
root = args.directory
frames = json.loads((root / f"{args.name}-frames.json").read_text())["frames"]
video = (root / f"{args.name}.nv12").read_bytes()
if args.width <= 0 or args.height <= 0 or args.width % 2 or args.height % 2:
    raise SystemExit("reference NV12 dimensions must be positive and even")
frame_size = args.width * args.height * 3 // 2
if len(video) != len(frames) * frame_size:
    raise SystemExit("reference picture count mismatch")
rows = []
for index, frame in enumerate(frames):
    timestamp = int(Decimal(frame["pts_time"]) * 1_000_000)
    digest = hashlib.sha256(video[index * frame_size:(index + 1) * frame_size]).hexdigest()
    rows.append(f"{timestamp} {digest}\n")
(root / f"{args.name}.video.sha256").write_text("".join(rows))
if args.nocts_name:
    packets = json.loads((root / f"{args.nocts_name}-packets.json").read_text())["packets"]
    clock = sorted(int(Decimal(packet["pts_time"]) * 1_000_000) for packet in packets)
    if len(clock) != len(rows):
        raise SystemExit("no-ctts packet/picture count mismatch")
    (root / f"{args.nocts_name}.video.sha256").write_text("".join(f"{time} {rows[index].split()[1]}\n" for index, time in enumerate(clock)))
if not args.audio_name:
    raise SystemExit(0)
pcm = (root / f"{args.audio_name}.s16").read_bytes()
if len(pcm) % 4:
    raise SystemExit("reference stereo PCM is incomplete")
samples = list(struct.iter_unpack("<hh", pcm))
rows = []
for offset in range(0, len(samples), 1024):
    block = samples[offset:offset + 1024]
    left = math.sqrt(sum(a * a for a, b in block) / len(block)) / 32768
    right = math.sqrt(sum(b * b for a, b in block) / len(block)) / 32768
    rows.append(f"RMS frame={offset} count={len(block)} left={left:.9f} right={right:.9f}\n")
(root / f"{args.audio_name}.rms").write_text("".join(rows))
