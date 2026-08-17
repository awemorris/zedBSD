/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/ufs-snapshot.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define SECTORS 96U
static unsigned char medium[SECTORS][512];
static unsigned io_operation,fail_operation;
static int read_io(void *c,uint64_t at,uint32_t n,void *p)
{ (void)c;if(at>SECTORS||n>SECTORS-at)return EIO;memcpy(p,medium[at],(size_t)n*512U);return 0; }
static int write_io(void *c,uint64_t at,uint32_t n,const void *p)
{ (void)c;if(at>SECTORS||n>SECTORS-at)return EIO;
	if(++io_operation==fail_operation)return EIO;
	memcpy(medium[at],p,(size_t)n*512U);return 0; }
static int flush_io(void *c){(void)c;return ++io_operation==fail_operation?EIO:0;}

int main(void)
{
	struct ufs_journal_io io={NULL,read_io,write_io,flush_io};
	struct ufs_snapshot snapshot,reopened;
	struct ufs_snapshot_entry map[64],map2[64];
	unsigned char before[1024],after[1024],view[1024];unsigned n;
	for(n=0;n<32;n++)memset(medium[n],(int)n,512);
	memcpy(before,medium[3],512);memcpy(before+512,medium[4],512);
	assert(ufs_snapshot_init(&snapshot,&io,32,32,63,map,64)==0);
	assert(ufs_snapshot_open(&snapshot)==0&&!snapshot.active);
	assert(ufs_snapshot_create(&snapshot)==0);
	assert(ufs_snapshot_preserve(&snapshot,3,2)==0);
	memset(after,0xa5,sizeof(after));assert(write_io(NULL,3,2,after)==0);
	assert(ufs_snapshot_preserve(&snapshot,3,2)==0&&snapshot.next_record==2);
	assert(ufs_snapshot_read(&snapshot,3,2,view)==0&&
	    memcmp(view,before,sizeof(view))==0);
	assert(ufs_snapshot_init(&reopened,&io,32,32,63,map2,64)==0);
	assert(ufs_snapshot_open(&reopened)==0&&reopened.active&&
	    reopened.next_record==2);
	assert(ufs_snapshot_read(&reopened,3,2,view)==0&&
	    memcmp(view,before,sizeof(view))==0);
	assert(ufs_snapshot_delete(&reopened)==0);
	assert(ufs_snapshot_open(&reopened)==0&&!reopened.active);
	/* Every persistence boundary either leaves an empty active snapshot or a
	 * committed before-image.  The live sector is not modified by this test. */
	for(n=1;n<=6;n++) {
		struct ufs_snapshot trial,recovery;
		struct ufs_snapshot_entry trial_map[64],recovery_map[64];
		unsigned char original[512],recovered[512];int error;
		memset(medium,0,sizeof(medium));memset(medium[7],0x37,512);
		memcpy(original,medium[7],512);io_operation=fail_operation=0;
		assert(ufs_snapshot_init(&trial,&io,32,32,63,trial_map,64)==0);
		assert(ufs_snapshot_create(&trial)==0);io_operation=0;fail_operation=n;
		error=ufs_snapshot_preserve(&trial,7,1);assert(error==EIO);
		fail_operation=0;io_operation=0;
		assert(ufs_snapshot_init(&recovery,&io,32,32,63,recovery_map,64)==0);
		assert(ufs_snapshot_open(&recovery)==0&&recovery.active);
		assert(ufs_snapshot_read(&recovery,7,1,recovered)==0);
		assert(memcmp(original,recovered,512)==0);
	}
	puts("zedBSD persistent block snapshot tests: PASS");return 0;
}
