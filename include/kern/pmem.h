/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_KERN_PMEM_H
#define KERN_KERN_PMEM_H

#include <hal/hal.h>
#include <stdint.h>

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

/*
 * Page protection flags.
 *
 * These name what a mapping permits, and match the values the
 * address-space layer records in a region.
 */
#define KERN_PROT_NONE		0U
#define KERN_PROT_READ		1U
#define KERN_PROT_WRITE		2U
#define KERN_PROT_EXEC		4U

/* Device mapping attributes for kern_device_map(). */
#define KERN_DEVICE_UNCACHED	0U
#define KERN_DEVICE_WRITETHROUGH	1U

/*
 * Allocate and release physical RAM.
 *
 * The run records the size it was asked for, because the release
 * needs it. kern_pmem_alloc_limited() restricts the result to what a
 * device can reach: max_address is the highest address it can name,
 * and boundary, when not zero, is a power-of-two block the run must
 * not cross. All report 0 on success.
 */
int kern_pmem_alloc(size_t size, size_t alignment,
		    struct kern_pmem *run);
int kern_pmem_alloc_limited(size_t size, size_t alignment,
			    uint64_t max_address, size_t boundary,
			    struct kern_pmem *run);
int kern_pmem_free(struct kern_pmem *run);

/*
 * Translate a physical RAM address to its kernel address.
 *
 * RAM is direct-mapped, so this never fails for managed RAM and
 * reports NULL for anything else.
 */
void *kern_pmem_to_kernel(hal_physaddr_t address);

/*
 * Map and unmap a device range in kernel space.
 *
 * Device memory is not direct-mapped. The kernel chooses the address
 * and reports it. attributes is KERN_DEVICE_UNCACHED or
 * KERN_DEVICE_WRITETHROUGH.
 */
int kern_device_map(uint64_t address, size_t size,
		    unsigned attributes, void **mapped);
int kern_device_unmap(void *mapped, size_t size);

/* Report the page size of one translation level; 1 is the smallest. */
size_t kern_page_size(int level);

/*
 * Report physical memory accounting.
 *
 * The record is the platform's own, so its fields follow the port.
 */
/*
 * Physical memory accounting.
 *
 * The kernel reports the totals it can state portably. A port may
 * observe more, and anything it cannot observe stays zero.
 */
struct kern_memstat {
	size_t physical_total;
	size_t physical_reserved;
	size_t physical_allocated;
	size_t physical_free;

	size_t task_stack_bytes;
	uint32_t task_count;
	uint32_t space_count;
	uint32_t page_table_count;
};

void kern_memstat(struct kern_memstat *statistics);

#endif
