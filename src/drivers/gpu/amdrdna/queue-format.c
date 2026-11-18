/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The queue images and queue words of the amdrdna driver.
 *
 * A hardware queue of the command processor is described by a queue
 * image: 512 words in memory the firmware loads the queue's registers
 * from when it attaches the queue, and saves them to when it detaches it.
 * The driver fills an image from the values the queue's registers hold
 * (the caller reads them) and the buffers it chose.  The other functions
 * build the words that select a queue, that name the scheduler's own
 * queue, and that tell the scheduler which compute queues it runs.  None
 * touches the hardware.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"

/*
 * The fixed words of a compute queue image: its header, the pipeline
 * statistics on, every compute unit of the four shader engines allowed,
 * and the reserved word the firmware expects.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_compute_mqd_init).
 */
#define QUEUE_COMPUTE_HEADER		0xc0310800U
#define QUEUE_COMPUTE_STATISTICS	0x00000001U
#define QUEUE_COMPUTE_ALL_UNITS		0xffffffffU
#define QUEUE_COMPUTE_RESERVED		0x00000003U

/*
 * The fields of a compute queue's registers the image sets: the
 * end-of-pipe buffer's size (5:0, as log2 of the dwords less one; 2 KiB
 * is 8), the doorbell (offset 27:2, source 28, enable 30, hit 31), the
 * image control's context (3:0), the ring control (size 5:0 as log2 of
 * the dwords less one, read-pointer block 13:8 likewise, 9 for a page;
 * unordered dispatch 28, tunnel 29, privileged 30, kernel queue 31), the
 * preload size of the persistent state (17:8, 0x53) and the least
 * indirect buffer room of the buffer control (21:20, 3).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_sh_mask.h
 * (CP_HQD_EOP_CONTROL__EOP_SIZE_MASK, the CP_HQD_PQ_DOORBELL_CONTROL
 * fields, CP_MQD_CONTROL__VMID_MASK, the CP_HQD_PQ_CONTROL fields,
 * CP_HQD_PERSISTENT_STATE__PRELOAD_SIZE_MASK,
 * CP_HQD_IB_CONTROL__MIN_IB_AVAIL_SIZE_MASK) and
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_compute_mqd_init,
 * GFX10_MEC_HPD_SIZE).
 */
#define QUEUE_END_SIZE_MASK		0x0000003fU
#define QUEUE_END_SIZE			8U
#define QUEUE_DOORBELL_OFFSET_MASK	0x0ffffffcU
#define QUEUE_DOORBELL_OFFSET_SHIFT	2U
#define QUEUE_DOORBELL_SOURCE		0x10000000U
#define QUEUE_DOORBELL_ON		0x40000000U
#define QUEUE_DOORBELL_HIT		0x80000000U
#define QUEUE_CONTEXT_MASK		0x0000000fU
#define QUEUE_RING_SIZE_MASK		0x0000003fU
#define QUEUE_RING_BLOCK_MASK		0x00003f00U
#define QUEUE_RING_BLOCK_SHIFT		8U
#define QUEUE_RING_BLOCK_PAGE		9U
#define QUEUE_RING_UNORDERED		0x10000000U
#define QUEUE_RING_TUNNEL		0x20000000U
#define QUEUE_RING_PRIVILEGED		0x40000000U
#define QUEUE_RING_KERNEL		0x80000000U
#define QUEUE_PRELOAD_MASK		0x0003ff00U
#define QUEUE_PRELOAD			0x00005300U
#define QUEUE_BUFFER_ROOM_MASK		0x00300000U
#define QUEUE_BUFFER_ROOM		0x00300000U

/*
 * The fields of the graphics queue's registers its image sets: the image
 * control's context (3:0), privileged state (8) and cache policy (25:24),
 * the priority level (3:0), the quantum enable (0), and the ring control's
 * size (5:0, log2 of the dwords less one) and block (13:8, the size less
 * two).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_sh_mask.h
 * (CP_GFX_MQD_CONTROL__VMID_MASK, CP_GFX_MQD_CONTROL__PRIV_STATE_MASK,
 * CP_GFX_MQD_CONTROL__CACHE_POLICY_MASK,
 * CP_GFX_HQD_QUEUE_PRIORITY__PRIORITY_LEVEL_MASK,
 * CP_GFX_HQD_QUANTUM__QUANTUM_EN_MASK, CP_GFX_HQD_CNTL__RB_BUFSZ_MASK,
 * CP_GFX_HQD_CNTL__RB_BLKSZ_MASK) and gfx_v10_0.c (gfx_v10_0_gfx_mqd_init).
 */
