/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Mali compiler's lowering (shader.h): from the shared scalar IR to
 * a list of Valhall instructions, which va-schedule.c then gives its
 * waits and writes as code.
 *
 * Every IR value lives in a register below those the stage's hardware
 * preloads (r55 for compute, r57 for fragment, r59 for vertex programs),
 * for one lane of the warp; a constant takes no register of its own but
 * is read from the constant table or made again at each reader.  A
 * value gets the lowest free register when it is defined and gives it
 * back after the last instruction that reads it; a value made before a
 * loop and read in it lives to the loop's end, and a loop variable keeps
 * its register through every MOVE into it.  A message's results land in
 * a block of consecutive registers (aligned to two, or to four for three
 * or more), and a message that reads a vector reads it from such a block
 * the values are copied into.  Every arithmetic source is a register or
 * an entry of the constant table: other constants are made with
 * IADD_IMM, uniforms are moved from the FAU.
 *
 * The IR runs both sides of a selection, so the program is straight
 * code: a loop is the one backward branch, and a predicated memory access
 * the one forward branch, around the access.
 *
 * A vertex shader is lowered twice from its IR: once keeping only the
 * position (the position program) and once keeping only the generic
 * varyings (the varying program).
 */

#include "drivers/gpu/mali/compiler/shader.h"

#include "drivers/gpu/mali/compiler/va-private.h"

#include "drivers/gpu/compiler/optimize.h"
#include "drivers/gpu/compiler/spirv.h"

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include <stddef.h>
#include <stdint.h>

/* A value without a register. */
#define VA_NO_REG		0xFFU

/* A value not yet defined, and the end of the program as a last use. */
#define VA_NOT_DEFINED		UINT32_MAX


/* The deepest loop nest. */
#define VA_LOOP_DEPTH		16U

/* The registers one IR instruction's constants may take while it is lowered. */
#define VA_MAX_TEMPS		16U

/* The most values one IR instruction reads. */
#define VA_MAX_SOURCES		DRV_GPU_IR_TEXTURE_MAX_PARAMS

/* An operand that is not a register: an entry of the constant table, or nothing. */
#define VA_OPERAND_IMMEDIATE	0x100U
#define VA_OPERAND_NONE		0xFFFFU

/* The variants of a vertex shader, and the one program of the other stages. */
#define VA_VARIANT_MAIN		0U
#define VA_VARIANT_POSITION	1U
#define VA_VARIANT_VARYING	2U

/*
 * Floats the lowering needs as constants: 0.5, 1.0, 2^24, the largest
 * 1 / w the viewport transform keeps (2^15) and its negative,
 * 1 - 2^-20, 0.25, 1 / (2 pi) and 2 pi.
 *
 * Values referenced: IEEE 754-2019 (binary32 encodings).
 */
#define VA_FLOAT_HALF		0x3F000000U
#define VA_FLOAT_ONE		0x3F800000U
#define VA_FLOAT_2_POW_24	0x4B800000U
#define VA_FLOAT_W_RECIP_MAX	0x47000000U
#define VA_FLOAT_W_RECIP_MIN	0xC7000000U
#define VA_FLOAT_ONE_LESS_2_POW_20	0x3F7FFFF0U
#define VA_FLOAT_QUARTER	0x3E800000U
#define VA_FLOAT_INV_2PI	0x3E22F983U
#define VA_FLOAT_2PI		0x40C90FDBU

/*
 * Floats a cube sample with derivatives scales its level by: 1/16 and
 * 16.
 *
 * Values referenced: IEEE 754-2019 (binary32 encodings).
 */
#define VA_FLOAT_SIXTEENTH	0x3D800000U
#define VA_FLOAT_SIXTEEN	0x41800000U

/*
 * The Taylor coefficients of sin(y) = y (1 + c3 y^2 + c5 y^4 + ... + c11
 * y^10): -1/3!, 1/5!, -1/7!, 1/9!, -1/11!, whose error on [0, pi / 2] is
 * below 6e-7.
 *
 * Values referenced: the Taylor series of the sine (binary32 encodings
 * of the reciprocal factorials).
 */
#define VA_FLOAT_SIN_C3		0xBE2AAAABU
#define VA_FLOAT_SIN_C5		0x3C088889U
#define VA_FLOAT_SIN_C7		0xB9500D01U
#define VA_FLOAT_SIN_C9		0x3638EF1DU
#define VA_FLOAT_SIN_C11	0xB2D7322BU

/* The sign bit of a float. */
#define VA_SIGN_BIT		0x80000000U

/*
 * The largest 1 / w the viewport transform keeps (2^15), so a w near 0
 * keeps the precision Vulkan asks for.
 *
 * Value referenced: Mesa 25.0.7
 * src/compiler/nir/nir_lower_viewport_transform.c (MAX_W_RECIP_ABS).
 */

/* The byte offset of the attribute record within a vertex's output (the point size). */
#define VA_ATTRIBUTE_RECORD	4U

/* The kinds of sampler message: a filtered sample, a texel load and a gather. */
#define VA_TEXTURE_SAMPLE	0U
#define VA_TEXTURE_FETCH	1U
#define VA_TEXTURE_GATHER	2U

/* The bits of a cube's s coordinate below its face (bits 31:29). */
#define VA_CUBE_COORDINATE_MASK	0x1FFFFFFFU

/*
 * The 32-bit words of thread local storage a program may keep spilled
 * values in (1 KiB a thread), no word, and the least bytes a thread's
 * storage has.
 *
 * Value referenced: Mesa 26.2.6 src/panfrost/lib/pan_scratch.c (a
 * thread's stack is a power of two of at least 16 bytes).
 */
#define VA_TLS_WORDS		256U
#define VA_NO_TLS_WORD		UINT32_MAX
#define VA_TLS_MIN_BYTES	16U

/*
 * One lowering in progress: the IR, the program it describes, the list of
 * instructions, each value's register (or its constant: a constant is made
 * again in a temporary register at each reader, or read from the constant
 * table), each value's last use, the free registers and the first one the
 * stage's preloads leave alone, the temporaries of the instruction,
 * the outputs the program writes at its end (a fragment program's depth,
 * sample mask and stencil reference among them), the loops it is in (their heads' places
 * in the list and in the IR), and the compile's MALI_SHADER_OPTION_*.
 *
 * A value the registers cannot keep is spilled to a word of thread local
 * storage (tls_word, VA_NO_TLS_WORD for none) and loaded again where it
 * is read: pinned marks the values a register must keep (a loop
 * variable, which every MOVE writes, and a predicated load's result),
 * touched the step (one a lowered IR instruction, step counting them)
 * that last handed out a value's register, which may not be spilled
 * while that step lasts; sources lists the values the instruction being
 * lowered reads; tls_used marks the words in use, tls_words counts the
 * words ever used; in_branch is nonzero inside a block that only some
 * lanes run, where nothing may be spilled or loaded again; loop_reserve
 * is the registers a loop's head makes free by spilling.
 */
struct va_compiler {
	const struct drv_gpu_shader_ir *ir;
	struct mali_shader_program *program;
	uint32_t variant;
	struct va_code code;

	uint8_t *reg;
	uint8_t *is_constant;
	uint32_t *constant;
	uint32_t *last_use;
	uint32_t *defined_at;
	uint8_t *done;

	uint64_t free_regs;
	uint64_t used_regs;
	uint32_t reg_limit;
	uint32_t temps[VA_MAX_TEMPS];
	uint32_t temp_count;
	uint32_t preload;

	uint32_t output_value[MALI_SHADER_LOCATIONS][4];
	uint32_t position_value[4];
	uint32_t point_size_value;
	uint32_t depth_value;
	uint32_t sample_mask_value;
	uint32_t stencil_value;
	uint32_t options;

	uint32_t loop_start[VA_LOOP_DEPTH];
	uint32_t loop_head[VA_LOOP_DEPTH];
	uint32_t loop_depth;

	uint32_t *tls_word;
	uint8_t *pinned;
	uint32_t *touched;
	uint32_t step;
	uint32_t sources[VA_MAX_SOURCES];
	uint32_t source_count;
	uint8_t tls_used[VA_TLS_WORDS];
	uint32_t tls_words;
	uint32_t in_branch;
	uint32_t loop_reserve;

	int error;
	const char *reason;
	uint32_t at;
};

/*
 * One sampler message as the lowering reads it: its kind (VA_TEXTURE_*),
 * whether it compares with a depth reference, whether its image is a
 * cube and an array, its dimension and the coordinate parameters it has
 * (a cube's three of the direction), its level of detail mode, whether
 * it gives the coordinates' derivatives (each coordinate is then
 * followed by its x and y derivatives, so stride is the parameters a
 * coordinate takes), the places among its parameters of the first
 * coordinate, the reference, the level (or bias) and the layer
 * (VA_NOT_DEFINED for none), a gather's channel, the constant texel
 * offset, and the parameters' registers.
 */
struct va_texture {
	uint32_t kind;
	uint32_t compare;
	uint32_t cube;
	uint32_t array;
	uint32_t dimension;
	uint32_t coordinates;
	uint32_t lod_mode;
	uint32_t gradient;
	uint32_t stride;
	uint32_t first;
	uint32_t ref_slot;
	uint32_t lod_slot;
	uint32_t layer_slot;
	uint32_t channel;
	uint32_t offset;
	uint32_t parameter_count;
	uint32_t parameters[DRV_GPU_IR_TEXTURE_MAX_PARAMS];
};

/* The texture and sampler handle's table and index bits for texture slot k. */
static uint32_t texture_handle(uint32_t slot);

static int compile_variant(const struct drv_gpu_shader_ir *ir, struct mali_shader_program *program, uint32_t variant, uint32_t options, uint8_t *out, size_t room, size_t *length, uint32_t *work_registers, uint32_t *preload, struct mali_shader_diagnostic *diagnostic);
static int lay_out(const struct drv_gpu_shader_ir *ir, struct mali_shader_program *program);
static uint32_t buffer_of(const struct drv_gpu_shader_ir *ir, const struct mali_shader_program *program, uint32_t uniform);
static void lower_size_query(struct va_compiler *c, const struct drv_gpu_shader_ir_inst *inst);
static int optimize_for(const struct drv_gpu_shader_ir *ir, uint32_t variant, struct drv_gpu_shader_ir *optimized);
static int lower(struct va_compiler *c);
static int lower_with_room(struct va_compiler *c);
static void make_loop_room(struct va_compiler *c);
static int read_in_loop(const struct va_compiler *c, uint32_t value, uint32_t begin, uint32_t end);
static void lower_instruction(struct va_compiler *c, uint32_t index);
static void lower_end(struct va_compiler *c);
static uint32_t ir_sources(const struct drv_gpu_shader_ir_inst *inst, uint32_t *sources);
static uint32_t ir_defines(const struct drv_gpu_shader_ir_inst *inst);
static int compute_liveness(struct va_compiler *c);
static void fail(struct va_compiler *c, int error, const char *reason);
static uint32_t alloc_block(struct va_compiler *c, uint32_t count);
static uint32_t find_block(struct va_compiler *c, uint32_t count);
static int spill_one(struct va_compiler *c);
static int spill_candidate(const struct va_compiler *c, uint32_t value);
static uint32_t reload_value(struct va_compiler *c, uint32_t value);
static void reload_before_branch(struct va_compiler *c, uint32_t value);
static void mark_pinned(struct va_compiler *c);
static uint64_t tls_access(uint32_t word);
static void free_reg(struct va_compiler *c, uint32_t reg);
static uint32_t value_reg(struct va_compiler *c, uint32_t value);
static uint32_t value_operand(struct va_compiler *c, uint32_t value);
static uint32_t constant_temp(struct va_compiler *c, uint32_t value);
static void release_temps(struct va_compiler *c);
static uint32_t define_value(struct va_compiler *c, uint32_t value);
static void bind_value(struct va_compiler *c, uint32_t value, uint32_t reg);
static void release_after(struct va_compiler *c, uint32_t index);
static uint8_t operand_byte(uint32_t operand);
static int is_immediate(uint32_t operand);
static uint64_t operand_mask(uint32_t operand);
static void emit(struct va_compiler *c, uint64_t bits, uint64_t reads, uint64_t writes, uint32_t flags);
static void emit_alu(struct va_compiler *c, uint64_t bits, uint32_t dst, uint32_t a, uint32_t b, uint32_t third);
static void emit_constant(struct va_compiler *c, uint32_t dst, uint32_t value);
static uint32_t temp_constant(struct va_compiler *c, uint32_t value);
static void emit_move(struct va_compiler *c, uint32_t dst, uint32_t src);
static void emit_move_uniform(struct va_compiler *c, uint32_t dst, uint32_t word);
static void emit_fmul(struct va_compiler *c, uint32_t dst, uint32_t a, uint32_t b);
static void emit_fma(struct va_compiler *c, uint32_t dst, uint32_t a, uint32_t b, uint32_t addend);
static void emit_log2(struct va_compiler *c, uint32_t dst, uint32_t a);
static void emit_exp2(struct va_compiler *c, uint32_t dst, uint32_t a);
static void emit_shift(struct va_compiler *c, uint64_t op, uint32_t dst, uint32_t a, uint32_t amount);
static void emit_sine(struct va_compiler *c, uint32_t dst, uint32_t a, uint32_t turn_offset);
static void emit_derivative(struct va_compiler *c, uint32_t op, uint32_t dst, uint32_t a);
static void emit_divide(struct va_compiler *c, uint32_t op, uint32_t dst, uint32_t n, uint32_t d);
static void emit_unsigned_divide(struct va_compiler *c, uint32_t quotient, uint32_t remainder, uint32_t n, uint32_t d);
static void lower_alu(struct va_compiler *c, const struct drv_gpu_shader_ir_inst *inst);
static void lower_load_input(struct va_compiler *c, uint32_t index);
static void lower_vertex_attribute(struct va_compiler *c, uint32_t index);
static void lower_varying(struct va_compiler *c, uint32_t index);
static void lower_special_input(struct va_compiler *c, const struct drv_gpu_shader_ir_inst *inst);
static void lower_load_ubo(struct va_compiler *c, uint32_t index);
static uint32_t ubo_run(const struct va_compiler *c, uint32_t index);
static uint32_t buffer_address(struct va_compiler *c, uint32_t uniform, uint32_t offset_reg);
static void lower_storage(struct va_compiler *c, const struct drv_gpu_shader_ir_inst *inst);
static void lower_atomic(struct va_compiler *c, const struct drv_gpu_shader_ir_inst *inst);
static void lower_system(struct va_compiler *c, const struct drv_gpu_shader_ir_inst *inst);
static void lower_texture(struct va_compiler *c, const struct drv_gpu_shader_ir_inst *inst);
static void read_texture(struct va_compiler *c, const struct drv_gpu_shader_ir_inst *inst, struct va_texture *texture);
static void emit_cube_coordinates(struct va_compiler *c, uint32_t block, uint32_t x, uint32_t y, uint32_t z);
static uint32_t emit_gradient_descriptor(struct va_compiler *c, const struct va_texture *texture, uint32_t slot);
static uint32_t emit_cube_gradient_level(struct va_compiler *c, const struct va_texture *texture, uint32_t slot);
static void emit_cube_major(struct va_compiler *c, uint32_t dst, const uint32_t *axes, uint32_t z_major, uint32_t not_x_major);
static void lower_image(struct va_compiler *c, const struct drv_gpu_shader_ir_inst *inst);
static uint32_t image_coordinates(struct va_compiler *c, const struct drv_gpu_shader_ir_inst *inst);
static uint32_t image_address(struct va_compiler *c, uint32_t slot, uint32_t coordinates);
static uint32_t image_slot(struct va_compiler *c, uint32_t uniform);
static uint32_t image_register_format(uint32_t format);
static uint32_t image_components(uint32_t format);
static void lower_store_output(struct va_compiler *c, const struct drv_gpu_shader_ir_inst *inst);
static uint32_t texture_slot(struct va_compiler *c, uint32_t set, uint32_t binding);
static uint32_t buffer_slot(struct va_compiler *c, uint32_t uniform);
static uint32_t input_run(struct va_compiler *c, uint32_t index, uint32_t *last);
static void bind_run(struct va_compiler *c, uint32_t index, uint32_t last, uint32_t block, uint32_t count);
static uint32_t gather_block(struct va_compiler *c, const uint32_t *values, uint32_t count, uint32_t fill);
static uint32_t output_address(struct va_compiler *c, uint32_t index_offset);
static void lower_position_end(struct va_compiler *c);
static void lower_varying_end(struct va_compiler *c);
static void lower_fragment_end(struct va_compiler *c);
static void branch_begin(struct va_compiler *c, uint32_t predicate, uint32_t *branch);
static void branch_end(struct va_compiler *c, uint32_t branch);
static int noperspective_output(const struct drv_gpu_shader_ir *ir, uint32_t location);

/*
 * Compiles a SPIR-V module's stage.
 */
int
mali_shader_compile(
	const uint32_t *words,
	size_t word_count,
	uint32_t stage,
	const struct mali_shader_options *options,
	struct mali_shader_program **program,
	struct mali_shader_diagnostic *diagnostic)
{
	struct drv_gpu_compile_diagnostic parse_diagnostic;
	struct drv_gpu_shader_ir *ir;
	enum drv_gpu_shader_stage ir_stage;
	int error;

	/* Starts with no program and no refusal. */
	if (program != NULL)
		*program = NULL;
	if (diagnostic != NULL)
		kern_memset(diagnostic, 0, sizeof(*diagnostic));

	/* A module without words, or nowhere to put the program, is refused. */
	if (words == NULL || program == NULL)
		return EINVAL;

	/* Chooses the frontend's stage. */
	if (stage == MALI_SHADER_COMPUTE) {
		ir_stage = DRV_GPU_STAGE_COMPUTE;
	} else if (stage == MALI_SHADER_VERTEX) {
		ir_stage = DRV_GPU_STAGE_VERTEX;
	} else if (stage == MALI_SHADER_FRAGMENT) {
		ir_stage = DRV_GPU_STAGE_FRAGMENT;
	} else {
		/* Another stage is not one this compiler takes. */
		return EINVAL;
	}

	/* Parses the stage, with the images (a compute shader's included) the compiler lowers. */
	ir = NULL;
	kern_memset(&parse_diagnostic, 0, sizeof(parse_diagnostic));
	error = drv_gpu_shader_parse_features(words, word_count, ir_stage, MALI_SHADER_SPIRV_FEATURES, &ir, &parse_diagnostic);
	if (error != 0) {
		if (diagnostic != NULL) {
			diagnostic->instruction = parse_diagnostic.word_offset;
			diagnostic->reason = parse_diagnostic.reason;
		}

		/* Reports why the stage could not be parsed. */
		return error;
	}

	/* Compiles the parsed program. */
	error = mali_shader_compile_ir(ir, options, program, diagnostic);
	drv_gpu_shader_ir_free(ir);

	/* Reports the lowering's refusal. */
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the program. */
	return 0;
}

/*
 * Compiles a parsed program.
 */
int
mali_shader_compile_ir(
	const struct drv_gpu_shader_ir *ir,
	const struct mali_shader_options *options,
	struct mali_shader_program **program,
	struct mali_shader_diagnostic *diagnostic)
{
	struct mali_shader_program *made;
	uint8_t *code;
	size_t room;
	size_t first;
	size_t second;
	uint32_t work;
	uint32_t preload;
	uint32_t flags;
	int error;

	/* Starts with no program and no refusal. */
	if (program != NULL)
		*program = NULL;
	if (diagnostic != NULL)
		kern_memset(diagnostic, 0, sizeof(*diagnostic));

	/* A missing IR or result is refused. */
	if (ir == NULL || program == NULL)
		return EINVAL;

	/* Takes the options asked for (none without a record). */
	flags = 0U;
	if (options != NULL)
		flags = options->flags;

	/* Allocates the program's description. */
	made = kern_calloc(1U, sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* Chooses the program's stage. */
	if (ir->stage == DRV_GPU_STAGE_COMPUTE) {
		made->stage = MALI_SHADER_COMPUTE;
	} else if (ir->stage == DRV_GPU_STAGE_VERTEX) {
		made->stage = MALI_SHADER_VERTEX;
	} else if (ir->stage == DRV_GPU_STAGE_FRAGMENT) {
		made->stage = MALI_SHADER_FRAGMENT;
	} else {
		kern_free(made);

		/* A geometry shader has no Valhall program here. */
		return ENOTSUP;
	}

	/* Lays out the FAU words, the buffers and the textures. */
	error = lay_out(ir, made);
	if (error != 0) {
		kern_free(made);
		if (diagnostic != NULL)
			diagnostic->reason = "too many buffers or textures";

		/* Reports the refused layout. */
		return error;
	}

	/* Allocates room for the code of both variants. */
	room = (size_t)ir->instruction_count * 512U + 8192U;
	code = kern_malloc(room);
	if (code == NULL) {
		kern_free(made);
		return ENOMEM;
	}

	/* Compiles the main program, or a vertex shader's position program. */
	first = 0U;
	if (ir->stage == DRV_GPU_STAGE_VERTEX) {
		error = compile_variant(ir, made, VA_VARIANT_POSITION, flags, code, room, &first, &work, &preload, diagnostic);
	} else {
		error = compile_variant(ir, made, VA_VARIANT_MAIN, flags, code, room, &first, &work, &preload, diagnostic);
	}
	if (error != 0) {
		kern_free(code);
		kern_free(made);

		/* Reports why the program was refused. */
		return error;
	}
	made->work_registers = work;
	made->preload = preload;

	/* Compiles a vertex shader's varying program after the position program, when it writes varyings. */
	second = 0U;
	if (ir->stage == DRV_GPU_STAGE_VERTEX && made->varying_mask != 0U) {
		error = compile_variant(ir, made, VA_VARIANT_VARYING, flags, code + first, room - first, &second, &work, &preload, diagnostic);
		if (error != 0) {
			kern_free(code);
			kern_free(made);

			/* Reports why the varying program was refused. */
			return error;
		}
		made->varying_offset = first;
		made->varying_work_registers = work;
		made->varying_preload = preload;
	}

	/* Publishes the code. */
	made->code = code;
	made->length = first + second;

	/* Succeeded: the caller owns the program. */
	*program = made;
	return 0;
}

/*
 * Frees a compiled program.
 */
void
mali_shader_program_free(
	struct mali_shader_program *program)
{
	/* Nothing to free. */
	if (program == NULL)
		return;

	/* Frees the code and the description. */
	kern_free(program->code);
	kern_free(program);
}

/* Compiles one program of the shader into out. */
static int
compile_variant(
	const struct drv_gpu_shader_ir *ir,
	struct mali_shader_program *program,
	uint32_t variant,
	uint32_t options,
	uint8_t *out,
	size_t room,
	size_t *length,
	uint32_t *work_registers,
	uint32_t *preload,
	struct mali_shader_diagnostic *diagnostic)
{
	struct drv_gpu_shader_ir optimized;
	struct va_compiler *c;
	size_t written;
	size_t bound;
	uint32_t highest;
	uint32_t bytes;
	int error;

	/* Optimizes a private copy for what this program writes. */
	kern_memset(&optimized, 0, sizeof(optimized));
	error = optimize_for(ir, variant, &optimized);
	if (error != 0)
		return error;

	/* Allocates the lowering's state. */
	c = kern_calloc(1U, sizeof(*c));
	if (c == NULL) {
		kern_free(optimized.instructions);
		return ENOMEM;
	}
	c->program = program;
	c->variant = variant;
	c->options = options;
	if (optimized.instructions != NULL) {
		c->ir = &optimized;
	} else {
		c->ir = ir;
	}

	/* Lowers the program, then the parsed one if the rewritten one was refused for any reason but memory. */
	error = lower_with_room(c);
	if (error != 0 && error != ENOMEM && c->ir != ir) {
		va_code_release(&c->code);
		kern_memset(c, 0, sizeof(*c));
		c->program = program;
		c->variant = variant;
		c->options = options;
		c->ir = ir;
		error = lower_with_room(c);
	}
	if (error != 0) {
		if (diagnostic != NULL) {
			diagnostic->instruction = c->at;
			diagnostic->reason = c->reason;
		}
		va_code_release(&c->code);
		kern_free(c);
		kern_free(optimized.instructions);

		/* Reports why the program was refused. */
		return error;
	}

	/* Schedules and writes the program, when its bound fits the room. */
	written = 0U;
	bound = va_schedule_bound(&c->code);
	if (bound <= room)
		written = va_schedule_write(&c->code, out, room);
	if (written == 0U) {
		va_code_release(&c->code);
		kern_free(c);
		kern_free(optimized.instructions);
		if (diagnostic != NULL)
			diagnostic->reason = "the program does not fit its code buffer";

		/* Reports the program as too large. */
		return ENOMEM;
	}

	/* Counts the registers up to the highest one touched or preloaded. */
	highest = 0U;
	while (highest < VA_REGISTERS && ((c->used_regs >> highest) != 0U))
		highest++;

	/* Reports the program's size, registers and preloads. */
	*length = written;
	*work_registers = highest;
	*preload = c->preload;

	/* A thread's spill words, as a power of two of at least the least storage, for every program of the shader. */
	if (c->tls_words != 0U) {
		bytes = VA_TLS_MIN_BYTES;
		while (bytes < c->tls_words * 4U)
			bytes *= 2U;
		if (bytes > program->tls_bytes)
			program->tls_bytes = bytes;
	}

