/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host's stand-ins for the kernel's heap, mutexes, log and direct map
 * that the render executor's contract tests (memory, forget; BUG-244,
 * BUG-260) link instead of host_unreached.c: the host's heap, mutexes that
 * only count how deeply they are held (one thread), a log that counts and
 * shows its lines, and the state.c format table no check uses.
 */

#ifndef DRIVERS_GPU_I915_TESTS_CONTRACTS_HOST_RENDER_H
#define DRIVERS_GPU_I915_TESTS_CONTRACTS_HOST_RENDER_H

/* How many log lines the code under test wrote, and how deeply its mutexes are held now. */
extern unsigned host_render_log_lines;
extern int host_render_lock_depth;

#endif
