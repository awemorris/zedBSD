/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_KERN_PMEM_H
#define KERN_KERN_PMEM_H

#include <stddef.h>
#include <stdint.h>

/*
 * A physical address.
 *
 * It is wide enough for any physical address the platform hands to the
 * kernel, so a driver can hold one without naming a platform type.
 */
typedef uintptr_t kern_physaddr_t;

/*
 * One physical run owned by a kernel subsystem.
 *
 * The allocation reports a physical address only, and the release needs
 * the matching size, so callers which hold a run for a while keep both
 * together. Use kern_pmem_to_kernel() to address the run.
 */
struct kern_pmem {
	kern_physaddr_t paddr;
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

/*
 * Device mapping attributes for kern_device_map().
 *
 * The low byte names one cache policy. KERN_DEVICE_WRITECOMBINE lets the
 * CPU gather writes before they reach the device, for a linear aperture
 * such as a framebuffer; never use it for registers. KERN_DEVICE_READONLY
 * may be added to any policy for a view the caller only reads, such as a
 * firmware table or an option ROM.
 */
#define KERN_DEVICE_UNCACHED		0U
#define KERN_DEVICE_WRITETHROUGH	1U
#define KERN_DEVICE_WRITECOMBINE	2U
#define KERN_DEVICE_READONLY		0x100U

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
void *kern_pmem_to_kernel(kern_physaddr_t address);

/*
 * Map and unmap a device range in kernel space.
 *
 * Device memory is not direct-mapped. The kernel chooses the address
 * and reports it. attributes is one cache policy, KERN_DEVICE_UNCACHED,
 * KERN_DEVICE_WRITETHROUGH or KERN_DEVICE_WRITECOMBINE, optionally with
 * KERN_DEVICE_READONLY. Both report 0 on success. kern_device_map()
 * reports EINVAL for attributes it does not know and ENOTSUP for a policy
 * the platform cannot provide.
 */
int kern_device_map(uint64_t address, size_t size,
		    unsigned attributes, void **mapped);
int kern_device_unmap(void *mapped, size_t size);

/* Firmware mapping attributes for kern_firmware_map(). */
#define KERN_FIRMWARE_CACHED	0U
#define KERN_FIRMWARE_UNCACHED	1U

/*
 * Map a firmware range at the kernel address the firmware was told.
 *
 * Firmware that keeps running under the kernel, such as UEFI's runtime
 * services, was given the kernel addresses of its ranges before the
 * kernel started, so the caller names the address.  protection is the OR
 * of KERN_PROT_*, and may include KERN_PROT_EXEC for the firmware's code;
 * attributes is KERN_FIRMWARE_CACHED or KERN_FIRMWARE_UNCACHED.  The
 * range must not be RAM the kernel manages, and the mapping lasts for the
 * kernel's life.  A platform without a firmware window refuses the call.
 */
int kern_firmware_map(uint64_t address, void *mapped, size_t size,
		      unsigned protection, unsigned attributes);

/*
 * Map and unmap a physical run a second time, without caching.
 *
 * For memory shared with a device that does not snoop the CPU caches.  The
 * run must be page aligned; while the uncached view exists the run is not
 * touched through its direct-map address.  Only architectures that have such
 * devices provide these calls, so callers declare them weak and test them.
 */
int kern_pmem_map_uncached(const struct kern_pmem *run, void **mapped);
int kern_pmem_unmap_uncached(void *mapped, size_t size);

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