	/* Succeeded: the program is written. */
	va_code_release(&c->code);
	kern_free(c);
	kern_free(optimized.instructions);
	return 0;
}

/* Lays out the FAU words, buffers, textures and interface masks of the program. */
static int
lay_out(
	const struct drv_gpu_shader_ir *ir,
	struct mali_shader_program *program)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t word;
	uint32_t index;
	uint32_t location;
	uint32_t slot;
	uint32_t message;
	uint32_t sized;

	/* The push constants come first, and the next words start on an even word. */
	program->push_bytes = ir->push_bytes;
	word = (ir->push_bytes + 3U) / 4U;
	word = (word + 1U) & ~1U;

	/* A vertex program's viewport transform follows them (six words, kept even). */
	if (ir->stage == DRV_GPU_STAGE_VERTEX) {
		program->viewport_fau_word = word;
		word += 6U;
	}

	/* Each uniform buffer and storage buffer takes a 64-bit address; each sampled image a texture slot. */
	for (index = 0U; index < ir->uniform_count; index++) {
		uniform = &ir->uniforms[index];
		if (uniform->kind == DRV_GPU_IR_UNIFORM_BLOCK || uniform->kind == DRV_GPU_IR_UNIFORM_STORAGE) {
			if (program->buffer_count == MALI_SHADER_BUFFERS)
				return ENOSPC;
			program->buffers[program->buffer_count].set = uniform->set;
			program->buffers[program->buffer_count].binding = uniform->binding;
			program->buffers[program->buffer_count].fau_word = word;
			program->buffers[program->buffer_count].size_fau_word = MALI_SHADER_NO_FAU_WORD;
			if (uniform->kind == DRV_GPU_IR_UNIFORM_BLOCK) {
				program->buffers[program->buffer_count].kind = MALI_SHADER_BUFFER_UNIFORM;
				program->buffers[program->buffer_count].size = uniform->offset + uniform->size;
			} else {
				program->buffers[program->buffer_count].kind = MALI_SHADER_BUFFER_STORAGE;
				program->buffers[program->buffer_count].size = 0U;
			}
			program->buffer_count++;
			word += 2U;
		} else if (uniform->kind == DRV_GPU_IR_UNIFORM_SAMPLED_IMAGE) {
			if (program->texture_count == MALI_SHADER_TEXTURES)
				return ENOSPC;
			program->textures[program->texture_count].set = uniform->set;
			program->textures[program->texture_count].binding = uniform->binding;
			program->textures[program->texture_count].index = program->texture_count;
			program->textures[program->texture_count].size_fau_word = MALI_SHADER_NO_FAU_WORD;
			program->texture_count++;
		} else if (uniform->kind == DRV_GPU_IR_UNIFORM_STORAGE_IMAGE) {
			if (program->image_count == MALI_SHADER_IMAGES)
				return ENOSPC;
			program->images[program->image_count].set = uniform->set;
			program->images[program->image_count].binding = uniform->binding;
			program->images[program->image_count].index = program->image_count;
			program->images[program->image_count].format = uniform->format;
			program->images[program->image_count].size_fau_word = MALI_SHADER_NO_FAU_WORD;
			program->image_count++;
		}
	}
	/*
	 * Gives FAU words to the sizes the program reads: those it queries,
	 * and a cube's, from which a sample with derivatives computes its
	 * level.
	 */
	for (index = 0U; index < ir->instruction_count; index++) {
		inst = &ir->instructions[index];

		/* Whether a texture instruction reads its image's sizes. */
		sized = 0U;
		if (inst->op == DRV_GPU_IR_TEXTURE) {
			message = inst->component & DRV_GPU_IR_TEXTURE_MESSAGE_MASK;

			/* A size query reads them all. */
			if (message == DRV_GPU_IR_TEXTURE_RESINFO)
				sized = 1U;

			/* A cube sample with derivatives reads the cube's width. */
			if ((message == DRV_GPU_IR_TEXTURE_SAMPLE_DERIVS ||
			     message == DRV_GPU_IR_TEXTURE_SAMPLE_DERIV_COMPARE) &&
			    (inst->image_shape & DRV_GPU_IR_IMAGE_KIND_MASK) == DRV_GPU_IR_IMAGE_CUBE)
				sized = 1U;
		}

		/* A buffer's range takes one word, an image's sizes four. */
		if (inst->op == DRV_GPU_IR_STORAGE_SIZE) {
			slot = buffer_of(ir, program, inst->location);
			if (slot < program->buffer_count && program->buffers[slot].size_fau_word == MALI_SHADER_NO_FAU_WORD) {
				program->buffers[slot].size_fau_word = word;
				word++;
			}
		} else if (sized != 0U) {
			for (slot = 0U; slot < program->texture_count; slot++) {
				if (program->textures[slot].set != inst->location || program->textures[slot].binding != inst->immediate)
					continue;
				if (program->textures[slot].size_fau_word == MALI_SHADER_NO_FAU_WORD) {
					program->textures[slot].size_fau_word = word;
					word += 4U;
				}
			}
			for (slot = 0U; slot < program->image_count; slot++) {
				if (program->images[slot].set != inst->location || program->images[slot].binding != inst->immediate)
					continue;
				if (program->images[slot].size_fau_word == MALI_SHADER_NO_FAU_WORD) {
					program->images[slot].size_fau_word = word;
					word += 4U;
				}
			}
		}
	}
	program->fau_words = (word + 1U) & ~1U;

	/* Records the interface: a vertex program's inputs and varyings, a fragment program's inputs and colours. */
	for (index = 0U; index < ir->input_count; index++) {
		location = ir->inputs[index].location;
		if (location >= MALI_SHADER_LOCATIONS)
			continue;
		if (ir->stage == DRV_GPU_STAGE_VERTEX) {
			program->attribute_mask |= 1U << location;
		} else if (ir->stage == DRV_GPU_STAGE_FRAGMENT) {
			program->input_mask |= 1U << location;
			if (ir->inputs[index].flat != 0U)
				program->flat_mask |= 1U << location;
		}
	}
	for (index = 0U; index < ir->output_count; index++) {
		location = ir->outputs[index].location;
		if (location >= MALI_SHADER_LOCATIONS)
			continue;
		if (ir->stage == DRV_GPU_STAGE_VERTEX) {
			program->varying_mask |= 1U << location;
			if (location + 1U > program->varying_bytes / MALI_SHADER_VARYING_STRIDE)
				program->varying_bytes = (location + 1U) * MALI_SHADER_VARYING_STRIDE;
		} else if (ir->stage == DRV_GPU_STAGE_FRAGMENT && location < MALI_SHADER_COLORS) {
			program->color_mask |= 1U << location;
		}
	}

	/* A compute program's workgroup. */
	program->local_size[0] = ir->local_size[0];
	program->local_size[1] = ir->local_size[1];
	program->local_size[2] = ir->local_size[2];
	program->shared_bytes = ir->shared_bytes;
	program->uses_barrier = ir->uses_barrier;

	/* Succeeded: the layout is recorded. */
	return 0;
}

/* Finds the program's buffer of an IR uniform, or the buffer count when it has none. */
static uint32_t
buffer_of(
	const struct drv_gpu_shader_ir *ir,
	const struct mali_shader_program *program,
	uint32_t uniform)
{
	uint32_t index;

	/* A uniform the IR does not have has no buffer. */
	if (uniform >= ir->uniform_count)
		return program->buffer_count;

	/* Looks through the buffers by (set, binding). */
	for (index = 0U; index < program->buffer_count; index++) {
		if (program->buffers[index].set == ir->uniforms[uniform].set && program->buffers[index].binding == ir->uniforms[uniform].binding)
			return index;
	}

	/* The uniform is not a buffer. */
	return program->buffer_count;
}

/*
 * Optimizes a copy of the IR keeping only the outputs the variant writes:
 * the position for the position program, the generic varyings (and the
 * position's w, which a NoPerspective varying is multiplied by) for the
 * varying program, everything otherwise.
 */
static int
optimize_for(
	const struct drv_gpu_shader_ir *ir,
	uint32_t variant,
	struct drv_gpu_shader_ir *optimized)
{
	struct drv_gpu_shader_optimize_options options;
	uint32_t slot;
	uint32_t index;
	int error;

	/* Every block runs, and known bits fold. */
	kern_memset(&options, 0, sizeof(options));
	options.runs_every_block = 1U;
	options.simplify = 1U;

	/* Keeps the outputs the variant writes. */
	if (variant == VA_VARIANT_POSITION) {
		for (slot = DRV_GPU_IR_OPTIMIZE_POSITION; slot < DRV_GPU_IR_OPTIMIZE_SLOTS; slot++)
			options.needed[slot] = 1U;
	} else if (variant == VA_VARIANT_VARYING) {
		for (slot = 0U; slot < DRV_GPU_IR_OPTIMIZE_POSITION; slot++)
			options.needed[slot] = 1U;
		for (index = 0U; index < ir->output_count; index++) {
			if (ir->outputs[index].noperspective != 0U)
				options.needed[DRV_GPU_IR_OPTIMIZE_POSITION + 3U] = 1U;
		}
	} else {
		kern_memset(options.needed, 1, sizeof(options.needed));
	}

	/* Optimizes; a program the pass leaves alone keeps its instructions NULL. */
	error = drv_gpu_shader_optimize(ir, &options, optimized);
	if (error == ENOMEM)
		return error;

	/* Any other refusal of the optimizer lowers the parsed program. */
	if (error != 0)
		kern_memset(optimized, 0, sizeof(*optimized));

	/* Succeeded: the copy, or none, is ready. */
	return 0;
}

/*
 * Lowers the program, again with more registers kept free at each loop's
 * head (by spilling the values that live through the loop without being
 * read in it) while the registers run out.
 */
static int
lower_with_room(
	struct va_compiler *c)
{
	static const uint32_t reserves[] = { 0U, 16U, 32U, VA_REGISTERS };
	const struct drv_gpu_shader_ir *ir;
	struct mali_shader_program *program;
	uint32_t variant;
	uint32_t options;
	uint32_t attempt;
	int error;

	/* Tries each reserve while the registers run out. */
	ir = c->ir;
	program = c->program;
	variant = c->variant;
	options = c->options;
	error = 0;
	for (attempt = 0U; attempt < sizeof(reserves) / sizeof(reserves[0]); attempt++) {
		if (attempt != 0U) {
			va_code_release(&c->code);
			kern_memset(c, 0, sizeof(*c));
			c->program = program;
			c->variant = variant;
			c->options = options;
			c->ir = ir;
		}
		c->loop_reserve = reserves[attempt];
		error = lower(c);
		if (error != ENOSPC)
			break;
	}

	/* Reports the lowering's refusal. */
	if (error != 0)
		return error;

	/* Succeeded: the list is complete. */
	return 0;
}

/* Lowers the whole program. */
static int
lower(
	struct va_compiler *c)
{
	uint32_t value_count;
	uint32_t index;
	uint32_t location;
	uint32_t component;
	int error;

	/* Allocates each value's register, a value with none being VA_NO_REG. */
	error = ENOMEM;
	value_count = c->ir->value_count + 1U;
	c->reg = kern_malloc(value_count);
	if (c->reg == NULL)
		goto out;
	kern_memset(c->reg, VA_NO_REG, value_count);

	/* Allocates each value's constant mark and constant. */
	c->is_constant = kern_calloc(value_count, 1U);
	if (c->is_constant == NULL)
		goto out;
	c->constant = kern_calloc(value_count, sizeof(*c->constant));
	if (c->constant == NULL)
		goto out;

	/* Allocates each value's last use and first definition. */
	c->last_use = kern_malloc((size_t)value_count * sizeof(*c->last_use));
	if (c->last_use == NULL)
		goto out;
	c->defined_at = kern_malloc((size_t)value_count * sizeof(*c->defined_at));
	if (c->defined_at == NULL)
		goto out;

	/* Allocates the marks of the instructions lowered with an earlier one. */
	c->done = kern_calloc((size_t)c->ir->instruction_count + 1U, 1U);
	if (c->done == NULL)
		goto out;

	/* Allocates each value's spill word, pin and last step. */
	c->tls_word = kern_malloc((size_t)value_count * sizeof(*c->tls_word));
	if (c->tls_word == NULL)
		goto out;
	kern_memset(c->tls_word, 0xFF, (size_t)value_count * sizeof(*c->tls_word));
	c->pinned = kern_calloc(value_count, 1U);
	if (c->pinned == NULL)
		goto out;
	c->touched = kern_calloc(value_count, sizeof(*c->touched));
	if (c->touched == NULL)
		goto out;
	mark_pinned(c);

	/*
	 * Starts with every register below the stage's preloads free (r48, the
	 * blend shader's link, is free without blend shaders) and no output
	 * written.
	 */
	c->reg_limit = VA_PRELOAD_LOCAL_ID_XY;
	if (c->ir->stage == DRV_GPU_STAGE_FRAGMENT)
		c->reg_limit = VA_PRELOAD_WORKGROUP_X;
	if (c->ir->stage == DRV_GPU_STAGE_VERTEX)
		c->reg_limit = VA_PRELOAD_OUTPUT_INDEX;
	c->free_regs = va_register_mask(0U, c->reg_limit);
	for (location = 0U; location < MALI_SHADER_LOCATIONS; location++) {
		for (component = 0U; component < 4U; component++)
			c->output_value[location][component] = VA_NOT_DEFINED;
	}
	for (component = 0U; component < 4U; component++)
		c->position_value[component] = VA_NOT_DEFINED;
	c->point_size_value = VA_NOT_DEFINED;
	c->depth_value = VA_NOT_DEFINED;
	c->sample_mask_value = VA_NOT_DEFINED;
	c->stencil_value = VA_NOT_DEFINED;

	/* Finds each value's definition and last use. */
	error = compute_liveness(c);
	if (error != 0) {
		fail(c, error, "the loops nest too deep or do not pair");
		goto out;
	}

	/* Lowers every instruction, each a step reading its sources, freeing what dies after it. */
	for (index = 0U; index < c->ir->instruction_count && c->error == 0; index++) {
		c->at = index;
		c->step++;
		c->source_count = ir_sources(&c->ir->instructions[index], c->sources);
		if (c->done[index] == 0U)
			lower_instruction(c, index);
		release_after(c, index);
	}

	/* Writes what the stage leaves at its end, a step of its own. */
	c->step++;
	c->source_count = 0U;
	if (c->error == 0)
		lower_end(c);

	/* An append that ran out of memory fails the program. */
	if (c->error == 0 && c->code.failed != 0)
		fail(c, c->code.failed, "out of memory");
	error = c->error;

out:
	kern_free(c->reg);
	kern_free(c->is_constant);
	kern_free(c->constant);
	kern_free(c->last_use);
	kern_free(c->defined_at);
	kern_free(c->done);
	kern_free(c->tls_word);
	kern_free(c->pinned);
	kern_free(c->touched);
	c->tls_word = NULL;
	c->pinned = NULL;
	c->touched = NULL;
	c->reg = NULL;
	c->is_constant = NULL;
	c->constant = NULL;
	c->last_use = NULL;
	c->defined_at = NULL;
	c->done = NULL;

	/* Reports the lowering's refusal. */
	if (error != 0)
		return error;

	/* Succeeded: the list is complete. */
	return 0;
}

/* Lowers one IR instruction. */
static void
lower_instruction(
	struct va_compiler *c,
	uint32_t index)
{
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t reg;

	inst = &c->ir->instructions[index];

	/* Dispatches on the operation. */
	switch (inst->op) {
	case DRV_GPU_IR_NOP:
	case DRV_GPU_IR_SKIP_BEGIN:
	case DRV_GPU_IR_SKIP_END:
	case DRV_GPU_IR_FENCE:
		/* Straight code needs nothing for these: every block runs, and accesses are serialized. */
		break;

	case DRV_GPU_IR_CONST:
	case DRV_GPU_IR_ICONST:
	case DRV_GPU_IR_BOOL:
		/* A constant takes no register: each reader makes it again or reads it from the table. */
		if (inst->dst <= c->ir->value_count) {
			c->is_constant[inst->dst] = 1U;
			c->constant[inst->dst] = inst->immediate;
		}
		break;

	case DRV_GPU_IR_LOAD_PUSH:
		reg = define_value(c, inst->dst);
		emit_move_uniform(c, reg, inst->immediate / 4U);
		break;

	case DRV_GPU_IR_LOAD_INPUT:
		lower_load_input(c, index);
		break;

	case DRV_GPU_IR_STORE_OUTPUT:
		lower_store_output(c, inst);
		break;

	case DRV_GPU_IR_LOAD_UBO:
		lower_load_ubo(c, index);
		break;

	case DRV_GPU_IR_LOAD_STORAGE:
	case DRV_GPU_IR_STORE_STORAGE:
	case DRV_GPU_IR_LOAD_SHARED:
	case DRV_GPU_IR_STORE_SHARED:
		lower_storage(c, inst);

		/* A store to a storage buffer writes memory the draw's kill must wait for. */
		if (inst->op == DRV_GPU_IR_STORE_STORAGE)
			c->program->writes_memory = 1U;
		break;

	case DRV_GPU_IR_ATOMIC:
		lower_atomic(c, inst);

		/* An atomic on a buffer or an image writes memory; one on shared memory does not leave the workgroup. */
		if (inst->location != DRV_GPU_IR_LOCATION_SHARED)
			c->program->writes_memory = 1U;
		break;

	case DRV_GPU_IR_LOAD_SYSTEM:
		lower_system(c, inst);
		break;

	case DRV_GPU_IR_SAMPLE:
	case DRV_GPU_IR_SAMPLE_BIAS:
	case DRV_GPU_IR_SAMPLE_LOD:
		lower_texture(c, inst);
		break;

	case DRV_GPU_IR_TEXTURE:
		/* A size query reads the FAU; every other message samples. */
		if ((inst->component & DRV_GPU_IR_TEXTURE_MESSAGE_MASK) == DRV_GPU_IR_TEXTURE_RESINFO) {
			lower_size_query(c, inst);
		} else {
			lower_texture(c, inst);
		}
		break;

	case DRV_GPU_IR_STORAGE_SIZE:
		lower_size_query(c, inst);
		break;

	case DRV_GPU_IR_IMAGE_READ:
	case DRV_GPU_IR_IMAGE_WRITE:
		lower_image(c, inst);

		/* A store to a storage image writes memory. */
		if (inst->op == DRV_GPU_IR_IMAGE_WRITE)
			c->program->writes_memory = 1U;
		break;

	case DRV_GPU_IR_KILL:
		/* Discards and ends the lanes whose Boolean is set (all ones is a NaN, unequal to zero). */
		emit(c,
		     VA_OP_DISCARD_F32 | operand_byte(value_reg(c, inst->src[0])) |
		     va_field(operand_byte(VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO), VA_SHIFT_SOURCE1) |
		     va_field(VA_CONDITION_NE, VA_SHIFT_CONDITION) | va_dest(0U),
		     operand_mask(value_reg(c, inst->src[0])),
		     0U,
		     0U);
		c->program->discards = 1U;
		break;

	case DRV_GPU_IR_BARRIER:
		emit(c, VA_OP_BARRIER | va_dest(0U), 0U, 0U, VA_INST_MESSAGE | VA_INST_BARRIER);
		break;

	case DRV_GPU_IR_LOOP_BEGIN:
		/* The next instruction is the loop's head, which the loop's end branches back to. */
		if (c->loop_depth == VA_LOOP_DEPTH) {
			fail(c, ENOTSUP, "loops nest too deep");
			break;
		}
		make_loop_room(c);
		c->loop_start[c->loop_depth] = c->code.count;
		c->loop_head[c->loop_depth] = c->at;
		c->loop_depth++;
		break;

	case DRV_GPU_IR_LOOP_END:
		/* Branches back to the head for the lanes whose Boolean holds. */
		if (c->loop_depth == 0U) {
			fail(c, EINVAL, "a loop end without its begin");
			break;
		}
		c->loop_depth--;
		if (c->loop_start[c->loop_depth] == c->code.count)
			emit(c, VA_OP_NOP | va_dest(0U), 0U, 0U, 0U);
		c->code.list[c->loop_start[c->loop_depth]].flags |= VA_INST_TARGET;
		emit(c,
		     VA_OP_BRANCHZ | operand_byte(value_reg(c, inst->src[0])) | va_dest(0U),
		     operand_mask(value_reg(c, inst->src[0])),
		     0U,
		     VA_INST_BRANCH);
		if (c->error == 0 && c->code.failed == 0) {
			c->code.list[c->code.count - 1U].target = (int32_t)c->loop_start[c->loop_depth];
			c->code.list[c->code.count - 1U].flow = VA_FLOW_RECONVERGE;
		}
		break;

	case DRV_GPU_IR_MOVE:
		/* A loop variable keeps its register; its first definition takes one. */
		reg = c->reg[inst->dst];
		if (reg == VA_NO_REG)
			reg = define_value(c, inst->dst);
		emit_move(c, reg, value_operand(c, inst->src[0]));
		break;

	default:
		lower_alu(c, inst);
		break;
	}
}

/* Writes what the stage leaves at its end. */
static void
lower_end(
	struct va_compiler *c)
{
	/* Dispatches on the program. */
	switch (c->variant) {
	case VA_VARIANT_POSITION:
		lower_position_end(c);
		break;

	case VA_VARIANT_VARYING:
		lower_varying_end(c);
		break;

	default:
		if (c->ir->stage == DRV_GPU_STAGE_FRAGMENT)
			lower_fragment_end(c);
		break;
	}
}

