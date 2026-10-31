/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Byte-level access to the memory the GSP reads.
 *
 * The GSP is a little-endian processor and its records are packed by byte
 * offset, so every field is read and written one byte at a time in
 * little-endian order.  This keeps the records independent of the C
 * compiler's structure layout and alignment.  The page helpers find the CPU
 * and bus address of one page of a chunked DMA region.
 */

#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"

/* A word that may alias the bytes it is stored over. */
typedef uint32_t __attribute__((may_alias)) wire_word;

/*
 * Reads a little-endian 32-bit field.
 */
uint32_t
nvrtx_get32(
	const uint8_t *bytes,
	size_t at)
{
	uint32_t value;

	/* Assembles the field from its four bytes, lowest first. */
	value = (uint32_t)bytes[at];
	value |= (uint32_t)bytes[at + 1U] << 8;
	value |= (uint32_t)bytes[at + 2U] << 16;
	value |= (uint32_t)bytes[at + 3U] << 24;

	/* Succeeded: the field's value. */
	return value;
}

/*
 * Reads a little-endian 64-bit field.
 */
uint64_t
nvrtx_get64(
	const uint8_t *bytes,
	size_t at)
{
	uint64_t low;
	uint64_t high;

	/* Reads the two halves, the low one first in memory. */
	low = nvrtx_get32(bytes, at);
	high = nvrtx_get32(bytes, at + 4U);

	/* Succeeded: the field's value. */
	return low | (high << 32);
}

/*
 * Writes an 8-bit field.
 */
void
nvrtx_put8(
	uint8_t *bytes,
	size_t at,
	uint8_t value)
{
	/* Stores the byte. */
	bytes[at] = value;
}

/*
 * Writes a little-endian 32-bit field: one store on a little-endian CPU
 * when the field is on a word boundary, else byte by byte.
 */
void
nvrtx_put32(
	uint8_t *bytes,
	size_t at,
	uint32_t value)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	/* The CPU's own order is the field's. */
	if (((uintptr_t)(bytes + at) & 3U) == 0U) {
		*(wire_word *)(void *)(bytes + at) = value;
		return;
	}
#endif

	/* Stores the four bytes, lowest first. */
	bytes[at] = (uint8_t)(value & 0xffU);
	bytes[at + 1U] = (uint8_t)((value >> 8) & 0xffU);
	bytes[at + 2U] = (uint8_t)((value >> 16) & 0xffU);
	bytes[at + 3U] = (uint8_t)((value >> 24) & 0xffU);
}

/*
 * Writes a little-endian 64-bit field.
 */
void
nvrtx_put64(
	uint8_t *bytes,
	size_t at,
	uint64_t value)
{
	/* Stores the low half first in memory, then the high half. */
	nvrtx_put32(bytes, at, (uint32_t)(value & 0xffffffffU));
	nvrtx_put32(bytes, at + 4U, (uint32_t)(value >> 32));
}

/*
 * Folds a message into the 32-bit checksum of the queue element.
 *
 * The 64-bit little-endian words of the message are combined with
 * exclusive or, and the two halves of the result with each other.  The
 * sender writes the fold of the element with its checksum field zero, so the
 * fold of the finished element is zero.  The length is a multiple of 8.
 */
uint32_t
nvrtx_fold_checksum(
	const uint8_t *bytes,
	size_t length)
{
	uint64_t fold;
	size_t at;

	/* Combines every word of the message. */
	fold = 0U;
	for (at = 0U; at + 8U <= length; at += 8U)
		fold ^= nvrtx_get64(bytes, at);

	/* Succeeded: the two halves folded together. */
	return (uint32_t)(fold & 0xffffffffU) ^ (uint32_t)(fold >> 32);
}

/*
 * Finds the CPU address of one page of a chunked region.
 */
uint8_t *
nvrtx_pages_cpu(
	const struct nvrtx_pages *pages,
	size_t page)
{
	size_t pages_per_chunk;
	size_t chunk;
	size_t within;

	/* Finds the chunk that holds the page and the page's place in it. */
	pages_per_chunk = pages->chunk_bytes / NVRTX_GSP_PAGE_SIZE;
	chunk = page / pages_per_chunk;
	within = page % pages_per_chunk;

	/* Succeeded: the chunk's CPU address plus the page's offset. */
	return pages->chunk_cpu[chunk] + within * NVRTX_GSP_PAGE_SIZE;
}

/*
 * Finds the bus address of one page of a chunked region.
 */
uint64_t
nvrtx_pages_bus(
	const struct nvrtx_pages *pages,
	size_t page)
{
	size_t pages_per_chunk;
	size_t chunk;
	size_t within;

	/* Finds the chunk that holds the page and the page's place in it. */
	pages_per_chunk = pages->chunk_bytes / NVRTX_GSP_PAGE_SIZE;
	chunk = page / pages_per_chunk;
	within = page % pages_per_chunk;

	/* Succeeded: the chunk's bus address plus the page's offset. */
	return pages->chunk_bus[chunk] + (uint64_t)within * NVRTX_GSP_PAGE_SIZE;
}
