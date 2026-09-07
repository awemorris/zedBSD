/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

struct ufs_inode_reservation {
	struct ufs_release_group images;
	struct ufs_transaction_outcome outcome;
	struct quota_charge charge;
};

static int reserve_inode_locked(struct inode *inode, const struct inode_creation_request *request, struct ufs_inode_reservation *group);
static int reserve_inode_group(struct inode *inode, const struct inode_creation_request *request);

/* Reserves a number with initialized zero-link identity and directory accounting. */
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
	int found;
	int error;

	/* Validates the requested kind before selecting an unowned inode slot. */
	ms = state(inode->i_mount);
	if (!ms->writable)
		return EROFS;
	kind = inode_type_mode(request->type);
	if (kind == 0 || inode->i_ino != 0)
		return EINVAL;
	found = 0;
	cg = local = 0;
	is_directory = request->type == INODE_DIR;

	/* Finds a free number while excluding all allocation-map mutations. */
	for (attempt = 0; attempt < ms->super.ncg; attempt++) {
		cg = (ms->rotor_cg + attempt) % ms->super.ncg;
		error = load_cg_locked(inode->i_mount, cg);
		if (error != 0)
			return error;
		local = cg == 0 ? UFS_ROOT_INO + 1U : 0U;
		for (; local < ms->super.ipg; local++) {
			if (!bit_test(ms->cg + ms->cg_iusedoff, local)) {
				found = 1;
				break;
			}
		}
		if (found)
			break;
	}
	if (!found)
		return ENOSPC;
	free_inodes = ufs_get32(ms->cg, UFS_CG_NIFREE, ms->super.swapped);
	directories = ufs_get32(ms->cg, UFS_CG_NDIR, ms->super.swapped);
	if (free_inodes == 0 || ms->super.cstotal_nifree == 0 ||
	    (is_directory && (directories == UINT32_MAX || ms->super.cstotal_ndir == UINT64_MAX)))
		return EIO;

	/* Initializes all persistent ownership fields before making the slot allocated. */
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
	error = read_block(inode->i_mount, fragment, group->images.dinode);
	if (error != 0)
		return error;
	raw = group->images.dinode + (local % ms->super.inopb) * UFS_DINODE_SIZE;
	generation = ufs_get32(raw, UFS_DI_GEN, ms->super.swapped) + 1U;
	if (generation == 0)
		generation = 1;
	memset(raw, 0, UFS_DINODE_SIZE);
	image->generation = generation;
	encode_inode_locked(&image->inode, group->images.dinode);
	ufs_put32(raw, UFS_DI_GEN, generation, ms->super.swapped);
	error = prepare_super_summaries(inode->i_mount, group->images.summaries);
	if (error != 0)
		return error;

	/* Couples the new identity with inode and directory allocation totals. */
	memcpy(group->images.cg, ms->cg, ms->super.bsize);
	bit_set(group->images.cg + ms->cg_iusedoff, local);
	ufs_put32(group->images.cg, UFS_CG_NIFREE, free_inodes - 1U, ms->super.swapped);
	ufs_put64(group->images.summaries, UFS_FS_CSTOTAL_NIFREE,
	    ms->super.cstotal_nifree - 1U, ms->super.swapped);
	if (is_directory) {
		ufs_put32(group->images.cg, UFS_CG_NDIR, directories + 1U, ms->super.swapped);
		ufs_put64(group->images.summaries, UFS_FS_CSTOTAL_NDIR,
		    ms->super.cstotal_ndir + 1U, ms->super.swapped);
	}
	extents[0].target = (cgstart(&ms->super, cg) + ms->super.cblkno) << ms->super.fsbtodb;
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
	error = metadata_group_commit(inode->i_mount, extents, 3, NULL, &group->outcome);

	/* Publishes only established identity without copying mutexes or reference state. */
	if (group->outcome.committed) {
		memcpy(ms->cg, group->images.cg, ms->super.bsize);
		ms->super.cstotal_nifree--;
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
	if (group->outcome.committed || group->outcome.uncertain)
		ms->cg_dirty = group->outcome.uncertain;

	/* Preserves original errors separately from durable identity ownership. */
	return error;
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

	/* Allocates all staging memory before reserving quota or entering the mount. */
	ms = state(inode->i_mount);
	bytes = 2U * ms->super.bsize + UFS_SBLOCK_SIZE;
	group = kern_calloc(1, sizeof(*group));
	if (group == NULL)
		return ENOMEM;
	group->images.memory = kern_malloc(bytes);
	if (group->images.memory == NULL) {
		kern_free(group);
		return ENOMEM;
	}
	group->images.cg = group->images.memory;
	group->images.dinode = group->images.cg + ms->super.bsize;
	group->images.summaries = group->images.dinode + ms->super.bsize;
	error = quota_reserve(&ms->quota, request->uid, request->gid, 0, 1, quota_now(), &group->charge);
	if (error == 0) {
		mutex_lock(&inode->i_lock);
		mutex_lock(&ms->lock);
		error = reserve_inode_locked(inode, request, group);

		/* Holds quota for every possibly committed reservation until recovery. */
		if (group->outcome.committed || group->outcome.uncertain)
			quota_commit(&group->charge);
		else
			quota_rollback(&group->charge);
		mutex_unlock(&ms->lock);
		mutex_unlock(&inode->i_lock);
	}
	kern_free(group->images.memory);
	kern_free(group);

	/* Leaves failure cleanup and final name publication to the creation owner. */
	return error;
}
