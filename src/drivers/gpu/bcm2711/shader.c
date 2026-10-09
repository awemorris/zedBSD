/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native graphics lowering reuses the device-independent Zlib frontend without linking a Gen12 backend. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/shader-private.h"
#include "drivers/gpu/i915/compiler/compiler.h"

/* Nine constant levels match the shared frontend's eight aggregate edges followed by one scalar leaf. */
#define GRAPHICS_CONSTANT_LEVELS 9U

/* The shared frontend admits this finite ID space; private metadata occupies one heap byte per ID. */
#define GRAPHICS_MODULE_IDS 65536U

static int preflight_module(const uint32_t *words, size_t word_count, enum bcm2711_shader_stage stage, struct bcm2711_shader_diagnostic *diagnostic);
static int preflight_declaration(const uint32_t *instruction, uint32_t count, uint8_t *levels, uint32_t bound, uint32_t model, const char **reason);
static int compile_program(struct bcm2711_shader_compiler *compiler);

/*
 * Compiles one SPIR-V entry point into a fully owned native graphics program.
 */
int
bcm2711_shader_compile(
	const uint32_t *words,
	size_t word_count,
	enum bcm2711_shader_stage stage,
	const struct bcm2711_shader_key *key,
	struct bcm2711_shader_binary **binary,
	struct bcm2711_shader_diagnostic *diagnostic)
{
	struct bcm2711_shader_compiler compiler;
	struct i915_shader_ir *ir;
	struct i915_compile_diagnostic parse_diagnostic;
	enum i915_shader_stage parse_stage;
	int error;

	/* A refused compile never publishes a partial program or leaves an old output pointer live. */
	if (binary == NULL)
		return EINVAL;
	*binary = NULL;
	if (diagnostic != NULL)
		kern_memset(diagnostic, 0, sizeof(*diagnostic));
	if (words == NULL || key == NULL || stage < BCM2711_SHADER_COORDINATE ||
	    stage > BCM2711_SHADER_FRAGMENT || word_count > 1048576U)
		return EINVAL;
	if (key->swap_red_blue > 1 || key->premultiplied_blend > 1)
		return EINVAL;

	/* Bounds declaration recursion and fixes the graphics execution model before the shared parser allocates or expands constants. */
	error = preflight_module(words, word_count, stage, diagnostic);
	if (error != 0)
		return error;

	/* Both native vertex variants parse the same source stage; their VPM epilogues differ. */
	parse_stage = I915_STAGE_VERTEX;
	if (stage == BCM2711_SHADER_FRAGMENT)
		parse_stage = I915_STAGE_FRAGMENT;
	parse_diagnostic.reason = NULL;
	ir = NULL;
	error = drv_i915_shader_parse(words, word_count, parse_stage, &ir, &parse_diagnostic);
	if (error != 0) {
		if (diagnostic != NULL) {
			diagnostic->instruction = parse_diagnostic.word_offset;
			diagnostic->reason = parse_diagnostic.reason;
		}

		/* A parse refusal has no IR ownership to transfer into native lowering. */
		return error;
	}

	/* Compiler state owns no device reference and is unpublished until native lowering finishes. */
	kern_memset(&compiler, 0, sizeof(compiler));
	compiler.ir = ir;
	compiler.key = key;
	compiler.diagnostic = diagnostic;

	/* Allocates the public program independently so every later failure has a single complete unwind. */
	compiler.binary = kern_calloc(1, sizeof(*compiler.binary));
	if (compiler.binary == NULL) {
		drv_i915_shader_ir_free(ir);
		return ENOMEM;
	}

	/* The stage remains immutable throughout both interface validation and native output construction. */
	compiler.binary->stage = stage;

	/* One builder owns the complete unpublished prefix and supplies one checked outcome for common cleanup. */
	error = compile_program(&compiler);

	/* Source IR and allocator metadata retire before the immutable native program becomes visible. */
	kern_free(compiler.values);
	drv_i915_shader_ir_free(ir);

	/* A failed native prefix can never escape as a usable shader binary. */
	if (error != 0) {
		bcm2711_shader_binary_free(compiler.binary);
		return error;
	}

