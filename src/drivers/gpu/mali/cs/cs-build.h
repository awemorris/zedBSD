/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Mali command stream's instructions, and a writer of them.
 *
 * A command stream is a run of 64-bit instructions the firmware's
 * command stream front end reads through the group's address space: the
 * opcode in the top byte, the operands below.  The front end has a file
 * of 32-bit work registers; an instruction names registers by number, a
 * 64-bit operand by the even register of a pair.  An instruction that
 * starts work on the GPU's endpoints, or that moves memory, runs
 * asynchronously: it waits for the scoreboard entries of its wait mask
 * before it starts and signals its signal entry when it ends.
 *
 * The encoders build one instruction each and keep no state, so they
 * run unchanged in a host test.  The writer appends instructions to a
 * buffer of a fixed capacity and latches the first overflow.
 */

#ifndef KERN_DRIVERS_GPU_MALI_CS_BUILD_H
#define KERN_DRIVERS_GPU_MALI_CS_BUILD_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The opcodes.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum "CS Opcode").
 */
#define MALI_CS_OP_NOP			0U
#define MALI_CS_OP_MOVE48		1U
#define MALI_CS_OP_MOVE32		2U
#define MALI_CS_OP_WAIT			3U
#define MALI_CS_OP_RUN_COMPUTE		4U
#define MALI_CS_OP_RUN_TILING		5U
#define MALI_CS_OP_RUN_IDVS		6U
#define MALI_CS_OP_RUN_FRAGMENT		7U
#define MALI_CS_OP_RUN_FULLSCREEN	8U
#define MALI_CS_OP_FINISH_TILING	9U
#define MALI_CS_OP_FINISH_FRAGMENT	11U
#define MALI_CS_OP_ADD32		16U
#define MALI_CS_OP_ADD64		17U
#define MALI_CS_OP_UMIN32		18U
#define MALI_CS_OP_LOAD			20U
#define MALI_CS_OP_STORE		21U
#define MALI_CS_OP_BRANCH		22U
#define MALI_CS_OP_SET_SCOREBOARD	23U
#define MALI_CS_OP_PROGRESS_WAIT	24U
#define MALI_CS_OP_SET_EXCEPTION	25U
#define MALI_CS_OP_CALL			32U
#define MALI_CS_OP_JUMP			33U
#define MALI_CS_OP_REQUEST_RESOURCE	34U
#define MALI_CS_OP_FLUSH_CACHE		36U
#define MALI_CS_OP_SYNC_ADD32		37U
#define MALI_CS_OP_SYNC_SET32		38U
#define MALI_CS_OP_SYNC_WAIT32		39U
#define MALI_CS_OP_STORE_STATE		40U
#define MALI_CS_OP_RUN_COMPUTE_INDIRECT	44U
#define MALI_CS_OP_ERROR_BARRIER	47U
#define MALI_CS_OP_HEAP_SET		48U
#define MALI_CS_OP_HEAP_OPERATION	49U
#define MALI_CS_OP_SYNC_ADD64		51U
#define MALI_CS_OP_SYNC_SET64		52U
#define MALI_CS_OP_SYNC_WAIT64		53U

/*
 * A branch's or a sync wait's condition, the register compared with
 * zero (a branch) or the memory compared with the register (a wait).
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum "CS Condition").
 */
#define MALI_CS_LEQUAL			0U
#define MALI_CS_GREATER			1U
#define MALI_CS_EQUAL			2U
#define MALI_CS_NEQUAL			3U
#define MALI_CS_LESS			4U
#define MALI_CS_GEQUAL			5U
#define MALI_CS_ALWAYS			6U

/*
 * A cache flush's modes, a sync object's scope, a heap operation, the
 * state a store reads, a compute task's axis and a resource request's
 * bits.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enums "CS Flush Mode",
 * "CS Other Flush Mode", "CS Sync Scope", "CS Heap Operation",
 * "CS State", "Task Axis", "CS Exception Type"; struct
 * "CS REQ_RESOURCE").
 */
#define MALI_CS_FLUSH_NONE		0U
#define MALI_CS_FLUSH_CLEAN		1U
#define MALI_CS_FLUSH_CLEAN_INVALIDATE	3U
#define MALI_CS_FLUSH_OTHER_INVALIDATE	2U
#define MALI_CS_SCOPE_SYSTEM		0U
#define MALI_CS_SCOPE_GROUP		2U
#define MALI_CS_HEAP_VERTEX_STARTED	0U
#define MALI_CS_HEAP_VERTEX_COMPLETED	1U
#define MALI_CS_HEAP_FRAGMENT_COMPLETED	3U
#define MALI_CS_STATE_TIMESTAMP		0U
#define MALI_CS_STATE_CYCLES		1U
#define MALI_CS_STATE_ERROR		3U
#define MALI_CS_AXIS_X			0U
#define MALI_CS_AXIS_Y			1U
#define MALI_CS_AXIS_Z			2U
#define MALI_CS_EXCEPTION_TILER_OOM	2U
#define MALI_CS_RESOURCE_COMPUTE	1U
#define MALI_CS_RESOURCE_FRAGMENT	2U
#define MALI_CS_RESOURCE_TILER		4U
#define MALI_CS_RESOURCE_IDVS		8U

