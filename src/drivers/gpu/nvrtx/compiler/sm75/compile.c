/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The entry points of the nvrtx shader compiler (see sm75.h).
 *
 * A SPIR-V module goes through the shared frontend and optimizer
 * (drivers/gpu/compiler/) and then through the lowering (see lower.h).
 * This file checks the program and maps its uniforms to the constant
 * buffers, lowers its instructions in order, appends each instruction
 * the lowering emits, and completes the program: its end and padding,
 * the reordering and the dependence marking (schedule.c), and a graphics
 * program's header.
 */

#include "drivers/gpu/nvrtx/compiler/sm75/lower.h"
#include "drivers/gpu/nvrtx/compiler/sm75/schedule.h"

#include "drivers/gpu/compiler/optimize.h"
#include "drivers/gpu/compiler/spirv.h"

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include <stddef.h>

/* The most SM75 instructions one IR instruction becomes, and the end of the program (EXIT, BRA and the padding). */
#define NVRTX_SM75_EXPANSION		40U
#define NVRTX_SM75_TAIL			10U

/* The instructions in one 128-byte line of code, to which the program is padded. */
#define NVRTX_SM75_LINE_INSTRUCTIONS	8U

static int nvrtx_sm75_known_model(uint32_t sm);
static int nvrtx_sm75_is_global(const struct nvrtx_sm75_word *word);
static int nvrtx_sm75_prepare(struct nvrtx_sm75_lowering *lowering);
static int nvrtx_sm75_map_uniforms(struct nvrtx_sm75_lowering *lowering);
static int nvrtx_sm75_finish(struct nvrtx_sm75_lowering *lowering);

/*
 * Compiles a SPIR-V compute module.
 *
 * The module is parsed by the shared frontend, optimized, and lowered; a
 * program the lowering refuses after the optimizer rewrote it is lowered
 * again from the parsed one, which keeps the parsed program's outcome.
 * Returns 0 with the program, or the frontend's or the lowering's error
 * with the diagnostic filled.
 */
int
nvrtx_sm75_compile(
	const uint32_t *words,
	size_t word_count,
	struct nvrtx_sm75_binary **binary,
	struct nvrtx_sm75_diagnostic *diagnostic)
{
	int error;

	/* The compute stage. */
	error = nvrtx_sm75_compile_stage(words, word_count, DRV_GPU_STAGE_COMPUTE, binary, diagnostic);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the program. */
	return 0;
}

/*
 * Compiles one stage of a SPIR-V module (compute, vertex or fragment) for
 * Turing, as nvrtx_sm75_compile() does the compute one.
 */
int
nvrtx_sm75_compile_stage(
	const uint32_t *words,
	size_t word_count,
	enum drv_gpu_shader_stage stage,
	struct nvrtx_sm75_binary **binary,
	struct nvrtx_sm75_diagnostic *diagnostic)
{
	int error;

	/* Turing's code. */
	error = nvrtx_sm75_compile_stage_sm(words, word_count, stage, NVRTX_SM_TURING, binary, diagnostic);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the program. */
	return 0;
}

/*
 * Compiles a parsed program for Turing.
 *
 * Returns 0 with the program in *binary, or the error of
 * nvrtx_sm75_compile_ir_sm().
 */
int
nvrtx_sm75_compile_ir(
	const struct drv_gpu_shader_ir *ir,
	struct nvrtx_sm75_binary **binary,
	struct nvrtx_sm75_diagnostic *diagnostic)
{
	int error;

	/* Turing's code. */
	error = nvrtx_sm75_compile_ir_sm(ir, NVRTX_SM_TURING, binary, diagnostic);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the program. */
	return 0;
}

/*
 * Compiles one stage of a SPIR-V module for a shader model.
 *
 * The module is parsed by the shared frontend, optimized, and lowered; a
 * program the lowering refuses after the optimizer rewrote it is lowered
 * again from the parsed one, which keeps the parsed program's outcome.
 * Returns 0 with the program, or the frontend's or the lowering's error
 * with the diagnostic filled.
 */
int
nvrtx_sm75_compile_stage_sm(
	const uint32_t *words,
	size_t word_count,
	enum drv_gpu_shader_stage stage,
	uint32_t sm,
	struct nvrtx_sm75_binary **binary,
	struct nvrtx_sm75_diagnostic *diagnostic)
{
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_shader_ir optimized;
	struct drv_gpu_shader_optimize_options options;
	struct drv_gpu_compile_diagnostic parse_diagnostic;
	int error;

