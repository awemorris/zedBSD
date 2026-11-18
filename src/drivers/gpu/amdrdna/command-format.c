/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The command words the amdrdna driver puts on the GPU's rings.
 *
 * The command processor reads type-3 packets: a header with the packet's
 * opcode and the count of its body words, then the body.  The copy engine
 * reads its own packets, whose first word holds an opcode and a
 * sub-opcode.  Every function here writes one packet into a word array
 * and reports how many words it wrote; nothing touches the hardware, so
 * the host tests check the same words the kernel writes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * The value write's control word: the destination in bits 11:8 (0 a
 * register, 5 memory written directly), the confirmation bit (20) that
 * makes the engine wait until the write landed.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/nvd.h
 * (WRITE_DATA_DST_SEL, WR_CONFIRM).
 */
#define COMMAND_WRITE_TO_REGISTER	0x00000000U
#define COMMAND_WRITE_TO_MEMORY		0x00000500U
#define COMMAND_WRITE_CONFIRM		0x00100000U

/*
 * The engine that runs a value write or a value wait: the fetcher (the
 * graphics ring's prefetch parser, 1 at bit 30 of a write and at bit 8 of
 * a wait) instead of the micro engine.  The graphics ring writes a
 * context's table and waits for its drop on the fetcher, which reads the
 * indirect buffers that follow.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/nvd.h
 * (WRITE_DATA_ENGINE_SEL, WAIT_REG_MEM_ENGINE) and gfx_v10_0.c
 * (gfx_v10_0_ring_emit_wreg, gfx_v10_0_ring_emit_reg_write_reg_wait: the
 * graphics ring uses the prefetch parser).
 */
#define COMMAND_WRITE_BY_FETCHER	0x40000000U
#define COMMAND_WAIT_BY_FETCHER		0x00000100U

/*
 * The value wait's operation (bits 7:6: 0 wait only, 1 write the first
 * register then wait on the second) and the poll interval of a register
 * wait (0x20); its comparison, space and memory poll interval are in
 * amdrdna-hw.h.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/nvd.h
 * (WAIT_REG_MEM_OPERATION) and gfx_v10_0.c (gfx_v10_0_wait_reg_mem and
 * its caller gfx_v10_0_ring_emit_reg_write_reg_wait: the poll interval).
 */
#define COMMAND_WAIT_AFTER_WRITE	0x00000040U
#define COMMAND_WAIT_REGISTER_POLL	0x00000020U

/*
 * The event fence's first control word: the cache flush and timestamp
 * event (0x14) with event index 5, the level-2 write-back (21), the
 * metadata write-back and invalidate (12, 13), the sequential order of
 * the cache actions (22) and the bypass cache policy (3 at bit 25); and
 * its second: the low 32 bits of data (1 at bit 29) and the interrupt
 * after the write is confirmed (2 at bit 24).
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/nvd.h
 * (PACKET3_RELEASE_MEM_EVENT_TYPE, PACKET3_RELEASE_MEM_EVENT_INDEX,
 * PACKET3_RELEASE_MEM_GCR_GLM_WB, PACKET3_RELEASE_MEM_GCR_GLM_INV,
 * PACKET3_RELEASE_MEM_GCR_GL2_WB, PACKET3_RELEASE_MEM_GCR_SEQ,
 * PACKET3_RELEASE_MEM_CACHE_POLICY, PACKET3_RELEASE_MEM_DATA_SEL,
 * PACKET3_RELEASE_MEM_INT_SEL), navi10_enum.h
 * (CACHE_FLUSH_AND_INV_TS_EVENT) and gfx_v10_0.c
 * (gfx_v10_0_ring_emit_fence).
 * derived: 0x14 | 5 << 8 | 1 << 12 | 1 << 13 | 1 << 21 | 1 << 22 | 3 << 25
 * = 0x06603514.
 */
#define COMMAND_FENCE_EVENT		0x06603514U
#define COMMAND_FENCE_INTERRUPT		0x02000000U

/*
 * The cache sync's cache control: every instruction, scalar, vector,
 * level-1, metadata and level-2 cache invalidated, the metadata and
 * level-2 caches written back (its size and poll interval are in
 * amdrdna-hw.h).
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/nvd.h
 * (the PACKET3_ACQUIRE_MEM_GCR_CNTL fields) and gfx_v10_0.c
 * (gfx_v10_0_emit_mem_sync).
 * derived: 1 | 1 << 4 | 1 << 5 | 1 << 7 | 1 << 8 | 1 << 9 | 1 << 14 |
 * 1 << 15 = 0xc3b1.
 */
