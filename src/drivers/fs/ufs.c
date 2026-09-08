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
#include <hal/hal.h>
#include <kern/inode.h>
#include <kern/quota.h>

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/statvfs.h>
#include <zedbsd/blkid.h>
#include <zedbsd/quota.h>
#include <zedbsd/snapshot.h>

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

#define UFS_ALLOCATION_BLOCKS		16U
#define UFS_ALLOCATION_BYTES		65536U

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

/*
 * Forward declaration.
 */
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
static int write_sectors_impl(struct mount *mountp, uint64_t lba, uint32_t count, const void *buffer, const struct io_context *context);
static int write_sectors_context(struct mount *mountp, uint64_t lba, uint32_t count, const void *buffer, const struct io_context *context);
static int write_sectors(struct mount *mountp, uint64_t lba, uint32_t count, const void *buffer);
static int observed_disk_read(struct disk *disk, uint64_t block, uint32_t count, void *buffer);
static int ufs_sync(struct mount *mountp);
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
static struct ufs_inode_info *info(const struct inode *inode);
static int journal_read_image(struct ufs_mount_state *ms, uint64_t first, uint32_t count, void *buffer);
static int read_metadata_sectors(struct mount *mountp, uint64_t first, uint32_t count, void *buffer);
static int read_block(struct mount *mountp, uint64_t fragment, void *buffer);
static int write_block(struct mount *mountp, uint64_t fragment, const void *buffer);
static size_t content_run_bytes(struct inode *inode, uint64_t logical, uint64_t first, size_t remaining, int writing, int *mapping_error);
static int read_content_block(struct mount *mountp, uint64_t fragment, void *buffer);
static int write_content_block(struct mount *mountp, uint64_t fragment, const void *buffer);
static int write_content_context(struct mount *mountp, uint64_t fragment, const void *buffer, const struct io_context *context);
static int bit_test(const uint8_t *map, uint32_t bit);
static void bit_set(uint8_t *map, uint32_t bit);
static void bit_clear(uint8_t *map, uint32_t bit);
static uint64_t cgstart(const struct ufs_super *super, uint32_t cg);
static uint32_t cg_ndblk(const struct ufs_super *super, uint32_t cg);
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
static int allocate_block_compat(struct mount *mountp, uid_t uid, gid_t gid, uint64_t *result);
static void allocation_begin(struct ufs_allocation *context, struct mount *mountp, uid_t uid, gid_t gid);
static int allocation_allocate(struct ufs_allocation *context, uint64_t *result);
static void allocation_commit(struct ufs_allocation *context);
static void allocation_abort(struct ufs_allocation *context);
static int allocate_block(struct mount *mountp, uid_t uid, gid_t gid, uint64_t *result);
static int free_block(struct mount *mountp, uint64_t fragment, uid_t uid, gid_t gid);
static int allocate_inode_number(struct mount *mountp, uid_t uid, gid_t gid, uint32_t *number);
static int free_inode_number(struct mount *mountp, uint32_t number, uid_t uid, gid_t gid);
static int indirect_entry(struct mount *mountp, uint64_t fragment, uint32_t index, uint64_t *result);
static int bmap(struct inode *inode, uint64_t logical, uint64_t *result);
static int bmap_ensure(struct inode *inode, uint64_t logical, uint64_t *result);
static ssize_t pread_inode(struct inode *inode, void *buffer, size_t length, off_t offset);
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
static int next_dirent(struct inode *directory, off_t *cursor, uint32_t *number, uint8_t *type, char name[NAME_MAX + 1U]);
static uint16_t dir_minimum(uint8_t length);
static uint8_t dir_type(enum inode_type type);
static int restore_directory_block(struct inode *directory, uint64_t fragment, const uint8_t *original, int original_error);
static int dir_find_record(struct inode *directory, const struct componentname *name, uint8_t *block, uint32_t *offset, uint32_t *previous, uint32_t *number);
static int dir_add(struct inode *directory, const struct componentname *name, uint32_t number, uint8_t type);
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
static int remove_group_locked(struct inode *directory, const struct componentname *name, struct inode *target, struct ufs_remove_group *group);
static int remove_group(struct inode *directory, const struct componentname *name, struct inode *target, int *handled);
static int directory_image_insert(struct inode *directory, const struct componentname *name, struct inode *target, uint8_t *block);
static int link_group_locked(struct inode *directory, const struct componentname *name, struct inode *target, struct ufs_link_group *group);
static int link_group(struct inode *directory, const struct componentname *name, struct inode *target, int *handled);
static int directory_image_change(struct inode *directory, uint8_t *block, const struct componentname *name, uint32_t expected, uint32_t replacement, uint8_t type);
static int rename_group_locked(struct inode *old_directory, const struct componentname *old_name, struct inode *new_directory, const struct componentname *new_name, struct inode *source, struct inode *target, struct ufs_rename_group *group);
static int rename_group(struct inode *old_directory, const struct componentname *old_name, struct inode *new_directory, const struct componentname *new_name, struct inode *source, struct inode *target, int *handled);
static int creation_group_locked(struct inode *directory, const struct componentname *name, struct inode *target, struct ufs_link_group *group);
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
static struct inode *ufs_alloc_inode(struct mount *mountp);
static void ufs_free_inode(struct inode *inode);
static int ufs_read_super(struct disk *disk, struct ufs_super *super);
static char ufs_identity_hex(unsigned value);
static void ufs_identity_hex32(char output[8], uint32_t value);
static void ufs_identity_label(char *output, size_t capacity, const uint8_t *input, size_t length);
static int ufs_write_clean(struct mount *mountp, uint8_t clean);
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
static int ufs_statvfs(struct mount *mountp, struct statvfs *result);
static int ufs_quotactl(struct mount *mountp, struct quota_control *request);
static int ufs_snapshotctl(struct mount *mountp, struct snapshot_control *request);
static int ufs_prepare_unmount(struct mount *mountp);
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
	memset(journal, 0, sizeof(*journal));
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
	memset(descriptor, 0, sizeof(descriptor));

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

	memset(commit, 0, sizeof(commit));

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
			memcpy((uint8_t *)buffer + (size_t)done * SECTOR_SIZE,
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
 * Implements the drv ufs put64 operation.
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
	memset(view, 0, sizeof(*view));
	(void)__atomic_fetch_sub(&journal->image_readers, 1U, __ATOMIC_RELEASE);
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
	memset(snapshot, 0, sizeof(*snapshot));
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
 * Implements the drv ufs snapshot open operation.
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
	signature = memcmp(control, "ZSN1", 4);

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
 * Implements the drv ufs snapshot create operation.
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
		memset(header, 0, sizeof(header));

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
 * Implements the drv ufs snapshot delete operation.
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

	memset(super, 0, sizeof(*super));

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
	int error;

	/* Rejects a call that names no disk or nowhere to report. */
	if (disk == NULL || identity == NULL)
		return EINVAL;

	/* This driver reads 512-byte sectors and nothing else. */
	if (disk->d_block_size != UFS_SECTOR_SIZE)
		return EOPNOTSUPP;
	first_block = UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;

	/* The superblock sits at a fixed offset, in whole sectors. */
	block_count = UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;
	if (disk->d_block_count < first_block + block_count)
		return EOPNOTSUPP;

	/* Takes the staging the superblock is read into. */
	buffer = kern_malloc(UFS_SBLOCK_SIZE);
	if (buffer == NULL)
		return ENOMEM;

	/* Reads the superblock, which is where every field below lives. */
	error = disk_read_direct(disk, first_block, (uint32_t)block_count,
				 buffer);
	if (error == 0) {
		error = drv_ufs_super_decode(buffer, UFS_SBLOCK_SIZE,
					     disk->d_block_count, &super);
	}
	if (error != 0) {
		kern_free(buffer);

		/* Failed. */
		return error;
	}

	strcpy(identity->type, "ufs");
	identity->flags |= ZEDBSD_BLKID_TYPE;
	first = drv_ufs_get32(buffer, UFS_FS_ID, super.swapped);

	/* The two halves of the volume identifier. */
	second = drv_ufs_get32(buffer, UFS_FS_ID + 4U, super.swapped);
	if (first != 0U || second != 0U) {
		ufs_identity_hex32(identity->uuid, first);
		ufs_identity_hex32(identity->uuid + 8U, second);
		identity->uuid[16] = '\0';
		identity->flags |= ZEDBSD_BLKID_UUID;
	}

	ufs_identity_label(identity->label, sizeof(identity->label),
			   buffer + UFS_FS_VOLNAME, UFS_FS_VOLNAME_SIZE);

	/* Reports the volume label when the superblock carries one. */
	if (identity->label[0] != '\0')
		identity->flags |= ZEDBSD_BLKID_LABEL;
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
		ms->writable = 0;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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
		bytes = (uint64_t)count * disk->d_block_size;

	io_stats_record(IO_UFS_READ, bytes);

	/* Reads through the disk, so the fault injection sees it. */
	error = disk_read(disk, block, count, buffer);

	/* Reports how the read went. */
	return error;
}

/* Supports the journal read operation. */
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

/* Supports the journal write operation. */
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
	io_stats_record(IO_UFS_WRITE,
			(uint64_t)count * owner->disk->d_block_size);

	/* Writes the sectors through that context. */
	bytes_written =
		disk_write_context(owner->disk, lba, count, buffer, &child);

	/* Reports how many bytes reached the disk. */
	return bytes_written;
}

/* Supports the journal flush operation. */
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

/* Supports the locator get32 operation. */
static uint32_t
locator_get32(
	const uint8_t *p)
{
	/* A locator is stored little-endian, whatever the volume is. */
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
		(uint32_t)p[3] << 24;
}

/* Supports the locator get64 operation. */
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

