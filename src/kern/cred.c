/*
 * POSIX process credentials
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include "kern/cred.h"
#include "kern/inode.h"
#include "kern/kmem.h"
#include "kern/process.h"
#include "kern/thread.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

struct ucred *
cred_alloc_root(void)
{
	struct ucred *cred = kern_calloc(1, sizeof(*cred));
	if (cred != NULL)
		cred->usecount = 1;
	return cred;
}

struct ucred *
cred_copy(const struct ucred *source)
{
	struct ucred *cred;
	if (source == NULL)
		return NULL;
	cred = kern_calloc(1, sizeof(*cred));
	if (cred != NULL) {
		memcpy(cred, source, sizeof(*cred));
		cred->usecount = 1;
	}
	return cred;
}

void cred_ref(struct ucred *cred) { if (cred != NULL) cred->usecount++; }
void cred_release(struct ucred *cred)
{
	if (cred != NULL && cred->usecount != 0 && --cred->usecount == 0)
		kern_free(cred);
}
int cred_is_superuser(const struct ucred *cred)
{ return cred != NULL && cred->euid == 0; }
int cred_in_group(const struct ucred *cred, gid_t group)
{
	unsigned i;
	if (cred == NULL)
		return 0;
	if (cred->egid == group)
		return 1;
	for (i = 0; i < cred->ngroups; i++)
		if (cred->groups[i] == group)
			return 1;
	return 0;
}

const struct ucred *
cred_current(void)
{
	struct thread *thread = thread_current();
	return thread != NULL && thread->proc != NULL ? thread->proc->cred : NULL;
}

int
vfs_access(const struct inode *inode, const struct ucred *cred, int requested)
{
	mode_t bits;
	unsigned shift;
	if (inode == NULL || cred == NULL || (requested & ~(R_OK|W_OK|X_OK)) != 0)
		return EINVAL;
	if (requested == F_OK)
		return 0;
	if (cred_is_superuser(cred)) {
		if ((requested & X_OK) != 0 && inode->i_type == INODE_REG &&
		    (inode->i_mode & 0111U) == 0)
			return EACCES;
		return 0;
	}
	shift = cred->euid == inode->i_uid ? 6U :
	    (cred_in_group(cred, inode->i_gid) ? 3U : 0U);
	bits = (inode->i_mode >> shift) & 7U;
	if (((requested & R_OK) != 0 && (bits & 4U) == 0) ||
	    ((requested & W_OK) != 0 && (bits & 2U) == 0) ||
	    ((requested & X_OK) != 0 && (bits & 1U) == 0))
		return EACCES;
	return 0;
}
