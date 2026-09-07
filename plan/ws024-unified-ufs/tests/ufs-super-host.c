/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "src/drivers/fs/ufs/ufs-super.h"
#include "src/drivers/fs/ufs/ufs-endian.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { \
	printf("FAIL line %u\n", __LINE__); return 1; } } while (0)

/* Exercises both byte orders and malformed geometry through the real decoder. */
int
main(void)
{
	static const struct { size_t offset; uint32_t value; } fields[] = {
		{UFS_FS_MAGIC, UFS_MAGIC}, {UFS_FS_SBLKNO, 64},
		{UFS_FS_CBLKNO, 72}, {UFS_FS_IBLKNO, 80},
		{UFS_FS_DBLKNO, 144}, {UFS_FS_NCG, 2},
		{UFS_FS_BSIZE, 8192}, {UFS_FS_FSIZE, 1024},
		{UFS_FS_FRAG, 8}, {UFS_FS_BSHIFT, 13},
		{UFS_FS_FSHIFT, 10}, {UFS_FS_FRAGSHIFT, 3},
		{UFS_FS_FSBTODB, 1}, {UFS_FS_SBSIZE, 1376},
		{UFS_FS_NINDIR, 1024}, {UFS_FS_INOPB, 32},
		{UFS_FS_IPG, 256}, {UFS_FS_FPG, 4096},
		{UFS_FS_CGSIZE, 8192}
	};
	static const struct { size_t offset; uint32_t value; } bad[] = {
		{UFS_FS_BSHIFT, 32}, {UFS_FS_FSHIFT, 63},
		{UFS_FS_FRAGSHIFT, 32}, {UFS_FS_FSBTODB, 32},
		{UFS_FS_BSHIFT, 12}, {UFS_FS_FSHIFT, 9},
		{UFS_FS_FRAGSHIFT, 2}, {UFS_FS_FSBTODB, 2},
		{UFS_FS_NCG, 0}, {UFS_FS_NCG, UINT32_MAX},
		{UFS_FS_IPG, UINT32_MAX}, {UFS_FS_FPG, UINT32_MAX},
		{UFS_FS_DBLKNO, 4096}, {UFS_FS_IBLKNO, 143},
		{UFS_FS_NINDIR, 2048}, {UFS_FS_INOPB, 64}
	};
	uint8_t raw[UFS_SBLOCK_SIZE], changed[UFS_SBLOCK_SIZE];
	struct ufs_super super;
	size_t i;
	int swapped;

	/* Rebuild independent little- and big-endian superblocks. */
	for (swapped = 0; swapped < 2; swapped++) {
		memset(raw, 0, sizeof(raw));
		for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
			ufs_put32(raw, fields[i].offset, fields[i].value, swapped);
		ufs_put64(raw, UFS_FS_SBLOCKLOC, 65536, swapped);
		ufs_put64(raw, UFS_FS_SIZE, 8192, swapped);
		ufs_put64(raw, UFS_FS_DSIZE, 7904, swapped);
		CHECK(ufs_super_decode(raw, sizeof(raw), 16384, &super) == 0);
		CHECK(super.swapped == swapped && super.size == 8192);
		CHECK(ufs_super_decode(raw, 1375, 16384, &super) == EINVAL);
		CHECK(ufs_super_decode(raw, sizeof(raw), 16383, &super) == EINVAL);

		/* Reject every mutation independently of previous failures. */
		for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			memcpy(changed, raw, sizeof(raw));
			ufs_put32(changed, bad[i].offset, bad[i].value, swapped);
			CHECK(ufs_super_decode(changed, sizeof(changed), UINT64_MAX, &super) == EINVAL);
		}

		/* Reject legacy magic and a wrapped final cylinder-group extent. */
		memcpy(changed, raw, sizeof(raw));
		ufs_put32(changed, UFS_FS_MAGIC, 0x11954, swapped);
		CHECK(ufs_super_decode(changed, sizeof(changed), 16384, &super) == EOPNOTSUPP);
		ufs_put64(raw, UFS_FS_SIZE, UINT64_MAX, swapped);
		CHECK(ufs_super_decode(raw, sizeof(raw), UINT64_MAX, &super) == EINVAL);
	}

	/* Report the completed geometry matrix. */
	printf("UFS endian and malformed geometry PASS: %u checks\n", checks);
	return 0;
}