	/* Publication transfers all completed code and uniform storage to the caller at once. */
	*binary = compiler.binary;

	/* The caller owns a complete native program, including all draw-time uniform identities. */
	return 0;
}

/*
 * Releases a completed or partially constructed native program.
 */
void
bcm2711_shader_binary_free(
	struct bcm2711_shader_binary *binary)
{
	/* A failed allocation leaves no program storage to release. */
	if (binary == NULL)
		return;

	/* Only compiler-owned host storage is released here; uploaded GPU copies have separate owners. */
	kern_free(binary->code);
	kern_free(binary->uniforms);
	kern_free(binary);
}

/*
 * Appends one completely checked native word within the prevalidated program capacity.
 */
int
bcm2711_shader_append(
	struct bcm2711_shader_compiler *compiler,
	const struct bcm2711_qpu_instruction *instruction)
{
	uint64_t word;
	int error;

	/* Capacity refusal cannot expose a truncated successful program. */
	if (compiler->binary->code_count == compiler->code_capacity)
		return E2BIG;

	/* The encoder validates shared port, signal and flag fields before returning an atomic word. */
	error = bcm2711_qpu_encode(instruction, &word);
	if (error != 0)
		return error;
	compiler->binary->code[compiler->binary->code_count] = word;
	compiler->binary->code_count++;

	/* This program prefix contains only fully encoded native instructions. */
	return 0;
}

/*
 * Inserts conservative idle instructions without altering lane flags or physical registers.
 */
int
bcm2711_shader_nops(
	struct bcm2711_shader_compiler *compiler,
	uint32_t count)
{
	struct bcm2711_qpu_instruction instruction;
	uint32_t index;
	int error;

	/* A zero-filled semantic instruction leaves both native ALUs idle and sends no signal. */
	kern_memset(&instruction, 0, sizeof(instruction));

	/* Delay slots are explicit program words, so they remain inside the uploaded execution interval. */
	for (index = 0; index < count; index++) {
		error = bcm2711_shader_append(compiler, &instruction);
		if (error != 0)
			return error;
	}

	/* The conservative gap never introduces an implicit register or uniform dependency. */
	return 0;
}

/*
 * Emits one semantic operation followed by conservative register and SFU dependency spacing.
 */
int
bcm2711_shader_operation(
	struct bcm2711_shader_compiler *compiler,
	enum bcm2711_qpu_operation operation,
	uint32_t destination,
	uint32_t peripheral,
	struct bcm2711_qpu_source left,
	struct bcm2711_qpu_source right,
	uint32_t predicate,
	uint32_t flags)
{
	struct bcm2711_qpu_instruction instruction;
	int error;

	/* Exactly one ALU is active; no independent instruction is paired across a hidden dependency. */
	kern_memset(&instruction, 0, sizeof(instruction));
	instruction.operation = operation;
	instruction.destination.number = destination;
	instruction.destination.peripheral = peripheral;
	instruction.left = left;
	instruction.right = right;
	instruction.predicate = predicate;
	instruction.push_flags = flags;
	error = bcm2711_shader_append(compiler, &instruction);
	if (error != 0)
		return error;

	/* Two explicit idle words avoid immediate register and three-cycle SFU reuse hazards. */
	error = bcm2711_shader_nops(compiler, 2);
	if (error != 0)
		return error;

	/* The destination is ready for the following source operation under the conservative schedule. */
	return 0;
}

/*
 * Records one actual uniform FIFO consumption in the immutable program interface.
 */
int
bcm2711_shader_uniform_append(
	struct bcm2711_shader_compiler *compiler,
	const struct bcm2711_shader_uniform *uniform)
{
	/* Every hardware consumption must fit the validated descriptor capacity. */
	if (compiler->binary->uniform_count == compiler->uniform_capacity)
		return E2BIG;
	compiler->binary->uniforms[compiler->binary->uniform_count] = *uniform;
	compiler->binary->uniform_count++;

	/* The runtime must supply this word at exactly this position in the stage's FIFO stream. */
	return 0;
}

/*
 * Loads one uniform directly into a physical register without clobbering the varying coefficient accumulator.
 */
