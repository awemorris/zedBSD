/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The encoders of the GHCI 1.0 commands and of the records the GPU
 * reads from memory, and the comparison of 64-bit fence values.
 *
 * Every command is written dword by dword and every record byte by byte
 * in little-endian order, so the layout does not depend on the compiler's
 * structure padding or the host's byte order.  Floating-point values
 * arrive as their bit patterns; nothing here computes with them.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "ghci-cmd.h"
#include "ghci-regs.h"

/* The largest client session number. */
#define SESSION_MAX			15U

/* The descriptor sets a table can be bound to. */
#define DESCRIPTOR_SETS			4U

/* The topologies the pipeline state accepts: POINT to TRIANGLE_STRIP, and PATCH_LIST with tessellation. */
#define TOPOLOGY_MAX			4U

/* The largest texture extent and mip count. */
#define EXTENT_MAX			16384U
#define MIP_LEVELS_MAX			15U

/* The limits of the pipeline state's fields. */
#define REGISTERS_MAX			63U
#define VARYINGS_MAX			16U
#define LDS_BYTES_MAX			16384U
#define ATTRIBUTE_OFFSET_MAX		524287U
#define LOCAL_SIZE_XY_MAX		1024U
#define LOCAL_SIZE_Z_MAX		64U

static void put64(uint32_t *out, uint64_t value);
static uint32_t pack_xy(uint32_t x, uint32_t y);
static void store8(uint8_t *out, uint32_t offset, uint32_t value);
static void store16(uint8_t *out, uint32_t offset, uint32_t value);
static void store32(uint8_t *out, uint32_t offset, uint32_t value);
static void store64(uint8_t *out, uint32_t offset, uint64_t value);
static void clear_bytes(uint8_t *out, uint32_t bytes);
static int sample_count_valid(uint32_t samples);
static uint32_t pso_flags_word(const struct ghci_pso *pso);
static uint32_t pso_raster_word(const struct ghci_pso *pso);
static uint32_t pso_depth_stencil_word(const struct ghci_pso *pso);
static uint32_t pso_blend_word(const struct ghci_pso *pso);
static int pso_check(const struct ghci_pso *pso, int patches);
static void pso_store(uint8_t *out, const struct ghci_pso *pso, uint32_t flags);
static int pso_ext_check(const struct ghci_pso_ext *ext, int *tessellation);
static int pso_ext_tess_check(const struct ghci_pso_ext *ext);
static int pso_ext_gs_check(const struct ghci_pso_ext *ext);
static void pso_ext_store(uint8_t *out, const struct ghci_pso_ext *ext);
static void store_attachment(uint8_t *out, uint32_t offset, const struct ghci_rp_attachment *attachment);
static uint32_t buffer_image(uint32_t *out, uint32_t opcode, uint64_t first_va, uint64_t second_va, uint32_t first_pitch, uint32_t second_pitch, const struct ghci_slice *image, const struct ghci_transfer *transfer);

/*
 * Builds a packet header from its opcode, size and flags.
 *
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 7.1, [7:0] opcode, [15:8] dwords, [31:16] flags).
 */
uint32_t
ghci_cmd_header(
	uint32_t opcode,
	uint32_t dwords,
	uint32_t flags)
{
	uint32_t header;

	/* Places the three fields. */
	header = opcode & GHCI_HEADER_OPCODE_MASK;
	header |= (dwords & GHCI_HEADER_DWORDS_MASK) << GHCI_HEADER_DWORDS_SHIFT;
	header |= (flags & 0xFFFFU) << GHCI_HEADER_FLAGS_SHIFT;

	/* Succeeded: the header word. */
	return header;
}

/*
 * Writes one NOP of a size, the padding before the end of the ring.
 */
uint32_t
ghci_cmd_nop(
	uint32_t *out,
	uint32_t dwords)
{
	uint32_t index;

	/* Refuses a size a header cannot hold. */
	if (dwords == 0U || dwords > GHCI_PACKET_DWORDS_MAX)
		return 0U;

	/* Writes the header and zeroes the payload the CP skips. */
	out[0] = ghci_cmd_header(GHCI_OP_NOP, dwords, 0U);
	for (index = 1U; index < dwords; index++)
		out[index] = 0U;

	/* Succeeded: the NOP fills the dwords asked for. */
	return dwords;
}

/*
 * Writes a VM_MAP of host or local pages into a session's address space.
 */
uint32_t
ghci_cmd_vm_map(
	uint32_t *out,
	uint32_t session,
	uint32_t flags,
	uint64_t va,
	uint64_t bus,
	uint64_t bytes)
{
	/* Refuses a session number the GPU does not have. */
	if (session > SESSION_MAX)
		return 0U;

	/* Refuses an empty range and addresses not on a 4 KiB page. */
	if (bytes == 0U)
		return 0U;
	if (((va | bus | bytes) & (GHCI_PAGE_BYTES - 1U)) != 0U)
		return 0U;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 7.3, CMD_VM_MAP: session, flags, reserved, VA, phys, size).
	 */
	out[0] = ghci_cmd_header(GHCI_OP_VM_MAP, GHCI_DW_VM_MAP, 0U);
	out[1] = session;
	out[2] = flags;
	out[3] = 0U;
	put64(&out[4], va);
	put64(&out[6], bus);
	put64(&out[8], bytes);

	/* Succeeded: ten dwords. */
	return GHCI_DW_VM_MAP;
}

/*
 * Writes a VM_UNMAP of a range of a session's address space.
 */
uint32_t
ghci_cmd_vm_unmap(
	uint32_t *out,
	uint32_t session,
	uint64_t va,
	uint64_t bytes)
{
	/* Refuses a session number the GPU does not have. */
	if (session > SESSION_MAX)
		return 0U;

	/* Refuses an empty range and addresses not on a 4 KiB page. */
	if (bytes == 0U)
		return 0U;
	if (((va | bytes) & (GHCI_PAGE_BYTES - 1U)) != 0U)
		return 0U;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 7.3, CMD_VM_UNMAP: session, flags, reserved, VA, size).
	 */
	out[0] = ghci_cmd_header(GHCI_OP_VM_UNMAP, GHCI_DW_VM_UNMAP, 0U);
	out[1] = session;
	out[2] = 0U;
	out[3] = 0U;
	put64(&out[4], va);
	put64(&out[6], bytes);

	/* Succeeded: eight dwords. */
	return GHCI_DW_VM_UNMAP;
}

/*
 * Writes a SESSION_OPEN of a client session.
 */
uint32_t
ghci_cmd_session_open(
	uint32_t *out,
	uint32_t session,
	uint32_t flags)
{
	/* Refuses session 0, which is always open, and numbers past 15. */
	if (session == 0U || session > SESSION_MAX)
		return 0U;

	/* Writes the session and its flags (bit 0, a protected context). */
	out[0] = ghci_cmd_header(GHCI_OP_SESSION_OPEN, GHCI_DW_SESSION_OPEN, 0U);
	out[1] = session;
	out[2] = flags;
	out[3] = 0U;

	/* Succeeded: four dwords. */
	return GHCI_DW_SESSION_OPEN;
}

/*
 * Writes a SESSION_CLOSE of a client session.
 */
uint32_t
ghci_cmd_session_close(
	uint32_t *out,
	uint32_t session)
{
	/* Refuses session 0, which never closes, and numbers past 15. */
	if (session == 0U || session > SESSION_MAX)
		return 0U;

	/* Writes the session. */
	out[0] = ghci_cmd_header(GHCI_OP_SESSION_CLOSE, GHCI_DW_SESSION_CLOSE, 0U);
	out[1] = session;

	/* Succeeded: two dwords. */
	return GHCI_DW_SESSION_CLOSE;
}

/*
 * Writes a BIND_SESSION, which selects the session later work runs in.
 */
uint32_t
ghci_cmd_bind_session(
	uint32_t *out,
	uint32_t session)
{
	/* Refuses a session number the GPU does not have. */
	if (session > SESSION_MAX)
		return 0U;

	/* Writes the session. */
	out[0] = ghci_cmd_header(GHCI_OP_BIND_SESSION, GHCI_DW_BIND_SESSION, 0U);
	out[1] = session;

	/* Succeeded: two dwords. */
	return GHCI_DW_BIND_SESSION;
}

/*
 * Writes a QUERY_CAPS, which has the command processor write its
 * capability block to a buffer of whole 64-byte lines at a bus address,
 * no larger than the block.
 */
