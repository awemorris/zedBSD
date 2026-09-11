/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_PMEM_H
#define ZEDBSD_KERN_PMEM_H

#include <hal/hal.h>

/*
 * One physical run owned by a kernel subsystem.
 *
 * hal_pmem_alloc() returns a physical address only, and hal_pmem_free()
 * needs the matching size, so callers which hold a run for a while keep
 * both together. Use hal_pmem_to_kernel() to address the run.
 */
struct kern_pmem {
	hal_physaddr_t paddr;
	size_t size;
};

#endif
