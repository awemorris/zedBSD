/*
 * Boots kernel bring-up bridge
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * The HAL enters this ordinary C function after locore has selected the
 * kernel boot stack.  This wires
 * the HAL to Boots' own services — the freestanding heap becomes the
 * HAL allocator, the framebuffer takes console ownership when BeUI is
 * open — and then runs the existing Boots main.
 *
 * kernel_timer_handler() is the tick the HAL delivers on IRQ 0; with
 * the single-task scheduler stub it only advances the millisecond
 * clock that BeUI and, later, the network stack read.
 */

#include <stddef.h>
#include <stdint.h>

#include "libc/heap.h"
#include "kern/boot.h"
#include "kern/kernel.h"
#include "kern/platform.h"

/*
 * Minimal HAL entry points the kernel wires up.  Declared locally so
 * this Boots-side bridge stays in Boots' type world rather than pulling
 * in the HAL's sys/ headers (which define their own uint types).
 */
void hal_set_allocator(void *(*alloc)(size_t size), void (*free)(void *p));
void hal_fatal(const char *file, int line, const char *message);

static void *
kernel_alloc(size_t size)
{
	return boots_malloc(size);
}

static void
kernel_free(void *p)
{
	boots_free(p);
}

/*
 * Called by the HAL with the boot handoff pointer.
 */
void
kernel_entry(const void *handoff)
{
	const struct boots_handoff *h = handoff;
	static struct boots_device devices[KERN_PLATFORM_MAX_DEVICES];
	size_t device_count;

	if (h == NULL || h->magic != BOOTS_HANDOFF_MAGIC || h->version != 2 ||
	    h->size < sizeof(*h))
		hal_fatal(__FILE__, __LINE__, "invalid Boots handoff");
	/* The HAL allocates only through this hook (task creation); point
	 * it at the freestanding heap before anything in the HAL needs it. */
	hal_set_allocator(kernel_alloc, kernel_free);

	device_count = kern_platform_init(h, devices, KERN_PLATFORM_MAX_DEVICES);
	if (device_count == 0)
		hal_fatal(__FILE__, __LINE__, "no boot devices");
	kernel_main(h, devices, (unsigned)device_count);
}
