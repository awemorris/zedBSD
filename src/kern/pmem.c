/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Physical memory and device mappings for drivers.
 *
 * Each call forwards to the HAL. RAM is reached through the direct map, so
 * kern_pmem_to_kernel() is a pure address translation; device memory is not
 * direct-mapped and needs kern_device_map().
 */

#include <errno.h>
#include <hal/hal.h>

#include "kern/clock.h"
#include "kern/platform.h"
#include "kern/pmem.h"

/*
 * Reports an errno for one HAL status.
 */
static int
pmem_error(
	int status)
{
	/* Maps the statuses the memory paths can report. */
	switch (status) {
	case HAL_OK:
		return 0;
	case HAL_ERR_INVALID:
		return EINVAL;
	case HAL_ERR_NOMEM:
		return ENOMEM;
	case HAL_ERR_BUSY:
		return EBUSY;
	case HAL_ERR_UNSUPPORTED:
		return ENOTSUP;
	default:
		return EIO;
	}
}

/*
 * Allocates one physical RAM run.
 */
int
kern_pmem_alloc(
	size_t size,
	size_t alignment,
	struct kern_pmem *run)
{
	int status;

	/* Requires a destination for the run. */
	if (run == NULL)
		return EINVAL;

	/* Records the requested size so the release can repeat it. */
	status = hal_pmem_alloc(size, alignment, &run->paddr);
	if (status != HAL_OK)
		return pmem_error(status);
	run->size = size;
	return 0;
}

/*
 * Allocates one physical RAM run a device can reach.
 *
 * max_address is the highest physical address the device can address, and
 * boundary, when not zero, is a power-of-two block the run must not cross.
 */
int
kern_pmem_alloc_limited(
	size_t size,
	size_t alignment,
	uint64_t max_address,
	size_t boundary,
	struct kern_pmem *run)
{
	int status;

	/* Requires a destination for the run. */
	if (run == NULL)
		return EINVAL;

	/* Searches only the range the device can reach. */
	status = hal_pmem_alloc_limited(size, alignment,
					(hal_physaddr_t)max_address, boundary,
					&run->paddr);
	if (status != HAL_OK)
		return pmem_error(status);
	run->size = size;
	return 0;
}

/*
 * Releases one physical RAM run.
 */
int
kern_pmem_free(
	struct kern_pmem *run)
{
	int status;

	/* Ignores an empty run so a failed allocation is safe to release. */
	if (run == NULL)
		return EINVAL;
	if (run->size == 0)
		return 0;

	/* Releases the run and empties the caller's record. */
	status = hal_pmem_free(&run->paddr, run->size);
	if (status != HAL_OK)
		return pmem_error(status);
	run->size = 0;
	return 0;
}

/*
 * Translates a physical RAM address to its kernel address.
 */
void *
kern_pmem_to_kernel(
	hal_physaddr_t address)
{
	/* RAM is direct-mapped; device memory has no kernel alias. */
	return hal_pmem_to_kernel(address);
}

/*
 * Maps one device range into kernel space.
 */
int
kern_device_map(
	uint64_t address,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	uint32_t flags;

	/* Every device mapping is readable and writable. */
	flags = HAL_SPACE_READ | HAL_SPACE_WRITE;

	/* Selects write-through when the caller asked for it. */
	if ((attributes & KERN_DEVICE_WRITETHROUGH) != 0)
		flags |= HAL_SPACE_WRITETHRU;
	else
		flags |= HAL_SPACE_NOCACHE;
	return pmem_error(hal_space_map_device((hal_physaddr_t)address, size,
					       flags, mapped));
}

/*
 * Removes one device mapping.
 */
int
kern_device_unmap(
	void *mapped,
	size_t size)
{
	/* Releases the kernel window the mapping occupied. */
	return pmem_error(hal_space_unmap_device(mapped, size));
}

/*
 * Reports the page size of one translation level.
 */
size_t
kern_page_size(
	int level)
{
	/* Level 1 is the smallest page the architecture supports. */
	return hal_space_get_page_size(level);
}

/*
 * Reports physical memory accounting.
 */
void
kern_memstat(
	struct kern_memstat *statistics)
{
	struct hal_memstat platform;

	/* Ignores an absent destination. */
	if (statistics == NULL)
		return;

	/* Copies the subset the kernel states portably. */
	hal_get_memstat(&platform);
	statistics->physical_total = platform.physical_total;
	statistics->physical_reserved = platform.physical_reserved;
	statistics->physical_allocated = platform.physical_allocated;
	statistics->physical_free = platform.physical_free;
	statistics->task_stack_bytes = platform.task_stack_bytes;
	statistics->task_count = platform.task_count;
	statistics->space_count = platform.space_count;
	statistics->page_table_count = platform.page_table_count;
}

/*
 * Looks up one boot handoff object by name.
 */
void *
kern_boot_handoff(
	const char *name)
{
	/* The platform keeps ownership of the object. */
	return hal_get_arch_handoff(name);
}

/*
 * Reads the monotonic counter and its frequency.
 */
bool
kern_rtc_read_counter(
	uint64_t *counter,
	uint64_t *frequency_hz)
{
	/* Only differences between samples are meaningful. */
	return hal_rtc_read_counter(counter, frequency_hz);
}
