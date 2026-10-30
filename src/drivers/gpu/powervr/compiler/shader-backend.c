/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR shader compiler's back end: from the intermediate form to
 * machine operations on registers, and from those to the shading
 * cluster's instruction groups.
 *
 * Each value gets temporaries for the span of the program it is live in
 * (a value used across a loop's iterations for the whole loop).  Each
 * intermediate instruction becomes a few machine operations: constants
 * and modifiers the hardware cannot take where they are used go through
 * scratch temporaries first.  Structured control flow becomes the
 * execution mask counter's steps.  Each machine operation is one
 * instruction group, laid out as the Mesa compiler lays out the same
 * operation.  The same code runs on the build host.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/compiler/shader.h"
#include "drivers/gpu/powervr/compiler/shader-ir.h"
#include "drivers/gpu/powervr/compiler/shader-private.h"
#include "drivers/gpu/powervr/compiler/usc-encode.h"

/*
 * The temporaries the back end keeps for itself: the execution mask
 * counter, scratch words, the coordinate pair of a sample, the address
 * pair of a memory access and the store's data right after it (the
 * hardware's st32 takes the data from the register after the address
 * pair, as Mesa lays them out in one vector).  Values start
 * after them.
 */
#define TEMP_EMC		0U
#define TEMP_SCRATCH		1U
#define TEMP_SCRATCH_COUNT	3U
#define TEMP_COORDINATES	4U
#define TEMP_ADDRESS		6U
#define TEMP_STORE_DATA		8U
#define TEMP_FIRST_VALUE	9U

/*
 * The special registers the back end names: constant zero, one, the
 * float one and all ones, and the instance number.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_internal.h (pco_zero 0, pco_one
 * 1, pco_7 7, pco_fone 64, pco_true 143, all of the constant class) and
 * src/imagination/pco/pco_isa.py (SR inst_num 51).
 */
#define SPECIAL_ZERO		0U
#define SPECIAL_ONE		1U
#define SPECIAL_SEVEN		7U
#define SPECIAL_FLOAT_ONE	64U
#define SPECIAL_TRUE		143U
#define SPECIAL_INSTANCE	51U
#define SPECIAL_PIXEL_OUTPUT	32U

/*
 * The fields of the operations the back end builds.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py (F_TST_OP z 0, g 5, e 4,
 * ge 6, ne 7, l 8; F_TST_TYPE f32 0, u32 5, s32 6; F_PCK_FORMAT u32
 * 0b00110, s32 0b00111, f16f16 0b01110, zero 0b11110; F_LOGICAL_OP or 0,
 * and 1, xor 2;
 * F_SHIFT2_OP lsl 0, shr 1, asr_twb 4; F_CNDINST st 0, ef 1, sm 2,
 * lt 3, end 4; F_PCND always 0, p0_true 1, p0_false 3; F_BPRED cc 0,
 * allp 1; F_DSIZE 32bit 0b10; F_MOVW01 ft0 0, ft1 1, ft2 2, fte 3;
 * F_IS0_SEL s1 0b100, s2 0b101; F_IS4_SEL ft0 0, ft1 1, ft2 2, fte 3;
 * F_IS1_SEL ft0 0, fte 1; F_DMN 2d 0b10; ctrlop ditr 0b1100; F_SCHED_CTRL
 * wdf 0b10).
 */
#define TEST_ZERO		0U
#define TEST_EQUAL		4U
#define TEST_GREATER		5U
#define TEST_GREATER_EQUAL	6U
#define TEST_NOT_EQUAL		7U
#define TEST_LESS		8U
#define TYPE_F32		0U
#define TYPE_U32		5U
#define TYPE_S32		6U
#define PACK_U8888		0U
#define PACK_F16F16		14U
#define PACK_U32		6U
#define PACK_S32		7U
#define PACK_ZERO		30U
#define PACK_SCALE		1U
#define PACK_ROUND_TO_ZERO	2U
#define LOGIC_OR		0U
#define LOGIC_AND		1U
#define LOGIC_XOR		2U
#define SHIFT_LEFT		0U
#define SHIFT_RIGHT		1U
#define SHIFT_RIGHT_SIGNED	4U
#define CONDITION_START		0U
#define CONDITION_ELSE		1U
#define CONDITION_SET		2U
#define CONDITION_LOOP_TEST	3U
#define CONDITION_END		4U
#define PREDICATE_ALWAYS	0U
#define PREDICATE_P0_FALSE	3U
#define BRANCH_ANY_CONDITION	0U
#define BRANCH_ALL_DISABLED	1U
#define SIZE_32			2U
#define SELECT_FT0		0U
#define SELECT_FT1		1U
#define SELECT_FT2		2U
#define SELECT_FTE		3U
#define MUX_S1			4U
#define MUX_S2			5U
#define CONTROL_ITERATE		12U
#define CONTROL_ITERATE_SAMPLE	8U
#define SCHEDULE_WAIT_DATA	2U
#define DIMENSIONS_2D		2U

/* The most ifs and loops open at once. */
#define NESTING_MAX		32U

/* A sum of products' source that is the pixel output, and one it leaves unused (sum_slots). */
#define SUM_DESTINATION		4U
#define SUM_UNUSED		5U

/* One open if: the branch to its else or end, and the else's branch to the end. */
struct backend_if {
	unsigned branch;
	unsigned else_branch;
	unsigned has_else;
};

/* One open loop: its prologue's branch, its body's first operation, its ifs and its interlogue. */
struct backend_loop {
	unsigned branch;
	unsigned body;
	unsigned ifs;
	unsigned interlogue;
};

/*
 * Which operations pair_operations puts two to a group (phases 0 and 1
 * of the main unit): 0 none, 1 float adds and multiplies, 2 also
 * multiply-adds and moves (on the board a group of two independent
 * multiplies took the time of one, so a group's cost is its issue).
 */
#ifndef POWERVR_SHADER_PAIR
#define POWERVR_SHADER_PAIR		2
#endif

/*
 * A test switch (0 as built): every float add, multiply and
 * multiply-add in the low precision forms (Mesa 26.2.5 pco_isa.py
 * fadd_lp, fmul_lp, lp_fma; pco uses none), to see on the board whether
 * they issue faster.  Results lose precision; for timing only.
 */
#ifndef POWERVR_SHADER_LOW_PRECISION_TRIAL
#define POWERVR_SHADER_LOW_PRECISION_TRIAL	0
#endif

/* How far ahead pair_operations looks for an operation to move up beside another. */
#define PAIR_WINDOW			12U

/* The selection's state while it walks the intermediate form. */
struct backend_select {
	const struct powervr_ir_shader *shader;
	struct powervr_shader_program *program;
	struct shader_backend *backend;
	struct backend_if ifs[NESTING_MAX];
	struct backend_loop loops[NESTING_MAX];
	unsigned if_depth;
	unsigned loop_depth;
	unsigned scratch;
	uint32_t output_base;
	unsigned literals;
};

static int liveness(const struct powervr_ir_shader *shader, struct shader_backend *backend);
static void note_reference(struct shader_backend *backend, unsigned value, unsigned at, unsigned is_definition);
static int allocate(const struct powervr_ir_shader *shader, struct shader_backend *backend);
static void compact_temporaries(struct powervr_shader_program *program, struct shader_backend *backend);
static void propagate_copies(struct powervr_ir_shader *shader);
static void simplify_products(struct powervr_ir_shader *shader);
static void hoist_addends(struct powervr_ir_shader *shader, uint32_t stage);
static void alias_shared(const struct powervr_ir_shader *shader, const struct powervr_shader_program *program, struct shader_backend *backend);
static void fuse_multiply_adds(struct powervr_ir_shader *shader, uint32_t stage);
static int fuse_multiply_add(struct powervr_ir_shader *shader, unsigned at, unsigned which);
static int fuse_clamp(struct powervr_ir_shader *shader, unsigned at);
static int fuse_saturate(struct powervr_ir_shader *shader, unsigned at);
static int clamp_bound(const struct powervr_ir_instruction *instruction, unsigned opcode, uint32_t bound, unsigned *other);
static unsigned value_references(const struct powervr_ir_shader *shader, unsigned value, unsigned *writes, unsigned *writer);
static int free_constant(uint32_t bits);
static void remove_dead_writes(struct powervr_ir_shader *shader);
static int compile_program(const uint32_t *words, size_t count, uint32_t stage, const struct powervr_shader_key *key, const uint32_t *shared_values, uint32_t value_count, unsigned split, struct powervr_shader_workspace *workspace, struct powervr_shader_program *program, uint8_t *code, size_t room);
static int shared_build(struct powervr_shader_workspace *workspace, struct powervr_shader_program *program);
static int split_shared(struct powervr_ir_shader *shader, struct powervr_shader_program *program, struct shader_uniform *uniform, struct powervr_ir_shader *shared, unsigned *moved);
static int uniform_mark(const struct powervr_ir_shader *shader, const struct powervr_shader_program *program, struct shader_uniform *uniform);
static int uniform_mixes(struct powervr_ir_shader *shader, const struct powervr_shader_program *program, struct shader_uniform *uniform);
static int uniform_operand(const struct powervr_ir_shader *shader, const struct shader_uniform *uniform, const struct powervr_ir_operand *operand);
static int uniform_scaled_sums(struct powervr_ir_shader *shader, const struct powervr_shader_program *program, struct shader_uniform *uniform);
static int uniform_scaled_products(struct powervr_ir_shader *shader, const struct powervr_shader_program *program, struct shader_uniform *uniform);
static unsigned uniform_kind(const struct powervr_ir_shader *shader, const struct powervr_shader_program *program, const struct shader_uniform *uniform, const struct powervr_ir_instruction *instruction);
static int uniform_colour(const struct powervr_ir_shader *shader, const struct shader_uniform *uniform, const struct powervr_ir_instruction *instruction);
static int shared_literal(struct powervr_shader_program *program, uint32_t bits, uint32_t *reg);
static int shared_shader_build(const struct powervr_ir_shader *shader, struct shader_uniform *uniform, struct powervr_ir_shader *shared, unsigned base);
static void shared_store(struct powervr_ir_instruction *instruction, unsigned value, unsigned reg);
static void drop_unread(struct powervr_ir_shader *shader, struct shader_uniform *uniform, const uint8_t *candidates);
static int end_on_last(const struct powervr_shader_program *program, const struct shader_backend *backend);
static void fold_moves(const struct powervr_shader_program *program, struct shader_backend *backend, unsigned output_base);
static int fold_move(struct shader_backend *backend, unsigned at, unsigned output_base);
static int simple_kind(unsigned kind);
static void merge_iterates(const struct powervr_shader_program *program, struct shader_backend *backend);
static void reuse_immediates(struct shader_backend *backend);
static void unpack_in_place(const struct powervr_shader_program *program, struct shader_backend *backend);
static void alias_coefficients(const struct powervr_ir_shader *shader, const struct powervr_shader_program *program, struct shader_backend *backend);
static void mark_iterate_samples(const struct powervr_ir_shader *shader, const struct powervr_shader_program *program, struct shader_backend *backend);
static int lone_iterate(const struct powervr_ir_shader *shader, const struct powervr_ir_operand *operand, const struct powervr_ir_instruction **iterate);
static int half_reader(const struct powervr_ir_shader *shader, unsigned value);
static uint32_t iterate_coefficient(const struct powervr_ir_shader *shader, unsigned value);
static void pair_operations(struct shader_backend *backend);
static int pairable(const struct shader_machine_operation *operation);
static int pair_group(const struct shader_machine_operation *first, const struct shader_machine_operation *second, struct powervr_usc_group *group);
static int commutes(const struct shader_machine_operation *a, const struct shader_machine_operation *b);
static int pairs_with(const struct shader_machine_operation *first, const struct shader_machine_operation *second);
static int writes_register(const struct shader_machine_operation *operation, struct shader_machine_register reg);
static int pack_constant(const struct powervr_ir_instruction *instruction, uint32_t *packed);
static int repeated_immediate(const struct shader_backend *backend, unsigned at);
static unsigned temporary_block(unsigned index);
static int reads_register(const struct shader_machine_operation *operation, struct shader_machine_register reg);
static int same_register(struct shader_machine_register a, struct shader_machine_register b);
static int dead_write(const struct powervr_ir_shader *shader, unsigned at);
static int select_instruction(struct backend_select *select, const struct powervr_ir_instruction *instruction);
static int select_structure(struct backend_select *select, const struct powervr_ir_instruction *instruction);

static struct shader_machine_operation *append(struct backend_select *select, unsigned kind);
static struct shader_machine_register temp_register(unsigned index);
static struct shader_machine_register bank_register(unsigned bank, unsigned index);
static struct shader_machine_register destination_of(struct backend_select *select, const struct powervr_ir_instruction *instruction);
static struct shader_machine_register operand_register(struct backend_select *select, const struct powervr_ir_operand *operand, unsigned *modifiers);
static struct shader_machine_register operand_plain(struct backend_select *select, const struct powervr_ir_operand *operand, unsigned allowed);
static struct shader_machine_register scratch_register(struct backend_select *select);
static int adjacent_words(const struct backend_select *select, const struct powervr_ir_instruction *instruction, struct shader_machine_register *reg);
static int literal_register(struct backend_select *select, uint32_t bits, struct shader_machine_register *reg);
static void select_float_pair(struct backend_select *select, unsigned kind, struct shader_machine_register destination, const struct powervr_ir_operand *a, const struct powervr_ir_operand *b, unsigned floor);
static void select_two_sourced(struct backend_select *select, unsigned kind, unsigned variant, uint32_t type, struct shader_machine_register destination, const struct powervr_ir_operand *a, const struct powervr_ir_operand *b, unsigned allowed);
static void select_address(struct backend_select *select, const struct powervr_ir_instruction *instruction);
static void select_sum(struct backend_select *select, const struct powervr_ir_instruction *instruction, enum powervr_usc_sum form);
static int select_sum_half(struct backend_select *select, const struct powervr_ir_instruction *instruction, enum powervr_usc_sum form);
static void select_condition(struct backend_select *select, unsigned kind, unsigned predicate, unsigned adjust, struct shader_machine_register source);
static unsigned select_branch(struct backend_select *select, unsigned prediction, unsigned condition);
static int build_group_fetch(void *context, size_t index, struct powervr_usc_group *group);
static void group_register(struct powervr_usc_register *operand, struct shader_machine_register reg);

/*
 * Where a sum of products takes its registers, one row per form
 * of enum powervr_usc_sum and one entry per source of the group: the
 * colour's channel (0 red to 3 alpha), SUM_DESTINATION for the pixel
 * output it blends with, or SUM_UNUSED.  Sources 1 and 4 are the second
 * of their operand words, whose bank field has two bits.
 */
static const uint8_t sum_slots[POWERVR_USC_SUM_COUNT][6] = {
	{ 0, 1, 2, 3, SUM_UNUSED, SUM_UNUSED },
	{ SUM_DESTINATION, 3, 0, 1, 2, SUM_UNUSED },
	{ SUM_DESTINATION, 3, 0, 1, 2, SUM_UNUSED },
	{ SUM_DESTINATION, 0, 1, 2, 3, SUM_UNUSED },
	{ SUM_DESTINATION, 0, 1, 2, 3, SUM_UNUSED },
	{ 0, 1, SUM_UNUSED, SUM_UNUSED, SUM_UNUSED, SUM_UNUSED },
	{ SUM_DESTINATION, 1, 0, SUM_UNUSED, SUM_UNUSED, SUM_UNUSED }
};

/* A group with no operation, no operand and a repeat count of one. */
static const struct powervr_usc_group blank_group = {
	.repeat = 1
};

/*
 * Builds a shader's machine operations: liveness, register allocation,
 * selection, the ending; and fills in the program's register counts.
 */
int
shader_backend_build(
	const struct powervr_ir_shader *shader,
	struct powervr_shader_program *program,
	struct shader_backend *backend)
{
	struct backend_select select;
	struct shader_machine_operation *operation;
	unsigned at;
	unsigned index;
	int error;

	/* Clears the back end's state. */
	backend->operation_count = 0;
	backend->temps = TEMP_FIRST_VALUE;
	backend->control_flow = 0;
	backend->error = 0;

	/* The shared words only float operations read are read where they are; the varyings samples iterate themselves. */
	alias_shared(shader, program, backend);
	alias_coefficients(shader, program, backend);
	mark_iterate_samples(shader, program, backend);

	/* Works out where each value is live, and gives it registers. */
	error = liveness(shader, backend);
	if (error != 0)
		return error;
	error = allocate(shader, backend);
	if (error != 0)
		return error;

	/* Fragment colours go to the temporaries after the values. */
	select.shader = shader;
	select.program = program;
	select.backend = backend;
	select.if_depth = 0;
	select.loop_depth = 0;
	select.scratch = 0;
	select.output_base = backend->temps;
	select.literals = 0;
	if (program->stage == POWERVR_SHADER_FRAGMENT) {
		for (index = 0; index < program->output_count; index++) {
			if (program->outputs[index].format != 0)
				continue;
			program->outputs[index].reg = select.output_base + program->outputs[index].location * 4U;
			if (program->outputs[index].reg + 4U > backend->temps)
				backend->temps = program->outputs[index].reg + 4U;
		}
	}
	if (backend->temps > SHADER_MACHINE_TEMPORARIES)
		return ENOSPC;

	/* Starts the execution mask counter when the shader branches. */
	for (at = 0; at < shader->instruction_count; at++) {
		if (shader->instructions[at].opcode == POWERVR_IR_IF || shader->instructions[at].opcode == POWERVR_IR_LOOP)
			backend->control_flow = 1;
	}
	if (backend->control_flow != 0)
		select_condition(&select, CONDITION_START, PREDICATE_ALWAYS, 1, bank_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_ZERO));

	/* Each instruction in turn. */
	for (at = 0; at < shader->instruction_count; at++) {
		select.scratch = 0;
		error = select_instruction(&select, &shader->instructions[at]);
		if (error != 0)
			return error;
		if (backend->error != 0)
			return backend->error;
	}
	if (select.if_depth != 0 || select.loop_depth != 0)
		return EINVAL;

	/* Moves whose source was just computed for them write in place; neighbouring iterates go as one. */
	fold_moves(program, backend, select.output_base);
	merge_iterates(program, backend);
	reuse_immediates(backend);
	unpack_in_place(program, backend);
	if (POWERVR_SHADER_PAIR != 0)
		pair_operations(backend);

	/*
	 * The ending: a vertex emits its outputs, every task ends; a
	 * fragment program without branches ends on its last move or
	 * immediate when nothing branches past it (one group less a
	 * pixel, as Mesa's pco_end sets the end on the last instruction).
	 */
	if (program->stage == POWERVR_SHADER_VERTEX) {
		operation = append(&select, MACHINE_VERTEX_END);
	} else if (end_on_last(program, backend)) {
		operation = &backend->operations[backend->operation_count - 1U];
	} else {
		operation = append(&select, MACHINE_END);
	}
	if (operation != NULL)
		operation->end = 1;
	if (backend->error != 0)
		return backend->error;

	/* The back end's own temporaries a program does not use are given back. */
	compact_temporaries(program, backend);

	/* Succeeded: the operations and the register count. */
	program->temps = backend->temps;
	return 0;
}

/*
 * Gives back the back end's own temporaries (0 to 8) a program does not
 * use, in three blocks: the execution mask counter (0), the scratch (1 to
 * 3), and the coordinates, address and store data (4 to 8, which stay
 * together: a store reads its data from the register after the address
 * pair).  The blocks used move down to the start, the values after them
 * (every program took the nine whatever it used).
 */
static void
compact_temporaries(
	struct powervr_shader_program *program,
	struct shader_backend *backend)
{
	static const unsigned starts[3] = { TEMP_EMC, TEMP_SCRATCH, TEMP_COORDINATES };
	static const unsigned ends[3] = { TEMP_SCRATCH, TEMP_COORDINATES, TEMP_FIRST_VALUE };
	struct shader_machine_operation *operation;
	struct shader_machine_register *reg;
	unsigned used[3];
	unsigned bases[3];
	unsigned total;
	unsigned shift;
	unsigned index;
	unsigned slot;
	unsigned block;

	/* The blocks any operation names. */
	for (block = 0; block < 3U; block++)
		used[block] = 0;
	for (index = 0; index < backend->operation_count; index++) {
		operation = &backend->operations[index];
		for (slot = 0; slot < 6U; slot++) {
			reg = slot < 2U ? &operation->destination[slot] : &operation->source[slot - 2U];
			if (reg->bank != POWERVR_USC_BANK_TEMPORARY || reg->index >= TEMP_FIRST_VALUE)
				continue;
			for (block = 0; block < 3U; block++) {
				if (reg->index >= starts[block] && reg->index < ends[block])
					used[block] = 1;
			}
		}
	}

	/* The used blocks' new places, and how far the values move down. */
	total = 0;
	for (block = 0; block < 3U; block++) {
		bases[block] = total;
		if (used[block])
			total += ends[block] - starts[block];
	}
	shift = TEMP_FIRST_VALUE - total;
	if (shift == 0)
		return;

	/* Every temporary renamed. */
	for (index = 0; index < backend->operation_count; index++) {
		operation = &backend->operations[index];
		for (slot = 0; slot < 6U; slot++) {
			reg = slot < 2U ? &operation->destination[slot] : &operation->source[slot - 2U];
			if (reg->bank != POWERVR_USC_BANK_TEMPORARY)
				continue;
			if (reg->index >= TEMP_FIRST_VALUE) {
				reg->index = (uint16_t)(reg->index - shift);
				continue;
			}
			for (block = 0; block < 3U; block++) {
				if (reg->index >= starts[block] && reg->index < ends[block])
					reg->index = (uint16_t)(bases[block] + reg->index - starts[block]);
			}
		}
	}

	/* The fragment colours kept in temporaries, and the count. */
	if (program->stage == POWERVR_SHADER_FRAGMENT) {
		for (index = 0; index < program->output_count; index++) {
			if (program->outputs[index].format == 0 && program->outputs[index].reg >= TEMP_FIRST_VALUE)
				program->outputs[index].reg -= shift;
		}
	}
	backend->temps -= shift;
}

/*
 * Compiles a SPIR-V module's entry point of a stage into a shading
 * cluster program in code, and fills in what the driver needs to start
 * it.
 */
int
powervr_shader_compile(
	const uint32_t *words,
	size_t count,
	uint32_t stage,
	const struct powervr_shader_key *key,
	struct powervr_shader_workspace *workspace,
	struct powervr_shader_program *program,
	uint8_t *code,
	size_t room)
{
	int error;

	/* The shared registers stay registers. */
	error = powervr_shader_compile_inline(words, count, stage, key, NULL, 0, workspace, program, code, room);
	if (error != 0)
		return error;

	/* Succeeded: the program and its code. */
	return 0;
}

/*
 * Compiles a SPIR-V module's entry point with the shared register words
 * given up front: each read of one becomes the constant, so the program
 * runs without a data sequencer program filling shared registers.  The
 * values are indexed like the shared registers the requests would fill
 * (the program's request table says which is which); a value table of
 * NULL compiles the ordinary program.
 */
int
powervr_shader_compile_inline(
	const uint32_t *words,
	size_t count,
	uint32_t stage,
	const struct powervr_shader_key *key,
	const uint32_t *shared_values,
	uint32_t value_count,
	struct powervr_shader_workspace *workspace,
	struct powervr_shader_program *program,
	uint8_t *code,
	size_t room)
{
	struct powervr_shader_key plain;
	unsigned split;
	int error;

	/* A keyed fragment program read through shared registers may move its uniform work to a shared program. */
	split = 0;
	if (stage == POWERVR_SHADER_FRAGMENT &&
	    key != NULL &&
	    (key->forms & POWERVR_SHADER_FORM_SHARED_PROGRAM) != 0 &&
	    shared_values == NULL)
		split = 1;

	/* Compiles, with the shared program when it may have one. */
	error = compile_program(words, count, stage, key, shared_values, value_count, split, workspace, program, code, room);

	/* A program whose shared program could not be built is compiled again without one. */
	if (error != 0 && split != 0)
		error = compile_program(words, count, stage, key, shared_values, value_count, 0, workspace, program, code, room);

	/* A program the sum of products could not write is compiled again with the arithmetic. */
	if (error != 0 && key != NULL && (key->forms & POWERVR_SHADER_FORM_SUM_OF_PRODUCTS) != 0) {
		plain = *key;
		plain.forms &= ~POWERVR_SHADER_FORM_SUM_OF_PRODUCTS;
		error = powervr_shader_compile_inline(words, count, stage, &plain, shared_values, value_count, workspace, program, code, room);
	}
	if (error != 0)
		return error;

	/* Succeeded: the program and its code. */
	return 0;
}

/*
 * Compiles a module once: reads it, simplifies the intermediate form,
 * with split moves the uniform work to the shared program and builds
 * that first (so the program's own operations stay the back end's last,
 * for the host check's simulator), then selects, allocates and encodes
 * the program, and puts the shared program's code after it.
 */
