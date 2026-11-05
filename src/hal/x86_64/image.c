/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 kernel image geometry: where it was linked and where the loader
 * put it.  Every image-to-physical conversion in the HAL goes through the
 * geometry established here, so a relocated image is handled in one place.
 */

#include <hal/hal.h>
#include "bsp.h"
#include "image.h"
#include "bsp-pcat/handoff-validation.h"
#include "bootloader/include/amd64-handoff.h"
#include "bootloader/include/amd64-kernel-image.h"

extern char __kernel_virt_start[];
extern char __kernel_virt_end[];
extern char __kernel_phys_start[];
extern char __kernel_phys_end[];
extern char __kernel_text_phys_start[];
extern char __kernel_text_phys_end[];
extern char __kernel_rodata_phys_start[];
extern char __kernel_rodata_phys_end[];

static struct amd64_kernel_image image;
static int image_ready;

static void report_low_memory_map(void);

/*
 * Establishes the image geometry from the linker symbols and the loader's
 * reported placement, validating that the placement can actually be used.
 */
void
prekern_amd64_image_init(
	void)
{
	uint64_t start;
	uint64_t end;
	uint64_t delta;
	uint64_t cursor;
	uint64_t base;
	uint64_t size;
	uint32_t type;
	uint32_t index;
	int known;

	/* The linked extent is fixed by platform/amd64/vmunix.ld. */
	image.virt_start = (uintptr_t)__kernel_virt_start;
	image.virt_end = (uintptr_t)__kernel_virt_end;
	image.link_phys_start = (uintptr_t)__kernel_phys_start;
	image.link_phys_end = (uintptr_t)__kernel_phys_end;
	if (image.link_phys_start != AMD64_KERNEL_LINK_PHYS_START ||
	    image.virt_start != AMD64_KERNEL_LINK_VIRT_BASE + AMD64_KERNEL_LINK_PHYS_START ||
	    image.link_phys_end - image.link_phys_start != image.virt_end - image.virt_start ||
	    image.link_phys_end - image.link_phys_start > AMD64_KERNEL_MAX_BYTES)
		HAL_FATAL("amd64 kernel link layout disagrees with amd64-kernel-image.h");

	/* A loader that reports no placement loaded the image where it was linked. */
	known = bsp_kernel_placement(&start, &end);
	if (!known) {
		start = image.link_phys_start;
		end = image.link_phys_end;
	}
	hal_printf("A64 KERNEL link=%llx-%llx load=%llx-%llx (%s)\n",
	    (unsigned long long)image.link_phys_start,
	    (unsigned long long)image.link_phys_end,
	    (unsigned long long)start, (unsigned long long)end,
	    !known ? "loader reported none" :
	    start == image.link_phys_start ? "as linked" : "relocated by the loader");

	/* Explains a rejected placement before stopping. */
	if (!zbl6_kernel_placement_valid(start, end)) {
		hal_printf("A64 KERNEL placement rejected: need %llx-aligned start >= %llx, "
		    "end <= %llx, size <= %llx\n",
		    (unsigned long long)AMD64_KERNEL_PHYS_ALIGN,
		    (unsigned long long)AMD64_KERNEL_LINK_PHYS_START,
		    (unsigned long long)AMD64_KERNEL_PHYS_LIMIT,
		    (unsigned long long)AMD64_KERNEL_MAX_BYTES);
		HAL_FATAL("amd64 kernel placement invalid");
	}
	if (end - start != image.link_phys_end - image.link_phys_start) {
		hal_printf("A64 KERNEL placement size %llx differs from the linked size %llx\n",
		    (unsigned long long)(end - start),
		    (unsigned long long)(image.link_phys_end - image.link_phys_start));
		HAL_FATAL("amd64 kernel placement size mismatch");
	}

	/* Rebases the load extents; text/rodata keep their linked offsets. */
	delta = start - image.link_phys_start;
	image.phys_start = start;
	image.phys_end = end;
	image.text_phys_start = (uintptr_t)__kernel_text_phys_start + delta;
	image.text_phys_end = (uintptr_t)__kernel_text_phys_end + delta;
	image.rodata_phys_start = (uintptr_t)__kernel_rodata_phys_start + delta;
	image.rodata_phys_end = (uintptr_t)__kernel_rodata_phys_end + delta;
	image.relocated = delta != 0;
	/*
	 * The loader redirects the linked range in whole 2 MiB bootstrap slots,
	 * so the slot tail past the image end also shows relocated memory: the
	 * shadow is the slot-rounded linked range, not just the image size.
	 */
	image.shadow_start = image.relocated ?
	    (image.link_phys_start & ~(AMD64_KERNEL_PHYS_ALIGN - 1U)) : 0;
	image.shadow_end = image.relocated ?
	    ((image.link_phys_end + AMD64_KERNEL_PHYS_ALIGN - 1U) &
	    ~(AMD64_KERNEL_PHYS_ALIGN - 1U)) : 0;

	/*
	 * The direct map aliases only reported RAM and the W^X invariants look
	 * the image up there, so the placement must lie inside usable or
	 * boot-reclaimable ranges.  The ranges are validated ascending.
	 */
	if (bsp_memory_source() != 0) {
		cursor = start;
		for (index = 0; index < bsp_mem_range_count() && cursor < end; index++) {
			if (!bsp_mem_range(index, &base, &size, &type))
				break;
			if (type != ZBL6_MEMORY_USABLE && type != ZBL6_MEMORY_BOOT_RECLAIM)
				continue;
			if (base <= cursor && cursor < base + size)
				cursor = base + size;
		}
		if (cursor < end) {
			hal_printf("A64 KERNEL %llx-%llx is not inside reported RAM (gap at %llx)\n",
			    (unsigned long long)start, (unsigned long long)end,
			    (unsigned long long)cursor);
			report_low_memory_map();
			HAL_FATAL("amd64 kernel image lies outside reported RAM");
		}
	} else if (end > bsp_mem_probe()) {
		HAL_FATAL("too little amd64 memory for the kernel image");
	}
	if (image.relocated)
		hal_printf("A64 KERNEL shadow %llx-%llx reserved (linked slots behind the relocated image)\n",
		    (unsigned long long)image.shadow_start,
		    (unsigned long long)image.shadow_end);
	image_ready = 1;
}

/*
 * Returns the established geometry.
 */
const struct amd64_kernel_image *
amd64_kernel_image(
	void)
{
	/* Using the geometry before it exists would silently assume identity. */
	if (!image_ready)
		HAL_FATAL("amd64 kernel image geometry used before initialization");
	return &image;
}

/*
 * Reports whether a physical page belongs to the image or to its shadow.
 */
int
amd64_kernel_image_owns(
	uint64_t physical)
{
	const struct amd64_kernel_image *img = amd64_kernel_image();

	return (physical >= img->phys_start && physical < img->phys_end) ||
	    (img->relocated && physical >= img->shadow_start &&
	    physical < img->shadow_end);
}

/* Prints the firmware ranges below the bootstrap window for diagnosis. */
static void
report_low_memory_map(void)
{
	uint64_t base;
	uint64_t size;
	uint32_t type;
	uint32_t index;

	for (index = 0; index < bsp_mem_range_count(); index++) {
		if (!bsp_mem_range(index, &base, &size, &type))
			break;
		if (base >= AMD64_KERNEL_PHYS_LIMIT)
			continue;
		hal_printf("A64 KERNEL map[%u] %llx-%llx type=%u\n", index,
		    (unsigned long long)base, (unsigned long long)(base + size), type);
	}
}
