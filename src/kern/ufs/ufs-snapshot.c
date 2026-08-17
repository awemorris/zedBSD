/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/ufs-snapshot.h"

#include <errno.h>
#include <string.h>

#define SECTOR_SIZE 512U
#define SNAPSHOT_VERSION 1U
#define SNAPSHOT_ACTIVE 1U
#define RECORD_MAGIC 0x52534e5aU /* ZNSR */

static void put32(uint8_t *p,uint32_t v)
{ p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
static void put64(uint8_t *p,uint64_t v)
{ put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32)); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t get64(const uint8_t *p)
{ return get32(p)|(uint64_t)get32(p+4)<<32; }
static uint32_t digest(const void *buffer,size_t length)
{
	const uint8_t *p=buffer;uint32_t value=2166136261U;size_t n;
	for(n=0;n<length;n++){value^=p[n];value*=16777619U;}return value;
}

static size_t hash_sector(uint64_t sector,size_t count)
{ sector^=sector>>33;sector*=0xff51afd7ed558ccdULL;sector^=sector>>33;
	return (size_t)(sector%count); }
static struct ufs_snapshot_entry *
map_find(struct ufs_snapshot *snapshot,uint64_t sector,int insert)
{
	size_t start=hash_sector(sector,snapshot->map_count),slot=start;
	do {struct ufs_snapshot_entry *entry=&snapshot->map[slot];
		if(entry->sector==sector)return entry;
		if(entry->sector==UFS_SNAPSHOT_EMPTY)return insert?entry:NULL;
		slot=(slot+1U)%snapshot->map_count;
	} while(slot!=start);
	return NULL;
}
static void map_clear(struct ufs_snapshot *snapshot)
{ size_t n;for(n=0;n<snapshot->map_count;n++){
		snapshot->map[n].sector=UFS_SNAPSHOT_EMPTY;snapshot->map[n].record=0;} }

static int
write_control(struct ufs_snapshot *snapshot,unsigned active,uint32_t next)
{
	uint8_t sector[SECTOR_SIZE];int error;
	memset(sector,0,sizeof(sector));memcpy(sector,"ZSN1",4);
	put32(sector+4,SNAPSHOT_VERSION);put32(sector+8,active?SNAPSHOT_ACTIVE:0);
	put32(sector+12,next);put32(sector+16,snapshot->max_records);
	put64(sector+24,snapshot->volume_sectors);
	put32(sector+32,digest(sector,32));
	error=snapshot->io.write(snapshot->io.context,snapshot->first_sector,1,sector);
	return error!=0?error:snapshot->io.flush(snapshot->io.context);
}

int
ufs_snapshot_init(struct ufs_snapshot *snapshot,const struct ufs_journal_io *io,
	uint64_t volume,uint64_t first,uint32_t sectors,
	struct ufs_snapshot_entry *map,size_t map_count)
{
	uint32_t records;
	if(snapshot==NULL||io==NULL||io->read==NULL||io->write==NULL||
	    io->flush==NULL||volume==0||sectors<3U||map==NULL||map_count<2U)
		return EINVAL;
	records=(sectors-1U)/2U;
	if(records==0||map_count<(size_t)records*2U)return EINVAL;
	memset(snapshot,0,sizeof(*snapshot));snapshot->io=*io;
	snapshot->volume_sectors=volume;snapshot->first_sector=first;
	snapshot->sector_count=sectors;snapshot->max_records=records;
	snapshot->map=map;snapshot->map_count=map_count;map_clear(snapshot);
	return 0;
}