/* Lowers an arithmetic instruction. */
static void
lower_alu(
	struct va_compiler *c,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t dst;
	uint32_t a;
	uint32_t b;
	uint32_t s;
	uint32_t t;
	uint32_t zero;
	uint32_t count;

	/* Reads the sources the operation has, then defines its result. */
	a = VA_OPERAND_NONE;
	b = VA_OPERAND_NONE;
	s = VA_OPERAND_NONE;
	count = ir_sources(inst, NULL);
	if (count >= 1U)
		a = value_operand(c, inst->src[0]);
	if (count >= 2U)
		b = value_operand(c, inst->src[1]);
	if (count >= 3U)
		s = value_operand(c, inst->src[2]);
	if (c->error != 0)
		return;
	dst = define_value(c, inst->dst);
	if (c->error != 0)
		return;
	zero = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO;

	/* Dispatches on the operation. */
	switch (inst->op) {
	case DRV_GPU_IR_FADD:
		emit_alu(c, VA_OP_FADD_F32, dst, a, b, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_FSUB:
		emit_alu(c, VA_OP_FADD_F32 | va_field(1U, VA_SHIFT_NEG_SOURCE1), dst, a, b, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_FMUL:
		emit_fmul(c, dst, a, b);
		break;

	case DRV_GPU_IR_FNEG:
		/* -a + -0 is -a for every a, zero included. */
		emit_alu(c,
			 VA_OP_FADD_F32 | va_field(1U, VA_SHIFT_NEG_SOURCE0) | va_field(1U, VA_SHIFT_NEG_SOURCE1),
			 dst,
			 a,
			 zero,
			 VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_FABS:
		emit_alu(c,
			 VA_OP_FADD_F32 | va_field(1U, VA_SHIFT_ABS_SOURCE0) | va_field(1U, VA_SHIFT_NEG_SOURCE1),
			 dst,
			 a,
			 zero,
			 VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_RCP:
		emit_alu(c, VA_OP_FRCP_F32, dst, a, VA_OPERAND_NONE, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_RSQ:
		emit_alu(c, VA_OP_FRSQ_F32, dst, a, VA_OPERAND_NONE, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_SQRT:
		/* sqrt(a) = 1 / (1 / sqrt(a)): 0 gives 1 / inf = 0, inf gives 1 / 0 = inf. */
		t = alloc_block(c, 1U);
		emit_alu(c, VA_OP_FRSQ_F32, t, a, VA_OPERAND_NONE, VA_OPERAND_NONE);
		emit_alu(c, VA_OP_FRCP_F32, dst, t, VA_OPERAND_NONE, VA_OPERAND_NONE);
		free_reg(c, t);
		break;

	case DRV_GPU_IR_EXP2:
		emit_exp2(c, dst, a);
		break;

	case DRV_GPU_IR_LOG2:
		emit_log2(c, dst, a);
		break;

	case DRV_GPU_IR_FLOOR:
		emit_alu(c, VA_OP_FROUND_F32 | va_field(VA_ROUND_RTN, VA_SHIFT_ROUND_MODE), dst, a, VA_OPERAND_NONE, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_FTRUNC:
		emit_alu(c, VA_OP_FROUND_F32 | va_field(VA_ROUND_RTZ, VA_SHIFT_ROUND_MODE), dst, a, VA_OPERAND_NONE, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_FROUND_EVEN:
		emit_alu(c, VA_OP_FROUND_F32 | va_field(VA_ROUND_RTE, VA_SHIFT_ROUND_MODE), dst, a, VA_OPERAND_NONE, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_FRACT:
		/* a - floor(a). */
		t = alloc_block(c, 1U);
		emit_alu(c, VA_OP_FROUND_F32 | va_field(VA_ROUND_RTN, VA_SHIFT_ROUND_MODE), t, a, VA_OPERAND_NONE, VA_OPERAND_NONE);
		emit_alu(c, VA_OP_FADD_F32 | va_field(1U, VA_SHIFT_NEG_SOURCE1), dst, a, t, VA_OPERAND_NONE);
		free_reg(c, t);
		break;

	case DRV_GPU_IR_FMIN:
		emit_alu(c, VA_OP_FMIN_F32, dst, a, b, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_FMAX:
		emit_alu(c, VA_OP_FMAX_F32, dst, a, b, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_FLT:
		emit_alu(c, VA_OP_FCMP_OR_F32 | va_field(VA_CONDITION_LT, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE), dst, a, b, zero);
		break;

	case DRV_GPU_IR_FGE:
		emit_alu(c, VA_OP_FCMP_OR_F32 | va_field(VA_CONDITION_GE, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE), dst, a, b, zero);
		break;

	case DRV_GPU_IR_FEQ:
		emit_alu(c, VA_OP_FCMP_OR_F32 | va_field(VA_CONDITION_EQ, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE), dst, a, b, zero);
		break;

	case DRV_GPU_IR_FNEU:
		emit_alu(c, VA_OP_FCMP_OR_F32 | va_field(VA_CONDITION_NE, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE), dst, a, b, zero);
		break;

	case DRV_GPU_IR_ILT:
		emit_alu(c, VA_OP_ICMP_OR_S32 | va_field(VA_CONDITION_LT, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE), dst, a, b, zero);
		break;

	case DRV_GPU_IR_IGE:
		emit_alu(c, VA_OP_ICMP_OR_S32 | va_field(VA_CONDITION_GE, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE), dst, a, b, zero);
		break;

	case DRV_GPU_IR_ULT:
		emit_alu(c, VA_OP_ICMP_OR_U32 | va_field(VA_CONDITION_LT, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE), dst, a, b, zero);
		break;

	case DRV_GPU_IR_UGE:
		emit_alu(c, VA_OP_ICMP_OR_U32 | va_field(VA_CONDITION_GE, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE), dst, a, b, zero);
		break;

	case DRV_GPU_IR_IEQ:
		emit_alu(c, VA_OP_ICMP_OR_U32 | va_field(VA_CONDITION_EQ, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE), dst, a, b, zero);
		break;

	case DRV_GPU_IR_INE:
		emit_alu(c, VA_OP_ICMP_OR_U32 | va_field(VA_CONDITION_NE, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE), dst, a, b, zero);
		break;

	case DRV_GPU_IR_AND:
	case DRV_GPU_IR_IAND:
		/* a & (b << 0): the shift operand is the constant zero's byte lanes. */
		emit_alu(c, VA_OP_LSHIFT_AND_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), dst, a, zero, b);
		break;

	case DRV_GPU_IR_OR:
	case DRV_GPU_IR_IOR:
		emit_alu(c, VA_OP_LSHIFT_OR_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), dst, a, zero, b);
		break;

	case DRV_GPU_IR_IXOR:
		emit_alu(c, VA_OP_LSHIFT_XOR_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), dst, a, zero, b);
		break;

	case DRV_GPU_IR_NOT:
	case DRV_GPU_IR_INOT:
		/* 0 | ~a. */
		emit_alu(c,
			 VA_OP_LSHIFT_OR_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES) | va_field(1U, VA_SHIFT_NOT),
			 dst,
			 zero,
			 zero,
			 a);
		break;

	case DRV_GPU_IR_SHL:
		emit_shift(c, VA_OP_LSHIFT_OR_I32, dst, a, b);
		break;

	case DRV_GPU_IR_SHR:
		emit_shift(c, VA_OP_RSHIFT_OR_I32, dst, a, b);
		break;

	case DRV_GPU_IR_ASR:
		emit_shift(c, VA_OP_RSHIFT_OR_I32 | va_field(1U, VA_SHIFT_ARITHMETIC), dst, a, b);
		break;

	case DRV_GPU_IR_SELECT:
		/* MUX keeps its first source where the mask is zero: the false value. */
		emit_alu(c, VA_OP_MUX_I32 | va_field(VA_MUX_INT_ZERO, VA_SHIFT_MUX), dst, s, b, a);
		break;

	case DRV_GPU_IR_IADD:
		emit_alu(c, VA_OP_IADD_U32, dst, a, b, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_ISUB:
		emit_alu(c, VA_OP_ISUB_U32, dst, a, b, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_IMUL:
		emit_alu(c, VA_OP_IMUL_I32, dst, a, b, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_INEG:
		emit_alu(c, VA_OP_ISUB_U32, dst, zero, a, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_I2F:
		emit_alu(c, VA_OP_S32_TO_F32 | va_field(VA_ROUND_RTE, VA_SHIFT_ROUND_MODE), dst, a, VA_OPERAND_NONE, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_U2F:
		emit_alu(c, VA_OP_U32_TO_F32 | va_field(VA_ROUND_RTE, VA_SHIFT_ROUND_MODE), dst, a, VA_OPERAND_NONE, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_F2I:
		emit_alu(c, VA_OP_F32_TO_S32 | va_field(VA_ROUND_RTZ, VA_SHIFT_ROUND_MODE), dst, a, VA_OPERAND_NONE, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_F2U:
		emit_alu(c, VA_OP_F32_TO_U32 | va_field(VA_ROUND_RTZ, VA_SHIFT_ROUND_MODE), dst, a, VA_OPERAND_NONE, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_SIN:
		emit_sine(c, dst, a, 0U);
		break;

	case DRV_GPU_IR_COS:
		/* cos(x) = sin(x + pi / 2): a quarter turn more. */
		emit_sine(c, dst, a, VA_FLOAT_QUARTER);
		break;

	case DRV_GPU_IR_DDX:
	case DRV_GPU_IR_DDX_FINE:
	case DRV_GPU_IR_DDY:
	case DRV_GPU_IR_DDY_FINE:
		emit_derivative(c, inst->op, dst, a);
		break;

	case DRV_GPU_IR_UDIV:
	case DRV_GPU_IR_UMOD:
	case DRV_GPU_IR_IDIV:
	case DRV_GPU_IR_IREM:
		emit_divide(c, inst->op, dst, a, b);
		break;

	case DRV_GPU_IR_PACK_HALF:
		emit_alu(c, VA_OP_V2F32_TO_V2F16 | va_field(VA_ROUND_RTE, VA_SHIFT_ROUND_MODE), dst, a, b, VA_OPERAND_NONE);
		break;

	case DRV_GPU_IR_UNPACK_HALF:
		emit_alu(c, VA_OP_F16_TO_F32 | va_field(inst->component & 1U, VA_SHIFT_LANE_SOURCE0), dst, a, VA_OPERAND_NONE, VA_OPERAND_NONE);
		break;

	default:
		fail(c, ENOTSUP, "an operation the Valhall lowering does not have");
		break;
	}
}

/* Moves a register (or a constant table entry) into a register. */
static void
emit_move(
	struct va_compiler *c,
	uint32_t dst,
	uint32_t src)
{
	/* MOV.i32 copies the bits. */
	emit_alu(c, VA_OP_MOV_I32, dst, src, VA_OPERAND_NONE, VA_OPERAND_NONE);
}

/* Moves a FAU uniform word into a register. */
static void
emit_move_uniform(
	struct va_compiler *c,
	uint32_t dst,
	uint32_t word)
{
	uint64_t bits;

	/* MOV.i32 from the word's slot, on the word's page. */
	bits = VA_OP_MOV_I32 | va_source_uniform(word) | va_dest(dst);
	bits |= va_field(va_uniform_page(word), VA_SHIFT_FAU_PAGE);
	emit(c, bits, 0U, va_register_mask(dst, 1U), 0U);
}

/* Writes a 32-bit constant into a register. */
static void
emit_constant(
	struct va_compiler *c,
	uint32_t dst,
	uint32_t value)
{
	uint64_t bits;

	/* IADD_IMM.i32 adds the 32-bit immediate to the constant zero. */
	bits = VA_OP_IADD_IMM_I32 | operand_byte(VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO);
	bits |= va_field(value, VA_SHIFT_SOURCE1);
	bits |= va_dest(dst);
	emit(c, bits, 0U, va_register_mask(dst, 1U), 0U);
}

/* Makes a constant in a fresh register, which the caller frees. */
static uint32_t
temp_constant(
	struct va_compiler *c,
	uint32_t value)
{
	uint32_t reg;

	/* Allocates the register and writes the constant. */
	reg = alloc_block(c, 1U);
	if (reg == VA_NO_REG)
		return 0U;
	emit_constant(c, reg, value);

	/* Reports the register. */
	return reg;
}

/* Multiplies two floats: a * b + -0 keeps the sign of a zero product. */
static void
emit_fmul(
	struct va_compiler *c,
	uint32_t dst,
	uint32_t a,
	uint32_t b)
{
	/* FMA.f32 with the negated constant zero added. */
	emit_alu(c,
		 VA_OP_FMA_F32 | va_field(1U, VA_SHIFT_NEG_SOURCE2),
		 dst,
		 a,
		 b,
		 VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO);
}

/* Computes a * b + addend. */
static void
emit_fma(
	struct va_compiler *c,
	uint32_t dst,
	uint32_t a,
	uint32_t b,
	uint32_t addend)
{
	/* FMA.f32. */
	emit_alu(c, VA_OP_FMA_F32, dst, a, b, addend);
}

/*
 * Computes log2(a): the exponent of a, plus FLOGD's estimate of the
 * logarithm of its mantissa over the mantissa less one, times that.
 *
 * Procedure referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/bifrost_compile.c (bi_flog2_32).
 */
static void
emit_log2(
	struct va_compiler *c,
	uint32_t dst,
	uint32_t a)
{
	uint32_t exponent;
	uint32_t reduced;
	uint32_t estimate;

	/* Takes the exponent as a float. */
	exponent = alloc_block(c, 1U);
	reduced = alloc_block(c, 1U);
	estimate = alloc_block(c, 1U);
	if (c->error != 0)
		return;
	emit_alu(c, VA_OP_FREXPE_F32 | va_field(1U, VA_SHIFT_FREXP_SQRT), exponent, a, VA_OPERAND_NONE, VA_OPERAND_NONE);
	emit_alu(c, VA_OP_S32_TO_F32 | va_field(VA_ROUND_RTE, VA_SHIFT_ROUND_MODE), exponent, exponent, VA_OPERAND_NONE, VA_OPERAND_NONE);

	/* Takes the mantissa less one, and the logarithm's estimate. */
	emit_alu(c,
		 VA_OP_FADD_LSCALE_F32 | va_field(1U, VA_SHIFT_NEG_SOURCE0),
		 reduced,
		 VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_FLOAT_ONE,
		 a,
		 VA_OPERAND_NONE);
	emit_alu(c, VA_OP_FLOGD_F32, estimate, a, VA_OPERAND_NONE, VA_OPERAND_NONE);

	/* Combines them. */
	emit_fma(c, dst, estimate, reduced, exponent);
	free_reg(c, exponent);
	free_reg(c, reduced);
	free_reg(c, estimate);
}

/*
 * Computes 2^a: FEXP of a as an 8:24 fixed-point number, with a itself
 * for the special cases.
 *
 * Procedure referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/bifrost_compile.c (bi_fexp_32).
 */
static void
emit_exp2(
	struct va_compiler *c,
	uint32_t dst,
	uint32_t a)
{
	uint32_t scale;
	uint32_t scaled;

	/* Scales a by 2^24 and converts it to an integer. */
	scale = temp_constant(c, VA_FLOAT_2_POW_24);
	scaled = alloc_block(c, 1U);
	if (c->error != 0)
		return;
	emit_fmul(c, scaled, a, scale);
	emit_alu(c, VA_OP_F32_TO_S32 | va_field(VA_ROUND_RTE, VA_SHIFT_ROUND_MODE), scale, scaled, VA_OPERAND_NONE, VA_OPERAND_NONE);

	/* Takes the exponent of the fixed-point number. */
	emit_alu(c, VA_OP_FEXP_F32, dst, scale, scaled, VA_OPERAND_NONE);
	free_reg(c, scale);
	free_reg(c, scaled);
}

/*
 * Computes sin(2 pi (a / (2 pi) + turn_offset)): the angle in turns
 * reduced to [-1/2, 1/2] by the nearest whole turn, its magnitude folded
 * to [0, 1/4] (sin(pi - y) = sin(y)), the Taylor polynomial there, and
 * the sign of the turn given back.
 */
static void
emit_sine(
	struct va_compiler *c,
	uint32_t dst,
	uint32_t a,
	uint32_t turn_offset)
{
	static const uint32_t coefficients[] = {
		VA_FLOAT_SIN_C9,
		VA_FLOAT_SIN_C7,
		VA_FLOAT_SIN_C5,
		VA_FLOAT_SIN_C3
	};
	uint32_t turn;
	uint32_t whole;
	uint32_t fold;
	uint32_t square;
	uint32_t constant;
	uint32_t index;

	/* Allocates the turn, its whole part (then the polynomial), the fold and the square. */
	turn = alloc_block(c, 1U);
	whole = alloc_block(c, 1U);
	fold = alloc_block(c, 1U);
	square = alloc_block(c, 1U);
	constant = alloc_block(c, 1U);
	if (c->error != 0)
		return;

	/* The angle in turns, less its nearest whole turn. */
	emit_constant(c, constant, VA_FLOAT_INV_2PI);
	if (turn_offset != 0U) {
		emit_constant(c, fold, turn_offset);
		emit_fma(c, turn, a, constant, fold);
	} else {
		emit_fmul(c, turn, a, constant);
	}
	emit_alu(c, VA_OP_FROUND_F32 | va_field(VA_ROUND_RTE, VA_SHIFT_ROUND_MODE), whole, turn, VA_OPERAND_NONE, VA_OPERAND_NONE);
	emit_alu(c, VA_OP_FADD_F32 | va_field(1U, VA_SHIFT_NEG_SOURCE1), turn, turn, whole, VA_OPERAND_NONE);

	/* Folds the magnitude: min(|t|, 1/2 - |t|), then to radians. */
	emit_constant(c, constant, VA_FLOAT_HALF);
	emit_alu(c, VA_OP_FADD_F32 | va_field(1U, VA_SHIFT_ABS_SOURCE0) | va_field(1U, VA_SHIFT_NEG_SOURCE1), fold, turn, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO, VA_OPERAND_NONE);
	emit_alu(c, VA_OP_FADD_F32 | va_field(1U, VA_SHIFT_NEG_SOURCE1), whole, constant, fold, VA_OPERAND_NONE);
	emit_alu(c, VA_OP_FMIN_F32, fold, fold, whole, VA_OPERAND_NONE);
	emit_constant(c, constant, VA_FLOAT_2PI);
	emit_fmul(c, fold, fold, constant);

	/* Evaluates y (1 + y^2 (c3 + y^2 (c5 + ... + y^2 c11))) by Horner's rule. */
	emit_fmul(c, square, fold, fold);
	emit_constant(c, whole, VA_FLOAT_SIN_C11);
	for (index = 0U; index < sizeof(coefficients) / sizeof(coefficients[0]); index++) {
		emit_constant(c, constant, coefficients[index]);
		emit_fma(c, whole, whole, square, constant);
	}
	emit_fma(c, whole, whole, square, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_FLOAT_ONE);
	emit_fmul(c, whole, whole, fold);

	/* Gives the result the sign of the reduced turn. */
	emit_constant(c, constant, VA_SIGN_BIT);
	emit_alu(c, VA_OP_LSHIFT_AND_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), turn, turn, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO, constant);
	emit_alu(c, VA_OP_LSHIFT_XOR_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), dst, whole, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO, turn);
	free_reg(c, turn);
	free_reg(c, whole);
	free_reg(c, fold);
	free_reg(c, square);
	free_reg(c, constant);
}

/*
 * Computes a derivative across the pixel's quad with CLPER, which reads a
 * value from another lane of the quad (lane 0 top left, 1 top right, 2
 * bottom left, 3 bottom right): coarse, the top left pixel's difference
 * for all four (lane 1 or 2 less lane 0); fine, the neighbour across the
 * axis (the lane exclusive-or 1 or 2) less the lane's own value, with
 * the sign flipped on the right or bottom lanes, where the neighbour is
 * on the other side.
 *
 * Procedure referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/bifrost_compile.c (bi_clper,
 * bi_emit_derivative).
 */
static void
emit_derivative(
	struct va_compiler *c,
	uint32_t op,
	uint32_t dst,
	uint32_t a)
{
	uint32_t axis;
	uint32_t left;
	uint32_t right;
	uint32_t lane;
	uint64_t clper;
	uint64_t bits;

	/* Chooses the axis's lane bit. */
	axis = 1U;
	if (op == DRV_GPU_IR_DDY || op == DRV_GPU_IR_DDY_FINE)
		axis = 2U;
	left = alloc_block(c, 1U);
	right = alloc_block(c, 1U);
	if (c->error != 0)
		return;

	/* CLPER in the quad, an inactive lane reading zero; the lane comes from a byte of 0x03020100. */
	clper = VA_OP_CLPER_I32 | va_field(VA_SUBGROUP_QUAD, VA_SHIFT_SUBGROUP);
	clper |= va_field(va_source_immediate(VA_IMMEDIATE_INTEGERS_0123), VA_SHIFT_SOURCE1);

	/* A coarse derivative reads lane 0 and the lane across the axis. */
	if (op == DRV_GPU_IR_DDX || op == DRV_GPU_IR_DDY) {
		emit(c,
		     clper | va_source_register(a) | va_field(VA_LANES_B00, VA_SHIFT_LANES) | va_dest(left),
		     va_register_mask(a, 1U),
		     va_register_mask(left, 1U),
		     0U);
		emit(c,
		     clper | va_source_register(a) | va_field(VA_LANES_B00 + axis, VA_SHIFT_LANES) | va_dest(right),
		     va_register_mask(a, 1U),
		     va_register_mask(right, 1U),
		     0U);
		emit_alu(c, VA_OP_FADD_F32 | va_field(1U, VA_SHIFT_NEG_SOURCE1), dst, right, left, VA_OPERAND_NONE);
		free_reg(c, left);
		free_reg(c, right);
		return;
	}

	/* A fine derivative reads the neighbour across the axis. */
	bits = clper | va_source_register(a) | va_field(VA_LANES_B00 + axis, VA_SHIFT_LANES);
	bits |= va_field(VA_LANE_OPERATION_XOR, VA_SHIFT_LANE_OPERATION) | va_dest(right);
	emit(c, bits, va_register_mask(a, 1U), va_register_mask(right, 1U), 0U);
	emit_alu(c, VA_OP_FADD_F32 | va_field(1U, VA_SHIFT_NEG_SOURCE1), left, right, a, VA_OPERAND_NONE);

	/* Flips the sign where the lane's axis bit is set: (lane & axis) << (31 or 30) into the sign. */
	lane = right;
	emit(c,
	     VA_OP_MOV_I32 | va_source_special(VA_SPECIAL_LANE_ID, 0U) | va_field(3U, VA_SHIFT_FAU_PAGE) | va_dest(lane),
	     0U,
	     va_register_mask(lane, 1U),
	     0U);
	emit_constant(c, dst, axis);
	emit_alu(c, VA_OP_LSHIFT_AND_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), lane, lane, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO, dst);
	emit_constant(c, dst, 32U - axis);
	emit_alu(c, VA_OP_LSHIFT_XOR_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), dst, lane, dst, left);
	free_reg(c, left);
	free_reg(c, right);
}

/*
 * Divides 32-bit integers, unsigned or signed: the quotient or the
 * remainder of n and d into dst.  A signed division divides the
 * magnitudes and gives the quotient the sign of n times that of d, the
 * remainder the sign of n.
 */
static void
emit_divide(
	struct va_compiler *c,
	uint32_t op,
	uint32_t dst,
	uint32_t n,
	uint32_t d)
{
	uint32_t quotient;
	uint32_t remainder;
	uint32_t magnitude_n;
	uint32_t magnitude_d;
	uint32_t sign_n;
	uint32_t sign_d;
	uint32_t zero;
	int is_signed;

	/* Allocates the quotient and the remainder. */
	quotient = alloc_block(c, 1U);
	remainder = alloc_block(c, 1U);
	if (c->error != 0)
		return;
	zero = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO;
	is_signed = 0;
	if (op == DRV_GPU_IR_IDIV || op == DRV_GPU_IR_IREM)
		is_signed = 1;

	/* An unsigned division divides the operands as they are. */
	if (is_signed == 0) {
		emit_unsigned_divide(c, quotient, remainder, n, d);
		if (op == DRV_GPU_IR_UDIV) {
			emit_move(c, dst, quotient);
		} else {
			emit_move(c, dst, remainder);
		}
		free_reg(c, quotient);
		free_reg(c, remainder);
		return;
	}

	/* Takes the signs (all ones for a negative operand) and the magnitudes. */
	sign_n = alloc_block(c, 1U);
	sign_d = alloc_block(c, 1U);
	magnitude_n = alloc_block(c, 1U);
	magnitude_d = alloc_block(c, 1U);
	if (c->error != 0)
		return;
	emit_alu(c, VA_OP_ICMP_OR_S32 | va_field(VA_CONDITION_LT, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE), sign_n, n, zero, zero);
	emit_alu(c, VA_OP_ICMP_OR_S32 | va_field(VA_CONDITION_LT, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE), sign_d, d, zero, zero);
	emit_alu(c, VA_OP_LSHIFT_XOR_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), magnitude_n, n, zero, sign_n);
	emit_alu(c, VA_OP_ISUB_U32, magnitude_n, magnitude_n, sign_n, VA_OPERAND_NONE);
	emit_alu(c, VA_OP_LSHIFT_XOR_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), magnitude_d, d, zero, sign_d);
	emit_alu(c, VA_OP_ISUB_U32, magnitude_d, magnitude_d, sign_d, VA_OPERAND_NONE);

	/* Divides the magnitudes. */
	emit_unsigned_divide(c, quotient, remainder, magnitude_n, magnitude_d);

	/* Gives the result its sign: (x ^ s) - s negates x where s is all ones. */
	if (op == DRV_GPU_IR_IDIV) {
		emit_alu(c, VA_OP_LSHIFT_XOR_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), sign_n, sign_n, zero, sign_d);
		emit_alu(c, VA_OP_LSHIFT_XOR_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), quotient, quotient, zero, sign_n);
		emit_alu(c, VA_OP_ISUB_U32, dst, quotient, sign_n, VA_OPERAND_NONE);
	} else {
		emit_alu(c, VA_OP_LSHIFT_XOR_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), remainder, remainder, zero, sign_n);
		emit_alu(c, VA_OP_ISUB_U32, dst, remainder, sign_n, VA_OPERAND_NONE);
	}
	free_reg(c, quotient);
	free_reg(c, remainder);
	free_reg(c, sign_n);
	free_reg(c, sign_d);
	free_reg(c, magnitude_n);
	free_reg(c, magnitude_d);
}

/*
 * Divides unsigned 32-bit integers.
 *
 * The float reciprocal of d, made a little smaller (by 2^-20, more than
 * the conversions' and the reciprocal's errors together), never
 * overestimates a quotient: the estimate n * (1 / d), truncated, leaves a
 * remainder in [0, 2^-19 n + d).  Two refinements with the remainder's
 * own estimate leave it below d + 1, and two corrections by one bring it
 * into [0, d).
 */
static void
emit_unsigned_divide(
	struct va_compiler *c,
	uint32_t quotient,
	uint32_t remainder,
	uint32_t n,
	uint32_t d)
{
	uint32_t reciprocal;
	uint32_t scratch;
	uint32_t mask;
	uint32_t round;
	uint32_t zero;

	/* Allocates the reciprocal and two scratch registers. */
	reciprocal = alloc_block(c, 1U);
	scratch = alloc_block(c, 1U);
	mask = alloc_block(c, 1U);
	if (c->error != 0)
		return;
	zero = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO;

	/* The reduced reciprocal (1 - 2^-20) / d. */
	emit_alu(c, VA_OP_U32_TO_F32 | va_field(VA_ROUND_RTE, VA_SHIFT_ROUND_MODE), scratch, d, VA_OPERAND_NONE, VA_OPERAND_NONE);
	emit_alu(c, VA_OP_FRCP_F32, reciprocal, scratch, VA_OPERAND_NONE, VA_OPERAND_NONE);
	emit_constant(c, scratch, VA_FLOAT_ONE_LESS_2_POW_20);
	emit_fmul(c, reciprocal, reciprocal, scratch);

	/* The estimate, and the remainder it leaves. */
	emit_alu(c, VA_OP_U32_TO_F32 | va_field(VA_ROUND_RTE, VA_SHIFT_ROUND_MODE), scratch, n, VA_OPERAND_NONE, VA_OPERAND_NONE);
	emit_fmul(c, scratch, scratch, reciprocal);
	emit_alu(c, VA_OP_F32_TO_U32 | va_field(VA_ROUND_RTZ, VA_SHIFT_ROUND_MODE), quotient, scratch, VA_OPERAND_NONE, VA_OPERAND_NONE);
	emit_alu(c, VA_OP_IMUL_I32, scratch, quotient, d, VA_OPERAND_NONE);
	emit_alu(c, VA_OP_ISUB_U32, remainder, n, scratch, VA_OPERAND_NONE);

	/* Refines twice with the remainder's own estimate. */
	for (round = 0U; round < 2U; round++) {
		emit_alu(c, VA_OP_U32_TO_F32 | va_field(VA_ROUND_RTE, VA_SHIFT_ROUND_MODE), scratch, remainder, VA_OPERAND_NONE, VA_OPERAND_NONE);
		emit_fmul(c, scratch, scratch, reciprocal);
		emit_alu(c, VA_OP_F32_TO_U32 | va_field(VA_ROUND_RTZ, VA_SHIFT_ROUND_MODE), scratch, scratch, VA_OPERAND_NONE, VA_OPERAND_NONE);
		emit_alu(c, VA_OP_IADD_U32, quotient, quotient, scratch, VA_OPERAND_NONE);
		emit_alu(c, VA_OP_IMUL_I32, scratch, quotient, d, VA_OPERAND_NONE);
		emit_alu(c, VA_OP_ISUB_U32, remainder, n, scratch, VA_OPERAND_NONE);
	}

	/* Corrects up by one, twice, while the remainder is at least d. */
	for (round = 0U; round < 2U; round++) {
		emit_alu(c, VA_OP_ICMP_OR_U32 | va_field(VA_CONDITION_GE, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE), mask, remainder, d, zero);
		emit_alu(c, VA_OP_ISUB_U32, quotient, quotient, mask, VA_OPERAND_NONE);
		emit_alu(c, VA_OP_LSHIFT_AND_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), scratch, d, zero, mask);
		emit_alu(c, VA_OP_ISUB_U32, remainder, remainder, scratch, VA_OPERAND_NONE);
	}
	free_reg(c, reciprocal);
	free_reg(c, scratch);
	free_reg(c, mask);
}

/* Shifts a by the low five bits of amount. */
static void
emit_shift(
	struct va_compiler *c,
	uint64_t op,
	uint32_t dst,
	uint32_t a,
	uint32_t amount)
{
	uint32_t mask;
	uint32_t masked;

	/* Keeps the amount modulo 32. */
	mask = temp_constant(c, 31U);
	masked = alloc_block(c, 1U);
	if (c->error != 0)
		return;
	emit_alu(c, VA_OP_LSHIFT_AND_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), masked, amount, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO, mask);

	/* Shifts, or-ing in zero. */
	emit_alu(c, op | va_field(VA_LANES_B00, VA_SHIFT_LANES), dst, a, masked, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO);
	free_reg(c, mask);
	free_reg(c, masked);
}

/* Lowers a LOAD_INPUT by the stage. */
static void
lower_load_input(
	struct va_compiler *c,
	uint32_t index)
{
	const struct drv_gpu_shader_ir_inst *inst;

	inst = &c->ir->instructions[index];

	/* A builtin input comes from the preloaded registers or a special varying. */
	if (inst->location >= MALI_SHADER_LOCATIONS) {
		lower_special_input(c, inst);
		return;
	}

	/* A located input is a vertex attribute or a fragment varying. */
	if (c->ir->stage == DRV_GPU_STAGE_VERTEX) {
		lower_vertex_attribute(c, index);
	} else if (c->ir->stage == DRV_GPU_STAGE_FRAGMENT) {
		lower_varying(c, index);
	} else {
		fail(c, EINVAL, "a compute shader with an input");
	}
}

/*
 * Finds the run of LOAD_INPUTs of the same location that starts at index
 * and the highest component it loads, marking the others done.
 */
static uint32_t
input_run(
	struct va_compiler *c,
	uint32_t index,
	uint32_t *last)
{
	const struct drv_gpu_shader_ir_inst *first;
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t highest;
	uint32_t next;

	first = &c->ir->instructions[index];
	highest = first->component;

	/* Takes every following load of the same location. */
	for (next = index + 1U; next < c->ir->instruction_count; next++) {
		inst = &c->ir->instructions[next];
		if (inst->op != DRV_GPU_IR_LOAD_INPUT || inst->location != first->location)
			break;
		if (inst->component > highest)
			highest = inst->component;
	}
	*last = next;

	/* Reports the highest component. */
	return highest;
}

/* Gives the values of a run of loads the registers of the block they were loaded into. */
static void
bind_run(
	struct va_compiler *c,
	uint32_t index,
	uint32_t last,
	uint32_t block,
	uint32_t count)
{
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t taken;
	uint32_t next;
	uint32_t component;

	/* Each load's value takes its component's register; a second load of a component copies it. */
	taken = 0U;
	for (next = index; next < last; next++) {
		inst = &c->ir->instructions[next];
		component = inst->component;
		if ((taken & (1U << component)) == 0U) {
			bind_value(c, inst->dst, block + component);
			taken |= 1U << component;
		} else {
			emit_move(c, define_value(c, inst->dst), block + component);
		}
		c->done[next] = 1U;
	}

	/* The block's registers no value took are free again. */
	for (component = 0U; component < count; component++) {
		if ((taken & (1U << component)) == 0U)
			free_reg(c, block + component);
	}
}

/*
 * Loads a vertex attribute: LD_ATTR_IMM from the attribute table at the
 * location's entry, for the vertex and instance index the hardware
 * preloads, converted as the descriptor's format says (auto).
 */
static void
lower_vertex_attribute(
	struct va_compiler *c,
	uint32_t index)
{
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t highest;
	uint32_t last;
	uint32_t count;
	uint32_t block;
	uint64_t bits;

	inst = &c->ir->instructions[index];

	/* Loads every component the run reads, from the first. */
	highest = input_run(c, index, &last);
	count = highest + 1U;
	block = alloc_block(c, count);
	if (c->error != 0)
		return;
	bits = VA_OP_LD_ATTR_IMM;
	bits |= va_source_register(VA_PRELOAD_VERTEX_ID);
	bits |= va_field(va_source_register(VA_PRELOAD_INSTANCE_ID), VA_SHIFT_SOURCE1);
	bits |= va_field(MALI_SHADER_TABLE_ATTRIBUTES, VA_SHIFT_ATTR_TABLE);
	bits |= va_field(inst->location, VA_SHIFT_ATTR_INDEX);
	bits |= va_field(VA_REGISTER_FORMAT_AUTO, VA_SHIFT_REGISTER_FORMAT);
	bits |= va_field(count - 1U, VA_SHIFT_VECTOR_SIZE);
	bits |= va_staging(block, count, VA_STAGING_WRITE);
	emit(c,
	     bits,
	     va_register_mask(VA_PRELOAD_VERTEX_ID, 2U),
	     va_register_mask(block, count),
	     VA_INST_MESSAGE);
	c->preload |= 3U << (VA_PRELOAD_VERTEX_ID - VA_PRELOAD_FIRST);

	/* Gives the loaded components to the run's values. */
	bind_run(c, index, last, block, count);
}

/*
 * Loads a fragment varying: LD_VAR_BUF_IMM of the location's 16 bytes of
 * the varying record, interpolated at the centre (or flat), and a
 * NoPerspective one multiplied by the fragment's 1 / w.
 *
 * Procedure referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/bifrost_compile.c (bi_emit_load_var_buf)
 * and src/panfrost/compiler/pan_nir_lower_noperspective.c.
 */
static void
lower_varying(
	struct va_compiler *c,
	uint32_t index)
{
	const struct drv_gpu_shader_ir_inst *inst;
	const struct drv_gpu_shader_ir_io *io;
	uint32_t highest;
	uint32_t last;
	uint32_t count;
	uint32_t block;
	uint32_t component;
	uint32_t w;
	uint32_t flat;
	uint32_t noperspective;
	uint32_t entry;
	uint64_t bits;

	inst = &c->ir->instructions[index];

	/* Finds how the location is interpolated. */
	flat = 0U;
	noperspective = 0U;
	for (entry = 0U; entry < c->ir->input_count; entry++) {
		io = &c->ir->inputs[entry];
		if (io->location == inst->location) {
			flat = io->flat;
			noperspective = io->noperspective;
		}
	}

	/* Loads every component the run reads, from the first. */
	highest = input_run(c, index, &last);
	count = highest + 1U;
	block = alloc_block(c, count);
	if (c->error != 0)
		return;
	bits = VA_OP_LD_VAR_BUF_IMM_F32;
	if (flat != 0U) {
		bits |= operand_byte(VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO);
		bits |= va_field(VA_SOURCE_FORMAT_FLAT32, VA_SHIFT_SOURCE_FORMAT);
	} else {
		bits |= va_source_register(VA_PRELOAD_VARYING_SOURCE);
		bits |= va_field(VA_SOURCE_FORMAT_F32, VA_SHIFT_SOURCE_FORMAT);
		c->preload |= 1U << (VA_PRELOAD_VARYING_SOURCE - VA_PRELOAD_FIRST);
	}
	bits |= va_field(inst->location * MALI_SHADER_VARYING_STRIDE, VA_SHIFT_VARYING_INDEX);
	bits |= va_field(count - 1U, VA_SHIFT_VECTOR_SIZE);
	bits |= va_field(VA_UPDATE_STORE, VA_SHIFT_UPDATE);
	bits |= va_field(VA_SAMPLE_CENTER, VA_SHIFT_SAMPLE);
	bits |= va_staging(block, count, VA_STAGING_WRITE);
	emit(c,
	     bits,
	     va_register_mask(VA_PRELOAD_VARYING_SOURCE, 1U),
	     va_register_mask(block, count),
	     VA_INST_MESSAGE | VA_INST_VARYING);

	/* Undoes the vertex program's multiplication by w of a NoPerspective varying. */
	if (noperspective != 0U && flat == 0U) {
		w = alloc_block(c, 1U);
		if (c->error != 0)
			return;
		bits = VA_OP_LD_VAR_SPECIAL | operand_byte(VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO);
		bits |= va_field(2U, VA_SHIFT_SPECIAL_INDEX);
		bits |= va_field(VA_REGISTER_FORMAT_F32, VA_SHIFT_REGISTER_FORMAT);
		bits |= va_field(VA_UPDATE_CLOBBER, VA_SHIFT_UPDATE);
		bits |= va_staging(w, 1U, VA_STAGING_WRITE);
		emit(c, bits, 0U, va_register_mask(w, 1U), VA_INST_MESSAGE | VA_INST_VARYING);
		for (component = 0U; component < count; component++)
			emit_fmul(c, block + component, block + component, w);
		free_reg(c, w);
	}

	/* Gives the loaded components to the run's values. */
	bind_run(c, index, last, block, count);
}

/*
 * Reads a builtin input: the vertex and instance index from r60 and r61,
 * a fragment's coordinate (x and y from the pixel position in r59, at the
 * pixel's centre; z and w from the special varyings), its point
 * coordinate, its primitive's number from r57, the samples it covers
 * and the number of the sample it runs for from r61.
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/ISA.xml (LD_VAR_SPECIAL: index
 * 0 and 1 the point coordinate, 2 the fragment's w, 3 its z).
 */
static void
lower_special_input(
	struct va_compiler *c,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t dst;
	uint32_t half;
	uint32_t special;
	uint32_t shift;
	uint32_t mask;
	uint64_t bits;

	dst = define_value(c, inst->dst);
	if (c->error != 0)
		return;

	/* The vertex and instance index are preloaded. */
	if (inst->location == DRV_GPU_SHADER_LOCATION_VERTEX_INDEX) {
		emit_move(c, dst, VA_PRELOAD_VERTEX_ID);
		c->preload |= 1U << (VA_PRELOAD_VERTEX_ID - VA_PRELOAD_FIRST);
		c->program->reads_vertex_index = 1U;
		return;
	}
	if (inst->location == DRV_GPU_SHADER_LOCATION_INSTANCE_INDEX) {
		emit_move(c, dst, VA_PRELOAD_INSTANCE_ID);
		c->preload |= 1U << (VA_PRELOAD_INSTANCE_ID - VA_PRELOAD_FIRST);
		c->program->reads_instance_index = 1U;
		return;
	}

	/* A front face has bit 0 of the primitive flags clear. */
	if (inst->location == DRV_GPU_SHADER_LOCATION_FRONT_FACING) {
		emit_alu(c,
			 VA_OP_ICMP_OR_U32 | va_field(VA_CONDITION_EQ, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE),
			 dst,
			 VA_PRELOAD_PRIMITIVE_FLAGS,
			 VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO,
			 VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO);
		c->preload |= 1U << (VA_PRELOAD_PRIMITIVE_FLAGS - VA_PRELOAD_FIRST);
		return;
	}

	/* A fragment's primitive number is preloaded in r57. */
	if (inst->location == DRV_GPU_SHADER_LOCATION_PRIMITIVE_ID) {
		emit_move(c, dst, VA_PRELOAD_WORKGROUP_X);
		c->preload |= 1U << (VA_PRELOAD_WORKGROUP_X - VA_PRELOAD_FIRST);
		return;
	}

	/* The fragment coordinate's x and y: the pixel's integer position plus a half. */
	if (inst->location == DRV_GPU_SHADER_LOCATION_FRAG_COORD && inst->component < 2U) {
		emit(c,
		     VA_OP_U16_TO_F32 | va_source_register(VA_PRELOAD_POSITION_XY) |
		     va_field(inst->component, VA_SHIFT_LANE_SOURCE0) | va_dest(dst),
		     va_register_mask(VA_PRELOAD_POSITION_XY, 1U),
		     va_register_mask(dst, 1U),
		     0U);
		half = temp_constant(c, VA_FLOAT_HALF);
		emit_alu(c, VA_OP_FADD_F32, dst, dst, half, VA_OPERAND_NONE);
		free_reg(c, half);
		c->preload |= 1U << (VA_PRELOAD_POSITION_XY - VA_PRELOAD_FIRST);
		c->program->reads_frag_coord = 1U;
		return;
	}

	/*
	 * gl_SampleMaskIn: the samples the rasterizer found covered, bits
	 * 15:0 of r61, widened.
	 *
	 * Procedure referenced: Mesa 26.2.6
	 * src/panfrost/compiler/bifrost/bifrost_compile.c (the
	 * load_sample_mask_in intrinsic).
	 */
	if (inst->location == DRV_GPU_SHADER_LOCATION_SAMPLE_MASK) {
		emit(c,
		     VA_OP_U16_TO_U32 | va_source_register(VA_PRELOAD_RASTER_COVERAGE) | va_dest(dst),
		     va_register_mask(VA_PRELOAD_RASTER_COVERAGE, 1U),
		     va_register_mask(dst, 1U),
		     0U);
		c->preload |= 1U << (VA_PRELOAD_RASTER_COVERAGE - VA_PRELOAD_FIRST);
		c->program->reads_sample_mask = 1U;
		return;
	}

	/*
	 * gl_SampleID: bits 20:16 of r61 (the sample's number is bits 23:16,
	 * whose top bits are not to be trusted).
	 *
	 * Procedure referenced: Mesa 26.2.6
	 * src/panfrost/compiler/bifrost/bifrost_compile.c
	 * (bi_load_sample_id_to: the shift by 16 and the 5-bit mask).
	 */
	if (inst->location == DRV_GPU_SHADER_LOCATION_SAMPLE_ID) {
		shift = temp_constant(c, VA_SAMPLE_ID_SHIFT);
		mask = temp_constant(c, VA_SAMPLE_ID_MASK);
		if (c->error != 0)
			return;
		emit_alu(c,
			 VA_OP_RSHIFT_AND_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES),
			 dst,
			 VA_PRELOAD_SAMPLE_ID,
			 shift,
			 mask);
		free_reg(c, shift);
		free_reg(c, mask);
		c->preload |= 1U << (VA_PRELOAD_SAMPLE_ID - VA_PRELOAD_FIRST);
		c->program->reads_sample_id = 1U;
		return;
	}

	/* The fragment coordinate's z and w, and the point coordinate, are special varyings. */
	if (inst->location == DRV_GPU_SHADER_LOCATION_FRAG_COORD) {
		special = 3U;
		if (inst->component == 3U)
			special = 2U;
		c->program->reads_frag_coord = 1U;
	} else if (inst->location == DRV_GPU_SHADER_LOCATION_POINT_COORD && inst->component < 2U) {
		special = inst->component;
	} else {
		fail(c, ENOTSUP, "a builtin input the Valhall lowering does not have");
		return;
	}
	bits = VA_OP_LD_VAR_SPECIAL | operand_byte(VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO);
	bits |= va_field(special, VA_SHIFT_SPECIAL_INDEX);
	bits |= va_field(VA_REGISTER_FORMAT_F32, VA_SHIFT_REGISTER_FORMAT);
	bits |= va_field(VA_UPDATE_CLOBBER, VA_SHIFT_UPDATE);
	bits |= va_staging(dst, 1U, VA_STAGING_WRITE);
	emit(c, bits, 0U, va_register_mask(dst, 1U), VA_INST_MESSAGE | VA_INST_VARYING);
}

/*
 * Loads uniform buffer words: one LOAD from the buffer's address in the
 * FAU at the first word's byte offset (a 16-bit immediate; a farther
 * word's address is computed), of the word alone or, with
 * MALI_SHADER_OPTION_VECTOR_UBO, of the run of following words
 * ubo_run() finds, into a block whose registers the run's values take.
 */
static void
lower_load_ubo(
	struct va_compiler *c,
	uint32_t index)
{
	static const uint64_t loads[4] = {
		VA_OP_LOAD_I32,
		VA_OP_LOAD_I64 | ((uint64_t)VA_LOAD_LANE_64_IDENTITY << VA_SHIFT_LOAD_LANE),
		VA_OP_LOAD_I96 | ((uint64_t)VA_LOAD_LANE_96_IDENTITY << VA_SHIFT_LOAD_LANE),
		VA_OP_LOAD_I128 | ((uint64_t)VA_LOAD_LANE_128_IDENTITY << VA_SHIFT_LOAD_LANE)
	};
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t slot;
	uint32_t word;
	uint32_t dst;
	uint32_t count;
	uint32_t next;
	uint32_t offset;
	uint32_t address;
	uint64_t bits;

	inst = &c->ir->instructions[index];

	/* Finds the buffer's address word. */
	slot = buffer_slot(c, inst->location);
	if (c->error != 0)
		return;
	word = c->program->buffers[slot].fau_word;

	/* Gives the word its register, or the run's words a block of them. */
	count = ubo_run(c, index);
	if (count == 1U) {
		dst = define_value(c, inst->dst);
	} else {
		dst = alloc_block(c, count);
	}
	if (c->error != 0)
		return;

	/* The run's values take the block's registers; their own loads are done. */
	if (count != 1U)
		bind_value(c, inst->dst, dst);
	for (next = 1U; next < count; next++) {
		bind_value(c, c->ir->instructions[index + next].dst, dst + next);
		c->done[index + next] = 1U;
	}

	/* A near offset loads from the FAU address directly. */
	if (inst->immediate < 0x8000U) {
		bits = loads[count - 1U] | va_source_uniform(word);
		bits |= va_field(va_uniform_page(word), VA_SHIFT_FAU_PAGE);
		bits |= va_field(inst->immediate, VA_SHIFT_MEMORY_OFFSET);
		bits |= va_field(1U, VA_SHIFT_LOAD_UNSIGNED);
		bits |= va_staging(dst, count, VA_STAGING_WRITE);
		emit(c, bits, 0U, va_register_mask(dst, count), VA_INST_MESSAGE);
		return;
	}

	/* A far offset adds it to the address first. */
	offset = temp_constant(c, inst->immediate);
	address = buffer_address(c, inst->location, offset);
	free_reg(c, offset);
	if (c->error != 0)
		return;
	bits = loads[count - 1U] | va_source_register(address);
	bits |= va_field(1U, VA_SHIFT_LOAD_UNSIGNED);
	bits |= va_staging(dst, count, VA_STAGING_WRITE);
	emit(c, bits, va_register_mask(address, 2U), va_register_mask(dst, count), VA_INST_MESSAGE);
	free_reg(c, address);
	free_reg(c, address + 1U);
}

/*
 * Counts the uniform buffer words one load reads from the LOAD_UBO at
 * index: with MALI_SHADER_OPTION_VECTOR_UBO, it and the LOAD_UBOs right
 * after it of the next words of the same buffer, up to four, kept to two
 * when the first word's offset is not aligned to 16 bytes and to one
 * when it is not aligned to 8 (a load is aligned to its size rounded up
 * to a power of two); otherwise one.
 *
 * Procedure referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/bifrost_nir.c (mem_access_size_align_cb:
 * at most 16 bytes, aligned to the next power of two of their size).
 */
static uint32_t
ubo_run(
	const struct va_compiler *c,
	uint32_t index)
{
	const struct drv_gpu_shader_ir_inst *first;
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t count;

	/* Without the option every word is a load of its own. */
	if ((c->options & MALI_SHADER_OPTION_VECTOR_UBO) == 0U)
		return 1U;

	/* Takes each following load of the next word of the same buffer. */
	first = &c->ir->instructions[index];
	count = 1U;
	while (count < 4U && index + count < c->ir->instruction_count) {
		inst = &c->ir->instructions[index + count];
		if (inst->op != DRV_GPU_IR_LOAD_UBO || inst->location != first->location)
			break;
		if (inst->immediate != first->immediate + 4U * count || inst->dst > c->ir->value_count)
			break;
		count++;
	}

	/* Three or four words need 16-byte alignment, two need 8. */
	if (count > 2U && (first->immediate & 15U) != 0U)
		count = 2U;
	if (count == 2U && (first->immediate & 7U) != 0U)
		count = 1U;

	/* Reports the words of the load. */
	return count;
}

/*
 * Computes the 64-bit address of a byte of a buffer (or, for the shared
 * location, of the workgroup's shared memory) into a fresh register pair:
 * SHADDX.u64 adds the 32-bit offset, zero-extended, to the base.
 */
static uint32_t
buffer_address(
	struct va_compiler *c,
	uint32_t uniform,
	uint32_t offset_reg)
{
	uint32_t pair;
	uint32_t slot;
	uint32_t word;
	uint64_t bits;

	/* Allocates the address's register pair. */
	pair = alloc_block(c, 2U);
	if (c->error != 0)
		return 0U;

	/* Adds the offset to the shared memory's base, or to the buffer's FAU address. */
	bits = VA_OP_SHADDX_U64 | va_dest(pair);
	if (uniform == DRV_GPU_IR_LOCATION_SHARED) {
		bits |= va_source_special(VA_SPECIAL_WORKGROUP_LOCAL, 0U);
		bits |= va_field(1U, VA_SHIFT_FAU_PAGE);
	} else {
		slot = buffer_slot(c, uniform);
		if (c->error != 0)
			return 0U;
		word = c->program->buffers[slot].fau_word;
		bits |= va_source_uniform(word);
		bits |= va_field(va_uniform_page(word), VA_SHIFT_FAU_PAGE);
	}
	bits |= va_field(va_source_register(offset_reg), VA_SHIFT_SOURCE1);
	bits |= va_field(VA_WIDEN64_W0, VA_SHIFT_WIDEN_SOURCE1);
	emit(c, bits, va_register_mask(offset_reg, 1U), va_register_mask(pair, 2U), 0U);

	/* Reports the pair. */
	return pair;
}

/*
 * Lowers a storage buffer or shared memory load or store: the address
 * from the buffer's base and the byte offset, then LOAD.i32 or
 * STORE.i32; a predicated access is branched around for the lanes whose
 * predicate is false.
 */
static void
lower_storage(
	struct va_compiler *c,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t location;
	uint32_t address;
	uint32_t dst;
	uint32_t data;
	uint32_t branch;
	uint32_t predicated;
	uint32_t predicate;
	uint64_t bits;

	/* Finds the memory and whether the access is predicated. */
	location = inst->location;
	if (inst->op == DRV_GPU_IR_LOAD_SHARED || inst->op == DRV_GPU_IR_STORE_SHARED)
		location = DRV_GPU_IR_LOCATION_SHARED;
	predicated = 0U;
	predicate = 0U;
	if (inst->component == 1U) {
		predicated = 1U;
		if (inst->op == DRV_GPU_IR_LOAD_STORAGE || inst->op == DRV_GPU_IR_LOAD_SHARED) {
			predicate = inst->src[1];
		} else {
			predicate = inst->src[2];
		}
	}

	/* A load's value keeps its register, so lanes that skip it keep what they had. */
	dst = VA_NO_REG;
	if (inst->op == DRV_GPU_IR_LOAD_STORAGE || inst->op == DRV_GPU_IR_LOAD_SHARED) {
		dst = c->reg[inst->dst];
		if (dst == VA_NO_REG)
			dst = define_value(c, inst->dst);
	}

	/* Computes the address. */
	address = buffer_address(c, location, value_reg(c, inst->src[0]));
	if (c->error != 0)
		return;

	/* A spilled stored value is loaded before the block that stores it. */
	if (dst == VA_NO_REG)
		reload_before_branch(c, inst->src[1]);

	/* Skips the access for the lanes whose predicate is false. */
	branch = 0U;
	if (predicated != 0U)
		branch_begin(c, value_reg(c, predicate), &branch);

	/* Loads or stores the word. */
	if (dst != VA_NO_REG) {
		bits = VA_OP_LOAD_I32 | va_source_register(address);
		bits |= va_field(1U, VA_SHIFT_LOAD_UNSIGNED);
		bits |= va_staging(dst, 1U, VA_STAGING_WRITE);
		emit(c, bits, va_register_mask(address, 2U), va_register_mask(dst, 1U), VA_INST_MESSAGE | VA_INST_MEMORY);
	} else {
		data = value_reg(c, inst->src[1]);
		bits = VA_OP_STORE_I32 | va_source_register(address);
		bits |= va_staging(data, 1U, VA_STAGING_READ);
		emit(c, bits, va_register_mask(address, 2U) | va_register_mask(data, 1U), 0U, VA_INST_MESSAGE | VA_INST_MEMORY);
	}

	/* Rejoins the lanes that skipped it. */
	if (predicated != 0U)
		branch_end(c, branch);
	free_reg(c, address);
	free_reg(c, address + 1U);
}

/*
 * Lowers an atomic operation on a storage buffer or shared memory word:
 * ATOM_RETURN.i32 reads the operand (for a compare and exchange, the new
 * value and then the value compared, with the compare bit) from its
 * staging registers and writes the old value; a subtraction adds the
 * negated operand.
 *
 * Procedure referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/bifrost_compile.c (bi_emit_acmpxchg_to:
 * the order of the two staging words) and valhall/va_pack.c (the compare
 * bit, the read and write staging of ATOM_RETURN).
 */
static void
lower_atomic(
	struct va_compiler *c,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t operation;
	uint32_t address;
	uint32_t dst;
	uint32_t block;
	uint32_t count;
	uint32_t branch;
	uint32_t predicate;
	uint32_t compare;
	uint32_t slot;
	uint32_t coordinates;
	uint64_t bits;

	/* Chooses the hardware operation. */
	compare = 0U;
	switch (inst->immediate) {
	case DRV_GPU_IR_ATOMIC_ADD:
	case DRV_GPU_IR_ATOMIC_SUB:
		operation = VA_ATOMIC_ADD;
		break;

	case DRV_GPU_IR_ATOMIC_SMIN:
		operation = VA_ATOMIC_SMIN;
		break;

	case DRV_GPU_IR_ATOMIC_SMAX:
		operation = VA_ATOMIC_SMAX;
		break;

	case DRV_GPU_IR_ATOMIC_UMIN:
		operation = VA_ATOMIC_UMIN;
		break;

	case DRV_GPU_IR_ATOMIC_UMAX:
		operation = VA_ATOMIC_UMAX;
		break;

	case DRV_GPU_IR_ATOMIC_AND:
		operation = VA_ATOMIC_AND;
		break;

	case DRV_GPU_IR_ATOMIC_OR:
		operation = VA_ATOMIC_OR;
		break;

	case DRV_GPU_IR_ATOMIC_XOR:
		operation = VA_ATOMIC_XOR;
		break;

	case DRV_GPU_IR_ATOMIC_XCHG:
		operation = VA_ATOMIC_XCHG;
		break;

	case DRV_GPU_IR_ATOMIC_CMPXCHG:
		operation = VA_ATOMIC_XCHG;
		compare = 1U;
		break;

	default:
		fail(c, ENOTSUP, "an atomic operation the Valhall lowering does not have");
		return;
	}

	/* Computes the address of the buffer's word, or of the storage image's texel. */
	if ((inst->image_shape & DRV_GPU_IR_IMAGE_TEXEL) != 0U) {
		slot = image_slot(c, inst->location);
		if (c->error != 0)
			return;
		coordinates = image_coordinates(c, inst);
		if (c->error != 0)
			return;
		address = image_address(c, slot, coordinates);
		if (c->error != 0)
			return;
		free_reg(c, address + 2U);
	} else {
		address = buffer_address(c, inst->location, value_reg(c, inst->src[0]));
	}

	/* Copies the operands into the staging block. */
	count = 1U + compare;
	block = alloc_block(c, count);
	dst = define_value(c, inst->dst);
	if (c->error != 0)
		return;
	if (inst->immediate == DRV_GPU_IR_ATOMIC_SUB) {
		emit_alu(c, VA_OP_ISUB_U32, block, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO, value_operand(c, inst->src[1]), VA_OPERAND_NONE);
	} else {
		emit_move(c, block, value_operand(c, inst->src[1]));
	}
	if (compare != 0U)
		emit_move(c, block + 1U, value_operand(c, inst->src[2]));

	/* Skips the operation for the lanes whose predicate is false. */
	branch = 0U;
	if (inst->component == DRV_GPU_IR_ATOMIC_PREDICATED) {
		predicate = value_reg(c, inst->src[2 + compare]);
		branch_begin(c, predicate, &branch);
	}

	/* Operates, the old value to the result. */
	bits = VA_OP_ATOM_RETURN_I32 | va_source_register(address);
	bits |= va_field(operation, VA_SHIFT_ATOMIC_OPERATION);
	bits |= va_field(compare, VA_SHIFT_ATOMIC_COMPARE);
	bits |= va_field(0U, VA_SHIFT_STAGING_WRITE_COUNT);
	bits |= va_field(dst, VA_SHIFT_STAGING_DEST);
	bits |= va_staging(block, count, VA_STAGING_READ | VA_STAGING_WRITE);
	emit(c,
	     bits,
	     va_register_mask(address, 2U) | va_register_mask(block, count),
	     va_register_mask(dst, 1U) | va_register_mask(block, count),
	     VA_INST_MESSAGE | VA_INST_MEMORY);

	/* Rejoins the lanes that skipped it. */
	if (inst->component == DRV_GPU_IR_ATOMIC_PREDICATED)
		branch_end(c, branch);
	free_reg(c, address);
	free_reg(c, address + 1U);
	free_reg(c, block);
	if (compare != 0U)
		free_reg(c, block + 1U);
}

/*
 * Reads a compute invocation's builtin: the local id's 16-bit halves of
 * r55 and r56 (MKVEC with zero), its linear index, or the workgroup id
 * from r57 to r59.
 *
 * Procedure referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/valhall/test/assembler-cases.txt
 * (MKVEC.v2i16 r2, r55^.h0, 0x0.h0 and its h1 form).
 */
static void
lower_system(
	struct va_compiler *c,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t dst;
	uint32_t y;
	uint32_t z;
	uint32_t size;
	uint32_t component;

	dst = define_value(c, inst->dst);
	if (c->error != 0)
		return;
	component = inst->component;

	/* The workgroup id is preloaded, one register per axis. */
	if (component >= DRV_GPU_IR_SYSTEM_GROUP_ID_X && component <= DRV_GPU_IR_SYSTEM_GROUP_ID_Z) {
		emit_move(c, dst, VA_PRELOAD_WORKGROUP_X + component - DRV_GPU_IR_SYSTEM_GROUP_ID_X);
		c->preload |= 1U << (VA_PRELOAD_WORKGROUP_X + component - DRV_GPU_IR_SYSTEM_GROUP_ID_X - VA_PRELOAD_FIRST);
		return;
	}

	/* A local id axis is one zero-extended half. */
	if (component == DRV_GPU_IR_SYSTEM_LOCAL_ID_X) {
		emit_alu(c, VA_OP_MKVEC_V2I16, dst, VA_PRELOAD_LOCAL_ID_XY, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO, VA_OPERAND_NONE);
		c->preload |= 1U << (VA_PRELOAD_LOCAL_ID_XY - VA_PRELOAD_FIRST);
		return;
	}
	if (component == DRV_GPU_IR_SYSTEM_LOCAL_ID_Y) {
		emit_alu(c, VA_OP_MKVEC_V2I16 | va_field(1U, VA_SHIFT_LANE_SOURCE0), dst, VA_PRELOAD_LOCAL_ID_XY, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO, VA_OPERAND_NONE);
		c->preload |= 1U << (VA_PRELOAD_LOCAL_ID_XY - VA_PRELOAD_FIRST);
		return;
	}
	if (component == DRV_GPU_IR_SYSTEM_LOCAL_ID_Z) {
		emit_alu(c, VA_OP_MKVEC_V2I16, dst, VA_PRELOAD_LOCAL_ID_Z, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO, VA_OPERAND_NONE);
		c->preload |= 1U << (VA_PRELOAD_LOCAL_ID_Z - VA_PRELOAD_FIRST);
		return;
	}

	/* The linear index is x + y * size x + z * size x * size y. */
	if (component == DRV_GPU_IR_SYSTEM_LOCAL_INDEX) {
		y = alloc_block(c, 1U);
		z = alloc_block(c, 1U);
		if (c->error != 0)
			return;
		emit_alu(c, VA_OP_MKVEC_V2I16, dst, VA_PRELOAD_LOCAL_ID_XY, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO, VA_OPERAND_NONE);
		emit_alu(c, VA_OP_MKVEC_V2I16 | va_field(1U, VA_SHIFT_LANE_SOURCE0), y, VA_PRELOAD_LOCAL_ID_XY, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO, VA_OPERAND_NONE);
		emit_alu(c, VA_OP_MKVEC_V2I16, z, VA_PRELOAD_LOCAL_ID_Z, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO, VA_OPERAND_NONE);
		size = temp_constant(c, c->ir->local_size[0]);
		emit_alu(c, VA_OP_IMUL_I32, y, y, size, VA_OPERAND_NONE);
		emit_alu(c, VA_OP_IADD_U32, dst, dst, y, VA_OPERAND_NONE);
		emit_constant(c, size, c->ir->local_size[0] * c->ir->local_size[1]);
		emit_alu(c, VA_OP_IMUL_I32, z, z, size, VA_OPERAND_NONE);
		emit_alu(c, VA_OP_IADD_U32, dst, dst, z, VA_OPERAND_NONE);
		free_reg(c, size);
		free_reg(c, y);
		free_reg(c, z);
		c->preload |= 3U << (VA_PRELOAD_LOCAL_ID_XY - VA_PRELOAD_FIRST);
		return;
	}

	/* Another builtin is not a compute one. */
	fail(c, ENOTSUP, "a system value the Valhall lowering does not have");
}

/*
 * Reads a sampler message: its kind, whether it compares, its image's
 * dimension and array, and where its coordinates, its layer, its depth
 * reference and its level of detail (or bias) are among its parameters
 * (the parameters' registers in texture->parameters).
 *
 * The TEXTURE operation's parameters are, in order: the reference of a
 * compare, the bias or level of a biased or levelled sample, then the
 * coordinates (u, v, and r of a 3D image; the direction x, y, z of a
 * cube) and the layer of an array; a texel load's are u, v, the level,
 * then r.  A sample with derivatives has each coordinate followed by
 * its x and y derivatives.  The SAMPLE operations are a 2D image's u and
 * v and the bias or level after them.
 */
static void
read_texture(
	struct va_compiler *c,
	const struct drv_gpu_shader_ir_inst *inst,
	struct va_texture *texture)
{
	uint32_t kind;
	uint32_t message;
	uint32_t component;

	/* Starts as a 2D sample of no array, computed level and no offset. */
	kern_memset(texture, 0, sizeof(*texture));
	texture->kind = VA_TEXTURE_SAMPLE;
	texture->coordinates = 2U;
	texture->dimension = VA_DIMENSION_2D;
	texture->lod_mode = VA_LOD_MODE_COMPUTED;
	texture->ref_slot = VA_NOT_DEFINED;
	texture->lod_slot = VA_NOT_DEFINED;
	texture->layer_slot = VA_NOT_DEFINED;
	texture->stride = 1U;

	/* The SAMPLE operations: a 2D image's u and v, the bias or level after them. */
	if (inst->op != DRV_GPU_IR_TEXTURE) {
		texture->offset = inst->component & 0xFFFU;
		texture->parameter_count = 2U;
		texture->parameters[0] = value_reg(c, inst->src[0]);
		texture->parameters[1] = value_reg(c, inst->src[1]);
		if (inst->op == DRV_GPU_IR_SAMPLE_BIAS || inst->op == DRV_GPU_IR_SAMPLE_LOD) {
			texture->parameters[2] = value_reg(c, inst->src[2]);
			texture->parameter_count = 3U;
			texture->lod_slot = 2U;
			texture->lod_mode = VA_LOD_MODE_COMPUTED_BIAS;
			if (inst->op == DRV_GPU_IR_SAMPLE_LOD)
				texture->lod_mode = VA_LOD_MODE_EXPLICIT;
		}
		return;
	}

	/* The image's shape: 2D, 3D or cube, of an array or not. */
	message = inst->component & DRV_GPU_IR_TEXTURE_MESSAGE_MASK;
	texture->offset = (inst->component >> 8) & 0xFFFU;
	texture->channel = DRV_GPU_IR_TEXTURE_CHANNEL(inst->component);
	kind = inst->image_shape & DRV_GPU_IR_IMAGE_KIND_MASK;
	if (kind == DRV_GPU_IR_IMAGE_3D) {
		texture->coordinates = 3U;
		texture->dimension = VA_DIMENSION_3D;
	} else if (kind == DRV_GPU_IR_IMAGE_CUBE) {
		/* A cube's direction is three parameters, sampled as two coordinates and the face. */
		texture->coordinates = 3U;
		texture->dimension = VA_DIMENSION_CUBE;
		texture->cube = 1U;
	} else if (kind != DRV_GPU_IR_IMAGE_2D) {
		fail(c, ENOTSUP, "a buffer texture");
		return;
	}
	if ((inst->image_shape & DRV_GPU_IR_IMAGE_ARRAYED) != 0U)
		texture->array = 1U;
	if ((inst->image_shape & DRV_GPU_IR_IMAGE_MULTISAMPLED) != 0U) {
		fail(c, ENOTSUP, "a multisampled texture");
		return;
	}

	/* Places the reference, the level and the coordinates by the message. */
	switch (message) {
	case DRV_GPU_IR_TEXTURE_SAMPLE:
		break;

	case DRV_GPU_IR_TEXTURE_SAMPLE_BIAS:
	case DRV_GPU_IR_TEXTURE_SAMPLE_LOD:
		texture->lod_slot = 0U;
		texture->first = 1U;
		break;

	case DRV_GPU_IR_TEXTURE_SAMPLE_COMPARE:
		texture->compare = 1U;
		texture->ref_slot = 0U;
		texture->first = 1U;
		break;

	case DRV_GPU_IR_TEXTURE_SAMPLE_BIAS_COMPARE:
	case DRV_GPU_IR_TEXTURE_SAMPLE_LOD_COMPARE:
		texture->compare = 1U;
		texture->ref_slot = 0U;
		texture->lod_slot = 1U;
		texture->first = 2U;
		break;

	case DRV_GPU_IR_TEXTURE_SAMPLE_DERIVS:
		texture->gradient = 1U;
		texture->stride = 3U;
		break;

	case DRV_GPU_IR_TEXTURE_SAMPLE_DERIV_COMPARE:
		texture->gradient = 1U;
		texture->stride = 3U;
		texture->compare = 1U;
		texture->ref_slot = 0U;
		texture->first = 1U;
		break;

	case DRV_GPU_IR_TEXTURE_LD:
		texture->kind = VA_TEXTURE_FETCH;
		texture->lod_slot = 2U;
		break;

	case DRV_GPU_IR_TEXTURE_GATHER4:
		texture->kind = VA_TEXTURE_GATHER;
		break;

	case DRV_GPU_IR_TEXTURE_GATHER4_COMPARE:
		texture->kind = VA_TEXTURE_GATHER;
		texture->compare = 1U;
		texture->ref_slot = 0U;
		texture->first = 1U;
		break;

	default:
		fail(c, ENOTSUP, "a texture message the Valhall lowering does not have");
		return;
	}

	/* A biased or levelled message's mode. */
	if (message == DRV_GPU_IR_TEXTURE_SAMPLE_BIAS || message == DRV_GPU_IR_TEXTURE_SAMPLE_BIAS_COMPARE)
		texture->lod_mode = VA_LOD_MODE_COMPUTED_BIAS;
	if (message == DRV_GPU_IR_TEXTURE_SAMPLE_LOD || message == DRV_GPU_IR_TEXTURE_SAMPLE_LOD_COMPARE)
		texture->lod_mode = VA_LOD_MODE_EXPLICIT;

	/*
	 * A sample with derivatives takes its level from a gradient
	 * descriptor; a cube's is computed and given explicitly.
	 */
	if (texture->gradient != 0U) {
		texture->lod_mode = VA_LOD_MODE_GRDESC;
		if (texture->cube != 0U)
			texture->lod_mode = VA_LOD_MODE_EXPLICIT;
	}

	/* A cube has no texel load; the layer follows the coordinates (a texel load's is its fourth). */
	if (texture->cube != 0U && texture->kind == VA_TEXTURE_FETCH) {
		fail(c, ENOTSUP, "a texel load of a cube");
		return;
	}
	if (texture->array != 0U) {
		texture->layer_slot = texture->first + texture->coordinates * texture->stride;
		if (texture->kind == VA_TEXTURE_FETCH)
			texture->layer_slot = 3U;
	}

	/* Reads the parameters' registers. */
	texture->parameter_count = inst->src[1];
	if (texture->parameter_count > DRV_GPU_IR_TEXTURE_MAX_PARAMS)
		texture->parameter_count = DRV_GPU_IR_TEXTURE_MAX_PARAMS;
	for (component = 0U; component < texture->parameter_count; component++)
		texture->parameters[component] = value_reg(c, inst->src[0] + component);
}

/*
 * Samples a texture: TEX_SINGLE (TEX_FETCH for a texel load, TEX_GATHER
 * for a gather) with the coordinates, the array layer, the depth
 * reference of a compare and the level of detail or its bias in a block
 * of staging registers, the texture's and sampler's table entries in a
 * register pair (with the constant texel offset in its high word), and
 * the results in a block of four.
 *
 * Values referenced: Mesa 26.2.6
 * src/panfrost/compiler/pan_nir_lower_tex.c (va_lower_tex: the order of
 * the staging registers, the narrow handle and offsets, the 8.8 fixed
 * level of detail, the shadow reference) and bifrost_compile.c
 * (bi_emit_tex_valhall: the gather's channel).
 */
static void
lower_texture(
	struct va_compiler *c,
	const struct drv_gpu_shader_ir_inst *inst)
{
	struct va_texture texture;
	uint32_t staging[DRV_GPU_IR_TEXTURE_MAX_PARAMS + 2U];
	uint32_t count;
	uint32_t slot;
	uint32_t handle;
	uint32_t block;
	uint32_t result;
	uint32_t component;
	uint32_t narrow;
	uint32_t lod;
	uint32_t layer_index;
	uint32_t scale;
	uint32_t write_mask;
	uint32_t write_count;
	uint32_t descriptor;
	uint32_t descriptor_index;
	uint32_t level;
	uint64_t bits;

	/* Reads the message and its parameters, and finds the texture's slot. */
	read_texture(c, inst, &texture);
	if (c->error != 0)
		return;
	slot = texture_slot(c, inst->location, inst->immediate);
	if (c->error != 0)
		return;

	/*
	 * A sample with derivatives first makes its gradient descriptor (a
	 * cube's, its level).
	 */
	descriptor = VA_NO_REG;
	level = VA_NO_REG;
	if (texture.gradient != 0U && texture.cube != 0U) {
		level = emit_cube_gradient_level(c, &texture, slot);
	} else if (texture.gradient != 0U) {
		descriptor = emit_gradient_descriptor(c, &texture, slot);
	}
	if (c->error != 0)
		return;

	/*
	 * Lists the staging registers: s, t (r), the layer, the reference,
	 * then the offsets word or the level.  A cube's s and t are made from
	 * its direction once the block is allocated.
	 */
	count = 0U;
	for (component = 0U; component < texture.coordinates; component++) {
		if (texture.cube != 0U) {
			/* The direction's three values become s (with the face) and t. */
			if (component < 2U) {
				staging[count] = VA_OPERAND_NONE;
				count++;
			}
			continue;
		}
		if (texture.kind == VA_TEXTURE_FETCH && component == 2U) {
			staging[count] = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO;
			if (texture.parameter_count > 3U)
				staging[count] = texture.parameters[3];
		} else if (texture.first + component * texture.stride < texture.parameter_count) {
			staging[count] = texture.parameters[texture.first + component * texture.stride];
		} else {
			staging[count] = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO;
		}
		count++;
	}
	layer_index = VA_NOT_DEFINED;
	if (texture.layer_slot != VA_NOT_DEFINED) {
		layer_index = count;
		staging[count] = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO;
		if (texture.layer_slot < texture.parameter_count)
			staging[count] = texture.parameters[texture.layer_slot];
		count++;
	}
	if (texture.ref_slot != VA_NOT_DEFINED) {
		staging[count] = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO;
		if (texture.ref_slot < texture.parameter_count)
			staging[count] = texture.parameters[texture.ref_slot];
		count++;
	}
	lod = VA_OPERAND_NONE;
	if (texture.lod_slot != VA_NOT_DEFINED && texture.lod_slot < texture.parameter_count)
		lod = texture.parameters[texture.lod_slot];
	if (level != VA_NO_REG)
		lod = level;
	if (lod != VA_OPERAND_NONE) {
		staging[count] = lod;
		count++;
	}

	/* The gradient descriptor's two words come last; they are copied in below. */
	descriptor_index = VA_NOT_DEFINED;
	if (descriptor != VA_NO_REG) {
		descriptor_index = count;
		staging[count] = VA_OPERAND_NONE;
		staging[count + 1U] = VA_OPERAND_NONE;
		count += 2U;
	}

	/* The high handle word holds the texel offset's signed nibbles s, t, r. */
	narrow = (texture.offset >> 8) & 0xFU;
	narrow |= ((texture.offset >> 4) & 0xFU) << 4;
	narrow |= (texture.offset & 0xFU) << 8;

	/* Writes the handle pair. */
	handle = alloc_block(c, 2U);
	if (c->error != 0)
		return;
	emit_constant(c, handle, texture_handle(slot));
	emit_constant(c, handle + 1U, narrow);

	/* Copies the staging values into a block (a cube's first two are made below). */
	block = alloc_block(c, count);
	if (c->error != 0)
		return;
	for (component = 0U; component < count; component++) {
		if (staging[component] != VA_OPERAND_NONE)
			emit_move(c, block + component, staging[component]);
	}

	/* A cube's direction becomes its face's s and t. */
	if (texture.cube != 0U) {
		emit_cube_coordinates(c,
				      block,
				      texture.parameters[texture.first],
				      texture.parameters[texture.first + texture.stride],
				      texture.parameters[texture.first + 2U * texture.stride]);
	}

	/* The gradient descriptor follows the other staging values. */
	if (descriptor != VA_NO_REG) {
		emit_move(c, block + descriptor_index, descriptor);
		emit_move(c, block + descriptor_index + 1U, descriptor + 1U);
		free_reg(c, descriptor);
		free_reg(c, descriptor + 1U);
	}

	/* A cube's computed level is in the block now. */
	if (level != VA_NO_REG)
		free_reg(c, level);

	/* A sampled array's layer is the nearest integer to the coordinate. */
	if (layer_index != VA_NOT_DEFINED && texture.kind != VA_TEXTURE_FETCH) {
		emit_alu(c, VA_OP_FROUND_F32 | va_field(VA_ROUND_RTE, VA_SHIFT_ROUND_MODE), block + layer_index, block + layer_index, VA_OPERAND_NONE, VA_OPERAND_NONE);
		emit_alu(c, VA_OP_F32_TO_U32 | va_field(VA_ROUND_RTZ, VA_SHIFT_ROUND_MODE), block + layer_index, block + layer_index, VA_OPERAND_NONE, VA_OPERAND_NONE);
	}

	/* A sampled level or bias is 8.8 fixed point (a bias in the low half, no minimum level above it). */
	if (lod != VA_OPERAND_NONE && texture.kind != VA_TEXTURE_FETCH) {
		scale = temp_constant(c, 0x43800000U);
		emit_fmul(c, block + count - 1U, block + count - 1U, scale);
		emit_alu(c, VA_OP_F32_TO_S32 | va_field(VA_ROUND_RTE, VA_SHIFT_ROUND_MODE), block + count - 1U, block + count - 1U, VA_OPERAND_NONE, VA_OPERAND_NONE);
		if (texture.lod_mode == VA_LOD_MODE_COMPUTED_BIAS) {
			emit_constant(c, scale, 0xFFFFU);
			emit_alu(c, VA_OP_LSHIFT_AND_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), block + count - 1U, block + count - 1U, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO, scale);
		}
		free_reg(c, scale);
	}

	/* A texel load's level is the top byte of its offsets word, below it the offsets. */
	if (lod != VA_OPERAND_NONE && texture.kind == VA_TEXTURE_FETCH) {
		scale = temp_constant(c, 24U);
		emit_alu(c, VA_OP_LSHIFT_OR_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), block + count - 1U, block + count - 1U, scale, handle + 1U);
		emit_constant(c, handle + 1U, 0U);
		free_reg(c, scale);
	}

	/* A compared sample writes its one result; the others write four. */
	write_mask = VA_WRITE_MASK_RGBA;
	write_count = 3U;
	if (texture.compare != 0U && texture.kind == VA_TEXTURE_SAMPLE) {
		write_mask = 1U;
		write_count = 0U;
	}

	/* Chooses the instruction and its own fields. */
	if (texture.kind == VA_TEXTURE_FETCH) {
		bits = VA_OP_TEX_FETCH;
		if (lod != VA_OPERAND_NONE)
			bits |= va_field(1U, VA_SHIFT_TEX_OFFSET);
	} else if (texture.kind == VA_TEXTURE_GATHER) {
		bits = VA_OP_TEX_GATHER | va_field(texture.channel, VA_SHIFT_TEX_FETCH_COMPONENT);
		bits |= va_field(texture.compare, VA_SHIFT_TEX_SHADOW);
	} else {
		bits = VA_OP_TEX_SINGLE | va_field(texture.lod_mode, VA_SHIFT_TEX_LOD_MODE);
		bits |= va_field(texture.compare, VA_SHIFT_TEX_SHADOW);
	}

	/* Samples into a block of four results. */
	result = alloc_block(c, 4U);
	if (c->error != 0)
		return;
	bits |= va_source_register(handle);
	bits |= va_field(texture.array, VA_SHIFT_TEX_ARRAY);
	bits |= va_field(result, VA_SHIFT_STAGING_DEST);
	bits |= va_field(write_mask, VA_SHIFT_TEX_WRITE_MASK);
	bits |= va_field(VA_REGISTER_TYPE_F, VA_SHIFT_TEX_REGISTER_TYPE);
	bits |= va_field(texture.dimension, VA_SHIFT_TEX_DIMENSION);
	bits |= va_field(1U, VA_SHIFT_TEX_REGISTER_WIDTH);
	bits |= va_field(write_count, VA_SHIFT_STAGING_WRITE_COUNT);
	bits |= va_field(count, VA_SHIFT_STAGING_COUNT);
	bits |= va_field(block, VA_SHIFT_DEST);
	emit(c,
	     bits,
	     va_register_mask(handle, 2U) | va_register_mask(block, count),
	     va_register_mask(result, 4U),
	     VA_INST_MESSAGE);
	free_reg(c, handle);
	free_reg(c, handle + 1U);
	for (component = 0U; component < count; component++)
		free_reg(c, block + component);

	/* Gives the four results to the instruction's values. */
	for (component = 0U; component < 4U; component++)
		bind_value(c, inst->dst + component, result + component);
}

/*
 * Makes a cube's s and t from its direction (x, y, z) in block[0] and
 * block[1]: the major axis's magnitude (CUBEFACE1) and its face
 * (CUBEFACE2, the face number in bits 31:29), the face's coordinates
 * (CUBE_SSEL, CUBE_TSEL) scaled by half the magnitude's reciprocal and
 * offset by a half, clamped to [0, 1], and the face put in s's top
 * three bits (a float in [0, 1] has them 001, which the hardware
 * restores).
 *
 * Procedure referenced: Mesa 26.2.6
 * src/panfrost/compiler/pan_nir_lower_tex.c (build_cube_coord2_face and
 * build_cube_desc: the operand order of the selects, the scale and the
 * 29-bit mask) and bifrost_compile.c (nir_intrinsic_cubeface_pan on v9
 * and later: CUBEFACE1 and CUBEFACE2_V9).
 */
static void
emit_cube_coordinates(
	struct va_compiler *c,
	uint32_t block,
	uint32_t x,
	uint32_t y,
	uint32_t z)
{
	uint32_t magnitude;
	uint32_t face;
	uint32_t half;
	uint32_t mask;

	/* The major axis's magnitude and its face. */
	magnitude = alloc_block(c, 1U);
	face = alloc_block(c, 1U);
	if (c->error != 0)
		return;
	emit_alu(c, VA_OP_CUBEFACE1, magnitude, x, y, z);
	emit_alu(c, VA_OP_CUBEFACE2_V9, face, x, y, z);

	/* The face's coordinates. */
	emit_alu(c, VA_OP_CUBE_SSEL, block, z, x, face);
	emit_alu(c, VA_OP_CUBE_TSEL, block + 1U, y, z, face);

	/* Half the magnitude's reciprocal, and s, t = coordinate * it + 0.5 within [0, 1]. */
	emit_alu(c, VA_OP_FRCP_F32, magnitude, magnitude, VA_OPERAND_NONE, VA_OPERAND_NONE);
	half = temp_constant(c, VA_FLOAT_HALF);
	if (c->error != 0)
		return;
	emit_fmul(c, magnitude, magnitude, half);
	emit_alu(c, VA_OP_FMA_F32 | va_field(VA_CLAMP_0_1, VA_SHIFT_CLAMP), block, block, magnitude, half);
	emit_alu(c, VA_OP_FMA_F32 | va_field(VA_CLAMP_0_1, VA_SHIFT_CLAMP), block + 1U, block + 1U, magnitude, half);

	/* The face in s's top three bits. */
	mask = temp_constant(c, VA_CUBE_COORDINATE_MASK);
	if (c->error != 0)
		return;
	emit_alu(c, VA_OP_MUX_I32 | va_field(VA_MUX_BIT, VA_SHIFT_MUX), block, block, face, mask);
	free_reg(c, mask);
	free_reg(c, half);
	free_reg(c, magnitude);
	free_reg(c, face);
}

/*
 * Makes the gradient descriptor of a sample with derivatives: TEX_GRADIENT
 * of the texture's table entries (the high handle word zero) with each
 * coordinate's x and y derivatives in a block of staging registers, the
 * descriptor's two words in a pair of registers the caller frees.
 *
 * Procedure referenced: Mesa 26.2.6
 * src/panfrost/compiler/pan_nir_lower_tex.c (build_va_gradient_desc: the
 * derivatives as x, y pairs per coordinate, the flags, the handle's high
 * word; va_lower_tex: the descriptor's two staging registers after the
 * others and the grdesc level of detail mode) and bifrost_compile.c
 * (bi_emit_tex_valhall: TEX_GRADIENT's 32-bit unsigned results).
 */
static uint32_t
emit_gradient_descriptor(
	struct va_compiler *c,
	const struct va_texture *texture,
	uint32_t slot)
{
	uint32_t derivatives;
	uint32_t count;
	uint32_t handle;
	uint32_t descriptor;
	uint32_t component;
	uint32_t parameter;
	uint32_t operand;
	uint64_t bits;

	/* Copies the derivatives into a block: dx, dy of each coordinate in turn. */
	count = 2U * texture->coordinates;
	derivatives = alloc_block(c, count);
	if (c->error != 0)
		return VA_NO_REG;
	for (component = 0U; component < count; component++) {
		parameter = texture->first + (component / 2U) * texture->stride + 1U + component % 2U;
		operand = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO;
		if (parameter < texture->parameter_count)
			operand = texture->parameters[parameter];
		emit_move(c, derivatives + component, operand);
	}

	/* Writes the handle pair, with no texel offset or level in its high word. */
	handle = alloc_block(c, 2U);
	if (c->error != 0)
		return VA_NO_REG;
	emit_constant(c, handle, texture_handle(slot));
	emit_constant(c, handle + 1U, 0U);

	/* Makes the descriptor's two words. */
	descriptor = alloc_block(c, 2U);
	if (c->error != 0)
		return VA_NO_REG;
	bits = VA_OP_TEX_GRADIENT | va_source_register(handle);
	bits |= va_field(1U, VA_SHIFT_TEX_LOD_BIAS_DISABLE);
	bits |= va_field(1U, VA_SHIFT_TEX_LOD_CLAMP_DISABLE);
	bits |= va_field(1U, VA_SHIFT_TEX_DERIVATIVE_ENABLE);
	bits |= va_field(descriptor, VA_SHIFT_STAGING_DEST);
	bits |= va_field(VA_WRITE_MASK_RG, VA_SHIFT_TEX_WRITE_MASK);
	bits |= va_field(VA_REGISTER_TYPE_U, VA_SHIFT_TEX_REGISTER_TYPE);
	bits |= va_field(texture->dimension, VA_SHIFT_TEX_DIMENSION);
	bits |= va_field(1U, VA_SHIFT_TEX_REGISTER_WIDTH);
	bits |= va_field(1U, VA_SHIFT_STAGING_WRITE_COUNT);
	bits |= va_field(count, VA_SHIFT_STAGING_COUNT);
	bits |= va_field(derivatives, VA_SHIFT_DEST);
	emit(c,
	     bits,
	     va_register_mask(handle, 2U) | va_register_mask(derivatives, count),
	     va_register_mask(descriptor, 2U),
	     VA_INST_MESSAGE);
	free_reg(c, handle);
	free_reg(c, handle + 1U);
	for (component = 0U; component < count; component++)
		free_reg(c, derivatives + component);

	/* Reports the descriptor's pair. */
	return descriptor;
}

/*
 * Computes the level of detail of a cube sample with derivatives, which
 * the sample then gives explicitly: the direction and its derivatives
 * reordered so the major axis is the third (emit_cube_major()), the
 * derivatives of the face's coordinates by the quotient rule, and the
 * level -1 + log2(L * L * max(|dx|^2, |dy|^2)) / 2 for the cube's size L,
 * kept within [-16, 16] (the 8.8 fixed point level's range), in a
 * register the caller frees.
 *
 * Procedure referenced: Mesa 26.2.6 src/compiler/nir/nir_lower_tex.c
 * (lower_gradient_cube_map: the selection of the major axis, the
 * quotient rule and the level, which pan_compiler.c asks for with
 * lower_txd_cube_map) and src/panfrost/compiler/pan_nir_lower_tex.c
 * (build_sfixed_8_8: the range of a level).
 */
static uint32_t
emit_cube_gradient_level(
	struct va_compiler *c,
	const struct va_texture *texture,
	uint32_t slot)
{
	uint32_t axes[3];
	uint32_t magnitude[3];
	uint32_t larger;
	uint32_t z_major;
	uint32_t not_x_major;
	uint32_t direction;
	uint32_t dx;
	uint32_t dy;
	uint32_t size;
	uint32_t half;
	uint32_t axis;
	uint32_t set;
	uint32_t word;
	uint32_t parameter;

	/* The magnitudes of the direction's axes. */
	for (axis = 0U; axis < 3U; axis++) {
		magnitude[axis] = alloc_block(c, 1U);
		if (c->error != 0)
			return VA_NO_REG;
		emit_alu(c,
			 VA_OP_FADD_F32 | va_field(1U, VA_SHIFT_ABS_SOURCE0) | va_field(1U, VA_SHIFT_NEG_SOURCE1),
			 magnitude[axis],
			 texture->parameters[texture->first + axis * texture->stride],
			 VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO,
			 VA_OPERAND_NONE);
	}

	/* Masks of a major z, and of a major y or z (z winning a tie over y, y over x). */
	larger = alloc_block(c, 1U);
	z_major = alloc_block(c, 1U);
	not_x_major = alloc_block(c, 1U);
	if (c->error != 0)
		return VA_NO_REG;
	emit_alu(c, VA_OP_FMAX_F32, larger, magnitude[0], magnitude[1], VA_OPERAND_NONE);
	emit_alu(c,
		 VA_OP_FCMP_OR_F32 | va_field(VA_CONDITION_GE, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE),
		 z_major,
		 magnitude[2],
		 larger,
		 VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO);
	emit_alu(c, VA_OP_FMAX_F32, larger, magnitude[0], magnitude[2], VA_OPERAND_NONE);
	emit_alu(c,
		 VA_OP_FCMP_OR_F32 | va_field(VA_CONDITION_GE, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE),
		 not_x_major,
		 magnitude[1],
		 larger,
		 z_major);
	free_reg(c, larger);
	for (axis = 0U; axis < 3U; axis++)
		free_reg(c, magnitude[axis]);

	/* The direction, its x derivatives and its y derivatives with the major axis third. */
	direction = alloc_block(c, 3U);
	dx = alloc_block(c, 3U);
	dy = alloc_block(c, 3U);
	if (c->error != 0)
		return VA_NO_REG;
	for (set = 0U; set < 3U; set++) {
		for (axis = 0U; axis < 3U; axis++) {
			parameter = texture->first + axis * texture->stride + set;
			axes[axis] = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO;
			if (parameter < texture->parameter_count)
				axes[axis] = texture->parameters[parameter];
		}
		if (set == 0U) {
			emit_cube_major(c, direction, axes, z_major, not_x_major);
		} else if (set == 1U) {
			emit_cube_major(c, dx, axes, z_major, not_x_major);
		} else {
			emit_cube_major(c, dy, axes, z_major, not_x_major);
		}
	}
	free_reg(c, z_major);
	free_reg(c, not_x_major);

	/* The face's coordinates over the major axis: q = Q.xy / Q.z. */
	emit_alu(c, VA_OP_FRCP_F32, direction + 2U, direction + 2U, VA_OPERAND_NONE, VA_OPERAND_NONE);
	emit_fmul(c, direction, direction, direction + 2U);
	emit_fmul(c, direction + 1U, direction + 1U, direction + 2U);

	/*
	 * Each derivative of q by the quotient rule, (dQ.xy - q dQ.z) / Q.z,
	 * and its squared length in its third register.
	 */
	for (set = 0U; set < 2U; set++) {
		word = dx;
		if (set == 1U)
			word = dy;
		for (axis = 0U; axis < 2U; axis++) {
			emit_alu(c, VA_OP_FMA_F32 | va_field(1U, VA_SHIFT_NEG_SOURCE0), word + axis, direction + axis, word + 2U, word + axis);
			emit_fmul(c, word + axis, word + axis, direction + 2U);
		}
		emit_fmul(c, word + 2U, word, word);
		emit_fma(c, word + 2U, word + 1U, word + 1U, word + 2U);
	}

	/* L * L times the larger squared length. */
	size = alloc_block(c, 1U);
	if (c->error != 0)
		return VA_NO_REG;
	emit_alu(c, VA_OP_FMAX_F32, dx + 2U, dx + 2U, dy + 2U, VA_OPERAND_NONE);
	emit_move_uniform(c, size, c->program->textures[slot].size_fau_word);
	emit_alu(c, VA_OP_U32_TO_F32 | va_field(VA_ROUND_RTE, VA_SHIFT_ROUND_MODE), size, size, VA_OPERAND_NONE, VA_OPERAND_NONE);
	emit_fmul(c, size, size, size);
	emit_fmul(c, size, size, dx + 2U);
	for (axis = 0U; axis < 3U; axis++) {
		free_reg(c, direction + axis);
		free_reg(c, dx + axis);
		free_reg(c, dy + axis);
	}

	/* The level: half the logarithm less one, kept within 16 levels either way. */
	emit_log2(c, size, size);
	half = temp_constant(c, VA_FLOAT_HALF);
	if (c->error != 0)
		return VA_NO_REG;
	emit_alu(c, VA_OP_FMA_F32 | va_field(1U, VA_SHIFT_NEG_SOURCE2), size, size, half, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_FLOAT_ONE);
	emit_constant(c, half, VA_FLOAT_SIXTEENTH);
	emit_alu(c,
		 VA_OP_FMA_F32 | va_field(1U, VA_SHIFT_NEG_SOURCE2) | va_field(VA_CLAMP_M1_1, VA_SHIFT_CLAMP),
		 size,
		 size,
		 half,
		 VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO);
	emit_constant(c, half, VA_FLOAT_SIXTEEN);
	emit_fmul(c, size, size, half);
	free_reg(c, half);

	/* Reports the level's register. */
	return size;
}

/*
 * Reorders three values of a cube's direction (x, y, z) so the major
 * axis is the third: (x, y, z) for a major z, (x, z, y) for a major y,
 * (y, z, x) for a major x, into a block of three.
 *
 * Procedure referenced: Mesa 26.2.6 src/compiler/nir/nir_lower_tex.c
 * (lower_gradient_cube_map: the swizzles of the selection).
 */
static void
emit_cube_major(
	struct va_compiler *c,
	uint32_t dst,
	const uint32_t *axes,
	uint32_t z_major,
	uint32_t not_x_major)
{
	/* The first: x unless x is the major axis, then y. */
	emit_alu(c, VA_OP_MUX_I32 | va_field(VA_MUX_INT_ZERO, VA_SHIFT_MUX), dst, axes[1], axes[0], not_x_major);

	/* The second: y for a major z, else z. */
	emit_alu(c, VA_OP_MUX_I32 | va_field(VA_MUX_INT_ZERO, VA_SHIFT_MUX), dst + 1U, axes[2], axes[1], z_major);

	/* The third: the major axis itself. */
	emit_alu(c, VA_OP_MUX_I32 | va_field(VA_MUX_INT_ZERO, VA_SHIFT_MUX), dst + 2U, axes[0], axes[1], not_x_major);
	emit_alu(c, VA_OP_MUX_I32 | va_field(VA_MUX_INT_ZERO, VA_SHIFT_MUX), dst + 2U, dst + 2U, axes[2], z_major);
}

/*
 * Lowers a storage image's texel read or write.  The texel's x, y and
 * layer go as 16-bit halves in two registers: x | y << 16 and
 * layer << 16 (image_coordinates()).  A read is
 * LD_TEX_IMM of the image's table entry into four registers, converted
 * to the format's kind (floats, signed or unsigned integers; the
 * components the format lacks read 0, 0, 0, 1); a write finds the
 * texel's address and conversion with LEA_TEX_IMM and stores the
 * format's components with ST_CVT.  A predicated access is branched
 * around for the lanes whose predicate is false.
 *
 * Procedure referenced: Mesa 26.2.6
 * src/panfrost/compiler/pan_nir_lower_image.c (pack_image_coords: the
 * 16:16 coordinates with the array index in the top half of the second
 * register on v9 and later; lower_image_store: LEA_TEX and the store with
 * conversion, the data trimmed to the format's components, the automatic
 * register format) and bifrost_compile.c (bi_emit_load_tex_to,
 * bi_emit_lea_tex_to: the immediate forms).
 */
static void
lower_image(
	struct va_compiler *c,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t values[4];
	uint32_t slot;
	uint32_t coordinates;
	uint32_t predicate;
	uint32_t branch;
	uint32_t block;
	uint32_t address;
	uint32_t data;
	uint32_t components;
	uint32_t component;
	uint64_t bits;

	/* Finds the image and packs the texel's coordinates. */
	slot = image_slot(c, inst->location);
	if (c->error != 0)
		return;
	coordinates = image_coordinates(c, inst);
	if (c->error != 0)
		return;

	/* The predicate of a predicated access, read before its branch. */
	predicate = VA_NO_REG;
	if (inst->component == 1U && inst->op == DRV_GPU_IR_IMAGE_READ)
		predicate = value_reg(c, inst->src[2]);
	if (inst->component == 1U && inst->op == DRV_GPU_IR_IMAGE_WRITE)
		predicate = value_reg(c, inst->src[3]);

	/* A read loads four components into its results. */
	if (inst->op == DRV_GPU_IR_IMAGE_READ) {
		block = alloc_block(c, 4U);
		if (c->error != 0)
			return;
		branch = 0U;
		if (predicate != VA_NO_REG)
			branch_begin(c, predicate, &branch);
		bits = VA_OP_LD_TEX_IMM | va_source_register(coordinates);
		bits |= va_field(va_source_register(coordinates + 1U), VA_SHIFT_SOURCE1);
		bits |= va_field(MALI_SHADER_TABLE_IMAGES, VA_SHIFT_TEX_IMM_TABLE);
		bits |= va_field(c->program->images[slot].index, VA_SHIFT_TEX_IMM_INDEX);
		bits |= va_field(image_register_format(c->program->images[slot].format), VA_SHIFT_REGISTER_FORMAT);
		bits |= va_field(3U, VA_SHIFT_VECTOR_SIZE);
		bits |= va_staging(block, 4U, VA_STAGING_WRITE);
		emit(c, bits, va_register_mask(coordinates, 2U), va_register_mask(block, 4U), VA_INST_MESSAGE | VA_INST_MEMORY);
		if (predicate != VA_NO_REG)
			branch_end(c, branch);
		free_reg(c, coordinates);
		free_reg(c, coordinates + 1U);

		/* Gives the four components to the instruction's values. */
		for (component = 0U; component < 4U; component++)
			bind_value(c, inst->dst + component, block + component);
		return;
	}

	/* A write's components: those of the format, from its values. */
	components = image_components(c->program->images[slot].format);
	for (component = 0U; component < components; component++)
		values[component] = value_operand(c, inst->src[2] + component);
	data = gather_block(c, values, components, components);
	if (c->error != 0)
		return;

	/* The texel's address and conversion. */
	address = image_address(c, slot, coordinates);
	if (c->error != 0)
		return;

	/* Stores the components, converted, for the lanes whose predicate holds. */
	branch = 0U;
	if (predicate != VA_NO_REG)
		branch_begin(c, predicate, &branch);
	bits = VA_OP_ST_CVT | va_source_register(address);
	bits |= va_field(va_source_register(address + 2U), VA_SHIFT_SOURCE2);
	bits |= va_field(VA_REGISTER_FORMAT_AUTO, VA_SHIFT_REGISTER_FORMAT);
	bits |= va_field(components - 1U, VA_SHIFT_VECTOR_SIZE);
	bits |= va_staging(data, components, VA_STAGING_READ);
	emit(c,
	     bits,
	     va_register_mask(address, 3U) | va_register_mask(data, components),
	     0U,
	     VA_INST_MESSAGE | VA_INST_MEMORY);
	if (predicate != VA_NO_REG)
		branch_end(c, branch);

	/* Gives back the address and the data. */
	for (component = 0U; component < 3U; component++)
		free_reg(c, address + component);
	for (component = 0U; component < components; component++)
		free_reg(c, data + component);
}

/*
 * Packs a texel's coordinates (the three consecutive integers from the
 * instruction's src[0]) into a fresh register pair: x | y << 16, and the
 * third (an array's layer, a cube's face, a 3D image's slice) << 16.  A
 * storage image is a 2D image of layers to the program (a 3D one of its
 * slices, as the shader ABI says), so an atomic's texel is found the same
 * way whatever the dimension its IR names (always with its layer).
 */
static uint32_t
image_coordinates(
	struct va_compiler *c,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t kind;
	uint32_t pair;
	uint32_t x;
	uint32_t y;
	uint32_t third;
	uint32_t zero;
	uint32_t layered;

	/* A multisampled or buffer image has no texel of this form. */
	kind = inst->image_shape & DRV_GPU_IR_IMAGE_KIND_MASK;
	if ((inst->image_shape & DRV_GPU_IR_IMAGE_MULTISAMPLED) != 0U || kind == DRV_GPU_IR_IMAGE_BUFFER) {
		fail(c, ENOTSUP, "a multisampled or buffer storage image");
		return 0U;
	}

	/* An array, a cube, a 3D image and an atomic's texel have a layer. */
	layered = 0U;
	if ((inst->image_shape & (DRV_GPU_IR_IMAGE_ARRAYED | DRV_GPU_IR_IMAGE_TEXEL)) != 0U)
		layered = 1U;
	if (kind == DRV_GPU_IR_IMAGE_CUBE || kind == DRV_GPU_IR_IMAGE_3D)
		layered = 1U;

	/* Reads the three coordinates. */
	x = value_reg(c, inst->src[0]);
	y = value_reg(c, inst->src[0] + 1U);
	third = value_reg(c, inst->src[0] + 2U);
	pair = alloc_block(c, 2U);
	if (c->error != 0)
		return 0U;
	zero = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO;

	/* x and y; then the layer, which a 2D image of one layer does not have. */
	emit_alu(c, VA_OP_MKVEC_V2I16, pair, x, y, VA_OPERAND_NONE);
	if (layered != 0U) {
		emit_alu(c, VA_OP_MKVEC_V2I16, pair + 1U, zero, third, VA_OPERAND_NONE);
	} else {
		emit_move(c, pair + 1U, zero);
	}

	/* Reports the pair. */
	return pair;
}

/*
 * Finds a texel's address and its conversion descriptor with LEA_TEX_IMM
 * into a fresh aligned block of three registers (the address's low and
 * high words, then the conversion), and gives back the coordinates.
 */
static uint32_t
image_address(
	struct va_compiler *c,
	uint32_t slot,
	uint32_t coordinates)
{
	uint32_t block;
	uint64_t bits;

	/* Allocates the three registers. */
	block = alloc_block(c, 3U);
	if (c->error != 0)
		return 0U;

	/* Asks for the texel's address. */
	bits = VA_OP_LEA_TEX_IMM | va_source_register(coordinates);
	bits |= va_field(va_source_register(coordinates + 1U), VA_SHIFT_SOURCE1);
	bits |= va_field(MALI_SHADER_TABLE_IMAGES, VA_SHIFT_TEX_IMM_TABLE);
	bits |= va_field(c->program->images[slot].index, VA_SHIFT_TEX_IMM_INDEX);
	bits |= va_staging(block, 3U, VA_STAGING_WRITE);
	emit(c, bits, va_register_mask(coordinates, 2U), va_register_mask(block, 3U), VA_INST_MESSAGE);
	free_reg(c, coordinates);
	free_reg(c, coordinates + 1U);

	/* Reports the block. */
	return block;
}

/* Finds the image slot of an IR uniform. */
static uint32_t
image_slot(
	struct va_compiler *c,
	uint32_t uniform)
{
	const struct drv_gpu_shader_ir_uniform *record;
	uint32_t index;

	/* A uniform the IR does not have is refused. */
	if (uniform >= c->ir->uniform_count) {
		fail(c, EINVAL, "an image without its uniform");
		return 0U;
	}
	record = &c->ir->uniforms[uniform];

	/* Only a storage image has a slot. */
	if (record->kind != DRV_GPU_IR_UNIFORM_STORAGE_IMAGE) {
		fail(c, ENOTSUP, "a storage texel buffer or an input attachment");
		return 0U;
	}

	/* Looks through the program's images. */
	for (index = 0U; index < c->program->image_count; index++) {
		if (c->program->images[index].set == record->set && c->program->images[index].binding == record->binding)
			return index;
	}

	/* An image the layout did not list is refused. */
	fail(c, EINVAL, "an image without its slot");
	return 0U;
}

/*
 * Reports the register format a storage image's texels load as: signed
 * or unsigned integers for an integer format, floats otherwise.
 *
 * Values referenced: the Vulkan specification, VkFormat (R8G8B8A8_UINT
 * 41, R8G8B8A8_SINT 42, R16G16B16A16_UINT 95, R16G16B16A16_SINT 96,
 * R32_UINT 98, R32_SINT 99, R32G32_UINT 101, R32G32_SINT 102,
 * R32G32B32A32_UINT 107, R32G32B32A32_SINT 108).
 */
static uint32_t
image_register_format(
	uint32_t format)
{
	/* One case a format of integers. */
	switch (format) {
	case 41U:
	case 95U:
	case 98U:
	case 101U:
	case 107U:
		return VA_REGISTER_FORMAT_U32;

	case 42U:
	case 96U:
	case 99U:
	case 102U:
	case 108U:
		return VA_REGISTER_FORMAT_S32;

	default:
		return VA_REGISTER_FORMAT_F32;
	}
}

/*
 * Reports the components of a storage image's format.
 *
 * Values referenced: the Vulkan specification, VkFormat (R32_UINT 98,
 * R32_SINT 99, R32_SFLOAT 100, R32G32_UINT 101, R32G32_SINT 102,
 * R32G32_SFLOAT 103).
 */
static uint32_t
image_components(
	uint32_t format)
{
	/* The one- and two-component formats; the others have four. */
	switch (format) {
	case 98U:
	case 99U:
	case 100U:
		return 1U;

	case 101U:
	case 102U:
	case 103U:
		return 2U;

	default:
		return 4U;
	}
}

/*
 * Reads a size the driver writes in the FAU: a storage buffer's bytes, or
 * a texture's size at a level (the width, height and, for a 3D image,
 * depth of level 0 shifted right by the level, at least 1; an array's
 * layers; the level count).
 */
static void
lower_size_query(
	struct va_compiler *c,
	const struct drv_gpu_shader_ir_inst *inst)
{
	uint32_t slot;
	uint32_t word;
	uint32_t component;
	uint32_t level;
	uint32_t mask;
	uint32_t reg;

	/* A buffer's bytes are one word. */
	if (inst->op == DRV_GPU_IR_STORAGE_SIZE) {
		slot = buffer_slot(c, inst->location);
		if (c->error != 0)
			return;
		word = c->program->buffers[slot].size_fau_word;
		reg = define_value(c, inst->dst);
		if (c->error != 0)
			return;
		emit_move_uniform(c, reg, word);
		return;
	}

	/* A storage image's four words, or a texture's. */
	word = MALI_SHADER_NO_FAU_WORD;
	for (slot = 0U; slot < c->program->image_count; slot++) {
		if (c->program->images[slot].set == inst->location && c->program->images[slot].binding == inst->immediate)
			word = c->program->images[slot].size_fau_word;
	}
	if (word == MALI_SHADER_NO_FAU_WORD) {
		slot = texture_slot(c, inst->location, inst->immediate);
		if (c->error != 0)
			return;
		word = c->program->textures[slot].size_fau_word;
	}

	/* The first three are shifted by the level. */
	level = value_reg(c, inst->src[0]);
	mask = alloc_block(c, 1U);
	if (c->error != 0)
		return;
	for (component = 0U; component < 4U; component++) {
		reg = define_value(c, inst->dst + component);
		if (c->error != 0)
			return;
		emit_move_uniform(c, reg, word + component);
		/* The level count, and an array's layers, are not shifted. */
		if (component == 3U)
			continue;
		if (component == 2U && (inst->image_shape & DRV_GPU_IR_IMAGE_KIND_MASK) != DRV_GPU_IR_IMAGE_3D)
			continue;

		/* max(size >> level, 1). */
		emit_alu(c, VA_OP_RSHIFT_OR_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES), reg, reg, level, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO);
		emit_alu(c, VA_OP_ICMP_OR_U32 | va_field(VA_CONDITION_EQ, VA_SHIFT_CONDITION) | va_field(VA_RESULT_M1, VA_SHIFT_RESULT_TYPE), mask, reg, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO, VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO);
		emit_alu(c, VA_OP_ISUB_U32, reg, reg, mask, VA_OPERAND_NONE);
	}
	free_reg(c, mask);
}

