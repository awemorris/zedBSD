#!/bin/sh
# ws103-p006 (WS103 V1), paths revised by ws131-p009: the Keiland compositor (userland/desktop/wayland/) uses the GPU only
# through Vulkan; the zedBSD GPU buffers are libkeiland-backend's (libkeiland-backend-zedbsd/gpu-zedbsd.c reads the kernel's
# GPU types; libvulkan, the driver, is not checked here).
#  1. grep: no compositor or zedBSD backend source but libkeiland-backend-zedbsd/gpu-zedbsd.c includes a GPU UAPI header (uapi/gpu*.h), and no
#     compositor or zedBSD backend source calls a GPU ioctl (the backend's own device ioctls are not GPU ones).
#  2. poisoned headers: every GPU UAPI header is replaced by one that is an #error, first on the include path, and every
#     compositor source (the zedBSD Makefile's list, without libkeiland-backend's) is compiled with -fsyntax-only (the
#     zedBSD build's compiler and flags); a source that reads a GPU UAPI header, directly or through another header, fails.
#  3. the compositor opens no GPU node and has no --gpu option.
# There is no exception: the game mode's images are lent and shown through libvulkan (ws309-p015).
# Prints "v1-check: PASS" or "v1-check: FAIL ...".
#   sh plan/tools/gpu-boundary/v1-check.sh [BUILD]     (BUILD for the sysroot, default build/amd64)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
build=${1:-build/amd64}
dir=userland/desktop/wayland
backend=userland/desktop/libkeiland-backend-zedbsd/gpu-zedbsd.c
files=$(find "$dir" userland/desktop/libkeiland-backend-zedbsd -name "*.[ch]" -print | sort)
failures=0
fail() { echo "v1-check: FAIL $*"; failures=$((failures + 1)); }

# 1. The GPU UAPI headers only in the backend, and no GPU ioctl anywhere.
readers=$(grep -ln '#include <uapi/gpu' $files | grep -v -e "^$backend$")
[ -z "$readers" ] || fail "GPU UAPI included by: $readers"
calls=$(grep -n 'ioctl([^,]*, *GPU_' $files)
[ -z "$calls" ] || fail "GPU ioctl: $calls"
backend_calls=$(grep -c 'ioctl(' $backend)
[ "$backend_calls" = 0 ] || fail "the backend calls ioctl"

# 2. Every source but the backend compiles with the GPU UAPI headers poisoned.
# The work directory stays in build/tmp (2026-10-06 user: deleting is Q1's step; plan/tools/q1-clean.sh removes it).
mkdir -p build/tmp
work=$(mktemp -d "$(pwd)/build/tmp/v1-check.XXXXXX")
mkdir -p "$work/uapi"
for header in include/uapi/gpu*.h; do
	name=$(basename "$header")
	printf '#error "%s is a GPU UAPI header (ws103 V1)"\n' "$name" > "$work/uapi/$name"
done
sources=$(make -s -f /dev/stdin print <<'EOF' 2>/dev/null
include userland/desktop/wayland/Makefile
print:
	@echo $(KEILAND_SOURCES)
EOF
)
[ -n "$sources" ] || sources=$(find "$dir" -name "*.c" -print | sort)
compiled=0
for source in $sources; do
	case $source in
	userland/desktop/libkeiland-backend*) continue ;;
	esac
	build/llvm/bin/clang --target=x86_64-unknown-zedbsd --sysroot="$build/sysroot" -nostdinc \
		-I"$work" -I. -Iinclude -isystem "$build/sysroot/usr/include" \
		-DHAL_ARCH_AMD64 -DKERN_USER_ABI_LP64 -DKERN_DYNAMIC_LIBC -m64 -ffreestanding -fno-builtin \
		-Wall -Wextra -Werror -fsyntax-only "$source" > "$work/out.txt" 2>&1 || {
		fail "$source: $(grep -m1 'error' "$work/out.txt")"
		continue
	}
	compiled=$((compiled + 1))
done
echo "v1-check: $compiled sources compiled with the GPU UAPI headers poisoned"

# 3. No GPU node and no --gpu option.
nodes=$(grep -n '"/dev/gpu' $files)
[ -z "$nodes" ] || fail "GPU node: $nodes"
option=$(grep -n '"--gpu' $(find "$dir" -name "*.c" -print))
[ -z "$option" ] || fail "--gpu option: $option"

[ $failures = 0 ] || exit 1
echo "v1-check: PASS"
