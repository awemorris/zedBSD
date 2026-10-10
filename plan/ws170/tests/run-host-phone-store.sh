#!/bin/sh
# ws170-p002: builds and runs the host test of Phone's store (host-phone-store.c with userland/desktop/phone/store.c userland/desktop/phone/phonebook.c)
# under ASan and UBSan, in a fresh folder each run (under OUTPUT's folder; Q1's cleaning removes the old ones).
#   sh plan/ws170/tests/run-host-phone-store.sh [OUTPUT]   (default build/ws170/host-phone-store)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws170/host-phone-store}
dir=$(dirname -- "$out")
mkdir -p "$dir/include/keiland"
cp userland/desktop/include/keiland/keiland.h "$dir/include/keiland/keiland.h"
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	-I. -I"$dir/include" plan/ws170/tests/host-phone-store.c userland/desktop/phone/store.c userland/desktop/phone/phonebook.c -o "$out"
folder=$(mktemp -d "$dir/phone-store.XXXXXX")
timeout 60 "$out" "$folder/Phone"
