/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Conservative FreeBSD-derived UFS2 VFS implementation. */
#include "kern/ufs2.h"
#include "kern/ufs2/ufs2-disk.h"
#include "kern/ufs1/ufs1-endian.h"
#include "kern/ufs2/ufs2-super.h"
#include "kern/disk.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/mount.h"
#include "kern/namei.h"
#include "kern/pipe.h"
#include "kern/quota.h"
#include "kern/test-fault.h"
#include "kern/ufs-consistency.h"
#include "kern/ufs-snapshot.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/statvfs.h>
#include <zedbsd/quota.h>
#include <zedbsd/snapshot.h>

void clock_realtime(time_t *,long *);

#define UFS2_IFMT 0170000U
#define UFS2_IFIFO 0010000U
#define UFS2_IFCHR 0020000U
#define UFS2_IFDIR 0040000U
#define UFS2_IFBLK 0060000U
#define UFS2_IFREG 0100000U
#define UFS2_IFLNK 0120000U
#define UFS2_IFSOCK 0140000U
#define UFS2_QUOTA_XATTR "system.zedbsd.quota"

struct ufs2_mount_state {
	struct ufs2_super super;
	struct mutex namespace_lock;
	struct mutex lock;
	struct mutex journal_lock;
	uint8_t *cg;
	uint32_t cg_iusedoff;
	uint32_t cg_freeoff;
	uint32_t cg_nextfreeoff;
	uint32_t active_cg;
	uint32_t rotor_cg;
	struct ufs_journal journal;
	struct ufs_snapshot snapshot;
	struct ufs_snapshot_entry *snapshot_map;
	struct disk *snapshot_disk;
	struct mutex snapshot_lock;
	struct quota_state quota;
	int journal_enabled;
	int snapshot_available;
	int writable;
};

static int journal_read(void *context,uint64_t lba,uint32_t count,void *buffer)
{ return disk_read(context,lba,count,buffer); }
static int journal_write(void *context,uint64_t lba,uint32_t count,const void *buffer)
{ return disk_write(context,lba,count,buffer); }
static int journal_flush(void *context) { return disk_sync(context); }

static uint32_t locator_get32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t locator_get64(const uint8_t *p)
{ return locator_get32(p)|(uint64_t)locator_get32(p+4)<<32; }
static uint32_t locator_digest(const uint8_t *p,size_t length)
{ uint32_t value=2166136261U;size_t n;for(n=0;n<length;n++){value^=p[n];value*=16777619U;}return value; }

static int
journal_discover(struct mount *mountp,struct ufs2_mount_state *ms)
{
	struct ufs_journal_io io;
	uint8_t locator[UFS2_SECTOR_SIZE];
	uint64_t end=ms->super.size<<ms->super.fsbtodb;
	uint32_t sectors;
	int error;
	if(end>=mountp->m_disk->d_block_count)return 0;
	error=disk_read(mountp->m_disk,end,1,locator);if(error!=0)return error;
	if(memcmp(locator,"ZUJ1",4)!=0)return 0;
	sectors=locator_get32(locator+8);
	if(locator_get32(locator+4)!=1U||locator_get64(locator+12)!=end||
	    locator_get32(locator+24)!=locator_digest(locator,24)||sectors<18U||
	    (uint64_t)sectors+1U>mountp->m_disk->d_block_count-end)return EINVAL;
	io.context=mountp->m_disk;io.read=journal_read;io.write=journal_write;
	io.flush=journal_flush;
	error=ufs_journal_init(&ms->journal,&io,end+1U,sectors);
	if(error==0){ms->journal_enabled=1;error=ufs_journal_replay(&ms->journal);}
	return error;
}

static int
snapshot_discover(struct mount *mountp,struct ufs2_mount_state *ms)
{
	struct ufs_journal_io io;uint8_t locator[UFS2_SECTOR_SIZE];
	uint64_t end=ms->super.size<<ms->super.fsbtodb,cursor=end;
	uint32_t sectors,max_records;size_t map_count;int error;
	if(end>=mountp->m_disk->d_block_count)return 0;
	error=disk_read(mountp->m_disk,end,1,locator);if(error!=0)return error;
	if(memcmp(locator,"ZUJ1",4)==0) {
		sectors=locator_get32(locator+8);
		if(locator_get32(locator+4)!=1U||locator_get64(locator+12)!=end||
		    sectors>mountp->m_disk->d_block_count-end-1U)return EINVAL;
		cursor=end+1U+sectors;
	}
	if(cursor>=mountp->m_disk->d_block_count)return 0;
	error=disk_read(mountp->m_disk,cursor,1,locator);if(error!=0)return error;
	if(memcmp(locator,"ZSL1",4)!=0)return 0;
	sectors=locator_get32(locator+8);
	if(locator_get32(locator+4)!=1U||locator_get64(locator+16)!=cursor||
	    locator_get64(locator+24)!=end||
	    locator_get32(locator+32)!=locator_digest(locator,32)||sectors<3U||
	    (uint64_t)sectors+1U>mountp->m_disk->d_block_count-cursor)return EINVAL;
	max_records=(sectors-1U)/2U;
	#if SIZE_MAX == UINT32_MAX
	if(max_records>SIZE_MAX/(2U*sizeof(*ms->snapshot_map)))return EOVERFLOW;
	#endif
	map_count=(size_t)max_records*2U;
	ms->snapshot_map=kern_calloc(map_count,sizeof(*ms->snapshot_map));
	if(ms->snapshot_map==NULL)return ENOMEM;
	io.context=mountp->m_disk;io.read=journal_read;io.write=journal_write;
	io.flush=journal_flush;
	error=ufs_snapshot_init(&ms->snapshot,&io,end,cursor+1U,sectors,
	    ms->snapshot_map,map_count);
	if(error==0)error=ufs_snapshot_open(&ms->snapshot);
	if(error!=0){kern_free(ms->snapshot_map);ms->snapshot_map=NULL;return error;}
	ms->snapshot_available=1;return 0;
}

static int
write_sectors(struct mount *mountp,uint64_t lba,uint32_t count,
	const void *buffer)
{
	struct ufs2_mount_state *ms=mountp!=NULL?mountp->m_data:NULL;
	int snapshot_locked=0;
	if(ms!=NULL&&ms->snapshot_available) {
		int error;
		mutex_lock(&ms->snapshot_lock);snapshot_locked=1;
		error=ufs_snapshot_preserve(&ms->snapshot,lba,count);
		if(error!=0){mutex_unlock(&ms->snapshot_lock);return error;}
	}
	if(ms!=NULL&&ms->journal_enabled) {
		int error;
		mutex_lock(&ms->journal_lock);
		error=ufs_journal_commit(&ms->journal,lba,buffer,count);
		if(error!=0&&ms->journal.poisoned)
			ms->writable=0;
		mutex_unlock(&ms->journal_lock);
		if(snapshot_locked)mutex_unlock(&ms->snapshot_lock);
		return error;
	}
	{
		int error=disk_write(mountp->m_disk,lba,count,buffer);
		if(snapshot_locked)mutex_unlock(&ms->snapshot_lock);
		return error;
	}
}

struct ufs2_inode_info {
	struct inode inode;
	uint64_t extattr[UFS2_NXADDR];
	uint32_t extattr_size;
	uint64_t direct[UFS2_NDADDR];
	uint64_t indirect[UFS2_NIADDR];
	uint32_t disk_flags;
	uint64_t blocks;
	uint32_t generation;
	uint8_t shortlink[120];
};

static const struct inode_ops ufs2_inode_ops;
static const struct file_ops ufs2_regular_ops;
static const struct file_ops ufs2_directory_ops;
static int ufs2_lookup(struct inode *,const struct componentname *,
	struct inode **);
static int persist_inode(struct inode *);
static ssize_t ufs2_getxattr(struct inode *,const char *,void *,size_t);
static int ufs2_setxattr(struct inode *,const char *,const void *,size_t,
	unsigned);

static struct ufs2_mount_state *state(const struct mount *mountp)
{ return mountp != NULL ? mountp->m_data : NULL; }
static struct ufs2_inode_info *info(const struct inode *inode)
{ return (struct ufs2_inode_info *)(uintptr_t)inode; }

static int
read_block(struct mount *mountp, uint64_t fragment, void *buffer)
{
	const struct ufs2_super *s = &state(mountp)->super;
	if (fragment == 0) { memset(buffer, 0, s->bsize); return 0; }
	if (fragment >= s->size || s->frag > s->size - fragment)
		return EIO;
	return disk_read(mountp->m_disk, (uint64_t)fragment << s->fsbtodb,
	    s->bsize / UFS2_SECTOR_SIZE, buffer);
}

static int
write_block(struct mount *mountp, uint64_t fragment, const void *buffer)
{
	const struct ufs2_super *s = &state(mountp)->super;
	if (fragment == 0 || fragment >= s->size || s->frag > s->size-fragment)
		return EIO;
	return write_sectors(mountp,(uint64_t)fragment<<s->fsbtodb,
	    s->bsize/UFS2_SECTOR_SIZE,buffer);
}

static int bit_test(const uint8_t *map,uint32_t bit)
{ return (map[bit>>3]&(uint8_t)(1U<<(bit&7U)))!=0; }
static void bit_set(uint8_t *map,uint32_t bit)
{ map[bit>>3]|=(uint8_t)(1U<<(bit&7U)); }
static void bit_clear(uint8_t *map,uint32_t bit)
{ map[bit>>3]&=(uint8_t)~(1U<<(bit&7U)); }

static uint64_t
cgstart(const struct ufs2_super *super, uint32_t cg)
{
	return (uint64_t)cg * super->fpg +
	    (uint64_t)super->cgoffset * (cg & ~super->cgmask);
}

static uint32_t
cg_ndblk(const struct ufs2_super *super, uint32_t cg)
{
	uint64_t start = cgstart(super, cg);
	uint64_t remaining = start < super->size ? super->size - start : 0;
	return remaining > super->fpg ? super->fpg : (uint32_t)remaining;
}

static int
load_cg_locked(struct mount *mountp, uint32_t cg)
{
	struct ufs2_mount_state *ms = state(mountp);
	uint32_t inode_map_bytes, free_map_bytes, ndblk;
	uint32_t ndir, nbfree, nifree, nffree;
	uint64_t fragment;
	int error;

	if (cg >= ms->super.ncg)
		return EINVAL;
	fragment = cgstart(&ms->super, cg) + ms->super.cblkno;
	if (fragment >= ms->super.size ||
	    ms->super.frag > ms->super.size - fragment)
		return EINVAL;
	error = disk_read(mountp->m_disk, fragment << ms->super.fsbtodb,
	    ms->super.bsize / UFS2_SECTOR_SIZE, ms->cg);
	if (error != 0)
		return error;
	ndblk = cg_ndblk(&ms->super, cg);
	inode_map_bytes = (ms->super.ipg + 7U) / 8U;
	free_map_bytes = (ms->super.fpg + 7U) / 8U;
	ndir = ufs1_get32(ms->cg, UFS2_CG_NDIR, ms->super.swapped);
	nbfree = ufs1_get32(ms->cg, UFS2_CG_NBFREE, ms->super.swapped);
	nifree = ufs1_get32(ms->cg, UFS2_CG_NIFREE, ms->super.swapped);
	nffree = ufs1_get32(ms->cg, UFS2_CG_NFFREE, ms->super.swapped);
	ms->cg_iusedoff = ufs1_get32(ms->cg, UFS2_CG_IUSEDOFF,
	    ms->super.swapped);
	ms->cg_freeoff = ufs1_get32(ms->cg, UFS2_CG_FREEOFF,
	    ms->super.swapped);
	ms->cg_nextfreeoff = ufs1_get32(ms->cg, UFS2_CG_NEXTFREEOFF,
	    ms->super.swapped);
	if (ufs1_get32(ms->cg, UFS2_CG_MAGIC, ms->super.swapped) !=
	    UFS2_CG_MAGIC_VALUE ||
	    ufs1_get32(ms->cg, UFS2_CG_CGX, ms->super.swapped) != cg ||
	    ufs1_get32(ms->cg, UFS2_CG_NDBLK, ms->super.swapped) != ndblk ||
	    ms->cg_iusedoff >= ms->super.bsize ||
	    ms->cg_freeoff > ms->super.bsize ||
	    ms->cg_nextfreeoff > ms->super.cgsize ||
	    ms->cg_iusedoff > ms->cg_freeoff ||
	    inode_map_bytes > ms->cg_freeoff - ms->cg_iusedoff ||
	    ms->cg_freeoff > ms->cg_nextfreeoff ||
	    free_map_bytes > ms->cg_nextfreeoff - ms->cg_freeoff ||
	    ndir > ms->super.ipg || nifree > ms->super.ipg ||
	    nbfree > ndblk / ms->super.frag || nffree > ndblk ||
	    (uint64_t)nbfree * ms->super.frag + nffree > ndblk)
		return EINVAL;
	ms->active_cg = cg;
	return 0;
}

static int
valid_inode_fragment(const struct ufs2_super *super, uint64_t fragment)
{
	uint32_t cg;
	if (fragment == 0)
		return 1;
	for (cg = 0; cg < super->ncg; cg++) {
		uint64_t start = cgstart(super, cg);
		uint32_t ndblk = cg_ndblk(super, cg);
		if (fragment >= start + super->dblkno &&
		    fragment < start + ndblk &&
		    super->frag <= start + ndblk - fragment)
			return 1;
	}
	return 0;
}

static int
write_super_summaries(struct mount *mountp)
{
	struct ufs2_mount_state *ms=state(mountp);
	uint8_t *buffer=kern_malloc(UFS2_SBLOCK_SIZE);
	int error;
	if(buffer==NULL)return ENOMEM;
	error=disk_read(mountp->m_disk,UFS2_SBLOCK_OFFSET/UFS2_SECTOR_SIZE,
	    UFS2_SBLOCK_SIZE/UFS2_SECTOR_SIZE,buffer);
	if(error==0) {
		ufs1_put64(buffer,UFS2_FS_CSTOTAL_NDIR,
		    ms->super.cstotal_ndir,
		    ms->super.swapped);
		ufs1_put64(buffer,UFS2_FS_CSTOTAL_NBFREE,
		    ms->super.cstotal_nbfree,
		    ms->super.swapped);
		ufs1_put64(buffer,UFS2_FS_CSTOTAL_NIFREE,
		    ms->super.cstotal_nifree,
		    ms->super.swapped);
		ufs1_put64(buffer,UFS2_FS_CSTOTAL_NFFREE,
		    ms->super.cstotal_nffree,
		    ms->super.swapped);
		error=write_sectors(mountp,
		    UFS2_SBLOCK_OFFSET/UFS2_SECTOR_SIZE,
		    UFS2_SBLOCK_SIZE/UFS2_SECTOR_SIZE,buffer);
	}
	kern_free(buffer);
	return error;
}

