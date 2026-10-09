/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The geometry part of the code generator (ws075-p007a), included by
 * compile.c.
 *
 * Register conventions of a SIMD8 geometry thread on Gen12.0, one channel
 * to an input primitive (Mesa 25.0.7 brw_fs_thread_payload.cpp,
 * gs_thread_payload; brw_compile_gs.cpp):
 *
 *   r0              the thread header (the scratch space in dwords 3 and 5)
 *   r1              the output URB handles, a dword to a channel; bits 31:27
 *                   carry the instance, so a write takes bits 15:0 only
 *   r2              the input primitive's number, when the kernel reads it
 *                   (3DSTATE_GS Include Primitive ID)
 *   then vertices_in the URB handles of the input vertices, one register to
 *                   a vertex (3DSTATE_GS Include Vertex Handles)
 *   then            the push data, from the dispatch's first register after
 *                   the fixed ones (dispatch_grf_start), as a vertex
 *                   shader's; no input is pushed (Vertex URB Entry Read
 *                   Length 0), every input is pulled
 *   values          the vertex count and the cut bits in the first two value
 *                   registers, then the IR's values
 *   staged VUE      as a vertex shader's, r(127 - 4 slots) .. r126:
 *                   [header: 0, layer, 0, 0][position][varyings ascending];
 *                   right below it the header of each vertex write -- the
 *                   output handles (r1 bits 15:0, copied once) and the
 *                   per-slot offsets of the vertex
 *   r126, r127      the last write: the vertex count and the handles
 *
 * An input is read from the producer's VUE with one SIMD8 URB read of the
 * slot it is in (pull), from the handle of the vertex: a constant vertex's
 * own register, or a handle chosen channel by channel by comparing the
 * vertex's number with each vertex in turn.  The reply is a register to a
 * component, up to the one read.
 *
 * The output URB entry (brw_compile_gs.cpp, with the vertex count dynamic):
 * the vertex count in its first 32 bytes, then the control data header (the
 * cut bits), then the vertices, each a whole number of 32-byte units.
 *
 * An emit writes the staged VUE two slots at a time with per-slot offsets
 * -- the vertex's place, the vertex count times the vertex's size -- for the
 * channels of its block that have room for one more vertex (a shader that
 * emits past its OutputVertices loses the extra vertices, as Mesa's
 * nir_lower_gs_intrinsics guard does, instead of writing past its entry);
 * then the count and the offset move on.  The payload is split after the
 * header (the handles and the offsets) as Mesa's brw_opt_split_sends()
 * splits it.  An end sets the cut bit of the last vertex emitted.  The cut
 * bits are gathered a dword at a time: written once at the thread's end
 * when they are 32 or fewer, otherwise each time 32 more vertices start
 * (with a channel mask, and per-slot offsets past 128 bits) and once more
 * at the end (Mesa's emit_gs_control_data_bits()).  The thread ends with
 * the write of the vertex count.
 *
 * The file is part of compile.c's translation unit: it uses that file's
 * state, constants and helpers, and its functions are declared with the
 * others at the top of compile.c.
 */

/* Geometry: the output URB handles, and the input primitive's number when the kernel reads it. */
#define COMPILE_GS_OUTPUT_HANDLES_GRF	1U
#define COMPILE_GS_PRIMITIVE_ID_GRF	2U

/* Geometry: the bits of an output handle; bits 31:27 are the instance (Mesa: the AND with 0xFFFF on Gen12). */
#define COMPILE_GS_HANDLE_MASK		0xFFFFU

/* Geometry: the most vertices an input primitive has, a triangle with adjacency. */
#define COMPILE_GS_MAX_VERTICES_IN	6U

/*
 * Geometry: the 32-byte unit the vertex count, the control data header and
 * each output vertex take in the output URB entry, the 64-byte unit the
 * entry's size is given in, and the largest entry (Mesa 25.0.7
 * brw_eu_defines.h, GFX7_MAX_GS_URB_ENTRY_SIZE_BYTES).
 */
#define COMPILE_GS_HWORD_BYTES		32U
#define COMPILE_GS_URB_UNIT_BYTES	64U
#define COMPILE_GS_MAX_URB_ENTRY_BYTES	(512U * 64U)

/* Geometry: one VUE slot is 16 bytes; the control data header holds 256 bits to a 32-byte unit. */
#define COMPILE_GS_SLOT_BYTES		16U
#define COMPILE_GS_HWORD_BITS		256U

/*
 * Geometry: the cut bits one register gathers before they are written (a
 * dword to a channel), and the header size past which a write needs a
 * channel mask (one dword of four in an OWord) and per-slot offsets (one
 * OWord of several).
 */
#define COMPILE_GS_CUT_DWORD_BITS	32U
#define COMPILE_GS_CUT_DWORD_SHIFT	5U
#define COMPILE_GS_CUT_OWORD_BITS	128U

/* Geometry: the OWord, in OWords, of the control data header, after the vertex count's 32 bytes. */
#define COMPILE_GS_CONTROL_OWORD	2U

/* Geometry: one OWord is four dwords, the dword a channel mask picks; its bits are 23:16 of the mask register. */
#define COMPILE_GS_OWORD_DWORDS_SHIFT	2U
#define COMPILE_GS_OWORD_DWORD_MASK	3U
#define COMPILE_GS_CHANNEL_MASK_SHIFT	16U

/* The registers of data a write with a channel mask carries: the dword four times, one for each it may land on. */
#define COMPILE_GS_MASKED_DATA_REGS	4U

/*
 * Geometry: the control data formats of 3DSTATE_GS -- cut bits (a strip
 * ends after the vertex whose bit is set) and stream IDs, which a shader
 * emitting points has (Mesa 25.0.7 brw_eu_defines.h,
 * GFX7_GS_CONTROL_DATA_FORMAT_GSCTL_CUT and _SID).
 */
#define COMPILE_GS_CONTROL_CUT		0U
#define COMPILE_GS_CONTROL_SID		1U

/*
 * The URB's SIMD8 write message (Mesa 25.0.7 brw_eu_defines.h,
 * GFX8_URB_OPCODE_SIMD8_WRITE; brw_eu.h brw_urb_desc()): bit 17 says
 * per-slot offsets follow the handles, bit 15 a channel mask after them.
 */
#define COMPILE_URB_OPCODE_SIMD8_WRITE	7U
#define COMPILE_URB_PER_SLOT_PRESENT	(1U << 17)
#define COMPILE_URB_CHANNEL_MASK_PRESENT	(1U << 15)
#define COMPILE_URB_GLOBAL_OFFSET_SHIFT	4U

/*
 * The URB's SIMD8 read message (Mesa 25.0.7 brw_eu_defines.h,
 * GFX8_URB_OPCODE_SIMD8_READ; brw_eu.h brw_urb_desc()): the handle as the
 * one-register header, `length` registers of reply -- component c of the
 * slot in register c -- and the slot in the global offset (OWords).
 */
#define COMPILE_URB_OPCODE_SIMD8_READ	8U
#define COMPILE_DESC_URB_READ(slot, length)	((1U << EU_DESC_MLEN_SHIFT) | \
						 ((uint32_t)(length) << EU_DESC_RLEN_SHIFT) | \
						 EU_DESC_HEADER_PRESENT | \
						 ((uint32_t)(slot) << 4) | \
						 COMPILE_URB_OPCODE_SIMD8_READ)

