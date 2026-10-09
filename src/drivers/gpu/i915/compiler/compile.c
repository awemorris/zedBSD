/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Baseline shader code generator: lowers the scalar IR to Gen12 EU code one
 * instruction at a time, no optimization.
 *
 * An IR value is 32 bits (a float, an integer or a Boolean) per SIMD
 * channel, which is exactly one general register in a SIMD8 dispatch, so a
 * value lives in one register from its definition to its last use.  The IR
 * is SSA apart from the loop variables, so the last use is known from one
 * look over the instructions, widened for loops: a value made before a loop
 * and read inside it lives to the loop's end, since the loop reads it again
 * on every pass.  A Boolean is all ones or zero, as CMP writes it; it is
 * read as a signed integer.
 *
 * When more values live at once than there are registers, the shader is
 * lowered again with one more value moved to scratch memory, until it fits:
 * the value that lives longest past the point where the registers ran out.
 * A value in scratch memory has no register of its own; every definition
 * writes it out and every instruction that reads it reads it back into a
 * temporary first -- what Mesa's brw_spill_reg() does to a spilled virtual
 * register on Gen12.0 (brw_reg_allocate.cpp).  A vertex shader that runs out
 * first gathers its VUE at the end instead of staging it for its whole run.
 *
 * The register and message conventions are the ones Mesa's compiler uses for
 * the same shaders (tools/refvk.c, disassembled with gentool) -- see the table
 * below -- and the emitted kernels are judged by Mesa's assembler and
 * disassembler (plan/ws031/tests/run-vk-gentool-test.sh) before they meet a
 * GPU.  The encoded words are returned for the caller to place in a GPU
 * object.
 */

#include "compiler.h"
#include "eu.h"
#include <kern/kcrt.h>

#include <kern/kmem.h>

#include <uapi/errno.h>

#include "../intel/eu-encoding-gen12.h"

/*
 * Register conventions of a SIMD8 dispatch on Gen12 (one register = one 32-bit value to a channel).
 *
 * Vertex shader payload:   r0 header, r1 the URB handles of the eight vertices,
 *                          r2.. the push data (32 bytes to a register, scalar regions): the push constants,
 *                          then the uniform blocks, then four registers (x y z w) to an attribute, attributes
 *                          in ascending location order -- the order of the vertex elements the draw programs.
 * Vertex shader output:    the VUE is [header][position][varyings in ascending location order], four
 *                          registers to a slot, staged at r(127 - 4 * slots) .. r126 so that its last two
 *                          slots are the payload of the write that ends the thread, which has to sit in
 *                          r112..r127: the handles copied to r127, the slots right below.  The slots go out
 *                          two at a time from the end (Mesa's emit_urb_writes() writes eight registers of a
 *                          SIMD8 VUE at most); every write but the last takes the handles from r1.
 * Fragment shader payload: r0 header, r1 pixel positions, r2 / r3 the two perspective barycentrics of
 *                          each pixel, then only when the kernel reads them the two linear barycentrics,
 *                          the source depth and the source w, a register each (brw_fs_thread_payload.cpp),
 *                          the push data, then two registers to an input (ascending
 *                          location): component c keeps its plane in floats 4 * (c & 1) .. + 3 of
 *                          register c / 2 as [d/d bary1, d/d bary2, -, value at the origin].
 *                          Gen11+ has no PLN: value = origin + d1 * bary1 + d2 * bary2, written out,
 *                          with the linear barycentrics for an input without perspective.
 * Fragment shader output:  location n (0 to 3) in r(124 - 4 n)..r(127 - 4 n); one render-target write for
 *                          each location the shader stores, in ascending order, to binding table entry
 *                          I915_SHADER_RT_BTI(n) with render target index n in the extended descriptor
 *                          (Mesa's lower_fb_write_logical_send() on Gen11+); the last one ends the thread.
 * Texture:                 one SIMD8 "sample" message, u and v each its own payload run, the reply
 *                          four registers; binding table entry 1 + n, sampler n for the n-th
 *                          sampled image of the shader (entry 0 is the render target).
 * Registers after the payload: the one temporary of the interpolation in r15, the values from r16 --
 *                          or, when the payload reaches r15, right after the payload -- up to r95 or to
 *                          the staged VUE, whichever comes first.  The temporary of a multi-instruction
 *                          lowering (the high partial product of a 32-bit integer multiply) is taken from
 *                          the same registers for the length of that one lowering.
 * Gathered VUE:            a vertex shader whose staged VUE leaves too few registers keeps, instead, the
 *                          value each output component last stored alive to the end, and writes the VUE
 *                          there two slots at a time through r119..r126 (the handles in r127 for the
 *                          last write, which ends the thread).
 * Scratch memory:          a shader that spills gives the highest value register to the header of its
 *                          scratch messages, built once at the start outside the channel mask (dword 3
 *                          the per-thread scratch space from r0.3, dword 5 the thread's scratch base
 *                          from r0.5; Mesa's generate_scratch_header()).  Spilled value n lives at byte
 *                          32 n of the thread's scratch space: dword 2 of the header takes the offset in
 *                          OWords, then an OWord block write of the register under the execution mask
 *                          (so a channel a loop has stopped keeps what it wrote), or an OWord block read
 *                          into a temporary outside the mask, to the stateless binding table entry 253.
 *                          The per-thread scratch space, a power of two from 1 KiB, is in the binary for
 *                          the draw to program (3DSTATE_VS / PS).
 * Flags:                   f0.0 is written by every comparison and read by the SEL of a SELECT or by the
 *                          WHILE that ends a loop, each right after its own comparison; a fragment shader
 *                          that discards keeps its live pixels in f1.0, loaded from the dispatch mask
 *                          (dword 7 of r1) at the start, and predicates its render-target write on it
 *                          (Mesa: brw_compile_fs.cpp, sample_mask_flag_subreg() is f1.0).
 * Loops:                   the body runs between the loop's first instruction and a WHILE predicated on
 *                          the channels that go round again (brw_WHILE); a channel that stops waits after
 *                          the WHILE until the others stop too, so it no longer writes a register.
 * Geometry shader:         compile-geometry.inc (ws075-p007a).
 */

/* Vertex: the push data starts here. */
#define COMPILE_PAYLOAD_GRF	2U

/* Compute: the push data starts right after the thread header (compile-compute.inc). */
#define COMPILE_CS_PUSH_GRF	1U

/* Fragment: the two perspective barycentrics. */
#define COMPILE_FS_BARY1_GRF	2U
#define COMPILE_FS_BARY2_GRF	3U

/*
 * Fragment: the register after the perspective barycentrics, where the
 * payload goes on with what the kernel asked for, then the push data and the
 * input planes.
 */
#define COMPILE_FS_SETUP_GRF	4U

/*
 * Fragment: the subspan coordinates of r1 (words 4 .. 7: x and y of
 * subspan 0, then of subspan 1), and the offsets of the four pixels of a
 * subspan (x 0 1 0 1, y 0 0 1 1) as an immediate of eight nibbles
 * (brw_compile_fs.cpp, Gen12.0).
 */
#define COMPILE_FS_SUBSPAN_BYTE		8U
#define COMPILE_FS_PIXEL_OFFSETS	0x11001010U

/* Fragment: where the y of the pixels starts among the sixteen words of positions. */
#define COMPILE_FS_PIXEL_Y_BYTE		8U

/* The bits of the float 0.5: the centre of a pixel. */
#define COMPILE_FLOAT_HALF		0x3F000000U

/*
 * Fragment: the dispatched pixels, the low 16 bits of dword 7 of r1 (byte
 * 28), which Mesa reads as brw_vec1_grf(1, 7) retyped to UW: g1.14<0,1,0>UW.
 */
#define COMPILE_FS_DISPATCH_GRF		1U
#define COMPILE_FS_DISPATCH_BYTE	28U

/* The one temporary of the interpolation, unless the payload reaches it. */
#define COMPILE_SCRATCH_GRF	15U

/* The registers values are given: from right after the temporary to r95 at most. */
#define COMPILE_FIRST_VALUE_GRF	16U
#define COMPILE_LAST_VALUE_GRF	95U

/* A message that ends the thread reads r112..r127 only. */
#define COMPILE_EOT_GRF		112U

#define COMPILE_MAX_GRF		127U

/* The value_grf entry of a value that has no register yet. */
#define COMPILE_NO_GRF		0U

/* The def_index entry of a value no instruction defines. */
#define COMPILE_NO_INDEX	0xFFFFFFFFU

/* No value: a victim not found, an output component never stored. */
#define COMPILE_NO_VALUE	0xFFFFFFFFU

/* Vertex, gathered VUE: the two slots of each write, and the handles of the last one in r127. */
#define COMPILE_GATHER_GRF	119U

/* Vertex: the point size is dword 3 of the VUE header (component 3 of slot 0). */
#define COMPILE_VUE_POINT_SIZE	3U

/* Geometry: the layer is dword 1 of the VUE header, the render target array index (ws075-p007a). */
#define COMPILE_VUE_LAYER	1U

/*
 * Geometry: the registers right below the staged VUE that head each vertex
 * write -- the output handles, then the per-slot offsets of the vertex
 * (ws075-p007a, compile-geometry.inc).
 */
#define COMPILE_GS_WRITE_HEADER_REGS	2U

/* The spilled values one instruction reads at most (three sources), and defines at most (a sample's four). */
#define COMPILE_MAX_FILLS	(DRV_GPU_IR_TEXTURE_MAX_PARAMS + 1U)
#define COMPILE_MAX_SPILLS	4U

/* A spilled value takes one register of scratch memory: 32 bytes, two OWords. */
#define COMPILE_SLOT_BYTES	32U
#define COMPILE_OWORD_BYTES	16U

/* The per-thread scratch space 3DSTATE_VS / PS can describe: 1 KiB to 2 MiB, a power of two. */
#define COMPILE_SCRATCH_MIN_BYTES	1024U
#define COMPILE_SCRATCH_MAX_BYTES	(2048U * 1024U)

/* Vertex: the varyings after the position, each a VUE slot of its own. */
#define COMPILE_MAX_VARYINGS	I915_SHADER_MAX_INPUTS

/* Vertex attributes, or fragment inputs, in the payload. */
#define COMPILE_MAX_INPUTS	I915_SHADER_MAX_INPUTS

/* The highest sampler index the sample descriptor can name. */
#define COMPILE_MAX_SAMPLER	14U

/* The deepest nesting of loops the code generator follows. */
#define COMPILE_MAX_LOOPS	16U

/*
 * The most texture messages of one shader that run under an IF on their
 * guard (ws075-p021); the ones past it run unguarded, as before.
 */
#define COMPILE_MAX_GUARDS	256U

/*
 * The most IR instructions a shader may have for its skippable regions to be
 * checked (ws075-p023); a longer one runs every region.  The check walks the
 * rest of the shader once per region.
 */
#define COMPILE_MAX_SKIP_CHECK	16384U

/* How deep i915_compile_bool_table() follows ANDs, ORs and NOTs. */
#define COMPILE_IMPLIES_DEPTH	16U

/*
 * The Booleans a region check works out truth tables over, at most, and the
 * 64-bit words of one table (one entry per assignment of the atoms).
 */
#define COMPILE_SKIP_ATOMS	8U
#define COMPILE_SKIP_WORDS	4U

/*
 * The IR instructions a region needs for its IF: the IF costs six EU
 * instructions (clearing the flag, two comparisons, the flag's store, IF and
 * ENDIF) and a jump, more than a shorter region saves (the compositor's
 * panel.frag: the next comparison of an else-if chain, two instructions, and
 * the selects of a merge, four; ws075-p023).
 */
#define COMPILE_SKIP_MIN_INSTRUCTIONS	6U

/* The VUE slots a URB write carries at most: eight registers of a SIMD8 VUE. */
#define COMPILE_URB_WRITE_SLOTS	2U

/* Every kernel is a SIMD8 kernel. */
#define COMPILE_SIMD		8U

/* The EU's signed 32-bit integer type, which the VUE header is written as. */
#define COMPILE_TYPE_D		6U

/* The EU's unsigned 32-bit integer type, which a word copied bit for bit is read as. */
#define COMPILE_TYPE_UD		2U

/* The shared functions the kernels send messages to. */
#define COMPILE_SFID_SAMPLER		2U
#define COMPILE_SFID_RENDER_CACHE	5U
#define COMPILE_SFID_URB		6U
#define COMPILE_SFID_DATA_CACHE_1	12U

/*
 * Message descriptors (gentool's reading of Mesa's kernels for the same shaders).
 *
 * URB write: mlen 1 (handles), header, SIMD8 write, the first slot in the
 * global offset.  Sample: mlen 1, rlen 4, SIMD8 sample.  Render-target write:
 * mlen 4, SIMD8 single source, last target, entry 0.  The extended descriptor
 * carries the second run's length.
 */
#define COMPILE_DESC_URB_WRITE(slot)	(0x02080007U | ((uint32_t)(slot) << 4))
#define COMPILE_DESC_SAMPLE(bti, smp)	(0x02420000U | ((uint32_t)(smp) << 8) | (uint32_t)(bti))
#define COMPILE_DESC_RT_WRITE		0x08031400U
#define COMPILE_DESC_RT_LAST		0x00001000U

/*
 * The SIMD8 dual-source render-target write (ws031-p032): message control 2,
 * "SIMD8 dual source subspan01" (Mesa 25.0.7 brw_eu_defines.h
 * BRW_DATAPORT_RENDER_TARGET_WRITE_SIMD8_DUAL_SOURCE_SUBSPAN01) in place of
 * the single source's 4; src0 the first colour (mlen 4), src1 the second
 * (the extended descriptor's mlen 4), as Mesa's split send of it.
 */
#define COMPILE_DESC_RT_WRITE_DUAL	0x08031200U
#define COMPILE_EX_MLEN(n)		((uint32_t)(n) << 6)
#define COMPILE_EX_RT_INDEX(n)		((uint32_t)(n) << 12)

/*
 * A64 untyped surface read and write of one channel, SIMD8, stateless
 * (Mesa's brw_dp_a64_untyped_surface_rw_desc(): message type 0x11 / 0x19,
 * channel mask 0xe, SIMD mode 2, binding table entry 253): the 64-bit
 * addresses in two registers, the read's word in one reply register, the
 * write's in one register of the second run.
 */
#define COMPILE_DESC_A64_READ		((2U << 25) | (1U << 20) | (0x11U << 14) | (0x2eU << 8) | 253U)
#define COMPILE_DESC_A64_WRITE		((2U << 25) | (0x19U << 14) | (0x2eU << 8) | 253U)

/*
 * The scratch messages (Mesa's emit_spill() and emit_unspill() before LSC):
 * the header as the one-register payload, a stateless non-coherent OWord
 * block write of one register -- the data the second payload run -- or an
 * OWord block read of one register.
 */
#define COMPILE_DESC_SCRATCH_WRITE	((1U << EU_DESC_MLEN_SHIFT) | \
					 EU_DESC_HEADER_PRESENT | \
					 (EU_DP_OWORD_BLOCK_WRITE << EU_DP_TYPE_SHIFT) | \
					 (EU_DP_OWORD_BLOCK_2_OWORDS << EU_DP_CONTROL_SHIFT) | \
					 EU_BTI_STATELESS_NON_COHERENT)
#define COMPILE_DESC_SCRATCH_READ	((1U << EU_DESC_MLEN_SHIFT) | \
					 (1U << EU_DESC_RLEN_SHIFT) | \
					 EU_DESC_HEADER_PRESENT | \
					 (EU_DP_OWORD_BLOCK_READ << EU_DP_TYPE_SHIFT) | \
					 (EU_DP_OWORD_BLOCK_2_OWORDS << EU_DP_CONTROL_SHIFT) | \
					 EU_BTI_STATELESS_NON_COHERENT)

/*
 * One loop of the IR: the index of its LOOP_BEGIN and of its LOOP_END.
 *
 * The liveness pass lists them in the order their ends come, so an inner
 * loop is always before the loop around it.
 */
struct i915_compile_loop {
	uint32_t begin;
	uint32_t end;
};

/*
 * The lowering state threaded through the instruction walk.
 *
 * It lives on the stack of drv_i915_shader_compile() for one compile and
 * owns the encoder buffer and the value maps until the compile ends.  Each
 * attempt at lowering clears it but for the IR, the maps, the values given
 * to scratch memory and the choice of a gathered VUE, which only grow from
 * one attempt to the next.
 */
struct i915_compile_state {
	const struct drv_gpu_shader_ir *ir;
	struct i915_eu_buf code;

	/* The register a value lives in; COMPILE_NO_GRF before its definition, and always for a spilled value. */
	uint32_t *value_grf;

	/* The index of the last instruction reading a value, widened for loops. */
	uint32_t *last_use;

	/* The index of the instruction that first defines a value; COMPILE_NO_INDEX for none. */
	uint32_t *def_index;

	/* Per value: its scratch slot plus one when it lives in scratch memory, zero when in a register. */
	uint32_t *spill_slot;

	/* How many values live in scratch memory: the slots given out. */
	uint32_t spill_count;

	/* Vertex: nonzero when the VUE is gathered at the end instead of staged in registers throughout. */
	int late_vue;

	/*
	 * Nonzero once an attempt found no register for a value or a
	 * temporary; `victim` is then the value chosen to live in scratch
	 * memory from the next attempt on (COMPILE_NO_VALUE when none can).
	 */
	int out_of_registers;
	uint32_t victim;

	/* The header register of the scratch messages; COMPILE_NO_GRF for a shader that spills nothing. */
	uint32_t header_grf;

	/* This instruction: the spilled values read back and the temporaries they were read into. */
	uint32_t fill_value[COMPILE_MAX_FILLS];
	uint32_t fill_grf[COMPILE_MAX_FILLS];
	uint32_t fill_count;

	/* This instruction: the spilled values it defines and the temporaries that hold them until written out. */
	uint32_t spill_value[COMPILE_MAX_SPILLS];
	uint32_t spill_grf[COMPILE_MAX_SPILLS];
	uint32_t spill_pending;

	/* Vertex, gathered VUE: the value each component of VUE slot s last stored, at 4 s + component. */
	uint32_t output_value[4U * (2U + COMPILE_MAX_VARYINGS)];

	uint8_t grf_busy[COMPILE_MAX_GRF + 1U];

	/* The instruction being lowered. */
	uint32_t index;

	/* One past the highest value register ever used. */
	uint32_t grf_high;

	/* The temporary of the interpolation, and the registers values are given. */
	uint32_t scratch_grf;
	uint32_t first_value_grf;
	uint32_t last_value_grf;

	/* Fragment: nonzero when the shader discards, so f1.0 holds the live pixels. */
	int uses_kill;

	/* The colour locations a fragment shader stores (bit n for location n). */
	uint32_t fs_outputs;

	/* Fragment: nonzero when it stores a second colour (Location 0 Index 1), written dual source. */
	int uses_second_color;

	/*
	 * Fragment: nonzero when the kernel reads an input without perspective,
	 * gl_FragCoord.z and gl_FragCoord.w, so the payload carries the linear
	 * barycentrics, the source depth and the source w; the registers they
	 * land in, and the register the push data and the planes start at after
	 * them.
	 */
	int uses_linear;
	int uses_depth;
	int uses_w;
	uint32_t fs_linear_grf;
	uint32_t fs_depth_grf;
	uint32_t fs_w_grf;
	uint32_t fs_setup_grf;

	/* Vertex: nonzero when the shader writes the point size into its VUE header. */
	int writes_point_size;

	int error;

	/* The IR asks for something this compiler cannot lower. */
	int unsupported;

	/* The input locations, ascending. */
	uint32_t inputs[COMPILE_MAX_INPUTS];
	uint32_t input_count;

	/* Vertex: the output locations other than Position, ascending. */
	uint32_t varyings[COMPILE_MAX_VARYINGS];
	uint32_t varying_count;

	/* Vertex: the first register of the staged VUE (its header). */
	uint32_t vue_grf;

	/* Registers of push data: the push constants, then the uniform blocks. */
	uint32_t push_regs;
	uint32_t push_constant_regs;

	/* Per uniform of the IR: where its block starts in the push data, and the first byte pushed. */
	uint32_t block_push_offset[I915_SHADER_MAX_BLOCKS];
	uint32_t block_first_byte[I915_SHADER_MAX_BLOCKS];
	uint32_t block_uniform[I915_SHADER_MAX_BLOCKS];
	uint32_t block_bytes[I915_SHADER_MAX_BLOCKS];
	uint32_t block_count;

	/* Nonzero for a block that is a storage buffer's address (see struct i915_shader_block). */
	uint8_t block_address[I915_SHADER_MAX_BLOCKS];

	/* The instruction position of each loop being lowered, innermost last. */
	uint32_t loop_tops[COMPILE_MAX_LOOPS];
	uint32_t loop_depth;

	/*
	 * The ENDIFs of guarded texture messages inside loops whose WHILE is
	 * not placed yet: the position of each and the loop depth it is at.
	 * The WHILE of that depth points their JIP at itself, and they leave
	 * the list; the ENDIFs outside every loop keep the next instruction.
	 */
	uint32_t endif_position[COMPILE_MAX_GUARDS];
	uint32_t endif_depth[COMPILE_MAX_GUARDS];
	uint32_t endif_count;

	/* How many texture messages were guarded; past COMPILE_MAX_GUARDS they are not. */
	uint32_t guards;

	/*
	 * Per IR instruction: nonzero for a SKIP_BEGIN whose region is really
	 * skipped (i915_compile_skips()).  Filled once before the attempts, in
	 * the maps' allocation; `skip_taint` and `skip_def` are its scratch (per
	 * value: made inside the region being checked, or from such a value; and
	 * the instruction that defines the value, plus one).
	 */
	uint32_t *skip_ok;
	uint32_t *skip_taint;
	uint32_t *skip_def;

	/*
	 * The region check's truth tables: per value, the channels it may be
	 * garbage on (COMPILE_SKIP_WORDS words each, allocated for the check
	 * only), and the atoms the tables are over.
	 */
	uint64_t *skip_garbage;
	uint32_t skip_atoms[COMPILE_SKIP_ATOMS];
	uint32_t skip_atom_count;

	/* While a skipped region is lowered: nonzero, and its IF's position. */
	int skip_active;
	uint32_t skip_if;

	/*
	 * The condition value plus one whose "not zero" f0.0 holds for the
	 * SELECT just lowered, zero for none: a SELECT right after one on the
	 * same condition reuses the flag (ws075-p024).
	 */
	uint32_t select_flag;

	/*
	 * Geometry (ws075-p007a): the vertex kernel before it, whose VUE layout
	 * places the per-vertex inputs (NULL when the caller gave none); it
	 * lasts over the attempts.
	 */
	const struct i915_shader_binary *producer;

	/*
	 * Geometry: the first register of the input vertices' URB handles and
	 * the register the push data starts at (the dispatch's first payload
	 * register after the fixed ones); the two registers that count the
	 * vertices emitted and gather the cut bits; and the layout of the output
	 * URB entry -- one vertex and the control data header in 32-byte units,
	 * the whole entry in 64-byte units.
	 */
	uint32_t gs_handle_grf;
	uint32_t gs_push_grf;
	uint32_t gs_count_grf;
	uint32_t gs_cut_grf;
	uint32_t gs_vertex_hwords;
	uint32_t gs_control_hwords;
	uint32_t gs_urb_entry_size;

	/* Geometry: the bits of the control data header, a cut bit to each vertex the shader may emit, or none. */
	uint32_t gs_control_bits;
};

