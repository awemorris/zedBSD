/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_IO_SCRATCH_H
#define ZEDBSD_KERN_IO_SCRATCH_H
#include <hal/hal.h>
#include <kern/pmem.h>

/* One contiguous physical region for CPU scratch. */
struct io_scratch {
	void *vaddr;
	size_t size;
	struct kern_pmem physical;
};
int io_scratch_alloc(size_t size, struct io_scratch *result);
int io_scratch_free(struct io_scratch *scratch);
#endif
