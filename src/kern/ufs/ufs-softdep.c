/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/ufs-consistency.h"
#include <errno.h>
#include <string.h>

void ufs_softdep_init(struct ufs_softdep *tracker)
{ if(tracker!=NULL){memset(tracker,0,sizeof(*tracker));tracker->next_id=1;} }

static int find(const struct ufs_softdep *tracker,uint64_t id)
{ unsigned n;for(n=0;n<UFS_SOFTDEP_MAX;n++)if(tracker->entries[n].active&&tracker->entries[n].id==id)return (int)n;return -1; }

int ufs_softdep_add(struct ufs_softdep *tracker,uint64_t block,uint64_t *id)
{
	unsigned n;
	if(tracker==NULL||id==NULL||block==0)return EINVAL;
	for(n=0;n<UFS_SOFTDEP_MAX;n++)if(!tracker->entries[n].active){
		tracker->entries[n].active=1;tracker->entries[n].id=tracker->next_id++;
		tracker->entries[n].block=block;tracker->entries[n].prerequisites=0;
		tracker->count++;*id=tracker->entries[n].id;return 0;
	}
	return ENOSPC;
}

int ufs_softdep_depend(struct ufs_softdep *tracker,uint64_t after,uint64_t before)
{
	int a=find(tracker,after),b=find(tracker,before);
	if(a<0||b<0||a==b)return EINVAL;
	tracker->entries[a].prerequisites|=(uint64_t)1U<<b;
	return 0;
}

int ufs_softdep_drain(struct ufs_softdep *tracker,ufs_softdep_write_fn write,
	void *context)
{
	unsigned remaining,n,other,progress;
	int error;
	if(tracker==NULL||write==NULL)return EINVAL;
	remaining=tracker->count;
	while(remaining!=0){
		progress=0;
		for(n=0;n<UFS_SOFTDEP_MAX;n++)if(tracker->entries[n].active&&
		    tracker->entries[n].prerequisites==0){
			error=write(context,tracker->entries[n].block);if(error!=0)return error;
			tracker->entries[n].active=0;
			/* Make completion persistent in the tracker.  A later write may
			 * fail and the owner must be able to retry drain without the
			 * already durable prerequisite being mistaken for a cycle. */
			for(other=0;other<UFS_SOFTDEP_MAX;other++)
				tracker->entries[other].prerequisites&=
				    ~((uint64_t)1U<<n);
			tracker->count--;remaining--;progress=1;
		}
		if(!progress)return EDEADLK;
	}
	return 0;
}
