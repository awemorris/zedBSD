/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The driver's own transfer programs.
 *
 * A device without the blit engine copies, fills and updates buffers,
 * copies between buffers and images and between images, and resets
 * query pools with compute programs of the driver's; every device packs
 * query results with one, and copies the bytes of a buffer copy that are
 * not whole words, and one aspect of a combined depth and stencil image.
 * The programs are written here as the shared scalar IR, compiled with
 * the GSI1 compiler once per GPU at their first use, and placed in the
 * code heap of each device that uses them (they run at the device's
 * session's addresses).
 *
 * They reach their memory the way every pipeline does (the compiler's
 * convention): two storage buffers of the resource table (set 2), the
 * source and the destination, whose descriptors the executor writes with
 * the 4-byte aligned start and the bytes of each side, and the region in
 * push constants (c0 on).  GSI1 reads and writes memory a word at a time,
 * so an element that is not a whole aligned word is read from the one or
 * two words it lies in and written with atomic and and or of its bytes
 * alone, which leave the other bytes of the words as they are even when
 * another invocation writes them at the same time.
 *
 * Without the blit engine a resolve and a blit are draws: one triangle
 * over the destination's rectangle, made from the vertex index alone,
 * into the destination as colour attachment 0; the fragment program reads
 * the source through texture and sampler slot 0 (set 0 and set 1), as
 * texels of a multisampled image at the pixel's place moved by the push
 * constants (averaged, or sample 0 of an integer format), or sampled at
 * the coordinate the vertex program makes of the two rectangles.  The
 * texture unit decodes the source's format and filters, and the colour
 * stage encodes the destination's.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#include "vk.h"
#include "../compiler/ghci-isa.h"
#include "drivers/gpu/compiler/spirv.h"

/* The most instructions, uniforms and interface locations one program's IR takes. */
#define META_INSTRUCTIONS_MAX		128U
#define META_UNIFORMS			2U
#define META_LOCATIONS			1U

/*
 * The float bits of 0, 0.25, 0.5, 1 and 4.
 *
 * Values referenced: IEEE 754-2008 binary32 (sign 31, biased exponent
 * 30:23 with a bias of 127, fraction 22:0).
 */
#define META_FLOAT_ZERO			0x00000000U
#define META_FLOAT_QUARTER		0x3e800000U
#define META_FLOAT_HALF			0x3f000000U
#define META_FLOAT_ONE			0x3f800000U
#define META_FLOAT_FOUR			0x40800000U

/*
 * A draw's pipeline state: a triangle list, the line width 1.0 (4.4
 * fixed point), every colour component written, every sample.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 8.2,
 * State Encodings: the topology is VkPrimitiveTopology's).
 */
#define META_TOPOLOGY_TRIANGLES		3U
#define META_LINE_WIDTH_ONE		16U
#define META_WRITE_ALL			0xfU
#define META_SAMPLE_MASK_ALL		0xfU

/* The uniforms of a program: the source buffer (set 0, binding 0) and the destination (binding 1). */
#define META_SOURCE			0U
#define META_DESTINATION		1U

/*
 * The alignment of a placed program's pipeline state record (128 bytes,
 * also the 64 BIND_PIPELINE asks) and of its code (a multiple of an
 * instruction's 8).
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 8.2,
 * GhciPipelineState: 128 bytes, aligned to 64 or 128; section 7.3,
 * CMD_BIND_PIPELINE: the record's address 64-byte aligned; section 12.4,
 * Instruction format: every instruction at an 8-byte aligned address).
 */
#define META_PSO_ALIGN			128U
#define META_CODE_ALIGN			64U

/*
 * The flags of a buffer descriptor a transfer program only reads.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 8.3,
 * GhciBufferDescriptor: flags [0] ReadOnly).
 */
#define META_READ_ONLY			(1U << 0)

/*
 * The alignment of the resource table placed after the copy program in
 * session 0 (the 64 a descriptor table takes, as the executor's tables).
 */
#define META_TABLE_ALIGN		64U

/* The bytes of a query's slot (the result, then the availability word). */
#define META_QUERY_SLOT_BYTES		16U

/* The most bytes a buffer descriptor reaches (32-bit addresses). */
#define META_ADDRESS_LIMIT		0x100000000ULL

/*
 * The most elements one copy takes: the invocations of its last
 * workgroup still count below 2^32, so that no invocation's element
 * wraps to one of the copy's.
 */
#define META_ELEMENTS_MAX		(0xffffffffU - (GHCI_VK_META_GROUP - 1U))

/*
 * One program's IR being written: the instructions, the uniforms, and
 * the next value number.  overflow says that an instruction did not fit,
 * which refuses the program.
 */
struct meta_builder {
	struct drv_gpu_shader_ir ir;
	struct drv_gpu_shader_ir_inst instructions[META_INSTRUCTIONS_MAX];
	struct drv_gpu_shader_ir_uniform uniforms[META_UNIFORMS];
	struct drv_gpu_shader_ir_io inputs[META_LOCATIONS];
	struct drv_gpu_shader_ir_io outputs[META_LOCATIONS];
	uint32_t values;
	int overflow;
};

static int meta_build(enum ghci_vk_meta_kind kind, uint32_t which, struct meta_builder *builder);
static int meta_compile_one(const struct ghci_caps *caps, enum ghci_vk_meta_kind kind, uint32_t which, const struct ghci_gsi1_bindings *bindings, struct ghci_gsi1_program **program);
static void build_copy(struct meta_builder *builder);
static void build_vertex(struct meta_builder *builder, int blit);
static void build_resolve(struct meta_builder *builder, uint32_t samples);
static void build_blit(struct meta_builder *builder);
static uint32_t build_blit_axis(struct meta_builder *builder, uint32_t position, uint32_t first_word, uint32_t size_word);
static uint32_t ir_texture(struct meta_builder *builder, uint32_t first, uint32_t count, uint32_t message, uint32_t shape);
static uint32_t ir_input(struct meta_builder *builder, uint32_t location, uint32_t component);
static void ir_output(struct meta_builder *builder, uint32_t location, uint32_t component, uint32_t value);
static void build_query(struct meta_builder *builder);
static uint32_t build_index(struct meta_builder *builder, uint32_t first_word, uint32_t count_word, uint32_t *inside);
static uint32_t build_address(struct meta_builder *builder, uint32_t column, uint32_t row, uint32_t slice, uint32_t first_word);
static struct drv_gpu_shader_ir_inst *ir_next(struct meta_builder *builder);
static uint32_t ir_value(struct meta_builder *builder, enum drv_gpu_shader_ir_op op, uint32_t left, uint32_t right);
static uint32_t ir_constant(struct meta_builder *builder, uint32_t bits);
static uint32_t ir_float(struct meta_builder *builder, uint32_t bits);
static uint32_t ir_push(struct meta_builder *builder, uint32_t word);
static uint32_t ir_system(struct meta_builder *builder, uint32_t component);
static uint32_t ir_select(struct meta_builder *builder, uint32_t condition, uint32_t chosen, uint32_t other);
static uint32_t ir_load(struct meta_builder *builder, uint32_t buffer, uint32_t offset);
static void ir_store(struct meta_builder *builder, uint32_t buffer, uint32_t offset, uint32_t value, uint32_t predicate);
static void ir_atomic(struct meta_builder *builder, uint32_t operation, uint32_t buffer, uint32_t offset, uint32_t value, uint32_t predicate);
static void ir_fence(struct meta_builder *builder, uint32_t fences);
static int meta_place(struct ghci_vk_session *session, struct ghci_vk_device *device, struct ghci_gsi1_program *const *programs, struct ghci_vk_meta_pipeline *placed);
static int meta_entries(struct ghci_gsi1_program *const *programs, struct ghci_vk_meta_pipeline *placed);
static uint32_t meta_layout(struct ghci_gsi1_program *const *programs, uint32_t *code_at, uint32_t *fragment_at);
static int meta_write(struct ghci_gsi1_program *const *programs, uint8_t *view, uint64_t base, uint32_t code_at, uint32_t fragment_at, struct ghci_vk_meta_pipeline *placed);
static int rows_place(struct ghci_device *gpu, struct ghci_vk_rows *rows);
static void meta_pso(struct ghci_gsi1_program *const *programs, uint64_t code, uint64_t fragment, struct ghci_pso *pso);
static int meta_entry(const struct ghci_gsi1_program *program, uint32_t binding, uint32_t *offset);
static int meta_span(uint64_t address, uint64_t bytes, uint64_t *base, uint32_t *size);
static int meta_programs(struct ghci_device *gpu, enum ghci_vk_meta_kind kind, struct ghci_gsi1_program **programs);
static void meta_refuse(struct ghci_device *gpu, enum ghci_vk_meta_kind kind);

/*
 * Compiles the programs of one of the driver's transfers for a device of
 * caps: programs[0] the compute or vertex program, programs[1] a draw's
 * fragment program (which extends the vertex program's tables) or NULL.
 * Returns 0 with the programs (the caller frees them with
 * ghci_gsi1_program_free()), ENOMEM, or ENOTSUP when the compiler refuses
 * one or it would need scratch memory.
 */
int
ghci_vk_meta_compile(
	const struct ghci_caps *caps,
	enum ghci_vk_meta_kind kind,
	struct ghci_gsi1_program **programs)
{
	int error;

	/* The compute or vertex program. */
	programs[0] = NULL;
	programs[1] = NULL;
	error = meta_compile_one(caps, kind, 0U, NULL, &programs[0]);
	if (error != 0)
		return error;

	/* A draw's fragment program, with the vertex program's tables. */
	if (kind == GHCI_VK_META_COPY || kind == GHCI_VK_META_QUERY)
		return 0;
	error = meta_compile_one(caps, kind, 1U, &programs[0]->bindings, &programs[1]);
	if (error != 0) {
		ghci_gsi1_program_free(programs[0]);
		programs[0] = NULL;
		return error;
	}

	/* Succeeded: both programs. */
	return 0;
}

/*
 * Finds one of the driver's transfer programs placed in a device's code
 * heap, compiling it once for the GPU and placing it once for the device
 * at its first use.  Returns 0 with the placed program, ENOMEM, EIO (no
 * room could be made in the heap), or ENOTSUP (the compiler refuses it,
 * or its pipeline state cannot hold it; the GPU remembers that, and the
 * kind is not compiled again).
 */
int
ghci_vk_meta_pipeline(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device,
	enum ghci_vk_meta_kind kind,
	const struct ghci_vk_meta_pipeline **pipeline)
{
	struct ghci_gsi1_program *programs[GHCI_VK_META_PROGRAMS];
	int error;

	/* Placed for this device already. */
	if (kind >= GHCI_VK_META_KINDS)
		return EINVAL;
	if (device->meta[kind].pso_va != 0U) {
		*pipeline = &device->meta[kind];
		return 0;
	}

	/* The GPU's programs, compiled at their first use. */
	error = meta_programs(session->device, kind, programs);
	if (error != 0)
		return error;

	/* The programs' place in the device's code heap. */
	error = meta_place(session, device, programs, &device->meta[kind]);
	if (error == ENOTSUP)
		meta_refuse(session->device, kind);
	if (error != 0)
		return error;

	/* Succeeded: the programs are placed. */
	*pipeline = &device->meta[kind];
	return 0;
}

/*
 * Plans a copy of elements: the descriptors start at the word of each
 * side's first byte and end at the word of its last, and the push
 * constants give the region from there.  Returns 0 with the dispatch, or
 * EINVAL for elements of another size, no elements, more than
 * META_ELEMENTS_MAX of them, or a side that does not lie below 4 GiB.
 */
int
ghci_vk_meta_copy(
	const struct ghci_vk_meta_copy *copy,
	struct ghci_vk_meta_dispatch *dispatch)
{
	uint64_t elements;
	uint64_t source_span;
	uint64_t destination_span;
	uint64_t source_base;
	uint64_t destination_base;
	uint32_t index;
	int error;

	/* Elements of 1 to 4 bytes, at least one, a count the program holds. */
	kern_memset(dispatch, 0, sizeof(*dispatch));
	if (copy->element_bytes == 0U || copy->element_bytes > 4U)
		return EINVAL;
	if (copy->per_row == 0U || copy->rows == 0U || copy->slices == 0U)
		return EINVAL;
	elements = (uint64_t)copy->per_row * copy->rows * copy->slices;
	if (elements > META_ELEMENTS_MAX)
		return EINVAL;

	/* The bytes from each side's first element to the end of its last. */
	source_span = (uint64_t)(copy->per_row - 1U) * copy->source_step[0] + (uint64_t)(copy->rows - 1U) * copy->source_step[1]
		+ (uint64_t)(copy->slices - 1U) * copy->source_step[2] + copy->element_bytes;
	destination_span = (uint64_t)(copy->per_row - 1U) * copy->destination_step[0] + (uint64_t)(copy->rows - 1U) * copy->destination_step[1]
		+ (uint64_t)(copy->slices - 1U) * copy->destination_step[2] + copy->element_bytes;

	/* The descriptors: from the word of the first byte, whole words. */
	error = meta_span(copy->source, source_span, &source_base, &dispatch->source_bytes);
	if (error != 0)
		return error;
	error = meta_span(copy->destination, destination_span, &destination_base, &dispatch->destination_bytes);
	if (error != 0)
		return error;
	dispatch->source = source_base;
	dispatch->destination = destination_base;

	/* The region, from the descriptors' addresses. */
	dispatch->kind = GHCI_VK_META_COPY;
	dispatch->push[GHCI_VK_META_COPY_COUNT] = (uint32_t)elements;
	dispatch->push[GHCI_VK_META_COPY_PER_ROW] = copy->per_row;
	dispatch->push[GHCI_VK_META_COPY_ROWS] = copy->rows;
	dispatch->push[GHCI_VK_META_COPY_MASK] = 0xffffffffU;
	if (copy->element_bytes < 4U)
		dispatch->push[GHCI_VK_META_COPY_MASK] = (1U << (8U * copy->element_bytes)) - 1U;
	dispatch->push[GHCI_VK_META_COPY_SOURCE] = (uint32_t)(copy->source - source_base);
	dispatch->push[GHCI_VK_META_COPY_DESTINATION] = (uint32_t)(copy->destination - destination_base);
	for (index = 0U; index < 3U; index++) {
		dispatch->push[GHCI_VK_META_COPY_SOURCE + 1U + index] = copy->source_step[index];
		dispatch->push[GHCI_VK_META_COPY_DESTINATION + 1U + index] = copy->destination_step[index];
	}
	dispatch->push[GHCI_VK_META_COPY_BYTES] = copy->element_bytes;
	dispatch->push_words = GHCI_VK_META_COPY_WORDS;
	dispatch->first_word = GHCI_VK_META_COPY_FIRST;
	dispatch->elements = (uint32_t)elements;

	/* Succeeded: the dispatch is planned. */
	return 0;
}

/*
 * Plans a copy of query results: count 16-byte slots from source (the
 * first query's slot) to destination, stride bytes apart, with the
 * program's flags (GHCI_VK_META_QUERY_*).  Returns 0 with the dispatch,
 * or EINVAL for no queries or more than META_ELEMENTS_MAX, a destination
 * or stride not aligned to the results' size, or a side that does not
 * lie below 4 GiB.
 */
int
ghci_vk_meta_query(
	uint64_t source,
	uint32_t count,
	uint64_t destination,
	uint32_t stride,
	uint32_t flags,
	struct ghci_vk_meta_dispatch *dispatch)
{
	uint64_t result_bytes;
	uint64_t span;
	uint64_t source_base;
	uint64_t destination_base;
	uint32_t alignment;
	int error;

	/* At least one query, within the program's count; results on their own size. */
	kern_memset(dispatch, 0, sizeof(*dispatch));
	if (count == 0U || count > META_ELEMENTS_MAX)
		return EINVAL;
	alignment = 4U;
	result_bytes = 4U;
	if ((flags & GHCI_VK_META_QUERY_64) != 0U) {
		alignment = 8U;
		result_bytes = 8U;
	}
	if ((destination & (alignment - 1U)) != 0U || (stride & (alignment - 1U)) != 0U)
		return EINVAL;
	if ((flags & GHCI_VK_META_QUERY_AVAILABILITY) != 0U)
		result_bytes *= 2U;

	/* The slots, and the results with their availability. */
	error = meta_span(source, (uint64_t)count * META_QUERY_SLOT_BYTES, &source_base, &dispatch->source_bytes);
	if (error != 0)
		return error;
	span = (uint64_t)(count - 1U) * stride + result_bytes;
	error = meta_span(destination, span, &destination_base, &dispatch->destination_bytes);
	if (error != 0)
		return error;
	dispatch->source = source_base;
	dispatch->destination = destination_base;

	/* The queries, from the descriptors' addresses. */
	dispatch->kind = GHCI_VK_META_QUERY;
	dispatch->push[GHCI_VK_META_QUERY_COUNT] = count;
	dispatch->push[GHCI_VK_META_QUERY_SOURCE] = (uint32_t)(source - source_base);
	dispatch->push[GHCI_VK_META_QUERY_DESTINATION] = (uint32_t)(destination - destination_base);
	dispatch->push[GHCI_VK_META_QUERY_STRIDE] = stride;
	dispatch->push[GHCI_VK_META_QUERY_FLAGS] = flags;
	dispatch->push_words = GHCI_VK_META_QUERY_WORDS;
	dispatch->first_word = GHCI_VK_META_QUERY_FIRST;
	dispatch->elements = count;

	/* Succeeded: the dispatch is planned. */
	return 0;
}

/*
 * Writes the resource table of a transfer through the host's view of it:
 * the source's buffer descriptor (read only) and the destination's, at
 * the offsets the program reads them from.
 */
void
ghci_vk_meta_table(
	const struct ghci_vk_meta_pipeline *pipeline,
	const struct ghci_vk_meta_dispatch *dispatch,
	uint8_t *view)
{
	struct ghci_buffer_desc descriptor;

	/* The source, which the program only reads. */
	kern_memset(&descriptor, 0, sizeof(descriptor));
	descriptor.base = dispatch->source;
	descriptor.bytes = dispatch->source_bytes;
	descriptor.flags = META_READ_ONLY;
	(void)ghci_buffer_desc_encode(view + pipeline->source_entry, &descriptor);

	/* The destination. */
	kern_memset(&descriptor, 0, sizeof(descriptor));
	descriptor.base = dispatch->destination;
	descriptor.bytes = dispatch->destination_bytes;
	(void)ghci_buffer_desc_encode(view + pipeline->destination_entry, &descriptor);
}

/*
 * Writes the binds of a transfer: its pipeline, and its resource table
 * at the GPU address table.  Returns the dwords written
 * (GHCI_VK_META_BIND_DWORDS).
 */
uint32_t
ghci_vk_meta_bind(
	const struct ghci_vk_meta_pipeline *pipeline,
	uint64_t table,
	uint32_t *words)
{
	uint32_t dwords;

	/* The program, then its table of set 2. */
	dwords = ghci_cmd_bind_pipeline(words, pipeline->pso_va);
	dwords += ghci_cmd_bind_descriptor_table(&words[dwords], GSI1_ABI_SET_RESOURCES, table, pipeline->resource_bytes / GSI1_ABI_RESOURCE_UNIT);

	/* Succeeded: the binds. */
	return dwords;
}

/*
 * Writes the next step of a transfer from its element *first: the push
 * constants with that first element, and one dispatch of at most
 * GHCI_VK_META_GROUPS_MAX workgroups of GHCI_VK_META_GROUP invocations;
 * *first moves past the step's elements.  Returns the dwords written (at
 * most GHCI_VK_META_STEP_DWORDS), or 0 once every element was dispatched.
 */
uint32_t
ghci_vk_meta_step(
	const struct ghci_vk_meta_dispatch *dispatch,
	uint64_t *first,
	uint32_t *words)
{
	uint32_t push[GHCI_VK_META_PUSH_WORDS];
	uint64_t groups;
	uint32_t dwords;

	/* Every element dispatched. */
	if (*first >= dispatch->elements)
		return 0U;

	/* The workgroups of the elements left, as many as one dispatch runs. */
	groups = ((uint64_t)dispatch->elements - *first + GHCI_VK_META_GROUP - 1U) / GHCI_VK_META_GROUP;
	if (groups > GHCI_VK_META_GROUPS_MAX)
		groups = GHCI_VK_META_GROUPS_MAX;

	/* The constants with the step's first element, and the dispatch. */
	kern_memcpy(push, dispatch->push, sizeof(push));
	push[dispatch->first_word] = (uint32_t)*first;
	dwords = ghci_cmd_load_const(words, GSI1_ABI_BANK_PUSH_LOW, dispatch->push_words, push);
	dwords += ghci_cmd_dispatch(&words[dwords], (uint32_t)groups, 1U, 1U);
	*first += groups * GHCI_VK_META_GROUP;

	/* Succeeded: the step. */
	return dwords;
}

/*
 * Copies rows in session 0 with the driver's copy program (the executor's
 * copy_rows): the program placed in session 0 at the first copy, the
 * resource table written, and the binds, the steps and the barriers
 * around them placed in the ring as the driver's own work.  Returns 0
 * with the ticket the caller waits for, ENOTSUP with nothing placed when
 * the program cannot make the copy, ENOMEM, or EIO.
 *
 * Procedure referenced: amaterrace-gpu ad23fe01 docs/ghci.md (section
 * 6.0, Session 0 Execution Context: the PSO, the descriptors and the
 * buffers of session 0's compute work mapped into session 0; section
 * 11.5, CMD_BARRIER: INVAL_DESCRIPTOR after the host rewrote a table,
 * INVAL_ICACHE after it wrote code).
 */
int
ghci_vk_copy_rows(
	struct ghci_device *device,
	const struct ghci_rows *rows,
	struct ghci_ticket *ticket)
{
	struct ghci_vk_meta_dispatch dispatch;
	struct ghci_vk_meta_copy copy;
	struct ghci_vk_rows *placed;
	struct ghci_vk *vk;
	uint64_t steps;
	uint64_t first;
	uint64_t table;
	uint32_t *packets;
	uint32_t capacity;
	uint32_t dwords;
	uint32_t step;
	int error;

	/* The copy as the copy program makes it: words, a row of them at a time. */
	kern_memset(&copy, 0, sizeof(copy));
	copy.source = rows->source;
	copy.destination = rows->destination;
	copy.element_bytes = 4U;
	copy.per_row = rows->bytes / 4U;
	copy.rows = rows->rows;
	copy.slices = 1U;
	copy.source_step[0] = 4U;
	copy.source_step[1] = rows->source_stride;
	copy.destination_step[0] = 4U;
	copy.destination_step[1] = rows->destination_stride;
	if ((rows->bytes & 3U) != 0U)
		return ENOTSUP;
	error = ghci_vk_meta_copy(&copy, &dispatch);
	if (error != 0)
		return ENOTSUP;

	/* The packets' room: the barriers, the binds and every step. */
	steps = ((uint64_t)dispatch.elements + (uint64_t)GHCI_VK_META_GROUP * GHCI_VK_META_GROUPS_MAX - 1U) /
	    ((uint64_t)GHCI_VK_META_GROUP * GHCI_VK_META_GROUPS_MAX);
	capacity = 2U * GHCI_DW_BARRIER + GHCI_VK_META_BIND_DWORDS + (uint32_t)steps * GHCI_VK_META_STEP_DWORDS;
	packets = kern_malloc((size_t)capacity * sizeof(*packets));
	if (packets == NULL)
		return ENOMEM;

	/* The program in session 0, the table and the packets in the ring, one copy at a time. */
	vk = device->vk;
	placed = &vk->rows;
	mutex_lock(&placed->lock);

	error = 0;
	if (placed->memory == NULL)
		error = rows_place(device, placed);
	if (error == 0) {
		/* The table, after the code in the run. */
		ghci_vk_meta_table(&placed->pipeline, &dispatch, placed->memory->view + placed->table_at);
		table = placed->mapping->va + placed->table_at;

		/* The head: the host's code and table seen; the binds and steps; the tail: the copy in memory. */
		dwords = ghci_cmd_barrier(packets, 0U, 0U, GHCI_VK_HEAD_FLAGS);
		dwords += ghci_vk_meta_bind(&placed->pipeline, table, &packets[dwords]);
		first = 0U;
		for (;;) {
			step = ghci_vk_meta_step(&dispatch, &first, &packets[dwords]);
			if (step == 0U)
				break;
			dwords += step;
		}
		dwords += ghci_cmd_barrier(&packets[dwords], 0U, 0U, GHCI_VK_TAIL_FLAGS);

		/* The driver's own work, in session 0. */
		error = ghci_cp_submit(device, NULL, packets, dwords, ticket);
	}

	mutex_unlock(&placed->lock);

	/* Reports a copy that was not placed. */
	kern_free(packets);
	if (error != 0)
		return error;

	/* Succeeded: the copy is in the ring. */
	return 0;
}

/*
 * Lets the copy program's run in session 0 go at the detach: its mapping
 * goes with the unmap's fence, and the run with the mapping.
 */
void
ghci_vk_rows_release(
	struct ghci_device *device)
{
	struct ghci_vk_rows *placed;
	struct ghci_vk *vk;

	/* Nothing placed. */
	vk = device->vk;
	placed = &vk->rows;
	if (placed->memory == NULL)
		return;

	/* The mapping, then the run's hold. */
	ghci_vm_unmap(device, placed->mapping);
	mutex_lock(&device->lock);

	ghci_memory_unhold(device, placed->memory);

	mutex_unlock(&device->lock);
	placed->mapping = NULL;
	placed->memory = NULL;
}

/*
 * Compiles one program of a transfer (which: 0 the compute or vertex
 * program, 1 the fragment program) with the device's options and the
 * tables the other program took.  Returns 0 with the program, ENOMEM, or
 * ENOTSUP.
 */
static int
meta_compile_one(
	const struct ghci_caps *caps,
	enum ghci_vk_meta_kind kind,
	uint32_t which,
	const struct ghci_gsi1_bindings *bindings,
	struct ghci_gsi1_program **program)
{
	struct ghci_gsi1_diagnostic diagnostic;
	struct ghci_gsi1_options *options;
	struct meta_builder *builder;
	int error;

	/* The IR and the options, too large for the stack. */
	builder = kern_calloc(1U, sizeof(*builder));
	if (builder == NULL)
		return ENOMEM;
	options = kern_calloc(1U, sizeof(*options));
	if (options == NULL) {
		kern_free(builder);
		return ENOMEM;
	}
	error = meta_build(kind, which, builder);
	if (error != 0) {
		kern_free(options);
		kern_free(builder);
		return error;
	}

	/* The device's options; the programs check their own ranges; a vertex program fetches no attribute. */
	ghci_isa_options(caps, options);
	options->robust_buffer_access = 0U;
	if (builder->ir.stage == DRV_GPU_STAGE_VERTEX)
		options->has_attribute_map = 1U;
	if (builder->ir.stage == DRV_GPU_STAGE_FRAGMENT)
		options->samples = 1U;
	if (bindings != NULL)
		options->bindings = *bindings;

	/* The compile. */
	kern_memset(&diagnostic, 0, sizeof(diagnostic));
	*program = NULL;
	error = ghci_gsi1_compile_ir(&builder->ir, options, program, &diagnostic);
	kern_free(options);
	kern_free(builder);
	if (error == ENOMEM)
		return ENOMEM;
	if (error != 0)
		return ENOTSUP;

	/* A program that spills would need a driver table of its own. */
	if ((*program)->scratch_bytes != 0U) {
		ghci_gsi1_program_free(*program);
		*program = NULL;
		return ENOTSUP;
	}

	/* Succeeded: the program. */
	return 0;
}

/*
 * Writes the IR of a program of a kind into builder (which: 0 the
 * compute or vertex program, 1 the fragment program).  Returns 0, or
 * ENOTSUP for a program there is none of.
 */
static int
meta_build(
	enum ghci_vk_meta_kind kind,
	uint32_t which,
	struct meta_builder *builder)
{
	struct drv_gpu_shader_ir *ir;

	/* The lists. */
	ir = &builder->ir;
	ir->instructions = builder->instructions;
	ir->uniforms = builder->uniforms;
	ir->inputs = builder->inputs;
	ir->outputs = builder->outputs;

	/* A compute program of 64 invocations, the two buffers its uniforms 0 and 1. */
	if (kind == GHCI_VK_META_COPY || kind == GHCI_VK_META_QUERY) {
		ir->stage = DRV_GPU_STAGE_COMPUTE;
		ir->uniform_count = META_UNIFORMS;
		ir->local_size[0] = GHCI_VK_META_GROUP;
		ir->local_size[1] = 1U;
		ir->local_size[2] = 1U;
		builder->uniforms[META_SOURCE].set = 0U;
		builder->uniforms[META_SOURCE].binding = 0U;
		builder->uniforms[META_SOURCE].kind = DRV_GPU_IR_UNIFORM_STORAGE;
		builder->uniforms[META_DESTINATION].set = 0U;
		builder->uniforms[META_DESTINATION].binding = 1U;
		builder->uniforms[META_DESTINATION].kind = DRV_GPU_IR_UNIFORM_STORAGE;
	} else {
		/* A draw: the source as the combined image sampler of set 0, binding 0. */
		ir->stage = which == 0U ? DRV_GPU_STAGE_VERTEX : DRV_GPU_STAGE_FRAGMENT;
		builder->uniforms[0].set = 0U;
		builder->uniforms[0].binding = 0U;
		builder->uniforms[0].kind = DRV_GPU_IR_UNIFORM_SAMPLED_IMAGE;
		if (which == 1U)
			ir->uniform_count = 1U;
	}

	/* The kind's instructions. */
	switch (kind) {
	case GHCI_VK_META_COPY:
		build_copy(builder);
		ir->push_bytes = GHCI_VK_META_COPY_WORDS * 4U;
		break;
	case GHCI_VK_META_QUERY:
		build_query(builder);
		ir->push_bytes = GHCI_VK_META_QUERY_WORDS * 4U;
		break;
	case GHCI_VK_META_RESOLVE_AVERAGE2:
	case GHCI_VK_META_RESOLVE_AVERAGE4:
	case GHCI_VK_META_RESOLVE_FIRST:
		if (which == 0U)
			build_vertex(builder, 0);
		else if (kind == GHCI_VK_META_RESOLVE_AVERAGE2)
			build_resolve(builder, 2U);
		else if (kind == GHCI_VK_META_RESOLVE_AVERAGE4)
			build_resolve(builder, 4U);
		else
			build_resolve(builder, 1U);
		ir->push_bytes = GHCI_VK_META_RESOLVE_WORDS * 4U;
		break;
	case GHCI_VK_META_BLIT:
		if (which == 0U)
			build_vertex(builder, 1);
		else
			build_blit(builder);
		ir->push_bytes = GHCI_VK_META_BLIT_WORDS * 4U;
		break;
	default:
		return ENOTSUP;
	}
	if (builder->overflow)
		return ENOTSUP;
	ir->value_count = builder->values;

	/* Succeeded: the IR is written. */
	return 0;
}

/*
 * Writes the copy program, as
 *
 *   i = first + group * 64 + local;  inside = i < count;  (i = 0 outside)
 *   column = i % per_row;  row = i / per_row % rows;  slice = i / per_row / rows;
 *   s = source + column * stride + row * pitch + slice * slice_pitch;  (d the same)
 *   value = the bytes of the words at s & ~3 (and the next when the
 *           element runs past it), shifted down by s & 3, masked;
 *   a whole aligned word: d's word = value;
 *   otherwise: d's word &= ~(mask << 8(d & 3)), |= value << 8(d & 3), and
 *              the next word the same with what is left of mask and value.
 */
static void
build_copy(
	struct meta_builder *builder)
{
	uint32_t inside;
	uint32_t index;
	uint32_t per_row;
	uint32_t rows;
	uint32_t column;
	uint32_t rest;
	uint32_t row;
	uint32_t slice;
	uint32_t source;
	uint32_t destination;
	uint32_t zero;
	uint32_t three;
	uint32_t four;
	uint32_t thirty_two;
	uint32_t ones;
	uint32_t word;
	uint32_t byte;
	uint32_t shift;
	uint32_t end;
	uint32_t straddles;
	uint32_t next;
	uint32_t low;
	uint32_t high;
	uint32_t up;
	uint32_t aligned;
	uint32_t mask;
	uint32_t value;
	uint32_t whole;
	uint32_t store;
	uint32_t partial;
	uint32_t low_mask;
	uint32_t low_bits;
	uint32_t high_mask;
	uint32_t high_bits;
	uint32_t has_high;
	uint32_t write_high;
	uint32_t operand;
	uint32_t other;

	/* The element of the invocation, and whether it is one of the copy's. */
	index = build_index(builder, GHCI_VK_META_COPY_FIRST, GHCI_VK_META_COPY_COUNT, &inside);

	/* Its column, row and slice. */
	per_row = ir_push(builder, GHCI_VK_META_COPY_PER_ROW);
	rows = ir_push(builder, GHCI_VK_META_COPY_ROWS);
	column = ir_value(builder, DRV_GPU_IR_UMOD, index, per_row);
	rest = ir_value(builder, DRV_GPU_IR_UDIV, index, per_row);
	row = ir_value(builder, DRV_GPU_IR_UMOD, rest, rows);
	slice = ir_value(builder, DRV_GPU_IR_UDIV, rest, rows);

	/* The byte offsets of the element on both sides. */
	source = build_address(builder, column, row, slice, GHCI_VK_META_COPY_SOURCE);
	destination = build_address(builder, column, row, slice, GHCI_VK_META_COPY_DESTINATION);

	/* The constants. */
	zero = ir_constant(builder, 0U);
	three = ir_constant(builder, 3U);
	four = ir_constant(builder, 4U);
	thirty_two = ir_constant(builder, 32U);
	ones = ir_constant(builder, 0xffffffffU);

	/* The source's word, and the next one when the element runs past it. */
	other = ir_constant(builder, ~3U);
	word = ir_value(builder, DRV_GPU_IR_IAND, source, other);
	byte = ir_value(builder, DRV_GPU_IR_IAND, source, three);
	shift = ir_value(builder, DRV_GPU_IR_SHL, byte, three);
	operand = ir_push(builder, GHCI_VK_META_COPY_BYTES);
	end = ir_value(builder, DRV_GPU_IR_IADD, byte, operand);
	straddles = ir_value(builder, DRV_GPU_IR_ULT, four, end);
	operand = ir_value(builder, DRV_GPU_IR_IADD, word, four);
	next = ir_select(builder, straddles, operand, word);
	low = ir_load(builder, META_SOURCE, word);
	high = ir_load(builder, META_SOURCE, next);

	/* The element's bytes, shifted down to bit 0 (no bits of the next word for a shift of 0). */
	operand = ir_value(builder, DRV_GPU_IR_ISUB, thirty_two, shift);
	up = ir_value(builder, DRV_GPU_IR_SHL, high, operand);
	aligned = ir_value(builder, DRV_GPU_IR_IEQ, shift, zero);
	up = ir_select(builder, aligned, zero, up);
	mask = ir_push(builder, GHCI_VK_META_COPY_MASK);
	value = ir_value(builder, DRV_GPU_IR_SHR, low, shift);
	value = ir_value(builder, DRV_GPU_IR_IOR, value, up);
	value = ir_value(builder, DRV_GPU_IR_IAND, value, mask);

	/* The destination's word, and the element's bytes in it. */
	word = ir_value(builder, DRV_GPU_IR_IAND, destination, other);
	byte = ir_value(builder, DRV_GPU_IR_IAND, destination, three);
	shift = ir_value(builder, DRV_GPU_IR_SHL, byte, three);
	low_mask = ir_value(builder, DRV_GPU_IR_SHL, mask, shift);
	low_bits = ir_value(builder, DRV_GPU_IR_SHL, value, shift);

	/* A whole aligned word is stored; any other part of a word is cleared and set. */
	whole = ir_value(builder, DRV_GPU_IR_IEQ, low_mask, ones);
	store = ir_value(builder, DRV_GPU_IR_AND, inside, whole);
	operand = ir_value(builder, DRV_GPU_IR_NOT, whole, 0U);
	partial = ir_value(builder, DRV_GPU_IR_AND, inside, operand);
	ir_store(builder, META_DESTINATION, word, value, store);
	operand = ir_value(builder, DRV_GPU_IR_INOT, low_mask, 0U);
	ir_atomic(builder, DRV_GPU_IR_ATOMIC_AND, META_DESTINATION, word, operand, partial);
	ir_atomic(builder, DRV_GPU_IR_ATOMIC_OR, META_DESTINATION, word, low_bits, partial);

	/* The bytes that run into the next word (none for a shift of 0). */
	aligned = ir_value(builder, DRV_GPU_IR_IEQ, shift, zero);
	operand = ir_value(builder, DRV_GPU_IR_ISUB, thirty_two, shift);
	high_mask = ir_value(builder, DRV_GPU_IR_SHR, mask, operand);
	high_mask = ir_select(builder, aligned, zero, high_mask);
	high_bits = ir_value(builder, DRV_GPU_IR_SHR, value, operand);
	high_bits = ir_select(builder, aligned, zero, high_bits);
	has_high = ir_value(builder, DRV_GPU_IR_INE, high_mask, zero);
	write_high = ir_value(builder, DRV_GPU_IR_AND, inside, has_high);
	next = ir_value(builder, DRV_GPU_IR_IADD, word, four);
	operand = ir_value(builder, DRV_GPU_IR_INOT, high_mask, 0U);
	ir_atomic(builder, DRV_GPU_IR_ATOMIC_AND, META_DESTINATION, next, operand, write_high);
	ir_atomic(builder, DRV_GPU_IR_ATOMIC_OR, META_DESTINATION, next, high_bits, write_high);
}

/*
 * Writes the query program, as
 *
 *   q = first + group * 64 + local;  inside = q < count;  (q = 0 outside)
 *   slot = source + 16 q: availability at slot + 8, then (after an acquire
 *   fence) the value's two words at slot;
 *   d = destination + q * stride;
 *   the value (low word, and the high word for 64 bits) at d when it is
 *   available or ANY is set; the availability (1 or 0; a 64-bit word for
 *   64 bits) after it with AVAILABILITY.
 */
static void
build_query(
	struct meta_builder *builder)
{
	uint32_t inside;
	uint32_t index;
	uint32_t zero;
	uint32_t one;
	uint32_t four;
	uint32_t eight;
	uint32_t slot;
	uint32_t low;
	uint32_t high;
	uint32_t available;
	uint32_t flags;
	uint32_t wide;
	uint32_t with_availability;
	uint32_t any;
	uint32_t destination;
	uint32_t write_value;
	uint32_t write_high;
	uint32_t write_availability;
	uint32_t write_pad;
	uint32_t at;
	uint32_t operand;

	/* The query of the invocation, and whether it is one of the copy's. */
	index = build_index(builder, GHCI_VK_META_QUERY_FIRST, GHCI_VK_META_QUERY_COUNT, &inside);

	/* The constants. */
	zero = ir_constant(builder, 0U);
	one = ir_constant(builder, 1U);
	four = ir_constant(builder, 4U);
	eight = ir_constant(builder, 8U);

	/*
	 * The slot: the availability first, then an acquire fence, then the
	 * value's two words, so that a value read as available is the one
	 * its availability was written after.
	 *
	 * Procedure referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
	 * 12.8, Global memory: fence 4 (acquire) before reading data another
	 * agent wrote).
	 */
	operand = ir_constant(builder, META_QUERY_SLOT_BYTES);
	slot = ir_value(builder, DRV_GPU_IR_IMUL, index, operand);
	operand = ir_push(builder, GHCI_VK_META_QUERY_SOURCE);
	slot = ir_value(builder, DRV_GPU_IR_IADD, slot, operand);
	operand = ir_value(builder, DRV_GPU_IR_IADD, slot, eight);
	operand = ir_load(builder, META_SOURCE, operand);
	available = ir_value(builder, DRV_GPU_IR_INE, operand, zero);
	ir_fence(builder, DRV_GPU_IR_FENCE_ACQUIRE);
	low = ir_load(builder, META_SOURCE, slot);
	operand = ir_value(builder, DRV_GPU_IR_IADD, slot, four);
	high = ir_load(builder, META_SOURCE, operand);

	/* The flags. */
	flags = ir_push(builder, GHCI_VK_META_QUERY_FLAGS);
	operand = ir_constant(builder, GHCI_VK_META_QUERY_64);
	operand = ir_value(builder, DRV_GPU_IR_IAND, flags, operand);
	wide = ir_value(builder, DRV_GPU_IR_INE, operand, zero);
	operand = ir_constant(builder, GHCI_VK_META_QUERY_AVAILABILITY);
	operand = ir_value(builder, DRV_GPU_IR_IAND, flags, operand);
	with_availability = ir_value(builder, DRV_GPU_IR_INE, operand, zero);
	operand = ir_constant(builder, GHCI_VK_META_QUERY_ANY);
	operand = ir_value(builder, DRV_GPU_IR_IAND, flags, operand);
	any = ir_value(builder, DRV_GPU_IR_INE, operand, zero);

	/* The query's results in the destination. */
	operand = ir_push(builder, GHCI_VK_META_QUERY_STRIDE);
	destination = ir_value(builder, DRV_GPU_IR_IMUL, index, operand);
	operand = ir_push(builder, GHCI_VK_META_QUERY_DESTINATION);
	destination = ir_value(builder, DRV_GPU_IR_IADD, destination, operand);

	/* The value, when it is available or any value is asked for. */
	operand = ir_value(builder, DRV_GPU_IR_OR, available, any);
	write_value = ir_value(builder, DRV_GPU_IR_AND, inside, operand);
	write_high = ir_value(builder, DRV_GPU_IR_AND, write_value, wide);
	ir_store(builder, META_DESTINATION, destination, low, write_value);
	operand = ir_value(builder, DRV_GPU_IR_IADD, destination, four);
	ir_store(builder, META_DESTINATION, operand, high, write_high);

	/* The availability after the value, a 64-bit 1 or 0 for 64-bit results. */
	write_availability = ir_value(builder, DRV_GPU_IR_AND, inside, with_availability);
	write_pad = ir_value(builder, DRV_GPU_IR_AND, write_availability, wide);
	operand = ir_select(builder, wide, eight, four);
	at = ir_value(builder, DRV_GPU_IR_IADD, destination, operand);
	operand = ir_select(builder, available, one, zero);
	ir_store(builder, META_DESTINATION, at, operand, write_availability);
	operand = ir_constant(builder, 12U);
	at = ir_value(builder, DRV_GPU_IR_IADD, destination, operand);
	ir_store(builder, META_DESTINATION, at, zero, write_pad);
}

/*
 * Writes a draw's vertex program: one triangle that covers the viewport,
 * made from the vertex index alone,
 *
 *   p = vec2(index & 1, index >> 1) * 4 - 1;   (-1, -1), (3, -1), (-1, 3)
 *   gl_Position = vec4(p, 0, 1);
 *
 * and for a blit the source's coordinate at location 0 (build_blit_axis).
 */
static void
build_vertex(
	struct meta_builder *builder,
	int blit)
{
	uint32_t index;
	uint32_t corner[2];
	uint32_t position[2];
	uint32_t axis;
	uint32_t four;
	uint32_t one;
	uint32_t operand;

	/* The corner of the vertex. */
	index = ir_input(builder, DRV_GPU_SHADER_LOCATION_VERTEX_INDEX, 0U);
	operand = ir_constant(builder, 1U);
	corner[0] = ir_value(builder, DRV_GPU_IR_IAND, index, operand);
	corner[1] = ir_value(builder, DRV_GPU_IR_SHR, index, operand);

	/* Its position: 4 x corner - 1, at depth 0. */
	four = ir_float(builder, META_FLOAT_FOUR);
	one = ir_float(builder, META_FLOAT_ONE);
	for (axis = 0U; axis < 2U; axis++) {
		position[axis] = ir_value(builder, DRV_GPU_IR_U2F, corner[axis], 0U);
		position[axis] = ir_value(builder, DRV_GPU_IR_FMUL, position[axis], four);
		position[axis] = ir_value(builder, DRV_GPU_IR_FSUB, position[axis], one);
		ir_output(builder, DRV_GPU_IR_LOCATION_POSITION, axis, position[axis]);
	}
	operand = ir_float(builder, META_FLOAT_ZERO);
	ir_output(builder, DRV_GPU_IR_LOCATION_POSITION, 2U, operand);
	ir_output(builder, DRV_GPU_IR_LOCATION_POSITION, 3U, one);

	/* A blit's coordinate in the source. */
	if (!blit)
		return;
	operand = build_blit_axis(builder, position[0], GHCI_VK_META_BLIT_X, GHCI_VK_META_BLIT_SIZE);
	ir_output(builder, 0U, 0U, operand);
	operand = build_blit_axis(builder, position[1], GHCI_VK_META_BLIT_Y, GHCI_VK_META_BLIT_SIZE + 1U);
	ir_output(builder, 0U, 1U, operand);
	builder->outputs[0].location = 0U;
	builder->outputs[0].components = 2U;
	builder->ir.output_count = 1U;
}

/*
 * Writes the source's coordinate of one axis of a blit at a position of
 * the viewport (-1 to 1 across it), from the push constants: the
 * source's corners s0 and s1, the destination's d0 and d1, the
 * viewport's first pixel and end, and the source's size,
 *
 *   x = first + (position + 1) / 2 x (end - first);    (the destination's pixel)
 *   coordinate = (s0 + (x - d0) x (s1 - s0) / (d1 - d0)) / size.
 *
 * Returns its value.
 */
static uint32_t
build_blit_axis(
	struct meta_builder *builder,
	uint32_t position,
	uint32_t first_word,
	uint32_t size_word)
{
	uint32_t corner[6];
	uint32_t index;
	uint32_t pixel;
	uint32_t scale;
	uint32_t coordinate;
	uint32_t operand;

	/* The corners and the viewport, as floats. */
	for (index = 0U; index < 6U; index++) {
		operand = ir_push(builder, first_word + index);
		corner[index] = ir_value(builder, DRV_GPU_IR_I2F, operand, 0U);
	}

	/* The destination's pixel at the position. */
	operand = ir_float(builder, META_FLOAT_ONE);
	pixel = ir_value(builder, DRV_GPU_IR_FADD, position, operand);
	operand = ir_float(builder, META_FLOAT_HALF);
	pixel = ir_value(builder, DRV_GPU_IR_FMUL, pixel, operand);
	operand = ir_value(builder, DRV_GPU_IR_FSUB, corner[5], corner[4]);
	pixel = ir_value(builder, DRV_GPU_IR_FMUL, pixel, operand);
	pixel = ir_value(builder, DRV_GPU_IR_FADD, pixel, corner[4]);

	/* The source's coordinate there, normalized. */
	scale = ir_value(builder, DRV_GPU_IR_FSUB, corner[1], corner[0]);
	operand = ir_value(builder, DRV_GPU_IR_FSUB, corner[3], corner[2]);
	operand = ir_value(builder, DRV_GPU_IR_RCP, operand, 0U);
	scale = ir_value(builder, DRV_GPU_IR_FMUL, scale, operand);
	coordinate = ir_value(builder, DRV_GPU_IR_FSUB, pixel, corner[2]);
	coordinate = ir_value(builder, DRV_GPU_IR_FMUL, coordinate, scale);
	coordinate = ir_value(builder, DRV_GPU_IR_FADD, coordinate, corner[0]);
	operand = ir_push(builder, size_word);
	operand = ir_value(builder, DRV_GPU_IR_I2F, operand, 0U);
	operand = ir_value(builder, DRV_GPU_IR_RCP, operand, 0U);
	coordinate = ir_value(builder, DRV_GPU_IR_FMUL, coordinate, operand);

	/* Succeeded: the coordinate. */
	return coordinate;
}

/*
 * Writes a resolve's fragment program: the texels of samples 0 to
 * samples - 1 of the source at the pixel moved by the push constants,
 * averaged (one sample: as it is, an integer format's sample 0), to
 * colour 0.
 */
static void
build_resolve(
	struct meta_builder *builder,
	uint32_t samples)
{
	uint32_t pixel[2];
	uint32_t move[2];
	uint32_t sum[4];
	uint32_t texel;
	uint32_t first;
	uint32_t sample;
	uint32_t channel;
	uint32_t weight;
	uint32_t operand;

	/* The pixel (the centre's integer part), and how far the source's texel is from it. */
	operand = ir_input(builder, DRV_GPU_SHADER_LOCATION_FRAG_COORD, 0U);
	pixel[0] = ir_value(builder, DRV_GPU_IR_F2I, operand, 0U);
	operand = ir_input(builder, DRV_GPU_SHADER_LOCATION_FRAG_COORD, 1U);
	pixel[1] = ir_value(builder, DRV_GPU_IR_F2I, operand, 0U);
	move[0] = ir_push(builder, GHCI_VK_META_RESOLVE_DX);
	move[1] = ir_push(builder, GHCI_VK_META_RESOLVE_DY);

	/* Each sample: the fetch's parameters u, v, level 0 and the sample, values one after the other. */
	for (sample = 0U; sample < samples; sample++) {
		first = ir_value(builder, DRV_GPU_IR_IADD, pixel[0], move[0]);
		(void)ir_value(builder, DRV_GPU_IR_IADD, pixel[1], move[1]);
		(void)ir_constant(builder, 0U);
		(void)ir_constant(builder, sample);
		texel = ir_texture(builder, first, 4U, DRV_GPU_IR_TEXTURE_LD, DRV_GPU_IR_IMAGE_2D | DRV_GPU_IR_IMAGE_MULTISAMPLED);
		for (channel = 0U; channel < 4U; channel++) {
			if (sample == 0U)
				sum[channel] = texel + channel;
			else
				sum[channel] = ir_value(builder, DRV_GPU_IR_FADD, sum[channel], texel + channel);
		}
	}

	/* The average (one sample is its own), to colour 0. */
	weight = 0U;
	if (samples > 1U)
		weight = ir_float(builder, samples == 2U ? META_FLOAT_HALF : META_FLOAT_QUARTER);
	for (channel = 0U; channel < 4U; channel++) {
		if (samples > 1U)
			sum[channel] = ir_value(builder, DRV_GPU_IR_FMUL, sum[channel], weight);
		ir_output(builder, 0U, channel, sum[channel]);
	}
	builder->outputs[0].location = 0U;
	builder->outputs[0].components = 4U;
	builder->ir.output_count = 1U;
}

/* Writes a blit's fragment program: the source sampled at level 0 at the coordinate of location 0, to colour 0. */
static void
build_blit(
	struct meta_builder *builder)
{
	struct drv_gpu_shader_ir_inst *instruction;
	uint32_t coordinate[2];
	uint32_t level;
	uint32_t texel;
	uint32_t channel;

	/* The coordinate, and level 0. */
	coordinate[0] = ir_input(builder, 0U, 0U);
	coordinate[1] = ir_input(builder, 0U, 1U);
	builder->inputs[0].location = 0U;
	builder->inputs[0].components = 2U;
	builder->ir.input_count = 1U;
	level = ir_float(builder, META_FLOAT_ZERO);

	/* The sample of set 0, binding 0 (the sampler filters). */
	instruction = ir_next(builder);
	instruction->op = DRV_GPU_IR_SAMPLE_LOD;
	instruction->dst = builder->values;
	builder->values += 4U;
	instruction->src[0] = coordinate[0];
	instruction->src[1] = coordinate[1];
	instruction->src[2] = level;
	instruction->location = 0U;
	instruction->immediate = 0U;
	texel = instruction->dst;

	/* Its four channels to colour 0. */
	for (channel = 0U; channel < 4U; channel++)
		ir_output(builder, 0U, channel, texel + channel);
	builder->outputs[0].location = 0U;
	builder->outputs[0].components = 4U;
	builder->ir.output_count = 1U;
}

/*
 * Writes the element of an invocation: first + workgroup x 64 + local
 * index, with inside the Boolean of an element below count.  Returns the
 * element, 0 for an invocation outside (whose writes are predicated off
 * and whose reads stay within the buffers).
 */
static uint32_t
build_index(
	struct meta_builder *builder,
	uint32_t first_word,
	uint32_t count_word,
	uint32_t *inside)
{
	uint32_t group;
	uint32_t local;
	uint32_t index;
	uint32_t operand;

	/* The invocation's number in the dispatch, after the first. */
	group = ir_system(builder, DRV_GPU_IR_SYSTEM_GROUP_ID_X);
	operand = ir_constant(builder, GHCI_VK_META_GROUP);
	group = ir_value(builder, DRV_GPU_IR_IMUL, group, operand);
	local = ir_system(builder, DRV_GPU_IR_SYSTEM_LOCAL_INDEX);
	index = ir_value(builder, DRV_GPU_IR_IADD, group, local);
	operand = ir_push(builder, first_word);
	index = ir_value(builder, DRV_GPU_IR_IADD, index, operand);

	/* Inside the count, or element 0. */
	operand = ir_push(builder, count_word);
	*inside = ir_value(builder, DRV_GPU_IR_ULT, index, operand);
	operand = ir_constant(builder, 0U);
	index = ir_select(builder, *inside, index, operand);

	/* Succeeded: the element. */
	return index;
}

/* Writes the byte offset of an element from the push words at first_word: first + column x stride + row x pitch + slice x slice pitch. */
static uint32_t
build_address(
	struct meta_builder *builder,
	uint32_t column,
	uint32_t row,
	uint32_t slice,
	uint32_t first_word)
{
	uint32_t address;
	uint32_t operand;

	/* The first byte, then each step. */
	address = ir_push(builder, first_word);
	operand = ir_push(builder, first_word + 1U);
	operand = ir_value(builder, DRV_GPU_IR_IMUL, column, operand);
	address = ir_value(builder, DRV_GPU_IR_IADD, address, operand);
	operand = ir_push(builder, first_word + 2U);
	operand = ir_value(builder, DRV_GPU_IR_IMUL, row, operand);
	address = ir_value(builder, DRV_GPU_IR_IADD, address, operand);
	operand = ir_push(builder, first_word + 3U);
	operand = ir_value(builder, DRV_GPU_IR_IMUL, slice, operand);
	address = ir_value(builder, DRV_GPU_IR_IADD, address, operand);

	/* Succeeded: the offset. */
	return address;
}

/* Reports the next instruction of a program, cleared (a spare one when the list is full, which refuses the program). */
static struct drv_gpu_shader_ir_inst *
ir_next(
	struct meta_builder *builder)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* A full list: the last instruction is written again, and the program is refused. */
	if (builder->ir.instruction_count == META_INSTRUCTIONS_MAX) {
		builder->overflow = 1;
		builder->ir.instruction_count--;
	}

	/* The next one, cleared. */
	instruction = &builder->instructions[builder->ir.instruction_count++];
	kern_memset(instruction, 0, sizeof(*instruction));

	/* Succeeded: the instruction to fill. */
	return instruction;
}

