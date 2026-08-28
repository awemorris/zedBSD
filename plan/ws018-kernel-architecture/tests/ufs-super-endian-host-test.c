/*
 * KA-T020: independent UFS superblock/endian behavior fixture
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(KA_UFS1)
#include "ufs1-disk.h"
#include "ufs1-endian.h"
#include "ufs1-super.h"
#define FS_NAME "UFS1"
#define FS_STRUCT_SIZE UFS1_FS_STRUCT_SIZE
#define FS_MAGIC_OFFSET UFS1_FS_MAGIC
#define FS_MAGIC UFS1_MAGIC
#define GET16 ufs1_get16
#define GET32 ufs1_get32
#define GET64 ufs1_get64
#define PUT16 ufs1_put16
#define PUT32 ufs1_put32
#define PUT64 ufs1_put64
#elif defined(KA_UFS2)
#include "ufs2-disk.h"
#include "ufs2-super.h"
#if defined(KA_LEGACY_UFS_COMMON)
#include "ufs1-endian.h"
#define GET16 ufs1_get16
#define GET32 ufs1_get32
#define GET64 ufs1_get64
#define PUT16 ufs1_put16
#define PUT32 ufs1_put32
#define PUT64 ufs1_put64
#else
#include "ufs2-endian.h"
#define GET16 ufs2_get16
#define GET32 ufs2_get32
#define GET64 ufs2_get64
#define PUT16 ufs2_put16
#define PUT32 ufs2_put32
#define PUT64 ufs2_put64
#endif
#define FS_NAME "UFS2"
#define FS_STRUCT_SIZE UFS2_FS_STRUCT_SIZE
#define FS_MAGIC_OFFSET UFS2_FS_MAGIC
#define FS_MAGIC UFS2_MAGIC
#else
#error "define exactly one of KA_UFS1 or KA_UFS2"
#endif

static unsigned checks;

#define CHECK(expression)                                                   \
	do {                                                                 \
		checks++;                                                    \
		if (!(expression)) {                                        \
			fprintf(stderr, "KA-T020 %s: check failed at %s:%d: %s\n", \
			    FS_NAME, __FILE__, __LINE__, #expression);          \
			exit(1);                                             \
		}                                                            \
	} while (0)

static void
test_endian_helpers(void)
{
	uint8_t buffer[40];

	memset(buffer, 0xa5, sizeof(buffer));
	PUT16(buffer, 1, UINT16_C(0x1234), 0);
	PUT32(buffer, 4, UINT32_C(0x89abcdef), 0);
	PUT64(buffer, 9, UINT64_C(0x0123456789abcdef), 0);
	CHECK(buffer[1] == 0x34 && buffer[2] == 0x12);
	CHECK(buffer[4] == 0xef && buffer[7] == 0x89);
	CHECK(buffer[9] == 0xef && buffer[16] == 0x01);
	CHECK(GET16(buffer, 1, 0) == UINT16_C(0x1234));
	CHECK(GET32(buffer, 4, 0) == UINT32_C(0x89abcdef));
	CHECK(GET64(buffer, 9, 0) == UINT64_C(0x0123456789abcdef));

	PUT16(buffer, 18, UINT16_C(0x1234), 1);
	PUT32(buffer, 20, UINT32_C(0x89abcdef), 1);
	PUT64(buffer, 25, UINT64_C(0x0123456789abcdef), 1);
	CHECK(buffer[18] == 0x12 && buffer[19] == 0x34);
	CHECK(buffer[20] == 0x89 && buffer[23] == 0xef);
	CHECK(buffer[25] == 0x01 && buffer[32] == 0xef);
	CHECK(GET16(buffer, 18, 1) == UINT16_C(0x1234));
	CHECK(GET32(buffer, 20, 1) == UINT32_C(0x89abcdef));
	CHECK(GET64(buffer, 25, 1) == UINT64_C(0x0123456789abcdef));
}

#if defined(KA_UFS1)
static void
build_superblock(uint8_t *buffer, int swapped)
{
	memset(buffer, 0, UFS1_SBLOCK_SIZE);
	PUT32(buffer, UFS1_FS_SBLKNO, 8, swapped);
	PUT32(buffer, UFS1_FS_CBLKNO, 16, swapped);
	PUT32(buffer, UFS1_FS_IBLKNO, 24, swapped);
	PUT32(buffer, UFS1_FS_DBLKNO, 40, swapped);
	PUT32(buffer, UFS1_FS_OLD_CGOFFSET, 0, swapped);
	PUT32(buffer, UFS1_FS_OLD_CGMASK, 0, swapped);
	PUT32(buffer, UFS1_FS_OLD_SIZE, 4096, swapped);
	PUT32(buffer, UFS1_FS_OLD_DSIZE, 3000, swapped);
	PUT32(buffer, UFS1_FS_NCG, 1, swapped);
	PUT32(buffer, UFS1_FS_BSIZE, 8192, swapped);
	PUT32(buffer, UFS1_FS_FSIZE, 1024, swapped);
	PUT32(buffer, UFS1_FS_FRAG, 8, swapped);
	PUT32(buffer, UFS1_FS_BSHIFT, 13, swapped);
	PUT32(buffer, UFS1_FS_FSHIFT, 10, swapped);
	PUT32(buffer, UFS1_FS_FRAGSHIFT, 3, swapped);
	PUT32(buffer, UFS1_FS_FSBTODB, 1, swapped);
	PUT32(buffer, UFS1_FS_SBSIZE, UFS1_FS_STRUCT_SIZE, swapped);
	PUT32(buffer, UFS1_FS_NINDIR, 2048, swapped);
	PUT32(buffer, UFS1_FS_INOPB, 64, swapped);
	PUT32(buffer, UFS1_FS_CSSIZE, 0, swapped);
	PUT32(buffer, UFS1_FS_CGSIZE, 512, swapped);
	PUT32(buffer, UFS1_FS_IPG, 64, swapped);
	PUT32(buffer, UFS1_FS_FPG, 4096, swapped);
	PUT32(buffer, UFS1_FS_MAXSYMLINKLEN, 60, swapped);
	PUT32(buffer, UFS1_FS_INODEFMT, UFS1_44INODEFMT, swapped);
	PUT64(buffer, UFS1_FS_MAXFILESIZE, UINT64_C(0x7fffffff), swapped);
	PUT32(buffer, UFS1_FS_MAGIC, UFS1_MAGIC, swapped);
}

static int
decode_superblock(const uint8_t *buffer, size_t length, uint64_t sectors,
	int *swapped)
{
	struct ufs1_super super;
	int error = ufs1_super_decode(buffer, length, sectors, &super);

	if (error == 0) {
		CHECK(super.size == 4096);
		CHECK(super.bsize == 8192);
		*swapped = super.swapped;
	}
	return error;
}
#else
static void
build_superblock(uint8_t *buffer, int swapped)
{
	memset(buffer, 0, UFS2_SBLOCK_SIZE);
	PUT32(buffer, UFS2_FS_SBLKNO, 64, swapped);
	PUT32(buffer, UFS2_FS_CBLKNO, 72, swapped);
	PUT32(buffer, UFS2_FS_IBLKNO, 80, swapped);
	PUT32(buffer, UFS2_FS_DBLKNO, 96, swapped);
	PUT32(buffer, UFS2_FS_NCG, 1, swapped);
	PUT32(buffer, UFS2_FS_BSIZE, 8192, swapped);
	PUT32(buffer, UFS2_FS_FSIZE, 1024, swapped);
	PUT32(buffer, UFS2_FS_FRAG, 8, swapped);
	PUT32(buffer, UFS2_FS_BSHIFT, 13, swapped);
	PUT32(buffer, UFS2_FS_FSHIFT, 10, swapped);
	PUT32(buffer, UFS2_FS_FRAGSHIFT, 3, swapped);
	PUT32(buffer, UFS2_FS_FSBTODB, 1, swapped);
	PUT32(buffer, UFS2_FS_SBSIZE, UFS2_FS_STRUCT_SIZE, swapped);
	PUT32(buffer, UFS2_FS_NINDIR, 1024, swapped);
	PUT32(buffer, UFS2_FS_INOPB, 32, swapped);
	PUT32(buffer, UFS2_FS_CSSIZE, 0, swapped);
	PUT32(buffer, UFS2_FS_CGSIZE, 512, swapped);
	PUT32(buffer, UFS2_FS_IPG, 32, swapped);
	PUT32(buffer, UFS2_FS_FPG, 8192, swapped);
	PUT64(buffer, UFS2_FS_SBLOCKLOC, UFS2_SBLOCK_OFFSET, swapped);
	PUT64(buffer, UFS2_FS_SIZE, 8192, swapped);
	PUT64(buffer, UFS2_FS_DSIZE, 7000, swapped);
	PUT32(buffer, UFS2_FS_MAXSYMLINKLEN, 120, swapped);
	PUT64(buffer, UFS2_FS_MAXFILESIZE, UINT64_C(0x7fffffffffff), swapped);
	PUT32(buffer, UFS2_FS_MAGIC, UFS2_MAGIC, swapped);
}

static int
decode_superblock(const uint8_t *buffer, size_t length, uint64_t sectors,
	int *swapped)
{
	struct ufs2_super super;
	int error = ufs2_super_decode(buffer, length, sectors, &super);

	if (error == 0) {
		CHECK(super.size == 8192);
		CHECK(super.bsize == 8192);
		*swapped = super.swapped;
	}
	return error;
}
#endif

static void
test_superblock_decode(void)
{
	uint8_t buffer[8192];
	int swapped = -1;

	build_superblock(buffer, 0);
	CHECK(decode_superblock(buffer, sizeof(buffer), 16384, &swapped) == 0);
	CHECK(swapped == 0);

	build_superblock(buffer, 1);
	CHECK(decode_superblock(buffer, sizeof(buffer), 16384, &swapped) == 0);
	CHECK(swapped == 1);

	build_superblock(buffer, 0);
	PUT32(buffer, FS_MAGIC_OFFSET, UINT32_C(0xdeadbeef), 0);
	CHECK(decode_superblock(buffer, sizeof(buffer), 16384, &swapped) ==
	    EOPNOTSUPP);

	build_superblock(buffer, 0);
	CHECK(decode_superblock(buffer, FS_STRUCT_SIZE - 1U, 16384, &swapped) ==
	    EINVAL);

	build_superblock(buffer, 0);
	CHECK(decode_superblock(buffer, sizeof(buffer), 32, &swapped) == EINVAL);
}

int
main(void)
{
	test_endian_helpers();
	test_superblock_decode();
	printf("KA-T020 %s: PASS (%u checks)\n", FS_NAME, checks);
	return 0;
}
