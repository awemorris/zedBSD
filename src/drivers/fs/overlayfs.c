/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The direct upper/lower overlay filesystem.
 *
 * An overlay presents a writable upper directory tree over a read-only
 * lower one.  Lookups merge the two, whiteouts and opaque directories
 * recorded in a journal on the upper mount hide lower objects, and a
 * write copies a lower file up under a reserved temporary name before
 * renaming it into place.  Every upper mutation is followed by a mount
 * sync; a cleanup that cannot be made durable quarantines the mount
 * read-only rather than expose an inconsistent namespace.
 */

#include "kern/overlayfs.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/kmem.h"
#include "kern/namecache.h"
#include "kern/namei.h"
#include "kern/pipe.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>

#define OVERLAY_INODE_MAX 256U
#define OVERLAY_IDENTITY_MAX 128U
#define OVERLAY_METADATA_MAX 128U
#define OVERLAY_JOURNAL_BYTES (128U * 1024U)
#define OVERLAY_RECORD_BYTES 512U
#define OVERLAY_SLOT_SECTORS (OVERLAY_JOURNAL_BYTES / OVERLAY_RECORD_BYTES)
#define OVERLAY_PATH_RECORD_MAX 468U
#ifdef ZEDBSD_OVERLAY_CONTENT_HOST_TEST
/*
 * Keep host-test functions independently discardable.  The kernel link
 * still collects the complete overlay implementation in high memory.
 */
#define OVERLAY_HIGH
#else
#define OVERLAY_HIGH __attribute__((section(".hightext")))
#endif

#define OVERLAY_META_WHITEOUT 0x01U
#define OVERLAY_META_OPAQUE 0x02U
#define OVERLAY_OP_ADD_WHITEOUT 1U
#define OVERLAY_OP_REMOVE_WHITEOUT 2U
#define OVERLAY_OP_SET_OPAQUE 3U
#define OVERLAY_OP_CLEAR_OPAQUE 4U

#define OVERLAY_MATERIALIZATION_MAX ((ZEDBSD_PATH_MAX / 2U) + 1U)

enum overlay_identity_state {
	OVERLAY_ID_FREE,
	OVERLAY_ID_ACTIVE,
	OVERLAY_ID_RETIRED,
};

struct overlay_identity {
	ino_t ino;
	uint8_t state;
	char path[ZEDBSD_PATH_MAX];
};

struct overlay_metadata {
	uint8_t used;
	uint8_t flags;
	uint64_t sequence;
	char path[ZEDBSD_PATH_MAX];
};

struct overlay_mount_state {
	struct mount *owner;
	struct path upper_root;
	struct path lower_root;
	unsigned flags;
	ino_t next_ino;
	struct overlay_identity identities[OVERLAY_IDENTITY_MAX];
	struct overlay_metadata metadata[OVERLAY_METADATA_MAX];
	struct file *journal[2];
	unsigned active_slot;
	unsigned next_sector;
	uint64_t epoch;
	uint64_t sequence;
	uint32_t journal_generation;
	uint16_t temp_counter;
	struct mutex copy_up_lock;
};

struct overlay_journal_view {
	int valid;
	uint64_t epoch;
	uint64_t sequence;
	unsigned next_sector;
	uint32_t digest;
	struct overlay_metadata metadata[OVERLAY_METADATA_MAX];
};

struct overlay_inode_info {
	struct path upper;
	struct path lower;
	unsigned identity_index;
	char path[ZEDBSD_PATH_MAX];
};

struct overlay_inode_slot {
	struct inode inode;
	struct overlay_inode_info info;
	uint8_t used;
};

struct overlay_file_info {
	struct file *real;
};

enum overlay_dir_phase {
	OVERLAY_DIR_UPPER,
	OVERLAY_DIR_LOWER,
	OVERLAY_DIR_DONE,
};

struct overlay_dir_cursor {
	enum overlay_dir_phase phase;
	struct file *active;
};

enum overlay_path_selection {
	OVERLAY_PATH_VISIBLE,
	OVERLAY_PATH_UPPER,
	OVERLAY_PATH_LOWER,
};

struct overlay_materialization_entry {
	struct overlay_materialization_entry *next;
	struct inode *directory;
	struct path parent_upper;
	struct path created_upper;
	char name[NAME_MAX + 1U];
};

struct overlay_materialization_transaction {
	struct overlay_materialization_entry *created;
	unsigned count;
};

static struct overlay_inode_slot overlay_inodes[OVERLAY_INODE_MAX]
	__attribute__((section(".vfs_bss")));

#ifndef ZEDBSD_OVERLAY_CONTENT_HOST_TEST
typedef char overlay_record_path_must_fit[
	(ZEDBSD_PATH_MAX - 1U <= OVERLAY_PATH_RECORD_MAX) ? 1 : -1];
#endif

static int overlay_layers_supported(const struct overlay_mount_args *args);
static OVERLAY_HIGH uint16_t overlay_get16(const uint8_t *p);
static OVERLAY_HIGH uint32_t overlay_get32(const uint8_t *p);
static OVERLAY_HIGH uint64_t overlay_get64(const uint8_t *p);
static OVERLAY_HIGH void overlay_put16(uint8_t *p, uint16_t value);
static OVERLAY_HIGH void overlay_put32(uint8_t *p, uint32_t value);
static OVERLAY_HIGH void overlay_put64(uint8_t *p, uint64_t value);
static OVERLAY_HIGH uint32_t overlay_crc_update(uint32_t crc,
    const uint8_t *data, size_t length);
static OVERLAY_HIGH uint32_t overlay_record_crc(const uint8_t record[OVERLAY_RECORD_BYTES]);
static OVERLAY_HIGH int overlay_all_zero(const uint8_t *data, size_t length);
static OVERLAY_HIGH const uint8_t * overlay_id(const struct overlay_mount_state *state);
static OVERLAY_HIGH int overlay_metadata_find(const struct overlay_metadata entries[OVERLAY_METADATA_MAX], const char *path);
static OVERLAY_HIGH int overlay_metadata_apply(struct overlay_metadata entries[OVERLAY_METADATA_MAX], const char *path, unsigned opcode, uint64_t sequence);
static OVERLAY_HIGH unsigned overlay_metadata_flags(const struct overlay_mount_state *state, const char *path);
static OVERLAY_HIGH uint32_t overlay_metadata_digest(const struct overlay_metadata entries[OVERLAY_METADATA_MAX]);
static OVERLAY_HIGH int overlay_read_record(struct file *file, unsigned sector,
    uint8_t record[OVERLAY_RECORD_BYTES]);
static OVERLAY_HIGH int overlay_record_valid(const uint8_t record[OVERLAY_RECORD_BYTES]);
static OVERLAY_HIGH int overlay_snapshot_apply(struct overlay_journal_view *view, const uint8_t record[OVERLAY_RECORD_BYTES], const uint8_t id[4]);
static OVERLAY_HIGH int overlay_operation_apply(struct overlay_journal_view *view, const uint8_t record[OVERLAY_RECORD_BYTES], const uint8_t id[4]);
static OVERLAY_HIGH int overlay_validate_slot(struct overlay_mount_state *state,
    unsigned slot, struct overlay_journal_view *view);
static OVERLAY_HIGH int overlay_open_journal(struct overlay_mount_state *state,
    unsigned slot);
static OVERLAY_HIGH int overlay_journal_load(struct overlay_mount_state *state);
static OVERLAY_HIGH int overlay_write_record(struct file *file, unsigned sector,
    const uint8_t record[OVERLAY_RECORD_BYTES]);
static OVERLAY_HIGH unsigned overlay_metadata_count(const struct overlay_metadata entries[OVERLAY_METADATA_MAX]);
static OVERLAY_HIGH int overlay_metadata_sorted_index(const struct overlay_metadata entries[OVERLAY_METADATA_MAX], const char *after);
static OVERLAY_HIGH int overlay_journal_compact(struct overlay_mount_state *state);
static OVERLAY_HIGH int overlay_journal_compact_impl(struct overlay_mount_state *state);
static OVERLAY_HIGH int overlay_journal_append(struct overlay_mount_state *state, unsigned opcode, const char *path);
static OVERLAY_HIGH int overlay_journal_append_impl(struct overlay_mount_state *state, unsigned opcode, const char *path);
static OVERLAY_HIGH struct overlay_inode_info * overlay_info(const struct inode *inode);
static OVERLAY_HIGH int overlay_slot_index(const struct inode *inode);
static OVERLAY_HIGH struct inode * overlay_alloc_inode(struct mount *mountp);
static OVERLAY_HIGH void overlay_free_inode(struct inode *inode);
static OVERLAY_HIGH const struct path * overlay_select_path_locked(const struct overlay_inode_info *info, enum overlay_path_selection selection);
static OVERLAY_HIGH int overlay_path_snapshot(struct inode *inode,
    enum overlay_path_selection selection, struct path *result);
static OVERLAY_HIGH int overlay_info_snapshot(struct inode *inode,
    struct path *upper, struct path *lower, char relative[ZEDBSD_PATH_MAX]);
static OVERLAY_HIGH int overlay_temporary_name(const char *name);
static OVERLAY_HIGH int overlay_reserved_name(const char *name);
static OVERLAY_HIGH int overlay_component_text(const struct componentname *component, char name[NAME_MAX + 1U]);
static OVERLAY_HIGH int overlay_join(const char *parent, const char *name,
    char result[ZEDBSD_PATH_MAX]);
static OVERLAY_HIGH int overlay_identity_get(struct overlay_mount_state *state,
    const char *path, unsigned *index_out, ino_t *ino_out, int *created_out);
static OVERLAY_HIGH int overlay_lookup_real(const struct path *directory,
    const struct componentname *component, struct path *result);
static OVERLAY_HIGH void overlay_refresh_locked(struct inode *inode);
static OVERLAY_HIGH void overlay_refresh(struct inode *inode);
static OVERLAY_HIGH int overlay_make_inode(struct mount *mountp,
    const char *relative, struct path *upper, struct path *lower,
    struct inode **result);
static OVERLAY_HIGH int overlay_lookup(struct inode *directory,
    const struct componentname *component, struct inode **result);
static OVERLAY_HIGH int overlay_getattr(struct inode *inode,
    struct stat *status);
static OVERLAY_HIGH int overlay_find_relative(struct mount *mountp,
    const char *relative, struct inode **result);
static OVERLAY_HIGH int overlay_split_path(const char *path,
    char parent[ZEDBSD_PATH_MAX], struct componentname *name);
static OVERLAY_HIGH void overlay_publish_upper(struct inode *inode,
    const struct path *upper, int clear_lower, const char *relative);
static OVERLAY_HIGH void overlay_install_upper(struct inode *inode,
    const struct path *upper);
static OVERLAY_HIGH void overlay_clear_upper_if(struct inode *inode,
    const struct path *expected);
static OVERLAY_HIGH int overlay_materialization_complete(struct overlay_mount_state *state, struct overlay_materialization_transaction *transaction, int error);
static OVERLAY_HIGH int overlay_ensure_upper_dir_tracked(struct inode *directory, struct overlay_materialization_transaction *transaction);
static OVERLAY_HIGH int overlay_ensure_upper_dir(struct inode *directory);
static OVERLAY_HIGH void overlay_temp_name(uint16_t number, char name[11]);
static OVERLAY_HIGH int overlay_copy_up_regular(struct inode *inode);
static OVERLAY_HIGH int overlay_new_preflight(struct inode *directory,
    const struct componentname *name, char text[NAME_MAX + 1U],
    char relative[ZEDBSD_PATH_MAX], struct inode **hidden_lower);
static OVERLAY_HIGH int overlay_finish_new(struct inode *directory,
    const struct componentname *name, const char *relative,
    int directory_object, int opaque_added, struct inode **result);
static OVERLAY_HIGH int overlay_create(struct inode *directory,
    const struct componentname *name,
    const struct inode_creation_request *request, struct inode **result);
static OVERLAY_HIGH int overlay_mkdir(struct inode *directory,
    const struct componentname *name,
    const struct inode_creation_request *request, struct inode **result);
static OVERLAY_HIGH void overlay_special_clear(struct inode *inode,
    void *expected);
static OVERLAY_HIGH int overlay_special_transfer(struct inode *source,
    struct inode *destination, void *expected);
static OVERLAY_HIGH int overlay_mknod_socket(struct inode *directory,
    const struct componentname *name,
    const struct inode_creation_request *request, const char *relative,
    struct inode **result);
static OVERLAY_HIGH int overlay_mknod(struct inode *directory,
    const struct componentname *name,
    const struct inode_creation_request *request, struct inode **result);
static OVERLAY_HIGH int overlay_symlink(struct inode *directory,
    const struct componentname *name, const char *target,
    const struct inode_creation_request *request, struct inode **result);
static OVERLAY_HIGH ssize_t overlay_readlink(struct inode *inode, char *buffer,
    size_t capacity);
static OVERLAY_HIGH int overlay_path_is_below(const char *path,
    const char *root);
static OVERLAY_HIGH int overlay_repath_preflight(struct overlay_mount_state *state, const char *old_path, const char *new_path, const struct inode *replaced);
static OVERLAY_HIGH void overlay_repath_commit(struct overlay_mount_state *state, struct mount *mountp, const char *old_path, const char *new_path);
static OVERLAY_HIGH int overlay_rename(struct inode *old_directory,
    const struct componentname *old_name, struct inode *new_directory,
    const struct componentname *new_name, unsigned flags);
static OVERLAY_HIGH void overlay_retire_inode(struct inode *inode);
static OVERLAY_HIGH int overlay_directory_empty(struct inode *inode);
static OVERLAY_HIGH int overlay_remove(struct inode *directory,
    const struct componentname *name, int removing_directory);
static OVERLAY_HIGH int overlay_unlink(struct inode *directory,
    const struct componentname *name);
static OVERLAY_HIGH int overlay_rmdir(struct inode *directory,
    const struct componentname *name);
static OVERLAY_HIGH int overlay_truncate_upper(struct inode *inode,
    const struct inode_truncate_request *request,
    struct inode_truncate_result *result);
static OVERLAY_HIGH int overlay_truncate_limited(struct inode *inode,
    const struct inode_truncate_request *request,
    struct inode_truncate_result *result);
static OVERLAY_HIGH int overlay_truncate(struct inode *inode, off_t size);
static OVERLAY_HIGH int overlay_setattr(struct inode *inode,
    const struct stat *status, unsigned mask);
static OVERLAY_HIGH void overlay_reclaim(struct inode *inode);
static OVERLAY_HIGH int overlay_regular_open(struct file *file);
static OVERLAY_HIGH ssize_t overlay_pread(struct file *file, void *buffer,
    size_t size, off_t offset);
static OVERLAY_HIGH ssize_t overlay_pread_internal(struct file *file,
    void *buffer, size_t size, off_t offset, unsigned flags);
static OVERLAY_HIGH ssize_t overlay_read(struct file *file, void *buffer,
    size_t size);
static OVERLAY_HIGH ssize_t overlay_pwrite(struct file *file,
    const void *buffer, size_t size, off_t offset);
static OVERLAY_HIGH ssize_t overlay_pwrite_internal(struct file *file,
    const void *buffer, size_t size, off_t offset, unsigned flags,
    const struct ucred *credential, const struct io_context *context);
#ifdef ZEDBSD_OVERLAY_CONTENT_HOST_TEST
static int overlay_host_truncate_limited(struct inode *inode,
    const struct inode_truncate_request *request,
    struct inode_truncate_result *result);
#endif
static OVERLAY_HIGH ssize_t overlay_write(struct file *file, const void *buffer,
    size_t size);
static OVERLAY_HIGH int overlay_regular_fsync(struct file *file);
static OVERLAY_HIGH int overlay_regular_close(struct file *file);
static OVERLAY_HIGH int overlay_dir_open(struct file *file);
static OVERLAY_HIGH void overlay_dir_drop_active(struct overlay_dir_cursor *cursor);
static OVERLAY_HIGH int overlay_dir_open_phase(struct file *file,
    struct overlay_dir_cursor *cursor);
static OVERLAY_HIGH int overlay_dir_upper_has(struct inode *directory,
    const char *name);
static OVERLAY_HIGH int overlay_dir_child_hidden(struct inode *directory,
    const char *name);
static OVERLAY_HIGH int overlay_dir_emit(struct file *file, const char *name,
    struct dirent *entry);
static OVERLAY_HIGH int overlay_readdir(struct file *file, struct dirent *entry,
    int *eof);
static OVERLAY_HIGH off_t overlay_dir_seek(struct file *file, off_t offset,
    int whence);
static OVERLAY_HIGH int overlay_dir_close(struct file *file);
static OVERLAY_HIGH int overlay_directory_fsync(struct file *file);
static OVERLAY_HIGH int overlay_cleanup_temps(struct path *directory,
    unsigned depth, unsigned *visited, unsigned *deleted);
static OVERLAY_HIGH int overlay_mount_impl(struct mount *mountp);
static OVERLAY_HIGH int overlay_sync_mount(struct mount *mountp);
static OVERLAY_HIGH int overlay_statvfs(struct mount *mountp,
    struct statvfs *result);
static OVERLAY_HIGH void overlay_unmount_impl(struct mount *mountp);
static OVERLAY_HIGH int overlay_prepare_mutation(struct inode *inode);

static const struct inode_ops overlay_inode_ops = {
	.lookup = overlay_lookup,
	.create = overlay_create,
	.mkdir = overlay_mkdir,
	.mknod = overlay_mknod,
	.unlink = overlay_unlink,
	.rmdir = overlay_rmdir,
	.rename = overlay_rename,
	.symlink = overlay_symlink,
	.readlink = overlay_readlink,
	.getattr = overlay_getattr,
	.prepare_mutation = overlay_prepare_mutation,
	.setattr = overlay_setattr,
	.truncate = overlay_truncate,
	.truncate_limited = overlay_truncate_limited,
	.reclaim = overlay_reclaim,
};

static const struct file_ops overlay_regular_ops = {
	.open = overlay_regular_open,
	.read = overlay_read,
	.write = overlay_write,
	.pread = overlay_pread,
	.pwrite = overlay_pwrite,
	.pread_internal = overlay_pread_internal,
	.pwrite_internal = overlay_pwrite_internal,
	.fsync = overlay_regular_fsync,
	.close = overlay_regular_close,
};

static const struct file_ops overlay_directory_ops = {
	.open = overlay_dir_open,
	.readdir = overlay_readdir,
	.seek = overlay_dir_seek,
	.fsync = overlay_directory_fsync,
	.close = overlay_dir_close,
};

static const struct filesystem_type overlay_filesystem_type = {
	.fs_name = "overlay",
	.fs_flags = FILESYSTEM_NODEV,
	.mount = overlay_mount_impl,
	.sync = overlay_sync_mount,
	.statvfs = overlay_statvfs,
	.unmount = overlay_unmount_impl,
	.alloc_inode = overlay_alloc_inode,
	.free_inode = overlay_free_inode,
};

/* Returns the immutable lower-root relationship of a referenced overlay. */
OVERLAY_HIGH int
drv_overlay_lower_root_ref(struct mount *mountp, struct path *result)
{
	struct overlay_mount_state *state;

	/* A native root has no overlay lower image. */
	if (mountp == NULL || result == NULL)
		return EINVAL;
	path_init(result);
	if (mountp->m_type != &overlay_filesystem_type)
		return EOPNOTSUPP;
	state = mountp->m_data;
	if (state == NULL || state->lower_root.p_mount == NULL)
		return ENXIO;

	/* The caller's mount reference prevents destruction of these owned paths. */
	path_set(result, state->lower_root.p_mount, state->lower_root.p_inode);
	return 0;
}

/*
 * Registers the overlay filesystem type.
 */