/* Appends an operation of one or two sources.  Returns its value. */
static uint32_t
ir_value(
	struct meta_builder *builder,
	enum drv_gpu_shader_ir_op op,
	uint32_t left,
	uint32_t right)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The operation into a new value. */
	instruction = ir_next(builder);
	instruction->op = op;
	instruction->dst = builder->values++;
	instruction->src[0] = left;
	instruction->src[1] = right;

	/* Succeeded: its value. */
	return instruction->dst;
}

/*
 * Appends one sampler message to set 0, binding 0 with the count
 * parameters from the value first on.  Returns the first of its four
 * values.
 */
static uint32_t
ir_texture(
	struct meta_builder *builder,
	uint32_t first,
	uint32_t count,
	uint32_t message,
	uint32_t shape)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The message into four new values. */
	instruction = ir_next(builder);
	instruction->op = DRV_GPU_IR_TEXTURE;
	instruction->dst = builder->values;
	builder->values += 4U;
	instruction->src[0] = first;
	instruction->src[1] = count;
	instruction->component = message;
	instruction->image_shape = shape;
	instruction->location = 0U;
	instruction->immediate = 0U;

	/* Succeeded: its first value. */
	return instruction->dst;
}

/* Appends the read of an input's component (or a built-in's, by its location).  Returns its value. */
static uint32_t
ir_input(
	struct meta_builder *builder,
	uint32_t location,
	uint32_t component)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The component into a new value. */
	instruction = ir_next(builder);
	instruction->op = DRV_GPU_IR_LOAD_INPUT;
	instruction->dst = builder->values++;
	instruction->location = location;
	instruction->component = component;

	/* Succeeded: its value. */
	return instruction->dst;
}

