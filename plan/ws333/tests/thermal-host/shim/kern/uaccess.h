/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* The host test's stand-in for the copies to and from a process: the test's own memory, or a fault. */

#ifndef KERN_KERN_UACCESS_H
#define KERN_KERN_UACCESS_H

#include <stddef.h>
#include <stdint.h>

int
copyin(
	uintptr_t source,
	void *destination,
	size_t size);

int
copyout(
	const void *source,
	uintptr_t destination,
	size_t size);

#endif
