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

	/* Finds one complete aligned data block within the filesystem geometry. */
	for (cg = 0; cg < super->ncg; cg++) {
		start = cgstart(super, cg);
		if (child < start || child - start < super->dblkno ||
		    child - start >= cg_ndblk(super, cg))
			continue;
		*local = (uint32_t)(child - start);
		if (*local % super->frag != 0 || super->frag > cg_ndblk(super, cg) - *local)
			return EIO;
		*group = cg;
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

	/* Validates the complete inode owner before changing private allocation maps. */
	ms = state(inode->i_mount);
	ui = info(inode);
	keep = length != 0;
	released = count - keep;
	if (!ms->writable)
		return EROFS;
	if (ui->blocks < (uint64_t)count * (ms->super.bsize / UFS_SECTOR_SIZE) ||
	    ms->super.cstotal_nbfree > UINT64_MAX - released)
		return EIO;

	/* Verifies the retained payload block remains allocated before replacing it. */
	if (keep != 0) {
		error = xattr_release_location(&ms->super, ui->extattr[0], &cg, &local);
		if (error != 0)
			return error;
		error = load_cg_locked(inode->i_mount, cg);
		if (error != 0)
			return error;
		for (n = 0; n < ms->super.frag; n++) {
			if (bit_test(ms->cg + ms->cg_freeoff, local + n))
				return EIO;
		}
		memset(group->data, 0, ms->super.bsize);
		memcpy(group->data, area, length);
	}

	/* Loads each affected CG once and removes only allocated, distinct blocks. */
	for (index = keep; index < count; index++) {
		child = ui->extattr[index];
		for (n = 0; n < index; n++) {
			if (ui->extattr[n] == child)
				return EIO;
		}
		error = xattr_release_location(&ms->super, child, &cg, &local);
		if (error != 0)
			return error;
		for (slot = 0; slot < group->group_count; slot++) {
			if (group->groups[slot] == cg)
				break;
		}
		if (slot == group->group_count) {
			error = load_cg_locked(inode->i_mount, cg);
			if (error != 0)
				return error;
			memcpy(group->cg[slot], ms->cg, ms->super.bsize);
			group->groups[slot] = cg;
			group->group_count++;
		}

		/* Reads offsets from this CG image because different CG layouts may differ. */
		n = ufs_get32(group->cg[slot], UFS_CG_FREEOFF, ms->super.swapped);
		for (cg = 0; cg < ms->super.frag; cg++) {
			if (bit_test(group->cg[slot] + n, local + cg))
				return EIO;
			bit_set(group->cg[slot] + n, local + cg);
		}
		free_blocks = ufs_get32(group->cg[slot], UFS_CG_NBFREE, ms->super.swapped);
		if (free_blocks == UINT32_MAX)
			return EIO;
		ufs_put32(group->cg[slot], UFS_CG_NBFREE, free_blocks + 1U, ms->super.swapped);
	}

	/* Prepares the complete new serialized area and its remaining block ownership. */
	memcpy(&group->image, ui, sizeof(group->image));
	memset(group->image.extattr, 0, sizeof(group->image.extattr));
	if (keep != 0)
		group->image.extattr[0] = ui->extattr[0];
	group->image.extattr_size = (uint32_t)length;
	group->image.blocks -= (uint64_t)released * (ms->super.bsize / UFS_SECTOR_SIZE);
	error = prepare_inode_locked(&group->image.inode, group->dinode, &fragment);
	if (error != 0)
		return error;
	if (released != 0) {
		error = prepare_super_summaries(inode->i_mount, group->summaries);
		if (error != 0)
			return error;
		ufs_put64(group->summaries, UFS_FS_CSTOTAL_NBFREE,
		    ms->super.cstotal_nbfree + released, ms->super.swapped);
	}

	/* Publishes only changed maps plus the payload, dinode and changed totals. */
	extent_count = 0;
	for (n = 0; n < group->group_count; n++) {
		extents[extent_count].target = (cgstart(&ms->super, group->groups[n]) + ms->super.cblkno) << ms->super.fsbtodb;
		extents[extent_count].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
		extents[extent_count].payload = group->cg[n];
		extent_count++;
	}
	if (released != 0) {
		extents[extent_count].target = UFS_SBLOCK_OFFSET / UFS_SECTOR_SIZE;
		extents[extent_count].sectors = UFS_SBLOCK_SIZE / UFS_SECTOR_SIZE;
		extents[extent_count].payload = group->summaries;
		extent_count++;
	}
	if (keep != 0) {
		extents[extent_count].target = ui->extattr[0] << ms->super.fsbtodb;
		extents[extent_count].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
		extents[extent_count].payload = group->data;
		extent_count++;
	}
	extents[extent_count].target = fragment << ms->super.fsbtodb;
	extents[extent_count].sectors = ms->super.bsize / UFS_SECTOR_SIZE;
	extents[extent_count].payload = group->dinode;
	extent_count++;
	ms->cg_valid = 0;
	buf_view_release(&ms->cg_view);
	error = metadata_group_commit(inode->i_mount, extents, extent_count, NULL, &outcome);

	/* Publishes live attribute ownership and releases quota only after proven commit. */
	if (outcome.committed) {
		memcpy(ui->extattr, group->image.extattr, sizeof(ui->extattr));
		ui->extattr_size = group->image.extattr_size;
		ui->blocks = group->image.blocks;
		ms->super.cstotal_nbfree += released;
		quota_error = 0;
		if (released != 0)
			quota_error = quota_release(&ms->quota, inode->i_uid, inode->i_gid, released, 0);
		if (quota_error != 0) {
			ms->writable = 0;
			if (error == 0)
				error = quota_error;
		}
	}
	if (outcome.committed || outcome.uncertain)
		ms->cg_dirty = outcome.uncertain;

	/* Returns the original errno without compensating writes or repeated frees. */
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

	/* Bounds the complete serialized area before accepting this operation. */
	ms = state(inode->i_mount);
	ui = info(inode);
	*handled = 0;
	if (!ms->journal_enabled)
		return 0;
	*handled = 1;
	keep = length != 0;
	if (length > ms->super.bsize || (keep != 0 && area == NULL))
		return EINVAL;
	if ((uint64_t)ui->extattr_size > (uint64_t)UFS_NXADDR * ms->super.bsize)
		return EIO;
	count = (ui->extattr_size + ms->super.bsize - 1U) / ms->super.bsize;
	for (n = 0; n < UFS_NXADDR; n++) {
		if ((n < count) != (ui->extattr[n] != 0))
			return EIO;
	}
	if (count == 0) {
		if (keep != 0)
			*handled = 0;
		return 0;
	}
	unique = 0;

	/* Reserves the actual deduplicated footprint before admitting the group. */
	for (n = keep; n < count; n++) {
		error = xattr_release_location(&ms->super, ui->extattr[n], &groups[n], &local);
		if (error != 0)
			return error;
		for (j = keep; j < n; j++) {
			if (groups[j] == groups[n])
				break;
		}
		if (j == n)
			unique++;
	}
	bytes = (unique + 1U + keep) * ms->super.bsize;
	if (count > keep)
		bytes += UFS_SBLOCK_SIZE;
	if (ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U) {
		*handled = 0;
		return 0;
	}

	/* Allocates the changed maps, dinode, optional payload and optional totals. */
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;
	group->memory = kern_malloc(bytes);
	if (group->memory == NULL) {
		kern_free(group);
		return ENOMEM;
	}
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
	return error;
}
