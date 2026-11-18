/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Uncached views of RAM for devices that do not snoop the CPU caches.
 *
 * Built only for architectures whose HAL provides hal_pmem_map_uncached();
 * the DMA layer refers to these calls weakly.
 */

#include <hal/hal.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

static int uncached_error(int status);

/*
 * Maps a physical run a second time, without caching.
 */
int
kern_pmem_map_uncached(
	const struct kern_pmem *run,
	void **mapped)
{
	int status;

	/* Refuses a missing run or destination. */
	if (run == NULL || mapped == NULL)
		return EINVAL;

	/* Asks the HAL for the view. */
	status = hal_pmem_map_uncached(run->paddr, run->size, mapped);
	if (status != HAL_OK)
		return uncached_error(status);

	/* Succeeded: the run is reachable uncached at mapped. */
	return 0;
}

/*
 * Removes an uncached view.
 */
int
kern_pmem_unmap_uncached(
	void *mapped,
	size_t size)
{
	int status;

	/* Asks the HAL to remove the view. */
	status = hal_pmem_unmap_uncached(mapped, size);
	if (status != HAL_OK)
		return uncached_error(status);

	/* Succeeded: the run is reachable through the direct map only. */
	return 0;
}

/* Reports an errno for one HAL status. */
static int
uncached_error(
	int status)
{
	/* Maps the statuses the uncached calls report. */
	switch (status) {
	case HAL_ERR_INVALID:
		return EINVAL;
	case HAL_ERR_NOMEM:
		return ENOMEM;
	case HAL_ERR_UNSUPPORTED:
		return ENOTSUP;
	default:
		return EIO;
	}
}
