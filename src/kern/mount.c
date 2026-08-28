/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/mount.h"
#include "kern/backing-claim.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/namecache.h"
#include "kern/namei.h"
#include "kern/rootfs.h"

#include <errno.h>
#include <string.h>
#include <sys/statvfs.h>
#include <zedbsd/blkid.h>
#include <zedbsd/quota.h>
#include <zedbsd/snapshot.h>

#define FILESYSTEM_MAX 8U
#define MOUNT_BIND_INTERNAL 0x00000001U
#define MOUNT_HIGH __attribute__((section(".hightext")))

static struct mount mounts[MOUNT_MAX] __attribute__((section(".vfs_bss")));
static uint8_t mount_used[MOUNT_MAX] __attribute__((section(".vfs_bss")));
static struct mount *mount_head;
static struct mount *root_mount;
static const struct filesystem_type *filesystems[FILESYSTEM_MAX]
	__attribute__((section(".vfs_bss")));
static unsigned filesystem_count;
static struct spinlock namespace_lock;

static struct mount *
mount_alloc(void)
{
	struct mount *result = NULL;
	unsigned long irq = spin_lock_irqsave(&namespace_lock);
	unsigned i;
	for (i = 0; i < MOUNT_MAX; i++) {
		if (!mount_used[i]) {
			mount_used[i] = 1;
			memset(&mounts[i], 0, sizeof(mounts[i]));
			refcount_init(&mounts[i].m_refs, 1);
			(void)mutex_init(&mounts[i].m_lock, LOCK_RANK_NAMESPACE,
			    "mount");
			(void)mutex_init(&mounts[i].m_vfs_transaction_storage,
			    LOCK_RANK_VFS_TRANSACTION, "VFS namespace transaction");
			mounts[i].m_vfs_transaction_lock =
			    &mounts[i].m_vfs_transaction_storage;
			waitq_init(&mounts[i].m_waitq, "mount state");
			mounts[i].m_state = MOUNT_STATE_PREPARING;
			result = &mounts[i];
			break;
		}
	}
	spin_unlock_irqrestore(&namespace_lock, irq);
	return result;
}
void
mount_vfs_transaction_enter(struct mount *mountp)
{
	if (mountp != NULL)
		mutex_lock(mountp->m_vfs_transaction_lock);
}

void
mount_vfs_transaction_leave(struct mount *mountp)
{
	if (mountp != NULL)
		mutex_unlock(mountp->m_vfs_transaction_lock);
}