#define COMMAND_SYNC_CACHES		0x0000c3b1U

/*
 * The memory move's control word, with both addresses in memory: the
 * engine waits for the move before going on (bit 31); and its command
 * word's byte count (bits 25:0).
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/nvd.h
 * (PACKET3_DMA_DATA_CP_SYNC, the DMA_DATA command word) and Mesa 25.3.6
 * src/amd/vulkan/radv_cp_dma.c (radv_cs_emit_cp_dma: the 26-bit count of
 * GFX9 and later).
 */
#define COMMAND_MOVE_SYNC		0x80000000U
#define COMMAND_MOVE_COUNT_MASK		0x03ffffffU

/*
 * The memory move's control word fields that read the source and write
 * the destination through the level-2 cache (3 at bits 30:29 and 21:20),
 * the way a move inside a session's address space must go: a move by the
 * plain address does not reach a session's pages.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/nvd.h
 * (PACKET3_DMA_DATA_SRC_SEL, PACKET3_DMA_DATA_DST_SEL: 3 is the address
 * through L2) and Mesa 25.3.6 src/amd/vulkan/radv_cp_dma.c
 * (radv_cs_emit_cp_dma: GFX9 and later move through L2).
 */
#define COMMAND_MOVE_THROUGH_CACHE	0x60300000U

/*
 * The indirect buffer's control word: the size in dwords (19:0), the
 * valid bit a compute queue needs (23) and the context (27:24); and the
 * low address word's dword alignment.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/nvd.h
 * (INDIRECT_BUFFER_VALID) and gfx_v10_0.c (gfx_v10_0_ring_emit_ib_gfx,
 * gfx_v10_0_ring_emit_ib_compute).
 */
#define COMMAND_INDIRECT_SIZE_MASK	0x000fffffU
#define COMMAND_INDIRECT_VALID		0x00800000U
#define COMMAND_INDIRECT_CONTEXT_SHIFT	24U
#define COMMAND_INDIRECT_ALIGN_MASK	0xfffffffcU

/*
 * The state bracket's begin and end of the clear state, the context
 * loads' word that updates the load enables, and the partition base's
 * index (the constant engine's partition, 3) and its two halves.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/nvd.h
 * (PACKET3_PREAMBLE_BEGIN_CLEAR_STATE, PACKET3_PREAMBLE_END_CLEAR_STATE,
 * CE_PARTITION_BASE) and gfx_v10_0.c (gfx_v10_0_cp_gfx_start: 0x80000000
 * twice, the partition 0x8000 and 0x8000).
 */
#define COMMAND_BRACKET_BEGIN		0x20000000U
#define COMMAND_BRACKET_END		0x30000000U
#define COMMAND_LOADS_UPDATE		0x80000000U
#define COMMAND_PARTITION_CONSTANT	3U
#define COMMAND_PARTITION_HALF		0x00008000U

/*
 * The queue attach packet's first word: the queue (bit 13 on), the pipe
 * (16 on), the engine (18 on: 0 for the first compute engine, 1 for the
 * graphics engine or the second compute engine), the engine kind (26
 * on) and one queue (1 at bit 29); its doorbell word (the doorbell from
 * bit 2).  The detach packet's first word: the action (1:0), the engine
 * kind (26 on) and one queue; its doorbell word.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/nvd.h
 * (PACKET3_MAP_QUEUES_QUEUE, PACKET3_MAP_QUEUES_PIPE, PACKET3_MAP_QUEUES_ME,
 * PACKET3_MAP_QUEUES_ENGINE_SEL, PACKET3_MAP_QUEUES_NUM_QUEUES,
 * PACKET3_MAP_QUEUES_DOORBELL_OFFSET, PACKET3_UNMAP_QUEUES_ACTION,
 * PACKET3_UNMAP_QUEUES_ENGINE_SEL, PACKET3_UNMAP_QUEUES_NUM_QUEUES,
 * PACKET3_UNMAP_QUEUES_DOORBELL_OFFSET0) and gfx_v10_0.c
 * (gfx10_kiq_map_queues: the engine field is 0 for engine 1 and 1
 * otherwise).
 */
#define COMMAND_QUEUE_SHIFT		13U
#define COMMAND_QUEUE_PIPE_SHIFT	16U
#define COMMAND_QUEUE_ENGINE_SHIFT	18U
#define COMMAND_QUEUE_KIND_SHIFT	26U
#define COMMAND_QUEUE_ONE		0x20000000U
#define COMMAND_QUEUE_DOORBELL_SHIFT	2U