uint32_t
ghci_cmd_query_caps(
	uint32_t *out,
	uint64_t bus,
	uint32_t bytes)
{
	/* Refuses a buffer that is empty, not in whole lines or larger than the block. */
	if (bytes == 0U || (bytes % GHCI_QUERY_CAPS_ALIGN) != 0U)
		return 0U;
	if (bytes > GHCI_QUERY_CAPS_BYTES_MAX)
		return 0U;
	if ((bus % GHCI_QUERY_CAPS_ALIGN) != 0U)
		return 0U;

	/* Writes the address, the size and the reserved word. */
	out[0] = ghci_cmd_header(GHCI_OP_QUERY_CAPS, GHCI_DW_QUERY_CAPS, 0U);
	put64(&out[1], bus);
	out[3] = bytes;
	out[4] = 0U;

	/* Succeeded: five dwords. */
	return GHCI_DW_QUERY_CAPS;
}

/*
 * Writes a BIND_PIPELINE of a pipeline state in the session's memory.
 */
uint32_t
ghci_cmd_bind_pipeline(
	uint32_t *out,
	uint64_t pso_va)
{
	/* Refuses a pipeline state not on 64 bytes. */
	if ((pso_va & 63U) != 0U)
		return 0U;

	/* Writes the address. */
	out[0] = ghci_cmd_header(GHCI_OP_BIND_PIPELINE, GHCI_DW_BIND_PIPELINE, 0U);
	put64(&out[1], pso_va);

	/* Succeeded: three dwords. */
	return GHCI_DW_BIND_PIPELINE;
}

/*
 * Writes a BIND_VERTEX_BUFFERS of a run of consecutive bindings.
 */
uint32_t
ghci_cmd_bind_vertex_buffers(
	uint32_t *out,
	uint32_t first,
	uint32_t count,
	const struct ghci_vertex_binding *bindings)
{
	uint32_t index;
	uint32_t dwords;

	/* Refuses an empty run and one past the sixteen bindings. */
	if (count == 0U || count > GHCI_VERTEX_BINDINGS)
		return 0U;
	if (first >= GHCI_VERTEX_BINDINGS || first + count > GHCI_VERTEX_BINDINGS)
		return 0U;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 7.3, CMD_BIND_VERTEX_BUFFERS: 2 + 4N dwords).
	 */
	dwords = 2U + 4U * count;
	out[0] = ghci_cmd_header(GHCI_OP_BIND_VERTEX_BUFFERS, dwords, 0U);
	out[1] = first | (count << 16);

	/* Writes each binding's address, size and stride. */
	for (index = 0U; index < count; index++) {
		put64(&out[2U + 4U * index], bindings[index].va);
		out[4U + 4U * index] = bindings[index].bytes;
		out[5U + 4U * index] = bindings[index].stride;
	}

	/* Succeeded: two dwords and four per binding. */
	return dwords;
}

/*
 * Writes a BIND_INDEX_BUFFER of 16-bit or 32-bit indices.
 */
uint32_t
ghci_cmd_bind_index_buffer(
	uint32_t *out,
	uint64_t va,
	uint32_t index_32bit)
{
	/* Writes the address and the index type (0 16-bit, 1 32-bit). */
	out[0] = ghci_cmd_header(GHCI_OP_BIND_INDEX_BUFFER, GHCI_DW_BIND_INDEX_BUFFER, 0U);
	put64(&out[1], va);
	out[3] = 0U;
	if (index_32bit != 0U)
		out[3] = 1U;

	/* Succeeded: four dwords. */
	return GHCI_DW_BIND_INDEX_BUFFER;
}

/*
 * Writes a BIND_DESCRIPTOR_TABLE of one of the four sets.
 */
uint32_t
ghci_cmd_bind_descriptor_table(
	uint32_t *out,
	uint32_t set,
	uint64_t table_va,
	uint32_t count)
{
	/* Refuses a set past 3 and a count the field does not hold. */
	if (set >= DESCRIPTOR_SETS || count > 0xFFFFU)
		return 0U;

	/* Writes the table and its set and descriptor count. */
	out[0] = ghci_cmd_header(GHCI_OP_BIND_DESCRIPTOR_TABLE, GHCI_DW_BIND_DESCRIPTOR_TABLE, 0U);
	put64(&out[1], table_va);
	out[3] = set | (count << 16);

	/* Succeeded: four dwords. */
	return GHCI_DW_BIND_DESCRIPTOR_TABLE;
}

/*
 * Writes a SET_VIEWPORT from the bits of six floats.
 *
 * The floats are x, y, width, height, min depth and max depth, passed
 * through as the client's bit patterns.
 */
uint32_t
ghci_cmd_set_viewport(
	uint32_t *out,
	const uint32_t *float_bits)
{
	uint32_t index;

	/* Writes the header and the six floats. */
	out[0] = ghci_cmd_header(GHCI_OP_SET_VIEWPORT, GHCI_DW_SET_VIEWPORT, 0U);
	for (index = 0U; index < 6U; index++)
		out[1U + index] = float_bits[index];

	/* Succeeded: seven dwords. */
	return GHCI_DW_SET_VIEWPORT;
}

/*
 * Writes a SET_SCISSOR.
 */
uint32_t
ghci_cmd_set_scissor(
	uint32_t *out,
	int32_t x,
	int32_t y,
	uint32_t width,
	uint32_t height)
{
	/* Writes the corner as signed words and the extent. */
	out[0] = ghci_cmd_header(GHCI_OP_SET_SCISSOR, GHCI_DW_SET_SCISSOR, 0U);
	out[1] = (uint32_t)x;
	out[2] = (uint32_t)y;
	out[3] = width;
	out[4] = height;

	/* Succeeded: five dwords. */
	return GHCI_DW_SET_SCISSOR;
}

/*
 * Writes a SET_DYNAMIC_STATE of one state with its four payload words.
 */
uint32_t
ghci_cmd_set_dynamic_state(
	uint32_t *out,
	uint32_t state_id,
	const uint32_t *payload)
{
	uint32_t index;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 7.3, CMD_SET_DYNAMIC_STATE: 0x01..0x04, 0x07..0x0E, 0x10).
	 */
	if (state_id == 0U || state_id == 5U || state_id == 6U)
		return 0U;
	if (state_id == 0x0FU || state_id > GHCI_DYNAMIC_BORDER_COLOR_TABLE)
		return 0U;

	/* Writes the state and its payload; unused payload words are the caller's zeros. */
	out[0] = ghci_cmd_header(GHCI_OP_SET_DYNAMIC_STATE, GHCI_DW_SET_DYNAMIC_STATE, 0U);
	out[1] = state_id;
	for (index = 0U; index < 4U; index++)
		out[2U + index] = payload[index];

	/* Succeeded: six dwords. */
	return GHCI_DW_SET_DYNAMIC_STATE;
}

/*
 * Writes a DRAW.
 */
uint32_t
ghci_cmd_draw(
	uint32_t *out,
	uint32_t vertices,
	uint32_t instances,
	uint32_t first_vertex,
	uint32_t first_instance)
{
	/* Writes the counts and the first vertex and instance. */
	out[0] = ghci_cmd_header(GHCI_OP_DRAW, GHCI_DW_DRAW, 0U);
	out[1] = vertices;
	out[2] = instances;
	out[3] = first_vertex;
	out[4] = first_instance;

	/* Succeeded: five dwords. */
	return GHCI_DW_DRAW;
}

/*
 * Writes a DRAW_INDEXED.
 */
uint32_t
ghci_cmd_draw_indexed(
	uint32_t *out,
	uint32_t indices,
	uint32_t instances,
	uint32_t first_index,
	int32_t vertex_offset,
	uint32_t first_instance)
{
	/* Writes the counts, the first index, the signed vertex offset and the first instance. */
	out[0] = ghci_cmd_header(GHCI_OP_DRAW_INDEXED, GHCI_DW_DRAW_INDEXED, 0U);
	out[1] = indices;
	out[2] = instances;
	out[3] = first_index;
	out[4] = (uint32_t)vertex_offset;
	out[5] = first_instance;

	/* Succeeded: six dwords. */
	return GHCI_DW_DRAW_INDEXED;
}

/*
 * Writes a DRAW_INDIRECT with a fixed draw count.
 *
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 7.3, CMD_DRAW_INDIRECT, 7 dwords; INDEXED in the header's
 * flags[0], dword 6 reserved).
 */