static int
write_cg(struct mount *mountp)
{
	struct ufs2_mount_state *ms=state(mountp);
	struct kern_test_fault_result fault;
	if(KERN_TEST_FAULT(KERN_TEST_FAULT_UFS_CG_WRITE,UINT32_MAX,
	    UINT32_MAX,&fault))return fault.error!=0?fault.error:EIO;
	int error=write_sectors(mountp,
	    (cgstart(&ms->super,ms->active_cg)+ms->super.cblkno)<<
	    ms->super.fsbtodb,
	    ms->super.bsize/UFS2_SECTOR_SIZE,ms->cg);
	return error!=0?error:write_super_summaries(mountp);
}

/* Caller holds ms->lock and has already restored the in-memory CG image. */
static int
write_cg_rollback(struct mount *mountp, int original_error)
{
	struct ufs2_mount_state *ms = state(mountp);
	if (write_cg(mountp) != 0)
		ms->writable = 0;
	return original_error;
}

static int
adjust_directory_count(struct mount *mountp,uint32_t ino,int delta)
{
	struct ufs2_mount_state *ms=state(mountp);
	uint32_t count,cg=ino/ms->super.ipg;
	uint64_t old_total;
	int error;
	mutex_lock(&ms->lock);
	error=load_cg_locked(mountp,cg);
	if(error!=0){mutex_unlock(&ms->lock);return error;}
	count=ufs1_get32(ms->cg,UFS2_CG_NDIR,ms->super.swapped);
	old_total=ms->super.cstotal_ndir;
	if((delta<0&&count==0)||(delta>0&&count==UINT32_MAX))
		error=EIO;
	else {
		ufs1_put32(ms->cg,UFS2_CG_NDIR,
		    delta<0?count-1U:count+1U,ms->super.swapped);
		ms->super.cstotal_ndir=delta<0?old_total-1U:old_total+1U;
		error=write_cg(mountp);
		if(error!=0) {
			ufs1_put32(ms->cg,UFS2_CG_NDIR,count,
			    ms->super.swapped);
			ms->super.cstotal_ndir=old_total;
			error=write_cg_rollback(mountp,error);
		}
	}
	mutex_unlock(&ms->lock);
	return error;
}

static uint64_t
quota_now(void)
{
	time_t seconds=0;long nanoseconds=0;
	clock_realtime(&seconds,&nanoseconds);(void)nanoseconds;
	return seconds>0?(uint64_t)seconds:0;
}

static int
allocate_block(struct mount *mountp,uid_t uid,gid_t gid,uint64_t *result)
{
	struct ufs2_mount_state *ms=state(mountp); uint8_t *map;
	struct quota_charge charge;
	uint32_t fragment,n,cg,attempt; uint64_t old_total; int error;
	error=quota_reserve(&ms->quota,uid,gid,1,0,quota_now(),&charge);
	if(error!=0)return error;
	error=ENOSPC;
	mutex_lock(&ms->lock);
	for(attempt=0;attempt<ms->super.ncg;attempt++){
		uint32_t ndblk;
		cg=(ms->rotor_cg+attempt)%ms->super.ncg;
		error=load_cg_locked(mountp,cg);if(error!=0)break;
		error=ENOSPC;
		map=ms->cg+ms->cg_freeoff;ndblk=cg_ndblk(&ms->super,cg);
	for(fragment=(ms->super.dblkno+ms->super.frag-1U)&~(ms->super.frag-1U);
	    fragment+ms->super.frag<=ndblk;fragment+=ms->super.frag){
		for(n=0;n<ms->super.frag&&bit_test(map,fragment+n);n++)
			;
		if(n!=ms->super.frag)
			continue;
		for(n=0;n<ms->super.frag;n++)bit_clear(map,fragment+n);
		{ old_total=ms->super.cstotal_nbfree;
			uint32_t free=ufs1_get32(ms->cg,UFS2_CG_NBFREE,ms->super.swapped);
			if(free==0){for(n=0;n<ms->super.frag;n++)bit_set(map,fragment+n);break;}
			ufs1_put32(ms->cg,UFS2_CG_NBFREE,free-1U,ms->super.swapped);
			ms->super.cstotal_nbfree=old_total-1U;
		}
		error=write_cg(mountp);
		if(error==0){uint8_t *zero=kern_calloc(1,ms->super.bsize);uint64_t absolute=cgstart(&ms->super,cg)+fragment;if(zero==NULL)error=ENOMEM;else{error=write_block(mountp,absolute,zero);kern_free(zero);}}
		if(error!=0){for(n=0;n<ms->super.frag;n++)bit_set(map,fragment+n);ufs1_put32(ms->cg,UFS2_CG_NBFREE,ufs1_get32(ms->cg,UFS2_CG_NBFREE,ms->super.swapped)+1U,ms->super.swapped);ms->super.cstotal_nbfree=old_total;error=write_cg_rollback(mountp,error);}else {*result=cgstart(&ms->super,cg)+fragment;ms->rotor_cg=cg;}
		break;
	}
		if(error!=ENOSPC)break;
	}
	mutex_unlock(&ms->lock);
	if(error==0)quota_commit(&charge);else quota_rollback(&charge);
	return error;
}

static int
free_block(struct mount *mountp,uint64_t fragment,uid_t uid,gid_t gid)
{
	struct ufs2_mount_state *ms=state(mountp);uint8_t *map;uint32_t n,free,cg,local=0;uint64_t old_total;int error;
	for(cg=0;cg<ms->super.ncg;cg++){uint64_t start=cgstart(&ms->super,cg);uint32_t ndblk=cg_ndblk(&ms->super,cg);if(fragment>=start+ms->super.dblkno&&fragment+ms->super.frag<=start+ndblk){local=(uint32_t)(fragment-start);break;}}
	if(cg==ms->super.ncg)return EIO;
	mutex_lock(&ms->lock);error=load_cg_locked(mountp,cg);if(error!=0){mutex_unlock(&ms->lock);return error;}map=ms->cg+ms->cg_freeoff;
	for(n=0;n<ms->super.frag;n++)if(bit_test(map,local+n)){mutex_unlock(&ms->lock);return EIO;}
	free=ufs1_get32(ms->cg,UFS2_CG_NBFREE,ms->super.swapped);
	if(free==UINT32_MAX){mutex_unlock(&ms->lock);return EIO;}
	old_total=ms->super.cstotal_nbfree;
	for(n=0;n<ms->super.frag;n++)bit_set(map,local+n);
	ufs1_put32(ms->cg,UFS2_CG_NBFREE,free+1U,ms->super.swapped);
	ms->super.cstotal_nbfree=old_total+1U;
	error=write_cg(mountp);
	if(error!=0){for(n=0;n<ms->super.frag;n++)bit_clear(map,local+n);ufs1_put32(ms->cg,UFS2_CG_NBFREE,free,ms->super.swapped);ms->super.cstotal_nbfree=old_total;error=write_cg_rollback(mountp,error);}
	mutex_unlock(&ms->lock);
	if(error==0&&quota_release(&ms->quota,uid,gid,1,0)!=0){ms->writable=0;return EIO;}
	return error;
}

static int
allocate_inode_number(struct mount *mountp,uid_t uid,gid_t gid,uint32_t *number)
{
	struct ufs2_mount_state *ms=state(mountp);struct quota_charge charge;
	uint8_t *map;uint32_t ino,cg,attempt;uint64_t old_total;int error;
	error=quota_reserve(&ms->quota,uid,gid,0,1,quota_now(),&charge);
	if(error!=0)return error;
	error=ENOSPC;mutex_lock(&ms->lock);
	for(attempt=0;attempt<ms->super.ncg;attempt++){cg=(ms->rotor_cg+attempt)%ms->super.ncg;error=load_cg_locked(mountp,cg);if(error!=0)break;error=ENOSPC;map=ms->cg+ms->cg_iusedoff;
	for(ino=cg==0?UFS2_ROOT_INO+1U:0U;ino<ms->super.ipg;ino++)if(!bit_test(map,ino)){
		uint32_t free=ufs1_get32(ms->cg,UFS2_CG_NIFREE,ms->super.swapped);
		if(free==0)break;
		old_total=ms->super.cstotal_nifree;bit_set(map,ino);
		ufs1_put32(ms->cg,UFS2_CG_NIFREE,free-1U,ms->super.swapped);
		ms->super.cstotal_nifree=old_total-1U;
		error=write_cg(mountp);if(error!=0){bit_clear(map,ino);ufs1_put32(ms->cg,UFS2_CG_NIFREE,free,ms->super.swapped);ms->super.cstotal_nifree=old_total;error=write_cg_rollback(mountp,error);}else {*number=cg*ms->super.ipg+ino;ms->rotor_cg=cg;}break;
	}
		if(error!=ENOSPC)break;
	}
	mutex_unlock(&ms->lock);
	if(error==0)quota_commit(&charge);else quota_rollback(&charge);
	return error;
}

static int
free_inode_number(struct mount *mountp,uint32_t number,uid_t uid,gid_t gid)
{
	struct ufs2_mount_state *ms=state(mountp);uint8_t *map;uint32_t free,cg=number/ms->super.ipg,local=number%ms->super.ipg;uint64_t old_total;int error;
	if(number<=UFS2_ROOT_INO||cg>=ms->super.ncg)return EIO;
	mutex_lock(&ms->lock);error=load_cg_locked(mountp,cg);if(error!=0){mutex_unlock(&ms->lock);return error;}map=ms->cg+ms->cg_iusedoff;
	if(!bit_test(map,local)){mutex_unlock(&ms->lock);return EIO;}
	free=ufs1_get32(ms->cg,UFS2_CG_NIFREE,ms->super.swapped);
	if(free==UINT32_MAX){mutex_unlock(&ms->lock);return EIO;}
	old_total=ms->super.cstotal_nifree;bit_clear(map,local);
	ufs1_put32(ms->cg,UFS2_CG_NIFREE,free+1U,ms->super.swapped);
	ms->super.cstotal_nifree=old_total+1U;error=write_cg(mountp);
	if(error!=0){bit_set(map,local);ufs1_put32(ms->cg,UFS2_CG_NIFREE,free,ms->super.swapped);ms->super.cstotal_nifree=old_total;error=write_cg_rollback(mountp,error);}
	mutex_unlock(&ms->lock);
	if(error==0&&quota_release(&ms->quota,uid,gid,0,1)!=0){ms->writable=0;return EIO;}
	return error;
}

static int
indirect_entry(struct mount *mountp, uint64_t fragment, uint32_t index,
	uint64_t *result)
{
	const struct ufs2_super *s = &state(mountp)->super;
	uint8_t *block;
	int error;
	if (fragment == 0) { *result = 0; return 0; }
	if (index >= s->nindir)
		return EIO;
	block = kern_malloc(s->bsize);
	if (block == NULL)
		return ENOMEM;
	error = read_block(mountp, fragment, block);
	if (error == 0)
		*result = ufs1_get64(block, (size_t)index * 8U, s->swapped);
	kern_free(block);
	return error;
}

static int
bmap(struct inode *inode, uint64_t logical, uint64_t *result)
{
	struct ufs2_inode_info *ui = info(inode);
	const struct ufs2_super *s = &state(inode->i_mount)->super;
	uint64_t span = s->nindir;
	uint64_t fragment;
	unsigned level, depth;
	int error;
	if (logical < UFS2_NDADDR) { *result = ui->direct[logical]; return 0; }
	logical -= UFS2_NDADDR;
	for (level = 0; level < UFS2_NIADDR; level++) {
		if (logical < span)
			break;
		logical -= span;
		if (span > UINT64_MAX / s->nindir)
			return EOVERFLOW;
		span *= s->nindir;
	}
	if (level == UFS2_NIADDR)
		return EFBIG;
	fragment = ui->indirect[level];
	for (depth = level + 1U; depth != 0; depth--) {
		uint64_t divisor = 1;
		uint32_t index;
		unsigned n;
		for (n = 1; n < depth; n++)
			divisor *= s->nindir;
		index = (uint32_t)(logical / divisor);
		logical %= divisor;
		error = indirect_entry(inode->i_mount, fragment, index, &fragment);
		if (error != 0 || fragment == 0)
			break;
	}
	*result = fragment;
	return error;
}

static int
bmap_ensure(struct inode *inode,uint64_t logical,uint64_t *result)
{
	struct ufs2_inode_info *ui=info(inode);
	const struct ufs2_super *s=&state(inode->i_mount)->super;
	uint64_t span=s->nindir;
	uint64_t *root,fragment;
	unsigned level,depth;
	int error;

	if(logical<UFS2_NDADDR) {
		if(ui->direct[logical]==0) {
			uint64_t allocated;
			error=allocate_block(inode->i_mount,inode->i_uid,inode->i_gid,&allocated);
			if(error!=0)
				return error;
			ui->direct[logical]=allocated;
			ui->blocks+=s->bsize/UFS2_SECTOR_SIZE;
			/* Make the allocation reachable before user data I/O. */
			error=persist_inode(inode);
			if(error!=0) {
				int free_error;
				ui->direct[logical]=0;
				ui->blocks-=s->bsize/UFS2_SECTOR_SIZE;
				free_error=free_block(inode->i_mount,allocated,inode->i_uid,inode->i_gid);
				if(free_error!=0) {
					ui->direct[logical]=allocated;
					ui->blocks+=s->bsize/UFS2_SECTOR_SIZE;
					(void)persist_inode(inode);
				}
				return error;
			}
		}
		*result=ui->direct[logical];
		return 0;
	}
	logical-=UFS2_NDADDR;
	for(level=0;level<UFS2_NIADDR;level++) {
		if(logical<span)
			break;
		logical-=span;
		if(span>UINT64_MAX/s->nindir)
			return EOVERFLOW;
		span*=s->nindir;
	}
	if(level==UFS2_NIADDR)
		return EFBIG;
	root=&ui->indirect[level];
	if(*root==0) {
		uint64_t allocated;
		error=allocate_block(inode->i_mount,inode->i_uid,inode->i_gid,&allocated);
		if(error!=0)
			return error;
		*root=allocated;
		ui->blocks+=s->bsize/UFS2_SECTOR_SIZE;
		error=persist_inode(inode);
		if(error!=0) {
			int free_error;
			*root=0;
			ui->blocks-=s->bsize/UFS2_SECTOR_SIZE;
			free_error=free_block(inode->i_mount,allocated,inode->i_uid,inode->i_gid);
			if(free_error!=0) {
				*root=allocated;
				ui->blocks+=s->bsize/UFS2_SECTOR_SIZE;
				(void)persist_inode(inode);
			}
			return error;
		}
	}
	fragment=*root;
	for(depth=level+1U;depth!=0;depth--) {
		uint8_t *block;
		uint64_t divisor=1;
		uint32_t index;
		uint64_t next;
		unsigned n;

		for(n=1;n<depth;n++)
			divisor*=s->nindir;
		index=(uint32_t)(logical/divisor);
		logical%=divisor;
		block=kern_malloc(s->bsize);
		if(block==NULL)
			return ENOMEM;
		error=read_block(inode->i_mount,fragment,block);
		if(error!=0) {
			kern_free(block);
			return error;
		}
		next=ufs1_get64(block,(size_t)index*8U,s->swapped);
		if(next==0) {
			uint64_t allocated=0;
			error=allocate_block(inode->i_mount,inode->i_uid,inode->i_gid,&next);
			if(error==0) {
				allocated=next;
				ufs1_put64(block,(size_t)index*8U,next,s->swapped);
				error=write_block(inode->i_mount,fragment,block);
			}
			if(error!=0) {
				if(allocated!=0) {
					int rollback_error;
					/* A short write may have published the pointer even
					 * though write_block() reported EIO.  Make it
					 * unreachable before returning its block. */
					ufs1_put64(block,(size_t)index*8U,0,s->swapped);
					rollback_error=write_block(inode->i_mount,
					    fragment,block);
					if(rollback_error==0)
						rollback_error=free_block(inode->i_mount,
						    allocated,inode->i_uid,inode->i_gid);
					if(rollback_error!=0) {
						/* The block may remain reachable.  Never free
						 * uncertain storage or continue writable. */
						ui->blocks+=s->bsize/UFS2_SECTOR_SIZE;
						state(inode->i_mount)->writable=0;
					}
				}
				kern_free(block);
				return error;
			}
			ui->blocks+=s->bsize/UFS2_SECTOR_SIZE;
		}
		kern_free(block);
		fragment=next;
	}
	*result=fragment;
	return 0;
}