static void
mount_free(struct mount *mountp)
{
	unsigned long irq;
	unsigned i;
	if (mountp == NULL || refcount_load(&mountp->m_refs) != 1)
		return;
	backing_mutation_end(&mountp->m_backing_guard);
	irq = spin_lock_irqsave(&namespace_lock);
	if (refcount_load(&mountp->m_refs) != 1) {
		spin_unlock_irqrestore(&namespace_lock, irq);
		return;
	}
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

void path_init(struct path *path)
{
	if (path != NULL)
		memset(path, 0, sizeof(*path));
}

void path_set(struct path *path, struct mount *mountp, struct inode *inode)
{
	if (path == NULL)
		return;
	path->p_mount = mountp;
	path->p_inode = inode;
	if (mountp != NULL)
		mount_ref(mountp);
	if (inode != NULL)
		inode_ref(inode);
}

void path_ref(struct path *path)
{
	if (path != NULL)
		path_set(path, path->p_mount, path->p_inode);
}

void path_release(struct path *path)
{
	if (path == NULL)
		return;
	if (path->p_inode != NULL)
		inode_release(path->p_inode);
	if (path->p_mount != NULL)
		mount_release(path->p_mount);
	path_init(path);
}

int path_equal(const struct path *left, const struct path *right)
{
	return left != NULL && right != NULL &&
		left->p_mount == right->p_mount && left->p_inode == right->p_inode;
}

int
filesystem_register(const struct filesystem_type *type)
{
	unsigned long irq;
	unsigned i;
	if (type == NULL || type->fs_name == NULL || type->mount == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&namespace_lock);
	for (i = 0; i < filesystem_count; i++)
		if (!strcmp(filesystems[i]->fs_name, type->fs_name)) {
			spin_unlock_irqrestore(&namespace_lock, irq);
			return EEXIST;
		}
	if (filesystem_count >= FILESYSTEM_MAX) {
		spin_unlock_irqrestore(&namespace_lock, irq);
		return ENOSPC;
	}
	filesystems[filesystem_count++] = type;
	spin_unlock_irqrestore(&namespace_lock, irq);
	return 0;
}

static int
identity_text_zero(const char *text, size_t capacity)
{
	size_t index;

	for (index = 0; index < capacity; index++)
		if (text[index] != '\0')
			return 0;
	return 1;
}

static int
identity_text_valid(
	const char *text,
	size_t capacity,
	uint32_t flags,
	uint32_t field_flag)
{
	if ((flags & field_flag) == 0)
		return identity_text_zero(text, capacity);
	return text[0] != '\0' && memchr(text, '\0', capacity) != NULL;
}

static int
filesystem_identity_valid(const struct block_identity *identity)
{
	const uint32_t allowed = ZEDBSD_BLKID_TYPE | ZEDBSD_BLKID_UUID |
	    ZEDBSD_BLKID_LABEL;

	if ((identity->flags & ~allowed) != 0 || identity->reserved != 0 ||
	    !identity_text_zero(identity->partuuid,
	    sizeof(identity->partuuid)) ||
	    !identity_text_zero(identity->partlabel,
	    sizeof(identity->partlabel)) ||
	    !identity_text_valid(identity->type, sizeof(identity->type),
	    identity->flags, ZEDBSD_BLKID_TYPE) ||
	    !identity_text_valid(identity->uuid, sizeof(identity->uuid),
	    identity->flags, ZEDBSD_BLKID_UUID) ||
	    !identity_text_valid(identity->label, sizeof(identity->label),
	    identity->flags, ZEDBSD_BLKID_LABEL))
		return 0;
	return 1;
}

int
filesystem_identify(struct disk *disk, struct block_identity *identity)
{
	const struct filesystem_type *snapshot[FILESYSTEM_MAX];
	struct block_identity candidate, match;
	unsigned snapshot_count = 0;
	unsigned match_count = 0;
	unsigned long irq;
	unsigned index;
	int saved_error = EOPNOTSUPP;

	if (identity == NULL)
		return EINVAL;
	memset(identity, 0, sizeof(*identity));
	if (disk == NULL)
		return EINVAL;

	/* Identity callbacks perform I/O, so only snapshot the registry locked. */
	irq = spin_lock_irqsave(&namespace_lock);
	for (index = 0; index < filesystem_count; index++) {
		const struct filesystem_type *type = filesystems[index];

		if ((type->fs_flags & FILESYSTEM_NODEV) != 0 ||
		    type->identify == NULL)
			continue;
		snapshot[snapshot_count++] = type;
	}
	spin_unlock_irqrestore(&namespace_lock, irq);

	for (index = 0; index < snapshot_count; index++) {
		int error;

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
		if (match_count++ != 0)
			return EEXIST;
		match = candidate;
	}

	if (match_count == 0)
		return saved_error;
	*identity = match;
	return 0;
}

void
mount_reset(void)
{
	namecache_reset();
	inode_cache_reset();
	rootfs_reset();
	spin_init(&namespace_lock, LOCK_RANK_NAMESPACE, "mount namespace");
	memset(mounts, 0, sizeof(mounts));
	memset(mount_used, 0, sizeof(mount_used));
	memset(filesystems, 0, sizeof(filesystems));
	mount_head = NULL;
	root_mount = NULL;
	filesystem_count = 0;
}

static const struct filesystem_type *
find_type(const char *name, struct disk *disk, int *probe_error)
{
	unsigned i;
	*probe_error = EOPNOTSUPP;
	for (i = 0; i < filesystem_count; i++) {
		const struct filesystem_type *type = filesystems[i];
		int error;
		if (strcmp(name, "auto") && strcmp(name, type->fs_name))
			continue;
		if ((type->fs_flags & FILESYSTEM_NODEV) != 0) {
			if (!strcmp(name, type->fs_name))
				return type;
			continue;
		}
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
	return NULL;
}

static int
mount_filesystem_on_disk(struct mount *mountp, const char *type_name,
			 struct disk *disk, int flags, void *data)
{
	const struct filesystem_type *type;
	int error;

	type = find_type(type_name, disk, &error);
	if (type == NULL)
		return disk == NULL && !strcmp(type_name, "auto") ? ENXIO : error;
	if (!(type->fs_flags & FILESYSTEM_NODEV) && disk == NULL)
		return ENXIO;
	if (disk != NULL) {
		if ((flags & MOUNT_READ_ONLY) == 0 &&
		    (disk->d_flags & DISK_READ_ONLY) == 0) {
			error = backing_mutation_begin_disk(
			    disk, 0, disk->d_block_count, NULL,
			    &mountp->m_backing_guard);
			if (error != 0)
				return error;
		}
		error = backing_claim_check_mount(disk, (unsigned)flags |
		    ((disk->d_flags & DISK_READ_ONLY) != 0 ? MOUNT_READ_ONLY : 0));
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
		return error != 0 ? error : EIO;
	}
	return 0;
}

int
mount_disk_writable_busy(struct disk *disk)
{
	struct disk *leaf, *last_leaf, *candidate_leaf, *candidate_last_leaf;
	uint64_t first, last, candidate_first, candidate_last;
	struct mount *mountp;
	unsigned index;
	unsigned long irq;
	int busy = 0;

	if (disk == NULL || disk->d_block_count == 0 ||
	    disk_resolve_range(disk, 0, 1, &leaf, &first) != 0 ||
	    disk_resolve_range(disk, disk->d_block_count - 1U, 1, &last_leaf,
	    &last) != 0 || leaf != last_leaf)
		return EINVAL;
	irq = spin_lock_irqsave(&namespace_lock);
	for (index = 0; index < MOUNT_MAX; index++) {
		if (!mount_used[index])
			continue;
		mountp = &mounts[index];
		if (mountp->m_state != MOUNT_STATE_LIVE || mountp->m_disk == NULL ||
		    (mountp->m_flags & MOUNT_READ_ONLY) != 0 ||
		    mountp->m_disk->d_block_count == 0)
			continue;
		if (disk_resolve_range(mountp->m_disk, 0, 1, &candidate_leaf,
		    &candidate_first) != 0 ||
		    disk_resolve_range(mountp->m_disk,
		    mountp->m_disk->d_block_count - 1U, 1,
		    &candidate_last_leaf, &candidate_last) != 0 ||
		    candidate_leaf != candidate_last_leaf)
			continue;
		if (candidate_leaf == leaf && candidate_first <= last &&
		    first <= candidate_last) {
			busy = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&namespace_lock, irq);
	return busy ? EBUSY : 0;
}

static int
mount_filesystem(struct mount *mountp, const char *type_name, int flags,
		 void *data)
{
	const struct fat_mount_args *args = data;
	struct disk *disk = NULL;
	unsigned i;
	/* A nodev filesystem owns the interpretation of its mount data. */
	for (i = 0; i < filesystem_count; i++)
		if (!strcmp(type_name, filesystems[i]->fs_name) &&
		    (filesystems[i]->fs_flags & FILESYSTEM_NODEV) != 0)
			return mount_filesystem_on_disk(mountp, type_name, NULL,
				flags, data);
	if (args != NULL && args->fspec != NULL)
		disk = disk_find(args->fspec);
	{
		int error = mount_filesystem_on_disk(mountp, type_name, disk,
		    flags, data);
		disk_release(disk);
		return error;
	}
}

void
mount_ref(struct mount *mountp)
{
	if (mountp != NULL)
		refcount_get(&mountp->m_refs);
}

void
mount_release(struct mount *mountp)
{
	if (mountp != NULL)
		(void)refcount_put_not_last(&mountp->m_refs);
}

int
mount_root_create(const char *type_name, int flags, void *data,
		  struct mount **result)
{
	struct mount *mountp;
	unsigned long irq;
	int error;
	if (type_name == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&namespace_lock);
	error = root_mount != NULL ? EBUSY : 0;
	spin_unlock_irqrestore(&namespace_lock, irq);
	if (error != 0)
		return error;
	mountp = mount_alloc();
	if (mountp == NULL)
		return ENOSPC;
	strcpy(mountp->m_path, "/");
	error = mount_filesystem(mountp, type_name, flags, data);
	if (error != 0) {
		mount_free(mountp);
		return error;
	}
	irq = spin_lock_irqsave(&namespace_lock);
	mount_head = root_mount = mountp;
	mountp->m_state = MOUNT_STATE_LIVE;
	spin_unlock_irqrestore(&namespace_lock, irq);
	backing_mutation_end(&mountp->m_backing_guard);
	if (result != NULL)
		*result = mountp;
	return 0;
}

int
mount_rootfs(void)
{
	struct mount *mountp;
	unsigned long irq;
	int error;
	irq = spin_lock_irqsave(&namespace_lock);
	error = root_mount != NULL ? EBUSY : 0;
	spin_unlock_irqrestore(&namespace_lock, irq);
	if (error != 0)
		return EBUSY;
	mountp = mount_alloc();
	if (mountp == NULL)
		return ENOSPC;
	strcpy(mountp->m_path, "/");
	mountp->m_flags = MOUNT_READ_ONLY;
	mountp->m_type = &rootfs_type;
	error = rootfs_type.mount(mountp);
	if (error != 0) {
		mount_free(mountp);
		return error;
	}
	irq = spin_lock_irqsave(&namespace_lock);
	mount_head = root_mount = mountp;
	mountp->m_state = MOUNT_STATE_LIVE;
	spin_unlock_irqrestore(&namespace_lock, irq);
	return 0;
}

struct mount *mount_root_get_ref(void)
{
	unsigned long irq = spin_lock_irqsave(&namespace_lock);
	struct mount *mountp = root_mount;
	if (mountp != NULL && mountp->m_state == MOUNT_STATE_LIVE)
		mount_ref(mountp);
	else
		mountp = NULL;
	spin_unlock_irqrestore(&namespace_lock, irq);
	return mountp;
}
struct inode *mount_root_inode(void)
{
	unsigned long irq = spin_lock_irqsave(&namespace_lock);
	struct inode *inode = root_mount != NULL ? root_mount->m_root : NULL;
	spin_unlock_irqrestore(&namespace_lock, irq);
	return inode;
}

static int
valid_component(const char *name)
{
	size_t length;
	if (name == NULL || name[0] == '\0' || !strcmp(name, ".") ||
	    !strcmp(name, "..") || strchr(name, '/') != NULL)
		return 0;
	length = strlen(name);
	return length <= NAME_MAX;
}

static void
link_child(struct mount *parent, struct mount *child)
{
	struct mount **link = &parent->m_children;
	while (*link != NULL)
		link = &(*link)->m_sibling;
	*link = child;
}

static void
link_global(struct mount *mountp)
{
	struct mount **link = &mount_head;
	while (*link != NULL)
		link = &(*link)->m_next;
	*link = mountp;
}

static int
set_mount_path(struct mount *mountp, const struct path *directory,
	       const char *name)
{
	const char *base = directory->p_mount->m_path;
	size_t base_length = strlen(base), name_length = strlen(name);
	if (base_length + (base_length > 1U ? 1U : 0U) + name_length >=
	    sizeof(mountp->m_path))
		return ENAMETOOLONG;
	strcpy(mountp->m_path, base);
	if (base_length > 1U)
		strcat(mountp->m_path, "/");
	strcat(mountp->m_path, name);
	strcpy(mountp->m_name, name);
	return 0;
}

int
mount_at(const char *type_name, const struct path *directory,
	 const char *name, int flags, void *data, struct mount **result)
{
	struct mount *mountp;
	unsigned long irq;
	int error;
	if (type_name == NULL || directory == NULL || directory->p_mount == NULL ||
	    directory->p_inode == NULL || directory->p_inode->i_type != INODE_DIR ||
	    !valid_component(name))
		return EINVAL;
	{
		struct componentname component = { name, strlen(name), 0 };
		struct path existing;
		if (mount_lookup_child(directory, &component, &existing) == 0) {
			path_release(&existing);
			return EBUSY;
		}
	}
	mountp = mount_alloc();
	if (mountp == NULL)
		return ENOSPC;
	error = set_mount_path(mountp, directory, name);
	if (error != 0)
		goto fail;
	path_set(&mountp->m_cover, directory->p_mount, directory->p_inode);
	mountp->m_parent = directory->p_mount;
	error = mount_filesystem(mountp, type_name, flags, data);
	if (error != 0)
		goto fail_cover;
	irq = spin_lock_irqsave(&namespace_lock);
	link_child(directory->p_mount, mountp);
	link_global(mountp);
	mountp->m_state = MOUNT_STATE_LIVE;
	spin_unlock_irqrestore(&namespace_lock, irq);
	backing_mutation_end(&mountp->m_backing_guard);
	if (result != NULL)
		*result = mountp;
	return 0;
fail_cover:
	path_release(&mountp->m_cover);
fail:
	mount_free(mountp);
	return error;
}

int
mount_bind_at(const struct path *source, const struct path *directory,
	      const char *name, struct mount **result)
{
	struct mount *mountp;
	unsigned long irq;
	int error;
	if (source == NULL || source->p_mount == NULL || source->p_inode == NULL ||
	    directory == NULL || directory->p_mount == NULL ||
	    directory->p_inode == NULL || directory->p_inode->i_type != INODE_DIR ||
	    !valid_component(name))
		return EINVAL;
	mountp = mount_alloc();
	if (mountp == NULL)
		return ENOSPC;
	error = set_mount_path(mountp, directory, name);
	if (error != 0)
		goto fail;
	path_set(&mountp->m_cover, directory->p_mount, directory->p_inode);
	mountp->m_parent = directory->p_mount;
	mountp->m_bind_source = source->p_mount;
	mount_ref(mountp->m_bind_source);
	mountp->m_vfs_transaction_lock =
	    source->p_mount->m_vfs_transaction_lock;
	mountp->m_internal_flags = MOUNT_BIND_INTERNAL;
	mountp->m_flags = source->p_mount->m_flags;
	mountp->m_type = source->p_inode->i_mount->m_type;
	mountp->m_root = source->p_inode;
	inode_ref(mountp->m_root);
	irq = spin_lock_irqsave(&namespace_lock);
	link_child(directory->p_mount, mountp);
	link_global(mountp);
	mountp->m_state = MOUNT_STATE_LIVE;
	spin_unlock_irqrestore(&namespace_lock, irq);
	if (result != NULL)
		*result = mountp;
	return 0;
fail:
	mount_free(mountp);
	return error;
}

MOUNT_HIGH int
mount_private(const char *type_name, struct disk *disk, int flags, void *data,
	      struct mount **result)
{
	struct mount *mountp;
	unsigned long irq;
	int error;
	if (type_name == NULL || disk == NULL || result == NULL)
		return EINVAL;
	*result = NULL;
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
	return 0;
}

MOUNT_HIGH int
mount_private_promote_root(struct mount *mountp, struct mount **result)
{
	unsigned long irq;
	int error = 0;

	if (!mount_is_private(mountp))
		return EINVAL;
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
		mount_head = root_mount = mountp;
	}
	spin_unlock_irqrestore(&namespace_lock, irq);
	if (error == 0 && result != NULL)
		*result = mountp;
	return error;
}

static MOUNT_HIGH int
valid_private_path(const char *path)
{
	const char *component = path;
	const char *cursor;
	if (path == NULL || path[0] == '\0' || path[0] == '/')
		return 0;
	for (cursor = path;; cursor++) {
		if (*cursor != '/' && *cursor != '\0')
			continue;
		if (cursor == component ||
		    (cursor - component == 1 && component[0] == '.') ||
		    (cursor - component == 2 && component[0] == '.' &&
		     component[1] == '.'))
			return 0;
		if (*cursor == '\0')
			return 1;
		component = cursor + 1;
	}
}

MOUNT_HIGH int
mount_private_lookup(struct mount *mountp, const char *relative,
		     struct path *result)
{
	struct cwdinfo context;
	struct path root;
	int error;
	if (!mount_is_private(mountp) || result == NULL ||
	    !valid_private_path(relative))
		return EINVAL;
	path_init(&root);
	path_set(&root, mountp, mountp->m_root);
	error = cwdinfo_init(&context, &root);
	path_release(&root);
	if (error != 0)
		return error;
	error = namei_path_at(&context, relative, result);
	cwdinfo_destroy(&context);
	return error;
}

MOUNT_HIGH int
mount_is_private(const struct mount *mountp)
{
	return mountp != NULL &&
		(mountp->m_internal_flags & MOUNT_PRIVATE_INTERNAL) != 0;
}

int
mount_sync(struct mount *mountp)
{
	if (mountp == NULL || mountp->m_type == NULL)
		return EINVAL;
	return mountp->m_type->sync != NULL ? mountp->m_type->sync(mountp) : 0;
}

int
mount_sync_all(void)
{
	struct mount *snapshot[MOUNT_MAX];
	unsigned count = 0, index;
	unsigned long irq = spin_lock_irqsave(&namespace_lock);
	struct mount *mountp;
	int first_error = 0;

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
	for (index = 0; index < count; index++) {
		int error = mount_sync(snapshot[index]);
		if (first_error == 0 && error != 0)
			first_error = error;
		mount_release(snapshot[index]);
	}
	return first_error;
}

int
mount_statvfs(struct mount *mountp, struct statvfs *result)
{
	int error = 0;
	if (mountp == NULL || result == NULL || mountp->m_type == NULL)
		return EINVAL;
	if (mountp->m_bind_source != NULL) {
		error = mount_statvfs(mountp->m_bind_source, result);
		if (error != 0)
			return error;
		goto flags;
	}
	memset(result, 0, sizeof(*result));
	if (mountp->m_type->statvfs != NULL)
		error = mountp->m_type->statvfs(mountp, result);
	else {
		result->f_bsize = mountp->m_disk != NULL ?
		    mountp->m_disk->d_block_size : 1U;
		result->f_frsize = result->f_bsize;
		result->f_blocks = mountp->m_disk != NULL ?
		    mountp->m_disk->d_block_count : 0;
		result->f_namemax = NAME_MAX;
	}
	if (error != 0)
		return error;
	result->f_fsid = mountp->m_disk != NULL ? mountp->m_disk->d_dev :
	    (uint64_t)(1U + (unsigned)(mountp - mounts));
flags:
	result->f_flag &= ~((uint64_t)ST_RDONLY | (uint64_t)ST_NOSUID);
	result->f_flag |= ((mountp->m_flags & MOUNT_READ_ONLY) != 0 ||
	    (mountp->m_disk != NULL &&
	    (mountp->m_disk->d_flags & DISK_READ_ONLY) != 0)) ? ST_RDONLY : 0U;
	result->f_flag |= (mountp->m_flags & MOUNT_NOSUID) != 0 ? ST_NOSUID : 0U;
	if (result->f_namemax == 0)
		result->f_namemax = NAME_MAX;
	return 0;
}

int
mount_quotactl(struct mount *mountp, struct quota_control *request)
{
	if (mountp == NULL || request == NULL)
		return EINVAL;
	if ((mountp->m_internal_flags & MOUNT_BIND_INTERNAL) != 0 &&
	    mountp->m_bind_source != NULL)
		return mount_quotactl(mountp->m_bind_source, request);
	if (mountp->m_type == NULL || mountp->m_type->quotactl == NULL)
		return EOPNOTSUPP;
	return mountp->m_type->quotactl(mountp, request);
}

int
mount_snapshotctl(struct mount *mountp, struct snapshot_control *request)
{
	if (mountp == NULL || request == NULL)
		return EINVAL;
	if ((mountp->m_internal_flags & MOUNT_BIND_INTERNAL) != 0 &&
	    mountp->m_bind_source != NULL)
		return mount_snapshotctl(mountp->m_bind_source, request);
	if (mountp->m_type == NULL || mountp->m_type->snapshotctl == NULL)
		return EOPNOTSUPP;
	return mountp->m_type->snapshotctl(mountp, request);
}

int
mount(const char *type_name, const char *dir, int flags, void *data)
{
	struct path root;
	struct mount *rootp = mount_root_get_ref();
	if (rootp == NULL || dir == NULL || dir[0] != '/' || dir[1] == '\0' ||
	    strchr(dir + 1, '/') != NULL) {
		mount_release(rootp);
		return EINVAL;
	}
	path_set(&root, rootp, rootp->m_root);
	mount_release(rootp);
	{
		int error = mount_at(type_name, &root, dir + 1, flags, data, NULL);
		path_release(&root);
		return error;
	}
}

struct mount *
mount_find_ref(const char *path)
{
	struct mount *mountp;
	unsigned long irq;
	if (path == NULL)
		return NULL;
	irq = spin_lock_irqsave(&namespace_lock);
	for (mountp = mount_head; mountp != NULL; mountp = mountp->m_next)
		if (mountp->m_state == MOUNT_STATE_LIVE &&
		    !strcmp(mountp->m_path, path)) {
			mount_ref(mountp);
			spin_unlock_irqrestore(&namespace_lock, irq);
			return mountp;
		}
	spin_unlock_irqrestore(&namespace_lock, irq);
	return NULL;
}

struct mount *mount_for_inode(const struct inode *inode)
{
	return inode != NULL ? inode->i_mount : NULL;
}

int
mount_lookup_child(const struct path *directory,
		   const struct componentname *component, struct path *result)
{
	struct mount *child;
	unsigned long irq;
	if (directory == NULL || component == NULL || result == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&namespace_lock);
	for (child = directory->p_mount->m_children; child != NULL;
	     child = child->m_sibling) {
		size_t length = strlen(child->m_name);
		if (child->m_cover.p_mount == directory->p_mount &&
		    child->m_cover.p_inode == directory->p_inode &&
		    length == component->cn_namelen &&
		    !memcmp(child->m_name, component->cn_nameptr, length)) {
			path_set(result, child, child->m_root);
			spin_unlock_irqrestore(&namespace_lock, irq);
			return 0;
		}
	}
	spin_unlock_irqrestore(&namespace_lock, irq);
	return ENOENT;
}

int
mount_cross_path_parent(const struct path *current, struct path *result)
{
	unsigned long irq;
	if (current == NULL || current->p_mount == NULL || result == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&namespace_lock);
	if (current->p_inode != current->p_mount->m_root ||
	    current->p_mount == root_mount ||
	    current->p_mount->m_cover.p_inode == NULL) {
		spin_unlock_irqrestore(&namespace_lock, irq);
		return ENOENT;
	}
	path_set(result, current->p_mount->m_cover.p_mount,
		 current->p_mount->m_cover.p_inode);
	spin_unlock_irqrestore(&namespace_lock, irq);
	return 0;
}

int
mount_follow(struct inode *inode, struct inode **result)
{
	struct mount *mountp;
	unsigned long irq;
	if (inode == NULL || result == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&namespace_lock);
	for (mountp = mount_head; mountp != NULL; mountp = mountp->m_next) {
		if (mountp->m_state == MOUNT_STATE_LIVE &&
		    mountp->m_mountpoint == inode) {
			inode_ref(mountp->m_root);
			*result = mountp->m_root;
			spin_unlock_irqrestore(&namespace_lock, irq);
			return 0;
		}
	}
	spin_unlock_irqrestore(&namespace_lock, irq);
	return ENOENT;
}

int
mount_cross_parent(struct inode *inode, struct inode **result)
{
	struct mount *mountp;
	struct inode *point = NULL;
	struct componentname dotdot = { "..", 2, COMPONENT_DOTDOT };
	unsigned long irq;
	if (inode == NULL || result == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&namespace_lock);
	for (mountp = mount_head; mountp != NULL; mountp = mountp->m_next)
		if (mountp->m_state == MOUNT_STATE_LIVE &&
		    mountp->m_root == inode && mountp->m_mountpoint != NULL) {
			point = mountp->m_mountpoint;
			inode_ref(point);
			break;
		}
	spin_unlock_irqrestore(&namespace_lock, irq);
	if (point != NULL) {
		int error = inode_lookup(point, &dotdot, result);
		inode_release(point);
		return error;
	}
	return ENOENT;
}

int
mount_readdir_child(const struct path *directory, unsigned *cursor,
		    struct dirent *entry)
{
	struct mount *child;
	unsigned long irq;
	unsigned index = 0;
	if (directory == NULL || cursor == NULL || entry == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&namespace_lock);
	for (child = directory->p_mount->m_children; child != NULL;
	     child = child->m_sibling) {
		if (child->m_cover.p_mount != directory->p_mount ||
		    child->m_cover.p_inode != directory->p_inode)
			continue;
		if (index++ != *cursor)
			continue;
		memset(entry, 0, sizeof(*entry));
		entry->d_ino = child->m_root->i_ino;
		entry->d_type = INODE_DIR;
		strcpy(entry->d_name, child->m_name);
		(*cursor)++;
		spin_unlock_irqrestore(&namespace_lock, irq);
		return 0;
	}
	spin_unlock_irqrestore(&namespace_lock, irq);
	return ENOENT;
}

int
mount_child_shadows(const struct path *directory, const char *name)
{
	struct componentname component;
	struct path found;
	int error;
	component.cn_nameptr = name;
	component.cn_namelen = strlen(name);
	component.cn_flags = 0;
	error = mount_lookup_child(directory, &component, &found);
	if (error == 0)
		path_release(&found);
	return error == 0;
}

static void
unlink_child(struct mount *mountp)
{
	struct mount **link;
	if (mountp->m_parent == NULL)
		return;
	for (link = &mountp->m_parent->m_children; *link != NULL;
	     link = &(*link)->m_sibling)
		if (*link == mountp) {
			*link = mountp->m_sibling;
			return;
		}
}

static int
prepare_filesystem_destroy(struct mount *mountp, unsigned expected_refs)
{
	int error;
	if (mountp == NULL || mountp->m_children != NULL ||
	    refcount_load(&mountp->m_refs) != expected_refs)
		return EBUSY;
	namecache_purge_mount(mountp);
	error = inode_cache_mount_busy(mountp);
	if (error != 0)
		return error;
	error = mount_sync(mountp);
	if (error != 0)
		return error;
	return mountp->m_type != NULL &&
	    mountp->m_type->prepare_unmount != NULL ?
		mountp->m_type->prepare_unmount(mountp) : 0;
}

static void
finalize_filesystem_destroy(struct mount *mountp)
{
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

MOUNT_HIGH int
unmount_private(struct mount *mountp)
{
	int error;
	if (!mount_is_private(mountp))
		return EINVAL;
	error = prepare_filesystem_destroy(mountp, 1);
	if (error != 0)
		return error;
	mountp->m_state = MOUNT_STATE_DYING;
	finalize_filesystem_destroy(mountp);
	mount_free(mountp);
	return 0;
}

unsigned
mount_count(void)
{
	unsigned i, count = 0;
	unsigned long irq = spin_lock_irqsave(&namespace_lock);
	for (i = 0; i < MOUNT_MAX; i++)
		count += mount_used[i] != 0;
	spin_unlock_irqrestore(&namespace_lock, irq);
	return count;
}

int
unmount(const char *dir, int flags)
{
	struct mount *mountp = mount_find_ref(dir);
	struct mount **link;
	unsigned long irq;
	int error;
	(void)flags;
	if (mountp == NULL)
		return ENOENT;
	irq = spin_lock_irqsave(&namespace_lock);
	if (mountp == root_mount || mountp->m_state != MOUNT_STATE_LIVE) {
		spin_unlock_irqrestore(&namespace_lock, irq);
		mount_release(mountp);
		return EBUSY;
	}
	if (mountp->m_children != NULL ||
	    refcount_load(&mountp->m_refs) != 2) {
		spin_unlock_irqrestore(&namespace_lock, irq);
		mount_release(mountp);
		return EBUSY;
	}
	mountp->m_state = MOUNT_STATE_DYING;
	unlink_child(mountp);
	for (link = &mount_head; *link != NULL; link = &(*link)->m_next)
		if (*link == mountp) {
			*link = mountp->m_next;
			break;
		}
	spin_unlock_irqrestore(&namespace_lock, irq);
	if ((mountp->m_internal_flags & MOUNT_BIND_INTERNAL) == 0) {
		error = prepare_filesystem_destroy(mountp, 2);
		if (error != 0) {
			irq = spin_lock_irqsave(&namespace_lock);
			mountp->m_state = MOUNT_STATE_LIVE;
			mountp->m_sibling = NULL;
			mountp->m_next = NULL;
			link_child(mountp->m_parent, mountp);
			link_global(mountp);
			spin_unlock_irqrestore(&namespace_lock, irq);
			mount_release(mountp);
			return error;
		}
	} else if (mountp->m_bind_source != NULL) {
		mount_release(mountp->m_bind_source);
	}
	path_release(&mountp->m_cover);
	if ((mountp->m_internal_flags & MOUNT_BIND_INTERNAL) == 0)
		finalize_filesystem_destroy(mountp);
	else if (mountp->m_root != NULL)
		inode_release(mountp->m_root);
	mount_release(mountp);
	mount_free(mountp);
	return 0;
}
