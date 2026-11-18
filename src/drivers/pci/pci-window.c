/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The placement of a BAR the firmware left unassigned.
 *
 * The firmware of some machines (the Latitude 5330's, for its LPSS I2C
 * controllers) leaves a memory BAR at address 0 for the operating system
 * to place.  A BAR is placed inside a window the
 * host bridge's _CRS gives its bus, at the lowest address aligned to its
 * size that no busy range touches: the other functions' BARs, the bridges'
 * windows, the motherboard's reserved ranges, the configuration space and
 * the firmware's memory map.  A BAR that can take a 64-bit address tries
 * the windows above 4 GiB first, where no hidden device of the chipset
 * lives; a 32-bit BAR uses only the part of a window below 4 GiB.  The
 * windows below 1 MiB (the legacy VGA and option ROM ranges) are never
 * used.
 *
 * Nothing here touches hardware, so a host test compiles this file as
 * it is.

 */

#include <drivers/pci/pci-window.h>
#include <uapi/errno.h>

#include <stddef.h>

/* The first address above the legacy ranges, and the first one above 32 bits. */
#define WINDOW_LEGACY_END	UINT64_C(0x100000)
#define WINDOW_4GIB		UINT64_C(0x100000000)

/* The passes over the windows: the preferred kind first, then the other. */
#define WINDOW_PASSES		2U

static bool window_bounds(const struct drv_pci_range *window, bool wide, unsigned pass, uint64_t *first, uint64_t *last);
static int place_in_window(uint64_t first, uint64_t last, const struct drv_pci_range *busy, unsigned busy_count, uint64_t span, uint64_t *address);
static bool busy_conflict(const struct drv_pci_range *busy, unsigned busy_count, uint64_t first, uint64_t last, uint64_t *busy_last);
static bool align_up(uint64_t value, uint64_t alignment, uint64_t *result);

/*
 * Finds the address for one BAR of the given size.
 *
 * wide says the BAR can take a 64-bit address.  On success the address
 * and the index of the window it lies in are reported; EINVAL reports a
 * size that is not a power of two, and ENOSPC a BAR that fits in no
 * window.
 */
int
drv_pci_window_place(
	const struct drv_pci_range *windows,
	unsigned window_count,
	const struct drv_pci_range *busy,
	unsigned busy_count,
	uint64_t size,
	bool wide,
	uint64_t *address,
	unsigned *window)
{
	uint64_t span;
	uint64_t first;
	uint64_t last;
	uint64_t placed;
	unsigned pass;
	unsigned index;
	bool usable;
	int error;

	/* Refuses missing storage and a size a BAR cannot have. */
	if (windows == NULL || address == NULL || window == NULL)
		return EINVAL;
	if (busy == NULL && busy_count != 0)
		return EINVAL;
	if (size == 0 || (size & (size - 1U)) != 0)
		return EINVAL;

	/* A BAR smaller than a page still takes a page of its own. */
	span = size;
	if (span < DRV_PCI_WINDOW_MIN_SPAN)
		span = DRV_PCI_WINDOW_MIN_SPAN;

	/* Tries the preferred windows in their order, then the others. */
	for (pass = 0; pass < WINDOW_PASSES; pass++) {
		for (index = 0; index < window_count; index++) {
			/* Passes over a window this pass does not use. */
			usable = window_bounds(&windows[index], wide, pass, &first, &last);
			if (!usable)
				continue;

			/* Looks for a free place in the window. */
			error = place_in_window(first, last, busy, busy_count, span, &placed);
			if (error != 0)
				continue;

			/* Reports the place and the window that holds it. */
			*address = placed;
			*window = index;
			return 0;
		}
	}

	/* No window has room for the BAR. */
	return ENOSPC;
}