uint32_t
ghci_cmd_draw_indirect(
	uint32_t *out,
	uint32_t indexed,
	uint64_t va,
	uint32_t offset,
	uint32_t draws,
	uint32_t stride)
{
	uint32_t flags;

	/* Marks an indexed draw in the header. */
	flags = 0U;
	if (indexed != 0U)
		flags = GHCI_DRAW_INDIRECT_INDEXED;

	/* Writes the buffer, offset, count and stride. */
	out[0] = ghci_cmd_header(GHCI_OP_DRAW_INDIRECT, GHCI_DW_DRAW_INDIRECT, flags);
	put64(&out[1], va);
	out[3] = offset;
	out[4] = draws;
	out[5] = stride;
	out[6] = 0U;

	/* Succeeded: seven dwords. */
	return GHCI_DW_DRAW_INDIRECT;
}

/*
 * Writes a DRAW_INDIRECT whose draw count is read from GPU memory.
 *
 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md
 * (section 7.3, CMD_DRAW_INDIRECT, 8 dwords with the header's flags[1]
 * set; INDEXED in flags[0]).
 */
uint32_t
ghci_cmd_draw_indirect_count(
	uint32_t *out,
	uint32_t indexed,
	uint64_t va,
	uint32_t offset,
	uint32_t max_draws,
	uint32_t stride,
	uint64_t count_va)
{
	uint32_t flags;

	/* Marks the form with a count buffer, and an indexed draw, in the header. */
	flags = GHCI_DRAW_INDIRECT_COUNT;
	if (indexed != 0U)
		flags |= GHCI_DRAW_INDIRECT_INDEXED;

	/* Writes the buffer, offset, maximum count, stride and the count's address. */
	out[0] = ghci_cmd_header(GHCI_OP_DRAW_INDIRECT, GHCI_DW_DRAW_INDIRECT_COUNT, flags);
	put64(&out[1], va);
	out[3] = offset;
	out[4] = max_draws;
	out[5] = stride;
	put64(&out[6], count_va);

	/* Succeeded: eight dwords. */
	return GHCI_DW_DRAW_INDIRECT_COUNT;
}

/*
 * Writes a DRAW_MESH_TASKS.
 */
uint32_t
ghci_cmd_draw_mesh_tasks(
	uint32_t *out,
	uint32_t x,
	uint32_t y,
	uint32_t z)
{
	/* Writes the workgroup counts. */
	out[0] = ghci_cmd_header(GHCI_OP_DRAW_MESH_TASKS, GHCI_DW_DRAW_MESH_TASKS, 0U);
	out[1] = x;
	out[2] = y;
	out[3] = z;

	/* Succeeded: four dwords. */
	return GHCI_DW_DRAW_MESH_TASKS;
}

/*
 * Writes a BEGIN_RENDER_PASS.
 *
 * The area is x, y, width and height; the clear values are bit patterns
 * and flags are the CLEAR_* bits.
 */
uint32_t
ghci_cmd_begin_render_pass(
	uint32_t *out,
	uint64_t desc_va,
	const uint32_t *area,
	uint32_t color_clear,
	uint32_t depth_clear,
	uint32_t stencil_clear,
	uint32_t flags)
{
	/* Refuses a description not on 64 bytes and an area the 16-bit fields do not hold. */
	if ((desc_va & 63U) != 0U)
		return 0U;
	if (area[0] > 0xFFFFU || area[1] > 0xFFFFU || area[2] > 0xFFFFU || area[3] > 0xFFFFU)
		return 0U;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 7.3, CMD_BEGIN_RENDER_PASS, 10 dwords).
	 */
	out[0] = ghci_cmd_header(GHCI_OP_BEGIN_RENDER_PASS, GHCI_DW_BEGIN_RENDER_PASS, 0U);
	put64(&out[1], desc_va);
	out[3] = pack_xy(area[0], area[1]);
	out[4] = pack_xy(area[2], area[3]);
	out[5] = color_clear;
	out[6] = depth_clear;
	out[7] = stencil_clear & 0xFFU;
	out[8] = flags;
	out[9] = 0U;

	/* Succeeded: ten dwords. */
	return GHCI_DW_BEGIN_RENDER_PASS;
}

/*
 * Writes an END_RENDER_PASS.
 */
uint32_t
ghci_cmd_end_render_pass(
	uint32_t *out)
{
	/* Writes the header alone. */
	out[0] = ghci_cmd_header(GHCI_OP_END_RENDER_PASS, GHCI_DW_END_RENDER_PASS, 0U);

	/* Succeeded: one dword. */
	return GHCI_DW_END_RENDER_PASS;
}

/*
 * Writes a LOAD_CONST of up to 32 words into the constant bank.
 */
uint32_t
ghci_cmd_load_const(
	uint32_t *out,
	uint32_t offset,
	uint32_t count,
	const uint32_t *data)
{
	uint32_t index;

	/* Refuses an empty load, one past 32 words and one past the bank's 512 words. */
	if (count == 0U || count > GHCI_CONST_LOAD_MAX)
		return 0U;
	if (offset >= GHCI_CONST_WORDS || offset + count > GHCI_CONST_WORDS)
		return 0U;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 7.3, CMD_LOAD_CONST: 3 + N dwords).
	 */
	out[0] = ghci_cmd_header(GHCI_OP_LOAD_CONST, 3U + count, 0U);
	out[1] = offset;
	out[2] = count;
	for (index = 0U; index < count; index++)
		out[3U + index] = data[index];

	/* Succeeded: three dwords and the data. */
	return 3U + count;
}

/*
 * Writes a DISPATCH_INDIRECT.
 */
uint32_t
ghci_cmd_dispatch_indirect(
	uint32_t *out,
	uint64_t va,
	uint32_t offset)
{
	/* Refuses an offset not on a dword. */
	if ((offset & 3U) != 0U)
		return 0U;

	/* Writes the buffer and offset; dwords 4 and 5 are reserved. */
	out[0] = ghci_cmd_header(GHCI_OP_DISPATCH_INDIRECT, GHCI_DW_DISPATCH_INDIRECT, 0U);
	put64(&out[1], va);
	out[3] = offset;
	out[4] = 0U;
	out[5] = 0U;

	/* Succeeded: six dwords. */
	return GHCI_DW_DISPATCH_INDIRECT;
}

/*
 * Writes a DISPATCH.
 */
uint32_t
ghci_cmd_dispatch(
	uint32_t *out,
	uint32_t x,
	uint32_t y,
	uint32_t z)
{
	/* Writes the workgroup counts. */
	out[0] = ghci_cmd_header(GHCI_OP_DISPATCH, GHCI_DW_DISPATCH, 0U);
	out[1] = x;
	out[2] = y;
	out[3] = z;

	/* Succeeded: four dwords. */
	return GHCI_DW_DISPATCH;
}

/*
 * Writes a BARRIER between pipeline stages with its cache operations.
 */
uint32_t
ghci_cmd_barrier(
	uint32_t *out,
	uint32_t src_stages,
	uint32_t dst_stages,
	uint32_t flags)
{
	/* Writes the stages to drain, the stages to hold and the cache operations. */
	out[0] = ghci_cmd_header(GHCI_OP_BARRIER, GHCI_DW_BARRIER, 0U);
	out[1] = src_stages;
	out[2] = dst_stages;
	out[3] = flags;

	/* Succeeded: four dwords. */
	return GHCI_DW_BARRIER;
}

/*
 * Writes a FENCE of a 64-bit sequence value to a bus address.
 */
uint32_t
ghci_cmd_fence(
	uint32_t *out,
	uint64_t bus,
	uint64_t seq,
	uint32_t flags)
{
	/* Refuses an address not on 8 bytes. */
	if ((bus & 7U) != 0U)
		return 0U;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 7.3, CMD_FENCE: address, sequence, flags; 6 dwords).
	 */
	out[0] = ghci_cmd_header(GHCI_OP_FENCE, GHCI_DW_FENCE, 0U);
	put64(&out[1], bus);
	put64(&out[3], seq);
	out[5] = flags;

	/* Succeeded: six dwords. */
	return GHCI_DW_FENCE;
}

/*
 * Writes a WAIT_FENCE until a 64-bit word reaches a value.
 */
uint32_t
ghci_cmd_wait_fence(
	uint32_t *out,
	uint64_t bus,
	uint64_t value)
{
	/* Refuses an address not on 8 bytes. */
	if ((bus & 7U) != 0U)
		return 0U;

	/* Writes the address and the value waited for. */
	out[0] = ghci_cmd_header(GHCI_OP_WAIT_FENCE, GHCI_DW_WAIT_FENCE, 0U);
	put64(&out[1], bus);
	put64(&out[3], value);

	/* Succeeded: five dwords. */
	return GHCI_DW_WAIT_FENCE;
}

