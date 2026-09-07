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

	/* Validates the locked name-to-inode relation before editing private bytes. */
	if (!ms->writable)
		return EROFS;
	if (target->i_linkcount == 0 || (removing_directory && directory->i_linkcount == 0))
		return EIO;
	error = dir_find_record(directory, name, group->directory, &offset, &previous, &number);
	if (error != 0)
		return error;
	if (number != (uint32_t)target->i_ino)
		return EIO;
	length = ufs_get16(group->directory, offset + 4U, ms->super.swapped);
	if (previous != UINT32_MAX && previous / UFS_DIRBLKSIZ == offset / UFS_DIRBLKSIZ) {
		prior = ufs_get16(group->directory, previous + 4U, ms->super.swapped);
		ufs_put16(group->directory, previous + 4U, prior + length, ms->super.swapped);
	} else {
		ufs_put32(group->directory, offset, 0, ms->super.swapped);
	}
	memcpy(&group->image, info(target), sizeof(group->image));
	if (removing_directory)
		group->image.inode.i_linkcount = 0;
	else
		group->image.inode.i_linkcount--;
	error = prepare_inode_locked(&group->image.inode, group->dinode, &fragment);
	if (error != 0)
		return error;

	/* Merges parent accounting with the target when both share a dinode block. */
	if (removing_directory) {
		memcpy(&group->parent_image, info(directory), sizeof(group->parent_image));
		group->parent_image.inode.i_linkcount--;
		parent_fragment = inode_fragment(directory);
		if (parent_fragment == fragment) {
			encode_inode_locked(&group->parent_image.inode, group->dinode);
		} else {
			error = prepare_inode_locked(&group->parent_image.inode, group->parent_dinode, &parent_fragment);
			if (error != 0)
				return error;
			extents[2].target = parent_fragment << ms->super.fsbtodb;
			extents[2].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
			extents[2].payload = group->parent_dinode;
			count++;
		}
	}

	/* Publishes directory bytes and all changed dinodes in one durable transaction. */
	extents[0].target = info(directory)->direct[0] << ms->super.fsbtodb;
	extents[0].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[0].payload = group->directory;
	extents[1].target = fragment << ms->super.fsbtodb;
	extents[1].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[1].payload = group->dinode;
	error = metadata_group_commit(directory->i_mount, extents, count, NULL, &group->outcome);
	if (group->outcome.committed) {
		target->i_linkcount = group->image.inode.i_linkcount;
		if (removing_directory)
			directory->i_linkcount = group->parent_image.inode.i_linkcount;
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

	/* Declines only before admission, independently of callback error values. */
	*handled = 0;
	if (!ms->journal_enabled)
		return 0;
	if (directory == target || name_is_dot(name)) {
		*handled = 1;
		return EINVAL;
	}
	bytes = 2U * ms->super.bsize;
	if (target->i_type == INODE_DIR && inode_fragment(directory) != inode_fragment(target))
		bytes += ms->super.bsize;
	if (bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)
		return 0;
	*handled = 1;
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(bytes);
	if (group->memory == NULL) {
		kern_free(group);
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

	/* Generic VFS invalidates only on success; uncertain/error outcomes need this. */
	if (error != 0 && (group->outcome.committed || group->outcome.uncertain)) {
		namecache_remove(directory, name);
		inode_dir_changed(directory);
	}
	kern_free(group->memory);
	kern_free(group);

	/* Reports the original group outcome after releasing all transient ownership. */
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

	/* Bounds names and the single-block directory before modifying private bytes. */
	if (name->cn_namelen == 0 || name->cn_namelen > 255U)
		return EINVAL;
	for (n = 0; n < name->cn_namelen; n++) {
		if (name->cn_nameptr[n] == '/')
			return EINVAL;
	}
	if (directory->i_size < 0 || (uint64_t)directory->i_size > ms->super.bsize ||
	    (uint64_t)directory->i_size % UFS_DIRBLKSIZ != 0)
		return EIO;
	need = dir_minimum((uint8_t)name->cn_namelen);

	/* Chooses the first available record while validating the complete directory. */
	pos = 0;
	while (pos < (uint32_t)directory->i_size) {
		if ((uint32_t)directory->i_size - pos < 8U || pos % UFS_DIRBLKSIZ > UFS_DIRBLKSIZ - 8U)
			return EIO;
		number = ufs_get32(block, pos, ms->super.swapped);
		length = ufs_get16(block, pos + 4U, ms->super.swapped);
		namesize = block[pos + 7U];
		minimum = dir_minimum(namesize);
		if (length < minimum || (length & 3U) != 0 ||
		    pos % UFS_DIRBLKSIZ + length > UFS_DIRBLKSIZ ||
		    length > (uint32_t)directory->i_size - pos)
			return EIO;
		if (number != 0 && namesize == name->cn_namelen &&
		    memcmp(block + pos + 8U, name->cn_nameptr, namesize) == 0)
			return EEXIST;
		if (at == UINT32_MAX) {
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
	if (at == UINT32_MAX) {
		if ((uint64_t)directory->i_size + UFS_DIRBLKSIZ > ms->super.bsize)
			return ENOSPC;
		at = (uint32_t)directory->i_size;
		available = UFS_DIRBLKSIZ;
		directory->i_size += UFS_DIRBLKSIZ;
	} else {
		/* Splits an occupied predecessor only after every record has been checked. */
		pos = 0;
		while (pos < at) {
			length = ufs_get16(block, pos + 4U, ms->super.swapped);
			if (pos + length > at) {
				ufs_put16(block, pos + 4U, (uint16_t)(at - pos), ms->super.swapped);
				break;
			}
			pos += length;
		}
	}
	ufs_put32(block, at, (uint32_t)target->i_ino, ms->super.swapped);
	ufs_put16(block, at + 4U, available, ms->super.swapped);
	block[at + 6U] = dir_type(target->i_type);
	block[at + 7U] = (uint8_t)name->cn_namelen;
	memcpy(block + at + 8U, name->cn_nameptr, name->cn_namelen);

	/* Returns a complete insertion with no persistent or live inode mutation. */
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

	if (!ms->writable)
		return EROFS;
	if (target->i_linkcount == UINT16_MAX)
		return EMLINK;
	memcpy(&group->directory_image, info(directory), sizeof(group->directory_image));
	memcpy(&group->target_image, info(target), sizeof(group->target_image));
	group->target_image.inode.i_linkcount++;
	error = metadata_image_get(&group->images, info(directory)->direct[0], &group->directory);
	if (error != 0)
		return error;
	error = directory_image_insert(&group->directory_image.inode, name, target, group->directory);
	if (error != 0)
		return error;

	/* Encodes all changed dinodes into unique, shared physical block images. */
	error = metadata_image_inode(&group->images, &group->directory_image.inode);
	if (error != 0)
		return error;
	error = metadata_image_inode(&group->images, &group->target_image.inode);
	if (error != 0)
		return error;
	error = metadata_group_commit(directory->i_mount, group->images.extents,
	    group->images.count, NULL, &group->outcome);
	if (group->outcome.committed) {
		directory->i_size = group->directory_image.inode.i_size;

		/* The generic inode_link wrapper increments live nlink only on success. */
		if (error != 0)
			target->i_linkcount = group->target_image.inode.i_linkcount;
	}
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
	if (!ms->journal_enabled || info(directory)->direct[0] == 0)
		return 0;
	bytes = (inode_fragment(directory) == inode_fragment(target) ? 2U : 3U) * ms->super.bsize;
	if (bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)
		return 0;
	*handled = 1;
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(bytes);
	if (group->memory == NULL) {
		kern_free(group);
		return ENOMEM;
	}
	metadata_images_init(&group->images, directory->i_mount, group->memory, bytes);
	mutex_lock(&directory->i_lock);
	mutex_lock(&target->i_lock);
	mutex_lock(&ms->lock);
	error = link_group_locked(directory, name, target, group);
	mutex_unlock(&ms->lock);
	mutex_unlock(&target->i_lock);
	mutex_unlock(&directory->i_lock);
	if (error != 0 && (group->outcome.committed || group->outcome.uncertain)) {
		namecache_remove(directory, name);
		inode_dir_changed(directory);
	}
	kern_free(group->memory);
	kern_free(group);
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
	if (directory->i_size < 0 || (uint64_t)directory->i_size > ms->super.bsize ||
	    (uint64_t)directory->i_size % UFS_DIRBLKSIZ != 0)
		return EIO;
	pos = 0;
	previous = found = prior = UINT32_MAX;

	/* Records one matching entry while rejecting malformed or duplicate records. */
	while (pos < (uint32_t)directory->i_size) {
		if ((uint32_t)directory->i_size - pos < 8U)
			return EIO;
		length = ufs_get16(block, pos + 4U, ms->super.swapped);
		namesize = block[pos + 7U];
		if (length < dir_minimum(namesize) || (length & 3U) != 0 ||
		    pos % UFS_DIRBLKSIZ + length > UFS_DIRBLKSIZ ||
		    length > (uint32_t)directory->i_size - pos)
			return EIO;
		number = ufs_get32(block, pos, ms->super.swapped);
		if (number != 0 && namesize == name->cn_namelen &&
		    memcmp(block + pos + 8U, name->cn_nameptr, namesize) == 0) {
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

	/* Replaces the inode reference or joins a removed record to its predecessor. */
	if (replacement != 0) {
		ufs_put32(block, found, replacement, ms->super.swapped);
		block[found + 6U] = type;
	} else if (prior != UINT32_MAX && prior / UFS_DIRBLKSIZ == found / UFS_DIRBLKSIZ) {
		length = ufs_get16(block, found + 4U, ms->super.swapped);
		length += ufs_get16(block, prior + 4U, ms->super.swapped);
		ufs_put16(block, prior + 4U, length, ms->super.swapped);
	} else {
		ufs_put32(block, found, 0, ms->super.swapped);
	}

	/* Leaves every changed byte private until the enclosing group is committed. */
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

	/* Copies only locked inode state and keeps a single image for identical parents. */
	ms = state(old_directory->i_mount);
	if (!ms->writable)
		return EROFS;
	memcpy(&group->old_image, info(old_directory), sizeof(group->old_image));
	memcpy(&group->new_image, info(new_directory), sizeof(group->new_image));
	old_image = &group->old_image.inode;
	new_image = old_directory == new_directory ? old_image : &group->new_image.inode;
	moving_directory = source->i_type == INODE_DIR;

	/* Validates all link transitions before editing namespace bytes. */
	if (source->i_linkcount == 0 || (target != NULL && target->i_linkcount == 0))
		return EIO;
	if (moving_directory && old_directory != new_directory) {
		if (old_image->i_linkcount == 0)
			return EIO;
		if (target == NULL && new_image->i_linkcount == UINT16_MAX)
			return EMLINK;
		old_image->i_linkcount--;
		if (target == NULL)
			new_image->i_linkcount++;
	} else if (moving_directory && target != NULL) {
		if (old_image->i_linkcount == 0)
			return EIO;
		old_image->i_linkcount--;
	}
	if (target != NULL) {
		memcpy(&group->target_image, info(target), sizeof(group->target_image));
		if (moving_directory)
			group->target_image.inode.i_linkcount = 0;
		else
			group->target_image.inode.i_linkcount--;
	}

	/* Removes the old name first so a full same-parent directory can reuse its space. */
	error = metadata_image_get(&group->images, info(old_directory)->direct[0], &old_block);
	if (error != 0)
		return error;
	error = directory_image_change(old_image, old_block, old_name, (uint32_t)source->i_ino, 0, 0);
	if (error != 0)
		return error;
	error = metadata_image_get(&group->images, info(new_directory)->direct[0], &new_block);
	if (error != 0)
		return error;
	if (target != NULL) {
		error = directory_image_change(new_image, new_block, new_name,
		    (uint32_t)target->i_ino, (uint32_t)source->i_ino, dir_type(source->i_type));
	} else {
		error = directory_image_insert(new_image, new_name, source, new_block);
	}
	if (error != 0)
		return error;

	/* Reparents a moved directory in the same transaction as its visible names. */
	if (moving_directory && old_directory != new_directory) {
		error = metadata_image_get(&group->images, info(source)->direct[0], &child_block);
		if (error != 0)
			return error;
		error = directory_image_change(source, child_block, &dotdot,
		    (uint32_t)old_directory->i_ino, (uint32_t)new_directory->i_ino, 4);
		if (error != 0)
			return error;
	}

	/* Merges all changed dinodes without reloading shared physical blocks. */
	error = metadata_image_inode(&group->images, old_image);
	if (error != 0)
		return error;
	if (new_image != old_image) {
		error = metadata_image_inode(&group->images, new_image);
		if (error != 0)
			return error;
	}
	if (target != NULL) {
		error = metadata_image_inode(&group->images, &group->target_image.inode);
		if (error != 0)
			return error;
	}
	error = metadata_group_commit(old_directory->i_mount, group->images.extents,
	    group->images.count, NULL, &group->outcome);

	/* Publishes only the live fields whose persistent images are proven committed. */
	if (group->outcome.committed) {
		old_directory->i_size = old_image->i_size;
		old_directory->i_linkcount = old_image->i_linkcount;
		new_directory->i_size = new_image->i_size;
		new_directory->i_linkcount = new_image->i_linkcount;
		if (target != NULL) {
			target->i_linkcount = group->target_image.inode.i_linkcount;
			if (target->i_linkcount == 0)
				target->i_flags |= INODE_DEAD;
		}
	}

	/* Preserves an error even when recovery establishes that the rename committed. */
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

	/* Declines unsupported backing before admission and never falls back on errno. */
	ms = state(old_directory->i_mount);
	*handled = 0;
	if (!ms->journal_enabled || info(new_directory)->direct[0] == 0)
		return 0;
	*handled = 1;
	if (source == old_directory || source == new_directory ||
	    target == old_directory || target == new_directory)
		return EINVAL;

	/* Counts distinct blocks so shared dinodes do not unnecessarily exhaust a slot. */
	fragments[0] = info(old_directory)->direct[0];
	fragments[1] = info(new_directory)->direct[0];
	fragments[2] = inode_fragment(old_directory);
	fragments[3] = inode_fragment(new_directory);
	count = 4;
	if (source->i_type == INODE_DIR && old_directory != new_directory)
		fragments[count++] = info(source)->direct[0];
	if (target != NULL)
		fragments[count++] = inode_fragment(target);
	unique = 0;

	/* Deduplicates the bounded footprint before allocating any private image. */
	for (n = 0; n < count; n++) {
		for (j = 0; j < n; j++) {
			if (fragments[j] == fragments[n])
				break;
		}
		if (j == n)
			unique++;
	}
	bytes = unique * ms->super.bsize;
	if (ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
		*handled = 0;
		return 0;
	}

	/* Allocates the operation and its exact private block storage. */
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(bytes);
	if (group->memory == NULL) {
		kern_free(group);
		return ENOMEM;
	}
	metadata_images_init(&group->images, old_directory->i_mount, group->memory, bytes);
	locks[0] = old_directory;
	lock_count = 1;
	if (new_directory != old_directory)
		locks[lock_count++] = new_directory;
	locks[lock_count++] = source;
	if (target != NULL && target != source)
		locks[lock_count++] = target;

	/* Retains namespace exclusion from the caller while locking each inode once. */
	for (n = 0; n < lock_count; n++)
		mutex_lock(&locks[n]->i_lock);
	mutex_lock(&ms->lock);
	error = rename_group_locked(old_directory, old_name, new_directory, new_name, source, target, group);
	mutex_unlock(&ms->lock);

	/* Releases inode ownership before cache publication, including error outcomes. */
	while (lock_count != 0)
		mutex_unlock(&locks[--lock_count]->i_lock);
	if (error != 0 && (group->outcome.committed || group->outcome.uncertain)) {
		namecache_remove(old_directory, old_name);
		namecache_remove(new_directory, new_name);
		inode_dir_changed(old_directory);
		if (new_directory != old_directory)
			inode_dir_changed(new_directory);
		if (source->i_type == INODE_DIR && old_directory != new_directory)
			inode_dir_changed(source);
	}
	kern_free(group->memory);
	kern_free(group);

	/* Returns the original transaction result after releasing transient ownership. */
	return error;
}
