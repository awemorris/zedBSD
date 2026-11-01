#!/bin/sh
# Host check of the PowerVR executor's NV12 images, plane views and copies,
# external memory queries, result status queries and video reports
# (ws322-p003): the pure parts (state-host-test.c over image-state.c and
# video-state.c), then the executor fed with libvulkan-encoded frames
# (nv12-host-test.c, which runs plan/tools/powervr/p009's executor check
# first).
#   plan/ws322/tests/p003/run.sh OUTDIR
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
root=$(cd -- "$(dirname -- "$0")/../../../.." && pwd)
here=$root/plan/ws322/tests/p003
p009=$root/plan/tools/powervr/p009
out=$1
mkdir -p "$out/inc" "$out/vk"
p="$root/src/drivers/gpu/powervr"
cc -std=c11 -O2 -Wall -Wextra -Werror -I"$root/include" -I"$root/src" \
	"$p/image-state.c" "$p/video-state.c" "$here/state-host-test.c" -o "$out/state-host-test"
"$out/state-host-test"
for shader in "$p009"/shaders/*.comp; do
	name=$(basename "$shader" .comp)
	glslc --target-env=vulkan1.0 -o "$out/$name.spv" "$shader"
done
ln -sfn "$root/include/libc/vulkan" "$out/inc/vulkan"
for file in wire codec; do
	cc -std=c11 -O1 -Wall -Wno-unused-parameter -I"$root" -I"$root/include" -I"$out/inc" -I"$root/userland/desktop/libvulkan" \
		-c "$root/userland/desktop/libvulkan/$file.c" -o "$out/vk/lib-$file.o"
done
for file in "$p009/frame-host.c" "$here/nv12-frames.c"; do
	name=$(basename "$file" .c)
	cc -std=c11 -O1 -Wall -Wextra -Werror -DPOWERVR_FORMAT_LINEAR_SAMPLED=0 -I"$root" -I"$root/include" -I"$out/inc" \
		-I"$root/userland/desktop/libvulkan" -I"$p009" -c "$file" -o "$out/vk/$name.o"
done
objects=""
for file in vk/vk-object vk/vk-stream vk/vk-device vk/vk-memory vk/vk-image vk/vk-query video-state image-state descriptor-set data-program vk/vk-layout vk/vk-command vk/vk-queue vk/vk-pass vk/vk-transfer vk/vk-draw render-draw render-pass render-program transfer-program compiler/shader-spirv compiler/shader-spirv-interface compiler/shader-spirv-control compiler/shader-spirv-instruction compiler/shader-backend compute-program render-sr-program compiler/usc-encode compiler/pds-encode; do
	name=${file##*/}
	cc -std=c11 -O1 -Wall -Wextra -Werror -DPOWERVR_FORMAT_LINEAR_SAMPLED=0 -I"$root/include" -I"$root/src" \
		-c "$p/$file.c" -o "$out/vk/$name.o"
	objects="$objects $out/vk/$name.o"
done
cc -std=c11 -O1 -Wall -Wextra -Werror -I"$root/include" -I"$root/src" \
	-c "$root/src/drivers/gpu/i915/vk/vk-codec.c" -o "$out/vk/i915-codec.o"
cc -std=c11 -O1 -Wall -Wextra -Werror -DPOWERVR_FORMAT_LINEAR_SAMPLED=0 -I"$root/include" -I"$root/src" -I"$p009" -I"$here" \
	-c "$here/nv12-host-test.c" -o "$out/vk/nv12-host-test.o"
cc -o "$out/nv12-host-test" "$out/vk/nv12-host-test.o" $objects "$out/vk/i915-codec.o" \
	"$out/vk/frame-host.o" "$out/vk/nv12-frames.o" "$out/vk/lib-wire.o" "$out/vk/lib-codec.o"
"$out/nv12-host-test" "$out" | grep -v '^  '