/*
 * Lays out a geometry thread: where its handles and its push data are, the
 * per-vertex inputs it reads, and the output URB entry its vertices make.
 * An entry larger than the URB takes is refused.
 */
static void
i915_compile_geometry_interface(
	struct i915_compile_state *state)
{
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t index;
	uint32_t control_bits;
	uint32_t entry_bytes;

	/* An input primitive of no vertex, or of more than a triangle with adjacency has, is inconsistent IR. */
	if (state->ir->vertices_in == 0U || state->ir->vertices_in > COMPILE_GS_MAX_VERTICES_IN) {
		state->error = 1;
		return;
	}

	/* The output is a VUE: no colour, no discard, no interpolated input; nor a point size (refused by the parser). */
	if (state->input_count != 0U ||
	    state->fs_outputs != 0U ||
	    state->writes_point_size != 0)
		state->unsupported = 1;

	/* Lists the located per-vertex inputs read, for the pipeline to check against the vertex shader's varyings. */
	for (index = 0U; index < state->ir->instruction_count; index++) {
		inst = &state->ir->instructions[index];
		if (inst->op != DRV_GPU_IR_LOAD_VERTEX_INPUT)
			continue;
		if (inst->location == DRV_GPU_IR_LOCATION_POSITION)
			continue;
		if (inst->location == DRV_GPU_IR_LOCATION_POINT_SIZE)
			continue;
		i915_compile_note(state, state->inputs, &state->input_count, COMPILE_MAX_INPUTS, inst->location);
	}

	/* The input vertices' handles follow r1 and the primitive's number; the push data follows them. */
	state->gs_handle_grf = COMPILE_GS_PRIMITIVE_ID_GRF;
	if (state->ir->uses_primitive_id != 0U)
		state->gs_handle_grf = COMPILE_GS_PRIMITIVE_ID_GRF + 1U;
	state->gs_push_grf = state->gs_handle_grf + state->ir->vertices_in;

	/* One output vertex: the header, the position and the varyings, a whole number of 32-byte units. */
	state->gs_vertex_hwords = ((2U + state->varying_count) * COMPILE_GS_SLOT_BYTES + COMPILE_GS_HWORD_BYTES - 1U) /
				  COMPILE_GS_HWORD_BYTES;

	/*
	 * The control data: a cut bit to each vertex when the shader ends
	 * strips, nothing for points (stream 0 only) or for a shader that never
	 * ends one.
	 */
	control_bits = 0U;
	if (state->ir->output_topology != DRV_GPU_IR_OUTPUT_POINTS && state->ir->uses_end_primitive != 0U)
		control_bits = state->ir->max_vertices;
	state->gs_control_bits = control_bits;
	state->gs_control_hwords = (control_bits + COMPILE_GS_HWORD_BITS - 1U) / COMPILE_GS_HWORD_BITS;