static int
compile_program(
	const uint32_t *words,
	size_t count,
	uint32_t stage,
	const struct powervr_shader_key *key,
	const uint32_t *shared_values,
	uint32_t value_count,
	unsigned split,
	struct powervr_shader_workspace *workspace,
	struct powervr_shader_program *program,
	uint8_t *code,
	size_t room)
{
	struct powervr_ir_shader *shader;
	size_t length;
	size_t offset;
	size_t index;
	unsigned moved;
	int error;

	/* Reads the module into the intermediate form. */
	error = powervr_shader_read(words, count, stage, key, workspace, program, &shader);
	if (error != 0)
		return error;

	/* Refuses a value table too short for the shared registers. */
	if (shared_values != NULL && value_count < program->shareds)
		return EINVAL;

	/* Reads copies' sources in place of the copies, fuses products into the adds of them, and drops the writes nothing reads. */
	simplify_products(shader);
	propagate_copies(shader);
	remove_dead_writes(shader);
	fuse_multiply_adds(shader, stage);
	propagate_copies(shader);
	remove_dead_writes(shader);

	/* Moves the work only uniform words decide to the shared program, and builds it; the other forms the key takes. */
	workspace->backend.inline_values = shared_values;
	workspace->backend.inline_count = value_count;
	workspace->backend.forms = 0;
	if (key != NULL && stage == POWERVR_SHADER_FRAGMENT)
		workspace->backend.forms = key->forms & (POWERVR_SHADER_FORM_ITERATE_SAMPLE | POWERVR_SHADER_FORM_SUM_OF_PRODUCTS | POWERVR_SHADER_FORM_SAMPLE_WAITS);
	moved = 0;
	if (split != 0) {
		error = uniform_mixes(shader, program, &workspace->uniform);
		if (error == 0)
			error = uniform_scaled_sums(shader, program, &workspace->uniform);
		if (error == 0)
			error = uniform_scaled_products(shader, program, &workspace->uniform);
		if (error != 0)
			return error;
		error = split_shared(shader, program, &workspace->uniform, &workspace->shared_shader, &moved);
		if (error != 0)
			return error;
	}
	if (moved != 0) {
		error = shared_build(workspace, program);
		if (error != 0)
			return error;
	}

	/* Gives the constants several multiply-adds add values of their own. */
	hoist_addends(shader, stage);

	/* Selects and allocates the machine operations. */
	error = shader_backend_build(shader, program, &workspace->backend);
	if (error != 0)
		return error;

	/* Encodes them. */
	error = shader_backend_encode(&workspace->backend, code, room, &length);
	if (error != 0)
		return error;

	/* The program alone, or with the shared program after it on a 16-byte boundary. */
	program->length = length;
	if (program->shared_length != 0) {
		offset = (length + 15U) & ~(size_t)15U;
		if (offset > room || program->shared_length > room - offset)
			return ENOSPC;
		for (index = length; index < offset; index++)
			code[index] = 0;
		for (index = 0; index < program->shared_length; index++)
			code[offset + index] = workspace->shared_code[index];
		program->shared_offset = offset;
		program->length = offset + program->shared_length;
	}

	/* Succeeded: the program and its length. */
	return 0;
}

/*
 * Builds a fragment program's shared program from its intermediate
 * form: selected as a compute task's, with the fragment program's
 * requests, so it reads the shared registers the data sequencer fills
 * and writes the computed ones; its operations are kept for the host
 * check and its code encoded beside them.
 */
static int
shared_build(
	struct powervr_shader_workspace *workspace,
	struct powervr_shader_program *program)
{
	struct powervr_shader_program *shared;
	size_t length;
	unsigned index;
	int error;

	/* A compute task's description with the program's requests and shared registers. */
	shared = &workspace->shared_program;
	*shared = *program;
	shared->stage = POWERVR_SHADER_COMPUTE;
	shared->output_count = 0;

	/* Selects and allocates its operations. */
	error = shader_backend_build(&workspace->shared_shader, shared, &workspace->backend);
	if (error != 0)
		return error;

	/* Keeps them, as the program's own selection reuses the back end. */
	if (workspace->backend.operation_count > SHADER_SHARED_OPERATIONS)
		return E2BIG;
	for (index = 0; index < workspace->backend.operation_count; index++)
		workspace->shared_operations[index] = workspace->backend.operations[index];
	workspace->shared_operation_count = workspace->backend.operation_count;

	/* Encodes them. */
	error = shader_backend_encode(&workspace->backend, workspace->shared_code, SHADER_SHARED_CODE, &length);
	if (error != 0)
		return error;

	/* Succeeded: the shared program's code and its temporaries. */
	program->shared_length = length;
	program->shared_temps = shared->temps;
	return 0;
}

/*
 * Splits from a fragment program the work only uniform words decide
 * (the board's flat quad of a push constant colour cost 2 ms a
 * 1080p frame more than one of a constant colour, the colour's pack a
 * pixel, where the proprietary driver's cost a tenth of that, as it
 * works such values out once a draw).
 *
 * A value is uniform when it is written once, outside every if and loop,
 * by arithmetic of constants and uniform values, or is a shared word the
 * data sequencer loads.  Those instructions are copied to the shared
 * program in order; each computed uniform value the rest of the program
 * reads, and each colour packed only from uniform values, becomes a word
 * of the computed request, which the shared program stores and the
 * program reads in place of its own work (the colour moved into the
 * pixel output as it is).  A colour of noughts and ones becomes a
 * literal the data sequencer writes.  Moved reports the computed words,
 * none when there is no shared program to build.
 */
static int
split_shared(
	struct powervr_ir_shader *shader,
	struct powervr_shader_program *program,
	struct shader_uniform *uniform,
	struct powervr_ir_shader *shared,
	unsigned *moved)
{
	struct powervr_ir_instruction *instruction;
	struct powervr_shader_request *request;
	const struct powervr_ir_operand *operand;
	uint32_t packed;
	uint32_t reg;
	int colour;
	int constant;
	unsigned at;
	unsigned value;
	unsigned source;
	unsigned slots;
	unsigned computed;
	unsigned base;
	int error;

	/* The uniform values and the instructions that write them. */
	*moved = 0;
	error = uniform_mark(shader, program, uniform);
	if (error != 0)
		return error;

	/*
	 * The words: each computed uniform value an instruction left in the
	 * program reads, and each colour packed only from uniform values
	 * (one of noughts and ones a literal at once), while there is room.
	 */
	slots = 0;
	for (at = 0; at < shader->instruction_count; at++) {
		instruction = &shader->instructions[at];
		if (uniform->moved[at] != 0)
			continue;

		/* A uniform colour: a literal when it is noughts and ones, else a word of its own. */
		colour = uniform_colour(shader, uniform, instruction);
		if (colour) {
			constant = pack_constant(instruction, &packed);
			error = ENOENT;
			if (constant)
				error = shared_literal(program, packed, &reg);
			if (error == 0) {
				instruction->opcode = POWERVR_IR_PIXEL_SHARED;
				instruction->immediate_2 = reg;
				for (source = 0; source < 4; source++)
					instruction->source[source].kind = POWERVR_IR_OPERAND_NONE;
			} else if (slots < SHADER_SHARED_WORDS) {
				uniform->pack_slot[at] = (uint8_t)++slots;
			}
			continue;
		}

		/* The computed uniform values it reads. */
		for (source = 0; source < 4; source++) {
			operand = &instruction->source[source];
			if (operand->kind != POWERVR_IR_OPERAND_VALUE || operand->value >= shader->value_count)
				continue;
			value = operand->value;
			if (uniform->kind[value] != SHADER_UNIFORM_COMPUTED ||
			    uniform->slot[value] != 0 ||
			    slots >= SHADER_SHARED_WORDS)
				continue;
			uniform->slot[value] = (uint8_t)++slots;
		}
	}

	/* No word, or no room for their request: the program stays as it is but for the literal colours. */
	if (slots == 0 || program->request_count >= POWERVR_SHADER_REQUESTS)
		return 0;

	/* The computed words' request, after the others' shared registers. */
	computed = program->request_count++;
	request = &program->requests[computed];
	request->kind = POWERVR_SHADER_REQUEST_COMPUTED;
	request->part = 0;
	request->words = (uint8_t)slots;
	request->shared = (uint16_t)program->shareds;
	request->set = 0;
	request->binding = 0;
	request->array_index = 0;
	request->offset = 0;
	request->literal = 0;
	base = program->shareds;
	program->shareds += slots;

	/* The shared program: the uniform instructions, then the stores of the words. */
	error = shared_shader_build(shader, uniform, shared, base);
	if (error != 0)
		return error;

	/* The program reads the words in place of the work. */
	for (at = 0; at < shader->instruction_count; at++) {
		instruction = &shader->instructions[at];
		if (uniform->pack_slot[at] != 0) {
			/* The packed colour, moved into the pixel output. */
			instruction->opcode = POWERVR_IR_PIXEL_SHARED;
			instruction->immediate_2 = base + uniform->pack_slot[at] - 1U;
			for (source = 0; source < 4; source++)
				instruction->source[source].kind = POWERVR_IR_OPERAND_NONE;
			continue;
		}
		if (uniform->moved[at] == 0 || uniform->slot[instruction->destination] == 0)
			continue;

		/* The computed word, read from its shared register, which stays. */
		value = instruction->destination;
		instruction->opcode = POWERVR_IR_SHARED;
		instruction->flags = 0;
		for (source = 0; source < 4; source++)
			instruction->source[source].kind = POWERVR_IR_OPERAND_NONE;
		instruction->immediate = computed;
		instruction->immediate_2 = uniform->slot[value] - 1U;
		uniform->moved[at] = 0;
	}

	/* The moved work nothing left reads goes. */
	drop_unread(shader, uniform, uniform->moved);

	/* Succeeded: the computed words. */
	*moved = slots;
	return 0;
}

/*
 * Marks the uniform values (split_shared): written once outside every if
 * and loop, only from uniform values and constants, or a shared word the
 * data sequencer loads; moved marks the instructions that write them.
 */
static int
uniform_mark(
	const struct powervr_ir_shader *shader,
	const struct powervr_shader_program *program,
	struct shader_uniform *uniform)
{
	const struct powervr_ir_instruction *instruction;
	unsigned opcode;
	unsigned depth;
	unsigned value;
	unsigned at;

	/* Nothing known of any value or instruction yet. */
	for (value = 0; value < shader->value_count; value++) {
		uniform->writes[value] = 0;
		uniform->kind[value] = SHADER_UNIFORM_VARYING;
		uniform->slot[value] = 0;
	}
	for (at = 0; at < shader->instruction_count; at++) {
		uniform->moved[at] = 0;
		uniform->pack_slot[at] = 0;
	}

	/* How often each value is written. */
	for (at = 0; at < shader->instruction_count; at++) {
		instruction = &shader->instructions[at];
		value = instruction->destination;
		if (instruction->opcode < POWERVR_IR_MOVE || value == 0 || value >= shader->value_count)
			continue;
		if (uniform->writes[value] < UINT16_MAX)
			uniform->writes[value]++;
	}

	/* The uniform values, in order. */
	depth = 0;
	for (at = 0; at < shader->instruction_count; at++) {
		instruction = &shader->instructions[at];
		opcode = instruction->opcode;
		if (opcode == POWERVR_IR_IF || opcode == POWERVR_IR_LOOP) {
			depth++;
			continue;
		}
		if (opcode == POWERVR_IR_END_IF || opcode == POWERVR_IR_END_LOOP) {
			if (depth == 0)
				return EINVAL;
			depth--;
			continue;
		}
		if (depth != 0 || opcode < POWERVR_IR_MOVE)
			continue;
		value = instruction->destination;
		if (value == 0 ||
		    value >= shader->value_count ||
		    shader->values[value].width != 1 ||
		    uniform->writes[value] != 1)
			continue;
		uniform->kind[value] = (uint8_t)uniform_kind(shader, program, uniform, instruction);
		if (uniform->kind[value] != SHADER_UNIFORM_VARYING)
			uniform->moved[at] = 1;
	}

	/* Succeeded: the values are marked. */
	return 0;
}

/* The most different weights uniform_mixes keeps one minus of. */
#define UNIFORM_MIX_WEIGHTS	8U

/*
 * Rewrites each mix of a varying x towards a uniform y by a uniform
 * weight z, x + (y - x) * z (the reader's two instructions, a pixel's
 * add and multiply-add), as x * (1 - z) + y * z: both products' factors
 * then are uniform, so the shared program works them out once a draw
 * and the pixel's work is one multiply-add.  One minus a weight is
 * worked out once for the mixes of that weight.
 */
static int
uniform_mixes(
	struct powervr_ir_shader *shader,
	const struct powervr_shader_program *program,
	struct shader_uniform *uniform)
{
	struct powervr_ir_instruction *mix;
	struct powervr_ir_instruction *difference;
	struct powervr_ir_instruction *inserted;
	struct powervr_ir_operand weights[UNIFORM_MIX_WEIGHTS];
	struct powervr_ir_operand x;
	struct powervr_ir_operand y;
	struct powervr_ir_operand rest;
	struct powervr_ir_operand product;
	unsigned rests[UNIFORM_MIX_WEIGHTS];
	unsigned weight_count;
	unsigned writes;
	unsigned writer;
	unsigned reads;
	unsigned slot;
	unsigned side;
	unsigned at;
	unsigned index;
	unsigned found;
	int error;

	/* Only a fragment program's work moves to a shared program. */
	if (program->stage != POWERVR_SHADER_FRAGMENT)
		return 0;
	error = uniform_mark(shader, program, uniform);
	if (error != 0)
		return error;

	/* Each multiply-add of a difference read once and a uniform weight, adding the difference's subtrahend. */
	weight_count = 0;
	for (at = 0; at < shader->instruction_count; at++) {
		mix = &shader->instructions[at];
		if (mix->opcode != POWERVR_IR_FLOAT_MULTIPLY_ADD || mix->source[2].kind != POWERVR_IR_OPERAND_VALUE || mix->source[2].modifiers != 0)
			continue;
		x = mix->source[2];
		if (x.value >= shader->value_count || uniform->kind[x.value] != SHADER_UNIFORM_VARYING)
			continue;
		for (slot = 0; slot < 2; slot++) {
			/* The difference, written once by an add, read only here. */
			if (mix->source[slot].kind != POWERVR_IR_OPERAND_VALUE ||
			    mix->source[slot].modifiers != 0 ||
			    mix->source[slot].component != 0 ||
			    mix->source[slot].value >= shader->value_count ||
			    shader->values[mix->source[slot].value].width != 1 ||
			    !uniform_operand(shader, uniform, &mix->source[1U - slot]))
				continue;
			reads = value_references(shader, mix->source[slot].value, &writes, &writer);
			if (reads != 1 || writes != 1 || writer >= at)
				continue;
			difference = &shader->instructions[writer];
			if (difference->opcode != POWERVR_IR_FLOAT_ADD || difference->flags != 0)
				continue;

			/* Its uniform minuend y, and x negated. */
			for (side = 0; side < 2; side++) {
				if (difference->source[1U - side].kind == POWERVR_IR_OPERAND_VALUE &&
				    difference->source[1U - side].value == x.value &&
				    difference->source[1U - side].component == x.component &&
				    difference->source[1U - side].modifiers == POWERVR_IR_NEGATE &&
				    uniform_operand(shader, uniform, &difference->source[side]))
					break;
			}
			if (side == 2)
				continue;
			y = difference->source[side];

			/* The weight written before the difference (a constant always is). */
			if (mix->source[1U - slot].kind == POWERVR_IR_OPERAND_VALUE) {
				(void)value_references(shader, mix->source[1U - slot].value, &writes, &writer);
				if (writer >= (unsigned)(difference - shader->instructions))
					continue;
			}

			/* One minus the weight, once for the weight: inserted before the mix. */
			found = UNIFORM_MIX_WEIGHTS;
			for (index = 0; index < weight_count; index++) {
				if (weights[index].kind == mix->source[1U - slot].kind &&
				    weights[index].value == mix->source[1U - slot].value &&
				    weights[index].constant == mix->source[1U - slot].constant &&
				    weights[index].component == mix->source[1U - slot].component)
					found = index;
			}
			if (found == UNIFORM_MIX_WEIGHTS) {
				if (weight_count == UNIFORM_MIX_WEIGHTS ||
				    shader->instruction_count >= POWERVR_IR_INSTRUCTIONS ||
				    shader->value_count >= POWERVR_IR_VALUES)
					continue;
				for (index = shader->instruction_count; index > at; index--)
					shader->instructions[index] = shader->instructions[index - 1U];
				shader->instruction_count++;
				inserted = &shader->instructions[at];
				mix = &shader->instructions[at + 1U];
				*inserted = (struct powervr_ir_instruction){ 0 };
				inserted->opcode = POWERVR_IR_FLOAT_ADD;
				inserted->destination = (uint16_t)shader->value_count;
				inserted->source[0].kind = POWERVR_IR_OPERAND_CONSTANT;
				inserted->source[0].constant = 0x3f800000U;
				inserted->source[1] = mix->source[1U - slot];
				inserted->source[1].modifiers = POWERVR_IR_NEGATE;
				shader->values[shader->value_count].width = 1;
				weights[weight_count] = mix->source[1U - slot];
				rests[weight_count] = shader->value_count;
				found = weight_count;
				weight_count++;
				shader->value_count++;
				at++;
			}
			rest = (struct powervr_ir_operand){ 0 };
			rest.kind = POWERVR_IR_OPERAND_VALUE;
			rest.value = (uint16_t)rests[found];

			/* The difference becomes y * z, and the mix x * (1 - z) + y * z. */
			product = mix->source[slot];
			difference->opcode = POWERVR_IR_FLOAT_MULTIPLY;
			difference->source[0] = y;
			difference->source[1] = mix->source[1U - slot];
			mix->source[0] = x;
			mix->source[1] = rest;
			mix->source[2] = product;
			break;
		}
	}

	/* Succeeded: the mixes read uniform products. */
	return 0;
}

/*
 * Rewrites each product of a sum and a uniform factor u2, (x + u1) * u2
 * with u1 uniform (either term may be negated, as a difference is; the
 * product may be a multiply-add of zero, a saturated multiply), as
 * x * u2 + u1 * u2: the shared program works out u1 * u2 once a draw,
 * and the pixel's add and multiply become one multiply-add.
 */
static int
uniform_scaled_sums(
	struct powervr_ir_shader *shader,
	const struct powervr_shader_program *program,
	struct shader_uniform *uniform)
{
	struct powervr_ir_instruction *product;
	struct powervr_ir_instruction *sum;
	struct powervr_ir_operand x;
	struct powervr_ir_operand scale;
	struct powervr_ir_operand term;
	struct powervr_ir_instruction moved;
	unsigned writes;
	unsigned writer;
	unsigned reads;
	unsigned slot;
	unsigned side;
	unsigned first;
	unsigned index;
	unsigned at;
	int error;

	/* Only a fragment program's work moves to a shared program. */
	if (program->stage != POWERVR_SHADER_FRAGMENT)
		return 0;
	error = uniform_mark(shader, program, uniform);
	if (error != 0)
		return error;

	/* Each multiply of a sum read once by a uniform factor. */
	for (at = 0; at < shader->instruction_count; at++) {
		product = &shader->instructions[at];
		if (product->opcode != POWERVR_IR_FLOAT_MULTIPLY &&
		    !(product->opcode == POWERVR_IR_FLOAT_MULTIPLY_ADD && product->source[2].kind == POWERVR_IR_OPERAND_CONSTANT &&
		      product->source[2].constant == 0 && product->source[2].modifiers == 0))
			continue;
		for (slot = 0; slot < 2; slot++) {
			/* The sum, written once by an unsaturated add, read only here; the factor uniform. */
			if (product->source[slot].kind != POWERVR_IR_OPERAND_VALUE ||
			    product->source[slot].modifiers != 0 ||
			    product->source[slot].component != 0 ||
			    product->source[slot].value >= shader->value_count ||
			    shader->values[product->source[slot].value].width != 1 ||
			    !uniform_operand(shader, uniform, &product->source[1U - slot]))
				continue;
			reads = value_references(shader, product->source[slot].value, &writes, &writer);
			if (reads != 1 || writes != 1 || writer >= at)
				continue;
			sum = &shader->instructions[writer];
			if (sum->opcode != POWERVR_IR_FLOAT_ADD || sum->flags != 0)
				continue;

			/* One term a varying value, the other uniform, each plain or negated. */
			for (side = 0; side < 2; side++) {
				if (sum->source[side].kind == POWERVR_IR_OPERAND_VALUE &&
				    sum->source[side].value < shader->value_count &&
				    uniform->kind[sum->source[side].value] == SHADER_UNIFORM_VARYING &&
				    (sum->source[side].modifiers & ~POWERVR_IR_NEGATE) == 0 &&
				    (sum->source[1U - side].modifiers & ~POWERVR_IR_NEGATE) == 0)
					break;
			}
			if (side == 2)
				continue;
			x = sum->source[side];
			term = sum->source[1U - side];
			term.modifiers = 0;
			if (!uniform_operand(shader, uniform, &term))
				continue;
			term.modifiers = sum->source[1U - side].modifiers;

			/*
			 * The factor written after the sum (a shared word read late): the
			 * sum moves to just before the product, with no structure
			 * between, as nothing else reads it.
			 */
			scale = product->source[1U - slot];
			if (scale.kind == POWERVR_IR_OPERAND_VALUE) {
				(void)value_references(shader, scale.value, &writes, &writer);
				first = (unsigned)(sum - shader->instructions);
				if (writer >= first) {
					for (index = first + 1U; index < at; index++) {
						if (shader->instructions[index].opcode < POWERVR_IR_MOVE)
							break;
					}
					if (index != at)
						continue;
					moved = *sum;
					for (index = first; index + 1U < at; index++)
						shader->instructions[index] = shader->instructions[index + 1U];
					shader->instructions[at - 1U] = moved;
					sum = &shader->instructions[at - 1U];
				}
			}

			/* The sum becomes u1 * u2, and the product x * u2 + u1 * u2. */
			sum->opcode = POWERVR_IR_FLOAT_MULTIPLY;
			sum->source[0] = term;
			sum->source[1] = scale;
			product->opcode = POWERVR_IR_FLOAT_MULTIPLY_ADD;
			product->source[2] = product->source[slot];
			product->source[0] = x;
			product->source[1] = scale;
			break;
		}
	}

	/* Succeeded: the products read uniform terms. */
	return 0;
}

/*
 * Rewrites each product of a product and a uniform factor u2,
 * (x * u1) * u2 with u1 uniform and the inner product read only there
 * (the product may be a multiply-add of zero, a saturated multiply), as
 * x * (u1 * u2): the shared program works out u1 * u2 once a draw, and
 * the pixel's two multiplies become one (a colour and an opacity, each
 * a push constant, scaled a cover so).
 */
static int
uniform_scaled_products(
	struct powervr_ir_shader *shader,
	const struct powervr_shader_program *program,
	struct shader_uniform *uniform)
{
	struct powervr_ir_instruction *product;
	struct powervr_ir_instruction *inner;
	struct powervr_ir_instruction moved;
	struct powervr_ir_operand x;
	struct powervr_ir_operand scale;
	struct powervr_ir_operand term;
	unsigned writes;
	unsigned writer;
	unsigned reads;
	unsigned slot;
	unsigned side;
	unsigned first;
	unsigned index;
	unsigned at;
	int error;

	/* Only a fragment program's work moves to a shared program. */
	if (program->stage != POWERVR_SHADER_FRAGMENT)
		return 0;
	error = uniform_mark(shader, program, uniform);
	if (error != 0)
		return error;

	/* Each multiply of an unsaturated product read once by a uniform factor. */
	for (at = 0; at < shader->instruction_count; at++) {
		product = &shader->instructions[at];
		if (product->opcode != POWERVR_IR_FLOAT_MULTIPLY &&
		    !(product->opcode == POWERVR_IR_FLOAT_MULTIPLY_ADD && product->source[2].kind == POWERVR_IR_OPERAND_CONSTANT &&
		      product->source[2].constant == 0 && product->source[2].modifiers == 0))
			continue;
		for (slot = 0; slot < 2; slot++) {
			/* The inner product, written once by an unsaturated multiply, read only here; the factor uniform. */
			if (product->source[slot].kind != POWERVR_IR_OPERAND_VALUE ||
			    product->source[slot].modifiers != 0 ||
			    product->source[slot].component != 0 ||
			    product->source[slot].value >= shader->value_count ||
			    shader->values[product->source[slot].value].width != 1 ||
			    uniform->kind[product->source[slot].value] != SHADER_UNIFORM_VARYING ||
			    !uniform_operand(shader, uniform, &product->source[1U - slot]))
				continue;
			reads = value_references(shader, product->source[slot].value, &writes, &writer);
			if (reads != 1 || writes != 1 || writer >= at)
				continue;
			inner = &shader->instructions[writer];
			if (inner->opcode != POWERVR_IR_FLOAT_MULTIPLY || inner->flags != 0)
				continue;

			/* One factor a varying value, the other uniform, each plain or negated. */
			for (side = 0; side < 2; side++) {
				if (inner->source[side].kind == POWERVR_IR_OPERAND_VALUE &&
				    inner->source[side].value < shader->value_count &&
				    uniform->kind[inner->source[side].value] == SHADER_UNIFORM_VARYING &&
				    (inner->source[side].modifiers & ~POWERVR_IR_NEGATE) == 0 &&
				    (inner->source[1U - side].modifiers & ~POWERVR_IR_NEGATE) == 0)
					break;
			}
			if (side == 2)
				continue;
			x = inner->source[side];
			term = inner->source[1U - side];
			term.modifiers = 0;
			if (!uniform_operand(shader, uniform, &term))
				continue;
			term.modifiers = inner->source[1U - side].modifiers;

			/* The factor written after the inner product: the inner product moves to just before the product. */
			scale = product->source[1U - slot];
			if (scale.kind == POWERVR_IR_OPERAND_VALUE) {
				(void)value_references(shader, scale.value, &writes, &writer);
				first = (unsigned)(inner - shader->instructions);
				if (writer >= first) {
					for (index = first + 1U; index < at; index++) {
						if (shader->instructions[index].opcode < POWERVR_IR_MOVE)
							break;
					}
					if (index != at)
						continue;
					moved = *inner;
					for (index = first; index + 1U < at; index++)
						shader->instructions[index] = shader->instructions[index + 1U];
					shader->instructions[at - 1U] = moved;
					inner = &shader->instructions[at - 1U];
				}
			}

			/* The inner product becomes u1 * u2, and the product x * (u1 * u2). */
			inner->source[0] = term;
			inner->source[1] = scale;
			product->source[1U - slot] = x;
			break;
		}
	}

