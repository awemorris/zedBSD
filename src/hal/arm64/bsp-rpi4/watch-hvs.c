/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 4's compositor (HVS) for the stall watch: the planes of
 * the display list it scans out on its first channel, and a list of one
 * plane alone that it is pointed at when a stop lasts long enough.
 */

#include <hal/hal.h>
#include "../bsp.h"

/* The BCM2711 compositor (HVS): its registers and display-list memory. */
#define HVS_BASE 0xfe400000U
#define HVS_WINDOW 0x8000U

/* Its display-list register, its display-list memory and its size in words. */
#define HVS_DISPLIST0 0x20U
#define HVS_DLIST 0x4000U
#define HVS_DLIST_WORDS 2048U

/* Where the watch's own one-plane list goes: above every list the display driver uses. */
#define HVS_OWN_LIST 1984U

/* A display-list control word: end of list, valid, size, unscaled, and the pixel format. */
#define HVS_CTL_END 0x80000000U
#define HVS_CTL_VALID 0x40000000U
#define HVS_CTL_SIZE_SHIFT 24U
#define HVS_CTL_SIZE_MASK 0x3fU
#define HVS_CTL_UNITY 0x00008000U
#define HVS_CTL_FORMAT_MASK 0x0fU
#define HVS_CTL_FORMAT_RGBA8888 7U

/* An unscaled plane is eight words; the most list entries looked at. */
#define HVS_PLANE_WORDS 8U
#define HVS_ENTRIES_MAX 16U

/*
 * The compositor's registers, mapped uncached, or NULL when the window
 * could not be mapped.  Mapped once at boot by arm64_bsp_watch_init();
 * read only to find and show planes.
 */
static volatile uint32_t *hvs;

/*
 * Maps the compositor's registers and display-list memory, uncached, for
 * the stall watch's report.
 */
void
arm64_bsp_watch_init(
	void)
{
	void *window;
	int error;

	/* A window the table cannot hold leaves the report on the framebuffer. */
	window = NULL;
	error = hal_space_map_device(HVS_BASE,
				     HVS_WINDOW,
				     HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_NOCACHE | HAL_SPACE_DEVICE,
				     &window);
	if (error != HAL_OK)
		return;

	/* Succeeded: the compositor's registers. */
	hvs = (volatile uint32_t *)window;
}

/*
 * Reports the unscaled 32-bit planes of the display list the compositor
 * shows on its first channel.
 *
 * The list is walked by each entry's own size; an entry that does not
 * look like a list entry ends the walk.
 */
unsigned
arm64_bsp_watch_planes(
	struct arm64_watch_plane *planes,
	unsigned capacity)
{
	volatile uint32_t *list;
	uint32_t index;
	uint32_t control;
	uint32_t size;
	uint32_t pointer;
	uint32_t extent;
	unsigned count;
	unsigned entries;

	/* Without the compositor's window there is nothing to report. */
	if (hvs == NULL)
		return 0;

	/* The list the hardware follows starts at the word DISPLIST0 names. */
	list = (volatile uint32_t *)((volatile uint8_t *)hvs + HVS_DLIST);
	index = hvs[HVS_DISPLIST0 / 4U];
	count = 0;

	/* Walks the entries until the end marker, a bad entry, or enough planes. */
	for (entries = 0; entries < HVS_ENTRIES_MAX && count < capacity; entries++) {
		/* An index past the list memory is not a list. */
		if (index >= HVS_DLIST_WORDS)
			break;
		control = list[index];
		if ((control & HVS_CTL_END) != 0)
			break;
		if ((control & HVS_CTL_VALID) == 0)
			break;

		/* An entry's size covers at least its control word, and stays in the memory. */
		size = (control >> HVS_CTL_SIZE_SHIFT) & HVS_CTL_SIZE_MASK;
		if (size == 0 || index + size > HVS_DLIST_WORDS)
			break;

		/* Only an unscaled 32-bit plane has its image where the watch can draw. */
		if (size == HVS_PLANE_WORDS &&
		    (control & HVS_CTL_UNITY) != 0 &&
		    (control & HVS_CTL_FORMAT_MASK) == HVS_CTL_FORMAT_RGBA8888) {
			pointer = list[index + 5U];
			extent = list[index + 3U];
			planes[count].handle = index;
			planes[count].physical = (uint64_t)(pointer & 0x3fffffffU);
			planes[count].pitch = list[index + 7U] & 0xffffU;
			planes[count].width = extent & 0xffffU;
			planes[count].height = extent >> 16;
			planes[count].x = list[index + 1U] & 0x3fffU;
			planes[count].y = (list[index + 1U] >> 16) & 0x3fffU;
			count++;
		}

		/* The next entry follows this one. */
		index += size;
	}

	/* Succeeded: the planes found. */
	return count;
}

/*
 * Points the compositor at a list of one plane alone.
 *
 * The list goes into display-list memory above every list the display
 * driver writes, with fresh context words as the driver writes them.  The
 * compositor takes it at its next frame.  A display driver that still
 * runs points it back at its own list with its next flip.
 */
void
arm64_bsp_watch_show_alone(
	const struct arm64_watch_plane *plane)
{
	volatile uint32_t *list;
	unsigned index;

	/* Without the compositor's window, or for an entry past its memory, nothing changes. */
	if (hvs == NULL)
		return;
	if (plane->handle + HVS_PLANE_WORDS > HVS_DLIST_WORDS)
		return;

	/* Copies the plane's words, with fresh context words, then the end marker. */
	list = (volatile uint32_t *)((volatile uint8_t *)hvs + HVS_DLIST);
	for (index = 0; index < HVS_PLANE_WORDS; index++)
		list[HVS_OWN_LIST + index] = list[plane->handle + index];
	list[HVS_OWN_LIST + 4U] = 0xc0c0c0c0U;
	list[HVS_OWN_LIST + 6U] = 0xc0c0c0c0U;
	list[HVS_OWN_LIST + HVS_PLANE_WORDS] = HVS_CTL_END;

	/* The complete list precedes the pointer the compositor follows. */
	__asm__ volatile("dsb sy" ::: "memory");
	hvs[HVS_DISPLIST0 / 4U] = HVS_OWN_LIST;
	__asm__ volatile("dsb sy" ::: "memory");
}