	/* The entry: the vertex count, the control data header, then every vertex the shader may emit. */
	entry_bytes = COMPILE_GS_HWORD_BYTES;
	entry_bytes += state->gs_control_hwords * COMPILE_GS_HWORD_BYTES;
	entry_bytes += state->ir->max_vertices * state->gs_vertex_hwords * COMPILE_GS_HWORD_BYTES;

	/* An entry larger than the URB takes is refused, as Mesa refuses it. */
	if (entry_bytes > COMPILE_GS_MAX_URB_ENTRY_BYTES) {
		state->unsupported = 1;
		return;
	}

	/* The entry's size in 64-byte units, as 3DSTATE_URB_ALLOC_GS takes it. */
	state->gs_urb_entry_size = (entry_bytes + COMPILE_GS_URB_UNIT_BYTES - 1U) / COMPILE_GS_URB_UNIT_BYTES;
}

/*
 * Prepares a geometry thread's vertex writes: the output handles without
 * the instance bits, copied once below the staged VUE, and the vertex
 * count and the cut bits, both zero.
 */
static void
i915_compile_geometry_prologue(
	struct i915_compile_state *state)
{
	struct i915_eu_reg counter;
	uint32_t handles;

	/* The handles every vertex write and the thread's end send to. */
	handles = state->vue_grf - COMPILE_GS_WRITE_HEADER_REGS;
	drv_i915_eu_alu2(&state->code,
			 I915_EU_AND,
			 drv_i915_eu_grf_ud(handles),
			 drv_i915_eu_grf_ud(COMPILE_GS_OUTPUT_HANDLES_GRF),
			 drv_i915_eu_imm_ud(COMPILE_GS_HANDLE_MASK));

	/* No vertex is emitted yet. */
	counter = drv_i915_eu_grf_ud(state->gs_count_grf);
	counter.type = COMPILE_TYPE_D;
	drv_i915_eu_mov(&state->code, counter, drv_i915_eu_imm_d(0U));

	/* No strip is ended yet. */
	counter = drv_i915_eu_grf_ud(state->gs_cut_grf);
	counter.type = COMPILE_TYPE_D;
	drv_i915_eu_mov(&state->code, counter, drv_i915_eu_imm_d(0U));

	/* The first vertex goes to the start of the vertices: a per-slot offset of zero. */
	counter = drv_i915_eu_grf_ud(handles + 1U);
	counter.type = COMPILE_TYPE_D;
	drv_i915_eu_mov(&state->code, counter, drv_i915_eu_imm_d(0U));
}

/*
 * Lowers a per-vertex input load: one URB read of the slot the input is in,
 * in the producer's VUE of the vertex, from the vertex's handle -- a
 * constant vertex's own register, or one chosen channel by channel.  The
 * value is the reply's register of the component, bit for bit.
 */
static void
i915_compile_load_vertex_input(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_buf *code;
	struct i915_eu_reg null;
	uint32_t slot;
	uint32_t component;
	uint32_t index_grf;
	uint32_t handle_grf;
	uint32_t reply_grf;
	uint32_t vertex;
	uint32_t dst;
	int chosen;
	int found;

	/* Emits into the shader's encoder buffer. */
	code = &state->code;

	/* Only a geometry shader reads a vertex of its input primitive. */
	if (state->ir->stage != DRV_GPU_STAGE_GEOMETRY) {
		state->error = 1;
		return;
	}

	/* Finds the slot and the component of the producer's VUE the input is in. */
	found = i915_compile_vertex_slot(state, inst, &slot, &component);
	if (found != 0) {
		state->unsupported = 1;
		return;
	}

	/* A constant vertex past the primitive's is inconsistent IR. */
	chosen = 0;
	if (inst->immediate == DRV_GPU_IR_VERTEX_DYNAMIC)
		chosen = 1;
	if (chosen == 0 && inst->immediate >= state->ir->vertices_in) {
		state->error = 1;
		return;
	}

	/* Reads the vertex's number when it is chosen at run time, then gives the value its register. */
	index_grf = COMPILE_NO_GRF;
	if (chosen != 0)
		index_grf = i915_compile_grf(state, inst->src[0]);
	dst = i915_compile_define(state, inst->dst, 1U);

	/* A constant vertex's handle is its own register of the payload. */
	if (chosen == 0) {
		handle_grf = state->gs_handle_grf + inst->immediate;
	} else {
		/* A chosen vertex's handle starts as vertex 0's and takes vertex v's on the channels whose number is v. */
		handle_grf = i915_compile_temporary(state);
		if (state->out_of_registers != 0)
			return;
		drv_i915_eu_mov(code, drv_i915_eu_grf_ud(handle_grf), drv_i915_eu_grf_ud(state->gs_handle_grf));
		null = drv_i915_eu_null();
		null.type = COMPILE_TYPE_D;

		/* Compares the number with each later vertex and selects that vertex's handle where it matches. */
		for (vertex = 1U; vertex < state->ir->vertices_in; vertex++) {
			drv_i915_eu_cmp(code,
					I915_EU_COND_EQ,
					I915_EU_FLAG_F0_0,
					0,
					null,
					drv_i915_eu_grf_d(index_grf),
					drv_i915_eu_imm_d(vertex));
			drv_i915_eu_select(code,
					   I915_EU_FLAG_F0_0,
					   drv_i915_eu_grf_ud(handle_grf),
					   drv_i915_eu_grf_ud(state->gs_handle_grf + vertex),
					   drv_i915_eu_grf_ud(handle_grf));
		}
	}

	/* The reply: the slot's components up to the one read, a register each. */
	reply_grf = i915_compile_temporaries(state, component + 1U);
	if (state->out_of_registers != 0)
		return;

	/* Reads the slot of the vertex's VUE. */
	drv_i915_eu_send(code,
			 drv_i915_eu_grf_ud(reply_grf),
			 drv_i915_eu_grf_ud(handle_grf),
			 drv_i915_eu_null(),
			 EU_SFID_URB,
			 COMPILE_DESC_URB_READ(slot, component + 1U),
			 0U,
			 0,
			 0);

	/* The value is the component's register, bit for bit. */
	drv_i915_eu_mov(code, drv_i915_eu_grf_ud(dst), drv_i915_eu_grf_ud(reply_grf + component));

	/* The reply and a chosen handle are free again. */
	for (vertex = 0U; vertex <= component; vertex++)
		state->grf_busy[reply_grf + vertex] = 0U;
	if (chosen != 0)
		state->grf_busy[handle_grf] = 0U;
}