/*
 * The staging registers a compute run reads: the resource tables, the
 * uniform pointers, the shader programs and the thread storage (four of
 * each, 64-bit pairs), the global offset, the workgroup size word, the
 * job's offset and its size in workgroups.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum "COMPUTE SR").
 */
#define MALI_CS_COMPUTE_SRT		0U
#define MALI_CS_COMPUTE_FAU		8U
#define MALI_CS_COMPUTE_SPD		16U
#define MALI_CS_COMPUTE_TSD		24U
#define MALI_CS_COMPUTE_GLOBAL_OFFSET	32U
#define MALI_CS_COMPUTE_WORKGROUP	33U
#define MALI_CS_COMPUTE_OFFSET_X	34U
#define MALI_CS_COMPUTE_OFFSET_Y	35U
#define MALI_CS_COMPUTE_OFFSET_Z	36U
#define MALI_CS_COMPUTE_SIZE_X		37U
#define MALI_CS_COMPUTE_SIZE_Y		38U
#define MALI_CS_COMPUTE_SIZE_Z		39U

/*
 * The staging registers an indexed vertex and tiling run reads.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum "IDVS SR").
 */
#define MALI_CS_IDVS_SRT		0U
#define MALI_CS_IDVS_FAU		8U
#define MALI_CS_IDVS_SPD		16U
#define MALI_CS_IDVS_TSD		24U
#define MALI_CS_IDVS_GLOBAL_OFFSET	32U
#define MALI_CS_IDVS_INDEX_COUNT	33U
#define MALI_CS_IDVS_INSTANCE_COUNT	34U
#define MALI_CS_IDVS_INDEX_OFFSET	35U
#define MALI_CS_IDVS_VERTEX_OFFSET	36U
#define MALI_CS_IDVS_INSTANCE_OFFSET	37U
#define MALI_CS_IDVS_DCD2		38U
#define MALI_CS_IDVS_INDEX_BUFFER_SIZE	39U
#define MALI_CS_IDVS_TILER_CONTEXT	40U
#define MALI_CS_IDVS_SCISSOR		42U
#define MALI_CS_IDVS_DEPTH_LOW		44U
#define MALI_CS_IDVS_DEPTH_HIGH		45U
#define MALI_CS_IDVS_OCCLUSION		46U
#define MALI_CS_IDVS_VARYING_SIZE	48U
#define MALI_CS_IDVS_BLEND		50U
#define MALI_CS_IDVS_DEPTH_STENCIL	52U
#define MALI_CS_IDVS_INDEX_BUFFER	54U
#define MALI_CS_IDVS_TILER_FLAGS	56U
#define MALI_CS_IDVS_DCD0		57U
#define MALI_CS_IDVS_DCD1		58U
#define MALI_CS_IDVS_PRIMITIVE_SIZE	60U

/*
 * The staging registers a fragment run reads.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum "FRAGMENT SR").
 */
#define MALI_CS_FRAGMENT_FRAMEBUFFER	40U
#define MALI_CS_FRAGMENT_BOX_MIN	42U
#define MALI_CS_FRAGMENT_BOX_MAX	43U
#define MALI_CS_FRAGMENT_TEM		44U
#define MALI_CS_FRAGMENT_TEM_STRIDE	46U

/* The bytes of one instruction, and the most registers one load or store moves. */
#define MALI_CS_INSTRUCTION_BYTES	8U
#define MALI_CS_MOVE_REGISTERS		16U

/*
 * A run of instructions being written into a buffer.
 *
 * words is the processor's view of the buffer (uncached, so the front
 * end reads what was written), capacity how many instructions fit and
 * count how many are written.  error latches the first instruction that
 * did not fit; nothing is written after it.
 */
struct mali_cs {
	volatile uint64_t *words;
	uint32_t capacity;
	uint32_t count;
	int error;
};

