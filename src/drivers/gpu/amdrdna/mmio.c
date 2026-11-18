/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Register and VRAM access of the amdrdna driver.
 *
 * Registers are named by dword index.  An index inside the mapped register
 * window is read directly; the few registers beyond it, and the
 * system-management unit's words, go through the indirect register window
 * (an address register and a data register).  VRAM outside the CPU's
 * window is read one dword at a time through the indirect VRAM window.
 * Each indirect window is two steps that must not interleave with
 * another user's, so each has its own spinlock, taken with interrupts
 * off.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/device-io.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"

/*
 * Reads one register by its dword index.
 *
 * An index past the mapped window goes through the indirect register
 * window.
 */
uint32_t
amdrdna_read(
	struct amdrdna_device *device,
	uint32_t index)
{
	const uint8_t *base;
	uint32_t value;

	/* Reads an index past the window through the indirect window, as a byte address. */
	if ((uint64_t)index * 4U >= device->registers.size) {
		value = amdrdna_far_read(device, index * 4U);
		return value;
	}

	/* Reads the register in the mapped window. */
	base = device->registers.address;
	value = kern_mmio_read32(base + (size_t)index * 4U);

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Writes one register inside the mapped window by its dword index.
 *
 * Every register the driver writes lies inside the mapped window; an
 * index past it is not written.
 */
void
amdrdna_write(
	struct amdrdna_device *device,
	uint32_t index,
	uint32_t value)
{
	uint8_t *base;

	/* Nothing past the mapped window is written by this path. */
	if ((uint64_t)index * 4U >= device->registers.size)
		return;

	/* Writes the register in the mapped window. */
	base = device->registers.address;
	kern_mmio_write32(base + (size_t)index * 4U, value);
}

/*
 * Turns a register of a known IP block into its absolute dword index.
 *
 * Returns 0 with the index, or ENOENT when the block table did not list
 * the block or the block has no such window.
 */
int
amdrdna_block_index(
	struct amdrdna_device *device,
	enum amdrdna_block block,
	unsigned window,
	uint32_t offset,
	uint32_t *index)
{
	const struct amdrdna_ip *ip;

	/* The block must be listed, with the window. */
	ip = &device->blocks.blocks[block];
	if (ip->found == 0U)
		return ENOENT;
	if (window >= ip->window_count)
		return ENOENT;

	/* The window's start plus the register's offset in it. */
	*index = ip->window[window] + offset;

	/* Succeeded: the index names the register. */
	return 0;
}

/*
 * Reads a register of a known IP block by its window and offset.
 *
 * A register of a block or window the block table did not list reads as
 * zero; the stages check the blocks they use before they start.
 */
uint32_t
amdrdna_block_read(
	struct amdrdna_device *device,
	enum amdrdna_block block,
	unsigned window,
	uint32_t offset)
{
	uint32_t index;
	uint32_t value;
	int error;

	/* Finds the register's index. */
	error = amdrdna_block_index(device, block, window, offset, &index);
	if (error != 0)
		return 0U;

	/* Reads it. */
	value = amdrdna_read(device, index);

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Writes a register of a known IP block by its window and offset.
 *
 * A register of a block or window the block table did not list is not
 * written.
 */
void
amdrdna_block_write(
	struct amdrdna_device *device,
	enum amdrdna_block block,
	unsigned window,
	uint32_t offset,
	uint32_t value)
{
	uint32_t index;
	int error;

	/* Finds the register's index. */
	error = amdrdna_block_index(device, block, window, offset, &index);
	if (error != 0)
		return;

	/* Writes it. */
	amdrdna_write(device, index, value);
}

/*
 * Reads one word through the indirect register window.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_device.c (amdgpu_device_indirect_rreg:
 * write the address, read it back to post the write, then read the data).
 */
uint32_t
amdrdna_far_read(
	struct amdrdna_device *device,
	uint32_t address)
{
	uint8_t *base;
	unsigned long irq;
	uint32_t value;

	/* Writes the address, posts it, and reads the data, without another user between. */
	base = device->registers.address;
	irq = spin_lock_irqsave(&device->far_lock);

	kern_mmio_write32(base + AMDRDNA_NBIO_REG_FAR_ADDRESS * 4U, address);
	(void)kern_mmio_read32(base + AMDRDNA_NBIO_REG_FAR_ADDRESS * 4U);
	value = kern_mmio_read32(base + AMDRDNA_NBIO_REG_FAR_DATA * 4U);

	spin_unlock_irqrestore(&device->far_lock, irq);

	/* Succeeded: the word at the address. */
	return value;
}

/*
 * Reads VRAM through the indirect VRAM window into a buffer.
 *
 * offset and length are whole dwords.  The lock is taken for one dword at
 * a time, so a long read does not keep interrupts off; each dword
 * therefore sets both address words.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_device.c (amdgpu_device_mm_access).
 */
void
amdrdna_vram_read(
	struct amdrdna_device *device,
	uint64_t offset,
	void *buffer,
	size_t length)
{
	uint32_t *words;
	uint32_t low;
	uint32_t high;
	uint64_t position;
	size_t index;
	unsigned long irq;

	/* Reads every dword of the range. */
	words = buffer;
	for (index = 0U; index < length / 4U; index++) {
		/* The low address word carries the request bit, the high one the bits above the low 31. */
		position = offset + (uint64_t)index * 4U;
		low = (uint32_t)(position & AMDRDNA_VRAM_WINDOW_LOW_MASK) | AMDRDNA_VRAM_WINDOW_REQUEST;
		high = (uint32_t)(position >> AMDRDNA_VRAM_WINDOW_HIGH_SHIFT);

		/* Sets the address and reads the dword, without another user between. */
		irq = spin_lock_irqsave(&device->vram_window_lock);

		amdrdna_write(device, AMDRDNA_NBIO_REG_VRAM_WINDOW_LOW, low);
		amdrdna_write(device, AMDRDNA_NBIO_REG_VRAM_WINDOW_HIGH, high);
		words[index] = amdrdna_read(device, AMDRDNA_NBIO_REG_VRAM_WINDOW_DATA);

		spin_unlock_irqrestore(&device->vram_window_lock, irq);
	}
}

/*
 * Writes a buffer to VRAM through the indirect VRAM window.
 *
 * offset and length are whole dwords.  As with the read, the lock is
 * taken for one dword at a time and each dword sets both address words.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_device.c (amdgpu_device_mm_access).
 */
void
amdrdna_vram_write(
	struct amdrdna_device *device,
	uint64_t offset,
	const void *buffer,
	size_t length)
{
	const uint32_t *words;
	uint32_t low;
	uint32_t high;
	uint64_t position;
	size_t index;
	unsigned long irq;

	/* Writes every dword of the range. */
	words = buffer;
	for (index = 0U; index < length / 4U; index++) {
		/* The low address word carries the request bit, the high one the bits above the low 31. */
		position = offset + (uint64_t)index * 4U;
		low = (uint32_t)(position & AMDRDNA_VRAM_WINDOW_LOW_MASK) | AMDRDNA_VRAM_WINDOW_REQUEST;
		high = (uint32_t)(position >> AMDRDNA_VRAM_WINDOW_HIGH_SHIFT);

		/* Sets the address and writes the dword, without another user between. */
		irq = spin_lock_irqsave(&device->vram_window_lock);

		amdrdna_write(device, AMDRDNA_NBIO_REG_VRAM_WINDOW_LOW, low);
		amdrdna_write(device, AMDRDNA_NBIO_REG_VRAM_WINDOW_HIGH, high);
		amdrdna_write(device, AMDRDNA_NBIO_REG_VRAM_WINDOW_DATA, words[index]);

		spin_unlock_irqrestore(&device->vram_window_lock, irq);
	}
}