	/* A refused compile never leaves an old output pointer live. */
	if (binary == NULL)
		return EINVAL;
	*binary = NULL;

	/* Starts with no refusal. */
	if (diagnostic != NULL)
		kern_memset(diagnostic, 0, sizeof(*diagnostic));

	/* A module without words is refused. */
	if (words == NULL)
		return EINVAL;

	/* Parses the module's stage. */
	ir = NULL;
	parse_diagnostic.reason = NULL;
	error = drv_gpu_shader_parse(words, word_count, stage, &ir, &parse_diagnostic);
	if (error != 0) {
		if (diagnostic != NULL) {
			diagnostic->instruction = parse_diagnostic.word_offset;
			diagnostic->reason = parse_diagnostic.reason;
		}

		/* Reports the frontend's refusal. */
		return error;
	}

	/* Optimizes a private copy: every block runs (there are no skipped blocks), and the known bits fold. */
	kern_memset(&options, 0, sizeof(options));
	options.runs_every_block = 1U;
	options.simplify = 1U;
	kern_memset(options.needed, 1, sizeof(options.needed));
	error = drv_gpu_shader_optimize(ir, &options, &optimized);
	if (error != 0) {
		drv_gpu_shader_ir_free(ir);
		return error;
	}

	/* Lowers the rewritten program, or the parsed one when the optimizer left it alone. */
	if (optimized.instructions != NULL) {
		error = nvrtx_sm75_compile_ir_sm(&optimized, sm, binary, diagnostic);
	} else {
		error = nvrtx_sm75_compile_ir_sm(ir, sm, binary, diagnostic);
	}

	/* A rewritten program refused for any reason but memory is lowered from the parsed one. */
	if (optimized.instructions != NULL &&
	    error != 0 &&
	    error != ENOMEM) {
		if (diagnostic != NULL)
			kern_memset(diagnostic, 0, sizeof(*diagnostic));
		error = nvrtx_sm75_compile_ir_sm(ir, sm, binary, diagnostic);
	}

	/* The IR retires before the program is handed over. */
	kern_free(optimized.instructions);
	drv_gpu_shader_ir_free(ir);

	/* Reports the lowering's refusal. */
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the program. */
	return 0;
}

/*
 * Compiles a parsed program for a shader model.
 *
 * Returns 0 with the program in *binary, EINVAL for a malformed program,
 * ENOTSUP for one the compiler does not cover or a shader model it does
 * not target (both with the diagnostic), or ENOMEM.
 */
int
nvrtx_sm75_compile_ir_sm(
	const struct drv_gpu_shader_ir *ir,
	uint32_t sm,
	struct nvrtx_sm75_binary **binary,
	struct nvrtx_sm75_diagnostic *diagnostic)
{
	struct nvrtx_sm75_lowering lowering;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t dst;
	uint32_t index;
	int defines;
	int known;
	int error;

	/* A refused compile never leaves an old output pointer live. */
	if (binary == NULL || ir == NULL)
		return EINVAL;
	*binary = NULL;

	/* A shader model the compiler does not target is refused. */
	known = nvrtx_sm75_known_model(sm);
	if (known == 0) {
		if (diagnostic != NULL) {
			diagnostic->instruction = 0U;
			diagnostic->reason = "the shader model is not one the compiler targets (75, 86 or 89)";
		}

		/* Reports the shader model as not covered. */
		return ENOTSUP;
	}

	/* The state, empty. */
	kern_memset(&lowering, 0, sizeof(lowering));
	lowering.sm = sm;
	lowering.ir = ir;
	lowering.diagnostic = diagnostic;

	/* The result, for the shader model. */
	lowering.binary = kern_calloc(1U, sizeof(*lowering.binary));
	if (lowering.binary == NULL)
		return ENOMEM;
	lowering.binary->shader_model = sm;

	/* Checks the program, finds the last reads, maps the uniforms and makes room for the code. */
	error = nvrtx_sm75_prepare(&lowering);

	/* A fragment program's 1 / w first. */
	if (error == 0)
		error = nvrtx_sm75_prologue(&lowering);