	/* Succeeded: the products read uniform factors. */
	return 0;
}

/* Reports whether an operand is a constant or a word of a uniform value, without modifiers. */
static int
uniform_operand(
	const struct powervr_ir_shader *shader,
	const struct shader_uniform *uniform,
	const struct powervr_ir_operand *operand)
{
	/* A plain constant. */
	if (operand->modifiers != 0)
		return 0;
	if (operand->kind == POWERVR_IR_OPERAND_CONSTANT)
		return 1;

	/* A uniform value's only word. */
	if (operand->kind != POWERVR_IR_OPERAND_VALUE || operand->component != 0 || operand->value >= shader->value_count)
		return 0;
	return uniform->kind[operand->value] != SHADER_UNIFORM_VARYING;
}

/* Reports how uniform an instruction's result is (SHADER_UNIFORM_*): a loaded shared word, a copy of one or of a constant, or arithmetic of uniform values. */
static unsigned
uniform_kind(
	const struct powervr_ir_shader *shader,
	const struct powervr_shader_program *program,
	const struct shader_uniform *uniform,
	const struct powervr_ir_instruction *instruction)
{
	const struct powervr_ir_operand *operand;
	unsigned kind;
	unsigned source;

	/* A shared word the data sequencer loads for the whole draw. */
	if (instruction->opcode == POWERVR_IR_SHARED) {
		if (instruction->immediate >= program->request_count)
			return SHADER_UNIFORM_VARYING;
		kind = program->requests[instruction->immediate].kind;
		if (kind == POWERVR_SHADER_REQUEST_WORKGROUP_ID || kind == POWERVR_SHADER_REQUEST_COMPUTED)
			return SHADER_UNIFORM_VARYING;
		return SHADER_UNIFORM_FREE;
	}

	/* Only arithmetic and copies, which have no other effect. */
	if (instruction->opcode < POWERVR_IR_MOVE || instruction->opcode > POWERVR_IR_UNSIGNED_TO_FLOAT)
		return SHADER_UNIFORM_VARYING;

	/* Every source a constant or the word of a uniform value. */
	for (source = 0; source < 4; source++) {
		operand = &instruction->source[source];
		if (operand->kind == POWERVR_IR_OPERAND_NONE || operand->kind == POWERVR_IR_OPERAND_CONSTANT)
			continue;
		if (operand->kind != POWERVR_IR_OPERAND_VALUE ||
		    operand->value >= shader->value_count ||
		    operand->component != 0)
			return SHADER_UNIFORM_VARYING;
		if (uniform->kind[operand->value] == SHADER_UNIFORM_VARYING)
			return SHADER_UNIFORM_VARYING;
	}

	/* A plain copy of a constant or of a loaded word costs the program what reading the word would. */
	operand = &instruction->source[0];
	if (instruction->opcode == POWERVR_IR_MOVE && instruction->flags == 0 && operand->modifiers == 0) {
		if (operand->kind == POWERVR_IR_OPERAND_CONSTANT)
			return SHADER_UNIFORM_FREE;
		if (uniform->kind[operand->value] == SHADER_UNIFORM_FREE)
			return SHADER_UNIFORM_FREE;
	}

	/* Succeeded: work the shared program can do. */
	return SHADER_UNIFORM_COMPUTED;
}

/* Reports whether an instruction packs a colour of 8-bit channels only from constants and uniform values. */
static int
uniform_colour(
	const struct powervr_ir_shader *shader,
	const struct shader_uniform *uniform,
	const struct powervr_ir_instruction *instruction)
{
	const struct powervr_ir_operand *operand;
	unsigned source;

	/* An 8-bit pack into one of the pixel outputs. */
	if (instruction->opcode != POWERVR_IR_PIXEL_STORE ||
	    instruction->immediate_2 != 0 ||
	    instruction->immediate >= 4)
		return 0;

	/* Each channel a constant or a uniform value's word. */
	for (source = 0; source < 4; source++) {
		operand = &instruction->source[source];
		if (operand->kind == POWERVR_IR_OPERAND_CONSTANT)
			continue;
		if (operand->kind != POWERVR_IR_OPERAND_VALUE || operand->value >= shader->value_count)
			return 0;
		if (uniform->kind[operand->value] == SHADER_UNIFORM_VARYING)
			return 0;
	}

	/* Succeeded: the shared program can pack it. */
	return 1;
}

/*
 * Names the shared register of a literal request for a word, adding one
 * while the table keeps room for the computed request; ENOSPC otherwise.
 */
static int
shared_literal(
	struct powervr_shader_program *program,
	uint32_t bits,
	uint32_t *reg)
{
	struct powervr_shader_request *request;
	unsigned at;

	/* The request for the same word. */
	for (at = 0; at < program->request_count; at++) {
		request = &program->requests[at];
		if (request->kind == POWERVR_SHADER_REQUEST_LITERAL && request->words == 1 && request->literal == bits) {
			*reg = request->shared;
			return 0;
		}
	}

	/* A new one after the others. */
	if (program->request_count + 1U >= POWERVR_SHADER_REQUESTS)
		return ENOSPC;
	request = &program->requests[program->request_count++];
	request->kind = POWERVR_SHADER_REQUEST_LITERAL;
	request->part = 0;
	request->words = 1;
	request->shared = (uint16_t)program->shareds;
	request->set = 0;
	request->binding = 0;
	request->array_index = 0;
	request->offset = 0;
	request->literal = bits;
	program->shareds++;

	/* Succeeded: the literal's shared register. */
	*reg = request->shared;
	return 0;
}

/*
 * Builds the shared program's intermediate form: the program's uniform
 * instructions in order, each word's store into its shared register from
 * base on (a colour's pack first), and none of the work no store needs.
 */
static int
shared_shader_build(
	const struct powervr_ir_shader *shader,
	struct shader_uniform *uniform,
	struct powervr_ir_shader *shared,
	unsigned base)
{
	const struct powervr_ir_instruction *instruction;
	struct powervr_ir_instruction *added;
	unsigned value;
	unsigned at;
	unsigned source;
	unsigned colour;

	/* The program's values, and no instruction yet. */
	shared->instruction_count = 0;
	shared->value_count = shader->value_count;
	shared->exact = shader->exact;
	for (value = 0; value < shader->value_count; value++)
		shared->values[value] = shader->values[value];

	/* The uniform instructions, as they are. */
	for (at = 0; at < shader->instruction_count; at++) {
		if (uniform->moved[at] == 0)
			continue;
		if (shared->instruction_count >= POWERVR_IR_INSTRUCTIONS)
			return ENOSPC;
		shared->instructions[shared->instruction_count++] = shader->instructions[at];
	}

	/* Each computed value's store. */
	for (value = 1; value < shader->value_count; value++) {
		if (uniform->slot[value] == 0)
			continue;
		if (shared->instruction_count >= POWERVR_IR_INSTRUCTIONS)
			return ENOSPC;
		added = &shared->instructions[shared->instruction_count++];
		shared_store(added, value, base + uniform->slot[value] - 1U);
	}

	/* Each colour's pack into a value of its own and its store. */
	for (at = 0; at < shader->instruction_count; at++) {
		if (uniform->pack_slot[at] == 0)
			continue;
		instruction = &shader->instructions[at];
		if (shared->instruction_count + 2U > POWERVR_IR_INSTRUCTIONS || shared->value_count >= POWERVR_IR_VALUES)
			return ENOSPC;
		colour = shared->value_count++;
		shared->values[colour].width = 1;
		added = &shared->instructions[shared->instruction_count++];
		*added = *instruction;
		added->opcode = POWERVR_IR_PACK;
		added->flags = 0;
		added->destination = (uint16_t)colour;
		added->immediate = 0;
		added->immediate_2 = 0;
		for (source = 0; source < 4; source++) {
			if (added->source[source].kind == POWERVR_IR_OPERAND_VALUE)
				added->source[source].component = 0;
		}
		added = &shared->instructions[shared->instruction_count++];
		shared_store(added, colour, base + uniform->pack_slot[at] - 1U);
	}

	/* The work no store needs goes. */
	drop_unread(shared, uniform, NULL);

	/* Succeeded: the shared program's form. */
	return 0;
}

/*
 * Drops, last first, the instructions that write a value no kept
 * instruction reads (remove_dead_writes takes a pass a link, too
 * few for a chain of uniform work): of the candidates only, or, with
 * none given, of every instruction that writes a value (the shared
 * program, which has no if or loop and writes each value once).
 */
static void
drop_unread(
	struct powervr_ir_shader *shader,
	struct shader_uniform *uniform,
	const uint8_t *candidates)
{
	const struct powervr_ir_instruction *instruction;
	unsigned value;
	unsigned at;
	unsigned kept;
	unsigned source;

	/* No value read yet. */
	for (value = 0; value < shader->value_count; value++)
		uniform->needed[value] = 0;

	/* From the last instruction: a candidate whose value nothing kept reads goes; what a kept one reads is needed. */
	for (at = shader->instruction_count; at > 0; at--) {
		instruction = &shader->instructions[at - 1U];
		value = instruction->destination;
		uniform->kept[at - 1U] = 1;
		if (instruction->opcode >= POWERVR_IR_MOVE &&
		    value != 0 &&
		    value < shader->value_count &&
		    (candidates == NULL || candidates[at - 1U] != 0) &&
		    uniform->needed[value] == 0) {
			uniform->kept[at - 1U] = 0;
			continue;
		}

		/* What it reads is needed. */
		for (source = 0; source < 4; source++) {
			if (instruction->source[source].kind == POWERVR_IR_OPERAND_VALUE && instruction->source[source].value < shader->value_count)
				uniform->needed[instruction->source[source].value] = 1;
		}
	}

	/* The kept ones close up. */
	kept = 0;
	for (at = 0; at < shader->instruction_count; at++) {
		if (uniform->kept[at] == 0)
			continue;
		if (kept != at)
			shader->instructions[kept] = shader->instructions[at];
		kept++;
	}
	shader->instruction_count = kept;
}

/* Fills an instruction that stores a value's word into a shared register. */
static void
shared_store(
	struct powervr_ir_instruction *instruction,
	unsigned value,
	unsigned reg)
{
	unsigned source;

	/* Source 0 the value, the others none. */
	instruction->opcode = POWERVR_IR_SHARED_STORE;
	instruction->flags = 0;
	instruction->destination = 0;
	for (source = 0; source < 4; source++) {
		instruction->source[source].kind = POWERVR_IR_OPERAND_NONE;
		instruction->source[source].modifiers = 0;
		instruction->source[source].component = 0;
		instruction->source[source].value = 0;
		instruction->source[source].constant = 0;
	}
	instruction->source[0].kind = POWERVR_IR_OPERAND_VALUE;
	instruction->source[0].value = (uint16_t)value;
	instruction->immediate = reg;
	instruction->immediate_2 = 0;
}

/*
 * Reports whether a fragment program can end on its last operation: no
 * branches (so every instance runs it), a move or an immediate run on
 * the enabled instances.
 */
static int
end_on_last(
	const struct powervr_shader_program *program,
	const struct shader_backend *backend)
{
	const struct shader_machine_operation *last;

	/* A fragment program without branches, with an operation. */
	if (program->stage != POWERVR_SHADER_FRAGMENT || backend->control_flow != 0 || backend->operation_count == 0)
		return 0;

	/* A move, an immediate or a sum of products, on the enabled instances. */
	last = &backend->operations[backend->operation_count - 1U];
	if (last->kind != MACHINE_MOVE &&
	    last->kind != MACHINE_IMMEDIATE &&
	    last->kind != MACHINE_SUM_OF_PRODUCTS)
		return 0;
	if (last->condition != POWERVR_USC_RUN_ENABLED)
		return 0;

	/* Succeeded: it can end the program. */
	return 1;
}

/*
 * Reads a plain copy's source in place of the copy (a sample's
 * words and an iterated varying were copied into the shader's variables
 * before every use, a move a word a pixel): after a move of a value's
 * word, or of a constant the core holds in a register, into a one-word
 * value, the reads of that value up to the next structure instruction
 * read the source instead, until either is written again.  The move
 * itself goes when nothing reads it any more (remove_dead_writes).
 */
static void
propagate_copies(
	struct powervr_ir_shader *shader)
{
	struct powervr_ir_instruction *instruction;
	struct powervr_ir_instruction *later;
	struct powervr_ir_operand copied;
	struct powervr_ir_operand *operand;
	unsigned at;
	unsigned next;
	unsigned source;
	unsigned value;
	uint8_t modifiers;

	/* Each plain move into a one-word value. */
	for (at = 0; at < shader->instruction_count; at++) {
		instruction = &shader->instructions[at];
		value = instruction->destination;
		if (instruction->opcode != POWERVR_IR_MOVE || instruction->flags != 0 || instruction->source[0].modifiers != 0)
			continue;
		if (value == 0 || value >= shader->value_count || shader->values[value].width != 1)
			continue;
		copied = instruction->source[0];
		if (copied.kind == POWERVR_IR_OPERAND_VALUE && copied.value == value)
			continue;
		if (copied.kind == POWERVR_IR_OPERAND_CONSTANT && !free_constant(copied.constant))
			continue;
		if (copied.kind != POWERVR_IR_OPERAND_VALUE && copied.kind != POWERVR_IR_OPERAND_CONSTANT)
			continue;

		/* The reads after it, up to a structure instruction or a write of either. */
		for (next = at + 1U; next < shader->instruction_count; next++) {
			later = &shader->instructions[next];
			if (later->opcode < POWERVR_IR_MOVE)
				break;
			for (source = 0; source < 4; source++) {
				operand = &later->source[source];
				if (operand->kind != POWERVR_IR_OPERAND_VALUE || operand->value != value)
					continue;
				/* A constant takes no modifiers in the back end, so a modified read keeps the copy. */
				modifiers = operand->modifiers;
				if (copied.kind == POWERVR_IR_OPERAND_CONSTANT && modifiers != 0)
					continue;
				*operand = copied;
				operand->modifiers = modifiers;
			}
			if (later->destination == value || (copied.kind == POWERVR_IR_OPERAND_VALUE && later->destination == copied.value))
				break;
		}
	}
}

/*
 * In a fragment shader that allows contraction, makes an add of a product
 * one multiply-add (Mesa's NIR fuses them the same way): a float
 * add whose one source is (or is minus) a one-word value written once by
 * an unsaturated multiply, read only there, in the same run of
 * instructions with neither of the multiply's sources written in
 * between.  The multiply goes when nothing reads it (remove_dead_writes).
 */
static void
fuse_multiply_adds(
	struct powervr_ir_shader *shader,
	uint32_t stage)
{
	unsigned at;

	/* Fragment shaders without a NoContraction result only. */
	if (stage != POWERVR_SHADER_FRAGMENT || shader->exact != 0)
		return;

	/*
	 * Each clamp to [0, 1] as a saturate, its inner operation then read by
	 * nothing gone (else its read keeps the saturate from the operation
	 * that computes the clamped value), then each add, by its first
	 * source, else its second.
	 */
	for (at = 0; at < shader->instruction_count; at++)
		(void)fuse_clamp(shader, at);
	remove_dead_writes(shader);
	for (at = 0; at < shader->instruction_count; at++) {
		if (shader->instructions[at].opcode != POWERVR_IR_FLOAT_ADD)
			continue;
		if (!fuse_multiply_add(shader, at, 0))
			(void)fuse_multiply_add(shader, at, 1);
	}

	/* Then each saturate of a lone result, by the operation that computes it. */
	for (at = 0; at < shader->instruction_count; at++)
		(void)fuse_saturate(shader, at);
}

/*
 * Makes the saturated add of zero of a float add, multiply or
 * multiply-add's result that nothing else reads a plain copy, and has
 * that operation saturate instead (the blend's clamp of a
 * computed colour was an operation a channel), when it is written once,
 * in the same run of instructions with its sources unchanged in between.
 * The copy goes when copies are read in place again.
 */
static int
fuse_saturate(
	struct powervr_ir_shader *shader,
	unsigned at)
{
	struct powervr_ir_instruction *add;
	struct powervr_ir_instruction *producer;
	const struct powervr_ir_instruction *between;
	const struct powervr_ir_operand *term;
	unsigned writes;
	unsigned writer;
	unsigned next;
	unsigned source;

	/* A saturated add of zero to a plain one-word value, written once and read only here. */
	add = &shader->instructions[at];
	if (add->opcode != POWERVR_IR_FLOAT_ADD || add->flags != POWERVR_IR_SATURATE)
		return 0;
	if (add->source[1].kind != POWERVR_IR_OPERAND_CONSTANT || add->source[1].modifiers != 0 || add->source[1].constant != 0)
		return 0;
	term = &add->source[0];
	if (term->kind != POWERVR_IR_OPERAND_VALUE || term->modifiers != 0 || term->value == 0 || term->value >= shader->value_count ||
	    shader->values[term->value].width != 1)
		return 0;
	if (value_references(shader, term->value, &writes, &writer) != 1 || writes != 1 || writer >= at)
		return 0;

	/* By an unsaturated float add, multiply or multiply-add. */
	producer = &shader->instructions[writer];
	if ((producer->opcode != POWERVR_IR_FLOAT_ADD && producer->opcode != POWERVR_IR_FLOAT_MULTIPLY &&
	     producer->opcode != POWERVR_IR_FLOAT_MULTIPLY_ADD) || producer->flags != 0)
		return 0;

	/* No structure between, and the producer's sources unchanged. */
	for (next = writer + 1U; next < at; next++) {
		between = &shader->instructions[next];
		if (between->opcode < POWERVR_IR_MOVE)
			return 0;
		for (source = 0; source < 3; source++) {
			if (producer->source[source].kind == POWERVR_IR_OPERAND_VALUE && between->destination == producer->source[source].value)
				return 0;
		}
	}

	/* The producer saturates; the add copies its result. */
	producer->flags = POWERVR_IR_SATURATE;
	add->opcode = POWERVR_IR_MOVE;
	add->flags = 0;
	add->source[1].kind = POWERVR_IR_OPERAND_NONE;
	return 1;
}

/*
 * Makes min(max(x, 0), 1) or max(min(x, 1), 0) one saturated add of
 * zero (GLSL's clamp to [0, 1] was two operations a pixel), when
 * the inner result is a one-word value written once, read only there, in
 * the same run of instructions with x unchanged in between.  The inner
 * one goes when nothing reads it (remove_dead_writes).
 */
static int
fuse_clamp(
	struct powervr_ir_shader *shader,
	unsigned at)
{
	struct powervr_ir_instruction *outer;
	const struct powervr_ir_instruction *inner;
	const struct powervr_ir_instruction *between;
	const struct powervr_ir_operand *term;
	struct powervr_ir_operand x;
	unsigned inner_opcode;
	uint32_t inner_bound;
	unsigned which;
	unsigned other;
	unsigned writes;
	unsigned writer;
	unsigned next;

	/* A minimum with 1.0 of a maximum with 0, or a maximum with 0 of a minimum with 1.0. */
	outer = &shader->instructions[at];
	if (clamp_bound(outer, POWERVR_IR_FLOAT_MINIMUM, 0x3f800000U, &which)) {
		inner_opcode = POWERVR_IR_FLOAT_MAXIMUM;
		inner_bound = 0;
	} else if (clamp_bound(outer, POWERVR_IR_FLOAT_MAXIMUM, 0, &which)) {
		inner_opcode = POWERVR_IR_FLOAT_MINIMUM;
		inner_bound = 0x3f800000U;
	} else {
		return 0;
	}

	/* The other source: a plain one-word value written once and read only here. */
	term = &outer->source[which];
	if (term->kind != POWERVR_IR_OPERAND_VALUE || term->modifiers != 0 || term->value == 0 || term->value >= shader->value_count ||
	    shader->values[term->value].width != 1)
		return 0;
	if (value_references(shader, term->value, &writes, &writer) != 1 || writes != 1 || writer >= at)
		return 0;
	inner = &shader->instructions[writer];
	if (!clamp_bound(inner, inner_opcode, inner_bound, &other))
		return 0;
	x = inner->source[other];

	/* No structure between, and x unchanged. */
	for (next = writer + 1U; next < at; next++) {
		between = &shader->instructions[next];
		if (between->opcode < POWERVR_IR_MOVE)
			return 0;
		if (x.kind == POWERVR_IR_OPERAND_VALUE && between->destination == x.value)
			return 0;
	}

	/* x + 0, saturated. */
	outer->opcode = POWERVR_IR_FLOAT_ADD;
	outer->flags = POWERVR_IR_SATURATE;
	outer->source[0] = x;
	outer->source[1].kind = POWERVR_IR_OPERAND_CONSTANT;
	outer->source[1].modifiers = 0;
	outer->source[1].component = 0;
	outer->source[1].value = 0;
	outer->source[1].constant = 0;
	outer->source[2].kind = POWERVR_IR_OPERAND_NONE;
	outer->source[3].kind = POWERVR_IR_OPERAND_NONE;
	return 1;
}

/* Reports whether an instruction is the opcode with one source the plain constant bound, and which source is the other. */
static int
clamp_bound(
	const struct powervr_ir_instruction *instruction,
	unsigned opcode,
	uint32_t bound,
	unsigned *other)
{
	unsigned source;

	/* The opcode, unsaturated. */
	if (instruction->opcode != opcode || instruction->flags != 0)
		return 0;

	/* The bound in either source. */
	for (source = 0; source < 2; source++) {
		if (instruction->source[source].kind == POWERVR_IR_OPERAND_CONSTANT && instruction->source[source].modifiers == 0 &&
		    instruction->source[source].constant == bound) {
			*other = 1U - source;
			return 1;
		}
	}
	return 0;
}

/* Fuses the product an add's source (which) reads into it; reports whether it did. */
static int
fuse_multiply_add(
	struct powervr_ir_shader *shader,
	unsigned at,
	unsigned which)
{
	struct powervr_ir_instruction *add;
	const struct powervr_ir_instruction *product;
	const struct powervr_ir_instruction *between;
	struct powervr_ir_operand a;
	struct powervr_ir_operand b;
	struct powervr_ir_operand c;
	const struct powervr_ir_operand *term;
	unsigned writes;
	unsigned writer;
	unsigned next;

	/* A plain or negated one-word value, written once and read once. */
	add = &shader->instructions[at];
	term = &add->source[which];
	if (term->kind != POWERVR_IR_OPERAND_VALUE || (term->modifiers & ~POWERVR_IR_NEGATE) != 0)
		return 0;
	if (term->value == 0 || term->value >= shader->value_count || shader->values[term->value].width != 1)
		return 0;
	if (value_references(shader, term->value, &writes, &writer) != 1 || writes != 1 || writer >= at)
		return 0;

	/* By an unsaturated multiply. */
	product = &shader->instructions[writer];
	if (product->opcode != POWERVR_IR_FLOAT_MULTIPLY || product->flags != 0)
		return 0;
	a = product->source[0];
	b = product->source[1];

	/* No structure between, and the multiply's sources unchanged. */
	for (next = writer + 1U; next < at; next++) {
		between = &shader->instructions[next];
		if (between->opcode < POWERVR_IR_MOVE)
			return 0;
		if ((a.kind == POWERVR_IR_OPERAND_VALUE && between->destination == a.value) ||
		    (b.kind == POWERVR_IR_OPERAND_VALUE && between->destination == b.value))
			return 0;
	}

	/* The multiply-add: minus the product negates its first factor; the add's saturate stays. */
	if ((term->modifiers & POWERVR_IR_NEGATE) != 0) {
		if (a.kind == POWERVR_IR_OPERAND_CONSTANT) {
			a.constant ^= 0x80000000U;
		} else {
			a.modifiers ^= POWERVR_IR_NEGATE;
		}
	}
	c = add->source[which == 0 ? 1U : 0U];
	add->opcode = POWERVR_IR_FLOAT_MULTIPLY_ADD;
	add->source[0] = a;
	add->source[1] = b;
	add->source[2] = c;
	add->source[3].kind = POWERVR_IR_OPERAND_NONE;
	return 1;
}

/* Counts the reads of a value over the shader, and its writes and the last writer. */
static unsigned
value_references(
	const struct powervr_ir_shader *shader,
	unsigned value,
	unsigned *writes,
	unsigned *writer)
{
	const struct powervr_ir_instruction *instruction;
	unsigned reads;
	unsigned at;
	unsigned source;

	/* Every instruction's sources and destination. */
	reads = 0;
	*writes = 0;
	*writer = 0;
	for (at = 0; at < shader->instruction_count; at++) {
		instruction = &shader->instructions[at];
		for (source = 0; source < 4; source++) {
			if (instruction->source[source].kind == POWERVR_IR_OPERAND_VALUE && instruction->source[source].value == value)
				reads++;
		}
		if (instruction->opcode >= POWERVR_IR_MOVE && instruction->destination == value) {
			(*writes)++;
			*writer = at;
		}
	}
	return reads;
}

/*
 * Finds the one-word values that are a shared register's word, written
 * only by that read and read only by float adds, multiplies and
 * multiply-adds, whose lower sources name a shared register as well as
 * a temporary (as Mesa's pco reads sh registers in place, where
 * each read was a move a pixel).  Those operations read the shared
 * register, and the read itself selects nothing.
 */