/*
 * The copy engine's packets: the opcode in bits 7:0 and the sub-opcode in
 * 15:8 of the first word; the opcodes of nothing, the linear copy, the
 * linear write, the fence, the trap and the constant fill; the fence's
 * memory type (3, uncached, at bit 16); a count of bytes or words less
 * one.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h (SDMA_OP_NOP,
 * SDMA_OP_COPY, SDMA_OP_WRITE, SDMA_OP_FENCE, SDMA_OP_TRAP,
 * SDMA_OP_CONST_FILL, SDMA_SUBOP_COPY_LINEAR, SDMA_SUBOP_WRITE_LINEAR,
 * SDMA_PKT_FENCE_HEADER_mtype_shift) and sdma_v5_0.c
 * (sdma_v5_0_ring_emit_fence: MTYPE 3; sdma_v5_0_emit_copy_buffer and
 * sdma_v5_0_emit_fill_buffer: the count less one).
 */
#define COMMAND_COPY_OP_NOTHING		0U
#define COMMAND_COPY_OP_COPY		1U
#define COMMAND_COPY_OP_WRITE		2U
#define COMMAND_COPY_OP_FENCE		5U
#define COMMAND_COPY_OP_TRAP		6U
#define COMMAND_COPY_OP_FILL		11U
#define COMMAND_COPY_FENCE_UNCACHED	0x00030000U
#define COMMAND_COPY_COUNT_MASK		0x003fffffU

/* Reports the one-word filler that pads a command ring. */
uint32_t
amdrdna_pm4_filler(void)
{
	uint32_t word;

	/* A nothing packet whose count says it has no body. */
	word = AMDRDNA_PM4_TYPE3;
	word |= AMDRDNA_PM4_FILLER_COUNT << AMDRDNA_PM4_COUNT_SHIFT;
	word |= AMDRDNA_PM4_OP_NOTHING << AMDRDNA_PM4_OPCODE_SHIFT;

	/* Succeeded: the filler word. */
	return word;
}

/*
 * Writes a packet that sets count consecutive registers of one space,
 * from the absolute register index first on, to values.  Reports the
 * words written: two and the values.
 */
unsigned
amdrdna_pm4_registers(
	uint32_t *words,
	enum amdrdna_register_space space,
	uint32_t first,
	const uint32_t *values,
	unsigned count,
	bool compute)
{
	uint32_t opcode;
	uint32_t base;
	unsigned index;

	/* The opcode and the first register of the space. */
	opcode = AMDRDNA_PM4_OP_USER_REGISTERS;
	base = AMDRDNA_PM4_USER_FIRST;
	if (space == AMDRDNA_SPACE_CONTEXT) {
		opcode = AMDRDNA_PM4_OP_CONTEXT_REGISTERS;
		base = AMDRDNA_PM4_CONTEXT_FIRST;
	} else if (space == AMDRDNA_SPACE_SHADER) {
		opcode = AMDRDNA_PM4_OP_SHADER_REGISTERS;
		base = AMDRDNA_PM4_SHADER_FIRST;
	}

	/* The header, and the first register relative to its space. */
	words[0] = amdrdna_pm4_header(opcode, count + 1U, compute);
	words[1] = first - base;

	/* The values, one per register. */
	for (index = 0U; index < count; index++)
		words[2U + index] = values[index];

	/* Succeeded: the header, the offset and the values. */
	return 2U + count;
}

/*
 * Writes a packet that writes one 32-bit value to memory and waits until
 * the write landed.  Reports the words written.
 */
unsigned
amdrdna_pm4_write_memory(
	uint32_t *words,
	uint64_t address,
	uint32_t value,
	bool compute)
{
	/* The header and the control word. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_WRITE_VALUE, 4U, compute);
	words[1] = COMMAND_WRITE_TO_MEMORY | COMMAND_WRITE_CONFIRM;

	/* The address and the value. */
	words[2] = (uint32_t)address;
	words[3] = (uint32_t)(address >> 32);
	words[4] = value;

	/* Succeeded: five words. */
	return AMDRDNA_PM4_WRITE_WORDS;
}

/*
 * Writes a packet that writes one register (an absolute index) and waits
 * until the write landed.  Reports the words written.
 */
