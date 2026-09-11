/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * FAT filesystem
 *
 * Supports FAT12,FAT16, and FAT32.
 */

#include "kern/fat.h"
#include "kern/block-identity.h"
#include "kern/kmem.h"
#include <kern/io-stats.h>
#include "kern/namecache.h"
#include "kern/namei.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/statvfs.h>

/*
 * Sector, BPB, cache, and shared FAT primitives.
 */
#define FAT_PROGRESS_INTERVAL		(64U * 1024U)
#define FAT_MOUNT_MAX			MOUNT_MAX
#define FAT_INODE_MAX			256U
#define FAT_FILE_MAX			96U
#define FAT_ATTRIBUTE_READ_ONLY		0x01U
#define FAT_ATTRIBUTE_DIRECTORY		0x10U
#define FAT_INODE_ORPHANED		0x01U
#define FAT_EPOCH_1980			315532800L
#define FAT_METADATA_MAX		32U
#define FAT_CLEAN_SLOTS			4U
#define FAT_LFN_MAX_UNITS		255U
#define FAT_LFN_MAX_ENTRIES		((FAT_LFN_MAX_UNITS + 12U) / 13U)
#define FAT_BATCH_SECTORS		8U
#define FAT_BATCH_ENTRIES		32U
#define FAT16_RESERVED_CLUSTER		0xfff0U
#define FAT16_END_OF_CHAIN		0xffffU
#define FAT12_RESERVED_CLUSTER		0xff0U
#define FAT12_END_OF_CHAIN		0xfffU
#define FAT32_RESERVED_CLUSTER		0x0ffffff0U
#define FAT32_END_OF_CHAIN		0x0fffffffU
#define FAT16_DIRECTORY_ENTRY_SIZE	32U
#define FAT16_ENTRIES_PER_SECTOR	(512U / FAT16_DIRECTORY_ENTRY_SIZE)
#define FAT_MUTATION			__attribute__((section(".hightext")))

/*
 * One directory entry, in the form the rest of the driver works with.
 *
 * The name is already decoded from whichever of the short and long forms the
 * volume stored, so nothing above this file has to know that FAT keeps two.
 */
struct fat_dir_entry {
	char name[KERN_PATH_MAX];
	uint64_t size;
	uint8_t attributes;
};

struct fat_directory {
	/* Cluster zero denotes FAT16's fixed root-directory table. */
	uint32_t first_cluster;
};

enum fat_name_match {
	FAT_NAME_EXACT,
	FAT_NAME_CASEFOLD,
};

struct fat_component {
	char text[KERN_PATH_MAX];
	char sfn[11];
};

/*
 * The mode and owner this mount presents for one path.
 *
 * FAT stores neither, so a mount that wants them keeps them here instead.
 * An entry lives for as long as the mount does and is never written to disk.
 */
struct fat_metadata {
	char path[KERN_PATH_MAX];
	mode_t mode;
	uid_t uid;
	gid_t gid;
};

/*
 * Every presented mode and owner of one mount.
 *
 * The table is a fixed array rather than a list, because it is filled during
 * mount and is only ever searched afterwards.
 */
struct fat_metadata_table {
	struct fat_metadata entries[FAT_METADATA_MAX];
	unsigned count;
};

/*
 * A sector as it stood before a mutation touched it.
 *
 * A mutation that fails part way through writes these back, so that a caller
 * never sees a directory half updated.  A slot is valid only between the
 * start and the end of one mutation.
 */
struct fat_clean_sector {
	uint8_t bytes[512];
	uint32_t lba;
	unsigned valid;
};

/*
 * A position inside a cluster chain.
 *
 * Walking a chain costs one table read per link, so a file remembers where
 * it last stopped and resumes from there when the next request continues
 * where the previous one ended.
 */
struct fat_chain_cursor {
	uint32_t index;
	uint32_t cluster;
};

/*
 * One mounted volume.
 *
 * It holds the geometry read from the boot sector, the one-sector cache every
 * table and directory access goes through, the saved sectors an unfinished
 * mutation may have to restore, and the generation that tells a file's cached
 * chain cursor it has gone stale.  A state lives from mount to unmount and is
 * taken from the static pool below.
 */
struct fat_mount_state {
	const struct io_context *write_context;
	struct mount *owner;
	struct disk *disk;
	struct mutex lock;
	struct fat_metadata_table *metadata;
	uint32_t fat_start;
	uint32_t root_start;
	uint32_t data_start;
	uint32_t total_sectors;
	uint32_t cluster_count;
	uint32_t fat_sectors;
	uint32_t allocation_hint;
	uint32_t root_cluster;
	uint32_t pending_orphans[FAT_INODE_MAX];
	uint16_t pending_orphan_count;
	uint16_t bytes_per_sector;
	uint16_t root_entries;
	uint16_t sectors_per_cluster;
	uint16_t fsinfo_sector;
	uint8_t sector_scale;
	uint8_t number_of_fats;
	uint8_t type;
	uint8_t fat16_layout;
	uint8_t fat32_layout;
	struct fat_clean_sector clean_sectors[FAT_CLEAN_SLOTS];
	unsigned clean_rotor;
	uint64_t chain_generation;
	uint8_t sector_cache[512];
	uint32_t sector_cache_lba;
	uint8_t sector_cache_valid;
	uint8_t sector_cache_dirty;
	uint8_t read_only;
	uint8_t direct_io;
	uint8_t used;
};

/*
 * One open file.
 *
 * It caches the position of the file's directory entry, so a size change does
 * not have to search for it again, and the chain cursor that makes sequential
 * access cost one table read per cluster rather than per request.  A state
 * lives from open to close and is taken from the static pool below.
 */
struct fat_file_state {
	const struct fat_loop_extent *loop_map;
	unsigned loop_map_count;
	struct fat_mount_state *mount;
	struct inode *owner;
	uint64_t size;
	uint32_t first_cluster;
	uint32_t directory_lba;
	uint16_t directory_offset;
	struct fat_chain_cursor chain_cursor;
	uint64_t cursor_generation;
	uint64_t cursor_offset;
	uint32_t cursor_first;
	uint32_t cursor_last;
	unsigned cursor_valid;
	uint8_t directory_dirty;
	uint8_t pending_close;
	uint8_t used;
};

/*
 * The FAT-specific half of an inode.
 *
 * The generic inode is embedded first, so a pointer to either one converts to
 * the other.  The rest records where the file starts on the volume and where
 * its directory entry lives.
 */
struct fat_inode_info {
	struct inode fi_inode;
	uint32_t fi_first_cluster;
	uint32_t fi_dirent_lba;
	uint16_t fi_dirent_offset;
	uint8_t fi_attributes;
	uint8_t fi_flags;
};

/*
 * A long name being assembled from its entries.
 *
 * VFAT stores a long name backwards, in up to twenty entries that precede the
 * short-name entry.  This accumulates them as the directory is read, and is
 * reset whenever the run turns out to be broken or the checksum does not
 * match the short name that follows it.
 */
struct fat_lfn_state {
	uint16_t units[FAT_LFN_MAX_UNITS + 1U];
	uint16_t unit_limit;
	uint8_t expected;
	uint8_t checksum;
	uint8_t active;
};

/*
 * One entry of the static inode pool.
 *
 * The path is kept beside the inode so that a lookup can find an inode that
 * already exists without walking the directories again.
 */
struct fat_inode_slot {
	struct fat_inode_info info;
	char path[KERN_PATH_MAX];
	uint8_t used;
};

/*
 * Where a rename left the entry it created.
 *
 * The caller needs it to repoint the inode at its new directory entry once
 * the rename has committed.
 */
struct fat_rename_result {
	uint32_t lba;
	uint16_t offset;
	uint8_t attributes;
};

struct fat_entry_change {
	uint32_t cluster;
	uint32_t value;
};

struct fat_sector_change {
	uint32_t lba;
	uint8_t old_bytes[512];
	uint8_t new_bytes[512];
};

struct fat_casefold_range {
	uint32_t start;
	/*
	 * Bit 31 marks a stride-two range; Unicode scalar values never use it.
	 */
	uint32_t encoded_end;

	int32_t delta;
};

/*
 * How a chain walk asks for the next cluster and reports its progress.
 *
 * The walk is shared between the table widths and between reading and
 * writing, so each caller supplies the step and the progress hook it wants.
 */
typedef int (*fat_next_cluster_fn)(struct fat_mount_state *, uint32_t,
	uint32_t *);
typedef void (*fat_read_progress_fn)(void *, uint32_t);

/*
 * Where the thirteen name units of a long-name entry sit.
 *
 * VFAT splits the units across three runs inside the entry, around the fields
 * it inherited from the short-name layout, so the offsets are listed rather
 * than computed.
 */
static const uint8_t lfn_offsets[13] = {
	1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30,
};

/*
 * Guards the four static pools below.
 *
 * It is a spinlock rather than a mutex because a slot is taken and released
 * on paths that must not sleep.  It is only ever held across the search for
 * a free slot, never across an access to the volume.
 */
static struct spinlock fat_pool_lock = {
	{0}, LOCK_RANK_INODE, "FAT object pools", 0, 0
};

/*
 * The mount, metadata, inode and open-file pools.
 *
 * They are arrays rather than allocations because a file system is brought up
 * before the general allocator, and they are placed in .vfs_bss because that
 * section is mapped by the early boot path and is not cleared again once the
 * kernel takes over.  A slot is claimed under fat_pool_lock and lives until
 * whatever claimed it is retired.
 */
static struct fat_mount_state fat_mounts[FAT_MOUNT_MAX] __attribute__((section(".vfs_bss")));
static struct fat_metadata_table fat_metadata_tables[FAT_MOUNT_MAX] __attribute__((section(".vfs_bss")));
static struct fat_inode_slot fat_inodes[FAT_INODE_MAX] __attribute__((section(".vfs_bss")));
static struct fat_file_state fat_files[FAT_FILE_MAX] __attribute__((section(".vfs_bss")));

static struct fat_inode_info *fat_inode(struct inode *inode);
static int fat_engine_write_sector_result(struct fat_mount_state *filesystem,
	uint32_t lba, uint8_t **sector);
static int parse_bpb(struct fat_mount_state *fat);
static FAT_MUTATION int fat_raw_mkdir(struct fat_mount_state *filesystem,
	const char *path, uint32_t *created_cluster);
static FAT_MUTATION int fat_raw_remove(struct fat_mount_state *filesystem,
	const char *path, int directory);
static FAT_MUTATION int fat_raw_unlink(struct fat_mount_state *filesystem,
	const char *path);
static FAT_MUTATION int fat_raw_rmdir(struct fat_mount_state *filesystem,
	const char *path);
static int fat_raw_read(struct fat_file_state *file, uint64_t offset,
	void *buffer, uint32_t length, fat_read_progress_fn progress,
	void *progress_context);
static int fat_engine_file_extents(struct fat_file_state *file,
	fat_extent_cb callback, void *context);
static int fat_engine_discard_chain_result(struct fat_mount_state *filesystem,
	uint32_t first_cluster);
static int fat12_mount(struct fat_mount_state *filesystem);
static int fat32_mount(struct fat_mount_state *filesystem);
static struct fat_mount_state *fat_mount_state(struct mount *mountp);
static void fat_metadata_load(struct fat_mount_state *state);
static const struct fat_metadata *fat_metadata_find(const struct fat_mount_state *state, const char *path);
static void fat_metadata_apply(struct mount *mountp, const char *path,
	struct inode *inode);
static struct fat_inode_slot *fat_slot(struct inode *inode);
static int fat_creation_representation(const struct fat_mount_state *state,
	const char *path, mode_t *mode, uid_t *uid, gid_t *gid);
static int fat_creation_representable(const struct fat_mount_state *state,
	const char *path, const struct inode_creation_request *request,
	enum inode_type type);
static ino_t fat_ino(uint32_t lba, uint16_t offset);
static int fat_month_days(int year, int month);
static int fat_stat_path(struct mount *mountp, const char *path,
	struct inode **result);
static int fat_stat_path_casefold(struct mount *mountp, const char *path,
	struct inode **result);
static int fat_lookup_casefold_unlocked(struct inode *directory,
	const struct componentname *name, struct inode **result);
static struct fat_file_state *fat_file_get(struct file *file);
static ssize_t fat_pread_file_unlocked(struct file *file, void *buffer,
	size_t length, off_t offset);
static int fat_create_unlocked(struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request, struct inode **result);
static FAT_MUTATION void fat_release_orphan(struct inode *inode);
static FAT_MUTATION int fat_mkdir_unlocked(struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request, struct inode **result);
static FAT_MUTATION int fat_remove_inode_unlocked(struct inode *directory,
	const struct componentname *name, int remove_directory,
	struct inode **orphaned);
static void fat_reclaim_unlocked(struct inode *inode);
static int fat_probe_volume(struct disk *disk, int direct_io,
	enum bootfat_type *type);
static char fat_hex_digit(unsigned value);
static void fat_hex32(char output[9], uint32_t value);
static uint16_t fat_engine_get16(const uint8_t *bytes);
static uint32_t fat_engine_get32(const uint8_t *bytes);
static int fat_sector_read(struct fat_mount_state *state, uint32_t lba,
	void *buffer);
static int fat_sector_write(struct fat_mount_state *state, uint32_t lba,
	const void *buffer);
static int fat_engine_flush(struct fat_mount_state *filesystem);
static void fat_engine_invalidate(struct fat_mount_state *filesystem);
static int fat_engine_read_sector_result(struct fat_mount_state *filesystem,
	uint32_t lba, const uint8_t **sector);
static int fat_engine_mark_sector_dirty(struct fat_mount_state *filesystem);
static int fat_engine_cluster_lba(struct fat_mount_state *filesystem,
	uint32_t cluster, uint32_t sector_in_cluster, uint32_t *lba);
static int fat_engine_mount(struct fat_mount_state *filesystem,
	enum bootfat_type required_type);
static int fat_sfn_encode(const char *path, char output[11]);
static void fat_sfn_decode_lower(const uint8_t raw[32],
	struct fat_dir_entry *entry);
static void fat_lfn_reset(struct fat_lfn_state *state);
static uint8_t fat_lfn_checksum(const uint8_t sfn[11]);
static int fat_lfn_feed(struct fat_lfn_state *state, const uint8_t raw[32]);
static int append_utf8(char *output, size_t capacity, size_t *used,
	uint32_t scalar);
static int fat_lfn_finish(struct fat_lfn_state *state, const uint8_t sfn[32],
	char *output, size_t capacity);
static void fat_sfn_decode_preserve(const uint8_t raw[32], char *output,
	size_t capacity);
static uint32_t fold_scalar(uint32_t scalar);
static int decode_utf8(const uint8_t **cursor, uint32_t *scalar);
static int fat_utf8_to_utf16(const char *name,
	uint16_t units[FAT_LFN_MAX_UNITS], unsigned *unit_count);
static void fat_lfn_build_entry(uint8_t raw[32], const uint16_t *units,
	unsigned unit_count, unsigned ordinal, uint8_t checksum);
static int sfn_character(uint8_t c);
static int fat_sfn_make_alias(const char *name, unsigned serial,
	uint8_t sfn[11]);
static int fat_utf8_casefold_equal(const char *left, const char *right);
static void text_copy(char *destination, const char *source, size_t capacity);
static void copy_bytes(void *destination, const void *source, uint32_t length);
static void clear_bytes(void *destination, uint32_t length);
static void put16(uint8_t *bytes, uint16_t value);
static void put32(uint8_t *bytes, uint32_t value);
static int fat16_mount(struct fat_mount_state *filesystem);
static int fat_raw_valid_cluster(const struct fat_mount_state *fat,
	uint32_t cluster);
static int fat_raw_is_end(const struct fat_mount_state *fat, uint32_t cluster);
static uint32_t fat_raw_reserved_limit(const struct fat_mount_state *fat);
static uint32_t fat_raw_end_of_chain(const struct fat_mount_state *fat);
static uint32_t fat_raw_entry_offset(const struct fat_mount_state *fat,
	uint32_t cluster);
static int fat_raw_next_cluster(struct fat_mount_state *filesystem,
	uint32_t cluster, uint32_t *next_cluster);
static int fat_engine_count_free_clusters(struct fat_mount_state *filesystem,
	uint32_t *free_clusters);
static int fat_raw_set_entry_byte(struct fat_mount_state *filesystem,
	uint32_t copy_start, uint32_t offset, uint8_t keep_mask,
	uint8_t merge_value);
static int fat_raw_set_cluster_copy(struct fat_mount_state *filesystem,
	uint32_t cluster, uint32_t value, unsigned copy);
static int fat_raw_set_cluster_immediate(struct fat_mount_state *filesystem,
	uint32_t cluster, uint32_t value);
static uint32_t fat_raw_dir_cluster(const struct fat_mount_state *fat,
	const uint8_t raw[32]);
static void fat_raw_put_dir_cluster(const struct fat_mount_state *fat,
	uint8_t raw[32], uint32_t cluster);
static uint32_t fat_raw_root_cluster(const struct fat_mount_state *fat);
static int fat_raw_validate_chain_at(struct fat_mount_state *filesystem,
	uint32_t first_cluster, uint32_t wanted_index,
	struct fat_chain_cursor *cursor, uint32_t *last_cluster);
static int fat_raw_validate_chain_count(struct fat_mount_state *filesystem,
	uint32_t first_cluster, uint32_t wanted_index,
	struct fat_chain_cursor *cursor, uint32_t *last_cluster,
	uint32_t *allocated_clusters);
static int fat_raw_growth_capacity(struct fat_mount_state *filesystem,
	uint32_t allocated_clusters, uint64_t size);
static int fat_raw_validate_chain(struct fat_mount_state *filesystem,
	uint32_t first_cluster);
static int fat_raw_free_chain(struct fat_mount_state *filesystem,
	uint32_t first_cluster);
static int fat_drain_pending_orphans(struct fat_mount_state *filesystem);
static int fat_defer_orphan(struct fat_mount_state *filesystem,
	uint32_t first_cluster);
static int fat_raw_find_free_cluster(struct fat_mount_state *filesystem,
	uint32_t *free_cluster);
static int fat_raw_zero_cluster(struct fat_mount_state *filesystem,
	uint32_t cluster);
static int fat_raw_allocate_cluster(struct fat_mount_state *filesystem,
	uint32_t *cluster);
static int fat_raw_directory_entry(struct fat_mount_state *filesystem,
	const struct fat_directory *directory, uint32_t index,
	uint32_t *entry_lba, uint16_t *entry_offset, const uint8_t **raw);
static int fat_raw_find_entry(struct fat_mount_state *filesystem,
	const struct fat_directory *directory,
	const struct fat_component *component, enum fat_name_match match,
	uint32_t *entry_lba, uint16_t *entry_offset, uint32_t *free_lba,
	uint16_t *free_offset, char found_name[KERN_PATH_MAX]);
static int fat_raw_resolve_parent(struct fat_mount_state *filesystem,
	const char *path, struct fat_directory *parent,
	struct fat_component *component);
static int fat_raw_resolve_entry(struct fat_mount_state *filesystem,
	const char *path, uint32_t *lba, uint16_t *offset, const uint8_t **raw,
	enum fat_name_match match, char found_name[KERN_PATH_MAX]);
static int fat_raw_populate_file(struct fat_file_state *file, uint32_t lba,
	uint16_t offset, const uint8_t raw[32]);
static void fat_file_bind(struct fat_file_state *file,
	struct fat_mount_state *mount);
static int fat_raw_open(struct fat_mount_state *filesystem, const char *path,
	struct fat_file_state *file);
static int fat_raw_flush_file(struct fat_file_state *file);
static int fat_raw_advance_cluster(struct fat_file_state *file,
	uint32_t cluster, int allocate, uint32_t *next);
static int fat_raw_cluster_at(struct fat_file_state *file,
	uint32_t cluster_index, int allocate, uint32_t *found_cluster,
	struct fat_chain_cursor *cursor);
static int fat_raw_write_bytes(struct fat_file_state *file, uint32_t offset,
	const uint8_t *input, uint32_t length, int zero,
	struct fat_chain_cursor *cursor);
static int fat_raw_rollback_growth(struct fat_file_state *file,
	uint32_t old_first, uint32_t old_last, uint64_t old_size,
	uint8_t old_directory_dirty);
static int fat_raw_restore_directory(struct fat_file_state *file,
	uint32_t first_cluster, uint64_t size, uint8_t directory_dirty);
static int fat_raw_write(struct fat_file_state *file, uint64_t offset,
	const void *buffer, uint32_t length);
static int fat_raw_truncate(struct fat_file_state *file, uint64_t size);
static int fat_raw_sfn_in_use(struct fat_mount_state *filesystem,
	const struct fat_directory *directory, const uint8_t sfn[11]);
static int fat_raw_extend_directory(struct fat_mount_state *filesystem,
	const struct fat_directory *directory);
static int fat_raw_find_free_run(struct fat_mount_state *filesystem,
	const struct fat_directory *directory, unsigned needed,
	uint32_t *first_index);
static FAT_MUTATION int fat_raw_restore_directory_entry(struct fat_mount_state *filesystem, uint32_t lba, uint16_t offset, const uint8_t entry[32]);
static FAT_MUTATION int fat32_create_entry(struct fat_mount_state *filesystem,
	const struct fat_directory *parent,
	const struct fat_component *component, uint8_t attributes,
	uint32_t first_cluster, uint32_t size, uint32_t *entry_lba,
	uint16_t *entry_offset);
static FAT_MUTATION int fat_raw_insert_entry(struct fat_mount_state *filesystem,
	const struct fat_directory *parent,
	const struct fat_component *component, uint8_t attributes,
	uint32_t first_cluster, uint32_t size, uint32_t *entry_lba,
	uint16_t *entry_offset);
static FAT_MUTATION int fat_raw_delete_location(struct fat_mount_state *,
	const struct fat_directory *, uint32_t, uint16_t);
static FAT_MUTATION int fat_raw_create(struct fat_mount_state *filesystem,
	const char *path, struct fat_file_state *file);
static FAT_MUTATION int fat_raw_directory_empty(struct fat_mount_state *filesystem, uint32_t first_cluster);
static FAT_MUTATION int fat_raw_initialize_directory(struct fat_mount_state *filesystem, uint32_t cluster, uint32_t parent_cluster);
static FAT_MUTATION int fat_raw_rename(struct fat_mount_state *filesystem,
	const char *old_path, const char *new_path,
	uint32_t authoritative_cluster, uint32_t authoritative_size,
	struct fat_rename_result *renamed);
static int fat_engine_stat_location(struct fat_mount_state *filesystem,
	const char *path, struct fat_dir_entry *entry, uint32_t *lba,
	uint16_t *offset, uint32_t *first_cluster, uint8_t *attributes);
static int fat_engine_stat_location_casefold(struct fat_mount_state *filesystem,
	const char *path, struct fat_dir_entry *entry, uint32_t *lba,
	uint16_t *offset, uint32_t *first_cluster, uint8_t *attributes);
static const char *fat_path(struct inode *inode);
static struct inode *fat_alloc_inode(struct mount *mountp);
static int join_path(const char *parent, const struct componentname *name,
	char output[KERN_PATH_MAX]);
static int fat_creation_collision(struct fat_mount_state *state,
	const char *path);
static int fat_created_inode_matches(const struct fat_mount_state *state,
	const char *path, const struct inode *inode);
static time_t fat_decode_time(uint16_t date, uint16_t time);
static FAT_MUTATION int fat_encode_time(time_t seconds, uint16_t *date,
	uint16_t *time);
static void fat_load_inode_times(struct mount *mountp, struct inode *inode,
	uint32_t lba, uint16_t offset);
static int fat_make_inode(struct mount *mountp, const char *path,
	const struct fat_dir_entry *entry, uint32_t lba, uint16_t offset,
	uint32_t first_cluster, uint8_t attributes, struct inode **result);
static int fat_lookup_unlocked(struct inode *directory,
	const struct componentname *name, struct inode **result);
static FAT_MUTATION void fat_put16(uint8_t *bytes, uint16_t value);
static FAT_MUTATION int fat_setattr_unlocked(struct inode *inode,
	const struct stat *status, unsigned mask);
static void fat_sync_inode_state(struct inode *inode,
	const struct fat_file_state *file);
static ssize_t fat_pwrite_file_unlocked(struct file *file, const void *buffer,
	size_t length, off_t offset);
static int fat_readdir_unlocked(struct file *file, struct dirent *entry,
	int *eof);
static void fat_copy_label(char *output, size_t capacity, const uint8_t *input,
	size_t length);
static int fat_identify(struct disk *disk, struct block_identity *identity);
static int fat_mount_impl(struct mount *mountp);
static int fat_sync_mount(struct mount *mountp);
static void fat_unmount_impl(struct mount *mountp);
static int fat_statvfs(struct mount *mountp, struct statvfs *result);
static int fat_sfn_equal(const uint8_t entry[32], const char name[11]);
static int valid_cluster(uint32_t cluster, uint32_t end_of_chain);
static int fat_engine_read_chain(struct fat_file_state *file, uint64_t offset,
	void *buffer, uint32_t length, fat_read_progress_fn progress,
	void *progress_context, fat_next_cluster_fn next_cluster,
	uint32_t end_of_chain);
static int text_equal(const char *left, const char *right);
static FAT_MUTATION int fat_raw_update_dotdot(struct fat_mount_state *filesystem, uint32_t directory_cluster, uint32_t parent_cluster);
static FAT_MUTATION int fat_raw_restore_entry_payload(struct fat_mount_state *filesystem, uint32_t lba, uint16_t offset, const uint8_t raw[32]);
static FAT_MUTATION void fat_raw_rename_rollback_destination(struct fat_mount_state *filesystem, const struct fat_directory *parent, uint32_t lba, uint16_t offset, int replacing, const uint8_t target[32]);
static int fat_raw_canonical_basename(struct fat_mount_state *filesystem,
	const char *path, char basename[KERN_PATH_MAX]);
static int fat_raw_readdir(struct fat_mount_state *filesystem, const char *path,
	unsigned wanted, struct fat_dir_entry *entry);
static int fat_stat_location_mode(struct fat_mount_state *filesystem,
	const char *path, struct fat_dir_entry *entry, uint32_t *lba,
	uint16_t *offset, uint32_t *first_cluster, uint8_t *attributes,
	enum fat_name_match match);
static int fat_metadata_number(const char *text, unsigned base,
	uint32_t *value);
static void fat_free_inode(struct inode *inode);
static int fat_leap_year(int year);
static ssize_t fat_loop_transfer(struct file *file,
	struct fat_file_state *state, void *buffer, size_t length, off_t offset,
	int writing);
static FAT_MUTATION int fat_path_descendant(const char *parent,
	const char *path);
static FAT_MUTATION void fat_repath_descendants(struct mount *mountp,
	const char *old_path, const char *new_path);
static FAT_MUTATION int fat_repath_descendants_possible(struct mount *mountp,
	const char *old_path, const char *new_path);
static FAT_MUTATION int fat_rename_unlocked(struct inode *old_directory,
	const struct componentname *old_name, struct inode *new_directory,
	const struct componentname *new_name, unsigned flags,
	struct inode **orphaned);
static int fat_probe(struct disk *disk);
static void fat_chain_invalidate(struct fat_mount_state *state);
static int fat_raw_get_cluster_copy(struct fat_mount_state *filesystem,
	uint32_t cluster, unsigned copy, uint32_t *value);
static int fat_raw_extend_cluster(struct fat_mount_state *filesystem,
	uint32_t tail, uint32_t *added);
static int fat_raw_allocate_run(struct fat_mount_state *filesystem,
	uint32_t tail, uint32_t wanted, uint32_t *first);
static int fat_writeback_range(struct file *file, off_t offset, size_t length);
static int fat_file_validate_at(struct fat_file_state *file, uint64_t offset,
	struct fat_chain_cursor *cursor, uint32_t *last);
static void fat_file_save_cursor(struct fat_file_state *file,
	const struct fat_chain_cursor *cursor, uint64_t end,
	uint64_t generation, uint32_t last);
static void fat_engine_copy_bytes(void *destination, const void *source,
	uint32_t length);
static uint16_t get16(const uint8_t *p);
static int fat_lookup(struct inode *, const struct componentname *,
	struct inode **);
static int fat_lookup_casefold(struct inode *, const struct componentname *,
	struct inode **);
static int fat_create(struct inode *, const struct componentname *,
	const struct inode_creation_request *, struct inode **);
static int fat_mkdir(struct inode *, const struct componentname *,
	const struct inode_creation_request *, struct inode **);
static int fat_unlink(struct inode *, const struct componentname *);
static int fat_rmdir(struct inode *, const struct componentname *);
static int fat_rename(struct inode *, const struct componentname *,
	struct inode *, const struct componentname *, unsigned);
static int fat_truncate(struct inode *, off_t);
static int fat_getattr(struct inode *, struct stat *);
static int fat_setattr(struct inode *, const struct stat *, unsigned);
static void fat_reclaim(struct inode *);
static void fat_orphan(struct inode *);
static ssize_t fat_read_file(struct file *, void *, size_t);
static ssize_t fat_write_file(struct file *, const void *, size_t);
static ssize_t fat_pread_file(struct file *, void *, size_t, off_t);
static ssize_t fat_pwrite_file(struct file *, const void *, size_t, off_t);
static ssize_t fat_pwrite_context(struct file *file, const void *buffer,
	size_t length, off_t offset, unsigned flags,
	const struct ucred *credential, const struct io_context *context);
static int fat_readdir(struct file *, struct dirent *, int *);
static int fat_open_file(struct file *);
static int fat_fsync(struct file *);
static int fat_fsync_directory(struct file *);
static int fat_close_file(struct file *);
static int fat_flush_pending_closes(struct fat_mount_state *);
static void set_inode_ops(struct inode *inode);
static int fat_sector_images_commit(struct fat_mount_state *filesystem,
	struct fat_sector_change *slots, unsigned used, uint32_t publish_lba);
static int fat_directory_transaction(struct fat_mount_state *filesystem,
	const uint32_t *lbas, const uint16_t *offsets,
	const uint8_t entries[][32], unsigned count, int publish_last);
static int fat_batch_add_lba(uint32_t *lbas, unsigned *used, uint32_t lba);
static uint8_t *fat_batch_bytes(struct fat_sector_change *slots, unsigned used,
	uint32_t lba);
static int fat_table_transaction(struct fat_mount_state *filesystem,
	const struct fat_entry_change *changes, unsigned count, int *admitted);
static int fat_raw_set_cluster(struct fat_mount_state *filesystem,
	uint32_t cluster, uint32_t value);
static int fat_link_initialized_cluster(struct fat_mount_state *filesystem,
	uint32_t tail, uint32_t added);

/*
 * Generated by scripts/generate-unicode-casefold.py from
 * Unicode CaseFolding-17.0.0.txt; statuses C and S only.
 * Do not edit.
 */
static const struct fat_casefold_range fat_casefold_ranges[] = {
	{0x000041U, 0x0000005aU, 32},	  {0x0000b5U, 0x000000b5U, 775},
	{0x0000c0U, 0x000000d6U, 32},	  {0x0000d8U, 0x000000deU, 32},
	{0x000100U, 0x8000012eU, 1},	  {0x000132U, 0x80000136U, 1},
	{0x000139U, 0x80000147U, 1},	  {0x00014aU, 0x80000176U, 1},
	{0x000178U, 0x00000178U, -121},	  {0x000179U, 0x8000017dU, 1},
	{0x00017fU, 0x0000017fU, -268},	  {0x000181U, 0x00000181U, 210},
	{0x000182U, 0x80000184U, 1},	  {0x000186U, 0x00000186U, 206},
	{0x000187U, 0x00000187U, 1},	  {0x000189U, 0x0000018aU, 205},
	{0x00018bU, 0x0000018bU, 1},	  {0x00018eU, 0x0000018eU, 79},
	{0x00018fU, 0x0000018fU, 202},	  {0x000190U, 0x00000190U, 203},
	{0x000191U, 0x00000191U, 1},	  {0x000193U, 0x00000193U, 205},
	{0x000194U, 0x00000194U, 207},	  {0x000196U, 0x00000196U, 211},
	{0x000197U, 0x00000197U, 209},	  {0x000198U, 0x00000198U, 1},
	{0x00019cU, 0x0000019cU, 211},	  {0x00019dU, 0x0000019dU, 213},
	{0x00019fU, 0x0000019fU, 214},	  {0x0001a0U, 0x800001a4U, 1},
	{0x0001a6U, 0x000001a6U, 218},	  {0x0001a7U, 0x000001a7U, 1},
	{0x0001a9U, 0x000001a9U, 218},	  {0x0001acU, 0x000001acU, 1},
	{0x0001aeU, 0x000001aeU, 218},	  {0x0001afU, 0x000001afU, 1},
	{0x0001b1U, 0x000001b2U, 217},	  {0x0001b3U, 0x800001b5U, 1},
	{0x0001b7U, 0x000001b7U, 219},	  {0x0001b8U, 0x000001b8U, 1},
	{0x0001bcU, 0x000001bcU, 1},	  {0x0001c4U, 0x000001c4U, 2},
	{0x0001c5U, 0x000001c5U, 1},	  {0x0001c7U, 0x000001c7U, 2},
	{0x0001c8U, 0x000001c8U, 1},	  {0x0001caU, 0x000001caU, 2},
	{0x0001cbU, 0x800001dbU, 1},	  {0x0001deU, 0x800001eeU, 1},
	{0x0001f1U, 0x000001f1U, 2},	  {0x0001f2U, 0x800001f4U, 1},
	{0x0001f6U, 0x000001f6U, -97},	  {0x0001f7U, 0x000001f7U, -56},
	{0x0001f8U, 0x8000021eU, 1},	  {0x000220U, 0x00000220U, -130},
	{0x000222U, 0x80000232U, 1},	  {0x00023aU, 0x0000023aU, 10795},
	{0x00023bU, 0x0000023bU, 1},	  {0x00023dU, 0x0000023dU, -163},
	{0x00023eU, 0x0000023eU, 10792},  {0x000241U, 0x00000241U, 1},
	{0x000243U, 0x00000243U, -195},	  {0x000244U, 0x00000244U, 69},
	{0x000245U, 0x00000245U, 71},	  {0x000246U, 0x8000024eU, 1},
	{0x000345U, 0x00000345U, 116},	  {0x000370U, 0x80000372U, 1},
	{0x000376U, 0x00000376U, 1},	  {0x00037fU, 0x0000037fU, 116},
	{0x000386U, 0x00000386U, 38},	  {0x000388U, 0x0000038aU, 37},
	{0x00038cU, 0x0000038cU, 64},	  {0x00038eU, 0x0000038fU, 63},
	{0x000391U, 0x000003a1U, 32},	  {0x0003a3U, 0x000003abU, 32},
	{0x0003c2U, 0x000003c2U, 1},	  {0x0003cfU, 0x000003cfU, 8},
	{0x0003d0U, 0x000003d0U, -30},	  {0x0003d1U, 0x000003d1U, -25},
	{0x0003d5U, 0x000003d5U, -15},	  {0x0003d6U, 0x000003d6U, -22},
	{0x0003d8U, 0x800003eeU, 1},	  {0x0003f0U, 0x000003f0U, -54},
	{0x0003f1U, 0x000003f1U, -48},	  {0x0003f4U, 0x000003f4U, -60},
	{0x0003f5U, 0x000003f5U, -64},	  {0x0003f7U, 0x000003f7U, 1},
	{0x0003f9U, 0x000003f9U, -7},	  {0x0003faU, 0x000003faU, 1},
	{0x0003fdU, 0x000003ffU, -130},	  {0x000400U, 0x0000040fU, 80},
	{0x000410U, 0x0000042fU, 32},	  {0x000460U, 0x80000480U, 1},
	{0x00048aU, 0x800004beU, 1},	  {0x0004c0U, 0x000004c0U, 15},
	{0x0004c1U, 0x800004cdU, 1},	  {0x0004d0U, 0x8000052eU, 1},
	{0x000531U, 0x00000556U, 48},	  {0x0010a0U, 0x000010c5U, 7264},
	{0x0010c7U, 0x000010c7U, 7264},	  {0x0010cdU, 0x000010cdU, 7264},
	{0x0013f8U, 0x000013fdU, -8},	  {0x001c80U, 0x00001c80U, -6222},
	{0x001c81U, 0x00001c81U, -6221},  {0x001c82U, 0x00001c82U, -6212},
	{0x001c83U, 0x00001c84U, -6210},  {0x001c85U, 0x00001c85U, -6211},
	{0x001c86U, 0x00001c86U, -6204},  {0x001c87U, 0x00001c87U, -6180},
	{0x001c88U, 0x00001c88U, 35267},  {0x001c89U, 0x00001c89U, 1},
	{0x001c90U, 0x00001cbaU, -3008},  {0x001cbdU, 0x00001cbfU, -3008},
	{0x001e00U, 0x80001e94U, 1},	  {0x001e9bU, 0x00001e9bU, -58},
	{0x001e9eU, 0x00001e9eU, -7615},  {0x001ea0U, 0x80001efeU, 1},
	{0x001f08U, 0x00001f0fU, -8},	  {0x001f18U, 0x00001f1dU, -8},
	{0x001f28U, 0x00001f2fU, -8},	  {0x001f38U, 0x00001f3fU, -8},
	{0x001f48U, 0x00001f4dU, -8},	  {0x001f59U, 0x80001f5fU, -8},
	{0x001f68U, 0x00001f6fU, -8},	  {0x001f88U, 0x00001f8fU, -8},
	{0x001f98U, 0x00001f9fU, -8},	  {0x001fa8U, 0x00001fafU, -8},
	{0x001fb8U, 0x00001fb9U, -8},	  {0x001fbaU, 0x00001fbbU, -74},
	{0x001fbcU, 0x00001fbcU, -9},	  {0x001fbeU, 0x00001fbeU, -7173},
	{0x001fc8U, 0x00001fcbU, -86},	  {0x001fccU, 0x00001fccU, -9},
	{0x001fd3U, 0x00001fd3U, -7235},  {0x001fd8U, 0x00001fd9U, -8},
	{0x001fdaU, 0x00001fdbU, -100},	  {0x001fe3U, 0x00001fe3U, -7219},
	{0x001fe8U, 0x00001fe9U, -8},	  {0x001feaU, 0x00001febU, -112},
	{0x001fecU, 0x00001fecU, -7},	  {0x001ff8U, 0x00001ff9U, -128},
	{0x001ffaU, 0x00001ffbU, -126},	  {0x001ffcU, 0x00001ffcU, -9},
	{0x002126U, 0x00002126U, -7517},  {0x00212aU, 0x0000212aU, -8383},
	{0x00212bU, 0x0000212bU, -8262},  {0x002132U, 0x00002132U, 28},
	{0x002160U, 0x0000216fU, 16},	  {0x002183U, 0x00002183U, 1},
	{0x0024b6U, 0x000024cfU, 26},	  {0x002c00U, 0x00002c2fU, 48},
	{0x002c60U, 0x00002c60U, 1},	  {0x002c62U, 0x00002c62U, -10743},
	{0x002c63U, 0x00002c63U, -3814},  {0x002c64U, 0x00002c64U, -10727},
	{0x002c67U, 0x80002c6bU, 1},	  {0x002c6dU, 0x00002c6dU, -10780},
	{0x002c6eU, 0x00002c6eU, -10749}, {0x002c6fU, 0x00002c6fU, -10783},
	{0x002c70U, 0x00002c70U, -10782}, {0x002c72U, 0x00002c72U, 1},
	{0x002c75U, 0x00002c75U, 1},	  {0x002c7eU, 0x00002c7fU, -10815},
	{0x002c80U, 0x80002ce2U, 1},	  {0x002cebU, 0x80002cedU, 1},
	{0x002cf2U, 0x00002cf2U, 1},	  {0x00a640U, 0x8000a66cU, 1},
	{0x00a680U, 0x8000a69aU, 1},	  {0x00a722U, 0x8000a72eU, 1},
	{0x00a732U, 0x8000a76eU, 1},	  {0x00a779U, 0x8000a77bU, 1},
	{0x00a77dU, 0x0000a77dU, -35332}, {0x00a77eU, 0x8000a786U, 1},
	{0x00a78bU, 0x0000a78bU, 1},	  {0x00a78dU, 0x0000a78dU, -42280},
	{0x00a790U, 0x8000a792U, 1},	  {0x00a796U, 0x8000a7a8U, 1},
	{0x00a7aaU, 0x0000a7aaU, -42308}, {0x00a7abU, 0x0000a7abU, -42319},
	{0x00a7acU, 0x0000a7acU, -42315}, {0x00a7adU, 0x0000a7adU, -42305},
	{0x00a7aeU, 0x0000a7aeU, -42308}, {0x00a7b0U, 0x0000a7b0U, -42258},
	{0x00a7b1U, 0x0000a7b1U, -42282}, {0x00a7b2U, 0x0000a7b2U, -42261},
	{0x00a7b3U, 0x0000a7b3U, 928},	  {0x00a7b4U, 0x8000a7c2U, 1},
	{0x00a7c4U, 0x0000a7c4U, -48},	  {0x00a7c5U, 0x0000a7c5U, -42307},
	{0x00a7c6U, 0x0000a7c6U, -35384}, {0x00a7c7U, 0x8000a7c9U, 1},
	{0x00a7cbU, 0x0000a7cbU, -42343}, {0x00a7ccU, 0x8000a7daU, 1},
	{0x00a7dcU, 0x0000a7dcU, -42561}, {0x00a7f5U, 0x0000a7f5U, 1},
	{0x00ab70U, 0x0000abbfU, -38864}, {0x00fb05U, 0x0000fb05U, 1},
	{0x00ff21U, 0x0000ff3aU, 32},	  {0x010400U, 0x00010427U, 40},
	{0x0104b0U, 0x000104d3U, 40},	  {0x010570U, 0x0001057aU, 39},
	{0x01057cU, 0x0001058aU, 39},	  {0x01058cU, 0x00010592U, 39},
	{0x010594U, 0x00010595U, 39},	  {0x010c80U, 0x00010cb2U, 64},
	{0x010d50U, 0x00010d65U, 32},	  {0x0118a0U, 0x000118bfU, 32},
	{0x016e40U, 0x00016e5fU, 32},	  {0x016ea0U, 0x00016eb8U, 27},
	{0x01e900U, 0x0001e921U, 34},
};

/*
 * FAT filesystem
 */

static const struct inode_ops fat_inode_ops = {
	.lookup = fat_lookup,
	.lookup_casefold = fat_lookup_casefold,
	.create = fat_create,
	.mkdir = fat_mkdir,
	.unlink = fat_unlink,
	.rmdir = fat_rmdir,
	.rename = fat_rename,
	.getattr = fat_getattr,
	.setattr = fat_setattr,
	.truncate = fat_truncate,
	.sync = NULL,
	.reclaim = fat_reclaim,
};

static const struct file_ops fat_regular_ops = {
	.open = fat_open_file,
	.read = fat_read_file,
	.write = fat_write_file,
	.pread = fat_pread_file,
	.pwrite = fat_pwrite_file,
	.pwrite_internal = fat_pwrite_context,
	.fsync = fat_fsync,
	.close = fat_close_file,
};

static const struct file_ops fat_directory_ops = {
	.fsync = fat_fsync_directory,
	.readdir = fat_readdir,
	.close = fat_close_file,
};

const struct filesystem_type drv_fat_filesystem_type = {
	.file_backing_identity = drv_fat_file_backing_identity,
	.file_extents = drv_fat_file_extents,
	.writeback_range = fat_writeback_range,
	.fs_name = "fat",
	.probe = fat_probe,
	.identify = fat_identify,
	.mount = fat_mount_impl,
	.sync = fat_sync_mount,
	.statvfs = fat_statvfs,
	.unmount = fat_unmount_impl,
	.alloc_inode = fat_alloc_inode,
	.free_inode = fat_free_inode,
};

/*
 * Implements the drv fat probe type operation.
 */
int
drv_fat_probe_type(
	struct disk *disk,
	enum bootfat_type *type)
{
	int error;

	/* A disk read in other than 512-byte sectors is not one of these. */
	if (disk == NULL || type == NULL || disk->d_block_size != 512)
		return EOPNOTSUPP;

	/* Asks the shared probe which of the three widths this disk carries. */
	error = fat_probe_volume(disk, 0, type);

	/* Reports what the probe found. */
	return error;
}

/*
 * Implements the drv fat file set loop map operation.
 */
int
drv_fat_file_set_loop_map(
	struct file *file,
	const struct fat_loop_extent *map,
	unsigned count)
{
	struct fat_mount_state *mount;
	struct fat_file_state *state;
	uint64_t next = 0;
	unsigned i;
	int error = 0;

	/* A call that names no file has nothing to attach a map to. */
	if (file == NULL || file->f_inode == NULL ||
	    file->f_inode->i_mount == NULL)
		return EINVAL;	/* Failed. */

	/* Nor is a file of another file system one this driver maps. */
	if (file->f_inode->i_mount->m_type != &drv_fat_filesystem_type)
		return EINVAL;	/* Failed. */

	/* A map and a count are given together, or neither of them is. */
	if ((map == NULL) != (count == 0))
		return EINVAL;	/* Failed. */

	mount = fat_mount_state(file->f_inode->i_mount);
	mutex_lock(&mount->lock);

	/* A file that was never opened through this driver has no state. */
	state = fat_file_get(file);
	if (state == NULL) {
		error = EIO;
		goto out;
	}

	/* A call that names a map is installing one, not taking one away. */
	if (map != NULL) {
		/* A map is only trustworthy for a file with a backing claim. */
		if (file->f_backing_claim == NULL ||
		    mount->disk->d_block_size != 512U) {
			error = EINVAL;
			goto out;
		}

		/* Every extent is checked before any of it is installed. */
		for (i = 0; i < count; i++) {
			/* The extents have to cover the file without a gap. */
			if (map[i].file_block != next) {
				error = EIO;
				goto out;
			}

			/* An extent of no blocks describes nothing. */
			if (map[i].count == 0) {
				error = EIO;
				goto out;
			}

			/* An extent may not begin past the end of the disk. */
			if (map[i].disk_block > mount->disk->d_block_count) {
				error = EIO;
				goto out;
			}

			/* Nor may one that begins on it run off the end. */
			if (map[i].count > mount->disk->d_block_count -
			    map[i].disk_block) {
				error = EIO;
				goto out;
			}

			/* Nor may the running total of blocks overflow. */
			if (next > UINT64_MAX - map[i].count) {
				error = EIO;
				goto out;
			}

			next += map[i].count;
		}

		/* The extents have to cover the file sector for sector. */
		if (state->size % 512U != 0 || next != state->size / 512U) {
			error = EIO;
			goto out;
		}

		/* Nothing cached may outlive the change of the mapping. */
		error = fat_engine_flush(mount);
		if (error != 0)
			goto out;

		fat_engine_invalidate(mount);
	}

	state->loop_map = map;
	state->loop_map_count = count;
out:

	mutex_unlock(&mount->lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Implements the drv fat file backing identity operation.
 */
int
drv_fat_file_backing_identity(
	struct inode *inode,
	struct disk **disk,
	uint64_t *object)
{
	struct fat_mount_state *state;
	struct fat_inode_info *info;

	/* A call that leaves out any of what it needs cannot be answered. */
	if (inode == NULL || disk == NULL || object == NULL)
		return EINVAL;

	/* Only a regular file of a FAT volume has a chain to loop over. */
	if (inode->i_type != INODE_REG || inode->i_mount == NULL ||
	    inode->i_mount->m_type != &drv_fat_filesystem_type ||
	    inode->i_mount->m_disk == NULL) {
		/* Failed. */
		return EOPNOTSUPP;
	}

	/* A mount this driver did not make has no state of its own. */
	state = fat_mount_state(inode->i_mount);
	if (state == NULL)
		return EIO;

	mutex_lock(&state->lock);

	info = fat_inode(inode);
	*disk = inode->i_mount->m_disk;

	/*
	 * The directory-entry location is identical across separate mounts of
	 * the same FAT volume. Claimed rename is rejected before it can change
	 * these fields.
	 */
	*object = ((uint64_t)info->fi_dirent_lba << 16) |
		  (uint64_t)info->fi_dirent_offset;

	mutex_unlock(&state->lock);

	/* Succeeded. */
	return 0;
}

/*
 * Reports every extent of a FAT file to a callback.
 *
 * It is public because three subsystems outside the file systems need a
 * file's physical layout and cannot go through the ordinary read path: the
 * loop device builds its extent map from it, the format reservation in
 * file.c checks that a file is contiguous, and swap turns a file into a set
 * of device ranges.  Each of those holds a backing claim, which is what makes
 * the layout stable long enough to be worth reporting.
 */
int
drv_fat_file_extents(
	struct file *file,
	fat_extent_cb callback,
	void *context)
{
	struct fat_mount_state *mount_state;
	struct fat_file_state *state;
	int error;

	/* A call that names no file, or nowhere to report the extents to. */
	if (file == NULL || callback == NULL || file->f_inode == NULL)
		return EINVAL;	/* Failed. */

	/* Only a regular file has a run of blocks of its own. */
	if (file->f_inode->i_type != INODE_REG)
		return EINVAL;	/* Failed. */

	/* And it has to be a file of a FAT volume. */
	if (file->f_inode->i_mount == NULL ||
	    file->f_inode->i_mount->m_type != &drv_fat_filesystem_type)
		return EINVAL;	/* Failed. */

	/* A mount this driver did not make has no state of its own. */
	mount_state = fat_mount_state(file->f_inode->i_mount);
	if (mount_state == NULL)
		return EIO;
	mutex_lock(&mount_state->lock);

	/* A file that was never opened through this driver has no state. */
	state = fat_file_get(file);
	if (state == NULL)
		error = EIO;
	else
		error = fat_engine_file_extents(state, callback, context);

	mutex_unlock(&mount_state->lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Retires all shared chain proofs before a mutation or external invalidation.
 */
static void
fat_chain_invalidate(
	struct fat_mount_state *state)
{
	/* A generation that has run out stops, invalidating nothing. */
	if (state->chain_generation != UINT64_MAX)
		state->chain_generation++;
	io_stats_record(IO_FAT_CHAIN_INVALIDATE, 0);
}

/* Takes the private inode this driver keeps beside a kernel one. */
static struct fat_inode_info *
fat_inode(
	struct inode *inode)
{
	/* The private inode, which is the first member of the slot. */
	return (struct fat_inode_info *)inode;
}

/* Copies bytes without depending on a C library. */
static void
fat_engine_copy_bytes(
	void *destination,
	const void *source,
	uint32_t length)
{
	uint8_t *output = destination;
	const uint8_t *input = source;

	/* Copies the run one byte at a time, back to front. */
	while (length--)
		*output++ = *input++;
}

/* Reads a 16-bit field, which FAT stores least significant byte first. */
static uint16_t
fat_engine_get16(
	const uint8_t *bytes)
{
	/* The assembled halfword. */
	return bytes[0] | ((uint16_t)bytes[1] << 8);
}

/* Reads a 32-bit field, which FAT stores least significant halfword first. */
static uint32_t
fat_engine_get32(
	const uint8_t *bytes)
{
	uint16_t low;
	uint16_t high;

	/* The two halves the field is stored as. */
	low = fat_engine_get16(bytes);
	high = fat_engine_get16(bytes + 2);

	/* The assembled word. */
	return low | ((uint32_t)high << 16);
}

/* Reads one 512-byte sector of the volume. */
static int
fat_sector_read(
	struct fat_mount_state *state,
	uint32_t lba,
	void *buffer)
{
	int error;

	/* A call that names no disk, or nowhere to read into. */
	if (state == NULL || state->disk == NULL || buffer == NULL)
		return EINVAL;

	/*
	 * A mount opened for direct I/O never reads through the buffer cache.
	 */
	if (state->direct_io)
		error = disk_read_direct(state->disk, lba, 1, buffer);
	else
		error = disk_read(state->disk, lba, 1, buffer);

	/* Reports how the read went. */
	return error;
}

/* Writes one 512-byte sector of the volume, in mount order. */
static int
fat_sector_write(
	struct fat_mount_state *state,
	uint32_t lba,
	const void *buffer)
{
	struct io_context context;
	int written;
	int error;

	/* A call that names no disk, or nothing to write from. */
	if (state == NULL || state->disk == NULL || buffer == NULL)
		return EINVAL;

	/* A volume mounted read-only is never written to. */
	if (state->read_only)
		return EROFS;

	/* The write joins the mount's own ordering context. */
	error = io_context_child(&context, state->write_context,
				 IO_CONTEXT_ORDERED);
	if (error != 0)
		return error;

	/* Writes the sector out through that context. */
	written = disk_write_filesystem_context(state->disk, lba, 1, buffer,
						&context);

	/* Reports how the write went. */
	return written;
}

/* Writes the one cached sector back when it has been changed. */
static int
fat_engine_flush(
	struct fat_mount_state *filesystem)
{
	int result;

	/* A call that names no mount has nothing to flush. */
	if (!filesystem)
		return EINVAL;

	/* A cached sector that was never changed is already on the volume. */
	if (!filesystem->sector_cache_dirty)
		return 0;

	/* Writes the one cached sector back where it came from. */
	result = fat_sector_write(filesystem, filesystem->sector_cache_lba,
				  filesystem->sector_cache);
	if (result == 0) {
		filesystem->sector_cache_dirty = 0;

		/* The write epoch this mount opened is now closed again. */
		if (filesystem->owner != NULL)
			io_epoch_end(&filesystem->owner->m_write_epoch);
	}

	/* Reports how the write went. */
	return result;
}

/* Drops the cached sector without writing it back. */
static void
fat_engine_invalidate(
	struct fat_mount_state *filesystem)
{
	/* A call that names no mount has nothing to invalidate. */
	if (!filesystem)
		return;

	/* A change that is being dropped still closes the epoch it opened. */
	if (filesystem->sector_cache_dirty && filesystem->owner != NULL)
		io_epoch_end(&filesystem->owner->m_write_epoch);

	filesystem->sector_cache_valid = 0;
	filesystem->sector_cache_dirty = 0;
	memset(filesystem->clean_sectors, 0, sizeof(filesystem->clean_sectors));
	fat_chain_invalidate(filesystem);
}

/* Reads a sector into the one-sector cache and reports where it landed. */
static int
fat_engine_read_sector_result(
	struct fat_mount_state *filesystem,
	uint32_t lba,
	const uint8_t **sector)
{
	struct fat_clean_sector *slot;
	uint8_t saved[512];
	unsigned index, found;
	int result;

	/* Validates the request and preserves the active mutable identity. */
	if (filesystem == NULL || sector == NULL)
		return EINVAL;

	/* A sector past the end of the volume cannot be read. */
	if (lba >= filesystem->total_sectors)
		return EIO;

	/* The one cached sector answers a read of the sector it holds. */
	if (filesystem->sector_cache_valid &&
	    filesystem->sector_cache_lba == lba) {
		io_stats_record(IO_FAT_SECTOR_HIT, 512);
		*sector = filesystem->sector_cache;
		/* Succeeded. */
		return 0;
	}

	/* Completes the old mutable sector before retaining any clean copy. */

	/* The sector about to be replaced is written back first. */
	result = fat_engine_flush(filesystem);
	if (result != 0)
		return result;

	/* A recently read sector may still be held in a clean slot. */
	found = 0;
	for (index = 0; index < FAT_CLEAN_SLOTS; index++) {
		/* A slot holding this sector saves a read of the volume. */
		slot = &filesystem->clean_sectors[index];
		if (slot->valid && slot->lba == lba) {
			memcpy(saved, slot->bytes, sizeof(saved));
			slot->valid = 0;
			found = 1;
			break;
		}
	}

	/*
	 * Retains the previous clean window within the fixed mount-owned
	 * budget.
	 */
	if (filesystem->sector_cache_valid) {
		slot = &filesystem->clean_sectors[filesystem->clean_rotor];
		filesystem->clean_rotor =
			(filesystem->clean_rotor + 1U) % FAT_CLEAN_SLOTS;
		memcpy(slot->bytes, filesystem->sector_cache,
			sizeof(slot->bytes));
		slot->lba = filesystem->sector_cache_lba;
		slot->valid = 1;
	}

	filesystem->sector_cache_valid = 0;
	filesystem->sector_cache_dirty = 0;

	/*
	 * Copies a retained clean sector or performs the existing read on a
	 * miss.
	 */
	if (found) {
		memcpy(filesystem->sector_cache, saved, sizeof(saved));
		io_stats_record(IO_FAT_SECTOR_HIT, 512);
	} else {
		io_stats_record(IO_FAT_SECTOR_MISS, 512);

		/* Nothing held it, so it has to be read off the volume. */
		result = fat_sector_read(filesystem, lba,
					 filesystem->sector_cache);
		if (result != 0)
			return result;
	}

	filesystem->sector_cache_lba = lba;
	filesystem->sector_cache_valid = 1;
	*sector = filesystem->sector_cache;

	/* Publishes the sole mutable window. */
	return 0;
}

/* Reads a sector into the cache so the caller may change it in place. */
static int
fat_engine_write_sector_result(
	struct fat_mount_state *filesystem,
	uint32_t lba,
	uint8_t **sector)
{
	const uint8_t *read_sector;
	int result;

	/* A call that names no mount, or nowhere to report the sector. */
	if (!filesystem || !sector)
		return EINVAL;

	/* A volume mounted read-only is never written to. */
	if (filesystem->read_only)
		return EROFS;

	/* The sector is read first, because only part of it will change. */
	result = fat_engine_read_sector_result(filesystem, lba, &read_sector);
	if (result != 0)
		return result;

	*sector = (uint8_t *)read_sector;

	/* Succeeded. */
	return 0;
}

/* Records that the cached sector now differs from the volume. */
static int
fat_engine_mark_sector_dirty(
	struct fat_mount_state *filesystem)
{
	/* A volume mounted read-only has nothing that may be marked dirty. */
	if (!filesystem || filesystem->read_only)
		return EROFS;

	/* Nor has one whose cache holds no sector at all. */
	if (!filesystem->sector_cache_valid)
		return EIO;

	/* The first change of a sector opens the mount's write epoch. */
	if (filesystem->owner != NULL && !filesystem->sector_cache_dirty)
		io_epoch_begin(&filesystem->owner->m_write_epoch);

	filesystem->sector_cache_dirty = 1;

	/* Succeeded. */
	return 0;
}

/* Turns a cluster number and a sector inside it into a volume address. */
static int
fat_engine_cluster_lba(
	struct fat_mount_state *filesystem,
	uint32_t cluster,
	uint32_t sector_in_cluster,
	uint32_t *lba)
{
	struct fat_mount_state *fat;
	uint32_t cluster_offset;

	/* A call that names no mount, or nowhere to report the address. */
	if (!filesystem || !lba)
		return EINVAL;

	/* A cluster number the volume has not got, or a sector past its end. */
	fat = filesystem;
	if (cluster < 2 || cluster >= fat->cluster_count + 2 ||
	    sector_in_cluster >= fat->sectors_per_cluster) {
		/* Failed. */
		return EIO;
	}

	/* An address that would run past the end of the volume. */
	if (cluster - 2 >
	    (0xffffffffU - fat->data_start) / fat->sectors_per_cluster) {
		/* Failed. */
		return EIO;
	}

	cluster_offset = fat->data_start +
		(cluster - 2) * fat->sectors_per_cluster;

	/* Nor may adding the sector inside the cluster overflow it. */
	if (sector_in_cluster > 0xffffffffU - cluster_offset)
		return EIO;

	/* And the address that comes out has to lie inside the volume. */
	*lba = cluster_offset + sector_in_cluster;
	if (*lba >= fat->total_sectors)
		return EIO;

	/* Succeeded. */
	return 0;
}

/*
 * Reads the BIOS parameter block and derives the volume layout from it.
 *
 * The block gives sizes and counts, not addresses; where each area of the
 * volume starts has to be added up from them.  Every sum is checked for
 * overflow before it is made, because a malformed block would otherwise
 * produce an address that appears to lie inside the volume.
 */
static int
parse_bpb(
	struct fat_mount_state *fat)
{
	uint8_t bpb[512];
	uint32_t reserved, fat_sectors, fat32_sectors, root_sectors, metadata;
	uint32_t total, total_physical, data_sectors;
	uint16_t bytes, fat16_sectors;
	uint8_t sectors_per_cluster;
	int error;

	/* Reads the first sector, which is where the block lives. */
	error = fat_sector_read(fat, 0, bpb);
	if (error != 0)
		return EIO;	/* Failed. */

	/*
	 * How many bytes one sector of the volume holds.  This driver reads
	 * the volume in 512-byte units, so a sector is either one such unit
	 * or two of them, and nothing else can be mounted.
	 */
	bytes = fat_engine_get16(bpb + 11);
	if (bytes == 512) {
		fat->sector_scale = 1;
	} else if (bytes == 1024) {
		fat->sector_scale = 2;
	} else {
		fat->sector_scale = 0;
	}

	/* How many sectors make up one cluster, the unit files are given. */
	sectors_per_cluster = bpb[13];

	/* How many sectors sit in front of the first file allocation table. */
	reserved = fat_engine_get16(bpb + 14);

	/* How many copies of that table the volume keeps. */
	fat->number_of_fats = bpb[16];

	/* How many entries the fixed root directory holds, on FAT12 and 16. */
	fat->root_entries = fat_engine_get16(bpb + 17);

	/*
	 * How many sectors the volume holds.  A volume too large for the
	 * 16-bit field carries zero there and gives the count in the 32-bit
	 * one instead.
	 */
	total = fat_engine_get16(bpb + 19);
	if (total == 0)
		total = fat_engine_get32(bpb + 32);

	/*
	 * How many sectors one copy of the allocation table spans.  FAT32
	 * leaves the 16-bit field zero and uses the 32-bit one, which is also
	 * how the layout is told apart below.
	 */
	fat16_sectors = fat_engine_get16(bpb + 22);
	fat32_sectors = fat_engine_get32(bpb + 36);
	fat_sectors = fat16_sectors;
	if (fat_sectors == 0)
		fat_sectors = fat32_sectors;

	/* A sector size this driver cannot read leaves nothing to mount. */
	if (fat->sector_scale == 0)
		return EIO;	/* Failed. */

	/* A cluster of no sectors could hold no file. */
	if (sectors_per_cluster == 0)
		return EIO;	/* Failed. */

	/* The boot sector itself is reserved, so the count is never zero. */
	if (reserved == 0)
		return EIO;	/* Failed. */

	/* Nor is a volume without a single allocation table usable. */
	if (fat->number_of_fats == 0)
		return EIO;	/* Failed. */

	/* Nor one whose table spans no sectors. */
	if (fat_sectors == 0)
		return EIO;	/* Failed. */

	/* Nor one that declares no sectors at all. */
	if (total == 0)
		return EIO;	/* Failed. */

	/*
	 * The counts above are in the volume's own sectors and are about to
	 * be turned into 512-byte units.  A count that would not survive that
	 * multiplication describes a volume larger than can be addressed.
	 */
	if (total > 0xffffffffU / fat->sector_scale)
		return EIO;	/* Failed. */

	if (reserved > 0xffffffffU / fat->sector_scale)
		return EIO;	/* Failed. */

	if (fat_sectors > 0xffffffffU / fat->sector_scale)
		return EIO;	/* Failed. */

	/* The size of the whole volume, in the units the disk is read in. */
	total_physical = total * fat->sector_scale;

	/* A mount without a disk behind it has nothing to read. */
	if (fat->disk == NULL)
		return EIO;	/* Failed. */

	/* And a volume larger than its disk was never written there. */
	if (total_physical > fat->disk->d_block_count)
		return EIO;	/* Failed. */

	/* The same conversion for the two areas in front of the data. */
	reserved *= fat->sector_scale;
	fat_sectors *= fat->sector_scale;
	fat->sectors_per_cluster = sectors_per_cluster * fat->sector_scale;

	/* The fixed root directory holds thirty-two bytes for each entry. */
	root_sectors = ((uint32_t)fat->root_entries * 32 + 511) >> 9;

	/* Every copy of the allocation table has to fit after the reserved. */
	if (fat_sectors > (0xffffffffU - reserved) / fat->number_of_fats)
		return EIO;	/* Failed. */

	/* Everything in front of the file data: reserved area and tables. */
	metadata = reserved + fat_sectors * fat->number_of_fats;

	/* The root directory follows them, and has to fit as well. */
	if (root_sectors > 0xffffffffU - metadata)
		return EIO;	/* Failed. */

	metadata += root_sectors;

	/* A volume entirely taken up by its own metadata holds no file. */
	if (metadata >= total_physical)
		return EIO;	/* Failed. */

	/* What is left over is the data area, measured in clusters. */
	data_sectors = total_physical - metadata;
	fat->cluster_count = data_sectors / fat->sectors_per_cluster;

	/*
	 * The number of clusters is what decides the width of an allocation
	 * table entry, and so which of the three formats this volume is.
	 */
	if (fat->cluster_count < 4085)
		fat->type = KERN_FAT12;
	else if (fat->cluster_count < 65525)
		fat->type = KERN_FAT16;
	else
		fat->type = KERN_FAT32;

	/* Where the first allocation table starts, and how long it is. */
	fat->fat_start = reserved;
	fat->fat_sectors = fat_sectors;

	/* Where the fixed root directory starts, just past the last table. */
	fat->root_start = reserved + fat_sectors * fat->number_of_fats;

	/* And where the file data starts, just past that directory. */
	fat->data_start = fat->root_start + root_sectors;

	fat->total_sectors = total_physical;
	fat->bytes_per_sector = bytes;

	/* A FAT12 or FAT16 volume has a fixed root and a 16-bit table size. */
	fat->fat16_layout = fat16_sectors != 0 && fat->root_entries != 0;

	/* A FAT32 volume has neither, and keeps its root in the data area. */
	fat->fat32_layout = fat16_sectors == 0 && fat32_sectors != 0 &&
			    fat->root_entries == 0;

	/* Which cluster that root directory begins at. */
	fat->root_cluster = fat_engine_get32(bpb + 44) & 0x0fffffffU;

	/* And where the sector holding the free-cluster hints sits. */
	fat->fsinfo_sector = fat_engine_get16(bpb + 48);

	/* Everything below concerns FAT32 only. */
	if (fat->type != KERN_FAT32) {
		/* Succeeded. */
		return 0;
	}

	/* A FAT32 volume that does not carry the FAT32 layout is malformed. */
	if (!fat->fat32_layout)
		return EIO;	/* Failed. */

	/*
	 * The first two cluster numbers are reserved, so a root is never one.
	 */
	if (fat->root_cluster < 2U)
		return EIO;	/* Failed. */

	/* Nor may the root sit past the last cluster the volume has. */
	if (fat->root_cluster >= fat->cluster_count + 2U)
		return EIO;	/* Failed. */

	/* Succeeded. */
	return 0;
}

/* Reads the layout of a volume and refuses one of the wrong width. */
static int
fat_engine_mount(
	struct fat_mount_state *filesystem,
	enum bootfat_type required_type)
{
	struct fat_mount_state *fat = filesystem;
	int result;

	/* Reads the volume layout out of the BIOS parameter block. */
	result = parse_bpb(fat);
	if (result != 0)
		return result;	/* Failed. */

	/* A volume of another width is not the one being mounted. */
	if (fat->type != required_type)
		return EOPNOTSUPP;	/* Failed. */

	/* A FAT16 mount needs the fixed root directory that layout has. */
	if (required_type == KERN_FAT16 && !fat->fat16_layout)
		return EOPNOTSUPP;	/* Failed. */

	/* And a FAT32 mount needs the root that lives in the data area. */
	if (required_type == KERN_FAT32 && !fat->fat32_layout)
		return EOPNOTSUPP;	/* Failed. */

	fat->allocation_hint = 2;
	fat_engine_invalidate(filesystem);

	/* Succeeded. */
	return 0;
}

/* Renders a path's last component as a FAT short name, if it fits one. */
static int
fat_sfn_encode(
	const char *path,
	char output[11])
{
	char character;
	unsigned index;
	unsigned base = 0, extension = 0;

	/* An unused position of a short name holds a space, not a null. */
	for (index = 0; index < 11; index++)
		output[index] = ' ';

	/* A leading slash is the root the name is taken relative to. */
	if (*path == '/')
		path++;

	/* A component with no name has no short name either. */
	if (!*path)
		return 0;

	/* Takes the base, which is everything in front of the dot. */
	while (*path && *path != '.') {
		character = *path++;

		/* A name that is really a path, or a base of over eight. */
		if (character == '/' || base == 8)
			return 0;

		/* A short name is stored in upper case. */
		if (character >= 'a' && character <= 'z')
			output[base++] = (char)(character - 32);
		else
			output[base++] = character;
	}

	/* A component that is nothing but an extension has no short name. */
	if (!base)
		return 0;

	/* Steps over the dot that separates the two halves. */
	if (*path == '.')
		path++;

	/* Takes the extension, which is everything after it. */
	while (*path) {
		character = *path++;

		/* A second dot or a slash, or an extension of over three. */
		if (character == '/' || character == '.' || extension == 3)
			return 0;

		/* A short name is stored in upper case. */
		if (character >= 'a' && character <= 'z')
			output[8 + extension++] = (char)(character - 32);
		else
			output[8 + extension++] = character;
	}

	/* Succeeded. */
	return 1;
}

/* Compares two stored short names without regard to case. */
static int
fat_sfn_equal(
	const uint8_t entry[32],
	const char name[11])
{
	uint8_t left;
	uint8_t right;
	unsigned index;

	/* Compares the two names one stored character at a time. */
	for (index = 0; index < 11; index++) {
		left = entry[index];
		right = (uint8_t)name[index];

		/* A short name is compared without regard to case. */
		if (left >= 'a' && left <= 'z')
			left -= 'a' - 'A';

		if (right >= 'a' && right <= 'z')
			right -= 'a' - 'A';

		/* One character that differs is enough. */
		if (left != right)
			return 0;
	}

	/* Succeeded: the two names are the same. */
	return 1;
}

/* Renders a stored short name as the lower-case name a caller sees. */
static void
fat_sfn_decode_lower(
	const uint8_t raw[32],
	struct fat_dir_entry *entry)
{
	uint8_t character;
	unsigned index;
	unsigned output = 0;

	/* The base runs to the first space, of which there are up to eight. */
	for (index = 0; index < 8 && raw[index] != ' '; index++) {
		character = raw[index];

		/* A stored name is upper case; this driver shows it lower. */
		if (character >= 'A' && character <= 'Z')
			character += 'a' - 'A';

		entry->name[output++] = (char)character;
	}

	/*
	 * An extension is present when the position after the base is not
	 * blank.
	 */
	if (raw[8] != ' ') {
		/*
		 * The dot exists only in the shown name, never in the stored
		 * one.
		 */
		entry->name[output++] = '.';

		/* The extension runs the same way, over three positions. */
		for (index = 8; index < 11 && raw[index] != ' '; index++) {
			character = raw[index];

			/* Shown lower case, like the base. */
			if (character >= 'A' && character <= 'Z')
				character += 'a' - 'A';

			entry->name[output++] = (char)character;
		}
	}

	entry->name[output] = 0;

	/*
	 * The size and the attribute byte the record carries beside the name.
	 */
	entry->size = fat_engine_get32(raw + 28);
	entry->attributes = raw[11];
}

/* Tests whether a cluster number names a cluster the volume has. */
static int
valid_cluster(
	uint32_t cluster,
	uint32_t end_of_chain)
{
	/* Clusters zero and one are reserved and name no storage. */
	if (cluster < 2)
		return 0;

	/* Anything at or above the end marker is not a cluster either. */
	if (cluster >= end_of_chain)
		return 0;

	/* Reports that the number names a real cluster. */
	return 1;
}

/* Reads a run of bytes by walking the cluster chain of a file. */
static int
fat_engine_read_chain(
	struct fat_file_state *file,
	uint64_t offset,
	void *buffer,
	uint32_t length,
	fat_read_progress_fn progress,
	void *progress_context,
	fat_next_cluster_fn next_cluster,
	uint32_t end_of_chain)
{
	int result;
	uint32_t lba;
	uint32_t chunk;
	const uint8_t *input;
	struct fat_mount_state *filesystem = file->mount;
	struct fat_mount_state *fat = filesystem;
	struct fat_file_state *fat_file = file;
	uint32_t cluster = fat_file->first_cluster;
	uint32_t position, skip, within, since_update = 0;
	uint8_t *output = buffer;
	struct fat_chain_cursor cursor = {0, 0};
	uint32_t last;
	uint64_t generation, end;
	int validation;
	int valid;

	/* A read past what a 32-bit position can express cannot be made. */
	if (offset > 0xffffffffU || !next_cluster)
		return EINVAL;

	/* Nor one starting from a cluster that is not part of a chain. */
	/* Asks whether that is a cluster the volume has. */
	valid = valid_cluster(cluster, end_of_chain);
	if (!valid)
		return EIO;

	generation = fat->chain_generation;
	end = offset + length;

	/* Takes the cached cursor when it still stands where it should. */
	validation = fat_file_validate_at(file, offset, &cursor, &last);
	if (validation != 0)
		return validation;

	cluster = cursor.cluster;
	position = (uint32_t)offset;
	skip = position / 512 - cursor.index * fat->sectors_per_cluster;
	within = position & 511;

	/* Walks forward whole clusters until the run's first one is reached. */
	while (skip >= fat->sectors_per_cluster) {
		/* Steps to the cluster that follows this one. */
		result = next_cluster(filesystem, cluster, &cluster);
		if (result != 0)
			return result;

		/* A chain that leaves the volume means the file is corrupt. */
		/* Asks whether that is a cluster the volume has. */
		valid = valid_cluster(cluster, end_of_chain);
		if (!valid)
			return EIO;
		skip -= fat->sectors_per_cluster;
		cursor.index++;
	}

	while (length) {
		int result;

		chunk = 512 - within;

		/* Turns the cluster and the sector in it into an address. */
		result = fat_engine_cluster_lba(filesystem, cluster, skip,
			&lba);
		if (result != 0)
			return result;

		/* Reads that sector into the one-sector cache. */
		result = fat_engine_read_sector_result(filesystem, lba, &input);
		if (result != 0)
			return result;

		/* The run may end inside this sector rather than at its end. */
		if (chunk > length)
			chunk = length;

		fat_engine_copy_bytes(output, input + within, chunk);

		output += chunk;
		length -= chunk;

		/* A caller that asked about progress is told in steps. */
		if (progress) {
			since_update += chunk;

			/* The last step is reported however small it is. */
			if (since_update >= FAT_PROGRESS_INTERVAL || !length) {
				progress(progress_context, since_update);
				since_update = 0;
			}
		}

		within = 0;

		/* A run reaching the end of a cluster goes on in the next. */
		if (++skip >= fat->sectors_per_cluster && length) {
			skip = 0;
			cursor.index++;

			/* Steps to the cluster that follows this one. */
			result = next_cluster(filesystem, cluster, &cluster);
			if (result != 0)
				return result;

			/* A chain that leaves the volume means corruption. */
			/* Asks whether that is a cluster the volume has. */
			valid = valid_cluster(cluster, end_of_chain);
			if (!valid)
				return EIO;
		}
	}

	cursor.cluster = cluster;
	fat_file_save_cursor(file, &cursor, end, generation, last);

	/* Succeeded. */
	return 0;
}

/* Reads a 16-bit field, which FAT stores least significant byte first. */
static uint16_t
get16(
	const uint8_t *p)
{
	/* The assembled halfword. */
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* Forgets whatever long name was being collected. */
static void
fat_lfn_reset(
	struct fat_lfn_state *state)
{
	unsigned i;

	state->unit_limit = 0;
	state->expected = 0;
	state->checksum = 0;
	state->active = 0;
	/* An unwritten unit is the value no code point can have. */
	for (i = 0; i <= FAT_LFN_MAX_UNITS; i++)
		state->units[i] = 0xffffU;
}

/* Computes the checksum that ties a long name to its short one. */
static uint8_t
fat_lfn_checksum(
	const uint8_t sfn[11])
{
	uint8_t sum = 0;
	unsigned i;

	/* Every character of the short name is folded into the running sum. */
	for (i = 0; i < 11; i++)
		sum = (uint8_t)(((sum & 1U) << 7) | (sum >> 1)) + sfn[i];

	/* The checksum a long-name record has to carry. */
	return sum;
}

/* Takes one directory record into the long name being collected. */
static int
fat_lfn_feed(
	struct fat_lfn_state *state,
	const uint8_t raw[32])
{
	unsigned index;
	unsigned ordinal = raw[0] & 0x1fU;
	unsigned i;
	uint16_t cluster;

	/*
	 * A long-name entry leaves the first cluster field of the record zero.
	 */
	cluster = get16(raw + 26);

	/*
	 * A record belongs to a long name when it carries the long-name
	 * attribute, a zero type byte and that zero cluster field, and when
	 * its ordinal is one of the twenty a name may span.  Anything else is
	 * an ordinary record, which ends whatever name was being collected.
	 */
	if (raw[11] != 0x0fU || raw[12] != 0 || cluster != 0 ||
	    ordinal == 0 || ordinal > 20U) {
		fat_lfn_reset(state);

		/* Succeeded: the record is simply not part of a long name. */
		return 0;
	}

	/* The last record of a name is written first and marks itself so. */
	if (raw[0] & 0x40U) {
		fat_lfn_reset(state);
		state->active = 1;
		state->expected = (uint8_t)ordinal;
		state->checksum = raw[13];
		state->unit_limit = (uint16_t)(ordinal * 13U);

		/* A name longer than this driver holds is cut to fit. */
		if (state->unit_limit > FAT_LFN_MAX_UNITS + 1U)
			state->unit_limit = FAT_LFN_MAX_UNITS + 1U;
	}

	/* A record out of order, or of another name, ends the collection. */
	if (!state->active || ordinal != state->expected ||
	    raw[13] != state->checksum || (raw[0] & 0x80U)) {
		fat_lfn_reset(state);

		/* Succeeded. */
		return 0;
	}

	/* The thirteen units of a record are scattered over three runs. */
	for (i = 0; i < 13; i++) {
		/* Where in the whole name this record's units belong. */
		index = (ordinal - 1U) * 13U + i;
		if (index <= FAT_LFN_MAX_UNITS)
			state->units[index] = get16(raw + lfn_offsets[i]);
	}

	state->expected--;

	/* Succeeded: the record was taken into the name. */
	return 1;
}

/* Appends one code point to a UTF-8 string being built. */
static int
append_utf8(
	char *output,
	size_t capacity,
	size_t *used,
	uint32_t scalar)
{
	uint8_t bytes[4];
	unsigned count, i;

	/* How many bytes the code point takes depends on how large it is. */
	if (scalar <= 0x7fU) {
		bytes[0] = (uint8_t)scalar;
		count = 1;
	} else if (scalar <= 0x7ffU) {
		bytes[0] = (uint8_t)(0xc0U | (scalar >> 6));
		bytes[1] = (uint8_t)(0x80U | (scalar & 0x3fU));
		count = 2;
	} else if (scalar <= 0xffffU) {
		bytes[0] = (uint8_t)(0xe0U | (scalar >> 12));
		bytes[1] = (uint8_t)(0x80U | ((scalar >> 6) & 0x3fU));
		bytes[2] = (uint8_t)(0x80U | (scalar & 0x3fU));
		count = 3;
	} else {
		bytes[0] = (uint8_t)(0xf0U | (scalar >> 18));
		bytes[1] = (uint8_t)(0x80U | ((scalar >> 12) & 0x3fU));
		bytes[2] = (uint8_t)(0x80U | ((scalar >> 6) & 0x3fU));
		bytes[3] = (uint8_t)(0x80U | (scalar & 0x3fU));
		count = 4;
	}

	/* The buffer has to hold the bytes and a terminator after them. */
	if (*used + count >= capacity)
		return 0;
	/* Appends the bytes the code point was rendered as. */
	for (i = 0; i < count; i++)
		output[(*used)++] = (char)bytes[i];

	/* Succeeded. */
	return 1;
}

/* Renders a completed long name, if its records agree with the short one. */
static int
fat_lfn_finish(
	struct fat_lfn_state *state,
	const uint8_t sfn[32],
	char *output,
	size_t capacity)
{
	uint32_t low;
	uint32_t scalar;
	size_t used = 0;
	unsigned i;
	int terminated = 0;
	int checksum;
	int appended;

	/* A name is only complete when every record of it has arrived. */
	checksum = fat_lfn_checksum(sfn);
	if (!state->active || state->expected != 0 ||
	    state->checksum != checksum || capacity == 0)
		goto invalid;

	/* Renders the collected units as the UTF-8 name a caller sees. */
	for (i = 0; i < state->unit_limit; i++) {
		/* A zero unit is where the name ends. */
		scalar = state->units[i];
		if (scalar == 0) {
			terminated = 1;
			break;
		}

		/* A unit that was never written means a record is missing. */
		if (scalar == 0xffffU)
			goto invalid;

		/* A code point past the basic plane is stored as two units. */
		if (scalar >= 0xd800U && scalar <= 0xdbffU) {
			/* The second half has to be there. */
			if (++i >= state->unit_limit)
				goto invalid;

			/* And it has to be a low half, not another high one. */
			low = state->units[i];
			if (low < 0xdc00U || low > 0xdfffU)
				goto invalid;
			scalar = 0x10000U + ((scalar - 0xd800U) << 10) +
				 (low - 0xdc00U);
		} else if (scalar >= 0xdc00U && scalar <= 0xdfffU) {
			goto invalid;
		}

		/* A separator or a null in a name would make it unusable. */
		if (scalar == '/' || scalar == 0)
			goto invalid;

		/* Appends the code point to the name being built. */
		appended = append_utf8(output, capacity, &used, scalar);
		if (!appended)
			goto invalid;
	}

	/* A name of nothing, or one whose end was never seen. */
	if (used == 0 || (!terminated && state->unit_limit > FAT_LFN_MAX_UNITS))
		goto invalid;

	/* Everything past the end has to be padding, not more name. */
	if (terminated) {
		/* Walks whatever units follow the one that ended the name. */
		for (; i < state->unit_limit; i++) {
			/* A unit that is not zero or padding is corrupt. */
			if (state->units[i] != 0 && state->units[i] != 0xffffU)
				goto invalid;
		}
	}

	output[used] = '\0';
	fat_lfn_reset(state);

	/* Succeeded. */
	return 1;
invalid:
	fat_lfn_reset(state);

	/* An empty name is reported when the buffer can hold one. */
	if (capacity)
		output[0] = '\0';

	/* Succeeded. */
	return 0;
}

/* Renders a stored short name with the case its flag byte recorded. */
static void
fat_sfn_decode_preserve(
	const uint8_t raw[32],
	char *output,
	size_t capacity)
{
	uint8_t character;
	size_t used = 0;
	unsigned i;
	int lower_base = (raw[12] & 0x08U) != 0;
	int lower_ext = (raw[12] & 0x10U) != 0;

	/* A buffer with no room cannot even hold the terminator. */
	if (capacity == 0)
		return;

	/* The base runs to the first space, or to the end of the buffer. */
	for (i = 0; i < 8 && raw[i] != ' ' && used + 1U < capacity; i++) {
		character = raw[i];

		/*
		 * The stored name is upper case, and a flag byte records
		 * whether each half was lower case before it was stored.
		 */
		if (lower_base && character >= 'A' && character <= 'Z')
			character += 'a' - 'A';

		output[used++] = (char)character;
	}

	/*
	 * An extension is present when the position after the base is not
	 * blank.
	 */
	if (raw[8] != ' ' && used + 1U < capacity) {
		/* The dot exists only in the shown name. */
		output[used++] = '.';

		/* The extension runs the same way, over three positions. */
		for (i = 8; i < 11 && raw[i] != ' ' && used + 1U < capacity;
		     i++) {
			character = raw[i];

			/* Its own flag says whether it was lower case. */
			if (lower_ext && character >= 'A' && character <= 'Z')
				character += 'a' - 'A';

			output[used++] = (char)character;
		}
	}

	output[used] = '\0';
}

/* Folds one code point to the case-insensitive form names compare in. */
static uint32_t
fold_scalar(
	uint32_t scalar)
{
	size_t middle;
	const struct fat_casefold_range *range;
	uint32_t end;
	size_t low = 0;
	size_t high =
		sizeof(fat_casefold_ranges) / sizeof(fat_casefold_ranges[0]);

	/* Finds the first range that starts past this code point. */
	while (low < high) {
		/* Halves what is left of the table on every step. */
		middle = low + (high - low) / 2U;
		if (fat_casefold_ranges[middle].start <= scalar)
			low = middle + 1U;
		else
			high = middle;
	}

	/* The range in front of it is the one that may hold the point. */
	if (low != 0) {
		range = &fat_casefold_ranges[low - 1U];

		/* A range may cover every point, or every other one. */
		end = range->encoded_end & 0x7fffffffU;
		if (scalar <= end && (!(range->encoded_end & 0x80000000U) ||
				      ((scalar - range->start) & 1U) == 0)) {
			/* The folded point, as an offset from this one. */
			return (uint32_t)((int32_t)scalar + range->delta);
		}
	}

	/* A point in no range folds to itself. */
	return scalar;
}

/* Takes one code point out of a UTF-8 string. */
static int
decode_utf8(
	const uint8_t **cursor,
	uint32_t *scalar)
{
	uint8_t next;
	const uint8_t *p = *cursor;
	uint32_t value;
	unsigned count, i;

	/* A byte below the high half is a code point of its own. */
	if (*p < 0x80U) {
		*scalar = *p;
		*cursor = p + 1;
		/* A null ends the string rather than naming a point. */
		return *p != 0;
	}

	/* The leading byte says how many bytes follow it. */
	if (*p >= 0xc2U && *p <= 0xdfU) {
		value = *p & 0x1fU;
		count = 1;
	} else if (*p >= 0xe0U && *p <= 0xefU) {
		value = *p & 0x0fU;
		count = 2;
	} else if (*p >= 0xf0U && *p <= 0xf4U) {
		value = *p & 0x07U;
		count = 3;
	} else {
		/* Succeeded. */
		return 0;
	}

	/* Every continuation byte carries six more bits of the point. */
	for (i = 0; i < count; i++) {
		/* A byte that is not a continuation ends the sequence early. */
		next = p[i + 1U];
		if ((next & 0xc0U) != 0x80U)
			return 0;
		value = (value << 6) | (next & 0x3fU);
	}

	/* A two-byte sequence has to be the shortest form of its value. */
	if (count == 2 && value < 0x800U)
		return 0;	/* Failed. */

	/* And so does a three-byte one. */
	if (count == 3 && value < 0x10000U)
		return 0;	/* Failed. */

	/* No code point exists past this one. */
	if (value > 0x10ffffU)
		return 0;	/* Failed. */

	/* And half of a surrogate pair is not a code point of its own. */
	if (value >= 0xd800U && value <= 0xdfffU)
		return 0;	/* Failed. */

	/* Reports the code point and where the next sequence begins. */
	*scalar = value;
	*cursor = p + count + 1U;

	/* Succeeded. */
	return 1;
}

/* Renders a name as the UTF-16 units a long-name record stores. */
static int
fat_utf8_to_utf16(
	const char *name,
	uint16_t units[FAT_LFN_MAX_UNITS],
	unsigned *unit_count)
{
	uint32_t scalar;
	const uint8_t *cursor = (const uint8_t *)name;
	unsigned count = 0;
	int decoded;

	/* A call that names no name, or nowhere to report the units. */
	if (name == 0 || unit_count == 0 || !*cursor)
		return 0;

	/* Takes the name one code point at a time. */
	while (*cursor) {
		/* A separator or control character cannot be stored. */
		decoded = decode_utf8(&cursor, &scalar);
		if (!decoded || scalar == '/' ||
		    scalar < 0x20U || scalar == 0x7fU) {
			/* Succeeded. */
			return 0;
		}

		/* A point of the basic plane is stored as one unit. */
		if (scalar <= 0xffffU) {
			/* A name longer than a record chain could hold. */
			if (count >= FAT_LFN_MAX_UNITS)
				return 0;
			units[count++] = (uint16_t)scalar;
		} else {
			/* A point past that plane needs two units of room. */
			if (count + 2U > FAT_LFN_MAX_UNITS)
				return 0;
			scalar -= 0x10000U;
			units[count++] = (uint16_t)(0xd800U | (scalar >> 10));
			units[count++] =
				(uint16_t)(0xdc00U | (scalar & 0x3ffU));
		}
	}

	/* VFAT forbids trailing dot/space and these punctuation characters. */
	if (count == 0 || units[count - 1U] == '.' || units[count - 1U] == ' ')
		return 0;

	*unit_count = count;

	/* Succeeded. */
	return 1;
}

/* Builds one directory record of a long name. */
static void
fat_lfn_build_entry(
	uint8_t raw[32],
	const uint16_t *units,
	unsigned unit_count,
	unsigned ordinal,
	uint8_t checksum)
{
	unsigned index;
	uint16_t value;
	unsigned total = (unit_count + 12U) / 13U;
	unsigned i;

	/* An unused unit of a record is the value no code point can have. */
	for (i = 0; i < 32; i++)
		raw[i] = 0xffU;
	raw[0] = (uint8_t)ordinal;

	/* The last record of the name carries the mark that says so. */
	if (ordinal == total)
		raw[0] |= 0x40U;

	raw[11] = 0x0fU;
	raw[12] = 0;
	raw[13] = checksum;
	raw[26] = raw[27] = 0;

	/* Lays the thirteen units of this record into their three runs. */
	for (i = 0; i < 13; i++) {
		index = (ordinal - 1U) * 13U + i;
		if (index < unit_count)
			value = units[index];
		else if (index == unit_count)
			value = 0;
		else
			value = 0xffffU;
		raw[lfn_offsets[i]] = (uint8_t)value;
		raw[lfn_offsets[i] + 1U] = (uint8_t)(value >> 8);
	}
}

/* Tests whether a character may appear in a short file name. */
static int
sfn_character(
	uint8_t c)
{
	static const char punctuation[] = "$%\'-_@~`!(){}^#&";
	unsigned index;

	/* A short name is stored upper case, so lower case folds first. */
	if (c >= 'a' && c <= 'z')
		c -= 'a' - 'A';

	/* Letters are always allowed. */
	if (c >= 'A' && c <= 'Z')
		return 1;

	/* So are digits. */
	if (c >= '0' && c <= '9')
		return 1;

	/* The rest of the allowed set is a fixed list of punctuation. */
	for (index = 0; punctuation[index] != '\0'; index++) {
		if (c == (uint8_t)punctuation[index])
			return 1;
	}

	/* Reports that the character may not appear in a short name. */
	return 0;
}

/* Invents a short name for a long one, made unique by a serial. */
static int
fat_sfn_make_alias(
	const char *name,
	unsigned serial,
	uint8_t sfn[11])
{
	uint8_t character;
	const uint8_t *p = (const uint8_t *)name;
	const uint8_t *dot = 0, *q;
	uint8_t base[8], extension[3], digits[6];
	unsigned base_count = 0, extension_count = 0, digit_count = 0, i;
	int usable;

	/* The serial is what makes the alias unique, so it has to be one. */
	if (name == 0 || sfn == 0 || serial == 0 || serial > 999999U)
		return 0;

	/*
	 * The extension of a long name starts at its last dot, not its first.
	 */
	for (q = p; *q; q++) {
		if (*q == '.')
			dot = q;
	}

	/* Takes as much of the base as a short name is allowed to hold. */
	for (q = p; *q && q != dot; q++) {
		character = *q;

		/* A short name is stored in upper case. */
		if (character >= 'a' && character <= 'z')
			character -= 'a' - 'A';

		/* A character a short name cannot hold is simply dropped. */
		usable = sfn_character(character);
		if (usable && base_count < sizeof(base))
			base[base_count++] = character;
	}

	/* And the same for the extension, when the name has one. */
	if (dot != 0) {
		for (q = dot + 1; *q && extension_count < sizeof(extension);
		     q++) {
			character = *q;

			/* A short name is stored in upper case. */
			if (character >= 'a' && character <= 'z')
				character -= 'a' - 'A';

			/* A character a short name cannot hold is dropped. */
			usable = sfn_character(character);
			if (usable)
				extension[extension_count++] = character;
		}
	}

	/* A name of nothing usable still needs a base to hang the serial on. */
	if (base_count == 0) {
		base[0] = 'F';
		base[1] = 'I';
		base[2] = 'L';
		base[3] = 'E';
		base_count = 4;
	}

	/* Takes the digits of the serial, least significant one first. */
	while (serial) {
		digits[digit_count++] = (uint8_t)('0' + serial % 10U);
		serial /= 10U;
	}

	/* An unused position of a short name holds a space, not a null. */
	for (i = 0; i < 11; i++)
		sfn[i] = ' ';

	/* The base gives up whatever room the tilde and the digits need. */
	if (base_count > 7U - digit_count)
		base_count = 7U - digit_count;

	/*
	 * Lays down the base, the tilde that marks an alias, then the serial.
	 */
	for (i = 0; i < base_count; i++)
		sfn[i] = base[i];

	sfn[base_count++] = '~';

	/* The digits were taken in reverse, so they go back the other way. */
	while (digit_count)
		sfn[base_count++] = digits[--digit_count];

	/* And the extension keeps its own three positions at the end. */
	for (i = 0; i < extension_count; i++)
		sfn[8U + i] = extension[i];

	/* Succeeded. */
	return 1;
}

/* Compares two names without regard to case. */
static int
fat_utf8_casefold_equal(
	const char *left,
	const char *right)
{
	uint32_t left_scalar, right_scalar;
	const uint8_t *a = (const uint8_t *)left;
	const uint8_t *b = (const uint8_t *)right;
	int right_decoded;
	int left_decoded;
	uint32_t right_folded;
	uint32_t left_folded;

	/* A call that names nothing has nothing to compare. */
	if (left == NULL || right == NULL)
		return 0;

	/* Compares the two names one code point at a time. */
	while (*a && *b) {
		/* A point that will not decode, or folds differently. */
		left_decoded = decode_utf8(&a, &left_scalar);
		right_decoded = decode_utf8(&b, &right_scalar);
		if (!left_decoded || !right_decoded)
			return 0;

		/* Two points that fold to the same one are the same. */
		left_folded = fold_scalar(left_scalar);
		right_folded = fold_scalar(right_scalar);
		if (left_folded != right_folded) {
			/* Failed: the two names differ. */
			return 0;
		}
	}

	/* The names match only if both ran out at the same point. */
	if (*a != 0 || *b != 0)
		return 0;

	/* Reports that the two names are equal apart from case. */
	return 1;
}

/* Compares two strings without depending on a C library. */
static int
text_equal(
	const char *left,
	const char *right)
{
	/* Walks both strings while they agree. */
	while (*left && *left == *right) {
		left++;
		right++;
	}

	/* They are equal when they ended at the same place. */
	return *left == *right;
}

/* Copies a string into a bounded buffer, always terminating it. */
static void
text_copy(
	char *destination,
	const char *source,
	size_t capacity)
{
	/* A buffer with no room cannot even hold the terminator. */
	if (capacity == 0)
		return;

	/* Copies while there is both something to copy and room for it. */
	while (--capacity && *source)
		*destination++ = *source++;

	*destination = '\0';
}

/* Copies bytes without depending on a C library. */
static void
copy_bytes(
	void *destination,
	const void *source,
	uint32_t length)
{
	uint8_t *output = destination;
	const uint8_t *input = source;

	/* Copies the run one byte at a time, back to front. */
	while (length--)
		*output++ = *input++;
}

/* Fills a run of bytes with zero. */
static void
clear_bytes(
	void *destination,
	uint32_t length)
{
	uint8_t *output = destination;

	/* Clears the run one byte at a time, back to front. */
	while (length--)
		*output++ = 0;
}

/* Writes a 16-bit field, least significant byte first. */
static void
put16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/* Writes a 32-bit field, least significant byte first. */
static void
put32(
	uint8_t *bytes,
	uint32_t value)
{
	put16(bytes, (uint16_t)value);
	put16(bytes + 2, (uint16_t)(value >> 16));
}

/* Mounts a volume that has to be FAT16. */
static int
fat16_mount(
	struct fat_mount_state *filesystem)
{
	struct fat_mount_state *fat;
	int result;
	uint32_t fat_entries;

	/* Reads the layout and refuses a volume of another width. */
	result = fat_engine_mount(filesystem, KERN_FAT16);
	if (result != 0)
		return result;

	/* A FAT16 volume has a fixed root and a table that fits in memory. */
	fat = filesystem;
	if (!fat->root_entries || !fat->fat_sectors ||
	    fat->fat_sectors > 0xffffffffU / 512U) {
		/* Failed. */
		return EIO;
	}

	/* The table has to hold an entry for every cluster of the volume. */
	fat_entries = fat->fat_sectors * 512U / 2U;
	if (fat_entries < fat->cluster_count + 2U ||
	    fat->cluster_count + 2U >= FAT16_RESERVED_CLUSTER) {
		/* Failed. */
		return EIO;
	}

	/* Succeeded. */
	return 0;
}

/* Tests whether a cluster number names a cluster of this volume. */
static int
fat_raw_valid_cluster(
	const struct fat_mount_state *fat,
	uint32_t cluster)
{
	/* Clusters zero and one are reserved and name no storage. */
	if (cluster < 2U)
		return 0;

	/* Cluster numbers start at two, so the count is offset by two. */
	if (cluster >= fat->cluster_count + 2U)
		return 0;

	/* Reports that the number names a cluster this volume has. */
	return 1;
}

/* Tests whether a cluster number marks the end of a chain. */
static int
fat_raw_is_end(
	const struct fat_mount_state *fat,
	uint32_t cluster)
{
	uint32_t first_end;

	/* Each width reserves the top of its cluster range for end markers. */
	if (fat->type == KERN_FAT12)
		first_end = 0xff8U;
	else if (fat->type == KERN_FAT16)
		first_end = 0xfff8U;
	else
		first_end = 0x0ffffff8U;

	/* Anything at or above the first marker ends the chain. */
	if (cluster >= first_end)
		return 1;

	/* Reports that the cluster names a further link. */
	return 0;
}

/*
 * Reports the first cluster number a file may use.
 *
 * The numbers below it are reserved by the format for the media descriptor
 * and the dirty flags, and never name storage.
 */
static uint32_t
fat_raw_reserved_limit(
	const struct fat_mount_state *fat)
{
	/* Each width reserves a different number of entries. */
	if (fat->type == KERN_FAT12)
		return FAT12_RESERVED_CLUSTER;
	if (fat->type == KERN_FAT16)
		return FAT16_RESERVED_CLUSTER;

	/* Reports the limit of the only remaining width. */
	return FAT32_RESERVED_CLUSTER;
}

/* Reports the value this volume writes to terminate a cluster chain. */
static uint32_t
fat_raw_end_of_chain(
	const struct fat_mount_state *fat)
{
	/* Each width has its own end-of-chain value. */
	if (fat->type == KERN_FAT12)
		return FAT12_END_OF_CHAIN;
	if (fat->type == KERN_FAT16)
		return FAT16_END_OF_CHAIN;

	/* Reports the value of the only remaining width. */
	return FAT32_END_OF_CHAIN;
}

/* Reports the byte offset of a cluster's entry within the allocation table. */
static uint32_t
fat_raw_entry_offset(
	const struct fat_mount_state *fat,
	uint32_t cluster)
{
	/* A FAT12 entry is twelve bits, so every second one starts mid-byte. */
	if (fat->type == KERN_FAT12)
		return cluster + cluster / 2U;

	/* The wider formats hold two or four whole bytes per entry. */
	if (fat->type == KERN_FAT16)
		return cluster * 2U;

	/* Reports the offset of the only remaining width. */
	return cluster * 4U;
}

/* Reads the allocation table entry that follows one cluster. */
static int
fat_raw_next_cluster(
	struct fat_mount_state *filesystem,
	uint32_t cluster,
	uint32_t *next_cluster)
{
	uint8_t low;
	uint8_t high;
	uint32_t value;
	struct fat_mount_state *fat = filesystem;
	uint32_t offset;
	const uint8_t *sector;
	int result;
	int valid;

	io_stats_record(IO_FAT_CHAIN_STEP, 0);

	/* A cluster the volume has not got has no entry to read. */
	valid = fat_raw_valid_cluster(fat, cluster);
	if (!next_cluster || !valid)
		return EIO;

	offset = fat_raw_entry_offset(fat, cluster);

	/* Reads the sector of the table the entry falls in. */
	result = fat_engine_read_sector_result(
		filesystem, fat->fat_start + (offset >> 9), &sector);
	if (result != 0)
		return result;

	/* A FAT32 entry is a word, of which the top four bits are reserved. */
	if (fat->type == KERN_FAT32) {
		*next_cluster = fat_engine_get32(sector + (offset & 511U)) &
			0x0fffffffU;

		/* Succeeded. */
		return 0;
	}

	/* A FAT16 entry is a halfword and never crosses a sector. */
	if (fat->type == KERN_FAT16) {
		*next_cluster = fat_engine_get16(sector + (offset & 511U));

		/* Succeeded. */
		return 0;
	}

	/*
	 * A 12-bit entry may straddle a sector boundary, and the
	 * sector cache holds one sector, so latch the first byte
	 * before a second read can evict it.
	 */
	low = sector[offset & 511U];

	/* A FAT12 entry may straddle two sectors, so the next is read too. */
	if ((offset & 511U) == 511U) {
		/* Reads the sector the second half of the entry falls in. */
		result = fat_engine_read_sector_result(
			filesystem, fat->fat_start + (offset >> 9) + 1U,
			&sector);
		if (result != 0)
			return result;
		high = sector[0];
	} else {
		high = sector[(offset & 511U) + 1U];
	}

	value = (uint32_t)low | ((uint32_t)high << 8);
	*next_cluster = (cluster & 1U) ? value >> 4 : value & 0xfffU;

	/* Succeeded. */
	return 0;
}

/* Counts the clusters the allocation table still calls free. */
static int
fat_engine_count_free_clusters(
	struct fat_mount_state *filesystem,
	uint32_t *free_clusters)
{
	uint32_t value;
	int result;
	struct fat_mount_state *fat;
	uint32_t cluster, count = 0;

	/* A call that names no volume, or nowhere to report the count. */
	if (filesystem == NULL || free_clusters == NULL)
		return EINVAL;

	/* A volume with no clusters has none that could be free. */
	fat = filesystem;
	if (fat == NULL || fat->cluster_count == 0)
		return EIO;

	/* Walks every cluster the volume has, in order. */
	for (cluster = 2U; cluster < fat->cluster_count + 2U; cluster++) {
		/* Reads the table entry that belongs to this cluster. */
		result = fat_raw_next_cluster(filesystem, cluster, &value);
		if (result != 0)
			return result;

		/* An entry of zero is what marks a cluster as free. */
		if (value == 0)
			count++;
	}

	*free_clusters = count;

	/* Succeeded. */
	return 0;
}

/* Changes selected bits of one byte of one copy of the table. */
static int
fat_raw_set_entry_byte(
	struct fat_mount_state *filesystem,
	uint32_t copy_start,
	uint32_t offset,
	uint8_t keep_mask,
	uint8_t merge_value)
{
	uint8_t *sector;
	int result;

	/* An offset that would run past the end of this copy of the table. */
	if ((offset >> 9) > 0xffffffffU - copy_start)
		return EIO;

	/* Reads the sector so the one byte can be changed in place. */
	result = fat_engine_write_sector_result(filesystem,
						copy_start + (offset >> 9),
						&sector);
	if (result != 0)
		return result;

	sector[offset & 511U] =
		(uint8_t)((sector[offset & 511U] & keep_mask) | merge_value);

	/* The change only reaches the volume once the sector is written. */
	result = fat_engine_mark_sector_dirty(filesystem);
	if (result == 0)
		result = fat_engine_flush(filesystem);

	/* Failed. */
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Writes one cluster's entry into one copy of the allocation table. */
static int
fat_raw_set_cluster_copy(
	struct fat_mount_state *filesystem,
	uint32_t cluster,
	uint32_t value,
	unsigned copy)
{
	uint8_t *entry;
	uint32_t old;
	uint8_t *sector;
	struct fat_mount_state *fat = filesystem;
	uint32_t offset;
	uint32_t copy_start;
	int result;
	int valid;

	/* A cluster the volume has not got, or a copy it does not keep. */
	valid = fat_raw_valid_cluster(fat, cluster);
	if (!valid || copy >= fat->number_of_fats)
		return EIO;

	offset = fat_raw_entry_offset(fat, cluster);

	/* A copy whose start would run past the end of the volume. */
	if (copy > (0xffffffffU - fat->fat_start) / fat->fat_sectors)
		return EIO;

	/* A FAT16 or FAT32 entry never straddles two sectors. */
	copy_start = fat->fat_start + copy * fat->fat_sectors;
	if (fat->type != KERN_FAT12) {
		/* An offset that would run past the end of this copy. */
		if ((offset >> 9) > 0xffffffffU - copy_start)
			return EIO;

		/* Reads the sector so the entry can be changed in place. */
		result = fat_engine_write_sector_result(
			filesystem, copy_start + (offset >> 9), &sector);
		if (result != 0)
			return result;

		/* A FAT32 entry keeps the top four bits it already had. */
		if (fat->type == KERN_FAT32) {
			entry = sector + (offset & 511U);
			old = fat_engine_get32(entry);
			put32(entry,
			      (old & 0xf0000000U) | (value & 0x0fffffffU));
		} else {
			put16(sector + (offset & 511U), (uint16_t)value);
		}

		/* The change reaches the volume once the sector is written. */
		result = fat_engine_mark_sector_dirty(filesystem);
		if (result == 0)
			result = fat_engine_flush(filesystem);

		/* Failed. */
		return result;
	}

	/*
	 * Read-modify-write both bytes of the packed 12-bit entry; they may
	 * live in different sectors.
	 */
	if (cluster & 1U) {
		/* An even entry is the low byte and the nibble above it. */
		result = fat_raw_set_entry_byte(filesystem, copy_start, offset,
						0x0f,
						(uint8_t)((value << 4) & 0xf0));
		if (result == 0) {
			result = fat_raw_set_entry_byte(filesystem, copy_start,
							offset + 1U, 0x00,
							(uint8_t)(value >> 4));
		}
	} else {
		/* An odd one is the high nibble and the whole byte above it. */
		result = fat_raw_set_entry_byte(filesystem, copy_start, offset,
						0x00, (uint8_t)value);
		if (result == 0) {
			result = fat_raw_set_entry_byte(
				filesystem, copy_start, offset + 1U, 0xf0,
				(uint8_t)((value >> 8) & 0x0f));
		}
	}

	/* Failed. */
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Capture a mirror's entry without assuming that every copy already agrees. */
static int
fat_raw_get_cluster_copy(
	struct fat_mount_state *filesystem,
	uint32_t cluster,
	unsigned copy,
	uint32_t *value)
{
	const uint8_t *sector;
	uint32_t offset;
	uint32_t lba;
	uint8_t low;
	uint8_t high;
	int error;

	/*
	 * Read the selected copy through the ordinary coherent sector window.
	 */
	offset = fat_raw_entry_offset(filesystem, cluster);

	/* A copy whose start would run past the end of the volume. */
	if (copy >
	    (UINT32_MAX - filesystem->fat_start) / filesystem->fat_sectors) {
		/* Failed. */
		return EIO;
	}

	/* Which sector of that copy the entry falls in. */
	lba = filesystem->fat_start + copy * filesystem->fat_sectors;
	if ((offset >> 9) > UINT32_MAX - lba)
		return EIO;
	lba += offset >> 9;

	/* Reads that sector of the table. */
	error = fat_engine_read_sector_result(filesystem, lba, &sector);
	if (error != 0)
		return error;

	/*
	 * Decode aligned entries and retain each copy's reserved high bits on
	 * rewrite.
	 */
	if (filesystem->type == KERN_FAT32) {
		*value = fat_engine_get32(sector + (offset & 511U)) &
			 0x0fffffffU;

		/* Succeeded. */
		return 0;
	}

	/* A FAT16 entry is a halfword and never crosses a sector. */
	if (filesystem->type == KERN_FAT16) {
		*value = fat_engine_get16(sector + (offset & 511U));

		/* Succeeded. */
		return 0;
	}

	/*
	 * Preserve the first packed byte before switching a sector-boundary
	 * window.
	 */
	low = sector[offset & 511U];

	/* A FAT12 entry may straddle two sectors. */
	if ((offset & 511U) == 511U) {
		/* A second sector past the last one the volume can address. */
		if (lba == UINT32_MAX)
			return EIO;

		/* Reads the sector the entry continues in. */
		error = fat_engine_read_sector_result(filesystem,
						      lba + 1U,
						      &sector);
		if (error != 0)
			return error;

		high = sector[0];
	} else {
		high = sector[(offset & 511U) + 1U];
	}

	*value = (uint32_t)low | ((uint32_t)high << 8);
	*value = (cluster & 1U) ? *value >> 4 : *value & 0xfffU;

	/* Succeeded. */
	return 0;
}

/*
 * Writes one table entry to every copy, undoing the change if any copy fails.
 *
 * The copies must agree, so the old values are captured first and written
 * back when a later copy or its flush reports a failure.  A rollback that
 * itself fails leaves the volume read-only, because the copies then disagree
 * and nothing here can tell which one a later mount would believe.
 */
static int
fat_raw_set_cluster_immediate(
	struct fat_mount_state *filesystem,
	uint32_t cluster,
	uint32_t value)
{
	uint32_t old_values[256];
	unsigned copy;
	int error;
	int rollback;
	int restore;
	int valid;

	/* Refuses a cluster number that names no entry. */
	/* Asks whether that is a cluster the volume has. */
	valid = fat_raw_valid_cluster(filesystem, cluster);
	if (!valid)
		return EIO;

	/* Captures every old entry before changing any copy. */
	for (copy = 0; copy < filesystem->number_of_fats; copy++) {
		error = fat_raw_get_cluster_copy(filesystem,
						 cluster,
						 copy,
						 &old_values[copy]);
		if (error != 0)
			return error;
	}

	/* Puts the volume on disk in the state the capture just recorded. */
	error = fat_engine_flush(filesystem);
	if (error == 0)
		error = disk_sync(filesystem->disk);
	if (error != 0)
		return error;

	/* Every cached chain was measured against the table about to change. */
	fat_chain_invalidate(filesystem);

	/*
	 * Preserve initialization-before-link and confirm all mirror updates.
	 */
	for (copy = 0; copy < filesystem->number_of_fats; copy++) {
		error = fat_raw_set_cluster_copy(filesystem,
						 cluster,
						 value,
						 copy);
		if (error != 0)
			break;
	}

	/* Makes the change durable before reporting that it happened. */
	if (error == 0)
		error = disk_sync(filesystem->disk);

	/* Succeeded: every copy carries the new value. */
	if (error == 0)
		return 0;

	/* Restore even the copy whose write or final flush reported failure. */
	rollback = 0;
	for (copy = 0; copy < filesystem->number_of_fats; copy++) {
		restore = fat_raw_set_cluster_copy(filesystem,
						   cluster,
						   old_values[copy],
						   copy);
		if (rollback == 0 && restore != 0)
			rollback = restore;
	}

	/* The restored values are only back once they reach the disk. */
	restore = disk_sync(filesystem->disk);
	if (rollback == 0)
		rollback = restore;

	/* Copies that disagree cannot be written to again. */
	if (rollback != 0)
		filesystem->read_only = 1;

	fat_engine_invalidate(filesystem);

	/* Reports the failed rollback, or the write that made it necessary. */
	if (rollback != 0)
		return rollback;

	return error;
}

/* Admit every affected physical sector before making any table changes. */
static int
fat_batch_add_lba(
	uint32_t *lbas,
	unsigned *used,
	uint32_t lba)
{
	unsigned n;

	/* Reuse one image for all changes that share a physical sector. */
	for (n = 0; n < *used; n++) {
		/* A sector already in the list is not added to it twice. */
		if (lbas[n] == lba)
			return 0;
	}

	/*
	 * Ask the caller to split before exhausting bounded transaction
	 * storage.
	 */
	if (*used == FAT_BATCH_SECTORS)
		return E2BIG;

	lbas[(*used)++] = lba;

	/* Succeeded. */
	return 0;
}

/* Find the private replacement image admitted during preflight. */
static uint8_t *
fat_batch_bytes(
	struct fat_sector_change *slots,
	unsigned used,
	uint32_t lba)
{
	unsigned n;

	/*
	 * Locate the exact physical identity without aliasing the mutable
	 * window.
	 */
	for (n = 0; n < used; n++) {
		/* Finds the staged copy of one sector by its address. */
		if (slots[n].lba == lba)
			return slots[n].new_bytes;
	}

	/*
	 * Report an inconsistent preflight rather than dereference a missing
	 * image.
	 */
	return NULL;
}

/*
 * Publish complete sector images with optional final directory-name ordering.
 */
static int
fat_sector_images_commit(
	struct fat_mount_state *filesystem,
	struct fat_sector_change *slots,
	unsigned used,
	uint32_t publish_lba)
{
	unsigned n;
	int error;
	int rollback;
	int restored;

	/*
	 * Make initialization or directory detachment durable before changing
	 * reachability.
	 */
	error = disk_sync(filesystem->disk);
	if (error != 0) {
		return error;
	}

	fat_engine_invalidate(filesystem);

	/* The whole batch is written inside one write epoch. */
	if (filesystem->owner != NULL)
		io_epoch_begin(&filesystem->owner->m_write_epoch);

	/* Publish every mirror and confirm the complete table transaction. */
	for (n = 0; n < used; n++) {
		/* The sector that publishes the change is written last. */
		if (slots[n].lba == publish_lba)
			continue;

		/* Writes one mirror of the table out. */
		error = fat_sector_write(filesystem, slots[n].lba,
					 slots[n].new_bytes);
		if (error != 0)
			break;
	}

	/*
	 * Persist preceding LFN sectors before their final short-name
	 * publication.
	 */
	if (error == 0 && publish_lba != UINT32_MAX) {
		/* Every mirror is made durable before publishing. */
		if (used > 1)
			error = disk_sync(filesystem->disk);
		if (error == 0) {
			error = fat_sector_write(
				filesystem, publish_lba,
				fat_batch_bytes(slots, used, publish_lba));
		}
	}

	/* And the publishing write has to be durable in its turn. */
	if (error == 0)
		error = disk_sync(filesystem->disk);

	/*
	 * Restore all captured sectors, including writes that may have
	 * completed with error.
	 */
	rollback = 0;

	/* A batch that failed puts every sector back the way it was. */
	if (error != 0) {
		/* Walks the sectors this batch had captured. */
		for (n = 0; n < used; n++) {
			/* Writes the bytes it held before the batch. */
			restored = fat_sector_write(filesystem, slots[n].lba,
						    slots[n].old_bytes);
			if (rollback == 0 && restored != 0)
				rollback = restored;
		}

		/* The restored sectors have to reach the volume as well. */
		restored = disk_sync(filesystem->disk);
		if (rollback == 0)
			rollback = restored;

		/* A volume that cannot be restored stops being written. */
		if (rollback != 0)
			filesystem->read_only = 1;
	}

	fat_engine_invalidate(filesystem);

	/* The write epoch this batch opened is now closed again. */
	if (filesystem->owner != NULL)
		io_epoch_end(&filesystem->owner->m_write_epoch);

	/*
	 * Preserve failure and freeze the mount when restoration is uncertain.
	 */
	if (rollback != 0)
		return rollback;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Commit bounded FAT sector images and restore every mirror on uncertain
 * errors.
 */
static int
fat_table_transaction(
	struct fat_mount_state *filesystem,
	const struct fat_entry_change *changes,
	unsigned count,
	int *admitted)
{
	struct fat_sector_change *slots;
	uint32_t lbas[FAT_BATCH_SECTORS];
	uint32_t offset;
	uint32_t copy_start;
	uint32_t value;
	uint8_t *first;
	uint8_t *second;
	unsigned used;
	unsigned n;
	unsigned copy;
	unsigned entry;
	unsigned bytes;
	int error;
	int valid;

	/*
	 * Validate the complete bounded operation before any write or
	 * allocation.
	 */
	*admitted = 0;

	/* A volume mounted read-only is never written to. */
	if (filesystem->read_only)
		return EROFS;

	/* A batch of no changes has nothing to write. */
	if (count == 0)
		return 0;

	/* And one larger than the staging holds cannot be made at once. */
	if (count > FAT_BATCH_ENTRIES)
		return E2BIG;

	used = 0;
	bytes = filesystem->type == KERN_FAT32 ? 4U : 2U;

	/* Collects the sectors every change of the batch will touch. */
	for (entry = 0; entry < count; entry++) {
		/* A cluster the volume has not got has no entry to change. */
		valid = fat_raw_valid_cluster(filesystem,
					      changes[entry].cluster);
		if (!valid)
			return EIO;

		offset = fat_raw_entry_offset(filesystem,
			changes[entry].cluster);

		/* Every copy of the table holds the same entry. */
		for (copy = 0; copy < filesystem->number_of_fats; copy++) {
			/* A copy starting past the end of the volume. */
			if (copy > (UINT32_MAX - filesystem->fat_start) /
			    filesystem->fat_sectors) {
				/* Failed. */
				return EIO;
			}

			/* Nor may the entry run past the end of that copy. */
			copy_start = filesystem->fat_start +
				copy * filesystem->fat_sectors;
			if (((uint64_t)offset + bytes - 1U) / 512U >
			    UINT32_MAX - copy_start) {
				/* Failed. */
				return EIO;
			}

			/* The sector the entry begins in. */
			error = fat_batch_add_lba(lbas, &used,
				copy_start + offset / 512U);
			if (error != 0)
				return error;

			/* And the one it ends in, which FAT12 may differ in. */
			error = fat_batch_add_lba(lbas,
						  &used,
						  copy_start +
						  (offset + bytes - 1U) /
						  512U);
			if (error != 0)
				return error;
		}
	}

	/*
	 * Decline admission without side effects when working memory is
	 * unavailable.
	 */

	/* Takes the staging the whole batch is assembled in. */
	slots = kern_malloc(used * sizeof(*slots));
	if (slots == NULL)
		return ENOMEM;

	*admitted = 1;

	/* Nothing cached may survive the sectors being written under it. */
	error = fat_engine_flush(filesystem);
	if (error != 0) {
		kern_free(slots);

		/* Failed. */
		return error;
	}

	/*
	 * Preserve each mirror's actual old bytes, including unrelated packed
	 * entries.
	 */
	for (n = 0; n < used; n++) {
		slots[n].lba = lbas[n];

		/* Reads what each sector held, so the batch can be undone. */
		error = fat_sector_read(filesystem, lbas[n],
					slots[n].old_bytes);
		if (error != 0) {
			kern_free(slots);

			/* Failed. */
			return error;
		}

		memcpy(slots[n].new_bytes, slots[n].old_bytes, 512);
	}

	/*
	 * Merge replacements into the private sector images without changing
	 * neighbors.
	 */
	for (entry = 0; entry < count; entry++) {
		offset = fat_raw_entry_offset(filesystem,
			changes[entry].cluster);
		value = changes[entry].value;

		/* A FAT12 entry is two bytes that may lie in two sectors. */
		for (copy = 0; copy < filesystem->number_of_fats; copy++) {
			copy_start = filesystem->fat_start +
				copy * filesystem->fat_sectors;
			first = fat_batch_bytes(slots, used,
				copy_start + offset / 512U);

			/* Both of those sectors have to be in the batch. */
			second = fat_batch_bytes(slots, used,
				copy_start + (offset + 1U) / 512U);
			if (first == NULL || second == NULL) {
				kern_free(slots);

				/* Failed. */
				return EIO;
			}

			first += offset & 511U;
			second += (offset + 1U) & 511U;

			/* An entry is as wide as the volume format. */
			if (filesystem->type == KERN_FAT32) {
				put32(first,
				      (fat_engine_get32(first) & 0xf0000000U) |
				      (value & 0x0fffffffU));
			} else if (filesystem->type == KERN_FAT16) {
				put16(first, (uint16_t)value);
			} else if (changes[entry].cluster & 1U) {
				*first = (uint8_t)((*first & 0x0fU) |
						   ((value << 4) & 0xf0U));
				*second = (uint8_t)(value >> 4);
			} else {
				*first = (uint8_t)value;
				*second = (uint8_t)((*second & 0xf0U) |
						    ((value >> 8) & 0x0fU));
			}
		}
	}

	/*
	 * Publish the fully staged table and release its private working
	 * images.
	 */
	error = fat_sector_images_commit(filesystem, slots, used, UINT32_MAX);
	kern_free(slots);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Preserve the immediate fallback with the same required durability boundaries.
 */
static int
fat_raw_set_cluster(
	struct fat_mount_state *filesystem,
	uint32_t cluster,
	uint32_t value)
{
	struct fat_entry_change change;
	int error;
	int admitted;

	/*
	 * Prefer one bounded transaction, including both bytes of packed FAT12
	 * entries.
	 */
	change.cluster = cluster;
	change.value = value;

	/* Writes the one entry as a batch of one. */
	error = fat_table_transaction(filesystem, &change, 1, &admitted);
	if (admitted || (error != ENOMEM && error != E2BIG))
		return error;

	/*
	 * Retain all supported mirror counts and allocation-free failure
	 * recovery.
	 */

	/* Report the original update result without leaving deferred state. */
	error = fat_raw_set_cluster_immediate(filesystem, cluster, value);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Publish a newly initialized cluster together with a same-sector tail link. */
static int
fat_link_initialized_cluster(
	struct fat_mount_state *filesystem,
	uint32_t tail,
	uint32_t added)
{
	struct fat_entry_change changes[2];
	uint32_t first_offset;
	uint32_t second_offset;
	unsigned bytes;
	int admitted;
	int error;
	int rollback;

	/*
	 * Combine only changes that cannot expose a link to a separate pending
	 * sector.
	 */
	first_offset = fat_raw_entry_offset(filesystem, tail);
	second_offset = fat_raw_entry_offset(filesystem, added);

	/* Two entries may be written together when both fit two sectors. */
	bytes = filesystem->type == KERN_FAT32 ? 4U : 2U;
	if (first_offset / 512U == second_offset / 512U &&
	    first_offset / 512U == (first_offset + bytes - 1U) / 512U &&
	    second_offset / 512U == (second_offset + bytes - 1U) / 512U) {
		changes[0].cluster = added;
		changes[0].value = fat_raw_end_of_chain(filesystem);
		changes[1].cluster = tail;
		changes[1].value = added;

		/* Writes the two entries as one batch. */
		error = fat_table_transaction(filesystem, changes, 2,
					      &admitted);
		if (admitted || (error != ENOMEM && error != E2BIG))
			return error;
	}

	/*
	 * Make a cross-sector new endpoint durable before publishing its tail
	 * link.
	 */
	error = fat_raw_set_cluster(filesystem,
				    added,
				    fat_raw_end_of_chain(filesystem));
	if (error != 0)
		return error;

	/* Otherwise the two are written one after the other. */
	error = fat_raw_set_cluster(filesystem, tail, added);
	if (error == 0)
		return 0;

	/*
	 * Recycle the new endpoint only after the failed link has been
	 * restored.
	 */
	rollback = fat_raw_set_cluster(filesystem, added, 0);
	if (rollback != 0) {
		filesystem->read_only = 1;

		/* Failed. */
		return rollback;
	}

	/* Preserve the original tail-link failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Merge a bounded directory run while preserving neighboring and hidden slots.
 */
static int
fat_directory_transaction(
	struct fat_mount_state *filesystem,
	const uint32_t *lbas,
	const uint16_t *offsets,
	const uint8_t entries[][32],
	unsigned count,
	int publish_last)
{
	struct fat_sector_change *slots;
	uint32_t identities[FAT_BATCH_SECTORS];
	uint32_t publish_lba;
	uint8_t *bytes;
	unsigned used;
	unsigned n;
	int error;

	/*
	 * Validate and admit the complete directory run before touching its
	 * contents.
	 */
	if (filesystem->read_only)
		return EROFS;

	/* A batch of no records, or of more than a name could span. */
	if (count == 0 || count > FAT_LFN_MAX_ENTRIES + 1U)
		return EINVAL;

	used = 0;

	/* Collects the sectors every record of the batch will touch. */
	for (n = 0; n < count; n++) {
		/* A record sits on a 32-byte boundary inside its sector. */
		if (offsets[n] > 512U - 32U || (offsets[n] & 31U) != 0)
			return EINVAL;

		/* Adds the sector this record falls in, once. */
		error = fat_batch_add_lba(identities, &used, lbas[n]);
		if (error != 0)
			return error;
	}

	/* Takes the staging the whole batch is assembled in. */
	slots = kern_malloc(used * sizeof(*slots));
	if (slots == NULL)
		return ENOMEM;

	/* Nothing cached may survive the sectors written under it. */
	error = fat_engine_flush(filesystem);
	if (error != 0) {
		kern_free(slots);

		/* Failed. */
		return error;
	}

	/*
	 * Capture full old sectors, including data behind the original end
	 * marker.
	 */
	for (n = 0; n < used; n++) {
		slots[n].lba = identities[n];

		/* Reads what each sector held, so the batch can be undone. */
		error = fat_sector_read(filesystem, identities[n],
					slots[n].old_bytes);
		if (error != 0) {
			kern_free(slots);

			/* Failed. */
			return error;
		}

		memcpy(slots[n].new_bytes, slots[n].old_bytes, 512U);
	}

	/* Lays every record of the batch into its staged sector. */
	for (n = 0; n < count; n++) {
		/* Every record's sector has to be one the batch captured. */
		bytes = fat_batch_bytes(slots, used, lbas[n]);
		if (bytes == NULL) {
			kern_free(slots);

			/* Failed. */
			return EIO;
		}

		memcpy(bytes + offsets[n], entries[n], 32U);
	}

	/*
	 * Publish the short-name sector last when creating a multi-sector LFN
	 * run.
	 */
	/* The record that publishes the change is written last of all. */
	publish_lba = UINT32_MAX;
	if (publish_last)
		publish_lba = lbas[count - 1U];

	error = fat_sector_images_commit(filesystem, slots, used, publish_lba);
	kern_free(slots);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* End consolidated fat-batch.inc. */
static uint32_t
fat_raw_dir_cluster(
	const struct fat_mount_state *fat,
	const uint8_t raw[32])
{
	uint32_t cluster = fat_engine_get16(raw + 26);

	/* A FAT32 record keeps the top half of the number apart. */
	if (fat->type == KERN_FAT32)
		cluster |= (uint32_t)fat_engine_get16(raw + 20) << 16;

	/* The cluster number, of which four bits are reserved. */
	return cluster & 0x0fffffffU;
}

/* Stores a cluster number into the two halves a record keeps it in. */
static void
fat_raw_put_dir_cluster(
	const struct fat_mount_state *fat,
	uint8_t raw[32],
	uint32_t cluster)
{
	put16(raw + 26, (uint16_t)cluster);

	/* A FAT32 record keeps the top half of the number apart. */
	if (fat->type == KERN_FAT32)
		put16(raw + 20, (uint16_t)(cluster >> 16));
}

/* Reports which cluster the root directory begins at. */
static uint32_t
fat_raw_root_cluster(
	const struct fat_mount_state *fat)
{
	/* FAT32 puts its root in an ordinary cluster chain. */
	if (fat->type == KERN_FAT32)
		return fat->root_cluster;

	/* The narrower widths use a fixed table, which cluster zero denotes. */
	return 0;
}

/* Validates the whole chain before publishing a reusable position. */
static int
fat_raw_validate_chain_at(
	struct fat_mount_state *filesystem,
	uint32_t first_cluster,
	uint32_t wanted_index,
	struct fat_chain_cursor *cursor,
	uint32_t *last_cluster)
{
	return fat_raw_validate_chain_count(filesystem, first_cluster,
		wanted_index, cursor, last_cluster, NULL);
}

/* Validates the entire chain and optionally reports its owned capacity. */
static int
fat_raw_validate_chain_count(
	struct fat_mount_state *filesystem,
	uint32_t first_cluster,
	uint32_t wanted_index,
	struct fat_chain_cursor *cursor,
	uint32_t *last_cluster,
	uint32_t *allocated_clusters)
{
	uint32_t next;
	int result;
	struct fat_mount_state *fat = filesystem;
	uint32_t cluster = first_cluster, checkpoint = first_cluster;
	uint32_t steps, span = 0U, power = 1U;
	int last;
	int valid;

	/* A chain starting at a cluster the volume has not got. */
	/* Asks whether that is a cluster the volume has. */
	valid = fat_raw_valid_cluster(fat, first_cluster);
	if (!valid)
		return EIO;

	/* A cursor that is not filled in stands at no cluster. */
	if (cursor != NULL)
		cursor->cluster = 0U;

	/* Walks the chain, refusing to take more steps than it can have. */
	for (steps = 0; steps <= fat->cluster_count; steps++) {
		/* Remembers where the wanted position falls, going past. */
		if (cursor != NULL && steps == wanted_index) {
			cursor->index = steps;
			cursor->cluster = cluster;
		}

		/*
		 * Brent's single forward cursor retains bounded cycle detection
		 * without alternating distant FAT sectors on every link.  That
		 * alternation defeats this mount's one-sector cache for large
		 * loop backing files, even when the block cache already holds
		 * the FAT.
		 */

		/* Reads the entry that follows this cluster. */
		result = fat_raw_next_cluster(filesystem, cluster, &next);
		if (result != 0)
			return result;

		/* The end-of-chain marker is where the file stops. */
		/* Asks whether the entry is the end-of-chain marker. */
		last = fat_raw_is_end(fat, next);
		if (last) {
			/* Counts owned clusters, including allocation beyond EOF. */
			if (allocated_clusters != NULL)
				*allocated_clusters = steps + 1U;

			/* Reports the last cluster, if the caller asked. */
			if (last_cluster != NULL)
				*last_cluster = cluster;

			/*
			 * A growing write may begin beyond the old EOF.  Its
			 * seek can continue from this validated tail instead of
			 * the first link.
			 */
			if (cursor != NULL && cursor->cluster == 0U) {
				cursor->index = steps;
				cursor->cluster = cluster;
			}

			/* Succeeded. */
			return 0;
		}

		/* A chain that leaves the volume means the file is corrupt. */
		/* Asks whether that is a cluster the volume has. */
		valid = fat_raw_valid_cluster(fat, next);
		if (!valid)
			return EIO;
		cluster = next;
		span++;

		/* Meeting the checkpoint again means the chain loops. */
		if (cluster == checkpoint)
			return EIO;

		/* The checkpoint moves on after twice as many steps. */
		if (span == power) {
			checkpoint = cluster;
			span = 0U;

			/* The step count stops doubling before it overflows. */
			if (power <= UINT32_MAX / 2U)
				power *= 2U;
		}
	}

	/* Failed. */
	return EIO;
}

/* Reuses only the next sequential position in an already validated chain. */
static int
fat_file_validate_at(
	struct fat_file_state *file,
	uint64_t offset,
	struct fat_chain_cursor *cursor,
	uint32_t *last)
{
	uint32_t wanted;
	int reusable;
	int error;

	/* Which cluster of the file the wanted offset falls in. */
	wanted = (uint32_t)(offset /
			    ((uint64_t)file->mount->sectors_per_cluster *
			     512U));

	/*
	 * The cached cursor may be reused when it was left valid, when it
	 * stands at this exact offset, when the chain has not been rebuilt
	 * since it was taken, when it belongs to this file's own chain, when
	 * it has not already walked past the cluster wanted, and when the
	 * cluster it stands on is still one the volume has.
	 */
	reusable = 0;
	if (file->cursor_valid && file->cursor_offset == offset &&
	    file->cursor_generation == file->mount->chain_generation &&
	    file->cursor_generation != UINT64_MAX &&
	    file->cursor_first == file->first_cluster &&
	    file->chain_cursor.index <= wanted)
		reusable = fat_raw_valid_cluster(file->mount,
						 file->chain_cursor.cluster);

	if (reusable) {
		*cursor = file->chain_cursor;
		*last = file->cursor_last;
		file->cursor_valid = 0;
		io_stats_record(IO_FAT_CURSOR_HIT, 0);

		/* Succeeded. */
		return 0;
	}

	/*
	 * Consumes the old proof before fallible validation or subsequent I/O.
	 */
	file->cursor_valid = 0;

	/* Reports the failure. */
	error = fat_raw_validate_chain_at(file->mount,
					  file->first_cluster,
					  wanted,
					  cursor,
					  last);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Publishes a successful operation only if its validated chain stayed
 * unchanged.
 */
static void
fat_file_save_cursor(
	struct fat_file_state *file,
	const struct fat_chain_cursor *cursor,
	uint64_t end,
	uint64_t generation,
	uint32_t last)
{
	file->cursor_valid = 0;

	/* A cursor of another generation would resume in the wrong place. */
	if (generation == UINT64_MAX ||
	    generation != file->mount->chain_generation ||
	    cursor->cluster == 0) {
		/* Nothing is remembered. */
		return;
	}

	/* Records where the walk stopped so the next one resumes there. */
	file->chain_cursor = *cursor;
	file->cursor_generation = generation;
	file->cursor_offset = end;
	file->cursor_first = file->first_cluster;
	file->cursor_last = last;
	file->cursor_valid = 1;
}

/* Walks a cluster chain to its end, refusing one that loops. */
static int
fat_raw_validate_chain(
	struct fat_mount_state *filesystem,
	uint32_t first_cluster)
{
	int error;

	/* Walks the whole chain, which is what proves it terminates. */
	error = fat_raw_validate_chain_at(filesystem,
					  first_cluster,
					  0U,
					  NULL,
					  NULL);

	/* Reports whether the chain holds together. */
	return error;
}

/* Gives every cluster of a chain back to the allocation table. */
static int
fat_raw_free_chain(
	struct fat_mount_state *filesystem,
	uint32_t first_cluster)
{
	uint32_t next;
	uint32_t link;
	int restore_error;
	uint32_t restore;
	int rollback;
	struct fat_mount_state *fat = filesystem;
	uint32_t *clusters;
	uint32_t cluster = first_cluster, count = 0, index;
	int result;
	int admitted;
	unsigned batch, item;
	struct fat_entry_change changes[FAT_BATCH_ENTRIES];
	int last;

	/* A file that never had a cluster has no chain to free. */
	if (!first_cluster)
		return 0;

	/* A chain that does not hold together must not be freed. */
	result = fat_raw_validate_chain(filesystem, first_cluster);
	if (result != 0)
		return result;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Reads the entry that follows this cluster. */
		result = fat_raw_next_cluster(filesystem, cluster, &next);
		if (result != 0)
			return result;

		/* More steps than the volume has clusters means it loops. */
		if (++count > fat->cluster_count)
			return EIO;

		/* The end-of-chain marker is where the file stops. */
		last = fat_raw_is_end(fat, next);
		if (last)
			break;

		cluster = next;
	}

#if SIZE_MAX <= UINT32_MAX

	/* A chain longer than the staging could be allocated for. */
	if (count > (uint32_t)(SIZE_MAX / sizeof(*clusters)))
		return ENOMEM;
#endif

	/* Takes the staging the whole chain is collected in. */
	clusters = kern_malloc((size_t)count * sizeof(*clusters));
	if (clusters == NULL)
		return ENOMEM;

	/* Collects the chain before any of it is given back. */
	cluster = first_cluster;
	for (index = 0; index < count; index++) {
		clusters[index] = cluster;
		result = fat_raw_next_cluster(filesystem, cluster, &next);
		if (result != 0)
			goto out;

		cluster = next;
	}

	/* Frees the chain a bounded batch at a time. */
	index = 0;
	while (index < count) {
		/*
		 * Stage a bounded free prefix while retaining the complete
		 * recovery chain.
		 */
		batch = count - index;
		if (batch > FAT_BATCH_ENTRIES)
			batch = FAT_BATCH_ENTRIES;

		/* Every entry of this batch is set to the free value. */
		for (item = 0; item < batch; item++) {
			changes[item].cluster = clusters[index + item];
			changes[item].value = 0;
		}

		result = fat_table_transaction(filesystem,
					       changes,
					       batch,
					       &admitted);

		/*
		 * Split before admission when scattered sectors exceed the
		 * image budget.
		 */
		while (!admitted && result == E2BIG && batch > 1) {
			batch /= 2;
			result = fat_table_transaction(filesystem, changes,
						       batch, &admitted);
		}

		/* A batch the staging refused is retried one at a time. */
		if (!admitted && (result == ENOMEM || result == E2BIG)) {
			batch = 1;
			result = fat_raw_set_cluster(filesystem,
						     clusters[index], 0);
		}

		/* A batch that failed puts the chain back the way it was. */
		if (result != 0) {
			rollback = 0;

			/*
			 * The failing entry restores itself.  Recreate every
			 * link cleared earlier so callers can also restore the
			 * directory entry and expose the complete old file
			 * after an error.
			 */
			for (restore = 0; restore < index; restore++) {
				/* Points it back at the one after it. */
				if (restore + 1U < count)
					link = clusters[restore + 1U];
				else
					link = fat_raw_end_of_chain(fat);

				restore_error = fat_raw_set_cluster(
					filesystem, clusters[restore], link);
				if (rollback == 0 && restore_error != 0)
					rollback = restore_error;
			}

			/* A chain that cannot be restored stops all writing. */
			if (rollback != 0) {
				filesystem->read_only = 1;
				result = rollback;
			}

			goto out;
		}

		index += batch;
	}

	result = 0;
out:
	kern_free(clusters);

	/* Failed. */
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Frees the chains of files that were unlinked while still open. */
static int
fat_drain_pending_orphans(
	struct fat_mount_state *filesystem)
{
	unsigned index;
	int result;

	/* Frees the deferred chains, most recently added first. */
	while (filesystem->pending_orphan_count != 0U) {
		index = filesystem->pending_orphan_count - 1U;

		/* Frees one chain that was waiting for its file to close. */
		result = fat_raw_free_chain(filesystem,
					    filesystem->pending_orphans[index]);
		if (result != 0)
			return result;
		filesystem->pending_orphans[index] = 0;
		filesystem->pending_orphan_count--;
	}

	/* Succeeded. */
	return 0;
}

/* Remembers a chain to free once nothing holds the file open. */
static int
fat_defer_orphan(
	struct fat_mount_state *filesystem,
	uint32_t first_cluster)
{
	unsigned index;

	/* A call that names no volume, or no chain to remember. */
	if (filesystem == NULL || first_cluster == 0)
		return EINVAL;

	/* A chain already waiting is not remembered twice. */
	for (index = 0; index < filesystem->pending_orphan_count; index++) {
		/* This chain is already on the list. */
		if (filesystem->pending_orphans[index] == first_cluster)
			return 0;
	}

	/* The list holds no more than one entry per inode slot. */
	if (filesystem->pending_orphan_count >= FAT_INODE_MAX)
		return ENOSPC;

	filesystem->pending_orphans[filesystem->pending_orphan_count++] =
		first_cluster;

	/* Succeeded. */
	return 0;
}

/* Looks through the allocation table for a cluster that is free. */
static int
fat_raw_find_free_cluster(
	struct fat_mount_state *filesystem,
	uint32_t *free_cluster)
{
	uint32_t cluster;
	uint32_t value;
	int result;
	struct fat_mount_state *fat = filesystem;
	uint32_t start = fat->allocation_hint;
	uint32_t index;
	int valid;

	/* A volume with no clusters has none that could be free. */
	if (!free_cluster || !fat->cluster_count)
		return EIO;

	/* A hint that is not a cluster starts the search at the first. */
	/* Asks whether that is a cluster the volume has. */
	valid = fat_raw_valid_cluster(fat, start);
	if (!valid)
		start = 2;

	/* Walks every cluster once, beginning at the hint. */
	for (index = 0; index < fat->cluster_count; index++) {
		cluster = 2U + ((start - 2U + index) % fat->cluster_count);

		/* Reads the table entry that belongs to this cluster. */
		result = fat_raw_next_cluster(filesystem, cluster, &value);
		if (result != 0)
			return result;

		/* An entry of zero is what marks a cluster as free. */
		if (!value) {
			*free_cluster = cluster;
			fat->allocation_hint = cluster + 1U;

			/* A hint past the last cluster wraps to the first. */
			valid = fat_raw_valid_cluster(fat,
						      fat->allocation_hint);
			if (!valid)
				fat->allocation_hint = 2;

			/* Succeeded. */
			return 0;
		}
	}

	/* Failed. */
	return ENOSPC;
}

/* Fills a whole cluster with zero, as a new directory needs. */
static int
fat_raw_zero_cluster(
	struct fat_mount_state *filesystem,
	uint32_t cluster)
{
	uint32_t lba;
	uint8_t *sector;
	int result;
	struct fat_mount_state *fat = filesystem;
	uint32_t index;

	/* Zeroes the cluster one sector at a time. */
	for (index = 0; index < fat->sectors_per_cluster; index++) {
		/* Turns the cluster and the sector in it into an address. */
		result = fat_engine_cluster_lba(filesystem, cluster, index,
			&lba);
		if (result != 0)
			return result;

		/* Reads the sector so it can be filled in place. */
		result = fat_engine_write_sector_result(filesystem, lba,
							&sector);
		if (result != 0)
			return result;

		clear_bytes(sector, 512);

		/* The zeroes reach the volume once the sector is written. */
		result = fat_engine_mark_sector_dirty(filesystem);
		if (result == 0)
			result = fat_engine_flush(filesystem);
		if (result != 0)
			return result;
	}

	/* Succeeded. */
	return 0;
}

/* Takes one free cluster and marks it as the end of a chain. */
static int
fat_raw_allocate_cluster(
	struct fat_mount_state *filesystem,
	uint32_t *cluster)
{
	int error;
	int result;

	/* Looks through the table for a cluster that is free. */
	result = fat_raw_find_free_cluster(filesystem, cluster);
	if (result != 0)
		return result;

	/* A cluster handed out is zeroed before anything reads it. */
	result = fat_raw_zero_cluster(filesystem, *cluster);
	if (result != 0)
		return result;

	/* One cluster on its own is a chain that ends at itself. */
	error = fat_raw_set_cluster(filesystem,
				    *cluster,
				    fat_raw_end_of_chain(filesystem));

	/* Reports whether the cluster could be claimed. */
	return error;
}

/* Initialize a free cluster before staging its same-sector tail link. */
static int
fat_raw_extend_cluster(
	struct fat_mount_state *filesystem,
	uint32_t tail,
	uint32_t *added)
{
	int error;

	/*
	 * Keep the unallocated cluster private under the mount mutation lock.
	 */

	/* Looks through the table for a cluster that is free. */
	error = fat_raw_find_free_cluster(filesystem, added);
	if (error != 0)
		return error;

	/* A cluster handed out is zeroed before anything reads it. */
	error = fat_raw_zero_cluster(filesystem, *added);
	if (error != 0)
		return error;

	/* Report the fully published link or its completed rollback. */
	error = fat_link_initialized_cluster(filesystem, tail, *added);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Prepare an initialized private chain before linking it into an existing file.
 */
static int
fat_raw_allocate_run(
	struct fat_mount_state *filesystem,
	uint32_t tail,
	uint32_t wanted,
	uint32_t *first)
{
	struct fat_entry_change changes[FAT_BATCH_ENTRIES];
	uint32_t clusters[FAT_BATCH_ENTRIES];
	uint32_t cluster;
	uint32_t offset;
	uint32_t sector;
	uint32_t cluster_bytes;
	uint32_t limit;
	unsigned count;
	unsigned n;
	unsigned bytes;
	unsigned entries;
	int combined;
	int admitted;
	int error;
	int rollback;
	uint32_t first_offset;

	/*
	 * Bound initialization work and leave room for a same-sector old-tail
	 * link.
	 */
	*first = 0;
	cluster_bytes = (uint32_t)filesystem->sectors_per_cluster * 512U;

	/* One batch covers no more than sixty-four kilobytes. */
	limit = 65536U / cluster_bytes;
	if (limit == 0)
		limit = 1;

	/* Nor more entries than the staging holds, less the tail. */
	if (limit > FAT_BATCH_ENTRIES - (tail != 0))
		limit = FAT_BATCH_ENTRIES - (tail != 0);

	/* Nor more clusters than the caller actually asked for. */
	if (wanted < limit)
		limit = wanted;

	/* A batch of no clusters would allocate nothing. */
	if (limit == 0)
		return EINVAL;
	bytes = filesystem->type == KERN_FAT32 ? 4U : 2U;
	count = 0;
	sector = 0;

	/*
	 * Reserve identities under the mount lock without publishing FAT
	 * entries.
	 */
	while (count < limit) {
		/* Looks for the next free cluster of the run. */
		error = fat_raw_find_free_cluster(filesystem, &cluster);
		if (error == ENOSPC && count != 0)
			break;
		if (error != 0)
			return error;
		/* A cluster already in this batch would be handed out twice. */
		for (n = 0; n < count; n++) {
			/* This one is already in the batch. */
			if (clusters[n] == cluster)
				break;
		}

		/* The run stops at the first repeat. */
		if (n != count)
			break;

		/* A run is only batched while its entries share one sector. */
		offset = fat_raw_entry_offset(filesystem, cluster);
		if (count != 0 && (offset / 512U != sector ||
				   (offset + bytes - 1U) / 512U != sector)) {
			filesystem->allocation_hint = cluster;
			break;
		}

		sector = offset / 512U;

		/* A cluster handed out is zeroed before anything reads it. */
		error = fat_raw_zero_cluster(filesystem, cluster);
		if (error != 0)
			return error;
		clusters[count++] = cluster;

		/* An entry that ends in another sector ends the run. */
		if ((offset + bytes - 1U) / 512U != sector)
			break;
	}

	/*
	 * Encode a complete private chain, with its final endpoint already
	 * initialized.
	 */
	for (n = 0; n < count; n++) {
		changes[n].cluster = clusters[n];

		/* The last cluster of the run ends the chain. */
		if (n + 1U < count)
			changes[n].value = clusters[n + 1U];
		else
			changes[n].value = fat_raw_end_of_chain(filesystem);
	}

	entries = count;
	combined = 0;

	/* A chain that is being extended links its old tail on. */
	if (tail != 0) {
		/* The link only joins the batch when it shares its sector. */
		offset = fat_raw_entry_offset(filesystem, tail);
		first_offset = fat_raw_entry_offset(filesystem, clusters[0]);
		if (offset / 512U == sector &&
		    (offset + bytes - 1U) / 512U == sector &&
		    (first_offset + bytes -
		     1U) / 512U ==
			    sector) {
			changes[entries].cluster = tail;
			changes[entries++].value = clusters[0];
			combined = 1;
		}
	}

	error = fat_table_transaction(filesystem, changes, entries, &admitted);

	/*
	 * Preserve progress under workspace pressure without publishing unused
	 * candidates.
	 */
	if (!admitted && (error == ENOMEM || error == E2BIG)) {
		/* An extension links the old tail to the new first cluster. */
		if (tail != 0) {
			error = fat_link_initialized_cluster(filesystem, tail,
							     clusters[0]);
		} else {
			error = fat_raw_set_cluster(
				filesystem, clusters[0],
				fat_raw_end_of_chain(filesystem));
		}

		/* Reports the first cluster of what was allocated. */
		if (error == 0)
			*first = clusters[0];
		/* Failed. */
		return error;
	}

	/* Nothing is linked until the new chain is durable. */
	if (error != 0)
		return error;

	/*
	 * Link across sectors only after the entire new chain is durably
	 * readable.
	 */
	if (tail != 0 && !combined) {
		/* Links the old tail to the new run, in its own write. */
		error = fat_raw_set_cluster(filesystem, tail, clusters[0]);
		if (error != 0) {
			/* A frozen volume cannot be unwound further. */
			if (filesystem->read_only)
				return error;

			/* The clusters go back when the link failed. */
			rollback = fat_raw_free_chain(filesystem, clusters[0]);
			if (rollback != 0) {
				filesystem->read_only = 1;

				/* Failed. */
				return rollback;
			}

			/* Failed. */
			return error;
		}
	}

	/* Return only the published initialized chain head. */
	*first = clusters[0];

	/* Succeeded. */
	return 0;
}

/* Reads one 32-byte record of a directory by its index. */
static int
fat_raw_directory_entry(
	struct fat_mount_state *filesystem,
	const struct fat_directory *directory,
	uint32_t index,
	uint32_t *entry_lba,
	uint16_t *entry_offset,
	const uint8_t **raw)
{
	uint32_t next;
	uint32_t entries_per_cluster;
	uint32_t cluster;
	uint32_t cluster_index;
	uint32_t sector_index;
	struct fat_mount_state *fat = filesystem;
	uint32_t lba;
	uint16_t offset;
	const uint8_t *sector;
	int result;
	int last;
	int valid;

	/* A FAT12 or FAT16 root is a fixed run of sectors. */
	if (directory->first_cluster == 0) {
		/* An index past the last entry the root holds. */
		if (index >= fat->root_entries)
			return ENOENT;
		lba = fat->root_start + index / FAT16_ENTRIES_PER_SECTOR;
		offset = (uint16_t)((index % FAT16_ENTRIES_PER_SECTOR) *
				    FAT16_DIRECTORY_ENTRY_SIZE);
	} else {
		entries_per_cluster = (uint32_t)fat->sectors_per_cluster *
				      FAT16_ENTRIES_PER_SECTOR;

		/* A directory whose first cluster is not one of the volume. */
		cluster = directory->first_cluster;
		valid = fat_raw_valid_cluster(fat, cluster);
		if (!valid || !entries_per_cluster) {
			/* Failed. */
			return EIO;
		}

		/* Walks the chain to the cluster the index falls in. */
		cluster_index = index / entries_per_cluster;
		index %= entries_per_cluster;
		while (cluster_index--) {
			/* Reads the entry that follows this cluster. */
			result = fat_raw_next_cluster(filesystem, cluster,
				&next);
			if (result != 0)
				return result;

			/* An index past the end of the chain names no entry. */
			/* Asks whether the entry is the end-of-chain marker. */
			last = fat_raw_is_end(fat, next);
			if (last)
				return ENOENT;

			/* A chain that leaves the volume means corruption. */
			/* Asks whether that is a cluster the volume has. */
			valid = fat_raw_valid_cluster(fat, next);
			if (!valid)
				return EIO;
			cluster = next;
		}

		sector_index = index / FAT16_ENTRIES_PER_SECTOR;

		/* Turns the cluster and the sector in it into an address. */
		result = fat_engine_cluster_lba(filesystem, cluster,
			sector_index, &lba);
		if (result != 0)
			return result;

		offset = (uint16_t)((index % FAT16_ENTRIES_PER_SECTOR) *
				    FAT16_DIRECTORY_ENTRY_SIZE);
	}

	/* Reads the sector the record falls in. */
	result = fat_engine_read_sector_result(filesystem, lba, &sector);
	if (result != 0)
		return result;

	*entry_lba = lba;
	*entry_offset = offset;
	*raw = sector + offset;

	/* Succeeded. */
	return 0;
}

/* Looks through a directory for the record of one name. */
static int
fat_raw_find_entry(
	struct fat_mount_state *filesystem,
	const struct fat_directory *directory,
	const struct fat_component *component,
	enum fat_name_match match,
	uint32_t *entry_lba,
	uint16_t *entry_offset,
	uint32_t *free_lba,
	uint16_t *free_offset,
	char found_name[KERN_PATH_MAX])
{
	char long_name[KERN_PATH_MAX];
	struct fat_dir_entry short_entry;
	int matches;
	uint32_t lba;
	uint16_t offset;
	const uint8_t *raw;
	int result;
	struct fat_mount_state *fat = filesystem;
	struct fat_lfn_state lfn;
	uint32_t limit;
	uint32_t index;
	int have_free = 0;
	int same;
	int finished;

	/*
	 * Bounds the walk.  A FAT16 root is a fixed table of entries; every
	 * other directory is a cluster chain, so the bound is what the whole
	 * data area could hold.
	 */
	if (directory->first_cluster == 0)
		limit = fat->root_entries;
	else
		limit = fat->cluster_count *
		    (uint32_t)fat->sectors_per_cluster *
		    FAT16_ENTRIES_PER_SECTOR;

	fat_lfn_reset(&lfn);

	/* Walks the directory one record at a time. */
	for (index = 0; index < limit; index++) {
		/* Reads the record at this index. */
		result = fat_raw_directory_entry(filesystem, directory, index,
						 &lba, &offset, &raw);
		if (result == ENOENT)
			break;
		if (result != 0)
			return result;

		/*
		 * A record that was never used or has been erased is a slot
		 * a new name could be written into.  Only the first such slot
		 * is remembered, and only when the caller asked for one.
		 */
		if ((raw[0] == 0 || raw[0] == 0xe5) && !have_free &&
		    free_lba != NULL && free_offset != NULL) {
			*free_lba = lba;
			*free_offset = offset;
			have_free = 1;
		}

		/* An unused record is the end of the directory. */
		if (!raw[0]) {
			fat_lfn_reset(&lfn);
			break;
		}

		/* An erased record names nothing. */
		if (raw[0] == 0xe5) {
			fat_lfn_reset(&lfn);
			continue;
		}

		/* A long-name record is collected, not compared. */
		if (raw[11] == 0x0f) {
			/* Only FAT32 volumes carry long names at all. */
			if (fat->type == KERN_FAT32)
				(void)fat_lfn_feed(&lfn, raw);
			continue;
		}

		/* A volume label is not a name that can be looked up. */
		if (raw[11] & 0x08U) {
			fat_lfn_reset(&lfn);
			continue;
		}

		/* A FAT32 record may carry a long name in front of it. */
		if (fat->type == KERN_FAT32) {
			/* A long name that did not hold together is dropped. */
			finished = fat_lfn_finish(&lfn, raw, long_name,
						  sizeof(long_name));
			if (!finished) {
				fat_sfn_decode_preserve(raw,
							long_name,
							sizeof(long_name));
			}

			/*
			 * Compares the decoded name the way the caller asked.
			 */
			if (match == FAT_NAME_EXACT)
				matches = text_equal(long_name,
						     component->text);
			else
				matches = fat_utf8_casefold_equal(
						  long_name,
						  component->text);

			/* Skips an entry that names something else. */
			if (!matches)
				continue;

			/* Reports the name as the volume itself spells it. */
			if (found_name != 0)
				text_copy(found_name, long_name,
					KERN_PATH_MAX);
		} else {
			/* A short name is compared without regard to case. */
			same = fat_sfn_equal(raw, component->sfn);
			if (!same)
				continue;

			/* Reports the name as the volume itself spells it. */
			if (found_name != 0) {
				fat_sfn_decode_lower(raw, &short_entry);
				text_copy(found_name, short_entry.name,
					KERN_PATH_MAX);
			}
		}

		*entry_lba = lba;
		*entry_offset = offset;

		/* Succeeded. */
		return 0;
	}

	/*
	 * The name is not here.  A caller that is about to create it needs to
	 * know whether a free slot was passed on the way, so the absence of
	 * one is reported as no space rather than as no entry.
	 */
	if (have_free)
		return ENOENT;

	return ENOSPC;
}

/* Walks a path down to the directory holding its last component. */
static int
fat_raw_resolve_parent(
	struct fat_mount_state *filesystem,
	const char *path,
	struct fat_directory *parent,
	struct fat_component *component)
{
	unsigned length;
	const char *separator;
	uint32_t lba, free_lba;
	uint16_t offset, free_offset;
	const uint8_t *sector;
	int result;
	const char *cursor = path;
	int valid;
	int encoded;

	parent->first_cluster = fat_raw_root_cluster(filesystem);

	/* A leading slash is the root the path starts at. */
	if (*cursor == '/')
		cursor++;

	/* A path of nothing but a slash names no component. */
	if (!*cursor)
		return EINVAL;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		length = 0;
		lba = 0;
		free_lba = 0;
		offset = 0;
		free_offset = 0;

		/* Takes the next component, which ends at the next slash. */
		separator = cursor;
		while (*separator && *separator != '/')
			separator++;

		/* A component of nothing, or one longer than a name. */
		length = (unsigned)(separator - cursor);
		if (!length || length >= sizeof(component->text))
			return EINVAL;

		copy_bytes(component->text, cursor, length);
		component->text[length] = '\0';

		/* A volume without long names needs a short one to look up. */
		if (filesystem->type != KERN_FAT32) {
			encoded = fat_sfn_encode(component->text,
						 component->sfn);
			if (!encoded) {
				/* Failed. */
				return EINVAL;
			}
		}

		/* The last component is the caller's to resolve. */
		if (!*separator)
			return 0;

		/* A path that ends in a slash names no last component. */
		cursor = separator + 1;
		if (!*cursor)
			return EINVAL;

		/* Looks the component up in the directory reached so far. */
		result = fat_raw_find_entry(filesystem,
					    parent,
					    component,
					    FAT_NAME_EXACT,
					    &lba,
					    &offset,
					    &free_lba,
					    &free_offset,
					    0);
		if (result != 0) {
			if (result == ENOSPC)
				return ENOENT;	/* Failed. */

			return result;	/* Failed. */
		}

		result = fat_engine_read_sector_result(filesystem, lba,
			&sector);
		if (result != 0)
			return result;

		/* Only a directory can hold the components that follow. */
		if (!(sector[offset + 11] & 0x10U))
			return ENOENT;

		parent->first_cluster = fat_raw_dir_cluster(filesystem,
			sector + offset);

		/* A directory whose first cluster is not one of the volume. */
		valid = fat_raw_valid_cluster(filesystem,
					      parent->first_cluster);
		if (!valid)
			return EIO;
	}
}

/* Walks a path down to the directory record it names. */
static int
fat_raw_resolve_entry(
	struct fat_mount_state *filesystem,
	const char *path,
	uint32_t *lba,
	uint16_t *offset,
	const uint8_t **raw,
	enum fat_name_match match,
	char found_name[KERN_PATH_MAX])
{
	struct fat_directory parent;
	struct fat_component component;
	uint32_t free_lba = 0;
	uint16_t free_offset = 0;
	const uint8_t *sector;
	int result;

	/* Walks to the directory the last component lives in. */
	result = fat_raw_resolve_parent(filesystem, path, &parent, &component);
	if (result != 0)
		return result;

	/* Searches that directory for the component itself. */
	result = fat_raw_find_entry(filesystem,
				    &parent,
				    &component,
				    match,
				    lba,
				    offset,
				    &free_lba,
				    &free_offset,
				    found_name);

	/*
	 * The search reports no space when the name is absent and no free
	 * slot was passed.  A caller that is only resolving does not care
	 * which of the two it was.
	 */
	if (result == ENOSPC)
		return ENOENT;
	if (result != 0)
		return result;

	/* Takes the sector so the caller can read the entry itself. */
	result = fat_engine_read_sector_result(filesystem, *lba, &sector);
	if (result != 0)
		return result;

	*raw = sector + *offset;

	/* Succeeded: the caller now holds the entry and its location. */
	return 0;
}

/* Fills an open file from the directory entry that describes it. */
static int
fat_raw_populate_file(
	struct fat_file_state *file,
	uint32_t lba,
	uint16_t offset,
	const uint8_t raw[32])
{
	struct fat_mount_state *fat;
	struct fat_file_state *state;
	int valid;

	/* Records where the entry lives, so a size change can find it again. */
	fat = file->mount;
	state = file;
	state->first_cluster = fat_raw_dir_cluster(fat, raw);
	state->directory_lba = lba;
	state->directory_offset = offset;
	state->directory_dirty = 0;
	file->size = fat_engine_get32(raw + 28);

	/* Succeeded: an empty file with no chain has nothing left to check. */
	if (!file->size && !state->first_cluster)
		return 0;

	/* An entry naming a cluster outside the volume is corrupt. */
	/* Asks whether that is a cluster the volume has. */
	valid = fat_raw_valid_cluster(fat, state->first_cluster);
	if (!valid)
		return EIO;

	/* Succeeded: the file is described by an entry that makes sense. */
	return 0;
}

/* Ties an open file to the mount it was opened on. */
static void
fat_file_bind(
	struct fat_file_state *file,
	struct fat_mount_state *mount)
{
	file->mount = mount;
	file->cursor_valid = 0;
	file->size = 0;
	file->first_cluster = 0;
	file->directory_lba = 0;
	file->directory_offset = 0;
	file->directory_dirty = 0;
}

/* Opens the file a path names, filling in its chain and size. */
static int
fat_raw_open(
	struct fat_mount_state *filesystem,
	const char *path,
	struct fat_file_state *file)
{
	int error;
	uint32_t lba = 0;
	uint16_t offset = 0;
	const uint8_t *raw;
	int result;

	/* A call that names no volume, no path or no file to fill in. */
	if (filesystem == NULL || path == NULL || file == NULL)
		return EINVAL;

	fat_file_bind(file, filesystem);

	/* Walks the path down to the record it names. */
	result = fat_raw_resolve_entry(filesystem,
				       path,
				       &lba,
				       &offset,
				       &raw,
				       FAT_NAME_EXACT,
				       0);
	if (result != 0)
		return result;

	/* A directory is not opened as a file. */
	if (raw[11] & 0x10U)
		return EINVAL;

	/* Fills in the chain and size the record recorded. */
	error = fat_raw_populate_file(file, lba, offset, raw);

	/* Reports whether the file could be opened. */
	return error;
}

/*
 * Writes a file's cached size and first cluster back to its directory entry.
 */
static int
fat_raw_flush_file(
	struct fat_file_state *file)
{
	struct fat_file_state *state = file;
	uint8_t *sector;
	int error;

	/* Refuses to write to a read-only mount. */
	if (file->mount->read_only)
		return EROFS;

	/*
	 * Nothing more is due once the volume is flushed and the entry clean.
	 */
	error = fat_engine_flush(file->mount);
	if (error != 0 || !state->directory_dirty)
		return error;

	/*
	 * An unlinked-but-open inode has no directory entry to update.  Its
	 * cluster state remains live until the final reference triggers
	 * reclaim.
	 */
	if (state->owner != NULL && (state->owner->i_flags & INODE_DEAD) != 0) {
		state->directory_dirty = 0;

		/* Succeeded: there was no entry left to write. */
		return 0;
	}

	/* FAT stores a size in 32 bits and cannot record a longer file. */
	if (file->size > 0xffffffffU)
		return EINVAL;

	/* Takes the sector the directory entry lives in for writing. */
	error = fat_engine_write_sector_result(file->mount,
					       state->directory_lba, &sector);
	if (error != 0)
		return error;

	/* Publishes the first cluster and the size into the entry. */
	fat_raw_put_dir_cluster(file->mount,
				sector + state->directory_offset,
				state->first_cluster);
	put32(sector + state->directory_offset + 28, (uint32_t)file->size);

	/* Writes the sector out, and only then calls the entry clean. */
	error = fat_engine_mark_sector_dirty(file->mount);
	if (error == 0)
		error = fat_engine_flush(file->mount);
	if (error != 0)
		return error;

	state->directory_dirty = 0;

	/* Succeeded: the entry on disk matches the open file. */
	return 0;
}

/* Steps one cluster along a chain, growing it if asked to. */
static int
fat_raw_advance_cluster(
	struct fat_file_state *file,
	uint32_t cluster,
	int allocate,
	uint32_t *next)
{
	int result;
	int last;
	int valid;

	/* Reads the entry that follows this cluster. */
	result = fat_raw_next_cluster(file->mount, cluster, next);
	if (result != 0)
		return result;

	/* The end-of-chain marker is where the file stops. */
	last = fat_raw_is_end(file->mount, *next);
	if (last) {
		/* A read stops there; only a write goes past it. */
		if (!allocate)
			return EIO;

		/* Grows the chain by one cluster past its old end. */
		result = fat_raw_extend_cluster(file->mount, cluster, next);
		if (result != 0)
			return result;
	} else {
		valid = fat_raw_valid_cluster(file->mount, *next);
	}

	/* A chain that leaves the volume means corruption. */
	if (!last && !valid) {
		/* Failed. */
		return EIO;
	}

	/* Succeeded. */
	return 0;
}

/* Finds the cluster holding one position of a file. */
static int
fat_raw_cluster_at(
	struct fat_file_state *file,
	uint32_t cluster_index,
	int allocate,
	uint32_t *found_cluster,
	struct fat_chain_cursor *cursor)
{
	uint32_t next;
	struct fat_mount_state *fat = file->mount;
	struct fat_file_state *state = file;
	uint32_t cluster = state->first_cluster;
	uint32_t index = 0U;
	int result;
	int valid;

	/* The cached cursor saves walking the chain from the start. */
	if (cursor != NULL && cursor->cluster != 0U &&
	    cursor->index <= cluster_index) {
		cluster = cursor->cluster;
		index = cursor->index;
	}

	/* A file that never had a cluster starts with one. */
	if (!cluster) {
		/* A read of a file with no chain finds nothing. */
		if (!allocate)
			return EIO;

		/* Takes the first cluster of the file's own chain. */
		result = fat_raw_allocate_cluster(file->mount, &cluster);
		if (result != 0)
			return result;

		state->first_cluster = cluster;
		state->directory_dirty = 1;
	}

	/* A chain that leaves the volume means corruption. */
	/* Asks whether that is a cluster the volume has. */
	valid = fat_raw_valid_cluster(fat, cluster);
	if (!valid)
		return EIO;

	/* Walks the chain to the cluster the position falls in. */
	for (; index < cluster_index; index++) {
		/* More steps than the volume has clusters means it loops. */
		if (index >= fat->cluster_count)
			return EIO;

		/* Steps one cluster along, growing the chain if asked. */
		result = fat_raw_advance_cluster(file, cluster, allocate,
			&next);
		if (result != 0)
			return result;

		cluster = next;
	}
	*found_cluster = cluster;

	/* Leaves the cursor where the walk stopped. */
	if (cursor != NULL) {
		cursor->index = cluster_index;
		cursor->cluster = cluster;
	}

	/* Succeeded. */
	return 0;
}

/* Writes a run of bytes into a file, growing its chain as needed. */
static int
fat_raw_write_bytes(
	struct fat_file_state *file,
	uint32_t offset,
	const uint8_t *input,
	uint32_t length,
	int zero,
	struct fat_chain_cursor *cursor)
{
	uint32_t next;
	uint32_t in_cluster;
	uint32_t sector_index;
	uint32_t within;
	uint32_t chunk;
	uint32_t lba;
	uint8_t *sector;
	struct fat_mount_state *fat = file->mount;
	uint32_t cluster_bytes = (uint32_t)fat->sectors_per_cluster * 512U;
	uint32_t position = offset;
	uint32_t cluster;
	uint32_t wanted;
	int result;
	int valid;
	int last;

	/* A write of no bytes changes nothing. */
	if (length == 0U)
		return 0;

	/* Populate an empty file with one bounded initialized chain. */
	if (file->first_cluster == 0 && position == 0) {
		wanted = (uint32_t)(((uint64_t)length + cluster_bytes - 1U) /
				    cluster_bytes);

		/* A file with no chain is given one long enough to hold it. */
		result = fat_raw_allocate_run(fat, 0, wanted, &cluster);
		if (result != 0)
			return result;

		file->first_cluster = cluster;
		file->directory_dirty = 1;
	}

	/* Finds the cluster the write begins in. */
	result = fat_raw_cluster_at(file, position / cluster_bytes, 1, &cluster,
		cursor);
	if (result != 0)
		return result;

	/* Writes the run one sector at a time. */
	while (length) {
		in_cluster = position % cluster_bytes;
		sector_index = in_cluster / 512U;
		within = in_cluster & 511U;

		/* The run may end inside this sector rather than at its end. */
		chunk = 512U - within;
		if (chunk > length)
			chunk = length;

		/* Turns the cluster and the sector in it into an address. */
		result = fat_engine_cluster_lba(file->mount,
						cluster,
						sector_index,
						&lba);
		if (result != 0)
			return result;

		/* Reads the sector so it can be changed in place. */
		result = fat_engine_write_sector_result(file->mount,
							lba,
							&sector);
		if (result != 0)
			return result;

		/* A growing write fills the gap it leaves with zeroes. */
		if (zero)
			clear_bytes(sector + within, chunk);
		else
			copy_bytes(sector + within, input, chunk);

		/* The change reaches the volume once the sector is written. */
		result = fat_engine_mark_sector_dirty(file->mount);
		if (result == 0)
			result = fat_engine_flush(file->mount);
		if (result != 0)
			return result;

		/* A zeroing pass has no input of its own to advance. */
		if (!zero)
			input += chunk;
		position += chunk;
		length -= chunk;

		/*
		 * The mount lock keeps this operation's chain stable.  Reuse
		 * the current cluster within it and advance once at each
		 * boundary, instead of seeking again from the first cluster for
		 * every sector.
		 */
		if (length != 0U && position % cluster_bytes == 0U) {
			/* More clusters than the volume has means a loop. */
			if (cursor->index >= fat->cluster_count)
				return EIO;

			/* A write past the end of the chain has to grow it. */
			result = fat_raw_next_cluster(fat, cluster, &next);
			if (result == 0) {
				last = fat_raw_is_end(fat, next);
				if (last) {
					wanted = (uint32_t)(
						((uint64_t)length +
						 cluster_bytes - 1U) /
						cluster_bytes);
					result = fat_raw_allocate_run(fat,
						cluster, wanted, &next);
				}
			}

			/* A chain that leaves the volume means corruption. */
			if (result == 0) {
				valid = fat_raw_valid_cluster(fat, next);
				if (!valid)
					result = EIO;
			}
			if (result != 0)
				return result;

			cluster = next;
			cursor->index++;
			cursor->cluster = cluster;
		}
	}

	/* Succeeded. */
	return 0;
}

/* Puts a file back the size it was before a write that failed. */
static int
fat_raw_rollback_growth(
	struct fat_file_state *file,
	uint32_t old_first,
	uint32_t old_last,
	uint64_t old_size,
	uint8_t old_directory_dirty)
{
	struct fat_mount_state *fat = file->mount;
	struct fat_file_state *state = file;
	uint32_t added = 0;
	int result = 0;
	int valid;
	int last;

	/* A file that had no chain gives back the whole of the new one. */
	if (old_first == 0) {
		added = state->first_cluster;
	} else if (old_last != 0) {
		/* Otherwise it gives back what was added past the end. */
		result = fat_raw_next_cluster(fat, old_last, &added);
		if (result == 0) {
			last = fat_raw_is_end(fat, added);
			valid = fat_raw_valid_cluster(fat, added);

			/* Nothing was added if the chain still ends there. */
			if (last)
				added = 0;
			else if (!valid)
				result = EIO;
		}
		if (result == 0 && added != 0) {
			result = fat_raw_set_cluster(fat,
						     old_last,
						     fat_raw_end_of_chain(fat));
		}
	}

	/* Frees the clusters the write had added. */
	if (result == 0 && added != 0)
		result = fat_raw_free_chain(fat, added);

	state->first_cluster = old_first;
	file->size = old_size;
	state->directory_dirty = old_directory_dirty;

	/* A file that cannot be put back leaves the volume unwritable. */
	if (result != 0)
		fat->read_only = 1;

	/* Failed. */
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Puts a file's directory record back the way it was. */
static int
fat_raw_restore_directory(
	struct fat_file_state *file,
	uint32_t first_cluster,
	uint64_t size,
	uint8_t directory_dirty)
{
	struct fat_file_state *state = file;
	int result;

	state->first_cluster = first_cluster;
	file->size = size;
	state->directory_dirty = 1;

	/* Writes the record back with the size and chain it had. */
	result = fat_raw_flush_file(file);
	if (result == 0)
		state->directory_dirty = directory_dirty;

	/* Reports whether the record could be put back. */
	return result;
}

/* Writes a run of bytes into a file and publishes its new size. */
static int
fat_raw_write(
	struct fat_file_state *file,
	uint64_t offset,
	const void *buffer,
	uint32_t length)
{
	int rollback;
	uint64_t first_offset;
	struct fat_file_state *state = file;
	uint64_t end;
	uint64_t old_size;
	uint64_t generation;
	uint32_t old_first, old_last = 0;
	struct fat_chain_cursor cursor = {0U, 0U};
	uint8_t old_directory_dirty;
	int result;

	/* A volume mounted read-only is never written to. */
	if (file->mount->read_only)
		return EROFS;

	/* A run with no buffer, or one past what a position expresses. */
	if ((!buffer && length) || offset > 0xffffffffU ||
	    (uint64_t)length > 0xffffffffU - offset) {
		/* Failed. */
		return EINVAL;
	}

	/* A write of no bytes changes nothing. */
	if (!length)
		return 0;

	generation = file->mount->chain_generation;
	end = offset + length;
	old_first = state->first_cluster;
	old_size = file->size;
	old_directory_dirty = state->directory_dirty;

	/* A file that has a chain is validated before it is written. */
	if (file->first_cluster) {
		first_offset = offset < old_size ? offset : old_size;

		/*
		 * Full validation precedes all writes, including corruption
		 * beyond the requested range.  Retain only this call's start
		 * and old tail so data/zero-fill/growth do not repeat the same
		 * validated walk.
		 */
		result = fat_file_validate_at(file,
					      first_offset,
					      &cursor,
					      &old_last);
		if (result != 0)
			return result;
	}

	/* A write that starts past the end fills the gap with zeroes. */
	if (offset > file->size) {
		/* Zeroes from the old end up to where the write begins. */
		result = fat_raw_write_bytes(file,
					     (uint32_t)file->size, 0,
					     (uint32_t)(offset - file->size),
					     1,
					     &cursor);
		if (result != 0) {
			/* A gap that could not be filled leaves nothing. */
			rollback = fat_raw_rollback_growth(
				file, old_first, old_last, old_size,
				old_directory_dirty);
			if (rollback != 0)
				return rollback;

			/* Failed. */
			return result;
		}
	}

	/* Writes the caller's bytes at the position they asked for. */
	result = fat_raw_write_bytes(file,
				     (uint32_t)offset,
				     buffer,
				     length,
				     0,
				     &cursor);
	if (result != 0) {
		/* Puts the file back the way the failed write found it. */
		rollback = fat_raw_rollback_growth(file,
							  old_first,
							  old_last,
							  old_size,
							  old_directory_dirty);

		/* A failed rollback is the more serious of the two failures. */
		if (rollback != 0)
			return rollback;

		/* Failed. */
		return result;
	}

	/* A write past the old end makes the file that much longer. */
	if (end > file->size) {
		file->size = end;
		file->directory_dirty = 1;
	}

	fat_file_save_cursor(file, &cursor, end, generation, old_last);

	/* Succeeded. */
	return 0;
}

/*
 * Admits a complete truncate growth under the caller's mount mutation lock.
 * The allocation table is authoritative; FSInfo hints are not reservations.
 */
static int
fat_raw_growth_capacity(
	struct fat_mount_state *filesystem,
	uint32_t allocated_clusters,
	uint64_t size)
{
	uint64_t required;
	uint32_t cluster_bytes;
	uint32_t needed;
	uint32_t cluster;
	uint32_t value;
	int error;

	cluster_bytes = (uint32_t)filesystem->sectors_per_cluster * 512U;
	if (cluster_bytes == 0U)
		return EIO;

	/* Round in 64 bits, then credit every cluster the file already owns. */
	required = (size + cluster_bytes - 1U) / cluster_bytes;
	if (required <= allocated_clusters)
		return 0;
	if (required > filesystem->cluster_count)
		return ENOSPC;
	needed = (uint32_t)required - allocated_clusters;

	/* Stop once sufficient space is proven; allocation still owns the lock. */
	for (cluster = 2U; cluster < filesystem->cluster_count + 2U; cluster++) {
		error = fat_raw_next_cluster(filesystem, cluster, &value);
		if (error != 0)
			return error;
		if (value != 0U)
			continue;
		needed--;
		if (needed == 0U)
			return 0;
	}

	return ENOSPC;
}

/* Changes the size of a file, freeing or zeroing what that costs. */
static int
fat_raw_truncate(
	struct fat_file_state *file,
	uint64_t size)
{
	uint32_t cluster_bytes;
	uint32_t allocated_clusters = 0U;
	int rollback;
	int cleanup;
	uint64_t first_offset;
	uint32_t keep_index;
	uint32_t keep, tail;
	struct fat_mount_state *fat = file->mount;
	struct fat_file_state *state = file;
	uint32_t old_first = state->first_cluster;
	uint32_t old_last = 0U;
	struct fat_chain_cursor cursor = {0U, 0U};
	uint64_t old_size = file->size;
	uint8_t old_directory_dirty = state->directory_dirty;
	int result;
	int last;

	/* A volume mounted read-only is never written to. */
	if (file->mount->read_only)
		return EROFS;

	/* A size past what a 32-bit length expresses. */
	if (size > 0xffffffffU)
		return EINVAL;

	/* A zero-length file may still own a cluster chain. */
	if (size == file->size && (size || !state->first_cluster))
		return 0;

	/* A file that has a chain is validated before it is changed. */
	if (state->first_cluster) {
		cluster_bytes = (uint32_t)fat->sectors_per_cluster * 512U;

		/*
		 * Picks the offset the chain has to be walked to.  Growing
		 * only has to reach the old end; shrinking has to reach the
		 * last byte that survives, and an empty result reaches the
		 * start.
		 */
		if (size > old_size)
			first_offset = old_size;
		else if (size != 0U)
			first_offset = size - 1U;
		else
			first_offset = 0U;

		/* Walks the chain as far as the new size reaches. */
		result = fat_raw_validate_chain_count(
			file->mount, state->first_cluster,
			(uint32_t)first_offset / cluster_bytes, &cursor,
			&old_last, &allocated_clusters);
		if (result != 0)
			return result;
	}

	/* Growing a file fills the new space with zeroes. */
	if (size > file->size) {
		/* An existing size must fit its validated, owned chain. */
		cluster_bytes = (uint32_t)fat->sectors_per_cluster * 512U;
		if (old_size > (uint64_t)allocated_clusters * cluster_bytes)
			return EIO;

		/* Reject insufficient capacity before initializing new clusters. */
		result = fat_raw_growth_capacity(fat, allocated_clusters, size);
		if (result != 0)
			return result;

		/* Zeroes from the old end up to the new size. */
		result = fat_raw_write_bytes(file,
					     (uint32_t)file->size,
					     0,
					     (uint32_t)(size - file->size),
					     1,
					     &cursor);
		if (result != 0) {
			/* A growth that could not finish leaves nothing. */
			rollback = fat_raw_rollback_growth(
				file, old_first, old_last, old_size,
				old_directory_dirty);
			if (rollback != 0)
				return rollback;

			/* Failed. */
			return result;
		}

		file->size = size;
		state->directory_dirty = 1;

		/* Succeeded. */
		return 0;
	}

	/* A file truncated to nothing gives up its whole chain. */
	if (!size) {
		state->first_cluster = 0;
		file->size = 0;
		state->directory_dirty = 1;

		/* Publishes the empty record before the chain is freed. */
		result = fat_raw_flush_file(file);
		if (result != 0) {
			/*
			 * Puts the entry back the way the failed flush found
			 * it.
			 */
			rollback = fat_raw_restore_directory(
				file, old_first, old_size,
				old_directory_dirty);
			if (rollback != 0) {
				file->mount->read_only = 1;

				/* Failed. */
				return rollback;
			}

			/* Failed. */
			return result;
		}

		/* Gives the whole chain back to the allocation table. */
		result = fat_raw_free_chain(file->mount, old_first);
		if (result != 0) {
			/* A chain that could not be freed keeps its record. */
			rollback = fat_raw_restore_directory(
				file, old_first, old_size,
				old_directory_dirty);
			if (rollback != 0) {
				file->mount->read_only = 1;

				/* Failed. */
				return rollback;
			}

			/* Failed. */
			return result;
		}

		/* Succeeded. */
		return 0;
	}

	cluster_bytes = (uint32_t)fat->sectors_per_cluster * 512U;
	keep_index = ((uint32_t)size - 1U) / cluster_bytes;

	/* Finds the last cluster the new size still reaches. */
	result = fat_raw_cluster_at(file, keep_index, 0, &keep, &cursor);
	if (result != 0)
		return result;

	/* And the one after it, which is where the tail begins. */
	result = fat_raw_next_cluster(file->mount, keep, &tail);
	if (result != 0)
		return result;

	/* A chain that leaves the volume means corruption. */
	/* Asks whether the entry is the end-of-chain marker. */
	last = fat_raw_is_end(fat, tail) && !fat_raw_valid_cluster(fat, tail);
	if (!last) {
		/* Failed. */
		return EIO;
	}

	/* The kept part of the chain now ends at that cluster. */
	/* Asks whether the entry is the end-of-chain marker. */
	last = fat_raw_is_end(fat, tail);
	if (!last) {
		/* Marks it as the end before the tail is given back. */
		result = fat_raw_set_cluster(file->mount, keep,
					     fat_raw_end_of_chain(fat));
		if (result != 0)
			return result;
	}

	file->size = size;
	state->directory_dirty = 1;

	/* Publishes the shorter record before the tail is freed. */
	result = fat_raw_flush_file(file);
	if (result != 0) {
		rollback = 0;

		/* A record that could not be written puts the tail back. */
		/* Asks whether the entry is the end-of-chain marker. */
		last = fat_raw_is_end(fat, tail);
		if (!last) {
			cleanup = fat_raw_set_cluster(file->mount, keep, tail);

			/* The first failure of the unwind is reported. */
			if (rollback == 0 && cleanup != 0)
				rollback = cleanup;
		}

		/* Puts the record back the way the failed truncate found it. */
		cleanup = fat_raw_restore_directory(file, old_first, old_size,
			old_directory_dirty);
		if (rollback == 0 && cleanup != 0)
			rollback = cleanup;

		/* A volume that cannot be restored stops being written. */
		if (rollback != 0) {
			file->mount->read_only = 1;

			/* Failed. */
			return rollback;
		}

		/* Failed. */
		return result;
	}

	/* A chain that already ends here has no tail to free. */
	/* Asks whether the entry is the end-of-chain marker. */
	last = fat_raw_is_end(fat, tail);
	if (last)
		return 0;

	/* Gives the tail back to the allocation table. */
	result = fat_raw_free_chain(file->mount, tail);
	if (result != 0) {
		rollback = 0;

		/* Joins the tail back on when it could not be freed. */
		cleanup = fat_raw_set_cluster(file->mount, keep, tail);
		if (cleanup != 0)
			rollback = cleanup;

		/* And puts the record back the way it was. */
		cleanup = fat_raw_restore_directory(file, old_first, old_size,
			old_directory_dirty);
		if (rollback == 0 && cleanup != 0)
			rollback = cleanup;

		/* A volume that cannot be restored stops being written. */
		if (rollback != 0) {
			file->mount->read_only = 1;

			/* Failed. */
			return rollback;
		}

		/* Failed. */
		return result;
	}

	/* Succeeded. */
	return 0;
}

/* Asks whether a directory already holds a given short name. */
static int
fat_raw_sfn_in_use(
	struct fat_mount_state *filesystem,
	const struct fat_directory *directory,
	const uint8_t sfn[11])
{
	uint32_t lba;
	uint16_t offset;
	const uint8_t *raw;
	int result;
	struct fat_mount_state *fat = filesystem;
	uint32_t limit = fat->cluster_count *
		(uint32_t)fat->sectors_per_cluster *
		FAT16_ENTRIES_PER_SECTOR;
	uint32_t index;
	int same;

	/* Walks the directory looking for that stored short name. */
	for (index = 0; index < limit; index++) {
		result = fat_raw_directory_entry(filesystem,
						 directory,
						 index,
						 &lba,
						 &offset,
						 &raw);
		(void)lba;
		(void)offset;
		if (result == ENOENT)
			return ENOENT;
		if (result != 0)
			return result;

		/* An unused record is the end of the directory. */
		if (!raw[0])
			return ENOENT;

		/* An erased or long-name record holds no short name. */
		same = 0;
		if (raw[0] != 0xe5 && raw[11] != 0x0f)
			same = fat_sfn_equal(raw, (const char *)sfn);

		/* A live short record holding that exact name. */
		if (same) {
			/* Succeeded. */
			return 0;
		}
	}

	/* Failed. */
	return EIO;
}

/* Adds one cluster to a directory that has run out of records. */
static int
fat_raw_extend_directory(
	struct fat_mount_state *filesystem,
	const struct fat_directory *directory)
{
	struct fat_mount_state *fat = filesystem;
	uint32_t last = directory->first_cluster;
	uint32_t steps, next, added;
	int result;
	int chain_end;
	int valid;

	/* Only a FAT32 directory grows; the FAT16 root is fixed. */
	valid = fat_raw_valid_cluster(fat, last);
	if (fat->type != KERN_FAT32 || !valid)
		return ENOSPC;

	/* Walks to the last cluster the directory has. */
	for (steps = 0; steps < fat->cluster_count; steps++) {
		/* Reads the entry that follows this cluster. */
		result = fat_raw_next_cluster(filesystem, last, &next);
		if (result != 0)
			return result;

		/* The end-of-chain marker is the last cluster. */
		/* Asks whether the entry is the end-of-chain marker. */
		chain_end = fat_raw_is_end(fat, next);
		if (chain_end)
			break;

		/* A chain that leaves the volume means corruption. */
		/* Asks whether that is a cluster the volume has. */
		valid = fat_raw_valid_cluster(fat, next);
		if (!valid)
			return EIO;

		last = next;
	}

	/* Taking every step the volume allows means a loop. */
	if (steps == fat->cluster_count)
		return EIO;

	/* Adds one zeroed cluster past the old last one. */
	result = fat_raw_extend_cluster(filesystem, last, &added);
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Looks for a run of free records long enough to hold a name. */
static int
fat_raw_find_free_run(
	struct fat_mount_state *filesystem,
	const struct fat_directory *directory,
	unsigned needed,
	uint32_t *first_index)
{
	uint32_t lba;
	uint16_t offset;
	const uint8_t *raw;
	int result;
	struct fat_mount_state *fat = filesystem;
	uint32_t maximum = fat->cluster_count *
		(uint32_t)fat->sectors_per_cluster *
		FAT16_ENTRIES_PER_SECTOR;
	uint32_t index = 0, run_start = 0;
	unsigned run = 0;
	int after_end = 0;

	/* Walks the directory looking for a long enough gap. */
	while (index < maximum) {
		result = fat_raw_directory_entry(filesystem,
						 directory,
						 index,
						 &lba,
						 &offset,
						 &raw);
		(void)lba;
		(void)offset;
		if (result == ENOENT) {
			/* A directory out of records is extended. */
			result = fat_raw_extend_directory(filesystem,
				directory);
			if (result != 0)
				return result;
			continue;
		}

		/* Reports the failure to read the record. */
		if (result != 0)
			return result;

		/* An unused or erased record can hold a new name. */
		if (after_end || raw[0] == 0 || raw[0] == 0xe5) {
			/* The first free record is where the run begins. */
			if (run++ == 0)
				run_start = index;

			/* Nothing beyond the first unused record is in use. */
			if (raw[0] == 0)
				after_end = 1;

			/* A run long enough is the one this call was after. */
			if (run == needed) {
				*first_index = run_start;
				/* Succeeded. */
				return 0;
			}
		} else {
			run = 0;
		}

		index++;
	}

	/* Failed. */
	return ENOSPC;
}

/*
 * Writes a saved directory entry back over its own slot.
 *
 * A failed mutation uses this to undo what it had already published, so the
 * write is flushed before the caller is told the volume is consistent again.
 */
static FAT_MUTATION int
fat_raw_restore_directory_entry(
	struct fat_mount_state *filesystem,
	uint32_t lba,
	uint16_t offset,
	const uint8_t entry[32])
{
	uint8_t *sector;
	int error;

	/* Takes the sector the slot lives in for writing. */
	error = fat_engine_write_sector_result(filesystem, lba, &sector);
	if (error != 0)
		return error;

	/* Puts the saved bytes back into the slot. */
	copy_bytes(sector + offset, entry, 32U);

	/* Marks the sector so the flush below will carry it out. */
	error = fat_engine_mark_sector_dirty(filesystem);
	if (error != 0)
		return error;

	/* Writes it out, because the caller is undoing a published change. */
	error = fat_engine_flush(filesystem);
	if (error != 0)
		return error;

	/* Succeeded: the entry on disk is the one that was saved. */
	return 0;
}

/* Stage an entire long-name run and publish its short-name sector last. */
static FAT_MUTATION int
fat32_create_entry(
	struct fat_mount_state *filesystem,
	const struct fat_directory *parent,
	const struct fat_component *component,
	uint8_t attributes,
	uint32_t first_cluster,
	uint32_t size,
	uint32_t *entry_lba,
	uint16_t *entry_offset)
{
	uint16_t units[FAT_LFN_MAX_UNITS];
	uint8_t sfn[11];
	uint8_t entries[FAT_LFN_MAX_ENTRIES + 1U][32];
	uint32_t lbas[FAT_LFN_MAX_ENTRIES + 1U];
	uint16_t offsets[FAT_LFN_MAX_ENTRIES + 1U];
	const uint8_t *existing;
	unsigned unit_count;
	unsigned lfn_count;
	unsigned serial;
	unsigned n;
	uint32_t first_index;
	int result;
	int rendered;
	int made;

	/* Select an unused short alias before reserving directory positions. */
	rendered = fat_utf8_to_utf16(component->text, units, &unit_count);
	if (!rendered)
		return EINVAL;

	/* Tries serials in turn until an unused alias comes out. */
	for (serial = 1; serial <= 999999U; serial++) {
		/* A name no alias can be made from cannot be created. */
		made = fat_sfn_make_alias(component->text, serial, sfn);
		if (!made)
			return EINVAL;

		/* Asks whether the directory already holds that alias. */
		result = fat_raw_sfn_in_use(filesystem, parent, sfn);
		if (result == ENOENT)
			break;
		if (result != 0)
			return result;
	}

	/* A directory with every alias taken can hold no more. */
	if (serial > 999999U)
		return ENOSPC;

	lfn_count = (unit_count + 12U) / 13U;
	first_index = 0;

	/* Finds a run of records long enough for the whole name. */
	result = fat_raw_find_free_run(filesystem,
				       parent,
				       lfn_count + 1U,
				       &first_index);
	if (result != 0)
		return result;

	/*
	 * Resolve the full run while all original end-marker semantics remain
	 * intact.
	 */
	for (n = 0; n <= lfn_count; n++) {
		/* Reads where each record of the run sits on the volume. */
		result = fat_raw_directory_entry(filesystem,
						 parent,
						 first_index + n,
						 &lbas[n],
						 &offsets[n],
						 &existing);
		if (result != 0)
			return result;
	}

	/*
	 * Build private LFN and SFN entries before capturing shared sector
	 * images.
	 */
	for (n = 0; n < lfn_count; n++) {
		fat_lfn_build_entry(entries[n],
				    units,
				    unit_count,
				    lfn_count - n,
				    fat_lfn_checksum(sfn));
	}

	memset(entries[lfn_count], 0, 32U);
	memcpy(entries[lfn_count], sfn, 11U);
	entries[lfn_count][11] = attributes;

	fat_raw_put_dir_cluster(filesystem, entries[lfn_count], first_cluster);

	put32(entries[lfn_count] + 28, size);

	/* Writes every record of the name as one transaction. */
	result = fat_directory_transaction(filesystem,
					   lbas,
					   offsets,
					   entries,
					   lfn_count + 1U,
					   1);
	if (result != 0)
		return result;

	/* Return the committed public entry identity. */
	if (entry_lba != NULL)
		*entry_lba = lbas[lfn_count];

	/* Reports where the short record sits, if asked. */
	if (entry_offset != NULL)
		*entry_offset = offsets[lfn_count];

	/* Succeeded. */
	return 0;
}

/* Writes a name's long and short records into a directory. */
static FAT_MUTATION int
fat_raw_insert_entry(
	struct fat_mount_state *filesystem,
	const struct fat_directory *parent,
	const struct fat_component *component,
	uint8_t attributes,
	uint32_t first_cluster,
	uint32_t size,
	uint32_t *entry_lba,
	uint16_t *entry_offset)
{
	int error;
	uint32_t lba = 0, free_lba = 0;
	uint16_t offset = 0, free_offset = 0;
	uint8_t saved[32];
	uint8_t *sector;
	int result, rollback;

	/* A name the directory already holds cannot be added. */
	result = fat_raw_find_entry(filesystem,
				    parent,
				    component,
				    FAT_NAME_EXACT,
				    &lba,
				    &offset,
				    &free_lba,
				    &free_offset,
				    0);
	if (result == 0)
		return EEXIST;
	if (result != ENOENT && result != ENOSPC)
		return result;

	/* A FAT32 volume also refuses a name that differs by case. */
	if (filesystem->type == KERN_FAT32) {
		/* Looks the name up again, this time ignoring case. */
		result = fat_raw_find_entry(filesystem,
					    parent,
					    component,
					    FAT_NAME_CASEFOLD,
					    &lba,
					    &offset,
					    &free_lba,
					    &free_offset,
					    0);
		if (result == 0)
			return EEXIST;
		if (result != ENOENT && result != ENOSPC)
			return result;

		/* A FAT32 name is written as a long name and an alias. */
		error = fat32_create_entry(filesystem,
					   parent,
					   component,
					   attributes,
					   first_cluster,
					   size,
					   entry_lba,
					   entry_offset);

		/* Failed. */
		return error;
	}

	/* A directory with no room left can hold no more names. */
	if (result == ENOSPC)
		return result;

	/* Reads the free record so it can be filled in place. */
	result = fat_engine_write_sector_result(filesystem, free_lba, &sector);
	if (result != 0)
		return result;

	copy_bytes(saved, sector + free_offset, sizeof(saved));
	clear_bytes(sector + free_offset, 32);
	copy_bytes(sector + free_offset, component->sfn, 11);
	sector[free_offset + 11] = attributes;

	fat_raw_put_dir_cluster(filesystem, sector + free_offset,
		first_cluster);

	put32(sector + free_offset + 28, size);

	/* The record reaches the volume once the sector is written. */
	result = fat_engine_mark_sector_dirty(filesystem);
	if (result == 0)
		result = fat_engine_flush(filesystem);
	if (result != 0) {
		/* Puts the record back when it could not be published. */
		rollback = fat_raw_restore_directory_entry(filesystem,
							   free_lba,
							   free_offset,
							   saved);
		if (rollback != 0)
			filesystem->read_only = 1;

		/* Reports the failed rollback, or the write that needed it. */
		if (rollback != 0)
			return rollback;	/* Failed. */

		return result;	/* Failed. */
	}

	/* Reports where the record sits, if the caller asked. */
	if (result == 0) {
		/* The sector it falls in. */
		if (entry_lba != 0)
			*entry_lba = free_lba;

		/* And the position inside that sector. */
		if (entry_offset != 0)
			*entry_offset = free_offset;
	}

	/* Failed. */
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Creates an empty file and opens the caller's handle on it. */
static FAT_MUTATION int
fat_raw_create(
	struct fat_mount_state *filesystem,
	const char *path,
	struct fat_file_state *file)
{
	struct fat_directory parent;
	struct fat_component component;
	uint32_t lba = 0, free_lba = 0;
	uint16_t offset = 0, free_offset = 0;
	const uint8_t *sector;
	int result, rollback;

	/* A call that names no volume, no path or no file. */
	if (filesystem == NULL || path == NULL || file == NULL)
		return EINVAL;

	fat_file_bind(file, filesystem);

	/* A volume mounted read-only is never written to. */
	if (filesystem->read_only)
		return EROFS;

	/* Walks the path down to the directory that will hold it. */
	result = fat_raw_resolve_parent(filesystem, path, &parent, &component);
	if (result != 0)
		return result;

	/* A name the directory already holds cannot be created. */
	result = fat_raw_find_entry(filesystem,
				    &parent,
				    &component,
				    FAT_NAME_EXACT,
				    &lba,
				    &offset,
				    &free_lba,
				    &free_offset,
				    0);
	if (result == 0)
		return EEXIST;
	if (result != ENOENT &&
	    !(filesystem->type == KERN_FAT32 && result == ENOSPC)) {
		/* Failed. */
		return result;
	}

	/* Writes the records of the new name into the directory. */
	result = fat_raw_insert_entry(filesystem,
				      &parent,
				      &component,
				      0x20U,
				      0,
				      0,
				      &lba,
				      &offset);
	if (result != 0)
		return result;

	/* Fills the caller's handle in from the record just written. */
	result = fat_engine_read_sector_result(filesystem, lba, &sector);
	if (result == 0)
		result = fat_raw_populate_file(file, lba, offset,
			sector + offset);
	if (result == 0)
		return 0;

	/* Takes the record back out when the file could not open. */
	rollback = fat_raw_delete_location(filesystem, &parent, lba, offset);
	if (rollback != 0) {
		filesystem->read_only = 1;

		/* Failed. */
		return rollback;
	}

	/* Failed. */
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Remove a bounded long/short-name run with one image per affected sector. */
static FAT_MUTATION int
fat_raw_delete_location(
	struct fat_mount_state *filesystem,
	const struct fat_directory *parent,
	uint32_t target_lba,
	uint16_t target_offset)
{
	uint32_t limit;
	uint32_t index;
	uint32_t lba;
	uint16_t offset;
	const uint8_t *raw;
	uint32_t lbas[FAT_LFN_MAX_ENTRIES + 1U];
	uint16_t offsets[FAT_LFN_MAX_ENTRIES + 1U];
	uint8_t entries[FAT_LFN_MAX_ENTRIES + 1U][32];
	uint8_t target[32];
	unsigned count;
	unsigned n;
	int result;

	/*
	 * Find the public short-name entry before walking its bounded LFN
	 * prefix.
	 */
	if (parent->first_cluster == 0)
		limit = filesystem->root_entries;
	else
		limit = filesystem->cluster_count *
		    (uint32_t)filesystem->sectors_per_cluster *
		    FAT16_ENTRIES_PER_SECTOR;

	/* Walks the directory to the record being removed. */
	for (index = 0; index < limit; index++) {
		/* Reads the record at this index. */
		result = fat_raw_directory_entry(filesystem, parent, index,
			&lba, &offset, &raw);
		if (result != 0)
			return result;

		/* This is the record the caller named. */
		if (lba == target_lba && offset == target_offset)
			break;
	}

	/* A record the directory does not hold cannot be removed. */
	if (index == limit)
		return ENOENT;

	memcpy(target, raw, sizeof(target));

	/*
	 * Collect all associated records before changing directory
	 * interpretation.
	 */
	count = 0;
	while (index != 0) {
		/* Reads the record in front of the one being removed. */
		result = fat_raw_directory_entry(filesystem, parent, index - 1U,
			&lba, &offset, &raw);
		if (result != 0)
			return result;

		/* Anything but a live long-name record ends the run. */
		if (raw[11] != 0x0fU || raw[0] == 0xe5)
			break;

		/* A name longer than a record chain could hold. */
		if (count >= FAT_LFN_MAX_ENTRIES)
			return EIO;

		lbas[count] = lba;
		offsets[count] = offset;
		memcpy(entries[count++], raw, 32U);

		index--;
	}

	lbas[count] = target_lba;
	offsets[count] = target_offset;
	memcpy(entries[count++], target, 32U);

	/* Every record of the name is marked erased together. */
	for (n = 0; n < count; n++)
		entries[n][0] = 0xe5;

	/*
	 * Return with either complete deletion or restored original sector
	 * contents.
	 */
	result = fat_directory_transaction(filesystem, lbas, offsets, entries,
					   count, 0);
	if (result != 0)
		return result;

	/* Succeeded: the whole run is marked deleted on disk. */
	return 0;
}

/* Asks whether a directory holds anything but dot and dot-dot. */
static FAT_MUTATION int
fat_raw_directory_empty(
	struct fat_mount_state *filesystem,
	uint32_t first_cluster)
{
	uint32_t lba;
	uint16_t offset;
	const uint8_t *raw;
	int result;
	struct fat_mount_state *fat = filesystem;
	struct fat_directory directory = {.first_cluster = first_cluster};
	uint32_t limit = fat->cluster_count *
		(uint32_t)fat->sectors_per_cluster *
		FAT16_ENTRIES_PER_SECTOR;
	uint32_t index;

	/* Walks the directory one record at a time. */
	for (index = 0; index < limit; index++) {
		result = fat_raw_directory_entry(filesystem, &directory, index,
			&lba, &offset, &raw);
		(void)lba;
		(void)offset;
		if (result == ENOENT)
			return 0;
		if (result != 0)
			return result;

		/* An unused record is the end of the directory. */
		if (raw[0] == 0)
			return 0;

		/* Erased, long-name, label and dot records are skipped. */
		if (raw[0] == 0xe5 ||
		    raw[11] == 0x0fU ||
		    (raw[11] & 0x08U) != 0 ||
		    raw[0] == '.')
			continue;

		/* Failed. */
		return ENOTEMPTY;
	}

	/* Failed. */
	return EIO;
}

/*
 * Writes the two entries every directory begins with.
 *
 * A new directory's first cluster holds `.` and `..` before anything else, so
 * a walk that reaches it can go back up.
 */
static FAT_MUTATION int
fat_raw_initialize_directory(
	struct fat_mount_state *filesystem,
	uint32_t cluster,
	uint32_t parent_cluster)
{
	unsigned index;
	uint32_t lba;
	uint8_t *sector;
	uint8_t *raw;
	int error;

	/* Locates the first sector of the new directory's cluster. */
	error = fat_engine_cluster_lba(filesystem, cluster, 0, &lba);
	if (error != 0)
		return error;

	/* Takes that sector for writing. */
	error = fat_engine_write_sector_result(filesystem, lba, &sector);
	if (error != 0)
		return error;

	/* An entry that was never written must read as free, which is zero. */
	clear_bytes(sector, 512);

	/* Writes `.`, which names the directory itself. */
	raw = sector;
	for (index = 0; index < 11; index++)
		raw[index] = ' ';
	raw[0] = '.';
	raw[11] = 0x10U;
	fat_raw_put_dir_cluster(filesystem, raw, cluster);

	/* Writes `..`, which names the directory it was created in. */
	raw += 32;
	for (index = 0; index < 11; index++)
		raw[index] = ' ';
	raw[0] = '.';
	raw[1] = '.';
	raw[11] = 0x10U;
	fat_raw_put_dir_cluster(filesystem, raw, parent_cluster);

	/* Writes the sector out before anything links the directory in. */
	error = fat_engine_mark_sector_dirty(filesystem);
	if (error != 0)
		return error;

	error = fat_engine_flush(filesystem);
	if (error != 0)
		return error;

	/* Succeeded: the new directory can be walked in both directions. */
	return 0;
}

/* Creates a directory, with the dot and dot-dot records it needs. */
static FAT_MUTATION int
fat_raw_mkdir(
	struct fat_mount_state *filesystem,
	const char *path,
	uint32_t *created_cluster)
{
	struct fat_directory parent;
	struct fat_component component;
	uint32_t cluster, lba;
	uint16_t offset;
	int result;

	/* A call with nowhere to report the new cluster. */
	if (created_cluster == NULL)
		return EINVAL;

	*created_cluster = 0;

	/* A volume mounted read-only is never written to. */
	if (filesystem->read_only)
		return EROFS;

	/* Walks the path down to the directory that will hold it. */
	result = fat_raw_resolve_parent(filesystem, path, &parent, &component);
	if (result != 0)
		return result;

	/* A directory needs a cluster of its own before its name. */
	result = fat_raw_allocate_cluster(filesystem, &cluster);
	if (result != 0)
		return result;

	/*
	 * Make the child's dot entries durable before publishing its parent
	 * name.
	 */
	result = fat_raw_initialize_directory(filesystem,
					      cluster,
					      parent.first_cluster);
	if (result == 0)
		result = disk_sync(filesystem->disk);

	/* Publishes the name in the parent, marked as a directory. */
	if (result == 0) {
		result = fat_raw_insert_entry(filesystem,
					      &parent,
					      &component,
					      0x10U,
					      cluster,
					      0,
					      &lba,
					      &offset);
	}

	/* A directory whose name failed gives its cluster back. */
	if (result != 0) {
		int rollback;

		/*
		 * A failed entry rollback may have left a reachable reference
		 * to this cluster.  In that state leaking it is safer than
		 * freeing storage which an on-disk directory may still name.
		 */
		if (filesystem->read_only)
			return result;

		/* Frees the cluster the failed creation had taken. */
		rollback = fat_raw_free_chain(filesystem, cluster);
		if (rollback != 0) {
			filesystem->read_only = 1;

			/* Failed. */
			return rollback;
		}
	} else {
		*created_cluster = cluster;
	}

	/* Reports why the directory could not be created. */
	if (result != 0)
		return result;

	/* Succeeded: the caller now holds the new directory's cluster. */
	return 0;
}

/* Removes a name and frees whatever chain it held. */
static FAT_MUTATION int
fat_raw_remove(
	struct fat_mount_state *filesystem,
	const char *path,
	int directory)
{
	int error;
	struct fat_directory parent;
	struct fat_component component;
	uint32_t lba = 0, free_lba = 0, cluster;
	uint16_t offset = 0, free_offset = 0;
	uint8_t raw[32];
	const uint8_t *sector;
	int result;
	int valid;

	/* A volume mounted read-only is never written to. */
	if (filesystem->read_only)
		return EROFS;

	/* Walks to the directory the last component lives in. */
	result = fat_raw_resolve_parent(filesystem,
					path,
					&parent,
					&component);
	if (result != 0)
		return result;

	/* Looks the last component up in that directory. */
	result = fat_raw_find_entry(filesystem,
				    &parent,
				    &component,
				    FAT_NAME_EXACT,
				    &lba,
				    &offset,
				    &free_lba,
				    &free_offset,
				    0);
	if (result != 0) {
		if (result == ENOSPC)
			return ENOENT;	/* Failed. */

		return result;	/* Failed. */
	}

	/* Reads the sector the record sits in. */
	result = fat_engine_read_sector_result(filesystem, lba, &sector);
	if (result != 0)
		return result;

	copy_bytes(raw, sector + offset, sizeof(raw));

	/* A directory and a file are not removed the same way. */
	if (directory != ((raw[11] & 0x10U) != 0)) {
		/* Asked for a directory and found a file. */
		if (directory)
			return EINVAL;	/* Failed. */

		/* Asked for a file and found a directory. */
		return EISDIR;	/* Failed. */
	}

	/* A directory has to be empty before it can be removed. */
	cluster = fat_raw_dir_cluster(filesystem, raw);
	if (directory) {
		/* A first cluster that is not one of the volume. */
		/* Asks whether that is a cluster the volume has. */
		valid = fat_raw_valid_cluster(filesystem, cluster);
		if (!valid)
			return EIO;

		/* Asks whether it still holds anything of its own. */
		result = fat_raw_directory_empty(filesystem, cluster);
		if (result != 0)
			return result;
	}

	/* Marks the record, and any long name of it, as erased. */
	error = fat_raw_delete_location(filesystem, &parent, lba, offset);

	/* Reports whether the name could be removed. */
	return error;
}

/* Removes a name that must not be a directory. */
static FAT_MUTATION int
fat_raw_unlink(
	struct fat_mount_state *filesystem,
	const char *path)
{
	int removed;

	/* The zero asks the shared removal to refuse a directory. */
	removed = fat_raw_remove(filesystem, path, 0);
	if (removed != 0)
		return removed;

	/* Succeeded: the name is gone from its directory. */
	return 0;
}

/* Removes a name that must be an empty directory. */
static FAT_MUTATION int
fat_raw_rmdir(
	struct fat_mount_state *filesystem,
	const char *path)
{
	int removed;

	/* The one asks the shared removal to accept only a directory. */
	removed = fat_raw_remove(filesystem, path, 1);
	if (removed != 0)
		return removed;

	/* Succeeded: the directory is gone from its parent. */
	return 0;
}

/* Points a moved directory's dot-dot record at its new parent. */
static FAT_MUTATION int
fat_raw_update_dotdot(
	struct fat_mount_state *filesystem,
	uint32_t directory_cluster,
	uint32_t parent_cluster)
{
	struct fat_directory directory = {.first_cluster = directory_cluster};
	uint32_t lba;
	uint16_t offset;
	uint8_t saved[32];
	const uint8_t *raw;
	uint8_t *sector;
	int result, rollback;

	/* The dot-dot record is the second one of a directory. */
	result = fat_raw_directory_entry(filesystem,
					 &directory,
					 1,
					 &lba,
					 &offset,
					 &raw);
	if (result != 0)
		return result;

	copy_bytes(saved, raw, sizeof(saved));

	/* Reads the sector so the record can be changed in place. */
	result = fat_engine_write_sector_result(filesystem, lba, &sector);
	if (result != 0)
		return result;

	fat_raw_put_dir_cluster(filesystem, sector + offset, parent_cluster);

	/* The change reaches the volume once the sector is written. */
	result = fat_engine_mark_sector_dirty(filesystem);
	if (result == 0)
		result = fat_engine_flush(filesystem);
	if (result == 0)
		return 0;

	/* Puts the record back when it could not be published. */
	rollback = fat_raw_restore_directory_entry(filesystem, lba, offset,
		saved);
	if (rollback != 0)
		filesystem->read_only = 1;

	/* Reports the failed rollback, or the write that made it necessary. */
	if (rollback != 0)
		return rollback;

	return result;
}

/*
 * Puts the size and the cluster of an entry back, leaving its name alone.
 *
 * A failed truncate or write undoes only what it changed: the name and the
 * long-name run that goes with it were never touched, so the entry on disk is
 * read back and just those two fields are restored into it.
 */
static FAT_MUTATION int
fat_raw_restore_entry_payload(
	struct fat_mount_state *filesystem,
	uint32_t lba,
	uint16_t offset,
	const uint8_t raw[32])
{
	uint8_t restored[32];
	const uint8_t *current;
	int error;

	/* Reads the entry as it stands, so the name survives untouched. */
	error = fat_engine_read_sector_result(filesystem, lba, &current);
	if (error != 0)
		return error;

	/* Starts from the current entry and takes back the saved attributes. */
	copy_bytes(restored, current + offset, sizeof(restored));
	restored[11] = raw[11];

	/* Points it back at the cluster chain the file had. */
	fat_raw_put_dir_cluster(filesystem,
				restored,
				fat_raw_dir_cluster(filesystem, raw));

	/* Gives it back the size that went with that chain. */
	put32(restored + 28, fat_engine_get32(raw + 28));

	/* Writes the repaired entry over its own slot. */
	error = fat_raw_restore_directory_entry(filesystem, lba, offset,
						restored);
	if (error != 0)
		return error;

	/* Succeeded: the entry describes the file as it was. */
	return 0;
}

/* Takes back the records a rename had written at its destination. */
static FAT_MUTATION void
fat_raw_rename_rollback_destination(
	struct fat_mount_state *filesystem,
	const struct fat_directory *parent,
	uint32_t lba,
	uint16_t offset,
	int replacing,
	const uint8_t target[32])
{
	/* A rename that replaced a name puts the old one back. */
	if (replacing) {
		(void)fat_raw_restore_entry_payload(filesystem,
						    lba,
						    offset,
						    target);
	} else {
		(void)fat_raw_delete_location(filesystem,
					      parent,
					      lba,
					      offset);
	}
}

/* Reports a path's last component as the volume itself stores it. */
static int
fat_raw_canonical_basename(
	struct fat_mount_state *filesystem,
	const char *path,
	char basename[KERN_PATH_MAX])
{
	struct fat_dir_entry decoded;
	struct fat_directory parent;
	struct fat_component component;
	uint8_t raw[32];
	int result;

	/* A call that names no volume, path or buffer to fill in. */
	if (filesystem == NULL || path == NULL || basename == NULL)
		return EINVAL;

	/* Walks the path down to the directory holding its last name. */
	result = fat_raw_resolve_parent(filesystem, path, &parent, &component);
	if (result != 0)
		return result;

	/* A FAT32 volume stores the name as the caller spelled it. */
	if (filesystem->type == KERN_FAT32) {
		text_copy(basename, component.text, KERN_PATH_MAX);

		/* Succeeded. */
		return 0;
	}

	/*
	 * A volume with no long names stores the short one padded with
	 * spaces, so the canonical basename is what decoding that yields.
	 */
	clear_bytes(raw, sizeof(raw));
	copy_bytes(raw, component.sfn, sizeof(component.sfn));
	fat_sfn_decode_lower(raw, &decoded);
	text_copy(basename, decoded.name, KERN_PATH_MAX);

	/* Succeeded. */
	return 0;
}

/* Moves a name from one directory to another, or renames it in place. */
static FAT_MUTATION int
fat_raw_rename(
	struct fat_mount_state *filesystem,
	const char *old_path,
	const char *new_path,
	uint32_t authoritative_cluster,
	uint32_t authoritative_size,
	struct fat_rename_result *renamed)
{
	int rollback;
	struct fat_directory old_parent, new_parent;
	struct fat_component old_component, new_component;
	uint32_t old_lba = 0, old_free_lba = 0, new_lba = 0, new_free_lba = 0;
	uint16_t old_offset = 0, old_free_offset = 0;
	uint16_t new_offset = 0, new_free_offset = 0;
	uint8_t source[32], target[32];
	const uint8_t *sector;
	uint8_t *write_sector;
	uint32_t source_cluster;
	int replacing = 0;
	int result, target_result;

	/* A volume mounted read-only is never written to. */
	if (filesystem->read_only)
		return EROFS;

	/* Walks the old path down to the directory holding it. */
	result = fat_raw_resolve_parent(filesystem,
					old_path,
					&old_parent,
					&old_component);
	if (result != 0)
		return result;

	/* Looks the old name up in that directory. */
	result = fat_raw_find_entry(filesystem,
				    &old_parent,
				    &old_component,
				    FAT_NAME_EXACT,
				    &old_lba,
				    &old_offset,
				    &old_free_lba,
				    &old_free_offset,
				    0);
	if (result != 0) {
		if (result == ENOSPC)
			return ENOENT;	/* Failed. */

		return result;	/* Failed. */
	}

	/* Reads the sector the old record sits in. */
	result = fat_engine_read_sector_result(filesystem, old_lba, &sector);
	if (result != 0)
		return result;

	copy_bytes(source, sector + old_offset, sizeof(source));

	/*
	 * The inode/open-file state is authoritative while a writer is open.
	 * Publish that state at the destination as part of the rename commit so
	 * no fallible repair read/write remains after the old name is removed.
	 */
	fat_raw_put_dir_cluster(filesystem, source, authoritative_cluster);

	/* The size is published from the same authoritative state. */
	put32(source + 28, authoritative_size);

	/* Walks the new path down to the directory that will hold it. */
	result = fat_raw_resolve_parent(filesystem,
					new_path,
					&new_parent,
					&new_component);
	if (result != 0)
		return result;

	/* Asks whether the new name is already taken. */
	target_result = fat_raw_find_entry(filesystem,
					   &new_parent,
					   &new_component,
					   FAT_NAME_EXACT,
					   &new_lba,
					   &new_offset,
					   &new_free_lba,
					   &new_free_offset,
					   0);
	if (target_result == 0) {
		/* A rename onto the very same record changes nothing. */
		if (old_lba == new_lba && old_offset == new_offset) {
			/* Reports the record, which has not moved. */
			if (renamed != NULL) {
				renamed->lba = old_lba;
				renamed->offset = old_offset;
				renamed->attributes = source[11];
			}

			/* Succeeded. */
			return 0;
		}

		/* Reads the sector the record being replaced sits in. */
		result = fat_engine_read_sector_result(filesystem, new_lba,
			&sector);
		if (result != 0)
			return result;

		copy_bytes(target, sector + new_offset, sizeof(target));

		/*
		 * A directory may only replace a directory, and a file a file.
		 */
		if (((source[11] ^ target[11]) & 0x10U) != 0)
			return EINVAL;

		/* A directory being replaced has to be empty first. */
		if ((target[11] & 0x10U) != 0) {
			/* A directory being replaced has to be empty first. */
			result = fat_raw_directory_empty(
				filesystem,
				fat_raw_dir_cluster(filesystem, target));
			if (result != 0)
				return result;
		}

		/*
		 * Preserve the destination's spelling/LFN run and replace only
		 * its payload.  Open references to the old target keep their
		 * own cluster state and the VFS marks that inode orphaned after
		 * this succeeds.
		 */
		result = fat_engine_write_sector_result(filesystem,
							new_lba,
							&write_sector);
		if (result != 0)
			return result;

		write_sector[new_offset + 11] = source[11];

		fat_raw_put_dir_cluster(filesystem,
					write_sector + new_offset,
					fat_raw_dir_cluster(filesystem,
						source));

		put32(write_sector + new_offset + 28,
			fat_engine_get32(source + 28));

		/* The change reaches the volume once the sector is written. */
		result = fat_engine_mark_sector_dirty(filesystem);
		if (result == 0)
			result = fat_engine_flush(filesystem);
		if (result != 0) {
			rollback = fat_raw_restore_entry_payload(filesystem,
								 new_lba,
								 new_offset,
								 target);

			/* Reports the failed rollback, or the write itself. */
			if (rollback != 0)
				return rollback;

			return result;
		}

		replacing = 1;
	} else if (target_result != ENOENT && target_result != ENOSPC) {
		/* Reports why the new name could not be looked up. */
		return target_result;
	}

	/* The cluster the name being moved points at. */
	source_cluster = fat_raw_dir_cluster(filesystem, source);

	if (!replacing) {
		/* Writes the records of the new name into its directory. */
		result = fat_raw_insert_entry(filesystem,
					      &new_parent,
					      &new_component,
					      source[11],
					      source_cluster,
					      fat_engine_get32(source + 28),
					      &new_lba,
					      &new_offset);
		if (result != 0)
			return result;
	}

	/* A directory that changes parent has its dot-dot rewritten. */
	if ((source[11] & 0x10U) != 0 &&
	    old_parent.first_cluster != new_parent.first_cluster) {
		/* Points the dot-dot record at the new parent. */
		result = fat_raw_update_dotdot(filesystem,
					       source_cluster,
					       new_parent.first_cluster);
		if (result != 0) {
			fat_raw_rename_rollback_destination(filesystem,
							    &new_parent,
							    new_lba,
							    new_offset,
							    replacing,
							    target);

			/* Failed. */
			return result;
		}
	}

	/* Takes the old name out once the new one is in place. */
	result = fat_raw_delete_location(filesystem,
					 &old_parent,
					 old_lba,
					 old_offset);
	if (result != 0) {
		/* Puts the dot-dot record back when the removal failed. */
		if ((source[11] & 0x10U) != 0 &&
		    old_parent.first_cluster != new_parent.first_cluster) {
			(void)fat_raw_update_dotdot(filesystem,
						    source_cluster,
						    old_parent.first_cluster);
		}

		/*
		 * Undoes the destination entry, or reports where the new one
		 * landed.
		 */
		fat_raw_rename_rollback_destination(filesystem,
						    &new_parent,
						    new_lba,
						    new_offset,
						    replacing,
						    target);
	} else if (renamed != NULL) {
		renamed->lba = new_lba;
		renamed->offset = new_offset;
		renamed->attributes = source[11];
	}

	/* Failed. */
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Reads a run of bytes out of an open file. */
static int
fat_raw_read(
	struct fat_file_state *file,
	uint64_t offset,
	void *buffer,
	uint32_t length,
	fat_read_progress_fn progress,
	void *progress_context)
{
	int error;

	/* Reads the run by walking the file's cluster chain. */
	error = fat_engine_read_chain(
		file, offset, buffer, length, progress, progress_context,
		fat_raw_next_cluster, fat_raw_reserved_limit(file->mount));

	/* Reports how the read went. */
	return error;
}

/* Reads one visible entry of a directory by its position. */
static int
fat_raw_readdir(
	struct fat_mount_state *filesystem,
	const char *path,
	unsigned wanted,
	struct fat_dir_entry *entry)
{
	struct fat_directory directory;
	uint32_t parent_lba;
	uint16_t parent_offset;
	const uint8_t *parent_raw;
	uint32_t lba;
	uint16_t offset;
	const uint8_t *raw;
	int decoded;
	int error;
	struct fat_mount_state *fat = filesystem;
	struct fat_lfn_state lfn;
	uint32_t limit;
	unsigned visible = 0;
	uint32_t index;
	int valid;

	/* The search starts at the root of the volume. */
	directory.first_cluster = fat_raw_root_cluster(filesystem);

	/* A path other than the root names a directory to walk to. */
	if (*path && !(path[0] == '/' && !path[1])) {
		/* Walks the path down to the record it names. */
		error = fat_raw_resolve_entry(filesystem,
						     path,
						     &parent_lba,
						     &parent_offset,
						     &parent_raw,
						     FAT_NAME_EXACT,
						     0);
		if (error != 0)
			return error;

		/* Only a directory can be read as one. */
		if (!(parent_raw[11] & 0x10U))
			return EINVAL;

		directory.first_cluster = fat_raw_dir_cluster(fat, parent_raw);

		/* A first cluster that is not one of the volume. */
		/* Asks whether that is a cluster the volume has. */
		valid = fat_raw_valid_cluster(fat, directory.first_cluster);
		if (!valid)
			return EIO;
	}

	/* Bounds the walk and starts with no long name assembled. */
	if (directory.first_cluster == 0) {
		/* A FAT12 or FAT16 root holds a fixed number of records. */
		limit = fat->root_entries;
	} else {
		/* Any other directory is bounded by the whole data area. */
		limit = fat->cluster_count *
			(uint32_t)fat->sectors_per_cluster *
			FAT16_ENTRIES_PER_SECTOR;
	}
	fat_lfn_reset(&lfn);

	/* Walks the directory one record at a time. */
	for (index = 0; index < limit; index++) {
		/* Reads the record at this index. */
		error = fat_raw_directory_entry(filesystem,
							&directory,
							index,
							&lba,
							&offset,
							&raw);
		if (error == ENOENT)
			return error;

		/* Reports the failure to read the record. */
		if (error != 0)
			return error;

		/* An unused record is the end of the directory. */
		if (!raw[0]) {
			fat_lfn_reset(&lfn);

			/* Failed. */
			return ENOENT;
		}

		/* An erased record names nothing. */
		if (raw[0] == 0xe5) {
			fat_lfn_reset(&lfn);
			continue;
		}

		/* A long-name record is collected, not counted. */
		if (raw[11] == 0x0f) {
			/* Only FAT32 volumes carry long names at all. */
			if (fat->type == KERN_FAT32)
				(void)fat_lfn_feed(&lfn, raw);
			continue;
		}

		/* A volume label or a dot record is not an entry. */
		if ((raw[11] & 0x08U) || raw[0] == '.') {
			fat_lfn_reset(&lfn);
			continue;
		}

		/* Counts the visible entries until the wanted one. */
		if (visible++ != wanted) {
			fat_lfn_reset(&lfn);
			continue;
		}

		/*
		 * A long name is only available on FAT32, and only if complete.
		 */
		decoded = 0;
		if (fat->type == KERN_FAT32) {
			decoded = fat_lfn_finish(&lfn, raw, entry->name,
						 sizeof(entry->name));
		}

		/*
		 * Without one, the short name stands in.  FAT32 keeps its
		 * case bits, so it is decoded preserving them; the narrower
		 * widths have none and decode to lower case.
		 */
		if (!decoded && fat->type == KERN_FAT32) {
			fat_sfn_decode_preserve(raw, entry->name,
						sizeof(entry->name));
		} else if (!decoded) {
			fat_sfn_decode_lower(raw, entry);
		}

		entry->size = fat_engine_get32(raw + 28);
		entry->attributes = raw[11];

		/* Succeeded. */
		return 0;
	}

	/* Failed. */
	return ENOENT;
}

/* Reads a path's record, and where on the volume that record sits. */
static int
fat_stat_location_mode(
	struct fat_mount_state *filesystem,
	const char *path,
	struct fat_dir_entry *entry,
	uint32_t *lba,
	uint16_t *offset,
	uint32_t *first_cluster,
	uint8_t *attributes,
	enum fat_name_match match)
{
	const uint8_t *raw;
	char found_name[KERN_PATH_MAX];
	int result;

	/* A call that names no volume, no path or no entry to fill in. */
	if (filesystem == NULL || path == NULL || entry == NULL)
		return EINVAL;	/* Failed. */

	/* Nor one with nowhere to report where the entry was found. */
	if (lba == NULL || offset == NULL)
		return EINVAL;	/* Failed. */

	/* Nor one with nowhere to report what the entry holds. */
	if (first_cluster == NULL || attributes == NULL)
		return EINVAL;	/* Failed. */

	/* Walks the path down to the record it names. */
	result = fat_raw_resolve_entry(filesystem,
				       path,
				       lba,
				       offset,
				       &raw,
				       match,
				       found_name);
	if (result != 0)
		return result;

	/* A FAT32 volume reports the long name it stores. */
	if (filesystem->type == KERN_FAT32) {
		text_copy(entry->name, found_name, sizeof(entry->name));
		entry->size = fat_engine_get32(raw + 28);
		entry->attributes = raw[11];
	} else {
		fat_sfn_decode_lower(raw, entry);
	}

	*first_cluster = fat_raw_dir_cluster(filesystem, raw);
	*attributes = raw[11];

	/* Succeeded. */
	return 0;
}

/* Reads a path's record and location, taking the engine lock first. */
static int
fat_engine_stat_location(
	struct fat_mount_state *filesystem,
	const char *path,
	struct fat_dir_entry *entry,
	uint32_t *lba,
	uint16_t *offset,
	uint32_t *first_cluster,
	uint8_t *attributes)
{
	int error;

	/* Reads the record and where it sits, under the mount lock. */
	error = fat_stat_location_mode(filesystem,
						 path,
						 entry,
						 lba,
						 offset,
						 first_cluster,
						 attributes,
						 FAT_NAME_EXACT);

	/* Reports what the lookup found. */
	return error;
}

/*
 * Reports where a name lives, matching it without regard to case.
 *
 * Only FAT32 volumes carry the long names a case-folded match needs, so the
 * narrower widths refuse the request outright.
 */
static int
fat_engine_stat_location_casefold(
	struct fat_mount_state *filesystem,
	const char *path,
	struct fat_dir_entry *entry,
	uint32_t *lba,
	uint16_t *offset,
	uint32_t *first_cluster,
	uint8_t *attributes)
{
	int located;

	/* Refuses a volume that cannot hold the names this match needs. */
	if (filesystem == 0 || filesystem->type != KERN_FAT32)
		return EOPNOTSUPP;

	/* Looks the name up with case folding enabled. */
	located = fat_stat_location_mode(filesystem, path, entry, lba,
					 offset, first_cluster,
					 attributes, FAT_NAME_CASEFOLD);
	if (located != 0)
		return located;

	/* Succeeded: the caller now holds the entry and its location. */
	return 0;
}

/* Reports the runs of disk blocks a file's chain occupies. */
static int
fat_engine_file_extents(
	struct fat_file_state *file,
	fat_extent_cb callback,
	void *context)
{
	int error;
	uint32_t disk_block, blocks;
	uint32_t next;
	int result;
	struct fat_mount_state *filesystem;
	struct fat_mount_state *fat;
	struct fat_file_state *state;
	uint64_t remaining, file_block = 0, run_file = 0, run_disk = 0;
	uint32_t run_count = 0, cluster, steps;
	int last;
	int valid;
	uint64_t reported;

	/* A call that names no file, or nowhere to report to. */
	if (file == NULL || callback == NULL || file->mount == NULL)
		return EINVAL;
	filesystem = file->mount;
	fat = filesystem;
	state = file;

	/* A mount of no known width has no layout to report. */
	if (fat->type != KERN_FAT12 &&
	    fat->type != KERN_FAT16 &&
	    fat->type != KERN_FAT32) {
		/* Failed. */
		return EIO;
	}

	/* How much of the file is still to be reported. */
	remaining = file->size;
	if (remaining == 0) {
		/* Succeeded: an empty file with no chain has no extents. */
		if (state->first_cluster == 0)
			return 0;

		/* A file with a chain but no length is corrupt. */
		return EIO;	/* Failed. */
	}

	/* A first cluster that is not one of the volume. */
	cluster = state->first_cluster;
	/* Asks whether that is a cluster the volume has. */
	valid = fat_raw_valid_cluster(fat, cluster);
	if (!valid)
		return EIO;

	/* Walks the chain, turning each cluster into a run of blocks. */
	for (steps = 0; steps < fat->cluster_count && remaining != 0; steps++) {
		/* The last cluster holds only what is left of the file. */
		blocks = fat->sectors_per_cluster;
		if ((uint64_t)blocks * 512U > remaining)
			blocks = (uint32_t)((remaining + 511U) / 512U);

		/* Turns the cluster into the block its first sector sits at. */
		result = fat_engine_cluster_lba(filesystem, cluster, 0,
			&disk_block);
		if (result != 0)
			return result;

		/* A cluster next to the run being built extends it. */
		if (run_count != 0 && run_disk + run_count == disk_block &&
		    run_file + run_count == file_block) {
			run_count += blocks;
		} else {
			/* Otherwise the run that was open is reported first. */
			if (run_count != 0) {
				/* Hands the finished run to the caller. */
				result = callback(run_file, run_disk, run_count,
						  context);
				if (result != 0)
					return result;
			}

			run_file = file_block;
			run_disk = disk_block;
			run_count = blocks;
		}

		file_block += blocks;

		/* The last cluster holds only what is left of the file. */
		reported = (uint64_t)blocks * 512U;
		if (reported > remaining)
			reported = remaining;

		remaining -= reported;

		/* Reads the entry that follows this cluster. */
		result = fat_raw_next_cluster(filesystem, cluster, &next);
		if (result != 0)
			return result;

		/* A file that ends here has to end at the chain's end too. */
		if (remaining == 0) {
			/* A chain longer than the file disagrees with it. */
			/* Asks whether the entry is the end-of-chain marker. */
			last = fat_raw_is_end(fat, next);
			if (!last)
				return EIO;
			break;
		}

		/* A chain that leaves the volume means corruption. */
		/* Asks whether that is a cluster the volume has. */
		valid = fat_raw_valid_cluster(fat, next);
		if (!valid)
			return EIO;

		cluster = next;
	}

	/* A chain shorter than the file means the two disagree. */
	if (remaining != 0 || run_count == 0)
		return EIO;

	/* Reports the run that was still open when the chain ended. */
	error = callback(run_file, run_disk, run_count, context);
	if (error != 0)
		return error;

	/* Succeeded: every extent of the file has been reported. */
	return 0;
}

/* Frees a chain, or defers it when the file is still open. */
static int
fat_engine_discard_chain_result(
	struct fat_mount_state *filesystem,
	uint32_t first_cluster)
{
	int error;

	/* A call that names no volume has no chain to free. */
	if (filesystem == NULL)
		return EINVAL;

	/* Frees the chain, or defers it if the file is still open. */
	error = fat_raw_free_chain(filesystem, first_cluster);

	/* Reports whether the chain could be given back. */
	return error;
}

/* Mounts a volume that has to be FAT12. */
static int
fat12_mount(
	struct fat_mount_state *filesystem)
{
	struct fat_mount_state *fat;
	int result;
	uint32_t fat_entries;

	/* Reads the layout and refuses a volume of another width. */
	result = fat_engine_mount(filesystem, KERN_FAT12);
	if (result != 0)
		return result;

	/* A FAT12 volume has a fixed root and a small table. */
	fat = filesystem;
	if (!fat->root_entries || !fat->fat_sectors ||
	    fat->fat_sectors > 0xffffffffU / 512U) {
		/* Failed. */
		return EIO;
	}

	/* Three bytes of the table hold two twelve-bit entries. */
	fat_entries = fat->fat_sectors * 512U / 3U * 2U;
	if (fat_entries < fat->cluster_count + 2U ||
	    fat->cluster_count + 2U >= FAT12_RESERVED_CLUSTER) {
		/* Failed. */
		return EIO;
	}

	/* Succeeded. */
	return 0;
}

/* Mounts a volume that has to be FAT32. */
static int
fat32_mount(
	struct fat_mount_state *filesystem)
{
	int error;
	struct fat_mount_state *fat;
	int result;
	uint32_t fat_entries;
	int valid;

	/* Reads the layout and refuses a volume of another width. */
	result = fat_engine_mount(filesystem, KERN_FAT32);
	if (result != 0)
		return result;

	/* A FAT32 volume keeps its root in the data area. */
	fat = filesystem;
	if (!fat->fat32_layout || !fat->fat_sectors ||
	    fat->fat_sectors > 0xffffffffU / 512U) {
		/* Failed. */
		return EIO;
	}

	/* Every FAT32 entry is a whole word of the table. */
	fat_entries = fat->fat_sectors * 512U / 4U;
	if (fat_entries < fat->cluster_count + 2U ||
	    fat->cluster_count + 2U >= FAT32_RESERVED_CLUSTER) {
		/* Failed. */
		return EIO;
	}

	/* The root has to begin at a cluster the volume has. */
	valid = fat_raw_valid_cluster(fat, fat->root_cluster);
	error = 0;
	if (!valid)
		error = EIO;

	/* Reports whether the volume could be mounted. */
	return error;
}

/* Takes the private state this driver keeps beside a mount. */
static struct fat_mount_state *
fat_mount_state(
	struct mount *mountp)
{
	/* A mount that has gone away carries no state. */
	if (mountp == NULL)
		return NULL;

	/* Reports the state this mount was given when it was set up. */
	return mountp->m_data;
}

/* Reads one unsigned number out of the metadata file. */
static int
fat_metadata_number(
	const char *text,
	unsigned base,
	uint32_t *value)
{
	unsigned digit;
	uint32_t result = 0;

	/* A field of no digits is not a number. */
	if (*text == '\0')
		return EINVAL;

	/* Takes the digits one at a time, most significant first. */
	while (*text != '\0') {
		/* A digit outside the base, or one that would overflow. */
		digit = (unsigned)(*text++ - '0');
		if (digit >= base || result > (UINT32_MAX - digit) / base)
			return EINVAL;

		result = result * base + digit;
	}

	*value = result;

	/* Succeeded. */
	return 0;
}

/* Reads the file that records the modes and owners FAT cannot store. */
static void
fat_metadata_load(
	struct fat_mount_state *state)
{
	struct fat_metadata *metadata;
	char *line, *mode, *uid, *gid, *end;
	uint32_t mode_value, uid_value, gid_value;
	struct fat_file_state file = {0};
	char buffer[4096];
	uint32_t length, offset = 0;
	int opened;
	int error;
	size_t path_length;
	char *trailing;
	int loaded;
	uint32_t newline;

	/* A volume without the metadata file records nothing. */
	opened = fat_raw_open(state, "etc/unixmode", &file);
	if (opened != 0)
		return;

	/* Reads as much of the file as the buffer can hold and terminate. */
	if (file.size < sizeof(buffer) - 1U)
		length = (uint32_t)file.size;
	else
		length = (uint32_t)sizeof(buffer) - 1U;

	loaded = fat_raw_read(&file, 0, buffer, length, NULL, NULL);
	if (loaded != 0)
		return;

	buffer[length] = '\0';

	/* Takes the file one line at a time, while there is room. */
	while (offset < length && state->metadata != NULL &&
	       state->metadata->count < FAT_METADATA_MAX) {
		metadata = &state->metadata->entries[state->metadata->count];
		line = buffer + offset;

		/* A line ends at the newline, or at the end of the file. */
		end = strchr(line, '\n');
		if (end != NULL)
			*end = '\0';

		newline = 0U;
		if (end != NULL)
			newline = 1U;

		offset += (uint32_t)strlen(line) + newline;

		/* The mode follows the path, after the first colon. */
		mode = strchr(line, ':');
		if (mode == NULL)
			continue;
		*mode++ = '\0';

		/* The owner follows the mode, after the second. */
		uid = strchr(mode, ':');
		if (uid == NULL)
			continue;
		*uid++ = '\0';

		/* And the group follows the owner, after the third. */
		gid = strchr(uid, ':');
		if (gid == NULL)
			continue;
		*gid++ = '\0';

		/*
		 * Skips a line that carries a field this format does not have.
		 */
		trailing = strchr(gid, ':');
		if (trailing != NULL)
			continue;

		/*
		 * Skips a path written as absolute; these are mount-relative.
		 */
		if (line[0] == '/')
			continue;

		/* Skips an empty path, which names nothing. */
		if (line[0] == '\0')
			continue;

		/* Skips a path too long for the table to hold. */
		path_length = strlen(line);
		if (path_length >= sizeof(metadata->path))
			continue;

		/* Skips a line whose mode is not an octal number. */
		error = fat_metadata_number(mode, 8, &mode_value);
		if (error != 0)
			continue;

		/* Skips a line whose owner is not a decimal number. */
		error = fat_metadata_number(uid, 10, &uid_value);
		if (error != 0)
			continue;

		error = fat_metadata_number(gid, 10, &gid_value);
		if (error != 0)
			continue;

		/* Skips a mode with bits outside the permission field. */
		if (mode_value > 07777U)
			continue;

		/* Records one presented mode and owner. */
		strcpy(metadata->path, line);
		metadata->mode = (mode_t)mode_value;
		metadata->uid = (uid_t)uid_value;
		metadata->gid = (gid_t)gid_value;
		state->metadata->count++;
	}
}

/* Finds the recorded mode and owner of one path. */
static const struct fat_metadata *
fat_metadata_find(
	const struct fat_mount_state *state,
	const char *path)
{
	unsigned i;
	int difference;

	/* Walks the recorded entries looking for this path. */
	for (i = 0;
	     state != NULL && state->metadata != NULL &&
	     i < state->metadata->count;
	     i++) {
		/* An entry whose path is the one being looked for. */
		difference = strcmp(state->metadata->entries[i].path, path);
		if (difference == 0)
			return &state->metadata->entries[i];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Gives an inode the mode and owner recorded for its path. */
static void
fat_metadata_apply(
	struct mount *mountp,
	const char *path,
	struct inode *inode)
{
	const struct fat_metadata *metadata;
	struct fat_mount_state *state;

	/* Takes the private state this driver keeps beside the mount. */
	state = fat_mount_state(mountp);
	metadata = fat_metadata_find(state, path);

	/* A path the file records nothing for keeps its defaults. */
	if (metadata == NULL)
		return;

	inode->i_mode = (inode->i_mode & S_IFMT) | metadata->mode;
	inode->i_uid = metadata->uid;
	inode->i_gid = metadata->gid;
}

/* Takes the fixed slot this driver keeps a given inode in. */
static struct fat_inode_slot *
fat_slot(
	struct inode *inode)
{
	unsigned i;

	/* Walks the fixed slots looking for the one that holds it. */
	for (i = 0; i < FAT_INODE_MAX; i++) {
		/* This slot holds the inode that was asked about. */
		if (&fat_inodes[i].info.fi_inode == inode)
			return &fat_inodes[i];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Reports the path an inode was made for. */
static const char *
fat_path(
	struct inode *inode)
{
	struct fat_inode_slot *slot = fat_slot(inode);

	/* An inode outside the pool has no path recorded beside it. */
	if (slot == NULL)
		return NULL;

	/* Reports the path this inode was found at. */
	return slot->path;
}

/* Takes one of the driver's fixed inode slots. */
static struct inode *
fat_alloc_inode(
	struct mount *mountp)
{
	unsigned i;
	unsigned long irq;

	(void)mountp;

	irq = spin_lock_irqsave(&fat_pool_lock);

	/* Walks the fixed slots looking for one that is free. */
	for (i = 0; i < FAT_INODE_MAX; i++) {
		/* This slot is free, so it becomes the new inode. */
		if (!fat_inodes[i].used) {
			fat_inodes[i].used = 1;
			memset(&fat_inodes[i].info, 0,
				sizeof(fat_inodes[i].info));
			fat_inodes[i].path[0] = '\0';

			spin_unlock_irqrestore(&fat_pool_lock, irq);

			/* The kernel inode inside the slot. */
			return &fat_inodes[i].info.fi_inode;
		}
	}

	spin_unlock_irqrestore(&fat_pool_lock, irq);

	/* Reports that no result is available. */
	return NULL;
}

/* Gives an inode slot back, closing whatever it still held open. */
static void
fat_free_inode(
	struct inode *inode)
{
	struct fat_inode_slot *slot = fat_slot(inode);
	unsigned long irq;

	/* A slot is cleared under the lock that hands them out. */
	if (slot != NULL) {
		irq = spin_lock_irqsave(&fat_pool_lock);
		memset(slot, 0, sizeof(*slot));
		spin_unlock_irqrestore(&fat_pool_lock, irq);
	}
}

/* Builds the full path of a name inside a directory. */
static int
join_path(
	const char *parent,
	const struct componentname *name,
	char output[KERN_PATH_MAX])
{
	size_t parent_length;

	/* Rejects an empty component, which names nothing. */
	if (name->cn_namelen == 0)
		return ENAMETOOLONG;

	/* Rejects a component longer than one path element may be. */
	if (name->cn_namelen > NAME_MAX)
		return ENAMETOOLONG;

	/*
	 * Rejects a result that would not fit.  The room needed is the parent,
	 * the separator that follows it unless the parent is the root, the
	 * component itself, and the terminator.
	 */
	parent_length = strlen(parent);
	if (parent_length + (parent_length != 0) + name->cn_namelen >=
	    KERN_PATH_MAX)
		return ENAMETOOLONG;

	/*
	 * Copies the parent, separating it from the component unless it is
	 * root.
	 */
	memcpy(output, parent, parent_length);
	if (parent_length != 0)
		output[parent_length++] = '/';

	/* Appends the component and terminates the result. */
	memcpy(output + parent_length, name->cn_nameptr, name->cn_namelen);
	output[parent_length + name->cn_namelen] = '\0';

	/* Succeeded: the caller now holds the full path. */
	return 0;
}

/* Asks whether a path already exists, without regard to case. */
static int
fat_creation_collision(
	struct fat_mount_state *state,
	const char *path)
{
	struct fat_directory parent;
	struct fat_component component;
	uint32_t lba = 0, free_lba = 0;
	uint16_t offset = 0, free_offset = 0;
	int error;

	/* A call that names no volume or no path. */
	if (state == NULL || path == NULL)
		return EINVAL;

	/* Walks the path down to the directory holding its last name. */
	error = fat_raw_resolve_parent(state, path, &parent, &component);
	if (error != 0)
		return error;

	/* A name the directory already holds exactly. */
	error = fat_raw_find_entry(state,
				   &parent,
				   &component,
				   FAT_NAME_EXACT,
				   &lba,
				   &offset,
				   &free_lba,
				   &free_offset,
				   0);
	if (error == 0)
		return EEXIST;
	if (error != ENOENT && error != ENOSPC)
		return error;

	/* Only a FAT32 volume also refuses a name differing by case. */
	if (state->type != KERN_FAT32)
		return 0;

	/* Looks the name up again, this time ignoring case. */
	error = fat_raw_find_entry(state,
				   &parent,
				   &component,
				   FAT_NAME_CASEFOLD,
				   &lba,
				   &offset,
				   &free_lba,
				   &free_offset,
				   0);
	if (error == 0)
		return EEXIST;

	/*
	 * Succeeded: the name is free.  The search reports no space when the
	 * name is absent and no free slot was passed on the way, which is
	 * still an absence as far as a collision test is concerned.
	 */
	if (error == ENOENT || error == ENOSPC)
		return 0;

	/* Failed: the name is taken, or the search itself did not finish. */
	return error;
}

/* Reports the mode and owner this mount would show for a path. */
static int
fat_creation_representation(
	const struct fat_mount_state *state,
	const char *path,
	mode_t *mode,
	uid_t *uid,
	gid_t *gid)
{
	const struct fat_metadata *metadata;

	/* A call that names no mount or no path has nothing to look up. */
	if (state == NULL || path == NULL)
		return EINVAL;	/* Failed. */

	/* Nor one with nowhere to report the representation. */
	if (mode == NULL || uid == NULL || gid == NULL)
		return EINVAL;	/* Failed. */

	/* A path the metadata file records nothing for. */
	metadata = fat_metadata_find(state, path);
	if (metadata != NULL) {
		*mode = metadata->mode;
		*uid = metadata->uid;
		*gid = metadata->gid;
	} else {
		*mode = 0755U;
		*uid = 0;
		*gid = 0;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Tests whether FAT can represent the file a creation request asks for.
 *
 * FAT stores no owner and no permission bits of its own; a mount presents
 * fixed ones instead.  A request is representable only when it asks for
 * exactly what the mount would present anyway.
 */
static int
fat_creation_representable(
	const struct fat_mount_state *state,
	const char *path,
	const struct inode_creation_request *request,
	enum inode_type type)
{
	mode_t mode;
	uid_t uid;
	gid_t gid;
	int error;

	/* A call that names no request has nothing to create. */
	if (request == NULL)
		return EINVAL;	/* Failed. */

	/* An origin outside the range this kernel defines. */
	if (request->origin < INODE_CREATION_USER ||
	    request->origin > INODE_CREATION_PRESERVE)
		return EINVAL;	/* Failed. */

	/* A request for a kind other than the one being created here. */
	if (request->type != type)
		return EINVAL;	/* Failed. */

	/* A mode carrying file-type bits, which belong in the type instead. */
	if ((request->mode & S_IFMT) != 0)
		return EINVAL;	/* Failed. */

	/* And a special file, which FAT has no way to store. */
	if (request->special != NULL || request->rdev != 0)
		return EINVAL;	/* Failed. */

	/* Asks what this mount would present for a file at that path. */
	error = fat_creation_representation(state, path, &mode, &uid, &gid);
	if (error != 0)
		return error;

	/* Refuses a mode the mount would not present. */
	if (mode != (request->mode & 07777U))
		return EOPNOTSUPP;

	/* Refuses an owner the mount would not present. */
	if (uid != request->uid || gid != request->gid)
		return EOPNOTSUPP;

	/* Succeeded: the request asks for exactly what FAT will show. */
	return 0;
}

/*
 * Tests whether a created inode carries what this mount presents.
 *
 * It is the check that runs after a creation, against the inode that now
 * exists rather than against the request that asked for it.
 */
static int
fat_created_inode_matches(
	const struct fat_mount_state *state,
	const char *path,
	const struct inode *inode)
{
	mode_t mode;
	uid_t uid;
	gid_t gid;
	int error;

	/* Asks what this mount would present for a file at that path. */
	error = fat_creation_representation(state, path, &mode, &uid, &gid);
	if (error != 0)
		return error;

	/* Refuses an inode whose mode is not the one the mount presents. */
	if ((inode->i_mode & 07777U) != mode)
		return EOPNOTSUPP;

	/* Refuses an inode whose owner is not the one the mount presents. */
	if (inode->i_uid != uid || inode->i_gid != gid)
		return EOPNOTSUPP;

	/* Succeeded: the inode is the one this mount would present. */
	return 0;
}

/* Makes an inode number out of the place a record sits at. */
static ino_t
fat_ino(
	uint32_t lba,
	uint16_t offset)
{
	/* An inode number, made of where the record sits. */
	return 2U + (ino_t)lba * 16U + offset / 32U;
}

/* Tests whether a year has a twenty-ninth of February. */
static int
fat_leap_year(
	int year)
{
	/* A year not divisible by four is never a leap year. */
	if ((year % 4) != 0)
		return 0;

	/*
	 * A century is a leap year only when it is divisible by four hundred.
	 */
	if ((year % 100) == 0 && (year % 400) != 0)
		return 0;

	/* Reports that February has twenty-nine days. */
	return 1;
}

/* Reports how many days a month has in the given year. */
static int
fat_month_days(
	int year,
	int month)
{
	static const uint8_t days[] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
	};
	int leap;

	/* February gains a day in a leap year. */
	leap = fat_leap_year(year);
	if (month == 2 && leap)
		return 29;

	/* Every other month has a fixed length. */
	return days[month - 1];
}

/* Turns a stored date and time into seconds since the epoch. */
static time_t
fat_decode_time(
	uint16_t date,
	uint16_t time)
{
	int scan_year;
	int scan_month;
	int year, month, day, days = 0;
	int month_length;
	int64_t seconds;
	int leap;

	/* A stored date of zero means the record carries no timestamp. */
	if (date == 0)
		return 0;

	/* The three fields the date word is divided into. */
	year = 1980 + ((date >> 9) & 0x7f);
	month = (date >> 5) & 0x0f;
	day = date & 0x1f;

	/* A month outside the calendar means the record is corrupt. */
	if (month < 1 || month > 12) {
		/* Failed: the date is not one that exists. */
		return 0;
	}

	/* And so does a day the month it names does not have. */
	month_length = fat_month_days(year, month);
	if (day < 1 || day > month_length) {
		/* Failed: the date is not one that exists. */
		return 0;
	}

	/* Counts the whole years between the epoch and the stored one. */
	for (scan_year = 1970; scan_year < year; scan_year++) {
		leap = fat_leap_year(scan_year);
		if (leap)
			days += 366;
		else
			days += 365;
	}

	/* Then the whole months of the stored year in front of this one. */
	for (scan_month = 1; scan_month < month; scan_month++)
		days += fat_month_days(year, scan_month);

	/* And the days of this month, of which the first is day one. */
	days += day - 1;

	seconds = (int64_t)days * 86400 + ((time >> 11) & 0x1f) * 3600 +
		  ((time >> 5) & 0x3f) * 60 + (time & 0x1f) * 2;

#ifdef KERN_USER_ABI_LP64
	/* Reports the decoded time. */
	return (time_t)seconds;
#else
	/* A date beyond the epoch this kernel represents saturates. */
	if (seconds > INT32_MAX)
		return (time_t)INT32_MAX;

	/* Reports the decoded time. */
	return (time_t)seconds;
#endif
}

/* Turns seconds since the epoch into a stored date and time. */
static FAT_MUTATION int
fat_encode_time(
	time_t seconds,
	uint16_t *date,
	uint16_t *time)
{
	int64_t days, remainder;
	int year = 1970, month = 1;
	int leap;
	int month_days;
	int year_days;

	/* A time before the epoch FAT counts from cannot be stored. */
	if (seconds < FAT_EPOCH_1980)
		return EOVERFLOW;

	days = seconds / 86400;
	remainder = seconds % 86400;

	/* Counts off whole years until what is left is one year. */
	for (;;) {
		/* How many days the year being counted off holds. */
		leap = fat_leap_year(year);
		if (leap)
			year_days = 366;
		else
			year_days = 365;

		/* What is left is inside this year. */
		if (days < year_days)
			break;

		days -= year_days;
		year++;
	}

	/* A year past the last one the stored field can hold. */
	if (year > 2107)
		return EOVERFLOW;

	/* Then counts off whole months in the same way. */
	for (;;) {
		/* How many days the month being counted off holds. */
		month_days = fat_month_days(year, month);

		/* What is left is inside this month. */
		if (days < month_days)
			break;

		days -= month_days;
		month++;
	}

	*date = (uint16_t)(((year - 1980) << 9) | (month << 5) | (days + 1));
	*time = (uint16_t)(((remainder / 3600) << 11) |
			   (((remainder / 60) % 60) << 5) |
			   ((remainder % 60) / 2));

	/* Succeeded. */
	return 0;
}

/*
 * Fills an inode's timestamps from its directory entry.
 *
 * FAT keeps no access time of its own beyond a date, so the access time is
 * decoded with a zero time of day.
 */
static void
fat_load_inode_times(
	struct mount *mountp,
	struct inode *inode,
	uint32_t lba,
	uint16_t offset)
{
	struct fat_mount_state *state;
	const uint8_t *sector;
	const uint8_t *raw;
	int error;

	/* Leaves the timestamps alone when the mount has gone away. */
	state = fat_mount_state(mountp);
	if (state == NULL)
		return;

	/* Leaves them alone as well when the entry cannot be read. */
	error = fat_engine_read_sector_result(state, lba, &sector);
	if (error != 0)
		return;

	/* Decodes the three timestamps the entry carries. */
	raw = sector + offset;
	inode->i_atime.tv_sec = fat_decode_time(fat_engine_get16(raw + 18), 0);
	inode->i_mtime.tv_sec = fat_decode_time(fat_engine_get16(raw + 24),
	    fat_engine_get16(raw + 22));
	inode->i_ctime.tv_sec = fat_decode_time(fat_engine_get16(raw + 16),
	    fat_engine_get16(raw + 14));
}

/* Builds the kernel inode of one directory record. */
static int
fat_make_inode(
	struct mount *mountp,
	const char *path,
	const struct fat_dir_entry *entry,
	uint32_t lba,
	uint16_t offset,
	uint32_t first_cluster,
	uint8_t attributes,
	struct inode **result)
{
	struct fat_inode_info *info;
	struct fat_inode_slot *slot;
	struct inode *inode;
	ino_t ino = fat_ino(lba, offset);
	int error = inode_get(mountp, ino, result);
	size_t path_length;

	/* An inode already in core is handed back as it is. */
	if (error == 0)
		return 0;

	/* Takes one of the driver's fixed inode slots. */
	inode = inode_alloc(mountp);
	if (inode == NULL)
		return ENOSPC;

	info = fat_inode(inode);

	/* A path longer than a slot could hold. */
	slot = fat_slot(inode);
	path_length = strlen(path);
	if (slot == NULL || path_length >= KERN_PATH_MAX) {
		inode_release(inode);

		/* Failed. */
		return EINVAL;
	}

	/* Fills the inode from the directory entry that describes it. */
	strcpy(slot->path, path);
	info->fi_first_cluster = first_cluster;
	info->fi_dirent_lba = lba;
	info->fi_dirent_offset = offset;
	info->fi_attributes = attributes;
	inode->i_ino = ino;
	inode->i_data = info;
	inode->i_linkcount = 1;
	inode->i_uid = inode->i_gid = 0;
	inode->i_size = (off_t)entry->size;

	/* A directory and a file are given different operations. */
	if (attributes & FAT_ATTRIBUTE_DIRECTORY) {
		inode->i_type = INODE_DIR;
		inode->i_mode = S_IFDIR | 0755U;
		inode->i_size = 0;
	} else {
		inode->i_type = INODE_REG;

		/*
		 * FAT has no execute bit.  Mount regular files with the
		 * executable default expected by this boot/userland volume.
		 */
		inode->i_mode = S_IFREG | 0755U;
	}

	/* A read-only record takes the write bits off the mode. */
	if (attributes & FAT_ATTRIBUTE_READ_ONLY)
		inode->i_mode &= ~(mode_t)0222U;

	fat_metadata_apply(mountp, path, inode);
	fat_load_inode_times(mountp, inode, lba, offset);

	*result = inode;

	/* Succeeded. */
	return 0;
}

/* Gives an inode the operations its kind of file is served by. */
static void
set_inode_ops(
	struct inode *inode)
{
	inode->i_op = &fat_inode_ops;
	if (inode->i_type == INODE_DIR)
		inode->i_fop = &fat_directory_ops;
	else
		inode->i_fop = &fat_regular_ops;
}

/* Resolves a path to an inode. */
static int
fat_stat_path(
	struct mount *mountp,
	const char *path,
	struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	struct fat_dir_entry entry;
	char canonical[KERN_PATH_MAX];
	const char *slash;
	size_t prefix_length;
	uint32_t lba, first_cluster;
	uint16_t offset;
	uint8_t attributes;
	int error;
	int fsresult;
	size_t name_length;

	/* Looks the name up exactly as it was given. */
	fsresult = fat_engine_stat_location(state,
					    path,
					    &entry,
					    &lba,
					    &offset,
					    &first_cluster,
					    &attributes);

	/* Reports why the path could not be resolved. */
	if (fsresult != 0)
		return fsresult;

	slash = strrchr(path, '/');

	/* The canonical path is the parent plus the stored name. */
	prefix_length = 0;
	if (slash != NULL)
		prefix_length = (size_t)(slash - path + 1);

	/* The stored name has to fit after the parent that precedes it. */
	name_length = strlen(entry.name);
	if (prefix_length + name_length >= sizeof(canonical))
		return ENAMETOOLONG;

	memcpy(canonical, path, prefix_length);
	strcpy(canonical + prefix_length, entry.name);

	/* Builds the kernel inode of the record that was found. */
	error = fat_make_inode(mountp,
			       canonical,
			       &entry,
			       lba,
			       offset,
			       first_cluster,
			       attributes,
			       result);
	if (error == 0)
		set_inode_ops(*result);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Resolves a path to an inode, ignoring case in the last component. */
static int
fat_stat_path_casefold(
	struct mount *mountp,
	const char *path,
	struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	struct fat_dir_entry entry;
	char canonical[KERN_PATH_MAX];
	const char *slash;
	size_t prefix_length;
	uint32_t lba, first_cluster;
	uint16_t offset;
	uint8_t attributes;
	int error;
	int fsresult;
	size_t name_length;

	/* Looks the name up without regard to case. */
	fsresult = fat_engine_stat_location_casefold(state,
						     path,
						     &entry,
						     &lba,
						     &offset,
						     &first_cluster,
						     &attributes);

	/* Reports why the path could not be resolved. */
	if (fsresult != 0)
		return fsresult;
	slash = strrchr(path, '/');

	/* The canonical path is the parent plus the stored name. */
	prefix_length = 0;
	if (slash != NULL)
		prefix_length = (size_t)(slash - path + 1);

	/* The stored name has to fit after the parent that precedes it. */
	name_length = strlen(entry.name);
	if (prefix_length + name_length >= sizeof(canonical))
		return ENAMETOOLONG;

	memcpy(canonical, path, prefix_length);
	strcpy(canonical + prefix_length, entry.name);

	/* Builds the kernel inode of the record that was found. */
	error = fat_make_inode(mountp,
			       canonical,
			       &entry,
			       lba,
			       offset,
			       first_cluster,
			       attributes,
			       result);
	if (error == 0)
		set_inode_ops(*result);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Resolves one name under a directory, with the mount lock held. */
static int
fat_lookup_unlocked(
	struct inode *directory,
	const struct componentname *name,
	struct inode **result)
{
	char *slash;
	char path[KERN_PATH_MAX];
	const char *parent = fat_path(directory);
	int error;

	/* A directory whose own path is not known cannot be walked from. */
	if (parent == NULL)
		return EIO;

	/* Dot is the directory itself. */
	if (name->cn_namelen == 1 && name->cn_nameptr[0] == '.') {
		inode_ref(directory);
		*result = directory;

		/* Succeeded. */
		return 0;
	}

	/* And dot-dot is the directory above it. */
	if (name->cn_namelen == 2 && name->cn_nameptr[0] == '.' &&
	    name->cn_nameptr[1] == '.') {
		/* The root of the volume is its own parent. */
		if (parent[0] == '\0') {
			inode_ref(directory);
			*result = directory;

			/* Succeeded. */
			return 0;
		}

		/* The parent's path is this one with its last name cut off. */
		strcpy(path, parent);
		slash = strrchr(path, '/');

		/* A path with no slash left in it names a child of the root. */
		if (slash == NULL) {
			inode_ref(directory->i_mount->m_root);
			*result = directory->i_mount->m_root;

			/* Succeeded. */
			return 0;
		}

		*slash = '\0';

		/* Resolves the parent path that leaves. */
		error = fat_stat_path(directory->i_mount, path, result);

		/* Reports the inode, or why it could not be read. */
		return error;
	}

	/* An ordinary name is resolved under the directory's own path. */
	error = join_path(parent, name, path);
	if (error != 0)
		return error;

	error = fat_stat_path(directory->i_mount, path, result);

	/* Reports the inode, or why it could not be read. */
	return error;
}

/* Resolves one name under a directory. */
static int
fat_lookup(
	struct inode *directory,
	const struct componentname *name,
	struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	int error;

	mutex_lock(&state->lock);

	error = fat_lookup_unlocked(directory, name, result);

	mutex_unlock(&state->lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Resolves one name ignoring case, with the mount lock held. */
static int
fat_lookup_casefold_unlocked(
	struct inode *directory,
	const struct componentname *name,
	struct inode **result)
{
	char path[KERN_PATH_MAX];
	const char *parent = fat_path(directory);
	int error;

	/* A directory whose own path is not known cannot be walked from. */
	if (parent == NULL)
		return EIO;

	/* The name is resolved under the directory's own path. */
	error = join_path(parent, name, path);
	if (error != 0)
		return error;

	error = fat_stat_path_casefold(directory->i_mount, path, result);

	/* Reports the inode, or why it could not be read. */
	return error;
}

/* Reports what a caller may know about an inode. */
static int
fat_getattr(
	struct inode *inode,
	struct stat *status)
{
	memset(status, 0, sizeof(*status));
	status->st_dev = inode->i_mount->m_disk->d_dev;
	status->st_ino = inode->i_ino;
	status->st_mode = inode->i_mode;
	status->st_nlink = inode->i_linkcount;
	status->st_uid = inode->i_uid;
	status->st_gid = inode->i_gid;
	status->st_size = inode->i_size;
	status->st_atime = inode->i_atime.tv_sec;
	status->st_mtime = inode->i_mtime.tv_sec;
	status->st_ctime = inode->i_ctime.tv_sec;
	status->st_blksize = 512;
	/* An empty file occupies no blocks at all. */
	status->st_blocks = 0;
	if (inode->i_size > 0) {
		status->st_blocks =
			(blkcnt_t)(((uint64_t)inode->i_size + 511U) / 512U);
	}

	/* Succeeded. */
	return 0;
}

/* Writes a 16-bit field, least significant byte first. */
static FAT_MUTATION void
fat_put16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/* Changes the times a record stores, with the mount lock held. */
static FAT_MUTATION int
fat_setattr_unlocked(
	struct inode *inode,
	const struct stat *status,
	unsigned mask)
{
	uint8_t *rollback;
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	struct fat_inode_info *info = fat_inode(inode);
	uint8_t *sector, saved[32];
	uint16_t atime_date = 0, atime_time = 0;
	uint16_t mtime_date = 0, mtime_time = 0;
	mode_t permissions;
	int result;
	int error;
	int restored;

	/* An inode this driver did not make, or the root itself. */
	if (state == NULL || info == NULL || (inode->i_flags & INODE_ROOT) != 0)
		return EOPNOTSUPP;

	/* A size is changed by truncating, not by setting attributes. */
	if ((mask & INODE_ATTR_SIZE) != 0)
		return EOPNOTSUPP;

	/* FAT stores no owner, so it can only keep the one it shows. */
	if ((mask & INODE_ATTR_UID) != 0 && status->st_uid != inode->i_uid)
		return EOPNOTSUPP;

	/* And no group either. */
	if ((mask & INODE_ATTR_GID) != 0 && status->st_gid != inode->i_gid)
		return EOPNOTSUPP;

	/* FAT stores only a read-only bit, not a whole mode. */
	if (mask & INODE_ATTR_MODE) {
		/* So only the two modes that bit can express are accepted. */
		permissions = status->st_mode & 07777U;
		if (permissions != 0755U && permissions != 0555U)
			return EOPNOTSUPP;
	}

	/* An access time is stored as a date without a time of day. */
	if (mask & INODE_ATTR_ATIME) {
		/* A nanosecond field outside the second it belongs to. */
		if (status->st_atim.tv_nsec < 0 ||
		    status->st_atim.tv_nsec >= 1000000000L) {
			/* Failed. */
			return EINVAL;
		}

		/* Renders the time as the date field a record stores. */
		error = fat_encode_time(status->st_atim.tv_sec, &atime_date,
					&atime_time);
		if (error != 0)
			return error;
	}

	/* A modification time is stored as a date and a time. */
	if (mask & INODE_ATTR_MTIME) {
		/* A nanosecond field outside the second it belongs to. */
		if (status->st_mtim.tv_nsec < 0 ||
		    status->st_mtim.tv_nsec >= 1000000000L) {
			/* Failed. */
			return EINVAL;
		}

		/* Renders the time as the fields a record stores. */
		error = fat_encode_time(status->st_mtim.tv_sec, &mtime_date,
					&mtime_time);
		if (error != 0)
			return error;
	}

	/*
	 * The generic inode layer still applies attributes to an unlinked open
	 * inode, but its former FAT slot may already belong to another file.
	 */
	if ((inode->i_flags & INODE_DEAD) != 0)
		return 0;

	/* Reads the record so it can be changed in place. */
	result = fat_engine_write_sector_result(state, info->fi_dirent_lba,
		&sector);
	if (result != 0)
		return result;

	memcpy(saved, sector + info->fi_dirent_offset, sizeof(saved));
	sector += info->fi_dirent_offset;

	/* The mode is stored as the read-only bit and nothing else. */
	if (mask & INODE_ATTR_MODE) {
		/* A mode with no write bit at all sets that bit. */
		if ((status->st_mode & 0222U) == 0)
			sector[11] |= FAT_ATTRIBUTE_READ_ONLY;
		else
			sector[11] &= (uint8_t)~FAT_ATTRIBUTE_READ_ONLY;
	}

	/* The access date, which has no time of day beside it. */
	if (mask & INODE_ATTR_ATIME)
		fat_put16(sector + 18, atime_date);

	/* And the modification date, which has one. */
	if (mask & INODE_ATTR_MTIME) {
		fat_put16(sector + 22, mtime_time);
		fat_put16(sector + 24, mtime_date);
	}

	/* The change reaches the volume once the sector is written. */
	result = fat_engine_mark_sector_dirty(state);
	if (result == 0)
		result = fat_engine_flush(state);
	if (result != 0) {
		/* Puts the record back when it could not be published. */
		restored = fat_engine_write_sector_result(
			state, info->fi_dirent_lba, &rollback);
		if (restored == 0) {
			memcpy(rollback + info->fi_dirent_offset, saved,
			       sizeof(saved));
			(void)fat_engine_mark_sector_dirty(state);
		}

		/* Failed. */
		return result;
	}

	info->fi_attributes = sector[11];
	(void)atime_time;

	/* Succeeded. */
	return 0;
}

/* Changes the times a record stores. */
static FAT_MUTATION int
fat_setattr(
	struct inode *inode,
	const struct stat *status,
	unsigned mask)
{
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	int error;

	mutex_lock(&state->lock);

	/* Files closed but not yet flushed are written out first. */
	error = fat_flush_pending_closes(state);
	if (error == 0)
		error = fat_setattr_unlocked(inode, status, mask);

	mutex_unlock(&state->lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Takes the open-file state this driver keeps beside a file. */
static struct fat_file_state *
fat_file_get(
	struct file *file)
{
	struct fat_file_state *slot = NULL;
	struct fat_mount_state *mount_state;
	unsigned i;
	unsigned long irq;
	int opened;

	/* A file already bound to a slot keeps the one it has. */
	if (file->f_data != NULL)
		return file->f_data;
	irq = spin_lock_irqsave(&fat_pool_lock);

	/* Walks the fixed slots looking for one that is free. */
	for (i = 0; i < FAT_FILE_MAX; i++) {
		/* This slot is free, so it becomes the open file. */
		if (!fat_files[i].used) {
			memset(&fat_files[i], 0, sizeof(fat_files[i]));
			fat_files[i].used = 1;
			fat_files[i].owner = file->f_inode;
			slot = &fat_files[i];
			break;
		}
	}

	spin_unlock_irqrestore(&fat_pool_lock, irq);

	/* Every slot is taken, so no more files can be opened. */
	if (slot == NULL)
		return NULL;

	/* Opens the volume's file the kernel handle stands for. */
	mount_state = fat_mount_state(file->f_inode->i_mount);
	opened = fat_raw_open(mount_state, fat_path(file->f_inode), slot);
	if (opened != 0) {
		irq = spin_lock_irqsave(&fat_pool_lock);
		memset(slot, 0, sizeof(*slot));
		spin_unlock_irqrestore(&fat_pool_lock, irq);

		/* Reports that no result is available. */
		return NULL;
	}

	/*
	 * Another open file may have extended this inode without yet flushing
	 * its FAT directory entry.  The inode is the coherent in-memory
	 * size/cluster authority for every open description.
	 */
	slot->size = (uint64_t)file->f_inode->i_size;
	slot->first_cluster = fat_inode(file->f_inode)->fi_first_cluster;
	file->f_data = slot;

	/* The open-file state the caller now holds. */
	return slot;
}

/* Opens the volume's file behind a kernel file handle. */
static int
fat_open_file(
	struct file *file)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	struct fat_file_state *opened;
	int error;

	mutex_lock(&state->lock);

	/* Opens the volume's file the kernel handle stands for. */
	opened = fat_file_get(file);
	error = 0;
	if (opened == NULL)
		error = EIO;

	mutex_unlock(&state->lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Called under the FAT mount lock and the ordinary file/VM I/O lease. Mapped
 * operations never resize or publish allocation metadata.
 */
static ssize_t
fat_loop_transfer(
	struct file *file,
	struct fat_file_state *state,
	void *buffer,
	size_t length,
	off_t offset,
	int writing)
{
	const struct fat_loop_extent *extent;
	uint64_t within, amount;
	struct fat_mount_state *mount = state->mount;
	uint64_t block, remaining;
	size_t done = 0;
	unsigned i;
	int error;

	/* A file without a backing claim has no map to be read through. */
	if (file->f_backing_claim == NULL)
		return -EINVAL;	/* Failed. */

	/* A negative offset names nowhere in the file. */
	if (offset < 0)
		return -EINVAL;	/* Failed. */

	/* The map is addressed in sectors, so both ends have to be whole. */
	if (((uint64_t)offset & 511U) != 0 || (length & 511U) != 0)
		return -EINVAL;	/* Failed. */

	/* A run starting past the end of the file reads nothing. */
	if ((uint64_t)offset > state->size)
		return -EINVAL;	/* Failed. */

	/* Nor may one that starts inside it run off the end. */
	if (length > state->size - (uint64_t)offset)
		return -EINVAL;	/* Failed. */

	/* A volume mounted read-only is never written to. */
	if (writing && mount->read_only)
		return -EROFS;

	/*
	 * Drain before invalidating; never discard an earlier failed dirty
	 * write. The lock excludes all other readers of the single FAT sector
	 * slot.
	 */

	/* Nothing cached may outlive a transfer that bypasses it. */
	error = fat_engine_flush(mount);
	if (error != 0)
		return -error;
	fat_engine_invalidate(mount);
	block = (uint64_t)offset / 512U;
	remaining = length / 512U;
	/* Walks the extents until the whole run has been moved. */
	for (i = 0; i < state->loop_map_count && remaining != 0; i++) {
		/* The extents are in order, so a block before one is a gap. */
		extent = &state->loop_map[i];
		if (block < extent->file_block)
			return -EIO;

		/* An extent ending before the block holds none of it. */
		within = block - extent->file_block;
		if (within >= extent->count)
			continue;

		/* An extent holds no more of the run than is left. */
		amount = extent->count - within;
		if (amount > remaining)
			amount = remaining;

		/* The disk is reached directly, in the mount's own context. */
		if (writing) {
			error = disk_write_filesystem_context(
				mount->disk, extent->disk_block + within,
				(uint32_t)amount, (uint8_t *)buffer + done,
				mount->write_context);
		} else {
			error = disk_read(
				mount->disk, extent->disk_block + within,
				(uint32_t)amount, (uint8_t *)buffer + done);
		}

		/* A failure after moving something reports what moved. */
		if (error != 0) {
			/* A short transfer reports what it did move. */
			if (done != 0)
				return (ssize_t)done;

			return -error;	/* Failed. */
		}

		done += (size_t)amount * 512U;
		block += amount;
		remaining -= amount;
	}

	/* Succeeded: the whole run was carried through the loop map. */
	if (remaining == 0)
		return (ssize_t)done;

	/* The map ran out before the run did, which means it is wrong. */
	return -EIO;	/* Failed. */
}

/* Reads at a position, with the mount lock held. */
static ssize_t
fat_pread_file_unlocked(
	struct file *file,
	void *buffer,
	size_t length,
	off_t offset)
{
	ssize_t transferred;
	struct fat_file_state *state = fat_file_get(file);
	uint32_t count;
	int result;

	/* A file that was never opened through this driver. */
	if (state == NULL)
		return -EIO;

	/* A read starting past the end of the file returns nothing. */
	if (offset >= file->f_inode->i_size)
		return 0;

	/* And one that reaches past it stops at the end. */
	if (length > (size_t)(file->f_inode->i_size - offset))
		length = (size_t)(file->f_inode->i_size - offset);

	/* FAT records a length in 32 bits, so one pass cannot ask for more. */
	if (length > UINT32_MAX)
		count = UINT32_MAX;
	else
		count = (uint32_t)length;

	/*
	 * Reads through the loop map instead of the cluster chain when every
	 * one of the following holds:
	 *
	 * - the file has a loop map, so its extents are already known;
	 * - it holds a backing claim, which is what makes the map trustworthy;
	 * - the offset is not negative;
	 * - the offset and the length are both whole sectors, because the map
	 *   is addressed in sectors.
	 */
	if (state->loop_map != NULL &&
	    file->f_backing_claim != NULL &&
	    offset >= 0 &&
	    ((uint64_t)offset & 511U) == 0 &&
	    (count & 511U) == 0) {
		/* Reads the run straight out of the mapped extents. */
		transferred = fat_loop_transfer(file, state, buffer, count,
						offset, 0);

		/* Reports how much of the run was moved. */
		return transferred;
	}

	/* Reads the run by walking the file's cluster chain. */
	result = fat_raw_read(state, (uint64_t)offset, buffer, count, NULL,
		NULL);
	if (result != 0)
		return -result;

	/* Reports how many bytes were moved. */
	return count;
}

/* Copies a file's size and chain back into its inode. */
static void
fat_sync_inode_state(
	struct inode *inode,
	const struct fat_file_state *file)
{
	struct fat_file_state *open_state;
	struct fat_inode_info *info;
	const struct fat_file_state *state;
	unsigned i;
	unsigned long irq;

	/* A call that names no inode or no file. */
	if (inode == NULL || file == NULL)
		return;

	info = fat_inode(inode);

	state = file;
	info->fi_first_cluster = state->first_cluster;
	inode->i_size = (off_t)file->size;

	irq = spin_lock_irqsave(&fat_pool_lock);

	/* Every open file of this inode learns the new size. */
	for (i = 0; i < FAT_FILE_MAX; i++) {
		/* A slot of another inode is left alone. */
		if (!fat_files[i].used || fat_files[i].owner != inode)
			continue;
		fat_files[i].size = file->size;
		open_state = &fat_files[i];
		open_state->first_cluster = state->first_cluster;
	}

	spin_unlock_irqrestore(&fat_pool_lock, irq);
}

/*
 * Called with the mount mutex held.  A failed close leaves a self-contained
 * directory-entry retry record; it deliberately owns no inode reference so
 * generic unmount busy checks can reach filesystem sync.
 */
static int
fat_flush_pending_closes(
	struct fat_mount_state *mount_state)
{
	struct fat_file_state *state;
	struct inode *owner;
	unsigned long irq;
	int error;
	unsigned i;

	/* Walks the fixed slots looking for closes still pending. */
	for (i = 0; i < FAT_FILE_MAX; i++) {
		state = &fat_files[i];

		/* A slot of another mount, or one not pending, is skipped. */
		irq = spin_lock_irqsave(&fat_pool_lock);
		if (!state->used || state->mount != mount_state ||
		    !state->pending_close) {
			spin_unlock_irqrestore(&fat_pool_lock, irq);
			continue;
		}

		owner = state->owner;
		spin_unlock_irqrestore(&fat_pool_lock, irq);

		/* Writes out what the closed file still owed the volume. */
		error = fat_raw_flush_file(state);
		if (error != 0)
			return error;

		/* An inode still in core learns the size that was written. */
		if (owner != NULL)
			fat_sync_inode_state(owner, state);

		irq = spin_lock_irqsave(&fat_pool_lock);
		memset(state, 0, sizeof(*state));
		spin_unlock_irqrestore(&fat_pool_lock, irq);
	}

	/* Succeeded. */
	return 0;
}

/* Reads at a position, without moving the file position. */
static ssize_t
fat_pread_file(
	struct file *file,
	void *buffer,
	size_t length,
	off_t offset)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	ssize_t count;

	mutex_lock(&state->lock);

	count = fat_pread_file_unlocked(file, buffer, length, offset);

	mutex_unlock(&state->lock);

	/* Reports how many bytes were moved. */
	return count;
}

/* Reads at the file position and advances it. */
static ssize_t
fat_read_file(
	struct file *file,
	void *buffer,
	size_t length)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	ssize_t count;

	mutex_lock(&state->lock);

	/* Reads from where the file position stands. */
	count = fat_pread_file_unlocked(file, buffer, length, file->f_offset);
	if (count > 0)
		file->f_offset += count;

	mutex_unlock(&state->lock);

	/* Reports how many bytes were moved. */
	return count;
}

/* Writes at a position, with the mount lock held. */
static ssize_t
fat_pwrite_file_unlocked(
	struct file *file,
	const void *buffer,
	size_t length,
	off_t offset)
{
	ssize_t transferred;
	struct fat_file_state *state = fat_file_get(file);
	uint32_t count;
	int result;

	/* FAT records a length in 32 bits, so one pass moves no more. */
	count = UINT32_MAX;
	if (length < UINT32_MAX)
		count = (uint32_t)length;

	/* A file that was never opened through this driver. */
	if (state == NULL)
		return -EIO;

	/* A negative offset names nowhere in the file. */
	if (offset < 0)
		return -EINVAL;

	/* Nor may the run reach past what a 32-bit size expresses. */
	if ((uint64_t)offset > UINT32_MAX ||
	    (uint64_t)count > UINT32_MAX - (uint64_t)offset) {
		/* Failed. */
		return -EFBIG;
	}

	/* A mapped file is written straight through its own extents. */
	if (state->loop_map != NULL && file->f_backing_claim != NULL) {
		transferred = fat_loop_transfer(file, state, (void *)buffer,
						length, offset, 1);

		/* Reports how much of the run was moved. */
		return transferred;
	}

	/* Writes the run through the file's cluster chain. */
	result = fat_raw_write(state, (uint64_t)offset, buffer, count);
	if (result != 0)
		return -result;

	fat_sync_inode_state(file->f_inode, state);

	/* Reports how many bytes were moved. */
	return count;
}

/* Writes at a position, without moving the file position. */
static ssize_t
fat_pwrite_file(
	struct file *file,
	const void *buffer,
	size_t length,
	off_t offset)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	ssize_t count;

	mutex_lock(&state->lock);

	count = fat_pwrite_file_unlocked(file, buffer, length, offset);

	mutex_unlock(&state->lock);

	/* Reports how many bytes were moved. */
	return count;
}

/* Preserve synchronous provenance under the same lock as FAT mutation. */
static ssize_t
fat_pwrite_context(
	struct file *file,
	const void *buffer,
	size_t length,
	off_t offset,
	unsigned flags,
	const struct ucred *credential,
	const struct io_context *context)
{
	struct fat_mount_state *state;
	struct io_context child;
	const struct io_context *previous;
	ssize_t count;
	int error;

	/* The generic file layer already authorized flags and credentials. */
	(void)flags;
	(void)credential;

	/* The write joins the caller's own ordering context. */
	error = io_context_child(&child, context, IO_CONTEXT_DRAIN);
	if (error != 0)
		return -error;

	state = fat_mount_state(file->f_inode->i_mount);
	mutex_lock(&state->lock);

	previous = state->write_context;
	state->write_context = &child;
	count = fat_pwrite_file_unlocked(file, buffer, length, offset);
	state->write_context = previous;

	mutex_unlock(&state->lock);

	/* Reports how many bytes were moved. */
	return count;
}

/* Writes at the file position, or at the end, and advances it. */
static ssize_t
fat_write_file(
	struct file *file,
	const void *buffer,
	size_t length)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	off_t offset;
	ssize_t count;
	int status_flags;

	mutex_lock(&state->lock);

	/* An appending write always starts at the current end of the file. */
	status_flags = file_status_flags_get(file);
	if ((status_flags & O_APPEND) != 0)
		offset = file->f_inode->i_size;
	else
		offset = file->f_offset;

	/* Writes from where the file position stands. */
	count = fat_pwrite_file_unlocked(file, buffer, length, offset);
	if (count > 0)
		file->f_offset = offset + count;

	mutex_unlock(&state->lock);

	/* Reports how many bytes were moved. */
	return count;
}

/* Reads the next directory entry, with the mount lock held. */
static int
fat_readdir_unlocked(
	struct file *file,
	struct dirent *entry,
	int *eof)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	struct fat_dir_entry decoded;
	char child_path[KERN_PATH_MAX];
	struct componentname component;
	struct inode *child;
	int result;
	int error;

	result = fat_raw_readdir(state,
				 fat_path(file->f_inode),
				 (unsigned)file->f_offset,
				 &decoded);
	if (result == ENOENT) {
		*eof = 1;

		/* Succeeded. */
		return 0;
	}

	/* Reports why the entry could not be read. */
	if (result != 0)
		return result;

	component.cn_nameptr = decoded.name;
	component.cn_namelen = strlen(decoded.name);
	component.cn_flags = COMPONENT_LAST;

	/* The child's path is the directory's plus its own name. */
	error = join_path(fat_path(file->f_inode), &component, child_path);
	if (error != 0)
		return ENAMETOOLONG;

	/* Every entry reported carries the inode it names. */
	error = fat_stat_path(file->f_inode->i_mount, child_path, &child);
	if (error != 0)
		return EIO;

	memset(entry, 0, sizeof(*entry));
	entry->d_ino = child->i_ino;
	entry->d_type = child->i_type;
	strncpy(entry->d_name, decoded.name, NAME_MAX);
	entry->d_name[NAME_MAX] = '\0';

	inode_release(child);

	file->f_offset++;
	*eof = 0;

	/* Succeeded. */
	return 0;
}

/* Reads the next directory entry. */
static int
fat_readdir(
	struct file *file,
	struct dirent *entry,
	int *eof)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	int error;

	mutex_lock(&state->lock);

	error = fat_readdir_unlocked(file, entry, eof);

	mutex_unlock(&state->lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Closes a file, freeing a deferred chain if this was the last hold. */
static int
fat_close_file(
	struct file *file)
{
	struct fat_file_state *state;
	struct fat_mount_state *mount_state;
	unsigned long irq;
	int error;
	int status_flags;

	/* A file whose inode is already gone has no mount to lock. */
	state = file->f_data;
	mount_state = NULL;
	error = 0;
	if (file->f_inode != NULL)
		mount_state = fat_mount_state(file->f_inode->i_mount);

	/* Holds the mount for as long as the file state is touched. */
	if (mount_state != NULL)
		mutex_lock(&mount_state->lock);

	/* A file that was never opened through this driver. */
	if (state != NULL) {
		/* Only a writable file of a live inode owes the volume. */
		status_flags = file_status_flags_get(file);
		if ((status_flags & O_ACCMODE) != O_RDONLY &&
		    file->f_inode != NULL &&
		    (file->f_inode->i_flags & INODE_DEAD) == 0) {
			/* Writes out what the file still owed. */
			error = fat_raw_flush_file(state);
			if (error == 0)
				fat_sync_inode_state(file->f_inode, state);
		} else if (file->f_inode != NULL &&
			   (file->f_inode->i_flags & INODE_DEAD) != 0) {
			error = fat_engine_flush(
				fat_mount_state(file->f_inode->i_mount));
		}

		irq = spin_lock_irqsave(&fat_pool_lock);

		/* A record that could not be written is written later. */
		if (error != 0 && state->directory_dirty &&
		    file->f_inode != NULL &&
		    (file->f_inode->i_flags & INODE_DEAD) == 0) {
			/*
			 * The struct file is closing, but mount sync still
			 * needs the authoritative directory-entry retry record.
			 */
			state->pending_close = 1;
			state->owner = NULL;
		} else {
			memset(state, 0, sizeof(*state));
		}

		spin_unlock_irqrestore(&fat_pool_lock, irq);
		file->f_data = NULL;
	}

	/* A mount that was never taken has no lock to give back. */
	if (mount_state != NULL)
		mutex_unlock(&mount_state->lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Flushes directory publication without treating its cursor as file state. */
static int
fat_fsync_directory(
	struct file *file)
{
	struct inode *inode;

	if (file == NULL)
		return EINVAL;
	inode = file->f_inode;
	if (inode == NULL || inode->i_type != INODE_DIR)
		return EINVAL;
	return fat_sync_mount(inode->i_mount);
}

/* Makes everything this file has written durable. */
static int
fat_fsync(
	struct file *file)
{
	struct fat_mount_state *mount_state =
		fat_mount_state(file->f_inode->i_mount);
	struct fat_file_state *state;
	int error;

	mutex_lock(&mount_state->lock);

	/* A file that was never opened through this driver. */
	state = fat_file_get(file);
	if (state == NULL)
		error = EIO;
	else if (file->f_inode != NULL &&
		 (file->f_inode->i_flags & INODE_DEAD) != 0)
		error = fat_engine_flush(mount_state);
	else
		error = fat_raw_flush_file(state);
	if (error == 0)
		error = disk_sync(file->f_inode->i_mount->m_disk);

	mutex_unlock(&mount_state->lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Resolves one name under a directory, ignoring case. */
static int
fat_lookup_casefold(
	struct inode *directory,
	const struct componentname *name,
	struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	int error;

	mutex_lock(&state->lock);

	error = fat_lookup_casefold_unlocked(directory, name, result);

	mutex_unlock(&state->lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Changes the size of a file. */
static int
fat_truncate(
	struct inode *inode,
	off_t size)
{
	struct fat_file_state file = {0};
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	int result;

	/* A negative size is not a length. */
	if (size < 0)
		return EINVAL;

	/* Nor is one past what a 32-bit size expresses. */
	if ((uint64_t)size > UINT32_MAX)
		return EFBIG;
	mutex_lock(&state->lock);

	/* Files closed but not yet flushed are written out first. */
	result = fat_flush_pending_closes(state);
	if (result != 0) {
		mutex_unlock(&state->lock);

		/* Failed. */
		return result;
	}

	/* An unlinked inode is truncated through a borrowed handle. */
	if ((inode->i_flags & INODE_DEAD) != 0) {
		file.mount = state;
		file.owner = inode;
		file.size = (uint64_t)inode->i_size;
		file.first_cluster = fat_inode(inode)->fi_first_cluster;
		result = 0;
	} else {
		result = fat_raw_open(state, fat_path(inode), &file);
	}

	/* A truncate that succeeded publishes the new size. */
	if (result == 0) {
		/*
		 * An open writer may own a newer size/chain than the
		 * directory entry.  Preserve its directory location
		 * but truncate the coherent inode state, then
		 * propagate the result to every open handle.
		 */
		file.size = (uint64_t)inode->i_size;
		file.first_cluster = fat_inode(inode)->fi_first_cluster;
		result = fat_raw_truncate(&file, (uint64_t)size);
	}

	/* Writes the record back and copies the size into the inode. */
	if (result == 0)
		result = fat_raw_flush_file(&file);
	if (result == 0)
		fat_sync_inode_state(inode, &file);

	mutex_unlock(&state->lock);

	/* Failed. */
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Creates a regular file, with the mount lock held. */
static int
fat_create_unlocked(
	struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request,
	struct inode **result)
{
	mode_t mode;
	uid_t uid;
	gid_t gid;
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct fat_file_state file = {0};
	struct inode *created = NULL;
	char path[KERN_PATH_MAX];
	int error, rollback;
	int representable;

	*result = NULL;

	/* A pending close may still own the name about to be created. */
	error = fat_flush_pending_closes(state);
	if (error != 0)
		return error;

	/* Builds the full path the new file will be created at. */
	error = join_path(fat_path(directory), name, path);
	if (error != 0)
		return error;

	/* Refuses the request if that name is already taken. */
	error = fat_creation_collision(state, path);
	if (error != 0)
		return error;

	/* Refuses a mode or owner this mount could not present afterwards. */
	error = fat_creation_representable(state, path, request, INODE_REG);
	if (error != 0)
		return error;

	/* Creates the directory entry and opens it. */
	error = fat_raw_create(state, path, &file);
	if (error != 0)
		return error;

	/* Puts the entry on disk before anything can look the name up. */
	error = fat_raw_flush_file(&file);
	if (error != 0)
		goto rollback_raw;

	/* A negative cache entry for this name is now wrong. */
	namecache_remove(directory, name);

	/* Reads the entry back as the inode the caller will be given. */
	error = fat_stat_path(directory->i_mount, path, &created);
	if (error != 0)
		goto rollback_raw;

	/* Points the new inode at the open file that created it. */
	fat_sync_inode_state(created, &file);

	/* Lets the generic layer apply the request, then checks the result. */
	error = inode_creation_prepare(directory, created, request);
	if (error == 0) {
		error = fat_created_inode_matches(state,
						  fat_path(created),
						  created);
	}

	if (error != 0)
		goto rollback_inode;

	*result = created;

	/* Succeeded. */
	return 0;

rollback_inode:

	/* Takes the name back off the volume. */
	rollback = fat_raw_unlink(state, path);
	if (rollback == 0) {
		fat_orphan(created);
		namecache_remove(directory, name);
	} else {
		/*
		 * The name could not be taken back, so the inode stays.
		 * Give it whatever this mount would present for that path,
		 * so a later lookup and this inode agree.
		 */
		representable = fat_creation_representation(
			state, fat_path(created), &mode, &uid, &gid);
		if (representable == 0) {
			created->i_mode = S_IFREG | mode;
			created->i_uid = uid;
			created->i_gid = gid;
			created->i_rdev = 0;
		}

		state->read_only = 1;
		error = rollback;
	}

	*result = created;

	/* Failed: the caller still gets the inode that could not be undone. */
	return error;

rollback_raw:

	/* Takes the half-created name back off the volume. */
	rollback = fat_raw_unlink(state, path);
	if (rollback != 0) {
		state->read_only = 1;

		return rollback;	/* Failed. */
	}

	namecache_remove(directory, name);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Creates a regular file. */
static int
fat_create(
	struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request,
	struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct inode *created = NULL;
	int error;

	/* A call with nowhere to report the inode it makes. */
	if (result == NULL)
		return EINVAL;

	*result = NULL;

	mutex_lock(&state->lock);
	error = fat_create_unlocked(directory, name, request, &created);
	mutex_unlock(&state->lock);
	if (error != 0) {
		/* Gives the inode back when the creation could not finish. */
		if (created != NULL)
			inode_release(created);

		/* Failed. */
		return error;
	}

	*result = created;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Marks an unlinked inode whose chain is still held open. */
static FAT_MUTATION void
fat_orphan(
	struct inode *inode)
{
	struct fat_inode_info *info;

	/* A call that names no inode has nothing to mark. */
	if (inode == NULL)
		return;

	info = fat_inode(inode);
	info->fi_flags |= FAT_INODE_ORPHANED;
	inode->i_flags |= INODE_DEAD;
	namecache_purge_inode(inode);
}

/* Frees the chain of an unlinked inode nothing holds any more. */
static FAT_MUTATION void
fat_release_orphan(
	struct inode *inode)
{
	/* A call that names no inode has nothing to release. */
	if (inode == NULL)
		return;

	/*
	 * inode_release() owns the transition from the final external
	 * reference to cache-only DEAD state and performs
	 * reclaim/free synchronously.
	 */
	inode_release(inode);
}

/* Creates a directory, with the mount lock held. */
static FAT_MUTATION int
fat_mkdir_unlocked(
	struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request,
	struct inode **result)
{
	mode_t mode;
	uid_t uid;
	gid_t gid;
	int deferred;
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct inode *created = NULL;
	char path[KERN_PATH_MAX];
	uint32_t cluster = 0;
	int error, rollback;
	int representable;

	*result = NULL;

	/* The new directory's path is the parent's plus the name. */
	error = join_path(fat_path(directory), name, path);
	if (error != 0)
		return error;

	/* A name the volume already holds cannot be created. */
	error = fat_creation_collision(state, path);
	if (error != 0)
		return error;

	/* Nor one whose mode and owner this mount could not show. */
	error = fat_creation_representable(state, path, request, INODE_DIR);
	if (error != 0)
		return error;

	/* Creates the directory on the volume itself. */
	error = fat_raw_mkdir(state, path, &cluster);
	if (error != 0)
		return error;

	namecache_remove(directory, name);

	/* Resolves the directory that was just created. */
	error = fat_stat_path(directory->i_mount, path, &created);
	if (error != 0)
		goto rollback_raw;

	/* Gives the new inode the identity the request asked for. */
	error = inode_creation_prepare(directory, created, request);
	if (error == 0) {
		error = fat_created_inode_matches(state,
						  fat_path(created),
						  created);
	}

	/* Reports why the new directory could not be published. */
	if (error != 0)
		goto rollback_inode;

	*result = created;

	/* Succeeded. */
	return 0;

rollback_inode:

	/* Takes the directory back off the volume. */
	rollback = fat_raw_rmdir(state, path);
	if (rollback != 0) {
		/* A rollback that fails leaves the mode it would have shown. */
		representable = fat_creation_representation(
			state, fat_path(created), &mode, &uid, &gid);
		if (representable == 0) {
			created->i_mode = S_IFDIR | mode;
			created->i_uid = uid;
			created->i_gid = gid;
			created->i_rdev = 0;
		}

		state->read_only = 1;
		error = rollback;
		*result = created;

		/* Failed. */
		return error;
	}

	/*
	 * The directory entry is gone, so no live inode may retain
	 * the cluster once reclamation starts.  A recoverable free
	 * failure is represented by pending_orphans instead of the
	 * now-dead inode.
	 */
	fat_inode(created)->fi_first_cluster = 0;
	fat_orphan(created);
	namecache_remove(directory, name);

	/* Frees the cluster the directory had been given. */
	rollback = fat_raw_free_chain(state, cluster);
	if (rollback != 0) {
		/* A cluster that cannot be freed is deferred instead. */
		deferred = rollback;
		if (!state->read_only)
			deferred = fat_defer_orphan(state, cluster);

		/* A cluster that cannot even be deferred stops writing. */
		if (deferred != 0)
			state->read_only = 1;

		*result = created;

		/*
		 * The directory exists but its cluster could not be freed.
		 * Reports the deferral failure if there was one, and the
		 * failed free otherwise.
		 */
		if (deferred != 0)
			return deferred;	/* Failed. */

		return rollback;	/* Failed. */
	}

	*result = created;

	/* Failed: the caller still gets the directory that was created. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;

rollback_raw:

	/* Takes the half-created directory back off the volume. */
	rollback = fat_raw_rmdir(state, path);
	if (rollback != 0) {
		state->read_only = 1;

		return rollback;	/* Failed. */
	}

	/* Frees the cluster it had been given. */
	rollback = fat_raw_free_chain(state, cluster);
	if (rollback != 0) {
		/*
		 * The cluster is neither freed nor reachable.  Queue it for a
		 * later attempt, and give up on writing if even that fails.
		 */
		deferred = EIO;
		if (!state->read_only)
			deferred = fat_defer_orphan(state, cluster);

		/* A cluster that cannot even be deferred stops writing. */
		if (deferred != 0)
			state->read_only = 1;

		return rollback;	/* Failed. */
	}

	namecache_remove(directory, name);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Creates a directory. */
static FAT_MUTATION int
fat_mkdir(
	struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request,
	struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct inode *created = NULL;
	int error;

	/* A call with nowhere to report the inode it makes. */
	if (result == NULL)
		return EINVAL;

	*result = NULL;

	mutex_lock(&state->lock);
	error = fat_mkdir_unlocked(directory, name, request, &created);
	mutex_unlock(&state->lock);
	if (error != 0) {
		/* Gives the inode back when the creation could not finish. */
		if (created != NULL)
			inode_release(created);

		/* Failed. */
		return error;
	}

	*result = created;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Removes a name, reporting an inode whose chain outlives it. */
static FAT_MUTATION int
fat_remove_inode_unlocked(
	struct inode *directory,
	const struct componentname *name,
	int remove_directory,
	struct inode **orphaned)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct inode *victim = NULL;
	char path[KERN_PATH_MAX];
	int error;

	*orphaned = NULL;

	/* Pending closes and orphans are settled before a removal. */
	error = fat_flush_pending_closes(state);
	if (error == 0)
		error = fat_drain_pending_orphans(state);
	if (error != 0)
		return error;

	/* The path of the name being removed. */
	error = join_path(fat_path(directory), name, path);
	if (error != 0)
		return error;

	/* Resolves the name, so its inode outlives the record. */
	error = fat_lookup_unlocked(directory, name, &victim);
	if (error != 0)
		return error;

	/* Removes the name from the volume itself. */
	if (remove_directory)
		error = fat_raw_rmdir(state, path);
	else
		error = fat_raw_unlink(state, path);
	if (error == 0) {
		namecache_remove(directory, name);
		fat_orphan(victim);
		*orphaned = victim;
	}

	/* A removal that failed gives the inode straight back. */
	if (error != 0)
		inode_release(victim);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Removes a name that does not belong to a directory. */
static FAT_MUTATION int
fat_unlink(
	struct inode *directory,
	const struct componentname *name)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct inode *orphaned = NULL;
	int error;

	mutex_lock(&state->lock);

	error = fat_remove_inode_unlocked(directory, name, 0, &orphaned);

	mutex_unlock(&state->lock);

	/* An inode whose chain outlived its name is freed here. */
	if (orphaned != NULL)
		fat_release_orphan(orphaned);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Removes an empty directory. */
static FAT_MUTATION int
fat_rmdir(
	struct inode *directory,
	const struct componentname *name)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct inode *orphaned = NULL;
	int error;

	mutex_lock(&state->lock);

	error = fat_remove_inode_unlocked(directory, name, 1, &orphaned);

	mutex_unlock(&state->lock);

	/* An inode whose chain outlived its name is freed here. */
	if (orphaned != NULL)
		fat_release_orphan(orphaned);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Tests whether a path names something inside a parent directory. */
static FAT_MUTATION int
fat_path_descendant(
	const char *parent,
	const char *path)
{
	size_t length;
	int difference;

	/* Nothing lies under an empty parent name. */
	length = strlen(parent);
	if (length == 0)
		return 0;

	/* The parent name has to be a prefix of the path. */
	difference = memcmp(parent, path, length);
	if (difference != 0)
		return 0;

	/* That prefix has to end where a path component ends. */
	if (path[length] != '/')
		return 0;

	/* Reports that the path lies under the parent. */
	return 1;
}

/* Rewrites the recorded path of every inode below a moved one. */
static FAT_MUTATION void
fat_repath_descendants(
	struct mount *mountp,
	const char *old_path,
	const char *new_path)
{
	char replacement[KERN_PATH_MAX];
	size_t suffix;
	size_t old_length;
	size_t new_length;
	unsigned long irq;
	unsigned i;
	int below;

	/* Measures both names once, outside the walk. */
	old_length = strlen(old_path);
	new_length = strlen(new_path);

	/* Walks the inode pool under its lock. */
	irq = spin_lock_irqsave(&fat_pool_lock);

	for (i = 0; i < FAT_INODE_MAX; i++) {
		/* Skips a slot that holds no inode. */
		if (!fat_inodes[i].used)
			continue;

		/* Skips an inode that belongs to another mount. */
		if (fat_inodes[i].info.fi_inode.i_mount != mountp)
			continue;

		/* Skips an inode that does not lie under the old name. */
		below = fat_path_descendant(old_path, fat_inodes[i].path);
		if (!below)
			continue;

		/*
		 * Rebuilds the path with the new name in front of the suffix.
		 */
		suffix = strlen(fat_inodes[i].path + old_length);
		memcpy(replacement, new_path, new_length);
		memcpy(replacement + new_length,
			fat_inodes[i].path + old_length, suffix + 1U);
		strcpy(fat_inodes[i].path, replacement);
	}

	spin_unlock_irqrestore(&fat_pool_lock, irq);
}

/*
 * Tests whether every open inode under a directory would still fit if that
 * directory were renamed.
 *
 * The rename is refused before anything is written, because an inode whose
 * new path does not fit could not be repointed afterwards.
 */
static FAT_MUTATION int
fat_repath_descendants_possible(
	struct mount *mountp,
	const char *old_path,
	const char *new_path)
{
	size_t suffix;
	size_t old_length;
	size_t new_length;
	unsigned long irq;
	unsigned i;
	int error;
	int below;

	/* Measures both names once, outside the search. */
	old_length = strlen(old_path);
	new_length = strlen(new_path);
	error = 0;

	/* Walks the inode pool under its lock. */
	irq = spin_lock_irqsave(&fat_pool_lock);

	for (i = 0; i < FAT_INODE_MAX; i++) {
		/* Skips a slot that holds no inode. */
		if (!fat_inodes[i].used)
			continue;

		/* Skips an inode that belongs to another mount. */
		if (fat_inodes[i].info.fi_inode.i_mount != mountp)
			continue;

		/* Skips an inode that does not lie under the old name. */
		below = fat_path_descendant(old_path, fat_inodes[i].path);
		if (!below)
			continue;

		/*
		 * Refuses the rename when this inode's new path would not fit.
		 */
		suffix = strlen(fat_inodes[i].path + old_length);
		if (new_length + suffix >= KERN_PATH_MAX) {
			error = ENAMETOOLONG;
			break;
		}
	}

	spin_unlock_irqrestore(&fat_pool_lock, irq);

	/* Reports the inode that would not fit. */
	if (error != 0)
		return error;

	/* Succeeded: every open inode under the name would still fit. */
	return 0;
}

/* Renames or moves a name, with the mount lock held. */
static FAT_MUTATION int
fat_rename_unlocked(
	struct inode *old_directory,
	const struct componentname *old_name,
	struct inode *new_directory,
	const struct componentname *new_name,
	unsigned flags,
	struct inode **orphaned)
{
	struct fat_file_state *open_state;
	struct fat_mount_state *state = fat_mount_state(old_directory->i_mount);
	struct inode *source = NULL, *target = NULL;
	struct fat_inode_info *info;
	struct fat_rename_result renamed = {0};
	struct componentname canonical_name;
	char old_path[KERN_PATH_MAX], new_path[KERN_PATH_MAX];
	char canonical_basename[KERN_PATH_MAX];
	char old_canonical[KERN_PATH_MAX], new_canonical[KERN_PATH_MAX];
	unsigned i;
	unsigned long irq;
	int error, target_error;
	const char *target_path;
	const char *source_path;

	*orphaned = NULL;

	/* This driver defines no rename flags. */
	if (flags != 0)
		return EINVAL;	/* Failed. */

	/* A pending close may still own either of the two names. */
	error = fat_flush_pending_closes(state);
	if (error != 0)
		return error;	/* Failed. */

	/* Builds the full paths of both ends of the rename. */
	error = join_path(fat_path(old_directory), old_name, old_path);
	if (error == 0)
		error = join_path(fat_path(new_directory), new_name, new_path);
	if (error != 0)
		return error;	/* Failed. */

	/* Resolves the name being renamed. */
	error = fat_lookup_unlocked(old_directory, old_name, &source);
	if (error != 0)
		return error;	/* Failed. */

	/* An inode outside the pool has no recorded path to rename from. */
	source_path = fat_path(source);
	if (source_path == NULL) {
		inode_release(source);

		/* Failed. */
		return EIO;
	}

	strcpy(old_canonical, fat_path(source));

	/* Resolves whatever the new name already refers to, if anything. */
	target_error = fat_lookup_unlocked(new_directory, new_name, &target);
	if (target_error == 0 && target == source) {
		inode_release(target);
		inode_release(source);

		/* Succeeded. */
		return 0;
	}

	/* An absent destination is fine; any other failure is not. */
	if (target_error != 0 && target_error != ENOENT) {
		inode_release(source);

		/* Failed. */
		return target_error;
	}

	/* A rename onto a name settles that name's orphans first. */
	if (target != NULL) {
		/* Frees whatever chains were waiting to be given back. */
		error = fat_drain_pending_orphans(state);
		if (error != 0) {
			inode_release(target);
			inode_release(source);

			/* Failed. */
			return error;
		}
	}

	/* A rename onto a name needs that name's own path. */
	if (target != NULL) {
		/* An inode whose path is not known cannot be replaced. */
		target_path = fat_path(target);
		if (target_path == NULL) {
			inode_release(target);
			inode_release(source);

			/* Failed. */
			return EIO;
		}

		strcpy(new_canonical, fat_path(target));
	} else {
		/* The volume may spell the new name differently. */
		error = fat_raw_canonical_basename(state,
						   new_path,
						   canonical_basename);
		if (error != 0) {
			inode_release(source);

			/* Failed. */
			return error;
		}

		canonical_name.cn_nameptr = canonical_basename;
		canonical_name.cn_namelen = strlen(canonical_basename);
		canonical_name.cn_flags = COMPONENT_LAST;

		/* The canonical path is the parent's plus that spelling. */
		error = join_path(fat_path(new_directory),
				  &canonical_name,
				  new_canonical);
		if (error != 0) {
			inode_release(source);

			/* Failed. */
			return error;
		}
	}

	/* A directory carries every path below it with it. */
	if (source->i_type == INODE_DIR) {
		/* Asks whether every one of those paths would still fit. */
		error = fat_repath_descendants_possible(old_directory->i_mount,
							old_canonical,
							new_canonical);
		if (error != 0) {
			/* Gives back what the failed rename had taken. */
			if (target != NULL)
				inode_release(target);

			inode_release(source);

			/* Failed. */
			return error;
		}
	}

	info = fat_inode(source);

	/* A size past what a 32-bit length expresses. */
	if (source->i_size < 0 ||
	    (uint64_t)source->i_size > UINT32_MAX) {
		/* Gives back what the failed rename had taken. */
		if (target != NULL)
			inode_release(target);

		inode_release(source);

		/* Failed. */
		return EFBIG;
	}

	/* Moves the name on the volume itself. */
	error = fat_raw_rename(state,
			       old_path,
			       new_path,
			       info->fi_first_cluster,
			       (uint32_t)source->i_size,
			       &renamed);
	if (error != 0) {
		/* Gives back what the failed rename had taken. */
		if (target != NULL)
			inode_release(target);

		inode_release(source);

		/* Failed. */
		return error;
	}

	/* A replaced name's chain outlives its record. */
	if (target != NULL)
		fat_orphan(target);

	info->fi_dirent_lba = renamed.lba;
	info->fi_dirent_offset = renamed.offset;
	info->fi_attributes = renamed.attributes;
	source->i_ino = fat_ino(renamed.lba, renamed.offset);

	irq = spin_lock_irqsave(&fat_pool_lock);

	/* Every open file of the source learns its new record. */
	for (i = 0; i < FAT_FILE_MAX; i++) {
		/* A slot of another inode is left alone. */
		if (!fat_files[i].used || fat_files[i].owner != source)
			continue;

		open_state = &fat_files[i];
		open_state->directory_lba = renamed.lba;
		open_state->directory_offset = renamed.offset;
		open_state->directory_dirty = 0;
	}

	spin_unlock_irqrestore(&fat_pool_lock, irq);

	/* A directory carries every path below it with it. */
	if (source->i_type == INODE_DIR) {
		fat_repath_descendants(old_directory->i_mount,
				       old_canonical,
				       new_canonical);
	}

	irq = spin_lock_irqsave(&fat_pool_lock);

	strcpy(fat_slot(source)->path, new_canonical);

	spin_unlock_irqrestore(&fat_pool_lock, irq);

	namecache_remove(old_directory, old_name);
	namecache_remove(new_directory, new_name);

	/* Reports the replaced inode, whose chain is still held. */
	if (target != NULL)
		*orphaned = target;

	inode_release(source);

	/* Succeede. */
	return 0;
}

/* Renames or moves a name. */
static FAT_MUTATION int
fat_rename(
	struct inode *old_directory,
	const struct componentname *old_name,
	struct inode *new_directory,
	const struct componentname *new_name,
	unsigned flags)
{
	struct fat_mount_state *state = fat_mount_state(old_directory->i_mount);
	struct inode *orphaned = NULL;
	int error;

	mutex_lock(&state->lock);

	/* Renames under the mount lock, so the two directories cannot move. */
	error = fat_rename_unlocked(old_directory,
				    old_name,
				    new_directory,
				    new_name,
				    flags,
				    &orphaned);

	mutex_unlock(&state->lock);

	/* An inode whose chain outlived its name is freed here. */
	if (orphaned != NULL)
		fat_release_orphan(orphaned);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Retires an inode, with the mount lock held. */
static void
fat_reclaim_unlocked(
	struct inode *inode)
{
	int result;
	struct fat_inode_info *info = fat_inode(inode);
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	int deferred;

	/* An unlinked inode gives its chain back as it retires. */
	if ((info->fi_flags & FAT_INODE_ORPHANED) != 0 &&
	    info->fi_first_cluster != 0 && state != NULL) {
		/* A chain that cannot be freed now is deferred instead. */
		result = fat_engine_discard_chain_result(state,
			info->fi_first_cluster);
		deferred = 0;
		if (result != 0)
			deferred = fat_defer_orphan(state,
						    info->fi_first_cluster);

		/* The chain is gone once it is freed or safely deferred. */
		if (result == 0 || deferred == 0)
			info->fi_first_cluster = 0;
	}
}

/* Retires an inode the kernel no longer holds. */
static void
fat_reclaim(
	struct inode *inode)
{
	struct fat_inode_info *info;
	struct fat_mount_state *state;

	/* Resolves the mount and the inode this reclaim works on. */
	info = fat_inode(inode);
	state = fat_mount_state(inode->i_mount);

	/*
	 * There is nothing to free unless this is an orphan that still owns
	 * storage: a mount and an inode that are both still there, the
	 * orphaned flag, and a cluster chain to release.
	 */
	if (state == NULL || info == NULL ||
	    (info->fi_flags & FAT_INODE_ORPHANED) == 0 ||
	    info->fi_first_cluster == 0)
		return;
	mutex_lock(&state->lock);

	fat_reclaim_unlocked(inode);

	mutex_unlock(&state->lock);
}

/* Asks which of the three FAT widths a disk carries. */
static int
fat_probe_volume(
	struct disk *disk,
	int direct_io,
	enum bootfat_type *type)
{
	struct fat_mount_state candidate = {
		.disk = disk,
		.read_only = 1,
		.direct_io = direct_io != 0,
	};
	int result;

	/* A call that names no disk, or nowhere to report. */
	if (disk == NULL || type == NULL)
		return EOPNOTSUPP;

	/* Reads the layout, which is what names the width. */
	result = parse_bpb(&candidate);
	if (result != 0)
		return result;
	*type = (enum bootfat_type)candidate.type;
	/* Succeeded. */
	return 0;
}

/* Renders one nibble as a hexadecimal character. */
static char
fat_hex_digit(
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
fat_hex32(
	char output[9],
	uint32_t value)
{
	unsigned i;

	/* Renders the value one nibble at a time, high one first. */
	for (i = 0; i < 8U; i++)
		output[i] = fat_hex_digit((value >> (28U - i * 4U)) & 15U);

	output[8] = '\0';
}

/* Copies a volume label out, trimming and replacing what it cannot show. */
static void
fat_copy_label(
	char *output,
	size_t capacity,
	const uint8_t *input,
	size_t length)
{
	size_t end = length;
	size_t i;

	/* A stored label is padded with spaces to a fixed width. */
	while (end != 0U && (input[end - 1U] == ' ' || input[end - 1U] == 0U))
		end--;

	/* A label longer than the caller buffer is cut to fit. */
	if (end >= capacity)
		end = capacity - 1U;

	/* A byte outside printable ASCII is not shown as itself. */
	for (i = 0; i < end; i++) {
		if (input[i] >= 0x20U && input[i] <= 0x7eU)
			output[i] = (char)input[i];
		else
			output[i] = '_';
	}

	output[end] = '\0';
}

/*
 * Reports what file system a disk carries, if it carries FAT.
 *
 * The caller offers every disk to every driver in turn, so this has to reject
 * a disk that is not FAT without treating it as a damaged one.
 */
static int
fat_identify(
	struct disk *disk,
	struct block_identity *identity)
{
	enum bootfat_type type;
	uint8_t boot[512];
	uint32_t declared_sectors;
	uint32_t fat_sectors;
	uint32_t serial;
	uint32_t sector_scale;
	uint16_t sector_bytes;
	uint16_t reserved_sectors;
	unsigned label_offset;
	unsigned serial_offset;
	int error;
	int difference;

	/* Rejects a call that names no disk or nowhere to report. */
	if (disk == NULL || identity == NULL)
		return EINVAL;

	/* This driver reads 512-byte sectors and nothing else. */
	if (disk->d_block_size != 512U || disk->d_block_count == 0U)
		return EOPNOTSUPP;

	/* Reads the boot sector, which is where every field below lives. */
	error = disk_read_direct(disk, 0, 1, boot);
	if (error != 0)
		return EIO;

	/* A disk without the boot signature carries no FAT. */
	if (boot[510] != 0x55U || boot[511] != 0xaaU)
		return EOPNOTSUPP;

	/* This driver reads 512- and 1024-byte logical sectors. */
	sector_bytes = fat_engine_get16(boot + 11U);
	if (sector_bytes == 512U)
		sector_scale = 1U;
	else if (sector_bytes == 1024U)
		sector_scale = 2U;
	else
		sector_scale = 0U;

	/*
	 * The small counts are zero when the value did not fit in sixteen bits.
	 */
	declared_sectors = fat_engine_get16(boot + 19U);
	if (declared_sectors == 0U)
		declared_sectors = fat_engine_get32(boot + 32U);
	fat_sectors = fat_engine_get16(boot + 22U);
	if (fat_sectors == 0U)
		fat_sectors = fat_engine_get32(boot + 36U);

	/*
	 * An MBR has the same trailing signature.  Require a credible FAT BPB
	 * before treating decoder failures as filesystem corruption.
	 */

	/* A sector size this driver could not read is not a FAT volume. */
	if (sector_scale != 1U && sector_scale != 2U)
		return EOPNOTSUPP;	/* Failed. */

	/* Nor is a volume whose clusters hold no sectors. */
	if (boot[13U] == 0U)
		return EOPNOTSUPP;	/* Failed. */

	/* Nor one with no reserved sectors, since the boot sector is one. */
	reserved_sectors = fat_engine_get16(boot + 14U);
	if (reserved_sectors == 0U)
		return EOPNOTSUPP;	/* Failed. */

	/* Nor one that keeps no allocation table at all. */
	if (boot[16U] == 0U)
		return EOPNOTSUPP;	/* Failed. */

	/* Nor one that declares no sectors, or a table spanning none. */
	if (declared_sectors == 0U || fat_sectors == 0U)
		return EOPNOTSUPP;	/* Failed. */

	/* A volume that claims more sectors than the disk holds is damaged. */
	if (declared_sectors > UINT32_MAX / sector_scale ||
	    (uint64_t)declared_sectors * sector_scale > disk->d_block_count)
		return EINVAL;

	/* Asks the shared probe which of the three widths this is. */
	error = fat_probe_volume(disk, 1, &type);
	if (error != 0)
		return error;

	/* Reports the type, which is all a volume without a label carries. */
	memset(identity, 0, sizeof(*identity));
	strcpy(identity->type, "vfat");
	identity->flags = KERN_BLKID_TYPE;

	/* FAT32 moved the extended fields further into the boot sector. */
	if (type == KERN_FAT32) {
		serial_offset = 67U;
		label_offset = 71U;
	} else {
		serial_offset = 39U;
		label_offset = 43U;
	}

	/*
	 * Succeeded: a volume without the extended signature has no more to
	 * give.
	 */
	if (boot[serial_offset - 1U] != 0x29U)
		return 0;

	/* Renders the serial number in the two-group form tools expect. */
	serial = fat_engine_get32(boot + serial_offset);
	fat_hex32(identity->uuid, serial);
	memmove(identity->uuid + 5, identity->uuid + 4, 4U);
	identity->uuid[4] = '-';
	identity->uuid[9] = '\0';
	identity->flags |= KERN_BLKID_UUID;

	/* Reports the volume label, unless it is the placeholder one. */
	fat_copy_label(identity->label,
		       sizeof(identity->label),
		       boot + label_offset,
		       11U);
	difference = 1;
	if (identity->label[0] != '\0')
		difference = strcmp(identity->label, "NO NAME");

	/* The placeholder label every unnamed volume carries. */
	if (difference != 0)
		identity->flags |= KERN_BLKID_LABEL;

	/*
	 * Succeeded: the caller now holds everything the volume names itself
	 * by.
	 */
	return 0;
}

/* Asks whether a disk carries a volume this driver can mount. */
static int
fat_probe(
	struct disk *disk)
{
	int error;
	enum bootfat_type type;

	/* Asks the shared probe which width this disk carries. */
	error = drv_fat_probe_type(disk, &type);

	/* Reports what the probe found. */
	return error;
}

/* Mounts a volume and builds the inode of its root. */
static int
fat_mount_impl(
	struct mount *mountp)
{
	struct fat_mount_state *state = NULL;
	struct inode *root;
	struct fat_inode_info *info;
	enum bootfat_type type;
	unsigned i;
	unsigned long irq;
	int result;

	irq = spin_lock_irqsave(&fat_pool_lock);

	/* Walks the fixed mounts looking for one that is free. */
	for (i = 0; i < FAT_MOUNT_MAX; i++) {
		/* This slot is free, so it becomes the new mount. */
		if (!fat_mounts[i].used) {
			state = &fat_mounts[i];
			memset(state, 0, sizeof(*state));
			state->used = 1;
			break;
		}
	}

	spin_unlock_irqrestore(&fat_pool_lock, irq);

	/* Every mount slot is taken, so no more can be mounted. */
	if (state == NULL)
		return ENOSPC;

	/* Takes the table the recorded modes and owners are read into. */
	memset(&fat_metadata_tables[i], 0, sizeof(fat_metadata_tables[i]));
	state->metadata = &fat_metadata_tables[i];
	(void)mutex_init(&state->lock, LOCK_RANK_INODE, "FAT mount");
	state->disk = mountp->m_disk;
	state->owner = mountp;
	state->read_only = (mountp->m_flags & MOUNT_READ_ONLY) != 0 ||
			   (mountp->m_disk->d_flags & DISK_READ_ONLY) != 0;

	/* Which of the three widths this volume carries. */
	result = fat_probe_volume(mountp->m_disk, 0, &type);
	if (result == 0) {
		/* Each width has a mount of its own to check its table. */
		switch (type) {
		case KERN_FAT12:
			result = fat12_mount(state);
			break;
		case KERN_FAT16:
			result = fat16_mount(state);
			break;
		case KERN_FAT32:
			result = fat32_mount(state);
			break;
		default:
			result = EOPNOTSUPP;
			break;
		}
	}

	/* A mount that failed gives its slot straight back. */
	if (result != 0) {
		irq = spin_lock_irqsave(&fat_pool_lock);

		memset(&fat_metadata_tables[i], 0,
			sizeof(fat_metadata_tables[i]));
		memset(state, 0, sizeof(*state));

		spin_unlock_irqrestore(&fat_pool_lock, irq);

		/* Failed. */
		return result;
	}

	fat_metadata_load(state);
	mountp->m_data = state;

	/* Takes the inode the root of the volume is presented as. */
	root = inode_alloc(mountp);
	if (root == NULL) {
		irq = spin_lock_irqsave(&fat_pool_lock);

		memset(&fat_metadata_tables[i], 0,
			sizeof(fat_metadata_tables[i]));
		memset(state, 0, sizeof(*state));

		spin_unlock_irqrestore(&fat_pool_lock, irq);

		mountp->m_data = NULL;

		/* Failed. */
		return ENOSPC;
	}

	/* Publishes the root directory of the new mount. */
	info = fat_inode(root);
	root->i_type = INODE_DIR;
	root->i_ino = 1;
	root->i_mode = S_IFDIR | 0755U;
	root->i_linkcount = 1;
	root->i_flags = INODE_ROOT;
	root->i_data = info;
	set_inode_ops(root);
	mountp->m_root = root;

	/* Succeeded. */
	return 0;
}

/* Makes everything this mount has written durable. */
static int
fat_sync_mount(
	struct mount *mountp)
{
	struct fat_file_state *file;
	struct inode *owner;
	unsigned long irq;
	struct fat_mount_state *state = fat_mount_state(mountp);
	unsigned i;
	int error;

	/* A mount this driver did not make has no state. */
	if (state == NULL)
		return EINVAL;

	mutex_lock(&state->lock);

	/* Deferred chains and pending closes are settled first. */
	error = fat_drain_pending_orphans(state);
	if (error == 0)
		error = fat_flush_pending_closes(state);

	/*
	 * A successful filesystem sync includes directory-entry size/cluster
	 * state held by live open descriptions, not only the sector currently
	 * resident in the mount cache.
	 */
	for (i = 0; i < FAT_FILE_MAX && error == 0; i++) {
		file = &fat_files[i];

		/* A slot of another mount, or one with nothing owed. */
		irq = spin_lock_irqsave(&fat_pool_lock);
		if (!file->used || file->mount != state ||
		    !file->directory_dirty) {
			spin_unlock_irqrestore(&fat_pool_lock, irq);
			continue;
		}

		owner = file->owner;
		spin_unlock_irqrestore(&fat_pool_lock, irq);

		/* A retired inode has nothing left to write back. */
		if (owner == NULL || (owner->i_flags & INODE_DEAD) != 0)
			continue;

		/* Writes out what the open file still owed the volume. */
		error = fat_raw_flush_file(file);
		if (error == 0)
			fat_sync_inode_state(owner, file);
	}

	/* The cached sector and the disk are flushed in turn. */
	if (error == 0)
		error = fat_engine_flush(state);
	if (error == 0)
		error = disk_sync(mountp->m_disk);

	mutex_unlock(&state->lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Takes a mount out of service and gives its state back. */
static void
fat_unmount_impl(
	struct mount *mountp)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	struct fat_metadata_table *metadata;
	unsigned long irq;

	/* A mount this driver did not make has no state. */
	if (state == NULL)
		return;

	mutex_lock(&state->lock);

	fat_engine_invalidate(state);

	mutex_unlock(&state->lock);

	metadata = state->metadata;

	irq = spin_lock_irqsave(&fat_pool_lock);

	memset(state, 0, sizeof(*state));

	/* The recorded modes and owners go back with the slot. */
	if (metadata != NULL)
		memset(metadata, 0, sizeof(*metadata));

	spin_unlock_irqrestore(&fat_pool_lock, irq);

	mountp->m_data = NULL;
}

/* Reports how much of the volume is used and how much is free. */
static int
fat_statvfs(
	struct mount *mountp,
	struct statvfs *result)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	struct fat_mount_state *fat;
	uint32_t free_clusters;
	int error;

	/* A mount this driver did not make, or nowhere to report. */
	if (state == NULL || result == NULL)
		return EINVAL;

	mutex_lock(&state->lock);

	fat = state;

	/* Counts the clusters the allocation table calls free. */
	error = fat_engine_count_free_clusters(state, &free_clusters);
	if (error == 0) {
		memset(result, 0, sizeof(*result));
		result->f_bsize = (uint64_t)fat->sectors_per_cluster * 512U;
		result->f_frsize = result->f_bsize;
		result->f_blocks = fat->cluster_count;
		result->f_bfree = free_clusters;
		result->f_bavail = free_clusters;

		/*
		 * FAT has no fixed inode table.  Use clusters as the capacity
		 * unit for the advisory file counts as well.
		 */
		result->f_files = fat->cluster_count;
		result->f_ffree = free_clusters;
		result->f_favail = free_clusters;
		result->f_namemax = NAME_MAX;
	}

	mutex_unlock(&state->lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Proves an overwrite stays within a complete existing cluster chain. */
static int
fat_writeback_range(
	struct file *file,
	off_t offset,
	size_t length)
{
	struct fat_mount_state *mount;
	struct fat_file_state *state;
	struct fat_inode_info *info;
	struct fat_chain_cursor cursor;
	uint32_t wanted;
	int error;
	int valid;

	/* Rejects ranges which require file growth or allocation. */
	if (offset < 0 ||
	    length == 0 ||
	    file->f_inode->i_type != INODE_REG)
		return 0;

	/*
	 * Uses the already opened private writer without creating open state.
	 */
	mount = fat_mount_state(file->f_inode->i_mount);
	mutex_lock(&mount->lock);

	state = file->f_data;

	/*
	 * There is nothing for this path to do on a read-only volume, on a
	 * file that was never opened through it, on one read through a loop
	 * map, or for a run that reaches past the end of the file.
	 */
	if (mount->read_only ||
	    state == NULL ||
	    state->loop_map != NULL ||
	    offset > file->f_inode->i_size ||
	    (uint64_t)length > (uint64_t)(file->f_inode->i_size - offset)) {
		mutex_unlock(&mount->lock);

		/* Succeeded: there was nothing to do. */
		return 0;
	}

	/*
	 * Validates the full chain and rejects a tail before the requested
	 * byte.
	 */
	wanted = (uint32_t)(((uint64_t)offset + length - 1U) /
			    ((uint64_t)mount->sectors_per_cluster * 512U));
	info = fat_inode(file->f_inode);
	error = fat_raw_validate_chain_at(mount, info->fi_first_cluster, wanted,
					  &cursor, NULL);

	mutex_unlock(&mount->lock);

	/* Reports why the position could not be resolved. */
	if (error != 0)
		return -error;

	/* Reports only the requested, valid cluster position as eligible. */
	valid = fat_raw_valid_cluster(mount, cursor.cluster);
	if (cursor.index != wanted || !valid) {
		/* Succeeded. */
		return 0;
	}

	/* Succeeded: the position is one the caller may use. */
	return 1;
}