/* The instruction encoders: each builds one 64-bit instruction from its operands. */
uint64_t mali_cs_nop(void);
uint64_t mali_cs_move48(unsigned destination, uint64_t immediate);
uint64_t mali_cs_move32(unsigned destination, uint32_t immediate);
uint64_t mali_cs_wait(uint32_t wait_mask);
uint64_t mali_cs_run_compute(unsigned task_increment, unsigned task_axis, unsigned srt, unsigned spd, unsigned tsd, unsigned fau);
uint64_t mali_cs_run_idvs(uint32_t flags_override, bool malloc_enable, bool varying_srt, bool varying_fau, bool varying_tsd, bool fragment_srt, bool fragment_tsd);
uint64_t mali_cs_run_fragment(bool tile_enable_map, unsigned tile_order);
uint64_t mali_cs_finish_tiling(void);
uint64_t mali_cs_finish_fragment(bool increment_completed, uint32_t wait_mask, unsigned last_chunk, unsigned first_chunk, unsigned signal_slot);
uint64_t mali_cs_add32(unsigned destination, unsigned source, int32_t immediate);
uint64_t mali_cs_add64(unsigned destination, unsigned source, int32_t immediate);
uint64_t mali_cs_load(unsigned base_register, unsigned address, uint32_t mask, int32_t offset);
uint64_t mali_cs_store(unsigned base_register, unsigned address, uint32_t mask, int32_t offset);
uint64_t mali_cs_branch(int32_t offset, unsigned condition, unsigned value);
uint64_t mali_cs_set_scoreboard(unsigned endpoint_entry, unsigned other_entry);
uint64_t mali_cs_set_exception(unsigned exception, unsigned address, unsigned length);
uint64_t mali_cs_call(unsigned address, unsigned length);
uint64_t mali_cs_jump(unsigned address, unsigned length);
uint64_t mali_cs_request_resource(unsigned resources);
uint64_t mali_cs_flush_cache(unsigned l2, unsigned load_store, unsigned other, uint32_t wait_mask, unsigned flush_id, unsigned signal_slot);
uint64_t mali_cs_sync_add32(bool propagate, unsigned scope, uint32_t wait_mask, unsigned data, unsigned address, unsigned signal_slot);
uint64_t mali_cs_sync_set32(bool propagate, unsigned scope, uint32_t wait_mask, unsigned data, unsigned address, unsigned signal_slot);
uint64_t mali_cs_sync_wait32(bool reject, unsigned condition, unsigned data, unsigned address);
uint64_t mali_cs_sync_add64(bool propagate, unsigned scope, uint32_t wait_mask, unsigned data, unsigned address, unsigned signal_slot);
uint64_t mali_cs_sync_set64(bool propagate, unsigned scope, uint32_t wait_mask, unsigned data, unsigned address, unsigned signal_slot);
uint64_t mali_cs_sync_wait64(bool reject, unsigned condition, unsigned data, unsigned address);
uint64_t mali_cs_store_state(int32_t offset, uint32_t wait_mask, unsigned state, unsigned address, unsigned signal_slot);
uint64_t mali_cs_error_barrier(void);
uint64_t mali_cs_heap_set(unsigned address);
uint64_t mali_cs_heap_operation(uint32_t wait_mask, unsigned operation, unsigned signal_slot);

/* Reports the opcode of an instruction. */
unsigned mali_cs_opcode(uint64_t instruction);

/* The writer: a buffer of instructions, appended to in order. */
void mali_cs_init(struct mali_cs *cs, volatile uint64_t *words, uint32_t capacity);
void mali_cs_emit(struct mali_cs *cs, uint64_t instruction);
void mali_cs_move64(struct mali_cs *cs, unsigned destination, uint64_t value);
uint32_t mali_cs_bytes(const struct mali_cs *cs);

/*
 * Writes a loop that copies bytes (a multiple of 64) from one GPU
 * address to another through sixteen work registers from data, with
 * scratch the even first of four more registers for the two addresses
 * and the count.
 */
void mali_cs_copy(struct mali_cs *cs, unsigned data, unsigned scratch, uint64_t destination, uint64_t source, uint64_t bytes, unsigned load_store_slot);

/*
 * Writes a loop that fills bytes (a multiple of 64) at a GPU address
 * with one 32-bit pattern, through sixteen work registers from data and
 * scratch the even first of four more.
 */
void mali_cs_fill(struct mali_cs *cs, unsigned data, unsigned scratch, uint64_t destination, uint32_t pattern, uint64_t bytes, unsigned load_store_slot);

/*
 * Writes a loop that copies rows rows of row_bytes (a multiple of four)
 * from one GPU address to another, each row's start moved on by its
 * stride; data is the first of sixteen work registers, scratch the even
 * first of eight more (two address pairs, two row starts, two counts).
 */
void mali_cs_copy_rows(struct mali_cs *cs, unsigned data, unsigned scratch, uint64_t destination, int32_t destination_stride, uint64_t source, int32_t source_stride, uint32_t row_bytes, uint32_t rows, unsigned load_store_slot);

/*
 * Writes a loop that fills rows rows of row_bytes (a multiple of four)
 * at a GPU address with one 32-bit pattern, each row's start moved on by
 * the stride; registers as mali_cs_copy_rows().
 */
void mali_cs_fill_rows(struct mali_cs *cs, unsigned data, unsigned scratch, uint64_t destination, int32_t stride, uint32_t pattern, uint32_t row_bytes, uint32_t rows, unsigned load_store_slot);

#endif
