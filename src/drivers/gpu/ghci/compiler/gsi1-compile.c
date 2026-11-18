/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GSI1 compiler's driver and core (gsi1-compile.h): from the shared
 * scalar IR to a GSI1 program and the record of what it needs.
 *
 * GSI1 is a scalar ISA with 32 lanes to a warp, so every IR value lives in
 * one register, one 32-bit lane per invocation.  A value gets the lowest
 * free register when it is defined and gives it back after the last
 * instruction that reads it; a value read inside a loop that was made
 * before the loop lives to the loop's end, and a loop variable keeps its
 * register through every MOVE into it.  A constant has no register of its
 * own: a special operand stands for the common ones, and the others are
 * put into a scratch register where an instruction reads them, or folded
 * into its immediate.
 *
 * Control flow follows the IR: both sides of a selection run and SELECT
 * keeps what each lane took, so only loops and skippable blocks branch.
 * A loop saves EXEC, drops at each pass's end the lanes whose condition is
 * false and branches back while any lane is left, and restores EXEC after
 * it; lanes that left early keep the values they had, which is what the
 * IR's loop variables say.  A skippable block runs for every lane.  A
 * discard sets the lanes' KILLED bits,
 * which take away their side effects; they keep running.
 *
 * A program whose values do not fit the registers is not refused for it:
 * it is compiled again with its pure instructions moved down to their
 * readers (gsi1-schedule.c), and when that is not enough, once more with
 * values kept in the warp's scratch memory (gsi1-spill.c).  A program
 * that fits is compiled as the parser ordered it.
 */

#include "gsi1-private.h"

#include "drivers/gpu/compiler/optimize.h"
#include "drivers/gpu/compiler/spirv.h"

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include <stddef.h>
#include <stdint.h>

/* The two words of an instruction. */
#define GSI1_WORDS_PER_INSTRUCTION	2U

/* The instructions of the fragment program of a pipeline without a fragment stage. */
#define GSI1_EMPTY_FRAGMENT_INSTRUCTIONS 3U

/* All ones: the Boolean true of the IR and of GSI1. */
#define GSI1_TRUE			UINT32_MAX

/*
 * The bits of the floats GSI1 has special operands for: 1.0, 0.5, 2.0 and
 * -1.0.
 *
 * Values referenced: IEEE 754-2019 (the binary32 encodings of 1.0, 0.5,
 * 2.0 and -1.0).
 */
#define GSI1_FLOAT_ONE			0x3f800000U
#define GSI1_FLOAT_HALF			0x3f000000U
#define GSI1_FLOAT_TWO			0x40000000U
#define GSI1_FLOAT_MINUS_ONE		0xbf800000U

