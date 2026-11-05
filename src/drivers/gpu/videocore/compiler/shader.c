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

#include "drivers/gpu/bcm2711/compiler/shader-private.h"
#include "drivers/gpu/compiler/optimize.h"
#include "drivers/gpu/compiler/spirv.h"

/* Nine constant levels match the shared frontend's eight aggregate edges followed by one scalar leaf. */
#define GRAPHICS_CONSTANT_LEVELS 9U

/* The shared frontend admits this finite ID space; private metadata occupies one heap byte per ID. */
#define GRAPHICS_MODULE_IDS 65536U

static int preflight_declaration(const uint32_t *instruction, uint32_t count, uint8_t *levels, uint32_t bound, uint32_t model, const char **reason);
static int compile_program(struct bcm2711_shader_compiler *compiler);
static void optimize_options(enum bcm2711_shader_stage stage, const struct bcm2711_shader_key *key, struct drv_gpu_shader_optimize_options *options);
static int compile_stage(const struct drv_gpu_shader_ir *ir, const struct bcm2711_shader_key *key, enum bcm2711_shader_stage stage, struct bcm2711_shader_diagnostic *diagnostic, struct bcm2711_shader_binary **binary);
static int compile_threads(const struct drv_gpu_shader_ir *ir, const struct bcm2711_shader_key *key, enum bcm2711_shader_stage stage, uint32_t threads, struct bcm2711_shader_diagnostic *diagnostic, struct bcm2711_shader_binary **binary);
static int encode_register(const struct bcm2711_shader_compiler *compiler, uint32_t *number);
static uint32_t operation_gaps(enum bcm2711_qpu_operation operation, uint32_t destination, uint32_t peripheral);
static int record_op(struct bcm2711_shader_compiler *compiler, const struct bcm2711_qpu_instruction *instruction, enum bcm2711_shader_switch role);
static uint32_t consumes_uniform(const struct bcm2711_qpu_instruction *instruction);

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
	struct bcm2711_shader_binary *created;
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_shader_ir optimized;
	struct drv_gpu_shader_optimize_options options;
	struct drv_gpu_compile_diagnostic parse_diagnostic;
	enum drv_gpu_shader_stage parse_stage;
	int error;

	/* A refused compile never publishes a partial program or leaves an old output pointer live. */
	if (binary == NULL)
		return EINVAL;
	*binary = NULL;
	if (diagnostic != NULL)
		kern_memset(diagnostic, 0, sizeof(*diagnostic));
	if (words == NULL ||
	    key == NULL ||
	    stage < BCM2711_SHADER_COORDINATE ||
	    stage > BCM2711_SHADER_FRAGMENT ||
	    word_count > 1048576U)
		return EINVAL;
	if (key->swap_red_blue > 1)
		return EINVAL;

	/* Bounds declaration recursion and fixes the graphics execution model before the shared parser allocates or expands constants. */
	error = bcm2711_shader_preflight(words, word_count, stage, diagnostic);
	if (error != 0)
		return error;

	/* Both native vertex variants parse the same source stage; their VPM epilogues differ. */
	parse_stage = DRV_GPU_STAGE_VERTEX;
	if (stage == BCM2711_SHADER_FRAGMENT)
		parse_stage = DRV_GPU_STAGE_FRAGMENT;
	parse_diagnostic.reason = NULL;
	ir = NULL;
	error = drv_gpu_shader_parse(words, word_count, parse_stage, &ir, &parse_diagnostic);
	if (error != 0) {
		if (diagnostic != NULL) {
			diagnostic->instruction = parse_diagnostic.word_offset;
			diagnostic->reason = parse_diagnostic.reason;
		}

		/* A parse refusal has no IR ownership to transfer into native lowering. */
		return error;
	}

	/* Removes repeated and unread work from a private copy of the source program. */
	optimize_options(stage, key, &options);
	error = drv_gpu_shader_optimize(ir, &options, &optimized);
	if (error != 0) {
		drv_gpu_shader_ir_free(ir);
		return error;
	}

	/* Lowers the rewritten program, or the source program when the optimizer left it alone. */
	created = NULL;
	if (optimized.instructions != NULL) {
		error = compile_stage(&optimized, key, stage, diagnostic, &created);
		if (error == 0)
			created->optimized = 1;
	} else {
		error = compile_stage(ir, key, stage, diagnostic, &created);
	}

	/*
	 * A rewritten program the native compiler refuses for any reason but
	 * memory is lowered from its source, which keeps the source program's
	 * own outcome and diagnostic.
	 */
	if (optimized.instructions != NULL && error != 0 && error != ENOMEM) {
		if (diagnostic != NULL)
			kern_memset(diagnostic, 0, sizeof(*diagnostic));
		error = compile_stage(ir, key, stage, diagnostic, &created);
	}

	/* Source IR and its rewritten instructions retire before the immutable native program becomes visible. */
	kern_free(optimized.instructions);
	drv_gpu_shader_ir_free(ir);

	/* A failed native prefix can never escape as a usable shader binary. */
	if (error != 0)
		return error;

	/* Publication transfers all completed code and uniform storage to the caller at once. */
	*binary = created;

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

	/* Succeeded: the retired program owns no code or uniform metadata. */
	return;
}

