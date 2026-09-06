/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * POSIX access control lists.
 *
 * An ACL is stored as an extended attribute in its fixed wire layout.  The
 * access check follows the POSIX.1e algorithm: owner, named user, owning
 * and named groups filtered through the mask, then other.  chmod() and
 * inheritance keep the ACL consistent with the file mode.
 */

#include "kern/posix-acl.h"
#include "kern/cred.h"
#include "kern/inode.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

_Static_assert(sizeof(struct posix_acl_entry) == 8,
    "POSIX ACL entry must have a fixed wire layout");

static const struct posix_acl_entry *acl_entry(const struct posix_acl *acl, uint16_t tag);
static unsigned requested_permissions(int requested);

/*
 * Validates the structure of an ACL.
 *
 * Entries must be sorted by tag, named entries by identifier, and the ACL
 * must carry exactly one owner, owning-group, and other entry plus a mask
 * whenever it has named entries.
 */
int
posix_acl_validate(
	const struct posix_acl *acl)
{
	const struct posix_acl_entry *entry;
	uint32_t index;
	uint32_t prior_id;
	uint16_t prior_tag;
	unsigned user_obj;
	unsigned group_obj;
	unsigned mask;
	unsigned other;
	unsigned named;

	prior_id = 0;
	prior_tag = 0;
	user_obj = 0;
	group_obj = 0;
	mask = 0;
	other = 0;
	named = 0;

	/* Rejects a missing ACL, a foreign version, or an impossible size. */
	if (acl == NULL ||
	    acl->version != POSIX_ACL_VERSION ||
	    acl->count < 3 ||
	    acl->count > POSIX_ACL_MAX_ENTRIES)
		return EINVAL;

	/* Checks every entry against its predecessor and counts the kinds. */
	for (index = 0; index < acl->count; index++) {
		entry = &acl->entries[index];

		/* Rejects bad permissions, an unknown tag, or a tag out of order. */
		if (entry->permissions > 7U ||
		    entry->tag < POSIX_ACL_USER_OBJ ||
		    entry->tag > POSIX_ACL_OTHER ||
		    entry->tag < prior_tag)
			return EINVAL;

		/* Named entries need distinct ascending identifiers; others none. */
		if (entry->tag == POSIX_ACL_USER || entry->tag == POSIX_ACL_GROUP) {
			if (entry->id == POSIX_ACL_UNDEFINED_ID ||
			    (entry->tag == prior_tag && entry->id <= prior_id))
				return EINVAL;
			prior_id = entry->id;
			named++;
		} else if (entry->id != POSIX_ACL_UNDEFINED_ID) {
			return EINVAL;
		}

		/* Counts the singleton kinds. */
		switch (entry->tag) {
		case POSIX_ACL_USER_OBJ:
			user_obj++;
			break;
		case POSIX_ACL_GROUP_OBJ:
			group_obj++;
			break;
		case POSIX_ACL_MASK:
			mask++;
			break;
		case POSIX_ACL_OTHER:
			other++;
			break;
		default:
			break;
		}
		prior_tag = entry->tag;
	}

	/* Requires exactly one of each base entry and a mask with named entries. */
	if (user_obj != 1 ||
	    group_obj != 1 ||
	    other != 1 ||
	    mask > 1 ||
	    (named != 0 && mask != 1))
		return EINVAL;

	/* Reports a valid ACL. */
	return 0;
}

/*
 * Decides whether credentials may access an inode under its ACL.
 *
 * The first matching class decides: the owner, a named user, the union of
 * matching groups filtered through the mask, or other.
 */
