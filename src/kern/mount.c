/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mount namespace.
 *
 * Mounts live in a fixed table and form a tree under the root mount,
 * each covering a directory inode of its parent.  Filesystem types are
 * registered by name and probed in order for "auto" mounts.  Private
 * mounts stay outside the tree for boot-time inspection, and bind
 * mounts re-expose an inode of another mount under a new path.
 */

#include "kern/mount.h"
#include "kern/writeback.h"
#include "kern/readahead.h"
#include "kern/backing-claim.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/namecache.h"
#include "kern/namei.h"
#include "kern/vm-object.h"
#include "kern/klog.h"
#include <hal/hal.h>

#include <errno.h>
#include <string.h>
#include <sys/statvfs.h>
#include <zedbsd/blkid.h>
#include <zedbsd/quota.h>
#include <zedbsd/snapshot.h>
#include <zedbsd/mountinfo.h>

#define FILESYSTEM_MAX 8U
#define MOUNT_BIND_INTERNAL 0x00000001U
#define MOUNT_HIGH __attribute__((section(".hightext")))
#ifdef KERN_STORAGE_HOST_TEST
#undef MOUNT_HIGH
#define MOUNT_HIGH
#endif

struct mount_io_boundary {
	struct writeback_unmount writeback;
	struct readahead_boundary readahead;
};

extern int readahead_boundary_begin(struct readahead_boundary *, struct mount *) __attribute__((weak));
extern void readahead_boundary_end(struct readahead_boundary *) __attribute__((weak));
extern int writeback_unmount_begin(struct mount *, struct writeback_unmount *) __attribute__((weak));
extern int writeback_unmount_begin_revoked(struct mount *, struct writeback_unmount *) __attribute__((weak));
extern void writeback_unmount_finish(struct writeback_unmount *, int) __attribute__((weak));

extern unsigned vm_object_cache_drain(struct mount *) __attribute__((weak));
extern int vm_object_sync_mount_buffer(struct mount *, void *, size_t) __attribute__((weak));
extern int vm_object_discard_mount_refs(struct mount *, struct inode *, unsigned *) __attribute__((weak));
extern int vm_object_discard_mount(struct mount *, uint64_t *) __attribute__((weak));

extern void io_error_record(struct io_error_state *, int) __attribute__((weak));
extern void io_error_snapshot(struct io_error_state *, struct io_error_snapshot *) __attribute__((weak));
extern int io_error_observe(const struct io_error_snapshot *, volatile uint64_t *) __attribute__((weak));

static struct mount mounts[MOUNT_MAX] __attribute__((section(".vfs_bss")));
static uint8_t mount_used[MOUNT_MAX] __attribute__((section(".vfs_bss")));
static struct mount *mount_head;
static struct mount *root_mount;
static const struct filesystem_type *filesystems[FILESYSTEM_MAX]
	__attribute__((section(".vfs_bss")));
static unsigned filesystem_count;
static struct spinlock namespace_lock;
static struct mutex namespace_transaction;

static int mount_io_quiesce(struct mount *mountp, struct mount_io_boundary *boundary);
static int mount_io_quiesce_mode(struct mount *, struct mount_io_boundary *, int);
static void mount_io_finish(struct mount_io_boundary *boundary, int committed);
static struct mount * mount_alloc(void);
static void mount_free(struct mount *mountp);
static int identity_text_zero(const char *text, size_t capacity);
static int identity_text_valid(const char *text, size_t capacity, uint32_t flags, uint32_t field_flag);
static int filesystem_identity_valid(const struct block_identity *identity);
static const struct filesystem_type * find_type(const char *name, struct disk *disk, int *probe_error);
static int mount_filesystem_on_disk(struct mount *mountp, const char *type_name, struct disk *disk, int flags, void *data);
static int mount_filesystem(struct mount *mountp, const char *type_name, int flags, void *data);
static int valid_component(const char *name);
static void link_child(struct mount *parent, struct mount *child);
static void link_global(struct mount *mountp);
static int set_mount_path(struct mount *mountp, const struct path *directory, const char *name);
static MOUNT_HIGH int valid_private_path(const char *path);
static void unlink_child(struct mount *mountp);
static int prepare_filesystem_destroy(struct mount *mountp, unsigned expected_refs);
static void finalize_filesystem_destroy(struct mount *mountp);
static void detach_mount(struct mount *mountp);
static int reserve_mount(struct mount *mountp, const struct path *directory, const char *name, const struct inode *expected);
static int mount_at_target(const char *type_name, const struct path *directory, const char *name, int flags, void *data, struct mount **result, const struct inode *expected);
static int unmount_owned(struct mount *mountp);
static int unmount_revoked_owned(struct mount *mountp);
static int same_inode(const struct inode *left, const struct inode *right);
static void unlink_global(struct mount *mountp);

/*
 * Enters a mount's VFS namespace transaction.
 */
void
mount_vfs_transaction_enter(
	struct mount *mountp)
{
	if (mountp != NULL)
		mutex_lock(mountp->m_vfs_transaction_lock);
}

/*
 * Leaves a mount's VFS namespace transaction.
 */
void
mount_vfs_transaction_leave(
	struct mount *mountp)
{
	if (mountp != NULL)
		mutex_unlock(mountp->m_vfs_transaction_lock);
}

/*
 * Clears a path.
 */
void
path_init(
	struct path *path)
{
	if (path != NULL)
		memset(path, 0, sizeof(*path));
}

/*
 * Sets a path to a mount and inode, taking references to both.
 */
void
path_set(
	struct path *path,
	struct mount *mountp,
	struct inode *inode)
{
	/* Ignores a missing path. */
	if (path == NULL)
		return;

	/* Stores and references the pair. */
	path->p_mount = mountp;
	path->p_inode = inode;
	if (mountp != NULL)
		mount_ref(mountp);
	if (inode != NULL)
		inode_ref(inode);
}

/*
 * Takes another reference to a path's mount and inode.
 */
void
path_ref(
	struct path *path)
{
	if (path != NULL)
		path_set(path, path->p_mount, path->p_inode);
}

/*
 * Drops a path's references and clears it.
 */
void
path_release(
	struct path *path)
{
	/* Ignores a missing path. */
	if (path == NULL)
		return;

	/* Drops the references and clears the pair. */
	if (path->p_inode != NULL)
		inode_release(path->p_inode);
	if (path->p_mount != NULL)
		mount_release(path->p_mount);
	path_init(path);
}

/*
 * Tests whether two paths name the same mount and inode.
 */
int
path_equal(
	const struct path *left,
	const struct path *right)
{
	if (left == NULL || right == NULL)
		return 0;
	if (left->p_mount != right->p_mount)
		return 0;
	if (left->p_inode == right->p_inode)
		return 1;
	if (same_inode(left->p_inode, right->p_inode))
		return 1;

	/* Reports distinct namespace paths. */
	return 0;
}

/*
 * Registers a filesystem type by name.
 */
int
filesystem_register(
	const struct filesystem_type *type)
{
	unsigned long irq;
	unsigned i;

	/* Rejects a type without a name or mount routine. */
	if (type == NULL || type->fs_name == NULL || type->mount == NULL)
		return EINVAL;

	/* The name must be new and the registry not full. */
	irq = spin_lock_irqsave(&namespace_lock);

	for (i = 0; i < filesystem_count; i++) {
		if (!strcmp(filesystems[i]->fs_name, type->fs_name)) {
			spin_unlock_irqrestore(&namespace_lock, irq);
			return EEXIST;
		}
	}

	if (filesystem_count >= FILESYSTEM_MAX) {
		spin_unlock_irqrestore(&namespace_lock, irq);
		return ENOSPC;
	}

	filesystems[filesystem_count++] = type;

	spin_unlock_irqrestore(&namespace_lock, irq);

	/* Reports the registered type. */
	return 0;
}

/*
 * Identifies the filesystem on a disk.
 *
 * Every registered type that can identify is asked; exactly one must
 * claim the disk with a well-formed identity.
 */
int
filesystem_identify(
	struct disk *disk,
	struct block_identity *identity)
{
	const struct filesystem_type *snapshot[FILESYSTEM_MAX];
	struct block_identity candidate;
	struct block_identity match;
	unsigned snapshot_count;
	unsigned match_count;
	unsigned long irq;
	unsigned index;
	int saved_error;
	const struct filesystem_type *type;
	int error;

	snapshot_count = 0;
	match_count = 0;
	saved_error = EOPNOTSUPP;

	/* Rejects a missing identity or disk. */
	if (identity == NULL)
		return EINVAL;
	memset(identity, 0, sizeof(*identity));
	if (disk == NULL)
		return EINVAL;

	/* Identity callbacks perform I/O, so only snapshot the registry locked. */
	irq = spin_lock_irqsave(&namespace_lock);

	for (index = 0; index < filesystem_count; index++) {
		type = filesystems[index];
		if ((type->fs_flags & FILESYSTEM_NODEV) != 0 ||
		    type->identify == NULL)
			continue;
		snapshot[snapshot_count++] = type;
	}

	spin_unlock_irqrestore(&namespace_lock, irq);

	/* Asks each type, keeping the first real error and refusing two claims. */
	for (index = 0; index < snapshot_count; index++) {
		memset(&candidate, 0, sizeof(candidate));
		error = snapshot[index]->identify(disk, &candidate);
		if (error == EOPNOTSUPP)
			continue;
		if (error == 0 && !filesystem_identity_valid(&candidate))
			error = EINVAL;
		if (error != 0) {
			if (saved_error == EOPNOTSUPP)
				saved_error = error;
			continue;
		}

		if (match_count != 0)
			return EEXIST;
		match_count++;
		match = candidate;
	}

