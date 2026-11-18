/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The address space's tie to the machine layer (HAL): the kernel's
 * handle of an address space's translation tables is the HAL's space.
 * Only the kernel's core includes this header; drivers use
 * <kern/vmspace.h>.
 */

#ifndef KERN_KERN_VMSPACE_PRIVATE_H
#define KERN_KERN_VMSPACE_PRIVATE_H

#include <hal/hal.h>
#include <kern/types.h>
#include <kern/vmspace.h>

/* The kernel's handle of an address space is the HAL's. */
_Static_assert(__builtin_types_compatible_p(kern_space_handle_t, hal_space_t),
	       "kern_space_handle_t is not hal_space_t");

#endif
