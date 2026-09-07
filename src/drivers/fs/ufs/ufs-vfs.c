/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Conservative FreeBSD-derived UFS VFS implementation. */
#include <kern/io-pool.h>
#include <kern/cache-memory.h>
#include <kern/page.h>
#include <kern/buf.h>
#include <kern/sched.h>
#include <kern/writeback.h>
#include "kern/ufs.h"
#include "ufs-disk.h"
#include "ufs-endian.h"
#include "ufs-super.h"
#include "kern/disk.h"
#include "kern/io-stats.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/mount.h"
#include "kern/namei.h"
#include "kern/namecache.h"
#include "kern/pipe.h"
#include "kern/quota.h"
#include "kern/test-fault.h"
#include "ufs-consistency.h"
#include "ufs-private.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/statvfs.h>
#include <zedbsd/blkid.h>
#include <zedbsd/quota.h>
#include <zedbsd/snapshot.h>

void clock_realtime(time_t *,long *);

static int ufs_writeback_range(struct file *file, off_t offset, size_t length);
static int inode_size_values(const uint8_t *raw, const struct ufs_super *super, uint64_t *size, uint64_t *blocks);
static int journal_image_alloc(struct ufs_mount_state *ms);
static void journal_image_free(struct ufs_mount_state *ms);
static int journal_read(void *context, uint64_t lba, uint32_t count, void *buffer);
static int journal_write(void *context, uint64_t lba, uint32_t count, const void *buffer);
static int journal_flush(void *context);
static ssize_t pwrite_inode(struct inode *inode, const void *buffer, size_t length, off_t offset);
static ssize_t pwrite_inode_context(struct inode *inode, const void *buffer, size_t length, off_t offset, const struct io_context *context);
static ssize_t ufs_pwrite_context(struct file *file, const void *buffer, size_t length, off_t offset, unsigned flags, const struct ucred *credential, const struct io_context *context);
static int write_sectors_impl(struct mount *mountp, uint64_t lba, uint32_t count, const void *buffer, const struct io_context *context);
static int write_sectors_context(struct mount *mountp, uint64_t lba, uint32_t count, const void *buffer, const struct io_context *context);
static int write_sectors(struct mount *mountp, uint64_t lba, uint32_t count, const void *buffer);
static int observed_disk_read(struct disk *disk, uint64_t block, uint32_t count, void *buffer);
static int ufs_sync(struct mount *mountp);
extern void io_error_record(struct io_error_state *, int) __attribute__((weak));

/* Caller owns journal_lock; no borrowed operation context survives this drain. */
static int
journal_checkpoint_locked(struct mount *mountp)
{
	struct ufs_mount_state *ms = mountp->m_data;
	struct io_context child;
	int error;

	if (!ms->journal_enabled)
		return 0;
	error = io_context_child(&child, NULL, IO_CONTEXT_ORDERED);
	if (error != 0)
		return error;
	ms->journal_io.context = &child;
	error = ufs_journal_drain(&ms->journal);
	ms->journal_io.context = NULL;
	if (error != 0 && io_error_record != NULL) {
		io_error_record(&mountp->m_metadata_error, error);
		io_error_record(&mountp->m_write_error, error);
	}
	if (ms->journal.poisoned)
		ms->writable = 0;
	return error;
}

/* Counts filesystem disk requests, including metadata and journal traffic. */
static int
observed_disk_read(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	void *buffer)
{
	io_stats_record(IO_UFS_READ,
	    disk != NULL ? (uint64_t)count * disk->d_block_size : 0);
	return disk_read(disk, block, count, buffer);
}

#define UFS_IFMT 0170000U
#define UFS_IFIFO 0010000U
#define UFS_IFCHR 0020000U
#define UFS_IFDIR 0040000U
#define UFS_IFBLK 0060000U
#define UFS_IFREG 0100000U
#define UFS_IFLNK 0120000U
#define UFS_IFSOCK 0140000U
#define UFS_QUOTA_XATTR "system.zedbsd.quota"



static int
journal_read(void *context, uint64_t lba, uint32_t count, void *buffer)
{
	struct ufs_io_owner *owner = context;

	return observed_disk_read(owner->disk, lba, count, buffer);
}

static int
journal_write(void *context, uint64_t lba, uint32_t count, const void *buffer)
{
	struct ufs_io_owner *owner = context;
	struct io_context child;
	int error;

	error = io_context_child(&child, owner->context, IO_CONTEXT_ORDERED);
	if (error != 0)
		return error;
	io_stats_record(IO_UFS_WRITE, (uint64_t)count * owner->disk->d_block_size);
	return disk_write_context(owner->disk, lba, count, buffer, &child);
}

static int
journal_flush(void *context)
{
	struct ufs_io_owner *owner = context;

	return disk_sync(owner->disk);
}

static uint32_t locator_get32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t locator_get64(const uint8_t *p)
{ return locator_get32(p)|(uint64_t)locator_get32(p+4)<<32; }
static uint32_t locator_digest(const uint8_t *p,size_t length)
{ uint32_t value=2166136261U;size_t n;for(n=0;n<length;n++){value^=p[n];value*=16777619U;}return value; }

static void journal_wait_readers(struct ufs_mount_state *ms);

/* Drains short immutable copies before the serialized writer reuses their backing. */
static void
journal_wait_readers(struct ufs_mount_state *ms)
{
	/* Readers release their pins without acquiring the writer's journal mutex. */
	while (ufs_journal_views_busy(&ms->journal))
		sched_yield();
}

/* Reserves and accounts immutable redo storage before journal recovery/admission. */
static int
journal_image_alloc(struct ufs_mount_state *ms)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, UFS_JOURNAL_IMAGE_BYTES, ZEDBSD_PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	struct hal_pmem memory;
	int error;

	/* Obtains backing without holding a metadata or journal mutation lock. */
	memset(&memory, 0, sizeof(memory));
	error = hal_pmem_alloc(&request, &memory);
	if (error != HAL_OK || memory.vaddr == NULL || memory.size < UFS_JOURNAL_IMAGE_BYTES) {
		if (memory.size != 0 && hal_pmem_free(&memory) != HAL_OK)
			HAL_FATAL("ufs journal allocation rollback failed");
		return ENOMEM;
	}

	/* Charges the allocator's complete rounded backing to shared metadata memory. */
	error = cache_memory_reserve(CACHE_MEMORY_BUF_META, memory.size, 0);
	if (error != 0) {
		if (hal_pmem_free(&memory) != HAL_OK)
			HAL_FATAL("ufs journal reservation rollback failed");
		return error;
	}
	cache_memory_commit(CACHE_MEMORY_BUF_META, memory.size);
	ms->journal_memory = memory;
	error = ufs_journal_bind_image(&ms->journal, memory.vaddr, memory.size);
	if (error != 0)
		journal_image_free(ms);

	/* Reports a complete immutable-image owner or a fully unwound failure. */
	return error;
}

/* Releases backing only after the mount owner has excluded every journal caller. */
static void
journal_image_free(struct ufs_mount_state *ms)
{
	size_t bytes;

	/* Failed mount recovery may retain durable redo, but has no admitted readers. */
	bytes = ms->journal_memory.size;
	if (bytes == 0)
		return;
	ufs_journal_views_close(&ms->journal);
	journal_wait_readers(ms);
	if (hal_pmem_free(&ms->journal_memory) != HAL_OK)
		HAL_FATAL("ufs journal backing release failed");
	cache_memory_release(CACHE_MEMORY_BUF_META, bytes);
	memset(&ms->journal_memory, 0, sizeof(ms->journal_memory));
	ms->journal.image = NULL;
	ms->journal.image_valid = 0;
}

static int
journal_discover(struct mount *mountp,struct ufs_mount_state *ms)
{
	struct ufs_journal_io io;
	uint8_t locator[UFS_SECTOR_SIZE];
	uint64_t end=ms->super.size<<ms->super.fsbtodb;
	uint32_t sectors;
	int error;
	if(end>=mountp->m_disk->d_block_count)return 0;
	error=observed_disk_read(mountp->m_disk,end,1,locator);if(error!=0)return error;
	if(memcmp(locator,"ZUJ",3)==0&&memcmp(locator,"ZUJ2",4)!=0)return EINVAL;
	if(memcmp(locator,"ZUJ2",4)!=0)return 0;
	sectors=locator_get32(locator+8);
	if(locator_get32(locator+4)!=2U||locator_get64(locator+12)!=end||
	    locator_get32(locator+24)!=locator_digest(locator,24)||sectors<18U||
	    (uint64_t)sectors+1U>mountp->m_disk->d_block_count-end)return EINVAL;
	ms->journal_io.disk=mountp->m_disk;
	io.context=&ms->journal_io;io.read=journal_read;io.write=journal_write;
	io.flush=journal_flush;
	error=ufs_journal_init(&ms->journal,&io,end+1U,sectors,end);
	if(error==0)error=journal_image_alloc(ms);
	if(error==0){ms->journal_enabled=1;error=ufs_journal_replay(&ms->journal);}
	return error;
}

static int
snapshot_discover(struct mount *mountp,struct ufs_mount_state *ms)
{
	struct ufs_journal_io io;uint8_t locator[UFS_SECTOR_SIZE];
	uint64_t end=ms->super.size<<ms->super.fsbtodb,cursor=end;
	uint32_t sectors,max_records;size_t map_count;int error;
	if(end>=mountp->m_disk->d_block_count)return 0;
	error=observed_disk_read(mountp->m_disk,end,1,locator);if(error!=0)return error;
	if(memcmp(locator,"ZUJ2",4)==0) {
		sectors=locator_get32(locator+8);
		if(locator_get32(locator+4)!=2U||locator_get64(locator+12)!=end||
		    sectors>mountp->m_disk->d_block_count-end-1U)return EINVAL;
		cursor=end+1U+sectors;
	}
	if(cursor>=mountp->m_disk->d_block_count)return 0;
	error=observed_disk_read(mountp->m_disk,cursor,1,locator);if(error!=0)return error;
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
	ms->snapshot_io.disk=mountp->m_disk;
	io.context=&ms->snapshot_io;io.read=journal_read;io.write=journal_write;
	io.flush=journal_flush;
	error=ufs_snapshot_init(&ms->snapshot,&io,end,cursor+1U,sectors,
	    ms->snapshot_map,map_count);
	if(error==0)error=ufs_snapshot_open(&ms->snapshot);
	if(error!=0){kern_free(ms->snapshot_map);ms->snapshot_map=NULL;return error;}
	ms->snapshot_available=1;return 0;
}

static int
write_sectors_impl(struct mount *mountp,uint64_t lba,uint32_t count,
	const void *buffer, const struct io_context *context)
{
	struct ufs_mount_state *ms=mountp!=NULL?mountp->m_data:NULL;
	int snapshot_locked=0;
	if(ms!=NULL&&ms->snapshot_available) {
		int error;
		mutex_lock(&ms->snapshot_lock);snapshot_locked=1;
		ms->snapshot_io.context=context;
		error=ufs_snapshot_preserve(&ms->snapshot,lba,count);
		ms->snapshot_io.context=NULL;
		if(error!=0){mutex_unlock(&ms->snapshot_lock);return error;}
	}
	if(ms!=NULL&&ms->journal_enabled) {
		int error;
		mutex_lock(&ms->journal_lock);
		error=journal_checkpoint_locked(mountp);
		if(error==0) {
			journal_wait_readers(ms);
			ms->journal_io.context=context;
			error=ufs_journal_commit(&ms->journal,lba,buffer,count);
			ms->journal_io.context=NULL;
		}
		if(error!=0&&ms->journal.poisoned)
			ms->writable=0;
		mutex_unlock(&ms->journal_lock);
		if(snapshot_locked)mutex_unlock(&ms->snapshot_lock);
		return error;
	}
	{
		int error;
		io_stats_record(IO_UFS_WRITE, (uint64_t)count * mountp->m_disk->d_block_size);
		error=disk_write_context(mountp->m_disk,lba,count,buffer,context);
		if(snapshot_locked)mutex_unlock(&ms->snapshot_lock);
		return error;
	}
}

/* Retains the logical write owner across snapshot and journal completion. */
static int
write_sectors_context(struct mount *mountp, uint64_t lba, uint32_t count, const void *buffer, const struct io_context *context)
{
	struct io_context child;
	int error;

	error = io_context_child(&child, context, IO_CONTEXT_ORDERED);
	if (error != 0)
		return error;
	io_epoch_begin(&mountp->m_write_epoch);
	error = write_sectors_impl(mountp, lba, count, buffer, &child);
	io_epoch_end(&mountp->m_write_epoch);
	return error;
}

static int
write_sectors(struct mount *mountp, uint64_t lba, uint32_t count, const void *buffer)
{
	return write_sectors_context(mountp, lba, count, buffer, NULL);
}




static const struct inode_ops ufs_inode_ops;
static const struct file_ops ufs_regular_ops;
static const struct file_ops ufs_directory_ops;
static int ufs_lookup(struct inode *,const struct componentname *,
	struct inode **);
static int persist_inode(struct inode *);
static int reclaim_unlinked_inode(struct inode *inode);
static int discard_reserved_inode(struct inode *inode);
static int creation_unlink_group(struct inode *inode);
static int persist_inode_locked(struct inode *);
static int prepare_inode_locked(struct inode *inode, uint8_t *block, uint64_t *location);
static uint64_t inode_fragment(struct inode *inode);
static void encode_inode_locked(struct inode *inode, uint8_t *block);
static ssize_t ufs_getxattr(struct inode *,const char *,void *,size_t);
static int ufs_setxattr(struct inode *,const char *,const void *,size_t,
	unsigned);

static struct ufs_mount_state *state(const struct mount *mountp)
{ return mountp != NULL ? mountp->m_data : NULL; }
static struct ufs_inode_info *info(const struct inode *inode)
{ return (struct ufs_inode_info *)(uintptr_t)inode; }

static int journal_read_image(struct ufs_mount_state *ms, uint64_t first, uint32_t count, void *buffer);

/* Copies a fully covered committed image without joining checkpoint device I/O. */
static int
journal_read_image(struct ufs_mount_state *ms, uint64_t first, uint32_t count, void *buffer)
{
	struct ufs_journal_view view = {0};
	int error;

	/* Acquires a generation whose storage cannot be retired underneath the copy. */
	if (!ms->journal_enabled)
		return ENOENT;
	error = ufs_journal_view_acquire(&ms->journal, &view);
	if (error != 0)
		return error;
	error = ufs_journal_view_copy(&view, first, count, buffer);
	ufs_journal_view_release(&view);

	/* Releases before any caller falls back to the serialized home-read path. */
	return error;
}

/* Reads metadata at any sector granularity through the committed redo owner. */
static int
read_metadata_sectors(struct mount *mountp, uint64_t first, uint32_t count, void *buffer)
{
	struct ufs_mount_state *ms = state(mountp);
	int error;

	error = journal_read_image(ms, first, count, buffer);
	if (error != ENOENT)
		return error;
	if (ms->journal_enabled)
		mutex_lock(&ms->journal_lock);
	if (ms->journal_enabled && (ms->journal.pending_sequence != 0 || ms->journal.poisoned))
		error = ufs_journal_read(&ms->journal, first, count, buffer);
	else
		error = observed_disk_read(mountp->m_disk, first, count, buffer);
	if (ms->journal_enabled)
		mutex_unlock(&ms->journal_lock);
	return error;
}

static int
read_block(struct mount *mountp, uint64_t fragment, void *buffer)
{
	const struct ufs_super *s = &state(mountp)->super;

	if (fragment == 0) { memset(buffer, 0, s->bsize); return 0; }
	if (fragment >= s->size || s->frag > s->size - fragment)
		return EIO;
	return read_metadata_sectors(mountp, fragment << s->fsbtodb,
	    s->bsize / UFS_SECTOR_SIZE, buffer);
}

static int
write_block(struct mount *mountp, uint64_t fragment, const void *buffer)
{
	const struct ufs_super *s = &state(mountp)->super;
	if (fragment == 0 || fragment >= s->size || s->frag > s->size-fragment)
		return EIO;
	return write_sectors(mountp,(uint64_t)fragment<<s->fsbtodb,
	    s->bsize/UFS_SECTOR_SIZE,buffer);
}

static size_t content_run_bytes(struct inode *inode, uint64_t logical, uint64_t first, size_t remaining, int writing, int *mapping_error);
static int read_content_block(struct mount *mountp, uint64_t fragment, void *buffer);
static int write_content_block(struct mount *mountp, uint64_t fragment, const void *buffer);
static int write_content_context(struct mount *mountp, uint64_t fragment, const void *buffer, const struct io_context *context);