#define QUEUE_GRAPHICS_CONTEXT_MASK	0x0000000fU
#define QUEUE_GRAPHICS_PRIVILEGED	0x00000100U
#define QUEUE_GRAPHICS_POLICY_MASK	0x03000000U
#define QUEUE_GRAPHICS_PRIORITY_MASK	0x0000000fU
#define QUEUE_GRAPHICS_QUANTUM_ON	0x00000001U
#define QUEUE_GRAPHICS_SIZE_MASK	0x0000003fU
#define QUEUE_GRAPHICS_BLOCK_MASK	0x00003f00U
#define QUEUE_GRAPHICS_BLOCK_SHIFT	8U

/*
 * The address words of an image: an image or a report address keeps its
 * low word dword-aligned, and the high word of a report address keeps 16
 * bits.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_compute_mqd_init,
 * gfx_v10_0_gfx_mqd_init: the 0xfffffffc and 0xffff masks).
 */
#define QUEUE_LOW_ALIGN_MASK		0xfffffffcU
#define QUEUE_REPORT_HIGH_MASK		0x0000ffffU

/*
 * The word that names the scheduler's own queue for the run-list
 * controller: the queue in its low byte (engine 7:5, pipe 4:3, queue
 * 2:0) and the valid bit 7.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_kiq_setting).
 */
#define QUEUE_SCHEDULER_KEEP_MASK	0xffffff00U
#define QUEUE_SCHEDULER_ENGINE_SHIFT	5U
#define QUEUE_SCHEDULER_PIPE_SHIFT	3U
#define QUEUE_SCHEDULER_VALID		0x00000080U

/*
 * The bit of a compute queue in the scheduler's resource mask: 32 per
 * compute engine, 8 per pipe, one per queue.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_gfx.c
 * (amdgpu_queue_mask_bit_to_set_resource_bit).
 */
#define QUEUE_RESOURCE_PER_ENGINE	32U
#define QUEUE_RESOURCE_PER_PIPE		8U

/*
 * A compute queue image mirrors the queue's registers from its word 128
 * on, in the order of the registers from the image's address on; the
 * word positions of amdrdna-hw.h must agree with that order.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/v10_structs.h (struct v10_compute_mqd:
 * cp_mqd_base_addr_lo at offset 128).
 */
#define QUEUE_IMAGE_MIRROR_FIRST	128U
#define QUEUE_MIRROR(reg)		(QUEUE_IMAGE_MIRROR_FIRST + ((reg) - AMDRDNA_GC_REG_QUEUE_IMAGE_LOW))

