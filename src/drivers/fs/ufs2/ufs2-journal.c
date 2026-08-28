/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "ufs2-consistency.h"

#include <errno.h>
#include <string.h>

#define SECTOR_SIZE 512U
#define DESC_MAGIC 0x4a534655U /* UFSJ */
#define COMMIT_MAGIC 0x434a4655U /* UFJC */
#define JOURNAL_VERSION 1U

static uint32_t
checksum(const void *buffer, size_t length)
{
	const uint8_t *bytes = buffer;
	uint32_t value = 2166136261U;
	size_t index;
	for (index = 0; index < length; index++) {
		value ^= bytes[index];
		value *= 16777619U;
	}
	return value;
}

static void put32(uint8_t *p,uint32_t v)
{ p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
static void put64(uint8_t *p,uint64_t v)
{ put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32)); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t get64(const uint8_t *p)
{ return get32(p)|(uint64_t)get32(p+4)<<32; }

int
ufs2_journal_init(struct ufs2_journal *journal,
	const struct ufs2_journal_io *io, uint64_t first, uint32_t count)
{
	if (journal == NULL || io == NULL || io->read == NULL ||
	    io->write == NULL || io->flush == NULL || count < 3U ||
	    first > UINT64_MAX - count)
		return EINVAL;
	memset(journal,0,sizeof(*journal));
	journal->io=*io;journal->first_sector=first;
	journal->sector_count=count;journal->next_sequence=1;
	return 0;
}

static int
clear_descriptor(struct ufs2_journal *journal)
{
	uint8_t zero[SECTOR_SIZE];
	int error;
	memset(zero,0,sizeof(zero));
	error=journal->io.write(journal->io.context,journal->first_sector,1,zero);
	return error!=0?error:journal->io.flush(journal->io.context);
}

int
ufs2_journal_commit(struct ufs2_journal *journal,uint64_t target,
	const void *payload,uint32_t sectors)
{
	uint8_t descriptor[SECTOR_SIZE],commit[SECTOR_SIZE];
	uint64_t sequence;
	uint32_t digest;
	int error,recovery_error;
	if(journal==NULL||payload==NULL||sectors==0||
	    sectors>journal->sector_count-2U||target>UINT64_MAX-sectors||
	    (target<journal->first_sector+journal->sector_count&&
	    journal->first_sector<target+sectors))
		return EINVAL;
	if(journal->poisoned)
		return EIO;
	if(journal->next_sequence==0||journal->next_sequence==UINT64_MAX)
		return EOVERFLOW;
	sequence=journal->next_sequence++;
	digest=checksum(payload,(size_t)sectors*SECTOR_SIZE);
	memset(descriptor,0,sizeof(descriptor));
	put32(descriptor+0,DESC_MAGIC);put32(descriptor+4,JOURNAL_VERSION);
	put64(descriptor+8,sequence);put64(descriptor+16,target);
	put32(descriptor+24,sectors);put32(descriptor+28,digest);
	put32(descriptor+32,checksum(descriptor,32));
	memset(commit,0,sizeof(commit));put32(commit+0,COMMIT_MAGIC);
	put64(commit+8,sequence);put32(commit+16,digest);
	put32(commit+20,checksum(commit,20));
	error=journal->io.write(journal->io.context,journal->first_sector,1,
	    descriptor);
	if(error==0)error=journal->io.flush(journal->io.context);
	if(error==0)error=journal->io.write(journal->io.context,
	    journal->first_sector+1U,sectors,payload);
	if(error==0)error=journal->io.flush(journal->io.context);
	if(error==0)error=journal->io.write(journal->io.context,
	    journal->first_sector+1U+sectors,1,commit);
	if(error==0)error=journal->io.flush(journal->io.context);
	if(error==0)error=journal->io.write(journal->io.context,target,sectors,
	    payload);
	if(error==0)error=journal->io.flush(journal->io.context);
	if(error==0)error=clear_descriptor(journal);
	if(error!=0) {
		/* The caller may continue only after the on-disk slot has reached a
		 * definite empty or replayed state.  A transient error can therefore
		 * fail this operation without poisoning the mounted filesystem. */
		recovery_error=ufs2_journal_replay(journal);
		if(recovery_error!=0)
			journal->poisoned=1;
	}
	return error;
}

int
ufs2_journal_replay(struct ufs2_journal *journal)
{
	uint8_t descriptor[SECTOR_SIZE],commit[SECTOR_SIZE];
	uint8_t sector[SECTOR_SIZE];
	uint64_t sequence,target;
	uint32_t sectors,digest,index;
	int error;
	if(journal==NULL)return EINVAL;
	error=journal->io.read(journal->io.context,journal->first_sector,1,
	    descriptor);
	if(error!=0)return error;
	if(get32(descriptor)==0)return 0;
	if(get32(descriptor)!=DESC_MAGIC||get32(descriptor+4)!=JOURNAL_VERSION||
	    get32(descriptor+32)!=checksum(descriptor,32))return EIO;
	sequence=get64(descriptor+8);target=get64(descriptor+16);
	sectors=get32(descriptor+24);digest=get32(descriptor+28);
	if(sectors==0||sectors>journal->sector_count-2U||
	    sequence==0||target>UINT64_MAX-sectors||
	    (target<journal->first_sector+journal->sector_count&&
	    journal->first_sector<target+sectors))return EIO;
	error=journal->io.read(journal->io.context,
	    journal->first_sector+1U+sectors,1,commit);
	if(error!=0)return error;
	/* An uncommitted descriptor is discarded; home storage was not touched. */
	if(get32(commit)!=COMMIT_MAGIC||get64(commit+8)!=sequence||
	    get32(commit+16)!=digest||get32(commit+20)!=checksum(commit,20))
		return clear_descriptor(journal);
	for(index=0;index<sectors;index++) {
		error=journal->io.read(journal->io.context,
		    journal->first_sector+1U+index,1,sector);
		if(error!=0)return error;
		/* Validate the complete payload before changing any home block. */
		if(index==0) {
			uint32_t running=2166136261U,part,byte;
			for(part=0;part<sectors;part++) {
				error=journal->io.read(journal->io.context,
				    journal->first_sector+1U+part,1,sector);
				if(error!=0)return error;
				for(byte=0;byte<SECTOR_SIZE;byte++){
					running^=sector[byte];running*=16777619U;
				}
			}
			if(running!=digest)return EIO;
		}
	}
	for(index=0;index<sectors;index++) {
		error=journal->io.read(journal->io.context,
		    journal->first_sector+1U+index,1,sector);
		if(error==0)error=journal->io.write(journal->io.context,
		    target+index,1,sector);
		if(error!=0)return error;
	}
	error=journal->io.flush(journal->io.context);
	if(error==0)error=clear_descriptor(journal);
	if(error==0&&sequence>=journal->next_sequence)
		journal->next_sequence=sequence+1U;
	return error;
}