/* Counts populated content blocks separately from metadata operations. */
static int
read_content_block(
	struct mount *mountp,
	uint64_t fragment,
	void *buffer)
{
	const struct ufs_super *s = &state(mountp)->super;

	if (fragment != 0 && fragment < s->size &&
	    s->frag <= s->size - fragment)
		io_stats_record(IO_UFS_CONTENT_READ, s->bsize);
	return read_block(mountp, fragment, buffer);
}

static int
write_content_block(
	struct mount *mountp,
	uint64_t fragment,
	const void *buffer)
{
	const struct ufs_super *s = &state(mountp)->super;

	if (fragment != 0 && fragment < s->size &&
	    s->frag <= s->size - fragment)
		io_stats_record(IO_UFS_CONTENT_WRITE, s->bsize);
	return write_block(mountp, fragment, buffer);
}

static int
write_content_context(
	struct mount *mountp,
	uint64_t fragment,
	const void *buffer,
	const struct io_context *context)
{
	const struct ufs_super *super;

	super = &state(mountp)->super;
	if (fragment == 0 || fragment >= super->size ||
	    super->frag > super->size - fragment)
		return EIO;
	io_stats_record(IO_UFS_CONTENT_WRITE, super->bsize);
	return write_sectors_context(mountp, fragment << super->fsbtodb,
	    super->bsize / UFS_SECTOR_SIZE, buffer, context);
}

static int bit_test(const uint8_t *map,uint32_t bit)
{ return (map[bit>>3]&(uint8_t)(1U<<(bit&7U)))!=0; }
static void bit_set(uint8_t *map,uint32_t bit)
{ map[bit>>3]|=(uint8_t)(1U<<(bit&7U)); }
static void bit_clear(uint8_t *map,uint32_t bit)
{ map[bit>>3]&=(uint8_t)~(1U<<(bit&7U)); }

static uint64_t
cgstart(const struct ufs_super *super, uint32_t cg)
{
	return (uint64_t)cg * super->fpg +
	    (uint64_t)super->cgoffset * (cg & ~super->cgmask);
}

static uint32_t
cg_ndblk(const struct ufs_super *super, uint32_t cg)
{
	uint64_t start = cgstart(super, cg);
	uint64_t remaining = start < super->size ? super->size - start : 0;
	return remaining > super->fpg ? super->fpg : (uint32_t)remaining;
}

static int load_cg_image(struct mount *mountp, uint32_t cg, uint64_t fragment);

/* Resolves the CG through immutable redo before considering cached home bytes. */
static int
load_cg_image(struct mount *mountp, uint32_t cg, uint64_t fragment)
{
	struct ufs_mount_state *ms;
	int error;

	/* Drops the home-view identity when committed redo supplies the working image. */
	ms = state(mountp);
	error = journal_read_image(ms, fragment << ms->super.fsbtodb,
	    ms->super.bsize / UFS_SECTOR_SIZE, ms->cg);
	if (error == 0) {
		buf_view_release(&ms->cg_view);
		io_stats_record(IO_UFS_CG_HIT, ms->super.bsize);
		return 0;
	}
	if (error != ENOENT)
		return error;

	/* Serializes uncovered/uncertain reads and cache identity with checkpoint writes. */
	if (ms->journal_enabled)
		mutex_lock(&ms->journal_lock);
	if (ms->journal_enabled && (ms->journal.pending_sequence != 0 || ms->journal.poisoned)) {
		buf_view_release(&ms->cg_view);
		error = ufs_journal_read(&ms->journal, fragment << ms->super.fsbtodb,
		    ms->super.bsize / UFS_SECTOR_SIZE, ms->cg);
	} else if (ms->cg_valid && ms->active_cg == cg &&
	    disk_view_matches(mountp->m_disk, &ms->cg_view)) {
		io_stats_record(IO_UFS_CG_HIT, ms->super.bsize);
		error = 0;
	} else {
		buf_view_release(&ms->cg_view);
		io_stats_record(IO_UFS_CG_MISS, ms->super.bsize);
		io_stats_record(IO_UFS_READ, ms->super.bsize);
		error = disk_read_view(mountp->m_disk, fragment << ms->super.fsbtodb,
		    ms->super.bsize / UFS_SECTOR_SIZE, ms->cg, &ms->cg_view);
	}
	if (ms->journal_enabled)
		mutex_unlock(&ms->journal_lock);
	return error;
}

