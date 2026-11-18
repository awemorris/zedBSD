/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_UAPI_TLS_H
#define KERN_UAPI_TLS_H

#include <stddef.h>
#include <stdint.h>

#define KERN_TLS_MEMORY_MAX	(1024U * 1024U)
#define KERN_TLS_ALIGN_MAX	4096U
#define KERN_TLS_TCB_RESERVE	4096U

/*
 * The two places a static TLS block can take next to its control block.
 *
 * In variant I the block follows the control block's reserve, which is
 * where the m68k and PowerPC thread pointers (0x7000 bytes past the
 * block's start, see src/libc/m68k-read-tp.c and the PowerPC hardware
 * layers' task.c) expect it.  In variant II the block
 * ends where the control block starts, as the x86 FS or GS base expects.
 */
#define KERN_TLS_VARIANT_I	1
#define KERN_TLS_VARIANT_II	2

/*
 * The placement the loader and the C library use on this processor.
 *
 * Only the m68k and the PowerPC place the block after the reserve.
 * Every other processor keeps the x86 placement, which is what the C
 * library already gives a thread there.
 */
#if defined(__m68k__) || defined(__powerpc__)
#define KERN_TLS_STATIC_VARIANT	KERN_TLS_VARIANT_I
#else
#define KERN_TLS_STATIC_VARIANT	KERN_TLS_VARIANT_II
#endif

/*
 * The compiler loads this first word through FS:0 or GS:0.
 */
struct kern_tls_prefix {
	uintptr_t self;
	uintptr_t mapping_base;
	size_t mapping_size;
	uintptr_t template_address;
	size_t template_size;
	size_t memory_size;
	size_t alignment;
	size_t distance;
};

/*
 * Where one thread's control block and static TLS block lie in the
 * mapping that holds both.
 *
 * The loader builds the first thread's mapping at exec and the C library
 * builds every later one from the same distance, so the two agree on the
 * placement through these offsets.  Each offset counts from the mapping's
 * first byte.
 */
struct kern_tls_static_layout {
	size_t mapping_size;
	size_t control_offset;
	size_t block_offset;
};

/*
 * Measures the static TLS block a PT_TLS segment needs.
 *
 * The result is what kern_tls_prefix.distance records.  In variant II it
 * runs from the block's start up to the control block, padded so that the
 * block's start keeps the segment's alignment.  In variant I the block
 * starts on a page boundary after the reserve, so it is the block's size.
 */
static __inline size_t
kern_tls_static_distance(
	uint64_t address,
	uint64_t memory_size,
	size_t alignment,
	int variant)
{
	uint64_t padding;

	/* The block starts right after the reserve and needs no padding. */
	if (variant == KERN_TLS_VARIANT_I)
		return (size_t)memory_size;

	/* Pads the block so its start keeps the segment's alignment. */
	padding = (0U - address - memory_size) & (uint64_t)(alignment - 1U);

	/* Reports the span from the block's start up to the control block. */
	return (size_t)(memory_size + padding);
}

/*
 * Places a thread's control block and static TLS block in one mapping.
 *
 * The block takes whole pages and the control block a reserve of
 * KERN_TLS_TCB_RESERVE bytes, in the order the variant gives.
 */
static __inline void
kern_tls_static_layout(
	size_t distance,
	size_t page_size,
	int variant,
	struct kern_tls_static_layout *layout)
{
	size_t payload_size;

	/* The block, rounded up to whole pages. */
	payload_size = (distance + page_size - 1U) & ~(page_size - 1U);
	layout->mapping_size = payload_size + KERN_TLS_TCB_RESERVE;

	/* The control block first, then the block after its reserve. */
	if (variant == KERN_TLS_VARIANT_I) {
		layout->control_offset = 0;
		layout->block_offset = KERN_TLS_TCB_RESERVE;
		return;
	}

	/* The block first, ending where the control block starts. */
	layout->control_offset = payload_size;
	layout->block_offset = payload_size - distance;
}

#endif