int
posix_acl_check_access(
	const struct posix_acl *acl,
	const struct inode *inode,
	const struct ucred *cred,
	int requested)
{
	const struct posix_acl_entry *entry;
	const struct posix_acl_entry *mask;
	uint32_t index;
	unsigned wanted;
	unsigned allowed;
	unsigned group_match;
	int error;

	allowed = 0;
	group_match = 0;

	/* Rejects an invalid ACL or a missing inode or credential. */
	error = posix_acl_validate(acl);
	if (error != 0)
		return error;
	if (inode == NULL || cred == NULL)
		return EINVAL;

	wanted = requested_permissions(requested);

	/* The owner is judged by the owner entry alone. */
	if (cred->euid == inode->i_uid) {
		entry = acl_entry(acl, POSIX_ACL_USER_OBJ);
		if ((entry->permissions & wanted) != wanted)
			return EACCES;
		return 0;
	}

	/* A named user is judged by its entry filtered through the mask. */
	mask = acl_entry(acl, POSIX_ACL_MASK);
	for (index = 0; index < acl->count; index++) {
		entry = &acl->entries[index];
		if (entry->tag == POSIX_ACL_USER && entry->id == cred->euid) {
			allowed = entry->permissions;
			if (mask != NULL)
				allowed &= mask->permissions;
			if ((allowed & wanted) != wanted)
				return EACCES;
			return 0;
		}
	}

	/* Unions the owning group and every matching named group. */
	if (cred_in_group(cred, inode->i_gid)) {
		entry = acl_entry(acl, POSIX_ACL_GROUP_OBJ);
		allowed |= entry->permissions;
		group_match = 1;
	}
	for (index = 0; index < acl->count; index++) {
		entry = &acl->entries[index];
		if (entry->tag == POSIX_ACL_GROUP && cred_in_group(cred, entry->id)) {
			allowed |= entry->permissions;
			group_match = 1;
		}
	}

	/* A group member is judged by that union filtered through the mask. */
	if (group_match) {
		if (mask != NULL)
			allowed &= mask->permissions;
		if ((allowed & wanted) != wanted)
			return EACCES;
		return 0;
	}

	/* Everyone else is judged by the other entry. */
	entry = acl_entry(acl, POSIX_ACL_OTHER);
	if ((entry->permissions & wanted) != wanted)
		return EACCES;

	/* Reports permitted access. */
	return 0;
}

/*
 * Loads an ACL attribute of an inode.
 *
 * A malformed attribute is reported as EIO rather than as an invalid
 * argument, because the caller supplied nothing wrong.
 */
int
posix_acl_load(
	struct inode *inode,
	const char *name,
	struct posix_acl *acl)
{
	ssize_t size;
	size_t expected;
	int error;

	/* Rejects a missing inode, attribute name, or result. */
	if (inode == NULL || name == NULL || acl == NULL)
		return EINVAL;

	/* Reads the attribute into the fixed layout. */
	memset(acl, 0, sizeof(*acl));
	size = inode_getxattr(inode, name, acl, sizeof(*acl));
	if (size < 0)
		return (int)-size;

	/* Rejects an attribute whose size disagrees with its entry count. */
	if ((size_t)size < 8U)
		return EIO;
	expected = 8U + (size_t)acl->count * sizeof(acl->entries[0]);
	if (expected != (size_t)size || expected > sizeof(*acl))
		return EIO;

	/* Rejects a stored ACL that fails validation. */
	error = posix_acl_validate(acl);
	if (error != 0)
		return EIO;

	/* Reports the loaded ACL. */
	return 0;
}

/*
 * Stores an ACL as an attribute of an inode.
 */
int
posix_acl_store(
	struct inode *inode,
	const char *name,
	const struct posix_acl *acl)
{
	size_t size;
	int error;

	/* Refuses to store an invalid ACL. */
	error = posix_acl_validate(acl);
	if (error != 0)
		return error;

	/* Writes exactly the used part of the layout. */
	size = 8U + (size_t)acl->count * sizeof(acl->entries[0]);
	error = inode_setxattr(inode, name, acl, size, 0);

	/* Reports the attribute write result. */
	return error;
}

/*
 * Updates the access ACL of an inode after a mode change.
 *
 * The owner, mask (or owning group without a mask), and other entries
 * take the corresponding mode bits.  An inode without an ACL is left alone.
 */
int
posix_acl_chmod(
	struct inode *inode,
	mode_t mode)
{
	struct posix_acl acl;
	uint32_t index;
	int error;

	/* Loads the access ACL; an absent one needs no update. */
	error = posix_acl_load(inode, POSIX_ACL_XATTR_ACCESS, &acl);
	if (error == ENODATA || error == EOPNOTSUPP)
		return 0;
	if (error != 0)
		return error;

	/* Copies the mode classes into the entries they govern. */
	for (index = 0; index < acl.count; index++) {
		switch (acl.entries[index].tag) {
		case POSIX_ACL_USER_OBJ:
			acl.entries[index].permissions =
			    (uint16_t)((mode >> 6) & 7U);
			break;
		case POSIX_ACL_MASK:
			acl.entries[index].permissions =
			    (uint16_t)((mode >> 3) & 7U);
			break;
		case POSIX_ACL_GROUP_OBJ:
			if (acl_entry(&acl, POSIX_ACL_MASK) == NULL)
				acl.entries[index].permissions =
				    (uint16_t)((mode >> 3) & 7U);
			break;
		case POSIX_ACL_OTHER:
			acl.entries[index].permissions = (uint16_t)(mode & 7U);
			break;
		default:
			break;
		}
	}

	/* Stores the updated ACL. */
	error = posix_acl_store(inode, POSIX_ACL_XATTR_ACCESS, &acl);

	/* Reports the store result. */
	return error;
}