unsigned
amdrdna_pm4_write_register(
	uint32_t *words,
	uint32_t index,
	uint32_t value,
	bool compute)
{
	/* The header and the control word. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_WRITE_VALUE, 4U, compute);
	words[1] = COMMAND_WRITE_TO_REGISTER | COMMAND_WRITE_CONFIRM;

	/* The register, a zero high word, and the value. */
	words[2] = index;
	words[3] = 0U;
	words[4] = value;

	/* Succeeded: five words. */
	return AMDRDNA_PM4_WRITE_WORDS;
}

/*
 * Writes the fence that ends a submission: once everything before it is
 * done and the caches are written back, the engine writes value to the
 * 32-bit word at address, and raises the end-of-pipe interrupt when
 * asked.  Reports the words written.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_ring_emit_fence: the
 * packet's eight words).
 */
unsigned
amdrdna_pm4_fence(
	uint32_t *words,
	uint64_t address,
	uint32_t value,
	bool interrupt,
	bool compute)
{
	/* The header, the event and its caches. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_EVENT_FENCE, 7U, compute);
	words[1] = COMMAND_FENCE_EVENT;

	/* The low 32 bits of data, with the interrupt when asked. */
	words[2] = AMDRDNA_PM4_FENCE_DATA_LOW;
	if (interrupt)
		words[2] |= COMMAND_FENCE_INTERRUPT;

	/* The address, the value (its high word 0) and a zero word. */
	words[3] = (uint32_t)address;
	words[4] = (uint32_t)(address >> 32);
	words[5] = value;
	words[6] = 0U;
	words[7] = 0U;

	/* Succeeded: eight words. */
	return AMDRDNA_PM4_FENCE_WORDS;
}

/*
 * Writes a packet that copies the GPU's timestamp counter, both words, to
 * memory at address when the command processor reaches it.  Reports the
 * words written.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_query.c
 * (radv_write_timestamp: the counter copied at the top of the pipe).
 */
unsigned
amdrdna_pm4_stamp(
	uint32_t *words,
	uint64_t address,
	bool compute)
{
	/* The header and the copy of the counter to memory, confirmed. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_COPY_WORDS, AMDRDNA_PM4_STAMP_WORDS - 1U, compute);
	words[1] = AMDRDNA_PM4_COPY_COUNTER_TO_MEMORY;

	/* The counter is the source: no source address. */
	words[2] = 0U;
	words[3] = 0U;

	/* Where the two words go. */
	words[4] = (uint32_t)address;
	words[5] = (uint32_t)(address >> 32);

	/* Succeeded: six words. */
	return AMDRDNA_PM4_STAMP_WORDS;
}

/*
 * Writes an event fence that writes the GPU's timestamp counter, both
 * words, to memory at address once everything before it has finished,
 * without an interrupt.  Reports the words written.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_query.c
 * (radv_write_timestamp: the counter written at the bottom of the pipe).
 */
unsigned
amdrdna_pm4_stamp_done(
	uint32_t *words,
	uint64_t address,
	bool compute)
{
	/* The header, the fence's event and its caches, and the counter as its data. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_EVENT_FENCE, 7U, compute);
	words[1] = COMMAND_FENCE_EVENT;
	words[2] = AMDRDNA_PM4_FENCE_DATA_COUNTER;

	/* The address, then words the counter's data does not use. */
	words[3] = (uint32_t)address;
	words[4] = (uint32_t)(address >> 32);
	words[5] = 0U;
	words[6] = 0U;
	words[7] = 0U;

	/* Succeeded: eight words. */
	return AMDRDNA_PM4_FENCE_WORDS;
}

/*
 * Writes a packet that invalidates the GPU's caches and writes the dirty
 * ones back, so what follows reads memory as the CPU and the earlier
 * commands left it.  Reports the words written.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_emit_mem_sync).
 */
unsigned
amdrdna_pm4_cache_sync(
	uint32_t *words,
	bool compute)
{
	/* The header and a zero coherence control. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_CACHE_SYNC, 7U, compute);
	words[1] = 0U;

	/* The whole address space. */
	words[2] = AMDRDNA_PM4_SYNC_SIZE_LOW;
	words[3] = AMDRDNA_PM4_SYNC_SIZE_HIGH;
	words[4] = 0U;
	words[5] = 0U;

	/* The poll interval and the caches. */
	words[6] = AMDRDNA_PM4_SYNC_POLL;
	words[7] = COMMAND_SYNC_CACHES;

	/* Succeeded: eight words. */
	return AMDRDNA_PM4_SYNC_WORDS;
}