static ssize_t
pread_inode(struct inode *inode, void *buffer, size_t length, off_t offset)
{
	const struct ufs2_super *s = &state(inode->i_mount)->super;
	uint8_t *scratch;
	size_t done = 0;
	if (offset < 0)
		return -EINVAL;
	if (offset >= inode->i_size || length == 0)
		return 0;
	if ((uint64_t)length > (uint64_t)inode->i_size - (uint64_t)offset)
		length = (size_t)((uint64_t)inode->i_size - (uint64_t)offset);
	scratch = kern_malloc(s->bsize);
	if (scratch == NULL)
		return -ENOMEM;
	while (done < length) {
		uint64_t position = (uint64_t)offset + done;
		uint64_t logical = position / s->bsize;
		size_t within = (size_t)(position % s->bsize);
		size_t amount = s->bsize - within;
		uint64_t fragment;
		int error;
		if (amount > length - done) amount = length - done;
		error = bmap(inode, logical, &fragment);
		if (error == 0)
			error = read_block(inode->i_mount, fragment, scratch);
		if (error != 0) { kern_free(scratch); return done ? (ssize_t)done : -error; }
		memcpy((uint8_t *)buffer + done, scratch + within, amount);
		done += amount;
	}
	kern_free(scratch);
	return (ssize_t)done;
}

static int
persist_inode(struct inode *inode)
{
	struct ufs2_inode_info *ui=info(inode);struct ufs2_mount_state *ms=state(inode->i_mount);
	uint32_t number=(uint32_t)inode->i_ino,cg=number/ms->super.ipg,index=number%ms->super.ipg;
	uint64_t fragment=cgstart(&ms->super,cg)+ms->super.iblkno+
	    (index/ms->super.inopb)*ms->super.frag;
	uint8_t *block=kern_malloc(ms->super.bsize),*raw;unsigned n;int error;
	if(block==NULL)
		return ENOMEM;
	error=read_block(inode->i_mount,fragment,block);
	if(error!=0){kern_free(block);return error;}raw=block+(index%ms->super.inopb)*UFS2_DINODE_SIZE;
	ufs1_put16(raw,UFS2_DI_MODE,(uint16_t)inode->i_mode,ms->super.swapped);
	ufs1_put16(raw,UFS2_DI_NLINK,(uint16_t)inode->i_linkcount,ms->super.swapped);
	ufs1_put64(raw,UFS2_DI_SIZE,(uint64_t)inode->i_size,ms->super.swapped);
	ufs1_put64(raw,UFS2_DI_ATIME,(uint64_t)inode->i_atime.tv_sec,
		ms->super.swapped);
	ufs1_put32(raw,UFS2_DI_ATIMENSEC,(uint32_t)inode->i_atime.tv_nsec,
		ms->super.swapped);
	ufs1_put64(raw,UFS2_DI_MTIME,(uint64_t)inode->i_mtime.tv_sec,
		ms->super.swapped);
	ufs1_put32(raw,UFS2_DI_MTIMENSEC,(uint32_t)inode->i_mtime.tv_nsec,
		ms->super.swapped);
	ufs1_put64(raw,UFS2_DI_CTIME,(uint64_t)inode->i_ctime.tv_sec,
		ms->super.swapped);
	ufs1_put32(raw,UFS2_DI_CTIMENSEC,(uint32_t)inode->i_ctime.tv_nsec,
		ms->super.swapped);
	ufs1_put32(raw,UFS2_DI_EXTSIZE,ui->extattr_size,ms->super.swapped);
	for(n=0;n<UFS2_NXADDR;n++)
		ufs1_put64(raw,UFS2_DI_EXTB+n*8U,ui->extattr[n],
		    ms->super.swapped);
	if(inode->i_type==INODE_SYMLINK&&(uint64_t)inode->i_size<=ms->super.maxsymlinklen&&inode->i_size<=120) {
		memset(raw+UFS2_DI_DB,0,120U);
		memcpy(raw+UFS2_DI_DB,ui->shortlink,(size_t)inode->i_size);
	} else {
		for(n=0;n<UFS2_NDADDR;n++)ufs1_put64(raw,UFS2_DI_DB+n*8U,ui->direct[n],ms->super.swapped);
		for(n=0;n<UFS2_NIADDR;n++)ufs1_put64(raw,UFS2_DI_IB+n*8U,ui->indirect[n],ms->super.swapped);
	}
	ufs1_put64(raw,UFS2_DI_BLOCKS,ui->blocks,ms->super.swapped);
	ufs1_put32(raw,UFS2_DI_UID,inode->i_uid,ms->super.swapped);ufs1_put32(raw,UFS2_DI_GID,inode->i_gid,ms->super.swapped);
	error=write_block(inode->i_mount,fragment,block);kern_free(block);return error;
}

static ssize_t
pwrite_inode(struct inode *inode,const void *buffer,size_t length,off_t offset)
{
	struct ufs2_mount_state *ms=state(inode->i_mount);
	uint8_t *scratch;size_t done=0;int final_error=0;
	if(!ms->writable)
		return -EROFS;
	if(offset<0||(uint64_t)offset+length<(uint64_t)offset)
		return -EINVAL;
	if((uint64_t)offset+length>ms->super.maxfilesize)return -EFBIG;
	scratch=kern_malloc(ms->super.bsize);if(scratch==NULL)return -ENOMEM;mutex_lock(&inode->i_lock);
	while(done<length){uint64_t pos=(uint64_t)offset+done,lbn=pos/ms->super.bsize;size_t within=(size_t)(pos%ms->super.bsize),amount=ms->super.bsize-within;uint64_t fragment=0;int error;
		if(amount>length-done)amount=length-done;
		error=bmap(inode,lbn,&fragment);
		if(error!=0){final_error=error;break;}
		if(fragment==0){error=bmap_ensure(inode,lbn,&fragment);if(error){final_error=error;break;}memset(scratch,0,ms->super.bsize);}else{error=read_block(inode->i_mount,fragment,scratch);if(error){final_error=error;break;}}
		memcpy(scratch+within,(const uint8_t *)buffer+done,amount);error=write_block(inode->i_mount,fragment,scratch);if(error){final_error=error;break;}done+=amount;
		if((uint64_t)inode->i_size<(uint64_t)offset+done)inode->i_size=(off_t)((uint64_t)offset+done);
	}
	if(done!=0){int error=persist_inode(inode);if(error!=0&&final_error==0)final_error=error;}
	mutex_unlock(&inode->i_lock);kern_free(scratch);return done?(ssize_t)done:-final_error;
}

static uint64_t
indirect_span(const struct ufs2_super *super,unsigned depth)
{
	uint64_t span=1;
	while(depth--!=0)
		span*=super->nindir;
	return span;
}

static int
truncate_indirect(struct inode *inode,uint64_t *root,unsigned depth,
	uint64_t base,uint64_t keep)
{
	struct ufs2_mount_state *ms=state(inode->i_mount);
	struct ufs2_inode_info *ui=info(inode);
	uint8_t *block;
	uint64_t child_span=indirect_span(&ms->super,depth-1U);
	unsigned index;
	int changed=0,empty=1,error;

	if(*root==0)
		return 0;
	block=kern_malloc(ms->super.bsize);
	if(block==NULL)
		return ENOMEM;
	error=read_block(inode->i_mount,*root,block);
	if(error!=0) {
		kern_free(block);
		return error;
	}
	for(index=0;index<ms->super.nindir;index++) {
		uint64_t child=ufs1_get64(block,(size_t)index*8U,
			ms->super.swapped);
		uint64_t child_base=base+(uint64_t)index*child_span;
		if(child==0)
			continue;
		if(depth==1U) {
			if(child_base>=keep) {
				error=free_block(inode->i_mount,child,inode->i_uid,inode->i_gid);
				if(error!=0) {
					kern_free(block);
					return error;
				}
				ufs1_put64(block,(size_t)index*8U,0,
					ms->super.swapped);
				ui->blocks-=ms->super.bsize/UFS2_SECTOR_SIZE;
				changed=1;
				continue;
			}
		} else if(child_base+child_span>keep) {
			error=truncate_indirect(inode,&child,depth-1U,child_base,keep);
			if(error!=0) {
				kern_free(block);
				return error;
			}
			if(child!=ufs1_get64(block,(size_t)index*8U,
			    ms->super.swapped)) {
				ufs1_put64(block,(size_t)index*8U,child,
					ms->super.swapped);
				changed=1;
			}
		}
		if(ufs1_get64(block,(size_t)index*8U,ms->super.swapped)!=0)
			empty=0;
	}
	if(empty) {
		error=free_block(inode->i_mount,*root,inode->i_uid,inode->i_gid);
		if(error==0) {
			*root=0;
			ui->blocks-=ms->super.bsize/UFS2_SECTOR_SIZE;
		}
	} else if(changed) {
		error=write_block(inode->i_mount,*root,block);
	} else {
		error=0;
	}
	kern_free(block);
	return error;
}

static int
ufs2_truncate(struct inode *inode,off_t size)
{
	struct ufs2_mount_state *ms=state(inode->i_mount);struct ufs2_inode_info *ui=info(inode);uint8_t *block=NULL;uint64_t keep,base;unsigned n;int error=0;
	if(!ms->writable)
		return EROFS;
	if(size<0||(uint64_t)size>ms->super.maxfilesize)
		return EFBIG;
	mutex_lock(&inode->i_lock);keep=((uint64_t)size+ms->super.bsize-1U)/ms->super.bsize;
	if(size<inode->i_size&&size!=0&&(size%ms->super.bsize)!=0){uint64_t fragment=0;error=bmap(inode,(uint64_t)size/ms->super.bsize,&fragment);if(error)goto out;if(fragment!=0){block=kern_malloc(ms->super.bsize);if(block==NULL){error=ENOMEM;goto out;}error=read_block(inode->i_mount,fragment,block);if(error)goto out;memset(block+(size%ms->super.bsize),0,ms->super.bsize-(size%ms->super.bsize));error=write_block(inode->i_mount,fragment,block);if(error)goto out;}}
	for(n=(unsigned)keep;n<UFS2_NDADDR;n++)if(ui->direct[n]!=0){error=free_block(inode->i_mount,ui->direct[n],inode->i_uid,inode->i_gid);if(error)goto out;ui->direct[n]=0;ui->blocks-=ms->super.bsize/UFS2_SECTOR_SIZE;}
	base=UFS2_NDADDR;
	for(n=0;n<UFS2_NIADDR;n++) {
		error=truncate_indirect(inode,&ui->indirect[n],n+1U,base,keep);
		if(error!=0)
			goto out;
		base+=indirect_span(&ms->super,n+1U);
	}
	inode->i_size=size;error=persist_inode(inode);
out:	kern_free(block);mutex_unlock(&inode->i_lock);return error;
}

static enum inode_type
mode_type(uint16_t mode)
{
	switch (mode & UFS2_IFMT) {
	case UFS2_IFREG: return INODE_REG; case UFS2_IFDIR: return INODE_DIR;
	case UFS2_IFLNK: return INODE_SYMLINK; case UFS2_IFCHR: return INODE_CHAR;
	case UFS2_IFBLK: return INODE_BLOCK; case UFS2_IFIFO: return INODE_FIFO;
	case UFS2_IFSOCK: return INODE_SOCKET; default: return INODE_NONE;
	}
}