	/* Lowers every instruction in order. */
	for (index = 0U; error == 0 && index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		lowering.index = index;

		/* A product fused into the addition that reads it emits nothing here. */
		defines = nvrtx_sm75_defines(instruction);
		if (defines != 0 && lowering.fold[instruction->dst] == NVRTX_SM75_FOLD_PRODUCT)
			continue;

		/* A word read in place is checked but needs no register: it is lowered into RZ, which emits nothing. */
		if (defines != 0 && lowering.fold[instruction->dst] == NVRTX_SM75_FOLD_OPERAND)
			defines = 0;

		/* The destination's register, a value redefined by a MOVE keeping its own. */
		dst = NVRTX_SM75_RZ;
		if (defines != 0) {
			if (lowering.reg[instruction->dst] == NVRTX_SM75_NO_REG) {
				error = nvrtx_sm75_take(&lowering, &dst);
				if (error != 0)
					break;
				lowering.reg[instruction->dst] = (uint16_t)dst;
			}

			/* The value's register, just taken or its own. */
			dst = lowering.reg[instruction->dst];
		}

		/* The instruction's SM75 instructions. */
		error = nvrtx_sm75_lower(&lowering, instruction, dst);
		if (error != 0)
			break;

		/* Registers of values no later instruction reads go back. */
		nvrtx_sm75_retire(&lowering, instruction);
	}

	/* The end of the program. */
	if (error == 0)
		error = nvrtx_sm75_finish(&lowering);

	/* The per-value and per-uniform arrays are only the compilation's. */
	kern_free(lowering.reg);
	kern_free(lowering.last_use);
	kern_free(lowering.slot);
	kern_free(lowering.fold);
	kern_free(lowering.def);

	/* A refused program is never handed over. */
	if (error != 0) {
		nvrtx_sm75_binary_free(lowering.binary);
		return error;
	}

	/* Succeeded: the caller owns the program. */
	*binary = lowering.binary;
	return 0;
}

/*
 * Frees a compiled program.
 */
void
nvrtx_sm75_binary_free(
	struct nvrtx_sm75_binary *binary)
{
	/* Nothing to free. */
	if (binary == NULL)
		return;

	/* The code, then the record. */
	kern_free(binary->code);
	kern_free(binary);
}

/*
 * Records a refusal of the current instruction and reports its error.
 */
int
nvrtx_sm75_refuse(
	struct nvrtx_sm75_lowering *lowering,
	int error,
	const char *reason)
{
	/* The instruction and the reason, when the caller asked for them. */
	if (lowering->diagnostic != NULL) {
		lowering->diagnostic->instruction = lowering->index;
		lowering->diagnostic->reason = reason;
	}

	/* Succeeded in describing the refusal; the error goes back. */
	return error;
}

/*
 * Appends an instruction.  Its scheduling control is set when the program
 * is complete (nvrtx_sm75_finish); here the kind the lowering emits it as is
 * checked against how the dependence marking reads it back: a
 * variable-latency kind must be an instruction with a scoreboard, a fixed
 * one an instruction without, and a branch, convergence or barrier kind an
 * instruction that ends its block.
 */