/* Records a STORE_OUTPUT, which the program's end writes. */
static void
lower_store_output(
	struct va_compiler *c,
	const struct drv_gpu_shader_ir_inst *inst)
{
	/*
	 * The position, the point size, the depth, the sample mask, the
	 * stencil reference and the located outputs are kept by slot.
	 */
	if (inst->location == DRV_GPU_IR_LOCATION_POSITION) {
		if (inst->component < 4U)
			c->position_value[inst->component] = inst->src[0];
		return;
	}
	if (inst->location == DRV_GPU_IR_LOCATION_POINT_SIZE) {
		c->point_size_value = inst->src[0];
		return;
	}
	if (inst->location == DRV_GPU_IR_LOCATION_FRAG_DEPTH) {
		c->depth_value = inst->src[0];
		return;
	}
	if (inst->location == DRV_GPU_IR_LOCATION_SAMPLE_MASK) {
		c->sample_mask_value = inst->src[0];
		return;
	}
	if (inst->location == DRV_GPU_IR_LOCATION_FRAG_STENCIL) {
		c->stencil_value = inst->src[0];
		return;
	}
	if (inst->location < MALI_SHADER_LOCATIONS && inst->component < 4U) {
		c->output_value[inst->location][inst->component] = inst->src[0];
		return;
	}

	/* Another builtin output is not one this program writes. */
	fail(c, ENOTSUP, "a builtin output the Valhall lowering does not have");
}