/*
 * Finds where a per-vertex input is in the producer's VUE: gl_Position is
 * slot 1, gl_PointSize component 3 of the header (slot 0), and a located
 * input the slot after the position of its location among the producer's
 * varyings.  Returns ENOENT for a location the producer does not write, or
 * when there is no producer to say.
 */
static int
i915_compile_vertex_slot(
	const struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst,
	uint32_t *slot,
	uint32_t *component)
{
	uint32_t rank;
	int found;

	/* A component beyond w is not lowered. */
	if (inst->component > 3U)
		return ENOENT;

	/* The position follows the header; the point size is the header's last dword; a located input is a varying. */
	if (inst->location == DRV_GPU_IR_LOCATION_POSITION) {
		*slot = 1U;
		*component = inst->component;
	} else if (inst->location == DRV_GPU_IR_LOCATION_POINT_SIZE) {
		*slot = 0U;
		*component = COMPILE_VUE_POINT_SIZE;
	} else {
		/* A located input needs the producer's varyings to be placed. */
		if (state->producer == NULL)
			return ENOENT;

		/* Its slot follows the position, in the producer's ascending location order. */
		found = i915_compile_rank(state->producer->varying_locations, state->producer->varying_count, inst->location, &rank);
		if (found != 0)
			return found;
		*slot = 2U + rank;
		*component = inst->component;
	}

	/* Succeeded: the input's slot and component. */
	return 0;
}

/* Lowers a geometry thread's LOAD_SYSTEM: the input primitive's number, a dword to a channel of its own payload register. */
static void
i915_compile_geometry_system(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t dst;

	/* A geometry thread has no other built-in; reading this one without asking for it is inconsistent IR. */
	if (inst->component != DRV_GPU_IR_SYSTEM_PRIMITIVE_ID || state->ir->uses_primitive_id == 0U) {
		state->error = 1;
		return;
	}

	/* Moves the number into the value's register. */
	dst = i915_compile_define(state, inst->dst, 1U);
	drv_i915_eu_mov(&state->code, drv_i915_eu_grf_ud(dst), drv_i915_eu_grf_ud(COMPILE_GS_PRIMITIVE_ID_GRF));
}

/*
 * Ends a geometry thread: writes the cut bits gathered last, then how many
 * vertices it emitted into the first dword of its output URB entry, and
 * retires (Mesa's thread end with a dynamic vertex count).  The handles and
 * the count are the payload in r112..r127, as a message that ends the
 * thread needs.
 */
static void
i915_compile_terminate_geometry(
	struct i915_compile_state *state)
{
	struct i915_eu_buf *code;
	struct i915_eu_reg null;

	/* Emits into the shader's encoder buffer. */
	code = &state->code;

	/*
	 * The cut bits gathered since the last write: all of them in one dword
	 * when they are 32 or fewer; past that, the dword of the last vertex,
	 * for the channels that emitted one (no vertex, no dword to name).
	 */
	if (state->gs_control_bits > COMPILE_GS_CUT_DWORD_BITS) {
		null = drv_i915_eu_null();
		null.type = COMPILE_TYPE_D;
		drv_i915_eu_cmp(code, I915_EU_COND_NE, I915_EU_FLAG_F0_1, 0, null, drv_i915_eu_grf_d(state->gs_count_grf), drv_i915_eu_imm_d(0U));
		i915_compile_control_write(state, 1);
	} else if (state->gs_control_bits != 0U) {
		i915_compile_control_write(state, 0);
	}