int
nvrtx_sm75_emit(
	struct nvrtx_sm75_lowering *lowering,
	struct nvrtx_sm75_word *word,
	int kind)
{
	struct nvrtx_sm75_operands operands;
	uint32_t *slot;
	int unknown;
	int variable;
	int ends_block;
	int global;
	int error;

	/* The code has room for every expansion; running out is a bug of the lowering. */
	if (lowering->count >= lowering->capacity) {
		error = nvrtx_sm75_refuse(lowering, EINVAL, "the code outgrew its room");
		return error;
	}

	/* How the dependence marking reads the instruction. */
	unknown = nvrtx_sm75_operands(word->w, &operands);
	if (unknown != 0) {
		error = nvrtx_sm75_refuse(lowering, EINVAL, "the dependence marking does not know an instruction the lowering emitted");
		return error;
	}

	/* The variable-latency kinds, which the marking gives a scoreboard. */
	variable = 0;
	if (kind == NVRTX_SM75_VARIABLE_WRITE ||
	    kind == NVRTX_SM75_VARIABLE_STORE ||
	    kind == NVRTX_SM75_VARIABLE_SPECIAL)
		variable = 1;

	/* The kinds that end a block: branches, convergence and barriers. */
	ends_block = 0;
	if (kind == NVRTX_SM75_CONTROL || kind == NVRTX_SM75_SYNC)
		ends_block = 1;

	/* The marking ends a block at the same instructions as the kind says. */
	if (ends_block != operands.boundary) {
		error = nvrtx_sm75_refuse(lowering, EINVAL, "an instruction emitted as a block's end that does not end one, or the reverse");
		return error;
	}

	/* A variable-latency kind is an instruction the marking gives a scoreboard. */
	if (ends_block == 0 &&
	    variable != 0 &&
	    operands.class_of != NVRTX_SM75_CLASS_DECOUPLED) {
		error = nvrtx_sm75_refuse(lowering, EINVAL, "an instruction emitted as variable-latency that has no scoreboard");
		return error;
	}

	/* A fixed-latency kind is one the marking does not. */
	if (ends_block == 0 &&
	    variable == 0 &&
	    operands.class_of == NVRTX_SM75_CLASS_DECOUPLED) {
		error = nvrtx_sm75_refuse(lowering, EINVAL, "an instruction emitted as fixed-latency that needs a scoreboard");
		return error;
	}

	/* A global access gets the shader model's memory order field. */
	global = nvrtx_sm75_is_global(word);
	if (global != 0)
		nvrtx_sm75_memory_order(word, lowering->sm);

	/* The four words into the code. */
	slot = lowering->binary->code + (size_t)lowering->count * NVRTX_SM75_INSTRUCTION_WORDS;
	slot[0] = word->w[0];
	slot[1] = word->w[1];
	slot[2] = word->w[2];
	slot[3] = word->w[3];
	lowering->count++;

	/* Succeeded: the instruction is in the code. */
	return 0;
}

/* Reports nonzero for a shader model the compiler targets: Turing (75), Ampere (86) and Ada (89). */
static int
nvrtx_sm75_known_model(
	uint32_t sm)
{
	/* The three. */
	if (sm == NVRTX_SM_TURING)
		return 1;
	if (sm == NVRTX_SM_AMPERE)
		return 1;
	if (sm == NVRTX_SM_ADA)
		return 1;

	/* Any other model is not targeted. */
	return 0;
}

/* Reports nonzero for an instruction that accesses global memory (LDG, STG and the global atomics). */
static int
nvrtx_sm75_is_global(
	const struct nvrtx_sm75_word *word)
{
	uint32_t opcode;

	/* The 12-bit opcode (the memory accesses have no ALU form). */
	opcode = (uint32_t)nvrtx_sm75_get(word, NVRTX_SM75_OPCODE_LOW, NVRTX_SM75_OPCODE_BITS);

	/* The loads and the stores. */
	if (opcode == NVRTX_SM75_OP_LDG || opcode == NVRTX_SM75_OP_STG)
		return 1;

	/* The atomics. */
	if (opcode == NVRTX_SM75_OP_ATOMG || opcode == NVRTX_SM75_OP_ATOMG_CAS)
		return 1;

	/* Succeeded: no global access. */
	return 0;
}

/*
 * Checks that the program uses only operations the compiler covers, finds each
 * value's last read, maps the uniforms, and allocates the per-value arrays
 * and the code.
 */
