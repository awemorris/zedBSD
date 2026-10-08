/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host fixture for the baseline shader compiler (p006). Parses the vkdemo
 * shaders, lowers them to Gen12 GEN code and checks the binary is non-empty,
 * one register per value, and carries the expected instruction kinds. Semantic
 * correctness on hardware is the big-bang test; this verifies the lowering.
 */

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned fixture_live;

void *
kern_calloc(size_t count, size_t size)
{
	void *pointer = calloc(count, size);
	if (pointer != NULL)
		fixture_live++;
	return pointer;
}

void
kern_free(void *pointer)
{
	if (pointer != NULL)
		fixture_live--;
	free(pointer);
}

#include "../../../src/drivers/gpu/i915/compiler/spirv.c"
#include "../../../src/drivers/gpu/i915/compiler/eu.c"
#include "../../../src/drivers/gpu/i915/compiler/compile.c"
#include "../../../src/drivers/gpu/i915/tests/fixtures/generality-shaders-gen.inc"
#include "../../ws075/tests/guard/scoreboard-check.h"

static uint32_t *
load_spv(const char *name, size_t *words)
{
	char path[512];
	FILE *file;
	long size;
	uint32_t *code;

	snprintf(path, sizeof(path), "%s/userland/tests/vkdemo/shaders/%s", VK_REPO, name);
	file = fopen(path, "rb");
	assert(file != NULL);
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	code = malloc((size_t)size);
	assert(fread(code, 1, (size_t)size, file) == (size_t)size);
	fclose(file);
	*words = (size_t)size / 4U;
	return code;
}

/* The staged position of a vertex kernel: the VUE ends at r126, header first (compile.c "Register conventions"). */
static unsigned
vue_position(const struct i915_shader_binary *binary)
{
	return COMPILE_MAX_GRF - 4U * (2U + binary->varying_count) + 4U;
}

/*
 * E-128: a kernel opens with a prologue that zeroes its outputs (compile.c compile_prologue): MOVs of
 * an immediate into the staging registers.  The instructions of the IR start after it.
 */
static const uint32_t *
body(const struct i915_shader_binary *binary)
{
	const uint32_t *inst = binary->code;

	while ((inst[0] & 0x7FU) == EU_OP_MOV && ((inst[1] >> (EU_SRC0_IS_IMM_BIT - 32U)) & 1U) != 0U &&
	    (inst[1] >> 24) >= COMPILE_LAST_VALUE_GRF + 1U)
		inst += 4;
	return inst;
}

/* Reports whether any instruction in the code carries the given hw opcode. */
static int
has_opcode(const struct i915_shader_binary *binary, uint32_t opcode)
{
	uint32_t dwords;
	uint32_t index;

	dwords = binary->code_bytes / 4U;
	for (index = 0U; index + 4U <= dwords; index += 4U) {
		if ((binary->code[index] & 0x7FU) == opcode)
			return 1;
	}
	return 0;
}

static void
compile_shader(const char *name, enum i915_shader_stage stage, uint32_t expect_math)
{
	uint32_t *spv;
	size_t words;
	struct i915_shader_ir *ir;
	struct i915_shader_binary *binary;
	int error;

	struct i915_compile_diagnostic diag;

	spv = load_spv(name, &words);
	error = drv_i915_shader_parse(spv, words, stage, &ir, &diag);
	if (error != 0)
		printf("  %s refused: opcode %u at word %u: %s\n", name, diag.opcode, diag.word_offset, diag.reason != NULL ? diag.reason : "-");
	assert(error == 0);

	error = drv_i915_shader_compile(ir, &binary);
	assert(error == 0);

	/* The binary carries whole instructions, more than the payload registers. */
	assert(binary->code_bytes > 0U);
	assert((binary->code_bytes % (4U * 4U)) == 0U);
	assert(binary->stage == stage);
	assert(binary->grf_used > COMPILE_FIRST_VALUE_GRF);

	/* Every shader ends by sending its output and retiring the thread. */
	assert(has_opcode(binary, stage == I915_STAGE_VERTEX ? EU_OP_SEND : EU_OP_SENDC));

	/* The vertex shader's rotation lowers to math instructions. */
	if (expect_math != 0U)
		assert(has_opcode(binary, EU_OP_MATH));

	drv_i915_shader_binary_free(binary);
	drv_i915_shader_ir_free(ir);
	free(spv);
}

/* bit `position` of a 128-bit EU instruction */
static unsigned
inst_bit(const uint32_t *inst, unsigned position)
{
	return (inst[position / 32U] >> (position % 32U)) & 1U;
}

static unsigned
inst_field(const uint32_t *inst, unsigned high, unsigned low)
{
	unsigned value = 0U, position;

	for (position = low; position <= high; position++)
		value |= inst_bit(inst, position) << (position - low);
	return value;
}

static struct i915_shader_ir *
hand_ir(struct i915_shader_ir_inst *insts, unsigned count)
{
	static struct i915_shader_ir ir;

	memset(&ir, 0, sizeof(ir));
	ir.stage = I915_STAGE_VERTEX;
	ir.instructions = insts;
	ir.instruction_count = count;
	ir.value_count = 16U;
	ir.push_bytes = 8U;
	return &ir;
}

/*
 * a - b is not a + b and not b - a.  The EU has no SUB used here: the lowering is an ADD whose
 * SECOND source carries the negate modifier.  5 - 2 = 3 and 2 - 5 = -3 differ exactly in which
 * operand is negated, so the check is on operand numbers and modifiers, not on an opcode name.
 */
static void
test_fsub_is_add_with_second_source_negated(void)
{
	struct i915_shader_ir_inst insts[3];
	struct i915_shader_binary *binary;
	const uint32_t *inst;
	unsigned src0_nr, src1_nr;
	int error;

	/* two loads first, so %1 and %2 own known registers whatever order the operands are visited in */
	memset(insts, 0, sizeof(insts));
	insts[0].op = I915_IR_LOAD_INPUT; insts[0].dst = 1U;
	insts[1].op = I915_IR_LOAD_INPUT; insts[1].dst = 2U;
	insts[2].op = I915_IR_FSUB;
	insts[2].dst = 3U; insts[2].src[0] = 1U; insts[2].src[1] = 2U;      /* %3 = %1 - %2 */
	error = drv_i915_shader_compile(hand_ir(insts, 3U), &binary);
	assert(error == 0);
	inst = body(binary) + 2U * 4U;
	assert((inst[0] & 0x7FU) == EU_OP_ADD);
	src0_nr = inst_field(inst, EU_SRC0_REG_NR_HI, EU_SRC0_REG_NR_LO);
	src1_nr = inst_field(inst, EU_SRC1_REG_NR_HI, EU_SRC1_REG_NR_LO);
	/* %1 -> first value register, %2 -> the next: the minuend is source 0, the subtrahend source 1 */
	assert(src0_nr == COMPILE_FIRST_VALUE_GRF && src1_nr == COMPILE_FIRST_VALUE_GRF + 1U);
	assert(inst_bit(inst, EU_SRC0_NEGATE_BIT) == 0U);                  /* the minuend is read as is */
	assert(inst_bit(inst, EU_SRC1_NEGATE_BIT) == 1U);                  /* the subtrahend is negated */
	drv_i915_shader_binary_free(binary);

	/* and an ADD stays an ADD with no modifier */
	insts[2].op = I915_IR_FADD;
	error = drv_i915_shader_compile(hand_ir(insts, 3U), &binary);
	assert(error == 0);
	inst = body(binary) + 2U * 4U;
	assert((inst[0] & 0x7FU) == EU_OP_ADD);
	assert(inst_bit(inst, EU_SRC0_NEGATE_BIT) == 0U && inst_bit(inst, EU_SRC1_NEGATE_BIT) == 0U);
	drv_i915_shader_binary_free(binary);
}

/* -a is a MOV whose source is negated; a push constant is one float read as a scalar region */
static void
test_fneg_and_push_operands(void)
{
	struct i915_shader_ir_inst insts[3];
	struct i915_shader_binary *binary;
	const uint32_t *inst;
	int error;

	memset(insts, 0, sizeof(insts));
	insts[0].op = I915_IR_LOAD_PUSH; insts[0].dst = 1U; insts[0].immediate = 4U;   /* the SECOND float */
	insts[1].op = I915_IR_FNEG; insts[1].dst = 2U; insts[1].src[0] = 1U;
	insts[2].op = I915_IR_STORE_OUTPUT; insts[2].src[0] = 2U; insts[2].location = 0U; insts[2].component = 3U;
	error = drv_i915_shader_compile(hand_ir(insts, 3U), &binary);
	assert(error == 0);
	inst = body(binary);                                            /* MOV r16 <- r2.4<0;1,0> */
	assert((inst[0] & 0x7FU) == EU_OP_MOV);
	assert(inst_field(inst, EU_SRC0_REG_NR_HI, EU_SRC0_REG_NR_LO) == COMPILE_PAYLOAD_GRF);
	assert(inst_field(inst, EU_SRC0_SUBREG_HI, EU_SRC0_SUBREG_LO) == 4U);
	assert(inst_field(inst, EU_SRC0_VSTRIDE_HI, EU_SRC0_VSTRIDE_LO) == EU_VSTRIDE_0);
	assert(inst_field(inst, EU_SRC0_WIDTH_HI, EU_SRC0_WIDTH_LO) == EU_WIDTH_1);
	assert(inst_field(inst, EU_SRC0_HSTRIDE_HI, EU_SRC0_HSTRIDE_LO) == EU_HSTRIDE_0);
	inst = body(binary) + 4U;                                       /* MOV r17 <- -r16 */
	assert((inst[0] & 0x7FU) == EU_OP_MOV);
	assert(inst_field(inst, EU_SRC0_REG_NR_HI, EU_SRC0_REG_NR_LO) == COMPILE_FIRST_VALUE_GRF);
	assert(inst_bit(inst, EU_SRC0_NEGATE_BIT) == 1U);
	assert(inst_field(inst, EU_DST_REG_NR_HI, EU_DST_REG_NR_LO) == COMPILE_FIRST_VALUE_GRF + 1U);
	inst = body(binary) + 8U;                                       /* MOV the one varying, component 3 <- r17 */
	assert(inst_field(inst, EU_DST_REG_NR_HI, EU_DST_REG_NR_LO) == COMPILE_MAX_GRF - 4U + 3U);
	assert(inst_field(inst, EU_SRC0_REG_NR_HI, EU_SRC0_REG_NR_LO) == COMPILE_FIRST_VALUE_GRF + 1U);
	assert(inst_bit(inst, EU_SRC0_NEGATE_BIT) == 0U);
	drv_i915_shader_binary_free(binary);
}

/* a register is reused only after the LAST reader of its value */
static void
test_register_lifetime(void)
{
	struct i915_shader_ir_inst insts[6];
	struct i915_shader_binary *binary;
	const uint32_t *inst;
	int error;

	memset(insts, 0, sizeof(insts));
	insts[0].op = I915_IR_LOAD_INPUT; insts[0].dst = 1U;                                   /* r16, read at 2 and 4 */
	insts[1].op = I915_IR_LOAD_INPUT; insts[1].dst = 2U; insts[1].component = 1U;          /* r17, last read at 2 */
	insts[2].op = I915_IR_FMUL; insts[2].dst = 3U; insts[2].src[0] = 1U; insts[2].src[1] = 2U;  /* r18 */
	insts[3].op = I915_IR_CONST; insts[3].dst = 4U; insts[3].immediate = 0x40000000U;      /* takes r17 (free), NOT r16 */
	insts[4].op = I915_IR_FSUB; insts[4].dst = 5U; insts[4].src[0] = 1U; insts[4].src[1] = 4U;
	insts[5].op = I915_IR_STORE_OUTPUT; insts[5].src[0] = 5U;
	error = drv_i915_shader_compile(hand_ir(insts, 6U), &binary);
	assert(error == 0);
	inst = body(binary) + 3U * 4U;
	assert(inst_field(inst, EU_DST_REG_NR_HI, EU_DST_REG_NR_LO) == COMPILE_FIRST_VALUE_GRF + 1U);
	inst = body(binary) + 4U * 4U;
	assert(inst_field(inst, EU_SRC0_REG_NR_HI, EU_SRC0_REG_NR_LO) == COMPILE_FIRST_VALUE_GRF);       /* %1 still intact */
	assert(inst_field(inst, EU_SRC1_REG_NR_HI, EU_SRC1_REG_NR_LO) == COMPILE_FIRST_VALUE_GRF + 1U);
	assert(inst_bit(inst, EU_SRC1_NEGATE_BIT) == 1U);
	drv_i915_shader_binary_free(binary);
}

/* an unknown IR operation, a value read before its definition, a push constant outside the block: no binary */
static void
test_not_lowered_ir_is_refused(void)
{
	struct i915_shader_ir_inst insts[1];
	struct i915_shader_binary *binary;
	int error;

	memset(insts, 0, sizeof(insts));
	insts[0].op = (enum i915_shader_ir_op)0x7fff;
	insts[0].dst = 3U; insts[0].src[0] = 1U; insts[0].src[1] = 2U;
	binary = (struct i915_shader_binary *)1;
	error = drv_i915_shader_compile(hand_ir(insts, 1U), &binary);
	assert(error == ENOTSUP && binary == NULL);

	memset(insts, 0, sizeof(insts));
	insts[0].op = I915_IR_FADD; insts[0].dst = 3U; insts[0].src[0] = 1U; insts[0].src[1] = 2U;
	error = drv_i915_shader_compile(hand_ir(insts, 1U), &binary);
	assert(error == EINVAL && binary == NULL);

	memset(insts, 0, sizeof(insts));
	insts[0].op = I915_IR_LOAD_PUSH; insts[0].dst = 3U; insts[0].immediate = 8U;   /* push_bytes is 8 */
	error = drv_i915_shader_compile(hand_ir(insts, 1U), &binary);
	assert(error == EINVAL && binary == NULL);

	/* a stage past I915_STAGE_COUNT is inconsistent, not taken for a fragment shader (ws075-p007a a4) */
	{
		struct i915_shader_ir *ir;

		memset(insts, 0, sizeof(insts));
		insts[0].op = I915_IR_NOP;
		ir = hand_ir(insts, 1U);
		ir->stage = I915_STAGE_COUNT;
		binary = (struct i915_shader_binary *)1;
		error = drv_i915_shader_compile(ir, &binary);
		assert(error == EINVAL && binary == NULL);
	}

	/* more varyings than a VUE carries (COMPILE_MAX_VARYINGS, sixteen) */
	{
		struct i915_shader_ir_inst many[18];
		unsigned k;

		memset(many, 0, sizeof(many));
		many[0].op = I915_IR_LOAD_PUSH; many[0].dst = 1U;
		for (k = 1U; k < 18U; k++) {
			many[k].op = I915_IR_STORE_OUTPUT; many[k].src[0] = 1U; many[k].location = k - 1U;
		}
		error = drv_i915_shader_compile(hand_ir(many, 18U), &binary);
		assert(error == ENOTSUP && binary == NULL);
	}
}

/*
 * EU generation for the shipped vertex shader: one EU instruction per IR instruction plus the
 * terminator, the transcendentals as MATH.  This is the "EU generated" stage: it says nothing
 * about the payload / URB conventions; those are judged by run-vk-gentool-test.sh and on hardware (E-128).
 */
static void
test_vertex_shader_generates_eu(void)
{
	uint32_t *spv;
	size_t words;
	struct i915_shader_ir *ir;
	struct i915_shader_binary *binary;

	spv = load_spv("cuboid.vert.spv", &words);
	assert(drv_i915_shader_parse(spv, words, I915_STAGE_VERTEX, &ir, NULL) == 0);
	assert(drv_i915_shader_compile(ir, &binary) == 0);
	/*
	 * one EU instruction to an IR instruction, plus: the prologue (4 header + 4 position + 4 for the one
	 * varying) and the end -- URB write, handle copy, URB write; the sync.nops (where the scoreboard pass of
	 * ws075-p022 put them) are counted apart, and there is at least one, before the MATHs' results are read
	 */
	{
		unsigned index, syncs = 0U;

		for (index = 0U; index < binary->code_bytes / 16U; index++)
			if (inst_field(binary->code + index * 4U, EU_OPCODE_HI, EU_OPCODE_LO) == EU_OP_SYNC)
				syncs++;
		assert(syncs >= 1U);
		assert(binary->code_bytes / 16U - syncs == ir->instruction_count + 12U + 3U);
	}
	assert(binary->grf_used > COMPILE_FIRST_VALUE_GRF && binary->grf_used <= COMPILE_LAST_VALUE_GRF + 1U);
	assert(binary->dispatch_grf_start == COMPILE_PAYLOAD_GRF && binary->push_regs == 1U);
	assert(binary->input_count == 2U && binary->input_locations[0] == 0U && binary->input_locations[1] == 1U);
	assert(binary->varying_count == 1U);
	printf("  cuboid.vert.spv: %u IR instructions -> %u EU instructions, value registers r%u..r%u\n",
		ir->instruction_count, binary->code_bytes / 16U, COMPILE_FIRST_VALUE_GRF, binary->grf_used - 1U);
	drv_i915_shader_binary_free(binary);
	drv_i915_shader_ir_free(ir);
	free(spv);
}

/*
 * A model of what the generated EU words compute, for the instructions this compiler emits:
 * 8 SIMD channels, a register = 8 dwords (32 bytes), a <8;8,1> region reads channel c from
 * dword c, a <0;1,0> region reads the one element at byte `subnr` for every channel, an
 * immediate is the same for every channel.  The model decodes the ENCODED words (bit fields
 * from the transcribed encoding table), so an operand in the wrong slot, a missing negate or
 * abs, a wrong conditional modifier, a flag read or written in the wrong place, a predicate
 * on the wrong instruction, a register reused while its value is still needed, or a push
 * constant read as a vector all change the result.  Registers hold bits: a float, or a
 * Boolean as all ones / zero.
 *
 * Flags: f0.0 f0.1 f1.0 f1.1, sixteen channel bits each.  A predicated instruction runs only
 * on the channels whose flag bit is set -- except SEL, which takes source 0 there and source
 * 1 elsewhere.  A CMP writes the flag bits (and its register destination) of the channels it
 * runs on.  The render-target write that ends a fragment thread records the channels it went
 * to (the predicate) in `written`, and the thread ends there.
 *
 * It is a model of the instruction semantics only; it says nothing about how the hardware
 * fills the payload or consumes the staged outputs.
 */
/* How many WHILEs sent some but not all of their channels back (divergent loop passes). */
static unsigned eu_model_divergent_whiles;

/*
 * Scratch memory (p014 E3): the thread's scratch space, junk until written; r0.3 carries the per-thread scratch
 * space and r0.5 the thread's scratch base, both with junk around the bits a header copies.  A block write stores
 * the channels it runs on (the execution mask), a block read loads all eight and must run outside the mask.
 */