/*
 * Applies a parent's default ACL to a new child.
 *
 * The child's access ACL is the default ACL masked by the creation mode; a
 * new directory also inherits the default ACL itself.  The effective mode is
 * reported back through mode.
 */
int
posix_acl_inherit(
	struct inode *parent,
	struct inode *child,
	mode_t *mode)
{
	struct posix_acl acl;
	struct posix_acl default_acl;
	const struct posix_acl_entry *mask;
	uint32_t index;
	mode_t owner_bits;
	mode_t group_bits;
	mode_t other_bits;
	int error;

	/* Rejects a missing parent, child, or mode. */
	if (parent == NULL || child == NULL || mode == NULL)
		return EINVAL;

	/* Loads the parent's default ACL; an absent one means no inheritance. */
	error = posix_acl_load(parent, POSIX_ACL_XATTR_DEFAULT, &acl);
	if (error == ENODATA || error == EOPNOTSUPP)
		return 0;
	if (error != 0)
		return error;
	default_acl = acl;

	/* Masks the inherited entries with the creation mode. */
	for (index = 0; index < acl.count; index++) {
		switch (acl.entries[index].tag) {
		case POSIX_ACL_USER_OBJ:
			acl.entries[index].permissions &=
			    (uint16_t)((*mode >> 6) & 7U);
			break;
		case POSIX_ACL_MASK:
		case POSIX_ACL_GROUP_OBJ:
			acl.entries[index].permissions &=
			    (uint16_t)((*mode >> 3) & 7U);
			break;
		case POSIX_ACL_OTHER:
			acl.entries[index].permissions &=
			    (uint16_t)(*mode & 7U);
			break;
		default:
			break;
		}
	}

	/* Stores the access ACL, and the default ACL on a directory. */
	error = posix_acl_store(child, POSIX_ACL_XATTR_ACCESS, &acl);
	if (error == 0 && child->i_type == INODE_DIR)
		error = posix_acl_store(child, POSIX_ACL_XATTR_DEFAULT, &default_acl);
	if (error != 0)
		return error;

	/* Derives the effective mode from the stored access ACL. */
	mask = acl_entry(&acl, POSIX_ACL_MASK);
	owner_bits = acl_entry(&acl, POSIX_ACL_USER_OBJ)->permissions;
	if (mask != NULL)
		group_bits = mask->permissions;
	else
		group_bits = acl_entry(&acl, POSIX_ACL_GROUP_OBJ)->permissions;
	other_bits = acl_entry(&acl, POSIX_ACL_OTHER)->permissions;
	*mode = (*mode & ~(mode_t)0777U) |
	    (owner_bits << 6) |
	    (group_bits << 3) |
	    other_bits;

	/* Reports the inherited ACL. */
	return 0;
}

/* Finds the first entry with a tag. */
static const struct posix_acl_entry *
acl_entry(
	const struct posix_acl *acl,
	uint16_t tag)
{
	uint32_t index;

	/* Searches the entries in order. */
	for (index = 0; index < acl->count; index++) {
		if (acl->entries[index].tag == tag)
			return &acl->entries[index];
	}

	/* Reports a missing entry. */
	return NULL;
}

/* Converts access() request bits to ACL permission bits. */
static unsigned
requested_permissions(
	int requested)
{
	unsigned permissions;

	/* Maps each request bit onto the rwx bit it names. */
	permissions = 0;
	if ((requested & R_OK) != 0)
		permissions |= 4U;
	if ((requested & W_OK) != 0)
		permissions |= 2U;
	if ((requested & X_OK) != 0)
		permissions |= 1U;

	/* Reports the permission bits. */
	return permissions;
}