static void
alias_shared(
	const struct powervr_ir_shader *shader,
	const struct powervr_shader_program *program,
	struct shader_backend *backend)
{
	const struct powervr_ir_instruction *instruction;
	const struct powervr_ir_instruction *reader;
	const struct powervr_shader_request *request;
	unsigned value;
	unsigned writes;
	unsigned writer;
	unsigned at;
	unsigned source;
	int ok;

	/* None yet; none at all when the words are given up front. */
	for (value = 0; value < POWERVR_IR_VALUES; value++)
		backend->shared_alias[value] = 0;
	if (backend->inline_values != NULL)
		return;

	/* Each shared word read into a one-word value written nowhere else. */
	for (at = 0; at < shader->instruction_count; at++) {
		instruction = &shader->instructions[at];
		value = instruction->destination;
		if (instruction->opcode != POWERVR_IR_SHARED || value == 0 || value >= shader->value_count || shader->values[value].width != 1)
			continue;
		if (instruction->immediate >= program->request_count)
			continue;
		request = &program->requests[instruction->immediate];
		if (instruction->immediate_2 >= request->words || request->kind == POWERVR_SHADER_REQUEST_WORKGROUP_ID)
			continue;
		(void)value_references(shader, value, &writes, &writer);
		if (writes != 1)
			continue;

		/* Read only by float adds, multiplies and multiply-adds. */
		ok = 1;
		for (writer = 0; ok && writer < shader->instruction_count; writer++) {
			reader = &shader->instructions[writer];
			for (source = 0; source < 4; source++) {
				if (reader->source[source].kind != POWERVR_IR_OPERAND_VALUE || reader->source[source].value != value)
					continue;
				if (reader->opcode != POWERVR_IR_FLOAT_ADD && reader->opcode != POWERVR_IR_FLOAT_MULTIPLY && reader->opcode != POWERVR_IR_FLOAT_MULTIPLY_ADD)
					ok = 0;
			}
		}
		if (ok)
			backend->shared_alias[value] = (uint16_t)(request->shared + instruction->immediate_2 + 1U);
	}
}

/*
 * Finds the flat varyings, each a one-word value a flat iteration writes
 * once, read only by float adds, multiplies and multiply-adds: those
 * read the varying's C coefficient register in place, as any source of
 * theirs can name a coefficient register, and the iteration selects
 * nothing (a move into a temporary was an operation a word a pixel).
 */
/* The highest coefficient register a source of any operand form names (the narrowest index field has 8 bits). */
#define COEFFICIENT_ALIAS_MOST	255U

static void
alias_coefficients(
	const struct powervr_ir_shader *shader,
	const struct powervr_shader_program *program,
	struct shader_backend *backend)
{
	const struct powervr_ir_instruction *instruction;
	const struct powervr_ir_instruction *reader;
	unsigned value;
	unsigned writes;
	unsigned writer;
	unsigned at;
	unsigned source;
	int ok;

	/* None yet; only a fragment program's varyings. */
	for (value = 0; value < POWERVR_IR_VALUES; value++)
		backend->coefficient_alias[value] = 0;
	if (program->stage != POWERVR_SHADER_FRAGMENT)
		return;

	/* Each flat iteration into a one-word value written nowhere else. */
	for (at = 0; at < shader->instruction_count; at++) {
		instruction = &shader->instructions[at];
		value = instruction->destination;
		if (instruction->opcode != POWERVR_IR_ITERATE || (instruction->flags & POWERVR_IR_FLAT) == 0 ||
		    value == 0 || value >= shader->value_count || shader->values[value].width != 1 ||
		    instruction->immediate + 2U > COEFFICIENT_ALIAS_MOST)
			continue;
		(void)value_references(shader, value, &writes, &writer);
		if (writes != 1)
			continue;

		/* Read only by float adds, multiplies and multiply-adds. */
		ok = 1;
		for (writer = 0; ok && writer < shader->instruction_count; writer++) {
			reader = &shader->instructions[writer];
			for (source = 0; source < 4; source++) {
				if (reader->source[source].kind != POWERVR_IR_OPERAND_VALUE || reader->source[source].value != value)
					continue;
				if (reader->opcode != POWERVR_IR_FLOAT_ADD && reader->opcode != POWERVR_IR_FLOAT_MULTIPLY && reader->opcode != POWERVR_IR_FLOAT_MULTIPLY_ADD)
					ok = 0;
			}
		}
		if (ok)
			backend->coefficient_alias[value] = (uint16_t)(instruction->immediate + 2U + 1U);
	}
}

/* The most constants hoist_addends puts in values. */
#define HOISTED_MAX			4U

/*
 * In a fragment shader without structure, gives a constant that two or
 * more multiply-adds add a value of its own, written once at the start
 * (a constant addend is a shared register, which the third source
 * of a group's second phase cannot name, so such multiply-adds did not
 * pair; a temporary can).  The core's free constants stay as they are.
 */
static void
hoist_addends(
	struct powervr_ir_shader *shader,
	uint32_t stage)
{
	struct powervr_ir_instruction *instruction;
	uint32_t constants[HOISTED_MAX];
	unsigned values[HOISTED_MAX];
	unsigned count;
	unsigned uses;
	unsigned at;
	unsigned other;
	unsigned index;
	unsigned known;
	uint32_t bits;

	/* Fragment shaders without structure, with room for the moves. */
	if (stage != POWERVR_SHADER_FRAGMENT)
		return;
	for (at = 0; at < shader->instruction_count; at++) {
		if (shader->instructions[at].opcode < POWERVR_IR_MOVE)
			return;
	}

	/* The constants added by two or more multiply-adds. */
	count = 0;
	for (at = 0; at < shader->instruction_count && count < HOISTED_MAX; at++) {
		instruction = &shader->instructions[at];
		if (instruction->opcode != POWERVR_IR_FLOAT_MULTIPLY_ADD || instruction->source[2].kind != POWERVR_IR_OPERAND_CONSTANT)
			continue;
		bits = instruction->source[2].constant;
		if (free_constant(bits) || instruction->source[2].modifiers != 0)
			continue;
		known = 0;
		for (index = 0; index < count; index++) {
			if (constants[index] == bits)
				known = 1;
		}
		if (known)
			continue;
		uses = 0;
		for (other = at; other < shader->instruction_count; other++) {
			if (shader->instructions[other].opcode == POWERVR_IR_FLOAT_MULTIPLY_ADD &&
			    shader->instructions[other].source[2].kind == POWERVR_IR_OPERAND_CONSTANT &&
			    shader->instructions[other].source[2].modifiers == 0 && shader->instructions[other].source[2].constant == bits)
				uses++;
		}
		if (uses < 2 || shader->value_count >= POWERVR_IR_VALUES || shader->instruction_count + count + 1U > POWERVR_IR_INSTRUCTIONS)
			continue;
		constants[count] = bits;
		values[count] = shader->value_count;
		shader->values[shader->value_count].width = 1;
		shader->value_count++;
		count++;
	}
	if (count == 0)
		return;

	/* The addends read the values. */
	for (at = 0; at < shader->instruction_count; at++) {
		instruction = &shader->instructions[at];
		if (instruction->opcode != POWERVR_IR_FLOAT_MULTIPLY_ADD || instruction->source[2].kind != POWERVR_IR_OPERAND_CONSTANT ||
		    instruction->source[2].modifiers != 0)
			continue;
		for (index = 0; index < count; index++) {
			if (instruction->source[2].constant == constants[index]) {
				instruction->source[2].kind = POWERVR_IR_OPERAND_VALUE;
				instruction->source[2].value = (uint16_t)values[index];
				instruction->source[2].component = 0;
				instruction->source[2].constant = 0;
			}
		}
	}

	/* The moves that write them, first. */
	for (at = shader->instruction_count; at > 0; at--)
		shader->instructions[at - 1U + count] = shader->instructions[at - 1U];
	for (index = 0; index < count; index++) {
		instruction = &shader->instructions[index];
		*instruction = (struct powervr_ir_instruction){ 0 };
		instruction->opcode = POWERVR_IR_MOVE;
		instruction->destination = (uint16_t)values[index];
		instruction->source[0].kind = POWERVR_IR_OPERAND_CONSTANT;
		instruction->source[0].constant = constants[index];
	}
	shader->instruction_count += count;
}

/*
 * Makes each unsaturated float multiply by the constant 1.0 a copy of
 * its other source (exact; a division of 1.0 was a reciprocal and
 * a multiply by 1.0).  The copy goes when copies are read in place.
 */
static void
simplify_products(
	struct powervr_ir_shader *shader)
{
	struct powervr_ir_instruction *instruction;
	unsigned at;
	unsigned one;

	/* Each multiply with a plain 1.0 source. */
	for (at = 0; at < shader->instruction_count; at++) {
		instruction = &shader->instructions[at];
		if (instruction->opcode != POWERVR_IR_FLOAT_MULTIPLY || instruction->flags != 0)
			continue;
		for (one = 0; one < 2U; one++) {
			if (instruction->source[one].kind == POWERVR_IR_OPERAND_CONSTANT && instruction->source[one].modifiers == 0 &&
			    instruction->source[one].constant == 0x3f800000U)
				break;
		}
		if (one == 2U)
			continue;

		/* A copy of the other source, its modifiers kept. */
		instruction->opcode = POWERVR_IR_MOVE;
		instruction->source[0] = instruction->source[1U - one];
		instruction->source[1].kind = POWERVR_IR_OPERAND_NONE;
	}
}

/* Reports whether a constant is one the core holds in a register (zero, one, 1.0 and all ones), so reading it costs nothing. */
static int
free_constant(
	uint32_t bits)
{
	/* The constants operand_register names without an immediate. */
	return bits == 0 || bits == 1U || bits == 0x3f800000U || bits == 0xffffffffU;
}

/* An operation fold_moves or merge_iterates folded away, until the operations close up. */
#define MACHINE_FOLDED			0xffU

/* The most words one iterate takes (a vector of four). */
#define ITERATE_WORDS_MAX		4U

/*
 * In a fragment program without branches, iterates one set of words in
 * one operation where the reader iterated a word at a time, each with
 * its wait (Mesa's pco iterates a vector in one fitr): an iterate
 * and its wait followed by the iterate of the next coefficient set into
 * the next register become one iterate of both, the second's wait kept.
 */
static void
merge_iterates(
	const struct powervr_shader_program *program,
	struct shader_backend *backend)
{
	struct shader_machine_operation *first;
	const struct shader_machine_operation *wait;
	const struct shader_machine_operation *next;
	unsigned words;
	unsigned at;
	unsigned last;
	unsigned kept;

	/* Fragment programs without branches only (no branch targets to keep). */
	if (program->stage != POWERVR_SHADER_FRAGMENT || backend->control_flow != 0)
		return;

	/* Each iterate takes the iterates that follow it (each with its wait after it), marking them and the waits between folded. */
	for (at = 0; at < backend->operation_count; at++) {
		first = &backend->operations[at];
		if (first->kind != MACHINE_ITERATE || first->destination[0].bank != POWERVR_USC_BANK_TEMPORARY || first->condition != POWERVR_USC_RUN_ENABLED)
			continue;
		last = at;
		while (last + 3U < backend->operation_count) {
			wait = &backend->operations[last + 1U];
			next = &backend->operations[last + 2U];
			words = first->variant + 1U;
			if (wait->kind != MACHINE_WAIT || next->kind != MACHINE_ITERATE || backend->operations[last + 3U].kind != MACHINE_WAIT || words >= ITERATE_WORDS_MAX)
				break;
			if (next->destination[0].bank != POWERVR_USC_BANK_TEMPORARY || next->destination[0].index != first->destination[0].index + words ||
			    temporary_block(next->destination[0].index) != temporary_block(first->destination[0].index) ||
			    next->immediate != first->immediate + 4U * words || next->immediate_2 != first->immediate_2 ||
			    next->variant != 0 || next->condition != first->condition)
				break;
			first->variant++;
			backend->operations[last + 1U].kind = MACHINE_FOLDED;
			backend->operations[last + 2U].kind = MACHINE_FOLDED;
			last += 2U;
		}
		at = last;
	}

	/* The rest close up. */
	kept = 0;
	for (at = 0; at < backend->operation_count; at++) {
		if (backend->operations[at].kind == MACHINE_FOLDED)
			continue;
		if (kept != at)
			backend->operations[kept] = backend->operations[at];
		kept++;
	}
	backend->operation_count = kept;
}

/*
 * In a program without branches, drops an immediate into a register that
 * already holds that value from the last immediate into it, nothing
 * having written it since (the reader names a constant operand by
 * an immediate into the scratch temporary at each use, four in a row
 * for a vector scaled by one constant).
 */
static void
reuse_immediates(
	struct shader_backend *backend)
{
	unsigned at;
	unsigned kept;

	/* Programs without branches only (no paths join). */
	if (backend->control_flow != 0)
		return;

	/* Each repeated immediate is marked folded, then the rest close up. */
	for (at = 0; at < backend->operation_count; at++) {
		if (repeated_immediate(backend, at))
			backend->operations[at].kind = MACHINE_FOLDED;
	}
	kept = 0;
	for (at = 0; at < backend->operation_count; at++) {
		if (backend->operations[at].kind == MACHINE_FOLDED)
			continue;
		if (kept != at)
			backend->operations[kept] = backend->operations[at];
		kept++;
	}
	backend->operation_count = kept;
}

/* Reports whether the immediate at an index writes the value its register holds from an earlier immediate. */
static int
repeated_immediate(
	const struct shader_backend *backend,
	unsigned at)
{
	const struct shader_machine_operation *immediate;
	const struct shader_machine_operation *other;
	unsigned before;

	/* A plain immediate into a temporary. */
	immediate = &backend->operations[at];
	if (immediate->kind != MACHINE_IMMEDIATE || immediate->end != 0 || immediate->condition != POWERVR_USC_RUN_ENABLED ||
	    immediate->destination[0].bank != POWERVR_USC_BANK_TEMPORARY)
		return 0;

	/* Back to the last write of its register: through simple operations, waits and vertex writes that leave it. */
	for (before = at; before > 0; before--) {
		other = &backend->operations[before - 1U];
		if (other->kind == MACHINE_FOLDED || other->kind == MACHINE_WAIT || other->kind == MACHINE_VERTEX_WRITE)
			continue;
		if (!simple_kind(other->kind))
			return 0;
		if (same_register(other->destination[1], immediate->destination[0]))
			return 0;
		if (same_register(other->destination[0], immediate->destination[0]))
			return other->kind == MACHINE_IMMEDIATE && other->immediate == immediate->immediate &&
			       other->condition == POWERVR_USC_RUN_ENABLED;
	}
	return 0;
}

/*
 * In a fragment program without branches, unpacks the colour already in
 * a pixel output register from the register itself under the overlap
 * check, where it was moved into a temporary first (a group a
 * blended pixel).
 */
static void
unpack_in_place(
	const struct powervr_shader_program *program,
	struct shader_backend *backend)
{
	struct shader_machine_operation *move;
	struct shader_machine_operation *unpack;
	unsigned at;
	unsigned next;
	unsigned kept;
	int read;

	/* Fragment programs without branches only (no branch targets to keep). */
	if (program->stage != POWERVR_SHADER_FRAGMENT || backend->control_flow != 0)
		return;

	/* A checked move out of a pixel output register into a temporary only the unpack after it reads. */
	for (at = 0; at + 1U < backend->operation_count; at++) {
		move = &backend->operations[at];
		unpack = &backend->operations[at + 1U];
		if (move->kind != MACHINE_MOVE || move->variant != 1 || move->modifiers[0] != 0 || move->source[0].bank != POWERVR_USC_BANK_SPECIAL ||
		    move->destination[0].bank != POWERVR_USC_BANK_TEMPORARY || move->condition != POWERVR_USC_RUN_ENABLED || move->end != 0)
			continue;
		if (unpack->kind != MACHINE_UNPACK || !same_register(unpack->source[0], move->destination[0]) || unpack->condition != move->condition)
			continue;
		read = 0;
		for (next = at + 2U; next < backend->operation_count; next++) {
			if (reads_register(&backend->operations[next], move->destination[0])) {
				read = 1;
				break;
			}
			if (same_register(backend->operations[next].destination[0], move->destination[0]))
				break;
		}
		if (read)
			continue;
		unpack->source[0] = move->source[0];
		unpack->overlap = 1;
		move->kind = MACHINE_FOLDED;
	}

	/* The rest close up. */
	kept = 0;
	for (at = 0; at < backend->operation_count; at++) {
		if (backend->operations[at].kind == MACHINE_FOLDED)
			continue;
		if (kept != at)
			backend->operations[kept] = backend->operations[at];
		kept++;
	}
	backend->operation_count = kept;
}

/*
 * Marks the varyings a sample iterates itself (the iterate and sample form:
 * BXE-4-32 has the control unit's iterate and sample): the two
 * coordinates of a sample, unmodified, each a one-word value written
 * only by the iteration of a varying word and read only there, the
 * second's coefficient set the one after the first's, with the image
 * and sampler states on multiples of four shared registers.
 */
static void
mark_iterate_samples(
	const struct powervr_ir_shader *shader,
	const struct powervr_shader_program *program,
	struct shader_backend *backend)
{
	const struct powervr_ir_instruction *instruction;
	const struct powervr_ir_instruction *first;
	const struct powervr_ir_instruction *second;
	unsigned value;
	unsigned at;
	int found;

	/* None yet; none at all unless the key takes the form. */
	for (value = 0; value < shader->value_count; value++) {
		backend->iterate_sample[value] = 0;
		backend->sample_half[value] = 0;
	}
	if ((backend->forms & POWERVR_SHADER_FORM_ITERATE_SAMPLE) == 0 || program->stage != POWERVR_SHADER_FRAGMENT)
		return;

	/* Each sample whose coordinates are two iterated words in order. */
	for (at = 0; at < shader->instruction_count; at++) {
		instruction = &shader->instructions[at];
		if (instruction->opcode != POWERVR_IR_SAMPLE)
			continue;
		if (instruction->immediate >= program->request_count || instruction->immediate_2 >= program->request_count)
			continue;
		if ((program->requests[instruction->immediate].shared & 3U) != 0 || (program->requests[instruction->immediate_2].shared & 3U) != 0)
			continue;
		found = lone_iterate(shader, &instruction->source[0], &first);
		if (!found)
			continue;
		found = lone_iterate(shader, &instruction->source[1], &second);
		if (!found || second->immediate != first->immediate + 4U)
			continue;
		backend->iterate_sample[instruction->source[0].value] = 1;
		backend->iterate_sample[instruction->source[1].value] = 1;

		/* Its channels as half floats when one sum of products alone reads them. */
		if ((backend->forms & POWERVR_SHADER_FORM_SUM_OF_PRODUCTS) != 0 &&
		    instruction->destination < shader->value_count &&
		    half_reader(shader, instruction->destination))
			backend->sample_half[instruction->destination] = 1;
	}
}

/*
 * Reports whether a sample's four-word value is read only by one pixel
 * store into an 8-bit attachment or one blend over, as its four words
 * in order without modifiers: the store or blend the half forms of the
 * sum of products do.
 */
static int
half_reader(
	const struct powervr_ir_shader *shader,
	unsigned value)
{
	const struct powervr_ir_instruction *instruction;
	const struct powervr_ir_instruction *reader;
	unsigned at;
	unsigned source;
	unsigned reads;

	/* A four-word value. */
	if (shader->values[value].width != 4)
		return 0;

	/* The one instruction reading it. */
	reader = NULL;
	for (at = 0; at < shader->instruction_count; at++) {
		instruction = &shader->instructions[at];
		reads = 0;
		for (source = 0; source < 4; source++) {
			if (instruction->source[source].kind == POWERVR_IR_OPERAND_VALUE && instruction->source[source].value == value)
				reads++;
		}
		if (reads == 0)
			continue;
		if (reader != NULL)
			return 0;
		reader = instruction;
	}
	if (reader == NULL)
		return 0;

	/* An 8-bit store, or a blend over. */
	if (reader->opcode == POWERVR_IR_PIXEL_STORE) {
		if (reader->immediate_2 != 0 || reader->immediate >= 4)
			return 0;
	} else if (reader->opcode == POWERVR_IR_PIXEL_BLEND) {
		if (reader->immediate_2 != POWERVR_IR_BLEND_OVER || reader->immediate >= 4)
			return 0;
	} else {
		return 0;
	}

	/* Its words in order, as they are. */
	for (source = 0; source < 4; source++) {
		if (reader->source[source].kind != POWERVR_IR_OPERAND_VALUE ||
		    reader->source[source].value != value ||
		    reader->source[source].component != source ||
		    reader->source[source].modifiers != 0)
			return 0;
	}

	/* Succeeded: the half forms can read it. */
	return 1;
}

/* Reports whether a sample's coordinate is a one-word value only one varying word's iteration writes and only it reads, and names that iteration. */
static int
lone_iterate(
	const struct powervr_ir_shader *shader,
	const struct powervr_ir_operand *operand,
	const struct powervr_ir_instruction **iterate)
{
	const struct powervr_ir_instruction *writer_instruction;
	unsigned writes;
	unsigned writer;
	unsigned reads;

	/* A plain read of a one-word value. */
	if (operand->kind != POWERVR_IR_OPERAND_VALUE ||
	    operand->modifiers != 0 ||
	    operand->component != 0 ||
	    operand->value == 0 ||
	    operand->value >= shader->value_count ||
	    shader->values[operand->value].width != 1)
		return 0;

	/* Written once by an iteration, read once. */
	reads = value_references(shader, operand->value, &writes, &writer);
	if (reads != 1 || writes != 1)
		return 0;
	writer_instruction = &shader->instructions[writer];
	if (writer_instruction->opcode != POWERVR_IR_ITERATE || (writer_instruction->flags & POWERVR_IR_FLAT) != 0)
		return 0;

	/* Succeeded: the iteration. */
	*iterate = writer_instruction;
	return 1;
}

/* Names the coefficient set of the iteration that writes a value (mark_iterate_samples found it). */
static uint32_t
iterate_coefficient(
	const struct powervr_ir_shader *shader,
	unsigned value)
{
	unsigned writes;
	unsigned writer;

	/* The value's one writer. */
	(void)value_references(shader, value, &writes, &writer);

	/* Succeeded: its coefficient set. */
	return shader->instructions[writer].immediate;
}

/*
 * Works out the packed 8-bit word of a pixel store whose four channels
 * are the constants 0.0 (or -0.0) and 1.0, red in the lowest byte; other
 * constants are left to the pack (the kernel does no float arithmetic).
 */
static int
pack_constant(
	const struct powervr_ir_instruction *instruction,
	uint32_t *packed)
{
	const struct powervr_ir_operand *operand;
	unsigned channel;

	/* Eight-bit channels only. */
	if (instruction->immediate_2 != 0)
		return 0;

	/* Each channel nought or one. */
	*packed = 0;
	for (channel = 0; channel < 4; channel++) {
		operand = &instruction->source[channel];
		if (operand->kind != POWERVR_IR_OPERAND_CONSTANT || operand->modifiers != 0)
			return 0;
		if (operand->constant == 0x3f800000U) {
			*packed |= 0xffU << (channel * 8U);
		} else if (operand->constant != 0 && operand->constant != 0x80000000U) {
			return 0;
		}
	}
	return 1;
}

/*
 * In a program without branches, puts two independent operations in one
 * group, the first in phase 0 (sources s0 to s2, ft0, w0) and the second
 * in phase 1 (s3 to s5, ft1, w1), marking the first paired with the
 * next (a group's cost on the board is its issue, so a pair costs
 * one).  An operation up to PAIR_WINDOW ahead that commutes with every
 * one between is moved up beside the one it pairs with.  Only pairs whose
 * group encodes are made.
 */
static void
pair_operations(
	struct shader_backend *backend)
{
	struct shader_machine_operation moved;
	struct shader_machine_operation *first;
	struct powervr_usc_group group;
	unsigned at;
	unsigned next;
	unsigned between;
	unsigned found;
	unsigned swap;
	size_t size;

	/* Programs without branches only (moving operations keeps no branch targets). */
	if (backend->control_flow != 0)
		return;

	/* Each operation not yet in a pair looks ahead for one to pair with. */
	for (at = 0; at + 1U < backend->operation_count; at++) {
		first = &backend->operations[at];
		if (!pairable(first) || first->end != 0)
			continue;
		found = backend->operation_count;
		swap = 0;
		for (next = at + 1U; next < backend->operation_count && next <= at + PAIR_WINDOW; next++) {
			/*
			 * One that pairs, commutes with the first and with all
			 * between, and whose pair encodes, the first in phase 0 or,
			 * when that does not encode and neither reads the other's
			 * result, in phase 1 (an addend in a shared register is
			 * named only in phase 0).
			 */
			if (pairable(&backend->operations[next]) && pairs_with(first, &backend->operations[next])) {
				for (between = at + 1U; between < next; between++) {
					if (!commutes(&backend->operations[between], &backend->operations[next]))
						break;
				}
				if (between == next && pair_group(first, &backend->operations[next], &group) == 0 &&
				    powervr_usc_group_size(&group, &size) == 0) {
					found = next;
					break;
				}
				if (between == next && pairs_with(&backend->operations[next], first) &&
				    pair_group(&backend->operations[next], first, &group) == 0 && powervr_usc_group_size(&group, &size) == 0) {
					found = next;
					swap = 1;
					break;
				}
			}

			/* Nothing moves past an operation it cannot be reordered with. */
			if (!simple_kind(backend->operations[next].kind) || backend->operations[next].end != 0)
				break;
		}
		if (found == backend->operation_count)
			continue;

		/* The second moves up beside the first (before it when swapped), and the earlier of the two is marked paired. */
		moved = backend->operations[found];
		for (next = found; next > at + 1U; next--)
			backend->operations[next] = backend->operations[next - 1U];
		backend->operations[at + 1U] = moved;
		if (swap != 0) {
			moved = backend->operations[at];
			backend->operations[at] = backend->operations[at + 1U];
			backend->operations[at + 1U] = moved;
		}
		backend->operations[at].pair = 1;
		at++;
	}
}