	/* The handles, without the instance bits, and the count, in the last two registers. */
	drv_i915_eu_mov(code, drv_i915_eu_grf_ud(COMPILE_MAX_GRF), drv_i915_eu_grf_ud(state->vue_grf - COMPILE_GS_WRITE_HEADER_REGS));
	drv_i915_eu_mov(code, drv_i915_eu_grf_ud(COMPILE_MAX_GRF - 1U), drv_i915_eu_grf_ud(state->gs_count_grf));

	/* Writes the count at the entry's start and ends the thread. */
	drv_i915_eu_send(code,
			 drv_i915_eu_null(),
			 drv_i915_eu_grf(COMPILE_MAX_GRF),
			 drv_i915_eu_grf(COMPILE_MAX_GRF - 1U),
			 EU_SFID_URB,
			 COMPILE_DESC_URB_WRITE(0U),
			 COMPILE_EX_MLEN(1U),
			 0,
			 1);
}

/*
 * Lowers EMIT_VERTEX: for the channels of the emit's block that have room
 * for one more vertex, writes the staged VUE at the vertex's place in the
 * output entry, then counts the vertex and moves the place on.  Past 32 cut
 * bits, the vertex that starts a new dword first writes the dword before.
 */
static void
i915_compile_emit_vertex(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_buf *code;
	struct i915_eu_reg null;
	uint32_t predicate_grf;
	uint32_t live_grf;
	uint32_t handles;
	uint32_t vertices;
	uint32_t slots;
	uint32_t first;
	uint32_t count;
	uint32_t descriptor;

	/* Emits into the shader's encoder buffer. */
	code = &state->code;

	/* Reads the predicate of the emit's block when it has one. */
	predicate_grf = COMPILE_NO_GRF;
	if (inst->component != 0U)
		predicate_grf = i915_compile_grf(state, inst->src[0]);

	/* Takes the register of the channels that emit. */
	live_grf = i915_compile_temporary(state);
	if (state->out_of_registers != 0)
		return;

	/* The channels with room for one more vertex, and of those the ones whose block runs the emit. */
	drv_i915_eu_cmp(code,
			I915_EU_COND_LT,
			I915_EU_FLAG_F0_0,
			0,
			drv_i915_eu_grf_d(live_grf),
			drv_i915_eu_grf_d(state->gs_count_grf),
			drv_i915_eu_imm_d(state->ir->max_vertices));
	if (predicate_grf != COMPILE_NO_GRF)
		drv_i915_eu_alu2(code, I915_EU_AND, drv_i915_eu_grf_d(live_grf), drv_i915_eu_grf_d(live_grf), drv_i915_eu_grf_d(predicate_grf));

	/* Past 32 cut bits, a vertex that starts a new dword writes the dword before first. */
	if (state->gs_control_bits > COMPILE_GS_CUT_DWORD_BITS)
		i915_compile_flush_cut_bits(state, live_grf);

	/* f0.0 holds the channels that emit. */
	null = drv_i915_eu_null();
	null.type = COMPILE_TYPE_D;
	drv_i915_eu_cmp(code, I915_EU_COND_NE, I915_EU_FLAG_F0_0, 0, null, drv_i915_eu_grf_d(live_grf), drv_i915_eu_imm_d(0U));

	/* Writes the staged VUE two slots at a time, after the vertex count and the control data, at the vertex's place. */
	handles = state->vue_grf - COMPILE_GS_WRITE_HEADER_REGS;
	slots = 2U + state->varying_count;
	for (first = 0U; first < slots; first += count) {
		/* Two slots a write, one for a lone last slot. */
		count = COMPILE_URB_WRITE_SLOTS;
		if (slots - first < COMPILE_URB_WRITE_SLOTS)
			count = slots - first;

		/* The slots after the vertex count and the control data header, offset per channel by the vertex's place. */
		descriptor = i915_compile_urb_write_descriptor(COMPILE_GS_CONTROL_OWORD + 2U * state->gs_control_hwords + first,
							      COMPILE_GS_WRITE_HEADER_REGS,
							      1,
							      0);
		drv_i915_eu_send_masked(code,
					I915_EU_FLAG_F0_0,
					drv_i915_eu_null(),
					drv_i915_eu_grf_ud(handles),
					drv_i915_eu_grf_ud(state->vue_grf + 4U * first),
					EU_SFID_URB,
					descriptor,
					COMPILE_EX_MLEN(4U * count),
					0,
					0);
	}

	/* Counts the vertex and moves the place on by one vertex, in OWords. */
	vertices = 2U * state->gs_vertex_hwords;
	drv_i915_eu_alu2_masked(code,
				I915_EU_FLAG_F0_0,
				I915_EU_ADD,
				drv_i915_eu_grf_d(state->gs_count_grf),
				drv_i915_eu_grf_d(state->gs_count_grf),
				drv_i915_eu_imm_d(1U));
	drv_i915_eu_alu2_masked(code,
				I915_EU_FLAG_F0_0,
				I915_EU_ADD,
				drv_i915_eu_grf_d(handles + 1U),
				drv_i915_eu_grf_d(handles + 1U),
				drv_i915_eu_imm_d(vertices));

	/* The register of the channels that emit is free again. */
	state->grf_busy[live_grf] = 0U;
}

