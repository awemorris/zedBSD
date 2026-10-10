#!/bin/sh
# ws157-p004: builds and runs the host test of Photos' library, database and import (host-photos-db.c with
# userland/desktop/photos/exif.c, library.c, db.c and import.c, and the C library's SHA-256) under ASan and UBSan, on
# the folders make-photos.py writes into a fresh folder each run (under OUTPUT's folder; Q1's cleaning removes the old
# ones).
#   sh plan/ws157/tests/run-host-photos-db.sh [OUTPUT]   (default build/ws157/host-photos-db)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws157/host-photos-db}
dir=$(dirname -- "$out")
mkdir -p "$dir/inc/keiland"
cp userland/desktop/include/keiland/keiland.h "$dir/inc/keiland/keiland.h"
cp include/libc/sha2.h "$dir/inc/sha2.h"
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -O1 -g -w -c src/libc/openbsd-sha2.c -I"$dir/inc" -o "$dir/sha2.o"
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	-I. -I"$dir/inc" -Iuserland/desktop/photos plan/ws157/tests/host-photos-db.c \
	userland/desktop/photos/exif.c userland/desktop/photos/library.c userland/desktop/photos/db.c \
	userland/desktop/photos/import.c userland/desktop/mediastorage/dimensions.c "$dir/sha2.o" -o "$out"
run=$(mktemp -d "$(pwd)/$dir/photos-db.XXXXXX")
python3 plan/ws157/tests/make-photos.py "$run/Pictures"
python3 plan/ws157/tests/make-photos.py --other "$run/Other"
TZ=UTC timeout 60 "$out" "$run/Pictures" "$run/Other" "$run/Library"