#define EU_MODEL_SCRATCH_BYTES	65536U
#define EU_MODEL_R0_3		0x5a5a5a50U     /* per-thread scratch space 0 (1 KiB) under junk */
#define EU_MODEL_R0_5		0x00340000U     /* the scratch base; the low ten bits get junk */

/* The URB writes of a vertex thread: VUE slot, component, channel. */
#define EU_MODEL_VUE_SLOTS	34U

/* The input vertex handles of a geometry thread: six vertices of eight channels' primitives, from this value on. */
#define EU_MODEL_GS_HANDLE	0x1000U
#define EU_MODEL_GS_HANDLES	48U

static unsigned eu_model_scratch_writes;
static unsigned eu_model_scratch_reads;
static unsigned eu_model_scratch_partial;       /* block writes that ran on some channels only (a loop had stopped others) */

/* how many kernels had their scoreboard checked (ws075-p022) */
static unsigned eu_model_scoreboard_checks;

/* how many IFs the model ran, and how many it jumped over with no channel left (ws075-p023) */
static unsigned eu_model_ifs, eu_model_ifs_jumped;

struct eu_model {
	uint32_t grf[128][8];
	uint16_t flag[4];
	unsigned written;               /* the channels the render-target write went to */
	int ended;                      /* the thread ended (an end-of-thread SEND ran) */
	unsigned dispatched;            /* the channels the thread runs on (the execution mask) */
	uint32_t scratch[EU_MODEL_SCRATCH_BYTES / 4U];
	uint32_t vue[EU_MODEL_VUE_SLOTS][4][8];
	uint64_t vue_written;           /* the VUE slots a URB write reached */

	/*
	 * A geometry thread (ws075-p007a): the input VUEs a URB read finds by handle (EU_MODEL_GS_HANDLE + 8 v + c is
	 * vertex v of channel c's primitive).  Its writes go to eu_model_gs_entry.
	 */
	uint32_t gs_input[EU_MODEL_GS_HANDLES][EU_MODEL_VUE_SLOTS][4];
};

/*
 * The output URB entries of a geometry thread's eight channels, EU_MODEL_GS_ENTRY_DWORDS dwords each, which its
 * URB writes fill when a test points this at them (ws075-p007a); NULL for a vertex thread, whose writes go to the
 * model's VUE.  A dword no write reached keeps EU_MODEL_GS_UNWRITTEN.
 */
#define EU_MODEL_GS_ENTRY_DWORDS	4096U
#define EU_MODEL_GS_UNWRITTEN		0xDEADBEEFU
static uint32_t *eu_model_gs_entry;

static float
mget(const struct eu_model *m, unsigned r, unsigned c)
{
	float value;

	memcpy(&value, &m->grf[r][c], sizeof(value));
	return value;
}

static void
mset(struct eu_model *m, unsigned r, unsigned c, float value)
{
	memcpy(&m->grf[r][c], &value, sizeof(value));
}

static float
bits_float(uint32_t bits)
{
	float value;

	memcpy(&value, &bits, sizeof(value));
	return value;
}