/*
 * Writes a packet that has the command processor move bytes from one
 * memory address to another and wait until the move is done.  Reports
 * the words written.
 *
 * Procedure referenced: Mesa 25.3.6 src/amd/vulkan/radv_cp_dma.c
 * (radv_cs_emit_cp_dma).
 */
unsigned
amdrdna_pm4_move(
	uint32_t *words,
	uint64_t source,
	uint64_t destination,
	uint32_t bytes)
{
	/* The header and the control word. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_MOVE, 6U, false);
	words[1] = COMMAND_MOVE_SYNC;

	/* The source and the destination. */
	words[2] = (uint32_t)source;
	words[3] = (uint32_t)(source >> 32);
	words[4] = (uint32_t)destination;
	words[5] = (uint32_t)(destination >> 32);

	/* The byte count. */
	words[6] = bytes & COMMAND_MOVE_COUNT_MASK;

	/* Succeeded: seven words. */
	return AMDRDNA_PM4_MOVE_WORDS;
}

/*
 * Writes a packet that has the command processor move bytes between two
 * addresses of a session's space, through the level-2 cache, and wait
 * until the move is done.  Reports the words written.
 *
 * Procedure referenced: Mesa 25.3.6 src/amd/vulkan/radv_cp_dma.c
 * (radv_cs_emit_cp_dma).
 */
unsigned
amdrdna_pm4_move_cached(
	uint32_t *words,
	uint64_t source,
	uint64_t destination,
	uint32_t bytes)
{
	unsigned count;

	/* The plain move, then its control word read and written through the cache. */
	count = amdrdna_pm4_move(words, source, destination, bytes);
	words[1] |= COMMAND_MOVE_THROUGH_CACHE;

	/* Succeeded: seven words. */
	return count;
}

/*
 * Writes a packet that launches x by y by z workgroups of the compute
 * program the shader registers name.  Reports the words written.
 */
unsigned
amdrdna_pm4_dispatch(
	uint32_t *words,
	uint32_t x,
	uint32_t y,
	uint32_t z,
	uint32_t initiator)
{
	/* The header. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_DISPATCH, 4U, true);

	/* The workgroup counts and the launch word. */
	words[1] = x;
	words[2] = y;
	words[3] = z;
	words[4] = initiator;

	/* Succeeded: five words. */
	return AMDRDNA_PM4_DISPATCH_WORDS;
}

/*
 * Writes the scheduler's resource packet: the compute queues it may run
 * (queue_mask), no context of its own and no cleaner program.  Reports
 * the words written.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx10_kiq_set_resources).
 */
unsigned
amdrdna_pm4_queue_resources(
	uint32_t *words,
	uint64_t queue_mask)
{
	/* The header and the scheduler's own queue kind, no contexts. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_QUEUE_RESOURCES, 7U, false);
	words[1] = 0U;

	/* The queues. */
	words[2] = (uint32_t)queue_mask;
	words[3] = (uint32_t)(queue_mask >> 32);

	/* No cleaner program, and no ordered-append or data-share heap. */
	words[4] = 0U;
	words[5] = 0U;
	words[6] = 0U;
	words[7] = 0U;

	/* Succeeded: eight words. */
	return AMDRDNA_PM4_RESOURCES_WORDS;
}

/*
 * Writes the scheduler's packet that attaches one queue: its place, the
 * engine kind (AMDRDNA_QUEUE_ENGINE_*), its doorbell, the address of its
 * queue image and of its write pointer's copy.  Reports the words
 * written.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx10_kiq_map_queues).
 */
unsigned
amdrdna_pm4_queue_attach(
	uint32_t *words,
	const struct amdrdna_queue_place *place,
	uint32_t engine,
	uint32_t doorbell,
	uint64_t image,
	uint64_t write_pointer)
{
	uint32_t engine_field;

	/* The engine field is 0 for the first compute engine and 1 for the others. */
	engine_field = 1U;
	if (place->engine == AMDRDNA_ENGINE_COMPUTE_FIRST)
		engine_field = 0U;

	/* The header and the queue's place. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_QUEUE_ATTACH, 6U, false);
	words[1] = place->queue << COMMAND_QUEUE_SHIFT;
	words[1] |= place->pipe << COMMAND_QUEUE_PIPE_SHIFT;
	words[1] |= engine_field << COMMAND_QUEUE_ENGINE_SHIFT;
	words[1] |= engine << COMMAND_QUEUE_KIND_SHIFT;
	words[1] |= COMMAND_QUEUE_ONE;

	/* The doorbell. */
	words[2] = doorbell << COMMAND_QUEUE_DOORBELL_SHIFT;

	/* The queue image and the write pointer's copy. */
	words[3] = (uint32_t)image;
	words[4] = (uint32_t)(image >> 32);
	words[5] = (uint32_t)write_pointer;
	words[6] = (uint32_t)(write_pointer >> 32);

	/* Succeeded: seven words. */
	return AMDRDNA_PM4_ATTACH_WORDS;
}