/* Tells whether one pass uses a window, and the addresses of it the BAR may take. */
static bool
window_bounds(
	const struct drv_pci_range *window,
	bool wide,
	unsigned pass,
	uint64_t *first,
	uint64_t *last)
{
	bool high;

	/* An empty window, or one that runs past the end of the address space, is none. */
	if (window->length == 0)
		return false;
	if (window->base > UINT64_MAX - (window->length - 1U))
		return false;
	*first = window->base;
	*last = window->base + (window->length - 1U);

	/* The legacy VGA and option ROM ranges are not for BARs. */
	if (*first < WINDOW_LEGACY_END)
		return false;

	/* A 64-bit BAR takes the windows above 4 GiB first, then the others. */
	high = *first >= WINDOW_4GIB;
	if (wide) {
		if (pass == 0 && !high)
			return false;
		if (pass != 0 && high)
			return false;

		/* Succeeded: the whole window may be used. */
		return true;
	}

	/* A 32-bit BAR takes, in the first pass only, the part of a window below 4 GiB. */
	if (pass != 0 || high)
		return false;
	if (*last >= WINDOW_4GIB)
		*last = WINDOW_4GIB - 1U;

	/* Succeeded: the part below 4 GiB may be used. */
	return true;
}

/* Finds the lowest place of the span in a window that no busy range touches. */
static int
place_in_window(
	uint64_t first,
	uint64_t last,
	const struct drv_pci_range *busy,
	unsigned busy_count,
	uint64_t span,
	uint64_t *address)
{
	uint64_t candidate;
	uint64_t busy_last;
	bool aligned;
	bool conflict;

	/* Starts at the first aligned address of the window. */
	aligned = align_up(first, span, &candidate);
	if (!aligned)
		return ENOSPC;

	/*
	 * Moves past each busy range the candidate touches.  The candidate
	 * only grows, past the end of the range it touched, so a range is
	 * moved past at most once and the loop ends.
	 */
	for (;;) {
		/* Gives up once the span no longer fits before the window's end. */
		if (candidate > last)
			return ENOSPC;
		if (span - 1U > last - candidate)
			return ENOSPC;

		/* Takes a candidate nothing decodes. */
		conflict = busy_conflict(busy, busy_count, candidate, candidate + (span - 1U), &busy_last);
		if (!conflict)
			break;

		/* Tries the first aligned address after the range in the way. */
		if (busy_last == UINT64_MAX)
			return ENOSPC;
		aligned = align_up(busy_last + 1U, span, &candidate);
		if (!aligned)
			return ENOSPC;
	}

	/* Succeeded: the span fits at the candidate. */
	*address = candidate;
	return 0;
}

/* Tells whether a busy range touches the addresses first to last, and where it ends. */
static bool
busy_conflict(
	const struct drv_pci_range *busy,
	unsigned busy_count,
	uint64_t first,
	uint64_t last,
	uint64_t *busy_last)
{
	uint64_t range_last;
	unsigned index;

	/* Looks at each busy range. */
	for (index = 0; index < busy_count; index++) {
		/* An empty range decodes nothing. */
		if (busy[index].length == 0)
			continue;

		/* A range that runs past the end of the address space ends at its end. */
		range_last = UINT64_MAX;
		if (busy[index].base <= UINT64_MAX - (busy[index].length - 1U))
			range_last = busy[index].base + (busy[index].length - 1U);

		/* A range wholly before or wholly after the addresses does not touch them. */
		if (range_last < first)
			continue;
		if (busy[index].base > last)
			continue;

		/* Reports the range in the way by its last address. */
		*busy_last = range_last;
		return true;
	}

	/* Succeeded: nothing touches the addresses. */
	return false;
}

/* Rounds a value up to a power-of-two alignment, refusing one that would wrap. */
static bool
align_up(
	uint64_t value,
	uint64_t alignment,
	uint64_t *result)
{
	/* A value within one alignment of the end cannot be rounded up. */
	if (value > UINT64_MAX - (alignment - 1U))
		return false;

	/* Succeeded: the rounded value. */
	*result = (value + (alignment - 1U)) & ~(alignment - 1U);
	return true;
}