_Static_assert(AMDRDNA_COMPUTE_IMAGE_SELF_LOW == QUEUE_MIRROR(AMDRDNA_GC_REG_QUEUE_IMAGE_LOW), "compute image: image");
_Static_assert(AMDRDNA_COMPUTE_IMAGE_ACTIVE == QUEUE_MIRROR(AMDRDNA_GC_REG_QUEUE_ACTIVE), "compute image: active");
_Static_assert(AMDRDNA_COMPUTE_IMAGE_CONTEXT == QUEUE_MIRROR(AMDRDNA_GC_REG_QUEUE_CONTEXT), "compute image: context");
_Static_assert(AMDRDNA_COMPUTE_IMAGE_PERSISTENT == QUEUE_MIRROR(AMDRDNA_GC_REG_QUEUE_PERSISTENT), "compute image: persistent");
_Static_assert(AMDRDNA_COMPUTE_IMAGE_RING_LOW == QUEUE_MIRROR(AMDRDNA_GC_REG_QUEUE_RING_LOW), "compute image: ring");
_Static_assert(AMDRDNA_COMPUTE_IMAGE_READ == QUEUE_MIRROR(AMDRDNA_GC_REG_QUEUE_READ), "compute image: read pointer");
_Static_assert(AMDRDNA_COMPUTE_IMAGE_READ_REPORT_LOW == QUEUE_MIRROR(AMDRDNA_GC_REG_QUEUE_READ_REPORT_LOW), "compute image: read report");
_Static_assert(AMDRDNA_COMPUTE_IMAGE_WRITE_POLL_LOW == QUEUE_MIRROR(AMDRDNA_GC_REG_QUEUE_WRITE_POLL_LOW), "compute image: write poll");
_Static_assert(AMDRDNA_COMPUTE_IMAGE_DOORBELL == QUEUE_MIRROR(AMDRDNA_GC_REG_QUEUE_DOORBELL), "compute image: doorbell");
_Static_assert(AMDRDNA_COMPUTE_IMAGE_RING_CONTROL == QUEUE_MIRROR(AMDRDNA_GC_REG_QUEUE_RING_CONTROL), "compute image: ring control");
_Static_assert(AMDRDNA_COMPUTE_IMAGE_BUFFER_CONTROL == QUEUE_MIRROR(AMDRDNA_GC_REG_QUEUE_BUFFER_CONTROL), "compute image: buffer control");
_Static_assert(AMDRDNA_COMPUTE_IMAGE_DEQUEUE == QUEUE_MIRROR(AMDRDNA_GC_REG_QUEUE_DEQUEUE), "compute image: dequeue");
_Static_assert(AMDRDNA_COMPUTE_IMAGE_CONTROL == QUEUE_MIRROR(AMDRDNA_GC_REG_QUEUE_IMAGE_CONTROL), "compute image: control");
_Static_assert(AMDRDNA_COMPUTE_IMAGE_END_LOW == QUEUE_MIRROR(AMDRDNA_GC_REG_QUEUE_END_LOW), "compute image: end of pipe");
_Static_assert(AMDRDNA_COMPUTE_IMAGE_END_CONTROL == QUEUE_MIRROR(AMDRDNA_GC_REG_QUEUE_END_CONTROL), "compute image: end control");
_Static_assert(AMDRDNA_COMPUTE_IMAGE_WRITE_LOW == QUEUE_MIRROR(AMDRDNA_GC_REG_QUEUE_WRITE_LOW), "compute image: write pointer");

static unsigned queue_log2(uint32_t value);

/*
 * Fills a compute queue's image from the buffers in input and the values
 * its registers held (read with the queue selected).  The image is active
 * only for the scheduler's own queue; the scheduler activates the others
 * when it attaches them.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_compute_mqd_init).
 */
