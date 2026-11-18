/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_KERN_CRED_H
#define KERN_KERN_CRED_H

#include <kern/atomic.h>
#include <uapi/types.h>

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
	const struct ucred *source);

void
cred_ref(
	struct ucred *cred);

void
cred_release(
	struct ucred *cred);

int
cred_is_superuser(
	const struct ucred *cred);

int
cred_in_group(
	const struct ucred *cred,
	gid_t group);

/*
 * Tells whether two credentials differ in a user or group identity (the
 * real, effective and saved ones; the supplementary groups do not count).
 */
int
cred_ids_differ(
	const struct ucred *left,
	const struct ucred *right);

/*
 * Tells whether a credential may trace a process of another: the superuser
 * may trace any; anyone else only a process whose real, effective and
 * saved user and group identities are all the tracer's real ones (and its
 * effective ones), and which has not changed its identity (set_id).
 */
int
cred_may_trace(
	const struct ucred *tracer,
	const struct ucred *target,
	unsigned target_set_id);

const struct ucred *
cred_current(void);

struct ucred *
cred_current_ref(void);

struct ucred *
cred_process_ref(
	struct process *process);

int
vfs_access(
	const struct inode *inode,
	const struct ucred *cred,
	int requested);

int
vfs_may_create(
	const struct inode *parent,
	const struct ucred *cred);

int
vfs_may_remove(
	const struct inode *parent,
	const struct inode *victim,
	const struct ucred *cred);

int
vfs_may_rename(
	const struct inode *old_parent,
	const struct inode *source,
	const struct inode *new_parent,
	const struct inode *target,
	const struct ucred *cred);

int
vfs_may_chown(
	const struct inode *inode,
	const struct ucred *cred,
	uid_t uid,
	gid_t gid);

int
vfs_clear_setid_on_write(
	struct inode *inode,
	const struct ucred *cred);

/*
 * Dirty MAP_SHARED pages have no meaningful writer credential.  Clearing
 *
 * privilege bits before their first backend writeback closes the interval in
 *
 * which exec could observe changed contents with stale set-id metadata.
 */
int
vfs_clear_setid_on_content_change(
	struct inode *inode);

ssize_t
vfs_getxattr(
	struct inode *inode,
	const struct ucred *cred,
	const char *name,
	void *value,
	size_t size);

int
vfs_setxattr(
	struct inode *inode,
	const struct ucred *cred,
	const char *name,
	const void *value,
	size_t size,
	unsigned flags);

ssize_t
vfs_listxattr(
	struct inode *inode,
	const struct ucred *cred,
	char *list,
	size_t size);

int
vfs_removexattr(
	struct inode *inode,
	const struct ucred *cred,
	const char *name);

#endif