/*
 * Records one checked operation or signal within the prevalidated program capacity.
 *
 * Native words are chosen after the whole program is recorded, so the
 * scheduler can pair operations and fill delays; an encoding refusal is
 * still reported here, at the source instruction that caused it.
 */
int
bcm2711_shader_append(
	struct bcm2711_shader_compiler *compiler,
	const struct bcm2711_qpu_instruction *instruction)
{
	int error;

	/* An ordinary record has no thread switch role. */
	error = record_op(compiler, instruction, BCM2711_SHADER_SWITCH_NONE);
	if (error != 0)
		return error;

	/* Succeeded: the program's record list ends with this operation. */
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
 * Emits one semantic operation followed by the idle words its result needs before a consumer.
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

	/* Special-function, VPM, wait and peripheral transactions keep two idle words; a plain result needs none. */
	error = bcm2711_shader_nops(compiler, operation_gaps(operation, destination, peripheral));
	if (error != 0)
		return error;

	/* The destination is ready for the following source operation. */
	return 0;
}

/*
 * Describes the uniform the next consuming transaction reads from the stage's FIFO stream.
 */
int
bcm2711_shader_uniform_append(
	struct bcm2711_shader_compiler *compiler,
	const struct bcm2711_shader_uniform *uniform)
{
	/* One description waits for its transaction; a second one would lose the first's position. */
	if (compiler->has_pending_uniform != 0)
		return EINVAL;
	compiler->pending_uniform = *uniform;
	compiler->has_pending_uniform = 1;

	/* The next consuming record takes the word; its FIFO position follows that record's native word. */
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

	/* Addressed uniform loads use the 4.2 direct register signal; the next instruction can read the word. */
	error = bcm2711_shader_signal(compiler, BCM2711_QPU_SIGNAL_UNIFORM_REGISTER, destination, 0);
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
 * Allocates one free physical expression register.
 */
int
bcm2711_shader_register(
	struct bcm2711_shader_compiler *compiler,
	uint32_t *number)
{
	uint32_t index;
	uint32_t chosen;

	/* Payload and materialization scratch registers are permanently excluded; the longest-released free register wins. */
	chosen = BCM2711_SHADER_ABSENT;
	for (index = BCM2711_SHADER_FIRST_REGISTER; index < compiler->register_end; index++) {
		if (compiler->registers[index] != 0)
			continue;
		if (chosen == BCM2711_SHADER_ABSENT || compiler->released[index] < compiler->released[chosen])
			chosen = index;
	}

	/* A register holding a cached constant no current instruction reads is given up, the one read longest ago. */
	if (chosen == BCM2711_SHADER_ABSENT) {
		for (index = BCM2711_SHADER_FIRST_REGISTER; index < compiler->register_end; index++) {
			if (compiler->constant_held[index] == 0 || compiler->constant_touched[index] != 0)
				continue;
			if (chosen == BCM2711_SHADER_ABSENT || compiler->constant_clock[index] < compiler->constant_clock[chosen])
				chosen = index;
		}

		/* The given-up register is free again. */
		if (chosen != BCM2711_SHADER_ABSENT) {
			compiler->constant_held[chosen] = 0;
			bcm2711_shader_release(compiler, chosen);
		}
	}

	/* Register spilling has no implementation and must never silently replace a live scalar. */
	if (chosen == BCM2711_SHADER_ABSENT)
		return E2BIG;

	/* The register now holds one live value. */
	compiler->registers[chosen] = 1;
	*number = chosen;
	if (compiler->binary->registers_used <= chosen)
		compiler->binary->registers_used = chosen + 1;

	/* Succeeded: the caller owns the register until it releases the value. */
	return 0;
}

/*
 * Releases one expression register whose value has no later reader.
 */
void
bcm2711_shader_release(
	struct bcm2711_shader_compiler *compiler,
	uint32_t number)
{
	/* The release time sends the register to the back of the allocation order. */
	compiler->release_clock++;
	compiler->registers[number] = 0;
	compiler->released[number] = compiler->release_clock;

	/* Succeeded: a later value may take the register. */
	return;
}

/*
 * Chooses a register for a short-lived scratch value.
 *
 * A free expression register is borrowed while enough others stay free, so
 * consecutive materializations do not chain through one fixed scratch
 * register; otherwise the caller's preferred fixed scratch register is used.
 */
uint32_t
bcm2711_shader_scratch(
	struct bcm2711_shader_compiler *compiler,
	uint32_t preferred)
{
	uint32_t index;
	uint32_t free_count;
	uint32_t number;
	int error;

	/* Counts the free expression registers. */
	free_count = 0;
	for (index = BCM2711_SHADER_FIRST_REGISTER; index < compiler->register_end; index++) {
		if (compiler->registers[index] == 0)
			free_count++;
	}

	/* A program short of registers keeps them for its values and uses the fixed scratch register. */
	if (free_count <= BCM2711_SHADER_SCRATCH_RESERVE)
		return preferred;

	/* Borrows the longest-released free register until the current source instruction is lowered. */
	error = bcm2711_shader_register(compiler, &number);
	if (error != 0)
		return preferred;
	compiler->borrowed[number] = 1;

	/* Succeeded: the register holds the scratch value until the borrowed registers are released. */
	return number;
}

/*
 * Places a full-width constant in a register, loading it from the uniform stream only when no register holds it.
 *
 * While enough registers are free, a loaded constant stays in its register
 * for later reads; the register is given up when a value needs it.
 * Otherwise the constant goes to a scratch register for this read only.
 */
int
bcm2711_shader_constant(
	struct bcm2711_shader_compiler *compiler,
	uint32_t bits,
	uint32_t preferred,
	uint32_t *number)
{
	struct bcm2711_shader_uniform uniform;
	uint32_t index;
	uint32_t free_count;
	uint32_t chosen;
	int error;

	/* A register already holding the bits serves this read too. */
	for (index = BCM2711_SHADER_FIRST_REGISTER; index < compiler->register_end; index++) {
		if (compiler->constant_held[index] != 0 && compiler->constant_bits[index] == bits) {
			compiler->release_clock++;
			compiler->constant_clock[index] = compiler->release_clock;
			compiler->constant_touched[index] = 1;
			*number = index;
			return 0;
		}
	}

	/* Counts the free expression registers. */
	free_count = 0;
	for (index = BCM2711_SHADER_FIRST_REGISTER; index < compiler->register_end; index++) {
		if (compiler->registers[index] == 0)
			free_count++;
	}

	/* A program with registers to spare keeps the constant; otherwise it takes a scratch register. */
	chosen = BCM2711_SHADER_ABSENT;
	if (free_count > BCM2711_SHADER_CONSTANT_RESERVE) {
		error = bcm2711_shader_register(compiler, &chosen);
		if (error != 0)
			chosen = BCM2711_SHADER_ABSENT;
	}

	/* Without a kept register the constant takes a scratch one. */
	if (chosen == BCM2711_SHADER_ABSENT)
		chosen = bcm2711_shader_scratch(compiler, preferred);

	/* The constant's exact bits come from the uniform stream. */
	kern_memset(&uniform, 0, sizeof(uniform));
	uniform.kind = BCM2711_SHADER_CONSTANT;
	uniform.bits = bits;
	error = bcm2711_shader_uniform_load(compiler, &uniform, chosen);
	if (error != 0)
		return error;

	/* A kept constant is marked for later reads and protected for this instruction. */
	if (chosen >= BCM2711_SHADER_FIRST_REGISTER &&
	    chosen < compiler->register_end &&
	    compiler->borrowed[chosen] == 0 &&
	    compiler->registers[chosen] != 0) {
		compiler->release_clock++;
		compiler->constant_held[chosen] = 1;
		compiler->constant_bits[chosen] = bits;
		compiler->constant_clock[chosen] = compiler->release_clock;
		compiler->constant_touched[chosen] = 1;
	}

	/* The caller reads the constant from here. */
	*number = chosen;

	/* Succeeded: the register holds the constant's exact bits. */
	return 0;
}

/*
 * Releases every expression register borrowed as a scratch register.
 */
void
bcm2711_shader_release_scratch(
	struct bcm2711_shader_compiler *compiler)
{
	uint32_t index;

	/* Each borrowed register returns to the allocation order; cached constants may be given up again. */
	for (index = 0; index < 64; index++) {
		if (compiler->borrowed[index] != 0) {
			compiler->borrowed[index] = 0;
			bcm2711_shader_release(compiler, index);
		}

		/* A finished instruction no longer protects the constants it read. */
		compiler->constant_touched[index] = 0;
	}

	/* Succeeded: no scratch value outlives the instruction that needed it. */
	return;
}

/*
 * Records one thread switch with the delay words its role needs when the program is not scheduled.
 */
int
bcm2711_shader_switch(
	struct bcm2711_shader_compiler *compiler,
	enum bcm2711_shader_switch role)
{
	struct bcm2711_qpu_instruction instruction;
	uint32_t gaps;
	int error;

	/* The switch is a signal on an otherwise idle word. */
	kern_memset(&instruction, 0, sizeof(instruction));
	instruction.signal = BCM2711_QPU_SIGNAL_SWITCH;
	error = record_op(compiler, &instruction, role);
	if (error != 0)
		return error;

	/*
	 * A switch's two delay words follow it; the last switch is marked by a
	 * second switch in the first delay word, and tile writes start after the
	 * second switch's own delay words.
	 */
	gaps = 2;
	if (role == BCM2711_SHADER_SWITCH_LAST_FIRST)
		gaps = 0;
	error = bcm2711_shader_nops(compiler, gaps);
	if (error != 0)
		return error;

	/* Succeeded: the switch and its delay words are recorded. */
	return 0;
}

/*
 * Turns the recorded program into native words and its uniform stream.
 *
 * A fragment program is scheduled: operations are paired and delays filled.
 * A program the scheduler refuses, and every vertex program, keeps one
 * recorded operation per word in source order.  Running out of memory while
 * scheduling fails the compile like any other allocation failure.
 */
int
bcm2711_shader_emit(
	struct bcm2711_shader_compiler *compiler)
{
	int error;

	/* Every described uniform must have been consumed by its transaction. */
	if (compiler->has_pending_uniform != 0)
		return EINVAL;

	/* Fragment programs run once per pixel and are worth scheduling; running out of memory fails the compile. */
	if (compiler->binary->stage == BCM2711_SHADER_FRAGMENT) {
		error = bcm2711_shader_schedule(compiler);
		if (error == ENOMEM)
			return error;
		if (error == 0) {
			compiler->binary->scheduled = 1;
			return 0;
		}
	}

	/* The source-order words keep every conservative delay. */
	compiler->binary->scheduled = 0;
	error = bcm2711_shader_emit_in_order(compiler);
	if (error != 0)
		return error;

	/* Succeeded: the program's words and uniform stream are complete. */
	return 0;
}

/*
 * Encodes one recorded operation per native word, in source order.
 */
int
bcm2711_shader_emit_in_order(
	struct bcm2711_shader_compiler *compiler)
{
	struct bcm2711_shader_op *op;
	uint64_t word;
	uint32_t index;
	int error;

	/* A scheduling attempt may have written words and uniforms; they are replaced. */
	compiler->binary->code_count = 0;
	compiler->binary->uniform_count = 0;

	/* Each record becomes one word, and its uniform takes the next stream position. */
	for (index = 0; index < compiler->op_count; index++) {
		op = &compiler->ops[index];
		error = bcm2711_qpu_encode(&op->instruction, &word);
		if (error != 0)
			return error;
		if (compiler->binary->code_count == compiler->code_capacity)
			return E2BIG;
		compiler->binary->code[compiler->binary->code_count] = word;
		compiler->binary->code_count++;

		/* The runtime supplies the uniform at exactly this position in the stage's FIFO stream. */
		if (op->has_uniform != 0) {
			if (compiler->binary->uniform_count == compiler->uniform_capacity)
				return E2BIG;
			compiler->binary->uniforms[compiler->binary->uniform_count] = op->uniform;
			compiler->binary->uniform_count++;
		}
	}

	/* Succeeded: the words follow the source order with every conservative delay. */
	return 0;
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
	uint32_t encoded;
	uint32_t register_number;
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

	/* Full-width constants use a cached, borrowed or caller-selected scratch register that cannot overwrite an expression value. */
	error = bcm2711_shader_constant(compiler, scalar->number, scratch, &register_number);
	if (error != 0)
		return error;
	*source = bcm2711_shader_rf(register_number);

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

	/* A recorded refusal preserves its exact checked failure for pipeline creation. */
	if (error != 0)
		return error;

	/* Succeeded: an optional diagnostic with no refusal preserves successful status. */
	return 0;
}

/*
 * Bounds graphics declarations before any private caller enters the shared recursive frontend.
 */
int
bcm2711_shader_preflight(
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

	/* Every native frontend user requires a real bounded graphics module before reading its header. */
	if (words == NULL || word_count > 1048576U)
		return EINVAL;
	if (stage < BCM2711_SHADER_COORDINATE || stage > BCM2711_SHADER_FRAGMENT)
		return EINVAL;

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

	/* Recorded operations wait here until the whole program is known. */
	compiler->ops = kern_calloc(compiler->code_capacity, sizeof(*compiler->ops));
	if (compiler->ops == NULL)
		return ENOMEM;

	/* Uniform descriptors follow actual hardware consumption order, rather than source declaration order. */
	compiler->binary->uniforms = kern_calloc(compiler->uniform_capacity, sizeof(*compiler->binary->uniforms));
	if (compiler->binary->uniforms == NULL)
		return ENOMEM;

	/* A native failure leaves every completed prefix allocation visible to the outer common cleanup. */
	error = bcm2711_shader_lower(compiler);
	if (error != 0)
		return error;

	/* The recorded program becomes native words and its uniform stream. */
	error = bcm2711_shader_emit(compiler);
	if (error != 0)
		return error;

	/* All native instructions and stage epilogues fit their validated owned storage. */
	return 0;
}

/*
 * Tells how many idle words follow one operation before an instruction may consume its result.
 *
 * A 4.2 ALU result in the physical register file is readable by the very next
 * instruction, and colour writes to the tile buffer and texture coordinate
 * writes to the TMU follow each other directly.  Special-function results
 * arrive later, and VPM and wait operations and VPM writes talk to the vertex
 * pipeline, so those keep two idle words.
 */
static uint32_t
operation_gaps(
	enum bcm2711_qpu_operation operation,
	uint32_t destination,
	uint32_t peripheral)
{
	/* The tile buffer's colour write addresses (with and without a configuration uniform) take consecutive words. */
	if (peripheral != 0 && (destination == 7 || destination == 8))
		return 0;

	/* The TMU's coordinate and parameter addresses take consecutive words; its results come after a thread switch. */
	if (peripheral != 0 && destination >= 32 && destination <= 44)
		return 0;

	/* Any other peripheral write starts a VPM transaction. */
	if (peripheral != 0)
		return 2;

	/* Operations whose results or effects come from another unit. */
	switch (operation) {
	case BCM2711_QPU_FLOAT_RECIPROCAL:
	case BCM2711_QPU_FLOAT_INVERSE_ROOT:
	case BCM2711_QPU_FLOAT_EXPONENT:
	case BCM2711_QPU_FLOAT_LOGARITHM:
	case BCM2711_QPU_FLOAT_SINE:
	case BCM2711_QPU_VPM_LOAD:
	case BCM2711_QPU_VPM_STORE:
	case BCM2711_QPU_TEXTURE_WAIT:
	case BCM2711_QPU_VPM_WAIT:
		return 2;
	default:
		break;
	}

	/* A plain ALU result is ready for the next instruction. */
	return 0;
}

/*
 * Describes the native stage to the shared optimization.
 *
 * Every program runs every skippable block and samples for every channel.
 * A fragment program writes its colour (every slot is kept, so a store the
 * analysis refuses still reaches it); a coordinate program passes on its
 * position alone, a vertex program its position and the key's varyings.
 * Only fragment programs, which run once per pixel, are folded and
 * simplified; vertex programs are only cleared of repeated and unread work.
 */
static void
optimize_options(
	enum bcm2711_shader_stage stage,
	const struct bcm2711_shader_key *key,
	struct drv_gpu_shader_optimize_options *options)
{
	const struct bcm2711_shader_component *varying;
	uint32_t index;

	/* Starts with no slot needed. */
	kern_memset(options, 0, sizeof(*options));
	options->runs_every_block = 1;

	/* A fragment program keeps every output and is simplified. */
	if (stage == BCM2711_SHADER_FRAGMENT) {
		kern_memset(options->needed, 1, sizeof(options->needed));
		options->simplify = 1;
		return;
	}

	/* Both vertex programs pass on the position. */
	for (index = 0; index < 4; index++)
		options->needed[DRV_GPU_IR_OPTIMIZE_POSITION + index] = 1;

	/* A vertex program also passes on the key's varyings, which the analysis checks later. */
	if (stage != BCM2711_SHADER_VERTEX)
		return;
	for (index = 0; index < key->varying_count && index < BCM2711_SHADER_INTERFACE_WORDS; index++) {
		varying = &key->varyings[index];
		if (varying->location < 16 && varying->component < 4)
			options->needed[varying->location * 4U + varying->component] = 1;
	}
}

/*
 * Compiles one source program for the threads it fits.
 *
 * A fragment program first tries four threads per QPU, which hide texture
 * latency better but leave each thread only rf0 to rf31; a program whose
 * values do not fit is compiled again for two threads.  Vertex programs
 * use two threads.
 */
static int
compile_stage(
	const struct drv_gpu_shader_ir *ir,
	const struct bcm2711_shader_key *key,
	enum bcm2711_shader_stage stage,
	struct bcm2711_shader_diagnostic *diagnostic,
	struct bcm2711_shader_binary **binary)
{
	int error;

	/* Tries four threads for a fragment program. */
	error = E2BIG;
	if (stage == BCM2711_SHADER_FRAGMENT)
		error = compile_threads(ir, key, stage, 4, diagnostic, binary);

	/* Falls back to two threads; the four-thread attempt's refusal is not this program's outcome. */
	if (error == E2BIG) {
		if (diagnostic != NULL)
			kern_memset(diagnostic, 0, sizeof(*diagnostic));
		error = compile_threads(ir, key, stage, 2, diagnostic, binary);
	}

	/* Reports why neither thread count compiled. */
	if (error != 0)
		return error;

	/* Succeeded: *binary is the compiled program. */
	return 0;
}

/*
 * Compiles one parsed program for a number of threads per QPU.
 *
 * E2BIG reports that the program's values do not fit the thread's registers
 * (or its code does not fit), so the caller may try fewer threads.
 */
static int
compile_threads(
	const struct drv_gpu_shader_ir *ir,
	const struct bcm2711_shader_key *key,
	enum bcm2711_shader_stage stage,
	uint32_t threads,
	struct bcm2711_shader_diagnostic *diagnostic,
	struct bcm2711_shader_binary **binary)
{
	struct bcm2711_shader_compiler *compiler;
	struct bcm2711_shader_binary *created;
	int error;

	/* Large register/output tables live in unpublished CPU storage while lowering uses the finite kernel stack. */
	*binary = NULL;
	compiler = kern_calloc(1, sizeof(*compiler));
	if (compiler == NULL)
		return ENOMEM;

	/* Compiler state owns no device reference and retires before any complete native program is published. */
	compiler->ir = ir;
	compiler->key = key;
	compiler->diagnostic = diagnostic;
	compiler->threads = threads;
	compiler->register_end = BCM2711_SHADER_REGISTER_END;
	if (threads == 4)
		compiler->register_end = BCM2711_SHADER_QUAD_REGISTER_END;

	/* Allocates the public program independently so every later failure has a single complete unwind. */
	compiler->binary = kern_calloc(1, sizeof(*compiler->binary));
	if (compiler->binary == NULL) {
		kern_free(compiler);
		return ENOMEM;
	}

	/* The stage remains immutable throughout both interface validation and native output construction. */
	compiler->binary->stage = stage;

	/* One builder owns the complete unpublished prefix and supplies one checked outcome for common cleanup. */
	error = compile_program(compiler);

	/* Allocator metadata retires before the immutable native program becomes visible. */
	created = compiler->binary;
	kern_free(compiler->values);
	kern_free(compiler->ops);
	kern_free(compiler);

	/* A failed native prefix can never escape as a usable shader binary. */
	if (error != 0) {
		bcm2711_shader_binary_free(created);
		return error;
	}

	/* Succeeded: the caller owns the complete program. */
	*binary = created;
	return 0;
}

/*
 * Places one physical register number in the program's thread window.
 *
 * Expression and payload registers keep their numbers; the scratch registers
 * rf60 to rf63 become rf28 to rf31 in a four-thread program.  E2BIG reports
 * a register a four-thread program cannot reach.
 */
static int
encode_register(
	const struct bcm2711_shader_compiler *compiler,
	uint32_t *number)
{
	/* Two-thread programs see all sixty-four registers. */
	if (compiler->threads != 4)
		return 0;

	/* The scratch registers move to the top of the four-thread window. */
	if (*number >= BCM2711_SHADER_SCRATCH_LEFT && *number < 64U) {
		*number -= BCM2711_SHADER_SCRATCH_LEFT - BCM2711_SHADER_QUAD_REGISTER_END;
		return 0;
	}

	/* A register between the window and the scratch registers cannot be reached. */
	if (*number >= BCM2711_SHADER_QUAD_REGISTER_END)
		return E2BIG;

	/* Succeeded: the register is inside the four-thread window. */
	return 0;
}

/* Records one operation with its consumed uniform and the program's thread-window register numbers. */
static int
record_op(
	struct bcm2711_shader_compiler *compiler,
	const struct bcm2711_qpu_instruction *instruction,
	enum bcm2711_shader_switch role)
{
	struct bcm2711_shader_op *op;
	struct bcm2711_qpu_instruction encoded;
	uint64_t word;
	uint32_t consumes;
	int error;

	/* Capacity refusal cannot expose a truncated successful program. */
	if (compiler->op_count == compiler->code_capacity)
		return E2BIG;

	/* Physical registers take this program's thread window before the encoder sees them. */
	encoded = *instruction;
	if (encoded.destination.peripheral == 0) {
		error = encode_register(compiler, &encoded.destination.number);
		if (error != 0)
			return error;
	}

	/* The left source register takes the window too. */
	if (encoded.left.kind == BCM2711_QPU_REGISTER) {
		error = encode_register(compiler, &encoded.left.number);
		if (error != 0)
			return error;
	}

	/* The right source register takes the window too. */
	if (encoded.right.kind == BCM2711_QPU_REGISTER) {
		error = encode_register(compiler, &encoded.right.number);
		if (error != 0)
			return error;
	}

	/* An addressed signal's register takes the window too. */
	if (encoded.signal != BCM2711_QPU_SIGNAL_NONE && encoded.signal_destination.peripheral == 0) {
		error = encode_register(compiler, &encoded.signal_destination.number);
		if (error != 0)
			return error;
	}

	/* The encoder validates the record alone now, so a refusal names the source instruction that caused it. */
	error = bcm2711_qpu_encode(&encoded, &word);
	if (error != 0)
		return error;

	/*
	 * A uniform-consuming transaction takes the described word; one without
	 * a description is refused.  The stream's count bounds the recorded
	 * consumptions here; emission writes the stream in native word order.
	 */
	op = &compiler->ops[compiler->op_count];
	kern_memset(op, 0, sizeof(*op));
	op->instruction = encoded;
	op->role = role;
	consumes = consumes_uniform(&encoded);
	if (consumes != 0) {
		if (compiler->has_pending_uniform == 0)
			return EINVAL;
		if (compiler->binary->uniform_count == compiler->uniform_capacity)
			return E2BIG;
		op->uniform = compiler->pending_uniform;
		op->has_uniform = 1;
		compiler->has_pending_uniform = 0;
		compiler->binary->uniform_count++;
	}

	/* The record list ends with this operation. */
	compiler->op_count++;

	/* Succeeded: the record list ends with this operation. */
	return 0;
}

/* Tells whether one recorded transaction reads the next word of the stage's uniform stream. */
static uint32_t
consumes_uniform(
	const struct bcm2711_qpu_instruction *instruction)
{
	/* Uniform loads and texture configuration signals read the stream. */
	if (instruction->signal == BCM2711_QPU_SIGNAL_UNIFORM)
		return 1;
	if (instruction->signal == BCM2711_QPU_SIGNAL_UNIFORM_REGISTER)
		return 1;
	if (instruction->signal == BCM2711_QPU_SIGNAL_TEXTURE_CONFIG)
		return 1;

	/* A tile write with a configuration uniform reads one too. */
	if (instruction->operation != BCM2711_QPU_IDLE &&
	    instruction->destination.peripheral != 0 &&
	    instruction->destination.number == 8)
		return 1;

	/* No other transaction touches the uniform stream. */
	return 0;
}
