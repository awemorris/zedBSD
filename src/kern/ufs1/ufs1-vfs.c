/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Conservative 4.4BSD-derived UFS1 VFS implementation. */
#include "kern/ufs1.h"
#include "kern/ufs1/ufs1-disk.h"
#include "kern/ufs1/ufs1-endian.h"
#include "kern/ufs1/ufs1-super.h"
#include "kern/disk.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/mount.h"
#include "kern/namei.h"
#include "kern/pipe.h"
#include "kern/test-fault.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/statvfs.h>

#define UFS1_IFMT 0170000U
#define UFS1_IFIFO 0010000U
#define UFS1_IFCHR 0020000U
#define UFS1_IFDIR 0040000U
#define UFS1_IFBLK 0060000U
#define UFS1_IFREG 0100000U
#define UFS1_IFLNK 0120000U
#define UFS1_IFSOCK 0140000U

struct ufs1_mount_state {
	struct ufs1_super super;
	struct mutex namespace_lock;
	struct mutex lock;
	uint8_t *cg;
	uint32_t cg_iusedoff;
	uint32_t cg_freeoff;
	uint32_t cg_nextfreeoff;
	uint32_t active_cg;
	uint32_t rotor_cg;
	int writable;
};

struct ufs1_inode_info {
	struct inode inode;
	uint32_t direct[UFS1_NDADDR];
	uint32_t indirect[UFS1_NIADDR];
	uint32_t disk_flags;
	uint32_t blocks;
	uint32_t generation;
	uint8_t shortlink[60];
};

static const struct inode_ops ufs1_inode_ops;
static const struct file_ops ufs1_regular_ops;
static const struct file_ops ufs1_directory_ops;
static int ufs1_lookup(struct inode *,const struct componentname *,
	struct inode **);
static int persist_inode(struct inode *);

static struct ufs1_mount_state *state(const struct mount *mountp)
{ return mountp != NULL ? mountp->m_data : NULL; }
static struct ufs1_inode_info *info(const struct inode *inode)
{ return (struct ufs1_inode_info *)(uintptr_t)inode; }

static int
read_block(struct mount *mountp, uint32_t fragment, void *buffer)
{
	const struct ufs1_super *s = &state(mountp)->super;
	if (fragment == 0) { memset(buffer, 0, s->bsize); return 0; }
	if (fragment >= s->size || s->frag > s->size - fragment)
		return EIO;
	return disk_read(mountp->m_disk, (uint64_t)fragment << s->fsbtodb,
	    s->bsize / UFS1_SECTOR_SIZE, buffer);
}

static int
write_block(struct mount *mountp, uint32_t fragment, const void *buffer)
{
	const struct ufs1_super *s = &state(mountp)->super;
	if (fragment == 0 || fragment >= s->size || s->frag > s->size-fragment)
		return EIO;
	return disk_write(mountp->m_disk,(uint64_t)fragment<<s->fsbtodb,
	    s->bsize/UFS1_SECTOR_SIZE,buffer);
}

static int bit_test(const uint8_t *map,uint32_t bit)
{ return (map[bit>>3]&(uint8_t)(1U<<(bit&7U)))!=0; }
static void bit_set(uint8_t *map,uint32_t bit)
{ map[bit>>3]|=(uint8_t)(1U<<(bit&7U)); }
static void bit_clear(uint8_t *map,uint32_t bit)
{ map[bit>>3]&=(uint8_t)~(1U<<(bit&7U)); }

static uint64_t
cgstart(const struct ufs1_super *super, uint32_t cg)
{
	return (uint64_t)cg * super->fpg +
	    (uint64_t)super->cgoffset * (cg & ~super->cgmask);
}

static uint32_t
cg_ndblk(const struct ufs1_super *super, uint32_t cg)
{
	uint64_t start = cgstart(super, cg);
	uint64_t remaining = start < super->size ? super->size - start : 0;
	return remaining > super->fpg ? super->fpg : (uint32_t)remaining;
}

static int
load_cg_locked(struct mount *mountp, uint32_t cg)
{
	struct ufs1_mount_state *ms = state(mountp);
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
	    ms->super.bsize / UFS1_SECTOR_SIZE, ms->cg);
	if (error != 0)
		return error;
	ndblk = cg_ndblk(&ms->super, cg);
	inode_map_bytes = (ms->super.ipg + 7U) / 8U;
	free_map_bytes = (ms->super.fpg + 7U) / 8U;
	ndir = ufs1_get32(ms->cg, UFS1_CG_NDIR, ms->super.swapped);
	nbfree = ufs1_get32(ms->cg, UFS1_CG_NBFREE, ms->super.swapped);
	nifree = ufs1_get32(ms->cg, UFS1_CG_NIFREE, ms->super.swapped);
	nffree = ufs1_get32(ms->cg, UFS1_CG_NFFREE, ms->super.swapped);
	ms->cg_iusedoff = ufs1_get32(ms->cg, UFS1_CG_IUSEDOFF,
	    ms->super.swapped);
	ms->cg_freeoff = ufs1_get32(ms->cg, UFS1_CG_FREEOFF,
	    ms->super.swapped);
	ms->cg_nextfreeoff = ufs1_get32(ms->cg, UFS1_CG_NEXTFREEOFF,
	    ms->super.swapped);
	if (ufs1_get32(ms->cg, UFS1_CG_MAGIC, ms->super.swapped) !=
	    UFS1_CG_MAGIC_VALUE ||
	    ufs1_get32(ms->cg, UFS1_CG_CGX, ms->super.swapped) != cg ||
	    ufs1_get32(ms->cg, UFS1_CG_NDBLK, ms->super.swapped) != ndblk ||
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
valid_inode_fragment(const struct ufs1_super *super, uint32_t fragment)
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
	struct ufs1_mount_state *ms=state(mountp);
	uint8_t *buffer=kern_malloc(UFS1_SBLOCK_SIZE);
	int error;
	if(buffer==NULL)return ENOMEM;
	error=disk_read(mountp->m_disk,UFS1_SBLOCK_OFFSET/UFS1_SECTOR_SIZE,
	    UFS1_SBLOCK_SIZE/UFS1_SECTOR_SIZE,buffer);
	if(error==0) {
		ufs1_put32(buffer,UFS1_FS_CSTOTAL_NDIR,
		    ms->super.cstotal_ndir,
		    ms->super.swapped);
		ufs1_put32(buffer,UFS1_FS_CSTOTAL_NBFREE,
		    ms->super.cstotal_nbfree,
		    ms->super.swapped);
		ufs1_put32(buffer,UFS1_FS_CSTOTAL_NIFREE,
		    ms->super.cstotal_nifree,
		    ms->super.swapped);
		ufs1_put32(buffer,UFS1_FS_CSTOTAL_NFFREE,
		    ms->super.cstotal_nffree,
		    ms->super.swapped);
		error=disk_write(mountp->m_disk,
		    UFS1_SBLOCK_OFFSET/UFS1_SECTOR_SIZE,
		    UFS1_SBLOCK_SIZE/UFS1_SECTOR_SIZE,buffer);
	}
	kern_free(buffer);
	return error;
}

