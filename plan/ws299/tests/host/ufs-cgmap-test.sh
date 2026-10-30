#!/bin/sh
# ws299: host test of the UFS allocator's free-map search (cg_free_blocks and cg_block_free in src/drivers/fs/ufs.c,
# taken out of the file as they are): on random free maps of 1, 2, 4, 8 and 16 fragments to a block, from every
# start on the block grid, the first free block and the length of its run (up to WANT blocks) must be the ones a
# plain walk of the bits finds.  Output goes to a new build/tmp/ufs-cgmap.* directory.
#   sh plan/ws299/tests/host/ufs-cgmap-test.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname "$0")/../../../.."
mkdir -p build/tmp
out=$(mktemp -d build/tmp/ufs-cgmap.XXXXXX)
# Each function from the line before its name (the return type) to its closing brace.
take() {
	awk -v name="$1" '
		{ prev = $0 }
		$0 == name "(" { printing = 1; print last }
		printing { print }
		printing && $0 == "}" { printing = 0 }
		{ last = prev }' src/drivers/fs/ufs.c
}
{
	cat <<'HEAD'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
struct ufs_super { uint32_t frag; };
static int bit_test(const uint8_t *map, uint32_t bit);
static int cg_block_free(const uint8_t *map, uint32_t fragment, uint32_t frag);
HEAD
	take bit_test
	take cg_block_free
	take cg_free_blocks
	cat <<'TAIL'

/* The plain walk: the first block from start whose bits are all set, and its run. */
static uint32_t
reference(uint32_t frag, const uint8_t *map, uint32_t start, uint32_t end, uint32_t want, uint32_t *found)
{
	uint32_t f, n, count;
	for (f = start; f + frag <= end; f += frag) {
		for (n = 0; n < frag && (map[(f + n) >> 3] >> ((f + n) & 7) & 1); n++)
			;
		if (n != frag)
			continue;
		count = 0;
		while (count < want && f + (count + 1) * frag <= end) {
			for (n = 0; n < frag && (map[(f + count * frag + n) >> 3] >> ((f + count * frag + n) & 7) & 1); n++)
				;
			if (n != frag)
				break;
			count++;
		}
		*found = f;
		return count;
	}
	return 0;
}

int
main(void)
{
	static const uint32_t frags[] = { 1, 2, 4, 8, 16 };
	uint8_t map[512];
	uint32_t round, i, k, frag, end, start, base, want, got, ref, found, rfound;
	unsigned long checks = 0, failures = 0;
	srand(299);
	for (round = 0; round < 400; round++) {
		/* Mostly used maps with free stretches, and some all free or all used. */
		for (i = 0; i < sizeof(map); i++) {
			k = (uint32_t)rand() % 16;
			map[i] = k < 9 ? 0 : k < 13 ? 0xff : (uint8_t)rand();
			if (round % 50 == 1) map[i] = 0xff;
			if (round % 50 == 2) map[i] = 0;
		}
		for (k = 0; k < sizeof(frags) / sizeof(frags[0]); k++) {
			struct ufs_super super;
			frag = frags[k];
			super.frag = frag;
			base = (uint32_t)(rand() % 3) * 8 + (frag < 8 ? (uint32_t)(rand() % (8 / frag)) * frag : 0);
			end = sizeof(map) * 8 - (uint32_t)(rand() % 40);
			want = 1 + (uint32_t)rand() % 140;
			for (start = base; start <= end; start += frag) {
				found = rfound = 0xffffffffU;
				got = cg_free_blocks(&super, map, start, end, want, &found);
				ref = reference(frag, map, start, end, want, &rfound);
				checks++;
				if (got != ref || (got != 0 && found != rfound)) {
					if (failures++ < 10)
						printf("FAIL frag=%u start=%u end=%u want=%u: got %u at %u, want %u at %u\n",
						    frag, start, end, want, got, found, ref, rfound);
				}
			}
		}
	}
	printf("%lu checks, %lu failures\n", checks, failures);
	return failures != 0;
}
TAIL
} > "$out/test.c"
cc -std=c99 -O2 -Wall -Wextra -Werror -Wno-unused-function -o "$out/test" "$out/test.c"
if "$out/test"; then
	echo "ufs-cgmap-test: PASS $out"
else
	echo "ufs-cgmap-test: FAIL $out"
	exit 1
fi