static int
load_inode(struct mount *mountp, uint32_t number, struct inode **result)
{
	const struct ufs2_super *s = &state(mountp)->super;
	struct ufs2_inode_info *ui;
	struct inode *inode;
	uint8_t *block, *raw;
	uint32_t cg, index;
	uint64_t fragment;
	uint16_t mode;
	unsigned n;
	int error;
	if (number < UFS2_ROOT_INO || number >= s->ncg * s->ipg)
		return EIO;
	if (inode_get(mountp, number, result) == 0)
		return 0;
	cg = number / s->ipg; index = number % s->ipg;
	fragment = cgstart(s, cg) + s->iblkno +
	    (index / s->inopb) * s->frag;
	block = kern_malloc(s->bsize);
	if (block == NULL)
		return ENOMEM;
	error = read_block(mountp, fragment, block);
	if (error != 0) { kern_free(block); return error; }
	raw = block + (index % s->inopb) * UFS2_DINODE_SIZE;
	mode = ufs1_get16(raw, UFS2_DI_MODE, s->swapped);
	if (mode_type(mode) == INODE_NONE) { kern_free(block); return EOPNOTSUPP; }
	inode = inode_alloc(mountp);
	if (inode == NULL) { kern_free(block); return ENOSPC; }
	ui = info(inode);
	inode->i_type = mode_type(mode); inode->i_ino = number;
	inode->i_mode = mode; inode->i_linkcount = ufs1_get16(raw,UFS2_DI_NLINK,s->swapped);
	inode->i_size = (off_t)ufs1_get64(raw,UFS2_DI_SIZE,s->swapped);
	inode->i_uid = ufs1_get32(raw,UFS2_DI_UID,s->swapped);
	inode->i_gid = ufs1_get32(raw,UFS2_DI_GID,s->swapped);
	inode->i_atime.tv_sec = (time_t)ufs1_get64(raw,UFS2_DI_ATIME,s->swapped);
	inode->i_atime.tv_nsec = ufs1_get32(raw,UFS2_DI_ATIMENSEC,s->swapped);
	inode->i_mtime.tv_sec = (time_t)ufs1_get64(raw,UFS2_DI_MTIME,s->swapped);
	inode->i_mtime.tv_nsec = ufs1_get32(raw,UFS2_DI_MTIMENSEC,s->swapped);
	inode->i_ctime.tv_sec = (time_t)ufs1_get64(raw,UFS2_DI_CTIME,s->swapped);
	inode->i_ctime.tv_nsec = ufs1_get32(raw,UFS2_DI_CTIMENSEC,s->swapped);
	ui->extattr_size=ufs1_get32(raw,UFS2_DI_EXTSIZE,s->swapped);
	for(n=0;n<UFS2_NXADDR;n++)
		ui->extattr[n]=ufs1_get64(raw,UFS2_DI_EXTB+n*8U,s->swapped);
	if(inode->i_type==INODE_SYMLINK&&(uint64_t)inode->i_size<=
	    s->maxsymlinklen&&inode->i_size<=120) {
		memcpy(ui->shortlink,raw+UFS2_DI_DB,sizeof(ui->shortlink));
	} else {
		for (n=0;n<UFS2_NDADDR;n++)
			ui->direct[n]=ufs1_get64(raw,UFS2_DI_DB+n*8U,s->swapped);
		for (n=0;n<UFS2_NIADDR;n++)
			ui->indirect[n]=ufs1_get64(raw,UFS2_DI_IB+n*8U,s->swapped);
	}
	ui->disk_flags=ufs1_get32(raw,UFS2_DI_FLAGS,s->swapped);
	ui->blocks=ufs1_get64(raw,UFS2_DI_BLOCKS,s->swapped);
	ui->generation=ufs1_get32(raw,UFS2_DI_GEN,s->swapped);
	if (inode->i_linkcount == 0 || inode->i_size < 0 ||
	    (uint64_t)inode->i_size > s->maxfilesize ||
	    ui->extattr_size > UFS2_NXADDR*s->bsize ||
	    inode->i_atime.tv_nsec >= 1000000000L ||
	    inode->i_mtime.tv_nsec >= 1000000000L ||
	    inode->i_ctime.tv_nsec >= 1000000000L ||
	    (inode->i_type == INODE_DIR &&
	    ((uint64_t)inode->i_size < UFS2_DIRBLKSIZ ||
	    (uint64_t)inode->i_size % UFS2_DIRBLKSIZ != 0))) {
		inode->i_flags |= INODE_DEAD;
		inode_release(inode);
		kern_free(block);
		return EIO;
	}
	for (n=0;n<UFS2_NXADDR;n++) {
		int needed=ui->extattr_size>n*s->bsize;
		if ((needed&&ui->extattr[n]==0)||
		    (!needed&&ui->extattr[n]!=0)||
		    (needed&&!valid_inode_fragment(s,ui->extattr[n]))) {
			inode->i_flags|=INODE_DEAD;
			inode_release(inode);kern_free(block);return EIO;
		}
	}
	if (!(inode->i_type == INODE_SYMLINK &&
	    (uint64_t)inode->i_size <= s->maxsymlinklen &&
	    inode->i_size <= 120)) {
		for (n = 0; n < UFS2_NDADDR; n++)
			if (!valid_inode_fragment(s, ui->direct[n])) {
				inode->i_flags |= INODE_DEAD;
				inode_release(inode);
				kern_free(block);
				return EIO;
			}
		for (n = 0; n < UFS2_NIADDR; n++)
			if (!valid_inode_fragment(s, ui->indirect[n])) {
				inode->i_flags |= INODE_DEAD;
				inode_release(inode);
				kern_free(block);
				return EIO;
			}
	}
	inode->i_op=&ufs2_inode_ops;
	inode->i_fop=inode->i_type==INODE_DIR?&ufs2_directory_ops:
		inode->i_type==INODE_REG?&ufs2_regular_ops:
		inode->i_type==INODE_FIFO?&fifo_file_ops:NULL;
	kern_free(block); *result=inode; return 0;
}

static int
next_dirent(struct inode *directory, off_t *cursor, uint32_t *number,
	uint8_t *type, char name[NAME_MAX+1U])
{
	uint8_t head[8];
	while (*cursor < directory->i_size) {
		uint16_t reclen; uint8_t namelen; ssize_t count;
		if ((uint64_t)*cursor % UFS2_DIRBLKSIZ > UFS2_DIRBLKSIZ-8U) return EIO;
		count=pread_inode(directory,head,sizeof(head),*cursor);
		if (count!=sizeof(head)) return EIO;
		*number=ufs1_get32(head,0,state(directory->i_mount)->super.swapped);
		reclen=ufs1_get16(head,4,state(directory->i_mount)->super.swapped);
		*type=head[6]; namelen=head[7];
		if (reclen<8U || (reclen&3U)!=0 ||
		    8U+namelen>reclen || (uint64_t)*cursor%UFS2_DIRBLKSIZ+reclen>UFS2_DIRBLKSIZ ||
		    (uint64_t)reclen>(uint64_t)directory->i_size-(uint64_t)*cursor) return EIO;
		if (namelen != 0 && pread_inode(directory,name,namelen,*cursor+8)!=namelen) return EIO;
		name[namelen]='\0'; *cursor += reclen;
		if (*number != 0) return 0;
	}
	return ENOENT;
}

static uint16_t dir_minimum(uint8_t length)
{ return (uint16_t)((8U+length+3U)&~3U); }
static uint8_t dir_type(enum inode_type type)
{ return type==INODE_FIFO?1U:type==INODE_DIR?4U:type==INODE_REG?8U:
	type==INODE_SYMLINK?10U:type==INODE_SOCKET?12U:0U; }

static int
restore_directory_block(struct inode *directory, uint64_t fragment,
	const uint8_t *original, int original_error)
{
	struct ufs2_mount_state *ms=state(directory->i_mount);
	if(write_block(directory->i_mount,fragment,original)!=0)
		ms->writable=0;
	return original_error;
}

static int
dir_find_record(struct inode *directory,const struct componentname *name,
	uint8_t *block,uint32_t *offset,uint32_t *previous,uint32_t *number)
{
	struct ufs2_mount_state *ms=state(directory->i_mount);uint32_t pos=0,prev=UINT32_MAX;int error;
	if(directory->i_size<0 || (uint64_t)directory->i_size>ms->super.bsize ||
	    info(directory)->direct[0]==0)
		return EIO;
	error=read_block(directory->i_mount,info(directory)->direct[0],block);if(error)return error;
	while(pos<(uint32_t)directory->i_size){uint32_t ino=ufs1_get32(block,pos,ms->super.swapped);uint16_t reclen=ufs1_get16(block,pos+4U,ms->super.swapped);uint8_t nlen=block[pos+7U];
		if(reclen<8U||(reclen&3U)!=0||pos%UFS2_DIRBLKSIZ+reclen>UFS2_DIRBLKSIZ||pos+reclen>(uint32_t)directory->i_size||8U+nlen>reclen)return EIO;
		if(ino!=0&&nlen==name->cn_namelen&&memcmp(block+pos+8U,name->cn_nameptr,nlen)==0){*offset=pos;*previous=prev;*number=ino;return 0;}
		prev=pos;pos+=reclen;
	}
	return ENOENT;
}

static int
dir_add(struct inode *directory,const struct componentname *name,uint32_t number,uint8_t type)
{
	struct ufs2_mount_state *ms=state(directory->i_mount);struct ufs2_inode_info *ui=info(directory);uint8_t *block,*original;uint16_t need;uint32_t pos=0;uint64_t old_direct,allocated=0,old_blocks;off_t old_size;int error,rollback;
	if(name->cn_namelen==0||name->cn_namelen>255U)
		return EINVAL;
	for(pos=0;pos<name->cn_namelen;pos++)
		if(name->cn_nameptr[pos]=='/')
			return EINVAL;
	pos=0;
	need=dir_minimum((uint8_t)name->cn_namelen);block=kern_calloc(1,ms->super.bsize);original=kern_malloc(ms->super.bsize);if(block==NULL||original==NULL){kern_free(block);kern_free(original);return ENOMEM;}
	mutex_lock(&directory->i_lock);
	old_size=directory->i_size;old_direct=ui->direct[0];old_blocks=ui->blocks;
	if(ui->direct[0]==0){error=allocate_block(directory->i_mount,directory->i_uid,directory->i_gid,&ui->direct[0]);if(error)goto out;allocated=ui->direct[0];ui->blocks+=ms->super.bsize/UFS2_SECTOR_SIZE;}
	error=read_block(directory->i_mount,ui->direct[0],block);if(error)goto out;
	memcpy(original,block,ms->super.bsize);
	while(pos<(uint32_t)directory->i_size){uint16_t reclen=ufs1_get16(block,pos+4U,ms->super.swapped);uint8_t nlen=block[pos+7U];uint16_t minimum=dir_minimum(nlen);
		if(reclen<minimum||pos%UFS2_DIRBLKSIZ+reclen>UFS2_DIRBLKSIZ){error=EIO;goto out;}
		if(reclen-minimum>=need){uint32_t at=pos+minimum;ufs1_put16(block,pos+4U,minimum,ms->super.swapped);ufs1_put32(block,at,number,ms->super.swapped);ufs1_put16(block,at+4U,reclen-minimum,ms->super.swapped);block[at+6U]=type;block[at+7U]=(uint8_t)name->cn_namelen;memcpy(block+at+8U,name->cn_nameptr,name->cn_namelen);error=write_block(directory->i_mount,ui->direct[0],block);goto commit;}
		pos+=reclen;
	}
	if((uint64_t)directory->i_size+UFS2_DIRBLKSIZ>ms->super.bsize){error=ENOSPC;goto out;}
	pos=(uint32_t)directory->i_size;ufs1_put32(block,pos,number,ms->super.swapped);ufs1_put16(block,pos+4U,UFS2_DIRBLKSIZ,ms->super.swapped);block[pos+6U]=type;block[pos+7U]=(uint8_t)name->cn_namelen;memcpy(block+pos+8U,name->cn_nameptr,name->cn_namelen);directory->i_size+=UFS2_DIRBLKSIZ;error=write_block(directory->i_mount,ui->direct[0],block);
commit:	if(error==0)error=persist_inode(directory);
	if(error!=0){
		rollback=restore_directory_block(directory,ui->direct[0],original,error);
		directory->i_size=old_size;ui->direct[0]=old_direct;ui->blocks=old_blocks;
		if(persist_inode(directory)!=0)ms->writable=0;
		if(allocated!=0&&free_block(directory->i_mount,allocated,directory->i_uid,directory->i_gid)!=0)ms->writable=0;
		error=rollback;
	}
out:	if(error!=0&&allocated!=0&&ui->direct[0]==allocated){
		directory->i_size=old_size;ui->direct[0]=old_direct;ui->blocks=old_blocks;
		if(persist_inode(directory)!=0)ms->writable=0;
		if(free_block(directory->i_mount,allocated,directory->i_uid,directory->i_gid)!=0)ms->writable=0;
	}
	mutex_unlock(&directory->i_lock);kern_free(original);kern_free(block);return error;
}

static int
dir_remove(struct inode *directory,const struct componentname *name,uint32_t *number)
{
	struct ufs2_mount_state *ms=state(directory->i_mount);
	uint8_t *block=kern_malloc(ms->super.bsize);
	uint8_t *original=kern_malloc(ms->super.bsize);
	uint32_t offset,previous;
	int error;

	if(block==NULL||original==NULL) {
		kern_free(block);kern_free(original);
		return ENOMEM;
	}
	mutex_lock(&directory->i_lock);
	error=dir_find_record(directory,name,block,&offset,&previous,number);
	if(error==0) {
		memcpy(original,block,ms->super.bsize);
		uint16_t reclen=ufs1_get16(block,offset+4U,
			ms->super.swapped);
		if(previous!=UINT32_MAX &&
		    previous/UFS2_DIRBLKSIZ==offset/UFS2_DIRBLKSIZ) {
			uint16_t prior=ufs1_get16(block,previous+4U,
				ms->super.swapped);
			ufs1_put16(block,previous+4U,prior+reclen,
				ms->super.swapped);
		} else {
			ufs1_put32(block,offset,0,ms->super.swapped);
		}
		error=write_block(directory->i_mount,
			info(directory)->direct[0],block);
		if(error!=0)
			error=restore_directory_block(directory,
				info(directory)->direct[0],original,error);
	}
	mutex_unlock(&directory->i_lock);
	kern_free(original);
	kern_free(block);
	return error;
}

static int
dir_replace(struct inode *directory,const struct componentname *name,
	uint32_t number,uint8_t type,uint32_t *old_number,uint8_t *old_type)
{
	struct ufs2_mount_state *ms=state(directory->i_mount);
	uint8_t *block=kern_malloc(ms->super.bsize);
	uint8_t *original=kern_malloc(ms->super.bsize);
	uint32_t offset,previous;
	int error;

	if(block==NULL||original==NULL) {
		kern_free(block);kern_free(original);
		return ENOMEM;
	}
	mutex_lock(&directory->i_lock);
	error=dir_find_record(directory,name,block,&offset,&previous,old_number);
	if(error==0) {
		memcpy(original,block,ms->super.bsize);
		(void)previous;
		*old_type=block[offset+6U];
		ufs1_put32(block,offset,number,ms->super.swapped);
		block[offset+6U]=type;
		error=write_block(directory->i_mount,
			info(directory)->direct[0],block);
		if(error!=0)
			error=restore_directory_block(directory,
				info(directory)->direct[0],original,error);
	}
	mutex_unlock(&directory->i_lock);
	kern_free(original);
	kern_free(block);
	return error;
}

static int
name_is_dot(const struct componentname *name)
{
	return (name->cn_namelen==1U && name->cn_nameptr[0]=='.') ||
	    (name->cn_namelen==2U && name->cn_nameptr[0]=='.' &&
	    name->cn_nameptr[1]=='.');
}

static int
new_inode(struct mount *mountp,mode_t mode,enum inode_type type,nlink_t links,
	uid_t uid,gid_t gid,struct inode **result)
{
	struct inode *inode;
	uint32_t number=0;
	int error=allocate_inode_number(mountp,uid,gid,&number);

	if(error)
		return error;
	inode=inode_alloc(mountp);
	if(inode==NULL) {
		(void)free_inode_number(mountp,number,uid,gid);
		return ENOSPC;
	}
	inode->i_ino=number;
	inode->i_mode=mode;
	inode->i_type=type;
	inode->i_linkcount=links;
	inode->i_uid=uid;inode->i_gid=gid;
	inode->i_op=&ufs2_inode_ops;
	inode->i_fop=type==INODE_DIR?&ufs2_directory_ops:
		type==INODE_REG?&ufs2_regular_ops:
		type==INODE_FIFO?&fifo_file_ops:NULL;
	info(inode)->generation=number;
	error=persist_inode(inode);
	if(error) {
		inode->i_linkcount=0;
		inode->i_type=INODE_NONE;
		inode->i_mode=0;
		inode->i_flags|=INODE_DEAD;
		inode_release(inode);
		return error;
	}
	if(type==INODE_DIR) {
		error=adjust_directory_count(mountp,number,1);
		if(error!=0) {
			inode->i_linkcount=0;
			inode->i_type=INODE_NONE;
			inode->i_mode=0;
			inode->i_flags|=INODE_DEAD;
			inode_release(inode);
			return error;
		}
	}
	*result=inode;
	return 0;
}

