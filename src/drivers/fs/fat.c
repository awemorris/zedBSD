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
	char name[ZEDBSD_PATH_MAX];
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
	char text[ZEDBSD_PATH_MAX];
	char sfn[11];
};

/*
 * The mode and owner this mount presents for one path.
 *
 * FAT stores neither, so a mount that wants them keeps them here instead.
 * An entry lives for as long as the mount does and is never written to disk.
 */
struct fat_metadata {
	char path[ZEDBSD_PATH_MAX];
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
	char path[ZEDBSD_PATH_MAX];
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
typedef int (*fat_next_cluster_fn)(struct fat_mount_state *, uint32_t, uint32_t *);
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

/*
 * Forward declaration.
 */
static struct fat_inode_info *fat_inode(struct inode *inode);
static int fat_engine_write_sector_result(struct fat_mount_state *filesystem, uint32_t lba, uint8_t **sector);
static int parse_bpb(struct fat_mount_state *fat);
static FAT_MUTATION int fat_raw_mkdir(struct fat_mount_state *filesystem, const char *path, uint32_t *created_cluster);
static FAT_MUTATION int fat_raw_remove(struct fat_mount_state *filesystem, const char *path, int directory);
static FAT_MUTATION int fat_raw_unlink(struct fat_mount_state *filesystem, const char *path);
static FAT_MUTATION int fat_raw_rmdir(struct fat_mount_state *filesystem, const char *path);
static int fat_raw_read(struct fat_file_state *file, uint64_t offset, void *buffer, uint32_t length, fat_read_progress_fn progress, void *progress_context);
static int fat_engine_file_extents(struct fat_file_state *file, fat_extent_cb callback, void *context);
static int fat_engine_discard_chain_result(struct fat_mount_state *filesystem, uint32_t first_cluster);
static int fat12_mount(struct fat_mount_state *filesystem);
static int fat32_mount(struct fat_mount_state *filesystem);
static struct fat_mount_state *fat_mount_state(struct mount *mountp);
static void fat_metadata_load(struct fat_mount_state *state);
static const struct fat_metadata *fat_metadata_find(const struct fat_mount_state *state, const char *path);
static void fat_metadata_apply(struct mount *mountp, const char *path, struct inode *inode);
static struct fat_inode_slot *fat_slot(struct inode *inode);
static int fat_creation_representation(const struct fat_mount_state *state, const char *path, mode_t *mode, uid_t *uid, gid_t *gid);
static int fat_creation_representable(const struct fat_mount_state *state, const char *path, const struct inode_creation_request *request, enum inode_type type);
static ino_t fat_ino(uint32_t lba, uint16_t offset);
static int fat_month_days(int year, int month);
static int fat_stat_path(struct mount *mountp, const char *path, struct inode **result);
static int fat_stat_path_casefold(struct mount *mountp, const char *path, struct inode **result);
static int fat_lookup_casefold_unlocked(struct inode *directory, const struct componentname *name, struct inode **result);
static struct fat_file_state *fat_file_get(struct file *file);
static ssize_t fat_pread_file_unlocked(struct file *file, void *buffer, size_t length, off_t offset);
static int fat_create_unlocked(struct inode *directory, const struct componentname *name, const struct inode_creation_request *request, struct inode **result);
static FAT_MUTATION void fat_release_orphan(struct inode *inode);
static FAT_MUTATION int fat_mkdir_unlocked(struct inode *directory, const struct componentname *name, const struct inode_creation_request *request, struct inode **result);
static FAT_MUTATION int fat_remove_inode_unlocked(struct inode *directory, const struct componentname *name, int remove_directory, struct inode **orphaned);
static void fat_reclaim_unlocked(struct inode *inode);
static int fat_probe_volume(struct disk *disk, int direct_io, enum bootfat_type *type);
static char fat_hex_digit(unsigned value);
static void fat_hex32(char output[9], uint32_t value);
static uint16_t fat_engine_get16(const uint8_t *bytes);
static uint32_t fat_engine_get32(const uint8_t *bytes);
static int fat_sector_read(struct fat_mount_state *state, uint32_t lba, void *buffer);
static int fat_sector_write(struct fat_mount_state *state, uint32_t lba, const void *buffer);
static int fat_engine_flush(struct fat_mount_state *filesystem);
static void fat_engine_invalidate(struct fat_mount_state *filesystem);
static int fat_engine_read_sector_result(struct fat_mount_state *filesystem, uint32_t lba, const uint8_t **sector);
static int fat_engine_mark_sector_dirty(struct fat_mount_state *filesystem);
static int fat_engine_cluster_lba(struct fat_mount_state *filesystem, uint32_t cluster, uint32_t sector_in_cluster, uint32_t *lba);
static int fat_engine_mount(struct fat_mount_state *filesystem, enum bootfat_type required_type);
static int fat_sfn_encode(const char *path, char output[11]);
static void fat_sfn_decode_lower(const uint8_t raw[32], struct fat_dir_entry *entry);
static void fat_lfn_reset(struct fat_lfn_state *state);
static uint8_t fat_lfn_checksum(const uint8_t sfn[11]);
static int fat_lfn_feed(struct fat_lfn_state *state, const uint8_t raw[32]);
static int append_utf8(char *output, size_t capacity, size_t *used, uint32_t scalar);
static int fat_lfn_finish(struct fat_lfn_state *state, const uint8_t sfn[32], char *output, size_t capacity);
static void fat_sfn_decode_preserve(const uint8_t raw[32], char *output, size_t capacity);
static uint32_t fold_scalar(uint32_t scalar);
static int decode_utf8(const uint8_t **cursor, uint32_t *scalar);
static int fat_utf8_to_utf16(const char *name, uint16_t units[FAT_LFN_MAX_UNITS], unsigned *unit_count);
static void fat_lfn_build_entry(uint8_t raw[32], const uint16_t *units, unsigned unit_count, unsigned ordinal, uint8_t checksum);
static int sfn_character(uint8_t c);
static int fat_sfn_make_alias(const char *name, unsigned serial, uint8_t sfn[11]);
static int fat_utf8_casefold_equal(const char *left, const char *right);
static void text_copy(char *destination, const char *source, size_t capacity);
static void copy_bytes(void *destination, const void *source, uint32_t length);
static void clear_bytes(void *destination, uint32_t length);
static void put16(uint8_t *bytes, uint16_t value);
static void put32(uint8_t *bytes, uint32_t value);
static int fat16_mount(struct fat_mount_state *filesystem);
static int fat_raw_valid_cluster(const struct fat_mount_state *fat, uint32_t cluster);
static int fat_raw_is_end(const struct fat_mount_state *fat, uint32_t cluster);
static uint32_t fat_raw_reserved_limit(const struct fat_mount_state *fat);
static uint32_t fat_raw_end_of_chain(const struct fat_mount_state *fat);
static uint32_t fat_raw_entry_offset(const struct fat_mount_state *fat, uint32_t cluster);
static int fat_raw_next_cluster(struct fat_mount_state *filesystem, uint32_t cluster, uint32_t *next_cluster);
static int fat_engine_count_free_clusters(struct fat_mount_state *filesystem, uint32_t *free_clusters);
static int fat_raw_set_entry_byte(struct fat_mount_state *filesystem, uint32_t copy_start, uint32_t offset, uint8_t keep_mask, uint8_t merge_value);
static int fat_raw_set_cluster_copy(struct fat_mount_state *filesystem, uint32_t cluster, uint32_t value, unsigned copy);
static int fat_raw_set_cluster_immediate(struct fat_mount_state *filesystem, uint32_t cluster, uint32_t value);
static uint32_t fat_raw_dir_cluster(const struct fat_mount_state *fat, const uint8_t raw[32]);
static void fat_raw_put_dir_cluster(const struct fat_mount_state *fat, uint8_t raw[32], uint32_t cluster);
static uint32_t fat_raw_root_cluster(const struct fat_mount_state *fat);
static int fat_raw_validate_chain_at(struct fat_mount_state *filesystem, uint32_t first_cluster, uint32_t wanted_index, struct fat_chain_cursor *cursor, uint32_t *last_cluster);
static int fat_raw_validate_chain(struct fat_mount_state *filesystem, uint32_t first_cluster);
static int fat_raw_free_chain(struct fat_mount_state *filesystem, uint32_t first_cluster);
static int fat_drain_pending_orphans(struct fat_mount_state *filesystem);
static int fat_defer_orphan(struct fat_mount_state *filesystem, uint32_t first_cluster);
static int fat_raw_find_free_cluster(struct fat_mount_state *filesystem, uint32_t *free_cluster);
static int fat_raw_zero_cluster(struct fat_mount_state *filesystem, uint32_t cluster);
static int fat_raw_allocate_cluster(struct fat_mount_state *filesystem, uint32_t *cluster);
static int fat_raw_directory_entry(struct fat_mount_state *filesystem, const struct fat_directory *directory, uint32_t index, uint32_t *entry_lba, uint16_t *entry_offset, const uint8_t **raw);
static int fat_raw_find_entry(struct fat_mount_state *filesystem, const struct fat_directory *directory, const struct fat_component *component, enum fat_name_match match, uint32_t *entry_lba, uint16_t *entry_offset, uint32_t *free_lba, uint16_t *free_offset, char found_name[ZEDBSD_PATH_MAX]);
static int fat_raw_resolve_parent(struct fat_mount_state *filesystem, const char *path, struct fat_directory *parent, struct fat_component *component);
static int fat_raw_resolve_entry(struct fat_mount_state *filesystem, const char *path, uint32_t *lba, uint16_t *offset, const uint8_t **raw, enum fat_name_match match, char found_name[ZEDBSD_PATH_MAX]);
static int fat_raw_populate_file(struct fat_file_state *file, uint32_t lba, uint16_t offset, const uint8_t raw[32]);
static void fat_file_bind(struct fat_file_state *file, struct fat_mount_state *mount);
static int fat_raw_open(struct fat_mount_state *filesystem, const char *path, struct fat_file_state *file);
static int fat_raw_flush_file(struct fat_file_state *file);
static int fat_raw_advance_cluster(struct fat_file_state *file, uint32_t cluster, int allocate, uint32_t *next);
static int fat_raw_cluster_at(struct fat_file_state *file, uint32_t cluster_index, int allocate, uint32_t *found_cluster, struct fat_chain_cursor *cursor);
static int fat_raw_write_bytes(struct fat_file_state *file, uint32_t offset, const uint8_t *input, uint32_t length, int zero, struct fat_chain_cursor *cursor);
static int fat_raw_rollback_growth(struct fat_file_state *file, uint32_t old_first, uint32_t old_last, uint64_t old_size, uint8_t old_directory_dirty);
static int fat_raw_restore_directory(struct fat_file_state *file, uint32_t first_cluster, uint64_t size, uint8_t directory_dirty);
static int fat_raw_write(struct fat_file_state *file, uint64_t offset, const void *buffer, uint32_t length);
static int fat_raw_truncate(struct fat_file_state *file, uint64_t size);
static int fat_raw_sfn_in_use(struct fat_mount_state *filesystem, const struct fat_directory *directory, const uint8_t sfn[11]);
static int fat_raw_extend_directory(struct fat_mount_state *filesystem, const struct fat_directory *directory);
static int fat_raw_find_free_run(struct fat_mount_state *filesystem, const struct fat_directory *directory, unsigned needed, uint32_t *first_index);
static FAT_MUTATION int fat_raw_restore_directory_entry(struct fat_mount_state *filesystem, uint32_t lba, uint16_t offset, const uint8_t entry[32]);
static FAT_MUTATION int fat32_create_entry(struct fat_mount_state *filesystem, const struct fat_directory *parent, const struct fat_component *component, uint8_t attributes, uint32_t first_cluster, uint32_t size, uint32_t *entry_lba, uint16_t *entry_offset);
static FAT_MUTATION int fat_raw_insert_entry(struct fat_mount_state *filesystem, const struct fat_directory *parent, const struct fat_component *component, uint8_t attributes, uint32_t first_cluster, uint32_t size, uint32_t *entry_lba, uint16_t *entry_offset);
static FAT_MUTATION int fat_raw_delete_location(struct fat_mount_state *, const struct fat_directory *, uint32_t, uint16_t);
static FAT_MUTATION int fat_raw_create(struct fat_mount_state *filesystem, const char *path, struct fat_file_state *file);
static FAT_MUTATION int fat_raw_directory_empty(struct fat_mount_state *filesystem, uint32_t first_cluster);
static FAT_MUTATION int fat_raw_initialize_directory(struct fat_mount_state *filesystem, uint32_t cluster, uint32_t parent_cluster);
static FAT_MUTATION int fat_raw_rename(struct fat_mount_state *filesystem, const char *old_path, const char *new_path, uint32_t authoritative_cluster, uint32_t authoritative_size, struct fat_rename_result *renamed);
static int fat_engine_stat_location(struct fat_mount_state *filesystem, const char *path, struct fat_dir_entry *entry, uint32_t *lba, uint16_t *offset, uint32_t *first_cluster, uint8_t *attributes);
static int fat_engine_stat_location_casefold(struct fat_mount_state *filesystem, const char *path, struct fat_dir_entry *entry, uint32_t *lba, uint16_t *offset, uint32_t *first_cluster, uint8_t *attributes);
static const char *fat_path(struct inode *inode);
static struct inode *fat_alloc_inode(struct mount *mountp);
static int join_path(const char *parent, const struct componentname *name, char output[ZEDBSD_PATH_MAX]);
static int fat_creation_collision(struct fat_mount_state *state, const char *path);
static int fat_created_inode_matches(const struct fat_mount_state *state, const char *path, const struct inode *inode);
static time_t fat_decode_time(uint16_t date, uint16_t time);
static FAT_MUTATION int fat_encode_time(time_t seconds, uint16_t *date, uint16_t *time);
static void fat_load_inode_times(struct mount *mountp, struct inode *inode, uint32_t lba, uint16_t offset);
static int fat_make_inode(struct mount *mountp, const char *path, const struct fat_dir_entry *entry, uint32_t lba, uint16_t offset, uint32_t first_cluster, uint8_t attributes, struct inode **result);
static int fat_lookup_unlocked(struct inode *directory, const struct componentname *name, struct inode **result);
static FAT_MUTATION void fat_put16(uint8_t *bytes, uint16_t value);
static FAT_MUTATION int fat_setattr_unlocked(struct inode *inode, const struct stat *status, unsigned mask);
static void fat_sync_inode_state(struct inode *inode, const struct fat_file_state *file);
static ssize_t fat_pwrite_file_unlocked(struct file *file, const void *buffer, size_t length, off_t offset);
static int fat_readdir_unlocked(struct file *file, struct dirent *entry, int *eof);
static void fat_copy_label(char *output, size_t capacity, const uint8_t *input, size_t length);
static int fat_identify(struct disk *disk, struct block_identity *identity);
static int fat_mount_impl(struct mount *mountp);
static int fat_sync_mount(struct mount *mountp);
static void fat_unmount_impl(struct mount *mountp);
static int fat_statvfs(struct mount *mountp, struct statvfs *result);
static int fat_sfn_equal(const uint8_t entry[32], const char name[11]);
static int valid_cluster(uint32_t cluster, uint32_t end_of_chain);
static int fat_engine_read_chain(struct fat_file_state *file, uint64_t offset, void *buffer, uint32_t length, fat_read_progress_fn progress, void *progress_context, fat_next_cluster_fn next_cluster, uint32_t end_of_chain);
static int text_equal(const char *left, const char *right);
static FAT_MUTATION int fat_raw_update_dotdot(struct fat_mount_state *filesystem, uint32_t directory_cluster, uint32_t parent_cluster);
static FAT_MUTATION int fat_raw_restore_entry_payload(struct fat_mount_state *filesystem, uint32_t lba, uint16_t offset, const uint8_t raw[32]);
static FAT_MUTATION void fat_raw_rename_rollback_destination(struct fat_mount_state *filesystem, const struct fat_directory *parent, uint32_t lba, uint16_t offset, int replacing, const uint8_t target[32]);
static int fat_raw_canonical_basename(struct fat_mount_state *filesystem, const char *path, char basename[ZEDBSD_PATH_MAX]);
static int fat_raw_readdir(struct fat_mount_state *filesystem, const char *path, unsigned wanted, struct fat_dir_entry *entry);
static int fat_stat_location_mode(struct fat_mount_state *filesystem, const char *path, struct fat_dir_entry *entry, uint32_t *lba, uint16_t *offset, uint32_t *first_cluster, uint8_t *attributes, enum fat_name_match match);
static int fat_metadata_number(const char *text, unsigned base, uint32_t *value);
static void fat_free_inode(struct inode *inode);
static int fat_leap_year(int year);
static ssize_t fat_loop_transfer(struct file *file, struct fat_file_state *state, void *buffer, size_t length, off_t offset, int writing);
static FAT_MUTATION int fat_path_descendant(const char *parent, const char *path);
static FAT_MUTATION void fat_repath_descendants(struct mount *mountp, const char *old_path, const char *new_path);
static FAT_MUTATION int fat_repath_descendants_possible(struct mount *mountp, const char *old_path, const char *new_path);
static FAT_MUTATION int fat_rename_unlocked(struct inode *old_directory, const struct componentname *old_name, struct inode *new_directory, const struct componentname *new_name, unsigned flags, struct inode **orphaned);
static int fat_probe(struct disk *disk);
static void fat_chain_invalidate(struct fat_mount_state *state);
static int fat_raw_get_cluster_copy(struct fat_mount_state *filesystem, uint32_t cluster, unsigned copy, uint32_t *value);
static int fat_raw_extend_cluster(struct fat_mount_state *filesystem, uint32_t tail, uint32_t *added);
static int fat_raw_allocate_run(struct fat_mount_state *filesystem, uint32_t tail, uint32_t wanted, uint32_t *first);
static int fat_writeback_range(struct file *file, off_t offset, size_t length);
static int fat_file_validate_at(struct fat_file_state *file, uint64_t offset, struct fat_chain_cursor *cursor, uint32_t *last);
static void fat_file_save_cursor(struct fat_file_state *file, const struct fat_chain_cursor *cursor, uint64_t end, uint64_t generation, uint32_t last);
static void fat_engine_copy_bytes(void *destination, const void *source, uint32_t length);
static uint16_t get16(const uint8_t *p);
static int fat_lookup(struct inode *, const struct componentname *, struct inode **);
static int fat_lookup_casefold(struct inode *, const struct componentname *, struct inode **);
static int fat_create(struct inode *, const struct componentname *, const struct inode_creation_request *, struct inode **);
static int fat_mkdir(struct inode *, const struct componentname *, const struct inode_creation_request *, struct inode **);
static int fat_unlink(struct inode *, const struct componentname *);
static int fat_rmdir(struct inode *, const struct componentname *);
static int fat_rename(struct inode *, const struct componentname *, struct inode *, const struct componentname *, unsigned);
static int fat_truncate(struct inode *, off_t);
static int fat_getattr(struct inode *, struct stat *);
static int fat_setattr(struct inode *, const struct stat *, unsigned);
static void fat_reclaim(struct inode *);
static void fat_orphan(struct inode *);
static ssize_t fat_read_file(struct file *, void *, size_t);
static ssize_t fat_write_file(struct file *, const void *, size_t);
static ssize_t fat_pread_file(struct file *, void *, size_t, off_t);
static ssize_t fat_pwrite_file(struct file *, const void *, size_t, off_t);
static ssize_t fat_pwrite_context(struct file *file, const void *buffer, size_t length, off_t offset, unsigned flags, const struct ucred *credential, const struct io_context *context);
static int fat_readdir(struct file *, struct dirent *, int *);
static int fat_open_file(struct file *);
static int fat_fsync(struct file *);
static int fat_close_file(struct file *);
static int fat_flush_pending_closes(struct fat_mount_state *);
static void set_inode_ops(struct inode *inode);
static int fat_sector_images_commit(struct fat_mount_state *filesystem, struct fat_sector_change *slots, unsigned used, uint32_t publish_lba);
static int fat_directory_transaction(struct fat_mount_state *filesystem, const uint32_t *lbas, const uint16_t *offsets, const uint8_t entries[][32], unsigned count, int publish_last);
static int fat_batch_add_lba(uint32_t *lbas, unsigned *used, uint32_t lba);
static uint8_t *fat_batch_bytes(struct fat_sector_change *slots, unsigned used, uint32_t lba);
static int fat_table_transaction(struct fat_mount_state *filesystem, const struct fat_entry_change *changes, unsigned count, int *admitted);
static int fat_raw_set_cluster(struct fat_mount_state *filesystem, uint32_t cluster, uint32_t value);
static int fat_link_initialized_cluster(struct fat_mount_state *filesystem, uint32_t tail, uint32_t added);

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
	.readdir = fat_readdir,
	.close = fat_close_file,
};