	/* Reports the single claim, or the first error. */
	if (match_count == 0)
		return saved_error;
	*identity = match;
	return 0;
}

/*
 * Empties the mount namespace and the caches over it.
 */
void
mount_reset(
	void)
{
	namecache_reset();
	inode_cache_reset();
	spin_init(&namespace_lock, LOCK_RANK_NAMESPACE, "mount namespace");
	(void)mutex_init(&namespace_transaction, LOCK_RANK_VFS_TRANSACTION,
	    "VFS namespace transaction");
	memset(mounts, 0, sizeof(mounts));
	memset(mount_used, 0, sizeof(mount_used));
	memset(filesystems, 0, sizeof(filesystems));
	mount_head = NULL;
	root_mount = NULL;
	filesystem_count = 0;
}

/*
 * Tests whether a writable live mount overlaps a disk's blocks.
 */
int
mount_disk_writable_busy(
	struct disk *disk)
{
	struct disk *leaf;
	struct disk *last_leaf;
	struct disk *candidate_leaf;
	struct disk *candidate_last_leaf;
	uint64_t first;
	uint64_t last;
	uint64_t candidate_first;
	uint64_t candidate_last;
	struct mount *mountp;
	unsigned index;
	unsigned long irq;
	int busy;

	busy = 0;

	/* The disk must map onto one leaf. */
	if (disk == NULL || disk->d_block_count == 0)
		return EINVAL;
	if (disk_resolve_range(disk, 0, 1, &leaf, &first) != 0)
		return EINVAL;
	if (disk_resolve_range(disk, disk->d_block_count - 1U, 1, &last_leaf,
	    &last) != 0)
		return EINVAL;
	if (leaf != last_leaf)
		return EINVAL;

	/* Compares against every writable live mount on the same leaf. */
	irq = spin_lock_irqsave(&namespace_lock);

	for (index = 0; index < MOUNT_MAX; index++) {
		if (!mount_used[index])
			continue;
		mountp = &mounts[index];
		if (mountp->m_state != MOUNT_STATE_LIVE ||
		    mountp->m_disk == NULL ||
		    (mountp->m_flags & MOUNT_READ_ONLY) != 0 ||
		    mountp->m_disk->d_block_count == 0)
			continue;
		if (disk_resolve_range(mountp->m_disk, 0, 1, &candidate_leaf,
		    &candidate_first) != 0)
			continue;
		if (disk_resolve_range(mountp->m_disk,
		    mountp->m_disk->d_block_count - 1U, 1,
		    &candidate_last_leaf, &candidate_last) != 0)
			continue;
		if (candidate_leaf != candidate_last_leaf)
			continue;
		if (candidate_leaf == leaf &&
		    candidate_first <= last &&
		    first <= candidate_last) {
			busy = 1;
			break;
		}
	}

	spin_unlock_irqrestore(&namespace_lock, irq);

	/* Reports an overlap as busy. */
	if (busy)
		return EBUSY;
	return 0;
}

/*
 * Takes a reference to a mount.
 */
void
mount_ref(
	struct mount *mountp)
{
	if (mountp != NULL)
		refcount_get(&mountp->m_refs);
}

/*
 * Drops a reference to a mount; the table slot outlives the last one.
 */
void
mount_release(
	struct mount *mountp)
{
	if (mountp != NULL)
		(void)refcount_put_not_last(&mountp->m_refs);
}

/*
 * Mounts a filesystem as the root of the namespace.
 */
int
mount_root_create(
	const char *type_name,
	int flags,
	void *data,
	struct mount **result)
{
	struct mount *mountp;
	unsigned long irq;
	int error;
	int entered;

	/* Rejects an absent filesystem type before allocating a root slot. */
	if (type_name == NULL)
		return EINVAL;
	mountp = mount_alloc();
	if (mountp == NULL)
		return ENOSPC;
	entered = mount_vfs_transaction_join(mountp);
	irq = spin_lock_irqsave(&namespace_lock);

	if (root_mount != NULL)
		error = EBUSY;
	else
		error = 0;

	/* Reserves the root slot before filesystem I/O releases the gate. */
	if (error == 0)
		root_mount = mountp;

	spin_unlock_irqrestore(&namespace_lock, irq);

	if (entered)
		mount_vfs_transaction_leave(mountp);
	if (error != 0) {
		mount_free(mountp);
		return error;
	}

	strcpy(mountp->m_path, "/");
	error = mount_filesystem(mountp, type_name, flags, data);
	entered = mount_vfs_transaction_join(mountp);
	irq = spin_lock_irqsave(&namespace_lock);

	if (error != 0) {
		root_mount = NULL;
		spin_unlock_irqrestore(&namespace_lock, irq);
		if (entered)
			mount_vfs_transaction_leave(mountp);
		mount_free(mountp);
		return error;
	}

	/* Publishes the prepared root while the reservation remains owned. */
	mount_head = mountp;
	mountp->m_state = MOUNT_STATE_LIVE;

	spin_unlock_irqrestore(&namespace_lock, irq);

	if (entered)
		mount_vfs_transaction_leave(mountp);
	backing_mutation_end(&mountp->m_backing_guard);
	if (result != NULL)
		*result = mountp;

	/* Reports the mounted root. */
	return 0;
}

/*
 * Takes a reference to the live root mount, or reports none.
 */
struct mount *
mount_root_get_ref(
	void)
{
	unsigned long irq;
	struct mount *mountp;

	irq = spin_lock_irqsave(&namespace_lock);

	/* Only a live root can be referenced. */
	mountp = root_mount;
	if (mountp != NULL && mountp->m_state == MOUNT_STATE_LIVE)
		mount_ref(mountp);
	else
		mountp = NULL;

	spin_unlock_irqrestore(&namespace_lock, irq);

	/* Reports the referenced root, or NULL. */
	return mountp;
}

/*
 * Mounts a filesystem on a named entry of a directory.
 */
int
mount_at(
	const char *type_name,
	const struct path *directory,
	const char *name,
	int flags,
	void *data,
	struct mount **result)
{
	int error;

	/* Bootstrap mounts may create a namespace attachment without a covered inode. */
	error = mount_at_target(type_name, directory, name, flags, data, result, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the requested bootstrap attachment is published. */
	return 0;
}

/*
 * Binds an inode of one mount under a name in a directory.
 */
int
mount_bind_at(
	const struct path *source,
	const struct path *directory,
	const char *name,
	struct mount **result)
{
	struct mount *mountp;
	unsigned long irq;
	int error;
	int entered;

	/* Rejects a missing source, a non-directory target, or an unusable name. */
	if (source == NULL ||
	    source->p_mount == NULL ||
	    source->p_inode == NULL ||
	    directory == NULL ||
	    directory->p_mount == NULL ||
	    directory->p_inode == NULL ||
	    directory->p_inode->i_type != INODE_DIR ||
	    !valid_component(name))
		return EINVAL;

	/* Fills a slot that shares the source mount's type, flags, and transaction lock. */
	mountp = mount_alloc();
	if (mountp == NULL)
		return ENOSPC;
	entered = mount_vfs_transaction_join(mountp);
	irq = spin_lock_irqsave(&namespace_lock);

	error = source->p_mount->m_state == MOUNT_STATE_LIVE ? 0 : EBUSY;

	spin_unlock_irqrestore(&namespace_lock, irq);

	if (error == 0 && (source->p_inode->i_flags & INODE_DEAD) != 0)
		error = ENOENT;
	if (error == 0)
		error = reserve_mount(mountp, directory, name, NULL);
	if (error != 0)
		goto fail;
	mountp->m_bind_source = source->p_mount;
	mount_ref(mountp->m_bind_source);
	mountp->m_internal_flags = MOUNT_BIND_INTERNAL;
	mountp->m_flags = source->p_mount->m_flags;
	mountp->m_type = source->p_inode->i_mount->m_type;
	mountp->m_root = source->p_inode;
	inode_ref(mountp->m_root);

	/* Publishes it in the tree. */
	irq = spin_lock_irqsave(&namespace_lock);

	mountp->m_state = MOUNT_STATE_LIVE;

	spin_unlock_irqrestore(&namespace_lock, irq);

	inode_dir_changed(directory->p_inode);
	if (entered)
		mount_vfs_transaction_leave(mountp);
	if (result != NULL)
		*result = mountp;

	/* Reports the bound mount. */
	return 0;
fail:
	if (entered)
		mount_vfs_transaction_leave(mountp);
	mount_free(mountp);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Mounts a disk privately, outside the namespace tree.
 */
MOUNT_HIGH int
mount_private(
	const char *type_name,
	struct disk *disk,
	int flags,
	void *data,
	struct mount **result)
{
	struct mount *mountp;
	unsigned long irq;
	int error;

	/* Rejects a missing type, disk, or result. */
	if (type_name == NULL || disk == NULL || result == NULL)
		return EINVAL;
	*result = NULL;

	/* Mounts the filesystem into a slot marked private. */
	mountp = mount_alloc();
	if (mountp == NULL)
		return ENOSPC;
	mountp->m_internal_flags = MOUNT_PRIVATE_INTERNAL;
	error = mount_filesystem_on_disk(mountp, type_name, disk, flags, data);
	if (error != 0) {
		mount_free(mountp);
		return error;
	}

	irq = spin_lock_irqsave(&namespace_lock);

	mountp->m_state = MOUNT_STATE_LIVE;

	spin_unlock_irqrestore(&namespace_lock, irq);

	backing_mutation_end(&mountp->m_backing_guard);
	*result = mountp;