int
ufs_snapshot_open(struct ufs_snapshot *snapshot)
{
	uint8_t control[SECTOR_SIZE],header[SECTOR_SIZE],data[SECTOR_SIZE];
	uint32_t count,record;int error;
	if(snapshot==NULL)return EINVAL;
	map_clear(snapshot);snapshot->active=0;snapshot->next_record=0;
	error=snapshot->io.read(snapshot->io.context,snapshot->first_sector,1,control);
	if(error!=0)return error;
	if(memcmp(control,"ZSN1",4)!=0)return 0;
	if(get32(control+4)!=SNAPSHOT_VERSION||get32(control+16)!=snapshot->max_records||
	    get64(control+24)!=snapshot->volume_sectors||
	    get32(control+32)!=digest(control,32))return EIO;
	if(get32(control+8)==0)return 0;
	if(get32(control+8)!=SNAPSHOT_ACTIVE)return EIO;
	count=get32(control+12);if(count>snapshot->max_records)return EIO;
	for(record=0;record<count;record++) {
		struct ufs_snapshot_entry *entry;uint64_t target;
		error=snapshot->io.read(snapshot->io.context,
		    snapshot->first_sector+1U+(uint64_t)record*2U,1,header);
		if(error==0)error=snapshot->io.read(snapshot->io.context,
		    snapshot->first_sector+2U+(uint64_t)record*2U,1,data);
		if(error!=0)return error;
		target=get64(header+8);
		if(get32(header)!=RECORD_MAGIC||get32(header+4)!=SNAPSHOT_VERSION||
		    target>=snapshot->volume_sectors||get32(header+16)!=digest(data,sizeof(data))||
		    get32(header+20)!=digest(header,20)||(entry=map_find(snapshot,target,1))==NULL||
		    entry->sector!=UFS_SNAPSHOT_EMPTY)return EIO;
		entry->sector=target;entry->record=record;
	}
	snapshot->next_record=count;snapshot->active=1;return 0;
}

int
ufs_snapshot_create(struct ufs_snapshot *snapshot)
{
	int error;
	if(snapshot==NULL)return EINVAL;
	if(snapshot->active)return EBUSY;
	error=write_control(snapshot,1,0);
	if(error==0){map_clear(snapshot);snapshot->next_record=0;snapshot->active=1;}
	return error;
}

int
ufs_snapshot_preserve(struct ufs_snapshot *snapshot,uint64_t first,uint32_t count)
{
	uint8_t data[SECTOR_SIZE],header[SECTOR_SIZE];uint32_t n;int error=0;
	if(snapshot==NULL||count==0||first>=snapshot->volume_sectors||
	    count>snapshot->volume_sectors-first)return EINVAL;
	if(!snapshot->active)return 0;
	for(n=0;n<count;n++) {
		uint64_t target=first+n;uint32_t record;struct ufs_snapshot_entry *entry;
		entry=map_find(snapshot,target,1);if(entry==NULL)return ENOSPC;
		if(entry->sector==target)continue;
		if(snapshot->next_record>=snapshot->max_records)return ENOSPC;
		record=snapshot->next_record;
		error=snapshot->io.read(snapshot->io.context,target,1,data);
		if(error!=0)return error;
		error=snapshot->io.write(snapshot->io.context,
		    snapshot->first_sector+2U+(uint64_t)record*2U,1,data);
		if(error==0)error=snapshot->io.flush(snapshot->io.context);
		memset(header,0,sizeof(header));put32(header,RECORD_MAGIC);
		put32(header+4,SNAPSHOT_VERSION);put64(header+8,target);
		put32(header+16,digest(data,sizeof(data)));put32(header+20,digest(header,20));
		if(error==0)error=snapshot->io.write(snapshot->io.context,
		    snapshot->first_sector+1U+(uint64_t)record*2U,1,header);
		if(error==0)error=snapshot->io.flush(snapshot->io.context);
		if(error==0)error=write_control(snapshot,1,record+1U);
		if(error!=0)return error;
		entry->sector=target;entry->record=record;snapshot->next_record=record+1U;
	}
	return 0;
}

int
ufs_snapshot_read(struct ufs_snapshot *snapshot,uint64_t first,uint32_t count,
	void *buffer)
{
	uint8_t *bytes=buffer;uint32_t n;int error;
	if(snapshot==NULL||buffer==NULL||count==0||!snapshot->active||
	    first>=snapshot->volume_sectors||count>snapshot->volume_sectors-first)
		return EINVAL;
	for(n=0;n<count;n++) {
		struct ufs_snapshot_entry *entry=map_find(snapshot,first+n,0);
		uint64_t source=entry==NULL?first+n:snapshot->first_sector+2U+
		    (uint64_t)entry->record*2U;
		error=snapshot->io.read(snapshot->io.context,source,1,
		    bytes+(size_t)n*SECTOR_SIZE);if(error!=0)return error;
	}
	return 0;
}

int
ufs_snapshot_delete(struct ufs_snapshot *snapshot)
{
	int error;
	if(snapshot==NULL)return EINVAL;
	if(!snapshot->active)return ENOENT;
	error=write_control(snapshot,0,0);
	if(error==0){snapshot->active=0;snapshot->next_record=0;map_clear(snapshot);}
	return error;
}
