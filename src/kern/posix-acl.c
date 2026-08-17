/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/posix-acl.h"
#include "kern/cred.h"
#include "kern/inode.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

_Static_assert(sizeof(struct posix_acl_entry) == 8,
    "POSIX ACL entry must have a fixed wire layout");

static const struct posix_acl_entry *
acl_entry(const struct posix_acl *acl, uint16_t tag)
{
	uint32_t index;
	for (index = 0; index < acl->count; index++)
		if (acl->entries[index].tag == tag)
			return &acl->entries[index];
	return NULL;
}

int
posix_acl_validate(const struct posix_acl *acl)
{
	uint32_t index, prior_id = 0;
	uint16_t prior_tag = 0;
	unsigned user_obj = 0, group_obj = 0, mask = 0, other = 0;
	unsigned named = 0;
	if (acl == NULL || acl->version != POSIX_ACL_VERSION || acl->count < 3 ||
	    acl->count > POSIX_ACL_MAX_ENTRIES)
		return EINVAL;
	for (index = 0; index < acl->count; index++) {
		const struct posix_acl_entry *entry = &acl->entries[index];
		if (entry->permissions > 7U || entry->tag < POSIX_ACL_USER_OBJ ||
		    entry->tag > POSIX_ACL_OTHER || entry->tag < prior_tag)
			return EINVAL;
		if (entry->tag == POSIX_ACL_USER || entry->tag == POSIX_ACL_GROUP) {
			if (entry->id == POSIX_ACL_UNDEFINED_ID ||
			    (entry->tag == prior_tag && entry->id <= prior_id))
				return EINVAL;
			prior_id = entry->id;named++;
		} else if (entry->id != POSIX_ACL_UNDEFINED_ID)
			return EINVAL;
		switch (entry->tag) {
		case POSIX_ACL_USER_OBJ: user_obj++;break;
		case POSIX_ACL_GROUP_OBJ: group_obj++;break;
		case POSIX_ACL_MASK: mask++;break;
		case POSIX_ACL_OTHER: other++;break;
		default: break;
		}
		prior_tag = entry->tag;
	}
	if (user_obj != 1 || group_obj != 1 || other != 1 || mask > 1 ||
	    (named != 0 && mask != 1))
		return EINVAL;
	return 0;
}

static unsigned
requested_permissions(int requested)
{
	return ((requested & R_OK) != 0 ? 4U : 0U) |
	    ((requested & W_OK) != 0 ? 2U : 0U) |
	    ((requested & X_OK) != 0 ? 1U : 0U);
}

int
posix_acl_check_access(const struct posix_acl *acl, const struct inode *inode,
	const struct ucred *cred, int requested)
{
	const struct posix_acl_entry *entry, *mask;
	unsigned wanted, allowed = 0, group_match = 0;
	uint32_t index;
	int error = posix_acl_validate(acl);
	if (error != 0 || inode == NULL || cred == NULL)
		return error != 0 ? error : EINVAL;
	wanted = requested_permissions(requested);
	if (cred->euid == inode->i_uid) {
		entry = acl_entry(acl, POSIX_ACL_USER_OBJ);
		return (entry->permissions & wanted) == wanted ? 0 : EACCES;
	}
	mask = acl_entry(acl, POSIX_ACL_MASK);
	for (index = 0; index < acl->count; index++) {
		entry = &acl->entries[index];
		if (entry->tag == POSIX_ACL_USER && entry->id == cred->euid) {
			allowed = entry->permissions &
			    (mask != NULL ? mask->permissions : 7U);
			return (allowed & wanted) == wanted ? 0 : EACCES;
		}
	}
	if (cred_in_group(cred, inode->i_gid)) {
		entry = acl_entry(acl, POSIX_ACL_GROUP_OBJ);
		allowed |= entry->permissions;group_match = 1;
	}
	for (index = 0; index < acl->count; index++) {
		entry = &acl->entries[index];
		if (entry->tag == POSIX_ACL_GROUP && cred_in_group(cred, entry->id)) {
			allowed |= entry->permissions;group_match = 1;
		}
	}
	if (group_match) {
		allowed &= mask != NULL ? mask->permissions : 7U;
		return (allowed & wanted) == wanted ? 0 : EACCES;
	}
	entry = acl_entry(acl, POSIX_ACL_OTHER);
	return (entry->permissions & wanted) == wanted ? 0 : EACCES;
}

