/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The feedback table builder of the host unit tests of the x86 processor
 * decoders: it writes a hardware feedback table the way the processor
 * would, from the offsets of Intel SDM Vol. 3B rev. 093, Table 17-7 notes 1
 * to 6, without the decoder's code.  CP is the number of capabilities and
 * CL of classes; R8 rounds a run of bytes up to a multiple of 8.
 */

#ifndef CPUX86_TEST_FEEDBACKBUILD_H
#define CPUX86_TEST_FEEDBACKBUILD_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Rounds a byte count up to a multiple of 8 (CP * CL + R8 of the notes). */
static __inline size_t
feedbackbuild_round8(
	size_t bytes)
{
	/* Reports the next multiple of 8 at or above bytes. */
	return (bytes + 7U) / 8U * 8U;
}

/*
 * Reports the offset of the cell of capability cp of class cl in logical
 * processor entry i (note 3: 8 + (i + 1) * (CP * CL + R8) + CP * cl + cp).
 */
static __inline size_t
feedbackbuild_cell_offset(
	size_t capabilities,
	size_t classes,
	size_t row,
	size_t class_index,
	size_t capability)
{
	size_t stride;

	/* One entry is the cells of every class, padded to 8 bytes. */
	stride = feedbackbuild_round8(capabilities * classes);

	/* Reports the cell's place after the header and the rows before it. */
	return 8U + (row + 1U) * stride + capabilities * class_index + capability;
}

/*
 * Writes one cell; a cell outside the buffer is a test bug and stops the
 * test.
 */
static __inline void
feedbackbuild_cell(
	uint8_t *table,
	size_t table_size,
	size_t capabilities,
	size_t classes,
	size_t row,
	size_t class_index,
	size_t capability,
	uint8_t value)
{
	size_t offset;

	/* Finds the cell from the specification's formula. */
	offset = feedbackbuild_cell_offset(capabilities, classes, row, class_index, capability);
	if (offset >= table_size) {
		printf("feedbackbuild_cell: offset %lu beyond %lu\n", (unsigned long)offset, (unsigned long)table_size);
		abort();
	}

	/* Stores the value where the processor would. */
	table[offset] = value;
}

/*
 * Writes one change-flag byte of the header (note 1: 8 + CP * cl + cp).
 */
static __inline void
feedbackbuild_change(
	uint8_t *table,
	size_t table_size,
	size_t capabilities,
	size_t class_index,
	size_t capability,
	uint8_t flags)
{
	size_t offset;

	/* Finds the byte from the specification's formula. */
	offset = 8U + capabilities * class_index + capability;
	if (offset >= table_size) {
		printf("feedbackbuild_change: offset %lu beyond %lu\n", (unsigned long)offset, (unsigned long)table_size);
		abort();
	}

	/* Stores the flags where the processor would. */
	table[offset] = flags;
}

/*
 * Writes the timestamp, the first 8 bytes, least significant byte first.
 */
static __inline void
feedbackbuild_timestamp(
	uint8_t *table,
	uint64_t timestamp)
{
	unsigned index;

	/* Stores one byte per step, the lowest first. */
	for (index = 0U; index < 8U; index++)
		table[index] = (uint8_t)((timestamp >> (8U * index)) & 0xFFU);
}

#endif