/* Appends the store of a value to an output's component. */
static void
ir_output(
	struct meta_builder *builder,
	uint32_t location,
	uint32_t component,
	uint32_t value)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The store. */
	instruction = ir_next(builder);
	instruction->op = DRV_GPU_IR_STORE_OUTPUT;
	instruction->location = location;
	instruction->component = component;
	instruction->src[0] = value;
}

/* Appends a float constant (its bits).  Returns its value. */
static uint32_t
ir_float(
	struct meta_builder *builder,
	uint32_t bits)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The constant into a new value. */
	instruction = ir_next(builder);
	instruction->op = DRV_GPU_IR_CONST;
	instruction->dst = builder->values++;
	instruction->immediate = bits;

	/* Succeeded: its value. */
	return instruction->dst;
}

/* Appends a 32-bit integer constant.  Returns its value. */
static uint32_t
ir_constant(
	struct meta_builder *builder,
	uint32_t bits)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The constant into a new value. */
	instruction = ir_next(builder);
	instruction->op = DRV_GPU_IR_ICONST;
	instruction->dst = builder->values++;
	instruction->immediate = bits;

	/* Succeeded: its value. */
	return instruction->dst;
}

/* Appends the read of a push-constant word.  Returns its value. */
static uint32_t
ir_push(
	struct meta_builder *builder,
	uint32_t word)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The word into a new value. */
	instruction = ir_next(builder);
	instruction->op = DRV_GPU_IR_LOAD_PUSH;
	instruction->dst = builder->values++;
	instruction->immediate = word * 4U;

	/* Succeeded: its value. */
	return instruction->dst;
}