int
posix_acl_load(struct inode *inode, const char *name, struct posix_acl *acl)
{
	ssize_t size;
	size_t expected;
	if (inode == NULL || name == NULL || acl == NULL)
		return EINVAL;
	memset(acl, 0, sizeof(*acl));
	size = inode_getxattr(inode, name, acl, sizeof(*acl));
	if (size < 0)
		return (int)-size;
	if ((size_t)size < 8U)
		return EIO;
	expected = 8U + (size_t)acl->count * sizeof(acl->entries[0]);
	if (expected != (size_t)size || expected > sizeof(*acl))
		return EIO;
	return posix_acl_validate(acl) == 0 ? 0 : EIO;
}

int
posix_acl_store(struct inode *inode, const char *name,
	const struct posix_acl *acl)
{
	size_t size;
	int error = posix_acl_validate(acl);
	if (error != 0)
		return error;
	size = 8U + (size_t)acl->count * sizeof(acl->entries[0]);
	return inode_setxattr(inode, name, acl, size, 0);
}

int
posix_acl_chmod(struct inode *inode, mode_t mode)
{
	struct posix_acl acl;
	uint32_t index;
	int error = posix_acl_load(inode, POSIX_ACL_XATTR_ACCESS, &acl);
	if (error == ENODATA || error == EOPNOTSUPP)
		return 0;
	if (error != 0)
		return error;
	for (index = 0; index < acl.count; index++)
		switch (acl.entries[index].tag) {
		case POSIX_ACL_USER_OBJ:
			acl.entries[index].permissions = (uint16_t)((mode >> 6) & 7U);break;
		case POSIX_ACL_MASK:
			acl.entries[index].permissions = (uint16_t)((mode >> 3) & 7U);break;
		case POSIX_ACL_GROUP_OBJ:
			if (acl_entry(&acl, POSIX_ACL_MASK) == NULL)
				acl.entries[index].permissions =
				    (uint16_t)((mode >> 3) & 7U);
			break;
		case POSIX_ACL_OTHER:
			acl.entries[index].permissions = (uint16_t)(mode & 7U);break;
		default: break;
		}
	return posix_acl_store(inode, POSIX_ACL_XATTR_ACCESS, &acl);
}

int
posix_acl_inherit(struct inode *parent, struct inode *child, mode_t *mode)
{
	struct posix_acl acl, default_acl;
	const struct posix_acl_entry *mask;
	uint32_t index;
	int error;
	if (parent == NULL || child == NULL || mode == NULL)
		return EINVAL;
	error = posix_acl_load(parent, POSIX_ACL_XATTR_DEFAULT, &acl);
	if (error == ENODATA || error == EOPNOTSUPP)
		return 0;
	if (error != 0)
		return error;
	default_acl = acl;
	for (index = 0; index < acl.count; index++)
		switch (acl.entries[index].tag) {
		case POSIX_ACL_USER_OBJ:
			acl.entries[index].permissions &= (uint16_t)((*mode >> 6) & 7U);break;
		case POSIX_ACL_MASK:
		case POSIX_ACL_GROUP_OBJ:
			acl.entries[index].permissions &= (uint16_t)((*mode >> 3) & 7U);break;
		case POSIX_ACL_OTHER:
			acl.entries[index].permissions &= (uint16_t)(*mode & 7U);break;
		default: break;
		}
	error = posix_acl_store(child, POSIX_ACL_XATTR_ACCESS, &acl);
	if (error == 0 && child->i_type == INODE_DIR)
		error = posix_acl_store(child, POSIX_ACL_XATTR_DEFAULT, &default_acl);
	if (error != 0)
		return error;
	mask = acl_entry(&acl, POSIX_ACL_MASK);
	*mode = (*mode & ~(mode_t)0777U) |
	    ((mode_t)acl_entry(&acl, POSIX_ACL_USER_OBJ)->permissions << 6) |
	    ((mode_t)(mask != NULL ? mask->permissions :
	    acl_entry(&acl, POSIX_ACL_GROUP_OBJ)->permissions) << 3) |
	    acl_entry(&acl, POSIX_ACL_OTHER)->permissions;
	return 0;
}