/*
 * Writes the scheduler's packet that detaches the queue behind a doorbell:
 * the engine kind and the action (AMDRDNA_QUEUE_DETACH_*).  Reports the
 * words written.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx10_kiq_unmap_queues).
 */
unsigned
amdrdna_pm4_queue_detach(
	uint32_t *words,
	uint32_t engine,
	uint32_t action,
	uint32_t doorbell)
{
	/* The header, the action, the engine kind, one queue. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_QUEUE_DETACH, 5U, false);
	words[1] = action;
	words[1] |= engine << COMMAND_QUEUE_KIND_SHIFT;
	words[1] |= COMMAND_QUEUE_ONE;

	/* The doorbell, and no trail fence. */
	words[2] = doorbell << COMMAND_QUEUE_DOORBELL_SHIFT;
	words[3] = 0U;
	words[4] = 0U;
	words[5] = 0U;

	/* Succeeded: six words. */
	return AMDRDNA_PM4_DETACH_WORDS;
}

/*
 * Writes a packet that has the engine run count words from an indirect
 * buffer at address, in a context; a compute queue's packet carries the
 * valid bit.  Reports the words written.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_ring_emit_ib_gfx,
 * gfx_v10_0_ring_emit_ib_compute).
 */
unsigned
amdrdna_pm4_indirect(
	uint32_t *words,
	uint64_t address,
	uint32_t count,
	unsigned context,
	bool compute)
{
	/* The header and the buffer's address. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_INDIRECT, 3U, false);
	words[1] = (uint32_t)address & COMMAND_INDIRECT_ALIGN_MASK;
	words[2] = (uint32_t)(address >> 32);

	/* The size and the context, and the valid bit of a compute queue. */
	words[3] = count & COMMAND_INDIRECT_SIZE_MASK;
	words[3] |= (uint32_t)context << COMMAND_INDIRECT_CONTEXT_SHIFT;
	if (compute)
		words[3] |= COMMAND_INDIRECT_VALID;

	/* Succeeded: four words. */
	return AMDRDNA_PM4_INDIRECT_WORDS;
}

/*
 * Writes the state bracket's begin (begin true) or end of the clear
 * state.  Reports the words written.
 */
unsigned
amdrdna_pm4_bracket(
	uint32_t *words,
	bool begin)
{
	/* The header and the bracket. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_STATE_BRACKET, 1U, false);
	words[1] = COMMAND_BRACKET_END;
	if (begin)
		words[1] = COMMAND_BRACKET_BEGIN;

	/* Succeeded: two words. */
	return 2U;
}

/*
 * Writes the packet that tells the engine which state the next commands
 * load: the load enables are updated, nothing is shadowed.  Reports the
 * words written.
 */
unsigned
amdrdna_pm4_context_loads(
	uint32_t *words)
{
	/* The header and the two control words. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_CONTEXT_LOADS, 2U, false);
	words[1] = COMMAND_LOADS_UPDATE;
	words[2] = COMMAND_LOADS_UPDATE;

	/* Succeeded: three words. */
	return 3U;
}

/*
 * Writes the packet that resets the context state to the clear state.
 * Reports the words written.
 */
unsigned
amdrdna_pm4_state_reset(
	uint32_t *words)
{
	/* The header and a zero word. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_STATE_RESET, 1U, false);
	words[1] = 0U;

	/* Succeeded: two words. */
	return 2U;
}

/*
 * Writes the packet that places the constant engine's partition.
 * Reports the words written.
 */
unsigned
amdrdna_pm4_partition(
	uint32_t *words)
{
	/* The header, the partition's index and its two halves. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_PARTITION, 3U, false);
	words[1] = COMMAND_PARTITION_CONSTANT;
	words[2] = COMMAND_PARTITION_HALF;
	words[3] = COMMAND_PARTITION_HALF;

	/* Succeeded: four words. */
	return 4U;
}

/*
 * Writes a packet that has the graphics ring's fetcher write one register
 * (an absolute index) and wait until the write landed.  Reports the words
 * written.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_ring_emit_wreg, the
 * graphics ring's case).
 */