static int
load_cg_locked(struct mount *mountp, uint32_t cg)
{
	struct ufs_mount_state *ms = state(mountp);
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
	error = load_cg_image(mountp, cg, fragment);
	if (error != 0) {
		ms->cg_valid = 0;
		return error;
	}
	ms->cg_valid = 0;
	ndblk = cg_ndblk(&ms->super, cg);
	inode_map_bytes = (ms->super.ipg + 7U) / 8U;
	free_map_bytes = (ms->super.fpg + 7U) / 8U;
	ndir = ufs_get32(ms->cg, UFS_CG_NDIR, ms->super.swapped);
	nbfree = ufs_get32(ms->cg, UFS_CG_NBFREE, ms->super.swapped);
	nifree = ufs_get32(ms->cg, UFS_CG_NIFREE, ms->super.swapped);
	nffree = ufs_get32(ms->cg, UFS_CG_NFFREE, ms->super.swapped);
	ms->cg_iusedoff = ufs_get32(ms->cg, UFS_CG_IUSEDOFF,
	    ms->super.swapped);
	ms->cg_freeoff = ufs_get32(ms->cg, UFS_CG_FREEOFF,
	    ms->super.swapped);
	ms->cg_nextfreeoff = ufs_get32(ms->cg, UFS_CG_NEXTFREEOFF,
	    ms->super.swapped);
	if (ufs_get32(ms->cg, UFS_CG_MAGIC, ms->super.swapped) !=
	    UFS_CG_MAGIC_VALUE ||
	    ufs_get32(ms->cg, UFS_CG_CGX, ms->super.swapped) != cg ||
	    ufs_get32(ms->cg, UFS_CG_NDBLK, ms->super.swapped) != ndblk ||
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
	{
		buf_view_release(&ms->cg_view);
		return EINVAL;
	}
	ms->active_cg = cg;
	ms->cg_valid = 1;
	ms->cg_dirty = 0;
	return 0;
}

static int
valid_inode_fragment(const struct ufs_super *super, uint64_t fragment)
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
prepare_super_summaries(struct mount *mountp, uint8_t *buffer)
{
	struct ufs_mount_state *ms=state(mountp);
	int error;
	error=read_metadata_sectors(mountp,UFS_SBLOCK_OFFSET/UFS_SECTOR_SIZE,
	    UFS_SBLOCK_SIZE/UFS_SECTOR_SIZE,buffer);
	if(error==0) {
		ufs_put64(buffer,UFS_FS_CSTOTAL_NDIR,
		    ms->super.cstotal_ndir,
		    ms->super.swapped);
		ufs_put64(buffer,UFS_FS_CSTOTAL_NBFREE,
		    ms->super.cstotal_nbfree,
		    ms->super.swapped);
		ufs_put64(buffer,UFS_FS_CSTOTAL_NIFREE,
		    ms->super.cstotal_nifree,
		    ms->super.swapped);
		ufs_put64(buffer,UFS_FS_CSTOTAL_NFFREE,
		    ms->super.cstotal_nffree,
		    ms->super.swapped);
	}
	return error;
}

/* Writes an independently prepared summary for synchronous metadata callers. */
static int
write_super_summaries(struct mount *mountp)
{
	uint8_t *buffer;
	int error;

	buffer = kern_malloc(UFS_SBLOCK_SIZE);
	if (buffer == NULL)
		return ENOMEM;
	error = prepare_super_summaries(mountp, buffer);
	if (error == 0) {
		error = write_sectors(mountp, UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE,
		    UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE, buffer);
	}
	kern_free(buffer);
	return error;
}

/* Writes the mount-owned CG image immediately; failed ownership remains explicit. */
static int
write_cg(
	struct mount *mountp)
{
	struct ufs_mount_state *ms;
	struct kern_test_fault_result fault;
	int error;

	/* Releases optional copy pins before writing or entering nested cache paths. */
	ms = state(mountp);
	ms->cg_valid = 0;
	ms->cg_dirty = 1;
	buf_view_release(&ms->cg_view);
	if (KERN_TEST_FAULT(KERN_TEST_FAULT_UFS_CG_WRITE, UINT32_MAX, UINT32_MAX, &fault))
		return fault.error != 0 ? fault.error : EIO;
	error = write_sectors(mountp,
	    (cgstart(&ms->super, ms->active_cg) + ms->super.cblkno) << ms->super.fsbtodb,
	    ms->super.bsize / UFS_SECTOR_SIZE, ms->cg);
	if (error == 0)
		error = write_super_summaries(mountp);
	if (error == 0)
		ms->cg_dirty = 0;

	/* Preserves the immediate writer's original error convention. */
	return error;
}

/* Caller holds ms->lock and has already restored the in-memory CG image. */
static int
write_cg_rollback(struct mount *mountp, int original_error)
{
	struct ufs_mount_state *ms = state(mountp);
	int rollback=write_cg(mountp);
	if (rollback != 0) {
		ms->writable = 0;
		return rollback;
	}
	return original_error;
}

static int
adjust_directory_count(struct mount *mountp,uint32_t ino,int delta)
{
	struct ufs_mount_state *ms=state(mountp);
	uint32_t count,cg=ino/ms->super.ipg;
	uint64_t old_total;
	int error;
	mutex_lock(&ms->lock);
	error=load_cg_locked(mountp,cg);
	if(error!=0){mutex_unlock(&ms->lock);return error;}
	count=ufs_get32(ms->cg,UFS_CG_NDIR,ms->super.swapped);
	old_total=ms->super.cstotal_ndir;
	if((delta<0&&count==0)||(delta>0&&count==UINT32_MAX))
		error=EIO;
	else {
		ufs_put32(ms->cg,UFS_CG_NDIR,
		    delta<0?count-1U:count+1U,ms->super.swapped);
		ms->super.cstotal_ndir=delta<0?old_total-1U:old_total+1U;
		error=write_cg(mountp);
		if(error!=0) {
			ufs_put32(ms->cg,UFS_CG_NDIR,count,
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
allocate_block_compat(struct mount *mountp,uid_t uid,gid_t gid,uint64_t *result)
{
	struct ufs_mount_state *ms=state(mountp); uint8_t *map;
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
			uint32_t free=ufs_get32(ms->cg,UFS_CG_NBFREE,ms->super.swapped);
			if(free==0){for(n=0;n<ms->super.frag;n++)bit_set(map,fragment+n);break;}
			ufs_put32(ms->cg,UFS_CG_NBFREE,free-1U,ms->super.swapped);
			ms->super.cstotal_nbfree=old_total-1U;
		}
		error=write_cg(mountp);
		if(error==0){uint8_t *zero=kern_calloc(1,ms->super.bsize);uint64_t absolute=cgstart(&ms->super,cg)+fragment;if(zero==NULL)error=ENOMEM;else{error=write_block(mountp,absolute,zero);kern_free(zero);}}
		if(error!=0){for(n=0;n<ms->super.frag;n++)bit_set(map,fragment+n);ufs_put32(ms->cg,UFS_CG_NBFREE,ufs_get32(ms->cg,UFS_CG_NBFREE,ms->super.swapped)+1U,ms->super.swapped);ms->super.cstotal_nbfree=old_total;error=write_cg_rollback(mountp,error);}else {*result=cgstart(&ms->super,cg)+fragment;ms->rotor_cg=cg;}
		break;
	}
		if(error!=ENOSPC)break;
	}
	mutex_unlock(&ms->lock);
	if(error==0)quota_commit(&charge);else quota_rollback(&charge);
	return error;
}

/* Immediate compatibility scope; p011 adds bounded deferred metadata ownership. */
struct ufs_allocation {
	struct mount *mountp;
	uid_t uid;
	gid_t gid;
	unsigned active;
};

static void allocation_begin(struct ufs_allocation *context, struct mount *mountp, uid_t uid, gid_t gid);
static int allocation_allocate(struct ufs_allocation *context, uint64_t *result);
static void allocation_commit(struct ufs_allocation *context);
static void allocation_abort(struct ufs_allocation *context);

/* Opens an allocation scope without reserving memory or delaying metadata. */
static void
allocation_begin(
	struct ufs_allocation *context,
	struct mount *mountp,
	uid_t uid,
	gid_t gid)
{
	context->mountp = mountp;
	context->uid = uid;
	context->gid = gid;
	context->active = 1;
	io_stats_record(IO_UFS_ALLOC_BEGIN, 0);
}

/* Uses the unchanged allocation/zero/rollback implementation in this initial stage. */
static int
allocation_allocate(
	struct ufs_allocation *context,
	uint64_t *result)
{
	int error;

	if (!context->active)
		return EINVAL;
	io_stats_record(IO_UFS_ALLOCATE, state(context->mountp)->super.bsize);
	error = allocate_block_compat(context->mountp, context->uid, context->gid, result);
	return error;
}

/* Closes an already-persisted allocation; there is no deferred state in p010. */
static void
allocation_commit(
	struct ufs_allocation *context)
{
	context->active = 0;
	io_stats_record(IO_UFS_ALLOC_COMMIT, 0);
}

/* Ends a failed scope whose compatibility allocator already owns rollback. */
static void
allocation_abort(
	struct ufs_allocation *context)
{
	context->active = 0;
	io_stats_record(IO_UFS_ALLOC_ABORT, 0);
}

/* Routes every block allocation through the explicit immediate scope. */
static int
allocate_block(struct mount *mountp, uid_t uid, gid_t gid, uint64_t *result)
{
	struct ufs_allocation context;
	int error;

	allocation_begin(&context, mountp, uid, gid);
	error = allocation_allocate(&context, result);
	if (error != 0) {
		allocation_abort(&context);
		return error;
	}
	allocation_commit(&context);
	return 0;
}

static int
free_block(struct mount *mountp,uint64_t fragment,uid_t uid,gid_t gid)
{
	struct ufs_mount_state *ms=state(mountp);uint8_t *map;uint32_t n,free,cg,local=0;uint64_t old_total;int error;
	for(cg=0;cg<ms->super.ncg;cg++){uint64_t start=cgstart(&ms->super,cg);uint32_t ndblk=cg_ndblk(&ms->super,cg);if(fragment>=start+ms->super.dblkno&&fragment+ms->super.frag<=start+ndblk){local=(uint32_t)(fragment-start);break;}}
	if(cg==ms->super.ncg)return EIO;
	mutex_lock(&ms->lock);error=load_cg_locked(mountp,cg);if(error!=0){mutex_unlock(&ms->lock);return error;}map=ms->cg+ms->cg_freeoff;
	for(n=0;n<ms->super.frag;n++)if(bit_test(map,local+n)){mutex_unlock(&ms->lock);return EIO;}
	free=ufs_get32(ms->cg,UFS_CG_NBFREE,ms->super.swapped);
	if(free==UINT32_MAX){mutex_unlock(&ms->lock);return EIO;}
	old_total=ms->super.cstotal_nbfree;
	for(n=0;n<ms->super.frag;n++)bit_set(map,local+n);
	ufs_put32(ms->cg,UFS_CG_NBFREE,free+1U,ms->super.swapped);
	ms->super.cstotal_nbfree=old_total+1U;
	error=write_cg(mountp);
	if(error!=0){for(n=0;n<ms->super.frag;n++)bit_clear(map,local+n);ufs_put32(ms->cg,UFS_CG_NBFREE,free,ms->super.swapped);ms->super.cstotal_nbfree=old_total;error=write_cg_rollback(mountp,error);}
	mutex_unlock(&ms->lock);
	if(error==0&&quota_release(&ms->quota,uid,gid,1,0)!=0){ms->writable=0;return EIO;}
	return error;
}

static int
allocate_inode_number(struct mount *mountp,uid_t uid,gid_t gid,uint32_t *number)
{
	struct ufs_mount_state *ms=state(mountp);struct quota_charge charge;
	uint8_t *map;uint32_t ino,cg,attempt;uint64_t old_total;int error;
	error=quota_reserve(&ms->quota,uid,gid,0,1,quota_now(),&charge);
	if(error!=0)return error;
	error=ENOSPC;mutex_lock(&ms->lock);
	for(attempt=0;attempt<ms->super.ncg;attempt++){cg=(ms->rotor_cg+attempt)%ms->super.ncg;error=load_cg_locked(mountp,cg);if(error!=0)break;error=ENOSPC;map=ms->cg+ms->cg_iusedoff;
	for(ino=cg==0?UFS_ROOT_INO+1U:0U;ino<ms->super.ipg;ino++)if(!bit_test(map,ino)){
		uint32_t free=ufs_get32(ms->cg,UFS_CG_NIFREE,ms->super.swapped);
		if(free==0)break;
		old_total=ms->super.cstotal_nifree;bit_set(map,ino);
		ufs_put32(ms->cg,UFS_CG_NIFREE,free-1U,ms->super.swapped);
		ms->super.cstotal_nifree=old_total-1U;
		error=write_cg(mountp);if(error!=0){bit_clear(map,ino);ufs_put32(ms->cg,UFS_CG_NIFREE,free,ms->super.swapped);ms->super.cstotal_nifree=old_total;error=write_cg_rollback(mountp,error);}else {*number=cg*ms->super.ipg+ino;ms->rotor_cg=cg;}break;
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
	struct ufs_mount_state *ms=state(mountp);uint8_t *map;uint32_t free,cg=number/ms->super.ipg,local=number%ms->super.ipg;uint64_t old_total;int error;
	if(number<=UFS_ROOT_INO||cg>=ms->super.ncg)return EIO;
	mutex_lock(&ms->lock);error=load_cg_locked(mountp,cg);if(error!=0){mutex_unlock(&ms->lock);return error;}map=ms->cg+ms->cg_iusedoff;
	if(!bit_test(map,local)){mutex_unlock(&ms->lock);return EIO;}
	free=ufs_get32(ms->cg,UFS_CG_NIFREE,ms->super.swapped);
	if(free==UINT32_MAX){mutex_unlock(&ms->lock);return EIO;}
	old_total=ms->super.cstotal_nifree;bit_clear(map,local);
	ufs_put32(ms->cg,UFS_CG_NIFREE,free+1U,ms->super.swapped);
	ms->super.cstotal_nifree=old_total+1U;error=write_cg(mountp);
	if(error!=0){bit_set(map,local);ufs_put32(ms->cg,UFS_CG_NIFREE,free,ms->super.swapped);ms->super.cstotal_nifree=old_total;error=write_cg_rollback(mountp,error);}
	mutex_unlock(&ms->lock);
	if(error==0&&quota_release(&ms->quota,uid,gid,0,1)!=0){ms->writable=0;return EIO;}
	return error;
}

/* Reads one pointer through the common bounded cache with sector-sized stack scratch. */
static int
indirect_entry(
	struct mount *mountp,
	uint64_t fragment,
	uint32_t index,
	uint64_t *result)
{
	const struct ufs_super *super;
	uint8_t sector[UFS_SECTOR_SIZE];
	uint64_t byte_offset;
	uint64_t lba;
	int error;

	/* Rejects invalid mappings before calculating the containing sector. */
	super = &state(mountp)->super;
	if (fragment == 0) {
		*result = 0;
		return 0;
	}
	if (index >= super->nindir || fragment >= super->size ||
	    super->frag > super->size - fragment)
		return EIO;
	byte_offset = (uint64_t)index * 8U;
	lba = ((uint64_t)fragment << super->fsbtodb) + byte_offset / UFS_SECTOR_SIZE;
	io_stats_record(IO_UFS_INDIRECT_WINDOW, sizeof(sector));
	error = read_metadata_sectors(mountp, lba, 1, sector);
	if (error == 0)
		*result = ufs_get64(sector, (size_t)(byte_offset % UFS_SECTOR_SIZE), super->swapped);

	/* Releases the common cache pin inside disk_read before returning the pointer. */
	return error;
}

static int
bmap(struct inode *inode, uint64_t logical, uint64_t *result)
{
	struct ufs_inode_info *ui = info(inode);
	const struct ufs_super *s = &state(inode->i_mount)->super;
	uint64_t span = s->nindir;
	uint64_t fragment;
	unsigned level, depth;
	int error;
	if (logical < UFS_NDADDR) { *result = ui->direct[logical]; return 0; }
	logical -= UFS_NDADDR;
	for (level = 0; level < UFS_NIADDR; level++) {
		if (logical < span)
			break;
		logical -= span;
		if (span > UINT64_MAX / s->nindir)
			return EOVERFLOW;
		span *= s->nindir;
	}
	if (level == UFS_NIADDR)
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
	struct ufs_inode_info *ui=info(inode);
	const struct ufs_super *s=&state(inode->i_mount)->super;
	uint64_t span=s->nindir;
	uint64_t *root,fragment;
	unsigned level,depth;
	int error;

	if(logical<UFS_NDADDR) {
		if(ui->direct[logical]==0) {
			uint64_t allocated;
			error=allocate_block(inode->i_mount,inode->i_uid,inode->i_gid,&allocated);
			if(error!=0)
				return error;
			ui->direct[logical]=allocated;
			ui->blocks+=s->bsize/UFS_SECTOR_SIZE;
			/* Make the allocation reachable before user data I/O. */
			error=persist_inode(inode);
			if(error!=0) {
				int rollback;
				ui->direct[logical]=0;
				ui->blocks-=s->bsize/UFS_SECTOR_SIZE;
				/* Failure can follow a committed write (including replay).
				 * Confirm pointer removal before recycling the allocation. */
				rollback=persist_inode(inode);
				if(rollback==0)rollback=disk_sync(inode->i_mount->m_disk);
				if(rollback==0)rollback=free_block(inode->i_mount,allocated,inode->i_uid,inode->i_gid);
				if(rollback!=0)state(inode->i_mount)->writable=0;
				return error;
			}
		}
		*result=ui->direct[logical];
		return 0;
	}
	logical-=UFS_NDADDR;
	for(level=0;level<UFS_NIADDR;level++) {
		if(logical<span)
			break;
		logical-=span;
		if(span>UINT64_MAX/s->nindir)
			return EOVERFLOW;
		span*=s->nindir;
	}
	if(level==UFS_NIADDR)
		return EFBIG;
	root=&ui->indirect[level];
	if(*root==0) {
		uint64_t allocated;
		error=allocate_block(inode->i_mount,inode->i_uid,inode->i_gid,&allocated);
		if(error!=0)
			return error;
		*root=allocated;
		ui->blocks+=s->bsize/UFS_SECTOR_SIZE;
		error=persist_inode(inode);
		if(error!=0) {
			int rollback;
			*root=0;
			ui->blocks-=s->bsize/UFS_SECTOR_SIZE;
			/* Failure can follow a committed write (including replay).
			 * Confirm pointer removal before recycling the allocation. */
			rollback=persist_inode(inode);
			if(rollback==0)rollback=disk_sync(inode->i_mount->m_disk);
			if(rollback==0)rollback=free_block(inode->i_mount,allocated,inode->i_uid,inode->i_gid);
			if(rollback!=0)state(inode->i_mount)->writable=0;
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
		next=ufs_get64(block,(size_t)index*8U,s->swapped);
		if(next==0) {
			uint64_t allocated=0;
			error=allocate_block(inode->i_mount,inode->i_uid,inode->i_gid,&next);
			if(error==0) {
				allocated=next;
				ufs_put64(block,(size_t)index*8U,next,s->swapped);
				error=write_block(inode->i_mount,fragment,block);
			}
			if(error!=0) {
				if(allocated!=0) {
					int rollback_error;
					/* A short write may have published the pointer even
					 * though write_block() reported EIO.  Make it
					 * unreachable before returning its block. */
					ufs_put64(block,(size_t)index*8U,0,s->swapped);
					rollback_error=write_block(inode->i_mount,
					    fragment,block);
					if(rollback_error==0)
						rollback_error=disk_sync(inode->i_mount->m_disk);
					if(rollback_error==0)
						rollback_error=free_block(inode->i_mount,
						    allocated,inode->i_uid,inode->i_gid);
					if(rollback_error!=0) {
						/* The block may remain reachable.  Never free
						 * uncertain storage or continue writable. */
						ui->blocks+=s->bsize/UFS_SECTOR_SIZE;
						state(inode->i_mount)->writable=0;
					}
				}
				kern_free(block);
				return error;
			}
			ui->blocks+=s->bsize/UFS_SECTOR_SIZE;
		}
		kern_free(block);
		fragment=next;
	}
	*result=fragment;
	return 0;
}

/* Measures an existing physical run without changing allocation or publishing size. */
static size_t
content_run_bytes(
	struct inode *inode,
	uint64_t logical,
	uint64_t first,
	size_t remaining,
	int writing,
	int *mapping_error)
{
	const struct ufs_super *super;
	uint64_t boundary;
	uint64_t maximum;
	uint64_t blocks;
	uint64_t next;
	int error;
	uint64_t journal_blocks;

	/* Bounds the mapping scan to one indirect leaf and the common byte limit. */
	*mapping_error = 0;
	super = &state(inode->i_mount)->super;
	if (first == 0 || first >= super->size || super->frag > super->size - first)
		return 0;
	maximum = remaining / super->bsize;
	if (maximum > KERN_IO_BATCH_MAX / super->bsize)
		maximum = KERN_IO_BATCH_MAX / super->bsize;
	if (logical < UFS_NDADDR)
		boundary = UFS_NDADDR - logical;
	else
		boundary = super->nindir - (logical - UFS_NDADDR) % super->nindir;
	if (maximum > boundary)
		maximum = boundary;
	if (maximum > (super->size - first) / super->frag)
		maximum = (super->size - first) / super->frag;

	/* Preserves the existing journal's per-transaction payload capacity. */
	if (writing && state(inode->i_mount)->journal_enabled) {
		journal_blocks = state(inode->i_mount)->journal.sector_count;
		journal_blocks = journal_blocks > 2U ? journal_blocks - 2U : 0;
		journal_blocks /= super->bsize / UFS_SECTOR_SIZE;
		if (maximum > journal_blocks)
			maximum = journal_blocks;
	}

	/* Stops before a hole, discontinuity or a failed optional lookahead. */
	if (maximum == 0)
		return 0;
	for (blocks = 1; blocks < maximum; blocks++) {
		error = bmap(inode, logical + blocks, &next);
		if (error != 0) {
			*mapping_error = error;
			break;
		}
		if (next == 0 || next != first + blocks * super->frag)
			break;
	}

	/* Returns only the validated contiguous byte span. */
	return (size_t)blocks * super->bsize;
}

/* Reads full mapped runs directly and retains block scratch for edges and holes. */
static ssize_t
pread_inode(
	struct inode *inode,
	void *buffer,
	size_t length,
	off_t offset)
{
	const struct ufs_super *super;
	uint8_t *scratch;
	size_t done;
	size_t within;
	size_t amount;
	uint64_t position;
	uint64_t logical;
	uint64_t fragment;
	int error;
	int mapping_error;

	/* Clips the request to the current file contents. */
	super = &state(inode->i_mount)->super;
	if (offset < 0)
		return -EINVAL;
	if (offset >= inode->i_size || length == 0)
		return 0;
	if ((uint64_t)length > (uint64_t)inode->i_size - (uint64_t)offset)
		length = (size_t)((uint64_t)inode->i_size - (uint64_t)offset);
	scratch = NULL;
	done = 0;

	/* Uses caller storage for complete blocks in each validated mapped run. */
	while (done < length) {
		position = (uint64_t)offset + done;
		logical = position / super->bsize;
		within = (size_t)(position % super->bsize);
		error = bmap(inode, logical, &fragment);
		if (error != 0) {
			kern_free(scratch);
			return done != 0 ? (ssize_t)done : -error;
		}
		mapping_error = 0;
		amount = within == 0 ? content_run_bytes(inode, logical, fragment,
		    length - done, 0, &mapping_error) : 0;
		if (amount != 0) {
			io_stats_record(IO_UFS_CONTENT_READ, amount);
			error = observed_disk_read(inode->i_mount->m_disk,
			    (uint64_t)fragment << super->fsbtodb,
			    (uint32_t)(amount / UFS_SECTOR_SIZE), (uint8_t *)buffer + done);
		} else {
			/* Allocates edge scratch only when the request needs it. */
			if (scratch == NULL) {
				scratch = kern_malloc(super->bsize);
				if (scratch == NULL)
					return done != 0 ? (ssize_t)done : -ENOMEM;
			}
			amount = super->bsize - within;
			if (amount > length - done)
				amount = length - done;
			error = read_content_block(inode->i_mount, fragment, scratch);
			if (error == 0)
				memcpy((uint8_t *)buffer + done, scratch + within, amount);
		}
		if (error != 0) {
			kern_free(scratch);
			return done != 0 ? (ssize_t)done : -error;
		}
		done += amount;
		if (mapping_error != 0)
			break;
	}
	kern_free(scratch);

	/* Reports the successfully read prefix. */
	return (ssize_t)done;
}

/* Locates the shared on-disk block containing a dinode. */
static uint64_t
inode_fragment(struct inode *inode)
{
	struct ufs_mount_state *ms = state(inode->i_mount);
	uint32_t number = (uint32_t)inode->i_ino;
	uint32_t cg = number / ms->super.ipg;
	uint32_t index = number % ms->super.ipg;

	return cgstart(&ms->super, cg) + ms->super.iblkno +
	    (index / ms->super.inopb) * ms->super.frag;
}

/* Patches only one prepared dinode into a caller-owned shared block image. */
static void
encode_inode_locked(struct inode *inode, uint8_t *block)
{
	struct ufs_inode_info *ui = info(inode);
	struct ufs_mount_state *ms = state(inode->i_mount);
	uint32_t index = (uint32_t)inode->i_ino % ms->super.ipg;
	uint8_t *raw;
	unsigned n;

	raw=block+(index%ms->super.inopb)*UFS_DINODE_SIZE;
	ufs_put16(raw,UFS_DI_MODE,(uint16_t)inode->i_mode,ms->super.swapped);
	ufs_put16(raw,UFS_DI_NLINK,(uint16_t)inode->i_linkcount,ms->super.swapped);
	ufs_put64(raw,UFS_DI_SIZE,(uint64_t)inode->i_size,ms->super.swapped);
	ufs_put64(raw,UFS_DI_ATIME,(uint64_t)inode->i_atime.tv_sec,
		ms->super.swapped);
	ufs_put32(raw,UFS_DI_ATIMENSEC,(uint32_t)inode->i_atime.tv_nsec,
		ms->super.swapped);
	ufs_put64(raw,UFS_DI_MTIME,(uint64_t)inode->i_mtime.tv_sec,
		ms->super.swapped);
	ufs_put32(raw,UFS_DI_MTIMENSEC,(uint32_t)inode->i_mtime.tv_nsec,
		ms->super.swapped);
	ufs_put64(raw,UFS_DI_CTIME,(uint64_t)inode->i_ctime.tv_sec,
		ms->super.swapped);
	ufs_put32(raw,UFS_DI_CTIMENSEC,(uint32_t)inode->i_ctime.tv_nsec,
		ms->super.swapped);
	ufs_put32(raw,UFS_DI_EXTSIZE,ui->extattr_size,ms->super.swapped);
	for(n=0;n<UFS_NXADDR;n++)
		ufs_put64(raw,UFS_DI_EXTB+n*8U,ui->extattr[n],
		    ms->super.swapped);
	if(inode->i_type==INODE_CHAR||inode->i_type==INODE_BLOCK) {
		memset(raw+UFS_DI_DB,0,120U);
		ufs_put64(raw,UFS_DI_DB,(uint64_t)inode->i_rdev,
			ms->super.swapped);
		for(n=0;n<UFS_NIADDR;n++)
			ufs_put64(raw,UFS_DI_IB+n*8U,0,ms->super.swapped);
	} else if(inode->i_type==INODE_SYMLINK&&
	    (uint64_t)inode->i_size<=ms->super.maxsymlinklen&&
	    inode->i_size<=120) {
		memset(raw+UFS_DI_DB,0,120U);
		memcpy(raw+UFS_DI_DB,ui->shortlink,(size_t)inode->i_size);
	} else {
		for(n=0;n<UFS_NDADDR;n++)ufs_put64(raw,UFS_DI_DB+n*8U,ui->direct[n],ms->super.swapped);
		for(n=0;n<UFS_NIADDR;n++)ufs_put64(raw,UFS_DI_IB+n*8U,ui->indirect[n],ms->super.swapped);
	}
	ufs_put64(raw,UFS_DI_BLOCKS,ui->blocks,ms->super.swapped);
	ufs_put32(raw,UFS_DI_UID,inode->i_uid,ms->super.swapped);ufs_put32(raw,UFS_DI_GID,inode->i_gid,ms->super.swapped);
}

/* Loads shared bytes once before encoding a private or public inode image. */
static int
prepare_inode_locked(struct inode *inode, uint8_t *block, uint64_t *location)
{
	int error;

	*location = inode_fragment(inode);
	error = read_block(inode->i_mount, *location, block);
	if (error != 0)
		return error;
	encode_inode_locked(inode, block);
	return 0;
}

/* Writes one prepared shared-dinode image while preserving mount exclusion. */
static int
persist_inode_locked(struct inode *inode)
{
	uint8_t *block;
	uint64_t fragment;
	int error;

	block = kern_malloc(state(inode->i_mount)->super.bsize);
	if (block == NULL)
		return ENOMEM;
	error = prepare_inode_locked(inode, block, &fragment);
	if (error == 0)
		error = write_block(inode->i_mount, fragment, block);
	kern_free(block);
	return error;
}

/* Protects shared dinode blocks for ordinary metadata callers. */
static int
persist_inode(
	struct inode *inode)
{
	struct ufs_mount_state *ms;
	int error;

	/* Serialize the complete shared-block read/modify/write operation. */
	ms = state(inode->i_mount);
	mutex_lock(&ms->lock);
	error = persist_inode_locked(inode);
	mutex_unlock(&ms->lock);

	/* Report the original serialization result. */
	return error;
}

#include "ufs-transaction.inc"
#include "ufs-allocation.inc"
#include "ufs-xattr-release.inc"
#include "ufs-xattr-allocation.inc"

/* Batches existing full blocks while preserving allocation and size publication. */
static ssize_t
pwrite_inode_context(
	struct inode *inode,
	const void *buffer,
	size_t length,
	off_t offset,
	const struct io_context *context)
{
	struct ufs_mount_state *ms;
	uint8_t *scratch;
	size_t done;
	size_t within;
	size_t amount;
	size_t eligible;
	uint64_t position;
	uint64_t logical;
	uint64_t fragment;
	int error;
	int final_error;
	int mapping_error;
	int metadata_dirty;
	ssize_t allocated;

	/* Validates the request before taking the inode's mutation lock. */
	ms = state(inode->i_mount);
	if (!ms->writable)
		return -EROFS;
	if (offset < 0 || (uint64_t)offset + length < (uint64_t)offset)
		return -EINVAL;
	if ((uint64_t)offset + length > ms->super.maxfilesize ||
	    (uint64_t)offset + length > (sizeof(off_t) == 8 ? INT64_MAX : INT32_MAX))
		return -EFBIG;
	scratch = NULL;
	done = 0;
	final_error = 0;
	metadata_dirty = 0;
	mutex_lock(&inode->i_lock);

	/* Limits direct runs to already published file bytes and allocated blocks. */
	while (done < length) {
		position = (uint64_t)offset + done;
		logical = position / ms->super.bsize;
		within = (size_t)(position % ms->super.bsize);
		error = bmap(inode, logical, &fragment);
		if (error != 0) {
			final_error = error;
			break;
		}
		/* Initialize new full blocks with a private allocation/publication batch. */
		if (fragment == 0 && within == 0) {
			allocated = allocation_write_run(inode, (const uint8_t *)buffer + done,
			    length - done, logical, context);
			if (allocated < 0) {
				final_error = (int)-allocated;
				break;
			}
			if (allocated > 0) {
				done += (size_t)allocated;
				metadata_dirty = 0;
				continue;
			}
		}
		eligible = 0;
		if (within == 0 && position < (uint64_t)inode->i_size) {
			eligible = length - done;
			if (eligible > (uint64_t)inode->i_size - position)
				eligible = (size_t)((uint64_t)inode->i_size - position);
		}
		amount = content_run_bytes(inode, logical, fragment, eligible, 1, &mapping_error);
		if (amount != 0) {
			io_stats_record(IO_UFS_CONTENT_WRITE, amount);
			error = write_sectors_context(inode->i_mount,
			    (uint64_t)fragment << ms->super.fsbtodb,
			    (uint32_t)(amount / UFS_SECTOR_SIZE),
			    (const uint8_t *)buffer + done, context);
		} else {
			/* Keeps the established allocation/zero and partial-block path. */
			if (scratch == NULL) {
				scratch = kern_malloc(ms->super.bsize);
				if (scratch == NULL) {
					final_error = ENOMEM;
					break;
				}
			}
			amount = ms->super.bsize - within;
			if (amount > length - done)
				amount = length - done;
			if (fragment == 0) {
				error = bmap_ensure(inode, logical, &fragment);
				if (error != 0) {
					final_error = error;
					break;
				}
				memset(scratch, 0, ms->super.bsize);
			} else if (within != 0 || amount != ms->super.bsize) {
				error = read_content_block(inode->i_mount, fragment, scratch);
				if (error != 0) {
					final_error = error;
					break;
				}
			}
			memcpy(scratch + within, (const uint8_t *)buffer + done, amount);
			error = write_content_context(inode->i_mount, fragment, scratch, context);
		}
		if (error != 0) {
			final_error = error;
			break;
		}
		done += amount;
		metadata_dirty = 1;
		if ((uint64_t)inode->i_size < (uint64_t)offset + done)
			inode->i_size = (off_t)((uint64_t)offset + done);
		if (mapping_error != 0) {
			final_error = mapping_error;
			break;
		}
	}

	/* Preserves the existing metadata publication and partial-result convention. */
	if (done != 0 && metadata_dirty) {
		error = persist_inode(inode);
		if (error != 0 && final_error == 0)
			final_error = error;
	}
	mutex_unlock(&inode->i_lock);
	kern_free(scratch);

	/* Reports only the completed prefix, or the first error without progress. */
	return done != 0 ? (ssize_t)done : -final_error;
}

static uint64_t
indirect_span(const struct ufs_super *super,unsigned depth)
{
	uint64_t span=1;
	while(depth--!=0)
		span*=super->nindir;
	return span;
}

#include "ufs-release.inc"

/* Detach one inode-owned pointer durably before making its block reusable.
 * On uncertain metadata I/O keep the allocation and stop further mutations. */
static int
detach_inode_block(struct inode *inode, uint64_t *pointer)
{
	struct ufs_mount_state *ms = state(inode->i_mount);
	struct ufs_inode_info *ui = info(inode);
	uint64_t fragment = *pointer;
	unsigned sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	unsigned index;
	int error;
	int handled;

	if (fragment == 0)
		return 0;
	if (ui->blocks < sectors)
		return EIO;
	for (index = 0; index < UFS_NDADDR; index++) {
		if (pointer == &ui->direct[index])
			break;
	}
	if (index == UFS_NDADDR) {
		for (index = 0; index < UFS_NIADDR; index++) {
			if (pointer == &ui->indirect[index])
				break;
		}
		index += UFS_NDADDR;
	}
	error = release_group(inode, 0, index, fragment, &handled);
	if (handled)
		return error;
	*pointer = 0;
	ui->blocks -= sectors;
	error = persist_inode(inode);
	if (error == 0)
		error = disk_sync(inode->i_mount->m_disk);
	if (error == 0)
		error = free_block(inode->i_mount, fragment,inode->i_uid,inode->i_gid);
	if (error != 0)
		ms->writable = 0;
	return error;
}

/* Leave an empty root allocated until its caller has detached the owning
 * pointer. A child is never freed while its parent still names it on disk. */
static int
truncate_indirect(struct inode *inode, uint64_t root, unsigned depth,
	uint64_t base, uint64_t keep, int *empty)
{
	struct ufs_mount_state *ms = state(inode->i_mount);
	struct ufs_inode_info *ui = info(inode);
	uint8_t *block;
	uint64_t child_span = indirect_span(&ms->super, depth - 1U);
	unsigned index, sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	int error = 0;

	*empty = 1;
	if (root == 0)
		return 0;
	block = kern_malloc(ms->super.bsize);
	if (block == NULL)
		return ENOMEM;
	error = read_block(inode->i_mount, root, block);
	if (error != 0)
		goto out;
	for (index = 0; index < ms->super.nindir; index++) {
		uint64_t child = ufs_get64(block, (size_t)index * 8U,
		    ms->super.swapped);
		uint64_t child_base = base + (uint64_t)index * child_span;
		int remove = 0;
		int handled;

		if (child == 0)
			continue;
		if (depth == 1U) {
			remove = child_base >= keep;
		} else if (child_base + child_span > keep) {
			error = truncate_indirect(inode, child, depth - 1U,
			    child_base, keep, &remove);
			if (error != 0)
				goto out;
		}
		if (!remove) {
			*empty = 0;
			continue;
		}
		if (ui->blocks < sectors) { error = EIO; goto out; }
		error = release_group(inode, root, index, child, &handled);
		if (handled) {
			if (error != 0)
				goto out;
			ufs_put64(block, (size_t)index * 8U, 0, ms->super.swapped);
			continue;
		}
		ufs_put64(block, (size_t)index * 8U, 0, ms->super.swapped);
		error = write_block(inode->i_mount, root, block);
		if (error == 0)
			error = disk_sync(inode->i_mount->m_disk);
		if (error == 0) {
			ui->blocks -= sectors;
			error = free_block(inode->i_mount, child,inode->i_uid,inode->i_gid);
		}
		if (error != 0) {
			ms->writable = 0;
			goto out;
		}
	}
out:
	kern_free(block);
	return error;
}

static int
ufs_truncate(struct inode *inode, off_t size)
{
	struct ufs_mount_state *ms = state(inode->i_mount);
	struct ufs_inode_info *ui = info(inode);
	uint8_t *block = NULL;
	uint64_t keep, base;
	unsigned n;
	int error = 0;

	if (!ms->writable)
		return EROFS;
	if (size < 0 || (uint64_t)size > ms->super.maxfilesize)
		return EFBIG;
	mutex_lock(&inode->i_lock);
	keep = ((uint64_t)size + ms->super.bsize - 1U) / ms->super.bsize;
	if (size < inode->i_size && size != 0 && size % ms->super.bsize != 0) {
		uint64_t fragment = 0;
		error = bmap(inode, (uint64_t)size / ms->super.bsize, &fragment);
		if (error != 0)
			goto out;
		if (fragment != 0) {
			block = kern_malloc(ms->super.bsize);
			if (block == NULL) { error = ENOMEM; goto out; }
			error = read_content_block(inode->i_mount, fragment, block);
			if (error != 0)
				goto out;
			memset(block + size % ms->super.bsize, 0,
			    ms->super.bsize - size % ms->super.bsize);
			error = write_content_block(inode->i_mount, fragment, block);
			if (error != 0)
				goto out;
		}
	}
	/* Bound before narrowing: a large sparse size must not wrap to a
	 * direct-block index and release unrelated data. */
	for (n = 0; n < UFS_NDADDR; n++) {
		if ((uint64_t)n < keep)
			continue;
		error = detach_inode_block(inode, &ui->direct[n]);
		if (error != 0)
			goto out;
	}
	base = UFS_NDADDR;
	for (n = 0; n < UFS_NIADDR; n++) {
		int empty;
		error = truncate_indirect(inode, ui->indirect[n], n + 1U,
		    base, keep, &empty);
		if (error == 0 && empty)
			error = detach_inode_block(inode, &ui->indirect[n]);
		if (error != 0)
			goto out;
		base += indirect_span(&ms->super, n + 1U);
	}
	inode->i_size = size;
	error = persist_inode(inode);
out:
	kern_free(block);
	mutex_unlock(&inode->i_lock);
	return error;
}

static enum inode_type
mode_type(uint16_t mode)
{
	switch (mode & UFS_IFMT) {
	case UFS_IFREG: return INODE_REG; case UFS_IFDIR: return INODE_DIR;
	case UFS_IFLNK: return INODE_SYMLINK; case UFS_IFCHR: return INODE_CHAR;
	case UFS_IFBLK: return INODE_BLOCK; case UFS_IFIFO: return INODE_FIFO;
	case UFS_IFSOCK: return INODE_SOCKET; default: return INODE_NONE;
	}
}

/* Rejects disk values that the active VFS ABI cannot represent. */
static int
inode_size_values(
	const uint8_t *raw,
	const struct ufs_super *super,
	uint64_t *size,
	uint64_t *blocks)
{
	uint64_t disk_size, disk_blocks;

	disk_size = ufs_get64(raw, UFS_DI_SIZE, super->swapped);
	disk_blocks = ufs_get64(raw, UFS_DI_BLOCKS, super->swapped);
	if (disk_size > (sizeof(off_t) == 8 ? (uint64_t)INT64_MAX : (uint64_t)INT32_MAX))
		return EFBIG;
	if (disk_blocks > (sizeof(blkcnt_t) == 8 ? (uint64_t)INT64_MAX : (uint64_t)INT32_MAX))
		return EOVERFLOW;
	*size = disk_size;
	*blocks = disk_blocks;
	return 0;
}

static int decode_inode_raw(struct inode *inode, const uint8_t *raw, uint32_t number, int orphan);

/* Decodes one private identity, keeping namespace and recovery admission distinct. */
static int
decode_inode_raw(struct inode *inode, const uint8_t *raw, uint32_t number, int orphan)
{
	const struct ufs_super *s = &state(inode->i_mount)->super;
	struct ufs_inode_info *ui;
	uint64_t disk_size, disk_blocks;
	uint16_t mode;
	unsigned n;
	int error;

	/* Validates the disk representation before populating the private inode. */
	mode = ufs_get16(raw, UFS_DI_MODE, s->swapped);
	if (mode_type(mode) == INODE_NONE)
		return EOPNOTSUPP;
	error = inode_size_values(raw, s, &disk_size, &disk_blocks);
	if (error != 0)
		return error;
	ui = info(inode);
	inode->i_type = mode_type(mode); inode->i_ino = number;
	inode->i_mode = mode; inode->i_linkcount = ufs_get16(raw,UFS_DI_NLINK,s->swapped);
	inode->i_size = (off_t)disk_size;
	inode->i_uid = ufs_get32(raw,UFS_DI_UID,s->swapped);
	inode->i_gid = ufs_get32(raw,UFS_DI_GID,s->swapped);
	inode->i_atime.tv_sec = (time_t)ufs_get64(raw,UFS_DI_ATIME,s->swapped);
	inode->i_atime.tv_nsec = ufs_get32(raw,UFS_DI_ATIMENSEC,s->swapped);
	inode->i_mtime.tv_sec = (time_t)ufs_get64(raw,UFS_DI_MTIME,s->swapped);
	inode->i_mtime.tv_nsec = ufs_get32(raw,UFS_DI_MTIMENSEC,s->swapped);
	inode->i_ctime.tv_sec = (time_t)ufs_get64(raw,UFS_DI_CTIME,s->swapped);
	inode->i_ctime.tv_nsec = ufs_get32(raw,UFS_DI_CTIMENSEC,s->swapped);
	ui->extattr_size=ufs_get32(raw,UFS_DI_EXTSIZE,s->swapped);
	for(n=0;n<UFS_NXADDR;n++)
		ui->extattr[n]=ufs_get64(raw,UFS_DI_EXTB+n*8U,s->swapped);
	if(inode->i_type==INODE_CHAR||inode->i_type==INODE_BLOCK) {
		inode->i_rdev=(dev_t)ufs_get64(raw,UFS_DI_DB,s->swapped);
	} else if(inode->i_type==INODE_SYMLINK&&(uint64_t)inode->i_size<=
	    s->maxsymlinklen&&inode->i_size<=120) {
		memcpy(ui->shortlink,raw+UFS_DI_DB,sizeof(ui->shortlink));
	} else {
		for (n=0;n<UFS_NDADDR;n++)
			ui->direct[n]=ufs_get64(raw,UFS_DI_DB+n*8U,s->swapped);
		for (n=0;n<UFS_NIADDR;n++)
			ui->indirect[n]=ufs_get64(raw,UFS_DI_IB+n*8U,s->swapped);
	}
	ui->disk_flags=ufs_get32(raw,UFS_DI_FLAGS,s->swapped);
	ui->blocks=disk_blocks;
	ui->generation=ufs_get32(raw,UFS_DI_GEN,s->swapped);
	if ((orphan ? inode->i_linkcount != 0 : inode->i_linkcount == 0) || inode->i_size < 0 ||
	    (uint64_t)inode->i_size > s->maxfilesize ||
	    ui->extattr_size > UFS_NXADDR*s->bsize ||
	    inode->i_atime.tv_nsec >= 1000000000L ||
	    inode->i_mtime.tv_nsec >= 1000000000L ||
	    inode->i_ctime.tv_nsec >= 1000000000L ||
	    (inode->i_type == INODE_DIR &&
	    ((!orphan && (uint64_t)inode->i_size < UFS_DIRBLKSIZ) ||
	    (uint64_t)inode->i_size % UFS_DIRBLKSIZ != 0))) {
		return EIO;
	}
	for (n=0;n<UFS_NXADDR;n++) {
		int needed=ui->extattr_size>n*s->bsize;
		if ((needed&&ui->extattr[n]==0)||
		    (!needed&&ui->extattr[n]!=0)||
		    (needed&&!valid_inode_fragment(s,ui->extattr[n]))) {
			return EIO;
		}
	}
	if (!(inode->i_type == INODE_SYMLINK &&
	    (uint64_t)inode->i_size <= s->maxsymlinklen &&
	    inode->i_size <= 120)) {
		for (n = 0; n < UFS_NDADDR; n++)
			if (!valid_inode_fragment(s, ui->direct[n])) {
				return EIO;
			}
		for (n = 0; n < UFS_NIADDR; n++)
			if (!valid_inode_fragment(s, ui->indirect[n])) {
				return EIO;
			}
	}
	inode->i_op=&ufs_inode_ops;
	inode->i_fop=inode->i_type==INODE_DIR?&ufs_directory_ops:
		inode->i_type==INODE_REG?&ufs_regular_ops:
		inode->i_type==INODE_FIFO?&fifo_file_ops:NULL;

	/* Returns a validated identity without adding it to the inode cache. */
	return 0;
}

static int
load_inode_locked(struct mount *mountp, uint32_t number, struct inode **result)
{
	const struct ufs_super *s = &state(mountp)->super;
	struct inode *inode;
	uint8_t *block, *raw;
	uint32_t cg, index;
	uint64_t fragment, disk_size, disk_blocks;
	uint16_t mode;
	int error;
	if (number < UFS_ROOT_INO || number >= s->ncg * s->ipg)
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
	raw = block + (index % s->inopb) * UFS_DINODE_SIZE;
	mode = ufs_get16(raw, UFS_DI_MODE, s->swapped);
	if (mode_type(mode) == INODE_NONE) { kern_free(block); return EOPNOTSUPP; }
	error = inode_size_values(raw, s, &disk_size, &disk_blocks);
	if (error != 0) {
		kern_free(block);
		return error;
	}
	inode = inode_alloc(mountp);
	if (inode == NULL) { kern_free(block); return ENOSPC; }
	error = decode_inode_raw(inode, raw, number, 0);
	if (error != 0) {
		inode->i_flags |= INODE_DEAD;
		inode_release(inode);
		kern_free(block);
		return error;
	}
	kern_free(block); *result=inode; return 0;
}

/* Creation already holds this gate. Miss, allocation and initialization must
 * form one admission so aliases cannot publish duplicate in-core objects. */
static int
load_inode(struct mount *mountp, uint32_t number, struct inode **result)
{
	struct mutex *gate = &state(mountp)->namespace_lock;
	int entered = !mutex_owned(gate), error;
	if (entered)
		mutex_lock(gate);
	error = load_inode_locked(mountp, number, result);
	if (entered)
		mutex_unlock(gate);
	return error;
}

static int
next_dirent(struct inode *directory, off_t *cursor, uint32_t *number,
	uint8_t *type, char name[NAME_MAX+1U])
{
	struct ufs_mount_state *ms = state(directory->i_mount);
	uint8_t head[8];
	int error = 0;

	/* Refuses namespace bytes after unresolved journal I/O invalidated cache state. */
	if (ms->journal_enabled) {
		mutex_lock(&ms->journal_lock);
		if (ms->journal.poisoned)
			error = EIO;
		mutex_unlock(&ms->journal_lock);
	}
	if (error != 0)
		return error;
	while (*cursor < directory->i_size) {
		uint16_t reclen; uint8_t namelen; ssize_t count;
		if ((uint64_t)*cursor % UFS_DIRBLKSIZ > UFS_DIRBLKSIZ-8U) return EIO;
		count=pread_inode(directory,head,sizeof(head),*cursor);
		if (count!=sizeof(head)) return EIO;
		*number=ufs_get32(head,0,state(directory->i_mount)->super.swapped);
		reclen=ufs_get16(head,4,state(directory->i_mount)->super.swapped);
		*type=head[6]; namelen=head[7];
		if (reclen<8U || (reclen&3U)!=0 ||
		    8U+namelen>reclen || (uint64_t)*cursor%UFS_DIRBLKSIZ+reclen>UFS_DIRBLKSIZ ||
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
	struct ufs_mount_state *ms=state(directory->i_mount);
	int rollback=write_block(directory->i_mount,fragment,original);
	if(rollback!=0) {
		ms->writable=0;
		return rollback;
	}
	return original_error;
}

static int
dir_find_record(struct inode *directory,const struct componentname *name,
	uint8_t *block,uint32_t *offset,uint32_t *previous,uint32_t *number)
{
	struct ufs_mount_state *ms=state(directory->i_mount);uint32_t pos=0,prev=UINT32_MAX;int error;
	if(directory->i_size<0 || (uint64_t)directory->i_size>ms->super.bsize ||
	    (uint64_t)directory->i_size%UFS_DIRBLKSIZ!=0 ||
	    info(directory)->direct[0]==0)
		return EIO;
	error=read_block(directory->i_mount,info(directory)->direct[0],block);if(error)return error;
	while(pos<(uint32_t)directory->i_size){if((uint32_t)directory->i_size-pos<8U||pos%UFS_DIRBLKSIZ>UFS_DIRBLKSIZ-8U){return EIO;}uint32_t ino=ufs_get32(block,pos,ms->super.swapped);uint16_t reclen=ufs_get16(block,pos+4U,ms->super.swapped);uint8_t nlen=block[pos+7U];
		if(reclen<8U||(reclen&3U)!=0||pos%UFS_DIRBLKSIZ+reclen>UFS_DIRBLKSIZ||pos+reclen>(uint32_t)directory->i_size||8U+nlen>reclen)return EIO;
		if(ino!=0&&nlen==name->cn_namelen&&memcmp(block+pos+8U,name->cn_nameptr,nlen)==0){*offset=pos;*previous=prev;*number=ino;return 0;}
		prev=pos;pos+=reclen;
	}
	return ENOENT;
}

static int
dir_add(struct inode *directory,const struct componentname *name,uint32_t number,uint8_t type)
{
	struct ufs_mount_state *ms=state(directory->i_mount);struct ufs_inode_info *ui=info(directory);uint8_t *block,*original;uint16_t need;uint32_t pos=0;uint64_t old_direct,allocated=0,old_blocks;off_t old_size;int error,rollback;
	int handled;
	if(name->cn_namelen==0||name->cn_namelen>255U)
		return EINVAL;
	for(pos=0;pos<name->cn_namelen;pos++)
		if(name->cn_nameptr[pos]=='/')
			return EINVAL;
	pos=0;
	need=dir_minimum((uint8_t)name->cn_namelen);block=kern_calloc(1,ms->super.bsize);original=kern_malloc(ms->super.bsize);if(block==NULL||original==NULL){kern_free(block);kern_free(original);return ENOMEM;}
	mutex_lock(&directory->i_lock);
	old_size=directory->i_size;old_direct=ui->direct[0];old_blocks=ui->blocks;
	if(old_size<0 || (uint64_t)old_size>ms->super.bsize ||
	    (uint64_t)old_size%UFS_DIRBLKSIZ!=0) {
		error=EIO;goto out;
	}
	if(ui->direct[0]==0) {
		error=directory_backing_group(directory,&handled);
		if(handled) {
			if(error!=0)goto out;
			/* Retain committed empty backing if later entry publication fails. */
			old_direct=ui->direct[0];
			old_blocks=ui->blocks;
		} else {
			error=allocate_block(directory->i_mount,directory->i_uid,directory->i_gid,&ui->direct[0]);
			if(error)goto out;
			allocated=ui->direct[0];
			ui->blocks+=ms->super.bsize/UFS_SECTOR_SIZE;
		}
	}
	error=read_block(directory->i_mount,ui->direct[0],block);if(error)goto out;
	memcpy(original,block,ms->super.bsize);
	while(pos<(uint32_t)directory->i_size){if((uint32_t)directory->i_size-pos<8U||pos%UFS_DIRBLKSIZ>UFS_DIRBLKSIZ-8U){error=EIO;goto out;}uint16_t reclen=ufs_get16(block,pos+4U,ms->super.swapped);uint8_t nlen=block[pos+7U];uint16_t minimum=dir_minimum(nlen);
		if(reclen<minimum||(reclen&3U)!=0||pos%UFS_DIRBLKSIZ+reclen>UFS_DIRBLKSIZ||reclen>(uint32_t)directory->i_size-pos){error=EIO;goto out;}
		if(reclen-minimum>=need){uint32_t at=pos+minimum;ufs_put16(block,pos+4U,minimum,ms->super.swapped);ufs_put32(block,at,number,ms->super.swapped);ufs_put16(block,at+4U,reclen-minimum,ms->super.swapped);block[at+6U]=type;block[at+7U]=(uint8_t)name->cn_namelen;memcpy(block+at+8U,name->cn_nameptr,name->cn_namelen);error=write_block(directory->i_mount,ui->direct[0],block);goto commit;}
		pos+=reclen;
	}
	if((uint64_t)directory->i_size+UFS_DIRBLKSIZ>ms->super.bsize){error=ENOSPC;goto out;}
	pos=(uint32_t)directory->i_size;ufs_put32(block,pos,number,ms->super.swapped);ufs_put16(block,pos+4U,UFS_DIRBLKSIZ,ms->super.swapped);block[pos+6U]=type;block[pos+7U]=(uint8_t)name->cn_namelen;memcpy(block+pos+8U,name->cn_nameptr,name->cn_namelen);directory->i_size+=UFS_DIRBLKSIZ;error=write_block(directory->i_mount,ui->direct[0],block);
commit:	if(error==0)error=persist_inode(directory);
	if(error!=0){
		rollback=restore_directory_block(directory,ui->direct[0],original,error);
		directory->i_size=old_size;ui->direct[0]=old_direct;ui->blocks=old_blocks;
		error=persist_inode(directory);
		if(error==0)error=disk_sync(directory->i_mount->m_disk);
		if(error!=0) {
			ms->writable=0;
		} else if(ms->writable&&allocated!=0) {
			error=free_block(directory->i_mount,allocated,
				directory->i_uid,directory->i_gid);
			if(error!=0)ms->writable=0;
		}
		if(error==0)error=rollback;
	}
out:	if(error!=0&&allocated!=0&&ui->direct[0]==allocated){
		directory->i_size=old_size;ui->direct[0]=old_direct;ui->blocks=old_blocks;
		rollback=persist_inode(directory);
		if(rollback==0)rollback=disk_sync(directory->i_mount->m_disk);
		if(rollback!=0) {
			ms->writable=0;
			error=rollback;
		} else {
			rollback=free_block(directory->i_mount,allocated,
				directory->i_uid,directory->i_gid);
			if(rollback!=0){ms->writable=0;error=rollback;}
		}
	}
	mutex_unlock(&directory->i_lock);kern_free(original);kern_free(block);return error;
}

static int
dir_remove(struct inode *directory,const struct componentname *name,uint32_t *number)
{
	struct ufs_mount_state *ms=state(directory->i_mount);
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
		uint16_t reclen=ufs_get16(block,offset+4U,
			ms->super.swapped);
		if(previous!=UINT32_MAX &&
		    previous/UFS_DIRBLKSIZ==offset/UFS_DIRBLKSIZ) {
			uint16_t prior=ufs_get16(block,previous+4U,
				ms->super.swapped);
			ufs_put16(block,previous+4U,prior+reclen,
				ms->super.swapped);
		} else {
			ufs_put32(block,offset,0,ms->super.swapped);
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
	struct ufs_mount_state *ms=state(directory->i_mount);
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
		ufs_put32(block,offset,number,ms->super.swapped);
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

static void
detach_new_socket_special(struct inode *inode)
{
	/* The pathname socket endpoint in a creation request is borrowed.  On
	 * any failed publication, detach it before releasing a possibly cached
	 * inode.  This also makes a name retained by an unsuccessful directory
	 * rollback inert instead of exposing a future dangling endpoint.
	 */
	if(inode==NULL)
		return;
	mutex_lock(&inode->i_lock);
	if(inode->i_type==INODE_SOCKET) {
		inode->i_special=NULL;
		inode->i_special_destroy=NULL;
	}
	mutex_unlock(&inode->i_lock);
}

static int
discard_new_inode(struct inode *inode,int directory_counted)
{
	struct ufs_mount_state *ms=state(inode->i_mount);
	struct ufs_inode_info *ui=info(inode);
	uint64_t block=ui->direct[0],extattr[UFS_NXADDR];
	uint32_t number=(uint32_t)inode->i_ino,old_extattr_size=ui->extattr_size;
	mode_t old_mode=inode->i_mode;
	enum inode_type old_type=inode->i_type;
	nlink_t old_links=inode->i_linkcount;
	off_t old_size=inode->i_size;
	uint64_t old_blocks=ui->blocks;
	uid_t uid=inode->i_uid;
	gid_t gid=inode->i_gid;
	unsigned n;
	int error=0,cleanup;

	if(ms->journal_enabled && ms->journal.sector_count>2U &&
	    (2U*ms->super.bsize+UFS_SBLOCK_SIZE)/UFS_SECTOR_SIZE<=UFS_JOURNAL_GROUP_SECTORS &&
	    (2U*ms->super.bsize+UFS_SBLOCK_SIZE)/UFS_SECTOR_SIZE<=ms->journal.sector_count-2U &&
	    (inode->i_type!=INODE_DIR || directory_counted))
		return discard_reserved_inode(inode);
	detach_new_socket_special(inode);
	for(n=1;n<UFS_NDADDR;n++)
		if(ui->direct[n]!=0){ms->writable=0;inode_release(inode);return EIO;}
	for(n=0;n<UFS_NIADDR;n++)
		if(ui->indirect[n]!=0){ms->writable=0;inode_release(inode);return EIO;}
	for(n=0;n<UFS_NXADDR;n++){extattr[n]=ui->extattr[n];ui->extattr[n]=0;}
	inode->i_mode=0;
	inode->i_type=INODE_NONE;
	inode->i_linkcount=0;
	inode->i_size=0;
	ui->direct[0]=0;
	ui->extattr_size=0;
	ui->blocks=0;
	cleanup=persist_inode(inode);
	if(cleanup==0)cleanup=disk_sync(inode->i_mount->m_disk);
	if(cleanup!=0) {
		inode->i_mode=old_mode;
		inode->i_type=old_type;
		inode->i_linkcount=old_links;
		inode->i_size=old_size;
		ui->direct[0]=block;
		ui->extattr_size=old_extattr_size;
		for(n=0;n<UFS_NXADDR;n++)ui->extattr[n]=extattr[n];
		ui->blocks=old_blocks;
		ms->writable=0;
		inode_release(inode);
		return cleanup;
	}
	if(directory_counted) {
		cleanup=adjust_directory_count(inode->i_mount,number,-1);
		if(error==0&&cleanup!=0)error=cleanup;
	}
	if(block!=0) {
		cleanup=free_block(inode->i_mount,block,uid,gid);
		if(error==0&&cleanup!=0)error=cleanup;
	}
	for(n=0;n<UFS_NXADDR;n++)
		if(extattr[n]!=0) {
			cleanup=free_block(inode->i_mount,extattr[n],uid,gid);
			if(error==0&&cleanup!=0)error=cleanup;
		}
	cleanup=free_inode_number(inode->i_mount,number,uid,gid);
	if(error==0&&cleanup!=0)error=cleanup;
	if(error!=0)
		ms->writable=0;
	inode->i_ino=0;
	inode->i_flags|=INODE_DEAD;
	inode_release(inode);
	return error;
}

static int
discard_new_inode_after_error(struct inode *inode,int directory_counted,
	int original_error)
{
	struct ufs_mount_state *ms=state(inode->i_mount);
	int cleanup;

	if(!ms->writable){
		detach_new_socket_special(inode);
		inode_release(inode);
		return original_error;
	}
	cleanup=discard_new_inode(inode,directory_counted);
	return cleanup!=0?cleanup:original_error;
}

#include "ufs-inode-reservation.inc"

static int
new_inode(struct inode *directory,
	const struct inode_creation_request *request,nlink_t links,
	struct inode **result)
{
	struct mount *mountp;
	struct inode *inode;
	uint32_t number=0;
	int error;
	int grouped=0;
	int directory_counted=0;
	size_t reservation_bytes;
	struct ufs_mount_state *ms;

	if(directory==NULL||request==NULL||result==NULL)
		return EINVAL;
	*result=NULL;
	mountp=directory->i_mount;
	ms=state(mountp);
	/* Admit the complete preparation chain, including first directory backing. */
	reservation_bytes=3U*ms->super.bsize+UFS_SBLOCK_SIZE;
	grouped=ms->journal_enabled && ms->journal.sector_count>2U &&
	    reservation_bytes/UFS_SECTOR_SIZE<=UFS_JOURNAL_GROUP_SECTORS &&
	    reservation_bytes/UFS_SECTOR_SIZE<=ms->journal.sector_count-2U;
	if(grouped) {
		inode=inode_alloc(mountp);
		if(inode==NULL)return ENOSPC;
		inode->i_op=&ufs_inode_ops;
		error=reserve_inode_group(inode,request);
		if(error!=0) {
			inode->i_flags|=INODE_DEAD;
			inode_release(inode);
			return error;
		}
		number=(uint32_t)inode->i_ino;
		directory_counted=request->type==INODE_DIR;
	} else {
		error=allocate_inode_number(mountp,request->uid,request->gid,&number);

		if(error)
			return error;
		inode=inode_alloc(mountp);
		if(inode==NULL) {
			error=free_inode_number(mountp,number,request->uid,request->gid);
			if(error!=0) {
				state(mountp)->writable=0;
				return error;
			}
			return ENOSPC;
		}
	}
	inode->i_ino=number;
	inode->i_type=request->type;
	inode->i_linkcount=grouped?0:links;
	inode->i_op=&ufs_inode_ops;
	inode->i_fop=request->type==INODE_DIR?&ufs_directory_ops:
		request->type==INODE_REG?&ufs_regular_ops:
		request->type==INODE_FIFO?&fifo_file_ops:NULL;
	if(!grouped)info(inode)->generation=number;
	error=inode_creation_prepare(directory,inode,request);
	if(error!=0) {
		return discard_new_inode_after_error(inode,directory_counted,error);
	}
	error=persist_inode(inode);
	if(error) {
		return discard_new_inode_after_error(inode,directory_counted,error);
	}
	if(request->type==INODE_DIR && !directory_counted) {
		error=adjust_directory_count(mountp,number,1);
		if(error!=0)
			return discard_new_inode_after_error(inode,directory_counted,error);
	}
	*result=inode;
	return 0;
}

static int
ufs_lookup_locked(struct inode *directory,const struct componentname *component,
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
ufs_lookup(struct inode *directory, const struct componentname *component,
	struct inode **result)
{
	struct mutex *gate = &state(directory->i_mount)->namespace_lock;
	int entered = !mutex_owned(gate), error;
	if (entered)
		mutex_lock(gate);
	error = ufs_lookup_locked(directory, component, result);
	if (entered)
		mutex_unlock(gate);
	return error;
}

#include "ufs-namespace.inc"
#include "ufs-creation.inc"

static int
ufs_create(struct inode *directory,const struct componentname *name,
	const struct inode_creation_request *request,struct inode **result)
{
	struct inode *existing,*inode;
	struct ufs_mount_state *ms=state(directory->i_mount);
	int error;
	*result=NULL;
	if(!ms->writable)return EROFS;
	mutex_lock(&ms->namespace_lock);
	if(!ms->writable){error=EROFS;goto out;}
	error=ufs_lookup(directory,name,&existing);
	if(error==0){inode_release(existing);error=EEXIST;goto out;}
	if(error!=ENOENT)goto out;
	error=new_inode(directory,request,1,&inode);
	if(error)goto out;
	if(inode->i_linkcount==0) {
		error=creation_publish(directory,name,inode,result);
		goto out;
	}
	error=dir_add(directory,name,(uint32_t)inode->i_ino,8);
	if(error){error=discard_new_inode_after_error(inode,0,error);goto out;}
	*result=inode;
out:
	mutex_unlock(&ms->namespace_lock);
	return error;
}

static int
ufs_mkdir(struct inode *directory,const struct componentname *name,
	const struct inode_creation_request *request,struct inode **result)
{
	struct inode *existing,*inode;struct componentname dot={".",1,0},dotdot={"..",2,0};
	struct ufs_mount_state *ms=state(directory->i_mount);
	uint32_t removed=0;
	nlink_t old_directory_links;
	int error,rollback_error;
	*result=NULL;
	if(!ms->writable)return EROFS;
	mutex_lock(&ms->namespace_lock);
	if(!ms->writable){error=EROFS;goto out;}
	error=ufs_lookup(directory,name,&existing);
	if(error==0){inode_release(existing);error=EEXIST;goto out;}
	if(error!=ENOENT)goto out;
	error=new_inode(directory,request,2,&inode);if(error)goto out;
	error=dir_add(inode,&dot,(uint32_t)inode->i_ino,4);if(error==0)error=dir_add(inode,&dotdot,(uint32_t)directory->i_ino,4);
	if(error==0 && inode->i_linkcount==0) {
		error=creation_publish(directory,name,inode,result);
		goto out;
	}
	if(error==0)error=dir_add(directory,name,(uint32_t)inode->i_ino,4);
	if(error){error=discard_new_inode_after_error(inode,1,error);goto out;}
	mutex_lock(&directory->i_lock);
	old_directory_links=directory->i_linkcount;
	directory->i_linkcount++;
	error=persist_inode(directory);
	mutex_unlock(&directory->i_lock);
	if(error==0)
		*result=inode;
	else {
		int name_removed=0;

		rollback_error=dir_remove(directory,name,&removed);
		if(rollback_error==0) {
			name_removed=1;
			if(removed!=(uint32_t)inode->i_ino)
				rollback_error=EIO;
			mutex_lock(&directory->i_lock);
			directory->i_linkcount=old_directory_links;
			if(rollback_error==0)
				rollback_error=persist_inode(directory);
			mutex_unlock(&directory->i_lock);
		}
		if(!name_removed) {
			ms->writable=0;
			inode_release(inode);
		} else {
			int cleanup=discard_new_inode(inode,1);

			if(rollback_error==0&&cleanup!=0)
				rollback_error=cleanup;
			if(rollback_error!=0)
				ms->writable=0;
		}
		if(rollback_error!=0)
			error=rollback_error;
	}
out:
	mutex_unlock(&ms->namespace_lock);
	return error;
}

static int
ufs_mknod(struct inode *directory,const struct componentname *name,
	const struct inode_creation_request *request,struct inode **result)
{
	struct inode *existing,*inode;
	struct ufs_mount_state *ms=state(directory->i_mount);
	int error;
	if(request==NULL||(request->type!=INODE_FIFO&&
	    request->type!=INODE_SOCKET&&request->type!=INODE_CHAR&&
	    request->type!=INODE_BLOCK))
		return EOPNOTSUPP;
	*result=NULL;
	if(!ms->writable)
		return EROFS;
	mutex_lock(&ms->namespace_lock);
	if(!ms->writable){error=EROFS;goto out;}
	error=ufs_lookup(directory,name,&existing);
	if(error==0){inode_release(existing);error=EEXIST;goto out;}
	if(error!=ENOENT)goto out;
	error=new_inode(directory,request,1,&inode);
	if(error!=0)goto out;
	if(inode->i_linkcount==0) {
		error=creation_publish(directory,name,inode,result);
		goto out;
	}
	error=dir_add(directory,name,(uint32_t)inode->i_ino,
		dir_type(request->type));
	if(error!=0){
		error=discard_new_inode_after_error(inode,0,error);
		goto out;
	}
	*result=inode;
out:
	mutex_unlock(&ms->namespace_lock);
	return error;
}


static int
ufs_unlink(struct inode *directory,const struct componentname *name)
{
	struct ufs_mount_state *ms=state(directory->i_mount);
	struct inode *target=NULL;uint32_t number=0;int error,rollback_error;
	int removed=0;
	int handled;
	nlink_t old_links=0;
	unsigned old_flags=0;
	mutex_lock(&ms->namespace_lock);
	if(!ms->writable){error=EROFS;goto out;}
	error=ufs_lookup(directory,name,&target);
	if(error)goto out;
	if(target->i_type==INODE_DIR){error=EISDIR;goto out;}
	error=remove_group(directory,name,target,&handled);
	if(handled)goto out;
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
ufs_rmdir(struct inode *directory,const struct componentname *name)
{
	struct ufs_mount_state *ms=state(directory->i_mount);
	struct inode *target=NULL;uint32_t number=0;int empty,error,rollback_error;
	int removed=0;
	int handled;
	nlink_t old_target_links=0,old_directory_links=0;
	unsigned old_target_flags=0;
	if(name_is_dot(name))return EINVAL;
	mutex_lock(&ms->namespace_lock);
	if(!ms->writable){error=EROFS;goto out;}
	error=ufs_lookup(directory,name,&target);
	if(error)goto out;
	if(target->i_type!=INODE_DIR){error=ENOTDIR;goto out;}
	empty=directory_empty(target);
	if(empty<=0){error=empty==0?ENOTEMPTY:-empty;goto out;}
	error=remove_group(directory,name,target,&handled);
	if(handled)goto out;
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
ufs_rename(struct inode *old_directory,const struct componentname *old_name,
	struct inode *new_directory,const struct componentname *new_name,
	unsigned flags)
{
	struct ufs_mount_state *ms=state(old_directory->i_mount);
	struct inode *source=NULL,*target=NULL;
	uint32_t removed=0,replaced=0;
	uint8_t replaced_type=0;
	nlink_t old_target_links=0,old_old_directory_links=0;
	nlink_t old_new_directory_links=0;
	unsigned old_target_flags=0;
	int target_exists=0,namespace_committed=0,dotdot_changed=0;
	int empty,error,rollback_error=0;
	int handled;

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
	if(!ms->writable){error=EROFS;goto out;}
	error=ufs_lookup(old_directory,old_name,&source);
	if(error!=0)
		goto out;
	error=ufs_lookup(new_directory,new_name,&target);
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
	error=rename_group(old_directory,old_name,new_directory,new_name,source,target,&handled);
	if(handled)
		goto out;
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
ufs_link(struct inode *directory,const struct componentname *name,struct inode *target)
{
	struct ufs_mount_state *ms=state(directory->i_mount);
	struct inode *existing;uint32_t removed;int error,rollback_error;
	int handled;
	if(target==NULL||target->i_mount!=directory->i_mount)return EXDEV;
	if(target->i_type==INODE_DIR)return EPERM;
	mutex_lock(&ms->namespace_lock);
	if(!ms->writable){error=EROFS;goto out;}
	mutex_lock(&target->i_lock);
	if(target->i_linkcount==UINT16_MAX){mutex_unlock(&target->i_lock);error=EMLINK;goto out;}
	mutex_unlock(&target->i_lock);
	error=ufs_lookup(directory,name,&existing);
	if(error==0){inode_release(existing);error=EEXIST;goto out;}
	if(error!=ENOENT)goto out;
	error=link_group(directory,name,target,&handled);
	if(handled)goto out;
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
ufs_symlink(struct inode *directory,const struct componentname *name,
	const char *target,const struct inode_creation_request *request,
	struct inode **result)
{
	struct ufs_mount_state *ms=state(directory->i_mount);
	struct inode *existing,*inode;size_t length=strlen(target);int error;
	if(length>state(directory->i_mount)->super.maxsymlinklen||length>120U)return ENAMETOOLONG;
	*result=NULL;
	mutex_lock(&ms->namespace_lock);
	if(!ms->writable){error=EROFS;goto out;}
	error=ufs_lookup(directory,name,&existing);
	if(error==0){inode_release(existing);error=EEXIST;goto out;}
	if(error!=ENOENT)goto out;
	error=new_inode(directory,request,1,&inode);if(error)goto out;
	inode->i_size=(off_t)length;memcpy(info(inode)->shortlink,target,length);error=persist_inode(inode);
	if(error==0 && inode->i_linkcount==0) {
		error=creation_publish(directory,name,inode,result);
		goto out;
	}
	if(error==0)error=dir_add(directory,name,(uint32_t)inode->i_ino,10);
	if(error){error=discard_new_inode_after_error(inode,0,error);goto out;}
	*result=inode;
out:
	mutex_unlock(&ms->namespace_lock);
	return error;
}

static ssize_t
pwrite_inode(struct inode *inode, const void *buffer, size_t length, off_t offset)
{
	return pwrite_inode_context(inode, buffer, length, offset, NULL);
}

static ssize_t ufs_read(struct file *file,void *buffer,size_t length)
{ ssize_t n=pread_inode(file->f_inode,buffer,length,file->f_offset); if(n>0)file->f_offset+=n; return n; }
static ssize_t ufs_pread(struct file *file,void *buffer,size_t length,off_t offset)
{ return pread_inode(file->f_inode,buffer,length,offset); }
static ssize_t ufs_write(struct file *file,const void *buffer,size_t length)
{ ssize_t n=pwrite_inode(file->f_inode,buffer,length,file->f_offset);if(n>0)file->f_offset+=n;return n; }
static ssize_t ufs_pwrite(struct file *file,const void *buffer,size_t length,off_t offset)
{ return pwrite_inode(file->f_inode,buffer,length,offset); }
static ssize_t
ufs_pwrite_context(
	struct file *file,
	const void *buffer,
	size_t length,
	off_t offset,
	unsigned flags,
	const struct ucred *credential,
	const struct io_context *context)
{
	int error;

	(void)flags;
	(void)credential;
	error = io_context_validate(context);
	if (error != 0)
		return -error;
	return pwrite_inode_context(file->f_inode, buffer, length, offset, context);
}

static int ufs_readdir(struct file *file,struct dirent *entry,int *eof)
{
	uint32_t number; uint8_t type; char name[NAME_MAX+1U]; int error=next_dirent(file->f_inode,&file->f_offset,&number,&type,name);
	if(error==ENOENT){*eof=1;return 0;} if(error)return error;
	memset(entry,0,sizeof(*entry)); entry->d_ino=number;
	entry->d_type=type==1?INODE_FIFO:type==4?INODE_DIR:type==8?INODE_REG:
		type==10?INODE_SYMLINK:type==12?INODE_SOCKET:INODE_NONE;
	strcpy(entry->d_name,name); *eof=0; return 0;
}
static ssize_t ufs_readlink(struct inode *inode,char *buffer,size_t length)
{
	struct ufs_mount_state *ms=state(inode->i_mount);
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
		*name_space=UFS_EXTATTR_NAMESPACE_USER;part=name+5;
	} else if(strncmp(name,"system.",7)==0) {
		*name_space=UFS_EXTATTR_NAMESPACE_SYSTEM;part=name+7;
		if(strncmp(part,"security.",9)==0)return EINVAL;
	} else if(strncmp(name,"security.",9)==0) {
		*name_space=UFS_EXTATTR_NAMESPACE_SYSTEM;part=name;
	} else return EOPNOTSUPP;
	*stored_length=strlen(part);
	if(*stored_length==0||*stored_length>255U)return EINVAL;
	*stored=part;return 0;
}

static int
extattr_load(struct inode *inode,uint8_t **result,size_t *length)
{
	struct ufs_inode_info *ui=info(inode);
	struct ufs_mount_state *ms=state(inode->i_mount);
	uint8_t *area;unsigned block_count,index;size_t offset=0;
	if(result==NULL||length==NULL)return EINVAL;
	*result=NULL;*length=ui->extattr_size;
	if(ui->extattr_size==0)return 0;
	block_count=(ui->extattr_size+ms->super.bsize-1U)/ms->super.bsize;
	if(block_count==0||block_count>UFS_NXADDR)return EIO;
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
		if(ui->extattr_size-offset<UFS_EXTATTR_HEADER_SIZE)goto invalid;
		record=ufs_get32(area,offset,ms->super.swapped);
		padding=area[offset+5U];name_length=area[offset+6U];
		base=extattr_align(UFS_EXTATTR_HEADER_SIZE+name_length);
		if(record<base||(record&7U)!=0||record>ui->extattr_size-offset||
		    padding>record-base||area[offset+4U]<UFS_EXTATTR_NAMESPACE_USER||
		    area[offset+4U]>UFS_EXTATTR_NAMESPACE_SYSTEM)
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
	struct ufs_mount_state *ms=state(inode->i_mount);size_t offset=0;
	while(offset<area_length) {
		uint32_t record=ufs_get32(area,offset,ms->super.swapped);
		uint8_t disk_name_length=area[offset+6U];
		size_t base=extattr_align(UFS_EXTATTR_HEADER_SIZE+disk_name_length);
		if(area[offset+4U]==name_space&&disk_name_length==name_length&&
		    memcmp(area+offset+UFS_EXTATTR_HEADER_SIZE,name,name_length)==0) {
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
	struct ufs_inode_info *ui=info(inode);
	struct ufs_mount_state *ms=state(inode->i_mount);
	uint64_t old_ext[UFS_NXADDR],new_fragment=0;uint64_t old_blocks;
	uint32_t old_size=ui->extattr_size;uint8_t *block=NULL,*old_area=NULL;
	size_t old_area_length=0;unsigned old_count,index;int error=0,rollback;
	int handled;
	if(length>ms->super.bsize)return ENOSPC;
	old_ext[0]=ui->extattr[0];old_ext[1]=ui->extattr[1];old_blocks=ui->blocks;
	old_count=old_size==0?0U:(old_size+ms->super.bsize-1U)/ms->super.bsize;
	if((uint64_t)old_count*(ms->super.bsize/UFS_SECTOR_SIZE)>old_blocks)return EIO;
	if(old_size!=0)error=extattr_load(inode,&old_area,&old_area_length);
	if(error==0&&old_area_length!=old_size)error=EIO;
	if(error!=0)return error;
	if(area==NULL)length=0;
	if(length==0) {
		error=xattr_release_group(inode,&handled);
		if(handled){kern_free(old_area);return error;}
		ui->extattr_size=0;ui->extattr[0]=0;ui->extattr[1]=0;
		ui->blocks=old_blocks-(uint64_t)old_count*(ms->super.bsize/UFS_SECTOR_SIZE);
		error=persist_inode(inode);
		if(error==0)error=disk_sync(inode->i_mount->m_disk);
		if(error!=0){ui->extattr_size=old_size;ui->extattr[0]=old_ext[0];
			ui->extattr[1]=old_ext[1];ui->blocks=old_blocks;
			if(persist_inode(inode)!=0)ms->writable=0;
			kern_free(old_area);return error;}
		for(index=0;index<old_count;index++)if((rollback=free_block(inode->i_mount,
		    old_ext[index],inode->i_uid,inode->i_gid))!=0){ms->writable=0;error=rollback;break;}
		kern_free(old_area);return error;
	}
	error=xattr_existing_group(inode,area,length,&handled);
	if(handled){kern_free(old_area);return error;}
	if(ms->journal_enabled && old_size==0) {
		error=xattr_allocate_group(inode,area,length,&handled);
		if(handled){kern_free(old_area);return error;}
	}
	block=kern_calloc(1,ms->super.bsize);if(block==NULL){kern_free(old_area);return ENOMEM;}
	memcpy(block,area,length);
	if(old_ext[0]==0){error=allocate_block(inode->i_mount,inode->i_uid,inode->i_gid,&new_fragment);if(error!=0)goto out;}
	else new_fragment=old_ext[0];
	error=write_block(inode->i_mount,new_fragment,block);if(error!=0)goto rollback_data;
	ui->extattr_size=(uint32_t)length;ui->extattr[0]=new_fragment;ui->extattr[1]=0;
	ui->blocks=old_blocks-(uint64_t)old_count*(ms->super.bsize/UFS_SECTOR_SIZE)+
	    ms->super.bsize/UFS_SECTOR_SIZE;
	error=persist_inode(inode);
	if(error==0)error=disk_sync(inode->i_mount->m_disk);
	if(error!=0)goto rollback_metadata;
	if(old_count>1U&&(rollback=free_block(inode->i_mount,old_ext[1],inode->i_uid,inode->i_gid))!=0){
		ms->writable=0;error=rollback;
	}
	goto out;
rollback_metadata:
	ui->extattr_size=old_size;ui->extattr[0]=old_ext[0];ui->extattr[1]=old_ext[1];
	ui->blocks=old_blocks;
	rollback=persist_inode(inode);
	if(rollback==0)rollback=disk_sync(inode->i_mount->m_disk);
	if(rollback!=0) {
		/* The new pointer may still be committed: keep its allocation. */
		ms->writable=0;
		goto out;
	}
rollback_data:
	if(old_ext[0]==0) {
		if(ms->writable&&new_fragment!=0&&(rollback=free_block(inode->i_mount,new_fragment,inode->i_uid,inode->i_gid))!=0)
			ms->writable=0;
	} else if(old_area!=NULL&&write_block(inode->i_mount,old_ext[0],old_area)!=0)
		ms->writable=0;
out:
	kern_free(old_area);kern_free(block);return error;
}

static ssize_t
ufs_getxattr(struct inode *inode,const char *name,void *value,size_t size)
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
ufs_setxattr(struct inode *inode,const char *name,const void *value,size_t size,
	unsigned flags)
{
	struct ufs_mount_state *ms=state(inode->i_mount);uint8_t name_space,*area=NULL,*updated=NULL;
	const char *stored;size_t stored_length,area_length=0,at=0,old_record=0;
	size_t base,new_record,new_length,padding;int found,error;
	if(!ms->writable)
		return EROFS;
	if(value==NULL&&size!=0)
		return EINVAL;
	error=extattr_name(name,&name_space,&stored,&stored_length);if(error!=0)return error;
	base=extattr_align(UFS_EXTATTR_HEADER_SIZE+stored_length);
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
	ufs_put32(updated,at,(uint32_t)new_record,ms->super.swapped);
	updated[at+4U]=name_space;updated[at+5U]=(uint8_t)padding;
	updated[at+6U]=(uint8_t)stored_length;
	memcpy(updated+at+UFS_EXTATTR_HEADER_SIZE,stored,stored_length);
	if(size!=0)memcpy(updated+at+base,value,size);
	if(area_length>at+(found?old_record:0U))memcpy(updated+at+new_record,
	    area+at+(found?old_record:0U),area_length-at-(found?old_record:0U));
	error=extattr_publish(inode,updated,new_length);
out:
	mutex_unlock(&inode->i_lock);kern_free(updated);kern_free(area);return error;
}

static ssize_t
ufs_listxattr(struct inode *inode,char *list,size_t size)
{
	struct ufs_mount_state *ms=state(inode->i_mount);uint8_t *area=NULL;
	size_t area_length,offset=0,needed=0;int error;
	mutex_lock(&inode->i_lock);error=extattr_load(inode,&area,&area_length);
	if(error!=0)goto out;
	while(offset<area_length){uint32_t record=ufs_get32(area,offset,ms->super.swapped);
		uint8_t ns=area[offset+4U],nlen=area[offset+6U];const char *prefix;
		const uint8_t *disk_name=area+offset+UFS_EXTATTR_HEADER_SIZE;size_t prefix_length;
		if(ns==UFS_EXTATTR_NAMESPACE_USER){prefix="user.";prefix_length=5U;}
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
ufs_removexattr(struct inode *inode,const char *name)
{
	struct ufs_mount_state *ms=state(inode->i_mount);uint8_t name_space,*area=NULL,*updated=NULL;
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
ufs_getattr(struct inode *inode,struct stat *status)
{
	struct ufs_inode_info *ui=info(inode);

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
ufs_setattr(struct inode *inode,const struct stat *status,unsigned mask)
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
		error=ufs_truncate(inode,status->st_size);
		if(error!=0)
			return error;
	}

	mutex_lock(&inode->i_lock);
	if(!state(inode->i_mount)->writable){mutex_unlock(&inode->i_lock);return EROFS;}
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
		    (state(inode->i_mount)->super.bsize/UFS_SECTOR_SIZE),1,
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
ufs_inode_sync(struct inode *inode)
{
	int error;

	mutex_lock(&inode->i_lock);
	/* A retired or not-yet-bound cache object has no persistent inode identity. */
	if(inode->i_ino==0) {
		mutex_unlock(&inode->i_lock);
		return 0;
	}
	error=state(inode->i_mount)->writable ? persist_inode(inode) :
	    (inode->i_mount->m_flags&MOUNT_READ_ONLY)!=0 ? 0 : EROFS;
	mutex_unlock(&inode->i_lock);
	return error;
}

#include "ufs-inode-retirement.inc"

/* Releases every owner of an unlinked inode and reports the first failure. */
static int
reclaim_unlinked_inode(
	struct inode *inode)
{
	struct ufs_inode_info *ui;
	int handled;
	int error;

	/* Requires a nonreserved, unlinked identity on a writable mount. */
	ui = info(inode);
	if (inode->i_linkcount != 0 || inode->i_ino <= UFS_ROOT_INO)
		return EINVAL;
	if (!state(inode->i_mount)->writable)
		return EROFS;

	/* Keeps every remaining reference reachable until its own release commits. */
	error = ufs_truncate(inode, 0);
	if (error != 0)
		return error;
	mutex_lock(&inode->i_lock);
	error = extattr_publish(inode, NULL, 0);
	mutex_unlock(&inode->i_lock);
	if (error != 0)
		return error;
	error = retire_inode_group(inode, &handled);
	if (handled)
		return error;

	/* Preserves the ordered retirement path for profiles outside group admission. */
	if (inode->i_type == INODE_DIR) {
		error = adjust_directory_count(inode->i_mount, (uint32_t)inode->i_ino, -1);
		if (error != 0)
			return error;
	}
	inode->i_mode = 0;
	inode->i_type = INODE_NONE;
	ui->blocks = 0;
	error = persist_inode(inode);
	if (error != 0)
		return error;
	error = disk_sync(inode->i_mount->m_disk);
	if (error != 0)
		return error;
	error = free_inode_number(inode->i_mount, (uint32_t)inode->i_ino, inode->i_uid, inode->i_gid);

	/* Returns actual retirement failure to explicit cleanup and recovery callers. */
	return error;
}

/* Preserves the VFS final-reference callback while sharing checked reclamation. */
static void
ufs_reclaim(
	struct inode *inode)
{
	/* Ignores identities whose lifetime does not permit filesystem retirement. */
	if (inode->i_linkcount != 0 || inode->i_ino <= UFS_ROOT_INO ||
	    !state(inode->i_mount)->writable)
		return;

	/* The callback has no errno channel; explicit owners call the checked helper. */
	(void)reclaim_unlinked_inode(inode);
}

/* Marks a confirmed unpublished inode unlinked without discarding its resources. */
static int
creation_unlink_group(
	struct inode *inode)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *image;
	struct ufs_transaction_outcome outcome;
	struct ufs_journal_extent extent;
	uint8_t *block;
	uint64_t fragment;
	int error;

	/* Reserves one dinode image before acquiring metadata ownership. */
	ms = state(inode->i_mount);
	if (inode->i_ino <= UFS_ROOT_INO)
		return EINVAL;
	image = kern_malloc(sizeof(*image) + ms->super.bsize);
	if (image == NULL)
		return ENOMEM;
	block = (uint8_t *)(image + 1);
	memset(&outcome, 0, sizeof(outcome));
	mutex_lock(&inode->i_lock);
	mutex_lock(&ms->lock);
	memcpy(image, info(inode), sizeof(*image));
	image->inode.i_linkcount = 0;
	error = ms->writable ? prepare_inode_locked(&image->inode, block, &fragment) : EROFS;
	if (error == 0) {
		extent.target = fragment << ms->super.fsbtodb;
		extent.sectors = ms->super.bsize / UFS_SECTOR_SIZE;
		extent.payload = block;
		error = metadata_group_commit(inode->i_mount, &extent, 1, NULL, &outcome);
	}

	/* Makes a proven zero-link owner eligible for checked resource reclamation. */
	if (outcome.committed) {
		inode->i_linkcount = 0;
		inode->i_flags |= INODE_DEAD;
	}
	mutex_unlock(&ms->lock);
	mutex_unlock(&inode->i_lock);
	kern_free(image);

	/* Retains references and the original errno on an unsuccessful transition. */
	return error;
}

/* Discards an unpublished journal-backed creation through checked release owners. */
static int
discard_reserved_inode(
	struct inode *inode)
{
	int error;

	/* Detaches borrowed endpoints before any final-reference destruction is possible. */
	detach_new_socket_special(inode);
	error = creation_unlink_group(inode);
	if (error == 0)
		error = reclaim_unlinked_inode(inode);
	if (error == 0) {
		inode->i_ino = 0;
		inode->i_flags |= INODE_DEAD;
	}
	inode_release(inode);

	/* Reports incomplete cleanup without hiding which persistent owners remain. */
	return error;
}

static const struct inode_ops ufs_inode_ops={.lookup=ufs_lookup,.create=ufs_create,
	.mkdir=ufs_mkdir,.mknod=ufs_mknod,.unlink=ufs_unlink,.rmdir=ufs_rmdir,.rename=ufs_rename,
	.link=ufs_link,
	.symlink=ufs_symlink,.readlink=ufs_readlink,.getattr=ufs_getattr,
	.setattr=ufs_setattr,.truncate=ufs_truncate,.sync=ufs_inode_sync,
	.getxattr=ufs_getxattr,.setxattr=ufs_setxattr,
	.listxattr=ufs_listxattr,.removexattr=ufs_removexattr,
	.reclaim=ufs_reclaim};
static int ufs_file_sync(struct file *file)
{
	int error;
	if(file==NULL)return EINVAL;
	error=inode_sync(file->f_inode);
	return error!=0?error:ufs_sync(file->f_inode->i_mount);
}
static const struct file_ops ufs_regular_ops={.read=ufs_read,.write=ufs_write,.pread=ufs_pread,.pwrite=ufs_pwrite,.pwrite_internal=ufs_pwrite_context,.fsync=ufs_file_sync};
static const struct file_ops ufs_directory_ops={.readdir=ufs_readdir,.fsync=ufs_file_sync};

static struct inode *ufs_alloc_inode(struct mount *mountp)
{ (void)mountp; return (struct inode *)kern_calloc(1,sizeof(struct ufs_inode_info)); }
static void ufs_free_inode(struct inode *inode) { kern_free(inode); }

static int ufs_read_super(struct disk *disk,struct ufs_super *super)
{
	uint8_t *buffer; int error;
	if(disk==NULL||disk->d_block_size!=UFS_SECTOR_SIZE)return EOPNOTSUPP;
	buffer=kern_malloc(UFS_SBLOCK_SIZE); if(buffer==NULL)return ENOMEM;
	error=observed_disk_read(disk,UFS_SBLOCK_OFFSET/UFS_SECTOR_SIZE,UFS_SBLOCK_SIZE/UFS_SECTOR_SIZE,buffer);
	if(error==0)error=ufs_super_decode(buffer,UFS_SBLOCK_SIZE,disk->d_block_count,super);
	kern_free(buffer); return error;
}

static char
ufs_identity_hex(unsigned value)
{
	return (char)(value < 10U ? '0' + value : 'A' + value - 10U);
}

static void
ufs_identity_hex32(char output[8], uint32_t value)
{
	unsigned index;

	for (index = 0; index < 8U; index++)
		output[index] = ufs_identity_hex(
		    (value >> (28U - index * 4U)) & 15U);
}

static void
ufs_identity_label(char *output, size_t capacity, const uint8_t *input,
		    size_t length)
{
	size_t end = length;
	size_t index;

	while (end != 0U &&
	       (input[end - 1U] == ' ' || input[end - 1U] == 0U))
		end--;
	if (end >= capacity)
		end = capacity - 1U;
	for (index = 0; index < end; index++)
		output[index] = input[index] >= 0x20U && input[index] <= 0x7eU ?
		    (char)input[index] : '_';
	output[end] = '\0';
}

int
ufs_identify(struct disk *disk, struct block_identity *identity)
{
	struct ufs_super super;
	uint8_t *buffer;
	uint32_t first, second;
	uint64_t first_block, block_count;
	int error;

	if (disk == NULL || identity == NULL)
		return EINVAL;
	if (disk->d_block_size != UFS_SECTOR_SIZE)
		return EOPNOTSUPP;
	first_block = UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;
	block_count = UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;
	if (disk->d_block_count < first_block + block_count)
		return EOPNOTSUPP;
	buffer = kern_malloc(UFS_SBLOCK_SIZE);
	if (buffer == NULL)
		return ENOMEM;
	error = disk_read_direct(disk, first_block, (uint32_t)block_count,
	    buffer);
	if (error == 0)
		error = ufs_super_decode(buffer, UFS_SBLOCK_SIZE,
		    disk->d_block_count, &super);
	if (error != 0) {
		kern_free(buffer);
		return error;
	}

	strcpy(identity->type, "ufs");
	identity->flags |= ZEDBSD_BLKID_TYPE;
	first = ufs_get32(buffer, UFS_FS_ID, super.swapped);
	second = ufs_get32(buffer, UFS_FS_ID + 4U, super.swapped);
	if (first != 0U || second != 0U) {
		ufs_identity_hex32(identity->uuid, first);
		ufs_identity_hex32(identity->uuid + 8U, second);
		identity->uuid[16] = '\0';
		identity->flags |= ZEDBSD_BLKID_UUID;
	}
	ufs_identity_label(identity->label, sizeof(identity->label),
	    buffer + UFS_FS_VOLNAME, UFS_FS_VOLNAME_SIZE);
	if (identity->label[0] != '\0')
		identity->flags |= ZEDBSD_BLKID_LABEL;
	kern_free(buffer);
	return 0;
}

static int
ufs_write_clean(struct mount *mountp,uint8_t clean)
{
	struct ufs_mount_state *ms=state(mountp);uint8_t *buffer;int error;
	buffer=kern_malloc(UFS_SBLOCK_SIZE);if(buffer==NULL)return ENOMEM;
	mutex_lock(&ms->lock);
	mutex_lock(&ms->journal_lock);
	error=journal_checkpoint_locked(mountp);
	mutex_unlock(&ms->journal_lock);
	if(error==0)error=observed_disk_read(mountp->m_disk,UFS_SBLOCK_OFFSET/UFS_SECTOR_SIZE,UFS_SBLOCK_SIZE/UFS_SECTOR_SIZE,buffer);
	if(error==0){buffer[UFS_FS_CLEAN]=clean;error=write_sectors(mountp,UFS_SBLOCK_OFFSET/UFS_SECTOR_SIZE,UFS_SBLOCK_SIZE/UFS_SECTOR_SIZE,buffer);}
	if(error==0)
		error=disk_sync(mountp->m_disk);
	if(error==0)
		ms->super.clean=clean;
	mutex_unlock(&ms->lock);
	kern_free(buffer);
	return error;
}
static int ufs_probe(struct disk *disk) { struct ufs_super s; return ufs_read_super(disk,&s); }

static int
ufs_quota_rebuild(struct mount *mountp)
{
	struct ufs_mount_state *ms=state(mountp);uint8_t *block;
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
			raw=block+(index%ms->super.inopb)*UFS_DINODE_SIZE;
			mode=ufs_get16(raw,UFS_DI_MODE,ms->super.swapped);
			if(mode==0)continue;
			blocks=ufs_get64(raw,UFS_DI_BLOCKS,ms->super.swapped);
			if(blocks%(ms->super.bsize/UFS_SECTOR_SIZE)!=0){error=EIO;break;}
			error=quota_rebuild_add(&ms->quota,
			    ufs_get32(raw,UFS_DI_UID,ms->super.swapped),
			    ufs_get32(raw,UFS_DI_GID,ms->super.swapped),
			    blocks/(ms->super.bsize/UFS_SECTOR_SIZE),1);
			if(error!=0)break;
		}
	}
	kern_free(block);return error;
}

static int
ufs_quota_load(struct mount *mountp,struct inode *root)
{
	struct ufs_mount_state *ms=state(mountp);uint8_t *buffer;
	ssize_t length,loaded;int error;
	length=ufs_getxattr(root,UFS_QUOTA_XATTR,NULL,0);
	if(length==-ENODATA)return 0;
	if(length<0)return (int)-length;
	if(length==0||(size_t)length>ms->super.bsize)return EINVAL;
	buffer=kern_malloc((size_t)length);if(buffer==NULL)return ENOMEM;
	loaded=ufs_getxattr(root,UFS_QUOTA_XATTR,buffer,(size_t)length);
	error=loaded==length?quota_import_config(&ms->quota,buffer,(size_t)length):
	    (loaded<0?(int)-loaded:EIO);
	kern_free(buffer);return error;
}

static int
snapshot_disk_submit(struct disk *disk,struct bio *bio)
{
	struct ufs_mount_state *ms=disk!=NULL?disk->d_data:NULL;int error;
	if(ms==NULL||bio==NULL)return EINVAL;
	/* The callback context owns a disk pointer; it is not itself a disk. */
	if(bio->b_op==BIO_FLUSH)error=disk_sync(ms->snapshot_io.disk);
	else if(bio->b_op!=BIO_READ)error=EROFS;
	else {mutex_lock(&ms->snapshot_lock);error=ufs_snapshot_read(&ms->snapshot,
		bio->b_mapped_block,bio->b_block_count,bio->b_data);
		mutex_unlock(&ms->snapshot_lock);}
	bio_complete(bio,error,error==0&&bio->b_op==BIO_READ?
	    (size_t)bio->b_block_count*UFS_SECTOR_SIZE:0);return 0;
}
static const struct disk_ops snapshot_disk_ops={.submit=snapshot_disk_submit};
static unsigned snapshot_disk_sequence;

static int
snapshot_disk_publish(struct ufs_mount_state *ms)
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
		disk->d_flags=DISK_READ_ONLY;disk->d_block_size=UFS_SECTOR_SIZE;
		disk->d_block_count=ms->snapshot.volume_sectors;
		disk->d_max_transfer_blocks=128;disk->d_ops=&snapshot_disk_ops;
		disk->d_data=ms;error=disk_create(disk);
		if(error==0){ms->snapshot_disk=disk;return 0;}
		(void)disk_destroy(disk);if(error!=EEXIST)return error;
	}
	return ENOSPC;
}

static int
snapshot_disk_remove(struct ufs_mount_state *ms)
{
	struct disk *disk=ms->snapshot_disk;int error;
	if(disk==NULL)return 0;
	error=disk_gone_if_idle(disk);if(error!=0)return error;
	error=disk_destroy(disk);if(error==0)ms->snapshot_disk=NULL;
	return error;
}

static void
ufs_state_free(struct ufs_mount_state *ms)
{
	if(ms==NULL)return;
	journal_image_free(ms);
	buf_view_release(&ms->cg_view);kern_free(ms->snapshot_map);kern_free(ms->cg);kern_free(ms);
}

static int
ufs_quota_persist(struct mount *mountp)
{
	struct ufs_mount_state *ms=state(mountp);uint8_t *buffer;size_t length;
	int error;
	if(!ms->writable||mountp->m_root==NULL)return EROFS;
	buffer=kern_malloc(ms->super.bsize);if(buffer==NULL)return ENOMEM;
	error=quota_export_config(&ms->quota,buffer,ms->super.bsize,&length);
	if(error==0)error=ufs_setxattr(mountp->m_root,UFS_QUOTA_XATTR,
	    buffer,length,0);
	if(error==0)error=disk_sync(mountp->m_disk);
	kern_free(buffer);return error;
}

#include "ufs-orphan.inc"

static int ufs_mount_impl(struct mount *mountp)
{
	struct ufs_mount_state *ms; struct inode *root; int error;
	uint64_t total_ndir=0,total_nbfree=0,total_nifree=0,total_nffree=0;
	uint32_t cg;
	int summaries_rebuilt=0;
	off_t cursor=0;
	uint32_t number;
	uint8_t type;
	char name[NAME_MAX+1U];
	if(mountp==NULL||mountp->m_disk==NULL)return EINVAL;
	ms=kern_calloc(1,sizeof(*ms)); if(ms==NULL)return ENOMEM;
	error=ufs_read_super(mountp->m_disk,&ms->super); if(error){kern_free(ms);return error;}
	mountp->m_data=ms;
	(void)mutex_init(&ms->journal_lock,LOCK_RANK_DEVICE,"ufs journal");
	(void)mutex_init(&ms->snapshot_lock,LOCK_RANK_DEVICE,"ufs snapshot");
	error=journal_discover(mountp,ms);
	if(error!=0){mountp->m_data=NULL;ufs_state_free(ms);return error;}
	error=snapshot_discover(mountp,ms);
	if(error!=0){mountp->m_data=NULL;ufs_state_free(ms);return error;}
	(void)mutex_init(&ms->namespace_lock,LOCK_RANK_NAMESPACE,
		"ufs namespace");
	(void)mutex_init(&ms->lock,LOCK_RANK_INODE,"ufs mount");
	quota_state_init(&ms->quota);
	ms->cg=kern_malloc(ms->super.bsize);if(ms->cg==NULL){mountp->m_data=NULL;ufs_state_free(ms);return ENOMEM;}
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
		if(cg==0&&!bit_test(ms->cg+ms->cg_iusedoff,UFS_ROOT_INO))
			{error=EINVAL;break;}
		total_ndir+=ufs_get32(ms->cg,UFS_CG_NDIR,ms->super.swapped);
		total_nbfree+=ufs_get32(ms->cg,UFS_CG_NBFREE,ms->super.swapped);
		total_nifree+=ufs_get32(ms->cg,UFS_CG_NIFREE,ms->super.swapped);
		total_nffree+=ufs_get32(ms->cg,UFS_CG_NFFREE,ms->super.swapped);
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
	if(error==0)error=ufs_quota_rebuild(mountp);
	if(error==0)error=load_cg_locked(mountp,0);
	if(error!=0){mountp->m_data=NULL;ufs_state_free(ms);return error;}
	/*
	 * Preserve the ordinary persistent upper's validated reopen policy.
	 * Private root mounts remain mounted through shutdown sync, so a dirty
	 * marker alone is not proof of damaged metadata. Keep the structural,
	 * allocation-summary and root checks as mount admission gates.
	 */
	if((mountp->m_flags&MOUNT_READ_ONLY)==0){if((mountp->m_disk->d_flags&DISK_READ_ONLY)!=0){mountp->m_data=NULL;ufs_state_free(ms);return EROFS;}ms->writable=1;if(summaries_rebuilt){error=write_super_summaries(mountp);if(error!=0){mountp->m_data=NULL;ufs_state_free(ms);return error;}}}
	error=load_inode(mountp,UFS_ROOT_INO,&root);
	if(error||root->i_type!=INODE_DIR){if(!error){root->i_flags|=INODE_DEAD;inode_release(root);}mountp->m_data=NULL;ufs_state_free(ms);return error?error:EIO;}
	/* A malformed root must not become the namespace anchor.  Validate the
	 * mandatory entries while the mount is still private and unpublished. */
	error=next_dirent(root,&cursor,&number,&type,name);
	if(error==0&&(number!=UFS_ROOT_INO||strcmp(name,".")!=0))error=EIO;
	if(error==0)error=next_dirent(root,&cursor,&number,&type,name);
	if(error==0&&(number!=UFS_ROOT_INO||strcmp(name,"..")!=0))error=EIO;
	if(error==0)error=ufs_quota_load(mountp,root);
	if(error==0)error=orphan_recover(mountp);
	if(error!=0){root->i_flags|=INODE_DEAD;inode_release(root);mountp->m_data=NULL;ufs_state_free(ms);return error;}
	/* Do not dirty an image until every read-only mount validation, including
	 * the root inode, has succeeded. */
	if(ms->writable){error=ufs_write_clean(mountp,0);if(error){root->i_flags|=INODE_DEAD;inode_release(root);mountp->m_data=NULL;ufs_state_free(ms);return error;}}
	root->i_flags|=INODE_ROOT; mountp->m_root=root;
	if(ms->snapshot.active&&(error=snapshot_disk_publish(ms))!=0){mountp->m_root=NULL;
		root->i_flags|=INODE_DEAD;inode_release(root);mountp->m_data=NULL;
		ufs_state_free(ms);return error;}
	return 0;
}
/* Excludes metadata admission until all previously published homes are durable. */
static int
ufs_sync(struct mount *mountp)
{
	struct ufs_mount_state *ms;
	int error;

	if (mountp == NULL || (ms = state(mountp)) == NULL)
		return EINVAL;
	mutex_lock(&ms->lock);
	mutex_lock(&ms->journal_lock);
	error = journal_checkpoint_locked(mountp);
	mutex_unlock(&ms->journal_lock);
	if (error == 0)
		error = disk_sync(mountp->m_disk);
	mutex_unlock(&ms->lock);
	return error;
}
static int ufs_statvfs(struct mount *mountp,struct statvfs *result)
{
	struct ufs_mount_state *ms=state(mountp);uint64_t nbfree,nffree,nifree;
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
ufs_quotactl(struct mount *mountp,struct quota_control *request)
{
	struct ufs_mount_state *ms=state(mountp);
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
		return !ms->writable?disk_sync(mountp->m_disk):ufs_quota_persist(mountp);
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
	if(error==0)error=ufs_quota_persist(mountp);
	if(error!=0&&(quota_import_config(&ms->quota,saved,saved_length)!=0))
		ms->writable=0;
	kern_free(saved);return error;
}
static int
ufs_snapshotctl(struct mount *mountp,struct snapshot_control *request)
{
	struct ufs_mount_state *ms=state(mountp);int error=0;
	if(ms==NULL||request==NULL)return EINVAL;
	memset(request->device,0,sizeof(request->device));
	if(!ms->snapshot_available)return EOPNOTSUPP;
	switch(request->command) {
	case ZEDBSD_SNAPSHOT_CREATE:
		if(!ms->writable)return EROFS;
		mutex_lock(&ms->lock);
		mutex_lock(&ms->snapshot_lock);
		mutex_lock(&ms->journal_lock);
		error=journal_checkpoint_locked(mountp);
		mutex_unlock(&ms->journal_lock);
		if(error==0)error=disk_sync(mountp->m_disk);
		if(error==0)error=ufs_snapshot_create(&ms->snapshot);
		mutex_unlock(&ms->snapshot_lock);
		mutex_unlock(&ms->lock);
		if(error!=0)return error;
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
static int ufs_prepare_unmount(struct mount *mountp) { struct ufs_mount_state *ms=state(mountp);if(ms!=NULL&&ms->snapshot_disk!=NULL)return EBUSY;return ms!=NULL&&ms->writable?ufs_write_clean(mountp,1):0; }
static void ufs_unmount(struct mount *mountp) { if(mountp&&mountp->m_data){struct ufs_mount_state *ms=state(mountp);ufs_state_free(ms);mountp->m_data=NULL;} }

/* Validates existing blocks without allocating or publishing metadata. */
static int
ufs_writeback_range(
	struct file *file,
	off_t offset,
	size_t length)
{
	struct inode *inode;
	struct ufs_mount_state *ms;
	uint64_t logical;
	uint64_t last;
	uint64_t fragment;
	int error;

	/* Rejects invalid ranges before inspecting the allocation map. */
	inode = file->f_inode;
	ms = state(inode->i_mount);
	if (offset < 0 || length == 0 || inode->i_type != INODE_REG)
		return 0;

	/* Serializes the allocation proof with backend mutations. */
	mutex_lock(&inode->i_lock);
	if (!ms->writable || offset > inode->i_size ||
	    (uint64_t)length > (uint64_t)(inode->i_size - offset)) {
		mutex_unlock(&inode->i_lock);
		return 0;
	}

	/* Requires every touched block to have a published allocation. */
	logical = (uint64_t)offset / ms->super.bsize;
	last = ((uint64_t)offset + length - 1U) / ms->super.bsize;
	for (; logical <= last; logical++) {
		error = bmap(inode, logical, &fragment);
		if (error != 0 || fragment == 0) {
			mutex_unlock(&inode->i_lock);
			return error != 0 ? -error : 0;
		}
	}

	/* Reports an entirely allocated overwrite. */
	mutex_unlock(&inode->i_lock);
	return 1;
}

const struct filesystem_type ufs_filesystem_type={
	.writeback_range=ufs_writeback_range,
	.fs_name="ufs",.probe=ufs_probe,.identify=ufs_identify,
	.mount=ufs_mount_impl,.sync=ufs_sync,
	.statvfs=ufs_statvfs,.quotactl=ufs_quotactl,
	.snapshotctl=ufs_snapshotctl,
	.prepare_unmount=ufs_prepare_unmount,
	.unmount=ufs_unmount,.alloc_inode=ufs_alloc_inode,.free_inode=ufs_free_inode,
};
