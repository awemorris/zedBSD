/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UFS_DISK_H
#define ZEDBSD_UFS_DISK_H

#include <stddef.h>
#include <stdint.h>

#define UFS_SECTOR_SIZE 512U
#define UFS_SBLOCK_OFFSET 65536U
#define UFS_SBLOCK_SIZE 8192U
#define UFS_FS_STRUCT_SIZE 1376U
#define UFS_MAGIC 0x19540119U
#define UFS_DINODE_SIZE 256U
#define UFS_ROOT_INO 2U
#define UFS_NDADDR 12U
#define UFS_NIADDR 3U
#define UFS_DIRBLKSIZ 512U
#define UFS_NXADDR 2U

/* Native UFS extended-attribute record format. */
#define UFS_EXTATTR_NAMESPACE_USER 1U
#define UFS_EXTATTR_NAMESPACE_SYSTEM 2U
#define UFS_EXTATTR_HEADER_SIZE 7U

/* Canonical struct fs offsets for the unified UFS codec. */
#define UFS_FS_SBLKNO 8U
#define UFS_FS_CBLKNO 12U
#define UFS_FS_IBLKNO 16U
#define UFS_FS_DBLKNO 20U
#define UFS_FS_NCG 44U
#define UFS_FS_BSIZE 48U
#define UFS_FS_FSIZE 52U
#define UFS_FS_FRAG 56U
#define UFS_FS_BSHIFT 80U
#define UFS_FS_FSHIFT 84U
#define UFS_FS_FRAGSHIFT 96U
#define UFS_FS_FSBTODB 100U
#define UFS_FS_SBSIZE 104U
#define UFS_FS_NINDIR 116U
#define UFS_FS_INOPB 120U
#define UFS_FS_ID 144U
#define UFS_FS_CSSIZE 156U
#define UFS_FS_CGSIZE 160U
#define UFS_FS_IPG 184U
#define UFS_FS_FPG 188U
#define UFS_FS_CLEAN 209U
#define UFS_FS_VOLNAME 680U
#define UFS_FS_VOLNAME_SIZE 32U
#define UFS_FS_SBLOCKLOC 1000U
#define UFS_FS_CSTOTAL_NDIR 1008U
#define UFS_FS_CSTOTAL_NBFREE 1016U
#define UFS_FS_CSTOTAL_NIFREE 1024U
#define UFS_FS_CSTOTAL_NFFREE 1032U
#define UFS_FS_SIZE 1080U
#define UFS_FS_DSIZE 1088U
#define UFS_FS_CSADDR 1096U
#define UFS_FS_FLAGS 1312U
#define UFS_FS_MAXSYMLINKLEN 1320U
#define UFS_FS_MAXFILESIZE 1328U
#define UFS_FS_MAGIC 1372U

/* Canonical struct ufs_dinode offsets. */
#define UFS_DI_MODE 0U
#define UFS_DI_NLINK 2U
#define UFS_DI_UID 4U
#define UFS_DI_GID 8U
#define UFS_DI_BLKSIZE 12U
#define UFS_DI_SIZE 16U
#define UFS_DI_BLOCKS 24U
#define UFS_DI_ATIME 32U
#define UFS_DI_MTIME 40U
#define UFS_DI_CTIME 48U
#define UFS_DI_BIRTHTIME 56U
#define UFS_DI_MTIMENSEC 64U
#define UFS_DI_ATIMENSEC 68U
#define UFS_DI_CTIMENSEC 72U
#define UFS_DI_BIRTHNSEC 76U
#define UFS_DI_GEN 80U
#define UFS_DI_KERNFLAGS 84U
#define UFS_DI_FLAGS 88U
#define UFS_DI_EXTSIZE 92U
#define UFS_DI_EXTB 96U
#define UFS_DI_DB 112U
#define UFS_DI_IB 208U
#define UFS_DI_MODREV 232U

/* struct cg remains the canonical FFS cylinder-group format. */
#define UFS_CG_MAGIC_VALUE 0x00090255U
#define UFS_CG_MAGIC 4U
#define UFS_CG_CGX 12U
#define UFS_CG_NDBLK 20U
#define UFS_CG_NDIR 24U
#define UFS_CG_NBFREE 28U
#define UFS_CG_NIFREE 32U
#define UFS_CG_NFFREE 36U
#define UFS_CG_IUSEDOFF 92U
#define UFS_CG_FREEOFF 96U
#define UFS_CG_NEXTFREEOFF 100U

struct ufs_super {
	uint32_t sblkno, cblkno, iblkno, dblkno;
	uint32_t cgoffset, cgmask;
	uint32_t ncg, bsize, fsize, frag;
	uint32_t bshift, fshift, fragshift, fsbtodb;
	uint32_t sbsize, nindir, inopb, ipg, fpg, cssize, cgsize;
	uint64_t sblockloc, size, dsize, csaddr;
	uint64_t cstotal_ndir, cstotal_nbfree;
	uint64_t cstotal_nifree, cstotal_nffree;
	uint32_t flags, maxsymlinklen;
	uint64_t maxfilesize;
	uint8_t clean;
	int swapped;
};

#endif
