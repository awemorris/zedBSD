/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

struct ufs_orphan_scan {
	struct ufs_inode_info inode;
	uint8_t *bitmap;
	uint8_t *block;
};

static int orphan_recover_one(struct mount *mountp, uint32_t number, const uint8_t *raw, struct ufs_inode_info *owner);
static int orphan_scan_locked(struct mount *mountp, struct ufs_orphan_scan *scan);
static int orphan_recover(struct mount *mountp);

/* Reclaims one validated zero-link identity without publishing a cache object. */
static int
orphan_recover_one(
	struct mount *mountp,
	uint32_t number,
	const uint8_t *raw,
	struct ufs_inode_info *owner)
{
	struct ufs_mount_state *ms;
	size_t bytes;
	int error;

	/* Requires grouped release and retirement before permitting recovery mutations. */
	ms = state(mountp);
	bytes = 3U * ms->super.bsize + UFS_SBLOCK_SIZE;
	if (ms->journal.sector_count <= 2U ||
	    bytes / UFS_SECTOR_SIZE > UFS_JOURNAL_GROUP_SECTORS ||
	    bytes / UFS_SECTOR_SIZE > ms->journal.sector_count - 2U)
		return EOPNOTSUPP;
	memset(owner, 0, sizeof(*owner));
	owner->inode.i_mount = mountp;
	(void)mutex_init(&owner->inode.i_lock, LOCK_RANK_INODE, "ufs orphan");
	error = decode_inode_raw(&owner->inode, raw, number, 1);
	if (error != 0)
		return error;

	/* Reuses checked pointer/xattr/bitmap owners and their conservative outcomes. */
	error = reclaim_unlinked_inode(&owner->inode);
	return error;
}

/* Scans a private mount using stable candidate bits and freshly read dinodes. */
static int
orphan_scan_locked(
	struct mount *mountp,
	struct ufs_orphan_scan *scan)
{
	struct ufs_mount_state *ms;
	uint8_t *raw;
	uint64_t number;
	uint64_t fragment;
	uint32_t cg;
	uint32_t index;
	size_t map_bytes;
	int error;

	/* Saves each candidate map before reclaim can change the mount's CG buffer. */
	ms = state(mountp);
	map_bytes = ((size_t)ms->super.ipg + 7U) / 8U;
	for (cg = 0; cg < ms->super.ncg; cg++) {
		error = load_cg_locked(mountp, cg);
		if (error != 0)
			return error;
		memcpy(scan->bitmap, ms->cg + ms->cg_iusedoff, map_bytes);

		/* Leaves reserved slots and linked namespace owners untouched. */
		for (index = 0; index < ms->super.ipg; index++) {
			if (!bit_test(scan->bitmap, index))
				continue;
			number = (uint64_t)cg * ms->super.ipg + index;
			if (number <= UFS_ROOT_INO)
				continue;
			if (number > UINT32_MAX)
				return EOVERFLOW;
			fragment = cgstart(&ms->super, cg) + ms->super.iblkno +
			    (index / ms->super.inopb) * ms->super.frag;
			error = read_block(mountp, fragment, scan->block);
			if (error != 0)
				return error;
			raw = scan->block + (index % ms->super.inopb) * UFS_DINODE_SIZE;
			if (ufs_get16(raw, UFS_DI_NLINK, ms->super.swapped) != 0)
				continue;
			error = orphan_recover_one(mountp, (uint32_t)number, raw, &scan->inode);
			if (error != 0)
				return error;
		}
	}

	/* Completes all bounded per-inode reclamations before mount publication. */
	return 0;
}

/* Recovers journal-owned orphans after validation and quota rebuild on a private mount. */
static int
orphan_recover(
	struct mount *mountp)
{
	struct ufs_mount_state *ms;
	struct ufs_orphan_scan *scan;
	int error;

	/* Keeps readonly and nonjournal admission free of orphan-reclamation writes. */
	ms = state(mountp);
	if (!ms->writable || !ms->journal_enabled)
		return 0;
	if (mountp->m_root != NULL)
		return EBUSY;
	scan = kern_calloc(1, sizeof(*scan) + 2U * ms->super.bsize);
	if (scan == NULL)
		return ENOMEM;
	scan->bitmap = (uint8_t *)(scan + 1);
	scan->block = scan->bitmap + ms->super.bsize;

	/* Excludes namespace users while each checked owner takes its metadata locks. */
	mutex_lock(&ms->namespace_lock);
	error = orphan_scan_locked(mountp, scan);
	if (error != 0)
		ms->writable = 0;
	mutex_unlock(&ms->namespace_lock);
	kern_free(scan);

	/* Returns failure with persistent remaining ownership for the next mount attempt. */
	return error;
}