/*
 * Writes a WRITE_TIMESTAMP to a session address or, privileged, to a bus address.
 */
uint32_t
ghci_cmd_write_timestamp(
	uint32_t *out,
	uint32_t host,
	uint64_t address,
	uint32_t stage)
{
	uint32_t flags;

	/* Refuses an address not on 8 bytes and a stage past bottom of pipe. */
	if ((address & 7U) != 0U || stage > 2U)
		return 0U;

	/* Marks a host bus address in the header. */
	flags = 0U;
	if (host != 0U)
		flags = GHCI_TIMESTAMP_HOST;

	/* Writes the address and the stage. */
	out[0] = ghci_cmd_header(GHCI_OP_WRITE_TIMESTAMP, GHCI_DW_WRITE_TIMESTAMP, flags);
	put64(&out[1], address);
	out[3] = stage;

	/* Succeeded: four dwords. */
	return GHCI_DW_WRITE_TIMESTAMP;
}

/*
 * Writes a BEGIN_QUERY or an END_QUERY of an occlusion query.
 */
uint32_t
ghci_cmd_query(
	uint32_t *out,
	uint32_t end,
	uint64_t pool_va,
	uint32_t index,
	uint32_t flags)
{
	uint32_t opcode;

	/* Refuses a pool not on 8 bytes and an index past 24 bits. */
	if ((pool_va & 7U) != 0U || index > GHCI_QUERY_INDEX_MASK)
		return 0U;

	/* Picks the beginning or the end. */
	opcode = GHCI_OP_BEGIN_QUERY;
	if (end != 0U)
		opcode = GHCI_OP_END_QUERY;

	/* Writes the pool and the index with the PRECISE and AVAILABILITY bits. */
	out[0] = ghci_cmd_header(opcode, GHCI_DW_QUERY, 0U);
	put64(&out[1], pool_va);
	out[3] = index | (flags & (GHCI_QUERY_PRECISE | GHCI_QUERY_AVAILABILITY));

	/* Succeeded: four dwords. */
	return GHCI_DW_QUERY;
}

/*
 * Writes a COPY_BUFFER, or a pattern fill with the FILL_PATTERN flag.
 */
uint32_t
ghci_cmd_copy_buffer(
	uint32_t *out,
	uint64_t src_va,
	uint64_t dst_va,
	uint32_t bytes,
	uint32_t flags,
	uint32_t fill)
{
	/* Refuses an empty copy and a size not in whole dwords. */
	if (bytes == 0U || (bytes & 3U) != 0U)
		return 0U;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 7.3, CMD_COPY_BUFFER, 8 dwords).
	 */
	out[0] = ghci_cmd_header(GHCI_OP_COPY_BUFFER, GHCI_DW_COPY_BUFFER, 0U);
	put64(&out[1], src_va);
	put64(&out[3], dst_va);
	out[5] = bytes;
	out[6] = flags;
	out[7] = fill;

	/* Succeeded: eight dwords. */
	return GHCI_DW_COPY_BUFFER;
}

/*
 * Writes a COPY_IMAGE between two slices of one format.
 */
uint32_t
ghci_cmd_copy_image(
	uint32_t *out,
	const struct ghci_transfer *transfer)
{
	uint32_t format;

	/* Refuses an empty extent and a format past the byte. */
	if (transfer->width == 0U || transfer->height == 0U || transfer->format > 0xFFU)
		return 0U;

	/* Packs the format with the two tiling bits. */
	format = transfer->format;
	if (transfer->src.tiled != 0U)
		format |= 1U << 8;
	if (transfer->dst.tiled != 0U)
		format |= 1U << 9;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 7.3, CMD_COPY_IMAGE, 11 dwords).
	 */
	out[0] = ghci_cmd_header(GHCI_OP_COPY_IMAGE, GHCI_DW_COPY_IMAGE, 0U);
	put64(&out[1], transfer->src.va);
	put64(&out[3], transfer->dst.va);
	out[5] = transfer->src.pitch;
	out[6] = transfer->dst.pitch;
	out[7] = pack_xy(transfer->src.x, transfer->src.y);
	out[8] = pack_xy(transfer->dst.x, transfer->dst.y);
	out[9] = pack_xy(transfer->width, transfer->height);
	out[10] = format;

	/* Succeeded: eleven dwords. */
	return GHCI_DW_COPY_IMAGE;
}

/*
 * Writes a BLIT with scaling and format conversion.
 */
uint32_t
ghci_cmd_blit(
	uint32_t *out,
	const struct ghci_transfer *transfer)
{
	uint32_t formats;

	/* Refuses formats past the byte. */
	if (transfer->format > 0xFFU || transfer->dst_format > 0xFFU)
		return 0U;

	/* Packs the two formats, the filter and the two tiling bits. */
	formats = transfer->format | (transfer->dst_format << 8);
	if (transfer->filter != 0U)
		formats |= 1U << 16;
	if (transfer->src.tiled != 0U)
		formats |= 1U << 17;
	if (transfer->dst.tiled != 0U)
		formats |= 1U << 18;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 7.3, CMD_BLIT, 12 dwords).
	 */
	out[0] = ghci_cmd_header(GHCI_OP_BLIT, GHCI_DW_BLIT, 0U);
	put64(&out[1], transfer->src.va);
	put64(&out[3], transfer->dst.va);
	out[5] = transfer->src.pitch;
	out[6] = transfer->dst.pitch;
	out[7] = pack_xy(transfer->src.x, transfer->src.y);
	out[8] = pack_xy(transfer->src_x1, transfer->src_y1);
	out[9] = pack_xy(transfer->dst.x, transfer->dst.y);
	out[10] = pack_xy(transfer->dst_x1, transfer->dst_y1);
	out[11] = formats;

	/* Succeeded: twelve dwords. */
	return GHCI_DW_BLIT;
}

/*
 * Writes a FILL_BUFFER of a 32-bit pattern.
 */
uint32_t
ghci_cmd_fill_buffer(
	uint32_t *out,
	uint64_t dst_va,
	uint32_t bytes,
	uint32_t pattern)
{
	/* Refuses an empty fill and a size not in whole dwords. */
	if (bytes == 0U || (bytes & 3U) != 0U)
		return 0U;

	/* Writes the destination, size and pattern. */
	out[0] = ghci_cmd_header(GHCI_OP_FILL_BUFFER, GHCI_DW_FILL_BUFFER, 0U);
	put64(&out[1], dst_va);
	out[3] = bytes;
	out[4] = pattern;

	/* Succeeded: five dwords. */
	return GHCI_DW_FILL_BUFFER;
}

/*
 * Writes a RESOLVE_IMAGE of a multisampled slice into a single-sample one.
 */
uint32_t
ghci_cmd_resolve_image(
	uint32_t *out,
	const struct ghci_transfer *transfer)
{
	uint32_t samples_code;
	uint32_t params;

	/* Encodes the sample count (0 one, 1 two, 2 four); others are refused. */
	if (transfer->samples == 2U) {
		samples_code = 1U;
	} else if (transfer->samples == 4U) {
		samples_code = 2U;
	} else {
		return 0U;
	}

	/* Refuses a format past the byte and a mode past sample zero. */
	if (transfer->format > 0xFFU || transfer->resolve_mode > 3U)
		return 0U;

	/* Packs the format, the resolve mode and the sample count. */
	params = transfer->format | (transfer->resolve_mode << 8) | (samples_code << 10);

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 7.3, CMD_RESOLVE_IMAGE, 11 dwords).
	 */
	out[0] = ghci_cmd_header(GHCI_OP_RESOLVE_IMAGE, GHCI_DW_RESOLVE_IMAGE, 0U);
	put64(&out[1], transfer->src.va);
	put64(&out[3], transfer->dst.va);
	out[5] = transfer->src.pitch;
	out[6] = transfer->dst.pitch;
	out[7] = pack_xy(transfer->src.x, transfer->src.y);
	out[8] = pack_xy(transfer->dst.x, transfer->dst.y);
	out[9] = pack_xy(transfer->width, transfer->height);
	out[10] = params;

	/* Succeeded: eleven dwords. */
	return GHCI_DW_RESOLVE_IMAGE;
}