static int
write_cg(struct mount *mountp)
{
	struct ufs1_mount_state *ms=state(mountp);
	struct kern_test_fault_result fault;
	if(KERN_TEST_FAULT(KERN_TEST_FAULT_UFS_CG_WRITE,UINT32_MAX,
	    UINT32_MAX,&fault))return fault.error!=0?fault.error:EIO;
	int error=disk_write(mountp->m_disk,
	    (cgstart(&ms->super,ms->active_cg)+ms->super.cblkno)<<
	    ms->super.fsbtodb,
	    ms->super.bsize/UFS1_SECTOR_SIZE,ms->cg);
	return error!=0?error:write_super_summaries(mountp);
}

/* Caller holds ms->lock and has already restored the in-memory CG image. */
static int
write_cg_rollback(struct mount *mountp, int original_error)
{
	struct ufs1_mount_state *ms = state(mountp);
	if (write_cg(mountp) != 0)
		ms->writable = 0;
	return original_error;
}

static int
adjust_directory_count(struct mount *mountp,uint32_t ino,int delta)
{
	struct ufs1_mount_state *ms=state(mountp);
	uint32_t count,old_total,cg=ino/ms->super.ipg;
	int error;
	mutex_lock(&ms->lock);
	error=load_cg_locked(mountp,cg);
	if(error!=0){mutex_unlock(&ms->lock);return error;}
	count=ufs1_get32(ms->cg,UFS1_CG_NDIR,ms->super.swapped);
	old_total=ms->super.cstotal_ndir;
	if((delta<0&&count==0)||(delta>0&&count==UINT32_MAX))
		error=EIO;
	else {
		ufs1_put32(ms->cg,UFS1_CG_NDIR,
		    delta<0?count-1U:count+1U,ms->super.swapped);
		ms->super.cstotal_ndir=delta<0?old_total-1U:old_total+1U;
		error=write_cg(mountp);
		if(error!=0) {
			ufs1_put32(ms->cg,UFS1_CG_NDIR,count,
			    ms->super.swapped);
			ms->super.cstotal_ndir=old_total;
			error=write_cg_rollback(mountp,error);
		}
	}
	mutex_unlock(&ms->lock);
	return error;
}

static int
allocate_block(struct mount *mountp,uint32_t *result)
{
	struct ufs1_mount_state *ms=state(mountp); uint8_t *map;
	uint32_t fragment,n,cg,attempt,old_total; int error=ENOSPC;
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
			uint32_t free=ufs1_get32(ms->cg,UFS1_CG_NBFREE,ms->super.swapped);
			if(free==0){for(n=0;n<ms->super.frag;n++)bit_set(map,fragment+n);break;}
			ufs1_put32(ms->cg,UFS1_CG_NBFREE,free-1U,ms->super.swapped);
			ms->super.cstotal_nbfree=old_total-1U;
		}
		error=write_cg(mountp);
		if(error==0){uint8_t *zero=kern_calloc(1,ms->super.bsize);uint32_t absolute=(uint32_t)(cgstart(&ms->super,cg)+fragment);if(zero==NULL)error=ENOMEM;else{error=write_block(mountp,absolute,zero);kern_free(zero);}}
		if(error!=0){for(n=0;n<ms->super.frag;n++)bit_set(map,fragment+n);ufs1_put32(ms->cg,UFS1_CG_NBFREE,ufs1_get32(ms->cg,UFS1_CG_NBFREE,ms->super.swapped)+1U,ms->super.swapped);ms->super.cstotal_nbfree=old_total;error=write_cg_rollback(mountp,error);}else {*result=(uint32_t)(cgstart(&ms->super,cg)+fragment);ms->rotor_cg=cg;}
		break;
	}
		if(error!=ENOSPC)break;
	}
	mutex_unlock(&ms->lock); return error;
}

static int
free_block(struct mount *mountp,uint32_t fragment)
{
	struct ufs1_mount_state *ms=state(mountp);uint8_t *map;uint32_t n,free,cg,local=0,old_total;int error;
	for(cg=0;cg<ms->super.ncg;cg++){uint64_t start=cgstart(&ms->super,cg);uint32_t ndblk=cg_ndblk(&ms->super,cg);if(fragment>=start+ms->super.dblkno&&fragment+ms->super.frag<=start+ndblk){local=(uint32_t)(fragment-start);break;}}
	if(cg==ms->super.ncg)return EIO;
	mutex_lock(&ms->lock);error=load_cg_locked(mountp,cg);if(error!=0){mutex_unlock(&ms->lock);return error;}map=ms->cg+ms->cg_freeoff;
	for(n=0;n<ms->super.frag;n++)if(bit_test(map,local+n)){mutex_unlock(&ms->lock);return EIO;}
	free=ufs1_get32(ms->cg,UFS1_CG_NBFREE,ms->super.swapped);
	if(free==UINT32_MAX){mutex_unlock(&ms->lock);return EIO;}
	old_total=ms->super.cstotal_nbfree;
	for(n=0;n<ms->super.frag;n++)bit_set(map,local+n);
	ufs1_put32(ms->cg,UFS1_CG_NBFREE,free+1U,ms->super.swapped);
	ms->super.cstotal_nbfree=old_total+1U;
	error=write_cg(mountp);
	if(error!=0){for(n=0;n<ms->super.frag;n++)bit_clear(map,local+n);ufs1_put32(ms->cg,UFS1_CG_NBFREE,free,ms->super.swapped);ms->super.cstotal_nbfree=old_total;error=write_cg_rollback(mountp,error);}
	mutex_unlock(&ms->lock);return error;
}

static int
allocate_inode_number(struct mount *mountp,uint32_t *number)
{
	struct ufs1_mount_state *ms=state(mountp);uint8_t *map;uint32_t ino,cg,attempt,old_total;int error=ENOSPC;
	mutex_lock(&ms->lock);
	for(attempt=0;attempt<ms->super.ncg;attempt++){cg=(ms->rotor_cg+attempt)%ms->super.ncg;error=load_cg_locked(mountp,cg);if(error!=0)break;error=ENOSPC;map=ms->cg+ms->cg_iusedoff;
	for(ino=cg==0?UFS1_ROOT_INO+1U:0U;ino<ms->super.ipg;ino++)if(!bit_test(map,ino)){
		uint32_t free=ufs1_get32(ms->cg,UFS1_CG_NIFREE,ms->super.swapped);
		if(free==0)
			break;
		old_total=ms->super.cstotal_nifree;
		bit_set(map,ino);
		ufs1_put32(ms->cg,UFS1_CG_NIFREE,free-1U,ms->super.swapped);
		ms->super.cstotal_nifree=old_total-1U;
		error=write_cg(mountp);if(error!=0){bit_clear(map,ino);ufs1_put32(ms->cg,UFS1_CG_NIFREE,free,ms->super.swapped);ms->super.cstotal_nifree=old_total;error=write_cg_rollback(mountp,error);}else {*number=cg*ms->super.ipg+ino;ms->rotor_cg=cg;}break;
	}
		if(error!=ENOSPC)break;
	}
	mutex_unlock(&ms->lock);return error;
}