/* Finds the texture slot of a (set, binding). */
static uint32_t
texture_slot(
	struct va_compiler *c,
	uint32_t set,
	uint32_t binding)
{
	uint32_t index;

	/* Looks through the program's textures. */
	for (index = 0U; index < c->program->texture_count; index++) {
		if (c->program->textures[index].set == set && c->program->textures[index].binding == binding)
			return index;
	}

	/* A texture the IR does not list is refused. */
	fail(c, EINVAL, "a texture without its uniform");
	return 0U;
}

/* Finds the buffer slot of an IR uniform. */
static uint32_t
buffer_slot(
	struct va_compiler *c,
	uint32_t uniform)
{
	const struct drv_gpu_shader_ir_uniform *record;
	uint32_t index;

	/* A uniform the IR does not have is refused. */
	if (uniform >= c->ir->uniform_count) {
		fail(c, EINVAL, "a buffer without its uniform");
		return 0U;
	}
	record = &c->ir->uniforms[uniform];

	/* Looks through the program's buffers. */
	for (index = 0U; index < c->program->buffer_count; index++) {
		if (c->program->buffers[index].set == record->set && c->program->buffers[index].binding == record->binding)
			return index;
	}

	/* A uniform that is not a buffer is refused. */
	fail(c, EINVAL, "a buffer without its slot");
	return 0U;
}