/*
 * Writes a COPY_BUFFER_TO_IMAGE; the source is the buffer, the destination the slice.
 */
uint32_t
ghci_cmd_copy_buffer_to_image(
	uint32_t *out,
	const struct ghci_transfer *transfer)
{
	uint32_t dwords;

	/* Writes the buffer, then the image, with the image's corner and tiling. */
	dwords = buffer_image(out, GHCI_OP_COPY_BUFFER_TO_IMAGE, transfer->src.va, transfer->dst.va, transfer->src.pitch, transfer->dst.pitch, &transfer->dst, transfer);

	/* Reports the dwords written, 0 for refused arguments. */
	return dwords;
}

/*
 * Writes a COPY_IMAGE_TO_BUFFER; the source is the slice, the destination the buffer.
 */
uint32_t
ghci_cmd_copy_image_to_buffer(
	uint32_t *out,
	const struct ghci_transfer *transfer)
{
	uint32_t dwords;

	/* Writes the image, then the buffer, with the image's corner and tiling. */
	dwords = buffer_image(out, GHCI_OP_COPY_IMAGE_TO_BUFFER, transfer->src.va, transfer->dst.va, transfer->src.pitch, transfer->dst.pitch, &transfer->src, transfer);

	/* Reports the dwords written, 0 for refused arguments. */
	return dwords;
}

/*
 * Writes a DISP_SET of display register writes in ring order.
 *
 * The size is 1 + 2N dwords: the header and N offset and value pairs,
 * as the packet's dwords are laid out; the specification's 2 + 2N in
 * the same section counts a word the layout does not have.
 */
uint32_t
ghci_cmd_disp_set(
	uint32_t *out,
	uint32_t wait_idle,
	uint32_t pairs,
	const uint32_t *offsets,
	const uint32_t *values)
{
	uint32_t flags;
	uint32_t index;

	/* Refuses no pairs and more than a packet holds. */
	if (pairs == 0U || pairs > GHCI_DISP_SET_PAIRS_MAX)
		return 0U;

	/* Asks the controller to be idle first when the caller wants it. */
	flags = 0U;
	if (wait_idle != 0U)
		flags = GHCI_DISP_SET_WAIT_IDLE;

	/* Writes the header and each offset and value. */
	out[0] = ghci_cmd_header(GHCI_OP_DISP_SET, 1U + 2U * pairs, flags);
	for (index = 0U; index < pairs; index++) {
		out[1U + 2U * index] = offsets[index];
		out[2U + 2U * index] = values[index];
	}

	/* Succeeded: one dword and two per pair. */
	return 1U + 2U * pairs;
}

/*
 * Writes a FLIP of a scanout plane to a 32-bit physical framebuffer.
 */
uint32_t
ghci_cmd_flip(
	uint32_t *out,
	uint32_t display,
	uint32_t base_phys,
	uint32_t flags,
	uint32_t value,
	uint32_t record_phys)
{
	/* Refuses a display other than the primary one and a record not on 8 bytes. */
	if (display != 0U || (record_phys & 7U) != 0U)
		return 0U;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 7.3, CMD_FLIP, 8 dwords; the high words must be 0).
	 */
	out[0] = ghci_cmd_header(GHCI_OP_FLIP, GHCI_DW_FLIP, 0U);
	out[1] = display;
	out[2] = base_phys;
	out[3] = 0U;
	out[4] = flags;
	out[5] = value;
	out[6] = record_phys;
	out[7] = 0U;

	/* Succeeded: eight dwords. */
	return GHCI_DW_FLIP;
}

/*
 * Packs a pipeline state into the 128 bytes the GPU reads.
 */
int
ghci_pso_encode(
	uint8_t *out,
	const struct ghci_pso *pso)
{
	int error;

	/* Refuses a field its place in the record does not hold; a patch list needs the extended stages. */
	error = pso_check(pso, 0);
	if (error != 0)
		return error;

	/* Clears the record so that unused attributes read as format 0. */
	clear_bytes(out, GHCI_PSO_BYTES);

	/* Writes the record. */
	pso_store(out, pso, 0U);

	/* Succeeded: the record holds the pipeline state. */
	return 0;
}

/*
 * Packs a pipeline state with its extended stages into the 256 bytes the
 * GPU reads: the 128 bytes of the pipeline state with flags[16] set, and
 * the tessellation and geometry shader stages behind them.  The record
 * has tessellation, a geometry shader or both, and draws patches exactly
 * when it has tessellation.  Whether the device has the stages is the
 * caller's to know (a GPU without them faults on the record).
 */
int
ghci_pso_ext_encode(
	uint8_t *out,
	const struct ghci_pso *pso,
	const struct ghci_pso_ext *ext)
{
	int tessellation;
	int error;

	/* Refuses extended stages whose fields their places do not hold. */
	error = pso_ext_check(ext, &tessellation);
	if (error != 0)
		return error;

	/* Refuses a pipeline state that draws patches without tessellation, or other primitives with it. */
	error = pso_check(pso, tessellation);
	if (error != 0)
		return error;
	if (tessellation && pso->topology != GHCI_TOPOLOGY_PATCH_LIST)
		return EINVAL;

	/* Clears the record so that the reserved words and unused attributes read as 0. */
	clear_bytes(out, GHCI_PSO_EXT_BYTES);

	/* Writes the pipeline state, marked as extended, and the extended stages. */
	pso_store(out, pso, GHCI_PSO_FLAG_EXT_STAGES);
	pso_ext_store(out, ext);

	/* Succeeded: the record holds the pipeline state and its extended stages. */
	return 0;
}

/*
 * Packs a texture descriptor into its 32 bytes.
 */
int
ghci_texture_desc_encode(
	uint8_t *out,
	const struct ghci_texture_desc *desc)
{
	/* Refuses extents, mip counts and sample counts the GPU does not take. */
	if (desc->width == 0U || desc->width > EXTENT_MAX)
		return EINVAL;
	if (desc->height == 0U || desc->height > EXTENT_MAX)
		return EINVAL;
	if (desc->depth == 0U || desc->depth > 0xFFFFU)
		return EINVAL;
	if (desc->mip_levels == 0U || desc->mip_levels > MIP_LEVELS_MAX)
		return EINVAL;
	if (sample_count_valid(desc->samples) == 0)
		return EINVAL;

	/* Refuses a format past the byte, a dimension past the cube and a swizzle past 12 bits. */
	if (desc->format > 0xFFU || desc->dimension > 3U || desc->swizzle > 0xFFFU)
		return EINVAL;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 8.3, GhciTextureDescriptor).
	 */
	store64(out, GHCI_TEX_BASE, desc->base);
	store16(out, GHCI_TEX_WIDTH, desc->width);
	store16(out, GHCI_TEX_HEIGHT, desc->height);
	store16(out, GHCI_TEX_DEPTH, desc->depth);
	store8(out, GHCI_TEX_FORMAT, desc->format);
	store8(out, GHCI_TEX_MIP_LEVELS, desc->mip_levels);
	store32(out, GHCI_TEX_ROW_PITCH, desc->row_pitch);
	store32(out, GHCI_TEX_SLICE_PITCH, desc->slice_pitch);
	store8(out, GHCI_TEX_SAMPLES, desc->samples);
	store8(out, GHCI_TEX_DIMENSION, desc->dimension);
	store16(out, GHCI_TEX_FLAGS, desc->flags & (GHCI_TEX_FLAG_TILED | GHCI_TEX_FLAG_SRGB));
	store32(out, GHCI_TEX_SWIZZLE, desc->swizzle);

	/* Succeeded: the descriptor is written. */
	return 0;
}

/*
 * Packs a sampler descriptor into its 16 bytes.
 */
