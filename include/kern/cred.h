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

int
vfs_clear_setid_on_content_change(
	struct inode *inode);

/*
 * The set-id bits are judged when the content changes: a write by its
 * writer's credential, a store through a shared mapping (which has none) as
 * an unprivileged one.  A write-back of the page cache clears them only for
 * the stores that no later mode change with set-id bits has accepted, so the
 * bits a privileged chmod set after a write survive its write-back.
 */
void
vfs_note_mapped_store(
	struct inode *inode);

int
vfs_clear_setid_on_writeback(
	struct inode *inode);

unsigned
vfs_setid_accept_begin(
	struct inode *inode,
	mode_t mode);

void
vfs_setid_accept_abort(
	struct inode *inode,
	unsigned pending);

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
