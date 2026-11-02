/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The interpreter: runs checked code
 * units on the realm's VM stack.
 *
 * A frame is FRAME_HEADER slots followed by the function's registers.  The
 * header says where the caller's frame is (0 for the frame a run was
 * entered with), where the caller goes on, which function runs, which of
 * the caller's registers takes the result (and whether the frame is a
 * construction, whose result is its this value unless it returns an
 * object), the this value, the number of arguments and new.target
 * (undefined for a call).  A call from
 * bytecode to bytecode pushes a frame and a return pops it, both inside
 * one loop, so script recursion does not recurse in C; a native function
 * runs as a C call, and when it calls a script function the interpreter is
 * entered again (vm_interpret), with its own entry frame, up to
 * INTERPRETER_DEPTH_MAX times.
 *
 * A thrown exception (VM_THROWN) unwinds through the frames of the run:
 * the first handler whose range holds the throwing instruction (for a
 * caller, its call instruction) takes it; a run whose entry frame has no
 * handler returns VM_THROWN to its C caller.  Any other failure (out of
 * memory, a fault in the code's use of environments) ends the run at once.
 *
 * A task another thread asked to end (vm_heap_request_termination) is
 * seen at every loop's head, at every function's entry and resumption and
 * when an exception looks for its handler; the run then ends with
 * VM_TERMINATED like any other failure, so no catch or finally runs.
 */

#include "vm/bytecode.h"
#include "vm/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* The header's slots. */
#define FRAME_CALLER		0U
#define FRAME_RETURN		1U
#define FRAME_FUNCTION		2U
#define FRAME_RESULT		3U
#define FRAME_THIS		4U
#define FRAME_ARGC		5U
#define FRAME_NEW_TARGET	6U
#define FRAME_HEADER		7U

/* The bit of the result slot that marks a construction's frame. */
#define FRAME_CONSTRUCT		(1ULL << 32)

/* How many times the interpreter may be entered from C at once. */
#define INTERPRETER_DEPTH_MAX	256U

/* The words of a call or construct instruction (the opcode and five operands). */
#define INTERPRETER_CALL_WORDS	6U

/* What a step reports when the run's entry frame returned. */
#define INTERPRETER_DONE	(-2)

/*
 * One run of the interpreter: the realm, the base of the run's entry frame
 * and of the frame running, the offset of the instruction to run in that
 * frame's code, and the value the entry frame returned.  A run of a
 * generator or an async function has its generator, which the entry
 * frame's suspend instruction saves the frame to; suspended then says the
 * run ended there (result is the value yielded or awaited) rather than by
 * returning.
 */
struct interpreter {
	struct vm_realm *realm;
	uint32_t entry;
	uint32_t base;
	uint32_t pc;
	struct vm_code *code;
	vm_value result;
	struct vm_generator *generator;
	int suspended;
};

static int interpreter_push(struct vm_realm *realm, struct vm_function *function, vm_value this_value, const vm_value *args, unsigned count, uint32_t caller, uint32_t return_pc, uint64_t result_slot, uint32_t *base);
static int interpreter_arguments(struct vm_realm *realm, struct vm_function *function, const vm_value *args, unsigned count, vm_value *arguments);
static int interpreter_enter(struct vm_realm *realm, struct vm_function *function, vm_value this_value, const vm_value *args, unsigned count, uint64_t result_slot, vm_value new_target, struct interpreter *run);
static int interpreter_suspend(struct interpreter *run, const uint32_t *words, vm_value *registers, uint32_t next);
static int interpreter_run(struct interpreter *run);
static int interpreter_step(struct interpreter *run);
static int interpreter_step_js(struct interpreter *run, const uint32_t *words, vm_value *registers);
static int interpreter_operator(struct vm_realm *realm, const uint32_t *words, const vm_value *registers, vm_value *value);
static int interpreter_unary(struct vm_realm *realm, uint32_t opcode, vm_value operand, vm_value *value);
static int interpreter_property(struct interpreter *run, const uint32_t *words, vm_value *registers);
static int interpreter_scope(struct interpreter *run, const uint32_t *words, vm_value *registers);
static int interpreter_env(vm_value value, uint32_t hops, uint32_t slot, vm_value **place);
static int interpreter_step_wasm(struct interpreter *run, const uint32_t *words, vm_value *registers);
static int interpreter_call(struct interpreter *run, const uint32_t *words, vm_value *registers, uint32_t next);
static int interpreter_construct(struct interpreter *run, const uint32_t *words, vm_value *registers, uint32_t next);
static int interpreter_for_in_next(struct interpreter *run, const uint32_t *words, vm_value *registers, uint32_t next);
static int interpreter_for_of_next(struct interpreter *run, const uint32_t *words, vm_value *registers, uint32_t next);
static int interpreter_return(struct interpreter *run, vm_value value);
static int interpreter_unwind(struct interpreter *run);
static int interpreter_throw_error(struct vm_realm *realm, uint32_t kind, vm_value message);
static int interpreter_spread(struct interpreter *run, const uint32_t *words, vm_value *registers);
static int interpreter_class(struct interpreter *run, const uint32_t *words, vm_value *registers);
static int interpreter_super_construct(struct interpreter *run, const vm_value *args, unsigned count, vm_value *result);
static int interpreter_super_construct_array(struct interpreter *run, vm_value array, vm_value *result);
static int interpreter_args_rest(struct vm_realm *realm, vm_value arguments, uint32_t first, vm_value *result);
static struct vm_function *interpreter_function(const struct vm_realm *realm, uint32_t base);
static void interpreter_note_stack(const struct interpreter *run);
static int interpreter_is_strict(const struct interpreter *run);
static double interpreter_f64(vm_value bits);
static vm_value interpreter_bits(double number);

/*
 * Runs a bytecode function with a this value and arguments, entering the
 * interpreter from C; stores the result and returns 0, VM_THROWN with the
 * realm's exception set, or an errno value.
 */
int
vm_interpret(
	struct vm_realm *realm,
	struct vm_function *function,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct interpreter run;
	int suspendable;
	int status;

	/* A generator function makes its generator, an async function runs up to its first await. */
	suspendable = vm_code_is_suspendable(function);
	if (suspendable) {
		status = vm_generator_call(realm, function, this_value, args, count, result);
		return status;
	}

	/* An ordinary call. */
	memset(&run, 0, sizeof(run));
	status = interpreter_enter(realm, function, this_value, args, count, 0, VM_VALUE_UNDEFINED, &run);
	*result = run.result;
	if (status != 0)
		return status;

	/* Succeeded: the function's result. */
	return 0;
}

/*
 * Runs a bytecode function as a construction on the object new made (this
 * value; the empty value for a derived class's constructor, whose super
 * call makes it) with a new.target, entering the interpreter from C: the
 * result is that object unless the function returns another object.
 */
int
vm_interpret_construct(
	struct vm_realm *realm,
	struct vm_function *function,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value new_target,
	vm_value *result)
{
	struct interpreter run;
	int status;

	/* A call whose frame is marked as a construction. */
	memset(&run, 0, sizeof(run));
	status = interpreter_enter(realm, function, this_value, args, count, FRAME_CONSTRUCT, new_target, &run);
	*result = run.result;
	if (status != 0)
		return status;

	/* Succeeded: the object. */
	return 0;
}

/*
 * Starts the run of a generator or an async function from C: its frame
 * with the arguments, run until it returns or suspends (generator keeps
 * the frame then, and *suspended says so).  Stores the value returned,
 * yielded or awaited; returns 0, VM_THROWN or an errno value.
 */
int
vm_interpret_start(
	struct vm_realm *realm,
	struct vm_function *function,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	struct vm_generator *generator,
	vm_value *result,
	int *suspended)
{
	struct interpreter run;
	int status;

	/* A call whose run saves its frame to the generator when it suspends. */
	memset(&run, 0, sizeof(run));
	run.generator = generator;
	status = interpreter_enter(realm, function, this_value, args, count, 0, VM_VALUE_UNDEFINED, &run);
	*result = run.result;
	*suspended = run.suspended;
	if (status != 0)
		return status;

	/* Succeeded: the value it returned or suspended with. */
	return 0;
}

/*
 * Resumes a suspended generator or async function from C: its frame back
 * on the stack, with the value and how (VM_RESUME_*) in the suspend
 * instruction's registers, run until it returns or suspends again.
 * Stores the value returned, yielded or awaited; returns 0, VM_THROWN or
 * an errno value.
 */
int
vm_interpret_resume(
	struct vm_realm *realm,
	struct vm_generator *generator,
	vm_value value,
	int how,
	vm_value *result,
	int *suspended)
{
	struct interpreter run;
	struct vm_code *code;
	vm_value *frame;
	uint32_t size;
	int status;

	/* A task asked to end resumes nothing. */
	*result = VM_VALUE_UNDEFINED;
	*suspended = 0;
	status = vm_termination_poll(realm->heap);
	if (status != 0)
		return status;

	/* Too many entries from C, or a frame that does not fit, is a stack overflow for the script. */
	code = generator->function->code;
	size = FRAME_HEADER + code->register_count;
	if (realm->depth >= INTERPRETER_DEPTH_MAX || size > realm->stack_capacity - realm->stack_top) {
		status = vm_throw_range_error(realm, "Maximum call stack size exceeded");
		return status;
	}

	/* A generator that never suspended has no frame to go back to. */
	if (generator->registers == NULL || generator->register_count != code->register_count)
		return EINVAL;

	/* The entry frame as it was when it suspended. */
	memset(&run, 0, sizeof(run));
	run.realm = realm;
	run.result = VM_VALUE_UNDEFINED;
	run.entry = realm->stack_top;
	run.base = run.entry;
	frame = &realm->stack[run.base];
	realm->stack_top += size;
	frame[FRAME_CALLER] = 0;
	frame[FRAME_RETURN] = 0;
	frame[FRAME_FUNCTION] = vm_value_cell(generator->function);
	frame[FRAME_RESULT] = 0;
	frame[FRAME_THIS] = generator->this_value;
	frame[FRAME_ARGC] = generator->argument_count;
	frame[FRAME_NEW_TARGET] = generator->new_target;
	memcpy(&frame[FRAME_HEADER], generator->registers, (size_t)code->register_count * sizeof(vm_value));

	/* What the resumption brings, in the suspend instruction's registers. */
	frame[FRAME_HEADER + generator->value_register] = value;
	frame[FRAME_HEADER + generator->how_register] = vm_value_int32(how);

	/* Runs it from after the suspend instruction, one more entry from C while it runs. */
	run.code = code;
	run.pc = generator->pc;
	run.generator = generator;
	realm->depth++;
	status = interpreter_run(&run);
	realm->depth--;
	*result = run.result;
	*suspended = run.suspended;
	if (status != 0)
		return status;

	/* Succeeded: the value it returned or suspended with. */
	return 0;
}

/*
 * Enters the interpreter from C with an entry frame for a function; the
 * result slot says whether it is a construction.  run is the caller's
 * (its generator, when it has one, is kept); its result is the function's
 * result, undefined when it failed.
 */
static int
interpreter_enter(
	struct vm_realm *realm,
	struct vm_function *function,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	uint64_t result_slot,
	vm_value new_target,
	struct interpreter *run)
{
	struct vm_generator *generator;
	int status;

	/* The run, with the generator the caller gave. */
	generator = NULL;
	if (run->generator != NULL)
		generator = run->generator;
	memset(run, 0, sizeof(*run));
	run->realm = realm;
	run->generator = generator;
	run->result = VM_VALUE_UNDEFINED;

	/* Too many entries from C is a stack overflow for the script. */
	if (realm->depth >= INTERPRETER_DEPTH_MAX) {
		status = vm_throw_range_error(realm, "Maximum call stack size exceeded");
		return status;
	}

	/* The entry frame: no caller. */
	run->entry = realm->stack_top;
	status = interpreter_push(realm, function, this_value, args, count, 0, 0, result_slot, &run->base);
	if (status != 0)
		return status;
	realm->stack[run->base + FRAME_NEW_TARGET] = new_target;
	run->code = function->code;
	run->pc = 0;

	/* Runs it, one more entry from C while it runs. */
	realm->depth++;
	status = interpreter_run(run);
	realm->depth--;
	if (status != 0) {
		run->result = VM_VALUE_UNDEFINED;
		return status;
	}

	/* Succeeded: the function's result is the run's. */
	return 0;
}

/*
 * Pushes a frame for a bytecode function: the header, the arguments in
 * the first registers (undefined for the missing ones), every other
 * register undefined, and the arguments object when the code asks for
 * one.  caller is the caller frame's base plus one (0 for an entry frame);
 * result_slot is the caller's register, with FRAME_CONSTRUCT for new.
 */
static int
interpreter_push(
	struct vm_realm *realm,
	struct vm_function *function,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	uint32_t caller,
	uint32_t return_pc,
	uint64_t result_slot,
	uint32_t *base)
{
	struct vm_code *code;
	vm_value *frame;
	vm_value arguments;
	uint32_t size;
	uint32_t index;
	int status;

	/* A function's entry: a task asked to end calls nothing more. */
	status = vm_termination_poll(realm->heap);
	if (status != 0)
		return status;

	/* A frame that does not fit is a stack overflow for the script. */
	code = function->code;
	size = FRAME_HEADER + code->register_count;
	if (size > realm->stack_capacity - realm->stack_top) {
		status = vm_throw_range_error(realm, "Maximum call stack size exceeded");
		return status;
	}

	/* The frame goes on top of the stack. */
	*base = realm->stack_top;
	frame = &realm->stack[*base];
	realm->stack_top += size;

	/* The header. */
	frame[FRAME_CALLER] = caller;
	frame[FRAME_RETURN] = return_pc;
	frame[FRAME_FUNCTION] = vm_value_cell(function);
	frame[FRAME_RESULT] = result_slot;
	frame[FRAME_THIS] = this_value;
	frame[FRAME_ARGC] = count;
	frame[FRAME_NEW_TARGET] = VM_VALUE_UNDEFINED;

	/* The registers: the arguments, then undefined. */
	for (index = 0; index < code->register_count; index++) {
		frame[FRAME_HEADER + index] = VM_VALUE_UNDEFINED;
		if (index < code->parameter_count && index < count)
			frame[FRAME_HEADER + index] = args[index];
	}

	/* The arguments object, made once the frame is whole (making it may collect). */
	if ((code->flags & VM_CODE_ARGUMENTS) != 0U) {
		arguments = VM_VALUE_UNDEFINED;
		status = interpreter_arguments(realm, function, args, count, &arguments);
		if (status != 0) {
			realm->stack_top = *base;
			return status;
		}

		/* The object in its register. */
		realm->stack[*base + FRAME_HEADER + code->arguments_register] = arguments;
	}

	/* Succeeded: the frame is pushed. */
	return 0;
}

/*
 * Makes the arguments object of a call: an object from Object.prototype
 * with the arguments as its elements, its length, and (for sloppy code)
 * the function as callee.  It does not follow the parameters (the mapped
 * arguments of sloppy code come later).
 */
static int
interpreter_arguments(
	struct vm_realm *realm,
	struct vm_function *function,
	const vm_value *args,
	unsigned count,
	vm_value *arguments)
{
	struct vm_object *object;
	struct vm_accessor *accessor;
	struct vm_cell *roots[3];
	vm_value key;
	vm_value thrower;
	unsigned index;
	unsigned registered;
	int error;

	/* Stable slots retain the unpublished object, current key and accessor. */
	roots[0] = NULL;
	roots[1] = NULL;
	roots[2] = NULL;
	registered = 0;
	while (registered < 3U) {
		error = vm_heap_add_root(realm->heap, &roots[registered]);
		if (error != 0)
			goto cleanup;
		registered++;
	}

	/* The object. */
	object = vm_object_create(realm->heap, realm->object_prototype);
	if (object == NULL) {
		error = ENOMEM;
		goto cleanup;
	}

	/* The arguments object stays live through indexed property insertion. */
	roots[0] = &object->cell;
	object->kind = VM_KIND_ARGUMENTS;

	/* Each argument as an element. */
	for (index = 0; index < count; index++) {
		error = vm_object_define(realm->heap, object, vm_value_int32((int32_t)index), args[index], VM_PROPERTY_DEFAULT);
		if (error != 0)
			goto cleanup;
	}

	/* The length: writable and configurable, not enumerable. */
	key = vm_key_from_ascii(realm->heap, "length");
	if (key == VM_VALUE_EMPTY) {
		error = ENOMEM;
		goto cleanup;
	}

	/* Retains the length key until the object stores it. */
	roots[1] = vm_value_as_cell(key);
	error = vm_object_define(realm->heap, object, key, vm_value_int32((int32_t)count),
	    VM_PROPERTY_WRITABLE | VM_PROPERTY_CONFIGURABLE);
	if (error != 0)
		goto cleanup;

	/* Symbol.iterator: Array.prototype.values, once the built-ins exist. */
	if (realm->intrinsics[VM_INTRINSIC_ARRAY_VALUES] != NULL) {
		error = vm_object_define(realm->heap, object, vm_symbol_key(realm, VM_SYMBOL_ITERATOR),
		    vm_value_cell(realm->intrinsics[VM_INTRINSIC_ARRAY_VALUES]), VM_PROPERTY_WRITABLE | VM_PROPERTY_CONFIGURABLE);
		if (error != 0)
			goto cleanup;
	}

	/* callee: the function for sloppy code, an accessor that throws for strict code (once the realm has one). */
	key = vm_key_from_ascii(realm->heap, "callee");
	if (key == VM_VALUE_EMPTY) {
		error = ENOMEM;
		goto cleanup;
	}

	/* Retains the callee key during accessor or data publication. */
	roots[1] = vm_value_as_cell(key);
	if ((function->code->flags & VM_CODE_STRICT) == 0U) {
		error = vm_object_define(realm->heap, object, key, vm_value_cell(function),
		    VM_PROPERTY_WRITABLE | VM_PROPERTY_CONFIGURABLE);
		if (error != 0)
			goto cleanup;
	} else if (realm->intrinsics[VM_INTRINSIC_THROW_TYPE_ERROR] != NULL) {
		thrower = vm_value_cell(realm->intrinsics[VM_INTRINSIC_THROW_TYPE_ERROR]);
		accessor = vm_accessor_create(realm->heap, thrower, thrower);
		if (accessor == NULL) {
			error = ENOMEM;
			goto cleanup;
		}

		/* The strict callee accessor remains live until its property is installed. */
		roots[2] = &accessor->cell;
		error = vm_object_define(realm->heap, object, key, vm_value_cell(accessor), VM_PROPERTY_ACCESSOR);
		if (error != 0)
			goto cleanup;
	}

	/* Succeeded: the arguments object. */
	*arguments = vm_value_cell(object);
	error = 0;

cleanup:
	/* The frame takes ownership of a successfully built arguments object. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(realm->heap, &roots[registered]);
	}

	/* Reports any failed allocation or property definition. */
	if (error != 0)
		return error;

	/* Succeeded: the caller can publish the complete object. */
	return 0;
}

/* Runs instructions until the entry frame returns, an exception leaves the run, or something fails. */
static int
interpreter_run(
	struct interpreter *run)
{
	int status;

	/* One instruction after another. */
	for (;;) {
		status = interpreter_step(run);
		if (status == 0)
			continue;

		/* The entry frame returned. */
		if (status == INTERPRETER_DONE)
			return 0;

		/* An exception looks for a handler; without one it leaves the run (a task asked to end finds none). */
		if (status == VM_THROWN) {
			status = interpreter_unwind(run);
			if (status == 0)
				continue;
			if (status == VM_THROWN)
				return VM_THROWN;
		}

		/* Anything else ends the run and drops its frames. */
		run->realm->stack_top = run->entry;
		return status;
	}
}

/* Runs one instruction: the shared ones and those that jump here, the others by their group. */
static int
interpreter_step(
	struct interpreter *run)
{
	const uint32_t *words;
	vm_value *registers;
	uint32_t next;
	int truth;
	int status;

	/* The instruction, the offset after it and the frame's registers. */
	words = run->code->words + run->pc;
	next = run->pc + 1U + vm_opcodes[words[0]].operand_count;
	registers = &run->realm->stack[run->base + FRAME_HEADER];

	/* The shared instructions, and the JavaScript ones that move the pc themselves. */
	switch (words[0]) {
	case VM_OP_NOP:
		run->pc = next;
		return 0;
	case VM_OP_LOOP_HINT:
		/* A loop's head: a task asked to end leaves here. */
		status = vm_termination_poll(run->realm->heap);
		if (status != 0)
			return status;
		run->pc = next;
		return 0;
	case VM_OP_MOV:
		registers[words[1]] = registers[words[2]];
		run->pc = next;
		return 0;
	case VM_OP_LOAD_CONST:
		registers[words[1]] = run->code->constants[words[2]];
		run->pc = next;
		return 0;
	case VM_OP_LOAD_INT:
		registers[words[1]] = vm_value_int32((int32_t)words[2]);
		run->pc = next;
		return 0;
	case VM_OP_JUMP:
		run->pc += words[1];
		return 0;
	case VM_OP_JUMP_IF_TRUE:
	case VM_OP_JUMP_IF_FALSE:
		/* The jump is taken when the truth is the one the instruction names. */
		truth = vm_to_boolean(registers[words[1]]);
		if (words[0] == VM_OP_JUMP_IF_FALSE)
			truth = !truth;
		if (truth) {
			run->pc += words[2];
		} else {
			run->pc = next;
		}

		/* The run goes on from there. */
		return 0;
	case VM_OP_CALL:
		status = interpreter_call(run, words, registers, next);
		return status;
	case VM_OP_CONSTRUCT:
		status = interpreter_construct(run, words, registers, next);
		return status;
	case VM_OP_FOR_IN_NEXT:
		status = interpreter_for_in_next(run, words, registers, next);
		return status;
	case VM_OP_FOR_OF_NEXT:
		status = interpreter_for_of_next(run, words, registers, next);
		return status;
	case VM_OP_RETURN:
		status = interpreter_return(run, registers[words[1]]);
		return status;
	case VM_OP_THROW:
		/* An explicit new throw records its own frame even for the same primitive or undefined. */
		run->realm->throw_value = VM_VALUE_UNDEFINED;
		run->realm->throw_source = VM_VALUE_UNDEFINED;
		run->realm->throw_line = 0;
		run->realm->throw_column = 0;
		status = vm_throw(run->realm, registers[words[1]]);
		return status;
	case VM_OP_SUSPEND:
	case VM_OP_SUSPEND_RESULT:
		status = interpreter_suspend(run, words, registers, next);
		return status;
	default:
		break;
	}

	/* The JavaScript instructions. */
	if (words[0] < VM_OP_I32_CONST) {
		status = interpreter_step_js(run, words, registers);
		if (status != 0)
			return status;
		run->pc = next;
		return 0;
	}

	/* The Wasm instructions. */
	status = interpreter_step_wasm(run, words, registers);
	if (status != 0)
		return status;

	/* Succeeded: br_if set the pc itself when it jumped. */
	return 0;
}

/* Runs one JavaScript instruction on boxed values; the pc is moved by the caller. */
static int
interpreter_step_js(
	struct interpreter *run,
	const uint32_t *words,
	vm_value *registers)
{
	vm_value value;
	int status;

	/* The operators compute a value into their first register; the rest by what they touch. */
	switch (words[0]) {
	case VM_OP_ADD:
	case VM_OP_SUB:
	case VM_OP_MUL:
	case VM_OP_DIV:
	case VM_OP_MOD:
	case VM_OP_EXP:
	case VM_OP_BIT_AND:
	case VM_OP_BIT_OR:
	case VM_OP_BIT_XOR:
	case VM_OP_SHL:
	case VM_OP_SAR:
	case VM_OP_SHR:
	case VM_OP_LESS:
	case VM_OP_LESS_EQ:
	case VM_OP_GREATER:
	case VM_OP_GREATER_EQ:
	case VM_OP_STRICT_EQ:
	case VM_OP_LOOSE_EQ:
	case VM_OP_INSTANCEOF:
	case VM_OP_IN:
	case VM_OP_NEG:
	case VM_OP_TO_NUMBER:
	case VM_OP_TO_NUMERIC:
	case VM_OP_BIT_NOT:
	case VM_OP_NOT:
	case VM_OP_TYPEOF:
	case VM_OP_INC:
	case VM_OP_DEC:
		/* A failed operator writes nothing. */
		status = interpreter_operator(run->realm, words, registers, &value);
		if (status != 0)
			return status;
		registers[words[1]] = value;
		return 0;
	case VM_OP_NEW_OBJECT:
	case VM_OP_NEW_ARRAY:
	case VM_OP_GET_PROP:
	case VM_OP_PUT_PROP:
	case VM_OP_GET_ELEM:
	case VM_OP_PUT_ELEM:
	case VM_OP_DELETE_PROP:
	case VM_OP_DELETE_ELEM:
	case VM_OP_DEFINE_PROP:
	case VM_OP_DEFINE_ELEM:
	case VM_OP_DEFINE_GETTER:
	case VM_OP_DEFINE_SETTER:
	case VM_OP_SET_PROTO:
	case VM_OP_ARRAY_PUSH:
	case VM_OP_ARRAY_HOLE:
	case VM_OP_FOR_IN_START:
	case VM_OP_TO_PROPERTY_KEY:
	case VM_OP_NEW_REGEXP:
	case VM_OP_TO_STRING:
	case VM_OP_LOAD_EMPTY:
		status = interpreter_property(run, words, registers);
		return status;
	case VM_OP_DELEGATE_START:
	case VM_OP_DELEGATE_STEP:
	case VM_OP_ASYNC_ITER_START:
	case VM_OP_CHECK_OBJECT:
	case VM_OP_ITER_START:
	case VM_OP_ITER_NEXT:
	case VM_OP_ITER_REST:
	case VM_OP_ITER_CLOSE:
	case VM_OP_ARRAY_SPREAD:
	case VM_OP_COPY_DATA:
	case VM_OP_OBJECT_REST:
	case VM_OP_CALL_ARRAY:
	case VM_OP_CONSTRUCT_ARRAY:
	case VM_OP_CHECK_COERCIBLE:
	case VM_OP_ARGS_REST:
		status = interpreter_spread(run, words, registers);
		return status;
	case VM_OP_LOAD_NEW_TARGET:
	case VM_OP_CLASS_SETUP:
	case VM_OP_DEFINE_METHOD:
	case VM_OP_LOAD_HOME:
	case VM_OP_GET_SUPER:
	case VM_OP_SUPER_CONSTRUCT:
	case VM_OP_SUPER_CONSTRUCT_ARRAY:
	case VM_OP_NEW_PRIVATE_NAME:
	case VM_OP_PRIVATE_GET:
	case VM_OP_PRIVATE_SET:
	case VM_OP_PRIVATE_DEFINE:
	case VM_OP_PRIVATE_COPY:
	case VM_OP_PRIVATE_IN:
		status = interpreter_class(run, words, registers);
		return status;
	default:
		break;
	}

	/* The globals, environments, closures and the frame's own values. */
	status = interpreter_scope(run, words, registers);
	if (status != 0)
		return status;

	/* Succeeded: the instruction ran. */
	return 0;
}

/* Computes an operator's value from its operand registers. */
static int
interpreter_operator(
	struct vm_realm *realm,
	const uint32_t *words,
	const vm_value *registers,
	vm_value *value)
{
	vm_value left;
	vm_value right;
	int equal;
	int status;

	/* The operands (a unary operator has only the left). */
	left = registers[words[2]];
	right = VM_VALUE_UNDEFINED;
	if (vm_opcodes[words[0]].operand_count == 3U)
		right = registers[words[3]];

	/* The operator. */
	switch (words[0]) {
	case VM_OP_ADD:
		status = vm_add(realm, left, right, value);
		break;
	case VM_OP_SUB:
		status = vm_numeric(realm, VM_NUMERIC_SUB, left, right, value);
		break;
	case VM_OP_MUL:
		status = vm_numeric(realm, VM_NUMERIC_MUL, left, right, value);
		break;
	case VM_OP_DIV:
		status = vm_numeric(realm, VM_NUMERIC_DIV, left, right, value);
		break;
	case VM_OP_MOD:
		status = vm_numeric(realm, VM_NUMERIC_MOD, left, right, value);
		break;
	case VM_OP_EXP:
		status = vm_numeric(realm, VM_NUMERIC_EXP, left, right, value);
		break;
	case VM_OP_BIT_AND:
		status = vm_numeric(realm, VM_NUMERIC_AND, left, right, value);
		break;
	case VM_OP_BIT_OR:
		status = vm_numeric(realm, VM_NUMERIC_OR, left, right, value);
		break;
	case VM_OP_BIT_XOR:
		status = vm_numeric(realm, VM_NUMERIC_XOR, left, right, value);
		break;
	case VM_OP_SHL:
		status = vm_numeric(realm, VM_NUMERIC_SHL, left, right, value);
		break;
	case VM_OP_SAR:
		status = vm_numeric(realm, VM_NUMERIC_SAR, left, right, value);
		break;
	case VM_OP_SHR:
		status = vm_numeric(realm, VM_NUMERIC_SHR, left, right, value);
		break;
	case VM_OP_LESS:
		status = vm_relation(realm, VM_RELATION_LESS, left, right, value);
		break;
	case VM_OP_LESS_EQ:
		status = vm_relation(realm, VM_RELATION_LESS_EQUAL, left, right, value);
		break;
	case VM_OP_GREATER:
		status = vm_relation(realm, VM_RELATION_GREATER, left, right, value);
		break;
	case VM_OP_GREATER_EQ:
		status = vm_relation(realm, VM_RELATION_GREATER_EQUAL, left, right, value);
		break;
	case VM_OP_STRICT_EQ:
		*value = vm_value_boolean(vm_strict_equals(left, right));
		status = 0;
		break;
	case VM_OP_LOOSE_EQ:
		status = vm_loose_equals(realm, left, right, &equal);
		if (status == 0)
			*value = vm_value_boolean(equal);
		break;
	case VM_OP_INSTANCEOF:
		status = vm_instanceof(realm, left, right, value);
		break;
	case VM_OP_IN:
		status = vm_in(realm, left, right, value);
		break;
	default:
		status = interpreter_unary(realm, words[0], left, value);
		break;
	}

	/* A failed operator reports why. */
	if (status != 0)
		return status;

	/* Succeeded: the value. */
	return 0;
}

/* Computes a unary operator's value. */
static int
interpreter_unary(
	struct vm_realm *realm,
	uint32_t opcode,
	vm_value operand,
	vm_value *value)
{
	struct vm_bigint *big;
	vm_value numeric;
	double number;
	int32_t whole;
	int status;

	/* The operators that work on a value as it is. */
	if (opcode == VM_OP_NOT) {
		*value = vm_value_boolean(!vm_to_boolean(operand));
		return 0;
	}

	/* typeof names the type. */
	if (opcode == VM_OP_TYPEOF) {
		status = vm_typeof(realm, operand, value);
		if (status != 0)
			return status;
		return 0;
	}

	/* ToNumeric: a BigInt stays a BigInt. */
	if (opcode == VM_OP_TO_NUMERIC) {
		status = vm_to_numeric(realm, operand, value);
		if (status != 0)
			return status;
		return 0;
	}

	/* The operators that also work on a BigInt. */
	if (opcode == VM_OP_BIT_NOT || opcode == VM_OP_NEG || opcode == VM_OP_INC || opcode == VM_OP_DEC) {
		status = vm_to_numeric(realm, operand, &numeric);
		if (status != 0)
			return status;
		big = vm_value_as_bigint(numeric);
		if (big != NULL) {
			if (opcode == VM_OP_BIT_NOT)
				status = vm_bigint_not(realm->heap, big, value);
			else if (opcode == VM_OP_NEG)
				status = vm_bigint_negate(realm->heap, big, value);
			else
				status = vm_bigint_add_small(realm->heap, big, opcode == VM_OP_INC ? 1 : -1, value);
			return status;
		}
		operand = numeric;
	}

	/* ~ works on the int32. */
	if (opcode == VM_OP_BIT_NOT) {
		status = vm_to_int32(realm, operand, &whole);
		if (status != 0)
			return status;
		*value = vm_value_int32(~whole);
		return 0;
	}

	/* The others on the number. */
	status = vm_to_number(realm, operand, &number);
	if (status != 0)
		return status;

	/* The operator on the number. */
	switch (opcode) {
	case VM_OP_NEG:
		*value = vm_value_number(-number);
		break;
	case VM_OP_TO_NUMBER:
		*value = vm_value_number(number);
		break;
	case VM_OP_INC:
		*value = vm_value_number(number + 1.0);
		break;
	case VM_OP_DEC:
		*value = vm_value_number(number - 1.0);
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the value. */
	return 0;
}

/* Runs one instruction on objects' properties: making, reading, writing, deleting, defining and enumerating. */
static int
interpreter_property(
	struct interpreter *run,
	const uint32_t *words,
	vm_value *registers)
{
	struct vm_realm *realm;
	struct vm_object *object;
	struct vm_string *string;
	vm_value literal[2];
	vm_value value;
	vm_value key;
	int strict;
	int is_object;
	int status;

	/* Each instruction; one that computes a value writes it only when it succeeds. */
	realm = run->realm;
	strict = interpreter_is_strict(run);
	switch (words[0]) {
	case VM_OP_NEW_OBJECT:
		/* A plain object from Object.prototype. */
		object = vm_object_create(realm->heap, realm->object_prototype);
		if (object == NULL)
			return ENOMEM;
		registers[words[1]] = vm_value_cell(object);
		return 0;
	case VM_OP_NEW_ARRAY:
		/* An empty array from Array.prototype. */
		object = vm_array_create(realm->heap, realm->array_prototype);
		if (object == NULL)
			return ENOMEM;
		registers[words[1]] = vm_value_cell(object);
		return 0;
	case VM_OP_GET_PROP:
		status = vm_get(realm, registers[words[2]], run->code->constants[words[3]], &value);
		break;
	case VM_OP_PUT_PROP:
		status = vm_set(realm, registers[words[1]], run->code->constants[words[2]], registers[words[3]], strict);
		return status;
	case VM_OP_GET_ELEM:
		/* The base must have properties before the key is converted. */
		if (registers[words[2]] == VM_VALUE_UNDEFINED || registers[words[2]] == VM_VALUE_NULL) {
			status = vm_throw_type_error(realm, "Cannot read properties of undefined or null");
			return status;
		}

		/* The key, converted, then the read. */
		status = vm_to_key(realm, registers[words[3]], &key);
		if (status == 0)
			status = vm_get(realm, registers[words[2]], key, &value);
		break;
	case VM_OP_PUT_ELEM:
		/* The base must have properties before the key is converted. */
		if (registers[words[1]] == VM_VALUE_UNDEFINED || registers[words[1]] == VM_VALUE_NULL) {
			status = vm_throw_type_error(realm, "Cannot set properties of undefined or null");
			return status;
		}

		/* The key, converted, then the write. */
		status = vm_to_key(realm, registers[words[2]], &key);
		if (status == 0)
			status = vm_set(realm, registers[words[1]], key, registers[words[3]], strict);
		return status;
	case VM_OP_DELETE_PROP:
		status = vm_delete(realm, registers[words[2]], run->code->constants[words[3]], strict, &value);
		break;
	case VM_OP_DELETE_ELEM:
		/* The base must have properties before the key is converted. */
		if (registers[words[2]] == VM_VALUE_UNDEFINED || registers[words[2]] == VM_VALUE_NULL) {
			status = vm_throw_type_error(realm, "Cannot convert undefined or null to object");
			return status;
		}

		/* The key, converted, then the deletion. */
		status = vm_to_key(realm, registers[words[3]], &key);
		if (status == 0)
			status = vm_delete(realm, registers[words[2]], key, strict, &value);
		break;
	case VM_OP_DEFINE_PROP:
		status = vm_define_data(realm, registers[words[1]], run->code->constants[words[2]], registers[words[3]]);
		return status;
	case VM_OP_DEFINE_ELEM:
	case VM_OP_DEFINE_GETTER:
	case VM_OP_DEFINE_SETTER:
		/* The key as a property key, then the data property or the accessor half. */
		status = vm_to_key(realm, registers[words[2]], &key);
		if (status != 0)
			return status;
		if (words[0] == VM_OP_DEFINE_ELEM) {
			status = vm_define_data(realm, registers[words[1]], key, registers[words[3]]);
		} else {
			status = vm_define_accessor(realm, registers[words[1]], key, registers[words[3]], words[0] == VM_OP_DEFINE_SETTER);
		}

		/* Reports whether the definition succeeded. */
		return status;
	case VM_OP_SET_PROTO:
		/* An object or null becomes the literal's prototype; anything else is ignored. */
		object = (struct vm_object *)vm_value_as_cell(registers[words[1]]);
		value = registers[words[2]];
		is_object = vm_value_is_object(value);
		if (is_object)
			object->prototype = (struct vm_object *)vm_value_as_cell(value);
		if (value == VM_VALUE_NULL)
			object->prototype = NULL;
		return 0;
	case VM_OP_ARRAY_PUSH:
		/* The value at the array's end. */
		object = (struct vm_object *)vm_value_as_cell(registers[words[1]]);
		status = vm_object_define(realm->heap, object, vm_value_int32((int32_t)object->length), registers[words[2]],
		    VM_PROPERTY_DEFAULT);
		return status;
	case VM_OP_ARRAY_HOLE:
		/* The array one longer, with nothing at its end. */
		object = (struct vm_object *)vm_value_as_cell(registers[words[1]]);
		status = vm_array_set_length(realm->heap, object, object->length + 1U);
		return status;
	case VM_OP_FOR_IN_START:
		status = vm_for_in_start(realm, registers[words[2]], &value);
		break;
	case VM_OP_TO_PROPERTY_KEY:
		status = vm_to_key(realm, registers[words[2]], &value);
		break;
	case VM_OP_NEW_REGEXP:
		/* A new object of the realm's RegExp from the literal's pattern and flags (the built-ins install it). */
		if (realm->intrinsics[VM_INTRINSIC_REGEXP] == NULL) {
			status = vm_throw_type_error(realm, "regular expressions are not available");
			return status;
		}

		/* RegExp(pattern, flags) with itself as new.target. */
		value = vm_value_cell(realm->intrinsics[VM_INTRINSIC_REGEXP]);
		literal[0] = run->code->constants[words[2]];
		literal[1] = run->code->constants[words[3]];
		status = vm_construct(realm, value, literal, 2, value, &value);
		break;
	case VM_OP_TO_STRING:
		/* ToString: a template's substitution. */
		status = vm_to_string(realm, registers[words[2]], &string);
		if (status == 0)
			value = vm_value_cell(string);
		break;
	case VM_OP_LOAD_EMPTY:
		/* The mark of a let or const binding whose declaration has not run. */
		value = VM_VALUE_EMPTY;
		status = 0;
		break;
	default:
		return EINVAL;
	}

	/* A failed instruction writes nothing. */
	if (status != 0)
		return status;

	/* Succeeded: the value in the first operand's register. */
	registers[words[1]] = value;
	return 0;
}

/* Runs one instruction on the scope: globals, environments, closures, this and the callee. */
static int
interpreter_scope(
	struct interpreter *run,
	const uint32_t *words,
	vm_value *registers)
{
	struct vm_realm *realm;
	struct vm_function *function;
	struct vm_function *closure;
	struct vm_env *env;
	struct vm_env *parent;
	struct vm_import_binding *import;
	struct vm_cell *cell;
	vm_value *frame;
	vm_value *place;
	vm_value value;
	int is_cell;
	int strict;
	int status;

	/* Each instruction; one that computes a value writes it only when it succeeds. */
	realm = run->realm;
	frame = &realm->stack[run->base];
	function = interpreter_function(realm, run->base);
	strict = interpreter_is_strict(run);
	switch (words[0]) {
	case VM_OP_GET_GLOBAL:
	case VM_OP_GET_GLOBAL_TYPEOF:
		status = vm_get_global(realm, run->code->constants[words[2]], words[0] == VM_OP_GET_GLOBAL_TYPEOF, &value);
		break;
	case VM_OP_PUT_GLOBAL:
		status = vm_put_global(realm, run->code->constants[words[1]], registers[words[2]], strict);
		return status;
	case VM_OP_DEFINE_GLOBAL_VAR:
		status = vm_define_global_var(realm, run->code->constants[words[1]]);
		return status;
	case VM_OP_DEFINE_GLOBAL_FUNCTION:
		status = vm_define_global_function(realm, run->code->constants[words[1]], registers[words[2]]);
		return status;
	case VM_OP_DELETE_GLOBAL:
		status = vm_delete_global(realm, run->code->constants[words[2]], &value);
		break;
	case VM_OP_CHECK_INIT:
		/* A let or const binding read or written before its declaration ran. */
		if (registers[words[1]] == VM_VALUE_EMPTY) {
			status = vm_throw_uninitialized(realm, run->code->constants[words[2]]);
			return status;
		}

		/* An initialized binding goes on. */
		return 0;
	case VM_OP_THROW_ERROR:
		status = interpreter_throw_error(realm, words[1], run->code->constants[words[2]]);
		return status;
	case VM_OP_DEFINE_GLOBAL_LEXICAL:
		status = vm_define_global_lexical(realm, run->code->constants[words[1]], words[2] != 0U);
		return status;
	case VM_OP_INIT_GLOBAL_LEXICAL:
		status = vm_init_global_lexical(realm, run->code->constants[words[1]], registers[words[2]]);
		return status;
	case VM_OP_NEW_ENV:
		/* The parent is an environment or undefined. */
		status = interpreter_env(registers[words[2]], 0, 0, &place);
		if (status != 0)
			return status;
		parent = NULL;
		if (registers[words[2]] != VM_VALUE_UNDEFINED)
			parent = (struct vm_env *)vm_value_as_cell(registers[words[2]]);
		env = vm_env_create(realm->heap, parent, words[3]);
		if (env == NULL)
			return ENOMEM;
		value = vm_value_cell(env);
		status = 0;
		break;
	case VM_OP_GET_ENV:
		status = interpreter_env(registers[words[2]], words[3], words[4], &place);
		if (status != 0)
			return status;
		value = *place;
		break;
	case VM_OP_GET_IMPORT:
		/* An import follows its target's current value on every read. */
		value = registers[words[2]];
		is_cell = vm_value_is_cell(value);
		if (!is_cell)
			return vm_throw_reference_error(realm, "Module import is not linked");
		cell = vm_value_as_cell(value);
		if (cell->type != &vm_import_binding_type)
			return vm_throw_reference_error(realm, "Module import is not linked");
		import = (struct vm_import_binding *)cell;
		if (import->env == NULL || import->slot >= import->env->count)
			return EINVAL;
		value = vm_env_slots(import->env)[import->slot];
		if (value == VM_VALUE_EMPTY)
			return vm_throw_reference_error(realm, "Cannot access import before initialization");
		status = 0;
		break;
	case VM_OP_PUT_ENV:
		status = interpreter_env(registers[words[1]], words[2], words[3], &place);
		if (status != 0)
			return status;
		*place = registers[words[4]];
		return 0;
	case VM_OP_LOAD_CLOSURE_ENV:
		/* The running function's environment, undefined for none. */
		value = VM_VALUE_UNDEFINED;
		if (function->env != NULL)
			value = vm_value_cell(function->env);
		status = 0;
		break;
	case VM_OP_LOAD_THIS:
		/* A derived class's constructor has no this before its super call. */
		value = frame[FRAME_THIS];
		if (value == VM_VALUE_EMPTY) {
			status = vm_throw_reference_error(realm,
			    "Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
			return status;
		}

		/* Sloppy code sees the global object for undefined and null, and an object for a primitive. */
		status = 0;
		if (!strict && (value == VM_VALUE_UNDEFINED || value == VM_VALUE_NULL)) {
			value = vm_value_cell(realm->global);
		} else if (!strict) {
			status = vm_to_object(realm, value, &value);
		}

		/* The value. */
		break;
	case VM_OP_LOAD_CALLEE:
		value = vm_value_cell(function);
		status = 0;
		break;
	case VM_OP_NEW_CLOSURE:
		/* The environment is one or undefined; the constant is a code unit (the checker saw to it). */
		status = interpreter_env(registers[words[3]], 0, 0, &place);
		if (status != 0)
			return status;
		env = NULL;
		if (registers[words[3]] != VM_VALUE_UNDEFINED)
			env = (struct vm_env *)vm_value_as_cell(registers[words[3]]);
		closure = vm_closure_create(realm, (struct vm_code *)vm_value_as_cell(run->code->constants[words[2]]), env);
		if (closure == NULL)
			return ENOMEM;

		/* New closures retain the actual defining function's opaque source before publication. */
		closure->source = function->source;
		value = vm_value_cell(closure);
		break;
	default:
		return EINVAL;
	}

	/* A failed instruction writes nothing. */
	if (status != 0)
		return status;

	/* Succeeded: the value in the first operand's register. */
	registers[words[1]] = value;
	return 0;
}

/*
 * Finds a slot of an environment some hops out from one; EINVAL when the
 * code's use does not fit what is there (the compiler's fault, not the
 * script's).  With no hops and slot 0, it only checks that the value is an
 * environment or undefined.
 */
static int
interpreter_env(
	vm_value value,
	uint32_t hops,
	uint32_t slot,
	vm_value **place)
{
	struct vm_env *env;
	struct vm_cell *cell;
	int is_cell;

	/* undefined is no environment, which only a check accepts. */
	*place = NULL;
	if (value == VM_VALUE_UNDEFINED) {
		if (hops == 0 && slot == 0)
			return 0;
		return EINVAL;
	}

	/* The value must be an environment. */
	is_cell = vm_value_is_cell(value);
	if (!is_cell)
		return EINVAL;
	cell = vm_value_as_cell(value);
	if (cell->type != &vm_env_type)
		return EINVAL;
	env = (struct vm_env *)cell;

	/* Outwards by the hops. */
	while (hops > 0) {
		env = env->parent;
		if (env == NULL)
			return EINVAL;
		hops--;
	}

	/* The slot must be one of its own (a check of an empty environment asks for none). */
	if (slot >= env->count) {
		if (env->count == 0 && slot == 0)
			return 0;
		return EINVAL;
	}

	/* Succeeded: the slot. */
	*place = &vm_env_slots(env)[slot];
	return 0;
}

/* Runs one Wasm instruction on raw values, moving the pc itself. */
static int
interpreter_step_wasm(
	struct interpreter *run,
	const uint32_t *words,
	vm_value *registers)
{
	uint32_t start;
	uint32_t left;
	uint32_t right;
	int status;

	/* The next instruction, unless a branch is taken (jumps count from the instruction's start). */
	start = run->pc;
	run->pc = start + 1U + vm_opcodes[words[0]].operand_count;

	/* Each instruction; an i32 is the low 32 bits, zero-extended. */
	switch (words[0]) {
	case VM_OP_I32_CONST:
		registers[words[1]] = (uint64_t)words[2];
		break;
	case VM_OP_I32_ADD:
		registers[words[1]] = (uint64_t)(uint32_t)((uint32_t)registers[words[2]] + (uint32_t)registers[words[3]]);
		break;
	case VM_OP_I32_SUB:
		registers[words[1]] = (uint64_t)(uint32_t)((uint32_t)registers[words[2]] - (uint32_t)registers[words[3]]);
		break;
	case VM_OP_I32_MUL:
		registers[words[1]] = (uint64_t)(uint32_t)((uint32_t)registers[words[2]] * (uint32_t)registers[words[3]]);
		break;
	case VM_OP_I32_LT_S:
		/* Signed: the two as int32. */
		left = (uint32_t)registers[words[2]];
		right = (uint32_t)registers[words[3]];
		registers[words[1]] = 0U;
		if ((int32_t)left < (int32_t)right)
			registers[words[1]] = 1U;
		break;
	case VM_OP_I32_EQZ:
		registers[words[1]] = 0U;
		if ((uint32_t)registers[words[2]] == 0U)
			registers[words[1]] = 1U;
		break;
	case VM_OP_BR_IF:
		/* Taken when the i32 is not zero. */
		if ((uint32_t)registers[words[1]] == 0U)
			break;
		run->pc = start + words[2];

		/* A branch back is a loop's: a task asked to end leaves there. */
		if ((int32_t)words[2] <= 0) {
			status = vm_termination_poll(run->realm->heap);
			if (status != 0)
				return status;
		}
		break;
	case VM_OP_I64_CONST:
	case VM_OP_F64_CONST:
		/* The low and high words of the bits. */
		registers[words[1]] = (uint64_t)words[2] | ((uint64_t)words[3] << 32);
		break;
	case VM_OP_I64_ADD:
		registers[words[1]] = registers[words[2]] + registers[words[3]];
		break;
	case VM_OP_F64_ADD:
		registers[words[1]] = interpreter_bits(interpreter_f64(registers[words[2]]) + interpreter_f64(registers[words[3]]));
		break;
	case VM_OP_F64_MUL:
		registers[words[1]] = interpreter_bits(interpreter_f64(registers[words[2]]) * interpreter_f64(registers[words[3]]));
		break;
	case VM_OP_BOX_I32:
		registers[words[1]] = vm_value_int32((int32_t)(uint32_t)registers[words[2]]);
		break;
	case VM_OP_BOX_F64:
		registers[words[1]] = vm_value_number(interpreter_f64(registers[words[2]]));
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the instruction ran. */
	return 0;
}

/*
 * Calls a function from bytecode: a bytecode callee gets a frame and runs
 * next; a native one runs now and its result goes to the register.
 */
static int
interpreter_call(
	struct interpreter *run,
	const uint32_t *words,
	vm_value *registers,
	uint32_t next)
{
	struct vm_function *function;
	struct vm_cell *cell;
	vm_value callee;
	vm_value value;
	uint32_t base;
	int callable;
	int suspendable;
	int status;

	/* Only functions can be called. */
	callee = registers[words[2]];
	callable = vm_value_is_callable(callee);
	if (!callable) {
		status = vm_throw_type_error(run->realm, "value is not a function");
		return status;
	}

	/* An exotic callable must reach generic dispatch before any optimized vm_function access. */
	cell = vm_value_as_cell(callee);
	if (cell->type != &vm_function_type) {
		status = vm_call(run->realm, callee, registers[words[3]], &registers[words[4]], words[5], &value);
		if (status != 0)
			return status;
		registers[words[1]] = value;
		run->pc = next;

		/* Succeeded: the exotic result is published before the caller resumes. */
		return 0;
	}

	/* A class's constructor runs only with new. */
	function = (struct vm_function *)vm_value_as_cell(callee);
	if (function->code != NULL &&
	    (function->code->flags & VM_CODE_CLASS) != 0U) {
		status = vm_throw_class_call(run->realm, function);
		return status;
	}

	/* A foreign function enters its own realm and transports any thrown value. */
	if (function->realm != run->realm) {
		status = vm_call(
		    run->realm,
		    callee,
		    registers[words[3]],
		    &registers[words[4]],
		    words[5],
		    &value);
		if (status != 0)
			return status;

		/* Continues the caller after the foreign stack has unwound. */
		registers[words[1]] = value;
		run->pc = next;
		return 0;
	}

	/* A generator or an async function runs from C, which keeps its frame apart when it suspends. */
	suspendable = vm_code_is_suspendable(function);
	if (suspendable) {
		status = vm_generator_call(function->realm, function, registers[words[3]], &registers[words[4]], words[5], &value);
		if (status != 0)
			return status;

		/* The generator or the promise is in the register and the caller goes on. */
		registers[words[1]] = value;
		run->pc = next;
		return 0;
	}

	/* A bytecode function: its frame, whose caller goes on after the call. */
	if (function->code != NULL) {
		status = interpreter_push(
		    run->realm,
		    function,
		    registers[words[3]],
		    &registers[words[4]],
		    words[5],
		    run->base + 1U,
		    next,
		    words[1],
		    &base);
		if (status != 0)
			return status;
		run->base = base;
		run->code = function->code;
		run->pc = 0;
		return 0;
	}

	/* Uses the shared native call boundary to preserve reentrant callee state. */
	status = vm_call(
	    run->realm,
	    callee,
	    registers[words[3]],
	    &registers[words[4]],
	    words[5],
	    &value);
	if (status != 0)
		return status;

	/* Succeeded: the result is in the register and the caller goes on. */
	registers[words[1]] = value;
	run->pc = next;
	return 0;
}

/*
 * Constructs with new from bytecode: the new object from the constructor's
 * prototype is the this value; a bytecode constructor gets a frame that
 * returns it unless it returns an object; a native one runs now.
 */
static int
interpreter_construct(
	struct interpreter *run,
	const uint32_t *words,
	vm_value *registers,
	uint32_t next)
{
	struct vm_function *function;
	struct vm_cell *cell;
	vm_value callee;
	vm_value this_value;
	vm_value value;
	uint32_t base;
	int constructor;
	int status;

	/* Only constructors can be used with new. */
	callee = registers[words[2]];
	constructor = vm_value_is_constructor(callee);
	if (!constructor) {
		status = vm_throw_type_error(run->realm, "value is not a constructor");
		return status;
	}

	/* An exotic callable must reach generic dispatch before any optimized vm_function access. */
	cell = vm_value_as_cell(callee);
	if (cell->type != &vm_function_type) {
		status = vm_construct(run->realm, callee, &registers[words[4]], words[5], registers[words[3]], &value);
		if (status != 0)
			return status;
		registers[words[1]] = value;
		run->pc = next;

		/* Succeeded: the exotic result is published before the caller resumes. */
		return 0;
	}

	/* Constructs in a foreign realm without putting its code on our stack. */
	function = (struct vm_function *)vm_value_as_cell(callee);
	if (function->realm != run->realm) {
		status = vm_construct(
		    run->realm,
		    callee,
		    &registers[words[4]],
		    words[5],
		    registers[words[3]],
		    &value);
		if (status != 0)
			return status;

		/* Continues the caller with the foreign constructor's object. */
		registers[words[1]] = value;
		run->pc = next;
		return 0;
	}

	/* A bytecode constructor: the new object (none for a derived class's), then its frame, marked as a construction. */
	if (function->code != NULL) {
		this_value = VM_VALUE_EMPTY;
		if ((function->code->flags & VM_CODE_DERIVED) == 0U) {
			status = vm_construct_this(run->realm, registers[words[3]], &this_value);
			if (status != 0)
				return status;
		}

		/* The frame, with new.target. */
		status = interpreter_push(
		    run->realm,
		    function,
		    this_value,
		    &registers[words[4]],
		    words[5],
		    run->base + 1U,
		    next,
		    words[1] | FRAME_CONSTRUCT,
		    &base);
		if (status != 0)
			return status;
		run->realm->stack[base + FRAME_NEW_TARGET] = registers[words[3]];
		run->base = base;
		run->code = function->code;
		run->pc = 0;
		return 0;
	}

	/* Uses the shared native constructor boundary with scoped new.target. */
	status = vm_construct(
	    run->realm,
	    callee,
	    &registers[words[4]],
	    words[5],
	    registers[words[3]],
	    &value);
	if (status != 0)
		return status;

	/* Succeeded: the object is in the register and the caller goes on. */
	registers[words[1]] = value;
	run->pc = next;
	return 0;
}

/* Moves a for-in loop to its next key, or jumps when the loop is over. */
static int
interpreter_for_in_next(
	struct interpreter *run,
	const uint32_t *words,
	vm_value *registers,
	uint32_t next)
{
	vm_value key;
	int done;
	int status;

	/* The next key. */
	status = vm_for_in_next(run->realm, registers[words[2]], &key, &done);
	if (status != 0)
		return status;

	/* No key left: the loop ends. */
	if (done) {
		run->pc += words[3];
		return 0;
	}

	/* Succeeded: the key in the register, and the body runs. */
	registers[words[1]] = key;
	run->pc = next;
	return 0;
}

/* Moves a for-of loop to its iteration's next value, or jumps when the iteration has ended. */
static int
interpreter_for_of_next(
	struct interpreter *run,
	const uint32_t *words,
	vm_value *registers,
	uint32_t next)
{
	vm_value value;
	int done;
	int status;

	/* The next value. */
	status = vm_iter_next(run->realm, registers[words[2]], &value, &done);
	if (status != 0)
		return status;

	/* No value left: the loop ends. */
	if (done) {
		run->pc += words[3];
		return 0;
	}

	/* Succeeded: the value in the register, and the body runs. */
	registers[words[1]] = value;
	run->pc = next;
	return 0;
}

/* Returns from the running frame: to its caller's register, or out of the run from the entry frame. */
static int
interpreter_return(
	struct interpreter *run,
	vm_value value)
{
	struct vm_realm *realm;
	struct vm_function *function;
	vm_value *frame;
	uint64_t result_slot;
	uint32_t caller;
	uint32_t result_register;
	uint32_t return_pc;
	int is_object;
	int status;

	/* The frame's header, read before the frame is popped. */
	realm = run->realm;
	frame = &realm->stack[run->base];
	caller = (uint32_t)frame[FRAME_CALLER];
	result_slot = frame[FRAME_RESULT];
	result_register = (uint32_t)result_slot;
	return_pc = (uint32_t)frame[FRAME_RETURN];

	/* A construction returns its this value unless the code returned an object. */
	if ((result_slot & FRAME_CONSTRUCT) != 0U) {
		is_object = vm_value_is_object(value);
		if (!is_object && value != VM_VALUE_UNDEFINED && (run->code->flags & VM_CODE_DERIVED) != 0U) {
			status = vm_throw_type_error(realm, "Derived constructors may only return object or undefined");
			return status;
		}

		/* Any other value that is not an object gives this. */
		if (!is_object)
			value = frame[FRAME_THIS];

		/* A derived class's constructor must have called super. */
		if (value == VM_VALUE_EMPTY) {
			status = vm_throw_reference_error(realm,
			    "Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
			return status;
		}
	}

	/* The frame is popped. */
	realm->stack_top = run->base;

	/* The entry frame's return ends the run. */
	if (caller == 0U) {
		run->result = value;
		return INTERPRETER_DONE;
	}

	/* The caller goes on after its call with the value in its register. */
	run->base = caller - 1U;
	function = interpreter_function(realm, run->base);
	run->code = function->code;
	run->pc = return_pc;
	realm->stack[run->base + FRAME_HEADER + result_register] = value;

	/* Succeeded: the caller runs next. */
	return 0;
}

/*
 * Suspends the run of a generator or an async function at a suspend
 * instruction: its entry frame's registers and where it goes on are saved
 * to the run's generator, the frame is popped, and the run ends with the
 * value yielded or awaited.
 */
static int
interpreter_suspend(
	struct interpreter *run,
	const uint32_t *words,
	vm_value *registers,
	uint32_t next)
{
	struct vm_generator *generator;
	vm_value *frame;
	uint32_t count;

	/* Only the entry frame of a generator's run can suspend (the compiler writes the instruction nowhere else). */
	generator = run->generator;
	frame = &run->realm->stack[run->base];
	if (generator == NULL || run->base != run->entry)
		return EINVAL;

	/* The place for the registers, made when the run first suspends. */
	count = run->code->register_count;
	if (generator->registers == NULL) {
		generator->registers = malloc((size_t)count * sizeof(vm_value) + 1U);
		if (generator->registers == NULL)
			return ENOMEM;
		generator->register_count = count;
	}

	/* The frame: its registers, its header's values and where it goes on. */
	memcpy(generator->registers, registers, (size_t)count * sizeof(vm_value));
	generator->this_value = frame[FRAME_THIS];
	generator->argument_count = frame[FRAME_ARGC];
	generator->new_target = frame[FRAME_NEW_TARGET];
	generator->pc = next;
	generator->value_register = words[2];
	generator->how_register = words[3];

	/* Only delegated results bypass the built-in generator method's result wrapper. */
	generator->direct_result = 0;
	if (words[0] == VM_OP_SUSPEND_RESULT)
		generator->direct_result = 1;

	/* The frame is popped, and the run ends with the value. */
	run->result = registers[words[1]];
	run->suspended = 1;
	run->realm->stack_top = run->base;

	/* Succeeded: the run is over until the generator is resumed. */
	return INTERPRETER_DONE;
}

/*
 * Finds the handler of the realm's exception from the running
 * instruction outwards through the run's frames; 0 when one takes it (the
 * run goes on there), VM_THROWN when the run's entry frame has none, and
 * VM_TERMINATED (the exception dropped) when the task was asked to end.
 */
static int
interpreter_unwind(
	struct interpreter *run)
{
	struct vm_realm *realm;
	struct vm_function *function;
	const struct vm_handler *handler;
	uint32_t throw_pc;
	uint32_t caller;
	uint32_t index;
	int status;

	/* A task asked to end gives the exception to no handler: it is dropped with the run's frames. */
	realm = run->realm;
	status = vm_termination_poll(realm->heap);
	if (status != 0) {
		realm->exception = VM_VALUE_UNDEFINED;
		return status;
	}

	/* From the instruction that threw, frame by frame. */
	throw_pc = run->pc;

	/*
	 * A new exception's place is the instruction that threw it; an
	 * exception coming back out of a nested run keeps the place the inner
	 * run found.
	 */
	if (realm->throw_line == 0 || realm->throw_value != realm->exception) {
		realm->throw_value = realm->exception;
		realm->throw_line = 0;
		realm->throw_column = 0;

		/* Preserve defining-source metadata before any bytecode frame is removed. */
		function = interpreter_function(realm, run->base);
		realm->throw_source = function->source;
		vm_code_position(run->code, throw_pc, &realm->throw_line, &realm->throw_column);
		interpreter_note_stack(run);
	}

	/* Looks for a handler in each frame outwards. */
	for (;;) {
		/* A handler of this frame whose range holds the instruction. */
		for (index = 0; index < run->code->handler_count; index++) {
			handler = &run->code->handlers[index];
			if (throw_pc < handler->start || throw_pc >= handler->end)
				continue;

			/* It takes the exception, and the frame goes on there. */
			realm->stack[run->base + FRAME_HEADER + handler->exception_register] = realm->exception;
			realm->exception = VM_VALUE_UNDEFINED;
			run->pc = handler->handler;

			/* A caught exception's place is forgotten, so throwing it again records the new place. */
			realm->throw_value = VM_VALUE_UNDEFINED;
			realm->throw_source = VM_VALUE_UNDEFINED;
			realm->throw_line = 0;
			return 0;
		}

		/* Without one, the frame is popped; the entry frame's end leaves the run. */
		caller = (uint32_t)realm->stack[run->base + FRAME_CALLER];
		throw_pc = (uint32_t)realm->stack[run->base + FRAME_RETURN] - INTERPRETER_CALL_WORDS;
		realm->stack_top = run->base;
		if (caller == 0U)
			return VM_THROWN;

		/* The caller's call instruction is where the search goes on. */
		run->base = caller - 1U;
		function = interpreter_function(realm, run->base);
		run->code = function->code;
	}
}

/* Runs one instruction of spreading and destructuring. */
static int
interpreter_spread(
	struct interpreter *run,
	const uint32_t *words,
	vm_value *registers)
{
	struct vm_realm *realm;
	vm_value value;
	int done;
	int outcome;
	int how;
	int status;

	/* Each instruction; one that computes a value writes it only when it succeeds. */
	realm = run->realm;
	switch (words[0]) {
	case VM_OP_DELEGATE_START:
		status = vm_delegate_start(realm, registers[words[2]], &value);
		break;
	case VM_OP_DELEGATE_STEP:
		/* Decode the completion before invoking user iterator methods. */
		how = vm_value_as_int32(registers[words[4]]);
		status = vm_delegate_step(realm, registers[words[2]], registers[words[3]], how, &value, &outcome);
		if (status != 0)
			return status;

		/* Publish both outputs only after callbacks and getters have finished. */
		registers = &realm->stack[run->base + FRAME_HEADER];
		registers[words[5]] = vm_value_int32(outcome);
		break;
	case VM_OP_ITER_START:
		status = vm_iter_start(realm, registers[words[2]], &value);
		break;
	case VM_OP_ASYNC_ITER_START:
		status = vm_async_iter_start(realm, registers[words[3]], &value, &done);
		if (status != 0)
			return status;

		/* Both outputs are written once the methods called have returned. */
		registers = &realm->stack[run->base + FRAME_HEADER];
		registers[words[2]] = vm_value_boolean(done);
		break;
	case VM_OP_CHECK_OBJECT:
		/* An iterator's result must be an object. */
		if (!vm_value_is_object(registers[words[1]])) {
			status = vm_throw_type_error(realm, "Iterator result is not an object");
			return status;
		}

		/* Any object will do. */
		return 0;
	case VM_OP_ITER_NEXT:
		status = vm_iter_next(realm, registers[words[2]], &value, &done);
		break;
	case VM_OP_ITER_REST:
		status = vm_iter_rest(realm, registers[words[2]], &value);
		break;
	case VM_OP_ITER_CLOSE:
		status = vm_iter_close(realm, registers[words[1]], (int)words[2]);
		return status;
	case VM_OP_ARRAY_SPREAD:
		status = vm_array_spread(realm, registers[words[1]], registers[words[2]]);
		return status;
	case VM_OP_COPY_DATA:
		status = vm_copy_data_properties(realm, registers[words[1]], registers[words[2]]);
		return status;
	case VM_OP_OBJECT_REST:
		status = vm_object_rest(realm, registers[words[2]], registers[words[3]], &value);
		break;
	case VM_OP_CALL_ARRAY:
		status = vm_call_array(realm, registers[words[2]], registers[words[3]], registers[words[4]], 0, &value);
		break;
	case VM_OP_CONSTRUCT_ARRAY:
		status = vm_call_array(realm, registers[words[2]], VM_VALUE_UNDEFINED, registers[words[3]], 1, &value);
		break;
	case VM_OP_CHECK_COERCIBLE:
		/* An object pattern cannot take its properties from undefined or null. */
		if (registers[words[1]] == VM_VALUE_UNDEFINED || registers[words[1]] == VM_VALUE_NULL) {
			status = vm_throw_type_error(realm, "Cannot destructure a value that is undefined or null.");
			return status;
		}

		/* Any other value can be destructured. */
		return 0;
	case VM_OP_ARGS_REST:
		status = interpreter_args_rest(realm, registers[words[2]], words[3], &value);
		break;
	default:
		return EINVAL;
	}

	/* A failed instruction writes nothing. */
	if (status != 0)
		return status;

	/* Succeeded: the value in the first operand's register. */
	registers[words[1]] = value;
	return 0;
}

/* Runs one instruction of classes. */
static int
interpreter_class(
	struct interpreter *run,
	const uint32_t *words,
	vm_value *registers)
{
	struct vm_realm *realm;
	struct vm_function *function;
	struct vm_symbol *symbol;
	vm_value value;
	int status;

	/* Each instruction; one that computes a value writes it only when it succeeds. */
	realm = run->realm;
	switch (words[0]) {
	case VM_OP_LOAD_NEW_TARGET:
		value = realm->stack[run->base + FRAME_NEW_TARGET];
		status = 0;
		break;
	case VM_OP_CLASS_SETUP:
		status = vm_class_setup(realm, registers[words[2]], registers[words[3]], &value);
		break;
	case VM_OP_DEFINE_METHOD:
		status = vm_define_method(realm, registers[words[1]], registers[words[2]], registers[words[3]], words[4]);
		return status;
	case VM_OP_LOAD_HOME:
		/* The running method's home object, in its function's data. */
		function = interpreter_function(realm, run->base);
		value = function->data;
		status = 0;
		break;
	case VM_OP_GET_SUPER:
		status = vm_get_super(realm, registers[words[2]], registers[words[3]], registers[words[4]], &value);
		break;
	case VM_OP_SUPER_CONSTRUCT:
		status = interpreter_super_construct(run, &registers[words[2]], words[3], &value);
		break;
	case VM_OP_SUPER_CONSTRUCT_ARRAY:
		/* Copied argument values stay rooted across later element getters. */
		status = interpreter_super_construct_array(run, registers[words[2]], &value);
		break;
	case VM_OP_NEW_PRIVATE_NAME:
		/* A symbol marked as a private name, described by the constant. */
		symbol = vm_symbol_create(realm->heap, run->code->constants[words[2]]);
		if (symbol == NULL)
			return ENOMEM;
		symbol->private_name = 1;
		value = vm_value_cell(symbol);
		status = 0;
		break;
	case VM_OP_PRIVATE_GET:
		status = vm_private_get(realm, registers[words[2]], registers[words[3]], &value);
		break;
	case VM_OP_PRIVATE_SET:
		status = vm_private_set(realm, registers[words[1]], registers[words[2]], registers[words[3]]);
		return status;
	case VM_OP_PRIVATE_DEFINE:
		status = vm_private_define(realm, registers[words[1]], registers[words[2]], registers[words[3]]);
		return status;
	case VM_OP_PRIVATE_COPY:
		status = vm_private_copy(realm, registers[words[1]], registers[words[2]], registers[words[3]]);
		return status;
	case VM_OP_PRIVATE_IN:
		status = vm_private_in(realm, registers[words[2]], registers[words[3]], &value);
		break;
	default:
		return EINVAL;
	}

	/* A failed instruction writes nothing. */
	if (status != 0)
		return status;

	/* Succeeded: the value in the first operand's register. */
	registers[words[1]] = value;
	return 0;
}

/*
 * Runs super(...) in a derived class's constructor: the parent (the
 * constructor's own prototype) constructs with the frame's new.target, and
 * the object it makes becomes this, which may be bound only once.
 */
static int
interpreter_super_construct(
	struct interpreter *run,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_realm *realm;
	struct vm_function *function;
	vm_value parent;
	vm_value new_target;
	vm_value made;
	int is_constructor;
	int status;

	/* The parent, which must construct. */
	realm = run->realm;
	function = interpreter_function(realm, run->base);
	parent = VM_VALUE_NULL;
	if (function->object.prototype != NULL)
		parent = vm_value_cell(function->object.prototype);
	is_constructor = vm_value_is_constructor(parent);
	if (!is_constructor) {
		status = vm_throw_type_error(realm, "Super constructor is not a constructor");
		return status;
	}

	/* The construction, with this constructor's new.target. */
	new_target = realm->stack[run->base + FRAME_NEW_TARGET];
	status = vm_construct(realm, parent, args, count, new_target, &made);
	if (status != 0)
		return status;

	/* this is bound once. */
	if (realm->stack[run->base + FRAME_THIS] != VM_VALUE_EMPTY) {
		status = vm_throw_reference_error(realm, "Super constructor may only be called once");
		return status;
	}

	/* this is bound now. */
	realm->stack[run->base + FRAME_THIS] = made;

	/* Succeeded: super() is this. */
	*result = made;
	return 0;
}

/* Constructs super from indexed array values while retaining earlier getter results. */
static int
interpreter_super_construct_array(
	struct interpreter *run,
	vm_value array_value,
	vm_value *result)
{
	struct vm_object *array;
	struct vm_cell **roots;
	vm_value *args;
	size_t slots;
	uint32_t count;
	uint32_t index;
	uint32_t registered;
	int is_cell;
	int status;

	/* Reserves native argument and collector-root arrays before invoking getters. */
	array = (struct vm_object *)vm_value_as_cell(array_value);
	count = array->length;
	slots = (size_t)count + 1U;
	if (slots > SIZE_MAX / sizeof(*args) || slots > SIZE_MAX / sizeof(*roots))
		return EOVERFLOW;
	args = calloc(slots, sizeof(*args));
	if (args == NULL)
		return ENOMEM;
	roots = calloc(slots, sizeof(*roots));
	if (roots == NULL) {
		free(args);
		return ENOMEM;
	}

	/* Every native slot survives later property getters and construction. */
	registered = 0;
	status = 0;
	for (index = 0; index < count; index++) {
		status = vm_heap_add_root(run->realm->heap, &roots[index]);
		if (status != 0)
			goto cleanup;
		registered++;
	}

	/* Getters may execute script and collect values already copied out. */
	for (index = 0; index < count; index++) {
		status = vm_get(run->realm, array_value, vm_value_int32((int32_t)index), &args[index]);
		if (status != 0)
			goto cleanup;
		is_cell = vm_value_is_cell(args[index]);
		if (is_cell)
			roots[index] = vm_value_as_cell(args[index]);
	}

	/* The native constructor boundary receives every protected argument. */
	status = interpreter_super_construct(run, args, count, result);

cleanup:
	/* Stops retaining copied cells after the super call returns. */
	while (registered != 0U) {
		registered--;
		vm_heap_remove_root(run->realm->heap, &roots[registered]);
	}

	/* Native storage is no longer needed after the protected call. */
	free(roots);
	free(args);
	if (status != 0)
		return status;

	/* Succeeded: the result was published to the active constructor frame. */
	return 0;
}

/* Makes the array of a rest parameter: the arguments object's elements from a first index. */
static int
interpreter_args_rest(
	struct vm_realm *realm,
	vm_value arguments,
	uint32_t first,
	vm_value *result)
{
	struct vm_cell *iterator_root;
	vm_value iterator;
	vm_value value;
	uint32_t index;
	int done;
	int status;

	/* The arguments in order, skipping those before the first. */
	status = vm_iter_start(realm, arguments, &iterator);
	if (status != 0)
		return status;
	iterator_root = vm_value_as_cell(iterator);
	status = vm_heap_add_root(realm->heap, &iterator_root);
	if (status != 0)
		return status;
	for (index = 0; index < first; index++) {
		status = vm_iter_next(realm, iterator, &value, &done);
		if (status != 0)
			goto cleanup;
	}

	/* The rest as an array. */
	status = vm_iter_rest(realm, iterator, result);

cleanup:
	/* The iterator is no longer needed after exhaustion or failure. */
	vm_heap_remove_root(realm->heap, &iterator_root);
	if (status != 0)
		return status;

	/* Succeeded: the array. */
	return 0;
}

/* Throws a new error of a kind (enum vm_error_kind) with a message (a string constant). */
static int
interpreter_throw_error(
	struct vm_realm *realm,
	uint32_t kind,
	vm_value message)
{
	struct wb_buffer text;
	int status;

	/* The message as UTF-8. */
	wb_buffer_init(&text);
	status = vm_string_to_utf8((struct vm_string *)vm_value_as_cell(message), &text);
	if (status != 0) {
		wb_buffer_release(&text);
		return status;
	}

	/* The error. */
	status = vm_throw_error(realm, (int)kind, wb_buffer_string(&text));
	wb_buffer_release(&text);

	/* Reports the throw. */
	return status;
}

/* Reports the function a frame runs. */
static struct vm_function *
interpreter_function(
	const struct vm_realm *realm,
	uint32_t base)
{
	/* The header's function slot holds its cell. */
	return (struct vm_function *)vm_value_as_cell(realm->stack[base + FRAME_FUNCTION]);
}

/* Tells whether the running code is strict mode code. */
static int
interpreter_is_strict(
	const struct interpreter *run)
{
	/* The code unit's flag. */
	if ((run->code->flags & VM_CODE_STRICT) != 0U)
		return 1;

	/* Sloppy code. */
	return 0;
}

/* Reads a raw f64's bits as a double. */
static double
interpreter_f64(
	vm_value bits)
{
	double number;

	/* The same 64 bits. */
	memcpy(&number, &bits, sizeof(number));

	/* Reports the double. */
	return number;
}

/* Writes a double as a raw f64's bits. */
static vm_value
interpreter_bits(
	double number)
{
	vm_value bits;

	/* The same 64 bits. */
	memcpy(&bits, &number, sizeof(bits));

	/* Reports the bits. */
	return bits;
}

/* Records where each frame stands, from the throwing frame outwards (line:column of its running instruction), for a developer's diagnosis. */
static void
interpreter_note_stack(
	const struct interpreter *run)
{
	struct vm_realm *realm;
	struct vm_function *function;
	const struct vm_code *code;
	uint32_t base;
	uint32_t caller;
	uint32_t pc;
	uint32_t line;
	uint32_t column;
	size_t used;
	unsigned depth;
	int written;

	realm = run->realm;
	realm->throw_stack[0] = '\0';
	used = 0;
	base = run->base;
	code = run->code;
	pc = run->pc;
	for (depth = 0; depth < 12U; depth++) {
		line = 0;
		column = 0;
		vm_code_position(code, pc, &line, &column);
		written = snprintf(realm->throw_stack + used, sizeof(realm->throw_stack) - used, "%s%u:%u", used == 0 ? "" : " < ", (unsigned)line, (unsigned)column);
		if (written < 0 || (size_t)written >= sizeof(realm->throw_stack) - used)
			break;
		used += (size_t)written;
		caller = (uint32_t)realm->stack[base + FRAME_CALLER];
		if (caller == 0U)
			break;
		pc = (uint32_t)realm->stack[base + FRAME_RETURN] - INTERPRETER_CALL_WORDS;
		base = caller - 1U;
		function = interpreter_function(realm, base);
		code = function->code;
	}
	if (getenv("BROWSER_STACK_ALL") != NULL)
		fprintf(stderr, "throw: %s\n", realm->throw_stack);
}