	/* Reports the private mount. */
	return 0;
}

/*
 * Makes a childless private mount the root of the namespace.
 */
MOUNT_HIGH int
mount_private_promote_root(
	struct mount *mountp,
	struct mount **result)
{
	unsigned long irq;
	int error;
	int entered;

	error = 0;

	/* Rejects anything but a private mount. */
	if (!mount_is_private(mountp))
		return EINVAL;

	/* Requires no existing root and a live, childless mount. */
	entered = mount_vfs_transaction_join(mountp);
	irq = spin_lock_irqsave(&namespace_lock);

	if (root_mount != NULL)
		error = EBUSY;
	else if (mountp->m_state != MOUNT_STATE_LIVE ||
	    mountp->m_children != NULL)
		error = EBUSY;
	if (error == 0) {
		strcpy(mountp->m_path, "/");
		mountp->m_name[0] = '\0';
		mountp->m_internal_flags &= ~MOUNT_PRIVATE_INTERNAL;
		mountp->m_parent = NULL;
		mountp->m_sibling = NULL;
		mountp->m_next = NULL;
		mount_head = mountp;
		root_mount = mountp;
	}

	spin_unlock_irqrestore(&namespace_lock, irq);

	if (entered)
		mount_vfs_transaction_leave(mountp);
	if (error == 0 && result != NULL)
		*result = mountp;

	/* Reports why the promotion failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Resolves a relative path inside a private mount.
 */
MOUNT_HIGH int
mount_private_lookup(
	struct mount *mountp,
	const char *relative,
	struct path *result)
{
	struct cwdinfo context;
	struct path root;
	unsigned long irq;
	int error;

	/* Rejects anything but a private mount, a missing result, or an unusable path. */
	if (!mount_is_private(mountp) ||
	    result == NULL ||
	    !valid_private_path(relative))
		return EINVAL;

	/* Resolves from a temporary working directory at the mount's root. */
	path_init(&root);
	irq = spin_lock_irqsave(&namespace_lock);

	if (mountp->m_state != MOUNT_STATE_LIVE) {
		spin_unlock_irqrestore(&namespace_lock, irq);
		return EBUSY;
	}

	path_set(&root, mountp, mountp->m_root);

	spin_unlock_irqrestore(&namespace_lock, irq);

	error = cwdinfo_init(&context, &root);
	path_release(&root);
	if (error != 0)
		return error;
	error = namei_path_at(&context, relative, result);
	cwdinfo_destroy(&context);

	/* Reports why the lookup failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Tests whether a mount is private.
 */
MOUNT_HIGH int
mount_is_private(
	const struct mount *mountp)
{
	if (mountp == NULL)
		return 0;
	if ((mountp->m_internal_flags & MOUNT_PRIVATE_INTERNAL) == 0)
		return 0;
	return 1;
}

/*
 * Flushes a mount's filesystem.
 */
int
mount_sync_backend(
	struct mount *mountp)
{
	int error;

	/* Rejects a missing backend before dispatching a filesystem-only barrier. */
	if (mountp == NULL || mountp->m_type == NULL)
		return EINVAL;
	error = 0;
	if (mountp->m_type->sync != NULL)
		error = mountp->m_type->sync(mountp);

