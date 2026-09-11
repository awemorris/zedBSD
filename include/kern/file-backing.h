/*
 * Claimed regular-file backing capabilities.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef KERN_KERN_FILE_BACKING_H
#define KERN_KERN_FILE_BACKING_H

#include <kern/file.h>
#include <errno.h>

/*
 * Dispatches through the owning filesystem while the caller holds its claim.
 * The consumer validates complete logical coverage and owns collected ranges.
 */
static __inline int
file_backing_extents(
	struct file *file,
	file_extent_cb callback,
	void *context)
{
	struct mount *mountp;

	/* Requires a regular file and an extent consumer. */
	if (file == NULL || callback == NULL || file->f_inode == NULL)
		return EINVAL;
	if (file->f_inode->i_type != INODE_REG)
		return EINVAL;

	/* Unsupported filesystems cannot provide physical backing authority. */
	mountp = file->f_inode->i_mount;
	if (mountp == NULL || mountp->m_disk == NULL || mountp->m_type == NULL)
		return EOPNOTSUPP;
	if (mountp->m_type->file_extents == NULL)
		return EOPNOTSUPP;

	/* Preserves the provider's exact error and callback outcome. */
	return mountp->m_type->file_extents(file, callback, context);
}

/* Enumerates optional file-exclusive metadata under the same prepared claim. */
static __inline int
file_backing_metadata_extents(
	struct file *file,
	file_metadata_extent_cb callback,
	void *context)
{
	struct mount *mountp;

	if (file == NULL || callback == NULL || file->f_inode == NULL)
		return EINVAL;
	if (file->f_inode->i_type != INODE_REG)
		return EINVAL;
	mountp = file->f_inode->i_mount;
	if (mountp == NULL || mountp->m_disk == NULL || mountp->m_type == NULL)
		return EOPNOTSUPP;
	if (mountp->m_type->file_extents == NULL)
		return EOPNOTSUPP;
	if (mountp->m_type->file_metadata_extents == NULL)
		return 0;
	return mountp->m_type->file_metadata_extents(file, callback, context);
}

#endif
