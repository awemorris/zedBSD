#!/bin/sh
# ws068-p008: host test of the SPIR-V reflection and the gl_Position rewrite (needs glslc, spirv-val, cc).
#   plan/ws068/tests/spirv-host/run.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
here=$(cd "$(dirname -- "$0")" && pwd)
root=$here/../../../..
out=${1:-$root/build/ws068-p008-host}
mkdir -p "$out/shim"
for h in EGL GLES2 KHR wayland-egl-core.h; do ln -sfn "$root/include/libc/$h" "$out/shim/$h"; done
cc -std=c99 -Wall -Wextra -I"$out/shim" -o "$out/spirv-test" "$here/main.c" "$root/userland/base/libglesv2/spirv.c"
status=0
for stage in vert frag; do
	glslc --target-env=vulkan1.0 -o "$out/tri.$stage.spv" "$here/tri.$stage"
	"$out/spirv-test" "$out/tri.$stage.spv" "$out/tri.$stage.patched.spv" | tee "$out/tri.$stage.txt"
	spirv-val --target-env vulkan1.0 "$out/tri.$stage.patched.spv" && echo "spirv-val $stage: ok" || status=1
done
grep -q "uniform u_offsets type=0x8b50 size=3" "$out/tri.vert.txt" || status=1
grep -q "uniform u_texture type=0x8b5e size=1 offset=0 stride=0 mstride=0 sampler=1 binding=1" "$out/tri.frag.txt" || status=1
grep -q "in a_uv location=2 type=0x8b50 components=2" "$out/tri.vert.txt" || status=1
spirv-dis "$out/tri.vert.patched.spv" | grep -c "OpFNegate" | grep -qx 1 || status=1
[ $status -eq 0 ] && echo "spirv-host: PASS" || echo "spirv-host: FAIL"
exit $status