/* Appends the read of a compute invocation's built-in value (DRV_GPU_IR_SYSTEM_*).  Returns its value. */
static uint32_t
ir_system(
	struct meta_builder *builder,
	uint32_t component)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The value into a new value. */
	instruction = ir_next(builder);
	instruction->op = DRV_GPU_IR_LOAD_SYSTEM;
	instruction->dst = builder->values++;
	instruction->component = component;

	/* Succeeded: its value. */
	return instruction->dst;
}

/* Appends a choice by a Boolean.  Returns its value. */
static uint32_t
ir_select(
	struct meta_builder *builder,
	uint32_t condition,
	uint32_t chosen,
	uint32_t other)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The choice into a new value. */
	instruction = ir_next(builder);
	instruction->op = DRV_GPU_IR_SELECT;
	instruction->dst = builder->values++;
	instruction->src[0] = condition;
	instruction->src[1] = chosen;
	instruction->src[2] = other;

	/* Succeeded: its value. */
	return instruction->dst;
}

/* Appends the read of a buffer's word at a byte offset.  Returns its value. */
static uint32_t
ir_load(
	struct meta_builder *builder,
	uint32_t buffer,
	uint32_t offset)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The word into a new value. */
	instruction = ir_next(builder);
	instruction->op = DRV_GPU_IR_LOAD_STORAGE;
	instruction->dst = builder->values++;
	instruction->location = buffer;
	instruction->src[0] = offset;

	/* Succeeded: its value. */
	return instruction->dst;
}