/* The texture and sampler handle's table and index bits for texture slot k. */
static uint32_t
texture_handle(
	uint32_t slot)
{
	uint32_t handle;

	/* The texture in its table at the slot, and the sampler in its own. */
	handle = MALI_SHADER_TABLE_TEXTURES << VA_TEX_TEXTURE_TABLE_SHIFT;
	handle |= slot << VA_TEX_TEXTURE_INDEX_SHIFT;
	handle |= MALI_SHADER_TABLE_SAMPLERS << VA_TEX_SAMPLER_TABLE_SHIFT;
	handle |= slot;

	/* Reports the handle. */
	return handle;
}

/*
 * Copies count operands into a fresh aligned block of size registers
 * (the rest left for the caller), a missing one as zero.
 */
static uint32_t
gather_block(
	struct va_compiler *c,
	const uint32_t *values,
	uint32_t count,
	uint32_t size)
{
	uint32_t block;
	uint32_t index;

	/* Allocates the block. */
	block = alloc_block(c, size);
	if (c->error != 0)
		return 0U;

	/* Copies each operand into its place. */
	for (index = 0U; index < count; index++)
		emit_move(c, block + index, values[index]);

	/* Reports the block. */
	return block;
}

/*
 * Computes the address of this vertex's output record: LEA_BUF_IMM of the
 * vertex output buffer (table 61, entry 0) at the preloaded internal index
 * plus index_offset.
 *
 * Procedure referenced: Mesa 26.2.6
 * src/panfrost/compiler/pan_nir_lower_vs_outputs.c (build_attr_buf_write).
 */
