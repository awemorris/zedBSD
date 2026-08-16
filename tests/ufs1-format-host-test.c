/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/ufs1/ufs1-disk.h"
#include "kern/ufs1/ufs1-endian.h"
#include "kern/ufs1/ufs1-super.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void
make_super(uint8_t *data, int swapped)
{
	memset(data, 0, UFS1_FS_STRUCT_SIZE);
	ufs1_put32(data, UFS1_FS_SBLKNO, 8, swapped);
	ufs1_put32(data, UFS1_FS_CBLKNO, 16, swapped);
	ufs1_put32(data, UFS1_FS_IBLKNO, 24, swapped);
	ufs1_put32(data, UFS1_FS_DBLKNO, 56, swapped);
	ufs1_put32(data, UFS1_FS_OLD_SIZE, 4096, swapped);
	ufs1_put32(data, UFS1_FS_OLD_DSIZE, 4040, swapped);
	ufs1_put32(data, UFS1_FS_NCG, 1, swapped);
	ufs1_put32(data, UFS1_FS_BSIZE, 8192, swapped);
	ufs1_put32(data, UFS1_FS_FSIZE, 1024, swapped);
	ufs1_put32(data, UFS1_FS_FRAG, 8, swapped);
	ufs1_put32(data, UFS1_FS_BSHIFT, 13, swapped);
	ufs1_put32(data, UFS1_FS_FSHIFT, 10, swapped);
	ufs1_put32(data, UFS1_FS_FRAGSHIFT, 3, swapped);
	ufs1_put32(data, UFS1_FS_FSBTODB, 1, swapped);
	ufs1_put32(data, UFS1_FS_SBSIZE, UFS1_FS_STRUCT_SIZE, swapped);
	ufs1_put32(data, UFS1_FS_NINDIR, 2048, swapped);
	ufs1_put32(data, UFS1_FS_INOPB, 64, swapped);
	ufs1_put32(data, UFS1_FS_CGSIZE, 712, swapped);
	ufs1_put32(data, UFS1_FS_IPG, 256, swapped);
	ufs1_put32(data, UFS1_FS_FPG, 4096, swapped);
	ufs1_put32(data, UFS1_FS_MAXSYMLINKLEN, 60, swapped);
	ufs1_put32(data, UFS1_FS_INODEFMT, UFS1_44INODEFMT, swapped);
	ufs1_put64(data, UFS1_FS_MAXFILESIZE, 0x7fffffffffffffffULL,
	    swapped);
	ufs1_put32(data, UFS1_FS_MAGIC, UFS1_MAGIC, swapped);
	data[UFS1_FS_CLEAN] = 1;
}

int
main(void)
{
	uint8_t data[UFS1_FS_STRUCT_SIZE];
	struct ufs1_super super;
	make_super(data, 0);
	assert(ufs1_super_decode(data, sizeof(data), 8192, &super) == 0);
	assert(super.bsize == 8192 && super.fsize == 1024 && !super.swapped);
	make_super(data, 1);
	assert(ufs1_super_decode(data, sizeof(data), 8192, &super) == 0);
	assert(super.swapped && super.inodefmt == UFS1_44INODEFMT);
	ufs1_put32(data, UFS1_FS_BSIZE, 4096, 1);
	assert(ufs1_super_decode(data, sizeof(data), 8192, &super) == EINVAL);
	make_super(data, 0);
	ufs1_put32(data, UFS1_FS_NINDIR, 1024, 0);
	assert(ufs1_super_decode(data, sizeof(data), 8192, &super) == EINVAL);
	make_super(data, 0);
	ufs1_put32(data, UFS1_FS_INOPB, 63, 0);
	assert(ufs1_super_decode(data, sizeof(data), 8192, &super) == EINVAL);
	make_super(data, 0);
	ufs1_put32(data, UFS1_FS_OLD_DSIZE, 4097, 0);
	assert(ufs1_super_decode(data, sizeof(data), 8192, &super) == EINVAL);
	make_super(data, 0);
	ufs1_put32(data, UFS1_FS_OLD_SIZE, 4097, 0);
	assert(ufs1_super_decode(data, sizeof(data), 8192, &super) == EINVAL);
	make_super(data, 0);
	ufs1_put32(data, UFS1_FS_IBLKNO, 56, 0);
	assert(ufs1_super_decode(data, sizeof(data), 8192, &super) == EINVAL);
	make_super(data, 0);
	ufs1_put32(data, UFS1_FS_CBLKNO, 24, 0);
	assert(ufs1_super_decode(data, sizeof(data), 8192, &super) == EINVAL);
	make_super(data, 0);
	ufs1_put32(data, UFS1_FS_DBLKNO, 25, 0);
	assert(ufs1_super_decode(data, sizeof(data), 8192, &super) == EINVAL);
	make_super(data, 0);
	ufs1_put32(data, UFS1_FS_FPG, 2048, 0);
	assert(ufs1_super_decode(data, sizeof(data), 8192, &super) == EINVAL);
	make_super(data, 0);
	ufs1_put32(data, UFS1_FS_CGSIZE, 8193, 0);
	assert(ufs1_super_decode(data, sizeof(data), 8192, &super) == EINVAL);
	make_super(data, 0);
	ufs1_put32(data, UFS1_FS_BSHIFT, 12, 0);
	assert(ufs1_super_decode(data, sizeof(data), 8192, &super) == EINVAL);
	make_super(data, 0);
	ufs1_put32(data, UFS1_FS_SBSIZE, UFS1_SBLOCK_SIZE + 1U, 0);
	assert(ufs1_super_decode(data, sizeof(data), 8192, &super) == EINVAL);
	make_super(data, 0);
	assert(ufs1_super_decode(data, UFS1_FS_STRUCT_SIZE - 1U, 8192,
	    &super) == EINVAL);
	memset(data, 0, sizeof(data));
	assert(ufs1_super_decode(data, sizeof(data), 8192, &super) ==
	    EOPNOTSUPP);
	puts("zedBSD UFS1 disk-format tests: PASS");
	return 0;
}