/* Appends the write of a buffer's word at a byte offset, by the invocations where predicate holds. */
static void
ir_store(
	struct meta_builder *builder,
	uint32_t buffer,
	uint32_t offset,
	uint32_t value,
	uint32_t predicate)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The predicated store. */
	instruction = ir_next(builder);
	instruction->op = DRV_GPU_IR_STORE_STORAGE;
	instruction->location = buffer;
	instruction->component = 1U;
	instruction->src[0] = offset;
	instruction->src[1] = value;
	instruction->src[2] = predicate;
}

/* Appends an atomic operation on a buffer's word, by the invocations where predicate holds (its old value is not read). */
static void
ir_atomic(
	struct meta_builder *builder,
	uint32_t operation,
	uint32_t buffer,
	uint32_t offset,
	uint32_t value,
	uint32_t predicate)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The predicated atomic, its old value a value nothing reads. */
	instruction = ir_next(builder);
	instruction->op = DRV_GPU_IR_ATOMIC;
	instruction->dst = builder->values++;
	instruction->location = buffer;
	instruction->immediate = operation;
	instruction->component = DRV_GPU_IR_ATOMIC_PREDICATED;
	instruction->src[0] = offset;
	instruction->src[1] = value;
	instruction->src[2] = predicate;
}

/* Appends a memory fence of the fences (DRV_GPU_IR_FENCE_*). */
static void
ir_fence(
	struct meta_builder *builder,
	uint32_t fences)
{
	struct drv_gpu_shader_ir_inst *instruction;

	/* The fence. */
	instruction = ir_next(builder);
	instruction->op = DRV_GPU_IR_FENCE;
	instruction->immediate = fences;
}

