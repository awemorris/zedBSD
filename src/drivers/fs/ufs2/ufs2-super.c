/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "ufs2-super.h"
#include "ufs2-endian.h"

#include <errno.h>
#include <string.h>

static int
power2(uint32_t value)
{
	return value != 0 && (value & (value - 1U)) == 0;
}

int
ufs2_super_decode(const void *buffer, size_t length, uint64_t sectors,
	struct ufs2_super *super)
{
	uint64_t last_cg_start, inode_fragments, medium_fragments;
	uint32_t magic;
	int swapped;

	if (buffer == NULL || super == NULL || length < UFS2_FS_STRUCT_SIZE)
		return EINVAL;
	magic = ufs2_get32(buffer, UFS2_FS_MAGIC, 0);
	if (magic == UFS2_MAGIC)
		swapped = 0;
	else if (ufs2_get32(buffer, UFS2_FS_MAGIC, 1) == UFS2_MAGIC)
		swapped = 1;
	else
		return EOPNOTSUPP;
	memset(super, 0, sizeof(*super));
#define GET32(field, offset) super->field = ufs2_get32(buffer, offset, swapped)
#define GET64(field, offset) super->field = ufs2_get64(buffer, offset, swapped)
	GET32(sblkno,UFS2_FS_SBLKNO);GET32(cblkno,UFS2_FS_CBLKNO);
	GET32(iblkno,UFS2_FS_IBLKNO);GET32(dblkno,UFS2_FS_DBLKNO);
	GET32(ncg,UFS2_FS_NCG);GET32(bsize,UFS2_FS_BSIZE);
	GET32(fsize,UFS2_FS_FSIZE);GET32(frag,UFS2_FS_FRAG);
	GET32(bshift,UFS2_FS_BSHIFT);GET32(fshift,UFS2_FS_FSHIFT);
	GET32(fragshift,UFS2_FS_FRAGSHIFT);GET32(fsbtodb,UFS2_FS_FSBTODB);
	GET32(sbsize,UFS2_FS_SBSIZE);GET32(nindir,UFS2_FS_NINDIR);
	GET32(inopb,UFS2_FS_INOPB);GET32(cssize,UFS2_FS_CSSIZE);
	GET32(cgsize,UFS2_FS_CGSIZE);GET32(ipg,UFS2_FS_IPG);
	GET32(fpg,UFS2_FS_FPG);GET64(sblockloc,UFS2_FS_SBLOCKLOC);
	GET64(cstotal_ndir,UFS2_FS_CSTOTAL_NDIR);
	GET64(cstotal_nbfree,UFS2_FS_CSTOTAL_NBFREE);
	GET64(cstotal_nifree,UFS2_FS_CSTOTAL_NIFREE);
	GET64(cstotal_nffree,UFS2_FS_CSTOTAL_NFFREE);
	GET64(size,UFS2_FS_SIZE);GET64(dsize,UFS2_FS_DSIZE);
	GET64(csaddr,UFS2_FS_CSADDR);GET32(flags,UFS2_FS_FLAGS);
	GET32(maxsymlinklen,UFS2_FS_MAXSYMLINKLEN);
	GET64(maxfilesize,UFS2_FS_MAXFILESIZE);
#undef GET32
#undef GET64
	super->clean = *((const uint8_t *)buffer + UFS2_FS_CLEAN);
	super->swapped = swapped;
	if (super->fsize < UFS2_SECTOR_SIZE ||
	    super->fsize % UFS2_SECTOR_SIZE != 0)
		return EINVAL;
	medium_fragments = sectors / (super->fsize / UFS2_SECTOR_SIZE);
	last_cg_start = super->ncg == 0 ? UINT64_MAX :
	    (uint64_t)(super->ncg - 1U) * super->fpg;
	inode_fragments = super->inopb == 0 ? UINT64_MAX :
	    ((uint64_t)super->ipg + super->inopb - 1U) /
	    super->inopb * super->frag;
	if (!power2(super->bsize) || !power2(super->fsize) ||
	    super->bsize < super->fsize ||
	    super->bsize / super->fsize != super->frag ||
	    super->nindir != super->bsize / sizeof(uint64_t) ||
	    super->inopb != super->bsize / UFS2_DINODE_SIZE ||
	    super->sbsize < UFS2_FS_STRUCT_SIZE ||
	    super->sbsize > UFS2_SBLOCK_SIZE || super->ncg == 0 ||
	    super->ipg < 3U || super->fpg == 0 || super->size == 0 ||
	    super->dsize > super->size || super->size > medium_fragments ||
	    super->sblockloc != UFS2_SBLOCK_OFFSET ||
	    super->cgsize == 0 || super->cgsize > super->bsize ||
	    super->sblkno >= super->cblkno || super->cblkno >= super->iblkno ||
	    super->iblkno >= super->dblkno ||
	    inode_fragments > super->dblkno - super->iblkno ||
	    last_cg_start >= super->size ||
	    super->size - last_cg_start > super->fpg ||
	    last_cg_start + super->dblkno >= super->size ||
	    super->cstotal_ndir > (uint64_t)super->ncg * super->ipg ||
	    super->cstotal_nifree > (uint64_t)super->ncg * super->ipg ||
	    super->cstotal_nbfree > super->dsize / super->frag ||
	    super->cstotal_nffree > super->dsize)
		return EINVAL;
	return 0;
}
