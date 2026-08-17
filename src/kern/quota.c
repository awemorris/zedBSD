/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/quota.h"

#include <errno.h>
#include <string.h>

#define QUOTA_DISK_VERSION 1U
#define QUOTA_DISK_HEADER_SIZE 32U
#define QUOTA_DISK_RECORD_SIZE 56U

static uint32_t quota_get32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t quota_get64(const uint8_t *p)
{ return quota_get32(p)|(uint64_t)quota_get32(p+4)<<32; }
static void quota_put32(uint8_t *p,uint32_t v)
{ p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
static void quota_put64(uint8_t *p,uint64_t v)
{ quota_put32(p,(uint32_t)v);quota_put32(p+4,(uint32_t)(v>>32)); }
static uint32_t
quota_digest(const uint8_t *p,size_t length)
{
	uint32_t value=2166136261U;size_t n;
	for(n=0;n<length;n++){uint8_t byte=n>=12U&&n<16U?0:p[n];
		value^=byte;value*=16777619U;}
	return value;
}

static struct quota_record *
quota_find(struct quota_state *state,enum quota_type type,uint32_t id,int create)
{
	struct quota_record *free_record=NULL;unsigned index;
	for(index=0;index<QUOTA_MAX_RECORDS;index++) {
		struct quota_record *record=&state->records[type][index];
		if(record->present&&record->id==id)return record;
		if(!record->present&&free_record==NULL)free_record=record;
	}
	if(!create||free_record==NULL)return NULL;
	memset(free_record,0,sizeof(*free_record));free_record->id=id;
	free_record->present=1;return free_record;
}

void
quota_state_init(struct quota_state *state)
{
	if(state==NULL)return;
	memset(state,0,sizeof(*state));
	(void)mutex_init(&state->lock,LOCK_RANK_DEVICE,"filesystem quota");
	state->grace_seconds=QUOTA_DEFAULT_GRACE_SECONDS;
}

int
quota_enable(struct quota_state *state,enum quota_type type,int enabled)
{
	if(state==NULL||type<QUOTA_USER||type>=QUOTA_TYPES)return EINVAL;
	mutex_lock(&state->lock);state->enabled[type]=enabled!=0;mutex_unlock(&state->lock);
	return 0;
}

int
quota_enabled(struct quota_state *state,enum quota_type type,int *enabled)
{
	if(state==NULL||enabled==NULL||type<QUOTA_USER||type>=QUOTA_TYPES)
		return EINVAL;
	mutex_lock(&state->lock);*enabled=state->enabled[type]!=0;
	mutex_unlock(&state->lock);return 0;
}

int
quota_get_grace(struct quota_state *state,uint64_t *seconds)
{
	if(state==NULL||seconds==NULL)return EINVAL;
	mutex_lock(&state->lock);*seconds=state->grace_seconds;
	mutex_unlock(&state->lock);return 0;
}

int
quota_set_grace(struct quota_state *state,uint64_t seconds)
{
	if(state==NULL||seconds==0)return EINVAL;
	mutex_lock(&state->lock);state->grace_seconds=seconds;
	mutex_unlock(&state->lock);return 0;
}

int
quota_get(struct quota_state *state,enum quota_type type,uint32_t id,
	struct quota_record *result)
{
	struct quota_record *record;
	if(state==NULL||result==NULL||type<QUOTA_USER||type>=QUOTA_TYPES)return EINVAL;
	mutex_lock(&state->lock);record=quota_find(state,type,id,0);
	if(record!=NULL)*result=*record;else memset(result,0,sizeof(*result));
	result->id=id;mutex_unlock(&state->lock);return 0;
}

int
quota_set(struct quota_state *state,enum quota_type type,
	const struct quota_record *source)
{
	struct quota_record *record;
	if(state==NULL||source==NULL||type<QUOTA_USER||type>=QUOTA_TYPES||
	    (source->block_hard!=0&&source->block_soft>source->block_hard)||
	    (source->inode_hard!=0&&source->inode_soft>source->inode_hard))return EINVAL;
	mutex_lock(&state->lock);record=quota_find(state,type,source->id,1);
	if(record==NULL){mutex_unlock(&state->lock);return ENOSPC;}
	record->block_soft=source->block_soft;record->block_hard=source->block_hard;
	record->inode_soft=source->inode_soft;record->inode_hard=source->inode_hard;
	if(record->blocks<=record->block_soft||record->block_soft==0)record->block_deadline=0;
	if(record->inodes<=record->inode_soft||record->inode_soft==0)record->inode_deadline=0;
	mutex_unlock(&state->lock);return 0;
}

static int
quota_check(struct quota_state *state,struct quota_record *record,
	uint64_t blocks,uint64_t inodes,uint64_t now)
{
	uint64_t new_blocks,new_inodes;
	if(blocks>UINT64_MAX-record->blocks||inodes>UINT64_MAX-record->inodes)
		return EDQUOT;
	new_blocks=record->blocks+blocks;new_inodes=record->inodes+inodes;
	if((record->block_hard!=0&&new_blocks>record->block_hard)||
	    (record->inode_hard!=0&&new_inodes>record->inode_hard))return EDQUOT;
	if(record->block_soft!=0&&new_blocks>record->block_soft&&
	    record->block_deadline!=0&&now>=record->block_deadline)return EDQUOT;
	if(record->inode_soft!=0&&new_inodes>record->inode_soft&&
	    record->inode_deadline!=0&&now>=record->inode_deadline)return EDQUOT;
	(void)state;return 0;
}

static void
quota_add(struct quota_state *state,struct quota_record *record,
	uint64_t blocks,uint64_t inodes,uint64_t now)
{
	record->blocks+=blocks;record->inodes+=inodes;
	if(record->block_soft!=0&&record->blocks>record->block_soft&&
	    record->block_deadline==0)record->block_deadline=now+state->grace_seconds;
	if(record->inode_soft!=0&&record->inodes>record->inode_soft&&
	    record->inode_deadline==0)record->inode_deadline=now+state->grace_seconds;
}

static void
quota_subtract(struct quota_record *record,uint64_t blocks,uint64_t inodes)
{
	record->blocks-=blocks;record->inodes-=inodes;
	if(record->block_soft==0||record->blocks<=record->block_soft)
		record->block_deadline=0;
	if(record->inode_soft==0||record->inodes<=record->inode_soft)
		record->inode_deadline=0;
}

int
quota_reserve(struct quota_state *state,uid_t uid,gid_t gid,uint64_t blocks,
	uint64_t inodes,uint64_t now,struct quota_charge *charge)
{
	struct quota_record *user=NULL,*group=NULL;int error=0;
	if(state==NULL||charge==NULL)return EINVAL;
	memset(charge,0,sizeof(*charge));mutex_lock(&state->lock);
	user=quota_find(state,QUOTA_USER,uid,1);
	group=quota_find(state,QUOTA_GROUP,gid,1);
	if(user==NULL||group==NULL)error=ENOSPC;
	if(error==0&&state->enabled[QUOTA_USER])
		error=quota_check(state,user,blocks,inodes,now);
	if(error==0&&state->enabled[QUOTA_GROUP])
		error=quota_check(state,group,blocks,inodes,now);
	if(error==0){quota_add(state,user,blocks,inodes,now);
		quota_add(state,group,blocks,inodes,now);
		charge->state=state;charge->uid=uid;charge->gid=gid;
		charge->blocks=blocks;charge->inodes=inodes;charge->active=1;}
	mutex_unlock(&state->lock);return error;
}

void quota_commit(struct quota_charge *charge)
{ if(charge!=NULL)charge->active=0; }

void
quota_rollback(struct quota_charge *charge)
{
	struct quota_record *record;
	if(charge==NULL||!charge->active||charge->state==NULL)return;
	mutex_lock(&charge->state->lock);
	if((record=quota_find(charge->state,QUOTA_USER,charge->uid,0))!=NULL)
		quota_subtract(record,charge->blocks,charge->inodes);
	if((record=quota_find(charge->state,QUOTA_GROUP,charge->gid,0))!=NULL)
		quota_subtract(record,charge->blocks,charge->inodes);
	mutex_unlock(&charge->state->lock);charge->active=0;
}

int
quota_release(struct quota_state *state,uid_t uid,gid_t gid,uint64_t blocks,
	uint64_t inodes)
{
	struct quota_record *user,*group;
	if(state==NULL)
		return EINVAL;
	mutex_lock(&state->lock);
	user=quota_find(state,QUOTA_USER,uid,0);group=quota_find(state,QUOTA_GROUP,gid,0);
	if((state->enabled[QUOTA_USER]&&(user==NULL||user->blocks<blocks||user->inodes<inodes))||
	    (state->enabled[QUOTA_GROUP]&&(group==NULL||group->blocks<blocks||group->inodes<inodes))) {
		mutex_unlock(&state->lock);return EIO;
	}
	if(user!=NULL)quota_subtract(user,blocks,inodes);
	if(group!=NULL)quota_subtract(group,blocks,inodes);
	mutex_unlock(&state->lock);return 0;
}

int
quota_transfer_begin(struct quota_state *state,uid_t old_uid,gid_t old_gid,
	uid_t new_uid,gid_t new_gid,uint64_t blocks,uint64_t inodes,uint64_t now,
	struct quota_transfer *transfer)
{
	struct quota_record *old_user,*old_group,*new_user,*new_group;
	int error=0;
	if(state==NULL||transfer==NULL)return EINVAL;
	memset(transfer,0,sizeof(*transfer));
	if(old_uid==new_uid&&old_gid==new_gid)return 0;
	mutex_lock(&state->lock);
	old_user=quota_find(state,QUOTA_USER,old_uid,0);
	old_group=quota_find(state,QUOTA_GROUP,old_gid,0);
	new_user=quota_find(state,QUOTA_USER,new_uid,1);
	new_group=quota_find(state,QUOTA_GROUP,new_gid,1);
	if(old_user==NULL||old_group==NULL||new_user==NULL||new_group==NULL||
	    old_user->blocks<blocks||old_user->inodes<inodes||
	    old_group->blocks<blocks||old_group->inodes<inodes)
		error=EIO;
	if(error==0&&old_uid!=new_uid&&state->enabled[QUOTA_USER])
		error=quota_check(state,new_user,blocks,inodes,now);
	if(error==0&&old_gid!=new_gid&&state->enabled[QUOTA_GROUP])
		error=quota_check(state,new_group,blocks,inodes,now);
	if(error==0) {
		if(old_uid!=new_uid) {
			quota_subtract(old_user,blocks,inodes);
			quota_add(state,new_user,blocks,inodes,now);
		}
		if(old_gid!=new_gid) {
			quota_subtract(old_group,blocks,inodes);
			quota_add(state,new_group,blocks,inodes,now);
		}
		transfer->state=state;transfer->old_uid=old_uid;
		transfer->old_gid=old_gid;transfer->new_uid=new_uid;
		transfer->new_gid=new_gid;transfer->blocks=blocks;
		transfer->inodes=inodes;transfer->now=now;transfer->active=1;
	}
	mutex_unlock(&state->lock);return error;
}

void quota_transfer_commit(struct quota_transfer *transfer)
{ if(transfer!=NULL)transfer->active=0; }

void
quota_transfer_rollback(struct quota_transfer *transfer)
{
	struct quota_record *old_user,*old_group,*new_user,*new_group;
	if(transfer==NULL||!transfer->active||transfer->state==NULL)return;
	mutex_lock(&transfer->state->lock);
	old_user=quota_find(transfer->state,QUOTA_USER,transfer->old_uid,1);
	old_group=quota_find(transfer->state,QUOTA_GROUP,transfer->old_gid,1);
	new_user=quota_find(transfer->state,QUOTA_USER,transfer->new_uid,0);
	new_group=quota_find(transfer->state,QUOTA_GROUP,transfer->new_gid,0);
	if(transfer->old_uid!=transfer->new_uid&&old_user!=NULL&&new_user!=NULL&&
	    new_user->blocks>=transfer->blocks&&new_user->inodes>=transfer->inodes) {
		quota_subtract(new_user,transfer->blocks,transfer->inodes);
		quota_add(transfer->state,old_user,transfer->blocks,
		    transfer->inodes,transfer->now);
	}
	if(transfer->old_gid!=transfer->new_gid&&old_group!=NULL&&new_group!=NULL&&
	    new_group->blocks>=transfer->blocks&&new_group->inodes>=transfer->inodes) {
		quota_subtract(new_group,transfer->blocks,transfer->inodes);
		quota_add(transfer->state,old_group,transfer->blocks,
		    transfer->inodes,transfer->now);
	}
	mutex_unlock(&transfer->state->lock);transfer->active=0;
}

int
quota_transfer(struct quota_state *state,uid_t old_uid,gid_t old_gid,
	uid_t new_uid,gid_t new_gid,uint64_t blocks,uint64_t inodes,uint64_t now)
{
	struct quota_transfer transfer;int error;
	error=quota_transfer_begin(state,old_uid,old_gid,new_uid,new_gid,blocks,
	    inodes,now,&transfer);
	if(error==0)quota_transfer_commit(&transfer);
	return error;
}

int
quota_rebuild_add(struct quota_state *state,uid_t uid,gid_t gid,uint64_t blocks,
	uint64_t inodes)
{
	struct quota_record *user,*group;
	if(state==NULL)
		return EINVAL;
	mutex_lock(&state->lock);
	user=quota_find(state,QUOTA_USER,uid,1);group=quota_find(state,QUOTA_GROUP,gid,1);
	if(user==NULL||group==NULL||blocks>UINT64_MAX-user->blocks||
	    blocks>UINT64_MAX-group->blocks||inodes>UINT64_MAX-user->inodes||
	    inodes>UINT64_MAX-group->inodes){mutex_unlock(&state->lock);return ENOSPC;}
	user->blocks+=blocks;user->inodes+=inodes;group->blocks+=blocks;group->inodes+=inodes;
	mutex_unlock(&state->lock);return 0;
}

int
quota_export_config(struct quota_state *state,void *buffer,size_t capacity,
	size_t *length)
{
	uint8_t *bytes=buffer;size_t needed=QUOTA_DISK_HEADER_SIZE,offset;
	unsigned type,index,count=0;
	if(state==NULL||length==NULL)return EINVAL;
	mutex_lock(&state->lock);
	for(type=0;type<QUOTA_TYPES;type++)for(index=0;index<QUOTA_MAX_RECORDS;index++) {
		struct quota_record *r=&state->records[type][index];
		if(r->present&&(r->block_soft!=0||r->block_hard!=0||
		    r->inode_soft!=0||r->inode_hard!=0||r->block_deadline!=0||
		    r->inode_deadline!=0)){needed+=QUOTA_DISK_RECORD_SIZE;count++;}
	}
	*length=needed;
	if(buffer==NULL||capacity<needed){mutex_unlock(&state->lock);
		return buffer==NULL?0:ENOSPC;}
	memset(bytes,0,needed);memcpy(bytes,"ZQ01",4);
	quota_put32(bytes+4,QUOTA_DISK_VERSION);quota_put32(bytes+8,(uint32_t)needed);
	quota_put32(bytes+16,(state->enabled[QUOTA_USER]?1U:0U)|
	    (state->enabled[QUOTA_GROUP]?2U:0U));quota_put32(bytes+20,count);
	quota_put64(bytes+24,state->grace_seconds);offset=QUOTA_DISK_HEADER_SIZE;
	for(type=0;type<QUOTA_TYPES;type++)for(index=0;index<QUOTA_MAX_RECORDS;index++) {
		struct quota_record *r=&state->records[type][index];uint8_t *entry;
		if(!r->present||(r->block_soft==0&&r->block_hard==0&&
		    r->inode_soft==0&&r->inode_hard==0&&r->block_deadline==0&&
		    r->inode_deadline==0))continue;
		entry=bytes+offset;quota_put32(entry,type);quota_put32(entry+4,r->id);
		quota_put64(entry+8,r->block_soft);quota_put64(entry+16,r->block_hard);
		quota_put64(entry+24,r->inode_soft);quota_put64(entry+32,r->inode_hard);
		quota_put64(entry+40,r->block_deadline);
		quota_put64(entry+48,r->inode_deadline);offset+=QUOTA_DISK_RECORD_SIZE;
	}
	quota_put32(bytes+12,quota_digest(bytes,needed));
	mutex_unlock(&state->lock);return 0;
}

int
quota_import_config(struct quota_state *state,const void *buffer,size_t length)
{
	const uint8_t *bytes=buffer;uint32_t enabled,count;uint64_t grace;
	unsigned index,type;size_t offset;
	if(state==NULL||buffer==NULL||length<QUOTA_DISK_HEADER_SIZE||
	    memcmp(bytes,"ZQ01",4)!=0||quota_get32(bytes+4)!=QUOTA_DISK_VERSION||
	    quota_get32(bytes+8)!=length||quota_get32(bytes+12)!=quota_digest(bytes,length))
		return EINVAL;
	enabled=quota_get32(bytes+16);count=quota_get32(bytes+20);
	grace=quota_get64(bytes+24);
	if((enabled&~3U)!=0||grace==0||count>(length-QUOTA_DISK_HEADER_SIZE)/
	    QUOTA_DISK_RECORD_SIZE||length!=QUOTA_DISK_HEADER_SIZE+
	    (size_t)count*QUOTA_DISK_RECORD_SIZE)return EINVAL;
	for(index=0,offset=QUOTA_DISK_HEADER_SIZE;index<count;
	    index++,offset+=QUOTA_DISK_RECORD_SIZE) {
		uint64_t bs=quota_get64(bytes+offset+8),bh=quota_get64(bytes+offset+16);
		uint64_t is=quota_get64(bytes+offset+24),ih=quota_get64(bytes+offset+32);
		unsigned prior;uint32_t id=quota_get32(bytes+offset+4);
		type=quota_get32(bytes+offset);
		if(type>=QUOTA_TYPES||(bh!=0&&bs>bh)||(ih!=0&&is>ih))return EINVAL;
		for(prior=0;prior<index;prior++) {
			size_t before=QUOTA_DISK_HEADER_SIZE+
			    (size_t)prior*QUOTA_DISK_RECORD_SIZE;
			if(quota_get32(bytes+before)==type&&
			    quota_get32(bytes+before+4)==id)return EINVAL;
		}
	}
	mutex_lock(&state->lock);
	/* Preflight record capacity without changing the active policy. */
	for(type=0;type<QUOTA_TYPES;type++) {
		unsigned free_count=0,missing=0,n;
		for(n=0;n<QUOTA_MAX_RECORDS;n++)if(!state->records[type][n].present)free_count++;
		for(index=0,offset=QUOTA_DISK_HEADER_SIZE;index<count;
		    index++,offset+=QUOTA_DISK_RECORD_SIZE)if(quota_get32(bytes+offset)==type&&
		    quota_find(state,(enum quota_type)type,quota_get32(bytes+offset+4),0)==NULL)
			missing++;
		if(missing>free_count){mutex_unlock(&state->lock);return ENOSPC;}
	}
	for(type=0;type<QUOTA_TYPES;type++)for(index=0;index<QUOTA_MAX_RECORDS;index++) {
		struct quota_record *r=&state->records[type][index];
		if(r->present){r->block_soft=r->block_hard=r->inode_soft=r->inode_hard=0;
			r->block_deadline=r->inode_deadline=0;}
	}
	for(index=0,offset=QUOTA_DISK_HEADER_SIZE;index<count;
	    index++,offset+=QUOTA_DISK_RECORD_SIZE) {
		struct quota_record *r;type=quota_get32(bytes+offset);
		r=quota_find(state,(enum quota_type)type,quota_get32(bytes+offset+4),1);
		r->block_soft=quota_get64(bytes+offset+8);
		r->block_hard=quota_get64(bytes+offset+16);
		r->inode_soft=quota_get64(bytes+offset+24);
		r->inode_hard=quota_get64(bytes+offset+32);
		r->block_deadline=quota_get64(bytes+offset+40);
		r->inode_deadline=quota_get64(bytes+offset+48);
	}
	state->enabled[QUOTA_USER]=(enabled&1U)!=0;
	state->enabled[QUOTA_GROUP]=(enabled&2U)!=0;state->grace_seconds=grace;
	mutex_unlock(&state->lock);return 0;
}
