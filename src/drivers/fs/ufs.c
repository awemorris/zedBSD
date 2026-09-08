/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Begin consolidated ufs-vfs.c. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Conservative FreeBSD-derived UFS VFS implementation. */
#include <kern/io-pool.h>
#include <kern/cache-memory.h>
#include <kern/page.h>
#include <kern/buf.h>
#include <kern/sched.h>
#include <kern/writeback.h>
#include "kern/ufs.h"

/* Begin consolidated ufs-disk.h. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UFS_DISK_H
#define ZEDBSD_UFS_DISK_H

#include <stddef.h>
#include <stdint.h>

#define UFS_SECTOR_SIZE 512U
#define UFS_SBLOCK_OFFSET 65536U
#define UFS_SBLOCK_SIZE 8192U
#define UFS_FS_STRUCT_SIZE 1376U
#define UFS_MAGIC 0x19540119U
#define UFS_DINODE_SIZE 256U
#define UFS_ROOT_INO 2U
#define UFS_NDADDR 12U
#define UFS_NIADDR 3U
#define UFS_DIRBLKSIZ 512U
#define UFS_NXADDR 2U

/* Native UFS extended-attribute record format. */
#define UFS_EXTATTR_NAMESPACE_USER 1U
#define UFS_EXTATTR_NAMESPACE_SYSTEM 2U
#define UFS_EXTATTR_HEADER_SIZE 7U

/* Canonical struct fs offsets for the unified UFS codec. */
#define UFS_FS_SBLKNO 8U
#define UFS_FS_CBLKNO 12U
#define UFS_FS_IBLKNO 16U
#define UFS_FS_DBLKNO 20U
#define UFS_FS_NCG 44U
#define UFS_FS_BSIZE 48U
#define UFS_FS_FSIZE 52U
#define UFS_FS_FRAG 56U
#define UFS_FS_BSHIFT 80U
#define UFS_FS_FSHIFT 84U
#define UFS_FS_FRAGSHIFT 96U
#define UFS_FS_FSBTODB 100U
#define UFS_FS_SBSIZE 104U
#define UFS_FS_NINDIR 116U
#define UFS_FS_INOPB 120U
#define UFS_FS_ID 144U
#define UFS_FS_CSSIZE 156U
#define UFS_FS_CGSIZE 160U
#define UFS_FS_IPG 184U
#define UFS_FS_FPG 188U
#define UFS_FS_CLEAN 209U
#define UFS_FS_VOLNAME 680U
#define UFS_FS_VOLNAME_SIZE 32U
#define UFS_FS_SBLOCKLOC 1000U
#define UFS_FS_CSTOTAL_NDIR 1008U
#define UFS_FS_CSTOTAL_NBFREE 1016U
#define UFS_FS_CSTOTAL_NIFREE 1024U
#define UFS_FS_CSTOTAL_NFFREE 1032U
#define UFS_FS_SIZE 1080U
#define UFS_FS_DSIZE 1088U
#define UFS_FS_CSADDR 1096U
#define UFS_FS_FLAGS 1312U
#define UFS_FS_MAXSYMLINKLEN 1320U
#define UFS_FS_MAXFILESIZE 1328U
#define UFS_FS_MAGIC 1372U

/* Canonical struct ufs_dinode offsets. */
#define UFS_DI_MODE 0U
#define UFS_DI_NLINK 2U
#define UFS_DI_UID 4U
#define UFS_DI_GID 8U
#define UFS_DI_BLKSIZE 12U
#define UFS_DI_SIZE 16U
#define UFS_DI_BLOCKS 24U
#define UFS_DI_ATIME 32U
#define UFS_DI_MTIME 40U
#define UFS_DI_CTIME 48U
#define UFS_DI_BIRTHTIME 56U
#define UFS_DI_MTIMENSEC 64U
#define UFS_DI_ATIMENSEC 68U
#define UFS_DI_CTIMENSEC 72U
#define UFS_DI_BIRTHNSEC 76U
#define UFS_DI_GEN 80U
#define UFS_DI_KERNFLAGS 84U
#define UFS_DI_FLAGS 88U
#define UFS_DI_EXTSIZE 92U
#define UFS_DI_EXTB 96U
#define UFS_DI_DB 112U
#define UFS_DI_IB 208U
#define UFS_DI_MODREV 232U

/* struct cg remains the canonical FFS cylinder-group format. */
#define UFS_CG_MAGIC_VALUE 0x00090255U
#define UFS_CG_MAGIC 4U
#define UFS_CG_CGX 12U
#define UFS_CG_NDBLK 20U
#define UFS_CG_NDIR 24U
#define UFS_CG_NBFREE 28U
#define UFS_CG_NIFREE 32U
#define UFS_CG_NFFREE 36U
#define UFS_CG_IUSEDOFF 92U
#define UFS_CG_FREEOFF 96U
#define UFS_CG_NEXTFREEOFF 100U

struct ufs_super {
	uint32_t sblkno, cblkno, iblkno, dblkno;
	uint32_t cgoffset, cgmask;
	uint32_t ncg, bsize, fsize, frag;
	uint32_t bshift, fshift, fragshift, fsbtodb;
	uint32_t sbsize, nindir, inopb, ipg, fpg, cssize, cgsize;
	uint64_t sblockloc, size, dsize, csaddr;
	uint64_t cstotal_ndir, cstotal_nbfree;
	uint64_t cstotal_nifree, cstotal_nffree;
	uint32_t flags, maxsymlinklen;
	uint64_t maxfilesize;
	uint8_t clean;
	int swapped;
};

#endif
/* End consolidated ufs-disk.h. */

/* Begin consolidated ufs-endian.h. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UFS_ENDIAN_H
#define ZEDBSD_UFS_ENDIAN_H
#include <stddef.h>
#include <stdint.h>
uint16_t drv_ufs_get16(const void *, size_t, int);
uint32_t drv_ufs_get32(const void *, size_t, int);
uint64_t drv_ufs_get64(const void *, size_t, int);
void drv_ufs_put16(void *, size_t, uint16_t, int);
void drv_ufs_put32(void *, size_t, uint32_t, int);
void drv_ufs_put64(void *, size_t, uint64_t, int);
#endif
/* End consolidated ufs-endian.h. */

/* Begin consolidated ufs-super.h. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UFS_SUPER_H
#define ZEDBSD_UFS_SUPER_H

int drv_ufs_super_decode(const void *, size_t, uint64_t, struct ufs_super *);
#endif
/* End consolidated ufs-super.h. */

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

/* Begin consolidated ufs-consistency.h. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_DRIVERS_FS_UFS_CONSISTENCY_H
#define ZEDBSD_DRIVERS_FS_UFS_CONSISTENCY_H

#include <stddef.h>
#include <stdint.h>

struct ufs_journal_io {
	void *context;
	int (*read)(void *, uint64_t, uint32_t, void *);
	int (*write)(void *, uint64_t, uint32_t, const void *);
	int (*flush)(void *);
};

struct ufs_journal {
	struct ufs_journal_io io;
	uint64_t first_sector;
	uint32_t sector_count;
	uint64_t next_sequence;
	int poisoned;
	uint64_t home_sectors;
	uint64_t pending_sequence;
	uint32_t pending_digest;
	unsigned pending_ready;
	unsigned pending_clearing;
	uint64_t committed_sequence;
	uint32_t committed_digest;
	uint8_t *image;
	unsigned image_valid;
	uint32_t image_readers;
};

/*
 * A zero-initialized, noncopyable pin owns immutable bytes until view_release.
 */
struct ufs_journal_view {
	struct ufs_journal *journal;
	const uint8_t *image;
	uint64_t sequence;
	uint64_t home_sectors;
};

/*
 * View operations alone may run concurrently with the serialized writer.
 * Retirement closes acquisition; backing cannot be reused until pins drain.
 * Copy refuses any uncovered sector before modifying the destination. */
int drv_ufs_journal_view_acquire(struct ufs_journal *journal,
				 struct ufs_journal_view *view);
int drv_ufs_journal_view_copy(const struct ufs_journal_view *view,
			      uint64_t first, uint32_t count, void *buffer);
void drv_ufs_journal_view_release(struct ufs_journal_view *view);
int drv_ufs_journal_views_busy(const struct ufs_journal *journal);
/*
 * Writer/teardown owner closes acquisition, then drains pins before discarding
 * backing. */
void drv_ufs_journal_views_close(struct ufs_journal *journal);

int drv_ufs_journal_init(struct ufs_journal *journal,
			 const struct ufs_journal_io *io, uint64_t first,
			 uint32_t count, uint64_t home_sectors);

/*
 * A journal has one durable transaction slot.  Its owner must serialize
 * commit and replay calls; the core deliberately has no scheduler/lock
 * dependency so host recovery tools can share it.
 */
int drv_ufs_journal_commit(struct ufs_journal *journal, uint64_t target,
			   const void *payload, uint32_t sectors);

int drv_ufs_journal_replay(struct ufs_journal *journal);

/*
 * Bounded multi-target redo is the sole journal format.
 * The owner serializes non-view calls and replays after initialization before
 * mutation. Payloads remain immutable through commitv. A nonempty slot returns
 * EBUSY; commit errors remain errors even when immediate recovery installs the
 * group.
 */
#define UFS_JOURNAL_EXTENTS 30U
#define UFS_JOURNAL_GROUP_SECTORS 128U
#define UFS_JOURNAL_IMAGE_BYTES ((UFS_JOURNAL_GROUP_SECTORS + 1U) * 512U)

/*
 * Owner accounts and retains this exclusive storage until an idle detach.
 * Non-view calls remain serialized; initialization must not discard a live
 * owner. */
int drv_ufs_journal_bind_image(struct ufs_journal *journal, void *image,
			       size_t bytes);
struct ufs_journal_extent {
	uint64_t target;
	uint32_t sectors;
	const void *payload;
};
int drv_ufs_journal_commitv(struct ufs_journal *journal,
			    const struct ufs_journal_extent *extents,
			    unsigned count);

/*
 * Publication retains only durable redo and a witness, never caller payload
 * pointers. The serialized owner checkpoints or recovers pending state before
 * destruction. */
int drv_ufs_journal_publishv(struct ufs_journal *journal,
			     const struct ufs_journal_extent *extents,
			     unsigned count);
int drv_ufs_journal_checkpoint(struct ufs_journal *journal);
/* Drains a prior owner, retries recovery, and retains the original error. */
int drv_ufs_journal_drain(struct ufs_journal *journal);
int drv_ufs_journal_read(struct ufs_journal *journal, uint64_t first,
			 uint32_t count, void *buffer);

/*
 * Positive commit proof survives slot retirement until another commit or init.
 * Inspect under owner serialization before admitting another group. A false
 * result alone is not proof that rollback is safe after unresolved I/O. */
int drv_ufs_journal_committed(const struct ufs_journal *journal,
			      uint64_t sequence, uint32_t digest);

#define UFS_SNAPSHOT_EMPTY UINT64_MAX

struct ufs_snapshot_entry {
	uint64_t sector;
	uint32_t record;
	uint32_t reserved;
};

struct ufs_snapshot {
	struct ufs_journal_io io;
	uint64_t volume_sectors;
	uint64_t first_sector;
	uint32_t sector_count;
	uint32_t max_records;
	uint32_t next_record;
	struct ufs_snapshot_entry *map;
	size_t map_count;
	unsigned active;
};

int drv_ufs_snapshot_init(struct ufs_snapshot *snapshot,
			  const struct ufs_journal_io *io, uint64_t volume,
			  uint64_t first, uint32_t sectors,
			  struct ufs_snapshot_entry *map, size_t map_count);

int drv_ufs_snapshot_open(struct ufs_snapshot *snapshot);

int drv_ufs_snapshot_create(struct ufs_snapshot *snapshot);

int drv_ufs_snapshot_preserve(struct ufs_snapshot *snapshot, uint64_t first,
			      uint32_t count);

int drv_ufs_snapshot_read(struct ufs_snapshot *snapshot, uint64_t first,
			  uint32_t count, void *buffer);

int drv_ufs_snapshot_delete(struct ufs_snapshot *snapshot);

#endif
/* End consolidated ufs-consistency.h. */

/* Begin consolidated ufs-private.h. */
/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_UFS_PRIVATE_H
#define ZEDBSD_UFS_PRIVATE_H

#include <kern/buf.h>
#include <hal/hal.h>
#include <kern/inode.h>
#include <kern/quota.h>

struct ufs_io_owner {
	struct disk *disk;
	const struct io_context *context;
};

/* Share internal object layouts with production-linked lifetime fixtures. */
struct ufs_mount_state {
	/* Each borrowed context is protected by its corresponding I/O lock. */
	struct ufs_io_owner journal_io;
	struct ufs_io_owner snapshot_io;
	struct ufs_super super;
	struct mutex namespace_lock;
	struct mutex lock;
	struct mutex journal_lock;
	uint8_t *cg;
	struct buf_view cg_view;
	unsigned cg_valid;
	unsigned cg_dirty;
	uint32_t cg_iusedoff;
	uint32_t cg_freeoff;
	uint32_t cg_nextfreeoff;
	uint32_t active_cg;
	uint32_t rotor_cg;
	struct ufs_journal journal;
	struct hal_pmem journal_memory;
	struct ufs_snapshot snapshot;
	struct ufs_snapshot_entry *snapshot_map;
	struct disk *snapshot_disk;
	struct mutex snapshot_lock;
	struct quota_state quota;
	int journal_enabled;
	int snapshot_available;
	int writable;
};

struct ufs_inode_info {
	struct inode inode;
	uint64_t extattr[UFS_NXADDR];
	uint32_t extattr_size;
	uint64_t direct[UFS_NDADDR];
	uint64_t indirect[UFS_NIADDR];
	uint32_t disk_flags;
	uint64_t blocks;
	uint32_t generation;
	uint8_t shortlink[120];
};

#endif
/* End consolidated ufs-private.h. */

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/statvfs.h>
#include <zedbsd/blkid.h>
#include <zedbsd/quota.h>
#include <zedbsd/snapshot.h>

void clock_realtime(time_t *, long *);

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

static int journal_checkpoint_locked(struct mount *mountp);

/* Caller owns journal_lock; no borrowed operation context survives this drain. */
static int
journal_checkpoint_locked(
	struct mount *mountp)
{
	struct ufs_mount_state *ms = mountp->m_data;
	struct io_context child;
	int error;

	/* Handles the ms condition. */
	if (!ms->journal_enabled)
		return 0;
	error = io_context_child(&child, NULL, IO_CONTEXT_ORDERED);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	ms->journal_io.context = &child;
	error = drv_ufs_journal_drain(&ms->journal);
	ms->journal_io.context = NULL;

	/* Checks the operation status. */
	if (error != 0 && io_error_record != NULL) {
		io_error_record(&mountp->m_metadata_error, error);
		io_error_record(&mountp->m_write_error, error);
	}

	/* Handles the ms condition. */
	if (ms->journal.poisoned)
		ms->writable = 0;

	/* Returns the computed result. */
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
	int function_result;

	io_stats_record(IO_UFS_READ,
			disk != NULL ? (uint64_t)count * disk->d_block_size
				     : 0);

	/* Obtains the disk read result. */
	function_result = disk_read(disk, block, count, buffer);

	/* Returns the computed result. */
	return function_result;
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

/* Supports the journal read operation. */
static int
journal_read(
	void *context,
	uint64_t lba,
	uint32_t count,
	void *buffer)
{
	int function_result;
	struct ufs_io_owner *owner = context;

	/* Obtains the observed disk read result. */
	function_result = observed_disk_read(owner->disk, lba, count, buffer);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the journal write operation. */
static int
journal_write(
	void *context,
	uint64_t lba,
	uint32_t count,
	const void *buffer)
{
	int function_result;
	struct ufs_io_owner *owner = context;
	struct io_context child;
	int error;

	error = io_context_child(&child, owner->context, IO_CONTEXT_ORDERED);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	io_stats_record(IO_UFS_WRITE,
			(uint64_t)count * owner->disk->d_block_size);

	/* Obtains the disk write context result. */
	function_result =
		disk_write_context(owner->disk, lba, count, buffer, &child);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the journal flush operation. */
static int
journal_flush(
	void *context)
{
	int function_result;
	struct ufs_io_owner *owner = context;

	/* Obtains the disk sync result. */
	function_result = disk_sync(owner->disk);

	/* Returns the computed result. */
	return function_result;
}

static uint32_t locator_get32(const uint8_t *p);

/* Supports the locator get32 operation. */
static uint32_t
locator_get32(
	const uint8_t *p)
{
	/* Returns the computed result. */
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}
static uint64_t locator_get64(const uint8_t *p);

/* Supports the locator get64 operation. */
static uint64_t
locator_get64(
	const uint8_t *p)
{
	uint64_t function_result;

	/* Computes the function result. */
	function_result = locator_get32(p) | (uint64_t)locator_get32(p + 4)
						     << 32;

	/* Returns the computed result. */
	return function_result;
}
static uint32_t locator_digest(const uint8_t *p, size_t length);

/* Supports the locator digest operation. */
static uint32_t
locator_digest(
	const uint8_t *p,
	size_t length)
{
	uint32_t value = 2166136261U;
	size_t n;

	/* Process each remaining element. */
	for (n = 0; n < length; n++) {
		value ^= p[n];
		value *= 16777619U;
	}

	/* Returns the computed result. */
	return value;
}

static void journal_wait_readers(struct ufs_mount_state *ms);

/* Drains short immutable copies before the serialized writer reuses their backing. */
static void
journal_wait_readers(
	struct ufs_mount_state *ms)
{
	/*
 * Readers release their pins without acquiring the writer's journal
	 * mutex. */
	/* Continue while the operation condition remains true. */
	while (drv_ufs_journal_views_busy(&ms->journal))
		sched_yield();
}

/* Reserves and accounts immutable redo storage before journal recovery/admission. */
static int
journal_image_alloc(
	struct ufs_mount_state *ms)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, UFS_JOURNAL_IMAGE_BYTES, ZEDBSD_PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0};
	struct hal_pmem memory;
	int error;

	/*
 * Obtains backing without holding a metadata or journal mutation lock.
	 */
	memset(&memory, 0, sizeof(memory));
	error = hal_pmem_alloc(&request, &memory);

	/* Checks the operation status. */
	if (error != HAL_OK || memory.vaddr == NULL ||
	    memory.size < UFS_JOURNAL_IMAGE_BYTES) {
		/* Checks the hal pmem free result. */
		if (memory.size != 0 && hal_pmem_free(&memory) != HAL_OK)
			HAL_FATAL("ufs journal allocation rollback failed");

		/* Returns the computed result. */
		return ENOMEM;
	}

	/*
 * Charges the allocator's complete rounded backing to shared metadata
	 * memory. */
	error = cache_memory_reserve(CACHE_MEMORY_BUF_META, memory.size, 0);

	/* Checks the operation status. */
	if (error != 0) {
		/* Checks the hal pmem free result. */
		if (hal_pmem_free(&memory) != HAL_OK)
			HAL_FATAL("ufs journal reservation rollback failed");

		/* Returns the computed result. */
		return error;
	}
	cache_memory_commit(CACHE_MEMORY_BUF_META, memory.size);
	ms->journal_memory = memory;
	error = drv_ufs_journal_bind_image(&ms->journal, memory.vaddr,
					   memory.size);

	/* Checks the operation status. */
	if (error != 0)
		journal_image_free(ms);

	/*
 * Reports a complete immutable-image owner or a fully unwound failure.
	 */
	return error;
}

/* Releases backing only after the mount owner has excluded every journal caller. */
static void
journal_image_free(
	struct ufs_mount_state *ms)
{
	size_t bytes;

	/*
 * Failed mount recovery may retain durable redo, but has no admitted
	 * readers. */
	bytes = ms->journal_memory.size;

	/* Handles the bytes condition. */
	if (bytes == 0)
		return;
	drv_ufs_journal_views_close(&ms->journal);
	journal_wait_readers(ms);

	/* Checks the hal pmem free result. */
	if (hal_pmem_free(&ms->journal_memory) != HAL_OK)
		HAL_FATAL("ufs journal backing release failed");
	cache_memory_release(CACHE_MEMORY_BUF_META, bytes);
	memset(&ms->journal_memory, 0, sizeof(ms->journal_memory));
	ms->journal.image = NULL;
	ms->journal.image_valid = 0;
}

static int journal_discover(struct mount *mountp, struct ufs_mount_state *ms);

/* Supports the journal discover operation. */
static int
journal_discover(
	struct mount *mountp,
	struct ufs_mount_state *ms)
{
	struct ufs_journal_io io;
	uint8_t locator[UFS_SECTOR_SIZE];
	uint64_t end = ms->super.size << ms->super.fsbtodb;
	uint32_t sectors;
	int error;

	/* Checks the current endpoint. */
	if (end >= mountp->m_disk->d_block_count)
		return 0;
	error = observed_disk_read(mountp->m_disk, end, 1, locator);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the memcmp condition. */
	if (memcmp(locator, "ZUJ", 3) == 0 && memcmp(locator, "ZUJ2", 4) != 0)
		return EINVAL;

	/* Handles the memcmp condition. */
	if (memcmp(locator, "ZUJ2", 4) != 0)
		return 0;
	sectors = locator_get32(locator + 8);

	/* Checks the locator get32 result. */
	if (locator_get32(locator + 4) != 2U ||
	    locator_get64(locator + 12) != end ||
	    locator_get32(locator + 24) != locator_digest(locator, 24) ||
	    sectors < 18U ||
	    (uint64_t)sectors + 1U > mountp->m_disk->d_block_count - end)

		/* Returns the computed result. */
		return EINVAL;
	ms->journal_io.disk = mountp->m_disk;
	io.context = &ms->journal_io;
	io.read = journal_read;
	io.write = journal_write;
	io.flush = journal_flush;
	error = drv_ufs_journal_init(&ms->journal, &io, end + 1U, sectors, end);

	/* Checks the operation status. */
	if (error == 0)
		error = journal_image_alloc(ms);

	/* Checks the operation status. */
	if (error == 0) {
		ms->journal_enabled = 1;
		error = drv_ufs_journal_replay(&ms->journal);
	}

	/* Returns the computed result. */
	return error;
}

static int snapshot_discover(struct mount *mountp, struct ufs_mount_state *ms);

/* Supports the snapshot discover operation. */
static int
snapshot_discover(
	struct mount *mountp,
	struct ufs_mount_state *ms)
{
	struct ufs_journal_io io;
	uint8_t locator[UFS_SECTOR_SIZE];
	uint64_t end = ms->super.size << ms->super.fsbtodb, cursor = end;
	uint32_t sectors, max_records;
	size_t map_count;
	int error;

	/* Checks the current endpoint. */
	if (end >= mountp->m_disk->d_block_count)
		return 0;
	error = observed_disk_read(mountp->m_disk, end, 1, locator);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the memcmp condition. */
	if (memcmp(locator, "ZUJ2", 4) == 0) {
		sectors = locator_get32(locator + 8);

		/* Checks the locator get32 result. */
		if (locator_get32(locator + 4) != 2U ||
		    locator_get64(locator + 12) != end ||
		    sectors > mountp->m_disk->d_block_count - end - 1U)

			/* Returns the computed result. */
			return EINVAL;
		cursor = end + 1U + sectors;
	}

	/* Checks the current cursor position. */
	if (cursor >= mountp->m_disk->d_block_count)
		return 0;
	error = observed_disk_read(mountp->m_disk, cursor, 1, locator);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the memcmp condition. */
	if (memcmp(locator, "ZSL1", 4) != 0)
		return 0;
	sectors = locator_get32(locator + 8);

	/* Checks the locator get32 result. */
	if (locator_get32(locator + 4) != 1U ||
	    locator_get64(locator + 16) != cursor ||
	    locator_get64(locator + 24) != end ||
	    locator_get32(locator + 32) != locator_digest(locator, 32) ||
	    sectors < 3U ||
	    (uint64_t)sectors + 1U > mountp->m_disk->d_block_count - cursor)

		/* Returns the computed result. */
		return EINVAL;
	max_records = (sectors - 1U) / 2U;
#if SIZE_MAX == UINT32_MAX

	/* Handles the max records condition. */
	if (max_records > SIZE_MAX / (2U * sizeof(*ms->snapshot_map)))
		return EOVERFLOW;
#endif
	map_count = (size_t)max_records * 2U;
	ms->snapshot_map = kern_calloc(map_count, sizeof(*ms->snapshot_map));

	/* Handles the snapshot map availability. */
	if (ms->snapshot_map == NULL)
		return ENOMEM;
	ms->snapshot_io.disk = mountp->m_disk;
	io.context = &ms->snapshot_io;
	io.read = journal_read;
	io.write = journal_write;
	io.flush = journal_flush;
	error = drv_ufs_snapshot_init(&ms->snapshot, &io, end, cursor + 1U,
				      sectors, ms->snapshot_map, map_count);

	/* Checks the operation status. */
	if (error == 0)
		error = drv_ufs_snapshot_open(&ms->snapshot);

	/* Checks the operation status. */
	if (error != 0) {
		kern_free(ms->snapshot_map);
		ms->snapshot_map = NULL;

		/* Returns the computed result. */
		return error;
	}
	ms->snapshot_available = 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the write sectors impl operation. */
static int
write_sectors_impl(
	struct mount *mountp,
	uint64_t lba,
	uint32_t count,
	const void *buffer,
	const struct io_context *context)
{
	int error_local;
	int error_local1;
	int error_local2;
	struct ufs_mount_state *ms = mountp != NULL ? mountp->m_data : NULL;
	int snapshot_locked = 0;

	/* Handles the ms availability. */
	if (ms != NULL && ms->snapshot_available) {
		mutex_lock(&ms->snapshot_lock);
		snapshot_locked = 1;
		ms->snapshot_io.context = context;
		error_local =
			drv_ufs_snapshot_preserve(&ms->snapshot, lba, count);
		ms->snapshot_io.context = NULL;

		/* Checks the operation status. */
		if (error_local != 0) {
			mutex_unlock(&ms->snapshot_lock);

			/* Returns the computed result. */
			return error_local;
		}
	}

	/* Handles the ms availability. */
	if (ms != NULL && ms->journal_enabled) {
		mutex_lock(&ms->journal_lock);
		error_local1 = journal_checkpoint_locked(mountp);

		/* Checks the operation status. */
		if (error_local1 == 0) {
			journal_wait_readers(ms);
			ms->journal_io.context = context;
			error_local1 = drv_ufs_journal_commit(&ms->journal, lba,
							      buffer, count);
			ms->journal_io.context = NULL;
		}

		/* Checks the operation status. */
		if (error_local1 != 0 && ms->journal.poisoned)
			ms->writable = 0;
		mutex_unlock(&ms->journal_lock);

		/* Handles the snapshot locked condition. */
		if (snapshot_locked)
			mutex_unlock(&ms->snapshot_lock);

		/* Returns the computed result. */
		return error_local1;
	}

	io_stats_record(IO_UFS_WRITE,
			(uint64_t)count * mountp->m_disk->d_block_size);
	error_local2 =
		disk_write_context(mountp->m_disk, lba, count, buffer, context);

	/* Handles the snapshot locked condition. */
	if (snapshot_locked)
		mutex_unlock(&ms->snapshot_lock);

	/* Returns the computed result. */
	return error_local2;
}

/* Retains the logical write owner across snapshot and journal completion. */
static int
write_sectors_context(
	struct mount *mountp,
	uint64_t lba,
	uint32_t count,
	const void *buffer,
	const struct io_context *context)
{
	struct io_context child;
	int error;

	error = io_context_child(&child, context, IO_CONTEXT_ORDERED);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	io_epoch_begin(&mountp->m_write_epoch);
	error = write_sectors_impl(mountp, lba, count, buffer, &child);
	io_epoch_end(&mountp->m_write_epoch);

	/* Returns the computed result. */
	return error;
}

/* Supports the write sectors operation. */
static int
write_sectors(
	struct mount *mountp,
	uint64_t lba,
	uint32_t count,
	const void *buffer)
{
	int function_result;

	/* Obtains the write sectors context result. */
	function_result =
		write_sectors_context(mountp, lba, count, buffer, NULL);

	/* Returns the computed result. */
	return function_result;
}

static const struct inode_ops ufs_inode_ops;
static const struct file_ops ufs_regular_ops;
static const struct file_ops ufs_directory_ops;
static int ufs_lookup(struct inode *, const struct componentname *, struct inode **);
static int persist_inode(struct inode *);
static int reclaim_unlinked_inode(struct inode *inode);
static int discard_reserved_inode(struct inode *inode);
static int creation_unlink_group(struct inode *inode);
static int persist_inode_locked(struct inode *);
static int prepare_inode_locked(struct inode *inode, uint8_t *block, uint64_t *location);
static uint64_t inode_fragment(struct inode *inode);
static void encode_inode_locked(struct inode *inode, uint8_t *block);
static ssize_t ufs_getxattr(struct inode *, const char *, void *, size_t);
static int ufs_setxattr(struct inode *, const char *, const void *, size_t, unsigned);

static struct ufs_mount_state *state(const struct mount *mountp);

/* Supports the state operation. */
static struct ufs_mount_state *
state(
	const struct mount *mountp)
{
	/* Returns the computed result. */
	return mountp != NULL ? mountp->m_data : NULL;
}
static struct ufs_inode_info *info(const struct inode *inode);

/* Supports the info operation. */
static struct ufs_inode_info *
info(
	const struct inode *inode)
{
	/* Returns the computed result. */
	return (struct ufs_inode_info *)(uintptr_t)inode;
}

static int journal_read_image(struct ufs_mount_state *ms, uint64_t first, uint32_t count, void *buffer);

/* Copies a fully covered committed image without joining checkpoint device I/O. */
static int
journal_read_image(
	struct ufs_mount_state *ms,
	uint64_t first,
	uint32_t count,
	void *buffer)
{
	struct ufs_journal_view view = {0};
	int error;

	/*
 * Acquires a generation whose storage cannot be retired underneath the
	 * copy. */
	if (!ms->journal_enabled)
		return ENOENT;
	error = drv_ufs_journal_view_acquire(&ms->journal, &view);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = drv_ufs_journal_view_copy(&view, first, count, buffer);
	drv_ufs_journal_view_release(&view);

	/*
 * Releases before any caller falls back to the serialized home-read
	 * path. */
	return error;
}

static int read_metadata_sectors(struct mount *mountp, uint64_t first, uint32_t count, void *buffer);

/* Reads metadata at any sector granularity through the committed redo owner. */
static int
read_metadata_sectors(
	struct mount *mountp,
	uint64_t first,
	uint32_t count,
	void *buffer)
{
	struct ufs_mount_state *ms = state(mountp);
	int error;

	error = journal_read_image(ms, first, count, buffer);

	/* Checks the operation status. */
	if (error != ENOENT)
		return error;

	/* Handles the ms condition. */
	if (ms->journal_enabled)
		mutex_lock(&ms->journal_lock);

	/* Handles the ms condition. */
	if (ms->journal_enabled &&
	    (ms->journal.pending_sequence != 0 || ms->journal.poisoned)) {
		error = drv_ufs_journal_read(&ms->journal, first, count,
					     buffer);
	} else {
		error = observed_disk_read(mountp->m_disk, first, count,
					   buffer);
	}

	/* Handles the ms condition. */
	if (ms->journal_enabled)
		mutex_unlock(&ms->journal_lock);

	/* Returns the computed result. */
	return error;
}

static int read_block(struct mount *mountp, uint64_t fragment, void *buffer);

/* Supports the read block operation. */
static int
read_block(
	struct mount *mountp,
	uint64_t fragment,
	void *buffer)
{
	int function_result;
	const struct ufs_super *s = &state(mountp)->super;

	/* Handles the fragment condition. */
	if (fragment == 0) {
		memset(buffer, 0, s->bsize);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the fragment condition. */
	if (fragment >= s->size || s->frag > s->size - fragment)
		return EIO;

	/* Obtains the read metadata sectors result. */
	function_result =
		read_metadata_sectors(mountp, fragment << s->fsbtodb,
				      s->bsize / UFS_SECTOR_SIZE, buffer);

	/* Returns the computed result. */
	return function_result;
}

static int write_block(struct mount *mountp, uint64_t fragment, const void *buffer);

/* Supports the write block operation. */
static int
write_block(
	struct mount *mountp,
	uint64_t fragment,
	const void *buffer)
{
	int function_result;
	const struct ufs_super *s = &state(mountp)->super;

	/* Handles the fragment condition. */
	if (fragment == 0 || fragment >= s->size ||
	    s->frag > s->size - fragment)

		/* Returns the computed result. */
		return EIO;

	/* Obtains the write sectors result. */
	function_result =
		write_sectors(mountp, (uint64_t)fragment << s->fsbtodb,
			      s->bsize / UFS_SECTOR_SIZE, buffer);

	/* Returns the computed result. */
	return function_result;
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
	int function_result;
	const struct ufs_super *s = &state(mountp)->super;

	/* Handles the fragment condition. */
	if (fragment != 0 && fragment < s->size &&
	    s->frag <= s->size - fragment)
		io_stats_record(IO_UFS_CONTENT_READ, s->bsize);

	/* Obtains the read block result. */
	function_result = read_block(mountp, fragment, buffer);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the write content block operation. */
static int
write_content_block(
	struct mount *mountp,
	uint64_t fragment,
	const void *buffer)
{
	int function_result;
	const struct ufs_super *s = &state(mountp)->super;

	/* Handles the fragment condition. */
	if (fragment != 0 && fragment < s->size &&
	    s->frag <= s->size - fragment)
		io_stats_record(IO_UFS_CONTENT_WRITE, s->bsize);

	/* Obtains the write block result. */
	function_result = write_block(mountp, fragment, buffer);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the write content context operation. */
static int
write_content_context(
	struct mount *mountp,
	uint64_t fragment,
	const void *buffer,
	const struct io_context *context)
{
	int function_result;
	const struct ufs_super *super;

	super = &state(mountp)->super;

	/* Handles the fragment condition. */
	if (fragment == 0 || fragment >= super->size ||
	    super->frag > super->size - fragment)

		/* Returns the computed result. */
		return EIO;
	io_stats_record(IO_UFS_CONTENT_WRITE, super->bsize);

	/* Obtains the write sectors context result. */
	function_result = write_sectors_context(
		mountp, fragment << super->fsbtodb,
		super->bsize / UFS_SECTOR_SIZE, buffer, context);

	/* Returns the computed result. */
	return function_result;
}

static int bit_test(const uint8_t *map, uint32_t bit);

/* Supports the bit test operation. */
static int
bit_test(
	const uint8_t *map,
	uint32_t bit)
{
	/* Returns the computed result. */
	return (map[bit >> 3] & (uint8_t)(1U << (bit & 7U))) != 0;
}
static void bit_set(uint8_t *map, uint32_t bit);

/* Supports the bit set operation. */
static void
bit_set(
	uint8_t *map,
	uint32_t bit)
{
	map[bit >> 3] |= (uint8_t)(1U << (bit & 7U));
}
static void bit_clear(uint8_t *map, uint32_t bit);

/* Supports the bit clear operation. */
static void
bit_clear(
	uint8_t *map,
	uint32_t bit)
{
	map[bit >> 3] &= (uint8_t)~(1U << (bit & 7U));
}

static uint64_t cgstart(const struct ufs_super *super, uint32_t cg);

/* Supports the cgstart operation. */
static uint64_t
cgstart(
	const struct ufs_super *super,
	uint32_t cg)
{
	/* Returns the computed result. */
	return (uint64_t)cg * super->fpg +
	       (uint64_t)super->cgoffset * (cg & ~super->cgmask);
}

static uint32_t cg_ndblk(const struct ufs_super *super, uint32_t cg);

/* Supports the cg ndblk operation. */
static uint32_t
cg_ndblk(
	const struct ufs_super *super,
	uint32_t cg)
{
	uint64_t start = cgstart(super, cg);
	uint64_t remaining = start < super->size ? super->size - start : 0;

	/* Returns the computed result. */
	return remaining > super->fpg ? super->fpg : (uint32_t)remaining;
}

static int load_cg_image(struct mount *mountp, uint32_t cg, uint64_t fragment);

/* Resolves the CG through immutable redo before considering cached home bytes. */
static int
load_cg_image(
	struct mount *mountp,
	uint32_t cg,
	uint64_t fragment)
{
	struct ufs_mount_state *ms;
	int error;

	/*
 * Drops the home-view identity when committed redo supplies the working
	 * image. */
	ms = state(mountp);
	error = journal_read_image(ms, fragment << ms->super.fsbtodb,
				   ms->super.bsize / UFS_SECTOR_SIZE, ms->cg);

	/* Checks the operation status. */
	if (error == 0) {
		buf_view_release(&ms->cg_view);
		io_stats_record(IO_UFS_CG_HIT, ms->super.bsize);

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the operation status. */
	if (error != ENOENT)
		return error;

	/*
 * Serializes uncovered/uncertain reads and cache identity with
	 * checkpoint writes. */
	if (ms->journal_enabled)
		mutex_lock(&ms->journal_lock);

	/* Handles the ms condition. */
	if (ms->journal_enabled &&
	    (ms->journal.pending_sequence != 0 || ms->journal.poisoned)) {
		buf_view_release(&ms->cg_view);
		error = drv_ufs_journal_read(
			&ms->journal, fragment << ms->super.fsbtodb,
			ms->super.bsize / UFS_SECTOR_SIZE, ms->cg);
	} else if (ms->cg_valid && ms->active_cg == cg &&
		   disk_view_matches(mountp->m_disk, &ms->cg_view)) {
		io_stats_record(IO_UFS_CG_HIT, ms->super.bsize);
		error = 0;
	} else {
		buf_view_release(&ms->cg_view);
		io_stats_record(IO_UFS_CG_MISS, ms->super.bsize);
		io_stats_record(IO_UFS_READ, ms->super.bsize);
		error = disk_read_view(mountp->m_disk,
				       fragment << ms->super.fsbtodb,
				       ms->super.bsize / UFS_SECTOR_SIZE,
				       ms->cg, &ms->cg_view);
	}

	/* Handles the ms condition. */
	if (ms->journal_enabled)
		mutex_unlock(&ms->journal_lock);

	/* Returns the computed result. */
	return error;
}

static int load_cg_locked(struct mount *mountp, uint32_t cg);

/* Supports the load cg locked operation. */
static int
load_cg_locked(
	struct mount *mountp,
	uint32_t cg)
{
	struct ufs_mount_state *ms = state(mountp);
	uint32_t inode_map_bytes, free_map_bytes, ndblk;
	uint32_t ndir, nbfree, nifree, nffree;
	uint64_t fragment;
	int error;

	/* Handles the cg condition. */
	if (cg >= ms->super.ncg)
		return EINVAL;
	fragment = cgstart(&ms->super, cg) + ms->super.cblkno;

	/* Handles the fragment condition. */
	if (fragment >= ms->super.size ||
	    ms->super.frag > ms->super.size - fragment)

		/* Returns the computed result. */
		return EINVAL;
	error = load_cg_image(mountp, cg, fragment);

	/* Checks the operation status. */
	if (error != 0) {
		ms->cg_valid = 0;

		/* Returns the computed result. */
		return error;
	}
	ms->cg_valid = 0;
	ndblk = cg_ndblk(&ms->super, cg);
	inode_map_bytes = (ms->super.ipg + 7U) / 8U;
	free_map_bytes = (ms->super.fpg + 7U) / 8U;
	ndir = drv_ufs_get32(ms->cg, UFS_CG_NDIR, ms->super.swapped);
	nbfree = drv_ufs_get32(ms->cg, UFS_CG_NBFREE, ms->super.swapped);
	nifree = drv_ufs_get32(ms->cg, UFS_CG_NIFREE, ms->super.swapped);
	nffree = drv_ufs_get32(ms->cg, UFS_CG_NFFREE, ms->super.swapped);
	ms->cg_iusedoff =
		drv_ufs_get32(ms->cg, UFS_CG_IUSEDOFF, ms->super.swapped);
	ms->cg_freeoff =
		drv_ufs_get32(ms->cg, UFS_CG_FREEOFF, ms->super.swapped);
	ms->cg_nextfreeoff =
		drv_ufs_get32(ms->cg, UFS_CG_NEXTFREEOFF, ms->super.swapped);

	/* Checks the drv ufs get32 result. */
	if (drv_ufs_get32(ms->cg, UFS_CG_MAGIC, ms->super.swapped) !=
		    UFS_CG_MAGIC_VALUE ||
	    drv_ufs_get32(ms->cg, UFS_CG_CGX, ms->super.swapped) != cg ||
	    drv_ufs_get32(ms->cg, UFS_CG_NDBLK, ms->super.swapped) != ndblk ||
	    ms->cg_iusedoff >= ms->super.bsize ||
	    ms->cg_freeoff > ms->super.bsize ||
	    ms->cg_nextfreeoff > ms->super.cgsize ||
	    ms->cg_iusedoff > ms->cg_freeoff ||
	    inode_map_bytes > ms->cg_freeoff - ms->cg_iusedoff ||
	    ms->cg_freeoff > ms->cg_nextfreeoff ||
	    free_map_bytes > ms->cg_nextfreeoff - ms->cg_freeoff ||
	    ndir > ms->super.ipg || nifree > ms->super.ipg ||
	    nbfree > ndblk / ms->super.frag || nffree > ndblk ||
	    (uint64_t)nbfree * ms->super.frag + nffree > ndblk) {
		buf_view_release(&ms->cg_view);

		/* Returns the computed result. */
		return EINVAL;
	}
	ms->active_cg = cg;
	ms->cg_valid = 1;
	ms->cg_dirty = 0;

	/* Reports successful completion. */
	return 0;
}

static int valid_inode_fragment(const struct ufs_super *super, uint64_t fragment);

/* Supports the valid inode fragment operation. */
static int
valid_inode_fragment(
	const struct ufs_super *super,
	uint64_t fragment)
{
	uint64_t start;
	uint32_t ndblk;
	uint32_t cg;

	/* Handles the fragment condition. */
	if (fragment == 0)
		return 1;
	/* Process each element required by the operation. */
	for (cg = 0; cg < super->ncg; cg++) {
		start = cgstart(super, cg);
		ndblk = cg_ndblk(super, cg);

		/* Handles the fragment condition. */
		if (fragment >= start + super->dblkno &&
		    fragment < start + ndblk &&
		    super->frag <= start + ndblk - fragment)

			/* Reports operation failure. */
			return 1;
	}

	/* Reports successful completion. */
	return 0;
}

static int prepare_super_summaries(struct mount *mountp, uint8_t *buffer);

/* Supports the prepare super summaries operation. */
static int
prepare_super_summaries(
	struct mount *mountp,
	uint8_t *buffer)
{
	struct ufs_mount_state *ms = state(mountp);
	int error;

	error = read_metadata_sectors(
		mountp, UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE,
		UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE, buffer);

	/* Checks the operation status. */
	if (error == 0) {
		drv_ufs_put64(buffer, UFS_FS_CSTOTAL_NDIR,
			      ms->super.cstotal_ndir, ms->super.swapped);
		drv_ufs_put64(buffer, UFS_FS_CSTOTAL_NBFREE,
			      ms->super.cstotal_nbfree, ms->super.swapped);
		drv_ufs_put64(buffer, UFS_FS_CSTOTAL_NIFREE,
			      ms->super.cstotal_nifree, ms->super.swapped);
		drv_ufs_put64(buffer, UFS_FS_CSTOTAL_NFFREE,
			      ms->super.cstotal_nffree, ms->super.swapped);
	}

	/* Returns the computed result. */
	return error;
}

static int write_super_summaries(struct mount *mountp);

/* Writes an independently prepared summary for synchronous metadata callers. */
static int
write_super_summaries(
	struct mount *mountp)
{
	uint8_t *buffer;
	int error;

	buffer = kern_malloc(UFS_SBLOCK_SIZE);

	/* Handles the buffer availability. */
	if (buffer == NULL)
		return ENOMEM;
	error = prepare_super_summaries(mountp, buffer);

	/* Checks the operation status. */
	if (error == 0) {
		error = write_sectors(
			mountp, UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE,
			UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE, buffer);
	}
	kern_free(buffer);

	/* Returns the computed result. */
	return error;
}

static int write_cg(struct mount *mountp);

/* Writes the mount-owned CG image immediately; failed ownership remains explicit. */
static int
write_cg(
	struct mount *mountp)
{
	struct ufs_mount_state *ms;
	struct kern_test_fault_result fault;
	int error;

	/*
 * Releases optional copy pins before writing or entering nested cache
	 * paths. */
	ms = state(mountp);
	ms->cg_valid = 0;
	ms->cg_dirty = 1;
	buf_view_release(&ms->cg_view);

	/* Handles the fault condition. */
	if (KERN_TEST_FAULT(KERN_TEST_FAULT_UFS_CG_WRITE, UINT32_MAX,
			    UINT32_MAX, &fault))

		/* Returns the computed result. */
		return fault.error != 0 ? fault.error : EIO;
	error = write_sectors(
		mountp,
		(cgstart(&ms->super, ms->active_cg) + ms->super.cblkno)
			<< ms->super.fsbtodb,
		ms->super.bsize / UFS_SECTOR_SIZE, ms->cg);

	/* Checks the operation status. */
	if (error == 0)
		error = write_super_summaries(mountp);

	/* Checks the operation status. */
	if (error == 0)
		ms->cg_dirty = 0;

	/* Preserves the immediate writer's original error convention. */
	return error;
}

static int write_cg_rollback(struct mount *mountp, int original_error);

/* Caller holds ms->lock and has already restored the in-memory CG image. */
static int
write_cg_rollback(
	struct mount *mountp,
	int original_error)
{
	struct ufs_mount_state *ms = state(mountp);
	int rollback = write_cg(mountp);

	/* Handles the rollback condition. */
	if (rollback != 0) {
		ms->writable = 0;

		/* Returns the computed result. */
		return rollback;
	}

	/* Returns the computed result. */
	return original_error;
}

static int adjust_directory_count(struct mount *mountp, uint32_t ino, int delta);

/* Supports the adjust directory count operation. */
static int
adjust_directory_count(
	struct mount *mountp,
	uint32_t ino,
	int delta)
{
	struct ufs_mount_state *ms = state(mountp);
	uint32_t count, cg = ino / ms->super.ipg;
	uint64_t old_total;
	int error;

	mutex_lock(&ms->lock);
	error = load_cg_locked(mountp, cg);

	/* Checks the operation status. */
	if (error != 0) {
		mutex_unlock(&ms->lock);

		/* Returns the computed result. */
		return error;
	}
	count = drv_ufs_get32(ms->cg, UFS_CG_NDIR, ms->super.swapped);
	old_total = ms->super.cstotal_ndir;

	/* Handles the delta condition. */
	if ((delta < 0 && count == 0) || (delta > 0 && count == UINT32_MAX)) {
		error = EIO;
	} else {
		drv_ufs_put32(ms->cg, UFS_CG_NDIR,
			      delta < 0 ? count - 1U : count + 1U,
			      ms->super.swapped);
		ms->super.cstotal_ndir =
			delta < 0 ? old_total - 1U : old_total + 1U;
		error = write_cg(mountp);

		/* Checks the operation status. */
		if (error != 0) {
			drv_ufs_put32(ms->cg, UFS_CG_NDIR, count,
				      ms->super.swapped);
			ms->super.cstotal_ndir = old_total;
			error = write_cg_rollback(mountp, error);
		}
	}
	mutex_unlock(&ms->lock);

	/* Returns the computed result. */
	return error;
}

static uint64_t quota_now(void);

/* Supports the quota now operation. */
static uint64_t
quota_now(
	void)
{
	time_t seconds = 0;
	long nanoseconds = 0;

	clock_realtime(&seconds, &nanoseconds);
	(void)nanoseconds;

	/* Returns the computed result. */
	return seconds > 0 ? (uint64_t)seconds : 0;
}

static int allocate_block_compat(struct mount *mountp, uid_t uid, gid_t gid, uint64_t *result);

/* Supports the allocate block compat operation. */
static int
allocate_block_compat(
	struct mount *mountp,
	uid_t uid,
	gid_t gid,
	uint64_t *result)
{
	uint8_t *zero;
	uint64_t absolute;
	uint32_t ndblk;
	struct ufs_mount_state *ms = state(mountp);
	uint8_t *map;
	struct quota_charge charge;
	uint32_t fragment, n, cg, attempt;
	uint64_t old_total;
	int error;

	error = quota_reserve(&ms->quota, uid, gid, 1, 0, quota_now(), &charge);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = ENOSPC;
	mutex_lock(&ms->lock);
	/* Process each element required by the operation. */
	for (attempt = 0; attempt < ms->super.ncg; attempt++) {
		cg = (ms->rotor_cg + attempt) % ms->super.ncg;
		error = load_cg_locked(mountp, cg);

		/* Checks the operation status. */
		if (error != 0)
			break;
		error = ENOSPC;
		map = ms->cg + ms->cg_freeoff;
		ndblk = cg_ndblk(&ms->super, cg);
		/* Process each element required by the operation. */
		for (fragment = (ms->super.dblkno + ms->super.frag - 1U) &
				~(ms->super.frag - 1U);
		     fragment + ms->super.frag <= ndblk;
		     fragment += ms->super.frag) {
			/* Process each element required by the operation. */
			for (n = 0;
			     n < ms->super.frag && bit_test(map, fragment + n);
			     n++)
				;

			/* Checks the current item count. */
			if (n != ms->super.frag)
				continue;
			/* Process each element required by the operation. */
			for (n = 0; n < ms->super.frag; n++)
				bit_clear(map, fragment + n);
			old_total = ms->super.cstotal_nbfree;
			uint32_t free = drv_ufs_get32(ms->cg, UFS_CG_NBFREE,
						      ms->super.swapped);

			/* Handles the free condition. */
			if (free == 0) {
				/* Process each element required by the operation. */
				for (n = 0; n < ms->super.frag; n++)
					bit_set(map, fragment + n);
				break;
			}
			drv_ufs_put32(ms->cg, UFS_CG_NBFREE, free - 1U,
				      ms->super.swapped);
			ms->super.cstotal_nbfree = old_total - 1U;
			error = write_cg(mountp);

			/* Checks the operation status. */
			if (error == 0) {
				zero = kern_calloc(1, ms->super.bsize);
				absolute = cgstart(&ms->super, cg) + fragment;

				/* Handles the zero availability. */
				if (zero == NULL) {
					error = ENOMEM;
				} else {
					error = write_block(mountp, absolute,
							    zero);
					kern_free(zero);
				}
			}

			/* Checks the operation status. */
			if (error != 0) {
				/* Process each element required by the operation. */
				for (n = 0; n < ms->super.frag; n++)
					bit_set(map, fragment + n);
				drv_ufs_put32(ms->cg, UFS_CG_NBFREE,
					      drv_ufs_get32(ms->cg,
							    UFS_CG_NBFREE,
							    ms->super.swapped) +
						      1U,
					      ms->super.swapped);
				ms->super.cstotal_nbfree = old_total;
				error = write_cg_rollback(mountp, error);
			} else {
				*result = cgstart(&ms->super, cg) + fragment;
				ms->rotor_cg = cg;
			}
			break;
		}

		/* Checks the operation status. */
		if (error != ENOSPC)
			break;
	}
	mutex_unlock(&ms->lock);

	/* Checks the operation status. */
	if (error == 0)
		quota_commit(&charge);
	else
		quota_rollback(&charge);

	/* Returns the computed result. */
	return error;
}

/*
 * Immediate compatibility scope; p011 adds bounded deferred metadata ownership.
 */
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

	/* Handles the context condition. */
	if (!context->active)
		return EINVAL;
	io_stats_record(IO_UFS_ALLOCATE, state(context->mountp)->super.bsize);
	error = allocate_block_compat(context->mountp, context->uid,
				      context->gid, result);

	/* Returns the computed result. */
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

static int allocate_block(struct mount *mountp, uid_t uid, gid_t gid, uint64_t *result);

/* Routes every block allocation through the explicit immediate scope. */
static int
allocate_block(
	struct mount *mountp,
	uid_t uid,
	gid_t gid,
	uint64_t *result)
{
	struct ufs_allocation context;
	int error;

	allocation_begin(&context, mountp, uid, gid);
	error = allocation_allocate(&context, result);

	/* Checks the operation status. */
	if (error != 0) {
		allocation_abort(&context);

		/* Returns the computed result. */
		return error;
	}
	allocation_commit(&context);

	/* Reports successful completion. */
	return 0;
}

static int free_block(struct mount *mountp, uint64_t fragment, uid_t uid, gid_t gid);

/* Supports the free block operation. */
static int
free_block(
	struct mount *mountp,
	uint64_t fragment,
	uid_t uid,
	gid_t gid)
{
	uint64_t start;
	uint32_t ndblk;
	struct ufs_mount_state *ms = state(mountp);
	uint8_t *map;
	uint32_t n, free, cg, local = 0;
	uint64_t old_total;
	int error;

	/* Process each element required by the operation. */
	for (cg = 0; cg < ms->super.ncg; cg++) {
		start = cgstart(&ms->super, cg);
		ndblk = cg_ndblk(&ms->super, cg);

		/* Handles the fragment condition. */
		if (fragment >= start + ms->super.dblkno &&
		    fragment + ms->super.frag <= start + ndblk) {
			local = (uint32_t)(fragment - start);
			break;
		}
	}

	/* Handles the cg condition. */
	if (cg == ms->super.ncg)
		return EIO;
	mutex_lock(&ms->lock);
	error = load_cg_locked(mountp, cg);

	/* Checks the operation status. */
	if (error != 0) {
		mutex_unlock(&ms->lock);

		/* Returns the computed result. */
		return error;
	}
	map = ms->cg + ms->cg_freeoff;
	/* Process each element required by the operation. */
	for (n = 0; n < ms->super.frag; n++) {
		/* Handles the bit test condition. */
		if (bit_test(map, local + n)) {
			mutex_unlock(&ms->lock);

			/* Returns the computed result. */
			return EIO;
		}
	}
	free = drv_ufs_get32(ms->cg, UFS_CG_NBFREE, ms->super.swapped);

	/* Handles the free condition. */
	if (free == UINT32_MAX) {
		mutex_unlock(&ms->lock);

		/* Returns the computed result. */
		return EIO;
	}
	old_total = ms->super.cstotal_nbfree;
	/* Process each element required by the operation. */
	for (n = 0; n < ms->super.frag; n++)
		bit_set(map, local + n);
	drv_ufs_put32(ms->cg, UFS_CG_NBFREE, free + 1U, ms->super.swapped);
	ms->super.cstotal_nbfree = old_total + 1U;
	error = write_cg(mountp);

	/* Checks the operation status. */
	if (error != 0) {
		/* Process each element required by the operation. */
		for (n = 0; n < ms->super.frag; n++)
			bit_clear(map, local + n);
		drv_ufs_put32(ms->cg, UFS_CG_NBFREE, free, ms->super.swapped);
		ms->super.cstotal_nbfree = old_total;
		error = write_cg_rollback(mountp, error);
	}
	mutex_unlock(&ms->lock);

	/* Checks the operation status. */
	if (error == 0 && quota_release(&ms->quota, uid, gid, 1, 0) != 0) {
		ms->writable = 0;

		/* Returns the computed result. */
		return EIO;
	}

	/* Returns the computed result. */
	return error;
}

static int allocate_inode_number(struct mount *mountp, uid_t uid, gid_t gid, uint32_t *number);

/* Supports the allocate inode number operation. */
static int
allocate_inode_number(
	struct mount *mountp,
	uid_t uid,
	gid_t gid,
	uint32_t *number)
{
	uint32_t free;
	struct ufs_mount_state *ms = state(mountp);
	struct quota_charge charge;
	uint8_t *map;
	uint32_t ino, cg, attempt;
	uint64_t old_total;
	int error;

	error = quota_reserve(&ms->quota, uid, gid, 0, 1, quota_now(), &charge);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = ENOSPC;
	mutex_lock(&ms->lock);
	/* Process each element required by the operation. */
	for (attempt = 0; attempt < ms->super.ncg; attempt++) {
		cg = (ms->rotor_cg + attempt) % ms->super.ncg;
		error = load_cg_locked(mountp, cg);

		/* Checks the operation status. */
		if (error != 0)
			break;
		error = ENOSPC;
		map = ms->cg + ms->cg_iusedoff;
		/* Process each element required by the operation. */
		for (ino = cg == 0 ? UFS_ROOT_INO + 1U : 0U;
		     ino < ms->super.ipg; ino++) {
			/* Checks the bit test result. */
			if (!bit_test(map, ino)) {
				free = drv_ufs_get32(ms->cg, UFS_CG_NIFREE,
						     ms->super.swapped);

				/* Handles the free condition. */
				if (free == 0)
					break;
				old_total = ms->super.cstotal_nifree;
				bit_set(map, ino);
				drv_ufs_put32(ms->cg, UFS_CG_NIFREE, free - 1U,
					      ms->super.swapped);
				ms->super.cstotal_nifree = old_total - 1U;
				error = write_cg(mountp);

				/* Checks the operation status. */
				if (error != 0) {
					bit_clear(map, ino);
					drv_ufs_put32(ms->cg, UFS_CG_NIFREE,
						      free, ms->super.swapped);
					ms->super.cstotal_nifree = old_total;
					error = write_cg_rollback(mountp,
								  error);
				} else {
					*number = cg * ms->super.ipg + ino;
					ms->rotor_cg = cg;
				}
				break;
			}
		}

		/* Checks the operation status. */
		if (error != ENOSPC)
			break;
	}
	mutex_unlock(&ms->lock);

	/* Checks the operation status. */
	if (error == 0)
		quota_commit(&charge);
	else
		quota_rollback(&charge);

	/* Returns the computed result. */
	return error;
}

static int free_inode_number(struct mount *mountp, uint32_t number, uid_t uid, gid_t gid);

/* Supports the free inode number operation. */
static int
free_inode_number(
	struct mount *mountp,
	uint32_t number,
	uid_t uid,
	gid_t gid)
{
	struct ufs_mount_state *ms = state(mountp);
	uint8_t *map;
	uint32_t free, cg = number / ms->super.ipg,
		       local = number % ms->super.ipg;
	uint64_t old_total;
	int error;

	/* Handles the number condition. */
	if (number <= UFS_ROOT_INO || cg >= ms->super.ncg)
		return EIO;
	mutex_lock(&ms->lock);
	error = load_cg_locked(mountp, cg);

	/* Checks the operation status. */
	if (error != 0) {
		mutex_unlock(&ms->lock);

		/* Returns the computed result. */
		return error;
	}
	map = ms->cg + ms->cg_iusedoff;

	/* Checks the bit test result. */
	if (!bit_test(map, local)) {
		mutex_unlock(&ms->lock);

		/* Returns the computed result. */
		return EIO;
	}
	free = drv_ufs_get32(ms->cg, UFS_CG_NIFREE, ms->super.swapped);

	/* Handles the free condition. */
	if (free == UINT32_MAX) {
		mutex_unlock(&ms->lock);

		/* Returns the computed result. */
		return EIO;
	}
	old_total = ms->super.cstotal_nifree;
	bit_clear(map, local);
	drv_ufs_put32(ms->cg, UFS_CG_NIFREE, free + 1U, ms->super.swapped);
	ms->super.cstotal_nifree = old_total + 1U;
	error = write_cg(mountp);

	/* Checks the operation status. */
	if (error != 0) {
		bit_set(map, local);
		drv_ufs_put32(ms->cg, UFS_CG_NIFREE, free, ms->super.swapped);
		ms->super.cstotal_nifree = old_total;
		error = write_cg_rollback(mountp, error);
	}
	mutex_unlock(&ms->lock);

	/* Checks the operation status. */
	if (error == 0 && quota_release(&ms->quota, uid, gid, 0, 1) != 0) {
		ms->writable = 0;

		/* Returns the computed result. */
		return EIO;
	}

	/* Returns the computed result. */
	return error;
}

static int indirect_entry(struct mount *mountp, uint64_t fragment, uint32_t index, uint64_t *result);

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

	/* Handles the fragment condition. */
	if (fragment == 0) {
		*result = 0;
		/* Reports successful completion. */
		return 0;
	}

	/* Checks the current index. */
	if (index >= super->nindir || fragment >= super->size ||
	    super->frag > super->size - fragment)

		/* Returns the computed result. */
		return EIO;
	byte_offset = (uint64_t)index * 8U;
	lba = ((uint64_t)fragment << super->fsbtodb) +
	      byte_offset / UFS_SECTOR_SIZE;
	io_stats_record(IO_UFS_INDIRECT_WINDOW, sizeof(sector));
	error = read_metadata_sectors(mountp, lba, 1, sector);

	/* Checks the operation status. */
	if (error == 0) {
		*result = drv_ufs_get64(sector,
					(size_t)(byte_offset % UFS_SECTOR_SIZE),
					super->swapped);
	}

	/*
 * Releases the common cache pin inside disk_read before returning the
	 * pointer. */
	return error;
}

static int bmap(struct inode *inode, uint64_t logical, uint64_t *result);

/* Supports the bmap operation. */
static int
bmap(
	struct inode *inode,
	uint64_t logical,
	uint64_t *result)
{
	uint64_t divisor;
	uint32_t index;
	unsigned n;
	struct ufs_inode_info *ui = info(inode);
	const struct ufs_super *s = &state(inode->i_mount)->super;
	uint64_t span = s->nindir;
	uint64_t fragment;
	unsigned level, depth;
	int error;

	/* Handles the logical condition. */
	if (logical < UFS_NDADDR) {
		*result = ui->direct[logical];
		/* Reports successful completion. */
		return 0;
	}
	logical -= UFS_NDADDR;
	/* Process each element required by the operation. */
	for (level = 0; level < UFS_NIADDR; level++) {
		/* Handles the logical condition. */
		if (logical < span)
			break;
		logical -= span;

		/* Handles the span condition. */
		if (span > UINT64_MAX / s->nindir)
			return EOVERFLOW;
		span *= s->nindir;
	}

	/* Handles the level condition. */
	if (level == UFS_NIADDR)
		return EFBIG;
	fragment = ui->indirect[level];
	/* Process each element required by the operation. */
	for (depth = level + 1U; depth != 0; depth--) {
		divisor = 1;

		/* Process each element required by the operation. */
		for (n = 1; n < depth; n++)
			divisor *= s->nindir;
		index = (uint32_t)(logical / divisor);
		logical %= divisor;
		error = indirect_entry(inode->i_mount, fragment, index,
				       &fragment);

		/* Checks the operation status. */
		if (error != 0 || fragment == 0)
			break;
	}
	*result = fragment;
	/* Returns the computed result. */
	return error;
}

static int bmap_ensure(struct inode *inode, uint64_t logical, uint64_t *result);

/* Supports the bmap ensure operation. */
static int
bmap_ensure(
	struct inode *inode,
	uint64_t logical,
	uint64_t *result)
{
	uint64_t allocated_local;
	int rollback_local;
	uint64_t allocated_local2;
	int rollback_local1;
	uint64_t allocated_local3;
	int rollback_error;
	uint8_t *block;
	uint64_t divisor;
	uint32_t index;
	uint64_t next;
	unsigned n;
	struct ufs_inode_info *ui = info(inode);
	const struct ufs_super *s = &state(inode->i_mount)->super;
	uint64_t span = s->nindir;
	uint64_t *root, fragment;
	unsigned level, depth;
	int error;

	/* Handles the logical condition. */
	if (logical < UFS_NDADDR) {
		/* Handles the ui condition. */
		if (ui->direct[logical] == 0) {
			error = allocate_block(inode->i_mount, inode->i_uid,
					       inode->i_gid, &allocated_local);

			/* Checks the operation status. */
			if (error != 0)
				return error;
			ui->direct[logical] = allocated_local;
			ui->blocks += s->bsize / UFS_SECTOR_SIZE;

			/*
 * Make the allocation reachable before user data I/O.
			 */
			error = persist_inode(inode);

			/* Checks the operation status. */
			if (error != 0) {
				ui->direct[logical] = 0;
				ui->blocks -= s->bsize / UFS_SECTOR_SIZE;

				/*
 * Failure can follow a committed write
				 * (including replay). Confirm pointer removal
				 * before recycling the allocation. */
				rollback_local = persist_inode(inode);

				/* Handles the rollback local condition. */
				if (rollback_local == 0) {
					rollback_local = disk_sync(
						inode->i_mount->m_disk);
				}

				/* Handles the rollback local condition. */
				if (rollback_local == 0) {
					rollback_local = free_block(
						inode->i_mount, allocated_local,
						inode->i_uid, inode->i_gid);
				}

				/* Handles the rollback local condition. */
				if (rollback_local != 0)
					state(inode->i_mount)->writable = 0;

				/* Returns the computed result. */
				return error;
			}
		}
		*result = ui->direct[logical];
		/* Reports successful completion. */
		return 0;
	}
	logical -= UFS_NDADDR;
	/* Process each element required by the operation. */
	for (level = 0; level < UFS_NIADDR; level++) {
		/* Handles the logical condition. */
		if (logical < span)
			break;
		logical -= span;

		/* Handles the span condition. */
		if (span > UINT64_MAX / s->nindir)
			return EOVERFLOW;
		span *= s->nindir;
	}

	/* Handles the level condition. */
	if (level == UFS_NIADDR)
		return EFBIG;
	root = &ui->indirect[level];

	/* Handles the root condition. */
	if (*root == 0) {
		error = allocate_block(inode->i_mount, inode->i_uid,
				       inode->i_gid, &allocated_local2);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		*root = allocated_local2;
		ui->blocks += s->bsize / UFS_SECTOR_SIZE;
		error = persist_inode(inode);

		/* Checks the operation status. */
		if (error != 0) {
			*root = 0;
			ui->blocks -= s->bsize / UFS_SECTOR_SIZE;

			/*
 * Failure can follow a committed write (including
			 * replay). Confirm pointer removal before recycling the
			 * allocation. */
			rollback_local1 = persist_inode(inode);

			/* Handles the rollback local1 condition. */
			if (rollback_local1 == 0)
				rollback_local1 =
					disk_sync(inode->i_mount->m_disk);

			/* Handles the rollback local1 condition. */
			if (rollback_local1 == 0) {
				rollback_local1 = free_block(
					inode->i_mount, allocated_local2,
					inode->i_uid, inode->i_gid);
			}

			/* Handles the rollback local1 condition. */
			if (rollback_local1 != 0)
				state(inode->i_mount)->writable = 0;

			/* Returns the computed result. */
			return error;
		}
	}
	fragment = *root;
	/* Process each element required by the operation. */
	for (depth = level + 1U; depth != 0; depth--) {
		divisor = 1;

		/* Process each element required by the operation. */
		for (n = 1; n < depth; n++)
			divisor *= s->nindir;
		index = (uint32_t)(logical / divisor);
		logical %= divisor;
		block = kern_malloc(s->bsize);

		/* Handles the block availability. */
		if (block == NULL)
			return ENOMEM;
		error = read_block(inode->i_mount, fragment, block);

		/* Checks the operation status. */
		if (error != 0) {
			kern_free(block);

			/* Returns the computed result. */
			return error;
		}
		next = drv_ufs_get64(block, (size_t)index * 8U, s->swapped);

		/* Handles the next condition. */
		if (next == 0) {
			allocated_local3 = 0;
			error = allocate_block(inode->i_mount, inode->i_uid,
					       inode->i_gid, &next);

			/* Checks the operation status. */
			if (error == 0) {
				allocated_local3 = next;
				drv_ufs_put64(block, (size_t)index * 8U, next,
					      s->swapped);
				error = write_block(inode->i_mount, fragment,
						    block);
			}

			/* Checks the operation status. */
			if (error != 0) {
				/* Handles the allocated local3 condition. */
				if (allocated_local3 != 0) {
					/*
 * A short write may have published the
					 * pointer even though write_block()
					 * reported EIO.  Make it unreachable
					 * before returning its block. */
					drv_ufs_put64(block, (size_t)index * 8U,
						      0, s->swapped);
					rollback_error =
						write_block(inode->i_mount,
							    fragment, block);

					/* Checks the operation status. */
					if (rollback_error == 0) {
						rollback_error = disk_sync(
							inode->i_mount->m_disk);
					}

					/* Checks the operation status. */
					if (rollback_error == 0) {
						rollback_error = free_block(
							inode->i_mount,
							allocated_local3,
							inode->i_uid,
							inode->i_gid);
					}

					/* Checks the operation status. */
					if (rollback_error != 0) {
						/*
 * The block may remain
						 * reachable.  Never free
						 * uncertain storage or continue
						 * writable. */
						ui->blocks += s->bsize /
							      UFS_SECTOR_SIZE;
						state(inode->i_mount)
							->writable = 0;
					}
				}
				kern_free(block);

				/* Returns the computed result. */
				return error;
			}
			ui->blocks += s->bsize / UFS_SECTOR_SIZE;
		}
		kern_free(block);
		fragment = next;
	}
	*result = fragment;
	/* Reports successful completion. */
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

	/*
 * Bounds the mapping scan to one indirect leaf and the common byte
	 * limit. */
	*mapping_error = 0;
	super = &state(inode->i_mount)->super;

	/* Handles the first condition. */
	if (first == 0 || first >= super->size ||
	    super->frag > super->size - first)

		/* Reports successful completion. */
		return 0;
	maximum = remaining / super->bsize;

	/* Handles the maximum condition. */
	if (maximum > KERN_IO_BATCH_MAX / super->bsize)
		maximum = KERN_IO_BATCH_MAX / super->bsize;

	/* Handles the logical condition. */
	if (logical < UFS_NDADDR)
		boundary = UFS_NDADDR - logical;
	else
		boundary =
			super->nindir - (logical - UFS_NDADDR) % super->nindir;

	/* Handles the maximum condition. */
	if (maximum > boundary)
		maximum = boundary;

	/* Handles the maximum condition. */
	if (maximum > (super->size - first) / super->frag)
		maximum = (super->size - first) / super->frag;

	/* Preserves the existing journal's per-transaction payload capacity. */
	if (writing && state(inode->i_mount)->journal_enabled) {
		journal_blocks = state(inode->i_mount)->journal.sector_count;
		journal_blocks = journal_blocks > 2U ? journal_blocks - 2U : 0;
		journal_blocks /= super->bsize / UFS_SECTOR_SIZE;

		/* Handles the maximum condition. */
		if (maximum > journal_blocks)
			maximum = journal_blocks;
	}

	/* Stops before a hole, discontinuity or a failed optional lookahead. */
	if (maximum == 0)
		return 0;
	/* Process each element required by the operation. */
	for (blocks = 1; blocks < maximum; blocks++) {
		error = bmap(inode, logical + blocks, &next);

		/* Checks the operation status. */
		if (error != 0) {
			*mapping_error = error;
			break;
		}

		/* Handles the next condition. */
		if (next == 0 || next != first + blocks * super->frag)
			break;
	}

	/* Returns only the validated contiguous byte span. */
	return (size_t)blocks * super->bsize;
}

static ssize_t pread_inode(struct inode *inode, void *buffer, size_t length, off_t offset);

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

	/* Checks the current offset. */
	if (offset < 0)
		return -EINVAL;

	/* Checks the current offset. */
	if (offset >= inode->i_size || length == 0)
		return 0;

	/* Handles the uint64 t condition. */
	if ((uint64_t)length > (uint64_t)inode->i_size - (uint64_t)offset)
		length = (size_t)((uint64_t)inode->i_size - (uint64_t)offset);
	scratch = NULL;
	done = 0;

	/*
 * Uses caller storage for complete blocks in each validated mapped run.
	 */
	/* Process each remaining element. */
	while (done < length) {
		position = (uint64_t)offset + done;
		logical = position / super->bsize;
		within = (size_t)(position % super->bsize);
		error = bmap(inode, logical, &fragment);

		/* Checks the operation status. */
		if (error != 0) {
			kern_free(scratch);

			/* Returns the computed result. */
			return done != 0 ? (ssize_t)done : -error;
		}
		mapping_error = 0;
		amount = within == 0
				 ? content_run_bytes(inode, logical, fragment,
						     length - done, 0,
						     &mapping_error)
				 : 0;

		/* Handles the amount condition. */
		if (amount != 0) {
			io_stats_record(IO_UFS_CONTENT_READ, amount);
			error = observed_disk_read(
				inode->i_mount->m_disk,
				(uint64_t)fragment << super->fsbtodb,
				(uint32_t)(amount / UFS_SECTOR_SIZE),
				(uint8_t *)buffer + done);
		} else {
			/*
 * Allocates edge scratch only when the request needs
			 * it. */
			if (scratch == NULL) {
				scratch = kern_malloc(super->bsize);

				/* Handles the scratch availability. */
				if (scratch == NULL) {
					return done != 0 ? (ssize_t)done
							 : -ENOMEM;
				}
			}
			amount = super->bsize - within;

			/* Handles the amount condition. */
			if (amount > length - done)
				amount = length - done;
			error = read_content_block(inode->i_mount, fragment,
						   scratch);

			/* Checks the operation status. */
			if (error == 0) {
				memcpy((uint8_t *)buffer + done,
				       scratch + within, amount);
			}
		}

		/* Checks the operation status. */
		if (error != 0) {
			kern_free(scratch);

			/* Returns the computed result. */
			return done != 0 ? (ssize_t)done : -error;
		}
		done += amount;

		/* Checks the operation status. */
		if (mapping_error != 0)
			break;
	}
	kern_free(scratch);

	/* Reports the successfully read prefix. */
	return (ssize_t)done;
}

/* Locates the shared on-disk block containing a dinode. */
static uint64_t
inode_fragment(
	struct inode *inode)
{
	uint64_t function_result;
	struct ufs_mount_state *ms = state(inode->i_mount);
	uint32_t number = (uint32_t)inode->i_ino;
	uint32_t cg = number / ms->super.ipg;
	uint32_t index = number % ms->super.ipg;

	/* Computes the function result. */
	function_result = cgstart(&ms->super, cg) + ms->super.iblkno +
			  (index / ms->super.inopb) * ms->super.frag;

	/* Returns the computed result. */
	return function_result;
}

/* Patches only one prepared dinode into a caller-owned shared block image. */
static void
encode_inode_locked(
	struct inode *inode,
	uint8_t *block)
{
	struct ufs_inode_info *ui = info(inode);
	struct ufs_mount_state *ms = state(inode->i_mount);
	uint32_t index = (uint32_t)inode->i_ino % ms->super.ipg;
	uint8_t *raw;
	unsigned n;

	/* Writes the fixed on-disk fields in the volume's byte order. */
	raw = block + (index % ms->super.inopb) * UFS_DINODE_SIZE;
	drv_ufs_put16(raw, UFS_DI_MODE, (uint16_t)inode->i_mode,
		      ms->super.swapped);
	drv_ufs_put16(raw, UFS_DI_NLINK, (uint16_t)inode->i_linkcount,
		      ms->super.swapped);
	drv_ufs_put64(raw, UFS_DI_SIZE, (uint64_t)inode->i_size,
		      ms->super.swapped);
	drv_ufs_put64(raw, UFS_DI_ATIME, (uint64_t)inode->i_atime.tv_sec,
		      ms->super.swapped);
	drv_ufs_put32(raw, UFS_DI_ATIMENSEC, (uint32_t)inode->i_atime.tv_nsec,
		      ms->super.swapped);
	drv_ufs_put64(raw, UFS_DI_MTIME, (uint64_t)inode->i_mtime.tv_sec,
		      ms->super.swapped);
	drv_ufs_put32(raw, UFS_DI_MTIMENSEC, (uint32_t)inode->i_mtime.tv_nsec,
		      ms->super.swapped);
	drv_ufs_put64(raw, UFS_DI_CTIME, (uint64_t)inode->i_ctime.tv_sec,
		      ms->super.swapped);
	drv_ufs_put32(raw, UFS_DI_CTIMENSEC, (uint32_t)inode->i_ctime.tv_nsec,
		      ms->super.swapped);
	drv_ufs_put32(raw, UFS_DI_EXTSIZE, ui->extattr_size, ms->super.swapped);
	/* Process each element required by the operation. */
	for (n = 0; n < UFS_NXADDR; n++) {
		drv_ufs_put64(raw, UFS_DI_EXTB + n * 8U, ui->extattr[n],
			      ms->super.swapped);
	}

	/* Handles the inode condition. */
	if (inode->i_type == INODE_CHAR || inode->i_type == INODE_BLOCK) {
		memset(raw + UFS_DI_DB, 0, 120U);
		drv_ufs_put64(raw, UFS_DI_DB, (uint64_t)inode->i_rdev,
			      ms->super.swapped);
		/* Process each element required by the operation. */
		for (n = 0; n < UFS_NIADDR; n++) {
			drv_ufs_put64(raw, UFS_DI_IB + n * 8U, 0,
				      ms->super.swapped);
		}
	} else if (inode->i_type == INODE_SYMLINK &&
		   (uint64_t)inode->i_size <= ms->super.maxsymlinklen &&
		   inode->i_size <= 120) {
		memset(raw + UFS_DI_DB, 0, 120U);
		memcpy(raw + UFS_DI_DB, ui->shortlink, (size_t)inode->i_size);
	} else {
		/* Process each element required by the operation. */
		for (n = 0; n < UFS_NDADDR; n++) {
			drv_ufs_put64(raw, UFS_DI_DB + n * 8U, ui->direct[n],
				      ms->super.swapped);
		}
		/* Process each element required by the operation. */
		for (n = 0; n < UFS_NIADDR; n++) {
			drv_ufs_put64(raw, UFS_DI_IB + n * 8U, ui->indirect[n],
				      ms->super.swapped);
		}
	}
	drv_ufs_put64(raw, UFS_DI_BLOCKS, ui->blocks, ms->super.swapped);
	drv_ufs_put32(raw, UFS_DI_UID, inode->i_uid, ms->super.swapped);
	drv_ufs_put32(raw, UFS_DI_GID, inode->i_gid, ms->super.swapped);
}

/* Loads shared bytes once before encoding a private or public inode image. */
static int
prepare_inode_locked(
	struct inode *inode,
	uint8_t *block,
	uint64_t *location)
{
	int error;

	*location = inode_fragment(inode);
	error = read_block(inode->i_mount, *location, block);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	encode_inode_locked(inode, block);

	/* Reports successful completion. */
	return 0;
}

/* Writes one prepared shared-dinode image while preserving mount exclusion. */
static int
persist_inode_locked(
	struct inode *inode)
{
	uint8_t *block;
	uint64_t fragment;
	int error;

	block = kern_malloc(state(inode->i_mount)->super.bsize);

	/* Handles the block availability. */
	if (block == NULL)
		return ENOMEM;
	error = prepare_inode_locked(inode, block, &fragment);

	/* Checks the operation status. */
	if (error == 0)
		error = write_block(inode->i_mount, fragment, block);
	kern_free(block);

	/* Returns the computed result. */
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

/* Begin consolidated ufs-transaction.inc. */
/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

struct ufs_transaction_outcome {
	unsigned committed;
	unsigned uncertain;
};

/* Owns one editable image per physical block during a metadata operation. */
struct ufs_metadata_images {
	struct mount *mountp;
	uint8_t *memory;
	size_t capacity;
	unsigned count;
	struct ufs_journal_extent extents[UFS_JOURNAL_EXTENTS];
};

static void metadata_images_init(struct ufs_metadata_images *images, struct mount *mountp, uint8_t *memory, size_t bytes);
static int metadata_image_get(struct ufs_metadata_images *images, uint64_t fragment, uint8_t **result);
static int metadata_image_inode(struct ufs_metadata_images *images, struct inode *prepared);
static int metadata_group_commit(struct mount *mountp, const struct ufs_journal_extent *extents, unsigned count, const struct io_context *context, struct ufs_transaction_outcome *outcome);

/* Serializes snapshot preservation, exact commit outcome and metadata durability. */
static int
metadata_group_commit(
	struct mount *mountp,
	const struct ufs_journal_extent *extents,
	unsigned count,
	const struct io_context *context,
	struct ufs_transaction_outcome *outcome)
{
	struct ufs_mount_state *ms = state(mountp);
	struct io_context child;
	uint64_t sequence;
	unsigned n;
	int error, deferred;

	memset(outcome, 0, sizeof(*outcome));

	/*
 * Carries the logical owner through every snapshot and journal
	 * durability step. */
	error = io_context_child(&child, context, IO_CONTEXT_ORDERED);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/*
 * Policy-off drains under ms->lock after closing this admission query.
	 */
	deferred = context == NULL && writeback_mount_active(mountp);
	io_epoch_begin(&mountp->m_write_epoch);

	/* Handles the ms condition. */
	if (ms->snapshot_available) {
		mutex_lock(&ms->snapshot_lock);
		ms->snapshot_io.context = &child;

		/*
 * Preserves all original homes before a grouped checkpoint may
		 * overwrite any. */
		/* Process each remaining element. */
		for (n = 0; n < count; n++) {
			error = drv_ufs_snapshot_preserve(&ms->snapshot,
							  extents[n].target,
							  extents[n].sectors);

			/* Checks the operation status. */
			if (error != 0)
				break;
		}
		ms->snapshot_io.context = NULL;
	}

	/* Checks the operation status. */
	if (error == 0) {
		mutex_lock(&ms->journal_lock);
		error = journal_checkpoint_locked(mountp);
		sequence = 0;

		/* Checks the operation status. */
		if (error == 0) {
			journal_wait_readers(ms);
			ms->journal_io.context = &child;
			sequence = ms->journal.next_sequence;

			/* Handles the deferred condition. */
			if (deferred) {
				error = drv_ufs_journal_publishv(
					&ms->journal, extents, count);

				/* Checks the operation status. */
				if (error != 0 &&
				    ms->journal.pending_sequence != 0) {
					(void)drv_ufs_journal_drain(
						&ms->journal);
				}
			} else {
				error = drv_ufs_journal_commitv(&ms->journal,
								extents, count);
			}
		}

		/*
 * Sequence identity is unique until init; inspect before
		 * releasing admission. */
		outcome->committed = sequence != 0 &&
				     ms->journal.committed_sequence == sequence;
		outcome->uncertain = ms->journal.poisoned ||
				     (ms->journal.pending_sequence != 0 &&
				      !(error == 0 && outcome->committed &&
					ms->journal.pending_ready));

		/* Handles the outcome condition. */
		if (outcome->uncertain)
			ms->writable = 0;
		ms->journal_io.context = NULL;
		mutex_unlock(&ms->journal_lock);
	}

	/* Handles the ms condition. */
	if (ms->snapshot_available)
		mutex_unlock(&ms->snapshot_lock);
	io_epoch_end(&mountp->m_write_epoch);

	/*
 * Returns the original errno independently of positive committed
	 * ownership. */
	return error;
}

/* Bounds a caller-owned image set by both memory and journal capacity. */
static void
metadata_images_init(
	struct ufs_metadata_images *images,
	struct mount *mountp,
	uint8_t *memory,
	size_t bytes)
{
	struct ufs_mount_state *ms;
	size_t sectors;

	/*
 * Initializes an empty owner without allocating or publishing metadata.
	 */
	memset(images, 0, sizeof(*images));
	ms = state(mountp);
	images->mountp = mountp;
	images->memory = memory;
	sectors = bytes / UFS_SECTOR_SIZE;

	/*
 * Restricts admission to the active journal's payload and descriptor
	 * limits. */
	if (sectors > UFS_JOURNAL_GROUP_SECTORS)
		sectors = UFS_JOURNAL_GROUP_SECTORS;

	/* Handles the ms condition. */
	if (ms->journal.sector_count <= 2U)
		sectors = 0;
	else if (sectors > ms->journal.sector_count - 2U)
		sectors = ms->journal.sector_count - 2U;
	images->capacity = sectors * UFS_SECTOR_SIZE / ms->super.bsize;

	/* Handles the images condition. */
	if (images->capacity > UFS_JOURNAL_EXTENTS)
		images->capacity = UFS_JOURNAL_EXTENTS;
}

/* Returns the unique private image of a block without losing earlier edits. */
static int
metadata_image_get(
	struct ufs_metadata_images *images,
	uint64_t fragment,
	uint8_t **result)
{
	struct ufs_mount_state *ms;
	struct ufs_journal_extent *extent;
	uint8_t *block;
	uint64_t sector;
	uint32_t sectors;
	unsigned n;
	int error;

	/*
 * Validates the physical extent before matching it against existing
	 * owners. */
	*result = NULL;
	ms = state(images->mountp);
	sectors = ms->super.bsize / UFS_SECTOR_SIZE;

	/* Handles the fragment condition. */
	if (fragment == 0 || ms->super.fsbtodb >= 64U ||
	    fragment > (UINT64_MAX >> ms->super.fsbtodb))

		/* Returns the computed result. */
		return EIO;
	sector = fragment << ms->super.fsbtodb;

	/* Handles the sector condition. */
	if (sector > UINT64_MAX - sectors)
		return EIO;

	/*
 * Reuses identical blocks and rejects aliasing through a partial
	 * overlap. */
	/* Process each remaining element. */
	for (n = 0; n < images->count; n++) {
		extent = &images->extents[n];

		/* Handles the extent condition. */
		if (extent->target == sector) {
			*result = images->memory + n * ms->super.bsize;
			/* Reports successful completion. */
			return 0;
		}

		/* Handles the sector condition. */
		if (sector < extent->target + extent->sectors &&
		    extent->target < sector + sectors)

			/* Returns the computed result. */
			return EIO;
	}

	/*
 * Reads a new image only when its full block fits the reserved
	 * transaction. */
	if (images->count >= images->capacity)
		return ENOSPC;
	block = images->memory + images->count * ms->super.bsize;
	error = read_block(images->mountp, fragment, block);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/*
 * Admits only successfully loaded bytes and keeps the extent order
	 * stable. */
	extent = &images->extents[images->count];
	extent->target = sector;
	extent->sectors = sectors;
	extent->payload = block;
	images->count++;
	*result = block;

	/*
 * Returns the image for in-place private preparation under the mount
	 * lock. */
	return 0;
}

/* Merges a prepared dinode into the unique image of its containing block. */
static int
metadata_image_inode(
	struct ufs_metadata_images *images,
	struct inode *prepared)
{
	uint8_t *block;
	int error;

	/* Rejects a foreign inode before interpreting its physical location. */
	if (prepared->i_mount != images->mountp)
		return EXDEV;

	/* Preserves any sibling edits already present in this private block. */
	error = metadata_image_get(images, inode_fragment(prepared), &block);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	encode_inode_locked(prepared, block);

	/*
 * Leaves live inode publication to the operation's committed outcome.
	 */
	return 0;
}
/* End consolidated ufs-transaction.inc. */

/* Begin consolidated ufs-allocation.inc. */
/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Bound operation-local ownership independently of filesystem geometry. */
#define UFS_ALLOCATION_BLOCKS 16U
#define UFS_ALLOCATION_BYTES 65536U

struct ufs_allocation_run {
	struct ufs_inode_info image;
	struct quota_charge charges[UFS_ALLOCATION_BLOCKS];
	uint8_t *memory;
	uint8_t *old_cg;
	uint8_t *old_leaf;
	uint8_t *new_leaf;
	uint8_t *dinode;
	uint8_t *summaries;
	uint8_t *tree_memory;
	uint8_t *tree_images[UFS_NIADDR];
	uint64_t tree_targets[UFS_NIADDR];
	unsigned tree_indices[UFS_NIADDR];
	unsigned tree_count;
	unsigned missing_nodes;
	unsigned missing_root;
	unsigned root_level;
	uint64_t leaf;
	uint64_t first;
	uint64_t old_total;
	unsigned index;
	unsigned count;
	unsigned reserved;
	unsigned published;
	unsigned grouped;
	unsigned committed;
	unsigned uncertain;
};

static int allocation_missing_path(struct ufs_allocation_run *run, const struct ufs_super *super, uint64_t logical, unsigned depth);
static int allocation_group_commit(struct inode *inode, struct ufs_allocation_run *run, const struct io_context *context);
static int allocation_run_leaf(struct inode *inode, uint64_t logical, unsigned *count, struct ufs_allocation_run *run);
static int allocation_run_reserve(struct inode *inode, struct ufs_allocation_run *run);
static int allocation_run_abort(struct inode *inode, struct ufs_allocation_run *run);
static void allocation_run_release(struct ufs_allocation_run *run, int retained);
static ssize_t allocation_write_run(struct inode *inode, const void *buffer, size_t length, uint64_t logical, const struct io_context *context);

/* Commits allocation ownership and references under one exact journal owner. */
static int
allocation_group_commit(
	struct inode *inode,
	struct ufs_allocation_run *run,
	const struct io_context *context)
{
	struct mount *mountp;
	struct ufs_mount_state *ms;
	struct ufs_journal_extent extents[3 + UFS_NIADDR];
	struct kern_test_fault_result fault;
	struct ufs_transaction_outcome outcome;
	uint64_t fragment;
	unsigned count;
	unsigned n;
	int error;

	/*
 * Prepares only this operation's dinode while the shared mount lock is
	 * held. */
	mountp = inode->i_mount;
	ms = state(mountp);
	error = prepare_inode_locked(&run->image.inode, run->dinode, &fragment);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = prepare_super_summaries(mountp, run->summaries);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the fault condition. */
	if (KERN_TEST_FAULT(KERN_TEST_FAULT_UFS_CG_WRITE, UINT32_MAX,
			    UINT32_MAX, &fault))

		/* Returns the computed result. */
		return fault.error != 0 ? fault.error : EIO;

	/*
 * Describes disjoint shared blocks; core validation precedes every redo
	 * write. */
	extents[0].target =
		(cgstart(&ms->super, ms->active_cg) + ms->super.cblkno)
		<< ms->super.fsbtodb;
	extents[0].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[0].payload = ms->cg;
	extents[1].target = UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;
	extents[1].sectors = UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;
	extents[1].payload = run->summaries;
	extents[2].target = fragment << ms->super.fsbtodb;
	extents[2].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[2].payload = run->dinode;
	count = 3;

	/* Handles the run condition. */
	if (run->tree_count != 0) {
		/*
 * Includes every newly initialized node and its changed
		 * existing parent. */
		/* Process each remaining element. */
		for (n = 0; n < run->tree_count; n++) {
			extents[count].target = run->tree_targets[n]
						<< ms->super.fsbtodb;
			extents[count].sectors =
				ms->super.bsize / UFS_SECTOR_SIZE;
			extents[count].payload = run->tree_images[n];
			count++;
		}
	} else if (run->leaf != 0) {
		extents[3].target = run->leaf << ms->super.fsbtodb;
		extents[3].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
		extents[3].payload = run->new_leaf;
		count++;
	}

	error = metadata_group_commit(mountp, extents, count, context,
				      &outcome);
	run->committed = outcome.committed;
	run->uncertain = outcome.uncertain;

	/* Returns the computed result. */
	return error;
}

/* Prepares an absent suffix without installing empty nodes or live references. */
static int
allocation_missing_path(
	struct ufs_allocation_run *run,
	const struct ufs_super *super,
	uint64_t logical,
	unsigned depth)
{
	uint64_t divisor;
	unsigned n;
	unsigned slot;

	/* Allocates all private path images before quota or bitmap mutation. */
	run->tree_memory = kern_malloc(UFS_NIADDR * super->bsize);

	/* Handles the tree memory availability. */
	if (run->tree_memory == NULL)
		return ENOMEM;
	memset(run->tree_memory, 0, UFS_NIADDR * super->bsize);
	/* Process each element required by the operation. */
	for (n = 0; n < UFS_NIADDR; n++)
		run->tree_images[n] = run->tree_memory + n * super->bsize;

	/* Handles the run condition. */
	if (run->tree_count != 0)
		memcpy(run->tree_images[0], run->old_leaf, super->bsize);
	run->missing_nodes = depth;

	/*
 * Records the pointer slot in each missing level, ending at a private
	 * leaf. */
	/* Continue while the operation condition remains true. */
	while (depth != 0) {
		divisor = 1;
		/* Process each element required by the operation. */
		for (n = 1; n < depth; n++)
			divisor *= super->nindir;
		slot = run->tree_count++;
		run->tree_indices[slot] = (unsigned)(logical / divisor);
		logical %= divisor;
		depth--;
	}
	run->index = run->tree_indices[run->tree_count - 1U];
	run->new_leaf = run->tree_images[run->tree_count - 1U];

	/*
 * Leaves every physical address and inode root unpublished until
	 * reservation. */
	return 0;
}

/* Locates a leaf or prepares its missing path without publishing empty nodes. */
static int
allocation_run_leaf(
	struct inode *inode,
	uint64_t logical,
	unsigned *count,
	struct ufs_allocation_run *run)
{
	const struct ufs_super *super;
	struct ufs_inode_info *ui;
	uint64_t span;
	uint64_t divisor;
	uint64_t fragment;
	unsigned level;
	unsigned depth;
	unsigned index;
	unsigned n;
	uint64_t parent = 0;
	unsigned parent_index = 0;
	int error;

	/* Limit direct allocations to the current array of zero pointers. */
	super = &state(inode->i_mount)->super;
	ui = info(inode);

	/* Handles the logical condition. */
	if (logical < UFS_NDADDR) {
		run->index = (unsigned)logical;
		/* Process each remaining element. */
		for (n = 0; n < *count; n++) {
			/* Handles the logical condition. */
			if (logical + n >= UFS_NDADDR ||
			    ui->direct[logical + n] != 0)
				break;
		}
		*count = n;
		/* Reports successful completion. */
		return 0;
	}

	/* Select the indirect root and its checked logical range. */
	logical -= UFS_NDADDR;
	span = super->nindir;
	/* Process each element required by the operation. */
	for (level = 0; level < UFS_NIADDR; level++) {
		/* Handles the logical condition. */
		if (logical < span)
			break;
		logical -= span;

		/* Handles the span condition. */
		if (span > UINT64_MAX / super->nindir)
			return EOVERFLOW;
		span *= super->nindir;
	}

	/*
 * Traverses existing nodes, preparing an absent suffix only for grouped
	 * owners. */
	if (level == UFS_NIADDR)
		return EFBIG;
	fragment = ui->indirect[level];
	/* Process each element required by the operation. */
	for (depth = level + 1U; depth != 0; depth--) {
		/* Handles the fragment condition. */
		if (fragment == 0) {
			/* Handles the run condition. */
			if (!run->grouped) {
				*count = 0;
				/* Reports successful completion. */
				return 0;
			}
			run->root_level = level;
			run->missing_root = parent == 0;

			/* Handles the parent condition. */
			if (parent != 0) {
				run->tree_count = 1;
				run->tree_targets[0] = parent;
				run->tree_indices[0] = parent_index;
			}
			error = allocation_missing_path(run, super, logical,
							depth);

			/* Checks the operation status. */
			if (error != 0)
				return error;

			/* Checks the remaining item count. */
			if (*count > super->nindir - run->index)
				*count = super->nindir - run->index;
			/* Reports successful completion. */
			return 0;
		}
		error = read_block(inode->i_mount, fragment, run->old_leaf);

		/* Checks the operation status. */
		if (error != 0)
			return error;

		/*
 * Consume one level while retaining the last leaf's original
		 * bytes. */
		divisor = 1;
		/* Process each element required by the operation. */
		for (n = 1; n < depth; n++)
			divisor *= super->nindir;
		index = (unsigned)(logical / divisor);
		logical %= divisor;

		/* Handles the depth condition. */
		if (depth == 1) {
			run->leaf = fragment;
			run->index = index;
			break;
		}
		parent = fragment;
		parent_index = index;
		fragment = drv_ufs_get64(run->old_leaf, index * 8U,
					 super->swapped);
	}

	/* Stop at an occupied pointer or the end of this leaf. */
	for (n = 0; n < *count; n++) {
		/* Handles the run condition. */
		if (run->index + n >= super->nindir)
			break;

		/* Checks the drv ufs get64 result. */
		if (drv_ufs_get64(run->old_leaf, (run->index + n) * 8U,
				  super->swapped) != 0)
			break;
	}
	*count = n;
	memcpy(run->new_leaf, run->old_leaf, super->bsize);

	/* Report a private, unpublished leaf image. */
	return 0;
}

/* Reserve one contiguous prefix while retaining the mount allocator lock. */
static int
allocation_run_reserve(
	struct inode *inode,
	struct ufs_allocation_run *run)
{
	struct ufs_mount_state *ms;
	uint8_t *map;
	uint32_t cg;
	uint32_t attempt;
	uint32_t fragment;
	uint32_t ndblk;
	uint32_t free_blocks;
	unsigned count;
	unsigned n;
	int error;

	/*
 * Search groups without exposing pending changes to another allocator.
	 */
	ms = state(inode->i_mount);
	/* Process each element required by the operation. */
	for (attempt = 0; attempt < ms->super.ncg; attempt++) {
		cg = (ms->rotor_cg + attempt) % ms->super.ncg;
		error = load_cg_locked(inode->i_mount, cg);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		map = ms->cg + ms->cg_freeoff;
		ndblk = cg_ndblk(&ms->super, cg);

		/*
 * Find the first complete free block and its bounded contiguous
		 * run. */
		/* Process each element required by the operation. */
		for (fragment = ms->super.dblkno; fragment < ndblk;
		     fragment += ms->super.frag) {
			count = 0;
			/* Process each remaining element. */
			while (count < run->reserved) {
				/* Handles the uint64 t condition. */
				if ((uint64_t)fragment +
					    (count + 1U) * ms->super.frag >
				    ndblk)
					break;
				/* Process each element required by the operation. */
				for (n = 0; n < ms->super.frag; n++) {
					/* Checks the bit test result. */
					if (!bit_test(
						    map,
						    fragment +
							    count * ms->super
									    .frag +
							    n))
						break;
				}

				/* Checks the current item count. */
				if (n != ms->super.frag)
					break;
				count++;
			}

			/* Checks the remaining item count. */
			if (count <= run->missing_nodes)
				continue;

			/*
 * Save the entire current group before changing its
			 * ownership. */
			free_blocks = drv_ufs_get32(ms->cg, UFS_CG_NBFREE,
						    ms->super.swapped);

			/* Handles the free blocks condition. */
			if (free_blocks < count ||
			    ms->super.cstotal_nbfree < count)

				/* Returns the computed result. */
				return EIO;
			memcpy(run->old_cg, ms->cg, ms->super.bsize);
			run->old_total = ms->super.cstotal_nbfree;
			run->count = count;
			run->first = cgstart(&ms->super, cg) + fragment;

			/*
 * Remove the run from the single group and its global
			 * summary. */
			/* Process each remaining element. */
			for (n = 0; n < count * ms->super.frag; n++)
				bit_clear(map, fragment + n);
			drv_ufs_put32(ms->cg, UFS_CG_NBFREE,
				      free_blocks - count, ms->super.swapped);
			ms->super.cstotal_nbfree -= count;

			/* Reports successful completion. */
			return 0;
		}
	}

	/* Report exhaustion without changing allocation ownership. */
	return ENOSPC;
}

/* Confirm pointer removal before returning any uncertain allocation to the CG. */
static int
allocation_run_abort(
	struct inode *inode,
	struct ufs_allocation_run *run)
{
	struct ufs_mount_state *ms;
	int error;

	/*
 * Restore original reachability when a metadata write may have landed.
	 */
	ms = state(inode->i_mount);
	error = 0;

	/* Handles the run condition. */
	if (run->published) {
		/* Handles the run condition. */
		if (run->leaf != 0) {
			error = write_block(inode->i_mount, run->leaf,
					    run->old_leaf);
		}

		/* Checks the operation status. */
		if (error == 0)
			error = persist_inode_locked(inode);

		/* Checks the operation status. */
		if (error == 0)
			error = disk_sync(inode->i_mount->m_disk);
	}

	/*
 * Keep allocations charged and stop writes if reachability is
	 * uncertain. */
	if (error != 0) {
		ms->writable = 0;

		/* Returns the computed result. */
		return error;
	}

	/*
 * Restore allocation summaries only after no durable pointer can refer
	 * here. */
	memcpy(ms->cg, run->old_cg, ms->super.bsize);
	ms->super.cstotal_nbfree = run->old_total;
	error = write_cg(inode->i_mount);

	/* Checks the operation status. */
	if (error == 0)
		error = disk_sync(inode->i_mount->m_disk);

	/* Checks the operation status. */
	if (error != 0)
		ms->writable = 0;

	/*
 * Let the caller retain charges when rollback durability remains
	 * uncertain. */
	return error;
}

/* End every quota reservation and release private working memory. */
static void
allocation_run_release(
	struct ufs_allocation_run *run,
	int retained)
{
	unsigned n;

	/*
 * Commit only actual retained blocks and roll back unused prefix
	 * charges. */
	/* Process each element required by the operation. */
	for (n = 0; n < run->reserved; n++) {
		/* Handles the retained condition. */
		if (retained && n < run->count)
			quota_commit(&run->charges[n]);
		else
			quota_rollback(&run->charges[n]);
	}
	kern_free(run->tree_memory);
	kern_free(run->memory);
	kern_free(run);
}

/* Initialize full new blocks before publishing a bounded private pointer image. */
static ssize_t
allocation_write_run(
	struct inode *inode,
	const void *buffer,
	size_t length,
	uint64_t logical,
	const struct io_context *context)
{
	struct ufs_mount_state *ms;
	struct ufs_allocation_run *run;
	struct ufs_inode_info *ui;
	size_t bytes;
	unsigned count;
	unsigned n;
	unsigned next;
	int error;
	int rollback;
	int retained;

	/*
 * Bound content by the current transfer and journal payload contracts.
	 */
	ms = state(inode->i_mount);
	bytes = length < UFS_ALLOCATION_BYTES ? length : UFS_ALLOCATION_BYTES;

	/* Handles the ms condition. */
	if (ms->journal_enabled &&
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)
		bytes = (ms->journal.sector_count - 2U) * UFS_SECTOR_SIZE;
	count = (unsigned)(bytes / ms->super.bsize);

	/* Checks the remaining item count. */
	if (count > UFS_ALLOCATION_BLOCKS)
		count = UFS_ALLOCATION_BLOCKS;

	/* Checks the remaining item count. */
	if (count == 0)
		return 0;

	/*
 * Declines an oversized group before changing either quota or
	 * allocation state. */
	if (ms->journal_enabled) {
		bytes = 3U * ms->super.bsize + UFS_SBLOCK_SIZE;

		/* Handles the bytes condition. */
		if (bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
		    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)

			/* Reports successful completion. */
			return 0;
	}

	/*
 * Decline optimization before reservation when bounded memory is
	 * unavailable. */
	run = kern_calloc(1, sizeof(*run));

	/* Handles the run availability. */
	if (run == NULL)
		return 0;
	run->grouped = ms->journal_enabled;
	run->memory = kern_malloc((run->grouped ? 4U : 3U) * ms->super.bsize +
				  (run->grouped ? UFS_SBLOCK_SIZE : 0));

	/* Handles the memory availability. */
	if (run->memory == NULL) {
		kern_free(run);

		/* Reports successful completion. */
		return 0;
	}
	run->old_cg = run->memory;
	run->old_leaf = run->memory + ms->super.bsize;
	run->new_leaf = run->old_leaf + ms->super.bsize;

	/* Handles the run condition. */
	if (run->grouped) {
		run->dinode = run->new_leaf + ms->super.bsize;
		run->summaries = run->dinode + ms->super.bsize;
	}

	/*
 * Owns shared path images from their first read through publication or
	 * rollback. */
	mutex_lock(&ms->lock);

	/* Handles the ms condition. */
	if (!ms->writable) {
		mutex_unlock(&ms->lock);
		allocation_run_release(run, 0);

		/* Returns the computed result. */
		return -EROFS;
	}
	error = allocation_run_leaf(inode, logical, &count, run);

	/* Checks the operation status. */
	if (error != 0 || count == 0) {
		mutex_unlock(&ms->lock);
		allocation_run_release(run, 0);

		/* Returns the computed result. */
		return -error;
	}

	/*
 * Preflights every changed path block before reserving quota or
	 * allocation. */
	if (run->tree_count != 0) {
		bytes = (2U + run->tree_count) * ms->super.bsize +
			UFS_SBLOCK_SIZE;

		/* Handles the bytes condition. */
		if (bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
		    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
			mutex_unlock(&ms->lock);
			allocation_run_release(run, 0);

			/* Reports successful completion. */
			return 0;
		}

		/* Checks the remaining item count. */
		if (count > UFS_ALLOCATION_BLOCKS - run->missing_nodes)
			count = UFS_ALLOCATION_BLOCKS - run->missing_nodes;
		count += run->missing_nodes;
	}

	/*
 * Reserve quotas individually so a hard limit still permits a valid
	 * prefix. */
	/* Process each remaining element. */
	for (n = 0; n < count; n++) {
		error = quota_reserve(&ms->quota, inode->i_uid, inode->i_gid, 1,
				      0, quota_now(), &run->charges[n]);

		/* Checks the operation status. */
		if (error != 0)
			break;
		run->reserved++;
	}

	/* Handles the run condition. */
	if (run->reserved <= run->missing_nodes) {
		mutex_unlock(&ms->lock);
		allocation_run_release(run, 0);

		/* Returns the computed result. */
		return -error;
	}

	/*
 * Reserves bitmap ownership under the same lock as the prepared
	 * reference path. */
	error = allocation_run_reserve(inode, run);

	/* Checks the operation status. */
	if (error != 0) {
		mutex_unlock(&ms->lock);

		/* Checks the operation status. */
		if (error == ENOSPC && run->missing_nodes != 0)
			error = 0;
		allocation_run_release(run, 0);

		/* Returns the computed result. */
		return -error;
	}
	io_stats_record(IO_UFS_ALLOC_BEGIN, 0);
	bytes = (run->count - run->missing_nodes) * ms->super.bsize;
	/* Process each remaining element. */
	for (n = 0; n < run->count; n++)
		io_stats_record(IO_UFS_ALLOCATE, ms->super.bsize);

	/*
 * Keeps grouped allocation private until initialized data is durable.
	 */
	if (run->grouped) {
		ms->cg_valid = 0;
		ms->cg_dirty = 1;
		buf_view_release(&ms->cg_view);
		error = 0;
	} else {
		error = write_cg(inode->i_mount);
	}

	/* Checks the operation status. */
	if (error == 0) {
		io_stats_record(IO_UFS_CONTENT_WRITE, bytes);
		error = write_sectors_context(
			inode->i_mount, run->first << ms->super.fsbtodb,
			(uint32_t)(bytes / UFS_SECTOR_SIZE), buffer, context);
	}

	/* Checks the operation status. */
	if (error == 0)
		error = disk_sync(inode->i_mount->m_disk);

	/*
 * Prepare the new inode and leaf without changing the published inode.
	 */
	ui = info(inode);
	memcpy(&run->image, ui, sizeof(run->image));

	/* Handles the run condition. */
	if (run->tree_count != 0) {
		next = run->count - run->missing_nodes;

		/*
 * Assigns reserved metadata addresses only to the private path
		 * images. */
		/* Process each remaining element. */
		for (n = 0; n < run->tree_count; n++) {
			/* Handles the run condition. */
			if (run->tree_targets[n] == 0) {
				run->tree_targets[n] =
					run->first + next++ * ms->super.frag;
			}
		}
		/* Process each remaining element. */
		for (n = 0; n + 1U < run->tree_count; n++) {
			drv_ufs_put64(
				run->tree_images[n], run->tree_indices[n] * 8U,
				run->tree_targets[n + 1U], ms->super.swapped);
		}

		/* Handles the run condition. */
		if (run->missing_root) {
			run->image.indirect[run->root_level] =
				run->tree_targets[0];
		}
		run->leaf = run->tree_targets[run->tree_count - 1U];
	}
	/* Process each remaining element. */
	for (n = 0; n < run->count - run->missing_nodes; n++) {
		/* Handles the run condition. */
		if (run->leaf != 0) {
			drv_ufs_put64(run->new_leaf, (run->index + n) * 8U,
				      run->first + n * ms->super.frag,
				      ms->super.swapped);
		} else {
			run->image.direct[run->index + n] =
				run->first + n * ms->super.frag;
		}
	}
	run->image.blocks +=
		(uint64_t)run->count * ms->super.bsize / UFS_SECTOR_SIZE;

	/* Handles the uint64 t condition. */
	if ((uint64_t)run->image.inode.i_size <
	    logical * ms->super.bsize + bytes) {
		run->image.inode.i_size =
			(off_t)(logical * ms->super.bsize + bytes);
	}

	/*
 * Publish initialized pointers, retaining the old image until the flush
	 * passes. */
	if (error == 0 && run->grouped) {
		error = allocation_group_commit(inode, run, context);
	} else if (error == 0) {
		run->published = 1;

		/* Handles the run condition. */
		if (run->leaf != 0) {
			error = write_block(inode->i_mount, run->leaf,
					    run->new_leaf);
		}

		/* Checks the operation status. */
		if (error == 0)
			error = persist_inode_locked(&run->image.inode);

		/* Checks the operation status. */
		if (error == 0)
			error = disk_sync(inode->i_mount->m_disk);
	}

	/*
 * Publish in-memory state only after a durable private metadata image.
	 */
	retained = 1;

	/* Checks the operation status. */
	if (error == 0 || run->committed) {
		memcpy(ui->direct, run->image.direct, sizeof(ui->direct));
		memcpy(ui->indirect, run->image.indirect, sizeof(ui->indirect));
		ui->blocks = run->image.blocks;
		inode->i_size = run->image.inode.i_size;
		ms->rotor_cg = ms->active_cg;

		/* Handles the run condition. */
		if (run->grouped)
			ms->cg_dirty = run->uncertain;
		io_stats_record(IO_UFS_ALLOC_COMMIT, 0);
	} else if (run->grouped) {
		/*
 * Uncommitted private ownership needs no compensating disk
		 * transaction. */
		if (!run->uncertain) {
			memcpy(ms->cg, run->old_cg, ms->super.bsize);
			ms->super.cstotal_nbfree = run->old_total;
			ms->cg_dirty = 0;
			retained = 0;
		}
		io_stats_record(IO_UFS_ALLOC_ABORT, 0);
	} else {
		rollback = allocation_run_abort(inode, run);
		retained = rollback != 0;
		io_stats_record(IO_UFS_ALLOC_ABORT, 0);
	}
	mutex_unlock(&ms->lock);
	allocation_run_release(run, retained);

	/* Return only completely initialized and published content. */
	if (error != 0)
		return -error;

	/* Returns the computed result. */
	return (ssize_t)bytes;
}
/* End consolidated ufs-allocation.inc. */

/* Begin consolidated ufs-xattr-release.inc. */
/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

struct ufs_inode_metadata_images {
	struct ufs_inode_info image;
	uint8_t *memory;
	uint8_t *cg[UFS_NXADDR];
	uint8_t *dinode;
	uint8_t *summaries;
	uint8_t *data;
	uint32_t groups[UFS_NXADDR];
	unsigned group_count;
};

static int xattr_release_location(const struct ufs_super *super, uint64_t child, uint32_t *group, uint32_t *local);
static int xattr_existing_locked(struct inode *inode, struct ufs_inode_metadata_images *group, unsigned count, const uint8_t *area, size_t length);
static int xattr_existing_group(struct inode *inode, const uint8_t *area, size_t length, int *handled);
static int xattr_release_group(struct inode *inode, int *handled);

/* Validates an attribute block and returns its allocation-group coordinates. */
static int
xattr_release_location(
	const struct ufs_super *super,
	uint64_t child,
	uint32_t *group,
	uint32_t *local)
{
	uint64_t start;
	uint32_t cg;

	/*
 * Finds one complete aligned data block within the filesystem geometry.
	 */
	/* Process each element required by the operation. */
	for (cg = 0; cg < super->ncg; cg++) {
		start = cgstart(super, cg);

		/* Checks the cg ndblk result. */
		if (child < start || child - start < super->dblkno ||
		    child - start >= cg_ndblk(super, cg))
			continue;
		*local = (uint32_t)(child - start);
		/* Checks the cg ndblk result. */
		if (*local % super->frag != 0 ||
		    super->frag > cg_ndblk(super, cg) - *local)

			/* Returns the computed result. */
			return EIO;
		*group = cg;
		/* Reports successful completion. */
		return 0;
	}

	/* Refuses a pointer outside every allocation group. */
	return EIO;
}

/* Publishes an existing attribute area and releases any dropped backing. */
static int
xattr_existing_locked(
	struct inode *inode,
	struct ufs_inode_metadata_images *group,
	unsigned count,
	const uint8_t *area,
	size_t length)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	struct ufs_transaction_outcome outcome;
	struct ufs_journal_extent extents[UFS_NXADDR + 3U];
	uint64_t child;
	uint64_t fragment;
	uint32_t cg;
	uint32_t local;
	uint32_t free_blocks;
	unsigned keep;
	unsigned released;
	unsigned extent_count;
	unsigned index;
	unsigned slot;
	unsigned n;
	int error;
	int quota_error;

	/*
 * Validates the complete inode owner before changing private allocation
	 * maps. */
	ms = state(inode->i_mount);
	ui = info(inode);
	keep = length != 0;
	released = count - keep;

	/* Handles the ms condition. */
	if (!ms->writable)
		return EROFS;

	/* Handles the ui condition. */
	if (ui->blocks <
		    (uint64_t)count * (ms->super.bsize / UFS_SECTOR_SIZE) ||
	    ms->super.cstotal_nbfree > UINT64_MAX - released)

		/* Returns the computed result. */
		return EIO;

	/*
 * Verifies the retained payload block remains allocated before
	 * replacing it. */
	if (keep != 0) {
		error = xattr_release_location(&ms->super, ui->extattr[0], &cg,
					       &local);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		error = load_cg_locked(inode->i_mount, cg);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		/* Process each element required by the operation. */
		for (n = 0; n < ms->super.frag; n++) {
			/* Checks the bit test result. */
			if (bit_test(ms->cg + ms->cg_freeoff, local + n))
				return EIO;
		}
		memset(group->data, 0, ms->super.bsize);
		memcpy(group->data, area, length);
	}

	/*
 * Loads each affected CG once and removes only allocated, distinct
	 * blocks. */
	/* Process each remaining element. */
	for (index = keep; index < count; index++) {
		child = ui->extattr[index];
		/* Process each remaining element. */
		for (n = 0; n < index; n++) {
			/* Handles the ui condition. */
			if (ui->extattr[n] == child)
				return EIO;
		}
		error = xattr_release_location(&ms->super, child, &cg, &local);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		/* Process each remaining element. */
		for (slot = 0; slot < group->group_count; slot++) {
			/* Handles the group condition. */
			if (group->groups[slot] == cg)
				break;
		}

		/* Handles the slot condition. */
		if (slot == group->group_count) {
			error = load_cg_locked(inode->i_mount, cg);

			/* Checks the operation status. */
			if (error != 0)
				return error;
			memcpy(group->cg[slot], ms->cg, ms->super.bsize);
			group->groups[slot] = cg;
			group->group_count++;
		}

		/*
 * Reads offsets from this CG image because different CG layouts
		 * may differ. */
		n = drv_ufs_get32(group->cg[slot], UFS_CG_FREEOFF,
				  ms->super.swapped);
		/* Process each element required by the operation. */
		for (cg = 0; cg < ms->super.frag; cg++) {
			/* Checks the bit test result. */
			if (bit_test(group->cg[slot] + n, local + cg))
				return EIO;
			bit_set(group->cg[slot] + n, local + cg);
		}
		free_blocks = drv_ufs_get32(group->cg[slot], UFS_CG_NBFREE,
					    ms->super.swapped);

		/* Handles the free blocks condition. */
		if (free_blocks == UINT32_MAX)
			return EIO;
		drv_ufs_put32(group->cg[slot], UFS_CG_NBFREE, free_blocks + 1U,
			      ms->super.swapped);
	}

	/*
 * Prepares the complete new serialized area and its remaining block
	 * ownership. */
	memcpy(&group->image, ui, sizeof(group->image));
	memset(group->image.extattr, 0, sizeof(group->image.extattr));

	/* Handles the keep condition. */
	if (keep != 0)
		group->image.extattr[0] = ui->extattr[0];
	group->image.extattr_size = (uint32_t)length;
	group->image.blocks -=
		(uint64_t)released * (ms->super.bsize / UFS_SECTOR_SIZE);
	error = prepare_inode_locked(&group->image.inode, group->dinode,
				     &fragment);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the released condition. */
	if (released != 0) {
		error = prepare_super_summaries(inode->i_mount,
						group->summaries);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		drv_ufs_put64(group->summaries, UFS_FS_CSTOTAL_NBFREE,
			      ms->super.cstotal_nbfree + released,
			      ms->super.swapped);
	}

	/*
 * Publishes only changed maps plus the payload, dinode and changed
	 * totals. */
	extent_count = 0;
	/* Process each remaining element. */
	for (n = 0; n < group->group_count; n++) {
		extents[extent_count].target =
			(cgstart(&ms->super, group->groups[n]) +
			 ms->super.cblkno)
			<< ms->super.fsbtodb;
		extents[extent_count].sectors =
			ms->super.bsize / UFS_SECTOR_SIZE;
		extents[extent_count].payload = group->cg[n];
		extent_count++;
	}

	/* Handles the released condition. */
	if (released != 0) {
		extents[extent_count].target =
			UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;
		extents[extent_count].sectors =
			UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;
		extents[extent_count].payload = group->summaries;
		extent_count++;
	}

	/* Handles the keep condition. */
	if (keep != 0) {
		extents[extent_count].target = ui->extattr[0]
					       << ms->super.fsbtodb;
		extents[extent_count].sectors =
			ms->super.bsize / UFS_SECTOR_SIZE;
		extents[extent_count].payload = group->data;
		extent_count++;
	}
	extents[extent_count].target = fragment << ms->super.fsbtodb;
	extents[extent_count].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[extent_count].payload = group->dinode;
	extent_count++;
	ms->cg_valid = 0;
	buf_view_release(&ms->cg_view);
	error = metadata_group_commit(inode->i_mount, extents, extent_count,
				      NULL, &outcome);

	/*
 * Publishes live attribute ownership and releases quota only after
	 * proven commit. */
	if (outcome.committed) {
		memcpy(ui->extattr, group->image.extattr, sizeof(ui->extattr));
		ui->extattr_size = group->image.extattr_size;
		ui->blocks = group->image.blocks;
		ms->super.cstotal_nbfree += released;
		quota_error = 0;

		/* Handles the released condition. */
		if (released != 0) {
			quota_error = quota_release(&ms->quota, inode->i_uid,
						    inode->i_gid, released, 0);
		}

		/* Checks the operation status. */
		if (quota_error != 0) {
			ms->writable = 0;

			/* Checks the operation status. */
			if (error == 0)
				error = quota_error;
		}
	}

	/* Handles the outcome condition. */
	if (outcome.committed || outcome.uncertain)
		ms->cg_dirty = outcome.uncertain;

	/*
 * Returns the original errno without compensating writes or repeated
	 * frees. */
	return error;
}

/* Reserves bounded storage while the caller retains the inode lock. */
static int
xattr_existing_group(
	struct inode *inode,
	const uint8_t *area,
	size_t length,
	int *handled)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	struct ufs_inode_metadata_images *group;
	size_t bytes;
	uint32_t groups[UFS_NXADDR];
	uint32_t local;
	unsigned keep;
	unsigned unique;
	unsigned j;
	unsigned count;
	unsigned n;
	int error;

	/*
 * Bounds the complete serialized area before accepting this operation.
	 */
	ms = state(inode->i_mount);
	ui = info(inode);
	*handled = 0;
	/* Handles the ms condition. */
	if (!ms->journal_enabled)
		return 0;
	*handled = 1;
	keep = length != 0;

	/* Handles the area availability. */
	if (length > ms->super.bsize || (keep != 0 && area == NULL))
		return EINVAL;

	/* Handles the uint64 t condition. */
	if ((uint64_t)ui->extattr_size > (uint64_t)UFS_NXADDR * ms->super.bsize)
		return EIO;
	count = (ui->extattr_size + ms->super.bsize - 1U) / ms->super.bsize;
	/* Process each element required by the operation. */
	for (n = 0; n < UFS_NXADDR; n++) {
		/* Checks the current item count. */
		if ((n < count) != (ui->extattr[n] != 0))
			return EIO;
	}

	/* Checks the remaining item count. */
	if (count == 0) {
		/* Handles the keep condition. */
		if (keep != 0)
			*handled = 0;
		/* Reports successful completion. */
		return 0;
	}
	unique = 0;

	/*
 * Reserves the actual deduplicated footprint before admitting the
	 * group. */
	/* Process each remaining element. */
	for (n = keep; n < count; n++) {
		error = xattr_release_location(&ms->super, ui->extattr[n],
					       &groups[n], &local);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		/* Process each element required by the operation. */
		for (j = keep; j < n; j++) {
			/* Handles the groups condition. */
			if (groups[j] == groups[n])
				break;
		}

		/* Handles the j condition. */
		if (j == n)
			unique++;
	}
	bytes = (unique + 1U + keep) * ms->super.bsize;

	/* Checks the remaining item count. */
	if (count > keep)
		bytes += UFS_SBLOCK_SIZE;

	/* Handles the ms condition. */
	if (ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
		*handled = 0;
		/* Reports successful completion. */
		return 0;
	}

	/*
 * Allocates the changed maps, dinode, optional payload and optional
	 * totals. */
	group = kern_calloc(1, sizeof(*group));

	/* Handles the group availability. */
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(bytes);

	/* Handles the memory availability. */
	if (group->memory == NULL) {
		kern_free(group);

		/* Returns the computed result. */
		return ENOMEM;
	}
	/* Process each element required by the operation. */
	for (n = 0; n < unique; n++)
		group->cg[n] = group->memory + n * ms->super.bsize;
	group->dinode = group->memory + unique * ms->super.bsize;
	group->data = group->dinode + ms->super.bsize;
	group->summaries = group->data + keep * ms->super.bsize;
	mutex_lock(&ms->lock);
	error = xattr_existing_locked(inode, group, count, area, length);
	mutex_unlock(&ms->lock);
	kern_free(group->memory);
	kern_free(group);

	/* Reports only the admitted operation's outcome. */
	return error;
}

/* Clears all existing attribute backing through the shared update owner. */
static int
xattr_release_group(
	struct inode *inode,
	int *handled)
{
	int error;

	/* Keeps the clear caller's explicit handled/result convention. */
	error = xattr_existing_group(inode, NULL, 0, handled);

	/* Returns the computed result. */
	return error;
}
/* End consolidated ufs-xattr-release.inc. */

/* Begin consolidated ufs-xattr-allocation.inc. */
/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

enum ufs_initial_block_kind { UFS_INITIAL_XATTR, UFS_INITIAL_DIRECTORY };

struct ufs_initial_allocation {
	struct ufs_inode_metadata_images images;
	struct ufs_transaction_outcome outcome;
	struct quota_charge charge;
	uint64_t fragment;
	uint32_t cg;
};

static int initial_block_candidate(struct inode *inode, struct ufs_initial_allocation *group);
static int initial_block_locked(struct inode *inode, enum ufs_initial_block_kind kind, const uint8_t *area, size_t length, struct ufs_initial_allocation *group);
static int initial_block_group(struct inode *inode, enum ufs_initial_block_kind kind, const uint8_t *area, size_t length, int *handled);
static int directory_backing_group(struct inode *inode, int *handled);
static int xattr_allocate_group(struct inode *inode, const uint8_t *area, size_t length, int *handled);

/* Selects one free block and edits only its private allocation map. */
static int
initial_block_candidate(
	struct inode *inode,
	struct ufs_initial_allocation *group)
{
	struct ufs_mount_state *ms;
	uint32_t attempt;
	uint32_t cg;
	uint32_t local;
	uint32_t ndblk;
	uint32_t free_blocks;
	uint8_t *map;
	unsigned n;
	int error;

	/*
 * Scans valid allocation groups while the caller excludes other
	 * allocators. */
	ms = state(inode->i_mount);
	/* Process each element required by the operation. */
	for (attempt = 0; attempt < ms->super.ncg; attempt++) {
		cg = (ms->rotor_cg + attempt) % ms->super.ncg;
		error = load_cg_locked(inode->i_mount, cg);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		ndblk = cg_ndblk(&ms->super, cg);
		map = ms->cg + ms->cg_freeoff;
		local = (ms->super.dblkno + ms->super.frag - 1U) &
			~(ms->super.frag - 1U);

		/*
 * Requires a complete free filesystem block, without consuming
		 * fragments. */
		/* Continue while the operation condition remains true. */
		while (local < ndblk && ms->super.frag <= ndblk - local) {
			/* Process each element required by the operation. */
			for (n = 0; n < ms->super.frag; n++) {
				/* Checks the bit test result. */
				if (!bit_test(map, local + n))
					break;
			}

			/* Checks the current item count. */
			if (n == ms->super.frag) {
				free_blocks =
					drv_ufs_get32(ms->cg, UFS_CG_NBFREE,
						      ms->super.swapped);

				/* Handles the free blocks condition. */
				if (free_blocks == 0 ||
				    ms->super.cstotal_nbfree == 0)

					/* Returns the computed result. */
					return EIO;
				memcpy(group->images.cg[0], ms->cg,
				       ms->super.bsize);

				/*
 * Reserves only private bitmap bytes until the
				 * entire group commits. */
				/* Process each element required by the operation. */
				for (n = 0; n < ms->super.frag; n++) {
					bit_clear(group->images.cg[0] +
							  ms->cg_freeoff,
						  local + n);
				}
				drv_ufs_put32(group->images.cg[0],
					      UFS_CG_NBFREE, free_blocks - 1U,
					      ms->super.swapped);
				group->fragment =
					cgstart(&ms->super, cg) + local;
				group->cg = cg;

				/* Reports successful completion. */
				return 0;
			}
			local += ms->super.frag;
		}
	}

	/*
 * Reports exhausted full-block capacity without changing live
	 * accounting. */
	return ENOSPC;
}

/* Publishes allocation, initialized metadata and its inode reference together. */
static int
initial_block_locked(
	struct inode *inode,
	enum ufs_initial_block_kind kind,
	const uint8_t *area,
	size_t length,
	struct ufs_initial_allocation *group)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	struct ufs_inode_metadata_images *images;
	struct ufs_journal_extent extents[4];
	uint64_t dinode_fragment;
	unsigned n;
	int error;

	/*
 * Checks the requested owner before selecting backing for its first
	 * block. */
	ms = state(inode->i_mount);
	ui = info(inode);
	images = &group->images;

	/* Handles the ms condition. */
	if (!ms->writable)
		return EROFS;

	/* Handles the ui condition. */
	if (ui->blocks > UINT64_MAX - ms->super.bsize / UFS_SECTOR_SIZE)
		return EIO;

	/* Handles the kind condition. */
	if (kind == UFS_INITIAL_XATTR) {
		/* Handles the ui condition. */
		if (ui->extattr_size != 0 || ui->extattr[0] != 0 ||
		    ui->extattr[1] != 0)

			/* Returns the computed result. */
			return EIO;
	} else {
		/* Handles the inode condition. */
		if (inode->i_type != INODE_DIR || inode->i_size != 0)
			return EIO;
		/* Process each element required by the operation. */
		for (n = 0; n < UFS_NDADDR; n++) {
			/* Handles the ui condition. */
			if (ui->direct[n] != 0)
				return EIO;
		}
		/* Process each element required by the operation. */
		for (n = 0; n < UFS_NIADDR; n++) {
			/* Handles the ui condition. */
			if (ui->indirect[n] != 0)
				return EIO;
		}
	}
	error = initial_block_candidate(inode, group);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/*
 * Prepares a fully initialized payload and a private reference to its
	 * reservation. */
	memset(images->data, 0, ms->super.bsize);

	/* Checks the current data length. */
	if (length != 0)
		memcpy(images->data, area, length);
	memcpy(&images->image, ui, sizeof(images->image));

	/* Handles the kind condition. */
	if (kind == UFS_INITIAL_XATTR) {
		images->image.extattr[0] = group->fragment;
		images->image.extattr_size = (uint32_t)length;
	} else {
		images->image.direct[0] = group->fragment;
	}
	images->image.blocks += ms->super.bsize / UFS_SECTOR_SIZE;
	error = prepare_inode_locked(&images->image.inode, images->dinode,
				     &dinode_fragment);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = prepare_super_summaries(inode->i_mount, images->summaries);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	drv_ufs_put64(images->summaries, UFS_FS_CSTOTAL_NBFREE,
		      ms->super.cstotal_nbfree - 1U, ms->super.swapped);

	/* Orders every home mutation behind one validated commit record. */
	extents[0].target = (cgstart(&ms->super, group->cg) + ms->super.cblkno)
			    << ms->super.fsbtodb;
	extents[0].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[0].payload = images->cg[0];
	extents[1].target = UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;
	extents[1].sectors = UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;
	extents[1].payload = images->summaries;
	extents[2].target = group->fragment << ms->super.fsbtodb;
	extents[2].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[2].payload = images->data;
	extents[3].target = dinode_fragment << ms->super.fsbtodb;
	extents[3].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[3].payload = images->dinode;
	ms->cg_valid = 0;
	buf_view_release(&ms->cg_view);
	error = metadata_group_commit(inode->i_mount, extents, 4, NULL,
				      &group->outcome);

	/*
 * Makes the initialized area visible in RAM only after positive commit.
	 */
	if (group->outcome.committed) {
		memcpy(ms->cg, images->cg[0], ms->super.bsize);
		ms->super.cstotal_nbfree--;
		ms->rotor_cg = group->cg;

		/* Handles the kind condition. */
		if (kind == UFS_INITIAL_XATTR) {
			ui->extattr[0] = group->fragment;
			ui->extattr_size = (uint32_t)length;
		} else {
			ui->direct[0] = group->fragment;
		}
		ui->blocks = images->image.blocks;
	}

	/* Handles the group condition. */
	if (group->outcome.committed || group->outcome.uncertain)
		ms->cg_dirty = group->outcome.uncertain;

	/*
 * Preserves the original errno and its separately recorded ownership
	 * outcome. */
	return error;
}

/* Reserves memory and quota before admitting the first metadata block. */
static int
initial_block_group(
	struct inode *inode,
	enum ufs_initial_block_kind kind,
	const uint8_t *area,
	size_t length,
	int *handled)
{
	struct ufs_mount_state *ms;
	struct ufs_initial_allocation *group;
	size_t bytes;
	int error;

	/*
 * Declines only unsupported profiles before any quota or disk mutation.
	 */
	ms = state(inode->i_mount);
	*handled = 0;
	/* Handles the ms condition. */
	if (!ms->journal_enabled)
		return 0;
	*handled = 1;
	/* Handles the kind condition. */
	if (kind == UFS_INITIAL_XATTR) {
		/* Handles the area availability. */
		if (area == NULL || length == 0 || length > ms->super.bsize)
			return EINVAL;
	} else if (kind != UFS_INITIAL_DIRECTORY || area != NULL ||
		   length != 0) {
		/* Returns the computed result. */
		return EINVAL;
	}
	bytes = 3U * ms->super.bsize + UFS_SBLOCK_SIZE;

	/* Handles the ms condition. */
	if (ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
		*handled = 0;
		/* Reports successful completion. */
		return 0;
	}

	/* Allocates all private images before reserving quota. */
	group = kern_calloc(1, sizeof(*group));

	/* Handles the group availability. */
	if (group == NULL)
		return ENOMEM;
	group->images.memory = kern_malloc(bytes);

	/* Handles the memory availability. */
	if (group->images.memory == NULL) {
		kern_free(group);

		/* Returns the computed result. */
		return ENOMEM;
	}
	group->images.cg[0] = group->images.memory;
	group->images.dinode = group->images.memory + ms->super.bsize;
	group->images.data = group->images.dinode + ms->super.bsize;
	group->images.summaries = group->images.data + ms->super.bsize;
	error = quota_reserve(&ms->quota, inode->i_uid, inode->i_gid, 1, 0,
			      quota_now(), &group->charge);

	/* Checks the operation status. */
	if (error == 0) {
		mutex_lock(&ms->lock);
		error = initial_block_locked(inode, kind, area, length, group);

		/*
 * Retains quota for a possibly committed allocation until
		 * recovery settles it. */
		if (group->outcome.committed || group->outcome.uncertain)
			quota_commit(&group->charge);
		else
			quota_rollback(&group->charge);
		mutex_unlock(&ms->lock);
	}
	kern_free(group->images.memory);
	kern_free(group);

	/*
 * Returns an admitted failure without retrying the compatibility
	 * allocator. */
	return error;
}

/* Allocates initialized attribute backing through the common first-block owner. */
static int
xattr_allocate_group(
	struct inode *inode,
	const uint8_t *area,
	size_t length,
	int *handled)
{
	int error;

	/* Preserves the xattr caller's admission and errno contract. */
	error = initial_block_group(inode, UFS_INITIAL_XATTR, area, length,
				    handled);

	/* Returns the computed result. */
	return error;
}

/* Allocates empty directory backing without publishing any directory entry. */
static int
directory_backing_group(
	struct inode *inode,
	int *handled)
{
	int error;

	/*
 * Leaves size and link counts untouched until a later namespace
	 * operation. */
	error = initial_block_group(inode, UFS_INITIAL_DIRECTORY, NULL, 0,
				    handled);

	/* Returns the computed result. */
	return error;
}
/* End consolidated ufs-xattr-allocation.inc. */

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

	/* Handles the ms condition. */
	if (!ms->writable)
		return -EROFS;

	/* Checks the current offset. */
	if (offset < 0 || (uint64_t)offset + length < (uint64_t)offset)
		return -EINVAL;

	/* Handles the uint64 t condition. */
	if ((uint64_t)offset + length > ms->super.maxfilesize ||
	    (uint64_t)offset + length >
		    (sizeof(off_t) == 8 ? INT64_MAX : INT32_MAX))

		/* Returns the computed result. */
		return -EFBIG;
	scratch = NULL;
	done = 0;
	final_error = 0;
	metadata_dirty = 0;
	mutex_lock(&inode->i_lock);

	/*
 * Limits direct runs to already published file bytes and allocated
	 * blocks. */
	/* Process each remaining element. */
	while (done < length) {
		position = (uint64_t)offset + done;
		logical = position / ms->super.bsize;
		within = (size_t)(position % ms->super.bsize);
		error = bmap(inode, logical, &fragment);

		/* Checks the operation status. */
		if (error != 0) {
			final_error = error;
			break;
		}

		/*
 * Initialize new full blocks with a private
		 * allocation/publication batch. */
		if (fragment == 0 && within == 0) {
			allocated = allocation_write_run(
				inode, (const uint8_t *)buffer + done,
				length - done, logical, context);

			/* Handles the allocated condition. */
			if (allocated < 0) {
				final_error = (int)-allocated;
				break;
			}

			/* Handles the allocated condition. */
			if (allocated > 0) {
				done += (size_t)allocated;
				metadata_dirty = 0;
				continue;
			}
		}
		eligible = 0;

		/* Handles the within condition. */
		if (within == 0 && position < (uint64_t)inode->i_size) {
			eligible = length - done;

			/* Handles the eligible condition. */
			if (eligible > (uint64_t)inode->i_size - position) {
				eligible = (size_t)((uint64_t)inode->i_size -
						    position);
			}
		}
		amount = content_run_bytes(inode, logical, fragment, eligible,
					   1, &mapping_error);

		/* Handles the amount condition. */
		if (amount != 0) {
			io_stats_record(IO_UFS_CONTENT_WRITE, amount);
			error = write_sectors_context(
				inode->i_mount,
				(uint64_t)fragment << ms->super.fsbtodb,
				(uint32_t)(amount / UFS_SECTOR_SIZE),
				(const uint8_t *)buffer + done, context);
		} else {
			/*
 * Keeps the established allocation/zero and
			 * partial-block path. */
			if (scratch == NULL) {
				scratch = kern_malloc(ms->super.bsize);

				/* Handles the scratch availability. */
				if (scratch == NULL) {
					final_error = ENOMEM;
					break;
				}
			}
			amount = ms->super.bsize - within;

			/* Handles the amount condition. */
			if (amount > length - done)
				amount = length - done;

			/* Handles the fragment condition. */
			if (fragment == 0) {
				error = bmap_ensure(inode, logical, &fragment);

				/* Checks the operation status. */
				if (error != 0) {
					final_error = error;
					break;
				}
				memset(scratch, 0, ms->super.bsize);
			} else if (within != 0 || amount != ms->super.bsize) {
				error = read_content_block(inode->i_mount,
							   fragment, scratch);

				/* Checks the operation status. */
				if (error != 0) {
					final_error = error;
					break;
				}
			}
			memcpy(scratch + within, (const uint8_t *)buffer + done,
			       amount);
			error = write_content_context(inode->i_mount, fragment,
						      scratch, context);
		}

		/* Checks the operation status. */
		if (error != 0) {
			final_error = error;
			break;
		}
		done += amount;
		metadata_dirty = 1;

		/* Handles the uint64 t condition. */
		if ((uint64_t)inode->i_size < (uint64_t)offset + done)
			inode->i_size = (off_t)((uint64_t)offset + done);

		/* Checks the operation status. */
		if (mapping_error != 0) {
			final_error = mapping_error;
			break;
		}
	}

	/*
 * Preserves the existing metadata publication and partial-result
	 * convention. */
	if (done != 0 && metadata_dirty) {
		error = persist_inode(inode);

		/* Checks the operation status. */
		if (error != 0 && final_error == 0)
			final_error = error;
	}
	mutex_unlock(&inode->i_lock);
	kern_free(scratch);

	/*
 * Reports only the completed prefix, or the first error without
	 * progress. */
	return done != 0 ? (ssize_t)done : -final_error;
}

static uint64_t indirect_span(const struct ufs_super *super, unsigned depth);

/* Supports the indirect span operation. */
static uint64_t
indirect_span(
	const struct ufs_super *super,
	unsigned depth)
{
	uint64_t span = 1;

	/* Continue while the operation condition remains true. */
	while (depth-- != 0)
		span *= super->nindir;

	/* Returns the computed result. */
	return span;
}

/* Begin consolidated ufs-release.inc. */
/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

struct ufs_release_group {
	struct ufs_inode_info image;
	uint8_t *memory;
	uint8_t *cg;
	uint8_t *dinode;
	uint8_t *parent;
	uint8_t *summaries;
};

static int release_group_locked(struct inode *inode, uint64_t parent, unsigned index, uint64_t child, struct ufs_release_group *group);
static int release_group(struct inode *inode, uint64_t parent, unsigned index, uint64_t child, int *handled);

/* Prepares a private pointer removal and free map under mount mutation ownership. */
static int
release_group_locked(
	struct inode *inode,
	uint64_t parent,
	unsigned index,
	uint64_t child,
	struct ufs_release_group *group)
{
	struct ufs_mount_state *ms = state(inode->i_mount);
	struct ufs_inode_info *ui = info(inode);
	struct ufs_journal_extent extents[4];
	struct ufs_transaction_outcome outcome;
	uint64_t start;
	uint64_t fragment;
	uint64_t current;
	uint32_t cg;
	uint32_t local = 0;
	uint32_t free_blocks;
	unsigned n;
	unsigned count;
	int error;
	int quota_error;

	/*
 * Refuses malformed accounting before preparing any reusable
	 * allocation. */
	if (!ms->writable)
		return EROFS;

	/* Handles the ui condition. */
	if (ui->blocks < ms->super.bsize / UFS_SECTOR_SIZE)
		return EIO;
	/* Process each element required by the operation. */
	for (cg = 0; cg < ms->super.ncg; cg++) {
		start = cgstart(&ms->super, cg);

		/* Checks the cg ndblk result. */
		if (child < start || child - start < ms->super.dblkno ||
		    child - start >= cg_ndblk(&ms->super, cg))
			continue;
		local = (uint32_t)(child - start);

		/* Checks the cg ndblk result. */
		if (local % ms->super.frag != 0 ||
		    ms->super.frag > cg_ndblk(&ms->super, cg) - local)

			/* Returns the computed result. */
			return EIO;
		break;
	}

	/* Handles the cg condition. */
	if (cg == ms->super.ncg)
		return EIO;
	error = load_cg_locked(inode->i_mount, cg);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	memcpy(group->cg, ms->cg, ms->super.bsize);
	/* Process each element required by the operation. */
	for (n = 0; n < ms->super.frag; n++) {
		/* Checks the bit test result. */
		if (bit_test(group->cg + ms->cg_freeoff, local + n))
			return EIO;
	}
	free_blocks =
		drv_ufs_get32(group->cg, UFS_CG_NBFREE, ms->super.swapped);

	/* Handles the free blocks condition. */
	if (free_blocks == UINT32_MAX || ms->super.cstotal_nbfree == UINT64_MAX)
		return EIO;

	/*
 * Validates the current reference and changes only a private inode or
	 * parent. */
	memcpy(&group->image, ui, sizeof(group->image));

	/* Handles the parent condition. */
	if (parent != 0) {
		/* Checks the current index. */
		if (index >= ms->super.nindir)
			return EIO;
		error = read_block(inode->i_mount, parent, group->parent);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		current = drv_ufs_get64(group->parent, index * 8U,
					ms->super.swapped);
		drv_ufs_put64(group->parent, index * 8U, 0, ms->super.swapped);
	} else if (index < UFS_NDADDR) {
		current = group->image.direct[index];
		group->image.direct[index] = 0;
	} else {
		/* Checks the current index. */
		if (index - UFS_NDADDR >= UFS_NIADDR)
			return EIO;
		current = group->image.indirect[index - UFS_NDADDR];
		group->image.indirect[index - UFS_NDADDR] = 0;
	}

	/* Handles the current condition. */
	if (current != child)
		return EIO;
	group->image.blocks -= ms->super.bsize / UFS_SECTOR_SIZE;
	error = prepare_inode_locked(&group->image.inode, group->dinode,
				     &fragment);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = prepare_super_summaries(inode->i_mount, group->summaries);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Keeps the live free map unchanged until the release has committed. */
	for (n = 0; n < ms->super.frag; n++)
		bit_set(group->cg + ms->cg_freeoff, local + n);
	drv_ufs_put32(group->cg, UFS_CG_NBFREE, free_blocks + 1U,
		      ms->super.swapped);
	drv_ufs_put64(group->summaries, UFS_FS_CSTOTAL_NBFREE,
		      ms->super.cstotal_nbfree + 1U, ms->super.swapped);
	extents[0].target = (cgstart(&ms->super, cg) + ms->super.cblkno)
			    << ms->super.fsbtodb;
	extents[0].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[0].payload = group->cg;
	extents[1].target = UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;
	extents[1].sectors = UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;
	extents[1].payload = group->summaries;
	extents[2].target = fragment << ms->super.fsbtodb;
	extents[2].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[2].payload = group->dinode;
	count = 3;

	/* Handles the parent condition. */
	if (parent != 0) {
		extents[3].target = parent << ms->super.fsbtodb;
		extents[3].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
		extents[3].payload = group->parent;
		count++;
	}

	/*
 * Releases optional home-cache pins before the journal installs that
	 * block. */
	ms->cg_valid = 0;
	buf_view_release(&ms->cg_view);
	error = metadata_group_commit(inode->i_mount, extents, count, NULL,
				      &outcome);

	/* Handles the outcome condition. */
	if (outcome.committed) {
		memcpy(ms->cg, group->cg, ms->super.bsize);
		ms->super.cstotal_nbfree++;
		memcpy(ui->direct, group->image.direct, sizeof(ui->direct));
		memcpy(ui->indirect, group->image.indirect,
		       sizeof(ui->indirect));
		ui->blocks = group->image.blocks;
		quota_error = quota_release(&ms->quota, inode->i_uid,
					    inode->i_gid, 1, 0);

		/* Checks the operation status. */
		if (quota_error != 0) {
			ms->writable = 0;

			/* Checks the operation status. */
			if (error == 0)
				error = quota_error;
		}
	}

	/* Handles the outcome condition. */
	if (outcome.committed || outcome.uncertain) {
		ms->cg_valid = 0;
		ms->cg_dirty = outcome.uncertain;
		buf_view_release(&ms->cg_view);
	}

	/*
 * Preserves errno separately from any release that recovery
	 * established. */
	return error;
}

/* Reserves bounded private storage before a journal-backed pointer release. */
static int
release_group(
	struct inode *inode,
	uint64_t parent,
	unsigned index,
	uint64_t child,
	int *handled)
{
	struct ufs_mount_state *ms = state(inode->i_mount);
	struct ufs_release_group *group;
	size_t bytes;
	int error;

	/*
 * Keeps oversized or non-journal releases on the existing ordered path.
	 */
	*handled = 0;
	bytes = (parent != 0 ? 3U : 2U) * ms->super.bsize + UFS_SBLOCK_SIZE;

	/* Handles the ms condition. */
	if (!ms->journal_enabled)
		return 0;

	/* Handles the bytes condition. */
	if (bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)

		/* Reports successful completion. */
		return 0;
	*handled = 1;
	group = kern_calloc(1, sizeof(*group));

	/* Handles the group availability. */
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(3U * ms->super.bsize + UFS_SBLOCK_SIZE);

	/* Handles the memory availability. */
	if (group->memory == NULL) {
		kern_free(group);

		/* Returns the computed result. */
		return ENOMEM;
	}
	group->cg = group->memory;
	group->dinode = group->cg + ms->super.bsize;
	group->parent = group->dinode + ms->super.bsize;
	group->summaries = group->parent + ms->super.bsize;
	mutex_lock(&ms->lock);
	error = release_group_locked(inode, parent, index, child, group);
	mutex_unlock(&ms->lock);
	kern_free(group->memory);
	kern_free(group);

	/* Reports a complete bounded release or its original failure. */
	return error;
}
/* End consolidated ufs-release.inc. */

static int detach_inode_block(struct inode *inode, uint64_t *pointer);

/* Detach one inode-owned pointer durably before making its block reusable. On uncertain metadata I/O keep the allocation and stop further mutations. */
static int
detach_inode_block(
	struct inode *inode,
	uint64_t *pointer)
{
	struct ufs_mount_state *ms = state(inode->i_mount);
	struct ufs_inode_info *ui = info(inode);
	uint64_t fragment = *pointer;
	unsigned sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	unsigned index;
	int error;
	int handled;

	/* Handles the fragment condition. */
	if (fragment == 0)
		return 0;

	/* Handles the ui condition. */
	if (ui->blocks < sectors)
		return EIO;
	/* Process each remaining element. */
	for (index = 0; index < UFS_NDADDR; index++) {
		/* Handles the pointer condition. */
		if (pointer == &ui->direct[index])
			break;
	}

	/* Checks the current index. */
	if (index == UFS_NDADDR) {
		/* Process each remaining element. */
		for (index = 0; index < UFS_NIADDR; index++) {
			/* Handles the pointer condition. */
			if (pointer == &ui->indirect[index])
				break;
		}
		index += UFS_NDADDR;
	}
	error = release_group(inode, 0, index, fragment, &handled);

	/* Handles the handled condition. */
	if (handled)
		return error;
	*pointer = 0;
	ui->blocks -= sectors;
	error = persist_inode(inode);

	/* Checks the operation status. */
	if (error == 0)
		error = disk_sync(inode->i_mount->m_disk);

	/* Checks the operation status. */
	if (error == 0) {
		error = free_block(inode->i_mount, fragment, inode->i_uid,
				   inode->i_gid);
	}

	/* Checks the operation status. */
	if (error != 0)
		ms->writable = 0;

	/* Returns the computed result. */
	return error;
}

static int truncate_indirect(struct inode *inode, uint64_t root, unsigned depth, uint64_t base, uint64_t keep, int *empty);

/* Leave an empty root allocated until its caller has detached the owning pointer. A child is never freed while its parent still names it on disk. */
static int
truncate_indirect(
	struct inode *inode,
	uint64_t root,
	unsigned depth,
	uint64_t base,
	uint64_t keep,
	int *empty)
{
	uint64_t child;
	uint64_t child_base;
	int remove;
	int handled;
	struct ufs_mount_state *ms = state(inode->i_mount);
	struct ufs_inode_info *ui = info(inode);
	uint8_t *block;
	uint64_t child_span = indirect_span(&ms->super, depth - 1U);
	unsigned index, sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	int error = 0;

	*empty = 1;
	/* Handles the root condition. */
	if (root == 0)
		return 0;
	block = kern_malloc(ms->super.bsize);

	/* Handles the block availability. */
	if (block == NULL)
		return ENOMEM;
	error = read_block(inode->i_mount, root, block);

	/* Checks the operation status. */
	if (error != 0)
		goto out;
	/* Process each remaining element. */
	for (index = 0; index < ms->super.nindir; index++) {
		child = drv_ufs_get64(block, (size_t)index * 8U,
				      ms->super.swapped);
		child_base = base + (uint64_t)index * child_span;
		remove = 0;

		/* Checks the child process state. */
		if (child == 0)
			continue;

		/* Handles the depth condition. */
		if (depth == 1U) {
			remove = child_base >= keep;
		} else if (child_base + child_span > keep) {
			error = truncate_indirect(inode, child, depth - 1U,
						  child_base, keep, &remove);

			/* Checks the operation status. */
			if (error != 0)
				goto out;
		}

		/* Handles the remove condition. */
		if (!remove) {
			*empty = 0;
			continue;
		}

		/* Handles the ui condition. */
		if (ui->blocks < sectors) {
			error = EIO;
			goto out;
		}
		error = release_group(inode, root, index, child, &handled);

		/* Handles the handled condition. */
		if (handled) {
			/* Checks the operation status. */
			if (error != 0)
				goto out;
			drv_ufs_put64(block, (size_t)index * 8U, 0,
				      ms->super.swapped);
			continue;
		}
		drv_ufs_put64(block, (size_t)index * 8U, 0, ms->super.swapped);
		error = write_block(inode->i_mount, root, block);

		/* Checks the operation status. */
		if (error == 0)
			error = disk_sync(inode->i_mount->m_disk);

		/* Checks the operation status. */
		if (error == 0) {
			ui->blocks -= sectors;
			error = free_block(inode->i_mount, child, inode->i_uid,
					   inode->i_gid);
		}

		/* Checks the operation status. */
		if (error != 0) {
			ms->writable = 0;
			goto out;
		}
	}
out:
	kern_free(block);

	/* Returns the computed result. */
	return error;
}

static int ufs_truncate(struct inode *inode, off_t size);

/* Supports the ufs truncate operation. */
static int
ufs_truncate(
	struct inode *inode,
	off_t size)
{
	uint64_t fragment;
	int empty;
	struct ufs_mount_state *ms = state(inode->i_mount);
	struct ufs_inode_info *ui = info(inode);
	uint8_t *block = NULL;
	uint64_t keep, base;
	unsigned n;
	int error = 0;

	/* Handles the ms condition. */
	if (!ms->writable)
		return EROFS;

	/* Checks the current data size. */
	if (size < 0 || (uint64_t)size > ms->super.maxfilesize)
		return EFBIG;
	mutex_lock(&inode->i_lock);
	keep = ((uint64_t)size + ms->super.bsize - 1U) / ms->super.bsize;

	/* Checks the current data size. */
	if (size < inode->i_size && size != 0 && size % ms->super.bsize != 0) {
		fragment = 0;
		error = bmap(inode, (uint64_t)size / ms->super.bsize,
			     &fragment);

		/* Checks the operation status. */
		if (error != 0)
			goto out;

		/* Handles the fragment condition. */
		if (fragment != 0) {
			block = kern_malloc(ms->super.bsize);

			/* Handles the block availability. */
			if (block == NULL) {
				error = ENOMEM;
				goto out;
			}
			error = read_content_block(inode->i_mount, fragment,
						   block);

			/* Checks the operation status. */
			if (error != 0)
				goto out;
			memset(block + size % ms->super.bsize, 0,
			       ms->super.bsize - size % ms->super.bsize);
			error = write_content_block(inode->i_mount, fragment,
						    block);

			/* Checks the operation status. */
			if (error != 0)
				goto out;
		}
	}

	/*
 * Bound before narrowing: a large sparse size must not wrap to a
	 * direct-block index and release unrelated data. */
	/* Process each element required by the operation. */
	for (n = 0; n < UFS_NDADDR; n++) {
		/* Handles the uint64 t condition. */
		if ((uint64_t)n < keep)
			continue;
		error = detach_inode_block(inode, &ui->direct[n]);

		/* Checks the operation status. */
		if (error != 0)
			goto out;
	}
	base = UFS_NDADDR;
	/* Process each element required by the operation. */
	for (n = 0; n < UFS_NIADDR; n++) {
		error = truncate_indirect(inode, ui->indirect[n], n + 1U, base,
					  keep, &empty);

		/* Checks the operation status. */
		if (error == 0 && empty)
			error = detach_inode_block(inode, &ui->indirect[n]);

		/* Checks the operation status. */
		if (error != 0)
			goto out;
		base += indirect_span(&ms->super, n + 1U);
	}
	inode->i_size = size;
	error = persist_inode(inode);
out:
	kern_free(block);
	mutex_unlock(&inode->i_lock);

	/* Returns the computed result. */
	return error;
}

static enum inode_type mode_type(uint16_t mode);

/* Supports the mode type operation. */
static enum inode_type
mode_type(
	uint16_t mode)
{
	/* Dispatch the selected operation case. */
	switch (mode & UFS_IFMT) {
	case UFS_IFREG:
		/* Returns the computed result. */
		return INODE_REG;
	case UFS_IFDIR:
		/* Returns the computed result. */
		return INODE_DIR;
	case UFS_IFLNK:
		/* Returns the computed result. */
		return INODE_SYMLINK;
	case UFS_IFCHR:
		/* Returns the computed result. */
		return INODE_CHAR;
	case UFS_IFBLK:
		/* Returns the computed result. */
		return INODE_BLOCK;
	case UFS_IFIFO:
		/* Returns the computed result. */
		return INODE_FIFO;
	case UFS_IFSOCK:
		/* Returns the computed result. */
		return INODE_SOCKET;
	default:
		/* Returns the computed result. */
		return INODE_NONE;
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

	disk_size = drv_ufs_get64(raw, UFS_DI_SIZE, super->swapped);
	disk_blocks = drv_ufs_get64(raw, UFS_DI_BLOCKS, super->swapped);

	/* Handles the disk size condition. */
	if (disk_size >
	    (sizeof(off_t) == 8 ? (uint64_t)INT64_MAX : (uint64_t)INT32_MAX))

		/* Returns the computed result. */
		return EFBIG;

	/* Handles the disk blocks condition. */
	if (disk_blocks >
	    (sizeof(blkcnt_t) == 8 ? (uint64_t)INT64_MAX : (uint64_t)INT32_MAX))

		/* Returns the computed result. */
		return EOVERFLOW;
	*size = disk_size;
	*blocks = disk_blocks;
	/* Reports successful completion. */
	return 0;
}

static int decode_inode_raw(struct inode *inode, const uint8_t *raw, uint32_t number, int orphan);

/* Decodes one private identity, keeping namespace and recovery admission distinct. */
static int
decode_inode_raw(
	struct inode *inode,
	const uint8_t *raw,
	uint32_t number,
	int orphan)
{
	int needed;
	const struct ufs_super *s = &state(inode->i_mount)->super;
	struct ufs_inode_info *ui;
	uint64_t disk_size, disk_blocks;
	uint16_t mode;
	unsigned n;
	int error;

	/*
 * Validates the disk representation before populating the private
	 * inode. */
	mode = drv_ufs_get16(raw, UFS_DI_MODE, s->swapped);

	/* Checks the mode type result. */
	if (mode_type(mode) == INODE_NONE)
		return EOPNOTSUPP;
	error = inode_size_values(raw, s, &disk_size, &disk_blocks);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	ui = info(inode);
	inode->i_type = mode_type(mode);
	inode->i_ino = number;
	inode->i_mode = mode;
	inode->i_linkcount = drv_ufs_get16(raw, UFS_DI_NLINK, s->swapped);
	inode->i_size = (off_t)disk_size;
	inode->i_uid = drv_ufs_get32(raw, UFS_DI_UID, s->swapped);
	inode->i_gid = drv_ufs_get32(raw, UFS_DI_GID, s->swapped);
	inode->i_atime.tv_sec =
		(time_t)drv_ufs_get64(raw, UFS_DI_ATIME, s->swapped);
	inode->i_atime.tv_nsec =
		drv_ufs_get32(raw, UFS_DI_ATIMENSEC, s->swapped);
	inode->i_mtime.tv_sec =
		(time_t)drv_ufs_get64(raw, UFS_DI_MTIME, s->swapped);
	inode->i_mtime.tv_nsec =
		drv_ufs_get32(raw, UFS_DI_MTIMENSEC, s->swapped);
	inode->i_ctime.tv_sec =
		(time_t)drv_ufs_get64(raw, UFS_DI_CTIME, s->swapped);
	inode->i_ctime.tv_nsec =
		drv_ufs_get32(raw, UFS_DI_CTIMENSEC, s->swapped);
	ui->extattr_size = drv_ufs_get32(raw, UFS_DI_EXTSIZE, s->swapped);
	/* Process each element required by the operation. */
	for (n = 0; n < UFS_NXADDR; n++) {
		ui->extattr[n] =
			drv_ufs_get64(raw, UFS_DI_EXTB + n * 8U, s->swapped);
	}

	/* Handles the inode condition. */
	if (inode->i_type == INODE_CHAR || inode->i_type == INODE_BLOCK) {
		inode->i_rdev =
			(dev_t)drv_ufs_get64(raw, UFS_DI_DB, s->swapped);
	} else if (inode->i_type == INODE_SYMLINK &&
		   (uint64_t)inode->i_size <= s->maxsymlinklen &&
		   inode->i_size <= 120) {
		memcpy(ui->shortlink, raw + UFS_DI_DB, sizeof(ui->shortlink));
	} else {
		/* Process each element required by the operation. */
		for (n = 0; n < UFS_NDADDR; n++) {
			ui->direct[n] = drv_ufs_get64(raw, UFS_DI_DB + n * 8U,
						      s->swapped);
		}
		/* Process each element required by the operation. */
		for (n = 0; n < UFS_NIADDR; n++) {
			ui->indirect[n] = drv_ufs_get64(raw, UFS_DI_IB + n * 8U,
							s->swapped);
		}
	}
	ui->disk_flags = drv_ufs_get32(raw, UFS_DI_FLAGS, s->swapped);
	ui->blocks = disk_blocks;
	ui->generation = drv_ufs_get32(raw, UFS_DI_GEN, s->swapped);

	/* Handles the orphan condition. */
	if ((orphan ? inode->i_linkcount != 0 : inode->i_linkcount == 0) ||
	    inode->i_size < 0 || (uint64_t)inode->i_size > s->maxfilesize ||
	    ui->extattr_size > UFS_NXADDR * s->bsize ||
	    inode->i_atime.tv_nsec >= 1000000000L ||
	    inode->i_mtime.tv_nsec >= 1000000000L ||
	    inode->i_ctime.tv_nsec >= 1000000000L ||
	    (inode->i_type == INODE_DIR &&
	     ((!orphan && (uint64_t)inode->i_size < UFS_DIRBLKSIZ) ||
	      (uint64_t)inode->i_size % UFS_DIRBLKSIZ != 0))) {
		/* Returns the computed result. */
		return EIO;
	}
	/* Process each element required by the operation. */
	for (n = 0; n < UFS_NXADDR; n++) {
		needed = ui->extattr_size > n * s->bsize;

		/* Checks the valid inode fragment result. */
		if ((needed && ui->extattr[n] == 0) ||
		    (!needed && ui->extattr[n] != 0) ||
		    (needed && !valid_inode_fragment(s, ui->extattr[n]))) {
			/* Returns the computed result. */
			return EIO;
		}
	}

	/* Handles the inode condition. */
	if (!(inode->i_type == INODE_SYMLINK &&
	      (uint64_t)inode->i_size <= s->maxsymlinklen &&
	      inode->i_size <= 120)) {
		/* Process each element required by the operation. */
		for (n = 0; n < UFS_NDADDR; n++) {
			/* Checks the valid inode fragment result. */
			if (!valid_inode_fragment(s, ui->direct[n])) {
				return EIO;
			}
		}
		/* Process each element required by the operation. */
		for (n = 0; n < UFS_NIADDR; n++) {
			/* Checks the valid inode fragment result. */
			if (!valid_inode_fragment(s, ui->indirect[n])) {
				return EIO;
			}
		}
	}
	inode->i_op = &ufs_inode_ops;
	inode->i_fop = inode->i_type == INODE_DIR    ? &ufs_directory_ops
		       : inode->i_type == INODE_REG  ? &ufs_regular_ops
		       : inode->i_type == INODE_FIFO ? &fifo_file_ops
						     : NULL;

	/* Returns a validated identity without adding it to the inode cache. */
	return 0;
}

static int load_inode_locked(struct mount *mountp, uint32_t number, struct inode **result);

/* Supports the load inode locked operation. */
static int
load_inode_locked(
	struct mount *mountp,
	uint32_t number,
	struct inode **result)
{
	const struct ufs_super *s = &state(mountp)->super;
	struct inode *inode;
	uint8_t *block, *raw;
	uint32_t cg, index;
	uint64_t fragment, disk_size, disk_blocks;
	uint16_t mode;
	int error;

	/* Handles the number condition. */
	if (number < UFS_ROOT_INO || number >= s->ncg * s->ipg)
		return EIO;

	/* Checks the inode get result. */
	if (inode_get(mountp, number, result) == 0)
		return 0;
	cg = number / s->ipg;
	index = number % s->ipg;
	fragment = cgstart(s, cg) + s->iblkno + (index / s->inopb) * s->frag;
	block = kern_malloc(s->bsize);

	/* Handles the block availability. */
	if (block == NULL)
		return ENOMEM;
	error = read_block(mountp, fragment, block);

	/* Checks the operation status. */
	if (error != 0) {
		kern_free(block);

		/* Returns the computed result. */
		return error;
	}
	raw = block + (index % s->inopb) * UFS_DINODE_SIZE;
	mode = drv_ufs_get16(raw, UFS_DI_MODE, s->swapped);

	/* Checks the mode type result. */
	if (mode_type(mode) == INODE_NONE) {
		kern_free(block);

		/* Returns the computed result. */
		return EOPNOTSUPP;
	}
	error = inode_size_values(raw, s, &disk_size, &disk_blocks);

	/* Checks the operation status. */
	if (error != 0) {
		kern_free(block);

		/* Returns the computed result. */
		return error;
	}
	inode = inode_alloc(mountp);

	/* Handles the inode availability. */
	if (inode == NULL) {
		kern_free(block);

		/* Returns the computed result. */
		return ENOSPC;
	}
	error = decode_inode_raw(inode, raw, number, 0);

	/* Checks the operation status. */
	if (error != 0) {
		inode->i_flags |= INODE_DEAD;
		inode_release(inode);
		kern_free(block);

		/* Returns the computed result. */
		return error;
	}
	kern_free(block);
	*result = inode;
	/* Reports successful completion. */
	return 0;
}

static int load_inode(struct mount *mountp, uint32_t number, struct inode **result);

/* Creation already holds this gate. Miss, allocation and initialization must form one admission so aliases cannot publish duplicate in-core objects. */
static int
load_inode(
	struct mount *mountp,
	uint32_t number,
	struct inode **result)
{
	struct mutex *gate = &state(mountp)->namespace_lock;
	int entered = !mutex_owned(gate), error;

	/* Handles the entered condition. */
	if (entered)
		mutex_lock(gate);
	error = load_inode_locked(mountp, number, result);

	/* Handles the entered condition. */
	if (entered)
		mutex_unlock(gate);

	/* Returns the computed result. */
	return error;
}

static int next_dirent(struct inode *directory, off_t *cursor, uint32_t *number, uint8_t *type, char name[NAME_MAX + 1U]);

/* Supports the next dirent operation. */
static int
next_dirent(
	struct inode *directory,
	off_t *cursor,
	uint32_t *number,
	uint8_t *type,
	char name[NAME_MAX + 1U])
{
	uint16_t reclen;
	uint8_t namelen;
	ssize_t count;
	struct ufs_mount_state *ms = state(directory->i_mount);
	uint8_t head[8];
	int error = 0;

	/*
 * Refuses namespace bytes after unresolved journal I/O invalidated
	 * cache state. */
	if (ms->journal_enabled) {
		mutex_lock(&ms->journal_lock);

		/* Handles the ms condition. */
		if (ms->journal.poisoned)
			error = EIO;
		mutex_unlock(&ms->journal_lock);
	}

	/* Checks the operation status. */
	if (error != 0)
		return error;
	/* Process each remaining element. */
	while (*cursor < directory->i_size) {
		/* Handles the uint64 t condition. */
		if ((uint64_t)*cursor % UFS_DIRBLKSIZ > UFS_DIRBLKSIZ - 8U)
			return EIO;
		count = pread_inode(directory, head, sizeof(head), *cursor);

		/* Checks the remaining item count. */
		if (count != sizeof(head))
			return EIO;
		*number = drv_ufs_get32(
			head, 0, state(directory->i_mount)->super.swapped);
		reclen = drv_ufs_get16(
			head, 4, state(directory->i_mount)->super.swapped);
		*type = head[6];
		namelen = head[7];

		/* Handles the reclen condition. */
		if (reclen < 8U || (reclen & 3U) != 0 ||
		    8U + namelen > reclen ||
		    (uint64_t)*cursor % UFS_DIRBLKSIZ + reclen >
			    UFS_DIRBLKSIZ ||
		    (uint64_t)reclen >
			    (uint64_t)directory->i_size - (uint64_t)*cursor)

			/* Returns the computed result. */
			return EIO;

		/* Checks the pread inode result. */
		if (namelen != 0 && pread_inode(directory, name, namelen,
						*cursor + 8) != namelen)
			/* Returns the computed result. */
			return EIO;
		name[namelen] = '\0';
		*cursor += reclen;
		/* Handles the number condition. */
		if (*number != 0)
			return 0;
	}

	/* Returns the computed result. */
	return ENOENT;
}

static uint16_t dir_minimum(uint8_t length);

/* Supports the dir minimum operation. */
static uint16_t
dir_minimum(
	uint8_t length)
{
	/* Returns the computed result. */
	return (uint16_t)((8U + length + 3U) & ~3U);
}
static uint8_t dir_type(enum inode_type type);

/* Supports the dir type operation. */
static uint8_t
dir_type(
	enum inode_type type)
{
	/* Returns the computed result. */
	return type == INODE_FIFO      ? 1U
	       : type == INODE_DIR     ? 4U
	       : type == INODE_REG     ? 8U
	       : type == INODE_SYMLINK ? 10U
	       : type == INODE_SOCKET  ? 12U
				       : 0U;
}

static int restore_directory_block(struct inode *directory, uint64_t fragment, const uint8_t *original, int original_error);

/* Supports the restore directory block operation. */
static int
restore_directory_block(
	struct inode *directory,
	uint64_t fragment,
	const uint8_t *original,
	int original_error)
{
	struct ufs_mount_state *ms = state(directory->i_mount);
	int rollback = write_block(directory->i_mount, fragment, original);

	/* Handles the rollback condition. */
	if (rollback != 0) {
		ms->writable = 0;

		/* Returns the computed result. */
		return rollback;
	}

	/* Returns the computed result. */
	return original_error;
}

static int dir_find_record(struct inode *directory, const struct componentname *name, uint8_t *block, uint32_t *offset, uint32_t *previous, uint32_t *number);

/* Supports the dir find record operation. */
static int
dir_find_record(
	struct inode *directory,
	const struct componentname *name,
	uint8_t *block,
	uint32_t *offset,
	uint32_t *previous,
	uint32_t *number)
{
	struct ufs_mount_state *ms = state(directory->i_mount);
	uint32_t pos = 0, prev = UINT32_MAX;
	int error;

	/* Checks the info result. */
	if (directory->i_size < 0 ||
	    (uint64_t)directory->i_size > ms->super.bsize ||
	    (uint64_t)directory->i_size % UFS_DIRBLKSIZ != 0 ||
	    info(directory)->direct[0] == 0)

		/* Returns the computed result. */
		return EIO;
	error = read_block(directory->i_mount, info(directory)->direct[0],
			   block);

	/* Checks the operation status. */
	if (error)
		return error;
	/* Process each remaining element. */
	while (pos < (uint32_t)directory->i_size) {
		/* Handles the uint32 t condition. */
		if ((uint32_t)directory->i_size - pos < 8U ||
		    pos % UFS_DIRBLKSIZ > UFS_DIRBLKSIZ - 8U) {
			/* Returns the computed result. */
			return EIO;
		}
		uint32_t ino = drv_ufs_get32(block, pos, ms->super.swapped);
		uint16_t reclen =
			drv_ufs_get16(block, pos + 4U, ms->super.swapped);
		uint8_t nlen = block[pos + 7U];

		/* Handles the reclen condition. */
		if (reclen < 8U || (reclen & 3U) != 0 ||
		    pos % UFS_DIRBLKSIZ + reclen > UFS_DIRBLKSIZ ||
		    pos + reclen > (uint32_t)directory->i_size ||
		    8U + nlen > reclen)

			/* Returns the computed result. */
			return EIO;

		/* Handles the ino condition. */
		if (ino != 0 && nlen == name->cn_namelen &&
		    memcmp(block + pos + 8U, name->cn_nameptr, nlen) == 0) {
			*offset = pos;
			*previous = prev;
			*number = ino;
			/* Reports successful completion. */
			return 0;
		}
		prev = pos;
		pos += reclen;
	}

	/* Returns the computed result. */
	return ENOENT;
}

static int dir_add(struct inode *directory, const struct componentname *name, uint32_t number, uint8_t type);

/* Supports the dir add operation. */
static int
dir_add(
	struct inode *directory,
	const struct componentname *name,
	uint32_t number,
	uint8_t type)
{
	uint32_t at;
	struct ufs_mount_state *ms = state(directory->i_mount);
	struct ufs_inode_info *ui = info(directory);
	uint8_t *block, *original;
	uint16_t need;
	uint32_t pos = 0;
	uint64_t old_direct, allocated = 0, old_blocks;
	off_t old_size;
	int error, rollback;
	int handled;

	/* Validates the current name. */
	if (name->cn_namelen == 0 || name->cn_namelen > 255U)
		return EINVAL;
	/* Process each element required by the operation. */
	for (pos = 0; pos < name->cn_namelen; pos++) {
		/* Validates the current name. */
		if (name->cn_nameptr[pos] == '/')
			return EINVAL;
	}
	pos = 0;
	need = dir_minimum((uint8_t)name->cn_namelen);
	block = kern_calloc(1, ms->super.bsize);
	original = kern_malloc(ms->super.bsize);

	/* Handles the block availability. */
	if (block == NULL || original == NULL) {
		kern_free(block);
		kern_free(original);

		/* Returns the computed result. */
		return ENOMEM;
	}
	mutex_lock(&directory->i_lock);
	old_size = directory->i_size;
	old_direct = ui->direct[0];
	old_blocks = ui->blocks;

	/* Handles the old size condition. */
	if (old_size < 0 || (uint64_t)old_size > ms->super.bsize ||
	    (uint64_t)old_size % UFS_DIRBLKSIZ != 0) {
		error = EIO;
		goto out;
	}

	/* Handles the ui condition. */
	if (ui->direct[0] == 0) {
		error = directory_backing_group(directory, &handled);

		/* Handles the handled condition. */
		if (handled) {
			/* Checks the operation status. */
			if (error != 0)
				goto out;

			/*
 * Retain committed empty backing if later entry
			 * publication fails. */
			old_direct = ui->direct[0];
			old_blocks = ui->blocks;
		} else {
			error = allocate_block(
				directory->i_mount, directory->i_uid,
				directory->i_gid, &ui->direct[0]);

			/* Checks the operation status. */
			if (error)
				goto out;
			allocated = ui->direct[0];
			ui->blocks += ms->super.bsize / UFS_SECTOR_SIZE;
		}
	}
	error = read_block(directory->i_mount, ui->direct[0], block);

	/* Checks the operation status. */
	if (error)
		goto out;
	memcpy(original, block, ms->super.bsize);
	/* Process each remaining element. */
	while (pos < (uint32_t)directory->i_size) {
		/* Handles the uint32 t condition. */
		if ((uint32_t)directory->i_size - pos < 8U ||
		    pos % UFS_DIRBLKSIZ > UFS_DIRBLKSIZ - 8U) {
			error = EIO;
			goto out;
		}
		uint16_t reclen =
			drv_ufs_get16(block, pos + 4U, ms->super.swapped);
		uint8_t nlen = block[pos + 7U];
		uint16_t minimum = dir_minimum(nlen);

		/* Handles the reclen condition. */
		if (reclen < minimum || (reclen & 3U) != 0 ||
		    pos % UFS_DIRBLKSIZ + reclen > UFS_DIRBLKSIZ ||
		    reclen > (uint32_t)directory->i_size - pos) {
			error = EIO;
			goto out;
		}

		/* Handles the reclen condition. */
		if (reclen - minimum >= need) {
			at = pos + minimum;
			drv_ufs_put16(block, pos + 4U, minimum,
				      ms->super.swapped);
			drv_ufs_put32(block, at, number, ms->super.swapped);
			drv_ufs_put16(block, at + 4U, reclen - minimum,
				      ms->super.swapped);
			block[at + 6U] = type;
			block[at + 7U] = (uint8_t)name->cn_namelen;
			memcpy(block + at + 8U, name->cn_nameptr,
			       name->cn_namelen);
			error = write_block(directory->i_mount, ui->direct[0],
					    block);
			goto commit;
		}
		pos += reclen;
	}

	/* Handles the uint64 t condition. */
	if ((uint64_t)directory->i_size + UFS_DIRBLKSIZ > ms->super.bsize) {
		error = ENOSPC;
		goto out;
	}
	pos = (uint32_t)directory->i_size;
	drv_ufs_put32(block, pos, number, ms->super.swapped);
	drv_ufs_put16(block, pos + 4U, UFS_DIRBLKSIZ, ms->super.swapped);
	block[pos + 6U] = type;
	block[pos + 7U] = (uint8_t)name->cn_namelen;
	memcpy(block + pos + 8U, name->cn_nameptr, name->cn_namelen);
	directory->i_size += UFS_DIRBLKSIZ;
	error = write_block(directory->i_mount, ui->direct[0], block);
commit:

	/* Checks the operation status. */
	if (error == 0)
		error = persist_inode(directory);

	/* Checks the operation status. */
	if (error != 0) {
		rollback = restore_directory_block(directory, ui->direct[0],
						   original, error);
		directory->i_size = old_size;
		ui->direct[0] = old_direct;
		ui->blocks = old_blocks;
		error = persist_inode(directory);

		/* Checks the operation status. */
		if (error == 0)
			error = disk_sync(directory->i_mount->m_disk);

		/* Checks the operation status. */
		if (error != 0) {
			ms->writable = 0;
		} else if (ms->writable && allocated != 0) {
			error = free_block(directory->i_mount, allocated,
					   directory->i_uid, directory->i_gid);

			/* Checks the operation status. */
			if (error != 0)
				ms->writable = 0;
		}

		/* Checks the operation status. */
		if (error == 0)
			error = rollback;
	}
out:

	/* Checks the operation status. */
	if (error != 0 && allocated != 0 && ui->direct[0] == allocated) {
		directory->i_size = old_size;
		ui->direct[0] = old_direct;
		ui->blocks = old_blocks;
		rollback = persist_inode(directory);

		/* Handles the rollback condition. */
		if (rollback == 0)
			rollback = disk_sync(directory->i_mount->m_disk);

		/* Handles the rollback condition. */
		if (rollback != 0) {
			ms->writable = 0;
			error = rollback;
		} else {
			rollback =
				free_block(directory->i_mount, allocated,
					   directory->i_uid, directory->i_gid);

			/* Handles the rollback condition. */
			if (rollback != 0) {
				ms->writable = 0;
				error = rollback;
			}
		}
	}
	mutex_unlock(&directory->i_lock);
	kern_free(original);
	kern_free(block);

	/* Returns the computed result. */
	return error;
}

static int dir_remove(struct inode *directory, const struct componentname *name, uint32_t *number);

/* Supports the dir remove operation. */
static int
dir_remove(
	struct inode *directory,
	const struct componentname *name,
	uint32_t *number)
{
	uint16_t prior;
	struct ufs_mount_state *ms = state(directory->i_mount);
	uint8_t *block = kern_malloc(ms->super.bsize);
	uint8_t *original = kern_malloc(ms->super.bsize);
	uint32_t offset, previous;
	int error;

	/* Handles the block availability. */
	if (block == NULL || original == NULL) {
		kern_free(block);
		kern_free(original);

		/* Returns the computed result. */
		return ENOMEM;
	}
	mutex_lock(&directory->i_lock);
	error = dir_find_record(directory, name, block, &offset, &previous,
				number);

	/* Checks the operation status. */
	if (error == 0) {
		memcpy(original, block, ms->super.bsize);
		uint16_t reclen =
			drv_ufs_get16(block, offset + 4U, ms->super.swapped);

		/* Handles the previous condition. */
		if (previous != UINT32_MAX &&
		    previous / UFS_DIRBLKSIZ == offset / UFS_DIRBLKSIZ) {
			prior = drv_ufs_get16(block, previous + 4U,
					      ms->super.swapped);
			drv_ufs_put16(block, previous + 4U, prior + reclen,
				      ms->super.swapped);
		} else {
			drv_ufs_put32(block, offset, 0, ms->super.swapped);
		}
		error = write_block(directory->i_mount,
				    info(directory)->direct[0], block);

		/* Checks the operation status. */
		if (error != 0) {
			error = restore_directory_block(
				directory, info(directory)->direct[0], original,
				error);
		}
	}
	mutex_unlock(&directory->i_lock);
	kern_free(original);
	kern_free(block);

	/* Returns the computed result. */
	return error;
}

static int dir_replace(struct inode *directory, const struct componentname *name, uint32_t number, uint8_t type, uint32_t *old_number, uint8_t *old_type);

/* Supports the dir replace operation. */
static int
dir_replace(
	struct inode *directory,
	const struct componentname *name,
	uint32_t number,
	uint8_t type,
	uint32_t *old_number,
	uint8_t *old_type)
{
	struct ufs_mount_state *ms = state(directory->i_mount);
	uint8_t *block = kern_malloc(ms->super.bsize);
	uint8_t *original = kern_malloc(ms->super.bsize);
	uint32_t offset, previous;
	int error;

	/* Handles the block availability. */
	if (block == NULL || original == NULL) {
		kern_free(block);
		kern_free(original);

		/* Returns the computed result. */
		return ENOMEM;
	}
	mutex_lock(&directory->i_lock);
	error = dir_find_record(directory, name, block, &offset, &previous,
				old_number);

	/* Checks the operation status. */
	if (error == 0) {
		memcpy(original, block, ms->super.bsize);
		(void)previous;
		*old_type = block[offset + 6U];
		drv_ufs_put32(block, offset, number, ms->super.swapped);
		block[offset + 6U] = type;
		error = write_block(directory->i_mount,
				    info(directory)->direct[0], block);

		/* Checks the operation status. */
		if (error != 0) {
			error = restore_directory_block(
				directory, info(directory)->direct[0], original,
				error);
		}
	}
	mutex_unlock(&directory->i_lock);
	kern_free(original);
	kern_free(block);

	/* Returns the computed result. */
	return error;
}

static int name_is_dot(const struct componentname *name);

/* Supports the name is dot operation. */
static int
name_is_dot(
	const struct componentname *name)
{
	/* Returns the computed result. */
	return (name->cn_namelen == 1U && name->cn_nameptr[0] == '.') ||
	       (name->cn_namelen == 2U && name->cn_nameptr[0] == '.' &&
		name->cn_nameptr[1] == '.');
}

static void detach_new_socket_special(struct inode *inode);

/* Supports the detach new socket special operation. */
static void
detach_new_socket_special(
	struct inode *inode)
{
	/*
 * The pathname socket endpoint in a creation request is borrowed.  On
	 * any failed publication, detach it before releasing a possibly cached
	 * inode.  This also makes a name retained by an unsuccessful directory
	 * rollback inert instead of exposing a future dangling endpoint.
	 */
	if (inode == NULL)
		return;
	mutex_lock(&inode->i_lock);

	/* Handles the inode condition. */
	if (inode->i_type == INODE_SOCKET) {
		inode->i_special = NULL;
		inode->i_special_destroy = NULL;
	}
	mutex_unlock(&inode->i_lock);
}

static int discard_new_inode(struct inode *inode, int directory_counted);

/* Supports the discard new inode operation. */
static int
discard_new_inode(
	struct inode *inode,
	int directory_counted)
{
	int function_result;
	struct ufs_mount_state *ms = state(inode->i_mount);
	struct ufs_inode_info *ui = info(inode);
	uint64_t block = ui->direct[0], extattr[UFS_NXADDR];
	uint32_t number = (uint32_t)inode->i_ino,
		 old_extattr_size = ui->extattr_size;
	mode_t old_mode = inode->i_mode;
	enum inode_type old_type = inode->i_type;
	nlink_t old_links = inode->i_linkcount;
	off_t old_size = inode->i_size;
	uint64_t old_blocks = ui->blocks;
	uid_t uid = inode->i_uid;
	gid_t gid = inode->i_gid;
	unsigned n;
	int error = 0, cleanup;

	/* Handles the ms condition. */
	if (ms->journal_enabled && ms->journal.sector_count > 2U &&
	    (2U * ms->super.bsize + UFS_SBLOCK_SIZE) / UFS_SECTOR_SIZE <=
		    UFS_JOURNAL_GROUP_SECTORS &&
	    (2U * ms->super.bsize + UFS_SBLOCK_SIZE) / UFS_SECTOR_SIZE <=
		    ms->journal.sector_count - 2U &&
	    (inode->i_type != INODE_DIR || directory_counted)) {
		/* Obtains the discard reserved inode result. */
		function_result = discard_reserved_inode(inode);

		/* Returns the computed result. */
		return function_result;
	}
	detach_new_socket_special(inode);
	/* Process each element required by the operation. */
	for (n = 1; n < UFS_NDADDR; n++) {
		/* Handles the ui condition. */
		if (ui->direct[n] != 0) {
			ms->writable = 0;
			inode_release(inode);

			/* Returns the computed result. */
			return EIO;
		}
	}
	/* Process each element required by the operation. */
	for (n = 0; n < UFS_NIADDR; n++) {
		/* Handles the ui condition. */
		if (ui->indirect[n] != 0) {
			ms->writable = 0;
			inode_release(inode);

			/* Returns the computed result. */
			return EIO;
		}
	}
	/* Process each element required by the operation. */
	for (n = 0; n < UFS_NXADDR; n++) {
		extattr[n] = ui->extattr[n];
		ui->extattr[n] = 0;
	}
	inode->i_mode = 0;
	inode->i_type = INODE_NONE;
	inode->i_linkcount = 0;
	inode->i_size = 0;
	ui->direct[0] = 0;
	ui->extattr_size = 0;
	ui->blocks = 0;
	cleanup = persist_inode(inode);

	/* Handles the cleanup condition. */
	if (cleanup == 0)
		cleanup = disk_sync(inode->i_mount->m_disk);

	/* Handles the cleanup condition. */
	if (cleanup != 0) {
		inode->i_mode = old_mode;
		inode->i_type = old_type;
		inode->i_linkcount = old_links;
		inode->i_size = old_size;
		ui->direct[0] = block;
		ui->extattr_size = old_extattr_size;
		/* Process each element required by the operation. */
		for (n = 0; n < UFS_NXADDR; n++)
			ui->extattr[n] = extattr[n];
		ui->blocks = old_blocks;
		ms->writable = 0;
		inode_release(inode);

		/* Returns the computed result. */
		return cleanup;
	}

	/* Handles the directory counted condition. */
	if (directory_counted) {
		cleanup = adjust_directory_count(inode->i_mount, number, -1);

		/* Checks the operation status. */
		if (error == 0 && cleanup != 0)
			error = cleanup;
	}

	/* Handles the block condition. */
	if (block != 0) {
		cleanup = free_block(inode->i_mount, block, uid, gid);

		/* Checks the operation status. */
		if (error == 0 && cleanup != 0)
			error = cleanup;
	}
	/* Process each element required by the operation. */
	for (n = 0; n < UFS_NXADDR; n++) {
		/* Handles the extattr condition. */
		if (extattr[n] != 0) {
			cleanup = free_block(inode->i_mount, extattr[n], uid,
					     gid);

			/* Checks the operation status. */
			if (error == 0 && cleanup != 0)
				error = cleanup;
		}
	}
	cleanup = free_inode_number(inode->i_mount, number, uid, gid);

	/* Checks the operation status. */
	if (error == 0 && cleanup != 0)
		error = cleanup;

	/* Checks the operation status. */
	if (error != 0)
		ms->writable = 0;
	inode->i_ino = 0;
	inode->i_flags |= INODE_DEAD;
	inode_release(inode);

	/* Returns the computed result. */
	return error;
}

static int discard_new_inode_after_error(struct inode *inode, int directory_counted, int original_error);

/* Supports the discard new inode after error operation. */
static int
discard_new_inode_after_error(
	struct inode *inode,
	int directory_counted,
	int original_error)
{
	struct ufs_mount_state *ms = state(inode->i_mount);
	int cleanup;

	/* Handles the ms condition. */
	if (!ms->writable) {
		detach_new_socket_special(inode);
		inode_release(inode);

		/* Returns the computed result. */
		return original_error;
	}
	cleanup = discard_new_inode(inode, directory_counted);

	/* Returns the computed result. */
	return cleanup != 0 ? cleanup : original_error;
}

/* Begin consolidated ufs-inode-reservation.inc. */
/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

struct ufs_inode_reservation {
	struct ufs_release_group images;
	struct ufs_transaction_outcome outcome;
	struct quota_charge charge;
};

static int reserve_inode_locked(struct inode *inode, const struct inode_creation_request *request, struct ufs_inode_reservation *group);
static int reserve_inode_group(struct inode *inode, const struct inode_creation_request *request);

/* Reserves a number with initialized zero-link identity and directory accounting. */
static int
reserve_inode_locked(
	struct inode *inode,
	const struct inode_creation_request *request,
	struct ufs_inode_reservation *group)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *image;
	struct ufs_journal_extent extents[3];
	uint8_t *raw;
	uint64_t fragment;
	uint32_t cg;
	uint32_t attempt;
	uint32_t local;
	uint32_t free_inodes;
	uint32_t directories;
	uint32_t generation;
	mode_t kind;
	int is_directory;
	int found;
	int error;

	/*
 * Validates the requested kind before selecting an unowned inode slot.
	 */
	ms = state(inode->i_mount);

	/* Handles the ms condition. */
	if (!ms->writable)
		return EROFS;
	kind = inode_type_mode(request->type);

	/* Handles the kind condition. */
	if (kind == 0 || inode->i_ino != 0)
		return EINVAL;
	found = 0;
	cg = local = 0;
	is_directory = request->type == INODE_DIR;

	/* Finds a free number while excluding all allocation-map mutations. */
	for (attempt = 0; attempt < ms->super.ncg; attempt++) {
		cg = (ms->rotor_cg + attempt) % ms->super.ncg;
		error = load_cg_locked(inode->i_mount, cg);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		local = cg == 0 ? UFS_ROOT_INO + 1U : 0U;
		/* Process each element required by the operation. */
		for (; local < ms->super.ipg; local++) {
			/* Checks the bit test result. */
			if (!bit_test(ms->cg + ms->cg_iusedoff, local)) {
				found = 1;
				break;
			}
		}

		/* Handles the found condition. */
		if (found)
			break;
	}

	/* Handles the found condition. */
	if (!found)
		return ENOSPC;
	free_inodes = drv_ufs_get32(ms->cg, UFS_CG_NIFREE, ms->super.swapped);
	directories = drv_ufs_get32(ms->cg, UFS_CG_NDIR, ms->super.swapped);

	/* Handles the free inodes condition. */
	if (free_inodes == 0 || ms->super.cstotal_nifree == 0 ||
	    (is_directory && (directories == UINT32_MAX ||
			      ms->super.cstotal_ndir == UINT64_MAX)))

		/* Returns the computed result. */
		return EIO;

	/*
 * Initializes all persistent ownership fields before making the slot
	 * allocated. */
	image = &group->images.image;
	memcpy(image, info(inode), sizeof(*image));
	image->inode.i_ino = (uint64_t)cg * ms->super.ipg + local;
	image->inode.i_type = request->type;
	image->inode.i_mode = kind | (request->mode & 07777U);
	image->inode.i_uid = request->uid;
	image->inode.i_gid = request->gid;
	image->inode.i_rdev = request->rdev;
	image->inode.i_linkcount = 0;
	image->inode.i_size = 0;
	image->blocks = 0;
	image->extattr_size = 0;
	memset(image->direct, 0, sizeof(image->direct));
	memset(image->indirect, 0, sizeof(image->indirect));
	memset(image->extattr, 0, sizeof(image->extattr));
	memset(image->shortlink, 0, sizeof(image->shortlink));
	fragment = inode_fragment(&image->inode);
	error = read_block(inode->i_mount, fragment, group->images.dinode);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	raw = group->images.dinode +
	      (local % ms->super.inopb) * UFS_DINODE_SIZE;
	generation = drv_ufs_get32(raw, UFS_DI_GEN, ms->super.swapped) + 1U;

	/* Handles the generation condition. */
	if (generation == 0)
		generation = 1;
	memset(raw, 0, UFS_DINODE_SIZE);
	image->generation = generation;
	encode_inode_locked(&image->inode, group->images.dinode);
	drv_ufs_put32(raw, UFS_DI_GEN, generation, ms->super.swapped);
	error = prepare_super_summaries(inode->i_mount,
					group->images.summaries);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/*
 * Couples the new identity with inode and directory allocation totals.
	 */
	memcpy(group->images.cg, ms->cg, ms->super.bsize);
	bit_set(group->images.cg + ms->cg_iusedoff, local);
	drv_ufs_put32(group->images.cg, UFS_CG_NIFREE, free_inodes - 1U,
		      ms->super.swapped);
	drv_ufs_put64(group->images.summaries, UFS_FS_CSTOTAL_NIFREE,
		      ms->super.cstotal_nifree - 1U, ms->super.swapped);

	/* Handles the directory condition. */
	if (is_directory) {
		drv_ufs_put32(group->images.cg, UFS_CG_NDIR, directories + 1U,
			      ms->super.swapped);
		drv_ufs_put64(group->images.summaries, UFS_FS_CSTOTAL_NDIR,
			      ms->super.cstotal_ndir + 1U, ms->super.swapped);
	}
	extents[0].target = (cgstart(&ms->super, cg) + ms->super.cblkno)
			    << ms->super.fsbtodb;
	extents[0].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[0].payload = group->images.cg;
	extents[1].target = UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;
	extents[1].sectors = UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;
	extents[1].payload = group->images.summaries;
	extents[2].target = fragment << ms->super.fsbtodb;
	extents[2].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[2].payload = group->images.dinode;
	ms->cg_valid = 0;
	buf_view_release(&ms->cg_view);
	error = metadata_group_commit(inode->i_mount, extents, 3, NULL,
				      &group->outcome);

	/*
 * Publishes only established identity without copying mutexes or
	 * reference state. */
	if (group->outcome.committed) {
		memcpy(ms->cg, group->images.cg, ms->super.bsize);
		ms->super.cstotal_nifree--;

		/* Handles the directory condition. */
		if (is_directory)
			ms->super.cstotal_ndir++;
		ms->rotor_cg = cg;
		inode->i_ino = image->inode.i_ino;
		inode->i_type = image->inode.i_type;
		inode->i_mode = image->inode.i_mode;
		inode->i_uid = image->inode.i_uid;
		inode->i_gid = image->inode.i_gid;
		inode->i_rdev = image->inode.i_rdev;
		info(inode)->generation = generation;
	}

	/* Handles the group condition. */
	if (group->outcome.committed || group->outcome.uncertain)
		ms->cg_dirty = group->outcome.uncertain;

	/*
 * Preserves original errors separately from durable identity ownership.
	 */
	return error;
}

/* Owns quota and private images for a supported zero-link inode reservation. */
static int
reserve_inode_group(
	struct inode *inode,
	const struct inode_creation_request *request)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_reservation *group;
	size_t bytes;
	int error;

	/*
 * Allocates all staging memory before reserving quota or entering the
	 * mount. */
	ms = state(inode->i_mount);
	bytes = 2U * ms->super.bsize + UFS_SBLOCK_SIZE;
	group = kern_calloc(1, sizeof(*group));

	/* Handles the group availability. */
	if (group == NULL)
		return ENOMEM;
	group->images.memory = kern_malloc(bytes);

	/* Handles the memory availability. */
	if (group->images.memory == NULL) {
		kern_free(group);

		/* Returns the computed result. */
		return ENOMEM;
	}
	group->images.cg = group->images.memory;
	group->images.dinode = group->images.cg + ms->super.bsize;
	group->images.summaries = group->images.dinode + ms->super.bsize;
	error = quota_reserve(&ms->quota, request->uid, request->gid, 0, 1,
			      quota_now(), &group->charge);

	/* Checks the operation status. */
	if (error == 0) {
		mutex_lock(&inode->i_lock);
		mutex_lock(&ms->lock);
		error = reserve_inode_locked(inode, request, group);

		/*
 * Holds quota for every possibly committed reservation until
		 * recovery. */
		if (group->outcome.committed || group->outcome.uncertain)
			quota_commit(&group->charge);
		else
			quota_rollback(&group->charge);
		mutex_unlock(&ms->lock);
		mutex_unlock(&inode->i_lock);
	}
	kern_free(group->images.memory);
	kern_free(group);

	/*
 * Leaves failure cleanup and final name publication to the creation
	 * owner. */
	return error;
}
static int new_inode(struct inode *directory, const struct inode_creation_request *request, nlink_t links, struct inode **result);

/* End consolidated ufs-inode-reservation.inc. */
static int
new_inode(
	struct inode *directory,
	const struct inode_creation_request *request,
	nlink_t links,
	struct inode **result)
{
	int function_result;
	struct mount *mountp;
	struct inode *inode;
	uint32_t number = 0;
	int error;
	int grouped = 0;
	int directory_counted = 0;
	size_t reservation_bytes;
	struct ufs_mount_state *ms;

	/* Handles the directory availability. */
	if (directory == NULL || request == NULL || result == NULL)
		return EINVAL;
	*result = NULL;
	mountp = directory->i_mount;
	ms = state(mountp);

	/*
 * Admit the complete preparation chain, including first directory
	 * backing. */
	reservation_bytes = 3U * ms->super.bsize + UFS_SBLOCK_SIZE;
	grouped = ms->journal_enabled && ms->journal.sector_count > 2U &&
		  reservation_bytes / UFS_SECTOR_SIZE <=
			  UFS_JOURNAL_GROUP_SECTORS &&
		  reservation_bytes / UFS_SECTOR_SIZE <=
			  ms->journal.sector_count - 2U;

	/* Handles the grouped condition. */
	if (grouped) {
		inode = inode_alloc(mountp);

		/* Handles the inode availability. */
		if (inode == NULL)
			return ENOSPC;
		inode->i_op = &ufs_inode_ops;
		error = reserve_inode_group(inode, request);

		/* Checks the operation status. */
		if (error != 0) {
			inode->i_flags |= INODE_DEAD;
			inode_release(inode);

			/* Returns the computed result. */
			return error;
		}
		number = (uint32_t)inode->i_ino;
		directory_counted = request->type == INODE_DIR;
	} else {
		error = allocate_inode_number(mountp, request->uid,
					      request->gid, &number);

		/* Checks the operation status. */
		if (error)
			return error;
		inode = inode_alloc(mountp);

		/* Handles the inode availability. */
		if (inode == NULL) {
			error = free_inode_number(mountp, number, request->uid,
						  request->gid);

			/* Checks the operation status. */
			if (error != 0) {
				state(mountp)->writable = 0;

				/* Returns the computed result. */
				return error;
			}

			/* Returns the computed result. */
			return ENOSPC;
		}
	}
	inode->i_ino = number;
	inode->i_type = request->type;
	inode->i_linkcount = grouped ? 0 : links;
	inode->i_op = &ufs_inode_ops;
	inode->i_fop = request->type == INODE_DIR    ? &ufs_directory_ops
		       : request->type == INODE_REG  ? &ufs_regular_ops
		       : request->type == INODE_FIFO ? &fifo_file_ops
						     : NULL;

	/* Handles the grouped condition. */
	if (!grouped)
		info(inode)->generation = number;
	error = inode_creation_prepare(directory, inode, request);

	/* Checks the operation status. */
	if (error != 0) {
		/* Obtains the discard new inode after error result. */
		function_result = discard_new_inode_after_error(
			inode, directory_counted, error);

		/* Returns the computed result. */
		return function_result;
	}
	error = persist_inode(inode);

	/* Checks the operation status. */
	if (error) {
		/* Obtains the discard new inode after error result. */
		function_result = discard_new_inode_after_error(
			inode, directory_counted, error);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the request condition. */
	if (request->type == INODE_DIR && !directory_counted) {
		error = adjust_directory_count(mountp, number, 1);

		/* Checks the operation status. */
		if (error != 0) {
			/* Obtains the discard new inode after error result. */
			function_result = discard_new_inode_after_error(
				inode, directory_counted, error);

			/* Returns the computed result. */
			return function_result;
		}
	}
	*result = inode;
	/* Reports successful completion. */
	return 0;
}

static int ufs_lookup_locked(struct inode *directory, const struct componentname *component, struct inode **result);

/* Supports the ufs lookup locked operation. */
static int
ufs_lookup_locked(
	struct inode *directory,
	const struct componentname *component,
	struct inode **result)
{
	int function_result;
	off_t cursor = 0;
	uint32_t number;
	uint8_t type;
	char name[NAME_MAX + 1U];
	int error;

	/* Process each linked entry. */
	while ((error = next_dirent(directory, &cursor, &number, &type,
				    name)) == 0) {
		/* Checks the strlen result. */
		if (strlen(name) == component->cn_namelen &&
		    memcmp(name, component->cn_nameptr,
			   component->cn_namelen) == 0) {
			/* Obtains the load inode result. */
			function_result =
				load_inode(directory->i_mount, number, result);

			/* Returns the computed result. */
			return function_result;
		}
	}

	/* Returns the computed result. */
	return error;
}

/* Supports the ufs lookup operation. */
static int
ufs_lookup(
	struct inode *directory,
	const struct componentname *component,
	struct inode **result)
{
	struct mutex *gate = &state(directory->i_mount)->namespace_lock;
	int entered = !mutex_owned(gate), error;

	/* Handles the entered condition. */
	if (entered)
		mutex_lock(gate);
	error = ufs_lookup_locked(directory, component, result);

	/* Handles the entered condition. */
	if (entered)
		mutex_unlock(gate);

	/* Returns the computed result. */
	return error;
}

/* Begin consolidated ufs-namespace.inc. */
/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

struct ufs_remove_group {
	struct ufs_inode_info image;
	struct ufs_inode_info parent_image;
	struct ufs_transaction_outcome outcome;
	uint8_t *memory;
	uint8_t *directory;
	uint8_t *dinode;
	uint8_t *parent_dinode;
};

static int remove_group_locked(struct inode *directory, const struct componentname *name, struct inode *target, struct ufs_remove_group *group);
static int remove_group(struct inode *directory, const struct componentname *name, struct inode *target, int *handled);

/* Prepares one namespace removal and its target link count under shared ownership. */
static int
remove_group_locked(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target,
	struct ufs_remove_group *group)
{
	struct ufs_mount_state *ms = state(directory->i_mount);
	struct ufs_journal_extent extents[3];
	uint64_t fragment;
	uint64_t parent_fragment;
	unsigned count = 2;
	int removing_directory = target->i_type == INODE_DIR;
	uint32_t offset;
	uint32_t previous;
	uint32_t number;
	uint16_t length;
	uint16_t prior;
	int error;

	/*
 * Validates the locked name-to-inode relation before editing private
	 * bytes. */
	if (!ms->writable)
		return EROFS;

	/* Handles the target condition. */
	if (target->i_linkcount == 0 ||
	    (removing_directory && directory->i_linkcount == 0))

		/* Returns the computed result. */
		return EIO;
	error = dir_find_record(directory, name, group->directory, &offset,
				&previous, &number);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the number condition. */
	if (number != (uint32_t)target->i_ino)
		return EIO;
	length =
		drv_ufs_get16(group->directory, offset + 4U, ms->super.swapped);

	/* Handles the previous condition. */
	if (previous != UINT32_MAX &&
	    previous / UFS_DIRBLKSIZ == offset / UFS_DIRBLKSIZ) {
		prior = drv_ufs_get16(group->directory, previous + 4U,
				      ms->super.swapped);
		drv_ufs_put16(group->directory, previous + 4U, prior + length,
			      ms->super.swapped);
	} else {
		drv_ufs_put32(group->directory, offset, 0, ms->super.swapped);
	}
	memcpy(&group->image, info(target), sizeof(group->image));

	/* Handles the removing directory condition. */
	if (removing_directory)
		group->image.inode.i_linkcount = 0;
	else
		group->image.inode.i_linkcount--;
	error = prepare_inode_locked(&group->image.inode, group->dinode,
				     &fragment);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/*
 * Merges parent accounting with the target when both share a dinode
	 * block. */
	if (removing_directory) {
		memcpy(&group->parent_image, info(directory),
		       sizeof(group->parent_image));
		group->parent_image.inode.i_linkcount--;
		parent_fragment = inode_fragment(directory);

		/* Handles the parent fragment condition. */
		if (parent_fragment == fragment) {
			encode_inode_locked(&group->parent_image.inode,
					    group->dinode);
		} else {
			error = prepare_inode_locked(&group->parent_image.inode,
						     group->parent_dinode,
						     &parent_fragment);

			/* Checks the operation status. */
			if (error != 0)
				return error;
			extents[2].target = parent_fragment
					    << ms->super.fsbtodb;
			extents[2].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
			extents[2].payload = group->parent_dinode;
			count++;
		}
	}

	/*
 * Publishes directory bytes and all changed dinodes in one durable
	 * transaction. */
	extents[0].target = info(directory)->direct[0] << ms->super.fsbtodb;
	extents[0].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[0].payload = group->directory;
	extents[1].target = fragment << ms->super.fsbtodb;
	extents[1].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[1].payload = group->dinode;
	error = metadata_group_commit(directory->i_mount, extents, count, NULL,
				      &group->outcome);

	/* Handles the group condition. */
	if (group->outcome.committed) {
		target->i_linkcount = group->image.inode.i_linkcount;

		/* Handles the removing directory condition. */
		if (removing_directory) {
			directory->i_linkcount =
				group->parent_image.inode.i_linkcount;
		}

		/* Handles the target condition. */
		if (target->i_linkcount == 0)
			target->i_flags |= INODE_DEAD;
	}

	/* Keeps the original errno even when recovery established removal. */
	return error;
}

/* Owns bounded private storage and error-path namespace cache publication. */
static int
remove_group(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target,
	int *handled)
{
	struct ufs_mount_state *ms = state(directory->i_mount);
	struct ufs_remove_group *group;
	size_t bytes;
	int error;

	/*
 * Declines only before admission, independently of callback error
	 * values. */
	*handled = 0;
	/* Handles the ms condition. */
	if (!ms->journal_enabled)
		return 0;

	/* Checks the name is dot result. */
	if (directory == target || name_is_dot(name)) {
		*handled = 1;
		/* Returns the computed result. */
		return EINVAL;
	}
	bytes = 2U * ms->super.bsize;

	/* Checks the inode fragment result. */
	if (target->i_type == INODE_DIR &&
	    inode_fragment(directory) != inode_fragment(target))
		bytes += ms->super.bsize;

	/* Handles the bytes condition. */
	if (bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)

		/* Reports successful completion. */
		return 0;
	*handled = 1;
	group = kern_calloc(1, sizeof(*group));

	/* Handles the group availability. */
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(bytes);

	/* Handles the memory availability. */
	if (group->memory == NULL) {
		kern_free(group);

		/* Returns the computed result. */
		return ENOMEM;
	}
	group->directory = group->memory;
	group->dinode = group->memory + ms->super.bsize;
	group->parent_dinode = group->dinode + ms->super.bsize;
	mutex_lock(&directory->i_lock);
	mutex_lock(&target->i_lock);
	mutex_lock(&ms->lock);
	error = remove_group_locked(directory, name, target, group);
	mutex_unlock(&ms->lock);
	mutex_unlock(&target->i_lock);
	mutex_unlock(&directory->i_lock);

	/*
 * Generic VFS invalidates only on success; uncertain/error outcomes
	 * need this. */
	if (error != 0 &&
	    (group->outcome.committed || group->outcome.uncertain)) {
		namecache_remove(directory, name);
		inode_dir_changed(directory);
	}
	kern_free(group->memory);
	kern_free(group);

	/*
 * Reports the original group outcome after releasing all transient
	 * ownership. */
	return error;
}

struct ufs_link_group {
	struct ufs_metadata_images images;
	struct ufs_inode_info directory_image;
	struct ufs_inode_info target_image;
	struct ufs_transaction_outcome outcome;
	uint8_t *memory;
	uint8_t *directory;
};

static int directory_image_insert(struct inode *directory, const struct componentname *name, struct inode *target, uint8_t *block);
static int link_group_locked(struct inode *directory, const struct componentname *name, struct inode *target, struct ufs_link_group *group);
static int link_group(struct inode *directory, const struct componentname *name, struct inode *target, int *handled);

/* Inserts into a private existing directory block, including reusable empty records. */
static int
directory_image_insert(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target,
	uint8_t *block)
{
	struct ufs_mount_state *ms = state(directory->i_mount);
	uint32_t pos;
	uint32_t at = UINT32_MAX;
	uint32_t number;
	uint16_t need;
	uint16_t length;
	uint16_t minimum;
	uint16_t available = 0;
	uint8_t namesize;
	unsigned n;

	/*
 * Bounds names and the single-block directory before modifying private
	 * bytes. */
	if (name->cn_namelen == 0 || name->cn_namelen > 255U)
		return EINVAL;
	/* Process each element required by the operation. */
	for (n = 0; n < name->cn_namelen; n++) {
		/* Validates the current name. */
		if (name->cn_nameptr[n] == '/')
			return EINVAL;
	}

	/* Handles the directory condition. */
	if (directory->i_size < 0 ||
	    (uint64_t)directory->i_size > ms->super.bsize ||
	    (uint64_t)directory->i_size % UFS_DIRBLKSIZ != 0)

		/* Returns the computed result. */
		return EIO;
	need = dir_minimum((uint8_t)name->cn_namelen);

	/*
 * Chooses the first available record while validating the complete
	 * directory. */
	pos = 0;
	/* Process each remaining element. */
	while (pos < (uint32_t)directory->i_size) {
		/* Handles the uint32 t condition. */
		if ((uint32_t)directory->i_size - pos < 8U ||
		    pos % UFS_DIRBLKSIZ > UFS_DIRBLKSIZ - 8U)

			/* Returns the computed result. */
			return EIO;
		number = drv_ufs_get32(block, pos, ms->super.swapped);
		length = drv_ufs_get16(block, pos + 4U, ms->super.swapped);
		namesize = block[pos + 7U];
		minimum = dir_minimum(namesize);

		/* Checks the current data length. */
		if (length < minimum || (length & 3U) != 0 ||
		    pos % UFS_DIRBLKSIZ + length > UFS_DIRBLKSIZ ||
		    length > (uint32_t)directory->i_size - pos)

			/* Returns the computed result. */
			return EIO;

		/* Handles the number condition. */
		if (number != 0 && namesize == name->cn_namelen &&
		    memcmp(block + pos + 8U, name->cn_nameptr, namesize) == 0)

			/* Returns the computed result. */
			return EEXIST;

		/* Handles the at condition. */
		if (at == UINT32_MAX) {
			/* Handles the number condition. */
			if (number == 0 && length >= need) {
				at = pos;
				available = length;
			} else if (length - minimum >= need) {
				at = pos + minimum;
				available = length - minimum;
			}
		}
		pos += length;
	}

	/* Handles the at condition. */
	if (at == UINT32_MAX) {
		/* Handles the uint64 t condition. */
		if ((uint64_t)directory->i_size + UFS_DIRBLKSIZ >
		    ms->super.bsize)

			/* Returns the computed result. */
			return ENOSPC;
		at = (uint32_t)directory->i_size;
		available = UFS_DIRBLKSIZ;
		directory->i_size += UFS_DIRBLKSIZ;
	} else {
		/*
 * Splits an occupied predecessor only after every record has
		 * been checked. */
		pos = 0;
		/* Continue while the operation condition remains true. */
		while (pos < at) {
			length = drv_ufs_get16(block, pos + 4U,
					       ms->super.swapped);

			/* Handles the pos condition. */
			if (pos + length > at) {
				drv_ufs_put16(block, pos + 4U,
					      (uint16_t)(at - pos),
					      ms->super.swapped);
				break;
			}
			pos += length;
		}
	}
	drv_ufs_put32(block, at, (uint32_t)target->i_ino, ms->super.swapped);
	drv_ufs_put16(block, at + 4U, available, ms->super.swapped);
	block[at + 6U] = dir_type(target->i_type);
	block[at + 7U] = (uint8_t)name->cn_namelen;
	memcpy(block + at + 8U, name->cn_nameptr, name->cn_namelen);

	/*
 * Returns a complete insertion with no persistent or live inode
	 * mutation. */
	return 0;
}

/* Merges shared dinodes and publishes a hard-link insertion as one group. */
static int
link_group_locked(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target,
	struct ufs_link_group *group)
{
	struct ufs_mount_state *ms = state(directory->i_mount);
	int error;

	/* Handles the ms condition. */
	if (!ms->writable)
		return EROFS;

	/* Handles the target condition. */
	if (target->i_linkcount == UINT16_MAX)
		return EMLINK;
	memcpy(&group->directory_image, info(directory),
	       sizeof(group->directory_image));
	memcpy(&group->target_image, info(target), sizeof(group->target_image));
	group->target_image.inode.i_linkcount++;
	error = metadata_image_get(&group->images, info(directory)->direct[0],
				   &group->directory);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = directory_image_insert(&group->directory_image.inode, name,
				       target, group->directory);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/*
 * Encodes all changed dinodes into unique, shared physical block
	 * images. */
	error = metadata_image_inode(&group->images,
				     &group->directory_image.inode);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = metadata_image_inode(&group->images,
				     &group->target_image.inode);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = metadata_group_commit(directory->i_mount, group->images.extents,
				      group->images.count, NULL,
				      &group->outcome);

	/* Handles the group condition. */
	if (group->outcome.committed) {
		directory->i_size = group->directory_image.inode.i_size;

		/*
 * The generic inode_link wrapper increments live nlink only on
		 * success. */
		if (error != 0) {
			target->i_linkcount =
				group->target_image.inode.i_linkcount;
		}
	}

	/* Returns the computed result. */
	return error;
}

/* Owns private hard-link preparation through its live namespace outcome. */
static int
link_group(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target,
	int *handled)
{
	struct ufs_mount_state *ms = state(directory->i_mount);
	struct ufs_link_group *group;
	size_t bytes;
	int error;

	*handled = 0;
	/* Checks the info result. */
	if (!ms->journal_enabled || info(directory)->direct[0] == 0)
		return 0;
	bytes = (inode_fragment(directory) == inode_fragment(target) ? 2U
								     : 3U) *
		ms->super.bsize;

	/* Handles the bytes condition. */
	if (bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)

		/* Reports successful completion. */
		return 0;
	*handled = 1;
	group = kern_calloc(1, sizeof(*group));

	/* Handles the group availability. */
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(bytes);

	/* Handles the memory availability. */
	if (group->memory == NULL) {
		kern_free(group);

		/* Returns the computed result. */
		return ENOMEM;
	}
	metadata_images_init(&group->images, directory->i_mount, group->memory,
			     bytes);
	mutex_lock(&directory->i_lock);
	mutex_lock(&target->i_lock);
	mutex_lock(&ms->lock);
	error = link_group_locked(directory, name, target, group);
	mutex_unlock(&ms->lock);
	mutex_unlock(&target->i_lock);
	mutex_unlock(&directory->i_lock);

	/* Checks the operation status. */
	if (error != 0 &&
	    (group->outcome.committed || group->outcome.uncertain)) {
		namecache_remove(directory, name);
		inode_dir_changed(directory);
	}
	kern_free(group->memory);
	kern_free(group);

	/* Returns the computed result. */
	return error;
}

struct ufs_rename_group {
	struct ufs_metadata_images images;
	struct ufs_transaction_outcome outcome;
	struct ufs_inode_info old_image;
	struct ufs_inode_info new_image;
	struct ufs_inode_info target_image;
	uint8_t *memory;
};

static int directory_image_change(struct inode *directory, uint8_t *block, const struct componentname *name, uint32_t expected, uint32_t replacement, uint8_t type);
static int rename_group_locked(struct inode *old_directory, const struct componentname *old_name, struct inode *new_directory, const struct componentname *new_name, struct inode *source, struct inode *target, struct ufs_rename_group *group);
static int rename_group(struct inode *old_directory, const struct componentname *old_name, struct inode *new_directory, const struct componentname *new_name, struct inode *source, struct inode *target, int *handled);

/* Validates a complete private directory before replacing or removing one name. */
static int
directory_image_change(
	struct inode *directory,
	uint8_t *block,
	const struct componentname *name,
	uint32_t expected,
	uint32_t replacement,
	uint8_t type)
{
	struct ufs_mount_state *ms;
	uint32_t pos;
	uint32_t previous;
	uint32_t found;
	uint32_t prior;
	uint32_t number;
	uint16_t length;
	uint8_t namesize;

	/* Bounds traversal before inspecting record bytes. */
	ms = state(directory->i_mount);

	/* Handles the directory condition. */
	if (directory->i_size < 0 ||
	    (uint64_t)directory->i_size > ms->super.bsize ||
	    (uint64_t)directory->i_size % UFS_DIRBLKSIZ != 0)

		/* Returns the computed result. */
		return EIO;
	pos = 0;
	previous = found = prior = UINT32_MAX;

	/*
 * Records one matching entry while rejecting malformed or duplicate
	 * records. */
	/* Process each remaining element. */
	while (pos < (uint32_t)directory->i_size) {
		/* Handles the uint32 t condition. */
		if ((uint32_t)directory->i_size - pos < 8U)
			return EIO;
		length = drv_ufs_get16(block, pos + 4U, ms->super.swapped);
		namesize = block[pos + 7U];

		/* Checks the dir minimum result. */
		if (length < dir_minimum(namesize) || (length & 3U) != 0 ||
		    pos % UFS_DIRBLKSIZ + length > UFS_DIRBLKSIZ ||
		    length > (uint32_t)directory->i_size - pos)

			/* Returns the computed result. */
			return EIO;
		number = drv_ufs_get32(block, pos, ms->super.swapped);

		/* Handles the number condition. */
		if (number != 0 && namesize == name->cn_namelen &&
		    memcmp(block + pos + 8U, name->cn_nameptr, namesize) == 0) {
			/* Handles the found condition. */
			if (found != UINT32_MAX || number != expected)
				return EIO;
			found = pos;
			prior = previous;
		}
		previous = pos;
		pos += length;
	}

	/* Refuses a stale lookup without modifying its private directory. */
	if (found == UINT32_MAX)
		return ENOENT;

	/*
 * Replaces the inode reference or joins a removed record to its
	 * predecessor. */
	if (replacement != 0) {
		drv_ufs_put32(block, found, replacement, ms->super.swapped);
		block[found + 6U] = type;
	} else if (prior != UINT32_MAX &&
		   prior / UFS_DIRBLKSIZ == found / UFS_DIRBLKSIZ) {
		length = drv_ufs_get16(block, found + 4U, ms->super.swapped);
		length += drv_ufs_get16(block, prior + 4U, ms->super.swapped);
		drv_ufs_put16(block, prior + 4U, length, ms->super.swapped);
	} else {
		drv_ufs_put32(block, found, 0, ms->super.swapped);
	}

	/*
 * Leaves every changed byte private until the enclosing group is
	 * committed. */
	return 0;
}

/* Prepares both names, directory ancestry and link accounting as one operation. */
static int
rename_group_locked(
	struct inode *old_directory,
	const struct componentname *old_name,
	struct inode *new_directory,
	const struct componentname *new_name,
	struct inode *source,
	struct inode *target,
	struct ufs_rename_group *group)
{
	static const struct componentname dotdot = {"..", 2, 0};
	struct ufs_mount_state *ms;
	struct inode *old_image;
	struct inode *new_image;
	uint8_t *old_block;
	uint8_t *new_block;
	uint8_t *child_block;
	int moving_directory;
	int error;

	/*
 * Copies only locked inode state and keeps a single image for identical
	 * parents. */
	ms = state(old_directory->i_mount);

	/* Handles the ms condition. */
	if (!ms->writable)
		return EROFS;
	memcpy(&group->old_image, info(old_directory),
	       sizeof(group->old_image));
	memcpy(&group->new_image, info(new_directory),
	       sizeof(group->new_image));
	old_image = &group->old_image.inode;
	new_image = old_directory == new_directory ? old_image
						   : &group->new_image.inode;
	moving_directory = source->i_type == INODE_DIR;

	/* Validates all link transitions before editing namespace bytes. */
	if (source->i_linkcount == 0 ||
	    (target != NULL && target->i_linkcount == 0))

		/* Returns the computed result. */
		return EIO;

	/* Handles the moving directory condition. */
	if (moving_directory && old_directory != new_directory) {
		/* Handles the old image condition. */
		if (old_image->i_linkcount == 0)
			return EIO;

		/* Handles the target availability. */
		if (target == NULL && new_image->i_linkcount == UINT16_MAX)
			return EMLINK;
		old_image->i_linkcount--;

		/* Handles the target availability. */
		if (target == NULL)
			new_image->i_linkcount++;
	} else if (moving_directory && target != NULL) {
		/* Handles the old image condition. */
		if (old_image->i_linkcount == 0)
			return EIO;
		old_image->i_linkcount--;
	}

	/* Handles the target availability. */
	if (target != NULL) {
		memcpy(&group->target_image, info(target),
		       sizeof(group->target_image));

		/* Handles the moving directory condition. */
		if (moving_directory)
			group->target_image.inode.i_linkcount = 0;
		else
			group->target_image.inode.i_linkcount--;
	}

	/*
 * Removes the old name first so a full same-parent directory can reuse
	 * its space. */
	error = metadata_image_get(&group->images,
				   info(old_directory)->direct[0], &old_block);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = directory_image_change(old_image, old_block, old_name,
				       (uint32_t)source->i_ino, 0, 0);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = metadata_image_get(&group->images,
				   info(new_directory)->direct[0], &new_block);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the target availability. */
	if (target != NULL) {
		error = directory_image_change(
			new_image, new_block, new_name, (uint32_t)target->i_ino,
			(uint32_t)source->i_ino, dir_type(source->i_type));
	} else {
		error = directory_image_insert(new_image, new_name, source,
					       new_block);
	}

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/*
 * Reparents a moved directory in the same transaction as its visible
	 * names. */
	if (moving_directory && old_directory != new_directory) {
		error = metadata_image_get(
			&group->images, info(source)->direct[0], &child_block);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		error = directory_image_change(source, child_block, &dotdot,
					       (uint32_t)old_directory->i_ino,
					       (uint32_t)new_directory->i_ino,
					       4);

		/* Checks the operation status. */
		if (error != 0)
			return error;
	}

	/*
 * Merges all changed dinodes without reloading shared physical blocks.
	 */
	error = metadata_image_inode(&group->images, old_image);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the new image condition. */
	if (new_image != old_image) {
		error = metadata_image_inode(&group->images, new_image);

		/* Checks the operation status. */
		if (error != 0)
			return error;
	}

	/* Handles the target availability. */
	if (target != NULL) {
		error = metadata_image_inode(&group->images,
					     &group->target_image.inode);

		/* Checks the operation status. */
		if (error != 0)
			return error;
	}
	error = metadata_group_commit(
		old_directory->i_mount, group->images.extents,
		group->images.count, NULL, &group->outcome);

	/*
 * Publishes only the live fields whose persistent images are proven
	 * committed. */
	if (group->outcome.committed) {
		old_directory->i_size = old_image->i_size;
		old_directory->i_linkcount = old_image->i_linkcount;
		new_directory->i_size = new_image->i_size;
		new_directory->i_linkcount = new_image->i_linkcount;

		/* Handles the target availability. */
		if (target != NULL) {
			target->i_linkcount =
				group->target_image.inode.i_linkcount;

			/* Handles the target condition. */
			if (target->i_linkcount == 0)
				target->i_flags |= INODE_DEAD;
		}
	}

	/*
 * Preserves an error even when recovery establishes that the rename
	 * committed. */
	return error;
}

/* Owns unique inode locks, exact image capacity and rename cache publication. */
static int
rename_group(
	struct inode *old_directory,
	const struct componentname *old_name,
	struct inode *new_directory,
	const struct componentname *new_name,
	struct inode *source,
	struct inode *target,
	int *handled)
{
	struct ufs_mount_state *ms;
	struct ufs_rename_group *group;
	struct inode *locks[4];
	uint64_t fragments[6];
	size_t bytes;
	unsigned count;
	unsigned unique;
	unsigned lock_count;
	unsigned n;
	unsigned j;
	int error;

	/*
 * Declines unsupported backing before admission and never falls back on
	 * errno. */
	ms = state(old_directory->i_mount);
	*handled = 0;
	/* Checks the info result. */
	if (!ms->journal_enabled || info(new_directory)->direct[0] == 0)
		return 0;
	*handled = 1;
	/* Handles the source condition. */
	if (source == old_directory || source == new_directory ||
	    target == old_directory || target == new_directory)

		/* Returns the computed result. */
		return EINVAL;

	/*
 * Counts distinct blocks so shared dinodes do not unnecessarily exhaust
	 * a slot. */
	fragments[0] = info(old_directory)->direct[0];
	fragments[1] = info(new_directory)->direct[0];
	fragments[2] = inode_fragment(old_directory);
	fragments[3] = inode_fragment(new_directory);
	count = 4;

	/* Handles the source condition. */
	if (source->i_type == INODE_DIR && old_directory != new_directory)
		fragments[count++] = info(source)->direct[0];

	/* Handles the target availability. */
	if (target != NULL)
		fragments[count++] = inode_fragment(target);
	unique = 0;

	/*
 * Deduplicates the bounded footprint before allocating any private
	 * image. */
	/* Process each remaining element. */
	for (n = 0; n < count; n++) {
		/* Process each element required by the operation. */
		for (j = 0; j < n; j++) {
			/* Handles the fragments condition. */
			if (fragments[j] == fragments[n])
				break;
		}

		/* Handles the j condition. */
		if (j == n)
			unique++;
	}
	bytes = unique * ms->super.bsize;

	/* Handles the ms condition. */
	if (ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
		*handled = 0;
		/* Reports successful completion. */
		return 0;
	}

	/* Allocates the operation and its exact private block storage. */
	group = kern_calloc(1, sizeof(*group));

	/* Handles the group availability. */
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(bytes);

	/* Handles the memory availability. */
	if (group->memory == NULL) {
		kern_free(group);

		/* Returns the computed result. */
		return ENOMEM;
	}
	metadata_images_init(&group->images, old_directory->i_mount,
			     group->memory, bytes);
	locks[0] = old_directory;
	lock_count = 1;

	/* Handles the new directory condition. */
	if (new_directory != old_directory)
		locks[lock_count++] = new_directory;
	locks[lock_count++] = source;

	/* Handles the target availability. */
	if (target != NULL && target != source)
		locks[lock_count++] = target;

	/*
 * Retains namespace exclusion from the caller while locking each inode
	 * once. */
	/* Process each remaining element. */
	for (n = 0; n < lock_count; n++)
		mutex_lock(&locks[n]->i_lock);
	mutex_lock(&ms->lock);
	error = rename_group_locked(old_directory, old_name, new_directory,
				    new_name, source, target, group);
	mutex_unlock(&ms->lock);

	/*
 * Releases inode ownership before cache publication, including error
	 * outcomes. */
	/* Process each remaining element. */
	while (lock_count != 0)
		mutex_unlock(&locks[--lock_count]->i_lock);

	/* Checks the operation status. */
	if (error != 0 &&
	    (group->outcome.committed || group->outcome.uncertain)) {
		namecache_remove(old_directory, old_name);
		namecache_remove(new_directory, new_name);
		inode_dir_changed(old_directory);

		/* Handles the new directory condition. */
		if (new_directory != old_directory)
			inode_dir_changed(new_directory);

		/* Handles the source condition. */
		if (source->i_type == INODE_DIR &&
		    old_directory != new_directory)
			inode_dir_changed(source);
	}
	kern_free(group->memory);
	kern_free(group);

	/*
 * Returns the original transaction result after releasing transient
	 * ownership. */
	return error;
}
/* End consolidated ufs-namespace.inc. */

/* Begin consolidated ufs-creation.inc. */
/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

static int creation_group_locked(struct inode *directory, const struct componentname *name, struct inode *target, struct ufs_link_group *group);
static int creation_group(struct inode *directory, const struct componentname *name, struct inode *target, struct ufs_transaction_outcome *outcome);
static int creation_publish(struct inode *directory, const struct componentname *name, struct inode *target, struct inode **result);

/* Publishes a prepared zero-link child and its parent name in one redo group. */
static int
creation_group_locked(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target,
	struct ufs_link_group *group)
{
	struct ufs_mount_state *ms;
	int error;
	int is_directory;

	/* Rejects stale identities before editing either private dinode. */
	ms = state(directory->i_mount);

	/* Handles the ms condition. */
	if (!ms->writable)
		return EROFS;

	/* Checks the inode type mode result. */
	if (directory->i_type != INODE_DIR || target->i_linkcount != 0 ||
	    target->i_ino <= UFS_ROOT_INO || target->i_ino > UINT32_MAX ||
	    inode_type_mode(target->i_type) == 0 ||
	    info(directory)->direct[0] == 0)

		/* Returns the computed result. */
		return EIO;
	is_directory = target->i_type == INODE_DIR;

	/* Handles the directory condition. */
	if (is_directory && directory->i_linkcount == UINT16_MAX)
		return EMLINK;

	/*
 * Keeps all prepared content while changing only the final link
	 * relationship. */
	memcpy(&group->directory_image, info(directory),
	       sizeof(group->directory_image));
	memcpy(&group->target_image, info(target), sizeof(group->target_image));
	group->target_image.inode.i_linkcount = is_directory ? 2 : 1;

	/* Handles the directory condition. */
	if (is_directory)
		group->directory_image.inode.i_linkcount++;
	error = metadata_image_get(&group->images, info(directory)->direct[0],
				   &group->directory);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = directory_image_insert(&group->directory_image.inode, name,
				       target, group->directory);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/*
 * Merges shared parent and child slots before issuing any home
	 * mutation. */
	error = metadata_image_inode(&group->images,
				     &group->directory_image.inode);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = metadata_image_inode(&group->images,
				     &group->target_image.inode);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = metadata_group_commit(directory->i_mount, group->images.extents,
				      group->images.count, NULL,
				      &group->outcome);

	/*
 * Reflects a proven publication even when its checkpoint returned an
	 * error. */
	if (group->outcome.committed) {
		directory->i_size = group->directory_image.inode.i_size;
		directory->i_linkcount =
			group->directory_image.inode.i_linkcount;
		target->i_linkcount = group->target_image.inode.i_linkcount;
	}

	/* Returns the original I/O result separately from durable ownership. */
	return error;
}

/* Owns initial parent backing and private final publication under namespace exclusion. */
static int
creation_group(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target,
	struct ufs_transaction_outcome *outcome)
{
	struct ufs_mount_state *ms;
	struct ufs_link_group *group;
	size_t bytes;
	int handled;
	int error;

	/*
 * Establishes an unambiguous unpublished outcome before allocating
	 * resources. */
	memset(outcome, 0, sizeof(*outcome));

	/* Handles the directory condition. */
	if (directory == target || directory->i_mount != target->i_mount)
		return EINVAL;
	ms = state(directory->i_mount);
	bytes = 3U * ms->super.bsize;

	/* Handles the ms condition. */
	if (!ms->journal_enabled || ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)

		/* Returns the computed result. */
		return EOPNOTSUPP;
	group = kern_calloc(1, sizeof(*group));

	/* Handles the group availability. */
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(bytes);

	/* Handles the memory availability. */
	if (group->memory == NULL) {
		kern_free(group);

		/* Returns the computed result. */
		return ENOMEM;
	}

	/*
 * Prepares recoverable empty parent backing before publishing the new
	 * name. */
	metadata_images_init(&group->images, directory->i_mount, group->memory,
			     bytes);
	mutex_lock(&directory->i_lock);
	error = 0;

	/* Checks the info result. */
	if (info(directory)->direct[0] == 0) {
		error = directory_backing_group(directory, &handled);

		/* Checks the operation status. */
		if (error == 0 && !handled)
			error = EOPNOTSUPP;
	}

	/* Checks the operation status. */
	if (error == 0) {
		mutex_lock(&target->i_lock);
		mutex_lock(&ms->lock);
		error = creation_group_locked(directory, name, target, group);
		mutex_unlock(&ms->lock);
		mutex_unlock(&target->i_lock);
	}
	mutex_unlock(&directory->i_lock);
	*outcome = group->outcome;

	/*
 * Invalidates stale names when the generic caller cannot report
	 * success. */
	if (error != 0 && (outcome->committed || outcome->uncertain)) {
		namecache_remove(directory, name);
		inode_dir_changed(directory);
	}
	kern_free(group->memory);
	kern_free(group);

	/*
 * Preserves the admitted error without falling back to independent
	 * writes. */
	return error;
}

/* Finishes all grouped creation kinds without discarding a possibly named child. */
static int
creation_publish(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target,
	struct inode **result)
{
	struct ufs_transaction_outcome outcome;
	int error;

	/* Returns ownership to the generic successful-creation wrapper. */
	*result = NULL;
	error = creation_group(directory, name, target, &outcome);

	/* Checks the operation status. */
	if (error == 0) {
		*result = target;
		/* Reports successful completion. */
		return 0;
	}

	/*
 * Leaves committed or unresolved names for ordinary lifetime and mount
	 * recovery. */
	if (outcome.committed || outcome.uncertain) {
		detach_new_socket_special(target);
		inode_release(target);

		/* Returns the computed result. */
		return error;
	}

	/*
 * Reclaims only a child whose name publication was definitely not
	 * admitted. */
	error = discard_new_inode_after_error(
		target, target->i_type == INODE_DIR, error);

	/* Returns the computed result. */
	return error;
}
static int ufs_create(struct inode *directory, const struct componentname *name, const struct inode_creation_request *request, struct inode **result);

/* End consolidated ufs-creation.inc. */
static int
ufs_create(
	struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request,
	struct inode **result)
{
	struct inode *existing, *inode;
	struct ufs_mount_state *ms = state(directory->i_mount);
	int error;

	*result = NULL;
	/* Handles the ms condition. */
	if (!ms->writable)
		return EROFS;
	mutex_lock(&ms->namespace_lock);

	/* Handles the ms condition. */
	if (!ms->writable) {
		error = EROFS;
		goto out;
	}
	error = ufs_lookup(directory, name, &existing);

	/* Checks the operation status. */
	if (error == 0) {
		inode_release(existing);
		error = EEXIST;
		goto out;
	}

	/* Checks the operation status. */
	if (error != ENOENT)
		goto out;
	error = new_inode(directory, request, 1, &inode);

	/* Checks the operation status. */
	if (error)
		goto out;

	/* Handles the inode condition. */
	if (inode->i_linkcount == 0) {
		error = creation_publish(directory, name, inode, result);
		goto out;
	}
	error = dir_add(directory, name, (uint32_t)inode->i_ino, 8);

	/* Checks the operation status. */
	if (error) {
		error = discard_new_inode_after_error(inode, 0, error);
		goto out;
	}
	*result = inode;
out:
	mutex_unlock(&ms->namespace_lock);

	/* Returns the computed result. */
	return error;
}

static int ufs_mkdir(struct inode *directory, const struct componentname *name, const struct inode_creation_request *request, struct inode **result);

/* Supports the ufs mkdir operation. */
static int
ufs_mkdir(
	struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request,
	struct inode **result)
{
	int cleanup;
	int name_removed;
	struct inode *existing, *inode;
	struct componentname dot = {".", 1, 0}, dotdot = {"..", 2, 0};
	struct ufs_mount_state *ms = state(directory->i_mount);
	uint32_t removed = 0;
	nlink_t old_directory_links;
	int error, rollback_error;

	*result = NULL;
	/* Handles the ms condition. */
	if (!ms->writable)
		return EROFS;
	mutex_lock(&ms->namespace_lock);

	/* Handles the ms condition. */
	if (!ms->writable) {
		error = EROFS;
		goto out;
	}
	error = ufs_lookup(directory, name, &existing);

	/* Checks the operation status. */
	if (error == 0) {
		inode_release(existing);
		error = EEXIST;
		goto out;
	}

	/* Checks the operation status. */
	if (error != ENOENT)
		goto out;
	error = new_inode(directory, request, 2, &inode);

	/* Checks the operation status. */
	if (error)
		goto out;
	error = dir_add(inode, &dot, (uint32_t)inode->i_ino, 4);

	/* Checks the operation status. */
	if (error == 0)
		error = dir_add(inode, &dotdot, (uint32_t)directory->i_ino, 4);

	/* Checks the operation status. */
	if (error == 0 && inode->i_linkcount == 0) {
		error = creation_publish(directory, name, inode, result);
		goto out;
	}

	/* Checks the operation status. */
	if (error == 0)
		error = dir_add(directory, name, (uint32_t)inode->i_ino, 4);

	/* Checks the operation status. */
	if (error) {
		error = discard_new_inode_after_error(inode, 1, error);
		goto out;
	}
	mutex_lock(&directory->i_lock);
	old_directory_links = directory->i_linkcount;
	directory->i_linkcount++;
	error = persist_inode(directory);
	mutex_unlock(&directory->i_lock);

	/* Checks the operation status. */
	if (error == 0) {
		*result = inode;
	} else {
		name_removed = 0;

		rollback_error = dir_remove(directory, name, &removed);

		/* Checks the operation status. */
		if (rollback_error == 0) {
			name_removed = 1;

			/* Handles the removed condition. */
			if (removed != (uint32_t)inode->i_ino)
				rollback_error = EIO;
			mutex_lock(&directory->i_lock);
			directory->i_linkcount = old_directory_links;

			/* Checks the operation status. */
			if (rollback_error == 0)
				rollback_error = persist_inode(directory);
			mutex_unlock(&directory->i_lock);
		}

		/* Handles the name removed condition. */
		if (!name_removed) {
			ms->writable = 0;
			inode_release(inode);
		} else {
			cleanup = discard_new_inode(inode, 1);

			/* Checks the operation status. */
			if (rollback_error == 0 && cleanup != 0)
				rollback_error = cleanup;

			/* Checks the operation status. */
			if (rollback_error != 0)
				ms->writable = 0;
		}

		/* Checks the operation status. */
		if (rollback_error != 0)
			error = rollback_error;
	}
out:
	mutex_unlock(&ms->namespace_lock);

	/* Returns the computed result. */
	return error;
}

static int ufs_mknod(struct inode *directory, const struct componentname *name, const struct inode_creation_request *request, struct inode **result);

/* Supports the ufs mknod operation. */
static int
ufs_mknod(
	struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request,
	struct inode **result)
{
	struct inode *existing, *inode;
	struct ufs_mount_state *ms = state(directory->i_mount);
	int error;

	/* Handles the request availability. */
	if (request == NULL ||
	    (request->type != INODE_FIFO && request->type != INODE_SOCKET &&
	     request->type != INODE_CHAR && request->type != INODE_BLOCK))

		/* Returns the computed result. */
		return EOPNOTSUPP;
	*result = NULL;
	/* Handles the ms condition. */
	if (!ms->writable)
		return EROFS;
	mutex_lock(&ms->namespace_lock);

	/* Handles the ms condition. */
	if (!ms->writable) {
		error = EROFS;
		goto out;
	}
	error = ufs_lookup(directory, name, &existing);

	/* Checks the operation status. */
	if (error == 0) {
		inode_release(existing);
		error = EEXIST;
		goto out;
	}

	/* Checks the operation status. */
	if (error != ENOENT)
		goto out;
	error = new_inode(directory, request, 1, &inode);

	/* Checks the operation status. */
	if (error != 0)
		goto out;

	/* Handles the inode condition. */
	if (inode->i_linkcount == 0) {
		error = creation_publish(directory, name, inode, result);
		goto out;
	}
	error = dir_add(directory, name, (uint32_t)inode->i_ino,
			dir_type(request->type));

	/* Checks the operation status. */
	if (error != 0) {
		error = discard_new_inode_after_error(inode, 0, error);
		goto out;
	}
	*result = inode;
out:
	mutex_unlock(&ms->namespace_lock);

	/* Returns the computed result. */
	return error;
}

static int ufs_unlink(struct inode *directory, const struct componentname *name);

/* Supports the ufs unlink operation. */
static int
ufs_unlink(
	struct inode *directory,
	const struct componentname *name)
{
	struct ufs_mount_state *ms = state(directory->i_mount);
	struct inode *target = NULL;
	uint32_t number = 0;
	int error, rollback_error;
	int removed = 0;
	int handled;
	nlink_t old_links = 0;
	unsigned old_flags = 0;

	mutex_lock(&ms->namespace_lock);

	/* Handles the ms condition. */
	if (!ms->writable) {
		error = EROFS;
		goto out;
	}
	error = ufs_lookup(directory, name, &target);

	/* Checks the operation status. */
	if (error)
		goto out;

	/* Handles the target condition. */
	if (target->i_type == INODE_DIR) {
		error = EISDIR;
		goto out;
	}
	error = remove_group(directory, name, target, &handled);

	/* Handles the handled condition. */
	if (handled)
		goto out;
	old_links = target->i_linkcount;
	old_flags = target->i_flags;
	error = dir_remove(directory, name, &number);

	/* Checks the operation status. */
	if (error == 0) {
		removed = 1;
		mutex_lock(&target->i_lock);

		/* Handles the target condition. */
		if (target->i_linkcount == 0) {
			error = EIO;
		} else {
			target->i_linkcount--;
			error = persist_inode(target);

			/* Handles the target condition. */
			if (target->i_linkcount == 0)
				target->i_flags |= INODE_DEAD;
		}
		mutex_unlock(&target->i_lock);
	}

	/* Checks the operation status. */
	if (error != 0 && removed) {
		mutex_lock(&target->i_lock);
		target->i_linkcount = old_links;
		target->i_flags = old_flags;
		rollback_error = persist_inode(target);
		mutex_unlock(&target->i_lock);

		/* Checks the operation status. */
		if (rollback_error == 0) {
			rollback_error = dir_add(directory, name, number,
						 dir_type(target->i_type));
		}

		/* Checks the operation status. */
		if (rollback_error != 0)
			ms->writable = 0;
	}
out:
	inode_release(target);
	mutex_unlock(&ms->namespace_lock);

	/* Returns the computed result. */
	return error;
}

static int directory_empty(struct inode *directory);

/* Supports the directory empty operation. */
static int
directory_empty(
	struct inode *directory)
{
	off_t cursor = 0;
	uint32_t number;
	uint8_t type;
	char name[NAME_MAX + 1U];
	int error;

	/* Process each linked entry. */
	while ((error = next_dirent(directory, &cursor, &number, &type,
				    name)) == 0) {
		/* Selects the matching value. */
		if (strcmp(name, ".") && strcmp(name, ".."))
			return 0;
	}

	/* Returns the computed result. */
	return error == ENOENT ? 1 : -error;
}

static int ufs_rmdir(struct inode *directory, const struct componentname *name);

/* Supports the ufs rmdir operation. */
static int
ufs_rmdir(
	struct inode *directory,
	const struct componentname *name)
{
	struct ufs_mount_state *ms = state(directory->i_mount);
	struct inode *target = NULL;
	uint32_t number = 0;
	int empty, error, rollback_error;
	int removed = 0;
	int handled;
	nlink_t old_target_links = 0, old_directory_links = 0;
	unsigned old_target_flags = 0;

	/* Handles the name is dot condition. */
	if (name_is_dot(name))
		return EINVAL;
	mutex_lock(&ms->namespace_lock);

	/* Handles the ms condition. */
	if (!ms->writable) {
		error = EROFS;
		goto out;
	}
	error = ufs_lookup(directory, name, &target);

	/* Checks the operation status. */
	if (error)
		goto out;

	/* Handles the target condition. */
	if (target->i_type != INODE_DIR) {
		error = ENOTDIR;
		goto out;
	}
	empty = directory_empty(target);

	/* Handles the empty condition. */
	if (empty <= 0) {
		error = empty == 0 ? ENOTEMPTY : -empty;
		goto out;
	}
	error = remove_group(directory, name, target, &handled);

	/* Handles the handled condition. */
	if (handled)
		goto out;
	old_target_links = target->i_linkcount;
	old_target_flags = target->i_flags;
	old_directory_links = directory->i_linkcount;
	error = dir_remove(directory, name, &number);

	/* Checks the operation status. */
	if (error == 0) {
		removed = 1;
		mutex_lock(&target->i_lock);
		target->i_linkcount = 0;
		target->i_flags |= INODE_DEAD;
		error = persist_inode(target);
		mutex_unlock(&target->i_lock);
		mutex_lock(&directory->i_lock);

		/* Handles the directory condition. */
		if (directory->i_linkcount > 0)
			directory->i_linkcount--;

		/* Checks the operation status. */
		if (error == 0)
			error = persist_inode(directory);
		mutex_unlock(&directory->i_lock);
	}

	/* Checks the operation status. */
	if (error != 0 && removed) {
		mutex_lock(&target->i_lock);
		target->i_linkcount = old_target_links;
		target->i_flags = old_target_flags;
		rollback_error = persist_inode(target);
		mutex_unlock(&target->i_lock);
		mutex_lock(&directory->i_lock);
		directory->i_linkcount = old_directory_links;

		/* Checks the operation status. */
		if (rollback_error == 0)
			rollback_error = persist_inode(directory);
		mutex_unlock(&directory->i_lock);

		/* Checks the operation status. */
		if (rollback_error == 0)
			rollback_error = dir_add(directory, name, number, 4);

		/* Checks the operation status. */
		if (rollback_error != 0)
			ms->writable = 0;
	}
out:
	inode_release(target);
	mutex_unlock(&ms->namespace_lock);

	/* Returns the computed result. */
	return error;
}

static int ufs_rename(struct inode *old_directory, const struct componentname *old_name, struct inode *new_directory, const struct componentname *new_name, unsigned flags);

/* Supports the ufs rename operation. */
static int
ufs_rename(
	struct inode *old_directory,
	const struct componentname *old_name,
	struct inode *new_directory,
	const struct componentname *new_name,
	unsigned flags)
{
	uint32_t ignored_local;
	uint8_t ignored_type_local;
	uint32_t ignored_local1;
	static const struct componentname dotdot_local = {"..", 2, 0};
	static const struct componentname dotdot_local2 = {"..", 2, 0};
	uint32_t ignored_local3;
	uint8_t ignored_type_local4;
	uint32_t old_parent;
	uint8_t old_parent_type;
	struct ufs_mount_state *ms = state(old_directory->i_mount);
	struct inode *source = NULL, *target = NULL;
	uint32_t removed = 0, replaced = 0;
	uint8_t replaced_type = 0;
	nlink_t old_target_links = 0, old_old_directory_links = 0;
	nlink_t old_new_directory_links = 0;
	unsigned old_target_flags = 0;
	int target_exists = 0, namespace_committed = 0, dotdot_changed = 0;
	int empty, error, rollback_error = 0;
	int handled;

	/* Checks the active flags. */
	if (flags != 0)
		return EINVAL;

	/* Handles the old directory condition. */
	if (old_directory->i_mount != new_directory->i_mount)
		return EXDEV;

	/* Handles the name is dot condition. */
	if (name_is_dot(old_name) || name_is_dot(new_name))
		return EINVAL;

	/* Handles the old directory condition. */
	if (old_directory == new_directory &&
	    old_name->cn_namelen == new_name->cn_namelen &&
	    memcmp(old_name->cn_nameptr, new_name->cn_nameptr,
		   old_name->cn_namelen) == 0)

		/* Reports successful completion. */
		return 0;

	mutex_lock(&ms->namespace_lock);

	/* Handles the ms condition. */
	if (!ms->writable) {
		error = EROFS;
		goto out;
	}
	error = ufs_lookup(old_directory, old_name, &source);

	/* Checks the operation status. */
	if (error != 0)
		goto out;
	error = ufs_lookup(new_directory, new_name, &target);

	/* Checks the operation status. */
	if (error == 0) {
		target_exists = 1;

		/* Handles the target condition. */
		if (target->i_ino == source->i_ino) {
			error = 0;
			goto out;
		}

		/* Handles the source condition. */
		if (source->i_type == INODE_DIR &&
		    target->i_type != INODE_DIR) {
			error = ENOTDIR;
			goto out;
		}

		/* Handles the source condition. */
		if (source->i_type != INODE_DIR &&
		    target->i_type == INODE_DIR) {
			error = EISDIR;
			goto out;
		}

		/* Handles the target condition. */
		if (target->i_type == INODE_DIR) {
			empty = directory_empty(target);

			/* Handles the empty condition. */
			if (empty <= 0) {
				error = empty == 0 ? ENOTEMPTY : -empty;
				goto out;
			}
		}
	} else if (error == ENOENT) {
		error = 0;
	} else {
		goto out;
	}
	error = rename_group(old_directory, old_name, new_directory, new_name,
			     source, target, &handled);

	/* Handles the handled condition. */
	if (handled)
		goto out;
	old_old_directory_links = old_directory->i_linkcount;
	old_new_directory_links = new_directory->i_linkcount;

	/* Handles the target exists condition. */
	if (target_exists) {
		old_target_links = target->i_linkcount;
		old_target_flags = target->i_flags;
	}

	/* Handles the target exists condition. */
	if (target_exists) {
		error = dir_replace(
			new_directory, new_name, (uint32_t)source->i_ino,
			dir_type(source->i_type), &replaced, &replaced_type);
	} else {
		error = dir_add(new_directory, new_name,
				(uint32_t)source->i_ino,
				dir_type(source->i_type));
	}

	/* Checks the operation status. */
	if (error != 0)
		goto out;
	error = dir_remove(old_directory, old_name, &removed);

	/* Checks the operation status. */
	if (error != 0) {
		/* Handles the target exists condition. */
		if (target_exists) {
			(void)dir_replace(new_directory, new_name, replaced,
					  replaced_type, &ignored_local,
					  &ignored_type_local);
		} else {
			(void)dir_remove(new_directory, new_name,
					 &ignored_local1);
		}
		goto out;
	}

	/* Handles the removed condition. */
	if (removed != (uint32_t)source->i_ino) {
		error = EIO;
		goto out;
	}
	namespace_committed = 1;

	/* Handles the source condition. */
	if (source->i_type == INODE_DIR && old_directory != new_directory) {
		error = dir_replace(source, &dotdot_local,
				    (uint32_t)new_directory->i_ino, 4,
				    &old_parent, &old_parent_type);

		/* Checks the operation status. */
		if (error != 0)
			goto out;
		dotdot_changed = 1;
		(void)old_parent;
		(void)old_parent_type;
	}

	/* Handles the target exists condition. */
	if (target_exists) {
		mutex_lock(&target->i_lock);

		/* Handles the target condition. */
		if (target->i_type == INODE_DIR)
			target->i_linkcount = 0;
		else if (target->i_linkcount != 0)
			target->i_linkcount--;
		else
			error = EIO;

		/* Checks the operation status. */
		if (error == 0)
			error = persist_inode(target);

		/* Handles the target condition. */
		if (target->i_linkcount == 0)
			target->i_flags |= INODE_DEAD;
		mutex_unlock(&target->i_lock);
	}

	/* Checks the operation status. */
	if (error == 0 && source->i_type == INODE_DIR) {
		/* Handles the old directory condition. */
		if (old_directory != new_directory) {
			mutex_lock(&old_directory->i_lock);

			/* Handles the old directory condition. */
			if (old_directory->i_linkcount != 0)
				old_directory->i_linkcount--;
			error = persist_inode(old_directory);
			mutex_unlock(&old_directory->i_lock);

			/* Checks the operation status. */
			if (error == 0) {
				mutex_lock(&new_directory->i_lock);
				new_directory->i_linkcount++;

				/* Handles the target exists condition. */
				if (target_exists &&
				    target->i_type == INODE_DIR &&
				    new_directory->i_linkcount != 0)
					new_directory->i_linkcount--;
				error = persist_inode(new_directory);
				mutex_unlock(&new_directory->i_lock);
			}
		} else if (target_exists && target->i_type == INODE_DIR) {
			mutex_lock(&old_directory->i_lock);

			/* Handles the old directory condition. */
			if (old_directory->i_linkcount != 0)
				old_directory->i_linkcount--;
			error = persist_inode(old_directory);
			mutex_unlock(&old_directory->i_lock);
		}
	}
out:

	/* Checks the operation status. */
	if (error != 0 && namespace_committed) {
		/* Checks the dir replace result. */
		if (dotdot_changed &&
		    dir_replace(source, &dotdot_local2,
				(uint32_t)old_directory->i_ino, 4,
				&ignored_local3, &ignored_type_local4) != 0)
			rollback_error = EIO;

		/* Handles the target exists condition. */
		if (target_exists) {
			/* Checks the dir replace result. */
			if (dir_replace(new_directory, new_name,
					(uint32_t)target->i_ino,
					dir_type(target->i_type),
					&ignored_local3,
					&ignored_type_local4) != 0)
				rollback_error = EIO;
		} else if (dir_remove(new_directory, new_name,
				      &ignored_local3) != 0) {
			rollback_error = EIO;
		}

		/* Checks the dir add result. */
		if (dir_add(old_directory, old_name, (uint32_t)source->i_ino,
			    dir_type(source->i_type)) != 0)
			rollback_error = EIO;

		/* Handles the target exists condition. */
		if (target_exists) {
			mutex_lock(&target->i_lock);
			target->i_linkcount = old_target_links;
			target->i_flags = old_target_flags;

			/* Checks the persist inode result. */
			if (persist_inode(target) != 0)
				rollback_error = EIO;
			mutex_unlock(&target->i_lock);
		}
		mutex_lock(&old_directory->i_lock);
		old_directory->i_linkcount = old_old_directory_links;

		/* Checks the persist inode result. */
		if (persist_inode(old_directory) != 0)
			rollback_error = EIO;
		mutex_unlock(&old_directory->i_lock);

		/* Handles the new directory condition. */
		if (new_directory != old_directory) {
			mutex_lock(&new_directory->i_lock);
			new_directory->i_linkcount = old_new_directory_links;

			/* Checks the persist inode result. */
			if (persist_inode(new_directory) != 0)
				rollback_error = EIO;
			mutex_unlock(&new_directory->i_lock);
		}

		/* Checks the operation status. */
		if (rollback_error != 0)
			ms->writable = 0;
	}
	inode_release(target);
	inode_release(source);
	mutex_unlock(&ms->namespace_lock);

	/* Returns the computed result. */
	return error;
}

static int ufs_link(struct inode *directory, const struct componentname *name, struct inode *target);

/* Supports the ufs link operation. */
static int
ufs_link(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target)
{
	struct ufs_mount_state *ms = state(directory->i_mount);
	struct inode *existing;
	uint32_t removed;
	int error, rollback_error;
	int handled;

	/* Handles the target availability. */
	if (target == NULL || target->i_mount != directory->i_mount)
		return EXDEV;

	/* Handles the target condition. */
	if (target->i_type == INODE_DIR)
		return EPERM;
	mutex_lock(&ms->namespace_lock);

	/* Handles the ms condition. */
	if (!ms->writable) {
		error = EROFS;
		goto out;
	}
	mutex_lock(&target->i_lock);

	/* Handles the target condition. */
	if (target->i_linkcount == UINT16_MAX) {
		mutex_unlock(&target->i_lock);
		error = EMLINK;
		goto out;
	}
	mutex_unlock(&target->i_lock);
	error = ufs_lookup(directory, name, &existing);

	/* Checks the operation status. */
	if (error == 0) {
		inode_release(existing);
		error = EEXIST;
		goto out;
	}

	/* Checks the operation status. */
	if (error != ENOENT)
		goto out;
	error = link_group(directory, name, target, &handled);

	/* Handles the handled condition. */
	if (handled)
		goto out;
	error = dir_add(directory, name, (uint32_t)target->i_ino,
			dir_type(target->i_type));

	/* Checks the operation status. */
	if (error == 0) {
		/*
 * inode_link() applies the in-memory increment after this
		 * callback. */
		mutex_lock(&target->i_lock);
		target->i_linkcount++;
		error = persist_inode(target);
		target->i_linkcount--;
		mutex_unlock(&target->i_lock);

		/* Checks the operation status. */
		if (error != 0) {
			mutex_lock(&target->i_lock);
			rollback_error = persist_inode(target);
			mutex_unlock(&target->i_lock);

			/* Checks the operation status. */
			if (rollback_error == 0) {
				rollback_error =
					dir_remove(directory, name, &removed);
			}

			/* Checks the operation status. */
			if (rollback_error != 0)
				ms->writable = 0;
		}
	}
out:
	mutex_unlock(&ms->namespace_lock);

	/* Returns the computed result. */
	return error;
}

static int ufs_symlink(struct inode *directory, const struct componentname *name, const char *target, const struct inode_creation_request *request, struct inode **result);

/* Supports the ufs symlink operation. */
static int
ufs_symlink(
	struct inode *directory,
	const struct componentname *name,
	const char *target,
	const struct inode_creation_request *request,
	struct inode **result)
{
	struct ufs_mount_state *ms = state(directory->i_mount);
	struct inode *existing, *inode;
	size_t length = strlen(target);
	int error;

	/* Checks the state result. */
	if (length > state(directory->i_mount)->super.maxsymlinklen ||
	    length > 120U)

		/* Returns the computed result. */
		return ENAMETOOLONG;
	*result = NULL;
	mutex_lock(&ms->namespace_lock);

	/* Handles the ms condition. */
	if (!ms->writable) {
		error = EROFS;
		goto out;
	}
	error = ufs_lookup(directory, name, &existing);

	/* Checks the operation status. */
	if (error == 0) {
		inode_release(existing);
		error = EEXIST;
		goto out;
	}

	/* Checks the operation status. */
	if (error != ENOENT)
		goto out;
	error = new_inode(directory, request, 1, &inode);

	/* Checks the operation status. */
	if (error)
		goto out;
	inode->i_size = (off_t)length;
	memcpy(info(inode)->shortlink, target, length);
	error = persist_inode(inode);

	/* Checks the operation status. */
	if (error == 0 && inode->i_linkcount == 0) {
		error = creation_publish(directory, name, inode, result);
		goto out;
	}

	/* Checks the operation status. */
	if (error == 0)
		error = dir_add(directory, name, (uint32_t)inode->i_ino, 10);

	/* Checks the operation status. */
	if (error) {
		error = discard_new_inode_after_error(inode, 0, error);
		goto out;
	}
	*result = inode;
out:
	mutex_unlock(&ms->namespace_lock);

	/* Returns the computed result. */
	return error;
}

/* Supports the pwrite inode operation. */
static ssize_t
pwrite_inode(
	struct inode *inode,
	const void *buffer,
	size_t length,
	off_t offset)
{
	ssize_t function_result;

	/* Obtains the pwrite inode context result. */
	function_result =
		pwrite_inode_context(inode, buffer, length, offset, NULL);

	/* Returns the computed result. */
	return function_result;
}

static ssize_t ufs_read(struct file *file, void *buffer, size_t length);

/* Supports the ufs read operation. */
static ssize_t
ufs_read(
	struct file *file,
	void *buffer,
	size_t length)
{
	ssize_t n = pread_inode(file->f_inode, buffer, length, file->f_offset);

	/* Checks the current item count. */
	if (n > 0)
		file->f_offset += n;

	/* Returns the computed result. */
	return n;
}
static ssize_t ufs_pread(struct file *file, void *buffer, size_t length, off_t offset);

/* Supports the ufs pread operation. */
static ssize_t
ufs_pread(
	struct file *file,
	void *buffer,
	size_t length,
	off_t offset)
{
	ssize_t function_result;

	/* Obtains the pread inode result. */
	function_result = pread_inode(file->f_inode, buffer, length, offset);

	/* Returns the computed result. */
	return function_result;
}
static ssize_t ufs_write(struct file *file, const void *buffer, size_t length);

/* Supports the ufs write operation. */
static ssize_t
ufs_write(
	struct file *file,
	const void *buffer,
	size_t length)
{
	ssize_t n = pwrite_inode(file->f_inode, buffer, length, file->f_offset);

	/* Checks the current item count. */
	if (n > 0)
		file->f_offset += n;

	/* Returns the computed result. */
	return n;
}
static ssize_t ufs_pwrite(struct file *file, const void *buffer, size_t length, off_t offset);

/* Supports the ufs pwrite operation. */
static ssize_t
ufs_pwrite(
	struct file *file,
	const void *buffer,
	size_t length,
	off_t offset)
{
	ssize_t function_result;

	/* Obtains the pwrite inode result. */
	function_result = pwrite_inode(file->f_inode, buffer, length, offset);

	/* Returns the computed result. */
	return function_result;
}
/* Supports the ufs pwrite context operation. */
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
	ssize_t function_result;
	int error;

	(void)flags;
	(void)credential;
	error = io_context_validate(context);

	/* Checks the operation status. */
	if (error != 0)
		return -error;

	/* Obtains the pwrite inode context result. */
	function_result = pwrite_inode_context(file->f_inode, buffer, length,
					       offset, context);

	/* Returns the computed result. */
	return function_result;
}

static int ufs_readdir(struct file *file, struct dirent *entry, int *eof);

/* Supports the ufs readdir operation. */
static int
ufs_readdir(
	struct file *file,
	struct dirent *entry,
	int *eof)
{
	uint32_t number;
	uint8_t type;
	char name[NAME_MAX + 1U];
	int error = next_dirent(file->f_inode, &file->f_offset, &number, &type,
				name);

	/* Checks the operation status. */
	if (error == ENOENT) {
		*eof = 1;
		/* Reports successful completion. */
		return 0;
	}

	/* Checks the operation status. */
	if (error)
		return error;
	memset(entry, 0, sizeof(*entry));
	entry->d_ino = number;
	entry->d_type = type == 1    ? INODE_FIFO
			: type == 4  ? INODE_DIR
			: type == 8  ? INODE_REG
			: type == 10 ? INODE_SYMLINK
			: type == 12 ? INODE_SOCKET
				     : INODE_NONE;
	strcpy(entry->d_name, name);
	*eof = 0;
	/* Reports successful completion. */
	return 0;
}
static ssize_t ufs_readlink(struct inode *inode, char *buffer, size_t length);

/* Supports the ufs readlink operation. */
static ssize_t
ufs_readlink(
	struct inode *inode,
	char *buffer,
	size_t length)
{
	ssize_t function_result;
	size_t n;
	struct ufs_mount_state *ms = state(inode->i_mount);

	/* Handles the inode condition. */
	if (inode->i_type != INODE_SYMLINK)
		return -EINVAL;

	/* Handles the uint64 t condition. */
	if ((uint64_t)inode->i_size <= ms->super.maxsymlinklen &&
	    inode->i_size <= 120) {
		n = (size_t)inode->i_size;

		/* Checks the current item count. */
		if (n > length)
			n = length;
		memcpy(buffer, info(inode)->shortlink, n);

		/* Returns the computed result. */
		return (ssize_t)n;
	}

	/* Obtains the pread inode result. */
	function_result = pread_inode(inode, buffer, length, 0);

	/* Returns the computed result. */
	return function_result;
}

static size_t extattr_align(size_t value);

/* Supports the extattr align operation. */
static size_t
extattr_align(
	size_t value)
{
	/* Returns the computed result. */
	return (value + 7U) & ~(size_t)7U;
}

static int extattr_name(const char *name, uint8_t *name_space, const char **stored, size_t *stored_length);

/* Supports the extattr name operation. */
static int
extattr_name(
	const char *name,
	uint8_t *name_space,
	const char **stored,
	size_t *stored_length)
{
	const char *part;

	/* Handles the name availability. */
	if (name == NULL || name_space == NULL || stored == NULL ||
	    stored_length == NULL)

		/* Returns the computed result. */
		return EINVAL;

	/* Selects the matching prefix. */
	if (strncmp(name, "user.", 5) == 0) {
		*name_space = UFS_EXTATTR_NAMESPACE_USER;
		part = name + 5;
	} else if (strncmp(name, "system.", 7) == 0) {
		*name_space = UFS_EXTATTR_NAMESPACE_SYSTEM;
		part = name + 7;

		/* Selects the matching prefix. */
		if (strncmp(part, "security.", 9) == 0)
			return EINVAL;
	} else if (strncmp(name, "security.", 9) == 0) {
		*name_space = UFS_EXTATTR_NAMESPACE_SYSTEM;
		part = name;
	} else {
		/* Returns the computed result. */
		return EOPNOTSUPP;
	}
	*stored_length = strlen(part);
	/* Handles the stored length condition. */
	if (*stored_length == 0 || *stored_length > 255U)
		return EINVAL;
	*stored = part;
	/* Reports successful completion. */
	return 0;
}

static int extattr_load(struct inode *inode, uint8_t **result, size_t *length);

/* Supports the extattr load operation. */
static int
extattr_load(
	struct inode *inode,
	uint8_t **result,
	size_t *length)
{
	int error;
	uint32_t record;
	uint8_t name_length, padding;
	size_t base;
	struct ufs_inode_info *ui = info(inode);
	struct ufs_mount_state *ms = state(inode->i_mount);
	uint8_t *area;
	unsigned block_count, index;
	size_t offset = 0;

	/* Handles the result availability. */
	if (result == NULL || length == NULL)
		return EINVAL;
	*result = NULL;
	*length = ui->extattr_size;
	/* Handles the ui condition. */
	if (ui->extattr_size == 0)
		return 0;
	block_count =
		(ui->extattr_size + ms->super.bsize - 1U) / ms->super.bsize;

	/* Handles the block count condition. */
	if (block_count == 0 || block_count > UFS_NXADDR)
		return EIO;
	area = kern_calloc(block_count, ms->super.bsize);

	/* Handles the area availability. */
	if (area == NULL)
		return ENOMEM;
	/* Process each remaining element. */
	for (index = 0; index < block_count; index++) {
		error = read_block(inode->i_mount, ui->extattr[index],
				   area + index * ms->super.bsize);

		/* Checks the operation status. */
		if (error != 0) {
			kern_free(area);

			/* Returns the computed result. */
			return error;
		}
	}
	while (offset < ui->extattr_size) {
		/* Handles the ui condition. */
		if (ui->extattr_size - offset < UFS_EXTATTR_HEADER_SIZE)
			goto invalid;
		record = drv_ufs_get32(area, offset, ms->super.swapped);
		padding = area[offset + 5U];
		name_length = area[offset + 6U];
		base = extattr_align(UFS_EXTATTR_HEADER_SIZE + name_length);

		/* Handles the record condition. */
		if (record < base || (record & 7U) != 0 ||
		    record > ui->extattr_size - offset ||
		    padding > record - base ||
		    area[offset + 4U] < UFS_EXTATTR_NAMESPACE_USER ||
		    area[offset + 4U] > UFS_EXTATTR_NAMESPACE_SYSTEM)
			goto invalid;
		offset += record;
	}
	*result = area;
	/* Reports successful completion. */
	return 0;
invalid:
	kern_free(area);

	/* Returns the computed result. */
	return EIO;
}

static int extattr_find(struct inode *inode, const uint8_t *area, size_t area_length, uint8_t name_space, const char *name, size_t name_length, size_t *at, size_t *record_length, size_t *content_at, size_t *content_length);

/* Supports the extattr find operation. */
static int
extattr_find(
	struct inode *inode,
	const uint8_t *area,
	size_t area_length,
	uint8_t name_space,
	const char *name,
	size_t name_length,
	size_t *at,
	size_t *record_length,
	size_t *content_at,
	size_t *content_length)
{
	uint32_t record;
	uint8_t disk_name_length;
	size_t base;
	struct ufs_mount_state *ms = state(inode->i_mount);
	size_t offset = 0;

	/* Process each remaining element. */
	while (offset < area_length) {
		record = drv_ufs_get32(area, offset, ms->super.swapped);
		disk_name_length = area[offset + 6U];
		base = extattr_align(UFS_EXTATTR_HEADER_SIZE +
				     disk_name_length);

		/* Handles the area condition. */
		if (area[offset + 4U] == name_space &&
		    disk_name_length == name_length &&
		    memcmp(area + offset + UFS_EXTATTR_HEADER_SIZE, name,
			   name_length) == 0) {
			/* Handles the at availability. */
			if (at != NULL)
				*at = offset;
			/* Handles the record length availability. */
			if (record_length != NULL)
				*record_length = record;
			/* Handles the content at availability. */
			if (content_at != NULL)
				*content_at = offset + base;
			/* Handles the content length availability. */
			if (content_length != NULL) {
				*content_length =
					record - base - area[offset + 5U];
			}

			/* Reports successful completion. */
			return 0;
		}
		offset += record;
	}

	/* Returns the computed result. */
	return ENODATA;
}

static int extattr_publish(struct inode *inode, const uint8_t *area, size_t length);

/* Supports the extattr publish operation. */
static int
extattr_publish(
	struct inode *inode,
	const uint8_t *area,
	size_t length)
{
	struct ufs_inode_info *ui = info(inode);
	struct ufs_mount_state *ms = state(inode->i_mount);
	uint64_t old_ext[UFS_NXADDR], new_fragment = 0;
	uint64_t old_blocks;
	uint32_t old_size = ui->extattr_size;
	uint8_t *block = NULL, *old_area = NULL;
	size_t old_area_length = 0;
	unsigned old_count, index;
	int error = 0, rollback;
	int handled;

	/* Checks the current data length. */
	if (length > ms->super.bsize)
		return ENOSPC;
	old_ext[0] = ui->extattr[0];
	old_ext[1] = ui->extattr[1];
	old_blocks = ui->blocks;
	old_count = old_size == 0 ? 0U
				  : (old_size + ms->super.bsize - 1U) /
					    ms->super.bsize;

	/* Handles the uint64 t condition. */
	if ((uint64_t)old_count * (ms->super.bsize / UFS_SECTOR_SIZE) >
	    old_blocks)

		/* Returns the computed result. */
		return EIO;

	/* Handles the old size condition. */
	if (old_size != 0)
		error = extattr_load(inode, &old_area, &old_area_length);

	/* Checks the operation status. */
	if (error == 0 && old_area_length != old_size)
		error = EIO;

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the area availability. */
	if (area == NULL)
		length = 0;

	/* Checks the current data length. */
	if (length == 0) {
		error = xattr_release_group(inode, &handled);

		/* Handles the handled condition. */
		if (handled) {
			kern_free(old_area);

			/* Returns the computed result. */
			return error;
		}
		ui->extattr_size = 0;
		ui->extattr[0] = 0;
		ui->extattr[1] = 0;
		ui->blocks = old_blocks -
			     (uint64_t)old_count *
				     (ms->super.bsize / UFS_SECTOR_SIZE);
		error = persist_inode(inode);

		/* Checks the operation status. */
		if (error == 0)
			error = disk_sync(inode->i_mount->m_disk);

		/* Checks the operation status. */
		if (error != 0) {
			ui->extattr_size = old_size;
			ui->extattr[0] = old_ext[0];
			ui->extattr[1] = old_ext[1];
			ui->blocks = old_blocks;

			/* Checks the persist inode result. */
			if (persist_inode(inode) != 0)
				ms->writable = 0;
			kern_free(old_area);

			/* Returns the computed result. */
			return error;
		}
		/* Process each remaining element. */
		for (index = 0; index < old_count; index++) {
			/* Checks the free block result. */
			if ((rollback = free_block(inode->i_mount,
						   old_ext[index], inode->i_uid,
						   inode->i_gid)) != 0) {
				ms->writable = 0;
				error = rollback;
				break;
			}
		}
		kern_free(old_area);

		/* Returns the computed result. */
		return error;
	}
	error = xattr_existing_group(inode, area, length, &handled);

	/* Handles the handled condition. */
	if (handled) {
		kern_free(old_area);

		/* Returns the computed result. */
		return error;
	}

	/* Handles the ms condition. */
	if (ms->journal_enabled && old_size == 0) {
		error = xattr_allocate_group(inode, area, length, &handled);

		/* Handles the handled condition. */
		if (handled) {
			kern_free(old_area);

			/* Returns the computed result. */
			return error;
		}
	}
	block = kern_calloc(1, ms->super.bsize);

	/* Handles the block availability. */
	if (block == NULL) {
		kern_free(old_area);

		/* Returns the computed result. */
		return ENOMEM;
	}
	memcpy(block, area, length);

	/* Handles the old ext condition. */
	if (old_ext[0] == 0) {
		error = allocate_block(inode->i_mount, inode->i_uid,
				       inode->i_gid, &new_fragment);

		/* Checks the operation status. */
		if (error != 0)
			goto out;
	} else {
		new_fragment = old_ext[0];
	}
	error = write_block(inode->i_mount, new_fragment, block);

	/* Checks the operation status. */
	if (error != 0)
		goto rollback_data;
	ui->extattr_size = (uint32_t)length;
	ui->extattr[0] = new_fragment;
	ui->extattr[1] = 0;
	ui->blocks = old_blocks -
		     (uint64_t)old_count * (ms->super.bsize / UFS_SECTOR_SIZE) +
		     ms->super.bsize / UFS_SECTOR_SIZE;
	error = persist_inode(inode);

	/* Checks the operation status. */
	if (error == 0)
		error = disk_sync(inode->i_mount->m_disk);

	/* Checks the operation status. */
	if (error != 0)
		goto rollback_metadata;

	/* Checks the free block result. */
	if (old_count > 1U &&
	    (rollback = free_block(inode->i_mount, old_ext[1], inode->i_uid,
				   inode->i_gid)) != 0) {
		ms->writable = 0;
		error = rollback;
	}
	goto out;
rollback_metadata:
	ui->extattr_size = old_size;
	ui->extattr[0] = old_ext[0];
	ui->extattr[1] = old_ext[1];
	ui->blocks = old_blocks;
	rollback = persist_inode(inode);

	/* Handles the rollback condition. */
	if (rollback == 0)
		rollback = disk_sync(inode->i_mount->m_disk);

	/* Handles the rollback condition. */
	if (rollback != 0) {
		/*
 * The new pointer may still be committed: keep its allocation.
		 */
		ms->writable = 0;
		goto out;
	}
rollback_data:

	/* Handles the old ext condition. */
	if (old_ext[0] == 0) {
		/* Checks the free block result. */
		if (ms->writable && new_fragment != 0 &&
		    (rollback = free_block(inode->i_mount, new_fragment,
					   inode->i_uid, inode->i_gid)) != 0)
			ms->writable = 0;
	} else if (old_area != NULL &&
		   write_block(inode->i_mount, old_ext[0], old_area) != 0)
		ms->writable = 0;
out:
	kern_free(old_area);
	kern_free(block);

	/* Returns the computed result. */
	return error;
}

/* Supports the ufs getxattr operation. */
static ssize_t
ufs_getxattr(
	struct inode *inode,
	const char *name,
	void *value,
	size_t size)
{
	uint8_t name_space, *area = NULL;
	const char *stored;
	size_t stored_length;
	size_t area_length, content_at, content_length;
	int error;

	error = extattr_name(name, &name_space, &stored, &stored_length);

	/* Checks the operation status. */
	if (error != 0)
		return -error;
	mutex_lock(&inode->i_lock);
	error = extattr_load(inode, &area, &area_length);

	/* Checks the operation status. */
	if (error == 0) {
		error = extattr_find(inode, area, area_length, name_space,
				     stored, stored_length, NULL, NULL,
				     &content_at, &content_length);
	}

	/* Checks the operation status. */
	if (error == 0 && value != NULL && size < content_length)
		error = ERANGE;

	/* Checks the operation status. */
	if (error == 0 && value != NULL && content_length != 0)
		memcpy(value, area + content_at, content_length);
	mutex_unlock(&inode->i_lock);
	kern_free(area);

	/* Returns the computed result. */
	return error != 0 ? -(ssize_t)error : (ssize_t)content_length;
}

/* Supports the ufs setxattr operation. */
static int
ufs_setxattr(
	struct inode *inode,
	const char *name,
	const void *value,
	size_t size,
	unsigned flags)
{
	struct ufs_mount_state *ms = state(inode->i_mount);
	uint8_t name_space, *area = NULL, *updated = NULL;
	const char *stored;
	size_t stored_length, area_length = 0, at = 0, old_record = 0;
	size_t base, new_record, new_length, padding;
	int found, error;

	/* Handles the ms condition. */
	if (!ms->writable)
		return EROFS;

	/* Handles the value availability. */
	if (value == NULL && size != 0)
		return EINVAL;
	error = extattr_name(name, &name_space, &stored, &stored_length);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	base = extattr_align(UFS_EXTATTR_HEADER_SIZE + stored_length);

	/* Checks the current data size. */
	if (size > ms->super.bsize || base > ms->super.bsize - size)
		return E2BIG;
	new_record = extattr_align(base + size);
	padding = new_record - base - size;
	mutex_lock(&inode->i_lock);
	error = extattr_load(inode, &area, &area_length);

	/* Checks the operation status. */
	if (error != 0)
		goto out;
	found = extattr_find(inode, area, area_length, name_space, stored,
			     stored_length, &at, &old_record, NULL, NULL) == 0;

	/* Checks the active flags. */
	if ((flags & INODE_XATTR_CREATE) != 0 && found) {
		error = EEXIST;
		goto out;
	}

	/* Checks the active flags. */
	if ((flags & INODE_XATTR_REPLACE) != 0 && !found) {
		error = ENODATA;
		goto out;
	}
	new_length = area_length - (found ? old_record : 0U) + new_record;

	/* Handles the new length condition. */
	if (new_length > ms->super.bsize) {
		error = ENOSPC;
		goto out;
	}
	updated = kern_calloc(1, ms->super.bsize);

	/* Handles the updated availability. */
	if (updated == NULL) {
		error = ENOMEM;
		goto out;
	}

	/* Handles the at condition. */
	if (at != 0)
		memcpy(updated, area, at);
	drv_ufs_put32(updated, at, (uint32_t)new_record, ms->super.swapped);
	updated[at + 4U] = name_space;
	updated[at + 5U] = (uint8_t)padding;
	updated[at + 6U] = (uint8_t)stored_length;
	memcpy(updated + at + UFS_EXTATTR_HEADER_SIZE, stored, stored_length);

	/* Checks the current data size. */
	if (size != 0)
		memcpy(updated + at + base, value, size);

	/* Handles the area length condition. */
	if (area_length > at + (found ? old_record : 0U)) {
		memcpy(updated + at + new_record,
		       area + at + (found ? old_record : 0U),
		       area_length - at - (found ? old_record : 0U));
	}
	error = extattr_publish(inode, updated, new_length);
out:
	mutex_unlock(&inode->i_lock);
	kern_free(updated);
	kern_free(area);

	/* Returns the computed result. */
	return error;
}

static ssize_t ufs_listxattr(struct inode *inode, char *list, size_t size);

/* Supports the ufs listxattr operation. */
static ssize_t
ufs_listxattr(
	struct inode *inode,
	char *list,
	size_t size)
{
	uint32_t record;
	uint8_t ns, nlen;
	const char *prefix;
	const uint8_t *disk_name;
	size_t prefix_length;
	struct ufs_mount_state *ms = state(inode->i_mount);
	uint8_t *area = NULL;
	size_t area_length, offset = 0, needed = 0;
	int error;

	mutex_lock(&inode->i_lock);
	error = extattr_load(inode, &area, &area_length);

	/* Checks the operation status. */
	if (error != 0)
		goto out;
	/* Process each remaining element. */
	while (offset < area_length) {
		record = drv_ufs_get32(area, offset, ms->super.swapped);
		ns = area[offset + 4U];
		nlen = area[offset + 6U];

		disk_name = area + offset + UFS_EXTATTR_HEADER_SIZE;

		/* Handles the ns condition. */
		if (ns == UFS_EXTATTR_NAMESPACE_USER) {
			prefix = "user.";
			prefix_length = 5U;
		} else if (nlen >= 9U &&
			   memcmp(disk_name, "security.", 9) == 0) {
			prefix = "";
			prefix_length = 0;
		} else {
			prefix = "system.";
			prefix_length = 7U;
		}

		/* Handles the list availability. */
		if (list != NULL &&
		    (needed > size ||
		     prefix_length + nlen + 1U > size - needed)) {
			error = ERANGE;
			goto out;
		}

		/* Handles the list availability. */
		if (list != NULL) {
			memcpy(list + needed, prefix, prefix_length);
			memcpy(list + needed + prefix_length, disk_name, nlen);
			list[needed + prefix_length + nlen] = '\0';
		}
		needed += prefix_length + nlen + 1U;
		offset += record;
	}
out:
	mutex_unlock(&inode->i_lock);
	kern_free(area);

	/* Returns the computed result. */
	return error != 0 ? -(ssize_t)error : (ssize_t)needed;
}

static int ufs_removexattr(struct inode *inode, const char *name);

/* Supports the ufs removexattr operation. */
static int
ufs_removexattr(
	struct inode *inode,
	const char *name)
{
	struct ufs_mount_state *ms = state(inode->i_mount);
	uint8_t name_space, *area = NULL, *updated = NULL;
	const char *stored;
	size_t stored_length, area_length, at, record, new_length;
	int error;

	/* Handles the ms condition. */
	if (!ms->writable)
		return EROFS;
	error = extattr_name(name, &name_space, &stored, &stored_length);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	mutex_lock(&inode->i_lock);
	error = extattr_load(inode, &area, &area_length);

	/* Checks the operation status. */
	if (error != 0)
		goto out;
	error = extattr_find(inode, area, area_length, name_space, stored,
			     stored_length, &at, &record, NULL, NULL);

	/* Checks the operation status. */
	if (error != 0)
		goto out;
	new_length = area_length - record;

	/* Handles the new length condition. */
	if (new_length != 0) {
		updated = kern_calloc(1, ms->super.bsize);

		/* Handles the updated availability. */
		if (updated == NULL) {
			error = ENOMEM;
			goto out;
		}

		/* Handles the at condition. */
		if (at != 0)
			memcpy(updated, area, at);

		/* Handles the area length condition. */
		if (area_length > at + record) {
			memcpy(updated + at, area + at + record,
			       area_length - at - record);
		}
	}
	error = extattr_publish(inode, updated, new_length);
out:
	mutex_unlock(&inode->i_lock);
	kern_free(updated);
	kern_free(area);

	/* Returns the computed result. */
	return error;
}

static int ufs_getattr(struct inode *inode, struct stat *status);

/* Supports the ufs getattr operation. */
static int
ufs_getattr(
	struct inode *inode,
	struct stat *status)
{
	struct ufs_inode_info *ui = info(inode);

	/* Renders the in-core inode as the attributes a caller sees. */
	memset(status, 0, sizeof(*status));
	status->st_dev = inode->i_mount->m_disk->d_dev;
	status->st_ino = inode->i_ino;
	status->st_mode = inode->i_mode;
	status->st_nlink = inode->i_linkcount;
	status->st_uid = inode->i_uid;
	status->st_gid = inode->i_gid;
	status->st_rdev = inode->i_rdev;
	status->st_size = inode->i_size;
	status->st_atime = inode->i_atime.tv_sec;
	status->st_mtime = inode->i_mtime.tv_sec;
	status->st_ctime = inode->i_ctime.tv_sec;
	status->st_blksize = state(inode->i_mount)->super.bsize;
	status->st_blocks = ui->blocks;

	/* Reports successful completion. */
	return 0;
}

static int valid_disk_time(time_t seconds, long nanoseconds);

/* Supports the valid disk time operation. */
static int
valid_disk_time(
	time_t seconds,
	long nanoseconds)
{
	(void)seconds;

	/* Returns the computed result. */
	return nanoseconds >= 0 && nanoseconds < 1000000000L;
}

static int ufs_setattr(struct inode *inode, const struct stat *status, unsigned mask);

/* Supports the ufs setattr operation. */
static int
ufs_setattr(
	struct inode *inode,
	const struct stat *status,
	unsigned mask)
{
	uid_t new_uid;
	gid_t new_gid;
	struct quota_transfer quota_transfer_state;
	mode_t old_mode;
	uid_t old_uid;
	gid_t old_gid;
	struct inode_time old_atime, old_mtime, old_ctime;
	long atime_nsec = 0, mtime_nsec = 0, ctime_nsec = 0;
	int error, quota_moved = 0;

	memset(&quota_transfer_state, 0, sizeof(quota_transfer_state));

#ifdef ZEDBSD_SYS_STAT_H
	atime_nsec = status->st_atim.tv_nsec;
	mtime_nsec = status->st_mtim.tv_nsec;
	ctime_nsec = status->st_ctim.tv_nsec;
#endif

	/* Checks the valid disk time result. */
	if ((mask & INODE_ATTR_ATIME) != 0 &&
	    !valid_disk_time(status->st_atime, atime_nsec))

		/* Returns the computed result. */
		return EOVERFLOW;

	/* Checks the valid disk time result. */
	if ((mask & INODE_ATTR_MTIME) != 0 &&
	    !valid_disk_time(status->st_mtime, mtime_nsec))

		/* Returns the computed result. */
		return EOVERFLOW;

	/* Checks the valid disk time result. */
	if ((mask & INODE_ATTR_CTIME) != 0 &&
	    !valid_disk_time(status->st_ctime, ctime_nsec))

		/* Returns the computed result. */
		return EOVERFLOW;

	/* Handles the mask condition. */
	if ((mask & INODE_ATTR_SIZE) != 0) {
		error = ufs_truncate(inode, status->st_size);

		/* Checks the operation status. */
		if (error != 0)
			return error;
	}

	mutex_lock(&inode->i_lock);

	/* Checks the state result. */
	if (!state(inode->i_mount)->writable) {
		mutex_unlock(&inode->i_lock);

		/* Returns the computed result. */
		return EROFS;
	}
	old_mode = inode->i_mode;
	old_uid = inode->i_uid;
	old_gid = inode->i_gid;
	old_atime = inode->i_atime;
	old_mtime = inode->i_mtime;
	old_ctime = inode->i_ctime;

	/* Handles the mask condition. */
	if ((mask & (INODE_ATTR_UID | INODE_ATTR_GID)) != 0) {
		new_uid =
			(mask & INODE_ATTR_UID) != 0 ? status->st_uid : old_uid;
		new_gid =
			(mask & INODE_ATTR_GID) != 0 ? status->st_gid : old_gid;
		error = quota_transfer_begin(
			&state(inode->i_mount)->quota, old_uid, old_gid,
			new_uid, new_gid,
			info(inode)->blocks /
				(state(inode->i_mount)->super.bsize /
				 UFS_SECTOR_SIZE),
			1, quota_now(), &quota_transfer_state);

		/* Checks the operation status. */
		if (error != 0) {
			mutex_unlock(&inode->i_lock);

			/* Returns the computed result. */
			return error;
		}
		quota_moved = old_uid != new_uid || old_gid != new_gid;
	}

	/* Handles the mask condition. */
	if (mask & INODE_ATTR_MODE) {
		inode->i_mode =
			(inode->i_mode & S_IFMT) | (status->st_mode & ~S_IFMT);
	}

	/* Handles the mask condition. */
	if (mask & INODE_ATTR_UID)
		inode->i_uid = status->st_uid;

	/* Handles the mask condition. */
	if (mask & INODE_ATTR_GID)
		inode->i_gid = status->st_gid;

	/* Handles the mask condition. */
	if (mask & INODE_ATTR_ATIME) {
		inode->i_atime.tv_sec = status->st_atime;
		inode->i_atime.tv_nsec = atime_nsec;
	}

	/* Handles the mask condition. */
	if (mask & INODE_ATTR_MTIME) {
		inode->i_mtime.tv_sec = status->st_mtime;
		inode->i_mtime.tv_nsec = mtime_nsec;
	}

	/* Handles the mask condition. */
	if (mask & INODE_ATTR_CTIME) {
		inode->i_ctime.tv_sec = status->st_ctime;
		inode->i_ctime.tv_nsec = ctime_nsec;
	}
	error = persist_inode(inode);

	/* Checks the operation status. */
	if (error != 0) {
		/* Handles the quota moved condition. */
		if (quota_moved)
			quota_transfer_rollback(&quota_transfer_state);
		inode->i_mode = old_mode;
		inode->i_uid = old_uid;
		inode->i_gid = old_gid;
		inode->i_atime = old_atime;
		inode->i_mtime = old_mtime;
		inode->i_ctime = old_ctime;
	} else if (quota_moved) {
		quota_transfer_commit(&quota_transfer_state);
	}
	mutex_unlock(&inode->i_lock);

	/* Returns the computed result. */
	return error;
}

static int ufs_inode_sync(struct inode *inode);

/* Supports the ufs inode sync operation. */
static int
ufs_inode_sync(
	struct inode *inode)
{
	int error;

	mutex_lock(&inode->i_lock);

	/*
 * A retired or not-yet-bound cache object has no persistent inode
	 * identity. */
	if (inode->i_ino == 0) {
		mutex_unlock(&inode->i_lock);

		/* Reports successful completion. */
		return 0;
	}
	error = state(inode->i_mount)->writable ? persist_inode(inode)
		: (inode->i_mount->m_flags & MOUNT_READ_ONLY) != 0 ? 0
								   : EROFS;
	mutex_unlock(&inode->i_lock);

	/* Returns the computed result. */
	return error;
}

/* Begin consolidated ufs-inode-retirement.inc. */
/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

static int retire_inode_locked(struct inode *inode, struct ufs_release_group *group);
static int retire_inode_group(struct inode *inode, int *handled);

/* Retires an empty zero-link inode and its allocation accounting together. */
static int
retire_inode_locked(
	struct inode *inode,
	struct ufs_release_group *group)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	struct ufs_journal_extent extents[3];
	struct ufs_transaction_outcome outcome;
	uint64_t fragment;
	uint32_t cg;
	uint32_t local;
	uint32_t free_inodes;
	uint32_t directories;
	unsigned n;
	int is_directory;
	int error;
	int quota_error;

	/*
 * Refuses reuse while any persistent block owner or namespace link
	 * remains. */
	ms = state(inode->i_mount);
	ui = info(inode);

	/* Handles the ms condition. */
	if (!ms->writable)
		return EROFS;

	/* Handles the inode condition. */
	if (inode->i_ino <= UFS_ROOT_INO ||
	    (uint64_t)inode->i_ino >= (uint64_t)ms->super.ncg * ms->super.ipg ||
	    inode->i_linkcount != 0 || inode->i_size != 0 || ui->blocks != 0 ||
	    ui->extattr_size != 0)

		/* Returns the computed result. */
		return EIO;
	/* Process each element required by the operation. */
	for (n = 0; n < UFS_NDADDR; n++) {
		/* Handles the ui condition. */
		if (ui->direct[n] != 0)
			return EIO;
	}
	/* Process each element required by the operation. */
	for (n = 0; n < UFS_NIADDR; n++) {
		/* Handles the ui condition. */
		if (ui->indirect[n] != 0)
			return EIO;
	}
	/* Process each element required by the operation. */
	for (n = 0; n < UFS_NXADDR; n++) {
		/* Handles the ui condition. */
		if (ui->extattr[n] != 0)
			return EIO;
	}

	/*
 * Copies the allocated inode map and validates totals before private
	 * edits. */
	cg = inode->i_ino / ms->super.ipg;
	local = inode->i_ino % ms->super.ipg;
	error = load_cg_locked(inode->i_mount, cg);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Checks the bit test result. */
	if (!bit_test(ms->cg + ms->cg_iusedoff, local))
		return EIO;
	free_inodes = drv_ufs_get32(ms->cg, UFS_CG_NIFREE, ms->super.swapped);
	directories = drv_ufs_get32(ms->cg, UFS_CG_NDIR, ms->super.swapped);
	is_directory = inode->i_type == INODE_DIR;

	/* Handles the free inodes condition. */
	if (free_inodes >= ms->super.ipg ||
	    ms->super.cstotal_nifree == UINT64_MAX ||
	    (is_directory && (directories == 0 || ms->super.cstotal_ndir == 0)))

		/* Returns the computed result. */
		return EIO;
	memcpy(group->cg, ms->cg, ms->super.bsize);
	memcpy(&group->image, ui, sizeof(group->image));
	group->image.inode.i_mode = 0;
	group->image.inode.i_type = INODE_NONE;
	error = prepare_inode_locked(&group->image.inode, group->dinode,
				     &fragment);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = prepare_super_summaries(inode->i_mount, group->summaries);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/*
 * Makes the inode reusable only in the same group that retires its old
	 * kind. */
	bit_clear(group->cg + ms->cg_iusedoff, local);
	drv_ufs_put32(group->cg, UFS_CG_NIFREE, free_inodes + 1U,
		      ms->super.swapped);
	drv_ufs_put64(group->summaries, UFS_FS_CSTOTAL_NIFREE,
		      ms->super.cstotal_nifree + 1U, ms->super.swapped);

	/* Handles the directory condition. */
	if (is_directory) {
		drv_ufs_put32(group->cg, UFS_CG_NDIR, directories - 1U,
			      ms->super.swapped);
		drv_ufs_put64(group->summaries, UFS_FS_CSTOTAL_NDIR,
			      ms->super.cstotal_ndir - 1U, ms->super.swapped);
	}
	extents[0].target = (cgstart(&ms->super, cg) + ms->super.cblkno)
			    << ms->super.fsbtodb;
	extents[0].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[0].payload = group->cg;
	extents[1].target = UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;
	extents[1].sectors = UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;
	extents[1].payload = group->summaries;
	extents[2].target = fragment << ms->super.fsbtodb;
	extents[2].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[2].payload = group->dinode;
	ms->cg_valid = 0;
	buf_view_release(&ms->cg_view);
	error = metadata_group_commit(inode->i_mount, extents, 3, NULL,
				      &outcome);

	/*
 * Publishes positive retirement and releases quota once, even on
	 * recovered error. */
	if (outcome.committed) {
		memcpy(ms->cg, group->cg, ms->super.bsize);
		ms->super.cstotal_nifree++;

		/* Handles the directory condition. */
		if (is_directory)
			ms->super.cstotal_ndir--;
		inode->i_mode = 0;
		inode->i_type = INODE_NONE;

		/*
 * Prevents final-reference retry from writing an already
		 * reusable identity. */
		inode->i_ino = 0;
		quota_error = quota_release(&ms->quota, inode->i_uid,
					    inode->i_gid, 0, 1);

		/* Checks the operation status. */
		if (quota_error != 0) {
			ms->writable = 0;

			/* Checks the operation status. */
			if (error == 0)
				error = quota_error;
		}
	}

	/* Handles the outcome condition. */
	if (outcome.committed || outcome.uncertain)
		ms->cg_dirty = outcome.uncertain;

	/*
 * Preserves the original failure independently of established
	 * retirement. */
	return error;
}

/* Owns private retirement storage and exclusion after data and xattr teardown. */
static int
retire_inode_group(
	struct inode *inode,
	int *handled)
{
	struct ufs_mount_state *ms;
	struct ufs_release_group *group;
	size_t bytes;
	int error;

	/* Declines unsupported profiles before any metadata mutation. */
	ms = state(inode->i_mount);
	*handled = 0;
	bytes = 2U * ms->super.bsize + UFS_SBLOCK_SIZE;

	/* Handles the ms condition. */
	if (!ms->journal_enabled || ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)

		/* Reports successful completion. */
		return 0;
	*handled = 1;
	group = kern_calloc(1, sizeof(*group));

	/* Handles the group availability. */
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(bytes);

	/* Handles the memory availability. */
	if (group->memory == NULL) {
		kern_free(group);

		/* Returns the computed result. */
		return ENOMEM;
	}
	group->cg = group->memory;
	group->dinode = group->cg + ms->super.bsize;
	group->summaries = group->dinode + ms->super.bsize;
	mutex_lock(&inode->i_lock);
	mutex_lock(&ms->lock);
	error = retire_inode_locked(inode, group);
	mutex_unlock(&ms->lock);
	mutex_unlock(&inode->i_lock);
	kern_free(group->memory);
	kern_free(group);

	/*
 * Returns the admitted result without a second compensating retirement.
	 */
	return error;
}
/* End consolidated ufs-inode-retirement.inc. */

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

	/* Handles the inode condition. */
	if (inode->i_linkcount != 0 || inode->i_ino <= UFS_ROOT_INO)
		return EINVAL;

	/* Checks the state result. */
	if (!state(inode->i_mount)->writable)
		return EROFS;

	/*
 * Keeps every remaining reference reachable until its own release
	 * commits. */
	error = ufs_truncate(inode, 0);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	mutex_lock(&inode->i_lock);
	error = extattr_publish(inode, NULL, 0);
	mutex_unlock(&inode->i_lock);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = retire_inode_group(inode, &handled);

	/* Handles the handled condition. */
	if (handled)
		return error;

	/*
 * Preserves the ordered retirement path for profiles outside group
	 * admission. */
	if (inode->i_type == INODE_DIR) {
		error = adjust_directory_count(inode->i_mount,
					       (uint32_t)inode->i_ino, -1);

		/* Checks the operation status. */
		if (error != 0)
			return error;
	}
	inode->i_mode = 0;
	inode->i_type = INODE_NONE;
	ui->blocks = 0;
	error = persist_inode(inode);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = disk_sync(inode->i_mount->m_disk);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = free_inode_number(inode->i_mount, (uint32_t)inode->i_ino,
				  inode->i_uid, inode->i_gid);

	/*
 * Returns actual retirement failure to explicit cleanup and recovery
	 * callers. */
	return error;
}

static void ufs_reclaim(struct inode *inode);

/* Preserves the VFS final-reference callback while sharing checked reclamation. */
static void
ufs_reclaim(
	struct inode *inode)
{
	/*
 * Ignores identities whose lifetime does not permit filesystem
	 * retirement. */
	if (inode->i_linkcount != 0 || inode->i_ino <= UFS_ROOT_INO ||
	    !state(inode->i_mount)->writable)

		/* Returns the computed result. */
		return;

	/*
 * The callback has no errno channel; explicit owners call the checked
	 * helper. */
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

	/* Handles the inode condition. */
	if (inode->i_ino <= UFS_ROOT_INO)
		return EINVAL;
	image = kern_malloc(sizeof(*image) + ms->super.bsize);

	/* Handles the image availability. */
	if (image == NULL)
		return ENOMEM;
	block = (uint8_t *)(image + 1);
	memset(&outcome, 0, sizeof(outcome));
	mutex_lock(&inode->i_lock);
	mutex_lock(&ms->lock);
	memcpy(image, info(inode), sizeof(*image));
	image->inode.i_linkcount = 0;
	error = ms->writable
			? prepare_inode_locked(&image->inode, block, &fragment)
			: EROFS;

	/* Checks the operation status. */
	if (error == 0) {
		extent.target = fragment << ms->super.fsbtodb;
		extent.sectors = ms->super.bsize / UFS_SECTOR_SIZE;
		extent.payload = block;
		error = metadata_group_commit(inode->i_mount, &extent, 1, NULL,
					      &outcome);
	}

	/*
 * Makes a proven zero-link owner eligible for checked resource
	 * reclamation. */
	if (outcome.committed) {
		inode->i_linkcount = 0;
		inode->i_flags |= INODE_DEAD;
	}
	mutex_unlock(&ms->lock);
	mutex_unlock(&inode->i_lock);
	kern_free(image);

	/*
 * Retains references and the original errno on an unsuccessful
	 * transition. */
	return error;
}

/* Discards an unpublished journal-backed creation through checked release owners. */
static int
discard_reserved_inode(
	struct inode *inode)
{
	int error;

	/*
 * Detaches borrowed endpoints before any final-reference destruction is
	 * possible. */
	detach_new_socket_special(inode);
	error = creation_unlink_group(inode);

	/* Checks the operation status. */
	if (error == 0)
		error = reclaim_unlinked_inode(inode);

	/* Checks the operation status. */
	if (error == 0) {
		inode->i_ino = 0;
		inode->i_flags |= INODE_DEAD;
	}
	inode_release(inode);

	/*
 * Reports incomplete cleanup without hiding which persistent owners
	 * remain. */
	return error;
}

static const struct inode_ops ufs_inode_ops = {.lookup = ufs_lookup,
					       .create = ufs_create,
					       .mkdir = ufs_mkdir,
					       .mknod = ufs_mknod,
					       .unlink = ufs_unlink,
					       .rmdir = ufs_rmdir,
					       .rename = ufs_rename,
					       .link = ufs_link,
					       .symlink = ufs_symlink,
					       .readlink = ufs_readlink,
					       .getattr = ufs_getattr,
					       .setattr = ufs_setattr,
					       .truncate = ufs_truncate,
					       .sync = ufs_inode_sync,
					       .getxattr = ufs_getxattr,
					       .setxattr = ufs_setxattr,
					       .listxattr = ufs_listxattr,
					       .removexattr = ufs_removexattr,
					       .reclaim = ufs_reclaim};
static int ufs_file_sync(struct file *file);

/* Supports the ufs file sync operation. */
static int
ufs_file_sync(
	struct file *file)
{
	int function_result;
	int error;

	/* Handles the file availability. */
	if (file == NULL)
		return EINVAL;
	error = inode_sync(file->f_inode);

	/* Computes the function result. */
	function_result = error != 0 ? error : ufs_sync(file->f_inode->i_mount);

	/* Returns the computed result. */
	return function_result;
}
static const struct file_ops ufs_regular_ops = {.read = ufs_read,
						.write = ufs_write,
						.pread = ufs_pread,
						.pwrite = ufs_pwrite,
						.pwrite_internal =
							ufs_pwrite_context,
						.fsync = ufs_file_sync};
static const struct file_ops ufs_directory_ops = {.readdir = ufs_readdir,
						  .fsync = ufs_file_sync};

static struct inode *ufs_alloc_inode(struct mount *mountp);

/* Supports the ufs alloc inode operation. */
static struct inode *
ufs_alloc_inode(
	struct mount *mountp)
{
	struct inode *function_result;

	(void)mountp;

	/* Computes the function result. */
	function_result =
		(struct inode *)kern_calloc(1, sizeof(struct ufs_inode_info));

	/* Returns the computed result. */
	return function_result;
}
static void ufs_free_inode(struct inode *inode);

/* Supports the ufs free inode operation. */
static void
ufs_free_inode(
	struct inode *inode)
{
	kern_free(inode);
}

static int ufs_read_super(struct disk *disk, struct ufs_super *super);

/* Supports the ufs read super operation. */
static int
ufs_read_super(
	struct disk *disk,
	struct ufs_super *super)
{
	uint8_t *buffer;
	int error;

	/* Handles the disk availability. */
	if (disk == NULL || disk->d_block_size != UFS_SECTOR_SIZE)
		return EOPNOTSUPP;
	buffer = kern_malloc(UFS_SBLOCK_SIZE);

	/* Handles the buffer availability. */
	if (buffer == NULL)
		return ENOMEM;
	error = observed_disk_read(disk, UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE,
				   UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE, buffer);

	/* Checks the operation status. */
	if (error == 0) {
		error = drv_ufs_super_decode(buffer, UFS_SBLOCK_SIZE,
					     disk->d_block_count, super);
	}
	kern_free(buffer);

	/* Returns the computed result. */
	return error;
}

static char ufs_identity_hex(unsigned value);

/* Supports the ufs identity hex operation. */
static char
ufs_identity_hex(
	unsigned value)
{
	/* Returns the computed result. */
	return (char)(value < 10U ? '0' + value : 'A' + value - 10U);
}

static void ufs_identity_hex32(char output[8], uint32_t value);

/* Supports the ufs identity hex32 operation. */
static void
ufs_identity_hex32(
	char output[8],
	uint32_t value)
{
	unsigned index;

	/* Process each remaining element. */
	for (index = 0; index < 8U; index++) {
		output[index] =
			ufs_identity_hex((value >> (28U - index * 4U)) & 15U);
	}
}

static void ufs_identity_label(char *output, size_t capacity, const uint8_t *input, size_t length);

/* Supports the ufs identity label operation. */
static void
ufs_identity_label(
	char *output,
	size_t capacity,
	const uint8_t *input,
	size_t length)
{
	size_t end = length;
	size_t index;

	/* Continue while the operation condition remains true. */
	while (end != 0U && (input[end - 1U] == ' ' || input[end - 1U] == 0U))
		end--;

	/* Checks the current endpoint. */
	if (end >= capacity)
		end = capacity - 1U;
	/* Process each remaining element. */
	for (index = 0; index < end; index++) {
		output[index] = input[index] >= 0x20U && input[index] <= 0x7eU
					? (char)input[index]
					: '_';
	}
	output[end] = '\0';
}

/*
 * Implements the drv ufs identify operation.
 */
int
drv_ufs_identify(
	struct disk *disk,
	struct block_identity *identity)
{
	struct ufs_super super;
	uint8_t *buffer;
	uint32_t first, second;
	uint64_t first_block, block_count;
	int error;

	/* Handles the disk availability. */
	if (disk == NULL || identity == NULL)
		return EINVAL;

	/* Handles the disk condition. */
	if (disk->d_block_size != UFS_SECTOR_SIZE)
		return EOPNOTSUPP;
	first_block = UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;
	block_count = UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;

	/* Handles the disk condition. */
	if (disk->d_block_count < first_block + block_count)
		return EOPNOTSUPP;
	buffer = kern_malloc(UFS_SBLOCK_SIZE);

	/* Handles the buffer availability. */
	if (buffer == NULL)
		return ENOMEM;
	error = disk_read_direct(disk, first_block, (uint32_t)block_count,
				 buffer);

	/* Checks the operation status. */
	if (error == 0) {
		error = drv_ufs_super_decode(buffer, UFS_SBLOCK_SIZE,
					     disk->d_block_count, &super);
	}

	/* Checks the operation status. */
	if (error != 0) {
		kern_free(buffer);

		/* Returns the computed result. */
		return error;
	}

	strcpy(identity->type, "ufs");
	identity->flags |= ZEDBSD_BLKID_TYPE;
	first = drv_ufs_get32(buffer, UFS_FS_ID, super.swapped);
	second = drv_ufs_get32(buffer, UFS_FS_ID + 4U, super.swapped);

	/* Handles the first condition. */
	if (first != 0U || second != 0U) {
		ufs_identity_hex32(identity->uuid, first);
		ufs_identity_hex32(identity->uuid + 8U, second);
		identity->uuid[16] = '\0';
		identity->flags |= ZEDBSD_BLKID_UUID;
	}
	ufs_identity_label(identity->label, sizeof(identity->label),
			   buffer + UFS_FS_VOLNAME, UFS_FS_VOLNAME_SIZE);

	/* Handles the identity condition. */
	if (identity->label[0] != '\0')
		identity->flags |= ZEDBSD_BLKID_LABEL;
	kern_free(buffer);

	/* Reports successful completion. */
	return 0;
}

static int ufs_write_clean(struct mount *mountp, uint8_t clean);

/* Supports the ufs write clean operation. */
static int
ufs_write_clean(
	struct mount *mountp,
	uint8_t clean)
{
	struct ufs_mount_state *ms = state(mountp);
	uint8_t *buffer;
	int error;

	buffer = kern_malloc(UFS_SBLOCK_SIZE);

	/* Handles the buffer availability. */
	if (buffer == NULL)
		return ENOMEM;
	mutex_lock(&ms->lock);
	mutex_lock(&ms->journal_lock);
	error = journal_checkpoint_locked(mountp);
	mutex_unlock(&ms->journal_lock);

	/* Checks the operation status. */
	if (error == 0) {
		error = observed_disk_read(
			mountp->m_disk, UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE,
			UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE, buffer);
	}

	/* Checks the operation status. */
	if (error == 0) {
		buffer[UFS_FS_CLEAN] = clean;
		error = write_sectors(
			mountp, UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE,
			UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE, buffer);
	}

	/* Checks the operation status. */
	if (error == 0)
		error = disk_sync(mountp->m_disk);

	/* Checks the operation status. */
	if (error == 0)
		ms->super.clean = clean;
	mutex_unlock(&ms->lock);
	kern_free(buffer);

	/* Returns the computed result. */
	return error;
}
static int ufs_probe(struct disk *disk);

/* Supports the ufs probe operation. */
static int
ufs_probe(
	struct disk *disk)
{
	int function_result;
	struct ufs_super s;

	/* Obtains the ufs read super result. */
	function_result = ufs_read_super(disk, &s);

	/* Returns the computed result. */
	return function_result;
}

static int ufs_quota_rebuild(struct mount *mountp);

/* Supports the ufs quota rebuild operation. */
static int
ufs_quota_rebuild(
	struct mount *mountp)
{
	uint64_t fragment, blocks;
	uint8_t *raw;
	uint16_t mode;
	struct ufs_mount_state *ms = state(mountp);
	uint8_t *block;
	uint32_t cg, index;
	int error = 0;

	block = kern_malloc(ms->super.bsize);

	/* Handles the block availability. */
	if (block == NULL)
		return ENOMEM;
	/* Process each element required by the operation. */
	for (cg = 0; cg < ms->super.ncg && error == 0; cg++) {
		error = load_cg_locked(mountp, cg);

		/* Checks the operation status. */
		if (error != 0)
			break;
		/* Process each remaining element. */
		for (index = 0; index < ms->super.ipg; index++) {
			/* Checks the bit test result. */
			if (!bit_test(ms->cg + ms->cg_iusedoff, index))
				continue;
			fragment = cgstart(&ms->super, cg) + ms->super.iblkno +
				   (index / ms->super.inopb) * ms->super.frag;
			error = read_block(mountp, fragment, block);

			/* Checks the operation status. */
			if (error != 0)
				break;
			raw = block +
			      (index % ms->super.inopb) * UFS_DINODE_SIZE;
			mode = drv_ufs_get16(raw, UFS_DI_MODE,
					     ms->super.swapped);

			/* Validates the selected mode. */
			if (mode == 0)
				continue;
			blocks = drv_ufs_get64(raw, UFS_DI_BLOCKS,
					       ms->super.swapped);

			/* Handles the blocks condition. */
			if (blocks % (ms->super.bsize / UFS_SECTOR_SIZE) != 0) {
				error = EIO;
				break;
			}
			error = quota_rebuild_add(
				&ms->quota,
				drv_ufs_get32(raw, UFS_DI_UID,
					      ms->super.swapped),
				drv_ufs_get32(raw, UFS_DI_GID,
					      ms->super.swapped),
				blocks / (ms->super.bsize / UFS_SECTOR_SIZE),
				1);

			/* Checks the operation status. */
			if (error != 0)
				break;
		}
	}
	kern_free(block);

	/* Returns the computed result. */
	return error;
}

static int ufs_quota_load(struct mount *mountp, struct inode *root);

/* Supports the ufs quota load operation. */
static int
ufs_quota_load(
	struct mount *mountp,
	struct inode *root)
{
	struct ufs_mount_state *ms = state(mountp);
	uint8_t *buffer;
	ssize_t length, loaded;
	int error;

	length = ufs_getxattr(root, UFS_QUOTA_XATTR, NULL, 0);

	/* Checks the current data length. */
	if (length == -ENODATA)
		return 0;

	/* Checks the current data length. */
	if (length < 0)
		return (int)-length;

	/* Checks the current data length. */
	if (length == 0 || (size_t)length > ms->super.bsize)
		return EINVAL;
	buffer = kern_malloc((size_t)length);

	/* Handles the buffer availability. */
	if (buffer == NULL)
		return ENOMEM;
	loaded = ufs_getxattr(root, UFS_QUOTA_XATTR, buffer, (size_t)length);
	error = loaded == length ? quota_import_config(&ms->quota, buffer,
						       (size_t)length)
				 : (loaded < 0 ? (int)-loaded : EIO);
	kern_free(buffer);

	/* Returns the computed result. */
	return error;
}

static int snapshot_disk_submit(struct disk *disk, struct bio *bio);

/* Supports the snapshot disk submit operation. */
static int
snapshot_disk_submit(
	struct disk *disk,
	struct bio *bio)
{
	struct ufs_mount_state *ms = disk != NULL ? disk->d_data : NULL;
	int error;

	/* Handles the ms availability. */
	if (ms == NULL || bio == NULL)
		return EINVAL;

	/* The callback context owns a disk pointer; it is not itself a disk. */
	if (bio->b_op == BIO_FLUSH)
		error = disk_sync(ms->snapshot_io.disk);
	else if (bio->b_op != BIO_READ) {
		error = EROFS;
	} else {
		mutex_lock(&ms->snapshot_lock);
		error = drv_ufs_snapshot_read(&ms->snapshot,
					      bio->b_mapped_block,
					      bio->b_block_count, bio->b_data);
		mutex_unlock(&ms->snapshot_lock);
	}
	bio_complete(bio, error,
		     error == 0 && bio->b_op == BIO_READ
			     ? (size_t)bio->b_block_count * UFS_SECTOR_SIZE
			     : 0);

	/* Reports successful completion. */
	return 0;
}
static const struct disk_ops snapshot_disk_ops = {.submit =
							  snapshot_disk_submit};
static unsigned snapshot_disk_sequence;

static int snapshot_disk_publish(struct ufs_mount_state *ms);

/* Supports the snapshot disk publish operation. */
static int
snapshot_disk_publish(
	struct ufs_mount_state *ms)
{
	struct disk *disk;
	unsigned number;
	int error;
	unsigned attempt;

	/* Handles the snapshot disk availability. */
	if (ms->snapshot_disk != NULL)
		return 0;
	/* Process each element required by the operation. */
	for (attempt = 0; attempt < DISK_MAX; attempt++) {
		disk = disk_alloc();
		number = snapshot_disk_sequence++;

		/* Handles the disk availability. */
		if (disk == NULL)
			return ENOSPC;
		memcpy(disk->d_name, "ufssnap", 7);

		/* Handles the number condition. */
		if (number >= 100U)
			number %= 100U;

		/* Handles the number condition. */
		if (number >= 10U) {
			disk->d_name[7] = (char)('0' + number / 10U);
			disk->d_name[8] = (char)('0' + number % 10U);
			disk->d_name[9] = '\0';
		} else {
			disk->d_name[7] = (char)('0' + number);
			disk->d_name[8] = '\0';
		}
		disk->d_flags = DISK_READ_ONLY;
		disk->d_block_size = UFS_SECTOR_SIZE;
		disk->d_block_count = ms->snapshot.volume_sectors;
		disk->d_max_transfer_blocks = 128;
		disk->d_ops = &snapshot_disk_ops;
		disk->d_data = ms;
		error = disk_create(disk);

		/* Checks the operation status. */
		if (error == 0) {
			ms->snapshot_disk = disk;

			/* Reports successful completion. */
			return 0;
		}
		(void)disk_destroy(disk);

		/* Checks the operation status. */
		if (error != EEXIST)
			return error;
	}

	/* Returns the computed result. */
	return ENOSPC;
}

static int snapshot_disk_remove(struct ufs_mount_state *ms);

/* Supports the snapshot disk remove operation. */
static int
snapshot_disk_remove(
	struct ufs_mount_state *ms)
{
	struct disk *disk = ms->snapshot_disk;
	int error;

	/* Handles the disk availability. */
	if (disk == NULL)
		return 0;
	error = disk_gone_if_idle(disk);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = disk_destroy(disk);

	/* Checks the operation status. */
	if (error == 0)
		ms->snapshot_disk = NULL;

	/* Returns the computed result. */
	return error;
}

static void ufs_state_free(struct ufs_mount_state *ms);

/* Supports the ufs state free operation. */
static void
ufs_state_free(
	struct ufs_mount_state *ms)
{
	/* Handles the ms availability. */
	if (ms == NULL)
		return;
	journal_image_free(ms);
	buf_view_release(&ms->cg_view);
	kern_free(ms->snapshot_map);
	kern_free(ms->cg);
	kern_free(ms);
}

static int ufs_quota_persist(struct mount *mountp);

/* Supports the ufs quota persist operation. */
static int
ufs_quota_persist(
	struct mount *mountp)
{
	struct ufs_mount_state *ms = state(mountp);
	uint8_t *buffer;
	size_t length;
	int error;

	/* Handles the m root availability. */
	if (!ms->writable || mountp->m_root == NULL)
		return EROFS;
	buffer = kern_malloc(ms->super.bsize);

	/* Handles the buffer availability. */
	if (buffer == NULL)
		return ENOMEM;
	error = quota_export_config(&ms->quota, buffer, ms->super.bsize,
				    &length);

	/* Checks the operation status. */
	if (error == 0) {
		error = ufs_setxattr(mountp->m_root, UFS_QUOTA_XATTR, buffer,
				     length, 0);
	}

	/* Checks the operation status. */
	if (error == 0)
		error = disk_sync(mountp->m_disk);
	kern_free(buffer);

	/* Returns the computed result. */
	return error;
}

/* Begin consolidated ufs-orphan.inc. */
/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

struct ufs_orphan_scan {
	struct ufs_inode_info inode;
	uint8_t *bitmap;
	uint8_t *block;
};

static int orphan_recover_one(struct mount *mountp, uint32_t number, const uint8_t *raw, struct ufs_inode_info *owner);
static int orphan_scan_locked(struct mount *mountp, struct ufs_orphan_scan *scan);
static int orphan_recover(struct mount *mountp);

/* Reclaims one validated zero-link identity without publishing a cache object. */
static int
orphan_recover_one(
	struct mount *mountp,
	uint32_t number,
	const uint8_t *raw,
	struct ufs_inode_info *owner)
{
	struct ufs_mount_state *ms;
	size_t bytes;
	int error;

	/*
 * Requires grouped release and retirement before permitting recovery
	 * mutations. */
	ms = state(mountp);
	bytes = 3U * ms->super.bsize + UFS_SBLOCK_SIZE;

	/* Handles the ms condition. */
	if (ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)

		/* Returns the computed result. */
		return EOPNOTSUPP;
	memset(owner, 0, sizeof(*owner));
	owner->inode.i_mount = mountp;
	(void)mutex_init(&owner->inode.i_lock, LOCK_RANK_INODE, "ufs orphan");
	error = decode_inode_raw(&owner->inode, raw, number, 1);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/*
 * Reuses checked pointer/xattr/bitmap owners and their conservative
	 * outcomes. */
	error = reclaim_unlinked_inode(&owner->inode);

	/* Returns the computed result. */
	return error;
}

/* Scans a private mount using stable candidate bits and freshly read dinodes. */
static int
orphan_scan_locked(
	struct mount *mountp,
	struct ufs_orphan_scan *scan)
{
	struct ufs_mount_state *ms;
	uint8_t *raw;
	uint64_t number;
	uint64_t fragment;
	uint32_t cg;
	uint32_t index;
	size_t map_bytes;
	int error;

	/*
 * Saves each candidate map before reclaim can change the mount's CG
	 * buffer. */
	ms = state(mountp);
	map_bytes = ((size_t)ms->super.ipg + 7U) / 8U;
	/* Process each element required by the operation. */
	for (cg = 0; cg < ms->super.ncg; cg++) {
		error = load_cg_locked(mountp, cg);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		memcpy(scan->bitmap, ms->cg + ms->cg_iusedoff, map_bytes);

		/*
 * Leaves reserved slots and linked namespace owners untouched.
		 */
		/* Process each remaining element. */
		for (index = 0; index < ms->super.ipg; index++) {
			/* Checks the bit test result. */
			if (!bit_test(scan->bitmap, index))
				continue;
			number = (uint64_t)cg * ms->super.ipg + index;

			/* Handles the number condition. */
			if (number <= UFS_ROOT_INO)
				continue;

			/* Handles the number condition. */
			if (number > UINT32_MAX)
				return EOVERFLOW;
			fragment = cgstart(&ms->super, cg) + ms->super.iblkno +
				   (index / ms->super.inopb) * ms->super.frag;
			error = read_block(mountp, fragment, scan->block);

			/* Checks the operation status. */
			if (error != 0)
				return error;
			raw = scan->block +
			      (index % ms->super.inopb) * UFS_DINODE_SIZE;

			/* Checks the drv ufs get16 result. */
			if (drv_ufs_get16(raw, UFS_DI_NLINK,
					  ms->super.swapped) != 0)
				continue;
			error = orphan_recover_one(mountp, (uint32_t)number,
						   raw, &scan->inode);

			/* Checks the operation status. */
			if (error != 0)
				return error;
		}
	}

	/*
 * Completes all bounded per-inode reclamations before mount
	 * publication. */
	return 0;
}

/* Recovers journal-owned orphans after validation and quota rebuild on a private mount. */
static int
orphan_recover(
	struct mount *mountp)
{
	struct ufs_mount_state *ms;
	struct ufs_orphan_scan *scan;
	int error;

	/*
 * Keeps readonly and nonjournal admission free of orphan-reclamation
	 * writes. */
	ms = state(mountp);

	/* Handles the ms condition. */
	if (!ms->writable || !ms->journal_enabled)
		return 0;

	/* Handles the m root availability. */
	if (mountp->m_root != NULL)
		return EBUSY;
	scan = kern_calloc(1, sizeof(*scan) + 2U * ms->super.bsize);

	/* Handles the scan availability. */
	if (scan == NULL)
		return ENOMEM;
	scan->bitmap = (uint8_t *)(scan + 1);
	scan->block = scan->bitmap + ms->super.bsize;

	/*
 * Excludes namespace users while each checked owner takes its metadata
	 * locks. */
	mutex_lock(&ms->namespace_lock);
	error = orphan_scan_locked(mountp, scan);

	/* Checks the operation status. */
	if (error != 0)
		ms->writable = 0;
	mutex_unlock(&ms->namespace_lock);
	kern_free(scan);

	/*
 * Returns failure with persistent remaining ownership for the next
	 * mount attempt. */
	return error;
}
static int ufs_mount_impl(struct mount *mountp);

/* End consolidated ufs-orphan.inc. */
static int
ufs_mount_impl(
	struct mount *mountp)
{
	uint8_t *free_map;
	uint32_t fragment, ndblk;
	struct ufs_mount_state *ms;
	struct inode *root;
	int error;
	uint64_t total_ndir = 0, total_nbfree = 0, total_nifree = 0,
		 total_nffree = 0;
	uint32_t cg;
	int summaries_rebuilt = 0;
	off_t cursor = 0;
	uint32_t number;
	uint8_t type;
	char name[NAME_MAX + 1U];

	/* Handles the mountp availability. */
	if (mountp == NULL || mountp->m_disk == NULL)
		return EINVAL;
	ms = kern_calloc(1, sizeof(*ms));

	/* Handles the ms availability. */
	if (ms == NULL)
		return ENOMEM;
	error = ufs_read_super(mountp->m_disk, &ms->super);

	/* Checks the operation status. */
	if (error) {
		kern_free(ms);

		/* Returns the computed result. */
		return error;
	}
	mountp->m_data = ms;
	(void)mutex_init(&ms->journal_lock, LOCK_RANK_DEVICE, "ufs journal");
	(void)mutex_init(&ms->snapshot_lock, LOCK_RANK_DEVICE, "ufs snapshot");
	error = journal_discover(mountp, ms);

	/* Checks the operation status. */
	if (error != 0) {
		mountp->m_data = NULL;
		ufs_state_free(ms);

		/* Returns the computed result. */
		return error;
	}
	error = snapshot_discover(mountp, ms);

	/* Checks the operation status. */
	if (error != 0) {
		mountp->m_data = NULL;
		ufs_state_free(ms);

		/* Returns the computed result. */
		return error;
	}
	(void)mutex_init(&ms->namespace_lock, LOCK_RANK_NAMESPACE,
			 "ufs namespace");
	(void)mutex_init(&ms->lock, LOCK_RANK_INODE, "ufs mount");
	quota_state_init(&ms->quota);
	ms->cg = kern_malloc(ms->super.bsize);

	/* Handles the cg availability. */
	if (ms->cg == NULL) {
		mountp->m_data = NULL;
		ufs_state_free(ms);

		/* Returns the computed result. */
		return ENOMEM;
	}
	/* Process each element required by the operation. */
	for (cg = 0; cg < ms->super.ncg; cg++) {
		error = load_cg_locked(mountp, cg);

		/* Checks the operation status. */
		if (error != 0)
			break;
		ndblk = cg_ndblk(&ms->super, cg);
		free_map = ms->cg + ms->cg_freeoff;
		/* Process each element required by the operation. */
		for (fragment = 0;
		     fragment < ms->super.dblkno && fragment < ndblk;
		     fragment++) {
			/* Handles the bit test condition. */
			if (bit_test(free_map, fragment)) {
				error = EINVAL;
				break;
			}
		}
		/* Process each element required by the operation. */
		for (fragment = ndblk; error == 0 && fragment < ms->super.fpg;
		     fragment++) {
			/* Handles the bit test condition. */
			if (bit_test(free_map, fragment)) {
				error = EINVAL;
				break;
			}
		}

		/* Checks the operation status. */
		if (error != 0)
			break;

		/* Checks the bit test result. */
		if (cg == 0 &&
		    !bit_test(ms->cg + ms->cg_iusedoff, UFS_ROOT_INO)) {
			error = EINVAL;
			break;
		}
		total_ndir +=
			drv_ufs_get32(ms->cg, UFS_CG_NDIR, ms->super.swapped);
		total_nbfree +=
			drv_ufs_get32(ms->cg, UFS_CG_NBFREE, ms->super.swapped);
		total_nifree +=
			drv_ufs_get32(ms->cg, UFS_CG_NIFREE, ms->super.swapped);
		total_nffree +=
			drv_ufs_get32(ms->cg, UFS_CG_NFFREE, ms->super.swapped);
	}

	/* Checks the operation status. */
	if (error == 0 && (total_ndir != ms->super.cstotal_ndir ||
			   total_nbfree != ms->super.cstotal_nbfree ||
			   total_nifree != ms->super.cstotal_nifree ||
			   total_nffree != ms->super.cstotal_nffree)) {
		/* Handles the ms condition. */
		if (!ms->journal_enabled) {
			error = EINVAL;
		} else {
			ms->super.cstotal_ndir = total_ndir;
			ms->super.cstotal_nbfree = total_nbfree;
			ms->super.cstotal_nifree = total_nifree;
			ms->super.cstotal_nffree = total_nffree;
			summaries_rebuilt = 1;
		}
	}

	/* Checks the operation status. */
	if (error == 0)
		error = ufs_quota_rebuild(mountp);

	/* Checks the operation status. */
	if (error == 0)
		error = load_cg_locked(mountp, 0);

	/* Checks the operation status. */
	if (error != 0) {
		mountp->m_data = NULL;
		ufs_state_free(ms);

		/* Returns the computed result. */
		return error;
	}

	/*
	 * Preserve the ordinary persistent upper's validated reopen policy.
	 * Private root mounts remain mounted through shutdown sync, so a dirty
	 * marker alone is not proof of damaged metadata. Keep the structural,
	 * allocation-summary and root checks as mount admission gates.
	 */
	if ((mountp->m_flags & MOUNT_READ_ONLY) == 0) {
		/* Handles the mountp condition. */
		if ((mountp->m_disk->d_flags & DISK_READ_ONLY) != 0) {
			mountp->m_data = NULL;
			ufs_state_free(ms);

			/* Returns the computed result. */
			return EROFS;
		}
		ms->writable = 1;

		/* Handles the summaries rebuilt condition. */
		if (summaries_rebuilt) {
			error = write_super_summaries(mountp);

			/* Checks the operation status. */
			if (error != 0) {
				mountp->m_data = NULL;
				ufs_state_free(ms);

				/* Returns the computed result. */
				return error;
			}
		}
	}
	error = load_inode(mountp, UFS_ROOT_INO, &root);

	/* Checks the operation status. */
	if (error || root->i_type != INODE_DIR) {
		/* Checks the operation status. */
		if (!error) {
			root->i_flags |= INODE_DEAD;
			inode_release(root);
		}
		mountp->m_data = NULL;
		ufs_state_free(ms);

		/* Returns the computed result. */
		return error ? error : EIO;
	}

	/*
 * A malformed root must not become the namespace anchor.  Validate the
	 * mandatory entries while the mount is still private and unpublished.
	 */
	error = next_dirent(root, &cursor, &number, &type, name);

	/* Checks the operation status. */
	if (error == 0 && (number != UFS_ROOT_INO || strcmp(name, ".") != 0))
		error = EIO;

	/* Checks the operation status. */
	if (error == 0)
		error = next_dirent(root, &cursor, &number, &type, name);

	/* Checks the operation status. */
	if (error == 0 && (number != UFS_ROOT_INO || strcmp(name, "..") != 0))
		error = EIO;

	/* Checks the operation status. */
	if (error == 0)
		error = ufs_quota_load(mountp, root);

	/* Checks the operation status. */
	if (error == 0)
		error = orphan_recover(mountp);

	/* Checks the operation status. */
	if (error != 0) {
		root->i_flags |= INODE_DEAD;
		inode_release(root);
		mountp->m_data = NULL;
		ufs_state_free(ms);

		/* Returns the computed result. */
		return error;
	}

	/*
 * Do not dirty an image until every read-only mount validation,
	 * including the root inode, has succeeded. */
	if (ms->writable) {
		error = ufs_write_clean(mountp, 0);

		/* Checks the operation status. */
		if (error) {
			root->i_flags |= INODE_DEAD;
			inode_release(root);
			mountp->m_data = NULL;
			ufs_state_free(ms);

			/* Returns the computed result. */
			return error;
		}
	}
	root->i_flags |= INODE_ROOT;
	mountp->m_root = root;

	/* Checks the operation status. */
	if (ms->snapshot.active && (error = snapshot_disk_publish(ms)) != 0) {
		mountp->m_root = NULL;
		root->i_flags |= INODE_DEAD;
		inode_release(root);
		mountp->m_data = NULL;
		ufs_state_free(ms);

		/* Returns the computed result. */
		return error;
	}

	/* Reports successful completion. */
	return 0;
}
/* Excludes metadata admission until all previously published homes are durable. */
static int
ufs_sync(
	struct mount *mountp)
{
	struct ufs_mount_state *ms;
	int error;

	/* Checks the state result. */
	if (mountp == NULL || (ms = state(mountp)) == NULL)
		return EINVAL;
	mutex_lock(&ms->lock);
	mutex_lock(&ms->journal_lock);
	error = journal_checkpoint_locked(mountp);
	mutex_unlock(&ms->journal_lock);

	/* Checks the operation status. */
	if (error == 0)
		error = disk_sync(mountp->m_disk);
	mutex_unlock(&ms->lock);

	/* Returns the computed result. */
	return error;
}
static int ufs_statvfs(struct mount *mountp, struct statvfs *result);

/* Supports the ufs statvfs operation. */
static int
ufs_statvfs(
	struct mount *mountp,
	struct statvfs *result)
{
	struct ufs_mount_state *ms = state(mountp);
	uint64_t nbfree, nffree, nifree;

	/* Handles the ms availability. */
	if (ms == NULL || result == NULL)
		return EINVAL;
	mutex_lock(&ms->lock);
	nbfree = ms->super.cstotal_nbfree;
	nffree = ms->super.cstotal_nffree;
	nifree = ms->super.cstotal_nifree;
	memset(result, 0, sizeof(*result));
	result->f_bsize = ms->super.bsize;
	result->f_frsize = ms->super.fsize;
	result->f_blocks = ms->super.dsize;
	result->f_bfree = (uint64_t)nbfree * ms->super.frag + nffree;
	result->f_bavail = result->f_bfree;
	result->f_files = (uint64_t)ms->super.ncg * ms->super.ipg;
	result->f_ffree = nifree;
	result->f_favail = nifree;
	result->f_namemax = NAME_MAX;
	mutex_unlock(&ms->lock);

	/* Reports successful completion. */
	return 0;
}
static int ufs_quotactl(struct mount *mountp, struct quota_control *request);

/* Supports the ufs quotactl operation. */
static int
ufs_quotactl(
	struct mount *mountp,
	struct quota_control *request)
{
	int function_result;
	struct ufs_mount_state *ms = state(mountp);
	struct quota_record record;
	enum quota_type type;
	uint8_t *saved = NULL;
	size_t saved_length = 0;
	int enabled, error, mutating = 0;

	/* Handles the ms availability. */
	if (ms == NULL || request == NULL || request->type > ZEDBSD_QUOTA_GROUP)
		return EINVAL;
	type = request->type == ZEDBSD_QUOTA_USER ? QUOTA_USER : QUOTA_GROUP;
	/* Dispatch the selected operation case. */
	switch (request->command) {
	case ZEDBSD_QUOTA_GET:
		error = quota_get(&ms->quota, type, request->id, &record);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		error = quota_enabled(&ms->quota, type, &enabled);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		request->flags = enabled ? ZEDBSD_QUOTA_F_ENABLED : 0;
		request->block_soft = record.block_soft;
		request->block_hard = record.block_hard;
		request->inode_soft = record.inode_soft;
		request->inode_hard = record.inode_hard;
		request->blocks = record.blocks;
		request->inodes = record.inodes;
		request->block_deadline = record.block_deadline;
		request->inode_deadline = record.inode_deadline;

		/* Obtains the quota get grace result. */
		function_result =
			quota_get_grace(&ms->quota, &request->grace_seconds);

		/* Returns the computed result. */
		return function_result;
	case ZEDBSD_QUOTA_SET:
		/* Handles the ms condition. */
		if (!ms->writable)
			return EROFS;
		mutating = 1;
		break;
	case ZEDBSD_QUOTA_ENABLE:
	case ZEDBSD_QUOTA_DISABLE:
		/* Handles the ms condition. */
		if (!ms->writable)
			return EROFS;
		mutating = 1;
		break;
	case ZEDBSD_QUOTA_SYNC:
		/* Computes the function result. */
		function_result = !ms->writable ? disk_sync(mountp->m_disk)
						: ufs_quota_persist(mountp);

		/* Returns the computed result. */
		return function_result;
	default:
		/* Returns the computed result. */
		return EINVAL;
	}

	/* Handles the mutating condition. */
	if (mutating) {
		saved = kern_malloc(ms->super.bsize);

		/* Handles the saved availability. */
		if (saved == NULL)
			return ENOMEM;
		error = quota_export_config(&ms->quota, saved, ms->super.bsize,
					    &saved_length);

		/* Checks the operation status. */
		if (error != 0) {
			kern_free(saved);

			/* Returns the computed result. */
			return error;
		}
	}
	/* Dispatch the selected operation case. */
	switch (request->command) {
	case ZEDBSD_QUOTA_SET:
		memset(&record, 0, sizeof(record));
		record.id = request->id;
		record.block_soft = request->block_soft;
		record.block_hard = request->block_hard;
		record.inode_soft = request->inode_soft;
		record.inode_hard = request->inode_hard;
		error = quota_set(&ms->quota, type, &record);

		/* Checks the operation status. */
		if (error == 0 && request->grace_seconds != 0) {
			error = quota_set_grace(&ms->quota,
						request->grace_seconds);
		}
		break;
	case ZEDBSD_QUOTA_ENABLE:
		error = quota_enable(&ms->quota, type, 1);
		break;
	case ZEDBSD_QUOTA_DISABLE:
		error = quota_enable(&ms->quota, type, 0);
		break;
	default:
		error = EINVAL;
		break;
	}

	/* Checks the operation status. */
	if (error == 0)
		error = ufs_quota_persist(mountp);

	/* Checks the operation status. */
	if (error != 0 &&
	    (quota_import_config(&ms->quota, saved, saved_length) != 0))
		ms->writable = 0;
	kern_free(saved);

	/* Returns the computed result. */
	return error;
}
static int ufs_snapshotctl(struct mount *mountp, struct snapshot_control *request);

/* Supports the ufs snapshotctl operation. */
static int
ufs_snapshotctl(
	struct mount *mountp,
	struct snapshot_control *request)
{
	struct ufs_mount_state *ms = state(mountp);
	int error = 0;

	/* Handles the ms availability. */
	if (ms == NULL || request == NULL)
		return EINVAL;
	memset(request->device, 0, sizeof(request->device));

	/* Handles the ms condition. */
	if (!ms->snapshot_available)
		return EOPNOTSUPP;
	/* Dispatch the selected operation case. */
	switch (request->command) {
	case ZEDBSD_SNAPSHOT_CREATE:
		/* Handles the ms condition. */
		if (!ms->writable)
			return EROFS;
		mutex_lock(&ms->lock);
		mutex_lock(&ms->snapshot_lock);
		mutex_lock(&ms->journal_lock);
		error = journal_checkpoint_locked(mountp);
		mutex_unlock(&ms->journal_lock);

		/* Checks the operation status. */
		if (error == 0)
			error = disk_sync(mountp->m_disk);

		/* Checks the operation status. */
		if (error == 0)
			error = drv_ufs_snapshot_create(&ms->snapshot);
		mutex_unlock(&ms->snapshot_lock);
		mutex_unlock(&ms->lock);

		/* Checks the operation status. */
		if (error != 0)
			return error;

		/* Checks the operation status. */
		if (error == 0)
			error = snapshot_disk_publish(ms);

		/* Checks the operation status. */
		if (error != 0 && ms->snapshot.active) {
			mutex_lock(&ms->snapshot_lock);
			(void)drv_ufs_snapshot_delete(&ms->snapshot);
			mutex_unlock(&ms->snapshot_lock);
		}
		break;
	case ZEDBSD_SNAPSHOT_DELETE:
		/* Handles the ms condition. */
		if (!ms->writable)
			return EROFS;

		/* Handles the ms condition. */
		if (!ms->snapshot.active)
			return ENOENT;
		error = snapshot_disk_remove(ms);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		mutex_lock(&ms->snapshot_lock);
		error = drv_ufs_snapshot_delete(&ms->snapshot);
		mutex_unlock(&ms->snapshot_lock);

		/* Checks the operation status. */
		if (error != 0)
			(void)snapshot_disk_publish(ms);
		break;
	case ZEDBSD_SNAPSHOT_STATUS:
		break;
	default:
		/* Returns the computed result. */
		return EINVAL;
	}

	/* Checks the operation status. */
	if (error != 0)
		return error;
	request->flags = ms->snapshot.active ? ZEDBSD_SNAPSHOT_F_ACTIVE : 0;
	request->captured_sectors = ms->snapshot.next_record;
	request->capacity_sectors = ms->snapshot.max_records;

	/* Handles the snapshot disk availability. */
	if (ms->snapshot_disk != NULL) {
		memcpy(request->device, ms->snapshot_disk->d_name,
		       sizeof(request->device));
	}

	/* Reports successful completion. */
	return 0;
}
static int ufs_prepare_unmount(struct mount *mountp);

/* Supports the ufs prepare unmount operation. */
static int
ufs_prepare_unmount(
	struct mount *mountp)
{
	int function_result;
	struct ufs_mount_state *ms = state(mountp);

	/* Handles the ms availability. */
	if (ms != NULL && ms->snapshot_disk != NULL)
		return EBUSY;

	/* Computes the function result. */
	function_result =
		ms != NULL && ms->writable ? ufs_write_clean(mountp, 1) : 0;

	/* Returns the computed result. */
	return function_result;
}
static void ufs_unmount(struct mount *mountp);

/* Supports the ufs unmount operation. */
static void
ufs_unmount(
	struct mount *mountp)
{
	struct ufs_mount_state *ms;

	/* Handles the mountp condition. */
	if (mountp && mountp->m_data) {
		ms = state(mountp);
		ufs_state_free(ms);
		mountp->m_data = NULL;
	}
}

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

	/* Checks the current offset. */
	if (offset < 0 || length == 0 || inode->i_type != INODE_REG)
		return 0;

	/* Serializes the allocation proof with backend mutations. */
	mutex_lock(&inode->i_lock);

	/* Handles the ms condition. */
	if (!ms->writable || offset > inode->i_size ||
	    (uint64_t)length > (uint64_t)(inode->i_size - offset)) {
		mutex_unlock(&inode->i_lock);

		/* Reports successful completion. */
		return 0;
	}

	/* Requires every touched block to have a published allocation. */
	logical = (uint64_t)offset / ms->super.bsize;
	last = ((uint64_t)offset + length - 1U) / ms->super.bsize;
	/* Process each element required by the operation. */
	for (; logical <= last; logical++) {
		error = bmap(inode, logical, &fragment);

		/* Checks the operation status. */
		if (error != 0 || fragment == 0) {
			mutex_unlock(&inode->i_lock);

			/* Returns the computed result. */
			return error != 0 ? -error : 0;
		}
	}

	/* Reports an entirely allocated overwrite. */
	mutex_unlock(&inode->i_lock);

	/* Reports operation failure. */
	return 1;
}

const struct filesystem_type drv_ufs_filesystem_type = {
	.writeback_range = ufs_writeback_range,
	.fs_name = "ufs",
	.probe = ufs_probe,
	.identify = drv_ufs_identify,
	.mount = ufs_mount_impl,
	.sync = ufs_sync,
	.statvfs = ufs_statvfs,
	.quotactl = ufs_quotactl,
	.snapshotctl = ufs_snapshotctl,
	.prepare_unmount = ufs_prepare_unmount,
	.unmount = ufs_unmount,
	.alloc_inode = ufs_alloc_inode,
	.free_inode = ufs_free_inode,
};
/* End consolidated ufs-vfs.c. */

/* Begin consolidated ufs-endian.c. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

/*
 * Implements the drv ufs get16 operation.
 */
uint16_t
drv_ufs_get16(
	const void *buffer,
	size_t offset,
	int swapped)
{
	const uint8_t *p = (const uint8_t *)buffer + offset;

	/* Returns the computed result. */
	return swapped ? ((uint16_t)p[0] << 8) | p[1]
		       : (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
/*
 * Implements the drv ufs get32 operation.
 */
uint32_t
drv_ufs_get32(
	const void *buffer,
	size_t offset,
	int swapped)
{
	const uint8_t *p = (const uint8_t *)buffer + offset;

	/* Returns the computed result. */
	return swapped ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
				 ((uint32_t)p[2] << 8) | p[3]
		       : (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
				 ((uint32_t)p[2] << 16) |
				 ((uint32_t)p[3] << 24);
}
/*
 * Implements the drv ufs get64 operation.
 */
uint64_t
drv_ufs_get64(
	const void *buffer,
	size_t offset,
	int swapped)
{
	uint64_t low, high;

	/* Handles the swapped condition. */
	if (swapped) {
		high = drv_ufs_get32(buffer, offset, 1);
		low = drv_ufs_get32(buffer, offset + 4U, 1);
	} else {
		low = drv_ufs_get32(buffer, offset, 0);
		high = drv_ufs_get32(buffer, offset + 4U, 0);
	}

	/* Returns the computed result. */
	return low | (high << 32);
}
/*
 * Implements the drv ufs put16 operation.
 */
void
drv_ufs_put16(
	void *buffer,
	size_t offset,
	uint16_t value,
	int swapped)
{
	uint8_t *p = (uint8_t *)buffer + offset;

	/* Handles the swapped condition. */
	if (swapped) {
		p[0] = value >> 8;
		p[1] = value;
	} else {
		p[0] = value;
		p[1] = value >> 8;
	}
}
/*
 * Implements the drv ufs put32 operation.
 */
void
drv_ufs_put32(
	void *buffer,
	size_t offset,
	uint32_t value,
	int swapped)
{
	uint8_t *p = (uint8_t *)buffer + offset;

	/* Handles the swapped condition. */
	if (swapped) {
		p[0] = value >> 24;
		p[1] = value >> 16;
		p[2] = value >> 8;
		p[3] = value;
	} else {
		p[0] = value;
		p[1] = value >> 8;
		p[2] = value >> 16;
		p[3] = value >> 24;
	}
}
/*
 * Implements the drv ufs put64 operation.
 */
void
drv_ufs_put64(
	void *buffer,
	size_t offset,
	uint64_t value,
	int swapped)
{
	/* Handles the swapped condition. */
	if (swapped) {
		drv_ufs_put32(buffer, offset, (uint32_t)(value >> 32), 1);
		drv_ufs_put32(buffer, offset + 4U, (uint32_t)value, 1);
	} else {
		drv_ufs_put32(buffer, offset, (uint32_t)value, 0);
		drv_ufs_put32(buffer, offset + 4U, (uint32_t)(value >> 32), 0);
	}
}
/* End consolidated ufs-endian.c. */

/* Begin consolidated ufs-journal.c. */
/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <errno.h>
#include <string.h>

#define SECTOR_SIZE 512U
#define DESC_MAGIC 0x4a534655U	 /* UFSJ */
#define COMMIT_MAGIC 0x434a4655U /* UFJC */
#define JOURNAL_VERSION 2U

static uint32_t checksum(const void *buffer, size_t length);

/* Supports the checksum operation. */
static uint32_t
checksum(
	const void *buffer,
	size_t length)
{
	const uint8_t *bytes = buffer;
	uint32_t value = 2166136261U;
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < length; index++) {
		value ^= bytes[index];
		value *= 16777619U;
	}

	/* Returns the computed result. */
	return value;
}

static void put32(uint8_t *p, uint32_t v);

/* Supports the put32 operation. */
static void
put32(
	uint8_t *p,
	uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v);

/* Supports the put64 operation. */
static void
put64(
	uint8_t *p,
	uint64_t v)
{
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}
static uint32_t get32(const uint8_t *p);

/* Supports the get32 operation. */
static uint32_t
get32(
	const uint8_t *p)
{
	/* Returns the computed result. */
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p);

/* Supports the get64 operation. */
static uint64_t
get64(
	const uint8_t *p)
{
	uint64_t function_result;

	/* Computes the function result. */
	function_result = get32(p) | (uint64_t)get32(p + 4) << 32;

	/* Returns the computed result. */
	return function_result;
}

static int clear_record(struct ufs_journal *journal, uint64_t sector);

/* Supports the clear record operation. */
static int
clear_record(
	struct ufs_journal *journal,
	uint64_t sector)
{
	int function_result;
	uint8_t zero[SECTOR_SIZE];
	int error;

	memset(zero, 0, sizeof(zero));
	error = journal->io.write(journal->io.context, sector, 1, zero);

	/* Computes the function result. */
	function_result =
		error != 0 ? error : journal->io.flush(journal->io.context);

	/* Returns the computed result. */
	return function_result;
}

#define GROUP_VERSION JOURNAL_VERSION
#define GROUP_HEADER 32U
#define GROUP_ENTRY 16U
#define IMAGE_READERS_CLOSED (UINT32_C(1) << 31)

static uint32_t group_checksum(uint8_t *descriptor);
static int journal_finish(struct ufs_journal *journal);
static int group_validate(struct ufs_journal *journal, uint8_t *descriptor);
static int journal_replay(struct ufs_journal *journal, uint64_t expected_sequence, uint32_t expected_digest, int apply, uint8_t *view);
static void journal_close_views(struct ufs_journal *journal);
static int journal_view_transfer(const struct ufs_journal_view *view, uint64_t first, uint32_t count, void *buffer, int copy);

/*
 * Initializes the bounded multi-target journal using checked volume geometry.
 */
int
drv_ufs_journal_init(
	struct ufs_journal *journal,
	const struct ufs_journal_io *io,
	uint64_t first,
	uint32_t count,
	uint64_t home_sectors)
{
	/* Checks media callbacks and disjoint home/locator/journal geometry. */
	if (journal == NULL || io == NULL || io->read == NULL ||
	    io->write == NULL || io->flush == NULL || count < 3U ||
	    first > UINT64_MAX - count || home_sectors == 0 ||
	    home_sectors >= first)

		/* Returns the computed result. */
		return EINVAL;
	memset(journal, 0, sizeof(*journal));
	journal->io = *io;
	journal->first_sector = first;
	journal->sector_count = count;
	journal->next_sequence = 1;
	journal->home_sectors = home_sectors;
	journal->image_readers = IMAGE_READERS_CLOSED;

	/* Reports successful completion. */
	return 0;
}

/*
 * Binds owner-accounted redo storage before any transaction is admitted.
 */
int
drv_ufs_journal_bind_image(
	struct ufs_journal *journal,
	void *image,
	size_t bytes)
{
	/*
 * Rejects incomplete storage and live ownership without changing the
	 * binding. */
	if (journal == NULL || (image == NULL && bytes != 0) ||
	    (image != NULL && bytes < UFS_JOURNAL_IMAGE_BYTES))

		/* Returns the computed result. */
		return EINVAL;

	/* Handles the journal condition. */
	if (journal->pending_sequence != 0)
		return EBUSY;
	journal_close_views(journal);

	/* Handles the drv ufs journal views busy condition. */
	if (drv_ufs_journal_views_busy(journal))
		return EBUSY;
	journal->image = image;
	journal->image_valid = 0;

	/*
 * Reports exclusive storage ready for the next publication or boot
	 * replay. */
	return 0;
}

/*
 * Publishes durable redo without installing any home extent.
 */
int
drv_ufs_journal_publishv(
	struct ufs_journal *journal,
	const struct ufs_journal_extent *extents,
	unsigned count)
{
	uint8_t descriptor[SECTOR_SIZE];
	uint8_t commit[SECTOR_SIZE];
	uint8_t *entry;
	uint64_t cursor;
	uint32_t total;
	unsigned index;
	int error;

	/*
 * Validates every extent and the complete footprint before modifying
	 * the slot. */
	if (journal == NULL || extents == NULL || count == 0 ||
	    count > UFS_JOURNAL_EXTENTS)

		/* Returns the computed result. */
		return EINVAL;

	/* Handles the journal condition. */
	if (journal->poisoned)
		return EIO;

	/* Handles the journal condition. */
	if (journal->pending_sequence != 0)
		return EBUSY;

	/* Handles the drv ufs journal views busy condition. */
	if (drv_ufs_journal_views_busy(journal))
		return EBUSY;

	/* Handles the journal condition. */
	if (journal->next_sequence == 0 || journal->next_sequence == UINT64_MAX)
		return EOVERFLOW;
	memset(descriptor, 0, sizeof(descriptor));
	put32(descriptor, DESC_MAGIC);
	put32(descriptor + 4, GROUP_VERSION);
	put64(descriptor + 8, journal->next_sequence);
	put32(descriptor + 16, count);
	total = 0;
	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		/* Handles the payload availability. */
		if (extents[index].payload == NULL ||
		    extents[index].sectors == 0 ||
		    extents[index].sectors > UFS_JOURNAL_GROUP_SECTORS - total)

			/* Returns the computed result. */
			return EINVAL;
		entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
		put64(entry, extents[index].target);
		put32(entry + 8, extents[index].sectors);
		total += extents[index].sectors;
	}
	put32(descriptor + 20, total);
	put32(descriptor + 28, group_checksum(descriptor));
	error = group_validate(journal, descriptor);

	/* Checks the operation status. */
	if (error != 0)
		return EINVAL;

	/*
 * Refuses a live slot instead of overwriting committed or unresolved
	 * ownership. */
	error = journal->io.read(journal->io.context, journal->first_sector, 1,
				 commit);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Checks the get32 result. */
	if (get32(commit) != 0)
		return EBUSY;

	/*
 * Binds each payload only after all addresses and lengths have passed
	 * validation. */
	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
		put32(entry + 12,
		      checksum(extents[index].payload,
			       (size_t)extents[index].sectors * SECTOR_SIZE));
	}
	put32(descriptor + 28, group_checksum(descriptor));
	memset(commit, 0, sizeof(commit));
	put32(commit, COMMIT_MAGIC);
	put32(commit + 4, GROUP_VERSION);
	put64(commit + 8, journal->next_sequence++);
	put32(commit + 16, get32(descriptor + 28));
	put32(commit + 24, checksum(commit, 24));

	/*
 * Retains uncertain ownership before the first write can reach media.
	 */
	journal->pending_sequence = get64(descriptor + 8);
	journal->pending_digest = get32(descriptor + 28);
	journal->pending_ready = 0;
	journal->image_valid = 0;

	/*
 * Makes old commit evidence unreachable before publishing immutable
	 * redo bytes. */
	error = clear_record(journal, journal->first_sector +
					      journal->sector_count - 1U);

	/* Checks the operation status. */
	if (error == 0) {
		error = journal->io.write(journal->io.context,
					  journal->first_sector, 1, descriptor);
	}
	cursor = journal->first_sector + 1U;
	/* Process each remaining element. */
	for (index = 0; error == 0 && index < count; index++) {
		error = journal->io.write(journal->io.context, cursor,
					  extents[index].sectors,
					  extents[index].payload);
		cursor += extents[index].sectors;
	}

	/* Checks the operation status. */
	if (error == 0)
		error = journal->io.flush(journal->io.context);

	/* Checks the operation status. */
	if (error == 0) {
		error = journal->io.write(journal->io.context,
					  journal->first_sector +
						  journal->sector_count - 1U,
					  1, commit);
	}

	/* Checks the operation status. */
	if (error == 0)
		error = journal->io.flush(journal->io.context);

	/*
 * Confirms this exact commit before exposing redo to metadata readers.
	 */
	if (error == 0) {
		error = journal_replay(journal, journal->pending_sequence,
				       journal->pending_digest, 0, NULL);
	}

	/* Returns the computed result. */
	return error;
}

/*
 * Publishes and checkpoints one group for synchronous callers.
 */
int
drv_ufs_journal_commitv(
	struct ufs_journal *journal,
	const struct ufs_journal_extent *extents,
	unsigned count)
{
	int error;

	/*
 * Never recover a different caller's already pending group as a side
	 * effect. */
	if (journal != NULL && journal->pending_sequence != 0)
		return journal->poisoned ? EIO : EBUSY;

	/*
 * Preserves the operation error even when recovery establishes a safe
	 * slot. */
	error = drv_ufs_journal_publishv(journal, extents, count);

	/* Checks the operation status. */
	if (error == 0)
		error = drv_ufs_journal_checkpoint(journal);

	/* Checks the operation status. */
	if (error != 0 && journal != NULL && journal->pending_sequence != 0 &&
	    drv_ufs_journal_replay(journal) != 0) {
		journal->poisoned = 1;
		journal_close_views(journal);
	}

	/* Returns the computed result. */
	return error;
}

/*
 * Installs a verified pending group while retaining its witness on failure.
 */
int
drv_ufs_journal_checkpoint(
	struct ufs_journal *journal)
{
	int function_result;

	/*
 * Requires recovery to resolve an interrupted publication before normal
	 * reuse. */
	if (journal == NULL)
		return EINVAL;

	/* Handles the journal condition. */
	if (journal->pending_sequence == 0)
		return 0;

	/* Handles the journal condition. */
	if (!journal->pending_ready)
		return EBUSY;

	/* Handles the journal condition. */
	if (journal->pending_clearing) {
		/* Obtains the journal finish result. */
		function_result = journal_finish(journal);

		/* Returns the computed result. */
		return function_result;
	}

	/* Obtains the journal replay result. */
	function_result = journal_replay(journal, journal->pending_sequence,
					 journal->pending_digest, 1, NULL);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Resolves retained checkpoint work without hiding an initial device failure.
 */
int
drv_ufs_journal_drain(
	struct ufs_journal *journal)
{
	int error;

	/*
 * A poisoned owner requires remount recovery, even if no slot is
	 * pending. */
	if (journal == NULL)
		return EINVAL;

	/* Handles the journal condition. */
	if (journal->poisoned)
		return EIO;
	error = drv_ufs_journal_checkpoint(journal);

	/* Checks the operation status. */
	if (error != 0 && journal->pending_sequence != 0 &&
	    drv_ufs_journal_replay(journal) != 0) {
		journal->poisoned = 1;
		journal_close_views(journal);
	}

	/* Returns the computed result. */
	return error;
}

/*
 * Uses the grouped durability protocol for a single extent.
 */
int
drv_ufs_journal_commit(
	struct ufs_journal *journal,
	uint64_t target,
	const void *payload,
	uint32_t sectors)
{
	int function_result;
	struct ufs_journal_extent extent;

	/*
 * Shares validation, ordering and recovery with multi-target callers.
	 */
	extent.target = target;
	extent.sectors = sectors;
	extent.payload = payload;

	/* Obtains the drv ufs journal commitv result. */
	function_result = drv_ufs_journal_commitv(journal, &extent, 1);

	/* Returns the computed result. */
	return function_result;
}

/* Checks the entire descriptor without including its own checksum field. */
static uint32_t
group_checksum(
	uint8_t *descriptor)
{
	uint32_t saved;
	uint32_t digest;

	/* Restores the immutable caller image after sampling its checksum. */
	saved = get32(descriptor + 28);
	put32(descriptor + 28, 0);
	digest = checksum(descriptor, SECTOR_SIZE);
	put32(descriptor + 28, saved);

	/* Returns the computed result. */
	return digest;
}

/* Rejects malformed, overlapping or out-of-volume redo before any home write. */
static int
group_validate(
	struct ufs_journal *journal,
	uint8_t *descriptor)
{
	uint64_t target;
	uint64_t previous;
	uint32_t sectors;
	uint32_t total;
	uint32_t count;
	unsigned index;
	unsigned other;
	const uint8_t *entry;
	const uint8_t *prior;

	/* Validates the fixed header and bounded record count. */
	count = get32(descriptor + 16);

	/* Checks the get32 result. */
	if (get32(descriptor) != DESC_MAGIC ||
	    get32(descriptor + 4) != GROUP_VERSION ||
	    get64(descriptor + 8) == 0 || get64(descriptor + 8) == UINT64_MAX ||
	    count == 0 || count > UFS_JOURNAL_EXTENTS ||
	    get32(descriptor + 28) != group_checksum(descriptor))

		/* Returns the computed result. */
		return EIO;

	/*
 * Checks all addresses before reading payloads or changing persistent
	 * homes. */
	total = 0;
	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
		target = get64(entry);
		sectors = get32(entry + 8);

		/* Handles the sectors condition. */
		if (sectors == 0 ||
		    sectors > UFS_JOURNAL_GROUP_SECTORS - total ||
		    target >= journal->home_sectors ||
		    sectors > journal->home_sectors - target)

			/* Returns the computed result. */
			return EIO;
		/* Process each remaining element. */
		for (other = 0; other < index; other++) {
			prior = descriptor + GROUP_HEADER + other * GROUP_ENTRY;
			previous = get64(prior);

			/* Checks the get32 result. */
			if (target < previous + get32(prior + 8) &&
			    previous < target + sectors)

				/* Returns the computed result. */
				return EIO;
		}
		total += sectors;
	}

	/* Checks the get32 result. */
	if (total != get32(descriptor + 20) ||
	    total > journal->sector_count - 2U)

		/* Returns the computed result. */
		return EIO;

	/* Reports successful completion. */
	return 0;
}

/*
 * Replays a committed group, validating every byte before touching homes.
 */
int
drv_ufs_journal_replay(
	struct ufs_journal *journal)
{
	int function_result;

	/*
 * Boot recovery may discard incomplete redo without claiming a new
	 * commit. */
	if (journal == NULL)
		return EINVAL;

	/* Handles the journal condition. */
	if (journal->pending_clearing) {
		/* Obtains the journal finish result. */
		function_result = journal_finish(journal);

		/* Returns the computed result. */
		return function_result;
	}

	/* Obtains the journal replay result. */
	function_result = journal_replay(
		journal, journal->pending_ready ? journal->pending_sequence : 0,
		journal->pending_ready ? journal->pending_digest : 0, 1, NULL);

	/* Returns the computed result. */
	return function_result;
}

/* Requires a caller's committed identity when checkpoint follows publication. */
static int
journal_replay(
	struct ufs_journal *journal,
	uint64_t expected_sequence,
	uint32_t expected_digest,
	int apply,
	uint8_t *view)
{
	int function_result;
	uint8_t descriptor[SECTOR_SIZE];
	uint8_t commit[SECTOR_SIZE];
	uint8_t sector[SECTOR_SIZE];
	const uint8_t *entry;
	uint64_t cursor;
	uint64_t sequence;
	uint32_t digest;
	uint32_t count;
	uint32_t offset;
	uint32_t closed;
	unsigned index;
	unsigned part;
	unsigned byte;
	int error;

	/* Refuses a missing owner before reading its reserved slot. */
	if (journal == NULL)
		return EINVAL;

	/*
 * Reuses only the immutable image of this owner's verified pending
	 * identity. */
	if (journal->image_valid) {
		memcpy(descriptor, journal->image, SECTOR_SIZE);
		sequence = get64(descriptor + 8);

		/* Checks the get32 result. */
		if (sequence != expected_sequence ||
		    get32(descriptor + 28) != expected_digest)

			/* Returns the computed result. */
			return EIO;
		count = get32(descriptor + 16);
	} else {
		/*
 * Retired readers still own the old bytes even after its slot
		 * was cleared. */
		if (drv_ufs_journal_views_busy(journal))
			return EBUSY;

		/*
 * An empty descriptor is the durable terminal state of the
		 * slot. */
		error = journal->io.read(journal->io.context,
					 journal->first_sector, 1, descriptor);

		/* Checks the operation status. */
		if (error != 0)
			return error;

		/* Checks the get32 result. */
		if (get32(descriptor) == 0) {
			/* Handles the expected sequence condition. */
			if (expected_sequence != 0)
				return EIO;
			journal->pending_sequence = 0;
			journal->pending_digest = 0;
			journal->pending_ready = 0;

			/* Reports successful completion. */
			return 0;
		}
		error = group_validate(journal, descriptor);

		/* Checks the operation status. */
		if (error != 0)
			return error;

		/*
 * A writer must verify its own group, not merely any valid redo
		 * transaction. */
		sequence = get64(descriptor + 8);

		/* Checks the get32 result. */
		if (expected_sequence != 0 &&
		    (sequence != expected_sequence ||
		     get32(descriptor + 28) != expected_digest))

			/* Returns the computed result. */
			return EIO;
		count = get32(descriptor + 16);
		error = journal->io.read(journal->io.context,
					 journal->first_sector +
						 journal->sector_count - 1U,
					 1, commit);

		/* Checks the operation status. */
		if (error != 0)
			return error;

		/*
 * Drops uncommitted redo, whose home blocks have never been
		 * installed. */
		if (get32(commit) != COMMIT_MAGIC ||
		    get32(commit + 4) != GROUP_VERSION ||
		    get64(commit + 8) != sequence ||
		    get32(commit + 16) != get32(descriptor + 28) ||
		    get32(commit + 24) != checksum(commit, 24)) {
			/* Handles the expected sequence condition. */
			if (expected_sequence != 0)
				return EIO;
			error = clear_record(journal, journal->first_sector);

			/* Checks the operation status. */
			if (error == 0) {
				journal->pending_sequence = 0;
				journal->pending_digest = 0;
				journal->pending_ready = 0;
			}

			/* Returns the computed result. */
			return error;
		}

		/*
 * Fetches one bounded immutable payload image when the owner
		 * supplied storage. */
		if (journal->image != NULL) {
			error = journal->io.read(journal->io.context,
						 journal->first_sector + 1U,
						 get32(descriptor + 20),
						 journal->image + SECTOR_SIZE);

			/* Checks the operation status. */
			if (error != 0)
				return error;
		}

		/* Validates all payload extents before the first home write. */
		offset = SECTOR_SIZE;
		cursor = journal->first_sector + 1U;
		/* Process each remaining element. */
		for (index = 0; index < count; index++) {
			entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
			digest = 2166136261U;
			/* Process each element required by the operation. */
			for (part = 0; part < get32(entry + 8); part++) {
				/* Handles the image availability. */
				if (journal->image != NULL) {
					memcpy(sector, journal->image + offset,
					       SECTOR_SIZE);
					offset += SECTOR_SIZE;
				} else {
					error = journal->io.read(
						journal->io.context, cursor++,
						1, sector);

					/* Checks the operation status. */
					if (error != 0)
						return error;
				}
				/* Process each remaining element. */
				for (byte = 0; byte < SECTOR_SIZE; byte++) {
					digest ^= sector[byte];
					digest *= 16777619U;
				}
			}

			/* Checks the get32 result. */
			if (digest != get32(entry + 12))
				return EIO;
		}

		/* Handles the image availability. */
		if (journal->image != NULL) {
			memcpy(journal->image, descriptor, SECTOR_SIZE);
			journal->image_valid = 1;
		}
	}

	/*
 * Keeps the verified identity strict across any later failed home
	 * installation. */
	journal->pending_sequence = sequence;
	journal->pending_digest = get32(descriptor + 28);
	journal->pending_ready = 1;
	journal->committed_sequence = sequence;
	journal->committed_digest = journal->pending_digest;

	/*
 * Opens acquisition once, after every immutable byte and witness is
	 * validated. */
	if (journal->image_valid && !journal->poisoned) {
		closed = IMAGE_READERS_CLOSED;

		(void)__atomic_compare_exchange_n(
			&journal->image_readers, &closed, 0, 0,
			__ATOMIC_RELEASE, __ATOMIC_RELAXED);
	}

	/* Handles the apply condition. */
	if (!apply) {
		/* Handles the view availability. */
		if (view != NULL)
			memcpy(view, descriptor, SECTOR_SIZE);

		/* Reports successful completion. */
		return 0;
	}

	/*
 * Installs checked homes, then releases the slot only after their flush
	 * succeeds. */
	offset = SECTOR_SIZE;
	cursor = journal->first_sector + 1U;
	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;

		/* Handles the journal condition. */
		if (journal->image_valid) {
			error = journal->io.write(
				journal->io.context, get64(entry),
				get32(entry + 8), journal->image + offset);

			/* Checks the operation status. */
			if (error != 0)
				return error;
			offset += get32(entry + 8) * SECTOR_SIZE;
			continue;
		}
		/* Process each element required by the operation. */
		for (part = 0; part < get32(entry + 8); part++) {
			error = journal->io.read(journal->io.context, cursor++,
						 1, sector);

			/* Checks the operation status. */
			if (error == 0) {
				error = journal->io.write(journal->io.context,
							  get64(entry) + part,
							  1, sector);
			}

			/* Checks the operation status. */
			if (error != 0)
				return error;
		}
	}
	error = journal->io.flush(journal->io.context);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/*
 * Homes are durable even if clearing the descriptor has an uncertain
	 * result. */
	journal->pending_clearing = 1;

	/* Obtains the journal finish result. */
	function_result = journal_finish(journal);

	/* Returns the computed result. */
	return function_result;
}

/* Retries only slot retirement after home durability is already established. */
static int
journal_finish(
	struct ufs_journal *journal)
{
	int error;

	/*
 * Keeps the home-durable witness until clearing also crosses its flush
	 * boundary. */
	error = clear_record(journal, journal->first_sector);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	journal_close_views(journal);

	/* Handles the journal condition. */
	if (journal->pending_sequence >= journal->next_sequence)
		journal->next_sequence = journal->pending_sequence + 1U;
	journal->pending_sequence = 0;
	journal->pending_digest = 0;
	journal->pending_ready = 0;
	journal->pending_clearing = 0;
	journal->image_valid = 0;

	/* Reports successful completion. */
	return 0;
}

/*
 * Reads coherent home sectors through a verified pending redo group.
 */
int
drv_ufs_journal_read(
	struct ufs_journal *journal,
	uint64_t first,
	uint32_t count,
	void *buffer)
{
	int function_result;
	uint8_t descriptor[SECTOR_SIZE];
	const uint8_t *entry;
	uint64_t cursor;
	uint64_t current;
	uint64_t source;
	uint64_t target;
	uint32_t sectors;
	uint32_t done;
	uint32_t run;
	unsigned index;
	int error;

	/* Bounds the complete caller buffer before issuing any partial read. */
	if (journal == NULL || buffer == NULL || count == 0 ||
	    count > UFS_JOURNAL_GROUP_SECTORS ||
	    first >= journal->home_sectors ||
	    count > journal->home_sectors - first)

		/* Returns the computed result. */
		return EINVAL;

	/* Handles the journal condition. */
	if (journal->poisoned)
		return EIO;

	/* Handles the journal condition. */
	if (journal->pending_sequence == 0 || journal->pending_clearing) {
		/* Computes the function result. */
		function_result = journal->io.read(journal->io.context, first,
						   count, buffer);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the journal condition. */
	if (!journal->pending_ready)
		return EBUSY;

	/*
 * Validates the entire pending group before exposing any of its
	 * payloads. */
	error = journal_replay(journal, journal->pending_sequence,
			       journal->pending_digest, 0, descriptor);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/*
 * Coalesces each home gap or redo extent instead of issuing one read
	 * per sector. */
	done = 0;
	/* Process each remaining element. */
	while (done < count) {
		current = first + done;
		source = current;
		run = count - done;
		cursor = journal->first_sector + 1U;
		/* Process each remaining element. */
		for (index = 0; index < get32(descriptor + 16); index++) {
			entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
			target = get64(entry);
			sectors = get32(entry + 8);

			/* Handles the current condition. */
			if (current >= target && current - target < sectors) {
				source = cursor + current - target;

				/* Handles the run condition. */
				if (run > sectors - (current - target)) {
					run = (uint32_t)(sectors -
							 (current - target));
				}
				break;
			}

			/* Handles the target condition. */
			if (target > current && target - current < run)
				run = (uint32_t)(target - current);
			cursor += sectors;
		}

		/* Handles the journal condition. */
		if (journal->image_valid &&
		    source >= journal->first_sector + 1U) {
			memcpy((uint8_t *)buffer + (size_t)done * SECTOR_SIZE,
			       journal->image +
				       (size_t)(source -
						journal->first_sector) *
					       SECTOR_SIZE,
			       (size_t)run * SECTOR_SIZE);
		} else {
			error = journal->io.read(
				journal->io.context, source, run,
				(uint8_t *)buffer + (size_t)done * SECTOR_SIZE);

			/* Checks the operation status. */
			if (error != 0)
				return error;
		}
		done += run;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Reports positive commit proof independently of the current pending slot.
 */
int
drv_ufs_journal_committed(
	const struct ufs_journal *journal,
	uint64_t sequence,
	uint32_t digest)
{
	/*
 * Rejects absent and unissued witnesses without confusing them with a
	 * commit. */
	if (journal == NULL || sequence == 0)
		return 0;

	/* Handles the journal condition. */
	if (journal->committed_sequence != sequence)
		return 0;

	/* Handles the journal condition. */
	if (journal->committed_digest != digest)
		return 0;

	/* Reports operation failure. */
	return 1;
}

/* Closes new acquisition without revoking readers that already own the image. */
static void
journal_close_views(
	struct ufs_journal *journal)
{
	(void)__atomic_fetch_or(&journal->image_readers, IMAGE_READERS_CLOSED,
				__ATOMIC_ACQ_REL);
}

/*
 * Stops admission for an owner that will drain readers before destroying backing.
 */
void
drv_ufs_journal_views_close(
	struct ufs_journal *journal)
{
	/* Handles the journal availability. */
	if (journal != NULL)
		journal_close_views(journal);
}

/*
 * Reports backing ownership independently of whether the durable slot is empty.
 */
int
drv_ufs_journal_views_busy(
	const struct ufs_journal *journal)
{
	uint32_t readers;

	/* Handles the journal availability. */
	if (journal == NULL)
		return 0;
	readers = __atomic_load_n(&journal->image_readers, __ATOMIC_ACQUIRE);

	/* Returns the computed result. */
	return (readers & ~IMAGE_READERS_CLOSED) != 0;
}

/*
 * Pins one validated generation without waiting for checkpoint device I/O.
 */
int
drv_ufs_journal_view_acquire(
	struct ufs_journal *journal,
	struct ufs_journal_view *view)
{
	uint32_t readers;

	/*
 * Requires a fresh handle so repeated acquisition cannot lose a
	 * reference. */
	if (journal == NULL || view == NULL)
		return EINVAL;

	/* Handles the journal availability. */
	if (view->journal != NULL)
		return EBUSY;
	readers = __atomic_load_n(&journal->image_readers, __ATOMIC_ACQUIRE);
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the readers condition. */
		if ((readers & IMAGE_READERS_CLOSED) != 0)
			return ENOENT;

		/* Handles the readers condition. */
		if (readers == IMAGE_READERS_CLOSED - 1U)
			return EOVERFLOW;

		/* Checks the atomic compare exchange n result. */
		if (__atomic_compare_exchange_n(
			    &journal->image_readers, &readers, readers + 1U, 0,
			    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			break;
	}

	/* The acquired count prevents pointer rebinding and payload reuse. */
	view->journal = journal;
	view->image = journal->image;
	view->sequence = get64(view->image + 8);
	view->home_sectors = journal->home_sectors;

	/* Reports successful completion. */
	return 0;
}

/* Walks only immutable redo extents; uncovered home ranges never cause I/O. */
static int
journal_view_transfer(
	const struct ufs_journal_view *view,
	uint64_t first,
	uint32_t count,
	void *buffer,
	int copy)
{
	const uint8_t *entry;
	uint64_t current;
	uint64_t target;
	uint32_t done;
	uint32_t offset;
	uint32_t sectors;
	uint32_t run;
	unsigned index;

	/* Resolves every requested segment against the pinned generation. */
	done = 0;
	/* Process each remaining element. */
	while (done < count) {
		current = first + done;
		offset = SECTOR_SIZE;
		run = 0;
		/* Process each remaining element. */
		for (index = 0; index < get32(view->image + 16); index++) {
			entry = view->image + GROUP_HEADER +
				index * GROUP_ENTRY;
			target = get64(entry);
			sectors = get32(entry + 8);

			/* Handles the current condition. */
			if (current >= target && current - target < sectors) {
				run = sectors - (uint32_t)(current - target);

				/* Handles the run condition. */
				if (run > count - done)
					run = count - done;
				offset += (uint32_t)(current - target) *
					  SECTOR_SIZE;
				break;
			}
			offset += sectors * SECTOR_SIZE;
		}

		/* Handles the run condition. */
		if (run == 0)
			return ENOENT;

		/* Handles the copy condition. */
		if (copy) {
			memcpy((uint8_t *)buffer + (size_t)done * SECTOR_SIZE,
			       view->image + offset, (size_t)run * SECTOR_SIZE);
		}
		done += run;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Copies a fully covered range, leaving the destination intact on refusal.
 */
int
drv_ufs_journal_view_copy(
	const struct ufs_journal_view *view,
	uint64_t first,
	uint32_t count,
	void *buffer)
{
	int error;

	/* Bounds the entire request before resolving or copying any segment. */
	if (view == NULL || view->journal == NULL || buffer == NULL ||
	    count == 0 || count > UFS_JOURNAL_GROUP_SECTORS ||
	    first >= view->home_sectors || count > view->home_sectors - first)

		/* Returns the computed result. */
		return EINVAL;
	error = journal_view_transfer(view, first, count, buffer, 0);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = journal_view_transfer(view, first, count, buffer, 1);

	/* Returns the computed result. */
	return error;
}

/*
 * Releases backing only after the caller's last immutable copy has completed.
 */
void
drv_ufs_journal_view_release(
	struct ufs_journal_view *view)
{
	struct ufs_journal *journal;

	/* Handles the view availability. */
	if (view == NULL || view->journal == NULL)
		return;
	journal = view->journal;
	memset(view, 0, sizeof(*view));
	(void)__atomic_fetch_sub(&journal->image_readers, 1U, __ATOMIC_RELEASE);
}
/* End consolidated ufs-journal.c. */

/* Begin consolidated ufs-snapshot.c. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <errno.h>
#include <string.h>

#define SECTOR_SIZE 512U
#define SNAPSHOT_VERSION 1U
#define SNAPSHOT_ACTIVE 1U
#define RECORD_MAGIC 0x52534e5aU

static void snapshot_put32(uint8_t *p, uint32_t v);

/* ZNSR */
static void
snapshot_put32(
	uint8_t *p,
	uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}
static void snapshot_put64(uint8_t *p, uint64_t v);

/* Supports the snapshot put64 operation. */
static void
snapshot_put64(
	uint8_t *p,
	uint64_t v)
{
	snapshot_put32(p, (uint32_t)v);
	snapshot_put32(p + 4, (uint32_t)(v >> 32));
}
static uint32_t snapshot_get32(const uint8_t *p);

/* Supports the snapshot get32 operation. */
static uint32_t
snapshot_get32(
	const uint8_t *p)
{
	/* Returns the computed result. */
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}
static uint64_t snapshot_get64(const uint8_t *p);

/* Supports the snapshot get64 operation. */
static uint64_t
snapshot_get64(
	const uint8_t *p)
{
	uint64_t function_result;

	/* Computes the function result. */
	function_result = snapshot_get32(p) | (uint64_t)snapshot_get32(p + 4)
						      << 32;

	/* Returns the computed result. */
	return function_result;
}
static uint32_t digest(const void *buffer, size_t length);

/* Supports the digest operation. */
static uint32_t
digest(
	const void *buffer,
	size_t length)
{
	const uint8_t *p = buffer;
	uint32_t value = 2166136261U;
	size_t n;

	/* Process each remaining element. */
	for (n = 0; n < length; n++) {
		value ^= p[n];
		value *= 16777619U;
	}

	/* Returns the computed result. */
	return value;
}

static size_t hash_sector(uint64_t sector, size_t count);

/* Supports the hash sector operation. */
static size_t
hash_sector(
	uint64_t sector,
	size_t count)
{
	sector ^= sector >> 33;
	sector *= 0xff51afd7ed558ccdULL;
	sector ^= sector >> 33;

	/* Returns the computed result. */
	return (size_t)(sector % count);
}
static struct ufs_snapshot_entry *map_find(struct ufs_snapshot *snapshot, uint64_t sector, int insert);

/* Supports the map find operation. */
static struct ufs_snapshot_entry *
map_find(
	struct ufs_snapshot *snapshot,
	uint64_t sector,
	int insert)
{
	struct ufs_snapshot_entry *entry;
	size_t start = hash_sector(sector, snapshot->map_count), slot = start;

	do {
		entry = &snapshot->map[slot];

		/* Handles the entry condition. */
		if (entry->sector == sector)
			return entry;

		/* Handles the entry condition. */
		if (entry->sector == UFS_SNAPSHOT_EMPTY)
			return insert ? entry : NULL;
		slot = (slot + 1U) % snapshot->map_count;
	} while (slot != start);

	/* Reports that no result is available. */
	return NULL;
}
static void map_clear(struct ufs_snapshot *snapshot);

/* Supports the map clear operation. */
static void
map_clear(
	struct ufs_snapshot *snapshot)
{
	size_t n;

	/* Process each remaining element. */
	for (n = 0; n < snapshot->map_count; n++) {
		snapshot->map[n].sector = UFS_SNAPSHOT_EMPTY;
		snapshot->map[n].record = 0;
	}
}

static int write_control(struct ufs_snapshot *snapshot, unsigned active, uint32_t next);

/* Supports the write control operation. */
static int
write_control(
	struct ufs_snapshot *snapshot,
	unsigned active,
	uint32_t next)
{
	int function_result;
	uint8_t sector[SECTOR_SIZE];
	int error;

	/* Builds the control sector and writes it out. */
	memset(sector, 0, sizeof(sector));
	memcpy(sector, "ZSN1", 4);
	snapshot_put32(sector + 4, SNAPSHOT_VERSION);
	snapshot_put32(sector + 8, active ? SNAPSHOT_ACTIVE : 0);
	snapshot_put32(sector + 12, next);
	snapshot_put32(sector + 16, snapshot->max_records);
	snapshot_put64(sector + 24, snapshot->volume_sectors);
	snapshot_put32(sector + 32, digest(sector, 32));
	error = snapshot->io.write(snapshot->io.context, snapshot->first_sector,
				   1, sector);

	/* Computes the function result. */
	function_result =
		error != 0 ? error : snapshot->io.flush(snapshot->io.context);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv ufs snapshot init operation.
 */
int
drv_ufs_snapshot_init(
	struct ufs_snapshot *snapshot,
	const struct ufs_journal_io *io,
	uint64_t volume,
	uint64_t first,
	uint32_t sectors,
	struct ufs_snapshot_entry *map,
	size_t map_count)
{
	uint32_t records;

	/* Handles the snapshot availability. */
	if (snapshot == NULL || io == NULL || io->read == NULL ||
	    io->write == NULL || io->flush == NULL || volume == 0 ||
	    sectors < 3U || map == NULL || map_count < 2U)

		/* Returns the computed result. */
		return EINVAL;
	records = (sectors - 1U) / 2U;

	/* Handles the records condition. */
	if (records == 0 || map_count < (size_t)records * 2U)
		return EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->io = *io;
	snapshot->volume_sectors = volume;
	snapshot->first_sector = first;
	snapshot->sector_count = sectors;
	snapshot->max_records = records;
	snapshot->map = map;
	snapshot->map_count = map_count;
	map_clear(snapshot);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv ufs snapshot open operation.
 */
int
drv_ufs_snapshot_open(
	struct ufs_snapshot *snapshot)
{
	struct ufs_snapshot_entry *entry;
	uint64_t target;
	uint8_t control[SECTOR_SIZE], header[SECTOR_SIZE], data[SECTOR_SIZE];
	uint32_t count, record;
	int error;

	/* Handles the snapshot availability. */
	if (snapshot == NULL)
		return EINVAL;
	map_clear(snapshot);
	snapshot->active = 0;
	snapshot->next_record = 0;
	error = snapshot->io.read(snapshot->io.context, snapshot->first_sector,
				  1, control);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the memcmp condition. */
	if (memcmp(control, "ZSN1", 4) != 0)
		return 0;

	/* Checks the snapshot get32 result. */
	if (snapshot_get32(control + 4) != SNAPSHOT_VERSION ||
	    snapshot_get32(control + 16) != snapshot->max_records ||
	    snapshot_get64(control + 24) != snapshot->volume_sectors ||
	    snapshot_get32(control + 32) != digest(control, 32))

		/* Returns the computed result. */
		return EIO;

	/* Checks the snapshot get32 result. */
	if (snapshot_get32(control + 8) == 0)
		return 0;

	/* Checks the snapshot get32 result. */
	if (snapshot_get32(control + 8) != SNAPSHOT_ACTIVE)
		return EIO;
	count = snapshot_get32(control + 12);

	/* Checks the remaining item count. */
	if (count > snapshot->max_records)
		return EIO;
	/* Process each remaining element. */
	for (record = 0; record < count; record++) {
		error = snapshot->io.read(snapshot->io.context,
					  snapshot->first_sector + 1U +
						  (uint64_t)record * 2U,
					  1, header);

		/* Checks the operation status. */
		if (error == 0) {
			error = snapshot->io.read(snapshot->io.context,
						  snapshot->first_sector + 2U +
							  (uint64_t)record * 2U,
						  1, data);
		}

		/* Checks the operation status. */
		if (error != 0)
			return error;
		target = snapshot_get64(header + 8);

		/* Checks the snapshot get32 result. */
		if (snapshot_get32(header) != RECORD_MAGIC ||
		    snapshot_get32(header + 4) != SNAPSHOT_VERSION ||
		    target >= snapshot->volume_sectors ||
		    snapshot_get32(header + 16) != digest(data, sizeof(data)) ||
		    snapshot_get32(header + 20) != digest(header, 20) ||
		    (entry = map_find(snapshot, target, 1)) == NULL ||
		    entry->sector != UFS_SNAPSHOT_EMPTY)

			/* Returns the computed result. */
			return EIO;
		entry->sector = target;
		entry->record = record;
	}
	snapshot->next_record = count;
	snapshot->active = 1;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv ufs snapshot create operation.
 */
int
drv_ufs_snapshot_create(
	struct ufs_snapshot *snapshot)
{
	int error;

	/* Handles the snapshot availability. */
	if (snapshot == NULL)
		return EINVAL;

	/* Handles the snapshot condition. */
	if (snapshot->active)
		return EBUSY;
	error = write_control(snapshot, 1, 0);

	/* Checks the operation status. */
	if (error == 0) {
		map_clear(snapshot);
		snapshot->next_record = 0;
		snapshot->active = 1;
	}

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the drv ufs snapshot preserve operation.
 */
int
drv_ufs_snapshot_preserve(
	struct ufs_snapshot *snapshot,
	uint64_t first,
	uint32_t count)
{
	uint64_t target;
	uint32_t record;
	struct ufs_snapshot_entry *entry;
	uint8_t data[SECTOR_SIZE], header[SECTOR_SIZE];
	uint32_t n;
	int error = 0;

	/* Handles the snapshot availability. */
	if (snapshot == NULL || count == 0 ||
	    first >= snapshot->volume_sectors ||
	    count > snapshot->volume_sectors - first)

		/* Returns the computed result. */
		return EINVAL;

	/* Handles the snapshot condition. */
	if (!snapshot->active)
		return 0;
	/* Process each remaining element. */
	for (n = 0; n < count; n++) {
		target = first + n;

		entry = map_find(snapshot, target, 1);

		/* Handles the entry availability. */
		if (entry == NULL)
			return ENOSPC;

		/* Handles the entry condition. */
		if (entry->sector == target)
			continue;

		/* Handles the snapshot condition. */
		if (snapshot->next_record >= snapshot->max_records)
			return ENOSPC;
		record = snapshot->next_record;
		error = snapshot->io.read(snapshot->io.context, target, 1,
					  data);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		error = snapshot->io.write(snapshot->io.context,
					   snapshot->first_sector + 2U +
						   (uint64_t)record * 2U,
					   1, data);

		/* Checks the operation status. */
		if (error == 0)
			error = snapshot->io.flush(snapshot->io.context);
		memset(header, 0, sizeof(header));
		snapshot_put32(header, RECORD_MAGIC);
		snapshot_put32(header + 4, SNAPSHOT_VERSION);
		snapshot_put64(header + 8, target);
		snapshot_put32(header + 16, digest(data, sizeof(data)));
		snapshot_put32(header + 20, digest(header, 20));

		/* Checks the operation status. */
		if (error == 0) {
			error = snapshot->io.write(snapshot->io.context,
						   snapshot->first_sector + 1U +
							   (uint64_t)record *
								   2U,
						   1, header);
		}

		/* Checks the operation status. */
		if (error == 0)
			error = snapshot->io.flush(snapshot->io.context);

		/* Checks the operation status. */
		if (error == 0)
			error = write_control(snapshot, 1, record + 1U);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		entry->sector = target;
		entry->record = record;
		snapshot->next_record = record + 1U;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv ufs snapshot read operation.
 */
int
drv_ufs_snapshot_read(
	struct ufs_snapshot *snapshot,
	uint64_t first,
	uint32_t count,
	void *buffer)
{
	struct ufs_snapshot_entry *entry;
	uint64_t source;
	uint8_t *bytes = buffer;
	uint32_t n;
	int error;

	/* Handles the snapshot availability. */
	if (snapshot == NULL || buffer == NULL || count == 0 ||
	    !snapshot->active || first >= snapshot->volume_sectors ||
	    count > snapshot->volume_sectors - first)

		/* Returns the computed result. */
		return EINVAL;
	/* Process each remaining element. */
	for (n = 0; n < count; n++) {
		entry = map_find(snapshot, first + n, 0);
		source = entry == NULL ? first + n
				       : snapshot->first_sector + 2U +
						 (uint64_t)entry->record * 2U;
		error = snapshot->io.read(snapshot->io.context, source, 1,
					  bytes + (size_t)n * SECTOR_SIZE);

		/* Checks the operation status. */
		if (error != 0)
			return error;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv ufs snapshot delete operation.
 */
int
drv_ufs_snapshot_delete(
	struct ufs_snapshot *snapshot)
{
	int error;

	/* Handles the snapshot availability. */
	if (snapshot == NULL)
		return EINVAL;

	/* Handles the snapshot condition. */
	if (!snapshot->active)
		return ENOENT;
	error = write_control(snapshot, 0, 0);

	/* Checks the operation status. */
	if (error == 0) {
		snapshot->active = 0;
		snapshot->next_record = 0;
		map_clear(snapshot);
	}

	/* Returns the computed result. */
	return error;
}
/* End consolidated ufs-snapshot.c. */

/* Begin consolidated ufs-super.c. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <errno.h>
#include <string.h>

static int power2(uint32_t value);

/* Supports the power2 operation. */
static int
power2(
	uint32_t value)
{
	/* Returns the computed result. */
	return value != 0 && (value & (value - 1U)) == 0;
}

/*
 * Implements the drv ufs super decode operation.
 */
int
drv_ufs_super_decode(
	const void *buffer,
	size_t length,
	uint64_t sectors,
	struct ufs_super *super)
{
	uint64_t last_cg_start, inode_fragments, medium_fragments;
	uint32_t magic;
	int swapped;

	/* Handles the buffer availability. */
	if (buffer == NULL || super == NULL || length < UFS_FS_STRUCT_SIZE)
		return EINVAL;
	magic = drv_ufs_get32(buffer, UFS_FS_MAGIC, 0);

	/* Handles the magic condition. */
	if (magic == UFS_MAGIC)
		swapped = 0;
	else if (drv_ufs_get32(buffer, UFS_FS_MAGIC, 1) == UFS_MAGIC)
		swapped = 1;
	else

		/* Returns the computed result. */
		return EOPNOTSUPP;
	memset(super, 0, sizeof(*super));
#define GET32(field, offset)                                                   \
	super->field = drv_ufs_get32(buffer, offset, swapped)
#define GET64(field, offset)                                                   \
	super->field = drv_ufs_get64(buffer, offset, swapped)
	GET32(sblkno, UFS_FS_SBLKNO);
	GET32(cblkno, UFS_FS_CBLKNO);
	GET32(iblkno, UFS_FS_IBLKNO);
	GET32(dblkno, UFS_FS_DBLKNO);
	GET32(ncg, UFS_FS_NCG);
	GET32(bsize, UFS_FS_BSIZE);
	GET32(fsize, UFS_FS_FSIZE);
	GET32(frag, UFS_FS_FRAG);
	GET32(bshift, UFS_FS_BSHIFT);
	GET32(fshift, UFS_FS_FSHIFT);
	GET32(fragshift, UFS_FS_FRAGSHIFT);
	GET32(fsbtodb, UFS_FS_FSBTODB);
	GET32(sbsize, UFS_FS_SBSIZE);
	GET32(nindir, UFS_FS_NINDIR);
	GET32(inopb, UFS_FS_INOPB);
	GET32(cssize, UFS_FS_CSSIZE);
	GET32(cgsize, UFS_FS_CGSIZE);
	GET32(ipg, UFS_FS_IPG);
	GET32(fpg, UFS_FS_FPG);
	GET64(sblockloc, UFS_FS_SBLOCKLOC);
	GET64(cstotal_ndir, UFS_FS_CSTOTAL_NDIR);
	GET64(cstotal_nbfree, UFS_FS_CSTOTAL_NBFREE);
	GET64(cstotal_nifree, UFS_FS_CSTOTAL_NIFREE);
	GET64(cstotal_nffree, UFS_FS_CSTOTAL_NFFREE);
	GET64(size, UFS_FS_SIZE);
	GET64(dsize, UFS_FS_DSIZE);
	GET64(csaddr, UFS_FS_CSADDR);
	GET32(flags, UFS_FS_FLAGS);
	GET32(maxsymlinklen, UFS_FS_MAXSYMLINKLEN);
	GET64(maxfilesize, UFS_FS_MAXFILESIZE);
#undef GET32
#undef GET64
	super->clean = *((const uint8_t *)buffer + UFS_FS_CLEAN);
	super->swapped = swapped;

	/* Handles the super condition. */
	if (super->fsize < UFS_SECTOR_SIZE ||
	    super->fsize % UFS_SECTOR_SIZE != 0)

		/* Returns the computed result. */
		return EINVAL;
	medium_fragments = sectors / (super->fsize / UFS_SECTOR_SIZE);
	last_cg_start = super->ncg == 0
				? UINT64_MAX
				: (uint64_t)(super->ncg - 1U) * super->fpg;
	inode_fragments = super->inopb == 0
				  ? UINT64_MAX
				  : ((uint64_t)super->ipg + super->inopb - 1U) /
					    super->inopb * super->frag;

	/* Checks the power2 result. */
	if (!power2(super->bsize) || !power2(super->fsize) ||
	    super->bsize < super->fsize || super->bsize > 65536U ||
	    super->bshift >= 32U || super->fshift >= 32U ||
	    super->fragshift >= 32U || super->fsbtodb >= 32U ||
	    (UINT64_C(1) << super->bshift) != super->bsize ||
	    (UINT64_C(1) << super->fshift) != super->fsize ||
	    (UINT64_C(1) << super->fragshift) != super->frag ||
	    (UINT64_C(512) << super->fsbtodb) != super->fsize ||
	    super->bsize / super->fsize != super->frag ||
	    super->nindir != super->bsize / sizeof(uint64_t) ||
	    super->inopb != super->bsize / UFS_DINODE_SIZE ||
	    super->sbsize < UFS_FS_STRUCT_SIZE ||
	    super->sbsize > UFS_SBLOCK_SIZE || super->ncg == 0 ||
	    super->ipg < 3U || super->ipg > UINT32_MAX - 7U ||
	    super->fpg == 0 || super->fpg > UINT32_MAX - 7U ||
	    (uint64_t)super->ncg * super->ipg > UINT32_MAX ||
	    super->size == 0 || super->dsize > super->size ||
	    super->size > medium_fragments ||
	    super->sblockloc != UFS_SBLOCK_OFFSET || super->cgsize == 0 ||
	    super->cgsize > super->bsize || super->sblkno >= super->cblkno ||
	    super->cblkno >= super->iblkno || super->iblkno >= super->dblkno ||
	    inode_fragments > super->dblkno - super->iblkno ||
	    last_cg_start >= super->size ||
	    super->size - last_cg_start > super->fpg ||
	    super->dblkno >= super->size - last_cg_start ||
	    super->cstotal_ndir > (uint64_t)super->ncg * super->ipg ||
	    super->cstotal_nifree > (uint64_t)super->ncg * super->ipg ||
	    super->cstotal_nbfree > super->dsize / super->frag ||
	    super->cstotal_nffree > super->dsize)

		/* Returns the computed result. */
		return EINVAL;

	/* Reports successful completion. */
	return 0;
}
/* End consolidated ufs-super.c. */