int
bcm2711_shader_uniform_load(
	struct bcm2711_shader_compiler *compiler,
	const struct bcm2711_shader_uniform *uniform,
	uint32_t destination)
{
	int error;

	/* Metadata precedes its matching native transaction and remains private if that transaction fails. */
	error = bcm2711_shader_uniform_append(compiler, uniform);
	if (error != 0)
		return error;

	/* Addressed uniform loads use the 4.2 direct register signal and explicit latency slots. */
	error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_UNIFORM_REGISTER, destination, 2);
	if (error != 0)
		return error;

	/* The exact runtime word is now available without a secondary accumulator move. */
	return 0;
}

/*
 * Emits a single native transaction with a checked physical destination and explicit delay count.
 */
int
bcm2711_shader_signal(
	struct bcm2711_shader_compiler *compiler,
	enum bcm2711_qpu_signal signal,
	uint32_t destination,
	uint32_t gaps)
{
	struct bcm2711_qpu_instruction instruction;
	int error;

	/* An idle ALU instruction allows signals to use their complete destination field without flag aliasing. */
	kern_memset(&instruction, 0, sizeof(instruction));
	instruction.signal = signal;
	instruction.signal_destination.number = destination;
	error = bcm2711_shader_append(compiler, &instruction);
	if (error != 0)
		return error;

	/* A caller choosing zero gaps owns an explicit compound switch delay sequence. */
	error = bcm2711_shader_nops(compiler, gaps);
	if (error != 0)
		return error;

	/* The signal and all requested delay words are inside the program's exact code extent. */
	return 0;
}

/*
 * Allocates one free two-thread physical expression register.
 */
int
bcm2711_shader_register(
	struct bcm2711_shader_compiler *compiler,
	uint32_t *number)
{
	uint32_t index;

	/* Payload and materialization scratch registers are permanently excluded from expression allocation. */
	for (index = BCM2711_SHADER_FIRST_REGISTER; index < BCM2711_SHADER_REGISTER_END; index++) {
		if (compiler->registers[index] == 0) {
			compiler->registers[index] = 1;
			*number = index;
			if (compiler->binary->registers_used <= index)
				compiler->binary->registers_used = index + 1;
			return 0;
		}
	}

	/* Register spilling has no implementation and must never silently replace a live scalar. */
	return E2BIG;
}

/*
 * Materializes one checked scalar source while preserving exact constant bits.
 */
int
bcm2711_shader_operand(
	struct bcm2711_shader_compiler *compiler,
	uint32_t value,
	uint32_t scratch,
	struct bcm2711_qpu_source *source)
{
	struct bcm2711_shader_value *scalar;
	struct bcm2711_shader_uniform uniform;
	uint32_t encoded;
	int error;

	/* Liveness validation must have established the source before the emitter addresses it. */
	if (value >= compiler->ir->value_count)
		return EINVAL;
	scalar = &compiler->values[value];

	/* Live physical values need no hidden uniform transaction or temporary storage. */
	if (scalar->constant == 0) {
		if (scalar->register_live == 0)
			return EINVAL;
		*source = bcm2711_shader_rf(scalar->number);
		return 0;
	}

	/* Only exact native small constants may bypass full-width uniform materialization. */
	error = bcm2711_qpu_small_constant(scalar->number, &encoded);
	if (error == 0) {
		*source = bcm2711_shader_bits(scalar->number);
		return 0;
	}

	/* A malformed native conversion is distinct from an exact full-width constant that needs a uniform. */
	if (error != ENOTSUP)
		return error;

	/* Full-width constants use caller-selected scratch registers that cannot overwrite an expression value. */
	kern_memset(&uniform, 0, sizeof(uniform));
	uniform.kind = BCM2711_SHADER_CONSTANT;
	uniform.bits = scalar->number;
	error = bcm2711_shader_uniform_load(compiler, &uniform, scratch);
	if (error != 0)
		return error;
	*source = bcm2711_shader_rf(scratch);

	/* Every bit of the source is preserved, including signed zero and NaN payloads. */
	return 0;
}

/*
 * Constructs one native physical-register source without hiding any hardware transaction.
 */