static int
nvrtx_sm75_prepare(
	struct nvrtx_sm75_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	struct nvrtx_sm75_binary *binary;
	uint32_t sources[NVRTX_SM75_MAX_SOURCES];
	uint32_t source_count;
	uint32_t index;
	uint32_t source;
	int defines;
	int count;
	int error;

	/* The program and the result being built. */
	ir = lowering->ir;
	binary = lowering->binary;

	/* Compute, vertex and fragment programs are compiled. */
	if (ir->stage != DRV_GPU_STAGE_COMPUTE &&
	    ir->stage != DRV_GPU_STAGE_VERTEX &&
	    ir->stage != DRV_GPU_STAGE_FRAGMENT) {
		error = nvrtx_sm75_refuse(lowering, ENOTSUP, "only compute, vertex and fragment shaders are compiled");
		return error;
	}

	/* Each value's register. */
	lowering->reg = kern_calloc(ir->value_count + 1U, sizeof(*lowering->reg));
	if (lowering->reg == NULL)
		return ENOMEM;

	/* Each value's last read. */
	lowering->last_use = kern_calloc(ir->value_count + 1U, sizeof(*lowering->last_use));
	if (lowering->last_use == NULL)
		return ENOMEM;

	/* No value has a register yet. */
	for (index = 0U; index <= ir->value_count; index++)
		lowering->reg[index] = NVRTX_SM75_NO_REG;

	/* Each instruction: covered, its values in range, its sources' last reads. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		lowering->index = index;

		/* The sources of an operation the compiler covers. */
		count = nvrtx_sm75_sources(instruction, sources);
		if (count < 0) {
			error = nvrtx_sm75_refuse(lowering, ENOTSUP, "the operation is not compiled yet");
			return error;
		}

		/* The destination is a value of the program. */
		defines = nvrtx_sm75_defines(instruction);
		if (defines != 0 && instruction->dst >= ir->value_count) {
			error = nvrtx_sm75_refuse(lowering, EINVAL, "a destination is not a value of the program");
			return error;
		}

		/* A sample's four results are values of the program. */
		defines = nvrtx_sm75_is_sample(instruction->op);
		if (defines != 0 &&
		    (instruction->dst >= ir->value_count ||
		     ir->value_count - instruction->dst < 4U)) {
			error = nvrtx_sm75_refuse(lowering, EINVAL, "a sample's results are not values of the program");
			return error;
		}

		/* Each source is a value of the program, read last here so far. */
		source_count = (uint32_t)count;
		for (source = 0U; source < source_count; source++) {
			/* A source outside the program's values is refused. */
			if (sources[source] >= ir->value_count) {
				error = nvrtx_sm75_refuse(lowering, EINVAL, "a source is not a value of the program");
				return error;
			}

			/* The value is read here, the last time so far. */
			lowering->last_use[sources[source]] = index + 1U;
		}
	}

	/* The values that need no register of their own: words read in place and fused products. */
	error = nvrtx_sm75_plan_folds(lowering);
	if (error != 0)
		return error;

	/* Values read inside a loop that were made before it live to the loop's end. */
	error = nvrtx_sm75_extend_loops(lowering);
	if (error != 0)
		return error;

	/* The uniforms' slots. */
	error = nvrtx_sm75_map_uniforms(lowering);
	if (error != 0)
		return error;

	/* A graphics program's outputs, inputs and header. */
	error = nvrtx_sm75_prepare_graphics(lowering);
	if (error != 0)
		return error;

	/* The code: every instruction's largest expansion and the end. */
	lowering->capacity = ir->instruction_count * NVRTX_SM75_EXPANSION + NVRTX_SM75_TAIL;
	binary->code = kern_calloc(lowering->capacity, NVRTX_SM75_INSTRUCTION_BYTES);
	if (binary->code == NULL)
		return ENOMEM;

	/* The program's interface. */
	binary->local_size[0] = ir->local_size[0];
	binary->local_size[1] = ir->local_size[1];
	binary->local_size[2] = ir->local_size[2];
	binary->shared_bytes = ir->shared_bytes;
	binary->barrier_count = 0U;
	if (ir->uses_barrier != 0U)
		binary->barrier_count = 1U;

	/* Succeeded: the program can be lowered. */
	lowering->index = 0U;
	return 0;
}

/*
 * Gives each storage buffer of the uniform list a slot of the storage table
 * and each uniform buffer a constant buffer, in the list's order, and
 * records them in the result.
 */
