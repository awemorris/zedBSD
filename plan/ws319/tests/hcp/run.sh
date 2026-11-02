#!/bin/sh
# Host test of the i915 HCP builder (hcp-host-test.c with vk-video-hcp.c and vk-video-h265.c; ASan and UBSan).
#   sh plan/ws319/tests/hcp/run.sh OUTDIR
# Last line: hcp: PASS or hcp: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
root=$(cd -- "$(dirname -- "$0")/../../../.." && pwd)
out=${1:?usage: run.sh OUTDIR}
mkdir -p "$out"
timeout 300 cc -std=gnu11 -O1 -g -D_POSIX_C_SOURCE=200809L -DKERN_USER_ABI_LP64 -include time.h -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all -ffunction-sections -fdata-sections -Wl,--gc-sections \
	-I"$root/include" -I"$root/src" -I"$root" \
	"$root/plan/ws319/tests/hcp/hcp-host-test.c" "$root/src/drivers/gpu/i915/vk/vk-video-hcp.c" \
	"$root/src/drivers/gpu/i915/vk/vk-video-h265.c" "$root/src/drivers/gpu/i915/vk/vk-codec.c" -o "$out/hcp-host-test"
timeout 60 "$out/hcp-host-test"
