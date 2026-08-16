/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/ufs1/ufs1-super.h"
#include "kern/ufs1/ufs1-endian.h"
#include <errno.h>
#include <string.h>

static int power2(uint32_t value)
{ return value != 0 && (value & (value - 1U)) == 0; }

int
ufs1_super_decode(const void *buffer, size_t length, uint64_t sectors,
	struct ufs1_super *super)
{
	uint64_t inode_fragments, covered_fragments;
	uint32_t magic;
	int swapped;
	if (buffer == NULL || super == NULL || length < UFS1_FS_STRUCT_SIZE)
		return EINVAL;
	magic = ufs1_get32(buffer, UFS1_FS_MAGIC, 0);
	if (magic == UFS1_MAGIC)
		swapped = 0;
	else if (ufs1_get32(buffer, UFS1_FS_MAGIC, 1) == UFS1_MAGIC)
		swapped = 1;
	else
		return EOPNOTSUPP;
	memset(super, 0, sizeof(*super));
#define GET32(field, offset) super->field = ufs1_get32(buffer, offset, swapped)
	GET32(sblkno, UFS1_FS_SBLKNO); GET32(cblkno, UFS1_FS_CBLKNO);
	GET32(iblkno, UFS1_FS_IBLKNO); GET32(dblkno, UFS1_FS_DBLKNO);
	GET32(size, UFS1_FS_OLD_SIZE); GET32(dsize, UFS1_FS_OLD_DSIZE);
	GET32(ncg, UFS1_FS_NCG); GET32(bsize, UFS1_FS_BSIZE);
	GET32(fsize, UFS1_FS_FSIZE); GET32(frag, UFS1_FS_FRAG);
	GET32(bshift, UFS1_FS_BSHIFT); GET32(fshift, UFS1_FS_FSHIFT);
	GET32(fragshift, UFS1_FS_FRAGSHIFT); GET32(fsbtodb, UFS1_FS_FSBTODB);
	GET32(sbsize, UFS1_FS_SBSIZE); GET32(nindir, UFS1_FS_NINDIR);
	GET32(inopb, UFS1_FS_INOPB); GET32(csaddr, UFS1_FS_OLD_CSADDR);
	GET32(cssize, UFS1_FS_CSSIZE); GET32(cgsize, UFS1_FS_CGSIZE);
	GET32(ipg, UFS1_FS_IPG); GET32(fpg, UFS1_FS_FPG);
	GET32(maxsymlinklen, UFS1_FS_MAXSYMLINKLEN);
	super->inodefmt = (int32_t)ufs1_get32(buffer, UFS1_FS_INODEFMT,
	    swapped);
	super->maxfilesize = ufs1_get64(buffer, UFS1_FS_MAXFILESIZE, swapped);
	GET32(state, UFS1_FS_STATE);
#undef GET32
	super->clean = *((const uint8_t *)buffer + UFS1_FS_CLEAN);
	super->swapped = swapped;
	inode_fragments = ((uint64_t)super->ipg + super->inopb - 1U) /
	    super->inopb * super->frag;
	covered_fragments = (uint64_t)super->ncg * super->fpg;
	if (super->inodefmt != UFS1_44INODEFMT || super->bsize != 8192U ||
	    super->fsize != 1024U || super->frag != 8U ||
	    super->bshift != 13U || super->fshift != 10U ||
	    super->fragshift != 3U ||
	    super->nindir != super->bsize / 4U ||
	    super->inopb != super->bsize / UFS1_DINODE_SIZE ||
	    !power2(super->bsize) || !power2(super->fsize) ||
	    super->bsize / super->fsize != super->frag ||
	    super->fsbtodb != 1U || super->ncg == 0 || super->ipg < 3U ||
	    super->fpg == 0 || super->size == 0 || super->dsize > super->size ||
	    super->sbsize < UFS1_FS_STRUCT_SIZE ||
	    super->sbsize > UFS1_SBLOCK_SIZE ||
	    super->cgsize == 0 || super->cgsize > super->bsize ||
	    (uint64_t)super->size * (super->fsize / UFS1_SECTOR_SIZE) > sectors ||
	    super->sblkno >= super->size || super->cblkno >= super->size ||
	    super->iblkno >= super->size || super->dblkno >= super->size ||
	    super->sblkno >= super->cblkno || super->cblkno >= super->iblkno ||
	    super->iblkno >= super->dblkno ||
	    inode_fragments > super->dblkno - super->iblkno ||
	    covered_fragments < super->size ||
	    (super->ncg > 1U &&
	    (uint64_t)(super->ncg - 1U) * super->fpg >= super->size))
		return EINVAL;
	return 0;
}
