/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host's stand-ins for the kernel that the render executor's contract
 * tests link (see host_render.h).
 */

#include "contract.h"
#include "host_render.h"

#include "../../render/state.h"

#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/pmem.h>

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* How many log lines the code under test wrote; a test sets it to 0 before what it counts. */
unsigned host_render_log_lines;

/* How deeply the mutexes are held now: 0 when every one taken was let go. */
int host_render_lock_depth;

/*
 * Stands in for the kernel heap's zeroed allocation: the host's.
 */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	void *pointer;

	/* The host's allocation. */
	pointer = calloc(count, size);

	/* Reports it (NULL when the host had none). */
	return pointer;
}

/*
 * Stands in for the kernel heap's release: the host's.
 */
void
kern_free(
	void *pointer)
{
	/* The host's release. */
	free(pointer);
}

/*
 * Stands in for the kernel log: counts the lines and shows them.
 */
void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;

	/* One more line, shown. */
	host_render_log_lines++;
	va_start(arguments, format);
	(void)vprintf(format, arguments);
	va_end(arguments);
}

/*
 * Stands in for a mutex's setup: nothing to do on one thread.
 */
int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	UNUSED_PARAMETER(mutex);
	UNUSED_PARAMETER(rank);
	UNUSED_PARAMETER(name);

	/* Succeeded: ready. */
	return 0;
}

/*
 * Stands in for taking a mutex: counts how deeply mutexes are held.
 */
void
mutex_lock(
	struct mutex *mutex)
{
	UNUSED_PARAMETER(mutex);

	/* One more held. */
	host_render_lock_depth++;
}

/*
 * Stands in for letting a mutex go.
 */
void
mutex_unlock(
	struct mutex *mutex)
{
	UNUSED_PARAMETER(mutex);

	/* One fewer held. */
	host_render_lock_depth--;
}

/*
 * Stands in for the direct map of physical RAM: the address itself (the
 * test's storage has no pages, and no check reads through the view).
 */
void *
kern_pmem_to_kernel(
	hal_physaddr_t address)
{
	/* The address as a pointer. */
	return (void *)(uintptr_t)address;
}

/*
 * Stands in for the texel buffer formats of state.c, which no check uses.
 */
int
drv_i915_gfx_texel_buffer_format(
	uint32_t format,
	uint32_t *surface_format,
	uint32_t *bytes)
{
	UNUSED_PARAMETER(format);
	UNUSED_PARAMETER(surface_format);
	UNUSED_PARAMETER(bytes);

	/* No format in this test. */
	return EINVAL;
}