OVERLAY_HIGH int
drv_overlayfs_init(
	void)
{
	int error;

	/* Reports why the registration failed. */
	error = filesystem_register(&overlay_filesystem_type);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Mounts an overlay on a top-level directory of a namespace root.
 */
OVERLAY_HIGH int
drv_overlay_mount_at(
	struct mount *namespace_root,
	const char *target,
	const struct overlay_mount_args *args,
	struct mount **result)
{
	struct path root;
	const char *name;
	const char *separator;
	int mount_flags;
	int error;

	/* Rejects a missing operand or a target that is not one component. */
	name = target;
	if (namespace_root == NULL || target == NULL || args == NULL)
		return EINVAL;

	/* A leading slash is the root, which the mount is taken relative to. */
	if (name[0] == '/')
		name++;

	/* What is left has to be a name, and one single component of a path. */
	separator = strchr(name, '/');
	if (name[0] == '\0' || separator != NULL)
		return EINVAL;

	/* Mounts under the namespace root. */
	if (args->flags == OVERLAY_READ_ONLY)
		mount_flags = MOUNT_READ_ONLY;
	else
		mount_flags = 0;
	path_init(&root);
	path_set(&root, namespace_root, namespace_root->m_root);
	error = mount_at("overlay", &root, name, mount_flags, (void *)args,
	    result);
	path_release(&root);

	/* Reports why the mount failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

#ifdef ZEDBSD_OVERLAY_CONTENT_HOST_TEST
/*
 * Runs the stacked write callback for a host test with a temporary
 * overlay binding of an outer file over a real file.
 */
ssize_t
overlay_content_host_pwrite(
	struct file *outer,
	struct file *real,
	const void *buffer,
	size_t size,
	off_t offset,
	unsigned flags,
	const struct ucred *credential)
{
	struct overlay_file_info file_info;
	struct overlay_inode_info inode_info;
	const struct inode_ops *saved_inode_ops;
	const struct file_ops *saved_file_ops;
	void *saved_file_data;
	void *saved_inode_data;
	ssize_t count;

	/* Rejects a missing file or inode. */
	if (outer == NULL ||
	    outer->f_inode == NULL ||
	    real == NULL ||
	    real->f_inode == NULL)
		return -EINVAL;

	/* Binds the outer file to the real one for the duration of the call. */
	memset(&file_info, 0, sizeof(file_info));
	memset(&inode_info, 0, sizeof(inode_info));
	file_info.real = real;
	inode_info.upper.p_inode = real->f_inode;
	saved_file_data = outer->f_data;
	saved_inode_data = outer->f_inode->i_data;
	saved_inode_ops = outer->f_inode->i_op;
	saved_file_ops = outer->f_inode->i_fop;
	outer->f_data = &file_info;
	outer->f_inode->i_data = &inode_info;
	count = overlay_pwrite_internal(outer, buffer, size, offset, flags,
	    credential, NULL);
	outer->f_data = saved_file_data;
	outer->f_inode->i_data = saved_inode_data;
	outer->f_inode->i_op = saved_inode_ops;
	outer->f_inode->i_fop = saved_file_ops;

	/* Reports the write result. */
	return count;
}

/*
 * Runs the stacked truncate callback for a host test with a temporary
 * overlay binding of an outer inode over a real inode.
 */
int
overlay_content_host_truncate(
	struct inode *outer,
	struct inode *real,
	const struct inode_truncate_request *request,
	struct inode_truncate_result *result)
{
	struct overlay_mount_state state;
	struct overlay_inode_info inode_info;
	struct filesystem_type upper_type;
	struct mount outer_mount;
	struct mount upper_mount;
	struct inode_ops host_ops;
	const struct inode_ops *saved_ops;
	struct mount *saved_mount;
	void *saved_data;
	int error;

	/* Rejects a missing operand. */
	if (outer == NULL || real == NULL || request == NULL || result == NULL)
		return EINVAL;

	/* Builds a minimal writable overlay around the real inode. */
	memset(&state, 0, sizeof(state));
	memset(&inode_info, 0, sizeof(inode_info));
	memset(&upper_type, 0, sizeof(upper_type));
	memset(&outer_mount, 0, sizeof(outer_mount));
	memset(&upper_mount, 0, sizeof(upper_mount));
	memset(&host_ops, 0, sizeof(host_ops));
	state.flags = OVERLAY_READ_WRITE;
	outer_mount.m_data = &state;
	upper_mount.m_type = &upper_type;
	inode_info.upper.p_mount = &upper_mount;
	inode_info.upper.p_inode = real;
	host_ops.truncate_limited = overlay_host_truncate_limited;
	saved_ops = outer->i_op;
	saved_mount = outer->i_mount;
	saved_data = outer->i_data;
	outer->i_op = &host_ops;
	outer->i_mount = &outer_mount;
	outer->i_data = &inode_info;
	error = inode_truncate_transaction(outer, request, result);
	outer->i_op = saved_ops;
	outer->i_mount = saved_mount;
	outer->i_data = saved_data;

	/* Reports why the truncate failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reports whether a stacking of overlays on overlays would be accepted.
 */
int
overlay_content_host_layers_supported(
	int upper_overlay,
	int lower_overlay)
{
	struct mount upper;
	struct mount lower;
	struct overlay_mount_args args;
	int supported;

	/* Builds a mount pair of the requested layer kinds. */
	memset(&upper, 0, sizeof(upper));
	memset(&lower, 0, sizeof(lower));
	memset(&args, 0, sizeof(args));
	if (upper_overlay)
		upper.m_type = &overlay_filesystem_type;
	if (lower_overlay)
		lower.m_type = &overlay_filesystem_type;
	args.upper.p_mount = &upper;
	args.lower.p_mount = &lower;

	/* Asks whether the overlay would accept that pair of layers. */
	supported = overlay_layers_supported(&args);
	if (!supported)
		return EOPNOTSUPP;

	/* Succeeded. */
	return 0;
}
#endif

/* Tests that neither layer is itself an overlay. */
static int
overlay_layers_supported(
	const struct overlay_mount_args *args)
{
	if (args == NULL)
		return 0;
	if (args->upper.p_mount != NULL &&
	    args->upper.p_mount->m_type == &overlay_filesystem_type)
		return 0;
	if (args->lower.p_mount != NULL &&
	    args->lower.p_mount->m_type == &overlay_filesystem_type)
		return 0;
	return 1;
}

/* Reads a little-endian 16-bit field. */
static OVERLAY_HIGH uint16_t
overlay_get16(
	const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

/* Reads a little-endian 32-bit field. */
static OVERLAY_HIGH uint32_t
overlay_get32(
	const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		(uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Reads a little-endian 64-bit field. */
static OVERLAY_HIGH uint64_t
overlay_get64(
	const uint8_t *p)
{
	uint32_t low;
	uint32_t high;

	/* The two halves the field is stored as, least significant first. */
	low = overlay_get32(p);
	high = overlay_get32(p + 4);

	/* The two halves assembled into one value. */
	return low | (uint64_t)high << 32;
}

/* Writes a little-endian 16-bit field. */
static OVERLAY_HIGH void
overlay_put16(
	uint8_t *p,
	uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

/* Writes a little-endian 32-bit field. */
static OVERLAY_HIGH void
overlay_put32(
	uint8_t *p,
	uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

/* Writes a little-endian 64-bit field. */
static OVERLAY_HIGH void
overlay_put64(
	uint8_t *p,
	uint64_t value)
{
	overlay_put32(p, (uint32_t)value);
	overlay_put32(p + 4, (uint32_t)(value >> 32));
}

/* Feeds bytes into a CRC-32 in the reflected form. */
static OVERLAY_HIGH uint32_t
overlay_crc_update(
	uint32_t crc,
	const uint8_t *data,
	size_t length)
{
	size_t i;

	/* Folds each byte in one bit at a time. */
	while (length != 0) {
		crc ^= *data;
		data++;
		for (i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0xedb88320U &
				(uint32_t)-(int32_t)(crc & 1U));
		length--;
	}

	/* Reports the running value to the caller. */
	return crc;
}

/* Computes the CRC stored in the last four bytes of a record. */
static OVERLAY_HIGH uint32_t
overlay_record_crc(
	const uint8_t record[OVERLAY_RECORD_BYTES])
{
	return overlay_crc_update(0xffffffffU, record, 508U) ^ 0xffffffffU;
}

/* Tests whether a byte range is all zero. */
static OVERLAY_HIGH int
overlay_all_zero(
	const uint8_t *data,
	size_t length)
{
	while (length != 0) {
		if (*data != 0)
			return 0;
		data++;
		length--;
	}

	return 1;
}

/* Reports the four-byte identifier stamped on journal records. */
static OVERLAY_HIGH const uint8_t *
overlay_id(
	const struct overlay_mount_state *state)
{
	static const uint8_t overlay_id[4] = { 'Z', 'O', 'V', 'L' };

	(void)state;
	return overlay_id;
}

/* Finds the metadata entry of a path, or -1. */
static OVERLAY_HIGH int
overlay_metadata_find(
	const struct overlay_metadata entries[OVERLAY_METADATA_MAX],
	const char *path)
{
	unsigned i;
	int difference;

	/* Walks the table looking for the entry that holds this path. */
	for (i = 0; i < OVERLAY_METADATA_MAX; i++) {
		/* An unused slot holds no path to compare against. */
		if (!entries[i].used)
			continue;

		/* Compares the slot's path with the one being looked for. */
		difference = strcmp(entries[i].path, path);
		if (difference == 0)
			return (int)i;
	}

	/* Reports that no entry holds it. */
	return -1;
}

/* Applies a whiteout or opaque operation to a metadata table. */
static OVERLAY_HIGH int
overlay_metadata_apply(
	struct overlay_metadata entries[OVERLAY_METADATA_MAX],
	const char *path,
	unsigned opcode,
	uint64_t sequence)
{
	int index;
	unsigned i;
	unsigned bit;

	index = overlay_metadata_find(entries, path);

	/* Maps the opcode to the flag it sets or clears. */
	if (opcode == OVERLAY_OP_ADD_WHITEOUT ||
	    opcode == OVERLAY_OP_REMOVE_WHITEOUT)
		bit = OVERLAY_META_WHITEOUT;
	else if (opcode == OVERLAY_OP_SET_OPAQUE ||
		 opcode == OVERLAY_OP_CLEAR_OPAQUE)
		bit = OVERLAY_META_OPAQUE;
	else
		return EINVAL;

	/* A clear of an absent entry is a no-op; a set allocates one. */
	if (index < 0) {
		if (opcode == OVERLAY_OP_REMOVE_WHITEOUT ||
		    opcode == OVERLAY_OP_CLEAR_OPAQUE)
			return 0;
		for (i = 0; i < OVERLAY_METADATA_MAX; i++) {
			if (!entries[i].used) {
				index = (int)i;
				memset(&entries[i], 0, sizeof(entries[i]));
				entries[i].used = 1;
				strcpy(entries[i].path, path);
				break;
			}
		}

		if (index < 0)
			return ENOSPC;
	}

	/* Updates the flag, dropping an entry with none left. */
	if (opcode == OVERLAY_OP_ADD_WHITEOUT ||
	    opcode == OVERLAY_OP_SET_OPAQUE)
		entries[index].flags |= (uint8_t)bit;
	else
		entries[index].flags &= (uint8_t)~bit;
	entries[index].sequence = sequence;
	if (entries[index].flags == 0)
		memset(&entries[index], 0, sizeof(entries[index]));
	return 0;
}

/* Reports the metadata flags of a path. */
static OVERLAY_HIGH unsigned
overlay_metadata_flags(
	const struct overlay_mount_state *state,
	const char *path)
{
	int index;

	index = overlay_metadata_find(state->metadata, path);
	if (index < 0)
		return 0;
	return state->metadata[index].flags;
}

/* Digests a metadata table in path order, independent of slot layout. */
static OVERLAY_HIGH uint32_t
overlay_metadata_digest(
	const struct overlay_metadata entries[OVERLAY_METADATA_MAX])
{
	uint32_t crc;
	char previous[ZEDBSD_PATH_MAX];
	unsigned emitted;
	int best;
	int difference;
	unsigned i;

	crc = 0xffffffffU;
	emitted = 0;

	/* Emits each entry after the previous one in string order. */
	previous[0] = '\0';
	for (;;) {
		best = -1;
		for (i = 0; i < OVERLAY_METADATA_MAX; i++) {
			/* An unused slot holds no path to emit. */
			if (!entries[i].used)
				continue;

			/* Skips every path already emitted in this order. */
			if (emitted != 0) {
				difference = strcmp(entries[i].path, previous);
				if (difference <= 0)
					continue;
			}

			/* The first candidate is the best one so far. */
			if (best < 0) {
				best = (int)i;
				continue;
			}

			/* And any smaller path takes its place. */
			difference = strcmp(entries[i].path,
					    entries[best].path);
			if (difference < 0)
				best = (int)i;
		}

		if (best < 0)
			break;
		/* Folds the path and its flag into the running digest. */
		crc = overlay_crc_update(crc,
		    (const uint8_t *)entries[best].path,
		    strlen(entries[best].path) + 1U);
		crc = overlay_crc_update(crc, &entries[best].flags, 1U);
		strcpy(previous, entries[best].path);
		emitted++;
	}

	return crc ^ 0xffffffffU;
}

/* Reads one journal record. */
static OVERLAY_HIGH int
overlay_read_record(
	struct file *file,
	unsigned sector,
	uint8_t record[OVERLAY_RECORD_BYTES])
{
	ssize_t count;

	/* Rejects a sector outside the slot. */
	if (sector >= OVERLAY_SLOT_SECTORS)
		return EOVERFLOW;

	/* Reports a short read as a device error. */
	count = file_pread(file, record, OVERLAY_RECORD_BYTES,
		(off_t)(sector * OVERLAY_RECORD_BYTES));
	if (count == OVERLAY_RECORD_BYTES)
		return 0;
	if (count < 0)
		return (int)-count;
	return EIO;
}

/* Tests a record's CRC. */
static OVERLAY_HIGH int
overlay_record_valid(
	const uint8_t record[OVERLAY_RECORD_BYTES])
{
	uint32_t stored;
	uint32_t computed;

	/* The checksum the record carries, and the one it should carry. */
	stored = overlay_get32(record + 508);
	computed = overlay_record_crc(record);

	/* A record whose checksum disagrees was never fully written. */
	if (stored != computed)
		return 0;

	/* Succeeded. */
	return 1;
}

/* Applies a snapshot record to a journal view being rebuilt. */
static OVERLAY_HIGH int
overlay_snapshot_apply(
	struct overlay_journal_view *view,
	const uint8_t record[OVERLAY_RECORD_BYTES],
	const uint8_t id[4])
{
	uint32_t length;
	uint32_t reserved;
	unsigned flags;
	uint64_t sequence;
	uint64_t epoch;
	uint16_t version;
	char path[ZEDBSD_PATH_MAX];
	char *root;
	unsigned i;
	int signature;
	int identity;
	int tail_blank;
	int existing;

	/* Compares the record against the snapshot-record signature. */
	signature = memcmp(record, "ZOVLSNP\0", 8);
	if (signature != 0)
		return EINVAL;	/* Failed. */

	/* A record of another format version is not one this driver reads. */
	version = overlay_get16(record + 8);
	if (version != 1)
		return EINVAL;	/* Failed. */

	/* The flags that say why this path is in the table at all. */
	flags = overlay_get16(record + 0x0a);
	if ((flags & ~(OVERLAY_META_WHITEOUT | OVERLAY_META_OPAQUE)) != 0)
		return EINVAL;	/* Failed. */

	/* An entry carrying no flag records nothing about its path. */
	if (flags == 0)
		return EINVAL;	/* Failed. */

	/* How many bytes of path follow the header. */
	length = overlay_get32(record + 0x0c);
	if (length == 0)
		return EINVAL;	/* Failed. */

	/* A path longer than the kernel or the record itself can hold. */
	if (length >= ZEDBSD_PATH_MAX || length > OVERLAY_PATH_RECORD_MAX)
		return EINVAL;	/* Failed. */

	/* A record of another epoch belongs to a slot that was replaced. */
	epoch = overlay_get64(record + 0x10);
	if (epoch != view->epoch)
		return EINVAL;	/* Failed. */

	/* The sequence number the entry was last changed at. */
	sequence = overlay_get64(record + 0x18);
	if (sequence == 0 || sequence > view->sequence)
		return EINVAL;	/* Failed. */

	/* A record written for a different mount is not this one's. */
	identity = memcmp(record + 0x20, id, 4);
	if (identity != 0)
		return EINVAL;	/* Failed. */

	/* The reserved word is written as zero and has to read back so. */
	reserved = overlay_get32(record + 0x24);
	if (reserved != 0)
		return EINVAL;	/* Failed. */

	/* And nothing at all may follow the path inside the record. */
	tail_blank = overlay_all_zero(record + 0x28 + length,
				      508U - (0x28U + length));
	if (!tail_blank)
		return EINVAL;	/* Failed. */

	/* Takes the path the record names as a string of its own. */
	memcpy(path, record + 0x28, length);
	path[length] = '\0';

	/* A path is stored relative to the root, so it cannot start at one. */
	root = strchr(path, '/');
	if (root == path)
		return EINVAL;	/* Failed. */

	/* Nor may two records claim the same path. */
	existing = overlay_metadata_find(view->metadata, path);
	if (existing >= 0)
		return EINVAL;	/* Failed. */

	/* Stores the entry in a free slot. */
	for (i = 0; i < OVERLAY_METADATA_MAX; i++) {
		if (!view->metadata[i].used) {
			view->metadata[i].used = 1;
			view->metadata[i].flags = (uint8_t)flags;
			view->metadata[i].sequence = sequence;
			strcpy(view->metadata[i].path, path);
			return 0;
		}
	}

	return ENOSPC;
}

/* Applies an operation record to a journal view being rebuilt. */
static OVERLAY_HIGH int
overlay_operation_apply(
	struct overlay_journal_view *view,
	const uint8_t record[OVERLAY_RECORD_BYTES],
	const uint8_t id[4])
{
	uint32_t length;
	uint32_t reserved;
	unsigned opcode;
	uint64_t sequence;
	uint64_t epoch;
	uint16_t version;
	char path[ZEDBSD_PATH_MAX];
	int signature;
	int identity;
	int tail_blank;
	int applied;

	/* Compares the record against the operation-record signature. */
	signature = memcmp(record, "ZOVLOP\0\0", 8);
	if (signature != 0)
		return EINVAL;	/* Failed. */

	/* A record of another format version is not one this driver reads. */
	version = overlay_get16(record + 8);
	if (version != 1)
		return EINVAL;	/* Failed. */

	/* Which change to the table the record stands for. */
	opcode = overlay_get16(record + 0x0a);
	if (opcode < OVERLAY_OP_ADD_WHITEOUT ||
	    opcode > OVERLAY_OP_CLEAR_OPAQUE)
		return EINVAL;	/* Failed. */

	/* How many bytes of path follow the header. */
	length = overlay_get32(record + 0x0c);
	if (length == 0)
		return EINVAL;	/* Failed. */

	/* A path longer than the kernel or the record itself can hold. */
	if (length >= ZEDBSD_PATH_MAX || length > OVERLAY_PATH_RECORD_MAX)
		return EINVAL;	/* Failed. */

	/* A record of another epoch belongs to a slot that was replaced. */
	epoch = overlay_get64(record + 0x10);
	if (epoch != view->epoch)
		return EINVAL;	/* Failed. */

	/* An operation record has to continue the sequence exactly. */
	sequence = overlay_get64(record + 0x18);
	if (sequence != view->sequence + 1U)
		return EINVAL;	/* Failed. */

	/* A record written for a different mount is not this one's. */
	identity = memcmp(record + 0x20, id, 4);
	if (identity != 0)
		return EINVAL;	/* Failed. */

	/* The reserved word is written as zero and has to read back so. */
	reserved = overlay_get32(record + 0x24);
	if (reserved != 0)
		return EINVAL;	/* Failed. */

	/* And nothing at all may follow the path inside the record. */
	tail_blank = overlay_all_zero(record + 0x28 + length,
				      508U - (0x28U + length));
	if (!tail_blank)
		return EINVAL;	/* Failed. */

	/* Takes the path the record names as a string of its own. */
	memcpy(path, record + 0x28, length);
	path[length] = '\0';

	/* Applies the change the record recorded to the table being rebuilt. */
	applied = overlay_metadata_apply(view->metadata, path, opcode,
					 sequence);
	if (applied != 0)
		return ENOSPC;	/* Failed. */
	view->sequence = sequence;
	return 0;
}

/* Rebuilds a journal slot's view, leaving it invalid on a malformed slot. */
static OVERLAY_HIGH int
overlay_validate_slot(
	struct overlay_mount_state *state,
	unsigned slot,
	struct overlay_journal_view *view)
{
	uint8_t record[OVERLAY_RECORD_BYTES];
	uint8_t commit[OVERLAY_RECORD_BYTES];
	const uint8_t *id;
	uint32_t snapshot_count;
	uint32_t commit_sector;
	uint32_t digest;
	uint32_t record_bytes;
	uint32_t reserved;
	uint32_t stored_digest;
	uint64_t last_sequence;
	uint64_t epoch;
	uint16_t version;
	uint16_t header_bytes;
	unsigned sector;
	int signature;
	int identity;
	int tail_blank;
	int valid;
	int blank;
	int error;

	id = overlay_id(state);

	/*
	 * The slot header names the epoch, the snapshot count, and the commit
	 * sector.
	 */
	memset(view, 0, sizeof(*view));
	error = overlay_read_record(state->journal[slot], 0, record);
	if (error != 0)
		return error;
	/* A header whose checksum does not hold was never fully written. */
	valid = overlay_record_valid(record);
	if (!valid)
		return 0;

	/* Compares the sector against the slot-header signature. */
	signature = memcmp(record, "ZOVLSLT\0", 8);
	if (signature != 0)
		return 0;

	/* A header of another format version is not one this driver reads. */
	version = overlay_get16(record + 8);
	if (version != 1)
		return 0;

	/* The header and the record are both of a fixed size. */
	header_bytes = overlay_get16(record + 0x0a);
	if (header_bytes != 48)
		return 0;

	record_bytes = overlay_get32(record + 0x0c);
	if (record_bytes != OVERLAY_RECORD_BYTES)
		return 0;

	/* A slot written for a different mount is not this one's. */
	identity = memcmp(record + 0x10, id, 4);
	if (identity != 0)
		return 0;

	/* The reserved word is written as zero and has to read back so. */
	reserved = overlay_get32(record + 0x14);
	if (reserved != 0)
		return 0;

	/* Epoch zero stands for a slot that was never published. */
	epoch = overlay_get64(record + 0x18);
	if (epoch == 0)
		return 0;

	/* And nothing at all may follow the header inside the record. */
	tail_blank = overlay_all_zero(record + 0x30, 508U - 0x30U);
	if (!tail_blank)
		return 0;

	view->epoch = epoch;

	/* How many snapshot records the slot says follow the header. */
	snapshot_count = overlay_get32(record + 0x20);

	/* Which sector the commit record was written in. */
	commit_sector = overlay_get32(record + 0x24);

	/* And the sequence number the table stood at when it was written. */
	last_sequence = overlay_get64(record + 0x28);

	/* A table larger than this driver keeps room for is not readable. */
	if (snapshot_count > OVERLAY_METADATA_MAX)
		return 0;

	/* The commit record sits directly after the snapshot records. */
	if (commit_sector != 1U + snapshot_count)
		return 0;

	/* And has to fall inside the slot. */
	if (commit_sector >= OVERLAY_SLOT_SECTORS)
		return 0;

	view->sequence = last_sequence;

	/* The snapshot records rebuild the table and feed the digest. */
	digest = overlay_crc_update(0xffffffffU, record, sizeof(record));
	for (sector = 1; sector <= snapshot_count; sector++) {
		error = overlay_read_record(state->journal[slot], sector,
		    record);
		if (error != 0)
			return error;
		/* A record whose checksum does not hold ends the rebuild. */
		valid = overlay_record_valid(record);
		if (!valid)
			return 0;

		/* And so does one the table will not take. */
		error = overlay_snapshot_apply(view, record, id);
		if (error != 0)
			return 0;

		digest = overlay_crc_update(digest, record, sizeof(record));
	}

	/* The commit record must match the header and the digest. */
	error = overlay_read_record(state->journal[slot], commit_sector,
	    commit);
	if (error != 0)
		return error;
	/* A commit whose checksum does not hold was never fully written. */
	valid = overlay_record_valid(commit);
	if (!valid)
		return 0;

	/* Compares the sector against the commit-record signature. */
	signature = memcmp(commit, "ZOVLCMT\0", 8);
	if (signature != 0)
		return 0;

	/* A commit of another format version is not one this driver reads. */
	version = overlay_get16(commit + 8);
	if (version != 1)
		return 0;

	/* The word a snapshot record uses for flags is unused here. */
	header_bytes = overlay_get16(commit + 0x0a);
	if (header_bytes != 0)
		return 0;

	/* A commit written for a different mount is not this one's. */
	identity = memcmp(commit + 0x0c, id, 4);
	if (identity != 0)
		return 0;

	/* Every field the header already gave has to be repeated exactly. */
	epoch = overlay_get64(commit + 0x10);
	if (epoch != view->epoch)
		return 0;

	record_bytes = overlay_get32(commit + 0x18);
	if (record_bytes != snapshot_count)
		return 0;

	record_bytes = overlay_get32(commit + 0x1c);
	if (record_bytes != commit_sector)
		return 0;

	epoch = overlay_get64(commit + 0x20);
	if (epoch != last_sequence)
		return 0;

	/* The digest ties the commit to the exact records that were written. */
	stored_digest = overlay_get32(commit + 0x28);
	if (stored_digest != (digest ^ 0xffffffffU))
		return 0;

	/* And nothing at all may follow the commit inside the record. */
	tail_blank = overlay_all_zero(commit + 0x2c, 508U - 0x2cU);
	if (!tail_blank)
		return 0;

	/* Operation records follow until the first blank or broken one. */
	sector = commit_sector + 1U;
	while (sector < OVERLAY_SLOT_SECTORS) {
		error = overlay_read_record(state->journal[slot], sector,
		    record);
		if (error != 0)
			return error;
		/* A blank sector is where the operation records stop. */
		blank = overlay_all_zero(record, sizeof(record));
		if (blank)
			break;

		/* So is one whose checksum does not hold. */
		valid = overlay_record_valid(record);
		if (!valid)
			break;

		/* And so is one the table will not take. */
		error = overlay_operation_apply(view, record, id);
		if (error != 0)
			break;

		sector++;
	}

	view->next_sector = sector;
	view->digest = overlay_metadata_digest(view->metadata);
	view->valid = 1;
	return 0;
}

/* Opens one of the two journal files on the upper root. */
static OVERLAY_HIGH int
overlay_open_journal(
	struct overlay_mount_state *state,
	unsigned slot)
{
	struct componentname component;
	struct inode *inode;
	struct path path;
	char name[7];
	int error;
	int flags;

	/* The journal must be a regular file of exactly the journal size. */
	strcpy(name, ".zovl0");
	name[5] = (char)('0' + slot);
	component.cn_nameptr = name;
	component.cn_namelen = strlen(name);
	component.cn_flags = 0;
	error = inode_lookup(state->upper_root.p_inode, &component, &inode);
	if (error != 0)
		return error;
	if (inode->i_type != INODE_REG ||
	    inode->i_size != OVERLAY_JOURNAL_BYTES) {
		inode_release(inode);
		return EINVAL;
	}

	/* Opens it for writing on a writable overlay. */
	path_init(&path);
	path_set(&path, state->upper_root.p_mount, inode);
	inode_release(inode);
	if (state->flags == OVERLAY_READ_WRITE)
		flags = O_RDWR;
	else
		flags = O_RDONLY;
	error = file_open_resolved(&path, flags, &state->journal[slot]);
	path_release(&path);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Loads the metadata from the newer valid journal slot. */
static OVERLAY_HIGH int
overlay_journal_load(
	struct overlay_mount_state *state)
{
	struct overlay_journal_view *views[2];
	unsigned chosen;
	int error;
	int first_error;
	int second_error;

	/* Rebuilds both slots. */
	views[0] = kern_calloc(1, sizeof(*views[0]));
	views[1] = kern_calloc(1, sizeof(*views[1]));
	if (views[0] == NULL || views[1] == NULL) {
		error = ENOMEM;
		goto out;
	}

	/* Both journal slots have to be there before either can be read. */
	error = overlay_open_journal(state, 0);
	if (error != 0)
		goto out;
	error = overlay_open_journal(state, 1);
	if (error != 0)
		goto out;
	/* Rebuilds each slot; a broken one leaves its view invalid. */
	first_error = overlay_validate_slot(state, 0, views[0]);
	second_error = overlay_validate_slot(state, 1, views[1]);
	if (first_error != 0 && second_error != 0) {
		error = first_error;
		goto out;
	}

	if (!views[0]->valid && !views[1]->valid) {
		error = EINVAL;
		goto out;
	}

	/*
	 * Prefers the newer epoch, then the longer sequence; equal ones must
	 * agree.
	 */
	if (!views[0]->valid) {
		chosen = 1;
	} else if (!views[1]->valid) {
		chosen = 0;
	} else if (views[0]->epoch != views[1]->epoch) {
		if (views[0]->epoch > views[1]->epoch)
			chosen = 0;
		else
			chosen = 1;
	} else if (views[0]->sequence != views[1]->sequence) {
		if (views[0]->sequence > views[1]->sequence)
			chosen = 0;
		else
			chosen = 1;
	} else if (views[0]->digest != views[1]->digest) {
		error = EINVAL;
		goto out;
	} else {
		chosen = 0;
	}

	/* Takes the table, the epoch and the position of the chosen slot. */
	memcpy(state->metadata, views[chosen]->metadata,
	       sizeof(state->metadata));
	state->active_slot = chosen;
	state->epoch = views[chosen]->epoch;
	state->sequence = views[chosen]->sequence;
	state->next_sector = views[chosen]->next_sector;
	error = 0;
out:
	if (views[0] != NULL)
		kern_free(views[0]);
	if (views[1] != NULL)
		kern_free(views[1]);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Writes one journal record. */
static OVERLAY_HIGH int
overlay_write_record(
	struct file *file,
	unsigned sector,
	const uint8_t record[OVERLAY_RECORD_BYTES])
{
	ssize_t count;

	/* Rejects a sector outside the slot. */
	if (sector >= OVERLAY_SLOT_SECTORS)
		return ENOSPC;

	/* Reports a short write as a device error. */
	count = file_pwrite(file, record, OVERLAY_RECORD_BYTES,
		(off_t)(sector * OVERLAY_RECORD_BYTES));
	if (count == OVERLAY_RECORD_BYTES)
		return 0;
	if (count < 0)
		return (int)-count;
	return EIO;
}

/* Counts the used entries of a metadata table. */
static OVERLAY_HIGH unsigned
overlay_metadata_count(
	const struct overlay_metadata entries[OVERLAY_METADATA_MAX])
{
	unsigned i;
	unsigned count;

	/* Counts the entries that are in use. */
	count = 0;
	for (i = 0; i < OVERLAY_METADATA_MAX; i++) {
		if (entries[i].used)
			count++;
	}

	return count;
}

/* Finds the entry with the smallest path after a given one, or -1. */
static OVERLAY_HIGH int
overlay_metadata_sorted_index(
	const struct overlay_metadata entries[OVERLAY_METADATA_MAX],
	const char *after)
{
	int best;
	int difference;
	unsigned i;

	/* Takes the smallest path that sorts after the given one. */
	best = -1;
	for (i = 0; i < OVERLAY_METADATA_MAX; i++) {
		/* An unused slot holds no path to compare against. */
		if (!entries[i].used)
			continue;

		/* Skips every path that does not sort after the given one. */
		if (after != NULL) {
			difference = strcmp(entries[i].path, after);
			if (difference <= 0)
				continue;
		}

		/* The first candidate is the best one so far. */
		if (best < 0) {
			best = (int)i;
			continue;
		}

		/* And any smaller path takes its place. */
		difference = strcmp(entries[i].path, entries[best].path);
		if (difference < 0)
			best = (int)i;
	}

	/* Reports the entry found, or that none follows. */
	return best;
}

/* Writes the whole table as a new epoch into the other journal slot. */
static OVERLAY_HIGH int
overlay_journal_compact_impl(
	struct overlay_mount_state *state)
{
	uint8_t record[OVERLAY_RECORD_BYTES];
	uint32_t digest;
	uint64_t epoch;
	unsigned count;
	unsigned commit_sector;
	unsigned sector;
	unsigned target;
	char previous[ZEDBSD_PATH_MAX];
	const char *after;
	int index;
	int error;
	const uint8_t *id;
	size_t length;

	id = overlay_id(state);

	/* The next epoch must exist and the table must fit the slot. */
	if (state->epoch == UINT64_MAX)
		return ENOSPC;
	epoch = state->epoch + 1U;
	target = state->active_slot ^ 1U;
	count = overlay_metadata_count(state->metadata);
	commit_sector = 1U + count;
	if (commit_sector + 1U >= OVERLAY_SLOT_SECTORS)
		return ENOSPC;

	/* Writes the slot header. */
	memset(record, 0, sizeof(record));

	/* The signature and version a reader identifies the slot by. */
	memcpy(record, "ZOVLSLT\0", 8);
	overlay_put16(record + 8, 1);

	/* How long the header is, and how long each record of the slot is. */
	overlay_put16(record + 0x0a, 48);
	overlay_put32(record + 0x0c, OVERLAY_RECORD_BYTES);

	/* Which mount the slot belongs to. */
	memcpy(record + 0x10, id, 4);

	/* The epoch this slot is being published under. */
	overlay_put64(record + 0x18, epoch);

	/* How many snapshot records follow, and where the commit sits. */
	overlay_put32(record + 0x20, count);
	overlay_put32(record + 0x24, commit_sector);

	/* The sequence number the table stands at. */
	overlay_put64(record + 0x28, state->sequence);

	/* And a checksum over the whole record. */
	overlay_put32(record + 508, overlay_record_crc(record));
	error = overlay_write_record(state->journal[target], 0, record);
	if (error != 0)
		return error;

	/* Writes one snapshot record per entry in path order. */
	digest = overlay_crc_update(0xffffffffU, record, sizeof(record));
	previous[0] = '\0';
	for (sector = 1; sector <= count; sector++) {
		if (sector == 1)
			after = NULL;
		else
			after = previous;
		index = overlay_metadata_sorted_index(state->metadata, after);
		if (index < 0)
			return EIO;
		length = strlen(state->metadata[index].path);
		memset(record, 0, sizeof(record));

		/*
		 * The signature and version a reader identifies the entry by.
		 */
		memcpy(record, "ZOVLSNP\0", 8);
		overlay_put16(record + 8, 1);

		/* Why the path is in the table, and how long the path is. */
		overlay_put16(record + 0x0a, state->metadata[index].flags);
		overlay_put32(record + 0x0c, (uint32_t)length);

		/* The epoch of the slot, and when the entry last changed. */
		overlay_put64(record + 0x10, epoch);
		overlay_put64(record + 0x18,
			state->metadata[index].sequence);

		/* Which mount the entry belongs to. */
		memcpy(record + 0x20, id, 4);

		/* The path itself, which the header gave the length of. */
		memcpy(record + 0x28, state->metadata[index].path, length);

		/* And a checksum over the whole record. */
		overlay_put32(record + 508, overlay_record_crc(record));
		error = overlay_write_record(state->journal[target], sector,
		    record);
		if (error != 0)
			return error;
		digest = overlay_crc_update(digest, record, sizeof(record));
		strcpy(previous, state->metadata[index].path);
	}

	/* Writes the commit record and makes the slot durable. */
	memset(record, 0, sizeof(record));

	/* The signature and version a reader identifies the commit by. */
	memcpy(record, "ZOVLCMT\0", 8);
	overlay_put16(record + 8, 1);

	/* Which mount the slot belongs to. */
	memcpy(record + 0x0c, id, 4);

	/* Every field of the header, repeated so the two can be compared. */
	overlay_put64(record + 0x10, epoch);
	overlay_put32(record + 0x18, count);
	overlay_put32(record + 0x1c, commit_sector);
	overlay_put64(record + 0x20, state->sequence);

	/* The digest that ties the commit to the records just written. */
	overlay_put32(record + 0x28, digest ^ 0xffffffffU);

	/* And a checksum over the whole record. */
	overlay_put32(record + 508, overlay_record_crc(record));
	error = overlay_write_record(state->journal[target], commit_sector,
	    record);
	if (error == 0)
		error = file_fsync(state->journal[target]);
	if (error == 0)
		error = mount_sync_backend(state->upper_root.p_mount);
	if (error != 0)
		return error;

	/* Switches to the new slot. */
	state->active_slot = target;
	state->epoch = epoch;
	state->next_sector = commit_sector + 1U;
	return 0;
}

/* Keeps the logical journal boundary active until all of its work finishes. */
static OVERLAY_HIGH int
overlay_journal_compact(struct overlay_mount_state *state)
{
	int error;

	if (state->owner != NULL)
		io_epoch_begin(&state->owner->m_write_epoch);
	error = overlay_journal_compact_impl(state);
	if (state->owner != NULL)
		io_epoch_end(&state->owner->m_write_epoch);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Appends an operation to the journal and applies it to the table. */
static OVERLAY_HIGH int
overlay_journal_append_impl(
	struct overlay_mount_state *state,
	unsigned opcode,
	const char *path)
{
	uint8_t record[OVERLAY_RECORD_BYTES];
	uint64_t sequence;
	size_t length;
	int index;
	int error;
	unsigned i;

	/* Only a writable overlay journals, and only a relative path. */
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	if (path == NULL || path[0] == '\0' || path[0] == '/')
		return EINVAL;
	length = strlen(path);
	if (length >= ZEDBSD_PATH_MAX || length > OVERLAY_PATH_RECORD_MAX)
		return ENAMETOOLONG;

	/* A new entry needs a free table slot. */
	index = overlay_metadata_find(state->metadata, path);
	if (index < 0 && (opcode == OVERLAY_OP_ADD_WHITEOUT ||
	    opcode == OVERLAY_OP_SET_OPAQUE)) {
		for (i = 0; i < OVERLAY_METADATA_MAX; i++) {
			if (!state->metadata[i].used)
				break;
		}

		if (i == OVERLAY_METADATA_MAX)
			return ENOSPC;
	}

	if (state->sequence == UINT64_MAX)
		return ENOSPC;

	/* A full slot is compacted into the other one first. */
	if (state->next_sector >= OVERLAY_SLOT_SECTORS) {
		error = overlay_journal_compact(state);
		if (error != 0)
			return error;
	}

	/* Writes the record durably, then applies it. */
	sequence = state->sequence + 1U;
	memset(record, 0, sizeof(record));

	/* The signature and version a reader identifies the record by. */
	memcpy(record, "ZOVLOP\0\0", 8);
	overlay_put16(record + 8, 1);

	/* Which change to the table it stands for, and the path it names. */
	overlay_put16(record + 0x0a, (uint16_t)opcode);
	overlay_put32(record + 0x0c, (uint32_t)length);

	/* The epoch of the slot, and the sequence this record continues. */
	overlay_put64(record + 0x10, state->epoch);
	overlay_put64(record + 0x18, sequence);

	/* Which mount the record belongs to. */
	memcpy(record + 0x20, overlay_id(state), 4);

	/* The path itself, which the header gave the length of. */
	memcpy(record + 0x28, path, length);

	/* And a checksum over the whole record. */
	overlay_put32(record + 508, overlay_record_crc(record));
	error = overlay_write_record(state->journal[state->active_slot],
		state->next_sector, record);
	if (error == 0)
		error = file_fsync(state->journal[state->active_slot]);
	if (error != 0)
		return error;
	error = overlay_metadata_apply(state->metadata, path, opcode, sequence);
	if (error != 0)
		return error;
	state->sequence = sequence;
	state->next_sector++;
	state->journal_generation++;
	return 0;
}

/* Keeps the logical journal boundary active until all of its work finishes. */
static OVERLAY_HIGH int
overlay_journal_append(
	struct overlay_mount_state *state,
	unsigned opcode,
	const char *path)
{
	int error;

	if (state->owner != NULL)
		io_epoch_begin(&state->owner->m_write_epoch);
	error = overlay_journal_append_impl(state, opcode, path);
	if (state->owner != NULL)
		io_epoch_end(&state->owner->m_write_epoch);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the overlay information of an inode, or NULL. */
static OVERLAY_HIGH struct overlay_inode_info *
overlay_info(
	const struct inode *inode)
{
	if (inode == NULL)
		return NULL;
	return inode->i_data;
}

/* Reports the slot of an overlay inode, or -1. */
static OVERLAY_HIGH int
overlay_slot_index(
	const struct inode *inode)
{
	unsigned i;

	for (i = 0; i < OVERLAY_INODE_MAX; i++) {
		if (&overlay_inodes[i].inode == inode)
			return (int)i;
	}

	return -1;
}

/* Takes a free inode slot. */
static OVERLAY_HIGH struct inode *
overlay_alloc_inode(
	struct mount *mountp)
{
	unsigned i;

	/* Takes the first free slot of the static table. */
	(void)mountp;
	for (i = 0; i < OVERLAY_INODE_MAX; i++) {
		if (!overlay_inodes[i].used) {
			overlay_inodes[i].used = 1;
			memset(&overlay_inodes[i].info, 0,
			       sizeof(overlay_inodes[i].info));
			return &overlay_inodes[i].inode;
		}
	}

	/* Reports that the table is full. */
	return NULL;
}

/* Returns an inode slot. */
static OVERLAY_HIGH void
overlay_free_inode(
	struct inode *inode)
{
	int index;

	index = overlay_slot_index(inode);
	if (index >= 0)
		memset(&overlay_inodes[index], 0,
		    sizeof(overlay_inodes[index]));
}

/*
 * Selects the upper, lower, or visible path of an inode; the caller holds its
 * lock.
 */
static OVERLAY_HIGH const struct path *
overlay_select_path_locked(
	const struct overlay_inode_info *info,
	enum overlay_path_selection selection)
{
	/*
	 * overlay_inode_info path members are mutable cache state.  The
	 * overlay inode's ordinary lock is their publication lock; consumers
	 * take referenced snapshots and never retain a pointer into the
	 * mutable pair.
	 */
	if (selection == OVERLAY_PATH_UPPER)
		return &info->upper;
	if (selection == OVERLAY_PATH_LOWER)
		return &info->lower;
	if (info->upper.p_inode != NULL)
		return &info->upper;
	return &info->lower;
}

/* Takes a referenced copy of one of an inode's paths. */
static OVERLAY_HIGH int
overlay_path_snapshot(
	struct inode *inode,
	enum overlay_path_selection selection,
	struct path *result)
{
	struct overlay_inode_info *info;
	const struct path *selected;

	/* Rejects a missing result or an inode outside the overlay. */
	if (result == NULL)
		return EINVAL;
	path_init(result);
	info = overlay_info(inode);
	if (info == NULL)
		return EIO;

	/* Copies the selected path under the inode lock. */
	mutex_lock(&inode->i_lock);

	selected = overlay_select_path_locked(info, selection);
	if (selected->p_inode != NULL)
		path_set(result, selected->p_mount, selected->p_inode);

	mutex_unlock(&inode->i_lock);

	if (result->p_inode == NULL)
		return ENOENT;
	return 0;
}

/* Takes referenced copies of an inode's paths and its relative path text. */
static OVERLAY_HIGH int
overlay_info_snapshot(
	struct inode *inode,
	struct path *upper,
	struct path *lower,
	char relative[ZEDBSD_PATH_MAX])
{
	struct overlay_inode_info *info;

	info = overlay_info(inode);

	/* Clears the results before checking the inode. */
	if (upper != NULL)
		path_init(upper);
	if (lower != NULL)
		path_init(lower);
	if (info == NULL)
		return EIO;

	/* Copies everything asked for under the inode lock. */
	mutex_lock(&inode->i_lock);

	/* Reports only the parts of the pair the caller asked for. */
	if (upper != NULL && info->upper.p_inode != NULL)
		path_set(upper, info->upper.p_mount, info->upper.p_inode);
	if (lower != NULL && info->lower.p_inode != NULL)
		path_set(lower, info->lower.p_mount, info->lower.p_inode);
	if (relative != NULL)
		strcpy(relative, info->path);

	mutex_unlock(&inode->i_lock);

	return 0;
}

/* Tests whether a name is one of the overlay's temporary names, ovXXXX.tmp. */
static OVERLAY_HIGH int
overlay_temporary_name(
	const char *name)
{
	unsigned i;
	size_t length;
	int suffix;

	/* A call that names nothing has no name to test. */
	if (name == NULL)
		return 0;

	/* Every temporary name is exactly ten characters long. */
	length = strlen(name);
	if (length != 10U)
		return 0;

	/* It begins with the two letters that mark it as the overlay's. */
	if (name[0] != 'o' || name[1] != 'v')
		return 0;

	/* And it ends with the suffix that marks it as temporary. */
	suffix = strcmp(name + 6, ".tmp");
	if (suffix != 0)
		return 0;

	/* Requires the four middle characters to be hexadecimal. */
	for (i = 2; i < 6; i++) {
		if (!((name[i] >= '0' && name[i] <= '9') ||
		      (name[i] >= 'a' && name[i] <= 'f')))
			return 0;
	}

	/* Reports that the name is one this driver makes. */
	return 1;
}

/* Tests whether a name is reserved for the journal or temporaries. */
static OVERLAY_HIGH int
overlay_reserved_name(
	const char *name)
{
	int difference;
	int temporary;

	/* A call that names nothing has no name to test. */
	if (name == NULL)
		return 0;

	/* The two journal slots each have a name of their own. */
	difference = strcmp(name, ".zovl0");
	if (difference == 0)
		return 1;

	difference = strcmp(name, ".zovl1");
	if (difference == 0)
		return 1;

	/* And a name the overlay uses while replacing a file is reserved. */
	temporary = overlay_temporary_name(name);
	if (temporary)
		return 1;

	/* Reports that the name is the caller's to use. */
	return 0;
}

/* Copies a component into a terminated buffer. */
static OVERLAY_HIGH int
overlay_component_text(
	const struct componentname *component,
	char name[NAME_MAX + 1U])
{
	if (component == NULL ||
	    component->cn_namelen == 0 ||
	    component->cn_namelen > NAME_MAX)
		return EINVAL;
	memcpy(name, component->cn_nameptr, component->cn_namelen);
	name[component->cn_namelen] = '\0';
	return 0;
}

/* Joins a relative parent path and a name. */
static OVERLAY_HIGH int
overlay_join(
	const char *parent,
	const char *name,
	char result[ZEDBSD_PATH_MAX])
{
	size_t parent_length;
	size_t name_length;
	size_t separator_length;
	const char *separator;

	parent_length = strlen(parent);
	name_length = strlen(name);

	/* The root needs no separator; the result must fit. */
	if (parent_length != 0)
		separator_length = 1;
	else
		separator_length = 0;
	/* A component with no name, or one that is itself a path. */
	separator = strchr(name, '/');
	if (name_length == 0 || separator != NULL)
		return ENAMETOOLONG;

	/* And a result the caller buffer could not hold. */
	if (parent_length + separator_length + name_length >= ZEDBSD_PATH_MAX)
		return ENAMETOOLONG;
	memcpy(result, parent, parent_length);
	if (parent_length != 0) {
		result[parent_length] = '/';
		parent_length++;
	}

	memcpy(result + parent_length, name, name_length + 1U);
	return 0;
}

/* Finds or allocates the stable inode number of a relative path. */
static OVERLAY_HIGH int
overlay_identity_get(
	struct overlay_mount_state *state,
	const char *path,
	unsigned *index_out,
	ino_t *ino_out,
	int *created_out)
{
	unsigned i;
	unsigned free_index;
	int difference;

	free_index = OVERLAY_IDENTITY_MAX;

	/* Reuses an active identity, remembering the first free slot. */
	if (created_out != NULL)
		*created_out = 0;
	for (i = 0; i < OVERLAY_IDENTITY_MAX; i++) {
		/* Compares only the slots that still name a live path. */
		difference = 1;
		if (state->identities[i].state == OVERLAY_ID_ACTIVE)
			difference = strcmp(state->identities[i].path, path);

		/* An identity already made for this path is reused. */
		if (difference == 0) {
			*index_out = i;
			*ino_out = state->identities[i].ino;
			return 0;
		}

		if (free_index == OVERLAY_IDENTITY_MAX &&
		    state->identities[i].state == OVERLAY_ID_FREE)
			free_index = i;
	}

	/* Allocates a new identity with the next inode number. */
	if (free_index == OVERLAY_IDENTITY_MAX || state->next_ino == 0)
		return ENOSPC;
	state->identities[free_index].state = OVERLAY_ID_ACTIVE;
	state->identities[free_index].ino = state->next_ino;
	state->next_ino++;
	strcpy(state->identities[free_index].path, path);
	*index_out = free_index;
	*ino_out = state->identities[free_index].ino;
	if (created_out != NULL)
		*created_out = 1;
	return 0;
}

/* Looks a component up in a real layer directory. */
static OVERLAY_HIGH int
overlay_lookup_real(
	const struct path *directory,
	const struct componentname *component,
	struct path *result)
{
	struct inode *inode;
	int error;

	/* Rejects a directory that has no inode. */
	path_init(result);
	if (directory->p_inode == NULL)
		return ENOENT;

	/* Resolves the component against the real directory. */
	error = inode_lookup(directory->p_inode, component, &inode);
	if (error != 0)
		return error;

	/* Hands the path back with the caller holding the reference. */
	path_set(result, directory->p_mount, inode);
	inode_release(inode);
	return 0;
}

/*
 * Copies the visible layer's attributes into the overlay inode; the caller
 * holds its lock.
 */
static OVERLAY_HIGH void
overlay_refresh_locked(
	struct inode *inode)
{
	struct overlay_inode_info *info;
	const struct path *visible_path;
	const struct inode *visible;

	info = overlay_info(inode);

	/* An inode without a visible layer keeps its old attributes. */
	visible_path = overlay_select_path_locked(info, OVERLAY_PATH_VISIBLE);
	visible = visible_path->p_inode;
	if (visible == NULL)
		return;
	inode->i_type = visible->i_type;
	inode->i_mode = visible->i_mode;
	inode->i_linkcount = visible->i_linkcount;
	inode->i_uid = visible->i_uid;
	inode->i_gid = visible->i_gid;
	inode->i_size = visible->i_size;
	inode->i_rdev = visible->i_rdev;
	inode->i_atime = visible->i_atime;
	inode->i_mtime = visible->i_mtime;
	inode->i_ctime = visible->i_ctime;
#ifndef ZEDBSD_OVERLAY_CONTENT_HOST_TEST
	inode->i_op = &overlay_inode_ops;
	if (inode->i_type == INODE_DIR)
		inode->i_fop = &overlay_directory_ops;
	else if (inode->i_type == INODE_REG)
		inode->i_fop = &overlay_regular_ops;
	else if (inode->i_type == INODE_FIFO)
		inode->i_fop = &fifo_file_ops;
	else
		inode->i_fop = NULL;
#endif
}

/* Copies the visible layer's attributes into the overlay inode. */
static OVERLAY_HIGH void
overlay_refresh(
	struct inode *inode)
{
	struct overlay_inode_info *info;

	/* An inode this driver did not make has nothing to refresh from. */
	info = NULL;
	if (inode != NULL)
		info = overlay_info(inode);
	if (info == NULL)
		return;

	mutex_lock(&inode->i_lock);

	overlay_refresh_locked(inode);

	mutex_unlock(&inode->i_lock);
}

/*
 * Finds or creates the overlay inode of a relative path with its layer paths.
 */
static OVERLAY_HIGH int
overlay_make_inode(
	struct mount *mountp,
	const char *relative,
	struct path *upper,
	struct path *lower,
	struct inode **result)
{
	struct overlay_mount_state *state;
	struct overlay_inode_info *info;
	struct inode *inode;
	unsigned identity;
	ino_t ino;
	int cached;
	int error;
	int identity_created;
	int slot;

	state = mountp->m_data;

	/* A cached inode is refreshed and returned. */
	error = overlay_identity_get(state, relative, &identity, &ino,
	    &identity_created);
	if (error != 0)
		return error;
	/* Asks the inode cache whether this identity is already in core. */
	cached = inode_get(mountp, ino, result);
	if (cached == 0) {
		info = overlay_info(*result);
		if (info == NULL) {
			inode_release(*result);
			*result = NULL;
			return EIO;
		}

		/*
		 * The cached inode owns its path pair.  Overlay mutations
		 * update it explicitly under i_lock; ordinary lookup must not
		 * release and replace those references while readers are
		 * taking snapshots.  Direct external mutation of the private
		 * upper/lower mounts is outside the contract.
		 */
		overlay_refresh(*result);
		return 0;
	}

	/* Allocates a new inode, releasing a fresh identity on failure. */
	inode = inode_alloc(mountp);
	if (inode == NULL) {
		if (identity_created)
			memset(&state->identities[identity], 0,
			    sizeof(state->identities[identity]));
		return ENOSPC;
	}

	/* Which of the driver's fixed inode slots this one occupies. */
	slot = overlay_slot_index(inode);
	if (slot < 0) {
		inode_release(inode);
		if (identity_created)
			memset(&state->identities[identity], 0,
			    sizeof(state->identities[identity]));
		return EIO;
	}

	/* Records the layer paths and the identity. */
	info = &overlay_inodes[slot].info;
	path_init(&info->upper);
	path_init(&info->lower);
	if (upper != NULL && upper->p_inode != NULL)
		path_set(&info->upper, upper->p_mount, upper->p_inode);
	if (lower != NULL && lower->p_inode != NULL)
		path_set(&info->lower, lower->p_mount, lower->p_inode);
	info->identity_index = identity;
	strcpy(info->path, relative);
	inode->i_ino = ino;
	inode->i_data = info;
	overlay_refresh(inode);
	*result = inode;
	return 0;
}

/* Looks a name up in the merged view of a directory. */
static OVERLAY_HIGH int
overlay_lookup(
	struct inode *directory,
	const struct componentname *component,
	struct inode **result)
{
	struct path upper_directory;
	struct path lower_directory;
	struct path upper;
	struct path lower;
	struct path *upper_argument;
	struct path *lower_argument;
	char name[NAME_MAX + 1U];
	char parent_path[ZEDBSD_PATH_MAX];
	char relative[ZEDBSD_PATH_MAX];
	struct overlay_inode_info *info;
	unsigned parent_flags;
	unsigned name_flags;
	int opaque_parent;
	int whiteout_name;
	int reserved;
	int upper_error;
	int lower_error;
	int error;
	char *slash;

	/* Rejects anything but a directory this driver made. */
	info = overlay_info(directory);
	if (directory->i_type != INODE_DIR || info == NULL)
		return ENOTDIR;

	/* Dot is the directory itself. */
	if (component->cn_namelen == 1U && component->cn_nameptr[0] == '.') {
		inode_ref(directory);
		*result = directory;
		return 0;
	}

	error = overlay_info_snapshot(directory, &upper_directory,
	    &lower_directory, parent_path);
	if (error != 0)
		return error;

	/* Dot-dot re-resolves the parent's relative path from the root. */
	if (component->cn_namelen == 2U &&
	    component->cn_nameptr[0] == '.' &&
	    component->cn_nameptr[1] == '.') {
		if (parent_path[0] == '\0') {
			inode_ref(directory->i_mount->m_root);
			*result = directory->i_mount->m_root;
			error = 0;
			goto out_directories;
		}

		slash = strrchr(parent_path, '/');
		if (slash == NULL)
			parent_path[0] = '\0';
		else
			*slash = '\0';
		error = overlay_find_relative(directory->i_mount, parent_path,
		    result);
		goto out_directories;
	}

	/* Reserved names never exist. */
	error = overlay_component_text(component, name);
	if (error != 0)
		goto out_directories;
	/* A name the overlay keeps for itself never resolves to anything. */
	reserved = overlay_reserved_name(name);
	if (reserved) {
		error = ENOENT;
		goto out_directories;
	}

	error = overlay_join(parent_path, name, relative);
	if (error != 0)
		goto out_directories;

	/* Looks in the upper layer, then in the lower unless it is hidden. */
	path_init(&upper);
	path_init(&lower);
	upper_error = overlay_lookup_real(&upper_directory, component, &upper);

	/*
	 * An opaque parent hides every lower child of it, and a whiteout
	 * hides the one name it was recorded for.  Either way the lower layer
	 * is not looked in at all.
	 */
	parent_flags = overlay_metadata_flags(directory->i_mount->m_data,
					      parent_path);
	name_flags = overlay_metadata_flags(directory->i_mount->m_data,
					    relative);
	opaque_parent = (parent_flags & OVERLAY_META_OPAQUE) != 0;
	whiteout_name = (name_flags & OVERLAY_META_WHITEOUT) != 0;

	/* Decides whether the lower layer is looked in at all. */
	if (upper_error == 0 && opaque_parent) {
		lower_error = ENOENT;
	} else if (upper_error != 0 && (whiteout_name || opaque_parent)) {
		lower_error = ENOENT;
	} else {
		lower_error = overlay_lookup_real(&lower_directory, component,
		    &lower);
	}

	/* Combines the two results. */
	if (upper_error != 0 && upper_error != ENOENT) {
		error = upper_error;
	} else if (lower_error != 0 && lower_error != ENOENT) {
		error = lower_error;
	} else if (upper_error != 0 && lower_error != 0) {
		error = ENOENT;
	} else {
		/* A non-directory upper hides every lower object. */
		if (upper.p_inode != NULL && upper.p_inode->i_type != INODE_DIR)
			path_release(&lower);
		else if (upper.p_inode != NULL && lower.p_inode != NULL &&
			 lower.p_inode->i_type != INODE_DIR)
			path_release(&lower);
		if (upper.p_inode != NULL)
			upper_argument = &upper;
		else
			upper_argument = NULL;
		if (lower.p_inode != NULL)
			lower_argument = &lower;
		else
			lower_argument = NULL;
		error = overlay_make_inode(directory->i_mount, relative,
			upper_argument, lower_argument, result);
	}

	path_release(&upper);
	path_release(&lower);
out_directories:
	path_release(&upper_directory);
	path_release(&lower_directory);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the visible layer's attributes under the overlay's inode number. */
static OVERLAY_HIGH int
overlay_getattr(
	struct inode *inode,
	struct stat *status)
{
	struct path visible;
	int error;

	/* Rejects a missing result or an inode without a visible layer. */
	if (status == NULL)
		return EINVAL;
	error = overlay_path_snapshot(inode, OVERLAY_PATH_VISIBLE, &visible);
	if (error != 0) {
		if (error == ENOENT)
			return EIO;
		return error;
	}

	/* Takes the real attributes and replaces the identity. */
	error = inode_getattr(visible.p_inode, status);
	path_release(&visible);
	if (error == 0) {
		overlay_refresh(inode);
		status->st_ino = inode->i_ino;
		status->st_dev = 0;
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Resolves a relative path from the overlay root through overlay lookups. */
static OVERLAY_HIGH int
overlay_find_relative(
	struct mount *mountp,
	const char *relative,
	struct inode **result)
{
	struct inode *current;
	struct inode *next;
	const char *at;
	struct componentname component;
	const char *end;
	int error;

	at = relative;

	/* Rejects a missing operand. */
	if (mountp == NULL || relative == NULL || result == NULL)
		return EINVAL;

	/* The empty path is the root. */
	current = mountp->m_root;
	inode_ref(current);
	if (*at == '\0') {
		*result = current;
		return 0;
	}

	/* Walks one component at a time. */
	while (*at != '\0') {
		end = strchr(at, '/');
		component.cn_nameptr = at;
		if (end != NULL)
			component.cn_namelen = (size_t)(end - at);
		else
			component.cn_namelen = strlen(at);
		if (end == NULL)
			component.cn_flags = COMPONENT_LAST;
		else
			component.cn_flags = 0;
		error = inode_lookup(current, &component, &next);
		inode_release(current);
		if (error != 0)
			return error;
		current = next;
		if (end == NULL)
			break;
		at = end + 1;
	}

	*result = current;
	return 0;
}

/* Splits a relative path into its parent path and last component. */
static OVERLAY_HIGH int
overlay_split_path(
	const char *path,
	char parent[ZEDBSD_PATH_MAX],
	struct componentname *name)
{
	const char *slash;
	size_t length;

	/* Rejects a missing or empty path, or a missing result. */
	if (path == NULL || path[0] == '\0' || name == NULL)
		return EINVAL;

	/* Everything before the last slash is the parent. */
	slash = strrchr(path, '/');
	if (slash == NULL) {
		parent[0] = '\0';
		name->cn_nameptr = path;
	} else {
		length = (size_t)(slash - path);
		memcpy(parent, path, length);
		parent[length] = '\0';
		name->cn_nameptr = slash + 1;
	}

	name->cn_namelen = strlen(name->cn_nameptr);
	name->cn_flags = COMPONENT_LAST;
	if (name->cn_namelen == 0)
		return EINVAL;
	return 0;
}

/* Publishes a new upper path on an inode, optionally dropping the lower one. */
static OVERLAY_HIGH void
overlay_publish_upper(
	struct inode *inode,
	const struct path *upper,
	int clear_lower,
	const char *relative)
{
	struct overlay_inode_info *info;
	struct path replacement;
	struct path old_upper;
	struct path old_lower;

	info = overlay_info(inode);

	/* Ignores an inode outside the overlay or a missing upper. */
	path_init(&replacement);
	path_init(&old_upper);
	path_init(&old_lower);
	if (info == NULL || upper == NULL || upper->p_inode == NULL)
		return;

	/*
	 * Take the replacement references before entering the publication
	 * lock; release displaced references only after readers can no
	 * longer select them.
	 */
	path_set(&replacement, upper->p_mount, upper->p_inode);
	mutex_lock(&inode->i_lock);

	/* Takes the old pair aside so it can be released after the lock. */
	old_upper = info->upper;
	info->upper = replacement;
	path_init(&replacement);
	if (clear_lower) {
		old_lower = info->lower;
		path_init(&info->lower);
	}

	if (relative != NULL)
		strcpy(info->path, relative);
	overlay_refresh_locked(inode);

	mutex_unlock(&inode->i_lock);

	path_release(&old_upper);
	path_release(&old_lower);
	path_release(&replacement);
}

/* Publishes a new upper path on an inode, keeping its lower one. */
static OVERLAY_HIGH void
overlay_install_upper(
	struct inode *inode,
	const struct path *upper)
{
	overlay_publish_upper(inode, upper, 0, NULL);
}

/* Drops an inode's upper path when it is still the expected one. */
static OVERLAY_HIGH void
overlay_clear_upper_if(
	struct inode *inode,
	const struct path *expected)
{
	struct overlay_inode_info *info;
	struct path removed;

	info = overlay_info(inode);

	/* Ignores an inode outside the overlay or a missing expectation. */
	path_init(&removed);
	if (info == NULL || expected == NULL)
		return;

	/* Clears under the lock and releases outside it. */
	mutex_lock(&inode->i_lock);

	/* Clears the upper only when it is still the one that was published. */
	if (info->upper.p_mount == expected->p_mount &&
	    info->upper.p_inode == expected->p_inode) {
		removed = info->upper;
		path_init(&info->upper);
		overlay_refresh_locked(inode);
	}

	mutex_unlock(&inode->i_lock);

	path_release(&removed);
}

/*
 * Finishes a materialization transaction, removing its directories on failure.
 */
static OVERLAY_HIGH int
overlay_materialization_complete(
	struct overlay_mount_state *state,
	struct overlay_materialization_transaction *transaction,
	int error)
{
	struct overlay_materialization_entry *entry;
	struct overlay_materialization_entry *next;
	int cleanup_error;
	int one_error;
	struct componentname name;

	/*
	 * A leaf operation may need to materialize several lower-only
	 * ancestors.  Keep those allocations provisional until the leaf
	 * commits.  On failure, remove them from deepest to shallowest and
	 * preserve any entry whose rmdir failed as the authoritative upper
	 * while quarantining the mount read-only.
	 */

	/*
	 * A prior leaf rollback may already have quarantined the mount.
	 * Preserve that first cleanup errno while still attempting every
	 * ancestor cleanup.
	 */
	if (state->flags == OVERLAY_READ_ONLY)
		cleanup_error = error;
	else
		cleanup_error = 0;

	/* Removes each created directory on failure and frees the entries. */
	for (entry = transaction->created; entry != NULL; entry = next) {
		next = entry->next;
		if (error != 0) {
			name.cn_nameptr = entry->name;
			name.cn_namelen = strlen(entry->name);
			name.cn_flags = COMPONENT_LAST;
			one_error = inode_rmdir(entry->parent_upper.p_inode,
			    &name);
			if (one_error == 0)
				overlay_clear_upper_if(entry->directory,
				    &entry->created_upper);
			if (cleanup_error == 0)
				cleanup_error = one_error;
			one_error = mount_sync_backend(
				entry->parent_upper.p_mount);
			if (cleanup_error == 0)
				cleanup_error = one_error;
		}

		path_release(&entry->parent_upper);
		path_release(&entry->created_upper);
		inode_release(entry->directory);
		kern_free(entry);
	}

	transaction->created = NULL;
	transaction->count = 0;

	/* A failed cleanup quarantines the mount and is the reported error. */
	if (cleanup_error != 0) {
		state->flags = OVERLAY_READ_ONLY;
		return cleanup_error;
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Makes sure a directory exists in the upper layer, recording new ones. */
static OVERLAY_HIGH int
overlay_ensure_upper_dir_tracked(
	struct inode *directory,
	struct overlay_materialization_transaction *transaction)
{
	struct overlay_mount_state *state;
	struct overlay_materialization_entry *pending;
	struct inode *parent;
	struct inode *created;
	struct inode *source;
	struct inode_creation_request request;
	struct path upper;
	struct path lower;
	struct path parent_upper;
	struct componentname name;
	char relative[ZEDBSD_PATH_MAX];
	char parent_path[ZEDBSD_PATH_MAX];
	struct overlay_inode_info *info;
	int error;
	int cleanup_error;
	int sync_error;
	int created_new;
	struct path created_path;

	pending = NULL;
	created_new = 0;

	/* Rejects anything but a directory this driver made. */
	info = overlay_info(directory);
	if (info == NULL || directory->i_type != INODE_DIR)
		return ENOTDIR;

	state = directory->i_mount->m_data;
	error = overlay_info_snapshot(directory, &upper, &lower, relative);
	if (error != 0)
		return error;

	/* An existing upper must be a directory; the root always has one. */
	if (upper.p_inode != NULL) {
		if (upper.p_inode->i_type == INODE_DIR)
			error = 0;
		else
			error = ENOTDIR;
		goto out_paths;
	}

	if (directory == directory->i_mount->m_root) {
		error = EIO;
		goto out_paths;
	}

	/* Materializes the parent first, then creates this directory. */
	error = overlay_split_path(relative, parent_path, &name);
	if (error != 0)
		goto out_paths;
	error = overlay_find_relative(directory->i_mount, parent_path, &parent);
	if (error != 0)
		goto out_paths;
	error = overlay_ensure_upper_dir_tracked(parent, transaction);
	if (error == 0) {
		if (lower.p_inode != NULL)
			source = lower.p_inode;
		else
			source = directory;
		error = inode_creation_request_preserve(source, &request);
	}

	/* Takes the parent's upper directory, which the new one is made in. */
	path_init(&parent_upper);
	if (error == 0)
		error = overlay_path_snapshot(parent, OVERLAY_PATH_UPPER,
		    &parent_upper);
	if (error == 0 && transaction != NULL) {
		if (transaction->count >= OVERLAY_MATERIALIZATION_MAX) {
			error = ENAMETOOLONG;
		} else {
			pending = kern_calloc(1, sizeof(*pending));
			if (pending == NULL)
				error = ENOMEM;
		}
	}

	/* Makes the directory, or takes the one that is already there. */
	if (error == 0)
		error = inode_mkdir(parent_upper.p_inode, &name,
			&request, &created);
	if (error == 0)
		created_new = 1;
	if (error == EEXIST) {
		if (pending != NULL) {
			kern_free(pending);
			pending = NULL;
		}

		error = inode_lookup(parent_upper.p_inode, &name,
			&created);
	}

	if (error == 0) {
		/*
		 * A newly materialized directory is not published to the
		 * overlay inode until its upper namespace entry is durable.
		 * Otherwise a failed sync leaves a visible upper directory
		 * after returning an error.  An EEXIST lookup observes an
		 * already committed directory and needs no new durability
		 * transaction.
		 */
		path_init(&created_path);
		path_set(&created_path, parent_upper.p_mount, created);
		if (created_new)
			error = mount_sync_backend(parent_upper.p_mount);
		if (error == 0) {
			overlay_install_upper(directory, &created_path);
			if (created_new && transaction != NULL) {
				pending->directory = directory;
				inode_ref(directory);
				path_init(&pending->parent_upper);
				path_set(&pending->parent_upper,
				    parent_upper.p_mount, parent_upper.p_inode);
				path_init(&pending->created_upper);
				path_set(&pending->created_upper,
				    created_path.p_mount, created_path.p_inode);
				memcpy(pending->name, name.cn_nameptr,
				    name.cn_namelen);
				pending->name[name.cn_namelen] = '\0';
				pending->next = transaction->created;
				transaction->created = pending;
				transaction->count++;
				pending = NULL;
			}
		} else if (created_new) {
			/*
			 * A failed rmdir leaves the complete created directory
			 * in the upper namespace.  Keep it authoritative before
			 * quarantining the mount; a successful rmdir leaves no
			 * live entry to publish even if the following
			 * durability sync fails.
			 */
			cleanup_error = inode_rmdir(parent_upper.p_inode,
			    &name);
			if (cleanup_error != 0)
				overlay_install_upper(directory, &created_path);
			sync_error = mount_sync_backend(parent_upper.p_mount);
			if (cleanup_error == 0)
				cleanup_error = sync_error;
			if (cleanup_error != 0) {
				state->flags = OVERLAY_READ_ONLY;
				error = cleanup_error;
			}
		}

		path_release(&created_path);
		inode_release(created);
	}

	/* Gives back everything this call took a reference to. */
	path_release(&parent_upper);
	inode_release(parent);
out_paths:
	if (pending != NULL)
		kern_free(pending);
	path_release(&upper);
	path_release(&lower);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Makes sure a directory exists in the upper layer as its own transaction. */
static OVERLAY_HIGH int
overlay_ensure_upper_dir(
	struct inode *directory)
{
	struct overlay_materialization_transaction transaction;
	struct overlay_mount_state *state;
	int error;

	/* Materializes the directory inside one transaction. */
	transaction.created = NULL;
	transaction.count = 0U;
	state = directory->i_mount->m_data;
	error = overlay_ensure_upper_dir_tracked(directory, &transaction);

	/* Reports the failure. */
	error = overlay_materialization_complete(state, &transaction, error);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Formats a temporary name, ovXXXX.tmp, from a counter. */
static OVERLAY_HIGH void
overlay_temp_name(
	uint16_t number,
	char name[11])
{
	static const char hex[] = "0123456789abcdef";

	/* Renders the number as the fixed temporary-name layout. */
	name[0] = 'o';
	name[1] = 'v';
	name[2] = hex[(number >> 12) & 15U];
	name[3] = hex[(number >> 8) & 15U];
	name[4] = hex[(number >> 4) & 15U];
	name[5] = hex[number & 15U];
	name[6] = '.';
	name[7] = 't';
	name[8] = 'm';
	name[9] = 'p';
	name[10] = '\0';
}

/* Copies a lower regular file into the upper layer under its final name. */
static OVERLAY_HIGH int
overlay_copy_up_regular(
	struct inode *inode)
{
	struct overlay_mount_state *state;
	struct overlay_materialization_transaction materialization;
	struct inode *parent;
	struct inode *temp_inode;
	struct file *source;
	struct file *destination;
	struct path upper;
	struct path lower;
	struct path parent_upper;
	struct path temp_path;
	struct path final_path;
	struct componentname final_name;
	struct componentname temp_name_component;
	struct inode_creation_request request;
	char relative[ZEDBSD_PATH_MAX];
	char parent_path[ZEDBSD_PATH_MAX];
	char temp_name[11];
	uint8_t *buffer;
	off_t offset;
	int error;
	int cleanup_error;
	int original_error;
	int renamed;
	int final_removed;
	int retain_materialization;
	int entered_transaction;
	int io_owned;
	int transaction_owned;
	unsigned attempts;
	uint16_t number;
	size_t wanted;
	ssize_t count;
	ssize_t written;
	int close_error;
	int sync_error;

	/* Starts with nothing opened, created, or renamed. */
	state = inode->i_mount->m_data;
	materialization.created = NULL;
	materialization.count = 0U;
	parent = NULL;
	temp_inode = NULL;
	source = NULL;
	destination = NULL;
	buffer = NULL;
	offset = 0;
	error = 0;
	renamed = 0;
	final_removed = 0;
	retain_materialization = 0;
	entered_transaction = 0;

	/* Rejects a read-only overlay. */
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	/*
	 * Generic preparation has already committed the upper before taking
	 * i_io_lock.  Inner metadata and truncate calls must not reacquire
	 * namespace.
	 * Callers without i_io still join the gate below: another namespace
	 * operation may have published provisional ancestors pending rollback.
	 */
	io_owned = mutex_owned(&inode->i_io_lock);
	if (io_owned) {
		error = overlay_path_snapshot(inode, OVERLAY_PATH_UPPER,
					      &upper);
		if (error == 0)
			path_release(&upper);

		/* A caller inside i_io_lock could not wait for the copy-up. */
		if (error == ENOENT)
			return EDEADLK;

		/* Reports what the snapshot found. */
		return error;
	}

	path_init(&upper);
	path_init(&lower);
	path_init(&parent_upper);
	path_init(&temp_path);
	path_init(&final_path);

	/*
	 * Ordinary namespace syscalls already own this gate.  A writable open
	 * or truncate may enter copy-up after path resolution, so join the
	 * same gate here to exclude create/unlink/rename of the final name.
	 */
	transaction_owned = mutex_owned(inode->i_mount->m_vfs_transaction_lock);
	if (!transaction_owned) {
		mount_vfs_transaction_enter(inode->i_mount);
		entered_transaction = 1;
	}

	mutex_lock(&state->copy_up_lock);

	/*
	 * A cleanup failure may have quarantined the mount while this caller
	 * waited for the namespace/copy-up gates.  The check above is only a
	 * fast path; revalidate the authoritative state before creating any
	 * upper object.
	 */
	if (state->flags != OVERLAY_READ_WRITE) {
		error = EROFS;
		goto out;
	}

	/*
	 * The first waiter may have completed the copy while this caller slept.
	 */
	error = overlay_info_snapshot(inode, &upper, &lower, relative);
	if (error != 0)
		goto out;
	if (upper.p_inode != NULL) {
		error = 0;
		goto out;
	}

	if (lower.p_inode == NULL || lower.p_inode->i_type != INODE_REG) {
		error = EINVAL;
		goto out;
	}

	/* Materializes the parent and prepares a preserving creation. */
	error = overlay_split_path(relative, parent_path, &final_name);
	if (error == 0)
		error = overlay_find_relative(inode->i_mount, parent_path,
		    &parent);
	if (error == 0)
		error = overlay_ensure_upper_dir_tracked(parent,
		    &materialization);
	if (error == 0)
		error = overlay_path_snapshot(parent, OVERLAY_PATH_UPPER,
		    &parent_upper);
	if (error == 0)
		error = inode_creation_request_preserve(lower.p_inode,
		    &request);
	if (error != 0)
		goto out;

	/* Creates the copy under a fresh temporary name. */
	for (attempts = 0; attempts < 65536U; attempts++) {
		number = state->temp_counter;
		state->temp_counter++;
		overlay_temp_name(number, temp_name);
		temp_name_component.cn_nameptr = temp_name;
		temp_name_component.cn_namelen = 10;
		temp_name_component.cn_flags = COMPONENT_LAST;
		error = inode_create(parent_upper.p_inode,
			&temp_name_component, &request, &temp_inode);
		if (error == 0)
			break;
		if (error != EEXIST)
			goto out;
	}

	if (temp_inode == NULL) {
		error = ENOSPC;
		goto out;
	}

	/* Copies the contents. */
	path_set(&temp_path, parent_upper.p_mount, temp_inode);
	error = file_open_resolved(&lower, O_RDONLY, &source);
	if (error == 0)
		error = file_open_resolved(&temp_path, O_RDWR, &destination);
	buffer = kern_malloc(4096U);
	if (error == 0 && buffer == NULL)
		error = ENOMEM;
	while (error == 0 && offset < lower.p_inode->i_size) {
		wanted = (size_t)(lower.p_inode->i_size - offset);
		if (wanted > 4096U)
			wanted = 4096U;
		count = file_pread(source, buffer, wanted, offset);
		if (count != (ssize_t)wanted) {
			if (count < 0)
				error = (int)-count;
			else
				error = EIO;
			break;
		}

		/* Writes the block just read into the upper copy. */
		written = file_pwrite(destination, buffer, wanted, offset);
		if (written != (ssize_t)wanted) {
			if (written < 0)
				error = (int)-written;
			else
				error = ENOSPC;
			break;
		}

		offset += (off_t)wanted;
	}

	/*
	 * Writes may update timestamps and clear set-id bits.  Reapply the
	 * PRESERVE contract while the reserved temporary name is still hidden.
	 */
	if (error == 0)
		error = inode_creation_prepare(
		    parent_upper.p_inode, temp_inode, &request);
	if (error == 0)
		error = file_fsync(destination);
	if (destination != NULL) {
		close_error = file_close(destination);
		destination = NULL;
		if (error == 0)
			error = close_error;
	}

	if (source != NULL) {
		(void)file_close(source);
		source = NULL;
	}

	/* Renames the copy into place and makes it durable. */
	if (error == 0) {
		error = inode_rename(parent_upper.p_inode,
			&temp_name_component, parent_upper.p_inode,
			&final_name, 0);
		if (error == 0)
			renamed = 1;
	}

	/* A copy that reached its final name is published once durable. */
	if (renamed) {
		error = mount_sync_backend(parent_upper.p_mount);
		if (error == 0) {
			path_set(&final_path, parent_upper.p_mount, temp_inode);
			overlay_install_upper(inode, &final_path);
		} else {
			/*
			 * If unlink itself failed, the complete renamed upper
			 * is still authoritative.  Publish it before freezing
			 * so readers never select stale lower contents.  A
			 * removed-but-not-durable name remains unpublished in
			 * the live namespace.
			 */
			original_error = error;
			cleanup_error = inode_unlink(parent_upper.p_inode,
				&final_name);
			if (cleanup_error == 0)
				final_removed = 1;
			sync_error = mount_sync_backend(parent_upper.p_mount);
			if (cleanup_error == 0)
				cleanup_error = sync_error;
			if (cleanup_error == 0) {
				error = original_error;
			} else {
				if (!final_removed) {
					path_set(&final_path,
					    parent_upper.p_mount,
					    temp_inode);
					overlay_install_upper(inode,
					    &final_path);
					retain_materialization = 1;
				}

				state->flags = OVERLAY_READ_ONLY;
				error = cleanup_error;
			}
		}
	}

out:
	/* Removes a temporary that was never renamed. */
	if (buffer != NULL)
		kern_free(buffer);
	if (destination != NULL)
		(void)file_close(destination);
	if (source != NULL)
		(void)file_close(source);
	if (!renamed && temp_inode != NULL && parent_upper.p_inode != NULL) {
		cleanup_error = inode_unlink(parent_upper.p_inode,
			&temp_name_component);
		sync_error = mount_sync_backend(parent_upper.p_mount);
		if (cleanup_error == 0)
			cleanup_error = sync_error;
		if (cleanup_error != 0) {
			state->flags = OVERLAY_READ_ONLY;
			error = cleanup_error;
		}
	}

	/* Settles the materialized ancestors and releases everything. */
	if (retain_materialization)
		(void)overlay_materialization_complete(state, &materialization,
		    0);
	else
		error = overlay_materialization_complete(state,
		    &materialization, error);
	if (temp_inode != NULL)
		inode_release(temp_inode);
	if (parent != NULL)
		inode_release(parent);
	path_release(&upper);
	path_release(&lower);
	path_release(&parent_upper);
	path_release(&temp_path);
	path_release(&final_path);

	mutex_unlock(&state->copy_up_lock);

	if (entered_transaction)
		mount_vfs_transaction_leave(inode->i_mount);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Rejects an upper or visible-lower collision before changing the upper
 * namespace.
 */
static OVERLAY_HIGH int
overlay_new_preflight(
	struct inode *directory,
	const struct componentname *name,
	char text[NAME_MAX + 1U],
	char relative[ZEDBSD_PATH_MAX],
	struct inode **hidden_lower)
{
	struct overlay_mount_state *state;
	struct path upper;
	struct path lower;
	struct inode *found;
	struct overlay_inode_info *info;
	char parent_path[ZEDBSD_PATH_MAX];
	unsigned flags;
	unsigned parent_flags;
	int reserved;
	int error;

	state = directory->i_mount->m_data;
	found = NULL;

	/*
	 * A whiteout deliberately makes its matching lower name recreatable;
	 * an opaque parent makes all of its lower children invisible.
	 */
	if (hidden_lower != NULL)
		*hidden_lower = NULL;

	/* Rejects anything but a directory this driver made. */
	info = overlay_info(directory);
	if (info == NULL || directory->i_type != INODE_DIR)
		return ENOTDIR;
	error = overlay_info_snapshot(directory, &upper, &lower, parent_path);
	if (error != 0)
		return error;

	/* A reserved name can never be created. */
	error = overlay_component_text(name, text);
	if (error != 0)
		goto out;
	/* A name the overlay keeps for itself can never be created. */
	reserved = overlay_reserved_name(text);
	if (reserved) {
		error = EINVAL;
		goto out;
	}

	error = overlay_join(parent_path, text, relative);
	if (error != 0)
		goto out;

	/* The name must not exist in the upper layer. */
	if (upper.p_inode != NULL) {
		error = inode_lookup(upper.p_inode, name, &found);
		if (error == 0) {
			error = EEXIST;
			goto out;
		}

		if (error != ENOENT)
			goto out;
	}

	/*
	 * A visible lower object blocks the creation; a hidden one is reported.
	 */
	flags = overlay_metadata_flags(state, relative);
	parent_flags = overlay_metadata_flags(state, parent_path);
	if (lower.p_inode == NULL ||
	    (parent_flags & OVERLAY_META_OPAQUE) != 0) {
		error = 0;
		goto out;
	}

	error = inode_lookup(lower.p_inode, name, &found);
	if (error == ENOENT) {
		error = 0;
		goto out;
	}

	/* A name that exists may only be taken when a whiteout covers it. */
	if (error != 0)
		goto out;
	if ((flags & OVERLAY_META_WHITEOUT) == 0) {
		error = EEXIST;
		goto out;
	}

	if (hidden_lower != NULL) {
		*hidden_lower = found;
		found = NULL;
	}

	error = 0;
out:
	if (found != NULL)
		inode_release(found);
	path_release(&upper);
	path_release(&lower);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Makes a new upper object durable and visible, or removes it on failure. */
static OVERLAY_HIGH int
overlay_finish_new(
	struct inode *directory,
	const struct componentname *name,
	const char *relative,
	int directory_object,
	int opaque_added,
	struct inode **result)
{
	struct overlay_mount_state *state;
	struct path upper;
	unsigned name_flags;
	int error;
	int cleanup_error;
	int one_error;
	int whiteout_removed;

	state = directory->i_mount->m_data;
	cleanup_error = 0;
	whiteout_removed = 0;

	/*
	 * Makes the upper object durable and drops a whiteout that hid the
	 * name.
	 */
	error = overlay_path_snapshot(directory, OVERLAY_PATH_UPPER, &upper);
	if (error != 0) {
		if (error == ENOENT)
			return EIO;
		return error;
	}

	error = mount_sync_backend(upper.p_mount);

	/* Creating the name again retires the whiteout that hid it. */
	name_flags = overlay_metadata_flags(state, relative);
	if (error == 0 && (name_flags & OVERLAY_META_WHITEOUT) != 0) {
		error = overlay_journal_append(state,
		    OVERLAY_OP_REMOVE_WHITEOUT, relative);
		if (error == 0)
			whiteout_removed = 1;
	}

	/* Resolves the new object through the merged view. */
	if (error == 0) {
		namecache_remove(directory, name);
		error = overlay_lookup(directory, name, result);
	}

	if (error == 0) {
		path_release(&upper);
		return 0;
	}

	/*
	 * Try every rollback step even if an earlier one fails.  Restoring
	 * the whiteout first hides the new upper object while it is removed.
	 */
	if (whiteout_removed) {
		one_error = overlay_journal_append(state,
		    OVERLAY_OP_ADD_WHITEOUT, relative);
		if (cleanup_error == 0)
			cleanup_error = one_error;
	}

	/* Takes the half-made object back out of the upper layer. */
	if (directory_object)
		one_error = inode_rmdir(upper.p_inode, name);
	else
		one_error = inode_unlink(upper.p_inode, name);
	if (cleanup_error == 0)
		cleanup_error = one_error;
	if (opaque_added) {
		one_error = overlay_journal_append(state,
		    OVERLAY_OP_CLEAR_OPAQUE, relative);
		if (cleanup_error == 0)
			cleanup_error = one_error;
	}

	/* A rollback that could not be made durable stops all writing. */
	one_error = mount_sync_backend(upper.p_mount);
	if (cleanup_error == 0)
		cleanup_error = one_error;
	if (cleanup_error != 0)
		state->flags = OVERLAY_READ_ONLY;
	namecache_remove(directory, name);
	if (result != NULL)
		*result = NULL;
	path_release(&upper);
	if (cleanup_error != 0)
		return cleanup_error;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Creates a regular file in the upper layer. */
static OVERLAY_HIGH int
overlay_create(
	struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request,
	struct inode **result)
{
	struct overlay_mount_state *state;
	struct overlay_materialization_transaction materialization;
	struct path upper;
	struct inode *created;
	char text[NAME_MAX + 1U];
	char relative[ZEDBSD_PATH_MAX];
	int error;

	state = directory->i_mount->m_data;
	materialization.created = NULL;
	materialization.count = 0U;
	created = NULL;

	/* Rejects anything but a regular file request on a writable overlay. */
	if (request == NULL || request->type != INODE_REG || result == NULL)
		return EINVAL;
	path_init(&upper);
	*result = NULL;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;

	/* Checks the name, materializes the parent, and creates the file. */
	error = overlay_new_preflight(directory, name, text, relative, NULL);
	if (error == 0)
		error = overlay_ensure_upper_dir_tracked(directory,
		    &materialization);
	if (error == 0)
		error = overlay_path_snapshot(directory, OVERLAY_PATH_UPPER,
		    &upper);
	if (error == 0)
		error = inode_create(upper.p_inode, name, request, &created);
	if (error == 0)
		inode_release(created);
	path_release(&upper);
	if (error == 0)
		error = overlay_finish_new(directory, name, relative, 0, 0,
		    result);

	/* Reports the failure. */
	error = overlay_materialization_complete(state, &materialization,
	    error);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Creates a directory in the upper layer, opaque over a whited-out lower one.
 */
static OVERLAY_HIGH int
overlay_mkdir(
	struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request,
	struct inode **result)
{
	struct overlay_mount_state *state;
	struct overlay_materialization_transaction materialization;
	struct path upper;
	struct inode *created;
	struct inode *lower;
	char text[NAME_MAX + 1U];
	char relative[ZEDBSD_PATH_MAX];
	unsigned metadata_flags;
	int error;
	int opaque_added;
	int rollback_error;

	/* Creates the directory inside one transaction. */
	state = directory->i_mount->m_data;
	materialization.created = NULL;
	materialization.count = 0U;
	created = NULL;
	lower = NULL;
	opaque_added = 0;

	/* Rejects anything but a directory request on a writable overlay. */
	if (request == NULL || request->type != INODE_DIR || result == NULL)
		return EINVAL;
	path_init(&upper);
	*result = NULL;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;

	/* Checks the name and materializes the parent. */
	error = overlay_new_preflight(directory, name, text, relative, &lower);
	if (error != 0)
		goto out;
	error = overlay_ensure_upper_dir_tracked(directory, &materialization);
	if (error != 0)
		goto out;
	error = overlay_path_snapshot(directory, OVERLAY_PATH_UPPER, &upper);
	if (error != 0)
		goto out;

	/*
	 * A directory replacing a whited-out lower one must not merge with it.
	 */
	metadata_flags = overlay_metadata_flags(state, relative);
	if (lower != NULL &&
	    lower->i_type == INODE_DIR &&
	    (metadata_flags & OVERLAY_META_WHITEOUT) != 0 &&
	    (metadata_flags & OVERLAY_META_OPAQUE) == 0) {
		error = overlay_journal_append(state, OVERLAY_OP_SET_OPAQUE,
		    relative);
		if (error != 0)
			goto out;
		opaque_added = 1;
	}

	/* Creates the directory and publishes it. */
	error = inode_mkdir(upper.p_inode, name, request, &created);
	if (error != 0)
		goto out;
	inode_release(created);
	created = NULL;
	error = overlay_finish_new(directory, name, relative, 1,
	    opaque_added, result);
	opaque_added = 0;
out:
	/* Clears an opacity the finish step did not consume. */
	if (opaque_added) {
		rollback_error = overlay_journal_append(state,
		    OVERLAY_OP_CLEAR_OPAQUE, relative);
		if (rollback_error != 0) {
			state->flags = OVERLAY_READ_ONLY;
			error = rollback_error;
		}
	}

	if (created != NULL)
		inode_release(created);
	if (lower != NULL)
		inode_release(lower);
	path_release(&upper);

	/* Reports the failure. */
	error = overlay_materialization_complete(state, &materialization,
	    error);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Detaches a special endpoint from an inode when it is still the expected one.
 */
static OVERLAY_HIGH void
overlay_special_clear(
	struct inode *inode,
	void *expected)
{
	if (inode == NULL)
		return;
	mutex_lock(&inode->i_lock);

	if (inode->i_special == expected)
		inode->i_special = NULL;

	mutex_unlock(&inode->i_lock);
}

/*
 * Moves a special endpoint from one inode to another, restoring it on failure.
 */
static OVERLAY_HIGH int
overlay_special_transfer(
	struct inode *source,
	struct inode *destination,
	void *expected)
{
	int error;

	error = 0;

	/* Detaches the endpoint from the source. */
	mutex_lock(&source->i_lock);

	if (source->i_special != expected)
		error = EIO;
	else
		source->i_special = NULL;

	mutex_unlock(&source->i_lock);

	if (error != 0)
		return error;

	/* Attaches it to the destination, or puts it back. */
	mutex_lock(&destination->i_lock);

	if (destination->i_special != NULL)
		error = EADDRINUSE;
	else
		destination->i_special = expected;

	mutex_unlock(&destination->i_lock);

	/* A transfer that failed gives the object back to its source. */
	if (error != 0) {
		mutex_lock(&source->i_lock);
		if (source->i_special == NULL)
			source->i_special = expected;
		mutex_unlock(&source->i_lock);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Builds a pathname socket under a reserved temporary name and renames it into
 * place.
 */
static OVERLAY_HIGH int
overlay_mknod_socket(
	struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request,
	const char *relative,
	struct inode **result)
{
	struct overlay_mount_state *state;
	struct componentname temporary;
	struct inode *created;
	struct inode *prepared;
	struct path parent_upper;
	struct path temporary_path;
	char temporary_name[11];
	uint16_t number;
	unsigned attempts;
	int error;
	int cleanup_error;
	int sync_error;
	int renamed;

	state = directory->i_mount->m_data;
	created = NULL;
	prepared = NULL;
	error = 0;
	renamed = 0;

	/*
	 * The endpoint is attached to the cached overlay inode before the
	 * upper rename, so a successful lookup of the final name can never
	 * observe a half-bound socket node.
	 */
	path_init(&parent_upper);
	path_init(&temporary_path);
	error = overlay_path_snapshot(directory, OVERLAY_PATH_UPPER,
	    &parent_upper);
	if (error != 0)
		goto out_unlocked;
	mutex_lock(&state->copy_up_lock);

	/* Creates the node under a fresh temporary name. */
	for (attempts = 0; attempts < 65536U; attempts++) {
		number = state->temp_counter;
		state->temp_counter++;
		overlay_temp_name(number, temporary_name);
		temporary.cn_nameptr = temporary_name;
		temporary.cn_namelen = 10U;
		temporary.cn_flags = COMPONENT_LAST;
		error = inode_mknod(parent_upper.p_inode, &temporary, request,
		    &created);
		if (error == 0)
			break;
		if (error != EEXIST)
			goto out;
	}

	if (created == NULL) {
		error = ENOSPC;
		goto out;
	}

	/* Binds the endpoint to the overlay inode, then renames into place. */
	path_set(&temporary_path, parent_upper.p_mount, created);
	error = overlay_make_inode(directory->i_mount, relative,
	    &temporary_path, NULL, &prepared);
	if (error == 0)
		error = overlay_special_transfer(created, prepared,
		    request->special);
	if (error == 0)
		error = inode_rename(parent_upper.p_inode, &temporary,
		    parent_upper.p_inode, name, 0);
	if (error == 0)
		renamed = 1;
	if (error == 0)
		error = overlay_finish_new(directory, name, relative, 0, 0,
		    result);
out:
	/* Unbinds and retires the overlay inode on failure. */
	if (error != 0 && prepared != NULL) {
		overlay_special_clear(prepared, request->special);
		overlay_retire_inode(prepared);
	}

	/*
	 * The reserved temporary name is not visible through overlay lookup,
	 * but it is still persistent upper state.  Complete and sync its
	 * removal before reporting the original failure.  If cleanup cannot
	 * be made durable, its error is authoritative and the overlay is
	 * quarantined read-only.
	 */
	if (!renamed && created != NULL) {
		cleanup_error = inode_unlink(parent_upper.p_inode, &temporary);
		sync_error = mount_sync_backend(parent_upper.p_mount);
		if (cleanup_error == 0)
			cleanup_error = sync_error;
		if (cleanup_error != 0) {
			state->flags = OVERLAY_READ_ONLY;
			error = cleanup_error;
		}
	}

	/* Gives back everything this call took a reference to. */
	if (prepared != NULL)
		inode_release(prepared);
	if (created != NULL) {
		overlay_special_clear(created, request->special);
		inode_release(created);
	}

	path_release(&temporary_path);

	mutex_unlock(&state->copy_up_lock);

out_unlocked:
	path_release(&parent_upper);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Creates a special node in the upper layer. */
static OVERLAY_HIGH int
overlay_mknod(
	struct inode *directory,
	const struct componentname *name,
	const struct inode_creation_request *request,
	struct inode **result)
{
	struct overlay_mount_state *state;
	struct overlay_materialization_transaction materialization;
	struct path upper;
	struct inode *created;
	char text[NAME_MAX + 1U];
	char relative[ZEDBSD_PATH_MAX];
	int error;

	state = directory->i_mount->m_data;
	materialization.created = NULL;
	materialization.count = 0U;
	created = NULL;

	/* Rejects a missing operand or an unsupported node type. */
	if (request == NULL || result == NULL)
		return EINVAL;
	path_init(&upper);
	if (request->type != INODE_SOCKET &&
	    request->type != INODE_FIFO &&
	    request->type != INODE_CHAR &&
	    request->type != INODE_BLOCK)
		return EOPNOTSUPP;
	*result = NULL;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;

	/* Checks the name and materializes the parent. */
	error = overlay_new_preflight(directory, name, text, relative, NULL);
	if (error == 0)
		error = overlay_ensure_upper_dir_tracked(directory,
		    &materialization);

	/*
	 * A socket binds its endpoint first; anything else is created in place.
	 */
	if (error == 0 && request->type == INODE_SOCKET)
		error = overlay_mknod_socket(directory, name, request, relative,
		    result);
	else if (error == 0)
		error = overlay_path_snapshot(directory, OVERLAY_PATH_UPPER,
		    &upper);
	if (error == 0 && request->type != INODE_SOCKET)
		error = inode_mknod(upper.p_inode, name, request, &created);
	path_release(&upper);
	if (error == 0 && request->type != INODE_SOCKET) {
		inode_release(created);
		error = overlay_finish_new(directory, name, relative, 0, 0,
		    result);
	}

	/* Reports the failure. */
	error = overlay_materialization_complete(state, &materialization,
	    error);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Creates a symbolic link in the upper layer. */
static OVERLAY_HIGH int
overlay_symlink(
	struct inode *directory,
	const struct componentname *name,
	const char *target,
	const struct inode_creation_request *request,
	struct inode **result)
{
	struct overlay_mount_state *state;
	struct overlay_materialization_transaction materialization;
	struct path upper;
	struct inode *created;
	char text[NAME_MAX + 1U];
	char relative[ZEDBSD_PATH_MAX];
	int error;

	state = directory->i_mount->m_data;
	materialization.created = NULL;
	materialization.count = 0U;
	created = NULL;

	/*
	 * Rejects anything but a symbolic link request on a writable overlay.
	 */
	if (target == NULL ||
	    request == NULL ||
	    request->type != INODE_SYMLINK ||
	    result == NULL)
		return EINVAL;
	path_init(&upper);
	*result = NULL;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;

	/* Checks the name, materializes the parent, and creates the link. */
	error = overlay_new_preflight(directory, name, text, relative, NULL);
	if (error == 0)
		error = overlay_ensure_upper_dir_tracked(directory,
		    &materialization);
	if (error == 0)
		error = overlay_path_snapshot(directory, OVERLAY_PATH_UPPER,
		    &upper);
	if (error == 0)
		error = inode_symlink(upper.p_inode, name, target, request,
		    &created);
	path_release(&upper);
	if (error == 0) {
		inode_release(created);
		error = overlay_finish_new(directory, name, relative, 0, 0,
		    result);
	}

	/* Reports the failure. */
	error = overlay_materialization_complete(state, &materialization,
	    error);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reads a symbolic link from its visible layer. */
static OVERLAY_HIGH ssize_t
overlay_readlink(
	struct inode *inode,
	char *buffer,
	size_t capacity)
{
	struct path visible;
	ssize_t result;
	int error;

	/* Reports a missing target as a device error. */
	error = overlay_path_snapshot(inode, OVERLAY_PATH_VISIBLE, &visible);
	if (error != 0) {
		if (error == ENOENT)
			return -(ssize_t)EIO;
		return -(ssize_t)error;
	}

	/* Reads the link through the visible layer. */
	result = inode_readlink(visible.p_inode, buffer, capacity);
	path_release(&visible);
	return result;
}

/* Tests whether a path is a root or lies below it. */
static OVERLAY_HIGH int
overlay_path_is_below(
	const char *path,
	const char *root)
{
	size_t length;
	int difference;

	/* Requires the path to start at the root. */
	length = strlen(root);
	difference = strncmp(path, root, length);
	if (difference != 0)
		return 0;

	/* Requires the match to end on a component boundary. */
	if (path[length] != '\0' && path[length] != '/')
		return 0;
	return 1;
}

/*
 * Checks that moving a subtree keeps every identity path in bounds and unique.
 */
static OVERLAY_HIGH int
overlay_repath_preflight(
	struct overlay_mount_state *state,
	const char *old_path,
	const char *new_path,
	const struct inode *replaced)
{
	unsigned i;
	unsigned j;
	size_t old_length;
	size_t new_length;
	char candidate[ZEDBSD_PATH_MAX];
	const char *suffix;
	const struct overlay_inode_info *replaced_info;
	size_t suffix_length;
	int difference;
	int below;

	old_length = strlen(old_path);
	new_length = strlen(new_path);

	/*
	 * Every active identity below the old path must fit under the new one.
	 */
	for (i = 0; i < OVERLAY_IDENTITY_MAX; i++) {
		/* Only a live identity inside the subtree is being moved. */
		if (state->identities[i].state != OVERLAY_ID_ACTIVE)
			continue;

		below = overlay_path_is_below(state->identities[i].path,
					      old_path);
		if (!below)
			continue;

		/* What the identity's path holds past the subtree root. */
		suffix = state->identities[i].path + old_length;

		/*
		 * The rewritten path has to fit in the buffer it is built in.
		 */
		suffix_length = strlen(suffix);
		if (new_length + suffix_length >= sizeof(candidate))
			return ENAMETOOLONG;

		/*
		 * The rewritten path may only collide with the replaced object.
		 */
		strcpy(candidate, new_path);
		strcat(candidate, suffix);
		for (j = 0; j < OVERLAY_IDENTITY_MAX; j++) {
			/* A slot that names nothing cannot be collided with. */
			if (state->identities[j].state != OVERLAY_ID_ACTIVE)
				continue;

			/* Nor can one that is itself being moved. */
			below = overlay_path_is_below(
				state->identities[j].path, old_path);
			if (below)
				continue;

			/* Only an identity of the same path is a collision. */
			difference = strcmp(state->identities[j].path,
					    candidate);
			if (difference != 0)
				continue;

			/*
			 * A collision is allowed only with the object this
			 * rename replaces, which is about to go away.
			 */
			if (replaced == NULL)
				return EEXIST;

			replaced_info = overlay_info(replaced);
			if (replaced_info->identity_index != j)
				return EEXIST;
		}
	}

	return 0;
}

/* Rewrites every identity and cached inode path below a moved subtree. */
static OVERLAY_HIGH void
overlay_repath_commit(
	struct overlay_mount_state *state,
	struct mount *mountp,
	const char *old_path,
	const char *new_path)
{
	unsigned i;
	size_t old_length;
	char updated[ZEDBSD_PATH_MAX];
	struct overlay_inode_info *info;
	struct inode *inode;
	int below;

	old_length = strlen(old_path);

	/* Rewrites the identity table. */
	for (i = 0; i < OVERLAY_IDENTITY_MAX; i++) {
		/* Only a live identity inside the subtree is being moved. */
		if (state->identities[i].state != OVERLAY_ID_ACTIVE)
			continue;

		below = overlay_path_is_below(state->identities[i].path,
					      old_path);
		if (!below)
			continue;

		strcpy(updated, new_path);
		strcat(updated, state->identities[i].path + old_length);
		strcpy(state->identities[i].path, updated);
	}

	/* Rewrites every cached inode of this mount under its lock. */
	for (i = 0; i < OVERLAY_INODE_MAX; i++) {
		if (!overlay_inodes[i].used ||
		    overlay_inodes[i].inode.i_mount != mountp)
			continue;
		inode = &overlay_inodes[i].inode;
		info = &overlay_inodes[i].info;
		mutex_lock(&inode->i_lock);

		/* Only an inode inside the subtree has its path rewritten. */
		below = overlay_path_is_below(info->path, old_path);
		if (below) {
			strcpy(updated, new_path);
			strcat(updated, info->path + old_length);
			strcpy(info->path, updated);
		}

		mutex_unlock(&inode->i_lock);
	}
}

/* Renames within the upper layer, copying a lower regular source up first. */
static OVERLAY_HIGH int
overlay_rename(
	struct inode *old_directory,
	const struct componentname *old_name,
	struct inode *new_directory,
	const struct componentname *new_name,
	unsigned flags)
{
	struct overlay_mount_state *state;
	struct overlay_inode_info *source_info;
	struct inode *source;
	struct inode *target;
	struct path old_parent_upper;
	struct path new_parent_upper;
	struct path old_parent_lower;
	struct path source_upper;
	struct path source_lower;
	struct path new_upper_path;
	char old_text[NAME_MAX + 1U];
	char new_text[NAME_MAX + 1U];
	char old_parent_path[ZEDBSD_PATH_MAX];
	char new_parent_path[ZEDBSD_PATH_MAX];
	char old_relative[ZEDBSD_PATH_MAX];
	char new_relative[ZEDBSD_PATH_MAX];
	unsigned name_flags;
	int old_reserved;
	int new_reserved;
	int error;
	unsigned identity;

	state = old_directory->i_mount->m_data;
	source = NULL;
	target = NULL;

	/* Rejects a flagged rename, a read-only overlay, or a reserved name. */
	path_init(&old_parent_upper);
	path_init(&old_parent_lower);
	path_init(&new_parent_upper);
	path_init(&source_upper);
	path_init(&source_lower);
	path_init(&new_upper_path);
	if (flags != 0)
		return EINVAL;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	error = overlay_component_text(old_name, old_text);
	if (error == 0)
		error = overlay_component_text(new_name, new_text);
	if (error != 0)
		return error;
	/* A name the overlay keeps for itself is neither end of a rename. */
	old_reserved = overlay_reserved_name(old_text);
	new_reserved = overlay_reserved_name(new_text);
	if (old_reserved || new_reserved)
		return EINVAL;

	/* Resolves the source and any target through the merged view. */
	error = overlay_info_snapshot(old_directory, NULL, &old_parent_lower,
	    old_parent_path);
	if (error == 0)
		error = overlay_info_snapshot(new_directory, NULL, NULL,
		    new_parent_path);
	if (error == 0)
		error = overlay_join(old_parent_path, old_text, old_relative);
	if (error == 0)
		error = overlay_join(new_parent_path, new_text, new_relative);
	if (error == 0)
		error = overlay_lookup(old_directory, old_name, &source);
	if (error != 0)
		goto out;
	source_info = overlay_info(source);
	error = overlay_info_snapshot(source, &source_upper, &source_lower,
	    NULL);
	if (error != 0)
		goto out;
	/*
	 * The visible upper may omit its hidden lower. Renaming that upper must
	 * still whiteout the old backing name, just like unlink.
	 */
	if (source_lower.p_inode == NULL && old_parent_lower.p_inode != NULL) {
		error = overlay_lookup_real(&old_parent_lower, old_name,
		    &source_lower);
		if (error != 0 && error != ENOENT)
			goto out;
	}

	error = overlay_lookup(new_directory, new_name, &target);
	if (error == ENOENT)
		error = 0;
	else if (error != 0)
		goto out;

	/* A directory must be upper-only; a regular file is copied up. */
	if (source->i_type == INODE_DIR) {
		if (source_upper.p_inode == NULL ||
		    source_lower.p_inode != NULL) {
			error = EXDEV;
			goto out;
		}

		if (target != NULL && target->i_type != INODE_DIR) {
			error = ENOTDIR;
			goto out;
		}

		if (target != NULL) {
			error = overlay_directory_empty(target);
			if (error != 0)
				goto out;
		}

		/* Every identity under the subtree has to survive the move. */
		error = overlay_repath_preflight(state, old_relative,
			new_relative, target);
		if (error != 0)
			goto out;
	} else if (target != NULL && target->i_type == INODE_DIR) {
		error = EISDIR;
		goto out;
	} else if (source_upper.p_inode == NULL) {
		error = overlay_copy_up_regular(source);
		if (error != 0)
			goto out;
		path_release(&source_upper);
		path_release(&source_lower);
		error = overlay_info_snapshot(source, &source_upper,
		    &source_lower, NULL);
		if (error != 0)
			goto out;
	}

	/* Materializes the destination parent and hides a lower source. */
	error = overlay_ensure_upper_dir(new_directory);
	if (error != 0)
		goto out;
	error = overlay_path_snapshot(old_directory, OVERLAY_PATH_UPPER,
	    &old_parent_upper);
	if (error == 0)
		error = overlay_path_snapshot(new_directory, OVERLAY_PATH_UPPER,
		    &new_parent_upper);
	if (error != 0)
		goto out;
	if (source_lower.p_inode != NULL) {
		error = overlay_journal_append(state,
			OVERLAY_OP_ADD_WHITEOUT, old_relative);
		if (error != 0)
			goto out;
	}

	error = inode_rename(old_parent_upper.p_inode, old_name,
		new_parent_upper.p_inode, new_name, 0);
	if (error != 0)
		goto out;

	/*
	 * The backend rename is the namespace commit.  Mirror it in the
	 * cached overlay inode before durability work which may report a
	 * later error.
	 */
	path_set(&new_upper_path, new_parent_upper.p_mount,
	    source_upper.p_inode);
	identity = source_info->identity_index;
	if (source->i_type == INODE_DIR)
		overlay_repath_commit(state, source->i_mount, old_relative,
			new_relative);
	else
		strcpy(state->identities[identity].path, new_relative);
	overlay_publish_upper(source, &new_upper_path, 1, new_relative);
	if (target != NULL && target != source)
		overlay_retire_inode(target);
	/*
	 * Reject delayed pre-rename lookups even when the following sync fails.
	 */
	inode_dir_changed(old_directory);
	if (new_directory != old_directory)
		inode_dir_changed(new_directory);
	if (source->i_type == INODE_DIR && old_directory != new_directory)
		inode_dir_changed(source);
	namecache_remove(old_directory, old_name);
	namecache_remove(new_directory, new_name);

	/* Makes the rename durable and drops a whiteout on the new name. */
	error = mount_sync_backend(new_parent_upper.p_mount);
	if (error != 0)
		goto out;
	/* Renaming onto the name retires the whiteout that hid it. */
	name_flags = overlay_metadata_flags(state, new_relative);
	if ((name_flags & OVERLAY_META_WHITEOUT) != 0) {
		error = overlay_journal_append(state,
			OVERLAY_OP_REMOVE_WHITEOUT, new_relative);
		if (error != 0)
			goto out;
	}

	/* Gives back everything this call took a reference to. */
	error = 0;
out:
	if (target != NULL)
		inode_release(target);
	if (source != NULL)
		inode_release(source);
	path_release(&old_parent_upper);
	path_release(&old_parent_lower);
	path_release(&new_parent_upper);
	path_release(&source_upper);
	path_release(&source_lower);
	path_release(&new_upper_path);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Retires an inode's identity and marks the inode dead. */
static OVERLAY_HIGH void
overlay_retire_inode(
	struct inode *inode)
{
	struct overlay_mount_state *state;
	struct overlay_inode_info *info;

	state = inode->i_mount->m_data;
	info = overlay_info(inode);

	if (info->identity_index < OVERLAY_IDENTITY_MAX)
		state->identities[info->identity_index].state =
			OVERLAY_ID_RETIRED;
	inode->i_flags |= INODE_DEAD;
}

/* Tests whether a merged directory has no entries. */
static OVERLAY_HIGH int
overlay_directory_empty(
	struct inode *inode)
{
	struct path path;
	struct file *file;
	struct dirent entry;
	int eof;
	int error;

	eof = 0;

	/* Reads the first entry of the merged listing. */
	path_init(&path);
	path_set(&path, inode->i_mount, inode);
	error = file_open_resolved(&path, O_RDONLY | O_DIRECTORY, &file);
	path_release(&path);
	if (error != 0)
		return error;
	error = file_readdir(file, &entry, &eof);
	(void)file_close(file);
	if (error != 0)
		return error;
	if (eof)
		return 0;
	return ENOTEMPTY;
}

/*
 * Removes a name from the merged view, hiding a lower object with a whiteout.
 */
static OVERLAY_HIGH int
overlay_remove(
	struct inode *directory,
	const struct componentname *name,
	int removing_directory)
{
	struct overlay_mount_state *state;
	struct path parent_upper;
	struct path parent_lower;
	struct path target_upper;
	struct path target_lower;
	struct inode *target;
	char text[NAME_MAX + 1U];
	char parent_path[ZEDBSD_PATH_MAX];
	char relative[ZEDBSD_PATH_MAX];
	int reserved;
	int error;

	state = directory->i_mount->m_data;

	/* Rejects a read-only overlay or a reserved name. */
	path_init(&parent_upper);
	path_init(&parent_lower);
	path_init(&target_upper);
	path_init(&target_lower);
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	error = overlay_component_text(name, text);
	if (error != 0)
		return error;
	/* A name the overlay keeps for itself is never the caller's. */
	reserved = overlay_reserved_name(text);
	if (reserved)
		return EINVAL;

	/* Resolves the object through the merged view. */
	error = overlay_info_snapshot(directory, &parent_upper, &parent_lower,
	    parent_path);
	if (error == 0)
		error = overlay_join(parent_path, text, relative);
	if (error == 0)
		error = overlay_lookup(directory, name, &target);
	if (error != 0) {
		path_release(&parent_upper);
		path_release(&parent_lower);
		return error;
	}

	error = overlay_info_snapshot(target, &target_upper, &target_lower,
	    NULL);
	if (error != 0)
		goto out;

	/*
	 * The object must match the requested kind; a directory must be empty.
	 */
	if ((target->i_type == INODE_DIR) != removing_directory) {
		if (removing_directory)
			error = ENOTDIR;
		else
			error = EISDIR;
		goto out;
	}

	if (removing_directory) {
		error = overlay_directory_empty(target);
		if (error != 0)
			goto out;
	}

	/*
	 * A regular upper hides its lower path in the visible inode. Check the
	 * backing directory too, or unlink would resurrect the hidden entry.
	 */
	if (target_lower.p_inode == NULL && parent_lower.p_inode != NULL) {
		error = overlay_lookup_real(&parent_lower, name, &target_lower);
		if (error != 0 && error != ENOENT)
			goto out;
	}

	/* A lower object cannot be deleted, only covered by a whiteout. */
	if (target_lower.p_inode != NULL) {
		error = overlay_journal_append(state,
			OVERLAY_OP_ADD_WHITEOUT, relative);
		if (error != 0)
			goto out;
	}

	/* An upper object is removed from the upper layer itself. */
	if (target_upper.p_inode != NULL) {
		if (removing_directory)
			error = inode_rmdir(parent_upper.p_inode, name);
		else
			error = inode_unlink(parent_upper.p_inode, name);
		if (error != 0)
			goto out;
	}

	/*
	 * Removal is committed in the live namespace even if durability fails.
	 * Publish invalidation before sync; generic callers only do it on
	 * success.
	 */
	inode_dir_changed(directory);
	namecache_remove(directory, name);
	overlay_retire_inode(target);
	/* Only a removal that reached the upper layer must be made durable. */
	error = 0;
	if (target_upper.p_inode != NULL)
		error = mount_sync_backend(parent_upper.p_mount);
out:
	path_release(&parent_lower);
	inode_release(target);
	path_release(&parent_upper);
	path_release(&target_upper);
	path_release(&target_lower);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Unlinks a non-directory from the merged view. */
static OVERLAY_HIGH int
overlay_unlink(
	struct inode *directory,
	const struct componentname *name)
{
	int error;

	/* Reports the failure. */
	error = overlay_remove(directory, name, 0);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Removes an empty directory from the merged view. */
static OVERLAY_HIGH int
overlay_rmdir(
	struct inode *directory,
	const struct componentname *name)
{
	int error;

	/* Reports the failure. */
	error = overlay_remove(directory, name, 1);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Truncates the upper copy through the upper layer's transaction. */
static OVERLAY_HIGH int
overlay_truncate_upper(
	struct inode *inode,
	const struct inode_truncate_request *request,
	struct inode_truncate_result *result)
{
	struct path upper;
	struct inode_truncate_request inner_request;
	struct inode_truncate_result inner_result;
	struct overlay_inode_info *info;
	int error;

	/* An inode this driver did not make cannot be truncated through it. */
	info = overlay_info(inode);
	if (request == NULL || result == NULL || info == NULL)
		return EINVAL;

	result->actual_size = inode->i_size;
	result->limit_exceeded = 0;

	/*
	 * A credential-less stacked mutation has already crossed a content
	 * boundary, so the authoritative inode must conservatively remove
	 * set-id state.  Credential-bearing UAPI calls retain normal rules.
	 */
	inner_request = *request;
	if (inner_request.credential == NULL)
		inner_request.content_change = 1;
	error = overlay_path_snapshot(inode, OVERLAY_PATH_UPPER, &upper);
	if (error == 0)
		error = inode_truncate_transaction(upper.p_inode,
		    &inner_request, &inner_result);
	else
		memset(&inner_result, 0, sizeof(inner_result));
	result->limit_exceeded = inner_result.limit_exceeded;

	/*
	 * Refresh on every outcome.  The final inode may have cleared set-id
	 * or partially changed EOF before a later backend/durability error.
	 */
	overlay_refresh(inode);
	result->actual_size = inode->i_size;
	if (error == 0)
		error = mount_sync_backend(upper.p_mount);
	path_release(&upper);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Truncates a regular file, copying a lower one up first. */
static OVERLAY_HIGH int
overlay_truncate_limited(
	struct inode *inode,
	const struct inode_truncate_request *request,
	struct inode_truncate_result *result)
{
	struct overlay_mount_state *state;
	int error;

	state = inode->i_mount->m_data;

	/* Rejects a missing operand or a read-only overlay. */
	if (request == NULL || result == NULL)
		return EINVAL;
	result->actual_size = inode->i_size;
	result->limit_exceeded = 0;
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;

	/* Copies up, then truncates the upper copy. */
	error = overlay_copy_up_regular(inode);
	if (error != 0) {
		overlay_refresh(inode);
		result->actual_size = inode->i_size;
		return error;
	}

	/* Reports the failure. */
	error = overlay_truncate_upper(inode, request, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Truncates a regular file to a size without a growth limit. */
static OVERLAY_HIGH int
overlay_truncate(
	struct inode *inode,
	off_t size)
{
	const struct inode_truncate_request request = {
		.size = size,
		.growth_limit = UINT64_MAX,
		.credential = NULL,
		.content_change = 1,
	};
	struct inode_truncate_result result;
	int error;

	/* Reports the failure. */
	error = overlay_truncate_limited(inode, &request, &result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Changes attributes on the upper copy, materializing it first. */
static OVERLAY_HIGH int
overlay_setattr(
	struct inode *inode,
	const struct stat *status,
	unsigned mask)
{
	struct overlay_mount_state *state;
	struct path upper;
	int error;

	state = inode->i_mount->m_data;

	/* Rejects a read-only overlay. */
	path_init(&upper);
	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;

	/* Materializes the object in the upper layer. */
	if (inode->i_type == INODE_REG)
		error = overlay_copy_up_regular(inode);
	else if (inode->i_type == INODE_DIR)
		error = overlay_ensure_upper_dir(inode);
	else
		error = EOPNOTSUPP;

	/* Applies the change and makes it durable. */
	if (error == 0)
		error = overlay_path_snapshot(inode, OVERLAY_PATH_UPPER,
		    &upper);
	if (error == 0)
		error = inode_setattr(upper.p_inode, status, mask);
	if (error == 0) {
		overlay_refresh(inode);
		error = mount_sync_backend(upper.p_mount);
	}

	path_release(&upper);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Drops the layer references of an inode leaving the cache. */
static OVERLAY_HIGH void
overlay_reclaim(
	struct inode *inode)
{
	struct overlay_mount_state *state;
	struct overlay_inode_info *info;

	if (inode->i_mount != NULL)
		state = inode->i_mount->m_data;
	else
		state = NULL;
	info = overlay_info(inode);

	if (info == NULL)
		return;
	path_release(&info->upper);
	path_release(&info->lower);

	/* A retired identity is freed once its inode is gone. */
	if (state != NULL &&
	    info->identity_index < OVERLAY_IDENTITY_MAX &&
	    state->identities[info->identity_index].state == OVERLAY_ID_RETIRED)
		memset(&state->identities[info->identity_index], 0,
		       sizeof(state->identities[info->identity_index]));
}

/* Opens the visible layer's file behind an overlay regular file. */
static OVERLAY_HIGH int
overlay_regular_open(
	struct file *file)
{
	struct overlay_file_info *info;
	struct overlay_inode_info *inode_info;
	struct path visible;
	int status_flags;
	int error;
	int real_flags;

	/* A file this driver did not make cannot be opened through it. */
	inode_info = overlay_info(file->f_inode);
	if (inode_info == NULL)
		return EIO;

	/* A writable open copies a lower file up first. */
	status_flags = file_status_flags_get(file);
	if ((status_flags & O_ACCMODE) != O_RDONLY) {
		error = overlay_copy_up_regular(file->f_inode);
		if (error != 0)
			return error;
	}

	/* Opens the real file with the same access flags. */
	error = overlay_path_snapshot(file->f_inode, OVERLAY_PATH_VISIBLE,
	    &visible);
	if (error != 0) {
		if (error == ENOENT)
			return EIO;
		return error;
	}

	info = kern_malloc(sizeof(*info));
	if (info == NULL) {
		path_release(&visible);
		return ENOMEM;
	}

	/* The real file takes the flags the caller opened this one with. */
	real_flags = file_status_flags_get(file);
	real_flags &= ~(O_CREAT | O_EXCL | O_TRUNC);
	error = file_open_resolved(&visible, real_flags, &info->real);
	path_release(&visible);
	if (error != 0) {
		kern_free(info);
		return error;
	}

	file->f_data = info;
	file->f_vm_inode = file_vm_inode(info->real);
	return 0;
}

/* Reads at an offset from the real file. */
static OVERLAY_HIGH ssize_t
overlay_pread(
	struct file *file,
	void *buffer,
	size_t size,
	off_t offset)
{
	struct overlay_file_info *info;
	ssize_t count;

	info = file->f_data;

	/*
	 * The outer file owns the shared-cache transaction for f_vm_inode.
	 * The lower call is backend I/O within that transaction, not a
	 * second normal read which could wait on the outer CONTENT gate.
	 */
	if (info == NULL)
		return -EIO;
	count = file_pread_internal(info->real, buffer, size, offset,
	    FILE_IO_VM_OBJECT);
	return count;
}

/* Reads at an offset from the real file with the caller's I/O flags. */
static OVERLAY_HIGH ssize_t
overlay_pread_internal(
	struct file *file,
	void *buffer,
	size_t size,
	off_t offset,
	unsigned flags)
{
	struct overlay_file_info *info;
	ssize_t count;

	info = file->f_data;

	if (info == NULL)
		return -EIO;
	count = file_pread_internal(info->real, buffer, size, offset, flags);
	return count;
}

/* Reads at the file position, advancing it. */
static OVERLAY_HIGH ssize_t
overlay_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	ssize_t count;

	count = overlay_pread(file, buffer, size, file->f_offset);

	if (count > 0)
		file->f_offset += count;
	return count;
}

/* Writes at an offset to the upper copy. */
static OVERLAY_HIGH ssize_t
overlay_pwrite(
	struct file *file,
	const void *buffer,
	size_t size,
	off_t offset)
{
	struct overlay_file_info *info;
	ssize_t count;

	info = file->f_data;

	if (info == NULL)
		return -EIO;
	count = file_pwrite_internal(info->real, buffer, size, offset,
	    FILE_IO_VM_OBJECT);

	/*
	 * The final inode may have cleared set-id before a later backend
	 * error.  Always mirror that irreversible metadata transition to the
	 * visible inode.
	 */
	overlay_refresh(file->f_inode);
	return count;
}

/*
 * Writes at an offset to the upper copy with the caller's flags and credential.
 */
static OVERLAY_HIGH ssize_t
overlay_pwrite_internal(
	struct file *file,
	const void *buffer,
	size_t size,
	off_t offset,
	unsigned flags,
	const struct ucred *credential,
	const struct io_context *context)
{
	struct overlay_file_info *info;
	ssize_t count;

	/* Rejects a file that carries no overlay state. */
	info = file->f_data;
	if (info == NULL)
		return -EIO;

	/* Writes through and refreshes what the overlay reports. */
	count = file_pwrite_context(info->real, buffer, size, offset,
	    flags | FILE_IO_VM_OBJECT, credential, context);
	overlay_refresh(file->f_inode);
	return count;
}

#ifdef ZEDBSD_OVERLAY_CONTENT_HOST_TEST
/*
 * Exercises the real stacking callback without exposing overlay-private state.
 */
static int
overlay_host_truncate_limited(
	struct inode *inode,
	const struct inode_truncate_request *request,
	struct inode_truncate_result *result)
{
	int error;

	/* Reports the failure. */
	error = overlay_truncate_upper(inode, request, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
#endif

/* Writes at the file position or at the end, advancing the position. */
static OVERLAY_HIGH ssize_t
overlay_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	off_t offset;
	ssize_t count;
	int status_flags;

	/* An appending write always starts at the current end of the file. */
	status_flags = file_status_flags_get(file);
	if ((status_flags & O_APPEND) != 0)
		offset = file->f_inode->i_size;
	else
		offset = file->f_offset;
	count = overlay_pwrite(file, buffer, size, offset);

	if (count > 0)
		file->f_offset = offset + count;
	return count;
}

/* Syncs the real file and the overlay mount. */
static OVERLAY_HIGH int
overlay_regular_fsync(
	struct file *file)
{
	struct overlay_file_info *info;
	int error;

	info = file->f_data;
	if (info != NULL)
		error = file_fsync_backend(info->real);
	else
		error = EIO;

	if (error == 0)
		error = mount_sync_backend(file->f_inode->i_mount);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Closes the real file behind an overlay regular file. */
static OVERLAY_HIGH int
overlay_regular_close(
	struct file *file)
{
	struct overlay_file_info *info;
	int error;

	info = file->f_data;

	/* Closes the real file and releases the overlay state. */
	error = 0;
	if (info != NULL) {
		error = file_close(info->real);
		kern_free(info);
	}

	file->f_data = NULL;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Opens a cursor over the merged directory listing. */
static OVERLAY_HIGH int
overlay_dir_open(
	struct file *file)
{
	struct overlay_dir_cursor *cursor;

	cursor = kern_calloc(1, sizeof(*cursor));

	if (cursor == NULL)
		return ENOMEM;
	cursor->phase = OVERLAY_DIR_UPPER;
	file->f_data = cursor;
	return 0;
}

/* Closes the real directory a cursor is reading. */
static OVERLAY_HIGH void
overlay_dir_drop_active(
	struct overlay_dir_cursor *cursor)
{
	if (cursor->active != NULL)
		(void)file_close(cursor->active);
	cursor->active = NULL;
}

/* Opens the real directory of the cursor's current phase. */
static OVERLAY_HIGH int
overlay_dir_open_phase(
	struct file *file,
	struct overlay_dir_cursor *cursor)
{
	struct path path;
	struct path *upper_argument;
	struct path *lower_argument;
	char relative[ZEDBSD_PATH_MAX];
	unsigned directory_flags;
	int error;

	/* Takes the layer path of the current phase. */
	if (cursor->phase == OVERLAY_DIR_UPPER)
		upper_argument = &path;
	else
		upper_argument = NULL;
	if (cursor->phase == OVERLAY_DIR_LOWER)
		lower_argument = &path;
	else
		lower_argument = NULL;
	error = overlay_info_snapshot(file->f_inode, upper_argument,
	    lower_argument, relative);
	if (error != 0)
		return error;

	/* The lower phase is skipped below an opaque directory. */
	directory_flags = overlay_metadata_flags(
		file->f_inode->i_mount->m_data, relative);
	if (cursor->phase == OVERLAY_DIR_LOWER &&
	    (directory_flags & OVERLAY_META_OPAQUE) != 0)
		error = ENOENT;
	else if (path.p_inode == NULL || path.p_inode->i_type != INODE_DIR)
		error = ENOENT;
	else
		error = file_open_resolved(&path, O_RDONLY | O_DIRECTORY,
		    &cursor->active);
	path_release(&path);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Tests whether the upper layer has an entry of a name. */
static OVERLAY_HIGH int
overlay_dir_upper_has(
	struct inode *directory,
	const char *name)
{
	struct componentname component;
	struct path upper;
	struct inode *found;
	int error;

	/*
	 * A directory without an upper layer has nothing; a failure hides the
	 * name.
	 */
	error = overlay_path_snapshot(directory, OVERLAY_PATH_UPPER, &upper);
	if (error == ENOENT)
		return 0;
	if (error != 0)
		return 1;

	/* Looks the name up in the upper directory. */
	component.cn_nameptr = name;
	component.cn_namelen = strlen(name);
	component.cn_flags = 0;
	error = inode_lookup(upper.p_inode, &component, &found);
	if (error == 0)
		inode_release(found);
	path_release(&upper);
	if (error == 0)
		return 1;
	return 0;
}

/* Tests whether a lower entry is hidden by a whiteout. */
static OVERLAY_HIGH int
overlay_dir_child_hidden(
	struct inode *directory,
	const char *name)
{
	char parent[ZEDBSD_PATH_MAX];
	char relative[ZEDBSD_PATH_MAX];
	unsigned flags;
	int error;

	/* Treats a name it cannot resolve as hidden. */
	error = overlay_info_snapshot(directory, NULL, NULL, parent);
	if (error != 0)
		return 1;

	error = overlay_join(parent, name, relative);
	if (error != 0)
		return 1;

	/* Hides a name the upper layer marks as a whiteout. */
	flags = overlay_metadata_flags(directory->i_mount->m_data, relative);
	if ((flags & OVERLAY_META_WHITEOUT) != 0)
		return 1;
	return 0;
}

/* Fills a directory entry for a merged name with the overlay's identity. */
static OVERLAY_HIGH int
overlay_dir_emit(
	struct file *file,
	const char *name,
	struct dirent *entry)
{
	struct componentname component;
	struct inode *child;
	int error;

	/* Resolves the name to the inode the entry describes. */
	component.cn_nameptr = name;
	component.cn_namelen = strlen(name);
	component.cn_flags = 0;
	error = inode_lookup(file->f_inode, &component, &child);
	if (error != 0)
		return error;

	/* Fills the entry from the inode and releases it. */
	memset(entry, 0, sizeof(*entry));
	entry->d_ino = child->i_ino;
	entry->d_type = child->i_type;
	strncpy(entry->d_name, name, NAME_MAX);
	entry->d_name[NAME_MAX] = '\0';
	inode_release(child);
	return 0;
}

/* Reads the next merged directory entry. */
static OVERLAY_HIGH int
overlay_readdir(
	struct file *file,
	struct dirent *entry,
	int *eof)
{
	struct overlay_dir_cursor *cursor;
	struct overlay_inode_info *info;
	struct dirent real_entry;
	int real_eof;
	int dot;
	int dotdot;
	int reserved;
	int shadowed;
	int error;

	cursor = file->f_data;

	/* A directory this driver did not make cannot be read through it. */
	info = overlay_info(file->f_inode);
	if (cursor == NULL || info == NULL)
		return EIO;

	/*
	 * Walks the upper listing, then the lower one, skipping hidden names.
	 */
	while (cursor->phase != OVERLAY_DIR_DONE) {
		real_eof = 0;
		if (cursor->active == NULL) {
			error = overlay_dir_open_phase(file, cursor);
			if (error == ENOENT) {
				cursor->phase++;
				continue;
			}

			if (error != 0)
				return error;
		}

		/* Reads the next name of the layer this phase is walking. */
		error = file_readdir(cursor->active, &real_entry, &real_eof);
		if (error != 0)
			return error;
		if (real_eof) {
			overlay_dir_drop_active(cursor);
			cursor->phase++;
			continue;
		}

		/*
		 * Dot entries, reserved names, and shadowed lower names are
		 * skipped.
		 */
		dot = strcmp(real_entry.d_name, ".");
		dotdot = strcmp(real_entry.d_name, "..");
		if (dot == 0 || dotdot == 0)
			continue;

		/* A name the overlay keeps for itself is not the caller's. */
		reserved = overlay_reserved_name(real_entry.d_name);
		if (reserved)
			continue;

		/*
		 * A lower name the upper layer also holds, or one a whiteout
		 * covers, is not visible through the merged view.
		 */
		if (cursor->phase == OVERLAY_DIR_LOWER) {
			shadowed = overlay_dir_upper_has(file->f_inode,
							 real_entry.d_name);
			if (!shadowed)
				shadowed = overlay_dir_child_hidden(
					file->f_inode, real_entry.d_name);
			if (shadowed)
				continue;
		}
		error = overlay_dir_emit(file, real_entry.d_name, entry);
		if (error != 0)
			return error;
		file->f_offset++;
		*eof = 0;
		return 0;
	}

	*eof = 1;
	return 0;
}

/* Rewinds the merged listing; any other seek is refused. */
static OVERLAY_HIGH off_t
overlay_dir_seek(
	struct file *file,
	off_t offset,
	int whence)
{
	struct overlay_dir_cursor *cursor;

	/* Accepts only a rewind to the start of the directory. */
	cursor = file->f_data;
	if (cursor == NULL || whence != 0 || offset != 0)
		return -EINVAL;

	/* Restarts the walk at the upper layer. */
	overlay_dir_drop_active(cursor);
	cursor->phase = OVERLAY_DIR_UPPER;
	file->f_offset = 0;
	return 0;
}

/* Closes a merged directory cursor. */
static OVERLAY_HIGH int
overlay_dir_close(
	struct file *file)
{
	struct overlay_dir_cursor *cursor;

	/* Releases the walk state the cursor holds. */
	cursor = file->f_data;
	if (cursor != NULL) {
		overlay_dir_drop_active(cursor);
		kern_free(cursor);
	}

	file->f_data = NULL;
	return 0;
}

/* Syncs the upper directory, the active journal, and the upper mount. */
static OVERLAY_HIGH int
overlay_directory_fsync(
	struct file *file)
{
	struct overlay_mount_state *state;
	struct overlay_inode_info *info;
	struct path upper_path;
	struct file *upper;
	int error;
	int close_error;

	upper = NULL;

	/* A read-only overlay has nothing to sync. */
	if (file == NULL || file->f_inode == NULL)
		return EINVAL;
	state = file->f_inode->i_mount->m_data;

	/* A file this driver did not make cannot be synced through it. */
	info = overlay_info(file->f_inode);
	if (state == NULL || info == NULL)
		return EIO;
	if (state->flags == OVERLAY_READ_ONLY)
		return 0;

	/* Syncs the upper directory when there is one. */
	error = overlay_path_snapshot(file->f_inode, OVERLAY_PATH_UPPER,
	    &upper_path);
	if (error == 0) {
		error = file_open_resolved(&upper_path,
		    O_RDONLY | O_DIRECTORY, &upper);
		path_release(&upper_path);
		if (error != 0)
			return error;
		error = file_fsync(upper);
		close_error = file_close(upper);
		if (error != 0)
			return error;
		if (close_error != 0)
			return close_error;
	} else if (error != ENOENT) {
		return error;
	}

	/* Syncs the active journal and the upper mount. */
	if (state->journal[state->active_slot] == NULL)
		return EIO;
	error = file_fsync(state->journal[state->active_slot]);
	if (error != 0)
		return error;

	/* Reports the failure. */
	error = mount_sync_backend(state->upper_root.p_mount);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Deletes stale temporaries below an upper directory, bounded in depth and
 * work.
 */
static OVERLAY_HIGH int
overlay_cleanup_temps(
	struct path *directory,
	unsigned depth,
	unsigned *visited,
	unsigned *deleted)
{
	struct componentname component;
	struct inode *child;
	struct path child_path;
	struct file *file;
	struct dirent entry;
	int temporary;
	int dot;
	int dotdot;
	int eof;
	int error;

	if (depth > 16U)
		return ELOOP;

	/*
	 * Deletion can change FAT directory offsets.  Delete one and restart.
	 */
	for (;;) {
		error = file_open_resolved(directory, O_RDONLY | O_DIRECTORY,
		    &file);
		if (error != 0)
			return error;
		eof = 0;
		while (!eof) {
			error = file_readdir(file, &entry, &eof);
			if (error != 0 || eof)
				break;
			/* Only the overlay's own leftovers are cleaned up. */
			temporary = overlay_temporary_name(entry.d_name);
			if (!temporary)
				continue;
			component.cn_nameptr = entry.d_name;
			component.cn_namelen = strlen(entry.d_name);
			component.cn_flags = COMPONENT_LAST;
			error = inode_lookup(directory->p_inode, &component,
			    &child);
			if (error != 0)
				break;
			if (child->i_type == INODE_DIR) {
				inode_release(child);
				error = EINVAL;
				break;
			}

			inode_release(child);

			/* One pass deletes only so many names. */
			(*deleted)++;
			if (*deleted > 256U) {
				error = EOVERFLOW;
				break;
			}

			error = inode_unlink(directory->p_inode, &component);
			break;
		}

		/* The listing is reopened after every deletion it makes. */
		(void)file_close(file);
		if (error != 0)
			return error;
		if (eof)
			break;
	}

	/* With the current directory stable, recursively inspect children. */
	error = file_open_resolved(directory, O_RDONLY | O_DIRECTORY, &file);
	if (error != 0)
		return error;
	eof = 0;
	while (!eof) {
		error = file_readdir(file, &entry, &eof);
		if (error != 0 || eof)
			break;
		dot = strcmp(entry.d_name, ".");
		dotdot = strcmp(entry.d_name, "..");
		if (dot == 0 || dotdot == 0)
			continue;

		(*visited)++;
		if (*visited > 512U) {
			error = EOVERFLOW;
			break;
		}

		/* Descends into every child directory of this one. */
		component.cn_nameptr = entry.d_name;
		component.cn_namelen = strlen(entry.d_name);
		component.cn_flags = 0;
		error = inode_lookup(directory->p_inode, &component, &child);
		if (error != 0)
			break;
		if (child->i_type == INODE_DIR) {
			path_init(&child_path);
			path_set(&child_path, directory->p_mount, child);
			inode_release(child);
			error = overlay_cleanup_temps(&child_path, depth + 1U,
				visited, deleted);
			path_release(&child_path);
		} else {
			inode_release(child);
			error = 0;
		}

		if (error != 0)
			break;
	}

	(void)file_close(file);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Mounts an overlay from its upper and lower layer arguments. */
static OVERLAY_HIGH int
overlay_mount_impl(
	struct mount *mountp)
{
	const struct overlay_mount_args *args;
	struct overlay_mount_state *state;
	struct inode *root;
	unsigned visited;
	unsigned deleted;
	int supported;
	int error;

	args = mountp->m_data;
	visited = 0;
	deleted = 0;

	/* A mount that names no arguments has no layers to stack. */
	if (args == NULL)
		return EINVAL;	/* Failed. */

	/* An overlay is made of two layers, and needs both of them. */
	if (args->upper.p_inode == NULL || args->lower.p_inode == NULL)
		return EINVAL;	/* Failed. */

	/* Each layer is a tree, so each has to be named by a directory. */
	if (args->upper.p_inode->i_type != INODE_DIR ||
	    args->lower.p_inode->i_type != INODE_DIR)
		return EINVAL;	/* Failed. */

	/* And the mount is one of the two kinds this driver serves. */
	if (args->flags != OVERLAY_READ_ONLY &&
	    args->flags != OVERLAY_READ_WRITE)
		return EINVAL;	/* Failed. */

	/*
	 * The content-transaction lock chain currently has one visible
	 * wrapper and one authoritative inode.  A recursively stacked
	 * overlay would add a middle visible inode and introduce
	 * final->middle versus middle->final lock ordering.  Reject that
	 * unsupported topology explicitly rather than silently exposing
	 * stale metadata or an ABBA deadlock.
	 */
	supported = overlay_layers_supported(args);
	if (!supported)
		return EOPNOTSUPP;

	/* Records the layers with the root identity and loads the journal. */
	state = kern_calloc(1, sizeof(*state));
	if (state == NULL)
		return ENOMEM;
	(void)mutex_init(&state->copy_up_lock, LOCK_RANK_VFS_TRANSACTION,
	    "overlay copy-up");
	path_set(&state->upper_root, args->upper.p_mount, args->upper.p_inode);
	path_set(&state->lower_root, args->lower.p_mount, args->lower.p_inode);
	state->owner = mountp;
	state->flags = args->flags;
	state->next_ino = 2;
	state->identities[0].state = OVERLAY_ID_ACTIVE;
	state->identities[0].ino = 1;
	state->identities[0].path[0] = '\0';
	mountp->m_data = state;
	error = overlay_journal_load(state);
	if (error != 0)
		goto fail_state;

	/* Removes temporaries left by an interrupted copy-up. */
	error = overlay_cleanup_temps(&state->upper_root, 0, &visited,
	    &deleted);
	if (error == 0 && deleted != 0)
		error = mount_sync_backend(state->upper_root.p_mount);
	if (error != 0)
		goto fail_state;

	/* Creates the root inode over both layer roots. */
	error = overlay_make_inode(mountp, "", &state->upper_root,
		&state->lower_root, &root);
	if (error != 0)
		goto fail_state;
	root->i_flags |= INODE_ROOT;
	mountp->m_root = root;
	return 0;
fail_state:
	if (state->journal[0] != NULL)
		(void)file_close(state->journal[0]);
	if (state->journal[1] != NULL)
		(void)file_close(state->journal[1]);
	path_release(&state->lower_root);
	path_release(&state->upper_root);
	kern_free(state);
	mountp->m_data = NULL;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Syncs the active journal and the upper layer of a writable overlay. */
static OVERLAY_HIGH int
overlay_sync_mount(
	struct mount *mountp)
{
	struct overlay_mount_state *state;
	int error;

	/* Writes nothing back for a read-only overlay. */
	state = mountp->m_data;
	if (state == NULL || state->flags == OVERLAY_READ_ONLY)
		return 0;

	/* Flushes the journal and then the upper layer. */
	error = file_fsync(state->journal[state->active_slot]);
	if (error == 0)
		error = mount_sync_backend(state->upper_root.p_mount);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the statistics of the upper layer. */
static OVERLAY_HIGH int
overlay_statvfs(
	struct mount *mountp,
	struct statvfs *result)
{
	struct overlay_mount_state *state;
	int error;

	if (mountp != NULL)
		state = mountp->m_data;
	else
		state = NULL;

	if (state == NULL || result == NULL ||
	    state->upper_root.p_mount == NULL)
		return EINVAL;

	/* Reports the failure. */
	error = mount_statvfs(state->upper_root.p_mount, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Releases the journal files and the layer references of an overlay. */
static OVERLAY_HIGH void
overlay_unmount_impl(
	struct mount *mountp)
{
	struct overlay_mount_state *state;

	/* Does nothing for a mount that was never set up. */
	state = mountp->m_data;
	if (state == NULL)
		return;

	/* Closes both journal slots. */
	if (state->journal[0] != NULL)
		(void)file_close(state->journal[0]);
	if (state->journal[1] != NULL)
		(void)file_close(state->journal[1]);

	/* Releases both roots and the mount state. */
	path_release(&state->lower_root);
	path_release(&state->upper_root);
	kern_free(state);
	mountp->m_data = NULL;
}

/*
 * Prepare lower-only metadata/content before generic code takes i_io_lock.
 * Namespace mutations may hold that lock while reading parent attributes,
 * so materialization cannot acquire their gate from inside an I/O callback.
 */
static OVERLAY_HIGH int
overlay_prepare_mutation(
	struct inode *inode)
{
	struct overlay_mount_state *state = inode->i_mount->m_data;
	struct path upper;
	int io_owned;
	int error, entered;

	if (state->flags != OVERLAY_READ_WRITE)
		return EROFS;
	/*
	 * An already-owned I/O domain implies an outer preparation. Otherwise
	 * join even for an existing upper: it may belong to an in-flight
	 * ancestor materialization which can still roll back while holding
	 * namespace.
	 */
	io_owned = mutex_owned(&inode->i_io_lock);
	if (io_owned) {
		error = overlay_path_snapshot(inode, OVERLAY_PATH_UPPER,
					      &upper);
		if (error == 0)
			path_release(&upper);

		/* A caller inside i_io_lock could not wait for the copy-up. */
		if (error == ENOENT)
			return EDEADLK;

		/* Reports what the snapshot found. */
		return error;
	}

	entered = mount_vfs_transaction_join(inode->i_mount);
	if (state->flags != OVERLAY_READ_WRITE) {
		error = EROFS;
		goto out;
	}

	/* An object that has no upper copy yet is materialized here. */
	error = overlay_path_snapshot(inode, OVERLAY_PATH_UPPER, &upper);
	if (error == 0) {
		path_release(&upper);
	} else if (error == ENOENT) {
		if (inode->i_type == INODE_REG)
			error = overlay_copy_up_regular(inode);
		else if (inode->i_type == INODE_DIR)
			error = overlay_ensure_upper_dir(inode);
		else
			error = EOPNOTSUPP;
	}

out:
	if (entered)
		mount_vfs_transaction_leave(inode->i_mount);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