static int
free_inode_number(struct mount *mountp,uint32_t number)
{
	struct ufs1_mount_state *ms=state(mountp);uint8_t *map;uint32_t free,cg=number/ms->super.ipg,local=number%ms->super.ipg,old_total;int error;
	if(number<=UFS1_ROOT_INO||cg>=ms->super.ncg)return EIO;
	mutex_lock(&ms->lock);error=load_cg_locked(mountp,cg);if(error!=0){mutex_unlock(&ms->lock);return error;}map=ms->cg+ms->cg_iusedoff;
	if(!bit_test(map,local)){mutex_unlock(&ms->lock);return EIO;}
	free=ufs1_get32(ms->cg,UFS1_CG_NIFREE,ms->super.swapped);
	if(free==UINT32_MAX){mutex_unlock(&ms->lock);return EIO;}
	old_total=ms->super.cstotal_nifree;
	bit_clear(map,local);ufs1_put32(ms->cg,UFS1_CG_NIFREE,free+1U,ms->super.swapped);ms->super.cstotal_nifree=old_total+1U;
	error=write_cg(mountp);
	if(error!=0){bit_set(map,local);ufs1_put32(ms->cg,UFS1_CG_NIFREE,free,ms->super.swapped);ms->super.cstotal_nifree=old_total;error=write_cg_rollback(mountp,error);}
	mutex_unlock(&ms->lock);return error;
}

static int
indirect_entry(struct mount *mountp, uint32_t fragment, uint32_t index,
	uint32_t *result)
{
	const struct ufs1_super *s = &state(mountp)->super;
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
		*result = ufs1_get32(block, (size_t)index * 4U, s->swapped);
	kern_free(block);
	return error;
}