	/* Retains failures without certifying VM data or consuming an observer cursor. */
	if (error != 0) {
		io_epoch_mark(&mountp->m_write_epoch);
		if (error != EINVAL && error != EBADF && error != EOPNOTSUPP &&
		    io_error_record != NULL)
			io_error_record(&mountp->m_write_error, error);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Drains VM content and the filesystem before certifying a mount sync frontier.
 */
int
mount_sync(
	struct mount *mountp)
{
	int error;

	/* Uses optional shared scratch for ordinary callers. */

	/* Reports the failure. */
	error = mount_sync_buffer(mountp, NULL, 0);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Drains and certifies a mount with independently reserved worker scratch.
 */
int
mount_sync_buffer(
	struct mount *mountp,
	void *scratch,
	size_t capacity)
{
	int error;
	int observed;
	struct io_error_snapshot snapshot;
	uint64_t target;

	/* Rejects a mount without a type. */
	if (mountp == NULL || mountp->m_type == NULL)
		return EINVAL;

	target = io_epoch_target(&mountp->m_write_epoch);

	/* Drains authoritative VM data before publishing the filesystem barrier. */
	error = 0;
	if (vm_object_sync_mount_buffer != NULL)
		error = vm_object_sync_mount_buffer(mountp, scratch, capacity);
	if (error == 0 && mountp->m_type->sync != NULL)
		error = mountp->m_type->sync(mountp);
	if (error == 0)
		io_epoch_complete(&mountp->m_write_epoch, target);
	else {
		io_epoch_mark(&mountp->m_write_epoch);
		if (error != EINVAL && error != EBADF && error != EOPNOTSUPP &&
		    io_error_record != NULL)
			io_error_record(&mountp->m_write_error, error);
	}

	/* Mount sync observes independently of every open file description. */
	if (io_error_snapshot != NULL && io_error_observe != NULL) {
		io_error_snapshot(&mountp->m_write_error, &snapshot);
		if (error == 0 || error == snapshot.error) {
			observed = io_error_observe(&snapshot, &mountp->m_write_error_cursor);
			if (error == 0)
				error = observed;
		}
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Flushes every live non-bind mount, reporting the first error.
 */
int
mount_sync_all(
	void)
{
	struct mount *snapshot[MOUNT_MAX];
	unsigned count;
	unsigned index;
	unsigned long irq;
	struct mount *mountp;
	int first_error;
	int error;

	count = 0;
	first_error = 0;
	irq = spin_lock_irqsave(&namespace_lock);

	/* References keep the snapshot valid while slow filesystem sync runs. */
	for (mountp = mount_head; mountp != NULL && count < MOUNT_MAX;
	     mountp = mountp->m_next) {
		if (mountp->m_state != MOUNT_STATE_LIVE ||
		    mountp->m_bind_source != NULL)
			continue;
		mount_ref(mountp);
		snapshot[count++] = mountp;
	}

	spin_unlock_irqrestore(&namespace_lock, irq);

	/* Syncs each mount and drops its reference. */
	for (index = 0; index < count; index++) {
		error = mount_sync(snapshot[index]);
		if (first_error == 0 && error != 0)
			first_error = error;
		mount_release(snapshot[index]);
	}

	/* Reports the first error. */
	return first_error;
}

/*
 * Fills a statvfs for a mount.
 *
 * A bind mount reports its source's figures; a type without a statvfs
 * routine reports its disk geometry.
 */
int
mount_statvfs(
	struct mount *mountp,
	struct statvfs *result)
{
	int error;

	error = 0;

	/* Rejects a missing mount, result, or type. */
	if (mountp == NULL || result == NULL || mountp->m_type == NULL)
		return EINVAL;

	/* A bind mount reports its source, with its own flags. */
	if (mountp->m_bind_source != NULL) {
		error = mount_statvfs(mountp->m_bind_source, result);
		if (error != 0)
			return error;
		goto flags;
	}

	/* Asks the filesystem, or derives figures from the disk. */
	memset(result, 0, sizeof(*result));
	if (mountp->m_type->statvfs != NULL) {
		error = mountp->m_type->statvfs(mountp, result);
	} else {
		if (mountp->m_disk != NULL)
			result->f_bsize = mountp->m_disk->d_block_size;
		else
			result->f_bsize = 1U;
		result->f_frsize = result->f_bsize;
		if (mountp->m_disk != NULL)
			result->f_blocks = mountp->m_disk->d_block_count;
		else
			result->f_blocks = 0;
		result->f_namemax = NAME_MAX;
	}

	if (error != 0)
		return error;

	/* The filesystem id is the device number, or the slot number without a disk. */
	if (mountp->m_disk != NULL)
		result->f_fsid = mountp->m_disk->d_dev;
	else
		result->f_fsid = (uint64_t)(1U + (unsigned)(mountp - mounts));
flags:
	/* The mount flags override whatever the filesystem reported. */
	result->f_flag &= ~((uint64_t)ST_RDONLY | (uint64_t)ST_NOSUID);
	if ((mountp->m_flags & MOUNT_READ_ONLY) != 0 ||
	    (mountp->m_disk != NULL &&
	    (mountp->m_disk->d_flags & DISK_READ_ONLY) != 0))
		result->f_flag |= ST_RDONLY;
	if ((mountp->m_flags & MOUNT_NOSUID) != 0)
		result->f_flag |= ST_NOSUID;
	if (result->f_namemax == 0)
		result->f_namemax = NAME_MAX;

	/* Reports the filled structure. */
	return 0;
}

/*
 * Forwards a quota control request to a mount's filesystem.
 */
int
mount_quotactl(
	struct mount *mountp,
	struct quota_control *request)
{
	int error;

	/* Rejects a missing mount or request. */
	if (mountp == NULL || request == NULL)
		return EINVAL;

	/* A bind mount forwards to its source. */
	if ((mountp->m_internal_flags & MOUNT_BIND_INTERNAL) != 0 &&
	    mountp->m_bind_source != NULL) {
		error = mount_quotactl(mountp->m_bind_source, request);
		return error;
	}

	/* The filesystem must support quotas. */
	if (mountp->m_type == NULL || mountp->m_type->quotactl == NULL)
		return EOPNOTSUPP;

	/* Reports the failure. */
	error = mountp->m_type->quotactl(mountp, request);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Forwards a snapshot control request to a mount's filesystem.
 */
int
mount_snapshotctl(
	struct mount *mountp,
	struct snapshot_control *request)
{
	int error;

	/* Rejects a missing mount or request. */
	if (mountp == NULL || request == NULL)
		return EINVAL;

	/* A bind mount forwards to its source. */
	if ((mountp->m_internal_flags & MOUNT_BIND_INTERNAL) != 0 &&
	    mountp->m_bind_source != NULL) {
		error = mount_snapshotctl(mountp->m_bind_source, request);
		return error;
	}

	/* The filesystem must support snapshots. */
	if (mountp->m_type == NULL || mountp->m_type->snapshotctl == NULL)
		return EOPNOTSUPP;

	/* Reports the failure. */
	error = mountp->m_type->snapshotctl(mountp, request);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Mounts a filesystem on a top-level directory of the root.
 */
int
mount(
	const char *type_name,
	const char *dir,
	int flags,
	void *data)
{
	struct path root;
	struct mount *rootp;
	int error;

	/* Requires a live root and a single-component absolute directory. */
	rootp = mount_root_get_ref();
	if (rootp == NULL ||
	    dir == NULL ||
	    dir[0] != '/' ||
	    dir[1] == '\0' ||
	    strchr(dir + 1, '/') != NULL) {
		mount_release(rootp);
		return EINVAL;
	}

	/* Mounts under the root directory. */
	path_set(&root, rootp, rootp->m_root);
	mount_release(rootp);
	error = mount_at(type_name, &root, dir + 1, flags, data, NULL);
	path_release(&root);

	/* Reports why the mount failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Mounts on an existing directory within one snapshot of a process's root.
 */
int
mount_context(
	struct cwdinfo *context,
	const char *type_name,
	const char *directory,
	int flags,
	void *data)
{
	struct cwdinfo *snapshot;
	struct path target;
	struct path parent;
	struct componentname component;
	char canonical[KERN_PATH_MAX];
	char name[NAME_MAX + 1U];
	int error;

	/* A private directory-state copy prevents a concurrent chdir from retargeting us. */
	snapshot = NULL;
	path_init(&target);
	path_init(&parent);
	error = cwdinfo_clone(context, &snapshot);
	if (error != 0)
		return error;
	error = namei_path_at(snapshot, directory, &target);
	if (error == 0 && target.p_inode->i_type != INODE_DIR)
		error = ENOTDIR;
	if (error == 0 && same_inode(target.p_inode, target.p_mount->m_root))
		error = EBUSY;

	/* Resolve final symlinks and dot components before identifying the attachment parent. */
	if (error == 0)
		error = fs_chdir_path(snapshot, &target);
	if (error == 0)
		error = fs_getcwd(snapshot, canonical, sizeof(canonical));
	if (error == 0 && !strcmp(canonical, "/"))
		error = EBUSY;
	if (error == 0)
		error = namei_parent_path_at(snapshot, canonical, &parent,
		    &component, name);
	if (error == 0)
		error = mount_at_target(type_name, &parent, name, flags, data,
		    NULL, target.p_inode);

	/* Reservation checked the covered identity before publishing or unwinding. */
	path_release(&parent);
	path_release(&target);
	cwdinfo_release(snapshot);
	if (error != 0)
		return error;

	/* Succeeded: the resolved directory is covered by the new mount. */
	return 0;
}

/*
 * Unmounts the resolved mount root without looking up its spelling again.
 */
int
unmount_context(
	struct cwdinfo *context,
	const char *directory)
{
	return unmount_context_flags(context, directory, 0);
}

int
unmount_context_flags(
	struct cwdinfo *context,
	const char *directory,
	int flags)
{
	struct cwdinfo *snapshot;
	struct path target;
	struct mount *mountp;
	int error;

	if ((flags & ~(int)MNT_FORCE) != 0)
		return EINVAL;

	/* Resolve under one process root/cwd snapshot, including symbolic links. */
	snapshot = NULL;
	mountp = NULL;
	path_init(&target);
	error = cwdinfo_clone(context, &snapshot);
	if (error != 0)
		return error;
	error = namei_path_at(snapshot, directory, &target);
	if (error == 0 && !same_inode(target.p_inode, target.p_mount->m_root))
		error = EINVAL;
	if (error == 0) {
		mountp = target.p_mount;
		mount_ref(mountp);
	}

	/* Drop temporary inode/context owners, retaining the mount throughout teardown. */
	path_release(&target);
	cwdinfo_release(snapshot);
	if (error != 0)
		return error;
	if (flags & MNT_FORCE)
		error = unmount_revoked_owned(mountp);
	else
		error = unmount_owned(mountp);
	if (error != 0)
		return error;

	/* Succeeded: teardown consumed the operation's held mount reference. */
	return 0;
}

/*
 * Finds a live mount by path and takes a reference.
 */
struct mount *
mount_find_ref(
	const char *path)
{
	struct mount *mountp;
	unsigned long irq;

	/* Rejects a missing path. */
	if (path == NULL)
		return NULL;

	/* Searches the global list for a live mount with the path. */
	irq = spin_lock_irqsave(&namespace_lock);

	for (mountp = mount_head; mountp != NULL; mountp = mountp->m_next) {
		if (mountp->m_state == MOUNT_STATE_LIVE &&
		    !strcmp(mountp->m_path, path)) {
			mount_ref(mountp);
			spin_unlock_irqrestore(&namespace_lock, irq);
			return mountp;
		}
	}

	spin_unlock_irqrestore(&namespace_lock, irq);

	/* Reports no match. */
	return NULL;
}

/*
 * Reports the mount an inode belongs to.
 */
struct mount *
mount_for_inode(
	const struct inode *inode)
{
	if (inode == NULL)
		return NULL;
	return inode->i_mount;
}

/*
 * Finds the mount covering a named entry of a directory.
 */
int
mount_lookup_child(
	const struct path *directory,
	const struct componentname *component,
	struct path *result)
{
	struct mount *child;
	unsigned long irq;
	size_t length;

	/* Rejects a missing directory mount, component, or result. */
	if (directory == NULL || directory->p_mount == NULL ||
	    component == NULL || result == NULL)
		return EINVAL;

	/* Searches the children covering this directory inode by name. */
	irq = spin_lock_irqsave(&namespace_lock);

	for (child = directory->p_mount->m_children; child != NULL;
	     child = child->m_sibling) {
		length = strlen(child->m_name);
		if (child->m_cover.p_mount == directory->p_mount &&
		    same_inode(child->m_cover.p_inode, directory->p_inode) &&
		    length == component->cn_namelen &&
		    !memcmp(child->m_name, component->cn_nameptr, length)) {
			if (child->m_state != MOUNT_STATE_LIVE) {
				spin_unlock_irqrestore(&namespace_lock, irq);
				return EBUSY;
			}

			path_set(result, child, child->m_root);
			spin_unlock_irqrestore(&namespace_lock, irq);
			return 0;
		}
	}

	spin_unlock_irqrestore(&namespace_lock, irq);

	/* Reports no covering mount. */
	return ENOENT;
}

/*
 * Steps from a mount root to the directory it covers.
 */
int
mount_cross_path_parent(
	const struct path *current,
	struct path *result)
{
	unsigned long irq;

	/* Rejects a missing path or result. */
	if (current == NULL || current->p_mount == NULL || result == NULL)
		return EINVAL;

	/* Only a covered mount root has a parent to cross into. */
	irq = spin_lock_irqsave(&namespace_lock);

	if (!same_inode(current->p_inode, current->p_mount->m_root) ||
	    current->p_mount == root_mount ||
	    current->p_mount->m_cover.p_inode == NULL) {
		spin_unlock_irqrestore(&namespace_lock, irq);
		return ENOENT;
	}

	path_set(result, current->p_mount->m_cover.p_mount,
		 current->p_mount->m_cover.p_inode);

	spin_unlock_irqrestore(&namespace_lock, irq);

	/* Reports the covered directory. */
	return 0;
}

/*
 * Reads the next child mount of a directory as a directory entry.
 */
int
mount_readdir_child(
	const struct path *directory,
	unsigned *cursor,
	struct dirent *entry)
{
	struct mount *child;
	unsigned long irq;
	unsigned index;

	index = 0;

	/* Rejects a missing directory, cursor, or entry. */
	if (directory == NULL || cursor == NULL || entry == NULL)
		return EINVAL;

	/* Skips to the child at the cursor position. */
	irq = spin_lock_irqsave(&namespace_lock);

	for (child = directory->p_mount->m_children; child != NULL;
	     child = child->m_sibling) {
		if (child->m_cover.p_mount != directory->p_mount ||
		    !same_inode(child->m_cover.p_inode, directory->p_inode))
			continue;
		if (child->m_state != MOUNT_STATE_LIVE) {
			spin_unlock_irqrestore(&namespace_lock, irq);
			return EBUSY;
		}

		if (index != *cursor) {
			index++;
			continue;
		}

		memset(entry, 0, sizeof(*entry));
		entry->d_ino = child->m_root->i_ino;
		entry->d_type = child->m_root->i_type;
		strcpy(entry->d_name, child->m_name);
		(*cursor)++;
		spin_unlock_irqrestore(&namespace_lock, irq);
		return 0;
	}

	spin_unlock_irqrestore(&namespace_lock, irq);

	/* Reports the end of the children. */
	return ENOENT;
}

/*
 * Tests whether a mount covers a named entry of a directory.
 */
int
mount_child_shadows(
	const struct path *directory,
	const char *name)
{
	struct componentname component;
	struct path found;
	int error;

	/* Looks the name up among the covering mounts. */
	component.cn_nameptr = name;
	component.cn_namelen = strlen(name);
	component.cn_flags = 0;
	error = mount_lookup_child(directory, &component, &found);
	if (error == 0)
		path_release(&found);

	/* Keeps preparing and dying attachments visible as reserved names. */
	if (error == 0 || error == EBUSY)
		return 1;

	/* Reports an unshadowed child name. */
	return 0;
}

/*
 * Unmounts a private mount.
 */
MOUNT_HIGH int
unmount_private(
	struct mount *mountp)
{
	struct mount_io_boundary boundary = { 0 };
	unsigned expected_refs;
	unsigned long irq;
	int error, entered;

	if (!mount_is_private(mountp))
		return EINVAL;

	/* Joins optional reads before draining VM ownership or counting mount references. */
	error = mount_io_quiesce(mountp, &boundary);
	if (error != 0)
		return error;
	expected_refs = 1;
	if (boundary.writeback.mount != NULL)
		expected_refs++;

	/* Drains dirty cache handles before counting external mount references. */
	if (vm_object_sync_mount_buffer != NULL) {
		error = vm_object_sync_mount_buffer(mountp, NULL, 0);
		if (error != 0) {
			mount_io_finish(&boundary, 0);
			return error;
		}
	}

	if (vm_object_cache_drain != NULL)
		(void)vm_object_cache_drain(mountp);
	entered = mount_vfs_transaction_join(mountp);
	irq = spin_lock_irqsave(&namespace_lock);

	if (!mount_is_private(mountp) || mountp->m_state != MOUNT_STATE_LIVE ||
	    mountp->m_children != NULL || refcount_load(&mountp->m_refs) != expected_refs) {
		spin_unlock_irqrestore(&namespace_lock, irq);
		if (entered)
			mount_vfs_transaction_leave(mountp);
		mount_io_finish(&boundary, 0);
		return EBUSY;
	}

	mountp->m_state = MOUNT_STATE_DYING;

	spin_unlock_irqrestore(&namespace_lock, irq);

	if (entered)
		mount_vfs_transaction_leave(mountp);
	error = prepare_filesystem_destroy(mountp, expected_refs);
	if (error != 0) {
		irq = spin_lock_irqsave(&namespace_lock);
		mountp->m_state = MOUNT_STATE_LIVE;
		spin_unlock_irqrestore(&namespace_lock, irq);
		mount_io_finish(&boundary, 0);
		return error;
	}

	mount_io_finish(&boundary, 1);
	finalize_filesystem_destroy(mountp);
	mount_free(mountp);

	/* Reports the unmounted filesystem. */
	return 0;
}

/*
 * Counts the mounts in use, including private ones.
 */
unsigned
mount_count(
	void)
{
	unsigned i;
	unsigned count;
	unsigned long irq;

	count = 0;
	irq = spin_lock_irqsave(&namespace_lock);

	for (i = 0; i < MOUNT_MAX; i++)
		count += mount_used[i] != 0;

	spin_unlock_irqrestore(&namespace_lock, irq);

	/* Reports the sampled count. */
	return count;
}

/*
 * Unmounts the mount at a path.
 *
 * The mount must be idle with no children; the root cannot be unmounted.
 */
int
unmount(
	const char *dir,
	int flags)
{
	struct mount *mountp;
	int error;

	/* Internal callers retain the existing global-path lookup contract. */
	if ((flags & ~(int)MNT_FORCE) != 0)
		return EINVAL;
	mountp = mount_find_ref(dir);
	if (mountp == NULL)
		return ENOENT;
	if (flags & MNT_FORCE)
		error = unmount_revoked_owned(mountp);
	else
		error = unmount_owned(mountp);
	if (error != 0)
		return error;

	/* Succeeded: teardown consumed the lookup reference. */
	return 0;
}

/*
 * Snapshots live mount identities while retaining paths for reconstruction.
 */
MOUNT_HIGH int
mount_info_snapshot(
	struct kern_mount_info *entries,
	unsigned capacity,
	unsigned *count_out)
{
	struct mount *mountp, *source, *snapshot[MOUNT_MAX];
	struct path targets[MOUNT_MAX], sources[MOUNT_MAX];
	struct cwdinfo context;
	unsigned count = 0, i;
	int error = 0;
	unsigned long irq;
	struct kern_mount_info *info;

	/* Rejects a malformed request. */
	if (count_out == NULL || (capacity != 0 && entries == NULL) ||
	    capacity > KERN_MOUNT_INFO_MAX)
		return EINVAL;

	/* Counts the live mounts before deciding whether they fit. */
	irq = spin_lock_irqsave(&namespace_lock);

	for (mountp = mount_head; mountp != NULL; mountp = mountp->m_next)
		if (mountp->m_state == MOUNT_STATE_LIVE)
			count++;
	*count_out = count;
	if (count > capacity) {
		spin_unlock_irqrestore(&namespace_lock, irq);
		return ENOSPC;
	}

	count = 0;
	memset(&context, 0, sizeof(context));
	spin_init(&context.lock, LOCK_RANK_PROCESS_RESOURCE, "mount paths");
	if (root_mount != NULL && root_mount->m_state == MOUNT_STATE_LIVE)
		path_set(&context.root, root_mount, root_mount->m_root);
	for (mountp = mount_head; mountp != NULL; mountp = mountp->m_next) {
		if (mountp->m_state != MOUNT_STATE_LIVE)
			continue;
		path_set(&targets[count], mountp, mountp->m_root);
		path_init(&sources[count]);
		snapshot[count] = mountp;
		mount_ref(mountp);
		info = &entries[count++];
		memset(info, 0, sizeof(*info));
		info->flags = mountp->m_flags;
		source = mountp->m_bind_source;
		if (source != NULL) {
			info->kind = KERN_MOUNT_INFO_BIND;
			if (mount_is_private(source))
				strcpy(info->source, "(private)");
			else
				path_set(&sources[count - 1], source, mountp->m_root);
		} else {
			source = mountp;
			if (source->m_disk != NULL)
				strcpy(info->source, source->m_disk->d_name);
		}

		if (source->m_disk != NULL)
			info->device = (uint32_t)source->m_disk->d_dev;
		if (source->m_type != NULL) {
			strncpy(info->type, source->m_type->fs_name,
			    sizeof(info->type) - 1U);
		}
	}

	spin_unlock_irqrestore(&namespace_lock, irq);

	/*
	 * Mount references prevent teardown while pathname reconstruction performs
	 * directory I/O. Membership is captured together; pathname resolution has
	 * getcwd's bounded concurrent-rename semantics, not a rename transaction.
	 */
	for (i = 0; i < count && error == 0; i++) {
		path_set(&context.cwd, targets[i].p_mount, targets[i].p_inode);
		error = fs_getcwd(&context, entries[i].target,
		    sizeof(entries[i].target));
		path_release(&context.cwd);
		if (error == 0 && sources[i].p_inode != NULL) {
			path_set(&context.cwd, sources[i].p_mount, sources[i].p_inode);
			error = fs_getcwd(&context, entries[i].source,
			    sizeof(entries[i].source));
			path_release(&context.cwd);
		}
	}

	for (i = 0; i < count; i++) {
		path_release(&sources[i]);
		path_release(&targets[i]);
		mount_release(snapshot[i]);
	}

	path_release(&context.root);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Rejects mutations of mounted, bound, or ancestral namespace anchors.
 */
int
mount_namespace_check_inode(
	struct inode *inode)
{
	struct inode *anchors[MOUNT_MAX * 2U];
	struct mount *mountp;
	unsigned count = 0, index;
	unsigned long irq;
	int error = 0, ancestor;
	struct inode *cover;
	struct inode *source;

	if (inode == NULL)
		return EINVAL;
	/*
	 * Covers and bind roots are stable until the owning transaction ends.
	 * Take references under spin, then perform ancestor lookups without it.
	 * Covered-entry identity also handles alternate backend spellings.
	 */
	irq = spin_lock_irqsave(&namespace_lock);

	for (mountp = mount_head; mountp != NULL; mountp = mountp->m_next) {
		cover = mountp->m_cover.p_inode;
		source = (mountp->m_internal_flags &
		    MOUNT_BIND_INTERNAL) != 0 ? mountp->m_root : NULL;
		if (same_inode(mountp->m_covered_inode, inode)) {
			error = EBUSY;
			break;
		}

		if (cover != NULL && cover->i_mount == inode->i_mount) {
			inode_ref(cover);
			anchors[count++] = cover;
		}

		if (source != NULL && source->i_mount == inode->i_mount) {
			inode_ref(source);
			anchors[count++] = source;
		}
	}

	spin_unlock_irqrestore(&namespace_lock, irq);

	for (index = 0; index < count && error == 0; index++) {
		if (same_inode(inode, anchors[index]))
			error = EBUSY;
		else if (inode->i_type == INODE_DIR &&
		    anchors[index]->i_type == INODE_DIR) {
			error = inode_is_ancestor(inode, anchors[index], &ancestor);
			if (error == 0 && ancestor)
				error = EBUSY;
		}
	}

	for (index = 0; index < count; index++)
		inode_release(anchors[index]);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Rejects names reserved by preparing, live, or dying mounts.
 */
int
mount_namespace_check_name(
	struct inode *directory,
	const struct componentname *name)
{
	struct mount *mountp;
	unsigned long irq;
	int error = 0;

	/* Rejects a malformed name. */
	if (directory == NULL || name == NULL || name->cn_nameptr == NULL ||
	    name->cn_namelen == 0 || name->cn_namelen > NAME_MAX)
		return EINVAL;

	/* Refuses a name an existing mount already covers in that directory. */
	irq = spin_lock_irqsave(&namespace_lock);

	for (mountp = mount_head; mountp != NULL; mountp = mountp->m_next) {
		if (same_inode(mountp->m_cover.p_inode, directory) &&
		    strlen(mountp->m_name) == name->cn_namelen &&
		    !memcmp(mountp->m_name, name->cn_nameptr, name->cn_namelen)) {
			error = EBUSY;
			break;
		}
	}

	spin_unlock_irqrestore(&namespace_lock, irq);

	/* Reports whether the name is free. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Joins namespace serialization without releasing an existing owner.
 */
int
mount_vfs_transaction_join(
	struct mount *mountp)
{
	if (mountp == NULL || mountp->m_vfs_transaction_lock == NULL ||
	    mutex_owned(mountp->m_vfs_transaction_lock))
		return 0;
	mount_vfs_transaction_enter(mountp);
	return 1;
}

/* Takes a free table slot and initializes it in the preparing state. */
static struct mount *
mount_alloc(
	void)
{
	struct mount *result;
	unsigned long irq;
	unsigned i;

	result = NULL;
	irq = spin_lock_irqsave(&namespace_lock);

	/* Initializes the first unused slot. */
	for (i = 0; i < MOUNT_MAX; i++) {
		if (!mount_used[i]) {
			mount_used[i] = 1;
			memset(&mounts[i], 0, sizeof(mounts[i]));
			refcount_init(&mounts[i].m_refs, 1);
			(void)mutex_init(&mounts[i].m_lock, LOCK_RANK_NAMESPACE,
			    "mount");
			mounts[i].m_vfs_transaction_lock =
			    &namespace_transaction;
			waitq_init(&mounts[i].m_waitq, "mount state");
			mounts[i].m_state = MOUNT_STATE_PREPARING;
			result = &mounts[i];
			break;
		}
	}

	spin_unlock_irqrestore(&namespace_lock, irq);

	/* Reports the slot, or NULL when the table is full. */
	return result;
}

/* Frees a table slot once its last reference is dropped. */
static void
mount_free(
	struct mount *mountp)
{
	unsigned long irq;
	unsigned i;

	/* Only the last reference frees the slot. */
	if (mountp == NULL || refcount_load(&mountp->m_refs) != 1)
		return;
	backing_mutation_end(&mountp->m_backing_guard);
	irq = spin_lock_irqsave(&namespace_lock);

	if (refcount_load(&mountp->m_refs) != 1) {
		spin_unlock_irqrestore(&namespace_lock, irq);
		return;
	}

	/* Marks the slot dead, drops the reference, and clears it. */
	for (i = 0; i < MOUNT_MAX; i++) {
		if (&mounts[i] != mountp)
			continue;
		mountp->m_state = MOUNT_STATE_DEAD;
		if (!refcount_put(&mountp->m_refs)) {
			spin_unlock_irqrestore(&namespace_lock, irq);
			return;
		}

		memset(mountp, 0, sizeof(*mountp));
		mount_used[i] = 0;
		break;
	}

	spin_unlock_irqrestore(&namespace_lock, irq);
}

/* Tests that an identity text field is entirely zero. */
static int
identity_text_zero(
	const char *text,
	size_t capacity)
{
	size_t index;

	for (index = 0; index < capacity; index++) {
		if (text[index] != '\0')
			return 0;
	}

	return 1;
}

/* Tests that an identity text field is present or absent as its flag says. */
static int
identity_text_valid(
	const char *text,
	size_t capacity,
	uint32_t flags,
	uint32_t field_flag)
{
	int zero;

	/* An unflagged field must be zero. */
	if ((flags & field_flag) == 0) {
		zero = identity_text_zero(text, capacity);
		return zero;
	}

	/* A flagged field must be a non-empty terminated string. */
	if (text[0] == '\0')
		return 0;
	if (memchr(text, '\0', capacity) == NULL)
		return 0;
	return 1;
}

/* Tests that an identity uses only the filesystem fields and is well-formed. */
static int
filesystem_identity_valid(
	const struct block_identity *identity)
{
	const uint32_t allowed = KERN_BLKID_TYPE | KERN_BLKID_UUID |
	    KERN_BLKID_LABEL;

	/* Requires known flags, zero reserved fields and well-formed text. */
	if ((identity->flags & ~allowed) != 0 ||
	    identity->reserved != 0 ||
	    !identity_text_zero(identity->partuuid,
	    sizeof(identity->partuuid)) ||
	    !identity_text_zero(identity->partlabel,
	    sizeof(identity->partlabel)) ||
	    !identity_text_valid(identity->type, sizeof(identity->type),
	    identity->flags, KERN_BLKID_TYPE) ||
	    !identity_text_valid(identity->uuid, sizeof(identity->uuid),
	    identity->flags, KERN_BLKID_UUID) ||
	    !identity_text_valid(identity->label, sizeof(identity->label),
	    identity->flags, KERN_BLKID_LABEL))
		return 0;
	return 1;
}

/* Finds a filesystem type by name, or by probing the disk for "auto". */
static const struct filesystem_type *
find_type(
	const char *name,
	struct disk *disk,
	int *probe_error)
{
	unsigned i;
	const struct filesystem_type *type;
	int error;

	*probe_error = EOPNOTSUPP;

	/* Considers each type the name selects. */
	for (i = 0; i < filesystem_count; i++) {
		type = filesystems[i];
		if (strcmp(name, "auto") && strcmp(name, type->fs_name))
			continue;

		/* A nodev type matches only by exact name. */
		if ((type->fs_flags & FILESYSTEM_NODEV) != 0) {
			if (!strcmp(name, type->fs_name))
				return type;
			continue;
		}

		/* A disk type without a probe is taken on faith. */
		if (disk == NULL)
			continue;
		if (type->probe == NULL)
			return type;
		error = type->probe(disk);
		if (error == 0)
			return type;
		if (error != EOPNOTSUPP)
			*probe_error = error;
		if (strcmp(name, "auto"))
			break;
	}

	/* Reports no matching type. */
	return NULL;
}

/* Mounts a filesystem of a type on a disk into a prepared slot. */
static int
mount_filesystem_on_disk(
	struct mount *mountp,
	const char *type_name,
	struct disk *disk,
	int flags,
	void *data)
{
	const struct filesystem_type *type;
	unsigned check_flags;
	int error;

	/* Finds the type; "auto" without a disk has nothing to probe. */
	type = find_type(type_name, disk, &error);
	if (type == NULL) {
		if (disk == NULL && !strcmp(type_name, "auto"))
			return ENXIO;
		return error;
	}

	if (!(type->fs_flags & FILESYSTEM_NODEV) && disk == NULL)
		return ENXIO;

	/* Guards a writable disk against other claims and opens it. */
	if (disk != NULL) {
		if ((flags & MOUNT_READ_ONLY) == 0 &&
		    (disk->d_flags & DISK_READ_ONLY) == 0) {
			error = backing_mutation_begin_disk(
			    disk, 0, disk->d_block_count, NULL,
			    &mountp->m_backing_guard);
			if (error != 0)
				return error;
		}

		check_flags = (unsigned)flags;
		if ((disk->d_flags & DISK_READ_ONLY) != 0)
			check_flags |= MOUNT_READ_ONLY;
		error = backing_claim_check_mount(disk, check_flags);
		if (error != 0) {
			backing_mutation_end(&mountp->m_backing_guard);
			return error;
		}

		error = disk_open(disk);
		if (error != 0) {
			backing_mutation_end(&mountp->m_backing_guard);
			return error;
		}
	}

	/* Mounts the filesystem, which must produce a root inode. */
	mountp->m_flags = (unsigned)flags;
	if (disk != NULL && (disk->d_flags & DISK_READ_ONLY) != 0)
		mountp->m_flags |= MOUNT_READ_ONLY;
	mountp->m_disk = disk;
	mountp->m_type = type;
	mountp->m_data = data;
	error = type->mount(mountp);
	if (error != 0 || mountp->m_root == NULL) {
		if (disk != NULL)
			disk_close(disk);
		backing_mutation_end(&mountp->m_backing_guard);
		if (error != 0)
			return error;
		return EIO;
	}

	/* Reports the mounted filesystem. */
	return 0;
}

/* Mounts a filesystem by type name, resolving the disk from the mount data. */
static int
mount_filesystem(
	struct mount *mountp,
	const char *type_name,
	int flags,
	void *data)
{
	const struct fat_mount_args *args;
	struct disk *disk;
	unsigned i;
	int error;

	args = data;
	disk = NULL;

	/* A nodev filesystem owns the interpretation of its mount data. */
	for (i = 0; i < filesystem_count; i++) {
		if (!strcmp(type_name, filesystems[i]->fs_name) &&
		    (filesystems[i]->fs_flags & FILESYSTEM_NODEV) != 0) {
			error = mount_filesystem_on_disk(mountp, type_name, NULL,
				flags, data);
			return error;
		}
	}

	/* Otherwise the data names the disk. */
	if (args != NULL && args->fspec != NULL)
		disk = disk_find(args->fspec);
	error = mount_filesystem_on_disk(mountp, type_name, disk,
	    flags, data);
	disk_release(disk);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Tests that a name is a usable single path component. */
static int
valid_component(
	const char *name)
{
	size_t length;

	/* Rejects an empty name, a dot name, or one containing a separator. */
	if (name == NULL ||
	    name[0] == '\0' ||
	    !strcmp(name, ".") ||
	    !strcmp(name, "..") ||
	    strchr(name, '/') != NULL)
		return 0;

	/* Rejects a name longer than one path component may be. */
	length = strlen(name);
	if (length > NAME_MAX)
		return 0;
	return 1;
}

/* Appends a child to a parent's child list. */
static void
link_child(
	struct mount *parent,
	struct mount *child)
{
	struct mount **link;

	link = &parent->m_children;
	while (*link != NULL)
		link = &(*link)->m_sibling;
	*link = child;
}

/* Appends a mount to the global list. */
static void
link_global(
	struct mount *mountp)
{
	struct mount **link;

	link = &mount_head;
	while (*link != NULL)
		link = &(*link)->m_next;
	*link = mountp;
}

/* Builds a mount's path and name from its parent directory's mount and the entry name. */
static int
set_mount_path(
	struct mount *mountp,
	const struct path *directory,
	const char *name)
{
	struct cwdinfo context;
	char base[KERN_PATH_MAX];
	size_t base_length, name_length = strlen(name);
	int error;

	/* Resolves the covered directory to an absolute path. */
	memset(&context, 0, sizeof(context));
	spin_init(&context.lock, LOCK_RANK_PROCESS_RESOURCE, "mount path");
	path_set(&context.root, root_mount, root_mount->m_root);
	path_set(&context.cwd, directory->p_mount, directory->p_inode);
	error = fs_getcwd(&context, base, sizeof(base));
	path_release(&context.cwd);
	path_release(&context.root);
	if (error != 0)
		return error;

	/* Refuses a mount point whose path would not fit. */
	base_length = strlen(base);
	if (base_length + (base_length > 1U ? 1U : 0U) + name_length >=
	    sizeof(mountp->m_path))
		return ENAMETOOLONG;

	/* Joins the directory and the name into the mount's path. */
	strcpy(mountp->m_path, base);
	if (base_length > 1U)
		strcat(mountp->m_path, "/");
	strcat(mountp->m_path, name);
	strcpy(mountp->m_name, name);

	/* Reports the recorded path. */
	return 0;
}

/* Tests that a private-mount path is relative with no empty, dot, or dot-dot components. */
static MOUNT_HIGH int
valid_private_path(
	const char *path)
{
	const char *component;
	const char *cursor;

	/* Rejects a missing, empty, or absolute path. */
	component = path;
	if (path == NULL || path[0] == '\0' || path[0] == '/')
		return 0;

	/* Checks each component up to a slash or the terminator. */
	for (cursor = path;; cursor++) {
		if (*cursor != '/' && *cursor != '\0')
			continue;
		if (cursor == component ||
		    (cursor - component == 1 && component[0] == '.') ||
		    (cursor - component == 2 &&
		     component[0] == '.' &&
		     component[1] == '.'))
			return 0;
		if (*cursor == '\0')
			return 1;
		component = cursor + 1;
	}
}

/* Removes a mount from its parent's child list. */
static void
unlink_child(
	struct mount *mountp)
{
	struct mount **link;

	/* The root has no parent. */
	if (mountp->m_parent == NULL)
		return;

	/* Splices the mount out of the sibling list. */
	for (link = &mountp->m_parent->m_children; *link != NULL;
	     link = &(*link)->m_sibling) {
		if (*link == mountp) {
			*link = mountp->m_sibling;
			return;
		}
	}
}

/* Checks that a mount can be destroyed and asks its filesystem to prepare. */
static int
prepare_filesystem_destroy(
	struct mount *mountp,
	unsigned expected_refs)
{
	int error;

	/* Children or extra references keep the mount busy. */
	if (mountp == NULL ||
	    mountp->m_children != NULL ||
	    refcount_load(&mountp->m_refs) != expected_refs)
		return EBUSY;

	/* Drops the name cache, checks the inodes, and syncs. */
	namecache_purge_mount(mountp);
	error = inode_cache_mount_busy(mountp);
	if (error != 0)
		return error;
	error = mount_sync(mountp);
	if (error != 0)
		return error;

	/* Lets the filesystem refuse. */
	if (mountp->m_type != NULL && mountp->m_type->prepare_unmount != NULL)
		error = mountp->m_type->prepare_unmount(mountp);
	else
		error = 0;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Releases a mount's root inode, inodes, filesystem, and disk. */
static void
finalize_filesystem_destroy(
	struct mount *mountp)
{
	/* No refusal remains: drop filesystem directory owners before inode purge. */
	inode_cache_retire_namespace(mountp);

	if (mountp->m_root != NULL) {
		inode_release(mountp->m_root);
		mountp->m_root = NULL;
	}

	inode_cache_purge_mount(mountp);
	if (mountp->m_type != NULL && mountp->m_type->unmount != NULL)
		mountp->m_type->unmount(mountp);
	if (mountp->m_disk != NULL) {
		disk_close(mountp->m_disk);
		mountp->m_disk = NULL;
	}
}

/* The caller owns the transaction; release references after dropping spin. */
static void
detach_mount(
	struct mount *mountp)
{
	unsigned long irq = spin_lock_irqsave(&namespace_lock);

	/* Takes the mount out of the namespace. */
	unlink_child(mountp);
	unlink_global(mountp);
	mountp->m_state = MOUNT_STATE_DEAD;

	spin_unlock_irqrestore(&namespace_lock, irq);

	/* Uncovers the directory the mount was over. */
	path_release(&mountp->m_cover);
	if (mountp->m_covered_inode != NULL) {
		inode_release(mountp->m_covered_inode);
		mountp->m_covered_inode = NULL;
	}
}

/*
 * The transaction gate covers both admission and every filesystem mutation.
 * A PREPARING child reserves its name and anchors while mount I/O runs with
 * the gate released. Readers see EBUSY, never an uninitialized root inode.
 */
static int
reserve_mount(
	struct mount *mountp,
	const struct path *directory,
	const char *name,
	const struct inode *expected)
{
	struct componentname component = { name, strlen(name), 0 };
	struct path existing;
	unsigned long irq;
	int error;

	/* Requires a live namespace and a live directory to mount over. */
	irq = spin_lock_irqsave(&namespace_lock);

	error = directory->p_mount->m_state == MOUNT_STATE_LIVE &&
	    root_mount != NULL && root_mount->m_state == MOUNT_STATE_LIVE ?
	    0 : EBUSY;

	spin_unlock_irqrestore(&namespace_lock, irq);

	if (error != 0)
		return error;
	if ((directory->p_inode->i_flags & INODE_DEAD) != 0)
		return ENOENT;

	/* Refuses a name another mount already occupies. */
	error = mount_lookup_child(directory, &component, &existing);
	if (error == 0) {
		path_release(&existing);
		return EBUSY;
	}

	if (error != ENOENT)
		return error;

	/* Records the mount's path and the inode it covers. */
	error = set_mount_path(mountp, directory, name);
	if (error != 0)
		return error;
	error = inode_lookup(directory->p_inode, &component,
	    &mountp->m_covered_inode);
	if (error != 0 && error != ENOENT)
		return error;

	/* A public mount must still cover the directory admitted by name lookup. */
	if (expected != NULL && !same_inode(mountp->m_covered_inode, expected)) {
		inode_release(mountp->m_covered_inode);
		mountp->m_covered_inode = NULL;
		return EAGAIN;
	}

	/* Publishes the mount in the namespace. */
	path_set(&mountp->m_cover, directory->p_mount, directory->p_inode);
	mountp->m_parent = directory->p_mount;
	irq = spin_lock_irqsave(&namespace_lock);

	link_child(directory->p_mount, mountp);
	link_global(mountp);

	spin_unlock_irqrestore(&namespace_lock, irq);

	/* Reports the reserved mount point. */
	return 0;
}

/* Compares canonical inode identities within one filesystem. */
static int
same_inode(
	const struct inode *left,
	const struct inode *right)
{
	return left != NULL && right != NULL && (left == right ||
	    (left->i_mount == right->i_mount && left->i_ino == right->i_ino));
}

/* Removes a mount from the global attachment list. */
static void
unlink_global(
	struct mount *mountp)
{
	struct mount **link;

	for (link = &mount_head; *link != NULL; link = &(*link)->m_next)
		if (*link == mountp) {
			*link = mountp->m_next;
			return;
		}
}

/* Joins optional readers before reversible writeback and filesystem teardown. */
static int
mount_io_quiesce(
	struct mount *mountp,
	struct mount_io_boundary *boundary)
{
	return mount_io_quiesce_mode(mountp, boundary, 0);
}

static int
mount_io_quiesce_mode(
	struct mount *mountp,
	struct mount_io_boundary *boundary,
	int revoked)
{
	int error;

	/* Requires both halves of each optional lifecycle provider before changing state. */
	if (readahead_boundary_begin != NULL && readahead_boundary_end == NULL)
		return EOPNOTSUPP;
	if (writeback_unmount_begin != NULL && writeback_unmount_finish == NULL)
		return EOPNOTSUPP;
	if (readahead_boundary_begin != NULL) {
		error = readahead_boundary_begin(&boundary->readahead, mountp);
		if (error != 0)
			return error;
	}

	/* Keeps the read gate closed while writeback pauses and drains the same mount. */
	if (writeback_unmount_begin != NULL) {
		if (revoked)
			error = writeback_unmount_begin_revoked(mountp, &boundary->writeback);
		else
			error = writeback_unmount_begin(mountp, &boundary->writeback);
		if (error != 0) {
			if (readahead_boundary_end != NULL)
				readahead_boundary_end(&boundary->readahead);
			return error;
		}
	}

	return 0;
}

/* Restores an aborted mount boundary or releases a successfully quiesced identity. */
static void
mount_io_finish(
	struct mount_io_boundary *boundary,
	int committed)
{
	/* Restores writeback before admitting fresh optional readers after rollback. */
	if (writeback_unmount_finish != NULL)
		writeback_unmount_finish(&boundary->writeback, committed);
	if (readahead_boundary_end != NULL)
		readahead_boundary_end(&boundary->readahead);
}

/* Reserves the admitted target before filesystem preparation can yield. */
static int
mount_at_target(
	const char *type_name,
	const struct path *directory,
	const char *name,
	int flags,
	void *data,
	struct mount **result,
	const struct inode *expected)
{
	struct mount *mountp;
	unsigned long irq;
	int error;
	int entered;

	/* Rejects a missing type, a non-directory, or an unusable name. */
	if (type_name == NULL ||
	    directory == NULL ||
	    directory->p_mount == NULL ||
	    directory->p_inode == NULL ||
	    directory->p_inode->i_type != INODE_DIR ||
	    !valid_component(name))
		return EINVAL;

	/* Allocates a slot before reserving its name under the transaction. */
	mountp = mount_alloc();
	if (mountp == NULL)
		return ENOSPC;
	entered = mount_vfs_transaction_join(mountp);
	error = reserve_mount(mountp, directory, name, expected);
	if (entered)
		mount_vfs_transaction_leave(mountp);
	if (error != 0)
		goto fail;
	error = mount_filesystem(mountp, type_name, flags, data);
	entered = mount_vfs_transaction_join(mountp);
	if (error != 0) {
		detach_mount(mountp);
		if (entered)
			mount_vfs_transaction_leave(mountp);
		goto fail;
	}

	irq = spin_lock_irqsave(&namespace_lock);

	mountp->m_state = MOUNT_STATE_LIVE;

	spin_unlock_irqrestore(&namespace_lock, irq);

	inode_dir_changed(directory->p_inode);
	if (entered)
		mount_vfs_transaction_leave(mountp);
	backing_mutation_end(&mountp->m_backing_guard);
	if (result != NULL)
		*result = mountp;

	/* Reports the mounted filesystem. */
	return 0;
fail:
	mount_free(mountp);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Consumes one held reference; only precommit refusals can restore admission. */
static int
unmount_revoked_owned(
	struct mount *mountp)
{
	struct mount_io_boundary boundary = { 0 };
	unsigned long irq;
	unsigned expected;
	unsigned vm_refs;
	unsigned dirty_inodes;
	uint64_t dirty_bytes;
	int error;
	int entered;
	int reserved;

	reserved = 0;
	error = EOPNOTSUPP;
	if (mountp->m_disk == NULL || mountp->m_type == NULL ||
	    mountp->m_type->prepare_unmount_revoked == NULL ||
	    mountp->m_type->commit_unmount_revoked == NULL)
		goto out;
	if (vm_object_discard_mount_refs == NULL || vm_object_discard_mount == NULL ||
	    writeback_unmount_begin == NULL || writeback_unmount_begin_revoked == NULL ||
	    writeback_unmount_finish == NULL || readahead_boundary_begin == NULL ||
	    readahead_boundary_end == NULL)
		goto out;
	error = EINVAL;
	if (disk_media_status(mountp->m_disk) == 0)
		goto out;
	error = EBUSY;
	if (mountp == root_mount || mount_is_private(mountp) ||
	    (mountp->m_internal_flags & MOUNT_BIND_INTERNAL) != 0)
		goto out;

	/* Join optional reads and writes without trying to sync the lost medium. */
	error = mount_io_quiesce_mode(mountp, &boundary, 1);
	if (error != 0)
		goto out;

	entered = mount_vfs_transaction_join(mountp);
	irq = spin_lock_irqsave(&namespace_lock);
	error = EBUSY;
	if (mountp->m_state == MOUNT_STATE_LIVE && mountp->m_children == NULL) {
		mountp->m_state = MOUNT_STATE_DYING;
		reserved = 1;
		error = 0;
	}
	spin_unlock_irqrestore(&namespace_lock, irq);
	if (entered)
		mount_vfs_transaction_leave(mountp);
	if (error != 0)
		goto out;

	/* Drop reconstructible lookup cache owners before counting external users. */
	namecache_purge_mount(mountp);
	error = vm_object_discard_mount_refs(mountp, NULL, &vm_refs);
	if (error != 0)
		goto out;
	expected = 2U;
	if (boundary.writeback.mount != NULL)
		expected++;
	if (vm_refs > UINT_MAX - expected) {
		error = EBUSY;
		goto out;
	}
	expected += vm_refs;
	irq = spin_lock_irqsave(&namespace_lock);
	error = refcount_load(&mountp->m_refs) == expected ? 0 : EBUSY;
	spin_unlock_irqrestore(&namespace_lock, irq);
	if (error != 0)
		goto out;
	error = inode_cache_mount_revoked_check(mountp);
	if (error != 0)
		goto out;
	error = mountp->m_type->prepare_unmount_revoked(mountp);
	if (error != 0)
		goto out;

	/* Every refusal is behind us. No write, sync or clean-superblock claim. */
	mountp->m_type->commit_unmount_revoked(mountp);
	dirty_inodes = inode_cache_discard_mount_dirty(mountp);
	error = vm_object_discard_mount(mountp, &dirty_bytes);
	if (error != 0)
		HAL_FATAL("revoked unmount lost exclusive VM ownership");
	kern_logf("unmount: %s: revoked medium discarded; VM dirty bytes=%llu, dirty inodes=%u\n",
	    mountp->m_path, (unsigned long long)dirty_bytes, dirty_inodes);

	/* The disk retirement owner later disposes shared physical buffers. */
	mount_io_finish(&boundary, 1);
	finalize_filesystem_destroy(mountp);
	entered = mount_vfs_transaction_join(mountp);
	inode_dir_changed(mountp->m_cover.p_inode);
	detach_mount(mountp);
	if (entered)
		mount_vfs_transaction_leave(mountp);
	mount_release(mountp);
	mount_free(mountp);
	return 0;

out:
	if (reserved) {
		entered = mount_vfs_transaction_join(mountp);
		irq = spin_lock_irqsave(&namespace_lock);
		mountp->m_state = MOUNT_STATE_LIVE;
		spin_unlock_irqrestore(&namespace_lock, irq);
		if (entered)
			mount_vfs_transaction_leave(mountp);
	}
	mount_io_finish(&boundary, 0);
	mount_release(mountp);
	return error;
}

/* Consumes one held mount reference on every teardown outcome. */
static int
unmount_owned(
	struct mount *mountp)
{
	struct mount_io_boundary boundary = { 0 };
	unsigned expected_refs;
	unsigned long irq;
	int error;
	int entered;

	/* Joins optional reads before draining VM ownership or counting mount references. */
	error = mount_io_quiesce(mountp, &boundary);
	if (error != 0) {
		mount_release(mountp);
		return error;
	}

	expected_refs = 2;
	if (boundary.writeback.mount != NULL)
		expected_refs++;

	/* Preserves failed dirty owners instead of misreporting their handles as busy. */
	if (vm_object_sync_mount_buffer != NULL) {
		error = vm_object_sync_mount_buffer(mountp, NULL, 0);
		if (error != 0) {
			mount_io_finish(&boundary, 0);
			mount_release(mountp);
			return error;
		}
	}

	/* Drops clean cache paths before the namespace reference check. */
	if (vm_object_cache_drain != NULL)
		(void)vm_object_cache_drain(mountp);

	/* The root, a dying mount, or a busy one cannot be unmounted. */
	entered = mount_vfs_transaction_join(mountp);
	irq = spin_lock_irqsave(&namespace_lock);

	if (mountp == root_mount || mountp->m_state != MOUNT_STATE_LIVE) {
		spin_unlock_irqrestore(&namespace_lock, irq);
		if (entered)
			mount_vfs_transaction_leave(mountp);
		mount_io_finish(&boundary, 0);
		mount_release(mountp);
		return EBUSY;
	}

	if (mountp->m_children != NULL ||
	    refcount_load(&mountp->m_refs) != expected_refs) {
		spin_unlock_irqrestore(&namespace_lock, irq);
		if (entered)
			mount_vfs_transaction_leave(mountp);
		mount_io_finish(&boundary, 0);
		mount_release(mountp);
		return EBUSY;
	}

	/* Reserves the attachment while its filesystem completes teardown. */
	mountp->m_state = MOUNT_STATE_DYING;

	spin_unlock_irqrestore(&namespace_lock, irq);

	if (entered)
		mount_vfs_transaction_leave(mountp);
	/*
	 * Keep the DYING attachment reserved through every failure-capable step.
	 * Readers cannot acquire new references or fall through to covered data.
	 * Sync and teardown may call back through an overlay into the VFS.
	 */
	if ((mountp->m_internal_flags & MOUNT_BIND_INTERNAL) == 0) {
		error = prepare_filesystem_destroy(mountp, expected_refs);
		if (error != 0) {
			entered = mount_vfs_transaction_join(mountp);
			irq = spin_lock_irqsave(&namespace_lock);
			mountp->m_state = MOUNT_STATE_LIVE;
			spin_unlock_irqrestore(&namespace_lock, irq);
			if (entered)
				mount_vfs_transaction_leave(mountp);
			mount_io_finish(&boundary, 0);
			mount_release(mountp);
			return error;
		}

		mount_io_finish(&boundary, 1);
		finalize_filesystem_destroy(mountp);
	}

	mount_io_finish(&boundary, 1);
	entered = mount_vfs_transaction_join(mountp);
	inode_dir_changed(mountp->m_cover.p_inode);
	detach_mount(mountp);
	if ((mountp->m_internal_flags & MOUNT_BIND_INTERNAL) != 0 &&
	    mountp->m_root != NULL)
		inode_release(mountp->m_root);
	if (mountp->m_bind_source != NULL)
		mount_release(mountp->m_bind_source);
	if (entered)
		mount_vfs_transaction_leave(mountp);
	mount_release(mountp);
	mount_free(mountp);

	/* Reports the unmounted filesystem. */
	return 0;
}