/*
 * Places the compiled programs of a transfer in a device's code heap:
 * the pipeline state record (a compute pipeline, or a draw's), then the
 * compute or vertex program's code, then a draw's fragment program's.
 * Returns 0, ENOMEM, EIO, or ENOTSUP for a compute program whose two
 * buffers are not in its resource table, or a draw that does not read
 * texture and sampler slot 0.
 */
static int
meta_place(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device,
	struct ghci_gsi1_program *const *programs,
	struct ghci_vk_meta_pipeline *placed)
{
	struct ghci_vk_heap_block *block;
	uint32_t code_at;
	uint32_t fragment_at;
	uint32_t bytes;
	uint32_t offset;
	int error;

	/* What the programs read. */
	error = meta_entries(programs, placed);
	if (error != 0)
		return error;

	/* The run: the record, then the code of each program. */
	bytes = meta_layout(programs, &code_at, &fragment_at);
	error = ghci_vk_heap_take(session, device, GHCI_VK_HEAP_CODE, bytes, META_PSO_ALIGN, &block, &offset);
	if (error == ENOMEM)
		return ENOMEM;
	if (error != 0)
		return EIO;

	/* The record and the code, written at the run's place in the device's session. */
	error = meta_write(programs, block->allocation->view + offset, block->mapping->va + offset, code_at, fragment_at, placed);
	if (error != 0) {
		ghci_vk_heap_give(block, offset);
		return error;
	}
	placed->block = block;
	placed->offset = offset;

