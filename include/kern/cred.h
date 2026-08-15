/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_CRED_H
#define ZEDBSD_KERN_CRED_H

#include <sys/types.h>

#define KERN_NGROUPS_MAX 16U

struct inode;
struct ucred {
	unsigned usecount;
	uid_t ruid, euid, suid;
	gid_t rgid, egid, sgid;
	unsigned ngroups;
	gid_t groups[KERN_NGROUPS_MAX];
};

struct ucred *cred_alloc_root(void);
struct ucred *cred_copy(const struct ucred *);
void cred_ref(struct ucred *);
void cred_release(struct ucred *);
int cred_is_superuser(const struct ucred *);
int cred_in_group(const struct ucred *, gid_t);
const struct ucred *cred_current(void);
int vfs_access(const struct inode *, const struct ucred *, int);
int vfs_may_create(const struct inode *, const struct ucred *);
int vfs_may_remove(const struct inode *, const struct inode *,
		   const struct ucred *);
int vfs_may_rename(const struct inode *, const struct inode *,
		   const struct inode *, const struct inode *,
		   const struct ucred *);
int vfs_may_chown(const struct inode *, const struct ucred *, uid_t, gid_t);
int vfs_clear_setid_on_write(struct inode *, const struct ucred *);

#endif