struct bcm2711_qpu_source
bcm2711_shader_rf(
	uint32_t number)
{
	struct bcm2711_qpu_source source;

	/* The register number is checked by the encoder at its eventual consuming instruction. */
	source.kind = BCM2711_QPU_REGISTER;
	source.number = number;

	/* A source construction itself performs no machine operation. */
	return source;
}

/*
 * Constructs one exact native small-constant source without approximating its bits.
 */
struct bcm2711_qpu_source
bcm2711_shader_bits(
	uint32_t bits)
{
	struct bcm2711_qpu_source source;

	/* The full bit pattern remains available for exact small-constant validation by the encoder. */
	source.kind = BCM2711_QPU_CONSTANT;
	source.number = bits;

	/* No floating-point calculation is performed by the kernel compiler. */
	return source;
}

/*
 * Records one native lowering refusal at the source instruction currently being compiled.
 */
int
bcm2711_shader_fail(
	struct bcm2711_shader_compiler *compiler,
	int error,
	const char *reason)
{
	/* Diagnostics never point into transient frontend or compiler storage. */
	if (compiler->diagnostic != NULL) {
		compiler->diagnostic->instruction = compiler->instruction;
		compiler->diagnostic->reason = reason;
	}

	/* The caller retains the exact checked failure for pipeline creation. */
	return error;
}

/* Bounds graphics declaration graphs iteratively before the shared frontend can recurse through their constants. */
static int
preflight_module(
	const uint32_t *words,
	size_t word_count,
	enum bcm2711_shader_stage stage,
	struct bcm2711_shader_diagnostic *diagnostic)
{
	uint8_t *levels;
	const uint32_t *instruction;
	const char *reason;
	size_t offset;
	uint32_t bound;
	uint32_t count;
	uint32_t opcode;
	uint32_t model;
	int in_function;
	int error;

	/* A complete SPIR-V header must precede any ID-indexed metadata access. */
	if (word_count < 5U)
		return EINVAL;
	if (words[0] != 0x07230203U)
		return EINVAL;

	/* The same finite ID range bounds every earlier-constant lookup. */
	bound = words[3];
	if (bound == 0 || bound > GRAPHICS_MODULE_IDS)
		return EINVAL;

	/* Unpublished heap metadata keeps the declared ID space off the finite kernel stack. */
	levels = kern_calloc(bound, sizeof(*levels));
	if (levels == NULL)
		return ENOMEM;

	/* Coordinate and vertex binaries share the vertex execution model; only the fragment binary admits Fragment. */
	model = 0U;
	if (stage == BCM2711_SHADER_FRAGMENT)
		model = 4U;

	/* Walks complete instruction frames with the same module/function boundary as the shared declaration pass. */
	error = 0;
	reason = "graphics module has an incomplete instruction";
	in_function = 0;
	offset = 5U;
	while (offset < word_count) {
		/* Framing prevents every declaration helper from borrowing words outside this exact module. */
		instruction = words + offset;
		count = instruction[0] >> 16;
		opcode = instruction[0] & 0xffffU;
		if (count == 0 || count > word_count - offset) {
			error = EINVAL;
			break;
		}

		/* Function bodies cannot create module-level constant owners in the shared frontend. */
		if (opcode == 54U)
			in_function = 1;
		if (in_function != 0) {
			if (opcode == 56U)
				in_function = 0;
			offset += count;
			continue;
		}

		/* Each module declaration either preserves the finite native graph or refuses before parser recursion. */
		error = preflight_declaration(instruction, count, levels, bound, model, &reason);
		if (error != 0)
			break;
		offset += count;
	}

	/* No declaration metadata is retained by a source IR, native program, or diagnostic string. */
	kern_free(levels);

	/* A refusal names the exact source word and never publishes a partial program. */
	if (error != 0) {
		if (diagnostic != NULL) {
			diagnostic->instruction = (uint32_t)offset;
			diagnostic->reason = reason;
		}

		/* The caller receives the exact framing or declaration refusal after temporary metadata has retired. */
		return error;
	}

	/* Succeeded: graphics-only declarations have a finite constant expansion depth. */
	return 0;
}