static int
ufs2_lookup(struct inode *directory,const struct componentname *component,
	struct inode **result)
{
	off_t cursor=0; uint32_t number; uint8_t type; char name[NAME_MAX+1U]; int error;
	while ((error=next_dirent(directory,&cursor,&number,&type,name))==0)
		if (strlen(name)==component->cn_namelen &&
		    memcmp(name,component->cn_nameptr,component->cn_namelen)==0)
			return load_inode(directory->i_mount,number,result);
	return error;
}

static int
ufs2_create(struct inode *directory,const struct componentname *name,mode_t mode,
	struct inode **result)
{
	struct inode *existing,*inode;
	struct ufs2_mount_state *ms=state(directory->i_mount);
	int error;
	if(!ms->writable)return EROFS;
	mutex_lock(&ms->namespace_lock);
	error=ufs2_lookup(directory,name,&existing);
	if(error==0){inode_release(existing);error=EEXIST;goto out;}
	if(error!=ENOENT)goto out;
	error=new_inode(directory->i_mount,S_IFREG|(mode&07777U),INODE_REG,1,
	    directory->i_uid,directory->i_gid,&inode);
	if(error)goto out;
	error=dir_add(directory,name,(uint32_t)inode->i_ino,8);
	if(error){inode->i_linkcount=0;inode->i_flags|=INODE_DEAD;inode_release(inode);goto out;}
	*result=inode;
out:
	mutex_unlock(&ms->namespace_lock);
	return error;
}

static int
ufs2_mkdir(struct inode *directory,const struct componentname *name,mode_t mode,
	struct inode **result)
{
	struct inode *existing,*inode;struct componentname dot={".",1,0},dotdot={"..",2,0};
	struct ufs2_mount_state *ms=state(directory->i_mount);
	int error;
	if(!ms->writable)return EROFS;
	mutex_lock(&ms->namespace_lock);
	error=ufs2_lookup(directory,name,&existing);
	if(error==0){inode_release(existing);error=EEXIST;goto out;}
	if(error!=ENOENT)goto out;
	error=new_inode(directory->i_mount,S_IFDIR|(mode&07777U),INODE_DIR,2,
	    directory->i_uid,directory->i_gid,&inode);if(error)goto out;
	error=dir_add(inode,&dot,(uint32_t)inode->i_ino,4);if(error==0)error=dir_add(inode,&dotdot,(uint32_t)directory->i_ino,4);
	if(error==0)error=dir_add(directory,name,(uint32_t)inode->i_ino,4);
	if(error){inode->i_linkcount=0;inode->i_flags|=INODE_DEAD;inode_release(inode);goto out;}
	mutex_lock(&directory->i_lock);
	directory->i_linkcount++;
	error=persist_inode(directory);
	mutex_unlock(&directory->i_lock);
	if(error==0)
		*result=inode;
	else
		inode_release(inode);
out:
	mutex_unlock(&ms->namespace_lock);
	return error;
}

static int
ufs2_mknod(struct inode *directory,const struct componentname *name,
	enum inode_type type,mode_t mode,dev_t rdev,struct inode **result)
{
	struct inode *existing,*inode;
	struct ufs2_mount_state *ms=state(directory->i_mount);
	int error;
	if(type!=INODE_FIFO&&type!=INODE_SOCKET&&type!=INODE_CHAR&&type!=INODE_BLOCK)
		return EOPNOTSUPP;
	if(!ms->writable)
		return EROFS;
	mutex_lock(&ms->namespace_lock);
	error=ufs2_lookup(directory,name,&existing);
	if(error==0){inode_release(existing);error=EEXIST;goto out;}
	if(error!=ENOENT)goto out;
	error=new_inode(directory->i_mount,inode_type_mode(type)|
	    (mode&07777U),type,1,directory->i_uid,directory->i_gid,&inode);
	if(error!=0)goto out;
	inode->i_rdev=rdev;
	error=persist_inode(inode);
	if(error==0)
		error=dir_add(directory,name,(uint32_t)inode->i_ino,
			dir_type(type));
	if(error!=0){
		inode->i_linkcount=0;
		inode->i_flags|=INODE_DEAD;
		inode_release(inode);
		goto out;
	}
	*result=inode;
out:
	mutex_unlock(&ms->namespace_lock);
	return error;
}

static int
ufs2_unlink(struct inode *directory,const struct componentname *name)
{
	struct ufs2_mount_state *ms=state(directory->i_mount);
	struct inode *target=NULL;uint32_t number=0;int error,rollback_error;
	int removed=0;
	nlink_t old_links=0;
	unsigned old_flags=0;
	mutex_lock(&ms->namespace_lock);
	error=ufs2_lookup(directory,name,&target);
	if(error)goto out;
	if(target->i_type==INODE_DIR){error=EISDIR;goto out;}
	old_links=target->i_linkcount;
	old_flags=target->i_flags;
	error=dir_remove(directory,name,&number);
	if(error==0) {
		removed=1;
		mutex_lock(&target->i_lock);
		if(target->i_linkcount==0) {
			error=EIO;
		} else {
			target->i_linkcount--;
			error=persist_inode(target);
			if(target->i_linkcount==0)
				target->i_flags|=INODE_DEAD;
		}
		mutex_unlock(&target->i_lock);
	}
	if(error!=0&&removed) {
		mutex_lock(&target->i_lock);
		target->i_linkcount=old_links;
		target->i_flags=old_flags;
		rollback_error=persist_inode(target);
		mutex_unlock(&target->i_lock);
		if(rollback_error==0)
			rollback_error=dir_add(directory,name,number,
				dir_type(target->i_type));
		if(rollback_error!=0)
			ms->writable=0;
	}
out:
	inode_release(target);
	mutex_unlock(&ms->namespace_lock);
	return error;
}

static int
directory_empty(struct inode *directory)
{
	off_t cursor=0;uint32_t number;uint8_t type;char name[NAME_MAX+1U];int error;
	while((error=next_dirent(directory,&cursor,&number,&type,name))==0)
		if(strcmp(name,".")&&strcmp(name,".."))return 0;
	return error==ENOENT?1:-error;
}

static int
ufs2_rmdir(struct inode *directory,const struct componentname *name)
{
	struct ufs2_mount_state *ms=state(directory->i_mount);
	struct inode *target=NULL;uint32_t number=0;int empty,error,rollback_error;
	int removed=0;
	nlink_t old_target_links=0,old_directory_links=0;
	unsigned old_target_flags=0;
	mutex_lock(&ms->namespace_lock);
	error=ufs2_lookup(directory,name,&target);
	if(error)goto out;
	if(target->i_type!=INODE_DIR){error=ENOTDIR;goto out;}
	empty=directory_empty(target);
	if(empty<=0){error=empty==0?ENOTEMPTY:-empty;goto out;}
	old_target_links=target->i_linkcount;
	old_target_flags=target->i_flags;
	old_directory_links=directory->i_linkcount;
	error=dir_remove(directory,name,&number);
	if(error==0) {
		removed=1;
		mutex_lock(&target->i_lock);
		target->i_linkcount=0;
		target->i_flags|=INODE_DEAD;
		error=persist_inode(target);
		mutex_unlock(&target->i_lock);
		mutex_lock(&directory->i_lock);
		if(directory->i_linkcount>0)
			directory->i_linkcount--;
		if(error==0)
			error=persist_inode(directory);
		mutex_unlock(&directory->i_lock);
	}
	if(error!=0&&removed) {
		mutex_lock(&target->i_lock);
		target->i_linkcount=old_target_links;
		target->i_flags=old_target_flags;
		rollback_error=persist_inode(target);
		mutex_unlock(&target->i_lock);
		mutex_lock(&directory->i_lock);
		directory->i_linkcount=old_directory_links;
		if(rollback_error==0)
			rollback_error=persist_inode(directory);
		mutex_unlock(&directory->i_lock);
		if(rollback_error==0)
			rollback_error=dir_add(directory,name,number,4);
		if(rollback_error!=0)
			ms->writable=0;
	}
out:
	inode_release(target);
	mutex_unlock(&ms->namespace_lock);
	return error;
}

static int
ufs2_rename(struct inode *old_directory,const struct componentname *old_name,
	struct inode *new_directory,const struct componentname *new_name,
	unsigned flags)
{
	struct ufs2_mount_state *ms=state(old_directory->i_mount);
	struct inode *source=NULL,*target=NULL;
	uint32_t removed=0,replaced=0;
	uint8_t replaced_type=0;
	nlink_t old_target_links=0,old_old_directory_links=0;
	nlink_t old_new_directory_links=0;
	unsigned old_target_flags=0;
	int target_exists=0,namespace_committed=0,dotdot_changed=0;
	int empty,error,rollback_error=0;

	if(flags!=0)
		return EINVAL;
	if(old_directory->i_mount!=new_directory->i_mount)
		return EXDEV;
	if(name_is_dot(old_name)||name_is_dot(new_name))
		return EINVAL;
	if(old_directory==new_directory &&
	    old_name->cn_namelen==new_name->cn_namelen &&
	    memcmp(old_name->cn_nameptr,new_name->cn_nameptr,
	    old_name->cn_namelen)==0)
		return 0;

	mutex_lock(&ms->namespace_lock);
	error=ufs2_lookup(old_directory,old_name,&source);
	if(error!=0)
		goto out;
	error=ufs2_lookup(new_directory,new_name,&target);
	if(error==0) {
		target_exists=1;
		if(target->i_ino==source->i_ino) {
			error=0;
			goto out;
		}
		if(source->i_type==INODE_DIR && target->i_type!=INODE_DIR) {
			error=ENOTDIR;
			goto out;
		}
		if(source->i_type!=INODE_DIR && target->i_type==INODE_DIR) {
			error=EISDIR;
			goto out;
		}
		if(target->i_type==INODE_DIR) {
			empty=directory_empty(target);
			if(empty<=0) {
				error=empty==0?ENOTEMPTY:-empty;
				goto out;
			}
		}
	} else if(error==ENOENT) {
		error=0;
	} else {
		goto out;
	}
	old_old_directory_links=old_directory->i_linkcount;
	old_new_directory_links=new_directory->i_linkcount;
	if(target_exists) {
		old_target_links=target->i_linkcount;
		old_target_flags=target->i_flags;
	}
	if(target_exists) {
		error=dir_replace(new_directory,new_name,(uint32_t)source->i_ino,
			dir_type(source->i_type),&replaced,&replaced_type);
	} else {
		error=dir_add(new_directory,new_name,(uint32_t)source->i_ino,
			dir_type(source->i_type));
	}
	if(error!=0)
		goto out;
	error=dir_remove(old_directory,old_name,&removed);
	if(error!=0) {
		if(target_exists) {
			uint32_t ignored;
			uint8_t ignored_type;
			(void)dir_replace(new_directory,new_name,replaced,
				replaced_type,&ignored,&ignored_type);
		} else {
			uint32_t ignored;
			(void)dir_remove(new_directory,new_name,&ignored);
		}
		goto out;
	}
	if(removed!=(uint32_t)source->i_ino) {
		error=EIO;
		goto out;
	}
	namespace_committed=1;

	if(source->i_type==INODE_DIR && old_directory!=new_directory) {
		static const struct componentname dotdot={"..",2,0};
		uint32_t old_parent;
		uint8_t old_parent_type;
		error=dir_replace(source,&dotdot,(uint32_t)new_directory->i_ino,4,
			&old_parent,&old_parent_type);
		if(error!=0)
			goto out;
		dotdot_changed=1;
		(void)old_parent;
		(void)old_parent_type;
	}

	if(target_exists) {
		mutex_lock(&target->i_lock);
		if(target->i_type==INODE_DIR)
			target->i_linkcount=0;
		else if(target->i_linkcount!=0)
			target->i_linkcount--;
		else
			error=EIO;
		if(error==0)
			error=persist_inode(target);
		if(target->i_linkcount==0)
			target->i_flags|=INODE_DEAD;
		mutex_unlock(&target->i_lock);
	}
	if(error==0 && source->i_type==INODE_DIR) {
		if(old_directory!=new_directory) {
			mutex_lock(&old_directory->i_lock);
			if(old_directory->i_linkcount!=0)
				old_directory->i_linkcount--;
			error=persist_inode(old_directory);
			mutex_unlock(&old_directory->i_lock);
			if(error==0) {
				mutex_lock(&new_directory->i_lock);
				new_directory->i_linkcount++;
				if(target_exists && target->i_type==INODE_DIR &&
				    new_directory->i_linkcount!=0)
					new_directory->i_linkcount--;
				error=persist_inode(new_directory);
				mutex_unlock(&new_directory->i_lock);
			}
		} else if(target_exists && target->i_type==INODE_DIR) {
			mutex_lock(&old_directory->i_lock);
			if(old_directory->i_linkcount!=0)
				old_directory->i_linkcount--;
			error=persist_inode(old_directory);
			mutex_unlock(&old_directory->i_lock);
		}
	}
out:
	if(error!=0&&namespace_committed) {
		static const struct componentname dotdot={"..",2,0};
		uint32_t ignored;
		uint8_t ignored_type;

		if(dotdot_changed&&dir_replace(source,&dotdot,
		    (uint32_t)old_directory->i_ino,4,&ignored,
		    &ignored_type)!=0)
			rollback_error=EIO;
		if(target_exists) {
			if(dir_replace(new_directory,new_name,
			    (uint32_t)target->i_ino,dir_type(target->i_type),
			    &ignored,&ignored_type)!=0)
				rollback_error=EIO;
		} else if(dir_remove(new_directory,new_name,&ignored)!=0) {
			rollback_error=EIO;
		}
		if(dir_add(old_directory,old_name,(uint32_t)source->i_ino,
		    dir_type(source->i_type))!=0)
			rollback_error=EIO;
		if(target_exists) {
			mutex_lock(&target->i_lock);
			target->i_linkcount=old_target_links;
			target->i_flags=old_target_flags;
			if(persist_inode(target)!=0)
				rollback_error=EIO;
			mutex_unlock(&target->i_lock);
		}
		mutex_lock(&old_directory->i_lock);
		old_directory->i_linkcount=old_old_directory_links;
		if(persist_inode(old_directory)!=0)
			rollback_error=EIO;
		mutex_unlock(&old_directory->i_lock);
		if(new_directory!=old_directory) {
			mutex_lock(&new_directory->i_lock);
			new_directory->i_linkcount=old_new_directory_links;
			if(persist_inode(new_directory)!=0)
				rollback_error=EIO;
			mutex_unlock(&new_directory->i_lock);
		}
		if(rollback_error!=0)
			ms->writable=0;
	}
	inode_release(target);
	inode_release(source);
	mutex_unlock(&ms->namespace_lock);
	return error;
}

