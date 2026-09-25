/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_DEVCTL_H
#define LIBC_DEVCTL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

int
posix_devctl(
	int descriptor,
	int command,
	void *__restrict data,
	size_t size,
	int *__restrict information);

#ifdef __cplusplus
}
#endif

#endif