/* Admits immutable earlier-constant edges and the requested graphics model without interpreting native instructions. */
static int
preflight_declaration(
	const uint32_t *instruction,
	uint32_t count,
	uint8_t *levels,
	uint32_t bound,
	uint32_t model,
	const char **reason)
{
	uint32_t opcode;
	uint32_t identity;
	uint32_t constituent;
	uint32_t index;
	uint32_t depth;
	uint32_t child_depth;

	/* Standard SPIR-V opcode numbers identify declarations; unselected semantics remain the shared frontend's responsibility. */
	opcode = instruction[0] & 0xffffU;
	if (opcode == 15U) {
		/* An entry point must preserve the requested vertex or fragment model before the shared parser can overwrite its stage. */
		*reason = "entry point does not match the native graphics stage";
		if (count < 3U)
			return EINVAL;
		if (instruction[1] != model)
			return ENOTSUP;

		/* Succeeded: this entry cannot enable a compute-only declaration or recursive shared-memory layout. */
		return 0;
	}

	/* Native graphics has no Workgroup storage; refuse it before the shared compute layout helper can run. */
	if (opcode == 59U) {
		*reason = "Workgroup storage is outside native graphics";
		if (count < 4U)
			return EINVAL;
		if (instruction[3] == 4U)
			return ENOTSUP;

		/* Succeeded: ordinary graphics variable validation remains with the shared frontend. */
		return 0;
	}

	/* Only the four constant declarations supported by the shared frontend can create recursive constant owners. */
	if (opcode < 41U || opcode > 44U)
		return 0;

	/* Each constant ID is declared once, preserving the earlier-reference graph even if another declaration is malformed. */
	*reason = "constant has an invalid or repeated result ID";
	if (count < 3U)
		return EINVAL;
	identity = instruction[2];
	if (identity == 0 || identity >= bound)
		return EINVAL;
	if (levels[identity] != 0)
		return EINVAL;

	/* Scalar and Boolean leaves consume one expansion level and borrow no recursive constituent. */
	depth = 1U;
	if (opcode == 44U) {
		/* Composite edges refer only to already declared immutable constants, with the shared frontend's sixteen-member limit. */
		*reason = "constant composite exceeds native expansion limits";
		if (count < 4U || count > 19U)
			return ENOTSUP;
		for (index = 3U; index < count; index++) {
			/* A forward, absent, or self edge cannot enter the constant recursion graph. */
			constituent = instruction[index];
			if (constituent == 0 || constituent >= bound)
				return EINVAL;
			child_depth = levels[constituent];
			if (child_depth == 0)
				return EINVAL;

			/* One more aggregate edge must fit the same finite depth as the shared scalar type profile. */
			if (child_depth >= GRAPHICS_CONSTANT_LEVELS)
				return ENOTSUP;
			if (child_depth + 1U > depth)
				depth = child_depth + 1U;
		}
	}

	/* The immutable depth belongs to the preflight pass only and is discarded before parser allocation. */
	levels[identity] = (uint8_t)depth;

	/* Succeeded: every constant expansion path stays within the finite graphics profile. */
	return 0;
}

/* Builds a complete private program while leaving every allocation reachable through the caller's common unwind. */
static int
compile_program(
	struct bcm2711_shader_compiler *compiler)
{
	int error;

	/* Validation establishes bounded code/uniform capacities and exact scalar lifetimes. */
	error = bcm2711_shader_analyze(compiler);
	if (error != 0)
		return error;

	/* Native words remain private until every instruction and epilogue has been encoded. */
	compiler->binary->code = kern_calloc(compiler->code_capacity, sizeof(*compiler->binary->code));
	if (compiler->binary->code == NULL)
		return ENOMEM;

	/* Uniform descriptors follow actual hardware consumption order, rather than source declaration order. */
	compiler->binary->uniforms = kern_calloc(compiler->uniform_capacity, sizeof(*compiler->binary->uniforms));
	if (compiler->binary->uniforms == NULL)
		return ENOMEM;

	/* A native failure leaves every completed prefix allocation visible to the outer common cleanup. */
	error = bcm2711_shader_lower(compiler);
	if (error != 0)
		return error;

	/* All native instructions and stage epilogues fit their validated owned storage. */
	return 0;
}