int
ghci_sampler_desc_encode(
	uint8_t *out,
	const struct ghci_sampler_desc *desc)
{
	/* Refuses filters, address modes and operators the GPU does not have. */
	if (desc->min_filter > 1U || desc->mag_filter > 1U || desc->mipmap_mode > 1U)
		return EINVAL;
	if (desc->address[0] > 4U || desc->address[1] > 4U || desc->address[2] > 4U)
		return EINVAL;
	if (desc->compare_op > 7U || desc->border_type > 3U || desc->border_index > 15U)
		return EINVAL;

	/* Refuses a bias past the signed 8.8 field and an anisotropy past 16. */
	if (desc->lod_bias_fx8 < -32768 || desc->lod_bias_fx8 > 32767)
		return EINVAL;
	if (desc->max_anisotropy > 16U || desc->min_lod_fx4 > 0xFFU || desc->max_lod_fx4 > 0xFFU)
		return EINVAL;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 8.3, GhciSamplerDescriptor).
	 */
	store8(out, 0x00U, desc->min_filter);
	store8(out, 0x01U, desc->mag_filter);
	store8(out, 0x02U, desc->mipmap_mode);
	store8(out, 0x03U, desc->address[0]);
	store8(out, 0x04U, desc->address[1]);
	store8(out, 0x05U, desc->address[2]);
	store8(out, 0x06U, desc->compare_enable != 0U ? 1U : 0U);
	store8(out, 0x07U, desc->compare_op);
	store16(out, GHCI_SAMPLER_LOD_BIAS, (uint32_t)desc->lod_bias_fx8 & 0xFFFFU);
	store8(out, 0x0AU, desc->min_lod_fx4);
	store8(out, 0x0BU, desc->max_lod_fx4);
	store8(out, 0x0CU, desc->max_anisotropy);
	store8(out, 0x0DU, desc->border_type);
	store8(out, 0x0EU, desc->border_index);
	store8(out, GHCI_SAMPLER_UNNORMALIZED, desc->unnormalized != 0U ? 1U : 0U);

	/* Succeeded: the descriptor is written. */
	return 0;
}

/*
 * Packs a buffer descriptor into its 16 bytes.
 */
int
ghci_buffer_desc_encode(
	uint8_t *out,
	const struct ghci_buffer_desc *desc)
{
	/* Refuses a stride, format or flags its field does not hold. */
	if (desc->stride > 0xFFFFU || desc->format > 0xFFU || desc->flags > 3U)
		return EINVAL;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 8.3, GhciBufferDescriptor).
	 */
	store64(out, GHCI_BUFFER_BASE, desc->base);
	store32(out, GHCI_BUFFER_SIZE, desc->bytes);
	store16(out, GHCI_BUFFER_STRIDE, desc->stride);
	store8(out, GHCI_BUFFER_FORMAT, desc->format);
	store8(out, GHCI_BUFFER_FLAGS, desc->flags);

	/* Succeeded: the descriptor is written. */
	return 0;
}

/*
 * Packs a render pass description into its 192 bytes.
 *
 * Every attachment is a linear subresource whose address the caller has
 * already moved to the mip level and layer drawn, so mip_level and
 * base_array_layer are written 0 and layers is 1.
 */
int
ghci_render_pass_desc_encode(
	uint8_t *out,
	const struct ghci_render_pass_desc *desc)
{
	uint32_t index;

	/* Refuses an extent, a layer count or a sample count the GPU does not take. */
	if (desc->width == 0U || desc->width > EXTENT_MAX)
		return EINVAL;
	if (desc->height == 0U || desc->height > EXTENT_MAX)
		return EINVAL;
	if (desc->layers != 1U || sample_count_valid(desc->samples) == 0)
		return EINVAL;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 8.4, GhciRenderPassDesc and its attachment rules).
	 */
	clear_bytes(out, GHCI_RENDER_PASS_BYTES);
	for (index = 0U; index < GHCI_COLOR_ATTACHMENTS; index++)
		store_attachment(out, index * GHCI_RP_ATTACHMENT_BYTES, &desc->colors[index]);

	/* Writes the depth and stencil attachment and the resolve target. */
	store_attachment(out, GHCI_RP_DEPTH_STENCIL, &desc->depth_stencil);
	store_attachment(out, GHCI_RP_RESOLVE, &desc->resolve);

	/* Writes the framebuffer extent, layers, samples and the clear table. */
	store16(out, GHCI_RP_WIDTH, desc->width);
	store16(out, GHCI_RP_HEIGHT, desc->height);
	store16(out, GHCI_RP_LAYERS, desc->layers);
	store8(out, GHCI_RP_SAMPLES, desc->samples);
	store64(out, GHCI_RP_CLEAR_TABLE, desc->clear_table);

	/* Succeeded: the description is written. */
	return 0;
}

/*
 * Reports whether a 64-bit fence value in memory has reached a value.
 *
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 7.3, CMD_WAIT_FENCE, the signed 64-bit comparison).
 */
int
ghci_fence64_passed(
	uint64_t memory,
	uint64_t value)
{
	int64_t distance;

	/* The values are a circle: memory has reached value when it is less than half a turn ahead. */
	distance = (int64_t)(memory - value);
	if (distance >= 0)
		return 1;

	/* Succeeded: the fence has not been reached yet. */
	return 0;
}

/* Writes a 64-bit value as its low and then its high dword. */
static void
put64(
	uint32_t *out,
	uint64_t value)
{
	/* The low word first, as every 64-bit field of a packet is laid out. */
	out[0] = (uint32_t)value;
	out[1] = (uint32_t)(value >> 32);
}

/* Packs two 16-bit coordinates, x in the low half. */
static uint32_t
pack_xy(
	uint32_t x,
	uint32_t y)
{
	/* Keeps 16 bits of each. */
	return (x & 0xFFFFU) | ((y & 0xFFFFU) << 16);
}

/* Stores one byte of a record. */
static void
store8(
	uint8_t *out,
	uint32_t offset,
	uint32_t value)
{
	/* Keeps the low byte. */
	out[offset] = (uint8_t)value;
}

/* Stores a 16-bit field of a record in little-endian order. */
static void
store16(
	uint8_t *out,
	uint32_t offset,
	uint32_t value)
{
	/* The low byte first. */
	out[offset] = (uint8_t)value;
	out[offset + 1U] = (uint8_t)(value >> 8);
}

/* Stores a 32-bit field of a record in little-endian order. */
static void
store32(
	uint8_t *out,
	uint32_t offset,
	uint32_t value)
{
	/* The two halves, the low half first. */
	store16(out, offset, value & 0xFFFFU);
	store16(out, offset + 2U, value >> 16);
}

/* Stores a 64-bit field of a record in little-endian order. */
static void
store64(
	uint8_t *out,
	uint32_t offset,
	uint64_t value)
{
	/* The two words, the low word first. */
	store32(out, offset, (uint32_t)value);
	store32(out, offset + 4U, (uint32_t)(value >> 32));
}

/* Zeroes the bytes of a record before its fields are stored. */
static void
clear_bytes(
	uint8_t *out,
	uint32_t bytes)
{
	uint32_t index;

	/* Zeroes each byte; reserved fields stay 0. */
	for (index = 0U; index < bytes; index++)
		out[index] = 0U;
}

/* Reports whether a sample count is 1, 2 or 4. */
static int
sample_count_valid(
	uint32_t samples)
{
	/* Accepts the three counts the GPU rasterizes with. */
	if (samples == 1U || samples == 2U || samples == 4U)
		return 1;

	/* Succeeded: any other count is not valid. */
	return 0;
}

/* Packs dw8 of the pipeline state: topology and the per-sample flags. */
static uint32_t
pso_flags_word(
	const struct ghci_pso *pso)
{
	uint32_t word;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 8.2, dw8 flags).
	 */
	word = pso->topology;
	if (pso->primitive_restart != 0U)
		word |= 1U << 3;
	if (pso->depth_clamp != 0U)
		word |= 1U << 4;
	if (pso->logic_op_enable != 0U)
		word |= 1U << 5;
	word |= pso->logic_op << 6;
	if (pso->alpha_to_coverage != 0U)
		word |= 1U << 10;
	if (pso->sample_shading != 0U)
		word |= 1U << 11;
	word |= pso->sample_mask << 12;

	/* Succeeded: the flags word. */
	return word;
}

/* Packs dw9 of the pipeline state: culling, polygon mode, depth bias and line width. */
static uint32_t
pso_raster_word(
	const struct ghci_pso *pso)
{
	uint32_t word;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 8.2, dw9 raster_state; line width 16 = 1.0).
	 */
	word = pso->cull_mode;
	if (pso->front_cw != 0U)
		word |= 1U << 2;
	word |= pso->polygon_mode << 3;
	if (pso->depth_bias_enable != 0U)
		word |= 1U << 5;
	word |= pso->line_width_fx4 << 6;

	/* Succeeded: the raster word. */
	return word;
}

