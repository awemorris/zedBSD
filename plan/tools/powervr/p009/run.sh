#!/bin/sh
# Host check of ws269-p009: the Vulkan executor decoding frames that
# libvulkan's own encoders write, its compute dispatch compiled by the
# shader compiler (p010-compiler) from shaders/fill.comp.
#   plan/tools/powervr/p009/run.sh OUTDIR [SWITCH]
# SWITCH is the shared path's switch, POWERVR_FORMAT_LINEAR_SAMPLED
# (image-state.h): 0 (the default, as built) or 1.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
root=$(cd -- "$(dirname -- "$0")/../../../.." && pwd)
here=$root/plan/tools/powervr/p009
out=$1
switch=${2:-0}
define=-DPOWERVR_FORMAT_LINEAR_SAMPLED=$switch
mkdir -p "$out/inc"
for shader in "$here"/shaders/*.comp; do
	name=$(basename "$shader" .comp)
	glslc --target-env=vulkan1.0 -o "$out/$name.spv" "$shader"
done
# The executor: the kernel side (with host stand-ins) and libvulkan's encoders.
mkdir -p "$out/vk"
ln -sfn "$root/include/libc/vulkan" "$out/inc/vulkan"
for file in wire codec; do
	cc -std=c11 -O1 -Wall -Wno-unused-parameter -I"$root" -I"$root/include" -I"$out/inc" -I"$root/userland/desktop/libvulkan" \
		-c "$root/userland/desktop/libvulkan/$file.c" -o "$out/vk/lib-$file.o"
done
cc -std=c11 -O1 -Wall -Wextra -Werror $define -I"$root" -I"$root" -I"$root/include" -I"$out/inc" -I"$root/userland/desktop/libvulkan" \
	-c "$here/frame-host.c" -o "$out/vk/frame-host.o"
objects=""
for file in vk/vk-object vk/vk-stream vk/vk-device vk/vk-memory vk/vk-image vk/vk-query video-state image-state descriptor-set data-program vk/vk-layout vk/vk-command vk/vk-queue vk/vk-pass vk/vk-transfer vk/vk-draw render-draw render-pass render-program transfer-program compiler/shader-spirv compiler/shader-spirv-interface compiler/shader-spirv-control compiler/shader-spirv-instruction compiler/shader-backend compute-program render-sr-program compiler/usc-encode compiler/pds-encode; do
	name=${file##*/}
	cc -std=c11 -O1 -Wall -Wextra -Werror $define -I"$root/include" -I"$root/src" \
		-c "$root/src/drivers/gpu/powervr/$file.c" -o "$out/vk/$name.o"
	objects="$objects $out/vk/$name.o"
done
cc -std=c11 -O1 -Wall -Wextra -Werror $define -I"$root/include" -I"$root/src" \
	-c "$root/src/drivers/gpu/i915/vk/vk-codec.c" -o "$out/vk/i915-codec.o"
cc -std=c11 -O1 -Wall -Wextra -Werror $define -I"$root/include" -I"$root/src" -I"$here" \
	-c "$here/vk-host-test.c" -o "$out/vk/vk-host-test.o"
cc -o "$out/vk-host-test" "$out/vk/vk-host-test.o" $objects "$out/vk/i915-codec.o" \
	"$out/vk/frame-host.o" "$out/vk/lib-wire.o" "$out/vk/lib-codec.o"
"$out/vk-host-test" "$out"