/* Reports whether an operation can take either phase of a paired group: a float add, multiply or multiply-add, or a plain move. */
static int
pairable(
	const struct shader_machine_operation *operation)
{
	/* Run on the enabled instances, writing one register. */
	if (operation->condition != POWERVR_USC_RUN_ENABLED || operation->destination[1].bank != 0xff || operation->pair != 0)
		return 0;
	if (operation->kind == MACHINE_FLOAT_ADD || operation->kind == MACHINE_FLOAT_MULTIPLY)
		return 1;
	if (POWERVR_SHADER_PAIR < 2)
		return 0;
	if (operation->kind == MACHINE_FLOAT_MULTIPLY_ADD)
		return 1;
	return operation->kind == MACHINE_MOVE && operation->variant == 0 && operation->destination[0].bank == POWERVR_USC_BANK_TEMPORARY;
}

/*
 * Builds the group of two paired operations: the first's group, with
 * the second's phase 0 operation in phase 1, its sources in the upper
 * set, its result through ft1 to the second write.
 */
static int
pair_group(
	const struct shader_machine_operation *first,
	const struct shader_machine_operation *second,
	struct powervr_usc_group *group)
{
	struct powervr_usc_group other;
	unsigned index;
	int error;

	/* Each operation's own group. */
	error = shader_backend_group(first, 0, group);
	if (error != 0)
		return error;
	error = shader_backend_group(second, 0, &other);
	if (error != 0)
		return error;

	/* Only plain one-phase main groups pair. */
	if (group->unit != POWERVR_USC_UNIT_MAIN || other.unit != POWERVR_USC_UNIT_MAIN || group->repeat != 1 || other.repeat != 1 ||
	    group->overlap_check != 0 || other.overlap_check != 0 || group->mux_used != 0 || other.mux_used != 0 ||
	    group->operation[POWERVR_USC_PHASE_1].length != 0 || other.operation[POWERVR_USC_PHASE_1].length != 0 ||
	    group->operation[POWERVR_USC_PHASE_2].length != 0 || other.operation[POWERVR_USC_PHASE_2].length != 0 ||
	    group->operation[POWERVR_USC_PHASE_BACKEND].length != 0 || other.operation[POWERVR_USC_PHASE_BACKEND].length != 0 ||
	    group->source[3].used || group->source[4].used || group->source[5].used || other.source[3].used || other.source[4].used ||
	    other.source[5].used || group->write_1 != 0 || other.write_1 != 0)
		return EINVAL;

	/* The second's operation in phase 1, its sources in the upper set, its result to the second write. */
	group->operation[POWERVR_USC_PHASE_1] = other.operation[POWERVR_USC_PHASE_0];
	for (index = 0; index < 3U; index++)
		group->source[3U + index] = other.source[index];
	group->destination[1] = other.destination[0];
	group->write_1 = 1;
	group->internal[5] = SELECT_FT1;
	group->end = other.end;

	/* Succeeded: the paired group. */
	return 0;
}

/*
 * Reports whether two simple operations (or a simple one and a pairable
 * one) may swap places: neither reads what the other writes, and they
 * write different registers.
 */
static int
commutes(
	const struct shader_machine_operation *a,
	const struct shader_machine_operation *b)
{
	unsigned index;

	/* Only simple operations, none ending the program or paired, are reordered. */
	if (!simple_kind(a->kind) || !simple_kind(b->kind) || a->end != 0 || b->end != 0 || a->pair != 0 || b->pair != 0)
		return 0;
	if ((a->kind == MACHINE_MOVE && a->variant != 0) || (b->kind == MACHINE_MOVE && b->variant != 0))
		return 0;

	/* No register written by one and read or written by the other. */
	for (index = 0; index < 2U; index++) {
		if (a->destination[index].bank != 0xff && (reads_register(b, a->destination[index]) || writes_register(b, a->destination[index])))
			return 0;
		if (b->destination[index].bank != 0xff && reads_register(a, b->destination[index]))
			return 0;
	}
	return 1;
}

/*
 * Reports whether a second operation may share a group with a first:
 * both read before either writes, so only the second reading what the
 * first writes, or both writing one register, keeps them apart.
 */
static int
pairs_with(
	const struct shader_machine_operation *first,
	const struct shader_machine_operation *second)
{
	/* The second does not read or write the first's result. */
	return !reads_register(second, first->destination[0]) && !writes_register(second, first->destination[0]);
}

/* Reports whether an operation writes a register (its one or two destinations). */
static int
writes_register(
	const struct shader_machine_operation *operation,
	struct shader_machine_register reg)
{
	/* Either destination. */
	return same_register(operation->destination[0], reg) || same_register(operation->destination[1], reg);
}

/* Which block a temporary is in (the back end's own three, or the values'), so a span of them stays together when compacted. */
static unsigned
temporary_block(
	unsigned index)
{
	/* The execution mask counter, the scratch, the coordinates and address, then the values. */
	if (index < TEMP_SCRATCH)
		return 0;
	if (index < TEMP_COORDINATES)
		return 1;
	if (index < TEMP_FIRST_VALUE)
		return 2;
	return 3;
}

/*
 * In a fragment program without branches, has the operation that
 * computes a temporary write it where a later plain move copies it, and
 * drops the move, when nothing else reads the temporary and nothing in
 * between touches either register (the four words of a colour
 * were moved side by side for the pack after being computed, a move a
 * word a pixel).
 */
static void
fold_moves(
	const struct powervr_shader_program *program,
	struct shader_backend *backend,
	unsigned output_base)
{
	unsigned at;
	unsigned kept;

	/* Fragment programs without branches only (no branch targets to keep). */
	if (program->stage != POWERVR_SHADER_FRAGMENT || backend->control_flow != 0)
		return;

	/* Each move that can fold is marked folded, then the rest close up. */
	for (at = 0; at < backend->operation_count; at++) {
		if (fold_move(backend, at, output_base))
			backend->operations[at].kind = MACHINE_FOLDED;
	}
	kept = 0;
	for (at = 0; at < backend->operation_count; at++) {
		if (backend->operations[at].kind == MACHINE_FOLDED)
			continue;
		if (kept != at)
			backend->operations[kept] = backend->operations[at];
		kept++;
	}
	backend->operation_count = kept;
}

/* Folds the move at an index into the operation that computed its source; reports whether it did (output_base: the first colour temporary). */
static int
fold_move(
	struct shader_backend *backend,
	unsigned at,
	unsigned output_base)
{
	struct shader_machine_operation *move;
	struct shader_machine_operation *producer;
	const struct shader_machine_operation *other;
	struct shader_machine_register source;
	struct shader_machine_register destination;
	unsigned found;
	unsigned next;

	/* A plain move between temporaries on the enabled instances. */
	move = &backend->operations[at];
	if (move->kind != MACHINE_MOVE || move->variant != 0 || move->modifiers[0] != 0 || move->end != 0 || move->condition != POWERVR_USC_RUN_ENABLED)
		return 0;
	source = move->source[0];
	destination = move->destination[0];
	if (source.bank != POWERVR_USC_BANK_TEMPORARY || destination.bank != POWERVR_USC_BANK_TEMPORARY || same_register(source, destination))
		return 0;

	/* A colour's temporary (read after the program ends) is never the source. */
	if (source.index >= output_base)
		return 0;

	/* The operation that last wrote the source: a simple one, or an iterate of one word; only simple ones, iterates or waits in between, away from both registers. */
	for (found = at; found > 0; found--) {
		other = &backend->operations[found - 1U];
		if (other->kind == MACHINE_FOLDED)
			continue;
		if (same_register(other->destination[0], source) || same_register(other->destination[1], source))
			break;
		if (!simple_kind(other->kind) && other->kind != MACHINE_WAIT && other->kind != MACHINE_ITERATE)
			return 0;
		if (reads_register(other, source) || reads_register(other, destination) ||
		    same_register(other->destination[0], destination) || same_register(other->destination[1], destination))
			return 0;
	}
	if (found == 0)
		return 0;
	producer = &backend->operations[found - 1U];
	if ((!simple_kind(producer->kind) && producer->kind != MACHINE_ITERATE) || producer->destination[1].bank != 0xff ||
	    producer->condition != POWERVR_USC_RUN_ENABLED || reads_register(producer, destination))
		return 0;

	/* Nothing after the move reads the source before writing it. */
	for (next = at + 1U; next < backend->operation_count; next++) {
		other = &backend->operations[next];
		if (reads_register(other, source))
			return 0;
		if (same_register(other->destination[0], source))
			break;
	}

	/* Succeeded: the producer writes the move's destination. */
	producer->destination[0] = destination;
	return 1;
}

/* Reports whether an operation is a one-result arithmetic, move or immediate, reading only its named sources. */
static int
simple_kind(
	unsigned kind)
{
	/* The kinds of one group, one destination and plain sources. */
	return kind == MACHINE_IMMEDIATE || kind == MACHINE_MOVE || kind == MACHINE_FLOAT_ADD || kind == MACHINE_FLOAT_MULTIPLY ||
	       kind == MACHINE_FLOAT_MULTIPLY_ADD || kind == MACHINE_SINGLE || kind == MACHINE_MINMAX;
}

/* Reports whether an operation may read a register (the pack and the sample read several from their first; unknown kinds count as reading). */
static int
reads_register(
	const struct shader_machine_operation *operation,
	struct shader_machine_register reg)
{
	unsigned source;
	unsigned span;
	unsigned first;

	/* An operation without sources, or one ending the program. */
	if (operation->kind == MACHINE_IMMEDIATE || operation->kind == MACHINE_WAIT || operation->kind == MACHINE_ITERATE || operation->kind == MACHINE_END)
		return 0;

	/* A blending sum of products reads the pixel output it writes. */
	if (operation->kind == MACHINE_SUM_OF_PRODUCTS && operation->variant != POWERVR_USC_SUM_OPAQUE &&
	    operation->variant != POWERVR_USC_SUM_OPAQUE_HALF && same_register(operation->destination[0], reg))
		return 1;

	/* The kinds whose sources are known: each source, a span for the pack and the sample's coordinates. */
	if (!simple_kind(operation->kind) &&
	    operation->kind != MACHINE_PACK &&
	    operation->kind != MACHINE_UNPACK &&
	    operation->kind != MACHINE_SAMPLE &&
	    operation->kind != MACHINE_SUM_OF_PRODUCTS)
		return 1;
	for (source = 0; source < 4; source++) {
		if (operation->source[source].bank != reg.bank)
			continue;
		span = 1;
		if (operation->kind == MACHINE_PACK && source == 0)
			span = operation->immediate;
		if (operation->kind == MACHINE_SAMPLE && source == 1)
			span = 2;
		first = operation->source[source].index;
		if (reg.index >= first && reg.index < first + span)
			return 1;
	}
	return 0;
}

/* Reports whether two machine registers are the same. */
static int
same_register(
	struct shader_machine_register a,
	struct shader_machine_register b)
{
	/* The same bank and number. */
	return a.bank == b.bank && a.index == b.index;
}

/* The most passes of remove_dead_writes (a dead write can make the one before it dead). */
#define DEAD_WRITE_PASSES		4U

/*
 * Drops the writes of one-word values that are written again or never
 * read before the next structure instruction (a fragment colour
 * the reader starts at zero and the shader then writes cost four
 * operations a pixel).  Only operations without side effects go; a
 * write before an if, a loop or their ends stays, as the value may be
 * read on another way through.
 */
static void
remove_dead_writes(
	struct powervr_ir_shader *shader)
{
	unsigned pass;
	unsigned at;
	unsigned kept;
	unsigned removed;

	/* Passes until none goes, each keeping the instructions that are not dead. */
	for (pass = 0; pass < DEAD_WRITE_PASSES; pass++) {
		kept = 0;
		removed = 0;
		for (at = 0; at < shader->instruction_count; at++) {
			if (dead_write(shader, at)) {
				removed++;
				continue;
			}
			if (kept != at)
				shader->instructions[kept] = shader->instructions[at];
			kept++;
		}
		shader->instruction_count = kept;
		if (removed == 0)
			break;
	}
}

/* Reports whether an instruction only writes a one-word value that nothing reads, or that is written again or not read before the next structure instruction. */
static int
dead_write(
	const struct powervr_ir_shader *shader,
	unsigned at)
{
	const struct powervr_ir_instruction *instruction;
	const struct powervr_ir_instruction *later;
	unsigned opcode;
	unsigned value;
	unsigned next;
	unsigned source;
	unsigned writes;
	unsigned writer;

	/* Operations that only compute a one-word value. */
	instruction = &shader->instructions[at];
	opcode = instruction->opcode;
	value = instruction->destination;
	if (opcode < POWERVR_IR_MOVE || opcode == POWERVR_IR_LOAD || opcode == POWERVR_IR_STORE || opcode == POWERVR_IR_OUTPUT ||
	    opcode == POWERVR_IR_SAMPLE || opcode == POWERVR_IR_PIXEL_LOAD || opcode == POWERVR_IR_PIXEL_STORE || opcode == POWERVR_IR_FEEDBACK ||
	    opcode >= POWERVR_IR_OPCODE_COUNT)
		return 0;
	if (value == 0 || value >= shader->value_count || shader->values[value].width != 1)
		return 0;

	/* A value nothing reads anywhere. */
	if (value_references(shader, value, &writes, &writer) == 0)
		return 1;

	/* The instructions after it, up to the next structure instruction. */
	for (next = at + 1U; next < shader->instruction_count; next++) {
		later = &shader->instructions[next];
		if (later->opcode < POWERVR_IR_MOVE)
			return 0;
		for (source = 0; source < 4; source++) {
			if (later->source[source].kind == POWERVR_IR_OPERAND_VALUE && later->source[source].value == value)
				return 0;
		}
		if (later->destination == value)
			return 1;
	}

	/* Never read again. */
	return 1;
}

/*
 * Encodes a shader's machine operations as instruction groups, with
 * every branch's offset worked out from the groups' sizes.
 */