void
amdrdna_compute_image(
	const struct amdrdna_compute_image_input *input,
	uint32_t *image)
{
	uint32_t word;
	unsigned index;

	/* Starts from zero. */
	for (index = 0U; index < AMDRDNA_QUEUE_IMAGE_WORDS; index++)
		image[index] = 0U;

	/* The fixed words. */
	image[AMDRDNA_COMPUTE_IMAGE_HEADER] = QUEUE_COMPUTE_HEADER;
	image[AMDRDNA_COMPUTE_IMAGE_STATISTICS] = QUEUE_COMPUTE_STATISTICS;
	image[AMDRDNA_COMPUTE_IMAGE_UNITS_0] = QUEUE_COMPUTE_ALL_UNITS;
	image[AMDRDNA_COMPUTE_IMAGE_UNITS_1] = QUEUE_COMPUTE_ALL_UNITS;
	image[AMDRDNA_COMPUTE_IMAGE_UNITS_2] = QUEUE_COMPUTE_ALL_UNITS;
	image[AMDRDNA_COMPUTE_IMAGE_UNITS_3] = QUEUE_COMPUTE_ALL_UNITS;
	image[AMDRDNA_COMPUTE_IMAGE_RESERVED] = QUEUE_COMPUTE_RESERVED;

	/* The end-of-pipe buffer, in 256-byte units, and its size. */
	image[AMDRDNA_COMPUTE_IMAGE_END_LOW] = (uint32_t)(input->end_of_pipe >> 8);
	image[AMDRDNA_COMPUTE_IMAGE_END_HIGH] = (uint32_t)(input->end_of_pipe >> 40);
	word = input->end_control & ~QUEUE_END_SIZE_MASK;
	word |= QUEUE_END_SIZE;
	image[AMDRDNA_COMPUTE_IMAGE_END_CONTROL] = word;

	/* The doorbell, on, from the queue's own source. */
	word = input->doorbell_control & ~QUEUE_DOORBELL_OFFSET_MASK;
	word |= (input->doorbell << QUEUE_DOORBELL_OFFSET_SHIFT) & QUEUE_DOORBELL_OFFSET_MASK;
	word |= QUEUE_DOORBELL_ON;
	word &= ~QUEUE_DOORBELL_SOURCE;
	word &= ~QUEUE_DOORBELL_HIT;
	image[AMDRDNA_COMPUTE_IMAGE_DOORBELL] = word;

	/* No dequeue request and empty pointers. */
	image[AMDRDNA_COMPUTE_IMAGE_DEQUEUE] = 0U;
	image[AMDRDNA_COMPUTE_IMAGE_WRITE_LOW] = 0U;
	image[AMDRDNA_COMPUTE_IMAGE_WRITE_HIGH] = 0U;

	/* Where the image itself lives, and its context (0). */
	image[AMDRDNA_COMPUTE_IMAGE_SELF_LOW] = (uint32_t)input->image & QUEUE_LOW_ALIGN_MASK;
	image[AMDRDNA_COMPUTE_IMAGE_SELF_HIGH] = (uint32_t)(input->image >> 32);
	image[AMDRDNA_COMPUTE_IMAGE_CONTROL] = input->image_control & ~QUEUE_CONTEXT_MASK;

	/* The ring, in 256-byte units. */
	image[AMDRDNA_COMPUTE_IMAGE_RING_LOW] = (uint32_t)(input->ring >> 8);
	image[AMDRDNA_COMPUTE_IMAGE_RING_HIGH] = (uint32_t)(input->ring >> 40);

	/* The ring's size and read-pointer block, unordered dispatch, a privileged kernel queue. */
	word = input->ring_control & ~QUEUE_RING_SIZE_MASK;
	word |= (queue_log2(input->ring_bytes / 4U) - 1U) & QUEUE_RING_SIZE_MASK;
	word &= ~QUEUE_RING_BLOCK_MASK;
	word |= QUEUE_RING_BLOCK_PAGE << QUEUE_RING_BLOCK_SHIFT;
	word |= QUEUE_RING_UNORDERED;
	word &= ~QUEUE_RING_TUNNEL;
	word |= QUEUE_RING_PRIVILEGED;
	word |= QUEUE_RING_KERNEL;
	image[AMDRDNA_COMPUTE_IMAGE_RING_CONTROL] = word;

	/* Where the engine reports its read pointer and polls the write pointer. */
	image[AMDRDNA_COMPUTE_IMAGE_READ_REPORT_LOW] = (uint32_t)input->read_report & QUEUE_LOW_ALIGN_MASK;
	image[AMDRDNA_COMPUTE_IMAGE_READ_REPORT_HIGH] = (uint32_t)(input->read_report >> 32) & QUEUE_REPORT_HIGH_MASK;
	image[AMDRDNA_COMPUTE_IMAGE_WRITE_POLL_LOW] = (uint32_t)input->write_poll & QUEUE_LOW_ALIGN_MASK;
	image[AMDRDNA_COMPUTE_IMAGE_WRITE_POLL_HIGH] = (uint32_t)(input->write_poll >> 32) & QUEUE_REPORT_HIGH_MASK;

	/* The read pointer as the register holds it, and context 0. */
	image[AMDRDNA_COMPUTE_IMAGE_READ] = input->read_pointer;
	image[AMDRDNA_COMPUTE_IMAGE_CONTEXT] = 0U;

	/* The preload size and the least indirect buffer room. */
	word = input->persistent & ~QUEUE_PRELOAD_MASK;
	word |= QUEUE_PRELOAD;
	image[AMDRDNA_COMPUTE_IMAGE_PERSISTENT] = word;
	word = input->buffer_control & ~QUEUE_BUFFER_ROOM_MASK;
	word |= QUEUE_BUFFER_ROOM;
	image[AMDRDNA_COMPUTE_IMAGE_BUFFER_CONTROL] = word;

	/* Normal priorities. */
	image[AMDRDNA_COMPUTE_IMAGE_PIPE_PRIORITY] = 0U;
	image[AMDRDNA_COMPUTE_IMAGE_QUEUE_PRIORITY] = 0U;

	/* Active only when the driver activates the queue itself. */
	image[AMDRDNA_COMPUTE_IMAGE_ACTIVE] = 0U;
	if (input->active)
		image[AMDRDNA_COMPUTE_IMAGE_ACTIVE] = 1U;
}

