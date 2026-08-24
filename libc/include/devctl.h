/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_DEVCTL_H
#define ZEDBSD_DEVCTL_H

#include <sys/types.h>

int
posix_devctl(
	int descriptor,
	int command,
	void *restrict data,
	size_t size,
	int *restrict information);

#endif
