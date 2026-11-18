/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The checks of the devcfg driver that need no hardware: which bits of
 * CTRL a request may touch, whether a bitstream has its sync word, and
 * whether the configuration DMA can take a buffer.  They are separate so
 * that tests/zynq-devcfg-host-test.c runs them on the host.
 *
 * Values referenced: Linux v6.12 drivers/fpga/zynq-fpga.c,
 * zynq_fpga_has_sync (the sync word 0xAA995566 as the bytes 66 55 99 AA)
 * and zynq_fpga_ops_write (the DMA's 8-byte aligned address and length in
 * words, after "UG585 pg 212").
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/platform/zybo/zynq-devcfg.h>

/* The sync word as the configuration DMA reads it from memory, byte by byte. */
#define SYNC_BYTE_0		0x66U
#define SYNC_BYTE_1		0x55U
#define SYNC_BYTE_2		0x99U
#define SYNC_BYTE_3		0xaaU

/* The alignment of a DMA source address, and the size of the word the DMA counts in. */
#define DMA_ADDRESS_ALIGN	8U
#define DMA_WORD_BYTES		4U

/* The first address the DMA's 32-bit source register cannot hold. */
#define DMA_ADDRESS_LIMIT	0x100000000ULL

/*
 * Reports whether a request to set and clear bits of CTRL is allowed.
 */
int
drv_zynq_devcfg_ctrl_request_valid(
	uint32_t set,
	uint32_t clear)
{
	/* A request that changes nothing is a mistake of the caller. */
	if ((set | clear) == 0)
		return 0;

	/* A bit outside the allowed ones would reach the PS's reset or JTAG. */
	if (((set | clear) & ~DRV_ZYNQ_DEVCFG_CTRL_ALLOWED) != 0)
		return 0;

	/* Setting and clearing one bit at once says nothing. */
	if ((set & clear) != 0)
		return 0;

	/* Succeeded: the request only touches the configuration's bits. */
	return 1;
}

/*
 * Reports whether a bitstream carries the sync word near its start.
 */
int
drv_zynq_devcfg_has_sync(
	const uint8_t *data,
	size_t size)
{
	size_t limit;
	size_t offset;

	/* No data has no sync word. */
	if (data == NULL || size < DMA_WORD_BYTES)
		return 0;

	/* Looks only at the start, where the word comes after the pad. */
	limit = size;
	if (limit > DRV_ZYNQ_DEVCFG_SYNC_SEARCH)
		limit = DRV_ZYNQ_DEVCFG_SYNC_SEARCH;

	/* Each word boundary in reach. */
	for (offset = 0; offset + DMA_WORD_BYTES <= limit; offset += DMA_WORD_BYTES) {
		if (data[offset] != SYNC_BYTE_0)
			continue;
		if (data[offset + 1U] != SYNC_BYTE_1)
			continue;
		if (data[offset + 2U] != SYNC_BYTE_2)
			continue;
		if (data[offset + 3U] != SYNC_BYTE_3)
			continue;

		/* Succeeded: the sync word is at this word. */
		return 1;
	}

	/* No word in reach is the sync word. */
	return 0;
}

/*
 * Reports whether the configuration DMA can take a buffer.
 */
int
drv_zynq_devcfg_dma_valid(
	uint64_t physical,
	size_t length)
{
	/* The DMA reads from an 8-byte aligned address only. */
	if ((physical % DMA_ADDRESS_ALIGN) != 0)
		return 0;

	/* The DMA counts whole words, at least one. */
	if (length == 0 || (length % DMA_WORD_BYTES) != 0)
		return 0;

	/* The source register is 32 bits wide, and its low bit marks the last transfer. */
	if (physical >= DMA_ADDRESS_LIMIT)
		return 0;
	if ((uint64_t)length > DMA_ADDRESS_LIMIT - physical)
		return 0;

	/* Succeeded: the DMA can read the buffer in one transfer. */
	return 1;
}
