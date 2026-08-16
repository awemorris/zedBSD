/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_RESOURCE_H
#define ZEDBSD_KERN_RESOURCE_H

#include <zedbsd/system.h>

void kern_resource_snapshot(struct zedbsd_system_resources *);
int kern_resource_equal(const struct zedbsd_system_resources *,
	const struct zedbsd_system_resources *);

#endif