	/* Succeeded: the programs run in the device's session. */
	return 0;
}

/*
 * Finds what the programs of a transfer read: a compute program's two
 * buffers in its resource table (their offsets and the table's bytes go
 * into placed), or a draw's one texture and sampler.  Returns 0, or
 * ENOTSUP for programs that read something else.
 */
static int
meta_entries(
	struct ghci_gsi1_program *const *programs,
	struct ghci_vk_meta_pipeline *placed)
{
	int error;

	/* A draw reads texture and sampler slot 0, and no buffer. */
	if (programs[1] != NULL) {
		if (programs[1]->bindings.texture_count != 1U)
			return ENOTSUP;
		if (programs[1]->bindings.sampler_count != 1U)
			return ENOTSUP;
		if (programs[1]->bindings.resource_count != 0U)
			return ENOTSUP;
		return 0;
	}

	/* A compute program reads its source and writes its destination. */
	error = meta_entry(programs[0], 0U, &placed->source_entry);
	if (error != 0)
		return error;
	error = meta_entry(programs[0], 1U, &placed->destination_entry);
	if (error != 0)
		return error;
	placed->resource_bytes = programs[0]->bindings.resource_bytes;

	/* Succeeded: the entries are known. */
	return 0;
}

/*
 * Lays out the run of a transfer's programs: the pipeline state record at
 * 0, the compute or vertex program's code at *code_at, and a draw's
 * fragment program's at *fragment_at (0 for none).  Returns the run's
 * bytes.
 */
static uint32_t
meta_layout(
	struct ghci_gsi1_program *const *programs,
	uint32_t *code_at,
	uint32_t *fragment_at)
{
	uint32_t bytes;

	/* The record, then the first program's code. */
	*code_at = (GHCI_PSO_BYTES + META_CODE_ALIGN - 1U) & ~(META_CODE_ALIGN - 1U);
	bytes = *code_at + programs[0]->word_count * 4U;

	/* A draw's fragment program after it. */
	*fragment_at = 0U;
	if (programs[1] != NULL) {
		*fragment_at = (bytes + META_CODE_ALIGN - 1U) & ~(META_CODE_ALIGN - 1U);
		bytes = *fragment_at + programs[1]->word_count * 4U;
	}

	/* Succeeded: the run's bytes. */
	return bytes;
}

/*
 * Writes a transfer's run (laid out by meta_layout) through the host's
 * view of it, whose GPU address is base, and fills placed with the
 * addresses.  Returns 0, or ENOTSUP when the pipeline state cannot hold
 * the programs.
 */
static int
meta_write(
	struct ghci_gsi1_program *const *programs,
	uint8_t *view,
	uint64_t base,
	uint32_t code_at,
	uint32_t fragment_at,
	struct ghci_vk_meta_pipeline *placed)
{
	struct ghci_pso pso;
	uint32_t bytes;
	int error;

	/* The code of each program, the rest zero. */
	bytes = code_at + programs[0]->word_count * 4U;
	if (programs[1] != NULL)
		bytes = fragment_at + programs[1]->word_count * 4U;
	kern_memset(view, 0, bytes);
	kern_memcpy(view + code_at, programs[0]->words, programs[0]->word_count * 4U);
	if (programs[1] != NULL)
		kern_memcpy(view + fragment_at, programs[1]->words, programs[1]->word_count * 4U);