/*
 * Writes the cut bits gathered so far when a vertex starts a new dword of
 * them (the count a multiple of 32), for the channels in `live_grf` that
 * emit: the dword before is written unless no vertex came before, and the
 * gathering starts again from zero (which also forgets an end before the
 * first vertex).
 */
static void
i915_compile_flush_cut_bits(
	struct i915_compile_state *state,
	uint32_t live_grf)
{
	struct i915_eu_buf *code;
	struct i915_eu_reg null;
	uint32_t start_grf;
	uint32_t nonzero_grf;

	/* Emits into the shader's encoder buffer. */
	code = &state->code;

	/* Takes the registers of the two decisions. */
	start_grf = i915_compile_temporary(state);
	if (state->out_of_registers != 0)
		return;
	nonzero_grf = i915_compile_temporary(state);
	if (state->out_of_registers != 0)
		return;

	/* The emitting channels whose count is a multiple of 32: they start a new dword. */
	drv_i915_eu_alu2(code,
			 I915_EU_AND,
			 drv_i915_eu_grf_d(start_grf),
			 drv_i915_eu_grf_d(state->gs_count_grf),
			 drv_i915_eu_imm_d(COMPILE_GS_CUT_DWORD_BITS - 1U));
	drv_i915_eu_cmp(code,
			I915_EU_COND_EQ,
			I915_EU_FLAG_F0_1,
			0,
			drv_i915_eu_grf_d(start_grf),
			drv_i915_eu_grf_d(start_grf),
			drv_i915_eu_imm_d(0U));
	drv_i915_eu_alu2(code, I915_EU_AND, drv_i915_eu_grf_d(start_grf), drv_i915_eu_grf_d(start_grf), drv_i915_eu_grf_d(live_grf));

	/* Of those, the ones with a dword before to write. */
	drv_i915_eu_cmp(code,
			I915_EU_COND_NE,
			I915_EU_FLAG_F0_1,
			0,
			drv_i915_eu_grf_d(nonzero_grf),
			drv_i915_eu_grf_d(state->gs_count_grf),
			drv_i915_eu_imm_d(0U));
	drv_i915_eu_alu2(code, I915_EU_AND, drv_i915_eu_grf_d(nonzero_grf), drv_i915_eu_grf_d(nonzero_grf), drv_i915_eu_grf_d(start_grf));

	/* Writes the dword before for them. */
	null = drv_i915_eu_null();
	null.type = COMPILE_TYPE_D;
	drv_i915_eu_cmp(code, I915_EU_COND_NE, I915_EU_FLAG_F0_1, 0, null, drv_i915_eu_grf_d(nonzero_grf), drv_i915_eu_imm_d(0U));
	i915_compile_control_write(state, 1);

	/*
	 * Starts gathering again on every channel that starts a new dword
	 * (under f0.0, which the emit sets again after: Mesa's brw_asm reads a
	 * predicated AND with an immediate back as f0.0 whatever its flag).
	 */
	drv_i915_eu_cmp(code, I915_EU_COND_NE, I915_EU_FLAG_F0_0, 0, null, drv_i915_eu_grf_d(start_grf), drv_i915_eu_imm_d(0U));
	drv_i915_eu_alu2_masked(code,
				I915_EU_FLAG_F0_0,
				I915_EU_AND,
				drv_i915_eu_grf_d(state->gs_cut_grf),
				drv_i915_eu_grf_d(state->gs_cut_grf),
				drv_i915_eu_imm_d(0U));

	/* The registers of the decisions are free again. */
	state->grf_busy[start_grf] = 0U;
	state->grf_busy[nonzero_grf] = 0U;
}

/*
 * Writes the dword of cut bits the last vertex emitted falls in, for the
 * channels of f0.1 when `predicated`, all of them otherwise.  With 32 bits
 * or fewer the dword is the header's only one, written whole; past that a
 * channel mask picks the dword in its OWord and, past 128 bits, a per-slot
 * offset picks the OWord (Mesa's emit_gs_control_data_bits()).
 */