static uint32_t
float_bits(float value)
{
	uint32_t bits;

	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

/* Starts a model: every register junk that differs per channel, all eight channels dispatched. */
static void
eu_model_init(struct eu_model *m)
{
	unsigned r, c;

	memset(m, 0, sizeof(*m));
	for (r = 0U; r < 128U; r++)
		for (c = 0U; c < 8U; c++)
			mset(m, r, c, -1000.0f - (float)(r * 8U + c));
	m->dispatched = 0xFFU;
	/* the flags are undefined when a thread starts: all set, so a kernel that trusts a bit it did not write is caught (ws075-p023) */
	for (r = 0U; r < 4U; r++)
		m->flag[r] = 0xFFFFU;
	m->grf[0][3] = EU_MODEL_R0_3;
	m->grf[0][5] = EU_MODEL_R0_5 | 0x2A5U;
	for (r = 0U; r < EU_MODEL_SCRATCH_BYTES / 4U; r++)
		m->scratch[r] = 0x7F800001U + r;          /* NaNs, so a read of an unwritten slot shows */
}

/* The 32-bit message descriptor of a SEND, gathered from where eu.c scatters it. */
static uint32_t
eu_model_descriptor(const uint32_t *inst)
{
	return (inst_field(inst, 123U, 122U) << 30) | (inst_field(inst, 71U, 67U) << 25) |
	       (inst_field(inst, 55U, 51U) << 20) | (inst_field(inst, 121U, 113U) << 11) | inst_field(inst, 91U, 81U);
}

/* A scratch block write or read: the header in src0, the data in src1 (write) or the destination (read). */
static void
eu_model_scratch(struct eu_model *m, const struct i915_shader_binary *binary, const uint32_t *inst, unsigned enabled)
{
	uint32_t descriptor = eu_model_descriptor(inst);
	unsigned header = inst_field(inst, EU_SRC0_REG_NR_HI, EU_SRC0_REG_NR_LO);
	unsigned data = inst_field(inst, EU_SRC1_REG_NR_HI, EU_SRC1_REG_NR_LO);
	unsigned dst = inst_field(inst, EU_DST_REG_NR_HI, EU_DST_REG_NR_LO);
	unsigned no_mask = inst_bit(inst, EU_NO_MASK_BIT);
	uint32_t offset, channel;

	/* the header Mesa's generate_scratch_header() builds, and a slot inside the kernel's scratch space */
	assert(m->grf[header][3] == (EU_MODEL_R0_3 & EU_SCRATCH_SIZE_MASK));
	assert(m->grf[header][5] == EU_MODEL_R0_5);
	offset = m->grf[header][2] * 16U;
	assert(binary->scratch_bytes >= 1024U && (binary->scratch_bytes & (binary->scratch_bytes - 1U)) == 0U);
	assert((offset % 32U) == 0U && offset + 32U <= binary->scratch_bytes && offset + 32U <= EU_MODEL_SCRATCH_BYTES);
	if (descriptor == COMPILE_DESC_SCRATCH_WRITE) {
		assert(no_mask == 0U && inst_field(inst, 103U, 99U) == 1U);      /* under the mask, one data register */
		for (channel = 0U; channel < 8U; channel++)
			if ((enabled >> channel) & 1U)
				m->scratch[offset / 4U + channel] = m->grf[data][channel];
		eu_model_scratch_writes++;
		if (enabled != m->dispatched)
			eu_model_scratch_partial++;
		return;
	}
	assert(descriptor == COMPILE_DESC_SCRATCH_READ && no_mask == 1U);
	for (channel = 0U; channel < 8U; channel++)
		m->grf[dst][channel] = m->scratch[offset / 4U + channel];
	eu_model_scratch_reads++;
}

/*
 * A URB message: a SIMD8 read of a geometry thread's input VUE (the handle per channel in src0, a register to a
 * component in the reply), a geometry thread's write into its output entry (the handles, then per-slot offsets
 * and a channel mask when the descriptor says so, the data in the second payload run: register d to dword d from
 * the OWord the offsets name, the mask picking dword d % 4), or a vertex thread's slots from its second payload
 * run, from the global offset in the descriptor on.
 */
static void
eu_model_urb(struct eu_model *m, const uint32_t *inst, unsigned enabled)
{
	uint32_t descriptor = eu_model_descriptor(inst);
	unsigned data = inst_field(inst, EU_SRC1_REG_NR_HI, EU_SRC1_REG_NR_LO);
	unsigned slots = inst_field(inst, 103U, 99U) / 4U, first = (descriptor >> 4) & 0x7FFU, s, k, c;
	unsigned src0 = inst_field(inst, EU_SRC0_REG_NR_HI, EU_SRC0_REG_NR_LO);
	unsigned dst = inst_field(inst, EU_DST_REG_NR_HI, EU_DST_REG_NR_LO);

	/* a read: mlen 1 (the handles as the header), the reply's length, the slot */
	if ((descriptor & 0xFU) == COMPILE_URB_OPCODE_SIMD8_READ) {
		unsigned length = (descriptor >> EU_DESC_RLEN_SHIFT) & 0x1FU, handle;

		assert(((descriptor >> EU_DESC_MLEN_SHIFT) & 0xFU) == 1U && (descriptor & EU_DESC_HEADER_PRESENT) != 0U);
		assert(length >= 1U && length <= 4U && first < EU_MODEL_VUE_SLOTS && (descriptor & (1U << 17)) == 0U);
		for (c = 0U; c < 8U; c++) {
			if (((enabled >> c) & 1U) == 0U)
				continue;
			handle = m->grf[src0][c] - EU_MODEL_GS_HANDLE;
			assert(handle < EU_MODEL_GS_HANDLES && handle % 8U == c);
			for (k = 0U; k < length; k++)
				m->grf[dst + k][c] = m->gs_input[handle][first][k];
		}
		return;
	}

	/* a geometry thread's write into its output entry */
	if (eu_model_gs_entry != NULL) {
		unsigned per_slot = (descriptor >> 17) & 1U, mask = (descriptor >> 15) & 1U;
		unsigned length = inst_field(inst, 103U, 99U), dword, d;
		uint32_t base;

		assert((descriptor & 0xFU) == COMPILE_URB_OPCODE_SIMD8_WRITE);
		assert(((descriptor >> EU_DESC_MLEN_SHIFT) & 0xFU) == 1U + per_slot + mask && length >= 1U && length <= 8U);
		for (c = 0U; c < 8U; c++) {
			if (((enabled >> c) & 1U) == 0U)
				continue;
			assert((m->grf[src0][c] & 0xFFFF0000U) == 0U);         /* the handle without the instance bits */
			base = first + (per_slot ? m->grf[src0 + 1U][c] : 0U);
			for (d = 0U; d < length; d++) {
				if (mask && ((m->grf[src0 + 1U + per_slot][c] >> (16U + d % 4U)) & 1U) == 0U)
					continue;
				dword = 4U * base + d;
				assert(dword < EU_MODEL_GS_ENTRY_DWORDS);
				eu_model_gs_entry[c * EU_MODEL_GS_ENTRY_DWORDS + dword] = m->grf[data + d][c];
			}
		}
		return;
	}

	assert(slots >= 1U && slots <= 2U && first + slots <= EU_MODEL_VUE_SLOTS);
	for (s = 0U; s < slots; s++) {
		for (k = 0U; k < 4U; k++)
			for (c = 0U; c < 8U; c++)
				if ((enabled >> c) & 1U)
					m->vue[first + s][k][c] = m->grf[data + 4U * s + k][c];
		m->vue_written |= 1ULL << (first + s);
	}
}

/* The operand of one source as the model reads it: bits and type, modifiers applied (floats and integers). */
static uint32_t
eu_model_source(const struct eu_model *m, const uint32_t *inst, int which, unsigned channel, unsigned *type_out)
{
	unsigned file, nr, subnr, vstride, width, hstride, negate, absolute, type;
	uint32_t bits;

	if (which == 0) {
		file = inst_bit(inst, EU_SRC0_IS_IMM_BIT) != 0U ? EU_FILE_IMM : inst_bit(inst, EU_SRC0_REG_FILE_BIT);
		nr = inst_field(inst, EU_SRC0_REG_NR_HI, EU_SRC0_REG_NR_LO);
		subnr = inst_field(inst, EU_SRC0_SUBREG_HI, EU_SRC0_SUBREG_LO);
		vstride = inst_field(inst, EU_SRC0_VSTRIDE_HI, EU_SRC0_VSTRIDE_LO);
		width = inst_field(inst, EU_SRC0_WIDTH_HI, EU_SRC0_WIDTH_LO);
		hstride = inst_field(inst, EU_SRC0_HSTRIDE_HI, EU_SRC0_HSTRIDE_LO);
		negate = inst_bit(inst, EU_SRC0_NEGATE_BIT);
		absolute = inst_bit(inst, EU_SRC0_ABS_BIT);
		type = inst_field(inst, EU_SRC0_REG_TYPE_HI, EU_SRC0_REG_TYPE_LO);
	} else {
		file = inst_bit(inst, EU_SRC1_IS_IMM_BIT) != 0U ? EU_FILE_IMM : inst_bit(inst, EU_SRC1_REG_FILE_BIT);
		nr = inst_field(inst, EU_SRC1_REG_NR_HI, EU_SRC1_REG_NR_LO);
		subnr = inst_field(inst, EU_SRC1_SUBREG_HI, EU_SRC1_SUBREG_LO);
		vstride = inst_field(inst, EU_SRC1_VSTRIDE_HI, EU_SRC1_VSTRIDE_LO);
		width = inst_field(inst, EU_SRC1_WIDTH_HI, EU_SRC1_WIDTH_LO);
		hstride = inst_field(inst, EU_SRC1_HSTRIDE_HI, EU_SRC1_HSTRIDE_LO);
		negate = inst_bit(inst, EU_SRC1_NEGATE_BIT);
		absolute = inst_bit(inst, EU_SRC1_ABS_BIT);
		type = inst_field(inst, EU_SRC1_REG_TYPE_HI, EU_SRC1_REG_TYPE_LO);
	}
	assert(type == EU_TYPE_F || type == EU_TYPE_D || type == EU_TYPE_UD || type == EU_TYPE_UW);
	*type_out = type;
	if (file == EU_FILE_IMM) {
		bits = inst[3];
	} else {
		assert(file == EU_FILE_GRF && nr < 128U);
		if (vstride == EU_VSTRIDE_8 && width == EU_WIDTH_8 && hstride == EU_HSTRIDE_1) {
			assert(subnr == 0U && type != EU_TYPE_UW);
			bits = m->grf[nr][channel];
		} else if (vstride == EU_VSTRIDE_16 && width == EU_WIDTH_8 && hstride == EU_HSTRIDE_2) {
			/* every second word: the low (subregister 0) or high (2) half of each channel's dword */
			assert(type == EU_TYPE_UW && (subnr == 0U || subnr == 2U));
			bits = (m->grf[nr][channel] >> (subnr * 8U)) & 0xFFFFU;
		} else {
			assert(vstride == EU_VSTRIDE_0 && width == EU_WIDTH_1 && hstride == EU_HSTRIDE_0);
			if (type == EU_TYPE_UW) {
				assert((subnr % 2U) == 0U);
				bits = (m->grf[nr][subnr / 4U] >> ((subnr % 4U) * 8U)) & 0xFFFFU;
			} else {
				assert((subnr % 4U) == 0U);
				bits = m->grf[nr][subnr / 4U];
			}
		}
	}
	if (type == EU_TYPE_F) {
		float value = bits_float(bits);

		if (absolute != 0U)
			value = fabsf(value);
		if (negate != 0U)
			value = -value;
		bits = float_bits(value);
	} else {
		/* an integer modifier: |x| (signed), then -x, modulo 2^32 */
		assert(type != EU_TYPE_UW || (negate == 0U && absolute == 0U));
		if (absolute != 0U && (int32_t)bits < 0)
			bits = 0U - bits;
		if (negate != 0U)
			bits = 0U - bits;
	}
	return bits;
}

/* Evaluates a conditional modifier on two sources of one type (floats, signed or unsigned integers). */
static int
eu_model_test(unsigned cond, uint32_t a, uint32_t b, unsigned type)
{
	if (type == EU_TYPE_F) {
		float fa = bits_float(a), fb = bits_float(b);

		switch (cond) {
		case EU_COND_Z: return fa == fb;
		case EU_COND_NZ: return fa != fb;
		case EU_COND_G: return fa > fb;
		case EU_COND_GE: return fa >= fb;
		case EU_COND_L: return fa < fb;
		case EU_COND_LE: return fa <= fb;
		default: break;
		}
	} else if (type == EU_TYPE_UD) {
		switch (cond) {
		case EU_COND_Z: return a == b;
		case EU_COND_NZ: return a != b;
		case EU_COND_G: return a > b;
		case EU_COND_GE: return a >= b;
		case EU_COND_L: return a < b;
		case EU_COND_LE: return a <= b;
		default: break;
		}
	} else {
		int32_t ia = (int32_t)a, ib = (int32_t)b;

		switch (cond) {
		case EU_COND_Z: return ia == ib;
		case EU_COND_NZ: return ia != ib;
		case EU_COND_G: return ia > ib;
		case EU_COND_GE: return ia >= ib;
		case EU_COND_L: return ia < ib;
		case EU_COND_LE: return ia <= ib;
		default: break;
		}
	}
	assert(!"conditional modifier the model does not know");
	return 0;
}

/* the fake texture of the sampler: every component depends on u, v and the binding in its own way */
static void
eu_model_texture(uint32_t binding, float u, float v, float rgba[4])
{
	rgba[0] = u + 10.0f * (float)binding;
	rgba[1] = v;
	rgba[2] = u + 2.0f * v;
	rgba[3] = u * v + 0.5f;
}

/* An integer source as a signed or unsigned 64-bit value, by its type. */
static int64_t
eu_model_integer(uint32_t bits, unsigned type)
{
	if (type == EU_TYPE_D)
		return (int64_t)(int32_t)bits;
	return (int64_t)bits;
}

/* A result into the destination's type: a float result converted to an integer truncates toward zero. */
static uint32_t
eu_model_store(int is_float, float f, int64_t i, unsigned dst_type)
{
	if (dst_type == EU_TYPE_F) {
		if (is_float)
			return float_bits(f);
		return float_bits((float)i);            /* the default rounding: to nearest even */
	}
	assert(dst_type == EU_TYPE_D || dst_type == EU_TYPE_UD);
	if (is_float) {
		if (dst_type == EU_TYPE_D)
			return (uint32_t)(int32_t)f;
		return (uint32_t)f;
	}
	return (uint32_t)i;
}

/*
 * Runs the words up to the SEND that ends the thread; any instruction the model does not know fails.
 *
 * A WHILE sends the channels whose flag bit is set back to the loop's first instruction; the others wait after
 * it (parked) and run again, with every channel that entered the loop, once none goes back.
 */
static void
eu_model_run(struct eu_model *m, const struct i915_shader_binary *binary)
{
	unsigned count = binary->code_bytes / 16U, index, channel;
	unsigned active = m->dispatched;
	unsigned loop_at[32], parked[32], loops = 0U, passes = 0U;
	unsigned if_mask[32], ifs = 0U;         /* the active channels at each open IF (ws075-p023) */

	/* every kernel the model runs has its scoreboard checked first (ws075-p022) */
	{
		int fault = sbc_check(binary->code, count);

		if (fault >= 0)
			printf("  scoreboard fault at instruction %d of %u\n", fault, count);
		assert(fault < 0);
		eu_model_scoreboard_checks++;
	}

	for (index = 0U; index < count; index++) {
		const uint32_t *inst = binary->code + index * 4U;
		unsigned opcode = inst_field(inst, EU_OPCODE_HI, EU_OPCODE_LO);
		unsigned dst = inst_field(inst, EU_DST_REG_NR_HI, EU_DST_REG_NR_LO);
		unsigned dst_file = inst_bit(inst, EU_DST_REG_FILE_BIT);
		unsigned dst_type = inst_field(inst, EU_DST_REG_TYPE_HI, EU_DST_REG_TYPE_LO);
		unsigned exec = inst_field(inst, EU_EXEC_SIZE_HI, EU_EXEC_SIZE_LO);
		unsigned predicate = inst_field(inst, EU_PRED_CONTROL_HI, EU_PRED_CONTROL_LO);
		unsigned flag = inst_bit(inst, EU_FLAG_REG_NR_BIT) * 2U + inst_bit(inst, EU_FLAG_SUBREG_NR_BIT);
		unsigned cond = inst_field(inst, EU_COND_MODIFIER_HI, EU_COND_MODIFIER_LO);
		unsigned enabled, types[2];
		uint32_t result[8];

		if (opcode == EU_OP_SYNC)
			continue;
		assert(inst_bit(inst, EU_PRED_INV_BIT) == 0U);

		/* the channels that run: the active ones (all eight outside the mask), narrowed by a predicate (not for SEL) */
		enabled = active;
		if (inst_bit(inst, EU_NO_MASK_BIT) != 0U)
			enabled = 0xFFU;
		if (predicate != 0U) {
			assert(predicate == EU_PREDICATE_NORMAL);
			if (opcode != EU_OP_SEL)
				enabled &= m->flag[flag];
		}

		/* IF: the channels whose flag bit is set run the body; none left jumps to the ENDIF (its JIP, in bytes) */
		if (opcode == EU_OP_IF) {
			int32_t jump = (int32_t)inst[3];

			assert(predicate == EU_PREDICATE_NORMAL && jump > 0 && (jump % 16) == 0);
			assert(inst[2] == inst[3]);             /* no ELSE: the UIP is the JIP */
			assert(ifs < 32U);
			if_mask[ifs++] = active;
			active = enabled;
			eu_model_ifs++;
			if (active == 0U) {
				eu_model_ifs_jumped++;
				index = (unsigned)((int32_t)index + jump / 16) - 1U;   /* lands on the ENDIF */
			}
			continue;
		}

		/* ENDIF: back to the channels active at its IF */
		if (opcode == EU_OP_ENDIF) {
			assert(ifs != 0U);
			active = if_mask[--ifs];
			continue;
		}

		/* the flag's channel bits into a general register's dword (the any-channel IF, ws075-p023) */
		if (opcode == EU_OP_MOV && exec == EU_EXEC_SIZE_1 && dst_file == 1U &&
		    inst_bit(inst, EU_SRC0_REG_FILE_BIT) == 0U && inst_bit(inst, EU_SRC0_IS_IMM_BIT) == 0U) {
			unsigned src_nr = inst_field(inst, EU_SRC0_REG_NR_HI, EU_SRC0_REG_NR_LO);
			unsigned src_sub = inst_field(inst, EU_SRC0_SUBREG_HI, EU_SRC0_SUBREG_LO);
			unsigned subnr = inst_field(inst, EU_DST_SUBREG_HI, EU_DST_SUBREG_LO);

			assert(src_nr == EU_ARF_FLAG || src_nr == EU_ARF_FLAG + 1U);
			assert(dst_type == EU_TYPE_UD && (subnr % 4U) == 0U);
			m->grf[dst][subnr / 4U] = m->flag[(src_nr - EU_ARF_FLAG) * 2U + src_sub / 2U];
			/* only the active channels' bits: the others are stale, and one would take the IF for nothing */
			assert((m->grf[dst][subnr / 4U] & ~active) == 0U);
			continue;
		}

		if (opcode == EU_OP_WHILE) {
			int32_t jump = (int32_t)inst[3];
			unsigned back;

			assert(predicate == EU_PREDICATE_NORMAL && jump < 0 && (jump % 16) == 0);
			assert(inst_bit(inst, EU_SRC0_IS_IMM_BIT) == 1U);
			assert(++passes < 1000000U);
			if (loops == 0U || loop_at[loops - 1U] != index) {
				assert(loops < 32U);
				loop_at[loops] = index;
				parked[loops] = 0U;
				loops++;
			}
			back = enabled;
			if (back != 0U && back != active)
				eu_model_divergent_whiles++;
			parked[loops - 1U] |= active & ~back;
			if (back != 0U) {
				active = back;
				index = (unsigned)((int32_t)index + jump / 16) - 1U;
			} else {
				active = parked[loops - 1U];
				loops--;
			}
			continue;
		}

		if (opcode == EU_OP_SEND || opcode == EU_OP_SENDC) {
			unsigned sfid = inst_field(inst, EU_SEND_SFID_HI, EU_SEND_SFID_LO);
			unsigned src0 = inst_field(inst, EU_SRC0_REG_NR_HI, EU_SRC0_REG_NR_LO);
			unsigned src1 = inst_field(inst, EU_SRC1_REG_NR_HI, EU_SRC1_REG_NR_LO);

			if (sfid == EU_SFID_URB)
				eu_model_urb(m, inst, enabled);
			if (sfid == EU_SFID_DATA_CACHE) {
				eu_model_scratch(m, binary, inst, enabled);
				continue;
			}
			if (inst_bit(inst, EU_SEND_EOT_BIT) != 0U) {
				assert(index == count - 1U);
				assert(loops == 0U && active == m->dispatched);
				m->written = enabled;
				m->ended = 1;
				return;
			}
			if (sfid == EU_SFID_SAMPLER) {
				/* binding table entry 1 + n: the low byte of the descriptor, in bits 91:81 and on */
				unsigned binding = (inst_field(inst, 91U, 81U) & 0xFFU) - 1U;

				for (channel = 0U; channel < 8U; channel++) {
					float rgba[4];
					unsigned k;

					eu_model_texture(binding, mget(m, src0, channel), mget(m, src1, channel), rgba);
					for (k = 0U; k < 4U; k++)
						if ((enabled >> channel) & 1U)
							mset(m, dst + k, channel, rgba[k]);
				}
			}
			continue;
		}

		/* a SIMD1 move or AND of one dword outside the mask: the scratch header (p014 E3) */
		if (exec == EU_EXEC_SIZE_1 && dst_file == 1U) {
			unsigned type, type1, subnr = inst_field(inst, EU_DST_SUBREG_HI, EU_DST_SUBREG_LO);
			uint32_t a = eu_model_source(m, inst, 0, 0U, &type);

			assert(inst_bit(inst, EU_NO_MASK_BIT) == 1U && predicate == 0U && (subnr % 4U) == 0U);
			assert(dst_type == EU_TYPE_UD && type == EU_TYPE_UD);
			if (opcode == EU_OP_AND) {
				a &= eu_model_source(m, inst, 1, 0U, &type1);
				assert(type1 == EU_TYPE_UD);
			} else {
				assert(opcode == EU_OP_MOV);
			}
			m->grf[dst][subnr / 4U] = a;
			continue;
		}

		/* the SIMD1 flag load: a word of a general register into a flag subregister */
		if (exec == EU_EXEC_SIZE_1) {
			unsigned type;
			uint32_t word;

			assert(opcode == EU_OP_MOV && inst_bit(inst, EU_NO_MASK_BIT) == 1U);
			assert(dst_file == 0U && (dst == EU_ARF_FLAG || dst == EU_ARF_FLAG + 1U) && dst_type == EU_TYPE_UW);
			word = eu_model_source(m, inst, 0, 0U, &type);
			assert(type == EU_TYPE_UW);
			m->flag[(dst - EU_ARF_FLAG) * 2U + inst_field(inst, EU_DST_SUBREG_HI, EU_DST_SUBREG_LO) / 2U] = (uint16_t)word;
			continue;
		}

		assert(exec == EU_EXEC_SIZE_8);
		assert(inst_field(inst, EU_DST_SUBREG_HI, EU_DST_SUBREG_LO) == 0U);
		for (channel = 0U; channel < 8U; channel++) {
			uint32_t a = eu_model_source(m, inst, 0, channel, &types[0]);
			uint32_t b = 0U;
			float fa = bits_float(a), fb;
			int64_t ia = eu_model_integer(a, types[0]), ib = 0;
			int two_sources;

			two_sources = opcode != EU_OP_MOV && opcode != EU_OP_NOT && opcode != EU_OP_RNDD && opcode != EU_OP_FRC &&
				opcode != EU_OP_RNDZ && opcode != EU_OP_RNDE;
			if (opcode == EU_OP_MATH) {
				unsigned function = inst_field(inst, EU_MATH_FUNCTION_HI, EU_MATH_FUNCTION_LO);

				two_sources = function == EU_MATH_INT_DIV_QUOTIENT || function == EU_MATH_INT_DIV_REMAINDER;
			}
			if (two_sources) {
				b = eu_model_source(m, inst, 1, channel, &types[1]);
				ib = eu_model_integer(b, types[1]);
			}
			fb = bits_float(b);
			switch (opcode) {
			case EU_OP_MOV:
				/* a move within a type (or between the integer types), or a conversion between floats and integers */
				if (types[0] == EU_TYPE_F && dst_type != EU_TYPE_F)
					result[channel] = eu_model_store(1, fa, 0, dst_type);
				else if (types[0] != EU_TYPE_F && dst_type == EU_TYPE_F)
					result[channel] = eu_model_store(0, 0.0f, ia, dst_type);
				else
					result[channel] = a;
				break;
			case EU_OP_ADD:
				if (types[0] == EU_TYPE_F) {
					assert(types[1] == EU_TYPE_F && dst_type == EU_TYPE_F);
					result[channel] = float_bits(fa + fb);
				} else {
					assert(types[1] != EU_TYPE_F && dst_type != EU_TYPE_F);
					result[channel] = (uint32_t)(ia + ib);
				}
				break;
			case EU_OP_MUL:
				if (types[0] == EU_TYPE_F) {
					assert(types[1] == EU_TYPE_F && dst_type == EU_TYPE_F);
					result[channel] = float_bits(fa * fb);
				} else {
					/* the 32 x 16-bit multiply of the lowering: the low 32 bits of the product */
					assert(types[1] == EU_TYPE_UW && dst_type != EU_TYPE_F);
					result[channel] = (uint32_t)((uint64_t)ia * (uint64_t)ib);
				}
				break;
			case EU_OP_AND: assert(types[0] != EU_TYPE_F); result[channel] = a & b; break;
			case EU_OP_OR: assert(types[0] != EU_TYPE_F); result[channel] = a | b; break;
			case EU_OP_XOR: assert(types[0] != EU_TYPE_F); result[channel] = a ^ b; break;
			case EU_OP_NOT: assert(types[0] != EU_TYPE_F); result[channel] = ~a; break;
			case EU_OP_SHL: assert(types[0] != EU_TYPE_F); result[channel] = a << (b & 31U); break;
			case EU_OP_SHR: assert(types[0] == EU_TYPE_UD); result[channel] = a >> (b & 31U); break;
			case EU_OP_ASR: assert(types[0] == EU_TYPE_D); result[channel] = (uint32_t)((int32_t)a >> (b & 31U)); break;
			case EU_OP_RNDD: assert(types[0] == EU_TYPE_F); result[channel] = float_bits(floorf(fa)); break;
			case EU_OP_RNDZ: assert(types[0] == EU_TYPE_F); result[channel] = float_bits(truncf(fa)); break;
			case EU_OP_RNDE: assert(types[0] == EU_TYPE_F); result[channel] = float_bits(nearbyintf(fa)); break;
			case EU_OP_FRC: assert(types[0] == EU_TYPE_F); result[channel] = float_bits(fa - floorf(fa)); break;
			case EU_OP_SEL:
				if (predicate != 0U) {
					/* per channel: source 0 where the flag bit is set */
					result[channel] = ((m->flag[flag] >> channel) & 1U) ? a : b;
				} else {
					/* min / max: source 0 where the test holds */
					assert(cond == EU_COND_L || cond == EU_COND_GE);
					result[channel] = eu_model_test(cond, a, b, types[0]) ? a : b;
				}
				break;
			case EU_OP_CMP:
				assert(types[0] == types[1] || (types[0] != EU_TYPE_F && types[1] != EU_TYPE_F));
				result[channel] = eu_model_test(cond, a, b, types[0]) ? 0xFFFFFFFFU : 0U;
				break;
			case EU_OP_MATH: {
				unsigned function = inst_field(inst, EU_MATH_FUNCTION_HI, EU_MATH_FUNCTION_LO);

				switch (function) {
				case EU_MATH_SIN: result[channel] = float_bits(sinf(fa)); break;
				case EU_MATH_COS: result[channel] = float_bits(cosf(fa)); break;
				case EU_MATH_RSQ: result[channel] = float_bits(1.0f / sqrtf(fa)); break;
				case EU_MATH_INV: result[channel] = float_bits(1.0f / fa); break;
				case EU_MATH_SQRT: result[channel] = float_bits(sqrtf(fa)); break;
				case EU_MATH_EXP: result[channel] = float_bits(exp2f(fa)); break;
				case EU_MATH_LOG: result[channel] = float_bits(log2f(fa)); break;
				case EU_MATH_INT_DIV_QUOTIENT:
				case EU_MATH_INT_DIV_REMAINDER:
					/* signed or unsigned by the type, rounded toward zero; the remainder has the dividend's sign */
					assert(types[0] == types[1] && types[0] == dst_type && types[0] != EU_TYPE_F);
					if (((enabled >> channel) & 1U) == 0U) {
						result[channel] = 0U;
						break;
					}
					/*
					 * A zero divisor, and INT_MIN / -1, have no defined result (SPIR-V; ws031-p024): the model
					 * gives 0 where the hardware gives some value, and the tests compare only defined words.
					 */
					if (b == 0U || (types[0] == EU_TYPE_D && a == 0x80000000U && b == 0xFFFFFFFFU)) {
						result[channel] = 0U;
						break;
					}
					if (types[0] == EU_TYPE_D) {
						int32_t sa = (int32_t)a, sb = (int32_t)b;

						result[channel] = function == EU_MATH_INT_DIV_QUOTIENT ? (uint32_t)(sa / sb) : (uint32_t)(sa % sb);
					} else {
						result[channel] = function == EU_MATH_INT_DIV_QUOTIENT ? a / b : a % b;
					}
					break;
				default: assert(!"math function the model does not know"); break;
				}
				break;
			}
			default:
				assert(!"EU opcode the model does not know");
				break;
			}
		}

		/* a comparison writes the flag bits of the channels it ran on */
		if (opcode == EU_OP_CMP) {
			for (channel = 0U; channel < 8U; channel++) {
				if (((enabled >> channel) & 1U) == 0U)
					continue;
				if (result[channel] != 0U)
					m->flag[flag] |= (uint16_t)(1U << channel);
				else
					m->flag[flag] &= (uint16_t)~(1U << channel);
			}
		} else {
			assert(cond == 0U || opcode == EU_OP_SEL || opcode == EU_OP_MATH);
		}

		/* the destination: a general register (the null register discards) */
		if (dst_file == 0U) {
			assert(dst == 0U);
			continue;
		}
		assert(dst < 128U);
		for (channel = 0U; channel < 8U; channel++)
			if (((enabled >> channel) & 1U) || (opcode == EU_OP_SEL && ((active >> channel) & 1U)))
				m->grf[dst][channel] = result[channel];
	}
	assert(!"no terminating SEND");
}

/*
 * Fragment payload: the dispatch mask in the low word of dword 7 of r1 (the PS thread payload of the
 * BSpec; Mesa reads it as brw_vec1_grf(1, 7) retyped to UW), the barycentrics in r2 / r3.  The rest
 * of r1 stays junk, so a kernel that loads its discard flag from anywhere else discards at random.
 */
static void
eu_model_fs_payload(struct eu_model *m, const struct i915_shader_binary *binary, unsigned dispatch_mask,
	const float bary1[8], const float bary2[8])
{
	unsigned c;

	m->dispatched = dispatch_mask;
	m->grf[1][7] = 0xDEAD0000U | dispatch_mask;
	for (c = 0U; c < 8U; c++) {
		mset(m, COMPILE_FS_BARY1_GRF, c, bary1[c]);
		mset(m, COMPILE_FS_BARY2_GRF, c, bary2[c]);
	}
	(void)binary;
}

/* Fragment payload: the plane of component `component` of the input of payload rank `rank`. */
static void
eu_model_fs_plane(struct eu_model *m, const struct i915_shader_binary *binary, unsigned rank, unsigned component,
	float d1, float d2, float origin)
{
	unsigned reg = COMPILE_FS_SETUP_GRF + binary->push_regs + 2U * rank + component / 2U;
	unsigned first = (component & 1U) * 4U;

	mset(m, reg, first + 0U, d1);
	mset(m, reg, first + 1U, d2);
	mset(m, reg, first + 3U, origin);
}

/* What the kernel's interpolation computes for a plane: (d2 * b2 + origin) + d1 * b1, in that order. */
static float
eu_model_interpolate(float d1, float d2, float origin, float b1, float b2)
{
	float value = d2 * b2;

	value = value + origin;
	return value + d1 * b1;
}

/*
 * The shipped vertex shader through BOTH stages (SPIR-V -> IR -> EU words), eight different
 * vertices in the eight channels at once, against the GLSL source evaluated here in C.
 */
static void
test_vertex_shader_eu_computes_the_shader(void)
{
	static const float vertices[8][5] = {
		{ 0.5f, -1.25f, 2.0f, 0.25f, 0.75f }, { -1.0f, 1.0f, -1.0f, 0.0f, 1.0f },
		{ 1.0f, 0.5f, 0.75f, 1.0f, 0.0f }, { -0.3f, -0.7f, 1.9f, 0.125f, 0.625f },
		{ 2.0f, 3.0f, 5.0f, 0.5f, 0.25f }, { -7.0f, 0.1f, 0.2f, 0.75f, 0.5f },
		{ 0.0f, 0.0f, 1.0f, 0.375f, 0.875f }, { 1.5f, -2.5f, -3.5f, 0.0625f, 0.9375f },
	};
	const float seconds = 1.7f;
	const unsigned in0 = COMPILE_PAYLOAD_GRF + 1U;          /* after one register of push constants */
	uint32_t *spv;
	size_t words;
	struct i915_shader_ir *ir;
	struct i915_shader_binary *binary;
	static struct eu_model m;
	unsigned c, k;

	spv = load_spv("cuboid.vert.spv", &words);
	assert(drv_i915_shader_parse(spv, words, I915_STAGE_VERTEX, &ir, NULL) == 0);
	assert(drv_i915_shader_compile(ir, &binary) == 0);

	/* every register starts as junk that differs per channel, so an unset read cannot look right */
	eu_model_init(&m);
	mset(&m, COMPILE_PAYLOAD_GRF, 0U, seconds);             /* the other seven floats of the push register stay junk */
	for (c = 0U; c < 8U; c++) {
		for (k = 0U; k < 3U; k++)
			mset(&m, in0 + k, c, vertices[c][k]);
		mset(&m, in0 + 4U, c, vertices[c][3]);
		mset(&m, in0 + 5U, c, vertices[c][4]);
	}
	eu_model_run(&m, binary);

	for (c = 0U; c < 8U; c++) {
		const float *p = vertices[c];
		float angle_x = 0.30f + 0.43f * seconds, angle_y = 0.40f + 0.70f * seconds;
		float sx = sinf(angle_x), cx = cosf(angle_x), sy = sinf(angle_y), cy = cosf(angle_y);
		float rx = p[0], ry = cx * p[1] - sx * p[2], rz = sx * p[1] + cx * p[2];
		float vx = cy * rx + sy * rz, vy = ry, vz = -sy * rx + cy * rz + 3.0f;
		float want[6];

		want[0] = 1.2f * vx;
		want[1] = -1.6f * vy;
		want[2] = (10.0f / 9.9f) * vz - (1.0f / 9.9f);
		want[3] = vz;
		want[4] = p[3];
		want[5] = p[4];
		for (k = 0U; k < 6U; k++) {
			/* the position after the VUE header, the one varying in r123.. (compile.c "Register conventions") */
			float got = mget(&m, k < 4U ? vue_position(binary) + k : COMPILE_MAX_GRF - 4U + (k - 4U), c);
			float scale = fabsf(want[k]) > 1.0f ? fabsf(want[k]) : 1.0f;

			if (fabsf(got - want[k]) > 2e-6f * scale) {
				printf("  channel %u staged output %u = %.9g want %.9g\n", c, k, (double)got, (double)want[k]);
				assert(!"EU model result differs from the GLSL source");
			}
		}
	}
	printf("  cuboid.vert.spv: the generated EU words compute gl_Position / texture_coordinate of the GLSL source for 8 vertices (instruction-semantics model, not hardware)\n");
	drv_i915_shader_binary_free(binary);
	drv_i915_shader_ir_free(ir);
	free(spv);
}

/* ------------------------------------------------------------------ p014 stage C through the EU model */

#define COMPILER_SHADERS "src/drivers/gpu/i915/tests/render/compiler-shaders"
#define FEATURE_SHADERS "src/drivers/gpu/i915/tests/render/feature-shaders"
#define MVIEW_SHADERS "userland/tests/mview/shaders"

/* Parses and compiles a shader file, printing a refusal before failing. */
static struct i915_shader_binary *
compile_file(const char *directory, const char *name, enum i915_shader_stage stage)
{
	char path[512];
	FILE *file;
	long size;
	uint32_t *code;
	struct i915_shader_ir *ir;
	struct i915_shader_binary *binary;
	struct i915_compile_diagnostic diag;
	int error;

	snprintf(path, sizeof(path), "%s/%s/%s", VK_REPO, directory, name);
	file = fopen(path, "rb");
	assert(file != NULL);
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	code = malloc((size_t)size);
	assert(fread(code, 1, (size_t)size, file) == (size_t)size);
	fclose(file);
	error = drv_i915_shader_parse(code, (size_t)size / 4U, stage, &ir, &diag);
	if (error != 0)
		printf("  %s refused: opcode %u at word %u: %s\n", name, diag.opcode, diag.word_offset,
			diag.reason != NULL ? diag.reason : "-");
	assert(error == 0);
	error = drv_i915_shader_compile(ir, &binary);
	assert(error == 0);
	drv_i915_shader_ir_free(ir);
	free(code);
	return binary;
}

/*
 * Runs a fragment kernel whose one input (location 0) is v, over eight channels:
 * v.x = bary1, v.y = bary2 per channel, v.z and v.w the same for all.  `values` receives
 * what the kernel's interpolation makes of that, per channel.
 */
static void
fs_run(struct eu_model *m, const struct i915_shader_binary *binary, unsigned mask, const float x[8],
	const float y[8], float z, float w, float values[8][4])
{
	unsigned c;

	eu_model_init(m);
	eu_model_fs_payload(m, binary, mask, x, y);
	eu_model_fs_plane(m, binary, 0U, 0U, 1.0f, 0.0f, 0.0f);
	eu_model_fs_plane(m, binary, 0U, 1U, 0.0f, 1.0f, 0.0f);
	eu_model_fs_plane(m, binary, 0U, 2U, 0.0f, 0.0f, z);
	eu_model_fs_plane(m, binary, 0U, 3U, 0.0f, 0.0f, w);
	for (c = 0U; c < 8U; c++) {
		values[c][0] = eu_model_interpolate(1.0f, 0.0f, 0.0f, x[c], y[c]);
		values[c][1] = eu_model_interpolate(0.0f, 1.0f, 0.0f, x[c], y[c]);
		values[c][2] = eu_model_interpolate(0.0f, 0.0f, z, x[c], y[c]);
		values[c][3] = eu_model_interpolate(0.0f, 0.0f, w, x[c], y[c]);
	}
	eu_model_run(m, binary);
	assert(m->ended != 0);
}

/* Compares the colour of channel c (r124..r127) with four floats, bit for bit. */
static void
expect_channel(const struct eu_model *m, const char *what, unsigned c, const float v[4], const float want[4])
{
	unsigned k;

	for (k = 0U; k < 4U; k++) {
		if (m->grf[COMPILE_MAX_GRF - 3U + k][c] != float_bits(want[k])) {
			printf("  %s channel %u at (%g, %g, %g, %g): colour.%u = %.9g want %.9g\n", what, c, (double)v[0],
				(double)v[1], (double)v[2], (double)v[3], k, (double)mget(m, COMPILE_MAX_GRF - 3U + k, c),
				(double)want[k]);
			assert(!"EU model result differs from the GLSL source");
		}
	}
}

/* The GLSL sources of the math shaders evaluated in C (the lowering the parser documents). */
static void
math_reference(const char *shader, const float v[4], float want[4])
{
	float larger, length2, scale;

	if (strcmp(shader, "unary") == 0) {
		want[0] = fabsf(v[0]);
		want[1] = floorf(v[0]);
		want[2] = v[0] - floorf(v[0]);
		want[3] = sqrtf(v[1]);
	} else if (strcmp(shader, "exponent") == 0) {
		want[0] = exp2f(v[2]);
		want[1] = log2f(v[1]);
		want[2] = exp2f(log2f(v[1]) * v[3]);
		want[3] = 1.0f / sqrtf(v[1]);
	} else if (strcmp(shader, "minmax") == 0) {
		want[0] = v[0] < v[2] ? v[0] : v[2];
		want[1] = v[0] >= v[2] ? v[0] : v[2];
		larger = v[0] >= -0.5f ? v[0] : -0.5f;
		want[2] = larger < 0.75f ? larger : 0.75f;
		want[3] = v[0] * (1.0f - v[3]) + v[2] * v[3];
	} else {
		length2 = v[0] * v[0] + v[1] * v[1];
		length2 = length2 + v[2] * v[2];
		scale = 1.0f / sqrtf(length2);
		want[0] = v[0] * (1.0f / v[1]);
		want[1] = 1.0f * (1.0f / v[2]);
		want[2] = v[0] * scale;
		want[3] = v[2] * scale;
	}
}

/* The four math shaders through SPIR-V -> IR -> EU words, eight different inputs at a time. */
static void
test_eu_glsl_math(void)
{
	static const char *const shaders[4] = { "unary", "exponent", "minmax", "divide" };
	static const float xs[16] = {
		0.0f, 1.0f, -1.0f, 2.75f, -2.75f, 7.5f, -0.3f, 0.75f, -8.0f, 5.0f, 0.25f, 3.5f, -6.125f, 1e-3f, 100.0f, -0.5f,
	};
	static const float ys[16] = {
		1.0f, 2.0f, 0.5f, 3.0f, 0.1f, 100.0f, 0.01f, 1.5f, 42.0f, 5.0f, 4096.0f, 0.75f, 7.0f, 0.3f, 16.0f, 2.5f,
	};
	static const float zs[4] = { 1.0f, -6.25f, 10.0f, 0.5f };
	static const float ws[4] = { 0.0f, 0.5f, -1.25f, 2.5f };
	struct eu_model *m;
	unsigned s, batch, i, c, runs;

	m = malloc(sizeof(*m));
	runs = 0U;
	for (s = 0U; s < 4U; s++) {
		char name[32];
		struct i915_shader_binary *binary;

		snprintf(name, sizeof(name), "%s.frag.spv", shaders[s]);
		binary = compile_file(COMPILER_SHADERS, name, I915_STAGE_FRAGMENT);
		for (batch = 0U; batch < 2U; batch++) {
			for (i = 0U; i < 4U; i++) {
				float values[8][4];

				fs_run(m, binary, 0xFFU, xs + 8U * batch, ys + 8U * ((batch + i) % 2U), zs[i], ws[i], values);
				assert(m->written == 0xFFU);
				for (c = 0U; c < 8U; c++) {
					float want[4];

					math_reference(shaders[s], values[c], want);
					expect_channel(m, name, c, values[c], want);
				}
				runs++;
			}
		}
		drv_i915_shader_binary_free(binary);
	}
	free(m);
	printf("  EU model: unary / exponent / minmax / divide shaders compute their GLSL for %u x 8 channels\n", runs);
}

/*
 * dual.frag (ws031-p032): Location 0 Index 0 and Index 1 make one SIMD8
 * dual-source render-target write that ends the thread.  Its message
 * descriptor is what Mesa 25.0.7's brw_message_desc() | brw_fb_write_desc()
 * give (mlen 4, render-target write 12 in bits 18:14, message control 2
 * "SIMD8 dual source subspan01" in 13:8, last render target, entry 0), its
 * extended descriptor src1's length 4 and render target 0, src0 r124 the
 * first colour, src1 r120 the second; the EU model's registers hold both.
 */
static void
test_dual_source(void)
{
	static const float xs[8] = { 0.0f, 0.25f, 0.5f, 1.0f, 2.0f, -1.0f, 0.75f, 3.0f };
	static const float ys[8] = { 1.0f, 0.5f, 0.0f, -2.0f, 4.0f, 0.125f, 8.0f, 0.5f };
	const uint32_t expect_desc = (4U << 25) | (12U << 14) | (2U << 8) | (1U << 12) | 0U;
	struct i915_shader_binary *binary;
	struct eu_model *m;
	const uint32_t *inst;
	const uint32_t *write;
	float values[8][4];
	uint32_t desc;
	uint32_t ex_desc;
	uint32_t index;
	unsigned sends;
	unsigned c;
	unsigned k;

	binary = compile_file(COMPILER_SHADERS, "dual.frag.spv", I915_STAGE_FRAGMENT);
	assert(binary->dual_source == 1U);

	/* The one SENDC, which ends the thread. */
	sends = 0U;
	write = NULL;
	for (index = 0U; index + 4U <= binary->code_bytes / 4U; index += 4U) {
		inst = &binary->code[index];
		if ((inst[0] & 0x7FU) != EU_OP_SENDC)
			continue;
		sends++;
		write = inst;
	}
	assert(sends == 1U && write != NULL && inst_bit(write, EU_SEND_EOT_BIT) == 1U);

	/* The descriptors, gathered from where eu-encoding-gen12.h scatters them. */
	desc = (inst_field(write, 123U, 122U) << 30) | (inst_field(write, 71U, 67U) << 25) |
	    (inst_field(write, 55U, 51U) << 20) | (inst_field(write, 121U, 113U) << 11) | inst_field(write, 91U, 81U);
	ex_desc = (inst_field(write, 103U, 99U) << 6) | (inst_field(write, 47U, 35U) << 11);
	assert(desc == expect_desc);
	assert(((ex_desc >> 6) & 0x1FU) == 4U);
	assert(((ex_desc >> 12) & 0x7U) == 0U);
	assert(inst_field(write, EU_SRC0_REG_NR_HI, EU_SRC0_REG_NR_LO) == COMPILE_MAX_GRF - 3U);
	assert(inst_field(write, EU_SRC1_REG_NR_HI, EU_SRC1_REG_NR_LO) == COMPILE_MAX_GRF - 7U);

	/* The colours in the payload: v, then (v.w, v.z, v.y, 1 - v.x). */
	m = malloc(sizeof(*m));
	fs_run(m, binary, 0xFFU, xs, ys, 0.5f, -2.0f, values);
	assert(m->written == 0xFFU);
	for (c = 0U; c < 8U; c++) {
		float second[4];

		second[0] = values[c][3];
		second[1] = values[c][2];
		second[2] = values[c][1];
		second[3] = 1.0f - values[c][0];
		for (k = 0U; k < 4U; k++) {
			assert(m->grf[COMPILE_MAX_GRF - 3U + k][c] == float_bits(values[c][k]));
			assert(m->grf[COMPILE_MAX_GRF - 7U + k][c] == float_bits(second[k]));
		}
	}
	free(m);
	drv_i915_shader_binary_free(binary);

	/* A second colour without the first is refused. */
	{
		struct i915_shader_ir *ir;
		struct i915_shader_binary *refused;
		struct i915_compile_diagnostic diag;
		char path[512];
		FILE *file;
		long size;
		uint32_t *spv;
		int error;

		snprintf(path, sizeof(path), "%s/%s/dual-alone.frag.spv", VK_REPO, COMPILER_SHADERS);
		file = fopen(path, "rb");
		assert(file != NULL);
		fseek(file, 0, SEEK_END);
		size = ftell(file);
		fseek(file, 0, SEEK_SET);
		spv = malloc((size_t)size);
		assert(fread(spv, 1, (size_t)size, file) == (size_t)size);
		fclose(file);
		error = drv_i915_shader_parse(spv, (size_t)size / 4U, I915_STAGE_FRAGMENT, &ir, &diag);
		assert(error == 0);
		error = drv_i915_shader_compile(ir, &refused);
		assert(error != 0);
		drv_i915_shader_ir_free(ir);
		free(spv);
	}
	printf("  dual source: one SENDC, desc 0x%08x (Mesa brw_fb_write_desc: mlen 4, RT write, SIMD8 dual source, last), ex_mlen 4, src0 r124, src1 r120, both colours\n", desc);
}

/*
 * 16-bit integers in an interface's memory are refused (ws031-p039): a
 * uniform block's member at offset 0 (which a 32-bit load would otherwise
 * take), a storage buffer's store, push constants, an input and an output.
 */
static void
test_int16_refusals(void)
{
	static const char *const names[5] = { "ubo.frag.spv", "ssbo.frag.spv", "push.frag.spv", "input.frag.spv", "output.frag.spv" };
	struct i915_shader_ir *ir;
	struct i915_compile_diagnostic diag;
	char path[512];
	FILE *file;
	long size;
	uint32_t *spv;
	unsigned index;
	int error;

	for (index = 0U; index < 5U; index++) {
		snprintf(path, sizeof(path), "%s/plan/ws031/tests/int16/%s", VK_REPO, names[index]);
		file = fopen(path, "rb");
		assert(file != NULL);
		fseek(file, 0, SEEK_END);
		size = ftell(file);
		fseek(file, 0, SEEK_SET);
		spv = malloc((size_t)size);
		assert(fread(spv, 1, (size_t)size, file) == (size_t)size);
		fclose(file);
		memset(&diag, 0, sizeof(diag));
		error = drv_i915_shader_parse(spv, (size_t)size / 4U, I915_STAGE_FRAGMENT, &ir, &diag);
		assert(error == ENOTSUP);
		assert(diag.reason != NULL && strstr(diag.reason, "16-bit integer") != NULL);
		free(spv);
	}
	printf("  int16: a uniform block, a storage buffer, push constants, an input and an output of 16-bit integers are refused\n");
}

/* compare.frag through the EU model: eight different pairs at a time, one of them NaN. */
static void
test_eu_comparisons(void)
{
	static const float xs[8] = { 1.0f, 2.0f, 3.0f, -4.0f, 0.0f, 0.0f, 5.5f, 1.0f };
	static const float ys[8] = { 2.0f, 1.0f, 3.0f, 7.0f, -0.0f, 0.0f, -5.5f, 1.0f };
	struct i915_shader_binary *binary;
	struct eu_model *m;
	unsigned run, c;

	m = malloc(sizeof(*m));
	binary = compile_file(COMPILER_SHADERS, "compare.frag.spv", I915_STAGE_FRAGMENT);
	for (run = 0U; run < 3U; run++) {
		float x[8], y[8], values[8][4];

		memcpy(x, xs, sizeof(x));
		memcpy(y, ys, sizeof(y));
		if (run == 2U)
			x[7] = bits_float(0x7FC00000U);     /* a NaN in one channel (it reaches v.y too: 0 * NaN) */
		fs_run(m, binary, 0xFFU, x, y, run == 0U ? 5.0f : 3.0f, run == 0U ? 3.0f : 5.0f, values);
		for (c = 0U; c < 8U; c++) {
			const float *v = values[c];
			float want[4];

			want[0] = (float)(v[0] < v[1]) + 2.0f * (float)(v[0] > v[1]) + 4.0f * (float)(v[0] <= v[1]) +
				8.0f * (float)(v[0] >= v[1]);
			want[1] = (float)(v[0] == v[1]) + 2.0f * (float)(v[0] != v[1]) +
				4.0f * (float)(v[0] < v[1] && v[2] > v[3]) + 8.0f * (float)(v[0] < v[1] || v[2] > v[3]);
			want[2] = v[0] < v[2] ? v[1] : v[3];
			want[3] = (float)(!(v[0] < v[1])) + 2.0f * (v[2] < v[3] ? v[3] : v[2]);
			if (want[2] != want[2])
				continue;               /* a NaN picked: nothing bit-exact to compare */
			expect_channel(m, "compare.frag", c, v, want);
		}
	}
	drv_i915_shader_binary_free(binary);
	free(m);
	printf("  EU model: compare.frag -- per channel <, >, <=, >=, ==, != (NaN), &&, || (phi selects), ?:, !\n");
}

/* The GLSL of branch.frag evaluated in C at one pixel centre (x, y). */
static void
branch_reference(float x, float y, float want[4])
{
	float c[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	float t = 1.0f, s;
	unsigned k;

	if (x < 21.0f) {
		if (y < 11.0f) {
			c[0] = 1.0f;
		} else {
			c[1] = 1.0f;
			t = t * 2.0f;
		}
	} else if (y < 33.0f && x > 41.0f) {
		c[2] = 1.0f;
		t = 3.0f;
	} else {
		for (k = 0U; k < 4U; k++)
			c[k] = 0.25f;
	}
	s = (x < y + 0.5f) ? t : -t;
	for (k = 0U; k < 3U; k++)
		want[k] = c[k];
	want[3] = c[3] + s;
}

/* The GLSL of discard.frag evaluated in C: returns 1 when the pixel is discarded. */
static int
discard_reference(float x, float y, float want[4])
{
	float cell = floorf(x * 0.125f) + floorf(y * 0.125f);

	if ((cell * 0.5f - floorf(cell * 0.5f)) > 0.25f)
		return 1;
	want[0] = 0.5f;
	want[1] = 0.25f;
	want[2] = 1.0f;
	want[3] = 1.0f;
	if (y < 32.0f) {
		if ((x * 0.5f - floorf(x * 0.5f)) < 0.5f && x > 16.0f)
			return 1;
		want[0] = 1.0f;
	}
	return 0;
}

/*
 * branch.frag and discard.frag at every pixel centre of 64 x 64, eight neighbouring pixels to a
 * dispatch: the branches diverge inside a dispatch, and the discard leaves every mix of live and
 * discarded channels, all eight discarded included.  A partial dispatch mask is never written.
 */
static void
test_eu_branches_and_discard(void)
{
	struct i915_shader_binary *branch, *discard;
	struct eu_model *m;
	unsigned x0, y, c, divergent, all_gone, partial;

	m = malloc(sizeof(*m));
	branch = compile_file(COMPILER_SHADERS, "branch.frag.spv", I915_STAGE_FRAGMENT);
	discard = compile_file(COMPILER_SHADERS, "discard.frag.spv", I915_STAGE_FRAGMENT);
	assert(branch->uses_kill == 0U && discard->uses_kill == 1U);
	divergent = 0U;
	all_gone = 0U;
	partial = 0U;
	for (y = 0U; y < 64U; y++) {
		for (x0 = 0U; x0 < 64U; x0 += 8U) {
			float x[8], yy[8], values[8][4];
			unsigned killed, regions, mask;

			for (c = 0U; c < 8U; c++) {
				x[c] = (float)(x0 + c) + 0.5f;
				yy[c] = (float)y + 0.5f;
			}

			fs_run(m, branch, 0xFFU, x, yy, 0.0f, 0.0f, values);
			assert(m->written == 0xFFU);
			regions = 0U;
			for (c = 0U; c < 8U; c++) {
				float want[4];

				branch_reference(values[c][0], values[c][1], want);
				expect_channel(m, "branch.frag", c, values[c], want);
				regions |= 1U << (unsigned)(want[0] + 2.0f * want[1] + 4.0f * want[2]);
			}
			if ((regions & (regions - 1U)) != 0U)
				divergent++;

			/* every dispatched pixel, then only some of them */
			for (mask = 0xFFU; ; mask = 0xA5U) {
				fs_run(m, discard, mask, x, yy, 0.0f, 0.0f, values);
				killed = 0U;
				for (c = 0U; c < 8U; c++) {
					float want[4];

					if (discard_reference(values[c][0], values[c][1], want) != 0) {
						killed |= 1U << c;
						continue;
					}
					if ((mask >> c) & 1U)
						expect_channel(m, "discard.frag", c, values[c], want);
				}
				if (m->written != (mask & ~killed & 0xFFU)) {
					printf("  discard.frag at (%u.., %u) mask 0x%02x: written 0x%02x, want 0x%02x\n", x0, y, mask,
						m->written, mask & ~killed & 0xFFU);
					assert(!"discard mask mismatch");
				}
				if (mask == 0xFFU && killed == 0xFFU)
					all_gone++;
				if (mask == 0xFFU && killed != 0U && killed != 0xFFU)
					partial++;
				if (mask == 0xA5U)
					break;
			}
		}
	}
	assert(divergent > 0U && all_gone > 0U && partial > 0U);
	drv_i915_shader_binary_free(branch);
	drv_i915_shader_binary_free(discard);
	free(m);
	printf("  EU model: branch.frag diverges in %u dispatches; discard.frag leaves %u fully discarded and %u partly discarded dispatches, each ending with the write masked to the live pixels\n",
		divergent, all_gone, partial);
}

/* cells.vert and vsmath.vert through the EU model: eight vertices at a time. */
static void
test_eu_vertex_shaders(void)
{
	static const float values[8][4] = {
		{ 1.0f, 2.0f, 2.0f, -0.5f }, { -3.0f, 0.5f, 4.0f, 0.25f }, { 0.0f, 0.0f, 1.0f, 1.5f }, { 2.5f, -1.0f, -2.0f, 0.75f },
		{ 0.3f, 0.3f, 0.3f, 1.0f }, { -7.0f, 1.0f, 0.1f, 0.0f }, { 5.0f, 5.0f, -5.0f, 0.5f }, { 0.125f, 8.0f, 2.0f, 2.0f },
	};
	struct i915_shader_binary *cells, *vsmath;
	struct eu_model *m;
	unsigned c, k;

	m = malloc(sizeof(*m));
	cells = compile_file(COMPILER_SHADERS, "cells.vert.spv", I915_STAGE_VERTEX);
	vsmath = compile_file(COMPILER_SHADERS, "vsmath.vert.spv", I915_STAGE_VERTEX);
	assert(cells->input_count == 2U && cells->varying_count == 1U && cells->push_regs == 0U);

	/* the payload: location 0 (position) in r2..r5, location 1 (value) in r6..r9 */
	eu_model_init(m);
	for (c = 0U; c < 8U; c++) {
		for (k = 0U; k < 4U; k++) {
			mset(m, COMPILE_PAYLOAD_GRF + k, c, 0.125f * (float)(c + k));
			mset(m, COMPILE_PAYLOAD_GRF + 4U + k, c, values[c][k]);
		}
	}
	eu_model_run(m, cells);
	for (c = 0U; c < 8U; c++) {
		for (k = 0U; k < 4U; k++) {
			assert(mget(m, vue_position(cells) + k, c) == 0.125f * (float)(c + k));
			assert(mget(m, COMPILE_MAX_GRF - 4U + k, c) == values[c][k]);
		}
	}

	eu_model_init(m);
	for (c = 0U; c < 8U; c++)
		for (k = 0U; k < 4U; k++)
			mset(m, COMPILE_PAYLOAD_GRF + 4U + k, c, values[c][k]);
	eu_model_run(m, vsmath);
	for (c = 0U; c < 8U; c++) {
		const float *v = values[c];
		float length2, scale, n[3], lambert, want[4];

		length2 = v[0] * v[0] + v[1] * v[1];
		length2 = length2 + v[2] * v[2];
		scale = 1.0f / sqrtf(length2);
		for (k = 0U; k < 3U; k++)
			n[k] = v[k] * scale;
		lambert = n[0] * 0.267261f + n[1] * 0.534522f;
		lambert = lambert + n[2] * 0.801784f;
		want[0] = n[0];
		want[1] = n[1];
		want[2] = lambert >= 0.0f ? lambert : 0.0f;
		want[3] = v[3] >= 0.0f ? v[3] : 0.0f;
		want[3] = want[3] < 1.0f ? want[3] : 1.0f;
		for (k = 0U; k < 4U; k++)
			assert(m->grf[COMPILE_MAX_GRF - 4U + k][c] == float_bits(want[k]));
	}
	drv_i915_shader_binary_free(cells);
	drv_i915_shader_binary_free(vsmath);
	free(m);
	printf("  EU model: cells.vert passes its value on, vsmath.vert computes normalize / max(dot) / clamp for 8 vertices\n");
}

/*
 * mview's shaders through the EU model.  mview.vert has four registers of push constants and
 * three attributes: its payload reaches r17, so its values start at r19 (the interpolation
 * temporary at r18), and nothing may write the payload.  cutout.frag discards where the texel's
 * alpha times the pushed alpha is below one half.
 */
static void
test_eu_mview(void)
{
	static const float columns[7][4] = {
		{ 0.5f, 0.0f, 0.0f, 0.0f }, { 0.0f, -0.75f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.25f, 1.0f },
		{ 0.125f, -0.25f, 0.5f, 2.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f, 0.0f },
		{ 0.0f, 0.0f, 1.0f, 0.0f },
	};
	struct i915_shader_binary *vert, *frag, *cutout;
	struct eu_model *m;
	uint32_t payload[16][8];
	unsigned c, k, r;

	m = malloc(sizeof(*m));
	vert = compile_file(MVIEW_SHADERS, "mview.vert.spv", I915_STAGE_VERTEX);
	frag = compile_file(MVIEW_SHADERS, "mview.frag.spv", I915_STAGE_FRAGMENT);
	cutout = compile_file(MVIEW_SHADERS, "cutout.frag.spv", I915_STAGE_FRAGMENT);
	assert(vert->push_regs == 4U && vert->input_count == 3U && vert->varying_count == 2U);
	assert(vert->grf_used > 19U);
	assert(frag->push_regs == 4U && frag->input_count == 2U && frag->uses_kill == 0U);
	assert(cutout->push_regs == 4U && cutout->input_count == 2U && cutout->uses_kill == 1U);

	/* mview.vert: push in r2..r5 (read as scalars), position r6.., normal r10.., texture position r14.. */
	eu_model_init(m);
	for (k = 0U; k < 7U; k++)
		for (c = 0U; c < 4U; c++)
			mset(m, COMPILE_PAYLOAD_GRF + (4U * k + c) / 8U, (4U * k + c) % 8U, columns[k][c]);
	for (c = 0U; c < 8U; c++) {
		mset(m, 6U, c, 0.5f + 0.25f * (float)c);
		mset(m, 7U, c, -1.0f + 0.125f * (float)c);
		mset(m, 8U, c, 2.0f - 0.5f * (float)c);
		mset(m, 10U, c, 0.3f - 0.1f * (float)c);
		mset(m, 11U, c, -0.6f + 0.2f * (float)c);
		mset(m, 12U, c, 0.8f);
		mset(m, 14U, c, 0.0625f * (float)c);
		mset(m, 15U, c, 1.0f - 0.0625f * (float)c);
	}
	memcpy(payload, &m->grf[COMPILE_PAYLOAD_GRF], sizeof(payload));
	eu_model_run(m, vert);
	assert(memcmp(payload, &m->grf[COMPILE_PAYLOAD_GRF], sizeof(payload)) == 0);
	for (c = 0U; c < 8U; c++) {
		float p[3], nrm[3], turned[3], length2, scale, lambert, shade, clip;

		p[0] = mget(m, 6U, c);
		p[1] = mget(m, 7U, c);
		p[2] = mget(m, 8U, c);
		nrm[0] = mget(m, 10U, c);
		nrm[1] = mget(m, 11U, c);
		nrm[2] = mget(m, 12U, c);
		for (k = 0U; k < 3U; k++) {
			turned[k] = nrm[0] * columns[4][k] + nrm[1] * columns[5][k];
			turned[k] = turned[k] + nrm[2] * columns[6][k];
		}
		length2 = turned[0] * turned[0] + turned[1] * turned[1];
		length2 = length2 + turned[2] * turned[2];
		scale = 1.0f / sqrtf(length2);
		lambert = turned[0] * scale * 0.267261f + turned[1] * scale * 0.534522f;
		lambert = lambert + turned[2] * scale * 0.801784f;
		lambert = lambert >= 0.0f ? lambert : 0.0f;
		shade = 0.35f + 0.65f * lambert;
		for (k = 0U; k < 4U; k++) {
			clip = columns[0][k] * p[0] + columns[1][k] * p[1];
			clip = clip + columns[2][k] * p[2];
			clip = clip + columns[3][k];
			assert(fabsf(mget(m, vue_position(vert) + k, c) - clip) <= 1e-6f);
		}
		/* the varyings below the handles: location 0 (texture coordinate) at r119, location 1 (shade) at r123 */
		assert(mget(m, COMPILE_MAX_GRF - 8U, c) == mget(m, 14U, c));
		assert(mget(m, COMPILE_MAX_GRF - 7U, c) == mget(m, 15U, c));
		assert(fabsf(mget(m, COMPILE_MAX_GRF - 4U, c) - shade) <= 1e-6f);
	}

	/* the fragment shaders: push in r4..r7 (the colour at byte 112: r7.4..7), texture coordinate r8, shade r10 */
	for (r = 0U; r < 2U; r++) {
		const struct i915_shader_binary *binary = r == 0U ? frag : cutout;
		float u[8], v[8];
		unsigned killed;

		for (c = 0U; c < 8U; c++) {
			u[c] = 0.1f + 0.1f * (float)c;
			v[c] = 0.9f - 0.05f * (float)c;
		}
		eu_model_init(m);
		eu_model_fs_payload(m, binary, 0xFFU, u, v);
		mset(m, COMPILE_FS_SETUP_GRF + 3U, 4U, 0.5f);
		mset(m, COMPILE_FS_SETUP_GRF + 3U, 5U, 0.75f);
		mset(m, COMPILE_FS_SETUP_GRF + 3U, 6U, 1.0f);
		mset(m, COMPILE_FS_SETUP_GRF + 3U, 7U, 0.6f);
		eu_model_fs_plane(m, binary, 0U, 0U, 1.0f, 0.0f, 0.0f);
		eu_model_fs_plane(m, binary, 0U, 1U, 0.0f, 1.0f, 0.0f);
		eu_model_fs_plane(m, binary, 1U, 0U, 0.0f, 0.0f, 0.8f);
		eu_model_run(m, binary);
		killed = 0U;
		for (c = 0U; c < 8U; c++) {
			float rgba[4], want[4], color[4] = { 0.5f, 0.75f, 1.0f, 0.6f };

			eu_model_texture(0U, u[c], v[c], rgba);
			for (k = 0U; k < 3U; k++)
				want[k] = (rgba[k] * color[k]) * 0.8f;
			want[3] = rgba[3] * color[3];
			if (r == 1U && want[3] < 0.5f) {
				killed |= 1U << c;
				continue;
			}
			if (r == 1U)
				want[3] = 1.0f;
			for (k = 0U; k < 4U; k++)
				assert(m->grf[COMPILE_MAX_GRF - 3U + k][c] == float_bits(want[k]));
		}
		assert(m->written == (0xFFU & ~killed));
		if (r == 1U)
			assert(killed != 0U && killed != 0xFFU);
	}
	drv_i915_shader_binary_free(vert);
	drv_i915_shader_binary_free(frag);
	drv_i915_shader_binary_free(cutout);
	free(m);
	printf("  EU model: mview.vert (payload to r17, values from r19, payload untouched), mview.frag, cutout.frag (discard below alpha 0.5)\n");
}

/* Every mview and stage-C shader compiles; the vkdemo kernels are what they were before stage C. */
static void
test_all_shaders_compile(void)
{
	static const char *const files[][2] = {
		{ MVIEW_SHADERS, "mview.vert.spv" }, { MVIEW_SHADERS, "mview.frag.spv" }, { MVIEW_SHADERS, "cutout.frag.spv" },
		{ COMPILER_SHADERS, "cells.vert.spv" }, { COMPILER_SHADERS, "vsmath.vert.spv" },
		{ COMPILER_SHADERS, "passthrough.frag.spv" }, { COMPILER_SHADERS, "unary.frag.spv" },
		{ COMPILER_SHADERS, "exponent.frag.spv" }, { COMPILER_SHADERS, "minmax.frag.spv" },
		{ COMPILER_SHADERS, "divide.frag.spv" }, { COMPILER_SHADERS, "compare.frag.spv" },
		{ COMPILER_SHADERS, "branch.frag.spv" }, { COMPILER_SHADERS, "discard.frag.spv" },
		{ COMPILER_SHADERS, "shade.frag.spv" },
		{ FEATURE_SHADERS, "pass.vert.spv" }, { FEATURE_SHADERS, "color.frag.spv" },
		{ FEATURE_SHADERS, "ubo.vert.spv" }, { FEATURE_SHADERS, "ubo.frag.spv" },
		{ FEATURE_SHADERS, "tex3.frag.spv" },
	};
	struct i915_shader_binary *vertex, *fragment, *textures;
	unsigned i;

	for (i = 0U; i < sizeof(files) / sizeof(files[0]); i++) {
		struct i915_shader_binary *binary;
		enum i915_shader_stage stage;

		stage = strstr(files[i][1], ".vert.") != NULL ? I915_STAGE_VERTEX : I915_STAGE_FRAGMENT;
		binary = compile_file(files[i][0], files[i][1], stage);
		assert(binary->code_bytes > 0U && binary->stage == stage);
		drv_i915_shader_binary_free(binary);
	}
	printf("  compiled: mview.vert, mview.frag, cutout.frag and the %u stage-C and feature test shaders\n", i - 3U);

	/*
	 * The uniform blocks travel with the push data: ubo.vert reads bytes 0 .. 79 of set 0 binding 0 (the
	 * mat4 and the offset), ubo.frag bytes 0 .. 15, 32 .. 47 and 80 .. 95 of set 0 binding 1, each range
	 * widened to whole 32-byte registers; neither reads push constants.
	 */
	vertex = compile_file(FEATURE_SHADERS, "ubo.vert.spv", I915_STAGE_VERTEX);
	fragment = compile_file(FEATURE_SHADERS, "ubo.frag.spv", I915_STAGE_FRAGMENT);
	assert(vertex->push_constant_bytes == 0U && vertex->block_count == 1U);
	assert(vertex->blocks[0].set == 0U && vertex->blocks[0].binding == 0U);
	assert(vertex->blocks[0].offset == 0U && vertex->blocks[0].bytes == 96U && vertex->blocks[0].push_offset == 0U);
	assert(vertex->push_regs == 3U);
	assert(fragment->push_constant_bytes == 0U && fragment->block_count == 1U);
	assert(fragment->blocks[0].set == 0U && fragment->blocks[0].binding == 1U);
	assert(fragment->blocks[0].offset == 0U && fragment->blocks[0].bytes == 96U);
	assert(fragment->push_regs == 3U && fragment->sampler_count == 0U);

	/* tex3.frag samples three images, each its own binding table entry and sampler, in the order it names them. */
	textures = compile_file(FEATURE_SHADERS, "tex3.frag.spv", I915_STAGE_FRAGMENT);
	assert(textures->sampler_count == 3U);
	assert(textures->sampler_set[0] == 0U && textures->sampler_binding[0] == 0U);
	assert(textures->sampler_set[1] == 0U && textures->sampler_binding[1] == 2U);
	assert(textures->sampler_set[2] == 1U && textures->sampler_binding[2] == 1U);
	drv_i915_shader_binary_free(vertex);
	drv_i915_shader_binary_free(fragment);
	drv_i915_shader_binary_free(textures);
	printf("  feature shaders: uniform blocks laid out in the push data (vertex 3, fragment 3 registers), 3 samplers from 2 sets\n");
}

/* ------------------------------------------------------------------ the generality test (p014 E2) */

/* Compiles one of the generality test's embedded modules. */
static struct i915_shader_binary *
compile_words(const char *name, const uint32_t *words, size_t bytes, enum i915_shader_stage stage, int *refused)
{
	struct i915_shader_ir *ir;
	struct i915_shader_binary *binary;
	struct i915_compile_diagnostic diag;
	int error;

	error = drv_i915_shader_parse(words, bytes / 4U, stage, &ir, &diag);
	if (error != 0)
		printf("  %s refused: opcode %u at word %u: %s\n", name, diag.opcode, diag.word_offset,
			diag.reason != NULL ? diag.reason : "-");
	assert(error == 0);
	error = drv_i915_shader_compile(ir, &binary);
	drv_i915_shader_ir_free(ir);
	if (refused != NULL) {
		*refused = error;
		return error == 0 ? binary : NULL;
	}
	assert(error == 0);
	return binary;
}

/* Writes bytes into a kernel's push data, 32 bytes to a register from `first`. */
static void
push_bytes(struct eu_model *m, unsigned first, unsigned offset, const uint32_t *words, unsigned bytes)
{
	unsigned k;

	for (k = 0U; k < bytes / 4U; k++)
		m->grf[first + (offset + 4U * k) / 32U][((offset + 4U * k) % 32U) / 4U] = words[k];
}

/* Delivers a binary's push constants and uniform blocks as the draw does (the blocks from the given words). */
static void
push_data(struct eu_model *m, const struct i915_shader_binary *binary, unsigned first, const uint32_t *constants,
	unsigned constant_bytes, const uint32_t *block, unsigned block_bytes)
{
	unsigned k;

	if (binary->push_constant_bytes != 0U)
		push_bytes(m, first, 0U, constants, constant_bytes);
	for (k = 0U; k < binary->block_count; k++) {
		assert(binary->blocks[k].offset + binary->blocks[k].bytes <= block_bytes + 32U);
		push_bytes(m, first, binary->blocks[k].push_offset, block + binary->blocks[k].offset / 4U,
			binary->blocks[k].offset + binary->blocks[k].bytes <= block_bytes ? binary->blocks[k].bytes :
			block_bytes - binary->blocks[k].offset);
	}
}

/* The word an RGBA8 target stores for channel c's colour. */
static uint32_t
channel_word(const struct eu_model *m, unsigned c)
{
	uint32_t word = 0U;
	unsigned k;

	for (k = 0U; k < 4U; k++) {
		float value = mget(m, COMPILE_MAX_GRF - 3U + k, c);

		value = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
		word |= (uint32_t)floorf(value * 255.0f + 0.5f) << (8U * k);
	}
	return word;
}

/*
 * The generality test's fragment shaders at every pixel of 64 x 64, eight neighbouring pixels to a dispatch, the
 * push data delivered as the draw delivers it, against the words regenerate.py computed: the loops leave the
 * channels of a dispatch at different passes, and the integer division runs in the math box.
 */
static void
test_eu_generality_fragment(void)
{
	static const struct {
		const char *name;
		const uint32_t *words;
		size_t bytes;
		const uint32_t *expected;
	} steps[8] = {
		{ "matrix.frag", i915_vke2_matrix_frag, sizeof(i915_vke2_matrix_frag), i915_vke2_matrix_expected },
		{ "int.frag", i915_vke2_int_frag, sizeof(i915_vke2_int_frag), i915_vke2_int_expected },
		{ "float.frag", i915_vke2_float_frag, sizeof(i915_vke2_float_frag), i915_vke2_float_expected },
		{ "loop.frag", i915_vke2_loop_frag, sizeof(i915_vke2_loop_frag), i915_vke2_loop_expected },
		{ "spill.frag", i915_vke2_spill_frag, sizeof(i915_vke2_spill_frag), i915_vke2_spill_expected },
		/* ws031-p024: the integer boundaries, and the undefined results (the models' words: 0, low-5-bit shifts) */
		{ "edge.frag", i915_vke2_edge_frag, sizeof(i915_vke2_edge_frag), i915_vke2_edge_expected },
		{ "undef.frag", i915_vke2_undef_frag, sizeof(i915_vke2_undef_frag), i915_vke2_undef_expected },
		/* ws031-p039: 16-bit integers whose high half the compiler leaves undefined, made whole where it matters */
		{ "int16.frag", i915_vke2_int16_frag, sizeof(i915_vke2_int16_frag), i915_vke2_int16_expected },
	};
	struct eu_model *m;
	unsigned step, x0, y, c, divergent[8], spill_bytes = 0U;

	m = malloc(sizeof(*m));
	eu_model_scratch_writes = 0U;
	eu_model_scratch_reads = 0U;
	eu_model_scratch_partial = 0U;
	for (step = 0U; step < 8U; step++) {
		struct i915_shader_binary *binary;

		binary = compile_words(steps[step].name, steps[step].words, steps[step].bytes, I915_STAGE_FRAGMENT, NULL);
		assert(binary->input_count == 1U);
		if (step == 1U || step == 3U)
			assert(has_opcode(binary, EU_OP_MATH));         /* the integer division */
		assert((step == 4U) == (binary->scratch_bytes != 0U));  /* only spill.frag spills */
		if (step == 4U)
			spill_bytes = binary->scratch_bytes;
		eu_model_divergent_whiles = 0U;
		for (y = 0U; y < 64U; y++) {
			for (x0 = 0U; x0 < 64U; x0 += 8U) {
				float x[8], yy[8], values[8][4];

				for (c = 0U; c < 8U; c++) {
					x[c] = (float)(x0 + c) + 0.5f;
					yy[c] = (float)y + 0.5f;
				}
				eu_model_init(m);
				eu_model_fs_payload(m, binary, 0xFFU, x, yy);
				eu_model_fs_plane(m, binary, 0U, 0U, 1.0f, 0.0f, 0.0f);
				eu_model_fs_plane(m, binary, 0U, 1U, 0.0f, 1.0f, 0.0f);
				eu_model_fs_plane(m, binary, 0U, 2U, 0.0f, 0.0f, 0.0f);
				eu_model_fs_plane(m, binary, 0U, 3U, 0.0f, 0.0f, 0.0f);
				push_data(m, binary, COMPILE_FS_SETUP_GRF, i915_vke2_push, sizeof(i915_vke2_push),
					i915_vke2_matrices, sizeof(i915_vke2_matrices));
				(void)values;
				eu_model_run(m, binary);
				assert(m->ended != 0 && m->written == 0xFFU);
				for (c = 0U; c < 8U; c++) {
					uint32_t got = channel_word(m, c), want = steps[step].expected[y * 64U + x0 + c];

					if (got != want) {
						printf("  %s at (%u, %u): 0x%08x, want 0x%08x\n", steps[step].name, x0 + c, y, got, want);
						assert(!"EU model result differs from regenerate.py");
					}
				}
			}
		}
		divergent[step] = eu_model_divergent_whiles;
		drv_i915_shader_binary_free(binary);
	}
	assert(divergent[3] > 0U);
	assert(divergent[4] > 0U && eu_model_scratch_writes > 0U && eu_model_scratch_reads > 0U);
	assert(eu_model_scratch_partial > 0U);          /* a spilled loop variable written while a loop had stopped channels */
	free(m);
	printf("  EU model: generality matrix / int / float / loop / spill / edge / undef / int16 shaders match regenerate.py at 8 x 4096 pixels; loop.frag: %u divergent WHILE passes; "
		"spill.frag: %u bytes of scratch a thread, %u scratch writes (%u on some channels only) and %u reads, %u divergent WHILE passes\n",
		divergent[3], spill_bytes, eu_model_scratch_writes, eu_model_scratch_partial, eu_model_scratch_reads, divergent[4]);
}

/* Sets every component of fragment input `rank` to a constant (no slope), as a varying equal at every vertex. */
static void
plane_constant(struct eu_model *m, const struct i915_shader_binary *binary, unsigned rank, const float value[4])
{
	unsigned k;

	for (k = 0U; k < 4U; k++)
		eu_model_fs_plane(m, binary, rank, k, 0.0f, 0.0f, value[k]);
}

/* A varying of vary16.vert: the seed times k + 1 plus (k, -k, k / 2, 16 - k). */
static void
vary16_value(unsigned k, float value[4])
{
	const float offset[4] = { (float)k, -(float)k, 0.5f * (float)k, 16.0f - (float)k };
	unsigned c;

	for (c = 0U; c < 4U; c++)
		value[c] = bits_float(i915_vke2_seed[c]) * (float)(k + 1U) + offset[c];
}

/*
 * The generality test's interfaces through the EU model: vary16.vert stages sixteen varyings, vin16.vert reads
 * sixteen attributes (payload r2..r65 untouched), matrix.vert places its corners by a row-major and a
 * column-major matrix of the push data; vary16.frag, subset.frag (5 of 16, routed by location) and vin16.frag
 * read them back.  vio16.vert, sixteen attributes and sixteen varyings, and a hand-made IR of the same shape do not
 * fit the registers with a staged VUE: they gather the VUE at the end and spill, and their URB writes carry what
 * the shader computed.
 */
static void
test_eu_generality_interfaces(void)
{
	static const float corners[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };
	struct i915_shader_binary *vary_vert, *vary_frag, *subset, *vin_vert, *vin_frag, *matrix_vert, *vio_vert;
	struct i915_shader_ir_inst many[32];
	struct i915_shader_ir ir;
	struct eu_model *m;
	uint32_t payload[64][8];
	unsigned c, k, rank, x0, y;

	m = malloc(sizeof(*m));
	vary_vert = compile_words("vary16.vert", i915_vke2_vary16_vert, sizeof(i915_vke2_vary16_vert), I915_STAGE_VERTEX, NULL);
	vary_frag = compile_words("vary16.frag", i915_vke2_vary16_frag, sizeof(i915_vke2_vary16_frag), I915_STAGE_FRAGMENT, NULL);
	subset = compile_words("subset.frag", i915_vke2_subset_frag, sizeof(i915_vke2_subset_frag), I915_STAGE_FRAGMENT, NULL);
	vin_vert = compile_words("vin16.vert", i915_vke2_vin16_vert, sizeof(i915_vke2_vin16_vert), I915_STAGE_VERTEX, NULL);
	vin_frag = compile_words("vin16.frag", i915_vke2_vin16_frag, sizeof(i915_vke2_vin16_frag), I915_STAGE_FRAGMENT, NULL);
	matrix_vert = compile_words("matrix.vert", i915_vke2_matrix_vert, sizeof(i915_vke2_matrix_vert), I915_STAGE_VERTEX, NULL);
	vio_vert = compile_words("vio16.vert", i915_vke2_vio16_vert, sizeof(i915_vke2_vio16_vert), I915_STAGE_VERTEX, NULL);
	assert(vio_vert->input_count == 16U && vio_vert->varying_count == 16U && vio_vert->scratch_bytes != 0U);
	assert(vary_vert->scratch_bytes == 0U && vin_vert->scratch_bytes == 0U && matrix_vert->scratch_bytes == 0U);

	/* the interfaces: sixteen varyings in location order; five of them read; sixteen attributes */
	assert(vary_vert->input_count == 3U && vary_vert->varying_count == 16U);
	for (k = 0U; k < 16U; k++)
		assert(vary_vert->varying_locations[k] == k);
	assert(vary_frag->input_count == 16U);
	assert(subset->input_count == 5U);
	assert(subset->input_locations[0] == 0U && subset->input_locations[1] == 1U && subset->input_locations[2] == 6U &&
	       subset->input_locations[3] == 11U && subset->input_locations[4] == 15U);
	assert(vin_vert->input_count == 16U && vin_vert->varying_count == 2U);
	assert(vin_vert->varying_locations[0] == 0U && vin_vert->varying_locations[1] == 3U);
	assert(vin_frag->input_count == 2U && vin_frag->input_locations[1] == 3U);

	/* vary16.vert: the position r2.., the coordinate r6.., the seed r10.. (per vertex, to tell channels apart) */
	eu_model_init(m);
	for (c = 0U; c < 8U; c++) {
		for (k = 0U; k < 4U; k++) {
			mset(m, COMPILE_PAYLOAD_GRF + k, c, 0.25f * (float)(c + k));
			mset(m, COMPILE_PAYLOAD_GRF + 4U + k, c, (float)(8U * c + k));
			mset(m, COMPILE_PAYLOAD_GRF + 8U + k, c, bits_float(i915_vke2_seed[k]) + (float)c);
		}
	}
	eu_model_run(m, vary_vert);
	for (c = 0U; c < 8U; c++) {
		for (k = 0U; k < 4U; k++) {
			assert(mget(m, vue_position(vary_vert) + k, c) == 0.25f * (float)(c + k));
			assert(mget(m, vue_position(vary_vert) + 4U + k, c) == (float)(8U * c + k));
		}
		for (rank = 1U; rank < 16U; rank++) {
			const float offset[4] = { (float)rank, -(float)rank, 0.5f * (float)rank, 16.0f - (float)rank };

			for (k = 0U; k < 4U; k++) {
				float want = (bits_float(i915_vke2_seed[k]) + (float)c) * (float)(rank + 1U) + offset[k];

				assert(mget(m, vue_position(vary_vert) + 4U + 4U * rank + k, c) == want);
			}
		}
	}

	/* vary16.frag and subset.frag: the coordinate at rank 0, each other input constant */
	for (y = 0U; y < 64U; y += 9U) {
		for (x0 = 0U; x0 < 64U; x0 += 8U) {
			static const unsigned subset_locations[5] = { 0U, 1U, 6U, 11U, 15U };
			float x[8], yy[8], value[4];

			for (c = 0U; c < 8U; c++) {
				x[c] = (float)(x0 + c) + 0.5f;
				yy[c] = (float)y + 0.5f;
			}
			eu_model_init(m);
			eu_model_fs_payload(m, vary_frag, 0xFFU, x, yy);
			eu_model_fs_plane(m, vary_frag, 0U, 0U, 1.0f, 0.0f, 0.0f);
			eu_model_fs_plane(m, vary_frag, 0U, 1U, 0.0f, 1.0f, 0.0f);
			for (rank = 1U; rank < 16U; rank++) {
				vary16_value(rank, value);
				plane_constant(m, vary_frag, rank, value);
			}
			eu_model_run(m, vary_frag);
			for (c = 0U; c < 8U; c++)
				assert(channel_word(m, c) == float_bits((float)(2U * ((x0 + c) * 1000U + y * 100000U) + I915_VKE2_VARY16_TWICE) * 0.5f));

			eu_model_init(m);
			eu_model_fs_payload(m, subset, 0xFFU, x, yy);
			eu_model_fs_plane(m, subset, 0U, 0U, 1.0f, 0.0f, 0.0f);
			eu_model_fs_plane(m, subset, 0U, 1U, 0.0f, 1.0f, 0.0f);
			for (rank = 1U; rank < 5U; rank++) {
				vary16_value(subset_locations[rank], value);
				plane_constant(m, subset, rank, value);
			}
			eu_model_run(m, subset);
			for (c = 0U; c < 8U; c++)
				assert(channel_word(m, c) == float_bits((float)(2U * ((x0 + c) * 1000U + y * 100000U) + I915_VKE2_SUBSET_TWICE) * 0.5f));
		}
	}

	/* vin16.vert: sixteen attributes in r2..r65, nothing written there; the weighted sum at location 3 (rank 1) */
	eu_model_init(m);
	for (c = 0U; c < 8U; c++) {
		mset(m, COMPILE_PAYLOAD_GRF + 3U, c, 1.0f);
		mset(m, COMPILE_PAYLOAD_GRF + 4U, c, (float)c);
		for (k = 2U; k < 16U; k++)
			for (rank = 0U; rank < 4U; rank++)
				mset(m, COMPILE_PAYLOAD_GRF + 4U * k + rank, c, bits_float(i915_vke2_vin_data[(k - 2U) * 4U + rank]));
	}
	memcpy(payload, &m->grf[COMPILE_PAYLOAD_GRF], sizeof(payload));
	eu_model_run(m, vin_vert);
	assert(memcmp(payload, &m->grf[COMPILE_PAYLOAD_GRF], sizeof(payload)) == 0);
	for (c = 0U; c < 8U; c++) {
		float sum[4];

		assert(mget(m, vue_position(vin_vert) + 4U, c) == (float)c);
		for (rank = 0U; rank < 4U; rank++) {
			sum[rank] = bits_float(i915_vke2_vin_data[rank]) * 2.0f;
			for (k = 3U; k < 16U; k++)
				sum[rank] = sum[rank] + bits_float(i915_vke2_vin_data[(k - 2U) * 4U + rank]) * (float)k;
			assert(mget(m, vue_position(vin_vert) + 8U + rank, c) == sum[rank]);
		}
		if (c == 0U) {
			float x[8], yy[8];
			unsigned d;

			/* vin16.frag, fed that sum at rank 1 */
			for (d = 0U; d < 8U; d++) {
				x[d] = (float)d + 0.5f;
				yy[d] = 3.5f;
			}
			{
				struct eu_model *f = malloc(sizeof(*f));

				eu_model_init(f);
				eu_model_fs_payload(f, vin_frag, 0xFFU, x, yy);
				eu_model_fs_plane(f, vin_frag, 0U, 0U, 1.0f, 0.0f, 0.0f);
				eu_model_fs_plane(f, vin_frag, 0U, 1U, 0.0f, 1.0f, 0.0f);
				plane_constant(f, vin_frag, 1U, sum);
				eu_model_run(f, vin_frag);
				for (d = 0U; d < 8U; d++)
					assert(channel_word(f, d) == float_bits((float)(2U * (d * 1000U + 3U * 100000U) + I915_VKE2_VIN16_TWICE) * 0.5f));
				free(f);
			}
		}
	}

	/* matrix.vert: the placement block in the push data (r2..), the corner, then the coordinate */
	assert(matrix_vert->block_count == 1U && matrix_vert->push_constant_bytes == 0U);
	eu_model_init(m);
	push_data(m, matrix_vert, COMPILE_PAYLOAD_GRF, NULL, 0U, i915_vke2_placement, sizeof(i915_vke2_placement));
	for (c = 0U; c < 8U; c++) {
		unsigned first = COMPILE_PAYLOAD_GRF + matrix_vert->push_regs;

		mset(m, first + 0U, c, bits_float(i915_vke2_matrix_corners[2U * (c % 4U)]));
		mset(m, first + 1U, c, bits_float(i915_vke2_matrix_corners[2U * (c % 4U) + 1U]));
		mset(m, first + 2U, c, 0.0f);
		mset(m, first + 3U, c, 1.0f);
	}
	eu_model_run(m, matrix_vert);
	for (c = 0U; c < 8U; c++) {
		assert(mget(m, vue_position(matrix_vert) + 0U, c) == corners[c % 4U][0]);
		assert(mget(m, vue_position(matrix_vert) + 1U, c) == corners[c % 4U][1]);
		assert(mget(m, vue_position(matrix_vert) + 3U, c) == 1.0f);
	}

	/*
	 * vio16.vert: sixteen attributes in r2..r65 (a different vertex in each channel), sixteen varyings out through
	 * the URB writes of a gathered VUE: the header zeros, the position, the coordinate, then data k + 1 weighted by
	 * k + 1 plus the next attribute.
	 */
	eu_model_init(m);
	for (c = 0U; c < 8U; c++) {
		for (k = 0U; k < 16U; k++)
			for (rank = 0U; rank < 4U; rank++)
				mset(m, COMPILE_PAYLOAD_GRF + 4U * k + rank, c, (float)(k * 10U + rank) + 0.25f * (float)c);
	}
	eu_model_scratch_writes = 0U;
	eu_model_scratch_reads = 0U;
	eu_model_run(m, vio_vert);
	assert(m->ended != 0 && m->vue_written == (1ULL << 18) - 1U);
	assert(eu_model_scratch_writes > 0U && eu_model_scratch_reads > 0U);
	for (c = 0U; c < 8U; c++) {
		for (rank = 0U; rank < 4U; rank++) {
			assert(m->vue[0][rank][c] == 0U);
			assert(bits_float(m->vue[1][rank][c]) == (float)rank + 0.25f * (float)c);
			assert(bits_float(m->vue[2][rank][c]) == (float)(10U + rank) + 0.25f * (float)c);
			for (k = 1U; k < 16U; k++) {
				unsigned weighted = k == 15U ? 2U : k + 1U, next = k == 15U ? 15U : (k == 14U ? 2U : k + 2U);
				float scale = k == 15U ? 16.0f : (float)(k + 1U);
				float want = ((float)(weighted * 10U + rank) + 0.25f * (float)c) * scale + ((float)(next * 10U + rank) + 0.25f * (float)c);

				assert(bits_float(m->vue[2U + k][rank][c]) == want);
			}
		}
	}
	printf("  EU model: vio16.vert (16 attributes, 16 varyings): gathered VUE, %u bytes of scratch a thread, %u scratch writes, %u reads; all 18 slots right\n",
		vio_vert->scratch_bytes, eu_model_scratch_writes, eu_model_scratch_reads);

	/* sixteen attributes and sixteen varyings of a hand-made IR: output k is attribute k */
	memset(many, 0, sizeof(many));
	for (k = 0U; k < 16U; k++) {
		many[2U * k].op = I915_IR_LOAD_INPUT;
		many[2U * k].dst = k;
		many[2U * k].location = k;
		many[2U * k + 1U].op = I915_IR_STORE_OUTPUT;
		many[2U * k + 1U].src[0] = k;
		many[2U * k + 1U].location = k;
	}
	memset(&ir, 0, sizeof(ir));
	ir.stage = I915_STAGE_VERTEX;
	ir.instructions = many;
	ir.instruction_count = 32U;
	ir.value_count = 16U;
	{
		struct i915_shader_binary *binary = (struct i915_shader_binary *)1;

		assert(drv_i915_shader_compile(&ir, &binary) == 0 && binary != NULL);
		eu_model_init(m);
		for (c = 0U; c < 8U; c++)
			for (k = 0U; k < 64U; k++)
				m->grf[COMPILE_PAYLOAD_GRF + k][c] = 0x1000U * k + c;
		eu_model_run(m, binary);
		assert(m->ended != 0 && m->vue_written == (1ULL << 18) - 1U);
		for (c = 0U; c < 8U; c++)
			for (k = 0U; k < 16U; k++)
				assert(m->vue[2U + k][0][c] == 0x1000U * (4U * k) + c);
		drv_i915_shader_binary_free(binary);
		ir.instruction_count = 24U;             /* twelve of each fit with the VUE staged */
		assert(drv_i915_shader_compile(&ir, &binary) == 0 && binary->scratch_bytes == 0U);
		drv_i915_shader_binary_free(binary);
	}

	drv_i915_shader_binary_free(vary_vert);
	drv_i915_shader_binary_free(vary_frag);
	drv_i915_shader_binary_free(subset);
	drv_i915_shader_binary_free(vin_vert);
	drv_i915_shader_binary_free(vin_frag);
	drv_i915_shader_binary_free(matrix_vert);
	drv_i915_shader_binary_free(vio_vert);
	free(m);
	printf("  EU model: vary16.vert stages 16 varyings, vary16.frag / subset.frag (5 of 16) read them, vin16.vert reads 16 attributes, matrix.vert places its corners; a hand-made 16 attributes + 16 varyings gathers its VUE\n");
}

/* Parses one of the compiler test's geometry shaders and compiles it after `producer`; returns the compiler's error. */
static int
compile_geometry(const char *name, const struct i915_shader_binary *producer, struct i915_shader_binary **binary)
{
	char path[512];
	FILE *file;
	long size;
	uint32_t *code;
	struct i915_shader_ir *ir;
	struct i915_compile_diagnostic diag;
	int error;

	snprintf(path, sizeof(path), "%s/%s/%s", VK_REPO, COMPILER_SHADERS, name);
	file = fopen(path, "rb");
	assert(file != NULL);
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	code = malloc((size_t)size);
	assert(fread(code, 1, (size_t)size, file) == (size_t)size);
	fclose(file);
	error = drv_i915_shader_parse(code, (size_t)size / 4U, I915_STAGE_GEOMETRY, &ir, &diag);
	if (error != 0)
		printf("  %s refused: opcode %u at word %u: %s\n", name, diag.opcode, diag.word_offset,
			diag.reason != NULL ? diag.reason : "-");
	assert(error == 0 && ir->stage == I915_STAGE_GEOMETRY);
	*binary = NULL;
	error = drv_i915_shader_compile_stage(ir, producer, binary);
	drv_i915_shader_ir_free(ir);
	free(code);
	return error;
}

/* The output entries of a geometry test's eight channels. */
static uint32_t gs_entry[8U * EU_MODEL_GS_ENTRY_DWORDS];

/* Starts a geometry run: every dword of the entries unwritten, the model's writes going to them. */
static void
gs_entry_clear(void)
{
	unsigned index;

	for (index = 0U; index < 8U * EU_MODEL_GS_ENTRY_DWORDS; index++)
		gs_entry[index] = EU_MODEL_GS_UNWRITTEN;
	eu_model_gs_entry = gs_entry;
}

/* The value the model's input VUE of vertex v of channel c's primitive holds at a slot and component. */
static float
gs_input_value(unsigned v, unsigned c, unsigned slot, unsigned k)
{
	return (float)(1000U * v + 100U * slot + 10U * k + c);
}

/*
 * ws075-p007a increment a2: a geometry shader that reads and writes but emits no vertex.  The binary carries what
 * 3DSTATE_GS and the URB need; the inputs are pulled with SIMD8 URB reads of the producer's slots, a vertex chosen
 * at run time by CMP and SEL over the handles; the thread ends by writing a vertex count of zero.  The EU model runs
 * it over eight primitives and the staged VUE holds what the shader computed.
 */
static void
test_geometry_reads(void)
{
	struct i915_shader_binary producer, *binary;
	struct eu_model *m;
	const uint32_t *inst, *last;
	unsigned count, index, reads, cmps, sends_ending, rt_writes, v, c, k, slot, i, vue, sbc;
	int error;

	/* the vertex shader before it writes locations 0 and 2: slots 2 and 3 of its VUE */
	memset(&producer, 0, sizeof(producer));
	producer.stage = I915_STAGE_VERTEX;
	producer.varying_count = 2U;
	producer.varying_locations[0] = 0U;
	producer.varying_locations[1] = 2U;

	/* without the producer, or with one that does not write location 2, its located inputs have no place */
	assert(compile_geometry("noemit.geom.spv", NULL, &binary) == ENOTSUP && binary == NULL);
	producer.varying_locations[1] = 3U;
	assert(compile_geometry("noemit.geom.spv", &producer, &binary) == ENOTSUP && binary == NULL);
	producer.varying_locations[1] = 2U;

	/* a producer that is not a vertex kernel, or a producer for another stage, is inconsistent */
	producer.stage = I915_STAGE_FRAGMENT;
	assert(compile_geometry("noemit.geom.spv", &producer, &binary) == EINVAL && binary == NULL);
	producer.stage = I915_STAGE_VERTEX;

	error = compile_geometry("noemit.geom.spv", &producer, &binary);
	assert(error == 0);

	/* what the draw programs: r0, r1, the primitive's number in r2, three handles, the push data from r6 */
	assert(binary->stage == I915_STAGE_GEOMETRY);
	assert(binary->vertices_in == 3U && binary->output_topology == I915_IR_OUTPUT_TRIANGLE_STRIP);
	assert(binary->uses_primitive_id == 1U && binary->writes_layer == 1U);
	assert(binary->dispatch_grf_start == 6U && binary->push_regs == 0U);
	assert(binary->varying_count == 2U);
	assert(binary->varying_locations[0] == 0U && binary->varying_locations[1] == I915_SHADER_LOCATION_PRIMITIVE_ID);
	assert(binary->input_count == 2U && binary->input_locations[0] == 0U && binary->input_locations[1] == 2U);

	/* a vertex is four slots (two 32-byte units), no cut bits; the entry 32 + 3 x 64 bytes, four 64-byte units */
	assert(binary->output_vertex_hwords == 2U);
	assert(binary->control_data_hwords == 0U && binary->control_data_format == COMPILE_GS_CONTROL_CUT);
	assert(binary->urb_entry_size == 4U);

	/* the URB reads, the handle choices and the end; no render-target write */
	count = binary->code_bytes / 16U;
	reads = 0U;
	cmps = 0U;
	sends_ending = 0U;
	rt_writes = 0U;
	for (index = 0U; index < count; index++) {
		inst = binary->code + index * 4U;
		if (inst_field(inst, EU_OPCODE_HI, EU_OPCODE_LO) == EU_OP_CMP)
			cmps++;
		if (inst_field(inst, EU_OPCODE_HI, EU_OPCODE_LO) == EU_OP_SENDC)
			rt_writes++;
		if (inst_field(inst, EU_OPCODE_HI, EU_OPCODE_LO) != EU_OP_SEND)
			continue;
		assert(inst_field(inst, EU_SEND_SFID_HI, EU_SEND_SFID_LO) == EU_SFID_URB);
		if (inst_bit(inst, EU_SEND_EOT_BIT) != 0U) {
			sends_ending++;
			continue;
		}
		assert((eu_model_descriptor(inst) & 0xFU) == COMPILE_URB_OPCODE_SIMD8_READ);
		reads++;
	}

	/* v_colour[i] 4, v_uv[1] 2, gl_in[2] 4, gl_in[i] 4 components; i chosen by two comparisons each of 8 (min's own aside) */
	assert(reads == 14U);
	assert(cmps >= 16U);
	assert(sends_ending == 1U && rt_writes == 0U);
	last = binary->code + (count - 1U) * 4U;
	assert(inst_bit(last, EU_SEND_EOT_BIT) != 0U);
	assert(eu_model_descriptor(last) == COMPILE_DESC_URB_WRITE(0U));
	assert(inst_field(last, EU_SRC0_REG_NR_HI, EU_SRC0_REG_NR_LO) == COMPILE_MAX_GRF);

	/* the model: channel c's primitive is number c; its vertices' VUEs hold distinct values */
	m = malloc(sizeof(*m));
	assert(m != NULL);
	eu_model_init(m);
	for (c = 0U; c < 8U; c++) {
		m->grf[COMPILE_GS_OUTPUT_HANDLES_GRF][c] = 0x08000000U | (0x40U + c);
		m->grf[COMPILE_GS_PRIMITIVE_ID_GRF][c] = c;
		for (v = 0U; v < 3U; v++)
			m->grf[3U + v][c] = EU_MODEL_GS_HANDLE + 8U * v + c;
	}
	for (v = 0U; v < 6U; v++)
		for (c = 0U; c < 8U; c++)
			for (slot = 0U; slot < EU_MODEL_VUE_SLOTS; slot++)
				for (k = 0U; k < 4U; k++)
					m->gs_input[8U * v + c][slot][k] = float_bits(gs_input_value(v, c, slot, k));
	sbc = eu_model_scoreboard_checks;
	gs_entry_clear();
	eu_model_run(m, binary);
	eu_model_gs_entry = NULL;
	assert(eu_model_scoreboard_checks == sbc + 1U);
	assert(m->ended);

	/* no vertex emitted; the staged VUE: [header: 0, layer 1][position][location 0][location 69] */
	vue = COMPILE_MAX_GRF - 4U * 4U;
	for (c = 0U; c < 8U; c++) {
		i = (c & 3U) < 2U ? (c & 3U) : 2U;
		assert(gs_entry[c * EU_MODEL_GS_ENTRY_DWORDS] == 0U);
		assert(gs_entry[c * EU_MODEL_GS_ENTRY_DWORDS + 1U] == EU_MODEL_GS_UNWRITTEN);
		assert(m->grf[vue + COMPILE_VUE_LAYER][c] == 1U);
		for (k = 0U; k < 4U; k++) {
			float colour = gs_input_value(i, c, 2U, k) + (k < 2U ? gs_input_value(1U, c, 3U, k) : 0.0f) +
				gs_input_value(2U, c, 1U, k);

			assert(m->grf[vue + 4U + k][c] == float_bits(gs_input_value(i, c, 1U, k)));
			assert(m->grf[vue + 8U + k][c] == float_bits(colour));
		}
		assert(m->grf[vue + 12U][c] == c);
		assert(m->grf[COMPILE_MAX_GRF][c] == 0x40U + c);       /* the handle without the instance bits */
	}
	free(m);
	drv_i915_shader_binary_free(binary);
	printf("  geometry (ws075-p007a a2): noemit.geom pulls 14 slots from the producer's VUEs (a vertex chosen per channel), stages its VUE, ends with a count of 0\n");
}

/*
 * Starts a geometry thread on the model: channel c's primitive is number c, the r1 handles carry instance bits,
 * vertex v's handle of channel c is EU_MODEL_GS_HANDLE + 8 v + c, every input VUE dword is gs_input_value(), and
 * the output entries are unwritten.
 */
static void
gs_model_start(struct eu_model *m, const struct i915_shader_binary *binary)
{
	unsigned handle_grf = binary->dispatch_grf_start - binary->vertices_in, v, c, slot, k;

	eu_model_init(m);
	for (c = 0U; c < 8U; c++) {
		m->grf[COMPILE_GS_OUTPUT_HANDLES_GRF][c] = 0x08000000U | (0x40U + c);
		if (binary->uses_primitive_id != 0U)
			m->grf[COMPILE_GS_PRIMITIVE_ID_GRF][c] = c;
		for (v = 0U; v < binary->vertices_in; v++)
			m->grf[handle_grf + v][c] = EU_MODEL_GS_HANDLE + 8U * v + c;
	}
	for (v = 0U; v < 6U; v++)
		for (c = 0U; c < 8U; c++)
			for (slot = 0U; slot < EU_MODEL_VUE_SLOTS; slot++)
				for (k = 0U; k < 4U; k++)
					m->gs_input[8U * v + c][slot][k] = float_bits(gs_input_value(v, c, slot, k));
	gs_entry_clear();
}

/* Runs a geometry thread started with gs_model_start(); the thread must end. */
static void
gs_model_run(struct eu_model *m, const struct i915_shader_binary *binary)
{
	eu_model_run(m, binary);
	eu_model_gs_entry = NULL;
	assert(m->ended);
}

/* The dword of channel c's output entry that holds component k of slot `slot` of the emitted vertex v. */
static uint32_t
gs_vertex_word(const struct i915_shader_binary *binary, unsigned c, unsigned v, unsigned slot, unsigned k)
{
	unsigned oword = 2U + 2U * binary->control_data_hwords + 2U * binary->output_vertex_hwords * v + slot;

	return gs_entry[c * EU_MODEL_GS_ENTRY_DWORDS + 4U * oword + k];
}

/* The vertex count and dword n of the cut bits of channel c's output entry. */
static uint32_t
gs_count(unsigned c)
{
	return gs_entry[c * EU_MODEL_GS_ENTRY_DWORDS];
}

static uint32_t
gs_cut(unsigned c, unsigned n)
{
	return gs_entry[c * EU_MODEL_GS_ENTRY_DWORDS + 8U + n];
}

/*
 * ws075-p007a increment a3: geometry shaders that emit.  Each runs on the EU model over eight primitives, and the
 * output entries hold the vertex count, the cut bits and every emitted vertex: glxtest's three shaders, WS068's
 * varyings, emits past OutputVertices (lost), emits under selections (a count per channel), and 64 and 160 cut bits
 * (a channel mask, then per-slot offsets as well).  The sends carry what Mesa's write lowering gives them.
 */
static void
test_geometry_emits(void)
{
	static const float corner[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { -1.0f, 1.0f }, { 1.0f, 1.0f } };
	struct i915_shader_binary producer, *binary;
	struct eu_model *m;
	const uint32_t *inst;
	unsigned c, v, k, i, layer, copy, n, emitted, writes, index, count;
	uint32_t want, cut;
	float colour[4] = { 0.25f, 0.5f, 0.75f, 1.0f };

	m = malloc(sizeof(*m));
	assert(m != NULL);
	memset(&producer, 0, sizeof(producer));
	producer.stage = I915_STAGE_VERTEX;

	/* points.geom: a square of four vertices around each point, of the pushed colour, then one strip end */
	assert(compile_geometry("points.geom.spv", &producer, &binary) == 0);
	assert(binary->vertices_in == 1U && binary->dispatch_grf_start == 3U && binary->push_regs == 1U);
	assert(binary->output_vertex_hwords == 2U && binary->control_data_hwords == 1U);
	assert(binary->urb_entry_size == (32U + 32U + 4U * 64U + 63U) / 64U);

	/* every vertex write: mlen 2 (handles, per-slot offsets), bit 17, after the count and the control data; none ends */
	count = binary->code_bytes / 16U;
	writes = 0U;
	for (index = 0U; index + 1U < count; index++) {
		uint32_t descriptor;

		inst = binary->code + index * 4U;
		if (inst_field(inst, EU_OPCODE_HI, EU_OPCODE_LO) != EU_OP_SEND)
			continue;
		descriptor = eu_model_descriptor(inst);
		if ((descriptor & 0xFU) != COMPILE_URB_OPCODE_SIMD8_WRITE)
			continue;
		assert(inst_bit(inst, EU_SEND_EOT_BIT) == 0U);
		if ((descriptor & COMPILE_URB_PER_SLOT_PRESENT) == 0U) {
			/* the cut bits at the end: OWord 2, the handles alone, one data register */
			assert(descriptor == ((1U << EU_DESC_MLEN_SHIFT) | EU_DESC_HEADER_PRESENT | (2U << 4) | 7U));
			assert(inst_field(inst, 103U, 99U) == 1U);
			continue;
		}
		assert(((descriptor >> EU_DESC_MLEN_SHIFT) & 0xFU) == 2U && (descriptor & EU_DESC_HEADER_PRESENT) != 0U);
		assert(inst_field(inst, EU_PRED_CONTROL_HI, EU_PRED_CONTROL_LO) != 0U);
		assert(((descriptor >> 4) & 0x7FFU) == 4U || ((descriptor >> 4) & 0x7FFU) == 6U);
		writes++;
	}
	assert(writes == 8U);             /* four emits, two writes each (slots 0-1 and slot 2) */

	gs_model_start(&m[0], binary);
	for (k = 0U; k < 4U; k++)
		m->grf[3][k] = float_bits(colour[k]);
	gs_model_run(m, binary);
	for (c = 0U; c < 8U; c++) {
		assert(gs_count(c) == 4U && gs_cut(c, 0U) == (1U << 3));
		for (v = 0U; v < 4U; v++) {
			for (k = 0U; k < 4U; k++) {
				float position = gs_input_value(0U, c, 1U, k) + (k < 2U ? corner[v][k] : 0.0f);

				assert(gs_vertex_word(binary, c, v, 0U, k) == 0U);
				assert(gs_vertex_word(binary, c, v, 1U, k) == float_bits(position));
				assert(gs_vertex_word(binary, c, v, 2U, k) == float_bits(colour[k]));
			}
		}
		assert(gs_vertex_word(binary, c, 4U, 1U, 0U) == EU_MODEL_GS_UNWRITTEN);
	}
	drv_i915_shader_binary_free(binary);

	/* adjacency.geom: blue where the fourth vertex is right of 0.5 (the even channels here), red elsewhere */
	assert(compile_geometry("adjacency.geom.spv", &producer, &binary) == 0);
	assert(binary->vertices_in == 4U && binary->dispatch_grf_start == 6U);
	gs_model_start(m, binary);
	for (c = 1U; c < 8U; c += 2U)
		m->gs_input[8U * 3U + c][1][0] = float_bits(0.25f);
	gs_model_run(m, binary);
	for (c = 0U; c < 8U; c++) {
		assert(gs_count(c) == 4U && gs_cut(c, 0U) == (1U << 3));
		for (v = 0U; v < 4U; v++) {
			assert(gs_vertex_word(binary, c, v, 1U, 0U) == float_bits(corner[v][0]));
			assert(gs_vertex_word(binary, c, v, 1U, 1U) == float_bits(corner[v][1]));
			assert(gs_vertex_word(binary, c, v, 2U, 0U) == float_bits((c & 1U) != 0U ? 1.0f : 0.0f));
			assert(gs_vertex_word(binary, c, v, 2U, 2U) == float_bits((c & 1U) != 0U ? 0.0f : 1.0f));
		}
	}
	drv_i915_shader_binary_free(binary);

	/* layers.geom: the triangle into layer 0 in green, then into layer 1 in red, a strip each */
	assert(compile_geometry("layers.geom.spv", &producer, &binary) == 0);
	assert(binary->writes_layer == 1U && binary->vertices_in == 3U);
	gs_model_start(m, binary);
	gs_model_run(m, binary);
	for (c = 0U; c < 8U; c++) {
		assert(gs_count(c) == 6U && gs_cut(c, 0U) == ((1U << 2) | (1U << 5)));
		for (v = 0U; v < 6U; v++) {
			layer = v / 3U;
			i = v % 3U;
			assert(gs_vertex_word(binary, c, v, 0U, COMPILE_VUE_LAYER) == layer);
			for (k = 0U; k < 4U; k++)
				assert(gs_vertex_word(binary, c, v, 1U, k) == float_bits(gs_input_value(i, c, 1U, k)));
			assert(gs_vertex_word(binary, c, v, 2U, 0U) == float_bits((float)layer));
			assert(gs_vertex_word(binary, c, v, 2U, 1U) == float_bits((float)(1U - layer)));
			assert(gs_vertex_word(binary, c, v, 2U, 3U) == float_bits(1.0f));
		}
	}
	drv_i915_shader_binary_free(binary);

	/* varyings.geom: located per-vertex inputs and a block of them, the primitive's number in and out, a pushed offset */
	producer.varying_count = 3U;
	producer.varying_locations[0] = 0U;
	producer.varying_locations[1] = 1U;
	producer.varying_locations[2] = 2U;
	assert(compile_geometry("varyings.geom.spv", &producer, &binary) == 0);
	assert(binary->dispatch_grf_start == 6U && binary->push_regs == 1U && binary->varying_count == 3U);
	assert(binary->varying_locations[2] == I915_SHADER_LOCATION_PRIMITIVE_ID && binary->output_vertex_hwords == 3U);
	gs_model_start(m, binary);
	m->grf[6][0] = float_bits(0.5f);
	gs_model_run(m, binary);
	for (c = 0U; c < 8U; c++) {
		assert(gs_count(c) == 6U && gs_cut(c, 0U) == ((1U << 2) | (1U << 5)));
		for (v = 0U; v < 6U; v++) {
			copy = v / 3U;
			i = v % 3U;
			assert(gs_vertex_word(binary, c, v, 0U, COMPILE_VUE_LAYER) == 0U);
			for (k = 0U; k < 4U; k++) {
				float position = gs_input_value(i, c, 1U, k) + (k == 0U ? 0.5f * (float)copy : 0.0f);

				assert(gs_vertex_word(binary, c, v, 1U, k) == float_bits(position));
			}
			for (k = 0U; k < 3U; k++) {
				float factor = k < 2U ? gs_input_value(i, c, 3U, k) : 1.0f;

				assert(gs_vertex_word(binary, c, v, 2U, k) == float_bits(gs_input_value(i, c, 2U, k) * factor));
			}
			want = float_bits(gs_input_value(i, c, 4U, 0U)) + c;
			assert(gs_vertex_word(binary, c, v, 3U, 0U) == want);
			assert(gs_vertex_word(binary, c, v, 4U, 0U) == c);
		}
	}
	drv_i915_shader_binary_free(binary);
	producer.varying_count = 0U;

	/* overflow.geom: five emits with room for three; the last two are lost */
	assert(compile_geometry("overflow.geom.spv", &producer, &binary) == 0);
	assert(binary->control_data_hwords == 0U && binary->output_vertex_hwords == 1U);
	gs_model_start(m, binary);
	gs_model_run(m, binary);
	for (c = 0U; c < 8U; c++) {
		assert(gs_count(c) == 3U);
		for (v = 0U; v < 3U; v++)
			assert(gs_vertex_word(binary, c, v, 1U, 0U) == float_bits(gs_input_value(0U, c, 1U, 0U) + (float)v));
		for (k = 0U; k < 8U; k++)
			assert(gs_vertex_word(binary, c, 3U, 0U, k) == EU_MODEL_GS_UNWRITTEN);
	}
	drv_i915_shader_binary_free(binary);

	/* emitif.geom: odd primitives emit first, all second, those below 4 third; even ones end the strip */
	assert(compile_geometry("emitif.geom.spv", &producer, &binary) == 0);
	gs_model_start(m, binary);
	gs_model_run(m, binary);
	for (c = 0U; c < 8U; c++) {
		float values[3];

		emitted = 0U;
		if ((c & 1U) != 0U)
			values[emitted++] = 1.0f;
		values[emitted++] = 2.0f;
		if (c < 4U)
			values[emitted++] = 3.0f;
		assert(gs_count(c) == emitted);
		for (v = 0U; v < emitted; v++)
			assert(gs_vertex_word(binary, c, v, 2U, 0U) == float_bits(values[v]));
		assert(gs_vertex_word(binary, c, emitted, 2U, 0U) == EU_MODEL_GS_UNWRITTEN);
		cut = 0U;
		if ((c & 1U) == 0U)
			cut = 1U << (emitted - 1U);
		assert(gs_cut(c, 0U) == cut);
	}
	drv_i915_shader_binary_free(binary);

	/* cut64.geom and cut160.geom: forty and a hundred and forty vertices, the bits a dword at a time */
	for (n = 0U; n < 2U; n++) {
		unsigned total = n == 0U ? 40U : 140U, every = n == 0U ? 3U : 5U, masked = 0U, offsets = 0U;

		assert(compile_geometry(n == 0U ? "cut64.geom.spv" : "cut160.geom.spv", &producer, &binary) == 0);
		assert(binary->control_data_hwords == 1U && binary->output_vertex_hwords == 1U);
		count = binary->code_bytes / 16U;
		for (index = 0U; index < count; index++) {
			uint32_t descriptor;

			inst = binary->code + index * 4U;
			if (inst_field(inst, EU_OPCODE_HI, EU_OPCODE_LO) != EU_OP_SEND)
				continue;
			descriptor = eu_model_descriptor(inst);
			if ((descriptor & 0xFU) != COMPILE_URB_OPCODE_SIMD8_WRITE || ((descriptor >> 4) & 0x7FFU) != 2U)
				continue;
			/* a cut-bit write: the channel mask always, per-slot offsets past 128 bits; four copies of the dword */
			assert((descriptor & COMPILE_URB_CHANNEL_MASK_PRESENT) != 0U && inst_field(inst, 103U, 99U) == 4U);
			masked++;
			if ((descriptor & COMPILE_URB_PER_SLOT_PRESENT) != 0U)
				offsets++;
		}
		assert(masked == 2U && offsets == (n == 0U ? 0U : 2U));
		gs_model_start(m, binary);
		gs_model_run(m, binary);
		for (c = 0U; c < 8U; c++) {
			assert(gs_count(c) == total);
			for (k = 0U; k <= (total - 1U) / 32U; k++) {
				want = 0U;
				for (v = 32U * k; v < total && v < 32U * (k + 1U); v++)
					if (v % every == every - 1U)
						want |= 1U << (v % 32U);
				assert(gs_cut(c, k) == want);
			}
			assert(gs_cut(c, k) == EU_MODEL_GS_UNWRITTEN);
			for (v = 0U; v < total; v++)
				assert(gs_vertex_word(binary, c, v, 1U, 0U) == float_bits(gs_input_value(0U, c, 1U, 0U) + (float)v));
		}
		drv_i915_shader_binary_free(binary);
	}

	/* an entry past 32 KiB is refused */
	assert(compile_geometry("refuse-entry.geom.spv", &producer, &binary) == ENOTSUP && binary == NULL);

	free(m);
	printf("  geometry (ws075-p007a a3): points / adjacency / layers / varyings / overflow / emitif / cut64 / cut160 emit the right vertices, counts and cut bits on 8 primitives\n");
}

/*
 * ws075-p007a increment a4: a geometry shader that spills.  spill.geom keeps 96 values read from its input vertices
 * live over three emits (each vertex sums all of them with weights of its own), more than the registers between the
 * count and cut bits and the staged VUE hold, so some live in scratch memory.  The EU model runs it over eight
 * primitives: the scratch is written and read, and every emitted vertex carries its exact sum (the values are
 * integers, so the order of the additions does not matter).
 */
static void
test_geometry_spill(void)
{
	struct i915_shader_binary producer;
	struct i915_shader_binary *binary;
	struct eu_model *m;
	unsigned c;
	unsigned v;
	unsigned k;
	float value;
	float sum;
	int error;

	/* The vertex shader before it writes the position alone. */
	memset(&producer, 0, sizeof(producer));
	producer.stage = I915_STAGE_VERTEX;

	/* Compiles the shader: a triangle in, one varying out, and scratch memory for what did not fit. */
	error = compile_geometry("spill.geom.spv", &producer, &binary);
	assert(error == 0);
	assert(binary->vertices_in == 3U);
	assert(binary->varying_count == 1U);
	assert(binary->control_data_hwords == 0U);
	assert(binary->scratch_bytes != 0U);

	/* Runs the thread over eight primitives, counting the scratch traffic. */
	m = malloc(sizeof(*m));
	assert(m != NULL);
	eu_model_scratch_writes = 0U;
	eu_model_scratch_reads = 0U;
	gs_model_start(m, binary);
	gs_model_run(m, binary);
	assert(eu_model_scratch_writes > 0U);
	assert(eu_model_scratch_reads > 0U);

	/* Every channel emitted the triangle's three vertices, each with its position and its own sum. */
	for (c = 0U; c < 8U; c++) {
		assert(gs_count(c) == 3U);
		for (v = 0U; v < 3U; v++) {
			/* The value k is component (k / 3) % 4 of vertex k % 3's position, plus k; vertex v weighs it (k + v) % 5 + 1. */
			sum = 0.0f;
			for (k = 0U; k < 96U; k++) {
				value = gs_input_value(k % 3U, c, 1U, (k / 3U) % 4U) + (float)k;
				sum += value * (float)((k + v) % 5U + 1U);
			}

			/* The vertex: gl_in[v]'s position, then the sum, its number, 0 and 1. */
			for (k = 0U; k < 4U; k++)
				assert(gs_vertex_word(binary, c, v, 1U, k) == float_bits(gs_input_value(v, c, 1U, k)));
			assert(gs_vertex_word(binary, c, v, 2U, 0U) == float_bits(sum));
			assert(gs_vertex_word(binary, c, v, 2U, 1U) == float_bits((float)v));
			assert(gs_vertex_word(binary, c, v, 2U, 2U) == float_bits(0.0f));
			assert(gs_vertex_word(binary, c, v, 2U, 3U) == float_bits(1.0f));
		}
	}

	printf("  geometry (ws075-p007a a4): spill.geom keeps 96 values over three emits with %u bytes of scratch a thread (%u scratch writes, %u reads); all 24 vertices right\n",
		binary->scratch_bytes, eu_model_scratch_writes, eu_model_scratch_reads);
	free(m);
	drv_i915_shader_binary_free(binary);
}

/*
 * ws031-p024 increment 3: spilling mixed with a texture sample in a loop and
 * a discard (plan/ws031/tests/p024/gen-spill.py).  Each shader keeps count
 * values of the pixel live over a loop whose trip count differs from channel
 * to channel and samples a texture in it, discards the channels that looped
 * twice while the values are still live, then sums t[k] * t[(7k + 5) % count].
 * spillbig needs more than 4 KiB of scratch a thread.
 */
#define P024_SHADERS "plan/ws031/tests/p024"

static void
test_p024_spill_mix(void)
{
	static const struct {
		const char *file;
		unsigned count;
	} shaders[] = { { "spilltex.frag.spv", 96U }, { "spillbig.frag.spv", 224U } };
	struct i915_shader_binary *binary;
	struct eu_model *m;
	float u[8], v[8], t[224], rgba[4], a, b, sum, want[4];
	unsigned which, c, k, i, n, killed, scratch[2];

	m = malloc(sizeof(*m));
	assert(m != NULL);
	for (which = 0U; which < 2U; which++) {
		binary = compile_file(P024_SHADERS, shaders[which].file, I915_STAGE_FRAGMENT);
		assert(binary->uses_kill == 1U && binary->sampler_count == 1U && binary->scratch_bytes != 0U);
		scratch[which] = binary->scratch_bytes;

		/* The pixel (x, y) of channel c: the input's two components are the barycentrics as given. */
		for (c = 0U; c < 8U; c++) {
			u[c] = 0.1f + 0.1f * (float)c;
			v[c] = 0.9f - 0.05f * (float)c;
		}
		eu_model_init(m);
		eu_model_fs_payload(m, binary, 0xFFU, u, v);
		eu_model_fs_plane(m, binary, 0U, 0U, 1.0f, 0.0f, 0.0f);
		eu_model_fs_plane(m, binary, 0U, 1U, 0.0f, 1.0f, 0.0f);
		eu_model_scratch_writes = 0U;
		eu_model_scratch_reads = 0U;
		eu_model_run(m, binary);
		assert(eu_model_scratch_writes > 0U && eu_model_scratch_reads > 0U);

		/* Each channel against the shader evaluated here, in its order. */
		killed = 0U;
		for (c = 0U; c < 8U; c++) {
			for (k = 0U; k < shaders[which].count; k++)
				t[k] = u[c] * (float)(k + 1U) + v[c];
			a = t[7];
			b = t[50];
			n = (unsigned)(u[c] * 8.0f) % 5U;
			for (i = 0U; i < n; i++) {
				eu_model_texture(0U, a * 0.125f, b * 0.0625f, rgba);
				a = a * 0.5f + rgba[0];
				b = b + rgba[3] - rgba[2] * 0.25f;
			}
			if (n == 2U) {
				killed |= 1U << c;
				continue;
			}
			sum = 0.0f;
			for (k = 0U; k < shaders[which].count; k++)
				sum += t[k] * t[(7U * k + 5U) % shaders[which].count];
			want[0] = sum + a;
			want[1] = a;
			want[2] = b;
			want[3] = (float)n;
			for (k = 0U; k < 4U; k++) {
				if (m->grf[COMPILE_MAX_GRF - 3U + k][c] != float_bits(want[k]))
					fprintf(stderr, "%s channel %u component %u: got %.9g want %.9g\n", shaders[which].file, c, k,
						(double)mget(m, COMPILE_MAX_GRF - 3U + k, c), (double)want[k]);
				assert(m->grf[COMPILE_MAX_GRF - 3U + k][c] == float_bits(want[k]));
			}
		}
		assert(killed == 0x04U);              /* x = 0.3 loops twice */
		assert(m->written == (0xFFU & ~killed));
		drv_i915_shader_binary_free(binary);
	}
	assert(scratch[1] > 4096U);
	free(m);
	printf("  ws031-p024 spill mix: a texture sampled in a divergent loop and a discard among spilled values, %u and %u bytes of scratch a thread\n",
		scratch[0], scratch[1]);
}

int
main(void)
{
	compile_shader("cuboid.frag.spv", I915_STAGE_FRAGMENT, 0U);
	compile_shader("cuboid.vert.spv", I915_STAGE_VERTEX, 1U);
	test_vertex_shader_generates_eu();
	test_vertex_shader_eu_computes_the_shader();
	test_fneg_and_push_operands();
	test_register_lifetime();
	test_fsub_is_add_with_second_source_negated();
	test_not_lowered_ir_is_refused();
	test_all_shaders_compile();
	test_eu_glsl_math();
	test_eu_comparisons();
	test_dual_source();
	test_int16_refusals();
	test_eu_branches_and_discard();
	test_eu_vertex_shaders();
	test_eu_mview();
	test_eu_generality_fragment();
	test_eu_generality_interfaces();
	test_geometry_reads();
	test_geometry_emits();
	test_geometry_spill();
	test_p024_spill_mix();
	assert(fixture_live == 0U);
	printf("  scoreboard: %u kernels checked (ws075-p022)\n", eu_model_scoreboard_checks);
	printf("  skippable regions and guards (ws075-p023): %u IFs run, %u jumped over\n", eu_model_ifs, eu_model_ifs_jumped);
	printf("i915 vk compile host test PASS\n");
	return 0;
}