const struct filesystem_type drv_fat_filesystem_type = {
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

	/* Handles the disk availability. */
	if (disk == NULL || type == NULL || disk->d_block_size != 512)
		return EOPNOTSUPP;

	/* Obtains the fat probe volume result. */
	error = fat_probe_volume(disk, 0, type);

	/* Returns the computed result. */
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

	/* Handles the state availability. */
	state = fat_file_get(file);
	if (state == NULL) {
		error = EIO;
		goto out;
	}

	/* Handles the map availability. */
	if (map != NULL) {
		/* Handles the f backing claim availability. */
		if (file->f_backing_claim == NULL ||
		    mount->disk->d_block_size != 512U) {
			error = EINVAL;
			goto out;
		}

		/* Process each remaining element. */
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

		/* Handles the state condition. */
		if (state->size % 512U != 0 || next != state->size / 512U) {
			error = EIO;
			goto out;
		}

		/* Checks the operation status. */
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

	/* Handles the inode availability. */
	if (inode == NULL || disk == NULL || object == NULL)
		return EINVAL;

	/* Handles the i mount availability. */
	if (inode->i_type != INODE_REG || inode->i_mount == NULL ||
	    inode->i_mount->m_type != &drv_fat_filesystem_type ||
	    inode->i_mount->m_disk == NULL) {
		/* Failed. */
		return EOPNOTSUPP;
	}

	/* Handles the state availability. */
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

	/* Handles the mount state availability. */
	mount_state = fat_mount_state(file->f_inode->i_mount);
	if (mount_state == NULL)
		return EIO;
	mutex_lock(&mount_state->lock);

	/* Handles the state availability. */
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

/* Retires all shared chain proofs before a mutation or external invalidation. */
static void
fat_chain_invalidate(
	struct fat_mount_state *state)
{
	/* Handles the state condition. */
	if (state->chain_generation != UINT64_MAX)
		state->chain_generation++;
	io_stats_record(IO_FAT_CHAIN_INVALIDATE, 0);
}

/* Supports the fat inode operation. */
static struct fat_inode_info *
fat_inode(
	struct inode *inode)
{
	/* Returns the computed result. */
	return (struct fat_inode_info *)inode;
}

/* Supports the fat engine copy bytes operation. */
static void
fat_engine_copy_bytes(
	void *destination,
	const void *source,
	uint32_t length)
{
	uint8_t *output = destination;
	const uint8_t *input = source;

	/* Process each remaining element. */
	while (length--)
		*output++ = *input++;
}

/* Supports the fat engine get16 operation. */
static uint16_t
fat_engine_get16(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return bytes[0] | ((uint16_t)bytes[1] << 8);
}

/* Supports the fat engine get32 operation. */
static uint32_t
fat_engine_get32(
	const uint8_t *bytes)
{
	uint32_t function_result;

	/* Computes the function result. */
	function_result = fat_engine_get16(bytes) |
			  ((uint32_t)fat_engine_get16(bytes + 2) << 16);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the fat sector read operation. */
static int
fat_sector_read(
	struct fat_mount_state *state,
	uint32_t lba,
	void *buffer)
{
	int error;

	/* Handles the state availability. */
	if (state == NULL || state->disk == NULL || buffer == NULL)
		return EINVAL;

	/* Computes the function result. */
	error =
		state->direct_io ? disk_read_direct(state->disk, lba, 1, buffer)
				 : disk_read(state->disk, lba, 1, buffer);

	/* Returns the computed result. */
	return error;
}

/* Supports the fat sector write operation. */
static int
fat_sector_write(
	struct fat_mount_state *state,
	uint32_t lba,
	const void *buffer)
{
	int function_result;
	struct io_context context;
	int error;

	/* Handles the state availability. */
	if (state == NULL || state->disk == NULL || buffer == NULL)
		return EINVAL;

	/* Handles the state condition. */
	if (state->read_only)
		return EROFS;

	/* Checks the operation status. */
	error = io_context_child(&context, state->write_context,
				 IO_CONTEXT_ORDERED);
	if (error != 0)
		return error;

	/* Obtains the disk write filesystem context result. */
	function_result = disk_write_filesystem_context(state->disk, lba, 1,
							buffer, &context);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the fat engine flush operation. */
static int
fat_engine_flush(
	struct fat_mount_state *filesystem)
{
	int result;

	/* Handles the filesystem condition. */
	if (!filesystem)
		return EINVAL;

	/* Handles the filesystem condition. */
	if (!filesystem->sector_cache_dirty)
		return 0;

	/* Checks the operation result. */
	result = fat_sector_write(filesystem, filesystem->sector_cache_lba,
				  filesystem->sector_cache);
	if (result == 0) {
		filesystem->sector_cache_dirty = 0;

		/* Handles the owner availability. */
		if (filesystem->owner != NULL)
			io_epoch_end(&filesystem->owner->m_write_epoch);
	}

	/* Returns the computed result. */
	return result;
}

/* Supports the fat engine invalidate operation. */
static void
fat_engine_invalidate(
	struct fat_mount_state *filesystem)
{
	/* Handles the filesystem condition. */
	if (!filesystem)
		return;

	/* Handles the owner availability. */
	if (filesystem->sector_cache_dirty && filesystem->owner != NULL)
		io_epoch_end(&filesystem->owner->m_write_epoch);

	filesystem->sector_cache_valid = 0;
	filesystem->sector_cache_dirty = 0;
	memset(filesystem->clean_sectors, 0, sizeof(filesystem->clean_sectors));
	fat_chain_invalidate(filesystem);
}

/* Supports the fat engine read sector result operation. */
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

	/* Handles the lba condition. */
	if (lba >= filesystem->total_sectors)
		return EIO;

	/* Handles the filesystem condition. */
	if (filesystem->sector_cache_valid &&
	    filesystem->sector_cache_lba == lba) {
		io_stats_record(IO_FAT_SECTOR_HIT, 512);
		*sector = filesystem->sector_cache;
		/* Succeeded. */
		return 0;
	}

	/* Completes the old mutable sector before retaining any clean copy. */

	/* Checks the operation result. */
	result = fat_engine_flush(filesystem);
	if (result != 0)
		return result;

	/* Process each remaining element. */
	found = 0;
	for (index = 0; index < FAT_CLEAN_SLOTS; index++) {
		/* Handles the slot condition. */
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
		filesystem->clean_rotor = (filesystem->clean_rotor + 1U) % FAT_CLEAN_SLOTS;
		memcpy(slot->bytes, filesystem->sector_cache, sizeof(slot->bytes));
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

		/* Checks the operation result. */
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

/* Supports the fat engine write sector result operation. */
static int
fat_engine_write_sector_result(
	struct fat_mount_state *filesystem,
	uint32_t lba,
	uint8_t **sector)
{
	const uint8_t *read_sector;
	int result;

	/* Handles the filesystem condition. */
	if (!filesystem || !sector)
		return EINVAL;

	/* Handles the filesystem condition. */
	if (filesystem->read_only)
		return EROFS;

	/* Checks the operation result. */
	result = fat_engine_read_sector_result(filesystem, lba, &read_sector);
	if (result != 0)
		return result;

	*sector = (uint8_t *)read_sector;

	/* Succeeded. */
	return 0;
}

/* Supports the fat engine mark sector dirty operation. */
static int
fat_engine_mark_sector_dirty(
	struct fat_mount_state *filesystem)
{
	/* Handles the filesystem condition. */
	if (!filesystem || filesystem->read_only)
		return EROFS;

	/* Handles the filesystem condition. */
	if (!filesystem->sector_cache_valid)
		return EIO;

	/* Handles the owner availability. */
	if (filesystem->owner != NULL && !filesystem->sector_cache_dirty)
		io_epoch_begin(&filesystem->owner->m_write_epoch);

	filesystem->sector_cache_dirty = 1;

	/* Succeeded. */
	return 0;
}

/* Supports the fat engine cluster lba operation. */
static int
fat_engine_cluster_lba(
	struct fat_mount_state *filesystem,
	uint32_t cluster,
	uint32_t sector_in_cluster,
	uint32_t *lba)
{
	struct fat_mount_state *fat;
	uint32_t cluster_offset;

	/* Handles the filesystem condition. */
	if (!filesystem || !lba)
		return EINVAL;

	/* Handles the cluster condition. */
	fat = filesystem;
	if (cluster < 2 || cluster >= fat->cluster_count + 2 ||
	    sector_in_cluster >= fat->sectors_per_cluster) {
		/* Failed. */
		return EIO;
	}

	/* Handles the cluster condition. */
	if (cluster - 2 > (0xffffffffU - fat->data_start) / fat->sectors_per_cluster) {
		/* Failed. */
		return EIO;
	}

	cluster_offset = fat->data_start + (cluster - 2) * fat->sectors_per_cluster;

	/* Handles the sector in cluster condition. */
	if (sector_in_cluster > 0xffffffffU - cluster_offset)
		return EIO;

	/* Handles the lba condition. */
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
		fat->type = ZEDBSD_FAT12;
	else if (fat->cluster_count < 65525)
		fat->type = ZEDBSD_FAT16;
	else
		fat->type = ZEDBSD_FAT32;

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
	if (fat->type != ZEDBSD_FAT32) {
		/* Succeeded. */
		return 0;
	}

	/* A FAT32 volume that does not carry the FAT32 layout is malformed. */
	if (!fat->fat32_layout)
		return EIO;	/* Failed. */

	/* The first two cluster numbers are reserved, so a root is never one. */
	if (fat->root_cluster < 2U)
		return EIO;	/* Failed. */

	/* Nor may the root sit past the last cluster the volume has. */
	if (fat->root_cluster >= fat->cluster_count + 2U)
		return EIO;	/* Failed. */

	/* Succeeded. */
	return 0;
}

/* Supports the fat engine mount operation. */
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
	if (required_type == ZEDBSD_FAT16 && !fat->fat16_layout)
		return EOPNOTSUPP;	/* Failed. */

	/* And a FAT32 mount needs the root that lives in the data area. */
	if (required_type == ZEDBSD_FAT32 && !fat->fat32_layout)
		return EOPNOTSUPP;	/* Failed. */

	fat->allocation_hint = 2;
	fat_engine_invalidate(filesystem);

	/* Succeeded. */
	return 0;
}

/* Supports the fat sfn encode operation. */
static int
fat_sfn_encode(
	const char *path,
	char output[11])
{
	char character_local;
	char character_local1;
	unsigned i_index_for;
	unsigned base = 0, extension = 0;

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < 11; i_index_for++)
		output[i_index_for] = ' ';

	/* Handles the path condition. */
	if (*path == '/')
		path++;

	/* Handles the path condition. */
	if (!*path)
		return 0;

	/* Continue while the operation condition remains true. */
	while (*path && *path != '.') {
		/* Handles the character local condition. */
		character_local = *path++;
		if (character_local == '/' || base == 8)
			return 0;

		output[base++] =
			character_local >= 'a' && character_local <= 'z'
				? character_local - 32
				: character_local;
	}

	/* Handles the base condition. */
	if (!base)
		return 0;

	/* Handles the path condition. */
	if (*path == '.')
		path++;

	/* Continue while the operation condition remains true. */
	while (*path) {
		/* Handles the character local1 condition. */
		character_local1 = *path++;
		if (character_local1 == '/' ||
		    character_local1 == '.' ||
		    extension == 3)
			return 0;

		output[8 + extension++] = character_local1 >= 'a' && character_local1 <= 'z' ?
			character_local1 - 32 :
			character_local1;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the fat sfn equal operation. */
static int
fat_sfn_equal(
	const uint8_t entry[32],
	const char name[11])
{
	uint8_t left;
	uint8_t right;
	unsigned i_index_for;

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < 11; i_index_for++) {
		left = entry[i_index_for];

		/* Handles the left condition. */
		right = (uint8_t)name[i_index_for];
		if (left >= 'a' && left <= 'z')
			left -= 'a' - 'A';

		/* Handles the right condition. */
		if (right >= 'a' && right <= 'z')
			right -= 'a' - 'A';

		/* Handles the left condition. */
		if (left != right)
			return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the fat sfn decode lower operation. */
static void
fat_sfn_decode_lower(
	const uint8_t raw[32],
	struct fat_dir_entry *entry)
{
	uint8_t character_local;
	uint8_t character_local1;
	unsigned i_index_for;
	unsigned i_index_for1;
	unsigned output = 0;

	/* Process each remaining element. */
	for (i_index_for = 0;
	     i_index_for < 8 && raw[i_index_for] != ' ';
	     i_index_for++) {
		/* Handles the character local condition. */
		character_local = raw[i_index_for];
		if (character_local >= 'A' && character_local <= 'Z')
			character_local += 'a' - 'A';

		entry->name[output++] = (char)character_local;
	}

	/* Handles the raw condition. */
	if (raw[8] != ' ') {
		entry->name[output++] = '.';

		/* Process each remaining element. */
		for (i_index_for1 = 8;
		     i_index_for1 < 11 && raw[i_index_for1] != ' ';
		     i_index_for1++) {
			/* Handles the character local1 condition. */
			character_local1 = raw[i_index_for1];
			if (character_local1 >= 'A' && character_local1 <= 'Z')
				character_local1 += 'a' - 'A';

			entry->name[output++] = (char)character_local1;
		}
	}

	entry->name[output] = 0;
	entry->size = fat_engine_get32(raw + 28);
	entry->attributes = raw[11];
}

/* Supports the valid cluster operation. */
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

/* Supports the fat engine read chain operation. */
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
	int result_local;
	int result_local1;
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

	/* Checks the current offset. */
	if (offset > 0xffffffffU || !next_cluster)
		return EINVAL;

	/* Checks the valid cluster result. */
	if (!valid_cluster(cluster, end_of_chain))
		return EIO;

	generation = fat->chain_generation;
	end = offset + length;

	/* Handles the validation condition. */
	validation = fat_file_validate_at(file, offset, &cursor, &last);
	if (validation != 0)
		return validation;

	cluster = cursor.cluster;
	position = (uint32_t)offset;
	skip = position / 512 - cursor.index * fat->sectors_per_cluster;
	within = position & 511;

	/* Continue while the operation condition remains true. */
	while (skip >= fat->sectors_per_cluster) {
		/* Handles the result local condition. */
		result_local = next_cluster(filesystem, cluster, &cluster);
		if (result_local != 0)
			return result_local;

		/* Checks the valid cluster result. */
		if (!valid_cluster(cluster, end_of_chain))
			return EIO;
		skip -= fat->sectors_per_cluster;
		cursor.index++;
	}

	while (length) {
		int result;

		chunk = 512 - within;

		/* Checks the operation result. */
		result = fat_engine_cluster_lba(filesystem, cluster, skip, &lba);
		if (result != 0)
			return result;

		/* Checks the operation result. */
		result = fat_engine_read_sector_result(filesystem, lba, &input);
		if (result != 0)
			return result;

		/* Handles the chunk condition. */
		if (chunk > length)
			chunk = length;

		fat_engine_copy_bytes(output, input + within, chunk);

		output += chunk;
		length -= chunk;

		/* Handles the progress condition. */
		if (progress) {
			since_update += chunk;

			/* Handles the since update condition. */
			if (since_update >= FAT_PROGRESS_INTERVAL || !length) {
				progress(progress_context, since_update);
				since_update = 0;
			}
		}

		within = 0;

		/* Handles the skip condition. */
		if (++skip >= fat->sectors_per_cluster && length) {
			skip = 0;
			cursor.index++;

			/* Handles the result local1 condition. */
			result_local1 = next_cluster(filesystem, cluster, &cluster);
			if (result_local1 != 0)
				return result_local1;

			/* Checks the valid cluster result. */
			if (!valid_cluster(cluster, end_of_chain))
				return EIO;
		}
	}

	cursor.cluster = cluster;
	fat_file_save_cursor(file, &cursor, end, generation, last);

	/* Succeeded. */
	return 0;
}

/* Supports the get16 operation. */
static uint16_t
get16(
	const uint8_t *p)
{
	/* Returns the computed result. */
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* Supports the fat lfn reset operation. */
static void
fat_lfn_reset(
	struct fat_lfn_state *state)
{
	unsigned i;

	state->unit_limit = 0;
	state->expected = 0;
	state->checksum = 0;
	state->active = 0;
	/* Process each element required by the operation. */
	for (i = 0; i <= FAT_LFN_MAX_UNITS; i++)
		state->units[i] = 0xffffU;
}

/* Supports the fat lfn checksum operation. */
static uint8_t
fat_lfn_checksum(
	const uint8_t sfn[11])
{
	uint8_t sum = 0;
	unsigned i;

	/* Process each element required by the operation. */
	for (i = 0; i < 11; i++)
		sum = (uint8_t)(((sum & 1U) << 7) | (sum >> 1)) + sfn[i];

	/* Returns the computed result. */
	return sum;
}

/* Supports the fat lfn feed operation. */
static int
fat_lfn_feed(
	struct fat_lfn_state *state,
	const uint8_t raw[32])
{
	unsigned index;
	unsigned ordinal = raw[0] & 0x1fU;
	unsigned i;
	uint16_t cluster;

	/* A long-name entry leaves the first cluster field of the record zero. */
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

	/* Handles the raw condition. */
	if (raw[0] & 0x40U) {
		fat_lfn_reset(state);
		state->active = 1;
		state->expected = (uint8_t)ordinal;
		state->checksum = raw[13];
		state->unit_limit = (uint16_t)(ordinal * 13U);

		/* Handles the state condition. */
		if (state->unit_limit > FAT_LFN_MAX_UNITS + 1U)
			state->unit_limit = FAT_LFN_MAX_UNITS + 1U;
	}

	/* Handles the state condition. */
	if (!state->active || ordinal != state->expected ||
	    raw[13] != state->checksum || (raw[0] & 0x80U)) {
		fat_lfn_reset(state);

		/* Succeeded. */
		return 0;
	}

	/* Process each element required by the operation. */
	for (i = 0; i < 13; i++) {
		/* Checks the current index. */
		index = (ordinal - 1U) * 13U + i;
		if (index <= FAT_LFN_MAX_UNITS)
			state->units[index] = get16(raw + lfn_offsets[i]);
	}

	state->expected--;

	/* Reports operation failure. */
	return 1;
}

/* Supports the append utf8 operation. */
static int
append_utf8(
	char *output,
	size_t capacity,
	size_t *used,
	uint32_t scalar)
{
	uint8_t bytes[4];
	unsigned count, i;

	/* Handles the scalar condition. */
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

	/* Checks the current capacity usage. */
	if (*used + count >= capacity)
		return 0;
	/* Process each remaining element. */
	for (i = 0; i < count; i++)
		output[(*used)++] = (char)bytes[i];

	/* Reports operation failure. */
	return 1;
}

/* Supports the fat lfn finish operation. */
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

	/* Checks the fat lfn checksum result. */
	if (!state->active || state->expected != 0 ||
	    state->checksum != fat_lfn_checksum(sfn) || capacity == 0)
		goto invalid;

	/* Process each element required by the operation. */
	for (i = 0; i < state->unit_limit; i++) {
		/* Handles the scalar condition. */
		scalar = state->units[i];
		if (scalar == 0) {
			terminated = 1;
			break;
		}

		/* Handles the scalar condition. */
		if (scalar == 0xffffU)
			goto invalid;

		/* Handles the scalar condition. */
		if (scalar >= 0xd800U && scalar <= 0xdbffU) {
			/* Checks the current index. */
			if (++i >= state->unit_limit)
				goto invalid;

			/* Handles the low condition. */
			low = state->units[i];
			if (low < 0xdc00U || low > 0xdfffU)
				goto invalid;
			scalar = 0x10000U + ((scalar - 0xd800U) << 10) +
				 (low - 0xdc00U);
		} else if (scalar >= 0xdc00U && scalar <= 0xdfffU) {
			goto invalid;
		}

		/* Checks the append utf8 result. */
		if (scalar == '/' || scalar == 0 ||
		    !append_utf8(output, capacity, &used, scalar))
			goto invalid;
	}

	/* Checks the current capacity usage. */
	if (used == 0 || (!terminated && state->unit_limit > FAT_LFN_MAX_UNITS))
		goto invalid;

	/* Handles the terminated condition. */
	if (terminated) {
		/* Process each element required by the operation. */
		for (; i < state->unit_limit; i++) {
			/* Handles the state condition. */
			if (state->units[i] != 0 && state->units[i] != 0xffffU)
				goto invalid;
		}
	}

	output[used] = '\0';
	fat_lfn_reset(state);

	/* Reports operation failure. */
	return 1;
invalid:
	fat_lfn_reset(state);

	/* Handles the capacity condition. */
	if (capacity)
		output[0] = '\0';

	/* Succeeded. */
	return 0;
}

/* Supports the fat sfn decode preserve operation. */
static void
fat_sfn_decode_preserve(
	const uint8_t raw[32],
	char *output,
	size_t capacity)
{
	uint8_t c_local;
	uint8_t c_local1;
	size_t used = 0;
	unsigned i;
	int lower_base = (raw[12] & 0x08U) != 0;
	int lower_ext = (raw[12] & 0x10U) != 0;

	/* Handles the capacity condition. */
	if (capacity == 0)
		return;
	/* Process each element required by the operation. */
	for (i = 0; i < 8 && raw[i] != ' ' && used + 1U < capacity; i++) {
		/* Handles the lower base condition. */
		c_local = raw[i];
		if (lower_base && c_local >= 'A' && c_local <= 'Z')
			c_local += 'a' - 'A';
		output[used++] = (char)c_local;
	}

	/* Handles the raw condition. */
	if (raw[8] != ' ' && used + 1U < capacity) {
		output[used++] = '.';
		/* Process each element required by the operation. */
		for (i = 8; i < 11 && raw[i] != ' ' && used + 1U < capacity;
		     i++) {
			/* Handles the lower ext condition. */
			c_local1 = raw[i];
			if (lower_ext && c_local1 >= 'A' && c_local1 <= 'Z')
				c_local1 += 'a' - 'A';
			output[used++] = (char)c_local1;
		}
	}

	output[used] = '\0';
}

/* Supports the fold scalar operation. */
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

	/* Continue while the operation condition remains true. */
	while (low < high) {
		/* Handles the fat casefold ranges condition. */
		middle = low + (high - low) / 2U;
		if (fat_casefold_ranges[middle].start <= scalar)
			low = middle + 1U;
		else
			high = middle;
	}

	/* Handles the low condition. */
	if (low != 0) {
		range = &fat_casefold_ranges[low - 1U];

		/* Handles the scalar condition. */
		end = range->encoded_end & 0x7fffffffU;
		if (scalar <= end && (!(range->encoded_end & 0x80000000U) ||
				      ((scalar - range->start) & 1U) == 0)) {
			/* Returns the computed result. */
			return (uint32_t)((int32_t)scalar + range->delta);
		}
	}

	/* Returns the computed result. */
	return scalar;
}

/* Supports the decode utf8 operation. */
static int
decode_utf8(
	const uint8_t **cursor,
	uint32_t *scalar)
{
	uint8_t next;
	const uint8_t *p = *cursor;
	uint32_t value;
	unsigned count, i;

	/* Checks the current pointer. */
	if (*p < 0x80U) {
		*scalar = *p;
		*cursor = p + 1;
		/* Returns the computed result. */
		return *p != 0;
	}

	/* Checks the current pointer. */
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

	/* Process each remaining element. */
	for (i = 0; i < count; i++) {
		/* Handles the next condition. */
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

/* Supports the fat utf8 to utf16 operation. */
static int
fat_utf8_to_utf16(
	const char *name,
	uint16_t units[FAT_LFN_MAX_UNITS],
	unsigned *unit_count)
{
	uint32_t scalar;
	const uint8_t *cursor = (const uint8_t *)name;
	unsigned count = 0;

	/* Validates the current name. */
	if (name == 0 || unit_count == 0 || !*cursor)
		return 0;

	/* Continue while the operation condition remains true. */
	while (*cursor) {
		/* Checks the decode utf8 result. */
		if (!decode_utf8(&cursor, &scalar) || scalar == '/' ||
		    scalar < 0x20U || scalar == 0x7fU) {
			/* Succeeded. */
			return 0;
		}

		/* Handles the scalar condition. */
		if (scalar <= 0xffffU) {
			/* Checks the remaining item count. */
			if (count >= FAT_LFN_MAX_UNITS)
				return 0;
			units[count++] = (uint16_t)scalar;
		} else {
			/* Checks the remaining item count. */
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

	/* Reports operation failure. */
	return 1;
}

/* Supports the fat lfn build entry operation. */
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

	/* Process each element required by the operation. */
	for (i = 0; i < 32; i++)
		raw[i] = 0xffU;
	raw[0] = (uint8_t)ordinal;

	/* Handles the ordinal condition. */
	if (ordinal == total)
		raw[0] |= 0x40U;

	raw[11] = 0x0fU;
	raw[12] = 0;
	raw[13] = checksum;
	raw[26] = raw[27] = 0;

	/* Process each element required by the operation. */
	for (i = 0; i < 13; i++) {
		index = (ordinal - 1U) * 13U + i;
		value = index < unit_count
				? units[index]
				: (index == unit_count ? 0 : 0xffffU);
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

/* Supports the fat sfn make alias operation. */
static int
fat_sfn_make_alias(
	const char *name,
	unsigned serial,
	uint8_t sfn[11])
{
	uint8_t c_local;
	uint8_t c_local1;
	const uint8_t *p = (const uint8_t *)name;
	const uint8_t *dot = 0, *q;
	uint8_t base[8], extension[3], digits[6];
	unsigned base_count = 0, extension_count = 0, digit_count = 0, i;

	/* Validates the current name. */
	if (name == 0 || sfn == 0 || serial == 0 || serial > 999999U)
		return 0;

	/* Process each element required by the operation. */
	for (q = p; *q; q++) {
		/* Handles the q condition. */
		if (*q == '.')
			dot = q;
	}

	/* Process each element required by the operation. */
	for (q = p; *q && q != dot; q++) {
		/* Handles the c local condition. */
		c_local = *q;
		if (c_local >= 'a' && c_local <= 'z')
			c_local -= 'a' - 'A';

		/* Checks the sfn character result. */
		if (sfn_character(c_local) && base_count < sizeof(base))
			base[base_count++] = c_local;
	}

	/* Handles the dot condition. */
	if (dot != 0) {
		/* Process each remaining element. */
		for (q = dot + 1; *q && extension_count < sizeof(extension);
		     q++) {
			/* Handles the c local1 condition. */
			c_local1 = *q;
			if (c_local1 >= 'a' && c_local1 <= 'z')
				c_local1 -= 'a' - 'A';

			/* Handles the sfn character condition. */
			if (sfn_character(c_local1))
				extension[extension_count++] = c_local1;
		}
	}

	/* Handles the base count condition. */
	if (base_count == 0) {
		base[0] = 'F';
		base[1] = 'I';
		base[2] = 'L';
		base[3] = 'E';
		base_count = 4;
	}

	while (serial) {
		digits[digit_count++] = (uint8_t)('0' + serial % 10U);
		serial /= 10U;
	}

	/* Process each element required by the operation. */
	for (i = 0; i < 11; i++)
		sfn[i] = ' ';

	/* Handles the base count condition. */
	if (base_count > 7U - digit_count)
		base_count = 7U - digit_count;

	/* Process each remaining element. */
	for (i = 0; i < base_count; i++)
		sfn[i] = base[i];
	sfn[base_count++] = '~';

	/* Process each remaining element. */
	while (digit_count)
		sfn[base_count++] = digits[--digit_count];

	/* Process each remaining element. */
	for (i = 0; i < extension_count; i++)
		sfn[8U + i] = extension[i];

	/* Reports operation failure. */
	return 1;
}

/* Supports the fat utf8 casefold equal operation. */
static int
fat_utf8_casefold_equal(
	const char *left,
	const char *right)
{
	uint32_t left_scalar, right_scalar;
	const uint8_t *a = (const uint8_t *)left;
	const uint8_t *b = (const uint8_t *)right;

	/* Handles the left availability. */
	if (left == NULL || right == NULL)
		return 0;

	/* Continue while the operation condition remains true. */
	while (*a && *b) {
		/* Checks the decode utf8 result. */
		if (!decode_utf8(&a, &left_scalar) ||
		    !decode_utf8(&b, &right_scalar) ||
		    fold_scalar(left_scalar) != fold_scalar(right_scalar)) {
			/* Succeeded. */
			return 0;
		}
	}

	/* The names match only if both ran out at the same point. */
	if (*a != 0 || *b != 0)
		return 0;

	/* Reports that the two names are equal apart from case. */
	return 1;
}

/* Supports the text equal operation. */
static int
text_equal(
	const char *left,
	const char *right)
{
	/* Continue while the operation condition remains true. */
	while (*left && *left == *right) {
		left++;
		right++;
	}

	/* Returns the computed result. */
	return *left == *right;
}

/* Supports the text copy operation. */
static void
text_copy(
	char *destination,
	const char *source,
	size_t capacity)
{
	/* Handles the capacity condition. */
	if (capacity == 0)
		return;

	/* Continue while the operation condition remains true. */
	while (--capacity && *source)
		*destination++ = *source++;

	*destination = '\0';
}

/* Supports the copy bytes operation. */
static void
copy_bytes(
	void *destination,
	const void *source,
	uint32_t length)
{
	uint8_t *output = destination;
	const uint8_t *input = source;

	/* Process each remaining element. */
	while (length--)
		*output++ = *input++;
}

/* Supports the clear bytes operation. */
static void
clear_bytes(
	void *destination,
	uint32_t length)
{
	uint8_t *output = destination;

	/* Process each remaining element. */
	while (length--)
		*output++ = 0;
}

/* Supports the put16 operation. */
static void
put16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/* Supports the put32 operation. */
static void
put32(
	uint8_t *bytes,
	uint32_t value)
{
	put16(bytes, (uint16_t)value);
	put16(bytes + 2, (uint16_t)(value >> 16));
}

/* Supports the fat16 mount operation. */
static int
fat16_mount(
	struct fat_mount_state *filesystem)
{
	struct fat_mount_state *fat;
	int result;
	uint32_t fat_entries;

	/* Checks the operation result. */
	result = fat_engine_mount(filesystem, ZEDBSD_FAT16);
	if (result != 0)
		return result;

	/* Handles the fat condition. */
	fat = filesystem;
	if (!fat->root_entries || !fat->fat_sectors ||
	    fat->fat_sectors > 0xffffffffU / 512U) {
		/* Failed. */
		return EIO;
	}

	/* Handles the fat entries condition. */
	fat_entries = fat->fat_sectors * 512U / 2U;
	if (fat_entries < fat->cluster_count + 2U ||
	    fat->cluster_count + 2U >= FAT16_RESERVED_CLUSTER) {
		/* Failed. */
		return EIO;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the fat raw valid cluster operation. */
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
	if (fat->type == ZEDBSD_FAT12)
		first_end = 0xff8U;
	else if (fat->type == ZEDBSD_FAT16)
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
	if (fat->type == ZEDBSD_FAT12)
		return FAT12_RESERVED_CLUSTER;
	if (fat->type == ZEDBSD_FAT16)
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
	if (fat->type == ZEDBSD_FAT12)
		return FAT12_END_OF_CHAIN;
	if (fat->type == ZEDBSD_FAT16)
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
	if (fat->type == ZEDBSD_FAT12)
		return cluster + cluster / 2U;

	/* The wider formats hold two or four whole bytes per entry. */
	if (fat->type == ZEDBSD_FAT16)
		return cluster * 2U;

	/* Reports the offset of the only remaining width. */
	return cluster * 4U;
}

/* Supports the fat raw next cluster operation. */
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

	io_stats_record(IO_FAT_CHAIN_STEP, 0);

	/* Checks the fat raw valid cluster result. */
	if (!next_cluster || !fat_raw_valid_cluster(fat, cluster))
		return EIO;

	offset = fat_raw_entry_offset(fat, cluster);

	/* Checks the operation result. */
	result = fat_engine_read_sector_result(
		filesystem, fat->fat_start + (offset >> 9), &sector);
	if (result != 0)
		return result;

	/* Handles the fat condition. */
	if (fat->type == ZEDBSD_FAT32) {
		*next_cluster = fat_engine_get32(sector + (offset & 511U)) & 0x0fffffffU;

		/* Succeeded. */
		return 0;
	}

	/* Handles the fat condition. */
	if (fat->type == ZEDBSD_FAT16) {
		*next_cluster = fat_engine_get16(sector + (offset & 511U));
		/* Reports successful completion. */

		return 0;
	}

	/*
	 * A 12-bit entry may straddle a sector boundary, and the
	 * sector cache holds one sector, so latch the first byte
	 * before a second read can evict it.
	 */
	low = sector[offset & 511U];

	/* Checks the current offset. */
	if ((offset & 511U) == 511U) {
		/* Checks the operation result. */
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

/* Supports the fat engine count free clusters operation. */
static int
fat_engine_count_free_clusters(
	struct fat_mount_state *filesystem,
	uint32_t *free_clusters)
{
	uint32_t value;
	int result;
	struct fat_mount_state *fat;
	uint32_t cluster, count = 0;

	/* Handles the filesystem availability. */
	if (filesystem == NULL || free_clusters == NULL)
		return EINVAL;

	/* Handles the fat availability. */
	fat = filesystem;
	if (fat == NULL || fat->cluster_count == 0)
		return EIO;

	/* Process each remaining element. */
	for (cluster = 2U; cluster < fat->cluster_count + 2U; cluster++) {
		/* Checks the operation result. */
		result = fat_raw_next_cluster(filesystem, cluster, &value);
		if (result != 0)
			return result;

		/* Validates the current value. */
		if (value == 0)
			count++;
	}

	*free_clusters = count;

	/* Succeeded. */
	return 0;
}

/* Supports the fat raw set entry byte operation. */
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

	/* Checks the current offset. */
	if ((offset >> 9) > 0xffffffffU - copy_start)
		return EIO;

	/* Checks the operation result. */
	result = fat_engine_write_sector_result(filesystem,
						copy_start + (offset >> 9),
						&sector);
	if (result != 0)
		return result;

	sector[offset & 511U] = (uint8_t)((sector[offset & 511U] & keep_mask) | merge_value);

	/* Checks the operation result. */
	result = fat_engine_mark_sector_dirty(filesystem);
	if (result == 0)
		result = fat_engine_flush(filesystem);

	/* Failed. */
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Supports the fat raw set cluster copy operation. */
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

	/* Checks the fat raw valid cluster result. */
	if (!fat_raw_valid_cluster(fat, cluster) || copy >= fat->number_of_fats)
		return EIO;

	offset = fat_raw_entry_offset(fat, cluster);

	/* Handles the copy condition. */
	if (copy > (0xffffffffU - fat->fat_start) / fat->fat_sectors)
		return EIO;

	/* Handles the fat condition. */
	copy_start = fat->fat_start + copy * fat->fat_sectors;
	if (fat->type != ZEDBSD_FAT12) {
		/* Checks the current offset. */
		if ((offset >> 9) > 0xffffffffU - copy_start)
			return EIO;

		/* Checks the operation result. */
		result = fat_engine_write_sector_result(
			filesystem, copy_start + (offset >> 9), &sector);
		if (result != 0)
			return result;

		/* Handles the fat condition. */
		if (fat->type == ZEDBSD_FAT32) {
			entry = sector + (offset & 511U);
			old = fat_engine_get32(entry);
			put32(entry,
			      (old & 0xf0000000U) | (value & 0x0fffffffU));
		} else {
			put16(sector + (offset & 511U), (uint16_t)value);
		}

		/* Checks the operation result. */
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
		/* Checks the operation result. */
		result = fat_raw_set_entry_byte(filesystem, copy_start, offset,
						0x0f,
						(uint8_t)((value << 4) & 0xf0));
		if (result == 0) {
			result = fat_raw_set_entry_byte(filesystem, copy_start,
							offset + 1U, 0x00,
							(uint8_t)(value >> 4));
		}
	} else {
		/* Checks the operation result. */
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

	/* Handles the copy condition. */
	if (copy > (UINT32_MAX - filesystem->fat_start) / filesystem->fat_sectors) {
		/* Failed. */
		return EIO;
	}

	/* Checks the current offset. */
	lba = filesystem->fat_start + copy * filesystem->fat_sectors;
	if ((offset >> 9) > UINT32_MAX - lba)
		return EIO;
	lba += offset >> 9;

	/* Checks the operation status. */
	error = fat_engine_read_sector_result(filesystem, lba, &sector);
	if (error != 0)
		return error;

	/*
	 * Decode aligned entries and retain each copy's reserved high bits on
	 * rewrite.
	 */
	if (filesystem->type == ZEDBSD_FAT32) {
		*value = fat_engine_get32(sector + (offset & 511U)) &
			 0x0fffffffU;

		/* Succeeded. */
		return 0;
	}

	/* Handles the filesystem condition. */
	if (filesystem->type == ZEDBSD_FAT16) {
		*value = fat_engine_get16(sector + (offset & 511U));

		/* Succeeded. */
		return 0;
	}

	/*
	 * Preserve the first packed byte before switching a sector-boundary
	 * window.
	 */
	low = sector[offset & 511U];

	/* Checks the current offset. */
	if ((offset & 511U) == 511U) {
		/* Handles the lba condition. */
		if (lba == UINT32_MAX)
			return EIO;

		/* Checks the operation status. */
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

	/* Refuses a cluster number that names no entry. */
	if (!fat_raw_valid_cluster(filesystem, cluster))
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
		/* Handles the lbas condition. */
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
		/* Handles the slots condition. */
		if (slots[n].lba == lba)
			return slots[n].new_bytes;
	}

	/*
	 * Report an inconsistent preflight rather than dereference a missing
	 * image.
	 */
	return NULL;
}

/* Publish complete sector images with optional final directory-name ordering. */
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

	/* Handles the owner availability. */
	if (filesystem->owner != NULL)
		io_epoch_begin(&filesystem->owner->m_write_epoch);

	/* Publish every mirror and confirm the complete table transaction. */
	for (n = 0; n < used; n++) {
		/* Handles the slots condition. */
		if (slots[n].lba == publish_lba)
			continue;

		/* Checks the operation status. */
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
		/* Checks the current capacity usage. */
		if (used > 1)
			error = disk_sync(filesystem->disk);
		if (error == 0) {
			error = fat_sector_write(
				filesystem, publish_lba,
				fat_batch_bytes(slots, used, publish_lba));
		}
	}

	/* Checks the operation status. */
	if (error == 0)
		error = disk_sync(filesystem->disk);

	/*
	 * Restore all captured sectors, including writes that may have
	 * completed with error.
	 */
	rollback = 0;

	/* Checks the operation status. */
	if (error != 0) {
		/* Process each element required by the operation. */
		for (n = 0; n < used; n++) {
			/* Handles the rollback condition. */
			restored = fat_sector_write(filesystem, slots[n].lba,
						    slots[n].old_bytes);
			if (rollback == 0 && restored != 0)
				rollback = restored;
		}

		/* Handles the rollback condition. */
		restored = disk_sync(filesystem->disk);
		if (rollback == 0)
			rollback = restored;

		/* Handles the rollback condition. */
		if (rollback != 0)
			filesystem->read_only = 1;
	}

	fat_engine_invalidate(filesystem);

	/* Handles the owner availability. */
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

/* Commit bounded FAT sector images and restore every mirror on uncertain errors. */
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

	/*
	 * Validate the complete bounded operation before any write or
	 * allocation.
	 */
	*admitted = 0;

	/* Handles the filesystem condition. */
	if (filesystem->read_only)
		return EROFS;

	/* Checks the remaining item count. */
	if (count == 0)
		return 0;

	/* Checks the remaining item count. */
	if (count > FAT_BATCH_ENTRIES)
		return E2BIG;

	used = 0;
	bytes = filesystem->type == ZEDBSD_FAT32 ? 4U : 2U;

	/* Process each remaining element. */
	for (entry = 0; entry < count; entry++) {
		/* Checks the fat raw valid cluster result. */
		if (!fat_raw_valid_cluster(filesystem, changes[entry].cluster))
			return EIO;

		offset = fat_raw_entry_offset(filesystem, changes[entry].cluster);

		/* Process each element required by the operation. */
		for (copy = 0; copy < filesystem->number_of_fats; copy++) {
			/* Handles the copy condition. */
			if (copy > (UINT32_MAX - filesystem->fat_start) / filesystem->fat_sectors) {
				/* Failed. */
				return EIO;
			}

			/* Handles the uint64 t condition. */
			copy_start = filesystem->fat_start + copy * filesystem->fat_sectors;
			if (((uint64_t)offset + bytes - 1U) / 512U > UINT32_MAX - copy_start) {
				/* Failed. */
				return EIO;
			}

			/* Checks the operation status. */
			error = fat_batch_add_lba(lbas, &used, copy_start + offset / 512U);
			if (error != 0)
				return error;

			/* Checks the operation status. */
			error = fat_batch_add_lba(lbas,
						  &used,
						  copy_start + (offset + bytes - 1U) / 512U);
			if (error != 0)
				return error;
		}
	}

	/*
	 * Decline admission without side effects when working memory is
	 * unavailable.
	 */

	/* Handles the slots availability. */
	slots = kern_malloc(used * sizeof(*slots));
	if (slots == NULL)
		return ENOMEM;

	*admitted = 1;

	/* Checks the operation status. */
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

		/* Checks the operation status. */
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
		offset = fat_raw_entry_offset(filesystem, changes[entry].cluster);
		value = changes[entry].value;

		/* Process each element required by the operation. */
		for (copy = 0; copy < filesystem->number_of_fats; copy++) {
			copy_start = filesystem->fat_start + copy * filesystem->fat_sectors;
			first = fat_batch_bytes(slots, used, copy_start + offset / 512U);

			/* Handles the first availability. */
			second = fat_batch_bytes(slots, used, copy_start + (offset + 1U) / 512U);
			if (first == NULL || second == NULL) {
				kern_free(slots);

				/* Failed. */
				return EIO;
			}

			first += offset & 511U;
			second += (offset + 1U) & 511U;

			/* Handles the filesystem condition. */
			if (filesystem->type == ZEDBSD_FAT32) {
				put32(first, (fat_engine_get32(first) & 0xf0000000U) | (value & 0x0fffffffU));
			} else if (filesystem->type == ZEDBSD_FAT16) {
				put16(first, (uint16_t)value);
			} else if (changes[entry].cluster & 1U) {
				*first = (uint8_t)((*first & 0x0fU) | ((value << 4) & 0xf0U));
				*second = (uint8_t)(value >> 4);
			} else {
				*first = (uint8_t)value;
				*second = (uint8_t)((*second & 0xf0U) | ((value >> 8) & 0x0fU));
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

/* Preserve the immediate fallback with the same required durability boundaries. */
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

	/* Checks the operation status. */
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

	/* Handles the first offset condition. */
	bytes = filesystem->type == ZEDBSD_FAT32 ? 4U : 2U;
	if (first_offset / 512U == second_offset / 512U &&
	    first_offset / 512U == (first_offset + bytes - 1U) / 512U &&
	    second_offset / 512U == (second_offset + bytes - 1U) / 512U) {
		changes[0].cluster = added;
		changes[0].value = fat_raw_end_of_chain(filesystem);
		changes[1].cluster = tail;
		changes[1].value = added;

		/* Checks the operation status. */
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

	/* Checks the operation status. */
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

/* Merge a bounded directory run while preserving neighboring and hidden slots. */
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

	/* Checks the remaining item count. */
	if (count == 0 || count > FAT_LFN_MAX_ENTRIES + 1U)
		return EINVAL;

	used = 0;

	/* Process each remaining element. */
	for (n = 0; n < count; n++) {
		/* Handles the offsets condition. */
		if (offsets[n] > 512U - 32U || (offsets[n] & 31U) != 0)
			return EINVAL;

		/* Checks the operation status. */
		error = fat_batch_add_lba(identities, &used, lbas[n]);
		if (error != 0)
			return error;
	}

	/* Handles the slots availability. */
	slots = kern_malloc(used * sizeof(*slots));
	if (slots == NULL)
		return ENOMEM;

	/* Checks the operation status. */
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

		/* Checks the operation status. */
		error = fat_sector_read(filesystem, identities[n],
					slots[n].old_bytes);
		if (error != 0) {
			kern_free(slots);

			/* Failed. */
			return error;
		}

		memcpy(slots[n].new_bytes, slots[n].old_bytes, 512U);
	}

	/* Process each remaining element. */
	for (n = 0; n < count; n++) {
		/* Handles the bytes availability. */
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
	error = fat_sector_images_commit(filesystem,
					 slots,
					 used,
					 publish_last ? lbas[count - 1U] : UINT32_MAX);
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

	/* Handles the fat condition. */
	if (fat->type == ZEDBSD_FAT32)
		cluster |= (uint32_t)fat_engine_get16(raw + 20) << 16;

	/* Returns the computed result. */
	return cluster & 0x0fffffffU;
}

/* Supports the fat raw put dir cluster operation. */
static void
fat_raw_put_dir_cluster(
	const struct fat_mount_state *fat,
	uint8_t raw[32],
	uint32_t cluster)
{
	put16(raw + 26, (uint16_t)cluster);

	/* Handles the fat condition. */
	if (fat->type == ZEDBSD_FAT32)
		put16(raw + 20, (uint16_t)(cluster >> 16));
}

/* Supports the fat raw root cluster operation. */
static uint32_t
fat_raw_root_cluster(
	const struct fat_mount_state *fat)
{
	/* FAT32 puts its root in an ordinary cluster chain. */
	if (fat->type == ZEDBSD_FAT32)
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
	uint32_t next;
	int result;
	struct fat_mount_state *fat = filesystem;
	uint32_t cluster = first_cluster, checkpoint = first_cluster;
	uint32_t steps, span = 0U, power = 1U;

	/* Checks the fat raw valid cluster result. */
	if (!fat_raw_valid_cluster(fat, first_cluster))
		return EIO;

	/* Handles the cursor availability. */
	if (cursor != NULL)
		cursor->cluster = 0U;

	/* Process each remaining element. */
	for (steps = 0; steps <= fat->cluster_count; steps++) {
		/* Handles the cursor availability. */
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

		/* Checks the operation result. */
		result = fat_raw_next_cluster(filesystem, cluster, &next);
		if (result != 0)
			return result;

		/* Handles the fat raw is end condition. */
		if (fat_raw_is_end(fat, next)) {
			/* Handles the last cluster availability. */
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

		/* Checks the fat raw valid cluster result. */
		if (!fat_raw_valid_cluster(fat, next))
			return EIO;
		cluster = next;
		span++;

		/* Handles the cluster condition. */
		if (cluster == checkpoint)
			return EIO;

		/* Handles the span condition. */
		if (span == power) {
			checkpoint = cluster;
			span = 0U;

			/* Handles the power condition. */
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

	/* Consumes the old proof before fallible validation or subsequent I/O. */
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

/* Publishes a successful operation only if its validated chain stayed unchanged. */
static void
fat_file_save_cursor(
	struct fat_file_state *file,
	const struct fat_chain_cursor *cursor,
	uint64_t end,
	uint64_t generation,
	uint32_t last)
{
	file->cursor_valid = 0;

	/* Handles the generation condition. */
	if (generation == UINT64_MAX ||
	    generation != file->mount->chain_generation ||
	    cursor->cluster == 0) {
		/* Returns the computed result. */
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

/* Supports the fat raw validate chain operation. */
static int
fat_raw_validate_chain(
	struct fat_mount_state *filesystem,
	uint32_t first_cluster)
{
	int error;

	/* Obtains the fat raw validate chain at result. */
	error = fat_raw_validate_chain_at(filesystem,
					  first_cluster,
					  0U,
					  NULL,
					  NULL);

	/* Returns the computed result. */
	return error;
}

/* Supports the fat raw free chain operation. */
static int
fat_raw_free_chain(
	struct fat_mount_state *filesystem,
	uint32_t first_cluster)
{
	uint32_t next_local;
	uint32_t next_local1;
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

	/* Handles the first cluster condition. */
	if (!first_cluster)
		return 0;

	/* Checks the operation result. */
	result = fat_raw_validate_chain(filesystem, first_cluster);
	if (result != 0)
		return result;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Checks the operation result. */
		result = fat_raw_next_cluster(filesystem, cluster, &next_local);
		if (result != 0)
			return result;

		/* Checks the remaining item count. */
		if (++count > fat->cluster_count)
			return EIO;

		/* Handles the fat raw is end condition. */
		if (fat_raw_is_end(fat, next_local))
			break;
		cluster = next_local;
	}

#if SIZE_MAX <= UINT32_MAX

	/* Checks the remaining item count. */
	if (count > (uint32_t)(SIZE_MAX / sizeof(*clusters)))
		return ENOMEM;
#endif

	/* Handles the clusters availability. */
	clusters = kern_malloc((size_t)count * sizeof(*clusters));
	if (clusters == NULL)
		return ENOMEM;

	/* Process each remaining element. */
	cluster = first_cluster;
	for (index = 0; index < count; index++) {
		clusters[index] = cluster;
		result = fat_raw_next_cluster(filesystem, cluster, &next_local1);
		if (result != 0)
			goto out;
		cluster = next_local1;
	}

	/* Process each remaining element. */
	index = 0;
	while (index < count) {
		/*
		 * Stage a bounded free prefix while retaining the complete
		 * recovery chain.
		 */
		batch = count - index > FAT_BATCH_ENTRIES ?
			FAT_BATCH_ENTRIES :
			count - index;

		/* Process each element required by the operation. */
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

		/* Handles the admitted condition. */
		if (!admitted && (result == ENOMEM || result == E2BIG)) {
			batch = 1;
			result = fat_raw_set_cluster(filesystem,
						     clusters[index], 0);
		}

		/* Checks the operation result. */
		if (result != 0) {
			rollback = 0;

			/*
			 * The failing entry restores itself.  Recreate every
			 * link cleared earlier so callers can also restore the
			 * directory entry and expose the complete old file
			 * after an error.
			 */
			for (restore = 0; restore < index; restore++) {
				/* Checks the operation status. */
				restore_error = fat_raw_set_cluster(filesystem,
								    clusters[restore],
								    restore + 1U < count ?
								    clusters[restore + 1U] :
								    fat_raw_end_of_chain(fat));
				if (rollback == 0 && restore_error != 0)
					rollback = restore_error;
			}

			/* Handles the rollback condition. */
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

/* Supports the fat drain pending orphans operation. */
static int
fat_drain_pending_orphans(
	struct fat_mount_state *filesystem)
{
	unsigned index;
	int result;

	/* Process each remaining element. */
	while (filesystem->pending_orphan_count != 0U) {
		index = filesystem->pending_orphan_count - 1U;

		/* Checks the operation result. */
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

/* Supports the fat defer orphan operation. */
static int
fat_defer_orphan(
	struct fat_mount_state *filesystem,
	uint32_t first_cluster)
{
	unsigned index;

	/* Handles the filesystem availability. */
	if (filesystem == NULL || first_cluster == 0)
		return EINVAL;

	/* Process each remaining element. */
	for (index = 0; index < filesystem->pending_orphan_count; index++) {
		/* Handles the filesystem condition. */
		if (filesystem->pending_orphans[index] == first_cluster)
			return 0;
	}

	/* Handles the filesystem condition. */
	if (filesystem->pending_orphan_count >= FAT_INODE_MAX)
		return ENOSPC;

	filesystem->pending_orphans[filesystem->pending_orphan_count++] = first_cluster;

	/* Succeeded. */
	return 0;
}

/* Supports the fat raw find free cluster operation. */
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

	/* Handles the free cluster condition. */
	if (!free_cluster || !fat->cluster_count)
		return EIO;

	/* Checks the fat raw valid cluster result. */
	if (!fat_raw_valid_cluster(fat, start))
		start = 2;

	/* Process each remaining element. */
	for (index = 0; index < fat->cluster_count; index++) {
		cluster = 2U + ((start - 2U + index) % fat->cluster_count);

		/* Checks the operation result. */
		result = fat_raw_next_cluster(filesystem, cluster, &value);
		if (result != 0)
			return result;

		/* Validates the current value. */
		if (!value) {
			*free_cluster = cluster;
			fat->allocation_hint = cluster + 1U;

			/* Checks the fat raw valid cluster result. */
			if (!fat_raw_valid_cluster(fat, fat->allocation_hint))
				fat->allocation_hint = 2;

			/* Succeeded. */
			return 0;
		}
	}

	/* Failed. */
	return ENOSPC;
}

/* Supports the fat raw zero cluster operation. */
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

	/* Process each remaining element. */
	for (index = 0; index < fat->sectors_per_cluster; index++) {
		/* Checks the operation result. */
		result = fat_engine_cluster_lba(filesystem, cluster, index, &lba);
		if (result != 0)
			return result;

		/* Checks the operation result. */
		result = fat_engine_write_sector_result(filesystem, lba,
							&sector);
		if (result != 0)
			return result;

		clear_bytes(sector, 512);

		/* Checks the operation result. */
		result = fat_engine_mark_sector_dirty(filesystem);
		if (result == 0)
			result = fat_engine_flush(filesystem);
		if (result != 0)
			return result;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the fat raw allocate cluster operation. */
static int
fat_raw_allocate_cluster(
	struct fat_mount_state *filesystem,
	uint32_t *cluster)
{
	int error;
	int result;

	/* Checks the operation result. */
	result = fat_raw_find_free_cluster(filesystem, cluster);
	if (result != 0)
		return result;

	/* Checks the operation result. */
	result = fat_raw_zero_cluster(filesystem, *cluster);
	if (result != 0)
		return result;

	/* Obtains the fat raw set cluster result. */
	error = fat_raw_set_cluster(filesystem,
				    *cluster,
				    fat_raw_end_of_chain(filesystem));

	/* Returns the computed result. */
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

	/* Checks the operation status. */
	error = fat_raw_find_free_cluster(filesystem, added);
	if (error != 0)
		return error;

	/* Checks the operation status. */
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

/* Prepare an initialized private chain before linking it into an existing file. */
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

	/*
	 * Bound initialization work and leave room for a same-sector old-tail
	 * link.
	 */
	*first = 0;
	cluster_bytes = (uint32_t)filesystem->sectors_per_cluster * 512U;

	/* Handles the limit condition. */
	limit = 65536U / cluster_bytes;
	if (limit == 0)
		limit = 1;

	/* Handles the limit condition. */
	if (limit > FAT_BATCH_ENTRIES - (tail != 0))
		limit = FAT_BATCH_ENTRIES - (tail != 0);

	/* Handles the wanted condition. */
	if (wanted < limit)
		limit = wanted;

	/* Handles the limit condition. */
	if (limit == 0)
		return EINVAL;
	bytes = filesystem->type == ZEDBSD_FAT32 ? 4U : 2U;
	count = 0;
	sector = 0;

	/*
	 * Reserve identities under the mount lock without publishing FAT
	 * entries.
	 */
	while (count < limit) {
		/* Checks the operation status. */
		error = fat_raw_find_free_cluster(filesystem, &cluster);
		if (error == ENOSPC && count != 0)
			break;
		if (error != 0)
			return error;
		/* Process each remaining element. */
		for (n = 0; n < count; n++) {
			/* Handles the clusters condition. */
			if (clusters[n] == cluster)
				break;
		}

		/* Checks the current item count. */
		if (n != count)
			break;

		/* Checks the remaining item count. */
		offset = fat_raw_entry_offset(filesystem, cluster);
		if (count != 0 && (offset / 512U != sector ||
				   (offset + bytes - 1U) / 512U != sector)) {
			filesystem->allocation_hint = cluster;
			break;
		}

		sector = offset / 512U;

		/* Checks the operation status. */
		error = fat_raw_zero_cluster(filesystem, cluster);
		if (error != 0)
			return error;
		clusters[count++] = cluster;

		/* Checks the current offset. */
		if ((offset + bytes - 1U) / 512U != sector)
			break;
	}

	/*
	 * Encode a complete private chain, with its final endpoint already
	 * initialized.
	 */
	for (n = 0; n < count; n++) {
		changes[n].cluster = clusters[n];
		changes[n].value = n + 1U < count
					   ? clusters[n + 1U]
					   : fat_raw_end_of_chain(filesystem);
	}

	entries = count;
	combined = 0;

	/* Handles the tail condition. */
	if (tail != 0) {
		/* Checks the fat raw entry offset result. */
		offset = fat_raw_entry_offset(filesystem, tail);
		if (offset / 512U == sector &&
		    (offset + bytes - 1U) / 512U == sector &&
		    (fat_raw_entry_offset(filesystem, clusters[0]) + bytes -
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
		/* Handles the tail condition. */
		if (tail != 0) {
			error = fat_link_initialized_cluster(filesystem, tail,
							     clusters[0]);
		} else {
			error = fat_raw_set_cluster(
				filesystem, clusters[0],
				fat_raw_end_of_chain(filesystem));
		}

		/* Checks the operation status. */
		if (error == 0)
			*first = clusters[0];
		/* Failed. */
		return error;
	}

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/*
	 * Link across sectors only after the entire new chain is durably
	 * readable.
	 */
	if (tail != 0 && !combined) {
		/* Checks the operation status. */
		error = fat_raw_set_cluster(filesystem, tail, clusters[0]);
		if (error != 0) {
			/* Handles the filesystem condition. */
			if (filesystem->read_only)
				return error;

			/* Handles the rollback condition. */
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

/* Supports the fat raw directory entry operation. */
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

	/* Handles the directory condition. */
	if (directory->first_cluster == 0) {
		/* Checks the current index. */
		if (index >= fat->root_entries)
			return ENOENT;
		lba = fat->root_start + index / FAT16_ENTRIES_PER_SECTOR;
		offset = (uint16_t)((index % FAT16_ENTRIES_PER_SECTOR) *
				    FAT16_DIRECTORY_ENTRY_SIZE);
	} else {
		entries_per_cluster = (uint32_t)fat->sectors_per_cluster *
				      FAT16_ENTRIES_PER_SECTOR;

		/* Checks the fat raw valid cluster result. */
		cluster = directory->first_cluster;
		if (!fat_raw_valid_cluster(fat, cluster) ||
		    !entries_per_cluster) {
			/* Failed. */
			return EIO;
		}

		/* Process each remaining element. */
		cluster_index = index / entries_per_cluster;
		index %= entries_per_cluster;
		while (cluster_index--) {
			/* Checks the operation result. */
			result = fat_raw_next_cluster(filesystem, cluster, &next);
			if (result != 0)
				return result;

			/* Handles the fat raw is end condition. */
			if (fat_raw_is_end(fat, next))
				return ENOENT;

			/* Checks the fat raw valid cluster result. */
			if (!fat_raw_valid_cluster(fat, next))
				return EIO;
			cluster = next;
		}

		sector_index = index / FAT16_ENTRIES_PER_SECTOR;

		/* Checks the operation result. */
		result = fat_engine_cluster_lba(filesystem, cluster, sector_index, &lba);
		if (result != 0)
			return result;

		offset = (uint16_t)((index % FAT16_ENTRIES_PER_SECTOR) *
				    FAT16_DIRECTORY_ENTRY_SIZE);
	}

	/* Checks the operation result. */
	result = fat_engine_read_sector_result(filesystem, lba, &sector);
	if (result != 0)
		return result;

	*entry_lba = lba;
	*entry_offset = offset;
	*raw = sector + offset;

	/* Succeeded. */
	return 0;
}

/* Supports the fat raw find entry operation. */
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
	char found_name[ZEDBSD_PATH_MAX])
{
	char decoded_local[ZEDBSD_PATH_MAX];
	struct fat_dir_entry decoded_local1;
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

	/* Process each remaining element. */
	for (index = 0; index < limit; index++) {
		/* Checks the operation result. */
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

		/* Handles the raw condition. */
		if (!raw[0]) {
			fat_lfn_reset(&lfn);
			break;
		}

		/* Handles the raw condition. */
		if (raw[0] == 0xe5) {
			fat_lfn_reset(&lfn);
			continue;
		}

		/* Handles the raw condition. */
		if (raw[11] == 0x0f) {
			/* Handles the fat condition. */
			if (fat->type == ZEDBSD_FAT32)
				(void)fat_lfn_feed(&lfn, raw);
			continue;
		}

		/* Handles the raw condition. */
		if (raw[11] & 0x08U) {
			fat_lfn_reset(&lfn);
			continue;
		}

		/* Handles the fat condition. */
		if (fat->type == ZEDBSD_FAT32) {
			/* Checks the fat lfn finish result. */
			if (!fat_lfn_finish(&lfn,
					    raw,
					    decoded_local,
					    sizeof(decoded_local))) {
				fat_sfn_decode_preserve(raw,
							decoded_local,
							sizeof(decoded_local));
			}

			/* Compares the decoded name the way the caller asked. */
			if (match == FAT_NAME_EXACT)
				matches = text_equal(decoded_local,
						     component->text);
			else
				matches = fat_utf8_casefold_equal(
						  decoded_local,
						  component->text);

			/* Skips an entry that names something else. */
			if (!matches)
				continue;

			/* Handles the found name condition. */
			if (found_name != 0)
				text_copy(found_name, decoded_local, ZEDBSD_PATH_MAX);
		} else {
			/* Checks the fat sfn equal result. */
			if (!fat_sfn_equal(raw, component->sfn))
				continue;

			/* Handles the found name condition. */
			if (found_name != 0) {
				fat_sfn_decode_lower(raw, &decoded_local1);
				text_copy(found_name, decoded_local1.name, ZEDBSD_PATH_MAX);
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

/* Supports the fat raw resolve parent operation. */
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

	parent->first_cluster = fat_raw_root_cluster(filesystem);

	/* Checks the current cursor position. */
	if (*cursor == '/')
		cursor++;

	/* Checks the current cursor position. */
	if (!*cursor)
		return EINVAL;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		length = 0;
		lba = 0;
		free_lba = 0;
		offset = 0;
		free_offset = 0;

		/* Continue while the operation condition remains true. */
		separator = cursor;
		while (*separator && *separator != '/')
			separator++;

		/* Checks the current data length. */
		length = (unsigned)(separator - cursor);
		if (!length || length >= sizeof(component->text))
			return EINVAL;

		copy_bytes(component->text, cursor, length);
		component->text[length] = '\0';

		/* Checks the fat sfn encode result. */
		if (filesystem->type != ZEDBSD_FAT32) {
			if (!fat_sfn_encode(component->text, component->sfn)) {
				/* Failed. */
				return EINVAL;
			}
		}

		/* Handles the separator condition. */
		if (!*separator)
			return 0;

		/* Checks the current cursor position. */
		cursor = separator + 1;
		if (!*cursor)
			return EINVAL;

		/* Checks the operation result. */
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

		result = fat_engine_read_sector_result(filesystem, lba, &sector);
		if (result != 0)
			return result;

		/* Handles the sector condition. */
		if (!(sector[offset + 11] & 0x10U))
			return ENOENT;

		parent->first_cluster = fat_raw_dir_cluster(filesystem, sector + offset);

		/* Checks the fat raw valid cluster result. */
		if (!fat_raw_valid_cluster(filesystem, parent->first_cluster))
			return EIO;
	}
}

/* Supports the fat raw resolve entry operation. */
static int
fat_raw_resolve_entry(
	struct fat_mount_state *filesystem,
	const char *path,
	uint32_t *lba,
	uint16_t *offset,
	const uint8_t **raw,
	enum fat_name_match match,
	char found_name[ZEDBSD_PATH_MAX])
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
	if (!fat_raw_valid_cluster(fat, state->first_cluster))
		return EIO;

	/* Succeeded: the file is described by an entry that makes sense. */
	return 0;
}

/* Supports the fat file bind operation. */
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

/* Supports the fat raw open operation. */
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

	/* Handles the filesystem availability. */
	if (filesystem == NULL || path == NULL || file == NULL)
		return EINVAL;

	fat_file_bind(file, filesystem);

	/* Checks the operation result. */
	result = fat_raw_resolve_entry(filesystem,
				       path,
				       &lba,
				       &offset,
				       &raw,
				       FAT_NAME_EXACT,
				       0);
	if (result != 0)
		return result;

	/* Handles the raw condition. */
	if (raw[11] & 0x10U)
		return EINVAL;

	/* Obtains the fat raw populate file result. */
	error = fat_raw_populate_file(file, lba, offset, raw);

	/* Returns the computed result. */
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

	/* Nothing more is due once the volume is flushed and the entry clean. */
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

/* Supports the fat raw advance cluster operation. */
static int
fat_raw_advance_cluster(
	struct fat_file_state *file,
	uint32_t cluster,
	int allocate,
	uint32_t *next)
{
	int result = fat_raw_next_cluster(file->mount, cluster, next);

	/* Checks the operation result. */
	if (result != 0)
		return result;

	/* Checks the fat raw is end result. */
	if (fat_raw_is_end(file->mount, *next)) {
		/* Handles the allocate condition. */
		if (!allocate)
			return EIO;

		/* Checks the operation result. */
		result = fat_raw_extend_cluster(file->mount, cluster, next);
		if (result != 0)
			return result;
	} else if (!fat_raw_valid_cluster(file->mount, *next)) {
		/* Failed. */
		return EIO;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the fat raw cluster at operation. */
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

	/* Handles the cursor availability. */
	if (cursor != NULL && cursor->cluster != 0U &&
	    cursor->index <= cluster_index) {
		cluster = cursor->cluster;
		index = cursor->index;
	}

	/* Handles the cluster condition. */
	if (!cluster) {
		/* Handles the allocate condition. */
		if (!allocate)
			return EIO;

		/* Checks the operation result. */
		result = fat_raw_allocate_cluster(file->mount, &cluster);
		if (result != 0)
			return result;

		state->first_cluster = cluster;
		state->directory_dirty = 1;
	}

	/* Checks the fat raw valid cluster result. */
	if (!fat_raw_valid_cluster(fat, cluster))
		return EIO;

	/* Process each remaining element. */
	for (; index < cluster_index; index++) {
		/* Checks the current index. */
		if (index >= fat->cluster_count)
			return EIO;

		/* Checks the operation result. */
		result = fat_raw_advance_cluster(file, cluster, allocate, &next);
		if (result != 0)
			return result;

		cluster = next;
	}
	*found_cluster = cluster;

	/* Handles the cursor availability. */
	if (cursor != NULL) {
		cursor->index = cluster_index;
		cursor->cluster = cluster;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the fat raw write bytes operation. */
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

	/* Checks the current data length. */
	if (length == 0U)
		return 0;

	/* Populate an empty file with one bounded initialized chain. */
	if (file->first_cluster == 0 && position == 0) {
		wanted = (uint32_t)(((uint64_t)length + cluster_bytes - 1U) / cluster_bytes);

		/* Checks the operation result. */
		result = fat_raw_allocate_run(fat, 0, wanted, &cluster);
		if (result != 0)
			return result;

		file->first_cluster = cluster;
		file->directory_dirty = 1;
	}

	/* Checks the operation result. */
	result = fat_raw_cluster_at(file, position / cluster_bytes, 1, &cluster, cursor);
	if (result != 0)
		return result;

	/* Process each remaining element. */
	while (length) {
		in_cluster = position % cluster_bytes;
		sector_index = in_cluster / 512U;
		within = in_cluster & 511U;

		/* Handles the chunk condition. */
		chunk = 512U - within;
		if (chunk > length)
			chunk = length;

		/* Checks the operation result. */
		result = fat_engine_cluster_lba(file->mount,
						cluster,
						sector_index,
						&lba);
		if (result != 0)
			return result;

		/* Checks the operation result. */
		result = fat_engine_write_sector_result(file->mount,
							lba,
							&sector);
		if (result != 0)
			return result;

		/* Handles the zero condition. */
		if (zero)
			clear_bytes(sector + within, chunk);
		else
			copy_bytes(sector + within, input, chunk);

		/* Checks the operation result. */
		result = fat_engine_mark_sector_dirty(file->mount);
		if (result == 0)
			result = fat_engine_flush(file->mount);
		if (result != 0)
			return result;

		/* Handles the zero condition. */
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
			/* Checks the current cursor position. */
			if (cursor->index >= fat->cluster_count)
				return EIO;

			/* Checks the fat raw is end result. */
			result = fat_raw_next_cluster(fat, cluster, &next);
			if (result == 0) {
				if (fat_raw_is_end(fat, next)) {
					wanted = (uint32_t)(((uint64_t)length + cluster_bytes - 1U) / cluster_bytes);
					result = fat_raw_allocate_run(fat, cluster, wanted, &next);
				}
			}

			/* Checks the fat raw valid cluster result. */
			if (result == 0 && !fat_raw_valid_cluster(fat, next))
				result = EIO;
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

/* Supports the fat raw rollback growth operation. */
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

	/* Handles the old first condition. */
	if (old_first == 0) {
		added = state->first_cluster;
	} else if (old_last != 0) {
		/* Checks the fat raw is end result. */
		result = fat_raw_next_cluster(fat, old_last, &added);
		if (result == 0 && fat_raw_is_end(fat, added))
			added = 0;
		else if (result == 0 && !fat_raw_valid_cluster(fat, added))
			result = EIO;
		if (result == 0 && added != 0) {
			result = fat_raw_set_cluster(fat,
						     old_last,
						     fat_raw_end_of_chain(fat));
		}
	}

	/* Checks the operation result. */
	if (result == 0 && added != 0)
		result = fat_raw_free_chain(fat, added);

	state->first_cluster = old_first;
	file->size = old_size;
	state->directory_dirty = old_directory_dirty;

	/* Checks the operation result. */
	if (result != 0)
		fat->read_only = 1;

	/* Failed. */
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Supports the fat raw restore directory operation. */
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

	/* Checks the operation result. */
	result = fat_raw_flush_file(file);
	if (result == 0)
		state->directory_dirty = directory_dirty;

	/* Returns the computed result. */
	return result;
}

/* Supports the fat raw write operation. */
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

	/* Handles the file condition. */
	if (file->mount->read_only)
		return EROFS;

	/* Handles the buffer condition. */
	if ((!buffer && length) || offset > 0xffffffffU ||
	    (uint64_t)length > 0xffffffffU - offset) {
		/* Failed. */
		return EINVAL;
	}

	/* Checks the current data length. */
	if (!length)
		return 0;

	generation = file->mount->chain_generation;
	end = offset + length;
	old_first = state->first_cluster;
	old_size = file->size;
	old_directory_dirty = state->directory_dirty;

	/* Handles the file condition. */
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

	/* Checks the current offset. */
	if (offset > file->size) {
		/* Checks the operation result. */
		result = fat_raw_write_bytes(file,
					     (uint32_t)file->size, 0,
					     (uint32_t)(offset - file->size),
					     1,
					     &cursor);
		if (result != 0) {
			/* Handles the rollback local condition. */
			rollback = fat_raw_rollback_growth(file,
								 old_first,
								 old_last,
								 old_size,
								 old_directory_dirty);
			if (rollback != 0)
				return rollback;

			/* Failed. */
			return result;
		}
	}

	/* Checks the operation result. */
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

	/* Checks the current endpoint. */
	if (end > file->size) {
		file->size = end;
		file->directory_dirty = 1;
	}

	fat_file_save_cursor(file, &cursor, end, generation, old_last);

	/* Succeeded. */
	return 0;
}

/* Supports the fat raw truncate operation. */
static int
fat_raw_truncate(
	struct fat_file_state *file,
	uint64_t size)
{
	uint32_t cluster_bytes;
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

	/* Handles the file condition. */
	if (file->mount->read_only)
		return EROFS;

	/* Checks the current data size. */
	if (size > 0xffffffffU)
		return EINVAL;

	/* A zero-length file may still own a cluster chain. */
	if (size == file->size && (size || !state->first_cluster))
		return 0;

	/* Handles the state condition. */
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

		/* Checks the operation result. */
		result = fat_raw_validate_chain_at(file->mount,
						   state->first_cluster,
						   (uint32_t)first_offset / cluster_bytes,
						   &cursor,
						   &old_last);
		if (result != 0)
			return result;
	}

	/* Checks the current data size. */
	if (size > file->size) {
		/* Checks the operation result. */
		result = fat_raw_write_bytes(file,
					     (uint32_t)file->size,
					     0,
					     (uint32_t)(size - file->size),
					     1,
					     &cursor);
		if (result != 0) {
			/* Handles the rollback local condition. */
			rollback = fat_raw_rollback_growth(file,
								 old_first,
								 old_last,
								 old_size,
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

	/* Checks the current data size. */
	if (!size) {
		state->first_cluster = 0;
		file->size = 0;
		state->directory_dirty = 1;

		/* Checks the operation result. */
		result = fat_raw_flush_file(file);
		if (result != 0) {
			/* Puts the entry back the way the failed flush found it. */
			rollback = fat_raw_restore_directory(file,
								    old_first,
								    old_size,
								    old_directory_dirty);
			if (rollback != 0) {
				file->mount->read_only = 1;

				/* Failed. */
				return rollback;
			}

			/* Failed. */
			return result;
		}

		/* Checks the operation result. */
		result = fat_raw_free_chain(file->mount, old_first);
		if (result != 0) {
			/* Handles the rollback local2 condition. */
			rollback = fat_raw_restore_directory(file,
								    old_first,
								    old_size,
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

	/* Checks the operation result. */
	result = fat_raw_cluster_at(file, keep_index, 0, &keep, &cursor);
	if (result != 0)
		return result;

	/* Checks the operation result. */
	result = fat_raw_next_cluster(file->mount, keep, &tail);
	if (result != 0)
		return result;

	/* Checks the fat raw is end result. */
	if (!fat_raw_is_end(fat, tail) && !fat_raw_valid_cluster(fat, tail)) {
		/* Failed. */
		return EIO;
	}

	/* Checks the fat raw is end result. */
	if (!fat_raw_is_end(fat, tail)) {
		/* Checks the operation result. */
		result = fat_raw_set_cluster(file->mount, keep,
					     fat_raw_end_of_chain(fat));
		if (result != 0)
			return result;
	}

	file->size = size;
	state->directory_dirty = 1;

	/* Checks the operation result. */
	result = fat_raw_flush_file(file);
	if (result != 0) {
		rollback = 0;

		/* Checks the fat raw is end result. */
		if (!fat_raw_is_end(fat, tail)) {
			cleanup = fat_raw_set_cluster(file->mount, keep, tail);

			/* Handles the rollback local3 condition. */
			if (rollback == 0 && cleanup != 0)
				rollback = cleanup;
		}

		/* Handles the rollback local3 condition. */
		cleanup = fat_raw_restore_directory(file, old_first, old_size, old_directory_dirty);
		if (rollback == 0 && cleanup != 0)
			rollback = cleanup;

		/* Handles the rollback local3 condition. */
		if (rollback != 0) {
			file->mount->read_only = 1;

			/* Failed. */
			return rollback;
		}

		/* Failed. */
		return result;
	}

	/* Handles the fat raw is end condition. */
	if (fat_raw_is_end(fat, tail))
		return 0;

	/* Checks the operation result. */
	result = fat_raw_free_chain(file->mount, tail);
	if (result != 0) {
		rollback = 0;

		/* Handles the cleanup local5 condition. */
		cleanup = fat_raw_set_cluster(file->mount, keep, tail);
		if (cleanup != 0)
			rollback = cleanup;

		/* Handles the rollback local4 condition. */
		cleanup = fat_raw_restore_directory(file, old_first, old_size, old_directory_dirty);
		if (rollback == 0 && cleanup != 0)
			rollback = cleanup;

		/* Handles the rollback local4 condition. */
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

/* Supports the fat raw sfn in use operation. */
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
	uint32_t limit = fat->cluster_count * (uint32_t)fat->sectors_per_cluster * FAT16_ENTRIES_PER_SECTOR;
	uint32_t index;

	/* Process each remaining element. */
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

		/* Handles the raw condition. */
		if (!raw[0])
			return ENOENT;

		/* Checks the fat sfn equal result. */
		if (raw[0] != 0xe5 &&
		    raw[11] != 0x0f &&
		    fat_sfn_equal(raw, (const char *)sfn)) {
			/* Succeeded. */
			return 0;
		}
	}

	/* Failed. */
	return EIO;
}

/* Supports the fat raw extend directory operation. */
static int
fat_raw_extend_directory(
	struct fat_mount_state *filesystem,
	const struct fat_directory *directory)
{
	struct fat_mount_state *fat = filesystem;
	uint32_t last = directory->first_cluster;
	uint32_t steps, next, added;
	int result;

	/* Checks the fat raw valid cluster result. */
	if (fat->type != ZEDBSD_FAT32 || !fat_raw_valid_cluster(fat, last))
		return ENOSPC;

	/* Process each remaining element. */
	for (steps = 0; steps < fat->cluster_count; steps++) {
		/* Checks the operation result. */
		result = fat_raw_next_cluster(filesystem, last, &next);
		if (result != 0)
			return result;

		/* Handles the fat raw is end condition. */
		if (fat_raw_is_end(fat, next))
			break;

		/* Checks the fat raw valid cluster result. */
		if (!fat_raw_valid_cluster(fat, next))
			return EIO;

		last = next;
	}

	/* Handles the steps condition. */
	if (steps == fat->cluster_count)
		return EIO;

	/* Checks the operation result. */
	result = fat_raw_extend_cluster(filesystem, last, &added);
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Supports the fat raw find free run operation. */
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
	uint32_t maximum = fat->cluster_count * (uint32_t)fat->sectors_per_cluster * FAT16_ENTRIES_PER_SECTOR;
	uint32_t index = 0, run_start = 0;
	unsigned run = 0;
	int after_end = 0;

	/* Process each remaining element. */
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
			/* Checks the operation result. */
			result = fat_raw_extend_directory(filesystem, directory);
			if (result != 0)
				return result;
			continue;
		}

		/* Checks the operation result. */
		if (result != 0)
			return result;

		/* Handles the after end condition. */
		if (after_end || raw[0] == 0 || raw[0] == 0xe5) {
			/* Handles the run condition. */
			if (run++ == 0)
				run_start = index;

			/* Handles the raw condition. */
			if (raw[0] == 0)
				after_end = 1;

			/* Handles the run condition. */
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

	/* Select an unused short alias before reserving directory positions. */
	if (!fat_utf8_to_utf16(component->text, units, &unit_count))
		return EINVAL;

	/* Process each element required by the operation. */
	for (serial = 1; serial <= 999999U; serial++) {
		/* Checks the fat sfn make alias result. */
		if (!fat_sfn_make_alias(component->text, serial, sfn))
			return EINVAL;

		/* Checks the operation result. */
		result = fat_raw_sfn_in_use(filesystem, parent, sfn);
		if (result == ENOENT)
			break;
		if (result != 0)
			return result;
	}

	/* Handles the serial condition. */
	if (serial > 999999U)
		return ENOSPC;

	lfn_count = (unit_count + 12U) / 13U;
	first_index = 0;

	/* Checks the operation result. */
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
		/* Checks the operation result. */
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

	/* Checks the operation result. */
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

	/* Handles the entry offset availability. */
	if (entry_offset != NULL)
		*entry_offset = offsets[lfn_count];

	/* Succeeded. */
	return 0;
}

/* Supports the fat raw insert entry operation. */
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

	/* Checks the operation result. */
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

	/* Handles the filesystem condition. */
	if (filesystem->type == ZEDBSD_FAT32) {
		/* Checks the operation result. */
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

		/* Obtains the fat32 create entry result. */
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

	/* Checks the operation result. */
	if (result == ENOSPC)
		return result;

	/* Checks the operation result. */
	result = fat_engine_write_sector_result(filesystem, free_lba, &sector);
	if (result != 0)
		return result;

	copy_bytes(saved, sector + free_offset, sizeof(saved));
	clear_bytes(sector + free_offset, 32);
	copy_bytes(sector + free_offset, component->sfn, 11);
	sector[free_offset + 11] = attributes;

	fat_raw_put_dir_cluster(filesystem, sector + free_offset, first_cluster);

	put32(sector + free_offset + 28, size);

	/* Checks the operation result. */
	result = fat_engine_mark_sector_dirty(filesystem);
	if (result == 0)
		result = fat_engine_flush(filesystem);
	if (result != 0) {
		/* Handles the rollback condition. */
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

	/* Checks the operation result. */
	if (result == 0) {
		/* Handles the entry lba condition. */
		if (entry_lba != 0)
			*entry_lba = free_lba;

		/* Handles the entry offset condition. */
		if (entry_offset != 0)
			*entry_offset = free_offset;
	}

	/* Failed. */
	if (result != 0)
		return result;

	/* Succeeded. */
	return 0;
}

/* Supports the fat raw create operation. */
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

	/* Handles the filesystem availability. */
	if (filesystem == NULL || path == NULL || file == NULL)
		return EINVAL;

	fat_file_bind(file, filesystem);

	/* Handles the filesystem condition. */
	if (filesystem->read_only)
		return EROFS;

	/* Checks the operation result. */
	result = fat_raw_resolve_parent(filesystem, path, &parent, &component);
	if (result != 0)
		return result;

	/* Checks the operation result. */
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
	if (result != ENOENT && !(filesystem->type == ZEDBSD_FAT32 && result == ENOSPC)) {
		/* Failed. */
		return result;
	}

	/* Checks the operation result. */
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

	/* Checks the operation result. */
	result = fat_engine_read_sector_result(filesystem, lba, &sector);
	if (result == 0)
		result = fat_raw_populate_file(file, lba, offset, sector + offset);
	if (result == 0)
		return 0;

	/* Handles the rollback condition. */
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

	/* Process each remaining element. */
	for (index = 0; index < limit; index++) {
		/* Checks the operation result. */
		result = fat_raw_directory_entry(filesystem, parent, index, &lba, &offset, &raw);
		if (result != 0)
			return result;

		/* Handles the lba condition. */
		if (lba == target_lba && offset == target_offset)
			break;
	}

	/* Checks the current index. */
	if (index == limit)
		return ENOENT;

	memcpy(target, raw, sizeof(target));

	/*
	 * Collect all associated records before changing directory
	 * interpretation.
	 */
	count = 0;
	while (index != 0) {
		/* Checks the operation result. */
		result = fat_raw_directory_entry(filesystem, parent, index - 1U, &lba, &offset, &raw);
		if (result != 0)
			return result;

		/* Handles the raw condition. */
		if (raw[11] != 0x0fU || raw[0] == 0xe5)
			break;

		/* Checks the remaining item count. */
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

	/* Process each remaining element. */
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

/* Supports the fat raw directory empty operation. */
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
	uint32_t limit = fat->cluster_count * (uint32_t)fat->sectors_per_cluster * FAT16_ENTRIES_PER_SECTOR;
	uint32_t index;

	/* Process each remaining element. */
	for (index = 0; index < limit; index++) {
		result = fat_raw_directory_entry(filesystem, &directory, index, &lba, &offset, &raw);
		(void)lba;
		(void)offset;
		if (result == ENOENT)
			return 0;
		if (result != 0)
			return result;

		/* Handles the raw condition. */
		if (raw[0] == 0)
			return 0;

		/* Handles the raw condition. */
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

/* Supports the fat raw mkdir operation. */
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

	/* Handles the created cluster availability. */
	if (created_cluster == NULL)
		return EINVAL;

	*created_cluster = 0;

	/* Handles the filesystem condition. */
	if (filesystem->read_only)
		return EROFS;

	/* Checks the operation result. */
	result = fat_raw_resolve_parent(filesystem, path, &parent, &component);
	if (result != 0)
		return result;

	/* Checks the operation result. */
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

	/* Checks the operation result. */
	if (result != 0) {
		int rollback;

		/*
		 * A failed entry rollback may have left a reachable reference
		 * to this cluster.  In that state leaking it is safer than
		 * freeing storage which an on-disk directory may still name.
		 */
		if (filesystem->read_only)
			return result;

		/* Handles the rollback condition. */
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

/* Supports the fat raw remove operation. */
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

	/* Handles the filesystem condition. */
	if (filesystem->read_only)
		return EROFS;

	/* Walks to the directory the last component lives in. */
	result = fat_raw_resolve_parent(filesystem,
					path,
					&parent,
					&component);
	if (result != 0)
		return result;

	/* Checks the operation result. */
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

	/* Checks the operation result. */
	result = fat_engine_read_sector_result(filesystem, lba, &sector);
	if (result != 0)
		return result;

	copy_bytes(raw, sector + offset, sizeof(raw));

	/* Handles the directory condition. */
	if (directory != ((raw[11] & 0x10U) != 0)) {
		/* Asked for a directory and found a file. */
		if (directory)
			return EINVAL;	/* Failed. */

		/* Asked for a file and found a directory. */
		return EISDIR;	/* Failed. */
	}

	/* Handles the directory condition. */
	cluster = fat_raw_dir_cluster(filesystem, raw);
	if (directory) {
		/* Checks the fat raw valid cluster result. */
		if (!fat_raw_valid_cluster(filesystem, cluster))
			return EIO;

		/* Checks the operation result. */
		result = fat_raw_directory_empty(filesystem, cluster);
		if (result != 0)
			return result;
	}

	/* Obtains the fat raw delete location result. */
	error = fat_raw_delete_location(filesystem, &parent, lba, offset);

	/* Returns the computed result. */
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

/* Supports the fat raw update dotdot operation. */
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

	/* Checks the operation result. */
	result = fat_raw_directory_entry(filesystem,
					 &directory,
					 1,
					 &lba,
					 &offset,
					 &raw);
	if (result != 0)
		return result;

	copy_bytes(saved, raw, sizeof(saved));

	/* Checks the operation result. */
	result = fat_engine_write_sector_result(filesystem, lba, &sector);
	if (result != 0)
		return result;

	fat_raw_put_dir_cluster(filesystem, sector + offset, parent_cluster);

	/* Checks the operation result. */
	result = fat_engine_mark_sector_dirty(filesystem);
	if (result == 0)
		result = fat_engine_flush(filesystem);
	if (result == 0)
		return 0;

	/* Handles the rollback condition. */
	rollback = fat_raw_restore_directory_entry(filesystem, lba, offset, saved);
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

/* Supports the fat raw rename rollback destination operation. */
static FAT_MUTATION void
fat_raw_rename_rollback_destination(
	struct fat_mount_state *filesystem,
	const struct fat_directory *parent,
	uint32_t lba,
	uint16_t offset,
	int replacing,
	const uint8_t target[32])
{
	/* Handles the replacing condition. */
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

/* Supports the fat raw canonical basename operation. */
static int
fat_raw_canonical_basename(
	struct fat_mount_state *filesystem,
	const char *path,
	char basename[ZEDBSD_PATH_MAX])
{
	struct fat_dir_entry decoded;
	struct fat_directory parent;
	struct fat_component component;
	uint8_t raw[32];
	int result;

	/* Handles the filesystem availability. */
	if (filesystem == NULL || path == NULL || basename == NULL)
		return EINVAL;

	/* Checks the operation result. */
	result = fat_raw_resolve_parent(filesystem, path, &parent, &component);
	if (result != 0)
		return result;

	/* Handles the filesystem condition. */
	if (filesystem->type == ZEDBSD_FAT32) {
		text_copy(basename, component.text, ZEDBSD_PATH_MAX);

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
	text_copy(basename, decoded.name, ZEDBSD_PATH_MAX);

	/* Succeeded. */
	return 0;
}

/* Supports the fat raw rename operation. */
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

	/* Handles the filesystem condition. */
	if (filesystem->read_only)
		return EROFS;

	/* Checks the operation result. */
	result = fat_raw_resolve_parent(filesystem,
					old_path,
					&old_parent,
					&old_component);
	if (result != 0)
		return result;

	/* Checks the operation result. */
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

	/* Checks the operation result. */
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

	/* Checks the operation result. */
	result = fat_raw_resolve_parent(filesystem,
					new_path,
					&new_parent,
					&new_component);
	if (result != 0)
		return result;

	/* Handles the target result condition. */
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
		/* Handles the old lba condition. */
		if (old_lba == new_lba && old_offset == new_offset) {
			/* Handles the renamed availability. */
			if (renamed != NULL) {
				renamed->lba = old_lba;
				renamed->offset = old_offset;
				renamed->attributes = source[11];
			}

			/* Succeeded. */
			return 0;
		}

		/* Checks the operation result. */
		result = fat_engine_read_sector_result(filesystem, new_lba, &sector);
		if (result != 0)
			return result;

		copy_bytes(target, sector + new_offset, sizeof(target));

		/* A directory may only replace a directory, and a file a file. */
		if (((source[11] ^ target[11]) & 0x10U) != 0)
			return EINVAL;

		/* A directory being replaced has to be empty first. */
		if ((target[11] & 0x10U) != 0) {
			/* Checks the operation result. */
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
					fat_raw_dir_cluster(filesystem, source));

		put32(write_sector + new_offset + 28, fat_engine_get32(source + 28));

		/* Checks the operation result. */
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
		/* Returns the computed result. */
		return target_result;
	}

	/* Handles the replacing condition. */
	source_cluster = fat_raw_dir_cluster(filesystem, source);

	if (!replacing) {
		/* Checks the operation result. */
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

	/* Handles the source condition. */
	if ((source[11] & 0x10U) != 0 &&
	    old_parent.first_cluster != new_parent.first_cluster) {
		/* Checks the operation result. */
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

	/* Checks the operation result. */
	result = fat_raw_delete_location(filesystem,
					 &old_parent,
					 old_lba,
					 old_offset);
	if (result != 0) {
		/* Handles the source condition. */
		if ((source[11] & 0x10U) != 0 &&
		    old_parent.first_cluster != new_parent.first_cluster) {
			(void)fat_raw_update_dotdot(filesystem,
						    source_cluster,
						    old_parent.first_cluster);
		}

		/* Undoes the destination entry, or reports where the new one landed. */
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

/* Supports the fat raw read operation. */
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

	/* Obtains the fat engine read chain result. */
	error = fat_engine_read_chain(
		file, offset, buffer, length, progress, progress_context,
		fat_raw_next_cluster, fat_raw_reserved_limit(file->mount));

	/* Returns the computed result. */
	return error;
}

/* Supports the fat raw readdir operation. */
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

	/* The search starts at the root of the volume. */
	directory.first_cluster = fat_raw_root_cluster(filesystem);

	/* Handles the path condition. */
	if (*path && !(path[0] == '/' && !path[1])) {
		/* Handles the result local condition. */
		error = fat_raw_resolve_entry(filesystem,
						     path,
						     &parent_lba,
						     &parent_offset,
						     &parent_raw,
						     FAT_NAME_EXACT,
						     0);
		if (error != 0)
			return error;

		/* Handles the raw local condition. */
		if (!(parent_raw[11] & 0x10U))
			return EINVAL;

		directory.first_cluster = fat_raw_dir_cluster(fat, parent_raw);

		/* Checks the fat raw valid cluster result. */
		if (!fat_raw_valid_cluster(fat, directory.first_cluster))
			return EIO;
	}

	/* Bounds the walk and starts with no long name assembled. */
	limit = directory.first_cluster == 0
			? fat->root_entries
			: fat->cluster_count *
				  (uint32_t)fat->sectors_per_cluster *
				  FAT16_ENTRIES_PER_SECTOR;
	fat_lfn_reset(&lfn);

	/* Process each remaining element. */
	for (index = 0; index < limit; index++) {
		/* Handles the result local4 condition. */
		error = fat_raw_directory_entry(filesystem,
							&directory,
							index,
							&lba,
							&offset,
							&raw);
		if (error == ENOENT)
			return error;

		/* Handles the result local4 condition. */
		if (error != 0)
			return error;

		/* Handles the raw local3 condition. */
		if (!raw[0]) {
			fat_lfn_reset(&lfn);

			/* Failed. */
			return ENOENT;
		}

		/* Handles the raw local3 condition. */
		if (raw[0] == 0xe5) {
			fat_lfn_reset(&lfn);
			continue;
		}

		/* Handles the raw local3 condition. */
		if (raw[11] == 0x0f) {
			/* Handles the fat condition. */
			if (fat->type == ZEDBSD_FAT32)
				(void)fat_lfn_feed(&lfn, raw);
			continue;
		}

		/* Handles the raw local3 condition. */
		if ((raw[11] & 0x08U) || raw[0] == '.') {
			fat_lfn_reset(&lfn);
			continue;
		}

		/* Handles the visible condition. */
		if (visible++ != wanted) {
			fat_lfn_reset(&lfn);
			continue;
		}

		/* A long name is only available on FAT32, and only if complete. */
		decoded = 0;
		if (fat->type == ZEDBSD_FAT32) {
			decoded = fat_lfn_finish(&lfn, raw, entry->name,
						 sizeof(entry->name));
		}

		/*
		 * Without one, the short name stands in.  FAT32 keeps its
		 * case bits, so it is decoded preserving them; the narrower
		 * widths have none and decode to lower case.
		 */
		if (!decoded && fat->type == ZEDBSD_FAT32) {
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

/* Supports the fat stat location mode operation. */
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
	char found_name[ZEDBSD_PATH_MAX];
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

	/* Checks the operation result. */
	result = fat_raw_resolve_entry(filesystem,
				       path,
				       lba,
				       offset,
				       &raw,
				       match,
				       found_name);
	if (result != 0)
		return result;

	/* Handles the filesystem condition. */
	if (filesystem->type == ZEDBSD_FAT32) {
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

/* Supports the fat engine stat location operation. */
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

	/* Obtains the fat stat location mode result. */
	error = fat_stat_location_mode(filesystem,
						 path,
						 entry,
						 lba,
						 offset,
						 first_cluster,
						 attributes,
						 FAT_NAME_EXACT);

	/* Returns the computed result. */
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
	if (filesystem == 0 || filesystem->type != ZEDBSD_FAT32)
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

/* Supports the fat engine file extents operation. */
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

	/* Handles the file availability. */
	if (file == NULL || callback == NULL || file->mount == NULL)
		return EINVAL;
	filesystem = file->mount;
	fat = filesystem;
	state = file;

	/* Handles the fat condition. */
	if (fat->type != ZEDBSD_FAT12 &&
	    fat->type != ZEDBSD_FAT16 &&
	    fat->type != ZEDBSD_FAT32) {
		/* Failed. */
		return EIO;
	}

	/* Handles the remaining condition. */
	remaining = file->size;
	if (remaining == 0) {
		/* Succeeded: an empty file with no chain has no extents. */
		if (state->first_cluster == 0)
			return 0;

		/* A file with a chain but no length is corrupt. */
		return EIO;	/* Failed. */
	}

	/* Checks the fat raw valid cluster result. */
	cluster = state->first_cluster;
	if (!fat_raw_valid_cluster(fat, cluster))
		return EIO;

	/* Process each remaining element. */
	for (steps = 0; steps < fat->cluster_count && remaining != 0; steps++) {
		/* Handles the uint64 t condition. */
		blocks = fat->sectors_per_cluster;
		if ((uint64_t)blocks * 512U > remaining)
			blocks = (uint32_t)((remaining + 511U) / 512U);

		/* Checks the operation result. */
		result = fat_engine_cluster_lba(filesystem, cluster, 0, &disk_block);
		if (result != 0)
			return result;

		/* Handles the run count condition. */
		if (run_count != 0 && run_disk + run_count == disk_block &&
		    run_file + run_count == file_block) {
			run_count += blocks;
		} else {
			/* Handles the run count condition. */
			if (run_count != 0) {
				/* Checks the operation result. */
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
		remaining -= remaining > (uint64_t)blocks * 512U
				     ? (uint64_t)blocks * 512U
				     : remaining;

		/* Checks the operation result. */
		result = fat_raw_next_cluster(filesystem, cluster, &next);
		if (result != 0)
			return result;

		/* Handles the remaining condition. */
		if (remaining == 0) {
			/* Checks the fat raw is end result. */
			if (!fat_raw_is_end(fat, next))
				return EIO;
			break;
		}

		/* Checks the fat raw valid cluster result. */
		if (!fat_raw_valid_cluster(fat, next))
			return EIO;

		cluster = next;
	}

	/* Handles the remaining condition. */
	if (remaining != 0 || run_count == 0)
		return EIO;

	/* Reports the run that was still open when the chain ended. */
	error = callback(run_file, run_disk, run_count, context);
	if (error != 0)
		return error;

	/* Succeeded: every extent of the file has been reported. */
	return 0;
}

/* Supports the fat engine discard chain result operation. */
static int
fat_engine_discard_chain_result(
	struct fat_mount_state *filesystem,
	uint32_t first_cluster)
{
	int error;

	/* Handles the filesystem availability. */
	if (filesystem == NULL)
		return EINVAL;

	/* Obtains the fat raw free chain result. */
	error = fat_raw_free_chain(filesystem, first_cluster);

	/* Returns the computed result. */
	return error;
}

/* Supports the fat12 mount operation. */
static int
fat12_mount(
	struct fat_mount_state *filesystem)
{
	struct fat_mount_state *fat;
	int result;
	uint32_t fat_entries;

	/* Checks the operation result. */
	result = fat_engine_mount(filesystem, ZEDBSD_FAT12);
	if (result != 0)
		return result;

	/* Handles the fat condition. */
	fat = filesystem;
	if (!fat->root_entries || !fat->fat_sectors ||
	    fat->fat_sectors > 0xffffffffU / 512U) {
		/* Failed. */
		return EIO;
	}

	/* Handles the fat entries condition. */
	fat_entries = fat->fat_sectors * 512U / 3U * 2U;
	if (fat_entries < fat->cluster_count + 2U ||
	    fat->cluster_count + 2U >= FAT12_RESERVED_CLUSTER) {
		/* Failed. */
		return EIO;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the fat32 mount operation. */
static int
fat32_mount(
	struct fat_mount_state *filesystem)
{
	int error;
	struct fat_mount_state *fat;
	int result;
	uint32_t fat_entries;

	/* Checks the operation result. */
	result = fat_engine_mount(filesystem, ZEDBSD_FAT32);
	if (result != 0)
		return result;

	/* Handles the fat condition. */
	fat = filesystem;
	if (!fat->fat32_layout || !fat->fat_sectors ||
	    fat->fat_sectors > 0xffffffffU / 512U) {
		/* Failed. */
		return EIO;
	}

	/* Handles the fat entries condition. */
	fat_entries = fat->fat_sectors * 512U / 4U;
	if (fat_entries < fat->cluster_count + 2U ||
	    fat->cluster_count + 2U >= FAT32_RESERVED_CLUSTER) {
		/* Failed. */
		return EIO;
	}

	/* Computes the function result. */
	error = fat_raw_valid_cluster(fat, fat->root_cluster) ? 0 : EIO;

	/* Returns the computed result. */
	return error;
}

/* Supports the fat mount state operation. */
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

/* Supports the fat metadata number operation. */
static int
fat_metadata_number(
	const char *text,
	unsigned base,
	uint32_t *value)
{
	unsigned digit;
	uint32_t result = 0;

	/* Validates the current text. */
	if (*text == '\0')
		return EINVAL;

	/* Continue while the operation condition remains true. */
	while (*text != '\0') {
		/* Handles the digit condition. */
		digit = (unsigned)(*text++ - '0');
		if (digit >= base || result > (UINT32_MAX - digit) / base)
			return EINVAL;

		result = result * base + digit;
	}

	*value = result;

	/* Succeeded. */
	return 0;
}

/* Supports the fat metadata load operation. */
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

	/* Checks the fat raw open result. */
	if (fat_raw_open(state, "etc/unixmode", &file) != 0)
		return;

	/* Reads as much of the file as the buffer can hold and terminate. */
	if (file.size < sizeof(buffer) - 1U)
		length = (uint32_t)file.size;
	else
		length = (uint32_t)sizeof(buffer) - 1U;

	if (fat_raw_read(&file, 0, buffer, length, NULL, NULL) != 0)
		return;

	buffer[length] = '\0';

	/* Process each remaining element. */
	while (offset < length && state->metadata != NULL &&
	       state->metadata->count < FAT_METADATA_MAX) {
		metadata = &state->metadata->entries[state->metadata->count];
		line = buffer + offset;

		/* Handles the end availability. */
		end = strchr(line, '\n');
		if (end != NULL)
			*end = '\0';

		offset += (uint32_t)strlen(line) + (end != NULL ? 1U : 0U);

		/* Handles the mode availability. */
		mode = strchr(line, ':');
		if (mode == NULL)
			continue;
		*mode++ = '\0';

		/* Handles the uid availability. */
		uid = strchr(mode, ':');
		if (uid == NULL)
			continue;
		*uid++ = '\0';

		/* Handles the gid availability. */
		gid = strchr(uid, ':');
		if (gid == NULL)
			continue;
		*gid++ = '\0';

		/* Skips a line that carries a field this format does not have. */
		if (strchr(gid, ':') != NULL)
			continue;

		/* Skips a path written as absolute; these are mount-relative. */
		if (line[0] == '/')
			continue;

		/* Skips an empty path, which names nothing. */
		if (line[0] == '\0')
			continue;

		/* Skips a path too long for the table to hold. */
		if (strlen(line) >= sizeof(metadata->path))
			continue;

		/* Skips a line whose mode is not an octal number. */
		if (fat_metadata_number(mode, 8, &mode_value) != 0)
			continue;

		/* Skips a line whose owner is not a decimal number. */
		if (fat_metadata_number(uid, 10, &uid_value) != 0)
			continue;
		if (fat_metadata_number(gid, 10, &gid_value) != 0)
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

/* Supports the fat metadata find operation. */
static const struct fat_metadata *
fat_metadata_find(
	const struct fat_mount_state *state,
	const char *path)
{
	unsigned i;

	/* Process each remaining element. */
	for (i = 0;
	     state != NULL && state->metadata != NULL && i < state->metadata->count;
	     i++) {
		/* Selects the matching value. */
		if (!strcmp(state->metadata->entries[i].path, path))
			return &state->metadata->entries[i];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the fat metadata apply operation. */
static void
fat_metadata_apply(
	struct mount *mountp,
	const char *path,
	struct inode *inode)
{
	const struct fat_metadata *metadata = fat_metadata_find(fat_mount_state(mountp), path);

	/* Handles the metadata availability. */
	if (metadata == NULL)
		return;

	inode->i_mode = (inode->i_mode & S_IFMT) | metadata->mode;
	inode->i_uid = metadata->uid;
	inode->i_gid = metadata->gid;
}

/* Supports the fat slot operation. */
static struct fat_inode_slot *
fat_slot(
	struct inode *inode)
{
	unsigned i;

	/* Process each element required by the operation. */
	for (i = 0; i < FAT_INODE_MAX; i++) {
		/* Handles the fat inodes condition. */
		if (&fat_inodes[i].info.fi_inode == inode)
			return &fat_inodes[i];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the fat path operation. */
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

/* Supports the fat alloc inode operation. */
static struct inode *
fat_alloc_inode(
	struct mount *mountp)
{
	unsigned i;
	unsigned long irq;

	(void)mountp;

	irq = spin_lock_irqsave(&fat_pool_lock);

	/* Process each element required by the operation. */
	for (i = 0; i < FAT_INODE_MAX; i++) {
		/* Handles the fat inodes condition. */
		if (!fat_inodes[i].used) {
			fat_inodes[i].used = 1;
			memset(&fat_inodes[i].info, 0, sizeof(fat_inodes[i].info));
			fat_inodes[i].path[0] = '\0';

			spin_unlock_irqrestore(&fat_pool_lock, irq);

			/* Returns the computed result. */
			return &fat_inodes[i].info.fi_inode;
		}
	}

	spin_unlock_irqrestore(&fat_pool_lock, irq);

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the fat free inode operation. */
static void
fat_free_inode(
	struct inode *inode)
{
	struct fat_inode_slot *slot = fat_slot(inode);
	unsigned long irq;

	/* Handles the slot availability. */
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
	char output[ZEDBSD_PATH_MAX])
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
	    ZEDBSD_PATH_MAX)
		return ENAMETOOLONG;

	/* Copies the parent, separating it from the component unless it is root. */
	memcpy(output, parent, parent_length);
	if (parent_length != 0)
		output[parent_length++] = '/';

	/* Appends the component and terminates the result. */
	memcpy(output + parent_length, name->cn_nameptr, name->cn_namelen);
	output[parent_length + name->cn_namelen] = '\0';

	/* Succeeded: the caller now holds the full path. */
	return 0;
}

/* Supports the fat creation collision operation. */
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

	/* Handles the state availability. */
	if (state == NULL || path == NULL)
		return EINVAL;

	/* Checks the operation status. */
	error = fat_raw_resolve_parent(state, path, &parent, &component);
	if (error != 0)
		return error;

	/* Checks the operation status. */
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

	/* Handles the state condition. */
	if (state->type != ZEDBSD_FAT32)
		return 0;

	/* Checks the operation status. */
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

/* Supports the fat creation representation operation. */
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

	/* Handles the metadata availability. */
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

/* Supports the fat ino operation. */
static ino_t
fat_ino(
	uint32_t lba,
	uint16_t offset)
{
	/* Returns the computed result. */
	return 2U + (ino_t)lba * 16U + offset / 32U;
}

/* Supports the fat leap year operation. */
static int
fat_leap_year(
	int year)
{
	/* A year not divisible by four is never a leap year. */
	if ((year % 4) != 0)
		return 0;

	/* A century is a leap year only when it is divisible by four hundred. */
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

	/* February gains a day in a leap year. */
	if (month == 2 && fat_leap_year(year))
		return 29;

	/* Every other month has a fixed length. */
	return days[month - 1];
}

/* Supports the fat decode time operation. */
static time_t
fat_decode_time(
	uint16_t date,
	uint16_t time)
{
	int y_for;
	int m_for;
	int year, month, day, days = 0;
	int64_t seconds;

	/* Handles the date condition. */
	if (date == 0)
		return 0;

	year = 1980 + ((date >> 9) & 0x7f);
	month = (date >> 5) & 0x0f;

	/* Checks the fat month days result. */
	day = date & 0x1f;
	if (month < 1 ||
	    month > 12 ||
	    day < 1 ||
	    day > fat_month_days(year, month)) {
		/* Succeeded. */
		return 0;
	}

	/* Process each element required by the operation. */
	for (y_for = 1970; y_for < year; y_for++)
		days += fat_leap_year(y_for) ? 366 : 365;

	/* Process each element required by the operation. */
	for (m_for = 1; m_for < month; m_for++)
		days += fat_month_days(year, m_for);

	days += day - 1;

	seconds = (int64_t)days * 86400 + ((time >> 11) & 0x1f) * 3600 +
		  ((time >> 5) & 0x3f) * 60 + (time & 0x1f) * 2;

#ifdef ZEDBSD_USER_ABI_LP64
	/* Returns the computed result. */
	return (time_t)seconds;
#else
	/* A date beyond the epoch this kernel represents saturates. */
	if (seconds > INT32_MAX)
		return (time_t)INT32_MAX;

	/* Reports the decoded time. */
	return (time_t)seconds;
#endif
}

/* Supports the fat encode time operation. */
static FAT_MUTATION int
fat_encode_time(
	time_t seconds,
	uint16_t *date,
	uint16_t *time)
{
	int64_t days, remainder;
	int year = 1970, month = 1;

	/* Handles the seconds condition. */
	if (seconds < FAT_EPOCH_1980)
		return EOVERFLOW;

	days = seconds / 86400;
	remainder = seconds % 86400;

	/* Continue while the operation condition remains true. */
	while (days >= (fat_leap_year(year) ? 366 : 365)) {
		days -= fat_leap_year(year) ? 366 : 365;
		year++;
	}

	/* Handles the year condition. */
	if (year > 2107)
		return EOVERFLOW;

	/* Continue while the operation condition remains true. */
	while (days >= fat_month_days(year, month)) {
		days -= fat_month_days(year, month);
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

/* Supports the fat make inode operation. */
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

	/* Checks the operation status. */
	if (error == 0)
		return 0;

	/* Handles the inode availability. */
	inode = inode_alloc(mountp);
	if (inode == NULL)
		return ENOSPC;

	info = fat_inode(inode);

	/* Checks the strlen result. */
	slot = fat_slot(inode);
	if (slot == NULL || strlen(path) >= ZEDBSD_PATH_MAX) {
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

	/* Handles the attributes condition. */
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

	/* Handles the attributes condition. */
	if (attributes & FAT_ATTRIBUTE_READ_ONLY)
		inode->i_mode &= ~(mode_t)0222U;

	fat_metadata_apply(mountp, path, inode);
	fat_load_inode_times(mountp, inode, lba, offset);

	*result = inode;

	/* Succeeded. */
	return 0;
}

/* Supports the set inode ops operation. */
static void
set_inode_ops(
	struct inode *inode)
{
	inode->i_op = &fat_inode_ops;
	inode->i_fop = inode->i_type == INODE_DIR ? &fat_directory_ops : &fat_regular_ops;
}

/* Supports the fat stat path operation. */
static int
fat_stat_path(
	struct mount *mountp,
	const char *path,
	struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	struct fat_dir_entry entry;
	char canonical[ZEDBSD_PATH_MAX];
	const char *slash;
	size_t prefix_length;
	uint32_t lba, first_cluster;
	uint16_t offset;
	uint8_t attributes;
	int error;
	int fsresult;

	/* Looks the name up exactly as it was given. */
	fsresult = fat_engine_stat_location(state,
					    path,
					    &entry,
					    &lba,
					    &offset,
					    &first_cluster,
					    &attributes);

	/* Handles the fsresult condition. */
	if (fsresult != 0)
		return fsresult;

	slash = strrchr(path, '/');

	/* Checks the strlen result. */
	prefix_length = slash != NULL ? (size_t)(slash - path + 1) : 0;
	if (prefix_length + strlen(entry.name) >= sizeof(canonical))
		return ENAMETOOLONG;

	memcpy(canonical, path, prefix_length);
	strcpy(canonical + prefix_length, entry.name);

	/* Checks the operation status. */
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

/* Supports the fat stat path casefold operation. */
static int
fat_stat_path_casefold(
	struct mount *mountp,
	const char *path,
	struct inode **result)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	struct fat_dir_entry entry;
	char canonical[ZEDBSD_PATH_MAX];
	const char *slash;
	size_t prefix_length;
	uint32_t lba, first_cluster;
	uint16_t offset;
	uint8_t attributes;
	int error;
	int fsresult;

	/* Looks the name up without regard to case. */
	fsresult = fat_engine_stat_location_casefold(state,
						     path,
						     &entry,
						     &lba,
						     &offset,
						     &first_cluster,
						     &attributes);

	/* Handles the fsresult condition. */
	if (fsresult != 0)
		return fsresult;
	slash = strrchr(path, '/');

	/* Checks the strlen result. */
	prefix_length = slash != NULL ? (size_t)(slash - path + 1) : 0;
	if (prefix_length + strlen(entry.name) >= sizeof(canonical))
		return ENAMETOOLONG;

	memcpy(canonical, path, prefix_length);
	strcpy(canonical + prefix_length, entry.name);

	/* Checks the operation status. */
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

/* Supports the fat lookup unlocked operation. */
static int
fat_lookup_unlocked(
	struct inode *directory,
	const struct componentname *name,
	struct inode **result)
{
	int function_result;
	char *slash;
	char path[ZEDBSD_PATH_MAX];
	const char *parent = fat_path(directory);
	int error;

	/* Handles the parent availability. */
	if (parent == NULL)
		return EIO;

	/* Validates the current name. */
	if (name->cn_namelen == 1 && name->cn_nameptr[0] == '.') {
		inode_ref(directory);
		*result = directory;

		/* Succeeded. */
		return 0;
	}

	/* Validates the current name. */
	if (name->cn_namelen == 2 && name->cn_nameptr[0] == '.' &&
	    name->cn_nameptr[1] == '.') {
		/* Handles the parent condition. */
		if (parent[0] == '\0') {
			inode_ref(directory);
			*result = directory;

			/* Succeeded. */
			return 0;
		}

		strcpy(path, parent);

		/* Handles the slash availability. */
		slash = strrchr(path, '/');
		if (slash == NULL) {
			inode_ref(directory->i_mount->m_root);
			*result = directory->i_mount->m_root;

			/* Succeeded. */
			return 0;
		}

		*slash = '\0';

		/* Obtains the fat stat path result. */
		function_result = fat_stat_path(directory->i_mount, path, result);

		/* Returns the computed result. */
		return function_result;
	}

	error = join_path(parent, name, path);

	/* Computes the function result. */
	function_result = error != 0 ?
		error :
		fat_stat_path(directory->i_mount, path, result);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the fat lookup operation. */
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

/* Supports the fat lookup casefold unlocked operation. */
static int
fat_lookup_casefold_unlocked(
	struct inode *directory,
	const struct componentname *name,
	struct inode **result)
{
	int function_result;
	char path[ZEDBSD_PATH_MAX];
	const char *parent = fat_path(directory);
	int error;

	/* Handles the parent availability. */
	if (parent == NULL)
		return EIO;

	error = join_path(parent, name, path);

	/* Computes the function result. */
	function_result = error != 0 ?
		error :
		fat_stat_path_casefold(directory->i_mount, path, result);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the fat getattr operation. */
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
	status->st_blocks = inode->i_size > 0 ?
		(blkcnt_t)(((uint64_t)inode->i_size + 511U) / 512U) :
		0;

	/* Succeeded. */
	return 0;
}

/* Supports the fat put16 operation. */
static FAT_MUTATION void
fat_put16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/* Supports the fat setattr unlocked operation. */
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

	/* Handles the state availability. */
	if (state == NULL || info == NULL || (inode->i_flags & INODE_ROOT) != 0)
		return EOPNOTSUPP;

	/* Handles the mask condition. */
	if ((mask & INODE_ATTR_SIZE) != 0)
		return EOPNOTSUPP;

	/* Handles the mask condition. */
	if ((mask & INODE_ATTR_UID) != 0 && status->st_uid != inode->i_uid)
		return EOPNOTSUPP;

	/* Handles the mask condition. */
	if ((mask & INODE_ATTR_GID) != 0 && status->st_gid != inode->i_gid)
		return EOPNOTSUPP;

	/* Handles the mask condition. */
	if (mask & INODE_ATTR_MODE) {
		/* Handles the permissions condition. */
		permissions = status->st_mode & 07777U;
		if (permissions != 0755U && permissions != 0555U)
			return EOPNOTSUPP;
	}

	/* Handles the mask condition. */
	if (mask & INODE_ATTR_ATIME) {
		/* Checks the operation status. */
		if (status->st_atim.tv_nsec < 0 ||
		    status->st_atim.tv_nsec >= 1000000000L) {
			/* Failed. */
			return EINVAL;
		}

		/* Checks the operation status. */
		error = fat_encode_time(status->st_atim.tv_sec, &atime_date,
					&atime_time);
		if (error != 0)
			return error;
	}

	/* Handles the mask condition. */
	if (mask & INODE_ATTR_MTIME) {
		/* Checks the operation status. */
		if (status->st_mtim.tv_nsec < 0 ||
		    status->st_mtim.tv_nsec >= 1000000000L) {
			/* Failed. */
			return EINVAL;
		}

		/* Checks the operation status. */
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

	/* Checks the operation result. */
	result = fat_engine_write_sector_result(state, info->fi_dirent_lba, &sector);
	if (result != 0)
		return result;

	memcpy(saved, sector + info->fi_dirent_offset, sizeof(saved));
	sector += info->fi_dirent_offset;

	/* Handles the mask condition. */
	if (mask & INODE_ATTR_MODE) {
		/* Checks the operation status. */
		if ((status->st_mode & 0222U) == 0)
			sector[11] |= FAT_ATTRIBUTE_READ_ONLY;
		else
			sector[11] &= (uint8_t)~FAT_ATTRIBUTE_READ_ONLY;
	}

	/* Handles the mask condition. */
	if (mask & INODE_ATTR_ATIME)
		fat_put16(sector + 18, atime_date);

	/* Handles the mask condition. */
	if (mask & INODE_ATTR_MTIME) {
		fat_put16(sector + 22, mtime_time);
		fat_put16(sector + 24, mtime_date);
	}

	/* Checks the operation result. */
	result = fat_engine_mark_sector_dirty(state);
	if (result == 0)
		result = fat_engine_flush(state);
	if (result != 0) {
		/* Checks the fat engine write sector result result. */
		if (fat_engine_write_sector_result(state, info->fi_dirent_lba,
						   &rollback) == 0) {
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

/* Supports the fat setattr operation. */
static FAT_MUTATION int
fat_setattr(
	struct inode *inode,
	const struct stat *status,
	unsigned mask)
{
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	int error;

	mutex_lock(&state->lock);

	/* Checks the operation status. */
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

/* Supports the fat file get operation. */
static struct fat_file_state *
fat_file_get(
	struct file *file)
{
	struct fat_file_state *slot = NULL;
	struct fat_mount_state *mount_state;
	unsigned i;
	unsigned long irq;

	/* Handles the f data availability. */
	if (file->f_data != NULL)
		return file->f_data;
	irq = spin_lock_irqsave(&fat_pool_lock);

	/* Process each element required by the operation. */
	for (i = 0; i < FAT_FILE_MAX; i++) {
		/* Handles the fat files condition. */
		if (!fat_files[i].used) {
			memset(&fat_files[i], 0, sizeof(fat_files[i]));
			fat_files[i].used = 1;
			fat_files[i].owner = file->f_inode;
			slot = &fat_files[i];
			break;
		}
	}

	spin_unlock_irqrestore(&fat_pool_lock, irq);

	/* Handles the slot availability. */
	if (slot == NULL)
		return NULL;

	/* Checks the fat raw open result. */
	mount_state = fat_mount_state(file->f_inode->i_mount);
	if (fat_raw_open(mount_state, fat_path(file->f_inode), slot) != 0) {
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

	/* Returns the computed result. */
	return slot;
}

/* Supports the fat open file operation. */
static int
fat_open_file(
	struct file *file)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	int error;

	mutex_lock(&state->lock);

	error = fat_file_get(file) != NULL ? 0 : EIO;

	mutex_unlock(&state->lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Called under the FAT mount lock and the ordinary file/VM I/O lease. Mapped operations never resize or publish allocation metadata. */
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

	/* Handles the writing condition. */
	if (writing && mount->read_only)
		return -EROFS;

	/*
	 * Drain before invalidating; never discard an earlier failed dirty
	 * write. The lock excludes all other readers of the single FAT sector
	 * slot.
	 */

	/* Checks the operation status. */
	error = fat_engine_flush(mount);
	if (error != 0)
		return -error;
	fat_engine_invalidate(mount);
	block = (uint64_t)offset / 512U;
	remaining = length / 512U;
	/* Process each remaining element. */
	for (i = 0; i < state->loop_map_count && remaining != 0; i++) {
		/* Handles the block condition. */
		extent = &state->loop_map[i];
		if (block < extent->file_block)
			return -EIO;

		/* Handles the within condition. */
		within = block - extent->file_block;
		if (within >= extent->count)
			continue;

		/* Handles the amount condition. */
		amount = extent->count - within;
		if (amount > remaining)
			amount = remaining;

		/* Handles the writing condition. */
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

		/* Checks the operation status. */
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

/* Supports the fat pread file unlocked operation. */
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

	/* Handles the state availability. */
	if (state == NULL)
		return -EIO;

	/* Checks the current offset. */
	if (offset >= file->f_inode->i_size)
		return 0;

	/* Checks the current data length. */
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

	/* Checks the operation result. */
	result = fat_raw_read(state, (uint64_t)offset, buffer, count, NULL, NULL);
	if (result != 0)
		return -result;

	/* Returns the computed result. */
	return count;
}

/* Supports the fat sync inode state operation. */
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

	/* Handles the inode availability. */
	if (inode == NULL || file == NULL)
		return;

	info = fat_inode(inode);

	state = file;
	info->fi_first_cluster = state->first_cluster;
	inode->i_size = (off_t)file->size;

	irq = spin_lock_irqsave(&fat_pool_lock);

	/* Process each element required by the operation. */
	for (i = 0; i < FAT_FILE_MAX; i++) {
		/* Handles the fat files condition. */
		if (!fat_files[i].used || fat_files[i].owner != inode)
			continue;
		fat_files[i].size = file->size;
		open_state = &fat_files[i];
		open_state->first_cluster = state->first_cluster;
	}

	spin_unlock_irqrestore(&fat_pool_lock, irq);
}

/* Called with the mount mutex held.  A failed close leaves a self-contained directory-entry retry record; it deliberately owns no inode reference so generic unmount busy checks can reach filesystem sync. */
static int
fat_flush_pending_closes(
	struct fat_mount_state *mount_state)
{
	struct fat_file_state *state;
	struct inode *owner;
	unsigned long irq;
	int error;
	unsigned i;

	/* Process each element required by the operation. */
	for (i = 0; i < FAT_FILE_MAX; i++) {
		state = &fat_files[i];

		/* Handles the state condition. */
		irq = spin_lock_irqsave(&fat_pool_lock);
		if (!state->used || state->mount != mount_state ||
		    !state->pending_close) {
			spin_unlock_irqrestore(&fat_pool_lock, irq);
			continue;
		}

		owner = state->owner;
		spin_unlock_irqrestore(&fat_pool_lock, irq);

		/* Checks the operation status. */
		error = fat_raw_flush_file(state);
		if (error != 0)
			return error;

		/* Handles the owner availability. */
		if (owner != NULL)
			fat_sync_inode_state(owner, state);

		irq = spin_lock_irqsave(&fat_pool_lock);
		memset(state, 0, sizeof(*state));
		spin_unlock_irqrestore(&fat_pool_lock, irq);
	}

	/* Succeeded. */
	return 0;
}

/* Supports the fat pread file operation. */
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

	/* Returns the computed result. */
	return count;
}

/* Supports the fat read file operation. */
static ssize_t
fat_read_file(
	struct file *file,
	void *buffer,
	size_t length)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	ssize_t count;

	mutex_lock(&state->lock);

	/* Checks the remaining item count. */
	count = fat_pread_file_unlocked(file, buffer, length, file->f_offset);
	if (count > 0)
		file->f_offset += count;

	mutex_unlock(&state->lock);

	/* Returns the computed result. */
	return count;
}

/* Supports the fat pwrite file unlocked operation. */
static ssize_t
fat_pwrite_file_unlocked(
	struct file *file,
	const void *buffer,
	size_t length,
	off_t offset)
{
	ssize_t function_result;
	struct fat_file_state *state = fat_file_get(file);
	uint32_t count = length > UINT32_MAX ? UINT32_MAX : (uint32_t)length;
	int result;

	/* Handles the state availability. */
	if (state == NULL)
		return -EIO;

	/* Checks the current offset. */
	if (offset < 0)
		return -EINVAL;

	/* Handles the uint64 t condition. */
	if ((uint64_t)offset > UINT32_MAX ||
	    (uint64_t)count > UINT32_MAX - (uint64_t)offset) {
		/* Failed. */
		return -EFBIG;
	}

	/* Handles the loop map availability. */
	if (state->loop_map != NULL && file->f_backing_claim != NULL) {
		/* Obtains the fat loop transfer result. */
		function_result = fat_loop_transfer(file, state, (void *)buffer,
						    length, offset, 1);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the operation result. */
	result = fat_raw_write(state, (uint64_t)offset, buffer, count);
	if (result != 0)
		return -result;

	fat_sync_inode_state(file->f_inode, state);

	/* Returns the computed result. */
	return count;
}

/* Supports the fat pwrite file operation. */
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

	/* Returns the computed result. */
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

	/* Checks the operation status. */
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

	/* Returns the computed result. */
	return count;
}

/* Supports the fat write file operation. */
static ssize_t
fat_write_file(
	struct file *file,
	const void *buffer,
	size_t length)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	off_t offset;
	ssize_t count;

	mutex_lock(&state->lock);

	offset = (file_status_flags_get(file) & O_APPEND) != 0
			 ? file->f_inode->i_size
			 : file->f_offset;

	/* Checks the remaining item count. */
	count = fat_pwrite_file_unlocked(file, buffer, length, offset);
	if (count > 0)
		file->f_offset = offset + count;

	mutex_unlock(&state->lock);

	/* Returns the computed result. */
	return count;
}

/* Supports the fat readdir unlocked operation. */
static int
fat_readdir_unlocked(
	struct file *file,
	struct dirent *entry,
	int *eof)
{
	struct fat_mount_state *state = fat_mount_state(file->f_inode->i_mount);
	struct fat_dir_entry decoded;
	char child_path[ZEDBSD_PATH_MAX];
	struct componentname component;
	struct inode *child;
	int result;

	result = fat_raw_readdir(state,
				 fat_path(file->f_inode),
				 (unsigned)file->f_offset,
				 &decoded);
	if (result == ENOENT) {
		*eof = 1;

		/* Succeeded. */
		return 0;
	}

	/* Checks the operation result. */
	if (result != 0)
		return result;

	component.cn_nameptr = decoded.name;
	component.cn_namelen = strlen(decoded.name);
	component.cn_flags = COMPONENT_LAST;

	/* Checks the join path result. */
	if (join_path(fat_path(file->f_inode), &component, child_path) != 0)
		return ENAMETOOLONG;

	/* Checks the fat stat path result. */
	if (fat_stat_path(file->f_inode->i_mount, child_path, &child) != 0)
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

/* Supports the fat readdir operation. */
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

/* Supports the fat close file operation. */
static int
fat_close_file(
	struct file *file)
{
	struct fat_file_state *state;
	struct fat_mount_state *mount_state;
	unsigned long irq;
	int error;

	/* A file whose inode is already gone has no mount to lock. */
	state = file->f_data;
	mount_state = NULL;
	error = 0;
	if (file->f_inode != NULL)
		mount_state = fat_mount_state(file->f_inode->i_mount);

	/* Holds the mount for as long as the file state is touched. */
	if (mount_state != NULL)
		mutex_lock(&mount_state->lock);

	/* Handles the state availability. */
	if (state != NULL) {
		/* Checks the file status flags get result. */
		if ((file_status_flags_get(file) & O_ACCMODE) != O_RDONLY &&
		    file->f_inode != NULL &&
		    (file->f_inode->i_flags & INODE_DEAD) == 0) {
			/* Checks the operation status. */
			error = fat_raw_flush_file(state);
			if (error == 0)
				fat_sync_inode_state(file->f_inode, state);
		} else if (file->f_inode != NULL &&
			   (file->f_inode->i_flags & INODE_DEAD) != 0) {
			error = fat_engine_flush(fat_mount_state(file->f_inode->i_mount));
		}

		irq = spin_lock_irqsave(&fat_pool_lock);

		/* Checks the operation status. */
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

	/* Handles the mount state availability. */
	if (mount_state != NULL)
		mutex_unlock(&mount_state->lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the fat fsync operation. */
static int
fat_fsync(
	struct file *file)
{
	struct fat_mount_state *mount_state =
		fat_mount_state(file->f_inode->i_mount);
	struct fat_file_state *state;
	int error;

	mutex_lock(&mount_state->lock);

	/* Handles the state availability. */
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

/* Supports the fat lookup casefold operation. */
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

/* Supports the fat truncate operation. */
static int
fat_truncate(
	struct inode *inode,
	off_t size)
{
	struct fat_file_state file = {0};
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);
	int result;

	/* Checks the current data size. */
	if (size < 0)
		return EINVAL;

	/* Handles the uint64 t condition. */
	if ((uint64_t)size > UINT32_MAX)
		return EFBIG;
	mutex_lock(&state->lock);

	/* Checks the operation result. */
	result = fat_flush_pending_closes(state);
	if (result != 0) {
		mutex_unlock(&state->lock);

		/* Failed. */
		return result;
	}

	/* Handles the inode condition. */
	if ((inode->i_flags & INODE_DEAD) != 0) {
		file.mount = state;
		file.owner = inode;
		file.size = (uint64_t)inode->i_size;
		file.first_cluster = fat_inode(inode)->fi_first_cluster;
		result = 0;
	} else {
		result = fat_raw_open(state, fat_path(inode), &file);
	}

	/* Checks the operation result. */
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

	/* Checks the operation result. */
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

/* Supports the fat create unlocked operation. */
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
	char path[ZEDBSD_PATH_MAX];
	int error, rollback;

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
		if (fat_creation_representation(state,
						fat_path(created),
						&mode,
						&uid,
						&gid) == 0) {
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

/* Supports the fat create operation. */
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

	/* Handles the result availability. */
	if (result == NULL)
		return EINVAL;

	*result = NULL;

	mutex_lock(&state->lock);
	error = fat_create_unlocked(directory, name, request, &created);
	mutex_unlock(&state->lock);
	if (error != 0) {
		/* Handles the created availability. */
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

/* Supports the fat orphan operation. */
static FAT_MUTATION void
fat_orphan(
	struct inode *inode)
{
	struct fat_inode_info *info;

	/* Handles the inode availability. */
	if (inode == NULL)
		return;

	info = fat_inode(inode);
	info->fi_flags |= FAT_INODE_ORPHANED;
	inode->i_flags |= INODE_DEAD;
	namecache_purge_inode(inode);
}

/* Supports the fat release orphan operation. */
static FAT_MUTATION void
fat_release_orphan(
	struct inode *inode)
{
	/* Handles the inode availability. */
	if (inode == NULL)
		return;

	/*
	 * inode_release() owns the transition from the final external
	 * reference to cache-only DEAD state and performs
	 * reclaim/free synchronously.
	 */
	inode_release(inode);
}

/* Supports the fat mkdir unlocked operation. */
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
	char path[ZEDBSD_PATH_MAX];
	uint32_t cluster = 0;
	int error, rollback;

	*result = NULL;

	/* Checks the operation status. */
	error = join_path(fat_path(directory), name, path);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = fat_creation_collision(state, path);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = fat_creation_representable(state, path, request, INODE_DIR);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = fat_raw_mkdir(state, path, &cluster);
	if (error != 0)
		return error;

	namecache_remove(directory, name);

	/* Checks the operation status. */
	error = fat_stat_path(directory->i_mount, path, &created);
	if (error != 0)
		goto rollback_raw;

	/* Checks the operation status. */
	error = inode_creation_prepare(directory, created, request);
	if (error == 0) {
		error = fat_created_inode_matches(state,
						  fat_path(created),
						  created);
	}

	/* Checks the operation status. */
	if (error != 0)
		goto rollback_inode;

	*result = created;

	/* Succeeded. */
	return 0;

rollback_inode:

	/* Handles the rollback condition. */
	rollback = fat_raw_rmdir(state, path);
	if (rollback != 0) {
		/* Checks the fat creation representation result. */
		if (fat_creation_representation(state,
						fat_path(created),
						&mode,
						&uid,
						&gid) == 0) {
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

	/* Handles the rollback condition. */
	rollback = fat_raw_free_chain(state, cluster);
	if (rollback != 0) {
		/* Handles the deferred condition. */
		deferred = state->read_only ? rollback
					    : fat_defer_orphan(state, cluster);
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
		if (state->read_only || fat_defer_orphan(state, cluster) != 0)
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

/* Supports the fat mkdir operation. */
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

	/* Handles the result availability. */
	if (result == NULL)
		return EINVAL;

	*result = NULL;

	mutex_lock(&state->lock);
	error = fat_mkdir_unlocked(directory, name, request, &created);
	mutex_unlock(&state->lock);
	if (error != 0) {
		/* Handles the created availability. */
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

/* Supports the fat remove inode unlocked operation. */
static FAT_MUTATION int
fat_remove_inode_unlocked(
	struct inode *directory,
	const struct componentname *name,
	int remove_directory,
	struct inode **orphaned)
{
	struct fat_mount_state *state = fat_mount_state(directory->i_mount);
	struct inode *victim = NULL;
	char path[ZEDBSD_PATH_MAX];
	int error;

	*orphaned = NULL;

	/* Checks the operation status. */
	error = fat_flush_pending_closes(state);
	if (error == 0)
		error = fat_drain_pending_orphans(state);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = join_path(fat_path(directory), name, path);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = fat_lookup_unlocked(directory, name, &victim);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = remove_directory ? fat_raw_rmdir(state, path)
				 : fat_raw_unlink(state, path);
	if (error == 0) {
		namecache_remove(directory, name);
		fat_orphan(victim);
		*orphaned = victim;
	}

	/* Checks the operation status. */
	if (error != 0)
		inode_release(victim);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the fat unlink operation. */
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

	/* Handles the orphaned availability. */
	if (orphaned != NULL)
		fat_release_orphan(orphaned);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the fat rmdir operation. */
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

	/* Handles the orphaned availability. */
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

	/* Nothing lies under an empty parent name. */
	length = strlen(parent);
	if (length == 0)
		return 0;

	/* The parent name has to be a prefix of the path. */
	if (memcmp(parent, path, length) != 0)
		return 0;

	/* That prefix has to end where a path component ends. */
	if (path[length] != '/')
		return 0;

	/* Reports that the path lies under the parent. */
	return 1;
}

/* Supports the fat repath descendants operation. */
static FAT_MUTATION void
fat_repath_descendants(
	struct mount *mountp,
	const char *old_path,
	const char *new_path)
{
	char replacement[ZEDBSD_PATH_MAX];
	size_t suffix;
	size_t old_length;
	size_t new_length;
	unsigned long irq;
	unsigned i;

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
		if (!fat_path_descendant(old_path, fat_inodes[i].path))
			continue;

		/* Rebuilds the path with the new name in front of the suffix. */
		suffix = strlen(fat_inodes[i].path + old_length);
		memcpy(replacement, new_path, new_length);
		memcpy(replacement + new_length, fat_inodes[i].path + old_length, suffix + 1U);
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
		if (!fat_path_descendant(old_path, fat_inodes[i].path))
			continue;

		/* Refuses the rename when this inode's new path would not fit. */
		suffix = strlen(fat_inodes[i].path + old_length);
		if (new_length + suffix >= ZEDBSD_PATH_MAX) {
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

/* Supports the fat rename unlocked operation. */
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
	char old_path[ZEDBSD_PATH_MAX], new_path[ZEDBSD_PATH_MAX];
	char canonical_basename[ZEDBSD_PATH_MAX];
	char old_canonical[ZEDBSD_PATH_MAX], new_canonical[ZEDBSD_PATH_MAX];
	unsigned i;
	unsigned long irq;
	int error, target_error;

	*orphaned = NULL;

	/* Checks the active flags. */
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
	if (fat_path(source) == NULL) {
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

	/* Handles the target availability. */
	if (target != NULL) {
		/* Checks the operation status. */
		error = fat_drain_pending_orphans(state);
		if (error != 0) {
			inode_release(target);
			inode_release(source);

			/* Failed. */
			return error;
		}
	}

	/* Handles the target availability. */
	if (target != NULL) {
		/* Checks the fat path result. */
		if (fat_path(target) == NULL) {
			inode_release(target);
			inode_release(source);

			/* Failed. */
			return EIO;
		}

		strcpy(new_canonical, fat_path(target));
	} else {
		/* Checks the operation status. */
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

		/* Checks the operation status. */
		error = join_path(fat_path(new_directory),
				  &canonical_name,
				  new_canonical);
		if (error != 0) {
			inode_release(source);

			/* Failed. */
			return error;
		}
	}

	/* Handles the source condition. */
	if (source->i_type == INODE_DIR) {
		/* Checks the operation status. */
		error = fat_repath_descendants_possible(old_directory->i_mount,
							old_canonical,
							new_canonical);
		if (error != 0) {
			/* Handles the target availability. */
			if (target != NULL)
				inode_release(target);

			inode_release(source);

			/* Failed. */
			return error;
		}
	}

	info = fat_inode(source);

	/* Handles the source condition. */
	if (source->i_size < 0 ||
	    (uint64_t)source->i_size > UINT32_MAX) {
		/* Handles the target availability. */
		if (target != NULL)
			inode_release(target);

		inode_release(source);

		/* Failed. */
		return EFBIG;
	}

	/* Checks the operation status. */
	error = fat_raw_rename(state,
			       old_path,
			       new_path,
			       info->fi_first_cluster,
			       (uint32_t)source->i_size,
			       &renamed);
	if (error != 0) {
		/* Handles the target availability. */
		if (target != NULL)
			inode_release(target);

		inode_release(source);

		/* Failed. */
		return error;
	}

	/* Handles the target availability. */
	if (target != NULL)
		fat_orphan(target);

	info->fi_dirent_lba = renamed.lba;
	info->fi_dirent_offset = renamed.offset;
	info->fi_attributes = renamed.attributes;
	source->i_ino = fat_ino(renamed.lba, renamed.offset);

	irq = spin_lock_irqsave(&fat_pool_lock);

	/* Process each element required by the operation. */
	for (i = 0; i < FAT_FILE_MAX; i++) {
		/* Handles the fat files condition. */
		if (!fat_files[i].used || fat_files[i].owner != source)
			continue;

		open_state = &fat_files[i];
		open_state->directory_lba = renamed.lba;
		open_state->directory_offset = renamed.offset;
		open_state->directory_dirty = 0;
	}

	spin_unlock_irqrestore(&fat_pool_lock, irq);

	/* Handles the source condition. */
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

	/* Handles the target availability. */
	if (target != NULL)
		*orphaned = target;

	inode_release(source);

	/* Succeede. */
	return 0;
}

/* Supports the fat rename operation. */
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

	/* Handles the orphaned availability. */
	if (orphaned != NULL)
		fat_release_orphan(orphaned);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the fat reclaim unlocked operation. */
static void
fat_reclaim_unlocked(
	struct inode *inode)
{
	int result;
	struct fat_inode_info *info = fat_inode(inode);
	struct fat_mount_state *state = fat_mount_state(inode->i_mount);

	/* Handles the state availability. */
	if ((info->fi_flags & FAT_INODE_ORPHANED) != 0 &&
	    info->fi_first_cluster != 0 && state != NULL) {
		/* Checks the fat defer orphan result. */
		result = fat_engine_discard_chain_result(state, info->fi_first_cluster);
		if (result == 0 ||
		    fat_defer_orphan(state, info->fi_first_cluster) == 0)
			info->fi_first_cluster = 0;
	}
}

/* Supports the fat reclaim operation. */
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

/* Supports the fat probe volume operation. */
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

	/* Handles the disk availability. */
	if (disk == NULL || type == NULL)
		return EOPNOTSUPP;

	/* Checks the operation result. */
	result = parse_bpb(&candidate);
	if (result != 0)
		return result;
	*type = (enum bootfat_type)candidate.type;
	/* Succeeded. */
	return 0;
}

/* Supports the fat hex digit operation. */
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

/* Supports the fat hex32 operation. */
static void
fat_hex32(
	char output[9],
	uint32_t value)
{
	unsigned i;

	/* Process each element required by the operation. */
	for (i = 0; i < 8U; i++)
		output[i] = fat_hex_digit((value >> (28U - i * 4U)) & 15U);

	output[8] = '\0';
}

/* Supports the fat copy label operation. */
static void
fat_copy_label(
	char *output,
	size_t capacity,
	const uint8_t *input,
	size_t length)
{
	size_t end = length;
	size_t i;

	/* Continue while the operation condition remains true. */
	while (end != 0U && (input[end - 1U] == ' ' || input[end - 1U] == 0U))
		end--;

	/* Checks the current endpoint. */
	if (end >= capacity)
		end = capacity - 1U;

	/* Process each element required by the operation. */
	for (i = 0; i < end; i++) {
		output[i] = (input[i] >= 0x20U && input[i] <= 0x7eU) ?
			(char)input[i] :
			'_';
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

	/* The small counts are zero when the value did not fit in sixteen bits. */
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
	identity->flags = ZEDBSD_BLKID_TYPE;

	/* FAT32 moved the extended fields further into the boot sector. */
	if (type == ZEDBSD_FAT32) {
		serial_offset = 67U;
		label_offset = 71U;
	} else {
		serial_offset = 39U;
		label_offset = 43U;
	}

	/* Succeeded: a volume without the extended signature has no more to give. */
	if (boot[serial_offset - 1U] != 0x29U)
		return 0;

	/* Renders the serial number in the two-group form tools expect. */
	serial = fat_engine_get32(boot + serial_offset);
	fat_hex32(identity->uuid, serial);
	memmove(identity->uuid + 5, identity->uuid + 4, 4U);
	identity->uuid[4] = '-';
	identity->uuid[9] = '\0';
	identity->flags |= ZEDBSD_BLKID_UUID;

	/* Reports the volume label, unless it is the placeholder one. */
	fat_copy_label(identity->label,
		       sizeof(identity->label),
		       boot + label_offset,
		       11U);
	if (identity->label[0] != '\0' &&
	    strcmp(identity->label, "NO NAME") != 0)
		identity->flags |= ZEDBSD_BLKID_LABEL;

	/* Succeeded: the caller now holds everything the volume names itself by. */
	return 0;
}

/* Supports the fat probe operation. */
static int
fat_probe(
	struct disk *disk)
{
	int error;
	enum bootfat_type type;

	/* Obtains the drv fat probe type result. */
	error = drv_fat_probe_type(disk, &type);

	/* Returns the computed result. */
	return error;
}

/* Supports the fat mount impl operation. */
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

	/* Process each element required by the operation. */
	for (i = 0; i < FAT_MOUNT_MAX; i++) {
		/* Handles the fat mounts condition. */
		if (!fat_mounts[i].used) {
			state = &fat_mounts[i];
			memset(state, 0, sizeof(*state));
			state->used = 1;
			break;
		}
	}

	spin_unlock_irqrestore(&fat_pool_lock, irq);

	/* Handles the state availability. */
	if (state == NULL)
		return ENOSPC;

	memset(&fat_metadata_tables[i], 0, sizeof(fat_metadata_tables[i]));
	state->metadata = &fat_metadata_tables[i];
	(void)mutex_init(&state->lock, LOCK_RANK_INODE, "FAT mount");
	state->disk = mountp->m_disk;
	state->owner = mountp;
	state->read_only = (mountp->m_flags & MOUNT_READ_ONLY) != 0 ||
			   (mountp->m_disk->d_flags & DISK_READ_ONLY) != 0;

	/* Checks the operation result. */
	result = fat_probe_volume(mountp->m_disk, 0, &type);
	if (result == 0) {
		/* Dispatch the selected syntax or record type. */
		switch (type) {
		case ZEDBSD_FAT12:
			result = fat12_mount(state);
			break;
		case ZEDBSD_FAT16:
			result = fat16_mount(state);
			break;
		case ZEDBSD_FAT32:
			result = fat32_mount(state);
			break;
		default:
			result = EOPNOTSUPP;
			break;
		}
	}

	/* Checks the operation result. */
	if (result != 0) {
		irq = spin_lock_irqsave(&fat_pool_lock);

		memset(&fat_metadata_tables[i], 0, sizeof(fat_metadata_tables[i]));
		memset(state, 0, sizeof(*state));

		spin_unlock_irqrestore(&fat_pool_lock, irq);

		/* Failed. */
		return result;
	}

	fat_metadata_load(state);
	mountp->m_data = state;

	/* Handles the root availability. */
	root = inode_alloc(mountp);
	if (root == NULL) {
		irq = spin_lock_irqsave(&fat_pool_lock);

		memset(&fat_metadata_tables[i], 0, sizeof(fat_metadata_tables[i]));
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

/* Supports the fat sync mount operation. */
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

	/* Handles the state availability. */
	if (state == NULL)
		return EINVAL;

	mutex_lock(&state->lock);

	/* Checks the operation status. */
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

		/* Handles the file condition. */
		irq = spin_lock_irqsave(&fat_pool_lock);
		if (!file->used || file->mount != state ||
		    !file->directory_dirty) {
			spin_unlock_irqrestore(&fat_pool_lock, irq);
			continue;
		}

		owner = file->owner;
		spin_unlock_irqrestore(&fat_pool_lock, irq);

		/* Handles the owner availability. */
		if (owner == NULL || (owner->i_flags & INODE_DEAD) != 0)
			continue;

		/* Checks the operation status. */
		error = fat_raw_flush_file(file);
		if (error == 0)
			fat_sync_inode_state(owner, file);
	}

	/* Checks the operation status. */
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

/* Supports the fat unmount impl operation. */
static void
fat_unmount_impl(
	struct mount *mountp)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	struct fat_metadata_table *metadata;
	unsigned long irq;

	/* Handles the state availability. */
	if (state == NULL)
		return;

	mutex_lock(&state->lock);

	fat_engine_invalidate(state);

	mutex_unlock(&state->lock);

	metadata = state->metadata;

	irq = spin_lock_irqsave(&fat_pool_lock);

	memset(state, 0, sizeof(*state));

	/* Handles the metadata availability. */
	if (metadata != NULL)
		memset(metadata, 0, sizeof(*metadata));

	spin_unlock_irqrestore(&fat_pool_lock, irq);

	mountp->m_data = NULL;
}

/* Supports the fat statvfs operation. */
static int
fat_statvfs(
	struct mount *mountp,
	struct statvfs *result)
{
	struct fat_mount_state *state = fat_mount_state(mountp);
	struct fat_mount_state *fat;
	uint32_t free_clusters;
	int error;

	/* Handles the state availability. */
	if (state == NULL || result == NULL)
		return EINVAL;

	mutex_lock(&state->lock);

	fat = state;

	/* Checks the operation status. */
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
	struct fat_chain_cursor cursor;
	uint32_t wanted;
	int error;

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
	error = fat_raw_validate_chain_at(mount,
					  fat_inode(file->f_inode)->fi_first_cluster,
					  wanted,
					  &cursor, NULL);

	mutex_unlock(&mount->lock);

	/* Checks the operation status. */
	if (error != 0)
		return -error;

	/* Reports only the requested, valid cluster position as eligible. */
	if (cursor.index != wanted ||
	    !fat_raw_valid_cluster(mount, cursor.cluster)) {
		/* Succeeded. */
		return 0;
	}

	/* Reports operation failure. */
	return 1;
}