unsigned
amdrdna_pm4_fetcher_write_register(
	uint32_t *words,
	uint32_t index,
	uint32_t value)
{
	/* The header and the control word: a register, by the fetcher, confirmed. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_WRITE_VALUE, 4U, false);
	words[1] = COMMAND_WRITE_TO_REGISTER | COMMAND_WRITE_BY_FETCHER | COMMAND_WRITE_CONFIRM;

	/* The register, a zero high word, and the value. */
	words[2] = index;
	words[3] = 0U;
	words[4] = value;

	/* Succeeded: five words. */
	return AMDRDNA_PM4_WRITE_WORDS;
}

/*
 * Writes a packet that writes value to the register request and then
 * waits until the register done, masked by mask, equals value masked the
 * same way: the way a ring asks a memory hub to drop a context's cached
 * translations and waits for the hub's answer.  fetcher runs it on the
 * graphics ring's fetcher.  Reports the words written.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c
 * (gfx_v10_0_ring_emit_reg_write_reg_wait, gfx_v10_0_wait_reg_mem).
 */
unsigned
amdrdna_pm4_write_and_wait(
	uint32_t *words,
	uint32_t request,
	uint32_t done,
	uint32_t value,
	uint32_t mask,
	bool fetcher)
{
	/* The header and the control word: write then wait, registers, equal. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_WAIT_VALUE, 6U, false);
	words[1] = AMDRDNA_PM4_WAIT_EQUAL | COMMAND_WAIT_AFTER_WRITE;
	if (fetcher)
		words[1] |= COMMAND_WAIT_BY_FETCHER;

	/* The register written and the register waited on. */
	words[2] = request;
	words[3] = done;

	/* The value, the mask and the poll interval. */
	words[4] = value;
	words[5] = mask;
	words[6] = COMMAND_WAIT_REGISTER_POLL;

	/* Succeeded: seven words. */
	return AMDRDNA_PM4_WAIT_WORDS;
}

/*
 * Writes a packet that waits until a register (an absolute index), masked
 * by mask, equals value.  Reports the words written.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_ring_emit_reg_wait,
 * gfx_v10_0_wait_reg_mem).
 */
unsigned
amdrdna_pm4_wait_register(
	uint32_t *words,
	uint32_t index,
	uint32_t value,
	uint32_t mask)
{
	/* The header and the control word: wait only, a register, equal. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_WAIT_VALUE, 6U, false);
	words[1] = AMDRDNA_PM4_WAIT_EQUAL;

	/* The register, no second word. */
	words[2] = index;
	words[3] = 0U;

	/* The value, the mask and the poll interval. */
	words[4] = value;
	words[5] = mask;
	words[6] = COMMAND_WAIT_REGISTER_POLL;

	/* Succeeded: seven words. */
	return AMDRDNA_PM4_WAIT_WORDS;
}

/*
 * Writes a packet that waits until the 32-bit word at address equals
 * value: a ring waiting for its own fence, so the work before it has
 * ended before what follows starts.  fetcher runs it on the graphics
 * ring's fetcher.  Reports the words written.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c
 * (gfx_v10_0_ring_emit_pipeline_sync, gfx_v10_0_wait_reg_mem).
 */
unsigned
amdrdna_pm4_wait_memory(
	uint32_t *words,
	uint64_t address,
	uint32_t value,
	bool fetcher)
{
	/* The header and the control word: wait only, memory, equal. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_WAIT_VALUE, 6U, false);
	words[1] = AMDRDNA_PM4_WAIT_EQUAL | AMDRDNA_PM4_WAIT_IN_MEMORY;
	if (fetcher)
		words[1] |= COMMAND_WAIT_BY_FETCHER;

	/* The word's address, dword aligned. */
	words[2] = (uint32_t)address & COMMAND_INDIRECT_ALIGN_MASK;
	words[3] = (uint32_t)(address >> 32);

	/* The value, every bit compared, and the poll interval. */
	words[4] = value;
	words[5] = 0xffffffffU;
	words[6] = AMDRDNA_PM4_WAIT_MEMORY_POLL;

	/* Succeeded: seven words. */
	return AMDRDNA_PM4_WAIT_WORDS;
}

/*
 * Writes the packet that holds the graphics ring's fetcher until the
 * micro engine has caught up with it, so the fetcher does not read an
 * indirect buffer through translations dropped behind it.  Reports the
 * words written.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_ring_emit_vm_flush).
 */
