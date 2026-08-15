/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UFS1_DISK_H
#define ZEDBSD_UFS1_DISK_H

#include <stddef.h>
#include <stdint.h>

#define UFS1_SECTOR_SIZE 512U
#define UFS1_SBLOCK_OFFSET 8192U
#define UFS1_SBLOCK_SIZE 8192U
#define UFS1_FS_STRUCT_SIZE 1376U
#define UFS1_MAGIC 0x00011954U
#define UFS1_ROOT_INO 2U
#define UFS1_NDADDR 12U
#define UFS1_NIADDR 3U
#define UFS1_DINODE_SIZE 128U
#define UFS1_DIRBLKSIZ 512U
#define UFS1_44INODEFMT 2

/* Offsets in the 4.4BSD-derived UFS1 struct fs. */
#define UFS1_FS_SBLKNO 8U
#define UFS1_FS_CBLKNO 12U
#define UFS1_FS_IBLKNO 16U
#define UFS1_FS_DBLKNO 20U
#define UFS1_FS_OLD_SIZE 36U
#define UFS1_FS_OLD_DSIZE 40U
#define UFS1_FS_NCG 44U
#define UFS1_FS_BSIZE 48U
#define UFS1_FS_FSIZE 52U
#define UFS1_FS_FRAG 56U
#define UFS1_FS_BSHIFT 80U
#define UFS1_FS_FSHIFT 84U
#define UFS1_FS_FRAGSHIFT 96U
#define UFS1_FS_FSBTODB 100U
#define UFS1_FS_SBSIZE 104U
#define UFS1_FS_NINDIR 116U
#define UFS1_FS_INOPB 120U
#define UFS1_FS_OLD_CSADDR 152U
#define UFS1_FS_CSSIZE 156U
#define UFS1_FS_CGSIZE 160U
#define UFS1_FS_IPG 184U
#define UFS1_FS_FPG 188U
#define UFS1_FS_CSTOTAL_NDIR 192U
#define UFS1_FS_CSTOTAL_NBFREE 196U
#define UFS1_FS_CSTOTAL_NIFREE 200U
#define UFS1_FS_CSTOTAL_NFFREE 204U
#define UFS1_FS_CLEAN 209U
#define UFS1_FS_MAXSYMLINKLEN 1320U
#define UFS1_FS_INODEFMT 1324U
#define UFS1_FS_MAXFILESIZE 1328U
#define UFS1_FS_STATE 1352U
#define UFS1_FS_MAGIC 1372U

/* Offsets in struct ufs1_dinode. */
#define UFS1_DI_MODE 0U
#define UFS1_DI_NLINK 2U
#define UFS1_DI_SIZE 8U
#define UFS1_DI_ATIME 16U
#define UFS1_DI_ATIMENSEC 20U
#define UFS1_DI_MTIME 24U
#define UFS1_DI_MTIMENSEC 28U
#define UFS1_DI_CTIME 32U
#define UFS1_DI_CTIMENSEC 36U
#define UFS1_DI_DB 40U
#define UFS1_DI_IB 88U
#define UFS1_DI_FLAGS 100U
#define UFS1_DI_BLOCKS 104U
#define UFS1_DI_GEN 108U
#define UFS1_DI_UID 112U
#define UFS1_DI_GID 116U

#define UFS1_CG_MAGIC_VALUE 0x00090255U
#define UFS1_CG_MAGIC 4U
#define UFS1_CG_NDIR 24U
#define UFS1_CG_NBFREE 28U
#define UFS1_CG_NIFREE 32U
#define UFS1_CG_NFFREE 36U
#define UFS1_CG_IUSEDOFF 92U
#define UFS1_CG_FREEOFF 96U
#define UFS1_CG_NEXTFREEOFF 100U

struct ufs1_super {
	uint32_t sblkno, cblkno, iblkno, dblkno;
	uint32_t size, dsize, ncg;
	uint32_t bsize, fsize, frag;
	uint32_t bshift, fshift, fragshift, fsbtodb;
	uint32_t sbsize, nindir, inopb, ipg, fpg;
	uint32_t csaddr, cssize, cgsize;
	uint32_t maxsymlinklen;
	uint64_t maxfilesize;
	int32_t inodefmt;
	uint32_t state;
	uint8_t clean;
	int swapped;
};

#endif