static void
i915_compile_control_write(
	struct i915_compile_state *state,
	int predicated)
{
	struct i915_eu_buf *code;
	uint32_t header_grf;
	uint32_t data_grf;
	uint32_t index_grf;
	uint32_t header_regs;
	uint32_t data_regs;
	uint32_t descriptor;
	uint32_t next;
	uint32_t data;
	int per_slot;
	int channel_mask;

	/* Emits into the shader's encoder buffer. */
	code = &state->code;

	/* Past 32 bits a channel mask picks the dword, past 128 bits a per-slot offset the OWord. */
	channel_mask = 0;
	per_slot = 0;
	if (state->gs_control_bits > COMPILE_GS_CUT_DWORD_BITS)
		channel_mask = 1;
	if (state->gs_control_bits > COMPILE_GS_CUT_OWORD_BITS)
		per_slot = 1;

	/* The handles, the per-slot offsets and the channel mask; the dword once, or four times under a mask. */
	header_regs = 1U + (uint32_t)per_slot + (uint32_t)channel_mask;
	data_regs = 1U;
	if (channel_mask != 0)
		data_regs = COMPILE_GS_MASKED_DATA_REGS;

	/* Takes the registers of the message and of the dword's index. */
	header_grf = i915_compile_temporaries(state, header_regs);
	if (state->out_of_registers != 0)
		return;
	data_grf = i915_compile_temporaries(state, data_regs);
	if (state->out_of_registers != 0)
		return;
	index_grf = i915_compile_temporary(state);
	if (state->out_of_registers != 0)
		return;

	/* The handles every write goes to. */
	drv_i915_eu_mov(code, drv_i915_eu_grf_ud(header_grf), drv_i915_eu_grf_ud(state->vue_grf - COMPILE_GS_WRITE_HEADER_REGS));

	/* The dword of the last vertex's bit: (count - 1) / 32. */
	drv_i915_eu_alu2(code, I915_EU_ADD, drv_i915_eu_grf_d(index_grf), drv_i915_eu_grf_d(state->gs_count_grf), drv_i915_eu_imm_d(0xFFFFFFFFU));
	drv_i915_eu_alu2(code, I915_EU_SHR, drv_i915_eu_grf_ud(index_grf), drv_i915_eu_grf_ud(index_grf), drv_i915_eu_imm_ud(COMPILE_GS_CUT_DWORD_SHIFT));

	/* The OWord of the dword: its index / 4. */
	next = header_grf + 1U;
	if (per_slot != 0) {
		drv_i915_eu_alu2(code,
				 I915_EU_SHR,
				 drv_i915_eu_grf_ud(next),
				 drv_i915_eu_grf_ud(index_grf),
				 drv_i915_eu_imm_ud(COMPILE_GS_OWORD_DWORDS_SHIFT));
		next++;
	}

	/* The dword in its OWord: bit (index % 4) of the mask's bits 23:16. */
	if (channel_mask != 0) {
		drv_i915_eu_alu2(code, I915_EU_AND, drv_i915_eu_grf_ud(index_grf), drv_i915_eu_grf_ud(index_grf), drv_i915_eu_imm_ud(COMPILE_GS_OWORD_DWORD_MASK));
		drv_i915_eu_mov(code, drv_i915_eu_grf_ud(next), drv_i915_eu_imm_ud(1U));
		drv_i915_eu_alu2(code, I915_EU_SHL, drv_i915_eu_grf_ud(next), drv_i915_eu_grf_ud(next), drv_i915_eu_grf_ud(index_grf));
		drv_i915_eu_alu2(code, I915_EU_SHL, drv_i915_eu_grf_ud(next), drv_i915_eu_grf_ud(next), drv_i915_eu_imm_ud(COMPILE_GS_CHANNEL_MASK_SHIFT));
	}

	/* The cut bits, once for each dword the write may land on. */
	for (data = 0U; data < data_regs; data++)
		drv_i915_eu_mov(code, drv_i915_eu_grf_ud(data_grf + data), drv_i915_eu_grf_ud(state->gs_cut_grf));

	/* Writes them after the vertex count. */
	descriptor = i915_compile_urb_write_descriptor(COMPILE_GS_CONTROL_OWORD, header_regs, per_slot, channel_mask);
	if (predicated != 0) {
		drv_i915_eu_send_masked(code,
					I915_EU_FLAG_F0_1,
					drv_i915_eu_null(),
					drv_i915_eu_grf_ud(header_grf),
					drv_i915_eu_grf_ud(data_grf),
					EU_SFID_URB,
					descriptor,
					COMPILE_EX_MLEN(data_regs),
					0,
					0);
	} else {
		drv_i915_eu_send(code,
				 drv_i915_eu_null(),
				 drv_i915_eu_grf_ud(header_grf),
				 drv_i915_eu_grf_ud(data_grf),
				 EU_SFID_URB,
				 descriptor,
				 COMPILE_EX_MLEN(data_regs),
				 0,
				 0);
	}

	/* The registers of the message are free again. */
	for (data = 0U; data < header_regs; data++)
		state->grf_busy[header_grf + data] = 0U;
	for (data = 0U; data < data_regs; data++)
		state->grf_busy[data_grf + data] = 0U;
	state->grf_busy[index_grf] = 0U;
}

/*
 * Lowers END_PRIMITIVE: sets the cut bit of the last vertex emitted, bit
 * (count - 1) % 32 of the gathered dword (the SHL reads the low five bits
 * of its count; Mesa's emit_gs_end_primitive()), for the channels of the
 * end's block.
 */