static uint32_t
output_address(
	struct va_compiler *c,
	uint32_t index_offset)
{
	uint32_t pair;
	uint32_t index;
	uint64_t bits;

	/* Offsets the internal index when asked. */
	index = VA_PRELOAD_OUTPUT_INDEX;
	if (index_offset != 0U) {
		index = alloc_block(c, 1U);
		if (c->error != 0)
			return 0U;
		emit(c,
		     VA_OP_IADD_IMM_I32 | va_source_register(VA_PRELOAD_OUTPUT_INDEX) |
		     va_field(index_offset, VA_SHIFT_SOURCE1) | va_dest(index),
		     va_register_mask(VA_PRELOAD_OUTPUT_INDEX, 1U),
		     va_register_mask(index, 1U),
		     0U);
	}
	c->preload |= 1U << (VA_PRELOAD_OUTPUT_INDEX - VA_PRELOAD_FIRST);

	/* Loads the record's address into a pair. */
	pair = alloc_block(c, 2U);
	if (c->error != 0)
		return 0U;
	bits = VA_OP_LEA_BUF_IMM | va_source_register(index);
	bits |= va_field(VA_TABLE_61_FOLDED, VA_SHIFT_BUF_TABLE);
	bits |= va_field(0U, VA_SHIFT_BUF_INDEX);
	bits |= va_staging(pair, 2U, VA_STAGING_WRITE);
	emit(c, bits, va_register_mask(index, 1U), va_register_mask(pair, 2U), VA_INST_MESSAGE);
	if (index != VA_PRELOAD_OUTPUT_INDEX)
		free_reg(c, index);

	/* Reports the pair. */
	return pair;
}

/*
 * Ends the position program: the viewport transform of the position (x,
 * y, z divided by w, scaled and offset; w replaced by its clamped
 * reciprocal), stored as four floats at the vertex's position record,
 * and the point size, at least 1, as a 16-bit float at its attribute
 * record.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/compiler/nir/nir_lower_viewport_transform.c and Mesa 26.2.6
 * src/panfrost/compiler/pan_nir_lower_vs_outputs.c.
 */
static void
lower_position_end(
	struct va_compiler *c)
{
	uint32_t source[4];
	uint32_t block;
	uint32_t recip;
	uint32_t factor;
	uint32_t pair;
	uint32_t axis;
	uint32_t size;
	uint64_t bits;

	/* Reads the position, a missing component as 0 (w as 1). */
	for (axis = 0U; axis < 4U; axis++) {
		if (c->position_value[axis] != VA_NOT_DEFINED) {
			source[axis] = value_operand(c, c->position_value[axis]);
		} else if (axis == 3U) {
			source[axis] = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_FLOAT_ONE;
		} else {
			source[axis] = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO;
		}
	}
	block = alloc_block(c, 4U);
	recip = alloc_block(c, 1U);
	factor = alloc_block(c, 1U);
	if (c->error != 0)
		return;

	/* 1 / w, kept within +-2^15. */
	emit_alu(c, VA_OP_FRCP_F32, recip, source[3], VA_OPERAND_NONE, VA_OPERAND_NONE);
	emit_constant(c, factor, VA_FLOAT_W_RECIP_MAX);
	emit_alu(c, VA_OP_FMIN_F32, recip, recip, factor, VA_OPERAND_NONE);
	emit_constant(c, factor, VA_FLOAT_W_RECIP_MIN);
	emit_alu(c, VA_OP_FMAX_F32, block + 3U, recip, factor, VA_OPERAND_NONE);

	/* Each of x, y, z: (c / w) * scale + offset. */
	for (axis = 0U; axis < 3U; axis++) {
		emit_fmul(c, block + axis, source[axis], block + 3U);
		emit_move_uniform(c, factor, c->program->viewport_fau_word + axis);
		emit_move_uniform(c, recip, c->program->viewport_fau_word + 3U + axis);
		emit_fma(c, block + axis, block + axis, factor, recip);
	}
	free_reg(c, recip);
	free_reg(c, factor);

	/* Stores the position at the record. */
	pair = output_address(c, 0U);
	if (c->error != 0)
		return;
	bits = VA_OP_STORE_I128 | va_source_register(pair);
	bits |= va_field(VA_MEMORY_ISTREAM, VA_SHIFT_MEMORY_ACCESS);
	bits |= va_staging(block, 4U, VA_STAGING_READ);
	emit(c, bits, va_register_mask(pair, 2U) | va_register_mask(block, 4U), 0U, VA_INST_MESSAGE | VA_INST_MEMORY);
	free_reg(c, pair);
	free_reg(c, pair + 1U);
	for (axis = 0U; axis < 4U; axis++)
		free_reg(c, block + axis);

	/* Stores the point size, when written, at the attribute record. */
	if (c->point_size_value == VA_NOT_DEFINED)
		return;
	c->program->writes_point_size = 1U;
	size = alloc_block(c, 1U);
	if (c->error != 0)
		return;
	emit_alu(c, VA_OP_FMAX_F32, size, value_reg(c, c->point_size_value), VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_FLOAT_ONE, VA_OPERAND_NONE);
	emit_alu(c, VA_OP_V2F32_TO_V2F16 | va_field(VA_ROUND_RTE, VA_SHIFT_ROUND_MODE), size, size, size, VA_OPERAND_NONE);
	pair = output_address(c, VA_ATTRIBUTE_RECORD);
	if (c->error != 0)
		return;
	bits = VA_OP_STORE_I16 | va_source_register(pair);
	bits |= va_field(VA_MEMORY_ISTREAM, VA_SHIFT_MEMORY_ACCESS);
	bits |= va_staging(size, 1U, VA_STAGING_READ);
	emit(c, bits, va_register_mask(pair, 2U) | va_register_mask(size, 1U), 0U, VA_INST_MESSAGE | VA_INST_MEMORY);
	free_reg(c, pair);
	free_reg(c, pair + 1U);
	free_reg(c, size);
}

/*
 * Ends the varying program: each generic varying location written, as
 * four words (a missing component as 0) at byte 16 L of the vertex's
 * varying record; a NoPerspective one multiplied by w first.
 */
static void
lower_varying_end(
	struct va_compiler *c)
{
	uint32_t source[4];
	uint32_t location;
	uint32_t component;
	uint32_t block;
	uint32_t pair;
	uint32_t w;
	uint32_t noperspective;
	uint64_t bits;

	/* Loads the record's address once. */
	pair = output_address(c, 0U);
	if (c->error != 0)
		return;

	/* Stores each location written. */
	for (location = 0U; location < MALI_SHADER_LOCATIONS; location++) {
		if ((c->program->varying_mask & (1U << location)) == 0U)
			continue;
		for (component = 0U; component < 4U; component++) {
			if (c->output_value[location][component] != VA_NOT_DEFINED) {
				source[component] = value_operand(c, c->output_value[location][component]);
			} else {
				source[component] = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO;
			}
		}
		block = gather_block(c, source, 4U, 4U);
		release_temps(c);
		if (c->error != 0)
			return;

		/* A NoPerspective varying is multiplied by w, which the fragment program divides out. */
		noperspective = (uint32_t)noperspective_output(c->ir, location);
		if (noperspective != 0U) {
			if (c->position_value[3] != VA_NOT_DEFINED) {
				w = value_reg(c, c->position_value[3]);
				for (component = 0U; component < 4U; component++)
					emit_fmul(c, block + component, block + component, w);
			}
		}

		/* Stores the four words. */
		bits = VA_OP_STORE_I128 | va_source_register(pair);
		bits |= va_field(VA_MEMORY_ESTREAM, VA_SHIFT_MEMORY_ACCESS);
		bits |= va_field(location * MALI_SHADER_VARYING_STRIDE, VA_SHIFT_MEMORY_OFFSET);
		bits |= va_staging(block, 4U, VA_STAGING_READ);
		emit(c, bits, va_register_mask(pair, 2U) | va_register_mask(block, 4U), 0U, VA_INST_MESSAGE | VA_INST_MEMORY);
		for (component = 0U; component < 4U; component++)
			free_reg(c, block + component);
	}
	free_reg(c, pair);
	free_reg(c, pair + 1U);
}

/*
 * Ends a fragment program: the written sample mask ANDed into the
 * preloaded coverage, ATEST of the coverage with colour 0's alpha (1
 * without colour 0), ZS_EMIT of the written depth and stencil
 * reference, then BLEND of each
 * colour written, as four 32-bit floats, to its render target with the
 * target's blend descriptor.  The coverage stays in r60, which ATEST and
 * ZS_EMIT read and write.
 *
 * Procedure referenced: Mesa 26.2.6
 * src/panfrost/compiler/pan_nir_lower_fs_outputs.c and bifrost_compile.c
 * (the atest_pan, zs_emit_pan and blend_pan intrinsics) and bi_ra.c (the
 * coverage of ATEST and ZS_EMIT in the cumulative coverage register).
 */
static void
lower_fragment_end(
	struct va_compiler *c)
{
	uint32_t source[4];
	uint32_t location;
	uint32_t component;
	uint32_t alpha;
	uint32_t mask;
	uint32_t depth;
	uint32_t stencil;
	uint32_t block;
	uint64_t bits;
	uint64_t reads;

	/* A written sample mask keeps only the samples the hardware covers. */
	c->preload |= 1U << (VA_PRELOAD_COVERAGE - VA_PRELOAD_FIRST);
	if (c->sample_mask_value != VA_NOT_DEFINED) {
		mask = value_operand(c, c->sample_mask_value);
		emit_alu(c,
			 VA_OP_LSHIFT_AND_I32 | va_field(VA_LANES_B00, VA_SHIFT_LANES),
			 VA_PRELOAD_COVERAGE,
			 mask,
			 VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO,
			 VA_PRELOAD_COVERAGE);
		release_temps(c);
		c->program->writes_sample_mask = 1U;
	}

	/* Tests the coverage with colour 0's alpha. */
	alpha = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_FLOAT_ONE;
	if (c->output_value[0][3] != VA_NOT_DEFINED)
		alpha = value_reg(c, c->output_value[0][3]);
	if (c->error != 0)
		return;
	bits = VA_OP_ATEST | va_source_register(VA_PRELOAD_COVERAGE);
	bits |= va_field(operand_byte(alpha), VA_SHIFT_SOURCE1);
	bits |= va_field(va_source_special(VA_SPECIAL_ATEST_DATUM, 0U), VA_SHIFT_SOURCE2);
	bits |= va_staging(VA_PRELOAD_COVERAGE, 1U, VA_STAGING_WRITE);
	emit(c,
	     bits,
	     va_register_mask(VA_PRELOAD_COVERAGE, 1U) | operand_mask(alpha),
	     va_register_mask(VA_PRELOAD_COVERAGE, 1U),
	     VA_INST_MESSAGE | VA_INST_ATEST);

	/*
	 * Emits the written depth and stencil reference with the tested
	 * coverage, each from a register (the one not written is the
	 * constant zero, and its bit is clear).
	 */
	if (c->depth_value != VA_NOT_DEFINED || c->stencil_value != VA_NOT_DEFINED) {
		depth = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO;
		stencil = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO;
		reads = va_register_mask(VA_PRELOAD_COVERAGE, 1U);
		bits = VA_OP_ZS_EMIT;
		if (c->depth_value != VA_NOT_DEFINED) {
			depth = value_reg(c, c->depth_value);
			reads |= va_register_mask(depth, 1U);
			bits |= va_field(1U, VA_SHIFT_ZS_EMIT_DEPTH);
			c->program->writes_depth = 1U;
		}
		if (c->stencil_value != VA_NOT_DEFINED) {
			stencil = value_reg(c, c->stencil_value);
			reads |= va_register_mask(stencil, 1U);
			bits |= va_field(1U, VA_SHIFT_ZS_EMIT_STENCIL);
			c->program->writes_stencil = 1U;
		}
		if (c->error != 0)
			return;
		bits |= operand_byte(depth);
		bits |= va_field(operand_byte(stencil), VA_SHIFT_SOURCE1);
		bits |= va_field(va_source_register(VA_PRELOAD_COVERAGE), VA_SHIFT_SOURCE2);
		bits |= va_staging(VA_PRELOAD_COVERAGE, 1U, VA_STAGING_WRITE);
		emit(c,
		     bits,
		     reads,
		     va_register_mask(VA_PRELOAD_COVERAGE, 1U),
		     VA_INST_MESSAGE | VA_INST_ATEST);
		release_temps(c);
	}

	/* Blends each colour written. */
	for (location = 0U; location < MALI_SHADER_COLORS; location++) {
		if ((c->program->color_mask & (1U << location)) == 0U)
			continue;
		for (component = 0U; component < 4U; component++) {
			if (c->output_value[location][component] != VA_NOT_DEFINED) {
				source[component] = value_operand(c, c->output_value[location][component]);
			} else if (component == 3U) {
				source[component] = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_FLOAT_ONE;
			} else {
				source[component] = VA_OPERAND_IMMEDIATE | VA_IMMEDIATE_ZERO;
			}
		}
		block = gather_block(c, source, 4U, 4U);
		release_temps(c);
		if (c->error != 0)
			return;
		bits = VA_OP_BLEND | va_source_special(VA_SPECIAL_BLEND_DESCRIPTOR_0 + location, 0U);
		bits |= va_field(va_source_register(VA_PRELOAD_COVERAGE), VA_SHIFT_SOURCE2);
		bits |= va_field(VA_REGISTER_FORMAT_F32, VA_SHIFT_REGISTER_FORMAT);
		bits |= va_field(3U, VA_SHIFT_VECTOR_SIZE);
		bits |= va_staging(block, 4U, VA_STAGING_READ);
		emit(c,
		     bits,
		     va_register_mask(block, 4U) | va_register_mask(VA_PRELOAD_COVERAGE, 1U),
		     0U,
		     VA_INST_MESSAGE | VA_INST_BLEND);
		for (component = 0U; component < 4U; component++)
			free_reg(c, block + component);
	}
}

/*
 * Starts a block the lanes whose predicate is zero skip: BRANCHZ.eq to
 * the instruction after the block, whose target branch_end() sets.
 */
static void
branch_begin(
	struct va_compiler *c,
	uint32_t predicate,
	uint32_t *branch)
{
	/* Branches on a zero predicate, reconverging. */
	emit(c,
	     VA_OP_BRANCHZ | va_source_register(predicate) | va_field(1U, VA_SHIFT_BRANCH_EQ) | va_dest(0U),
	     va_register_mask(predicate, 1U),
	     0U,
	     VA_INST_BRANCH);
	*branch = c->code.count - 1U;
	if (c->code.failed == 0)
		c->code.list[*branch].flow = VA_FLOW_RECONVERGE;
	c->in_branch++;
}

/* Ends a skipped block: the next instruction is the branch's target. */
static void
branch_end(
	struct va_compiler *c,
	uint32_t branch)
{
	/* The skipped block may end the program: a NOP is the target then. */
	c->in_branch--;
	if (c->code.failed != 0)
		return;
	c->code.list[branch].target = (int32_t)c->code.count;
	emit(c, VA_OP_NOP | va_dest(0U), 0U, 0U, 0U);
	if (c->code.failed == 0)
		c->code.list[c->code.count - 1U].flags |= VA_INST_TARGET;
}

/* Reports whether a vertex program's output location is NoPerspective. */
static int
noperspective_output(
	const struct drv_gpu_shader_ir *ir,
	uint32_t location)
{
	uint32_t index;

	/* Looks through the outputs. */
	for (index = 0U; index < ir->output_count; index++) {
		if (ir->outputs[index].location == location && ir->outputs[index].noperspective != 0U)
			return 1;
	}

	/* The location interpolates with perspective. */
	return 0;
}

/*
 * Lists the values an IR instruction reads (into sources when not NULL)
 * and reports how many.
 */
static uint32_t
ir_sources(
	const struct drv_gpu_shader_ir_inst *inst,
	uint32_t *sources)
{
	uint32_t list[VA_MAX_SOURCES];
	uint32_t count;
	uint32_t index;

	/* Names the sources by operation. */
	count = 0U;
	switch (inst->op) {
	case DRV_GPU_IR_NOP:
	case DRV_GPU_IR_CONST:
	case DRV_GPU_IR_ICONST:
	case DRV_GPU_IR_BOOL:
	case DRV_GPU_IR_LOAD_INPUT:
	case DRV_GPU_IR_LOAD_PUSH:
	case DRV_GPU_IR_LOAD_UBO:
	case DRV_GPU_IR_LOOP_BEGIN:
	case DRV_GPU_IR_SKIP_END:
	case DRV_GPU_IR_LOAD_SYSTEM:
	case DRV_GPU_IR_STORAGE_SIZE:
	case DRV_GPU_IR_BARRIER:
	case DRV_GPU_IR_FENCE:
		break;

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
	case DRV_GPU_IR_INEG:
	case DRV_GPU_IR_INOT:
	case DRV_GPU_IR_I2F:
	case DRV_GPU_IR_U2F:
	case DRV_GPU_IR_F2I:
	case DRV_GPU_IR_F2U:
	case DRV_GPU_IR_MOVE:
	case DRV_GPU_IR_LOOP_END:
	case DRV_GPU_IR_FROUND_EVEN:
	case DRV_GPU_IR_DDX:
	case DRV_GPU_IR_DDX_FINE:
	case DRV_GPU_IR_DDY:
	case DRV_GPU_IR_DDY_FINE:
	case DRV_GPU_IR_UNPACK_HALF:
	case DRV_GPU_IR_SKIP_BEGIN:
		list[count++] = inst->src[0];
		break;

	case DRV_GPU_IR_SELECT:
	case DRV_GPU_IR_SAMPLE_BIAS:
	case DRV_GPU_IR_SAMPLE_LOD:
		list[count++] = inst->src[0];
		list[count++] = inst->src[1];
		list[count++] = inst->src[2];
		break;

	case DRV_GPU_IR_LOAD_STORAGE:
	case DRV_GPU_IR_LOAD_SHARED:
		list[count++] = inst->src[0];
		if (inst->component == 1U)
			list[count++] = inst->src[1];
		break;

	case DRV_GPU_IR_STORE_STORAGE:
	case DRV_GPU_IR_STORE_SHARED:
		list[count++] = inst->src[0];
		list[count++] = inst->src[1];
		if (inst->component == 1U)
			list[count++] = inst->src[2];
		break;

	case DRV_GPU_IR_ATOMIC:
		/* An image texel's atomic reads the texel's three coordinates from src[0]. */
		if ((inst->image_shape & DRV_GPU_IR_IMAGE_TEXEL) != 0U) {
			list[count++] = inst->src[0] + 1U;
			list[count++] = inst->src[0] + 2U;
		}
		list[count++] = inst->src[0];
		list[count++] = inst->src[1];
		if (inst->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG)
			list[count++] = inst->src[2];
		if (inst->component == DRV_GPU_IR_ATOMIC_PREDICATED) {
			if (inst->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG) {
				list[count++] = inst->src[3];
			} else {
				list[count++] = inst->src[2];
			}
		}
		break;

	case DRV_GPU_IR_IMAGE_READ:
		list[count++] = inst->src[0];
		list[count++] = inst->src[0] + 1U;
		list[count++] = inst->src[0] + 2U;
		if (inst->component == 1U)
			list[count++] = inst->src[2];
		break;

	case DRV_GPU_IR_IMAGE_WRITE:
		list[count++] = inst->src[0];
		list[count++] = inst->src[0] + 1U;
		list[count++] = inst->src[0] + 2U;
		for (index = 0U; index < 4U; index++)
			list[count++] = inst->src[2] + index;
		if (inst->component == 1U)
			list[count++] = inst->src[3];
		break;

	case DRV_GPU_IR_TEXTURE:
		for (index = 0U; index < inst->src[1] && index < VA_MAX_SOURCES; index++)
			list[count++] = inst->src[0] + index;
		break;

	default:
		list[count++] = inst->src[0];
		list[count++] = inst->src[1];
		break;
	}

	/* Copies the list out when asked. */
	if (sources != NULL) {
		for (index = 0U; index < count; index++)
			sources[index] = list[index];
	}

	/* Reports the count. */
	return count;
}