/* Supports the locator digest operation. */
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
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, UFS_JOURNAL_IMAGE_BYTES, ZEDBSD_PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0};
	struct hal_pmem memory;
	int released;
	int error;

	/*
	 * Obtains backing without holding a metadata or journal mutation lock.
	 */
	memset(&memory, 0, sizeof(memory));

	/* Takes the physical memory the journal image lives in. */
	error = hal_pmem_alloc(&request, &memory);
	if (error != HAL_OK || memory.vaddr == NULL ||
	    memory.size < UFS_JOURNAL_IMAGE_BYTES) {
		/*
		 * Gives the memory back when it is not the size that was asked
		 * for.
		 */
		if (memory.size != 0) {
			released = hal_pmem_free(&memory);
			if (released != HAL_OK)
				HAL_FATAL("ufs journal rollback failed");
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
		released = hal_pmem_free(&memory);
		if (released != HAL_OK)
			HAL_FATAL("ufs journal reservation rollback failed");

		/* Failed. */
		return error;
	}

	cache_memory_commit(CACHE_MEMORY_BUF_META, memory.size);
	ms->journal_memory = memory;

	/* Publishes the image to the journal. */
	error = drv_ufs_journal_bind_image(&ms->journal, memory.vaddr,
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
	released = hal_pmem_free(&ms->journal_memory);
	if (released != HAL_OK)
		HAL_FATAL("ufs journal backing release failed");
	cache_memory_release(CACHE_MEMORY_BUF_META, bytes);
	memset(&ms->journal_memory, 0, sizeof(ms->journal_memory));
	ms->journal.image = NULL;
	ms->journal.image_valid = 0;
}

/* Supports the journal discover operation. */
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
	int old_signature;
	int signature;
	int error;

	/* A volume too small to hold the locator carries no journal. */
	if (end >= mountp->m_disk->d_block_count)
		return 0;

	/* Reads the last sector, where the locator lives. */
	error = observed_disk_read(mountp->m_disk, end, 1, locator);
	if (error != 0)
		return error;

	/* Compares the sector against the old and the current signature. */
	old_signature = memcmp(locator, "ZUJ", 3);
	signature = memcmp(locator, "ZUJ2", 4);

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
	if ((uint64_t)sectors + 1U > mountp->m_disk->d_block_count - end) {
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

/* Supports the snapshot discover operation. */
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
	int signature;
	int error;

	/* A volume too small to hold the locator carries no snapshot. */
	if (end >= mountp->m_disk->d_block_count)
		return 0;

	/* Reads the last sector, where the locator lives. */
	error = observed_disk_read(mountp->m_disk, end, 1, locator);
	if (error != 0)
		return error;

	/* Compares the sector against the journal signature. */
	signature = memcmp(locator, "ZUJ2", 4);

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
		if (sectors > mountp->m_disk->d_block_count - end - 1U) {
			/* Failed. */
			return EINVAL;
		}

		/* Steps back over the journal to reach the snapshot locator. */
		cursor = end + 1U + sectors;
	}

	/* A locator pointing past the volume names nothing. */
	if (cursor >= mountp->m_disk->d_block_count)
		return 0;

	/* Reads the sector the locator points at. */
	error = observed_disk_read(mountp->m_disk, cursor, 1, locator);
	if (error != 0)
		return error;

	/* Compares the sector against the snapshot locator signature. */
	signature = memcmp(locator, "ZSL1", 4);

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
	if ((uint64_t)sectors + 1U > mountp->m_disk->d_block_count - cursor) {
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

/* Supports the write sectors impl operation. */
static int
write_sectors_impl(
	struct mount *mountp,
	uint64_t lba,
	uint32_t count,
	const void *buffer,
	const struct io_context *context)
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
			ms->writable = 0;
		mutex_unlock(&ms->journal_lock);

		/* Releases the snapshot hold the preservation took. */
		if (snapshot_locked)
			mutex_unlock(&ms->snapshot_lock);

		/* Failed: reports why the checkpoint could not run. */
		return error;
	}

	io_stats_record(IO_UFS_WRITE, (uint64_t)count * mountp->m_disk->d_block_size);

	error = disk_write_context(mountp->m_disk, lba, count, buffer, context);

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
	error = write_sectors_impl(mountp, lba, count, buffer, &child);
	io_epoch_end(&mountp->m_write_epoch);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the write sectors operation. */
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

/* Supports the state operation. */
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

/* Supports the info operation. */
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

/* Supports the read block operation. */
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
		memset(buffer, 0, s->bsize);

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

/* Supports the write block operation. */
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

/* Supports the write content block operation. */
static int
write_content_block(
	struct mount *mountp,
	uint64_t fragment,
	const void *buffer)
{
	int error;
	const struct ufs_super *s = &state(mountp)->super;

	/* A fragment outside the volume is not written to. */
	if (fragment != 0 && fragment < s->size &&
	    s->frag <= s->size - fragment)
		io_stats_record(IO_UFS_CONTENT_WRITE, s->bsize);

	/* Writes the block the fragment names. */
	error = write_block(mountp, fragment, buffer);

	/* Reports how the write went. */
	return error;
}

/* Supports the write content context operation. */
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
	error = write_sectors_context(
		mountp, fragment << super->fsbtodb,
		super->bsize / UFS_SECTOR_SIZE, buffer, context);

	/* Reports how the write went. */
	return error;
}

/* Supports the bit test operation. */
static int
bit_test(
	const uint8_t *map,
	uint32_t bit)
{
	/* The map is a bit per object, eight to a byte. */
	return (map[bit >> 3] & (uint8_t)(1U << (bit & 7U))) != 0;
}

/* Supports the bit set operation. */
static void
bit_set(
	uint8_t *map,
	uint32_t bit)
{
	map[bit >> 3] |= (uint8_t)(1U << (bit & 7U));
}

/* Supports the bit clear operation. */
static void
bit_clear(
	uint8_t *map,
	uint32_t bit)
{
	map[bit >> 3] &= (uint8_t)~(1U << (bit & 7U));
}

/* Supports the cgstart operation. */
static uint64_t
cgstart(
	const struct ufs_super *super,
	uint32_t cg)
{
	/* A cylinder group starts one group further in than the last. */
	return (uint64_t)cg * super->fpg + (uint64_t)super->cgoffset * (cg & ~super->cgmask);
}

