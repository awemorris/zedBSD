/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

static int retire_inode_locked(struct inode *inode, struct ufs_release_group *group);
static int retire_inode_group(struct inode *inode, int *handled);

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
	int error;
	int quota_error;

	/* Refuses reuse while any persistent block owner or namespace link remains. */
	ms = state(inode->i_mount);
	ui = info(inode);
	if (!ms->writable)
		return EROFS;
	if (inode->i_ino <= UFS_ROOT_INO ||
	    (uint64_t)inode->i_ino >= (uint64_t)ms->super.ncg * ms->super.ipg ||
	    inode->i_linkcount != 0 ||
	    inode->i_size != 0 || ui->blocks != 0 || ui->extattr_size != 0)
		return EIO;
	for (n = 0; n < UFS_NDADDR; n++) {
		if (ui->direct[n] != 0)
			return EIO;
	}
	for (n = 0; n < UFS_NIADDR; n++) {
		if (ui->indirect[n] != 0)
			return EIO;
	}
	for (n = 0; n < UFS_NXADDR; n++) {
		if (ui->extattr[n] != 0)
			return EIO;
	}

	/* Copies the allocated inode map and validates totals before private edits. */
	cg = inode->i_ino / ms->super.ipg;
	local = inode->i_ino % ms->super.ipg;
	error = load_cg_locked(inode->i_mount, cg);
	if (error != 0)
		return error;
	if (!bit_test(ms->cg + ms->cg_iusedoff, local))
		return EIO;
	free_inodes = ufs_get32(ms->cg, UFS_CG_NIFREE, ms->super.swapped);
	directories = ufs_get32(ms->cg, UFS_CG_NDIR, ms->super.swapped);
	is_directory = inode->i_type == INODE_DIR;
	if (free_inodes >= ms->super.ipg || ms->super.cstotal_nifree == UINT64_MAX ||
	    (is_directory && (directories == 0 || ms->super.cstotal_ndir == 0)))
		return EIO;
	memcpy(group->cg, ms->cg, ms->super.bsize);
	memcpy(&group->image, ui, sizeof(group->image));
	group->image.inode.i_mode = 0;
	group->image.inode.i_type = INODE_NONE;
	error = prepare_inode_locked(&group->image.inode, group->dinode, &fragment);
	if (error != 0)
		return error;
	error = prepare_super_summaries(inode->i_mount, group->summaries);
	if (error != 0)
		return error;

	/* Makes the inode reusable only in the same group that retires its old kind. */
	bit_clear(group->cg + ms->cg_iusedoff, local);
	ufs_put32(group->cg, UFS_CG_NIFREE, free_inodes + 1U, ms->super.swapped);
	ufs_put64(group->summaries, UFS_FS_CSTOTAL_NIFREE,
	    ms->super.cstotal_nifree + 1U, ms->super.swapped);
	if (is_directory) {
		ufs_put32(group->cg, UFS_CG_NDIR, directories - 1U, ms->super.swapped);
		ufs_put64(group->summaries, UFS_FS_CSTOTAL_NDIR,
		    ms->super.cstotal_ndir - 1U, ms->super.swapped);
	}
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

	/* Publishes positive retirement and releases quota once, even on recovered error. */
	if (outcome.committed) {
		memcpy(ms->cg, group->cg, ms->super.bsize);
		ms->super.cstotal_nifree++;
		if (is_directory)
			ms->super.cstotal_ndir--;
		inode->i_mode = 0;
		inode->i_type = INODE_NONE;

		/* Prevents final-reference retry from writing an already reusable identity. */
		inode->i_ino = 0;
		quota_error = quota_release(&ms->quota, inode->i_uid, inode->i_gid, 0, 1);
		if (quota_error != 0) {
			ms->writable = 0;
			if (error == 0)
				error = quota_error;
		}
	}
	if (outcome.committed || outcome.uncertain)
		ms->cg_dirty = outcome.uncertain;

	/* Preserves the original failure independently of established retirement. */
	return error;
}

/* Owns private retirement storage and exclusion after data and xattr teardown. */
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
	bytes = 2U * ms->super.bsize + UFS_SBLOCK_SIZE;
	if (!ms->journal_enabled || ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
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

	/* Returns the admitted result without a second compensating retirement. */
	return error;
}
