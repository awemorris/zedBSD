/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "ufs-super.h"
#include "ufs-endian.h"

#include <errno.h>
#include <string.h>

/* Supports the power2 operation. */
static int
power2(
	uint32_t value)
{
	return value != 0 && (value & (value - 1U)) == 0;
}

/*
 * Implements the ufs super decode operation.
 */
int
ufs_super_decode(
	const void *buffer,
	size_t length,
	uint64_t sectors,
	struct ufs_super *super)
{
	uint64_t last_cg_start, inode_fragments, medium_fragments;
	uint32_t magic;
	int swapped;

	if (buffer == NULL || super == NULL || length < UFS_FS_STRUCT_SIZE)
		return EINVAL;
	magic = ufs_get32(buffer, UFS_FS_MAGIC, 0);
	if (magic == UFS_MAGIC)
		swapped = 0;
	else if (ufs_get32(buffer, UFS_FS_MAGIC, 1) == UFS_MAGIC)
		swapped = 1;
	else
		return EOPNOTSUPP;
	memset(super, 0, sizeof(*super));
#define GET32(field, offset) super->field = ufs_get32(buffer, offset, swapped)
#define GET64(field, offset) super->field = ufs_get64(buffer, offset, swapped)
	GET32(sblkno, UFS_FS_SBLKNO);
	GET32(cblkno, UFS_FS_CBLKNO);
	GET32(iblkno, UFS_FS_IBLKNO);
	GET32(dblkno, UFS_FS_DBLKNO);
	GET32(ncg, UFS_FS_NCG);
	GET32(bsize, UFS_FS_BSIZE);
	GET32(fsize, UFS_FS_FSIZE);
	GET32(frag, UFS_FS_FRAG);
	GET32(bshift, UFS_FS_BSHIFT);
	GET32(fshift, UFS_FS_FSHIFT);
	GET32(fragshift, UFS_FS_FRAGSHIFT);
	GET32(fsbtodb, UFS_FS_FSBTODB);
	GET32(sbsize, UFS_FS_SBSIZE);
	GET32(nindir, UFS_FS_NINDIR);
	GET32(inopb, UFS_FS_INOPB);
	GET32(cssize, UFS_FS_CSSIZE);
	GET32(cgsize, UFS_FS_CGSIZE);
	GET32(ipg, UFS_FS_IPG);
	GET32(fpg, UFS_FS_FPG);
	GET64(sblockloc, UFS_FS_SBLOCKLOC);
	GET64(cstotal_ndir, UFS_FS_CSTOTAL_NDIR);
	GET64(cstotal_nbfree, UFS_FS_CSTOTAL_NBFREE);
	GET64(cstotal_nifree, UFS_FS_CSTOTAL_NIFREE);
	GET64(cstotal_nffree, UFS_FS_CSTOTAL_NFFREE);
	GET64(size, UFS_FS_SIZE);
	GET64(dsize, UFS_FS_DSIZE);
	GET64(csaddr, UFS_FS_CSADDR);
	GET32(flags, UFS_FS_FLAGS);
	GET32(maxsymlinklen, UFS_FS_MAXSYMLINKLEN);
	GET64(maxfilesize, UFS_FS_MAXFILESIZE);
#undef GET32
#undef GET64
	super->clean = *((const uint8_t *)buffer + UFS_FS_CLEAN);
	super->swapped = swapped;
	if (super->fsize < UFS_SECTOR_SIZE ||
	    super->fsize % UFS_SECTOR_SIZE != 0)
		return EINVAL;
	medium_fragments = sectors / (super->fsize / UFS_SECTOR_SIZE);
	last_cg_start = super->ncg == 0
			    ? UINT64_MAX
			    : (uint64_t)(super->ncg - 1U) * super->fpg;
	inode_fragments = super->inopb == 0
			      ? UINT64_MAX
			      : ((uint64_t)super->ipg + super->inopb - 1U) /
				    super->inopb * super->frag;
	if (!power2(super->bsize) || !power2(super->fsize) ||
	    super->bsize < super->fsize || super->bsize > 65536U ||
	    super->bshift >= 32U || super->fshift >= 32U ||
	    super->fragshift >= 32U || super->fsbtodb >= 32U ||
	    (UINT64_C(1) << super->bshift) != super->bsize ||
	    (UINT64_C(1) << super->fshift) != super->fsize ||
	    (UINT64_C(1) << super->fragshift) != super->frag ||
	    (UINT64_C(512) << super->fsbtodb) != super->fsize ||
	    super->bsize / super->fsize != super->frag ||
	    super->nindir != super->bsize / sizeof(uint64_t) ||
	    super->inopb != super->bsize / UFS_DINODE_SIZE ||
	    super->sbsize < UFS_FS_STRUCT_SIZE ||
	    super->sbsize > UFS_SBLOCK_SIZE || super->ncg == 0 ||
	    super->ipg < 3U || super->ipg > UINT32_MAX - 7U ||
	    super->fpg == 0 || super->fpg > UINT32_MAX - 7U ||
	    (uint64_t)super->ncg * super->ipg > UINT32_MAX ||
	    super->size == 0 || super->dsize > super->size ||
	    super->size > medium_fragments ||
	    super->sblockloc != UFS_SBLOCK_OFFSET || super->cgsize == 0 ||
	    super->cgsize > super->bsize || super->sblkno >= super->cblkno ||
	    super->cblkno >= super->iblkno || super->iblkno >= super->dblkno ||
	    inode_fragments > super->dblkno - super->iblkno ||
	    last_cg_start >= super->size ||
	    super->size - last_cg_start > super->fpg ||
	    super->dblkno >= super->size - last_cg_start ||
	    super->cstotal_ndir > (uint64_t)super->ncg * super->ipg ||
	    super->cstotal_nifree > (uint64_t)super->ncg * super->ipg ||
	    super->cstotal_nbfree > super->dsize / super->frag ||
	    super->cstotal_nffree > super->dsize)
		return EINVAL;
	return 0;
}
