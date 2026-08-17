/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/ufs1/ufs1-endian.h"
#include "kern/ufs2/ufs2-disk.h"
#include "kern/ufs2/ufs2-super.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void
make_super(uint8_t *data, int swapped)
{
	memset(data,0,UFS2_FS_STRUCT_SIZE);
#define PUT32(offset,value) ufs1_put32(data,(offset),(value),swapped)
#define PUT64(offset,value) ufs1_put64(data,(offset),(value),swapped)
	PUT32(UFS2_FS_SBLKNO,64);PUT32(UFS2_FS_CBLKNO,72);
	PUT32(UFS2_FS_IBLKNO,80);PUT32(UFS2_FS_DBLKNO,144);
	PUT32(UFS2_FS_NCG,2);PUT32(UFS2_FS_BSIZE,8192);
	PUT32(UFS2_FS_FSIZE,1024);PUT32(UFS2_FS_FRAG,8);
	PUT32(UFS2_FS_BSHIFT,13);PUT32(UFS2_FS_FSHIFT,10);
	PUT32(UFS2_FS_FRAGSHIFT,3);PUT32(UFS2_FS_FSBTODB,1);
	PUT32(UFS2_FS_SBSIZE,UFS2_FS_STRUCT_SIZE);
	PUT32(UFS2_FS_NINDIR,1024);PUT32(UFS2_FS_INOPB,32);
	PUT32(UFS2_FS_CGSIZE,1024);PUT32(UFS2_FS_IPG,256);
	PUT32(UFS2_FS_FPG,4096);PUT64(UFS2_FS_SBLOCKLOC,UFS2_SBLOCK_OFFSET);
	PUT64(UFS2_FS_CSTOTAL_NDIR,1);PUT64(UFS2_FS_CSTOTAL_NBFREE,900);
	PUT64(UFS2_FS_CSTOTAL_NIFREE,509);PUT64(UFS2_FS_CSTOTAL_NFFREE,0);
	PUT64(UFS2_FS_SIZE,8000);PUT64(UFS2_FS_DSIZE,7712);
	PUT64(UFS2_FS_CSADDR,144);PUT32(UFS2_FS_MAXSYMLINKLEN,120);
	PUT64(UFS2_FS_MAXFILESIZE,0x7fffffffffffffffULL);
	PUT32(UFS2_FS_MAGIC,UFS2_MAGIC);data[UFS2_FS_CLEAN]=1;
#undef PUT32
#undef PUT64
}

int
main(void)
{
	uint8_t data[UFS2_FS_STRUCT_SIZE];
	struct ufs2_super super;
	make_super(data,0);
	assert(ufs2_super_decode(data,sizeof(data),16000,&super)==0);
	assert(super.size==8000&&super.inopb==32&&!super.swapped);
	make_super(data,1);
	assert(ufs2_super_decode(data,sizeof(data),16000,&super)==0);
	assert(super.swapped&&super.sblockloc==UFS2_SBLOCK_OFFSET);
	ufs1_put32(data,UFS2_FS_NINDIR,2048,1);
	assert(ufs2_super_decode(data,sizeof(data),16000,&super)==EINVAL);
	make_super(data,0);ufs1_put64(data,UFS2_FS_SBLOCKLOC,8192,0);
	assert(ufs2_super_decode(data,sizeof(data),16000,&super)==EINVAL);
	make_super(data,0);ufs1_put64(data,UFS2_FS_SIZE,16001,0);
	assert(ufs2_super_decode(data,sizeof(data),16000,&super)==EINVAL);
	memset(data,0,sizeof(data));
	assert(ufs2_super_decode(data,sizeof(data),16000,&super)==EOPNOTSUPP);
	puts("zedBSD UFS2 disk-format codec tests: PASS");
	return 0;
}