static int
ufs2_link(struct inode *directory,const struct componentname *name,struct inode *target)
{
	struct ufs2_mount_state *ms=state(directory->i_mount);
	struct inode *existing;uint32_t removed;int error,rollback_error;
	if(target==NULL||target->i_mount!=directory->i_mount)return EXDEV;
	if(target->i_type==INODE_DIR)return EPERM;
	mutex_lock(&ms->namespace_lock);
	mutex_lock(&target->i_lock);
	if(target->i_linkcount==UINT16_MAX){mutex_unlock(&target->i_lock);error=EMLINK;goto out;}
	mutex_unlock(&target->i_lock);
	error=ufs2_lookup(directory,name,&existing);
	if(error==0){inode_release(existing);error=EEXIST;goto out;}
	if(error!=ENOENT)goto out;
	error=dir_add(directory,name,(uint32_t)target->i_ino,dir_type(target->i_type));
	if(error==0) {
		/* inode_link() applies the in-memory increment after this callback. */
		mutex_lock(&target->i_lock);
		target->i_linkcount++;
		error=persist_inode(target);
		target->i_linkcount--;
		mutex_unlock(&target->i_lock);
		if(error!=0) {
			mutex_lock(&target->i_lock);
			rollback_error=persist_inode(target);
			mutex_unlock(&target->i_lock);
			if(rollback_error==0)
				rollback_error=dir_remove(directory,name,&removed);
			if(rollback_error!=0)
				ms->writable=0;
		}
	}
out:
	mutex_unlock(&ms->namespace_lock);
	return error;
}

static int
ufs2_symlink(struct inode *directory,const struct componentname *name,
	const char *target,struct inode **result)
{
	struct ufs2_mount_state *ms=state(directory->i_mount);
	struct inode *existing,*inode;size_t length=strlen(target);int error;
	if(length>state(directory->i_mount)->super.maxsymlinklen||length>120U)return ENAMETOOLONG;
	mutex_lock(&ms->namespace_lock);
	error=ufs2_lookup(directory,name,&existing);
	if(error==0){inode_release(existing);error=EEXIST;goto out;}
	if(error!=ENOENT)goto out;
	error=new_inode(directory->i_mount,S_IFLNK|0777U,INODE_SYMLINK,1,
	    directory->i_uid,directory->i_gid,&inode);if(error)goto out;
	inode->i_size=(off_t)length;memcpy(info(inode)->shortlink,target,length);error=persist_inode(inode);
	if(error==0)error=dir_add(directory,name,(uint32_t)inode->i_ino,10);
	if(error){inode->i_linkcount=0;inode->i_flags|=INODE_DEAD;inode_release(inode);goto out;}
	*result=inode;
out:
	mutex_unlock(&ms->namespace_lock);
	return error;
}

static ssize_t ufs2_read(struct file *file,void *buffer,size_t length)
{ ssize_t n=pread_inode(file->f_inode,buffer,length,file->f_offset); if(n>0)file->f_offset+=n; return n; }
static ssize_t ufs2_pread(struct file *file,void *buffer,size_t length,off_t offset)
{ return pread_inode(file->f_inode,buffer,length,offset); }
static ssize_t ufs2_write(struct file *file,const void *buffer,size_t length)
{ ssize_t n=pwrite_inode(file->f_inode,buffer,length,file->f_offset);if(n>0)file->f_offset+=n;return n; }
static ssize_t ufs2_pwrite(struct file *file,const void *buffer,size_t length,off_t offset)
{ return pwrite_inode(file->f_inode,buffer,length,offset); }
static int ufs2_readdir(struct file *file,struct dirent *entry,int *eof)
{
	uint32_t number; uint8_t type; char name[NAME_MAX+1U]; int error=next_dirent(file->f_inode,&file->f_offset,&number,&type,name);
	if(error==ENOENT){*eof=1;return 0;} if(error)return error;
	memset(entry,0,sizeof(*entry)); entry->d_ino=number;
	entry->d_type=type==1?INODE_FIFO:type==4?INODE_DIR:type==8?INODE_REG:
		type==10?INODE_SYMLINK:type==12?INODE_SOCKET:INODE_NONE;
	strcpy(entry->d_name,name); *eof=0; return 0;
}
static ssize_t ufs2_readlink(struct inode *inode,char *buffer,size_t length)
{
	struct ufs2_mount_state *ms=state(inode->i_mount);
	if(inode->i_type!=INODE_SYMLINK)return -EINVAL;
	if((uint64_t)inode->i_size<=ms->super.maxsymlinklen && inode->i_size<=120){size_t n=(size_t)inode->i_size;if(n>length)n=length;memcpy(buffer,info(inode)->shortlink,n);return (ssize_t)n;}
	return pread_inode(inode,buffer,length,0);
}

static size_t
extattr_align(size_t value)
{ return (value+7U)&~(size_t)7U; }

static int
extattr_name(const char *name,uint8_t *name_space,const char **stored,
	size_t *stored_length)
{
	const char *part;
	if(name==NULL||name_space==NULL||stored==NULL||stored_length==NULL)
		return EINVAL;
	if(strncmp(name,"user.",5)==0) {
		*name_space=UFS2_EXTATTR_NAMESPACE_USER;part=name+5;
	} else if(strncmp(name,"system.",7)==0) {
		*name_space=UFS2_EXTATTR_NAMESPACE_SYSTEM;part=name+7;
		if(strncmp(part,"security.",9)==0)return EINVAL;
	} else if(strncmp(name,"security.",9)==0) {
		*name_space=UFS2_EXTATTR_NAMESPACE_SYSTEM;part=name;
	} else return EOPNOTSUPP;
	*stored_length=strlen(part);
	if(*stored_length==0||*stored_length>255U)return EINVAL;
	*stored=part;return 0;
}

static int
extattr_load(struct inode *inode,uint8_t **result,size_t *length)
{
	struct ufs2_inode_info *ui=info(inode);
	struct ufs2_mount_state *ms=state(inode->i_mount);
	uint8_t *area;unsigned block_count,index;size_t offset=0;
	if(result==NULL||length==NULL)return EINVAL;
	*result=NULL;*length=ui->extattr_size;
	if(ui->extattr_size==0)return 0;
	block_count=(ui->extattr_size+ms->super.bsize-1U)/ms->super.bsize;
	if(block_count==0||block_count>UFS2_NXADDR)return EIO;
	area=kern_calloc(block_count,ms->super.bsize);
	if(area==NULL)return ENOMEM;
	for(index=0;index<block_count;index++) {
		int error=read_block(inode->i_mount,ui->extattr[index],
		    area+index*ms->super.bsize);
		if(error!=0){kern_free(area);return error;}
	}
	while(offset<ui->extattr_size) {
		uint32_t record;
		uint8_t name_length,padding;
		size_t base;
		if(ui->extattr_size-offset<UFS2_EXTATTR_HEADER_SIZE)goto invalid;
		record=ufs1_get32(area,offset,ms->super.swapped);
		padding=area[offset+5U];name_length=area[offset+6U];
		base=extattr_align(UFS2_EXTATTR_HEADER_SIZE+name_length);
		if(record<base||(record&7U)!=0||record>ui->extattr_size-offset||
		    padding>record-base||area[offset+4U]<UFS2_EXTATTR_NAMESPACE_USER||
		    area[offset+4U]>UFS2_EXTATTR_NAMESPACE_SYSTEM)
			goto invalid;
		offset+=record;
	}
	*result=area;return 0;
invalid:
	kern_free(area);return EIO;
}

static int
extattr_find(struct inode *inode,const uint8_t *area,size_t area_length,
	uint8_t name_space,const char *name,size_t name_length,size_t *at,
	size_t *record_length,size_t *content_at,size_t *content_length)
{
	struct ufs2_mount_state *ms=state(inode->i_mount);size_t offset=0;
	while(offset<area_length) {
		uint32_t record=ufs1_get32(area,offset,ms->super.swapped);
		uint8_t disk_name_length=area[offset+6U];
		size_t base=extattr_align(UFS2_EXTATTR_HEADER_SIZE+disk_name_length);
		if(area[offset+4U]==name_space&&disk_name_length==name_length&&
		    memcmp(area+offset+UFS2_EXTATTR_HEADER_SIZE,name,name_length)==0) {
			if(at!=NULL)
				*at=offset;
			if(record_length!=NULL)
				*record_length=record;
			if(content_at!=NULL)
				*content_at=offset+base;
			if(content_length!=NULL)
				*content_length=record-base-area[offset+5U];
			return 0;
		}
		offset+=record;
	}
	return ENODATA;
}

static int
extattr_publish(struct inode *inode,const uint8_t *area,size_t length)
{
	struct ufs2_inode_info *ui=info(inode);
	struct ufs2_mount_state *ms=state(inode->i_mount);
	uint64_t old_ext[UFS2_NXADDR],new_fragment=0;uint64_t old_blocks;
	uint32_t old_size=ui->extattr_size;uint8_t *block=NULL,*old_area=NULL;
	size_t old_area_length=0;unsigned old_count,index;int error=0,rollback;
	if(length>ms->super.bsize)return ENOSPC;
	old_ext[0]=ui->extattr[0];old_ext[1]=ui->extattr[1];old_blocks=ui->blocks;
	old_count=old_size==0?0U:(old_size+ms->super.bsize-1U)/ms->super.bsize;
	if(old_size!=0)error=extattr_load(inode,&old_area,&old_area_length);
	if(error==0&&old_area_length!=old_size)error=EIO;
	if(error!=0)return error;
	if(area==NULL)length=0;
	if(length==0) {
		ui->extattr_size=0;ui->extattr[0]=0;ui->extattr[1]=0;
		ui->blocks=old_blocks-(uint64_t)old_count*(ms->super.bsize/UFS2_SECTOR_SIZE);
		error=persist_inode(inode);
		if(error!=0){ui->extattr_size=old_size;ui->extattr[0]=old_ext[0];
			ui->extattr[1]=old_ext[1];ui->blocks=old_blocks;
			if(persist_inode(inode)!=0)ms->writable=0;
			kern_free(old_area);return error;}
		for(index=0;index<old_count;index++)if((rollback=free_block(inode->i_mount,
		    old_ext[index],inode->i_uid,inode->i_gid))!=0){ms->writable=0;error=rollback;break;}
		kern_free(old_area);return error;
	}
	block=kern_calloc(1,ms->super.bsize);if(block==NULL){kern_free(old_area);return ENOMEM;}
	memcpy(block,area,length);
	if(old_ext[0]==0){error=allocate_block(inode->i_mount,inode->i_uid,inode->i_gid,&new_fragment);if(error!=0)goto out;}
	else new_fragment=old_ext[0];
	error=write_block(inode->i_mount,new_fragment,block);if(error!=0)goto rollback_data;
	ui->extattr_size=(uint32_t)length;ui->extattr[0]=new_fragment;ui->extattr[1]=0;
	ui->blocks=old_blocks-(uint64_t)old_count*(ms->super.bsize/UFS2_SECTOR_SIZE)+
	    ms->super.bsize/UFS2_SECTOR_SIZE;
	error=persist_inode(inode);if(error!=0)goto rollback_metadata;
	if(old_count>1U&&(rollback=free_block(inode->i_mount,old_ext[1],inode->i_uid,inode->i_gid))!=0){
		ms->writable=0;error=rollback;
	}
	goto out;
rollback_metadata:
	ui->extattr_size=old_size;ui->extattr[0]=old_ext[0];ui->extattr[1]=old_ext[1];
	ui->blocks=old_blocks;
	if(persist_inode(inode)!=0)
		ms->writable=0;
rollback_data:
	if(old_ext[0]==0) {
		if(new_fragment!=0&&(rollback=free_block(inode->i_mount,new_fragment,inode->i_uid,inode->i_gid))!=0)
			ms->writable=0;
	} else if(old_area!=NULL&&write_block(inode->i_mount,old_ext[0],old_area)!=0)
		ms->writable=0;
out:
	kern_free(old_area);kern_free(block);return error;
}

static ssize_t
ufs2_getxattr(struct inode *inode,const char *name,void *value,size_t size)
{
	uint8_t name_space,*area=NULL;const char *stored;size_t stored_length;
	size_t area_length,content_at,content_length;int error;
	error=extattr_name(name,&name_space,&stored,&stored_length);if(error!=0)return -error;
	mutex_lock(&inode->i_lock);error=extattr_load(inode,&area,&area_length);
	if(error==0)error=extattr_find(inode,area,area_length,name_space,stored,
	    stored_length,NULL,NULL,&content_at,&content_length);
	if(error==0&&value!=NULL&&size<content_length)error=ERANGE;
	if(error==0&&value!=NULL&&content_length!=0)memcpy(value,area+content_at,content_length);
	mutex_unlock(&inode->i_lock);kern_free(area);
	return error!=0?-(ssize_t)error:(ssize_t)content_length;
}

static int
ufs2_setxattr(struct inode *inode,const char *name,const void *value,size_t size,
	unsigned flags)
{
	struct ufs2_mount_state *ms=state(inode->i_mount);uint8_t name_space,*area=NULL,*updated=NULL;
	const char *stored;size_t stored_length,area_length=0,at=0,old_record=0;
	size_t base,new_record,new_length,padding;int found,error;
	if(!ms->writable)
		return EROFS;
	if(value==NULL&&size!=0)
		return EINVAL;
	error=extattr_name(name,&name_space,&stored,&stored_length);if(error!=0)return error;
	base=extattr_align(UFS2_EXTATTR_HEADER_SIZE+stored_length);
	if(size>ms->super.bsize||base>ms->super.bsize-size)return E2BIG;
	new_record=extattr_align(base+size);padding=new_record-base-size;
	mutex_lock(&inode->i_lock);error=extattr_load(inode,&area,&area_length);if(error!=0)goto out;
	found=extattr_find(inode,area,area_length,name_space,stored,stored_length,
	    &at,&old_record,NULL,NULL)==0;
	if((flags&INODE_XATTR_CREATE)!=0&&found){error=EEXIST;goto out;}
	if((flags&INODE_XATTR_REPLACE)!=0&&!found){error=ENODATA;goto out;}
	new_length=area_length-(found?old_record:0U)+new_record;
	if(new_length>ms->super.bsize){error=ENOSPC;goto out;}
	updated=kern_calloc(1,ms->super.bsize);if(updated==NULL){error=ENOMEM;goto out;}
	if(at!=0)memcpy(updated,area,at);
	ufs1_put32(updated,at,(uint32_t)new_record,ms->super.swapped);
	updated[at+4U]=name_space;updated[at+5U]=(uint8_t)padding;
	updated[at+6U]=(uint8_t)stored_length;
	memcpy(updated+at+UFS2_EXTATTR_HEADER_SIZE,stored,stored_length);
	if(size!=0)memcpy(updated+at+base,value,size);
	if(area_length>at+(found?old_record:0U))memcpy(updated+at+new_record,
	    area+at+(found?old_record:0U),area_length-at-(found?old_record:0U));
	error=extattr_publish(inode,updated,new_length);
out:
	mutex_unlock(&inode->i_lock);kern_free(updated);kern_free(area);return error;
}