/* Supports the cg ndblk operation. */
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
			error = disk_read_view(mountp->m_disk,
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

/* Supports the load cg locked operation. */
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

/* Supports the valid inode fragment operation. */
static int
valid_inode_fragment(
	const struct ufs_super *super,
	uint64_t fragment)
{
	uint64_t start;
	uint32_t ndblk;
	uint32_t cg;

	/* A fragment of zero names no block. */
	if (fragment == 0)
		return 1;
	/* Finds the cylinder group the fragment would live in. */
	for (cg = 0; cg < super->ncg; cg++) {
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

/* Supports the prepare super summaries operation. */
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
	struct ufs_mount_state *ms;
	int rollback;

	ms = state(mountp);
	rollback = write_cg(mountp);

	/* A failed rollback leaves the volume unwritable. */
	if (rollback != 0) {
		ms->writable = 0;

		/* Failed. */
		return rollback;
	}

	/* Reports the failure that made the rollback necessary. */
	return original_error;
}

/* Supports the adjust directory count operation. */
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

/* Supports the quota now operation. */
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
			if (error == 0) {
				zero = kern_calloc(1, ms->super.bsize);

				/*
				 * Zeroes the block, because a caller may read
				 * it before writing.
				 */
				absolute = cgstart(&ms->super, cg) + fragment;
				if (zero == NULL) {
					error = ENOMEM;
				} else {
					error = write_block(mountp, absolute,
							    zero);
					kern_free(zero);
				}
			}
			if (error != 0) {
				/*
				 * Puts the run back when the block could not be
				 * zeroed.
				 */
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
	struct ufs_mount_state *ms;
	uint8_t *map;
	uint32_t n;
	uint32_t free;
	uint32_t cg;
	uint32_t local;
	uint64_t old_total;
	int already_free;
	int released;
	int error;

	ms = state(mountp);
	local = 0;

	/* Finds the cylinder group the block being freed lives in. */
	for (cg = 0; cg < ms->super.ncg; cg++) {
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
	if (cg == ms->super.ncg)
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

	mutex_unlock(&ms->lock);

	/* Gives the block back to the owner quota. */
	if (error == 0) {
		released = quota_release(&ms->quota, uid, gid, 1, 0);

		/* An account that cannot be credited must not be written to. */
		if (released != 0) {
			ms->writable = 0;

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

/* Supports the allocate inode number operation. */
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
			ms->writable = 0;

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
	io_stats_record(IO_UFS_INDIRECT_WINDOW, sizeof(sector));

	/* Reads the sector the entry lives in. */
	error = read_metadata_sectors(mountp, lba, 1, sector);
	if (error == 0) {
		*result = drv_ufs_get64(sector,
					(size_t)(byte_offset % UFS_SECTOR_SIZE),
					super->swapped);
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
		error = indirect_entry(inode->i_mount,
				       fragment,
				       index,
				       &fragment);
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
					rollback_value = disk_sync(
						inode->i_mount->m_disk);
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
					state(inode->i_mount)->writable = 0;

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
		error = allocate_block(inode->i_mount,
				       inode->i_uid,
				       inode->i_gid,
				       &allocated_value);
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
					disk_sync(inode->i_mount->m_disk);

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
				state(inode->i_mount)->writable = 0;

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

			/* Takes a block and publishes it into the entry. */
			error = allocate_block(inode->i_mount, inode->i_uid,
					       inode->i_gid, &next);
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
						rollback_error = disk_sync(inode->i_mount->m_disk);
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
						state(inode->i_mount)->writable = 0;
					}
				}

				kern_free(block);

				/* Failed. */
				return error;
			}

			ui->blocks += s->bsize / UFS_SECTOR_SIZE;
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
	int *mapping_error)
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
		error = bmap(inode, logical + blocks, &next);
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
 * Reads full mapped runs directly and retains block scratch for edges and
 * holes.
 */
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

	/* Rejects an offset before the start of the file. */
	if (offset < 0)
		return -EINVAL;

	/* Succeeded: a read at or past the end returns nothing. */
	if (offset >= inode->i_size || length == 0)
		return 0;

	/* Clamps the run to what is left of the file. */
	if ((uint64_t)length > (uint64_t)inode->i_size - (uint64_t)offset)
		length = (size_t)((uint64_t)inode->i_size - (uint64_t)offset);

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
		error = bmap(inode, logical, &fragment);
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
			content_run_bytes(inode, logical, fragment, length - done, 0, &mapping_error) :
			0;
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
				memcpy((uint8_t *)buffer + done,
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
		memset(raw + UFS_DI_DB, 0, 120U);

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
		memset(raw + UFS_DI_DB, 0, 120U);
		memcpy(raw + UFS_DI_DB, ui->shortlink, (size_t)inode->i_size);
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

	memset(outcome, 0, sizeof(*outcome));

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
			ms->writable = 0;

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
	memset(images, 0, sizeof(*images));
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

	memset(run->tree_memory, 0, UFS_NIADDR * super->bsize);

	/* Starts out with no level of the path recorded. */
	for (n = 0; n < UFS_NIADDR; n++)
		run->tree_images[n] = run->tree_memory + n * super->bsize;

	/* A path already recorded is not recorded twice. */
	if (run->tree_count != 0)
		memcpy(run->tree_images[0], run->old_leaf, super->bsize);

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
	int free_bit;
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
		 * Find the first complete free block and its bounded contiguous
		 * run.
		 */
		for (fragment = ms->super.dblkno;
		     fragment < ndblk;
		     fragment += ms->super.frag) {
			count = 0;

			/*
			 * Walks the group for as many free blocks as the run
			 * reserved.
			 */
			while (count < run->reserved) {
				/*
				 * A run reaching past the group cannot be taken
				 * from it.
				 */
				if ((uint64_t)fragment + (count + 1U) * ms->super.frag > ndblk)
					break;

				/*
				 * Counts the free fragments that make up one
				 * block.
				 */
				for (n = 0; n < ms->super.frag; n++) {
					/*
					 * Asks whether this fragment is free.
					 */
					free_bit = bit_test(map, fragment + count * ms->super.frag + n);

					/* A used fragment ends the run. */
					if (!free_bit)
						break;
				}

				/*
				 * A block with a used fragment cannot be taken.
				 */
				if (n != ms->super.frag)
					break;

				count++;
			}

			/*
			 * A group that cannot cover the tree is no use to this
			 * run.
			 */
			if (count <= run->missing_nodes)
				continue;

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

			memcpy(run->old_cg, ms->cg, ms->super.bsize);
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
			error = disk_sync(inode->i_mount->m_disk);
	}

	/*
	 * Keep allocations charged and stop writes if reachability is
	 * uncertain.
	 */
	if (error != 0) {
		ms->writable = 0;

		/* Failed. */
		return error;
	}

	/*
	 * Restore allocation summaries only after no durable pointer can refer
	 * here.
	 */
	memcpy(ms->cg, run->old_cg, ms->super.bsize);
	ms->super.cstotal_nbfree = run->old_total;

	/* Writes the cylinder group back with the blocks freed. */
	error = write_cg(inode->i_mount);
	if (error == 0)
		error = disk_sync(inode->i_mount->m_disk);
	if (error != 0)
		ms->writable = 0;

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

	if (error == 0) {
		io_stats_record(IO_UFS_CONTENT_WRITE, bytes);
		error = write_sectors_context(inode->i_mount,
					      run->first << ms->super.fsbtodb,
					      (uint32_t)(bytes / UFS_SECTOR_SIZE),
					      buffer,
					      context);
	}

	if (error == 0)
		error = disk_sync(inode->i_mount->m_disk);

	/*
	 * Prepare the new inode and leaf without changing the published inode.
	 */

	ui = info(inode);
	memcpy(&run->image, ui, sizeof(run->image));

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
			error = disk_sync(inode->i_mount->m_disk);
	}

	/*
	 * Publish in-memory state only after a durable private metadata image.
	 */
	retained = 1;

	/*
	 * A committed group is durable even when the write reported an error.
	 */
	if (error == 0 || run->committed) {
		memcpy(ui->direct, run->image.direct, sizeof(ui->direct));
		memcpy(ui->indirect, run->image.indirect, sizeof(ui->indirect));
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
			memcpy(ms->cg, run->old_cg, ms->super.bsize);
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

	/*
	 * Finds one complete aligned data block within the filesystem geometry.
	 */
	for (cg = 0; cg < super->ncg; cg++) {
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

		memset(group->data, 0, ms->super.bsize);
		memcpy(group->data, area, length);
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
			memcpy(group->cg[slot], ms->cg, ms->super.bsize);
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
	memcpy(&group->image, ui, sizeof(group->image));
	memset(group->image.extattr, 0, sizeof(group->image.extattr));

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
		memcpy(ui->extattr, group->image.extattr, sizeof(ui->extattr));
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
			ms->writable = 0;

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

				memcpy(group->images.cg[0], ms->cg,
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
	unsigned n;
	int error;

	/*
	 * Checks the requested owner before selecting backing for its first
	 * block.
	 */
	ms = state(inode->i_mount);
	ui = info(inode);
	images = &group->images;

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
		/*
		 * A directory block is only the first if the directory is still
		 * empty.
		 */
		if (inode->i_type != INODE_DIR || inode->i_size != 0)
			return EIO;

		/* Refuses an inode that already names direct blocks. */
		for (n = 0; n < UFS_NDADDR; n++) {
			/*
			 * A direct pointer that is filled means this is not the
			 * first block.
			 */
			if (ui->direct[n] != 0)
				return EIO;
		}

		/* Refuses an inode that already names indirect blocks. */
		for (n = 0; n < UFS_NIADDR; n++) {
			/* An indirect pointer that is filled means the same. */
			if (ui->indirect[n] != 0)
				return EIO;
		}
	}

	/* Picks the block this allocation will take. */
	error = initial_block_candidate(inode, group);
	if (error != 0)
		return error;

	/*
	 * Prepares a fully initialized payload and a private reference to its
	 * reservation.
	 */
	memset(images->data, 0, ms->super.bsize);

	/* A caller with data writes it into the block before publishing. */
	if (length != 0)
		memcpy(images->data, area, length);

	memcpy(&images->image, ui, sizeof(images->image));

	/* An attribute block records its own length in the inode. */
	if (kind == UFS_INITIAL_XATTR) {
		images->image.extattr[0] = group->fragment;
		images->image.extattr_size = (uint32_t)length;
	} else {
		images->image.direct[0] = group->fragment;
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
		memcpy(ms->cg, images->cg[0], ms->super.bsize);
		ms->super.cstotal_nbfree--;
		ms->rotor_cg = group->cg;

		/* Publishes the attribute length the group committed. */
		if (kind == UFS_INITIAL_XATTR) {
			ui->extattr[0] = group->fragment;
			ui->extattr_size = (uint32_t)length;
		} else {
			ui->direct[0] = group->fragment;
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

/* Allocates empty directory backing without publishing any directory entry. */
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
					   &mapping_error);
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

				memset(scratch, 0, ms->super.bsize);
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
			memcpy(scratch + within, (const uint8_t *)buffer + done,
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

/* Supports the indirect span operation. */
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
	for (cg = 0; cg < ms->super.ncg; cg++) {
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
	if (cg == ms->super.ncg)
		return EIO;

	/* Reads that cylinder group so its free map can be inspected. */
	error = load_cg_locked(inode->i_mount, cg);
	if (error != 0)
		return error;

	memcpy(group->cg, ms->cg, ms->super.bsize);

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
	memcpy(&group->image, ui, sizeof(group->image));

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
		memcpy(ms->cg, group->cg, ms->super.bsize);
		ms->super.cstotal_nbfree++;
		memcpy(ui->direct, group->image.direct, sizeof(ui->direct));
		memcpy(ui->indirect, group->image.indirect,
		       sizeof(ui->indirect));
		ui->blocks = group->image.blocks;

		/* Gives the freed block back to the owner quota. */
		quota_error = quota_release(&ms->quota, inode->i_uid,
					    inode->i_gid, 1, 0);
		if (quota_error != 0) {
			ms->writable = 0;

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
		error = disk_sync(inode->i_mount->m_disk);
	if (error == 0) {
		error = free_block(inode->i_mount, fragment, inode->i_uid,
				   inode->i_gid);
	}
	if (error != 0)
		ms->writable = 0;

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
			error = disk_sync(inode->i_mount->m_disk);
		if (error == 0) {
			ui->blocks -= sectors;
			error = free_block(inode->i_mount, child, inode->i_uid,
					   inode->i_gid);
		}
		if (error != 0) {
			ms->writable = 0;
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

/* Supports the ufs truncate operation. */
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
			memset(block + size % ms->super.bsize,
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
		memcpy(ui->shortlink, raw + UFS_DI_DB, sizeof(ui->shortlink));
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

/* Supports the load inode locked operation. */
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
	struct mutex *gate;
	int entered;
	int error;

	/*
	 * The caller may already hold the gate, in which case this call waits.
	 */
	gate = &state(mountp)->namespace_lock;
	entered = !mutex_owned(gate);

	/* Releases the lock this call took. */
	if (entered)
		mutex_lock(gate);

	error = load_inode_locked(mountp, number, result);

	/* Releases the lock this call took. */
	if (entered)
		mutex_unlock(gate);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

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
	ssize_t read;
	struct ufs_mount_state *ms;
	uint8_t head[8];
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

		/* Reads the record header. */
		count = pread_inode(directory, head, sizeof(head), *cursor);
		if (count != sizeof(head))
			return EIO;
		*number = drv_ufs_get32(head, 0, ms->super.swapped);
		reclen = drv_ufs_get16(head, 4, ms->super.swapped);
		*type = head[6];

		/* The name length the header declares. */
		namelen = head[7];

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

		/* Reads the name that follows the header. */
		if (namelen != 0) {
			read = pread_inode(directory, name, namelen, *cursor + 8);

			/* A name the record promised must be there in full. */
			if (read != namelen) {
				/* Failed. */
				return EIO;
			}
		}
		name[namelen] = '\0';
		*cursor += reclen;

		/* Stops at the first record that names an inode. */
		if (*number != 0)
			return 0;
	}

	/* Failed. */
	return ENOENT;
}

/* Supports the dir minimum operation. */
static uint16_t
dir_minimum(
	uint8_t length)
{
	/* A record is the header, the name, and padding to four bytes. */
	return (uint16_t)((8U + length + 3U) & ~3U);
}

/* Supports the dir type operation. */
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

/* Supports the restore directory block operation. */
static int
restore_directory_block(
	struct inode *directory,
	uint64_t fragment,
	const uint8_t *original,
	int original_error)
{
	struct ufs_mount_state *ms;
	int rollback;

	ms = state(directory->i_mount);
	rollback = write_block(directory->i_mount, fragment, original);

	/* A failed restore leaves the volume unwritable. */
	if (rollback != 0) {
		ms->writable = 0;

		/* Failed. */
		return rollback;
	}

	/* Reports the failure that made the restore necessary. */
	return original_error;
}

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
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	uint32_t ino;
	uint32_t pos;
	uint32_t prev;
	uint16_t reclen;
	uint8_t nlen;
	int difference;
	int error;

	/* Takes the mount state this call runs against. */
	ms = state(directory->i_mount);

	/* Starts the walk at the first record, with nothing before it. */
	pos = 0;
	prev = UINT32_MAX;

	/* Takes the private inode the directory block pointer lives in. */
	ui = info(directory);

	/* Refuses a directory whose recorded size no block could hold. */
	if (directory->i_size < 0 ||
	    (uint64_t)directory->i_size > ms->super.bsize ||
	    (uint64_t)directory->i_size % UFS_DIRBLKSIZ != 0 ||
	    ui->direct[0] == 0) {
		/* Failed. */
		return EIO;
	}

	/* Reads the block the entries live in. */
	error = read_block(directory->i_mount, info(directory)->direct[0],
			   block);
	if (error)
		return error;

	/* Walks the entries of the block looking for the name. */
	while (pos < (uint32_t)directory->i_size) {
		/*
		 * A record that reaches past the block means the directory is
		 * corrupt.
		 */
		if ((uint32_t)directory->i_size - pos < 8U ||
		    pos % UFS_DIRBLKSIZ > UFS_DIRBLKSIZ - 8U) {
			/* Failed. */
			return EIO;
		}

		/* The inode number, the record length and the name length. */
		ino = drv_ufs_get32(block, pos, ms->super.swapped);
		reclen = drv_ufs_get16(block, pos + 4U, ms->super.swapped);
		nlen = block[pos + 7U];

		/* A record shorter than its own header cannot be one. */
		if (reclen < 8U)
			return EIO;	/* Failed. */

		/* Every record length is a multiple of four bytes. */
		if ((reclen & 3U) != 0)
			return EIO;	/* Failed. */

		/* A record may not straddle two directory blocks. */
		if (pos % UFS_DIRBLKSIZ + reclen > UFS_DIRBLKSIZ)
			return EIO;	/* Failed. */

		/* Nor run past the end of the directory itself. */
		if (pos + reclen > (uint32_t)directory->i_size)
			return EIO;	/* Failed. */

		/* And the name it declares has to fit inside it. */
		if (8U + nlen > reclen)
			return EIO;	/* Failed. */

		/*
		 * Compares the record name only when it could possibly match.
		 */
		difference = 1;
		if (ino != 0 && nlen == name->cn_namelen) {
			difference = memcmp(block + pos + 8U,
					    name->cn_nameptr,
					    nlen);
		}

		/*
		 * The record matches on its inode number and its name together.
		 */
		if (difference == 0) {
			*offset = pos;
			*previous = prev;
			*number = ino;

			/* Succeeded. */
			return 0;
		}

		prev = pos;
		pos += reclen;
	}

	/* Failed. */
	return ENOENT;
}

/* Supports the dir add operation. */
static int
dir_add(
	struct inode *directory,
	const struct componentname *name,
	uint32_t number,
	uint8_t type)
{
	uint32_t at;
	struct ufs_mount_state *ms;
	struct ufs_inode_info *ui;
	uint8_t *block;
	uint8_t *original;
	uint16_t need;
	uint32_t pos;
	uint64_t old_direct;
	uint64_t allocated;
	uint64_t old_blocks;
	off_t old_size;
	int error;
	int rollback;
	int handled;

	ms = state(directory->i_mount);
	ui = info(directory);
	pos = 0;
	allocated = 0;

	/* Validates the current name. */
	if (name->cn_namelen == 0 || name->cn_namelen > 255U)
		return EINVAL;

	/* Rejects a component that holds a byte no name may contain. */
	for (pos = 0; pos < name->cn_namelen; pos++) {
		/* Validates the current name. */
		if (name->cn_nameptr[pos] == '/')
			return EINVAL;
	}

	pos = 0;
	need = dir_minimum((uint8_t)name->cn_namelen);
	block = kern_calloc(1, ms->super.bsize);

	/* Takes the staging the directory block is edited in. */
	original = kern_malloc(ms->super.bsize);
	if (block == NULL || original == NULL) {
		kern_free(block);
		kern_free(original);

		/* Failed. */
		return ENOMEM;
	}

	mutex_lock(&directory->i_lock);

	old_size = directory->i_size;
	old_direct = ui->direct[0];
	old_blocks = ui->blocks;

	/* Refuses a directory whose recorded size no block could hold. */
	if (old_size < 0 || (uint64_t)old_size > ms->super.bsize ||
	    (uint64_t)old_size % UFS_DIRBLKSIZ != 0) {
		error = EIO;
		goto out;
	}

	/* A directory with no block yet gets its first one here. */
	if (ui->direct[0] == 0) {
		/*
		 * Asks whether the journal path has already created the block.
		 */
		error = directory_backing_group(directory, &handled);
		if (handled) {
			/* Reports why the first block could not be created. */
			if (error != 0)
				goto out;

			/*
			 * Retain committed empty backing if later entry
			 * publication fails.
			 */
			old_direct = ui->direct[0];
			old_blocks = ui->blocks;
		} else {
			/* Takes the first block of the directory. */
			error = allocate_block(
				directory->i_mount, directory->i_uid,
				directory->i_gid, &ui->direct[0]);
			if (error)
				goto out;
			allocated = ui->direct[0];
			ui->blocks += ms->super.bsize / UFS_SECTOR_SIZE;
		}
	}

	/* Reads the block the new entry will be placed in. */
	error = read_block(directory->i_mount, ui->direct[0], block);
	if (error)
		goto out;

	memcpy(original, block, ms->super.bsize);

	/* Walks the entries of the block looking for room. */
	while (pos < (uint32_t)directory->i_size) {
		uint16_t reclen;
		uint8_t nlen;
		uint16_t minimum;

		/*
		 * A record that reaches past the block means the directory is
		 * corrupt.
		 */
		if ((uint32_t)directory->i_size - pos < 8U ||
		    pos % UFS_DIRBLKSIZ > UFS_DIRBLKSIZ - 8U) {
			error = EIO;
			goto out;
		}

		reclen = drv_ufs_get16(block, pos + 4U, ms->super.swapped);
		nlen = block[pos + 7U];
		minimum = dir_minimum(nlen);

		/* A record shorter than its own header means the same. */
		if (reclen < minimum || (reclen & 3U) != 0 ||
		    pos % UFS_DIRBLKSIZ + reclen > UFS_DIRBLKSIZ ||
		    reclen > (uint32_t)directory->i_size - pos) {
			error = EIO;
			goto out;
		}

		/*
		 * An entry with slack after it can be split to hold the new
		 * name.
		 */
		if (reclen - minimum >= need) {
			at = pos + minimum;
			/* Shortens the record to what its name needs. */
			drv_ufs_put16(block, pos + 4U, minimum,
				      ms->super.swapped);

			/* And gives the bytes it gave up to a new record. */
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

	/* A directory that would outgrow one block cannot be extended here. */
	if ((uint64_t)directory->i_size + UFS_DIRBLKSIZ > ms->super.bsize) {
		error = ENOSPC;
		goto out;
	}

	/* Appends a fresh record at the end of the directory block. */
	pos = (uint32_t)directory->i_size;
	drv_ufs_put32(block, pos, number, ms->super.swapped);
	drv_ufs_put16(block, pos + 4U, UFS_DIRBLKSIZ, ms->super.swapped);
	block[pos + 6U] = type;
	block[pos + 7U] = (uint8_t)name->cn_namelen;
	memcpy(block + pos + 8U, name->cn_nameptr, name->cn_namelen);
	directory->i_size += UFS_DIRBLKSIZ;
	error = write_block(directory->i_mount, ui->direct[0], block);

commit:
	if (error == 0)
		error = persist_inode(directory);

	if (error != 0) {
		rollback = restore_directory_block(directory, ui->direct[0],
						   original, error);
		directory->i_size = old_size;
		ui->direct[0] = old_direct;
		ui->blocks = old_blocks;

		/* Publishes the directory with its new size. */
		error = persist_inode(directory);
		if (error == 0)
			error = disk_sync(directory->i_mount->m_disk);
		if (error != 0) {
			ms->writable = 0;
		} else if (ms->writable && allocated != 0) {
			/*
			 * Gives the block back when the directory could not be
			 * published.
			 */
			error = free_block(directory->i_mount, allocated,
					   directory->i_uid, directory->i_gid);
			if (error != 0)
				ms->writable = 0;
		}
		if (error == 0)
			error = rollback;
	}

out:
	if (error != 0 && allocated != 0 && ui->direct[0] == allocated) {
		directory->i_size = old_size;
		ui->direct[0] = old_direct;
		ui->blocks = old_blocks;

		/*
		 * Puts the recorded size back the way the failed write found
		 * it.
		 */
		rollback = persist_inode(directory);
		if (rollback == 0)
			rollback = disk_sync(directory->i_mount->m_disk);

		/* A failed restore leaves the volume unwritable. */
		if (rollback != 0) {
			ms->writable = 0;
			error = rollback;
		} else {
			rollback = free_block(directory->i_mount,
					      allocated,
					      directory->i_uid,
					      directory->i_gid);

			/*
			 * A block that could not be freed leaves the volume
			 * unwritable.
			 */
			if (rollback != 0) {
				ms->writable = 0;
				error = rollback;
			}
		}
	}

	mutex_unlock(&directory->i_lock);

	kern_free(original);
	kern_free(block);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the dir remove operation. */
static int
dir_remove(
	struct inode *directory,
	const struct componentname *name,
	uint32_t *number)
{
	uint16_t prior;
	struct ufs_mount_state *ms;
	uint8_t *block;
	uint8_t *original;
	uint32_t offset;
	uint32_t previous;
	int error;

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

	mutex_lock(&directory->i_lock);

	/* Finds the record the name occupies, and the one before it. */
	error = dir_find_record(directory,
				name,
				block,
				&offset,
				&previous,
				number);
	if (error == 0) {
		uint16_t reclen;

		memcpy(original, block, ms->super.bsize);

		reclen = drv_ufs_get16(block, offset + 4U, ms->super.swapped);

		/* The preceding record absorbs the one being removed. */
		if (previous != UINT32_MAX &&
		    previous / UFS_DIRBLKSIZ == offset / UFS_DIRBLKSIZ) {
			prior = drv_ufs_get16(block,
					      previous + 4U,
					      ms->super.swapped);
			drv_ufs_put16(block,
				      previous + 4U,
				      prior + reclen,
				      ms->super.swapped);
		} else {
			drv_ufs_put32(block, offset, 0, ms->super.swapped);
		}

		/* Writes the block back with the record gone. */
		error = write_block(directory->i_mount,
				    info(directory)->direct[0],
				    block);
		if (error != 0) {
			error = restore_directory_block(directory,
							info(directory)->direct[0],
							original,
							error);
		}
	}

	mutex_unlock(&directory->i_lock);

	kern_free(original);
	kern_free(block);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

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
	struct ufs_mount_state *ms;
	uint8_t *block;
	uint8_t *original;
	uint32_t offset;
	uint32_t previous;
	int error;

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

	mutex_lock(&directory->i_lock);

	/* Finds the record the name occupies. */
	error = dir_find_record(directory,
				name,
				block,
				&offset,
				&previous,
				old_number);
	if (error == 0) {
		memcpy(original, block, ms->super.bsize);
		(void)previous;

		*old_type = block[offset + 6U];

		drv_ufs_put32(block, offset, number, ms->super.swapped);

		block[offset + 6U] = type;

		/* Writes the block back with the record pointing elsewhere. */
		error = write_block(directory->i_mount,
				    info(directory)->direct[0],
				    block);
		if (error != 0) {
			error = restore_directory_block(directory,
							info(directory)->direct[0],
							original,
							error);
		}
	}

	mutex_unlock(&directory->i_lock);

	kern_free(original);
	kern_free(block);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the name is dot operation. */
static int
name_is_dot(
	const struct componentname *name)
{
	/* The two names every directory holds for itself and its parent. */
	return (name->cn_namelen == 1U && name->cn_nameptr[0] == '.') ||
		(name->cn_namelen == 2U && name->cn_nameptr[0] == '.' &&
		 name->cn_nameptr[1] == '.');
}

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

	/* A socket keeps no storage of its own once it is detached. */
	if (inode->i_type == INODE_SOCKET) {
		inode->i_special = NULL;
		inode->i_special_destroy = NULL;
	}

	mutex_unlock(&inode->i_lock);
}

/* Supports the discard new inode operation. */
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
			ms->writable = 0;
			inode_release(inode);

			/* Failed. */
			return EIO;
		}
	}

	/* Takes the indirect roots the creation had already allocated. */
	for (n = 0; n < UFS_NIADDR; n++) {
		/* An indirect pointer that was filled has a block to free. */
		if (ui->indirect[n] != 0) {
			ms->writable = 0;
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
		cleanup = disk_sync(inode->i_mount->m_disk);

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
		ms->writable = 0;
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
		ms->writable = 0;
	inode->i_ino = 0;
	inode->i_flags |= INODE_DEAD;
	inode_release(inode);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the discard new inode after error operation. */
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
	memset(raw, 0, UFS_DINODE_SIZE);
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
	memcpy(group->images.cg, ms->cg, ms->super.bsize);
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
		memcpy(ms->cg, group->images.cg, ms->super.bsize);
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
				state(mountp)->writable = 0;

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

/* Supports the ufs lookup locked operation. */
static int
ufs_lookup_locked(
	struct inode *directory,
	const struct componentname *component,
	struct inode **result)
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
		length = strlen(name);
		difference = memcmp(name, component->cn_nameptr,
				    component->cn_namelen);

		/*
		 * The entry matches on its name length and its bytes together.
		 */
		if (length == component->cn_namelen && difference == 0) {
			/* Reads the inode the entry names. */
			looked_up =
				load_inode(directory->i_mount, number, result);

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

/* Supports the ufs lookup operation. */
static int
ufs_lookup(
	struct inode *directory,
	const struct componentname *component,
	struct inode **result)
{
	struct mutex *gate;
	int entered;
	int error;

	/*
	 * The caller may already hold the gate, in which case this call waits.
	 */
	gate = &state(directory->i_mount)->namespace_lock;
	entered = !mutex_owned(gate);

	/* Releases the lock this call took. */
	if (entered)
		mutex_lock(gate);
	error = ufs_lookup_locked(directory, component, result);

	/* Releases the lock this call took. */
	if (entered)
		mutex_unlock(gate);

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
	error = dir_find_record(directory, name, group->directory, &offset,
				&previous, &number);
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

	memcpy(&group->image, info(target), sizeof(group->image));

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
		memcpy(&group->parent_image, info(directory),
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
	extents[0].target = info(directory)->direct[0] << ms->super.fsbtodb;
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
 * Inserts into a private existing directory block, including reusable empty
 * records.
 */
static int
directory_image_insert(
	struct inode *directory,
	const struct componentname *name,
	struct inode *target,
	uint8_t *block)
{
	struct ufs_mount_state *ms;
	uint32_t pos;
	uint32_t at;
	uint32_t number;
	uint16_t need;
	uint16_t length;
	uint16_t minimum;
	uint16_t available;
	uint8_t namesize;
	unsigned n;
	int difference;

	ms = state(directory->i_mount);
	at = UINT32_MAX;
	available = 0;

	/*
	 * Bounds names and the single-block directory before modifying private
	 * bytes.
	 */
	if (name->cn_namelen == 0 || name->cn_namelen > 255U)
		return EINVAL;

	/* Rejects a component that holds a byte no name may contain. */
	for (n = 0; n < name->cn_namelen; n++) {
		/* Validates the current name. */
		if (name->cn_nameptr[n] == '/')
			return EINVAL;
	}

	/* Refuses a directory whose recorded size no block could hold. */
	if (directory->i_size < 0 ||
	    (uint64_t)directory->i_size > ms->super.bsize ||
	    (uint64_t)directory->i_size % UFS_DIRBLKSIZ != 0) {
		/* Failed. */
		return EIO;
	}
	need = dir_minimum((uint8_t)name->cn_namelen);

	/*
	 * Chooses the first available record while validating the complete
	 * directory.
	 */
	pos = 0;

	/* Walks the entries of the block looking for room. */
	while (pos < (uint32_t)directory->i_size) {
		/*
		 * A record that reaches past the block means the directory is
		 * corrupt.
		 */
		if ((uint32_t)directory->i_size - pos < 8U ||
		    pos % UFS_DIRBLKSIZ > UFS_DIRBLKSIZ - 8U) {
			/* Failed. */
			return EIO;
		}
		number = drv_ufs_get32(block, pos, ms->super.swapped);
		length = drv_ufs_get16(block, pos + 4U, ms->super.swapped);
		namesize = block[pos + 7U];

		/* Measures the room this record needs. */
		minimum = dir_minimum(namesize);
		if (length < minimum || (length & 3U) != 0 ||
		    pos % UFS_DIRBLKSIZ + length > UFS_DIRBLKSIZ ||
		    length > (uint32_t)directory->i_size - pos) {
			/* Failed. */
			return EIO;
		}

		/*
		 * Compares the record name only when it could possibly match.
		 */
		difference = 1;
		if (number != 0 && namesize == name->cn_namelen)
			difference = memcmp(block + pos + 8U,
					    name->cn_nameptr, namesize);

		/* Refuses a name the directory already holds. */
		if (difference == 0) {
			/* Failed. */
			return EEXIST;
		}

		/* Remembers the first entry that has room for the new name. */
		if (at == UINT32_MAX) {
			/* A free entry long enough takes the new name whole. */
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

	/* No entry had room, so the directory has to grow. */
	if (at == UINT32_MAX) {
		/*
		 * A directory that would outgrow one block cannot be extended
		 * here.
		 */
		if ((uint64_t)directory->i_size + UFS_DIRBLKSIZ >
		    ms->super.bsize) {
			/* Failed. */
			return ENOSPC;
		}
		at = (uint32_t)directory->i_size;
		available = UFS_DIRBLKSIZ;
		directory->i_size += UFS_DIRBLKSIZ;
	} else {
		/*
		 * Splits an occupied predecessor only after every record has
		 * been checked.
		 */
		pos = 0;
		/* Walks up to the entry the new name goes into. */
		while (pos < at) {
			/* Steps over one record. */
			length = drv_ufs_get16(block, pos + 4U,
					       ms->super.swapped);
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
	 * mutation.
	 */
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

	memcpy(&group->directory_image, info(directory), sizeof(group->directory_image));
	memcpy(&group->target_image, info(target), sizeof(group->target_image));

	group->target_image.inode.i_linkcount++;

	/* Stages the block the new name goes into. */
	error = metadata_image_get(&group->images,
				   info(directory)->direct[0],
				   &group->directory);
	if (error != 0)
		return error;

	/* Writes the new name into that block. */
	error = directory_image_insert(&group->directory_image.inode,
				       name,
				       target, group->directory);
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
	mutex_lock(&directory->i_lock);
	mutex_lock(&target->i_lock);
	mutex_lock(&ms->lock);

	error = link_group_locked(directory, name, target, group);

	mutex_unlock(&ms->lock);
	mutex_unlock(&target->i_lock);
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
 * Validates a complete private directory before replacing or removing one name.
 */
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
	uint32_t minimum;
	uint16_t length;
	uint8_t namesize;
	int difference;

	/*
	 * Bounds traversal before inspecting record bytes.
	 */

	/* Takes the mount the image belongs to. */
	ms = state(directory->i_mount);
	if (directory->i_size < 0 ||
	    (uint64_t)directory->i_size > ms->super.bsize ||
	    (uint64_t)directory->i_size % UFS_DIRBLKSIZ != 0) {
		/* Failed. */
		return EIO;
	}

	/*
	 * Records one matching entry while rejecting malformed or duplicate
	 * records.
	 */
	pos = 0;
	previous = found = prior = UINT32_MAX;
	while (pos < (uint32_t)directory->i_size) {
		/*
		 * A record that reaches past the block means the directory is
		 * corrupt.
		 */
		if ((uint32_t)directory->i_size - pos < 8U)
			return EIO;
		length = drv_ufs_get16(block, pos + 4U, ms->super.swapped);

		/* The name length this record declares. */
		namesize = block[pos + 7U];

		/* The smallest record that could hold a name that long. */
		minimum = dir_minimum(namesize);
		if (length < minimum || (length & 3U) != 0 ||
		    pos % UFS_DIRBLKSIZ + length > UFS_DIRBLKSIZ ||
		    length > (uint32_t)directory->i_size - pos) {
			/* Failed. */
			return EIO;
		}

		/* The inode number this record names. */
		number = drv_ufs_get32(block, pos, ms->super.swapped);

		/*
		 * Compares the record name only when it could possibly match.
		 */
		difference = 1;
		if (number != 0 && namesize == name->cn_namelen)
			difference = memcmp(block + pos + 8U,
					    name->cn_nameptr, namesize);

		/* The record this change is looking for. */
		if (difference == 0) {
			/*
			 * Refuses a record that is not the one this change
			 * expects.
			 */
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
	 * predecessor.
	 */
	if (replacement != 0) {
		drv_ufs_put32(block, found, replacement, ms->super.swapped);
		block[found + 6U] = type;
	} else if (prior != UINT32_MAX &&
		   prior / UFS_DIRBLKSIZ == found / UFS_DIRBLKSIZ) {
		/* The record being removed and the one in front of it. */
		length = drv_ufs_get16(block, found + 4U, ms->super.swapped);
		length += drv_ufs_get16(block, prior + 4U, ms->super.swapped);

		/* The one in front grows over the one that is going away. */
		drv_ufs_put16(block, prior + 4U, length, ms->super.swapped);
	} else {
		drv_ufs_put32(block, found, 0, ms->super.swapped);
	}

	/*
	 * Leaves every changed byte private until the enclosing group is
	 * committed.
	 */
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
	memcpy(&group->old_image, info(old_directory),
	       sizeof(group->old_image));
	memcpy(&group->new_image, info(new_directory),
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
		memcpy(&group->target_image, info(target),
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
				   info(old_directory)->direct[0], &old_block);
	if (error != 0)
		return error;

	/* Removes the old name from that block. */
	error = directory_image_change(old_image, old_block, old_name,
				       (uint32_t)source->i_ino, 0, 0);
	if (error != 0)
		return error;

	/* Stages the block the new name lives in. */
	error = metadata_image_get(&group->images,
				   info(new_directory)->direct[0], &new_block);
	if (error != 0)
		return error;

	/* Points the existing entry at the inode being renamed. */
	if (target != NULL) {
		error = directory_image_change(new_image,
					       new_block,
					       new_name,
					       (uint32_t)target->i_ino,
					       (uint32_t)source->i_ino,
					       dir_type(source->i_type));
	} else {
		error = directory_image_insert(new_image,
					       new_name,
					       source,
					       new_block);
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
	fragments[0] = info(old_directory)->direct[0];
	fragments[1] = info(new_directory)->direct[0];
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

	mutex_lock(&ms->lock);
	error = rename_group_locked(old_directory,
				    old_name,
				    new_directory,
				    new_name,
				    source,
				    target,
				    group);
	mutex_unlock(&ms->lock);

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
	struct ufs_link_group *group)
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

	/* And the parent has to have the block its names live in. */
	if (ui->direct[0] == 0)
		return EIO;	/* Failed. */

	/* A new directory brings a link to its parent with it. */
	is_directory = target->i_type == INODE_DIR;
	if (is_directory && directory->i_linkcount == UINT16_MAX)
		return EMLINK;

	/*
	 * Keeps all prepared content while changing only the final link
	 * relationship.
	 */
	memcpy(&group->directory_image, info(directory),
	       sizeof(group->directory_image));
	memcpy(&group->target_image, info(target), sizeof(group->target_image));
	group->target_image.inode.i_linkcount = is_directory ? 2 : 1;

	/* A new directory starts with the two links it names itself by. */
	if (is_directory)
		group->directory_image.inode.i_linkcount++;

	/* Stages the block the new name goes into. */
	error = metadata_image_get(&group->images, info(directory)->direct[0],
				   &group->directory);
	if (error != 0)
		return error;

	/* Writes the new name into that block. */
	error = directory_image_insert(&group->directory_image.inode, name,
				       target, group->directory);
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
	struct ufs_inode_info *ui;
	size_t bytes;
	int handled;
	int error;

	/*
	 * Establishes an unambiguous unpublished outcome before allocating
	 * resources.
	 */
	memset(outcome, 0, sizeof(*outcome));

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

	/* Takes the private inode the directory block pointer lives in. */
	ui = info(directory);

	/* Starts out with the directory block already in place. */
	error = 0;
	if (ui->direct[0] == 0) {
		/*
		 * Creates the directory block first when it does not exist yet.
		 */
		error = directory_backing_group(directory, &handled);
		if (error == 0 && !handled)
			error = EOPNOTSUPP;
	}
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
	mutex_lock(&ms->namespace_lock);

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

	mutex_unlock(&ms->namespace_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

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

	mutex_lock(&ms->namespace_lock);

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
			ms->writable = 0;
			inode_release(inode);
		} else {
			/*
			 * Undoes the creation now that nothing names the inode.
			 */
			cleanup = discard_new_inode(inode, 1);
			if (rollback_error == 0 && cleanup != 0)
				rollback_error = cleanup;
			if (rollback_error != 0)
				ms->writable = 0;
		}
		if (rollback_error != 0)
			error = rollback_error;
	}

out:

	mutex_unlock(&ms->namespace_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the ufs mknod operation. */
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

	mutex_lock(&ms->namespace_lock);

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

	mutex_unlock(&ms->namespace_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the ufs unlink operation. */
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

	mutex_lock(&ms->namespace_lock);

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
			ms->writable = 0;
	}

out:
	inode_release(target);

	mutex_unlock(&ms->namespace_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the directory empty operation. */
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
		dot = strcmp(name, ".");
		dotdot = strcmp(name, "..");

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

/* Supports the ufs rmdir operation. */
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
	mutex_lock(&ms->namespace_lock);

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
			ms->writable = 0;
	}

out:
	inode_release(target);

	mutex_unlock(&ms->namespace_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the ufs rename operation. */
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
	difference = memcmp(old_name->cn_nameptr, new_name->cn_nameptr,
			    old_name->cn_namelen);

	/* Succeeded: renaming a name onto itself changes nothing. */
	if (old_directory == new_directory &&
	    old_name->cn_namelen == new_name->cn_namelen &&
	    difference == 0) {
		/* Succeeded. */
		return 0;
	}

	mutex_lock(&ms->namespace_lock);

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
			ms->writable = 0;
	}

	inode_release(target);
	inode_release(source);

	mutex_unlock(&ms->namespace_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the ufs link operation. */
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
	mutex_lock(&ms->namespace_lock);

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
				ms->writable = 0;
		}
	}

out:

	mutex_unlock(&ms->namespace_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the ufs symlink operation. */
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
	length = strlen(target);

	/* Refuses a target longer than this volume can store in an inode. */
	if (length > ms->super.maxsymlinklen || length > 120U) {
		/* Failed. */
		return ENAMETOOLONG;
	}

	*result = NULL;

	mutex_lock(&ms->namespace_lock);

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
	memcpy(info(inode)->shortlink, target, length);

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

	mutex_unlock(&ms->namespace_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the pwrite inode operation. */
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

/* Supports the ufs read operation. */
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

/* Supports the ufs pread operation. */
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

/* Supports the ufs write operation. */
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

/* Supports the ufs pwrite operation. */
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
	int error = next_dirent(file->f_inode,
				&file->f_offset,
				&number,
				&type,
				name);

	/* Succeeded: the walk reached the end of the directory. */
	if (error == ENOENT) {
		*eof = 1;

		/* Succeeded. */
		return 0;
	}

	/* Failed: reports why the entry could not be read. */
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

	/* Succeeded. */
	return 0;
}

/* Supports the ufs readlink operation. */
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
		memcpy(buffer, info(inode)->shortlink, n);

		/* Succeeded: reports how many bytes the target has. */
		return (ssize_t)n;
	}

	/* A longer target is read from the blocks like any other file. */
	read_bytes = pread_inode(inode, buffer, length, 0);

	/* Reports how many bytes were read, or why the read failed. */
	return read_bytes;
}

/* Supports the extattr align operation. */
static size_t
extattr_align(
	size_t value)
{
	/* Every attribute record starts on an eight-byte boundary. */
	return (value + 7U) & ~(size_t)7U;
}

/* Supports the extattr name operation. */
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
	user_prefix = strncmp(name, "user.", 5);
	system_prefix = strncmp(name, "system.", 7);
	security_prefix = strncmp(name, "security.", 9);

	/* The prefix decides which namespace the attribute lives in. */
	if (user_prefix == 0) {
		*name_space = UFS_EXTATTR_NAMESPACE_USER;
		part = name + 5;
	} else if (system_prefix == 0) {
		*name_space = UFS_EXTATTR_NAMESPACE_SYSTEM;
		part = name + 7;

		/* A name may not reach the security namespace this way. */
		security_prefix = strncmp(part, "security.", 9);
		if (security_prefix == 0)
			return EINVAL;
	} else if (security_prefix == 0) {
		*name_space = UFS_EXTATTR_NAMESPACE_SYSTEM;
		part = name;
	} else {
		/* Failed. */
		return EOPNOTSUPP;
	}

	*stored_length = strlen(part);

	/* Refuses a name the record header could not hold. */
	if (*stored_length == 0 || *stored_length > 255U)
		return EINVAL;

	*stored = part;

	/* Succeeded. */
	return 0;
}

/* Supports the extattr load operation. */
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
			difference = memcmp(area + offset +
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

/* Supports the extattr publish operation. */
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
			error = disk_sync(inode->i_mount->m_disk);
		if (error != 0) {
			ui->extattr_size = old_size;
			ui->extattr[0] = old_ext[0];
			ui->extattr[1] = old_ext[1];
			ui->blocks = old_blocks;

			/* Writes the inode back as it stood before. */
			persisted = persist_inode(inode);

			/* A failed write leaves the volume unwritable. */
			if (persisted != 0)
				ms->writable = 0;

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
				ms->writable = 0;
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

	memcpy(block, area, length);

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
		error = disk_sync(inode->i_mount->m_disk);
	if (error != 0)
		goto rollback_metadata;

	/* Frees the blocks the old attribute spanned beyond the first. */
	if (old_count > 1U) {
		/* Gives the second attribute block back to the volume. */
		rollback = free_block(inode->i_mount, old_ext[1],
				      inode->i_uid, inode->i_gid);

		/* A block that cannot be freed leaves it unwritable. */
		if (rollback != 0) {
			ms->writable = 0;
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
		rollback = disk_sync(inode->i_mount->m_disk);

	/* A failed restore leaves the volume unwritable. */
	if (rollback != 0) {
		/*
		 * The new pointer may still be committed: keep its allocation.
		 */
		ms->writable = 0;
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
				ms->writable = 0;
		}
	} else if (old_area != NULL) {
		/* Puts the previous attribute area back where it was. */
		rollback = write_block(inode->i_mount, old_ext[0], old_area);

		/* A write that fails leaves the volume unwritable. */
		if (rollback != 0)
			ms->writable = 0;
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

/* Supports the ufs getxattr operation. */
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
		memcpy(value, area + content_at, content_length);

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

/* Supports the ufs setxattr operation. */
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
		memcpy(updated, area, at);

	drv_ufs_put32(updated, at, (uint32_t)new_record, ms->super.swapped);
	updated[at + 4U] = name_space;
	updated[at + 5U] = (uint8_t)padding;
	updated[at + 6U] = (uint8_t)stored_length;
	memcpy(updated + at + UFS_EXTATTR_HEADER_SIZE, stored, stored_length);

	/* Writes the new record into the staging. */
	if (size != 0)
		memcpy(updated + at + base, value, size);

	/* Copies the records that follow the one being written. */
	if (area_length > at + replaced) {
		memcpy(updated + at + new_record, area + at + replaced,
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

/* Supports the ufs listxattr operation. */
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
			security = memcmp(disk_name, "security.", 9);

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

	/*
	 * Failed: reports the error as a negative value, as the caller expects.
	 */
	if (error != 0)
		return -(ssize_t)error;

	/* Succeeded: reports how much room the list needs. */
	return (ssize_t)needed;
}

/* Supports the ufs removexattr operation. */
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
			memcpy(updated, area, at);

		/* Copies the records that follow the one being removed. */
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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the ufs getattr operation. */
static int
ufs_getattr(
	struct inode *inode,
	struct stat *status)
{
	struct ufs_inode_info *ui;

	/* Takes the UFS half of the inode. */
	ui = info(inode);

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

	/* Succeeded. */
	return 0;
}

/* Supports the valid disk time operation. */
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
	struct inode_time old_atime;
	struct inode_time old_mtime;
	struct inode_time old_ctime;
	struct ufs_mount_state *ms;
	long atime_nsec = 0, mtime_nsec = 0, ctime_nsec = 0;
	int representable;
	int error;
	int quota_moved = 0;

	memset(&quota_transfer_state, 0, sizeof(quota_transfer_state));

#ifdef ZEDBSD_SYS_STAT_H
	atime_nsec = status->st_atim.tv_nsec;
	mtime_nsec = status->st_mtim.tv_nsec;
	ctime_nsec = status->st_ctim.tv_nsec;
#endif

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

/* Supports the ufs inode sync operation. */
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

	memcpy(group->cg, ms->cg, ms->super.bsize);
	memcpy(&group->image, ui, sizeof(group->image));
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
		memcpy(ms->cg, group->cg, ms->super.bsize);
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
			ms->writable = 0;

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
	error = disk_sync(inode->i_mount->m_disk);
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
	memset(&outcome, 0, sizeof(outcome));
	mutex_lock(&inode->i_lock);
	mutex_lock(&ms->lock);

	memcpy(image, info(inode), sizeof(*image));
	image->inode.i_linkcount = 0;

	/* A writable volume publishes the unwind; a read-only one cannot. */
	if (ms->writable)
		error = prepare_inode_locked(&image->inode, block, &fragment);
	else
		error = EROFS;

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

/* Supports the ufs file sync operation. */
static int
ufs_file_sync(
	struct file *file)
{
	int synced;
	int error;

	/* A call without a file has nothing to flush. */
	if (file == NULL)
		return EINVAL;
	error = inode_sync(file->f_inode);

	/* The mount is only flushed when the file itself came out clean. */
	if (error != 0)
		synced = error;
	else
		synced = ufs_sync(file->f_inode->i_mount);

	/* Reports how the flush went. */
	return synced;
}

/* Supports the ufs alloc inode operation. */
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

/* Supports the ufs free inode operation. */
static void
ufs_free_inode(
	struct inode *inode)
{
	kern_free(inode);
}

/* Supports the ufs read super operation. */
static int
ufs_read_super(
	struct disk *disk,
	struct ufs_super *super)
{
	uint8_t *buffer;
	int error;

	/* This driver reads 512-byte sectors and nothing else. */
	if (disk == NULL || disk->d_block_size != UFS_SECTOR_SIZE)
		return EOPNOTSUPP;

	/* Takes the staging the superblock is read into. */
	buffer = kern_malloc(UFS_SBLOCK_SIZE);
	if (buffer == NULL)
		return ENOMEM;

	/* Reads the superblock from its fixed offset. */
	error = observed_disk_read(disk, UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE,
				   UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE, buffer);
	if (error == 0) {
		error = drv_ufs_super_decode(buffer, UFS_SBLOCK_SIZE,
					     disk->d_block_count, super);
	}

	kern_free(buffer);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the ufs identity hex operation. */
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

/* Supports the ufs identity hex32 operation. */
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

/* Supports the ufs write clean operation. */
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

/* Supports the ufs probe operation. */
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

/* Supports the ufs quota rebuild operation. */
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

/* Supports the ufs quota load operation. */
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

/* Supports the snapshot disk submit operation. */
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

/* Supports the snapshot disk publish operation. */
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
		memcpy(disk->d_name, "ufssnap", 7);

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

/* Supports the snapshot disk remove operation. */
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

/* Supports the ufs state free operation. */
static void
ufs_state_free(
	struct ufs_mount_state *ms)
{
	/* Freeing a mount that was never set up does nothing. */
	if (ms == NULL)
		return;

	journal_image_free(ms);
	buf_view_release(&ms->cg_view);
	kern_free(ms->snapshot_map);
	kern_free(ms->cg);
	kern_free(ms);
}

/* Supports the ufs quota persist operation. */
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

	/* Sizes the staging from the images the recovery will hold. */
	bytes = 3U * ms->super.bsize + UFS_SBLOCK_SIZE;
	if (ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
		/* Failed. */
		return EOPNOTSUPP;
	}

	memset(owner, 0, sizeof(*owner));
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

		memcpy(scan->bitmap, ms->cg + ms->cg_iusedoff, map_bytes);

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

	/*
	 * Keeps readonly and nonjournal admission free of orphan-reclamation
	 * writes.
	 */

	/* Takes the mount the recovery runs against. */
	ms = state(mountp);
	if (!ms->writable || !ms->journal_enabled)
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
	mutex_lock(&ms->namespace_lock);

	/* Walks the volume for inodes nothing names. */
	error = orphan_scan_locked(mountp, scan);
	if (error != 0)
		ms->writable = 0;

	mutex_unlock(&ms->namespace_lock);

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

static int
ufs_mount_impl(
	struct mount *mountp)
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
	(void)mutex_init(&ms->snapshot_lock, LOCK_RANK_DEVICE, "ufs snapshot");

	/* Finds the journal, if this volume carries one. */
	error = journal_discover(mountp, ms);
	if (error != 0) {
		mountp->m_data = NULL;
		ufs_state_free(ms);

		/* Failed. */
		return error;
	}

	/* Finds the snapshot device, if this volume carries one. */
	error = snapshot_discover(mountp, ms);
	if (error != 0) {
		mountp->m_data = NULL;
		ufs_state_free(ms);

		/* Failed. */
		return error;
	}

	(void)mutex_init(&ms->namespace_lock, LOCK_RANK_NAMESPACE, "ufs namespace");
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
		 * A volume without a journal cannot replay, so it must be
		 * clean.
		 */
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

	/* Publishes the rebuilt summaries. */
	if (error == 0)
		error = ufs_quota_rebuild(mountp);
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
	error = next_dirent(root, &cursor, &number, &type, name);
	if (error == 0) {
		/* The first entry of every directory names the directory. */
		difference = strcmp(name, ".");
		if (number != UFS_ROOT_INO || difference != 0)
			error = EIO;
	}

	if (error == 0)
		error = next_dirent(root, &cursor, &number, &type, name);
	if (error == 0) {
		/* The second names the parent, which for the root is itself. */
		difference = strcmp(name, "..");
		if (number != UFS_ROOT_INO || difference != 0)
			error = EIO;
	}

	/* Loads the quota configuration and recovers any orphaned inodes. */
	if (error == 0)
		error = ufs_quota_load(mountp, root);
	if (error == 0)
		error = orphan_recover(mountp);
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
		error = ufs_write_clean(mountp, 0);
		if (error) {
			root->i_flags |= INODE_DEAD;
			inode_release(root);
			mountp->m_data = NULL;
			ufs_state_free(ms);

			/* Failed. */
			return error;
		}
	}

	root->i_flags |= INODE_ROOT;
	mountp->m_root = root;

	/* Publishes the snapshot device, if this volume carries one. */
	if (ms->snapshot.active) {
		/* Gives the snapshot a device of its own to be read through. */
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

	mutex_lock(&ms->lock);
	mutex_lock(&ms->journal_lock);

	error = journal_checkpoint_locked(mountp);

	mutex_unlock(&ms->journal_lock);

	/* The mount is only durable once the device has it. */
	if (error == 0)
		error = disk_sync(mountp->m_disk);

	mutex_unlock(&ms->lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the ufs statvfs operation. */
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

	/* Succeeded. */
	return 0;
}

/* Supports the ufs quotactl operation. */
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
	if (ms == NULL || request == NULL || request->type > ZEDBSD_QUOTA_GROUP)
		return EINVAL;

	type = request->type == ZEDBSD_QUOTA_USER ? QUOTA_USER : QUOTA_GROUP;

	/* Runs the operation the request names. */
	switch (request->command) {
	case ZEDBSD_QUOTA_GET:

		/* Reads the record the caller asked about. */
		error = quota_get(&ms->quota, type, request->id, &record);
		if (error != 0)
			return error;

		/* Reports whether quotas are switched on for that type. */
		error = quota_enabled(&ms->quota, type, &enabled);
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

		/* Reads the grace period the caller asked about. */
		applied = quota_get_grace(&ms->quota, &request->grace_seconds);

		/* Reports the grace period, or why it could not be read. */
		return applied;
	case ZEDBSD_QUOTA_SET:
		/* Refuses to write to a volume that is no longer writable. */
		if (!ms->writable)
			return EROFS;
		mutating = 1;
		break;
	case ZEDBSD_QUOTA_ENABLE:
	case ZEDBSD_QUOTA_DISABLE:
		/* Refuses to write to a volume that is no longer writable. */
		if (!ms->writable)
			return EROFS;
		mutating = 1;
		break;
	case ZEDBSD_QUOTA_SYNC:
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
	case ZEDBSD_QUOTA_SET:
		/* Builds the record out of the limits the caller named. */
		memset(&record, 0, sizeof(record));
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
	case ZEDBSD_QUOTA_ENABLE:
		/* Switches accounting on for the type the request names. */
		error = quota_enable(&ms->quota, type, 1);
		break;
	case ZEDBSD_QUOTA_DISABLE:
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
			ms->writable = 0;
	}
	kern_free(saved);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the ufs snapshotctl operation. */
static int
ufs_snapshotctl(
	struct mount *mountp,
	struct snapshot_control *request)
{
	struct ufs_mount_state *ms;
	int error;

	ms = state(mountp);
	error = 0;

	/* Rejects a call that names no mount or no request. */
	if (ms == NULL || request == NULL)
		return EINVAL;

	memset(request->device, 0, sizeof(request->device));

	/* A volume without snapshot storage has nothing to control. */
	if (!ms->snapshot_available)
		return EOPNOTSUPP;

	/* Runs the operation the request names. */
	switch (request->command) {
	case ZEDBSD_SNAPSHOT_CREATE:
		/* Refuses to write to a volume that is no longer writable. */
		if (!ms->writable)
			return EROFS;
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
			return error;
		if (error == 0)
			error = snapshot_disk_publish(ms);
		if (error != 0 && ms->snapshot.active) {
			mutex_lock(&ms->snapshot_lock);
			(void)drv_ufs_snapshot_delete(&ms->snapshot);
			mutex_unlock(&ms->snapshot_lock);
		}

		break;
	case ZEDBSD_SNAPSHOT_DELETE:
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
	case ZEDBSD_SNAPSHOT_STATUS:
		break;
	default:
		/* Failed. */
		return EINVAL;
	}

	/* Failed: reports why the snapshot could not be deleted. */
	if (error != 0)
		return error;

	request->flags = ms->snapshot.active ? ZEDBSD_SNAPSHOT_F_ACTIVE : 0;
	request->captured_sectors = ms->snapshot.next_record;
	request->capacity_sectors = ms->snapshot.max_records;

	/* Publishes the snapshot device the request created. */
	if (ms->snapshot_disk != NULL) {
		memcpy(request->device, ms->snapshot_disk->d_name,
		       sizeof(request->device));
	}

	/* Succeeded. */
	return 0;
}

/* Supports the ufs prepare unmount operation. */
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

	/* A writable volume is marked clean on the way out. */
	error = 0;
	if (ms != NULL && ms->writable)
		error = ufs_write_clean(mountp, 1);

	/* Reports whether the volume could be left clean. */
	return error;
}

/* Supports the ufs unmount operation. */
static void
ufs_unmount(
	struct mount *mountp)
{
	struct ufs_mount_state *ms;

	/* A mount that was never set up has nothing to release. */
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

/* Supports the checksum operation. */
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

/* Supports the put64 operation. */
static void
put64(
	uint8_t *p,
	uint64_t v)
{
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}

/* Supports the get32 operation. */
static uint32_t
get32(
	const uint8_t *p)
{
	/* A journal record is stored little-endian, whatever the volume is. */
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
		(uint32_t)p[3] << 24;
}

/* Supports the get64 operation. */
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

/* Supports the clear record operation. */
static int
clear_record(
	struct ufs_journal *journal,
	uint64_t sector)
{
	int cleared;
	uint8_t zero[SECTOR_SIZE];
	int error;

	memset(zero, 0, sizeof(zero));
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
		memcpy(descriptor, journal->image, SECTOR_SIZE);

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
					memcpy(sector, journal->image + offset,
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
			memcpy(journal->image, descriptor, SECTOR_SIZE);
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
			memcpy(view, descriptor, SECTOR_SIZE);

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
			memcpy((uint8_t *)buffer + (size_t)done * SECTOR_SIZE,
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

/* Supports the snapshot put64 operation. */
static void
snapshot_put64(
	uint8_t *p,
	uint64_t v)
{
	snapshot_put32(p, (uint32_t)v);
	snapshot_put32(p + 4, (uint32_t)(v >> 32));
}

/* Supports the snapshot get32 operation. */
static uint32_t
snapshot_get32(
	const uint8_t *p)
{
	/* A snapshot record is stored little-endian, whatever the volume is. */
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
		(uint32_t)p[3] << 24;
}

/* Supports the snapshot get64 operation. */
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

/* Supports the digest operation. */
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

/* Supports the hash sector operation. */
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

/* Supports the map find operation. */
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

/* Supports the map clear operation. */
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

/* Supports the write control operation. */
static int
write_control(
	struct ufs_snapshot *snapshot,
	unsigned active,
	uint32_t next)
{
	uint8_t sector[SECTOR_SIZE];
	int error;

	/* Builds the control sector the snapshot is described by. */
	memset(sector, 0, sizeof(sector));

	/* The signature and version a reader identifies the sector by. */
	memcpy(sector, "ZSN1", 4);
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

/* Supports the power2 operation. */
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
