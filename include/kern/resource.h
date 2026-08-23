/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_KERN_RESOURCE_H
#define ZEDBSD_KERN_RESOURCE_H

#include <zedbsd/system.h>

void
kern_resource_snapshot(
	struct system_resource_info *);

int
kern_resource_equal(
	const struct system_resource_info *,
	const struct system_resource_info *);

#endif
