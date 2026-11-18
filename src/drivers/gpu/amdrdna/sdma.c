/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage A5's copy engine: the first copy engine's graphics queue.
 *
 * The security processor loaded the copy engines' programs in stage A3.
 * This file gives both engines their recommended settings, unhalts the
 * first one and sets its ring up: the ring's size and base, where the
 * engine reports its read pointer and polls the write pointer, its
 * doorbell and the host bridge's doorbell range, its address translation
 * and page policy, then the ring and its indirect buffers on.  The second
 * engine stays halted.  The driver uses the copy engine for its own
 * copies only.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"

/*
 * The address translation's response mode (3) and redo delay (9).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c (sdma_v5_0_gfx_resume_instance).
 */
#define COPY_TRANSLATION_MODE		3U
#define COPY_TRANSLATION_DELAY		9U

/* The words of the ring's write-back slot: the read pointer and the write pointer's copy. */
#define COPY_WORD_READ			0U
#define COPY_WORD_WRITE			2U

static uint32_t copy_read(struct amdrdna_device *device, uint32_t offset);
static void copy_write(struct amdrdna_device *device, uint32_t offset, uint32_t value);
static void copy_change(struct amdrdna_device *device, uint32_t offset, uint32_t clear, uint32_t set);
static unsigned copy_log2(uint32_t value);

/*
 * Gives both copy engines their settings, starts the first one's ring,
 * turns its trap interrupt on and tests the ring.
 *
 * Returns 0, or ETIMEDOUT when the ring test failed.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c (sdma_v5_0_hw_init,
 * sdma_v5_0_init_golden_registers, sdma_v5_0_start, sdma_v5_0_enable,
 * sdma_v5_0_ctx_switch_enable, sdma_v5_0_gfx_resume_instance,
 * sdma_v5_0_set_trap_irq_state) and nbio_v2_3.c
 * (nbio_v2_3_sdma_doorbell_range).
 */
int
amdrdna_sdma_start(
	struct amdrdna_device *device)
{
	const struct amdrdna_register_setting *settings;
	struct amdrdna_ring *ring;
	volatile uint32_t *read_pointer;
	uint64_t address;
	uint32_t control;
	uint32_t word;
	unsigned count;
	unsigned index;
	bool reads;
	int error;

	/* The recommended settings of both engines. */
	ring = &device->engines.copy;
	settings = amdrdna_copy_settings(&count);
	for (index = 0U; index < count; index++) {
		/* Reads the old value only where it is kept. */
		word = 0U;
		reads = amdrdna_setting_reads(&settings[index]);
		if (reads)
			word = amdrdna_block_read(device, AMDRDNA_BLOCK_GC, settings[index].window, settings[index].offset);

		/* Writes the setting. */
		word = amdrdna_setting_word(&settings[index], word);
		amdrdna_block_write(device, AMDRDNA_BLOCK_GC, settings[index].window, settings[index].offset, word);
	}

	/* Unhalts the engine, with its automatic context switch on. */
	copy_change(device, AMDRDNA_COPY_REG_HALT, AMDRDNA_COPY_HALTED, 0U);
	copy_change(device, AMDRDNA_COPY_REG_CONTROL, 0U, AMDRDNA_COPY_CONTROL_SWITCH);
	device->engines.copy_running = true;

	/* No semaphore wait-fail timer. */
	copy_write(device, AMDRDNA_COPY_REG_WAIT_TIMER, 0U);

	/* The ring's size in dwords, as log2. */
	control = copy_read(device, AMDRDNA_COPY_REG_RING_CONTROL);
	control &= ~AMDRDNA_COPY_RING_SIZE_MASK;
	control |= (copy_log2(ring->bytes / 4U) << AMDRDNA_COPY_RING_SIZE_SHIFT) & AMDRDNA_COPY_RING_SIZE_MASK;
	copy_write(device, AMDRDNA_COPY_REG_RING_CONTROL, control);

	/* Empty read and write pointers. */
	copy_write(device, AMDRDNA_COPY_REG_READ_LOW, 0U);
	copy_write(device, AMDRDNA_COPY_REG_READ_HIGH, 0U);
	copy_write(device, AMDRDNA_COPY_REG_WRITE_LOW, 0U);
	copy_write(device, AMDRDNA_COPY_REG_WRITE_HIGH, 0U);

