/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_KERN_SYSCTL_H
#define KERN_KERN_SYSCTL_H

#include <stddef.h>

void
sysctl_init(void);

int
kern_sysctl(
	const int *name,
	unsigned namelen,
	void *oldp,
	size_t *oldlenp,
	const void *newp,
	size_t newlen,
	int superuser);

#endif