/*
 * Fills the graphics queue's image from the buffers in input and the
 * values its registers held (read with the queue selected).  The image is
 * active; the scheduler loads it when it attaches the queue.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_gfx_mqd_init).
 */
void
amdrdna_graphics_image(
	const struct amdrdna_graphics_image_input *input,
	uint32_t *image)
{
	uint32_t word;
	uint32_t size;
	unsigned index;

	/* Starts from zero. */
	for (index = 0U; index < AMDRDNA_QUEUE_IMAGE_WORDS; index++)
		image[index] = 0U;

	/* An empty write pointer. */
	image[AMDRDNA_GRAPHICS_IMAGE_WRITE_LOW] = 0U;
	image[AMDRDNA_GRAPHICS_IMAGE_WRITE_HIGH] = 0U;

	/* Where the image itself lives. */
	image[AMDRDNA_GRAPHICS_IMAGE_SELF_LOW] = (uint32_t)input->image & QUEUE_LOW_ALIGN_MASK;
	image[AMDRDNA_GRAPHICS_IMAGE_SELF_HIGH] = (uint32_t)(input->image >> 32);

	/* Context 0, privileged, the default cache policy. */
	word = input->image_control & ~QUEUE_GRAPHICS_CONTEXT_MASK;
	word |= QUEUE_GRAPHICS_PRIVILEGED;
	word &= ~QUEUE_GRAPHICS_POLICY_MASK;
	image[AMDRDNA_GRAPHICS_IMAGE_CONTROL] = word;
	image[AMDRDNA_GRAPHICS_IMAGE_CONTEXT] = 0U;

	/* The normal priority, and the time quantum on. */
	image[AMDRDNA_GRAPHICS_IMAGE_PRIORITY] = input->priority & ~QUEUE_GRAPHICS_PRIORITY_MASK;
	image[AMDRDNA_GRAPHICS_IMAGE_QUANTUM] = input->quantum | QUEUE_GRAPHICS_QUANTUM_ON;

	/* The ring, in 256-byte units. */
	image[AMDRDNA_GRAPHICS_IMAGE_RING_LOW] = (uint32_t)(input->ring >> 8);
	image[AMDRDNA_GRAPHICS_IMAGE_RING_HIGH] = (uint32_t)(input->ring >> 40);

	/* Where the engine reports its read pointer and polls the write pointer. */
	image[AMDRDNA_GRAPHICS_IMAGE_READ_REPORT_LOW] = (uint32_t)input->read_report & QUEUE_LOW_ALIGN_MASK;
	image[AMDRDNA_GRAPHICS_IMAGE_READ_REPORT_HIGH] = (uint32_t)(input->read_report >> 32) & QUEUE_REPORT_HIGH_MASK;
	image[AMDRDNA_GRAPHICS_IMAGE_WRITE_POLL_LOW] = (uint32_t)input->write_poll & QUEUE_LOW_ALIGN_MASK;
	image[AMDRDNA_GRAPHICS_IMAGE_WRITE_POLL_HIGH] = (uint32_t)(input->write_poll >> 32) & QUEUE_REPORT_HIGH_MASK;

	/* The ring's size and block, in the byte order of the host (no swap). */
	size = queue_log2(input->ring_bytes / 4U) - 1U;
	word = input->ring_control & ~QUEUE_GRAPHICS_SIZE_MASK;
	word |= size & QUEUE_GRAPHICS_SIZE_MASK;
	word &= ~QUEUE_GRAPHICS_BLOCK_MASK;
	word |= ((size - 2U) << QUEUE_GRAPHICS_BLOCK_SHIFT) & QUEUE_GRAPHICS_BLOCK_MASK;
	image[AMDRDNA_GRAPHICS_IMAGE_RING_CONTROL] = word;

	/* The doorbell, on. */
	word = input->doorbell_control & ~QUEUE_DOORBELL_OFFSET_MASK;
	word |= (input->doorbell << QUEUE_DOORBELL_OFFSET_SHIFT) & QUEUE_DOORBELL_OFFSET_MASK;
	word |= QUEUE_DOORBELL_ON;
	image[AMDRDNA_GRAPHICS_IMAGE_DOORBELL] = word;

	/* The read pointer as the register holds it, and the queue active. */
	image[AMDRDNA_GRAPHICS_IMAGE_READ] = input->read_pointer;
	image[AMDRDNA_GRAPHICS_IMAGE_ACTIVE] = 1U;
}

