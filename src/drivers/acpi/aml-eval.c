/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The core of the AML evaluator: term lists, control flow, TermArgs,
 * targets and the Store rules, and method invocation.
 *
 * The evaluator walks the AML bytes directly by recursive descent; nothing
 * is parsed ahead.  A method body is interpreted from its first byte each
 * time the method runs.  C recursion follows the nesting of the AML, so
 * every recursive step checks the stack budget (drv_acpi_stack_check()).
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "aml-internal.h"
#include "aml-os.h"

/*
 * How long one While loop may run, in 100-nanosecond timer units: thirty
 * seconds, after which firmware that waits for hardware that never answers
 * is abandoned.
 */
#define WHILE_TIMEOUT_TICKS (30ULL * 10000000ULL)

/*
 * The deepest the stack has been below an entry's frame, for measurement.
 * It only grows, and only the holder of the interpreter lock writes it.
 */
static size_t stack_deepest;


static int evaluate_node(struct drv_acpi_thread *thread, struct drv_acpi_node *node, struct drv_acpi_object **arguments, unsigned argument_count, struct drv_acpi_object **result);
static int eval_opcode_term(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int reference_data(struct drv_acpi_eval *eval, struct drv_acpi_object *reference, struct drv_acpi_object **result);
static int parse_name_target(struct drv_acpi_eval *eval, struct drv_acpi_target *target);
static int parse_opcode_target(struct drv_acpi_eval *eval, struct drv_acpi_target *target);
static int store_argument(struct drv_acpi_eval *eval, unsigned index, struct drv_acpi_object *value);
static int invoke_body(struct drv_acpi_eval *caller, struct drv_acpi_node *node, struct drv_acpi_object **arguments, unsigned argument_count, struct drv_acpi_object **result);
static int nothing_as_zero(struct drv_acpi_object **result);
static int package_element_read(struct drv_acpi_eval *eval, struct drv_acpi_object *package, uint32_t index, struct drv_acpi_object **result);
static int exec_term(struct drv_acpi_eval *eval);
static int exec_name_term(struct drv_acpi_eval *eval);
static int exec_opcode_term(struct drv_acpi_eval *eval);
static int exec_expression(struct drv_acpi_eval *eval, const uint8_t *start);
static int exec_block(struct drv_acpi_eval *eval, const uint8_t *block_end);
static int exec_if(struct drv_acpi_eval *eval);
static bool else_holds_if_alone(struct drv_acpi_eval *eval, const uint8_t *else_end);
static int __attribute__((noinline)) exec_while(struct drv_acpi_eval *eval);
static int __attribute__((noinline)) exec_return(struct drv_acpi_eval *eval);
static int eval_name(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int eval_method_call(struct drv_acpi_eval *eval, struct drv_acpi_node *node, struct drv_acpi_object **result);
static int eval_constant(struct drv_acpi_eval *eval, unsigned opcode, struct drv_acpi_object **result);
static int eval_string(struct drv_acpi_eval *eval, struct drv_acpi_object **result);
static int eval_local(struct drv_acpi_eval *eval, unsigned opcode, struct drv_acpi_object **result);
static int store_local(struct drv_acpi_object **slot, struct drv_acpi_object *value);
static int store_node(struct drv_acpi_eval *eval, struct drv_acpi_node *node, struct drv_acpi_object *value);
static int store_uninitialized(struct drv_acpi_node *node, struct drv_acpi_object *value);
static int store_reference(struct drv_acpi_eval *eval, struct drv_acpi_object *reference, struct drv_acpi_object *value);
static int store_index(struct drv_acpi_object *reference, struct drv_acpi_object *value);
static int store_element(struct drv_acpi_object *package, uint32_t index, struct drv_acpi_object *value);
static int store_buffer_into(struct drv_acpi_object *target, struct drv_acpi_object *value);
static int parse_reference_target(struct drv_acpi_eval *eval, unsigned opcode, struct drv_acpi_target *target);
static int deref_target(struct drv_acpi_eval *eval, struct drv_acpi_target *target);
static void debug_store(struct drv_acpi_object *value);
static void frame_release(struct drv_acpi_frame *frame);
static int serialize(struct drv_acpi_eval *eval, struct drv_acpi_object *method, bool acquire);
static void report_name(const char *what, const struct drv_acpi_name *name, int error);

/*
 * Evaluates a named object for a driver.
 *
 * A method is invoked with the arguments, anything else is read.  path is
 * relative to scope, or NULL for scope itself.  The result is NULL when a
 * method returned nothing.
 */
int
drv_acpi_evaluate(
	struct drv_acpi_node *scope,
	const char *path,
	struct drv_acpi_object **arguments,
	unsigned argument_count,
	struct drv_acpi_object **result)
{
	struct drv_acpi_thread storage;
	struct drv_acpi_thread *thread;
	struct drv_acpi_node *node;
	int error;

	/* Starts with no result, which a method that returns nothing leaves. */
	*result = NULL;

	/* Enters the interpreter, measuring the stack from here. */
	thread = drv_acpi_enter(&storage);

	/* Finds the object: the scope itself, or the path from it. */
	node = scope;
	error = 0;
	if (path != NULL)
		error = drv_acpi_lookup(scope, path, &node);

	/* A missing scope without a path names nothing. */
	if (error == 0 && node == NULL)
		error = ENOENT;

	/* Invokes a method or reads anything else, as the entered thread. */
	if (error == 0)
		error = evaluate_node(thread, node, arguments, argument_count, result);

	/* Leaves the interpreter. */
	drv_acpi_leave(thread);

	/* Reports a failed evaluation. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the result, if any. */
	return 0;
}

/*
 * Evaluates a named object that must produce an integer.
 */
int
drv_acpi_evaluate_integer(
	struct drv_acpi_node *scope,
	const char *path,
	uint64_t *value)
{
	struct drv_acpi_object *object;
	int error;

	/* Evaluates the object without arguments. */
	error = drv_acpi_evaluate(scope, path, NULL, 0, &object);
	if (error != 0)
		return error;

	/* Refuses a method that returned nothing. */
	if (object == NULL)
		return EINVAL;

	/* Converts the value by the implicit rules. */
	error = drv_acpi_convert_integer(object, value);
	drv_acpi_object_release(object);
	if (error != 0)
		return error;

	/* Succeeded: value holds the integer. */
	return 0;
}

/*
 * Runs the terms of the current term list until its end or a control transfer.
 */
int
drv_acpi_exec_term_list(
	struct drv_acpi_eval *eval)
{
	int error;

	/* Runs one term at a time while nothing has transferred control. */
	while (eval->position < eval->end) {
		/* Runs the next term. */
		error = exec_term(eval);
		if (error != 0)
			return error;

		/* Stops when a Return, Break or Continue leaves this list. */
		if (eval->control != DRV_ACPI_CONTROL_NEXT)
			return 0;
	}

	/* Succeeded: the list ran to its end. */
	return 0;
}

/*
 * Runs a term list in the scope of another node, then continues after it.
 *
 * The body of a Scope or a Device runs this way; it ends at end.
 */
int
drv_acpi_exec_scope(
	struct drv_acpi_eval *eval,
	struct drv_acpi_node *scope,
	const uint8_t *end)
{
	struct drv_acpi_node *outer_scope;
	int error;

	/* Runs the list with the node as the scope. */
	outer_scope = eval->scope;
	eval->scope = scope;
	error = exec_block(eval, end);
	eval->scope = outer_scope;
	if (error != 0)
		return error;

	/* Succeeded: the list ran in the node's scope. */
	return 0;
}

/*
 * Evaluates one TermArg and reports the object it produced.
 *
 * The caller receives one reference.  A name that resolves to a method is
 * invoked; a name that resolves to a field is read.
 */
int
drv_acpi_eval_term_arg(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	bool named;
	int error;

	/* Refuses to go deeper than the stack budget allows. */
	error = drv_acpi_stack_check();
	if (error != 0)
		return error;

	/* A name is a reference to an object or a method invocation; anything else starts with an opcode. */
	named = drv_acpi_stream_at_name(eval);
	if (named) {
		error = eval_name(eval, result);
	} else {
		error = eval_opcode_term(eval, result);
	}

	/* Reports a TermArg that could not be evaluated. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the value. */
	return 0;
}

/*
 * Evaluates a TermArg and converts its value to an integer.
 */
int
drv_acpi_eval_integer(
	struct drv_acpi_eval *eval,
	uint64_t *value)
{
	struct drv_acpi_object *object;
	int error;

	/* Evaluates the operand as data. */
	error = drv_acpi_eval_data(eval, &object);
	if (error != 0)
		return error;

	/* Converts it by the implicit rules. */
	error = drv_acpi_convert_integer(object, value);
	drv_acpi_object_release(object);
	if (error != 0)
		return error;

	/* Succeeded: value holds the integer. */
	return 0;
}

/*
 * Evaluates a TermArg and resolves references to the data they point at.
 *
 * An operand that computes (Add, Concatenate, ...) needs the value an
 * Index reference or a local's object reference stands for, not the
 * reference itself.
 */
int
drv_acpi_eval_data(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *object;
	struct drv_acpi_object *resolved;
	int error;

	/* Evaluates the operand. */
	error = drv_acpi_eval_term_arg(eval, &object);
	if (error != 0)
		return error;

	/* Anything but a reference is data already; a reference is resolved and let go. */
	if (object->type != DRV_ACPI_TYPE_REFERENCE) {
		resolved = object;
		error = 0;
	} else {
		resolved = NULL;
		error = reference_data(eval, object, &resolved);
		drv_acpi_object_release(object);
	}

	/* Reports a reference that could not be resolved. */
	if (error != 0)
		return error;

	/* Hands over the data. */
	*result = resolved;

	/* Succeeded: the caller holds the value. */
	return 0;
}

/*
 * Parses a SuperName or Target: where a result is to be stored.
 */
int
drv_acpi_parse_target(
	struct drv_acpi_eval *eval,
	struct drv_acpi_target *target)
{
	bool named;
	int error;

	/* Starts with a target that stores nowhere. */
	kern_memset(target, 0, sizeof(*target));

	/* A name is a namespace node; anything else starts with an opcode or is the null name. */
	named = drv_acpi_stream_at_name(eval);
	if (named) {
		error = parse_name_target(eval, target);
	} else {
		error = parse_opcode_target(eval, target);
	}

	/* Reports a target that could not be parsed. */
	if (error != 0)
		return error;

	/* Succeeded: target says where the result goes. */
	return 0;
}

/*
 * Releases what a parsed target holds.
 */
void
drv_acpi_target_release(
	struct drv_acpi_target *target)
{
	/* Only a reference target holds anything. */
	drv_acpi_object_release(target->reference);
	target->reference = NULL;
}

/*
 * Stores a value into a target by the rules of the Store operator.
 */
int
drv_acpi_store(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *value,
	struct drv_acpi_target *target)
{
	int error;

	/* Chooses the store by where the target is. */
	switch (target->kind) {
	case DRV_ACPI_TARGET_NONE:
		/* A null target discards the value. */
		error = 0;
		break;
	case DRV_ACPI_TARGET_DEBUG:
		/* The debug object logs the value. */
		debug_store(value);
		error = 0;
		break;
	case DRV_ACPI_TARGET_LOCAL:
		/* A local takes a copy of the value. */
		error = store_local(&eval->frame->locals[target->index], value);
		break;
	case DRV_ACPI_TARGET_ARGUMENT:
		/* An argument is stored through or replaced. */
		error = store_argument(eval, target->index, value);
		break;
	case DRV_ACPI_TARGET_NODE:
		/* A named object converts the value to its own type. */
		error = store_node(eval, target->node, value);
		break;
	case DRV_ACPI_TARGET_REFERENCE:
		/* A reference is stored through. */
		error = store_reference(eval, target->reference, value);
		break;
	default:
		/* Refuses a target kind that does not exist. */
		error = EINVAL;
		break;
	}

	/* Reports a failed store. */
	if (error != 0)
		return error;

	/* Succeeded: the target holds the value. */
	return 0;
}

/*
 * Invokes a control method.
 *
 * The arguments are shared with the invocation, which adds its own
 * references.  The result is what the method returned, or NULL when it
 * returned nothing.
 */
int
drv_acpi_invoke(
	struct drv_acpi_eval *caller,
	struct drv_acpi_node *node,
	struct drv_acpi_object **arguments,
	unsigned argument_count,
	struct drv_acpi_object **result)
{
	struct drv_acpi_method *method;
	int error;

	/* Starts with no result, which a method that returns nothing leaves. */
	*result = NULL;

	/* Refuses anything but a method. */
	if (node->object == NULL || node->object->type != DRV_ACPI_TYPE_METHOD)
		return EINVAL;
	method = &node->object->value.method;

	/* Refuses more arguments than a method can have. */
	if (argument_count > DRV_ACPI_ARGUMENT_COUNT)
		return EINVAL;

	/* A native method runs in C; any other runs its AML body. */
	if (method->native != NULL) {
		error = method->native(caller, arguments, argument_count, result);
	} else {
		error = invoke_body(caller, node, arguments, argument_count, result);
	}

	/* Reports a failed invocation. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the result, if any. */
	return 0;
}

/*
 * Reads the value of a named object.
 *
 * A field is read from its region, a method is invoked without arguments,
 * and anything else is shared.
 */
int
drv_acpi_read_node(
	struct drv_acpi_eval *eval,
	struct drv_acpi_node *node,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *object;
	int error;

	/* Reads through an alias. */
	node = drv_acpi_ns_resolve_alias(node);
	object = node->object;

	/* A node without an object has no value. */
	if (object == NULL)
		return EINVAL;

	/* Chooses the read by the kind of object. */
	switch (object->type) {
	case DRV_ACPI_TYPE_FIELD_UNIT:
		/* Reads the field from its region. */
		error = drv_acpi_field_read(eval, object, result);
		break;
	case DRV_ACPI_TYPE_BUFFER_FIELD:
		/* Reads the field from its buffer. */
		error = drv_acpi_buffer_field_read(object, result);
		break;
	case DRV_ACPI_TYPE_METHOD:
		/* Invokes the method; one that returned nothing reads as zero. */
		error = drv_acpi_invoke(eval, node, NULL, 0, result);
		if (error != 0)
			break;
		error = nothing_as_zero(result);
		break;
	default:
		/* Shares any other object with the caller. */
		drv_acpi_object_ref(object);
		*result = object;
		error = 0;
		break;
	}

	/* Reports a value that could not be read. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the value. */
	return 0;
}

/*
 * Reads the element an index reference points at.
 *
 * A package element is shared, and one that names an object reads as that
 * object's value, as the reference interpreter does.  A byte of a buffer or
 * a character of a string becomes an integer.  An element that was never
 * set is an error.
 */
int
drv_acpi_index_read(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *reference,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *container;
	struct drv_acpi_object *element;
	uint32_t index;
	int error;

	/* Finds the container and the element. */
	container = reference->value.reference.target;
	index = reference->value.reference.index;

	/* Reads a package element, or makes a byte or a character an integer. */
	element = NULL;
	if (container->type == DRV_ACPI_TYPE_PACKAGE) {
		error = package_element_read(eval, container, index, &element);
	} else if (container->type == DRV_ACPI_TYPE_BUFFER) {
		element = drv_acpi_object_integer_new(container->value.buffer.bytes[index]);
		error = 0;
		if (element == NULL)
			error = ENOMEM;
	} else {
		element = drv_acpi_object_integer_new((uint8_t)container->value.string.text[index]);
		error = 0;
		if (element == NULL)
			error = ENOMEM;
	}

	/* Reports an element that could not be read. */
	if (error != 0)
		return error;

	/* Hands over the element. */
	*result = element;

	/* Succeeded: the caller holds the element's value. */
	return 0;
}

/*
 * Refuses to recurse further when the stack budget is spent.
 */
int
drv_acpi_stack_check(void)
{
	struct drv_acpi_thread *thread;
	uintptr_t stack_base;
	uintptr_t here;
	size_t used;
	size_t budget;

	/* Nothing is measured outside an entry. */
	thread = drv_acpi_active_thread();
	if (thread == NULL || thread->stack_base == 0)
		return 0;

	/* Takes the frame the entry measures from. */
	stack_base = thread->stack_base;

	/*
	 * Measures from the outermost evaluation down to this frame.  The
	 * frame address is the machine stack even when a sanitizer moves
	 * locals elsewhere.
	 */
	here = (uintptr_t)__builtin_frame_address(0);
	if (here < stack_base) {
		used = (size_t)(stack_base - here);
	} else {
		used = (size_t)(here - stack_base);
	}

	/* Keeps the deepest point for measurement. */
	if (used > stack_deepest)
		stack_deepest = used;

	/* Refuses to go on past the budget. */
	budget = drv_acpi_os_stack_budget();
	if (used > budget) {
		drv_acpi_os_log("ACPI: AML nests deeper than the stack budget\n");
		return E2BIG;
	}

	/* Succeeded: there is room to go deeper. */
	return 0;
}

/*
 * Reports the deepest stack use measured so far.
 */
size_t
drv_acpi_stack_deepest(void)
{
	/* Reports the deepest stack use in bytes. */
	return stack_deepest;
}

/* Invokes a method or reads any other object for a driver's evaluation. */
static int
evaluate_node(
	struct drv_acpi_thread *thread,
	struct drv_acpi_node *node,
	struct drv_acpi_object **arguments,
	unsigned argument_count,
	struct drv_acpi_object **result)
{
	struct drv_acpi_eval entry;
	int error;

	/* Prepares an evaluation of the entered thread. */
	node = drv_acpi_ns_resolve_alias(node);
	kern_memset(&entry, 0, sizeof(entry));
	entry.thread = thread;

	/* Invokes a method; reads anything else. */
	if (node->object != NULL && node->object->type == DRV_ACPI_TYPE_METHOD) {
		error = drv_acpi_invoke(&entry, node, arguments, argument_count, result);
	} else {
		error = drv_acpi_read_node(&entry, node, result);
	}

	/* Reports a failed evaluation. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the result, if any. */
	return 0;
}

/* Evaluates a TermArg that starts with an opcode. */
static int
eval_opcode_term(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	const uint8_t *start;
	unsigned opcode;
	int error;

	/* Reads the opcode, keeping its start for the log of an unknown one. */
	start = eval->position;
	error = drv_acpi_stream_opcode(eval, &opcode);
	if (error != 0)
		return error;

	/* Chooses the evaluation by the kind of opcode. */
	switch (opcode) {
	case DRV_ACPI_OP_ZERO:
	case DRV_ACPI_OP_ONE:
	case DRV_ACPI_OP_ONES:
	case DRV_ACPI_OP_BYTE_PREFIX:
	case DRV_ACPI_OP_WORD_PREFIX:
	case DRV_ACPI_OP_DWORD_PREFIX:
	case DRV_ACPI_OP_QWORD_PREFIX:
	case DRV_ACPI_OP_STRING_PREFIX:
	case DRV_ACPI_OP_REVISION:
		/* Makes a constant. */
		error = eval_constant(eval, opcode, result);
		break;
	case DRV_ACPI_OP_BUFFER:
		/* Builds a Buffer literal. */
		error = drv_acpi_build_buffer(eval, result);
		break;
	case DRV_ACPI_OP_PACKAGE:
		/* Builds a Package literal. */
		error = drv_acpi_build_package(eval, false, result);
		break;
	case DRV_ACPI_OP_VAR_PACKAGE:
		/* Builds a VarPackage literal. */
		error = drv_acpi_build_package(eval, true, result);
		break;
	default:
		/* Locals and arguments hold objects of the running invocation. */
		if (opcode >= DRV_ACPI_OP_LOCAL0 && opcode <= DRV_ACPI_OP_ARG6) {
			error = eval_local(eval, opcode, result);
			break;
		}

		/* Everything else is an operator that computes a value. */
		error = drv_acpi_eval_operator(eval, opcode, result);
		if (error == ENOSYS) {
			/* No operator has the opcode: the table is damaged or newer than the interpreter. */
			drv_acpi_os_log(
				"ACPI: unknown opcode 0x%x at offset 0x%lx\n",
				opcode,
				(unsigned long)(start - eval->table->data));
			error = EIO;
		}

		break;
	}

	/* Reports a TermArg that could not be evaluated. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the value. */
	return 0;
}

/* Resolves a reference to the data it points at. */
static int
reference_data(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *reference,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *resolved;
	int error;

	/* Resolves the reference by its kind. */
	resolved = NULL;
	switch (reference->value.reference.kind) {
	case DRV_ACPI_REFERENCE_OBJECT:
		/* Shares the object the reference holds. */
		resolved = reference->value.reference.target;
		drv_acpi_object_ref(resolved);
		error = 0;
		break;
	case DRV_ACPI_REFERENCE_INDEX:
		/* Reads the element the index names. */
		error = drv_acpi_index_read(eval, reference, &resolved);
		break;
	case DRV_ACPI_REFERENCE_NODE:
		/* Reads the node. */
		error = drv_acpi_read_node(eval, reference->value.reference.node, &resolved);
		break;
	default:
		/* Refuses a name reference that never resolved. */
		error = EINVAL;
		break;
	}

	/* Reports a reference that could not be resolved. */
	if (error != 0)
		return error;

	/* Hands over the data. */
	*result = resolved;

	/* Succeeded: the caller holds the data. */
	return 0;
}

/*
 * Parses a name target.  A method named here is not invoked: ObjectType,
 * SizeOf, RefOf and the stores all name it as an object.
 */
static int
parse_name_target(
	struct drv_acpi_eval *eval,
	struct drv_acpi_target *target)
{
	struct drv_acpi_name name;
	struct drv_acpi_node *node;
	int error;

	/* Reads the name. */
	error = drv_acpi_stream_name(eval, &name);
	if (error != 0)
		return error;

	/* Resolves the name with the search rules of references. */
	error = drv_acpi_ns_lookup(eval->scope, &name, true, &node);
	if (error != 0) {
		report_name("target", &name, error);
		return error;
	}

	/* Makes the node the target; an alias stands for the node it names. */
	target->kind = DRV_ACPI_TARGET_NODE;
	target->node = drv_acpi_ns_resolve_alias(node);

	/* Succeeded: the target is the node. */
	return 0;
}

/* Parses a target that starts with an opcode, or the null name. */
static int
parse_opcode_target(
	struct drv_acpi_eval *eval,
	struct drv_acpi_target *target)
{
	unsigned opcode;
	uint8_t byte;
	int error;

	/* Looks at the first byte. */
	error = drv_acpi_stream_peek(eval, &byte);
	if (error != 0)
		return error;

	/* The null name discards the result. */
	if (byte == 0x00U) {
		eval->position++;
		return 0;
	}

	/* Reads the opcode of any other target. */
	error = drv_acpi_stream_opcode(eval, &opcode);
	if (error != 0)
		return error;

	/* A local or an argument of the running invocation, the debug object, or a reference operator. */
	error = 0;
	if (opcode >= DRV_ACPI_OP_LOCAL0 && opcode <= DRV_ACPI_OP_LOCAL7) {
		target->kind = DRV_ACPI_TARGET_LOCAL;
		target->index = opcode - DRV_ACPI_OP_LOCAL0;
	} else if (opcode >= DRV_ACPI_OP_ARG0 && opcode <= DRV_ACPI_OP_ARG6) {
		target->kind = DRV_ACPI_TARGET_ARGUMENT;
		target->index = opcode - DRV_ACPI_OP_ARG0;
	} else if (opcode == DRV_ACPI_OP_DEBUG) {
		/* The debug object logs what is stored into it. */
		target->kind = DRV_ACPI_TARGET_DEBUG;
	} else {
		/* RefOf, DerefOf and Index produce a reference to store through. */
		error = parse_reference_target(eval, opcode, target);
	}

	/* Reports a target that could not be parsed. */
	if (error != 0)
		return error;

	/* Refuses a local or an argument outside any method. */
	if (eval->frame == NULL &&
	    (target->kind == DRV_ACPI_TARGET_LOCAL ||
	     target->kind == DRV_ACPI_TARGET_ARGUMENT))
		return EINVAL;

	/* Succeeded: target says where the result goes. */
	return 0;
}

/* Stores into an argument of the running invocation. */
static int
store_argument(
	struct drv_acpi_eval *eval,
	unsigned index,
	struct drv_acpi_object *value)
{
	struct drv_acpi_object *argument;
	int error;

	/* An argument that holds a reference is stored through; any other is replaced like a local. */
	argument = eval->frame->arguments[index];
	if (argument != NULL && argument->type == DRV_ACPI_TYPE_REFERENCE) {
		error = store_reference(eval, argument, value);
	} else {
		error = store_local(&eval->frame->arguments[index], value);
	}

	/* Reports a failed store. */
	if (error != 0)
		return error;

	/* Succeeded: the argument, or what it refers to, holds the value. */
	return 0;
}

/* Runs the AML body of a method in a new invocation. */
static int
invoke_body(
	struct drv_acpi_eval *caller,
	struct drv_acpi_node *node,
	struct drv_acpi_object **arguments,
	unsigned argument_count,
	struct drv_acpi_object **result)
{
	struct drv_acpi_method *method;
	struct drv_acpi_frame *frame;
	struct drv_acpi_eval eval;
	unsigned depth;
	unsigned index;
	int release_error;
	int error;

	/* Finds the invocation's depth: one below the caller's. */
	method = &node->object->value.method;
	depth = 0;
	if (caller != NULL && caller->frame != NULL)
		depth = caller->frame->depth + 1U;

	/* Refuses an invocation nested deeper than the interpreter allows. */
	if (depth >= DRV_ACPI_CALL_DEPTH_MAX)
		return E2BIG;

	/* Allocates the invocation's locals and arguments. */
	frame = drv_acpi_os_alloc(sizeof(*frame));
	if (frame == NULL)
		return ENOMEM;

	/* Starts the frame empty, with the method, its depth and the caller's frame. */
	kern_memset(frame, 0, sizeof(*frame));
	frame->method = node;
	frame->depth = depth;
	if (caller != NULL)
		frame->caller = caller->frame;

	/* Gives the invocation its own reference to each argument. */
	for (index = 0; index < argument_count; index++) {
		/* Shares the argument; a missing one stays missing. */
		frame->arguments[index] = arguments[index];
		if (arguments[index] != NULL)
			drv_acpi_object_ref(arguments[index]);
	}

	/* Prepares to run the body in the scope of the method's node, as the caller's thread. */
	kern_memset(&eval, 0, sizeof(eval));
	eval.position = method->start;
	eval.end = method->end;
	eval.scope = node;
	eval.frame = frame;
	eval.table = method->table;
	eval.thread = drv_acpi_eval_thread(caller);

	/* Runs the body, under the method's mutex when it is serialized. */
	error = serialize(&eval, node->object, true);
	if (error == 0) {
		error = drv_acpi_exec_term_list(&eval);

		/* Lets the mutex go; a release that fails is logged and fails a body that succeeded. */
		release_error = serialize(&eval, node->object, false);
		if (release_error != 0) {
			drv_acpi_os_log("ACPI: a serialized method's mutex release failed (error %d)\n", release_error);
			if (error == 0)
				error = release_error;
		}
	}

	/* Hands the returned value to the caller, or drops it on failure. */
	if (error == 0) {
		*result = eval.return_value;
	} else {
		drv_acpi_object_release(eval.return_value);
	}

	/* Deletes what the invocation created and releases its objects. */
	frame_release(frame);

	/* Reports a failed body. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the result, if any. */
	return 0;
}

/* Makes a method's missing result an integer zero, which is its value then. */
static int
nothing_as_zero(
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *zero;

	/* A method that returned something keeps it. */
	if (*result != NULL)
		return 0;

	/* Makes the zero. */
	zero = drv_acpi_object_integer_new(0);
	if (zero == NULL)
		return ENOMEM;

	/* Hands it over in place of nothing. */
	*result = zero;

	/* Succeeded: result is the method's value. */
	return 0;
}

/* Reads one element of a package through an index reference. */
static int
package_element_read(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *package,
	uint32_t index,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *element;
	struct drv_acpi_node *node;
	int error;

	/* Refuses an element that was never set. */
	element = package->value.package.elements[index];
	if (element == NULL)
		return EINVAL;

	/* An element that names an object reads as the object's value; any other is shared. */
	node = drv_acpi_object_reference_node(element);
	if (node != NULL) {
		error = drv_acpi_read_node(eval, node, result);
	} else {
		drv_acpi_object_ref(element);
		*result = element;
		error = 0;
	}

	/* Reports an object that could not be read. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the element's value. */
	return 0;
}

/* Runs one term: a definition, a control statement, or an expression. */
static int
exec_term(
	struct drv_acpi_eval *eval)
{
	bool named;
	int error;

	/* Refuses to go deeper than the stack budget allows. */
	error = drv_acpi_stack_check();
	if (error != 0)
		return error;

	/* A name is a method invocation or a reference; anything else starts with an opcode. */
	named = drv_acpi_stream_at_name(eval);
	if (named) {
		error = exec_name_term(eval);
	} else {
		error = exec_opcode_term(eval);
	}

	/* Reports a term that could not run. */
	if (error != 0)
		return error;

	/* Succeeded: the term ran. */
	return 0;
}

/* Runs a term that is a name: a method invocation or a reference whose value is dropped. */
static int
exec_name_term(
	struct drv_acpi_eval *eval)
{
	struct drv_acpi_object *object;
	int error;

	/* Evaluates the name. */
	error = drv_acpi_eval_term_arg(eval, &object);
	if (error != 0)
		return error;

	/* Drops the value. */
	drv_acpi_object_release(object);

	/* Succeeded: the name was evaluated. */
	return 0;
}

/* Runs a term that starts with an opcode: a definition, a statement or an expression. */
static int
exec_opcode_term(
	struct drv_acpi_eval *eval)
{
	const uint8_t *start;
	unsigned opcode;
	bool definition;
	int error;

	/* Reads the opcode, keeping its start to hand it back to an expression. */
	start = eval->position;
	error = drv_acpi_stream_opcode(eval, &opcode);
	if (error != 0)
		return error;

	/* A definition creates a named object. */
	definition = drv_acpi_is_definition(opcode);
	if (definition) {
		error = drv_acpi_define(eval, opcode);
		if (error != 0)
			return error;

		/* Succeeded: the definition ran. */
		return 0;
	}

	/* Chooses the statement by its opcode. */
	switch (opcode) {
	case DRV_ACPI_OP_IF:
		/* Runs an If and its Else. */
		error = exec_if(eval);
		break;
	case DRV_ACPI_OP_ELSE:
		/* An Else without its If is skipped. */
		error = drv_acpi_stream_package_length(eval, &eval->position);
		break;
	case DRV_ACPI_OP_WHILE:
		/* Runs a While loop. */
		error = exec_while(eval);
		break;
	case DRV_ACPI_OP_RETURN:
		/* Runs a Return. */
		error = exec_return(eval);
		break;
	case DRV_ACPI_OP_BREAK:
		/* BREAK tells the enclosing term lists to leave up to the While. */
		eval->control = DRV_ACPI_CONTROL_BREAK;
		error = 0;
		break;
	case DRV_ACPI_OP_CONTINUE:
		/* CONTINUE tells the enclosing term lists to leave up to the While, which starts its next turn. */
		eval->control = DRV_ACPI_CONTROL_CONTINUE;
		error = 0;
		break;
	case DRV_ACPI_OP_NOOP:
	case DRV_ACPI_OP_BREAK_POINT:
		/* A Noop and a BreakPoint do nothing. */
		error = 0;
		break;
	default:
		/* Anything else is an expression whose value is dropped. */
		error = exec_expression(eval, start);
		break;
	}

	/* Reports a statement that could not run. */
	if (error != 0)
		return error;

	/* Succeeded: the statement ran. */
	return 0;
}

/* Runs an expression in a term position, from its opcode at start, and drops its value. */
static int
exec_expression(
	struct drv_acpi_eval *eval,
	const uint8_t *start)
{
	struct drv_acpi_object *object;
	int error;

	/* Evaluates the expression from its opcode. */
	eval->position = start;
	error = drv_acpi_eval_term_arg(eval, &object);
	if (error != 0)
		return error;

	/* Drops the value. */
	drv_acpi_object_release(object);

	/* Succeeded: the expression ran. */
	return 0;
}

/* Runs a nested term list that ends at block_end, then continues after it. */
static int
exec_block(
	struct drv_acpi_eval *eval,
	const uint8_t *block_end)
{
	const uint8_t *outer_end;
	int error;

	/* Bounds the stream by the block while it runs. */
	outer_end = eval->end;
	eval->end = block_end;
	error = drv_acpi_exec_term_list(eval);
	eval->end = outer_end;
	if (error != 0)
		return error;

	/* Continues after the block unless control left it. */
	if (eval->control == DRV_ACPI_CONTROL_NEXT)
		eval->position = block_end;

	/* Succeeded: the block ran. */
	return 0;
}

/*
 * Runs an If and the Else that may follow it.
 *
 * An Else that holds only another If (and that If's Else) -- an ElseIf, and
 * each Case of the chain ASL compiles a Switch to -- runs that If in this
 * frame, one turn of the loop, instead of one frame deeper.  A chain of any
 * length then takes the stack of one If: a Switch of 25 Cases in the Dell
 * Latitude 5330's DSDT nested past the stack budget (BUG-195).
 */
static int
exec_if(
	struct drv_acpi_eval *eval)
{
	const uint8_t *if_end;
	const uint8_t *else_end;
	const uint8_t *outer_end;
	const uint8_t *predicate_end;
	const uint8_t *chain_end;
	uint64_t predicate;
	uint8_t byte;
	bool chained;
	int error;

	/* The stream's end outside the If, which a chain bounds by its Else while it runs. */
	outer_end = eval->end;
	chain_end = NULL;
	error = 0;

	/* Runs the If, then each If an Else holds alone. */
	for (;;) {
		/* Reads the extent of the If. */
		error = drv_acpi_stream_package_length(eval, &if_end);
		if (error != 0)
			break;

		/* Evaluates the predicate inside the If's extent. */
		predicate_end = eval->end;
		eval->end = if_end;
		error = drv_acpi_eval_integer(eval, &predicate);
		eval->end = predicate_end;
		if (error != 0)
			break;

		/* Runs the If's body when the predicate holds, and steps over it otherwise. */
		if (predicate != 0) {
			error = exec_block(eval, if_end);
			if (error != 0)
				break;

			/* A Return, Break or Continue leaves before any Else. */
			if (eval->control != DRV_ACPI_CONTROL_NEXT)
				break;
		} else {
			eval->position = if_end;
		}

		/* Nothing more to do at the end of the term list. */
		if (eval->position >= eval->end)
			break;

		/* Nothing more to do when no Else follows. */
		byte = *eval->position;
		if (byte != DRV_ACPI_OP_ELSE)
			break;

		/* Reads the extent of the Else. */
		eval->position++;
		error = drv_acpi_stream_package_length(eval, &else_end);
		if (error != 0)
			break;

		/* Skips the Else when the If ran. */
		if (predicate != 0) {
			eval->position = else_end;
			break;
		}

		/* Runs an Else that holds more than one If as a block of its own. */
		chained = else_holds_if_alone(eval, else_end);
		if (!chained) {
			error = exec_block(eval, else_end);
			break;
		}

		/*
		 * Bounds the stream by the Else and steps onto its If, the next
		 * turn.  The Else's If and that If's Else end where the Else ends,
		 * so the outermost Else's end is where the whole chain ends.
		 */
		if (chain_end == NULL)
			chain_end = else_end;
		eval->end = else_end;
		eval->position++;
	}

	/* The stream's end is the If's own again. */
	eval->end = outer_end;

	/* Reports an If or an Else whose body failed. */
	if (error != 0)
		return error;

	/* A chain that ran to its end continues after its outermost Else. */
	if (chain_end != NULL && eval->control == DRV_ACPI_CONTROL_NEXT)
		eval->position = chain_end;

	/* Succeeded: the If or the Else ran. */
	return 0;
}

/*
 * Reports whether an Else, whose body starts at the stream's position and
 * ends at else_end, holds only an If and that If's Else.  The position is
 * left where it was; a body the stream cannot read is not a chain, so its
 * block reports the error as before.
 */
static bool
else_holds_if_alone(
	struct drv_acpi_eval *eval,
	const uint8_t *else_end)
{
	const uint8_t *body;
	const uint8_t *if_end;
	const uint8_t *inner_else_end;
	bool alone;
	int error;

	/* An empty Else, or one that does not start with an If, holds no chain. */
	body = eval->position;
	if (body >= else_end || *body != DRV_ACPI_OP_IF)
		return false;

	/* Reads the extent of the If in the Else. */
	eval->position = body + 1;
	error = drv_acpi_stream_package_length(eval, &if_end);

	/* An If that ends where the Else ends is the Else's only term. */
	alone = false;
	if (error == 0 && if_end == else_end)
		alone = true;

	/* So is an If whose own Else ends where the outer Else ends. */
	if (error == 0 && if_end < else_end && *if_end == DRV_ACPI_OP_ELSE) {
		eval->position = if_end + 1;
		error = drv_acpi_stream_package_length(eval, &inner_else_end);
		if (error == 0 && inner_else_end == else_end)
			alone = true;
	}

	/* Puts the position back at the Else's body. */
	eval->position = body;

	/* Reports whether the Else's If is all it holds. */
	return alone;
}

/* Runs a While loop. */
static int
exec_while(
	struct drv_acpi_eval *eval)
{
	const uint8_t *while_end;
	const uint8_t *predicate_start;
	const uint8_t *outer_end;
	uint64_t predicate;
	uint64_t started;
	uint64_t now;
	int error;

	/* Reads the extent of the loop. */
	error = drv_acpi_stream_package_length(eval, &while_end);
	if (error != 0)
		return error;

	/* Remembers where the predicate is and when the loop started. */
	predicate_start = eval->position;
	started = drv_acpi_os_timer();

	/* Runs the body for as long as the predicate holds. */
	for (;;) {
		/* Evaluates the predicate inside the loop's extent. */
		eval->position = predicate_start;
		outer_end = eval->end;
		eval->end = while_end;
		error = drv_acpi_eval_integer(eval, &predicate);
		eval->end = outer_end;
		if (error != 0)
			return error;

		/* Leaves when the predicate fails. */
		if (predicate == 0)
			break;

		/* Runs the body. */
		error = exec_block(eval, while_end);
		if (error != 0)
			return error;

		/*
		 * A Break leaves the loop, a Return the method; a Continue starts
		 * the next turn.  The loop consumes a Break or a Continue, so the
		 * terms after it see NEXT again.
		 */
		if (eval->control == DRV_ACPI_CONTROL_BREAK) {
			eval->control = DRV_ACPI_CONTROL_NEXT;
			break;
		} else if (eval->control == DRV_ACPI_CONTROL_RETURN) {
			return 0;
		} else if (eval->control == DRV_ACPI_CONTROL_CONTINUE) {
			eval->control = DRV_ACPI_CONTROL_NEXT;
		}

		/* Abandons a loop that waits for hardware that never answers. */
		now = drv_acpi_os_timer();
		if (now - started > WHILE_TIMEOUT_TICKS) {
			drv_acpi_os_log("ACPI: While loop timed out\n");
			return ETIMEDOUT;
		}
	}

	/* Continues after the loop. */
	eval->position = while_end;

	/* Succeeded: the loop ended. */
	return 0;
}

/* Runs a Return. */
static int
exec_return(
	struct drv_acpi_eval *eval)
{
	struct drv_acpi_object *value;
	int error;

	/* Evaluates the returned object. */
	error = drv_acpi_eval_term_arg(eval, &value);
	if (error != 0)
		return error;

	/*
	 * Hands it to the invocation; RETURN tells every enclosing term list
	 * to leave, up to the method's body.
	 */
	drv_acpi_object_release(eval->return_value);
	eval->return_value = value;
	eval->control = DRV_ACPI_CONTROL_RETURN;

	/* Succeeded: the method returns the value. */
	return 0;
}

/* Evaluates a name in a TermArg position. */
static int
eval_name(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_name name;
	struct drv_acpi_node *node;
	int error;

	/* Reads the name. */
	error = drv_acpi_stream_name(eval, &name);
	if (error != 0)
		return error;

	/* Resolves it with the search rules of references. */
	error = drv_acpi_ns_lookup(eval->scope, &name, true, &node);
	if (error != 0) {
		report_name("name", &name, error);
		return error;
	}

	/* An alias stands for the node it names. */
	node = drv_acpi_ns_resolve_alias(node);

	/*
	 * A method is invoked with the arguments that follow the name, and one
	 * that returned nothing gives zero as its value; anything else is read.
	 */
	if (node->object != NULL && node->object->type == DRV_ACPI_TYPE_METHOD) {
		error = eval_method_call(eval, node, result);
		if (error == 0)
			error = nothing_as_zero(result);
	} else {
		error = drv_acpi_read_node(eval, node, result);
	}

	/* Reports a name whose value could not be had. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the value. */
	return 0;
}

/* Evaluates the arguments that follow a method's name and invokes it. */
static int
eval_method_call(
	struct drv_acpi_eval *eval,
	struct drv_acpi_node *node,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *arguments[DRV_ACPI_ARGUMENT_COUNT];
	unsigned count;
	unsigned index;
	int error;

	/* The method's definition says how many arguments follow. */
	count = node->object->value.method.argument_count;

	/* Evaluates each argument in order. */
	for (index = 0; index < count; index++) {
		/* Evaluates one argument. */
		error = drv_acpi_eval_term_arg(eval, &arguments[index]);
		if (error != 0) {
			/* Releases the arguments evaluated before this one. */
			while (index != 0) {
				index--;
				drv_acpi_object_release(arguments[index]);
			}

			/* Reports the argument that failed. */
			return error;
		}
	}

	/* Invokes the method with them. */
	error = drv_acpi_invoke(eval, node, arguments, count, result);

	/* The invocation took its own references. */
	for (index = 0; index < count; index++)
		drv_acpi_object_release(arguments[index]);

	/* Reports a failed invocation. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the result, if any. */
	return 0;
}

/* Evaluates a constant or literal data object. */
static int
eval_constant(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *object;
	uint64_t value;
	int error;

	/* A string is a literal of its own. */
	if (opcode == DRV_ACPI_OP_STRING_PREFIX) {
		error = eval_string(eval, result);
		if (error != 0)
			return error;

		/* Succeeded: the caller holds the string. */
		return 0;
	}

	/* Chooses the integer constant by its opcode. */
	value = 0;
	error = 0;
	switch (opcode) {
	case DRV_ACPI_OP_ZERO:
		value = 0;
		break;
	case DRV_ACPI_OP_ONE:
		value = 1;
		break;
	case DRV_ACPI_OP_ONES:
		value = drv_acpi_integer_mask();
		break;
	case DRV_ACPI_OP_BYTE_PREFIX:
		/* Reads a byte. */
		error = drv_acpi_stream_integer(eval, 1, &value);
		break;
	case DRV_ACPI_OP_WORD_PREFIX:
		/* Reads a word. */
		error = drv_acpi_stream_integer(eval, 2, &value);
		break;
	case DRV_ACPI_OP_DWORD_PREFIX:
		/* Reads a double word. */
		error = drv_acpi_stream_integer(eval, 4, &value);
		break;
	case DRV_ACPI_OP_QWORD_PREFIX:
		/* Reads a quad word, cut to the integer width. */
		error = drv_acpi_stream_integer(eval, 8, &value);
		value &= drv_acpi_integer_mask();
		break;
	case DRV_ACPI_OP_REVISION:
		value = DRV_ACPI_AML_REVISION;
		break;
	default:
		/* Refuses an opcode that is no constant. */
		error = EINVAL;
		break;
	}

	/* Reports a constant that could not be read. */
	if (error != 0)
		return error;

	/* Makes the integer object. */
	object = drv_acpi_object_integer_new(value);
	if (object == NULL)
		return ENOMEM;

	/* Hands over the integer. */
	*result = object;

	/* Succeeded: the caller holds the constant. */
	return 0;
}

/* Evaluates a string literal, which stays in the table until it is copied. */
static int
eval_string(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *object;
	const char *text;
	size_t length;
	int error;

	/* Reads the string in place. */
	error = drv_acpi_stream_string(eval, &text, &length);
	if (error != 0)
		return error;

	/* Copies it into a string object. */
	object = drv_acpi_object_string_new_length(text, length);
	if (object == NULL)
		return ENOMEM;

	/* Hands over the string. */
	*result = object;

	/* Succeeded: the caller holds the string. */
	return 0;
}

/* Reads a local or an argument of the running invocation. */
static int
eval_local(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *object;

	/* Refuses a local or an argument outside any method. */
	if (eval->frame == NULL)
		return EINVAL;

	/* Finds the slot. */
	if (opcode <= DRV_ACPI_OP_LOCAL7) {
		object = eval->frame->locals[opcode - DRV_ACPI_OP_LOCAL0];
	} else {
		object = eval->frame->arguments[opcode - DRV_ACPI_OP_ARG0];
	}

	/* Refuses a slot that was never stored to. */
	if (object == NULL) {
		drv_acpi_os_log("ACPI: read of an uninitialized local or argument\n");
		return EINVAL;
	}

	/* Hands over the object with a reference of the caller's own. */
	drv_acpi_object_ref(object);
	*result = object;

	/* Succeeded: the caller shares the object. */
	return 0;
}

/* Replaces a local or argument with a copy of the value. */
static int
store_local(
	struct drv_acpi_object **slot,
	struct drv_acpi_object *value)
{
	struct drv_acpi_object *copy;
	int error;

	/* A local gets its own copy of data and shares anything else. */
	error = drv_acpi_object_copy(value, &copy);
	if (error != 0)
		return error;

	/* Replaces the old value. */
	drv_acpi_object_release(*slot);
	*slot = copy;

	/* Succeeded: the slot holds the value. */
	return 0;
}

/* Stores into a named object, converting to the object's type. */
static int
store_node(
	struct drv_acpi_eval *eval,
	struct drv_acpi_node *node,
	struct drv_acpi_object *value)
{
	struct drv_acpi_object *object;
	struct drv_acpi_object *converted;
	uint64_t integer;
	int error;

	/* Stores through an alias. */
	node = drv_acpi_ns_resolve_alias(node);
	object = node->object;

	/* A node without a value takes a copy of anything. */
	if (object == NULL || object->type == DRV_ACPI_TYPE_UNINITIALIZED) {
		error = store_uninitialized(node, value);
		if (error != 0)
			return error;

		/* Succeeded: the node holds a copy of the value. */
		return 0;
	}

	/* Chooses the conversion by the target's type; a field or a buffer is written in place. */
	converted = NULL;
	switch (object->type) {
	case DRV_ACPI_TYPE_INTEGER:
		/* Converts to an integer at the integer width. */
		error = drv_acpi_convert_integer(value, &integer);
		if (error != 0)
			break;

		/* Makes the new integer. */
		converted = drv_acpi_object_integer_new(integer & drv_acpi_integer_mask());
		if (converted == NULL)
			error = ENOMEM;
		break;
	case DRV_ACPI_TYPE_STRING:
		/* Converts to a string. */
		error = drv_acpi_convert_string(value, &converted);
		break;
	case DRV_ACPI_TYPE_BUFFER:
		/* Copies into the existing buffer, which keeps its length. */
		error = store_buffer_into(object, value);
		break;
	case DRV_ACPI_TYPE_PACKAGE:
		/* Only a package replaces a package. */
		if (value->type != DRV_ACPI_TYPE_PACKAGE) {
			error = EINVAL;
			break;
		}

		/* Copies the package. */
		error = drv_acpi_object_copy(value, &converted);
		break;
	case DRV_ACPI_TYPE_FIELD_UNIT:
		/* Writes the field's region. */
		error = drv_acpi_field_write(eval, object, value);
		break;
	case DRV_ACPI_TYPE_BUFFER_FIELD:
		/* Writes the field's buffer. */
		error = drv_acpi_buffer_field_write(object, value);
		break;
	default:
		/* Refuses an object that cannot be stored to. */
		drv_acpi_os_log("ACPI: store to an object of type %u\n", (unsigned)object->type);
		error = EINVAL;
		break;
	}

	/* Reports a value that could not be converted or written. */
	if (error != 0)
		return error;

	/* Replaces the node's value with the converted one, when the store made one. */
	if (converted != NULL) {
		drv_acpi_ns_attach(node, converted);
		drv_acpi_object_release(converted);
	}

	/* Succeeded: the node holds the value. */
	return 0;
}

/* Gives a node without a value a copy of the value. */
static int
store_uninitialized(
	struct drv_acpi_node *node,
	struct drv_acpi_object *value)
{
	struct drv_acpi_object *copy;
	int error;

	/* Copies the value. */
	error = drv_acpi_object_copy(value, &copy);
	if (error != 0)
		return error;

	/* The node takes its own reference to the copy; this one goes. */
	drv_acpi_ns_attach(node, copy);
	drv_acpi_object_release(copy);

	/* Succeeded: the node holds a copy of the value. */
	return 0;
}

/* Stores through a reference object. */
static int
store_reference(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *reference,
	struct drv_acpi_object *value)
{
	struct drv_acpi_object *target;
	int error;

	/* Chooses the store by what the reference points at. */
	switch (reference->value.reference.kind) {
	case DRV_ACPI_REFERENCE_NODE:
		/* Stores into the node. */
		error = store_node(eval, reference->value.reference.node, value);
		break;
	case DRV_ACPI_REFERENCE_INDEX:
		/* Stores into the element. */
		error = store_index(reference, value);
		break;
	case DRV_ACPI_REFERENCE_OBJECT:
		/* Only a buffer can be changed in place through such a reference. */
		target = reference->value.reference.target;
		if (target->type != DRV_ACPI_TYPE_BUFFER) {
			error = EINVAL;
			break;
		}

		/* Copies into the buffer. */
		error = store_buffer_into(target, value);
		break;
	default:
		/* Refuses a reference that cannot be stored through. */
		error = EINVAL;
		break;
	}

	/* Reports a failed store. */
	if (error != 0)
		return error;

	/* Succeeded: what the reference points at holds the value. */
	return 0;
}

/* Stores into one element of a package, buffer or string. */
static int
store_index(
	struct drv_acpi_object *reference,
	struct drv_acpi_object *value)
{
	struct drv_acpi_object *container;
	uint64_t integer;
	uint32_t index;
	int error;

	/* Finds the container and the element. */
	container = reference->value.reference.target;
	index = reference->value.reference.index;

	/* A package element is replaced by a copy of the value. */
	if (container->type == DRV_ACPI_TYPE_PACKAGE) {
		error = store_element(container, index, value);
		if (error != 0)
			return error;

		/* Succeeded: the element holds a copy of the value. */
		return 0;
	}

	/* A byte or a character takes the low byte of the value as an integer. */
	error = drv_acpi_convert_integer(value, &integer);
	if (error != 0)
		return error;

	/* Writes the byte into the buffer or the string. */
	if (container->type == DRV_ACPI_TYPE_BUFFER) {
		container->value.buffer.bytes[index] = (uint8_t)integer;
	} else {
		container->value.string.text[index] = (char)integer;
	}

	/* Succeeded: the byte holds the value's low byte. */
	return 0;
}

/* Replaces one element of a package with a copy of the value. */
static int
store_element(
	struct drv_acpi_object *package,
	uint32_t index,
	struct drv_acpi_object *value)
{
	struct drv_acpi_object *copy;
	int error;

	/* Copies the value. */
	error = drv_acpi_object_copy(value, &copy);
	if (error != 0)
		return error;

	/* Replaces the element, letting the old one go. */
	drv_acpi_object_release(package->value.package.elements[index]);
	package->value.package.elements[index] = copy;

	/* Succeeded: the element holds a copy of the value. */
	return 0;
}

/*
 * Copies a value into an existing buffer, which keeps its length: a
 * shorter value is followed by zeros and a longer one is cut.
 */
static int
store_buffer_into(
	struct drv_acpi_object *target,
	struct drv_acpi_object *value)
{
	struct drv_acpi_object *source;
	size_t length;
	int error;

	/* Converts the value to a buffer. */
	error = drv_acpi_convert_buffer(value, &source);
	if (error != 0)
		return error;

	/* Takes as much of the source as fits. */
	length = source->value.buffer.length;
	if (length > target->value.buffer.length)
		length = target->value.buffer.length;

	/* Clears the target. */
	if (target->value.buffer.length != 0)
		kern_memset(target->value.buffer.bytes, 0, target->value.buffer.length);

	/* Copies the source's bytes. */
	if (length != 0)
		kern_memcpy(target->value.buffer.bytes, source->value.buffer.bytes, length);

	/* The converted source is no longer needed. */
	drv_acpi_object_release(source);

	/* Succeeded: the buffer holds the value. */
	return 0;
}

/* Parses a RefOf, DerefOf or Index target. */
static int
parse_reference_target(
	struct drv_acpi_eval *eval,
	unsigned opcode,
	struct drv_acpi_target *target)
{
	struct drv_acpi_object *object;
	int error;

	/* Only operators that produce references can be targets. */
	if (opcode != DRV_ACPI_OP_REF_OF &&
	    opcode != DRV_ACPI_OP_DEREF_OF &&
	    opcode != DRV_ACPI_OP_INDEX) {
		drv_acpi_os_log("ACPI: opcode 0x%x is not a target\n", opcode);
		return EIO;
	}

	/* DerefOf in a target names what its operand refers to. */
	if (opcode == DRV_ACPI_OP_DEREF_OF) {
		error = deref_target(eval, target);
		if (error != 0)
			return error;

		/* Succeeded: the target is what the operand refers to. */
		return 0;
	}

	/* RefOf and Index compute the reference themselves. */
	error = drv_acpi_eval_operator(eval, opcode, &object);
	if (error != 0)
		return error;

	/* Makes the target store through the reference, which it holds. */
	target->kind = DRV_ACPI_TARGET_REFERENCE;
	target->reference = object;

	/* Succeeded: the target stores through the reference. */
	return 0;
}

/* Parses the operand of a DerefOf in a target: a path, or a reference. */
static int
deref_target(
	struct drv_acpi_eval *eval,
	struct drv_acpi_target *target)
{
	struct drv_acpi_object *object;
	struct drv_acpi_node *node;
	int error;

	/* Evaluates the operand. */
	error = drv_acpi_eval_term_arg(eval, &object);
	if (error != 0)
		return error;

	/* A string operand is the path of the target node; a reference is stored through. */
	if (object->type == DRV_ACPI_TYPE_STRING) {
		error = drv_acpi_lookup_path(eval->scope, object->value.string.text, true, &node);
		drv_acpi_object_release(object);
		if (error != 0)
			return error;

		/* Makes the node the target. */
		target->kind = DRV_ACPI_TARGET_NODE;
		target->node = node;
	} else if (object->type == DRV_ACPI_TYPE_REFERENCE) {
		/* Makes the target store through the reference, which it holds. */
		target->kind = DRV_ACPI_TARGET_REFERENCE;
		target->reference = object;
	} else {
		/* Refuses anything else. */
		drv_acpi_object_release(object);
		return EINVAL;
	}

	/* Succeeded: target says where the result goes. */
	return 0;
}

/* Logs a value stored into the debug object. */
static void
debug_store(
	struct drv_acpi_object *value)
{
	/* Writes the value in the form its type suggests. */
	switch (value->type) {
	case DRV_ACPI_TYPE_INTEGER:
		drv_acpi_os_log("ACPI Debug: 0x%llx\n", (unsigned long long)value->value.integer);
		break;
	case DRV_ACPI_TYPE_STRING:
		drv_acpi_os_log("ACPI Debug: \"%s\"\n", value->value.string.text);
		break;
	case DRV_ACPI_TYPE_BUFFER:
		drv_acpi_os_log("ACPI Debug: buffer of %lu bytes\n", (unsigned long)value->value.buffer.length);
		break;
	case DRV_ACPI_TYPE_PACKAGE:
		drv_acpi_os_log("ACPI Debug: package of %u elements\n", (unsigned)value->value.package.count);
		break;
	default:
		drv_acpi_os_log("ACPI Debug: object of type %u\n", (unsigned)value->type);
		break;
	}
}

/* Ends an invocation: deletes the nodes it created and releases its objects. */
static void
frame_release(
	struct drv_acpi_frame *frame)
{
	struct drv_acpi_node *node;
	unsigned index;

	/* Deletes the nodes, newest first so that children go before parents. */
	while (frame->created != NULL) {
		/* Unlinks the newest node and deletes it. */
		node = frame->created;
		frame->created = node->temporary_next;
		drv_acpi_ns_delete(node);
	}

	/* Releases the locals. */
	for (index = 0; index < DRV_ACPI_LOCAL_COUNT; index++)
		drv_acpi_object_release(frame->locals[index]);

	/* Releases the arguments. */
	for (index = 0; index < DRV_ACPI_ARGUMENT_COUNT; index++)
		drv_acpi_object_release(frame->arguments[index]);

	/* Frees the frame. */
	drv_acpi_os_free(frame);
}

/*
 * Acquires or releases the mutex of a serialized method, which keeps two
 * threads from running it at once; its sync level is the method's.
 */
static int
serialize(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object *method,
	bool acquire)
{
	struct drv_acpi_object *mutex;
	bool timed_out;
	int error;

	/* A method that is not serialized has no mutex. */
	if (!method->value.method.serialized)
		return 0;

	/* Releases the mutex after the body. */
	if (!acquire) {
		error = drv_acpi_mutex_release(eval->thread, method->value.method.serialization);
		if (error != 0)
			return error;

		/* Succeeded: another thread may run the method. */
		return 0;
	}

	/* Creates the mutex on the first invocation. */
	mutex = method->value.method.serialization;
	if (mutex == NULL) {
		/* Allocates the mutex. */
		mutex = drv_acpi_object_new(DRV_ACPI_TYPE_MUTEX);
		if (mutex == NULL)
			return ENOMEM;

		/* Gives it the method's sync level; the method holds it from now on. */
		mutex->value.mutex.sync_level = method->value.method.sync_level;
		method->value.method.serialization = mutex;
	}

	/* Acquires it, waiting as long as another thread runs the method. */
	error = drv_acpi_mutex_acquire(eval->thread, mutex, 0xffffU, &timed_out);
	if (error != 0)
		return error;

	/* Succeeded: this thread runs the method alone. */
	return 0;
}

/* Logs a name that could not be resolved. */
static void
report_name(
	const char *what,
	const struct drv_acpi_name *name,
	int error)
{
	char text[128];
	int text_error;

	/* Converts the name to text, or a mark when it does not fit. */
	text_error = drv_acpi_ns_name_text(name, text, sizeof(text));
	if (text_error != 0)
		kern_strcpy(text, "(long name)");

	/* Logs it with the error. */
	drv_acpi_os_log("ACPI: %s %s not resolved (error %d)\n", what, text, error);
}