static int compile_parsed(const struct drv_gpu_shader_ir *parsed, const struct ghci_gsi1_options *options, struct ghci_gsi1_program **program, struct ghci_gsi1_diagnostic *diagnostic);
static int compile_sunk(const struct drv_gpu_shader_ir *ir, const struct ghci_gsi1_options *options, struct ghci_gsi1_program **program, struct ghci_gsi1_diagnostic *diagnostic);
static int compile_once(const struct drv_gpu_shader_ir *ir, const struct ghci_gsi1_options *options, int spill, struct ghci_gsi1_program **program, struct ghci_gsi1_diagnostic *diagnostic, int *short_of_registers);
static int prepare(struct gsi1_lowering *lowering);
static int allocate_tables(struct gsi1_lowering *lowering);
static int check_instructions(struct gsi1_lowering *lowering);
static void classify_values(struct gsi1_lowering *lowering);
static uint32_t special_operand(uint32_t bits);
static int find_dead(struct gsi1_lowering *lowering);
static void find_last_uses(struct gsi1_lowering *lowering);
static int extend_loops(struct gsi1_lowering *lowering);
static void extend_body(struct gsi1_lowering *lowering, const uint32_t *first_definition, uint32_t start, uint32_t end);
static int lower_all(struct gsi1_lowering *lowering);
static int define_destination(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t *dst);
static int lower(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_float(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_float_unary(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_conversion(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_integer(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_immediate(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst, int *done);
static int lower_compare(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_select(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_move(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_loop(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);
static int lower_skip(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);
static int lower_kill(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);
static int lower_derivative(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static void give_scratch(struct gsi1_lowering *lowering);
static void retire(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);
static void retire_inputs(struct gsi1_lowering *lowering, uint32_t upto);
static int finish(struct gsi1_lowering *lowering);
static int resolve_branches(struct gsi1_lowering *lowering);
static int build_program(struct gsi1_lowering *lowering, struct ghci_gsi1_program *program);
static void free_tables(struct gsi1_lowering *lowering);

/*
 * Compiles a SPIR-V module's stage.
 *
 * The parsed program is optimized in a copy (every block runs, the known
 * bits fold); a program the lowering refuses after the optimizer rewrote
 * it is lowered again from the parsed one, which keeps the parsed
 * program's outcome.  Returns 0 with the program, or the frontend's or the
 * lowering's error with the diagnostic filled.
 */
int
ghci_gsi1_compile(
	const uint32_t *words,
	size_t word_count,
	enum drv_gpu_shader_stage stage,
	const struct ghci_gsi1_options *options,
	struct ghci_gsi1_program **program,
	struct ghci_gsi1_diagnostic *diagnostic)
{
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_compile_diagnostic parse_diagnostic;
	int error;

	/* A refused compile never leaves an old output pointer live. */
	if (program == NULL)
		return EINVAL;
	*program = NULL;

	/* Starts with no refusal. */
	if (diagnostic != NULL)
		kern_memset(diagnostic, 0, sizeof(*diagnostic));

	/* A module without words is refused. */
	if (words == NULL)
		return EINVAL;

	/*
	 * Parses the stage with the frontend's GHCI features, and arrays of
	 * combined image samplers and of buffers: each element is a uniform of
	 * its own, which takes its own slots or resource table entry.
	 */
	ir = NULL;
	parse_diagnostic.opcode = 0U;
	parse_diagnostic.word_offset = 0U;
	parse_diagnostic.reason = NULL;
	error = drv_gpu_shader_parse_features(words,
					      word_count,
					      stage,
					      DRV_GPU_SPIRV_FEATURES_GHCI | DRV_GPU_SPIRV_FEATURE_SAMPLER_ARRAYS |
					      DRV_GPU_SPIRV_FEATURE_BUFFER_ARRAYS,
					      &ir,
					      &parse_diagnostic);
	if (error != 0) {
		if (diagnostic != NULL) {
			diagnostic->instruction = parse_diagnostic.word_offset;
			diagnostic->reason = parse_diagnostic.reason;
		}

		/* Reports why the stage could not be parsed. */
		return error;
	}

	/* Optimizes and lowers the parsed program. */
	error = compile_parsed(ir, options, program, diagnostic);

	/* The IR retires before the program is handed over. */
	drv_gpu_shader_ir_free(ir);

	/* Reports the lowering's refusal. */
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the program. */
	return 0;
}

/*
 * Compiles a parsed program.
 *
 * Returns 0 with the program in *program, EINVAL for a malformed program,
 * ENOTSUP for one the compiler does not cover (both with the diagnostic),
 * or ENOMEM.  A program short of registers as the parser ordered it is
 * compiled with its pure instructions sunk, and then with spills.
 */
int
ghci_gsi1_compile_ir(
	const struct drv_gpu_shader_ir *ir,
	const struct ghci_gsi1_options *options,
	struct ghci_gsi1_program **program,
	struct ghci_gsi1_diagnostic *diagnostic)
{
	int short_of_registers;
	int error;

	/* A refused compile never leaves an old output pointer live. */
	if (program == NULL)
		return EINVAL;
	*program = NULL;

	/* A program is needed. */
	if (ir == NULL)
		return EINVAL;

	/* The program as the parser ordered it, with registers only. */
	short_of_registers = 0;
	error = compile_once(ir, options, 0, program, diagnostic, &short_of_registers);
	if (error == 0)
		return 0;

	/* Reports a refusal for anything but the registers. */
	if (error != ENOTSUP || short_of_registers == 0)
		return error;

	/* The registers do not hold the values: sunk, and then spilled. */
	if (diagnostic != NULL)
		kern_memset(diagnostic, 0, sizeof(*diagnostic));
	error = compile_sunk(ir, options, program, diagnostic);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the program. */
	return 0;
}

/*
 * Frees a compiled program.
 */
void
ghci_gsi1_program_free(
	struct ghci_gsi1_program *program)
{
	/* Nothing to free. */
	if (program == NULL)
		return;

	/* The binary, then the record. */
	kern_free(program->words);
	kern_free(program);
}

/*
 * Makes the fragment program of a pipeline without a fragment stage: all
 * ones (r0 = 0 - 1) exported to the sample mask, which leaves the
 * coverage as it is, and the end every program has.  The registers are
 * those the rasterizer writes before the warp starts.
 */
int
ghci_gsi1_empty_fragment(
	struct ghci_gsi1_program **program)
{
	struct ghci_gsi1_program *made;
	struct gsi1_extra extra;
	uint64_t code[GSI1_EMPTY_FRAGMENT_INSTRUCTIONS];
	uint32_t index;

	/* The record and its code. */
	made = kern_calloc(1U, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->words = kern_calloc(GSI1_EMPTY_FRAGMENT_INSTRUCTIONS * GSI1_WORDS_PER_INSTRUCTION, sizeof(*made->words));
	if (made->words == NULL) {
		kern_free(made);
		return ENOMEM;
	}

	/*
	 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
	 * 12.10.3, export targets of a fragment warp: a program ends with
	 * all ones exported to 0x105, and a pipeline without a fragment
	 * stage gets a program of these two instructions).
	 */
	kern_memset(&extra, 0, sizeof(extra));
	code[0] = gsi1_encode_i(GSI1_OP_OPIMM, GSI1_F3_ADD, 0U, GSI1_ZERO, -1, 0U);
	code[1] = gsi1_encode_s(GSI1_OP_CUSTOM1, GSI1_F3_EXP, 0U, 0U, (int32_t)GSI1_EXP_SAMPLE_MASK);
	code[2] = gsi1_encode_r(GSI1_OP_CUSTOM0, GSI1_F3_END, 0U, 0U, 0U, 0U, &extra);

	/* The code, word0 then word1 of each instruction. */
	for (index = 0U; index < GSI1_EMPTY_FRAGMENT_INSTRUCTIONS; index++) {
		made->words[2U * index] = (uint32_t)code[index];
		made->words[2U * index + 1U] = (uint32_t)(code[index] >> 32);
	}
	made->word_count = GSI1_EMPTY_FRAGMENT_INSTRUCTIONS * GSI1_WORDS_PER_INSTRUCTION;
	made->instruction_count = GSI1_EMPTY_FRAGMENT_INSTRUCTIONS;

	/* A fragment program with the rasterizer's inputs and nothing else. */
	made->stage = DRV_GPU_STAGE_FRAGMENT;
	made->used_regs = GSI1_IN_SAMPLE_MASK + 1U;

	/* Succeeded: the caller owns the program. */
	*program = made;
	return 0;
}

/*
 * Describes a refusal at the instruction being lowered and returns its
 * error.
 */
int
gsi1_refuse(
	struct gsi1_lowering *lowering,
	int error,
	const char *reason)
{
	/* The diagnostic is optional. */
	if (lowering->diagnostic != NULL) {
		lowering->diagnostic->instruction = lowering->index;
		lowering->diagnostic->reason = reason;
	}

	/* Reports the error. */
	return error;
}

/*
 * Lists the values an IR instruction reads.
 *
 * Returns how many it put in sources[], or -1 for an operation the
 * compiler does not lower.
 */
int
gsi1_ir_sources(
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t *sources)
{
	uint32_t count;
	uint32_t index;

	/* The number of values read, by the operation. */
	count = 0U;
	switch (instruction->op) {
	case DRV_GPU_IR_NOP:
	case DRV_GPU_IR_SKIP_END:
	case DRV_GPU_IR_CONST:
	case DRV_GPU_IR_ICONST:
	case DRV_GPU_IR_BOOL:
	case DRV_GPU_IR_LOAD_PUSH:
	case DRV_GPU_IR_LOAD_UBO:
	case DRV_GPU_IR_LOAD_SYSTEM:
	case DRV_GPU_IR_STORAGE_SIZE:
	case DRV_GPU_IR_LOOP_BEGIN:
	case DRV_GPU_IR_LOAD_INPUT:
	case DRV_GPU_IR_BARRIER:
	case DRV_GPU_IR_FENCE:
		count = 0U;
		break;
	case DRV_GPU_IR_FNEG:
	case DRV_GPU_IR_FABS:
	case DRV_GPU_IR_RSQ:
	case DRV_GPU_IR_RCP:
	case DRV_GPU_IR_SQRT:
	case DRV_GPU_IR_EXP2:
	case DRV_GPU_IR_LOG2:
	case DRV_GPU_IR_SIN:
	case DRV_GPU_IR_COS:
	case DRV_GPU_IR_FLOOR:
	case DRV_GPU_IR_FRACT:
	case DRV_GPU_IR_FTRUNC:
	case DRV_GPU_IR_FROUND_EVEN:
	case DRV_GPU_IR_NOT:
	case DRV_GPU_IR_INEG:
	case DRV_GPU_IR_INOT:
	case DRV_GPU_IR_I2F:
	case DRV_GPU_IR_U2F:
	case DRV_GPU_IR_F2I:
	case DRV_GPU_IR_F2U:
	case DRV_GPU_IR_MOVE:
	case DRV_GPU_IR_UNPACK_HALF:
	case DRV_GPU_IR_LOOP_END:
	case DRV_GPU_IR_STORE_OUTPUT:
	case DRV_GPU_IR_KILL:
	case DRV_GPU_IR_DDX:
	case DRV_GPU_IR_DDX_FINE:
	case DRV_GPU_IR_DDY:
	case DRV_GPU_IR_DDY_FINE:
	case DRV_GPU_IR_SKIP_BEGIN:
		count = 1U;
		break;
	case DRV_GPU_IR_FADD:
	case DRV_GPU_IR_FSUB:
	case DRV_GPU_IR_FMUL:
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
	case DRV_GPU_IR_UDIV:
	case DRV_GPU_IR_UMOD:
	case DRV_GPU_IR_IDIV:
	case DRV_GPU_IR_IREM:
	case DRV_GPU_IR_SAMPLE:
		count = 2U;
		break;
	case DRV_GPU_IR_SELECT:
	case DRV_GPU_IR_SAMPLE_BIAS:
	case DRV_GPU_IR_SAMPLE_LOD:
		count = 3U;
		break;
	case DRV_GPU_IR_ATOMIC:
		/* The offset and the value, the compare of a compare-and-exchange, then the predicate. */
		count = 2U;
		if (instruction->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG)
			count++;
		if (instruction->component == DRV_GPU_IR_ATOMIC_PREDICATED)
			count++;

		/* An image's texel also reads its y and its layer, after the others. */
		for (index = 0U; index < count; index++)
			sources[index] = instruction->src[index];
		if ((instruction->image_shape & DRV_GPU_IR_IMAGE_TEXEL) != 0U) {
			sources[count] = instruction->src[0] + 1U;
			sources[count + 1U] = instruction->src[0] + 2U;
			count += 2U;
		}
		return (int)count;
	case DRV_GPU_IR_TEXTURE:
	case DRV_GPU_IR_IMAGE_READ:
		/* The message's parameters are src[1] consecutive values from src[0]. */
		if (instruction->src[1] == 0U)
			return -1;
		if (instruction->src[1] > DRV_GPU_IR_TEXTURE_MAX_PARAMS)
			return -1;

		/* Lists the parameters. */
		for (index = 0U; index < instruction->src[1]; index++)
			sources[index] = instruction->src[0] + index;
		return (int)instruction->src[1];
	case DRV_GPU_IR_IMAGE_WRITE:
		/* The src[1] coordinates from src[0], then the four texel values from src[2]. */
		if (instruction->src[1] == 0U)
			return -1;
		if (instruction->src[1] > 4U)
			return -1;

		/* Lists the coordinates, then the texel. */
		for (index = 0U; index < instruction->src[1]; index++)
			sources[index] = instruction->src[0] + index;
		for (index = 0U; index < 4U; index++)
			sources[instruction->src[1] + index] = instruction->src[2] + index;
		return (int)(instruction->src[1] + 4U);
	case DRV_GPU_IR_LOAD_STORAGE:
	case DRV_GPU_IR_LOAD_SHARED:
		/* The offset, and the predicate of a guarded load. */
		count = 1U;
		if (instruction->component != 0U)
			count = 2U;
		break;
	case DRV_GPU_IR_STORE_STORAGE:
	case DRV_GPU_IR_STORE_SHARED:
		/* The offset and the data, and the predicate of a guarded store. */
		count = 2U;
		if (instruction->component != 0U)
			count = 3U;
		break;
	default:
		/* An operation the compiler does not lower (a geometry shader's). */
		return -1;
	}

	/* The sources are the first ones of the instruction. */
	for (index = 0U; index < count; index++)
		sources[index] = instruction->src[index];

	/* Reports the count. */
	return (int)count;
}

/*
 * Reports how many values from its destination an IR instruction
 * defines: 0, 1, or a sample's 4.
 */
int
gsi1_ir_defines(
	const struct drv_gpu_shader_ir_inst *instruction)
{
	/* The operations that define nothing, and those that define four. */
	switch (instruction->op) {
	case DRV_GPU_IR_SAMPLE:
	case DRV_GPU_IR_SAMPLE_BIAS:
	case DRV_GPU_IR_SAMPLE_LOD:
	case DRV_GPU_IR_TEXTURE:
	case DRV_GPU_IR_IMAGE_READ:
		return 4;
	case DRV_GPU_IR_NOP:
	case DRV_GPU_IR_SKIP_BEGIN:
	case DRV_GPU_IR_SKIP_END:
	case DRV_GPU_IR_STORE_STORAGE:
	case DRV_GPU_IR_STORE_SHARED:
	case DRV_GPU_IR_IMAGE_WRITE:
	case DRV_GPU_IR_BARRIER:
	case DRV_GPU_IR_FENCE:
	case DRV_GPU_IR_LOOP_BEGIN:
	case DRV_GPU_IR_LOOP_END:
	case DRV_GPU_IR_STORE_OUTPUT:
	case DRV_GPU_IR_KILL:
		return 0;
	default:
		return 1;
	}
}

/* Appends one instruction to the code; a list that cannot grow is remembered. */
void
gsi1_emit(
	struct gsi1_lowering *lowering,
	uint64_t instruction)
{
	uint64_t *code;
	uint32_t *labels;
	uint32_t capacity;

	/* A list that already failed takes nothing more. */
	if (lowering->out_of_memory != 0)
		return;

	/* A full list grows. */
	if (lowering->count == lowering->capacity) {
		capacity = lowering->capacity + GSI1_LIST_GROWTH;

		/* Allocates the larger code list. */
		code = kern_calloc(capacity, sizeof(*code));
		if (code == NULL) {
			lowering->out_of_memory = 1;
			return;
		}

		/* Allocates the larger list of branch labels. */
		labels = kern_calloc(capacity, sizeof(*labels));
		if (labels == NULL) {
			kern_free(code);
			lowering->out_of_memory = 1;
			return;
		}

		/* Moves what the lists held into the new ones. */
		if (lowering->count != 0U) {
			kern_memcpy(code, lowering->code, lowering->count * sizeof(*code));
			kern_memcpy(labels, lowering->branch_label, lowering->count * sizeof(*labels));
		}
		kern_free(lowering->code);
		kern_free(lowering->branch_label);
		lowering->code = code;
		lowering->branch_label = labels;
		lowering->capacity = capacity;
	}

	/* The instruction, which is not a branch until its caller says so. */
	lowering->code[lowering->count] = instruction;
	lowering->branch_label[lowering->count] = 0U;
	lowering->count++;
}

/*
 * Inserts one instruction at a code index (a spill's store after a
 * definition made earlier): the later instructions, the labels at or after
 * the index and the definitions recorded there move up by one.
 */
void
gsi1_insert(
	struct gsi1_lowering *lowering,
	uint32_t position,
	uint64_t instruction)
{
	uint32_t index;

	/* The list grows by one at its end. */
	gsi1_emit(lowering, 0U);
	if (lowering->out_of_memory != 0)
		return;

	/* The instructions from the index move up, with their branch labels. */
	kern_memmove(&lowering->code[position + 1U], &lowering->code[position], (lowering->count - 1U - position) * sizeof(*lowering->code));
	kern_memmove(&lowering->branch_label[position + 1U],
		     &lowering->branch_label[position],
		     (lowering->count - 1U - position) * sizeof(*lowering->branch_label));

	/* The new instruction, which is not a branch. */
	lowering->code[position] = instruction;
	lowering->branch_label[position] = 0U;

	/* The labels that name an instruction from the index on name it where it is now. */
	for (index = 0U; index < lowering->label_count; index++) {
		if (lowering->label_at[index] == GSI1_NOT_DEFINED)
			continue;
		if (lowering->label_at[index] >= position)
			lowering->label_at[index]++;
	}

	/* The definitions recorded from the index on, likewise. */
	for (index = 0U; index < lowering->spill.definition_count; index++) {
		if (lowering->spill.definition[index].position >= position)
			lowering->spill.definition[index].position++;
	}

	/* The start's movi of the scratch memory's size, likewise. */
	if (lowering->spill.enabled != 0 && lowering->spill.patch >= position)
		lowering->spill.patch++;
}

/*
 * Appends a branch to a label; its offset is filled in once every label
 * is placed.
 */
void
gsi1_emit_branch(
	struct gsi1_lowering *lowering,
	uint32_t funct3,
	uint32_t rs1,
	uint32_t label)
{
	/* The branch with a zero offset for now. */
	gsi1_emit(lowering, gsi1_encode_b(funct3, rs1, 0));
	if (lowering->out_of_memory != 0)
		return;

	/* The label it goes to, plus one. */
	lowering->branch_label[lowering->count - 1U] = label + 1U;
}

/*
 * Makes a new label, not yet placed.
 *
 * Returns GSI1_NOT_DEFINED (and remembers the failure) when the table
 * could not grow.
 */
uint32_t
gsi1_new_label(
	struct gsi1_lowering *lowering)
{
	uint32_t *table;
	uint32_t capacity;
	uint32_t label;

	/* A full table grows. */
	if (lowering->label_count == lowering->label_capacity) {
		capacity = lowering->label_capacity + GSI1_LABEL_GROWTH;

		/* Allocates the larger table. */
		table = kern_calloc(capacity, sizeof(*table));
		if (table == NULL) {
			lowering->out_of_memory = 1;
			return GSI1_NOT_DEFINED;
		}

		/* Moves the labels into it. */
		if (lowering->label_count != 0U)
			kern_memcpy(table, lowering->label_at, lowering->label_count * sizeof(*table));
		kern_free(lowering->label_at);
		lowering->label_at = table;
		lowering->label_capacity = capacity;
	}

	/* The label, not placed yet. */
	label = lowering->label_count;
	lowering->label_at[label] = GSI1_NOT_DEFINED;
	lowering->label_count++;

	/* Succeeded: the label's number. */
	return label;
}

/* Places a label at the next instruction. */
void
gsi1_place_label(
	struct gsi1_lowering *lowering,
	uint32_t label)
{
	/* A label that could not be made is not placed. */
	if (label >= lowering->label_count)
		return;

	/* The label names the instruction that comes next. */
	lowering->label_at[label] = lowering->count;
}

/*
 * Takes the lowest free register.
 *
 * Returns ENOTSUP when every register of the lane is taken and none can
 * be spilled.
 */
int
gsi1_take_register(
	struct gsi1_lowering *lowering,
	uint32_t *reg)
{
	int error;

	/* A run of one. */
	error = gsi1_take_block(lowering, 1U, reg);
	if (error != 0)
		return error;

	/* Succeeded: the register is the caller's. */
	return 0;
}

/*
 * Takes the lowest run of `count` free registers (a texture instruction's
 * coordinates and its four results).
 */
int
gsi1_take_block(
	struct gsi1_lowering *lowering,
	uint32_t count,
	uint32_t *reg)
{
	uint32_t first;
	uint32_t inner;
	int free_run;
	int error;

	/* Tries each first register in turn. */
	for (first = 0U; first + count <= lowering->register_limit; first++) {
		/* The run is free when each of its registers is. */
		free_run = 1;
		for (inner = 0U; inner < count; inner++) {
			if (lowering->taken[first + inner] != 0U) {
				free_run = 0;
				break;
			}
		}

		/* A taken register ends this try. */
		if (free_run == 0)
			continue;

		/* The run is taken, and the highest register remembered. */
		for (inner = 0U; inner < count; inner++)
			lowering->taken[first + inner] = 1U;
		if (first + count > lowering->register_highest)
			lowering->register_highest = first + count;

		/* Succeeded: the run's first register. */
		*reg = first;
		return 0;
	}

	/* No run is free: a compile that spills frees one, and takes it. */
	error = gsi1_spill_make_room(lowering, count, &first);
	if (error == 0) {
		for (inner = 0U; inner < count; inner++)
			lowering->taken[first + inner] = 1U;
		if (first + count > lowering->register_highest)
			lowering->register_highest = first + count;
		*reg = first;
		return 0;
	}

	/* Reports a failure to grow a list. */
	if (error == ENOMEM)
		return error;

	/* No run can be freed: the program needs more registers than the lane has. */
	lowering->short_of_registers = 1;
	error = gsi1_refuse(lowering, ENOTSUP, "the program needs more registers than a lane has");
	return error;
}

/* Gives a register back. */
void
gsi1_give_register(
	struct gsi1_lowering *lowering,
	uint32_t reg)
{
	/* Only a register of the lane goes back. */
	if (reg >= lowering->register_limit)
		return;

	/* The register is free for the next value. */
	lowering->taken[reg] = 0U;
}

/*
 * Takes a scratch register for the instruction being lowered; it goes back
 * after the instruction.
 */
int
gsi1_scratch(
	struct gsi1_lowering *lowering,
	uint32_t *reg)
{
	int error;

	/* A run of one. */
	error = gsi1_scratch_block(lowering, 1U, reg);
	if (error != 0)
		return error;

	/* Succeeded: the scratch register is the caller's. */
	return 0;
}

/*
 * Takes a run of scratch registers for the instruction being lowered; they
 * go back after the instruction.
 */
int
gsi1_scratch_block(
	struct gsi1_lowering *lowering,
	uint32_t count,
	uint32_t *reg)
{
	uint32_t inner;
	int error;

	/* The list of scratch registers has a bound. */
	if (lowering->scratch_count + count > GSI1_SCRATCH_MAX) {
		error = gsi1_refuse(lowering, ENOTSUP, "an instruction needs more scratch registers than the compiler keeps");
		return error;
	}

	/* Takes the run. */
	error = gsi1_take_block(lowering, count, reg);
	if (error != 0)
		return error;

	/* Remembers each register of the run for its return. */
	for (inner = 0U; inner < count; inner++) {
		lowering->scratch[lowering->scratch_count] = *reg + inner;
		lowering->scratch_count++;
	}

	/* Succeeded: the run is the instruction's. */
	return 0;
}

/* Reports the scratch registers taken so far, for gsi1_scratch_release(). */
uint32_t
gsi1_scratch_mark(
	const struct gsi1_lowering *lowering)
{
	/* The count of the list. */
	return lowering->scratch_count;
}

/*
 * Gives back the scratch registers taken after a mark, for a lowering that
 * needs temporaries in turn.
 */
void
gsi1_scratch_release(
	struct gsi1_lowering *lowering,
	uint32_t mark)
{
	/* Each one taken after the mark. */
	while (lowering->scratch_count > mark) {
		lowering->scratch_count--;
		gsi1_give_register(lowering, lowering->scratch[lowering->scratch_count]);
	}
}

/*
 * Finds the operand an instruction reads a value through.
 *
 * A register value and an operand value are read where they live; a
 * constant is put into a scratch register first.
 */
int
gsi1_source(
	struct gsi1_lowering *lowering,
	uint32_t value,
	uint32_t *operand)
{
	uint32_t reg;
	int spilled;
	int error;

	/* A constant is put into a scratch register. */
	if (lowering->kind[value] == GSI1_VALUE_CONSTANT) {
		error = gsi1_scratch(lowering, &reg);
		if (error != 0)
			return error;

		/* Sets the scratch register to the constant's bits. */
		gsi1_emit_constant(lowering, reg, lowering->constant[value]);
		*operand = reg;
		return 0;
	}

	/* A value kept in the scratch memory is read into a scratch register. */
	spilled = gsi1_spill_is_spilled(lowering, value);
	if (spilled != 0 && lowering->operand[value] == GSI1_NO_OPERAND) {
		error = gsi1_spill_reload(lowering, value, operand);
		if (error != 0)
			return error;

		/* Succeeded: the value is in the scratch register. */
		return 0;
	}

	/* A value that has no operand yet is read before it is defined. */
	if (lowering->operand[value] == GSI1_NO_OPERAND) {
		error = gsi1_refuse(lowering, EINVAL, "a value is read before it is defined");
		return error;
	}

	/* The value's register stays until the instruction is done. */
	gsi1_spill_pin(lowering, value);

	/* Succeeded: the value's own operand. */
	*operand = lowering->operand[value];
	return 0;
}

/* Reports the register a value defined by the instruction being lowered lives in. */
uint32_t
gsi1_value_register(
	const struct gsi1_lowering *lowering,
	uint32_t value)
{
	/* The register define_destination() gave it. */
	return lowering->operand[value];
}

/* Reports whether a value is a constant (or a special operand of a constant), with its bits. */
int
gsi1_value_constant(
	const struct gsi1_lowering *lowering,
	uint32_t value,
	uint32_t *bits)
{
	/* A constant kept for rematerialization. */
	if (lowering->kind[value] == GSI1_VALUE_CONSTANT) {
		*bits = lowering->constant[value];
		return 1;
	}

	/* A constant that has a special operand of its own (zero, one and the others). */
	if (lowering->kind[value] == GSI1_VALUE_OPERAND &&
	    lowering->constant[value] != 0U) {
		*bits = lowering->constant[value];
		return 1;
	}

	/* A zero constant reads as the zero operand, whose bits are zero. */
	if (lowering->kind[value] == GSI1_VALUE_OPERAND &&
	    lowering->operand[value] == GSI1_ZERO) {
		*bits = 0U;
		return 1;
	}

	/* Anything else is not known when the program is compiled. */
	return 0;
}

/* Emits a copy of an operand into a register: addi rd, operand, 0. */
void
gsi1_emit_move(
	struct gsi1_lowering *lowering,
	uint32_t rd,
	uint32_t operand)
{
	/* A copy of a register onto itself is nothing. */
	if (rd == operand)
		return;

	/* The integer copy keeps every bit (a float copy would flush denormals). */
	gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_OPIMM, GSI1_F3_ADD, rd, operand, 0, 0U));
}

/*
 * Emits a register set to a constant: addi from zero for one that fits
 * the 22-bit immediate, movi otherwise.
 */
void
gsi1_emit_constant(
	struct gsi1_lowering *lowering,
	uint32_t rd,
	uint32_t bits)
{
	int fits;

	/* A small integer is an addi. */
	fits = gsi1_immediate_fits((int64_t)(int32_t)bits);
	if (fits != 0) {
		gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_OPIMM, GSI1_F3_ADD, rd, GSI1_ZERO, (int32_t)bits, 0U));
		return;
	}

	/* Any other value is a movi. */
	gsi1_emit(lowering, gsi1_encode_movi(rd, bits));
}

/* Emits an integer instruction of the OP opcode. */
void
gsi1_emit_alu(
	struct gsi1_lowering *lowering,
	uint32_t funct3,
	uint32_t funct7,
	uint32_t rd,
	uint32_t rs1,
	uint32_t rs2)
{
	/* The R form without extras. */
	gsi1_emit(lowering, gsi1_encode_r(GSI1_OP_OP, funct3, funct7, rd, rs1, rs2, NULL));
}

/* Emits an integer instruction of the OP-IMM opcode with a 22-bit immediate. */
void
gsi1_emit_alu_immediate(
	struct gsi1_lowering *lowering,
	uint32_t funct3,
	uint32_t rd,
	uint32_t rs1,
	int32_t immediate)
{
	/* The I form without a sub-operation. */
	gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_OPIMM, funct3, rd, rs1, immediate, 0U));
}

/* Emits a float instruction of the OP-FP opcode with its modifiers (GSI1_W1_* bits). */
void
gsi1_emit_float(
	struct gsi1_lowering *lowering,
	uint32_t funct7,
	uint32_t funct3,
	uint32_t rd,
	uint32_t rs1,
	uint32_t rs2,
	uint32_t modifiers)
{
	struct gsi1_extra extra;

	/* The modifiers are the only extra. */
	kern_memset(&extra, 0, sizeof(extra));
	extra.modifiers = modifiers;

	/* The R form. */
	gsi1_emit(lowering, gsi1_encode_r(GSI1_OP_OPFP, funct3, funct7, rd, rs1, rs2, &extra));
}

/* Emits a fused multiply-add family instruction: rd = rs1 x rs2 + rs3 and its variants, RNE. */
void
gsi1_emit_fused(
	struct gsi1_lowering *lowering,
	uint32_t opcode,
	uint32_t rd,
	uint32_t rs1,
	uint32_t rs2,
	uint32_t rs3,
	uint32_t modifiers)
{
	struct gsi1_extra extra;

	/* The modifiers are the only extra. */
	kern_memset(&extra, 0, sizeof(extra));
	extra.modifiers = modifiers;

	/* The R4 form with the RNE rounding mode. */
	gsi1_emit(lowering, gsi1_encode_r4(opcode, GSI1_RM_RNE, rd, rs1, rs2, rs3, &extra));
}

/* Emits one of the transcendental unit's operations (and fract, which shares its encoding). */
void
gsi1_emit_transcendental(
	struct gsi1_lowering *lowering,
	uint32_t sub,
	uint32_t rd,
	uint32_t rs1)
{
	struct gsi1_extra extra;

	/* The operation is the sub field. */
	kern_memset(&extra, 0, sizeof(extra));
	extra.sub = sub;

	/* custom-3 funct3 011. */
	gsi1_emit(lowering, gsi1_encode_r(GSI1_OP_CUSTOM3, GSI1_F3_XU, 0U, rd, rs1, 0U, &extra));
}

/* Emits an exec.* operation: exec.save writes its operand, the others read it. */
void
gsi1_emit_exec(
	struct gsi1_lowering *lowering,
	uint32_t sub,
	uint32_t operand)
{
	struct gsi1_extra extra;

	/* The operation is the sub field. */
	kern_memset(&extra, 0, sizeof(extra));
	extra.sub = sub;

	/* exec.save names its destination in rd; the others their Boolean in rs1. */
	if (sub == GSI1_EXEC_SAVE) {
		gsi1_emit(lowering, gsi1_encode_r(GSI1_OP_CUSTOM3, GSI1_F3_EXEC, 0U, operand, 0U, 0U, &extra));
	} else {
		gsi1_emit(lowering, gsi1_encode_r(GSI1_OP_CUSTOM3, GSI1_F3_EXEC, 0U, 0U, operand, 0U, &extra));
	}
}

/* Finds the interface record of an input location; NULL when the program has none. */
const struct drv_gpu_shader_ir_io *
gsi1_find_input(
	const struct drv_gpu_shader_ir *ir,
	uint32_t location)
{
	uint32_t index;

	/* Each input of the interface. */
	for (index = 0U; index < ir->input_count; index++) {
		if (ir->inputs[index].location == location)
			return &ir->inputs[index];
	}

	/* No record. */
	return NULL;
}

/* Finds the interface record of an output location; NULL when the program has none. */
const struct drv_gpu_shader_ir_io *
gsi1_find_output(
	const struct drv_gpu_shader_ir *ir,
	uint32_t location)
{
	uint32_t index;

	/* Each output of the interface. */
	for (index = 0U; index < ir->output_count; index++) {
		if (ir->outputs[index].location == location)
			return &ir->outputs[index];
	}

	/* No record. */
	return NULL;
}

/*
 * Optimizes a parsed program in a copy and lowers it, falling back to the
 * parsed program when the lowering refuses the rewritten one.
 */
static int
compile_parsed(
	const struct drv_gpu_shader_ir *parsed,
	const struct ghci_gsi1_options *options,
	struct ghci_gsi1_program **program,
	struct ghci_gsi1_diagnostic *diagnostic)
{
	struct drv_gpu_shader_ir optimized;
	struct drv_gpu_shader_optimize_options optimize_options;
	int error;

	/* Optimizes a private copy: every block runs (nothing is skipped), and the known bits fold. */
	kern_memset(&optimize_options, 0, sizeof(optimize_options));
	optimize_options.runs_every_block = 1U;
	optimize_options.simplify = 1U;
	kern_memset(optimize_options.needed, 1, sizeof(optimize_options.needed));
	kern_memset(&optimized, 0, sizeof(optimized));
	error = drv_gpu_shader_optimize(parsed, &optimize_options, &optimized);
	if (error != 0)
		return error;

	/* Lowers the rewritten program, or the parsed one when the optimizer left it alone. */
	if (optimized.instructions != NULL) {
		error = ghci_gsi1_compile_ir(&optimized, options, program, diagnostic);
	} else {
		error = ghci_gsi1_compile_ir(parsed, options, program, diagnostic);
	}

	/* A rewritten program refused for any reason but memory is lowered from the parsed one. */
	if (optimized.instructions != NULL && error != 0 && error != ENOMEM) {
		if (diagnostic != NULL)
			kern_memset(diagnostic, 0, sizeof(*diagnostic));
		error = ghci_gsi1_compile_ir(parsed, options, program, diagnostic);
	}

	/* The optimizer's copy of the instructions is the compile's own. */
	kern_free(optimized.instructions);

	/* Reports the lowering's refusal. */
	if (error != 0)
		return error;

	/* Succeeded: the program is compiled. */
	return 0;
}

/*
 * Compiles a program with its pure instructions sunk to their readers:
 * with registers only, and when they do not hold the values, with spills.
 */
static int
compile_sunk(
	const struct drv_gpu_shader_ir *ir,
	const struct ghci_gsi1_options *options,
	struct ghci_gsi1_program **program,
	struct ghci_gsi1_diagnostic *diagnostic)
{
	struct drv_gpu_shader_ir sunk;
	struct drv_gpu_shader_ir_inst *instructions;
	int short_of_registers;
	int error;

	/* The instructions with the pure ones sunk. */
	error = gsi1_schedule_sink(ir, &instructions);
	if (error != 0)
		return error;

	/* The program with them: everything else is the parsed program's. */
	sunk = *ir;
	sunk.instructions = instructions;

	/* With registers only. */
	short_of_registers = 0;
	error = compile_once(&sunk, options, 0, program, diagnostic, &short_of_registers);

	/* Still short of registers: with spills. */
	if (error == ENOTSUP && short_of_registers != 0) {
		if (diagnostic != NULL)
			kern_memset(diagnostic, 0, sizeof(*diagnostic));
		short_of_registers = 0;
		error = compile_once(&sunk, options, 1, program, diagnostic, &short_of_registers);
	}

	/* The sunk instructions are only this compile's. */
	kern_free(instructions);

	/* Reports why the program could not be compiled. */
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the program. */
	return 0;
}

/*
 * Compiles a program once, with spills when `spill` is nonzero.
 *
 * Returns 0 with the program in *program, or the refusal; sets
 * *short_of_registers when a register was asked for and none could be
 * found.
 */
static int
compile_once(
	const struct drv_gpu_shader_ir *ir,
	const struct ghci_gsi1_options *options,
	int spill,
	struct ghci_gsi1_program **program,
	struct ghci_gsi1_diagnostic *diagnostic,
	int *short_of_registers)
{
	struct gsi1_lowering *lowering;
	struct ghci_gsi1_program *result;
	int error;

	/* The state lives in the heap: its tables are larger than a kernel stack likes. */
	lowering = kern_calloc(1U, sizeof(*lowering));
	if (lowering == NULL)
		return ENOMEM;

	/* The state starts empty with the options. */
	lowering->ir = ir;
	lowering->diagnostic = diagnostic;
	lowering->spill.enabled = spill;
	if (options != NULL)
		lowering->options = *options;
	lowering->bindings = lowering->options.bindings;

	/* Allocates the result record. */
	result = kern_calloc(1U, sizeof(*result));
	if (result == NULL) {
		kern_free(lowering);
		return ENOMEM;
	}
	result->stage = ir->stage;

	/* Checks the program and plans its values, inputs, outputs and uniforms. */
	error = prepare(lowering);

	/* Fills the inputs the start makes. */
	if (error == 0)
		error = gsi1_io_prologue(lowering);

	/* The lane's address in the scratch memory, when the compile spills. */
	if (error == 0)
		error = gsi1_spill_prologue(lowering);

	/* Lowers every instruction in order. */
	if (error == 0)
		error = lower_all(lowering);

	/* The exports and the end. */
	if (error == 0)
		error = finish(lowering);

	/* The branches find their labels. */
	if (error == 0)
		error = resolve_branches(lowering);

	/* The size of the scratch memory. */
	if (error == 0)
		error = gsi1_spill_finish(lowering);

	/* The code and the record of what the program needs. */
	if (error == 0)
		error = build_program(lowering, result);

	/* Whether the registers ran out, for the caller's next try. */
	*short_of_registers = lowering->short_of_registers;

	/* The tables are only the compilation's. */
	free_tables(lowering);
	kern_free(lowering);

	/* A refused program is never handed over. */
	if (error != 0) {
		ghci_gsi1_program_free(result);
		return error;
	}

	/* Succeeded: the caller owns the program. */
	*program = result;
	return 0;
}

/*
 * Checks the program and prepares its lowering: the tables, the kind of
 * every value, the instructions not to lower, the last read of every value
 * and input, the inputs and outputs, and the uniforms' places.
 */
static int
prepare(
	struct gsi1_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	int error;

	/* Names the module being lowered. */
	ir = lowering->ir;

	/* Compute, vertex and fragment programs are compiled. */
	if (ir->stage != DRV_GPU_STAGE_COMPUTE &&
	    ir->stage != DRV_GPU_STAGE_VERTEX &&
	    ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		error = gsi1_refuse(lowering, ENOTSUP, "only compute, vertex and fragment shaders are compiled");
		return error;
	}

	/* The registers of a lane: none given, or more than GSI1 has, reads as r0-r62. */
	lowering->register_limit = lowering->options.registers;
	if (lowering->register_limit == 0U || lowering->register_limit > GSI1_REGISTERS_MAX)
		lowering->register_limit = GSI1_REGISTERS_MAX;

	/* A fragment pipeline's samples. */
	if (lowering->options.samples == 0U)
		lowering->options.samples = 1U;

	/* A fragment program's primitive is a triangle, a line or a point. */
	if (lowering->options.primitive > GHCI_GSI1_PRIMITIVE_POINT) {
		error = gsi1_refuse(lowering, EINVAL, "a primitive kind GSI1 does not rasterize");
		return error;
	}

	/* The per-value and per-instruction tables. */
	error = allocate_tables(lowering);
	if (error != 0)
		return error;

	/* Every instruction is covered and names values of the program. */
	error = check_instructions(lowering);
	if (error != 0)
		return error;

	/* The uniforms' slots, resource table entries and dynamic offsets. */
	error = gsi1_memory_prepare(lowering);
	if (error != 0)
		return error;

	/* Each value's kind: a register, a fixed operand, or a constant. */
	classify_values(lowering);

	/* The instructions whose results nothing reads. */
	error = find_dead(lowering);
	if (error != 0)
		return error;

	/* Each value's last read, and each input's. */
	find_last_uses(lowering);

	/* The inputs and outputs of a graphics program. */
	error = gsi1_io_prepare(lowering);
	if (error != 0)
		return error;

	/* Values and inputs read inside a loop that were made before it live to the loop's end. */
	error = extend_loops(lowering);
	if (error != 0)
		return error;

	/* The outputs exported straight from where their values live. */
	gsi1_io_plan_outputs(lowering);

	/* The values' lives and readers, for the spills. */
	error = gsi1_spill_prepare(lowering);
	if (error != 0)
		return error;

	/* Succeeded: the program can be lowered. */
	lowering->index = 0U;
	return 0;
}

/* Allocates the per-value and per-instruction tables. */
static int
allocate_tables(
	struct gsi1_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	uint32_t index;

	/* Names the module being lowered. */
	ir = lowering->ir;

	/* Allocates the table of each value's kind. */
	lowering->kind = kern_calloc(ir->value_count + 1U, sizeof(*lowering->kind));
	if (lowering->kind == NULL)
		return ENOMEM;

	/* Allocates the table of each value's operand. */
	lowering->operand = kern_calloc(ir->value_count + 1U, sizeof(*lowering->operand));
	if (lowering->operand == NULL)
		return ENOMEM;

	/* Allocates the table of each constant's bits. */
	lowering->constant = kern_calloc(ir->value_count + 1U, sizeof(*lowering->constant));
	if (lowering->constant == NULL)
		return ENOMEM;

	/* Allocates the table of each value's last read. */
	lowering->last_use = kern_calloc(ir->value_count + 1U, sizeof(*lowering->last_use));
	if (lowering->last_use == NULL)
		return ENOMEM;

	/* Allocates the table of the values a MOVE writes. */
	lowering->moved = kern_calloc(ir->value_count + 1U, sizeof(*lowering->moved));
	if (lowering->moved == NULL)
		return ENOMEM;

	/* Allocates the table of the instructions that are dropped. */
	lowering->dead = kern_calloc(ir->instruction_count + 1U, sizeof(*lowering->dead));
	if (lowering->dead == NULL)
		return ENOMEM;

	/* Allocates the table of each uniform's place. */
	lowering->binding = kern_calloc(ir->uniform_count + 1U, sizeof(*lowering->binding));
	if (lowering->binding == NULL)
		return ENOMEM;

	/* No value has an operand yet. */
	for (index = 0U; index <= ir->value_count; index++)
		lowering->operand[index] = GSI1_NO_OPERAND;

	/* No input has a register yet. */
	for (index = 0U; index < GSI1_INPUT_COUNT; index++)
		lowering->input_reg[index] = GSI1_NO_OPERAND;

	/* No output channel has its value, its register or its last store yet. */
	for (index = 0U; index < GSI1_OUTPUT_CHANNELS; index++) {
		lowering->output_value[index] = GSI1_NOT_DEFINED;
		lowering->output_reg[index] = GSI1_NO_OPERAND;
		lowering->output_last[index] = GSI1_NOT_DEFINED;
	}

	/* Succeeded: the tables are ready. */
	return 0;
}

/*
 * Checks that every instruction is one the compiler lowers and names
 * values of the program, and marks the values a MOVE writes.
 */
static int
check_instructions(
	struct gsi1_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t sources[GSI1_MAX_SOURCES];
	uint32_t index;
	uint32_t source;
	int count;
	int defines;
	int error;

	/* Names the module being checked. */
	ir = lowering->ir;

	/* Each instruction in turn. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		lowering->index = index;

		/* The sources of an operation the compiler covers. */
		count = gsi1_ir_sources(instruction, sources);
		if (count < 0) {
			error = gsi1_refuse(lowering, ENOTSUP, "the operation is not compiled for GSI1");
			return error;
		}

		/* The destination is a value of the program. */
		defines = gsi1_ir_defines(instruction);
		if (defines != 0) {
			if (instruction->dst >= ir->value_count) {
				error = gsi1_refuse(lowering, EINVAL, "a destination is not a value of the program");
				return error;
			}

			/* The last of the four results of a sample is a value too. */
			if (ir->value_count - instruction->dst < (uint32_t)defines) {
				error = gsi1_refuse(lowering, EINVAL, "a destination is not a value of the program");
				return error;
			}
		}

		/* A MOVE's destination always lives in a register. */
		if (instruction->op == DRV_GPU_IR_MOVE)
			lowering->moved[instruction->dst] = 1U;

		/* Each source is a value of the program. */
		for (source = 0U; source < (uint32_t)count; source++) {
			if (sources[source] >= ir->value_count) {
				error = gsi1_refuse(lowering, EINVAL, "a source is not a value of the program");
				return error;
			}
		}
	}

	/* Succeeded: the instructions are covered. */
	return 0;
}

/*
 * Gives each value its kind.
 *
 * A constant takes a special operand when one has its bits, and is
 * rematerialized otherwise.  A compute program's built-in values are
 * special operands, and so are the group counts it reads from the system
 * buffer and a vertex program's vertex and instance index.  A push
 * constant in c0-c23 is a c operand.  Everything else (and anything a
 * MOVE writes) is a register value.
 */
static void
classify_values(
	struct gsi1_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t index;
	uint32_t dst;
	uint32_t special;
	uint32_t word;
	uint32_t bits;
	int defines;
	int known;

	/* Names the module being classified. */
	ir = lowering->ir;

	/* Each instruction that defines one value. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		dst = instruction->dst;

		/* Only the single-value definitions are looked at. */
		defines = gsi1_ir_defines(instruction);
		if (defines != 1)
			continue;

		/* A value a MOVE writes, and one met before, is a register value. */
		if (lowering->moved[dst] != 0U ||
		    lowering->kind[dst] != GSI1_VALUE_NONE) {
			lowering->kind[dst] = GSI1_VALUE_REGISTER;
			continue;
		}

		/* A constant: a special operand, or rematerialized. */
		if (instruction->op == DRV_GPU_IR_CONST ||
		    instruction->op == DRV_GPU_IR_ICONST ||
		    instruction->op == DRV_GPU_IR_BOOL) {
			bits = instruction->immediate;
			special = special_operand(bits);
			lowering->constant[dst] = bits;
			if (special != GSI1_NO_OPERAND) {
				lowering->kind[dst] = GSI1_VALUE_OPERAND;
				lowering->operand[dst] = (uint8_t)special;
			} else {
				lowering->kind[dst] = GSI1_VALUE_CONSTANT;
			}
			continue;
		}

		/* A compute program's built-in value is a special operand. */
		if (instruction->op == DRV_GPU_IR_LOAD_SYSTEM && ir->stage == DRV_GPU_STAGE_COMPUTE) {
			special = GSI1_NO_OPERAND;
			if (instruction->component == DRV_GPU_IR_SYSTEM_LOCAL_ID_X) {
				special = GSI1_LIDX;
			} else if (instruction->component == DRV_GPU_IR_SYSTEM_LOCAL_ID_Y) {
				special = GSI1_LIDY;
			} else if (instruction->component == DRV_GPU_IR_SYSTEM_LOCAL_ID_Z) {
				special = GSI1_LIDZ;
			} else if (instruction->component == DRV_GPU_IR_SYSTEM_LOCAL_INDEX) {
				special = GSI1_LIDN;
			} else if (instruction->component == DRV_GPU_IR_SYSTEM_GROUP_ID_X) {
				special = GSI1_WGX;
			} else if (instruction->component == DRV_GPU_IR_SYSTEM_GROUP_ID_Y) {
				special = GSI1_WGY;
			} else if (instruction->component == DRV_GPU_IR_SYSTEM_GROUP_ID_Z) {
				special = GSI1_WGZ;
			}

			/* A built-in GSI1 has an operand for; anything else is a register value lower() refuses. */
			if (special != GSI1_NO_OPERAND) {
				lowering->kind[dst] = GSI1_VALUE_OPERAND;
				lowering->operand[dst] = (uint8_t)special;
			} else {
				lowering->kind[dst] = GSI1_VALUE_REGISTER;
			}
			continue;
		}

		/*
		 * The group counts are read from the system buffer at offsets 0, 4
		 * and 8; GSI1 has them as special operands.
		 */
		if (instruction->op == DRV_GPU_IR_LOAD_STORAGE &&
		    instruction->component == 0U &&
		    instruction->location < ir->uniform_count &&
		    ir->uniforms[instruction->location].set == DRV_GPU_IR_SYSTEM_SET) {
			known = gsi1_value_constant(lowering, instruction->src[0], &bits);
			special = GSI1_NO_OPERAND;
			if (known != 0 && bits == 0U) {
				special = GSI1_NWGX;
			} else if (known != 0 && bits == 4U) {
				special = GSI1_NWGY;
			} else if (known != 0 && bits == 8U) {
				special = GSI1_NWGZ;
			}

			/* A count GSI1 has an operand for; any other read of the buffer is refused later. */
			if (special != GSI1_NO_OPERAND) {
				lowering->kind[dst] = GSI1_VALUE_OPERAND;
				lowering->operand[dst] = (uint8_t)special;
			} else {
				lowering->kind[dst] = GSI1_VALUE_REGISTER;
			}
			continue;
		}

		/* A vertex program's vertex and instance index are operands 120 and 121. */
		if (instruction->op == DRV_GPU_IR_LOAD_INPUT && ir->stage == DRV_GPU_STAGE_VERTEX) {
			special = GSI1_NO_OPERAND;
			if (instruction->location == DRV_GPU_SHADER_LOCATION_VERTEX_INDEX) {
				special = GSI1_VTX_INDEX;
			} else if (instruction->location == DRV_GPU_SHADER_LOCATION_INSTANCE_INDEX) {
				special = GSI1_INST_INDEX;
			}

			/* A system input is the operand; an attribute's component lives in a register. */
			if (special != GSI1_NO_OPERAND) {
				lowering->kind[dst] = GSI1_VALUE_OPERAND;
				lowering->operand[dst] = (uint8_t)special;
				continue;
			}
		}

		/* A push-constant word of c0-c23 is a c operand. */
		if (instruction->op == DRV_GPU_IR_LOAD_PUSH && (instruction->immediate & 3U) == 0U) {
			word = GSI1_ABI_BANK_PUSH_LOW + instruction->immediate / 4U;
			if (word < GSI1_ABI_PUSH_LOW_WORDS) {
				lowering->kind[dst] = GSI1_VALUE_OPERAND;
				lowering->operand[dst] = (uint8_t)GSI1_C(word);
				continue;
			}
		}

		/* Anything else lives in a register. */
		lowering->kind[dst] = GSI1_VALUE_REGISTER;
	}

	/* The results of the four-value instructions live in registers. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		defines = gsi1_ir_defines(instruction);
		if (defines != 4)
			continue;
		for (dst = 0U; dst < 4U; dst++)
			lowering->kind[instruction->dst + dst] = GSI1_VALUE_REGISTER;
	}
}

/* Reports the special operand whose value has these bits, or GSI1_NO_OPERAND. */
static uint32_t
special_operand(
	uint32_t bits)
{
	/* The special operands that read as constants. */
	switch (bits) {
	case 0U:
		return GSI1_ZERO;
	case 1U:
		return GSI1_ONEI;
	case GSI1_FLOAT_ONE:
		return GSI1_ONE;
	case GSI1_FLOAT_HALF:
		return GSI1_HALF;
	case GSI1_FLOAT_TWO:
		return GSI1_TWO;
	case GSI1_FLOAT_MINUS_ONE:
		return GSI1_MONE;
	default:
		return GSI1_NO_OPERAND;
	}
}

/*
 * Finds the instructions not to lower: those that only define values,
 * none of which any instruction still lowered reads.  Walking backwards,
 * a dropped instruction's own reads no longer count, so a chain that only
 * fed it goes too.  An atomic always runs, and so does an image write.
 */
static int
find_dead(
	struct gsi1_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t sources[GSI1_MAX_SOURCES];
	uint32_t *reads;
	uint32_t index;
	uint32_t source;
	uint32_t channel;
	int count;
	int defines;
	int unread;

	/* Names the module being scanned. */
	ir = lowering->ir;

	/* Allocates the count of readers of each value. */
	reads = kern_calloc(ir->value_count + 1U, sizeof(*reads));
	if (reads == NULL)
		return ENOMEM;

	/* How many instructions read each value. */
	for (index = 0U; index < ir->instruction_count; index++) {
		count = gsi1_ir_sources(&ir->instructions[index], sources);
		for (source = 0U; count > 0 && source < (uint32_t)count; source++)
			reads[sources[source]]++;
	}

	/* From the last instruction back: one whose values nobody reads is dropped, with its reads. */
	for (index = ir->instruction_count; index > 0U; index--) {
		instruction = &ir->instructions[index - 1U];
		defines = gsi1_ir_defines(instruction);
		if (defines == 0)
			continue;
		if (instruction->op == DRV_GPU_IR_ATOMIC)
			continue;

		/* Every value it defines is unread. */
		unread = 1;
		for (channel = 0U; channel < (uint32_t)defines; channel++) {
			if (reads[instruction->dst + channel] != 0U)
				unread = 0;
		}

		/* An instruction with a value somebody reads stays. */
		if (unread == 0)
			continue;

		/* The instruction goes, and its sources lose a reader. */
		lowering->dead[index - 1U] = 1U;
		count = gsi1_ir_sources(instruction, sources);
		for (source = 0U; count > 0 && source < (uint32_t)count; source++)
			reads[sources[source]]--;
	}

	/* Releases the read counts. */
	kern_free(reads);

	/* Succeeded: the dropped instructions are marked. */
	return 0;
}

/* Finds each value's and each input's last read by an instruction that is lowered. */
static void
find_last_uses(
	struct gsi1_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	uint32_t sources[GSI1_MAX_SOURCES];
	uint32_t inputs[GSI1_INPUT_COUNT];
	uint32_t index;
	uint32_t source;
	uint32_t input_count;
	int count;

	/* Names the module being scanned. */
	ir = lowering->ir;

	/* Each lowered instruction's reads, in order: the last one wins. */
	for (index = 0U; index < ir->instruction_count; index++) {
		if (lowering->dead[index] != 0U)
			continue;

		/* The values it reads. */
		count = gsi1_ir_sources(&ir->instructions[index], sources);
		for (source = 0U; count > 0 && source < (uint32_t)count; source++)
			lowering->last_use[sources[source]] = index + 1U;

		/* The inputs it reads. */
		input_count = gsi1_io_inputs(lowering, &ir->instructions[index], inputs);
		for (source = 0U; source < input_count; source++)
			lowering->input_until[inputs[source]] = index + 1U;
	}
}

/*
 * Makes every value and input a loop's body reads that was defined before
 * the loop live to the loop's end, so that the next pass still finds it.
 */
static int
extend_loops(
	struct gsi1_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t *first_definition;
	uint32_t starts[GSI1_LOOP_DEPTH];
	uint32_t depth;
	uint32_t index;
	uint32_t inner;
	int defines;
	int error;

	/* Names the module being scanned. */
	ir = lowering->ir;

	/* Allocates the table of each value's first definition. */
	first_definition = kern_calloc(ir->value_count + 1U, sizeof(*first_definition));
	if (first_definition == NULL)
		return ENOMEM;

	/* No value is defined before the instructions are read. */
	for (index = 0U; index <= ir->value_count; index++)
		first_definition[index] = GSI1_NOT_DEFINED;

	/* The first instruction that defines each value. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		defines = gsi1_ir_defines(instruction);
		for (inner = 0U; inner < (uint32_t)defines; inner++) {
			if (first_definition[instruction->dst + inner] == GSI1_NOT_DEFINED)
				first_definition[instruction->dst + inner] = index;
		}
	}

	/* Each loop, at its end: what its body reads that came before it lives to the end. */
	depth = 0U;
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		lowering->index = index;

		/* A loop's start is remembered. */
		if (instruction->op == DRV_GPU_IR_LOOP_BEGIN) {
			if (depth == GSI1_LOOP_DEPTH) {
				kern_free(first_definition);
				error = gsi1_refuse(lowering, ENOTSUP, "loops nested deeper than the compiler follows");
				return error;
			}

			/* Remembers where the loop starts. */
			starts[depth] = index;
			depth++;
			continue;
		}

		/* Anything but a loop's end is passed. */
		if (instruction->op != DRV_GPU_IR_LOOP_END)
			continue;

		/* An end without a start is malformed. */
		if (depth == 0U) {
			kern_free(first_definition);
			error = gsi1_refuse(lowering, EINVAL, "a loop's end without its beginning");
			return error;
		}

		/* The loop is closed; its body's reads of earlier values and inputs are extended. */
		depth--;
		extend_body(lowering, first_definition, starts[depth], index);
	}

	/* Releases the first-definition table. */
	kern_free(first_definition);

	/* Every loop must end. */
	if (depth != 0U) {
		error = gsi1_refuse(lowering, EINVAL, "a loop without its end");
		return error;
	}

	/* Succeeded: the values live through their loops. */
	return 0;
}

/*
 * Extends to the end of one loop (the instruction `end`) the life of every
 * value made before the loop's start and of every input read in its body.
 */
static void
extend_body(
	struct gsi1_lowering *lowering,
	const uint32_t *first_definition,
	uint32_t start,
	uint32_t end)
{
	const struct drv_gpu_shader_ir *ir;
	uint32_t sources[GSI1_MAX_SOURCES];
	uint32_t inputs[GSI1_INPUT_COUNT];
	uint32_t inner;
	uint32_t source;
	uint32_t input_count;
	uint32_t value;
	int count;

	/* Names the module being scanned. */
	ir = lowering->ir;

	/* Each instruction of the body that is lowered. */
	for (inner = start + 1U; inner <= end; inner++) {
		if (lowering->dead[inner] != 0U)
			continue;

		/* The values it reads that were made before the loop. */
		count = gsi1_ir_sources(&ir->instructions[inner], sources);
		for (source = 0U; count > 0 && source < (uint32_t)count; source++) {
			value = sources[source];
			if (first_definition[value] != GSI1_NOT_DEFINED && first_definition[value] >= start)
				continue;
			if (lowering->last_use[value] < end + 1U)
				lowering->last_use[value] = end + 1U;
		}

		/* The inputs it reads, which the start of the program made. */
		input_count = gsi1_io_inputs(lowering, &ir->instructions[inner], inputs);
		for (source = 0U; source < input_count; source++) {
			if (lowering->input_until[inputs[source]] < end + 1U)
				lowering->input_until[inputs[source]] = end + 1U;
		}
	}
}

/* Lowers every instruction in order, handing out and taking back the registers. */
static int
lower_all(
	struct gsi1_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t index;
	uint32_t dst;
	int error;

	/* Names the module being lowered. */
	ir = lowering->ir;

	/* Each instruction in turn. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		lowering->index = index;

		/* An instruction whose results nothing reads is not lowered. */
		if (lowering->dead[index] != 0U)
			continue;

		/* The values the last instruction read may be spilled again. */
		gsi1_spill_next(lowering);

		/* The destination's register (none for a value that has an operand of its own). */
		error = define_destination(lowering, instruction, &dst);
		if (error != 0)
			return error;

		/* The instruction's GSI1 instructions. */
		error = lower(lowering, instruction, dst);
		if (error != 0)
			return error;

		/* Where the values it defined live, for a spill later; the spilled ones are stored. */
		error = gsi1_spill_defined(lowering, instruction);
		if (error != 0)
			return error;

		/* A list that could not grow fails the compile. */
		if (lowering->out_of_memory != 0)
			return ENOMEM;

		/* The scratch registers, and the registers of values and inputs no later instruction reads, go back. */
		give_scratch(lowering);
		retire(lowering, instruction);
		retire_inputs(lowering, index + 1U);
	}

	/* Succeeded: every instruction is lowered. */
	return 0;
}

/*
 * Gives the value an instruction defines its register: a MOVE's target
 * keeps the one it has, the four results of a sample take a run of four,
 * a value with a fixed operand or a constant takes none (dst is then
 * GSI1_NO_OPERAND).
 */
static int
define_destination(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t *dst)
{
	uint32_t reg;
	uint32_t channel;
	int defines;
	int spilled;
	int error;

	/* Nothing defined, nothing taken. */
	*dst = GSI1_NO_OPERAND;
	defines = gsi1_ir_defines(instruction);
	if (defines == 0)
		return 0;

	/* A sample's four results in a run of their own. */
	if (defines == 4) {
		error = gsi1_take_block(lowering, 4U, &reg);
		if (error != 0)
			return error;

		/* Each result names its register, which stays until the instruction is done. */
		for (channel = 0U; channel < 4U; channel++) {
			lowering->operand[instruction->dst + channel] = (uint8_t)(reg + channel);
			gsi1_spill_pin(lowering, instruction->dst + channel);
		}
		*dst = reg;
		return 0;
	}

	/* A value with a fixed operand or a constant has no register. */
	if (lowering->kind[instruction->dst] != GSI1_VALUE_REGISTER)
		return 0;

	/* A spilled loop variable defined again is written in a scratch register and stored after. */
	spilled = gsi1_spill_is_spilled(lowering, instruction->dst);
	if (spilled != 0) {
		error = gsi1_spill_destination(lowering, instruction->dst, &reg);
		if (error != 0)
			return error;
		*dst = reg;
		return 0;
	}

	/* A value seen before (a loop variable) keeps its register; a new one takes the lowest free. */
	if (lowering->operand[instruction->dst] == GSI1_NO_OPERAND) {
		error = gsi1_take_register(lowering, &reg);
		if (error != 0)
			return error;
		lowering->operand[instruction->dst] = (uint8_t)reg;
	}

	/* The register stays until the instruction is done. */
	gsi1_spill_pin(lowering, instruction->dst);

	/* Succeeded: the register the value lives in. */
	*dst = lowering->operand[instruction->dst];
	return 0;
}

/* Lowers one IR instruction into dst (GSI1_NO_OPERAND when it defines none). */
static int
lower(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	int error;

	/* The operation chooses the lowering. */
	switch (instruction->op) {
	case DRV_GPU_IR_NOP:
	case DRV_GPU_IR_CONST:
	case DRV_GPU_IR_ICONST:
	case DRV_GPU_IR_BOOL:
		/* A constant only takes an instruction when a MOVE writes the same value later. */
		error = 0;
		if (dst != GSI1_NO_OPERAND)
			gsi1_emit_constant(lowering, dst, instruction->immediate);
		break;
	case DRV_GPU_IR_FADD:
	case DRV_GPU_IR_FSUB:
	case DRV_GPU_IR_FMUL:
	case DRV_GPU_IR_FMIN:
	case DRV_GPU_IR_FMAX:
	case DRV_GPU_IR_PACK_HALF:
		error = lower_float(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_FNEG:
	case DRV_GPU_IR_FABS:
	case DRV_GPU_IR_RSQ:
	case DRV_GPU_IR_RCP:
	case DRV_GPU_IR_SQRT:
	case DRV_GPU_IR_EXP2:
	case DRV_GPU_IR_LOG2:
	case DRV_GPU_IR_SIN:
	case DRV_GPU_IR_COS:
	case DRV_GPU_IR_FLOOR:
	case DRV_GPU_IR_FTRUNC:
	case DRV_GPU_IR_FROUND_EVEN:
	case DRV_GPU_IR_FRACT:
	case DRV_GPU_IR_UNPACK_HALF:
		error = lower_float_unary(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_I2F:
	case DRV_GPU_IR_U2F:
	case DRV_GPU_IR_F2I:
	case DRV_GPU_IR_F2U:
		error = lower_conversion(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_AND:
	case DRV_GPU_IR_OR:
	case DRV_GPU_IR_NOT:
	case DRV_GPU_IR_IADD:
	case DRV_GPU_IR_ISUB:
	case DRV_GPU_IR_IMUL:
	case DRV_GPU_IR_INEG:
	case DRV_GPU_IR_IAND:
	case DRV_GPU_IR_IOR:
	case DRV_GPU_IR_IXOR:
	case DRV_GPU_IR_INOT:
	case DRV_GPU_IR_SHL:
	case DRV_GPU_IR_SHR:
	case DRV_GPU_IR_ASR:
	case DRV_GPU_IR_UDIV:
	case DRV_GPU_IR_UMOD:
	case DRV_GPU_IR_IDIV:
	case DRV_GPU_IR_IREM:
		error = lower_integer(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_FLT:
	case DRV_GPU_IR_FGE:
	case DRV_GPU_IR_FEQ:
	case DRV_GPU_IR_FNEU:
	case DRV_GPU_IR_ILT:
	case DRV_GPU_IR_IGE:
	case DRV_GPU_IR_ULT:
	case DRV_GPU_IR_UGE:
	case DRV_GPU_IR_IEQ:
	case DRV_GPU_IR_INE:
		error = lower_compare(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_SELECT:
		error = lower_select(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_MOVE:
		error = lower_move(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_LOOP_BEGIN:
	case DRV_GPU_IR_LOOP_END:
		error = lower_loop(lowering, instruction);
		break;
	case DRV_GPU_IR_SKIP_BEGIN:
	case DRV_GPU_IR_SKIP_END:
		error = lower_skip(lowering, instruction);
		break;
	case DRV_GPU_IR_KILL:
		error = lower_kill(lowering, instruction);
		break;
	case DRV_GPU_IR_DDX:
	case DRV_GPU_IR_DDX_FINE:
	case DRV_GPU_IR_DDY:
	case DRV_GPU_IR_DDY_FINE:
		error = lower_derivative(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_LOAD_INPUT:
		error = gsi1_io_lower_input(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_STORE_OUTPUT:
		error = gsi1_io_lower_output(lowering, instruction);
		break;
	case DRV_GPU_IR_SAMPLE:
	case DRV_GPU_IR_SAMPLE_BIAS:
	case DRV_GPU_IR_SAMPLE_LOD:
	case DRV_GPU_IR_TEXTURE:
		error = gsi1_texture_lower(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_LOAD_SYSTEM:
		/* A built-in GSI1 has an operand for needs no instruction. */
		error = 0;
		if (dst != GSI1_NO_OPERAND)
			error = gsi1_refuse(lowering, ENOTSUP, "a built-in value GSI1 does not have");
		break;
	default:
		error = gsi1_memory_lower(lowering, instruction, dst);
		break;
	}

	/* Reports why the instruction could not be lowered. */
	if (error != 0)
		return error;

	/* Succeeded: the instruction is lowered. */
	return 0;
}

/* Lowers the two-source float operations: add, subtract, multiply, minimum, maximum and the half pack. */
static int
lower_float(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct gsi1_extra extra;
	uint32_t a;
	uint32_t b;
	int error;

	/* The two sources. */
	error = gsi1_source(lowering, instruction->src[0], &a);
	if (error != 0)
		return error;

	/* Finds the second source. */
	error = gsi1_source(lowering, instruction->src[1], &b);
	if (error != 0)
		return error;

	/* The instruction of the operation. */
	switch (instruction->op) {
	case DRV_GPU_IR_FADD:
		gsi1_emit_float(lowering, GSI1_F7_FADD, GSI1_RM_RNE, dst, a, b, 0U);
		break;
	case DRV_GPU_IR_FSUB:
		gsi1_emit_float(lowering, GSI1_F7_FSUB, GSI1_RM_RNE, dst, a, b, 0U);
		break;
	case DRV_GPU_IR_FMUL:
		gsi1_emit_float(lowering, GSI1_F7_FMUL, GSI1_RM_RNE, dst, a, b, 0U);
		break;
	case DRV_GPU_IR_FMIN:
		gsi1_emit_float(lowering, GSI1_F7_FMINMAX, 0U, dst, a, b, 0U);
		break;
	case DRV_GPU_IR_FMAX:
		gsi1_emit_float(lowering, GSI1_F7_FMINMAX, 1U, dst, a, b, 0U);
		break;
	default:
		/* packh: the first source to the low half, the second to the high half, RNE. */
		kern_memset(&extra, 0, sizeof(extra));
		gsi1_emit(lowering, gsi1_encode_r(GSI1_OP_CUSTOM2, GSI1_F3_PACK, GSI1_F7_PACKH, dst, a, b, &extra));
		break;
	}

	/* Succeeded: the result is in dst. */
	return 0;
}

/*
 * Lowers the one-source float operations: the sign changes (fsgnj with
 * both sources modified), the transcendental unit's, the square root, the
 * roundings, fract and the half unpack.
 */
static int
lower_float_unary(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct gsi1_extra extra;
	uint32_t a;
	int error;

	/* The source. */
	error = gsi1_source(lowering, instruction->src[0], &a);
	if (error != 0)
		return error;

	/* The instruction of the operation. */
	switch (instruction->op) {
	case DRV_GPU_IR_FNEG:
		/* The magnitude of -a with the sign of -a. */
		gsi1_emit_float(lowering, GSI1_F7_FSGNJ, 0U, dst, a, a, GSI1_W1_N1 | GSI1_W1_N2);
		break;
	case DRV_GPU_IR_FABS:
		/* The magnitude of |a| with the sign of |a|. */
		gsi1_emit_float(lowering, GSI1_F7_FSGNJ, 0U, dst, a, a, GSI1_W1_A1 | GSI1_W1_A2);
		break;
	case DRV_GPU_IR_RSQ:
		gsi1_emit_transcendental(lowering, GSI1_XU_RSQ, dst, a);
		break;
	case DRV_GPU_IR_RCP:
		gsi1_emit_transcendental(lowering, GSI1_XU_RCP, dst, a);
		break;
	case DRV_GPU_IR_EXP2:
		gsi1_emit_transcendental(lowering, GSI1_XU_EXP2, dst, a);
		break;
	case DRV_GPU_IR_LOG2:
		gsi1_emit_transcendental(lowering, GSI1_XU_LOG2, dst, a);
		break;
	case DRV_GPU_IR_SIN:
		gsi1_emit_transcendental(lowering, GSI1_XU_SIN, dst, a);
		break;
	case DRV_GPU_IR_COS:
		gsi1_emit_transcendental(lowering, GSI1_XU_COS, dst, a);
		break;
	case DRV_GPU_IR_FRACT:
		gsi1_emit_transcendental(lowering, GSI1_XU_FRACT, dst, a);
		break;
	case DRV_GPU_IR_SQRT:
		gsi1_emit_float(lowering, GSI1_F7_FSQRT, GSI1_RM_RNE, dst, a, 0U, 0U);
		break;
	case DRV_GPU_IR_FLOOR:
		gsi1_emit_float(lowering, GSI1_F7_FROUND, GSI1_RM_RDN, dst, a, GSI1_RS2_FROUND, 0U);
		break;
	case DRV_GPU_IR_FTRUNC:
		gsi1_emit_float(lowering, GSI1_F7_FROUND, GSI1_RM_RTZ, dst, a, GSI1_RS2_FROUND, 0U);
		break;
	case DRV_GPU_IR_FROUND_EVEN:
		gsi1_emit_float(lowering, GSI1_F7_FROUND, GSI1_RM_RNE, dst, a, GSI1_RS2_FROUND, 0U);
		break;
	default:
		/* unpackh: the half the component names, converted exactly. */
		kern_memset(&extra, 0, sizeof(extra));
		extra.sub = instruction->component & 1U;
		gsi1_emit(lowering, gsi1_encode_r(GSI1_OP_CUSTOM2, GSI1_F3_PACK, GSI1_F7_UNPACKH, dst, a, 0U, &extra));
		break;
	}

	/* Succeeded: the result is in dst. */
	return 0;
}

/*
 * Lowers the conversions between integers and floats: to a float with
 * RNE, to an integer rounded toward zero (fcvt saturates).
 */
static int
lower_conversion(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	uint32_t a;
	int error;

	/* The source. */
	error = gsi1_source(lowering, instruction->src[0], &a);
	if (error != 0)
		return error;

	/* The conversion: its funct7, the signedness in rs2, and its rounding. */
	switch (instruction->op) {
	case DRV_GPU_IR_I2F:
		gsi1_emit_float(lowering, GSI1_F7_FCVT_S_W, GSI1_RM_RNE, dst, a, 0U, 0U);
		break;
	case DRV_GPU_IR_U2F:
		gsi1_emit_float(lowering, GSI1_F7_FCVT_S_W, GSI1_RM_RNE, dst, a, 1U, 0U);
		break;
	case DRV_GPU_IR_F2I:
		gsi1_emit_float(lowering, GSI1_F7_FCVT_W_S, GSI1_RM_RTZ, dst, a, 0U, 0U);
		break;
	default:
		gsi1_emit_float(lowering, GSI1_F7_FCVT_W_S, GSI1_RM_RTZ, dst, a, 1U, 0U);
		break;
	}

	/* Succeeded: the result is in dst. */
	return 0;
}

/*
 * Lowers the integer and Boolean operations.  A second source that is a
 * small constant goes into the instruction's immediate.
 */
static int
lower_integer(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	uint32_t a;
	uint32_t b;
	int done;
	int error;

	/* An operation on a small constant takes the immediate form. */
	error = lower_immediate(lowering, instruction, dst, &done);
	if (error != 0)
		return error;
	if (done != 0)
		return 0;

	/* The first source. */
	error = gsi1_source(lowering, instruction->src[0], &a);
	if (error != 0)
		return error;

	/* The one-source operations. */
	switch (instruction->op) {
	case DRV_GPU_IR_NOT:
	case DRV_GPU_IR_INOT:
		gsi1_emit_alu_immediate(lowering, GSI1_F3_XOR, dst, a, -1);
		return 0;
	case DRV_GPU_IR_INEG:
		gsi1_emit_alu(lowering, GSI1_F3_ADD, GSI1_F7_ALT, dst, GSI1_ZERO, a);
		return 0;
	default:
		break;
	}

	/* The second source. */
	error = gsi1_source(lowering, instruction->src[1], &b);
	if (error != 0)
		return error;

	/* The two-source operations. */
	switch (instruction->op) {
	case DRV_GPU_IR_AND:
	case DRV_GPU_IR_IAND:
		gsi1_emit_alu(lowering, GSI1_F3_AND, GSI1_F7_BASE, dst, a, b);
		break;
	case DRV_GPU_IR_OR:
	case DRV_GPU_IR_IOR:
		gsi1_emit_alu(lowering, GSI1_F3_OR, GSI1_F7_BASE, dst, a, b);
		break;
	case DRV_GPU_IR_IXOR:
		gsi1_emit_alu(lowering, GSI1_F3_XOR, GSI1_F7_BASE, dst, a, b);
		break;
	case DRV_GPU_IR_IADD:
		gsi1_emit_alu(lowering, GSI1_F3_ADD, GSI1_F7_BASE, dst, a, b);
		break;
	case DRV_GPU_IR_ISUB:
		gsi1_emit_alu(lowering, GSI1_F3_ADD, GSI1_F7_ALT, dst, a, b);
		break;
	case DRV_GPU_IR_IMUL:
		gsi1_emit_alu(lowering, GSI1_F3_MUL, GSI1_F7_MULDIV, dst, a, b);
		break;
	case DRV_GPU_IR_SHL:
		gsi1_emit_alu(lowering, GSI1_F3_SLL, GSI1_F7_BASE, dst, a, b);
		break;
	case DRV_GPU_IR_SHR:
		gsi1_emit_alu(lowering, GSI1_F3_SRL, GSI1_F7_BASE, dst, a, b);
		break;
	case DRV_GPU_IR_ASR:
		gsi1_emit_alu(lowering, GSI1_F3_SRL, GSI1_F7_ALT, dst, a, b);
		break;
	case DRV_GPU_IR_UDIV:
		gsi1_emit_alu(lowering, GSI1_F3_DIVU, GSI1_F7_MULDIV, dst, a, b);
		break;
	case DRV_GPU_IR_UMOD:
		gsi1_emit_alu(lowering, GSI1_F3_REMU, GSI1_F7_MULDIV, dst, a, b);
		break;
	case DRV_GPU_IR_IDIV:
		gsi1_emit_alu(lowering, GSI1_F3_DIV, GSI1_F7_MULDIV, dst, a, b);
		break;
	default:
		gsi1_emit_alu(lowering, GSI1_F3_REM, GSI1_F7_MULDIV, dst, a, b);
		break;
	}

	/* Succeeded: the result is in dst. */
	return 0;
}

/*
 * Lowers an integer operation whose second source (or the first, for one
 * that commutes) is a constant that fits the immediate: addi, andi, ori,
 * xori, the shifts by a constant, and a product by a power of two as a
 * shift.  *done says whether it did.
 */
static int
lower_immediate(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst,
	int *done)
{
	uint32_t constant;
	uint32_t other;
	uint32_t shift;
	uint32_t a;
	int64_t immediate;
	int known;
	int commutes;
	int fits;
	int error;

	/* Nothing is lowered yet. */
	*done = 0;

	/* Only the two-source operations with an immediate form are looked at. */
	commutes = 0;
	switch (instruction->op) {
	case DRV_GPU_IR_IADD:
	case DRV_GPU_IR_IAND:
	case DRV_GPU_IR_IOR:
	case DRV_GPU_IR_IXOR:
	case DRV_GPU_IR_AND:
	case DRV_GPU_IR_OR:
		commutes = 1;
		break;
	case DRV_GPU_IR_IMUL:
		commutes = 1;
		break;
	case DRV_GPU_IR_ISUB:
	case DRV_GPU_IR_SHL:
	case DRV_GPU_IR_SHR:
	case DRV_GPU_IR_ASR:
		break;
	default:
		return 0;
	}

	/* The constant source: the second, or the first of an operation that commutes. */
	other = instruction->src[0];
	known = gsi1_value_constant(lowering, instruction->src[1], &constant);
	if (known == 0 && commutes != 0) {
		known = gsi1_value_constant(lowering, instruction->src[0], &constant);
		other = instruction->src[1];
	}
	if (known == 0)
		return 0;

	/* A product takes the immediate form only as a shift by a power of two. */
	shift = 0U;
	if (instruction->op == DRV_GPU_IR_IMUL) {
		if (constant == 0U || (constant & (constant - 1U)) != 0U)
			return 0;
		while ((1U << shift) != constant)
			shift++;
	}

	/* The immediate: the constant, negated for a subtraction. */
	immediate = (int64_t)(int32_t)constant;
	if (instruction->op == DRV_GPU_IR_ISUB)
		immediate = -immediate;
	fits = gsi1_immediate_fits(immediate);
	if (fits == 0)
		return 0;

	/* The register source. */
	error = gsi1_source(lowering, other, &a);
	if (error != 0)
		return error;

	/* The immediate instruction of the operation. */
	switch (instruction->op) {
	case DRV_GPU_IR_IADD:
	case DRV_GPU_IR_ISUB:
		gsi1_emit_alu_immediate(lowering, GSI1_F3_ADD, dst, a, (int32_t)immediate);
		break;
	case DRV_GPU_IR_IAND:
	case DRV_GPU_IR_AND:
		gsi1_emit_alu_immediate(lowering, GSI1_F3_AND, dst, a, (int32_t)immediate);
		break;
	case DRV_GPU_IR_IOR:
	case DRV_GPU_IR_OR:
		gsi1_emit_alu_immediate(lowering, GSI1_F3_OR, dst, a, (int32_t)immediate);
		break;
	case DRV_GPU_IR_IXOR:
		gsi1_emit_alu_immediate(lowering, GSI1_F3_XOR, dst, a, (int32_t)immediate);
		break;
	case DRV_GPU_IR_SHL:
		gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SLL, GSI1_F7_BASE, dst, a, constant));
		break;
	case DRV_GPU_IR_IMUL:
		gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SLL, GSI1_F7_BASE, dst, a, shift));
		break;
	case DRV_GPU_IR_SHR:
		gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SRL, GSI1_F7_BASE, dst, a, constant));
		break;
	default:
		gsi1_emit(lowering, gsi1_encode_shift(GSI1_F3_SRL, GSI1_F7_ALT, dst, a, constant));
		break;
	}

	/* Succeeded: the operation took its immediate form. */
	*done = 1;
	return 0;
}

/*
 * Lowers a comparison to a Boolean.  The float ones and the signed and
 * unsigned less-than are one instruction (>= of floats swaps the sources
 * of fle); >= of integers is less-than inverted, and (in)equality tests
 * the exclusive or against zero.
 */
static int
lower_compare(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	uint32_t a;
	uint32_t b;
	int error;

	/* The two sources. */
	error = gsi1_source(lowering, instruction->src[0], &a);
	if (error != 0)
		return error;

	/* Finds the second source. */
	error = gsi1_source(lowering, instruction->src[1], &b);
	if (error != 0)
		return error;

	/* The comparison's instructions. */
	switch (instruction->op) {
	case DRV_GPU_IR_FLT:
		gsi1_emit_float(lowering, GSI1_F7_FCMP, GSI1_F3_FLT, dst, a, b, 0U);
		break;
	case DRV_GPU_IR_FGE:
		gsi1_emit_float(lowering, GSI1_F7_FCMP, GSI1_F3_FLE, dst, b, a, 0U);
		break;
	case DRV_GPU_IR_FEQ:
		gsi1_emit_float(lowering, GSI1_F7_FCMP, GSI1_F3_FEQ, dst, a, b, 0U);
		break;
	case DRV_GPU_IR_FNEU:
		gsi1_emit_float(lowering, GSI1_F7_FCMP, GSI1_F3_FNE, dst, a, b, 0U);
		break;
	case DRV_GPU_IR_ILT:
		gsi1_emit_alu(lowering, GSI1_F3_SLT, GSI1_F7_BASE, dst, a, b);
		break;
	case DRV_GPU_IR_ULT:
		gsi1_emit_alu(lowering, GSI1_F3_SLTU, GSI1_F7_BASE, dst, a, b);
		break;
	case DRV_GPU_IR_IGE:
		gsi1_emit_alu(lowering, GSI1_F3_SLT, GSI1_F7_BASE, dst, a, b);
		gsi1_emit_alu_immediate(lowering, GSI1_F3_XOR, dst, dst, -1);
		break;
	case DRV_GPU_IR_UGE:
		gsi1_emit_alu(lowering, GSI1_F3_SLTU, GSI1_F7_BASE, dst, a, b);
		gsi1_emit_alu_immediate(lowering, GSI1_F3_XOR, dst, dst, -1);
		break;
	case DRV_GPU_IR_IEQ:
		/* Equal when the exclusive or is below 1, unsigned. */
		gsi1_emit_alu(lowering, GSI1_F3_XOR, GSI1_F7_BASE, dst, a, b);
		gsi1_emit_alu_immediate(lowering, GSI1_F3_SLTU, dst, dst, 1);
		break;
	default:
		/* Not equal when zero is below the exclusive or, unsigned. */
		gsi1_emit_alu(lowering, GSI1_F3_XOR, GSI1_F7_BASE, dst, a, b);
		gsi1_emit_alu(lowering, GSI1_F3_SLTU, GSI1_F7_BASE, dst, GSI1_ZERO, dst);
		break;
	}

	/* Succeeded: the Boolean is in dst. */
	return 0;
}

/* Lowers SELECT to sel, which chooses bit by bit as the IR's selection does. */
static int
lower_select(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct gsi1_extra extra;
	uint32_t condition;
	uint32_t taken;
	uint32_t other;
	int error;

	/* The Boolean and the two values. */
	error = gsi1_source(lowering, instruction->src[0], &condition);
	if (error != 0)
		return error;

	/* Finds the value taken where the Boolean holds. */
	error = gsi1_source(lowering, instruction->src[1], &taken);
	if (error != 0)
		return error;

	/* Finds the value taken elsewhere. */
	error = gsi1_source(lowering, instruction->src[2], &other);
	if (error != 0)
		return error;

	/* sel rd, condition, taken, other. */
	kern_memset(&extra, 0, sizeof(extra));
	gsi1_emit(lowering, gsi1_encode_r4(GSI1_OP_CUSTOM3, GSI1_F3_SEL, dst, condition, taken, other, &extra));

	/* Succeeded: the selection is in dst. */
	return 0;
}

/* Lowers MOVE: a copy into the loop variable's register (a constant set straight into it). */
static int
lower_move(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	uint32_t source;
	int error;

	/* A constant goes straight into the register. */
	if (lowering->kind[instruction->src[0]] == GSI1_VALUE_CONSTANT) {
		gsi1_emit_constant(lowering, dst, lowering->constant[instruction->src[0]]);
		return 0;
	}

	/* Finds where the value is read from. */
	error = gsi1_source(lowering, instruction->src[0], &source);
	if (error != 0)
		return error;

	/* Copies it. */
	gsi1_emit_move(lowering, dst, source);

	/* Succeeded: the loop variable holds the value. */
	return 0;
}

/*
 * Lowers a loop's start and end.
 *
 * The start saves EXEC (exec.save writes every lane) and places the label
 * the end branches back to.  The end drops the lanes whose condition is
 * false, branches back while any lane is left, and restores EXEC.
 */
static int
lower_loop(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	uint32_t save;
	uint32_t label;
	uint32_t condition;
	int error;

	/* The start of a loop. */
	if (instruction->op == DRV_GPU_IR_LOOP_BEGIN) {
		if (lowering->loop_depth == GSI1_LOOP_DEPTH) {
			error = gsi1_refuse(lowering, ENOTSUP, "loops nested deeper than the compiler follows");
			return error;
		}

		/* Takes the register EXEC is kept in for the loop. */
		error = gsi1_take_register(lowering, &save);
		if (error != 0)
			return error;

		/* Makes the label the end branches back to. */
		label = gsi1_new_label(lowering);
		if (label == GSI1_NOT_DEFINED)
			return ENOMEM;

		/* Saves EXEC, then places the label at the first instruction of the body. */
		gsi1_emit_exec(lowering, GSI1_EXEC_SAVE, save);
		gsi1_place_label(lowering, label);
		lowering->loop_save[lowering->loop_depth] = save;
		lowering->loop_label[lowering->loop_depth] = label;
		lowering->loop_start[lowering->loop_depth] = lowering->index;
		lowering->loop_depth++;
		return 0;
	}

	/* An end without a start is malformed. */
	if (lowering->loop_depth == 0U) {
		error = gsi1_refuse(lowering, EINVAL, "a loop's end without its beginning");
		return error;
	}

	/* Finds the loop's condition. */
	error = gsi1_source(lowering, instruction->src[0], &condition);
	if (error != 0)
		return error;

	/* Drops the lanes that leave, branches back while any is left, and restores EXEC. */
	lowering->loop_depth--;
	gsi1_emit_exec(lowering, GSI1_EXEC_AND, condition);
	gsi1_emit_branch(lowering, GSI1_F3_BEXNZ, 0U, lowering->loop_label[lowering->loop_depth]);
	gsi1_emit_exec(lowering, GSI1_EXEC_SET, lowering->loop_save[lowering->loop_depth]);

	/* The saved EXEC's register goes back. */
	gsi1_give_register(lowering, lowering->loop_save[lowering->loop_depth]);

	/* Succeeded: the loop is closed. */
	return 0;
}

/*
 * Lowers a skippable block's start and end: the block runs for every lane
 * (no branch), which the IR allows.  The parser's promise that what the
 * block makes is only seen through a selection by its predicate does not
 * hold for the selection itself when it is inside the block, so jumping
 * over a block needs the region checked first; GSI1 does not jump.
 */
static int
lower_skip(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	int error;

	/* The end of a block closes the innermost one. */
	if (instruction->op == DRV_GPU_IR_SKIP_END) {
		if (lowering->skip_depth == 0U) {
			error = gsi1_refuse(lowering, EINVAL, "a skippable block's end without its start");
			return error;
		}

		/* The block is closed. */
		lowering->skip_depth--;
		return 0;
	}

	/* The start of a block has a bound on its nesting. */
	if (lowering->skip_depth == GSI1_SKIP_DEPTH) {
		error = gsi1_refuse(lowering, ENOTSUP, "skippable blocks nested deeper than the compiler follows");
		return error;
	}

	/* The block is open. */
	lowering->skip_depth++;

	/* Succeeded: the block runs as it stands. */
	return 0;
}

/* Lowers a discard: kill sets the KILLED bits of the lanes whose Boolean holds. */
static int
lower_kill(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	struct gsi1_extra extra;
	uint32_t condition;
	int error;

	/* A discard is a fragment program's. */
	if (lowering->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		error = gsi1_refuse(lowering, EINVAL, "a discard outside a fragment shader");
		return error;
	}

	/* Finds the lanes to discard. */
	error = gsi1_source(lowering, instruction->src[0], &condition);
	if (error != 0)
		return error;

	/* kill rs1, and the program says that it discards. */
	kern_memset(&extra, 0, sizeof(extra));
	gsi1_emit(lowering, gsi1_encode_r(GSI1_OP_CUSTOM3, GSI1_F3_KILL, 0U, 0U, condition, 0U, &extra));
	lowering->flags |= GHCI_GSI1_FLAG_DISCARD;

	/* Succeeded: the discard is lowered. */
	return 0;
}

/*
 * Lowers a difference across the quad.  The fine ones are qdx and qdy (a
 * row's own and a column's own); the coarse ones take the quad's first
 * lane's difference for all four (qbcast).
 */
static int
lower_derivative(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	struct gsi1_extra extra;
	uint32_t a;
	uint32_t sub;
	int error;

	/* A difference across the quad is a fragment program's. */
	if (lowering->ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		error = gsi1_refuse(lowering, EINVAL, "a derivative outside a fragment shader");
		return error;
	}

	/* The source. */
	error = gsi1_source(lowering, instruction->src[0], &a);
	if (error != 0)
		return error;

	/* The direction: across the row or down the column. */
	sub = GSI1_QD_DX;
	if (instruction->op == DRV_GPU_IR_DDY || instruction->op == DRV_GPU_IR_DDY_FINE)
		sub = GSI1_QD_DY;

	/* The lane's own difference. */
	kern_memset(&extra, 0, sizeof(extra));
	extra.sub = sub;
	gsi1_emit(lowering, gsi1_encode_r(GSI1_OP_CUSTOM1, GSI1_F3_QD, 0U, dst, a, 0U, &extra));

	/* A coarse difference takes the quad's first lane's. */
	if (instruction->op == DRV_GPU_IR_DDX || instruction->op == DRV_GPU_IR_DDY) {
		extra.sub = GSI1_QD_BCAST;
		gsi1_emit(lowering, gsi1_encode_r(GSI1_OP_CUSTOM1, GSI1_F3_QD, 0U, dst, dst, 0U, &extra));
	}

	/* Succeeded: the difference is in dst. */
	return 0;
}

/* Gives back the scratch registers of the instruction just lowered. */
static void
give_scratch(
	struct gsi1_lowering *lowering)
{
	uint32_t index;

	/* Each one taken. */
	for (index = 0U; index < lowering->scratch_count; index++)
		gsi1_give_register(lowering, lowering->scratch[index]);

	/* None is taken any more. */
	lowering->scratch_count = 0U;
}

/*
 * Gives back the registers of the values whose last read was the
 * instruction just lowered, and of the values it defined that nothing
 * reads.
 */
static void
retire(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	uint32_t sources[GSI1_MAX_SOURCES];
	uint32_t index;
	uint32_t value;
	uint32_t until;
	int count;
	int defines;

	/* The instruction after which a value read last now is no longer needed. */
	until = lowering->index + 1U;

	/* The sources read for the last time. */
	count = gsi1_ir_sources(instruction, sources);
	for (index = 0U; count > 0 && index < (uint32_t)count; index++) {
		value = sources[index];
		if (lowering->kind[value] != GSI1_VALUE_REGISTER)
			continue;
		if (lowering->last_use[value] != until)
			continue;
		if (lowering->operand[value] == GSI1_NO_OPERAND)
			continue;

		/* The value's register goes back, once. */
		gsi1_give_register(lowering, lowering->operand[value]);
		lowering->operand[value] = GSI1_NO_OPERAND;
	}

	/* The values defined here that nothing reads. */
	defines = gsi1_ir_defines(instruction);
	for (index = 0U; index < (uint32_t)defines; index++) {
		value = instruction->dst + index;
		if (lowering->kind[value] != GSI1_VALUE_REGISTER)
			continue;
		if (lowering->last_use[value] != 0U)
			continue;
		if (lowering->operand[value] == GSI1_NO_OPERAND)
			continue;

		/* The unread value's register goes back. */
		gsi1_give_register(lowering, lowering->operand[value]);
		lowering->operand[value] = GSI1_NO_OPERAND;
	}
}

/* Gives back the registers of the inputs that no instruction after `upto` reads. */
static void
retire_inputs(
	struct gsi1_lowering *lowering,
	uint32_t upto)
{
	uint32_t input;

	/* Each input that has a register. */
	for (input = 0U; input < GSI1_INPUT_COUNT; input++) {
		if (lowering->input_reg[input] == GSI1_NO_OPERAND)
			continue;
		if (lowering->input_until[input] > upto)
			continue;

		/* The input's register goes back. */
		gsi1_give_register(lowering, lowering->input_reg[input]);
		lowering->input_reg[input] = GSI1_NO_OPERAND;
	}
}

/*
 * Ends the program: the exports of a graphics program, then end (which
 * waits for the warp's stores).
 */
static int
finish(
	struct gsi1_lowering *lowering)
{
	struct gsi1_extra extra;
	int error;

	/* Every loop and skippable block is closed. */
	lowering->index = lowering->ir->instruction_count;
	if (lowering->loop_depth != 0U) {
		error = gsi1_refuse(lowering, EINVAL, "a loop without its end");
		return error;
	}

	/* A block opened without its end is malformed. */
	if (lowering->skip_depth != 0U) {
		error = gsi1_refuse(lowering, EINVAL, "a skippable block without its end");
		return error;
	}

	/* The values the last instruction read may be spilled again. */
	gsi1_spill_next(lowering);

	/* The outputs. */
	error = gsi1_io_finish(lowering);
	if (error != 0)
		return error;

	/* end. */
	kern_memset(&extra, 0, sizeof(extra));
	gsi1_emit(lowering, gsi1_encode_r(GSI1_OP_CUSTOM0, GSI1_F3_END, 0U, 0U, 0U, 0U, &extra));
	give_scratch(lowering);

	/* A list that could not grow fails the compile. */
	if (lowering->out_of_memory != 0)
		return ENOMEM;

	/* Succeeded: the program ends. */
	return 0;
}

/* Fills in each branch's offset from its label's place. */
static int
resolve_branches(
	struct gsi1_lowering *lowering)
{
	uint64_t word;
	uint32_t index;
	uint32_t label;
	uint32_t funct3;
	uint32_t rs1;
	int32_t offset;
	int fits;
	int error;

	/* Each instruction that branches. */
	for (index = 0U; index < lowering->count; index++) {
		if (lowering->branch_label[index] == 0U)
			continue;

		/* The label must have been placed. */
		label = lowering->branch_label[index] - 1U;
		if (label >= lowering->label_count || lowering->label_at[label] == GSI1_NOT_DEFINED) {
			error = gsi1_refuse(lowering, EINVAL, "a branch to a label that was never placed");
			return error;
		}

		/* The offset counts instructions from the branch. */
		offset = (int32_t)lowering->label_at[label] - (int32_t)index;
		fits = gsi1_immediate_fits((int64_t)offset);
		if (fits == 0) {
			error = gsi1_refuse(lowering, ENOTSUP, "a branch reaches farther than its offset");
			return error;
		}

		/* Encodes the branch again with its offset, keeping its condition and Boolean. */
		word = lowering->code[index];
		funct3 = (uint32_t)(word >> GSI1_W0_FUNCT3_SHIFT) & 7U;
		rs1 = (uint32_t)(word >> GSI1_W0_RS1_SHIFT) & 0x1fU;
		rs1 |= (((uint32_t)(word >> 32) >> GSI1_W1_RS1_SHIFT) & 3U) << 5;
		lowering->code[index] = gsi1_encode_b(funct3, rs1, offset);
	}

	/* Succeeded: every branch has its offset. */
	return 0;
}

/*
 * Fills in the program record: the code, two words to an instruction,
 * and what the pipeline state and the executor take from the compile.
 */
static int
build_program(
	struct gsi1_lowering *lowering,
	struct ghci_gsi1_program *program)
{
	const struct drv_gpu_shader_ir *ir;
	uint32_t *words;
	uint32_t total;
	uint32_t index;
	uint32_t registers;

	/* Names the module being written. */
	ir = lowering->ir;

	/* Allocates the code: two words per instruction. */
	total = lowering->count * GSI1_WORDS_PER_INSTRUCTION;
	words = kern_calloc(total, sizeof(*words));
	if (words == NULL)
		return ENOMEM;

	/*
	 * The registers: the highest the program used, at least one, and at
	 * least those the hardware writes before the warp starts (the
	 * attributes, and a fragment warp's inputs).
	 */
	registers = lowering->register_highest;
	if (registers == 0U)
		registers = 1U;
	if (ir->stage == DRV_GPU_STAGE_VERTEX && registers < lowering->attribute_registers)
		registers = lowering->attribute_registers;
	if (ir->stage == DRV_GPU_STAGE_FRAGMENT && registers < GSI1_IN_SAMPLE_MASK + 1U)
		registers = GSI1_IN_SAMPLE_MASK + 1U;
	if ((lowering->flags & GHCI_GSI1_FLAG_CENTROID) != 0U && registers < GSI1_IN_COUNT)
		registers = GSI1_IN_COUNT;

	/* The code: word0 then word1 of each instruction. */
	for (index = 0U; index < lowering->count; index++) {
		words[2U * index] = (uint32_t)lowering->code[index];
		words[2U * index + 1U] = (uint32_t)(lowering->code[index] >> 32);
	}
	program->words = words;
	program->word_count = total;
	program->instruction_count = lowering->count;

	/* The pipeline state's numbers. */
	program->used_regs = registers;
	program->varying_count = lowering->varying_count;
	program->attribute_registers = lowering->attribute_registers;
	program->flags = lowering->flags;
	program->color_outputs = lowering->color_outputs;

	/* A compute program's shared memory and workgroup. */
	if (ir->stage == DRV_GPU_STAGE_COMPUTE) {
		program->lds_bytes = ir->shared_bytes;
		program->local_size[0] = ir->local_size[0];
		program->local_size[1] = ir->local_size[1];
		program->local_size[2] = ir->local_size[2];
	}

	/* The constant bank's words, the scratch memory and the pipeline's tables for the executor. */
	program->push_bytes = ir->push_bytes;
	program->dynamic_used = lowering->dynamic_used;
	program->scratch_bytes = lowering->spill.bytes;
	program->bindings = lowering->bindings;

	/* Succeeded: the record is the program's. */
	return 0;
}

/* Frees the tables of a compilation. */
static void
free_tables(
	struct gsi1_lowering *lowering)
{
	/* The per-value tables. */
	kern_free(lowering->kind);
	kern_free(lowering->operand);
	kern_free(lowering->constant);
	kern_free(lowering->last_use);
	kern_free(lowering->moved);

	/* The per-instruction and per-uniform tables. */
	kern_free(lowering->dead);
	kern_free(lowering->binding);

	/* The code and the labels. */
	kern_free(lowering->code);
	kern_free(lowering->branch_label);
	kern_free(lowering->label_at);

	/* The spill tables. */
	gsi1_spill_free(lowering);
}
