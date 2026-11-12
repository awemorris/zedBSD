/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* The host test's stand-in for the kernel's credentials: whether the caller is root, which the test sets. */

#ifndef KERN_KERN_CRED_H
#define KERN_KERN_CRED_H

/* A caller's credentials as the test models them. */
struct ucred {
	int superuser;
};

struct ucred *
cred_current_ref(void);

int
cred_is_superuser(
	const struct ucred *credential);

void
cred_release(
	struct ucred *credential);

#endif
