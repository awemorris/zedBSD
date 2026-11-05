/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 kernel image geometry: linked extent and actual placement.
 */

#ifndef KERN_HAL_AMD64_IMAGE_H
#define KERN_HAL_AMD64_IMAGE_H

#include <hal/types.h>

struct amd64_kernel_image {
	/* Linker-owned virtual extent (fixed). */
	uint64_t virt_start;
	uint64_t virt_end;

	/* The physical extent the image was linked for (fixed). */
	uint64_t link_phys_start;
	uint64_t link_phys_end;

	/* Where the loader actually put the image (equal to the linked extent
	 * unless the loader relocated it). */
	uint64_t phys_start;
	uint64_t phys_end;
	uint64_t text_phys_start;
	uint64_t text_phys_end;
	uint64_t rodata_phys_start;
	uint64_t rodata_phys_end;

	/* The linked physical range hidden behind a relocated image: it has no
	 * bootstrap-window address and is kept out of every allocator.  Empty
	 * (start == end == 0) when the image sits at its linked address. */
	uint64_t shadow_start;
	uint64_t shadow_end;
	int relocated;
};

/* Establishes and reports the geometry; fatal when the loader's placement
 * cannot be used.  Runs once the console is available. */
void prekern_amd64_image_init(void);

/* Returns the established geometry; fatal before prekern_amd64_image_init(). */
const struct amd64_kernel_image *amd64_kernel_image(void);

/* Reports whether a physical page lies in the image or in its shadow. */
int amd64_kernel_image_owns(uint64_t physical);

#endif