static void
i915_compile_end_primitive(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_buf *code;
	struct i915_eu_reg null;
	uint32_t predicate_grf;
	uint32_t bit_grf;

	/* Emits into the shader's encoder buffer. */
	code = &state->code;

	/* A shader without cut bits (points) has no strip to end. */
	if (state->gs_control_bits == 0U)
		return;

	/* Reads the predicate of the end's block when it has one. */
	predicate_grf = COMPILE_NO_GRF;
	if (inst->component != 0U)
		predicate_grf = i915_compile_grf(state, inst->src[0]);

	/* Takes the registers of the bit. */
	bit_grf = i915_compile_temporaries(state, 2U);
	if (state->out_of_registers != 0)
		return;

	/* The bit of the last vertex: 1 << (count - 1). */
	drv_i915_eu_alu2(code, I915_EU_ADD, drv_i915_eu_grf_d(bit_grf + 1U), drv_i915_eu_grf_d(state->gs_count_grf), drv_i915_eu_imm_d(0xFFFFFFFFU));
	drv_i915_eu_mov(code, drv_i915_eu_grf_ud(bit_grf), drv_i915_eu_imm_ud(1U));
	drv_i915_eu_alu2(code, I915_EU_SHL, drv_i915_eu_grf_ud(bit_grf), drv_i915_eu_grf_ud(bit_grf), drv_i915_eu_grf_ud(bit_grf + 1U));

	/* Sets it for the channels of the block, all of them when the block always runs. */
	if (predicate_grf != COMPILE_NO_GRF) {
		null = drv_i915_eu_null();
		null.type = COMPILE_TYPE_D;
		drv_i915_eu_cmp(code, I915_EU_COND_NE, I915_EU_FLAG_F0_0, 0, null, drv_i915_eu_grf_d(predicate_grf), drv_i915_eu_imm_d(0U));
		drv_i915_eu_alu2_masked(code,
					I915_EU_FLAG_F0_0,
					I915_EU_OR,
					drv_i915_eu_grf_ud(state->gs_cut_grf),
					drv_i915_eu_grf_ud(state->gs_cut_grf),
					drv_i915_eu_grf_ud(bit_grf));
	} else {
		drv_i915_eu_alu2(code,
				 I915_EU_OR,
				 drv_i915_eu_grf_ud(state->gs_cut_grf),
				 drv_i915_eu_grf_ud(state->gs_cut_grf),
				 drv_i915_eu_grf_ud(bit_grf));
	}

	/* The registers of the bit are free again. */
	state->grf_busy[bit_grf] = 0U;
	state->grf_busy[bit_grf + 1U] = 0U;
}

/*
 * Returns the descriptor of a SIMD8 URB write at OWord `global`, whose
 * first payload run is `header_regs` registers -- the handles, then the
 * per-slot offsets and the channel mask when present.
 */
static uint32_t
i915_compile_urb_write_descriptor(
	uint32_t global,
	uint32_t header_regs,
	int per_slot,
	int channel_mask)
{
	uint32_t descriptor;

	/* The header run, the write and its offset. */
	descriptor = (header_regs << EU_DESC_MLEN_SHIFT) | EU_DESC_HEADER_PRESENT | COMPILE_URB_OPCODE_SIMD8_WRITE;
	descriptor |= global << COMPILE_URB_GLOBAL_OFFSET_SHIFT;

	/* What follows the handles. */
	if (per_slot != 0)
		descriptor |= COMPILE_URB_PER_SLOT_PRESENT;
	if (channel_mask != 0)
		descriptor |= COMPILE_URB_CHANNEL_MASK_PRESENT;

	/* Succeeded: the descriptor. */
	return descriptor;
}

/* Records in the binary what the draw programs around a geometry kernel. */
static void
i915_compile_describe_geometry(
	const struct i915_compile_state *state,
	struct i915_shader_binary *binary)
{
	/* The varyings it writes, as a vertex kernel's, and where its push data starts. */
	binary->varying_count = state->varying_count;
	kern_memcpy(binary->varying_locations, state->varyings, sizeof(state->varyings));
	binary->dispatch_grf_start = state->gs_push_grf;

	/* The primitives it takes and emits. */
	binary->vertices_in = state->ir->vertices_in;
	binary->output_topology = state->ir->output_topology;

	/* The layout of its output URB entry. */
	binary->output_vertex_hwords = state->gs_vertex_hwords;
	binary->control_data_hwords = state->gs_control_hwords;
	binary->urb_entry_size = state->gs_urb_entry_size;

	/* Points carry stream IDs in their control data; strips cut bits. */
	binary->control_data_format = COMPILE_GS_CONTROL_CUT;
	if (state->ir->output_topology == DRV_GPU_IR_OUTPUT_POINTS)
		binary->control_data_format = COMPILE_GS_CONTROL_SID;

	/* What the payload carries and what the clipper reads from the VUE. */
	binary->uses_primitive_id = state->ir->uses_primitive_id;
	binary->writes_layer = state->ir->writes_layer;
}