unsigned
amdrdna_pm4_fetcher_sync(
	uint32_t *words)
{
	/* The header and a zero word. */
	words[0] = amdrdna_pm4_header(AMDRDNA_PM4_OP_FETCHER_SYNC, 1U, false);
	words[1] = 0U;

	/* Succeeded: two words. */
	return AMDRDNA_PM4_FETCH_SYNC_WORDS;
}

/* Reports the copy engine's one-word filler. */
uint32_t
amdrdna_copy_filler(void)
{
	/* The nothing packet with no body. */
	return COMMAND_COPY_OP_NOTHING;
}

/*
 * Writes the copy engine's packet that writes one 32-bit value to memory.
 * Reports the words written.
 */
unsigned
amdrdna_copy_write(
	uint32_t *words,
	uint64_t address,
	uint32_t value)
{
	/* The linear write. */
	words[0] = COMMAND_COPY_OP_WRITE;

	/* The address, one word, the value. */
	words[1] = (uint32_t)address;
	words[2] = (uint32_t)(address >> 32);
	words[3] = 0U;
	words[4] = value;

	/* Succeeded: five words. */
	return AMDRDNA_COPY_WRITE_WORDS;
}

/*
 * Writes the copy engine's packet that copies bytes from one address to
 * another, at most 4 MiB.  Reports the words written.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c (sdma_v5_0_emit_copy_buffer).
 */
unsigned
amdrdna_copy_linear(
	uint32_t *words,
	uint64_t source,
	uint64_t destination,
	uint32_t bytes)
{
	/* The linear copy. */
	words[0] = COMMAND_COPY_OP_COPY;

	/* The byte count less one, and no byte swap. */
	words[1] = (bytes - 1U) & COMMAND_COPY_COUNT_MASK;
	words[2] = 0U;

	/* The source and the destination. */
	words[3] = (uint32_t)source;
	words[4] = (uint32_t)(source >> 32);
	words[5] = (uint32_t)destination;
	words[6] = (uint32_t)(destination >> 32);

	/* Succeeded: seven words. */
	return AMDRDNA_COPY_LINEAR_WORDS;
}

/*
 * Writes the copy engine's packet that fills bytes at an address with a
 * 32-bit value, at most 4 MiB, a whole number of words.  Reports the words
 * written.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c (sdma_v5_0_emit_fill_buffer).
 */
unsigned
amdrdna_copy_fill(
	uint32_t *words,
	uint64_t destination,
	uint32_t value,
	uint32_t bytes)
{
	/* The constant fill. */
	words[0] = COMMAND_COPY_OP_FILL;

	/* The destination, the value, the byte count less one. */
	words[1] = (uint32_t)destination;
	words[2] = (uint32_t)(destination >> 32);
	words[3] = value;
	words[4] = (bytes - 1U) & COMMAND_COPY_COUNT_MASK;

	/* Succeeded: five words. */
	return AMDRDNA_COPY_FILL_WORDS;
}

/*
 * Writes the copy engine's fence: value to the 32-bit word at address once
 * everything before it is done, and the trap that raises its interrupt
 * when asked.  Reports the words written.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c (sdma_v5_0_ring_emit_fence).
 */
unsigned
amdrdna_copy_fence(
	uint32_t *words,
	uint64_t address,
	uint32_t value,
	bool interrupt)
{
	/* The fence, uncached. */
	words[0] = COMMAND_COPY_OP_FENCE | COMMAND_COPY_FENCE_UNCACHED;
	words[1] = (uint32_t)address;
	words[2] = (uint32_t)(address >> 32);
	words[3] = value;

	/* Without the interrupt, the fence is all. */
	if (!interrupt)
		return 4U;

	/* The trap, with a zero context. */
	words[4] = COMMAND_COPY_OP_TRAP;
	words[5] = 0U;

	/* Succeeded: the fence and the trap. */
	return AMDRDNA_COPY_FENCE_WORDS;
}

/* Builds a type-3 header for an opcode and its body's word count. */
uint32_t
amdrdna_pm4_header(
	uint32_t opcode,
	unsigned body_words,
	bool compute)
{
	uint32_t word;

	/* The type, the count less one and the opcode. */
	word = AMDRDNA_PM4_TYPE3;
	word |= ((uint32_t)(body_words - 1U) & AMDRDNA_PM4_COUNT_MASK) << AMDRDNA_PM4_COUNT_SHIFT;
	word |= opcode << AMDRDNA_PM4_OPCODE_SHIFT;

	/* The compute pipeline's mark. */
	if (compute)
		word |= AMDRDNA_PM4_COMPUTE;

	/* Succeeded: the header. */
	return word;
}
