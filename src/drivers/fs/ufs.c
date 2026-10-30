/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 4BSD-derived UFS file system.
 *
 * One unit holds everything that was once a separate translation unit: the
 * on-disk layout and its byte-order helpers, the superblock and the
 * consistency rules that guard it, the block and inode allocators, the
 * directory namespace, extended attributes, the redo journal, and the
 * snapshot device.  They share enough state -- the mount, its journal, and
 * the buffer cache underneath both -- that they are kept together until a
 * better boundary is found.
 *
 * Every mutation that spans more than one sector goes through the journal, so
 * a failure leaves either the whole change or none of it.
 */

#include "kern/clock.h"
#include "kern/disk.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/io-stats.h"
#include "kern/klog.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/mount.h"
#include "kern/namecache.h"
#include "kern/namei.h"
#include "kern/pipe.h"
#include "kern/quota.h"
#include "kern/test-fault.h"
#include "kern/ufs.h"
#include <kern/buf.h>
#include <kern/cache-memory.h>
#include <kern/io-pool.h>
#include <kern/page.h>
#include <kern/sched.h>
#include <kern/writeback.h>
#include <kern/pmem.h>
#include <kern/inode.h>
#include <kern/quota.h>
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <limits.h>
#include <uapi/limits.h>
#include <stddef.h>
#include <stdint.h>
#include <uapi/statvfs.h>
#include <uapi/blkid.h>
#include <uapi/quota.h>
#include <uapi/snapshot.h>
#include "kern/panic.h"

#define UFS_SECTOR_SIZE			512U
#define UFS_SBLOCK_OFFSET		65536U
#define UFS_SBLOCK_SIZE			8192U
#define UFS_FS_STRUCT_SIZE		1376U
#define UFS_MAGIC			0x19540119U
#define UFS_DINODE_SIZE			256U
#define UFS_ROOT_INO			2U
#define UFS_NDADDR			12U
#define UFS_NIADDR			3U
#define UFS_DIRBLKSIZ			512U
#define UFS_NXADDR			2U

/* Native UFS extended-attribute record format. */
#define UFS_EXTATTR_NAMESPACE_USER	1U
#define UFS_EXTATTR_NAMESPACE_SYSTEM	2U
#define UFS_EXTATTR_HEADER_SIZE		7U

/* Canonical struct fs offsets for the unified UFS codec. */
#define UFS_FS_SBLKNO			8U
#define UFS_FS_CBLKNO			12U
#define UFS_FS_IBLKNO			16U
#define UFS_FS_DBLKNO			20U
#define UFS_FS_NCG			44U
#define UFS_FS_BSIZE			48U
#define UFS_FS_FSIZE			52U
#define UFS_FS_FRAG			56U
#define UFS_FS_BSHIFT			80U
#define UFS_FS_FSHIFT			84U
#define UFS_FS_FRAGSHIFT		96U
#define UFS_FS_FSBTODB			100U
#define UFS_FS_SBSIZE			104U
#define UFS_FS_NINDIR			116U
#define UFS_FS_INOPB			120U
#define UFS_FS_ID			144U
#define UFS_FS_CSSIZE			156U
#define UFS_FS_CGSIZE			160U
#define UFS_FS_IPG			184U
#define UFS_FS_FPG			188U
#define UFS_FS_CLEAN			209U
#define UFS_FS_VOLNAME			680U
#define UFS_FS_VOLNAME_SIZE		32U
#define UFS_FS_SBLOCKLOC		1000U
#define UFS_FS_CSTOTAL_NDIR		1008U
#define UFS_FS_CSTOTAL_NBFREE		1016U
#define UFS_FS_CSTOTAL_NIFREE		1024U
#define UFS_FS_CSTOTAL_NFFREE		1032U
#define UFS_FS_SIZE			1080U
#define UFS_FS_DSIZE			1088U
#define UFS_FS_CSADDR			1096U
#define UFS_FS_FLAGS			1312U
#define UFS_FS_MAXSYMLINKLEN		1320U
#define UFS_FS_MAXFILESIZE		1328U
#define UFS_FS_MAGIC			1372U

/* Canonical struct ufs_dinode offsets. */
#define UFS_DI_MODE			0U
#define UFS_DI_NLINK			2U
#define UFS_DI_UID			4U
#define UFS_DI_GID			8U
#define UFS_DI_BLKSIZE			12U
#define UFS_DI_SIZE			16U
#define UFS_DI_BLOCKS			24U
#define UFS_DI_ATIME			32U
#define UFS_DI_MTIME			40U
#define UFS_DI_CTIME			48U
#define UFS_DI_BIRTHTIME		56U
#define UFS_DI_MTIMENSEC		64U
#define UFS_DI_ATIMENSEC		68U
#define UFS_DI_CTIMENSEC		72U
#define UFS_DI_BIRTHNSEC		76U
#define UFS_DI_GEN			80U
#define UFS_DI_KERNFLAGS		84U
#define UFS_DI_FLAGS			88U
#define UFS_DI_EXTSIZE			92U
#define UFS_DI_EXTB			96U
#define UFS_DI_DB			112U
#define UFS_DI_IB			208U
#define UFS_DI_MODREV			232U

/* struct cg remains the canonical FFS cylinder-group format. */
#define UFS_CG_MAGIC_VALUE		0x00090255U
#define UFS_CG_MAGIC			4U
#define UFS_CG_CGX			12U
#define UFS_CG_NDBLK			20U
#define UFS_CG_NDIR			24U
#define UFS_CG_NBFREE			28U
#define UFS_CG_NIFREE			32U
#define UFS_CG_NFFREE			36U
#define UFS_CG_IUSEDOFF			92U
#define UFS_CG_FREEOFF			96U
#define UFS_CG_NEXTFREEOFF		100U

#define UFS_SNAPSHOT_EMPTY UINT64_MAX

#define UFS_IFMT			0170000U
#define UFS_IFIFO			0010000U
#define UFS_IFCHR			0020000U
#define UFS_IFDIR			0040000U
#define UFS_IFBLK			0060000U
#define UFS_IFREG			0100000U
#define UFS_IFLNK			0120000U
#define UFS_IFSOCK			0140000U

#define UFS_QUOTA_XATTR			"system.zedbsd.quota"

/*
 * The most one allocation run of a file write covers: the blocks it
 * allocates, records in the cylinder group and the inode, and publishes
 * together, so that a long write updates them once a run.
 */
#define UFS_ALLOCATION_BLOCKS		128U
#define UFS_ALLOCATION_BYTES		(1024U * 1024U)

#define SECTOR_SIZE			512U

#define DESC_MAGIC			0x4a534655U	 /* UFSJ */
#define COMMIT_MAGIC			0x434a4655U /* UFJC */
#define JOURNAL_VERSION			2U
#define GROUP_VERSION			JOURNAL_VERSION
#define GROUP_HEADER			32U
#define GROUP_ENTRY			16U
#define IMAGE_READERS_CLOSED		(UINT32_C(1) << 31)
#define SNAPSHOT_VERSION		1U
#define SNAPSHOT_ACTIVE			1U
#define RECORD_MAGIC			0x52534e5aU

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
 * The batched metadata journal (v3).
 *
 * Metadata writes are pinned in the buffer cache and gathered into a
 * running transaction; a commit writes their current contents into one of
 * two slots of a journal file, flushes, writes a commit record, flushes,
 * and unpins them for the flusher to write home.
 *
 * The name of the journal file, which the root directory keeps to itself.
 */
#define J3_NAME			".ufs-journal"
/* The largest journal a volume gets without a size mkfs recorded, and at all. */
#define J3_DEFAULT_MAX_MIB	128U
#define J3_MAX_MIB		1024U
/* The bounds of the ranges one transaction may name. */
#define J3_RANGES_MIN		2048U
#define J3_RANGES_LIMIT		65536U
/* The fixed part of a descriptor and of a header, and one entry of each. */
#define J3_DESC_HEADER		64U
#define J3_DESC_ENTRY		16U
#define J3_HEADER_FIXED		64U
#define J3_EXTENT_ENTRY		16U
/* The sectors a commit or a replay moves at a time. */
#define J3_STAGING_SECTORS	128U
/* How many more commits a sync makes while a held range's home is owed (ws073-p051). */
#define UFS_SYNC_OWED_ROUNDS	2U
/* The slots of the set of freed blocks, a power of two. */
#define J3_FREED_MAX		16384U
/* The magic numbers of the header, a descriptor, a commit record, the locator and the request. */
#define J3_HEADER_MAGIC		0x334a555aU	/* "ZUJ3" */
#define J3_DESC_MAGIC		0x33444a5aU	/* "ZJD3" */
#define J3_COMMIT_MAGIC		0x33434a5aU	/* "ZJC3" */
#define J3_LOCATOR_MAGIC	0x4c334a5aU	/* "ZJ3L" */
#define J3_REQUEST_MAGIC	0x52334a5aU	/* "ZJ3R" */
/* The format versions of the journal's records and of the request. */
#define J3_VERSION		2U
#define J3_REQUEST_VERSION	1U
/* The byte offsets in the superblock of the locator and the request (spare words). */
#define J3_LOCATOR_OFFSET	1220U
#define J3_REQUEST_OFFSET	1248U

struct ufs_super {
	uint32_t sblkno;
	uint32_t cblkno;
	uint32_t iblkno;
	uint32_t dblkno;
	uint32_t cgoffset;
	uint32_t cgmask;
	uint32_t ncg;
	uint32_t bsize;
	uint32_t fsize;
	uint32_t frag;
	uint32_t bshift;
	uint32_t fshift;
	uint32_t fragshift;
	uint32_t fsbtodb;
	uint32_t sbsize;
	uint32_t nindir;
	uint32_t inopb;
	uint32_t ipg;
	uint32_t fpg;
	uint32_t cssize;
	uint32_t cgsize;
	uint64_t sblockloc;
	uint64_t size;
	uint64_t dsize;
	uint64_t csaddr;
	uint64_t cstotal_ndir;
	uint64_t cstotal_nbfree;
	uint64_t cstotal_nifree;
	uint64_t cstotal_nffree;
	uint32_t flags;
	uint32_t maxsymlinklen;
	uint64_t maxfilesize;
	uint8_t clean;
	int swapped;
};

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

struct ufs_journal_extent {
	uint64_t target;
	uint32_t sectors;
	const void *payload;
};

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

struct ufs_io_owner {
	struct disk *disk;
	const struct io_context *context;
};

/* One range of a running transaction; a hold is pinned but not logged. */
struct ufs_j3_range {
	uint64_t lba;
	uint32_t count;
	uint32_t hold;
};

/* One run of the journal file's sectors on the disk. */
struct ufs_j3_extent {
	uint64_t file_sector;
	uint64_t lba;
	uint32_t count;
};

/*
 * The run of disk sectors a file's sync has gathered from the file's block
 * map and not yet written back (BUG-307).  It lives on the sync's stack for
 * one walk of the map; adjacent blocks extend it, and a block elsewhere on
 * the disk first sends it to the cache's write-back.  A count of zero is
 * an empty run.
 */
struct ufs_sync_run {
	struct mount *mountp;
	uint64_t lba;
	uint32_t count;
};

/*
 * The batched metadata journal of one mount.
 *
 * It lives from the mount to the unmount inside the mount's state.  The
 * layout (extents, sizes, identity) is fixed once the journal is loaded or
 * made; the running transaction (ranges, index, freed set) changes under
 * lock and is emptied by each commit.
 */
struct ufs_j3 {
	/* The journal carries the metadata writes of the mount. */
	int active;
	/* The volume has a v3 journal: a locator names a valid header. */
	int present;
	/* The journal's own handling of its file may look the name up. */
	int creating;
	/* Serializes the running transaction and its commit. */
	struct mutex lock;
	/* The fragment of the header block, which the locator names. */
	uint64_t header_fragment;
	/* The journal's identity, which every record repeats. */
	uint64_t nonce;
	/* The sequence of the last commit written home and so recorded. */
	uint64_t applied;
	/* The sequence of the running transaction; its low bit picks the slot. */
	uint64_t sequence;
	/* The layout: the file's size, a block, the header and a slot, in sectors. */
	uint64_t total_sectors;
	uint32_t block_sectors;
	uint32_t header_sectors;
	uint32_t slot_sectors;
	/* The most sectors, ranges and descriptor sectors a transaction may take. */
	uint32_t payload_max;
	uint32_t ranges_max;
	uint32_t desc_sectors_max;
	/* The runs of disk sectors the file occupies, in file order. */
	struct ufs_j3_extent *extents;
	unsigned extent_count;
	/* The running transaction's ranges and the sectors it logs. */
	struct ufs_j3_range *ranges;
	unsigned range_count;
	uint32_t logged_sectors;
	/* An open-addressed index over the ranges: position plus one, zero empty. */
	unsigned *index;
	/* The blocks the running transaction freed, plus one, zero empty. */
	uint64_t *freed;
	unsigned freed_count;
	/* The buffer a commit copies metadata through. */
	uint8_t *staging;
	/* The mount the flusher's hook commits for, while it is registered. */
	struct mount *mountp;

	/*
	 * The closed transaction (ws073-p051, BUG-135).  A commit is done in
	 * two steps: j3_close_transaction() copies the payload into the slot
	 * and moves the running transaction here under the mount lock and the
	 * journal lock, and j3_finish_commit() seals it -- the device's
	 * flushes -- with both locks let go.  closing is set from the close to
	 * the end of the finish; the closed side belongs to the finisher
	 * meanwhile, and the next close waits on commit_idle for it to end.
	 * The ranges stay pinned under closed_sequence until the seal is
	 * durable; the freed blocks keep content out of their old owners'
	 * blocks until then.  A descriptor left here after closing drops to
	 * zero is a commit whose sealing failed, to be tried again.
	 */
	int closing;
	struct wait_queue commit_idle;
	uint64_t closed_sequence;
	struct ufs_j3_range *closed_ranges;
	unsigned closed_count;
	uint64_t *closed_freed;
	unsigned closed_freed_count;
	uint8_t *closed_descriptor;
	uint32_t closed_desc_sectors;
	/* The buffer the finisher copies a range home through; staging is the close's. */
	uint8_t *closed_staging;
	/*
	 * home_owed is set when a finish left a held range (content in a
	 * block an earlier transaction freed, with no copy in the slot)
	 * unwritten because a later transaction pinned its line again, and
	 * stays set until a finish leaves no line pinned by a later
	 * transaction: the line's next owner then wrote it whole.  A sync sees
	 * it to commit again before it returns (ws073-p051).  Under the
	 * journal lock.
	 */
	int home_owed;
};

/* Share internal object layouts with production-linked lifetime fixtures. */
struct ufs_mount_state {
	/* Guards the read-ahead state of the mount's inodes (ra_*); nothing is taken under it. */
	struct spinlock readahead_lock;
	/* Each borrowed context is protected by its corresponding I/O lock. */
	struct ufs_io_owner journal_io;
	struct ufs_io_owner snapshot_io;
	struct ufs_super super;
	struct mutex namespace_lock;

	/*
	 * The namespace's shared side (ws073-p045, BUG-135).  A name's lookup
	 * and the journal's periodic commit share the namespace: each counts
	 * itself in namespace_readers (under namespace_guard) after a brief
	 * pass through namespace_lock, so a lookup no longer waits for a
	 * commit's disk writes and cache flushes.  A namespace change holds
	 * namespace_lock and waits on namespace_drained until the count is
	 * zero, so it still runs alone against both.  load_lock admits one
	 * shared loader at a time to the in-core inode table, which the
	 * exclusive namespace_lock used to do for every loader.
	 */
	struct spinlock namespace_guard;
	unsigned namespace_readers;
	struct wait_queue namespace_drained;
	struct mutex load_lock;

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
	/*
	 * Where the last allocation run ended (a group and the fragment in it),
	 * so that the next run's search of that group starts there instead of
	 * walking the blocks it has just used from the group's start.  Only a
	 * starting point: a search that finds nothing past it goes on from the
	 * start, so a block freed below it is still found.
	 */
	uint32_t alloc_hint_cg;
	uint32_t alloc_hint_fragment;
	struct ufs_journal journal;
	struct kern_pmem journal_memory;
	struct ufs_snapshot snapshot;
	struct ufs_snapshot_entry *snapshot_map;
	struct disk *snapshot_disk;
	struct mutex snapshot_lock;
	struct quota_state quota;
	int journal_enabled;
	/* The volume's writes are delayed in the buffer cache (DISK_WRITE_CACHED). */
	int write_cached;
	/* The batched metadata journal. */
	struct ufs_j3 j3;
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
	/*
	 * The file's sequential reader, for read-ahead: where its last read
	 * ended, how far ahead blocks were asked for, and how many reads in a
	 * row were near the last; under the mount's readahead_lock.
	 */
	uint64_t ra_next;
	uint64_t ra_issued;
	unsigned ra_sequential;
};

/*
 * The indirect sectors one mapping walk has read, one for each of the first
 * UFS_BMAP_MEMO_LEVELS levels of indirection, so that a run of consecutive
 * blocks reads each sector of pointers once instead of once a block.  It
 * lives for one read or one read-ahead request, as long as the pointers it
 * read could be used anyway.
 */
#define UFS_BMAP_MEMO_LEVELS 2U
struct ufs_bmap_memo {
	uint64_t lba[UFS_BMAP_MEMO_LEVELS];
	unsigned valid[UFS_BMAP_MEMO_LEVELS];
	uint8_t sector[UFS_BMAP_MEMO_LEVELS][UFS_SECTOR_SIZE];
};

enum ufs_initial_block_kind { UFS_INITIAL_XATTR, UFS_INITIAL_DIRECTORY };

/*
 * Immediate compatibility scope; p011 adds bounded deferred metadata ownership.
 */
struct ufs_allocation {
	struct mount *mountp;
	uid_t uid;
	gid_t gid;
	unsigned active;
};

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

struct ufs_initial_allocation {
	struct ufs_inode_metadata_images images;
	struct ufs_transaction_outcome outcome;
	struct quota_charge charge;
	uint64_t fragment;
	uint32_t cg;
};

/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
struct ufs_release_group {
	struct ufs_inode_info image;
	uint8_t *memory;
	uint8_t *cg;
	uint8_t *dinode;
	uint8_t *parent;
	uint8_t *summaries;
};

struct ufs_inode_reservation {
	struct ufs_release_group images;
	struct ufs_transaction_outcome outcome;
	struct quota_charge charge;
};

struct ufs_remove_group {
	struct ufs_inode_info image;
	struct ufs_inode_info parent_image;
	struct ufs_transaction_outcome outcome;
	uint8_t *memory;
	uint8_t *directory;
	uint8_t *dinode;
	uint8_t *parent_dinode;
};

/*
 * What adding a name to a directory changed, so that a failure can undo it.
 * It lives on the stack of one addition, under the directory's lock.
 */
struct dir_addition {
	uint64_t fragment;	/* the block the record was written to */
	uint64_t old_direct;	/* that block's pointer before the addition */
	uint64_t allocated;	/* a block taken for the record, or 0 */
	uint64_t old_blocks;	/* the directory's block count before */
	off_t old_size;		/* the directory's size before */
	uint32_t edited;	/* the index of the block written */
	int fresh;		/* the block held no records before */
	int written;		/* a write of the block was attempted */
};

struct ufs_link_group {
	struct ufs_metadata_images images;
	struct ufs_inode_info directory_image;
	struct ufs_inode_info target_image;
	struct ufs_transaction_outcome outcome;
	uint8_t *memory;
	uint8_t *directory;
};

struct ufs_rename_group {
	struct ufs_metadata_images images;
	struct ufs_transaction_outcome outcome;
	struct ufs_inode_info old_image;
	struct ufs_inode_info new_image;
	struct ufs_inode_info target_image;
	uint8_t *memory;
	uint32_t old_index;	/* the old parent's block holding the old name */
	uint32_t new_index;	/* the new parent's block the new name goes in */
};

struct ufs_orphan_scan {
	struct ufs_inode_info inode;
	uint8_t *bitmap;
	uint8_t *block;
};

static const struct inode_ops ufs_inode_ops;
static const struct file_ops ufs_regular_ops;
static const struct file_ops ufs_directory_ops;
static unsigned snapshot_disk_sequence;

extern void io_error_record(struct io_error_state *, int) __attribute__((weak));

/*
 * XXX: The following should be privatized. Dependent tests should be removed.
 */
uint16_t drv_ufs_get16(const void *buffer, size_t offset, int swapped);
uint32_t drv_ufs_get32(const void *buffer, size_t offset, int swapped);
uint64_t drv_ufs_get64(const void *buffer, size_t offset, int swapped);
void drv_ufs_put16(void *buffer, size_t offset, uint16_t value, int swapped);
void drv_ufs_put32(void *buffer, size_t offset, uint32_t value, int swapped);
void drv_ufs_put64(void *buffer, size_t offset, uint64_t value, int swapped);
int drv_ufs_journal_init(struct ufs_journal *journal, const struct ufs_journal_io *io, uint64_t first, uint32_t count, uint64_t home_sectors);
int drv_ufs_journal_bind_image(struct ufs_journal *journal, void *image, size_t bytes);
int drv_ufs_journal_publishv(struct ufs_journal *journal, const struct ufs_journal_extent *extents, unsigned count);
int drv_ufs_journal_commitv(struct ufs_journal *journal, const struct ufs_journal_extent *extents, unsigned count);
int drv_ufs_journal_checkpoint(struct ufs_journal *journal);
int drv_ufs_journal_drain(struct ufs_journal *journal);
int drv_ufs_journal_commit(struct ufs_journal *journal, uint64_t target, const void *payload, uint32_t sectors);
int drv_ufs_journal_replay(struct ufs_journal *journal);
int drv_ufs_journal_read(struct ufs_journal *journal, uint64_t first, uint32_t count, void *buffer);
int drv_ufs_journal_committed(const struct ufs_journal *journal, uint64_t sequence, uint32_t digest);
void drv_ufs_journal_views_close(struct ufs_journal *journal);
int drv_ufs_journal_views_busy(const struct ufs_journal *journal);
int drv_ufs_journal_view_acquire(struct ufs_journal *journal, struct ufs_journal_view *view);
int drv_ufs_journal_view_copy(const struct ufs_journal_view *view, uint64_t first, uint32_t count, void *buffer);
void drv_ufs_journal_view_release(struct ufs_journal_view *view);
int drv_ufs_snapshot_init(struct ufs_snapshot *snapshot, const struct ufs_journal_io *io, uint64_t volume, uint64_t first, uint32_t sectors, struct ufs_snapshot_entry *map, size_t map_count);
int drv_ufs_snapshot_open(struct ufs_snapshot *snapshot);
int drv_ufs_snapshot_create(struct ufs_snapshot *snapshot);
int drv_ufs_snapshot_preserve(struct ufs_snapshot *snapshot, uint64_t first, uint32_t count);
int drv_ufs_snapshot_read(struct ufs_snapshot *snapshot, uint64_t first, uint32_t count, void *buffer);
int drv_ufs_snapshot_delete(struct ufs_snapshot *snapshot);
int drv_ufs_super_decode(const void *buffer, size_t length, uint64_t sectors, struct ufs_super *super);

static int ufs_identify(struct disk *disk, struct block_identity *identity);
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
static int write_sectors_impl(struct mount *mountp, uint64_t lba, uint32_t count, const void *buffer, const struct io_context *context, int content);
static int write_content_sectors_context(struct mount *mountp, uint64_t lba, uint32_t count, const void *buffer, const struct io_context *context);
static int write_sectors_context(struct mount *mountp, uint64_t lba, uint32_t count, const void *buffer, const struct io_context *context);
static int write_sectors(struct mount *mountp, uint64_t lba, uint32_t count, const void *buffer);
static int observed_disk_read(struct disk *disk, uint64_t block, uint32_t count, void *buffer);
static uint64_t ufs_disk_sectors(const struct disk *disk);
static void ufs_sector_blocks(const struct disk *disk, uint64_t sector, uint32_t count, uint64_t *block, uint32_t *blocks);
static int ufs_disk_read_view(struct disk *disk, uint64_t sector, uint32_t count, void *buffer, struct buf_view *view);
static int ufs_buf_write_pinned(struct disk *disk, uint64_t sector, uint32_t count, const void *data, uint64_t pin);
static int ufs_buf_unpin(struct disk *disk, uint64_t sector, uint32_t count, uint64_t pin, unsigned *kept);
static int ufs_buf_writeback_range(struct disk *disk, uint64_t sector, uint32_t count);
static int ufs_buf_writeback_cached(struct disk *disk, uint64_t sector, uint32_t count);
static uint32_t ufs_buf_prefetch(struct disk *disk, uint64_t sector, uint32_t count);
static int ufs_sync(struct mount *mountp);
static int ufs_sync_journal(struct ufs_mount_state *ms, struct mount *mountp);
static int ufs_sync_commit(struct ufs_mount_state *ms, struct mount *mountp);
static int ufs_sync_close(struct ufs_mount_state *ms, struct mount *mountp, int *closed, int *busy);
static int journal_checkpoint_locked(struct mount *mountp);
static uint32_t locator_get32(const uint8_t *p);
static uint64_t locator_get64(const uint8_t *p);
static uint32_t locator_digest(const uint8_t *p, size_t length);
static void journal_wait_readers(struct ufs_mount_state *ms);
static int journal_discover(struct mount *mountp, struct ufs_mount_state *ms);
static int snapshot_discover(struct mount *mountp, struct ufs_mount_state *ms);
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
static void stop_writing(struct mount *mountp, const char *reason, int error);
static struct ufs_inode_info *info(const struct inode *inode);
static int journal_read_image(struct ufs_mount_state *ms, uint64_t first, uint32_t count, void *buffer);
static int read_metadata_sectors(struct mount *mountp, uint64_t first, uint32_t count, void *buffer);
static int read_block(struct mount *mountp, uint64_t fragment, void *buffer);
static int write_block(struct mount *mountp, uint64_t fragment, const void *buffer);
static size_t content_run_bytes(struct inode *inode, uint64_t logical, uint64_t first, size_t remaining, int writing, int *mapping_error, struct ufs_bmap_memo *memo);
static int bmap_memo(struct inode *inode, uint64_t logical, uint64_t *result, struct ufs_bmap_memo *memo);
static int indirect_entry_memo(struct mount *mountp, uint64_t fragment, uint32_t index, uint64_t *result, struct ufs_bmap_memo *memo, unsigned level);
static int read_content_block(struct mount *mountp, uint64_t fragment, void *buffer);
static int write_content_block(struct mount *mountp, uint64_t fragment, const void *buffer);
static int write_content_context(struct mount *mountp, uint64_t fragment, const void *buffer, const struct io_context *context);
static int bit_test(const uint8_t *map, uint32_t bit);
static int cg_block_free(const uint8_t *map, uint32_t fragment, uint32_t frag);
static uint32_t cg_free_blocks(const struct ufs_super *super, const uint8_t *map, uint32_t start, uint32_t end, uint32_t want, uint32_t *found);
static uint32_t cg_search_start(const struct ufs_mount_state *ms, uint32_t cg, uint32_t base, uint32_t end);
static void bit_set(uint8_t *map, uint32_t bit);
static void bit_clear(uint8_t *map, uint32_t bit);
static uint64_t cgstart(const struct ufs_super *super, uint32_t cg);
static uint32_t cg_ndblk(const struct ufs_super *super, uint32_t cg);
static void cg_search_range(const struct ufs_super *super, uint64_t fragment, uint32_t *first, uint32_t *end);
static int load_cg_image(struct mount *mountp, uint32_t cg, uint64_t fragment);
static int cg_header_check(struct ufs_mount_state *ms, uint32_t cg, uint32_t ndblk);
static int load_cg_locked(struct mount *mountp, uint32_t cg);
static int valid_inode_fragment(const struct ufs_super *super, uint64_t fragment);
static int prepare_super_summaries(struct mount *mountp, uint8_t *buffer);
static int write_super_summaries(struct mount *mountp);
static int write_cg(struct mount *mountp);
static int write_cg_rollback(struct mount *mountp, int original_error);
static int adjust_directory_count(struct mount *mountp, uint32_t ino, int delta);
static uint64_t quota_now(void);
static int zero_new_block(struct mount *mountp, struct ufs_mount_state *ms, uint64_t fragment);
static int zero_block(struct mount *mountp, struct ufs_mount_state *ms, uint64_t fragment);
static int allocate_block_compat(struct mount *mountp, uid_t uid, gid_t gid, uint64_t *result);
static void allocation_begin(struct ufs_allocation *context, struct mount *mountp, uid_t uid, gid_t gid);
static int allocation_allocate(struct ufs_allocation *context, uint64_t *result);
static void allocation_commit(struct ufs_allocation *context);
static void allocation_abort(struct ufs_allocation *context);
static int allocate_block(struct mount *mountp, uid_t uid, gid_t gid, uint64_t *result);
static int allocate_indirect_block(struct inode *inode, uint64_t *result);
static int fragment_in_volume(const struct ufs_super *super, uint64_t fragment);
static int free_block(struct mount *mountp, uint64_t fragment, uid_t uid, gid_t gid);
static int allocate_inode_number(struct mount *mountp, uid_t uid, gid_t gid, uint32_t *number);
static int free_inode_number(struct mount *mountp, uint32_t number, uid_t uid, gid_t gid);
static int indirect_entry(struct mount *mountp, uint64_t fragment, uint32_t index, uint64_t *result);
static int bmap(struct inode *inode, uint64_t logical, uint64_t *result);
static int ufs_backing_block(struct mount *mountp, uint64_t fragment);
static int ufs_backing_map(struct inode *inode, uint64_t logical, uint64_t *result);
static int ufs_file_extents(struct file *file, file_extent_cb callback, void *context);
static int ufs_backing_identity(struct inode *inode, struct disk **disk, uint64_t *object);
static int ufs_backing_lock(struct file *file, struct ufs_mount_state **result);
static int ufs_metadata_tree(struct mount *mountp, uint64_t fragment, unsigned depth, uint64_t blocks, file_metadata_extent_cb callback, void *context);
static int ufs_file_metadata_extents(struct file *file, file_metadata_extent_cb callback, void *context);
static int bmap_ensure(struct inode *inode, uint64_t logical, uint64_t *result);
static int pread_edge(struct inode *inode, uint64_t fragment, size_t within, size_t wanted, uint8_t *buffer, size_t *amount);
static ssize_t pread_inode(struct inode *inode, void *buffer, size_t length, off_t offset);
static ssize_t pread_inode_memo(struct inode *inode, void *buffer, size_t length, off_t offset, struct ufs_bmap_memo *memo);
static void read_ahead(struct inode *inode, uint64_t offset, size_t length);
static void metadata_images_init(struct ufs_metadata_images *images, struct mount *mountp, uint8_t *memory, size_t bytes);
static int metadata_image_get(struct ufs_metadata_images *images, uint64_t fragment, uint8_t **result);
static int metadata_image_inode(struct ufs_metadata_images *images, struct inode *prepared);
static int metadata_group_commit(struct mount *mountp, const struct ufs_journal_extent *extents, unsigned count, const struct io_context *context, struct ufs_transaction_outcome *outcome);
static int allocation_missing_path(struct ufs_allocation_run *run, const struct ufs_super *super, uint64_t logical, unsigned depth);
static int allocation_group_commit(struct inode *inode, struct ufs_allocation_run *run, const struct io_context *context);
static int allocation_run_leaf(struct inode *inode, uint64_t logical, unsigned *count, struct ufs_allocation_run *run);
static int allocation_run_reserve(struct inode *inode, struct ufs_allocation_run *run);
static int allocation_run_abort(struct inode *inode, struct ufs_allocation_run *run);
static void allocation_run_release(struct ufs_allocation_run *run, int retained);
static ssize_t allocation_write_run(struct inode *inode, const void *buffer, size_t length, uint64_t logical, const struct io_context *context);
static int xattr_release_location(const struct ufs_super *super, uint64_t child, uint32_t *group, uint32_t *local);
static int xattr_existing_locked(struct inode *inode, struct ufs_inode_metadata_images *group, unsigned count, const uint8_t *area, size_t length);
static int xattr_existing_group(struct inode *inode, const uint8_t *area, size_t length, int *handled);
static int xattr_release_group(struct inode *inode, int *handled);
static int initial_block_candidate(struct inode *inode, struct ufs_initial_allocation *group);
static int initial_block_locked(struct inode *inode, enum ufs_initial_block_kind kind, const uint8_t *area, size_t length, struct ufs_initial_allocation *group);
static int initial_block_group(struct inode *inode, enum ufs_initial_block_kind kind, const uint8_t *area, size_t length, int *handled);
static int directory_backing_group(struct inode *inode, int *handled);
static int xattr_allocate_group(struct inode *inode, const uint8_t *area, size_t length, int *handled);
static uint64_t indirect_span(const struct ufs_super *super, unsigned depth);
static int release_group_locked(struct inode *inode, uint64_t parent, unsigned index, uint64_t child, struct ufs_release_group *group);
static int release_group(struct inode *inode, uint64_t parent, unsigned index, uint64_t child, int *handled);
static int detach_inode_block(struct inode *inode, uint64_t *pointer);
static int truncate_indirect(struct inode *inode, uint64_t root, unsigned depth, uint64_t base, uint64_t keep, int *empty);
static int ufs_truncate(struct inode *inode, off_t size);
static enum inode_type mode_type(uint16_t mode);
static int decode_inode_raw(struct inode *inode, const uint8_t *raw, uint32_t number, int orphan);
static int load_inode_locked(struct mount *mountp, uint32_t number, struct inode **result);
static int load_inode(struct mount *mountp, uint32_t number, struct inode **result);
static int load_inode_admitted(struct mount *mountp, uint32_t number, struct inode **result);
static void namespace_enter(struct ufs_mount_state *ms);
static void namespace_leave(struct ufs_mount_state *ms);
static void namespace_share(struct ufs_mount_state *ms);
static void namespace_unshare(struct ufs_mount_state *ms);
static int next_dirent(struct inode *directory, off_t *cursor, uint32_t *number, uint8_t *type, char name[NAME_MAX + 1U]);
static uint16_t dir_minimum(uint8_t length);
static uint8_t dir_type(enum inode_type type);
static int restore_directory_block(struct inode *directory, uint64_t fragment, const uint8_t *original, int original_error);
static int dir_check_size(struct inode *directory);
static uint32_t dir_block_count(struct inode *directory);
static uint32_t dir_block_length(struct inode *directory, uint32_t index);
static int dir_check_record(const struct ufs_mount_state *ms, const uint8_t *block, uint32_t length, uint32_t pos);
static int dir_find_in_block(const struct ufs_mount_state *ms, const uint8_t *block, uint32_t length, const struct componentname *name, uint32_t *offset, uint32_t *previous, uint32_t *number);
static int dir_find_record(struct inode *directory, const struct componentname *name, uint8_t *block, uint32_t *index, uint32_t *offset, uint32_t *previous, uint32_t *number);
static int dir_find_slack(const struct ufs_mount_state *ms, const uint8_t *block, uint32_t length, uint16_t need, uint32_t *offset);
static void dir_put_record(const struct ufs_mount_state *ms, uint8_t *block, uint32_t at, uint32_t number, uint16_t reclen, uint8_t type, const struct componentname *name);
static int dir_add(struct inode *directory, const struct componentname *name, uint32_t number, uint8_t type);
static int dir_add_place(struct inode *directory, const struct componentname *name, uint32_t number, uint8_t type, uint8_t *block, uint8_t *original, struct dir_addition *change);
static int dir_add_finish(struct inode *directory, const struct dir_addition *change, const uint8_t *original, int error);
static int directory_next_block(struct inode *inode, uint32_t *slot);
static int directory_insert_block(struct inode *directory, const struct componentname *name, uint8_t *scratch, uint32_t *index, int *grow);
static int directory_insert_prepare(struct inode *directory, const struct componentname *name, uint32_t *index);
static int rename_blocks_prepare(struct inode *old_directory, const struct componentname *old_name, struct inode *new_directory, const struct componentname *new_name, struct inode *target, struct ufs_rename_group *group);
static int dir_remove(struct inode *directory, const struct componentname *name, uint32_t *number);
static int dir_replace(struct inode *directory, const struct componentname *name, uint32_t number, uint8_t type, uint32_t *old_number, uint8_t *old_type);
static int name_is_dot(const struct componentname *name);
static void detach_new_socket_special(struct inode *inode);
static int discard_new_inode(struct inode *inode, int directory_counted);
static int discard_new_inode_after_error(struct inode *inode, int directory_counted, int original_error);
static int reserve_inode_locked(struct inode *inode, const struct inode_creation_request *request, struct ufs_inode_reservation *group);
static int reserve_inode_group(struct inode *inode, const struct inode_creation_request *request);
static int new_inode(struct inode *directory, const struct inode_creation_request *request, nlink_t links, struct inode **result);
static int ufs_lookup_locked(struct inode *directory, const struct componentname *component, struct inode **result);
static int ufs_lookup_scan(struct inode *directory, const struct componentname *component, struct inode **result, int shared);
static int remove_group_locked(struct inode *directory, const struct componentname *name, struct inode *target, struct ufs_remove_group *group);
static int remove_group(struct inode *directory, const struct componentname *name, struct inode *target, int *handled);
static int directory_image_insert(struct inode *directory, const struct componentname *name, struct inode *target, uint8_t *block, uint32_t index);
static int link_group_locked(struct inode *directory, const struct componentname *name, struct inode *target, struct ufs_link_group *group, uint32_t index);
static int link_group(struct inode *directory, const struct componentname *name, struct inode *target, int *handled);
static int directory_image_change(struct inode *directory, uint8_t *block, uint32_t index, const struct componentname *name, uint32_t expected, uint32_t replacement, uint8_t type);
static int rename_group_locked(struct inode *old_directory, const struct componentname *old_name, struct inode *new_directory, const struct componentname *new_name, struct inode *source, struct inode *target, struct ufs_rename_group *group);
static int rename_group(struct inode *old_directory, const struct componentname *old_name, struct inode *new_directory, const struct componentname *new_name, struct inode *source, struct inode *target, int *handled);
static int creation_group_locked(struct inode *directory, const struct componentname *name, struct inode *target, struct ufs_link_group *group, uint32_t index);
static int creation_group(struct inode *directory, const struct componentname *name, struct inode *target, struct ufs_transaction_outcome *outcome);
static int creation_publish(struct inode *directory, const struct componentname *name, struct inode *target, struct inode **result);
static int ufs_create(struct inode *directory, const struct componentname *name, const struct inode_creation_request *request, struct inode **result);
static int ufs_mkdir(struct inode *directory, const struct componentname *name, const struct inode_creation_request *request, struct inode **result);
static int ufs_mknod(struct inode *directory, const struct componentname *name, const struct inode_creation_request *request, struct inode **result);
static int ufs_unlink(struct inode *directory, const struct componentname *name);
static int directory_empty(struct inode *directory);
static int ufs_rmdir(struct inode *directory, const struct componentname *name);
static int ufs_rename(struct inode *old_directory, const struct componentname *old_name, struct inode *new_directory, const struct componentname *new_name, unsigned flags);
static int ufs_link(struct inode *directory, const struct componentname *name, struct inode *target);
static int ufs_symlink(struct inode *directory, const struct componentname *name, const char *target, const struct inode_creation_request *request, struct inode **result);
static ssize_t ufs_read(struct file *file, void *buffer, size_t length);
static ssize_t ufs_pread(struct file *file, void *buffer, size_t length, off_t offset);
static ssize_t ufs_write(struct file *file, const void *buffer, size_t length);
static ssize_t ufs_pwrite(struct file *file, const void *buffer, size_t length, off_t offset);
static int ufs_readdir(struct file *file, struct dirent *entry, int *eof);
static ssize_t ufs_readlink(struct inode *inode, char *buffer, size_t length);
static size_t extattr_align(size_t value);
static int extattr_name(const char *name, uint8_t *name_space, const char **stored, size_t *stored_length);
static int extattr_load(struct inode *inode, uint8_t **result, size_t *length);
static int extattr_find(struct inode *inode, const uint8_t *area, size_t area_length, uint8_t name_space, const char *name, size_t name_length, size_t *at, size_t *record_length, size_t *content_at, size_t *content_length);
static int extattr_publish(struct inode *inode, const uint8_t *area, size_t length);
static ssize_t ufs_listxattr(struct inode *inode, char *list, size_t size);
static int ufs_removexattr(struct inode *inode, const char *name);
static int ufs_getattr(struct inode *inode, struct stat *status);
static int valid_disk_time(time_t seconds, long nanoseconds);
static int ufs_setattr(struct inode *inode, const struct stat *status, unsigned mask);
static int ufs_inode_sync(struct inode *inode);
static int retire_inode_locked(struct inode *inode, struct ufs_release_group *group);
static int retire_inode_group(struct inode *inode, int *handled);
static void ufs_reclaim(struct inode *inode);
static int ufs_file_sync(struct file *file);
static int file_writeback_locked(struct inode *inode);
static int sync_tree(struct ufs_sync_run *run, uint64_t fragment, unsigned depth, uint64_t blocks, uint8_t *sectors);
static int sync_run_add(struct ufs_sync_run *run, uint64_t fragment);
static int sync_run_write(struct ufs_sync_run *run);
static struct inode *ufs_alloc_inode(struct mount *mountp);
static void ufs_free_inode(struct inode *inode);
static int ufs_read_super(struct disk *disk, struct ufs_super *super);
static char ufs_identity_hex(unsigned value);
static void ufs_identity_hex32(char output[8], uint32_t value);
static void ufs_identity_label(char *output, size_t capacity, const uint8_t *input, size_t length);
static int ufs_write_clean(struct mount *mountp, uint8_t clean);
static int order_barrier(struct mount *mountp);
static void write_cached_start(struct mount *mountp, struct ufs_mount_state *ms, struct inode *root);
static int write_cached_intended(const struct mount *mountp, const struct ufs_mount_state *ms);
static uint64_t j3_sector(const struct ufs_j3 *j3, uint64_t file_sector, uint32_t *run);
static int j3_io(struct mount *mountp, const struct ufs_j3 *j3, uint64_t file_sector, uint32_t count, void *buffer, int write);
static int j3_writeback(struct mount *mountp, const struct ufs_j3 *j3, uint64_t file_sector, uint32_t count);
static int j3_durable(struct mount *mountp);
static unsigned j3_hash(uint64_t key, unsigned size);
static void j3_range_add(struct ufs_j3 *j3, uint64_t lba, uint32_t count, uint32_t hold);
static int j3_freed_test(const uint64_t *set, uint64_t fragment);
static void j3_freed_add(struct ufs_mount_state *ms, uint64_t fragment);
static int j3_content_freed(const struct ufs_mount_state *ms, uint64_t lba, uint32_t count);
static int j3_room(const struct ufs_j3 *j3, uint32_t count, int hold);
static int j3_commit_pending(const struct ufs_j3 *j3);
static int j3_write(struct ufs_mount_state *ms, struct mount *mountp, uint64_t lba, uint32_t count, const void *buffer, const struct io_context *context, int content);
static int j3_geometry(struct ufs_j3 *j3);
static int j3_write_header(struct mount *mountp, struct ufs_j3 *j3);
static uint32_t j3_logged_ranges(const struct ufs_j3 *j3);
static int j3_copy_range(struct mount *mountp, struct ufs_j3 *j3, const struct ufs_j3_range *range, uint64_t cursor, uint32_t *sum);
static int j3_commit_payload(struct mount *mountp, struct ufs_j3 *j3, uint8_t *descriptor, uint32_t desc_sectors);
static int j3_commit_seal(struct mount *mountp, struct ufs_j3 *j3, uint8_t *descriptor, uint32_t desc_sectors, uint64_t sequence);
static void j3_unpin_ranges(struct mount *mountp, const struct ufs_j3_range *ranges, unsigned count, uint64_t pin);
static int j3_home_direct(struct mount *mountp, struct ufs_j3 *j3, const struct ufs_j3_range *range, uint64_t cursor);
static int j3_close_transaction(struct ufs_mount_state *ms, struct mount *mountp, int *closed);
static int j3_finish_commit(struct ufs_mount_state *ms, struct mount *mountp);
static void j3_commit_wait(struct ufs_j3 *j3);
static int j3_commit_now(struct ufs_mount_state *ms, struct mount *mountp);
static void j3_hook(void *argument);
static int j3_super_read(struct mount *mountp, uint8_t sector[SECTOR_SIZE]);
static int j3_locator_parse(const uint8_t sector[SECTOR_SIZE], uint64_t *fragment, uint64_t *nonce);
static int j3_request_parse(const uint8_t sector[SECTOR_SIZE], uint32_t *mib);
static int j3_header_check(const uint8_t *header, uint64_t nonce, uint32_t block_sectors, uint32_t block_bytes, uint32_t *count);
static int j3_extents_read(struct mount *mountp, struct ufs_j3 *j3, const uint8_t *header, uint32_t count, uint64_t *total);
static int j3_load(struct mount *mountp, struct ufs_mount_state *ms, uint64_t fragment, uint64_t nonce);
static uint64_t j3_slot_sequence(struct mount *mountp, struct ufs_j3 *j3, unsigned slot, uint32_t *desc_sectors, uint32_t *desc_sum);
static int j3_descriptor_check(const struct ufs_j3 *j3, const uint8_t *descriptor, uint32_t desc_sectors, uint64_t sequence, uint32_t desc_sum, uint32_t *entries);
static int j3_payload_sum(struct mount *mountp, struct ufs_j3 *j3, uint64_t cursor, uint32_t count, uint8_t *payload, uint32_t *sum);
static int j3_payload_home(struct mount *mountp, struct ufs_j3 *j3, uint64_t cursor, uint64_t lba, uint32_t count, uint8_t *payload);
static int j3_payload_apply(struct mount *mountp, struct ufs_j3 *j3, const uint8_t *descriptor, uint32_t entries, uint64_t first, uint8_t *payload);
static int j3_replay_slot(struct mount *mountp, struct ufs_j3 *j3, unsigned slot, uint64_t sequence, uint32_t desc_sectors, uint32_t desc_sum);
static int j3_replay_sealed(struct mount *mountp, struct ufs_j3 *j3, unsigned slot, uint64_t sequence, uint32_t desc_sectors, uint32_t desc_sum);
static int j3_replay(struct mount *mountp, struct ufs_mount_state *ms);
static uint64_t j3_wanted_sectors(struct mount *mountp, struct ufs_mount_state *ms);
static int j3_locator_write(struct mount *mountp, struct ufs_j3 *j3);
static unsigned mountp_disk_cached(struct mount *mountp);
static int j3_file_open(struct ufs_mount_state *ms, struct inode *root, struct inode **inode);
static int j3_file_matches(struct ufs_mount_state *ms, struct inode *root, int *matches);
static int j3_locator_erase(struct mount *mountp);
static int j3_file_fill(struct ufs_mount_state *ms, struct inode *inode, uint64_t blocks, struct ufs_j3_extent *extents, unsigned capacity, unsigned *count);
static int j3_allocate(struct ufs_mount_state *ms, struct inode *root, uint64_t sectors);
static int j3_make(struct mount *mountp, struct ufs_mount_state *ms, struct inode *root, uint64_t sectors);
static int j3_open(struct mount *mountp, struct ufs_mount_state *ms, struct inode *root);
static int j3_close(struct mount *mountp, struct ufs_mount_state *ms, int commit);
static int j3_hidden(const struct inode *directory, const struct componentname *component);
static int ufs_probe(struct disk *disk);
static int ufs_quota_rebuild(struct mount *mountp);
static int ufs_quota_load(struct mount *mountp, struct inode *root);
static int snapshot_disk_submit(struct disk *disk, struct bio *bio);
static int snapshot_disk_publish(struct ufs_mount_state *ms);
static int snapshot_disk_remove(struct ufs_mount_state *ms);
static void ufs_state_free(struct ufs_mount_state *ms);
static int ufs_quota_persist(struct mount *mountp);
static int orphan_recover_one(struct mount *mountp, uint32_t number, const uint8_t *raw, struct ufs_inode_info *owner);
static int orphan_scan_locked(struct mount *mountp, struct ufs_orphan_scan *scan);
static int orphan_recover(struct mount *mountp);
static int ufs_mount_impl(struct mount *mountp);
static int ufs_mount_steps(struct mount *mountp, const char **step);
static int ufs_statvfs(struct mount *mountp, struct statvfs *result);
static int ufs_quotactl(struct mount *mountp, struct quota_control *request);
static int ufs_snapshotctl(struct mount *mountp, struct snapshot_control *request);
static int ufs_prepare_unmount(struct mount *mountp);
static int ufs_prepare_unmount_revoked(struct mount *mountp);
static void ufs_commit_unmount_revoked(struct mount *mountp);
static void ufs_unmount(struct mount *mountp);
static uint32_t checksum(const void *buffer, size_t length);
static void put32(uint8_t *p, uint32_t v);
static void put64(uint8_t *p, uint64_t v);
static uint32_t get32(const uint8_t *p);
static uint64_t get64(const uint8_t *p);
static int clear_record(struct ufs_journal *journal, uint64_t sector);
static uint32_t group_checksum(uint8_t *descriptor);
static int journal_finish(struct ufs_journal *journal);
static int group_validate(struct ufs_journal *journal, uint8_t *descriptor);
static int journal_replay(struct ufs_journal *journal, uint64_t expected_sequence, uint32_t expected_digest, int apply, uint8_t *view);
static void journal_close_views(struct ufs_journal *journal);
static int journal_view_transfer(const struct ufs_journal_view *view, uint64_t first, uint32_t count, void *buffer, int copy);
static void snapshot_put32(uint8_t *p, uint32_t v);
static void snapshot_put64(uint8_t *p, uint64_t v);
static uint32_t snapshot_get32(const uint8_t *p);
static uint64_t snapshot_get64(const uint8_t *p);
static uint32_t digest(const void *buffer, size_t length);
static size_t hash_sector(uint64_t sector, size_t count);
static struct ufs_snapshot_entry *map_find(struct ufs_snapshot *snapshot, uint64_t sector, int insert);
static void map_clear(struct ufs_snapshot *snapshot);
static int write_control(struct ufs_snapshot *snapshot, unsigned active, uint32_t next);
static int power2(uint32_t value);

/*
 * UFS
 */

static const struct inode_ops ufs_inode_ops = {
	.lookup = ufs_lookup,
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
	.reclaim = ufs_reclaim
};

static const struct file_ops ufs_regular_ops = {
	.read = ufs_read,
	.write = ufs_write,
	.pread = ufs_pread,
	.pwrite = ufs_pwrite,
	.pwrite_internal =
	ufs_pwrite_context,
	.fsync = ufs_file_sync};

static const struct file_ops ufs_directory_ops = {
	.readdir = ufs_readdir,
	.fsync = ufs_file_sync
};

static const struct disk_ops snapshot_disk_ops = {
	.submit = snapshot_disk_submit
};

const struct filesystem_type drv_ufs_filesystem_type = {
	.file_backing_identity = ufs_backing_identity,
	.file_extents = ufs_file_extents,
	.file_metadata_extents = ufs_file_metadata_extents,
	.writeback_range = ufs_writeback_range,
	.fs_name = "ufs",
	.probe = ufs_probe,
	.identify = ufs_identify,
	.mount = ufs_mount_impl,
	.sync = ufs_sync,
	.statvfs = ufs_statvfs,
	.quotactl = ufs_quotactl,
	.snapshotctl = ufs_snapshotctl,
	.prepare_unmount = ufs_prepare_unmount,
	.prepare_unmount_revoked = ufs_prepare_unmount_revoked,
	.commit_unmount_revoked = ufs_commit_unmount_revoked,
	.unmount = ufs_unmount,
	.alloc_inode = ufs_alloc_inode,
	.free_inode = ufs_free_inode,
};

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
	/* A call that names no journal has nothing to initialize. */
	if (journal == NULL)
		return EINVAL;	/* Failed. */

	/* The journal reaches its sectors only through the given interface. */
	if (io == NULL || io->read == NULL || io->write == NULL ||
	    io->flush == NULL)
		return EINVAL;	/* Failed. */

	/* A descriptor, a payload and a commit are the least it holds. */
	if (count < 3U)
		return EINVAL;	/* Failed. */

	/* A journal whose last sector would wrap has no end to write to. */
	if (first > UINT64_MAX - count)
		return EINVAL;	/* Failed. */

	/* A volume with no home sectors gives the journal nothing to serve. */
	if (home_sectors == 0)
		return EINVAL;	/* Failed. */

	/* And the journal has to sit past the home area, never inside it. */
	if (home_sectors >= first)
		return EINVAL;	/* Failed. */

	/* Records the geometry and the device this journal runs over. */
	kern_memset(journal, 0, sizeof(*journal));
	journal->io = *io;
	journal->first_sector = first;
	journal->sector_count = count;
	journal->next_sequence = 1;
	journal->home_sectors = home_sectors;
	journal->image_readers = IMAGE_READERS_CLOSED;

	/* Succeeded. */
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
	int busy;

	/*
	 * Rejects incomplete storage and live ownership without changing the
	 * binding.
	 */
	if (journal == NULL)
		return EINVAL;	/* Failed. */

	/* Unbinding names no image, so it may not name a size either. */
	if (image == NULL && bytes != 0)
		return EINVAL;	/* Failed. */

	/* An image too small to hold a whole group could not serve one. */
	if (image != NULL && bytes < UFS_JOURNAL_IMAGE_BYTES)
		return EINVAL;	/* Failed. */

	/* A slot that already holds a group cannot take an image. */
	if (journal->pending_sequence != 0)
		return EBUSY;

	journal_close_views(journal);

	/* Asks whether any reader still holds a view of the old image. */
	busy = drv_ufs_journal_views_busy(journal);

	/* A reader still holding a view would see the image change. */
	if (busy)
		return EBUSY;

	journal->image = image;
	journal->image_valid = 0;

	/*
	 * Reports exclusive storage ready for the next publication or boot
	 * replay.
	 */
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
	uint32_t stamp;
	unsigned index;
	int busy;
	int error;

	/*
	 * Validates every extent and the complete footprint before modifying
	 * the slot.
	 */
	if (journal == NULL ||
	    extents == NULL ||
	    count == 0 ||
	    count > UFS_JOURNAL_EXTENTS) {
		/* Failed. */
		return EINVAL;
	}

	/* A poisoned journal may no longer be written to. */
	if (journal->poisoned)
		return EIO;

	/* A slot that already holds a group cannot take another. */
	if (journal->pending_sequence != 0)
		return EBUSY;

	/* Asks whether any reader still holds a view of the slot. */
	busy = drv_ufs_journal_views_busy(journal);

	/* A reader still holding a view would see the slot change. */
	if (busy)
		return EBUSY;

	/* Refuses a sequence number the journal could not record. */
	if (journal->next_sequence == 0 || journal->next_sequence == UINT64_MAX)
		return EOVERFLOW;

	/* Builds the descriptor that names the group and its targets. */
	kern_memset(descriptor, 0, sizeof(descriptor));

	/* The magic word and version a reader identifies the group by. */
	put32(descriptor, DESC_MAGIC);
	put32(descriptor + 4, GROUP_VERSION);

	/* The sequence number this group is published under. */
	put64(descriptor + 8, journal->next_sequence);

	/* And how many targets follow it. */
	put32(descriptor + 16, count);

	total = 0;

	/* Refuses a target the caller did not fully describe. */
	for (index = 0; index < count; index++) {
		/* A target needs both a payload and somewhere to write it. */
		if (extents[index].payload == NULL ||
		    extents[index].sectors == 0 ||
		    extents[index].sectors >
		    UFS_JOURNAL_GROUP_SECTORS - total) {
			/* Failed. */
			return EINVAL;
		}

		entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
		put64(entry, extents[index].target);
		put32(entry + 8, extents[index].sectors);
		total += extents[index].sectors;
	}

	put32(descriptor + 20, total);
	put32(descriptor + 28, group_checksum(descriptor));

	/* Refuses a descriptor that does not hold together. */
	error = group_validate(journal, descriptor);
	if (error != 0)
		return EINVAL;

	/*
	 * Refuses a live slot instead of overwriting committed or unresolved
	 * ownership.
	 */
	error = journal->io.read(journal->io.context, journal->first_sector, 1,
				 commit);
	if (error != 0)
		return error;

	/* The first word of an unused commit record is zero. */
	stamp = get32(commit);

	/* A commit record that is not empty means the slot is still in use. */
	if (stamp != 0)
		return EBUSY;

	/*
	 * Binds each payload only after all addresses and lengths have passed
	 * validation.
	 */
	for (index = 0; index < count; index++) {
		entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
		put32(entry + 12,
		      checksum(extents[index].payload,
			       (size_t)extents[index].sectors * SECTOR_SIZE));
	}

	/* Seals the descriptor with a checksum over everything it names. */
	put32(descriptor + 28, group_checksum(descriptor));

	kern_memset(commit, 0, sizeof(commit));

	/* The magic word and version a reader identifies the commit by. */
	put32(commit, COMMIT_MAGIC);
	put32(commit + 4, GROUP_VERSION);

	/* The sequence number, which this publication now consumes. */
	put64(commit + 8, journal->next_sequence++);

	/* The descriptor checksum, which ties the commit to that one group. */
	put32(commit + 16, get32(descriptor + 28));

	/* And a checksum of the commit record itself. */
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
	 * redo bytes.
	 */
	error = clear_record(journal, journal->first_sector +
			     journal->sector_count - 1U);
	if (error == 0) {
		error = journal->io.write(journal->io.context,
					  journal->first_sector,
					  1,
					  descriptor);
	}

	cursor = journal->first_sector + 1U;

	/* Writes the payload of every target into the journal. */
	for (index = 0; error == 0 && index < count; index++) {
		error = journal->io.write(journal->io.context, cursor,
					  extents[index].sectors,
					  extents[index].payload);
		cursor += extents[index].sectors;
	}

	if (error == 0)
		error = journal->io.flush(journal->io.context);

	/* The payload has to reach the device before the commit does. */
	if (error == 0) {
		error = journal->io.write(journal->io.context,
					  journal->first_sector +
					  journal->sector_count - 1U,
					  1, commit);
	}

	if (error == 0)
		error = journal->io.flush(journal->io.context);

	/*
	 * Confirms this exact commit before exposing redo to metadata readers.
	 */
	if (error == 0) {
		error = journal_replay(journal,
				       journal->pending_sequence,
				       journal->pending_digest,
				       0,
				       NULL);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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
	int replayed;
	int error;

	/*
	 * Never recover a different caller's already pending group as a side
	 * effect.
	 */
	if (journal != NULL && journal->pending_sequence != 0) {
		/* A poisoned journal will never let that group finish. */
		if (journal->poisoned)
			return EIO;	/* Failed. */

		/* Otherwise the caller may retry once the group commits. */
		return EBUSY;	/* Failed. */
	}

	/*
	 * Preserves the operation error even when recovery establishes a safe
	 * slot.
	 */
	error = drv_ufs_journal_publishv(journal, extents, count);
	if (error == 0)
		error = drv_ufs_journal_checkpoint(journal);
	if (error != 0 && journal != NULL && journal->pending_sequence != 0) {
		/* Tries to write out the group the slot still holds. */
		replayed = drv_ufs_journal_replay(journal);

		/* A slot that cannot be recovered may never be reused. */
		if (replayed != 0) {
			journal->poisoned = 1;
			journal_close_views(journal);
		}
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Installs a verified pending group while retaining its witness on failure.
 */
int
drv_ufs_journal_checkpoint(
	struct ufs_journal *journal)
{
	int error;

	/*
	 * Requires recovery to resolve an interrupted publication before normal
	 * reuse.
	 */
	if (journal == NULL)
		return EINVAL;

	/* A slot with no group has nothing to check point. */
	if (journal->pending_sequence == 0)
		return 0;

	/* A group that is not ready yet must not be written out. */
	if (!journal->pending_ready)
		return EBUSY;

	/* A group already written only has its slot to retire. */
	if (journal->pending_clearing) {
		/* Retires the slot the group was published in. */
		error = journal_finish(journal);

		/* Failed. */
		return error;
	}

	/* Writes the group out and then retires its slot. */
	error = journal_replay(journal, journal->pending_sequence,
			       journal->pending_digest, 1, NULL);

	/* Reports how the check point went. */
	return error;
}

/*
 * Resolves retained checkpoint work without hiding an initial device failure.
 */
int
drv_ufs_journal_drain(
	struct ufs_journal *journal)
{
	int replayed;
	int error;

	/*
	 * A poisoned owner requires remount recovery, even if no slot is
	 * pending.
	 */
	if (journal == NULL)
		return EINVAL;

	/* A poisoned journal can no longer be drained. */
	if (journal->poisoned)
		return EIO;

	/* Writes out whatever the slot still holds. */
	error = drv_ufs_journal_checkpoint(journal);
	if (error != 0 && journal->pending_sequence != 0) {
		/* Tries to write out the group the slot still holds. */
		replayed = drv_ufs_journal_replay(journal);

		/* A slot that cannot be recovered may never be reused. */
		if (replayed != 0) {
			journal->poisoned = 1;
			journal_close_views(journal);
		}
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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
	int error;
	struct ufs_journal_extent extent;

	/*
	 * Shares validation, ordering and recovery with multi-target callers.
	 */
	extent.target = target;
	extent.sectors = sectors;
	extent.payload = payload;

	/* One target is committed as a group of one. */
	error = drv_ufs_journal_commitv(journal, &extent, 1);

	/* Reports how the commit went. */
	return error;
}

/*
 * Replays a committed group, validating every byte before touching homes.
 */
int
drv_ufs_journal_replay(
	struct ufs_journal *journal)
{
	uint64_t sequence;
	uint32_t digest;
	int error;

	/*
	 * Boot recovery may discard incomplete redo without claiming a new
	 * commit.
	 */
	if (journal == NULL)
		return EINVAL;

	/* A group already written only has its slot to retire. */
	if (journal->pending_clearing) {
		/* Retires the slot the group was published in. */
		error = journal_finish(journal);

		/* Failed. */
		return error;
	}

	/* An unprepared group is replayed under no sequence of its own. */
	sequence = 0;
	digest = 0;
	if (journal->pending_ready) {
		sequence = journal->pending_sequence;
		digest = journal->pending_digest;
	}

	/* Writes the group out and then retires its slot. */
	error = journal_replay(journal, sequence, digest, 1, NULL);

	/* Reports how the replay went. */
	return error;
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
	int bytes_read;
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

	/* A call that names no journal or nowhere to read into. */
	if (journal == NULL || buffer == NULL)
		return EINVAL;	/* Failed. */

	/* A read of no sectors, or of more than a group can carry. */
	if (count == 0 || count > UFS_JOURNAL_GROUP_SECTORS)
		return EINVAL;	/* Failed. */

	/* A read starting past the home area has nothing to return. */
	if (first >= journal->home_sectors)
		return EINVAL;	/* Failed. */

	/* Nor may one that starts inside it run off the end. */
	if (count > journal->home_sectors - first)
		return EINVAL;	/* Failed. */

	/* A poisoned journal can no longer be trusted to answer a read. */
	if (journal->poisoned)
		return EIO;

	/* With no group pending, the device holds the current contents. */
	if (journal->pending_sequence == 0 || journal->pending_clearing) {
		/* Reads straight from the device. */
		bytes_read = journal->io.read(journal->io.context, first,
					      count, buffer);

		/* Reports how the device read went. */
		return bytes_read;
	}

	/* A group that is not ready yet has nothing to serve from. */
	if (!journal->pending_ready)
		return EBUSY;

	/*
	 * Validates the entire pending group before exposing any of its
	 * payloads.
	 */
	error = journal_replay(journal, journal->pending_sequence,
			       journal->pending_digest, 0, descriptor);
	if (error != 0)
		return error;

	/*
	 * Coalesces each home gap or redo extent instead of issuing one read
	 * per sector.
	 */
	done = 0;
	while (done < count) {
		current = first + done;
		source = current;
		run = count - done;
		cursor = journal->first_sector + 1U;
		/*
		 * Walks the targets of the pending group for one that covers
		 * the read.
		 */
		for (index = 0; index < get32(descriptor + 16); index++) {
			entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
			target = get64(entry);

			/* The sectors this target covers. */
			sectors = get32(entry + 8);
			if (current >= target && current - target < sectors) {
				source = cursor + current - target;

				/*
				 * Clamps the run to what this target actually
				 * holds.
				 */
				if (run > sectors - (current - target)) {
					run = (uint32_t)(sectors -
							 (current - target));
				}

				break;
			}

			/* A target that starts inside the run shortens it. */
			if (target > current && target - current < run)
				run = (uint32_t)(target - current);
			cursor += sectors;
		}

		/* A bound image serves the payload instead of the device. */
		if (journal->image_valid &&
		    source >= journal->first_sector + 1U) {
			kern_memcpy((uint8_t *)buffer + (size_t)done * SECTOR_SIZE,
			       journal->image +
			       (size_t)(source -
					journal->first_sector) *
			       SECTOR_SIZE,
			       (size_t)run * SECTOR_SIZE);
		} else {
			/* Reads the payload out of the pending group. */
			error = journal->io.read(
				journal->io.context, source, run,
				(uint8_t *)buffer + (size_t)done * SECTOR_SIZE);
			if (error != 0)
				return error;
		}

		done += run;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Reads a 16-bit field out of a raw on-disk structure.
 */
uint16_t
drv_ufs_get16(
	const void *buffer,
	size_t offset,
	int swapped)
{
	const uint8_t *p = (const uint8_t *)buffer + offset;
	uint16_t value;

	/* A swapped volume stores the most significant byte first. */
	if (swapped) {
		value = (uint16_t)((uint16_t)p[0] << 8);
		value = (uint16_t)(value | p[1]);
	} else {
		value = p[0];
		value = (uint16_t)(value | ((uint16_t)p[1] << 8));
	}

	/* The assembled halfword. */
	return value;
}

/*
 * Reads a 32-bit field out of a raw on-disk structure.
 */
uint32_t
drv_ufs_get32(
	const void *buffer,
	size_t offset,
	int swapped)
{
	const uint8_t *p = (const uint8_t *)buffer + offset;
	uint32_t value;

	/* A swapped volume stores the most significant byte first. */
	if (swapped) {
		value = (uint32_t)p[0] << 24;
		value |= (uint32_t)p[1] << 16;
		value |= (uint32_t)p[2] << 8;
		value |= p[3];
	} else {
		value = p[0];
		value |= (uint32_t)p[1] << 8;
		value |= (uint32_t)p[2] << 16;
		value |= (uint32_t)p[3] << 24;
	}

	/* The assembled word. */
	return value;
}

/*
 * Reads a 64-bit field out of a raw on-disk structure.
 */
uint64_t
drv_ufs_get64(
	const void *buffer,
	size_t offset,
	int swapped)
{
	uint64_t low;
	uint64_t high;

	/* A swapped volume stores both halves the other way round. */
	if (swapped) {
		high = drv_ufs_get32(buffer, offset, 1);
		low = drv_ufs_get32(buffer, offset + 4U, 1);
	} else {
		low = drv_ufs_get32(buffer, offset, 0);
		high = drv_ufs_get32(buffer, offset + 4U, 0);
	}

	/* The two halves assembled into one value. */
	return low | (high << 32);
}

/*
 * Writes a 16-bit field into a raw on-disk structure.
 */
void
drv_ufs_put16(
	void *buffer,
	size_t offset,
	uint16_t value,
	int swapped)
{
	uint8_t *p = (uint8_t *)buffer + offset;

	/* A swapped volume stores the bytes the other way round. */
	if (swapped) {
		p[0] = value >> 8;
		p[1] = value;
	} else {
		p[0] = value;
		p[1] = value >> 8;
	}
}

/*
 * Writes a 32-bit field into a raw on-disk structure.
 */
void
drv_ufs_put32(
	void *buffer,
	size_t offset,
	uint32_t value,
	int swapped)
{
	uint8_t *p = (uint8_t *)buffer + offset;

	/* A swapped volume stores the bytes the other way round. */
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
 * Writes a 64-bit field into a raw on-disk structure.
 */
void
drv_ufs_put64(
	void *buffer,
	size_t offset,
	uint64_t value,
	int swapped)
{
	/* A swapped volume stores both halves the other way round. */
	if (swapped) {
		drv_ufs_put32(buffer, offset, (uint32_t)(value >> 32), 1);
		drv_ufs_put32(buffer, offset + 4U, (uint32_t)value, 1);
	} else {
		drv_ufs_put32(buffer, offset, (uint32_t)value, 0);
		drv_ufs_put32(buffer, offset + 4U, (uint32_t)(value >> 32), 0);
	}
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
	 * commit.
	 */
	if (journal == NULL || sequence == 0)
		return 0;

	/* A different sequence is not the commit the caller asked about. */
	if (journal->committed_sequence != sequence)
		return 0;

	/* The same sequence with a different digest is a different group. */
	if (journal->committed_digest != digest)
		return 0;

	/* Reports that this exact group did commit. */
	return 1;
}

/*
 * Stops admission for an owner that will drain readers before
 * destroying backing.
 */
void
drv_ufs_journal_views_close(
	struct ufs_journal *journal)
{
	/* Closing a journal that is not there does nothing. */
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

	/* A journal that is not there holds no views. */
	if (journal == NULL)
		return 0;

	readers = __atomic_load_n(&journal->image_readers, __ATOMIC_ACQUIRE);

	/* The closed flag is not a reader, so it does not count as busy. */
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
	int taken;

	/*
	 * Requires a fresh handle so repeated acquisition cannot lose a
	 * reference.
	 */
	if (journal == NULL || view == NULL)
		return EINVAL;

	/* A view that already holds a journal must be released first. */
	if (view->journal != NULL)
		return EBUSY;
	readers = __atomic_load_n(&journal->image_readers, __ATOMIC_ACQUIRE);
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* A closed image gives out no more views. */
		if ((readers & IMAGE_READERS_CLOSED) != 0)
			return ENOENT;

		/* Refuses a view the reader count could not hold. */
		if (readers == IMAGE_READERS_CLOSED - 1U)
			return EOVERFLOW;

		/*
		 * Takes the view only if nothing else changed the count
		 * meanwhile.
		 */
		taken = __atomic_compare_exchange_n(&journal->image_readers,
						    &readers, readers + 1U, 0,
						    __ATOMIC_ACQUIRE,
						    __ATOMIC_RELAXED);
		if (taken)
			break;
	}

	/* The acquired count prevents pointer rebinding and payload reuse. */
	view->journal = journal;
	view->image = journal->image;
	view->sequence = get64(view->image + 8);
	view->home_sectors = journal->home_sectors;

	/* Succeeded. */
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

	/* A view that holds no journal has nothing to copy out of. */
	if (view == NULL || view->journal == NULL || buffer == NULL)
		return EINVAL;	/* Failed. */

	/* A copy of no sectors, or of more than a group can carry. */
	if (count == 0 || count > UFS_JOURNAL_GROUP_SECTORS)
		return EINVAL;	/* Failed. */

	/* A copy starting past the home area has nothing to return. */
	if (first >= view->home_sectors)
		return EINVAL;	/* Failed. */

	/* Nor may one that starts inside it run off the end. */
	if (count > view->home_sectors - first)
		return EINVAL;	/* Failed. */

	/* Copies the sectors the caller asked for out of the view. */
	error = journal_view_transfer(view, first, count, buffer, 0);
	if (error != 0)
		return error;

	/* Reports the failure. */
	error = journal_view_transfer(view, first, count, buffer, 1);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Releases backing only after the caller's last immutable copy has completed.
 */
void
drv_ufs_journal_view_release(
	struct ufs_journal_view *view)
{
	struct ufs_journal *journal;

	/* Releasing a view that holds no journal does nothing. */
	if (view == NULL || view->journal == NULL)
		return;
	journal = view->journal;
	kern_memset(view, 0, sizeof(*view));
	(void)__atomic_fetch_sub(&journal->image_readers, 1U, __ATOMIC_RELEASE);
}

/*
 * Binds a snapshot to the area of the volume it lives in.
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

	/* A call that names no snapshot has nothing to initialize. */
	if (snapshot == NULL)
		return EINVAL;	/* Failed. */

	/* The snapshot reaches its sectors only through the given interface. */
	if (io == NULL || io->read == NULL || io->write == NULL ||
	    io->flush == NULL)
		return EINVAL;	/* Failed. */

	/* A volume of no sectors has nothing worth preserving. */
	if (volume == 0)
		return EINVAL;	/* Failed. */

	/* A control sector and one preserved pair are the least it holds. */
	if (sectors < 3U)
		return EINVAL;	/* Failed. */

	/* The map of preserved sectors is the caller's to provide. */
	if (map == NULL || map_count < 2U)
		return EINVAL;	/* Failed. */

	/* One sector holds the control record; the rest hold pairs. */
	records = (sectors - 1U) / 2U;
	if (records == 0 || map_count < (size_t)records * 2U)
		return EINVAL;
	kern_memset(snapshot, 0, sizeof(*snapshot));
	snapshot->io = *io;
	snapshot->volume_sectors = volume;
	snapshot->first_sector = first;
	snapshot->sector_count = sectors;
	snapshot->max_records = records;
	snapshot->map = map;
	snapshot->map_count = map_count;
	map_clear(snapshot);

	/* Succeeded. */
	return 0;
}

/*
 * Rebuilds a snapshot's map from what its area holds.
 */
int
drv_ufs_snapshot_open(
	struct ufs_snapshot *snapshot)
{
	struct ufs_snapshot_entry *entry;
	uint64_t target;
	uint64_t volume_sectors;
	uint8_t control[SECTOR_SIZE];
	uint8_t header[SECTOR_SIZE];
	uint8_t data[SECTOR_SIZE];
	uint32_t count;
	uint32_t record;
	uint32_t version;
	uint32_t records;
	uint32_t control_state;
	uint32_t stored_digest;
	uint32_t computed_digest;
	int signature;
	int error;

	/* Rejects a call that names no snapshot. */
	if (snapshot == NULL)
		return EINVAL;
	map_clear(snapshot);
	snapshot->active = 0;
	snapshot->next_record = 0;

	/* Reads the control sector the snapshot is described by. */
	error = snapshot->io.read(snapshot->io.context, snapshot->first_sector,
				  1, control);
	if (error != 0)
		return error;

	/* Compares the sector against the snapshot control signature. */
	signature = kern_memcmp(control, "ZSN1", 4);

	/* A sector without the signature carries no snapshot. */
	if (signature != 0)
		return 0;

	/* The format version the control sector was written by. */
	version = snapshot_get32(control + 4);
	if (version != SNAPSHOT_VERSION) {
		/* Failed. */
		return EIO;
	}

	/* How many preserved sectors the control sector says it can hold. */
	records = snapshot_get32(control + 16);
	if (records != snapshot->max_records) {
		/* Failed. */
		return EIO;
	}

	/* The size of the volume the snapshot was taken against. */
	volume_sectors = snapshot_get64(control + 24);
	if (volume_sectors != snapshot->volume_sectors) {
		/* Failed. */
		return EIO;
	}

	/* The checksum that covers the state the control sector carries. */
	stored_digest = snapshot_get32(control + 32);
	computed_digest = digest(control, 32);
	if (stored_digest != computed_digest) {
		/* Failed. */
		return EIO;
	}

	/* The state word that says whether a snapshot is in progress. */
	control_state = snapshot_get32(control + 8);

	/* A control sector with no state has never been written. */
	if (control_state == 0)
		return 0;

	/* A snapshot that is not active has nothing to open. */
	if (control_state != SNAPSHOT_ACTIVE)
		return EIO;

	/* The number of records the snapshot holds. */
	count = snapshot_get32(control + 12);
	if (count > snapshot->max_records)
		return EIO;
	/* Reads every record into the map the reads will use. */
	for (record = 0; record < count; record++) {
		/* Reads one record header. */
		error = snapshot->io.read(snapshot->io.context,
					  snapshot->first_sector + 1U +
					  (uint64_t)record * 2U,
					  1, header);
		if (error == 0) {
			error = snapshot->io.read(snapshot->io.context,
						  snapshot->first_sector + 2U +
						  (uint64_t)record * 2U,
						  1, data);
		}
		if (error != 0)
			return error;

		/* The magic word that marks a preserved-sector record. */
		version = snapshot_get32(header);
		if (version != RECORD_MAGIC) {
			/* Failed. */
			return EIO;
		}

		/* The format version this record was written by. */
		version = snapshot_get32(header + 4);
		if (version != SNAPSHOT_VERSION) {
			/* Failed. */
			return EIO;
		}

		/* The volume sector this record stands in for. */
		target = snapshot_get64(header + 8);
		if (target >= snapshot->volume_sectors) {
			/* Failed. */
			return EIO;
		}

		/* The checksum that covers the preserved contents. */
		stored_digest = snapshot_get32(header + 16);
		computed_digest = digest(data, sizeof(data));
		if (stored_digest != computed_digest) {
			/* Failed. */
			return EIO;
		}

		/* The checksum that covers the record header itself. */
		stored_digest = snapshot_get32(header + 20);
		computed_digest = digest(header, 20);
		if (stored_digest != computed_digest) {
			/* Failed. */
			return EIO;
		}

		/* Takes the map slot the preserved sector belongs in. */
		entry = map_find(snapshot, target, 1);
		if (entry == NULL) {
			/* Failed. */
			return EIO;
		}

		/*
		 * Two records standing in for one sector cannot both be right.
		 */
		if (entry->sector != UFS_SNAPSHOT_EMPTY) {
			/* Failed. */
			return EIO;
		}
		entry->sector = target;
		entry->record = record;
	}

	snapshot->next_record = count;
	snapshot->active = 1;

	/* Succeeded. */
	return 0;
}

/*
 * Starts a snapshot, from which nothing is preserved yet.
 */
int
drv_ufs_snapshot_create(
	struct ufs_snapshot *snapshot)
{
	int error;

	/* Rejects a call that names no snapshot. */
	if (snapshot == NULL)
		return EINVAL;

	/* A snapshot that is already active cannot be created again. */
	if (snapshot->active)
		return EBUSY;

	/* Publishes the control record that makes the snapshot active. */
	error = write_control(snapshot, 1, 0);
	if (error == 0) {
		map_clear(snapshot);
		snapshot->next_record = 0;
		snapshot->active = 1;
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Copies sectors aside before a write overwrites them.
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
	uint8_t data[SECTOR_SIZE];
	uint8_t header[SECTOR_SIZE];
	uint32_t n;
	int error = 0;

	/* Rejects a call that names no snapshot or no sectors. */
	if (snapshot == NULL || count == 0 ||
	    first >= snapshot->volume_sectors ||
	    count > snapshot->volume_sectors - first) {
		/* Failed. */
		return EINVAL;
	}

	/* Succeeded: an inactive snapshot preserves nothing. */
	if (!snapshot->active)
		return 0;
	/* Preserves each sector the caller is about to overwrite. */
	for (n = 0; n < count; n++) {
		target = first + n;

		/*
		 * Finds the record this sector would occupy, creating it if
		 * free.
		 */
		entry = map_find(snapshot, target, 1);
		if (entry == NULL)
			return ENOSPC;

		/*
		 * Succeeded: a sector already preserved is not preserved twice.
		 */
		if (entry->sector == target)
			continue;

		/*
		 * A snapshot with no records left cannot preserve another
		 * sector.
		 */
		if (snapshot->next_record >= snapshot->max_records)
			return ENOSPC;
		record = snapshot->next_record;

		/*
		 * Reads the sector as it stands, before the caller changes it.
		 */
		error = snapshot->io.read(snapshot->io.context, target, 1,
					  data);
		if (error != 0)
			return error;

		/* Writes that copy into the snapshot. */
		error = snapshot->io.write(snapshot->io.context,
					   snapshot->first_sector + 2U +
					   (uint64_t)record * 2U,
					   1, data);
		if (error == 0)
			error = snapshot->io.flush(snapshot->io.context);
		kern_memset(header, 0, sizeof(header));

		/* The magic word and version a reader identifies it by. */
		snapshot_put32(header, RECORD_MAGIC);
		snapshot_put32(header + 4, SNAPSHOT_VERSION);

		/* Which volume sector this record stands in for. */
		snapshot_put64(header + 8, target);

		/* A checksum of the sector contents that were preserved. */
		snapshot_put32(header + 16, digest(data, sizeof(data)));

		/* And one of the header itself, written over the rest of it. */
		snapshot_put32(header + 20, digest(header, 20));
		if (error == 0) {
			error = snapshot->io.write(snapshot->io.context,
						   snapshot->first_sector + 1U +
						   (uint64_t)record *
						   2U,
						   1, header);
		}
		if (error == 0)
			error = snapshot->io.flush(snapshot->io.context);
		if (error == 0)
			error = write_control(snapshot, 1, record + 1U);
		if (error != 0)
			return error;
		entry->sector = target;
		entry->record = record;
		snapshot->next_record = record + 1U;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Reads sectors as they stood when the snapshot was taken.
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

	/* A call that names no snapshot or nowhere to read into. */
	if (snapshot == NULL || buffer == NULL)
		return EINVAL;	/* Failed. */

	/* A read of no sectors asks for nothing. */
	if (count == 0)
		return EINVAL;	/* Failed. */

	/* A snapshot that was never taken preserves nothing to read. */
	if (!snapshot->active)
		return EINVAL;	/* Failed. */

	/* A read starting past the volume has nothing to return. */
	if (first >= snapshot->volume_sectors)
		return EINVAL;	/* Failed. */

	/* Nor may one that starts inside it run off the end. */
	if (count > snapshot->volume_sectors - first)
		return EINVAL;	/* Failed. */

	/* Reads each sector from the record that preserved it. */
	for (n = 0; n < count; n++) {
		entry = map_find(snapshot, first + n, 0);
		if (entry == NULL) {
			/* Nothing preserved it, so it is still in place. */
			source = first + n;
		} else {
			/* The record that preserved the sector holds it. */
			source = snapshot->first_sector + 2U +
				(uint64_t)entry->record * 2U;
		}

		/* Reads one preserved sector. */
		error = snapshot->io.read(snapshot->io.context, source, 1,
					  bytes + (size_t)n * SECTOR_SIZE);
		if (error != 0)
			return error;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Ends a snapshot and lets its area be reused.
 */
int
drv_ufs_snapshot_delete(
	struct ufs_snapshot *snapshot)
{
	int error;

	/* Rejects a call that names no snapshot. */
	if (snapshot == NULL)
		return EINVAL;

	/* A snapshot that is not active has nothing to delete. */
	if (!snapshot->active)
		return ENOENT;

	/* Publishes the control record that makes the snapshot inactive. */
	error = write_control(snapshot, 0, 0);
	if (error == 0) {
		snapshot->active = 0;
		snapshot->next_record = 0;
		map_clear(snapshot);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Decodes a superblock and refuses one that does not describe a volume.
 *
 * The buffer is the raw sector run the superblock was read from.  Every field
 * is taken out one at a time, in the byte order the magic number revealed,
 * and then checked: the geometry has to be self-consistent, it has to fit on
 * the medium the caller measured, and the summary counts have to be within
 * what the geometry allows.  A volume that fails any of those is refused here
 * rather than being mounted and trusted later.
 */
int
drv_ufs_super_decode(
	const void *buffer,
	size_t length,
	uint64_t sectors,
	struct ufs_super *super)
{
	uint64_t last_cg_start;
	uint64_t inode_fragments;
	uint64_t medium_fragments;
	uint64_t inodes_total;
	uint32_t magic;
	uint32_t swapped_magic;
	int power_of_two;
	int swapped;

	/* Rejects a buffer too small to hold a superblock. */
	if (buffer == NULL || super == NULL || length < UFS_FS_STRUCT_SIZE)
		return EINVAL;	/* Failed. */

	/*
	 * The magic number tells the byte order as well as the format: it
	 * reads correctly one way round on a native volume and the other way
	 * round on one written by a machine of the opposite endianness.
	 */
	magic = drv_ufs_get32(buffer, UFS_FS_MAGIC, 0);
	swapped_magic = drv_ufs_get32(buffer, UFS_FS_MAGIC, 1);
	if (magic == UFS_MAGIC) {
		swapped = 0;
	} else if (swapped_magic == UFS_MAGIC) {
		swapped = 1;
	} else {
		/* Failed. */
		return EOPNOTSUPP;
	}

	kern_memset(super, 0, sizeof(*super));

	/* Where the superblock itself sits inside a cylinder group. */
	super->sblkno = drv_ufs_get32(buffer, UFS_FS_SBLKNO, swapped);

	/* Where that group's own bookkeeping block sits. */
	super->cblkno = drv_ufs_get32(buffer, UFS_FS_CBLKNO, swapped);

	/* Where that group's inode table starts. */
	super->iblkno = drv_ufs_get32(buffer, UFS_FS_IBLKNO, swapped);

	/* Where that group's data blocks start. */
	super->dblkno = drv_ufs_get32(buffer, UFS_FS_DBLKNO, swapped);

	/* How many cylinder groups the volume is divided into. */
	super->ncg = drv_ufs_get32(buffer, UFS_FS_NCG, swapped);

	/* The block size, which is the unit a whole file is written in. */
	super->bsize = drv_ufs_get32(buffer, UFS_FS_BSIZE, swapped);

	/* The fragment size, which is the unit a file's tail is written in. */
	super->fsize = drv_ufs_get32(buffer, UFS_FS_FSIZE, swapped);

	/* How many fragments make up one block. */
	super->frag = drv_ufs_get32(buffer, UFS_FS_FRAG, swapped);

	/* The base-two logarithm of the block size. */
	super->bshift = drv_ufs_get32(buffer, UFS_FS_BSHIFT, swapped);

	/* The base-two logarithm of the fragment size. */
	super->fshift = drv_ufs_get32(buffer, UFS_FS_FSHIFT, swapped);

	/* The base-two logarithm of the fragments-per-block count. */
	super->fragshift = drv_ufs_get32(buffer, UFS_FS_FRAGSHIFT, swapped);

	/* The shift that turns a fragment number into a 512-byte sector. */
	super->fsbtodb = drv_ufs_get32(buffer, UFS_FS_FSBTODB, swapped);

	/* How many bytes of the volume the superblock occupies. */
	super->sbsize = drv_ufs_get32(buffer, UFS_FS_SBSIZE, swapped);

	/* How many block pointers one indirect block holds. */
	super->nindir = drv_ufs_get32(buffer, UFS_FS_NINDIR, swapped);

	/* How many inodes one block holds. */
	super->inopb = drv_ufs_get32(buffer, UFS_FS_INOPB, swapped);

	/* How many bytes the summary area occupies. */
	super->cssize = drv_ufs_get32(buffer, UFS_FS_CSSIZE, swapped);

	/* How many bytes one cylinder group's bookkeeping occupies. */
	super->cgsize = drv_ufs_get32(buffer, UFS_FS_CGSIZE, swapped);

	/* How many inodes one cylinder group holds. */
	super->ipg = drv_ufs_get32(buffer, UFS_FS_IPG, swapped);

	/* How many fragments one cylinder group holds. */
	super->fpg = drv_ufs_get32(buffer, UFS_FS_FPG, swapped);

	/* The byte offset the superblock was written at. */
	super->sblockloc = drv_ufs_get64(buffer, UFS_FS_SBLOCKLOC, swapped);

	/* How many directories the volume holds, by its own count. */
	super->cstotal_ndir = drv_ufs_get64(buffer, UFS_FS_CSTOTAL_NDIR,
					    swapped);

	/* How many whole blocks are free, by its own count. */
	super->cstotal_nbfree = drv_ufs_get64(buffer, UFS_FS_CSTOTAL_NBFREE,
					      swapped);

	/* How many inodes are free, by its own count. */
	super->cstotal_nifree = drv_ufs_get64(buffer, UFS_FS_CSTOTAL_NIFREE,
					      swapped);

	/* How many fragments are free, by its own count. */
	super->cstotal_nffree = drv_ufs_get64(buffer, UFS_FS_CSTOTAL_NFFREE,
					      swapped);

	/* How many fragments the whole volume spans. */
	super->size = drv_ufs_get64(buffer, UFS_FS_SIZE, swapped);

	/* How many of those fragments hold file data. */
	super->dsize = drv_ufs_get64(buffer, UFS_FS_DSIZE, swapped);

	/* Where the summary area starts, as a fragment number. */
	super->csaddr = drv_ufs_get64(buffer, UFS_FS_CSADDR, swapped);

	/* The feature flags the volume was written with. */
	super->flags = drv_ufs_get32(buffer, UFS_FS_FLAGS, swapped);

	/* The longest symbolic link this volume stores inside an inode. */
	super->maxsymlinklen = drv_ufs_get32(buffer, UFS_FS_MAXSYMLINKLEN,
					     swapped);

	/* The largest file this volume can address. */
	super->maxfilesize = drv_ufs_get64(buffer, UFS_FS_MAXFILESIZE, swapped);

	/* The clean flag is one byte and needs no byte order. */
	super->clean = *((const uint8_t *)buffer + UFS_FS_CLEAN);

	/* Every field read above was read in this order. */
	super->swapped = swapped;

	/* A fragment smaller than a sector could not be addressed. */
	if (super->fsize < UFS_SECTOR_SIZE)
		return EINVAL;	/* Failed. */

	/* Nor could one that is not a whole number of sectors. */
	if (super->fsize % UFS_SECTOR_SIZE != 0)
		return EINVAL;	/* Failed. */

	/* How many fragments the medium the caller measured could hold. */
	medium_fragments = sectors / (super->fsize / UFS_SECTOR_SIZE);

	/*
	 * Where the last cylinder group starts.  A volume claiming no groups
	 * gets a start beyond any size, so the check below refuses it.
	 */
	if (super->ncg == 0)
		last_cg_start = UINT64_MAX;
	else
		last_cg_start = (uint64_t)(super->ncg - 1U) * super->fpg;

	/*
	 * How many fragments one group's inode table occupies.  A volume
	 * claiming no inodes per block gets a size beyond any group, so the
	 * check below refuses it.
	 */
	if (super->inopb == 0)
		inode_fragments = UINT64_MAX;
	else
		inode_fragments = ((uint64_t)super->ipg + super->inopb - 1U) /
		    super->inopb * super->frag;

	/* How many inodes the volume holds, over every group. */
	inodes_total = (uint64_t)super->ncg * super->ipg;

	/* A block size that is not a power of two cannot be shifted. */
	power_of_two = power2(super->bsize);
	if (!power_of_two)
		return EINVAL;	/* Failed. */

	/* Nor can a fragment size that is not a power of two. */
	power_of_two = power2(super->fsize);
	if (!power_of_two)
		return EINVAL;	/* Failed. */

	/* A fragment is a part of a block, never larger than one. */
	if (super->bsize < super->fsize)
		return EINVAL;	/* Failed. */

	/* This driver reads blocks of at most 64 KiB. */
	if (super->bsize > 65536U)
		return EINVAL;	/* Failed. */

	/* A shift of 32 or more would be undefined on a 32-bit value. */
	if (super->bshift >= 32U || super->fshift >= 32U ||
	    super->fragshift >= 32U || super->fsbtodb >= 32U)
		return EINVAL;	/* Failed. */

	/* The block shift has to be the logarithm of the block size. */
	if ((UINT64_C(1) << super->bshift) != super->bsize)
		return EINVAL;	/* Failed. */

	/* The fragment shift has to be the logarithm of the fragment size. */
	if ((UINT64_C(1) << super->fshift) != super->fsize)
		return EINVAL;	/* Failed. */

	/* The fragment-count shift has to match the fragments per block. */
	if ((UINT64_C(1) << super->fragshift) != super->frag)
		return EINVAL;	/* Failed. */

	/* The sector shift has to turn 512 bytes into one fragment. */
	if ((UINT64_C(512) << super->fsbtodb) != super->fsize)
		return EINVAL;	/* Failed. */

	/* And the fragments per block have to divide the block exactly. */
	if (super->bsize / super->fsize != super->frag)
		return EINVAL;	/* Failed. */

	/* An indirect block holds one 64-bit pointer per slot. */
	if (super->nindir != super->bsize / sizeof(uint64_t))
		return EINVAL;	/* Failed. */

	/* A block holds whole inodes of the fixed on-disk size. */
	if (super->inopb != super->bsize / UFS_DINODE_SIZE)
		return EINVAL;	/* Failed. */

	/* A superblock smaller than its own fields is truncated. */
	if (super->sbsize < UFS_FS_STRUCT_SIZE)
		return EINVAL;	/* Failed. */

	/* One larger than the space reserved for it is corrupt. */
	if (super->sbsize > UFS_SBLOCK_SIZE)
		return EINVAL;	/* Failed. */

	/* A volume is divided into at least one cylinder group. */
	if (super->ncg == 0)
		return EINVAL;	/* Failed. */

	/* A group holds at least the three inodes the format reserves. */
	if (super->ipg < 3U)
		return EINVAL;	/* Failed. */

	/* An inode count that near the limit would wrap when rounded up. */
	if (super->ipg > UINT32_MAX - 7U)
		return EINVAL;	/* Failed. */

	/* A group holds at least one fragment. */
	if (super->fpg == 0)
		return EINVAL;	/* Failed. */

	/* A fragment count that near the limit would wrap when rounded up. */
	if (super->fpg > UINT32_MAX - 7U)
		return EINVAL;	/* Failed. */

	/* An inode number is 32 bits, so the total has to fit in one. */
	if (inodes_total > UINT32_MAX)
		return EINVAL;	/* Failed. */

	/* A volume of no fragments holds nothing. */
	if (super->size == 0)
		return EINVAL;	/* Failed. */

	/* The data area is part of the volume, never larger than it. */
	if (super->dsize > super->size)
		return EINVAL;	/* Failed. */

	/* Nor may the volume be larger than the medium it was found on. */
	if (super->size > medium_fragments)
		return EINVAL;	/* Failed. */

	/* The superblock has to say it was written where it was found. */
	if (super->sblockloc != UFS_SBLOCK_OFFSET)
		return EINVAL;	/* Failed. */

	/* A cylinder group's bookkeeping occupies at least something. */
	if (super->cgsize == 0)
		return EINVAL;	/* Failed. */

	/* And at most one block, because that is how it is read. */
	if (super->cgsize > super->bsize)
		return EINVAL;	/* Failed. */

	/*
	 * Inside a group the four areas follow one another in a fixed order:
	 * the superblock copy, the bookkeeping block, the inode table, and
	 * then the data blocks.
	 */
	if (super->sblkno >= super->cblkno)
		return EINVAL;	/* Failed. */
	if (super->cblkno >= super->iblkno)
		return EINVAL;	/* Failed. */
	if (super->iblkno >= super->dblkno)
		return EINVAL;	/* Failed. */

	/* The inode table has to fit between its start and the data blocks. */
	if (inode_fragments > super->dblkno - super->iblkno)
		return EINVAL;	/* Failed. */

	/* The last cylinder group has to start inside the volume. */
	if (last_cg_start >= super->size)
		return EINVAL;	/* Failed. */

	/* And what is left after it cannot be more than one whole group. */
	if (super->size - last_cg_start > super->fpg)
		return EINVAL;	/* Failed. */

	/* That last group still has to have room for its own data blocks. */
	if (super->dblkno >= super->size - last_cg_start)
		return EINVAL;	/* Failed. */

	/* The volume cannot hold more directories than it holds inodes. */
	if (super->cstotal_ndir > inodes_total)
		return EINVAL;	/* Failed. */

	/* Nor more free inodes than it holds inodes. */
	if (super->cstotal_nifree > inodes_total)
		return EINVAL;	/* Failed. */

	/* Nor more free blocks than the data area is blocks. */
	if (super->cstotal_nbfree > super->dsize / super->frag)
		return EINVAL;	/* Failed. */

	/* Nor more free fragments than the data area is fragments. */
	if (super->cstotal_nffree > super->dsize)
		return EINVAL;	/* Failed. */

	/*
	 * Succeeded: the superblock describes a volume this driver can mount.
	 */
	return 0;
}

/*
 * Reports what file system a disk carries, if it carries UFS.
 *
 * The caller offers every disk to every driver in turn, so this has to reject
 * a disk that is not UFS without treating it as a damaged one.
 */
static int
ufs_identify(
	struct disk *disk,
	struct block_identity *identity)
{
	struct ufs_super super;
	uint8_t *buffer;
	uint32_t first;
	uint32_t second;
	uint64_t first_block;
	uint64_t block_count;
	uint64_t disk_sectors;
	int unsupported;
	int error;

	/* Rejects a call that names no disk or nowhere to report. */
	if (disk == NULL || identity == NULL)
		return EINVAL;

	/* This driver reads disks whose blocks are whole sectors. */
	unsupported = disk_sector_supported(disk);
	if (unsupported != 0)
		return EOPNOTSUPP;
	first_block = UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;

	/* The superblock sits at a fixed offset, in whole sectors. */
	block_count = UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;
	disk_sectors = ufs_disk_sectors(disk);
	if (disk_sectors < first_block + block_count)
		return EOPNOTSUPP;

	/* Takes the staging the superblock is read into. */
	buffer = kern_malloc(UFS_SBLOCK_SIZE);
	if (buffer == NULL)
		return ENOMEM;

	/* Reads the superblock, which is where every field below lives. */
	error = disk_sector_transfer(disk, first_block, (uint32_t)block_count,
				     buffer, 0, 1, NULL);
	if (error == 0) {
		error = drv_ufs_super_decode(buffer, UFS_SBLOCK_SIZE,
					     disk_sectors, &super);
	}

	/* A fragment smaller than the disk's block was not written on it. */
	if (error == 0 && super.fsize < disk->d_block_size)
		error = EOPNOTSUPP;

	/* Reports why the superblock could not be taken. */
	if (error != 0) {
		kern_free(buffer);

		/* Failed. */
		return error;
	}

	kern_strcpy(identity->type, "ufs");
	identity->flags |= KERN_BLKID_TYPE;
	first = drv_ufs_get32(buffer, UFS_FS_ID, super.swapped);

	/* The two halves of the volume identifier. */
	second = drv_ufs_get32(buffer, UFS_FS_ID + 4U, super.swapped);
	if (first != 0U || second != 0U) {
		ufs_identity_hex32(identity->uuid, first);
		ufs_identity_hex32(identity->uuid + 8U, second);
		identity->uuid[16] = '\0';
		identity->flags |= KERN_BLKID_UUID;
	}

	ufs_identity_label(identity->label, sizeof(identity->label),
			   buffer + UFS_FS_VOLNAME, UFS_FS_VOLNAME_SIZE);

	/* Reports the volume label when the superblock carries one. */
	if (identity->label[0] != '\0')
		identity->flags |= KERN_BLKID_LABEL;
	kern_free(buffer);

	/* Succeeded. */
	return 0;
}

/*
 * Caller owns journal_lock; no borrowed operation context survives this drain.
 */
static int
journal_checkpoint_locked(
	struct mount *mountp)
{
	struct ufs_mount_state *ms = mountp->m_data;
	struct io_context child;
	int error;

	/* A volume without a journal has nothing to check point. */
	if (!ms->journal_enabled)
		return 0;

	/* Opens an ordered child context, so the group is written in order. */
	error = io_context_child(&child, NULL, IO_CONTEXT_ORDERED);
	if (error != 0)
		return error;
	ms->journal_io.context = &child;
	error = drv_ufs_journal_drain(&ms->journal);
	ms->journal_io.context = NULL;
	if (error != 0 && io_error_record != NULL) {
		io_error_record(&mountp->m_metadata_error, error);
		io_error_record(&mountp->m_write_error, error);
	}

	/* A poisoned journal leaves the volume unwritable. */
	if (ms->journal.poisoned)
		stop_writing(mountp, "the journal could not be checkpointed", error);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * How many 512-byte sectors a disk holds, whatever the size of its blocks:
 * the unit every address of the volume is in (fsbtodb).
 */
static uint64_t
ufs_disk_sectors(
	const struct disk *disk)
{
	uint64_t per_block;

	/* The sectors in one of the disk's blocks. */
	per_block = disk->d_block_size / UFS_SECTOR_SIZE;

	/* A count that would not fit is as large as can be told. */
	if (disk->d_block_count > UINT64_MAX / per_block)
		return UINT64_MAX;
	return disk->d_block_count * per_block;
}

/*
 * The disk's blocks that a run of sectors lies in: the first and how many,
 * so that a guard over blocks covers every sector of the run.
 */
static void
ufs_sector_blocks(
	const struct disk *disk,
	uint64_t sector,
	uint32_t count,
	uint64_t *block,
	uint32_t *blocks)
{
	uint32_t per_block;
	uint64_t last;

	/* The sectors in one of the disk's blocks. */
	per_block = disk->d_block_size / UFS_SECTOR_SIZE;

	/* The block of the first sector, and the one of the last. */
	*block = sector / per_block;
	last = (sector + count + per_block - 1U) / per_block;
	*blocks = (uint32_t)(last - *block);
}

/*
 * Reads whole blocks of the volume into a view of the buffer cache.  The
 * run is in sectors; on a disk of larger blocks it has to start and end on
 * the disk's blocks, which a group's block does on a volume whose
 * fragments are no smaller than them (ufs_read_super).
 */
static int
ufs_disk_read_view(
	struct disk *disk,
	uint64_t sector,
	uint32_t count,
	void *buffer,
	struct buf_view *view)
{
	uint32_t per_block;
	int error;

	/* The sectors in one of the disk's blocks. */
	per_block = disk->d_block_size / UFS_SECTOR_SIZE;

	/* A run that is not whole blocks cannot be a view of them. */
	if (sector % per_block != 0U || count % per_block != 0U)
		return EIO;

	/* Reads the blocks themselves. */
	error = disk_read_view(disk, sector / per_block, count / per_block,
			       buffer, view);
	return error;
}

/*
 * Writes a run of sectors into the cache and pins its lines under a
 * journal's tag.  The cache counts in the disk's blocks; on a disk of
 * larger blocks a run that starts or ends inside a block is merged into
 * the block's current content first.  Such a block has no other owner:
 * a fragment is never smaller than the disk's block (ufs_read_super), and
 * the journal's lock, which the caller holds, orders the metadata writes
 * that share it.
 */
static int
ufs_buf_write_pinned(
	struct disk *disk,
	uint64_t sector,
	uint32_t count,
	const void *data,
	uint64_t pin)
{
	uint8_t *staging;
	uint64_t block;
	uint64_t first;
	uint32_t blocks;
	uint32_t per_block;
	size_t bytes;
	int error;

	/* The disk's blocks the run lies in. */
	per_block = disk->d_block_size / UFS_SECTOR_SIZE;
	ufs_sector_blocks(disk, sector, count, &block, &blocks);
	first = block * per_block;

	/* A run of whole blocks goes into the cache as it is. */
	if (sector == first && count == blocks * per_block) {
		/* Pins the blocks the run fills. */
		error = buf_write_pinned(disk, block, blocks, data, pin);
		if (error != 0)
			return error;

		/* Succeeded: the run waits pinned in the cache. */
		return 0;
	}

	/* Takes the staging the covering blocks are merged in. */
	bytes = (size_t)blocks * disk->d_block_size;
	staging = kern_malloc(bytes);
	if (staging == NULL)
		return ENOMEM;

	/* Reads the covering blocks as the cache holds them now. */
	error = observed_disk_read(disk, first, blocks * per_block, staging);
	if (error != 0) {
		kern_free(staging);
		return error;
	}

	/* Lays the run into its place and pins the whole blocks. */
	kern_memcpy(staging + (size_t)(sector - first) * UFS_SECTOR_SIZE,
		    data,
		    (size_t)count * UFS_SECTOR_SIZE);
	error = buf_write_pinned(disk, block, blocks, staging, pin);
	kern_free(staging);

	/* Reports why the merged blocks could not be pinned. */
	if (error != 0)
		return error;

	/* Succeeded: the run waits pinned in the cache. */
	return 0;
}

/* Releases a journal's pins on the cache lines a run of sectors lies in. */
static int
ufs_buf_unpin(
	struct disk *disk,
	uint64_t sector,
	uint32_t count,
	uint64_t pin,
	unsigned *kept)
{
	uint64_t block;
	uint32_t blocks;
	int error;

	/* Unpins the disk's blocks the run lies in. */
	ufs_sector_blocks(disk, sector, count, &block, &blocks);
	error = buf_unpin(disk, block, blocks, pin, kept);
	if (error != 0)
		return error;

	/* Succeeded: the lines are ordinary dirty lines again. */
	return 0;
}

/* Writes back the dirty cache lines a run of sectors lies in. */
static int
ufs_buf_writeback_range(
	struct disk *disk,
	uint64_t sector,
	uint32_t count)
{
	uint64_t block;
	uint32_t blocks;
	int error;

	/* Writes back the disk's blocks the run lies in. */
	ufs_sector_blocks(disk, sector, count, &block, &blocks);
	error = buf_writeback_range(disk, block, blocks);
	if (error != 0)
		return error;

	/* Succeeded: the lines are on their way to the device. */
	return 0;
}

/* Writes back the dirty lines the cache holds of a run of sectors. */
static int
ufs_buf_writeback_cached(
	struct disk *disk,
	uint64_t sector,
	uint32_t count)
{
	uint64_t block;
	uint32_t blocks;
	int error;

	/* Writes back the cached blocks the run lies in. */
	ufs_sector_blocks(disk, sector, count, &block, &blocks);
	error = buf_writeback_cached(disk, block, blocks);
	if (error != 0)
		return error;

	/* Succeeded: the lines are on their way to the device. */
	return 0;
}

/* Reads ahead a run of sectors; reports how many sectors were queued. */
static uint32_t
ufs_buf_prefetch(
	struct disk *disk,
	uint64_t sector,
	uint32_t count)
{
	uint64_t block;
	uint32_t blocks;
	uint32_t queued;
	uint32_t per_block;

	/* Asks for the disk's blocks the run lies in. */
	per_block = disk->d_block_size / UFS_SECTOR_SIZE;
	ufs_sector_blocks(disk, sector, count, &block, &blocks);
	queued = buf_prefetch(disk, block, blocks);

	/* All of the run was queued. */
	if (queued == blocks)
		return count;

	/* Reports the sectors of the blocks that were queued. */
	return queued * per_block;
}

/* Counts filesystem disk requests, including metadata and journal traffic. */
static int
observed_disk_read(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	void *buffer)
{
	uint64_t bytes;
	int error;

	/* A request with no disk behind it moves nothing. */
	bytes = 0;
	if (disk != NULL)
		bytes = (uint64_t)count * UFS_SECTOR_SIZE;

	io_stats_record(IO_UFS_READ, bytes);

	/*
	 * Reads the sectors through the disk's cache, so the fault injection
	 * sees it, on a disk of 512-byte blocks or of larger ones.
	 */
	error = disk_sector_transfer(disk, block, count, buffer, 0, 0, NULL);

	/* Reports how the read went. */
	return error;
}

/* Reads sectors of the journal through the mount's disk. */
static int
journal_read(
	void *context,
	uint64_t lba,
	uint32_t count,
	void *buffer)
{
	int error;
	struct ufs_io_owner *owner = context;

	/* Reads through the disk the journal was published on. */
	error = observed_disk_read(owner->disk, lba, count, buffer);

	/* Reports how the read went. */
	return error;
}

/* Writes sectors of the journal through the mount's disk. */
static int
journal_write(
	void *context,
	uint64_t lba,
	uint32_t count,
	const void *buffer)
{
	int bytes_written;
	struct ufs_io_owner *owner = context;
	struct io_context child;
	int error;

	/*
	 * Opens an ordered child context, so the sectors are written in order.
	 */
	error = io_context_child(&child, owner->context, IO_CONTEXT_ORDERED);
	if (error != 0)
		return error;
	io_stats_record(IO_UFS_WRITE, (uint64_t)count * UFS_SECTOR_SIZE);

	/* Writes the sectors through that context. */
	bytes_written = disk_sector_transfer(owner->disk, lba, count,
					     (void *)buffer, 1, 0, &child);

	/* Reports how many bytes reached the disk. */
	return bytes_written;
}

/* Makes the journal's own writes durable. */
static int
journal_flush(
	void *context)
{
	int error;
	struct ufs_io_owner *owner = context;

	/* The journal is only durable once the device has it. */
	error = disk_sync(owner->disk);

	/* Reports how the flush went. */
	return error;
}

/* Reads a 32-bit field of a locator sector. */
static uint32_t
locator_get32(
	const uint8_t *p)
{
	/* A locator is stored little-endian, whatever the volume is. */
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
		(uint32_t)p[3] << 24;
}

/* Reads a 64-bit field of a locator sector. */
static uint64_t
locator_get64(
	const uint8_t *p)
{
	uint64_t value;

	/* The two halves assembled into one value. */
	value = locator_get32(p) | (uint64_t)locator_get32(p + 4)
		<< 32;

	/* Reports the assembled value. */
	return value;
}

/* Computes the checksum a locator sector carries. */
static uint32_t
locator_digest(
	const uint8_t *p,
	size_t length)
{
	uint32_t value = 2166136261U;
	size_t n;

	/* Folds every byte into the running value. */
	for (n = 0; n < length; n++) {
		value ^= p[n];
		value *= 16777619U;
	}

	/* Reports the digest. */
	return value;
}

/*
 * Drains short immutable copies before the serialized writer reuses their
 * backing.
 */
static void
journal_wait_readers(
	struct ufs_mount_state *ms)
{
	int busy;

	/*
	 * Readers release their pins without acquiring the writer's journal
	 * mutex.
	 */
	for (;;) {
		/* Asks whether any reader still holds a view. */
		busy = drv_ufs_journal_views_busy(&ms->journal);
		if (!busy)
			break;

		sched_yield();
	}
}

/*
 * Reserves and accounts immutable redo storage before journal
 * recovery/admission.
 */
static int
journal_image_alloc(
	struct ufs_mount_state *ms)
{
	struct kern_pmem memory;
	int released;
	int error;

	/*
	 * Obtains backing without holding a metadata or journal mutation lock.
	 */
	kern_memset(&memory, 0, sizeof(memory));

	/* Takes the physical memory the journal image lives in. */
	error = kern_pmem_alloc(UFS_JOURNAL_IMAGE_BYTES, KERN_PAGE_SIZE,
				&memory);
	if (error != 0 || kern_pmem_to_kernel(memory.paddr) == NULL) {
		/*
		 * Gives the memory back when it is not the size that was asked
		 * for.
		 */
		if (memory.size != 0) {
			released = kern_pmem_free(&memory);
			if (released != 0)
				KERN_FATAL("ufs journal rollback failed");
		}

		/* Failed. */
		return ENOMEM;
	}

	/*
	 * Charges the allocator's complete rounded backing to shared metadata
	 * memory.
	 */

	/* Charges the image against the metadata cache budget. */
	error = cache_memory_reserve(CACHE_MEMORY_BUF_META, memory.size, 0);
	if (error != 0) {
		/* Gives the memory back when the budget refused it. */
		released = kern_pmem_free(&memory);
		if (released != 0)
			KERN_FATAL("ufs journal reservation rollback failed");

		/* Failed. */
		return error;
	}

	cache_memory_commit(CACHE_MEMORY_BUF_META, memory.size);
	ms->journal_memory = memory;

	/* Publishes the image to the journal. */
	error = drv_ufs_journal_bind_image(&ms->journal, kern_pmem_to_kernel(memory.paddr),
					   memory.size);
	if (error != 0)
		journal_image_free(ms);

	/*
	 * Reports a complete immutable-image owner or a fully unwound failure.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Releases backing only after the mount owner has excluded every journal
 * caller.
 */
static void
journal_image_free(
	struct ufs_mount_state *ms)
{
	size_t bytes;
	int released;

	/*
	 * Failed mount recovery may retain durable redo, but has no admitted
	 * readers.
	 */

	/* The size the image was charged against the cache budget for. */
	bytes = ms->journal_memory.size;
	if (bytes == 0)
		return;
	drv_ufs_journal_views_close(&ms->journal);
	journal_wait_readers(ms);

	/* A memory release that fails leaves the budget charged. */
	released = kern_pmem_free(&ms->journal_memory);
	if (released != 0)
		KERN_FATAL("ufs journal backing release failed");
	cache_memory_release(CACHE_MEMORY_BUF_META, bytes);
	kern_memset(&ms->journal_memory, 0, sizeof(ms->journal_memory));
	ms->journal.image = NULL;
	ms->journal.image_valid = 0;
}

/* Finds the journal a volume carries, and replays it. */
static int
journal_discover(
	struct mount *mountp,
	struct ufs_mount_state *ms)
{
	struct ufs_journal_io io;
	uint8_t locator[UFS_SECTOR_SIZE];
	uint64_t end = ms->super.size << ms->super.fsbtodb;
	uint64_t start;
	uint32_t sectors;
	uint32_t version;
	uint32_t stored_digest;
	uint32_t computed_digest;
	uint64_t disk_sectors;
	int old_signature;
	int signature;
	int error;

	/* A volume too small to hold the locator carries no journal. */
	disk_sectors = ufs_disk_sectors(mountp->m_disk);
	if (end >= disk_sectors)
		return 0;

	/* Reads the last sector, where the locator lives. */
	error = observed_disk_read(mountp->m_disk, end, 1, locator);
	if (error != 0)
		return error;

	/* Compares the sector against the old and the current signature. */
	old_signature = kern_memcmp(locator, "ZUJ", 3);
	signature = kern_memcmp(locator, "ZUJ2", 4);

	/* Refuses a journal written by a version this driver cannot read. */
	if (old_signature == 0 && signature != 0)
		return EINVAL;

	/* A sector without the signature carries no journal. */
	if (signature != 0)
		return 0;

	/* The format version the locator was written by. */
	version = locator_get32(locator + 4);
	if (version != 2U) {
		/* Failed. */
		return EINVAL;
	}

	/* The volume position the locator says it describes. */
	start = locator_get64(locator + 12);
	if (start != end) {
		/* Failed. */
		return EINVAL;
	}

	/* The checksum that covers the rest of the locator. */
	stored_digest = locator_get32(locator + 24);
	computed_digest = locator_digest(locator, 24);
	if (stored_digest != computed_digest) {
		/* Failed. */
		return EINVAL;
	}

	/* The number of sectors the journal spans. */
	sectors = locator_get32(locator + 8);

	/* A journal shorter than its own bookkeeping cannot work. */
	if (sectors < 18U) {
		/* Failed. */
		return EINVAL;
	}

	/* Nor can one that would reach past the end of the volume. */
	if ((uint64_t)sectors + 1U > disk_sectors - end) {
		/* Failed. */
		return EINVAL;
	}
	ms->journal_io.disk = mountp->m_disk;
	io.context = &ms->journal_io;
	io.read = journal_read;
	io.write = journal_write;
	io.flush = journal_flush;

	/* Publishes the journal the locator described. */
	error = drv_ufs_journal_init(&ms->journal, &io, end + 1U, sectors, end);
	if (error == 0)
		error = journal_image_alloc(ms);
	if (error == 0) {
		ms->journal_enabled = 1;
		error = drv_ufs_journal_replay(&ms->journal);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Finds the snapshot a volume carries, and opens it. */
static int
snapshot_discover(
	struct mount *mountp,
	struct ufs_mount_state *ms)
{
	struct ufs_journal_io io;
	uint8_t locator[UFS_SECTOR_SIZE];
	uint64_t end = ms->super.size << ms->super.fsbtodb, cursor = end;
	uint64_t start;
	uint32_t sectors;
	uint32_t max_records;
	uint32_t version;
	uint32_t stored_digest;
	uint32_t computed_digest;
	size_t map_count;
	uint64_t disk_sectors;
	int signature;
	int error;

	/* A volume too small to hold the locator carries no snapshot. */
	disk_sectors = ufs_disk_sectors(mountp->m_disk);
	if (end >= disk_sectors)
		return 0;

	/* Reads the last sector, where the locator lives. */
	error = observed_disk_read(mountp->m_disk, end, 1, locator);
	if (error != 0)
		return error;

	/* Compares the sector against the journal signature. */
	signature = kern_memcmp(locator, "ZUJ2", 4);

	/* A journal locator sits in front of the snapshot one. */
	if (signature == 0) {
		/* The format version the journal locator was written by. */
		version = locator_get32(locator + 4);
		if (version != 2U) {
			/* Failed. */
			return EINVAL;
		}

		/* The volume position that locator says it describes. */
		start = locator_get64(locator + 12);
		if (start != end) {
			/* Failed. */
			return EINVAL;
		}

		/* The number of sectors the journal spans. */
		sectors = locator_get32(locator + 8);

		/* A journal reaching past the volume names nothing real. */
		if (sectors > disk_sectors - end - 1U) {
			/* Failed. */
			return EINVAL;
		}

		/* Steps back over the journal to reach the snapshot locator. */
		cursor = end + 1U + sectors;
	}

	/* A locator pointing past the volume names nothing. */
	if (cursor >= disk_sectors)
		return 0;

	/* Reads the sector the locator points at. */
	error = observed_disk_read(mountp->m_disk, cursor, 1, locator);
	if (error != 0)
		return error;

	/* Compares the sector against the snapshot locator signature. */
	signature = kern_memcmp(locator, "ZSL1", 4);

	/* A sector without the signature carries no snapshot. */
	if (signature != 0)
		return 0;

	/* The format version the locator was written by. */
	version = locator_get32(locator + 4);
	if (version != 1U) {
		/* Failed. */
		return EINVAL;
	}

	/* The volume position the locator says it sits at. */
	start = locator_get64(locator + 16);
	if (start != cursor) {
		/* Failed. */
		return EINVAL;
	}

	/* The end of the file system the snapshot was taken against. */
	start = locator_get64(locator + 24);
	if (start != end) {
		/* Failed. */
		return EINVAL;
	}

	/* The checksum that covers the rest of the locator. */
	stored_digest = locator_get32(locator + 32);
	computed_digest = locator_digest(locator, 32);
	if (stored_digest != computed_digest) {
		/* Failed. */
		return EINVAL;
	}

	/* The number of sectors the snapshot area spans. */
	sectors = locator_get32(locator + 8);

	/* An area too small to hold a control sector and one record. */
	if (sectors < 3U) {
		/* Failed. */
		return EINVAL;
	}

	/* Nor may the area reach past the end of the volume. */
	if ((uint64_t)sectors + 1U > disk_sectors - cursor) {
		/* Failed. */
		return EINVAL;
	}
	max_records = (sectors - 1U) / 2U;

#if SIZE_MAX == UINT32_MAX
	/* Refuses a record count whose map could not be allocated. */
	if (max_records > SIZE_MAX / (2U * sizeof(*ms->snapshot_map)))
		return EOVERFLOW;
#endif

	map_count = (size_t)max_records * 2U;
	ms->snapshot_map = kern_calloc(map_count, sizeof(*ms->snapshot_map));

	/* Gives up before publishing anything when there is no map. */
	if (ms->snapshot_map == NULL)
		return ENOMEM;
	ms->snapshot_io.disk = mountp->m_disk;
	io.context = &ms->snapshot_io;
	io.read = journal_read;
	io.write = journal_write;
	io.flush = journal_flush;

	/* Publishes the snapshot the locator described. */
	error = drv_ufs_snapshot_init(&ms->snapshot, &io, end, cursor + 1U,
				      sectors, ms->snapshot_map, map_count);
	if (error == 0)
		error = drv_ufs_snapshot_open(&ms->snapshot);
	if (error != 0) {
		kern_free(ms->snapshot_map);
		ms->snapshot_map = NULL;

		/* Failed. */
		return error;
	}

	ms->snapshot_available = 1;

	/* Succeeded. */
	return 0;
}

/* Writes sectors, preserving what a snapshot still needs. */
static int
write_sectors_impl(
	struct mount *mountp,
	uint64_t lba,
	uint32_t count,
	const void *buffer,
	const struct io_context *context,
	int content)
{
	struct ufs_mount_state *ms;
	int snapshot_locked = 0;
	int error;

	/* A write that names no mount has no snapshot state to consult. */
	ms = NULL;
	if (mountp != NULL)
		ms = mountp->m_data;

	/* A snapshot has to keep the old contents of what is overwritten. */
	if (ms != NULL && ms->snapshot_available) {
		mutex_lock(&ms->snapshot_lock);
		snapshot_locked = 1;
		ms->snapshot_io.context = context;
		error = drv_ufs_snapshot_preserve(&ms->snapshot, lba, count);
		ms->snapshot_io.context = NULL;

		/* Reports why the old contents could not be preserved. */
		if (error != 0) {
			mutex_unlock(&ms->snapshot_lock);

			/* Failed: nothing has been written yet. */
			return error;
		}
	}

	/* The batched journal takes the write into its running transaction. */
	if (ms != NULL && ms->j3.active) {
		error = j3_write(ms, mountp, lba, count, buffer, context, content);

		/* Releases the snapshot hold the preservation took. */
		if (snapshot_locked)
			mutex_unlock(&ms->snapshot_lock);

		/* Reports why the journal could not take the write. */
		if (error != 0)
			return error;

		/* Succeeded: the journal holds the write. */
		return 0;
	}

	/*
	 * A journalled volume checkpoints before writing outside the journal.
	 */
	if (ms != NULL && ms->journal_enabled) {
		mutex_lock(&ms->journal_lock);

		/* Runs the checkpoint the write has to follow. */
		error = journal_checkpoint_locked(mountp);
		if (error == 0) {
			journal_wait_readers(ms);
			ms->journal_io.context = context;
			error = drv_ufs_journal_commit(&ms->journal, lba,
						       buffer, count);
			ms->journal_io.context = NULL;
		}
		if (error != 0 && ms->journal.poisoned)
			stop_writing(mountp, "a journal commit failed", error);
		mutex_unlock(&ms->journal_lock);

		/* Releases the snapshot hold the preservation took. */
		if (snapshot_locked)
			mutex_unlock(&ms->snapshot_lock);

		/* Failed: reports why the checkpoint could not run. */
		return error;
	}

	io_stats_record(IO_UFS_WRITE, (uint64_t)count * UFS_SECTOR_SIZE);

	error = disk_sector_transfer(mountp->m_disk, lba, count, (void *)buffer,
				     1, 0, context);

	/* Releases the snapshot hold the preservation took. */
	if (snapshot_locked)
		mutex_unlock(&ms->snapshot_lock);

	/* Reports how the write itself went. */
	return error;
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

	/*
	 * Opens an ordered child context, so the sectors are written in order.
	 */
	error = io_context_child(&child, context, IO_CONTEXT_ORDERED);
	if (error != 0)
		return error;
	io_epoch_begin(&mountp->m_write_epoch);
	error = write_sectors_impl(mountp, lba, count, buffer, &child, 0);
	io_epoch_end(&mountp->m_write_epoch);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes file content, which a batched journal does not carry, in the
 * caller's ordering context.
 */
static int
write_content_sectors_context(
	struct mount *mountp,
	uint64_t lba,
	uint32_t count,
	const void *buffer,
	const struct io_context *context)
{
	struct io_context child;
	int error;

	/* Opens an ordered child context, so the sectors are written in order. */
	error = io_context_child(&child, context, IO_CONTEXT_ORDERED);
	if (error != 0)
		return error;

	/* Writes the sectors as content inside the mount's write epoch. */
	io_epoch_begin(&mountp->m_write_epoch);
	error = write_sectors_impl(mountp, lba, count, buffer, &child, 1);
	io_epoch_end(&mountp->m_write_epoch);

	/* Reports why the content could not be written. */
	if (error != 0)
		return error;

	/* Succeeded: the content is written. */
	return 0;
}

/* Writes sectors through the mount's own ordering context. */
static int
write_sectors(
	struct mount *mountp,
	uint64_t lba,
	uint32_t count,
	const void *buffer)
{
	int error;

	/* Writes through a context of its own. */
	error =
		write_sectors_context(mountp, lba, count, buffer, NULL);

	/* Reports how the write went. */
	return error;
}

/* Takes the private state this driver keeps beside a mount. */
static struct ufs_mount_state *
state(
	const struct mount *mountp)
{
	/* A mount that has gone away carries no state. */
	if (mountp == NULL)
		return NULL;

	/* Reports the state this mount was given when it was set up. */
	return mountp->m_data;
}

/*
 * Refuses every later write to the volume, and says why the first time.
 *
 * A volume stops taking writes when a rollback, a quota credit or a journal
 * step leaves its on-disk state uncertain, since a later write would build
 * on that state.  Until the next mount the volume answers EROFS, and
 * without this report nothing says why (BUG-288).  Only the transition is
 * reported, so a volume that keeps failing does not flood the log.
 */
static void
stop_writing(
	struct mount *mountp,
	const char *reason,
	int error)
{
	struct ufs_mount_state *ms;
	const char *volume;
	int was_writable;

	/* A mount that has gone away has nothing left to stop. */
	ms = state(mountp);
	if (ms == NULL)
		return;

	/* Clears the flag every write path tests before it touches the disk. */
	was_writable = ms->writable;
	ms->writable = 0;

	/* A volume already refusing writes has been reported. */
	if (!was_writable)
		return;

	/* Names the disk the volume lives on. */
	volume = "(no disk)";
	if (mountp->m_disk != NULL)
		volume = mountp->m_disk->d_name;

	/* Reports the transition and its cause. */
	kern_logf("ufs: %s: the volume refuses writes until it is mounted again: %s (error %d)\n",
		  volume,
		  reason,
		  error);
}

/* Takes the private inode this driver keeps beside a kernel one. */
static struct ufs_inode_info *
info(
	const struct inode *inode)
{
	/* The generic inode is embedded first, so the two convert directly. */
	return (struct ufs_inode_info *)(uintptr_t)inode;
}

/*
 * Copies a fully covered committed image without joining checkpoint device I/O.
 */
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
	 * copy.
	 */
	if (!ms->journal_enabled)
		return ENOENT;

	/* Takes a view of the pending group, if there is one. */
	error = drv_ufs_journal_view_acquire(&ms->journal, &view);
	if (error != 0)
		return error;
	error = drv_ufs_journal_view_copy(&view, first, count, buffer);
	drv_ufs_journal_view_release(&view);

	/*
	 * Releases before any caller falls back to the serialized home-read
	 * path.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reads metadata at any sector granularity through the committed redo owner. */
static int
read_metadata_sectors(
	struct mount *mountp,
	uint64_t first,
	uint32_t count,
	void *buffer)
{
	struct ufs_mount_state *ms;
	int error;

	/* Takes the mount state this call runs against. */
	ms = state(mountp);

	/* A pending group holds the current contents of these sectors. */
	error = journal_read_image(ms, first, count, buffer);
	if (error != ENOENT)
		return error;

	/* Only a journalled volume can have a pending group. */
	if (ms->journal_enabled)
		mutex_lock(&ms->journal_lock);

	/*
	 * A group that no longer covers the sectors sends the read to the disk.
	 */
	if (ms->journal_enabled &&
	    (ms->journal.pending_sequence != 0 || ms->journal.poisoned)) {
		error = drv_ufs_journal_read(&ms->journal, first, count,
					     buffer);
	} else {
		error = observed_disk_read(mountp->m_disk, first, count,
					   buffer);
	}

	/* A journalled volume reads metadata through the journal. */
	if (ms->journal_enabled)
		mutex_unlock(&ms->journal_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reads one file system block off the volume. */
static int
read_block(
	struct mount *mountp,
	uint64_t fragment,
	void *buffer)
{
	int error;
	const struct ufs_super *s = &state(mountp)->super;

	/* A fragment of zero names a hole, which reads as zeroes. */
	if (fragment == 0) {
		kern_memset(buffer, 0, s->bsize);

		/* Succeeded. */
		return 0;
	}

	/* Refuses a fragment that reaches past the end of the volume. */
	if (fragment >= s->size || s->frag > s->size - fragment)
		return EIO;

	/* Reads the sectors the fragment covers. */
	error =
		read_metadata_sectors(mountp, fragment << s->fsbtodb,
				      s->bsize / UFS_SECTOR_SIZE, buffer);

	/* Reports how the read went. */
	return error;
}

/* Writes one file system block to the volume. */
static int
write_block(
	struct mount *mountp,
	uint64_t fragment,
	const void *buffer)
{
	int error;
	const struct ufs_super *s = &state(mountp)->super;

	/* Refuses a fragment that names no block of this volume. */
	if (fragment == 0 || fragment >= s->size ||
	    s->frag > s->size - fragment) {
		/* Failed. */
		return EIO;
	}

	/* Writes the sectors the fragment covers. */
	error = write_sectors(mountp,
			      (uint64_t)fragment << s->fsbtodb,
			      s->bsize / UFS_SECTOR_SIZE,
			      buffer);

	/* Reports how the write went. */
	return error;
}

/* Counts populated content blocks separately from metadata operations. */
static int
read_content_block(
	struct mount *mountp,
	uint64_t fragment,
	void *buffer)
{
	int error;
	const struct ufs_super *s = &state(mountp)->super;

	/* A fragment outside the volume reads as a hole rather than failing. */
	if (fragment != 0 && fragment < s->size &&
	    s->frag <= s->size - fragment)
		io_stats_record(IO_UFS_CONTENT_READ, s->bsize);

	/* Reads the block the fragment names. */
	error = read_block(mountp, fragment, buffer);

	/* Reports how the read went. */
	return error;
}

/* Writes one block of file content to the volume. */
static int
write_content_block(
	struct mount *mountp,
	uint64_t fragment,
	const void *buffer)
{
	int error;
	const struct ufs_super *s = &state(mountp)->super;

	/* Refuses a fragment that names no block of this volume. */
	if (fragment == 0 || fragment >= s->size ||
	    s->frag > s->size - fragment) {
		/* Failed. */
		return EIO;
	}

	/* Counts the block among the content writes. */
	io_stats_record(IO_UFS_CONTENT_WRITE, s->bsize);

	/* Writes the block the fragment names as content. */
	error = write_content_sectors_context(mountp,
					      (uint64_t)fragment << s->fsbtodb,
					      s->bsize / UFS_SECTOR_SIZE,
					      buffer,
					      NULL);
	if (error != 0)
		return error;

	/* Succeeded: the block is written. */
	return 0;
}

/* Writes one block of content in the caller's ordering context. */
static int
write_content_context(
	struct mount *mountp,
	uint64_t fragment,
	const void *buffer,
	const struct io_context *context)
{
	int error;
	const struct ufs_super *super;

	/* Takes the geometry the fragment is measured against. */
	super = &state(mountp)->super;
	if (fragment == 0 ||
	    fragment >= super->size ||
	    super->frag > super->size - fragment) {
		/* Failed. */
		return EIO;
	}
	io_stats_record(IO_UFS_CONTENT_WRITE, super->bsize);

	/* Writes the sectors the fragment covers. */
	error = write_content_sectors_context(
		mountp,
		fragment << super->fsbtodb,
		super->bsize / UFS_SECTOR_SIZE,
		buffer,
		context);
	if (error != 0)
		return error;

	/* Succeeded: the block is written. */
	return 0;
}

/* Asks whether one bit of a bitmap is set. */
static int
bit_test(
	const uint8_t *map,
	uint32_t bit)
{
	/* The map is a bit per object, eight to a byte. */
	return (map[bit >> 3] & (uint8_t)(1U << (bit & 7U))) != 0;
}

/* Sets one bit of a bitmap. */
static void
bit_set(
	uint8_t *map,
	uint32_t bit)
{
	map[bit >> 3] |= (uint8_t)(1U << (bit & 7U));
}

/* Clears one bit of a bitmap. */
static void
bit_clear(
	uint8_t *map,
	uint32_t bit)
{
	map[bit >> 3] &= (uint8_t)~(1U << (bit & 7U));
}

/* Asks whether every fragment of the block at fragment is free in a group's map. */
static int
cg_block_free(
	const uint8_t *map,
	uint32_t fragment,
	uint32_t frag)
{
	uint32_t n;
	int free_bit;

	/* Looks at each fragment of the block in turn. */
	for (n = 0; n < frag; n++) {
		/* A used fragment means the block cannot be taken. */
		free_bit = bit_test(map, fragment + n);
		if (!free_bit)
			return 0;
	}

	/* Succeeded: the whole block is free. */
	return 1;
}

/*
 * Finds the first whole free block of a group's free map from start, a
 * fragment on the group's block grid, to end, and counts the free blocks
 * that follow it unbroken, up to want.  Reports that count with the
 * block's fragment in found, or zero when the range has no free block.
 */
static uint32_t
cg_free_blocks(
	const struct ufs_super *super,
	const uint8_t *map,
	uint32_t start,
	uint32_t end,
	uint32_t want,
	uint32_t *found)
{
	uint32_t fragment;
	uint32_t count;
	int bytewise;
	int free_block;

	/*
	 * A block of eight fragments that starts on a byte boundary is one
	 * byte of the map, all of whose bits are set when it is free.
	 */
	bytewise = 0;
	if (super->frag == 8U && start % 8U == 0)
		bytewise = 1;

	/* Walks the blocks for the first one with every fragment free. */
	for (fragment = start;
	     fragment < end && super->frag <= end - fragment;
	     fragment += super->frag) {
		if (bytewise) {
			/* The block's byte says it at once. */
			free_block = 0;
			if (map[fragment >> 3] == 0xffU)
				free_block = 1;
		} else {
			/* The block's fragments are asked one by one. */
			free_block = cg_block_free(map, fragment, super->frag);
		}
		if (free_block)
			break;
	}

	/* The range has no whole free block left. */
	if (fragment >= end || super->frag > end - fragment)
		return 0;

	/* Counts the free blocks that follow it, up to the number wanted. */
	count = 1;
	while (count < want &&
	       super->frag <= end - fragment - count * super->frag) {
		/* A block with a used fragment ends the run. */
		free_block = cg_block_free(map, fragment + count * super->frag, super->frag);
		if (!free_block)
			break;

		count++;
	}

	*found = fragment;

	/* Succeeded: the run's length in blocks. */
	return count;
}

/*
 * Reports where an allocation run's search of a group starts: where the
 * last run in the same group ended, when that is inside the group and on
 * its block grid from base, and otherwise base.
 */
static uint32_t
cg_search_start(
	const struct ufs_mount_state *ms,
	uint32_t cg,
	uint32_t base,
	uint32_t end)
{
	/* Another group starts from its beginning. */
	if (cg != ms->alloc_hint_cg)
		return base;

	/* A hint outside the group's data, or off its block grid, is not used. */
	if (ms->alloc_hint_fragment <= base ||
	    ms->alloc_hint_fragment >= end ||
	    (ms->alloc_hint_fragment - base) % ms->super.frag != 0)
		return base;

	/* Succeeded: the search resumes where the last run ended. */
	return ms->alloc_hint_fragment;
}

/* Reports the fragment a cylinder group begins at. */
static uint64_t
cgstart(
	const struct ufs_super *super,
	uint32_t cg)
{
	/* A cylinder group starts one group further in than the last. */
	return (uint64_t)cg * super->fpg + (uint64_t)super->cgoffset * (cg & ~super->cgmask);
}

/* Reports how many data fragments a cylinder group holds. */
static uint32_t
cg_ndblk(
	const struct ufs_super *super,
	uint32_t cg)
{
	uint64_t start;
	uint64_t remaining;

	start = cgstart(super, cg);

	/* A group that starts past the end of the volume holds nothing. */
	remaining = 0;
	if (start < super->size)
		remaining = super->size - start;

	/* A full group is the most this cylinder group can hold. */
	if (remaining > super->fpg)
		return super->fpg;

	/* The last group holds only what is left of the volume. */
	return (uint32_t)remaining;
}

/*
 * Names the cylinder groups a fragment can belong to, as FIRST up to END.
 *
 * Groups follow one another fpg fragments apart, so only the group the
 * fragment divided by fpg falls in can hold it; a volume grown to many groups
 * (ws299) would otherwise be searched group by group for every block pointer.
 * A volume that staggers its group metadata (cgoffset under a cgmask that
 * leaves bits) moves where each group starts, so there every group is still
 * a candidate and the caller searches them all, as before.
 */
static void
cg_search_range(
	const struct ufs_super *super,
	uint64_t fragment,
	uint32_t *first,
	uint32_t *end)
{
	uint64_t group;

	/* A staggered volume leaves every group a candidate. */
	if (super->cgoffset != 0 && ~super->cgmask != 0U) {
		*first = 0;
		*end = super->ncg;
		return;
	}

	/* A fragment past the last group belongs to none of them. */
	group = fragment / super->fpg;
	if (group >= super->ncg) {
		*first = super->ncg;
		*end = super->ncg;
		return;
	}

	/* Otherwise the one group the fragment falls in is the only candidate. */
	*first = (uint32_t)group;
	*end = (uint32_t)group + 1U;
}

/*
 * Resolves the CG through immutable redo before considering cached home bytes.
 */
static int
load_cg_image(
	struct mount *mountp,
	uint32_t cg,
	uint64_t fragment)
{
	struct ufs_mount_state *ms;
	int cached;
	int error;

	/*
	 * Drops the home-view identity when committed redo supplies the working
	 * image.
	 */
	ms = state(mountp);

	/* A pending group holds the current contents of this group. */
	error = journal_read_image(ms, fragment << ms->super.fsbtodb,
				   ms->super.bsize / UFS_SECTOR_SIZE, ms->cg);
	if (error == 0) {
		buf_view_release(&ms->cg_view);
		io_stats_record(IO_UFS_CG_HIT, ms->super.bsize);

		/* Succeeded. */
		return 0;
	}

	/* A failure other than an absent group ends the read. */
	if (error != ENOENT)
		return error;

	/*
	 * Serializes uncovered/uncertain reads and cache identity with
	 * checkpoint writes.
	 */
	if (ms->journal_enabled)
		mutex_lock(&ms->journal_lock);

	/* A journalled volume reads the group through the journal. */
	if (ms->journal_enabled &&
	    (ms->journal.pending_sequence != 0 || ms->journal.poisoned)) {
		buf_view_release(&ms->cg_view);
		error = drv_ufs_journal_read(
			&ms->journal, fragment << ms->super.fsbtodb,
			ms->super.bsize / UFS_SECTOR_SIZE, ms->cg);
	} else {
		/* Asks whether the cached group is still the one wanted. */
		cached = 0;
		if (ms->cg_valid && ms->active_cg == cg)
			cached = disk_view_matches(mountp->m_disk,
						   &ms->cg_view);

		if (cached) {
			io_stats_record(IO_UFS_CG_HIT, ms->super.bsize);

			/* The cached bytes are still the current ones. */
			error = 0;
		} else {
			buf_view_release(&ms->cg_view);
			io_stats_record(IO_UFS_CG_MISS, ms->super.bsize);
			io_stats_record(IO_UFS_READ, ms->super.bsize);

			/* Reads the group off the volume itself. */
			error = ufs_disk_read_view(mountp->m_disk,
						   fragment << ms->super.fsbtodb,
						   ms->super.bsize /
						   UFS_SECTOR_SIZE,
						   ms->cg, &ms->cg_view);
		}
	}

	/* A journalled volume keeps the image it read from. */
	if (ms->journal_enabled)
		mutex_unlock(&ms->journal_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Checks that a cylinder group header agrees with itself and the volume.
 *
 * The header says where its own bitmaps live inside the group block, so a
 * header that disagrees with itself would send every later map read outside
 * the buffer.  The offsets are therefore checked against each other, and the
 * free counts against the geometry, before anything reads through them.  The
 * checks are made in the order the fields depend on one another: a difference
 * of two offsets is only taken once they have been proved ordered.
 */
static int
cg_header_check(
	struct ufs_mount_state *ms,
	uint32_t cg,
	uint32_t ndblk)
{
	uint32_t inode_map_bytes;
	uint32_t free_map_bytes;
	uint32_t magic;
	uint32_t recorded_cg;
	uint32_t recorded_ndblk;
	uint32_t ndir;
	uint32_t nbfree;
	uint32_t nifree;
	uint32_t nffree;

	/* The magic number that marks the start of a cylinder group. */
	magic = drv_ufs_get32(ms->cg, UFS_CG_MAGIC, ms->super.swapped);

	/* A group without its magic number means the volume is corrupt. */
	if (magic != UFS_CG_MAGIC_VALUE)
		return EINVAL;	/* Failed. */

	/* Which of the volume's groups the header says this one is. */
	recorded_cg = drv_ufs_get32(ms->cg, UFS_CG_CGX, ms->super.swapped);

	/* A header naming another group is not the one that was read. */
	if (recorded_cg != cg)
		return EINVAL;	/* Failed. */

	/* How many data fragments the header says the group holds. */
	recorded_ndblk = drv_ufs_get32(ms->cg, UFS_CG_NDBLK,
				       ms->super.swapped);

	/* That has to agree with what the volume geometry gives. */
	if (recorded_ndblk != ndblk)
		return EINVAL;	/* Failed. */

	/* The used-inode map has to start inside the group block. */
	if (ms->cg_iusedoff >= ms->super.bsize)
		return EINVAL;	/* Failed. */

	/* And the free-fragment map has to end inside it. */
	if (ms->cg_freeoff > ms->super.bsize)
		return EINVAL;	/* Failed. */

	/* Nothing the header describes may reach past the group size. */
	if (ms->cg_nextfreeoff > ms->super.cgsize)
		return EINVAL;	/* Failed. */

	/* The used-inode map comes before the free-fragment map. */
	if (ms->cg_iusedoff > ms->cg_freeoff)
		return EINVAL;	/* Failed. */

	/* One bit per inode of the group, which is what fits between them. */
	inode_map_bytes = (ms->super.ipg + 7U) / 8U;

	/* A map the room between the two offsets could not hold is wrong. */
	if (inode_map_bytes > ms->cg_freeoff - ms->cg_iusedoff)
		return EINVAL;	/* Failed. */

	/* The free-fragment map in turn comes before whatever follows it. */
	if (ms->cg_freeoff > ms->cg_nextfreeoff)
		return EINVAL;	/* Failed. */

	/* One bit per fragment of the group, which is what fits after it. */
	free_map_bytes = (ms->super.fpg + 7U) / 8U;

	/* A map that room could not hold is wrong in the same way. */
	if (free_map_bytes > ms->cg_nextfreeoff - ms->cg_freeoff)
		return EINVAL;	/* Failed. */

	/* How many of the group's inodes name directories. */
	ndir = drv_ufs_get32(ms->cg, UFS_CG_NDIR, ms->super.swapped);

	/* A group cannot hold more directories than it holds inodes. */
	if (ndir > ms->super.ipg)
		return EINVAL;	/* Failed. */

	/* How many of the group's inode numbers are still free. */
	nifree = drv_ufs_get32(ms->cg, UFS_CG_NIFREE, ms->super.swapped);

	/* Nor can more of its inodes be free than it has. */
	if (nifree > ms->super.ipg)
		return EINVAL;	/* Failed. */

	/* How many whole blocks of the group are still free. */
	nbfree = drv_ufs_get32(ms->cg, UFS_CG_NBFREE, ms->super.swapped);

	/* Nor more whole blocks than the data area is divided into. */
	if (nbfree > ndblk / ms->super.frag)
		return EINVAL;	/* Failed. */

	/* How many loose fragments outside those blocks are free. */
	nffree = drv_ufs_get32(ms->cg, UFS_CG_NFFREE, ms->super.swapped);

	/* Nor more loose fragments than the data area holds at all. */
	if (nffree > ndblk)
		return EINVAL;	/* Failed. */

	/* And the two free counts together still have to fit in the group. */
	if ((uint64_t)nbfree * ms->super.frag + nffree > ndblk)
		return EINVAL;	/* Failed. */

	/* Succeeded. */
	return 0;
}

/* Reads one cylinder group into the mount buffer and checks it. */
static int
load_cg_locked(
	struct mount *mountp,
	uint32_t cg)
{
	struct ufs_mount_state *ms;
	uint32_t ndblk;
	uint64_t fragment;
	int error;

	/* Takes the mount state this call runs against. */
	ms = state(mountp);

	/* Refuses a group number this volume has not got. */
	if (cg >= ms->super.ncg)
		return EINVAL;

	/* The fragment the cylinder group starts at. */
	fragment = cgstart(&ms->super, cg) + ms->super.cblkno;

	/* A group starting past the end of the volume is not there. */
	if (fragment >= ms->super.size) {
		/* Failed. */
		return EINVAL;
	}

	/* Nor is one whose block would run off the end of the volume. */
	if (ms->super.frag > ms->super.size - fragment) {
		/* Failed. */
		return EINVAL;
	}

	/* Reads the group into the mount buffer. */
	error = load_cg_image(mountp, cg, fragment);
	if (error != 0) {
		ms->cg_valid = 0;

		/* Failed. */
		return error;
	}

	/* Nothing may be read through the buffer until it has been checked. */
	ms->cg_valid = 0;

	/* How many data fragments the geometry gives this group. */
	ndblk = cg_ndblk(&ms->super, cg);

	/* Where the used-inode map starts inside the group block. */
	ms->cg_iusedoff = drv_ufs_get32(ms->cg, UFS_CG_IUSEDOFF,
					ms->super.swapped);

	/* Where the free-fragment map starts, just past that map. */
	ms->cg_freeoff = drv_ufs_get32(ms->cg, UFS_CG_FREEOFF,
				       ms->super.swapped);

	/* And where everything the group header describes ends. */
	ms->cg_nextfreeoff = drv_ufs_get32(ms->cg, UFS_CG_NEXTFREEOFF,
					   ms->super.swapped);

	/* Refuses a header whose own fields do not hold together. */
	error = cg_header_check(ms, cg, ndblk);
	if (error != 0) {
		buf_view_release(&ms->cg_view);

		/* Failed. */
		return error;
	}

	ms->active_cg = cg;
	ms->cg_valid = 1;
	ms->cg_dirty = 0;

	/* Succeeded. */
	return 0;
}

/* Tests whether a pointer names a data block of this volume. */
static int
valid_inode_fragment(
	const struct ufs_super *super,
	uint64_t fragment)
{
	uint64_t start;
	uint32_t ndblk;
	uint32_t cg;
	uint32_t end;

	/* A fragment of zero names no block. */
	if (fragment == 0)
		return 1;

	/* Finds the cylinder group the fragment would live in. */
	cg_search_range(super, fragment, &cg, &end);
	for (; cg < end; cg++) {
		start = cgstart(super, cg);

		/* The data blocks of a group end where its metadata begins. */
		ndblk = cg_ndblk(super, cg);
		if (fragment >= start + super->dblkno &&
		    fragment < start + ndblk &&
		    super->frag <= start + ndblk - fragment) {
			/*
			 * Reports that the fragment names a block of this
			 * volume.
			 */
			return 1;
		}
	}

	/* Succeeded. */
	return 0;
}

/* Builds the superblock image the volume totals are written in. */
static int
prepare_super_summaries(
	struct mount *mountp,
	uint8_t *buffer)
{
	struct ufs_mount_state *ms;
	int error;

	/* Takes the mount state this call runs against. */
	ms = state(mountp);

	/* Reads the superblock the summaries are written into. */
	error = read_metadata_sectors(mountp,
				      UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE,
				      UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE,
				      buffer);
	if (error == 0) {
		/* How many directories the whole volume holds. */
		drv_ufs_put64(buffer, UFS_FS_CSTOTAL_NDIR,
			      ms->super.cstotal_ndir, ms->super.swapped);

		/* How many whole blocks of it are still free. */
		drv_ufs_put64(buffer, UFS_FS_CSTOTAL_NBFREE,
			      ms->super.cstotal_nbfree, ms->super.swapped);

		/* How many inode numbers are still free. */
		drv_ufs_put64(buffer, UFS_FS_CSTOTAL_NIFREE,
			      ms->super.cstotal_nifree, ms->super.swapped);

		/* And how many loose fragments outside whole blocks. */
		drv_ufs_put64(buffer, UFS_FS_CSTOTAL_NFFREE,
			      ms->super.cstotal_nffree, ms->super.swapped);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Writes an independently prepared summary for synchronous metadata callers. */
static int
write_super_summaries(
	struct mount *mountp)
{
	uint8_t *buffer;
	int error;

	/* Takes the staging the superblock is written from. */
	buffer = kern_malloc(UFS_SBLOCK_SIZE);
	if (buffer == NULL)
		return ENOMEM;

	/* Fills it with the summaries as they now stand. */
	error = prepare_super_summaries(mountp, buffer);
	if (error == 0) {
		error = write_sectors(
			mountp, UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE,
			UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE, buffer);
	}

	kern_free(buffer);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes the mount-owned CG image immediately; failed ownership remains
 * explicit.
 */
static int
write_cg(
	struct mount *mountp)
{
	struct ufs_mount_state *ms;
	struct kern_test_fault_result fault;
	int error;

	/*
	 * Releases optional copy pins before writing or entering nested cache
	 * paths.
	 */
	ms = state(mountp);
	ms->cg_valid = 0;
	ms->cg_dirty = 1;
	buf_view_release(&ms->cg_view);

	/* The injected fault stands in for a cylinder-group write failure. */
	if (KERN_TEST_FAULT(KERN_TEST_FAULT_UFS_CG_WRITE,
			    UINT32_MAX,
			    UINT32_MAX,
			    &fault)) {
		/* Reports the injected error, or a device error by default. */
		if (fault.error != 0)
			return fault.error;	/* Failed. */

		return EIO;	/* Failed. */
	}

	/* Writes the cylinder group back. */
	error = write_sectors(mountp,
			      (cgstart(&ms->super, ms->active_cg) + ms->super.cblkno)
			      << ms->super.fsbtodb,
			      ms->super.bsize / UFS_SECTOR_SIZE,
			      ms->cg);
	if (error == 0)
		error = write_super_summaries(mountp);
	if (error == 0)
		ms->cg_dirty = 0;

	/* Preserves the immediate writer's original error convention. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Caller holds ms->lock and has already restored the in-memory CG image. */
static int
write_cg_rollback(
	struct mount *mountp,
	int original_error)
{
	int rollback;

	rollback = write_cg(mountp);

	/* A failed rollback leaves the volume unwritable. */
	if (rollback != 0) {
		stop_writing(mountp, "a cylinder group could not be rolled back", rollback);

		/* Failed. */
		return rollback;
	}

	/* Reports the failure that made the rollback necessary. */
	return original_error;
}

/* Adds or takes one directory from the counts a group keeps. */
static int
adjust_directory_count(
	struct mount *mountp,
	uint32_t ino,
	int delta)
{
	struct ufs_mount_state *ms;
	uint32_t count;
	uint32_t new_count;
	uint32_t cg;
	uint64_t old_total;
	uint64_t new_total;
	int error;

	ms = state(mountp);
	cg = ino / ms->super.ipg;

	mutex_lock(&ms->lock);

	/* Reads the cylinder group the count belongs to. */
	error = load_cg_locked(mountp, cg);
	if (error != 0) {
		mutex_unlock(&ms->lock);

		/* Failed. */
		return error;
	}

	count = drv_ufs_get32(ms->cg, UFS_CG_NDIR, ms->super.swapped);
	old_total = ms->super.cstotal_ndir;

	/* Refuses a change the count could not hold. */
	if ((delta < 0 && count == 0) || (delta > 0 && count == UINT32_MAX)) {
		error = EIO;
	} else {
		if (delta < 0) {
			/* One directory has left the cylinder group. */
			new_count = count - 1U;
			new_total = old_total - 1U;
		} else {
			/* One directory has joined the cylinder group. */
			new_count = count + 1U;
			new_total = old_total + 1U;
		}

		drv_ufs_put32(ms->cg, UFS_CG_NDIR, new_count, ms->super.swapped);

		ms->super.cstotal_ndir = new_total;

		/* Writes the cylinder group back with its new count. */
		error = write_cg(mountp);
		if (error != 0) {
			drv_ufs_put32(ms->cg, UFS_CG_NDIR, count,
				      ms->super.swapped);
			ms->super.cstotal_ndir = old_total;
			error = write_cg_rollback(mountp, error);
		}
	}

	mutex_unlock(&ms->lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the current time as the quota records measure it. */
static uint64_t
quota_now(
	void)
{
	time_t seconds = 0;
	long nanoseconds = 0;

	clock_realtime(&seconds, &nanoseconds);
	(void)nanoseconds;

	/* A timestamp before the epoch is stored as the epoch itself. */
	if (seconds > 0)
		return (uint64_t)seconds;

	return 0;
}

/*
 * Zeroes a newly allocated block, because a caller may read it before
 * writing.  The zeroes are content: a journal need not log them, and they
 * reach the device before the commit that makes the block reachable.  The
 * journal's own file, which nothing reads, is not zeroed here; its
 * indirect blocks, which a lookup does read, are zeroed where they are
 * taken (allocate_indirect_block).
 */
static int
zero_new_block(
	struct mount *mountp,
	struct ufs_mount_state *ms,
	uint64_t fragment)
{
	int error;

	/* Leaves the journal's own blocks as they are. */
	if (ms->j3.creating)
		return 0;

	/* Writes zeroes over the whole block. */
	error = zero_block(mountp, ms, fragment);
	if (error != 0)
		return error;

	/* Succeeded: the block reads as zeroes. */
	return 0;
}

/* Writes zeroes over one block, as content. */
static int
zero_block(
	struct mount *mountp,
	struct ufs_mount_state *ms,
	uint64_t fragment)
{
	uint8_t *zero;
	int error;

	/* Takes a block of zeroes. */
	zero = kern_calloc(1, ms->super.bsize);
	if (zero == NULL)
		return ENOMEM;

	/* Writes it over the new block. */
	error = write_content_block(mountp, fragment, zero);

	/* Gives the zeroes back, written or not. */
	kern_free(zero);

	/* Reports why the block could not be zeroed. */
	if (error != 0)
		return error;

	/* Succeeded: the block reads as zeroes. */
	return 0;
}

/* Takes one block, charging it to an owner's quota. */
static int
allocate_block_compat(
	struct mount *mountp,
	uid_t uid,
	gid_t gid,
	uint64_t *result)
{
	uint64_t absolute;
	uint32_t ndblk;
	struct ufs_mount_state *ms;
	uint8_t *map;
	struct quota_charge charge;
	uint32_t fragment;
	uint32_t n;
	uint32_t cg;
	uint32_t attempt;
	uint64_t old_total;
	int error;

	/* Takes the mount state this call runs against. */
	ms = state(mountp);

	/* Charges the block against the owner quota before taking it. */
	error = quota_reserve(&ms->quota, uid, gid, 1, 0, quota_now(), &charge);
	if (error != 0)
		return error;
	error = ENOSPC;

	mutex_lock(&ms->lock);

	/* Tries every cylinder group, starting at the preferred one. */
	for (attempt = 0; attempt < ms->super.ncg; attempt++) {
		cg = (ms->rotor_cg + attempt) % ms->super.ncg;

		/* Reads the cylinder group being searched. */
		error = load_cg_locked(mountp, cg);
		if (error != 0)
			break;

		error = ENOSPC;
		map = ms->cg + ms->cg_freeoff;
		ndblk = cg_ndblk(&ms->super, cg);

		/*
		 * Walks the group for a run of free fragments a whole block
		 * wide.
		 */
		for (fragment = (ms->super.dblkno + ms->super.frag - 1U) &
			     ~(ms->super.frag - 1U);
		     fragment + ms->super.frag <= ndblk;
		     fragment += ms->super.frag) {
			uint32_t free;

			/* Counts the free fragments that follow this one. */
			for (n = 0;
			     n < ms->super.frag && bit_test(map, fragment + n);
			     n++)
				;

			/* A run shorter than a block cannot hold one. */
			if (n != ms->super.frag)
				continue;

			/* Marks every fragment of the run as used. */
			for (n = 0; n < ms->super.frag; n++)
				bit_clear(map, fragment + n);

			old_total = ms->super.cstotal_nbfree;

			/* The group has no free blocks left to count down. */
			free = drv_ufs_get32(ms->cg, UFS_CG_NBFREE, ms->super.swapped);
			if (free == 0) {
				/*
				 * Puts the run back when the counts could not
				 * be lowered.
				 */
				for (n = 0; n < ms->super.frag; n++)
					bit_set(map, fragment + n);
				break;
			}

			drv_ufs_put32(ms->cg,
				      UFS_CG_NBFREE, free - 1U,
				      ms->super.swapped);

			ms->super.cstotal_nbfree = old_total - 1U;

			/*
			 * Writes the cylinder group back with its new free map.
			 */
			error = write_cg(mountp);

			/* Zeroes the block once the group records it as used. */
			absolute = cgstart(&ms->super, cg) + fragment;
			if (error == 0)
				error = zero_new_block(mountp, ms, absolute);

			/*
			 * Puts the run back when the group could not be written
			 * or the block could not be zeroed.
			 */
			if (error != 0) {
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

		/* A failure other than a full group ends the search. */
		if (error != ENOSPC)
			break;
	}

	mutex_unlock(&ms->lock);

	/* Gives the quota charge back when no block was taken. */
	if (error == 0)
		quota_commit(&charge);
	else
		quota_rollback(&charge);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

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

/*
 * Uses the unchanged allocation/zero/rollback implementation in this initial
 * stage.
 */
static int
allocation_allocate(
	struct ufs_allocation *context,
	uint64_t *result)
{
	int error;

	/* A context that was never opened allocates nothing. */
	if (!context->active)
		return EINVAL;

	io_stats_record(IO_UFS_ALLOCATE, state(context->mountp)->super.bsize);

	/* Reports the failure. */
	error = allocate_block_compat(context->mountp, context->uid,
				      context->gid, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Closes an already-persisted allocation; there is no deferred state in p010.
 */
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
allocate_block(
	struct mount *mountp,
	uid_t uid,
	gid_t gid,
	uint64_t *result)
{
	struct ufs_allocation context;
	int error;

	allocation_begin(&context, mountp, uid, gid);

	/* Takes one block through a context of its own. */
	error = allocation_allocate(&context, result);
	if (error != 0) {
		allocation_abort(&context);

		/* Failed. */
		return error;
	}

	allocation_commit(&context);

	/* Succeeded. */
	return 0;
}

/*
 * Takes a block for an indirect level of a file.
 *
 * A lookup takes every non-zero entry of an indirect block for a block of
 * the file, so a new one has to read as zeroes.  The allocator zeroes every
 * block except the journal's own, whose data nothing reads; an indirect
 * block of the journal is zeroed here.  Without it, the entries the block
 * kept from its former owner were followed as blocks of the journal, and
 * cutting the journal later failed on them (BUG-294).
 */
static int
allocate_indirect_block(
	struct inode *inode,
	uint64_t *result)
{
	struct ufs_mount_state *ms;
	uint64_t fragment;
	int rollback;
	int error;

	/* Takes the mount state this call runs against. */
	ms = state(inode->i_mount);

	/* Takes the block, charged to the file's owner. */
	error = allocate_block(inode->i_mount, inode->i_uid, inode->i_gid, &fragment);
	if (error != 0)
		return error;

	/* Zeroes it when the allocator, filling the journal, left it as it was. */
	if (ms->j3.creating) {
		error = zero_block(inode->i_mount, ms, fragment);
		if (error != 0) {
			/* Nothing names the block yet, so it is given straight back. */
			rollback = free_block(inode->i_mount, fragment, inode->i_uid, inode->i_gid);
			if (rollback != 0)
				stop_writing(inode->i_mount, "an indirect block that could not be zeroed could not be freed", rollback);

			/* Failed: reports why the block could not be zeroed. */
			return error;
		}
	}

	/* Succeeded: the block is the caller's, and reads as zeroes. */
	*result = fragment;
	return 0;
}

/* Asks whether a fragment names a whole block inside the volume. */
static int
fragment_in_volume(
	const struct ufs_super *super,
	uint64_t fragment)
{
	/* The fragment past the last one of the volume. */
	if (fragment >= super->size)
		return 0;

	/* A block that would run past the end of the volume. */
	if (super->frag > super->size - fragment)
		return 0;

	/* Succeeded: the block lies inside the volume. */
	return 1;
}

/* Gives one block back, crediting its owner's quota. */
static int
free_block(
	struct mount *mountp,
	uint64_t fragment,
	uid_t uid,
	gid_t gid)
{
	uint64_t start;
	uint32_t ndblk;
	struct ufs_mount_state *ms;
	uint8_t *map;
	uint32_t n;
	uint32_t free;
	uint32_t cg;
	uint32_t end;
	uint32_t local;
	uint64_t old_total;
	int already_free;
	int released;
	int error;

	ms = state(mountp);
	local = 0;

	/* Finds the cylinder group the block being freed lives in. */
	cg_search_range(&ms->super, fragment, &cg, &end);
	for (; cg < end; cg++) {
		start = cgstart(&ms->super, cg);

		/* The data blocks of a group end where its metadata begins. */
		ndblk = cg_ndblk(&ms->super, cg);
		if (fragment >= start + ms->super.dblkno &&
		    fragment + ms->super.frag <= start + ndblk) {
			local = (uint32_t)(fragment - start);
			break;
		}
	}

	/* A block that belongs to no group means the pointer is corrupt. */
	if (cg == end)
		return EIO;

	mutex_lock(&ms->lock);

	/* Reads that cylinder group so its free map can be changed. */
	error = load_cg_locked(mountp, cg);
	if (error != 0) {
		mutex_unlock(&ms->lock);

		/* Failed. */
		return error;
	}

	/* Refuses a block the free map already calls free. */
	map = ms->cg + ms->cg_freeoff;
	for (n = 0; n < ms->super.frag; n++) {
		/* Asks the free map whether it already holds this fragment. */
		already_free = bit_test(map, local + n);

		/* A block that is already free must not be freed twice. */
		if (already_free) {
			mutex_unlock(&ms->lock);

			/* Failed. */
			return EIO;
		}
	}

	/* Reads the free-block count this release raises. */
	free = drv_ufs_get32(ms->cg, UFS_CG_NBFREE, ms->super.swapped);
	if (free == UINT32_MAX) {
		mutex_unlock(&ms->lock);

		/* Failed. */
		return EIO;
	}

	old_total = ms->super.cstotal_nbfree;

	/* Marks every fragment of the block as free. */
	for (n = 0; n < ms->super.frag; n++)
		bit_set(map, local + n);

	drv_ufs_put32(ms->cg, UFS_CG_NBFREE, free + 1U, ms->super.swapped);

	ms->super.cstotal_nbfree = old_total + 1U;

	/* Writes the cylinder group back with its new free map. */
	error = write_cg(mountp);
	if (error != 0) {
		/*
		 * Puts the block back in use when the group could not be
		 * written.
		 */
		for (n = 0; n < ms->super.frag; n++)
			bit_clear(map, local + n);
		drv_ufs_put32(ms->cg, UFS_CG_NBFREE, free, ms->super.swapped);
		ms->super.cstotal_nbfree = old_total;
		error = write_cg_rollback(mountp, error);
	}

	/*
	 * Keeps a block freed by the running transaction from reaching disk
	 * with a new owner's content before the free is committed.
	 */
	if (error == 0 && ms->j3.active)
		j3_freed_add(ms, fragment);

	mutex_unlock(&ms->lock);

	/* Gives the block back to the owner quota. */
	if (error == 0) {
		released = quota_release(&ms->quota, uid, gid, 1, 0);

		/* An account that cannot be credited must not be written to. */
		if (released != 0) {
			stop_writing(mountp, "a freed block could not be credited to its quota", released);

			/* Failed. */
			return EIO;
		}
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Takes one free inode number, charging it to an owner. */
static int
allocate_inode_number(
	struct mount *mountp,
	uid_t uid,
	gid_t gid,
	uint32_t *number)
{
	uint32_t free;
	struct ufs_mount_state *ms;
	struct quota_charge charge;
	uint8_t *map;
	uint32_t ino;
	uint32_t cg;
	uint32_t attempt;
	uint64_t old_total;
	int used;
	int error;

	/* Takes the mount state this call runs against. */
	ms = state(mountp);

	/* Charges the inode against the owner quota before taking it. */
	error = quota_reserve(&ms->quota, uid, gid, 0, 1, quota_now(), &charge);
	if (error != 0)
		return error;

	error = ENOSPC;
	mutex_lock(&ms->lock);

	/* Tries every cylinder group, starting at the preferred one. */
	for (attempt = 0; attempt < ms->super.ncg; attempt++) {
		cg = (ms->rotor_cg + attempt) % ms->super.ncg;

		/* Reads the cylinder group being searched. */
		error = load_cg_locked(mountp, cg);
		if (error != 0)
			break;

		error = ENOSPC;

		/* Walks the group for an inode number that is free. */
		map = ms->cg + ms->cg_iusedoff;
		for (ino = cg == 0 ? UFS_ROOT_INO + 1U : 0U;
		     ino < ms->super.ipg;
		     ino++) {
			/* Asks the used map whether this number is taken. */
			used = bit_test(map, ino);

			/* A clear bit in the used map is a free number. */
			if (!used) {
				/*
				 * Reads the free-inode count this allocation
				 * lowers.
				 */
				free = drv_ufs_get32(ms->cg,
						     UFS_CG_NIFREE,
						     ms->super.swapped);
				if (free == 0)
					break;

				old_total = ms->super.cstotal_nifree;
				bit_set(map, ino);

				drv_ufs_put32(ms->cg,
					      UFS_CG_NIFREE,
					      free - 1U,
					      ms->super.swapped);

				ms->super.cstotal_nifree = old_total - 1U;

				/*
				 * Writes the cylinder group back with its new
				 * used map.
				 */
				error = write_cg(mountp);
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

		/* A failure other than a full group ends the search. */
		if (error != ENOSPC)
			break;
	}

	mutex_unlock(&ms->lock);

	/* Gives the quota charge back when no number was taken. */
	if (error == 0)
		quota_commit(&charge);
	else
		quota_rollback(&charge);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Gives one inode number back, crediting its owner. */
static int
free_inode_number(
	struct mount *mountp,
	uint32_t number,
	uid_t uid,
	gid_t gid)
{
	struct ufs_mount_state *ms = state(mountp);
	uint8_t *map;
	uint32_t free, cg = number / ms->super.ipg, local = number % ms->super.ipg;
	uint64_t old_total;
	int used;
	int released;
	int error;

	/* Refuses the root, or a number outside this volume. */
	if (number <= UFS_ROOT_INO || cg >= ms->super.ncg)
		return EIO;
	mutex_lock(&ms->lock);

	/* Reads the cylinder group the number lives in. */
	error = load_cg_locked(mountp, cg);
	if (error != 0) {
		mutex_unlock(&ms->lock);

		/* Failed. */
		return error;
	}

	/* The used map this release clears a bit in. */
	map = ms->cg + ms->cg_iusedoff;

	/* Asks the used map whether this number was ever handed out. */
	used = bit_test(map, local);

	/* Releasing a number that is already free would corrupt the count. */
	if (!used) {
		mutex_unlock(&ms->lock);

		/* Failed. */
		return EIO;
	}

	/* Reads the free-inode count this release raises. */
	free = drv_ufs_get32(ms->cg, UFS_CG_NIFREE, ms->super.swapped);
	if (free == UINT32_MAX) {
		mutex_unlock(&ms->lock);

		/* Failed. */
		return EIO;
	}

	old_total = ms->super.cstotal_nifree;
	bit_clear(map, local);
	drv_ufs_put32(ms->cg, UFS_CG_NIFREE, free + 1U, ms->super.swapped);
	ms->super.cstotal_nifree = old_total + 1U;

	/* Writes the cylinder group back with its new used map. */
	error = write_cg(mountp);
	if (error != 0) {
		bit_set(map, local);
		drv_ufs_put32(ms->cg, UFS_CG_NIFREE, free, ms->super.swapped);
		ms->super.cstotal_nifree = old_total;
		error = write_cg_rollback(mountp, error);
	}

	mutex_unlock(&ms->lock);

	/* Gives the inode back to the owner quota. */
	if (error == 0) {
		released = quota_release(&ms->quota, uid, gid, 0, 1);

		/* An account that cannot be credited must not be written to. */
		if (released != 0) {
			stop_writing(mountp, "a freed inode could not be credited to its quota", released);

			/* Failed. */
			return EIO;
		}
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reads one pointer through the common bounded cache with sector-sized stack
 * scratch.
 */
static int
indirect_entry(
	struct mount *mountp,
	uint64_t fragment,
	uint32_t index,
	uint64_t *result)
{
	int error;

	/* Reads without a memo. */
	error = indirect_entry_memo(mountp, fragment, index, result, NULL, 0);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reads one pointer as indirect_entry() does; with a memo, the sector of a
 * level (counted from the inode's pointer, the first levels only) is read
 * once and kept for the next pointer in it.
 */
static int
indirect_entry_memo(
	struct mount *mountp,
	uint64_t fragment,
	uint32_t index,
	uint64_t *result,
	struct ufs_bmap_memo *memo,
	unsigned level)
{
	const struct ufs_super *super;
	uint8_t sector[UFS_SECTOR_SIZE];
	uint64_t byte_offset;
	uint64_t lba;
	int error;

	/* Rejects invalid mappings before calculating the containing sector. */
	super = &state(mountp)->super;

	/* A level that is not there names a hole. */
	if (fragment == 0) {
		*result = 0;
		/* Succeeded. */
		return 0;
	}

	/* Refuses an index or a fragment this volume could not address. */
	if (index >= super->nindir || fragment >= super->size ||
	    super->frag > super->size - fragment) {
		/* Failed. */
		return EIO;
	}
	byte_offset = (uint64_t)index * 8U;
	lba = ((uint64_t)fragment << super->fsbtodb) +
		byte_offset / UFS_SECTOR_SIZE;
	/* A sector the memo holds needs no read. */
	if (memo != NULL && level < UFS_BMAP_MEMO_LEVELS &&
	    memo->valid[level] && memo->lba[level] == lba) {
		*result = drv_ufs_get64(memo->sector[level],
					(size_t)(byte_offset % UFS_SECTOR_SIZE),
					super->swapped);
		return 0;
	}
	io_stats_record(IO_UFS_INDIRECT_WINDOW, sizeof(sector));

	/* Reads the sector the entry lives in. */
	error = read_metadata_sectors(mountp, lba, 1, sector);
	if (error == 0) {
		*result = drv_ufs_get64(sector,
					(size_t)(byte_offset % UFS_SECTOR_SIZE),
					super->swapped);
		if (memo != NULL && level < UFS_BMAP_MEMO_LEVELS) {
			kern_memcpy(memo->sector[level], sector, UFS_SECTOR_SIZE);
			memo->lba[level] = lba;
			memo->valid[level] = 1;
		}
	}

	/*
	 * Releases the common cache pin inside disk_read before returning the
	 * pointer.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Inode numbers stay stable while a claim excludes unlink and reuse. */
static int
ufs_backing_identity(struct inode *inode, struct disk **disk, uint64_t *object)
{
	if (inode == NULL || disk == NULL || object == NULL)
		return EINVAL;
	if (inode->i_type != INODE_REG || inode->i_mount == NULL ||
	    inode->i_mount->m_disk == NULL || inode->i_mount->m_type == NULL ||
	    inode->i_mount->m_type->file_backing_identity != ufs_backing_identity)
		return EOPNOTSUPP;
	/* A virtual file-backed disk is not a canonical physical swap volume. */
	if ((inode->i_mount->m_disk->d_flags & DISK_FILE_BACKED) != 0)
		return EOPNOTSUPP;
	if (inode->i_ino == 0)
		return EIO;
	*disk = inode->i_mount->m_disk;
	*object = inode->i_ino;
	return 0;
}

/* Validates one fully allocated block before exporting or following it. */
static int
ufs_backing_block(struct mount *mountp, uint64_t fragment)
{
	struct ufs_mount_state *ms;
	const struct ufs_super *super;
	uint64_t group;
	uint64_t summary_fragments;
	uint32_t local;
	uint32_t n;
	int error;

	ms = state(mountp);
	super = &ms->super;
	if (fragment == 0 || fragment >= super->size ||
	    super->frag > super->size - fragment)
		return EIO;
	group = fragment / super->fpg;
	local = (uint32_t)(fragment % super->fpg);
	if (group >= super->ncg || local < super->dblkno ||
	    local % super->frag != 0 || super->frag > super->fpg - local)
		return EIO;

	/* Cylinder summaries occupy otherwise data-addressable fragments. */
	summary_fragments = ((uint64_t)super->cssize + super->fsize - 1U) /
	    super->fsize;
	if (summary_fragments != 0 &&
	    (super->csaddr >= super->size ||
	    summary_fragments > super->size - super->csaddr))
		return EIO;
	if (summary_fragments != 0 && fragment < super->csaddr + summary_fragments &&
	    super->csaddr < fragment + super->frag)
		return EIO;

	/* Every fragment must already belong to an allocation. */
	error = load_cg_locked(mountp, (uint32_t)group);
	if (error != 0)
		return error;
	for (n = 0; n < super->frag; n++) {
		if (bit_test(ms->cg + ms->cg_freeoff, local + n))
			return EIO;
	}
	return 0;
}

/* Follows only allocated indirect blocks; ordinary reads keep their own path. */
static int
ufs_backing_map(struct inode *inode, uint64_t logical, uint64_t *result)
{
	const struct ufs_super *super;
	struct ufs_inode_info *ui;
	uint64_t span;
	uint64_t fragment;
	uint64_t divisor;
	uint32_t index;
	unsigned level;
	unsigned depth;
	unsigned n;
	int error;

	super = &state(inode->i_mount)->super;
	ui = info(inode);
	if (logical < UFS_NDADDR) {
		fragment = ui->direct[logical];
	} else {
		logical -= UFS_NDADDR;
		span = super->nindir;
		for (level = 0; level < UFS_NIADDR; level++) {
			if (logical < span)
				break;
			logical -= span;
			if (span > UINT64_MAX / super->nindir)
				return EOVERFLOW;
			span *= super->nindir;
		}
		if (level == UFS_NIADDR)
			return EFBIG;
		fragment = ui->indirect[level];
		for (depth = level + 1U; depth != 0; depth--) {
			error = ufs_backing_block(inode->i_mount, fragment);
			if (error != 0)
				return error;
			divisor = 1;
			for (n = 1; n < depth; n++)
				divisor *= super->nindir;
			index = (uint32_t)(logical / divisor);
			logical %= divisor;
			error = indirect_entry(inode->i_mount, fragment, index, &fragment);
			if (error != 0)
				return error;
		}
	}
	error = ufs_backing_block(inode->i_mount, fragment);
	if (error != 0)
		return error;
	*result = fragment;
	return 0;
}

/* Shares geometry and snapshot admission for data and owned metadata. */
static int
ufs_backing_lock(struct file *file, struct ufs_mount_state **result)
{
	struct inode *inode;
	struct mount *mountp;
	struct ufs_mount_state *ms;
	const struct ufs_super *super;
	int unsupported;

	if (file == NULL || file->f_inode == NULL)
		return EINVAL;
	inode = file->f_inode;
	mountp = inode->i_mount;
	if (inode->i_type != INODE_REG || mountp == NULL ||
	    mountp->m_type == NULL || mountp->m_type->file_extents != ufs_file_extents ||
	    mountp->m_disk == NULL)
		return EOPNOTSUPP;
	ms = state(mountp);
	if (ms == NULL)
		return EIO;
	super = &ms->super;
	unsupported = disk_sector_supported(mountp->m_disk);
	if (unsupported != 0)
		return EOPNOTSUPP;
	if (super->cgoffset != 0)
		return EOPNOTSUPP;
	if (super->bsize == 0 || super->bsize % UFS_SECTOR_SIZE != 0 ||
	    super->fsize < UFS_SECTOR_SIZE || super->fsize % UFS_SECTOR_SIZE != 0 ||
	    super->frag == 0 || super->fpg == 0 || super->nindir == 0)
		return EIO;
	if (inode->i_size <= 0 || (uint64_t)inode->i_size % UFS_SECTOR_SIZE != 0)
		return EINVAL;

	/* A prepared claim keeps the layout fixed between count/fill traversals. */
	mutex_lock(&ms->lock);
	if (ms->snapshot.active) {
		mutex_unlock(&ms->lock);
		return EBUSY;
	}
	*result = ms;
	return 0;
}

/* Exports ordered sector runs without allocating or changing the backing file. */
static int
ufs_file_extents(struct file *file, file_extent_cb callback, void *context)
{
	struct inode *inode;
	struct mount *mountp;
	struct ufs_mount_state *ms;
	const struct ufs_super *super;
	uint64_t remaining;
	uint64_t logical;
	uint64_t fragment;
	uint64_t physical;
	uint64_t run_logical;
	uint64_t run_physical;
	uint32_t count;
	uint32_t run_count;
	uint64_t disk_sectors;
	int error;

	if (callback == NULL)
		return EINVAL;
	error = ufs_backing_lock(file, &ms);
	if (error != 0)
		return error;
	inode = file->f_inode;
	mountp = inode->i_mount;
	super = &ms->super;
	disk_sectors = ufs_disk_sectors(mountp->m_disk);
	remaining = (uint64_t)inode->i_size / UFS_SECTOR_SIZE;
	logical = 0;
	run_logical = 0;
	run_physical = 0;
	run_count = 0;
	error = 0;
	while (remaining != 0) {
		error = ufs_backing_map(inode, logical /
		    (super->bsize / UFS_SECTOR_SIZE), &fragment);
		if (error != 0)
			break;
		if (fragment > UINT64_MAX / (super->fsize / UFS_SECTOR_SIZE)) {
			error = EOVERFLOW;
			break;
		}
		physical = fragment * (super->fsize / UFS_SECTOR_SIZE);
		count = super->bsize / UFS_SECTOR_SIZE;
		if (remaining < count)
			count = (uint32_t)remaining;
		if (physical >= disk_sectors ||
		    count > disk_sectors - physical) {
			error = EIO;
			break;
		}
		if (run_count != 0 && (physical != run_physical + run_count ||
		    count > UINT32_MAX - run_count)) {
			error = callback(run_logical, run_physical, run_count, context);
			if (error != 0)
				break;
			run_count = 0;
		}
		if (run_count == 0) {
			run_logical = logical;
			run_physical = physical;
		}
		run_count += count;
		logical += count;
		remaining -= count;
	}
	if (error == 0 && run_count != 0)
		error = callback(run_logical, run_physical, run_count, context);
	mutex_unlock(&ms->lock);
	return error;
}

/* Reports owned indirect blocks, limited by EOF and at most three levels. */
static int
ufs_metadata_tree(struct mount *mountp, uint64_t fragment, unsigned depth,
	uint64_t blocks, file_metadata_extent_cb callback, void *context)
{
	const struct ufs_super *super;
	uint64_t physical;
	uint64_t span;
	uint64_t child;
	uint64_t used;
	uint32_t index;
	uint32_t sectors;
	uint64_t disk_sectors;
	unsigned level;
	int error;

	if (depth == 0 || depth > UFS_NIADDR || blocks == 0)
		return EINVAL;
	super = &state(mountp)->super;
	error = ufs_backing_block(mountp, fragment);
	if (error != 0)
		return error;
	if (fragment > UINT64_MAX / (super->fsize / UFS_SECTOR_SIZE))
		return EOVERFLOW;
	physical = fragment * (super->fsize / UFS_SECTOR_SIZE);
	sectors = super->bsize / UFS_SECTOR_SIZE;
	disk_sectors = ufs_disk_sectors(mountp->m_disk);
	if (physical >= disk_sectors ||
	    sectors > disk_sectors - physical)
		return EIO;
	error = callback(physical, sectors, context);
	if (error != 0 || depth == 1U)
		return error;

	span = 1;
	for (level = 1; level < depth; level++) {
		if (span > UINT64_MAX / super->nindir)
			return EOVERFLOW;
		span *= super->nindir;
	}
	index = 0;
	while (blocks != 0) {
		if (index >= super->nindir)
			return EIO;
		used = blocks < span ? blocks : span;
		error = indirect_entry(mountp, fragment, index, &child);
		if (error != 0)
			return error;
		error = ufs_metadata_tree(mountp, child, depth - 1U, used, callback, context);
		if (error != 0)
			return error;
		blocks -= used;
		index++;
	}
	return 0;
}

/* These ranges protect the layout and are not logical file contents. */
static int
ufs_file_metadata_extents(struct file *file, file_metadata_extent_cb callback,
	void *context)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	const struct ufs_super *super;
	uint64_t blocks;
	uint64_t span;
	uint64_t used;
	unsigned level;
	int error;

	if (callback == NULL)
		return EINVAL;
	error = ufs_backing_lock(file, &ms);
	if (error != 0)
		return error;
	super = &ms->super;
	ui = info(file->f_inode);
	blocks = ((uint64_t)file->f_inode->i_size - 1U) / super->bsize + 1U;
	blocks = blocks > UFS_NDADDR ? blocks - UFS_NDADDR : 0;
	span = super->nindir;
	for (level = 0; level < UFS_NIADDR && blocks != 0; level++) {
		used = blocks < span ? blocks : span;
		error = ufs_metadata_tree(file->f_inode->i_mount, ui->indirect[level],
		    level + 1U, used, callback, context);
		if (error != 0)
			break;
		blocks -= used;
		if (blocks != 0) {
			if (span > UINT64_MAX / super->nindir) {
				error = EOVERFLOW;
				break;
			}
			span *= super->nindir;
		}
	}
	if (error == 0 && blocks != 0)
		error = EFBIG;
	mutex_unlock(&ms->lock);
	return error;
}

/* Turns a position in a file into the fragment holding it. */
static int
bmap(
	struct inode *inode,
	uint64_t logical,
	uint64_t *result)
{
	int error;

	/* Maps without a memo. */
	error = bmap_memo(inode, logical, result, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a file's logical block to a fragment, as bmap() does, reading the
 * indirect sectors through memo when there is one.
 */
static int
bmap_memo(
	struct inode *inode,
	uint64_t logical,
	uint64_t *result,
	struct ufs_bmap_memo *memo)
{
	uint64_t divisor;
	uint32_t index;
	unsigned n;
	struct ufs_inode_info *ui = info(inode);
	const struct ufs_super *s = &state(inode->i_mount)->super;
	uint64_t span = s->nindir;
	uint64_t fragment;
	unsigned level;
	unsigned depth;
	int error;

	/* The first blocks of a file are named by the inode itself. */
	if (logical < UFS_NDADDR) {
		*result = ui->direct[logical];
		/* Succeeded. */
		return 0;
	}

	logical -= UFS_NDADDR;
	/* Finds which indirect level covers the block. */
	for (level = 0; level < UFS_NIADDR; level++) {
		/* The level whose span reaches this block is the one to use. */
		if (logical < span)
			break;

		logical -= span;

		/*
		 * A span this wide cannot be represented, let alone addressed.
		 */
		if (span > UINT64_MAX / s->nindir)
			return EOVERFLOW;

		span *= s->nindir;
	}

	/* A block past the last indirect level is past the largest file. */
	if (level == UFS_NIADDR)
		return EFBIG;

	fragment = ui->indirect[level];

	/* Walks down one indirect level per pass, to the fragment itself. */
	for (depth = level + 1U; depth != 0; depth--) {
		divisor = 1;

		/*
		 * The entry to follow is the logical block divided by the span.
		 */
		for (n = 1; n < depth; n++)
			divisor *= s->nindir;
		index = (uint32_t)(logical / divisor);
		logical %= divisor;

		/* Reads the entry this level names. */
		error = indirect_entry_memo(inode->i_mount,
					    fragment,
					    index,
					    &fragment,
					    memo,
					    level + 1U - depth);
		if (error != 0 || fragment == 0)
			break;
	}

	*result = fragment;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Resolves a logical block to a fragment, allocating whatever is missing.
 *
 * The first UFS_NDADDR blocks are named directly by the inode; the rest are
 * reached through one, two or three levels of indirect blocks.  Every
 * allocation on the way is made reachable before it is used, so a failure
 * leaves no block that is allocated but unnamed.
 */
static int
bmap_ensure(
	struct inode *inode,
	uint64_t logical,
	uint64_t *result)
{
	uint64_t allocated_value;
	int rollback_value;
	int rollback_error;
	uint8_t *block;
	uint64_t divisor;
	uint32_t index;
	uint64_t next;
	unsigned n;
	struct ufs_inode_info *ui = info(inode);
	const struct ufs_super *s = &state(inode->i_mount)->super;
	uint64_t span = s->nindir;
	uint64_t *root;
	uint64_t fragment;
	unsigned level;
	unsigned depth;
	int inside;
	int error;

	/* The first blocks of a file are named by the inode itself. */
	if (logical < UFS_NDADDR) {
		if (ui->direct[logical] == 0) {
			/*
			 * Takes a block for the hole this read or write found.
			 */
			error = allocate_block(inode->i_mount,
					       inode->i_uid,
					       inode->i_gid,
					       &allocated_value);
			if (error != 0)
				return error;	/* Failed. */

			ui->direct[logical] = allocated_value;
			ui->blocks += s->bsize / UFS_SECTOR_SIZE;

			/*
			 * Make the allocation reachable before user data I/O.
			 */
			error = persist_inode(inode);
			if (error != 0) {
				ui->direct[logical] = 0;
				ui->blocks -= s->bsize / UFS_SECTOR_SIZE;

				/*
				 * Failure can follow a committed write
				 * (including replay).  Confirm pointer removal
				 * before recycling the allocation.
				 */
				rollback_value = persist_inode(inode);
				if (rollback_value == 0) {
					rollback_value = order_barrier(
						inode->i_mount);
				}

				/* Only an unreachable block may be freed. */
				if (rollback_value == 0) {
					rollback_value = free_block(
						inode->i_mount, allocated_value,
						inode->i_uid, inode->i_gid);
				}

				/*
				 * The block may still be reachable and is now
				 * unaccounted, so nothing more may be written.
				 */
				if (rollback_value != 0)
					stop_writing(inode->i_mount, "a block allocation could not be rolled back", rollback_value);

				return error;	/* Failed. */
			}
		}

		/* Succeeded: the direct pointer names the fragment. */
		*result = ui->direct[logical];

		return 0;
	}

	/*
	 * Finds which indirect level covers the block.  Each level spans
	 * nindir times as much as the one below it.
	 */
	logical -= UFS_NDADDR;
	for (level = 0; level < UFS_NIADDR; level++) {
		if (logical < span)
			break;

		logical -= span;

		/*
		 * A span this wide cannot be represented, let alone addressed.
		 */
		if (span > UINT64_MAX / s->nindir)
			return EOVERFLOW;	/* Failed. */

		span *= s->nindir;
	}

	/* A block past the last indirect level is past the largest file. */
	if (level == UFS_NIADDR)
		return EFBIG;	/* Failed. */

	/* The root of that level may itself still have to be allocated. */
	root = &ui->indirect[level];
	if (*root == 0) {
		error = allocate_indirect_block(inode, &allocated_value);
		if (error != 0)
			return error;	/* Failed. */

		*root = allocated_value;
		ui->blocks += s->bsize / UFS_SECTOR_SIZE;

		/* Make the allocation reachable before user data I/O. */
		error = persist_inode(inode);
		if (error != 0) {
			*root = 0;
			ui->blocks -= s->bsize / UFS_SECTOR_SIZE;

			/*
			 * Failure can follow a committed write (including
			 * replay).  Confirm pointer removal before recycling
			 * the allocation.
			 */
			rollback_value = persist_inode(inode);
			if (rollback_value == 0)
				rollback_value =
					order_barrier(inode->i_mount);

			/* Only an unreachable block may be freed. */
			if (rollback_value == 0) {
				rollback_value = free_block(inode->i_mount,
							    allocated_value,
							    inode->i_uid,
							    inode->i_gid);
			}

			/*
			 * The block may still be reachable and is now
			 * unaccounted, so nothing more may be written.
			 */
			if (rollback_value != 0)
				stop_writing(inode->i_mount, "a block allocation could not be rolled back", rollback_value);

			return error;	/* Failed. */
		}
	}

	/* Walks down one indirect level per pass, to the fragment itself. */
	fragment = *root;
	for (depth = level + 1U; depth != 0; depth--) {
		/*
		 * The entry to follow is the logical block divided by the span.
		 */
		divisor = 1;
		for (n = 1; n < depth; n++)
			divisor *= s->nindir;
		index = (uint32_t)(logical / divisor);
		logical %= divisor;

		/* Reads the indirect block this level is named by. */
		block = kern_malloc(s->bsize);
		if (block == NULL)
			return ENOMEM;	/* Failed. */

		error = read_block(inode->i_mount, fragment, block);
		if (error != 0) {
			kern_free(block);

			return error;	/* Failed. */
		}

		/* An empty entry is a hole this pass has to fill. */
		next = drv_ufs_get64(block, (size_t)index * 8U, s->swapped);
		if (next == 0) {
			allocated_value = 0;

			/*
			 * Takes a block: an indirect block for a level above
			 * the last, else the file's own block.
			 */
			if (depth > 1U) {
				error = allocate_indirect_block(inode, &next);
			} else {
				error = allocate_block(inode->i_mount, inode->i_uid,
						       inode->i_gid, &next);
			}

			/* Publishes the block into the entry. */
			if (error == 0) {
				allocated_value = next;

				drv_ufs_put64(block,
					      (size_t)index * 8U,
					      next,
					      s->swapped);

				error = write_block(inode->i_mount,
						    fragment,
						    block);
			}

			if (error != 0) {
				if (allocated_value != 0) {
					/*
					 * A short write may have published the
					 * pointer even though write_block()
					 * reported EIO.  Make it unreachable
					 * before returning its block.
					 */
					drv_ufs_put64(block, (size_t)index * 8U, 0, s->swapped);
					rollback_error = write_block(inode->i_mount, fragment, block);
					if (rollback_error == 0) {
						rollback_error = order_barrier(inode->i_mount);
					}

					/*
					 * Only an unreachable block may be
					 * freed.
					 */
					if (rollback_error == 0) {
						rollback_error = free_block(inode->i_mount,
									    allocated_value,
									    inode->i_uid,
									    inode->i_gid);
					}

					if (rollback_error != 0) {
						/*
						 * The block may remain reachable.  Never free
						 * uncertain storage or continue writable.
						 */
						ui->blocks += s->bsize / UFS_SECTOR_SIZE;
						stop_writing(inode->i_mount, "an indirect block allocation could not be rolled back", rollback_error);
					}
				}

				kern_free(block);

				/* Failed. */
				return error;
			}

			ui->blocks += s->bsize / UFS_SECTOR_SIZE;
		}

		/*
		 * Refuses an entry that names no block of the volume: the
		 * indirect block is corrupt, and its entry must not be taken
		 * for a block of the file (BUG-294).
		 */
		inside = fragment_in_volume(s, next);
		if (!inside) {
			kern_logf("ufs: %s: inode %llu: indirect block %llu names block %llu at entry %u, outside the volume\n",
				  inode->i_mount->m_disk->d_name,
				  (unsigned long long)inode->i_ino,
				  (unsigned long long)fragment,
				  (unsigned long long)next,
				  (unsigned)index);
			kern_free(block);

			/* Failed: the file's block map is corrupt. */
			return EIO;
		}

		/* Follows the entry down to the next level. */
		kern_free(block);
		fragment = next;
	}

	/* Succeeded: the walk ended on the fragment the caller asked for. */
	*result = fragment;
	return 0;
}

/*
 * Measures an existing physical run without changing allocation or publishing
 * size.
 */
static size_t
content_run_bytes(
	struct inode *inode,
	uint64_t logical,
	uint64_t first,
	size_t remaining,
	int writing,
	int *mapping_error,
	struct ufs_bmap_memo *memo)
{
	const struct ufs_super *super;
	struct ufs_mount_state *ms;
	uint64_t boundary;
	uint64_t maximum;
	uint64_t blocks;
	uint64_t next;
	int error;
	uint64_t journal_blocks;

	/*
	 * Bounds the mapping scan to one indirect leaf and the common byte
	 * limit.
	 */
	*mapping_error = 0;

	/* Takes the geometry the run is measured against. */
	super = &state(inode->i_mount)->super;
	if (first == 0 || first >= super->size ||
	    super->frag > super->size - first) {
		/* Succeeded. */
		return 0;
	}

	/* The run cannot be longer than what the caller still wants. */
	maximum = remaining / super->bsize;
	if (maximum > KERN_IO_BATCH_MAX / super->bsize)
		maximum = KERN_IO_BATCH_MAX / super->bsize;

	/* A run inside the direct blocks stops at the last of them. */
	if (logical < UFS_NDADDR)
		boundary = UFS_NDADDR - logical;
	else
		boundary =
			super->nindir - (logical - UFS_NDADDR) % super->nindir;

	/* Nor may it cross the indirect boundary it started inside. */
	if (maximum > boundary)
		maximum = boundary;

	/* Nor may it run past the end of the volume. */
	if (maximum > (super->size - first) / super->frag)
		maximum = (super->size - first) / super->frag;

	/* Takes the mount state the journal geometry is read from. */
	ms = state(inode->i_mount);

	/* Preserves the existing journal's per-transaction payload capacity. */
	if (writing && ms->journal_enabled) {
		journal_blocks = ms->journal.sector_count;
		journal_blocks = journal_blocks > 2U ? journal_blocks - 2U : 0;
		journal_blocks /= super->bsize / UFS_SECTOR_SIZE;

		/* Nor may it be wider than the journal could carry. */
		if (maximum > journal_blocks)
			maximum = journal_blocks;
	}

	/* Stops before a hole, discontinuity or a failed optional lookahead. */
	if (maximum == 0)
		return 0;

	/* Extends the run while the blocks stay contiguous. */
	for (blocks = 1; blocks < maximum; blocks++) {
		/* Resolves the block that would continue the run. */
		error = bmap_memo(inode, logical + blocks, &next, memo);
		if (error != 0) {
			*mapping_error = error;
			break;
		}

		/* A hole or a gap ends the run. */
		if (next == 0 || next != first + blocks * super->frag)
			break;
	}

	/* Returns only the validated contiguous byte span. */
	return (size_t)blocks * super->bsize;
}

/*
 * Follows the sequential reader of a file and asks the buffer cache to read
 * the file's next blocks ahead of it (buf_prefetch()).
 *
 * A read that starts within the window of the last read's end counts as
 * sequential (the VM's own read-ahead reads a little ahead of its reader,
 * through here too); from the second such read on, the blocks from where
 * the last request stopped up to a window past this read are mapped and
 * asked for in runs of contiguous blocks.  Holes, a mapping error or a
 * full queue end the request early; the next read goes on from there.
 */
static void
read_ahead(
	struct inode *inode,
	uint64_t offset,
	size_t length)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	const struct ufs_super *super;
	struct ufs_bmap_memo *memo;
	uint64_t window;
	uint64_t end;
	uint64_t from;
	uint64_t to;
	uint64_t logical;
	uint64_t last;
	uint64_t fragment;
	uint64_t run_first;
	uint64_t run_blocks;
	uint64_t reached;
	uint64_t run_max;
	uint32_t sectors;
	uint32_t queued;
	unsigned long irq;
	int full;

	/* Reads ahead only where the cache can hold a window. */
	window = buf_prefetch_window();
	if (window == 0 || length == 0)
		return;
	ms = state(inode->i_mount);
	ui = info(inode);
	super = &ms->super;
	end = offset + length;

	/* Follows the stream and plans the request. */
	from = 0;
	to = 0;
	irq = spin_lock_irqsave(&ms->readahead_lock);

	if (ui->ra_sequential != 0 && offset + window >= ui->ra_next && offset <= ui->ra_next + window) {
		if (ui->ra_sequential < 2U)
			ui->ra_sequential++;
	} else {
		ui->ra_sequential = 1;
		ui->ra_issued = end;
	}
	if (end > ui->ra_next || ui->ra_sequential == 1U)
		ui->ra_next = end;
	if (ui->ra_sequential >= 2U) {
		from = ui->ra_issued > end ? ui->ra_issued : end;
		to = end + window;
		if (to > (uint64_t)inode->i_size)
			to = (uint64_t)inode->i_size;
		/* Asks again only once a run's worth has been read since. */
		if (to <= from || to - from < window / 4U)
			to = from;
		else
			ui->ra_issued = to;
	}

	spin_unlock_irqrestore(&ms->readahead_lock, irq);

	if (to <= from)
		return;

	/* Maps the whole blocks of [from, to) and asks for each contiguous run. */
	memo = kern_malloc(sizeof(*memo));
	if (memo != NULL)
		kern_memset(memo->valid, 0, sizeof(memo->valid));
	logical = (from + super->bsize - 1U) / super->bsize;
	last = (to + super->bsize - 1U) / super->bsize;
	run_max = KERN_IO_BATCH_MAX * 4U / super->bsize;
	run_first = 0;
	run_blocks = 0;
	reached = logical;
	full = 0;
	for (; logical < last && !full; logical++) {
		if (bmap_memo(inode, logical, &fragment, memo) != 0 || fragment == 0)
			break;
		if (run_blocks != 0 && fragment == run_first + run_blocks * super->frag && run_blocks < run_max) {
			run_blocks++;
			continue;
		}
		if (run_blocks != 0) {
			sectors = (uint32_t)(run_blocks * super->bsize / UFS_SECTOR_SIZE);
			queued = ufs_buf_prefetch(inode->i_mount->m_disk, run_first << super->fsbtodb, sectors);
			if (queued != sectors)
				full = 1;
			else
				reached = logical;
		}
		run_first = fragment;
		run_blocks = 1;
	}
	if (!full && run_blocks != 0) {
		sectors = (uint32_t)(run_blocks * super->bsize / UFS_SECTOR_SIZE);
		queued = ufs_buf_prefetch(inode->i_mount->m_disk, run_first << super->fsbtodb, sectors);
		if (queued == sectors)
			reached = logical;
		else
			full = 1;
	}

	kern_free(memo);

	/* What was not asked for is asked for by a later read. */
	if (reached * super->bsize < to) {
		irq = spin_lock_irqsave(&ms->readahead_lock);
		if (ui->ra_issued == to)
			ui->ra_issued = reached * super->bsize;
		spin_unlock_irqrestore(&ms->readahead_lock, irq);
	}
}

/*
 * Reads a file's bytes, mapping its blocks with one memo of indirect
 * sectors for the whole read (without one when there is no memory for it).
 */
static ssize_t
pread_inode(
	struct inode *inode,
	void *buffer,
	size_t length,
	off_t offset)
{
	struct ufs_bmap_memo *memo;
	ssize_t result;

	/* Reads with a fresh memo. */
	memo = kern_malloc(sizeof(*memo));
	if (memo != NULL)
		kern_memset(memo->valid, 0, sizeof(memo->valid));
	result = pread_inode_memo(inode, buffer, length, offset, memo);
	kern_free(memo);

	/* Reports what the read did. */
	return result;
}

/*
 * Reads full mapped runs directly and retains block scratch for edges and
 * holes.
 */
static ssize_t
pread_inode_memo(
	struct inode *inode,
	void *buffer,
	size_t length,
	off_t offset,
	struct ufs_bmap_memo *memo)
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

	/* Rejects an offset before the start of the file. */
	if (offset < 0)
		return -EINVAL;

	/* Succeeded: a read at or past the end returns nothing. */
	if (offset >= inode->i_size || length == 0)
		return 0;

	/* Clamps the run to what is left of the file. */
	if ((uint64_t)length > (uint64_t)inode->i_size - (uint64_t)offset)
		length = (size_t)((uint64_t)inode->i_size - (uint64_t)offset);

	/* Asks for the blocks a sequential reader of a file reads next. */
	if (inode->i_type == INODE_REG)
		read_ahead(inode, (uint64_t)offset, length);

	/*
	 * Uses caller storage for complete blocks in each validated mapped run.
	 */
	scratch = NULL;
	done = 0;
	while (done < length) {
		position = (uint64_t)offset + done;
		logical = position / super->bsize;
		within = (size_t)(position % super->bsize);

		/* Resolves the block this offset falls in. */
		error = bmap_memo(inode, logical, &fragment, memo);
		if (error != 0) {
			kern_free(scratch);

			/* A short read reports what it did transfer. */
			if (done != 0)
				return (ssize_t)done;

			return -error;	/* Failed. */
		}

		mapping_error = 0;

		/* A whole block is read straight into the caller buffer. */
		amount = within == 0 ?
			content_run_bytes(inode, logical, fragment, length - done, 0, &mapping_error, memo) :
			0;
		if (amount != 0) {
			io_stats_record(IO_UFS_CONTENT_READ, amount);
			error = observed_disk_read(
				inode->i_mount->m_disk,
				(uint64_t)fragment << super->fsbtodb,
				(uint32_t)(amount / UFS_SECTOR_SIZE),
				(uint8_t *)buffer + done);
		} else if (fragment != 0 &&
		    pread_edge(inode, fragment, within,
		    length - done, (uint8_t *)buffer + done, &amount) == 0) {
			/* A few sectors of a block were read without staging. */
			error = 0;
		} else {
			/*
			 * Allocates edge scratch only when the request needs
			 * it.
			 */
			if (scratch == NULL) {
				/*
				 * Takes the staging a partial block is read
				 * through.
				 */
				scratch = kern_malloc(super->bsize);
				if (scratch == NULL) {
					return done != 0 ? (ssize_t)done
						: -ENOMEM;
				}
			}

			/* A partial block is read to the end of that block. */
			amount = super->bsize - within;
			if (amount > length - done)
				amount = length - done;

			/* Reads the block the run continues in. */
			error = read_content_block(inode->i_mount, fragment,
						   scratch);
			if (error == 0) {
				kern_memcpy((uint8_t *)buffer + done,
				       scratch + within, amount);
			}
		}
		if (error != 0) {
			kern_free(scratch);

			/* A short read reports what it did transfer. */
			if (done != 0)
				return (ssize_t)done;

			return -error;	/* Failed. */
		}

		done += amount;

		/* A mapping failure leaves the read short rather than wrong. */
		if (mapping_error != 0)
			break;
	}

	kern_free(scratch);

	/* Reports the successfully read prefix. */
	return (ssize_t)done;
}

/*
 * Reads a short piece of one block, only the sectors it covers, into the
 * caller's buffer.  Reports ENOSPC when the piece spans more sectors than
 * it stages on the stack, for the caller's whole-block path.
 */
static int
pread_edge(
	struct inode *inode,
	uint64_t fragment,
	size_t within,
	size_t wanted,
	uint8_t *buffer,
	size_t *amount)
{
	const struct ufs_super *super;
	uint8_t edge[4U * UFS_SECTOR_SIZE];
	size_t first;
	size_t sectors;
	size_t length;
	int error;

	/* Clips the piece to the end of its block and counts its sectors. */
	super = &state(inode->i_mount)->super;
	length = super->bsize - within;
	if (length > wanted)
		length = wanted;
	first = within / UFS_SECTOR_SIZE;
	sectors = (within + length + UFS_SECTOR_SIZE - 1U) / UFS_SECTOR_SIZE -
	    first;
	if (sectors * UFS_SECTOR_SIZE > sizeof(edge))
		return ENOSPC;

	/* Reads the covering sectors and copies the piece out. */
	io_stats_record(IO_UFS_CONTENT_READ, sectors * UFS_SECTOR_SIZE);
	error = read_metadata_sectors(inode->i_mount,
	    (fragment << super->fsbtodb) + first, (uint32_t)sectors, edge);
	if (error != 0)
		return error;
	kern_memcpy(buffer, edge + within % UFS_SECTOR_SIZE, length);
	*amount = length;

	/* Succeeded. */
	return 0;
}

/* Locates the shared on-disk block containing a dinode. */
static uint64_t
inode_fragment(
	struct inode *inode)
{
	uint64_t fragment;
	struct ufs_mount_state *ms;
	uint32_t number;
	uint32_t cg;
	uint32_t index;

	ms = state(inode->i_mount);
	number = (uint32_t)inode->i_ino;
	cg = number / ms->super.ipg;
	index = number % ms->super.ipg;

	/* The fragment the inode block starts at. */
	fragment = cgstart(&ms->super, cg) + ms->super.iblkno + (index / ms->super.inopb) * ms->super.frag;

	/* Reports that fragment. */
	return fragment;
}

/* Patches only one prepared dinode into a caller-owned shared block image. */
static void
encode_inode_locked(
	struct inode *inode,
	uint8_t *block)
{
	struct ufs_inode_info *ui;
	struct ufs_mount_state *ms;
	uint32_t index;
	uint8_t *raw;
	unsigned n;

	ui = info(inode);
	ms = state(inode->i_mount);
	index = (uint32_t)inode->i_ino % ms->super.ipg;

	/* Where inside the shared block this one inode is written. */
	raw = block + (index % ms->super.inopb) * UFS_DINODE_SIZE;

	/* The file type and the permission bits, as one stored mode word. */
	drv_ufs_put16(raw, UFS_DI_MODE, (uint16_t)inode->i_mode,
		      ms->super.swapped);

	/* How many names in the file system point at this inode. */
	drv_ufs_put16(raw, UFS_DI_NLINK, (uint16_t)inode->i_linkcount,
		      ms->super.swapped);

	/* The length of the file, in bytes. */
	drv_ufs_put64(raw, UFS_DI_SIZE, (uint64_t)inode->i_size,
		      ms->super.swapped);

	/* When the contents were last read, in seconds and nanoseconds. */
	drv_ufs_put64(raw, UFS_DI_ATIME, (uint64_t)inode->i_atime.tv_sec,
		      ms->super.swapped);
	drv_ufs_put32(raw, UFS_DI_ATIMENSEC, (uint32_t)inode->i_atime.tv_nsec,
		      ms->super.swapped);

	/* When they were last written. */
	drv_ufs_put64(raw, UFS_DI_MTIME, (uint64_t)inode->i_mtime.tv_sec,
		      ms->super.swapped);
	drv_ufs_put32(raw, UFS_DI_MTIMENSEC, (uint32_t)inode->i_mtime.tv_nsec,
		      ms->super.swapped);

	/* And when the inode itself last changed. */
	drv_ufs_put64(raw, UFS_DI_CTIME, (uint64_t)inode->i_ctime.tv_sec,
		      ms->super.swapped);
	drv_ufs_put32(raw, UFS_DI_CTIMENSEC, (uint32_t)inode->i_ctime.tv_nsec,
		      ms->super.swapped);

	/* How many bytes of extended attributes the inode carries. */
	drv_ufs_put32(raw, UFS_DI_EXTSIZE, ui->extattr_size, ms->super.swapped);

	/* And the blocks those attributes live in. */
	for (n = 0; n < UFS_NXADDR; n++) {
		drv_ufs_put64(raw,
			      UFS_DI_EXTB + n * 8U,
			      ui->extattr[n],
			      ms->super.swapped);
	}

	/* A device inode keeps its number where the first block would be. */
	if (inode->i_type == INODE_CHAR || inode->i_type == INODE_BLOCK) {
		kern_memset(raw + UFS_DI_DB, 0, 120U);

		drv_ufs_put64(raw,
			      UFS_DI_DB,
			      (uint64_t)inode->i_rdev,
			      ms->super.swapped);

		/* A device inode has no indirect blocks to write. */
		for (n = 0; n < UFS_NIADDR; n++) {
			drv_ufs_put64(raw,
				      UFS_DI_IB + n * 8U,
				      0,
				      ms->super.swapped);
		}
	} else if (inode->i_type == INODE_SYMLINK &&
		   (uint64_t)inode->i_size <= ms->super.maxsymlinklen &&
		   inode->i_size <= 120) {
		kern_memset(raw + UFS_DI_DB, 0, 120U);
		kern_memcpy(raw + UFS_DI_DB, ui->shortlink, (size_t)inode->i_size);
	} else {
		/* Writes the direct block pointers. */
		for (n = 0; n < UFS_NDADDR; n++) {
			drv_ufs_put64(raw,
				      UFS_DI_DB + n * 8U,
				      ui->direct[n],
				      ms->super.swapped);
		}

		/* Writes the indirect block pointers. */
		for (n = 0; n < UFS_NIADDR; n++) {
			drv_ufs_put64(raw,
				      UFS_DI_IB + n * 8U,
				      ui->indirect[n],
				      ms->super.swapped);
		}
	}

	/* How many 512-byte sectors the file and its attributes occupy. */
	drv_ufs_put64(raw, UFS_DI_BLOCKS, ui->blocks, ms->super.swapped);

	/* And the two identities the file is accounted against. */
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

	/* Reads the block the inode lives in. */
	error = read_block(inode->i_mount, *location, block);
	if (error != 0)
		return error;

	encode_inode_locked(inode, block);

	/* Succeeded. */
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

	/* Takes the staging the inode block is written from. */
	block = kern_malloc(state(inode->i_mount)->super.bsize);
	if (block == NULL)
		return ENOMEM;

	/* Fills it with the inode as it now stands. */
	error = prepare_inode_locked(inode, block, &fragment);
	if (error == 0)
		error = write_block(inode->i_mount, fragment, block);

	kern_free(block);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Serializes snapshot preservation, exact commit outcome and metadata
 * durability.
 */
static int
metadata_group_commit(
	struct mount *mountp,
	const struct ufs_journal_extent *extents,
	unsigned count,
	const struct io_context *context,
	struct ufs_transaction_outcome *outcome)
{
	struct ufs_mount_state *ms;
	struct io_context child;
	uint64_t sequence;
	unsigned n;
	int error;
	int deferred;

	/* Takes the mount state this call runs against. */
	ms = state(mountp);

	kern_memset(outcome, 0, sizeof(*outcome));

	/*
	 * Carries the logical owner through every snapshot and journal
	 * durability step.
	 */

	/* Opens an ordered child context, so the group is written in order. */
	error = io_context_child(&child, context, IO_CONTEXT_ORDERED);
	if (error != 0)
		return error;

	/*
	 * Policy-off drains under ms->lock after closing this admission query.
	 */
	deferred = context == NULL && writeback_mount_active(mountp);
	io_epoch_begin(&mountp->m_write_epoch);

	/*
	 * A snapshot has to keep the old contents of everything being written.
	 */
	if (ms->snapshot_available) {
		mutex_lock(&ms->snapshot_lock);
		ms->snapshot_io.context = &child;

		/*
		 * Preserves all original homes before a grouped checkpoint may
		 * overwrite any.
		 */
		for (n = 0; n < count; n++) {
			/*
			 * Preserves the sectors the group is about to
			 * overwrite.
			 */
			error = drv_ufs_snapshot_preserve(&ms->snapshot,
							  extents[n].target,
							  extents[n].sectors);
			if (error != 0)
				break;
		}

		ms->snapshot_io.context = NULL;
	}

	/* Writes the group straight out when no journal carries it. */
	if (error == 0) {
		mutex_lock(&ms->journal_lock);
		error = journal_checkpoint_locked(mountp);

		/*
		 * Takes the sequence the journal will publish the group under.
		 */
		sequence = 0;
		if (error == 0) {
			journal_wait_readers(ms);
			ms->journal_io.context = &child;
			sequence = ms->journal.next_sequence;

			/*
			 * A deferred group is published now and written by the
			 * checkpoint.
			 */
			if (deferred) {
				/*
				 * Publishes the whole group into the journal.
				 */
				error = drv_ufs_journal_publishv(&ms->journal, extents, count);
				if (error != 0 && ms->journal.pending_sequence != 0) {
					(void)drv_ufs_journal_drain(&ms->journal);
				}
			} else {
				error = drv_ufs_journal_commitv(&ms->journal, extents, count);
			}
		}

		/*
		 * Sequence identity is unique until init; inspect before
		 * releasing admission.
		 */
		outcome->committed = sequence != 0 && ms->journal.committed_sequence == sequence;
		outcome->uncertain = ms->journal.poisoned ||
			(ms->journal.pending_sequence != 0 && !(error == 0 && outcome->committed && ms->journal.pending_ready));

		/* An uncertain outcome leaves the volume unwritable. */
		if (outcome->uncertain)
			stop_writing(mountp, "a metadata group commit has an uncertain outcome", error);

		ms->journal_io.context = NULL;
		mutex_unlock(&ms->journal_lock);
	}

	/* Releases the snapshot hold the preservation took. */
	if (ms->snapshot_available)
		mutex_unlock(&ms->snapshot_lock);

	io_epoch_end(&mountp->m_write_epoch);

	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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
	kern_memset(images, 0, sizeof(*images));
	ms = state(mountp);
	images->mountp = mountp;
	images->memory = memory;
	sectors = bytes / UFS_SECTOR_SIZE;

	/*
	 * Restricts admission to the active journal's payload and descriptor
	 * limits.
	 */
	if (sectors > UFS_JOURNAL_GROUP_SECTORS)
		sectors = UFS_JOURNAL_GROUP_SECTORS;

	/* A journal too small to hold a group carries none. */
	if (ms->journal.sector_count <= 2U)
		sectors = 0;
	else if (sectors > ms->journal.sector_count - 2U)
		sectors = ms->journal.sector_count - 2U;
	images->capacity = sectors * UFS_SECTOR_SIZE / ms->super.bsize;

	/* Bounds the images to what one journal group may name. */
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
	 * owners.
	 */
	*result = NULL;
	ms = state(images->mountp);
	sectors = ms->super.bsize / UFS_SECTOR_SIZE;

	/* Refuses a fragment this volume geometry could not address. */
	if (fragment == 0 || ms->super.fsbtodb >= 64U ||
	    fragment > (UINT64_MAX >> ms->super.fsbtodb)) {
		/* Failed. */
		return EIO;
	}

	/* The sector that fragment starts at. */
	sector = fragment << ms->super.fsbtodb;
	if (sector > UINT64_MAX - sectors)
		return EIO;

	/*
	 * Reuses identical blocks and rejects aliasing through a partial
	 * overlap.
	 */
	for (n = 0; n < images->count; n++) {
		/* Looks for an extent this block already belongs to. */
		extent = &images->extents[n];
		if (extent->target == sector) {
			*result = images->memory + n * ms->super.bsize;
			/* Succeeded. */
			return 0;
		}

		/* The block falls inside an extent the group already holds. */
		if (sector < extent->target + extent->sectors &&
		    extent->target < sector + sectors) {
			/* Failed. */
			return EIO;
		}
	}

	/*
	 * Reads a new image only when its full block fits the reserved
	 * transaction.
	 */
	if (images->count >= images->capacity)
		return ENOSPC;

	block = images->memory + images->count * ms->super.bsize;

	/* Reads the block into the extent the group just took. */
	error = read_block(images->mountp, fragment, block);
	if (error != 0)
		return error;

	/*
	 * Admits only successfully loaded bytes and keeps the extent order
	 * stable.
	 */
	extent = &images->extents[images->count];
	extent->target = sector;
	extent->sectors = sectors;
	extent->payload = block;
	images->count++;
	*result = block;

	/*
	 * Returns the image for in-place private preparation under the mount
	 * lock.
	 */
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

	/*
	 * Preserves any sibling edits already present in this private block.
	 * Stages the block the inode lives in.
	 */
	error = metadata_image_get(images, inode_fragment(prepared), &block);
	if (error != 0)
		return error;

	encode_inode_locked(prepared, block);

	/*
	 * Leaves live inode publication to the operation's committed outcome.
	 */
	return 0;
}

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
	 * held.
	 */
	mountp = inode->i_mount;
	ms = state(mountp);

	/* Stages the inode as it will stand once the run is published. */
	error = prepare_inode_locked(&run->image.inode, run->dinode, &fragment);
	if (error != 0)
		return error;

	/* Stages the superblock summaries the run changes. */
	error = prepare_super_summaries(mountp, run->summaries);
	if (error != 0)
		return error;

	/* The injected fault stands in for a cylinder-group write failure. */
	if (KERN_TEST_FAULT(KERN_TEST_FAULT_UFS_CG_WRITE, UINT32_MAX,
			    UINT32_MAX, &fault)) {
		/* Reports the injected error, or a device error by default. */
		if (fault.error != 0)
			return fault.error;	/* Failed. */

		/* Failed. */
		return EIO;
	}

	/*
	 * Describes disjoint shared blocks; core validation precedes every redo
	 * write.
	 */
	extents[0].target = (cgstart(&ms->super, ms->active_cg) + ms->super.cblkno) << ms->super.fsbtodb;
	extents[0].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[0].payload = ms->cg;
	extents[1].target = UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;
	extents[1].sectors = UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;
	extents[1].payload = run->summaries;
	extents[2].target = fragment << ms->super.fsbtodb;
	extents[2].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[2].payload = run->dinode;
	count = 3;

	/* Stages the tree nodes the run had to create. */
	if (run->tree_count != 0) {
		/*
		 * Includes every newly initialized node and its changed
		 * existing parent.
		 */
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

	error = metadata_group_commit(mountp, extents, count, context, &outcome);
	run->committed = outcome.committed;
	run->uncertain = outcome.uncertain;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Prepares an absent suffix without installing empty nodes or live references.
 */
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

	/* A run with no tree staging cannot record a path. */
	if (run->tree_memory == NULL)
		return ENOMEM;

	kern_memset(run->tree_memory, 0, UFS_NIADDR * super->bsize);

	/* Starts out with no level of the path recorded. */
	for (n = 0; n < UFS_NIADDR; n++)
		run->tree_images[n] = run->tree_memory + n * super->bsize;

	/* A path already recorded is not recorded twice. */
	if (run->tree_count != 0)
		kern_memcpy(run->tree_images[0], run->old_leaf, super->bsize);

	run->missing_nodes = depth;

	/*
	 * Records the pointer slot in each missing level, ending at a private
	 * leaf.
	 */
	while (depth != 0) {
		divisor = 1;

		/* The span this level covers. */
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
	 * reservation.
	 */
	return 0;
}

/*
 * Locates a leaf or prepares its missing path without publishing empty nodes.
 */
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
	uint64_t entry;
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

	/* A run inside the direct blocks needs no indirect walk. */
	if (logical < UFS_NDADDR) {
		run->index = (unsigned)logical;
		/* Counts how many of the direct blocks the run can cover. */
		for (n = 0; n < *count; n++) {
			/*
			 * The run stops at the last direct block, or at a
			 * filled one.
			 */
			if (logical + n >= UFS_NDADDR ||
			    ui->direct[logical + n] != 0)
				break;
		}

		*count = n;

		/* Succeeded. */
		return 0;
	}

	/* Select the indirect root and its checked logical range. */
	logical -= UFS_NDADDR;
	span = super->nindir;

	/* Finds which indirect level covers the block the run starts at. */
	for (level = 0; level < UFS_NIADDR; level++) {
		/* The level whose span reaches this block is the one to use. */
		if (logical < span)
			break;

		logical -= span;

		/*
		 * A span this wide cannot be represented, let alone addressed.
		 */
		if (span > UINT64_MAX / super->nindir)
			return EOVERFLOW;

		span *= super->nindir;
	}

	/*
	 * Traverses existing nodes, preparing an absent suffix only for grouped
	 * owners.
	 */
	if (level == UFS_NIADDR)
		return EFBIG;

	fragment = ui->indirect[level];

	/* Walks down one indirect level per pass, to the leaf itself. */
	for (depth = level + 1U; depth != 0; depth--) {
		/* A level that is not there yet has to be allocated. */
		if (fragment == 0) {
			/* Only a grouped run may allocate the path it walks. */
			if (!run->grouped) {
				*count = 0;

				/* Succeeded. */
				return 0;
			}

			run->root_level = level;
			run->missing_root = parent == 0;

			/*
			 * Remembers the block that will name the level being
			 * created.
			 */
			if (parent != 0) {
				run->tree_count = 1;
				run->tree_targets[0] = parent;
				run->tree_indices[0] = parent_index;
			}

			/* Records every level the run still has to allocate. */
			error = allocation_missing_path(run, super, logical, depth);
			if (error != 0)
				return error;

			/* The run stops at the end of the leaf it reached. */
			if (*count > super->nindir - run->index)
				*count = super->nindir - run->index;

			/* Succeeded. */
			return 0;
		}

		/* Reads the leaf so the entries around the run survive. */
		error = read_block(inode->i_mount, fragment, run->old_leaf);
		if (error != 0)
			return error;

		/*
		 * Consume one level while retaining the last leaf's original
		 * bytes.
		 */
		divisor = 1;

		/*
		 * The entry to follow is the logical block divided by the span.
		 */
		for (n = 1; n < depth; n++)
			divisor *= super->nindir;

		index = (unsigned)(logical / divisor);
		logical %= divisor;

		/* The last level is the leaf the run writes into. */
		if (depth == 1) {
			run->leaf = fragment;
			run->index = index;
			break;
		}

		parent = fragment;
		parent_index = index;
		fragment = drv_ufs_get64(run->old_leaf, index * 8U, super->swapped);
	}

	/* Stop at an occupied pointer or the end of this leaf. */
	for (n = 0; n < *count; n++) {
		/* The run stops at the end of the leaf. */
		if (run->index + n >= super->nindir)
			break;

		/* Reads the entry the run would have to extend over. */
		entry = drv_ufs_get64(run->old_leaf,
				      (run->index + n) * 8U,
				      super->swapped);

		/* A filled entry ends the run, because it is not a hole. */
		if (entry != 0)
			break;
	}

	*count = n;

	kern_memcpy(run->new_leaf, run->old_leaf, super->bsize);

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
	int wrapped;
	int error;

	/*
	 * Search groups without exposing pending changes to another allocator.
	 */

	ms = state(inode->i_mount);

	/* Tries every cylinder group, starting at the preferred one. */
	for (attempt = 0; attempt < ms->super.ncg; attempt++) {
		cg = (ms->rotor_cg + attempt) % ms->super.ncg;

		/* Reads the cylinder group being searched. */
		error = load_cg_locked(inode->i_mount, cg);
		if (error != 0)
			return error;

		map = ms->cg + ms->cg_freeoff;
		ndblk = cg_ndblk(&ms->super, cg);

		/*
		 * Finds the first complete free block and its bounded
		 * contiguous run: from where the last run in this group ended,
		 * and when nothing is free past that, from the group's start.
		 */
		fragment = cg_search_start(ms, cg, ms->super.dblkno, ndblk);
		wrapped = 0;
		if (fragment == ms->super.dblkno)
			wrapped = 1;
		for (;;) {
			/* Finds the next free block and the free blocks after it. */
			count = cg_free_blocks(&ms->super, map, fragment, ndblk, run->reserved, &fragment);

			/* Nothing is free past the hint: the start of the group is searched once. */
			if (count == 0 && !wrapped) {
				wrapped = 1;
				fragment = ms->super.dblkno;
				continue;
			}

			/* The group has no free block left. */
			if (count == 0)
				break;

			/*
			 * A group that cannot cover the tree is no use to this
			 * run.
			 */
			if (count <= run->missing_nodes) {
				fragment += ms->super.frag;
				continue;
			}

			/*
			 * Save the entire current group before changing its
			 * ownership.
			 */

			/*
			 * Reads the free-block count this reservation lowers.
			 */
			free_blocks = drv_ufs_get32(ms->cg, UFS_CG_NBFREE,
						    ms->super.swapped);
			if (free_blocks < count ||
			    ms->super.cstotal_nbfree < count) {
				/* Failed. */
				return EIO;
			}

			kern_memcpy(run->old_cg, ms->cg, ms->super.bsize);
			run->old_total = ms->super.cstotal_nbfree;
			run->count = count;
			run->first = cgstart(&ms->super, cg) + fragment;

			/*
			 * Remove the run from the single group and its global
			 * summary.
			 */
			for (n = 0; n < count * ms->super.frag; n++)
				bit_clear(map, fragment + n);

			drv_ufs_put32(ms->cg, UFS_CG_NBFREE,
				      free_blocks - count, ms->super.swapped);

			ms->super.cstotal_nbfree -= count;

			/* The group's next search starts past this run. */
			ms->alloc_hint_cg = cg;
			ms->alloc_hint_fragment = fragment + count * ms->super.frag;

			/* Succeeded. */
			return 0;
		}
	}

	/* Report exhaustion without changing allocation ownership. */
	return ENOSPC;
}

/*
 * Confirm pointer removal before returning any uncertain allocation to the CG.
 */
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

	/* Starts out with nothing to report. */
	error = 0;
	if (run->published) {
		/* Puts back the leaf entries the run had filled. */
		if (run->leaf != 0) {
			error = write_block(inode->i_mount, run->leaf,
					    run->old_leaf);
		}
		if (error == 0)
			error = persist_inode_locked(inode);
		if (error == 0)
			error = order_barrier(inode->i_mount);
	}

	/*
	 * Keep allocations charged and stop writes if reachability is
	 * uncertain.
	 */
	if (error != 0) {
		stop_writing(inode->i_mount, "an allocation run could not be undone", error);

		/* Failed. */
		return error;
	}

	/*
	 * Restore allocation summaries only after no durable pointer can refer
	 * here.
	 */
	kern_memcpy(ms->cg, run->old_cg, ms->super.bsize);
	ms->super.cstotal_nbfree = run->old_total;

	/* Writes the cylinder group back with the blocks freed. */
	error = write_cg(inode->i_mount);
	if (error == 0)
		error = order_barrier(inode->i_mount);
	if (error != 0)
		stop_writing(inode->i_mount, "the blocks of an aborted allocation could not be freed", error);

	/*
	 * Let the caller retain charges when rollback durability remains
	 * uncertain.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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
	 * charges.
	 */
	for (n = 0; n < run->reserved; n++) {
		/* A retained block stays allocated and is not released. */
		if (retained && n < run->count)
			quota_commit(&run->charges[n]);
		else
			quota_rollback(&run->charges[n]);
	}

	kern_free(run->tree_memory);
	kern_free(run->memory);
	kern_free(run);
}

/*
 * Initialize full new blocks before publishing a bounded private pointer image.
 */
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
	size_t staging;
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

	/* Bounds one pass to what a single allocation run may cover. */
	bytes = length < UFS_ALLOCATION_BYTES ? length : UFS_ALLOCATION_BYTES;
	if (ms->journal_enabled &&
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)
		bytes = (ms->journal.sector_count - 2U) * UFS_SECTOR_SIZE;

	/* Counts the whole blocks that many bytes reach. */
	count = (unsigned)(bytes / ms->super.bsize);
	if (count > UFS_ALLOCATION_BLOCKS)
		count = UFS_ALLOCATION_BLOCKS;

	/* A run shorter than one block is written by the ordinary path. */
	if (count == 0)
		return 0;

	/*
	 * Declines an oversized group before changing either quota or
	 * allocation state.
	 */
	if (ms->journal_enabled) {
		/*
		 * Reserves room for the leaf, its tree and the superblock
		 * image.
		 */
		bytes = 3U * ms->super.bsize + UFS_SBLOCK_SIZE;
		if (bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
		    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
			/* Succeeded. */
			return 0;
		}
	}

	/*
	 * Decline optimization before reservation when bounded memory is
	 * unavailable.
	 */

	/* Describes the run this pass will allocate and write. */
	run = kern_calloc(1, sizeof(*run));
	if (run == NULL)
		return 0;

	/* A grouped run also stages the tree and a superblock image. */
	run->grouped = ms->journal_enabled;
	if (run->grouped)
		staging = 4U * ms->super.bsize + UFS_SBLOCK_SIZE;
	else
		staging = 3U * ms->super.bsize;

	/* Gives up before touching the volume when there is no staging. */
	run->memory = kern_malloc(staging);
	if (run->memory == NULL) {
		kern_free(run);

		/* Succeeded. */
		return 0;
	}

	run->old_cg = run->memory;
	run->old_leaf = run->memory + ms->super.bsize;
	run->new_leaf = run->old_leaf + ms->super.bsize;

	/* A grouped run holds the journal open until it commits. */
	if (run->grouped) {
		run->dinode = run->new_leaf + ms->super.bsize;
		run->summaries = run->dinode + ms->super.bsize;
	}

	/*
	 * Owns shared path images from their first read through publication or
	 * rollback.
	 */
	mutex_lock(&ms->lock);

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable) {
		mutex_unlock(&ms->lock);
		allocation_run_release(run, 0);

		/* Failed. */
		return -EROFS;
	}

	/* Finds the leaf the run starts at and how many blocks it spans. */
	error = allocation_run_leaf(inode, logical, &count, run);
	if (error != 0 || count == 0) {
		mutex_unlock(&ms->lock);
		allocation_run_release(run, 0);

		/* Failed: the caller expects a negative error here. */
		return -error;
	}

	/*
	 * Preflights every changed path block before reserving quota or
	 * allocation.
	 */
	if (run->tree_count != 0) {
		/*
		 * Widens the reservation to the tree the leaf turned out to
		 * need.
		 */
		bytes = (2U + run->tree_count) * ms->super.bsize + UFS_SBLOCK_SIZE;
		if (bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
		    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
			mutex_unlock(&ms->lock);
			allocation_run_release(run, 0);

			/* Succeeded. */
			return 0;
		}

		/* Refuses a run that would outgrow the allocation image. */
		if (count > UFS_ALLOCATION_BLOCKS - run->missing_nodes)
			count = UFS_ALLOCATION_BLOCKS - run->missing_nodes;

		count += run->missing_nodes;
	}

	/*
	 * Reserve quotas individually so a hard limit still permits a valid
	 * prefix.
	 */

	/* Charges every block of the run against the owner quota. */
	for (n = 0; n < count; n++) {
		/* Charges one block against the owner quota. */
		error = quota_reserve(&ms->quota,
				      inode->i_uid,
				      inode->i_gid, 1,
				      0,
				      quota_now(),
				      &run->charges[n]);
		if (error != 0)
			break;

		run->reserved++;
	}

	/* Undoes the quota charge when only tree nodes were reserved. */
	if (run->reserved <= run->missing_nodes) {
		mutex_unlock(&ms->lock);
		allocation_run_release(run, 0);

		/* Failed: the caller expects a negative error here. */
		return -error;
	}

	/*
	 * Reserves bitmap ownership under the same lock as the prepared
	 * reference path.
	 */

	/* Takes every block the run needs, as one reservation. */
	error = allocation_run_reserve(inode, run);
	if (error != 0) {
		mutex_unlock(&ms->lock);
		if (error == ENOSPC && run->missing_nodes != 0)
			error = 0;

		allocation_run_release(run, 0);

		/* Failed: the caller expects a negative error here. */
		return -error;
	}

	io_stats_record(IO_UFS_ALLOC_BEGIN, 0);
	bytes = (run->count - run->missing_nodes) * ms->super.bsize;

	/* Publishes each allocated block into the run image. */
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

	/* Writes the content of the run out to the volume. */
	if (error == 0) {
		io_stats_record(IO_UFS_CONTENT_WRITE, bytes);
		error = write_content_sectors_context(inode->i_mount,
					      run->first << ms->super.fsbtodb,
					      (uint32_t)(bytes / UFS_SECTOR_SIZE),
					      buffer,
					      context);
	}

	if (error == 0)
		error = order_barrier(inode->i_mount);

	/*
	 * Prepare the new inode and leaf without changing the published inode.
	 */

	ui = info(inode);
	kern_memcpy(&run->image, ui, sizeof(run->image));

	/* A run that grew the tree publishes its new nodes as well. */
	if (run->tree_count != 0) {
		next = run->count - run->missing_nodes;

		/*
		 * Assigns reserved metadata addresses only to the private path
		 * images.
		 */

		/* Publishes each new tree node. */
		for (n = 0; n < run->tree_count; n++) {
			/*
			 * A node the reservation did not fill stays out of the
			 * image.
			 */
			if (run->tree_targets[n] == 0) {
				run->tree_targets[n] =
					run->first + next++ * ms->super.frag;
			}
		}

		/* Links each node to the one below it. */
		for (n = 0; n + 1U < run->tree_count; n++) {
			drv_ufs_put64(
				run->tree_images[n], run->tree_indices[n] * 8U,
				run->tree_targets[n + 1U], ms->super.swapped);
		}

		/* A new root is published into the inode itself. */
		if (run->missing_root) {
			run->image.indirect[run->root_level] =
				run->tree_targets[0];
		}

		run->leaf = run->tree_targets[run->tree_count - 1U];
	}

	/* Publishes the leaf blocks the run allocated. */
	for (n = 0; n < run->count - run->missing_nodes; n++) {
		/*
		 * A run that reached an existing leaf writes into it directly.
		 */
		if (run->leaf != 0) {
			drv_ufs_put64(run->new_leaf, (run->index + n) * 8U,
				      run->first + n * ms->super.frag,
				      ms->super.swapped);
		} else {
			run->image.direct[run->index + n] =
				run->first + n * ms->super.frag;
		}
	}

	run->image.blocks += (uint64_t)run->count * ms->super.bsize / UFS_SECTOR_SIZE;

	/* Grows the recorded size to cover what the run just wrote. */
	if ((uint64_t)run->image.inode.i_size < logical * ms->super.bsize + bytes) {
		run->image.inode.i_size = (off_t)(logical * ms->super.bsize + bytes);
	}

	/*
	 * Publish initialized pointers, retaining the old image until the flush
	 * passes.
	 */
	if (error == 0 && run->grouped) {
		error = allocation_group_commit(inode, run, context);
	} else if (error == 0) {
		run->published = 1;

		/* Writes the caller data into the leaf the run reached. */
		if (run->leaf != 0)
			error = write_block(inode->i_mount, run->leaf, run->new_leaf);

		if (error == 0)
			error = persist_inode_locked(&run->image.inode);

		if (error == 0)
			error = order_barrier(inode->i_mount);
	}

	/*
	 * Publish in-memory state only after a durable private metadata image.
	 */
	retained = 1;

	/*
	 * A committed group is durable even when the write reported an error.
	 */
	if (error == 0 || run->committed) {
		kern_memcpy(ui->direct, run->image.direct, sizeof(ui->direct));
		kern_memcpy(ui->indirect, run->image.indirect, sizeof(ui->indirect));
		ui->blocks = run->image.blocks;
		inode->i_size = run->image.inode.i_size;
		ms->rotor_cg = ms->active_cg;

		/* Closes the journal group this run held open. */
		if (run->grouped)
			ms->cg_dirty = run->uncertain;

		io_stats_record(IO_UFS_ALLOC_COMMIT, 0);
	} else if (run->grouped) {
		/*
		 * Uncommitted private ownership needs no compensating disk
		 * transaction.
		 */
		if (!run->uncertain) {
			kern_memcpy(ms->cg, run->old_cg, ms->super.bsize);
			ms->super.cstotal_nbfree = run->old_total;
			ms->cg_dirty = 0;
			retained = 0;
		}

		io_stats_record(IO_UFS_ALLOC_ABORT, 0);
	} else {
		/* An ungrouped run undoes its reservation on the volume. */
		rollback = allocation_run_abort(inode, run);
		retained = rollback != 0;
		io_stats_record(IO_UFS_ALLOC_ABORT, 0);
	}

	mutex_unlock(&ms->lock);

	allocation_run_release(run, retained);

	/* Return only completely initialized and published content. */
	if (error != 0)
		return -error;

	/* Succeeded: reports how many bytes the run carried. */
	return (ssize_t)bytes;
}

/* Validates an attribute block and returns its allocation-group coordinates. */
static int
xattr_release_location(
	const struct ufs_super *super,
	uint64_t child,
	uint32_t *group,
	uint32_t *local)
{
	uint64_t start;
	uint32_t ndblk;
	uint32_t cg;
	uint32_t end;

	/*
	 * Finds one complete aligned data block within the filesystem geometry.
	 */
	cg_search_range(super, child, &cg, &end);
	for (; cg < end; cg++) {
		/* Where this cylinder group starts on the volume. */
		start = cgstart(super, cg);

		/* How many data fragments this group holds. */
		ndblk = cg_ndblk(super, cg);

		/* A block outside this group's data area belongs to another. */
		if (child < start || child - start < super->dblkno ||
		    child - start >= ndblk)
			continue;

		*local = (uint32_t)(child - start);

		/* Refuses a block that is not aligned to a whole block. */
		if (*local % super->frag != 0 ||
		    super->frag > ndblk - *local) {
			/* Failed. */
			return EIO;
		}
		*group = cg;
		/* Succeeded. */
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
	int already_free;
	int error;
	int quota_error;

	/*
	 * Validates the complete inode owner before changing private allocation
	 * maps.
	 */
	ms = state(inode->i_mount);
	ui = info(inode);
	keep = length != 0;
	released = count - keep;

	/* Refuses to release attributes on a volume that is not writable. */
	if (!ms->writable)
		return EROFS;

	/*
	 * A block count below the attribute blocks means the inode is corrupt.
	 */
	if (ui->blocks < (uint64_t)count * (ms->super.bsize / UFS_SECTOR_SIZE) ||
	    ms->super.cstotal_nbfree > UINT64_MAX - released) {
		/* Failed. */
		return EIO;
	}

	/*
	 * Verifies the retained payload block remains allocated before
	 * replacing it.
	 */
	if (keep != 0) {
		/*
		 * Locates the cylinder group the first attribute block lives
		 * in.
		 */
		error = xattr_release_location(&ms->super,
					       ui->extattr[0],
					       &cg,
					       &local);
		if (error != 0)
			return error;

		/*
		 * Reads that cylinder group so its free map can be inspected.
		 */
		error = load_cg_locked(inode->i_mount, cg);
		if (error != 0)
			return error;

		/* Refuses a block the free map already calls free. */
		for (n = 0; n < ms->super.frag; n++) {
			/* Asks the free map whether it already holds this. */
			already_free = bit_test(ms->cg + ms->cg_freeoff, local + n);

			/* A free block must not be released twice. */
			if (already_free)
				return EIO;
		}

		kern_memset(group->data, 0, ms->super.bsize);
		kern_memcpy(group->data, area, length);
	}

	/*
	 * Loads each affected CG once and removes only allocated, distinct
	 * blocks.
	 */
	/* Walks the attribute blocks this release will give back. */
	for (index = keep; index < count; index++) {
		child = ui->extattr[index];

		/* Refuses a block that appears twice in the inode. */
		for (n = 0; n < index; n++) {
			/* The same block named twice would be freed twice. */
			if (ui->extattr[n] == child)
				return EIO;
		}

		/* Locates the cylinder group this block lives in. */
		error = xattr_release_location(&ms->super, child, &cg, &local);
		if (error != 0)
			return error;

		/* Reuses a group this release has already loaded. */
		for (slot = 0; slot < group->group_count; slot++) {
			/* This group is one the release already holds. */
			if (group->groups[slot] == cg)
				break;
		}

		/* Loads a group the release has not seen yet. */
		if (slot == group->group_count) {
			/*
			 * Reads that cylinder group so its free map can be
			 * inspected.
			 */
			error = load_cg_locked(inode->i_mount, cg);
			if (error != 0)
				return error;
			kern_memcpy(group->cg[slot], ms->cg, ms->super.bsize);
			group->groups[slot] = cg;
			group->group_count++;
		}

		/*
		 * Reads offsets from this CG image because different CG layouts
		 * may differ.
		 */
		n = drv_ufs_get32(group->cg[slot], UFS_CG_FREEOFF,
				  ms->super.swapped);

		/* Refuses a block the free map already calls free. */
		for (cg = 0; cg < ms->super.frag; cg++) {
			/* Asks the free map whether it already holds this. */
			already_free = bit_test(group->cg[slot] + n, local + cg);

			/* A free block must not be released twice. */
			if (already_free)
				return EIO;

			bit_set(group->cg[slot] + n, local + cg);
		}

		/* Reads the free-block count this release will adjust. */
		free_blocks = drv_ufs_get32(group->cg[slot], UFS_CG_NBFREE, ms->super.swapped);
		if (free_blocks == UINT32_MAX)
			return EIO;

		drv_ufs_put32(group->cg[slot],
			      UFS_CG_NBFREE,
			      free_blocks + 1U,
			      ms->super.swapped);
	}

	/*
	 * Prepares the complete new serialized area and its remaining block
	 * ownership.
	 */
	kern_memcpy(&group->image, ui, sizeof(group->image));
	kern_memset(group->image.extattr, 0, sizeof(group->image.extattr));

	/* Keeps whichever attribute blocks the caller asked to retain. */
	if (keep != 0)
		group->image.extattr[0] = ui->extattr[0];

	group->image.extattr_size = (uint32_t)length;
	group->image.blocks -= (uint64_t)released * (ms->super.bsize / UFS_SECTOR_SIZE);

	/* Stages the inode as it will stand once the blocks are gone. */
	error = prepare_inode_locked(&group->image.inode, group->dinode,
				     &fragment);
	if (error != 0)
		return error;

	/* Stages the superblock summaries the release changes. */
	if (released != 0) {
		/* Stages the superblock summaries the release changes. */
		error = prepare_super_summaries(inode->i_mount,
						group->summaries);
		if (error != 0)
			return error;

		drv_ufs_put64(group->summaries,
			      UFS_FS_CSTOTAL_NBFREE,
			      ms->super.cstotal_nbfree + released,
			      ms->super.swapped);
	}

	/*
	 * Publishes only changed maps plus the payload, dinode and changed
	 * totals.
	 */
	extent_count = 0;

	/* Stages every cylinder group the release touches. */
	for (n = 0; n < group->group_count; n++) {
		extents[extent_count].target = (cgstart(&ms->super, group->groups[n]) + ms->super.cblkno) << ms->super.fsbtodb;
		extents[extent_count].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
		extents[extent_count].payload = group->cg[n];
		extent_count++;
	}

	/* Publishes the whole release as one journal group. */
	if (released != 0) {
		extents[extent_count].target = UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;
		extents[extent_count].sectors = UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;
		extents[extent_count].payload = group->summaries;
		extent_count++;
	}

	/* A retained block leaves the inode still owning attributes. */
	if (keep != 0) {
		extents[extent_count].target = ui->extattr[0] << ms->super.fsbtodb;
		extents[extent_count].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
		extents[extent_count].payload = group->data;
		extent_count++;
	}

	/* Adds the inode itself as the last target of the group. */
	extents[extent_count].target = fragment << ms->super.fsbtodb;
	extents[extent_count].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[extent_count].payload = group->dinode;
	extent_count++;

	ms->cg_valid = 0;

	buf_view_release(&ms->cg_view);

	error = metadata_group_commit(inode->i_mount,
				      extents,
				      extent_count,
				      NULL,
				      &outcome);

	/*
	 * Publishes live attribute ownership and releases quota only after
	 * proven commit.
	 */
	if (outcome.committed) {
		kern_memcpy(ui->extattr, group->image.extattr, sizeof(ui->extattr));
		ui->extattr_size = group->image.extattr_size;
		ui->blocks = group->image.blocks;
		ms->super.cstotal_nbfree += released;

		/* Gives the freed blocks back to the owner quota. */
		quota_error = 0;
		if (released != 0) {
			quota_error = quota_release(&ms->quota,
						    inode->i_uid,
						    inode->i_gid,
						    released,
						    0);
		}
		if (quota_error != 0) {
			stop_writing(inode->i_mount, "an extended attribute could not be credited to its quota", quota_error);

			/* Only a committed release may give the quota back. */
			if (error == 0)
				error = quota_error;
		}
	}

	/* An uncertain group leaves the volume unwritable. */
	if (outcome.committed || outcome.uncertain)
		ms->cg_dirty = outcome.uncertain;

	/*
	 * Returns the original errno without compensating writes or repeated
	 * frees.
	 */

	/* Failed: reports why the release could not finish. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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

	/*
	 * A volume without a journal releases attributes one block at a time.
	 */
	if (!ms->journal_enabled)
		return 0;

	*handled = 1;

	/* A caller with data keeps the first block and releases the rest. */
	keep = length != 0;
	if (length > ms->super.bsize || (keep != 0 && area == NULL))
		return EINVAL;

	/* Refuses an attribute length no inode could hold. */
	if ((uint64_t)ui->extattr_size > (uint64_t)UFS_NXADDR * ms->super.bsize)
		return EIO;

	/* Refuses an inode whose pointers disagree with its length. */
	count = (ui->extattr_size + ms->super.bsize - 1U) / ms->super.bsize;
	for (n = 0; n < UFS_NXADDR; n++) {
		/*
		 * A pointer must be filled exactly for the blocks the length
		 * covers.
		 */
		if ((n < count) != (ui->extattr[n] != 0))
			return EIO;
	}

	/* An inode with no attribute blocks has nothing to release. */
	if (count == 0) {
		/* A caller with data still needs a block to write it into. */
		if (keep != 0)
			*handled = 0;
		/* Succeeded. */
		return 0;
	}

	unique = 0;

	/*
	 * Reserves the actual deduplicated footprint before admitting the
	 * group.
	 */
	for (n = keep; n < count; n++) {
		/* Locates the cylinder group each released block lives in. */
		error = xattr_release_location(&ms->super,
					       ui->extattr[n],
					       &groups[n],
					       &local);
		if (error != 0)
			return error;

		/* Counts the cylinder groups the release actually touches. */
		for (j = keep; j < n; j++) {
			/* A group already counted is not staged twice. */
			if (groups[j] == groups[n])
				break;
		}

		/* This block lives in a group the release has not seen yet. */
		if (j == n)
			unique++;
	}

	/* Sizes the staging from the groups and the inode it will hold. */
	bytes = (unique + 1U + keep) * ms->super.bsize;
	if (count > keep)
		bytes += UFS_SBLOCK_SIZE;

	/* A journal too small for the group cannot carry the release. */
	if (ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
		*handled = 0;
		/* Succeeded. */
		return 0;
	}

	/*
	 * Allocates the changed maps, dinode, optional payload and optional
	 * totals.
	 */

	/* Takes the staging the whole release is assembled in. */
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;

	/* Gives up before touching the volume when there is no staging. */
	group->memory = kern_malloc(bytes);
	if (group->memory == NULL) {
		kern_free(group);

		/* Failed. */
		return ENOMEM;
	}

	/* Points each group image at its slice of the staging. */
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
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Clears all existing attribute backing through the shared update owner. */
static int
xattr_release_group(
	struct inode *inode,
	int *handled)
{
	int error;

	/* Keeps the clear caller's explicit handled/result convention. */

	/* Reports the failure. */
	error = xattr_existing_group(inode, NULL, 0, handled);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

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
	int free_bit;
	int error;

	/*
	 * Scans valid allocation groups while the caller excludes other
	 * allocators.
	 */
	ms = state(inode->i_mount);

	/* Tries every cylinder group, starting at the preferred one. */
	for (attempt = 0; attempt < ms->super.ncg; attempt++) {
		cg = (ms->rotor_cg + attempt) % ms->super.ncg;

		/* Reads the cylinder group being searched. */
		error = load_cg_locked(inode->i_mount, cg);
		if (error != 0)
			return error;

		ndblk = cg_ndblk(&ms->super, cg);
		map = ms->cg + ms->cg_freeoff;
		local = (ms->super.dblkno + ms->super.frag - 1U) & ~(ms->super.frag - 1U);

		/*
		 * Requires a complete free filesystem block, without consuming
		 * fragments.
		 */
		while (local < ndblk && ms->super.frag <= ndblk - local) {
			/*
			 * Looks for a run of free fragments a whole block wide.
			 */
			for (n = 0; n < ms->super.frag; n++) {
				/* Asks whether this fragment is free. */
				free_bit = bit_test(map, local + n);

				/* A used fragment ends the run. */
				if (!free_bit)
					break;
			}

			/* The whole block is free, so this is the candidate. */
			if (n == ms->super.frag) {
				free_blocks = drv_ufs_get32(ms->cg, UFS_CG_NBFREE, ms->super.swapped);

				/*
				 * A group with no free blocks left cannot give
				 * one up.
				 */
				if (free_blocks == 0 ||
				    ms->super.cstotal_nbfree == 0) {
					/* Failed. */
					return EIO;
				}

				kern_memcpy(group->images.cg[0], ms->cg,
				       ms->super.bsize);

				/*
				 * Reserves only private bitmap bytes until the
				 * entire group commits.
				 */
				for (n = 0; n < ms->super.frag; n++) {
					bit_clear(group->images.cg[0] +
						  ms->cg_freeoff,
						  local + n);
				}

				/* Takes the block out of the private counts. */
				drv_ufs_put32(group->images.cg[0],
					      UFS_CG_NBFREE, free_blocks - 1U,
					      ms->super.swapped);

				group->fragment = cgstart(&ms->super, cg) + local;
				group->cg = cg;

				/* Succeeded. */
				return 0;
			}

			local += ms->super.frag;
		}
	}

	/*
	 * Reports exhausted full-block capacity without changing live
	 * accounting.
	 */
	return ENOSPC;
}

/*
 * Publishes allocation, initialized metadata and its inode reference together.
 */
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
	uint32_t slot;
	int error;

	/*
	 * Checks the requested owner before selecting backing for its first
	 * block.
	 */
	ms = state(inode->i_mount);
	ui = info(inode);
	images = &group->images;
	slot = 0;

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable)
		return EROFS;

	/* Refuses a block count that would wrap once one block is added. */
	if (ui->blocks > UINT64_MAX - ms->super.bsize / UFS_SECTOR_SIZE)
		return EIO;

	/* An attribute block is only the first if the inode holds none. */
	if (kind == UFS_INITIAL_XATTR) {
		/*
		 * An inode that already names attributes is not at its first
		 * block.
		 */
		if (ui->extattr_size != 0 || ui->extattr[0] != 0 ||
		    ui->extattr[1] != 0) {
			/* Failed. */
			return EIO;
		}
	} else {
		/* A directory block is the one after the blocks its size covers. */
		error = directory_next_block(inode, &slot);
		if (error != 0)
			return error;
	}

	/* Picks the block this allocation will take. */
	error = initial_block_candidate(inode, group);
	if (error != 0)
		return error;

	/*
	 * Prepares a fully initialized payload and a private reference to its
	 * reservation.
	 */
	kern_memset(images->data, 0, ms->super.bsize);

	/* A caller with data writes it into the block before publishing. */
	if (length != 0)
		kern_memcpy(images->data, area, length);

	kern_memcpy(&images->image, ui, sizeof(images->image));

	/* An attribute block records its own length in the inode. */
	if (kind == UFS_INITIAL_XATTR) {
		images->image.extattr[0] = group->fragment;
		images->image.extattr_size = (uint32_t)length;
	} else {
		images->image.direct[slot] = group->fragment;
	}

	images->image.blocks += ms->super.bsize / UFS_SECTOR_SIZE;

	/* Stages the inode pointing at the new block. */
	error = prepare_inode_locked(&images->image.inode,
				     images->dinode,
				     &dinode_fragment);
	if (error != 0)
		return error;

	/* Stages the superblock summaries the allocation changes. */
	error = prepare_super_summaries(inode->i_mount, images->summaries);
	if (error != 0)
		return error;

	drv_ufs_put64(images->summaries,
		      UFS_FS_CSTOTAL_NBFREE,
		      ms->super.cstotal_nbfree - 1U,
		      ms->super.swapped);

	/* Orders every home mutation behind one validated commit record. */
	extents[0].target = (cgstart(&ms->super, group->cg) + ms->super.cblkno) << ms->super.fsbtodb;
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

	error = metadata_group_commit(inode->i_mount,
				      extents,
				      4,
				      NULL,
				      &group->outcome);

	/*
	 * Makes the initialized area visible in RAM only after positive commit.
	 */
	if (group->outcome.committed) {
		kern_memcpy(ms->cg, images->cg[0], ms->super.bsize);
		ms->super.cstotal_nbfree--;
		ms->rotor_cg = group->cg;

		/* Publishes the attribute length the group committed. */
		if (kind == UFS_INITIAL_XATTR) {
			ui->extattr[0] = group->fragment;
			ui->extattr_size = (uint32_t)length;
		} else {
			ui->direct[slot] = group->fragment;
		}

		ui->blocks = images->image.blocks;
	}

	/* An uncertain group leaves the volume unwritable. */
	if (group->outcome.committed || group->outcome.uncertain)
		ms->cg_dirty = group->outcome.uncertain;

	/*
	 * Preserves the original errno and its separately recorded ownership
	 * outcome.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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

	/* A volume without a journal takes the block one step at a time. */
	if (!ms->journal_enabled)
		return 0;

	*handled = 1;

	/* An attribute block carries the caller data into the new block. */
	if (kind == UFS_INITIAL_XATTR) {
		/* Rejects a value no attribute block could hold. */
		if (area == NULL || length == 0 || length > ms->super.bsize)
			return EINVAL;
	} else if (kind != UFS_INITIAL_DIRECTORY || area != NULL ||
		   length != 0) {
		/* Failed. */
		return EINVAL;
	}

	/* Sizes the staging from the images the group will hold. */
	bytes = 3U * ms->super.bsize + UFS_SBLOCK_SIZE;
	if (ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
		*handled = 0;
		/* Succeeded. */
		return 0;
	}

	/*
	 * Allocates all private images before reserving quota.
	 */

	/* Takes the staging the whole allocation is assembled in. */
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;

	/* Gives up before touching the volume when there is no staging. */
	group->images.memory = kern_malloc(bytes);
	if (group->images.memory == NULL) {
		kern_free(group);

		/* Failed. */
		return ENOMEM;
	}

	group->images.cg[0] = group->images.memory;
	group->images.dinode = group->images.memory + ms->super.bsize;
	group->images.data = group->images.dinode + ms->super.bsize;
	group->images.summaries = group->images.data + ms->super.bsize;

	/* Charges the block against the owner quota before taking it. */
	error = quota_reserve(&ms->quota,
			      inode->i_uid,
			      inode->i_gid,
			      1,
			      0,
			      quota_now(),
			      &group->charge);
	if (error == 0) {
		mutex_lock(&ms->lock);
		error = initial_block_locked(inode, kind, area, length, group);

		/*
		 * Retains quota for a possibly committed allocation until
		 * recovery settles it.
		 */
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
	 * allocator.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Allocates initialized attribute backing through the common first-block owner.
 */
static int
xattr_allocate_group(
	struct inode *inode,
	const uint8_t *area,
	size_t length,
	int *handled)
{
	int error;

	/*
	 * Preserves the xattr caller's admission and errno contract.
	 */

	/* Reports the failure. */
	error = initial_block_group(inode, UFS_INITIAL_XATTR, area, length,
				    handled);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Finds the slot of the block a directory grows into next.
 *
 * A directory grows by whole blocks once its size fills the ones it has, so
 * the next block is the one its size would reach: every slot before it is
 * filled, it and every slot after it are empty, and no indirect block is
 * named.  Past the direct blocks the directory cannot grow here.
 */
static int
directory_next_block(
	struct inode *inode,
	uint32_t *slot)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	uint32_t next;
	uint32_t n;

	/* Takes the mount state and the private inode. */
	ms = state(inode->i_mount);
	ui = info(inode);

	/* Only a directory whose size fills whole blocks grows by one. */
	if (inode->i_type != INODE_DIR || inode->i_size < 0)
		return EIO;
	if ((uint64_t)inode->i_size % ms->super.bsize != 0)
		return EIO;

	/* Refuses a directory that has used every direct block. */
	next = (uint32_t)((uint64_t)inode->i_size / ms->super.bsize);
	if (next >= UFS_NDADDR)
		return ENOSPC;

	/* The slots before the next one are filled, the rest are empty. */
	for (n = 0; n < UFS_NDADDR; n++) {
		/* A filled slot must be before the next one, an empty one not. */
		if (n < next && ui->direct[n] == 0)
			return EIO;
		if (n >= next && ui->direct[n] != 0)
			return EIO;
	}

	/* Refuses an inode that already names indirect blocks. */
	for (n = 0; n < UFS_NIADDR; n++) {
		/* A directory here never names one. */
		if (ui->indirect[n] != 0)
			return EIO;
	}

	/* Reports the slot the next block takes. */
	*slot = next;
	return 0;
}

/*
 * Allocates the next empty block of a directory without publishing any
 * directory entry.
 */
static int
directory_backing_group(
	struct inode *inode,
	int *handled)
{
	int error;

	/*
	 * Leaves size and link counts untouched until a later namespace
	 * operation.
	 */

	/* Reports the failure. */
	error = initial_block_group(inode,
				    UFS_INITIAL_DIRECTORY,
				    NULL,
				    0,
				    handled);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Batches existing full blocks while preserving allocation and size
 * publication.
 */
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

	/*
	 * Validates the request before taking the inode's mutation lock.
	 */

	/* Takes the mount the write runs against. */
	ms = state(inode->i_mount);
	if (!ms->writable)
		return -EROFS;

	/* Rejects an offset that is negative or that would wrap. */
	if (offset < 0 || (uint64_t)offset + length < (uint64_t)offset)
		return -EINVAL;

	/* Refuses a write past the largest file this volume can hold. */
	if ((uint64_t)offset + length > ms->super.maxfilesize ||
	    (uint64_t)offset + length >
	    (sizeof(off_t) == 8 ? INT64_MAX : INT32_MAX)) {
		/* Failed. */
		return -EFBIG;
	}

	scratch = NULL;
	done = 0;
	final_error = 0;
	metadata_dirty = 0;
	mutex_lock(&inode->i_lock);

	/*
	 * Limits direct runs to already published file bytes and allocated
	 * blocks.
	 */
	while (done < length) {
		position = (uint64_t)offset + done;
		logical = position / ms->super.bsize;
		within = (size_t)(position % ms->super.bsize);

		/*
		 * Resolves the block this offset falls in, without allocating.
		 */
		error = bmap(inode, logical, &fragment);
		if (error != 0) {
			final_error = error;
			break;
		}

		/*
		 * Initialize new full blocks with a private
		 * allocation/publication batch.
		 */
		if (fragment == 0 && within == 0) {
			/*
			 * Offers the run to the allocating path, which may take
			 * it whole.
			 */
			allocated = allocation_write_run(
				inode, (const uint8_t *)buffer + done,
				length - done, logical, context);
			if (allocated < 0) {
				final_error = (int)-allocated;
				break;
			}

			/*
			 * The allocating path carried this part of the write.
			 */
			if (allocated > 0) {
				done += (size_t)allocated;
				metadata_dirty = 0;
				continue;
			}
		}

		/*
		 * A whole-block run may be written straight from the caller
		 * buffer.
		 */
		eligible = 0;
		if (within == 0 && position < (uint64_t)inode->i_size) {
			/* The rest of the request is eligible for that run. */
			eligible = length - done;
			if (eligible > (uint64_t)inode->i_size - position) {
				eligible = (size_t)((uint64_t)inode->i_size - position);
			}
		}

		/*
		 * Measures how much of the request one contiguous run covers.
		 */
		amount = content_run_bytes(inode,
					   logical,
					   fragment,
					   eligible,
					   1,
					   &mapping_error,
					   NULL);
		if (amount != 0) {
			io_stats_record(IO_UFS_CONTENT_WRITE, amount);
			error = write_content_sectors_context(
				inode->i_mount,
				(uint64_t)fragment << ms->super.fsbtodb,
				(uint32_t)(amount / UFS_SECTOR_SIZE),
				(const uint8_t *)buffer + done, context);
		} else {
			/*
			 * Keeps the established allocation/zero and
			 * partial-block path.
			 */
			if (scratch == NULL) {
				/*
				 * Takes the staging a partial block is edited
				 * in.
				 */
				scratch = kern_malloc(ms->super.bsize);
				if (scratch == NULL) {
					final_error = ENOMEM;
					break;
				}
			}

			/*
			 * A partial block is filled to the end of that block.
			 */
			amount = ms->super.bsize - within;
			if (amount > length - done)
				amount = length - done;

			/*
			 * A hole has to be filled before it can be written
			 * into.
			 */
			if (fragment == 0) {
				/* Allocates the block this offset falls in. */
				error = bmap_ensure(inode, logical, &fragment);
				if (error != 0) {
					final_error = error;
					break;
				}

				kern_memset(scratch, 0, ms->super.bsize);
			} else if (within != 0 || amount != ms->super.bsize) {
				/*
				 * Reads the block so the untouched bytes
				 * survive the write.
				 */
				error = read_content_block(inode->i_mount,
							   fragment, scratch);
				if (error != 0) {
					final_error = error;
					break;
				}
			}

			/* Lays the bytes into the block and writes it. */
			kern_memcpy(scratch + within, (const uint8_t *)buffer + done,
			       amount);
			error = write_content_context(inode->i_mount, fragment,
						      scratch, context);
		}
		if (error != 0) {
			final_error = error;
			break;
		}

		done += amount;
		metadata_dirty = 1;

		/* Grows the recorded size to cover what the write reached. */
		if ((uint64_t)inode->i_size < (uint64_t)offset + done)
			inode->i_size = (off_t)((uint64_t)offset + done);

		/* A mapping failure leaves the recorded size untrustworthy. */
		if (mapping_error != 0) {
			final_error = mapping_error;
			break;
		}
	}

	/*
	 * Preserves the existing metadata publication and partial-result
	 * convention.
	 */
	if (done != 0 && metadata_dirty) {
		/* Publishes the inode with its new size. */
		error = persist_inode(inode);
		if (error != 0 && final_error == 0)
			final_error = error;
	}

	mutex_unlock(&inode->i_lock);

	kern_free(scratch);

	/*
	 * Reports only the completed prefix, or the first error without
	 * progress.
	 */
	if (done != 0)
		return (ssize_t)done;

	return -final_error;	/* Failed. */
}

/* Reports how many blocks one indirect level reaches over. */
static uint64_t
indirect_span(
	const struct ufs_super *super,
	unsigned depth)
{
	uint64_t span = 1;

	/* Each level spans nindir times as much as the one below. */
	while (depth-- != 0)
		span *= super->nindir;

	/* Reports the span of that level. */
	return span;
}

/*
 * Prepares a private pointer removal and free map under mount mutation
 * ownership.
 */
static int
release_group_locked(
	struct inode *inode,
	uint64_t parent,
	unsigned index,
	uint64_t child,
	struct ufs_release_group *group)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	struct ufs_journal_extent extents[4];
	struct ufs_transaction_outcome outcome;
	uint64_t start;
	uint64_t fragment;
	uint64_t current;
	uint32_t cg;
	uint32_t end;
	uint32_t local;
	uint32_t ndblk;
	uint32_t free_blocks;
	unsigned n;
	unsigned count;
	int already_free;
	int error;
	int quota_error;

	ms = state(inode->i_mount);
	ui = info(inode);
	local = 0;

	/*
	 * Refuses malformed accounting before preparing any reusable
	 * allocation.
	 */
	if (!ms->writable)
		return EROFS;

	/* A block count below one block means the inode is corrupt. */
	if (ui->blocks < ms->super.bsize / UFS_SECTOR_SIZE)
		return EIO;

	/* Finds the cylinder group the block being released lives in. */
	cg_search_range(&ms->super, child, &cg, &end);
	for (; cg < end; cg++) {
		/* Locates where this group starts on the volume. */
		start = cgstart(&ms->super, cg);

		/* How many data fragments this group holds. */
		ndblk = cg_ndblk(&ms->super, cg);

		/* A block outside this group's data area belongs to another. */
		if (child < start || child - start < ms->super.dblkno ||
		    child - start >= ndblk)
			continue;

		/* The block position inside its own group. */
		local = (uint32_t)(child - start);

		/* Refuses a block that is not aligned to a whole block. */
		if (local % ms->super.frag != 0 ||
		    ms->super.frag > ndblk - local) {
			/* Failed. */
			return EIO;
		}
		break;
	}

	/* A block that belongs to no group means the pointer is corrupt. */
	if (cg == end)
		return EIO;

	/* Reads that cylinder group so its free map can be inspected. */
	error = load_cg_locked(inode->i_mount, cg);
	if (error != 0)
		return error;

	kern_memcpy(group->cg, ms->cg, ms->super.bsize);

	/* Refuses a block the free map already calls free. */
	for (n = 0; n < ms->super.frag; n++) {
		/* Asks the free map whether it already holds this fragment. */
		already_free = bit_test(group->cg + ms->cg_freeoff, local + n);

		/* A block that is already free must not be released twice. */
		if (already_free)
			return EIO;
	}

	/* Refuses a count the summaries could not hold once raised. */
	free_blocks = drv_ufs_get32(group->cg, UFS_CG_NBFREE, ms->super.swapped);
	if (free_blocks == UINT32_MAX || ms->super.cstotal_nbfree == UINT64_MAX)
		return EIO;

	/*
	 * Validates the current reference and changes only a private inode or
	 * parent.
	 */
	kern_memcpy(&group->image, ui, sizeof(group->image));

	/* A block named by an indirect block is cleared inside it. */
	if (parent != 0) {
		/* An index past the block cannot name an entry in it. */
		if (index >= ms->super.nindir)
			return EIO;

		/* Reads the indirect block the pointer lives in. */
		error = read_block(inode->i_mount, parent, group->parent);
		if (error != 0)
			return error;
		current = drv_ufs_get64(group->parent, index * 8U,
					ms->super.swapped);
		drv_ufs_put64(group->parent, index * 8U, 0, ms->super.swapped);
	} else if (index < UFS_NDADDR) {
		current = group->image.direct[index];
		group->image.direct[index] = 0;
	} else {
		/* An index past the indirect levels names nothing. */
		if (index - UFS_NDADDR >= UFS_NIADDR)
			return EIO;
		current = group->image.indirect[index - UFS_NDADDR];
		group->image.indirect[index - UFS_NDADDR] = 0;
	}

	/* The pointer does not name the block this release was given. */
	if (current != child)
		return EIO;
	group->image.blocks -= ms->super.bsize / UFS_SECTOR_SIZE;

	/* Stages the inode as it will stand once the block is gone. */
	error = prepare_inode_locked(&group->image.inode, group->dinode,
				     &fragment);
	if (error != 0)
		return error;

	/* Stages the superblock summaries the release changes. */
	error = prepare_super_summaries(inode->i_mount, group->summaries);
	if (error != 0)
		return error;

	/* Keeps the live free map unchanged until the release has committed. */
	for (n = 0; n < ms->super.frag; n++)
		bit_set(group->cg + ms->cg_freeoff, local + n);

	/* Gives the block back to the counts the group and volume keep. */
	drv_ufs_put32(group->cg, UFS_CG_NBFREE, free_blocks + 1U, ms->super.swapped);
	drv_ufs_put64(group->summaries, UFS_FS_CSTOTAL_NBFREE, ms->super.cstotal_nbfree + 1U, ms->super.swapped);
	extents[0].target = (cgstart(&ms->super, cg) + ms->super.cblkno) << ms->super.fsbtodb;
	extents[0].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[0].payload = group->cg;
	extents[1].target = UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;
	extents[1].sectors = UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;
	extents[1].payload = group->summaries;
	extents[2].target = fragment << ms->super.fsbtodb;
	extents[2].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[2].payload = group->dinode;
	count = 3;

	/* Stages the indirect block with the pointer cleared. */
	if (parent != 0) {
		extents[3].target = parent << ms->super.fsbtodb;
		extents[3].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
		extents[3].payload = group->parent;
		count++;
	}

	/*
	 * Releases optional home-cache pins before the journal installs that
	 * block.
	 */
	ms->cg_valid = 0;
	buf_view_release(&ms->cg_view);
	error = metadata_group_commit(inode->i_mount, extents, count, NULL,
				      &outcome);

	/* Only a committed release may give the quota back. */
	if (outcome.committed) {
		kern_memcpy(ms->cg, group->cg, ms->super.bsize);
		ms->super.cstotal_nbfree++;
		kern_memcpy(ui->direct, group->image.direct, sizeof(ui->direct));
		kern_memcpy(ui->indirect, group->image.indirect,
		       sizeof(ui->indirect));
		ui->blocks = group->image.blocks;

		/* Gives the freed block back to the owner quota. */
		quota_error = quota_release(&ms->quota, inode->i_uid,
					    inode->i_gid, 1, 0);
		if (quota_error != 0) {
			stop_writing(inode->i_mount, "a released block could not be credited to its quota", quota_error);

			/* Publishes the inode as the group committed it. */
			if (error == 0)
				error = quota_error;
		}
	}

	/* An uncertain group leaves the volume unwritable. */
	if (outcome.committed || outcome.uncertain) {
		ms->cg_valid = 0;
		ms->cg_dirty = outcome.uncertain;
		buf_view_release(&ms->cg_view);
	}

	/*
	 * Preserves errno separately from any release that recovery
	 * established.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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
	struct ufs_mount_state *ms;
	struct ufs_release_group *group;
	size_t bytes;
	int error;

	/* Takes the mount state this call runs against. */
	ms = state(inode->i_mount);

	/*
	 * Keeps oversized or non-journal releases on the existing ordered path.
	 */
	*handled = 0;

	/* Sizes the staging from the images the group will hold. */
	bytes = (parent != 0 ? 3U : 2U) * ms->super.bsize + UFS_SBLOCK_SIZE;
	if (!ms->journal_enabled)
		return 0;

	/* A group too wide for the journal cannot be carried by it. */
	if (bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
		/* Succeeded. */
		return 0;
	}
	*handled = 1;

	/* Takes the staging the whole release is assembled in. */
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(3U * ms->super.bsize + UFS_SBLOCK_SIZE);

	/* Gives up before touching the volume when there is no staging. */
	if (group->memory == NULL) {
		kern_free(group);

		/* Failed. */
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
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Detach one inode-owned pointer durably before making its block reusable. On
 * uncertain metadata I/O keep the allocation and stop further mutations.
 */
static int
detach_inode_block(
	struct inode *inode,
	uint64_t *pointer)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	uint64_t fragment;
	unsigned sectors;
	unsigned index;
	int error;
	int handled;

	ms = state(inode->i_mount);
	ui = info(inode);
	fragment = *pointer;
	sectors = ms->super.bsize / UFS_SECTOR_SIZE;

	/* A pointer that names no block has nothing to detach. */
	if (fragment == 0)
		return 0;

	/* A block count below one block means the inode is corrupt. */
	if (ui->blocks < sectors)
		return EIO;
	/* Finds which direct pointer is being cleared. */
	for (index = 0; index < UFS_NDADDR; index++) {
		/* This is the direct pointer the caller named. */
		if (pointer == &ui->direct[index])
			break;
	}

	/* A pointer that is not direct must be one of the indirect roots. */
	if (index == UFS_NDADDR) {
		/* Finds which indirect root is being cleared. */
		for (index = 0; index < UFS_NIADDR; index++) {
			/* This is the indirect root the caller named. */
			if (pointer == &ui->indirect[index])
				break;
		}

		index += UFS_NDADDR;
	}

	/* Asks whether the journal path has already carried the release out. */
	error = release_group(inode, 0, index, fragment, &handled);
	if (handled)
		return error;

	*pointer = 0;
	ui->blocks -= sectors;

	/* Publishes the inode with the pointer cleared. */
	error = persist_inode(inode);
	if (error == 0)
		error = order_barrier(inode->i_mount);
	if (error == 0) {
		error = free_block(inode->i_mount, fragment, inode->i_uid,
				   inode->i_gid);
	}
	if (error != 0)
		stop_writing(inode->i_mount, "a block could not be detached from its inode", error);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Leave an empty root allocated until its caller has detached the owning
 * pointer. A child is never freed while its parent still names it on disk.
 */
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
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	uint8_t *block;
	uint64_t child_span;
	unsigned index;
	unsigned sectors;
	int error;

	ms = state(inode->i_mount);
	ui = info(inode);
	child_span = indirect_span(&ms->super, depth - 1U);
	sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	error = 0;

	*empty = 1;

	/* A level that is not there has nothing to detach. */
	if (root == 0)
		return 0;

	/* Takes the staging the indirect block is edited in. */
	block = kern_malloc(ms->super.bsize);
	if (block == NULL)
		return ENOMEM;

	/* Reads the indirect block this level is named by. */
	error = read_block(inode->i_mount, root, block);
	if (error != 0)
		goto out;

	/* Walks the entries of the block, deepest level first. */
	for (index = 0; index < ms->super.nindir; index++) {
		child = drv_ufs_get64(block, (size_t)index * 8U, ms->super.swapped);
		child_base = base + (uint64_t)index * child_span;
		remove = 0;

		/* An empty entry names nothing to detach. */
		if (child == 0)
			continue;

		/* The last level names the file blocks themselves. */
		if (depth == 1U) {
			remove = child_base >= keep;
		} else if (child_base + child_span > keep) {
			/*
			 * Walks the level below before detaching anything at
			 * this one.
			 */
			error = truncate_indirect(inode, child, depth - 1U,
						  child_base, keep, &remove);
			if (error != 0)
				goto out;
		}

		/* An entry below the new end is kept. */
		if (!remove) {
			*empty = 0;
			continue;
		}

		/*
		 * A block count below what is being freed means the inode is
		 * corrupt.
		 */
		if (ui->blocks < sectors) {
			error = EIO;
			goto out;
		}

		/*
		 * Asks whether the journal path has already carried the release
		 * out.
		 */
		error = release_group(inode, root, index, child, &handled);
		if (handled) {
			/* Reports why the block could not be released. */
			if (error != 0)
				goto out;
			drv_ufs_put64(block, (size_t)index * 8U, 0,
				      ms->super.swapped);
			continue;
		}

		drv_ufs_put64(block, (size_t)index * 8U, 0, ms->super.swapped);

		/* Writes the indirect block back with the entries cleared. */
		error = write_block(inode->i_mount, root, block);
		if (error == 0)
			error = order_barrier(inode->i_mount);
		if (error == 0) {
			ui->blocks -= sectors;
			error = free_block(inode->i_mount, child, inode->i_uid,
					   inode->i_gid);
		}
		if (error != 0) {
			/* Names the file, the indirect block, the entry and the block it named. */
			kern_logf("ufs: %s: inode %llu: entry %u of indirect block %llu, naming block %llu, could not be released (error %d)\n",
				  inode->i_mount->m_disk->d_name,
				  (unsigned long long)inode->i_ino,
				  index,
				  (unsigned long long)root,
				  (unsigned long long)child,
				  error);
			stop_writing(inode->i_mount, "an indirect block could not be truncated", error);
			goto out;
		}
	}

out:
	kern_free(block);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Changes the size of a file. */
static int
ufs_truncate(
	struct inode *inode,
	off_t size)
{
	uint64_t fragment;
	int empty;
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	uint8_t *block;
	uint64_t keep;
	uint64_t base;
	unsigned n;
	int error;

	ms = state(inode->i_mount);
	ui = info(inode);
	block = NULL;
	error = 0;

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable)
		return EROFS;

	/* Refuses a size no file on this volume could have. */
	if (size < 0 || (uint64_t)size > ms->super.maxfilesize)
		return EFBIG;

	mutex_lock(&inode->i_lock);

	keep = ((uint64_t)size + ms->super.bsize - 1U) / ms->super.bsize;

	/* A shrink that stops mid-block has to clear the rest of it. */
	if (size < inode->i_size && size != 0 && size % ms->super.bsize != 0) {
		fragment = 0;

		/* Resolves the block the new end of file falls in. */
		error = bmap(inode, (uint64_t)size / ms->super.bsize,
			     &fragment);
		if (error != 0)
			goto out;

		/* A hole at the new end has nothing to clear. */
		if (fragment != 0) {
			/* Takes the staging that block is edited in. */
			block = kern_malloc(ms->super.bsize);
			if (block == NULL) {
				error = ENOMEM;
				goto out;
			}

			/*
			 * Reads the block so the bytes before the new end
			 * survive.
			 */
			error = read_content_block(inode->i_mount, fragment, block);
			if (error != 0)
				goto out;
			kern_memset(block + size % ms->super.bsize,
			       0,
			       ms->super.bsize - size % ms->super.bsize);

			/* Writes the block back with the tail cleared. */
			error = write_content_block(inode->i_mount,
						    fragment,
						    block);
			if (error != 0)
				goto out;
		}
	}

	/*
	 * Bound before narrowing: a large sparse size must not wrap to a
	 * direct-block index and release unrelated data.
	 */
	for (n = 0; n < UFS_NDADDR; n++) {
		/* A direct block below the new end is kept. */
		if ((uint64_t)n < keep)
			continue;

		/* Detaches a direct block the file no longer reaches. */
		error = detach_inode_block(inode, &ui->direct[n]);
		if (error != 0)
			goto out;
	}

	/* Detaches whatever the indirect levels no longer reach. */
	base = UFS_NDADDR;
	for (n = 0; n < UFS_NIADDR; n++) {
		/*
		 * Walks one indirect level, detaching what falls past the end.
		 */
		error = truncate_indirect(inode, ui->indirect[n], n + 1U, base,
					  keep, &empty);

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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Renders the file-type field of a stored mode as an inode type.
 *
 * A field this kernel does not define reads as no type at all, which is what
 * makes a corrupt inode refuse to be used rather than be taken for a file.
 */
static enum inode_type
mode_type(
	uint16_t mode)
{
	/*
	 * The type is the top bits of the mode, in the format's own encoding.
	 */
	switch (mode & UFS_IFMT) {
	case UFS_IFREG:
		return INODE_REG;
	case UFS_IFDIR:
		return INODE_DIR;
	case UFS_IFLNK:
		return INODE_SYMLINK;
	case UFS_IFCHR:
		return INODE_CHAR;
	case UFS_IFBLK:
		return INODE_BLOCK;
	case UFS_IFIFO:
		return INODE_FIFO;
	case UFS_IFSOCK:
		return INODE_SOCKET;
	default:
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
	uint64_t disk_size;
	uint64_t disk_blocks;

	disk_size = drv_ufs_get64(raw, UFS_DI_SIZE, super->swapped);
	disk_blocks = drv_ufs_get64(raw, UFS_DI_BLOCKS, super->swapped);

	/* Refuses a size no file on this volume could have. */
	if (disk_size >
	    (sizeof(off_t) == 8 ? (uint64_t)INT64_MAX : (uint64_t)INT32_MAX)) {
		/* Failed. */
		return EFBIG;
	}

	/* Refuses a block count no file on this volume could have. */
	if (disk_blocks >
	    (sizeof(blkcnt_t) == 8 ? (uint64_t)INT64_MAX
	     : (uint64_t)INT32_MAX)) {
		/* Failed. */
		return EOVERFLOW;
	}
	*size = disk_size;
	*blocks = disk_blocks;
	/* Succeeded. */
	return 0;
}

/*
 * Decodes one private identity, keeping namespace and recovery admission
 * distinct.
 */
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
	uint64_t disk_size;
	uint64_t disk_blocks;
	enum inode_type type;
	uint16_t mode;
	unsigned n;
	int valid;
	int error;

	/*
	 * Validates the disk representation before populating the private
	 * inode.
	 */

	/* Renders the stored mode as the type and permissions of the inode. */
	mode = drv_ufs_get16(raw, UFS_DI_MODE, s->swapped);
	type = mode_type(mode);

	/* A mode this driver has no file kind for cannot be represented. */
	if (type == INODE_NONE)
		return EOPNOTSUPP;

	/* Reads the size and block count, refusing values that disagree. */
	error = inode_size_values(raw, s, &disk_size, &disk_blocks);
	if (error != 0)
		return error;
	ui = info(inode);

	/* The kind of file, its permissions, and the number it was read as. */
	inode->i_type = type;
	inode->i_ino = number;
	inode->i_mode = mode;

	/* How many names in the file system point at this inode. */
	inode->i_linkcount = drv_ufs_get16(raw, UFS_DI_NLINK, s->swapped);

	/* The length of the file, already checked against the block count. */
	inode->i_size = (off_t)disk_size;

	/* The two identities the file is accounted against. */
	inode->i_uid = drv_ufs_get32(raw, UFS_DI_UID, s->swapped);
	inode->i_gid = drv_ufs_get32(raw, UFS_DI_GID, s->swapped);

	/* When the contents were last read, in seconds and nanoseconds. */
	inode->i_atime.tv_sec = (time_t)drv_ufs_get64(raw, UFS_DI_ATIME, s->swapped);
	inode->i_atime.tv_nsec = drv_ufs_get32(raw, UFS_DI_ATIMENSEC, s->swapped);

	/* When they were last written. */
	inode->i_mtime.tv_sec = (time_t)drv_ufs_get64(raw, UFS_DI_MTIME, s->swapped);
	inode->i_mtime.tv_nsec = drv_ufs_get32(raw, UFS_DI_MTIMENSEC, s->swapped);

	/* And when the inode itself last changed. */
	inode->i_ctime.tv_sec = (time_t)drv_ufs_get64(raw, UFS_DI_CTIME, s->swapped);
	inode->i_ctime.tv_nsec = drv_ufs_get32(raw, UFS_DI_CTIMENSEC, s->swapped);

	/* How many bytes of extended attributes the inode carries. */
	ui->extattr_size = drv_ufs_get32(raw, UFS_DI_EXTSIZE, s->swapped);

	/* And the blocks those attributes live in. */
	for (n = 0; n < UFS_NXADDR; n++) {
		ui->extattr[n] =
			drv_ufs_get64(raw, UFS_DI_EXTB + n * 8U, s->swapped);
	}

	/* A device inode keeps its number where the first block would be. */
	if (inode->i_type == INODE_CHAR || inode->i_type == INODE_BLOCK) {
		inode->i_rdev =
			(dev_t)drv_ufs_get64(raw, UFS_DI_DB, s->swapped);
	} else if (inode->i_type == INODE_SYMLINK &&
		   (uint64_t)inode->i_size <= s->maxsymlinklen &&
		   inode->i_size <= 120) {
		kern_memcpy(ui->shortlink, raw + UFS_DI_DB, sizeof(ui->shortlink));
	} else {
		/* Reads the direct block pointers. */
		for (n = 0; n < UFS_NDADDR; n++) {
			ui->direct[n] = drv_ufs_get64(raw, UFS_DI_DB + n * 8U,
						      s->swapped);
		}

		/* Reads the indirect block pointers. */
		for (n = 0; n < UFS_NIADDR; n++) {
			ui->indirect[n] = drv_ufs_get64(raw, UFS_DI_IB + n * 8U,
							s->swapped);
		}
	}

	ui->disk_flags = drv_ufs_get32(raw, UFS_DI_FLAGS, s->swapped);
	ui->blocks = disk_blocks;
	ui->generation = drv_ufs_get32(raw, UFS_DI_GEN, s->swapped);

	/* An orphan is on the list precisely because nothing links to it. */
	if (orphan && inode->i_linkcount != 0) {
		/* Failed. */
		return EIO;
	}

	/* A live inode is reachable, so at least one name must point at it. */
	if (!orphan && inode->i_linkcount == 0) {
		/* Failed. */
		return EIO;
	}

	/* A negative size is not a length this file system can store. */
	if (inode->i_size < 0)
		return EIO;	/* Failed. */

	/* Nor one longer than the volume geometry could ever address. */
	if ((uint64_t)inode->i_size > s->maxfilesize)
		return EIO;	/* Failed. */

	/* The attributes cannot span more blocks than an inode has pointers. */
	if (ui->extattr_size > UFS_NXADDR * s->bsize)
		return EIO;	/* Failed. */

	/* A time whose nanoseconds overflow into seconds was never written. */
	if (inode->i_atime.tv_nsec >= 1000000000L)
		return EIO;	/* Failed. */

	if (inode->i_mtime.tv_nsec >= 1000000000L)
		return EIO;	/* Failed. */

	if (inode->i_ctime.tv_nsec >= 1000000000L)
		return EIO;	/* Failed. */

	/* A live directory holds at least the block that names dot. */
	if (inode->i_type == INODE_DIR && !orphan &&
	    (uint64_t)inode->i_size < UFS_DIRBLKSIZ)
		return EIO;	/* Failed. */

	/* And every directory is a whole number of record blocks long. */
	if (inode->i_type == INODE_DIR &&
	    (uint64_t)inode->i_size % UFS_DIRBLKSIZ != 0)
		return EIO;	/* Failed. */

	/* Refuses an attribute pointer that disagrees with the length. */
	for (n = 0; n < UFS_NXADDR; n++) {
		/*
		 * A pointer is needed exactly for the blocks the length covers.
		 */
		needed = ui->extattr_size > n * s->bsize;

		/* A block the length covers must have a pointer to it. */
		if (needed && ui->extattr[n] == 0) {
			/* Failed. */
			return EIO;
		}

		/* And a block it does not cover must have none. */
		if (!needed && ui->extattr[n] != 0) {
			/* Failed. */
			return EIO;
		}

		/* Asks whether the pointer names a block of this volume. */
		valid = 1;
		if (needed)
			valid = valid_inode_fragment(s, ui->extattr[n]);

		/* A pointer outside the data area means corruption. */
		if (!valid) {
			/* Failed. */
			return EIO;
		}
	}

	/* A short symbolic link keeps its target where the blocks would be. */
	if (!(inode->i_type == INODE_SYMLINK &&
	      (uint64_t)inode->i_size <= s->maxsymlinklen &&
	      inode->i_size <= 120)) {
		/*
		 * Refuses a direct pointer that names no block of this volume.
		 */
		for (n = 0; n < UFS_NDADDR; n++) {
			/*
			 * Asks whether the pointer names a block of this
			 * volume.
			 */
			valid = valid_inode_fragment(s, ui->direct[n]);

			/* A pointer outside the data area means corruption. */
			if (!valid) {
				return EIO;
			}
		}

		/*
		 * Refuses an indirect pointer that names no block of this
		 * volume.
		 */
		for (n = 0; n < UFS_NIADDR; n++) {
			/*
			 * Asks whether the pointer names a block of this
			 * volume.
			 */
			valid = valid_inode_fragment(s, ui->indirect[n]);

			/* A pointer outside the data area means corruption. */
			if (!valid) {
				return EIO;
			}
		}
	}

	inode->i_op = &ufs_inode_ops;

	/* Which file operations apply follows from the kind of file it is. */
	switch (inode->i_type) {
	case INODE_DIR:
		inode->i_fop = &ufs_directory_ops;
		break;
	case INODE_REG:
		inode->i_fop = &ufs_regular_ops;
		break;
	case INODE_FIFO:
		inode->i_fop = &fifo_file_ops;
		break;
	default:
		/* A device or socket is served by the node it names. */
		inode->i_fop = NULL;
		break;
	}

	/* Returns a validated identity without adding it to the inode cache. */
	return 0;
}

/* Reads one inode off the volume, with the mount lock held. */
static int
load_inode_locked(
	struct mount *mountp,
	uint32_t number,
	struct inode **result)
{
	const struct ufs_super *s = &state(mountp)->super;
	struct inode *inode;
	uint8_t *block;
	uint8_t *raw;
	uint32_t cg;
	uint32_t index;
	uint64_t fragment;
	uint64_t disk_size;
	uint64_t disk_blocks;
	enum inode_type type;
	uint16_t mode;
	int cached;
	int error;

	/* Refuses a number outside the range this volume defines. */
	if (number < UFS_ROOT_INO || number >= s->ncg * s->ipg)
		return EIO;

	/* Asks the inode cache whether this number is already in core. */
	cached = inode_get(mountp, number, result);

	/* Succeeded: an inode already in core is handed back as it is. */
	if (cached == 0)
		return 0;

	cg = number / s->ipg;
	index = number % s->ipg;
	fragment = cgstart(s, cg) + s->iblkno + (index / s->inopb) * s->frag;

	/* Takes the staging the inode block is read into. */
	block = kern_malloc(s->bsize);
	if (block == NULL)
		return ENOMEM;

	/* Reads the block the inode lives in. */
	error = read_block(mountp, fragment, block);
	if (error != 0) {
		kern_free(block);

		/* Failed. */
		return error;
	}

	raw = block + (index % s->inopb) * UFS_DINODE_SIZE;

	/* Refuses an inode whose type field this kernel does not define. */
	mode = drv_ufs_get16(raw, UFS_DI_MODE, s->swapped);
	type = mode_type(mode);
	if (type == INODE_NONE) {
		kern_free(block);

		/* Failed. */
		return EOPNOTSUPP;
	}

	/* Reads the size and block count, refusing values that disagree. */
	error = inode_size_values(raw, s, &disk_size, &disk_blocks);
	if (error != 0) {
		kern_free(block);

		/* Failed. */
		return error;
	}

	/* Takes the in-core inode the file will be described by. */
	inode = inode_alloc(mountp);
	if (inode == NULL) {
		kern_free(block);

		/* Failed. */
		return ENOSPC;
	}

	/* Fills it from the raw inode just read. */
	error = decode_inode_raw(inode, raw, number, 0);
	if (error != 0) {
		inode->i_flags |= INODE_DEAD;
		inode_release(inode);
		kern_free(block);

		/* Failed. */
		return error;
	}

	kern_free(block);
	*result = inode;
	/* Succeeded. */
	return 0;
}

/*
 * Creation already holds this gate. Miss, allocation and initialization must
 * form one admission so aliases cannot publish duplicate in-core objects.
 */
static int
load_inode(
	struct mount *mountp,
	uint32_t number,
	struct inode **result)
{
	struct ufs_mount_state *ms;
	int owned;
	int error;

	/* A caller that holds the namespace alone loads with no other loader about. */
	ms = state(mountp);
	owned = mutex_owned(&ms->namespace_lock);
	if (owned) {
		error = load_inode_locked(mountp, number, result);
	} else {
		/* Any other caller shares the namespace and is admitted as the one loader. */
		namespace_share(ms);
		error = load_inode_admitted(mountp, number, result);
		namespace_unshare(ms);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reads the directory record that follows a cursor. */
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
	size_t wanted;
	struct ufs_mount_state *ms;
	uint8_t record[8U + NAME_MAX];
	int error;

	ms = state(directory->i_mount);
	error = 0;

	/*
	 * Refuses namespace bytes after unresolved journal I/O invalidated
	 * cache state.
	 */
	if (ms->journal_enabled) {
		mutex_lock(&ms->journal_lock);

		/*
		 * A poisoned journal can no longer be trusted to serve a read.
		 */
		if (ms->journal.poisoned)
			error = EIO;
		mutex_unlock(&ms->journal_lock);
	}

	if (error != 0)
		return error;

	/* Walks the entries of the directory from the cursor. */
	while (*cursor < directory->i_size) {
		/*
		 * A record header that would straddle a block means it is
		 * corrupt.
		 */
		if ((uint64_t)*cursor % UFS_DIRBLKSIZ > UFS_DIRBLKSIZ - 8U)
			return EIO;

		/*
		 * Reads the record header and as much of a name as the rest
		 * of the directory block can hold, in one read.
		 */
		wanted = UFS_DIRBLKSIZ - (size_t)((uint64_t)*cursor % UFS_DIRBLKSIZ);
		if (wanted > sizeof(record))
			wanted = sizeof(record);
		count = pread_inode(directory, record, wanted, *cursor);
		if (count < 8)
			return EIO;
		*number = drv_ufs_get32(record, 0, ms->super.swapped);
		reclen = drv_ufs_get16(record, 4, ms->super.swapped);
		*type = record[6];

		/* The name length the header declares. */
		namelen = record[7];

		/* A record shorter than its own header cannot be one. */
		if (reclen < 8U)
			return EIO;	/* Failed. */

		/* Every record length is a multiple of four bytes. */
		if ((reclen & 3U) != 0)
			return EIO;	/* Failed. */

		/* The name it declares has to fit inside it. */
		if (8U + namelen > reclen)
			return EIO;	/* Failed. */

		/* A record may not straddle two directory blocks. */
		if ((uint64_t)*cursor % UFS_DIRBLKSIZ + reclen > UFS_DIRBLKSIZ)
			return EIO;	/* Failed. */

		/* Nor run past the end of the directory itself. */
		if ((uint64_t)reclen >
		    (uint64_t)directory->i_size - (uint64_t)*cursor)
			return EIO;	/* Failed. */

		/* A name the record promised must be there in full. */
		if ((size_t)count < 8U + namelen)
			return EIO;	/* Failed. */

		/* Takes the name that follows the header. */
		kern_memcpy(name, record + 8, namelen);
		name[namelen] = '\0';
		*cursor += reclen;

		/* Stops at the first record that names an inode. */
		if (*number != 0)
			return 0;
	}

	/* Failed. */
	return ENOENT;
}

/* Reports the smallest record that could hold a name that long. */
static uint16_t
dir_minimum(
	uint8_t length)
{
	/* A record is the header, the name, and padding to four bytes. */
	return (uint16_t)((8U + length + 3U) & ~3U);
}

/* Renders a file kind as the type byte a record stores. */
static uint8_t
dir_type(
	enum inode_type type)
{
	uint8_t encoded;

	/* The type field of a directory record has its own encoding. */
	switch (type) {
	case INODE_FIFO:
		encoded = 1U;
		break;
	case INODE_DIR:
		encoded = 4U;
		break;
	case INODE_REG:
		encoded = 8U;
		break;
	case INODE_SYMLINK:
		encoded = 10U;
		break;
	case INODE_SOCKET:
		encoded = 12U;
		break;
	default:
		/* A kind the record encoding has no number for. */
		encoded = 0U;
		break;
	}

	/* The encoded type. */
	return encoded;
}

/* Puts a directory block back the way a failed change found it. */
static int
restore_directory_block(
	struct inode *directory,
	uint64_t fragment,
	const uint8_t *original,
	int original_error)
{
	int rollback;

	rollback = write_block(directory->i_mount, fragment, original);

	/* A failed restore leaves the volume unwritable. */
	if (rollback != 0) {
		stop_writing(directory->i_mount, "a directory block could not be restored", rollback);

		/* Failed. */
		return rollback;
	}

	/* Reports the failure that made the restore necessary. */
	return original_error;
}

/*
 * Checks that a directory's recorded size is one its direct blocks can hold.
 *
 * A directory grows by whole record chunks within the direct blocks; the
 * indirect blocks are read like any file's but never edited here.
 */
static int
dir_check_size(
	struct inode *directory)
{
	struct ufs_mount_state *ms;
	uint64_t limit;

	/* Takes the mount state this call runs against. */
	ms = state(directory->i_mount);
	limit = (uint64_t)UFS_NDADDR * ms->super.bsize;

	/* Refuses a size no run of whole record chunks could have. */
	if (directory->i_size < 0)
		return EIO;
	if ((uint64_t)directory->i_size % UFS_DIRBLKSIZ != 0)
		return EIO;

	/* Refuses a directory that reaches past its direct blocks. */
	if ((uint64_t)directory->i_size > limit)
		return EIO;

	/* The size is one the directory can be edited at. */
	return 0;
}

/* Reports how many blocks a directory's recorded size covers. */
static uint32_t
dir_block_count(
	struct inode *directory)
{
	struct ufs_mount_state *ms;
	uint64_t size;

	/* Rounds the size up to whole blocks. */
	ms = state(directory->i_mount);
	size = (uint64_t)directory->i_size;

	/* Reports the whole blocks the size reaches into. */
	return (uint32_t)((size + ms->super.bsize - 1U) / ms->super.bsize);
}

/* Reports how many bytes of one directory block hold records. */
static uint32_t
dir_block_length(
	struct inode *directory,
	uint32_t index)
{
	struct ufs_mount_state *ms;
	uint64_t start;
	uint64_t remaining;

	/* Measures what is left of the directory from the block's start. */
	ms = state(directory->i_mount);
	start = (uint64_t)index * ms->super.bsize;
	remaining = (uint64_t)directory->i_size - start;

	/* A block before the last is full. */
	if (remaining > ms->super.bsize)
		return ms->super.bsize;

	/* The last block holds what remains. */
	return (uint32_t)remaining;
}

/*
 * Checks the record at one position of a directory block.
 *
 * The record must fit its header and name, keep to a multiple of four bytes,
 * stay within its record chunk and end before the block's records do.
 */
static int
dir_check_record(
	const struct ufs_mount_state *ms,
	const uint8_t *block,
	uint32_t length,
	uint32_t pos)
{
	uint16_t reclen;
	uint8_t nlen;

	/* A header that would straddle a chunk or the end is corrupt. */
	if (length - pos < 8U)
		return EIO;
	if (pos % UFS_DIRBLKSIZ > UFS_DIRBLKSIZ - 8U)
		return EIO;

	/* The record length and the name length the header declares. */
	reclen = drv_ufs_get16(block, pos + 4U, ms->super.swapped);
	nlen = block[pos + 7U];

	/* A record shorter than its own header cannot be one. */
	if (reclen < 8U)
		return EIO;

	/* Every record length is a multiple of four bytes. */
	if ((reclen & 3U) != 0)
		return EIO;

	/* A record may not straddle two record chunks. */
	if (pos % UFS_DIRBLKSIZ + reclen > UFS_DIRBLKSIZ)
		return EIO;

	/* Nor run past the records of the block. */
	if (reclen > length - pos)
		return EIO;

	/* And the name it declares has to fit inside it. */
	if (8U + nlen > reclen)
		return EIO;

	/* The record is well formed. */
	return 0;
}

/* Looks through one directory block for the record of a name. */
static int
dir_find_in_block(
	const struct ufs_mount_state *ms,
	const uint8_t *block,
	uint32_t length,
	const struct componentname *name,
	uint32_t *offset,
	uint32_t *previous,
	uint32_t *number)
{
	uint32_t ino;
	uint32_t pos;
	uint32_t prev;
	uint16_t reclen;
	uint8_t nlen;
	int difference;
	int error;

	/* Starts the walk at the first record, with nothing before it. */
	pos = 0;
	prev = UINT32_MAX;

	/* Walks the records of the block looking for the name. */
	while (pos < length) {
		/* Refuses a record that is not well formed. */
		error = dir_check_record(ms, block, length, pos);
		if (error != 0)
			return error;

		/* The inode number, the record length and the name length. */
		ino = drv_ufs_get32(block, pos, ms->super.swapped);
		reclen = drv_ufs_get16(block, pos + 4U, ms->super.swapped);
		nlen = block[pos + 7U];

		/* Compares the record name only when it could possibly match. */
		difference = 1;
		if (ino != 0 && nlen == name->cn_namelen)
			difference = kern_memcmp(block + pos + 8U, name->cn_nameptr, nlen);

		/* The record matches on its inode number and its name together. */
		if (difference == 0) {
			*offset = pos;
			*previous = prev;
			*number = ino;

			/* Succeeded. */
			return 0;
		}

		/* Moves on to the next record. */
		prev = pos;
		pos += reclen;
	}

	/* The name is not in this block. */
	return ENOENT;
}

/*
 * Looks through the blocks of a directory for the record of one name.
 *
 * The block holding the record is left in the buffer; its index among the
 * directory's blocks is reported with the record's offset inside it and the
 * offset of the record before it in the same block.
 */
static int
dir_find_record(
	struct inode *directory,
	const struct componentname *name,
	uint8_t *block,
	uint32_t *index,
	uint32_t *offset,
	uint32_t *previous,
	uint32_t *number)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	uint32_t count;
	uint32_t current;
	uint32_t length;
	int error;

	/* Takes the mount state and the private inode. */
	ms = state(directory->i_mount);
	ui = info(directory);

	/* Refuses a directory whose size or first block is not usable. */
	error = dir_check_size(directory);
	if (error != 0)
		return error;
	if (ui->direct[0] == 0)
		return EIO;

	/* Searches the blocks in order. */
	count = dir_block_count(directory);
	for (current = 0; current < count; current++) {
		/* A block inside the size must have been allocated. */
		if (ui->direct[current] == 0)
			return EIO;

		/* Reads the block. */
		error = read_block(directory->i_mount, ui->direct[current], block);
		if (error != 0)
			return error;

		/* Looks for the name among the block's records. */
		length = dir_block_length(directory, current);
		error = dir_find_in_block(ms, block, length, name, offset, previous, number);
		if (error == 0) {
			*index = current;

			/* Succeeded. */
			return 0;
		}

		/* Anything but a miss ends the search. */
		if (error != ENOENT)
			return error;
	}

	/* No block holds the name. */
	return ENOENT;
}

/*
 * Looks through one directory block for a record with room after its own
 * name for a new record of a given size.
 */
static int
dir_find_slack(
	const struct ufs_mount_state *ms,
	const uint8_t *block,
	uint32_t length,
	uint16_t need,
	uint32_t *offset)
{
	uint32_t pos;
	uint16_t reclen;
	uint16_t minimum;
	uint8_t nlen;
	int error;

	/* Walks the records of the block looking for room. */
	pos = 0;
	while (pos < length) {
		/* Refuses a record that is not well formed. */
		error = dir_check_record(ms, block, length, pos);
		if (error != 0)
			return error;

		/* Measures what the record needs and what it has. */
		reclen = drv_ufs_get16(block, pos + 4U, ms->super.swapped);
		nlen = block[pos + 7U];
		minimum = dir_minimum(nlen);

		/* A record with enough slack after its name can be split. */
		if (reclen - minimum >= need) {
			*offset = pos;

			/* Succeeded. */
			return 0;
		}

		/* Moves on to the next record. */
		pos += reclen;
	}

	/* The block has no room for the record. */
	return ENOENT;
}

/* Writes one record into a directory block at an offset. */
static void
dir_put_record(
	const struct ufs_mount_state *ms,
	uint8_t *block,
	uint32_t at,
	uint32_t number,
	uint16_t reclen,
	uint8_t type,
	const struct componentname *name)
{
	/* The header, then the name. */
	drv_ufs_put32(block, at, number, ms->super.swapped);
	drv_ufs_put16(block, at + 4U, reclen, ms->super.swapped);
	block[at + 6U] = type;
	block[at + 7U] = (uint8_t)name->cn_namelen;
	kern_memcpy(block + at + 8U, name->cn_nameptr, name->cn_namelen);
}

/*
 * Adds one name to a directory.
 *
 * The name goes into the first record with room after it, in any block.
 * With no such record, a new record chunk is added after the last one, in
 * the last block when it has room, or else in a new block.  Under the journal
 * only the first block can be added here; the others wait for its group.
 */
static int
dir_add(
	struct inode *directory,
	const struct componentname *name,
	uint32_t number,
	uint8_t type)
{
	struct ufs_mount_state *ms;
	struct dir_addition change;
	uint8_t *block;
	uint8_t *original;
	uint32_t pos;
	int error;

	/* A name of nothing, or one longer than a record can hold. */
	if (name->cn_namelen == 0 || name->cn_namelen > 255U)
		return EINVAL;

	/* Rejects a component that holds a byte no name may contain. */
	for (pos = 0; pos < name->cn_namelen; pos++) {
		/* A separator is the one byte a component may not hold. */
		if (name->cn_nameptr[pos] == '/')
			return EINVAL;
	}

	/* Takes the staging the directory block is edited in. */
	ms = state(directory->i_mount);
	block = kern_calloc(1, ms->super.bsize);
	original = kern_malloc(ms->super.bsize);
	if (block == NULL || original == NULL) {
		kern_free(block);
		kern_free(original);

		/* Failed. */
		return ENOMEM;
	}

	/* Places the record and publishes it, or undoes it, under the lock. */
	mutex_lock(&directory->i_lock);

	/* Notes the directory as it is, then places and publishes the record. */
	kern_memset(&change, 0, sizeof(change));
	change.old_size = directory->i_size;
	change.old_blocks = info(directory)->blocks;
	error = dir_check_size(directory);
	if (error == 0)
		error = dir_add_place(directory, name, number, type, block, original, &change);
	error = dir_add_finish(directory, &change, original, error);

	/* The directory's records, size and blocks are settled. */
	mutex_unlock(&directory->i_lock);

	/* Gives the staging back. */
	kern_free(original);
	kern_free(block);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Places a new record in a directory and writes the block it goes into.
 *
 * What it changes is noted in the addition, so that a failure of the write
 * or of the inode's publication can be undone.
 */
static int
dir_add_place(
	struct inode *directory,
	const struct componentname *name,
	uint32_t number,
	uint8_t type,
	uint8_t *block,
	uint8_t *original,
	struct dir_addition *change)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	uint32_t count;
	uint32_t current;
	uint32_t length;
	uint32_t pos;
	uint32_t at;
	uint16_t need;
	uint16_t reclen;
	uint16_t minimum;
	int found;
	int handled;
	int error;

	/* Takes the mount state, the private inode, and the room needed. */
	ms = state(directory->i_mount);
	ui = info(directory);
	need = dir_minimum((uint8_t)name->cn_namelen);
	found = 0;
	pos = 0;

	/* Looks for a record with room after it, block by block. */
	count = dir_block_count(directory);
	for (current = 0; current < count; current++) {
		/* A block inside the size must have been allocated. */
		change->fragment = ui->direct[current];
		if (change->fragment == 0)
			return EIO;

		/* Reads the block. */
		error = read_block(directory->i_mount, change->fragment, block);
		if (error != 0)
			return error;

		/* Looks for room among the block's records. */
		length = dir_block_length(directory, current);
		error = dir_find_slack(ms, block, length, need, &pos);
		if (error == 0) {
			found = 1;
			change->edited = current;
			break;
		}

		/* Anything but a block without room ends the search. */
		if (error != ENOENT)
			return error;
	}

	/* Splits the record that has room, giving the new name its slack. */
	if (found) {
		kern_memcpy(original, block, ms->super.bsize);
		change->old_direct = ui->direct[change->edited];
		reclen = drv_ufs_get16(block, pos + 4U, ms->super.swapped);
		minimum = dir_minimum(block[pos + 7U]);
		at = pos + minimum;
		drv_ufs_put16(block, pos + 4U, minimum, ms->super.swapped);
		dir_put_record(ms, block, at, number, reclen - minimum, type, name);
		change->written = 1;

		/* Reports how the write went. */
		return write_block(directory->i_mount, change->fragment, block);
	}

	/* A last block with room takes a new record chunk after its last. */
	if ((uint64_t)directory->i_size % ms->super.bsize != 0) {
		change->edited = count - 1U;
		change->old_direct = ui->direct[change->edited];
		kern_memcpy(original, block, ms->super.bsize);
		pos = (uint32_t)((uint64_t)directory->i_size % ms->super.bsize);
		dir_put_record(ms, block, pos, number, UFS_DIRBLKSIZ, type, name);
		directory->i_size += UFS_DIRBLKSIZ;
		change->written = 1;

		/* Reports how the write went. */
		return write_block(directory->i_mount, change->fragment, block);
	}

	/* Otherwise the record chunk starts a new block, if there can be one. */
	change->edited = count;
	if (count >= UFS_NDADDR)
		return ENOSPC;

	/* Notes the pointer the new block's slot holds now. */
	change->old_direct = ui->direct[count];

	/* The journal path may add the block itself, in a group of its own. */
	if (ui->direct[count] == 0) {
		error = directory_backing_group(directory, &handled);
		if (handled) {
			/* Reports why the block could not be added. */
			if (error != 0)
				return error;

			/* Keeps the committed empty block if the entry fails. */
			change->old_direct = ui->direct[count];
			change->old_blocks = ui->blocks;
		}
	}

	/* Takes a new block when there is none. */
	if (ui->direct[count] == 0) {
		error = allocate_block(directory->i_mount, directory->i_uid, directory->i_gid, &ui->direct[count]);
		if (error != 0)
			return error;

		/* Counts the block as the directory's. */
		change->allocated = ui->direct[count];
		ui->blocks += ms->super.bsize / UFS_SECTOR_SIZE;
	}

	/* Writes the block with the one record chunk the name starts. */
	change->fresh = 1;
	change->fragment = ui->direct[count];
	kern_memset(block, 0, ms->super.bsize);
	dir_put_record(ms, block, 0, number, UFS_DIRBLKSIZ, type, name);
	directory->i_size += UFS_DIRBLKSIZ;
	change->written = 1;

	/* Reports how the write went. */
	return write_block(directory->i_mount, change->fragment, block);
}

/*
 * Publishes a directory after a new record was placed, or undoes the
 * placement when it or the publication failed.
 *
 * Undoing puts back the block's records, then the inode's size and block,
 * and gives back a block taken for the record once the old size is on disk.
 * A volume that cannot be put back is left unwritable.
 */
static int
dir_add_finish(
	struct inode *directory,
	const struct dir_addition *change,
	const uint8_t *original,
	int error)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	int rollback;

	/* A placement that wrote nothing changed nothing. */
	if (!change->written)
		return error;

	/* Publishes the directory with its new size. */
	ms = state(directory->i_mount);
	ui = info(directory);
	if (error == 0)
		error = persist_inode(directory);
	if (error == 0)
		return 0;

	/* Puts the block's records back unless the block is new. */
	rollback = error;
	if (!change->fresh)
		rollback = restore_directory_block(directory, change->fragment, original, error);

	/* Puts the inode back and publishes it. */
	directory->i_size = change->old_size;
	ui->direct[change->edited] = change->old_direct;
	ui->blocks = change->old_blocks;
	error = persist_inode(directory);
	if (error == 0)
		error = order_barrier(directory->i_mount);
	if (error != 0) {
		stop_writing(directory->i_mount, "a directory addition could not be undone", error);

		/* Reports why the volume could not be put back. */
		return error;
	}

	/* Gives a new block back once the old size is published. */
	if (ms->writable && change->allocated != 0) {
		error = free_block(directory->i_mount, change->allocated, directory->i_uid, directory->i_gid);
		if (error != 0) {
			stop_writing(directory->i_mount, "the block of an undone directory addition could not be freed", error);

			/* Reports why the block could not be given back. */
			return error;
		}
	}

	/* Reports why the change was undone. */
	return rollback;
}

/* Takes one name out of a directory. */
static int
dir_remove(
	struct inode *directory,
	const struct componentname *name,
	uint32_t *number)
{
	struct ufs_mount_state *ms;
	uint8_t *block;
	uint8_t *original;
	uint64_t fragment;
	uint32_t index;
	uint32_t offset;
	uint32_t previous;
	uint16_t reclen;
	uint16_t prior;
	int error;

	/* Takes the staging the directory block is edited in. */
	ms = state(directory->i_mount);
	block = kern_malloc(ms->super.bsize);
	original = kern_malloc(ms->super.bsize);

	/* Gives up before touching the volume when there is no staging. */
	if (block == NULL || original == NULL) {
		kern_free(block);
		kern_free(original);

		/* Failed. */
		return ENOMEM;
	}

	/* Edits the record under the directory's lock. */
	mutex_lock(&directory->i_lock);

	/* Finds the record the name occupies, and the one before it. */
	error = dir_find_record(directory, name, block, &index, &offset, &previous, number);
	if (error == 0) {
		kern_memcpy(original, block, ms->super.bsize);
		fragment = info(directory)->direct[index];
		reclen = drv_ufs_get16(block, offset + 4U, ms->super.swapped);

		/* The preceding record in the same chunk absorbs the one removed. */
		if (previous != UINT32_MAX && previous / UFS_DIRBLKSIZ == offset / UFS_DIRBLKSIZ) {
			prior = drv_ufs_get16(block, previous + 4U, ms->super.swapped);
			drv_ufs_put16(block, previous + 4U, prior + reclen, ms->super.swapped);
		} else {
			drv_ufs_put32(block, offset, 0, ms->super.swapped);
		}

		/* Writes the block back with the record gone. */
		error = write_block(directory->i_mount, fragment, block);
		if (error != 0)
			error = restore_directory_block(directory, fragment, original, error);
	}

	/* The block holds the edited record, or its old one again. */
	mutex_unlock(&directory->i_lock);

	/* Gives the staging back. */
	kern_free(original);
	kern_free(block);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Points one name at another inode. */
static int
dir_replace(
	struct inode *directory,
	const struct componentname *name,
	uint32_t number,
	uint8_t type,
	uint32_t *old_number,
	uint8_t *old_type)
{
	struct ufs_mount_state *ms;
	uint8_t *block;
	uint8_t *original;
	uint64_t fragment;
	uint32_t index;
	uint32_t offset;
	uint32_t previous;
	int error;

	/* Takes the staging the directory block is edited in. */
	ms = state(directory->i_mount);
	block = kern_malloc(ms->super.bsize);
	original = kern_malloc(ms->super.bsize);

	/* Gives up before touching the volume when there is no staging. */
	if (block == NULL || original == NULL) {
		kern_free(block);
		kern_free(original);

		/* Failed. */
		return ENOMEM;
	}

	/* Edits the record under the directory's lock. */
	mutex_lock(&directory->i_lock);

	/* Finds the record the name occupies. */
	error = dir_find_record(directory, name, block, &index, &offset, &previous, old_number);
	if (error == 0) {
		kern_memcpy(original, block, ms->super.bsize);
		fragment = info(directory)->direct[index];
		*old_type = block[offset + 6U];

		/* Points the record at the new inode. */
		drv_ufs_put32(block, offset, number, ms->super.swapped);
		block[offset + 6U] = type;

		/* Writes the block back with the record pointing elsewhere. */
		error = write_block(directory->i_mount, fragment, block);
		if (error != 0)
			error = restore_directory_block(directory, fragment, original, error);
	}

	/* The block holds the edited record, or its old one again. */
	mutex_unlock(&directory->i_lock);

	/* Gives the staging back. */
	kern_free(original);
	kern_free(block);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Tests whether a name is the directory itself or its parent. */
static int
name_is_dot(
	const struct componentname *name)
{
	/* The two names every directory holds for itself and its parent. */
	return (name->cn_namelen == 1U && name->cn_nameptr[0] == '.') ||
		(name->cn_namelen == 2U && name->cn_nameptr[0] == '.' &&
		 name->cn_nameptr[1] == '.');
}

/* Takes the socket a half-made inode still holds. */
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

	/* A socket keeps no storage of its own once it is detached. */
	if (inode->i_type == INODE_SOCKET) {
		inode->i_special = NULL;
		inode->i_special_destroy = NULL;
	}

	mutex_unlock(&inode->i_lock);
}

/* Undoes a creation that could not be finished. */
static int
discard_new_inode(
	struct inode *inode,
	int directory_counted)
{
	int discarded;
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
	uint32_t group_sectors;
	int grouped;
	int error = 0, cleanup;

	/* How many sectors undoing the creation as one group would take. */
	group_sectors = (2U * ms->super.bsize + UFS_SBLOCK_SIZE) /
		UFS_SECTOR_SIZE;

	/* Whether the journal could carry a group of that size at all. */
	grouped = 0;
	if (ms->journal_enabled && ms->journal.sector_count > 2U &&
	    group_sectors <= UFS_JOURNAL_GROUP_SECTORS &&
	    group_sectors <= ms->journal.sector_count - 2U)
		grouped = 1;

	/* A directory is undone as a group only once its link is counted. */
	if (inode->i_type == INODE_DIR && !directory_counted)
		grouped = 0;

	/* A journalled volume undoes the whole creation as one group. */
	if (grouped) {
		/* Gives the reserved inode back. */
		discarded = discard_reserved_inode(inode);

		/* Reports whether the reservation could be given back. */
		return discarded;
	}

	detach_new_socket_special(inode);
	/* Takes the direct blocks the creation had already allocated. */
	for (n = 1; n < UFS_NDADDR; n++) {
		/* A direct pointer that was filled has a block to free. */
		if (ui->direct[n] != 0) {
			stop_writing(inode->i_mount, "a new inode with direct blocks could not be discarded", 0);
			inode_release(inode);

			/* Failed. */
			return EIO;
		}
	}

	/* Takes the indirect roots the creation had already allocated. */
	for (n = 0; n < UFS_NIADDR; n++) {
		/* An indirect pointer that was filled has a block to free. */
		if (ui->indirect[n] != 0) {
			stop_writing(inode->i_mount, "a new inode with indirect blocks could not be discarded", 0);
			inode_release(inode);

			/* Failed. */
			return EIO;
		}
	}

	/* Takes the attribute blocks the creation had already allocated. */
	for (n = 0; n < UFS_NXADDR; n++) {
		extattr[n] = ui->extattr[n];
		ui->extattr[n] = 0;
	}

	/* Empties the inode so nothing it held is pointed at any more. */
	inode->i_mode = 0;
	inode->i_type = INODE_NONE;
	inode->i_linkcount = 0;
	inode->i_size = 0;
	ui->direct[0] = 0;
	ui->extattr_size = 0;
	ui->blocks = 0;

	/* Publishes the emptied inode before any block is freed. */
	cleanup = persist_inode(inode);
	if (cleanup == 0)
		cleanup = order_barrier(inode->i_mount);

	/* A failed write leaves the volume unwritable. */
	if (cleanup != 0) {
		inode->i_mode = old_mode;
		inode->i_type = old_type;
		inode->i_linkcount = old_links;
		inode->i_size = old_size;
		ui->direct[0] = block;
		ui->extattr_size = old_extattr_size;
		/* Frees the attribute blocks the inode has given up. */
		for (n = 0; n < UFS_NXADDR; n++)
			ui->extattr[n] = extattr[n];
		ui->blocks = old_blocks;
		stop_writing(inode->i_mount, "a new inode could not be cleared", cleanup);
		inode_release(inode);

		/* Failed. */
		return cleanup;
	}

	/* A directory that was counted is taken back off the count. */
	if (directory_counted) {
		/* Takes the new directory back off the directory count. */
		cleanup = adjust_directory_count(inode->i_mount, number, -1);
		if (error == 0 && cleanup != 0)
			error = cleanup;
	}

	/* Frees the block the creation had allocated. */
	if (block != 0) {
		/* Frees the block the creation had allocated. */
		cleanup = free_block(inode->i_mount, block, uid, gid);
		if (error == 0 && cleanup != 0)
			error = cleanup;
	}

	/* Frees each attribute block the creation had allocated. */
	for (n = 0; n < UFS_NXADDR; n++) {
		/* An attribute pointer that was filled has a block to free. */
		if (extattr[n] != 0) {
			/* Frees one attribute block. */
			cleanup = free_block(inode->i_mount, extattr[n], uid,
					     gid);
			if (error == 0 && cleanup != 0)
				error = cleanup;
		}
	}

	/* Gives the inode number itself back. */
	cleanup = free_inode_number(inode->i_mount, number, uid, gid);
	if (error == 0 && cleanup != 0)
		error = cleanup;
	if (error != 0)
		stop_writing(inode->i_mount, "the storage of a new inode could not be given back", error);
	inode->i_ino = 0;
	inode->i_flags |= INODE_DEAD;
	inode_release(inode);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Undoes a creation and reports the error that caused it. */
static int
discard_new_inode_after_error(
	struct inode *inode,
	int directory_counted,
	int original_error)
{
	struct ufs_mount_state *ms;
	int cleanup;

	/* Takes the mount state this call runs against. */
	ms = state(inode->i_mount);

	/* A volume that is no longer writable cannot be unwound. */
	if (!ms->writable) {
		detach_new_socket_special(inode);
		inode_release(inode);

		/* Reports the failure that made the unwind necessary. */
		return original_error;
	}

	cleanup = discard_new_inode(inode, directory_counted);

	/*
	 * A failed cleanup is the more serious of the two, because it leaves
	 * the volume in a state the original error did not.
	 */
	if (cleanup != 0)
		return cleanup;	/* Failed. */

	return original_error;	/* Failed. */
}

/*
 * Reserves a number with initialized zero-link identity and directory
 * accounting.
 */
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
	int used;
	int found;
	int error;

	/*
	 * Validates the requested kind before selecting an unowned inode slot.
	 */

	/* Takes the mount the reservation runs against. */
	ms = state(inode->i_mount);
	if (!ms->writable)
		return EROFS;

	/* Renders the requested type as the mode bits the format stores. */
	kind = inode_type_mode(request->type);
	if (kind == 0 || inode->i_ino != 0)
		return EINVAL;

	/* Finds a free number while excluding all allocation-map mutations. */
	found = 0;
	cg = local = 0;
	is_directory = request->type == INODE_DIR;
	for (attempt = 0; attempt < ms->super.ncg; attempt++) {
		cg = (ms->rotor_cg + attempt) % ms->super.ncg;

		/* Reads the cylinder group the search starts in. */
		error = load_cg_locked(inode->i_mount, cg);
		if (error != 0)
			return error;
		local = cg == 0 ? UFS_ROOT_INO + 1U : 0U;
		/* Walks the group looking for an inode number that is free. */
		for (; local < ms->super.ipg; local++) {
			/* Asks the used map whether this number is taken. */
			used = bit_test(ms->cg + ms->cg_iusedoff, local);

			/* A clear bit in the used map is a free number. */
			if (!used) {
				found = 1;
				break;
			}
		}

		/* The group holds a number this reservation can take. */
		if (found)
			break;
	}

	/* No group holds a free inode number. */
	if (!found)
		return ENOSPC;
	free_inodes = drv_ufs_get32(ms->cg, UFS_CG_NIFREE, ms->super.swapped);

	/* Reads the counts this reservation will adjust. */
	directories = drv_ufs_get32(ms->cg, UFS_CG_NDIR, ms->super.swapped);

	/* A group that calls no inode free has none to hand out. */
	if (free_inodes == 0 || ms->super.cstotal_nifree == 0)
		return EIO;	/* Failed. */

	/* Nor may a directory count be raised past what it can hold. */
	if (is_directory && (directories == UINT32_MAX ||
			     ms->super.cstotal_ndir == UINT64_MAX))
		return EIO;	/* Failed. */

	/*
	 * Initializes all persistent ownership fields before making the slot
	 * allocated.
	 */
	image = &group->images.image;
	kern_memcpy(image, info(inode), sizeof(*image));
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
	kern_memset(image->direct, 0, sizeof(image->direct));
	kern_memset(image->indirect, 0, sizeof(image->indirect));
	kern_memset(image->extattr, 0, sizeof(image->extattr));
	kern_memset(image->shortlink, 0, sizeof(image->shortlink));
	fragment = inode_fragment(&image->inode);

	/* Reads the block the chosen inode lives in. */
	error = read_block(inode->i_mount, fragment, group->images.dinode);
	if (error != 0)
		return error;
	raw = group->images.dinode +
		(local % ms->super.inopb) * UFS_DINODE_SIZE;

	/* A reused inode number gets a new generation, so old handles fail. */
	generation = drv_ufs_get32(raw, UFS_DI_GEN, ms->super.swapped) + 1U;
	if (generation == 0)
		generation = 1;
	kern_memset(raw, 0, UFS_DINODE_SIZE);
	image->generation = generation;
	encode_inode_locked(&image->inode, group->images.dinode);
	drv_ufs_put32(raw, UFS_DI_GEN, generation, ms->super.swapped);

	/* Stages the superblock summaries the reservation changes. */
	error = prepare_super_summaries(inode->i_mount,
					group->images.summaries);
	if (error != 0)
		return error;

	/*
	 * Couples the new identity with inode and directory allocation totals.
	 */
	kern_memcpy(group->images.cg, ms->cg, ms->super.bsize);
	bit_set(group->images.cg + ms->cg_iusedoff, local);
	drv_ufs_put32(group->images.cg, UFS_CG_NIFREE, free_inodes - 1U,
		      ms->super.swapped);
	drv_ufs_put64(group->images.summaries, UFS_FS_CSTOTAL_NIFREE,
		      ms->super.cstotal_nifree - 1U, ms->super.swapped);

	/* A directory also raises the count its group keeps. */
	if (is_directory) {
		drv_ufs_put32(group->images.cg, UFS_CG_NDIR, directories + 1U,
			      ms->super.swapped);
		drv_ufs_put64(group->images.summaries, UFS_FS_CSTOTAL_NDIR,
			      ms->super.cstotal_ndir + 1U, ms->super.swapped);
	}

	/* Names the group counts, the summaries and the inode block. */
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
	 * reference state.
	 */
	if (group->outcome.committed) {
		kern_memcpy(ms->cg, group->images.cg, ms->super.bsize);
		ms->super.cstotal_nifree--;

		/* Publishes the directory count the group committed. */
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

	/* An uncertain group leaves the volume unwritable. */
	if (group->outcome.committed || group->outcome.uncertain)
		ms->cg_dirty = group->outcome.uncertain;

	/*
	 * Preserves original errors separately from durable identity ownership.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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
	 * mount.
	 */
	ms = state(inode->i_mount);
	bytes = 2U * ms->super.bsize + UFS_SBLOCK_SIZE;

	/* Takes the staging the whole reservation is assembled in. */
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;
	group->images.memory = kern_malloc(bytes);

	/* Gives up before touching the volume when there is no staging. */
	if (group->images.memory == NULL) {
		kern_free(group);

		/* Failed. */
		return ENOMEM;
	}

	group->images.cg = group->images.memory;
	group->images.dinode = group->images.cg + ms->super.bsize;
	group->images.summaries = group->images.dinode + ms->super.bsize;

	/* Charges the inode against the owner quota before taking it. */
	error = quota_reserve(&ms->quota, request->uid, request->gid, 0, 1,
			      quota_now(), &group->charge);
	if (error == 0) {
		mutex_lock(&inode->i_lock);
		mutex_lock(&ms->lock);
		error = reserve_inode_locked(inode, request, group);

		/*
		 * Holds quota for every possibly committed reservation until
		 * recovery.
		 */
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
	 * owner.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static int
new_inode(
	struct inode *directory,
	const struct inode_creation_request *request,
	nlink_t links,
	struct inode **result)
{
	int discarded;
	struct mount *mountp;
	struct inode *inode;
	uint32_t number = 0;
	int error;
	int grouped = 0;
	int directory_counted = 0;
	size_t reservation_bytes;
	struct ufs_mount_state *ms;

	/* Rejects a call that leaves the creation underspecified. */
	if (directory == NULL || request == NULL || result == NULL)
		return EINVAL;
	*result = NULL;
	mountp = directory->i_mount;
	ms = state(mountp);

	/*
	 * Admit the complete preparation chain, including first directory
	 * backing.
	 */
	reservation_bytes = 3U * ms->super.bsize + UFS_SBLOCK_SIZE;

	/* A journalled volume publishes the whole creation as one group. */
	grouped = ms->journal_enabled && ms->journal.sector_count > 2U &&
		reservation_bytes / UFS_SECTOR_SIZE <=
		UFS_JOURNAL_GROUP_SECTORS &&
		reservation_bytes / UFS_SECTOR_SIZE <=
		ms->journal.sector_count - 2U;
	if (grouped) {
		/* Takes the in-core inode the new file will be described by. */
		inode = inode_alloc(mountp);
		if (inode == NULL)
			return ENOSPC;
		inode->i_op = &ufs_inode_ops;

		/* Reserves the inode number and its blocks as one group. */
		error = reserve_inode_group(inode, request);
		if (error != 0) {
			inode->i_flags |= INODE_DEAD;
			inode_release(inode);

			/* Failed. */
			return error;
		}

		number = (uint32_t)inode->i_ino;
		directory_counted = request->type == INODE_DIR;
	} else {
		/* Takes an inode number the ordinary way. */
		error = allocate_inode_number(mountp, request->uid,
					      request->gid, &number);
		if (error)
			return error;

		/* Takes the in-core inode the new file will be described by. */
		inode = inode_alloc(mountp);
		if (inode == NULL) {
			/*
			 * Gives the number back when no inode could be taken.
			 */
			error = free_inode_number(mountp, number, request->uid,
						  request->gid);
			if (error != 0) {
				stop_writing(mountp, "an inode number could not be given back", error);

				/* Failed. */
				return error;
			}

			/* Failed. */
			return ENOSPC;
		}
	}

	inode->i_ino = number;
	inode->i_type = request->type;
	inode->i_linkcount = grouped ? 0 : links;
	inode->i_op = &ufs_inode_ops;

	/* Which file operations apply follows from the kind of file it is. */
	switch (request->type) {
	case INODE_DIR:
		inode->i_fop = &ufs_directory_ops;
		break;
	case INODE_REG:
		inode->i_fop = &ufs_regular_ops;
		break;
	case INODE_FIFO:
		inode->i_fop = &fifo_file_ops;
		break;
	default:
		/* A device or socket is served by the node it names. */
		inode->i_fop = NULL;
		break;
	}

	/* A grouped creation has already published everything below. */
	if (!grouped)
		info(inode)->generation = number;

	/* Lets the generic layer apply the request to the new inode. */
	error = inode_creation_prepare(directory, inode, request);
	if (error != 0) {
		/*
		 * Undoes the whole creation when the request could not be
		 * applied.
		 */
		discarded = discard_new_inode_after_error(
			inode, directory_counted, error);

		/*
		 * Reports why the creation could not be undone, or why it
		 * failed.
		 */
		return discarded;
	}

	/* Publishes the new inode before anything can name it. */
	error = persist_inode(inode);
	if (error) {
		/*
		 * Undoes the whole creation when the inode could not be
		 * written.
		 */
		discarded = discard_new_inode_after_error(
			inode, directory_counted, error);

		/*
		 * Reports why the creation could not be undone, or why it
		 * failed.
		 */
		return discarded;
	}

	/* A new directory is added to the count its group keeps. */
	if (request->type == INODE_DIR && !directory_counted) {
		/* Adds the new directory to the directory count. */
		error = adjust_directory_count(mountp, number, 1);
		if (error != 0) {
			/*
			 * Undoes the whole creation when the count could not be
			 * raised.
			 */
			discarded = discard_new_inode_after_error(
				inode, directory_counted, error);

			/*
			 * Reports why the creation could not be undone, or why
			 * it failed.
			 */
			return discarded;
		}
	}

	*result = inode;

	/* Succeeded. */
	return 0;
}

/* Loads an inode as the one admitted loader, with the namespace shared (ws073-p045). */
static int
load_inode_admitted(
	struct mount *mountp,
	uint32_t number,
	struct inode **result)
{
	struct ufs_mount_state *ms;
	int error;

	/* One shared loader at a time publishes into the in-core inode table. */
	ms = state(mountp);
	mutex_lock(&ms->load_lock);
	error = load_inode_locked(mountp, number, result);
	mutex_unlock(&ms->load_lock);

	/* Reports why the inode could not be read. */
	if (error != 0)
		return error;

	/* Succeeded: the inode is in core. */
	return 0;
}

/*
 * Holds the namespace alone for a change: takes the namespace lock, then
 * waits until no lookup or commit shares the namespace (ws073-p045).
 */
static void
namespace_enter(
	struct ufs_mount_state *ms)
{
	unsigned long irq;
	uint64_t sequence;

	/* The lock keeps new sharers out from here on. */
	mutex_lock(&ms->namespace_lock);

	/* Waits for the sharers already in to leave. */
	irq = spin_lock_irqsave(&ms->namespace_guard);
	while (ms->namespace_readers != 0U) {
		sequence = waitq_sequence(&ms->namespace_drained);
		(void)waitq_sleep(&ms->namespace_drained, &ms->namespace_guard, sequence, 0, 0);
	}
	spin_unlock_irqrestore(&ms->namespace_guard, irq);
}

/* Lets the namespace go after a change. */
static void
namespace_leave(
	struct ufs_mount_state *ms)
{
	/* Sharers and the next change may come in. */
	mutex_unlock(&ms->namespace_lock);
}

/*
 * Shares the namespace with other lookups and the journal's commit: waits
 * out a change that holds it alone, then counts this caller in.
 */
static void
namespace_share(
	struct ufs_mount_state *ms)
{
	unsigned long irq;

	/* A brief pass through the lock waits for a change under way. */
	mutex_lock(&ms->namespace_lock);

	/* The count a change waits on, which this sharer joins. */
	irq = spin_lock_irqsave(&ms->namespace_guard);
	ms->namespace_readers++;
	spin_unlock_irqrestore(&ms->namespace_guard, irq);
	mutex_unlock(&ms->namespace_lock);
}

/* Stops sharing the namespace; the last sharer out wakes a change that waits. */
static void
namespace_unshare(
	struct ufs_mount_state *ms)
{
	unsigned long irq;

	/* The count goes down, and a change waiting for zero is woken. */
	irq = spin_lock_irqsave(&ms->namespace_guard);
	ms->namespace_readers--;
	if (ms->namespace_readers == 0U)
		waitq_wake_all(&ms->namespace_drained);
	spin_unlock_irqrestore(&ms->namespace_guard, irq);
}

/* Resolves one name under a directory, with the namespace held alone. */
static int
ufs_lookup_locked(
	struct inode *directory,
	const struct componentname *component,
	struct inode **result)
{
	int error;

	/* The scan loads the inode it finds as the namespace's only user. */
	error = ufs_lookup_scan(directory, component, result, 0);

	/* Reports the inode, or why it could not be found or read. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Resolves one name under a directory: the namespace is held alone, or
 * shared (shared) by a lookup, which then loads the inode it finds as the
 * admitted loader (ws073-p045).
 */
static int
ufs_lookup_scan(
	struct inode *directory,
	const struct componentname *component,
	struct inode **result,
	int shared)
{
	int looked_up;
	off_t cursor = 0;
	size_t length;
	uint32_t number;
	uint8_t type;
	char name[NAME_MAX + 1U];
	int difference;
	int error;

	/* Walks the directory entry by entry. */
	for (;;) {
		/* Reads the next entry the directory holds. */
		error = next_dirent(directory, &cursor, &number, &type, name);
		if (error != 0)
			break;

		/*
		 * Measures the entry name and compares it with the wanted one.
		 */
		length = kern_strlen(name);
		difference = kern_memcmp(name, component->cn_nameptr,
				    component->cn_namelen);

		/*
		 * The entry matches on its name length and its bytes together.
		 */
		if (length == component->cn_namelen && difference == 0) {
			/* Reads the inode the entry names (a shared lookup as the admitted loader). */
			if (shared) {
				looked_up = load_inode_admitted(directory->i_mount, number, result);
			} else {
				looked_up = load_inode(directory->i_mount, number, result);
			}

			/* Reports the inode, or why it could not be read. */
			return looked_up;
		}
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Resolves one name under a directory. */
static int
ufs_lookup(
	struct inode *directory,
	const struct componentname *component,
	struct inode **result)
{
	struct ufs_mount_state *ms;
	int hidden;
	int owned;
	int error;

	/* The journal file is the volume's own and no one else's to name. */
	hidden = j3_hidden(directory, component);
	if (hidden)
		return EPERM;

	/*
	 * A caller that holds the namespace alone looks up inside its change;
	 * any other shares the namespace with other lookups and the journal's
	 * commit (ws073-p045, BUG-135: a commit's cache flush no longer holds
	 * up stat() and open()).
	 */
	ms = state(directory->i_mount);
	owned = mutex_owned(&ms->namespace_lock);
	if (owned) {
		error = ufs_lookup_locked(directory, component, result);
	} else {
		namespace_share(ms);
		error = ufs_lookup_scan(directory, component, result, 1);
		namespace_unshare(ms);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Prepares one namespace removal and its target link count under shared
 * ownership.
 */
static int
remove_group_locked(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target,
	struct ufs_remove_group *group)
{
	struct ufs_mount_state *ms;
	struct ufs_journal_extent extents[3];
	uint64_t fragment;
	uint64_t parent_fragment;
	unsigned count;
	int removing_directory;
	uint32_t index;
	uint32_t offset;
	uint32_t previous;
	uint32_t number;
	uint16_t length;
	uint16_t prior;
	int error;

	ms = state(directory->i_mount);
	count = 2;
	removing_directory = target->i_type == INODE_DIR;

	/*
	 * Validates the locked name-to-inode relation before editing private
	 * bytes.
	 */
	if (!ms->writable)
		return EROFS;

	/* Refuses an inode whose link count could not lose another link. */
	if (target->i_linkcount == 0 ||
	    (removing_directory && directory->i_linkcount == 0)) {
		/* Failed. */
		return EIO;
	}

	/* Finds the record the name occupies, and the one before it. */
	error = dir_find_record(directory, name, group->directory, &index,
				&offset, &previous, &number);
	if (error != 0)
		return error;

	/* The record does not name the inode this removal was given. */
	if (number != (uint32_t)target->i_ino)
		return EIO;
	length =
		drv_ufs_get16(group->directory, offset + 4U, ms->super.swapped);

	/* The preceding record absorbs the one being removed. */
	if (previous != UINT32_MAX &&
	    previous / UFS_DIRBLKSIZ == offset / UFS_DIRBLKSIZ) {
		prior = drv_ufs_get16(group->directory, previous + 4U,
				      ms->super.swapped);
		drv_ufs_put16(group->directory, previous + 4U, prior + length,
			      ms->super.swapped);
	} else {
		drv_ufs_put32(group->directory, offset, 0, ms->super.swapped);
	}

	kern_memcpy(&group->image, info(target), sizeof(group->image));

	/* A directory also takes its parent link away. */
	if (removing_directory)
		group->image.inode.i_linkcount = 0;
	else
		group->image.inode.i_linkcount--;

	/* Stages the inode with its new link count. */
	error = prepare_inode_locked(&group->image.inode, group->dinode,
				     &fragment);
	if (error != 0)
		return error;

	/*
	 * Merges parent accounting with the target when both share a dinode
	 * block.
	 */
	if (removing_directory) {
		kern_memcpy(&group->parent_image, info(directory),
		       sizeof(group->parent_image));
		group->parent_image.inode.i_linkcount--;

		/*
		 * The parent inode changes too, so its block is staged as well.
		 */
		parent_fragment = inode_fragment(directory);
		if (parent_fragment == fragment) {
			encode_inode_locked(&group->parent_image.inode,
					    group->dinode);
		} else {
			/* Stages the parent inode with its new link count. */
			error = prepare_inode_locked(&group->parent_image.inode,
						     group->parent_dinode,
						     &parent_fragment);
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
	 * transaction.
	 */
	extents[0].target = info(directory)->direct[index] << ms->super.fsbtodb;
	extents[0].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[0].payload = group->directory;
	extents[1].target = fragment << ms->super.fsbtodb;
	extents[1].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[1].payload = group->dinode;
	error = metadata_group_commit(directory->i_mount, extents, count, NULL,
				      &group->outcome);

	/* Publishes the inodes as the group committed them. */
	if (group->outcome.committed) {
		target->i_linkcount = group->image.inode.i_linkcount;

		/* A removed directory takes a link off its parent. */
		if (removing_directory) {
			directory->i_linkcount =
				group->parent_image.inode.i_linkcount;
		}

		/* An inode with no links left is retired. */
		if (target->i_linkcount == 0)
			target->i_flags |= INODE_DEAD;
	}

	/* Keeps the original errno even when recovery established removal. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Owns bounded private storage and error-path namespace cache publication. */
static int
remove_group(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target,
	int *handled)
{
	struct ufs_mount_state *ms;
	struct ufs_remove_group *group;
	uint64_t directory_fragment;
	uint64_t target_fragment;
	size_t bytes;
	int dot;
	int error;

	/* Takes the mount state this call runs against. */
	ms = state(directory->i_mount);

	/*
	 * Declines only before admission, independently of callback error
	 * values.
	 */
	*handled = 0;
	/* A volume without a journal removes the name one step at a time. */
	if (!ms->journal_enabled)
		return 0;

	/* Asks whether the name is the directory itself or its parent. */
	dot = name_is_dot(name);

	/* Refuses a removal whose parent and target are the same inode. */
	if (directory == target || dot) {
		*handled = 1;
		/* Failed. */
		return EINVAL;
	}

	/* Sizes the staging from the images the group will hold. */
	bytes = 2U * ms->super.bsize;

	/* Locates the shared blocks the two inodes are written in. */
	directory_fragment = inode_fragment(directory);
	target_fragment = inode_fragment(target);

	/* A directory in a block of its own needs one image more. */
	if (target->i_type == INODE_DIR &&
	    directory_fragment != target_fragment)
		bytes += ms->super.bsize;

	/* A group too wide for the journal cannot be carried by it. */
	if (bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
		/* Succeeded. */
		return 0;
	}
	*handled = 1;

	/* Takes the staging the whole removal is assembled in. */
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(bytes);

	/* Gives up before touching the volume when there is no staging. */
	if (group->memory == NULL) {
		kern_free(group);

		/* Failed. */
		return ENOMEM;
	}

	/* Carves the staging into the images this group will hold. */
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
	 * need this.
	 */
	if (error != 0 &&
	    (group->outcome.committed || group->outcome.uncertain)) {
		namecache_remove(directory, name);
		inode_dir_changed(directory);
	}

	kern_free(group->memory);
	kern_free(group);

	/*
	 * Reports the original group outcome after releasing all transient
	 * ownership.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Inserts a name into one block of a private directory image.
 *
 * The name takes the first free record long enough for it, or the slack after
 * a record's own name.  With no room in the block, a new record chunk is added
 * after the last one when this is the directory's last block and it has room.
 * The block's records are all checked first, and a name the block already
 * holds is refused.
 */
static int
directory_image_insert(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target,
	uint8_t *block,
	uint32_t index)
{
	struct ufs_mount_state *ms;
	uint64_t start;
	uint32_t length;
	uint32_t pos;
	uint32_t at;
	uint32_t number;
	uint16_t need;
	uint16_t reclen;
	uint16_t minimum;
	uint16_t available;
	uint8_t namesize;
	unsigned n;
	int difference;
	int error;

	/* Starts with no record chosen for the name. */
	ms = state(directory->i_mount);
	at = UINT32_MAX;
	available = 0;

	/* Bounds the name before modifying private bytes. */
	if (name->cn_namelen == 0 || name->cn_namelen > 255U)
		return EINVAL;

	/* Rejects a component that holds a byte no name may contain. */
	for (n = 0; n < name->cn_namelen; n++) {
		/* A separator is the one byte a component may not hold. */
		if (name->cn_nameptr[n] == '/')
			return EINVAL;
	}

	/* Refuses a size the direct blocks could not hold, or a block past it. */
	error = dir_check_size(directory);
	if (error != 0)
		return error;

	/* Refuses a block that starts past the directory's end. */
	start = (uint64_t)index * ms->super.bsize;
	if (start > (uint64_t)directory->i_size)
		return EIO;

	/* Measures the block's records and the room the name needs. */
	length = dir_block_length(directory, index);
	need = dir_minimum((uint8_t)name->cn_namelen);

	/* Checks every record while choosing the first one with room. */
	pos = 0;
	while (pos < length) {
		/* Refuses a record that is not well formed. */
		error = dir_check_record(ms, block, length, pos);
		if (error != 0)
			return error;

		/* The inode number, the record length and the name length. */
		number = drv_ufs_get32(block, pos, ms->super.swapped);
		reclen = drv_ufs_get16(block, pos + 4U, ms->super.swapped);
		namesize = block[pos + 7U];
		minimum = dir_minimum(namesize);

		/* Compares the record name only when it could possibly match. */
		difference = 1;
		if (number != 0 && namesize == name->cn_namelen)
			difference = kern_memcmp(block + pos + 8U, name->cn_nameptr, namesize);

		/* Refuses a name the block already holds. */
		if (difference == 0)
			return EEXIST;

		/* Remembers the first record that has room for the new name. */
		if (at == UINT32_MAX) {
			/* A free record long enough takes the new name whole. */
			if (number == 0 && reclen >= need) {
				at = pos;
				available = reclen;
			} else if (reclen - minimum >= need) {
				at = pos + minimum;
				available = reclen - minimum;
			}
		}

		/* Moves on to the next record. */
		pos += reclen;
	}

	/* With no room, a last block with space takes a new record chunk. */
	if (at == UINT32_MAX) {
		/* Refuses when this is not the last block or the block is full. */
		if (start + length != (uint64_t)directory->i_size)
			return ENOSPC;
		if (length + UFS_DIRBLKSIZ > ms->super.bsize)
			return ENOSPC;

		/* The name takes the whole new chunk. */
		at = length;
		available = UFS_DIRBLKSIZ;
		directory->i_size += UFS_DIRBLKSIZ;
	} else {
		/* Shortens an occupied record in front of the new one. */
		pos = 0;
		while (pos < at) {
			/* Steps over one record, cutting the one the name splits. */
			reclen = drv_ufs_get16(block, pos + 4U, ms->super.swapped);
			if (pos + reclen > at) {
				drv_ufs_put16(block, pos + 4U, (uint16_t)(at - pos), ms->super.swapped);
				break;
			}

			/* Moves on to the next record. */
			pos += reclen;
		}
	}

	/* Writes the new record. */
	drv_ufs_put32(block, at, (uint32_t)target->i_ino, ms->super.swapped);
	drv_ufs_put16(block, at + 4U, available, ms->super.swapped);
	block[at + 6U] = dir_type(target->i_type);
	block[at + 7U] = (uint8_t)name->cn_namelen;
	kern_memcpy(block + at + 8U, name->cn_nameptr, name->cn_namelen);

	/* The insertion is complete in the private images. */
	return 0;
}

/*
 * Chooses the block of a directory a new name goes into.
 *
 * Every block is read: a name any block already holds is refused, and the
 * first block with a record that has room is chosen.  With none, the last
 * block is chosen when it has room for another record chunk, or else the
 * block the directory would grow into; the caller adds that one first.
 */
static int
directory_insert_block(
	struct inode *directory,
	const struct componentname *name,
	uint8_t *scratch,
	uint32_t *index,
	int *grow)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	uint32_t count;
	uint32_t current;
	uint32_t length;
	uint32_t chosen;
	uint32_t offset;
	uint32_t previous;
	uint32_t number;
	uint16_t need;
	int error;

	/* Takes the mount state, the private inode and the room needed. */
	ms = state(directory->i_mount);
	ui = info(directory);
	need = dir_minimum((uint8_t)name->cn_namelen);
	chosen = UINT32_MAX;
	*grow = 0;

	/* Refuses a directory whose size its direct blocks could not hold. */
	error = dir_check_size(directory);
	if (error != 0)
		return error;

	/* Reads every block, looking for the name and for room. */
	count = dir_block_count(directory);
	for (current = 0; current < count; current++) {
		/* A block inside the size must have been allocated. */
		if (ui->direct[current] == 0)
			return EIO;

		/* Reads the block. */
		error = read_block(directory->i_mount, ui->direct[current], scratch);
		if (error != 0)
			return error;

		/* Refuses a name the block already holds. */
		length = dir_block_length(directory, current);
		error = dir_find_in_block(ms, scratch, length, name, &offset, &previous, &number);
		if (error == 0)
			return EEXIST;
		if (error != ENOENT)
			return error;

		/* Keeps the first block with a record that has room. */
		if (chosen == UINT32_MAX) {
			error = dir_find_slack(ms, scratch, length, need, &offset);
			if (error == 0)
				chosen = current;
			else if (error != ENOENT)
				return error;
		}
	}

	/* A block with room takes the name. */
	if (chosen != UINT32_MAX) {
		*index = chosen;

		/* Succeeded. */
		return 0;
	}

	/* A last block with space takes a new record chunk. */
	if ((uint64_t)directory->i_size % ms->super.bsize != 0) {
		*index = count - 1U;

		/* Succeeded. */
		return 0;
	}

	/* Otherwise the name starts the next block, if there can be one. */
	if (count >= UFS_NDADDR)
		return ENOSPC;

	/* Asks the caller to add the block the name starts. */
	*index = count;
	*grow = 1;

	/* Succeeded: the caller adds the block first. */
	return 0;
}

/*
 * Chooses the block a new name goes into and, when every block is full, adds
 * the next block to the directory in a group of its own.
 *
 * The directory's lock is held and the mount's is not.  A directory with no
 * block yet is grown the same way; a block already there (kept after an
 * earlier name failed to go in) is used as it is.
 */
static int
directory_insert_prepare(
	struct inode *directory,
	const struct componentname *name,
	uint32_t *index)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	uint8_t *scratch;
	int grow;
	int handled;
	int error;

	/* Takes the staging the blocks are read into. */
	ms = state(directory->i_mount);
	ui = info(directory);
	scratch = kern_malloc(ms->super.bsize);
	if (scratch == NULL)
		return ENOMEM;

	/* Chooses the block, reading the directory. */
	grow = 0;
	error = directory_insert_block(directory, name, scratch, index, &grow);

	/* Gives the staging back. */
	kern_free(scratch);

	/* Reports why no block could be chosen. */
	if (error != 0)
		return error;

	/* Adds the next block when the name starts it and it is not there yet. */
	if (grow && ui->direct[*index] == 0) {
		error = directory_backing_group(directory, &handled);
		if (error == 0 && !handled)
			error = EOPNOTSUPP;
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Merges shared dinodes and publishes a hard-link insertion as one group. */
static int
link_group_locked(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target,
	struct ufs_link_group *group,
	uint32_t index)
{
	struct ufs_mount_state *ms;
	int error;

	/* Takes the mount state this call runs against. */
	ms = state(directory->i_mount);

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable)
		return EROFS;

	/* Refuses a link the target link count could not hold. */
	if (target->i_linkcount == UINT16_MAX)
		return EMLINK;

	kern_memcpy(&group->directory_image, info(directory), sizeof(group->directory_image));
	kern_memcpy(&group->target_image, info(target), sizeof(group->target_image));

	group->target_image.inode.i_linkcount++;

	/* Stages the block the new name goes into. */
	error = metadata_image_get(&group->images,
				   info(directory)->direct[index],
				   &group->directory);
	if (error != 0)
		return error;

	/* Writes the new name into that block. */
	error = directory_image_insert(&group->directory_image.inode,
				       name,
				       target, group->directory, index);
	if (error != 0)
		return error;

	/*
	 * Encodes all changed dinodes into unique, shared physical block
	 * images.
	 */

	/* Stages the target inode with its new link count. */
	error = metadata_image_inode(&group->images,
				     &group->directory_image.inode);
	if (error != 0)
		return error;

	/* Stages the target inode with its new link count. */
	error = metadata_image_inode(&group->images,
				     &group->target_image.inode);
	if (error != 0)
		return error;

	error = metadata_group_commit(directory->i_mount, group->images.extents,
				      group->images.count, NULL,
				      &group->outcome);

	/* Publishes the target inode as the group committed it. */
	if (group->outcome.committed) {
		directory->i_size = group->directory_image.inode.i_size;

		/*
		 * The generic inode_link wrapper increments live nlink only on
		 * success.
		 */
		if (error != 0) {
			target->i_linkcount =
				group->target_image.inode.i_linkcount;
		}
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Owns private hard-link preparation through its live namespace outcome. */
static int
link_group(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target,
	int *handled)
{
	struct ufs_mount_state *ms;
	struct ufs_link_group *group;
	struct ufs_inode_info *ui;
	uint64_t directory_fragment;
	uint64_t target_fragment;
	uint32_t index;
	unsigned images;
	size_t bytes;
	int error;

	/* Takes the mount state this call runs against. */
	ms = state(directory->i_mount);

	*handled = 0;

	/* Takes the private inode the directory block pointer lives in. */
	ui = info(directory);

	/* A volume without a journal links one step at a time. */
	if (!ms->journal_enabled || ui->direct[0] == 0)
		return 0;

	/* Locates the shared blocks the two inodes are written in. */
	directory_fragment = inode_fragment(directory);
	target_fragment = inode_fragment(target);

	/* Two inodes in the same block need one image, not two. */
	images = 3U;
	if (directory_fragment == target_fragment)
		images = 2U;

	bytes = images * ms->super.bsize;
	if (bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
		/* Succeeded. */
		return 0;
	}
	*handled = 1;

	/* Takes the staging the whole link is assembled in. */
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(bytes);

	/* Gives up before touching the volume when there is no staging. */
	if (group->memory == NULL) {
		kern_free(group);

		/* Failed. */
		return ENOMEM;
	}

	metadata_images_init(&group->images, directory->i_mount, group->memory,
			     bytes);

	/* Chooses the block the name goes into and links it, under the locks. */
	mutex_lock(&directory->i_lock);

	/* Chooses the block, adding one when all are full, then links the name. */
	error = directory_insert_prepare(directory, name, &index);
	if (error == 0) {
		mutex_lock(&target->i_lock);
		mutex_lock(&ms->lock);
		error = link_group_locked(directory, name, target, group, index);
		mutex_unlock(&ms->lock);
		mutex_unlock(&target->i_lock);
	}

	/* The name is published, or the group left nothing behind. */
	mutex_unlock(&directory->i_lock);

	/* A group that could not be assembled leaves nothing behind. */
	if (error != 0 &&
	    (group->outcome.committed || group->outcome.uncertain)) {
		namecache_remove(directory, name);
		inode_dir_changed(directory);
	}

	kern_free(group->memory);
	kern_free(group);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Replaces or removes one name in one block of a private directory image.
 *
 * Every record of the block is checked first; the name must be there once and
 * name the inode the change expects.  A replacement points the record at
 * another inode; a removal joins the record to the one before it in the same
 * record chunk, or frees it when it is the chunk's first.
 */
static int
directory_image_change(
	struct inode *directory,
	uint8_t *block,
	uint32_t index,
	const struct componentname *name,
	uint32_t expected,
	uint32_t replacement,
	uint8_t type)
{
	struct ufs_mount_state *ms;
	uint32_t length;
	uint32_t pos;
	uint32_t previous;
	uint32_t found;
	uint32_t prior;
	uint32_t number;
	uint16_t reclen;
	uint8_t namesize;
	int difference;
	int error;

	/* Refuses a size the direct blocks could not hold, or a block past it. */
	ms = state(directory->i_mount);
	error = dir_check_size(directory);
	if (error != 0)
		return error;
	if ((uint64_t)index * ms->super.bsize >= (uint64_t)directory->i_size)
		return EIO;

	/* Finds the one record of the name, checking every record. */
	length = dir_block_length(directory, index);
	pos = 0;
	previous = UINT32_MAX;
	found = UINT32_MAX;
	prior = UINT32_MAX;
	while (pos < length) {
		/* Refuses a record that is not well formed. */
		error = dir_check_record(ms, block, length, pos);
		if (error != 0)
			return error;

		/* The inode number, the record length and the name length. */
		number = drv_ufs_get32(block, pos, ms->super.swapped);
		reclen = drv_ufs_get16(block, pos + 4U, ms->super.swapped);
		namesize = block[pos + 7U];

		/* Compares the record name only when it could possibly match. */
		difference = 1;
		if (number != 0 && namesize == name->cn_namelen)
			difference = kern_memcmp(block + pos + 8U, name->cn_nameptr, namesize);

		/* Refuses a second record of the name, or one naming another inode. */
		if (difference == 0) {
			if (found != UINT32_MAX || number != expected)
				return EIO;

			/* Remembers the record and the one before it. */
			found = pos;
			prior = previous;
		}

		/* Moves on to the next record. */
		previous = pos;
		pos += reclen;
	}

	/* Refuses a stale lookup without modifying its private directory. */
	if (found == UINT32_MAX)
		return ENOENT;

	/* Replaces the reference, or joins the removed record to its predecessor. */
	if (replacement != 0) {
		drv_ufs_put32(block, found, replacement, ms->super.swapped);
		block[found + 6U] = type;
	} else if (prior != UINT32_MAX && prior / UFS_DIRBLKSIZ == found / UFS_DIRBLKSIZ) {
		reclen = drv_ufs_get16(block, found + 4U, ms->super.swapped);
		reclen += drv_ufs_get16(block, prior + 4U, ms->super.swapped);
		drv_ufs_put16(block, prior + 4U, reclen, ms->super.swapped);
	} else {
		drv_ufs_put32(block, found, 0, ms->super.swapped);
	}

	/* Every changed byte stays private until the group commits. */
	return 0;
}

/*
 * Prepares both names, directory ancestry and link accounting as one operation.
 */
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
	 * parents.
	 */

	/* Takes the mount and the two directory images the rename touches. */
	ms = state(old_directory->i_mount);
	if (!ms->writable)
		return EROFS;
	kern_memcpy(&group->old_image, info(old_directory),
	       sizeof(group->old_image));
	kern_memcpy(&group->new_image, info(new_directory),
	       sizeof(group->new_image));
	old_image = &group->old_image.inode;

	/* A rename that stays inside one directory edits a single image. */
	if (old_directory == new_directory)
		new_image = old_image;
	else
		new_image = &group->new_image.inode;

	moving_directory = source->i_type == INODE_DIR;

	/* Validates all link transitions before editing namespace bytes. */
	if (source->i_linkcount == 0 ||
	    (target != NULL && target->i_linkcount == 0)) {
		/* Failed. */
		return EIO;
	}

	/* A directory that moves takes its parent link with it. */
	if (moving_directory && old_directory != new_directory) {
		/* A parent with no links left cannot lose another one. */
		if (old_image->i_linkcount == 0)
			return EIO;

		/* Refuses a link count the new parent cannot hold. */
		if (target == NULL && new_image->i_linkcount == UINT16_MAX)
			return EMLINK;
		old_image->i_linkcount--;

		/* The new parent gains the link the moved directory brings. */
		if (target == NULL)
			new_image->i_linkcount++;
	} else if (moving_directory && target != NULL) {
		/* A parent with no links left cannot lose another one. */
		if (old_image->i_linkcount == 0)
			return EIO;
		old_image->i_linkcount--;
	}

	/* A replaced inode loses the link the old name held. */
	if (target != NULL) {
		kern_memcpy(&group->target_image, info(target),
		       sizeof(group->target_image));

		/* A replaced directory also loses its own parent link. */
		if (moving_directory)
			group->target_image.inode.i_linkcount = 0;
		else
			group->target_image.inode.i_linkcount--;
	}

	/*
	 * Removes the old name first so a full same-parent directory can reuse
	 * its space.
	 */

	/* Stages the block the old name lives in. */
	error = metadata_image_get(&group->images,
				   info(old_directory)->direct[group->old_index], &old_block);
	if (error != 0)
		return error;

	/* Removes the old name from that block. */
	error = directory_image_change(old_image, old_block, group->old_index,
				       old_name, (uint32_t)source->i_ino, 0, 0);
	if (error != 0)
		return error;

	/* Stages the block the new name lives in. */
	error = metadata_image_get(&group->images,
				   info(new_directory)->direct[group->new_index], &new_block);
	if (error != 0)
		return error;

	/* Points the existing entry at the inode being renamed. */
	if (target != NULL) {
		error = directory_image_change(new_image,
					       new_block,
					       group->new_index,
					       new_name,
					       (uint32_t)target->i_ino,
					       (uint32_t)source->i_ino,
					       dir_type(source->i_type));
	} else {
		error = directory_image_insert(new_image,
					       new_name,
					       source,
					       new_block,
					       group->new_index);
	}
	if (error != 0)
		return error;

	/*
	 * Reparents a moved directory in the same transaction as its visible
	 * names.
	 */
	if (moving_directory && old_directory != new_directory) {
		/*
		 * Stages the block the moved directory keeps its parent link
		 * in.
		 */
		error = metadata_image_get(&group->images, info(source)->direct[0], &child_block);
		if (error != 0)
			return error;

		/* Repoints that parent link at the new parent. */
		error = directory_image_change(source,
					       child_block,
					       0,
					       &dotdot,
					       (uint32_t)old_directory->i_ino,
					       (uint32_t)new_directory->i_ino,
					       4);
		if (error != 0)
			return error;
	}

	/*
	 * Merges all changed dinodes without reloading shared physical blocks.
	 */

	/* Stages the old parent inode with its new link count. */
	error = metadata_image_inode(&group->images, old_image);
	if (error != 0)
		return error;

	/* Stages the new parent as well when it is a different inode. */
	if (new_image != old_image) {
		/* Stages the new parent inode with its new link count. */
		error = metadata_image_inode(&group->images, new_image);
		if (error != 0)
			return error;
	}

	/* Stages the replaced inode with its new link count. */
	if (target != NULL) {
		/* Stages the replaced inode with its new link count. */
		error = metadata_image_inode(&group->images,
					     &group->target_image.inode);
		if (error != 0)
			return error;
	}

	error = metadata_group_commit(old_directory->i_mount,
				      group->images.extents,
				      group->images.count,
				      NULL,
				      &group->outcome);

	/*
	 * Publishes only the live fields whose persistent images are proven
	 * committed.
	 */
	if (group->outcome.committed) {
		old_directory->i_size = old_image->i_size;
		old_directory->i_linkcount = old_image->i_linkcount;
		new_directory->i_size = new_image->i_size;
		new_directory->i_linkcount = new_image->i_linkcount;

		/* Publishes the replaced inode as the group committed it. */
		if (target != NULL) {
			target->i_linkcount = group->target_image.inode.i_linkcount;

			/* A replaced inode with no links left is retired. */
			if (target->i_linkcount == 0)
				target->i_flags |= INODE_DEAD;
		}
	}

	/*
	 * Preserves an error even when recovery establishes that the rename
	 * committed.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Finds the block the old name is in and the block the new name goes into,
 * adding the new directory's next block when every block of it is full.
 *
 * The inodes' locks are held and the mount's is not.  A rename inside one
 * full directory reuses the old name's block: the old name leaves room there.
 */
static int
rename_blocks_prepare(
	struct inode *old_directory,
	const struct componentname *old_name,
	struct inode *new_directory,
	const struct componentname *new_name,
	struct inode *target,
	struct ufs_rename_group *group)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	uint8_t *scratch;
	uint32_t offset;
	uint32_t previous;
	uint32_t number;
	int grow;
	int handled;
	int error;

	/* Takes the staging the blocks are read into. */
	ms = state(old_directory->i_mount);
	ui = info(new_directory);
	scratch = kern_malloc(ms->super.bsize);
	if (scratch == NULL)
		return ENOMEM;

	/* Finds the block the old name is in. */
	grow = 0;
	error = dir_find_record(old_directory, old_name, scratch, &group->old_index, &offset, &previous, &number);

	/* A replaced name keeps its block; a new one is given a block. */
	if (error == 0 && target != NULL) {
		error = dir_find_record(new_directory, new_name, scratch, &group->new_index, &offset, &previous, &number);
	} else if (error == 0) {
		error = directory_insert_block(new_directory, new_name, scratch, &group->new_index, &grow);

		/* A full directory the name stays in takes it where it was. */
		if (error == ENOSPC && old_directory == new_directory) {
			group->new_index = group->old_index;
			error = 0;
		}
	}

	/* Gives the staging back. */
	kern_free(scratch);

	/* Adds the next block when the new name starts it and it is not there yet. */
	if (error == 0 && grow && ui->direct[group->new_index] == 0) {
		error = directory_backing_group(new_directory, &handled);
		if (error == 0 && !handled)
			error = EOPNOTSUPP;
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Owns unique inode locks, exact image capacity and rename cache publication.
 */
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
	struct ufs_inode_info *ui;
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
	 * errno.
	 */
	ms = state(old_directory->i_mount);
	*handled = 0;

	/* Takes the private inode the directory block pointer lives in. */
	ui = info(new_directory);

	/* A volume without a journal renames one step at a time. */
	if (!ms->journal_enabled || ui->direct[0] == 0)
		return 0;

	*handled = 1;
	/* Refuses a rename whose ends are the directories themselves. */
	if (source == old_directory || source == new_directory ||
	    target == old_directory || target == new_directory) {
		/* Failed. */
		return EINVAL;
	}

	/*
	 * Counts distinct blocks so shared dinodes do not unnecessarily exhaust
	 * a slot.
	 */
	/*
	 * The blocks of the two names are chosen once the inodes are locked,
	 * so they are counted as two blocks no inode shares.
	 */
	fragments[0] = UINT64_MAX;
	fragments[1] = UINT64_MAX - 1U;
	fragments[2] = inode_fragment(old_directory);
	fragments[3] = inode_fragment(new_directory);

	/* Counts the blocks the rename touches, before removing duplicates. */
	count = 4;
	if (source->i_type == INODE_DIR && old_directory != new_directory)
		fragments[count++] = info(source)->direct[0];

	/* A replaced inode adds its own block to that count. */
	if (target != NULL)
		fragments[count++] = inode_fragment(target);
	unique = 0;

	/*
	 * Deduplicates the bounded footprint before allocating any private
	 * image.
	 */
	for (n = 0; n < count; n++) {
		/* Counts the blocks the rename actually touches. */
		for (j = 0; j < n; j++) {
			/* A block already counted is not staged twice. */
			if (fragments[j] == fragments[n])
				break;
		}

		/* This block is one the rename has not seen yet. */
		if (j == n)
			unique++;
	}

	/* Sizes the staging from the blocks it will hold. */
	bytes = unique * ms->super.bsize;
	if (ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
		*handled = 0;
		/* Succeeded. */
		return 0;
	}

	/* Allocates the operation and its exact private block storage. */

	/* Takes the staging the whole rename is assembled in. */
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(bytes);

	/* Gives up before touching the volume when there is no staging. */
	if (group->memory == NULL) {
		kern_free(group);

		/* Failed. */
		return ENOMEM;
	}

	metadata_images_init(&group->images, old_directory->i_mount,
			     group->memory, bytes);
	locks[0] = old_directory;

	/* Locks the inodes the rename changes, in a fixed order. */
	lock_count = 1;
	if (new_directory != old_directory)
		locks[lock_count++] = new_directory;
	locks[lock_count++] = source;

	/* A replaced inode that is not the source is locked as well. */
	if (target != NULL && target != source)
		locks[lock_count++] = target;

	/*
	 * Retains namespace exclusion from the caller while locking each inode
	 * once.
	 */
	for (n = 0; n < lock_count; n++)
		mutex_lock(&locks[n]->i_lock);

	/* Finds the blocks of both names, adding one when the new name needs it. */
	error = rename_blocks_prepare(old_directory, old_name, new_directory, new_name, target, group);

	/* Renames the name with every inode of the rename held. */
	if (error == 0) {
		mutex_lock(&ms->lock);
		error = rename_group_locked(old_directory,
					    old_name,
					    new_directory,
					    new_name,
					    source,
					    target,
					    group);
		mutex_unlock(&ms->lock);
	}

	/*
	 * Releases inode ownership before cache publication, including error
	 * outcomes.
	 */
	while (lock_count != 0)
		mutex_unlock(&locks[--lock_count]->i_lock);

	/* A group that could not be assembled leaves nothing behind. */
	if (error != 0 &&
	    (group->outcome.committed || group->outcome.uncertain)) {
		namecache_remove(old_directory, old_name);
		namecache_remove(new_directory, new_name);
		inode_dir_changed(old_directory);

		/* Releases the second directory when the two differ. */
		if (new_directory != old_directory)
			inode_dir_changed(new_directory);

		/*
		 * A directory that moved has its parent link changed as well.
		 */
		if (source->i_type == INODE_DIR &&
		    old_directory != new_directory)
			inode_dir_changed(source);
	}

	kern_free(group->memory);
	kern_free(group);

	/*
	 * Returns the original transaction result after releasing transient
	 * ownership.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Publishes a prepared zero-link child and its parent name in one redo group.
 */
static int
creation_group_locked(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target,
	struct ufs_link_group *group,
	uint32_t index)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	mode_t kind;
	int error;
	int is_directory;

	/*
	 * Rejects stale identities before editing either private dinode.
	 */

	/* Takes the mount the creation runs against. */
	ms = state(directory->i_mount);
	if (!ms->writable)
		return EROFS;

	/* The stored mode this kind of file is written to disk as. */
	kind = inode_type_mode(target->i_type);

	/* Takes the private inode the directory block pointer lives in. */
	ui = info(directory);

	/* A name can only be added to something that is a directory. */
	if (directory->i_type != INODE_DIR)
		return EIO;	/* Failed. */

	/* A target a name already points at is not a fresh inode. */
	if (target->i_linkcount != 0)
		return EIO;	/* Failed. */

	/* Nor is the root, nor a number the on-disk record could not hold. */
	if (target->i_ino <= UFS_ROOT_INO || target->i_ino > UINT32_MAX)
		return EIO;	/* Failed. */

	/* A kind with no stored mode could not be written to the volume. */
	if (kind == 0)
		return EIO;	/* Failed. */

	/* And the parent has to have the block the name goes into. */
	if (index >= UFS_NDADDR || ui->direct[index] == 0)
		return EIO;	/* Failed. */

	/* A new directory brings a link to its parent with it. */
	is_directory = target->i_type == INODE_DIR;
	if (is_directory && directory->i_linkcount == UINT16_MAX)
		return EMLINK;

	/*
	 * Keeps all prepared content while changing only the final link
	 * relationship.
	 */
	kern_memcpy(&group->directory_image, info(directory),
	       sizeof(group->directory_image));
	kern_memcpy(&group->target_image, info(target), sizeof(group->target_image));
	group->target_image.inode.i_linkcount = is_directory ? 2 : 1;

	/* A new directory starts with the two links it names itself by. */
	if (is_directory)
		group->directory_image.inode.i_linkcount++;

	/* Stages the block the new name goes into. */
	error = metadata_image_get(&group->images, info(directory)->direct[index],
				   &group->directory);
	if (error != 0)
		return error;

	/* Writes the new name into that block. */
	error = directory_image_insert(&group->directory_image.inode, name,
				       target, group->directory, index);
	if (error != 0)
		return error;

	/*
	 * Merges shared parent and child slots before issuing any home
	 * mutation.
	 */

	/* Stages the parent inode with its new link count. */
	error = metadata_image_inode(&group->images,
				     &group->directory_image.inode);
	if (error != 0)
		return error;

	/* Stages the new inode itself. */
	error = metadata_image_inode(&group->images,
				     &group->target_image.inode);
	if (error != 0)
		return error;
	error = metadata_group_commit(directory->i_mount, group->images.extents,
				      group->images.count, NULL,
				      &group->outcome);

	/*
	 * Reflects a proven publication even when its checkpoint returned an
	 * error.
	 */
	if (group->outcome.committed) {
		directory->i_size = group->directory_image.inode.i_size;
		directory->i_linkcount =
			group->directory_image.inode.i_linkcount;
		target->i_linkcount = group->target_image.inode.i_linkcount;
	}

	/* Returns the original I/O result separately from durable ownership. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Owns initial parent backing and private final publication under namespace
 * exclusion.
 */
static int
creation_group(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target,
	struct ufs_transaction_outcome *outcome)
{
	struct ufs_mount_state *ms;
	struct ufs_link_group *group;
	uint32_t index;
	size_t bytes;
	int error;

	/*
	 * Establishes an unambiguous unpublished outcome before allocating
	 * resources.
	 */
	kern_memset(outcome, 0, sizeof(*outcome));

	/* Refuses a creation whose parent and target are the same inode. */
	if (directory == target || directory->i_mount != target->i_mount)
		return EINVAL;
	ms = state(directory->i_mount);

	/* Sizes the staging from the images the group will hold. */
	bytes = 3U * ms->super.bsize;
	if (!ms->journal_enabled || ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
		/* Failed. */
		return EOPNOTSUPP;
	}

	/* Takes the staging the whole creation is assembled in. */
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(bytes);

	/* Gives up before touching the volume when there is no staging. */
	if (group->memory == NULL) {
		kern_free(group);

		/* Failed. */
		return ENOMEM;
	}

	/*
	 * Prepares recoverable empty parent backing before publishing the new
	 * name.
	 */
	metadata_images_init(&group->images, directory->i_mount, group->memory,
			     bytes);
	mutex_lock(&directory->i_lock);

	/* Chooses the block the name goes into, adding one when all are full. */
	error = directory_insert_prepare(directory, name, &index);
	if (error == 0) {
		mutex_lock(&target->i_lock);
		mutex_lock(&ms->lock);
		error = creation_group_locked(directory, name, target, group, index);
		mutex_unlock(&ms->lock);
		mutex_unlock(&target->i_lock);
	}

	mutex_unlock(&directory->i_lock);

	*outcome = group->outcome;

	/*
	 * Invalidates stale names when the generic caller cannot report
	 * success.
	 */
	if (error != 0 && (outcome->committed || outcome->uncertain)) {
		namecache_remove(directory, name);
		inode_dir_changed(directory);
	}

	kern_free(group->memory);
	kern_free(group);

	/*
	 * Preserves the admitted error without falling back to independent
	 * writes.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Finishes all grouped creation kinds without discarding a possibly named
 * child.
 */
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

	/* Publishes the whole creation as one journal group. */
	error = creation_group(directory, name, target, &outcome);
	if (error == 0) {
		*result = target;
		/* Succeeded. */
		return 0;
	}

	/*
	 * Leaves committed or unresolved names for ordinary lifetime and mount
	 * recovery.
	 */
	if (outcome.committed || outcome.uncertain) {
		detach_new_socket_special(target);
		inode_release(target);

		/* Failed. */
		return error;
	}

	/*
	 * Reclaims only a child whose name publication was definitely not
	 * admitted.
	 */

	/* Reports the failure. */
	error = discard_new_inode_after_error(
		target, target->i_type == INODE_DIR, error);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static int
ufs_create(
	struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request,
	struct inode **result)
{
	struct inode *existing;
	struct inode *inode;
	struct ufs_mount_state *ms;
	int error;

	/* Takes the mount state this call runs against. */
	ms = state(directory->i_mount);

	*result = NULL;
	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable)
		return EROFS;
	namespace_enter(ms);

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable) {
		error = EROFS;
		goto out;
	}

	/* Refuses a name the directory already holds. */
	error = ufs_lookup(directory, name, &existing);
	if (error == 0) {
		inode_release(existing);
		error = EEXIST;
		goto out;
	}
	if (error != ENOENT)
		goto out;

	/* Creates the inode the new name will refer to. */
	error = new_inode(directory, request, 1, &inode);
	if (error)
		goto out;

	/* A grouped creation has already published the name as well. */
	if (inode->i_linkcount == 0) {
		error = creation_publish(directory, name, inode, result);
		goto out;
	}

	/* Writes the entry that names the new inode. */
	error = dir_add(directory, name, (uint32_t)inode->i_ino, 8);
	if (error) {
		error = discard_new_inode_after_error(inode, 0, error);
		goto out;
	}

	*result = inode;
out:

	namespace_leave(ms);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Creates a directory. */
static int
ufs_mkdir(
	struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request,
	struct inode **result)
{
	int cleanup;
	int name_removed;
	struct inode *existing;
	struct inode *inode;
	struct componentname dot = {".", 1, 0}, dotdot = {"..", 2, 0};
	struct ufs_mount_state *ms;
	uint32_t removed;
	nlink_t old_directory_links;
	int error;
	int rollback_error;

	ms = state(directory->i_mount);
	removed = 0;

	*result = NULL;

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable)
		return EROFS;

	namespace_enter(ms);

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable) {
		error = EROFS;
		goto out;
	}

	/* Refuses a name the directory already holds. */
	error = ufs_lookup(directory, name, &existing);
	if (error == 0) {
		inode_release(existing);
		error = EEXIST;
		goto out;
	}
	if (error != ENOENT)
		goto out;

	/* Creates the inode, with the two links every directory starts with. */
	error = new_inode(directory, request, 2, &inode);
	if (error)
		goto out;

	/* Writes the entry that names the new directory itself. */
	error = dir_add(inode, &dot, (uint32_t)inode->i_ino, 4);

	if (error == 0)
		error = dir_add(inode, &dotdot, (uint32_t)directory->i_ino, 4);

	if (error == 0 && inode->i_linkcount == 0) {
		error = creation_publish(directory, name, inode, result);
		goto out;
	}

	if (error == 0)
		error = dir_add(directory, name, (uint32_t)inode->i_ino, 4);

	if (error) {
		error = discard_new_inode_after_error(inode, 1, error);
		goto out;
	}

	mutex_lock(&directory->i_lock);

	old_directory_links = directory->i_linkcount;
	directory->i_linkcount++;
	error = persist_inode(directory);

	mutex_unlock(&directory->i_lock);

	/* Publishes the new directory in its parent. */
	if (error == 0) {
		*result = inode;
	} else {
		name_removed = 0;

		/*
		 * Takes the name back out when the parent could not be
		 * published.
		 */
		rollback_error = dir_remove(directory, name, &removed);
		if (rollback_error == 0) {
			name_removed = 1;

			/*
			 * The entry that was removed was not the one just
			 * created.
			 */
			if (removed != (uint32_t)inode->i_ino)
				rollback_error = EIO;

			/* Puts the parent's link count back as well. */
			mutex_lock(&directory->i_lock);
			directory->i_linkcount = old_directory_links;
			if (rollback_error == 0)
				rollback_error = persist_inode(directory);
			mutex_unlock(&directory->i_lock);
		}

		/*
		 * A name that could not be removed leaves the volume
		 * unwritable.
		 */
		if (!name_removed) {
			stop_writing(directory->i_mount, "the name of a failed new directory could not be removed", rollback_error);
			inode_release(inode);
		} else {
			/*
			 * Undoes the creation now that nothing names the inode.
			 */
			cleanup = discard_new_inode(inode, 1);
			if (rollback_error == 0 && cleanup != 0)
				rollback_error = cleanup;
			if (rollback_error != 0)
				stop_writing(directory->i_mount, "a failed new directory could not be undone", rollback_error);
		}
		if (rollback_error != 0)
			error = rollback_error;
	}

out:

	namespace_leave(ms);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Creates a device, socket or named pipe. */
static int
ufs_mknod(
	struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request,
	struct inode **result)
{
	struct inode *existing;
	struct inode *inode;
	struct ufs_mount_state *ms;
	int error;

	/* Takes the mount state this call runs against. */
	ms = state(directory->i_mount);

	/* A call that names no request has nothing to create. */
	if (request == NULL)
		return EOPNOTSUPP;	/* Failed. */

	/* Only these four kinds of node are made through this entry point. */
	if (request->type != INODE_FIFO && request->type != INODE_SOCKET &&
	    request->type != INODE_CHAR && request->type != INODE_BLOCK)
		return EOPNOTSUPP;	/* Failed. */
	*result = NULL;

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable)
		return EROFS;

	namespace_enter(ms);

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable) {
		error = EROFS;
		goto out;
	}

	/* Refuses a name the directory already holds. */
	error = ufs_lookup(directory, name, &existing);
	if (error == 0) {
		inode_release(existing);
		error = EEXIST;
		goto out;
	}
	if (error != ENOENT)
		goto out;

	/* Creates the inode the new name will refer to. */
	error = new_inode(directory, request, 1, &inode);
	if (error != 0)
		goto out;

	/* A grouped creation has already published the name as well. */
	if (inode->i_linkcount == 0) {
		error = creation_publish(directory, name, inode, result);
		goto out;
	}

	/* Writes the entry that names the new inode. */
	error = dir_add(directory, name, (uint32_t)inode->i_ino,
			dir_type(request->type));
	if (error != 0) {
		error = discard_new_inode_after_error(inode, 0, error);
		goto out;
	}

	*result = inode;
out:

	namespace_leave(ms);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Removes a name that does not belong to a directory. */
static int
ufs_unlink(
	struct inode *directory,
	const struct componentname *name)
{
	struct ufs_mount_state *ms;
	struct inode *target;
	uint32_t number;
	int error;
	int rollback_error;
	int removed;
	int handled;
	nlink_t old_links;
	unsigned old_flags;

	/* Starts with nothing taken and nothing to put back. */
	ms = state(directory->i_mount);
	target = NULL;
	number = 0;
	removed = 0;
	old_links = 0;
	old_flags = 0;

	namespace_enter(ms);

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable) {
		error = EROFS;
		goto out;
	}

	/* Resolves the name being removed. */
	error = ufs_lookup(directory, name, &target);
	if (error)
		goto out;

	/* Refuses a name that is a directory. */
	if (target->i_type == INODE_DIR) {
		error = EISDIR;
		goto out;
	}

	error = remove_group(directory, name, target, &handled);

	/* The journal path has already carried the removal out. */
	if (handled)
		goto out;

	old_links = target->i_linkcount;
	old_flags = target->i_flags;

	/* Takes the name out of its directory. */
	error = dir_remove(directory, name, &number);
	if (error == 0) {
		removed = 1;
		mutex_lock(&target->i_lock);

		/* An inode with no links left is reclaimed. */
		if (target->i_linkcount == 0) {
			error = EIO;
		} else {
			target->i_linkcount--;
			error = persist_inode(target);

			/* An inode with no links left is reclaimed. */
			if (target->i_linkcount == 0)
				target->i_flags |= INODE_DEAD;
		}

		mutex_unlock(&target->i_lock);
	}

	/* Puts the name back when the inode could not be published. */
	if (error != 0 && removed) {
		mutex_lock(&target->i_lock);
		target->i_linkcount = old_links;
		target->i_flags = old_flags;
		rollback_error = persist_inode(target);
		mutex_unlock(&target->i_lock);
		if (rollback_error == 0) {
			rollback_error = dir_add(directory, name, number,
						 dir_type(target->i_type));
		}
		if (rollback_error != 0)
			stop_writing(directory->i_mount, "a failed unlink could not be undone", rollback_error);
	}

out:
	inode_release(target);

	namespace_leave(ms);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Asks whether a directory holds anything but dot and dot-dot. */
static int
directory_empty(
	struct inode *directory)
{
	off_t cursor = 0;
	uint32_t number;
	uint8_t type;
	char name[NAME_MAX + 1U];
	int dot;
	int dotdot;
	int error;

	/* Walks the directory looking for anything but dot and dot-dot. */
	for (;;) {
		/* Reads the next entry the directory holds. */
		error = next_dirent(directory, &cursor, &number, &type, name);
		if (error != 0)
			break;

		/* Compares the name against the two every directory carries. */
		dot = kern_strcmp(name, ".");
		dotdot = kern_strcmp(name, "..");

		/* Any other name means the directory still holds something. */
		if (dot != 0 && dotdot != 0)
			return 0;
	}

	/*
	 * Succeeded: nothing of that name is present, which is what was asked.
	 */
	if (error == ENOENT)
		return 1;

	/* Failed: the search itself did not finish. */
	return -error;
}

/* Removes an empty directory. */
static int
ufs_rmdir(
	struct inode *directory,
	const struct componentname *name)
{
	struct ufs_mount_state *ms;
	struct inode *target;
	uint32_t number;
	int dot;
	int empty;
	int error;
	int rollback_error;
	int removed;
	int handled;
	nlink_t old_target_links;
	nlink_t old_directory_links;
	unsigned old_target_flags;

	/* Starts with nothing taken and nothing to put back. */
	ms = state(directory->i_mount);
	target = NULL;
	number = 0;
	removed = 0;
	old_target_links = 0;
	old_directory_links = 0;
	old_target_flags = 0;

	/* Asks whether the name is the directory itself or its parent. */
	dot = name_is_dot(name);

	/* Refuses to remove the directory itself or its parent link. */
	if (dot)
		return EINVAL;
	namespace_enter(ms);

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable) {
		error = EROFS;
		goto out;
	}

	/* Resolves the name being removed. */
	error = ufs_lookup(directory, name, &target);
	if (error)
		goto out;

	/* Refuses a name that is not a directory. */
	if (target->i_type != INODE_DIR) {
		error = ENOTDIR;
		goto out;
	}

	/* Asks whether the directory still holds anything. */
	empty = directory_empty(target);
	if (empty <= 0) {
		if (empty == 0) {
			/* The directory still holds names of its own. */
			error = ENOTEMPTY;
		} else {
			/* The scan failed, reporting its error negated. */
			error = -empty;
		}

		goto out;
	}

	error = remove_group(directory, name, target, &handled);

	/* The journal path has already carried the removal out. */
	if (handled)
		goto out;

	old_target_links = target->i_linkcount;
	old_target_flags = target->i_flags;
	old_directory_links = directory->i_linkcount;

	/* Takes the name out of its parent. */
	error = dir_remove(directory, name, &number);
	if (error == 0) {
		removed = 1;
		mutex_lock(&target->i_lock);
		target->i_linkcount = 0;
		target->i_flags |= INODE_DEAD;
		error = persist_inode(target);
		mutex_unlock(&target->i_lock);
		mutex_lock(&directory->i_lock);

		/* The parent loses the link the removed directory held. */
		if (directory->i_linkcount > 0)
			directory->i_linkcount--;

		/* Publishes the parent with its new link count. */
		if (error == 0)
			error = persist_inode(directory);

		mutex_unlock(&directory->i_lock);
	}

	/* A removal that failed puts both inodes and the name back. */
	if (error != 0 && removed) {
		mutex_lock(&target->i_lock);
		target->i_linkcount = old_target_links;
		target->i_flags = old_target_flags;
		rollback_error = persist_inode(target);
		mutex_unlock(&target->i_lock);
		mutex_lock(&directory->i_lock);
		directory->i_linkcount = old_directory_links;
		if (rollback_error == 0)
			rollback_error = persist_inode(directory);
		mutex_unlock(&directory->i_lock);
		if (rollback_error == 0)
			rollback_error = dir_add(directory, name, number, 4);
		if (rollback_error != 0)
			stop_writing(directory->i_mount, "a failed rmdir could not be undone", rollback_error);
	}

out:
	inode_release(target);

	namespace_leave(ms);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Renames or moves a name. */
static int
ufs_rename(
	struct inode *old_directory,
	const struct componentname *old_name,
	struct inode *new_directory,
	const struct componentname *new_name,
	unsigned flags)
{
	uint32_t ignored;
	uint8_t ignored_type;
	static const struct componentname dotdot = {"..", 2, 0};
	uint32_t old_parent;
	uint8_t old_parent_type;
	struct ufs_mount_state *ms;
	struct inode *source;
	struct inode *target;
	uint32_t removed;
	uint32_t replaced;
	uint8_t replaced_type;
	nlink_t old_target_links;
	nlink_t old_old_directory_links;
	nlink_t old_new_directory_links;
	unsigned old_target_flags;
	int target_exists;
	int namespace_committed;
	int dotdot_changed;
	int old_dot;
	int new_dot;
	int difference;
	int restored;
	int persisted;
	int empty;
	int error;
	int rollback_error;
	int handled;

	/* Starts with nothing taken and nothing to put back. */
	ms = state(old_directory->i_mount);
	source = NULL;
	target = NULL;
	removed = 0;
	replaced = 0;
	replaced_type = 0;
	old_target_links = 0;
	old_old_directory_links = 0;
	old_new_directory_links = 0;
	old_target_flags = 0;
	target_exists = 0;
	namespace_committed = 0;
	dotdot_changed = 0;
	rollback_error = 0;

	/* Rejects a flag word this file system does not define. */
	if (flags != 0)
		return EINVAL;

	/* Refuses a rename that would cross file systems. */
	if (old_directory->i_mount != new_directory->i_mount)
		return EXDEV;

	/* Asks whether either name is the directory itself or its parent. */
	old_dot = name_is_dot(old_name);
	new_dot = name_is_dot(new_name);

	/* Refuses a rename of the directory itself or of its parent link. */
	if (old_dot || new_dot)
		return EINVAL;

	/* Compares the two names byte for byte. */
	difference = kern_memcmp(old_name->cn_nameptr, new_name->cn_nameptr,
			    old_name->cn_namelen);

	/* Succeeded: renaming a name onto itself changes nothing. */
	if (old_directory == new_directory &&
	    old_name->cn_namelen == new_name->cn_namelen &&
	    difference == 0) {
		/* Succeeded. */
		return 0;
	}

	namespace_enter(ms);

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable) {
		error = EROFS;
		goto out;
	}

	/* Resolves the name being renamed. */
	error = ufs_lookup(old_directory, old_name, &source);
	if (error != 0)
		goto out;

	/* Resolves whatever the new name already refers to, if anything. */
	error = ufs_lookup(new_directory, new_name, &target);
	if (error == 0) {
		target_exists = 1;

		/* Succeeded: both names already refer to the same inode. */
		if (target->i_ino == source->i_ino) {
			error = 0;
			goto out;
		}

		/* A directory may only be renamed onto a directory. */
		if (source->i_type == INODE_DIR &&
		    target->i_type != INODE_DIR) {
			error = ENOTDIR;
			goto out;
		}

		/* A file may only be renamed onto a file. */
		if (source->i_type != INODE_DIR &&
		    target->i_type == INODE_DIR) {
			error = EISDIR;
			goto out;
		}

		/* A directory being replaced has to be empty first. */
		if (target->i_type == INODE_DIR) {
			/*
			 * Asks whether the directory that would be replaced is
			 * empty.
			 */
			empty = directory_empty(target);
			if (empty <= 0) {
				if (empty == 0) {
					/* It still holds names of its own. */
					error = ENOTEMPTY;
				} else {
					/* The scan failed, reported negated. */
					error = -empty;
				}

				goto out;
			}
		}
	} else if (error == ENOENT) {
		/* A name that is not there yet is what a rename wants. */
		error = 0;
	} else {
		goto out;
	}

	/* Renames the name in one group, or reports that it could not. */
	error = rename_group(old_directory, old_name, new_directory, new_name,
			     source, target, &handled);

	/* The journal path has already carried the whole rename out. */
	if (handled)
		goto out;
	old_old_directory_links = old_directory->i_linkcount;
	old_new_directory_links = new_directory->i_linkcount;

	/* Takes the link counts the unwind below has to put back. */
	if (target_exists) {
		old_target_links = target->i_linkcount;
		old_target_flags = target->i_flags;
	}

	/* Drops the link the replaced directory held on its parent. */
	if (target_exists) {
		error = dir_replace(
			new_directory, new_name, (uint32_t)source->i_ino,
			dir_type(source->i_type), &replaced, &replaced_type);
	} else {
		error = dir_add(new_directory, new_name,
				(uint32_t)source->i_ino,
				dir_type(source->i_type));
	}
	if (error != 0)
		goto out;

	/* Takes the old name out of its directory. */
	error = dir_remove(old_directory, old_name, &removed);
	if (error != 0) {
		/* Puts the replaced entry back if the removal failed. */
		if (target_exists) {
			(void)dir_replace(new_directory, new_name, replaced,
					  replaced_type, &ignored,
					  &ignored_type);
		} else {
			(void)dir_remove(new_directory, new_name,
					 &ignored);
		}

		goto out;
	}

	/* The entry that was removed was not the one being renamed. */
	if (removed != (uint32_t)source->i_ino) {
		error = EIO;
		goto out;
	}

	namespace_committed = 1;

	/*
	 * A directory that moved has to point its parent link at the new
	 * parent.
	 */
	if (source->i_type == INODE_DIR && old_directory != new_directory) {
		/* Repoints the parent link of the directory that moved. */
		error = dir_replace(source, &dotdot,
				    (uint32_t)new_directory->i_ino, 4,
				    &old_parent, &old_parent_type);
		if (error != 0)
			goto out;
		dotdot_changed = 1;
		(void)old_parent;
		(void)old_parent_type;
	}

	/* Gives the replaced inode its links back on the unwind path. */
	if (target_exists) {
		mutex_lock(&target->i_lock);

		/* A replaced directory also gets its parent link back. */
		if (target->i_type == INODE_DIR)
			target->i_linkcount = 0;
		else if (target->i_linkcount != 0)
			target->i_linkcount--;
		else
			error = EIO;
		if (error == 0)
			error = persist_inode(target);

		/* A replaced inode with no links left is retired. */
		if (target->i_linkcount == 0)
			target->i_flags |= INODE_DEAD;
		mutex_unlock(&target->i_lock);
	}

	/* A directory that moved changes the link counts of both parents. */
	if (error == 0 && source->i_type == INODE_DIR) {
		/* The old parent loses the link the moved directory held. */
		if (old_directory != new_directory) {
			mutex_lock(&old_directory->i_lock);

			/*
			 * Writes the old parent back with its new link count.
			 */
			if (old_directory->i_linkcount != 0)
				old_directory->i_linkcount--;
			error = persist_inode(old_directory);
			mutex_unlock(&old_directory->i_lock);
			if (error == 0) {
				mutex_lock(&new_directory->i_lock);
				new_directory->i_linkcount++;

				/*
				 * Publishes the replaced inode with its new
				 * link count.
				 */
				if (target_exists &&
				    target->i_type == INODE_DIR &&
				    new_directory->i_linkcount != 0)
					new_directory->i_linkcount--;
				error = persist_inode(new_directory);
				mutex_unlock(&new_directory->i_lock);
			}
		} else if (target_exists && target->i_type == INODE_DIR) {
			mutex_lock(&old_directory->i_lock);

			/*
			 * Writes the old parent back with its new link count.
			 */
			if (old_directory->i_linkcount != 0)
				old_directory->i_linkcount--;
			error = persist_inode(old_directory);
			mutex_unlock(&old_directory->i_lock);
		}
	}

out:
	if (error != 0 && namespace_committed) {
		/*
		 * Puts the parent link back when the rename could not be
		 * finished.
		 */
		if (dotdot_changed) {
			restored = dir_replace(source, &dotdot,
					       (uint32_t)old_directory->i_ino,
					       4, &ignored, &ignored_type);

			/* A failed restore leaves the volume unwritable. */
			if (restored != 0)
				rollback_error = EIO;
		}

		/*
		 * Puts the replaced entry back when the rename could not be
		 * finished.
		 */
		if (target_exists) {
			/* Puts the entry the rename had replaced back. */
			restored = dir_replace(new_directory, new_name,
					       (uint32_t)target->i_ino,
					       dir_type(target->i_type),
					       &ignored, &ignored_type);

			/* A failed restore leaves the volume unwritable. */
			if (restored != 0)
				rollback_error = EIO;
		} else {
			/* Takes the entry the rename had added away again. */
			restored = dir_remove(new_directory, new_name,
					      &ignored);

			/* A failed removal leaves the volume unwritable. */
			if (restored != 0)
				rollback_error = EIO;
		}

		/*
		 * Puts the old name back when the rename could not be finished.
		 */
		restored = dir_add(old_directory, old_name,
				   (uint32_t)source->i_ino,
				   dir_type(source->i_type));

		/* A failed restore leaves the volume unwritable. */
		if (restored != 0)
			rollback_error = EIO;

		/*
		 * Publishes the replaced inode as it stood before the rename.
		 */
		if (target_exists) {
			mutex_lock(&target->i_lock);
			target->i_linkcount = old_target_links;
			target->i_flags = old_target_flags;

			/* Writes the inode back as it stood before. */
			persisted = persist_inode(target);

			/* A failed write leaves the volume unwritable. */
			if (persisted != 0)
				rollback_error = EIO;

			mutex_unlock(&target->i_lock);
		}

		mutex_lock(&old_directory->i_lock);
		old_directory->i_linkcount = old_old_directory_links;

		/* Writes the directory back as it stood before. */
		persisted = persist_inode(old_directory);

		/* A failed write leaves the volume unwritable. */
		if (persisted != 0)
			rollback_error = EIO;

		mutex_unlock(&old_directory->i_lock);

		/* A rename across directories writes both of them back. */
		if (new_directory != old_directory) {
			mutex_lock(&new_directory->i_lock);
			new_directory->i_linkcount = old_new_directory_links;

			/* Writes the directory back as it stood before. */
			persisted = persist_inode(new_directory);

			/* A failed write leaves the volume unwritable. */
			if (persisted != 0)
				rollback_error = EIO;

			mutex_unlock(&new_directory->i_lock);
		}
		if (rollback_error != 0)
			stop_writing(old_directory->i_mount, "a failed rename could not be undone", rollback_error);
	}

	inode_release(target);
	inode_release(source);

	namespace_leave(ms);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Adds a second name for an inode that already has one. */
static int
ufs_link(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target)
{
	struct ufs_mount_state *ms;
	struct inode *existing;
	uint32_t removed;
	int error;
	int rollback_error;
	int handled;

	/* Takes the mount state this call runs against. */
	ms = state(directory->i_mount);

	/* Refuses a link whose target is on another file system. */
	if (target == NULL || target->i_mount != directory->i_mount)
		return EXDEV;

	/* Refuses a link to a directory. */
	if (target->i_type == INODE_DIR)
		return EPERM;
	namespace_enter(ms);

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable) {
		error = EROFS;
		goto out;
	}

	mutex_lock(&target->i_lock);

	/* Refuses a link the target link count could not hold. */
	if (target->i_linkcount == UINT16_MAX) {
		mutex_unlock(&target->i_lock);
		error = EMLINK;
		goto out;
	}

	mutex_unlock(&target->i_lock);

	/* Refuses a name the directory already holds. */
	error = ufs_lookup(directory, name, &existing);
	if (error == 0) {
		inode_release(existing);
		error = EEXIST;
		goto out;
	}
	if (error != ENOENT)
		goto out;

	/* Asks whether the journal path has already carried the link out. */
	error = link_group(directory, name, target, &handled);
	if (handled)
		goto out;

	/* Writes the entry that names the target. */
	error = dir_add(directory, name, (uint32_t)target->i_ino,
			dir_type(target->i_type));
	if (error == 0) {
		/*
		 * inode_link() applies the in-memory increment after this
		 * callback.
		 */
		mutex_lock(&target->i_lock);
		target->i_linkcount++;
		error = persist_inode(target);
		target->i_linkcount--;
		mutex_unlock(&target->i_lock);
		if (error != 0) {
			mutex_lock(&target->i_lock);
			rollback_error = persist_inode(target);
			mutex_unlock(&target->i_lock);
			if (rollback_error == 0) {
				rollback_error =
					dir_remove(directory, name, &removed);
			}
			if (rollback_error != 0)
				stop_writing(directory->i_mount, "a failed link could not be undone", rollback_error);
		}
	}

out:

	namespace_leave(ms);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Creates a symbolic link. */
static int
ufs_symlink(
	struct inode *directory,
	const struct componentname *name,
	const char *target,
	const struct inode_creation_request *request,
	struct inode **result)
{
	struct ufs_mount_state *ms;
	struct inode *existing;
	struct inode *inode;
	size_t length;
	int error;

	ms = state(directory->i_mount);
	length = kern_strlen(target);

	/* Refuses a target longer than this volume can store in an inode. */
	if (length > ms->super.maxsymlinklen || length > 120U) {
		/* Failed. */
		return ENAMETOOLONG;
	}

	*result = NULL;

	namespace_enter(ms);

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable) {
		error = EROFS;
		goto out;
	}

	/* Refuses a name the directory already holds. */
	error = ufs_lookup(directory, name, &existing);
	if (error == 0) {
		inode_release(existing);
		error = EEXIST;
		goto out;
	}
	if (error != ENOENT)
		goto out;

	/* Creates the inode the link will be stored in. */
	error = new_inode(directory, request, 1, &inode);
	if (error)
		goto out;

	inode->i_size = (off_t)length;
	kern_memcpy(info(inode)->shortlink, target, length);

	/* Publishes the inode with the target written into it. */
	error = persist_inode(inode);
	if (error == 0 && inode->i_linkcount == 0) {
		error = creation_publish(directory, name, inode, result);
		goto out;
	}
	if (error == 0)
		error = dir_add(directory, name, (uint32_t)inode->i_ino, 10);
	if (error) {
		error = discard_new_inode_after_error(inode, 0, error);
		goto out;
	}

	*result = inode;

out:

	namespace_leave(ms);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Writes at a position of an inode's own content. */
static ssize_t
pwrite_inode(
	struct inode *inode,
	const void *buffer,
	size_t length,
	off_t offset)
{
	ssize_t written;

	/* Writes through a context of its own. */
	written = pwrite_inode_context(inode, buffer, length, offset, NULL);

	/* Reports how many bytes were written. */
	return written;
}

/* Reads at the file position and advances it. */
static ssize_t
ufs_read(
	struct file *file,
	void *buffer,
	size_t length)
{
	ssize_t n;

	n = pread_inode(file->f_inode, buffer, length, file->f_offset);

	/* A read that moved bytes advances the file offset. */
	if (n > 0)
		file->f_offset += n;

	/* Reports how many bytes were read. */
	return n;
}

/* Reads at a position, without moving the file position. */
static ssize_t
ufs_pread(
	struct file *file,
	void *buffer,
	size_t length,
	off_t offset)
{
	ssize_t read_bytes;

	/* Reads at the offset the caller named. */
	read_bytes = pread_inode(file->f_inode, buffer, length, offset);

	/* Reports how many bytes were read. */
	return read_bytes;
}

/* Writes at the file position and advances it. */
static ssize_t
ufs_write(
	struct file *file,
	const void *buffer,
	size_t length)
{
	ssize_t n;

	n = pwrite_inode(file->f_inode, buffer, length, file->f_offset);

	/* A write that moved bytes advances the file offset. */
	if (n > 0)
		file->f_offset += n;

	/* Reports how many bytes were written. */
	return n;
}

/* Writes at a position, without moving the file position. */
static ssize_t
ufs_pwrite(
	struct file *file,
	const void *buffer,
	size_t length,
	off_t offset)
{
	ssize_t written;

	/* Writes at the offset the caller named. */
	written = pwrite_inode(file->f_inode, buffer, length, offset);

	/* Reports how many bytes were written. */
	return written;
}

/* Writes at a position in the caller's ordering context. */
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
	ssize_t written;
	int error;

	(void)flags;
	(void)credential;

	/* Refuses a context this write could not run under. */
	error = io_context_validate(context);
	if (error != 0)
		return -error;

	/* Writes at the offset the caller named. */
	written = pwrite_inode_context(file->f_inode,
				       buffer,
				       length,
				       offset,
				       context);

	/* Reports how many bytes were written. */
	return written;
}

/* Reads the next directory entry. */
static int
ufs_readdir(
	struct file *file,
	struct dirent *entry,
	int *eof)
{
	uint32_t number;
	uint8_t type;
	char name[NAME_MAX + 1U];
	struct componentname component;
	int hidden;
	int error = next_dirent(file->f_inode,
				&file->f_offset,
				&number,
				&type,
				name);

	/* Steps over the journal file, which the volume keeps to itself. */
	if (error == 0) {
		component.cn_nameptr = name;
		component.cn_namelen = kern_strlen(name);
		component.cn_flags = 0;
		hidden = j3_hidden(file->f_inode, &component);
		if (hidden) {
			error = next_dirent(file->f_inode,
					    &file->f_offset,
					    &number,
					    &type,
					    name);
		}
	}

	/* Succeeded: the walk reached the end of the directory. */
	if (error == ENOENT) {
		*eof = 1;

		/* Succeeded. */
		return 0;
	}

	/* Failed: reports why the entry could not be read. */
	if (error)
		return error;

	/* Renders the record as the entry the caller reads. */
	kern_memset(entry, 0, sizeof(*entry));
	entry->d_ino = number;

	/* The type byte of a record has its own encoding. */
	switch (type) {
	case 1:
		entry->d_type = INODE_FIFO;
		break;
	case 4:
		entry->d_type = INODE_DIR;
		break;
	case 8:
		entry->d_type = INODE_REG;
		break;
	case 10:
		entry->d_type = INODE_SYMLINK;
		break;
	case 12:
		entry->d_type = INODE_SOCKET;
		break;
	default:
		/* A kind this record encoding has no number for. */
		entry->d_type = INODE_NONE;
		break;
	}

	kern_strcpy(entry->d_name, name);
	*eof = 0;

	/* Succeeded. */
	return 0;
}

/* Reads the target a symbolic link names. */
static ssize_t
ufs_readlink(
	struct inode *inode,
	char *buffer,
	size_t length)
{
	ssize_t read_bytes;
	size_t n;
	struct ufs_mount_state *ms;

	/* Takes the mount state this call runs against. */
	ms = state(inode->i_mount);

	/* Refuses an inode that is not a symbolic link. */
	if (inode->i_type != INODE_SYMLINK)
		return -EINVAL;

	/* A short target is stored in the inode instead of in a block. */
	if ((uint64_t)inode->i_size <= ms->super.maxsymlinklen &&
	    inode->i_size <= 120) {
		/* The target is as long as the recorded size says. */
		n = (size_t)inode->i_size;
		if (n > length)
			n = length;
		kern_memcpy(buffer, info(inode)->shortlink, n);

		/* Succeeded: reports how many bytes the target has. */
		return (ssize_t)n;
	}

	/* A longer target is read from the blocks like any other file. */
	read_bytes = pread_inode(inode, buffer, length, 0);

	/* Reports how many bytes were read, or why the read failed. */
	return read_bytes;
}

/* Rounds a length up to the boundary a record starts on. */
static size_t
extattr_align(
	size_t value)
{
	/* Every attribute record starts on an eight-byte boundary. */
	return (value + 7U) & ~(size_t)7U;
}

/* Splits an attribute name into its namespace and the rest. */
static int
extattr_name(
	const char *name,
	uint8_t *name_space,
	const char **stored,
	size_t *stored_length)
{
	const char *part;
	int user_prefix;
	int system_prefix;
	int security_prefix;

	/* Rejects a call that names nothing to split. */
	if (name == NULL || name_space == NULL || stored == NULL ||
	    stored_length == NULL) {
		/* Failed. */
		return EINVAL;
	}

	/* Compares the name against each namespace prefix in turn. */
	user_prefix = kern_strncmp(name, "user.", 5);
	system_prefix = kern_strncmp(name, "system.", 7);
	security_prefix = kern_strncmp(name, "security.", 9);

	/* The prefix decides which namespace the attribute lives in. */
	if (user_prefix == 0) {
		*name_space = UFS_EXTATTR_NAMESPACE_USER;
		part = name + 5;
	} else if (system_prefix == 0) {
		*name_space = UFS_EXTATTR_NAMESPACE_SYSTEM;
		part = name + 7;

		/* A name may not reach the security namespace this way. */
		security_prefix = kern_strncmp(part, "security.", 9);
		if (security_prefix == 0)
			return EINVAL;
	} else if (security_prefix == 0) {
		*name_space = UFS_EXTATTR_NAMESPACE_SYSTEM;
		part = name;
	} else {
		/* Failed. */
		return EOPNOTSUPP;
	}

	*stored_length = kern_strlen(part);

	/* Refuses a name the record header could not hold. */
	if (*stored_length == 0 || *stored_length > 255U)
		return EINVAL;

	*stored = part;

	/* Succeeded. */
	return 0;
}

/* Reads and validates the whole attribute area of an inode. */
static int
extattr_load(
	struct inode *inode,
	uint8_t **result,
	size_t *length)
{
	int error;
	uint32_t record;
	uint8_t name_length;
	uint8_t padding;
	size_t base;
	struct ufs_inode_info *ui;
	struct ufs_mount_state *ms;
	uint8_t *area;
	unsigned block_count;
	unsigned index;
	size_t offset;

	ui = info(inode);
	ms = state(inode->i_mount);
	offset = 0;

	/* Rejects a call that names nowhere to report the area. */
	if (result == NULL || length == NULL)
		return EINVAL;

	*result = NULL;
	*length = ui->extattr_size;

	/* Succeeded: an inode with no attributes has an empty area. */
	if (ui->extattr_size == 0)
		return 0;

	/* Refuses a length no inode could hold in its attribute blocks. */
	block_count = (ui->extattr_size + ms->super.bsize - 1U) / ms->super.bsize;
	if (block_count == 0 || block_count > UFS_NXADDR)
		return EIO;

	/* Takes the staging the whole area is read into. */
	area = kern_calloc(block_count, ms->super.bsize);
	if (area == NULL)
		return ENOMEM;

	/* Reads each attribute block into its place in the area. */
	for (index = 0; index < block_count; index++) {
		/* Reads one attribute block. */
		error = read_block(inode->i_mount, ui->extattr[index],
				   area + index * ms->super.bsize);
		if (error != 0) {
			kern_free(area);

			/* Failed. */
			return error;
		}
	}
	while (offset < ui->extattr_size) {
		/*
		 * A record whose header runs past the area means it is corrupt.
		 */
		if (ui->extattr_size - offset < UFS_EXTATTR_HEADER_SIZE)
			goto invalid;

		record = drv_ufs_get32(area, offset, ms->super.swapped);
		padding = area[offset + 5U];
		name_length = area[offset + 6U];

		/* Measures the record this name occupies. */
		base = extattr_align(UFS_EXTATTR_HEADER_SIZE + name_length);

		/* A record shorter than its header and name is malformed. */
		if (record < base)
			goto invalid;

		/* Every record starts on an eight-byte boundary. */
		if ((record & 7U) != 0)
			goto invalid;

		/* A record may not run past the end of the attribute area. */
		if (record > ui->extattr_size - offset)
			goto invalid;

		/* Nor may its padding claim more than the record has spare. */
		if (padding > record - base)
			goto invalid;

		/* And the namespace has to be one this file system defines. */
		if (area[offset + 4U] < UFS_EXTATTR_NAMESPACE_USER ||
		    area[offset + 4U] > UFS_EXTATTR_NAMESPACE_SYSTEM)
			goto invalid;

		offset += record;
	}

	*result = area;

	/* Succeeded. */
	return 0;

invalid:
	kern_free(area);

	/* Failed. */
	return EIO;
}

/* Finds one attribute record inside a loaded area. */
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
	struct ufs_mount_state *ms;
	size_t offset;
	int difference;

	ms = state(inode->i_mount);
	offset = 0;

	/* Walks the records of the area looking for the name. */
	while (offset < area_length) {
		record = drv_ufs_get32(area, offset, ms->super.swapped);
		disk_name_length = area[offset + 6U];
		base = extattr_align(UFS_EXTATTR_HEADER_SIZE + disk_name_length);

		/*
		 * Compares the record name only when it could possibly match.
		 */
		difference = 1;
		if (area[offset + 4U] == name_space &&
		    disk_name_length == name_length) {
			difference = kern_memcmp(area + offset +
					    UFS_EXTATTR_HEADER_SIZE,
					    name,
					    name_length);
		}

		/* A record matches on its namespace and its name together. */
		if (difference == 0) {
			/* Reports where the record starts. */
			if (at != NULL)
				*at = offset;

			/* Reports how long the whole record is. */
			if (record_length != NULL)
				*record_length = record;

			/* Reports where the value inside it starts. */
			if (content_at != NULL)
				*content_at = offset + base;

			/* Reports how long that value is. */
			if (content_length != NULL)
				*content_length = record - base - area[offset + 5U];

			/* Succeeded. */
			return 0;
		}

		offset += record;
	}

	/* Failed. */
	return ENODATA;
}

/* Writes a new attribute area, moving blocks as that needs. */
static int
extattr_publish(
	struct inode *inode,
	const uint8_t *area,
	size_t length)
{
	struct ufs_inode_info *ui;
	struct ufs_mount_state *ms;
	uint64_t old_ext[UFS_NXADDR];
	uint64_t new_fragment;
	uint64_t old_blocks;
	uint32_t old_size;
	uint8_t *block;
	uint8_t *old_area;
	size_t old_area_length;
	unsigned old_count;
	unsigned index;
	int persisted;
	int error;
	int handled;
	int rollback;

	/* Remembers what the inode held, so the change can be undone. */
	ui = info(inode);
	ms = state(inode->i_mount);
	new_fragment = 0;
	old_size = ui->extattr_size;
	block = NULL;
	old_area = NULL;
	old_area_length = 0;
	error = 0;

	/* Refuses an attribute block larger than the file system uses. */
	if (length > ms->super.bsize)
		return ENOSPC;
	old_ext[0] = ui->extattr[0];
	old_ext[1] = ui->extattr[1];
	old_blocks = ui->blocks;

	/* Counts the blocks the inode currently spends on attributes. */
	if (old_size == 0)
		old_count = 0U;
	else
		old_count = (old_size + ms->super.bsize - 1U) / ms->super.bsize;

	if ((uint64_t)old_count * (ms->super.bsize / UFS_SECTOR_SIZE) > old_blocks) {
		/* Failed. */
		return EIO;
	}

	/* Reads the attribute block that is being replaced. */
	if (old_size != 0)
		error = extattr_load(inode, &old_area, &old_area_length);
	if (error == 0 && old_area_length != old_size)
		error = EIO;
	if (error != 0)
		return error;

	/* Gives up before touching the volume when there is no staging. */
	if (area == NULL)
		length = 0;

	/* An empty attribute releases the blocks instead of writing one. */
	if (length == 0) {
		error = xattr_release_group(inode, &handled);

		/* The journal path has already carried the release out. */
		if (handled) {
			kern_free(old_area);

			/* Failed. */
			return error;
		}

		/* Drops every attribute block the inode used to point at. */
		ui->extattr_size = 0;
		ui->extattr[0] = 0;
		ui->extattr[1] = 0;
		ui->blocks = old_blocks - (uint64_t)old_count * (ms->super.bsize / UFS_SECTOR_SIZE);

		/* Publishes the inode with its attribute blocks gone. */
		error = persist_inode(inode);
		if (error == 0)
			error = order_barrier(inode->i_mount);
		if (error != 0) {
			ui->extattr_size = old_size;
			ui->extattr[0] = old_ext[0];
			ui->extattr[1] = old_ext[1];
			ui->blocks = old_blocks;

			/* Writes the inode back as it stood before. */
			persisted = persist_inode(inode);

			/* A failed write leaves the volume unwritable. */
			if (persisted != 0)
				stop_writing(inode->i_mount, "an inode could not be restored after an extended attribute change", persisted);

			kern_free(old_area);

			/* Failed. */
			return error;
		}

		/* Frees each attribute block the inode has given up. */
		for (index = 0; index < old_count; index++) {
			/* Gives one attribute block back to the volume. */
			rollback = free_block(inode->i_mount,
					      old_ext[index],
					      inode->i_uid,
					      inode->i_gid);

			/* A block that cannot be freed leaves it unwritable. */
			if (rollback != 0) {
				stop_writing(inode->i_mount, "an extended attribute block could not be freed", rollback);
				error = rollback;
				break;
			}
		}

		kern_free(old_area);

		/* Failed. */
		return error;
	}

	error = xattr_existing_group(inode, area, length, &handled);

	/* The journal path has already carried the publication out. */
	if (handled) {
		kern_free(old_area);

		/* Failed. */
		return error;
	}

	/* A first attribute on a journalled volume is published as a group. */
	if (ms->journal_enabled && old_size == 0) {
		error = xattr_allocate_group(inode, area, length, &handled);

		/* The journal path has already carried the publication out. */
		if (handled) {
			kern_free(old_area);

			/* Failed. */
			return error;
		}
	}

	/* Builds the block the attribute will live in. */
	block = kern_calloc(1, ms->super.bsize);
	if (block == NULL) {
		kern_free(old_area);

		/* Failed. */
		return ENOMEM;
	}

	kern_memcpy(block, area, length);

	/* A first attribute needs a block of its own. */
	if (old_ext[0] == 0) {
		/* Takes the block the attribute will live in. */
		error = allocate_block(inode->i_mount,
				       inode->i_uid,
				       inode->i_gid,
				       &new_fragment);
		if (error != 0)
			goto out;
	} else {
		new_fragment = old_ext[0];
	}

	/* Writes the attribute out before the inode names it. */
	error = write_block(inode->i_mount, new_fragment, block);
	if (error != 0)
		goto rollback_data;

	ui->extattr_size = (uint32_t)length;
	ui->extattr[0] = new_fragment;
	ui->extattr[1] = 0;
	ui->blocks = old_blocks - (uint64_t)old_count * (ms->super.bsize / UFS_SECTOR_SIZE) + ms->super.bsize / UFS_SECTOR_SIZE;

	/* Publishes the inode pointing at the new block. */
	error = persist_inode(inode);
	if (error == 0)
		error = order_barrier(inode->i_mount);
	if (error != 0)
		goto rollback_metadata;

	/* Frees the blocks the old attribute spanned beyond the first. */
	if (old_count > 1U) {
		/* Gives the second attribute block back to the volume. */
		rollback = free_block(inode->i_mount, old_ext[1],
				      inode->i_uid, inode->i_gid);

		/* A block that cannot be freed leaves it unwritable. */
		if (rollback != 0) {
			stop_writing(inode->i_mount, "the second extended attribute block could not be freed", rollback);
			error = rollback;
		}
	}

	goto out;

rollback_metadata:
	ui->extattr_size = old_size;
	ui->extattr[0] = old_ext[0];
	ui->extattr[1] = old_ext[1];
	ui->blocks = old_blocks;

	/* Puts the inode back the way the failed publication found it. */
	rollback = persist_inode(inode);
	if (rollback == 0)
		rollback = order_barrier(inode->i_mount);

	/* A failed restore leaves the volume unwritable. */
	if (rollback != 0) {
		/*
		 * The new pointer may still be committed: keep its allocation.
		 */
		stop_writing(inode->i_mount, "an extended attribute area could not be restored", rollback);
		goto out;
	}

rollback_data:
	/* A block this publication allocated is given back. */
	if (old_ext[0] == 0) {
		if (ms->writable && new_fragment != 0) {
			/* Gives the block this publication took back. */
			rollback = free_block(inode->i_mount, new_fragment,
					      inode->i_uid, inode->i_gid);

			/* A block that cannot be freed leaves it unwritable. */
			if (rollback != 0)
				stop_writing(inode->i_mount, "a new extended attribute block could not be freed", rollback);
		}
	} else if (old_area != NULL) {
		/* Puts the previous attribute area back where it was. */
		rollback = write_block(inode->i_mount, old_ext[0], old_area);

		/* A write that fails leaves the volume unwritable. */
		if (rollback != 0)
			stop_writing(inode->i_mount, "the previous extended attribute area could not be written back", rollback);
	}

out:
	kern_free(old_area);
	kern_free(block);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reads the value of one extended attribute. */
static ssize_t
ufs_getxattr(
	struct inode *inode,
	const char *name,
	void *value,
	size_t size)
{
	uint8_t name_space;
	uint8_t *area = NULL;
	const char *stored;
	size_t stored_length;
	size_t area_length;
	size_t content_at;
	size_t content_length;
	int error;

	/* Splits the name into the namespace and the stored form. */
	error = extattr_name(name, &name_space, &stored, &stored_length);
	if (error != 0)
		return -error;

	mutex_lock(&inode->i_lock);

	/* Reads the attribute area as it stands. */
	error = extattr_load(inode, &area, &area_length);
	if (error == 0) {
		error = extattr_find(inode, area, area_length, name_space,
				     stored, stored_length, NULL, NULL,
				     &content_at, &content_length);
	}
	if (error == 0 && value != NULL && size < content_length)
		error = ERANGE;
	if (error == 0 && value != NULL && content_length != 0)
		kern_memcpy(value, area + content_at, content_length);

	mutex_unlock(&inode->i_lock);

	kern_free(area);

	/*
	 * Failed: reports the error as a negative value, as the caller expects.
	 */
	if (error != 0)
		return -(ssize_t)error;

	/* Succeeded: reports how many bytes the attribute holds. */
	return (ssize_t)content_length;
}

/* Writes, adds or replaces one extended attribute. */
static int
ufs_setxattr(
	struct inode *inode,
	const char *name,
	const void *value,
	size_t size,
	unsigned flags)
{
	struct ufs_mount_state *ms;
	uint8_t name_space;
	uint8_t *area;
	uint8_t *updated;
	const char *stored;
	size_t stored_length;
	size_t area_length;
	size_t at;
	size_t old_record;
	size_t replaced;
	size_t base;
	size_t new_record;
	size_t new_length;
	size_t padding;
	int found;
	int error;

	/* Starts with no area read and no record found in one. */
	ms = state(inode->i_mount);
	area = NULL;
	updated = NULL;
	area_length = 0;
	at = 0;
	old_record = 0;

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable)
		return EROFS;

	/* Rejects a call that names a length but no value. */
	if (value == NULL && size != 0)
		return EINVAL;

	/* Splits the name into the namespace and the stored form. */
	error = extattr_name(name, &name_space, &stored, &stored_length);
	if (error != 0)
		return error;

	/* Measures the record this attribute will occupy. */
	base = extattr_align(UFS_EXTATTR_HEADER_SIZE + stored_length);
	if (size > ms->super.bsize || base > ms->super.bsize - size)
		return E2BIG;

	new_record = extattr_align(base + size);
	padding = new_record - base - size;
	mutex_lock(&inode->i_lock);

	/* Reads the attribute area as it stands. */
	error = extattr_load(inode, &area, &area_length);
	if (error != 0)
		goto out;

	/* Looks for a record this name already has. */
	found = extattr_find(inode, area, area_length, name_space, stored,
			     stored_length, &at, &old_record, NULL, NULL) == 0;
	if ((flags & INODE_XATTR_CREATE) != 0 && found) {
		error = EEXIST;
		goto out;
	}

	/* A replace of a name that is not there fails. */
	if ((flags & INODE_XATTR_REPLACE) != 0 && !found) {
		error = ENODATA;
		goto out;
	}

	/* A record that is being replaced gives its bytes back first. */
	replaced = 0U;
	if (found)
		replaced = old_record;

	/* Measures the area once the record is added or replaced. */
	new_length = area_length - replaced + new_record;
	if (new_length > ms->super.bsize) {
		error = ENOSPC;
		goto out;
	}

	/* Takes the staging the new area is assembled in. */
	updated = kern_calloc(1, ms->super.bsize);
	if (updated == NULL) {
		error = ENOMEM;
		goto out;
	}

	/* Copies the records that precede the one being written. */
	if (at != 0)
		kern_memcpy(updated, area, at);

	drv_ufs_put32(updated, at, (uint32_t)new_record, ms->super.swapped);
	updated[at + 4U] = name_space;
	updated[at + 5U] = (uint8_t)padding;
	updated[at + 6U] = (uint8_t)stored_length;
	kern_memcpy(updated + at + UFS_EXTATTR_HEADER_SIZE, stored, stored_length);

	/* Writes the new record into the staging. */
	if (size != 0)
		kern_memcpy(updated + at + base, value, size);

	/* Copies the records that follow the one being written. */
	if (area_length > at + replaced) {
		kern_memcpy(updated + at + new_record, area + at + replaced,
		       area_length - at - replaced);
	}

	error = extattr_publish(inode, updated, new_length);

out:
	mutex_unlock(&inode->i_lock);

	kern_free(updated);
	kern_free(area);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the names of every attribute an inode carries. */
static ssize_t
ufs_listxattr(
	struct inode *inode,
	char *list,
	size_t size)
{
	uint32_t record;
	uint8_t ns;
	uint8_t nlen;
	const char *prefix;
	const uint8_t *disk_name;
	size_t prefix_length;
	struct ufs_mount_state *ms;
	uint8_t *area;
	size_t area_length;
	size_t offset;
	size_t needed;
	int security;
	int error;

	ms = state(inode->i_mount);
	area = NULL;
	offset = 0;
	needed = 0;

	mutex_lock(&inode->i_lock);

	/* Reads the attribute area as it stands. */
	error = extattr_load(inode, &area, &area_length);
	if (error != 0)
		goto out;

	/* Walks the records of the area. */
	while (offset < area_length) {
		record = drv_ufs_get32(area, offset, ms->super.swapped);
		ns = area[offset + 4U];
		nlen = area[offset + 6U];

		disk_name = area + offset + UFS_EXTATTR_HEADER_SIZE;

		/*
		 * Only the user namespace is reported through this interface.
		 */

		/* Compares the stored name against the security prefix. */
		security = 1;
		if (nlen >= 9U)
			security = kern_memcmp(disk_name, "security.", 9);

		/* The prefix a name is reported with follows its namespace. */
		if (ns == UFS_EXTATTR_NAMESPACE_USER) {
			prefix = "user.";
			prefix_length = 5U;
		} else if (security == 0) {
			prefix = "";
			prefix_length = 0;
		} else {
			prefix = "system.";
			prefix_length = 7U;
		}

		/* Refuses to write past the end of the caller buffer. */
		if (list != NULL &&
		    (needed > size ||
		     prefix_length + nlen + 1U > size - needed)) {
			error = ERANGE;
			goto out;
		}

		/* Copies the name out when the caller asked for the names. */
		if (list != NULL) {
			kern_memcpy(list + needed, prefix, prefix_length);
			kern_memcpy(list + needed + prefix_length, disk_name, nlen);
			list[needed + prefix_length + nlen] = '\0';
		}

		needed += prefix_length + nlen + 1U;
		offset += record;
	}

out:
	mutex_unlock(&inode->i_lock);

	kern_free(area);

	/*
	 * Failed: reports the error as a negative value, as the caller expects.
	 */
	if (error != 0)
		return -(ssize_t)error;

	/* Succeeded: reports how much room the list needs. */
	return (ssize_t)needed;
}

/* Takes one extended attribute away. */
static int
ufs_removexattr(
	struct inode *inode,
	const char *name)
{
	struct ufs_mount_state *ms;
	uint8_t name_space;
	uint8_t *area;
	uint8_t *updated;
	const char *stored;
	size_t stored_length;
	size_t area_length;
	size_t at;
	size_t record;
	size_t new_length;
	int error;

	ms = state(inode->i_mount);
	area = NULL;
	updated = NULL;

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable)
		return EROFS;

	/* Splits the name into the namespace and the stored form. */
	error = extattr_name(name, &name_space, &stored, &stored_length);
	if (error != 0)
		return error;
	mutex_lock(&inode->i_lock);

	/* Reads the attribute area as it stands. */
	error = extattr_load(inode, &area, &area_length);
	if (error != 0)
		goto out;

	/* Refuses a name the area does not hold. */
	error = extattr_find(inode, area, area_length, name_space, stored,
			     stored_length, &at, &record, NULL, NULL);
	if (error != 0)
		goto out;

	/* Measures the area once the record is gone. */
	new_length = area_length - record;
	if (new_length != 0) {
		/* Takes the staging the new area is assembled in. */
		updated = kern_calloc(1, ms->super.bsize);
		if (updated == NULL) {
			error = ENOMEM;
			goto out;
		}

		/* Copies the records that precede the one being removed. */
		if (at != 0)
			kern_memcpy(updated, area, at);

		/* Copies the records that follow the one being removed. */
		if (area_length > at + record) {
			kern_memcpy(updated + at, area + at + record,
			       area_length - at - record);
		}
	}

	error = extattr_publish(inode, updated, new_length);

out:
	mutex_unlock(&inode->i_lock);

	kern_free(updated);
	kern_free(area);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports what a caller may know about an inode. */
static int
ufs_getattr(
	struct inode *inode,
	struct stat *status)
{
	struct ufs_inode_info *ui;

	/* Takes the UFS half of the inode. */
	ui = info(inode);

	/* Renders the in-core inode as the attributes a caller sees. */
	kern_memset(status, 0, sizeof(*status));
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

	/* Preserve the precision needed by stat-based attribute copying. */
	status->st_atim.tv_nsec = inode->i_atime.tv_nsec;
	status->st_mtim.tv_nsec = inode->i_mtime.tv_nsec;
	status->st_ctim.tv_nsec = inode->i_ctime.tv_nsec;

	status->st_blksize = state(inode->i_mount)->super.bsize;
	status->st_blocks = ui->blocks;

	/* Succeeded. */
	return 0;
}

/* Tests whether a time is one the on-disk format can hold. */
static int
valid_disk_time(
	time_t seconds,
	long nanoseconds)
{
	(void)seconds;

	/* A negative fraction of a second names no time. */
	if (nanoseconds < 0)
		return 0;

	/* Nor does one that is a whole second or more. */
	if (nanoseconds >= 1000000000L)
		return 0;

	/* Reports that the fraction is one a timestamp can hold. */
	return 1;
}

/* Changes the mode, owner or times of an inode. */
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
	struct inode_time old_atime;
	struct inode_time old_mtime;
	struct inode_time old_ctime;
	struct ufs_mount_state *ms;
	long atime_nsec = 0, mtime_nsec = 0, ctime_nsec = 0;
	int representable;
	int error;
	int quota_moved = 0;

	kern_memset(&quota_transfer_state, 0, sizeof(quota_transfer_state));

	atime_nsec = status->st_atim.tv_nsec;
	mtime_nsec = status->st_mtim.tv_nsec;
	ctime_nsec = status->st_ctim.tv_nsec;

	/* Refuses an access time this on-disk format cannot hold. */
	if ((mask & INODE_ATTR_ATIME) != 0) {
		representable = valid_disk_time(status->st_atime, atime_nsec);
		if (!representable) {
			/* Failed. */
			return EOVERFLOW;
		}
	}

	/* Refuses a modification time this on-disk format cannot hold. */
	if ((mask & INODE_ATTR_MTIME) != 0) {
		representable = valid_disk_time(status->st_mtime, mtime_nsec);
		if (!representable) {
			/* Failed. */
			return EOVERFLOW;
		}
	}

	/* Refuses a change time this on-disk format cannot hold. */
	if ((mask & INODE_ATTR_CTIME) != 0) {
		representable = valid_disk_time(status->st_ctime, ctime_nsec);
		if (!representable) {
			/* Failed. */
			return EOVERFLOW;
		}
	}

	/* A size change is carried out by the truncate path first. */
	if ((mask & INODE_ATTR_SIZE) != 0) {
		/* Truncates or extends the file to the requested size. */
		error = ufs_truncate(inode, status->st_size);
		if (error != 0)
			return error;
	}

	mutex_lock(&inode->i_lock);

	/* Takes the mount state this inode belongs to. */
	ms = state(inode->i_mount);

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable) {
		mutex_unlock(&inode->i_lock);

		/* Failed. */
		return EROFS;
	}

	/* Remembers what the inode held, so the change can be undone. */
	old_mode = inode->i_mode;
	old_uid = inode->i_uid;
	old_gid = inode->i_gid;
	old_atime = inode->i_atime;
	old_mtime = inode->i_mtime;
	old_ctime = inode->i_ctime;

	/* A change of owner moves the file between quota accounts. */
	if ((mask & (INODE_ATTR_UID | INODE_ATTR_GID)) != 0) {
		/* Only the identities the mask names actually change. */
		new_uid = old_uid;
		if ((mask & INODE_ATTR_UID) != 0)
			new_uid = status->st_uid;

		new_gid = old_gid;
		if ((mask & INODE_ATTR_GID) != 0)
			new_gid = status->st_gid;

		/*
		 * Opens the quota transfer, which the publication below
		 * commits.
		 */
		error = quota_transfer_begin(
			&state(inode->i_mount)->quota, old_uid, old_gid,
			new_uid, new_gid,
			info(inode)->blocks /
			(state(inode->i_mount)->super.bsize /
			 UFS_SECTOR_SIZE),
			1, quota_now(), &quota_transfer_state);
		if (error != 0) {
			mutex_unlock(&inode->i_lock);

			/* Failed. */
			return error;
		}

		quota_moved = old_uid != new_uid || old_gid != new_gid;
	}

	/* Applies the requested permission bits. */
	if (mask & INODE_ATTR_MODE) {
		inode->i_mode =
			(inode->i_mode & S_IFMT) | (status->st_mode & ~S_IFMT);
	}

	/* Applies the requested owner. */
	if (mask & INODE_ATTR_UID)
		inode->i_uid = status->st_uid;

	/* Applies the requested group. */
	if (mask & INODE_ATTR_GID)
		inode->i_gid = status->st_gid;

	/* Applies the requested access time. */
	if (mask & INODE_ATTR_ATIME) {
		inode->i_atime.tv_sec = status->st_atime;
		inode->i_atime.tv_nsec = atime_nsec;
	}

	/* Applies the requested modification time. */
	if (mask & INODE_ATTR_MTIME) {
		inode->i_mtime.tv_sec = status->st_mtime;
		inode->i_mtime.tv_nsec = mtime_nsec;
	}

	/* Applies the requested change time. */
	if (mask & INODE_ATTR_CTIME) {
		inode->i_ctime.tv_sec = status->st_ctime;
		inode->i_ctime.tv_nsec = ctime_nsec;
	}

	/* Publishes the inode with the new attributes. */
	error = persist_inode(inode);
	if (error != 0) {
		/*
		 * Closes the quota transfer, keeping it only if the write
		 * succeeded.
		 */
		if (quota_moved)
			quota_transfer_rollback(&quota_transfer_state);

		/* A change that failed puts every field back as it was. */
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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Writes one inode back to the volume. */
static int
ufs_inode_sync(
	struct inode *inode)
{
	struct ufs_mount_state *ms;
	int error;

	mutex_lock(&inode->i_lock);

	/*
	 * A retired or not-yet-bound cache object has no persistent inode
	 * identity.
	 */
	if (inode->i_ino == 0) {
		mutex_unlock(&inode->i_lock);

		/* Succeeded. */
		return 0;
	}

	/* Takes the mount state this inode belongs to. */
	ms = state(inode->i_mount);
	if (ms->writable) {
		/* Writes the inode back to the volume. */
		error = persist_inode(inode);
	} else if ((inode->i_mount->m_flags & MOUNT_READ_ONLY) != 0) {
		/* A volume mounted read-only was never going to be written. */
		error = 0;
	} else {
		/* The volume lost write access after it was mounted. */
		error = EROFS;
	}

	mutex_unlock(&inode->i_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

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
	int used;
	int error;
	int quota_error;

	/*
	 * Refuses reuse while any persistent block owner or namespace link
	 * remains.
	 */
	ms = state(inode->i_mount);
	ui = info(inode);

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable)
		return EROFS;

	/*
	 * Refuses to retire the root, or an inode number the volume has not
	 * got.
	 */
	if (inode->i_ino <= UFS_ROOT_INO)
		return EIO;	/* Failed. */

	/* Nor one the volume never had an inode number for. */
	if ((uint64_t)inode->i_ino >= (uint64_t)ms->super.ncg * ms->super.ipg)
		return EIO;	/* Failed. */

	/* An inode a name still points at is not finished with. */
	if (inode->i_linkcount != 0)
		return EIO;	/* Failed. */

	/* Nor is one that still has content the caller did not truncate. */
	if (inode->i_size != 0 || ui->blocks != 0)
		return EIO;	/* Failed. */

	/* Nor one whose attributes still occupy a block. */
	if (ui->extattr_size != 0)
		return EIO;	/* Failed. */

	/* Refuses an inode that still names direct blocks. */
	for (n = 0; n < UFS_NDADDR; n++) {
		/*
		 * A direct pointer that is still filled means the file was not
		 * truncated.
		 */
		if (ui->direct[n] != 0)
			return EIO;
	}

	/* Refuses an inode that still names indirect blocks. */
	for (n = 0; n < UFS_NIADDR; n++) {
		/* An indirect pointer that is still filled means the same. */
		if (ui->indirect[n] != 0)
			return EIO;
	}

	/* Refuses an inode that still names attribute blocks. */
	for (n = 0; n < UFS_NXADDR; n++) {
		/* An attribute pointer that is still filled means the same. */
		if (ui->extattr[n] != 0)
			return EIO;
	}

	/*
	 * Copies the allocated inode map and validates totals before private
	 * edits.
	 */
	cg = inode->i_ino / ms->super.ipg;
	local = inode->i_ino % ms->super.ipg;

	/* Reads the cylinder group the inode number lives in. */
	error = load_cg_locked(inode->i_mount, cg);
	if (error != 0)
		return error;

	/* Asks the used map whether the number is still handed out. */
	used = bit_test(ms->cg + ms->cg_iusedoff, local);

	/* An inode the group already calls free must not be retired twice. */
	if (!used)
		return EIO;

	free_inodes = drv_ufs_get32(ms->cg, UFS_CG_NIFREE, ms->super.swapped);
	directories = drv_ufs_get32(ms->cg, UFS_CG_NDIR, ms->super.swapped);

	/* A directory also comes off the count its group keeps. */
	is_directory = inode->i_type == INODE_DIR;

	/* A group that already calls every inode free cannot free another. */
	if (free_inodes >= ms->super.ipg)
		return EIO;	/* Failed. */

	/* Nor can the volume total be raised any further. */
	if (ms->super.cstotal_nifree == UINT64_MAX)
		return EIO;	/* Failed. */

	/* A directory count of zero has no directory left to take away. */
	if (is_directory && (directories == 0 || ms->super.cstotal_ndir == 0))
		return EIO;	/* Failed. */

	kern_memcpy(group->cg, ms->cg, ms->super.bsize);
	kern_memcpy(&group->image, ui, sizeof(group->image));
	group->image.inode.i_mode = 0;
	group->image.inode.i_type = INODE_NONE;

	/* Stages the emptied inode. */
	error = prepare_inode_locked(&group->image.inode, group->dinode,
				     &fragment);
	if (error != 0)
		return error;

	/* Stages the superblock summaries the retirement changes. */
	error = prepare_super_summaries(inode->i_mount, group->summaries);
	if (error != 0)
		return error;

	/*
	 * Makes the inode reusable only in the same group that retires its old
	 * kind.
	 */
	bit_clear(group->cg + ms->cg_iusedoff, local);
	drv_ufs_put32(group->cg, UFS_CG_NIFREE, free_inodes + 1U,
		      ms->super.swapped);
	drv_ufs_put64(group->summaries, UFS_FS_CSTOTAL_NIFREE,
		      ms->super.cstotal_nifree + 1U, ms->super.swapped);

	/* Stages the directory count the group keeps. */
	if (is_directory) {
		drv_ufs_put32(group->cg, UFS_CG_NDIR, directories - 1U,
			      ms->super.swapped);
		drv_ufs_put64(group->summaries, UFS_FS_CSTOTAL_NDIR,
			      ms->super.cstotal_ndir - 1U, ms->super.swapped);
	}

	/* Names the group counts, the summaries and the inode block. */
	extents[0].target = (cgstart(&ms->super, cg) + ms->super.cblkno) << ms->super.fsbtodb;
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
	error = metadata_group_commit(inode->i_mount, extents, 3, NULL, &outcome);

	/*
	 * Publishes positive retirement and releases quota once, even on
	 * recovered error.
	 */
	if (outcome.committed) {
		kern_memcpy(ms->cg, group->cg, ms->super.bsize);
		ms->super.cstotal_nifree++;

		/* Publishes the directory count the group keeps. */
		if (is_directory)
			ms->super.cstotal_ndir--;

		inode->i_mode = 0;
		inode->i_type = INODE_NONE;

		/*
		 * Prevents final-reference retry from writing an already
		 * reusable identity.
		 */
		inode->i_ino = 0;

		/* Gives the inode back to the owner quota. */
		quota_error = quota_release(&ms->quota, inode->i_uid,
					    inode->i_gid, 0, 1);
		if (quota_error != 0) {
			stop_writing(inode->i_mount, "a retired inode could not be credited to its quota", quota_error);

			/*
			 * Only a committed retirement may give the quota back.
			 */
			if (error == 0)
				error = quota_error;
		}
	}

	/* An uncertain group leaves the volume unwritable. */
	if (outcome.committed || outcome.uncertain)
		ms->cg_dirty = outcome.uncertain;

	/*
	 * Preserves the original failure independently of established
	 * retirement.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Owns private retirement storage and exclusion after data and xattr teardown.
 */
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

	/* Sizes the staging from the images the group will hold. */
	bytes = 2U * ms->super.bsize + UFS_SBLOCK_SIZE;
	if (!ms->journal_enabled || ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
		/* Succeeded. */
		return 0;
	}
	*handled = 1;

	/* Takes the staging the whole retirement is assembled in. */
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(bytes);

	/* Gives up before touching the volume when there is no staging. */
	if (group->memory == NULL) {
		kern_free(group);

		/* Failed. */
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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Releases every owner of an unlinked inode and reports the first failure. */
static int
reclaim_unlinked_inode(
	struct inode *inode)
{
	struct ufs_inode_info *ui;
	struct ufs_mount_state *ms;
	int handled;
	int error;

	/* Requires a nonreserved, unlinked identity on a writable mount. */
	ui = info(inode);

	/* Only an unlinked inode other than the root is reclaimed. */
	if (inode->i_linkcount != 0 || inode->i_ino <= UFS_ROOT_INO)
		return EINVAL;

	/* Takes the mount state this inode belongs to. */
	ms = state(inode->i_mount);

	/* Refuses to write to a volume that is no longer writable. */
	if (!ms->writable)
		return EROFS;

	/*
	 * Keeps every remaining reference reachable until its own release
	 * commits.
	 */

	/* Frees every block the file still holds. */
	error = ufs_truncate(inode, 0);
	if (error != 0)
		return error;
	mutex_lock(&inode->i_lock);

	error = extattr_publish(inode, NULL, 0);

	mutex_unlock(&inode->i_lock);

	/* Failed: the inode keeps its blocks until they can be freed. */
	if (error != 0)
		return error;

	/* Asks whether the journal path has already retired the inode. */
	error = retire_inode_group(inode, &handled);
	if (handled)
		return error;

	/*
	 * Preserves the ordered retirement path for profiles outside group
	 * admission.
	 */
	if (inode->i_type == INODE_DIR) {
		/* A directory also comes off the count its group keeps. */
		error = adjust_directory_count(inode->i_mount,
					       (uint32_t)inode->i_ino, -1);
		if (error != 0)
			return error;
	}

	inode->i_mode = 0;
	inode->i_type = INODE_NONE;
	ui->blocks = 0;

	/* Publishes the emptied inode. */
	error = persist_inode(inode);
	if (error != 0)
		return error;

	/* The reclaim is only durable once the device has it. */
	error = order_barrier(inode->i_mount);
	if (error != 0)
		return error;
	error = free_inode_number(inode->i_mount, (uint32_t)inode->i_ino,
				  inode->i_uid, inode->i_gid);

	/*
	 * Returns actual retirement failure to explicit cleanup and recovery
	 * callers.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Preserves the VFS final-reference callback while sharing checked reclamation.
 */
static void
ufs_reclaim(
	struct inode *inode)
{
	struct ufs_mount_state *ms;

	/* Takes the mount state this inode belongs to. */
	ms = state(inode->i_mount);

	/*
	 * Ignores identities whose lifetime does not permit filesystem
	 * retirement.
	 */
	if (inode->i_linkcount != 0 || inode->i_ino <= UFS_ROOT_INO ||
	    !ms->writable) {
		/* Nothing more can be done once the volume is unwritable. */
		return;
	}

	/*
	 * The callback has no errno channel; explicit owners call the checked
	 * helper.
	 */
	(void)reclaim_unlinked_inode(inode);
}

/*
 * Marks a confirmed unpublished inode unlinked without discarding its
 * resources.
 */
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

	/* Takes the mount the unwind runs against. */
	ms = state(inode->i_mount);
	if (inode->i_ino <= UFS_ROOT_INO)
		return EINVAL;

	/* Takes the staging the inode image is assembled in. */
	image = kern_malloc(sizeof(*image) + ms->super.bsize);
	if (image == NULL)
		return ENOMEM;
	block = (uint8_t *)(image + 1);
	kern_memset(&outcome, 0, sizeof(outcome));
	mutex_lock(&inode->i_lock);
	mutex_lock(&ms->lock);

	kern_memcpy(image, info(inode), sizeof(*image));
	image->inode.i_linkcount = 0;

	/* A writable volume publishes the unwind; a read-only one cannot. */
	if (ms->writable)
		error = prepare_inode_locked(&image->inode, block, &fragment);
	else
		error = EROFS;

	/* Publishes the emptied inode as one group of its own. */
	if (error == 0) {
		extent.target = fragment << ms->super.fsbtodb;
		extent.sectors = ms->super.bsize / UFS_SECTOR_SIZE;
		extent.payload = block;
		error = metadata_group_commit(inode->i_mount, &extent, 1, NULL,
					      &outcome);
	}

	/*
	 * Makes a proven zero-link owner eligible for checked resource
	 * reclamation.
	 */
	if (outcome.committed) {
		inode->i_linkcount = 0;
		inode->i_flags |= INODE_DEAD;
	}

	mutex_unlock(&ms->lock);
	mutex_unlock(&inode->i_lock);

	kern_free(image);

	/*
	 * Retains references and the original errno on an unsuccessful
	 * transition.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Discards an unpublished journal-backed creation through checked release
 * owners.
 */
static int
discard_reserved_inode(
	struct inode *inode)
{
	int error;

	/*
	 * Detaches borrowed endpoints before any final-reference destruction is
	 * possible.
	 */
	detach_new_socket_special(inode);

	/* Undoes the reservation as one journal group. */
	error = creation_unlink_group(inode);
	if (error == 0)
		error = reclaim_unlinked_inode(inode);
	if (error == 0) {
		inode->i_ino = 0;
		inode->i_flags |= INODE_DEAD;
	}

	inode_release(inode);

	/*
	 * Reports incomplete cleanup without hiding which persistent owners
	 * remain.
	 */

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Makes everything a file has written durable (BUG-307).
 *
 * On a volume whose metadata the batched journal carries, only the file's
 * own blocks are written from the cache, and then the running transaction
 * is committed, which holds the file's inode, its indirect blocks and the
 * allocation records of its blocks.  The commit writes, before it seals,
 * the content that waits in the cache, so that no block a commit makes
 * reachable is read back with its old owner's bytes; past that point the
 * sync writes nothing of the other files, whose later writes wait for the
 * flusher or the next commit instead of being chased through the whole
 * cache again and again.  A namespace change made before the sync (a new
 * name, a rename) is in the same transaction and is committed with it.  A
 * volume without the batched journal is synced whole, as before.
 */
static int
ufs_file_sync(
	struct file *file)
{
	struct inode *inode;
	struct mount *mountp;
	struct ufs_mount_state *ms;
	int journaled;
	int error;

	/* A call without a file has nothing to flush. */
	if (file == NULL)
		return EINVAL;
	inode = file->f_inode;
	mountp = inode->i_mount;

	/* Puts the inode as it now stands into the volume's metadata. */
	error = inode_sync(inode);
	if (error != 0)
		return error;

	/* Reads whether the batched journal carries the volume's metadata. */
	ms = state(mountp);
	mutex_lock(&ms->lock);

	journaled = ms->j3.active;

	mutex_unlock(&ms->lock);

	/*
	 * Without the batched journal the metadata is written where it lives
	 * or through the tail journal, which a whole sync drains; a volume
	 * that writes through has little left for it to write.
	 */
	if (!journaled) {
		error = ufs_sync(mountp);
		if (error != 0)
			return error;

		/* Succeeded: the volume is durable, and the file with it. */
		return 0;
	}

	/*
	 * Writes the file's own blocks that wait in the cache, before the
	 * commit seals the metadata that names them.
	 */
	mutex_lock(&inode->i_lock);

	error = file_writeback_locked(inode);

	mutex_unlock(&inode->i_lock);

	/* Reports why the file's blocks could not be written. */
	if (error != 0)
		return error;

	/* Commits the running transaction, and again while a held home is owed. */
	error = ufs_sync_journal(ms, mountp);
	if (error != 0)
		return error;

	/*
	 * Flushes the device: the commit's seal made the file's blocks and its
	 * metadata durable, and the content a commit holds in a block freed
	 * before it reaches its home only after the seal.
	 */
	error = bio_flush(mountp->m_disk);
	if (error != 0)
		return error;

	/* Succeeded: the file's data and metadata are on the device. */
	return 0;
}

/*
 * Writes the blocks of a regular file that wait in the cache, the inode's
 * lock held so that the block map holds still (BUG-307).  The indirect
 * blocks are metadata, which the journal carries, and are not written here.
 */
static int
file_writeback_locked(
	struct inode *inode)
{
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	struct ufs_sync_run run;
	uint8_t sectors[UFS_NIADDR * UFS_SECTOR_SIZE];
	uint64_t blocks;
	uint64_t direct_blocks;
	uint64_t span;
	uint64_t used;
	uint64_t n;
	unsigned level;
	int error;

	/*
	 * Only a regular file keeps content in its blocks; a directory's
	 * blocks are metadata.  An inode without a home on the volume has no
	 * blocks of its own to write.
	 */
	if (inode->i_type != INODE_REG)
		return 0;
	if (inode->i_ino == 0)
		return 0;

	/* Counts the blocks the file's size reaches. */
	ms = state(inode->i_mount);
	ui = info(inode);
	blocks = 0;
	if (inode->i_size > 0)
		blocks = ((uint64_t)inode->i_size - 1U) / ms->super.bsize + 1U;

	/* Starts with no run gathered. */
	run.mountp = inode->i_mount;
	run.lba = 0;
	run.count = 0;

	/* Gathers the blocks the inode names itself. */
	direct_blocks = blocks;
	if (direct_blocks > UFS_NDADDR)
		direct_blocks = UFS_NDADDR;
	error = 0;
	for (n = 0; n < direct_blocks && error == 0; n++) {
		/* A hole has no block to write. */
		if (ui->direct[n] != 0)
			error = sync_run_add(&run, ui->direct[n]);
	}

	/* Reports why a direct block could not be gathered. */
	if (error != 0)
		return error;

	/* A file of direct blocks only has nothing more to walk. */
	blocks -= direct_blocks;
	if (blocks != 0) {
		/*
		 * Walks each indirect level the size reaches, each nindir times
		 * wider, reading entries into a sector of each level on the
		 * stack, so that a sync has no allocation to fail.
		 */
		span = ms->super.nindir;
		for (level = 0; level < UFS_NIADDR && blocks != 0 && error == 0; level++) {
			/* Walks the blocks of this level's tree. */
			used = blocks;
			if (used > span)
				used = span;
			error = sync_tree(&run, ui->indirect[level], level + 1U, used, sectors);
			blocks -= used;

			/* Widens the span for the next level, unless it cannot be represented. */
			if (error == 0 && blocks != 0) {
				if (span > UINT64_MAX / ms->super.nindir)
					error = EOVERFLOW;
				else
					span *= ms->super.nindir;
			}
		}

		/* Reports why the tree could not be walked. */
		if (error != 0)
			return error;

		/* A size past the last indirect level names more than a file can hold. */
		if (blocks != 0)
			return EFBIG;
	}

	/* Writes the last run gathered. */
	error = sync_run_write(&run);
	if (error != 0)
		return error;

	/* Succeeded: the file's cached blocks are on their way to the device. */
	return 0;
}

/*
 * Gathers the data blocks under one indirect block of the given depth (one
 * names data blocks), at most blocks of them, reading its entries a sector
 * at a time into the depth's sector of sectors.
 */
static int
sync_tree(
	struct ufs_sync_run *run,
	uint64_t fragment,
	unsigned depth,
	uint64_t blocks,
	uint8_t *sectors)
{
	const struct ufs_super *super;
	uint8_t *sector;
	uint64_t span;
	uint64_t child;
	uint64_t used;
	uint64_t lba;
	uint32_t index;
	uint32_t per_sector;
	unsigned level;
	int error;

	/* A level that is not there is a hole, with no blocks under it. */
	if (fragment == 0)
		return 0;

	/* Refuses a depth the walk does not reach and a block the volume could not hold. */
	super = &state(run->mountp)->super;
	if (depth == 0 || depth > UFS_NIADDR)
		return EINVAL;
	if (fragment >= super->size || super->frag > super->size - fragment)
		return EIO;

	/* Measures how many data blocks one entry of this level reaches. */
	span = 1;
	for (level = 1; level < depth; level++) {
		if (span > UINT64_MAX / super->nindir)
			return EOVERFLOW;
		span *= super->nindir;
	}

	/* Follows each entry the blocks reach, reading the entries a sector at a time. */
	sector = sectors + (size_t)(depth - 1U) * UFS_SECTOR_SIZE;
	per_sector = UFS_SECTOR_SIZE / 8U;
	index = 0;
	while (blocks != 0) {
		/* An indirect block names no more than nindir entries. */
		if (index >= super->nindir)
			return EIO;

		/* Reads the sector the next entries live in when the walk enters it. */
		if (index % per_sector == 0) {
			lba = (fragment << super->fsbtodb) + index / per_sector;
			error = read_metadata_sectors(run->mountp, lba, 1, sector);
			if (error != 0)
				return error;
		}

		/* Takes the entry and the blocks it reaches. */
		child = drv_ufs_get64(sector, (size_t)(index % per_sector) * 8U, super->swapped);
		used = blocks;
		if (used > span)
			used = span;

		/* Gathers a data block, or walks the indirect block below. */
		error = 0;
		if (depth == 1U) {
			/* A hole has no block to write. */
			if (child != 0)
				error = sync_run_add(run, child);
		} else {
			error = sync_tree(run, child, depth - 1U, used, sectors);
		}

		/* Reports why the entry's blocks could not be gathered. */
		if (error != 0)
			return error;

		/* Steps to the next entry. */
		blocks -= used;
		index++;
	}

	/* Succeeded: every block under the indirect block is gathered. */
	return 0;
}

/*
 * Adds one data block to the run, first writing the run gathered so far
 * when the block does not follow it on the disk.  A block that the volume
 * could not hold is a damaged map.
 */
static int
sync_run_add(
	struct ufs_sync_run *run,
	uint64_t fragment)
{
	const struct ufs_super *super;
	uint64_t lba;
	uint64_t fragments;
	uint32_t sectors;
	int error;

	/* Refuses a block outside the volume. */
	super = &state(run->mountp)->super;
	if (fragment >= super->size)
		return EIO;

	/*
	 * Takes the whole block, or what the volume has of it: a file's last
	 * block may be fragments, and a write-back of the cache's lines past
	 * them writes only what is waiting there anyway.
	 */
	fragments = super->frag;
	if (fragments > super->size - fragment)
		fragments = super->size - fragment;
	lba = fragment << super->fsbtodb;
	sectors = (uint32_t)(fragments << super->fsbtodb);

	/* Extends the run when the block follows it and the count still fits. */
	if (run->count != 0 &&
	    run->lba + run->count == lba &&
	    run->count <= UINT32_MAX - sectors) {
		run->count += sectors;

		/* Succeeded: the block joined the run. */
		return 0;
	}

	/* Writes the run so far, since the block starts another. */
	error = sync_run_write(run);
	if (error != 0)
		return error;

	/* Starts the next run at the block. */
	run->lba = lba;
	run->count = sectors;

	/* Succeeded: the block starts the run. */
	return 0;
}

/* Writes the run's lines that wait in the cache, and empties the run. */
static int
sync_run_write(
	struct ufs_sync_run *run)
{
	int error;

	/* An empty run has nothing to write. */
	if (run->count == 0)
		return 0;

	/* Writes the cached dirty lines the run covers, leaving the device unflushed. */
	error = ufs_buf_writeback_cached(run->mountp->m_disk, run->lba, run->count);
	run->count = 0;
	if (error != 0)
		return error;

	/* Succeeded: the run is on its way to the device. */
	return 0;
}

/* Takes one of the driver's fixed inode slots. */
static struct inode *
ufs_alloc_inode(
	struct mount *mountp)
{
	struct inode *allocated;

	(void)mountp;

	/* Takes an inode out of the shared pool. */
	allocated =
		(struct inode *)kern_calloc(1, sizeof(struct ufs_inode_info));

	/* Reports the inode, or that the pool is full. */
	return allocated;
}

/* Gives an inode slot back. */
static void
ufs_free_inode(
	struct inode *inode)
{
	kern_free(inode);
}

/* Reads and decodes the superblock of a volume. */
static int
ufs_read_super(
	struct disk *disk,
	struct ufs_super *super)
{
	uint8_t *buffer;
	uint64_t disk_sectors;
	int unsupported;
	int error;

	/* This driver reads disks whose blocks are whole sectors. */
	if (disk == NULL)
		return EOPNOTSUPP;
	unsupported = disk_sector_supported(disk);
	if (unsupported != 0)
		return EOPNOTSUPP;

	/* Takes the staging the superblock is read into. */
	buffer = kern_malloc(UFS_SBLOCK_SIZE);
	if (buffer == NULL)
		return ENOMEM;

	/* Reads the superblock from its fixed offset. */
	error = observed_disk_read(disk, UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE,
				   UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE, buffer);
	if (error == 0) {
		disk_sectors = ufs_disk_sectors(disk);
		error = drv_ufs_super_decode(buffer, UFS_SBLOCK_SIZE,
					     disk_sectors, super);
	}

	/* A fragment smaller than the disk's block was not written on it. */
	if (error == 0 && super->fsize < disk->d_block_size)
		error = EOPNOTSUPP;

	kern_free(buffer);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Renders one nibble as a hexadecimal character. */
static char
ufs_identity_hex(
	unsigned value)
{
	/* The first ten values are digits. */
	if (value < 10U)
		return (char)('0' + value);

	/* The rest are the upper-case letters that follow them. */
	return (char)('A' + value - 10U);
}

/* Renders a 32-bit value as eight hexadecimal characters. */
static void
ufs_identity_hex32(
	char output[8],
	uint32_t value)
{
	unsigned index;

	/* Renders the value as eight hexadecimal digits. */
	for (index = 0; index < 8U; index++) {
		output[index] = ufs_identity_hex((value >> (28U - index * 4U)) & 15U);
	}
}

/* Copies a volume label out, replacing what it cannot show. */
static void
ufs_identity_label(
	char *output,
	size_t capacity,
	const uint8_t *input,
	size_t length)
{
	size_t end = length;
	size_t index;

	/* Trims the padding the format stores a label with. */
	while (end != 0U && (input[end - 1U] == ' ' || input[end - 1U] == 0U))
		end--;

	/* Refuses a label the caller buffer could not hold. */
	if (end >= capacity)
		end = capacity - 1U;

	/* Copies the trimmed label out. */
	for (index = 0; index < end; index++) {
		/* A byte outside printable ASCII is not shown as itself. */
		if (input[index] >= 0x20U && input[index] <= 0x7eU)
			output[index] = (char)input[index];
		else
			output[index] = '_';
	}

	output[end] = '\0';
}

/* Marks the volume clean or dirty in its superblock. */
static int
ufs_write_clean(
	struct mount *mountp,
	uint8_t clean)
{
	struct ufs_mount_state *ms;
	uint8_t *buffer;
	int error;

	/* Takes the mount state this call runs against. */
	ms = state(mountp);

	/* Takes the staging the superblock is written from. */
	buffer = kern_malloc(UFS_SBLOCK_SIZE);
	if (buffer == NULL)
		return ENOMEM;

	mutex_lock(&ms->lock);
	mutex_lock(&ms->journal_lock);

	error = journal_checkpoint_locked(mountp);

	mutex_unlock(&ms->journal_lock);

	/* Publishes the clean flag the next mount reads. */
	if (error == 0) {
		error = observed_disk_read(
			mountp->m_disk, UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE,
			UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE, buffer);
	}
	if (error == 0) {
		buffer[UFS_FS_CLEAN] = clean;
		error = write_sectors(
			mountp, UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE,
			UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE, buffer);
	}
	if (error == 0)
		error = disk_sync(mountp->m_disk);
	if (error == 0)
		ms->super.clean = clean;

	mutex_unlock(&ms->lock);

	kern_free(buffer);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Asks whether a disk carries a volume this driver can mount. */
static int
ufs_probe(
	struct disk *disk)
{
	int error;
	struct ufs_super s;

	/* A disk carries UFS only if its superblock reads. */
	error = ufs_read_super(disk, &s);

	/* Reports whether it did. */
	return error;
}

/* Counts every inode's usage to rebuild the quota records. */
static int
ufs_quota_rebuild(
	struct mount *mountp)
{
	uint64_t fragment;
	uint64_t blocks;
	uint8_t *raw;
	uint16_t mode;
	struct ufs_mount_state *ms;
	uint8_t *block;
	uint32_t cg;
	uint32_t index;
	int used;
	int error;

	ms = state(mountp);
	error = 0;

	/* Takes the staging each inode block is read into. */
	block = kern_malloc(ms->super.bsize);
	if (block == NULL)
		return ENOMEM;
	/* Walks every cylinder group, charging the inodes it holds. */
	for (cg = 0; cg < ms->super.ncg && error == 0; cg++) {
		/* Reads the cylinder group being walked. */
		error = load_cg_locked(mountp, cg);
		if (error != 0)
			break;

		/* Walks the inodes of that group. */
		for (index = 0; index < ms->super.ipg; index++) {
			/*
			 * Asks the used map whether this number is handed out.
			 */
			used = bit_test(ms->cg + ms->cg_iusedoff, index);

			/* A clear bit in the used map names no inode. */
			if (!used)
				continue;
			fragment = cgstart(&ms->super, cg) + ms->super.iblkno +
				(index / ms->super.inopb) * ms->super.frag;

			/* Reads the block the inode lives in. */
			error = read_block(mountp, fragment, block);
			if (error != 0)
				break;
			raw = block +
				(index % ms->super.inopb) * UFS_DINODE_SIZE;

			/* Validates the selected mode. */
			mode = drv_ufs_get16(raw, UFS_DI_MODE,
					     ms->super.swapped);
			if (mode == 0)
				continue;

			/* Reads the block count this inode is charged for. */
			blocks = drv_ufs_get64(raw, UFS_DI_BLOCKS,
					       ms->super.swapped);
			if (blocks % (ms->super.bsize / UFS_SECTOR_SIZE) != 0) {
				error = EIO;
				break;
			}

			/* Charges the inode and its blocks to its owner. */
			error = quota_rebuild_add(
				&ms->quota,
				drv_ufs_get32(raw, UFS_DI_UID,
					      ms->super.swapped),
				drv_ufs_get32(raw, UFS_DI_GID,
					      ms->super.swapped),
				blocks / (ms->super.bsize / UFS_SECTOR_SIZE),
				1);
			if (error != 0)
				break;
		}
	}

	kern_free(block);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reads the quota configuration the root inode carries. */
static int
ufs_quota_load(
	struct mount *mountp,
	struct inode *root)
{
	struct ufs_mount_state *ms;
	uint8_t *buffer;
	ssize_t length;
	ssize_t loaded;
	int error;

	/* Takes the mount state this call runs against. */
	ms = state(mountp);

	/* Asks how long the stored quota configuration is. */
	length = ufs_getxattr(root, UFS_QUOTA_XATTR, NULL, 0);
	if (length == -ENODATA)
		return 0;

	/* Failed: reports why the attribute could not be read. */
	if (length < 0)
		return (int)-length;

	/* A volume with no configuration, or one too long, loads nothing. */
	if (length == 0 || (size_t)length > ms->super.bsize)
		return EINVAL;

	/* Takes the staging the configuration is read into. */
	buffer = kern_malloc((size_t)length);
	if (buffer == NULL)
		return ENOMEM;

	loaded = ufs_getxattr(root, UFS_QUOTA_XATTR, buffer, (size_t)length);
	if (loaded == length) {
		/* Parses the configuration the attribute held. */
		error = quota_import_config(&ms->quota, buffer,
					    (size_t)length);
	} else if (loaded < 0) {
		/* The read failed, reporting its error negated. */
		error = (int)-loaded;
	} else {
		/* A short read means the attribute was truncated. */
		error = EIO;
	}
	kern_free(buffer);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Serves one request against the snapshot's own disk. */
static int
snapshot_disk_submit(
	struct disk *disk,
	struct bio *bio)
{
	struct ufs_mount_state *ms;
	size_t moved;
	int error;

	/* A request that names no disk has no mount state behind it. */
	ms = NULL;
	if (disk != NULL)
		ms = disk->d_data;

	/* Rejects a call that names no mount or no request. */
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

	/* Only a read that succeeded has moved any bytes into the buffer. */
	moved = 0;
	if (error == 0 && bio->b_op == BIO_READ)
		moved = (size_t)bio->b_block_count * UFS_SECTOR_SIZE;

	bio_complete(bio, error, moved);

	/* Succeeded. */
	return 0;
}

/* Publishes the snapshot as a read-only disk of its own. */
static int
snapshot_disk_publish(
	struct ufs_mount_state *ms)
{
	struct disk *disk;
	unsigned number;
	int error;
	unsigned attempt;

	/* Succeeded: the snapshot device is already published. */
	if (ms->snapshot_disk != NULL)
		return 0;

	/* Tries each free disk slot in turn. */
	for (attempt = 0; attempt < DISK_MAX; attempt++) {
		disk = disk_alloc();
		number = snapshot_disk_sequence++;

		/* A slot that holds no disk cannot be taken. */
		if (disk == NULL)
			return ENOSPC;
		kern_memcpy(disk->d_name, "ufssnap", 7);

		/* The name has room for two digits and no more. */
		if (number >= 100U)
			number %= 100U;

		/* Renders the slot number into the device name. */
		if (number >= 10U) {
			disk->d_name[7] = (char)('0' + number / 10U);
			disk->d_name[8] = (char)('0' + number % 10U);
			disk->d_name[9] = '\0';
		} else {
			disk->d_name[7] = (char)('0' + number);
			disk->d_name[8] = '\0';
		}

		/* Describes it as a read-only disk of the same size. */
		disk->d_flags = DISK_READ_ONLY;
		disk->d_block_size = UFS_SECTOR_SIZE;
		disk->d_block_count = ms->snapshot.volume_sectors;
		disk->d_max_transfer_blocks = 128;
		disk->d_ops = &snapshot_disk_ops;
		disk->d_data = ms;

		/* Publishes the snapshot as a disk of its own. */
		error = disk_create(disk);
		if (error == 0) {
			ms->snapshot_disk = disk;

			/* Succeeded. */
			return 0;
		}

		(void)disk_destroy(disk);

		/* A name already taken is tried again with the next number. */
		if (error != EEXIST)
			return error;
	}

	/* Failed. */
	return ENOSPC;
}

/* Takes that snapshot disk back out of service. */
static int
snapshot_disk_remove(
	struct ufs_mount_state *ms)
{
	struct disk *disk = ms->snapshot_disk;
	int error;

	/* A device that was never published has nothing to remove. */
	if (disk == NULL)
		return 0;

	/* Takes the device out of service, if nothing is using it. */
	error = disk_gone_if_idle(disk);
	if (error != 0)
		return error;

	/* Destroys the device now that nothing can reach it. */
	error = disk_destroy(disk);
	if (error == 0)
		ms->snapshot_disk = NULL;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Gives a mount's private state and everything it holds back. */
static void
ufs_state_free(
	struct ufs_mount_state *ms)
{
	/* Freeing a mount that was never set up does nothing. */
	if (ms == NULL)
		return;

	journal_image_free(ms);
	buf_view_release(&ms->cg_view);
	kern_free(ms->j3.extents);
	kern_free(ms->snapshot_map);
	kern_free(ms->cg);
	kern_free(ms);
}

/* Writes the quota records back to the volume. */
static int
ufs_quota_persist(
	struct mount *mountp)
{
	struct ufs_mount_state *ms;
	uint8_t *buffer;
	size_t length;
	int error;

	/* Takes the mount state this call runs against. */
	ms = state(mountp);

	/* A read-only volume, or one without a root, stores nothing. */
	if (!ms->writable || mountp->m_root == NULL)
		return EROFS;

	/* Takes the staging the configuration is written from. */
	buffer = kern_malloc(ms->super.bsize);
	if (buffer == NULL)
		return ENOMEM;

	/* Renders the configuration as the attribute stores it. */
	error = quota_export_config(&ms->quota, buffer, ms->super.bsize, &length);
	if (error == 0) {
		error = ufs_setxattr(mountp->m_root, UFS_QUOTA_XATTR, buffer, length, 0);
	}
	if (error == 0)
		error = disk_sync(mountp->m_disk);
	kern_free(buffer);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reclaims one validated zero-link identity without publishing a cache object.
 */
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
	 * mutations.
	 */
	ms = state(mountp);

	/*
	 * Sizes the staging from the images the recovery will hold, on a
	 * volume with the tail journal, whose grouped retirement needs the
	 * room; another volume retires the inode by the ordered steps of a
	 * write-through volume (ws073-p054).
	 */
	bytes = 3U * ms->super.bsize + UFS_SBLOCK_SIZE;
	if (ms->journal_enabled &&
	    (ms->journal.sector_count <= 2U ||
	     bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	     bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)) {
		/* Failed. */
		return EOPNOTSUPP;
	}

	kern_memset(owner, 0, sizeof(*owner));
	owner->inode.i_mount = mountp;

	(void)mutex_init(&owner->inode.i_lock, LOCK_RANK_INODE, "ufs orphan");

	/* Decodes the raw inode, allowing the zero link count of an orphan. */
	error = decode_inode_raw(&owner->inode, raw, number, 1);
	if (error != 0)
		return error;

	/*
	 * Reuses checked pointer/xattr/bitmap owners and their conservative
	 * outcomes.
	 */
	error = reclaim_unlinked_inode(&owner->inode);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Scans a private mount using stable candidate bits and freshly read dinodes.
 */
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
	uint16_t links;
	size_t map_bytes;
	int used;
	int error;

	/*
	 * Saves each candidate map before reclaim can change the mount's CG
	 * buffer.
	 */
	ms = state(mountp);
	map_bytes = ((size_t)ms->super.ipg + 7U) / 8U;

	/* Walks every cylinder group looking for unlinked inodes. */
	for (cg = 0; cg < ms->super.ncg; cg++) {
		/* Reads the cylinder group being scanned. */
		error = load_cg_locked(mountp, cg);
		if (error != 0)
			return error;

		kern_memcpy(scan->bitmap, ms->cg + ms->cg_iusedoff, map_bytes);

		/*
		 * Leaves reserved slots and linked namespace owners untouched.
		 */
		for (index = 0; index < ms->super.ipg; index++) {
			/* Asks the candidate map whether this number is set. */
			used = bit_test(scan->bitmap, index);

			/* A clear bit in the used map names no inode. */
			if (!used)
				continue;

			/* The inode number this bit stands for. */
			number = (uint64_t)cg * ms->super.ipg + index;
			if (number <= UFS_ROOT_INO)
				continue;

			/*
			 * Refuses a number the on-disk format could not record.
			 */
			if (number > UINT32_MAX)
				return EOVERFLOW;

			fragment = cgstart(&ms->super, cg) + ms->super.iblkno + (index / ms->super.inopb) * ms->super.frag;

			/* Reads the block the inode lives in. */
			error = read_block(mountp, fragment, scan->block);
			if (error != 0)
				return error;

			/* Takes the raw inode out of that block. */
			raw = scan->block + (index % ms->super.inopb) * UFS_DINODE_SIZE;

			/* The link count the stored inode still carries. */
			links = drv_ufs_get16(raw, UFS_DI_NLINK, ms->super.swapped);

			/* An inode a name still points at is not an orphan. */
			if (links != 0)
				continue;

			/*
			 * Recovers the inode when it turns out to be an orphan.
			 */
			error = orphan_recover_one(mountp, (uint32_t)number, raw, &scan->inode);
			if (error != 0)
				return error;
		}
	}

	/*
	 * Completes all bounded per-inode reclamations before mount
	 * publication.
	 */
	return 0;
}

/*
 * Recovers journal-owned orphans after validation and quota rebuild on a
 * private mount.
 */
static int
orphan_recover(
	struct mount *mountp)
{
	struct ufs_mount_state *ms;
	struct ufs_orphan_scan *scan;
	int error;

	/* Keeps a read-only mount free of orphan-reclamation writes. */
	ms = state(mountp);
	if (!ms->writable)
		return 0;

	/*
	 * A volume with the tail journal is scanned at every mount.  Another
	 * one -- the batched journal's (v3) or none -- only when its last
	 * session did not end clean: a file that lost its last name, or was
	 * still open when it did, is retired later than its unlink, in another
	 * transaction of the batched journal, so a crash between the two
	 * leaves an allocated inode nothing names (ws073-p054, BUG-164).  The
	 * scan runs before the batched journal opens and before the writes are
	 * delayed, so each retirement goes through in its ordered steps.
	 */
	if (!ms->journal_enabled && ms->super.clean != 0)
		return 0;

	/* A mount that already has a root has been recovered already. */
	if (mountp->m_root != NULL)
		return EBUSY;

	/* Takes the staging the scan reads inodes through. */
	scan = kern_calloc(1, sizeof(*scan) + 2U * ms->super.bsize);
	if (scan == NULL)
		return ENOMEM;

	scan->bitmap = (uint8_t *)(scan + 1);
	scan->block = scan->bitmap + ms->super.bsize;

	/*
	 * Excludes namespace users while each checked owner takes its metadata
	 * locks.
	 */
	namespace_enter(ms);

	/* Walks the volume for inodes nothing names. */
	error = orphan_scan_locked(mountp, scan);
	if (error != 0)
		stop_writing(mountp, "the scan for orphaned inodes failed", error);

	namespace_leave(ms);

	kern_free(scan);

	/*
	 * Returns failure with persistent remaining ownership for the next
	 * mount attempt.
	 */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Mounts a volume (the file system type's mount operation): the steps of
 * ufs_mount_steps(), and on a failure one log line naming the step that
 * failed (BUG-312: a volume that could not be mounted after a power cut
 * left nothing in the log).  A disk without a UFS superblock is not
 * logged: the root probe and "auto" try every partition.
 */
static int
ufs_mount_impl(
	struct mount *mountp)
{
	const char *step;
	const char *volume;
	const char *access;
	int error;

	/* The steps, each naming itself before it runs. */
	step = "reading the superblock";
	error = ufs_mount_steps(mountp, &step);

	/* A failure is logged with its step, unless the disk holds no UFS volume at all. */
	if (error != 0 && error != EOPNOTSUPP && mountp != NULL) {
		volume = "(no disk)";
		if (mountp->m_disk != NULL)
			volume = mountp->m_disk->d_name;
		access = "read-write";
		if ((mountp->m_flags & MOUNT_READ_ONLY) != 0)
			access = "read-only";
		kern_logf("ufs: %s: the volume cannot be mounted %s: %s (error %d)\n", volume, access, step, error);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * The steps of a mount: the superblock, the journals and their replay,
 * the snapshot, the cylinder groups and the summaries, the root directory,
 * the quota and the orphans, and the clean flag.  *step names the step
 * running, for the log of a failure.
 */
static int
ufs_mount_steps(
	struct mount *mountp,
	const char **step)
{
	uint8_t *free_map;
	uint32_t fragment;
	uint32_t ndblk;
	struct ufs_mount_state *ms;
	struct inode *root;
	int error;
	uint64_t total_ndir = 0, total_nbfree = 0, total_nifree = 0,
		total_nffree = 0;
	uint32_t cg;
	int summaries_rebuilt = 0;
	int free_bit;
	int difference;
	int cached;
	int read_only;
	const char *volume;
	off_t cursor = 0;
	uint32_t number;
	uint8_t type;
	char name[NAME_MAX + 1U];

	/* Refuses a mount that names no disk to read. */
	if (mountp == NULL || mountp->m_disk == NULL)
		return EINVAL;

	/* Takes the state this mount will be described by. */
	ms = kern_calloc(1, sizeof(*ms));
	if (ms == NULL)
		return ENOMEM;

	/* Reads and validates the superblock. */
	error = ufs_read_super(mountp->m_disk, &ms->super);
	if (error) {
		kern_free(ms);

		/* Failed. */
		return error;
	}

	mountp->m_data = ms;
	(void)mutex_init(&ms->journal_lock, LOCK_RANK_DEVICE, "ufs journal");
	(void)mutex_init(&ms->j3.lock, LOCK_RANK_DEVICE, "ufs batched journal");
	waitq_init(&ms->j3.commit_idle, "ufs batched journal commit");
	(void)mutex_init(&ms->snapshot_lock, LOCK_RANK_DEVICE, "ufs snapshot");

	/* Finds the journal, if this volume carries one. */
	*step = "finding the journal";
	error = journal_discover(mountp, ms);

	/* Applies the last commit of a batched journal before reading metadata. */
	if (error == 0 && !ms->journal_enabled) {
		*step = "replaying the batched journal";
		error = j3_replay(mountp, ms);
	}

	/* Gives the mount up when a journal could not be read or applied. */
	if (error != 0) {
		mountp->m_data = NULL;
		ufs_state_free(ms);

		/* Failed. */
		return error;
	}

	/* Finds the snapshot device, if this volume carries one. */
	*step = "finding the snapshot";
	error = snapshot_discover(mountp, ms);
	if (error != 0) {
		mountp->m_data = NULL;
		ufs_state_free(ms);

		/* Failed. */
		return error;
	}

	(void)mutex_init(&ms->namespace_lock, LOCK_RANK_NAMESPACE, "ufs namespace");
	spin_init(&ms->namespace_guard, LOCK_RANK_NAMESPACE, "ufs namespace readers");
	spin_init(&ms->readahead_lock, LOCK_RANK_DISK, "ufs read-ahead");
	ms->namespace_readers = 0;
	waitq_init(&ms->namespace_drained, "ufs namespace drained");
	(void)mutex_init(&ms->load_lock, LOCK_RANK_INODE_IO, "ufs inode load");
	(void)mutex_init(&ms->lock, LOCK_RANK_INODE, "ufs mount");
	quota_state_init(&ms->quota);
	ms->cg = kern_malloc(ms->super.bsize);

	/* Takes the buffer every cylinder group is read into. */
	if (ms->cg == NULL) {
		mountp->m_data = NULL;
		ufs_state_free(ms);

		/* Failed. */
		return ENOMEM;
	}

	/* Walks every cylinder group to rebuild the free counts. */
	*step = "reading the cylinder groups";
	for (cg = 0; cg < ms->super.ncg; cg++) {
		/* Reads one cylinder group. */
		error = load_cg_locked(mountp, cg);
		if (error != 0)
			break;

		ndblk = cg_ndblk(&ms->super, cg);
		free_map = ms->cg + ms->cg_freeoff;

		/* Counts the free fragments inside the data area. */
		for (fragment = 0;
		     fragment < ms->super.dblkno && fragment < ndblk;
		     fragment++) {
			/* Asks the free map whether this fragment is free. */
			free_bit = bit_test(free_map, fragment);

			/* A free fragment inside the metadata area is wrong. */
			if (free_bit) {
				*step = "a cylinder group marks a fragment of its metadata area free";
				error = EINVAL;
				break;
			}
		}

		/* Counts the free fragments past the last data block. */
		for (fragment = ndblk;
		     error == 0 && fragment < ms->super.fpg;
		     fragment++) {
			/* Asks the free map whether this fragment is free. */
			free_bit = bit_test(free_map, fragment);

			/* A free fragment past the last data block is wrong. */
			if (free_bit) {
				*step = "a cylinder group marks a fragment past its last data block free";
				error = EINVAL;
				break;
			}
		}
		if (error != 0)
			break;

		/*
		 * The first group also carries the inodes the superblock
		 * counts.
		 */
		if (cg == 0) {
			/* Asks the used map whether the root inode is there. */
			free_bit = bit_test(ms->cg + ms->cg_iusedoff, UFS_ROOT_INO);

			/* A volume without a root inode cannot be mounted. */
			if (!free_bit) {
				*step = "the root inode is not allocated";
				error = EINVAL;
				break;
			}
		}

		/* Adds this group's counts to the totals being checked. */
		total_ndir += drv_ufs_get32(ms->cg, UFS_CG_NDIR, ms->super.swapped);
		total_nbfree += drv_ufs_get32(ms->cg, UFS_CG_NBFREE, ms->super.swapped);
		total_nifree += drv_ufs_get32(ms->cg, UFS_CG_NIFREE, ms->super.swapped);
		total_nffree += drv_ufs_get32(ms->cg, UFS_CG_NFFREE, ms->super.swapped);
	}

	/* Rebuilds the summaries when what was counted disagrees with them. */
	if (error == 0 &&
	    (total_ndir != ms->super.cstotal_ndir ||
	     total_nbfree != ms->super.cstotal_nbfree ||
	     total_nifree != ms->super.cstotal_nifree ||
	     total_nffree != ms->super.cstotal_nffree)) {
		/*
		 * The groups' own counts are the truth: a write-cached mount
		 * can leave the totals behind them at a crash, and either
		 * journal's replay leaves them as the groups say.  A batched
		 * journal's volume was mounted write-cached when it crashed,
		 * whatever this mount is, and a read-only mount only reports
		 * the totals (BUG-312: the root probe's read-only mount of a
		 * volume that crashed was refused, so the root was not found).
		 * Only a read-write mount of a volume without any journal, not
		 * write-cached, must find them clean.
		 */
		cached = write_cached_intended(mountp, ms);
		read_only = (mountp->m_flags & MOUNT_READ_ONLY) != 0;
		if (!ms->journal_enabled && !ms->j3.present && !cached && !read_only) {
			*step = "the summary totals differ from the cylinder groups on a volume without a journal";
			error = EINVAL;
		} else {
			volume = mountp->m_disk->d_name;
			kern_logf("ufs: %s: the summary totals (%llu directories, %llu blocks, %llu inodes, %llu fragments free) differ from the cylinder groups' (%llu, %llu, %llu, %llu); the groups' are used\n",
				  volume,
				  (unsigned long long)ms->super.cstotal_ndir,
				  (unsigned long long)ms->super.cstotal_nbfree,
				  (unsigned long long)ms->super.cstotal_nifree,
				  (unsigned long long)ms->super.cstotal_nffree,
				  (unsigned long long)total_ndir,
				  (unsigned long long)total_nbfree,
				  (unsigned long long)total_nifree,
				  (unsigned long long)total_nffree);
			ms->super.cstotal_ndir = total_ndir;
			ms->super.cstotal_nbfree = total_nbfree;
			ms->super.cstotal_nifree = total_nifree;
			ms->super.cstotal_nffree = total_nffree;
			summaries_rebuilt = 1;
		}
	}

	/* Publishes the rebuilt summaries. */
	if (error == 0) {
		*step = "rebuilding the quota usage";
		error = ufs_quota_rebuild(mountp);
	}
	if (error == 0)
		error = load_cg_locked(mountp, 0);
	if (error != 0) {
		mountp->m_data = NULL;
		ufs_state_free(ms);

		/* Failed. */
		return error;
	}

	/*
	 * Preserve the ordinary persistent upper's validated reopen policy.
	 * Private root mounts remain mounted through shutdown sync, so a dirty
	 * marker alone is not proof of damaged metadata. Keep the structural,
	 * allocation-summary and root checks as mount admission gates.
	 */
	if ((mountp->m_flags & MOUNT_READ_ONLY) == 0) {
		/*
		 * A read-only disk is mounted read-only whatever the volume
		 * says.
		 */
		if ((mountp->m_disk->d_flags & DISK_READ_ONLY) != 0) {
			*step = "the disk is read-only";
			mountp->m_data = NULL;
			ufs_state_free(ms);

			/* Failed. */
			return EROFS;
		}

		ms->writable = 1;

		/*
		 * Writes the rebuilt summaries back before anything else runs.
		 */
		if (summaries_rebuilt) {
			/* Writes the rebuilt summaries back. */
			*step = "writing the rebuilt summary totals";
			error = write_super_summaries(mountp);
			if (error != 0) {
				mountp->m_data = NULL;
				ufs_state_free(ms);

				/* Failed. */
				return error;
			}
		}
	}

	/* Reads the root inode, which every walk starts from. */
	*step = "reading the root directory's inode";
	error = load_inode(mountp, UFS_ROOT_INO, &root);
	if (error || root->i_type != INODE_DIR) {
		/* Publishes the root inode as the mount point. */
		if (!error) {
			root->i_flags |= INODE_DEAD;
			inode_release(root);
		}

		mountp->m_data = NULL;
		ufs_state_free(ms);

		/*
		 * Reports why the mount failed, or a device error by default.
		 */
		if (error != 0)
			return error;	/* Failed. */

		return EIO;	/* Failed. */
	}

	/*
	 * A malformed root must not become the namespace anchor.  Validate the
	 * mandatory entries while the mount is still private and unpublished.
	 */
	*step = "the root directory's . and .. entries";
	error = next_dirent(root, &cursor, &number, &type, name);
	if (error == 0) {
		/* The first entry of every directory names the directory. */
		difference = kern_strcmp(name, ".");
		if (number != UFS_ROOT_INO || difference != 0)
			error = EIO;
	}

	if (error == 0)
		error = next_dirent(root, &cursor, &number, &type, name);
	if (error == 0) {
		/* The second names the parent, which for the root is itself. */
		difference = kern_strcmp(name, "..");
		if (number != UFS_ROOT_INO || difference != 0)
			error = EIO;
	}

	/* Loads the quota configuration and recovers any orphaned inodes. */
	if (error == 0) {
		*step = "loading the quota configuration";
		error = ufs_quota_load(mountp, root);
	}

	/* The orphans after the quota, which their retirement credits. */
	if (error == 0) {
		*step = "recovering orphaned inodes";
		error = orphan_recover(mountp);
	}
	if (error != 0) {
		root->i_flags |= INODE_DEAD;
		inode_release(root);
		mountp->m_data = NULL;
		ufs_state_free(ms);

		/* Failed. */
		return error;
	}

	/*
	 * Do not dirty an image until every read-only mount validation,
	 * including the root inode, has succeeded.
	 */
	if (ms->writable) {
		/* Marks the volume clean now that the mount has finished. */
		*step = "writing the clean flag";
		error = ufs_write_clean(mountp, 0);
		if (error) {
			root->i_flags |= INODE_DEAD;
			inode_release(root);
			mountp->m_data = NULL;
			ufs_state_free(ms);

			/* Failed. */
			return error;
		}

		/* Delays the volume's writes from here on, when the mount allows it. */
		write_cached_start(mountp, ms, root);
	}

	/* Publishes the root inode as the mount's root. */
	root->i_flags |= INODE_ROOT;
	mountp->m_root = root;

	/* Publishes the snapshot device, if this volume carries one. */
	if (ms->snapshot.active) {
		/* Gives the snapshot a device of its own to be read through. */
		*step = "publishing the snapshot device";
		error = snapshot_disk_publish(ms);
		if (error != 0) {
			mountp->m_root = NULL;
			root->i_flags |= INODE_DEAD;
			inode_release(root);
			mountp->m_data = NULL;
			ufs_state_free(ms);

			/* Failed. */
			return error;
		}
	}

	/* Succeeded. */
	return 0;
}

/*
 * Excludes metadata admission until all previously published homes are durable.
 */
static int
ufs_sync(
	struct mount *mountp)
{
	struct ufs_mount_state *ms;
	int error;

	/* A call that names no mount has nothing to flush. */
	if (mountp == NULL)
		return EINVAL;

	/* Takes the mount state this call runs against. */
	ms = state(mountp);
	if (ms == NULL)
		return EINVAL;

	/* Commits the running transaction, and again while a held home is owed. */
	error = ufs_sync_journal(ms, mountp);

	/*
	 * Writes the dirty lines, then the content a journal's pin still holds
	 * back: on a volume whose start is not aligned to a cache line, a line
	 * spans two blocks, and a transaction that pins one block's metadata
	 * pins the other block's content with it, again and again while the
	 * metadata keeps changing.  The content's own blocks are written
	 * beneath the cache; the journaled ones wait for their commit
	 * (ws073-p053, BUG-163).  On an aligned volume no pinned line holds
	 * content, and nothing more is written.
	 */
	if (error == 0)
		error = buf_sync(mountp->m_disk);
	if (error == 0)
		error = buf_write_unjournaled(mountp->m_disk);

	/*
	 * The mount is only durable once the device has it.  The writes and the
	 * device's cache flush need no mount lock (ws073-p045, BUG-135: under a
	 * slow flush the lock held every change of the volume for seconds).
	 */
	if (error == 0)
		error = disk_sync(mountp->m_disk);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Commits the running transaction for a sync.  A held range a finish could
 * not write, because a later transaction pinned its line again, reaches
 * its home with the commit of that later transaction, so the sync commits
 * again while a home is owed, at most UFS_SYNC_OWED_ROUNDS more times
 * (ws073-p051).  A volume's sync and a file's sync both commit this way.
 */
static int
ufs_sync_journal(
	struct ufs_mount_state *ms,
	struct mount *mountp)
{
	unsigned round;
	int owed;
	int error;

	/* Commits the running transaction, then again while a home is owed. */
	error = ufs_sync_commit(ms, mountp);
	for (round = 0; round < UFS_SYNC_OWED_ROUNDS && error == 0; round++) {
		/* Reads whether a home is owed. */
		mutex_lock(&ms->j3.lock);
		owed = ms->j3.home_owed;
		mutex_unlock(&ms->j3.lock);

		/* Stops once every held range is on its way home. */
		if (!owed)
			break;

		/* Commits the transaction that holds the owed line. */
		error = ufs_sync_commit(ms, mountp);
	}

	/* Reports why the transaction could not be committed. */
	if (error != 0)
		return error;

	/* Succeeded: the running transaction is committed. */
	return 0;
}

/*
 * One commit of a sync (ws073-p051): checks the tail journal's point and
 * closes the batched journal's running transaction under the mount lock,
 * which takes only memory copies, then seals it -- the device's flushes --
 * with the locks let go.  A commit another thread is writing is waited
 * out with the mount lock let go, so that no change of the volume waits
 * behind its flushes (BUG-135).
 */
static int
ufs_sync_commit(
	struct ufs_mount_state *ms,
	struct mount *mountp)
{
	int closed;
	int busy;
	int error;

	/* Closes the running transaction, waiting out a commit under way. */
	for (;;) {
		error = ufs_sync_close(ms, mountp, &closed, &busy);
		if (!busy)
			break;

		/* Waits for the commit under way to end, then tries again. */
		mutex_lock(&ms->j3.lock);
		j3_commit_wait(&ms->j3);
		mutex_unlock(&ms->j3.lock);
	}

	/* Reports why the transaction could not be closed. */
	if (error != 0)
		return error;

	/* Seals the closed transaction. */
	if (closed)
		error = j3_finish_commit(ms, mountp);

	/* Reports why the commit failed. */
	if (error != 0)
		return error;

	/* Succeeded: the running transaction is committed, or there was none. */
	return 0;
}

/*
 * The locked part of a sync (ws073-p051): checks the tail journal's point
 * and closes the batched journal's running transaction under the mount
 * lock.  *closed reports a transaction for j3_finish_commit() to seal;
 * *busy reports that a commit is being written, so the caller is to wait
 * for it with the mount lock let go and try again.
 */
static int
ufs_sync_close(
	struct ufs_mount_state *ms,
	struct mount *mountp,
	int *closed,
	int *busy)
{
	int error;

	/* Checks the tail journal's point under the mount lock. */
	*closed = 0;
	*busy = 0;
	mutex_lock(&ms->lock);
	mutex_lock(&ms->journal_lock);
	error = journal_checkpoint_locked(mountp);
	mutex_unlock(&ms->journal_lock);

	/* A failed check point ends the sync. */
	if (error != 0) {
		mutex_unlock(&ms->lock);
		return error;
	}

	/* A batched journal out of service has nothing to close. */
	if (!ms->j3.active) {
		mutex_unlock(&ms->lock);
		return 0;
	}

	/* Closes the running transaction, unless a commit is being written. */
	mutex_lock(&ms->j3.lock);
	if (ms->j3.closing)
		*busy = 1;
	else
		error = j3_close_transaction(ms, mountp, closed);
	mutex_unlock(&ms->j3.lock);
	mutex_unlock(&ms->lock);

	/* Reports why the transaction could not be closed. */
	if (error != 0)
		return error;

	/* Succeeded: the transaction is closed, or a commit under way is reported. */
	return 0;
}

/* Reports how much of the volume is used and how much is free. */
static int
ufs_statvfs(
	struct mount *mountp,
	struct statvfs *result)
{
	struct ufs_mount_state *ms;
	uint64_t nbfree;
	uint64_t nffree;
	uint64_t nifree;

	/* Takes the mount state this call runs against. */
	ms = state(mountp);

	/* Rejects a call that names no mount or nowhere to report. */
	if (ms == NULL || result == NULL)
		return EINVAL;
	mutex_lock(&ms->lock);

	/* Renders the volume's own counts as the fields statvfs defines. */
	nbfree = ms->super.cstotal_nbfree;
	nffree = ms->super.cstotal_nffree;
	nifree = ms->super.cstotal_nifree;
	kern_memset(result, 0, sizeof(*result));
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

	/* Succeeded. */
	return 0;
}

/* Serves one quota request against this mount. */
static int
ufs_quotactl(
	struct mount *mountp,
	struct quota_control *request)
{
	int applied;
	struct ufs_mount_state *ms;
	struct quota_record record;
	enum quota_type type;
	uint8_t *saved;
	size_t saved_length;
	int enabled;
	int restored;
	int error;
	int mutating;

	ms = state(mountp);
	saved = NULL;
	saved_length = 0;
	mutating = 0;

	/* Rejects a call that names no mount or no known quota type. */
	if (ms == NULL || request == NULL || request->type > KERN_QUOTA_GROUP)
		return EINVAL;

	type = request->type == KERN_QUOTA_USER ? QUOTA_USER : QUOTA_GROUP;

	/* Runs the operation the request names. */
	switch (request->command) {
	case KERN_QUOTA_GET:

		/* Reads the record the caller asked about. */
		error = quota_get(&ms->quota, type, request->id, &record);
		if (error != 0)
			return error;

		/* Reports whether quotas are switched on for that type. */
		error = quota_enabled(&ms->quota, type, &enabled);
		if (error != 0)
			return error;
		request->flags = enabled ? KERN_QUOTA_F_ENABLED : 0;
		request->block_soft = record.block_soft;
		request->block_hard = record.block_hard;
		request->inode_soft = record.inode_soft;
		request->inode_hard = record.inode_hard;
		request->blocks = record.blocks;
		request->inodes = record.inodes;
		request->block_deadline = record.block_deadline;
		request->inode_deadline = record.inode_deadline;

		/* Reads the grace period the caller asked about. */
		applied = quota_get_grace(&ms->quota, &request->grace_seconds);

		/* Reports the grace period, or why it could not be read. */
		return applied;
	case KERN_QUOTA_SET:
		/* Refuses to write to a volume that is no longer writable. */
		if (!ms->writable)
			return EROFS;
		mutating = 1;
		break;
	case KERN_QUOTA_ENABLE:
	case KERN_QUOTA_DISABLE:
		/* Refuses to write to a volume that is no longer writable. */
		if (!ms->writable)
			return EROFS;
		mutating = 1;
		break;
	case KERN_QUOTA_SYNC:
		if (!ms->writable) {
			/* A read-only volume only has to be flushed. */
			applied = disk_sync(mountp->m_disk);
		} else {
			/* A writable volume writes the records back first. */
			applied = ufs_quota_persist(mountp);
		}

		/* Reports whether the change reached the volume. */
		return applied;
	default:
		/* Failed. */
		return EINVAL;
	}

	/* A change is staged so it can be put back if the write fails. */
	if (mutating) {
		/* Takes the staging the old configuration is saved in. */
		saved = kern_malloc(ms->super.bsize);
		if (saved == NULL)
			return ENOMEM;

		/* Saves the configuration as it stands before the change. */
		error = quota_export_config(&ms->quota, saved, ms->super.bsize,
					    &saved_length);
		if (error != 0) {
			kern_free(saved);

			/* Failed. */
			return error;
		}
	}

	/* Runs the operation the request names. */
	switch (request->command) {
	case KERN_QUOTA_SET:
		/* Builds the record out of the limits the caller named. */
		kern_memset(&record, 0, sizeof(record));
		record.id = request->id;
		record.block_soft = request->block_soft;
		record.block_hard = request->block_hard;
		record.inode_soft = request->inode_soft;
		record.inode_hard = request->inode_hard;

		/* Applies the record the caller asked for. */
		error = quota_set(&ms->quota, type, &record);
		if (error == 0 && request->grace_seconds != 0) {
			error = quota_set_grace(&ms->quota,
						request->grace_seconds);
		}

		break;
	case KERN_QUOTA_ENABLE:
		/* Switches accounting on for the type the request names. */
		error = quota_enable(&ms->quota, type, 1);
		break;
	case KERN_QUOTA_DISABLE:
		/* And off again for that same type. */
		error = quota_enable(&ms->quota, type, 0);
		break;
	default:
		/* A command this driver has no handler for. */
		error = EINVAL;
		break;
	}

	/* A change is only made when it also reaches the volume. */
	if (error == 0)
		error = ufs_quota_persist(mountp);
	if (error != 0) {
		/* Puts the configuration that was in force back. */
		restored = quota_import_config(&ms->quota, saved,
					       saved_length);

		/* A configuration that cannot be restored ends writing. */
		if (restored != 0)
			stop_writing(mountp, "the quota configuration could not be restored", restored);
	}
	kern_free(saved);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Serves one snapshot request against this mount. */
static int
ufs_snapshotctl(
	struct mount *mountp,
	struct snapshot_control *request)
{
	struct ufs_mount_state *ms;
	struct backing_mutation_guard guard;
	int error;

	ms = state(mountp);
	error = 0;

	/* Rejects a call that names no mount or no request. */
	if (ms == NULL || request == NULL)
		return EINVAL;

	kern_memset(request->device, 0, sizeof(request->device));

	/* A volume without snapshot storage has nothing to control. */
	if (!ms->snapshot_available)
		return EOPNOTSUPP;

	/* Runs the operation the request names. */
	switch (request->command) {
	case KERN_SNAPSHOT_CREATE:
		/* Refuses to write to a volume that is no longer writable. */
		if (!ms->writable)
			return EROFS;
		/* Excludes existing and newly preparing file backing across aliases. */
		error = backing_mutation_begin_disk(mountp->m_disk, 0,
		    mountp->m_disk->d_block_count, NULL, &guard);
		if (error != 0)
			return error;
		mutex_lock(&ms->lock);
		mutex_lock(&ms->snapshot_lock);
		mutex_lock(&ms->journal_lock);
		error = journal_checkpoint_locked(mountp);
		mutex_unlock(&ms->journal_lock);
		if (error == 0)
			error = disk_sync(mountp->m_disk);
		if (error == 0)
			error = drv_ufs_snapshot_create(&ms->snapshot);
		mutex_unlock(&ms->snapshot_lock);
		mutex_unlock(&ms->lock);
		if (error != 0)
			goto create_finished;
		if (error == 0)
			error = snapshot_disk_publish(ms);
		if (error != 0 && ms->snapshot.active) {
			mutex_lock(&ms->snapshot_lock);
			(void)drv_ufs_snapshot_delete(&ms->snapshot);
			mutex_unlock(&ms->snapshot_lock);
		}
create_finished:
		backing_mutation_end(&guard);
		break;
	case KERN_SNAPSHOT_DELETE:
		/* Refuses to write to a volume that is no longer writable. */
		if (!ms->writable)
			return EROFS;

		/* A snapshot that is not active has nothing to delete. */
		if (!ms->snapshot.active)
			return ENOENT;

		/*
		 * Takes the snapshot device out of service before deleting it.
		 */
		error = snapshot_disk_remove(ms);
		if (error != 0)
			return error;
		mutex_lock(&ms->snapshot_lock);
		error = drv_ufs_snapshot_delete(&ms->snapshot);
		mutex_unlock(&ms->snapshot_lock);
		if (error != 0)
			(void)snapshot_disk_publish(ms);
		break;
	case KERN_SNAPSHOT_STATUS:
		break;
	default:
		/* Failed. */
		return EINVAL;
	}

	/* Failed: reports why the snapshot could not be deleted. */
	if (error != 0)
		return error;

	request->flags = ms->snapshot.active ? KERN_SNAPSHOT_F_ACTIVE : 0;
	request->captured_sectors = ms->snapshot.next_record;
	request->capacity_sectors = ms->snapshot.max_records;

	/* Publishes the snapshot device the request created. */
	if (ms->snapshot_disk != NULL) {
		kern_memcpy(request->device, ms->snapshot_disk->d_name,
		       sizeof(request->device));
	}

	/* Succeeded. */
	return 0;
}

/* Refuses an unmount the volume is not ready for. */
static int
ufs_prepare_unmount(
	struct mount *mountp)
{
	int error;
	struct ufs_mount_state *ms;

	/* Takes the mount state this call runs against. */
	ms = state(mountp);

	/* Takes the snapshot device out of service before unmounting. */
	if (ms != NULL && ms->snapshot_disk != NULL)
		return EBUSY;

	/* Takes the journal out of service with everything committed and home. */
	if (ms != NULL && ms->j3.active) {
		error = j3_close(mountp, ms, 1);
		if (error != 0)
			return error;
	}

	/* Lets the volume's writes go through from here on. */
	if (ms != NULL && ms->write_cached)
		mountp->m_disk->d_flags &= ~DISK_WRITE_CACHED;

	/*
	 * Marks a writable volume clean on the way out; the clean marker's
	 * sync writes out what was delayed.
	 */
	error = 0;
	if (ms != NULL && ms->writable)
		error = ufs_write_clean(mountp, 1);

	/* Delays the writes again when the volume stays mounted. */
	if (error != 0 &&
	    ms != NULL &&
	    ms->write_cached)
		mountp->m_disk->d_flags |= DISK_WRITE_CACHED;

	/* Reports why the volume could not be left clean. */
	if (error != 0)
		return error;

	/* Succeeded: the volume is left clean. */
	return 0;
}

/* Checks local ownership without marking a lost medium clean. */
static int
ufs_prepare_unmount_revoked(
	struct mount *mountp)
{
	struct ufs_mount_state *ms;

	/* Only an initialized mount on an irrevocably lost medium is eligible. */
	if (mountp == NULL)
		return EINVAL;
	if (mountp->m_disk == NULL)
		return EINVAL;
	if (disk_media_status(mountp->m_disk) == 0)
		return EINVAL;
	ms = state(mountp);
	if (ms == NULL)
		return EINVAL;

	/* A published snapshot retains independent ownership of this volume. */
	if (ms->snapshot_disk != NULL)
		return EBUSY;
	if (drv_ufs_journal_views_busy(&ms->journal))
		return EBUSY;

	/* The caller still owns all namespace, inode and cache preparation. */
	return 0;
}

/* Disables backend reclaim before any final file or inode owner is released. */
static void
ufs_commit_unmount_revoked(
	struct mount *mountp)
{
	struct ufs_mount_state *ms;

	if (ufs_prepare_unmount_revoked(mountp) != 0)
		KERN_FATAL("UFS revoked commit without eligible owners");
	if (mountp->m_state != MOUNT_STATE_DYING)
		KERN_FATAL("UFS revoked commit without closed admission");

	/* This is local disposal, not an on-disk read-only or clean transition. */
	ms = state(mountp);
	drv_ufs_journal_views_close(&ms->journal);
	ms->writable = 0;
}

/* Takes a mount out of service and gives its state back. */
static void
ufs_unmount(
	struct mount *mountp)
{
	struct ufs_mount_state *ms;

	/* A mount that was never set up has nothing to release. */
	if (mountp == NULL)
		return;
	if (mountp->m_data == NULL)
		return;

	/*
	 * Drops a journal still in service without a commit; an orderly
	 * unmount has already taken it out of service with everything home.
	 */
	ms = state(mountp);
	if (ms->j3.active)
		(void)j3_close(mountp, ms, 0);

	/* Lets the disk's writes go through again. */
	if (ms->write_cached && mountp->m_disk != NULL)
		mountp->m_disk->d_flags &= ~DISK_WRITE_CACHED;

	/* Gives the mount's state back. */
	ufs_state_free(ms);
	mountp->m_data = NULL;
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

	/* Rejects a range this path cannot write back. */
	if (offset < 0 || length == 0 || inode->i_type != INODE_REG)
		return 0;

	/* Serializes the allocation proof with backend mutations. */
	mutex_lock(&inode->i_lock);

	/* A read-only volume, or a range past the end, writes nothing back. */
	if (!ms->writable || offset > inode->i_size ||
	    (uint64_t)length > (uint64_t)(inode->i_size - offset)) {
		mutex_unlock(&inode->i_lock);

		/* Succeeded. */
		return 0;
	}

	/* Requires every touched block to have a published allocation. */
	logical = (uint64_t)offset / ms->super.bsize;
	last = ((uint64_t)offset + length - 1U) / ms->super.bsize;
	/* Walks the blocks the range covers. */
	for (; logical <= last; logical++) {
		/* Resolves the block this offset falls in. */
		error = bmap(inode, logical, &fragment);
		if (error != 0 || fragment == 0) {
			mutex_unlock(&inode->i_lock);

			/* Failed: the caller expects a negative error here. */
			if (error != 0)
				return -error;

			/* Succeeded. */
			return 0;
		}
	}

	mutex_unlock(&inode->i_lock);

	/* Reports that every block of the range is mapped and written. */
	return 1;
}

/* Computes the checksum a journal record carries. */
static uint32_t
checksum(
	const void *buffer,
	size_t length)
{
	const uint8_t *bytes = buffer;
	uint32_t value = 2166136261U;
	size_t index;

	/* Folds every byte into the running value. */
	for (index = 0; index < length; index++) {
		value ^= bytes[index];
		value *= 16777619U;
	}

	/* Reports the checksum. */
	return value;
}

/* Writes a 32-bit field of a journal record. */
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

/* Writes a 64-bit field of a journal record. */
static void
put64(
	uint8_t *p,
	uint64_t v)
{
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}

/* Reads a 32-bit field of a journal record. */
static uint32_t
get32(
	const uint8_t *p)
{
	/* A journal record is stored little-endian, whatever the volume is. */
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
		(uint32_t)p[3] << 24;
}

/* Reads a 64-bit field of a journal record. */
static uint64_t
get64(
	const uint8_t *p)
{
	uint64_t value;

	/* The two halves assembled into one value. */
	value = get32(p) | (uint64_t)get32(p + 4) << 32;

	/* Reports the assembled value. */
	return value;
}

/* Blanks one sector of the journal. */
static int
clear_record(
	struct ufs_journal *journal,
	uint64_t sector)
{
	int cleared;
	uint8_t zero[SECTOR_SIZE];
	int error;

	kern_memset(zero, 0, sizeof(zero));
	error = journal->io.write(journal->io.context, sector, 1, zero);

	/* The clear is only durable once the device has it. */
	if (error != 0)
		cleared = error;
	else
		cleared = journal->io.flush(journal->io.context);

	/* Reports how the clear went. */
	return cleared;
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

	/* Reports the digest of the whole descriptor. */
	return digest;
}

/*
 * Rejects malformed, overlapping or out-of-volume redo before any home write.
 */
static int
group_validate(
	struct ufs_journal *journal,
	uint8_t *descriptor)
{
	uint64_t target;
	uint64_t previous;
	uint64_t sequence;
	uint32_t previous_sectors;
	uint32_t sectors;
	uint32_t total;
	uint32_t claimed_total;
	uint32_t count;
	uint32_t magic;
	uint32_t version;
	uint32_t stored_checksum;
	uint32_t computed_checksum;
	unsigned index;
	unsigned other;
	const uint8_t *entry;
	const uint8_t *prior;

	/* The magic word and version the descriptor identifies itself by. */
	magic = get32(descriptor);
	version = get32(descriptor + 4);

	/* The sequence number the group was published under. */
	sequence = get64(descriptor + 8);

	/* How many targets the descriptor says it carries. */
	count = get32(descriptor + 16);

	/* The checksum the descriptor was written with. */
	stored_checksum = get32(descriptor + 28);
	computed_checksum = group_checksum(descriptor);

	/* A sector without the magic word holds no descriptor at all. */
	if (magic != DESC_MAGIC)
		return EIO;	/* Failed. */

	/* Nor is a descriptor of another version one this driver can read. */
	if (version != GROUP_VERSION)
		return EIO;	/* Failed. */

	/* Sequence zero means unused, and the last value has no successor. */
	if (sequence == 0 || sequence == UINT64_MAX)
		return EIO;	/* Failed. */

	/* A group carries one target at least, and no more than fits. */
	if (count == 0 || count > UFS_JOURNAL_EXTENTS)
		return EIO;	/* Failed. */

	/* A checksum that disagrees means it was never fully written. */
	if (stored_checksum != computed_checksum)
		return EIO;	/* Failed. */

	/*
	 * Checks all addresses before reading payloads or changing persistent
	 * homes.
	 */
	total = 0;

	/* Walks the targets the descriptor claims. */
	for (index = 0; index < count; index++) {
		entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
		target = get64(entry);

		/* The sectors this target covers. */
		sectors = get32(entry + 8);
		if (sectors == 0 ||
		    sectors > UFS_JOURNAL_GROUP_SECTORS - total ||
		    target >= journal->home_sectors ||
		    sectors > journal->home_sectors - target) {
			/* Failed. */
			return EIO;
		}
		/* Refuses a target that overlaps one already in the group. */
		for (other = 0; other < index; other++) {
			prior = descriptor + GROUP_HEADER + other * GROUP_ENTRY;

			/* The first sector and length of the earlier target. */
			previous = get64(prior);
			previous_sectors = get32(prior + 8);

			/* Two targets that overlap cannot both be replayed. */
			if (target < previous + previous_sectors &&
			    previous < target + sectors) {
				/* Failed. */
				return EIO;
			}
		}

		total += sectors;
	}

	/* The total number of sectors the descriptor claims to carry. */
	claimed_total = get32(descriptor + 20);

	/* Refuses a descriptor whose totals disagree with its targets. */
	if (total != claimed_total ||
	    total > journal->sector_count - 2U) {
		/* Failed. */
		return EIO;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Requires a caller's committed identity when checkpoint follows publication.
 */
static int
journal_replay(
	struct ufs_journal *journal,
	uint64_t expected_sequence,
	uint32_t expected_digest,
	int apply,
	uint8_t *view)
{
	int finished;
	uint8_t descriptor[SECTOR_SIZE];
	uint8_t commit[SECTOR_SIZE];
	uint8_t sector[SECTOR_SIZE];
	const uint8_t *entry;
	uint64_t cursor;
	uint64_t sequence;
	uint64_t commit_sequence;
	uint32_t digest;
	uint32_t stored_digest;
	uint32_t descriptor_digest;
	uint32_t commit_magic;
	uint32_t commit_version;
	uint32_t commit_checksum;
	uint32_t computed_checksum;
	uint32_t count;
	uint32_t offset;
	uint32_t closed;
	unsigned index;
	unsigned part;
	unsigned byte;
	int busy;
	int error;

	/* Refuses a missing owner before reading its reserved slot. */
	if (journal == NULL)
		return EINVAL;

	/*
	 * Reuses only the immutable image of this owner's verified pending
	 * identity.
	 */
	if (journal->image_valid) {
		kern_memcpy(descriptor, journal->image, SECTOR_SIZE);

		/*
		 * Reads the sequence number the descriptor was written under.
		 */
		sequence = get64(descriptor + 8);

		/* And the checksum that stands for the group's identity. */
		descriptor_digest = get32(descriptor + 28);

		/* An image that is not the caller's own group is not usable. */
		if (sequence != expected_sequence ||
		    descriptor_digest != expected_digest) {
			/* Failed. */
			return EIO;
		}

		count = get32(descriptor + 16);
	} else {
		/*
		 * Retired readers still own the old bytes even after its slot
		 * was cleared.
		 */
		busy = drv_ufs_journal_views_busy(journal);
		if (busy)
			return EBUSY;

		/*
		 * An empty descriptor is the durable terminal state of the
		 * slot.
		 */
		error = journal->io.read(journal->io.context,
					 journal->first_sector,
					 1,
					 descriptor);
		if (error != 0)
			return error;

		/* The first word of an unwritten descriptor is zero. */
		stored_digest = get32(descriptor);

		/*
		 * An empty descriptor means the slot holds no group to replay.
		 */
		if (stored_digest == 0) {
			/*
			 * A caller waiting for a sequence learns that it is not
			 * there.
			 */
			if (expected_sequence != 0)
				return EIO;

			journal->pending_sequence = 0;
			journal->pending_digest = 0;
			journal->pending_ready = 0;

			/* Succeeded. */
			return 0;
		}

		/* Refuses a group whose own record does not hold together. */
		error = group_validate(journal, descriptor);
		if (error != 0)
			return error;

		/*
		 * A writer must verify its own group, not merely any valid redo
		 * transaction.
		 */
		sequence = get64(descriptor + 8);

		/* And the checksum that stands for the group's identity. */
		descriptor_digest = get32(descriptor + 28);

		/* A caller naming a sequence must be given that one. */
		if (expected_sequence != 0 &&
		    (sequence != expected_sequence ||
		     descriptor_digest != expected_digest)) {
			/* Failed. */
			return EIO;
		}

		count = get32(descriptor + 16);

		/* Reads the payload the group promised to write. */
		error = journal->io.read(journal->io.context,
					 journal->first_sector +
					 journal->sector_count - 1U,
					 1, commit);
		if (error != 0)
			return error;

		/*
		 * Drops uncommitted redo, whose home blocks have never been
		 * installed.
		 */
		commit_magic = get32(commit);
		commit_version = get32(commit + 4);
		commit_sequence = get64(commit + 8);

		/* The group identity and the checksum the record carries. */
		stored_digest = get32(commit + 16);
		commit_checksum = get32(commit + 24);
		computed_checksum = checksum(commit, 24);
		/*
		 * A commit record that does not name this exact group, in this
		 * version, with a checksum of its own that holds, stands for
		 * redo that was never completed.
		 */
		if (commit_magic != COMMIT_MAGIC ||
		    commit_version != GROUP_VERSION ||
		    commit_sequence != sequence ||
		    stored_digest != descriptor_digest ||
		    commit_checksum != computed_checksum) {
			/*
			 * A caller waiting for a sequence learns which one is
			 * present.
			 */
			if (expected_sequence != 0)
				return EIO;

			/*
			 * Clears a slot whose group must not be replayed again.
			 */
			error = clear_record(journal, journal->first_sector);
			if (error == 0) {
				journal->pending_sequence = 0;
				journal->pending_digest = 0;
				journal->pending_ready = 0;
			}

			/* Failed. */
			return error;
		}

		/*
		 * Fetches one bounded immutable payload image when the owner
		 * supplied storage.
		 */
		if (journal->image != NULL) {
			/* Reads the descriptor the replay works from. */
			error = journal->io.read(journal->io.context,
						 journal->first_sector + 1U,
						 get32(descriptor + 20),
						 journal->image + SECTOR_SIZE);
			if (error != 0)
				return error;
		}

		/* Validates all payload extents before the first home write. */
		offset = SECTOR_SIZE;
		cursor = journal->first_sector + 1U;

		/*
		 * Checks every target of the group before writing any of them.
		 */
		for (index = 0; index < count; index++) {
			entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
			digest = 2166136261U;

			/* Walks the sectors this target covers. */
			for (part = 0; part < get32(entry + 8); part++) {
				/*
				 * A bound image serves the payload instead of
				 * the device.
				 */
				if (journal->image != NULL) {
					kern_memcpy(sector, journal->image + offset,
					       SECTOR_SIZE);
					offset += SECTOR_SIZE;
				} else {
					/* Reads one payload sector. */
					error = journal->io.read(
						journal->io.context, cursor++,
						1, sector);
					if (error != 0)
						return error;
				}

				/*
				 * Folds the sector into the digest the group
				 * recorded.
				 */
				for (byte = 0; byte < SECTOR_SIZE; byte++) {
					digest ^= sector[byte];
					digest *= 16777619U;
				}
			}

			/* The digest the entry recorded for its payload. */
			stored_digest = get32(entry + 12);

			/*
			 * A digest that disagrees means it was never written.
			 */
			if (digest != stored_digest)
				return EIO;
		}

		/* A bound image serves the payload instead of the device. */
		if (journal->image != NULL) {
			kern_memcpy(journal->image, descriptor, SECTOR_SIZE);
			journal->image_valid = 1;
		}
	}

	/*
	 * Keeps the verified identity strict across any later failed home
	 * installation.
	 */
	journal->pending_sequence = sequence;
	journal->pending_digest = get32(descriptor + 28);
	journal->pending_ready = 1;
	journal->committed_sequence = sequence;
	journal->committed_digest = journal->pending_digest;

	/*
	 * Opens acquisition once, after every immutable byte and witness is
	 * validated.
	 */
	if (journal->image_valid && !journal->poisoned) {
		closed = IMAGE_READERS_CLOSED;

		(void)__atomic_compare_exchange_n(
			&journal->image_readers, &closed, 0, 0,
			__ATOMIC_RELEASE, __ATOMIC_RELAXED);
	}

	/* A validation-only pass stops once the group holds together. */
	if (!apply) {
		/* Releases the view the caller was given. */
		if (view != NULL)
			kern_memcpy(view, descriptor, SECTOR_SIZE);

		/* Succeeded. */
		return 0;
	}

	/*
	 * Installs checked homes, then releases the slot only after their flush
	 * succeeds.
	 */
	offset = SECTOR_SIZE;
	cursor = journal->first_sector + 1U;

	/* Writes every target of the group, now that all of them are sound. */
	for (index = 0; index < count; index++) {
		/* Takes the next target of the group. */
		entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
		if (journal->image_valid) {
			/* Writes one target sector. */
			error = journal->io.write(
				journal->io.context, get64(entry),
				get32(entry + 8), journal->image + offset);
			if (error != 0)
				return error;
			offset += get32(entry + 8) * SECTOR_SIZE;
			continue;
		}

		/* Walks the sectors this target covers. */
		for (part = 0; part < get32(entry + 8); part++) {
			/* Reads one payload sector. */
			error = journal->io.read(journal->io.context, cursor++,
						 1, sector);
			if (error == 0) {
				error = journal->io.write(journal->io.context,
							  get64(entry) + part,
							  1, sector);
			}
			if (error != 0)
				return error;
		}
	}

	/* The replay is only durable once the device has it. */
	error = journal->io.flush(journal->io.context);
	if (error != 0)
		return error;

	/*
	 * Homes are durable even if clearing the descriptor has an uncertain
	 * result.
	 */
	journal->pending_clearing = 1;

	/* Retires the slot now that its group has been written out. */
	finished = journal_finish(journal);

	/* Reports whether the slot could be retired. */
	return finished;
}

/* Retries only slot retirement after home durability is already established. */
static int
journal_finish(
	struct ufs_journal *journal)
{
	int error;

	/*
	 * Keeps the home-durable witness until clearing also crosses its flush
	 * boundary.
	 */
	error = clear_record(journal, journal->first_sector);
	if (error != 0)
		return error;
	journal_close_views(journal);

	/* A sequence at or past the next one means the slot is corrupt. */
	if (journal->pending_sequence >= journal->next_sequence)
		journal->next_sequence = journal->pending_sequence + 1U;

	journal->pending_sequence = 0;
	journal->pending_digest = 0;
	journal->pending_ready = 0;
	journal->pending_clearing = 0;
	journal->image_valid = 0;

	/* Succeeded. */
	return 0;
}

/*
 * Closes new acquisition without revoking readers that already own the image.
 */
static void
journal_close_views(
	struct ufs_journal *journal)
{
	(void)__atomic_fetch_or(&journal->image_readers, IMAGE_READERS_CLOSED,
				__ATOMIC_ACQ_REL);
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
	/* Walks the run the caller asked for, one target at a time. */
	while (done < count) {
		current = first + done;
		offset = SECTOR_SIZE;
		run = 0;
		/*
		 * Walks the targets of the group for one that covers the run.
		 */
		for (index = 0; index < get32(view->image + 16); index++) {
			entry = view->image + GROUP_HEADER +
				index * GROUP_ENTRY;
			target = get64(entry);

			/* The sectors this target covers. */
			sectors = get32(entry + 8);
			if (current >= target && current - target < sectors) {
				/*
				 * Clamps the run to what this target actually
				 * holds.
				 */
				run = sectors - (uint32_t)(current - target);
				if (run > count - done)
					run = count - done;
				offset += (uint32_t)(current - target) *
					SECTOR_SIZE;
				break;
			}

			offset += sectors * SECTOR_SIZE;
		}

		/* A target that covers nothing of the run is skipped. */
		if (run == 0)
			return ENOENT;

		/* Copies out only when the caller asked for the bytes. */
		if (copy) {
			kern_memcpy((uint8_t *)buffer + (size_t)done * SECTOR_SIZE,
			       view->image + offset, (size_t)run * SECTOR_SIZE);
		}

		done += run;
	}

	/* Succeeded. */
	return 0;
}

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

/* Writes a 64-bit field of a snapshot record. */
static void
snapshot_put64(
	uint8_t *p,
	uint64_t v)
{
	snapshot_put32(p, (uint32_t)v);
	snapshot_put32(p + 4, (uint32_t)(v >> 32));
}

/* Reads a 32-bit field of a snapshot record. */
static uint32_t
snapshot_get32(
	const uint8_t *p)
{
	/* A snapshot record is stored little-endian, whatever the volume is. */
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
		(uint32_t)p[3] << 24;
}

/* Reads a 64-bit field of a snapshot record. */
static uint64_t
snapshot_get64(
	const uint8_t *p)
{
	uint64_t value;

	/* The two halves assembled into one value. */
	value = snapshot_get32(p) | (uint64_t)snapshot_get32(p + 4)
		<< 32;

	/* Reports the assembled value. */
	return value;
}

/* Computes the checksum a snapshot record carries. */
static uint32_t
digest(
	const void *buffer,
	size_t length)
{
	const uint8_t *p = buffer;
	uint32_t value = 2166136261U;
	size_t n;

	/* Folds every byte into the running value. */
	for (n = 0; n < length; n++) {
		value ^= p[n];
		value *= 16777619U;
	}

	/* Reports the digest. */
	return value;
}

/* Reports which map slot a volume sector hashes to. */
static size_t
hash_sector(
	uint64_t sector,
	size_t count)
{
	sector ^= sector >> 33;
	sector *= 0xff51afd7ed558ccdULL;
	sector ^= sector >> 33;

	/* The map is open-addressed, so a sector hashes to its first slot. */
	return (size_t)(sector % count);
}

/* Finds, or makes, the map entry that stands for one sector. */
static struct ufs_snapshot_entry *
map_find(
	struct ufs_snapshot *snapshot,
	uint64_t sector,
	int insert)
{
	struct ufs_snapshot_entry *entry;
	size_t start;
	size_t slot;

	start = hash_sector(sector, snapshot->map_count);
	slot = start;

	do {
		/* The slot this probe lands on. */
		entry = &snapshot->map[slot];
		if (entry->sector == sector)
			return entry;

		/* A free slot ends the probe. */
		if (entry->sector == UFS_SNAPSHOT_EMPTY) {
			/*
			 * An insertion claims the first free slot it reaches.
			 */
			if (insert)
				return entry;

			/*
			 * A lookup that reaches a free slot has not found it.
			 */
			return NULL;
		}
		slot = (slot + 1U) % snapshot->map_count;
	} while (slot != start);

	/* Reports that no result is available. */
	return NULL;
}

/* Empties the map of preserved sectors. */
static void
map_clear(
	struct ufs_snapshot *snapshot)
{
	size_t n;

	/* Marks every slot of the map free. */
	for (n = 0; n < snapshot->map_count; n++) {
		snapshot->map[n].sector = UFS_SNAPSHOT_EMPTY;
		snapshot->map[n].record = 0;
	}
}

/* Writes the control sector that describes a snapshot. */
static int
write_control(
	struct ufs_snapshot *snapshot,
	unsigned active,
	uint32_t next)
{
	uint8_t sector[SECTOR_SIZE];
	int error;

	/* Builds the control sector the snapshot is described by. */
	kern_memset(sector, 0, sizeof(sector));

	/* The signature and version a reader identifies the sector by. */
	kern_memcpy(sector, "ZSN1", 4);
	snapshot_put32(sector + 4, SNAPSHOT_VERSION);

	/* Whether a snapshot is in progress at all. */
	snapshot_put32(sector + 8, active ? SNAPSHOT_ACTIVE : 0);

	/* How many records were written, and how many there is room for. */
	snapshot_put32(sector + 12, next);
	snapshot_put32(sector + 16, snapshot->max_records);

	/* The size of the volume the snapshot was taken against. */
	snapshot_put64(sector + 24, snapshot->volume_sectors);

	/* And a checksum over everything above. */
	snapshot_put32(sector + 32, digest(sector, 32));
	error = snapshot->io.write(snapshot->io.context, snapshot->first_sector,
				   1, sector);
	if (error != 0)
		return error;	/* Failed. */

	/* The control sector is only durable once the device has it. */
	error = snapshot->io.flush(snapshot->io.context);
	if (error != 0)
		return error;	/* Failed. */

	/* Succeeded. */
	return 0;
}

/* Tests whether a value is a power of two. */
static int
power2(
	uint32_t value)
{
	/* Zero is not a power of two. */
	if (value == 0)
		return 0;

	/* A power of two has exactly one bit, so clearing it leaves nothing. */
	if ((value & (value - 1U)) != 0)
		return 0;

	/* Reports that the value is a power of two. */
	return 1;
}

/*
 * Makes the writes so far durable before a write that depends on them.
 *
 * A write-through volume keeps its metadata consistent on the device at
 * every step, so that a crash leaves a volume that mounts.  A write-cached
 * volume gives that up for speed: the barrier is skipped, and fsync, sync
 * and unmount are what make its writes durable.
 */
static int
order_barrier(
	struct mount *mountp)
{
	int error;

	/* Skips the barrier while the volume's writes are delayed. */
	if ((mountp->m_disk->d_flags & DISK_WRITE_CACHED) != 0)
		return 0;

	/* Writes and flushes everything before the dependent write. */
	error = disk_sync(mountp->m_disk);
	if (error != 0)
		return error;

	/* Succeeded: the earlier writes are on the device. */
	return 0;
}

/*
 * Delays a writable volume's writes in the cache from here on, unless the
 * mount asked for write-through or the volume has the tail journal, whose
 * records must reach the device before the blocks they cover.  The batched
 * journal carries the metadata unless the mount declines it.
 */
static void
write_cached_start(
	struct mount *mountp,
	struct ufs_mount_state *ms,
	struct inode *root)
{
	int intended;
	int error;

	/* Leaves the writes going through when the mount is not to delay them. */
	intended = write_cached_intended(mountp, ms);
	if (!intended)
		return;

	/* Starts the flusher, without which delayed writes would never go out. */
	error = buf_flusher_start();
	if (error != 0)
		return;

	/*
	 * Journals the metadata unless the mount declines; a volume whose
	 * journal cannot be opened or made goes on without one.
	 */
	if ((mountp->m_flags & MOUNT_NO_JOURNAL) == 0) {
		error = j3_open(mountp, ms, root);

		/* Says so when a volume large enough for a journal goes on without one. */
		if (error != 0 && error != ENOENT) {
			kern_logf("ufs: %s: the batched journal could not be opened or made (error %d); the volume goes on without one\n",
				  mountp->m_disk->d_name,
				  error);
		}
	}

	/* Delays the volume's writes in the cache from here on. */
	ms->write_cached = 1;
	mountp->m_disk->d_flags |= DISK_WRITE_CACHED;
}

/*
 * Tests whether a writable mount is to delay its writes: a volume without
 * the tail journal, mounted without write-through, on a writable disk.
 */
static int
write_cached_intended(
	const struct mount *mountp,
	const struct ufs_mount_state *ms)
{
	/* A read-only mount writes nothing that could be delayed. */
	if ((mountp->m_flags & MOUNT_READ_ONLY) != 0)
		return 0;

	/* A write-through mount asked for every write to reach the device. */
	if ((mountp->m_flags & MOUNT_WRITE_THROUGH) != 0)
		return 0;

	/* A read-only disk takes no writes at all. */
	if ((mountp->m_disk->d_flags & DISK_READ_ONLY) != 0)
		return 0;

	/* The tail journal writes each change through itself, in order. */
	if (ms->journal_enabled)
		return 0;

	/* Succeeded: reports that the mount's writes are to be delayed. */
	return 1;
}

/*
 * The batched metadata journal (v3).
 *
 * The journal lives in the regular file `.ufs-journal` of the root
 * directory.  Its first block is a header naming the runs (extents) of
 * disk sectors the file occupies, which need not be contiguous; the
 * superblock's spare words name the header (the locator) and hold the size
 * mkfs recorded for it (the request).  Journal I/O goes to those sectors
 * directly, never through the file layer.
 *
 * After the header come two slots, used in turn by the sequence number of
 * the transaction they hold.  A slot is a descriptor listing the ranges the
 * transaction logged, the ranges' contents (the payload), and in its last
 * sector a commit record that seals the descriptor.  Replay applies the
 * newest sealed transaction the header has not recorded as applied.
 */

/* Names the disk sector of a sector of the journal file. */
static uint64_t
j3_sector(
	const struct ufs_j3 *j3,
	uint64_t file_sector,
	uint32_t *run)
{
	const struct ufs_j3_extent *extent;
	uint64_t offset;
	unsigned low;
	unsigned high;
	unsigned middle;

	/*
	 * Searches the extents, which are in file order, for the last one that
	 * starts at or before the sector.
	 */
	low = 0;
	high = j3->extent_count;
	while (high - low > 1U) {
		middle = low + (high - low) / 2U;

		/* Keeps the half that can still hold the sector. */
		if (j3->extents[middle].file_sector <= file_sector)
			low = middle;
		else
			high = middle;
	}

	/* Locates the sector inside that extent, and the sectors after it there. */
	extent = &j3->extents[low];
	offset = file_sector - extent->file_sector;
	*run = extent->count - (uint32_t)offset;

	/* Reports the disk sector. */
	return extent->lba + offset;
}

/* Reads or writes sectors of the journal file, an extent at a time. */
static int
j3_io(
	struct mount *mountp,
	const struct ufs_j3 *j3,
	uint64_t file_sector,
	uint32_t count,
	void *buffer,
	int write)
{
	uint8_t *bytes;
	uint64_t lba;
	uint32_t amount;
	int error;

	/* Refuses sectors past the end of the journal. */
	if (file_sector + count > j3->total_sectors)
		return EINVAL;

	/* Transfers the run inside each extent in turn. */
	bytes = buffer;
	while (count != 0) {
		/* Finds where the next sectors lie, and how many follow there. */
		lba = j3_sector(j3, file_sector, &amount);

		/* Takes no more of the run than the transfer still needs. */
		if (amount > count)
			amount = count;

		/* Moves the run between the buffer and the disk. */
		if (write) {
			error = disk_sector_transfer(
				mountp->m_disk,
				lba,
				amount,
				(void *)bytes,
				1,
				0,
				NULL);
		} else {
			error = observed_disk_read(
				mountp->m_disk,
				lba,
				amount,
				bytes);
		}

		/* Reports why the run could not be moved. */
		if (error != 0)
			return error;

		/* Steps past the run. */
		bytes += (size_t)amount * SECTOR_SIZE;
		file_sector += amount;
		count -= amount;
	}

	/* Succeeded: every sector was moved. */
	return 0;
}

/*
 * Writes the journal file's sectors that wait in the cache, an extent at a
 * time, the ones in a line a journal pins included: such a line, which a
 * journal's slot shares with a block the transaction logs on a volume
 * whose lines do not fall on its blocks, is left by a whole write-back
 * until the commit, while what the slot holds has to be on the device
 * before the commit record that seals it (BUG-307).  The device is not
 * flushed.
 */
static int
j3_writeback(
	struct mount *mountp,
	const struct ufs_j3 *j3,
	uint64_t file_sector,
	uint32_t count)
{
	uint64_t lba;
	uint32_t amount;
	int error;

	/* Refuses sectors past the end of the journal. */
	if (file_sector + count > j3->total_sectors)
		return EINVAL;

	/* Writes the run inside each extent in turn. */
	while (count != 0) {
		/* Finds where the next sectors lie, and how many follow there. */
		lba = j3_sector(j3, file_sector, &amount);

		/* Takes no more of the run than the write still needs. */
		if (amount > count)
			amount = count;

		/* Writes the run's cached dirty sectors. */
		error = ufs_buf_writeback_cached(mountp->m_disk, lba, amount);
		if (error != 0)
			return error;

		/* Steps past the run. */
		file_sector += amount;
		count -= amount;
	}

	/* Succeeded: the sectors are on their way to the device. */
	return 0;
}

/* Makes the writes so far durable on the device. */
static int
j3_durable(
	struct mount *mountp)
{
	int error;

	/* Writes the unpinned dirty buffers, then flushes the device. */
	error = disk_sync(mountp->m_disk);
	if (error != 0)
		return error;

	/* Succeeded: the writes so far are durable. */
	return 0;
}

/* Hashes a key into a power-of-two table. */
static unsigned
j3_hash(
	uint64_t key,
	unsigned size)
{
	unsigned mixed;

	/* Spreads neighbouring keys with a multiplicative hash. */
	mixed = (unsigned)((key * UINT64_C(0x9e3779b97f4a7c15)) >> 40);

	/* Reports the slot the hash names in the table. */
	return mixed & (size - 1U);
}

/* Adds a range to the running transaction, once for each exact range. */
static void
j3_range_add(
	struct ufs_j3 *j3,
	uint64_t lba,
	uint32_t count,
	uint32_t hold)
{
	struct ufs_j3_range *range;
	unsigned size;
	unsigned slot;
	unsigned position;

	/* Probes the index for the range already recorded, or an empty slot. */
	size = 2U * j3->ranges_max;
	slot = j3_hash(lba, size);
	for (;;) {
		/* An empty slot ends the probe: the range is new. */
		position = j3->index[slot];
		if (position == 0)
			break;

		/* The same range is already in the transaction. */
		range = &j3->ranges[position - 1U];
		if (range->lba == lba &&
		    range->count == count &&
		    range->hold == hold)
			return;

		/* Probes the next slot. */
		slot = (slot + 1U) & (size - 1U);
	}

	/*
	 * Records the new range.  The index holds the range's position plus
	 * one, so that zero marks an empty slot.
	 */
	range = &j3->ranges[j3->range_count];
	range->lba = lba;
	range->count = count;
	range->hold = hold;
	j3->range_count++;
	j3->index[slot] = j3->range_count;

	/* Counts the sectors the commit copies; a held range is only pinned. */
	if (!hold)
		j3->logged_sectors += count;
}

/* Asks whether a block is in a set of blocks a transaction freed. */
static int
j3_freed_test(
	const uint64_t *set,
	uint64_t fragment)
{
	unsigned slot;

	/*
	 * Probes the open-addressed set from the block's hash.  The set holds
	 * each block plus one, so that zero marks an empty slot.
	 */
	slot = j3_hash(fragment, J3_FREED_MAX);
	while (set[slot] != 0) {
		/* The block is in the set. */
		if (set[slot] == fragment + 1U)
			return 1;

		/* Probes the next slot. */
		slot = (slot + 1U) & (J3_FREED_MAX - 1U);
	}

	/* The block is not in the set. */
	return 0;
}

/*
 * Records a block the running transaction freed.
 *
 * The set takes blocks only while it is less than half full, which keeps
 * every probe short.  A block freed after that is not held back: content
 * written into it before the commit can reach the disk while a crash would
 * still give the block to its old owner.
 */
static void
j3_freed_add(
	struct ufs_mount_state *ms,
	uint64_t fragment)
{
	struct ufs_j3 *j3;
	unsigned slot;
	int known;

	/* Adds the block to the set under the journal lock. */
	j3 = &ms->j3;
	mutex_lock(&j3->lock);

	/*
	 * A journal out of service tracks nothing and a full set takes no
	 * more; either way the block counts as known.
	 */
	known = 1;
	if (j3->active && j3->freed_count < J3_FREED_MAX / 2U)
		known = j3_freed_test(j3->freed, fragment);

	/* Stores a block not yet in the set. */
	if (!known) {
		/* Finds the first empty slot of the block's probe. */
		slot = j3_hash(fragment, J3_FREED_MAX);
		while (j3->freed[slot] != 0)
			slot = (slot + 1U) & (J3_FREED_MAX - 1U);

		/* Stores the block plus one, since zero marks an empty slot. */
		j3->freed[slot] = fragment + 1U;
		j3->freed_count++;
	}

	mutex_unlock(&j3->lock);
}

/* Asks whether content sectors fall in a block freed by the transaction. */
static int
j3_content_freed(
	const struct ufs_mount_state *ms,
	uint64_t lba,
	uint32_t count)
{
	uint64_t fragment;
	uint64_t last;
	uint64_t block;
	int freed;

	/* Visits each block the sectors touch, from the block of the first. */
	fragment = lba >> ms->super.fsbtodb;
	last = (lba + count - 1U) >> ms->super.fsbtodb;
	for (block = fragment - fragment % ms->super.frag;
	     block <= last;
	     block += ms->super.frag) {
		/* Content in a block the running transaction freed waits for its commit. */
		freed = 0;
		if (ms->j3.freed_count != 0)
			freed = j3_freed_test(ms->j3.freed, block);
		if (freed)
			return 1;

		/*
		 * So does content in a block the closed transaction freed, until
		 * that commit is durable (ws073-p051).
		 */
		if (ms->j3.closed_freed_count != 0)
			freed = j3_freed_test(ms->j3.closed_freed, block);
		if (freed)
			return 1;
	}

	/* No block of the range was freed. */
	return 0;
}

/* Asks whether the running transaction has room for a write of count sectors. */
static int
j3_room(
	const struct ufs_j3 *j3,
	uint32_t count,
	int hold)
{
	/* The descriptor lists no more ranges. */
	if (j3->range_count >= j3->ranges_max)
		return 0;

	/* The slot's payload holds no more sectors; a held range is only pinned. */
	if (!hold && j3->logged_sectors + count > j3->payload_max)
		return 0;

	/* There is room. */
	return 1;
}

/* Asks whether a commit is still to be done: a transaction that runs, or a seal that failed. */
static int
j3_commit_pending(
	const struct ufs_j3 *j3)
{
	/* A commit whose sealing failed waits to be sealed again. */
	if (j3->closed_descriptor != NULL)
		return 1;

	/* A running transaction with ranges waits for its commit. */
	if (j3->range_count != 0)
		return 1;

	/* Nothing is to be committed. */
	return 0;
}

/*
 * Takes a write into the running transaction.
 *
 * Metadata is pinned in the cache and logged.  Content goes to the cache
 * as an ordinary delayed write, unless it lands in a block the running
 * transaction freed: then it is pinned until the commit, so that a crash
 * that undoes the free finds the old owner's block untouched.
 */
static int
j3_write(
	struct ufs_mount_state *ms,
	struct mount *mountp,
	uint64_t lba,
	uint32_t count,
	const void *buffer,
	const struct io_context *context,
	int content)
{
	struct ufs_j3 *j3;
	uint64_t bytes;
	int hold;
	int room;
	int pending;
	int error;

	/* Takes the write under the journal lock. */
	j3 = &ms->j3;
	bytes = (uint64_t)count * UFS_SECTOR_SIZE;
	mutex_lock(&j3->lock);

	/* Holds content that lands in a block the running or the closed transaction freed. */
	hold = 0;
	if (content && (j3->freed_count != 0 || j3->closed_freed_count != 0))
		hold = j3_content_freed(ms, lba, count);

	/* Writes other content as an ordinary delayed write, outside the journal. */
	if (content && !hold) {
		mutex_unlock(&j3->lock);

		/*
		 * Writes the content in the caller's context, whose backing claim
		 * (a formatter's lease) authorizes the write; the buffer cache
		 * keeps it for the flusher unless the claim needs it written now.
		 */
		io_stats_record(IO_UFS_WRITE, bytes);
		error = disk_sector_transfer(mountp->m_disk, lba, count,
					     (void *)buffer, 1, 0, context);
		if (error != 0)
			return error;

		/* Succeeded: the content waits in the cache. */
		return 0;
	}

	/*
	 * Commits first, in place, while the transaction has no room for the
	 * write: a seal that failed earlier, then the running transaction
	 * (ws073-p051).  The caller is in the middle of a change and holds
	 * the mount lock, so this rare commit waits for its flushes here.
	 */
	error = 0;
	room = j3_room(j3, count, hold);
	pending = j3_commit_pending(j3);
	while (!room && pending && error == 0) {
		error = j3_commit_now(ms, mountp);
		room = j3_room(j3, count, hold);
		pending = j3_commit_pending(j3);
	}

	/* Reports why the transaction could not make room. */
	if (error != 0) {
		mutex_unlock(&j3->lock);
		return error;
	}

	/*
	 * Pins the sectors in the cache under the running transaction's
	 * sequence, where they wait for its commit; a line a commit being
	 * written still holds now waits for this one instead.
	 */
	error = ufs_buf_write_pinned(mountp->m_disk, lba, count, buffer, j3->sequence);
	if (error != 0) {
		mutex_unlock(&j3->lock);
		return error;
	}

	/* Records the pinned sectors in the running transaction. */
	io_stats_record(IO_UFS_WRITE, bytes);
	j3_range_add(j3, lba, count, (uint32_t)hold);

	mutex_unlock(&j3->lock);

	/* Succeeded: the write is part of the running transaction. */
	return 0;
}

/*
 * Derives the slot layout from the journal's size: two slots after the
 * header, each a descriptor, a payload and a commit record.
 */
static int
j3_geometry(
	struct ufs_j3 *j3)
{
	uint64_t slot;
	uint32_t ranges;

	/* Refuses a journal too small for a header and two slots. */
	if (j3->total_sectors <= j3->header_sectors + 4U)
		return EINVAL;

	/* Splits what follows the header into two equal slots. */
	slot = (j3->total_sectors - j3->header_sectors) / 2U;

	/* Refuses a slot whose sectors cannot be counted in 32 bits. */
	if (slot > UINT32_MAX)
		return EINVAL;

	/*
	 * Lets a larger slot name more ranges: doubles them while the slot
	 * still has 32 sectors for each, up to the limit.
	 */
	ranges = J3_RANGES_MIN;
	while (ranges < J3_RANGES_LIMIT && (uint64_t)ranges * 2U * 16U <= slot)
		ranges *= 2U;

	/* Sizes the descriptor, in whole sectors, for that many ranges. */
	j3->ranges_max = ranges;
	j3->desc_sectors_max = (J3_DESC_HEADER + ranges * J3_DESC_ENTRY + SECTOR_SIZE - 1U) / SECTOR_SIZE;

	/* Refuses a slot with no payload after the descriptor and the commit record. */
	if (slot <= (uint64_t)j3->desc_sectors_max + 2U)
		return EINVAL;

	/* Gives the payload what the descriptor and the commit record leave. */
	j3->slot_sectors = (uint32_t)slot;
	j3->payload_max = j3->slot_sectors - j3->desc_sectors_max - 1U;

	/* Succeeded: the slots are laid out. */
	return 0;
}

/* Writes the header block: identity, the last applied commit, the extents. */
static int
j3_write_header(
	struct mount *mountp,
	struct ufs_j3 *j3)
{
	uint8_t *header;
	uint8_t *entry;
	size_t bytes;
	uint32_t sum;
	unsigned n;
	int error;

	/* Takes an image of the header's sectors. */
	bytes = (size_t)j3->header_sectors * SECTOR_SIZE;
	header = kern_malloc(bytes);
	if (header == NULL)
		return ENOMEM;

	/*
	 * Stores the fixed words: the magic and the version, the journal's
	 * identity (the nonce), the sequence of the last applied commit, the
	 * journal's size in sectors, the slot size, the number of extents,
	 * and the block and header sizes in sectors.
	 */
	kern_memset(header, 0, bytes);
	put32(header, J3_HEADER_MAGIC);
	put32(header + 4, J3_VERSION);
	put64(header + 8, j3->nonce);
	put64(header + 16, j3->applied);
	put64(header + 24, j3->total_sectors);
	put32(header + 32, j3->slot_sectors);
	put32(header + 36, j3->extent_count);
	put32(header + 40, j3->block_sectors);
	put32(header + 44, j3->header_sectors);

	/* Lists each extent: its first disk sector and its length. */
	for (n = 0; n < j3->extent_count; n++) {
		entry = header + J3_HEADER_FIXED + J3_EXTENT_ENTRY * n;
		put64(entry, j3->extents[n].lba);
		put32(entry + 8, j3->extents[n].count);
	}

	/* Seals the extent list, then the fixed words, each with a checksum. */
	sum = checksum(header + J3_HEADER_FIXED, (size_t)J3_EXTENT_ENTRY * j3->extent_count);
	put32(header + 48, sum);
	sum = checksum(header, 52);
	put32(header + 52, sum);

	/* Writes the header where the journal's first sectors lie. */
	error = j3_io(mountp, j3, 0, j3->header_sectors, header, 1);

	/* Gives the image back, written or not. */
	kern_free(header);

	/* Reports why the header could not be written. */
	if (error != 0)
		return error;

	/* Succeeded: the header is written, though not yet durable. */
	return 0;
}

/* Counts the ranges of the running transaction that a commit logs. */
static uint32_t
j3_logged_ranges(
	const struct ufs_j3 *j3)
{
	uint32_t logged;
	unsigned n;

	/* Counts every range that is not merely held. */
	logged = 0;
	for (n = 0; n < j3->range_count; n++) {
		if (!j3->ranges[n].hold)
			logged++;
	}

	/* Reports the count. */
	return logged;
}

/*
 * Copies one range's current contents into the journal, a staging buffer
 * at a time, and reports the sum replay checks them by.
 */
static int
j3_copy_range(
	struct mount *mountp,
	struct ufs_j3 *j3,
	const struct ufs_j3_range *range,
	uint64_t cursor,
	uint32_t *sum)
{
	uint32_t done;
	uint32_t amount;
	uint32_t chunk_sum;
	int error;

	/* Moves the range through the staging buffer in chunks. */
	*sum = 0;
	for (done = 0; done < range->count; done += amount) {
		/* Takes no more than the staging buffer holds. */
		amount = range->count - done;
		if (amount > J3_STAGING_SECTORS)
			amount = J3_STAGING_SECTORS;

		/* Reads the sectors' current contents, which the cache holds pinned. */
		error = observed_disk_read(mountp->m_disk, range->lba + done, amount, j3->staging);
		if (error != 0)
			return error;

		/* Folds the chunk into the range's sum. */
		chunk_sum = checksum(j3->staging, (size_t)amount * SECTOR_SIZE);
		*sum = (*sum * 16777619U) ^ chunk_sum;

		/* Writes the chunk into the slot. */
		error = j3_io(mountp, j3, cursor + done, amount, j3->staging, 1);
		if (error != 0)
			return error;
	}

	/* Succeeded: the range is in the slot. */
	return 0;
}

/*
 * Copies every logged range into the slot of the running transaction and
 * fills in the descriptor that lists them.
 */
static int
j3_commit_payload(
	struct mount *mountp,
	struct ufs_j3 *j3,
	uint8_t *descriptor,
	uint32_t desc_sectors)
{
	const struct ufs_j3_range *range;
	uint8_t *entry;
	uint64_t cursor;
	uint32_t logged;
	uint32_t sum;
	unsigned n;
	int error;

	/* Places the payload after the descriptor, in the slot the sequence picks. */
	cursor = j3->header_sectors + (j3->sequence & 1U) * j3->slot_sectors + desc_sectors;
	logged = 0;
	for (n = 0; n < j3->range_count; n++) {
		/* A held range is pinned until the commit but not logged. */
		range = &j3->ranges[n];
		if (range->hold)
			continue;

		/* Copies the range after the ones before it. */
		error = j3_copy_range(mountp, j3, range, cursor, &sum);
		if (error != 0)
			return error;

		/* Lists the range: its home, its length and its sum. */
		entry = descriptor + J3_DESC_HEADER + J3_DESC_ENTRY * logged;
		put64(entry, range->lba);
		put32(entry + 8, range->count);
		put32(entry + 12, sum);
		logged++;
		cursor += range->count;
	}

	/*
	 * Stores the fixed words: the magic and the version, the sequence and
	 * the journal's identity, the number of ranges, the descriptor's
	 * sectors, and the payload's sectors.
	 */
	put32(descriptor, J3_DESC_MAGIC);
	put32(descriptor + 4, J3_VERSION);
	put64(descriptor + 8, j3->sequence);
	put64(descriptor + 16, j3->nonce);
	put32(descriptor + 24, logged);
	put32(descriptor + 28, desc_sectors);
	put32(descriptor + 32, j3->logged_sectors);

	/* Seals the list of ranges, then the fixed words, each with a checksum. */
	sum = checksum(descriptor + J3_DESC_HEADER, (size_t)logged * J3_DESC_ENTRY);
	put32(descriptor + 36, sum);
	sum = checksum(descriptor, 40);
	put32(descriptor + 40, sum);

	/* Succeeded: the payload is in the slot and the descriptor lists it. */
	return 0;
}

/*
 * Seals the closed transaction of the given sequence: writes the descriptor
 * at the head of its slot, makes the slot durable, then writes and makes
 * durable the commit record in the slot's last sector.  From then on replay
 * applies it.
 */
static int
j3_commit_seal(
	struct mount *mountp,
	struct ufs_j3 *j3,
	uint8_t *descriptor,
	uint32_t desc_sectors,
	uint64_t sequence)
{
	uint8_t commit[SECTOR_SIZE];
	uint64_t base;
	uint64_t used;
	uint32_t desc_sum;
	uint32_t sum;
	int error;

	/* Writes the descriptor at the head of the slot the sequence picks. */
	base = j3->header_sectors + (sequence & 1U) * j3->slot_sectors;
	error = j3_io(mountp, j3, base, desc_sectors, descriptor, 1);
	if (error != 0)
		return error;

	/* Counts the sectors the descriptor and the payloads after it take in the slot. */
	used = (uint64_t)desc_sectors + get32(descriptor + 32);

	/* A slot holds no more than what its commit record leaves. */
	if (used > (uint64_t)j3->slot_sectors - 1U)
		return EINVAL;

	/*
	 * Writes those sectors, the ones in a line a journal pins included,
	 * which the whole write-back below leaves for the commit, so that the
	 * record never reaches the device before what it seals (BUG-307).
	 */
	error = j3_writeback(mountp, j3, base, (uint32_t)used);
	if (error != 0)
		return error;

	/* Makes the slot durable before anything seals it. */
	error = j3_durable(mountp);
	if (error != 0)
		return error;

	/*
	 * Builds the commit record: the magic and the version, the sequence
	 * and the journal's identity, and the checksum and size of the
	 * descriptor it seals, all under a checksum of its own.
	 */
	desc_sum = get32(descriptor + 40);
	kern_memset(commit, 0, sizeof(commit));
	put32(commit, J3_COMMIT_MAGIC);
	put32(commit + 4, J3_VERSION);
	put64(commit + 8, sequence);
	put64(commit + 16, j3->nonce);
	put32(commit + 24, desc_sum);
	put32(commit + 28, desc_sectors);
	sum = checksum(commit, 32);
	put32(commit + 32, sum);

	/* Writes the record, which makes the transaction the current one. */
	error = j3_io(mountp, j3, base + j3->slot_sectors - 1U, 1, commit, 1);
	if (error != 0)
		return error;

	/*
	 * Makes the record durable.  Everything the record seals was made
	 * durable before it was written, so only the record's own line is
	 * written and the device flushed: the rest of the cache, which writers
	 * keep dirtying meanwhile, waits for the flusher or the next commit
	 * (BUG-307).
	 */
	error = j3_writeback(mountp, j3, base + j3->slot_sectors - 1U, 1);
	if (error != 0)
		return error;
	error = bio_flush(mountp->m_disk);
	if (error != 0)
		return error;

	/* Succeeded: the transaction is committed. */
	return 0;
}

/* Releases the pins a transaction of the given sequence set on its ranges. */
static void
j3_unpin_ranges(
	struct mount *mountp,
	const struct ufs_j3_range *ranges,
	unsigned count,
	uint64_t pin)
{
	unsigned n;

	/*
	 * Lets the flusher write each range home from here on; a line a later
	 * transaction pinned again stays with that transaction.
	 */
	for (n = 0; n < count; n++)
		(void)ufs_buf_unpin(mountp->m_disk, ranges[n].lba, ranges[n].count, pin, NULL);
}

/*
 * Writes one logged range of the closed transaction home from its copy in
 * the slot, beneath the cache (ws073-p051).  For a range whose line the
 * running transaction pinned again: the cache will not write that line
 * until the running transaction commits, and that commit's payload carries
 * only its own ranges of the line, so the closed transaction's range has
 * to reach its home by itself before its slot is reused.  The cache line,
 * which holds the newer content, is left as it is and is written whole
 * later.
 */
static int
j3_home_direct(
	struct mount *mountp,
	struct ufs_j3 *j3,
	const struct ufs_j3_range *range,
	uint64_t cursor)
{
	struct backing_mutation_guard guard;
	uint64_t guard_block;
	uint32_t guard_count;
	uint32_t done;
	uint32_t amount;
	int error;

	/* Copies the range through the finisher's buffer in chunks. */
	for (done = 0; done < range->count; done += amount) {
		/* Takes no more than the buffer holds. */
		amount = range->count - done;
		if (amount > J3_STAGING_SECTORS)
			amount = J3_STAGING_SECTORS;

		/* Reads the chunk from the slot. */
		error = j3_io(mountp, j3, cursor + done, amount, j3->closed_staging, 0);
		if (error != 0)
			return error;

		/* Writes it home as the filesystem, beneath the cache, the way a delayed write-back does. */
		ufs_sector_blocks(mountp->m_disk, range->lba + done, amount, &guard_block, &guard_count);
		error = backing_mutation_begin_disk_filesystem(mountp->m_disk, guard_block, guard_count, &guard);
		if (error != 0)
			return error;
		error = disk_sector_transfer(mountp->m_disk, range->lba + done, amount, j3->closed_staging, 1, 1, NULL);
		backing_mutation_end(&guard);

		/* Reports why the chunk could not be written. */
		if (error != 0)
			return error;
	}

	/* Succeeded: the range is at its home on the device. */
	return 0;
}

/*
 * Closes the running transaction (ws073-p051): copies its logged ranges
 * into their slot, lists them in a descriptor, and moves the transaction
 * aside as the closed one for j3_finish_commit() to seal, while a new
 * empty transaction starts running.  Only memory and the cache are
 * touched, so the caller holds the mount lock and the journal lock only
 * briefly.  No commit may be being written (closing is zero).  *closed
 * reports whether there is something for j3_finish_commit() to write: the
 * transaction closed now, or an earlier one whose sealing failed and is to
 * be tried again.
 */
static int
j3_close_transaction(
	struct ufs_mount_state *ms,
	struct mount *mountp,
	int *closed)
{
	struct ufs_j3 *j3;
	struct ufs_j3_range *ranges;
	uint64_t *freed;
	uint8_t *descriptor;
	size_t bytes;
	uint32_t logged;
	uint32_t desc_sectors;
	int error;

	/* A commit whose sealing failed is sealed again before anything new is closed. */
	j3 = &ms->j3;
	*closed = 0;
	if (j3->closed_descriptor != NULL) {
		j3->closing = 1;
		*closed = 1;

		/* Succeeded: the earlier commit is to be sealed again. */
		return 0;
	}

	/* A transaction with nothing in it has nothing to commit. */
	if (j3->range_count == 0)
		return 0;

	/* Sizes the descriptor, in whole sectors, for the ranges the commit logs. */
	logged = j3_logged_ranges(j3);
	desc_sectors = (J3_DESC_HEADER + logged * J3_DESC_ENTRY + SECTOR_SIZE - 1U) / SECTOR_SIZE;
	bytes = (size_t)desc_sectors * SECTOR_SIZE;

	/* Takes the descriptor. */
	descriptor = kern_malloc(bytes);
	if (descriptor == NULL)
		return ENOMEM;

	/* Starts it empty, so that the sectors past its last range are zero. */
	kern_memset(descriptor, 0, bytes);

	/*
	 * Copies the logged ranges into the slot and lists them.  A copy that
	 * did not complete leaves the volume unwritable, with the transaction
	 * still running and pinned.
	 */
	error = j3_commit_payload(mountp, j3, descriptor, desc_sectors);
	if (error != 0) {
		kern_free(descriptor);
		stop_writing(mountp, "a j3 transaction could not be committed", error);
		return error;
	}

	/*
	 * Moves the transaction aside as the closed one: its ranges, which
	 * stay pinned under its sequence, its freed blocks, which keep content
	 * out of their old owners' blocks until the commit is durable, and its
	 * descriptor.  The arrays change places with the closed side's, which
	 * the last finish left empty.
	 */
	ranges = j3->closed_ranges;
	j3->closed_ranges = j3->ranges;
	j3->ranges = ranges;
	j3->closed_count = j3->range_count;
	freed = j3->closed_freed;
	j3->closed_freed = j3->freed;
	j3->freed = freed;
	j3->closed_freed_count = j3->freed_count;
	j3->closed_sequence = j3->sequence;
	j3->closed_descriptor = descriptor;
	j3->closed_desc_sectors = desc_sectors;

	/* Starts the next transaction empty, under the next sequence and so in the other slot. */
	j3->range_count = 0;
	j3->logged_sectors = 0;
	kern_memset(j3->index, 0, sizeof(unsigned) * 2U * j3->ranges_max);
	j3->freed_count = 0;
	j3->sequence++;

	/*
	 * closing marks a commit between its close and the end of its writes:
	 * the closed side is the finisher's, and the next close waits for it.
	 */
	j3->closing = 1;
	*closed = 1;

	/* Succeeded: the transaction is closed and awaits its seal. */
	return 0;
}

/*
 * Seals the closed transaction and lets its sectors go home (ws073-p051).
 * Runs with neither the mount lock nor the journal lock, so that the
 * device's flushes hold up no lookup or change of the volume; the closed
 * side is this caller's alone while closing is set.
 */
static int
j3_finish_commit(
	struct ufs_mount_state *ms,
	struct mount *mountp)
{
	struct ufs_j3 *j3;
	const struct ufs_j3_range *range;
	uint64_t cursor;
	unsigned kept;
	unsigned n;
	int held_kept;
	int any_kept;
	int home_error;
	int error;

	/*
	 * Writes the descriptor and the commit record into the closed
	 * transaction's slot, each made durable, in that order.
	 */
	j3 = &ms->j3;
	error = j3_commit_seal(mountp, j3, j3->closed_descriptor, j3->closed_desc_sectors, j3->closed_sequence);

	/*
	 * Sends every range home now that replay would write it too, and
	 * before the next transaction closes: the next commit's durable flush
	 * then covers these writes, before the slot after that reuses this
	 * one.  A range whose lines are unpinned is written by the cache; a
	 * range whose line the running transaction pinned again is written
	 * from the slot's copy beneath the cache, since the cache holds the
	 * line until that transaction commits and its payload carries only
	 * its own ranges of the line.  The copies lie in the slot after the
	 * descriptor, in the order of the logged ranges.
	 */
	home_error = 0;
	held_kept = 0;
	any_kept = 0;
	if (error == 0) {
		cursor = j3->header_sectors + (j3->closed_sequence & 1U) * j3->slot_sectors + j3->closed_desc_sectors;
		for (n = 0; n < j3->closed_count; n++) {
			/* Unpins the range's lines, counting the ones a later tag holds. */
			range = &j3->closed_ranges[n];
			kept = 0;
			(void)ufs_buf_unpin(mountp->m_disk, range->lba, range->count, j3->closed_sequence, &kept);

			/* Notes a line a later transaction holds, which this finish cannot write whole. */
			if (kept != 0U)
				any_kept = 1;

			/*
			 * A held range is pinned content with no copy in the slot; the
			 * cache writes it when it can, and a sync waits for the line's
			 * next owner to write it when that is later.
			 */
			if (range->hold) {
				if (kept == 0 && home_error == 0)
					home_error = ufs_buf_writeback_range(mountp->m_disk, range->lba, range->count);
				else if (kept != 0U)
					held_kept = 1;
				continue;
			}

			/* Writes a logged range home through the cache, or from the slot when the cache holds its line. */
			if (home_error == 0 && kept == 0)
				home_error = ufs_buf_writeback_range(mountp->m_disk, range->lba, range->count);
			else if (home_error == 0)
				home_error = j3_home_direct(mountp, j3, range, cursor);

			/* Steps past the range's copy in the slot. */
			cursor += range->count;
		}
	}

	/* Ends the commit under the journal lock. */
	mutex_lock(&j3->lock);

	/*
	 * A commit that did not complete leaves the volume unwritable; the
	 * closed transaction stays pinned, to be sealed again by the next
	 * commit or let go when the journal goes out of service.
	 */
	if (error != 0) {
		stop_writing(mountp, "a j3 commit did not complete", error);
	} else {
		/* Empties the closed side for the next close. */
		j3->closed_count = 0;
		if (j3->closed_freed_count != 0) {
			kern_memset(j3->closed_freed, 0, sizeof(uint64_t) * J3_FREED_MAX);
			j3->closed_freed_count = 0;
		}

		/* Gives the sealed descriptor back. */
		kern_free(j3->closed_descriptor);
		j3->closed_descriptor = NULL;
		j3->closed_desc_sectors = 0;

		/*
		 * Owes a home for a held range this finish left to the line's next
		 * owner; a finish that left no line to a later transaction has
		 * written every owed line whole, since the transaction that pinned
		 * an owed line again has a range in it.
		 */
		if (held_kept)
			j3->home_owed = 1;
		else if (!any_kept)
			j3->home_owed = 0;
	}

	/*
	 * A home that could not be written leaves the volume unwritable too:
	 * the commit is durable and the next mount's replay writes the home,
	 * but no later transaction may be built on the line meanwhile.
	 */
	if (home_error != 0)
		stop_writing(mountp, "a j3 home block could not be written", home_error);

	/* The commit is over, sealed or not; a close waiting for it may go on. */
	j3->closing = 0;
	waitq_wake_all(&j3->commit_idle);
	mutex_unlock(&j3->lock);

	/* Reports why the seal failed. */
	if (error != 0)
		return error;

	/* Reports why a home could not be written. */
	if (home_error != 0)
		return home_error;

	/* Succeeded: the transaction is committed and its homes are on their way. */
	return 0;
}

/* Waits, with the journal lock yielded meanwhile, until no commit is being written. */
static void
j3_commit_wait(
	struct ufs_j3 *j3)
{
	uint64_t sequence;

	/* Sleeps until the finish of the commit under way wakes the waiters. */
	while (j3->closing) {
		sequence = waitq_sequence(&j3->commit_idle);
		(void)mutex_wait(&j3->lock, &j3->commit_idle, sequence, 0, 0);
	}
}

/*
 * Commits in place (ws073-p051): waits out a commit being written, closes
 * the running transaction and seals it before returning, with the journal
 * lock let go for the writes.  For the callers that cannot go on before the
 * commit is durable: a change whose transaction has no room left, and the
 * journal going out of service.  The caller holds the journal lock, and the
 * mount lock when the volume may be changing.
 */
static int
j3_commit_now(
	struct ufs_mount_state *ms,
	struct mount *mountp)
{
	struct ufs_j3 *j3;
	int closed;
	int error;

	/* Waits for a commit another thread is writing. */
	j3 = &ms->j3;
	j3_commit_wait(j3);

	/* Closes the running transaction, or takes up a seal that failed. */
	error = j3_close_transaction(ms, mountp, &closed);
	if (error != 0)
		return error;

	/* Nothing closed means nothing to seal. */
	if (!closed)
		return 0;

	/* Seals it with the journal lock let go, as every finish runs. */
	mutex_unlock(&j3->lock);
	error = j3_finish_commit(ms, mountp);
	mutex_lock(&j3->lock);

	/* Reports why the commit failed. */
	if (error != 0)
		return error;

	/* Succeeded: the transaction is committed. */
	return 0;
}

/*
 * Commits on the flusher's interval, between whole namespace operations
 * and whole mount-lock steps.
 */
static void
j3_hook(
	void *argument)
{
	struct ufs_mount_state *ms;
	struct mount *mountp;
	int closed;

	/* The mount the hook was registered for; none once the journal is going out of service. */
	ms = argument;
	mountp = ms->j3.mountp;
	if (mountp == NULL)
		return;

	/*
	 * Shares the namespace (no change is half done while it is shared, and
	 * lookups go on meanwhile, ws073-p045), then takes the mount lock and
	 * the journal lock, lowest first, so that no operation is half done
	 * when the transaction closes.
	 */
	namespace_share(ms);
	mutex_lock(&ms->lock);
	mutex_lock(&ms->j3.lock);

	/*
	 * Closes the running transaction, unless a commit is still being
	 * written: then this interval's commit waits for the next interval
	 * rather than holding the locks for the flushes (ws073-p051).
	 */
	closed = 0;
	if (!ms->j3.closing)
		(void)j3_close_transaction(ms, mountp, &closed);
	mutex_unlock(&ms->j3.lock);
	mutex_unlock(&ms->lock);
	namespace_unshare(ms);

	/* Seals the closed transaction with the locks let go, so that its flushes hold up no lookup or change. */
	if (closed)
		(void)j3_finish_commit(ms, mountp);
}

/* Reads the superblock sector that holds the locator and the request. */
static int
j3_super_read(
	struct mount *mountp,
	uint8_t sector[SECTOR_SIZE])
{
	uint64_t lba;
	int error;

	/* Both live in the superblock's spare words, in one sector. */
	lba = (UFS_SBLOCK_OFFSET + J3_LOCATOR_OFFSET) / SECTOR_SIZE;
	error = observed_disk_read(mountp->m_disk, lba, 1, sector);
	if (error != 0)
		return error;

	/* Succeeded: the sector is read. */
	return 0;
}

/* Parses the locator; reports whether the volume names a journal header. */
static int
j3_locator_parse(
	const uint8_t sector[SECTOR_SIZE],
	uint64_t *fragment,
	uint64_t *nonce)
{
	const uint8_t *locator;
	uint32_t word;
	uint32_t sum;

	/* Finds the locator in the sector. */
	locator = sector + (UFS_SBLOCK_OFFSET + J3_LOCATOR_OFFSET) % SECTOR_SIZE;

	/* A locator names its kind first; a volume that never had one has zeroes. */
	word = get32(locator);
	if (word != J3_LOCATOR_MAGIC)
		return 0;

	/* Only this version's locator is understood. */
	word = get32(locator + 4);
	if (word != J3_VERSION)
		return 0;

	/* A locator whose checksum fails was not completely written. */
	word = get32(locator + 24);
	sum = checksum(locator, 24);
	if (word != sum)
		return 0;

	/* Takes the header's fragment and the journal's identity. */
	*fragment = get64(locator + 8);
	*nonce = get64(locator + 16);

	/* Succeeded: the volume names a journal. */
	return 1;
}

/* Parses the size mkfs recorded; reports whether there is a record. */
static int
j3_request_parse(
	const uint8_t sector[SECTOR_SIZE],
	uint32_t *mib)
{
	const uint8_t *request;
	uint32_t word;
	uint32_t sum;

	/* Finds the record in the sector. */
	request = sector + (UFS_SBLOCK_OFFSET + J3_REQUEST_OFFSET) % SECTOR_SIZE;

	/* A record names its kind first; an older volume has none. */
	word = get32(request);
	if (word != J3_REQUEST_MAGIC)
		return 0;

	/* Only this version's record is understood. */
	word = get32(request + 4);
	if (word != J3_REQUEST_VERSION)
		return 0;

	/* A record whose checksum fails is not trusted. */
	word = get32(request + 12);
	sum = checksum(request, 12);
	if (word != sum)
		return 0;

	/* Takes the size, no more than the largest journal. */
	*mib = get32(request + 8);
	if (*mib > J3_MAX_MIB)
		*mib = J3_MAX_MIB;

	/* Succeeded: the volume carries a record. */
	return 1;
}

/*
 * Checks a header block against the journal the locator names and the
 * block size of the mount, and reports how many extents it lists.
 */
static int
j3_header_check(
	const uint8_t *header,
	uint64_t nonce,
	uint32_t block_sectors,
	uint32_t block_bytes,
	uint32_t *count)
{
	uint64_t identity;
	uint32_t word;
	uint32_t sum;
	uint32_t extents;

	/* A header names its kind and version first. */
	word = get32(header);
	if (word != J3_HEADER_MAGIC)
		return EINVAL;
	word = get32(header + 4);
	if (word != J3_VERSION)
		return EINVAL;

	/* It belongs to the journal the locator names. */
	identity = get64(header + 8);
	if (identity != nonce)
		return EINVAL;

	/* Its fixed words are as they were written. */
	word = get32(header + 52);
	sum = checksum(header, 52);
	if (word != sum)
		return EINVAL;

	/* Its blocks, and the header itself, are one block of this volume. */
	word = get32(header + 40);
	if (word != block_sectors)
		return EINVAL;
	word = get32(header + 44);
	if (word != block_sectors)
		return EINVAL;

	/* It lists at least one extent, and no more than its block holds. */
	extents = get32(header + 36);
	if (extents == 0)
		return EINVAL;
	if (J3_HEADER_FIXED + (uint64_t)extents * J3_EXTENT_ENTRY > block_bytes)
		return EINVAL;

	/* Its extent list is as it was written. */
	word = get32(header + 48);
	sum = checksum(header + J3_HEADER_FIXED, (size_t)J3_EXTENT_ENTRY * extents);
	if (word != sum)
		return EINVAL;

	/* Succeeded: reports the number of extents. */
	*count = extents;
	return 0;
}

/*
 * Takes the extents a checked header lists, numbering the file's sectors
 * as it goes, and reports how many sectors they cover.
 */
static int
j3_extents_read(
	struct mount *mountp,
	struct ufs_j3 *j3,
	const uint8_t *header,
	uint32_t count,
	uint64_t *total)
{
	struct ufs_j3_extent *extent;
	const uint8_t *entry;
	uint64_t file_sector;
	uint64_t disk_sectors;
	uint32_t n;

	/* Takes the table of extents. */
	j3->extents = kern_malloc(sizeof(struct ufs_j3_extent) * count);
	if (j3->extents == NULL)
		return ENOMEM;

	/* Reads each extent: its first disk sector and its length. */
	disk_sectors = ufs_disk_sectors(mountp->m_disk);
	file_sector = 0;
	for (n = 0; n < count; n++) {
		extent = &j3->extents[n];
		entry = header + J3_HEADER_FIXED + J3_EXTENT_ENTRY * n;
		extent->file_sector = file_sector;
		extent->lba = get64(entry);
		extent->count = get32(entry + 8);

		/* An empty extent, or one past the end of the disk, is not the journal's. */
		if (extent->count == 0)
			return EINVAL;
		if (extent->lba + extent->count > disk_sectors)
			return EINVAL;

		/* The next extent continues the file. */
		file_sector += extent->count;
	}

	/* Succeeded: reports the sectors the extents cover. */
	*total = file_sector;
	return 0;
}

/*
 * Loads the journal's header: its identity, what was applied, and the
 * extents of its file.
 */
static int
j3_load(
	struct mount *mountp,
	struct ufs_mount_state *ms,
	uint64_t fragment,
	uint64_t nonce)
{
	struct ufs_j3 *j3;
	uint8_t *header;
	uint64_t covered;
	uint32_t block_sectors;
	uint32_t count;
	uint32_t slot_sectors;
	int error;

	/* Takes a buffer for the first block of the file, which is the header. */
	j3 = &ms->j3;
	block_sectors = ms->super.bsize / SECTOR_SIZE;
	header = kern_malloc(ms->super.bsize);
	if (header == NULL)
		return ENOMEM;

	/* Reads the header where the locator says it is. */
	error = observed_disk_read(mountp->m_disk, fragment << ms->super.fsbtodb, block_sectors, header);

	/* Checks that it is the named journal's header, laid out for this volume. */
	count = 0;
	if (error == 0)
		error = j3_header_check(header, nonce, block_sectors, ms->super.bsize, &count);

	/* Takes the extents it lists. */
	covered = 0;
	if (error == 0)
		error = j3_extents_read(mountp, j3, header, count, &covered);

	/* Takes the journal's identity, its progress and its size. */
	if (error == 0) {
		j3->extent_count = count;
		j3->header_fragment = fragment;
		j3->nonce = nonce;
		j3->applied = get64(header + 16);
		j3->total_sectors = get64(header + 24);
		j3->block_sectors = block_sectors;
		j3->header_sectors = block_sectors;
		j3->sequence = j3->applied + 1U;
	}

	/* The extents cover exactly the sectors the journal has. */
	if (error == 0 && covered != j3->total_sectors)
		error = EINVAL;

	/* Lays the slots out. */
	if (error == 0)
		error = j3_geometry(j3);

	/* The slots are as the header recorded them. */
	slot_sectors = get32(header + 32);
	if (error == 0 && j3->slot_sectors != slot_sectors)
		error = EINVAL;

	/* Gives the header block back. */
	kern_free(header);

	/* A header that does not hold together keeps no extents. */
	if (error != 0) {
		kern_free(j3->extents);
		j3->extents = NULL;
		j3->extent_count = 0;
		return error;
	}

	/* Succeeded: the journal is loaded. */
	return 0;
}

/*
 * Reads one slot's commit record; reports its sequence when it seals a
 * commit of this journal, else zero.
 */
static uint64_t
j3_slot_sequence(
	struct mount *mountp,
	struct ufs_j3 *j3,
	unsigned slot,
	uint32_t *desc_sectors,
	uint32_t *desc_sum)
{
	uint8_t commit[SECTOR_SIZE];
	uint64_t base;
	uint64_t value;
	uint32_t word;
	uint32_t sum;
	int error;

	/* Reads the record at the end of the slot; an unreadable one seals nothing. */
	base = j3->header_sectors + (uint64_t)slot * j3->slot_sectors;
	error = j3_io(mountp, j3, base + j3->slot_sectors - 1U, 1, commit, 0);
	if (error != 0)
		return 0;

	/* A record names its kind and version first; a cleared one has zeroes. */
	word = get32(commit);
	if (word != J3_COMMIT_MAGIC)
		return 0;
	word = get32(commit + 4);
	if (word != J3_VERSION)
		return 0;

	/* It belongs to this journal, not to a former one in the same blocks. */
	value = get64(commit + 16);
	if (value != j3->nonce)
		return 0;

	/* It was completely written. */
	word = get32(commit + 32);
	sum = checksum(commit, 32);
	if (word != sum)
		return 0;

	/* Its sequence picks this slot. */
	value = get64(commit + 8);
	if ((value & 1U) != slot)
		return 0;

	/* Takes the size and the checksum of the descriptor the record seals. */
	*desc_sectors = get32(commit + 28);
	*desc_sum = get32(commit + 24);

	/* Succeeded: reports the sequence the record seals. */
	return value;
}

/*
 * Checks that a descriptor is the one a commit record sealed, and reports
 * how many ranges it lists.
 */
static int
j3_descriptor_check(
	const struct ufs_j3 *j3,
	const uint8_t *descriptor,
	uint32_t desc_sectors,
	uint64_t sequence,
	uint32_t desc_sum,
	uint32_t *entries)
{
	uint64_t value;
	uint32_t word;
	uint32_t sum;
	uint32_t count;

	/* A descriptor names its kind first. */
	word = get32(descriptor);
	if (word != J3_DESC_MAGIC)
		return EINVAL;

	/* It belongs to the sealed transaction of this journal. */
	value = get64(descriptor + 8);
	if (value != sequence)
		return EINVAL;
	value = get64(descriptor + 16);
	if (value != j3->nonce)
		return EINVAL;

	/* Its fixed words are as written, and the record sealed that checksum. */
	word = get32(descriptor + 40);
	sum = checksum(descriptor, 40);
	if (word != sum)
		return EINVAL;
	if (word != desc_sum)
		return EINVAL;

	/* Its list of ranges fits in its sectors and is as it was written. */
	count = get32(descriptor + 24);
	if (J3_DESC_HEADER + (uint64_t)count * J3_DESC_ENTRY > (uint64_t)desc_sectors * SECTOR_SIZE)
		return EINVAL;
	word = get32(descriptor + 36);
	sum = checksum(descriptor + J3_DESC_HEADER, (size_t)count * J3_DESC_ENTRY);
	if (word != sum)
		return EINVAL;

	/* Succeeded: reports the number of ranges. */
	*entries = count;
	return 0;
}

/* Sums a range's payload in the journal the way the commit summed it. */
static int
j3_payload_sum(
	struct mount *mountp,
	struct ufs_j3 *j3,
	uint64_t cursor,
	uint32_t count,
	uint8_t *payload,
	uint32_t *sum)
{
	uint32_t done;
	uint32_t amount;
	uint32_t chunk_sum;
	int error;

	/* Reads the payload through the buffer in chunks. */
	*sum = 0;
	for (done = 0; done < count; done += amount) {
		/* Takes no more than the buffer holds. */
		amount = count - done;
		if (amount > J3_STAGING_SECTORS)
			amount = J3_STAGING_SECTORS;

		/* Reads the chunk from the slot. */
		error = j3_io(mountp, j3, cursor + done, amount, payload, 0);
		if (error != 0)
			return error;

		/* Folds the chunk into the range's sum. */
		chunk_sum = checksum(payload, (size_t)amount * SECTOR_SIZE);
		*sum = (*sum * 16777619U) ^ chunk_sum;
	}

	/* Succeeded: reports the sum. */
	return 0;
}

/* Copies a range's payload from the journal to its home. */
static int
j3_payload_home(
	struct mount *mountp,
	struct ufs_j3 *j3,
	uint64_t cursor,
	uint64_t lba,
	uint32_t count,
	uint8_t *payload)
{
	uint32_t done;
	uint32_t amount;
	int error;

	/* Copies the payload through the buffer in chunks. */
	for (done = 0; done < count; done += amount) {
		/* Takes no more than the buffer holds. */
		amount = count - done;
		if (amount > J3_STAGING_SECTORS)
			amount = J3_STAGING_SECTORS;

		/* Reads the chunk from the slot. */
		error = j3_io(mountp, j3, cursor + done, amount, payload, 0);
		if (error != 0)
			return error;

		/* Writes it home. */
		error = disk_sector_transfer(mountp->m_disk, lba + done, amount, payload, 1, 0, NULL);
		if (error != 0)
			return error;
	}

	/* Succeeded: the range is home. */
	return 0;
}

/*
 * Applies the ranges a checked descriptor lists: checks every payload
 * against its sum before writing any, then writes each range home and
 * makes the homes durable.
 */
static int
j3_payload_apply(
	struct mount *mountp,
	struct ufs_j3 *j3,
	const uint8_t *descriptor,
	uint32_t entries,
	uint64_t first,
	uint8_t *payload)
{
	const uint8_t *entry;
	uint64_t cursor;
	uint64_t lba;
	uint32_t count;
	uint32_t sum;
	uint32_t stored;
	uint32_t n;
	int error;

	/* Checks each payload, in the order the commit wrote them. */
	cursor = first;
	for (n = 0; n < entries; n++) {
		/* Sums the range's payload. */
		entry = descriptor + J3_DESC_HEADER + J3_DESC_ENTRY * n;
		count = get32(entry + 8);
		error = j3_payload_sum(mountp, j3, cursor, count, payload, &sum);
		if (error != 0)
			return error;

		/* A payload that does not match its sum was not completely written. */
		stored = get32(entry + 12);
		if (sum != stored)
			return EINVAL;

		/* The next payload follows this one. */
		cursor += count;
	}

	/* Writes each range home. */
	cursor = first;
	for (n = 0; n < entries; n++) {
		/* Copies the range's payload to the home it lists. */
		entry = descriptor + J3_DESC_HEADER + J3_DESC_ENTRY * n;
		lba = get64(entry);
		count = get32(entry + 8);
		error = j3_payload_home(mountp, j3, cursor, lba, count, payload);
		if (error != 0)
			return error;

		/* The next payload follows this one. */
		cursor += count;
	}

	/* Makes the homes durable. */
	error = j3_durable(mountp);
	if (error != 0)
		return error;

	/* Succeeded: the transaction is home. */
	return 0;
}

/*
 * Applies the transaction a slot's commit record seals: reads and checks
 * its descriptor, then applies its ranges.
 */
static int
j3_replay_slot(
	struct mount *mountp,
	struct ufs_j3 *j3,
	unsigned slot,
	uint64_t sequence,
	uint32_t desc_sectors,
	uint32_t desc_sum)
{
	uint8_t *descriptor;
	uint8_t *payload;
	uint64_t base;
	uint32_t entries;
	int error;

	/* Takes a buffer for the descriptor. */
	descriptor = kern_malloc((size_t)desc_sectors * SECTOR_SIZE);
	if (descriptor == NULL)
		return ENOMEM;

	/* Takes the buffer the payload passes through. */
	payload = kern_malloc((size_t)J3_STAGING_SECTORS * SECTOR_SIZE);
	if (payload == NULL) {
		kern_free(descriptor);
		return ENOMEM;
	}

	/* Reads the descriptor at the head of the slot. */
	base = j3->header_sectors + (uint64_t)slot * j3->slot_sectors;
	error = j3_io(mountp, j3, base, desc_sectors, descriptor, 0);

	/* Checks that it is the descriptor the record sealed. */
	entries = 0;
	if (error == 0)
		error = j3_descriptor_check(j3, descriptor, desc_sectors, sequence, desc_sum, &entries);

	/* Applies the ranges it lists, whose payloads follow it. */
	if (error == 0)
		error = j3_payload_apply(mountp, j3, descriptor, entries, base + desc_sectors, payload);

	/* Gives the buffers back, applied or not. */
	kern_free(descriptor);
	kern_free(payload);

	/* Reports why the transaction could not be applied. */
	if (error != 0)
		return error;

	/* Succeeded: the transaction is home. */
	return 0;
}

/*
 * Applies the transaction a slot's commit record seals, after checking
 * that the record names a descriptor a commit could have written.
 * Returns 0, EINVAL when the record, its descriptor or its payload do not
 * agree (the commit did not finish), or the I/O's error.
 */
static int
j3_replay_sealed(
	struct mount *mountp,
	struct ufs_j3 *j3,
	unsigned slot,
	uint64_t sequence,
	uint32_t desc_sectors,
	uint32_t desc_sum)
{
	int error;

	/* Refuses a record naming a descriptor no commit could have written. */
	if (desc_sectors == 0)
		return EINVAL;
	if (desc_sectors > j3->desc_sectors_max)
		return EINVAL;

	/* Applies the transaction. */
	error = j3_replay_slot(mountp, j3, slot, sequence, desc_sectors, desc_sum);
	if (error != 0)
		return error;

	/* Succeeded: the transaction is home. */
	return 0;
}

/*
 * Finds the journal the superblock names and applies its newest commit that
 * is not yet applied.  A volume whose journal cannot be found or read goes
 * on as one without a journal; the mount makes a new one.
 */
static int
j3_replay(
	struct mount *mountp,
	struct ufs_mount_state *ms)
{
	struct ufs_j3 *j3;
	uint8_t sector[SECTOR_SIZE];
	uint64_t fragment;
	uint64_t nonce;
	uint64_t sequence[2];
	uint32_t desc_sectors[2];
	uint32_t desc_sum[2];
	unsigned slot;
	unsigned other;
	int named;
	int error;

	/* Reads the locator. */
	j3 = &ms->j3;
	error = j3_super_read(mountp, sector);
	if (error != 0)
		return error;

	/* A volume whose superblock names no journal has nothing to apply. */
	named = j3_locator_parse(sector, &fragment, &nonce);
	if (!named)
		return 0;

	/* Loads the journal; one that does not hold together is made again. */
	error = j3_load(mountp, ms, fragment, nonce);
	if (error == EINVAL) {
		kern_logf("ufs: %s: the batched journal's header does not hold together; it is not replayed and a new journal is made\n", mountp->m_disk->d_name);
		return 0;
	}
	if (error != 0)
		return error;

	/* The volume has a journal. */
	j3->present = 1;

	/* Reads both slots' commit records; a slot that seals nothing reports zero. */
	sequence[0] = j3_slot_sequence(mountp, j3, 0, &desc_sectors[0], &desc_sum[0]);
	sequence[1] = j3_slot_sequence(mountp, j3, 1, &desc_sectors[1], &desc_sum[1]);

	/* Picks the newer of the two. */
	slot = 0;
	if (sequence[1] > sequence[0])
		slot = 1;

	/* Numbers the next transaction after anything either slot holds. */
	if (sequence[slot] >= j3->sequence)
		j3->sequence = sequence[slot] + 1U;

	/* A commit the header records as applied is already home. */
	if (sequence[slot] <= j3->applied)
		return 0;

	/* A read-only disk cannot take the transaction home. */
	if ((mountp->m_disk->d_flags & DISK_READ_ONLY) != 0)
		return EROFS;

	/* Applies the transaction. */
	error = j3_replay_sealed(mountp, j3, slot, sequence[slot], desc_sectors[slot], desc_sum[slot]);

	/*
	 * A record whose descriptor or payload does not match it seals a
	 * commit that did not finish: its record reached the device and the
	 * rest did not.  Such a commit never completed, so nothing waited on
	 * it, and its homes were never written (they are written after the
	 * seal).  It is dropped, and the volume is as the commit before it
	 * left it: that one is applied when the other slot still holds it
	 * unapplied (BUG-312: the mount failed instead).
	 */
	if (error == EINVAL) {
		other = slot ^ 1U;
		kern_logf("ufs: %s: the batched journal's newest commit %llu (slot %u) did not finish (its descriptor or payload does not match its record); it is dropped\n",
			  mountp->m_disk->d_name,
			  (unsigned long long)sequence[slot],
			  slot);
		error = 0;
		slot = other;
		if (sequence[other] <= j3->applied)
			return 0;

		/* The commit before it, which its home writes never followed. */
		kern_logf("ufs: %s: the batched journal's commit %llu (slot %u) before it is applied instead\n",
			  mountp->m_disk->d_name,
			  (unsigned long long)sequence[other],
			  other);
		error = j3_replay_sealed(mountp, j3, other, sequence[other], desc_sectors[other], desc_sum[other]);
	}

	/* A commit that could not be applied fails the mount. */
	if (error != 0) {
		kern_logf("ufs: %s: the batched journal's commit %llu (slot %u) could not be applied (error %d)\n",
			  mountp->m_disk->d_name,
			  (unsigned long long)sequence[slot],
			  slot,
			  error);
		return error;
	}

	/* Records the transaction as applied, durably. */
	j3->applied = sequence[slot];
	error = j3_write_header(mountp, j3);
	if (error != 0)
		return error;
	error = j3_durable(mountp);
	if (error != 0)
		return error;

	/* Succeeded: the volume holds the newest committed transaction. */
	return 0;
}

/*
 * Picks the journal's size in sectors: what mkfs recorded, or else the
 * ext4 default for the volume's size, capped at J3_DEFAULT_MAX_MIB.
 * Either is kept to an eighth of the room the journal may take: the free
 * space plus what the journal already holds.  Zero means no journal.
 */
static uint64_t
j3_wanted_sectors(
	struct mount *mountp,
	struct ufs_mount_state *ms)
{
	uint8_t sector[SECTOR_SIZE];
	uint64_t volume;
	uint64_t room;
	uint64_t bytes;
	uint32_t mib;
	int recorded;
	int error;

	/* Takes the size mkfs recorded, when there is one. */
	mib = 0;
	recorded = 0;
	error = j3_super_read(mountp, sector);
	if (error == 0)
		recorded = j3_request_parse(sector, &mib);

	/* Otherwise follows the ext4 table for the volume's size in MiB. */
	if (!recorded) {
		volume = (ms->super.size * ms->super.fsize) >> 20;
		if (volume < 8U)
			mib = 0;
		else if (volume < 128U)
			mib = 4;
		else if (volume < 1024U)
			mib = 16;
		else if (volume < 2048U)
			mib = 32;
		else if (volume < 16384U)
			mib = 64;
		else
			mib = J3_DEFAULT_MAX_MIB;
	}

	/* Measures the room: the free blocks and the journal's own. */
	bytes = (uint64_t)mib << 20;
	room = ms->super.cstotal_nbfree * ms->super.bsize;
	if (ms->j3.present)
		room += ms->j3.total_sectors * SECTOR_SIZE;

	/* Keeps the journal to an eighth of the room, in whole MiB. */
	if (bytes > room / 8U)
		bytes = (room / 8U) & ~((UINT64_C(1) << 20) - 1U);

	/* Rounds the size down to whole blocks. */
	bytes -= bytes % ms->super.bsize;

	/* Reports the size in sectors. */
	return bytes / SECTOR_SIZE;
}

/* Names the journal file in the superblock's locator. */
static int
j3_locator_write(
	struct mount *mountp,
	struct ufs_j3 *j3)
{
	uint8_t sector[SECTOR_SIZE];
	uint8_t *locator;
	uint64_t lba;
	uint32_t sum;
	int error;

	/* Reads the sector, so that the rest of it is kept. */
	error = j3_super_read(mountp, sector);
	if (error != 0)
		return error;

	/*
	 * Stores the locator: the magic and the version, the header's
	 * fragment and the journal's identity, under a checksum.
	 */
	locator = sector + (UFS_SBLOCK_OFFSET + J3_LOCATOR_OFFSET) % SECTOR_SIZE;
	kern_memset(locator, 0, 28);
	put32(locator, J3_LOCATOR_MAGIC);
	put32(locator + 4, J3_VERSION);
	put64(locator + 8, j3->header_fragment);
	put64(locator + 16, j3->nonce);
	sum = checksum(locator, 24);
	put32(locator + 24, sum);

	/* Writes it, before the journal carries any metadata. */
	lba = (UFS_SBLOCK_OFFSET + J3_LOCATOR_OFFSET) / SECTOR_SIZE;
	error = write_sectors(mountp, lba, 1, sector);
	if (error != 0)
		return error;

	/* Succeeded: the locator is written, though not yet durable. */
	return 0;
}

/*
 * Erases the locator, so that the volume names no journal, and makes the
 * erasure durable.  A volume that names none is left as it is.
 */
static int
j3_locator_erase(
	struct mount *mountp)
{
	uint8_t sector[SECTOR_SIZE];
	uint8_t *locator;
	uint64_t fragment;
	uint64_t nonce;
	uint64_t lba;
	int named;
	int error;

	/* Reads the sector, so that the rest of it is kept. */
	error = j3_super_read(mountp, sector);
	if (error != 0)
		return error;

	/* A volume that names no journal has nothing to erase. */
	named = j3_locator_parse(sector, &fragment, &nonce);
	if (!named)
		return 0;

	/* Clears the locator's words, its magic first among them. */
	locator = sector + (UFS_SBLOCK_OFFSET + J3_LOCATOR_OFFSET) % SECTOR_SIZE;
	kern_memset(locator, 0, 28);

	/* Writes the sector back. */
	lba = (UFS_SBLOCK_OFFSET + J3_LOCATOR_OFFSET) / SECTOR_SIZE;
	error = write_sectors(mountp, lba, 1, sector);
	if (error != 0)
		return error;

	/* Makes the erasure durable before the journal's file is touched. */
	error = j3_durable(mountp);
	if (error != 0)
		return error;

	/* Succeeded: the volume names no journal. */
	return 0;
}

/*
 * Asks whether the journal's file still holds the loaded journal: the file
 * is there, its first block is the header the locator names, and its size
 * is the journal's.
 */
static int
j3_file_matches(
	struct ufs_mount_state *ms,
	struct inode *root,
	int *matches)
{
	struct componentname component;
	struct inode *inode;
	struct ufs_inode_info *ui;
	uint64_t bytes;
	int error;

	/* Answers no until the file is found to hold the journal. */
	*matches = 0;

	/* Names the file, and lets the journal's own lookup see the name. */
	component.cn_nameptr = J3_NAME;
	component.cn_namelen = kern_strlen(J3_NAME);
	component.cn_flags = 0;
	ms->j3.creating = 1;

	/* Finds the file the volume has. */
	inode = NULL;
	error = ufs_lookup(root, &component, &inode);

	/* The name is refused again from here on. */
	ms->j3.creating = 0;

	/* A volume without the file has no journal to keep. */
	if (error == ENOENT)
		return 0;

	/* Reports why the file could not be looked up. */
	if (error != 0)
		return error;

	/* Compares the file's first block and size with the journal's. */
	ui = info(inode);
	bytes = ms->j3.total_sectors * SECTOR_SIZE;
	if (ui->direct[0] == ms->j3.header_fragment &&
	    (uint64_t)inode->i_size == bytes)
		*matches = 1;

	/* Lets the file go. */
	inode_release(inode);

	/* Succeeded: *matches says whether the journal can be kept. */
	return 0;
}

/*
 * Delays a mount's writes in the cache; reports whether they already were.
 */
static unsigned
mountp_disk_cached(
	struct mount *mountp)
{
	unsigned cached;

	/* Remembers whether the writes were delayed already. */
	cached = 0;
	if ((mountp->m_disk->d_flags & DISK_WRITE_CACHED) != 0)
		cached = 1;

	/* Delays them from here on. */
	mountp->m_disk->d_flags |= DISK_WRITE_CACHED;

	/* Reports what the flag was. */
	return cached;
}

/*
 * Finds the journal file in the root directory, or creates it.  The name
 * is refused to everyone else, so the lookup and the creation are done as
 * the journal's own.
 */
static int
j3_file_open(
	struct ufs_mount_state *ms,
	struct inode *root,
	struct inode **inode)
{
	struct componentname component;
	struct inode_creation_request request;
	int error;

	/* Names the file, and lets the journal's own lookups see the name. */
	component.cn_nameptr = J3_NAME;
	component.cn_namelen = kern_strlen(J3_NAME);
	component.cn_flags = 0;
	ms->j3.creating = 1;

	/* Finds the file the volume already has. */
	error = ufs_lookup(root, &component, inode);

	/* Creates it, closed to everyone but the system, when there is none. */
	if (error == ENOENT) {
		error = inode_creation_request_system(INODE_REG, 0600, 0, 0, 0, &request);
		if (error == 0)
			error = ufs_create(root, &component, &request, inode);
	}

	/* The name is refused again from here on. */
	ms->j3.creating = 0;

	/* Reports why the file could not be found or created. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the file. */
	return 0;
}

/*
 * Gives the journal file its blocks, one after another, and merges blocks
 * that follow each other on the disk into extents.
 */
static int
j3_file_fill(
	struct ufs_mount_state *ms,
	struct inode *inode,
	uint64_t blocks,
	struct ufs_j3_extent *extents,
	unsigned capacity,
	unsigned *count)
{
	struct ufs_j3_extent *last;
	uint64_t fragment;
	uint64_t lba;
	uint64_t n;
	unsigned used;
	int error;

	/*
	 * Allocates the blocks as the journal's own, which the allocator does
	 * not zero: nothing reads the journal through the file.
	 */
	ms->j3.creating = 1;
	error = 0;
	used = 0;
	for (n = 0; n < blocks; n++) {
		/* Allocates the file's next block. */
		mutex_lock(&inode->i_lock);

		error = bmap_ensure(inode, n, &fragment);

		mutex_unlock(&inode->i_lock);

		/* Stops at a block that could not be allocated. */
		if (error != 0)
			break;

		/* The first block is the header, which the locator will name. */
		lba = fragment << ms->super.fsbtodb;
		if (n == 0)
			ms->j3.header_fragment = fragment;

		/* Extends the last extent when the block follows it, or starts one. */
		last = NULL;
		if (used != 0)
			last = &extents[used - 1U];
		if (last != NULL && last->lba + last->count == lba) {
			last->count += ms->j3.block_sectors;
		} else if (used < capacity) {
			extents[used].file_sector = n * ms->j3.block_sectors;
			extents[used].lba = lba;
			extents[used].count = ms->j3.block_sectors;
			used++;
		} else {
			/* The header block lists no more extents. */
			error = ENOSPC;
			break;
		}
	}

	/* The journal's own allocations are over. */
	ms->j3.creating = 0;

	/* Reports how many extents the blocks took. */
	*count = used;

	/* Reports why the file could not be given all its blocks. */
	if (error != 0)
		return error;

	/* Succeeded: the file has its blocks. */
	return 0;
}

/*
 * Gives the journal file the wanted size and records its extents: finds or
 * creates `.ufs-journal`, cuts it to nothing, allocates its blocks again,
 * and merges adjacent blocks into extents.
 */
static int
j3_allocate(
	struct ufs_mount_state *ms,
	struct inode *root,
	uint64_t sectors)
{
	struct ufs_j3 *j3;
	struct inode *inode;
	struct ufs_j3_extent *extents;
	uint64_t blocks;
	unsigned capacity;
	unsigned count;
	unsigned cached;
	int error;

	/* Bounds the extents to what one header block can list. */
	j3 = &ms->j3;
	j3->block_sectors = ms->super.bsize / SECTOR_SIZE;
	j3->header_sectors = j3->block_sectors;
	capacity = (ms->super.bsize - J3_HEADER_FIXED) / J3_EXTENT_ENTRY;

	/* Takes the table the new extents are gathered in. */
	extents = kern_malloc(sizeof(struct ufs_j3_extent) * capacity);
	if (extents == NULL)
		return ENOMEM;

	/* Finds the file, or creates it. */
	inode = NULL;
	error = j3_file_open(ms, root, &inode);

	/*
	 * Delays the allocation's metadata writes and makes them durable once
	 * at the end: a journal file is allocated block by block, and each
	 * block written through would take a round trip to the device.  A
	 * crash before the locator names the file leaves it to be cut and
	 * allocated again at the next mount.
	 */
	cached = mountp_disk_cached(root->i_mount);

	/* Gives the old blocks back. */
	if (error == 0)
		error = ufs_truncate(inode, 0);

	/* Takes the wanted number of blocks again. */
	blocks = sectors / j3->block_sectors;
	count = 0;
	if (error == 0)
		error = j3_file_fill(ms, inode, blocks, extents, capacity, &count);

	/* Records the file's new size in its inode. */
	if (error == 0) {
		inode->i_size = (off_t)(blocks * ms->super.bsize);
		error = persist_inode(inode);
	}

	/* Lets the file go; the journal reaches its blocks through the extents. */
	if (inode != NULL)
		inode_release(inode);

	/* Stops delaying the writes when they were not delayed before. */
	if (!cached)
		root->i_mount->m_disk->d_flags &= ~DISK_WRITE_CACHED;

	/* Makes the allocation durable in one pass. */
	if (error == 0)
		error = j3_durable(root->i_mount);

	/* Gives up the new extents when the file could not be given its size. */
	if (error != 0) {
		kern_free(extents);
		return error;
	}

	/* Publishes the extents in place of the old ones. */
	kern_free(j3->extents);
	j3->extents = extents;
	j3->extent_count = count;
	j3->total_sectors = blocks * j3->block_sectors;

	/* Succeeded: the file has the wanted size. */
	return 0;
}

/*
 * Makes a fresh journal of the wanted size: a new file layout, a new
 * identity, empty slots, and a locator naming it.
 */
static int
j3_make(
	struct mount *mountp,
	struct ufs_mount_state *ms,
	struct inode *root,
	uint64_t sectors)
{
	struct ufs_j3 *j3;
	uint8_t sector[SECTOR_SIZE];
	uint64_t ticks;
	uint64_t record;
	unsigned slot;
	int error;

	/*
	 * Unnames the old journal, durably, before its file is cut: a make
	 * that fails after the cut must not leave a locator naming blocks
	 * that are free again (BUG-294).
	 */
	j3 = &ms->j3;
	j3->present = 0;
	error = j3_locator_erase(mountp);
	if (error != 0)
		return error;

	/* Allocates the file; the volume has no journal until the locator names it. */
	error = j3_allocate(ms, root, sectors);
	if (error != 0)
		return error;

	/* Lays the slots out. */
	error = j3_geometry(j3);
	if (error != 0)
		return error;

	/*
	 * Gives the journal a new identity, so that records a former journal
	 * left in the same blocks are not taken for this one's.
	 */
	ticks = sched_ticks();
	j3->nonce = ((ticks + 1U) * UINT64_C(0x9e3779b97f4a7c15)) ^
	    ms->super.size ^
	    ((uint64_t)j3->header_fragment << 20) ^
	    j3->nonce;

	/* Writes a header that has applied nothing. */
	j3->applied = 0;
	j3->sequence = 1;
	error = j3_write_header(mountp, j3);
	if (error != 0)
		return error;

	/* Clears both slots' commit records, so that neither seals anything. */
	kern_memset(sector, 0, sizeof(sector));
	for (slot = 0; slot < 2U; slot++) {
		record = j3->header_sectors + (uint64_t)(slot + 1U) * j3->slot_sectors - 1U;
		error = j3_io(mountp, j3, record, 1, sector, 1);
		if (error != 0)
			return error;
	}

	/* Makes the empty journal durable before anything names it. */
	error = j3_durable(mountp);
	if (error != 0)
		return error;

	/* Names the journal in the superblock, last, and makes the name durable. */
	error = j3_locator_write(mountp, j3);
	if (error != 0)
		return error;
	error = j3_durable(mountp);
	if (error != 0)
		return error;

	/* Succeeded: the volume has the new journal. */
	j3->present = 1;
	return 0;
}

/*
 * Puts the journal in service for a writable mount: reuses the journal the
 * volume has when it is the wanted size, and otherwise makes it again.
 */
static int
j3_open(
	struct mount *mountp,
	struct ufs_mount_state *ms,
	struct inode *root)
{
	struct ufs_j3 *j3;
	uint64_t wanted;
	int matches;
	int error;

	/* A volume too small for more than a few blocks of journal keeps none. */
	j3 = &ms->j3;
	wanted = j3_wanted_sectors(mountp, ms);
	if (wanted <= (uint64_t)ms->super.bsize / SECTOR_SIZE * 8U)
		return ENOENT;

	/*
	 * Keeps the journal the volume names only while its file still holds
	 * it: a make that failed after cutting the file, or an fsck that
	 * cleared the file, leaves a locator naming blocks that are free, and
	 * a journal reused there would write over other files (BUG-294).
	 */
	matches = 0;
	if (j3->present && j3->total_sectors == wanted) {
		error = j3_file_matches(ms, root, &matches);
		if (error != 0)
			return error;
	}

	/* Makes the journal again when there is none, it has another size, or its file no longer holds it. */
	if (!matches) {
		error = j3_make(mountp, ms, root, wanted);
		if (error != 0)
			return error;
	}

	/* Takes the table of the running transaction's ranges. */
	j3->ranges = kern_malloc(sizeof(struct ufs_j3_range) * j3->ranges_max);
	if (j3->ranges == NULL) {
		(void)j3_close(mountp, ms, 0);
		return ENOMEM;
	}

	/* Takes the hash index over the ranges. */
	j3->index = kern_malloc(sizeof(unsigned) * 2U * j3->ranges_max);
	if (j3->index == NULL) {
		(void)j3_close(mountp, ms, 0);
		return ENOMEM;
	}

	/* Takes the set of blocks the transaction frees. */
	j3->freed = kern_malloc(sizeof(uint64_t) * J3_FREED_MAX);
	if (j3->freed == NULL) {
		(void)j3_close(mountp, ms, 0);
		return ENOMEM;
	}

	/* Takes the buffer a commit copies the metadata through. */
	j3->staging = kern_malloc((size_t)J3_STAGING_SECTORS * SECTOR_SIZE);
	if (j3->staging == NULL) {
		(void)j3_close(mountp, ms, 0);
		return ENOMEM;
	}

	/* Takes the table the closed transaction's ranges change places into (ws073-p051). */
	j3->closed_ranges = kern_malloc(sizeof(struct ufs_j3_range) * j3->ranges_max);
	if (j3->closed_ranges == NULL) {
		(void)j3_close(mountp, ms, 0);
		return ENOMEM;
	}

	/* Takes the set the closed transaction's freed blocks change places into. */
	j3->closed_freed = kern_malloc(sizeof(uint64_t) * J3_FREED_MAX);
	if (j3->closed_freed == NULL) {
		(void)j3_close(mountp, ms, 0);
		return ENOMEM;
	}

	/* Takes the buffer the finisher copies a range home through. */
	j3->closed_staging = kern_malloc((size_t)J3_STAGING_SECTORS * SECTOR_SIZE);
	if (j3->closed_staging == NULL) {
		(void)j3_close(mountp, ms, 0);
		return ENOMEM;
	}

	/* Starts an empty transaction, with no commit closed or being written. */
	kern_memset(j3->index, 0, sizeof(unsigned) * 2U * j3->ranges_max);
	kern_memset(j3->freed, 0, sizeof(uint64_t) * J3_FREED_MAX);
	kern_memset(j3->closed_freed, 0, sizeof(uint64_t) * J3_FREED_MAX);
	j3->range_count = 0;
	j3->logged_sectors = 0;
	j3->freed_count = 0;
	j3->closed_count = 0;
	j3->closed_freed_count = 0;
	j3->closed_descriptor = NULL;
	j3->closed_desc_sectors = 0;
	j3->closing = 0;
	j3->home_owed = 0;

	/*
	 * Carries the metadata writes from here on, and commits them on the
	 * flusher's interval for this mount.
	 */
	j3->active = 1;
	j3->mountp = mountp;
	error = buf_flusher_hook(j3_hook, ms, 1);
	if (error != 0) {
		(void)j3_close(mountp, ms, 1);
		return error;
	}

	/* Succeeded: the journal is in service. */
	return 0;
}

/*
 * Takes the journal out of service: commits what runs when asked, writes
 * everything home and records it all as applied.
 */
static int
j3_close(
	struct mount *mountp,
	struct ufs_mount_state *ms,
	int commit)
{
	struct ufs_j3 *j3;
	int pending;
	int error;

	/* Stops the interval commits before taking the journal down. */
	j3 = &ms->j3;
	(void)buf_flusher_hook(j3_hook, ms, 0);
	j3->mountp = NULL;

	/*
	 * Ends the running transaction -- committed when asked, else its
	 * pinned sectors let go unlogged -- and carries no more writes.  A
	 * commit a sync is still writing is waited out first (ws073-p051).
	 */
	mutex_lock(&j3->lock);

	/* Waits out a commit a sync is still writing. */
	j3_commit_wait(j3);

	/* Commits what is pending when asked, else lets the pinned sectors go unlogged. */
	error = 0;
	if (commit &&
	    j3->active &&
	    j3->ranges != NULL) {
		/* Seals a commit whose sealing failed, then commits what runs. */
		pending = j3_commit_pending(j3);
		while (pending && error == 0) {
			error = j3_commit_now(ms, mountp);
			pending = j3_commit_pending(j3);
		}
	} else if (!commit && j3->ranges != NULL) {
		j3_unpin_ranges(mountp, j3->ranges, j3->range_count, j3->sequence);
		j3_unpin_ranges(mountp, j3->closed_ranges, j3->closed_count, j3->closed_sequence);
	}

	/* Carries no more writes. */
	j3->active = 0;

	mutex_unlock(&j3->lock);

	/* Writes the committed homes, when the journal was asked to commit. */
	if (commit && error == 0)
		error = j3_durable(mountp);

	/* Records every commit as applied, so that the next mount replays none. */
	if (commit &&
	    error == 0 &&
	    j3->extents != NULL) {
		j3->applied = j3->sequence - 1U;
		error = j3_write_header(mountp, j3);
		if (error == 0)
			error = j3_durable(mountp);
	}

	/* Gives back the memory of the running and the closed transaction. */
	kern_free(j3->ranges);
	kern_free(j3->index);
	kern_free(j3->freed);
	kern_free(j3->staging);
	kern_free(j3->closed_ranges);
	kern_free(j3->closed_freed);
	kern_free(j3->closed_descriptor);
	kern_free(j3->closed_staging);
	j3->ranges = NULL;
	j3->index = NULL;
	j3->freed = NULL;
	j3->staging = NULL;
	j3->closed_ranges = NULL;
	j3->closed_freed = NULL;
	j3->closed_descriptor = NULL;
	j3->closed_staging = NULL;
	j3->range_count = 0;
	j3->closed_count = 0;
	j3->closed_freed_count = 0;
	j3->closed_desc_sectors = 0;

	/* Reports why the last commit failed. */
	if (error != 0)
		return error;

	/* Succeeded: the journal is out of service with everything home. */
	return 0;
}

/*
 * Asks whether a name is the journal file in the root directory.  The name
 * is the volume's own: nothing through the VFS may create, open, remove or
 * rename onto it, and a directory listing skips it.
 */
static int
j3_hidden(
	const struct inode *directory,
	const struct componentname *component)
{
	struct ufs_mount_state *ms;
	size_t length;
	int differs;

	/* Only the root directory holds the journal. */
	if (directory->i_ino != UFS_ROOT_INO)
		return 0;

	/* A mount without state has no journal. */
	ms = state(directory->i_mount);
	if (ms == NULL)
		return 0;

	/* The journal's own lookups see the name. */
	if (ms->j3.creating)
		return 0;

	/* Compares the name with the journal's, length first. */
	length = kern_strlen(J3_NAME);
	if (component->cn_namelen != length)
		return 0;
	differs = kern_memcmp(component->cn_nameptr, J3_NAME, length);
	if (differs != 0)
		return 0;

	/* Succeeded: the name is the journal's. */
	return 1;
}