/* Packs dw10 of the pipeline state: the depth test and both faces' stencil. */
static uint32_t
pso_depth_stencil_word(
	const struct ghci_pso *pso)
{
	uint32_t word;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 8.2, dw10 depth_stencil_state; each face's fail, pass,
	 * depth fail and compare in three bits).
	 */
	word = 0U;
	if (pso->depth_test != 0U)
		word |= 1U << 0;
	if (pso->depth_write != 0U)
		word |= 1U << 1;
	word |= pso->depth_op << 2;
	if (pso->stencil_test != 0U)
		word |= 1U << 5;
	word |= pso->front_stencil[0] << 6;
	word |= pso->front_stencil[1] << 9;
	word |= pso->front_stencil[2] << 12;
	word |= pso->front_stencil[3] << 15;
	word |= pso->back_stencil[0] << 18;
	word |= pso->back_stencil[1] << 21;
	word |= pso->back_stencil[2] << 24;
	word |= pso->back_stencil[3] << 27;
	if (pso->separate_back != 0U)
		word |= 1U << 30;

	/* Succeeded: the depth and stencil word. */
	return word;
}

/* Packs dw11 of the pipeline state: the blend factors, operators and write mask. */
static uint32_t
pso_blend_word(
	const struct ghci_pso *pso)
{
	uint32_t word;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 8.2, dw11 blend_state).
	 */
	word = 0U;
	if (pso->blend_enable != 0U)
		word |= 1U << 0;
	word |= pso->src_color << 1;
	word |= pso->dst_color << 6;
	word |= pso->color_op << 11;
	word |= pso->src_alpha << 14;
	word |= pso->dst_alpha << 19;
	word |= pso->alpha_op << 24;
	word |= pso->write_mask << 27;

	/* Succeeded: the blend word. */
	return word;
}

/*
 * Refuses a pipeline state field that its place in the record does not
 * hold.  patches is 1 when the record has tessellation, whose topology is
 * the patch list (and only then).
 */
static int
pso_check(
	const struct ghci_pso *pso,
	int patches)
{
	uint32_t index;

	/* The topology: a primitive, or the patch list of tessellation. */
	if (pso->topology > TOPOLOGY_MAX) {
		if (!patches)
			return EINVAL;
		if (pso->topology != GHCI_TOPOLOGY_PATCH_LIST)
			return EINVAL;
	}

	/* The packed state words: each field within its bits. */
	if (pso->logic_op > 15U || pso->sample_mask > 15U)
		return EINVAL;
	if (pso->cull_mode > 3U || pso->polygon_mode > 2U || pso->line_width_fx4 > 0xFFU)
		return EINVAL;
	if (pso->depth_op > 7U)
		return EINVAL;

	/* Each stencil operation and compare operator in three bits. */
	for (index = 0U; index < 4U; index++) {
		if (pso->front_stencil[index] > 7U || pso->back_stencil[index] > 7U)
			return EINVAL;
	}

	/* The blend factors in five bits, the operators to MAX, the write mask in four. */
	if (pso->src_color > 18U || pso->dst_color > 18U || pso->src_alpha > 18U || pso->dst_alpha > 18U)
		return EINVAL;
	if (pso->color_op > 4U || pso->alpha_op > 4U || pso->write_mask > 15U)
		return EINVAL;

	/* The default strides in a byte each. */
	for (index = 0U; index < 4U; index++) {
		if (pso->strides[index] > 0xFFU)
			return EINVAL;
	}

	/* The register counts, varyings, workgroup size and shared memory. */
	if (pso->regs_vs > REGISTERS_MAX || pso->regs_fs > REGISTERS_MAX || pso->regs_cs > REGISTERS_MAX)
		return EINVAL;
	if (pso->varyings > VARYINGS_MAX || pso->lds_bytes > LDS_BYTES_MAX)
		return EINVAL;
	if (pso->local_size[0] > LOCAL_SIZE_XY_MAX || pso->local_size[1] > LOCAL_SIZE_XY_MAX || pso->local_size[2] > LOCAL_SIZE_Z_MAX)
		return EINVAL;

	/* The attributes: sixteen at most, each with a format, binding and offset that fit. */
	if (pso->attribute_count > GHCI_VERTEX_ATTRIBUTES)
		return EINVAL;
	for (index = 0U; index < pso->attribute_count; index++) {
		if (pso->attributes[index].format > 0xFFU || pso->attributes[index].binding > 15U)
			return EINVAL;
		if (pso->attributes[index].offset > ATTRIBUTE_OFFSET_MAX)
			return EINVAL;
	}

	/* Succeeded: every field fits. */
	return 0;
}

/* Writes the 128 bytes of a pipeline state, with flags added to dw8's. */
static void
pso_store(
	uint8_t *out,
	const struct ghci_pso *pso,
	uint32_t flags)
{
	uint32_t index;
	uint32_t word;

	/* Writes the four stage entry points. */
	store64(out, GHCI_PSO_VS_CODE, pso->vs_code);
	store64(out, GHCI_PSO_FS_CODE, pso->fs_code);
	store64(out, GHCI_PSO_CS_CODE, pso->cs_code);
	store64(out, GHCI_PSO_MS_CODE, pso->ms_code);

	/* Writes the packed state words dw8 to dw11. */
	store32(out, GHCI_PSO_FLAGS, pso_flags_word(pso) | flags);
	store32(out, GHCI_PSO_RASTER, pso_raster_word(pso));
	store32(out, GHCI_PSO_DEPTH_STENCIL, pso_depth_stencil_word(pso));
	store32(out, GHCI_PSO_BLEND, pso_blend_word(pso));

	/* Writes the default strides of bindings 0 to 3, one byte each. */
	word = pso->strides[0] | (pso->strides[1] << 8) | (pso->strides[2] << 16) | (pso->strides[3] << 24);
	store32(out, GHCI_PSO_STRIDES, word);

	/* Writes the register counts of each stage and the varying count. */
	word = pso->regs_vs | (pso->regs_fs << 8) | (pso->regs_cs << 16) | (pso->varyings << 24);
	store32(out, GHCI_PSO_LIMITS, word);

	/* Writes the compute workgroup size. */
	word = pso->local_size[0] | (pso->local_size[1] << 10) | (pso->local_size[2] << 20);
	store32(out, GHCI_PSO_LOCAL_SIZE, word);

	/* Writes the shared memory size and the fragment flags. */
	word = pso->lds_bytes;
	if (pso->fs_centroid != 0U)
		word |= 1U << 16;
	if (pso->fs_depth_export != 0U)
		word |= 1U << 17;
	if (pso->fs_discard != 0U)
		word |= 1U << 18;
	store32(out, GHCI_PSO_LDS_FLAGS, word);

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 8.2, the attribute: [7:0] format, [11:8] binding,
	 * [12] input rate, [31:13] offset).
	 */
	for (index = 0U; index < pso->attribute_count; index++) {
		word = pso->attributes[index].format;
		word |= pso->attributes[index].binding << 8;
		if (pso->attributes[index].per_instance != 0U)
			word |= 1U << 12;
		word |= pso->attributes[index].offset << 13;
		store32(out, GHCI_PSO_ATTRIBUTES + 4U * index, word);
	}
}

/*
 * Refuses extended stages that are none, half of tessellation, or a
 * field its place in the record does not hold, and reports whether they
 * have tessellation.
 */
static int
pso_ext_check(
	const struct ghci_pso_ext *ext,
	int *tessellation)
{
	int error;

	/* Tessellation is both of its stages or neither. */
	*tessellation = 0;
	if (ext->tcs_code != 0U || ext->tes_code != 0U) {
		if (ext->tcs_code == 0U || ext->tes_code == 0U)
			return EINVAL;
		*tessellation = 1;
	}

	/* Extended stages are tessellation, a geometry shader or both. */
	if (!*tessellation && ext->gs_code == 0U)
		return EINVAL;

	/* The tessellation's fields, when it is there. */
	if (*tessellation) {
		error = pso_ext_tess_check(ext);
		if (error != 0)
			return error;
	}

	/* The geometry shader's fields, when it is there. */
	if (ext->gs_code != 0U) {
		error = pso_ext_gs_check(ext);
		if (error != 0)
			return error;
	}

	/* The spill ring: on 64 bytes, and no ring without its address. */
	if ((ext->spill_va % GHCI_PSO_ALIGN) != 0U)
		return EINVAL;
	if (ext->spill_va == 0U && (ext->spill_bytes != 0U || ext->spill_stride != 0U))
		return EINVAL;

	/* Succeeded: every field fits. */
	return 0;
}