static int
bmap(struct inode *inode, uint64_t logical, uint32_t *result)
{
	struct ufs1_inode_info *ui = info(inode);
	const struct ufs1_super *s = &state(inode->i_mount)->super;
	uint64_t span = s->nindir;
	uint32_t fragment;
	unsigned level, depth;
	int error;
	if (logical < UFS1_NDADDR) { *result = ui->direct[logical]; return 0; }
	logical -= UFS1_NDADDR;
	for (level = 0; level < UFS1_NIADDR; level++) {
		if (logical < span)
			break;
		logical -= span;
		if (span > UINT64_MAX / s->nindir)
			return EOVERFLOW;
		span *= s->nindir;
	}
	if (level == UFS1_NIADDR)
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
bmap_ensure(struct inode *inode,uint64_t logical,uint32_t *result)
{
	struct ufs1_inode_info *ui=info(inode);
	const struct ufs1_super *s=&state(inode->i_mount)->super;
	uint64_t span=s->nindir;
	uint32_t *root,fragment;
	unsigned level,depth;
	int error;

	if(logical<UFS1_NDADDR) {
		if(ui->direct[logical]==0) {
			uint32_t allocated;
			error=allocate_block(inode->i_mount,&allocated);
			if(error!=0)
				return error;
			ui->direct[logical]=allocated;
			ui->blocks+=s->bsize/UFS1_SECTOR_SIZE;
			/* Make the allocation reachable before user data I/O. */
			error=persist_inode(inode);
			if(error!=0) {
				int free_error;
				ui->direct[logical]=0;
				ui->blocks-=s->bsize/UFS1_SECTOR_SIZE;
				free_error=free_block(inode->i_mount,allocated);
				if(free_error!=0) {
					ui->direct[logical]=allocated;
					ui->blocks+=s->bsize/UFS1_SECTOR_SIZE;
					(void)persist_inode(inode);
				}
				return error;
			}
		}
		*result=ui->direct[logical];
		return 0;
	}
	logical-=UFS1_NDADDR;
	for(level=0;level<UFS1_NIADDR;level++) {
		if(logical<span)
			break;
		logical-=span;
		if(span>UINT64_MAX/s->nindir)
			return EOVERFLOW;
		span*=s->nindir;
	}
	if(level==UFS1_NIADDR)
		return EFBIG;
	root=&ui->indirect[level];
	if(*root==0) {
		uint32_t allocated;
		error=allocate_block(inode->i_mount,&allocated);
		if(error!=0)
			return error;
		*root=allocated;
		ui->blocks+=s->bsize/UFS1_SECTOR_SIZE;
		error=persist_inode(inode);
		if(error!=0) {
			int free_error;
			*root=0;
			ui->blocks-=s->bsize/UFS1_SECTOR_SIZE;
			free_error=free_block(inode->i_mount,allocated);
			if(free_error!=0) {
				*root=allocated;
				ui->blocks+=s->bsize/UFS1_SECTOR_SIZE;
				(void)persist_inode(inode);
			}
			return error;
		}
	}
	fragment=*root;
	for(depth=level+1U;depth!=0;depth--) {
		uint8_t *block;
		uint64_t divisor=1;
		uint32_t index,next;
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
		next=ufs1_get32(block,(size_t)index*4U,s->swapped);
		if(next==0) {
			uint32_t allocated=0;
			error=allocate_block(inode->i_mount,&next);
			if(error==0) {
				allocated=next;
				ufs1_put32(block,(size_t)index*4U,next,s->swapped);
				error=write_block(inode->i_mount,fragment,block);
			}
			if(error!=0) {
				if(allocated!=0) {
					int rollback_error;
					/* A short write may have published the pointer even
					 * though write_block() reported EIO.  Make it
					 * unreachable before returning its block. */
					ufs1_put32(block,(size_t)index*4U,0,s->swapped);
					rollback_error=write_block(inode->i_mount,
					    fragment,block);
					if(rollback_error==0)
						rollback_error=free_block(inode->i_mount,
						    allocated);
					if(rollback_error!=0) {
						/* The block may remain reachable.  Never free
						 * uncertain storage or continue writable. */
						ui->blocks+=s->bsize/UFS1_SECTOR_SIZE;
						state(inode->i_mount)->writable=0;
					}
				}
				kern_free(block);
				return error;
			}
			ui->blocks+=s->bsize/UFS1_SECTOR_SIZE;
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
	const struct ufs1_super *s = &state(inode->i_mount)->super;
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
		uint32_t fragment;
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
	struct ufs1_inode_info *ui=info(inode);struct ufs1_mount_state *ms=state(inode->i_mount);
	uint32_t number=(uint32_t)inode->i_ino,cg=number/ms->super.ipg,index=number%ms->super.ipg;
	uint32_t fragment=(uint32_t)(cgstart(&ms->super,cg)+ms->super.iblkno+
	    (index/ms->super.inopb)*ms->super.frag);
	uint8_t *block=kern_malloc(ms->super.bsize),*raw;unsigned n;int error;
	if(block==NULL)
		return ENOMEM;
	error=read_block(inode->i_mount,fragment,block);
	if(error!=0){kern_free(block);return error;}raw=block+(index%ms->super.inopb)*UFS1_DINODE_SIZE;
	ufs1_put16(raw,UFS1_DI_MODE,(uint16_t)inode->i_mode,ms->super.swapped);
	ufs1_put16(raw,UFS1_DI_NLINK,(uint16_t)inode->i_linkcount,ms->super.swapped);
	ufs1_put64(raw,UFS1_DI_SIZE,(uint64_t)inode->i_size,ms->super.swapped);
	ufs1_put32(raw,UFS1_DI_ATIME,(uint32_t)inode->i_atime.tv_sec,
		ms->super.swapped);
	ufs1_put32(raw,UFS1_DI_ATIMENSEC,(uint32_t)inode->i_atime.tv_nsec,
		ms->super.swapped);
	ufs1_put32(raw,UFS1_DI_MTIME,(uint32_t)inode->i_mtime.tv_sec,
		ms->super.swapped);
	ufs1_put32(raw,UFS1_DI_MTIMENSEC,(uint32_t)inode->i_mtime.tv_nsec,
		ms->super.swapped);
	ufs1_put32(raw,UFS1_DI_CTIME,(uint32_t)inode->i_ctime.tv_sec,
		ms->super.swapped);
	ufs1_put32(raw,UFS1_DI_CTIMENSEC,(uint32_t)inode->i_ctime.tv_nsec,
		ms->super.swapped);
	if(inode->i_type==INODE_SYMLINK&&(uint64_t)inode->i_size<=ms->super.maxsymlinklen&&inode->i_size<=60) {
		memset(raw+UFS1_DI_DB,0,60U);
		memcpy(raw+UFS1_DI_DB,ui->shortlink,(size_t)inode->i_size);
	} else {
		for(n=0;n<UFS1_NDADDR;n++)ufs1_put32(raw,UFS1_DI_DB+n*4U,ui->direct[n],ms->super.swapped);
		for(n=0;n<UFS1_NIADDR;n++)ufs1_put32(raw,UFS1_DI_IB+n*4U,ui->indirect[n],ms->super.swapped);
	}
	ufs1_put32(raw,UFS1_DI_BLOCKS,ui->blocks,ms->super.swapped);
	ufs1_put32(raw,UFS1_DI_UID,inode->i_uid,ms->super.swapped);ufs1_put32(raw,UFS1_DI_GID,inode->i_gid,ms->super.swapped);
	error=write_block(inode->i_mount,fragment,block);kern_free(block);return error;
}

static ssize_t
pwrite_inode(struct inode *inode,const void *buffer,size_t length,off_t offset)
{
	struct ufs1_mount_state *ms=state(inode->i_mount);
	uint8_t *scratch;size_t done=0;int final_error=0;
	if(!ms->writable)
		return -EROFS;
	if(offset<0||(uint64_t)offset+length<(uint64_t)offset)
		return -EINVAL;
	if((uint64_t)offset+length>ms->super.maxfilesize)return -EFBIG;
	scratch=kern_malloc(ms->super.bsize);if(scratch==NULL)return -ENOMEM;mutex_lock(&inode->i_lock);
	while(done<length){uint64_t pos=(uint64_t)offset+done,lbn=pos/ms->super.bsize;size_t within=(size_t)(pos%ms->super.bsize),amount=ms->super.bsize-within;uint32_t fragment=0;int error;
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
indirect_span(const struct ufs1_super *super,unsigned depth)
{
	uint64_t span=1;
	while(depth--!=0)
		span*=super->nindir;
	return span;
}

static int
truncate_indirect(struct inode *inode,uint32_t *root,unsigned depth,
	uint64_t base,uint64_t keep)
{
	struct ufs1_mount_state *ms=state(inode->i_mount);
	struct ufs1_inode_info *ui=info(inode);
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
		uint32_t child=ufs1_get32(block,(size_t)index*4U,
			ms->super.swapped);
		uint64_t child_base=base+(uint64_t)index*child_span;
		if(child==0)
			continue;
		if(depth==1U) {
			if(child_base>=keep) {
				error=free_block(inode->i_mount,child);
				if(error!=0) {
					kern_free(block);
					return error;
				}
				ufs1_put32(block,(size_t)index*4U,0,
					ms->super.swapped);
				ui->blocks-=ms->super.bsize/UFS1_SECTOR_SIZE;
				changed=1;
				continue;
			}
		} else if(child_base+child_span>keep) {
			error=truncate_indirect(inode,&child,depth-1U,child_base,keep);
			if(error!=0) {
				kern_free(block);
				return error;
			}
			if(child!=ufs1_get32(block,(size_t)index*4U,
			    ms->super.swapped)) {
				ufs1_put32(block,(size_t)index*4U,child,
					ms->super.swapped);
				changed=1;
			}
		}
		if(ufs1_get32(block,(size_t)index*4U,ms->super.swapped)!=0)
			empty=0;
	}
	if(empty) {
		error=free_block(inode->i_mount,*root);
		if(error==0) {
			*root=0;
			ui->blocks-=ms->super.bsize/UFS1_SECTOR_SIZE;
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
ufs1_truncate(struct inode *inode,off_t size)
{
	struct ufs1_mount_state *ms=state(inode->i_mount);struct ufs1_inode_info *ui=info(inode);uint8_t *block=NULL;uint64_t keep,base;unsigned n;int error=0;
	if(!ms->writable)
		return EROFS;
	if(size<0||(uint64_t)size>ms->super.maxfilesize)
		return EFBIG;
	mutex_lock(&inode->i_lock);keep=((uint64_t)size+ms->super.bsize-1U)/ms->super.bsize;
	if(size<inode->i_size&&size!=0&&(size%ms->super.bsize)!=0){uint32_t fragment=0;error=bmap(inode,(uint64_t)size/ms->super.bsize,&fragment);if(error)goto out;if(fragment!=0){block=kern_malloc(ms->super.bsize);if(block==NULL){error=ENOMEM;goto out;}error=read_block(inode->i_mount,fragment,block);if(error)goto out;memset(block+(size%ms->super.bsize),0,ms->super.bsize-(size%ms->super.bsize));error=write_block(inode->i_mount,fragment,block);if(error)goto out;}}
	for(n=(unsigned)keep;n<UFS1_NDADDR;n++)if(ui->direct[n]!=0){error=free_block(inode->i_mount,ui->direct[n]);if(error)goto out;ui->direct[n]=0;ui->blocks-=ms->super.bsize/UFS1_SECTOR_SIZE;}
	base=UFS1_NDADDR;
	for(n=0;n<UFS1_NIADDR;n++) {
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
	switch (mode & UFS1_IFMT) {
	case UFS1_IFREG: return INODE_REG; case UFS1_IFDIR: return INODE_DIR;
	case UFS1_IFLNK: return INODE_SYMLINK; case UFS1_IFCHR: return INODE_CHAR;
	case UFS1_IFBLK: return INODE_BLOCK; case UFS1_IFIFO: return INODE_FIFO;
	case UFS1_IFSOCK: return INODE_SOCKET; default: return INODE_NONE;
	}
}

static int
load_inode(struct mount *mountp, uint32_t number, struct inode **result)
{
	const struct ufs1_super *s = &state(mountp)->super;
	struct ufs1_inode_info *ui;
	struct inode *inode;
	uint8_t *block, *raw;
	uint32_t cg, index, fragment;
	uint16_t mode;
	unsigned n;
	int error;
	if (number < UFS1_ROOT_INO || number >= s->ncg * s->ipg)
		return EIO;
	if (inode_get(mountp, number, result) == 0)
		return 0;
	cg = number / s->ipg; index = number % s->ipg;
	fragment = (uint32_t)(cgstart(s, cg) + s->iblkno +
	    (index / s->inopb) * s->frag);
	block = kern_malloc(s->bsize);
	if (block == NULL)
		return ENOMEM;
	error = read_block(mountp, fragment, block);
	if (error != 0) { kern_free(block); return error; }
	raw = block + (index % s->inopb) * UFS1_DINODE_SIZE;
	mode = ufs1_get16(raw, UFS1_DI_MODE, s->swapped);
	if (mode_type(mode) == INODE_NONE) { kern_free(block); return EOPNOTSUPP; }
	inode = inode_alloc(mountp);
	if (inode == NULL) { kern_free(block); return ENOSPC; }
	ui = info(inode);
	inode->i_type = mode_type(mode); inode->i_ino = number;
	inode->i_mode = mode; inode->i_linkcount = ufs1_get16(raw,UFS1_DI_NLINK,s->swapped);
	inode->i_size = (off_t)ufs1_get64(raw,UFS1_DI_SIZE,s->swapped);
	inode->i_uid = ufs1_get32(raw,UFS1_DI_UID,s->swapped);
	inode->i_gid = ufs1_get32(raw,UFS1_DI_GID,s->swapped);
	inode->i_atime.tv_sec = (int32_t)ufs1_get32(raw,UFS1_DI_ATIME,s->swapped);
	inode->i_atime.tv_nsec = ufs1_get32(raw,UFS1_DI_ATIMENSEC,s->swapped);
	inode->i_mtime.tv_sec = (int32_t)ufs1_get32(raw,UFS1_DI_MTIME,s->swapped);
	inode->i_mtime.tv_nsec = ufs1_get32(raw,UFS1_DI_MTIMENSEC,s->swapped);
	inode->i_ctime.tv_sec = (int32_t)ufs1_get32(raw,UFS1_DI_CTIME,s->swapped);
	inode->i_ctime.tv_nsec = ufs1_get32(raw,UFS1_DI_CTIMENSEC,s->swapped);
	if(inode->i_type==INODE_SYMLINK&&(uint64_t)inode->i_size<=
	    s->maxsymlinklen&&inode->i_size<=60) {
		memcpy(ui->shortlink,raw+UFS1_DI_DB,sizeof(ui->shortlink));
	} else {
		for (n=0;n<UFS1_NDADDR;n++)
			ui->direct[n]=ufs1_get32(raw,UFS1_DI_DB+n*4U,s->swapped);
		for (n=0;n<UFS1_NIADDR;n++)
			ui->indirect[n]=ufs1_get32(raw,UFS1_DI_IB+n*4U,s->swapped);
	}
	ui->disk_flags=ufs1_get32(raw,UFS1_DI_FLAGS,s->swapped);
	ui->blocks=ufs1_get32(raw,UFS1_DI_BLOCKS,s->swapped);
	ui->generation=ufs1_get32(raw,UFS1_DI_GEN,s->swapped);
	if (inode->i_linkcount == 0 || inode->i_size < 0 ||
	    (uint64_t)inode->i_size > s->maxfilesize ||
	    inode->i_atime.tv_nsec >= 1000000000L ||
	    inode->i_mtime.tv_nsec >= 1000000000L ||
	    inode->i_ctime.tv_nsec >= 1000000000L ||
	    (inode->i_type == INODE_DIR &&
	    ((uint64_t)inode->i_size < UFS1_DIRBLKSIZ ||
	    (uint64_t)inode->i_size % UFS1_DIRBLKSIZ != 0))) {
		inode->i_flags |= INODE_DEAD;
		inode_release(inode);
		kern_free(block);
		return EIO;
	}
	if (!(inode->i_type == INODE_SYMLINK &&
	    (uint64_t)inode->i_size <= s->maxsymlinklen &&
	    inode->i_size <= 60)) {
		for (n = 0; n < UFS1_NDADDR; n++)
			if (!valid_inode_fragment(s, ui->direct[n])) {
				inode->i_flags |= INODE_DEAD;
				inode_release(inode);
				kern_free(block);
				return EIO;
			}
		for (n = 0; n < UFS1_NIADDR; n++)
			if (!valid_inode_fragment(s, ui->indirect[n])) {
				inode->i_flags |= INODE_DEAD;
				inode_release(inode);
				kern_free(block);
				return EIO;
			}
	}
	inode->i_op=&ufs1_inode_ops;
	inode->i_fop=inode->i_type==INODE_DIR?&ufs1_directory_ops:
		inode->i_type==INODE_REG?&ufs1_regular_ops:
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
		if ((uint64_t)*cursor % UFS1_DIRBLKSIZ > UFS1_DIRBLKSIZ-8U) return EIO;
		count=pread_inode(directory,head,sizeof(head),*cursor);
		if (count!=sizeof(head)) return EIO;
		*number=ufs1_get32(head,0,state(directory->i_mount)->super.swapped);
		reclen=ufs1_get16(head,4,state(directory->i_mount)->super.swapped);
		*type=head[6]; namelen=head[7];
		if (reclen<8U || (reclen&3U)!=0 ||
		    8U+namelen>reclen || (uint64_t)*cursor%UFS1_DIRBLKSIZ+reclen>UFS1_DIRBLKSIZ ||
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
restore_directory_block(struct inode *directory, uint32_t fragment,
	const uint8_t *original, int original_error)
{
	struct ufs1_mount_state *ms=state(directory->i_mount);
	if(write_block(directory->i_mount,fragment,original)!=0)
		ms->writable=0;
	return original_error;
}

static int
dir_find_record(struct inode *directory,const struct componentname *name,
	uint8_t *block,uint32_t *offset,uint32_t *previous,uint32_t *number)
{
	struct ufs1_mount_state *ms=state(directory->i_mount);uint32_t pos=0,prev=UINT32_MAX;int error;
	if(directory->i_size<0 || (uint64_t)directory->i_size>ms->super.bsize ||
	    info(directory)->direct[0]==0)
		return EIO;
	error=read_block(directory->i_mount,info(directory)->direct[0],block);if(error)return error;
	while(pos<(uint32_t)directory->i_size){uint32_t ino=ufs1_get32(block,pos,ms->super.swapped);uint16_t reclen=ufs1_get16(block,pos+4U,ms->super.swapped);uint8_t nlen=block[pos+7U];
		if(reclen<8U||(reclen&3U)!=0||pos%UFS1_DIRBLKSIZ+reclen>UFS1_DIRBLKSIZ||pos+reclen>(uint32_t)directory->i_size||8U+nlen>reclen)return EIO;
		if(ino!=0&&nlen==name->cn_namelen&&memcmp(block+pos+8U,name->cn_nameptr,nlen)==0){*offset=pos;*previous=prev;*number=ino;return 0;}
		prev=pos;pos+=reclen;
	}
	return ENOENT;
}

static int
dir_add(struct inode *directory,const struct componentname *name,uint32_t number,uint8_t type)
{
	struct ufs1_mount_state *ms=state(directory->i_mount);struct ufs1_inode_info *ui=info(directory);uint8_t *block,*original;uint16_t need;uint32_t pos=0,old_direct,allocated=0;uint32_t old_blocks;off_t old_size;int error,rollback;
	if(name->cn_namelen==0||name->cn_namelen>255U)
		return EINVAL;
	for(pos=0;pos<name->cn_namelen;pos++)
		if(name->cn_nameptr[pos]=='/')
			return EINVAL;
	pos=0;
	need=dir_minimum((uint8_t)name->cn_namelen);block=kern_calloc(1,ms->super.bsize);original=kern_malloc(ms->super.bsize);if(block==NULL||original==NULL){kern_free(block);kern_free(original);return ENOMEM;}
	mutex_lock(&directory->i_lock);
	old_size=directory->i_size;old_direct=ui->direct[0];old_blocks=ui->blocks;
	if(ui->direct[0]==0){error=allocate_block(directory->i_mount,&ui->direct[0]);if(error)goto out;allocated=ui->direct[0];ui->blocks+=ms->super.bsize/UFS1_SECTOR_SIZE;}
	error=read_block(directory->i_mount,ui->direct[0],block);if(error)goto out;
	memcpy(original,block,ms->super.bsize);
	while(pos<(uint32_t)directory->i_size){uint16_t reclen=ufs1_get16(block,pos+4U,ms->super.swapped);uint8_t nlen=block[pos+7U];uint16_t minimum=dir_minimum(nlen);
		if(reclen<minimum||pos%UFS1_DIRBLKSIZ+reclen>UFS1_DIRBLKSIZ){error=EIO;goto out;}
		if(reclen-minimum>=need){uint32_t at=pos+minimum;ufs1_put16(block,pos+4U,minimum,ms->super.swapped);ufs1_put32(block,at,number,ms->super.swapped);ufs1_put16(block,at+4U,reclen-minimum,ms->super.swapped);block[at+6U]=type;block[at+7U]=(uint8_t)name->cn_namelen;memcpy(block+at+8U,name->cn_nameptr,name->cn_namelen);error=write_block(directory->i_mount,ui->direct[0],block);goto commit;}
		pos+=reclen;
	}
	if((uint64_t)directory->i_size+UFS1_DIRBLKSIZ>ms->super.bsize){error=ENOSPC;goto out;}
	pos=(uint32_t)directory->i_size;ufs1_put32(block,pos,number,ms->super.swapped);ufs1_put16(block,pos+4U,UFS1_DIRBLKSIZ,ms->super.swapped);block[pos+6U]=type;block[pos+7U]=(uint8_t)name->cn_namelen;memcpy(block+pos+8U,name->cn_nameptr,name->cn_namelen);directory->i_size+=UFS1_DIRBLKSIZ;error=write_block(directory->i_mount,ui->direct[0],block);
commit:	if(error==0)error=persist_inode(directory);
	if(error!=0){
		rollback=restore_directory_block(directory,ui->direct[0],original,error);
		directory->i_size=old_size;ui->direct[0]=old_direct;ui->blocks=old_blocks;
		if(persist_inode(directory)!=0)ms->writable=0;
		if(allocated!=0&&free_block(directory->i_mount,allocated)!=0)ms->writable=0;
		error=rollback;
	}
out:	if(error!=0&&allocated!=0&&ui->direct[0]==allocated){
		directory->i_size=old_size;ui->direct[0]=old_direct;ui->blocks=old_blocks;
		if(persist_inode(directory)!=0)ms->writable=0;
		if(free_block(directory->i_mount,allocated)!=0)ms->writable=0;
	}
	mutex_unlock(&directory->i_lock);kern_free(original);kern_free(block);return error;
}

static int
dir_remove(struct inode *directory,const struct componentname *name,uint32_t *number)
{
	struct ufs1_mount_state *ms=state(directory->i_mount);
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
		    previous/UFS1_DIRBLKSIZ==offset/UFS1_DIRBLKSIZ) {
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
	struct ufs1_mount_state *ms=state(directory->i_mount);
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
directory_is_descendant(struct inode *candidate,struct inode *ancestor)
{
	static const struct componentname dotdot={"..",2,0};
	struct inode *current,*parent;
	uint32_t limit=state(candidate->i_mount)->super.ncg*
	    state(candidate->i_mount)->super.ipg,count=0;
	int error=0;

	inode_ref(candidate);
	current=candidate;
	while(count++<limit) {
		if(current->i_ino==ancestor->i_ino) {
			error=EINVAL;
			break;
		}
		if(current->i_ino==UFS1_ROOT_INO)
			break;
		error=ufs1_lookup(current,&dotdot,&parent);
		if(error!=0)
			break;
		if(parent->i_ino==current->i_ino) {
			inode_release(parent);
			break;
		}
		inode_release(current);
		current=parent;
	}
	if(count>limit && error==0)
		error=EIO;
	inode_release(current);
	return error;
}

static int
new_inode(struct mount *mountp,mode_t mode,enum inode_type type,nlink_t links,
	struct inode **result)
{
	struct inode *inode;
	uint32_t number=0;
	int error=allocate_inode_number(mountp,&number);

	if(error)
		return error;
	inode=inode_alloc(mountp);
	if(inode==NULL) {
		(void)free_inode_number(mountp,number);
		return ENOSPC;
	}
	inode->i_ino=number;
	inode->i_mode=mode;
	inode->i_type=type;
	inode->i_linkcount=links;
	inode->i_op=&ufs1_inode_ops;
	inode->i_fop=type==INODE_DIR?&ufs1_directory_ops:
		type==INODE_REG?&ufs1_regular_ops:
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
ufs1_lookup(struct inode *directory,const struct componentname *component,
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
ufs1_create(struct inode *directory,const struct componentname *name,mode_t mode,
	struct inode **result)
{
	struct inode *existing,*inode;
	struct ufs1_mount_state *ms=state(directory->i_mount);
	int error;
	if(!ms->writable)return EROFS;
	mutex_lock(&ms->namespace_lock);
	error=ufs1_lookup(directory,name,&existing);
	if(error==0){inode_release(existing);error=EEXIST;goto out;}
	if(error!=ENOENT)goto out;
	error=new_inode(directory->i_mount,S_IFREG|(mode&07777U),INODE_REG,1,&inode);
	if(error)goto out;
	error=dir_add(directory,name,(uint32_t)inode->i_ino,8);
	if(error){inode->i_linkcount=0;inode->i_flags|=INODE_DEAD;inode_release(inode);goto out;}
	*result=inode;
out:
	mutex_unlock(&ms->namespace_lock);
	return error;
}

static int
ufs1_mkdir(struct inode *directory,const struct componentname *name,mode_t mode,
	struct inode **result)
{
	struct inode *existing,*inode;struct componentname dot={".",1,0},dotdot={"..",2,0};
	struct ufs1_mount_state *ms=state(directory->i_mount);
	int error;
	if(!ms->writable)return EROFS;
	mutex_lock(&ms->namespace_lock);
	error=ufs1_lookup(directory,name,&existing);
	if(error==0){inode_release(existing);error=EEXIST;goto out;}
	if(error!=ENOENT)goto out;
	error=new_inode(directory->i_mount,S_IFDIR|(mode&07777U),INODE_DIR,2,&inode);if(error)goto out;
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
ufs1_mknod(struct inode *directory,const struct componentname *name,
	enum inode_type type,mode_t mode,dev_t rdev,struct inode **result)
{
	struct inode *existing,*inode;
	struct ufs1_mount_state *ms=state(directory->i_mount);
	int error;
	if(type!=INODE_FIFO&&type!=INODE_SOCKET)
		return EOPNOTSUPP;
	if(!ms->writable)
		return EROFS;
	mutex_lock(&ms->namespace_lock);
	error=ufs1_lookup(directory,name,&existing);
	if(error==0){inode_release(existing);error=EEXIST;goto out;}
	if(error!=ENOENT)goto out;
	error=new_inode(directory->i_mount,inode_type_mode(type)|
		(mode&07777U),type,1,&inode);
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
ufs1_unlink(struct inode *directory,const struct componentname *name)
{
	struct ufs1_mount_state *ms=state(directory->i_mount);
	struct inode *target=NULL;uint32_t number=0;int error,rollback_error;
	int removed=0;
	nlink_t old_links=0;
	unsigned old_flags=0;
	mutex_lock(&ms->namespace_lock);
	error=ufs1_lookup(directory,name,&target);
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
ufs1_rmdir(struct inode *directory,const struct componentname *name)
{
	struct ufs1_mount_state *ms=state(directory->i_mount);
	struct inode *target=NULL;uint32_t number=0;int empty,error,rollback_error;
	int removed=0;
	nlink_t old_target_links=0,old_directory_links=0;
	unsigned old_target_flags=0;
	mutex_lock(&ms->namespace_lock);
	error=ufs1_lookup(directory,name,&target);
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
ufs1_rename(struct inode *old_directory,const struct componentname *old_name,
	struct inode *new_directory,const struct componentname *new_name,
	unsigned flags)
{
	struct ufs1_mount_state *ms=state(old_directory->i_mount);
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
	error=ufs1_lookup(old_directory,old_name,&source);
	if(error!=0)
		goto out;
	error=ufs1_lookup(new_directory,new_name,&target);
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
	if(source->i_type==INODE_DIR && old_directory!=new_directory) {
		error=directory_is_descendant(new_directory,source);
		if(error!=0)
			goto out;
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
ufs1_link(struct inode *directory,const struct componentname *name,struct inode *target)
{
	struct ufs1_mount_state *ms=state(directory->i_mount);
	struct inode *existing;uint32_t removed;int error,rollback_error;
	if(target==NULL||target->i_mount!=directory->i_mount)return EXDEV;
	if(target->i_type==INODE_DIR)return EPERM;
	mutex_lock(&ms->namespace_lock);
	mutex_lock(&target->i_lock);
	if(target->i_linkcount==UINT16_MAX){mutex_unlock(&target->i_lock);error=EMLINK;goto out;}
	mutex_unlock(&target->i_lock);
	error=ufs1_lookup(directory,name,&existing);
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
ufs1_symlink(struct inode *directory,const struct componentname *name,
	const char *target,struct inode **result)
{
	struct ufs1_mount_state *ms=state(directory->i_mount);
	struct inode *existing,*inode;size_t length=strlen(target);int error;
	if(length>state(directory->i_mount)->super.maxsymlinklen||length>60U)return ENAMETOOLONG;
	mutex_lock(&ms->namespace_lock);
	error=ufs1_lookup(directory,name,&existing);
	if(error==0){inode_release(existing);error=EEXIST;goto out;}
	if(error!=ENOENT)goto out;
	error=new_inode(directory->i_mount,S_IFLNK|0777U,INODE_SYMLINK,1,&inode);if(error)goto out;
	inode->i_size=(off_t)length;memcpy(info(inode)->shortlink,target,length);error=persist_inode(inode);
	if(error==0)error=dir_add(directory,name,(uint32_t)inode->i_ino,10);
	if(error){inode->i_linkcount=0;inode->i_flags|=INODE_DEAD;inode_release(inode);goto out;}
	*result=inode;
out:
	mutex_unlock(&ms->namespace_lock);
	return error;
}

static ssize_t ufs1_read(struct file *file,void *buffer,size_t length)
{ ssize_t n=pread_inode(file->f_inode,buffer,length,file->f_offset); if(n>0)file->f_offset+=n; return n; }
static ssize_t ufs1_pread(struct file *file,void *buffer,size_t length,off_t offset)
{ return pread_inode(file->f_inode,buffer,length,offset); }
static ssize_t ufs1_write(struct file *file,const void *buffer,size_t length)
{ ssize_t n=pwrite_inode(file->f_inode,buffer,length,file->f_offset);if(n>0)file->f_offset+=n;return n; }
static ssize_t ufs1_pwrite(struct file *file,const void *buffer,size_t length,off_t offset)
{ return pwrite_inode(file->f_inode,buffer,length,offset); }
static int ufs1_readdir(struct file *file,struct dirent *entry,int *eof)
{
	uint32_t number; uint8_t type; char name[NAME_MAX+1U]; int error=next_dirent(file->f_inode,&file->f_offset,&number,&type,name);
	if(error==ENOENT){*eof=1;return 0;} if(error)return error;
	memset(entry,0,sizeof(*entry)); entry->d_ino=number;
	entry->d_type=type==1?INODE_FIFO:type==4?INODE_DIR:type==8?INODE_REG:
		type==10?INODE_SYMLINK:type==12?INODE_SOCKET:INODE_NONE;
	strcpy(entry->d_name,name); *eof=0; return 0;
}
static ssize_t ufs1_readlink(struct inode *inode,char *buffer,size_t length)
{
	struct ufs1_mount_state *ms=state(inode->i_mount);
	if(inode->i_type!=INODE_SYMLINK)return -EINVAL;
	if((uint64_t)inode->i_size<=ms->super.maxsymlinklen && inode->i_size<=60){size_t n=(size_t)inode->i_size;if(n>length)n=length;memcpy(buffer,info(inode)->shortlink,n);return (ssize_t)n;}
	return pread_inode(inode,buffer,length,0);
}

static int
ufs1_getattr(struct inode *inode,struct stat *status)
{
	struct ufs1_inode_info *ui=info(inode);

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
	return seconds>=INT32_MIN && seconds<=INT32_MAX && nanoseconds>=0 &&
	    nanoseconds<1000000000L;
}

static int
ufs1_setattr(struct inode *inode,const struct stat *status,unsigned mask)
{
	mode_t old_mode;
	uid_t old_uid;
	gid_t old_gid;
	struct inode_time old_atime,old_mtime,old_ctime;
	long atime_nsec=0,mtime_nsec=0,ctime_nsec=0;
	int error;

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
		error=ufs1_truncate(inode,status->st_size);
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
	inode->i_mode=old_mode;
	inode->i_uid=old_uid;
	inode->i_gid=old_gid;
	inode->i_atime=old_atime;
	inode->i_mtime=old_mtime;
	inode->i_ctime=old_ctime;
	mutex_unlock(&inode->i_lock);
	return error;
}

static int
ufs1_inode_sync(struct inode *inode)
{
	int error;

	mutex_lock(&inode->i_lock);
	error=persist_inode(inode);
	mutex_unlock(&inode->i_lock);
	return error;
}

static void ufs1_reclaim(struct inode *inode)
{
	struct ufs1_inode_info *ui=info(inode);
	if(inode->i_linkcount!=0||inode->i_ino<=UFS1_ROOT_INO||!state(inode->i_mount)->writable)return;
	if(ufs1_truncate(inode,0)!=0)
		return;
	if(inode->i_type==INODE_DIR&&adjust_directory_count(inode->i_mount,
	    (uint32_t)inode->i_ino,-1)!=0)
		return;
	inode->i_mode=0;inode->i_type=INODE_NONE;ui->blocks=0;
	if(persist_inode(inode)==0)
		(void)free_inode_number(inode->i_mount,(uint32_t)inode->i_ino);
}
static const struct inode_ops ufs1_inode_ops={.lookup=ufs1_lookup,.create=ufs1_create,
	.mkdir=ufs1_mkdir,.mknod=ufs1_mknod,.unlink=ufs1_unlink,.rmdir=ufs1_rmdir,.rename=ufs1_rename,
	.link=ufs1_link,
	.symlink=ufs1_symlink,.readlink=ufs1_readlink,.getattr=ufs1_getattr,
	.setattr=ufs1_setattr,.truncate=ufs1_truncate,.sync=ufs1_inode_sync,
	.reclaim=ufs1_reclaim};
static int ufs1_file_sync(struct file *file)
{
	int error;
	if(file==NULL)return EINVAL;
	error=inode_sync(file->f_inode);
	return error!=0?error:disk_sync(file->f_inode->i_mount->m_disk);
}
static const struct file_ops ufs1_regular_ops={.read=ufs1_read,.write=ufs1_write,.pread=ufs1_pread,.pwrite=ufs1_pwrite,.fsync=ufs1_file_sync};
static const struct file_ops ufs1_directory_ops={.readdir=ufs1_readdir};

static struct inode *ufs1_alloc_inode(struct mount *mountp)
{ (void)mountp; return (struct inode *)kern_calloc(1,sizeof(struct ufs1_inode_info)); }
static void ufs1_free_inode(struct inode *inode) { kern_free(inode); }

static int ufs1_read_super(struct disk *disk,struct ufs1_super *super)
{
	uint8_t *buffer; int error;
	if(disk==NULL||disk->d_block_size!=UFS1_SECTOR_SIZE)return EOPNOTSUPP;
	buffer=kern_malloc(UFS1_SBLOCK_SIZE); if(buffer==NULL)return ENOMEM;
	error=disk_read(disk,UFS1_SBLOCK_OFFSET/UFS1_SECTOR_SIZE,UFS1_SBLOCK_SIZE/UFS1_SECTOR_SIZE,buffer);
	if(error==0)error=ufs1_super_decode(buffer,UFS1_SBLOCK_SIZE,disk->d_block_count,super);
	kern_free(buffer); return error;
}
static int
ufs1_write_clean(struct mount *mountp,uint8_t clean)
{
	struct ufs1_mount_state *ms=state(mountp);uint8_t *buffer;int error;
	buffer=kern_malloc(UFS1_SBLOCK_SIZE);if(buffer==NULL)return ENOMEM;
	error=disk_read(mountp->m_disk,UFS1_SBLOCK_OFFSET/UFS1_SECTOR_SIZE,UFS1_SBLOCK_SIZE/UFS1_SECTOR_SIZE,buffer);
	if(error==0){buffer[UFS1_FS_CLEAN]=clean;error=disk_write(mountp->m_disk,UFS1_SBLOCK_OFFSET/UFS1_SECTOR_SIZE,UFS1_SBLOCK_SIZE/UFS1_SECTOR_SIZE,buffer);}
	if(error==0)
		error=disk_sync(mountp->m_disk);
	if(error==0)
		ms->super.clean=clean;
	kern_free(buffer);
	return error;
}
static int ufs1_probe(struct disk *disk) { struct ufs1_super s; return ufs1_read_super(disk,&s); }
static int ufs1_mount_impl(struct mount *mountp)
{
	struct ufs1_mount_state *ms; struct inode *root; int error;
	uint64_t total_ndir=0,total_nbfree=0,total_nifree=0,total_nffree=0;
	uint32_t cg;
	off_t cursor=0;
	uint32_t number;
	uint8_t type;
	char name[NAME_MAX+1U];
	if(mountp==NULL||mountp->m_disk==NULL)return EINVAL;
	ms=kern_calloc(1,sizeof(*ms)); if(ms==NULL)return ENOMEM;
	error=ufs1_read_super(mountp->m_disk,&ms->super); if(error){kern_free(ms);return error;}
	(void)mutex_init(&ms->namespace_lock,LOCK_RANK_NAMESPACE,
		"ufs1 namespace");
	(void)mutex_init(&ms->lock,LOCK_RANK_INODE,"ufs1 mount");
	ms->cg=kern_malloc(ms->super.bsize);if(ms->cg==NULL){kern_free(ms);return ENOMEM;}
	mountp->m_data=ms;
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
		if(cg==0&&!bit_test(ms->cg+ms->cg_iusedoff,UFS1_ROOT_INO))
			{error=EINVAL;break;}
		total_ndir+=ufs1_get32(ms->cg,UFS1_CG_NDIR,ms->super.swapped);
		total_nbfree+=ufs1_get32(ms->cg,UFS1_CG_NBFREE,ms->super.swapped);
		total_nifree+=ufs1_get32(ms->cg,UFS1_CG_NIFREE,ms->super.swapped);
		total_nffree+=ufs1_get32(ms->cg,UFS1_CG_NFFREE,ms->super.swapped);
	}
	if(error==0&&(total_ndir!=ms->super.cstotal_ndir||
	    total_nbfree!=ms->super.cstotal_nbfree||
	    total_nifree!=ms->super.cstotal_nifree||
	    total_nffree!=ms->super.cstotal_nffree))error=EINVAL;
	if(error==0)error=load_cg_locked(mountp,0);
	if(error!=0){mountp->m_data=NULL;kern_free(ms->cg);kern_free(ms);return error;}
	/* The structural and allocation-summary checks above are the recovery
	 * gate for an unclean filesystem.  A power loss must not make a valid
	 * persistent upper permanently unmountable merely because fs_clean is 0. */
	if((mountp->m_flags&MOUNT_READ_ONLY)==0){if((mountp->m_disk->d_flags&DISK_READ_ONLY)!=0){mountp->m_data=NULL;kern_free(ms->cg);kern_free(ms);return EROFS;}ms->writable=1;}
	error=load_inode(mountp,UFS1_ROOT_INO,&root);
	if(error||root->i_type!=INODE_DIR){if(!error){root->i_flags|=INODE_DEAD;inode_release(root);}mountp->m_data=NULL;kern_free(ms->cg);kern_free(ms);return error?error:EIO;}
	/* A malformed root must not become the namespace anchor.  Validate the
	 * mandatory entries while the mount is still private and unpublished. */
	error=next_dirent(root,&cursor,&number,&type,name);
	if(error==0&&(number!=UFS1_ROOT_INO||strcmp(name,".")!=0))error=EIO;
	if(error==0)error=next_dirent(root,&cursor,&number,&type,name);
	if(error==0&&(number!=UFS1_ROOT_INO||strcmp(name,"..")!=0))error=EIO;
	if(error!=0){root->i_flags|=INODE_DEAD;inode_release(root);mountp->m_data=NULL;kern_free(ms->cg);kern_free(ms);return error;}
	/* Do not dirty an image until every read-only mount validation, including
	 * the root inode, has succeeded. */
	if(ms->writable){error=ufs1_write_clean(mountp,0);if(error){root->i_flags|=INODE_DEAD;inode_release(root);mountp->m_data=NULL;kern_free(ms->cg);kern_free(ms);return error;}}
	root->i_flags|=INODE_ROOT; mountp->m_root=root; return 0;
}
static int ufs1_sync(struct mount *mountp) { return mountp==NULL?EINVAL:disk_sync(mountp->m_disk); }
static int ufs1_statvfs(struct mount *mountp,struct statvfs *result)
{
	struct ufs1_mount_state *ms=state(mountp);uint32_t nbfree,nffree,nifree;
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
static int ufs1_prepare_unmount(struct mount *mountp) { struct ufs1_mount_state *ms=state(mountp);return ms!=NULL&&ms->writable?ufs1_write_clean(mountp,1):0; }
static void ufs1_unmount(struct mount *mountp) { if(mountp&&mountp->m_data){struct ufs1_mount_state *ms=state(mountp);kern_free(ms->cg);kern_free(ms);mountp->m_data=NULL;} }

const struct filesystem_type ufs1_filesystem_type={
	.fs_name="ufs1",.probe=ufs1_probe,.mount=ufs1_mount_impl,.sync=ufs1_sync,
	.statvfs=ufs1_statvfs,
	.prepare_unmount=ufs1_prepare_unmount,
	.unmount=ufs1_unmount,.alloc_inode=ufs1_alloc_inode,.free_inode=ufs1_free_inode,
};
