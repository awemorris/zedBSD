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
	if (!ms->writable)
		return EROFS;
	if (directory->i_type != INODE_DIR || target->i_linkcount != 0 ||
	    target->i_ino <= UFS_ROOT_INO || target->i_ino > UINT32_MAX ||
	    inode_type_mode(target->i_type) == 0 || info(directory)->direct[0] == 0)
		return EIO;
	is_directory = target->i_type == INODE_DIR;
	if (is_directory && directory->i_linkcount == UINT16_MAX)
		return EMLINK;

	/* Keeps all prepared content while changing only the final link relationship. */
	memcpy(&group->directory_image, info(directory), sizeof(group->directory_image));
	memcpy(&group->target_image, info(target), sizeof(group->target_image));
	group->target_image.inode.i_linkcount = is_directory ? 2 : 1;
	if (is_directory)
		group->directory_image.inode.i_linkcount++;
	error = metadata_image_get(&group->images, info(directory)->direct[0], &group->directory);
	if (error != 0)
		return error;
	error = directory_image_insert(&group->directory_image.inode, name, target, group->directory);
	if (error != 0)
		return error;

	/* Merges shared parent and child slots before issuing any home mutation. */
	error = metadata_image_inode(&group->images, &group->directory_image.inode);
	if (error != 0)
		return error;
	error = metadata_image_inode(&group->images, &group->target_image.inode);
	if (error != 0)
		return error;
	error = metadata_group_commit(directory->i_mount, group->images.extents,
	    group->images.count, NULL, &group->outcome);

	/* Reflects a proven publication even when its checkpoint returned an error. */
	if (group->outcome.committed) {
		directory->i_size = group->directory_image.inode.i_size;
		directory->i_linkcount = group->directory_image.inode.i_linkcount;
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

	/* Establishes an unambiguous unpublished outcome before allocating resources. */
	memset(outcome, 0, sizeof(*outcome));
	if (directory == target || directory->i_mount != target->i_mount)
		return EINVAL;
	ms = state(directory->i_mount);
	bytes = 3U * ms->super.bsize;
	if (!ms->journal_enabled || ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)
		return EOPNOTSUPP;
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(bytes);
	if (group->memory == NULL) {
		kern_free(group);
		return ENOMEM;
	}

	/* Prepares recoverable empty parent backing before publishing the new name. */
	metadata_images_init(&group->images, directory->i_mount, group->memory, bytes);
	mutex_lock(&directory->i_lock);
	error = 0;
	if (info(directory)->direct[0] == 0) {
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

	/* Invalidates stale names when the generic caller cannot report success. */
	if (error != 0 && (outcome->committed || outcome->uncertain)) {
		namecache_remove(directory, name);
		inode_dir_changed(directory);
	}
	kern_free(group->memory);
	kern_free(group);

	/* Preserves the admitted error without falling back to independent writes. */
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
	if (error == 0) {
		*result = target;
		return 0;
	}

	/* Leaves committed or unresolved names for ordinary lifetime and mount recovery. */
	if (outcome.committed || outcome.uncertain) {
		detach_new_socket_special(target);
		inode_release(target);
		return error;
	}

	/* Reclaims only a child whose name publication was definitely not admitted. */
	error = discard_new_inode_after_error(target, target->i_type == INODE_DIR, error);
	return error;
}
