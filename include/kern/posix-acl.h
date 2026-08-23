/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_POSIX_ACL_H
#define ZEDBSD_KERN_POSIX_ACL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define POSIX_ACL_XATTR_ACCESS		"system.posix_acl_access"
#define POSIX_ACL_XATTR_DEFAULT		"system.posix_acl_default"
#define POSIX_ACL_VERSION		1U
#define POSIX_ACL_MAX_ENTRIES		32U
#define POSIX_ACL_UNDEFINED_ID		((uint32_t)0xffffffffU)

enum posix_acl_tag {
	POSIX_ACL_USER_OBJ = 1,
	POSIX_ACL_USER = 2,
	POSIX_ACL_GROUP_OBJ = 3,
	POSIX_ACL_GROUP = 4,
	POSIX_ACL_MASK = 5,
	POSIX_ACL_OTHER = 6,
};

struct posix_acl_entry {
	uint16_t tag;
	uint16_t permissions;
	uint32_t id;
};

struct posix_acl {
	uint32_t version;
	uint32_t count;
	struct posix_acl_entry entries[POSIX_ACL_MAX_ENTRIES];
};

struct inode;
struct ucred;

int
posix_acl_validate(
	const struct posix_acl *);

int
posix_acl_check_access(
	const struct posix_acl *,
	const struct inode *,
	const struct ucred *,
	int);

int
posix_acl_load(
	struct inode *,
	const char *,
	struct posix_acl *);

int
posix_acl_store(
	struct inode *,
	const char *,
	const struct posix_acl *);

int
posix_acl_chmod(
	struct inode *,
	mode_t);

int
posix_acl_inherit(
	struct inode *,
	struct inode *,
	mode_t *);

#endif