/*
 * Refuses tessellation fields their places in tess_state and dw40 do not
 * hold, or a patch the shared memory of a patch does not hold.
 *
 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 8.2,
 * tess_state: [5:0] and [11:6] the control points 1..32, [13:12] the
 * domain 0..2, [15:14] the spacing 0..2, [24:18] cp_stride_words 0..127,
 * [31:25] patch_stride_128b 1..127, patch_stride_128b x 128 >= 0x220 +
 * output points x cp_stride_words x 4; dw40 the registers 1..63;
 * section 12.10.4, 0x220 + output points x cp_stride_words x 4 <= 16256).
 */
static int
pso_ext_tess_check(
	const struct ghci_pso_ext *ext)
{
	uint32_t patch_bytes;

	/* The control points of a patch in and out. */
	if (ext->input_points == 0U || ext->input_points > GHCI_TESS_POINTS_MAX)
		return EINVAL;
	if (ext->output_points == 0U || ext->output_points > GHCI_TESS_POINTS_MAX)
		return EINVAL;

	/* The domain, the spacing and the two flags. */
	if (ext->domain > GHCI_TESS_DOMAIN_ISOLINE || ext->spacing > GHCI_TESS_SPACING_FRACTIONAL_ODD)
		return EINVAL;
	if (ext->clockwise > 1U || ext->point_mode > 1U)
		return EINVAL;

	/* The patch: the output control points from 0x220 within the patch's bytes and the most a patch has. */
	if (ext->cp_stride_words > GHCI_TESS_CP_STRIDE_MAX)
		return EINVAL;
	if (ext->patch_stride_128b == 0U || ext->patch_stride_128b > GHCI_TESS_PATCH_STRIDE_MAX)
		return EINVAL;
	patch_bytes = GHCI_TESS_PATCH_CP_FIRST + ext->output_points * ext->cp_stride_words * 4U;
	if (patch_bytes > GHCI_TESS_PATCH_BYTES_MAX)
		return EINVAL;
	if (ext->patch_stride_128b * GHCI_TESS_PATCH_UNIT < patch_bytes)
		return EINVAL;

	/* The registers of both stages. */
	if (ext->regs_tcs == 0U || ext->regs_tcs > REGISTERS_MAX)
		return EINVAL;
	if (ext->regs_tes == 0U || ext->regs_tes > REGISTERS_MAX)
		return EINVAL;

	/* Succeeded: the tessellation's fields fit. */
	return 0;
}

/*
 * Refuses geometry shader fields their places in gs_state and dw40 do
 * not hold, or an output larger than an invocation may emit.
 *
 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 8.2,
 * gs_state: [3:0] the input topology 0..9 but 5, [6:4] the output 0..2,
 * [15:7] max_vertices 1..256, [21:16] invocations 1..32, [23:22] the
 * streams less 1, [30:24] output_stride_words 1..127; dw40 the registers
 * 1..63; section 12.10.4, max_output_vertices x output_stride_words at
 * most 1024).
 */
static int
pso_ext_gs_check(
	const struct ghci_pso_ext *ext)
{
	/* The input and output topologies. */
	if (ext->gs_input > GHCI_GS_INPUT_TRIANGLE_STRIP_ADJ || ext->gs_input == GHCI_GS_INPUT_RESERVED)
		return EINVAL;
	if (ext->gs_output > GHCI_GS_OUTPUT_TRIANGLE_STRIP)
		return EINVAL;

	/* The vertices, the invocations and the streams. */
	if (ext->gs_vertices == 0U || ext->gs_vertices > GHCI_GS_VERTICES_MAX)
		return EINVAL;
	if (ext->gs_invocations == 0U || ext->gs_invocations > GHCI_GS_INVOCATIONS_MAX)
		return EINVAL;
	if (ext->gs_streams == 0U || ext->gs_streams > GHCI_GS_STREAMS_MAX)
		return EINVAL;

	/* An output vertex's words, and the words of all the vertices one invocation emits. */
	if (ext->gs_stride_words == 0U || ext->gs_stride_words > GHCI_GS_STRIDE_WORDS_MAX)
		return EINVAL;
	if (ext->gs_vertices * ext->gs_stride_words > GHCI_GS_OUTPUT_WORDS_MAX)
		return EINVAL;

	/* The registers. */
	if (ext->regs_gs == 0U || ext->regs_gs > REGISTERS_MAX)
		return EINVAL;

	/* Succeeded: the geometry shader's fields fit. */
	return 0;
}

/*
 * Writes the extended stages behind the 128 bytes of a pipeline state;
 * an absent stage leaves its words 0.
 *
 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 8.2,
 * GhciPipelineStateExt: tess_state [16] the clockwise winding, [17]
 * point mode; gs_state as pso_ext_gs_check() has it; dw40 [7:0] TCS,
 * [15:8] TES, [23:16] GS registers).
 */
static void
pso_ext_store(
	uint8_t *out,
	const struct ghci_pso_ext *ext)
{
	uint32_t word;

	/* Writes the three stage entry points. */
	store64(out, GHCI_PSO_TCS_CODE, ext->tcs_code);
	store64(out, GHCI_PSO_TES_CODE, ext->tes_code);
	store64(out, GHCI_PSO_GS_CODE, ext->gs_code);

	/* Writes tess_state, for tessellation. */
	if (ext->tcs_code != 0U) {
		word = ext->input_points | (ext->output_points << 6) | (ext->domain << 12) | (ext->spacing << 14);
		if (ext->clockwise != 0U)
			word |= 1U << 16;
		if (ext->point_mode != 0U)
			word |= 1U << 17;
		word |= (ext->cp_stride_words << 18) | (ext->patch_stride_128b << 25);
		store32(out, GHCI_PSO_TESS_STATE, word);
	}

	/* Writes gs_state, for a geometry shader. */
	if (ext->gs_code != 0U) {
		word = ext->gs_input | (ext->gs_output << 4) | (ext->gs_vertices << 7) | (ext->gs_invocations << 16);
		word |= ((ext->gs_streams - 1U) << 22) | (ext->gs_stride_words << 24);
		store32(out, GHCI_PSO_GS_STATE, word);
	}

	/* Writes the registers of the stages there are. */
	word = 0U;
	if (ext->tcs_code != 0U)
		word |= ext->regs_tcs | (ext->regs_tes << 8);
	if (ext->gs_code != 0U)
		word |= ext->regs_gs << 16;
	store32(out, GHCI_PSO_LIMITS_EXT, word);

	/* Writes the geometry shader's spill ring. */
	store64(out, GHCI_PSO_SPILL_VA, ext->spill_va);
	store32(out, GHCI_PSO_SPILL_BYTES, ext->spill_bytes);
	store32(out, GHCI_PSO_SPILL_STRIDE, ext->spill_stride);
}

/* Stores one 16-byte attachment of a render pass description. */
static void
store_attachment(
	uint8_t *out,
	uint32_t offset,
	const struct ghci_rp_attachment *attachment)
{
	/*
	 * The address, row pitch, format and operations; mip_level and
	 * base_array_layer stay 0 (the address already names the subresource).
	 */
	store64(out, offset, attachment->va);
	store32(out, offset + 8U, attachment->row_pitch);
	store8(out, offset + 12U, attachment->format);
	store8(out, offset + 13U, attachment->ops);
}

/* Writes a copy between a buffer and an image slice in either direction. */
static uint32_t
buffer_image(
	uint32_t *out,
	uint32_t opcode,
	uint64_t first_va,
	uint64_t second_va,
	uint32_t first_pitch,
	uint32_t second_pitch,
	const struct ghci_slice *image,
	const struct ghci_transfer *transfer)
{
	uint32_t format;

	/* Refuses an empty extent and a format past the byte. */
	if (transfer->width == 0U || transfer->height == 0U || transfer->format > 0xFFU)
		return 0U;

	/* Packs the format with the image's tiling bit. */
	format = transfer->format;
	if (image->tiled != 0U)
		format |= 1U << 8;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 7.3, CMD_COPY_BUFFER_TO_IMAGE and CMD_COPY_IMAGE_TO_BUFFER,
	 * 10 dwords: source, destination, the two pitches in the same order,
	 * the image's corner, the extent, the format and tiling).
	 */
	out[0] = ghci_cmd_header(opcode, GHCI_DW_BUFFER_IMAGE, 0U);
	put64(&out[1], first_va);
	put64(&out[3], second_va);
	out[5] = first_pitch;
	out[6] = second_pitch;
	out[7] = pack_xy(image->x, image->y);
	out[8] = pack_xy(transfer->width, transfer->height);
	out[9] = format;

	/* Succeeded: ten dwords. */
	return GHCI_DW_BUFFER_IMAGE;
}
