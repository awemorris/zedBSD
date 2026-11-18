/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The procedure that finds whether a bus master's DMA is coherent with
 * the CPU's caches (design-peripherals.md section 2.4).
 *
 * The procedure sees the test's memory only through its callbacks, so a
 * host test can run it against a model of the caches: a cached view (the
 * view a driver uses), an uncached alias of the same memory, the cache
 * controller's flush and the bus master's copy.  Offsets are bytes from
 * the start of the test's region.
 */

#ifndef KERN_DRIVERS_VF2_DMATEST_CORE_H
#define KERN_DRIVERS_VF2_DMATEST_CORE_H

#include <stddef.h>
#include <stdint.h>

/* The test's region: 2 MiB, aligned to 2 MiB, holding every buffer. */
#define DRV_VF2_DMATEST_REGION		0x200000U

/* One buffer: 64 KiB, so a source and a destination are 1/16 of the L2. */
#define DRV_VF2_DMATEST_BUFFER		0x10000U

/* Each run moves its buffers this far into the region. */
#define DRV_VF2_DMATEST_STEP		0x20000U

/* The runs of the main test. */
#define DRV_VF2_DMATEST_RUNS		8U

/* The view a load or a store goes through. */
#define DRV_VF2_DMATEST_CACHED		0
#define DRV_VF2_DMATEST_ALIAS		1

/* The first control: whether the alias reaches the same memory. */
#define DRV_VF2_DMATEST_ALIAS_OK	0
#define DRV_VF2_DMATEST_ALIAS_MISMATCH	1

/* The second control: whether a stale cached line can be seen. */
#define DRV_VF2_DMATEST_CONTROL_OK		0
#define DRV_VF2_DMATEST_CONTROL_INCONCLUSIVE	1
#define DRV_VF2_DMATEST_CONTROL_SKIPPED		2

/* The answer. */
#define DRV_VF2_DMATEST_COHERENT	0
#define DRV_VF2_DMATEST_NONCOHERENT	1
#define DRV_VF2_DMATEST_COPY_FAILED	2

/* Which side's caching a mismatch points at. */
#define DRV_VF2_DMATEST_UNCLASSIFIED	0
#define DRV_VF2_DMATEST_CPU_TO_DEVICE	1
#define DRV_VF2_DMATEST_DEVICE_TO_CPU	2

/*
 * The test's memory and bus master.
 *
 * Load and store move one aligned 64-bit word through a view.  Flush
 * writes back and discards the cached lines of a range.  Copy makes the
 * bus master copy a range and wait for the end; it reports an errno.
 */
struct drv_vf2_dmatest_ops {
	void *context;
	uint64_t (*load)(void *context, int view, size_t offset);
	void (*store)(void *context, int view, size_t offset, uint64_t value);
	void (*flush)(void *context, size_t offset, size_t size);
	int (*copy)(void *context, size_t destination, size_t source, size_t size);
};

/*
 * What one test found.
 *
 * The first mismatch is the run, whether it was the reverse copy, the
 * byte offset in the destination buffer and the direction it points at.
 */
struct drv_vf2_dmatest_result {
	int alias;
	int control;
	int verdict;
	int direction;
	unsigned mismatches;
	unsigned run;
	int reverse;
	size_t offset;
	int copy_error;
};

/*
 * Runs the controls and the eight runs of both directions, then flushes
 * the whole region.  The result is always filled.
 */
void drv_vf2_dmatest_run(const struct drv_vf2_dmatest_ops *ops, struct drv_vf2_dmatest_result *result);

/*
 * Writes the result as text lines naming the bus master, terminated.
 * Reports the length without the terminator.
 */
size_t drv_vf2_dmatest_format(const struct drv_vf2_dmatest_result *result, const char *master, char *text, size_t capacity);

#endif