int
shader_backend_encode(
	struct shader_backend *backend,
	uint8_t *code,
	size_t room,
	size_t *length)
{
	struct powervr_usc_group group;
	size_t size;
	size_t total;
	unsigned index;
	int error;

	/* Refuses an empty program. */
	if (backend->operation_count == 0)
		return EINVAL;

	/*
	 * Sizes every group (a paired operation and the next are one); a
	 * branch's size does not depend on its offset.
	 */
	total = 0;
	backend->group_count = 0;
	for (index = 0; index < backend->operation_count; index++) {
		if (backend->operations[index].pair != 0 && index + 1U < backend->operation_count) {
			error = pair_group(&backend->operations[index], &backend->operations[index + 1U], &group);
		} else {
			error = shader_backend_group(&backend->operations[index], 0, &group);
		}
		if (error != 0)
			return error;
		error = powervr_usc_group_size(&group, &size);
		if (error != 0)
			return error;
		backend->group_first[backend->group_count++] = (uint16_t)index;
		backend->offsets[index] = (uint32_t)total;
		if (backend->operations[index].pair != 0 && index + 1U < backend->operation_count) {
			index++;
			backend->offsets[index] = (uint32_t)total;
		}
		total += size;
	}

	/*
	 * The encoder pads the group before the last so the last ends on
	 * the program alignment; the last group's start moves by the pad.
	 */
	if (backend->group_count >= 2 && (total % POWERVR_USC_PROGRAM_ALIGN) != 0)
		backend->offsets[backend->group_first[backend->group_count - 1U]] += (uint32_t)(POWERVR_USC_PROGRAM_ALIGN - total % POWERVR_USC_PROGRAM_ALIGN);

	/* Encodes the groups, the branches with their offsets. */
	error = powervr_usc_program_build(backend->group_count, build_group_fetch, backend, code, room, length);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Works out each value's live span over the instruction list: from its
 * first reference to its last, widened to a whole loop for a value that
 * crosses the loop's iterations.
 */
static int
liveness(
	const struct powervr_ir_shader *shader,
	struct shader_backend *backend)
{
	const struct powervr_ir_instruction *instruction;
	unsigned loop_start[NESTING_MAX];
	unsigned depth;
	unsigned at;
	unsigned value;
	unsigned source;
	unsigned start;
	unsigned inside;
	unsigned outside;
	unsigned scan;
	unsigned first_definition;
	unsigned definitions;
	unsigned first_use;

	/* No value is referenced yet. */
	for (value = 0; value < shader->value_count; value++) {
		backend->first_use[value] = UINT32_MAX;
		backend->last_use[value] = 0;
		backend->uses[value] = 0;
	}

	/* Notes every reference's place. */
	for (at = 0; at < shader->instruction_count; at++) {
		instruction = &shader->instructions[at];
		for (source = 0; source < 4; source++) {
			if (instruction->source[source].kind == POWERVR_IR_OPERAND_VALUE)
				note_reference(backend, instruction->source[source].value, at, 0);
		}
		if (instruction->destination != 0)
			note_reference(backend, instruction->destination, at, 1);
	}

	/*
	 * Widens the spans over loops, innermost first: a value referenced
	 * inside a loop stays live over the whole loop unless all its
	 * references are inside it and it is written once, before any read.
	 */
	depth = 0;
	for (at = 0; at < shader->instruction_count; at++) {
		if (shader->instructions[at].opcode == POWERVR_IR_LOOP) {
			if (depth >= NESTING_MAX)
				return ENOTSUP;
			loop_start[depth++] = at;
			continue;
		}
		if (shader->instructions[at].opcode != POWERVR_IR_END_LOOP)
			continue;
		if (depth == 0)
			return EINVAL;
		start = loop_start[--depth];

		/* Each value referenced inside the loop. */
		for (value = 1; value < shader->value_count; value++) {
			if (backend->first_use[value] == UINT32_MAX)
				continue;
			if (backend->last_use[value] < start || backend->first_use[value] > at)
				continue;

			/* Counts its references inside and outside, and its writes. */
			inside = 0;
			outside = 0;
			definitions = 0;
			first_definition = UINT32_MAX;
			first_use = UINT32_MAX;
			for (scan = 0; scan < shader->instruction_count; scan++) {
				instruction = &shader->instructions[scan];
				for (source = 0; source < 4; source++) {
					if (instruction->source[source].kind != POWERVR_IR_OPERAND_VALUE || instruction->source[source].value != value)
						continue;
					if (scan > start && scan < at) {
						inside++;
						if (first_use == UINT32_MAX)
							first_use = scan;
					} else {
						outside++;
					}
				}
				if (instruction->destination == value) {
					definitions++;
					if (scan > start && scan < at) {
						inside++;
						if (first_definition == UINT32_MAX)
							first_definition = scan;
					} else {
						outside++;
					}
				}
			}
			if (inside == 0)
				continue;

			/* A value local to one iteration keeps its span. */
			if (outside == 0 && definitions == 1 && first_definition != UINT32_MAX && (first_use == UINT32_MAX || first_definition < first_use))
				continue;

			/* Otherwise it lives over the whole loop. */
			if (backend->first_use[value] > start)
				backend->first_use[value] = start;
			if (backend->last_use[value] < at)
				backend->last_use[value] = at;
		}
	}

	/* Succeeded: the spans. */
	return 0;
}

/* Notes one reference to a value. */
static void
note_reference(
	struct shader_backend *backend,
	unsigned value,
	unsigned at,
	unsigned is_definition)
{
	/* Widens the span to the reference. */
	(void)is_definition;
	if (backend->first_use[value] == UINT32_MAX || at < backend->first_use[value])
		backend->first_use[value] = at;
	if (at > backend->last_use[value])
		backend->last_use[value] = at;
	backend->uses[value]++;
}

/*
 * Gives each value temporaries for its span: the lowest run of free
 * temporaries wide enough, freed after the span's last instruction.
 */
static int
allocate(
	const struct powervr_ir_shader *shader,
	struct shader_backend *backend)
{
	uint32_t busy_until[SHADER_MACHINE_TEMPORARIES];
	unsigned at;
	unsigned value;
	unsigned reg;
	unsigned width;
	unsigned word;
	unsigned found;

	/* Every temporary past the back end's own is free. */
	for (reg = 0; reg < SHADER_MACHINE_TEMPORARIES; reg++)
		busy_until[reg] = 0;

	/* At each instruction, places the values whose span starts there. */
	for (at = 0; at < shader->instruction_count; at++) {
		for (value = 1; value < shader->value_count; value++) {
			if (backend->first_use[value] != at)
				continue;

			/* The lowest run of temporaries free at this instruction (a sample returning half floats takes two). */
			width = shader->values[value].width;
			if (width == 0 || width > 4)
				return EINVAL;
			if (backend->sample_half[value] != 0)
				width = 2;
			found = 0;
			for (reg = TEMP_FIRST_VALUE; reg + width <= SHADER_MACHINE_TEMPORARIES; reg++) {
				for (word = 0; word < width; word++) {
					if (busy_until[reg + word] != 0 && busy_until[reg + word] >= at + 1U)
						break;
				}
				if (word == width) {
					found = 1;
					break;
				}
			}
			if (found == 0)
				return ENOSPC;

			/* Holds them until after the span's last instruction. */
			backend->value_register[value] = (uint16_t)reg;
			for (word = 0; word < width; word++)
				busy_until[reg + word] = backend->last_use[value] + 1U;
			if (reg + width > backend->temps)
				backend->temps = reg + width;
		}
	}

	/* Succeeded: every referenced value has registers. */
	return 0;
}

/* Selects the machine operations of one instruction. */
static int
select_instruction(
	struct backend_select *select,
	const struct powervr_ir_instruction *instruction)
{
	const struct powervr_shader_request *request;
	const struct powervr_shader_request *image;
	const struct powervr_shader_request *sampler;
	struct shader_machine_operation *operation;
	struct shader_machine_register destination;
	struct shader_machine_register source;
	struct shader_machine_register condition;
	struct shader_machine_register a;
	struct shader_machine_register b;
	struct shader_machine_register c;
	unsigned modifiers;
	unsigned ma;
	unsigned mb;
	unsigned mc;
	unsigned channel;
	unsigned word;
	unsigned opcode;
	unsigned variant;
	uint32_t packed;
	uint32_t coefficient;
	int error;

	/* Structure has its own selection. */
	opcode = instruction->opcode;
	select->literals = 0;
	if (opcode <= POWERVR_IR_CONTINUE) {
		error = select_structure(select, instruction);
		if (error != 0)
			return error;

		/* Succeeded: the structure's operations. */
		return 0;
	}

	/*
	 * A float add, multiply or multiply-add of a fragment program reads a
	 * constant from a shared register the data sequencer fills,
	 * not an immediate a pixel; the shared register never goes in the
	 * second source (select_float_pair and the multiply-add swap or move
	 * it).
	 */
	select->literals = (opcode == POWERVR_IR_FLOAT_ADD || opcode == POWERVR_IR_FLOAT_MULTIPLY || opcode == POWERVR_IR_FLOAT_MULTIPLY_ADD) &&
			   select->program->stage == POWERVR_SHADER_FRAGMENT && select->backend->inline_values == NULL;

	/* The operations that write a value. */
	switch (opcode) {
	case POWERVR_IR_MOVE:
		destination = destination_of(select, instruction);
		if (instruction->source[0].kind == POWERVR_IR_OPERAND_CONSTANT) {
			operation = append(select, MACHINE_IMMEDIATE);
			if (operation != NULL) {
				operation->destination[0] = destination;
				operation->immediate = instruction->source[0].constant;
			}
			break;
		}
		source = operand_register(select, &instruction->source[0], &modifiers);
		operation = append(select, MACHINE_MOVE);
		if (operation != NULL) {
			operation->destination[0] = destination;
			operation->source[0] = source;
			operation->modifiers[0] = (uint8_t)modifiers;
		}
		break;
	case POWERVR_IR_FLOAT_ADD:
		select_float_pair(select, MACHINE_FLOAT_ADD, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], 0);
		break;
	case POWERVR_IR_FLOAT_MULTIPLY:
		select_float_pair(select, MACHINE_FLOAT_MULTIPLY, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], 0);
		break;
	case POWERVR_IR_FLOAT_FLOOR:
		select_float_pair(select, MACHINE_FLOAT_ADD, destination_of(select, instruction), &instruction->source[0], NULL, 1);
		break;
	case POWERVR_IR_FLOAT_MULTIPLY_ADD:
		destination = destination_of(select, instruction);
		a = operand_register(select, &instruction->source[0], &ma);
		b = operand_register(select, &instruction->source[1], &mb);
		c = operand_register(select, &instruction->source[2], &mc);
		if (a.bank == POWERVR_USC_BANK_SHARED && b.bank == POWERVR_USC_BANK_SHARED && select->literals != 0) {
			/* Two shared factors would need a move: a constant of the two is an immediate instead. */
			select->literals = 0;
			if (instruction->source[1].kind == POWERVR_IR_OPERAND_CONSTANT) {
				b = operand_register(select, &instruction->source[1], &mb);
			} else if (instruction->source[0].kind == POWERVR_IR_OPERAND_CONSTANT) {
				a = operand_register(select, &instruction->source[0], &ma);
			}
			select->literals = 1;
		}
		if (b.bank == POWERVR_USC_BANK_SHARED) {
			/* A shared register cannot be the second source: the factors swap, or it is moved. */
			if (a.bank != POWERVR_USC_BANK_SHARED) {
				source = a;
				a = b;
				b = source;
				modifiers = ma;
				ma = mb;
				mb = modifiers;
			} else {
				source = scratch_register(select);
				operation = append(select, MACHINE_MOVE);
				if (operation != NULL) {
					operation->destination[0] = source;
					operation->source[0] = b;
				}
				b = source;
			}
		}
		operation = append(select, MACHINE_FLOAT_MULTIPLY_ADD);
		if (operation != NULL) {
			operation->destination[0] = destination;
			operation->source[0] = a;
			operation->source[1] = b;
			operation->source[2] = c;
			operation->modifiers[0] = (uint8_t)ma;
			operation->modifiers[1] = (uint8_t)mb;
			operation->modifiers[2] = (uint8_t)mc;
		}
		break;
	case POWERVR_IR_FLOAT_RECIPROCAL:
	case POWERVR_IR_FLOAT_RECIPROCAL_ROOT:
	case POWERVR_IR_FLOAT_LOG2:
	case POWERVR_IR_FLOAT_EXP2:
		destination = destination_of(select, instruction);
		source = operand_register(select, &instruction->source[0], &modifiers);
		operation = append(select, MACHINE_SINGLE);
		if (operation != NULL) {
			operation->destination[0] = destination;
			operation->source[0] = source;
			operation->modifiers[0] = (uint8_t)modifiers;
			if (opcode == POWERVR_IR_FLOAT_RECIPROCAL) {
				operation->variant = POWERVR_USC_SINGLE_RECIPROCAL;
			} else if (opcode == POWERVR_IR_FLOAT_RECIPROCAL_ROOT) {
				operation->variant = POWERVR_USC_SINGLE_RECIPROCAL_ROOT;
			} else if (opcode == POWERVR_IR_FLOAT_LOG2) {
				operation->variant = POWERVR_USC_SINGLE_LOG2;
			} else {
				operation->variant = POWERVR_USC_SINGLE_EXP2;
			}
		}
		break;
	case POWERVR_IR_FLOAT_MINIMUM:
	case POWERVR_IR_FLOAT_MAXIMUM:
		/*
		 * A constant bound is read first, from a shared register the data
		 * sequencer fills like a float operation's constant (an immediate
		 * was a group a pixel); a shared register cannot be the second
		 * source, so the other source must not be one.
		 */
		variant = opcode == POWERVR_IR_FLOAT_MINIMUM ? TEST_LESS : TEST_GREATER;
		select->literals = select->program->stage == POWERVR_SHADER_FRAGMENT && select->backend->inline_values == NULL &&
				   instruction->source[1].kind == POWERVR_IR_OPERAND_CONSTANT && instruction->source[0].kind == POWERVR_IR_OPERAND_VALUE &&
				   select->backend->shared_alias[instruction->source[0].value] == 0;
		if (select->literals != 0) {
			select_two_sourced(select, MACHINE_MINMAX, variant, TYPE_F32, destination_of(select, instruction), &instruction->source[1], &instruction->source[0], POWERVR_IR_NEGATE | POWERVR_IR_ABSOLUTE);
			select->literals = 0;
			break;
		}
		select_two_sourced(select, MACHINE_MINMAX, variant, TYPE_F32, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], POWERVR_IR_NEGATE | POWERVR_IR_ABSOLUTE);
		break;
	case POWERVR_IR_UNSIGNED_MINIMUM:
		select_two_sourced(select, MACHINE_MINMAX, TEST_LESS, TYPE_U32, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], 0);
		break;
	case POWERVR_IR_UNSIGNED_MAXIMUM:
		select_two_sourced(select, MACHINE_MINMAX, TEST_GREATER, TYPE_U32, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], 0);
		break;
	case POWERVR_IR_SIGNED_MINIMUM:
		select_two_sourced(select, MACHINE_MINMAX, TEST_LESS, TYPE_S32, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], 0);
		break;
	case POWERVR_IR_SIGNED_MAXIMUM:
		select_two_sourced(select, MACHINE_MINMAX, TEST_GREATER, TYPE_S32, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], 0);
		break;
	case POWERVR_IR_FLOAT_LESS:
		select_two_sourced(select, MACHINE_COMPARE, TEST_LESS, TYPE_F32, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], POWERVR_IR_NEGATE | POWERVR_IR_ABSOLUTE);
		break;
	case POWERVR_IR_FLOAT_GREATER_EQUAL:
		select_two_sourced(select, MACHINE_COMPARE, TEST_GREATER_EQUAL, TYPE_F32, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], POWERVR_IR_NEGATE | POWERVR_IR_ABSOLUTE);
		break;
	case POWERVR_IR_FLOAT_EQUAL:
		select_two_sourced(select, MACHINE_COMPARE, TEST_EQUAL, TYPE_F32, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], POWERVR_IR_NEGATE | POWERVR_IR_ABSOLUTE);
		break;
	case POWERVR_IR_FLOAT_NOT_EQUAL:
		select_two_sourced(select, MACHINE_COMPARE, TEST_NOT_EQUAL, TYPE_F32, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], POWERVR_IR_NEGATE | POWERVR_IR_ABSOLUTE);
		break;
	case POWERVR_IR_INTEGER_EQUAL:
		select_two_sourced(select, MACHINE_COMPARE, TEST_EQUAL, TYPE_U32, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], 0);
		break;
	case POWERVR_IR_INTEGER_NOT_EQUAL:
		select_two_sourced(select, MACHINE_COMPARE, TEST_NOT_EQUAL, TYPE_U32, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], 0);
		break;
	case POWERVR_IR_UNSIGNED_LESS:
		select_two_sourced(select, MACHINE_COMPARE, TEST_LESS, TYPE_U32, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], 0);
		break;
	case POWERVR_IR_UNSIGNED_GREATER_EQUAL:
		select_two_sourced(select, MACHINE_COMPARE, TEST_GREATER_EQUAL, TYPE_U32, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], 0);
		break;
	case POWERVR_IR_SIGNED_LESS:
		select_two_sourced(select, MACHINE_COMPARE, TEST_LESS, TYPE_S32, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], 0);
		break;
	case POWERVR_IR_SIGNED_GREATER_EQUAL:
		select_two_sourced(select, MACHINE_COMPARE, TEST_GREATER_EQUAL, TYPE_S32, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], 0);
		break;
	case POWERVR_IR_SELECT:
		destination = destination_of(select, instruction);
		condition = operand_plain(select, &instruction->source[0], 0);
		a = operand_register(select, &instruction->source[1], &ma);
		b = operand_register(select, &instruction->source[2], &mb);
		operation = append(select, MACHINE_SELECT);
		if (operation != NULL) {
			operation->destination[0] = destination;
			operation->source[0] = condition;
			operation->source[1] = a;
			operation->source[2] = b;
			operation->modifiers[1] = (uint8_t)ma;
			operation->modifiers[2] = (uint8_t)mb;
		}
		break;
	case POWERVR_IR_INTEGER_ADD:
	case POWERVR_IR_INTEGER_MULTIPLY:
		/*
		 * The sources as they are, a constant one or zero from its special
		 * register: the multiply-add reads special registers (the rule
		 * probe shows it, as Mesa's compiler does).
		 */
		destination = destination_of(select, instruction);
		a = operand_plain(select, &instruction->source[0], 0);
		b = operand_plain(select, &instruction->source[1], 0);
		if (opcode == POWERVR_IR_INTEGER_ADD)
			c = bank_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_ONE);
		else
			c = bank_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_ZERO);
		operation = append(select, MACHINE_INTEGER);
		if (operation != NULL) {
			operation->destination[0] = destination;
			operation->source[0] = a;
			if (opcode == POWERVR_IR_INTEGER_ADD) {
				/* a times one plus b. */
				operation->source[1] = c;
				operation->source[2] = b;
			} else {
				/* a times b plus zero. */
				operation->source[1] = b;
				operation->source[2] = c;
			}
		}
		break;
	case POWERVR_IR_BIT_AND:
	case POWERVR_IR_BIT_OR:
	case POWERVR_IR_BIT_XOR:
		/* The logic operation the opcode names. */
		variant = LOGIC_XOR;
		if (opcode == POWERVR_IR_BIT_AND)
			variant = LOGIC_AND;
		else if (opcode == POWERVR_IR_BIT_OR)
			variant = LOGIC_OR;
		select_two_sourced(select, MACHINE_LOGIC, variant, 0, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], 0);
		break;
	case POWERVR_IR_SHIFT_LEFT:
	case POWERVR_IR_SHIFT_RIGHT:
	case POWERVR_IR_SHIFT_RIGHT_SIGNED:
		/* The shift the opcode names. */
		variant = SHIFT_RIGHT_SIGNED;
		if (opcode == POWERVR_IR_SHIFT_LEFT)
			variant = SHIFT_LEFT;
		else if (opcode == POWERVR_IR_SHIFT_RIGHT)
			variant = SHIFT_RIGHT;
		select_two_sourced(select, MACHINE_SHIFT, variant, 0, destination_of(select, instruction), &instruction->source[0], &instruction->source[1], 0);
		break;
	case POWERVR_IR_FLOAT_TO_SIGNED:
	case POWERVR_IR_FLOAT_TO_UNSIGNED:
		destination = destination_of(select, instruction);
		source = operand_plain(select, &instruction->source[0], 0);
		operation = append(select, MACHINE_PACK);
		if (operation != NULL) {
			operation->destination[0] = destination;
			operation->source[0] = source;
			operation->variant = opcode == POWERVR_IR_FLOAT_TO_SIGNED ? PACK_S32 : PACK_U32;
			operation->immediate = 1;
			operation->immediate_2 = PACK_ROUND_TO_ZERO;
		}
		break;
	case POWERVR_IR_SIGNED_TO_FLOAT:
	case POWERVR_IR_UNSIGNED_TO_FLOAT:
		destination = destination_of(select, instruction);
		source = operand_plain(select, &instruction->source[0], 0);
		operation = append(select, MACHINE_UNPACK);
		if (operation != NULL) {
			operation->destination[0] = destination;
			operation->source[0] = source;
			operation->variant = opcode == POWERVR_IR_SIGNED_TO_FLOAT ? PACK_S32 : PACK_U32;
			operation->immediate = 1;
		}
		break;
	case POWERVR_IR_SHARED:
		/* A move from the request's shared register. */
		if (instruction->immediate >= select->program->request_count)
			return EINVAL;
		request = &select->program->requests[instruction->immediate];
		if (instruction->immediate_2 >= request->words)
			return EINVAL;
		if (POWERVR_SHADER_WORKGROUP_COEFFICIENTS && request->kind == POWERVR_SHADER_REQUEST_WORKGROUP_ID) {
			/* The workgroup id: the data sequencer writes word k into coefficient k for each workgroup. */
			operation = append(select, MACHINE_MOVE);
			if (operation != NULL) {
				operation->destination[0] = destination_of(select, instruction);
				operation->source[0] = bank_register(POWERVR_USC_BANK_COEFFICIENT, instruction->immediate_2);
			}
			break;
		}
		if (select->backend->inline_values != NULL) {
			/* The word given up front, as a constant. */
			operation = append(select, MACHINE_IMMEDIATE);
			if (operation != NULL) {
				operation->destination[0] = destination_of(select, instruction);
				operation->immediate = select->backend->inline_values[request->shared + instruction->immediate_2];
			}
			break;
		}
		/* A word read in place needs no copy. */
		if (select->backend->shared_alias[instruction->destination] != 0)
			break;
		operation = append(select, MACHINE_MOVE);
		if (operation != NULL) {
			operation->destination[0] = destination_of(select, instruction);
			operation->source[0] = bank_register(POWERVR_USC_BANK_SHARED, request->shared + instruction->immediate_2);
		}
		break;
	case POWERVR_IR_INSTANCE:
		operation = append(select, MACHINE_MOVE_SPECIAL);
		if (operation != NULL) {
			operation->destination[0] = destination_of(select, instruction);
			operation->source[0] = bank_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_INSTANCE);
		}
		break;
	case POWERVR_IR_INPUT:
		operation = append(select, MACHINE_MOVE);
		if (operation != NULL) {
			operation->destination[0] = destination_of(select, instruction);
			operation->source[0] = bank_register(POWERVR_USC_BANK_VERTEX_INPUT, instruction->immediate);
		}
		break;
	case POWERVR_IR_ITERATE:
		destination = destination_of(select, instruction);
		if (select->backend->iterate_sample[instruction->destination] != 0) {
			/* A coordinate the sample that reads it iterates itself. */
			break;
		}
		if ((instruction->flags & POWERVR_IR_FLAT) != 0) {
			/* A flat varying is its C coefficient, read in place when only float operations read it. */
			if (select->backend->coefficient_alias[instruction->destination] != 0)
				break;
			operation = append(select, MACHINE_MOVE);
			if (operation != NULL) {
				operation->destination[0] = destination;
				operation->source[0] = bank_register(POWERVR_USC_BANK_COEFFICIENT, instruction->immediate + 2U);
			}
		} else {
			/* An iterated varying, then the wait for it. */
			operation = append(select, MACHINE_ITERATE);
			if (operation != NULL) {
				operation->destination[0] = destination;
				operation->immediate = instruction->immediate;
				operation->immediate_2 = select->program->w_coefficient;
			}
			(void)append(select, MACHINE_WAIT);
		}
		break;
	case POWERVR_IR_LOAD:
		/* The address, the load, the wait for it. */
		destination = destination_of(select, instruction);
		select_address(select, instruction);
		operation = append(select, MACHINE_LOAD);
		if (operation != NULL) {
			operation->destination[0] = destination;
			operation->source[0] = temp_register(TEMP_ADDRESS);
		}
		(void)append(select, MACHINE_WAIT);
		break;
	case POWERVR_IR_STORE:
		/*
		 * The data moved right after the address pair, the address, the
		 * store, the wait for it: the store reads its data from the
		 * register after the pair, whatever its data source names (the
		 * rule probe showed data elsewhere was not stored; Mesa's
		 * compiler places it the same way).
		 */
		source = operand_plain(select, &instruction->source[3], 0);
		operation = append(select, MACHINE_MOVE);
		if (operation != NULL) {
			operation->destination[0] = temp_register(TEMP_STORE_DATA);
			operation->source[0] = source;
		}
		select_address(select, instruction);
		operation = append(select, MACHINE_STORE);
		if (operation != NULL) {
			operation->source[0] = temp_register(TEMP_STORE_DATA);
			operation->source[1] = temp_register(TEMP_ADDRESS);
		}
		(void)append(select, MACHINE_WAIT);
		break;
	case POWERVR_IR_OUTPUT:
		source = operand_plain(select, &instruction->source[0], 0);
		if (select->program->stage == POWERVR_SHADER_VERTEX) {
			operation = append(select, MACHINE_VERTEX_WRITE);
			if (operation != NULL) {
				operation->source[0] = source;
				operation->immediate = instruction->immediate;
			}
		} else if (select->program->stage == POWERVR_SHADER_FRAGMENT) {
			operation = append(select, MACHINE_MOVE);
			if (operation != NULL) {
				operation->destination[0] = temp_register(select->output_base + instruction->immediate);
				operation->source[0] = source;
			}
		} else {
			return EINVAL;
		}
		break;
	case POWERVR_IR_SAMPLE:
		/* The coordinates side by side, the sample, the wait for it. */
		if (instruction->immediate >= select->program->request_count || instruction->immediate_2 >= select->program->request_count)
			return EINVAL;
		image = &select->program->requests[instruction->immediate];
		sampler = &select->program->requests[instruction->immediate_2];
		if (instruction->source[0].kind == POWERVR_IR_OPERAND_VALUE && select->backend->iterate_sample[instruction->source[0].value] != 0) {
			/* Its coordinates iterated by the sample itself, then the wait for it. */
			coefficient = iterate_coefficient(select->shader, instruction->source[0].value);
			operation = append(select, MACHINE_ITERATE_SAMPLE);
			if (operation != NULL) {
				operation->destination[0] = destination_of(select, instruction);
				operation->source[0] = bank_register(POWERVR_USC_BANK_SHARED, image->shared);
				operation->source[2] = bank_register(POWERVR_USC_BANK_SHARED, sampler->shared);
				operation->immediate = coefficient;
				operation->immediate_2 = select->program->w_coefficient;
				operation->half = select->backend->sample_half[instruction->destination];
			}

			/* The wait for its data: the operation's own (the form that waits), or a wait after it. */
			if ((select->backend->forms & POWERVR_SHADER_FORM_SAMPLE_WAITS) != 0) {
				if (operation != NULL)
					operation->variant = SCHEDULE_WAIT_DATA;
				break;
			}
			(void)append(select, MACHINE_WAIT);
			break;
		}
		a = operand_plain(select, &instruction->source[0], 0);
		operation = append(select, MACHINE_MOVE);
		if (operation != NULL) {
			operation->destination[0] = temp_register(TEMP_COORDINATES);
			operation->source[0] = a;
		}
		b = operand_plain(select, &instruction->source[1], 0);
		operation = append(select, MACHINE_MOVE);
		if (operation != NULL) {
			operation->destination[0] = temp_register(TEMP_COORDINATES + 1U);
			operation->source[0] = b;
		}
		operation = append(select, MACHINE_SAMPLE);
		if (operation != NULL) {
			operation->destination[0] = destination_of(select, instruction);
			operation->source[0] = bank_register(POWERVR_USC_BANK_SHARED, image->shared);
			operation->source[1] = temp_register(TEMP_COORDINATES);
			operation->source[2] = bank_register(POWERVR_USC_BANK_SHARED, sampler->shared);
		}
		(void)append(select, MACHINE_WAIT);
		break;
	case POWERVR_IR_PIXEL_LOAD:
		/* The pixel output, under the overlap check, unpacked into the four words (8-bit unorm, or half floats from two registers). */
		if (instruction->immediate >= 4 || (instruction->immediate_2 != 0 && instruction->immediate >= 3))
			return ENOTSUP;
		destination = destination_of(select, instruction);
		for (word = 0; word < (instruction->immediate_2 != 0 ? 2U : 1U); word++) {
			operation = append(select, MACHINE_MOVE);
			if (operation != NULL) {
				operation->destination[0] = temp_register(TEMP_SCRATCH);
				operation->source[0] = bank_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_PIXEL_OUTPUT + instruction->immediate + word);
				operation->variant = 1;
			}
			operation = append(select, MACHINE_UNPACK);
			if (operation != NULL) {
				operation->destination[0] = destination;
				operation->destination[0].index = (uint16_t)(destination.index + word * 2U);
				operation->source[0] = temp_register(TEMP_SCRATCH);
				if (instruction->immediate_2 != 0) {
					operation->variant = PACK_F16F16;
					operation->immediate = 2;
				} else {
					operation->variant = PACK_U8888;
					operation->immediate = 4;
					operation->immediate_2 = PACK_SCALE;
				}
			}
		}
		break;
	case POWERVR_IR_PIXEL_STORE:
		/* The four words side by side, packed, into the pixel output under the overlap check (8-bit unorm, or half floats into two registers). */
		if (instruction->immediate >= 4 || (instruction->immediate_2 != 0 && instruction->immediate >= 3))
			return ENOTSUP;
		if (pack_constant(instruction, &packed)) {
			/* A constant colour of noughts and ones: its packed word, moved under the overlap check. */
			operation = append(select, MACHINE_IMMEDIATE);
			if (operation != NULL) {
				operation->destination[0] = temp_register(TEMP_SCRATCH);
				operation->immediate = packed;
			}
			operation = append(select, MACHINE_MOVE);
			if (operation != NULL) {
				operation->destination[0] = bank_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_PIXEL_OUTPUT + instruction->immediate);
				operation->source[0] = temp_register(TEMP_SCRATCH);
				operation->variant = 1;
			}
			break;
		}
		if ((select->backend->forms & POWERVR_SHADER_FORM_SUM_OF_PRODUCTS) != 0 && instruction->immediate_2 == 0) {
			/* The sum-of-products form: the four floats (or a sample's half floats) written by one operation. */
			if (!select_sum_half(select, instruction, POWERVR_USC_SUM_OPAQUE_HALF))
				select_sum(select, instruction, POWERVR_USC_SUM_OPAQUE);
			break;
		}

		/* The words where one value holds them in order (a sample's four, two moves a pixel), else moved side by side. */
		a = temp_register(TEMP_COORDINATES);
		error = adjacent_words(select, instruction, &a);
		if (error == 0) {
			for (channel = 0; channel < 4; channel++) {
				select->scratch = 0;
				source = operand_plain(select, &instruction->source[channel], 0);
				operation = append(select, MACHINE_MOVE);
				if (operation != NULL) {
					operation->destination[0] = temp_register(TEMP_COORDINATES + channel);
					operation->source[0] = source;
				}
			}
		}
		for (word = 0; word < (instruction->immediate_2 != 0 ? 2U : 1U); word++) {
			operation = append(select, MACHINE_PACK);
			if (operation != NULL) {
				operation->destination[0] = temp_register(TEMP_SCRATCH);
				operation->source[0] = temp_register(a.index + word * 2U);
				if (instruction->immediate_2 != 0) {
					operation->variant = PACK_F16F16;
					operation->immediate = 2;
				} else {
					operation->variant = PACK_U8888;
					operation->immediate = 4;
					operation->immediate_2 = PACK_SCALE;
				}
			}
			operation = append(select, MACHINE_MOVE);
			if (operation != NULL) {
				operation->destination[0] = bank_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_PIXEL_OUTPUT + instruction->immediate + word);
				operation->source[0] = temp_register(TEMP_SCRATCH);
				operation->variant = 1;
			}
		}
		break;
	case POWERVR_IR_PACK:
		/* The four words side by side, packed as 8-bit unsigned normalised channels into the value (a shared program's colour). */
		for (channel = 0; channel < 4; channel++) {
			select->scratch = 0;
			source = operand_plain(select, &instruction->source[channel], 0);
			operation = append(select, MACHINE_MOVE);
			if (operation != NULL) {
				operation->destination[0] = temp_register(TEMP_COORDINATES + channel);
				operation->source[0] = source;
			}
		}
		operation = append(select, MACHINE_PACK);
		if (operation != NULL) {
			operation->destination[0] = destination_of(select, instruction);
			operation->source[0] = temp_register(TEMP_COORDINATES);
			operation->variant = PACK_U8888;
			operation->immediate = 4;
			operation->immediate_2 = PACK_SCALE;
		}
		break;
	case POWERVR_IR_SHARED_STORE:
		/* The word into the shared register the program's pixels read it from. */
		source = operand_plain(select, &instruction->source[0], 0);
		operation = append(select, MACHINE_MOVE);
		if (operation != NULL) {
			operation->destination[0] = bank_register(POWERVR_USC_BANK_SHARED, instruction->immediate);
			operation->source[0] = source;
		}
		break;
	case POWERVR_IR_PIXEL_BLEND:
		/* The blend of the sum-of-products form: one operation of the colour and the destination's. */
		if (instruction->immediate >= 4)
			return ENOTSUP;
		switch (instruction->immediate_2) {
		case POWERVR_IR_BLEND_OVER:
			if (!select_sum_half(select, instruction, POWERVR_USC_SUM_OVER_HALF))
				select_sum(select, instruction, POWERVR_USC_SUM_OVER);
			break;
		case POWERVR_IR_BLEND_SUBTRACT:
			select_sum(select, instruction, POWERVR_USC_SUM_SUBTRACT);
			break;
		case POWERVR_IR_BLEND_ADD:
			select_sum(select, instruction, POWERVR_USC_SUM_ADD);
			break;
		case POWERVR_IR_BLEND_MULTIPLY:
			select_sum(select, instruction, POWERVR_USC_SUM_MULTIPLY);
			break;
		default:
			return ENOTSUP;
		}
		break;
	case POWERVR_IR_FEEDBACK:
		/*
		 * p0 = the discard is zero; the alpha feedback that passes the
		 * fragment, run only where p0 holds (a discarded fragment gives
		 * none); then the wait for its data return counter.
		 *
		 * Procedure referenced:
		 * Mesa 26.2.5 src/imagination/pco/pco_trans_nir.c
		 * (nir_intrinsic_isp_feedback_pco without depth feedback: tstz
		 * of the discard into p0 as u32, alphaf drc0 with sources zero,
		 * zero and 7 under the overlap check, its execution condition
		 * e1_z1) and pco_schedule.c (a wdf after an instruction that
		 * names a data return counter).
		 */
		if (select->program->stage != POWERVR_SHADER_FRAGMENT)
			return ENOTSUP;
		condition = operand_plain(select, &instruction->source[0], 0);
		operation = append(select, MACHINE_TEST_ZERO);
		if (operation != NULL)
			operation->source[0] = condition;
		operation = append(select, MACHINE_FEEDBACK);
		if (operation != NULL)
			operation->condition = POWERVR_USC_RUN_IF_PREDICATE;
		(void)append(select, MACHINE_WAIT);
		break;
	case POWERVR_IR_PIXEL_SHARED:
		/* The colour the shared program packed (or a literal), moved into the pixel output under the overlap check. */
		if (instruction->immediate >= 4)
			return ENOTSUP;
		operation = append(select, MACHINE_MOVE);
		if (operation != NULL) {
			operation->destination[0] = bank_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_PIXEL_OUTPUT + instruction->immediate);
			operation->source[0] = bank_register(POWERVR_USC_BANK_SHARED, instruction->immediate_2);
			operation->variant = 1;
		}
		break;
	default:
		return ENOTSUP;
	}

	/* A saturated float operation: its own operation, the last one selected, clamps. */
	if ((instruction->flags & POWERVR_IR_SATURATE) != 0 &&
	    (opcode == POWERVR_IR_FLOAT_ADD || opcode == POWERVR_IR_FLOAT_MULTIPLY || opcode == POWERVR_IR_FLOAT_MULTIPLY_ADD)) {
		if (select->backend->operation_count == 0)
			return EINVAL;
		operation = &select->backend->operations[select->backend->operation_count - 1U];
		if (operation->kind != MACHINE_FLOAT_ADD && operation->kind != MACHINE_FLOAT_MULTIPLY && operation->kind != MACHINE_FLOAT_MULTIPLY_ADD)
			return ENOTSUP;
		operation->saturate = 1;
	}

	/* Reports what the selection came to. */
	return select->backend->error;
}