/*
 * Reports the control word of the graphics ring the driver runs without
 * the scheduler: its size as log2 of its 8-byte units and the block two
 * less, in the byte order of the host.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_cp_gfx_resume).
 */
uint32_t
amdrdna_graphics_ring_word(
	uint32_t ring_bytes)
{
	uint32_t size;
	uint32_t word;

	/* The size and the block. */
	size = queue_log2(ring_bytes / 8U);
	word = size & QUEUE_GRAPHICS_SIZE_MASK;
	word |= ((size - 2U) << QUEUE_GRAPHICS_BLOCK_SHIFT) & QUEUE_GRAPHICS_BLOCK_MASK;

	/* Succeeded: the control word. */
	return word;
}

/*
 * Reports the queue select word that points the queue registers at one
 * queue (and the shader memory registers at one context).
 */
uint32_t
amdrdna_queue_select_word(
	const struct amdrdna_queue_place *place,
	unsigned context)
{
	uint32_t word;

	/* The pipe, the engine, the context and the queue. */
	word = place->pipe << AMDRDNA_GC_SELECT_PIPE_SHIFT;
	word |= place->engine << AMDRDNA_GC_SELECT_ENGINE_SHIFT;
	word |= (uint32_t)context << AMDRDNA_GC_SELECT_CONTEXT_SHIFT;
	word |= place->queue << AMDRDNA_GC_SELECT_QUEUE_SHIFT;

	/* Succeeded: the select word. */
	return word;
}

/*
 * Reports the word that names the scheduler's own queue to the run-list
 * controller, from the word the register held.
 */
uint32_t
amdrdna_scheduler_queue_word(
	uint32_t old,
	const struct amdrdna_queue_place *place)
{
	uint32_t word;

	/* Keeps the upper bytes, and names the queue as valid. */
	word = old & QUEUE_SCHEDULER_KEEP_MASK;
	word |= place->engine << QUEUE_SCHEDULER_ENGINE_SHIFT;
	word |= place->pipe << QUEUE_SCHEDULER_PIPE_SHIFT;
	word |= place->queue;
	word |= QUEUE_SCHEDULER_VALID;

	/* Succeeded: the word to write. */
	return word;
}

/*
 * Reports the scheduler's resource mask of compute queues: one bit for
 * each queue in places (the places must be on a compute engine).
 */
uint64_t
amdrdna_queue_resource_mask(
	const struct amdrdna_queue_place *places,
	unsigned count)
{
	uint64_t mask;
	unsigned bit;
	unsigned index;

	/* One bit per queue. */
	mask = 0U;
	for (index = 0U; index < count; index++) {
		bit = (places[index].engine - AMDRDNA_ENGINE_COMPUTE_FIRST) * QUEUE_RESOURCE_PER_ENGINE;
		bit += places[index].pipe * QUEUE_RESOURCE_PER_PIPE;
		bit += places[index].queue;
		mask |= (uint64_t)1U << bit;
	}

	/* Succeeded: the mask. */
	return mask;
}

/* Reports log2 of a power of two, or of the next power of two above. */
static unsigned
queue_log2(
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