static ssize_t
ufs2_listxattr(struct inode *inode,char *list,size_t size)
{
	struct ufs2_mount_state *ms=state(inode->i_mount);uint8_t *area=NULL;
	size_t area_length,offset=0,needed=0;int error;
	mutex_lock(&inode->i_lock);error=extattr_load(inode,&area,&area_length);
	if(error!=0)goto out;
	while(offset<area_length){uint32_t record=ufs1_get32(area,offset,ms->super.swapped);
		uint8_t ns=area[offset+4U],nlen=area[offset+6U];const char *prefix;
		const uint8_t *disk_name=area+offset+UFS2_EXTATTR_HEADER_SIZE;size_t prefix_length;
		if(ns==UFS2_EXTATTR_NAMESPACE_USER){prefix="user.";prefix_length=5U;}
		else if(nlen>=9U&&memcmp(disk_name,"security.",9)==0){prefix="";prefix_length=0;}
		else {prefix="system.";prefix_length=7U;}
		if(list!=NULL&&(needed>size||prefix_length+nlen+1U>size-needed))
			{error=ERANGE;goto out;}
		if(list!=NULL){memcpy(list+needed,prefix,prefix_length);
			memcpy(list+needed+prefix_length,disk_name,nlen);
			list[needed+prefix_length+nlen]='\0';}
		needed+=prefix_length+nlen+1U;offset+=record;
	}
out:
	mutex_unlock(&inode->i_lock);kern_free(area);
	return error!=0?-(ssize_t)error:(ssize_t)needed;
}

static int
ufs2_removexattr(struct inode *inode,const char *name)
{
	struct ufs2_mount_state *ms=state(inode->i_mount);uint8_t name_space,*area=NULL,*updated=NULL;
	const char *stored;size_t stored_length,area_length,at,record,new_length;int error;
	if(!ms->writable)return EROFS;
	error=extattr_name(name,&name_space,&stored,&stored_length);if(error!=0)return error;
	mutex_lock(&inode->i_lock);error=extattr_load(inode,&area,&area_length);if(error!=0)goto out;
	error=extattr_find(inode,area,area_length,name_space,stored,stored_length,
	    &at,&record,NULL,NULL);if(error!=0)goto out;
	new_length=area_length-record;
	if(new_length!=0){updated=kern_calloc(1,ms->super.bsize);if(updated==NULL){error=ENOMEM;goto out;}
		if(at!=0)
			memcpy(updated,area,at);
		if(area_length>at+record)
			memcpy(updated+at,area+at+record,area_length-at-record);}
	error=extattr_publish(inode,updated,new_length);
out:
	mutex_unlock(&inode->i_lock);kern_free(updated);kern_free(area);return error;
}

static int
ufs2_getattr(struct inode *inode,struct stat *status)
{
	struct ufs2_inode_info *ui=info(inode);

	memset(status,0,sizeof(*status));
	status->st_dev=inode->i_mount->m_disk->d_dev;
	status->st_ino=inode->i_ino;
	status->st_mode=inode->i_mode;
	status->st_nlink=inode->i_linkcount;
	status->st_uid=inode->i_uid;
	status->st_gid=inode->i_gid;
	status->st_rdev=inode->i_rdev;
	status->st_size=inode->i_size;
	status->st_atime=inode->i_atime.tv_sec;
	status->st_mtime=inode->i_mtime.tv_sec;
	status->st_ctime=inode->i_ctime.tv_sec;
	status->st_blksize=state(inode->i_mount)->super.bsize;
	status->st_blocks=ui->blocks;
	return 0;
}

static int
valid_disk_time(time_t seconds,long nanoseconds)
{
	(void)seconds;
	return nanoseconds>=0 && nanoseconds<1000000000L;
}

static int
ufs2_setattr(struct inode *inode,const struct stat *status,unsigned mask)
{
	struct quota_transfer quota_transfer_state;
	mode_t old_mode;
	uid_t old_uid;
	gid_t old_gid;
	struct inode_time old_atime,old_mtime,old_ctime;
	long atime_nsec=0,mtime_nsec=0,ctime_nsec=0;
	int error,quota_moved=0;
	memset(&quota_transfer_state,0,sizeof(quota_transfer_state));

#ifdef ZEDBSD_SYS_STAT_H
	atime_nsec=status->st_atim.tv_nsec;
	mtime_nsec=status->st_mtim.tv_nsec;
	ctime_nsec=status->st_ctim.tv_nsec;
#endif
	if((mask&INODE_ATTR_ATIME)!=0 &&
	    !valid_disk_time(status->st_atime,atime_nsec))
		return EOVERFLOW;
	if((mask&INODE_ATTR_MTIME)!=0 &&
	    !valid_disk_time(status->st_mtime,mtime_nsec))
		return EOVERFLOW;
	if((mask&INODE_ATTR_CTIME)!=0 &&
	    !valid_disk_time(status->st_ctime,ctime_nsec))
		return EOVERFLOW;
	if((mask&INODE_ATTR_SIZE)!=0) {
		error=ufs2_truncate(inode,status->st_size);
		if(error!=0)
			return error;
	}

	mutex_lock(&inode->i_lock);
	old_mode=inode->i_mode;
	old_uid=inode->i_uid;
	old_gid=inode->i_gid;
	old_atime=inode->i_atime;
	old_mtime=inode->i_mtime;
	old_ctime=inode->i_ctime;
	if((mask&(INODE_ATTR_UID|INODE_ATTR_GID))!=0) {
		uid_t new_uid=(mask&INODE_ATTR_UID)!=0?status->st_uid:old_uid;
		gid_t new_gid=(mask&INODE_ATTR_GID)!=0?status->st_gid:old_gid;
		error=quota_transfer_begin(&state(inode->i_mount)->quota,
		    old_uid,old_gid,
		    new_uid,new_gid,info(inode)->blocks/
		    (state(inode->i_mount)->super.bsize/UFS2_SECTOR_SIZE),1,
		    quota_now(),&quota_transfer_state);
		if(error!=0){mutex_unlock(&inode->i_lock);return error;}
		quota_moved=old_uid!=new_uid||old_gid!=new_gid;
	}
	if(mask&INODE_ATTR_MODE)
		inode->i_mode=(inode->i_mode&S_IFMT)|(status->st_mode&~S_IFMT);
	if(mask&INODE_ATTR_UID)
		inode->i_uid=status->st_uid;
	if(mask&INODE_ATTR_GID)
		inode->i_gid=status->st_gid;
	if(mask&INODE_ATTR_ATIME) {
		inode->i_atime.tv_sec=status->st_atime;
		inode->i_atime.tv_nsec=atime_nsec;
	}
	if(mask&INODE_ATTR_MTIME) {
		inode->i_mtime.tv_sec=status->st_mtime;
		inode->i_mtime.tv_nsec=mtime_nsec;
	}
	if(mask&INODE_ATTR_CTIME) {
		inode->i_ctime.tv_sec=status->st_ctime;
		inode->i_ctime.tv_nsec=ctime_nsec;
	}
	error=persist_inode(inode);
	if(error!=0) {
		if(quota_moved)
			quota_transfer_rollback(&quota_transfer_state);
		inode->i_mode=old_mode;
		inode->i_uid=old_uid;
		inode->i_gid=old_gid;
		inode->i_atime=old_atime;
		inode->i_mtime=old_mtime;
		inode->i_ctime=old_ctime;
	} else if(quota_moved) {
		quota_transfer_commit(&quota_transfer_state);
	}
	mutex_unlock(&inode->i_lock);
	return error;
}

static int
ufs2_inode_sync(struct inode *inode)
{
	int error;

	mutex_lock(&inode->i_lock);
	error=persist_inode(inode);
	mutex_unlock(&inode->i_lock);
	return error;
}

static void ufs2_reclaim(struct inode *inode)
{
	struct ufs2_inode_info *ui=info(inode);
	if(inode->i_linkcount!=0||inode->i_ino<=UFS2_ROOT_INO||!state(inode->i_mount)->writable)return;
	if(ufs2_truncate(inode,0)!=0)
		return;
	mutex_lock(&inode->i_lock);
	if(extattr_publish(inode,NULL,0)!=0){mutex_unlock(&inode->i_lock);return;}
	mutex_unlock(&inode->i_lock);
	if(inode->i_type==INODE_DIR&&adjust_directory_count(inode->i_mount,
	    (uint32_t)inode->i_ino,-1)!=0)
		return;
	inode->i_mode=0;inode->i_type=INODE_NONE;ui->blocks=0;
	if(persist_inode(inode)==0)
		(void)free_inode_number(inode->i_mount,(uint32_t)inode->i_ino,
		    inode->i_uid,inode->i_gid);
}
static const struct inode_ops ufs2_inode_ops={.lookup=ufs2_lookup,.create=ufs2_create,
	.mkdir=ufs2_mkdir,.mknod=ufs2_mknod,.unlink=ufs2_unlink,.rmdir=ufs2_rmdir,.rename=ufs2_rename,
	.link=ufs2_link,
	.symlink=ufs2_symlink,.readlink=ufs2_readlink,.getattr=ufs2_getattr,
	.setattr=ufs2_setattr,.truncate=ufs2_truncate,.sync=ufs2_inode_sync,
	.getxattr=ufs2_getxattr,.setxattr=ufs2_setxattr,
	.listxattr=ufs2_listxattr,.removexattr=ufs2_removexattr,
	.reclaim=ufs2_reclaim};
static int ufs2_file_sync(struct file *file)
{
	int error;
	if(file==NULL)return EINVAL;
	error=inode_sync(file->f_inode);
	return error!=0?error:disk_sync(file->f_inode->i_mount->m_disk);
}
static const struct file_ops ufs2_regular_ops={.read=ufs2_read,.write=ufs2_write,.pread=ufs2_pread,.pwrite=ufs2_pwrite,.fsync=ufs2_file_sync};
static const struct file_ops ufs2_directory_ops={.readdir=ufs2_readdir};

static struct inode *ufs2_alloc_inode(struct mount *mountp)
{ (void)mountp; return (struct inode *)kern_calloc(1,sizeof(struct ufs2_inode_info)); }
static void ufs2_free_inode(struct inode *inode) { kern_free(inode); }

static int ufs2_read_super(struct disk *disk,struct ufs2_super *super)
{
	uint8_t *buffer; int error;
	if(disk==NULL||disk->d_block_size!=UFS2_SECTOR_SIZE)return EOPNOTSUPP;
	buffer=kern_malloc(UFS2_SBLOCK_SIZE); if(buffer==NULL)return ENOMEM;
	error=disk_read(disk,UFS2_SBLOCK_OFFSET/UFS2_SECTOR_SIZE,UFS2_SBLOCK_SIZE/UFS2_SECTOR_SIZE,buffer);
	if(error==0)error=ufs2_super_decode(buffer,UFS2_SBLOCK_SIZE,disk->d_block_count,super);
	kern_free(buffer); return error;
}
static int
ufs2_write_clean(struct mount *mountp,uint8_t clean)
{
	struct ufs2_mount_state *ms=state(mountp);uint8_t *buffer;int error;
	buffer=kern_malloc(UFS2_SBLOCK_SIZE);if(buffer==NULL)return ENOMEM;
	error=disk_read(mountp->m_disk,UFS2_SBLOCK_OFFSET/UFS2_SECTOR_SIZE,UFS2_SBLOCK_SIZE/UFS2_SECTOR_SIZE,buffer);
	if(error==0){buffer[UFS2_FS_CLEAN]=clean;error=write_sectors(mountp,UFS2_SBLOCK_OFFSET/UFS2_SECTOR_SIZE,UFS2_SBLOCK_SIZE/UFS2_SECTOR_SIZE,buffer);}
	if(error==0)
		error=disk_sync(mountp->m_disk);
	if(error==0)
		ms->super.clean=clean;
	kern_free(buffer);
	return error;
}
static int ufs2_probe(struct disk *disk) { struct ufs2_super s; return ufs2_read_super(disk,&s); }

static int
ufs2_quota_rebuild(struct mount *mountp)
{
	struct ufs2_mount_state *ms=state(mountp);uint8_t *block;
	uint32_t cg,index;int error=0;
	block=kern_malloc(ms->super.bsize);if(block==NULL)return ENOMEM;
	for(cg=0;cg<ms->super.ncg&&error==0;cg++) {
		error=load_cg_locked(mountp,cg);if(error!=0)break;
		for(index=0;index<ms->super.ipg;index++) {
			uint64_t fragment,blocks;uint8_t *raw;uint16_t mode;
			if(!bit_test(ms->cg+ms->cg_iusedoff,index))continue;
			fragment=cgstart(&ms->super,cg)+ms->super.iblkno+
			    (index/ms->super.inopb)*ms->super.frag;
			error=read_block(mountp,fragment,block);if(error!=0)break;
			raw=block+(index%ms->super.inopb)*UFS2_DINODE_SIZE;
			mode=ufs1_get16(raw,UFS2_DI_MODE,ms->super.swapped);
			if(mode==0)continue;
			blocks=ufs1_get64(raw,UFS2_DI_BLOCKS,ms->super.swapped);
			if(blocks%(ms->super.bsize/UFS2_SECTOR_SIZE)!=0){error=EIO;break;}
			error=quota_rebuild_add(&ms->quota,
			    ufs1_get32(raw,UFS2_DI_UID,ms->super.swapped),
			    ufs1_get32(raw,UFS2_DI_GID,ms->super.swapped),
			    blocks/(ms->super.bsize/UFS2_SECTOR_SIZE),1);
			if(error!=0)break;
		}
	}
	kern_free(block);return error;
}

static int
ufs2_quota_load(struct mount *mountp,struct inode *root)
{
	struct ufs2_mount_state *ms=state(mountp);uint8_t *buffer;
	ssize_t length,loaded;int error;
	length=ufs2_getxattr(root,UFS2_QUOTA_XATTR,NULL,0);
	if(length==-ENODATA)return 0;
	if(length<0)return (int)-length;
	if(length==0||(size_t)length>ms->super.bsize)return EINVAL;
	buffer=kern_malloc((size_t)length);if(buffer==NULL)return ENOMEM;
	loaded=ufs2_getxattr(root,UFS2_QUOTA_XATTR,buffer,(size_t)length);
	error=loaded==length?quota_import_config(&ms->quota,buffer,(size_t)length):
	    (loaded<0?(int)-loaded:EIO);
	kern_free(buffer);return error;
}

static int
snapshot_disk_submit(struct disk *disk,struct bio *bio)
{
	struct ufs2_mount_state *ms=disk!=NULL?disk->d_data:NULL;int error;
	if(ms==NULL||bio==NULL)return EINVAL;
	if(bio->b_op==BIO_FLUSH)error=disk_sync(ms->snapshot.io.context);
	else if(bio->b_op!=BIO_READ)error=EROFS;
	else {mutex_lock(&ms->snapshot_lock);error=ufs_snapshot_read(&ms->snapshot,
		bio->b_mapped_block,bio->b_block_count,bio->b_data);
		mutex_unlock(&ms->snapshot_lock);}
	bio_complete(bio,error,error==0&&bio->b_op==BIO_READ?
	    (size_t)bio->b_block_count*UFS2_SECTOR_SIZE:0);return 0;
}
static const struct disk_ops snapshot_disk_ops={.submit=snapshot_disk_submit};
static unsigned snapshot_disk_sequence;