static uint32_t i915_compile_sources(const struct drv_gpu_shader_ir_inst *inst);
static uint32_t i915_compile_operands(const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_guarded_texture(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_skips(struct i915_compile_state *state);
static int i915_compile_skip_region(struct i915_compile_state *state, uint32_t begin, uint32_t end);
static int i915_compile_skip_garbage(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst, uint64_t garbage[COMPILE_SKIP_WORDS]);
static const uint64_t *i915_compile_skip_value_garbage(const struct i915_compile_state *state, uint32_t value);
static int i915_compile_bool_table(struct i915_compile_state *state, uint32_t value, uint64_t table[COMPILE_SKIP_WORDS], uint32_t depth);
static int i915_compile_table_within(const uint64_t table[COMPILE_SKIP_WORDS], const uint64_t within[COMPILE_SKIP_WORDS]);
static void i915_compile_skip_begin(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static uint32_t i915_compile_if_any(struct i915_compile_state *state, uint32_t predicate);
static void i915_compile_skip_end(struct i915_compile_state *state);
static uint32_t i915_compile_source(const struct drv_gpu_shader_ir_inst *inst, uint32_t index);
static uint32_t i915_compile_results(const struct drv_gpu_shader_ir_inst *inst);
static int i915_compile_attempt(struct i915_compile_state *state);
static void i915_compile_reset(struct i915_compile_state *state);
static int i915_compile_liveness(struct i915_compile_state *state);
static void i915_compile_widen_loops(struct i915_compile_state *state, const struct i915_compile_loop *loops, uint32_t loop_count);
static void i915_compile_keep_outputs(struct i915_compile_state *state);
static uint32_t i915_compile_grf(struct i915_compile_state *state, uint32_t value);
static uint32_t i915_compile_define(struct i915_compile_state *state, uint32_t value, uint32_t count);
static uint32_t i915_compile_temporary(struct i915_compile_state *state);
static uint32_t i915_compile_temporaries(struct i915_compile_state *state, uint32_t count);
static void i915_compile_exhausted(struct i915_compile_state *state);
static uint32_t i915_compile_choose_victim(const struct i915_compile_state *state);
static int i915_compile_involves(const struct drv_gpu_shader_ir_inst *inst, uint32_t value);
static uint32_t i915_compile_fill(struct i915_compile_state *state, uint32_t value);
static void i915_compile_scratch_offset(struct i915_compile_state *state, uint32_t value);
static void i915_compile_scratch_header(struct i915_compile_state *state);
static void i915_compile_spill_results(struct i915_compile_state *state);
static void i915_compile_release_temporaries(struct i915_compile_state *state);
static void i915_compile_release(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_instruction(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_load_input(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst, uint32_t payload_inputs);
static void i915_compile_load_push(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst, uint32_t payload_inputs);
static void i915_compile_load_block(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst, uint32_t payload_inputs);
static void i915_compile_storage(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst, uint32_t payload_inputs);
static void i915_compile_store_output(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_arithmetic(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_negate(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_math(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_unary(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_minmax(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_compare(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_logic(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_select(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_kill(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_sample(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static int i915_compile_sampler_index(const struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst, uint32_t *sampler);
static void i915_compile_sample_message(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst, uint32_t sampler);
static void i915_compile_texture(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_integer(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_multiply(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_divide(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_convert(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_integer_compare(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_move(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_derivative(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_half(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_loop_begin(struct i915_compile_state *state);
static void i915_compile_loop_end(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static int i915_compile_rank(const uint32_t *list, uint32_t count, uint32_t location, uint32_t *rank);
static uint32_t i915_compile_flat_mask(const struct i915_compile_state *state);
static int i915_compile_input_flat(const struct i915_compile_state *state, uint32_t location);
static int i915_compile_input_linear(const struct i915_compile_state *state, uint32_t location);
static void i915_compile_note(struct i915_compile_state *state, uint32_t *list, uint32_t *count, uint32_t limit, uint32_t location);
static void i915_compile_interface(struct i915_compile_state *state);
static void i915_compile_fragment_payload(struct i915_compile_state *state);
static void i915_compile_frag_coord(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_blocks(struct i915_compile_state *state);
static void i915_compile_prologue(struct i915_compile_state *state);
static void i915_compile_terminate(struct i915_compile_state *state);
static uint32_t i915_compile_fs_outputs(const struct i915_compile_state *state);
static void i915_compile_terminate_dual(struct i915_compile_state *state);
static void i915_compile_terminate_vertex(struct i915_compile_state *state);
static void i915_compile_terminate_gathered(struct i915_compile_state *state);
static void i915_compile_gather(struct i915_compile_state *state, uint32_t first, uint32_t count);
static void i915_compile_read_into(struct i915_compile_state *state, uint32_t value, uint32_t grf);
static void i915_compile_describe(const struct i915_compile_state *state, struct i915_shader_binary *binary);
static void i915_compile_compute_interface(struct i915_compile_state *state);
static void i915_compile_load_system(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst, uint32_t payload_inputs);
static void i915_compile_atomic(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst, uint32_t payload_inputs);
static void i915_compile_storage_size(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst, uint32_t payload_inputs);
static int i915_compile_storage_block(struct i915_compile_state *state, uint32_t uniform, uint32_t *block);
static void i915_compile_terminate_compute(struct i915_compile_state *state);
static void i915_compile_describe_compute(const struct i915_compile_state *state, struct i915_shader_binary *binary);
static void i915_compile_shared(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_atomic_shared(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_fence(struct i915_compile_state *state, uint32_t fences);
static void i915_compile_barrier(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static int i915_compile_group_threads(const struct i915_compile_state *state);
static void i915_compile_geometry_interface(struct i915_compile_state *state);
static void i915_compile_geometry_prologue(struct i915_compile_state *state);
static void i915_compile_load_vertex_input(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static int i915_compile_vertex_slot(const struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst, uint32_t *slot, uint32_t *component);
static void i915_compile_geometry_system(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_terminate_geometry(struct i915_compile_state *state);
static void i915_compile_describe_geometry(const struct i915_compile_state *state, struct i915_shader_binary *binary);
static void i915_compile_emit_vertex(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_end_primitive(struct i915_compile_state *state, const struct drv_gpu_shader_ir_inst *inst);
static void i915_compile_flush_cut_bits(struct i915_compile_state *state, uint32_t live_grf);
static void i915_compile_control_write(struct i915_compile_state *state, int predicated);
static uint32_t i915_compile_urb_write_descriptor(uint32_t global, uint32_t header_regs, int per_slot, int channel_mask);

/*
 * Compiles one shader IR into a Gen12 EU binary, without a stage before it.
 *
 * As drv_i915_shader_compile_stage() with no producer: a geometry shader
 * compiled this way may read gl_in's built-ins only.
 */
int
drv_i915_shader_compile(
	const struct drv_gpu_shader_ir *ir,
	struct i915_shader_binary **out)
{
	int error;

	/* Compiles the stage on its own. */
	error = drv_i915_shader_compile_stage(ir, NULL, out);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the binary. */
	return 0;
}

/*
 * Compiles one shader IR into a Gen12 EU binary.
 *
 * `producer` is the binary of the stage before a geometry shader, the
 * vertex kernel whose VUE the geometry shader reads its per-vertex inputs
 * from; NULL for any other stage.  Returns 0 and the binary in `*out`,
 * ENOTSUP for IR this compiler cannot lower (never approximated), EINVAL
 * for inconsistent IR or an encoder failure, or ENOMEM.  On a failure
 * `*out` is NULL.
 */
int
drv_i915_shader_compile_stage(
	const struct drv_gpu_shader_ir *ir,
	const struct i915_shader_binary *producer,
	struct i915_shader_binary **out)
{
	struct i915_compile_state state;
	struct i915_shader_binary *binary;
	const uint32_t *words;
	uint32_t attempts;
	size_t map_entries;
	size_t code_size;
	size_t bytes;
	int error;

	/* The caller receives nothing unless the whole shader lowers. */
	*out = NULL;

	/*
	 * A stage past the known ones is inconsistent IR: every choice below by
	 * the stage names the vertex, compute and geometry stages and takes any
	 * other for the fragment stage.
	 */
	if (ir->stage >= DRV_GPU_STAGE_COUNT)
		return EINVAL;

	/* Only a geometry shader reads a stage before it, and that stage is a vertex shader. */
	if (producer != NULL && ir->stage != DRV_GPU_STAGE_GEOMETRY)
		return EINVAL;
	if (producer != NULL && producer->stage != DRV_GPU_STAGE_VERTEX)
		return EINVAL;

	/* Allocates the binary and records what is known before lowering. */
	binary = kern_calloc(1U, sizeof(*binary));
	if (binary == NULL)
		return ENOMEM;
	binary->stage = ir->stage;
	binary->simd = COMPILE_SIMD;

	/* Prepares what lasts over the attempts: every value in a register, the VUE staged. */
	kern_memset(&state, 0, sizeof(state));
	state.ir = ir;
	state.producer = producer;
	drv_i915_eu_init(&state.code);

	/*
	 * The register, last-use and definition maps, the scratch slots and the
	 * skippable regions' marks and scratch share one allocation.
	 */
	map_entries = 6U * (size_t)ir->value_count + (size_t)ir->instruction_count;
	if (map_entries == 0U)
		map_entries = 4U;

	/* The maps start every value without a register and outside scratch memory. */
	state.value_grf = kern_calloc(map_entries, sizeof(uint32_t));
	if (state.value_grf == NULL) {
		kern_free(binary);
		return ENOMEM;
	}

	/* Places the liveness lists after the contiguous value-register table. */
	state.last_use = state.value_grf + ir->value_count;
	state.def_index = state.last_use + ir->value_count;
	state.spill_slot = state.def_index + ir->value_count;
	state.skip_taint = state.spill_slot + ir->value_count;
	state.skip_def = state.skip_taint + ir->value_count;
	state.skip_ok = state.skip_def + ir->value_count;

	/* Decides once which skippable regions are really skipped. */
	i915_compile_skips(&state);

	/*
	 * Lowers the shader until its values fit the registers: a vertex shader
	 * that runs out first gathers its VUE at the end, then every attempt
	 * that runs out moves the value it chose to scratch memory.
	 */
	attempts = 0U;
	for (;;) {
		/* Lowers the whole shader once. */
		error = i915_compile_attempt(&state);
		if (error != 0) {
			drv_i915_eu_free(&state.code);
			kern_free(state.value_grf);
			kern_free(binary);
			return error;
		}

		/* The attempt fitted, or failed for a reason more registers do not change. */
		if (state.out_of_registers == 0)
			break;
		if (state.unsupported != 0)
			break;
		if (state.error != 0)
			break;
		if (state.code.error != 0)
			break;

		/* A vertex shader first gives its staged VUE's registers to the values. */
		if (ir->stage == DRV_GPU_STAGE_VERTEX && state.late_vue == 0) {
			state.late_vue = 1;
			continue;
		}

		/* A shader with no value left to move, or that has moved every one, is refused. */
		attempts++;
		if (state.victim == COMPILE_NO_VALUE || attempts > ir->value_count) {
			state.unsupported = 1;
			break;
		}

		/* The chosen value lives in scratch memory from the next attempt on. */
		state.spill_count++;
		state.spill_slot[state.victim] = state.spill_count;
	}

	/* A shader that needs something this compiler cannot lower is refused, never approximated. */
	if (state.unsupported != 0) {
		drv_i915_eu_free(&state.code);
		kern_free(state.value_grf);
		kern_free(binary);
		return ENOTSUP;
	}

	/* An encoder or allocator failure abandons the whole shader. */
	if (state.error != 0 || state.code.error != 0) {
		drv_i915_eu_free(&state.code);
		kern_free(state.value_grf);
		kern_free(binary);
		return EINVAL;
	}

	/* Reads the encoded words; an empty kernel still gets one byte of storage. */
	words = drv_i915_eu_data(&state.code, &bytes);
	if (bytes == 0U) {
		code_size = 1U;
	} else {
		code_size = bytes;
	}

	/* The encoded words are copied into the binary the caller owns. */
	binary->code = kern_calloc(code_size, 1U);
	if (binary->code == NULL) {
		drv_i915_eu_free(&state.code);
		kern_free(state.value_grf);
		kern_free(binary);
		return ENOMEM;
	}

	/* Copies the completed EU instruction stream into its owned shader binary. */
	kern_memcpy(binary->code, words, bytes);
	binary->code_bytes = (uint32_t)bytes;

	/* Records what a draw has to program around the kernel. */
	i915_compile_describe(&state, binary);

	/* The encoder buffer and the value maps are only needed while lowering. */
	drv_i915_eu_free(&state.code);
	kern_free(state.value_grf);

	/* Succeeded: the caller can place and run this shader. */
	*out = binary;
	return 0;
}

/*
 * Releases a compiled shader binary and its EU code.
 */
void
drv_i915_shader_binary_free(
	struct i915_shader_binary *binary)
{
	/* Nothing was compiled. */
	if (binary == NULL)
		return;

	/* Releases the code, then the binary. */
	if (binary->code != NULL)
		kern_free(binary->code);
	kern_free(binary);
}

/*
 * Lowers the whole shader once, with the values given to scratch memory so
 * far, into an empty encoder buffer.
 *
 * Returns 0 with the outcome in the state -- out_of_registers (and the
 * victim), unsupported or error -- or the liveness pass's EINVAL, ENOTSUP or
 * ENOMEM.  An attempt that runs out of registers stops at the instruction
 * that found none.
 */
static int
i915_compile_attempt(
	struct i915_compile_state *state)
{
	const struct drv_gpu_shader_ir *ir;
	uint32_t index;
	int error;

	/* Starts from nothing lowered and no register given out. */
	ir = state->ir;
	i915_compile_reset(state);

	/* Finds where each value is made and last read, the loops taken into account. */
	error = i915_compile_liveness(state);
	if (error != 0)
		return error;

	/* What the shader reads and writes fixes where its payload and its outputs are. */
	i915_compile_interface(state);
	if (state->out_of_registers != 0)
		return 0;
	i915_compile_prologue(state);

	/* Each IR instruction lowers to a short EU sequence in order; spilled results are written out after it. */
	for (index = 0U; index < ir->instruction_count; index++) {
		state->index = index;

		/* Only a SELECT's own compare writes f0.0 between two SELECTs; anything else may write it. */
		if (ir->instructions[index].op != DRV_GPU_IR_SELECT)
			state->select_flag = 0U;
		i915_compile_instruction(state, &ir->instructions[index]);
		i915_compile_spill_results(state);
		i915_compile_release(state, &ir->instructions[index]);
		i915_compile_release_temporaries(state);

		/* An attempt that ran out of registers ends here; the next one spills the victim. */
		if (state->out_of_registers != 0)
			return 0;
	}

	/* A loop left open has no WHILE to end it. */
	if (state->loop_depth != 0U)
		state->error = 1;

	/* The shader ends by writing its output and retiring the thread. */
	i915_compile_terminate(state);

	/* Succeeded: the outcome of the attempt is in the state. */
	return 0;
}

/*
 * Clears the lowering state for a new attempt, keeping the IR, the maps,
 * the values given to scratch memory and the choice of a gathered VUE.
 */
static void
i915_compile_reset(
	struct i915_compile_state *state)
{
	const struct drv_gpu_shader_ir *ir;
	const struct i915_shader_binary *producer;
	uint32_t *value_grf;
	uint32_t *spill_slot;
	uint32_t *skip_ok;
	uint32_t spill_count;
	uint32_t value;
	int late_vue;

	/* Remembers what lasts over the attempts. */
	ir = state->ir;
	producer = state->producer;
	value_grf = state->value_grf;
	spill_slot = state->spill_slot;
	spill_count = state->spill_count;
	late_vue = state->late_vue;
	skip_ok = state->skip_ok;

	/* Drops the previous attempt's code and clears everything else. */
	drv_i915_eu_free(&state->code);
	kern_memset(state, 0, sizeof(*state));
	drv_i915_eu_init(&state->code);

	/* Puts back what lasts. */
	state->ir = ir;
	state->producer = producer;
	state->value_grf = value_grf;
	state->last_use = value_grf + ir->value_count;
	state->def_index = state->last_use + ir->value_count;
	state->spill_slot = spill_slot;
	state->spill_count = spill_count;
	state->late_vue = late_vue;
	state->skip_ok = skip_ok;
	state->skip_taint = spill_slot + ir->value_count;
	state->skip_def = state->skip_taint + ir->value_count;

	/* No value has a register or a reader yet. */
	for (value = 0U; value < ir->value_count; value++) {
		state->value_grf[value] = COMPILE_NO_GRF;
		state->last_use[value] = 0U;
	}

	/* The registers start where the conventions put them; the interface may move the values up. */
	state->fs_setup_grf = COMPILE_FS_SETUP_GRF;
	state->scratch_grf = COMPILE_SCRATCH_GRF;
	state->first_value_grf = COMPILE_FIRST_VALUE_GRF;
	state->last_value_grf = COMPILE_LAST_VALUE_GRF;
	state->grf_high = COMPILE_FIRST_VALUE_GRF;
	state->header_grf = COMPILE_NO_GRF;
	state->victim = COMPILE_NO_VALUE;

	/* No output component of a gathered VUE has been stored. */
	for (value = 0U; value < 4U * (2U + COMPILE_MAX_VARYINGS); value++)
		state->output_value[value] = COMPILE_NO_VALUE;
}

/*
 * Returns how many values an instruction reads: its operands, and the guard
 * of a guarded texture message after them (see i915_compile_source()).
 */
static uint32_t
i915_compile_sources(
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t operands;

	/* The operands the operation names. */
	operands = i915_compile_operands(inst);

	/* A guard is read last. */
	if (inst->guard != 0U)
		return operands + 1U;

	/* Succeeded: only the operands. */
	return operands;
}

/* Returns how many of an instruction's src[] name values. */
static uint32_t
i915_compile_operands(
	const struct drv_gpu_shader_ir_inst *inst)
{
	/* The operation decides how many sources it reads. */
	switch (inst->op) {
	case DRV_GPU_IR_STORE_OUTPUT:
	case DRV_GPU_IR_FNEG:
	case DRV_GPU_IR_RSQ:
	case DRV_GPU_IR_SIN:
	case DRV_GPU_IR_COS:
	case DRV_GPU_IR_RCP:
	case DRV_GPU_IR_SQRT:
	case DRV_GPU_IR_EXP2:
	case DRV_GPU_IR_LOG2:
	case DRV_GPU_IR_FABS:
	case DRV_GPU_IR_FLOOR:
	case DRV_GPU_IR_FRACT:
	case DRV_GPU_IR_NOT:
	case DRV_GPU_IR_KILL:
	case DRV_GPU_IR_FTRUNC:
	case DRV_GPU_IR_FROUND_EVEN:
	case DRV_GPU_IR_INEG:
	case DRV_GPU_IR_INOT:
	case DRV_GPU_IR_I2F:
	case DRV_GPU_IR_U2F:
	case DRV_GPU_IR_F2I:
	case DRV_GPU_IR_F2U:
	case DRV_GPU_IR_MOVE:
	case DRV_GPU_IR_LOOP_END:
	case DRV_GPU_IR_DDX:
	case DRV_GPU_IR_DDX_FINE:
	case DRV_GPU_IR_DDY:
	case DRV_GPU_IR_DDY_FINE:
	case DRV_GPU_IR_UNPACK_HALF:
	case DRV_GPU_IR_SKIP_BEGIN:
		return 1U;

	case DRV_GPU_IR_LOAD_STORAGE:
	case DRV_GPU_IR_LOAD_SHARED:
		/* The offset, and the predicate of a predicated load (ws101-p002, ws101-p006). */
		return 1U + inst->component;

	case DRV_GPU_IR_ATOMIC:
		/* The offset and the value, the comparator of a compare and exchange, then a predicate (ws101-p002). */
		if (inst->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG)
			return 3U + inst->component;
		return 2U + inst->component;

	case DRV_GPU_IR_FADD:
	case DRV_GPU_IR_FSUB:
	case DRV_GPU_IR_FMUL:
	case DRV_GPU_IR_SAMPLE:
	case DRV_GPU_IR_FMIN:
	case DRV_GPU_IR_FMAX:
	case DRV_GPU_IR_FLT:
	case DRV_GPU_IR_FGE:
	case DRV_GPU_IR_FEQ:
	case DRV_GPU_IR_FNEU:
	case DRV_GPU_IR_AND:
	case DRV_GPU_IR_OR:
	case DRV_GPU_IR_IADD:
	case DRV_GPU_IR_ISUB:
	case DRV_GPU_IR_IMUL:
	case DRV_GPU_IR_UDIV:
	case DRV_GPU_IR_UMOD:
	case DRV_GPU_IR_IDIV:
	case DRV_GPU_IR_IREM:
	case DRV_GPU_IR_IAND:
	case DRV_GPU_IR_IOR:
	case DRV_GPU_IR_IXOR:
	case DRV_GPU_IR_SHL:
	case DRV_GPU_IR_SHR:
	case DRV_GPU_IR_ASR:
	case DRV_GPU_IR_ILT:
	case DRV_GPU_IR_IGE:
	case DRV_GPU_IR_ULT:
	case DRV_GPU_IR_UGE:
	case DRV_GPU_IR_IEQ:
	case DRV_GPU_IR_INE:
	case DRV_GPU_IR_PACK_HALF:
		return 2U;

	case DRV_GPU_IR_SELECT:
	case DRV_GPU_IR_SAMPLE_BIAS:
	case DRV_GPU_IR_SAMPLE_LOD:
		return 3U;

	case DRV_GPU_IR_TEXTURE:
		/* A texture message reads its run of parameters. */
		return inst->src[1];

	case DRV_GPU_IR_STORE_STORAGE:
	case DRV_GPU_IR_STORE_SHARED:
		/* The offset and the word, and the predicate of a predicated store. */
		return 2U + inst->component;

	case DRV_GPU_IR_LOAD_VERTEX_INPUT:
		/* The vertex's number when it is chosen at run time (ws075-p007a). */
		if (inst->immediate == DRV_GPU_IR_VERTEX_DYNAMIC)
			return 1U;
		return 0U;

	case DRV_GPU_IR_EMIT_VERTEX:
	case DRV_GPU_IR_END_PRIMITIVE:
		/* The predicate of an emit or an end under one (ws075-p007a). */
		return inst->component;

	default:
		break;
	}

	/* Constants, loads, loop starts and unknown operations read no value. */
	return 0U;
}

/*
 * Returns the value an instruction reads as its index-th source: src[index],
 * or for a texture message the index-th value of its run; the source after
 * the operands is a guard.
 */
static uint32_t
i915_compile_source(
	const struct drv_gpu_shader_ir_inst *inst,
	uint32_t index)
{
	uint32_t operands;

	/* The source after the operands is the guard, stored plus one. */
	operands = i915_compile_operands(inst);
	if (inst->guard != 0U && index == operands)
		return inst->guard - 1U;

	/* A texture message's parameters are consecutive values from src[0]. */
	if (inst->op == DRV_GPU_IR_TEXTURE)
		return inst->src[0] + index;

	/* Succeeded: any other instruction names each source. */
	return inst->src[index];
}

/* Returns how many consecutive values an instruction defines from its dst. */
static uint32_t
i915_compile_results(
	const struct drv_gpu_shader_ir_inst *inst)
{
	/* A sample defines four; a store, a discard, a loop mark or a no-op none; anything else one. */
	switch (inst->op) {
	case DRV_GPU_IR_SAMPLE:
	case DRV_GPU_IR_SAMPLE_BIAS:
	case DRV_GPU_IR_SAMPLE_LOD:
	case DRV_GPU_IR_TEXTURE:
		return 4U;

	case DRV_GPU_IR_STORE_OUTPUT:
	case DRV_GPU_IR_STORE_STORAGE:
	case DRV_GPU_IR_STORE_SHARED:
	case DRV_GPU_IR_BARRIER:
	case DRV_GPU_IR_FENCE:
	case DRV_GPU_IR_KILL:
	case DRV_GPU_IR_NOP:
	case DRV_GPU_IR_LOOP_BEGIN:
	case DRV_GPU_IR_LOOP_END:
	case DRV_GPU_IR_SKIP_BEGIN:
	case DRV_GPU_IR_SKIP_END:
	case DRV_GPU_IR_EMIT_VERTEX:
	case DRV_GPU_IR_END_PRIMITIVE:
		return 0U;

	default:
		break;
	}

	/* Succeeded: one value. */
	return 1U;
}

/*
 * Finds, for every value, the instruction that first defines it and the
 * last one that reads it, and pairs the loop marks.
 *
 * A MOVE into a value defined before counts as a read of it as well: the
 * register must hold the loop variable until the move rewrites it.  Every
 * loop then widens the lives of the values made before it and read in it to
 * its end.  Returns EINVAL for loop marks that do not pair, ENOTSUP for
 * loops nested deeper than the code generator follows, ENOMEM.
 */
static int
i915_compile_liveness(
	struct i915_compile_state *state)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *inst;
	struct i915_compile_loop *loops;
	uint32_t open[COMPILE_MAX_LOOPS];
	uint32_t open_count;
	uint32_t loop_count;
	uint32_t index;
	uint32_t source;
	uint32_t source_count;
	uint32_t result;
	uint32_t result_count;
	uint32_t value;

	/* Starts with no value defined. */
	ir = state->ir;
	for (value = 0U; value < ir->value_count; value++)
		state->def_index[value] = COMPILE_NO_INDEX;

	/* Counts the loops, for the list of their extents. */
	loop_count = 0U;
	for (index = 0U; index < ir->instruction_count; index++) {
		if (ir->instructions[index].op == DRV_GPU_IR_LOOP_END)
			loop_count++;
	}

	/* Allocates the list; a shader without loops still gets one entry. */
	loops = kern_calloc(loop_count + 1U, sizeof(*loops));
	if (loops == NULL)
		return ENOMEM;

	/* Notes the readers, the definitions and the loop extents, in order. */
	open_count = 0U;
	loop_count = 0U;
	for (index = 0U; index < ir->instruction_count; index++) {
		inst = &ir->instructions[index];

		/* Notes this instruction as the latest reader of each value it reads. */
		source_count = i915_compile_sources(inst);
		for (source = 0U; source < source_count; source++) {
			value = i915_compile_source(inst, source);
			if (value < ir->value_count)
				state->last_use[value] = index;
		}

		/* A MOVE into a value defined before keeps that value alive up to it. */
		if (inst->op == DRV_GPU_IR_MOVE &&
		    inst->dst < ir->value_count &&
		    state->def_index[inst->dst] != COMPILE_NO_INDEX)
			state->last_use[inst->dst] = index;

		/* Notes the first definition of each value the instruction defines. */
		result_count = i915_compile_results(inst);
		for (result = 0U; result < result_count; result++) {
			value = inst->dst + result;
			if (value < ir->value_count && state->def_index[value] == COMPILE_NO_INDEX)
				state->def_index[value] = index;
		}

		/* A loop start opens a loop; more than the code generator follows is refused. */
		if (inst->op == DRV_GPU_IR_LOOP_BEGIN) {
			if (open_count >= COMPILE_MAX_LOOPS) {
				kern_free(loops);
				return ENOTSUP;
			}

			/* Records this loop header in the open-loop stack. */
			open[open_count] = index;
			open_count++;
		}

		/* A loop end closes the innermost open loop. */
		if (inst->op == DRV_GPU_IR_LOOP_END) {
			if (open_count == 0U) {
				kern_free(loops);
				return EINVAL;
			}

			/* Closes the innermost loop and publishes its instruction extent. */
			open_count--;
			loops[loop_count].begin = open[open_count];
			loops[loop_count].end = index;
			loop_count++;
		}
	}

	/* A loop that never ends is inconsistent IR. */
	if (open_count != 0U) {
		kern_free(loops);
		return EINVAL;
	}

	/* Widens the lives the loops read again on every pass. */
	i915_compile_widen_loops(state, loops, loop_count);
	kern_free(loops);

	/* A vertex shader that gathers its VUE at the end keeps what each output component last stored until then. */
	if (state->late_vue != 0)
		i915_compile_keep_outputs(state);

	/* Succeeded: every value has its extent. */
	return 0;
}

/*
 * Widens to the loop's end the life of every value made before a loop and
 * read inside it; the loops come inner first, so a value an inner loop
 * widened is widened again by the loop around it.
 */
static void
i915_compile_widen_loops(
	struct i915_compile_state *state,
	const struct i915_compile_loop *loops,
	uint32_t loop_count)
{
	uint32_t loop;
	uint32_t value;
	uint32_t begin;
	uint32_t end;

	/* Takes each loop, inner loops first. */
	for (loop = 0U; loop < loop_count; loop++) {
		begin = loops[loop].begin;
		end = loops[loop].end;

		/* Widens each value made before the loop whose last reader is inside it. */
		for (value = 0U; value < state->ir->value_count; value++) {
			if (state->def_index[value] == COMPILE_NO_INDEX)
				continue;
			if (state->def_index[value] >= begin)
				continue;
			if (state->last_use[value] > begin && state->last_use[value] < end)
				state->last_use[value] = end;
		}
	}
}

/*
 * Keeps alive to the end of the shader the value each output component
 * last stores, in program order: every channel passes every store (a store
 * a channel skips stores what the component held), so that value is the
 * component's at the end.
 */
static void
i915_compile_keep_outputs(
	struct i915_compile_state *state)
{
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t seen_location[4U * (1U + COMPILE_MAX_VARYINGS)];
	uint32_t seen_component[4U * (1U + COMPILE_MAX_VARYINGS)];
	uint32_t seen_count;
	uint32_t index;
	uint32_t seen;
	int stored_later;

	/* Walks the stores from the last one back, noting each component once. */
	seen_count = 0U;
	for (index = state->ir->instruction_count; index > 0U; index--) {
		inst = &state->ir->instructions[index - 1U];
		if (inst->op != DRV_GPU_IR_STORE_OUTPUT)
			continue;

		/* A component stored again later keeps the later value. */
		stored_later = 0;
		for (seen = 0U; seen < seen_count; seen++) {
			if (seen_location[seen] == inst->location && seen_component[seen] == inst->component)
				stored_later = 1;
		}

		/* Keeps a value live when a later store still needs its previous definition. */
		if (stored_later != 0)
			continue;

		/* More components than a VUE holds are refused by the interface; nothing more is noted. */
		if (seen_count >= 4U * (1U + COMPILE_MAX_VARYINGS))
			return;

		/* Notes the component and keeps its last value alive past every instruction. */
		seen_location[seen_count] = inst->location;
		seen_component[seen_count] = inst->component;
		seen_count++;
		if (inst->src[0] < state->ir->value_count)
			state->last_use[inst->src[0]] = state->ir->instruction_count;
	}
}

/*
 * Returns the register a value lives in; a value in scratch memory is read
 * back into a temporary of the instruction being lowered.  Reading a value
 * that has no definition is an error.
 */
static uint32_t
i915_compile_grf(
	struct i915_compile_state *state,
	uint32_t value)
{
	uint32_t grf;

	/* A value outside the IR has no register. */
	if (value >= state->ir->value_count) {
		state->error = 1;
		return state->first_value_grf;
	}

	/* A value in scratch memory is read back first. */
	if (state->spill_slot[value] != 0U) {
		grf = i915_compile_fill(state, value);
		return grf;
	}

	/* A value read before its definition has no register either. */
	if (state->value_grf[value] == COMPILE_NO_GRF) {
		state->error = 1;
		return state->first_value_grf;
	}

	/* Succeeded: the value lives in this register. */
	return state->value_grf[value];
}

/*
 * Gives `count` consecutive values, starting at `value`, `count` consecutive
 * free registers (a message reply is consecutive registers) and returns the
 * first.  SSA: a value is defined once.  The register of a value that lives
 * in scratch memory is a temporary of the instruction, written out after it
 * (on every definition, so a loop variable's too).
 */
static uint32_t
i915_compile_define(
	struct i915_compile_state *state,
	uint32_t value,
	uint32_t count)
{
	uint32_t grf;
	uint32_t run;

	/* The values must exist in the IR. */
	if (value >= state->ir->value_count || count > state->ir->value_count - value) {
		state->error = 1;
		return state->first_value_grf;
	}

	/* A value defined a second time breaks SSA. */
	for (run = 0U; run < count; run++) {
		if (state->value_grf[value + run] != COMPILE_NO_GRF) {
			state->error = 1;
			return state->first_value_grf;
		}
	}

	/* More spilled results than one instruction defines is inconsistent. */
	if (state->spill_pending + count > COMPILE_MAX_SPILLS) {
		state->error = 1;
		return state->first_value_grf;
	}

	/* Takes the lowest run of `count` free value registers. */
	for (grf = state->first_value_grf; grf + count <= state->last_value_grf + 1U; grf++) {
		/* Measures how many free registers start here. */
		for (run = 0U; run < count && state->grf_busy[grf + run] == 0U; run++)
			;

		/* A run cut short by a busy register is not taken. */
		if (run != count)
			continue;

		/* Marks the run busy and gives each value its register; a spilled one's is written out after the instruction. */
		for (run = 0U; run < count; run++) {
			state->grf_busy[grf + run] = 1U;
			if (state->spill_slot[value + run] != 0U) {
				state->spill_value[state->spill_pending] = value + run;
				state->spill_grf[state->spill_pending] = grf + run;
				state->spill_pending++;
			} else {
				state->value_grf[value + run] = grf + run;
			}
		}

		/* Remembers the highest register the kernel uses. */
		if (grf + count > state->grf_high)
			state->grf_high = grf + count;

		/* Succeeded: the values live from this register on. */
		return grf;
	}

	/* More values live at once than registers: the next attempt spills one. */
	i915_compile_exhausted(state);
	return state->first_value_grf;
}

/*
 * Takes one free value register as a temporary of the instruction being
 * lowered; the caller frees it (grf_busy) before the lowering ends.  None
 * free ends the attempt as a value would.
 */
static uint32_t
i915_compile_temporary(
	struct i915_compile_state *state)
{
	uint32_t grf;

	/* Takes the lowest free value register. */
	for (grf = state->first_value_grf; grf <= state->last_value_grf; grf++) {
		if (state->grf_busy[grf] != 0U)
			continue;

		/* Marks it busy and remembers the highest register the kernel uses. */
		state->grf_busy[grf] = 1U;
		if (grf + 1U > state->grf_high)
			state->grf_high = grf + 1U;

		/* Succeeded: the temporary is this register. */
		return grf;
	}

	/* No register is free: the next attempt spills a value. */
	i915_compile_exhausted(state);
	return state->first_value_grf;
}

/*
 * Takes `count` consecutive free value registers as temporaries of the
 * instruction being lowered (a message payload is consecutive registers) and
 * returns the first; the caller frees them (grf_busy) before the lowering
 * ends.  None free ends the attempt as a value would.
 */
static uint32_t
i915_compile_temporaries(
	struct i915_compile_state *state,
	uint32_t count)
{
	uint32_t grf;
	uint32_t run;

	/* Takes the lowest run of `count` free value registers. */
	for (grf = state->first_value_grf; grf + count <= state->last_value_grf + 1U; grf++) {
		/* Measures how many free registers start here. */
		for (run = 0U; run < count && state->grf_busy[grf + run] == 0U; run++)
			;

		/* A run cut short by a busy register is not taken. */
		if (run != count)
			continue;

		/* Marks the run busy. */
		for (run = 0U; run < count; run++)
			state->grf_busy[grf + run] = 1U;

		/* Remembers the highest register the kernel uses. */
		if (grf + count > state->grf_high)
			state->grf_high = grf + count;

		/* Succeeded: the temporaries start here. */
		return grf;
	}

	/* No run is free: the next attempt spills a value. */
	i915_compile_exhausted(state);
	return state->first_value_grf;
}

/*
 * Ends the attempt for want of a register, choosing at this point the value
 * the next attempt keeps in scratch memory; only the first shortage of an
 * attempt chooses.
 */
static void
i915_compile_exhausted(
	struct i915_compile_state *state)
{
	/* The first shortage already chose. */
	if (state->out_of_registers != 0)
		return;

	/* Notes the shortage and the value whose register frees the most of the rest of the shader. */
	state->out_of_registers = 1;
	state->victim = i915_compile_choose_victim(state);
}

/*
 * Chooses the value to move to scratch memory where the registers ran out:
 * of the values holding a register past this instruction and not read or
 * defined by it, the one whose life ends last (Mesa's allocator weighs its
 * spill costs; the furthest end is the simple heuristic).  Returns
 * COMPILE_NO_VALUE when no value qualifies.
 */
static uint32_t
i915_compile_choose_victim(
	const struct i915_compile_state *state)
{
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t best;
	uint32_t best_end;
	uint32_t value;
	uint32_t grf;
	int involved;

	/* The instruction that ran out: its own values cannot give up their registers to it. */
	inst = &state->ir->instructions[state->index];

	/* Looks over every value that holds a register now. */
	best = COMPILE_NO_VALUE;
	best_end = 0U;
	for (value = 0U; value < state->ir->value_count; value++) {
		/* A value already in scratch memory, or never given a register, has none to give. */
		if (state->spill_slot[value] != 0U)
			continue;
		grf = state->value_grf[value];
		if (grf == COMPILE_NO_GRF)
			continue;

		/* A register freed since, or a value that dies here, frees nothing further on. */
		if (state->grf_busy[grf] == 0U)
			continue;
		if (state->last_use[value] <= state->index)
			continue;

		/* The instruction's sources and results stay in registers. */
		involved = i915_compile_involves(inst, value);
		if (involved != 0)
			continue;

		/* Keeps the value that lives longest. */
		if (state->last_use[value] > best_end) {
			best = value;
			best_end = state->last_use[value];
		}
	}

	/* Succeeded: the victim, or none. */
	return best;
}

/* Returns nonzero when an instruction reads or defines a value. */
static int
i915_compile_involves(
	const struct drv_gpu_shader_ir_inst *inst,
	uint32_t value)
{
	uint32_t source;
	uint32_t sources;
	uint32_t results;
	uint32_t read;

	/* One of its sources. */
	sources = i915_compile_sources(inst);
	for (source = 0U; source < sources; source++) {
		read = i915_compile_source(inst, source);
		if (read == value)
			return 1;
	}

	/* One of the values it defines from its destination on. */
	results = i915_compile_results(inst);
	if (results != 0U && value >= inst->dst && value - inst->dst < results)
		return 1;

	/* Neither. */
	return 0;
}

/*
 * Reads a spilled value back from scratch memory into a temporary of the
 * instruction being lowered and returns the temporary; a value the
 * instruction reads twice is read once.  All eight channels are read,
 * outside the channel mask, as Mesa's emit_unspill() does.
 */
static uint32_t
i915_compile_fill(
	struct i915_compile_state *state,
	uint32_t value)
{
	uint32_t index;
	uint32_t grf;

	/* A value this instruction already read back is in its temporary. */
	for (index = 0U; index < state->fill_count; index++) {
		if (state->fill_value[index] == value)
			return state->fill_grf[index];
	}

	/* More reads than sources is inconsistent. */
	if (state->fill_count >= COMPILE_MAX_FILLS) {
		state->error = 1;
		return state->first_value_grf;
	}

	/* Takes the temporary; none free ends the attempt. */
	grf = i915_compile_temporary(state);
	if (state->out_of_registers != 0)
		return grf;

	/* Points the header at the value's slot and reads the register back. */
	i915_compile_scratch_offset(state, value);
	drv_i915_eu_send_all(&state->code,
			     drv_i915_eu_grf_ud(grf),
			     drv_i915_eu_grf_ud(state->header_grf),
			     drv_i915_eu_null(),
			     EU_SFID_DATA_CACHE,
			     COMPILE_DESC_SCRATCH_READ,
			     0U);

	/* Remembers the temporary, freed after the instruction. */
	state->fill_value[state->fill_count] = value;
	state->fill_grf[state->fill_count] = grf;
	state->fill_count++;

	/* Succeeded: the value is in the temporary. */
	return grf;
}

/*
 * Writes the offset of a spilled value's slot, in OWords, into dword 2 of
 * the scratch header: a SIMD1 move outside the channel mask (Mesa's
 * build_legacy_scratch_header()).
 */
static void
i915_compile_scratch_offset(
	struct i915_compile_state *state,
	uint32_t value)
{
	struct i915_eu_reg dword;
	uint32_t offset;

	/* A shader that spills has a header; one without is inconsistent. */
	if (state->header_grf == COMPILE_NO_GRF) {
		state->error = 1;
		return;
	}

	/* Slot n is at byte 32 n of the thread's scratch space. */
	offset = (state->spill_slot[value] - 1U) * COMPILE_SLOT_BYTES / COMPILE_OWORD_BYTES;

	/* Moves the offset into dword 2. */
	dword = drv_i915_eu_grf_ud(state->header_grf);
	dword.subnr = 4U * EU_SCRATCH_HEADER_OFFSET_DWORD;
	drv_i915_eu_mov_scalar(&state->code, dword, drv_i915_eu_imm_ud(offset));
}

/*
 * Builds the scratch header once, at the start and outside the channel
 * mask, as Mesa's generate_scratch_header() does before each message: the
 * register cleared, then dword 3 the per-thread scratch space (r0.3 bits
 * 3:0) and dword 5 the thread's scratch base (r0.5 bits 31:10).  Only dword
 * 2 changes afterwards.
 */
static void
i915_compile_scratch_header(
	struct i915_compile_state *state)
{
	struct i915_eu_reg dword;
	struct i915_eu_reg payload;

	/* Clears the header on all eight channels. */
	drv_i915_eu_mov_all(&state->code, drv_i915_eu_grf_ud(state->header_grf), drv_i915_eu_imm_ud(0U));

	/* Copies the per-thread scratch space out of r0.3. */
	dword = drv_i915_eu_grf_ud(state->header_grf);
	dword.subnr = 4U * EU_SCRATCH_HEADER_SIZE_DWORD;
	payload = drv_i915_eu_grf_scalar(0U, 4U * EU_SCRATCH_HEADER_SIZE_DWORD);
	payload.type = COMPILE_TYPE_UD;
	drv_i915_eu_alu2_scalar(&state->code, I915_EU_AND, dword, payload, drv_i915_eu_imm_ud(EU_SCRATCH_SIZE_MASK));

	/* Copies the thread's scratch base out of r0.5. */
	dword = drv_i915_eu_grf_ud(state->header_grf);
	dword.subnr = 4U * EU_SCRATCH_HEADER_BASE_DWORD;
	payload = drv_i915_eu_grf_scalar(0U, 4U * EU_SCRATCH_HEADER_BASE_DWORD);
	payload.type = COMPILE_TYPE_UD;
	drv_i915_eu_alu2_scalar(&state->code, I915_EU_AND, dword, payload, drv_i915_eu_imm_ud(EU_SCRATCH_BASE_MASK));
}

/*
 * Writes out the spilled values the instruction just defined: an OWord
 * block write of each temporary under the execution mask, so the channels
 * the instruction did not run on keep what their slot held (Mesa's
 * emit_spill() of a per-channel destination).  A value nothing reads
 * afterwards is not written.
 */
static void
i915_compile_spill_results(
	struct i915_compile_state *state)
{
	uint32_t index;
	uint32_t value;

	/* Writes each pending result to its slot. */
	for (index = 0U; index < state->spill_pending; index++) {
		value = state->spill_value[index];
		if (state->last_use[value] <= state->index)
			continue;

		/* Points the header at the slot and writes the register. */
		i915_compile_scratch_offset(state, value);
		drv_i915_eu_send(&state->code,
				 drv_i915_eu_null(),
				 drv_i915_eu_grf_ud(state->header_grf),
				 drv_i915_eu_grf_ud(state->spill_grf[index]),
				 EU_SFID_DATA_CACHE,
				 COMPILE_DESC_SCRATCH_WRITE,
				 COMPILE_EX_MLEN(1U),
				 0,
				 0);
	}
}

/* Frees the temporaries the instruction read spilled values into and held spilled results in. */
static void
i915_compile_release_temporaries(
	struct i915_compile_state *state)
{
	uint32_t index;

	/* Frees the temporaries of the values read back. */
	for (index = 0U; index < state->fill_count; index++)
		state->grf_busy[state->fill_grf[index]] = 0U;
	state->fill_count = 0U;

	/* Frees the temporaries of the results written out. */
	for (index = 0U; index < state->spill_pending; index++)
		state->grf_busy[state->spill_grf[index]] = 0U;
	state->spill_pending = 0U;
}

/*
 * Frees the registers of the values whose last reader was this instruction
 * (or that nothing reads); a loop's end also frees the values whose lives
 * the loop widened to it.
 */
static void
i915_compile_release(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t sources;
	uint32_t source;
	uint32_t value;
	uint32_t defined;

	/* Frees each source whose last reader this is. */
	sources = i915_compile_sources(inst);
	for (source = 0U; source < sources; source++) {
		value = i915_compile_source(inst, source);
		if (value < state->ir->value_count &&
		    state->last_use[value] == state->index &&
		    state->value_grf[value] != COMPILE_NO_GRF)
			state->grf_busy[state->value_grf[value]] = 0U;
	}

	/* Frees each defined value that nothing reads afterwards. */
	defined = i915_compile_results(inst);
	for (value = inst->dst;
	     defined != 0U && value < state->ir->value_count;
	     value++, defined--) {
		/* last_use stays 0 unless instruction 0 reads the value. */
		if (state->last_use[value] <= state->index && state->value_grf[value] != COMPILE_NO_GRF)
			state->grf_busy[state->value_grf[value]] = 0U;
	}

	/* Only a loop's end has values of its own to free. */
	if (inst->op != DRV_GPU_IR_LOOP_END)
		return;

	/* Frees every value whose life the loop widened to its end. */
	for (value = 0U; value < state->ir->value_count; value++) {
		if (state->last_use[value] == state->index && state->value_grf[value] != COMPILE_NO_GRF)
			state->grf_busy[state->value_grf[value]] = 0U;
	}
}

/* Lowers one IR instruction to its EU sequence. */
static void
i915_compile_instruction(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t payload_inputs;
	uint32_t dst;

	/*
	 * The inputs follow the fixed payload registers and the push data; a
	 * compute thread's push data starts at r1, a geometry thread's after
	 * the input vertices' handles.
	 */
	if (state->ir->stage == DRV_GPU_STAGE_VERTEX) {
		payload_inputs = COMPILE_PAYLOAD_GRF + state->push_regs;
	} else if (state->ir->stage == DRV_GPU_STAGE_COMPUTE) {
		payload_inputs = COMPILE_CS_PUSH_GRF + state->push_regs;
	} else if (state->ir->stage == DRV_GPU_STAGE_GEOMETRY) {
		payload_inputs = state->gs_push_grf + state->push_regs;
	} else {
		payload_inputs = state->fs_setup_grf + state->push_regs;
	}

	/* Lowers by the operation. */
	switch (inst->op) {
	case DRV_GPU_IR_NOP:
		break;

	case DRV_GPU_IR_CONST:
		/* A constant enters a value register as an immediate. */
		dst = i915_compile_define(state, inst->dst, 1U);
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf(dst), drv_i915_eu_imm_f(inst->immediate));
		break;

	case DRV_GPU_IR_ICONST:
		/* An integer constant enters a value register as an integer immediate, bit for bit. */
		dst = i915_compile_define(state, inst->dst, 1U);
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf_d(dst), drv_i915_eu_imm_d(inst->immediate));
		break;

	case DRV_GPU_IR_LOAD_INPUT:
		/* A geometry shader reads its inputs per vertex, never so. */
		if (state->ir->stage == DRV_GPU_STAGE_GEOMETRY) {
			state->unsupported = 1;
			break;
		}

		/* Loads the remaining vertex inputs from their selected payload positions. */
		i915_compile_load_input(state, inst, payload_inputs);
		break;

	case DRV_GPU_IR_LOAD_VERTEX_INPUT:
		i915_compile_load_vertex_input(state, inst);
		break;

	case DRV_GPU_IR_EMIT_VERTEX:
		i915_compile_emit_vertex(state, inst);
		break;

	case DRV_GPU_IR_END_PRIMITIVE:
		i915_compile_end_primitive(state, inst);
		break;

	case DRV_GPU_IR_LOAD_PUSH:
		i915_compile_load_push(state, inst, payload_inputs);
		break;

	case DRV_GPU_IR_LOAD_UBO:
		i915_compile_load_block(state, inst, payload_inputs);
		break;

	case DRV_GPU_IR_LOAD_STORAGE:
	case DRV_GPU_IR_STORE_STORAGE:
		i915_compile_storage(state, inst, payload_inputs);
		break;

	case DRV_GPU_IR_LOAD_SYSTEM:
		/* A geometry thread's built-in is in its own payload; a compute thread's in its. */
		if (state->ir->stage == DRV_GPU_STAGE_GEOMETRY) {
			i915_compile_geometry_system(state, inst);
			break;
		}

		/* Loads the remaining invocation built-ins from their compute payload positions. */
		i915_compile_load_system(state, inst, payload_inputs);
		break;

	case DRV_GPU_IR_ATOMIC:
		i915_compile_atomic(state, inst, payload_inputs);
		break;

	case DRV_GPU_IR_STORAGE_SIZE:
		i915_compile_storage_size(state, inst, payload_inputs);
		break;

	case DRV_GPU_IR_LOAD_SHARED:
	case DRV_GPU_IR_STORE_SHARED:
		i915_compile_shared(state, inst);
		break;

	case DRV_GPU_IR_FENCE:
		i915_compile_fence(state, inst->immediate);
		break;

	case DRV_GPU_IR_BARRIER:
		i915_compile_barrier(state, inst);
		break;

	case DRV_GPU_IR_STORE_OUTPUT:
		i915_compile_store_output(state, inst);
		break;

	case DRV_GPU_IR_FADD:
	case DRV_GPU_IR_FSUB:
	case DRV_GPU_IR_FMUL:
		i915_compile_arithmetic(state, inst);
		break;

	case DRV_GPU_IR_FNEG:
		i915_compile_negate(state, inst);
		break;

	case DRV_GPU_IR_SIN:
	case DRV_GPU_IR_COS:
	case DRV_GPU_IR_RSQ:
	case DRV_GPU_IR_RCP:
	case DRV_GPU_IR_SQRT:
	case DRV_GPU_IR_EXP2:
	case DRV_GPU_IR_LOG2:
		i915_compile_math(state, inst);
		break;

	case DRV_GPU_IR_FABS:
	case DRV_GPU_IR_FLOOR:
	case DRV_GPU_IR_FRACT:
	case DRV_GPU_IR_FTRUNC:
	case DRV_GPU_IR_FROUND_EVEN:
		i915_compile_unary(state, inst);
		break;

	case DRV_GPU_IR_FMIN:
	case DRV_GPU_IR_FMAX:
		i915_compile_minmax(state, inst);
		break;

	case DRV_GPU_IR_FLT:
	case DRV_GPU_IR_FGE:
	case DRV_GPU_IR_FEQ:
	case DRV_GPU_IR_FNEU:
		i915_compile_compare(state, inst);
		break;

	case DRV_GPU_IR_AND:
	case DRV_GPU_IR_OR:
	case DRV_GPU_IR_NOT:
		i915_compile_logic(state, inst);
		break;

	case DRV_GPU_IR_BOOL:
		/* A Boolean enters a value register as an integer immediate, bit for bit. */
		dst = i915_compile_define(state, inst->dst, 1U);
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf_d(dst), drv_i915_eu_imm_d(inst->immediate));
		break;

	case DRV_GPU_IR_SELECT:
		i915_compile_select(state, inst);
		break;

	case DRV_GPU_IR_KILL:
		i915_compile_kill(state, inst);
		break;

	case DRV_GPU_IR_SAMPLE:
	case DRV_GPU_IR_SAMPLE_BIAS:
	case DRV_GPU_IR_SAMPLE_LOD:
	case DRV_GPU_IR_TEXTURE:
		i915_compile_guarded_texture(state, inst);
		break;

	case DRV_GPU_IR_IADD:
	case DRV_GPU_IR_ISUB:
	case DRV_GPU_IR_INEG:
	case DRV_GPU_IR_IAND:
	case DRV_GPU_IR_IOR:
	case DRV_GPU_IR_IXOR:
	case DRV_GPU_IR_INOT:
	case DRV_GPU_IR_SHL:
	case DRV_GPU_IR_SHR:
	case DRV_GPU_IR_ASR:
		i915_compile_integer(state, inst);
		break;

	case DRV_GPU_IR_IMUL:
		i915_compile_multiply(state, inst);
		break;

	case DRV_GPU_IR_UDIV:
	case DRV_GPU_IR_UMOD:
	case DRV_GPU_IR_IDIV:
	case DRV_GPU_IR_IREM:
		i915_compile_divide(state, inst);
		break;

	case DRV_GPU_IR_I2F:
	case DRV_GPU_IR_U2F:
	case DRV_GPU_IR_F2I:
	case DRV_GPU_IR_F2U:
		i915_compile_convert(state, inst);
		break;

	case DRV_GPU_IR_ILT:
	case DRV_GPU_IR_IGE:
	case DRV_GPU_IR_ULT:
	case DRV_GPU_IR_UGE:
	case DRV_GPU_IR_IEQ:
	case DRV_GPU_IR_INE:
		i915_compile_integer_compare(state, inst);
		break;

	case DRV_GPU_IR_MOVE:
		i915_compile_move(state, inst);
		break;

	case DRV_GPU_IR_LOOP_BEGIN:
		i915_compile_loop_begin(state);
		break;

	case DRV_GPU_IR_SKIP_BEGIN:
		i915_compile_skip_begin(state, inst);
		break;

	case DRV_GPU_IR_SKIP_END:
		i915_compile_skip_end(state);
		break;

	case DRV_GPU_IR_LOOP_END:
		i915_compile_loop_end(state, inst);
		break;

	case DRV_GPU_IR_DDX:
	case DRV_GPU_IR_DDX_FINE:
	case DRV_GPU_IR_DDY:
	case DRV_GPU_IR_DDY_FINE:
		i915_compile_derivative(state, inst);
		break;

	case DRV_GPU_IR_PACK_HALF:
	case DRV_GPU_IR_UNPACK_HALF:
		i915_compile_half(state, inst);
		break;

	default:
		/* An IR operation this compiler does not know is never dropped. */
		state->unsupported = 1;
		break;
	}
}

/* Lowers an input load: a payload register, or an interpolation over the input's plane. */
static void
i915_compile_load_input(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst,
	uint32_t payload_inputs)
{
	struct i915_eu_buf *code;
	uint32_t rank;
	uint32_t plane;
	uint32_t first;
	uint32_t dst;
	struct i915_eu_reg facing;
	uint32_t bary1;
	uint32_t bary2;
	int found;
	int flat;
	int linear;

	/* Emits into the shader's encoder buffer. */
	code = &state->code;

	/* A component beyond w is not lowered. */
	if (inst->component > 3U) {
		state->unsupported = 1;
		return;
	}

	/*
	 * gl_FrontFacing: bit 15 of the payload's r1.1 word (bit 31 of r1.0) is
	 * set for a back face; its sign spread over the channel, inverted, is the
	 * all-ones or zero Boolean (brw's Gen12 lowering).
	 */
	if (state->ir->stage == DRV_GPU_STAGE_FRAGMENT && inst->location == DRV_GPU_SHADER_LOCATION_FRONT_FACING) {
		dst = i915_compile_define(state, inst->dst, 1U);
		facing = drv_i915_eu_grf_scalar(COMPILE_FS_DISPATCH_GRF, 0U);
		facing.type = drv_i915_eu_grf_d(COMPILE_FS_DISPATCH_GRF).type;
		drv_i915_eu_alu2(code, I915_EU_ASR, drv_i915_eu_grf_d(dst), facing, drv_i915_eu_imm_d(31U));
		drv_i915_eu_alu1(code, I915_EU_NOT, drv_i915_eu_grf_d(dst), drv_i915_eu_grf_d(dst));
		return;
	}

	/* gl_FragCoord comes from the payload's pixel position, depth and w. */
	if (state->ir->stage == DRV_GPU_STAGE_FRAGMENT && inst->location == DRV_GPU_SHADER_LOCATION_FRAG_COORD) {
		i915_compile_frag_coord(state, inst);
		return;
	}

	/* Finds the input's place in the payload order. */
	found = i915_compile_rank(state->inputs, state->input_count, inst->location, &rank);
	if (found != 0) {
		state->unsupported = 1;
		return;
	}

	/* Gives the loaded value its register. */
	dst = i915_compile_define(state, inst->dst, 1U);

	/* An attribute component of a vertex is a payload register of its own. */
	if (state->ir->stage == DRV_GPU_STAGE_VERTEX) {
		drv_i915_eu_mov(code, drv_i915_eu_grf(dst), drv_i915_eu_grf(payload_inputs + 4U * rank + inst->component));
		return;
	}

	/* Locates the component's plane (see the conventions). */
	plane = state->fs_setup_grf + state->push_regs + 2U * rank + inst->component / 2U;
	first = (inst->component & 1U) * 16U;

	/* A Flat input is the plane's origin, the provoking vertex's value, moved bit for bit (an integer stays one). */
	flat = i915_compile_input_flat(state, inst->location);
	if (flat) {
		drv_i915_eu_mov(code, drv_i915_eu_grf(dst), drv_i915_eu_grf_scalar(plane, first + 12U));
		return;
	}

	/* An input without perspective is interpolated with the linear barycentrics. */
	bary1 = COMPILE_FS_BARY1_GRF;
	bary2 = COMPILE_FS_BARY2_GRF;
	linear = i915_compile_input_linear(state, inst->location);
	if (linear) {
		bary1 = state->fs_linear_grf;
		bary2 = state->fs_linear_grf + 1U;
	}

	/* origin + d1 * bary1 + d2 * bary2, written out: Gen11+ has no PLN. */
	drv_i915_eu_alu2(code, I915_EU_MUL, drv_i915_eu_grf(dst),
		drv_i915_eu_grf_scalar(plane, first + 4U),
		drv_i915_eu_grf(bary2));
	drv_i915_eu_alu2(code, I915_EU_ADD, drv_i915_eu_grf(dst),
		drv_i915_eu_grf(dst),
		drv_i915_eu_grf_scalar(plane, first + 12U));
	drv_i915_eu_alu2(code, I915_EU_MUL, drv_i915_eu_grf(state->scratch_grf),
		drv_i915_eu_grf_scalar(plane, first),
		drv_i915_eu_grf(bary1));
	drv_i915_eu_alu2(code, I915_EU_ADD, drv_i915_eu_grf(dst),
		drv_i915_eu_grf(dst),
		drv_i915_eu_grf(state->scratch_grf));
}

/*
 * Lowers a component of gl_FragCoord as Mesa does on Gen12.0
 * (brw_compile_fs.cpp, with anv's nir_lower_wpos_center): x and y are the
 * pixel's integer position plus 0.5 -- the x and y of each subspan (words
 * 4 .. 7 of r1) replicated to its four pixels and the offsets of the pixels
 * in the subspan added, sixteen words at once, then the x or the y of each
 * pixel converted to a float; z is the payload's source depth; w is the
 * reciprocal of the payload's source w.
 */
static void
i915_compile_frag_coord(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg subspans;
	struct i915_eu_reg pixels;
	uint32_t positions;
	uint32_t byte;
	uint32_t dst;

	/* Gives the component its register. */
	dst = i915_compile_define(state, inst->dst, 1U);

	/* z: the interpolated depth, moved. */
	if (inst->component == 2U) {
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf(dst), drv_i915_eu_grf(state->fs_depth_grf));
		return;
	}

	/* w: the reciprocal of the interpolated w. */
	if (inst->component == 3U) {
		drv_i915_eu_math(&state->code, I915_EU_MATH_INV, drv_i915_eu_grf(dst), drv_i915_eu_grf(state->fs_w_grf), drv_i915_eu_null());
		return;
	}

	/* A component beyond w is not lowered. */
	if (inst->component > 3U) {
		state->unsupported = 1;
		return;
	}

	/* Takes a temporary for the sixteen words of positions. */
	positions = i915_compile_temporary(state);
	if (state->out_of_registers != 0)
		return;

	/*
	 * Makes the positions: each subspan's x four times, its y four times,
	 * subspan 0 then 1 (<1;4,0> from word 4), plus the pixel offsets.
	 */
	subspans = drv_i915_eu_grf_region(COMPILE_FS_DISPATCH_GRF, COMPILE_FS_SUBSPAN_BYTE, EU_TYPE_UW, EU_VSTRIDE_1, EU_WIDTH_4, EU_HSTRIDE_0);
	drv_i915_eu_alu2_sixteen(&state->code,
				 I915_EU_ADD,
				 drv_i915_eu_grf_region(positions, 0U, EU_TYPE_UW, EU_VSTRIDE_8, EU_WIDTH_8, EU_HSTRIDE_1),
				 subspans,
				 drv_i915_eu_imm_v(COMPILE_FS_PIXEL_OFFSETS));

	/* Picks the x, or the y, of the eight pixels: four words of each subspan's eight. */
	byte = 0U;
	if (inst->component == 1U)
		byte = COMPILE_FS_PIXEL_Y_BYTE;
	pixels = drv_i915_eu_grf_region(positions, byte, EU_TYPE_UW, EU_VSTRIDE_8, EU_WIDTH_4, EU_HSTRIDE_1);
	drv_i915_eu_mov(&state->code, drv_i915_eu_grf(dst), pixels);

	/* The centre of the pixel. */
	drv_i915_eu_alu2(&state->code, I915_EU_ADD, drv_i915_eu_grf(dst), drv_i915_eu_grf(dst), drv_i915_eu_imm_f(COMPILE_FLOAT_HALF));

	/* The temporary is free again. */
	state->grf_busy[positions] = 0U;
}

/*
 * Lowers a push-constant load: one word of the push registers, read as a
 * scalar and moved bit for bit, so an integer arrives as it was pushed.
 */
static void
i915_compile_load_push(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst,
	uint32_t payload_inputs)
{
	struct i915_eu_reg source;
	uint32_t push_grf;
	uint32_t dst;

	/* Not a word inside the declared push-constant bytes. */
	if ((inst->immediate & 3U) != 0U || inst->immediate + 4U > state->ir->push_bytes) {
		state->error = 1;
		return;
	}

	/* Gives the loaded value its register. */
	dst = i915_compile_define(state, inst->dst, 1U);

	/* The push constants start the push data, 32 bytes to a register. */
	push_grf = payload_inputs - state->push_regs + inst->immediate / 32U;
	source = drv_i915_eu_grf_scalar(push_grf, inst->immediate % 32U);
	source.type = COMPILE_TYPE_UD;
	drv_i915_eu_mov(&state->code, drv_i915_eu_grf_ud(dst), source);
}

/*
 * Lowers a uniform block load: one word of the block's range in the push
 * data, read as a scalar and moved bit for bit.
 */
static void
i915_compile_load_block(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst,
	uint32_t payload_inputs)
{
	struct i915_eu_reg source;
	uint32_t block;
	uint32_t byte;
	uint32_t push_grf;
	uint32_t dst;

	/* Finds the block among the ones the push data carries. */
	for (block = 0U; block < state->block_count; block++) {
		if (state->block_uniform[block] == inst->location)
			break;
	}

	/* A block the interface did not lay out, or a word outside its pushed range, is inconsistent IR. */
	if (block >= state->block_count) {
		state->error = 1;
		return;
	}

	/* Refuses a push load that is misaligned or outside the declared push range. */
	if ((inst->immediate & 3U) != 0U ||
	    inst->immediate < state->block_first_byte[block] ||
	    inst->immediate + 4U > state->block_first_byte[block] + state->block_bytes[block]) {
		state->error = 1;
		return;
	}

	/* Gives the loaded value its register. */
	dst = i915_compile_define(state, inst->dst, 1U);

	/* The word's byte in the push data, then its register and its place in it. */
	byte = state->block_push_offset[block] + inst->immediate - state->block_first_byte[block];
	push_grf = payload_inputs - state->push_regs + byte / 32U;
	source = drv_i915_eu_grf_scalar(push_grf, byte % 32U);
	source.type = COMPILE_TYPE_UD;
	drv_i915_eu_mov(&state->code, drv_i915_eu_grf_ud(dst), source);
}

/*
 * Lowers a storage buffer word's load or store: an A64 untyped message of
 * one channel to the buffer's address, which the push data carries (see
 * i915_compile_blocks()), plus each channel's byte offset.
 *
 * The 64-bit address of each channel is the low word plus the offset, the
 * high word plus the carry (a CMP writes -1 where the sum is below the
 * offset, which the ADD subtracts: Tiger Lake has no 64-bit integer
 * arithmetic), the two interleaved into the two registers of the address
 * payload (Mesa's A64 payload).  A store under a predicate writes only the
 * channels whose predicate holds (f0.0).
 */
static void
i915_compile_storage(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst,
	uint32_t payload_inputs)
{
	struct i915_eu_reg low;
	struct i915_eu_reg high;
	struct i915_eu_reg interleaved;
	struct i915_eu_reg null;
	uint32_t block;
	uint32_t push_grf;
	uint32_t offset_grf;
	uint32_t data_grf;
	uint32_t predicate_grf;
	uint32_t payload;
	uint32_t sum;
	uint32_t carry;
	uint32_t dst;

	/* Finds the storage buffer's address among the push data's blocks. */
	for (block = 0U; block < state->block_count; block++) {
		if (state->block_uniform[block] == inst->location && state->block_address[block] != 0U)
			break;
	}

	/* A storage buffer the interface did not lay out is inconsistent IR. */
	if (block >= state->block_count) {
		state->error = 1;
		return;
	}

	/* The address's two words in the push data. */
	push_grf = payload_inputs - state->push_regs + state->block_push_offset[block] / 32U;
	low = drv_i915_eu_grf_scalar(push_grf, 0U);
	low.type = COMPILE_TYPE_UD;
	high = drv_i915_eu_grf_scalar(push_grf, 4U);
	high.type = COMPILE_TYPE_UD;

	/* Reads the offset, for a store the word and its predicate, and a predicated load's predicate. */
	offset_grf = i915_compile_grf(state, inst->src[0]);
	data_grf = 0U;
	predicate_grf = 0U;
	if (inst->op == DRV_GPU_IR_STORE_STORAGE) {
		data_grf = i915_compile_grf(state, inst->src[1]);
		if (inst->component != 0U)
			predicate_grf = i915_compile_grf(state, inst->src[2]);
	} else if (inst->component != 0U) {
		predicate_grf = i915_compile_grf(state, inst->src[1]);
	}

	/* A load's word gets its register. */
	dst = 0U;
	if (inst->op == DRV_GPU_IR_LOAD_STORAGE)
		dst = i915_compile_define(state, inst->dst, 1U);

	/* Takes the address payload and the sum and carry registers. */
	payload = i915_compile_temporaries(state, 2U);
	sum = i915_compile_temporary(state);
	carry = i915_compile_temporary(state);
	if (state->out_of_registers != 0)
		return;

	/* The low word of each address, and its carry into the high word. */
	drv_i915_eu_alu2(&state->code, I915_EU_ADD, drv_i915_eu_grf_ud(sum), low, drv_i915_eu_grf_ud(offset_grf));
	drv_i915_eu_cmp(&state->code,
			I915_EU_COND_LT,
			I915_EU_FLAG_F0_0,
			0,
			drv_i915_eu_grf_ud(carry),
			drv_i915_eu_grf_ud(sum),
			drv_i915_eu_grf_ud(offset_grf));
	drv_i915_eu_alu2(&state->code, I915_EU_ADD, drv_i915_eu_grf_ud(carry), high, drv_i915_eu_negate(drv_i915_eu_grf_ud(carry)));

	/* Interleaves them: low words at the even dwords of the payload, high words at the odd ones. */
	interleaved = drv_i915_eu_grf_ud(payload);
	interleaved.hstride = EU_HSTRIDE_2;
	drv_i915_eu_mov(&state->code, interleaved, drv_i915_eu_grf_ud(sum));
	interleaved.subnr = 4U;
	drv_i915_eu_mov(&state->code, interleaved, drv_i915_eu_grf_ud(carry));

	/* A predicated load reads where the predicate is not zero; the other channels keep what the register held (ws101-p002). */
	if (inst->op == DRV_GPU_IR_LOAD_STORAGE && inst->component != 0U) {
		null = drv_i915_eu_null();
		null.type = COMPILE_TYPE_D;
		drv_i915_eu_cmp(&state->code,
				I915_EU_COND_NE,
				I915_EU_FLAG_F0_0,
				0,
				null,
				drv_i915_eu_grf_d(predicate_grf),
				drv_i915_eu_imm_d(0U));
		drv_i915_eu_send_masked(&state->code,
					I915_EU_FLAG_F0_0,
					drv_i915_eu_grf(dst),
					drv_i915_eu_grf(payload),
					drv_i915_eu_null(),
					COMPILE_SFID_DATA_CACHE_1,
					COMPILE_DESC_A64_READ,
					0U,
					0,
					0);
	} else if (inst->op == DRV_GPU_IR_LOAD_STORAGE) {
		/* A load reads the word into its register. */
		drv_i915_eu_send(&state->code,
				 drv_i915_eu_grf(dst),
				 drv_i915_eu_grf(payload),
				 drv_i915_eu_null(),
				 COMPILE_SFID_DATA_CACHE_1,
				 COMPILE_DESC_A64_READ,
				 0U,
				 0,
				 0);
	} else if (inst->component != 0U) {
		/* A predicated store writes where the predicate is not zero. */
		null = drv_i915_eu_null();
		null.type = COMPILE_TYPE_D;
		drv_i915_eu_cmp(&state->code,
				I915_EU_COND_NE,
				I915_EU_FLAG_F0_0,
				0,
				null,
				drv_i915_eu_grf_d(predicate_grf),
				drv_i915_eu_imm_d(0U));
		drv_i915_eu_send_masked(&state->code,
					I915_EU_FLAG_F0_0,
					drv_i915_eu_null(),
					drv_i915_eu_grf(payload),
					drv_i915_eu_grf(data_grf),
					COMPILE_SFID_DATA_CACHE_1,
					COMPILE_DESC_A64_WRITE,
					COMPILE_EX_MLEN(1U),
					0,
					0);
	} else {
		/* A store writes every channel. */
		drv_i915_eu_send(&state->code,
				 drv_i915_eu_null(),
				 drv_i915_eu_grf(payload),
				 drv_i915_eu_grf(data_grf),
				 COMPILE_SFID_DATA_CACHE_1,
				 COMPILE_DESC_A64_WRITE,
				 COMPILE_EX_MLEN(1U),
				 0,
				 0);
	}

	/* The temporaries are free again. */
	state->grf_busy[payload] = 0U;
	state->grf_busy[payload + 1U] = 0U;
	state->grf_busy[sum] = 0U;
	state->grf_busy[carry] = 0U;
}

/* Lowers an output store: the component leaves through its staging register. */
static void
i915_compile_store_output(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t source_grf;
	uint32_t rank;
	uint32_t grf;
	int stages_vue;
	int found;

	/* A component beyond w is not lowered. */
	if (inst->component > 3U) {
		state->unsupported = 1;
		return;
	}

	/* A vertex and a geometry shader stage a VUE; a fragment shader its colours. */
	stages_vue = 0;
	if (state->ir->stage == DRV_GPU_STAGE_VERTEX)
		stages_vue = 1;
	if (state->ir->stage == DRV_GPU_STAGE_GEOMETRY)
		stages_vue = 1;

	/* The layer is a geometry shader's only: the staged header's second dword, an integer moved bit for bit. */
	if (inst->location == DRV_GPU_IR_LOCATION_LAYER) {
		if (state->ir->stage != DRV_GPU_STAGE_GEOMETRY) {
			state->unsupported = 1;
			return;
		}

		/* Resolves the scalar output before placing it in the vertex payload. */
		source_grf = i915_compile_grf(state, inst->src[0]);
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf_ud(state->vue_grf + COMPILE_VUE_LAYER), drv_i915_eu_grf_ud(source_grf));
		return;
	}

	/* The point size is the vertex's only: component 3 of the VUE header. */
	if (inst->location == DRV_GPU_IR_LOCATION_POINT_SIZE && state->ir->stage != DRV_GPU_STAGE_VERTEX) {
		state->unsupported = 1;
		return;
	}

	/* A gathered VUE only remembers the value; the end of the shader writes it. */
	if (state->ir->stage == DRV_GPU_STAGE_VERTEX && state->late_vue != 0) {
		/* The point size is slot 0's (the header's) last component. */
		if (inst->location == DRV_GPU_IR_LOCATION_POINT_SIZE) {
			state->output_value[COMPILE_VUE_POINT_SIZE] = inst->src[0];
			return;
		}

		/* The position is slot 1; a varying follows it, in ascending location order. */
		if (inst->location == DRV_GPU_IR_LOCATION_POSITION) {
			rank = 0U;
		} else {
			found = i915_compile_rank(state->varyings, state->varying_count, inst->location, &rank);
			if (found != 0) {
				state->unsupported = 1;
				return;
			}

			/* Places the next varying after the outputs already assigned. */
			rank++;
		}

		/* The last store in program order is the one the end writes. */
		state->output_value[4U * (1U + rank) + inst->component] = inst->src[0];
		return;
	}

	/* The point size is the staged header's last dword. */
	if (inst->location == DRV_GPU_IR_LOCATION_POINT_SIZE) {
		source_grf = i915_compile_grf(state, inst->src[0]);
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf(state->vue_grf + COMPILE_VUE_POINT_SIZE), drv_i915_eu_grf(source_grf));
		return;
	}

	/* Finds the staging registers of the output. */
	if (stages_vue != 0 && inst->location == DRV_GPU_IR_LOCATION_POSITION) {
		/* The position follows the VUE header. */
		grf = state->vue_grf + 4U;
	} else if (stages_vue != 0) {
		/* A varying follows the position, in ascending location order. */
		found = i915_compile_rank(state->varyings, state->varying_count, inst->location, &rank);
		if (found != 0) {
			state->unsupported = 1;
			return;
		}

		/* Locates this varying after the fixed vertex payload registers. */
		grf = state->vue_grf + 8U + 4U * rank;
	} else if (inst->location == DRV_GPU_IR_LOCATION_SECOND_COLOR) {
		/* The second colour of a dual-source write takes location 1's r120..r123 (location 1 is not written then). */
		grf = COMPILE_MAX_GRF - 3U - 4U;
	} else if (inst->location < I915_SHADER_MAX_COLOR_OUTPUTS) {
		/* Colour location n goes to r(124 - 4 n)..r(127 - 4 n). */
		grf = COMPILE_MAX_GRF - 3U - 4U * inst->location;
	} else {
		/* XXX: a colour location past the render targets; Position belongs to a vertex shader. */
		state->unsupported = 1;
		return;
	}

	/* Moves the value into the component's staging register. */
	source_grf = i915_compile_grf(state, inst->src[0]);
	drv_i915_eu_mov(&state->code, drv_i915_eu_grf(grf + inst->component), drv_i915_eu_grf(source_grf));
}

/* Lowers an add, subtract or multiply: sources are read before the destination gets its register. */
static void
i915_compile_arithmetic(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg left;
	struct i915_eu_reg right;
	enum i915_eu_alu op;
	uint32_t left_grf;
	uint32_t right_grf;
	uint32_t dst;

	/* Reads both sources. */
	left_grf = i915_compile_grf(state, inst->src[0]);
	left = drv_i915_eu_grf(left_grf);
	right_grf = i915_compile_grf(state, inst->src[1]);
	right = drv_i915_eu_grf(right_grf);

	/* a - b = a + (-b): ADD with the SECOND source negated (operand order matters). */
	if (inst->op == DRV_GPU_IR_FSUB)
		right = drv_i915_eu_negate(right);

	/* Gives the result its register. */
	dst = i915_compile_define(state, inst->dst, 1U);

	/* A multiply is MUL; an add or a subtract is ADD. */
	if (inst->op == DRV_GPU_IR_FMUL) {
		op = I915_EU_MUL;
	} else {
		op = I915_EU_ADD;
	}

	/* Emits the arithmetic. */
	drv_i915_eu_alu2(&state->code, op, drv_i915_eu_grf(dst), left, right);
}

/* Lowers a negation: a MOV whose source carries the negate modifier. */
static void
i915_compile_negate(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg source;
	uint32_t source_grf;
	uint32_t dst;

	/* Reads the source negated. */
	source_grf = i915_compile_grf(state, inst->src[0]);
	source = drv_i915_eu_grf(source_grf);
	source = drv_i915_eu_negate(source);

	/* Gives the result its register and moves the negated source into it. */
	dst = i915_compile_define(state, inst->dst, 1U);
	drv_i915_eu_mov(&state->code, drv_i915_eu_grf(dst), source);
}

/*
 * Lowers a one-operand math function to one math instruction: sine, cosine,
 * inverse square root, reciprocal, square root, 2^x and log2 (Mesa's RCP,
 * SQRT, EXP2 and LOG2 of frcp, fsqrt, fexp2 and flog2, brw_fs_nir.cpp).
 */
static void
i915_compile_math(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg source;
	enum i915_eu_math func;
	uint32_t source_grf;
	uint32_t dst;

	/* Reads the source. */
	source_grf = i915_compile_grf(state, inst->src[0]);
	source = drv_i915_eu_grf(source_grf);

	/* Gives the result its register. */
	dst = i915_compile_define(state, inst->dst, 1U);

	/* Chooses the math function of the IR operation. */
	if (inst->op == DRV_GPU_IR_SIN) {
		func = I915_EU_MATH_SIN;
	} else if (inst->op == DRV_GPU_IR_COS) {
		func = I915_EU_MATH_COS;
	} else if (inst->op == DRV_GPU_IR_RCP) {
		func = I915_EU_MATH_INV;
	} else if (inst->op == DRV_GPU_IR_SQRT) {
		func = I915_EU_MATH_SQRT;
	} else if (inst->op == DRV_GPU_IR_EXP2) {
		func = I915_EU_MATH_EXP;
	} else if (inst->op == DRV_GPU_IR_LOG2) {
		func = I915_EU_MATH_LOG;
	} else {
		func = I915_EU_MATH_RSQ;
	}

	/* Emits the math; its second operand is the null register. */
	drv_i915_eu_math(&state->code, func, drv_i915_eu_grf(dst), source, drv_i915_eu_null());
}

/*
 * Lowers an absolute value (a move with the abs modifier), a floor (RNDD),
 * a truncation (RNDZ), a rounding to even (RNDE) or a fraction (FRC), as
 * Mesa lowers fabs, ffloor, ftrunc, fround_even and ffract.
 */
static void
i915_compile_unary(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg source;
	uint32_t source_grf;
	uint32_t dst;

	/* Reads the source. */
	source_grf = i915_compile_grf(state, inst->src[0]);
	source = drv_i915_eu_grf(source_grf);

	/* Gives the result its register. */
	dst = i915_compile_define(state, inst->dst, 1U);

	/* Emits the operation. */
	if (inst->op == DRV_GPU_IR_FABS) {
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf(dst), drv_i915_eu_abs(source));
	} else if (inst->op == DRV_GPU_IR_FLOOR) {
		drv_i915_eu_alu1(&state->code, I915_EU_RNDD, drv_i915_eu_grf(dst), source);
	} else if (inst->op == DRV_GPU_IR_FTRUNC) {
		drv_i915_eu_alu1(&state->code, I915_EU_RNDZ, drv_i915_eu_grf(dst), source);
	} else if (inst->op == DRV_GPU_IR_FROUND_EVEN) {
		drv_i915_eu_alu1(&state->code, I915_EU_RNDE, drv_i915_eu_grf(dst), source);
	} else {
		drv_i915_eu_alu1(&state->code, I915_EU_FRC, drv_i915_eu_grf(dst), source);
	}
}

/* Lowers a minimum (sel.l) or a maximum (sel.ge), as Mesa's emit_minmax() does. */
static void
i915_compile_minmax(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg left;
	struct i915_eu_reg right;
	enum i915_eu_cond cond;
	uint32_t left_grf;
	uint32_t right_grf;
	uint32_t dst;

	/* Reads both sources. */
	left_grf = i915_compile_grf(state, inst->src[0]);
	left = drv_i915_eu_grf(left_grf);
	right_grf = i915_compile_grf(state, inst->src[1]);
	right = drv_i915_eu_grf(right_grf);

	/* Gives the result its register. */
	dst = i915_compile_define(state, inst->dst, 1U);

	/* The minimum keeps the source that is less, the maximum the one that is greater or equal. */
	if (inst->op == DRV_GPU_IR_FMIN) {
		cond = I915_EU_COND_LT;
	} else {
		cond = I915_EU_COND_GE;
	}

	/* Emits the selection. */
	drv_i915_eu_minmax(&state->code, cond, drv_i915_eu_grf(dst), left, right);
}

/*
 * Lowers a float comparison to CMP with a register destination, which
 * receives all ones or zero (Mesa: flt, fge, feq and fneu, brw_fs_nir.cpp,
 * with brw_cmod_for_nir_comparison()); the flag it also writes is f0.0.
 */
static void
i915_compile_compare(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg left;
	struct i915_eu_reg right;
	enum i915_eu_cond cond;
	uint32_t left_grf;
	uint32_t right_grf;
	uint32_t dst;

	/* Reads both sources. */
	left_grf = i915_compile_grf(state, inst->src[0]);
	left = drv_i915_eu_grf(left_grf);
	right_grf = i915_compile_grf(state, inst->src[1]);
	right = drv_i915_eu_grf(right_grf);

	/* Gives the Boolean its register. */
	dst = i915_compile_define(state, inst->dst, 1U);

	/* Picks the test; only NE holds when a source is NaN, the unordered comparison of the four. */
	if (inst->op == DRV_GPU_IR_FLT) {
		cond = I915_EU_COND_LT;
	} else if (inst->op == DRV_GPU_IR_FGE) {
		cond = I915_EU_COND_GE;
	} else if (inst->op == DRV_GPU_IR_FEQ) {
		cond = I915_EU_COND_EQ;
	} else {
		cond = I915_EU_COND_NE;
	}

	/* Emits the comparison. */
	drv_i915_eu_cmp(&state->code, cond, I915_EU_FLAG_F0_0, 0, drv_i915_eu_grf(dst), left, right);
}

/* Lowers a Boolean and, or or not to the integer AND, OR or NOT of the all-ones / zero words. */
static void
i915_compile_logic(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg left;
	struct i915_eu_reg right;
	uint32_t left_grf;
	uint32_t right_grf;
	uint32_t dst;

	/* A not reads one source. */
	if (inst->op == DRV_GPU_IR_NOT) {
		left_grf = i915_compile_grf(state, inst->src[0]);
		dst = i915_compile_define(state, inst->dst, 1U);
		drv_i915_eu_alu1(&state->code, I915_EU_NOT, drv_i915_eu_grf_d(dst), drv_i915_eu_grf_d(left_grf));
		return;
	}

	/* Reads both sources. */
	left_grf = i915_compile_grf(state, inst->src[0]);
	left = drv_i915_eu_grf_d(left_grf);
	right_grf = i915_compile_grf(state, inst->src[1]);
	right = drv_i915_eu_grf_d(right_grf);

	/* Gives the result its register and emits the operation. */
	dst = i915_compile_define(state, inst->dst, 1U);
	if (inst->op == DRV_GPU_IR_AND) {
		drv_i915_eu_alu2(&state->code, I915_EU_AND, drv_i915_eu_grf_d(dst), left, right);
	} else {
		drv_i915_eu_alu2(&state->code, I915_EU_OR, drv_i915_eu_grf_d(dst), left, right);
	}
}

/*
 * Lowers a per-channel selection as Mesa lowers b32csel: the condition
 * compared with zero into f0.0, then a SEL predicated on it.  The selection
 * is typed as integers so a float or a Boolean passes bit for bit.
 */
static void
i915_compile_select(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg taken;
	struct i915_eu_reg other;
	struct i915_eu_reg null;
	uint32_t condition_grf;
	uint32_t taken_grf;
	uint32_t other_grf;
	uint32_t dst;

	/* Reads the condition and the two values. */
	condition_grf = i915_compile_grf(state, inst->src[0]);
	taken_grf = i915_compile_grf(state, inst->src[1]);
	taken = drv_i915_eu_grf_d(taken_grf);
	other_grf = i915_compile_grf(state, inst->src[2]);
	other = drv_i915_eu_grf_d(other_grf);

	/* Gives the result its register. */
	dst = i915_compile_define(state, inst->dst, 1U);

	/* Sets f0.0 where the condition is true (not zero), unless the SELECT before set it for the same condition. */
	if (state->select_flag != inst->src[0] + 1U) {
		null = drv_i915_eu_null();
		null.type = COMPILE_TYPE_D;
		drv_i915_eu_cmp(&state->code,
				I915_EU_COND_NE,
				I915_EU_FLAG_F0_0,
				0,
				null,
				drv_i915_eu_grf_d(condition_grf),
				drv_i915_eu_imm_d(0U));
		state->select_flag = inst->src[0] + 1U;
	}

	/* Takes the first value where the flag is set, the second elsewhere. */
	drv_i915_eu_select(&state->code, I915_EU_FLAG_F0_0, drv_i915_eu_grf_d(dst), taken, other);
}

/*
 * Lowers a discard of the pixels where the condition is true: a CMP that
 * clears their bits of f1.0, predicated on f1.0 so a pixel discarded before
 * stays discarded (Mesa's terminate_if fallback, brw_fs_nir.cpp).  The pixels
 * keep executing; the render-target write at the end goes to the pixels f1.0
 * still holds, and ends the thread even when it holds none.
 *
 * XXX: Mesa also jumps to the end (HALT) once a whole quad is discarded;
 * that is an optimization this compiler leaves out.
 */
static void
i915_compile_kill(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg null;
	uint32_t condition_grf;

	/* Only a fragment shader discards; the interface already refused anything else. */
	if (state->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		state->unsupported = 1;
		return;
	}

	/* Reads the condition. */
	condition_grf = i915_compile_grf(state, inst->src[0]);

	/* Keeps, of the live pixels, those where the condition is false: zero. */
	null = drv_i915_eu_null();
	null.type = COMPILE_TYPE_D;
	drv_i915_eu_cmp(&state->code,
			I915_EU_COND_EQ,
			I915_EU_FLAG_F1_0,
			1,
			null,
			drv_i915_eu_grf_d(condition_grf),
			drv_i915_eu_imm_d(0U));
}

/*
 * Lowers texture(): u and v are the two payload runs, the reply is four
 * registers (see the conventions).  The n-th sampled image of the shader's
 * uniforms is binding table entry 1 + n and sampler n.  A sample with a bias,
 * a level of detail or a texel offset builds a longer message
 * (i915_compile_sample_message()).
 */
static void
i915_compile_sample(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg u;
	struct i915_eu_reg v;
	uint32_t u_grf;
	uint32_t v_grf;
	uint32_t sampler;
	uint32_t dst;
	int found;

	/* Finds the sampled image the instruction names. */
	found = i915_compile_sampler_index(state, inst, &sampler);
	if (found == 0) {
		state->unsupported = 1;
		return;
	}

	/* A sample with more than a coordinate builds its message. */
	if (inst->op != DRV_GPU_IR_SAMPLE || inst->component != 0U) {
		i915_compile_sample_message(state, inst, sampler);
		return;
	}

	/* Reads the coordinate. */
	u_grf = i915_compile_grf(state, inst->src[0]);
	u = drv_i915_eu_grf(u_grf);
	v_grf = i915_compile_grf(state, inst->src[1]);
	v = drv_i915_eu_grf(v_grf);

	/* Gives the four reply values four consecutive registers. */
	dst = i915_compile_define(state, inst->dst, 4U);

	/* Emits the sample: binding table entry 1 + n, sampler n. */
	drv_i915_eu_send(&state->code,
			 drv_i915_eu_grf(dst),
			 u,
			 v,
			 COMPILE_SFID_SAMPLER,
			 COMPILE_DESC_SAMPLE(1U + sampler, sampler),
			 COMPILE_EX_MLEN(1U),
			 0,
			 0);
}

/*
 * Finds the sampled image a sample names by its set and binding: its place
 * n among the shader's sampled images, which is its sampler.  Returns
 * nonzero when found and the descriptor can name it.
 */
static int
i915_compile_sampler_index(
	const struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst,
	uint32_t *sampler)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	uint32_t index;

	/* Counts the sampled images before the one named. */
	*sampler = 0U;
	for (index = 0U; index < state->ir->uniform_count; index++) {
		uniform = &state->ir->uniforms[index];
		if (uniform->kind != DRV_GPU_IR_UNIFORM_SAMPLED_IMAGE)
			continue;
		if (uniform->set == inst->location && uniform->binding == inst->immediate)
			break;
		(*sampler)++;
	}

	/* An unknown image, or one past what the descriptor can name, is not found. */
	if (index == state->ir->uniform_count || *sampler > COMPILE_MAX_SAMPLER)
		return 0;

	/* Succeeded: the image is the sampler-th. */
	return 1;
}

/*
 * Lowers a sample whose message carries more than the coordinate, as Mesa
 * builds it on Gen12.0 (lower_sampler_logical_send(),
 * brw_lower_logical_sends.cpp): a header when there is a texel offset
 * (cleared, the offset in dword 2, r0.3's sampler state pointer in dword
 * 3), then the bias (sample_b) or the level of detail (sample_l), then u
 * and v, one register each, copied into consecutive temporaries and sent
 * as one run.
 */
static void
i915_compile_sample_message(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst,
	uint32_t sampler)
{
	struct i915_eu_reg dword;
	struct i915_eu_reg thread;
	uint32_t u_grf;
	uint32_t v_grf;
	uint32_t level_grf;
	uint32_t payload;
	uint32_t length;
	uint32_t header;
	uint32_t message;
	uint32_t descriptor;
	uint32_t next;
	uint32_t dst;
	uint32_t index;

	/* Reads the coordinate and the bias or the level of detail. */
	u_grf = i915_compile_grf(state, inst->src[0]);
	v_grf = i915_compile_grf(state, inst->src[1]);
	level_grf = COMPILE_NO_GRF;
	if (inst->op != DRV_GPU_IR_SAMPLE)
		level_grf = i915_compile_grf(state, inst->src[2]);

	/* Gives the four reply values four consecutive registers. */
	dst = i915_compile_define(state, inst->dst, 4U);

	/* The message: a header for an offset, the bias or the level, u and v. */
	header = 0U;
	if (inst->component != 0U)
		header = 1U;
	length = header + 2U;
	if (level_grf != COMPILE_NO_GRF)
		length++;

	/* Takes the message's registers. */
	payload = i915_compile_temporaries(state, length);
	if (state->out_of_registers != 0)
		return;

	/* Writes the header: zeros, the offset, the sampler state pointer without its low bits. */
	next = payload;
	if (header != 0U) {
		drv_i915_eu_mov_all(&state->code, drv_i915_eu_grf_ud(payload), drv_i915_eu_imm_ud(0U));
		dword = drv_i915_eu_grf_ud(payload);
		dword.subnr = 4U * EU_SAMPLER_HEADER_OFFSET_DWORD;
		drv_i915_eu_mov_scalar(&state->code, dword, drv_i915_eu_imm_ud(inst->component));
		dword.subnr = 4U * EU_SAMPLER_HEADER_STATE_DWORD;
		thread = drv_i915_eu_grf_scalar(0U, 4U * EU_SAMPLER_HEADER_STATE_DWORD);
		thread.type = COMPILE_TYPE_UD;
		drv_i915_eu_alu2_scalar(&state->code, I915_EU_AND, dword, thread, drv_i915_eu_imm_ud(EU_SAMPLER_STATE_POINTER_MASK));
		next++;
	}

	/* Copies the bias or the level, then u and v, after it. */
	if (level_grf != COMPILE_NO_GRF) {
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf(next), drv_i915_eu_grf(level_grf));
		next++;
	}

	/* u and v after them. */
	drv_i915_eu_mov(&state->code, drv_i915_eu_grf(next), drv_i915_eu_grf(u_grf));
	drv_i915_eu_mov(&state->code, drv_i915_eu_grf(next + 1U), drv_i915_eu_grf(v_grf));

	/* The message type: sample_b with a bias, sample_l with a level, sample otherwise. */
	if (inst->op == DRV_GPU_IR_SAMPLE_BIAS) {
		message = EU_SAMPLER_MESSAGE_SAMPLE_BIAS;
	} else if (inst->op == DRV_GPU_IR_SAMPLE_LOD) {
		message = EU_SAMPLER_MESSAGE_SAMPLE_LOD;
	} else {
		message = EU_SAMPLER_MESSAGE_SAMPLE;
	}

	/* The descriptor: the message's length, a four-register reply, the header, SIMD8, the type, sampler n and entry 1 + n. */
	descriptor = (length << EU_DESC_MLEN_SHIFT) |
		     (4U << EU_DESC_RLEN_SHIFT) |
		     (EU_SAMPLER_SIMD8 << EU_SAMPLER_SIMD_SHIFT) |
		     (message << EU_SAMPLER_TYPE_SHIFT) |
		     (sampler << EU_SAMPLER_INDEX_SHIFT) |
		     (1U + sampler);
	if (header != 0U)
		descriptor |= EU_DESC_HEADER_PRESENT;

	/* Emits the sample. */
	drv_i915_eu_send(&state->code,
			 drv_i915_eu_grf(dst),
			 drv_i915_eu_grf(payload),
			 drv_i915_eu_null(),
			 COMPILE_SFID_SAMPLER,
			 descriptor,
			 0U,
			 0,
			 0);

	/* The message's registers are free again. */
	for (index = 0U; index < length; index++)
		state->grf_busy[payload + index] = 0U;
}

/*
 * Lowers a texture message (DRV_GPU_IR_TEXTURE), as Mesa builds it on Gen12.0
 * (lower_sampler_logical_send(), brw_lower_logical_sends.cpp): a header
 * when there is a texel offset (as i915_compile_sample_message() writes
 * it), then the parameters in the order the IR gives them, each copied
 * bit for bit into consecutive temporaries, sent as one run with a
 * four-register reply.  The n-th sampled image of the shader's uniforms
 * is binding table entry 1 + n and sampler n.
 */
static void
i915_compile_texture(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg dword;
	struct i915_eu_reg thread;
	uint32_t sampler;
	uint32_t param_count;
	uint32_t texel_offset;
	uint32_t payload;
	uint32_t length;
	uint32_t header;
	uint32_t descriptor;
	uint32_t source_grf;
	uint32_t next;
	uint32_t dst;
	uint32_t index;
	int found;

	/* Finds the sampled image the instruction names. */
	found = i915_compile_sampler_index(state, inst, &sampler);
	if (found == 0) {
		state->unsupported = 1;
		return;
	}

	/* Refuses a message longer than the IR allows. */
	param_count = inst->src[1];
	if (param_count == 0U || param_count > DRV_GPU_IR_TEXTURE_MAX_PARAMS) {
		state->error = 1;
		return;
	}

	/* Gives the four reply values four consecutive registers. */
	dst = i915_compile_define(state, inst->dst, 4U);

	/* The message: a header for an offset, then the parameters. */
	texel_offset = inst->component >> 8;
	header = 0U;
	if (texel_offset != 0U)
		header = 1U;
	length = header + param_count;

	/* Takes the message's registers. */
	payload = i915_compile_temporaries(state, length);
	if (state->out_of_registers != 0)
		return;

	/* Writes the header: zeros, the offset, the sampler state pointer without its low bits. */
	next = payload;
	if (header != 0U) {
		drv_i915_eu_mov_all(&state->code, drv_i915_eu_grf_ud(payload), drv_i915_eu_imm_ud(0U));
		dword = drv_i915_eu_grf_ud(payload);
		dword.subnr = 4U * EU_SAMPLER_HEADER_OFFSET_DWORD;
		drv_i915_eu_mov_scalar(&state->code, dword, drv_i915_eu_imm_ud(texel_offset));
		dword.subnr = 4U * EU_SAMPLER_HEADER_STATE_DWORD;
		thread = drv_i915_eu_grf_scalar(0U, 4U * EU_SAMPLER_HEADER_STATE_DWORD);
		thread.type = COMPILE_TYPE_UD;
		drv_i915_eu_alu2_scalar(&state->code, I915_EU_AND, dword, thread, drv_i915_eu_imm_ud(EU_SAMPLER_STATE_POINTER_MASK));
		next++;
	}

	/* Copies each parameter's bits after it: floats and integers alike. */
	for (index = 0U; index < param_count; index++) {
		source_grf = i915_compile_grf(state, inst->src[0] + index);
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf_ud(next), drv_i915_eu_grf_ud(source_grf));
		next++;
	}

	/* The descriptor: the message's length, a four-register reply, the header, SIMD8, the type, sampler n and entry 1 + n. */
	descriptor = (length << EU_DESC_MLEN_SHIFT) |
		     (4U << EU_DESC_RLEN_SHIFT) |
		     (EU_SAMPLER_SIMD8 << EU_SAMPLER_SIMD_SHIFT) |
		     ((inst->component & EU_SAMPLER_TYPE_MASK) << EU_SAMPLER_TYPE_SHIFT) |
		     (sampler << EU_SAMPLER_INDEX_SHIFT) |
		     (1U + sampler);
	if (header != 0U)
		descriptor |= EU_DESC_HEADER_PRESENT;

	/* Emits the message. */
	drv_i915_eu_send(&state->code,
			 drv_i915_eu_grf(dst),
			 drv_i915_eu_grf(payload),
			 drv_i915_eu_null(),
			 COMPILE_SFID_SAMPLER,
			 descriptor,
			 0U,
			 0,
			 0);

	/* The message's registers are free again. */
	for (index = 0U; index < length; index++)
		state->grf_busy[payload + index] = 0U;
}

/*
 * Lowers integer add, subtract, negate, the bitwise operations and the
 * shifts, each one instruction on signed (or, for a logical right shift,
 * unsigned) words: a subtraction adds the negated second source, a
 * negation moves the negated source (Mesa: iadd, ineg, iand, ior, ixor,
 * inot, ishl, ushr, ishr, brw_fs_nir.cpp).
 */
static void
i915_compile_integer(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg left;
	struct i915_eu_reg right;
	uint32_t left_grf;
	uint32_t right_grf;
	uint32_t dst;

	/* Reads the first source. */
	left_grf = i915_compile_grf(state, inst->src[0]);
	left = drv_i915_eu_grf_d(left_grf);

	/* A negation and a complement read one source. */
	if (inst->op == DRV_GPU_IR_INEG || inst->op == DRV_GPU_IR_INOT) {
		dst = i915_compile_define(state, inst->dst, 1U);
		if (inst->op == DRV_GPU_IR_INEG) {
			drv_i915_eu_mov(&state->code, drv_i915_eu_grf_d(dst), drv_i915_eu_negate(left));
		} else {
			drv_i915_eu_alu1(&state->code, I915_EU_NOT, drv_i915_eu_grf_d(dst), left);
		}

		/* Succeeded: the unary integer operation has its encoded destination. */
		return;
	}

	/* Reads the second source. */
	right_grf = i915_compile_grf(state, inst->src[1]);
	right = drv_i915_eu_grf_d(right_grf);

	/* Gives the result its register. */
	dst = i915_compile_define(state, inst->dst, 1U);

	/* Emits the operation. */
	switch (inst->op) {
	case DRV_GPU_IR_IADD:
		drv_i915_eu_alu2(&state->code, I915_EU_ADD, drv_i915_eu_grf_d(dst), left, right);
		break;

	case DRV_GPU_IR_ISUB:
		drv_i915_eu_alu2(&state->code, I915_EU_ADD, drv_i915_eu_grf_d(dst), left, drv_i915_eu_negate(right));
		break;

	case DRV_GPU_IR_IAND:
		drv_i915_eu_alu2(&state->code, I915_EU_AND, drv_i915_eu_grf_d(dst), left, right);
		break;

	case DRV_GPU_IR_IOR:
		drv_i915_eu_alu2(&state->code, I915_EU_OR, drv_i915_eu_grf_d(dst), left, right);
		break;

	case DRV_GPU_IR_IXOR:
		drv_i915_eu_alu2(&state->code, I915_EU_XOR, drv_i915_eu_grf_d(dst), left, right);
		break;

	case DRV_GPU_IR_SHL:
		drv_i915_eu_alu2(&state->code, I915_EU_SHL, drv_i915_eu_grf_d(dst), left, right);
		break;

	case DRV_GPU_IR_SHR:
		/* A logical shift reads the word as unsigned, so zeros come in. */
		drv_i915_eu_alu2(&state->code,
				 I915_EU_SHR,
				 drv_i915_eu_grf_ud(dst),
				 drv_i915_eu_grf_ud(left_grf),
				 drv_i915_eu_grf_ud(right_grf));
		break;

	default:
		/* An arithmetic shift copies the sign bit in. */
		drv_i915_eu_alu2(&state->code, I915_EU_ASR, drv_i915_eu_grf_d(dst), left, right);
		break;
	}
}

/*
 * Lowers a 32-bit integer multiply as Mesa does on a part without a 32 x
 * 32-bit multiply (brw_lower_integer_multiplication.cpp): the first source
 * times the low 16 bits of the second, plus, shifted up by 16, the first
 * source times the high 16 bits of the second -- the low 32 bits of the
 * product.
 */
static void
i915_compile_multiply(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t left_grf;
	uint32_t right_grf;
	uint32_t high;
	uint32_t dst;

	/* Reads both sources. */
	left_grf = i915_compile_grf(state, inst->src[0]);
	right_grf = i915_compile_grf(state, inst->src[1]);

	/* Gives the result its register, and takes a temporary for the high partial product. */
	dst = i915_compile_define(state, inst->dst, 1U);
	high = i915_compile_temporary(state);

	/* The two partial products. */
	drv_i915_eu_alu2(&state->code,
			 I915_EU_MUL,
			 drv_i915_eu_grf_d(dst),
			 drv_i915_eu_grf_d(left_grf),
			 drv_i915_eu_grf_uw_half(right_grf, 0));
	drv_i915_eu_alu2(&state->code,
			 I915_EU_MUL,
			 drv_i915_eu_grf_d(high),
			 drv_i915_eu_grf_d(left_grf),
			 drv_i915_eu_grf_uw_half(right_grf, 1));

	/* The high one moves up by 16 bits and joins the low one. */
	drv_i915_eu_alu2(&state->code, I915_EU_SHL, drv_i915_eu_grf_d(high), drv_i915_eu_grf_d(high), drv_i915_eu_imm_d(16U));
	drv_i915_eu_alu2(&state->code, I915_EU_ADD, drv_i915_eu_grf_d(dst), drv_i915_eu_grf_d(dst), drv_i915_eu_grf_d(high));

	/* The temporary is free again. */
	state->grf_busy[high] = 0U;
}

/*
 * Lowers an integer division or remainder to one math instruction, as Mesa
 * does on Gen12.0 (brw_fs_nir.cpp: idiv and udiv to INT_QUOTIENT, umod and
 * irem to INT_REMAINDER; brw_nir.c divides in NIR only from Xe-HP on).  The
 * operands are read as unsigned words for UDIV and UMOD and as signed ones
 * for IDIV and IREM: the hardware rounds a signed quotient toward zero and
 * gives a signed remainder the dividend's sign.  A zero divisor is left to
 * the hardware, as SPIR-V leaves it undefined.
 */
static void
i915_compile_divide(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg dividend;
	struct i915_eu_reg divisor;
	struct i915_eu_reg result;
	enum i915_eu_math func;
	uint32_t dividend_grf;
	uint32_t divisor_grf;
	uint32_t dst;

	/* Reads the dividend and the divisor. */
	dividend_grf = i915_compile_grf(state, inst->src[0]);
	divisor_grf = i915_compile_grf(state, inst->src[1]);

	/* Gives the result its register. */
	dst = i915_compile_define(state, inst->dst, 1U);

	/* An unsigned division reads and writes unsigned words, a signed one signed words. */
	if (inst->op == DRV_GPU_IR_UDIV || inst->op == DRV_GPU_IR_UMOD) {
		dividend = drv_i915_eu_grf_ud(dividend_grf);
		divisor = drv_i915_eu_grf_ud(divisor_grf);
		result = drv_i915_eu_grf_ud(dst);
	} else {
		dividend = drv_i915_eu_grf_d(dividend_grf);
		divisor = drv_i915_eu_grf_d(divisor_grf);
		result = drv_i915_eu_grf_d(dst);
	}

	/* A division keeps the quotient, a remainder the remainder. */
	if (inst->op == DRV_GPU_IR_UDIV || inst->op == DRV_GPU_IR_IDIV) {
		func = I915_EU_MATH_INT_QUOTIENT;
	} else {
		func = I915_EU_MATH_INT_REMAINDER;
	}

	/* Emits the math. */
	drv_i915_eu_math(&state->code, func, result, dividend, divisor);
}

/*
 * Lowers a conversion between floats and integers: a MOV from one type to
 * the other, which rounds a float toward zero when it becomes an integer
 * (Mesa: i2f, u2f, f2i, f2u, brw_fs_nir.cpp).
 */
static void
i915_compile_convert(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t source_grf;
	uint32_t dst;

	/* Reads the source and gives the result its register. */
	source_grf = i915_compile_grf(state, inst->src[0]);
	dst = i915_compile_define(state, inst->dst, 1U);

	/* Moves across the types. */
	if (inst->op == DRV_GPU_IR_I2F) {
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf(dst), drv_i915_eu_grf_d(source_grf));
	} else if (inst->op == DRV_GPU_IR_U2F) {
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf(dst), drv_i915_eu_grf_ud(source_grf));
	} else if (inst->op == DRV_GPU_IR_F2I) {
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf_d(dst), drv_i915_eu_grf(source_grf));
	} else {
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf_ud(dst), drv_i915_eu_grf(source_grf));
	}
}

/*
 * Lowers an integer comparison to CMP with a register destination, the
 * sources read as signed or as unsigned words (Mesa: ilt, ige, ult, uge,
 * ieq, ine, brw_fs_nir.cpp).
 */
static void
i915_compile_integer_compare(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg left;
	struct i915_eu_reg right;
	enum i915_eu_cond cond;
	uint32_t left_grf;
	uint32_t right_grf;
	uint32_t dst;

	/* Reads both sources, as unsigned words for an unsigned comparison. */
	left_grf = i915_compile_grf(state, inst->src[0]);
	right_grf = i915_compile_grf(state, inst->src[1]);
	if (inst->op == DRV_GPU_IR_ULT || inst->op == DRV_GPU_IR_UGE) {
		left = drv_i915_eu_grf_ud(left_grf);
		right = drv_i915_eu_grf_ud(right_grf);
	} else {
		left = drv_i915_eu_grf_d(left_grf);
		right = drv_i915_eu_grf_d(right_grf);
	}

	/* Gives the Boolean its register. */
	dst = i915_compile_define(state, inst->dst, 1U);

	/* Picks the test. */
	if (inst->op == DRV_GPU_IR_ILT || inst->op == DRV_GPU_IR_ULT) {
		cond = I915_EU_COND_LT;
	} else if (inst->op == DRV_GPU_IR_IGE || inst->op == DRV_GPU_IR_UGE) {
		cond = I915_EU_COND_GE;
	} else if (inst->op == DRV_GPU_IR_IEQ) {
		cond = I915_EU_COND_EQ;
	} else {
		cond = I915_EU_COND_NE;
	}

	/* Emits the comparison. */
	drv_i915_eu_cmp(&state->code, cond, I915_EU_FLAG_F0_0, 0, drv_i915_eu_grf_d(dst), left, right);
}

/*
 * Lowers a MOVE: the source's bits into the destination's register, which
 * a loop variable keeps from the move before its loop to its last use.
 */
static void
i915_compile_move(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t source_grf;
	uint32_t dst;

	/* Reads the source. */
	source_grf = i915_compile_grf(state, inst->src[0]);

	/* The destination keeps the register it has; the first move gives it one. */
	if (inst->dst < state->ir->value_count && state->value_grf[inst->dst] != COMPILE_NO_GRF) {
		dst = state->value_grf[inst->dst];
	} else {
		dst = i915_compile_define(state, inst->dst, 1U);
	}

	/* Moves the bits. */
	drv_i915_eu_mov(&state->code, drv_i915_eu_grf_ud(dst), drv_i915_eu_grf_ud(source_grf));
}

/*
 * Lowers a derivative as Mesa does (generate_ddx() and generate_ddy(),
 * brw_generator.cpp): one ADD of two regions of the source that pick, in
 * each 2x2 quad of pixels, the right and the left pixel (x) or the bottom
 * and the top one (y) -- the quad's top row for all four pixels (coarse),
 * or each row its own (fine x).  A vertex shader has no quads.
 */
static void
i915_compile_derivative(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg minuend;
	struct i915_eu_reg subtrahend;
	struct i915_eu_reg target;
	uint32_t source_grf;
	uint32_t dst;
	uint32_t quad;

	/* Only pixels come in quads. */
	if (state->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		state->unsupported = 1;
		return;
	}

	/* Reads the source and gives the result its register. */
	source_grf = i915_compile_grf(state, inst->src[0]);
	dst = i915_compile_define(state, inst->dst, 1U);

	/*
	 * The fine y derivative takes each column's own difference, as Mesa
	 * does on Gen11+ (generate_ddy(), brw_generator.cpp): per quad, a
	 * four-channel add of the bottom row less the top row, both read
	 * <0;2,1> so each row serves its two pixels; outside the channel mask,
	 * the destination being the value's own.
	 */
	if (inst->op == DRV_GPU_IR_DDY_FINE) {
		for (quad = 0U; quad < 2U; quad++) {
			minuend = drv_i915_eu_grf_region(source_grf, 16U * quad + 8U, EU_TYPE_F, EU_VSTRIDE_0, EU_WIDTH_2, EU_HSTRIDE_1);
			subtrahend = drv_i915_eu_grf_region(source_grf, 16U * quad, EU_TYPE_F, EU_VSTRIDE_0, EU_WIDTH_2, EU_HSTRIDE_1);
			target = drv_i915_eu_grf(dst);
			target.subnr = 16U * quad;
			drv_i915_eu_alu2_four(&state->code, I915_EU_ADD, target, minuend, drv_i915_eu_negate(subtrahend));
		}

		/* Succeeded: both quads have their differences. */
		return;
	}

	/* Picks the two pixels of each quad. */
	if (inst->op == DRV_GPU_IR_DDX_FINE) {
		/* Each row: its right pixel (one float on) less its left one, <2;2,0>. */
		minuend = drv_i915_eu_grf_region(source_grf, 4U, EU_TYPE_F, EU_VSTRIDE_2, EU_WIDTH_2, EU_HSTRIDE_0);
		subtrahend = drv_i915_eu_grf_region(source_grf, 0U, EU_TYPE_F, EU_VSTRIDE_2, EU_WIDTH_2, EU_HSTRIDE_0);
	} else if (inst->op == DRV_GPU_IR_DDX) {
		/* The top row: its right pixel less its left one, <4;4,0>. */
		minuend = drv_i915_eu_grf_region(source_grf, 4U, EU_TYPE_F, EU_VSTRIDE_4, EU_WIDTH_4, EU_HSTRIDE_0);
		subtrahend = drv_i915_eu_grf_region(source_grf, 0U, EU_TYPE_F, EU_VSTRIDE_4, EU_WIDTH_4, EU_HSTRIDE_0);
	} else {
		/* The left column: its bottom pixel (two floats on) less its top one, <4;4,0>. */
		minuend = drv_i915_eu_grf_region(source_grf, 8U, EU_TYPE_F, EU_VSTRIDE_4, EU_WIDTH_4, EU_HSTRIDE_0);
		subtrahend = drv_i915_eu_grf_region(source_grf, 0U, EU_TYPE_F, EU_VSTRIDE_4, EU_WIDTH_4, EU_HSTRIDE_0);
	}

	/* Emits the difference. */
	drv_i915_eu_alu2(&state->code, I915_EU_ADD, drv_i915_eu_grf(dst), minuend, drv_i915_eu_negate(subtrahend));
}

/*
 * Lowers the half-float packing as Mesa does (brw_lower_pack.cpp,
 * brw_fs_nir.cpp): a pack moves each float into its 16-bit half of the
 * destination's channels (an HF destination of stride two, the conversion
 * the move makes), an unpack moves the half the instruction names (an HF
 * source of stride two) into a float.
 */
static void
i915_compile_half(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t low_grf;
	uint32_t high_grf;
	uint32_t dst;

	/* An unpack converts one half of each channel. */
	if (inst->op == DRV_GPU_IR_UNPACK_HALF) {
		low_grf = i915_compile_grf(state, inst->src[0]);
		dst = i915_compile_define(state, inst->dst, 1U);
		drv_i915_eu_mov(&state->code,
				drv_i915_eu_grf(dst),
				drv_i915_eu_grf_region(low_grf, 2U * (inst->component & 1U), EU_TYPE_HF, EU_VSTRIDE_16, EU_WIDTH_8, EU_HSTRIDE_2));
		return;
	}

	/* Reads both floats and gives the packed word its register. */
	low_grf = i915_compile_grf(state, inst->src[0]);
	high_grf = i915_compile_grf(state, inst->src[1]);
	dst = i915_compile_define(state, inst->dst, 1U);

	/* Converts the first into the low halves, the second into the high ones. */
	drv_i915_eu_mov(&state->code,
			drv_i915_eu_grf_region(dst, 0U, EU_TYPE_HF, EU_VSTRIDE_16, EU_WIDTH_8, EU_HSTRIDE_2),
			drv_i915_eu_grf(low_grf));
	drv_i915_eu_mov(&state->code,
			drv_i915_eu_grf_region(dst, 2U, EU_TYPE_HF, EU_VSTRIDE_16, EU_WIDTH_8, EU_HSTRIDE_2),
			drv_i915_eu_grf(high_grf));
}

/* Lowers the start of a loop: remembers where the WHILE at its end jumps back to. */
static void
i915_compile_loop_begin(
	struct i915_compile_state *state)
{
	/* Loops nested deeper than the stack were refused by the liveness pass. */
	if (state->loop_depth >= COMPILE_MAX_LOOPS) {
		state->error = 1;
		return;
	}

	/* The next instruction is the first of the loop's body. */
	state->loop_tops[state->loop_depth] = drv_i915_eu_position(&state->code);
	state->loop_depth++;
}

/*
 * Lowers the end of a loop: f0.0 set where the Boolean holds, then a WHILE
 * on it back to the loop's first instruction.  The channels whose Boolean
 * is false stop running the loop; the loop ends when none goes round again.
 * The guarded-texture ENDIFs of the loop's body are pointed at the WHILE.
 */
static void
i915_compile_loop_end(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct i915_eu_reg null;
	uint32_t condition_grf;
	uint32_t here;
	uint32_t kept;
	uint32_t index;

	/* A loop end with no loop open was refused by the liveness pass. */
	if (state->loop_depth == 0U) {
		state->error = 1;
		return;
	}

	/* Reads the Boolean of the channels that go round again. */
	condition_grf = i915_compile_grf(state, inst->src[0]);

	/* Sets f0.0 where it is true: not zero. */
	null = drv_i915_eu_null();
	null.type = COMPILE_TYPE_D;
	drv_i915_eu_cmp(&state->code,
			I915_EU_COND_NE,
			I915_EU_FLAG_F0_0,
			0,
			null,
			drv_i915_eu_grf_d(condition_grf),
			drv_i915_eu_imm_d(0U));

	/* Jumps back for those channels, and closes the loop. */
	state->loop_depth--;
	here = drv_i915_eu_position(&state->code);
	drv_i915_eu_while(&state->code, I915_EU_FLAG_F0_0, state->loop_tops[state->loop_depth]);

	/*
	 * Points the JIP of the loop body's guarded-texture ENDIFs at the WHILE
	 * (Mesa's brw_set_uip_jip()), and drops them from the list.
	 */
	kept = 0U;
	for (index = 0U; index < state->endif_count; index++) {
		if (state->endif_depth[index] == state->loop_depth + 1U) {
			drv_i915_eu_patch_endif(&state->code, state->endif_position[index], here);
			continue;
		}

		/* An ENDIF of an outer loop stays for that loop's WHILE. */
		state->endif_position[kept] = state->endif_position[index];
		state->endif_depth[kept] = state->endif_depth[index];
		kept++;
	}

	/* The list holds the outer loops' ENDIFs only. */
	state->endif_count = kept;
}

/*
 * Lowers a texture message under an IF on its guard (ws075-p021): an IF that
 * every channel takes when any channel is in the guard (i915_compile_if_any(),
 * so the derivatives of the message's coordinates see every pixel of the
 * quad), the message, the ENDIF.  A thread none of whose channels is in the
 * guard jumps over the message.  The channels outside the guard read its result
 * only in selections that take something else (struct drv_gpu_shader_ir_inst).
 * An unguarded message, or one past COMPILE_MAX_GUARDS, is lowered as it is.
 */
static void
i915_compile_guarded_texture(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t if_position;
	uint32_t endif_position;
	int guarded;

	/* A guard, and room to remember its ENDIF, make the message guarded; inside a skipped region the region's IF guards it already. */
	guarded = 0;
	if (inst->guard != 0U && state->guards < COMPILE_MAX_GUARDS && !state->skip_active)
		guarded = 1;

	/* Opens an IF that every channel takes when any is in the guard. */
	if_position = 0U;
	if (guarded) {
		if_position = i915_compile_if_any(state, inst->guard - 1U);
		state->guards++;
	}

	/* Lowers the message itself. */
	if (inst->op == DRV_GPU_IR_TEXTURE) {
		i915_compile_texture(state, inst);
	} else {
		i915_compile_sample(state, inst);
	}

	/* An unguarded message is done. */
	if (!guarded)
		return;

	/* Closes the IF, points it at its ENDIF, and keeps an ENDIF inside a loop for the loop's WHILE. */
	endif_position = drv_i915_eu_endif(&state->code);
	drv_i915_eu_patch_if(&state->code, if_position, endif_position);
	if (state->loop_depth != 0U) {
		state->endif_position[state->endif_count] = endif_position;
		state->endif_depth[state->endif_count] = state->loop_depth;
		state->endif_count++;
	}
}


/*
 * Decides which skippable regions (SKIP_BEGIN .. SKIP_END, one block of the
 * parser each) are really skipped (ws075-p023): each is checked by
 * i915_compile_skip_region().  A shader longer than COMPILE_MAX_SKIP_CHECK
 * skips none.
 */
static void
i915_compile_skips(
	struct i915_compile_state *state)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t index;
	uint32_t end;
	uint32_t value;
	uint32_t results;
	int sound;

	/* The shader being compiled. */
	ir = state->ir;

	/* Nothing is skipped until proven safe. */
	for (index = 0U; index < ir->instruction_count; index++)
		state->skip_ok[index] = 0U;

	/* A long shader is not checked. */
	if (ir->instruction_count > COMPILE_MAX_SKIP_CHECK)
		return;

	/* The garbage tables; without them nothing is skipped. */
	state->skip_garbage = kern_calloc((size_t)ir->value_count * COMPILE_SKIP_WORDS + 1U, sizeof(uint64_t));
	if (state->skip_garbage == NULL)
		return;

	/* The instruction that defines each value, plus one (zero for none). */
	for (value = 0U; value < ir->value_count; value++)
		state->skip_def[value] = 0U;
	for (index = 0U; index < ir->instruction_count; index++) {
		inst = &ir->instructions[index];
		results = i915_compile_results(inst);
		for (value = inst->dst; results != 0U && value < ir->value_count; value++, results--) {
			if (state->skip_def[value] == 0U)
				state->skip_def[value] = index + 1U;
		}
	}

	/* Checks each region: from its SKIP_BEGIN to the next SKIP_END. */
	for (index = 0U; index < ir->instruction_count; index++) {
		if (ir->instructions[index].op != DRV_GPU_IR_SKIP_BEGIN)
			continue;

		/* Its end; a region without one is not skipped. */
		for (end = index + 1U; end < ir->instruction_count; end++) {
			if (ir->instructions[end].op == DRV_GPU_IR_SKIP_END)
				break;
			if (ir->instructions[end].op == DRV_GPU_IR_SKIP_BEGIN)
				break;
		}

		/* A region is a SKIP_BEGIN closed by its own SKIP_END, with nothing opened in between. */
		if (end >= ir->instruction_count || ir->instructions[end].op != DRV_GPU_IR_SKIP_END)
			continue;

		/* Skipped only when what leaves it is proven masked. */
		sound = i915_compile_skip_region(state, index, end);
		if (sound)
			state->skip_ok[index] = 1U;
	}

	/* The tables are needed no more. */
	kern_free(state->skip_garbage);
	state->skip_garbage = NULL;
}

/*
 * Reports whether the region from SKIP_BEGIN `begin` to SKIP_END `end` may be
 * skipped: when a thread skips it, no channel is in its predicate P, so a
 * channel where P holds does not exist, and a value the region would have
 * made may only be seen on such channels.
 *
 * Booleans are worked out as truth tables over the comparisons and other
 * Booleans they are made of (at most COMPILE_SKIP_ATOMS "atoms").  Each
 * value made in the region is garbage on every channel; each value made
 * from garbage records the table of the channels it may be garbage on (a
 * selection only where it takes the garbage, an AND only where the other
 * side is true, an OR only where it is false).  A value whose garbage is
 * confined to channels inside P is clean.  The region may not be skipped
 * when garbage outside P could reach an output, a storage or shared-memory
 * store, an atomic, a discard, a loop move, a loop end or another region's
 * predicate, or when its body has an effect that is not under P or a
 * workgroup barrier or a fence (every thread of the group must reach a
 * barrier, ws101-p006).  Returns 1 when it may be skipped.
 */
static int
i915_compile_skip_region(
	struct i915_compile_state *state,
	uint32_t begin,
	uint32_t end)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *inst;
	uint64_t inside[COMPILE_SKIP_WORDS];
	uint64_t garbage[COMPILE_SKIP_WORDS];
	uint64_t table[COMPILE_SKIP_WORDS];
	uint32_t index;
	uint32_t value;
	uint32_t results;
	uint32_t operands;
	int known;
	int clean;
	int word;

	/* The shader being compiled. */
	ir = state->ir;

	/* No atom yet, nothing garbage yet. */
	state->skip_atom_count = 0U;
	for (value = 0U; value < ir->value_count; value++)
		state->skip_taint[value] = 0U;

	/* The channels of P. */
	known = i915_compile_bool_table(state, ir->instructions[begin].src[0], inside, 0U);
	if (!known)
		return 0;

	/* The body: effects only under P, and every value it makes is garbage everywhere. */
	for (index = begin + 1U; index < end; index++) {
		inst = &ir->instructions[index];

		/*
		 * A loop, a loop move, an output write or a geometry shader's emit
		 * or end inside is not skipped over; nor a workgroup barrier, which
		 * every thread of the group must reach, nor a fence (ws101-p006).
		 */
		if (inst->op == DRV_GPU_IR_LOOP_BEGIN ||
		    inst->op == DRV_GPU_IR_LOOP_END ||
		    inst->op == DRV_GPU_IR_MOVE ||
		    inst->op == DRV_GPU_IR_STORE_OUTPUT ||
		    inst->op == DRV_GPU_IR_EMIT_VERTEX ||
		    inst->op == DRV_GPU_IR_END_PRIMITIVE ||
		    inst->op == DRV_GPU_IR_BARRIER ||
		    inst->op == DRV_GPU_IR_FENCE)
			return 0;

		/* A discard must be under P. */
		if (inst->op == DRV_GPU_IR_KILL) {
			known = i915_compile_bool_table(state, inst->src[0], table, 0U);
			if (!known)
				return 0;
			clean = i915_compile_table_within(table, inside);
			if (!clean)
				return 0;
		}

		/* A storage or shared-memory store must be predicated on something under P. */
		if (inst->op == DRV_GPU_IR_STORE_STORAGE || inst->op == DRV_GPU_IR_STORE_SHARED) {
			if (inst->component == 0U)
				return 0;
			known = i915_compile_bool_table(state, inst->src[2], table, 0U);
			if (!known)
				return 0;
			clean = i915_compile_table_within(table, inside);
			if (!clean)
				return 0;
		}

		/* So must an atomic, whose predicate is its last source (ws101-p002). */
		if (inst->op == DRV_GPU_IR_ATOMIC) {
			if (inst->component == 0U)
				return 0;
			operands = i915_compile_operands(inst);
			known = i915_compile_bool_table(state, inst->src[operands - 1U], table, 0U);
			if (!known)
				return 0;
			clean = i915_compile_table_within(table, inside);
			if (!clean)
				return 0;
		}

		/* Marks what it makes as garbage on every channel. */
		results = i915_compile_results(inst);
		for (value = inst->dst; results != 0U && value < ir->value_count; value++, results--) {
			state->skip_taint[value] = 1U;
			for (word = 0; word < (int)COMPILE_SKIP_WORDS; word++)
				state->skip_garbage[(size_t)value * COMPILE_SKIP_WORDS + (size_t)word] = ~(uint64_t)0;
		}
	}

	/* After it: follows the garbage in order. */
	for (index = end + 1U; index < ir->instruction_count; index++) {
		inst = &ir->instructions[index];

		/* Where the instruction's results may be garbage; nothing when it reads none. */
		known = i915_compile_skip_garbage(state, inst, garbage);
		results = i915_compile_results(inst);

		/* Garbage confined to P's channels does not exist: the results are clean. */
		clean = 1;
		if (known)
			clean = i915_compile_table_within(garbage, inside);

		/* Garbage that could be seen must not reach an effect or control. */
		if (!clean &&
		    (inst->op == DRV_GPU_IR_STORE_OUTPUT ||
		     inst->op == DRV_GPU_IR_EMIT_VERTEX ||
		     inst->op == DRV_GPU_IR_END_PRIMITIVE ||
		     inst->op == DRV_GPU_IR_STORE_STORAGE ||
		     inst->op == DRV_GPU_IR_STORE_SHARED ||
		     inst->op == DRV_GPU_IR_ATOMIC ||
		     inst->op == DRV_GPU_IR_KILL ||
		     inst->op == DRV_GPU_IR_MOVE ||
		     inst->op == DRV_GPU_IR_LOOP_END ||
		     inst->op == DRV_GPU_IR_SKIP_BEGIN))
			return 0;

		/* Records the results' garbage (a loop move may make a value clean again). */
		for (value = inst->dst; results != 0U && value < ir->value_count; value++, results--) {
			state->skip_taint[value] = 0U;
			if (clean)
				continue;
			state->skip_taint[value] = 1U;
			for (word = 0; word < (int)COMPILE_SKIP_WORDS; word++)
				state->skip_garbage[(size_t)value * COMPILE_SKIP_WORDS + (size_t)word] = garbage[word];
		}
	}

	/* Succeeded: nothing the region makes is seen where it could exist. */
	return 1;
}

/*
 * Works out where an instruction's results may be garbage, from the garbage
 * of what it reads.  Returns 0 when it reads no garbage (clean), 1 with the
 * table in `garbage` otherwise.
 */
static int
i915_compile_skip_garbage(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst,
	uint64_t garbage[COMPILE_SKIP_WORDS])
{
	uint64_t condition[COMPILE_SKIP_WORDS];
	uint64_t other[COMPILE_SKIP_WORDS];
	const uint64_t *first;
	const uint64_t *second;
	uint32_t sources;
	uint32_t source;
	uint32_t read;
	int tainted;
	int known;
	int word;

	/* Nothing garbage until a source is. */
	tainted = 0;
	for (word = 0; word < (int)COMPILE_SKIP_WORDS; word++)
		garbage[word] = 0U;

	/* A selection by a clean condition: the garbage of each side where it is taken. */
	if (inst->op == DRV_GPU_IR_SELECT && state->skip_taint[inst->src[0]] == 0U) {
		first = i915_compile_skip_value_garbage(state, inst->src[1]);
		second = i915_compile_skip_value_garbage(state, inst->src[2]);
		if (first == NULL && second == NULL)
			return 0;

		/* The condition's channels; an unknown condition takes either side anywhere. */
		known = i915_compile_bool_table(state, inst->src[0], condition, 0U);
		for (word = 0; word < (int)COMPILE_SKIP_WORDS; word++) {
			if (!known)
				condition[word] = ~(uint64_t)0;
			if (first != NULL)
				garbage[word] |= first[word] & condition[word];
			if (!known)
				condition[word] = 0U;
			if (second != NULL)
				garbage[word] |= second[word] & ~condition[word];
		}

		/* Garbage where the chosen side's is. */
		return 1;
	}

	/* An AND is garbage only where the other side is true; an OR only where it is false. */
	if ((inst->op == DRV_GPU_IR_AND || inst->op == DRV_GPU_IR_OR) &&
	    (state->skip_taint[inst->src[0]] == 0U || state->skip_taint[inst->src[1]] == 0U)) {
		first = i915_compile_skip_value_garbage(state, inst->src[0]);
		read = inst->src[1];
		if (first == NULL) {
			first = i915_compile_skip_value_garbage(state, inst->src[1]);
			read = inst->src[0];
		}

		/* Neither side garbage: the result is clean. */
		if (first == NULL)
			return 0;

		/* The clean side's channels; an unknown one leaves the garbage everywhere. */
		known = i915_compile_bool_table(state, read, other, 0U);
		for (word = 0; word < (int)COMPILE_SKIP_WORDS; word++) {
			if (!known) {
				garbage[word] = first[word];
			} else if (inst->op == DRV_GPU_IR_AND) {
				garbage[word] = first[word] & other[word];
			} else {
				garbage[word] = first[word] & ~other[word];
			}
		}

		/* Garbage where the garbage side decides. */
		return 1;
	}

	/* Anything else is garbage wherever a source is. */
	sources = i915_compile_sources(inst);
	for (source = 0U; source < sources; source++) {
		read = i915_compile_source(inst, source);
		first = i915_compile_skip_value_garbage(state, read);
		if (first == NULL)
			continue;
		tainted = 1;
		for (word = 0; word < (int)COMPILE_SKIP_WORDS; word++)
			garbage[word] |= first[word];
	}

	/* Reports whether it read garbage. */
	return tainted;
}

/* Returns the garbage table of a value, or NULL for a clean one (or one outside the IR). */
static const uint64_t *
i915_compile_skip_value_garbage(
	const struct i915_compile_state *state,
	uint32_t value)
{
	/* A value outside the IR, or a clean one, has no garbage. */
	if (value >= state->ir->value_count)
		return NULL;
	if (state->skip_taint[value] == 0U)
		return NULL;

	/* Succeeded: its table. */
	return &state->skip_garbage[(size_t)value * COMPILE_SKIP_WORDS];
}

/*
 * Works out the truth table of a clean Boolean over the atoms of the region
 * check: an AND, OR or NOT of Booleans is combined, a constant is all true
 * or all false, and any other Boolean is an atom (a new one while there is
 * room).  Returns 0 when the table cannot be made (too many atoms, too deep,
 * or a value outside the IR).
 */
static int
i915_compile_bool_table(
	struct i915_compile_state *state,
	uint32_t value,
	uint64_t table[COMPILE_SKIP_WORDS],
	uint32_t depth)
{
	const struct drv_gpu_shader_ir_inst *inst;
	uint64_t left[COMPILE_SKIP_WORDS];
	uint64_t right[COMPILE_SKIP_WORDS];
	uint32_t def;
	uint32_t atom;
	uint32_t entry;
	int known;
	int word;

	/* Too deep, or a value outside the IR. */
	if (depth >= COMPILE_IMPLIES_DEPTH || value >= state->ir->value_count)
		return 0;

	/* A Boolean made by an AND, OR, NOT or constant is combined. */
	def = state->skip_def[value];
	inst = NULL;
	if (def != 0U)
		inst = &state->ir->instructions[def - 1U];

	/* A constant: all true or all false. */
	if (inst != NULL && inst->op == DRV_GPU_IR_BOOL) {
		for (word = 0; word < (int)COMPILE_SKIP_WORDS; word++) {
			table[word] = 0U;
			if (inst->immediate != 0U)
				table[word] = ~(uint64_t)0;
		}

		/* The constant's table is known. */
		return 1;
	}

	/* NOT, AND and OR of their operands' tables. */
	if (inst != NULL && (inst->op == DRV_GPU_IR_NOT || inst->op == DRV_GPU_IR_AND || inst->op == DRV_GPU_IR_OR)) {
		known = i915_compile_bool_table(state, inst->src[0], left, depth + 1U);
		if (!known)
			return 0;
		if (inst->op != DRV_GPU_IR_NOT) {
			known = i915_compile_bool_table(state, inst->src[1], right, depth + 1U);
			if (!known)
				return 0;
		}

		/* Combined channel by channel. */
		for (word = 0; word < (int)COMPILE_SKIP_WORDS; word++) {
			if (inst->op == DRV_GPU_IR_NOT) {
				table[word] = ~left[word];
			} else if (inst->op == DRV_GPU_IR_AND) {
				table[word] = left[word] & right[word];
			} else {
				table[word] = left[word] | right[word];
			}
		}

		/* The combination's table is known. */
		return 1;
	}

	/* Any other Boolean is an atom: found, or added while there is room. */
	for (atom = 0U; atom < state->skip_atom_count; atom++) {
		if (state->skip_atoms[atom] == value)
			break;
	}

	/* A new atom takes the next slot; more than the tables hold is unknown. */
	if (atom == state->skip_atom_count) {
		if (state->skip_atom_count >= COMPILE_SKIP_ATOMS)
			return 0;
		state->skip_atoms[atom] = value;
		state->skip_atom_count++;
	}

	/* The atom is true on the table's entries whose bit `atom` is set. */
	for (word = 0; word < (int)COMPILE_SKIP_WORDS; word++)
		table[word] = 0U;
	for (entry = 0U; entry < 64U * COMPILE_SKIP_WORDS; entry++) {
		if (((entry >> atom) & 1U) != 0U)
			table[entry / 64U] |= (uint64_t)1 << (entry % 64U);
	}

	/* Succeeded: the atom's table. */
	return 1;
}

/* Reports whether every entry true in `table` is also true in `within`. */
static int
i915_compile_table_within(
	const uint64_t table[COMPILE_SKIP_WORDS],
	const uint64_t within[COMPILE_SKIP_WORDS])
{
	int word;

	/* An entry of the table outside `within` breaks it. */
	for (word = 0; word < (int)COMPILE_SKIP_WORDS; word++) {
		if ((table[word] & ~within[word]) != 0U)
			return 0;
	}

	/* Succeeded: the table is within. */
	return 1;
}

/*
 * Opens an IF on "any channel's `predicate` is true" and returns its
 * position: f0.0 set per channel, its sixteen bits copied into a temporary
 * register's dword, and f0.0 set again on every channel where that dword is
 * not zero.  Every channel the execution mask enables then runs the IF's body
 * or none does, so a body with derivatives (a sample, DDX) sees its whole
 * quad, and a thread with no channel in the predicate jumps to the ENDIF.
 */
static uint32_t
i915_compile_if_any(
	struct i915_compile_state *state,
	uint32_t predicate)
{
	struct i915_eu_reg null;
	struct i915_eu_reg word;
	uint32_t predicate_grf;
	uint32_t temporary;
	uint32_t position;

	/* f0.0 where the predicate is true (not zero); cleared first, since the comparison leaves the bits of inactive channels as they were. */
	predicate_grf = i915_compile_grf(state, predicate);
	drv_i915_eu_flag_clear(&state->code, I915_EU_FLAG_F0_0);
	null = drv_i915_eu_null();
	null.type = COMPILE_TYPE_D;
	drv_i915_eu_cmp(&state->code,
			I915_EU_COND_NE,
			I915_EU_FLAG_F0_0,
			0,
			null,
			drv_i915_eu_grf_d(predicate_grf),
			drv_i915_eu_imm_d(0U));

	/* The flag's bits as one dword, then f0.0 on every channel when any bit is set. */
	temporary = i915_compile_temporary(state);
	drv_i915_eu_flag_store(&state->code, I915_EU_FLAG_F0_0, temporary, 0U);
	word = drv_i915_eu_grf_scalar(temporary, 0U);
	word.type = EU_TYPE_UD;
	null.type = EU_TYPE_UD;
	drv_i915_eu_cmp(&state->code,
			I915_EU_COND_NE,
			I915_EU_FLAG_F0_0,
			0,
			null,
			word,
			drv_i915_eu_imm_ud(0U));
	state->grf_busy[temporary] = 0U;

	/* The IF on it. */
	position = drv_i915_eu_if(&state->code, I915_EU_FLAG_F0_0);

	/* Succeeded: the IF waits for its ENDIF. */
	return position;
}

/*
 * Lowers a SKIP_BEGIN: for a region that is really skipped (while there is
 * room to remember its ENDIF, and long enough to be worth it), an IF every
 * channel takes when any channel is in the predicate (i915_compile_if_any());
 * the region's body then runs for every channel, as it would without the IF,
 * and a thread none of whose channels is in it jumps to the ENDIF.
 */
static void
i915_compile_skip_begin(
	struct i915_compile_state *state,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t length;

	/* A region the check did not prove, or one past the room for ENDIFs, runs as it is. */
	if (state->skip_ok[state->index] == 0U)
		return;
	if (state->guards >= COMPILE_MAX_GUARDS || state->skip_active)
		return;

	/* So does one too short to be worth its IF. */
	length = 0U;
	while (state->index + 1U + length < state->ir->instruction_count &&
	       state->ir->instructions[state->index + 1U + length].op != DRV_GPU_IR_SKIP_END)
		length++;
	if (length < COMPILE_SKIP_MIN_INSTRUCTIONS)
		return;

	/* Opens an IF that every channel takes when any is in the predicate. */
	state->skip_if = i915_compile_if_any(state, inst->src[0]);

	/* The region is being skipped; it counts against the ENDIFs' room. */
	state->skip_active = 1;
	state->guards++;
}

/* Lowers a SKIP_END: the ENDIF of a skipped region, its IF pointed at it, and an ENDIF in a loop kept for the WHILE. */
static void
i915_compile_skip_end(
	struct i915_compile_state *state)
{
	uint32_t endif_position;

	/* A region that runs as it is has no ENDIF. */
	if (!state->skip_active)
		return;

	/* Closes the IF and points it at its ENDIF. */
	endif_position = drv_i915_eu_endif(&state->code);
	drv_i915_eu_patch_if(&state->code, state->skip_if, endif_position);
	state->skip_active = 0;

	/* An ENDIF inside a loop is pointed at the loop's WHILE when it is placed. */
	if (state->loop_depth != 0U) {
		state->endif_position[state->endif_count] = endif_position;
		state->endif_depth[state->endif_count] = state->loop_depth;
		state->endif_count++;
	}
}

/* Reports whether the IR input of a location is Flat. */
static int
i915_compile_input_flat(
	const struct i915_compile_state *state,
	uint32_t location)
{
	uint32_t index;

	/* The input of the location. */
	for (index = 0U; index < state->ir->input_count; index++) {
		if (state->ir->inputs[index].location == location && state->ir->inputs[index].flat != 0U)
			return 1;
	}

	/* Interpolated. */
	return 0;
}

/* Reports whether the IR input of a location is interpolated without perspective. */
static int
i915_compile_input_linear(
	const struct i915_compile_state *state,
	uint32_t location)
{
	uint32_t index;

	/* The input of the location. */
	for (index = 0U; index < state->ir->input_count; index++) {
		if (state->ir->inputs[index].location == location && state->ir->inputs[index].noperspective != 0U)
			return 1;
	}

	/* With perspective. */
	return 0;
}

/* Returns the Flat inputs of a kernel: bit n for its n-th input in payload order whose IR input is Flat. */
static uint32_t
i915_compile_flat_mask(
	const struct i915_compile_state *state)
{
	uint32_t mask;
	uint32_t rank;
	uint32_t index;

	/* Each input the kernel reads, by the IR input of its location. */
	mask = 0U;
	for (rank = 0U; rank < state->input_count; rank++) {
		for (index = 0U; index < state->ir->input_count; index++) {
			if (state->ir->inputs[index].location == state->inputs[rank] && state->ir->inputs[index].flat != 0U)
				mask |= 1U << rank;
		}
	}

	/* Succeeded: the mask. */
	return mask;
}
/* Finds the position of `location` in an ascending list. */
static int
i915_compile_rank(
	const uint32_t *list,
	uint32_t count,
	uint32_t location,
	uint32_t *rank)
{
	uint32_t index;

	/* Looks the location up. */
	for (index = 0U; index < count; index++) {
		if (list[index] == location) {
			/* Succeeded: the location is the index-th in the list. */
			*rank = index;
			return 0;
		}
	}

	/* The location is not in the list. */
	return ENOENT;
}

/* Adds `location` to an ascending list without duplicates; more than `limit` entries is refused. */
static void
i915_compile_note(
	struct i915_compile_state *state,
	uint32_t *list,
	uint32_t *count,
	uint32_t limit,
	uint32_t location)
{
	uint32_t index;
	uint32_t at;

	/* Finds where the location belongs in ascending order. */
	for (at = 0U; at < *count && list[at] < location; at++)
		;

	/* A location already listed is not listed twice. */
	if (at < *count && list[at] == location)
		return;

	/* A list that is full refuses the shader. */
	if (*count >= limit) {
		state->unsupported = 1;
		return;
	}

	/* Shifts the larger locations up to open the place. */
	for (index = *count; index > at; index--)
		list[index] = list[index - 1U];

	/* Inserts the location. */
	list[at] = location;
	(*count)++;
}

/* Reads the interface off the instructions: the locations read, the locations written, the push data. */
static void
i915_compile_interface(
	struct i915_compile_state *state)
{
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t payload_end;
	uint32_t index;
	uint32_t vue_slots;
	int linear;

	/* Lays out the push data: the push constants, then the uniform blocks. */
	i915_compile_blocks(state);

	/* Lists every input read and, for a vertex shader, every varying written; notes a discard. */
	for (index = 0U; index < state->ir->instruction_count; index++) {
		inst = &state->ir->instructions[index];
		if (inst->op == DRV_GPU_IR_LOAD_INPUT && inst->location == DRV_GPU_SHADER_LOCATION_FRONT_FACING) {
			/* The facing bit is in the payload's fixed registers, not an input of its own. */
			continue;
		} else if (inst->op == DRV_GPU_IR_LOAD_INPUT && inst->location == DRV_GPU_SHADER_LOCATION_FRAG_COORD) {
			/* The pixel position needs no input; its depth and w come in the payload when read. */
			if (inst->component == 2U)
				state->uses_depth = 1;
			if (inst->component == 3U)
				state->uses_w = 1;
		} else if (inst->op == DRV_GPU_IR_LOAD_INPUT) {
			/* An input, which the linear barycentrics interpolate when it has no perspective. */
			i915_compile_note(state, state->inputs, &state->input_count, COMPILE_MAX_INPUTS, inst->location);
			linear = i915_compile_input_linear(state, inst->location);
			if (linear != 0 && state->ir->stage == DRV_GPU_STAGE_FRAGMENT)
				state->uses_linear = 1;
		} else if (inst->op == DRV_GPU_IR_STORE_OUTPUT && inst->location == DRV_GPU_IR_LOCATION_POINT_SIZE) {
			/* The point size is in the VUE header, not a varying of its own. */
			state->writes_point_size = 1;
		} else if (inst->op == DRV_GPU_IR_STORE_OUTPUT && inst->location == DRV_GPU_IR_LOCATION_LAYER) {
			/* A geometry shader's layer is in the VUE header, not a varying of its own. */
			continue;
		} else if (inst->op == DRV_GPU_IR_STORE_OUTPUT &&
		    (state->ir->stage == DRV_GPU_STAGE_VERTEX || state->ir->stage == DRV_GPU_STAGE_GEOMETRY) &&
		    inst->location != DRV_GPU_IR_LOCATION_POSITION) {
			i915_compile_note(state, state->varyings, &state->varying_count, COMPILE_MAX_VARYINGS, inst->location);
		} else if (inst->op == DRV_GPU_IR_STORE_OUTPUT && inst->location == DRV_GPU_IR_LOCATION_SECOND_COLOR) {
			/* The second colour of a dual-source write, which goes with location 0's. */
			state->uses_second_color = 1;
		} else if (inst->op == DRV_GPU_IR_STORE_OUTPUT && inst->location < I915_SHADER_MAX_COLOR_OUTPUTS) {
			/* A fragment shader's colour location gets a render-target write. */
			state->fs_outputs |= 1U << inst->location;
		} else if (inst->op == DRV_GPU_IR_KILL) {
			state->uses_kill = 1;
		}
	}

	/* A discard belongs to a fragment shader. */
	if (state->uses_kill != 0 && state->ir->stage != DRV_GPU_STAGE_FRAGMENT)
		state->unsupported = 1;

	/* A second colour goes with location 0's alone (Vulkan: dual source writes attachment 0; Mesa needs both). */
	if (state->uses_second_color != 0 && (state->ir->stage != DRV_GPU_STAGE_FRAGMENT || state->fs_outputs != 1U))
		state->unsupported = 1;

	/* A fragment payload carries, after the perspective barycentrics, what the kernel asked for, in the order of the conventions. */
	if (state->ir->stage == DRV_GPU_STAGE_FRAGMENT)
		i915_compile_fragment_payload(state);

	/* A compute thread's payload is its header, its push data and its per-thread IDs. */
	if (state->ir->stage == DRV_GPU_STAGE_COMPUTE)
		i915_compile_compute_interface(state);

	/* A geometry thread's payload is its header, its handles and its push data; its output is a URB entry. */
	if (state->ir->stage == DRV_GPU_STAGE_GEOMETRY)
		i915_compile_geometry_interface(state);

	/*
	 * Finds where the payload ends: four registers to an attribute, two to
	 * an interpolated input, the IDs of a compute thread, nothing after a
	 * geometry thread's push data (it reads its inputs from the URB).
	 */
	if (state->ir->stage == DRV_GPU_STAGE_VERTEX) {
		payload_end = COMPILE_PAYLOAD_GRF + state->push_regs + 4U * state->input_count;
	} else if (state->ir->stage == DRV_GPU_STAGE_COMPUTE) {
		payload_end = COMPILE_CS_PUSH_GRF + state->push_regs + I915_SHADER_PER_THREAD_REGS;
	} else if (state->ir->stage == DRV_GPU_STAGE_GEOMETRY) {
		payload_end = state->gs_push_grf + state->push_regs;
	} else {
		payload_end = state->fs_setup_grf + state->push_regs + 2U * state->input_count;
	}

	/*
	 * The temporary and the values follow the payload: at r15 and r16 when it
	 * ends before r15, right after it otherwise.
	 */
	if (payload_end > COMPILE_SCRATCH_GRF) {
		state->scratch_grf = payload_end;
		state->first_value_grf = payload_end + 1U;
		state->grf_high = state->first_value_grf;
	}

	/*
	 * A geometry shader keeps the count of the vertices it emitted and its
	 * cut bits in the first two value registers for its whole run: they are
	 * not IR values, so no spill ever moves them.
	 */
	if (state->ir->stage == DRV_GPU_STAGE_GEOMETRY) {
		state->gs_count_grf = state->first_value_grf;
		state->gs_cut_grf = state->first_value_grf + 1U;
		state->first_value_grf += 2U;
		state->grf_high = state->first_value_grf;
	}

	/*
	 * A vertex shader stages its VUE below r127, and the values stay below
	 * it; a gathered VUE keeps only the window of its writes from them.
	 */
	if (state->ir->stage == DRV_GPU_STAGE_VERTEX && state->late_vue == 0) {
		vue_slots = 2U + state->varying_count;
		state->vue_grf = COMPILE_MAX_GRF - 4U * vue_slots;
		if (state->vue_grf <= state->last_value_grf)
			state->last_value_grf = state->vue_grf - 1U;
	} else if (state->ir->stage == DRV_GPU_STAGE_GEOMETRY) {
		/*
		 * A geometry shader stages its VUE as a vertex shader does, and keeps
		 * the two registers below it for the header of its vertex writes: the
		 * output handles, and the per-slot offsets of the vertex.
		 */
		vue_slots = 2U + state->varying_count;
		state->vue_grf = COMPILE_MAX_GRF - 4U * vue_slots;
		if (state->vue_grf - COMPILE_GS_WRITE_HEADER_REGS <= state->last_value_grf)
			state->last_value_grf = state->vue_grf - COMPILE_GS_WRITE_HEADER_REGS - 1U;
	} else if (state->ir->stage == DRV_GPU_STAGE_VERTEX) {
		if (COMPILE_GATHER_GRF <= state->last_value_grf)
			state->last_value_grf = COMPILE_GATHER_GRF - 1U;
	}

	/* A shader that spills gives its highest value register to the scratch header. */
	if (state->spill_count != 0U && state->first_value_grf < state->last_value_grf) {
		state->header_grf = state->last_value_grf;
		state->last_value_grf--;
	}

	/* Scratch memory beyond what 3DSTATE_VS / PS describe is refused. */
	if (state->spill_count > COMPILE_SCRATCH_MAX_BYTES / COMPILE_SLOT_BYTES)
		state->unsupported = 1;

	/*
	 * A payload that leaves no value register runs out of registers: a
	 * vertex shader then gathers its VUE, anything else is refused (there
	 * is no value to move).
	 */
	if (state->first_value_grf > state->last_value_grf)
		state->out_of_registers = 1;
	if (state->spill_count != 0U && state->header_grf == COMPILE_NO_GRF)
		state->out_of_registers = 1;
}

/*
 * Lays out the fragment payload after the perspective barycentrics: the
 * linear barycentrics, the source depth and the source w, each only when
 * the kernel reads it, in that order (brw_fs_thread_payload.cpp,
 * setup_fs_payload_gfx9()), then the push data and the input planes.
 */
static void
i915_compile_fragment_payload(
	struct i915_compile_state *state)
{
	uint32_t next;

	/* Starts after the perspective barycentrics. */
	next = COMPILE_FS_SETUP_GRF;

	/* The two linear barycentrics of each pixel. */
	if (state->uses_linear != 0) {
		state->fs_linear_grf = next;
		next += 2U;
	}

	/* The interpolated depth of each pixel. */
	if (state->uses_depth != 0) {
		state->fs_depth_grf = next;
		next++;
	}

	/* The interpolated w of each pixel. */
	if (state->uses_w != 0) {
		state->fs_w_grf = next;
		next++;
	}

	/* The push data and the planes follow. */
	state->fs_setup_grf = next;
}

/*
 * Lays out the push data: the push constants in whole registers, then the
 * range of each uniform block the shader reads, from the 32-byte boundary
 * below its first byte to the one above its last.
 */
static void
i915_compile_blocks(
	struct i915_compile_state *state)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	uint32_t index;
	uint32_t first;
	uint32_t end;
	uint32_t bytes;

	/* The push constants take whole registers of 32 bytes. */
	state->push_constant_regs = (state->ir->push_bytes + 31U) / 32U;
	bytes = state->push_constant_regs * 32U;

	/* Places each uniform block that is read, and each storage buffer's address, after what comes before it. */
	for (index = 0U; index < state->ir->uniform_count; index++) {
		uniform = &state->ir->uniforms[index];
		if (uniform->kind != DRV_GPU_IR_UNIFORM_STORAGE &&
		    (uniform->kind != DRV_GPU_IR_UNIFORM_BLOCK || uniform->size == 0U))
			continue;

		/* More blocks than a binary describes are refused. */
		if (state->block_count >= I915_SHADER_MAX_BLOCKS) {
			state->unsupported = 1;
			return;
		}

		/* The block's range, widened to whole registers; a storage buffer's address takes one register. */
		first = uniform->offset & ~31U;
		end = (uniform->offset + uniform->size + 31U) & ~31U;
		state->block_address[state->block_count] = 0U;
		if (uniform->kind == DRV_GPU_IR_UNIFORM_STORAGE) {
			first = 0U;
			end = 32U;
			state->block_address[state->block_count] = 1U;
		}

		/* Records where the range lands in the push data. */
		state->block_uniform[state->block_count] = index;
		state->block_first_byte[state->block_count] = first;
		state->block_bytes[state->block_count] = end - first;
		state->block_push_offset[state->block_count] = bytes;
		state->block_count++;
		bytes += end - first;
	}

	/* The push data takes whole registers. */
	state->push_regs = bytes / 32U;
}

/* Fills what the outputs hold where the shader stores nothing: zeros, and the VUE header. */
static void
i915_compile_prologue(
	struct i915_compile_state *state)
{
	struct i915_eu_reg header;
	uint32_t outputs;
	uint32_t grf;

	/* A shader that spills builds its scratch header before anything else. */
	if (state->header_grf != COMPILE_NO_GRF)
		i915_compile_scratch_header(state);

	/* A compute shader has no output to fill: it writes memory only. */
	if (state->ir->stage == DRV_GPU_STAGE_COMPUTE)
		return;

	/* A geometry shader prepares its vertex writes, then stages its VUE as a vertex shader does. */
	if (state->ir->stage == DRV_GPU_STAGE_GEOMETRY)
		i915_compile_geometry_prologue(state);

	/* A fragment shader's colours start as zeros. */
	if (state->ir->stage == DRV_GPU_STAGE_FRAGMENT) {
		outputs = i915_compile_fs_outputs(state);
		if (state->uses_second_color != 0)
			outputs |= 1U << 1;
		for (grf = COMPILE_MAX_GRF - 4U * I915_SHADER_MAX_COLOR_OUTPUTS + 1U; grf <= COMPILE_MAX_GRF; grf++) {
			/* Only the registers of a location the shader writes. */
			if ((outputs & (1U << ((COMPILE_MAX_GRF - grf) / 4U))) == 0U)
				continue;
			drv_i915_eu_mov(&state->code, drv_i915_eu_grf(grf), drv_i915_eu_imm_f(0U));
		}

		/* One that discards starts with every dispatched pixel live in f1.0. */
		if (state->uses_kill != 0)
			drv_i915_eu_flag_load(&state->code, I915_EU_FLAG_F1_0, COMPILE_FS_DISPATCH_GRF, COMPILE_FS_DISPATCH_BYTE);
		return;
	}

	/* A gathered VUE is written whole at the end; nothing is staged. */
	if (state->late_vue != 0)
		return;

	/* The header (point size, layer, viewport index) is integer zeros. */
	for (grf = state->vue_grf; grf < state->vue_grf + 4U; grf++) {
		header = drv_i915_eu_grf_ud(grf);
		header.type = COMPILE_TYPE_D;
		drv_i915_eu_mov(&state->code, header, drv_i915_eu_imm_d(0U));
	}

	/* The position and the varyings start as zeros. */
	for (grf = state->vue_grf + 4U; grf < COMPILE_MAX_GRF; grf++)
		drv_i915_eu_mov(&state->code, drv_i915_eu_grf(grf), drv_i915_eu_imm_f(0U));
}

/* Emits the shader's terminating output message. */
static void
i915_compile_terminate(
	struct i915_compile_state *state)
{
	struct i915_eu_buf *code;
	struct i915_eu_reg payload;
	uint32_t descriptor;
	uint32_t location;
	uint32_t outputs;
	int last;

	/* Emits into the shader's encoder buffer. */
	code = &state->code;

	/* A compute thread only retires. */
	if (state->ir->stage == DRV_GPU_STAGE_COMPUTE) {
		i915_compile_terminate_compute(state);
		return;
	}

	/* A geometry thread writes how many vertices it emitted and retires. */
	if (state->ir->stage == DRV_GPU_STAGE_GEOMETRY) {
		i915_compile_terminate_geometry(state);
		return;
	}

	/* A vertex shader writes its VUE, staged or gathered. */
	if (state->ir->stage == DRV_GPU_STAGE_VERTEX && state->late_vue != 0) {
		i915_compile_terminate_gathered(state);
		return;
	}

	/* Selects the vertex or fragment termination message for this kernel. */
	if (state->ir->stage == DRV_GPU_STAGE_VERTEX) {
		i915_compile_terminate_vertex(state);
		return;
	}

	/* Both colours of a dual-source blend in one write to render target 0, which ends the thread. */
	if (state->uses_second_color != 0) {
		i915_compile_terminate_dual(state);
		return;
	}

	/* Each colour location to its render target, in ascending order; the last write ends the thread. */
	outputs = i915_compile_fs_outputs(state);
	for (location = 0U; location < I915_SHADER_MAX_COLOR_OUTPUTS; location++) {
		if ((outputs & (1U << location)) == 0U)
			continue;

		/* The target's entry and index; only the last write names the last target and ends the thread. */
		last = 0;
		if ((outputs >> (location + 1U)) == 0U)
			last = 1;
		descriptor = (COMPILE_DESC_RT_WRITE & ~COMPILE_DESC_RT_LAST) | I915_SHADER_RT_BTI(location);
		if (last != 0)
			descriptor |= COMPILE_DESC_RT_LAST;
		payload = drv_i915_eu_grf(COMPILE_MAX_GRF - 3U - 4U * location);

		/* A shader that discards writes the colour only to the pixels f1.0 still holds. */
		if (state->uses_kill != 0) {
			drv_i915_eu_send_masked(code,
						I915_EU_FLAG_F1_0,
						drv_i915_eu_null(),
						payload,
						drv_i915_eu_null(),
						COMPILE_SFID_RENDER_CACHE,
						descriptor,
						COMPILE_EX_RT_INDEX(location),
						1,
						last);
			continue;
		}

		/* The colour to the render target: SENDC, as a render-target write must be. */
		drv_i915_eu_send(code,
				 drv_i915_eu_null(),
				 payload,
				 drv_i915_eu_null(),
				 COMPILE_SFID_RENDER_CACHE,
				 descriptor,
				 COMPILE_EX_RT_INDEX(location),
				 1,
				 last);
	}
}

/*
 * Writes a dual-source fragment shader's two colours with one SIMD8
 * dual-source render-target write to render target 0, which ends the
 * thread: the first colour (location 0, r124..r127) as src0, the second
 * (r120..r123) as src1 of a split send, as Mesa's lower_fb_write_logical_send()
 * and its split of the payload do on Gen12 (no header).  A shader that
 * discards writes only the pixels f1.0 still holds.
 */
static void
i915_compile_terminate_dual(
	struct i915_compile_state *state)
{
	struct i915_eu_buf *code;
	uint32_t descriptor;
	uint32_t extended;

	/* The write's descriptors: target 0's entry and index, both colours' lengths, the last target. */
	code = &state->code;
	descriptor = (COMPILE_DESC_RT_WRITE_DUAL & ~COMPILE_DESC_RT_LAST) | I915_SHADER_RT_BTI(0U) | COMPILE_DESC_RT_LAST;
	extended = COMPILE_EX_MLEN(4U) | COMPILE_EX_RT_INDEX(0U);

	/* Only the live pixels of a shader that discards. */
	if (state->uses_kill != 0) {
		drv_i915_eu_send_masked(code,
					I915_EU_FLAG_F1_0,
					drv_i915_eu_null(),
					drv_i915_eu_grf(COMPILE_MAX_GRF - 3U),
					drv_i915_eu_grf(COMPILE_MAX_GRF - 3U - 4U),
					COMPILE_SFID_RENDER_CACHE,
					descriptor,
					extended,
					1,
					1);
		return;
	}

	/* Succeeded: both colours to the render target, SENDC as a render-target write must be. */
	drv_i915_eu_send(code,
			 drv_i915_eu_null(),
			 drv_i915_eu_grf(COMPILE_MAX_GRF - 3U),
			 drv_i915_eu_grf(COMPILE_MAX_GRF - 3U - 4U),
			 COMPILE_SFID_RENDER_CACHE,
			 descriptor,
			 extended,
			 1,
			 1);
}

/*
 * Reports the colour locations a fragment shader writes: the ones it
 * stores, or location 0 alone for a shader that stores none (its thread
 * still ends with a render-target write, of zeros).
 */
static uint32_t
i915_compile_fs_outputs(
	const struct i915_compile_state *state)
{
	/* A shader with no colour store writes zeros to location 0. */
	if (state->fs_outputs == 0U)
		return 1U;

	/* Succeeded: the locations the shader stores. */
	return state->fs_outputs;
}

/*
 * Writes the staged VUE, two slots at a time from its end: every write but
 * the last takes the handles from r1; the last, which ends the thread,
 * takes them from r127 and its two slots from r119..r126.
 */
static void
i915_compile_terminate_vertex(
	struct i915_compile_state *state)
{
	struct i915_eu_buf *code;
	uint32_t slots;
	uint32_t first;
	uint32_t count;

	/* Emits into the shader's encoder buffer. */
	code = &state->code;

	/* The slots before the last two go out first, one write to a pair, a lone first slot on its own. */
	slots = 2U + state->varying_count;
	first = 0U;
	while (first + COMPILE_URB_WRITE_SLOTS < slots) {
		/* The first write takes one slot when the slots before the last pair are odd. */
		count = COMPILE_URB_WRITE_SLOTS;
		if (((slots - COMPILE_URB_WRITE_SLOTS - first) % COMPILE_URB_WRITE_SLOTS) != 0U)
			count = 1U;

		/* Writes the slots from their staging registers with the handles from r1. */
		drv_i915_eu_send(code,
				 drv_i915_eu_null(),
				 drv_i915_eu_grf(1U),
				 drv_i915_eu_grf(state->vue_grf + 4U * first),
				 COMPILE_SFID_URB,
				 COMPILE_DESC_URB_WRITE(first),
				 COMPILE_EX_MLEN(4U * count),
				 0,
				 0);
		first += count;
	}

	/* Writes the last two slots and ends the thread: handles and payload in r112..r127. */
	drv_i915_eu_mov(code, drv_i915_eu_grf_ud(COMPILE_MAX_GRF), drv_i915_eu_grf_ud(1U));
	drv_i915_eu_send(code,
			 drv_i915_eu_null(),
			 drv_i915_eu_grf(COMPILE_MAX_GRF),
			 drv_i915_eu_grf(state->vue_grf + 4U * first),
			 COMPILE_SFID_URB,
			 COMPILE_DESC_URB_WRITE(first),
			 COMPILE_EX_MLEN(4U * (slots - first)),
			 0,
			 1);
}

/*
 * Writes a gathered VUE, two slots at a time from its start as the staged
 * one is: each write's slots are gathered into r119 on (every write but the
 * last takes the handles from r1); the last, which ends the thread, takes
 * them from r127 and its two slots from r119..r126.
 */
static void
i915_compile_terminate_gathered(
	struct i915_compile_state *state)
{
	struct i915_eu_buf *code;
	uint32_t slots;
	uint32_t first;
	uint32_t count;

	/* Emits into the shader's encoder buffer. */
	code = &state->code;

	/* The slots before the last two go out first, one write to a pair, a lone first slot on its own. */
	slots = 2U + state->varying_count;
	first = 0U;
	while (first + COMPILE_URB_WRITE_SLOTS < slots) {
		/* The first write takes one slot when the slots before the last pair are odd. */
		count = COMPILE_URB_WRITE_SLOTS;
		if (((slots - COMPILE_URB_WRITE_SLOTS - first) % COMPILE_URB_WRITE_SLOTS) != 0U)
			count = 1U;

		/* Gathers the slots and writes them with the handles from r1. */
		i915_compile_gather(state, first, count);
		drv_i915_eu_send(code,
				 drv_i915_eu_null(),
				 drv_i915_eu_grf(1U),
				 drv_i915_eu_grf(COMPILE_GATHER_GRF),
				 COMPILE_SFID_URB,
				 COMPILE_DESC_URB_WRITE(first),
				 COMPILE_EX_MLEN(4U * count),
				 0,
				 0);
		first += count;
	}

	/* Gathers the last two slots and ends the thread: handles and payload in r112..r127. */
	i915_compile_gather(state, first, slots - first);
	drv_i915_eu_mov(code, drv_i915_eu_grf_ud(COMPILE_MAX_GRF), drv_i915_eu_grf_ud(1U));
	drv_i915_eu_send(code,
			 drv_i915_eu_null(),
			 drv_i915_eu_grf(COMPILE_MAX_GRF),
			 drv_i915_eu_grf(COMPILE_GATHER_GRF),
			 COMPILE_SFID_URB,
			 COMPILE_DESC_URB_WRITE(first),
			 COMPILE_EX_MLEN(4U * (slots - first)),
			 0,
			 1);
}

/*
 * Gathers `count` VUE slots from slot `first` into r119 on: the header as
 * integer zeros, every other component the value it last stored, or zero.
 */
static void
i915_compile_gather(
	struct i915_compile_state *state,
	uint32_t first,
	uint32_t count)
{
	struct i915_eu_reg header;
	uint32_t slot;
	uint32_t component;
	uint32_t value;
	uint32_t grf;

	/* Fills each component of each slot. */
	for (slot = first; slot < first + count; slot++) {
		for (component = 0U; component < 4U; component++) {
			grf = COMPILE_GATHER_GRF + 4U * (slot - first) + component;

			/* The header (layer, viewport index, and a point size never stored) is integer zeros. */
			value = state->output_value[4U * slot + component];
			if (slot == 0U && value == COMPILE_NO_VALUE) {
				header = drv_i915_eu_grf_ud(grf);
				header.type = COMPILE_TYPE_D;
				drv_i915_eu_mov(&state->code, header, drv_i915_eu_imm_d(0U));
				continue;
			}

			/* A component never stored is zero, as a staged one starts. */
			if (value == COMPILE_NO_VALUE) {
				drv_i915_eu_mov(&state->code, drv_i915_eu_grf(grf), drv_i915_eu_imm_f(0U));
				continue;
			}

			/* Anything else is the value it last stored. */
			i915_compile_read_into(state, value, grf);
		}
	}
}

/* Moves a value into a given register: a move from its register, or a read straight from scratch memory. */
static void
i915_compile_read_into(
	struct i915_compile_state *state,
	uint32_t value,
	uint32_t grf)
{
	/* A value outside the IR has no register. */
	if (value >= state->ir->value_count) {
		state->error = 1;
		return;
	}

	/* A value in scratch memory is read into the register itself. */
	if (state->spill_slot[value] != 0U) {
		i915_compile_scratch_offset(state, value);
		drv_i915_eu_send_all(&state->code,
				     drv_i915_eu_grf_ud(grf),
				     drv_i915_eu_grf_ud(state->header_grf),
				     drv_i915_eu_null(),
				     EU_SFID_DATA_CACHE,
				     COMPILE_DESC_SCRATCH_READ,
				     0U);
		return;
	}

	/* A value never defined has no register to move from. */
	if (state->value_grf[value] == COMPILE_NO_GRF) {
		state->error = 1;
		return;
	}

	/* Moves the bits. */
	drv_i915_eu_mov(&state->code, drv_i915_eu_grf_ud(grf), drv_i915_eu_grf_ud(state->value_grf[value]));
}

/* Records in the binary what a draw has to program around the kernel. */
static void
i915_compile_describe(
	const struct i915_compile_state *state,
	struct i915_shader_binary *binary)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	uint32_t index;
	uint32_t block;

	/* The registers, the thread count and the push data. */
	binary->grf_used = state->grf_high;
	binary->thread_count = 1U;

	/* The per-thread scratch space of a kernel that spills: its slots, rounded up to a power of two from 1 KiB. */
	if (state->spill_count != 0U) {
		binary->scratch_bytes = COMPILE_SCRATCH_MIN_BYTES;
		while (binary->scratch_bytes < state->spill_count * COMPILE_SLOT_BYTES)
			binary->scratch_bytes *= 2U;
	}

	/* Publishes the push-register layout consumed by the draw state. */
	binary->push_regs = state->push_regs;
	binary->push_constant_bytes = state->push_constant_regs * 32U;

	/* The inputs in payload order, and which of them are Flat. */
	binary->input_count = state->input_count;
	kern_memcpy(binary->input_locations, state->inputs, sizeof(state->inputs));
	binary->input_flat_mask = i915_compile_flat_mask(state);

	/* The sampled images in the order the kernel numbers them. */
	for (index = 0U; index < state->ir->uniform_count; index++) {
		uniform = &state->ir->uniforms[index];
		if (uniform->kind != DRV_GPU_IR_UNIFORM_SAMPLED_IMAGE)
			continue;
		if (binary->sampler_count >= I915_SHADER_MAX_SAMPLERS)
			break;
		binary->sampler_set[binary->sampler_count] = uniform->set;
		binary->sampler_binding[binary->sampler_count] = uniform->binding;
		binary->sampler_count++;
	}

	/* The uniform blocks and where they land in the push data. */
	binary->block_count = state->block_count;
	for (block = 0U; block < state->block_count; block++) {
		uniform = &state->ir->uniforms[state->block_uniform[block]];
		binary->blocks[block].set = uniform->set;
		binary->blocks[block].binding = uniform->binding;
		binary->blocks[block].offset = state->block_first_byte[block];
		binary->blocks[block].bytes = state->block_bytes[block];
		binary->blocks[block].push_offset = state->block_push_offset[block];
		binary->blocks[block].address = state->block_address[block];
	}

	/* A fragment kernel that discards has the draw say so. */
	if (state->uses_kill != 0)
		binary->uses_kill = 1U;

	/* A fragment kernel that writes dual source has the blend take its second colour. */
	if (state->uses_second_color != 0)
		binary->dual_source = 1U;

	/* A vertex kernel that writes the point size has the setup read it from the VUE. */
	if (state->writes_point_size != 0)
		binary->writes_point_size = 1U;

	/*
	 * A vertex shader passes its varyings on; a compute shader has a
	 * workgroup; a geometry shader its URB entry and its varyings; a fragment
	 * shader's varyings are its inputs.
	 */
	if (state->ir->stage == DRV_GPU_STAGE_VERTEX) {
		binary->varying_count = state->varying_count;
		kern_memcpy(binary->varying_locations, state->varyings, sizeof(state->varyings));
		binary->dispatch_grf_start = COMPILE_PAYLOAD_GRF;
	} else if (state->ir->stage == DRV_GPU_STAGE_COMPUTE) {
		i915_compile_describe_compute(state, binary);
	} else if (state->ir->stage == DRV_GPU_STAGE_GEOMETRY) {
		i915_compile_describe_geometry(state, binary);
	} else {
		binary->varying_count = state->input_count;
		binary->dispatch_grf_start = state->fs_setup_grf;
	}

	/* A fragment kernel has the draw put in its payload what it reads beyond the perspective barycentrics. */
	if (state->uses_linear != 0)
		binary->uses_linear_barycentrics = 1U;
	if (state->uses_depth != 0)
		binary->uses_source_depth = 1U;
	if (state->uses_w != 0)
		binary->uses_source_w = 1U;
}

/* The compute part of the code generator (ws101-p002). */
#include "compile-compute.inc"

/* The geometry part of the code generator (ws075-p007a). */
#include "compile-geometry.inc"