	/* The pipeline state, and its blend word of colour attachment 0 (little endian). */
	meta_pso(programs, base + code_at, base + fragment_at, &pso);
	error = ghci_pso_encode(view, &pso);
	if (error != 0)
		return ENOTSUP;
	placed->blend_word = (uint32_t)view[GHCI_PSO_BLEND];
	placed->blend_word |= (uint32_t)view[GHCI_PSO_BLEND + 1U] << 8;
	placed->blend_word |= (uint32_t)view[GHCI_PSO_BLEND + 2U] << 16;
	placed->blend_word |= (uint32_t)view[GHCI_PSO_BLEND + 3U] << 24;

	/* The placed programs. */
	placed->code_va = base + code_at;
	placed->code_words = programs[0]->word_count;
	if (programs[1] != NULL) {
		placed->fragment_va = base + fragment_at;
		placed->fragment_words = programs[1]->word_count;
	}
	placed->pso_va = base;

	/* Succeeded: the run holds the programs. */
	return 0;
}

/*
 * Places the copy program in session 0 for the copies of rows: one run of
 * host memory with the record, the code and the resource table after
 * them, mapped executable and read-only to the GPU.  Returns 0, ENOMEM,
 * EIO, or ENOTSUP for a program the compiler refuses or the pipeline
 * state cannot hold.  The caller holds rows->lock.
 */
static int
rows_place(
	struct ghci_device *gpu,
	struct ghci_vk_rows *rows)
{
	struct ghci_gsi1_program *programs[GHCI_VK_META_PROGRAMS];
	struct ghci_vk_meta_pipeline pipeline;
	struct ghci_allocation *memory;
	struct ghci_mapping *mapping;
	uint32_t code_at;
	uint32_t fragment_at;
	uint32_t table_at;
	uint32_t bytes;
	int error;

	/* The GPU's copy program, and the buffers it reads. */
	error = meta_programs(gpu, GHCI_VK_META_COPY, programs);
	if (error != 0)
		return error;
	kern_memset(&pipeline, 0, sizeof(pipeline));
	error = meta_entries(programs, &pipeline);
	if (error != 0) {
		meta_refuse(gpu, GHCI_VK_META_COPY);
		return error;
	}

	/* The run: the record and the code, then the table on its own line. */
	bytes = meta_layout(programs, &code_at, &fragment_at);
	table_at = (bytes + META_TABLE_ALIGN - 1U) & ~(META_TABLE_ALIGN - 1U);
	bytes = table_at + pipeline.resource_bytes;

	/* The memory, held by the run. */
	error = ghci_memory_alloc(gpu, bytes, 0U, &memory);
	if (error != 0)
		return error;
	mutex_lock(&gpu->lock);

	ghci_memory_hold(memory);

	mutex_unlock(&gpu->lock);

	/* Its mapping in session 0: executable, and only read by the GPU. */
	error = ghci_vm_map(gpu, &gpu->vm0, memory, GHCI_VM_FLAG_EXEC | GHCI_VM_MAP_NO_WRITE, &mapping);
	if (error != 0) {
		mutex_lock(&gpu->lock);

		ghci_memory_unhold(gpu, memory);

		mutex_unlock(&gpu->lock);
		return error;
	}

	/* The record and the code at the mapping's address, which the pipeline state may refuse. */
	kern_memset(memory->view, 0, bytes);
	error = meta_write(programs, memory->view, mapping->va, code_at, fragment_at, &pipeline);
	if (error == ENOTSUP)
		meta_refuse(gpu, GHCI_VK_META_COPY);
	if (error != 0) {
		ghci_vm_unmap(gpu, mapping);
		mutex_lock(&gpu->lock);

		ghci_memory_unhold(gpu, memory);

		mutex_unlock(&gpu->lock);
		return error;
	}

	/* Succeeded: the program runs in session 0. */
	rows->memory = memory;
	rows->mapping = mapping;
	rows->table_at = table_at;
	rows->pipeline = pipeline;
	return 0;
}

/*
 * Fills the pipeline state of a transfer's programs: a compute pipeline
 * of the program's registers, workgroup and shared memory; or a draw of
 * a triangle list with no culling, depth, stencil or blending, every
 * component of colour 0 written, one sample, the programs' registers,
 * varyings and fragment flags, and no vertex attribute.
 */
static void
meta_pso(
	struct ghci_gsi1_program *const *programs,
	uint64_t code,
	uint64_t fragment,
	struct ghci_pso *pso)
{
	/* A compute pipeline. */
	kern_memset(pso, 0, sizeof(*pso));
	if (programs[1] == NULL) {
		pso->cs_code = code;
		pso->regs_cs = programs[0]->used_regs;
		pso->local_size[0] = programs[0]->local_size[0];
		pso->local_size[1] = programs[0]->local_size[1];
		pso->local_size[2] = programs[0]->local_size[2];
		pso->lds_bytes = programs[0]->lds_bytes;
		return;
	}

	/* A draw. */
	pso->vs_code = code;
	pso->fs_code = fragment;
	pso->topology = META_TOPOLOGY_TRIANGLES;
	pso->sample_mask = META_SAMPLE_MASK_ALL;
	pso->line_width_fx4 = META_LINE_WIDTH_ONE;
	pso->write_mask = META_WRITE_ALL;
	pso->regs_vs = programs[0]->used_regs;
	pso->regs_fs = programs[1]->used_regs;
	pso->varyings = programs[0]->varying_count;
	pso->fs_centroid = (programs[1]->flags & GHCI_GSI1_FLAG_CENTROID) != 0U ? 1U : 0U;
	pso->fs_depth_export = (programs[1]->flags & GHCI_GSI1_FLAG_DEPTH_EXPORT) != 0U ? 1U : 0U;
	pso->fs_discard = (programs[1]->flags & GHCI_GSI1_FLAG_DISCARD) != 0U ? 1U : 0U;
}

/* Finds the resource table offset of a program's storage buffer of set 0, binding.  Returns 0, or ENOTSUP. */
static int
meta_entry(
	const struct ghci_gsi1_program *program,
	uint32_t binding,
	uint32_t *offset)
{
	const struct ghci_gsi1_resource *resource;
	uint32_t index;

	/* Each entry in turn. */
	for (index = 0U; index < program->bindings.resource_count && index < GHCI_GSI1_RESOURCES; index++) {
		resource = &program->bindings.resource[index];
		if (resource->kind == GHCI_GSI1_RESOURCE_STORAGE_BUFFER && resource->set == 0U && resource->binding == binding) {
			*offset = resource->offset;
			return 0;
		}
	}

	/* A program that does not read the buffer. */
	return ENOTSUP;
}

/*
 * Reports a buffer descriptor's reach of the bytes [address, address +
 * bytes): from the word of the first byte, whole words, below 4 GiB.
 * Returns 0, or EINVAL.
 */
static int
meta_span(
	uint64_t address,
	uint64_t bytes,
	uint64_t *base,
	uint32_t *size)
{
	uint64_t start;
	uint64_t end;

	/* From the word of the first byte to the end of the word of the last. */
	start = address & ~(uint64_t)3U;
	end = (address + bytes + 3U) & ~(uint64_t)3U;
	if (bytes == 0U || address >= META_ADDRESS_LIMIT || bytes > META_ADDRESS_LIMIT || end > META_ADDRESS_LIMIT)
		return EINVAL;
	if (end - start > UINT32_MAX)
		return EINVAL;

	/* Succeeded: the reach. */
	*base = start;
	*size = (uint32_t)(end - start);
	return 0;
}

/*
 * Finds the GPU's programs of a transfer, compiling them now when no
 * caller has before.  Returns 0 with the programs (kept to the detach),
 * ENOMEM, or ENOTSUP for a kind the compiler refuses (the GPU remembers
 * that, and the kind is not compiled again).
 */
static int
meta_programs(
	struct ghci_device *gpu,
	enum ghci_vk_meta_kind kind,
	struct ghci_gsi1_program **programs)
{
	struct ghci_gsi1_program *compiled[GHCI_VK_META_PROGRAMS];
	struct ghci_vk *vk;
	int refused;
	int error;

	/* The programs compiled before, unless the compiler refused them. */
	vk = gpu->vk;
	mutex_lock(&gpu->lock);

	programs[0] = vk->meta[kind][0];
	programs[1] = vk->meta[kind][1];
	refused = vk->meta_refused[kind];

	mutex_unlock(&gpu->lock);
	if (refused)
		return ENOTSUP;
	if (programs[0] != NULL)
		return 0;

	/* The compile, outside the lock. */
	error = ghci_vk_meta_compile(&gpu->caps, kind, compiled);
	if (error == ENOTSUP)
		meta_refuse(gpu, kind);
	if (error != 0)
		return error;

	/* The first compile kept; another caller's of the same time goes. */
	mutex_lock(&gpu->lock);

	if (vk->meta[kind][0] == NULL) {
		vk->meta[kind][0] = compiled[0];
		vk->meta[kind][1] = compiled[1];
	} else {
		ghci_gsi1_program_free(compiled[0]);
		ghci_gsi1_program_free(compiled[1]);
	}
	programs[0] = vk->meta[kind][0];
	programs[1] = vk->meta[kind][1];

	mutex_unlock(&gpu->lock);

	/* Succeeded: the programs. */
	return 0;
}

/* Marks a kind of the GPU's transfer programs as one the compiler refuses. */
static void
meta_refuse(
	struct ghci_device *gpu,
	enum ghci_vk_meta_kind kind)
{
	struct ghci_vk *vk;

	/* Once, under the device's lock. */
	vk = gpu->vk;
	mutex_lock(&gpu->lock);

	vk->meta_refused[kind] = 1;

	mutex_unlock(&gpu->lock);
}