/*
 * Selects the execution mask counter's steps for one structure
 * instruction.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_cf.c (lower_if_cond_exec,
 * lower_loop, lower_break_continue, pco_emc_ref: the prologue,
 * interlogue and epilogue of an if and of a loop, the break and
 * continue values of the nesting plus two and one).
 */
static int
select_structure(
	struct backend_select *select,
	const struct powervr_ir_instruction *instruction)
{
	struct shader_machine_operation *operation;
	struct shader_machine_register condition;
	struct backend_loop *loop;
	struct backend_if *open_if;
	unsigned target;

	/* Dispatches on the structure. */
	switch (instruction->opcode) {
	case POWERVR_IR_IF:
		/* p0 = the condition is zero; the counter starts where it is not; skip if no lane runs. */
		if (select->if_depth >= NESTING_MAX)
			return ENOTSUP;
		condition = operand_plain(select, &instruction->source[0], 0);
		operation = append(select, MACHINE_TEST_ZERO);
		if (operation != NULL)
			operation->source[0] = condition;
		select_condition(select, CONDITION_START, PREDICATE_P0_FALSE, 1, temp_register(TEMP_EMC));
		open_if = &select->ifs[select->if_depth++];
		open_if->branch = select_branch(select, BRANCH_ALL_DISABLED, POWERVR_USC_RUN_ENABLED);
		open_if->has_else = 0;
		if (select->loop_depth != 0)
			select->loops[select->loop_depth - 1U].ifs++;
		break;
	case POWERVR_IR_ELSE:
		/* The arms swap; the if's skip lands here; skip the else if no lane runs. */
		if (select->if_depth == 0)
			return EINVAL;
		open_if = &select->ifs[select->if_depth - 1U];
		target = select->backend->operation_count;
		select_condition(select, CONDITION_ELSE, PREDICATE_ALWAYS, 1, temp_register(TEMP_EMC));
		select->backend->operations[open_if->branch].target = target;
		open_if->else_branch = select_branch(select, BRANCH_ALL_DISABLED, POWERVR_USC_RUN_ENABLED);
		open_if->has_else = 1;
		break;
	case POWERVR_IR_END_IF:
		/* The pending skip lands on the counter's end step. */
		if (select->if_depth == 0)
			return EINVAL;
		open_if = &select->ifs[--select->if_depth];
		target = select->backend->operation_count;
		if (open_if->has_else != 0) {
			select->backend->operations[open_if->else_branch].target = target;
		} else {
			select->backend->operations[open_if->branch].target = target;
		}
		select_condition(select, CONDITION_END, PREDICATE_ALWAYS, 1, temp_register(TEMP_EMC));
		if (select->loop_depth != 0)
			select->loops[select->loop_depth - 1U].ifs--;
		break;
	case POWERVR_IR_LOOP:
		/* The prologue: the counter steps by two; skip to the epilogue if no lane runs. */
		if (select->loop_depth >= NESTING_MAX)
			return ENOTSUP;
		select_condition(select, CONDITION_START, PREDICATE_ALWAYS, 2, temp_register(TEMP_EMC));
		loop = &select->loops[select->loop_depth++];
		loop->branch = select_branch(select, BRANCH_ALL_DISABLED, POWERVR_USC_RUN_ENABLED);
		loop->body = select->backend->operation_count;
		loop->ifs = 0;
		loop->interlogue = 0;
		break;
	case POWERVR_IR_BREAK:
	case POWERVR_IR_CONTINUE:
		/* The running lanes stop, by the nesting plus two for a break, plus one for a continue. */
		if (select->loop_depth == 0)
			return EINVAL;
		loop = &select->loops[select->loop_depth - 1U];
		operation = append(select, MACHINE_IMMEDIATE);
		if (operation != NULL) {
			operation->destination[0] = temp_register(TEMP_SCRATCH);
			operation->immediate = loop->ifs + (instruction->opcode == POWERVR_IR_BREAK ? 2U : 1U);
		}
		operation = append(select, MACHINE_CONDITION);
		if (operation != NULL) {
			operation->variant = CONDITION_SET;
			operation->condition = POWERVR_USC_RUN_IGNORING_ENABLE;
			operation->immediate = PREDICATE_ALWAYS;
			operation->immediate_2 = 0;
			operation->destination[0] = temp_register(TEMP_EMC);
			operation->source[0] = temp_register(TEMP_EMC);
			operation->source[1] = temp_register(TEMP_SCRATCH);
		}
		break;
	case POWERVR_IR_LOOP_CONTINUE:
	case POWERVR_IR_END_LOOP:
		/* The interlogue, once: the continued lanes come back. */
		if (select->loop_depth == 0)
			return EINVAL;
		loop = &select->loops[select->loop_depth - 1U];
		if (loop->interlogue == 0) {
			select_condition(select, CONDITION_END, PREDICATE_ALWAYS, 1, temp_register(TEMP_EMC));
			select_condition(select, CONDITION_START, PREDICATE_ALWAYS, 1, temp_register(TEMP_EMC));
			loop->interlogue = 1;
		}
		if (instruction->opcode == POWERVR_IR_LOOP_CONTINUE)
			break;

		/* The epilogue: the loop test, and back to the body while a lane loops. */
		select->backend->operations[loop->branch].target = select->backend->operation_count;
		select_condition(select, CONDITION_LOOP_TEST, PREDICATE_ALWAYS, 2, temp_register(TEMP_EMC));
		target = select_branch(select, BRANCH_ANY_CONDITION, POWERVR_USC_RUN_IF_PREDICATE);
		select->backend->operations[target].target = loop->body;
		select->loop_depth--;
		break;
	default:
		return EINVAL;
	}

	/* Reports what the selection came to. */
	return select->backend->error;
}

/* Appends one machine operation, or notes that there is no room. */
static struct shader_machine_operation *
append(
	struct backend_select *select,
	unsigned kind)
{
	struct shader_machine_operation *operation;
	struct shader_backend *backend;
	unsigned index;

	/* Refuses a program with more operations than the table holds. */
	backend = select->backend;
	if (backend->operation_count >= SHADER_MACHINE_OPERATIONS) {
		if (backend->error == 0)
			backend->error = E2BIG;
		return NULL;
	}

	/* A cleared operation of the kind. */
	operation = &backend->operations[backend->operation_count++];
	operation->kind = (uint8_t)kind;
	operation->condition = POWERVR_USC_RUN_ENABLED;
	operation->variant = 0;
	operation->modifiers[0] = 0;
	operation->modifiers[1] = 0;
	operation->modifiers[2] = 0;
	operation->end = 0;
	operation->saturate = 0;
	operation->overlap = 0;
	operation->pair = 0;
	operation->half = 0;
	for (index = 0; index < 2; index++) {
		operation->destination[index].bank = 0xff;
		operation->destination[index].index = 0;
	}
	for (index = 0; index < 4; index++) {
		operation->source[index].bank = 0xff;
		operation->source[index].index = 0;
	}
	operation->immediate = 0;
	operation->immediate_2 = 0;
	operation->target = 0;
	return operation;
}

/* Names a temporary. */
static struct shader_machine_register
temp_register(
	unsigned index)
{
	struct shader_machine_register reg;

	/* The temporary bank's register. */
	reg = bank_register(POWERVR_USC_BANK_TEMPORARY, index);

	/* Succeeded: the temporary. */
	return reg;
}

/* Names a register of a bank. */
static struct shader_machine_register
bank_register(
	unsigned bank,
	unsigned index)
{
	struct shader_machine_register reg;

	reg.bank = (uint8_t)bank;
	reg.index = (uint16_t)index;
	return reg;
}

/* Names the first temporary of an instruction's destination. */
static struct shader_machine_register
destination_of(
	struct backend_select *select,
	const struct powervr_ir_instruction *instruction)
{
	struct shader_machine_register reg;

	/* The first temporary the value was given. */
	reg = temp_register(select->backend->value_register[instruction->destination]);

	/* Succeeded: the destination's register. */
	return reg;
}

/*
 * Names the register an operand reads, with its modifiers: a value's
 * temporary, a hardware constant, or a scratch temporary a constant is
 * moved into first.
 */
static struct shader_machine_register
operand_register(
	struct backend_select *select,
	const struct powervr_ir_operand *operand,
	unsigned *modifiers)
{
	struct shader_machine_operation *operation;
	struct shader_machine_register reg;

	/* A value's word: its shared register, or its coefficient register, when it is one only float operations read. */
	*modifiers = 0;
	if (operand->kind == POWERVR_IR_OPERAND_VALUE && select->backend->coefficient_alias[operand->value] != 0) {
		*modifiers = operand->modifiers;
		reg = bank_register(POWERVR_USC_BANK_COEFFICIENT, select->backend->coefficient_alias[operand->value] - 1U);

		/* Succeeded: the coefficient register. */
		return reg;
	}
	if (operand->kind == POWERVR_IR_OPERAND_VALUE && select->backend->shared_alias[operand->value] != 0) {
		*modifiers = operand->modifiers;
		reg = bank_register(POWERVR_USC_BANK_SHARED, select->backend->shared_alias[operand->value] - 1U);

		/* Succeeded: the shared register. */
		return reg;
	}
	if (operand->kind == POWERVR_IR_OPERAND_VALUE) {
		*modifiers = operand->modifiers;
		reg = temp_register(select->backend->value_register[operand->value] + operand->component);

		/* Succeeded: the value's register. */
		return reg;
	}

	/* The constants the hardware holds. */
	if (operand->constant == 0) {
		reg = bank_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_ZERO);

		/* Succeeded: the zero register. */
		return reg;
	}

	if (operand->constant == 1) {
		reg = bank_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_ONE);

		/* Succeeded: the one register. */
		return reg;
	}

	if (operand->constant == 0x3f800000U) {
		reg = bank_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_FLOAT_ONE);

		/* Succeeded: the float one register. */
		return reg;
	}

	if (operand->constant == 0xffffffffU) {
		reg = bank_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_TRUE);

		/* Succeeded: the true register. */
		return reg;
	}

	/* Any other constant: a shared register the data sequencer fills when a float operation reads it, else a scratch temporary. */
	if (select->literals != 0 && literal_register(select, operand->constant, &reg) == 0)
		return reg;
	reg = scratch_register(select);
	operation = append(select, MACHINE_IMMEDIATE);
	if (operation != NULL) {
		operation->destination[0] = reg;
		operation->immediate = operand->constant;
	}
	return reg;
}

/*
 * Names the register an operand reads with only the allowed modifiers
 * left on it; the others are applied by a move into a scratch temporary.
 */
static struct shader_machine_register
operand_plain(
	struct backend_select *select,
	const struct powervr_ir_operand *operand,
	unsigned allowed)
{
	struct shader_machine_operation *operation;
	struct shader_machine_register reg;
	struct shader_machine_register moved;
	unsigned modifiers;

	/* The register and its modifiers. */
	reg = operand_register(select, operand, &modifiers);
	if ((modifiers & ~allowed) == 0)
		return reg;

	/* A move applies them. */
	moved = scratch_register(select);
	operation = append(select, MACHINE_MOVE);
	if (operation != NULL) {
		operation->destination[0] = moved;
		operation->source[0] = reg;
		operation->modifiers[0] = (uint8_t)modifiers;
	}
	return moved;
}

/*
 * The most literals the back end adds, and the most requests a program
 * may have for it to add one, so the requests stay well within the data
 * sequencer's loads (POWERVR_DATA_LOADS_MAX, a load or two a request).
 */
#define LITERALS_MAX			10U
#define LITERAL_REQUESTS_MAX		20U

/*
 * Names the shared register that holds a constant word: the program's
 * literal request for it, added (one more shared register) when there is
 * none and the table has room; ENOSPC otherwise.
 */
static int
literal_register(
	struct backend_select *select,
	uint32_t bits,
	struct shader_machine_register *reg)
{
	struct powervr_shader_program *program;
	struct powervr_shader_request *request;
	unsigned literals;
	unsigned at;

	/* The request for the same word. */
	program = select->program;
	literals = 0;
	for (at = 0; at < program->request_count; at++) {
		request = &program->requests[at];
		if (request->kind != POWERVR_SHADER_REQUEST_LITERAL)
			continue;
		literals++;
		if (request->words == 1 && request->literal == bits) {
			*reg = bank_register(POWERVR_USC_BANK_SHARED, request->shared);
			return 0;
		}
	}

	/* A new one after the others, while the data sequencer's loads have room for the rest (one each). */
	if (program->request_count >= LITERAL_REQUESTS_MAX || literals >= LITERALS_MAX)
		return ENOSPC;
	request = &program->requests[program->request_count++];
	request->kind = POWERVR_SHADER_REQUEST_LITERAL;
	request->part = 0;
	request->words = 1;
	request->shared = (uint16_t)program->shareds;
	request->set = 0;
	request->binding = 0;
	request->array_index = 0;
	request->offset = 0;
	request->literal = bits;
	program->shareds++;
	*reg = bank_register(POWERVR_USC_BANK_SHARED, request->shared);
	return 0;
}

/*
 * Reports whether the four sources of a colour's store are the words of
 * one four-word value in order, unmodified, and names its first
 * register.
 */
static int
adjacent_words(
	const struct backend_select *select,
	const struct powervr_ir_instruction *instruction,
	struct shader_machine_register *reg)
{
	const struct powervr_ir_operand *operand;
	unsigned value;
	unsigned channel;

	/* A four-word value in the first source. */
	operand = &instruction->source[0];
	if (operand->kind != POWERVR_IR_OPERAND_VALUE || operand->value >= select->shader->value_count)
		return 0;
	value = operand->value;
	if (select->shader->values[value].width != 4)
		return 0;

	/* Its words in order, as they are. */
	for (channel = 0; channel < 4; channel++) {
		operand = &instruction->source[channel];
			if (operand->kind != POWERVR_IR_OPERAND_VALUE ||
		    operand->value != value ||
		    operand->component != channel ||
		    operand->modifiers != 0)
			return 0;
	}

	/* Succeeded: the value's first register. */
	*reg = temp_register(select->backend->value_register[value]);
	return 1;
}

/* Takes the next scratch temporary of the instruction being selected. */
static struct shader_machine_register
scratch_register(
	struct backend_select *select)
{
	struct shader_machine_register reg;
	unsigned index;

	/* Refuses an instruction that needs more scratch than there is. */
	if (select->scratch >= TEMP_SCRATCH_COUNT) {
		if (select->backend->error == 0)
			select->backend->error = ENOTSUP;
		reg = temp_register(TEMP_SCRATCH);

		/* The refusal is latched; the first scratch keeps the selection going. */
		return reg;
	}

	/* The next scratch temporary. */
	index = TEMP_SCRATCH + select->scratch++;
	reg = temp_register(index);

	/* Succeeded: the scratch temporary. */
	return reg;
}

/*
 * Selects a float add or multiply: the first source takes negate,
 * absolute and floor, the second only absolute, so a negated second
 * source is swapped, folded into a product's first, or moved.
 */
static void
select_float_pair(
	struct backend_select *select,
	unsigned kind,
	struct shader_machine_register destination,
	const struct powervr_ir_operand *a,
	const struct powervr_ir_operand *b,
	unsigned floor)
{
	struct shader_machine_operation *operation;
	struct shader_machine_register first;
	struct shader_machine_register second;
	struct shader_machine_register held;
	struct shader_machine_register moved;
	unsigned first_modifiers;
	unsigned second_modifiers;
	unsigned held_modifiers;
	unsigned swap;

	/* The sources; a floor adds zero. */
	first = operand_register(select, a, &first_modifiers);
	if (b != NULL) {
		second = operand_register(select, b, &second_modifiers);
	} else {
		second = bank_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_ZERO);
		second_modifiers = 0;
	}

	/* Two shared registers would need a move: a constant of the two is an immediate instead. */
	if (first.bank == POWERVR_USC_BANK_SHARED && second.bank == POWERVR_USC_BANK_SHARED && select->literals != 0) {
		select->literals = 0;
		if (b != NULL && b->kind == POWERVR_IR_OPERAND_CONSTANT) {
			second = operand_register(select, b, &second_modifiers);
		} else if (a->kind == POWERVR_IR_OPERAND_CONSTANT) {
			first = operand_register(select, a, &first_modifiers);
		}
		select->literals = 1;
	}

	/* A negated second source of a product moves its sign to the first. */
	if (kind == MACHINE_FLOAT_MULTIPLY && (second_modifiers & POWERVR_IR_NEGATE) != 0) {
		second_modifiers &= ~POWERVR_IR_NEGATE;
		first_modifiers ^= POWERVR_IR_NEGATE;
	}

	/* A negated second source of a sum swaps with an unnegated first, else is moved. */
	if ((second_modifiers & POWERVR_IR_NEGATE) != 0) {
		swap = 0;
		if ((first_modifiers & POWERVR_IR_NEGATE) == 0 && floor == 0)
			swap = 1;
		if (swap != 0) {
			held = first;
			held_modifiers = first_modifiers;
			first = second;
			first_modifiers = second_modifiers;
			second = held;
			second_modifiers = held_modifiers;
		} else {
			moved = scratch_register(select);
			operation = append(select, MACHINE_MOVE);
			if (operation != NULL) {
				operation->destination[0] = moved;
				operation->source[0] = second;
				operation->modifiers[0] = (uint8_t)second_modifiers;
			}
			second = moved;
			second_modifiers = 0;
		}
	}

	/*
	 * A shared register cannot be the second source (its field names
	 * no shared bank): it swaps with a first that is not shared and
	 * takes the second's modifiers, else it is moved.
	 */
	if (second.bank == POWERVR_USC_BANK_SHARED) {
		if (first.bank != POWERVR_USC_BANK_SHARED && floor == 0 && (first_modifiers & POWERVR_IR_NEGATE) == 0) {
			held = first;
			held_modifiers = first_modifiers;
			first = second;
			first_modifiers = second_modifiers;
			second = held;
			second_modifiers = held_modifiers;
		} else if (kind == MACHINE_FLOAT_ADD && floor == 0) {
			/* A sum: first * 1.0 + second, the shared register third (exact: the product is the first). */
			operation = append(select, MACHINE_FLOAT_MULTIPLY_ADD);
			if (operation != NULL) {
				operation->destination[0] = destination;
				operation->source[0] = first;
				operation->source[1] = bank_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_FLOAT_ONE);
				operation->source[2] = second;
				operation->modifiers[0] = (uint8_t)first_modifiers;
				operation->modifiers[2] = (uint8_t)second_modifiers;
			}
			return;
		} else {
			moved = scratch_register(select);
			operation = append(select, MACHINE_MOVE);
			if (operation != NULL) {
				operation->destination[0] = moved;
				operation->source[0] = second;
			}
			second = moved;
		}
	}

	/* The operation. */
	operation = append(select, kind);
	if (operation != NULL) {
		operation->destination[0] = destination;
		operation->source[0] = first;
		operation->source[1] = second;
		operation->modifiers[0] = (uint8_t)first_modifiers;
		operation->modifiers[1] = (uint8_t)second_modifiers;
		operation->variant = (uint8_t)floor;
	}
}

/* Selects an operation of two sources that may carry the allowed modifiers. */
static void
select_two_sourced(
	struct backend_select *select,
	unsigned kind,
	unsigned variant,
	uint32_t type,
	struct shader_machine_register destination,
	const struct powervr_ir_operand *a,
	const struct powervr_ir_operand *b,
	unsigned allowed)
{
	struct shader_machine_operation *operation;
	struct shader_machine_register first;
	struct shader_machine_register second;
	unsigned first_modifiers;
	unsigned second_modifiers;

	/* The sources with what modifiers they may keep. */
	first = operand_register(select, a, &first_modifiers);
	if ((first_modifiers & ~allowed) != 0) {
		first = operand_plain(select, a, allowed);
		first_modifiers = 0;
	}
	second = operand_register(select, b, &second_modifiers);
	if ((second_modifiers & ~allowed) != 0) {
		second = operand_plain(select, b, allowed);
		second_modifiers = 0;
	}

	/* The operation. */
	operation = append(select, kind);
	if (operation != NULL) {
		operation->destination[0] = destination;
		operation->source[0] = first;
		operation->source[1] = second;
		operation->modifiers[0] = (uint8_t)first_modifiers;
		operation->modifiers[1] = (uint8_t)second_modifiers;
		operation->variant = (uint8_t)variant;
		operation->immediate = type;
	}
}

/* Works out a memory access's address pair into the address temporaries. */
static void
select_address(
	struct backend_select *select,
	const struct powervr_ir_instruction *instruction)
{
	struct shader_machine_operation *operation;
	struct shader_machine_register low;
	struct shader_machine_register high;
	struct shader_machine_register offset;

	/* The base pair plus the byte offset, as a 64-bit add of a 32-bit word. */
	low = operand_plain(select, &instruction->source[0], 0);
	high = operand_plain(select, &instruction->source[1], 0);
	offset = operand_plain(select, &instruction->source[2], 0);
	operation = append(select, MACHINE_ADD64);
	if (operation != NULL) {
		operation->destination[0] = temp_register(TEMP_ADDRESS);
		operation->destination[1] = temp_register(TEMP_ADDRESS + 1U);
		operation->source[0] = low;
		operation->source[1] = high;
		operation->source[2] = offset;
	}
}

/*
 * Selects a sum of products writing an instruction's four floats
 * (sources 0 to 3) into its pixel output register in a form: each
 * channel's register, moved into a scratch temporary first when its
 * modifiers need applying or when its source's bank field cannot name
 * it.  Constants are read from the hardware's or the shared registers.
 */
static void
select_sum(
	struct backend_select *select,
	const struct powervr_ir_instruction *instruction,
	enum powervr_usc_sum form)
{
	struct shader_machine_operation *operation;
	struct shader_machine_register channels[4];
	struct shader_machine_register moved;
	unsigned channel;
	unsigned slot;

	/* Each channel's register, constants from the shared registers as a float operation reads them. */
	select->literals = 1;
	for (channel = 0; channel < 4; channel++)
		channels[channel] = operand_plain(select, &instruction->source[channel], 0);
	select->literals = 0;

	/* A channel in the second source of an operand word: a temporary unless its bank fits two bits. */
	for (slot = 1; slot < 6; slot += 3) {
		channel = sum_slots[form][slot];
		if (channel >= 4 || channels[channel].bank <= POWERVR_USC_BANK_COEFFICIENT)
			continue;
		moved = scratch_register(select);
		operation = append(select, MACHINE_MOVE);
		if (operation != NULL) {
			operation->destination[0] = moved;
			operation->source[0] = channels[channel];
		}
		channels[channel] = moved;
	}

	/* The operation into the pixel output under the overlap check. */
	operation = append(select, MACHINE_SUM_OF_PRODUCTS);
	if (operation != NULL) {
		operation->variant = (uint8_t)form;
		operation->overlap = 1;
		operation->destination[0] = bank_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_PIXEL_OUTPUT + instruction->immediate);
		for (channel = 0; channel < 4; channel++)
			operation->source[channel] = channels[channel];
	}
}

/*
 * Selects a sum of products in a half form when the instruction's four
 * floats are a sample's that returns half floats (sample_half): the
 * sample's two registers, red and green then blue and alpha, are the
 * operation's sources 0 and 1.  Reports whether it did.
 */
static int
select_sum_half(
	struct backend_select *select,
	const struct powervr_ir_instruction *instruction,
	enum powervr_usc_sum form)
{
	struct shader_machine_operation *operation;
	struct shader_machine_register pair;
	unsigned value;
	int found;

	/* A sample's four words, which it returns as half floats. */
	found = adjacent_words(select, instruction, &pair);
	if (!found)
		return 0;
	value = instruction->source[0].value;
	if (select->backend->sample_half[value] == 0)
		return 0;

	/* The operation into the pixel output under the overlap check. */
	operation = append(select, MACHINE_SUM_OF_PRODUCTS);
	if (operation != NULL) {
		operation->variant = (uint8_t)form;
		operation->overlap = 1;
		operation->destination[0] = bank_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_PIXEL_OUTPUT + instruction->immediate);
		operation->source[0] = pair;
		operation->source[1] = pair;
		operation->source[1].index = (uint16_t)(pair.index + 1U);
	}

	/* Succeeded: the half form. */
	return 1;
}

/* Appends one step of the execution mask counter, run on every lane. */
static void
select_condition(
	struct backend_select *select,
	unsigned kind,
	unsigned predicate,
	unsigned adjust,
	struct shader_machine_register source)
{
	struct shader_machine_operation *operation;

