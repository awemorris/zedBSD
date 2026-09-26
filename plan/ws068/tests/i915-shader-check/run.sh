#!/bin/sh
# ws068-p006: the i915 compiler on the host over libGL's fixed-function shaders and egltest's scene (as linked:
# the vertex shaders with gl_Position rewritten by libGLESv2's spirv.c, through the ws068 host fixture).
#   plan/ws068/tests/i915-shader-check/run.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
here=$(cd "$(dirname -- "$0")" && pwd)
root=$here/../../../..
out=${1:-$root/build/ws068-p006-shaders}
mkdir -p "$out"
cc -std=gnu99 -O0 -g -w -I"$root" -I"$root/include" -DHAL_ARCH_AMD64 -o "$out/check" "$here/main.c" -lm || exit 1
"$root/plan/ws068/tests/spirv-host/run.sh" "$out/host" >/dev/null 2>&1
for variant in smooth flat; do
	define=; [ $variant = flat ] && define=-DFLAT
	glslc --target-env=vulkan1.0 $define -o "$out/fixed-$variant.vert.spv" "$root/userland/X11/libGL/shaders/fixed.vert"
	glslc --target-env=vulkan1.0 $define -o "$out/fixed-$variant.frag.spv" "$root/userland/X11/libGL/shaders/fixed.frag"
	"$out/host/spirv-test" "$out/fixed-$variant.vert.spv" "$out/fixed-$variant.vert.linked.spv" >/dev/null
done
glslc --target-env=vulkan1.0 -o "$out/scene.vert.spv" "$root/userland/base/egltest/shaders/scene.vert"
glslc --target-env=vulkan1.0 -o "$out/scene.frag.spv" "$root/userland/base/egltest/shaders/scene.frag"
"$out/host/spirv-test" "$out/scene.vert.spv" "$out/scene.vert.linked.spv" >/dev/null
"$out/check" vertex "$out/fixed-smooth.vert.linked.spv" "$out/fixed-flat.vert.linked.spv" "$out/scene.vert.linked.spv"
"$out/check" fragment "$out/fixed-smooth.frag.spv" "$out/fixed-flat.frag.spv" "$out/scene.frag.spv"