static int
snapshot_disk_publish(struct ufs2_mount_state *ms)
{
	unsigned attempt;
	if(ms->snapshot_disk!=NULL)return 0;
	for(attempt=0;attempt<DISK_MAX;attempt++) {
		struct disk *disk=disk_alloc();unsigned number=snapshot_disk_sequence++;
		int error;
		if(disk==NULL)return ENOSPC;
		memcpy(disk->d_name,"ufssnap",7);
		if(number>=100U)number%=100U;
		if(number>=10U){disk->d_name[7]=(char)('0'+number/10U);
			disk->d_name[8]=(char)('0'+number%10U);disk->d_name[9]='\0';}
		else {disk->d_name[7]=(char)('0'+number);disk->d_name[8]='\0';}
		disk->d_flags=DISK_READ_ONLY;disk->d_block_size=UFS2_SECTOR_SIZE;
		disk->d_block_count=ms->snapshot.volume_sectors;
		disk->d_max_transfer_blocks=128;disk->d_ops=&snapshot_disk_ops;
		disk->d_data=ms;error=disk_create(disk);
		if(error==0){ms->snapshot_disk=disk;return 0;}
		(void)disk_destroy(disk);if(error!=EEXIST)return error;
	}
	return ENOSPC;
}

static int
snapshot_disk_remove(struct ufs2_mount_state *ms)
{
	struct disk *disk=ms->snapshot_disk;int error;
	if(disk==NULL)return 0;
	error=disk_gone_if_idle(disk);if(error!=0)return error;
	error=disk_destroy(disk);if(error==0)ms->snapshot_disk=NULL;
	return error;
}

static void
ufs2_state_free(struct ufs2_mount_state *ms)
{
	if(ms==NULL)return;
	kern_free(ms->snapshot_map);kern_free(ms->cg);kern_free(ms);
}

static int
ufs2_quota_persist(struct mount *mountp)
{
	struct ufs2_mount_state *ms=state(mountp);uint8_t *buffer;size_t length;
	int error;
	if(!ms->writable||mountp->m_root==NULL)return EROFS;
	buffer=kern_malloc(ms->super.bsize);if(buffer==NULL)return ENOMEM;
	error=quota_export_config(&ms->quota,buffer,ms->super.bsize,&length);
	if(error==0)error=ufs2_setxattr(mountp->m_root,UFS2_QUOTA_XATTR,
	    buffer,length,0);
	if(error==0)error=disk_sync(mountp->m_disk);
	kern_free(buffer);return error;
}

static int ufs2_mount_impl(struct mount *mountp)
{
	struct ufs2_mount_state *ms; struct inode *root; int error;
	uint64_t total_ndir=0,total_nbfree=0,total_nifree=0,total_nffree=0;
	uint32_t cg;
	int summaries_rebuilt=0;
	off_t cursor=0;
	uint32_t number;
	uint8_t type;
	char name[NAME_MAX+1U];
	if(mountp==NULL||mountp->m_disk==NULL)return EINVAL;
	ms=kern_calloc(1,sizeof(*ms)); if(ms==NULL)return ENOMEM;
	error=ufs2_read_super(mountp->m_disk,&ms->super); if(error){kern_free(ms);return error;}
	mountp->m_data=ms;
	(void)mutex_init(&ms->journal_lock,LOCK_RANK_DEVICE,"ufs2 journal");
	(void)mutex_init(&ms->snapshot_lock,LOCK_RANK_DEVICE,"ufs2 snapshot");
	error=journal_discover(mountp,ms);
	if(error!=0){mountp->m_data=NULL;kern_free(ms);return error;}
	error=snapshot_discover(mountp,ms);
	if(error!=0){mountp->m_data=NULL;ufs2_state_free(ms);return error;}
	(void)mutex_init(&ms->namespace_lock,LOCK_RANK_NAMESPACE,
		"ufs2 namespace");
	(void)mutex_init(&ms->lock,LOCK_RANK_INODE,"ufs2 mount");
	quota_state_init(&ms->quota);
	ms->cg=kern_malloc(ms->super.bsize);if(ms->cg==NULL){mountp->m_data=NULL;kern_free(ms);return ENOMEM;}
	for(cg=0;cg<ms->super.ncg;cg++) {
		uint8_t *free_map;
		uint32_t fragment,ndblk;
		error=load_cg_locked(mountp,cg);
		if(error!=0)break;
		ndblk=cg_ndblk(&ms->super,cg);
		free_map=ms->cg+ms->cg_freeoff;
		for(fragment=0;fragment<ms->super.dblkno&&fragment<ndblk;fragment++)
			if(bit_test(free_map,fragment)){error=EINVAL;break;}
		for(fragment=ndblk;error==0&&fragment<ms->super.fpg;fragment++)
			if(bit_test(free_map,fragment)){error=EINVAL;break;}
		if(error!=0)break;
		if(cg==0&&!bit_test(ms->cg+ms->cg_iusedoff,UFS2_ROOT_INO))
			{error=EINVAL;break;}
		total_ndir+=ufs1_get32(ms->cg,UFS2_CG_NDIR,ms->super.swapped);
		total_nbfree+=ufs1_get32(ms->cg,UFS2_CG_NBFREE,ms->super.swapped);
		total_nifree+=ufs1_get32(ms->cg,UFS2_CG_NIFREE,ms->super.swapped);
		total_nffree+=ufs1_get32(ms->cg,UFS2_CG_NFFREE,ms->super.swapped);
	}
	if(error==0&&(total_ndir!=ms->super.cstotal_ndir||
	    total_nbfree!=ms->super.cstotal_nbfree||
	    total_nifree!=ms->super.cstotal_nifree||
	    total_nffree!=ms->super.cstotal_nffree)){
		if(!ms->journal_enabled)error=EINVAL;
		else {ms->super.cstotal_ndir=total_ndir;
			ms->super.cstotal_nbfree=total_nbfree;
			ms->super.cstotal_nifree=total_nifree;
			ms->super.cstotal_nffree=total_nffree;summaries_rebuilt=1;}
	}
	if(error==0)error=ufs2_quota_rebuild(mountp);
	if(error==0)error=load_cg_locked(mountp,0);
	if(error!=0){mountp->m_data=NULL;ufs2_state_free(ms);return error;}
	if((mountp->m_flags&MOUNT_READ_ONLY)==0){if((mountp->m_disk->d_flags&DISK_READ_ONLY)!=0||(ms->super.clean==0&&!ms->journal_enabled)){mountp->m_data=NULL;ufs2_state_free(ms);return EROFS;}ms->writable=1;if(summaries_rebuilt){error=write_super_summaries(mountp);if(error!=0){mountp->m_data=NULL;ufs2_state_free(ms);return error;}}}
	error=load_inode(mountp,UFS2_ROOT_INO,&root);
	if(error||root->i_type!=INODE_DIR){if(!error){root->i_flags|=INODE_DEAD;inode_release(root);}mountp->m_data=NULL;ufs2_state_free(ms);return error?error:EIO;}
	/* A malformed root must not become the namespace anchor.  Validate the
	 * mandatory entries while the mount is still private and unpublished. */
	error=next_dirent(root,&cursor,&number,&type,name);
	if(error==0&&(number!=UFS2_ROOT_INO||strcmp(name,".")!=0))error=EIO;
	if(error==0)error=next_dirent(root,&cursor,&number,&type,name);
	if(error==0&&(number!=UFS2_ROOT_INO||strcmp(name,"..")!=0))error=EIO;
	if(error==0)error=ufs2_quota_load(mountp,root);
	if(error!=0){root->i_flags|=INODE_DEAD;inode_release(root);mountp->m_data=NULL;ufs2_state_free(ms);return error;}
	/* Do not dirty an image until every read-only mount validation, including
	 * the root inode, has succeeded. */
	if(ms->writable){error=ufs2_write_clean(mountp,0);if(error){root->i_flags|=INODE_DEAD;inode_release(root);mountp->m_data=NULL;ufs2_state_free(ms);return error;}}
	root->i_flags|=INODE_ROOT; mountp->m_root=root;
	if(ms->snapshot.active&&(error=snapshot_disk_publish(ms))!=0){mountp->m_root=NULL;
		root->i_flags|=INODE_DEAD;inode_release(root);mountp->m_data=NULL;
		ufs2_state_free(ms);return error;}
	return 0;
}
static int ufs2_sync(struct mount *mountp) { return mountp==NULL?EINVAL:disk_sync(mountp->m_disk); }
static int ufs2_statvfs(struct mount *mountp,struct statvfs *result)
{
	struct ufs2_mount_state *ms=state(mountp);uint64_t nbfree,nffree,nifree;
	if(ms==NULL||result==NULL)return EINVAL;
	mutex_lock(&ms->lock);
	nbfree=ms->super.cstotal_nbfree;nffree=ms->super.cstotal_nffree;
	nifree=ms->super.cstotal_nifree;
	memset(result,0,sizeof(*result));
	result->f_bsize=ms->super.bsize;result->f_frsize=ms->super.fsize;
	result->f_blocks=ms->super.dsize;
	result->f_bfree=(uint64_t)nbfree*ms->super.frag+nffree;
	result->f_bavail=result->f_bfree;
	result->f_files=(uint64_t)ms->super.ncg*ms->super.ipg;
	result->f_ffree=nifree;result->f_favail=nifree;
	result->f_namemax=NAME_MAX;
	mutex_unlock(&ms->lock);return 0;
}
static int
ufs2_quotactl(struct mount *mountp,struct quota_control *request)
{
	struct ufs2_mount_state *ms=state(mountp);
	struct quota_record record;
	enum quota_type type;
	uint8_t *saved=NULL;size_t saved_length=0;
	int enabled,error,mutating=0;
	if(ms==NULL||request==NULL||request->type>ZEDBSD_QUOTA_GROUP)
		return EINVAL;
	type=request->type==ZEDBSD_QUOTA_USER?QUOTA_USER:QUOTA_GROUP;
	switch(request->command) {
	case ZEDBSD_QUOTA_GET:
		error=quota_get(&ms->quota,type,request->id,&record);
		if(error!=0)return error;
		error=quota_enabled(&ms->quota,type,&enabled);
		if(error!=0)return error;
		request->flags=enabled?ZEDBSD_QUOTA_F_ENABLED:0;
		request->block_soft=record.block_soft;
		request->block_hard=record.block_hard;
		request->inode_soft=record.inode_soft;
		request->inode_hard=record.inode_hard;
		request->blocks=record.blocks;request->inodes=record.inodes;
		request->block_deadline=record.block_deadline;
		request->inode_deadline=record.inode_deadline;
		return quota_get_grace(&ms->quota,&request->grace_seconds);
	case ZEDBSD_QUOTA_SET:
		if(!ms->writable)return EROFS;
		mutating=1;
		break;
	case ZEDBSD_QUOTA_ENABLE:
	case ZEDBSD_QUOTA_DISABLE:
		if(!ms->writable)return EROFS;
		mutating=1;
		break;
	case ZEDBSD_QUOTA_SYNC:
		return !ms->writable?disk_sync(mountp->m_disk):ufs2_quota_persist(mountp);
	default:
		return EINVAL;
	}
	if(mutating) {
		saved=kern_malloc(ms->super.bsize);if(saved==NULL)return ENOMEM;
		error=quota_export_config(&ms->quota,saved,ms->super.bsize,
		    &saved_length);
		if(error!=0){kern_free(saved);return error;}
	}
	switch(request->command) {
	case ZEDBSD_QUOTA_SET:
		memset(&record,0,sizeof(record));record.id=request->id;
		record.block_soft=request->block_soft;
		record.block_hard=request->block_hard;
		record.inode_soft=request->inode_soft;
		record.inode_hard=request->inode_hard;
		error=quota_set(&ms->quota,type,&record);
		if(error==0&&request->grace_seconds!=0)
			error=quota_set_grace(&ms->quota,request->grace_seconds);
		break;
	case ZEDBSD_QUOTA_ENABLE:
		error=quota_enable(&ms->quota,type,1);break;
	case ZEDBSD_QUOTA_DISABLE:
		error=quota_enable(&ms->quota,type,0);break;
	default:
		error=EINVAL;break;
	}
	if(error==0)error=ufs2_quota_persist(mountp);
	if(error!=0&&(quota_import_config(&ms->quota,saved,saved_length)!=0))
		ms->writable=0;
	kern_free(saved);return error;
}
static int
ufs2_snapshotctl(struct mount *mountp,struct snapshot_control *request)
{
	struct ufs2_mount_state *ms=state(mountp);int error=0;
	if(ms==NULL||request==NULL)return EINVAL;
	memset(request->device,0,sizeof(request->device));
	if(!ms->snapshot_available)return EOPNOTSUPP;
	switch(request->command) {
	case ZEDBSD_SNAPSHOT_CREATE:
		if(!ms->writable)return EROFS;
		error=disk_sync(mountp->m_disk);if(error!=0)return error;
		mutex_lock(&ms->snapshot_lock);error=ufs_snapshot_create(&ms->snapshot);
		mutex_unlock(&ms->snapshot_lock);
		if(error==0)error=snapshot_disk_publish(ms);
		if(error!=0&&ms->snapshot.active){mutex_lock(&ms->snapshot_lock);
			(void)ufs_snapshot_delete(&ms->snapshot);
			mutex_unlock(&ms->snapshot_lock);}
		break;
	case ZEDBSD_SNAPSHOT_DELETE:
		if(!ms->writable)return EROFS;
		if(!ms->snapshot.active)return ENOENT;
		error=snapshot_disk_remove(ms);if(error!=0)return error;
		mutex_lock(&ms->snapshot_lock);error=ufs_snapshot_delete(&ms->snapshot);
		mutex_unlock(&ms->snapshot_lock);
		if(error!=0)(void)snapshot_disk_publish(ms);
		break;
	case ZEDBSD_SNAPSHOT_STATUS:
		break;
	default:
		return EINVAL;
	}
	if(error!=0)return error;
	request->flags=ms->snapshot.active?ZEDBSD_SNAPSHOT_F_ACTIVE:0;
	request->captured_sectors=ms->snapshot.next_record;
	request->capacity_sectors=ms->snapshot.max_records;
	if(ms->snapshot_disk!=NULL)
		memcpy(request->device,ms->snapshot_disk->d_name,
		    sizeof(request->device));
	return 0;
}
static int ufs2_prepare_unmount(struct mount *mountp) { struct ufs2_mount_state *ms=state(mountp);if(ms!=NULL&&ms->snapshot_disk!=NULL)return EBUSY;return ms!=NULL&&ms->writable?ufs2_write_clean(mountp,1):0; }
static void ufs2_unmount(struct mount *mountp) { if(mountp&&mountp->m_data){struct ufs2_mount_state *ms=state(mountp);ufs2_state_free(ms);mountp->m_data=NULL;} }

const struct filesystem_type ufs2_filesystem_type={
	.fs_name="ufs2",.probe=ufs2_probe,.mount=ufs2_mount_impl,.sync=ufs2_sync,
	.statvfs=ufs2_statvfs,.quotactl=ufs2_quotactl,
	.snapshotctl=ufs2_snapshotctl,
	.prepare_unmount=ufs2_prepare_unmount,
	.unmount=ufs2_unmount,.alloc_inode=ufs2_alloc_inode,.free_inode=ufs2_free_inode,
};