	/* The write pointer's copy, which the engine polls. */
	address = device->engines.writeback.gpu_address + ring->slot + COPY_WORD_WRITE * 4U;
	copy_write(device, AMDRDNA_COPY_REG_WRITE_POLL_LOW, (uint32_t)address);
	copy_write(device, AMDRDNA_COPY_REG_WRITE_POLL_HIGH, (uint32_t)(address >> 32));
	copy_change(device, AMDRDNA_COPY_REG_WRITE_POLL, 0U, AMDRDNA_COPY_WRITE_POLL_ON);

	/* Where the engine reports its read pointer, and the report on. */
	address = device->engines.writeback.gpu_address + ring->slot + COPY_WORD_READ * 4U;
	copy_write(device, AMDRDNA_COPY_REG_READ_REPORT_HIGH, (uint32_t)(address >> 32));
	copy_write(device, AMDRDNA_COPY_REG_READ_REPORT_LOW, (uint32_t)address & 0xfffffffcU);
	control |= AMDRDNA_COPY_RING_REPORT;

	/* The ring's base, in 256-byte units. */
	copy_write(device, AMDRDNA_COPY_REG_RING_BASE_LOW, (uint32_t)(ring->buffer.gpu_address >> 8));
	copy_write(device, AMDRDNA_COPY_REG_RING_BASE_HIGH, (uint32_t)(ring->buffer.gpu_address >> 40));

	/* The write pointer goes back to 0, which needs the minor pointer update around it. */
	ring->write_pointer = 0U;
	read_pointer = amdrdna_ring_word(device, ring, COPY_WORD_READ);
	*read_pointer = 0U;
	copy_write(device, AMDRDNA_COPY_REG_MINOR_UPDATE, 1U);
	copy_write(device, AMDRDNA_COPY_REG_WRITE_LOW, 0U);
	copy_write(device, AMDRDNA_COPY_REG_WRITE_HIGH, 0U);

	/* The doorbell on, at the engine's doorbell. */
	copy_change(device, AMDRDNA_COPY_REG_DOORBELL, 0U, AMDRDNA_COPY_DOORBELL_ON);
	copy_change(device, AMDRDNA_COPY_REG_DOORBELL_OFFSET, AMDRDNA_COPY_DOORBELL_OFFSET_MASK,
	    (ring->doorbell << AMDRDNA_COPY_DOORBELL_OFFSET_SHIFT) & AMDRDNA_COPY_DOORBELL_OFFSET_MASK);

	/* The host bridge routes the engine's doorbells. */
	word = amdrdna_block_read(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_HOST, AMDRDNA_NBIO_REG_COPY_DOORBELLS);
	word &= ~(AMDRDNA_NBIO_COPY_DOORBELLS_OFFSET_MASK | AMDRDNA_NBIO_COPY_DOORBELLS_SIZE_MASK);
	word |= (ring->doorbell << 2) & AMDRDNA_NBIO_COPY_DOORBELLS_OFFSET_MASK;
	word |= (AMDRDNA_DOORBELL_COPY_COUNT << AMDRDNA_NBIO_COPY_DOORBELLS_SIZE_SHIFT) & AMDRDNA_NBIO_COPY_DOORBELLS_SIZE_MASK;
	amdrdna_block_write(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_HOST, AMDRDNA_NBIO_REG_COPY_DOORBELLS, word);

	/* The minor pointer update done. */
	copy_write(device, AMDRDNA_COPY_REG_MINOR_UPDATE, 0U);

	/* Address translation and mid-command preemption on. */
	copy_change(device, AMDRDNA_COPY_REG_CONTROL, 0U, AMDRDNA_COPY_CONTROL_TRANSLATE | AMDRDNA_COPY_CONTROL_PREEMPT);

	/* The translation's response mode and redo delay. */
	word = copy_read(device, AMDRDNA_COPY_REG_TRANSLATION);
	word &= ~(AMDRDNA_COPY_TRANSLATION_MODE_MASK | AMDRDNA_COPY_TRANSLATION_DELAY_MASK);
	word |= COPY_TRANSLATION_MODE << AMDRDNA_COPY_TRANSLATION_MODE_SHIFT;
	word |= COPY_TRANSLATION_DELAY << AMDRDNA_COPY_TRANSLATION_DELAY_SHIFT;
	copy_write(device, AMDRDNA_COPY_REG_TRANSLATION, word);