static int
nvrtx_sm75_map_uniforms(
	struct nvrtx_sm75_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_uniform *uniform;
	struct nvrtx_sm75_binary *binary;
	uint32_t index;
	int error;

	/* The program and the result being built. */
	ir = lowering->ir;
	binary = lowering->binary;

	/* One slot per uniform. */
	lowering->slot = kern_calloc(ir->uniform_count + 1U, sizeof(*lowering->slot));
	if (lowering->slot == NULL)
		return ENOMEM;

	/* Each uniform in order. */
	for (index = 0U; index < ir->uniform_count; index++) {
		uniform = &ir->uniforms[index];
		lowering->slot[index] = NVRTX_SM75_NO_SLOT;

		/* Each kind of buffer takes the next place of its own. */
		if (uniform->kind == DRV_GPU_IR_UNIFORM_STORAGE) {
			/* A storage buffer takes the next entry of the table, which must have one. */
			if (binary->storage_count == NVRTX_SM75_CB_STORAGE_MAX) {
				error = nvrtx_sm75_refuse(lowering, ENOTSUP, "more storage buffers than the table holds");
				return error;
			}

			/* The entry is the buffer's. */
			lowering->slot[index] = binary->storage_count;
			binary->storage[binary->storage_count].set = uniform->set;
			binary->storage[binary->storage_count].binding = uniform->binding;
			binary->storage_count++;
		} else if (uniform->kind == DRV_GPU_IR_UNIFORM_SAMPLED_IMAGE) {
			/* A sampled image takes the next texture handle, which must have one. */
			if (binary->texture_count == NVRTX_SM75_CB_TEXTURES_MAX) {
				error = nvrtx_sm75_refuse(lowering, ENOTSUP, "more sampled images than texture handles");
				return error;
			}

			/* The handle is the image's. */
			lowering->slot[index] = binary->texture_count;
			binary->textures[binary->texture_count].set = uniform->set;
			binary->textures[binary->texture_count].binding = uniform->binding;
			binary->texture_count++;
		} else if (uniform->kind == DRV_GPU_IR_UNIFORM_BLOCK) {
			/* A uniform buffer takes the next constant buffer, which must have one. */
			if (binary->uniform_count == NVRTX_SM75_CB_UNIFORM_MAX) {
				error = nvrtx_sm75_refuse(lowering, ENOTSUP, "more uniform buffers than constant buffers");
				return error;
			}

			/* The constant buffer is the buffer's, its size the bytes the program reads. */
			lowering->slot[index] = NVRTX_SM75_CB_UNIFORM_FIRST + binary->uniform_count;
			binary->uniforms[binary->uniform_count].set = uniform->set;
			binary->uniforms[binary->uniform_count].binding = uniform->binding;
			binary->uniforms[binary->uniform_count].size = uniform->offset + uniform->size;
			binary->uniform_count++;
		}
	}

	/* Succeeded: every buffer has its place. */
	return 0;
}

/*
 * Ends the program: EXIT, a branch to itself (the fetch ahead never runs
 * past the end), and NOPs to a whole 128-byte line; then marks the
 * dependences of every instruction.  Records the code's size and the
 * register count.
 */
static int
nvrtx_sm75_finish(
	struct nvrtx_sm75_lowering *lowering)
{
	struct nvrtx_sm75_word word;
	const char *reason;
	uint32_t instruction;
	int error;

	/* EXIT. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_exit(&word);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_CONTROL);
	if (error != 0)
		return error;

	/* The branch to itself. */
	nvrtx_sm75_begin(&word);
	nvrtx_sm75_bra(&word, -1);
	error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_CONTROL);
	if (error != 0)
		return error;

	/* The padding. */
	while ((lowering->count % NVRTX_SM75_LINE_INSTRUCTIONS) != 0U) {
		nvrtx_sm75_begin(&word);
		nvrtx_sm75_nop(&word);
		error = nvrtx_sm75_emit(lowering, &word, NVRTX_SM75_FIXED);
		if (error != 0)
			return error;
	}

	/* The instructions of each block in an order that hides latency. */
	reason = NULL;
	instruction = 0U;
	error = nvrtx_sm75_reorder(lowering->binary->code, lowering->count, lowering->sm, &instruction, &reason);
	if (error != 0) {
		lowering->index = instruction;
		error = nvrtx_sm75_refuse(lowering, EINVAL, reason);
		return error;
	}

	/* The dependences of every instruction. */
	error = nvrtx_sm75_schedule(lowering->binary->code, lowering->count, lowering->sm, &instruction, &reason);
	if (error != 0) {
		lowering->index = instruction;
		error = nvrtx_sm75_refuse(lowering, EINVAL, reason);
		return error;
	}

	/* A graphics program's header. */
	nvrtx_sm75_header(lowering);

	/*
	 * The size and the registers (at least four, the hardware's two
	 * included).
	 *
	 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/api.rs
	 * (ShaderBin::new: num_gprs, at least 4) and
	 * src/nouveau/compiler/nak/sm70.rs (ShaderModel70::hw_reserved_gprs).
	 */
	lowering->binary->code_words = lowering->count * NVRTX_SM75_INSTRUCTION_WORDS;
	lowering->binary->gpr_count = lowering->highest + NVRTX_SM75_GPR_RESERVED;
	if (lowering->binary->gpr_count < NVRTX_SM75_GPR_MINIMUM)
		lowering->binary->gpr_count = NVRTX_SM75_GPR_MINIMUM;

	/* Succeeded: the program is complete. */
	return 0;
}
