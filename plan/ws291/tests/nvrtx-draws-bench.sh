#!/bin/sh
# ws291 (NVIDIA): the nvrtx Vulkan executor's CPU cost of mview's --draws=each frames on the host, and each draw's GPU
# state (nvrtx-draws-bench.c).  TREE (default: this tree) gives the executor's sources, so two trees' runs can be
# compared: their snapshot files must be equal.
#   sh plan/ws291/tests/nvrtx-draws-bench.sh OUT [TREE] [COPIES] [FRAMES] [SUBMITS]   (from the top of the tree)
# OUT gets the binary (vk-bench), the SPIR-V and snapshots.txt (the first frame's draws); prints the timing line.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(pwd)
out=${1:?usage: nvrtx-draws-bench.sh OUT [TREE] [COPIES] [FRAMES] [SUBMITS]}
tree=${2:-$repo}
copies=${3:-1000}
frames=${4:-20}
submits=${5:-1}
mkdir -p "$out/client-headers"
[ -e "$out/client-headers/vulkan" ] || ln -s "$repo/include/libc/vulkan" "$out/client-headers/vulkan"
src=$tree/src/drivers/gpu/nvrtx
for source in "$repo/userland/desktop/wayland/shaders/quad.vert" "$repo/userland/desktop/wayland/shaders/quad.frag"; do
	name=$(basename "$source" | tr . -)
	glslc --target-env=vulkan1.0 -o "$out/$name.spv" "$source"
done
cp "$repo/userland/desktop/libvulkan/codec.c" "$out/client-codec.c"
cc -std=gnu11 -Os -g -flto -D_POSIX_C_SOURCE=200809L -DKERN_USER_ABI_LP64 -include time.h -Wall -Wextra -Werror -Wno-maybe-uninitialized -ffunction-sections -fdata-sections -Wl,--gc-sections \
	-I"$out/client-headers" -I"$tree/include" -I"$tree/src" -I"$tree" -I"$repo/userland/desktop/libvulkan" -I"$repo" \
	-o "$out/vk-bench" \
	"$repo/plan/ws291/tests/nvrtx-draws-bench.c" \
	"$src/vk/vk-object.c" "$src/vk/vk-session.c" "$src/vk/vk-stream.c" "$src/vk/vk-dispatch.c" "$src/vk/vk-device.c" \
	"$src/vk/vk-reply.c" "$src/vk/vk-memory.c" "$src/vk/vk-resource.c" "$src/vk/vk-layout.c" "$src/vk/vk-target.c" "$src/vk/vk-pipeline.c" \
	"$src/vk/vk-command.c" "$src/vk/vk-record.c" "$src/vk/vk-execute.c" "$src/vk/vk-queue.c" "$src/vk/exec-push.c" "$src/draw-push.c" "$src/wire.c" \
	"$src/vk/vk-video.c" "$src/vk/vk-video-h264.c" "$src/vk/exec-copy.c" "$src/nvdec-records.c" \
	"$src/compiler/sm75"/*.c "$tree/src/drivers/gpu/compiler/spirv.c" \
	"$tree/src/drivers/gpu/compiler/optimize.c" \
	"$tree/src/drivers/gpu/i915/vk/vk-codec.c" "$repo/userland/desktop/libvulkan/wire.c" "$out/client-codec.c" -lm
"$out/vk-bench" "$out" "$copies" "$frames" "$submits" "$out/snapshots.txt"