	/* The default read and write policies of the level-2 cache. */
	word = copy_read(device, AMDRDNA_COPY_REG_PAGE_POLICY);
	word &= AMDRDNA_COPY_PAGE_POLICY_KEEP;
	word |= AMDRDNA_COPY_PAGE_POLICY_SET;
	copy_write(device, AMDRDNA_COPY_REG_PAGE_POLICY, word);

	/* Unhalts the engine again, as the queue setup ends. */
	copy_change(device, AMDRDNA_COPY_REG_HALT, AMDRDNA_COPY_HALTED, 0U);

	/* The ring on, and its indirect buffers. */
	copy_write(device, AMDRDNA_COPY_REG_RING_CONTROL, control | AMDRDNA_COPY_RING_ON);
	copy_change(device, AMDRDNA_COPY_REG_BUFFER_CONTROL, 0U, AMDRDNA_COPY_BUFFERS_ON);

	/* The trap interrupt on. */
	copy_change(device, AMDRDNA_COPY_REG_CONTROL, 0U, AMDRDNA_COPY_CONTROL_TRAP);

	/* Tests the ring. */
	error = amdrdna_ring_test(device, ring);
	if (error != 0)
		return error;
	ring->up = true;

	/* Succeeded: the copy engine runs its ring. */
	return 0;
}

/*
 * Stops the first copy engine as far as stage A5 started it: the trap
 * and the context switch off, the ring and its indirect buffers off, the
 * engine halted.  Running it twice does nothing the second time.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c (sdma_v5_0_hw_fini,
 * sdma_v5_0_gfx_stop, sdma_v5_0_enable).
 */
void
amdrdna_sdma_stop(
	struct amdrdna_device *device)
{
	/* Nothing to stop when the engine was never started. */
	if (!device->engines.copy_running)
		return;

	/* The trap and the automatic context switch off. */
	copy_change(device, AMDRDNA_COPY_REG_CONTROL, AMDRDNA_COPY_CONTROL_TRAP | AMDRDNA_COPY_CONTROL_SWITCH, 0U);

	/* The ring and its indirect buffers off. */
	copy_change(device, AMDRDNA_COPY_REG_RING_CONTROL, AMDRDNA_COPY_RING_ON, 0U);
	copy_change(device, AMDRDNA_COPY_REG_BUFFER_CONTROL, AMDRDNA_COPY_BUFFERS_ON, 0U);

	/* The engine halted. */
	copy_change(device, AMDRDNA_COPY_REG_HALT, 0U, AMDRDNA_COPY_HALTED);

	/* Records that it no longer runs. */
	device->engines.copy.up = false;
	device->engines.copy_running = false;
}

/* Reads a register of the first copy engine. */
static uint32_t
copy_read(
	struct amdrdna_device *device,
	uint32_t offset)
{
	uint32_t value;

	/* The engine's registers are in the graphics block's first window. */
	value = amdrdna_block_read(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, offset);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes a register of the first copy engine. */
static void
copy_write(
	struct amdrdna_device *device,
	uint32_t offset,
	uint32_t value)
{
	/* The engine's registers are in the graphics block's first window. */
	amdrdna_block_write(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, offset, value);
}

/* Clears and then sets bits of a register of the first copy engine. */
static void
copy_change(
	struct amdrdna_device *device,
	uint32_t offset,
	uint32_t clear,
	uint32_t set)
{
	uint32_t word;

	/* Reads, changes, writes. */
	word = copy_read(device, offset);
	word &= ~clear;
	word |= set;
	copy_write(device, offset, word);
}

/* Reports log2 of a power of two, or of the next power of two above. */
static unsigned
copy_log2(
	uint32_t value)
{
	unsigned bits;

	/* Counts the bits up to the value. */
	bits = 0U;
	while (((uint32_t)1U << bits) < value)
		bits++;

	/* Succeeded: the exponent. */
	return bits;
}