/* Reports how many values an IR instruction defines, from its dst. */
static uint32_t
ir_defines(
	const struct drv_gpu_shader_ir_inst *inst)
{
	/* Dispatches on the operation. */
	switch (inst->op) {
	case DRV_GPU_IR_NOP:
	case DRV_GPU_IR_STORE_OUTPUT:
	case DRV_GPU_IR_KILL:
	case DRV_GPU_IR_LOOP_BEGIN:
	case DRV_GPU_IR_LOOP_END:
	case DRV_GPU_IR_STORE_STORAGE:
	case DRV_GPU_IR_STORE_SHARED:
	case DRV_GPU_IR_SKIP_BEGIN:
	case DRV_GPU_IR_SKIP_END:
	case DRV_GPU_IR_BARRIER:
	case DRV_GPU_IR_FENCE:
	case DRV_GPU_IR_EMIT_VERTEX:
	case DRV_GPU_IR_END_PRIMITIVE:
	case DRV_GPU_IR_IMAGE_WRITE:
		return 0U;

	case DRV_GPU_IR_SAMPLE:
	case DRV_GPU_IR_SAMPLE_BIAS:
	case DRV_GPU_IR_SAMPLE_LOD:
	case DRV_GPU_IR_TEXTURE:
	case DRV_GPU_IR_IMAGE_READ:
		return 4U;

	default:
		return 1U;
	}
}

/*
 * Finds each value's first definition and its last use: the last reader,
 * the program's end for an output, and a loop's end for a value made
 * before the loop and read in it, or moved into in it.
 */
static int
compute_liveness(
	struct va_compiler *c)
{
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t begin[VA_LOOP_DEPTH];
	uint32_t stack[VA_LOOP_DEPTH];
	uint32_t *loop_end;
	uint32_t sources[VA_MAX_SOURCES];
	uint32_t count;
	uint32_t depth;
	uint32_t index;
	uint32_t source;
	uint32_t value;
	uint32_t level;
	uint32_t defines;
	uint32_t end;

	/* Starts with nothing defined or used. */
	for (value = 0U; value <= c->ir->value_count; value++) {
		c->last_use[value] = 0U;
		c->defined_at[value] = VA_NOT_DEFINED;
	}

	/* Pairs each loop's begin with its end. */
	loop_end = kern_calloc((size_t)c->ir->instruction_count + 1U, sizeof(*loop_end));
	if (loop_end == NULL)
		return ENOMEM;
	depth = 0U;
	for (index = 0U; index < c->ir->instruction_count; index++) {
		inst = &c->ir->instructions[index];
		if (inst->op == DRV_GPU_IR_LOOP_BEGIN) {
			if (depth == VA_LOOP_DEPTH) {
				kern_free(loop_end);
				return ENOTSUP;
			}
			stack[depth] = index;
			depth++;
		} else if (inst->op == DRV_GPU_IR_LOOP_END) {
			if (depth == 0U) {
				kern_free(loop_end);
				return EINVAL;
			}
			depth--;
			loop_end[stack[depth]] = index;
		}
	}

	/* Records definitions and reads, extending reads in loops to the loop's end. */
	depth = 0U;
	end = c->ir->instruction_count;
	for (index = 0U; index < c->ir->instruction_count; index++) {
		inst = &c->ir->instructions[index];
		if (inst->op == DRV_GPU_IR_LOOP_BEGIN && depth < VA_LOOP_DEPTH) {
			begin[depth] = index;
			depth++;
		}
		if (inst->op == DRV_GPU_IR_LOOP_END && depth != 0U)
			depth--;

		/* A read keeps the value to here, and through every loop made after the value. */
		count = ir_sources(inst, sources);
		for (source = 0U; source < count; source++) {
			value = sources[source];
			if (value > c->ir->value_count)
				continue;
			if (c->last_use[value] < index)
				c->last_use[value] = index;
			for (level = 0U; level < depth; level++) {
				if (c->defined_at[value] < begin[level] && c->last_use[value] < loop_end[begin[level]])
					c->last_use[value] = loop_end[begin[level]];
			}
		}

		/* An output lives to the end, where it is written. */
		if (inst->op == DRV_GPU_IR_STORE_OUTPUT && inst->src[0] <= c->ir->value_count)
			c->last_use[inst->src[0]] = end;

		/* A MOVE target lives to its MOVE, and through the loops it is moved in. */
		if (inst->op == DRV_GPU_IR_MOVE && inst->dst <= c->ir->value_count) {
			if (c->last_use[inst->dst] < index)
				c->last_use[inst->dst] = index;
			for (level = 0U; level < depth; level++) {
				if (c->last_use[inst->dst] < loop_end[begin[level]])
					c->last_use[inst->dst] = loop_end[begin[level]];
			}
		}

		/* Records the first definition of what the instruction defines. */
		defines = ir_defines(inst);
		for (source = 0U; source < defines; source++) {
			value = inst->dst + source;
			if (value <= c->ir->value_count && c->defined_at[value] == VA_NOT_DEFINED)
				c->defined_at[value] = index;
		}
	}
	kern_free(loop_end);

	/* Succeeded: the live ranges are known. */
	return 0;
}

/* Records the first refusal of the lowering. */
static void
fail(
	struct va_compiler *c,
	int error,
	const char *reason)
{
	/* Keeps the first. */
	if (c->error != 0)
		return;
	c->error = error;
	c->reason = reason;
}

/*
 * Allocates count consecutive free registers, aligned to two for two and
 * to four for more; when none are free, spills values to thread local
 * storage until they are.
 */
static uint32_t
alloc_block(
	struct va_compiler *c,
	uint32_t count)
{
	uint32_t reg;
	int spilled;

	/* Takes a free run, spilling one value at a time while there is none. */
	for (;;) {
		reg = find_block(c, count);
		if (reg != VA_NO_REG)
			return reg;
		spilled = spill_one(c);
		if (spilled == 0)
			break;
	}

	/* The registers ran out, and no value could be spilled. */
	fail(c, ENOSPC, "the program needs more registers than the preloads leave and spilling frees");
	return VA_NO_REG;
}

/* Takes the lowest aligned run of count free registers, or reports VA_NO_REG. */
static uint32_t
find_block(
	struct va_compiler *c,
	uint32_t count)
{
	uint64_t mask;
	uint32_t alignment;
	uint32_t reg;

	/* Chooses the alignment. */
	alignment = 1U;
	if (count == 2U)
		alignment = 2U;
	if (count > 2U)
		alignment = 4U;

	/* Takes the lowest aligned run of free registers. */
	mask = va_register_mask(0U, count);
	for (reg = 0U; reg + count <= c->reg_limit; reg += alignment) {
		if (((c->free_regs >> reg) & mask) == mask) {
			c->free_regs &= ~(mask << reg);
			c->used_regs |= mask << reg;
			return reg;
		}
	}

	/* No run is free. */
	return VA_NO_REG;
}

/*
 * Spills the value that lives longest of those that may be spilled: its
 * register is stored to a word of thread local storage (unless the word
 * already holds it) and given back.  Reports whether a register was
 * freed.
 *
 * Procedure referenced: Mesa 26.2.6
 * src/panfrost/compiler/bifrost/bi_ra.c (bi_store_tl, bi_load_tl on v9
 * and later: the thread local storage pointer as the address, the
 * immediate offset, the force access hint).
 */
static int
spill_one(
	struct va_compiler *c)
{
	uint32_t value;
	uint32_t victim;
	uint32_t word;
	uint32_t reg;
	int candidate;

	/* Nothing is spilled where only some lanes run. */
	if (c->in_branch != 0U)
		return 0;

	/* Chooses the candidate whose last read is farthest. */
	victim = VA_NOT_DEFINED;
	for (value = 0U; value <= c->ir->value_count; value++) {
		candidate = spill_candidate(c, value);
		if (candidate == 0)
			continue;
		if (victim == VA_NOT_DEFINED || c->last_use[value] > c->last_use[victim])
			victim = value;
	}
	if (victim == VA_NOT_DEFINED)
		return 0;
	reg = c->reg[victim];

	/* Stores the register to a word of its own, unless a word already holds the value. */
	if (c->tls_word[victim] == VA_NO_TLS_WORD) {
		for (word = 0U; word < VA_TLS_WORDS; word++) {
			if (c->tls_used[word] == 0U)
				break;
		}
		if (word == VA_TLS_WORDS)
			return 0;
		c->tls_used[word] = 1U;
		if (word + 1U > c->tls_words)
			c->tls_words = word + 1U;
		c->tls_word[victim] = word;
		emit(c,
		     VA_OP_STORE_I32 | tls_access(word) | va_staging(reg, 1U, VA_STAGING_READ),
		     va_register_mask(reg, 1U),
		     0U,
		     VA_INST_MESSAGE | VA_INST_MEMORY);
	}

	/* Gives the register back. */
	free_reg(c, reg);
	c->reg[victim] = VA_NO_REG;

	/* Succeeded: a register is free. */
	return 1;
}

/*
 * Reports whether a value may be spilled now: it is in a register, not
 * pinned, not handed out in this step and not read by the instruction
 * being lowered; inside a loop it was made in the innermost loop's body
 * (a value made before the loop is read again from its register when the
 * loop goes round, so it may only be spilled before the loop).
 */
static int
spill_candidate(
	const struct va_compiler *c,
	uint32_t value)
{
	uint32_t index;

	/* Only a value of a register that a register need not keep. */
	if (c->reg[value] == VA_NO_REG || c->reg[value] >= c->reg_limit)
		return 0;
	if (c->is_constant[value] != 0U || c->pinned[value] != 0U)
		return 0;
	if (c->touched[value] == c->step)
		return 0;

	/* Not one the instruction reads. */
	for (index = 0U; index < c->source_count; index++) {
		if (c->sources[index] == value)
			return 0;
	}

	/* Inside a loop, only one the innermost loop's body made. */
	if (c->loop_depth != 0U && c->defined_at[value] <= c->loop_head[c->loop_depth - 1U])
		return 0;

	/* The value may be spilled. */
	return 1;
}

/* Loads a spilled value into a fresh register, which then holds it to its last read. */
static uint32_t
reload_value(
	struct va_compiler *c,
	uint32_t value)
{
	uint32_t reg;

	/* Nothing is loaded again where only some lanes run. */
	if (c->in_branch != 0U) {
		fail(c, ENOSPC, "a spilled value read where only some lanes run");
		return 0U;
	}

	/* Takes a register and loads the value's word into it. */
	reg = alloc_block(c, 1U);
	if (reg == VA_NO_REG)
		return 0U;
	emit(c,
	     VA_OP_LOAD_I32 | tls_access(c->tls_word[value]) | va_field(1U, VA_SHIFT_LOAD_UNSIGNED) | va_staging(reg, 1U, VA_STAGING_WRITE),
	     0U,
	     va_register_mask(reg, 1U),
	     VA_INST_MESSAGE | VA_INST_MEMORY);
	c->reg[value] = (uint8_t)reg;
	c->touched[value] = c->step;

	/* Reports the register. */
	return reg;
}

/* Loads a spilled value again before a block only some lanes run reads it. */
static void
reload_before_branch(
	struct va_compiler *c,
	uint32_t value)
{
	/* Only a spilled value of the program. */
	if (value > c->ir->value_count || c->reg[value] != VA_NO_REG || c->tls_word[value] == VA_NO_TLS_WORD)
		return;

	/* Loads it now. */
	(void)reload_value(c, value);
}

/*
 * Frees loop_reserve registers before the loop beginning at the
 * instruction being lowered: spills, the farthest read first, the values
 * that live through the loop without being read in it (inside the loop
 * they could no longer be spilled).
 */
static void
make_loop_room(
	struct va_compiler *c)
{
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t end;
	uint32_t depth;
	uint32_t value;
	uint32_t victim;
	uint32_t word;
	uint32_t reg;
	int candidate;
	int read;

	/* Nothing to free without a reserve. */
	if (c->loop_reserve == 0U)
		return;

	/* Finds the loop's end. */
	depth = 0U;
	for (end = c->at; end < c->ir->instruction_count; end++) {
		inst = &c->ir->instructions[end];
		if (inst->op == DRV_GPU_IR_LOOP_BEGIN)
			depth++;
		if (inst->op == DRV_GPU_IR_LOOP_END)
			depth--;
		if (depth == 0U)
			break;
	}

	/* Spills while fewer registers than the reserve are free. */
	while ((uint32_t)__builtin_popcountll(c->free_regs) < c->loop_reserve && c->error == 0) {
		victim = VA_NOT_DEFINED;
		for (value = 0U; value <= c->ir->value_count; value++) {
			candidate = spill_candidate(c, value);
			if (candidate == 0 || c->last_use[value] <= end)
				continue;
			read = read_in_loop(c, value, c->at, end);
			if (read != 0)
				continue;
			if (victim == VA_NOT_DEFINED || c->last_use[value] > c->last_use[victim])
				victim = value;
		}
		if (victim == VA_NOT_DEFINED)
			return;

		/* Stores it to a word of its own unless one holds it, and gives its register back. */
		reg = c->reg[victim];
		if (c->tls_word[victim] == VA_NO_TLS_WORD) {
			for (word = 0U; word < VA_TLS_WORDS; word++) {
				if (c->tls_used[word] == 0U)
					break;
			}
			if (word == VA_TLS_WORDS)
				return;
			c->tls_used[word] = 1U;
			if (word + 1U > c->tls_words)
				c->tls_words = word + 1U;
			c->tls_word[victim] = word;
			emit(c,
			     VA_OP_STORE_I32 | tls_access(word) | va_staging(reg, 1U, VA_STAGING_READ),
			     va_register_mask(reg, 1U),
			     0U,
			     VA_INST_MESSAGE | VA_INST_MEMORY);
		}
		free_reg(c, reg);
		c->reg[victim] = VA_NO_REG;
	}
}

/* Reports whether an instruction of [begin, end] reads a value. */
static int
read_in_loop(
	const struct va_compiler *c,
	uint32_t value,
	uint32_t begin,
	uint32_t end)
{
	uint32_t sources[VA_MAX_SOURCES];
	uint32_t count;
	uint32_t index;
	uint32_t source;

	/* Looks through the loop's instructions. */
	for (index = begin; index <= end && index < c->ir->instruction_count; index++) {
		count = ir_sources(&c->ir->instructions[index], sources);
		for (source = 0U; source < count; source++) {
			if (sources[source] == value)
				return 1;
		}
	}

	/* No instruction of the loop reads it. */
	return 0;
}

/*
 * Pins the values a register must keep: a loop variable (a value more
 * than one MOVE writes, each into its register; a value one MOVE makes is
 * a plain copy) and a predicated load's result (the lanes that skip the
 * load keep what the register held).  pinned counts a value's MOVEs on
 * the way (1 for one, 2 for more).
 */
static void
mark_pinned(
	struct va_compiler *c)
{
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t index;
	uint32_t value;

	/* Counts each value's MOVEs, and pins the predicated loads' results. */
	for (index = 0U; index < c->ir->instruction_count; index++) {
		inst = &c->ir->instructions[index];
		if (inst->dst > c->ir->value_count)
			continue;
		if (inst->op == DRV_GPU_IR_MOVE && c->pinned[inst->dst] < 2U)
			c->pinned[inst->dst]++;
		if ((inst->op == DRV_GPU_IR_LOAD_STORAGE || inst->op == DRV_GPU_IR_LOAD_SHARED) && inst->component == 1U)
			c->pinned[inst->dst] = 2U;
	}

	/* Keeps the pins of the values written more than once. */
	for (value = 0U; value <= c->ir->value_count; value++) {
		if (c->pinned[value] < 2U)
			c->pinned[value] = 0U;
	}
}

/*
 * The address fields of a thread local storage word's access: the
 * thread local storage pointer (FAU special page 1), the word's byte
 * offset and the force hint (the access is made in a discarded lane
 * too).
 */
static uint64_t
tls_access(
	uint32_t word)
{
	uint64_t bits;

	/* The pointer, its page, the offset and the hint. */
	bits = va_source_special(VA_SPECIAL_THREAD_LOCAL, 0U);
	bits |= va_field(1U, VA_SHIFT_FAU_PAGE);
	bits |= va_field(word * 4U, VA_SHIFT_MEMORY_OFFSET);
	bits |= va_field(VA_MEMORY_FORCE, VA_SHIFT_MEMORY_ACCESS);

	/* Reports the fields. */
	return bits;
}

/* Gives a register back. */
static void
free_reg(
	struct va_compiler *c,
	uint32_t reg)
{
	/* Only an allocatable register returns to the pool. */
	if (reg >= c->reg_limit)
		return;
	c->free_regs |= (uint64_t)1U << reg;
}

/* Reports the register of a defined value. */
static uint32_t
value_reg(
	struct va_compiler *c,
	uint32_t value)
{
	/* A constant is made again in a temporary the instruction frees. */
	if (value <= c->ir->value_count && c->is_constant[value] != 0U)
		return constant_temp(c, c->constant[value]);

	/* A spilled value is loaded again. */
	if (value <= c->ir->value_count && c->reg[value] == VA_NO_REG && c->tls_word[value] != VA_NO_TLS_WORD)
		return reload_value(c, value);

	/* A value that has no register is a lowering error. */
	if (value > c->ir->value_count || c->reg[value] == VA_NO_REG) {
		fail(c, EINVAL, "a value read before its definition");
		return 0U;
	}

	/* Reports the register, handed out in this step. */
	c->touched[value] = c->step;
	return c->reg[value];
}

/* Gives a newly defined value a register. */
static uint32_t
define_value(
	struct va_compiler *c,
	uint32_t value)
{
	uint32_t reg;

	/* A value outside the IR is refused. */
	if (value > c->ir->value_count) {
		fail(c, EINVAL, "a value outside the program");
		return 0U;
	}

	/* Allocates one register, handed out in this step. */
	reg = alloc_block(c, 1U);
	if (reg == VA_NO_REG)
		return 0U;
	c->reg[value] = (uint8_t)reg;
	c->touched[value] = c->step;

	/* Reports the register. */
	return reg;
}

/* Gives a value a register already allocated for it. */
static void
bind_value(
	struct va_compiler *c,
	uint32_t value,
	uint32_t reg)
{
	/* A value outside the IR is refused. */
	if (value > c->ir->value_count) {
		fail(c, EINVAL, "a value outside the program");
		return;
	}
	c->reg[value] = (uint8_t)reg;
	c->touched[value] = c->step;
}

/* Frees the registers of the values that die after instruction index. */
static void
release_after(
	struct va_compiler *c,
	uint32_t index)
{
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t sources[VA_MAX_SOURCES];
	uint32_t count;
	uint32_t source;
	uint32_t value;
	uint32_t defines;

	inst = &c->ir->instructions[index];

	/* The instruction's temporaries die. */
	release_temps(c);

	/* A source whose last use is here dies, its spill word too. */
	count = ir_sources(inst, sources);
	for (source = 0U; source < count; source++) {
		value = sources[source];
		if (value > c->ir->value_count || c->last_use[value] != index)
			continue;
		if (c->tls_word[value] != VA_NO_TLS_WORD) {
			c->tls_used[c->tls_word[value]] = 0U;
			c->tls_word[value] = VA_NO_TLS_WORD;
		}
		if (c->reg[value] == VA_NO_REG)
			continue;
		free_reg(c, c->reg[value]);
		c->reg[value] = VA_NO_REG;
	}

	/* A value defined here that nothing reads dies at once. */
	defines = ir_defines(inst);
	for (source = 0U; source < defines; source++) {
		value = inst->dst + source;
		if (value > c->ir->value_count || c->reg[value] == VA_NO_REG)
			continue;
		if (c->last_use[value] <= index && c->defined_at[value] == index) {
			free_reg(c, c->reg[value]);
			c->reg[value] = VA_NO_REG;
		}
	}
}

/*
 * Reports a value as an operand: a constant the constant table has is
 * read from the table, another constant made in a temporary.
 */
static uint32_t
value_operand(
	struct va_compiler *c,
	uint32_t value)
{
	int entry;

	/* A constant of the table is read from it. */
	if (value <= c->ir->value_count && c->is_constant[value] != 0U) {
		entry = va_immediate_entry(c->constant[value]);
		if (entry >= 0)
			return VA_OPERAND_IMMEDIATE | (uint32_t)entry;
	}

	/* Anything else is a register. */
	return value_reg(c, value);
}

/* Makes a constant in a temporary register the instruction frees when it is lowered. */
static uint32_t
constant_temp(
	struct va_compiler *c,
	uint32_t value)
{
	uint32_t reg;

	/* The instruction's temporaries are bounded. */
	if (c->temp_count == VA_MAX_TEMPS) {
		fail(c, ENOSPC, "an instruction reads too many constants");
		return 0U;
	}

	/* Allocates the register and writes the constant. */
	reg = alloc_block(c, 1U);
	if (reg == VA_NO_REG)
		return 0U;
	emit_constant(c, reg, value);
	c->temps[c->temp_count] = reg;
	c->temp_count++;

	/* Reports the register. */
	return reg;
}

/* Frees the temporaries of the instruction just lowered. */
static void
release_temps(
	struct va_compiler *c)
{
	uint32_t index;

	/* Gives each temporary back. */
	for (index = 0U; index < c->temp_count; index++)
		free_reg(c, c->temps[index]);
	c->temp_count = 0U;
}

/* Encodes an operand as a source byte. */
static uint8_t
operand_byte(
	uint32_t operand)
{
	/* An entry of the constant table, nothing (the constant zero), or a register. */
	if ((operand & VA_OPERAND_IMMEDIATE) != 0U && operand != VA_OPERAND_NONE)
		return va_source_immediate(operand & 0xFFU);
	if (operand == VA_OPERAND_NONE)
		return 0U;

	/* Reports the register. */
	return va_source_register(operand);
}

/* Reports whether an operand is an entry of the constant table. */
static int
is_immediate(
	uint32_t operand)
{
	/* Nothing is not an entry. */
	if (operand == VA_OPERAND_NONE)
		return 0;
	if ((operand & VA_OPERAND_IMMEDIATE) == 0U)
		return 0;

	/* An entry of the table. */
	return 1;
}

/* Reports the registers an operand reads. */
static uint64_t
operand_mask(
	uint32_t operand)
{
	/* A constant reads no register. */
	if ((operand & VA_OPERAND_IMMEDIATE) != 0U)
		return 0U;

	/* Reports the register. */
	return va_register_mask(operand, 1U);
}

/* Appends one instruction. */
static void
emit(
	struct va_compiler *c,
	uint64_t bits,
	uint64_t reads,
	uint64_t writes,
	uint32_t flags)
{
	struct va_inst inst;

	/* A failed lowering appends nothing more. */
	if (c->error != 0)
		return;

	/* Builds the instruction. */
	inst.bits = bits;
	inst.reads = reads;
	inst.writes = writes;
	inst.target = VA_NO_TARGET;
	inst.flow = VA_FLOW_NONE;
	inst.flags = (uint8_t)flags;
	c->used_regs |= reads | writes;

	/* Appends it. */
	va_code_append(&c->code, &inst);
}

/* Appends an arithmetic instruction on up to three operands. */
static void
emit_alu(
	struct va_compiler *c,
	uint64_t bits,
	uint32_t dst,
	uint32_t a,
	uint32_t b,
	uint32_t third)
{
	uint64_t reads;
	int immediates;

	/* Counts the operands read from the constant table. */
	immediates = is_immediate(a);
	immediates += is_immediate(b);
	immediates += is_immediate(third);

	/*
	 * An instruction reads at most two words of the constant table and
	 * the FAU: a third distinct constant is made in a temporary.
	 */
	if (immediates == 3 && a != b && b != third && a != third)
		a = constant_temp(c, va_immediate_value(a & 0x1FU));

	/* Places the operands and the destination. */
	bits |= operand_byte(a);
	reads = 0U;
	if (a != VA_OPERAND_NONE)
		reads |= operand_mask(a);
	if (b != VA_OPERAND_NONE) {
		bits |= va_field(operand_byte(b), VA_SHIFT_SOURCE1);
		reads |= operand_mask(b);
	}
	if (third != VA_OPERAND_NONE) {
		bits |= va_field(operand_byte(third), VA_SHIFT_SOURCE2);
		reads |= operand_mask(third);
	}
	bits |= va_dest(dst);

	/* Appends it. */
	emit(c, bits, reads, va_register_mask(dst, 1U), 0U);
}
