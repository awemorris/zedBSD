/*
 * Boots kernel bring-up bridge
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * The HAL's cmain() calls kernel_entry() (asm, in stage2-entry.S) which
 * hands control here after switching to the kernel stack.  This wires
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
#include "platform/pc98/abi.h"

/*
 * Minimal HAL entry points the kernel wires up.  Declared locally so
 * this Boots-side bridge stays in Boots' type world rather than pulling
 * in the HAL's sys/ headers (which define their own uint types).
 */
void hal_set_allocator(void *(*alloc)(size_t size), void (*free)(void *p));
void hal_fatal(const char *file, int line, const char *message);

/* stage2.c */
void boots_main(const void *handoff);

/* Tick count maintained from the HAL timer interrupt. */
static volatile uint64_t kernel_ticks;

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
 * Called by the asm kernel_entry with the boot handoff pointer.
 */
void
boots_kernel_main(const void *handoff)
{
	const struct boots_handoff *h = handoff;

	if (h == NULL || h->magic != BOOTS_HANDOFF_MAGIC || h->version != 1 ||
	    h->size < sizeof(*h))
		hal_fatal(__FILE__, __LINE__, "invalid Boots handoff");
	/* The HAL allocates only through this hook (task creation); point
	 * it at the freestanding heap before anything in the HAL needs it. */
	hal_set_allocator(kernel_alloc, kernel_free);

	boots_main(handoff);
}

/*
 * Interval-timer tick from the HAL (hal.h contract).  CLOCK_HZ = 100,
 * so each tick is 10ms.
 */
void
kernel_timer_handler(void)
{
	kernel_ticks++;
}

uint64_t
boots_kernel_ticks(void)
{
	uint64_t first, second;
	do {
		first = kernel_ticks;
		second = kernel_ticks;
	} while (first != second);
	return first;
}

uint64_t
boots_kernel_milliseconds(void *context)
{
	(void)context;
	return boots_kernel_ticks() * 10U;
}
