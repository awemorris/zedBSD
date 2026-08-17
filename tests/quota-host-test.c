/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/quota.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int mutex_init(struct mutex *lock,enum lock_rank rank,const char *name)
{ (void)lock;(void)rank;(void)name;return 0; }
void mutex_lock(struct mutex *lock) { (void)lock; }
void mutex_unlock(struct mutex *lock) { (void)lock; }

int main(void)
{
	struct quota_state state;struct quota_record limits,result;
	struct quota_charge charge;
	unsigned char persisted[4096];size_t persisted_size;
	quota_state_init(&state);assert(quota_enable(&state,QUOTA_USER,1)==0);
	assert(quota_enable(&state,QUOTA_GROUP,1)==0);
	memset(&limits,0,sizeof(limits));limits.id=100;limits.block_soft=4;
	limits.block_hard=6;limits.inode_hard=2;
	assert(quota_set(&state,QUOTA_USER,&limits)==0);
	limits.id=200;limits.block_hard=8;limits.inode_hard=3;
	assert(quota_set(&state,QUOTA_GROUP,&limits)==0);
	assert(quota_reserve(&state,100,200,4,1,10,&charge)==0);
	quota_commit(&charge);assert(quota_get(&state,QUOTA_USER,100,&result)==0);
	assert(result.blocks==4&&result.inodes==1&&result.block_deadline==0);
	assert(quota_reserve(&state,100,200,1,1,20,&charge)==0);
	assert(quota_get(&state,QUOTA_USER,100,&result)==0);
	assert(result.blocks==5&&result.inodes==2&&result.block_deadline>20);
	quota_rollback(&charge);assert(quota_get(&state,QUOTA_USER,100,&result)==0);
	assert(result.blocks==4&&result.inodes==1&&result.block_deadline==0);
	assert(quota_reserve(&state,100,200,3,0,30,&charge)==EDQUOT);
	assert(quota_transfer(&state,100,200,101,201,4,1,40)==0);
	assert(quota_get(&state,QUOTA_USER,100,&result)==0&&result.blocks==0);
	assert(quota_get(&state,QUOTA_USER,101,&result)==0&&result.blocks==4);
	{
		struct quota_transfer transfer;
		assert(quota_transfer_begin(&state,101,201,102,202,4,1,50,
		    &transfer)==0);
		quota_transfer_rollback(&transfer);
		assert(quota_get(&state,QUOTA_USER,101,&result)==0&&
		    result.blocks==4&&result.inodes==1);
		assert(quota_get(&state,QUOTA_USER,102,&result)==0&&
		    result.blocks==0&&result.inodes==0);
	}
	assert(quota_release(&state,101,201,4,1)==0);
	assert(quota_release(&state,101,201,1,0)==EIO);
	assert(quota_export_config(&state,persisted,sizeof(persisted),
	    &persisted_size)==0);
	{
		struct quota_state restored;int enabled=0;
		quota_state_init(&restored);
		assert(quota_rebuild_add(&restored,100,200,3,2)==0);
		assert(quota_import_config(&restored,persisted,persisted_size)==0);
		assert(quota_enabled(&restored,QUOTA_USER,&enabled)==0&&enabled);
		assert(quota_get(&restored,QUOTA_USER,100,&result)==0);
		assert(result.blocks==3&&result.inodes==2&&result.block_hard==6);
		persisted[12]^=1;
		assert(quota_import_config(&restored,persisted,persisted_size)==EINVAL);
	}
	puts("zedBSD quota reservation tests: PASS");return 0;
}
