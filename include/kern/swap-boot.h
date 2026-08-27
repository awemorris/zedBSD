/* Boot-parameter selected swap sources.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_SWAP_BOOT_H
#define ZEDBSD_KERN_SWAP_BOOT_H

#include <kern/swap-source.h>

struct kern_boot_parameters;
struct kern_boot_source_context;

int
kern_swap_boot_prepare(
	const struct kern_boot_parameters *parameters,
	struct kern_boot_source_context *boot_sources,
	struct kern_swap_source_set *swap_sources,
	unsigned *failed_parameter);

#endif
