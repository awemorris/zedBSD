/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_CRED_H
#define ZEDBSD_KERN_CRED_H

#include <kern/atomic.h>
#include <sys/types.h>

#define KERN_NGROUPS_MAX	16U

struct inode;

struct process;

struct ucred {
	refcount_t refs;
	uid_t ruid, euid, suid;
	gid_t rgid, egid, sgid;
	unsigned ngroups;
	gid_t groups[KERN_NGROUPS_MAX];
};

struct ucred *
cred_alloc_root(void);

struct ucred *
cred_copy(
	const struct ucred *);

void
cred_ref(
	struct ucred *);

void
cred_release(
	struct ucred *);

int
cred_is_superuser(
	const struct ucred *);

int
cred_in_group(
	const struct ucred *,
	gid_t);

const struct ucred *
cred_current(void);

struct ucred *
cred_current_ref(void);

struct ucred *
cred_process_ref(
	struct process *);

int
vfs_access(
	const struct inode *,
	const struct ucred *,
	int);

int
vfs_may_create(
	const struct inode *,
	const struct ucred *);

int
vfs_may_remove(
	const struct inode *,
	const struct inode *,
	const struct ucred *);

int
vfs_may_rename(
	const struct inode *,
	const struct inode *,
	const struct inode *,
	const struct inode *,
	const struct ucred *);

int
vfs_may_chown(
	const struct inode *,
	const struct ucred *,
	uid_t,
	gid_t);

int
vfs_clear_setid_on_write(
	struct inode *,
	const struct ucred *);

/*
 * Dirty MAP_SHARED pages have no meaningful writer credential.  Clearing
 *
 * privilege bits before their first backend writeback closes the interval in
 *
 * which exec could observe changed contents with stale set-id metadata.
 */
int
vfs_clear_setid_on_content_change(
	struct inode *);

ssize_t
vfs_getxattr(
	struct inode *,
	const struct ucred *,
	const char *,
	void *,
	size_t);

int
vfs_setxattr(
	struct inode *,
	const struct ucred *,
	const char *,
	const void *,
	size_t,
	unsigned);

ssize_t
vfs_listxattr(
	struct inode *,
	const struct ucred *,
	char *,
	size_t);

int
vfs_removexattr(
	struct inode *,
	const struct ucred *,
	const char *);

#endif
