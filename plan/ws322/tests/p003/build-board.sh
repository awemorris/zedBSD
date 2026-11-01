#!/bin/sh
# Builds the board check nv12-board (nv12-board/main.c) for the VisionFive 2
# against a tree's build of config/ci/config-visionfive2.mk (its sysroot,
# libc.so and libvulkan.so), the shaders compiled by glslc into a header:
#   plan/ws322/tests/p003/build-board.sh BUILD      (writes BUILD/bin/nv12-board)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
root=$(cd -- "$(dirname -- "$0")/../../../.." && pwd)
here=$root/plan/ws322/tests/p003/nv12-board
build=${1:?usage: build-board.sh BUILD}
obj=$build/tmp/nv12-board
mkdir -p "$obj" "$build/bin"
clang=$root/build/llvm/bin/clang
sysroot=$root/build/riscv64/sysroot
for stage in vert frag; do
	glslc --target-env=vulkan1.0 -o "$obj/planes.$stage.spv" "$here/planes.$stage"
done
/usr/bin/python3 -I - "$obj" > "$obj/planes-spv.h" <<'PY'
import struct, sys
out = sys.argv[1]
print("/* The shaders of nv12-board, compiled by glslc at build time. */")
print("#include <stdint.h>")
for stage in ("vert", "frag"):
    data = open("%s/planes.%s.spv" % (out, stage), "rb").read()
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    print("static const uint32_t nv12_planes_%s[] = {" % stage)
    for i in range(0, len(words), 6):
        print("\t" + ", ".join("0x%08xU" % w for w in words[i:i + 6]) + ",")
    print("};")
PY
"$clang" --target=riscv64-unknown-zedbsd -nostdinc -I"$root" -I"$root/include" -I"$obj" -isystem "$sysroot/usr/include" \
	-DHAL_ARCH_RISCV64 -DKERN_USER_ABI_RISCV64 -DKERN_USER_ABI_LP64 -DKERN_DYNAMIC_LIBC -march=rv64gc -mabi=lp64d -mno-relax -Os \
	-ffreestanding -fPIC -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-ftls-model=global-dynamic -mtls-dialect=trad -Wall -Wextra -Werror -c "$here/main.c" -o "$obj/main.o"
"$clang" --target=riscv64-unknown-zedbsd --sysroot="$sysroot" -march=rv64gc -mabi=lp64d -mno-relax -nostdlib -pie -Wl,--no-relax \
	-Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code -Wl,-z,stack-size=0x100000,--allow-shlib-undefined \
	-Wl,--dynamic-linker=/lib/ld.so "$sysroot/usr/lib/crt1.o" "$obj/main.o" \
	-L"$build/dynamic" -Wl,-rpath-link,"$build/dynamic" -l:libvulkan.so -l:libc.so -o "$build/bin/nv12-board"
echo "built $build/bin/nv12-board"
