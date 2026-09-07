/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_IO_SCRATCH_H
#define ZEDBSD_KERN_IO_SCRATCH_H
#include <hal/hal.h>

/* CPU scratch only. A vmap owner never masquerades as contiguous hal_pmem. */
struct io_scratch {
	void *vaddr;
	size_t size;
	struct hal_pmem physical;
	struct hal_vmap *mapping;
};
int io_scratch_alloc(size_t size, int allow_vmap, struct io_scratch *result);
int io_scratch_free(struct io_scratch *scratch);
#endif