	/* The step reads and writes the counter. */
	operation = append(select, MACHINE_CONDITION);
	if (operation != NULL) {
		operation->variant = (uint8_t)kind;
		operation->condition = POWERVR_USC_RUN_IGNORING_ENABLE;
		operation->immediate = predicate;
		operation->immediate_2 = adjust;
		operation->destination[0] = temp_register(TEMP_EMC);
		operation->source[0] = source;
	}
}

/* Appends a branch whose target is set later; reports its index. */
static unsigned
select_branch(
	struct backend_select *select,
	unsigned prediction,
	unsigned condition)
{
	struct shader_machine_operation *operation;
	unsigned index;

	/* The branch, its target to be filled in. */
	index = select->backend->operation_count;
	operation = append(select, MACHINE_BRANCH);
	if (operation != NULL) {
		operation->variant = (uint8_t)prediction;
		operation->condition = (uint8_t)condition;
	}
	return index;
}

/* Hands over one group, its branch offset from the groups' starts. */
static int
build_group_fetch(
	void *context,
	size_t index,
	struct powervr_usc_group *group)
{
	const struct shader_backend *backend;
	const struct shader_machine_operation *operation;
	int32_t offset;
	int error;

	/* The group's first operation; a paired one builds the pair's group. */
	backend = context;
	index = backend->group_first[index];
	operation = &backend->operations[index];
	if (operation->pair != 0 && index + 1U < backend->operation_count)
		return pair_group(operation, &backend->operations[index + 1U], group);

	/* A branch's offset is from its own start to its target's. */
	offset = 0;
	if (operation->kind == MACHINE_BRANCH)
		offset = (int32_t)backend->offsets[operation->target] - (int32_t)backend->offsets[index];
	error = shader_backend_group(operation, offset, group);
	if (error != 0)
		return error;

	/* Succeeded: the operation's group. */
	return 0;
}

/* Copies a machine register into an operand of a group. */
static void
group_register(
	struct powervr_usc_register *operand,
	struct shader_machine_register reg)
{
	operand->used = 1;
	operand->bank = reg.bank;
	operand->index = reg.index;
}

/*
 * Builds the instruction group of one machine operation.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_map.py (group_map and encode_map
 * of O_MOVI32, O_MBYP, O_MOVS1 with O_MOVWM, O_FADD, O_FMUL, O_FMAD,
 * O_FRCP and the other single-source operations, O_MIN, O_MAX, O_BCMP,
 * O_BCSEL, O_TSTZ, O_IADD32, O_IMUL32 (O_IMADD32), O_ADD64_32,
 * O_LOGICAL with O_BBYP0S1, O_SHIFT with O_BBYP0BM, O_PCK, O_UNPCK,
 * O_LD, O_ST32 (O_ST), O_WDF, O_DITRP, O_SMP, O_UVSW_WRITE,
 * O_UVSW_EMIT_ENDTASK, O_ALPHAF, O_NOP, O_CNDST, O_CNDEF, O_CNDSM, O_CNDLT,
 * O_CNDEND, O_BR: the phases, the sources' slots, the internal
 * selectors, the write enables and the header fields) and
 * src/imagination/pco/pco_opt.c (a floor is an add of zero with the
 * floor modifier); the encoding variants are picked as the generated
 * encoder picks them (the first whose conditions hold, the extended
 * form otherwise; a conditional move's mask is all elements, so it is
 * always the extended form).
 */
int
shader_backend_group(
	const struct shader_machine_operation *operation,
	int32_t offset,
	struct powervr_usc_group *group)
{
	struct powervr_usc_operation part;
	unsigned negate_0;
	unsigned absolute_0;
	unsigned negate_1;
	unsigned absolute_1;
	unsigned negate_2;
	unsigned absolute_2;
	unsigned extended;
	unsigned length;
	unsigned slot;
	int error;

	/* Starts from a blank group under the operation's condition. */
	*group = blank_group;
	group->condition = operation->condition;
	negate_0 = (operation->modifiers[0] & POWERVR_IR_NEGATE) != 0;
	absolute_0 = (operation->modifiers[0] & POWERVR_IR_ABSOLUTE) != 0;
	negate_1 = (operation->modifiers[1] & POWERVR_IR_NEGATE) != 0;
	absolute_1 = (operation->modifiers[1] & POWERVR_IR_ABSOLUTE) != 0;
	negate_2 = (operation->modifiers[2] & POWERVR_IR_NEGATE) != 0;
	absolute_2 = (operation->modifiers[2] & POWERVR_IR_ABSOLUTE) != 0;
	error = 0;

	/* Builds each kind as the Mesa compiler lays it out. */
	switch (operation->kind) {
	case MACHINE_IMMEDIATE:
		/* Bitwise phase 0 with the 32-bit immediate, to the first write; it may end the program. */
		group->unit = POWERVR_USC_UNIT_BITWISE;
		group->write_0 = 1;
		group->end = operation->end;
		group_register(&group->destination[0], operation->destination[0]);
		error = powervr_usc_bit_immediate(operation->immediate, 1, 0, 2, 0, &group->operation[POWERVR_USC_PHASE_0]);
		break;
	case MACHINE_MOVE:
		/* Phase 0 passes s0 through with its modifiers. */
		group->unit = POWERVR_USC_UNIT_MAIN;
		group->write_0 = 1;
		group_register(&group->source[0], operation->source[0]);
		group_register(&group->destination[0], operation->destination[0]);
		group->internal[4] = SELECT_FT0;
		group->overlap_check = operation->variant;
		group->end = operation->end;
		error = powervr_usc_single(POWERVR_USC_SINGLE_PASS, negate_0, absolute_0, &group->operation[POWERVR_USC_PHASE_0]);
		break;
	case MACHINE_MOVE_SPECIAL:
		/* A special register reaches the move of phase 2 through s1 and the source selector. */
		group->unit = POWERVR_USC_UNIT_MAIN;
		group->write_0 = 1;
		group_register(&group->source[1], operation->source[0]);
		group_register(&group->destination[0], operation->destination[0]);
		group->mux_used = 1;
		group->mux = MUX_S1;
		group->internal[4] = SELECT_FTE;
		error = powervr_usc_move(SELECT_FT0, SELECT_FTE, 0xf, 1, 1, 1, &group->operation[POWERVR_USC_PHASE_2]);
		break;
	case MACHINE_FLOAT_ADD:
	case MACHINE_FLOAT_MULTIPLY:
		group->unit = POWERVR_USC_UNIT_MAIN;
		group->write_0 = 1;
		group_register(&group->source[0], operation->source[0]);
		group_register(&group->source[1], operation->source[1]);
		group_register(&group->destination[0], operation->destination[0]);
		group->internal[4] = SELECT_FT0;
		error = powervr_usc_float_pair(operation->kind == MACHINE_FLOAT_ADD ? (POWERVR_SHADER_LOW_PRECISION_TRIAL ? POWERVR_USC_FLOAT_ADD_LOW_PRECISION : POWERVR_USC_FLOAT_ADD) :
					       (POWERVR_SHADER_LOW_PRECISION_TRIAL ? POWERVR_USC_FLOAT_MULTIPLY_LOW_PRECISION : POWERVR_USC_FLOAT_MULTIPLY),
					       operation->saturate, negate_0, absolute_0, absolute_1, operation->variant, &group->operation[POWERVR_USC_PHASE_0]);
		break;
	case MACHINE_FLOAT_MULTIPLY_ADD:
		group->unit = POWERVR_USC_UNIT_MAIN;
		group->write_0 = 1;
		group_register(&group->source[0], operation->source[0]);
		group_register(&group->source[1], operation->source[1]);
		group_register(&group->source[2], operation->source[2]);
		group_register(&group->destination[0], operation->destination[0]);
		group->internal[4] = SELECT_FT0;
		error = powervr_usc_float_multiply_add(operation->saturate, negate_0, absolute_0, negate_1, absolute_1, negate_2, absolute_2, 0, POWERVR_SHADER_LOW_PRECISION_TRIAL, &group->operation[POWERVR_USC_PHASE_0]);
		break;
	case MACHINE_SINGLE:
		group->unit = POWERVR_USC_UNIT_MAIN;
		group->write_0 = 1;
		group_register(&group->source[0], operation->source[0]);
		group_register(&group->destination[0], operation->destination[0]);
		error = powervr_usc_single(operation->variant, negate_0, absolute_0, &group->operation[POWERVR_USC_PHASE_0]);
		break;
	case MACHINE_MINMAX:
		/* ft0 = a, ft1 = b; test is1 against is2; move ft0 where it holds, else ft1. */
		group->unit = POWERVR_USC_UNIT_MAIN;
		group->write_0 = 1;
		group_register(&group->source[0], operation->source[0]);
		group_register(&group->source[3], operation->source[1]);
		group_register(&group->destination[0], operation->destination[0]);
		group->internal[1] = 0;
		group->internal[2] = 0;
		group->internal[4] = SELECT_FT1;
		error = powervr_usc_single(POWERVR_USC_SINGLE_PASS, negate_0, absolute_0, &group->operation[POWERVR_USC_PHASE_0]);
		if (error == 0)
			error = powervr_usc_single(POWERVR_USC_SINGLE_PASS, negate_1, absolute_1, &group->operation[POWERVR_USC_PHASE_1]);
		if (error == 0)
			error = powervr_usc_move(SELECT_FT0, SELECT_FT0, 0xf, 0, 0, 1, &group->operation[POWERVR_USC_PHASE_2]);
		if (error == 0) {
			error = powervr_usc_test(operation->variant, operation->immediate, 0, 0, 1, 1, &part);
			if (error == 0 && group->operation[POWERVR_USC_PHASE_2].length + part.length <= POWERVR_USC_OPERATION_MAX) {
				for (length = 0; length < part.length; length++)
					group->operation[POWERVR_USC_PHASE_2].bytes[group->operation[POWERVR_USC_PHASE_2].length + length] = part.bytes[length];
				group->operation[POWERVR_USC_PHASE_2].length = (uint8_t)(group->operation[POWERVR_USC_PHASE_2].length + part.length);
			}
		}
		break;
	case MACHINE_COMPARE:
	case MACHINE_TEST_ZERO:
		/*
		 * ft0 = a (and ft1 = b for a comparison); the test; ft2 =
		 * the zero pack; move fte (the all-ones constant in s1) where
		 * the test holds, else ft2.  The zero test writes p0 and no
		 * register.
		 */
		group->unit = POWERVR_USC_UNIT_MAIN;
		group_register(&group->source[0], operation->source[0]);
		group->source[1].used = 1;
		group->source[1].bank = POWERVR_USC_BANK_SPECIAL;
		group->source[1].index = SPECIAL_TRUE;
		group->mux_used = 1;
		group->mux = MUX_S1;
		group->internal[1] = 0;
		group->internal[2] = 0;
		group->internal[4] = SELECT_FT2;
		error = powervr_usc_single(POWERVR_USC_SINGLE_PASS, negate_0, absolute_0, &group->operation[POWERVR_USC_PHASE_0]);
		if (error == 0 && operation->kind == MACHINE_COMPARE) {
			group->write_0 = 1;
			group_register(&group->source[3], operation->source[1]);
			group_register(&group->destination[0], operation->destination[0]);
			error = powervr_usc_single(POWERVR_USC_SINGLE_PASS, negate_1, absolute_1, &group->operation[POWERVR_USC_PHASE_1]);
		}
		if (error == 0)
			error = powervr_usc_move(SELECT_FT0, SELECT_FTE, 0xf, 0, 0, 1, &group->operation[POWERVR_USC_PHASE_2]);
		if (error == 0) {
			if (operation->kind == MACHINE_COMPARE) {
				extended = 1;
				if (operation->immediate == TYPE_F32 && operation->variant <= TEST_NOT_EQUAL)
					extended = 0;
				error = powervr_usc_test(operation->variant, operation->immediate, 0, 0, 0, extended, &part);
			} else {
				error = powervr_usc_test(TEST_ZERO, TYPE_U32, 0, 1, 0, 1, &part);
			}
		}
		if (error == 0 && group->operation[POWERVR_USC_PHASE_2].length + part.length <= POWERVR_USC_OPERATION_MAX) {
			for (length = 0; length < part.length; length++)
				group->operation[POWERVR_USC_PHASE_2].bytes[group->operation[POWERVR_USC_PHASE_2].length + length] = part.bytes[length];
			group->operation[POWERVR_USC_PHASE_2].length = (uint8_t)(group->operation[POWERVR_USC_PHASE_2].length + part.length);
		}
		if (error == 0)
			error = powervr_usc_pack(PACK_ZERO, 0, 0, 0, &part);
		if (error == 0 && group->operation[POWERVR_USC_PHASE_2].length + part.length <= POWERVR_USC_OPERATION_MAX) {
			for (length = 0; length < part.length; length++)
				group->operation[POWERVR_USC_PHASE_2].bytes[group->operation[POWERVR_USC_PHASE_2].length + length] = part.bytes[length];
			group->operation[POWERVR_USC_PHASE_2].length = (uint8_t)(group->operation[POWERVR_USC_PHASE_2].length + part.length);
		}
		break;
	case MACHINE_SELECT:
		/* ft0 = a, ft1 = b; test the condition (s1 through fte) for zero; move ft1 where it is, else ft0. */
		group->unit = POWERVR_USC_UNIT_MAIN;
		group->write_0 = 1;
		group_register(&group->source[0], operation->source[1]);
		group_register(&group->source[1], operation->source[0]);
		group_register(&group->source[3], operation->source[2]);
		group_register(&group->destination[0], operation->destination[0]);
		group->mux_used = 1;
		group->mux = MUX_S1;
		group->internal[1] = 1;
		group->internal[4] = SELECT_FT0;
		error = powervr_usc_single(POWERVR_USC_SINGLE_PASS, negate_1, absolute_1, &group->operation[POWERVR_USC_PHASE_0]);
		if (error == 0)
			error = powervr_usc_single(POWERVR_USC_SINGLE_PASS, negate_2, absolute_2, &group->operation[POWERVR_USC_PHASE_1]);
		if (error == 0)
			error = powervr_usc_move(SELECT_FT0, SELECT_FT1, 0xf, 0, 0, 1, &group->operation[POWERVR_USC_PHASE_2]);
		if (error == 0)
			error = powervr_usc_test(TEST_ZERO, TYPE_U32, 0, 0, 1, 1, &part);
		if (error == 0 && group->operation[POWERVR_USC_PHASE_2].length + part.length <= POWERVR_USC_OPERATION_MAX) {
			for (length = 0; length < part.length; length++)
				group->operation[POWERVR_USC_PHASE_2].bytes[group->operation[POWERVR_USC_PHASE_2].length + length] = part.bytes[length];
			group->operation[POWERVR_USC_PHASE_2].length = (uint8_t)(group->operation[POWERVR_USC_PHASE_2].length + part.length);
		}
		break;
	case MACHINE_INTEGER:
		group->unit = POWERVR_USC_UNIT_MAIN;
		group->write_0 = 1;
		group_register(&group->source[0], operation->source[0]);
		group_register(&group->source[1], operation->source[1]);
		group_register(&group->source[2], operation->source[2]);
		group_register(&group->destination[0], operation->destination[0]);
		group->internal[4] = SELECT_FT0;
		error = powervr_usc_integer(POWERVR_USC_INTEGER_MULTIPLY_ADD_32, 0, 0, 0, 0, 0, 0, 0, 0, &group->operation[POWERVR_USC_PHASE_0]);
		break;
	case MACHINE_ADD64:
		/* The low word from ft0, the high from fte. */
		group->unit = POWERVR_USC_UNIT_MAIN;
		group->write_0 = 1;
		group->write_1 = 1;
		group_register(&group->source[0], operation->source[0]);
		group_register(&group->source[1], operation->source[1]);
		group_register(&group->source[2], operation->source[2]);
		group_register(&group->destination[0], operation->destination[0]);
		group_register(&group->destination[1], operation->destination[1]);
		group->internal[4] = SELECT_FT0;
		group->internal[5] = SELECT_FTE;
		error = powervr_usc_integer(POWERVR_USC_INTEGER_ADD_64_FROM_32, 0, 0, 0, 0, 0, 0, 0, 0, &group->operation[POWERVR_USC_PHASE_0]);
		break;
	case MACHINE_LOGIC:
		/* Phase 0 passes s2 to ft2; phase 1 combines it with s3. */
		group->unit = POWERVR_USC_UNIT_BITWISE;
		group->write_0 = 1;
		group_register(&group->source[2], operation->source[0]);
		group_register(&group->source[3], operation->source[1]);
		group_register(&group->destination[0], operation->destination[0]);
		error = powervr_usc_bit_count_shift(0, 2, 0, 0, &group->operation[POWERVR_USC_PHASE_0]);
		if (error == 0)
			error = powervr_usc_bit_logic(operation->variant, 0, 0, &group->operation[POWERVR_USC_PHASE_1]);
		break;
	case MACHINE_SHIFT:
		/* Phase 0 passes s1; phase 2 shifts it by s4. */
		group->unit = POWERVR_USC_UNIT_BITWISE;
		group->write_0 = 1;
		group_register(&group->source[1], operation->source[0]);
		group_register(&group->source[4], operation->source[1]);
		group_register(&group->destination[0], operation->destination[0]);
		error = powervr_usc_bit_count_shift(0, 2, 0, 0, &group->operation[POWERVR_USC_PHASE_0]);
		if (error == 0)
			error = powervr_usc_bit_shift(operation->variant, 0, 0, 0, &group->operation[POWERVR_USC_PHASE_2]);
		break;
	case MACHINE_PACK:
		/* s2 reaches the pack through the source selector and is3; the result is ft2. */
		group->unit = POWERVR_USC_UNIT_MAIN;
		group->write_0 = 1;
		group_register(&group->source[2], operation->source[0]);
		group_register(&group->destination[0], operation->destination[0]);
		group->mux_used = 1;
		group->mux = MUX_S2;
		group->internal[3] = SELECT_FTE;
		group->internal[4] = SELECT_FT2;
		group->repeat = (uint8_t)operation->immediate;
		group->overlap_check = operation->overlap;
		error = powervr_usc_pack(operation->variant, 0, (operation->immediate_2 & PACK_ROUND_TO_ZERO) != 0, operation->immediate_2 & PACK_SCALE, &group->operation[POWERVR_USC_PHASE_2]);
		break;
	case MACHINE_UNPACK:
		group->unit = POWERVR_USC_UNIT_MAIN;
		group->write_0 = 1;
		group_register(&group->source[0], operation->source[0]);
		group_register(&group->destination[0], operation->destination[0]);
		group->internal[4] = SELECT_FT0;
		group->repeat = (uint8_t)operation->immediate;
		group->overlap_check = operation->overlap;
		error = powervr_usc_unpack(operation->variant, 0, operation->immediate_2 & PACK_SCALE, &group->operation[POWERVR_USC_PHASE_0]);
		break;
	case MACHINE_LOAD:
		/* The address pair in s0, the word returned into the register s3 names. */
		group->unit = POWERVR_USC_UNIT_MAIN;
		group_register(&group->source[0], operation->source[0]);
		group_register(&group->source[3], operation->destination[0]);
		error = powervr_usc_load(0, 1, 0, 0, 0, &group->operation[POWERVR_USC_PHASE_BACKEND]);
		break;
	case MACHINE_STORE:
		/* The data in s0, the address pair in s3. */
		group->unit = POWERVR_USC_UNIT_MAIN;
		group_register(&group->source[0], operation->source[0]);
		group_register(&group->source[3], operation->source[1]);
		error = powervr_usc_store(3, 1, 0, 0, SIZE_32, 0, 0, &group->operation[POWERVR_USC_PHASE_BACKEND]);
		break;
	case MACHINE_WAIT:
		/* Waits for data return counter 0. */
		group->unit = POWERVR_USC_UNIT_CONTROL;
		group->control = POWERVR_USC_CONTROL_WAIT_DATA;
		error = powervr_usc_empty(&group->operation[POWERVR_USC_PHASE_0]);
		break;
	case MACHINE_ITERATE:
		/* Variant + 1 words iterated with perspective from consecutive coefficient sets, through data return counter 0. */
		group->unit = POWERVR_USC_UNIT_CONTROL;
		group->control = CONTROL_ITERATE;
		error = powervr_usc_iterate(operation->destination[0].index, operation->immediate, 1, operation->immediate_2, 0, operation->variant + 1U, 0, &group->operation[POWERVR_USC_PHASE_0]);
		break;
	case MACHINE_ITERATE_SAMPLE:
		/* Two words iterated with perspective from consecutive coefficient sets, sampled with the shared registers' states, through data return counter 0. */
		group->unit = POWERVR_USC_UNIT_CONTROL;
		group->control = CONTROL_ITERATE_SAMPLE;
		error = powervr_usc_iterate_sample(operation->destination[0].index, operation->immediate, 1, operation->immediate_2, operation->source[0].index,
						   operation->source[2].index, 0, DIMENSIONS_2D, 4, 1, 0, operation->variant, operation->half, &group->operation[POWERVR_USC_PHASE_0]);
		break;
	case MACHINE_SAMPLE:
		/* Image state in s0, coordinates in s1, sampler state in s2, the four words into s4's registers. */
		group->unit = POWERVR_USC_UNIT_MAIN;
		group_register(&group->source[0], operation->source[0]);
		group_register(&group->source[1], operation->source[1]);
		group_register(&group->source[2], operation->source[2]);
		group_register(&group->source[4], operation->destination[0]);
		error = powervr_usc_sample(1, 0, DIMENSIONS_2D, 4, 0, &group->operation[POWERVR_USC_PHASE_BACKEND]);
		break;
	case MACHINE_VERTEX_WRITE:
		/* s0 reaches the first write port through the source selector and fte. */
		group->unit = POWERVR_USC_UNIT_MAIN;
		group_register(&group->source[0], operation->source[0]);
		group->mux_used = 1;
		group->mux = 0;
		group->internal[4] = SELECT_FTE;
		error = powervr_usc_vertex_output(0, 0, 0, 1, operation->immediate, &group->operation[POWERVR_USC_PHASE_BACKEND]);
		break;
	case MACHINE_SUM_OF_PRODUCTS:
		/* Phase 0's sum of products of the form, its registers in the form's sources, into the pixel output under the overlap check. */
		group->unit = POWERVR_USC_UNIT_MAIN;
		group->write_0 = 1;
		group->overlap_check = operation->overlap;
		group->end = operation->end;
		group->internal_absent = 1;
		if (operation->variant >= POWERVR_USC_SUM_COUNT) {
			error = EINVAL;
			break;
		}
		for (slot = 0; slot < 6U; slot++) {
			if (sum_slots[operation->variant][slot] == SUM_DESTINATION)
				group_register(&group->source[slot], operation->destination[0]);
			else if (sum_slots[operation->variant][slot] < 4U)
				group_register(&group->source[slot], operation->source[sum_slots[operation->variant][slot]]);
		}
		group_register(&group->destination[0], operation->destination[0]);
		error = powervr_usc_sum_of_products((enum powervr_usc_sum)operation->variant, &group->operation[POWERVR_USC_PHASE_0]);
		break;
	case MACHINE_FEEDBACK:
		/* The alpha test's feedback with sources zero, zero and 7 (the test always passes), no predicate written, under the overlap check. */
		group->unit = POWERVR_USC_UNIT_MAIN;
		group->overlap_check = 1;
		group->source[0].used = 1;
		group->source[0].bank = POWERVR_USC_BANK_SPECIAL;
		group->source[0].index = SPECIAL_ZERO;
		group->source[1].used = 1;
		group->source[1].bank = POWERVR_USC_BANK_SPECIAL;
		group->source[1].index = SPECIAL_ZERO;
		group->source[2].used = 1;
		group->source[2].bank = POWERVR_USC_BANK_SPECIAL;
		group->source[2].index = SPECIAL_SEVEN;
		error = powervr_usc_visibility_test(0, 1, 0, &group->operation[POWERVR_USC_PHASE_BACKEND]);
		break;
	case MACHINE_VERTEX_END:
		group->unit = POWERVR_USC_UNIT_MAIN;
		group->end = 1;
		error = powervr_usc_vertex_output(5, 0, 0, 0, 0, &group->operation[POWERVR_USC_PHASE_BACKEND]);
		break;
	case MACHINE_END:
		group->unit = POWERVR_USC_UNIT_CONTROL;
		group->control = POWERVR_USC_CONTROL_NOTHING;
		group->control_misc = 1;
		error = powervr_usc_nothing(&group->operation[POWERVR_USC_PHASE_0]);
		break;
	case MACHINE_CONDITION:
		/* The counter in s0 (and a set's value in s2), constant zero in s3, the counter written back. */
		group->unit = POWERVR_USC_UNIT_CONTROL;
		group->control = POWERVR_USC_CONTROL_CONDITIONAL;
		group->write_0 = 1;
		group_register(&group->source[0], operation->source[0]);
		group->source[3].used = 1;
		group->source[3].bank = POWERVR_USC_BANK_SPECIAL;
		group->source[3].index = SPECIAL_ZERO;
		if (operation->variant == CONDITION_SET)
			group_register(&group->source[2], operation->source[1]);
		group_register(&group->destination[0], operation->destination[0]);
		error = powervr_usc_conditional(operation->variant, operation->immediate, operation->immediate_2, &group->operation[POWERVR_USC_PHASE_0]);
		break;
	case MACHINE_BRANCH:
		group->unit = POWERVR_USC_UNIT_CONTROL;
		group->control = POWERVR_USC_CONTROL_BRANCH;
		error = powervr_usc_branch(offset, 0, 0, operation->variant, &group->operation[POWERVR_USC_PHASE_0]);
		break;
	default:
		return EINVAL;
	}

	/* Reports a field the encoder refused. */
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}
