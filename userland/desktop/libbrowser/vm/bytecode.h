/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shared bytecode (plan/ws074/design.md §11.5): the instructions the
 * interpreter runs for JavaScript and for Wasm, and the code unit that
 * holds them.  The JS compiler (ws074-p025) and the Wasm compiler
 * (ws074-p033) write it; the tests assemble it by hand.
 *
 * An instruction is one 32-bit word naming its opcode followed by the
 * operand words the opcode's entry in vm_opcodes says: register numbers
 * (slots of the frame), constant numbers (the code's table of boxed
 * values), immediates, jump offsets (in words, from the start of the
 * instruction) and counts.  JavaScript's instructions work on boxed values
 * (vm_value); Wasm's work on the raw 64 bits in a register (an i32 in the
 * low 32 bits), since Wasm's types are known when it is compiled.
 */

#ifndef KEILAND_BROWSER_VM_BYTECODE_H
#define KEILAND_BROWSER_VM_BYTECODE_H

#include "vm/vm.h"

/* The most operands an instruction has. */
#define VM_OPERANDS_MAX		5U

/* The kinds of operand. */
enum vm_operand_kind {
	VM_OPERAND_REGISTER,
	VM_OPERAND_CONSTANT,
	VM_OPERAND_IMMEDIATE,
	VM_OPERAND_JUMP,
	VM_OPERAND_COUNT
};

/*
 * The opcodes.  The comment of each gives its operands (R a register, C a
 * constant, I an immediate, J a jump, N a count) and what it does.
 */
enum vm_opcode {
	/* Shared. */
	VM_OP_NOP,		/* -: nothing */
	VM_OP_MOV,		/* R dst, R src */
	VM_OP_LOAD_CONST,	/* R dst, C value */
	VM_OP_LOAD_INT,		/* R dst, I int32 (boxed) */
	VM_OP_JUMP,		/* J target */
	VM_OP_JUMP_IF_TRUE,	/* R value, J target (ToBoolean) */
	VM_OP_JUMP_IF_FALSE,	/* R value, J target (ToBoolean) */
	VM_OP_CALL,		/* R dst, R callee, R this, R first argument, N argument count */
	VM_OP_RETURN,		/* R value */
	VM_OP_THROW,		/* R value */
	VM_OP_LOOP_HINT,	/* -: a loop's head (counted for the JIT's tier-up later) */

	/* JavaScript (boxed values). */
	VM_OP_ADD,		/* R dst, R left, R right */
	VM_OP_SUB,		/* R dst, R left, R right */
	VM_OP_MUL,		/* R dst, R left, R right */
	VM_OP_LESS,		/* R dst, R left, R right */
	VM_OP_STRICT_EQ,	/* R dst, R left, R right */
	VM_OP_NEW_OBJECT,	/* R dst */
	VM_OP_NEW_ARRAY,	/* R dst */
	VM_OP_GET_PROP,		/* R dst, R object, C key */
	VM_OP_PUT_PROP,		/* R object, C key, R value */
	VM_OP_GET_ELEM,		/* R dst, R object, R key */
	VM_OP_PUT_ELEM,		/* R object, R key, R value */
	VM_OP_GET_GLOBAL,	/* R dst, C key (a missing global is a ReferenceError) */
	VM_OP_PUT_GLOBAL,	/* C key, R value (strict code may not make a global) */
	VM_OP_DIV,		/* R dst, R left, R right */
	VM_OP_MOD,		/* R dst, R left, R right */
	VM_OP_EXP,		/* R dst, R left, R right */
	VM_OP_BIT_AND,		/* R dst, R left, R right */
	VM_OP_BIT_OR,		/* R dst, R left, R right */
	VM_OP_BIT_XOR,		/* R dst, R left, R right */
	VM_OP_SHL,		/* R dst, R left, R right */
	VM_OP_SAR,		/* R dst, R left, R right */
	VM_OP_SHR,		/* R dst, R left, R right */
	VM_OP_LESS_EQ,		/* R dst, R left, R right */
	VM_OP_GREATER,		/* R dst, R left, R right */
	VM_OP_GREATER_EQ,	/* R dst, R left, R right */
	VM_OP_LOOSE_EQ,		/* R dst, R left, R right */
	VM_OP_INSTANCEOF,	/* R dst, R value, R constructor */
	VM_OP_IN,		/* R dst, R key, R object */
	VM_OP_NEG,		/* R dst, R value */
	VM_OP_TO_NUMBER,	/* R dst, R value */
	VM_OP_BIT_NOT,		/* R dst, R value */
	VM_OP_NOT,		/* R dst, R value */
	VM_OP_TYPEOF,		/* R dst, R value */
	VM_OP_INC,		/* R dst, R value: ToNumber, plus one */
	VM_OP_DEC,		/* R dst, R value: ToNumber, minus one */
	VM_OP_GET_GLOBAL_TYPEOF,	/* R dst, C key (a missing global is undefined) */
	VM_OP_DEFINE_GLOBAL_VAR,	/* C key: a var of the script, undefined unless it exists */
	VM_OP_DEFINE_GLOBAL_FUNCTION,	/* C key, R function: a function declaration of the script */
	VM_OP_DELETE_PROP,	/* R dst, R object, C key */
	VM_OP_DELETE_ELEM,	/* R dst, R object, R key */
	VM_OP_DELETE_GLOBAL,	/* R dst, C key */
	VM_OP_DEFINE_PROP,	/* R object, C key, R value (an object literal's data property) */
	VM_OP_TO_NUMERIC,	/* R dst, R value: ToNumeric (a BigInt stays a BigInt) */
	VM_OP_DEFINE_ELEM,	/* R object, R key, R value (the same with a computed key) */
	VM_OP_DEFINE_GETTER,	/* R object, R key, R function */
	VM_OP_DEFINE_SETTER,	/* R object, R key, R function */
	VM_OP_SET_PROTO,	/* R object, R value (__proto__ in an object literal) */
	VM_OP_ARRAY_PUSH,	/* R array, R value: the next element */
	VM_OP_ARRAY_HOLE,	/* R array: a hole as the next element */
	VM_OP_NEW_ENV,		/* R dst, R parent environment (or undefined), I slot count */
	VM_OP_GET_ENV,		/* R dst, R environment, I hops outwards, I slot */
	VM_OP_GET_IMPORT,	/* R dst, R indirect binding: read its live target slot */
	VM_OP_PUT_ENV,		/* R environment, I hops outwards, I slot, R value */
	VM_OP_LOAD_CLOSURE_ENV,	/* R dst: the running function's environment (or undefined) */
	VM_OP_LOAD_THIS,	/* R dst: the this value (sloppy code's undefined or null is the global object) */
	VM_OP_LOAD_CALLEE,	/* R dst: the running function */
	VM_OP_NEW_CLOSURE,	/* R dst, C code, R environment (or undefined) */
	VM_OP_CONSTRUCT,	/* R dst, R constructor, R new.target, R first argument, N argument count */
	VM_OP_FOR_IN_START,	/* R dst, R object: an iterator over its enumerable keys */
	VM_OP_FOR_IN_NEXT,	/* R dst, R iterator, J target when there is no next key */
	VM_OP_TO_PROPERTY_KEY,	/* R dst, R value: a computed key, converted where it is written */
	VM_OP_NEW_REGEXP,	/* R dst, C pattern, C flags: a regular expression literal's new object (ws074-p027) */
	VM_OP_TO_STRING,	/* R dst, R value: ToString (a template's substitution, ws074-p078) */
	VM_OP_LOAD_EMPTY,	/* R dst: the empty value, a let or const binding before its declaration runs */
	VM_OP_CHECK_INIT,	/* R value, C name: a ReferenceError when the value is still the empty value */
	VM_OP_THROW_ERROR,	/* I kind (enum vm_error_kind), C message: throws a new error of the kind */
	VM_OP_DEFINE_GLOBAL_LEXICAL,	/* C key, I const: a script's top-level let or const, before its declaration runs */
	VM_OP_INIT_GLOBAL_LEXICAL,	/* C key, R value: the declaration of a script's top-level let or const runs */
	VM_OP_ITER_START,	/* R dst, R value: an iteration of the value (ws074-p079) */
	VM_OP_ITER_NEXT,	/* R dst, R iteration: its next value, undefined once it has ended */
	VM_OP_ITER_REST,	/* R dst, R iteration: an array of the values it has left */
	VM_OP_ARRAY_SPREAD,	/* R array, R value: every value of the iterable appended */
	VM_OP_COPY_DATA,	/* R object, R value: the value's own enumerable properties copied */
	VM_OP_OBJECT_REST,	/* R dst, R value, R excluded keys (an array): an object pattern's rest */
	VM_OP_CALL_ARRAY,	/* R dst, R function, R this, R arguments (an array) */
	VM_OP_CONSTRUCT_ARRAY,	/* R dst, R constructor, R arguments (an array) */
	VM_OP_CHECK_COERCIBLE,	/* R value: a TypeError for undefined and null (an object pattern's value) */
	VM_OP_ARGS_REST,	/* R dst, R arguments object, I first: an array of the arguments from the first (a rest parameter) */
	VM_OP_LOAD_NEW_TARGET,	/* R dst: new.target (undefined in a call, ws074-p080) */
	VM_OP_CLASS_SETUP,	/* R dst prototype, R constructor, R parent (the empty value without extends) */
	VM_OP_DEFINE_METHOD,	/* R home object, R key, R function, I kind (0 method, 1 getter, 2 setter) */
	VM_OP_LOAD_HOME,	/* R dst: the running method's home object */
	VM_OP_GET_SUPER,	/* R dst, R home object, R key, R this: super[key] with this as the receiver */
	VM_OP_SUPER_CONSTRUCT,	/* R dst, R first argument, N argument count: super(...), which binds this */
	VM_OP_SUPER_CONSTRUCT_ARRAY,	/* R dst, R arguments (an array): super(...) with a spread */
	VM_OP_NEW_PRIVATE_NAME,	/* R dst, C description: a class's new private name */
	VM_OP_PRIVATE_GET,	/* R dst, R object, R private name */
	VM_OP_PRIVATE_SET,	/* R object, R private name, R value */
	VM_OP_PRIVATE_DEFINE,	/* R object, R private name, R value: a private field */
	VM_OP_PRIVATE_COPY,	/* R object, R source, R private name: a private method the prototype keeps, onto an instance */
	VM_OP_PRIVATE_IN,	/* R dst, R private name, R object: #x in object */
	VM_OP_FOR_OF_NEXT,	/* R dst, R iteration, J target when it has ended: a for-of loop's next value (ws074-p087) */
	VM_OP_ITER_CLOSE,	/* R iteration, I quiet: an iteration left early is closed (quiet when leaving by an exception) */
	VM_OP_SUSPEND,		/* R value, R sent, R how: a generator yields (an async function awaits) the value; resumed, sent and how (VM_RESUME_*) hold what came (ws074-p086) */

	VM_OP_DELEGATE_START,	/* R state, R iterable: native synchronous iterator record */
	VM_OP_DELEGATE_STEP,	/* R result, R state, R sent, R how, R outcome: -1 yields the result, NEXT completes the expression, RETURN returns */
	VM_OP_SUSPEND_RESULT,	/* R result, R sent, R how: yield the original iterator result without reading its value */
	VM_OP_ASYNC_ITER_START,	/* R iterator, R sync, R value: the iterator of a for await (sync is true when it is the value's synchronous one) */
	VM_OP_CHECK_OBJECT,	/* R value: a TypeError unless the value is an object (an iterator's result) */
	VM_OP_SET_FUNCTION_NAME,	/* R function, R key, I prefix (0 none, 1 get, 2 set): an anonymous function named after its key */

	/* Wasm (raw values). */
	VM_OP_I32_CONST,	/* R dst, I value */
	VM_OP_I32_ADD,		/* R dst, R left, R right */
	VM_OP_I32_SUB,		/* R dst, R left, R right */
	VM_OP_I32_MUL,		/* R dst, R left, R right */
	VM_OP_I32_LT_S,		/* R dst, R left, R right */
	VM_OP_I32_EQZ,		/* R dst, R value */
	VM_OP_BR_IF,		/* R i32, J target (taken when not zero) */
	VM_OP_I64_CONST,	/* R dst, I low, I high */
	VM_OP_I64_ADD,		/* R dst, R left, R right */
	VM_OP_F64_CONST,	/* R dst, I low bits, I high bits */
	VM_OP_F64_ADD,		/* R dst, R left, R right */
	VM_OP_F64_MUL,		/* R dst, R left, R right */
	VM_OP_BOX_I32,		/* R dst, R i32: the JS number of a raw i32 */
	VM_OP_BOX_F64,		/* R dst, R f64: the JS number of a raw f64 */

	VM_OPCODE_COUNT
};

/*
 * What the interpreter and the checker know of an opcode: its name and
 * the kinds of its operands.
 */
struct vm_opcode_info {
	const char *name;
	unsigned operand_count;
	unsigned char kinds[VM_OPERANDS_MAX];
};

/*
 * One exception handler: an exception thrown by an instruction in
 * [start, end) (word offsets) lands at handler with the exception in the
 * register.
 */
struct vm_handler {
	uint32_t start;
	uint32_t end;
	uint32_t handler;
	uint32_t exception_register;
};

/*
 * Where in the source an instruction came from: the instructions from
 * offset (a word offset) up to the next position's were compiled from the
 * expression or statement at line and column (1-based, in the script's
 * text).  It is only for reporting an uncaught exception's place.
 */
struct vm_position {
	uint32_t offset;
	uint32_t line;
	uint32_t column;
};

/*
 * A code unit: one function's instructions and what they refer to.
 *
 * The words, the constants and the handlers are malloc'd copies freed with
 * the cell; the constants are boxed values the cell keeps alive.  The
 * first parameter_count registers receive the arguments.  A code unit is
 * checked when it is made and never changes.
 */
struct vm_code {
	struct vm_cell cell;
	uint32_t *words;
	uint32_t word_count;
	uint32_t register_count;
	uint32_t parameter_count;
	uint32_t constant_count;
	vm_value *constants;
	struct vm_handler *handlers;
	uint32_t handler_count;
	struct vm_position *positions;
	uint32_t position_count;
	uint32_t flags;
	uint32_t arguments_register;
	uint32_t length;
	struct vm_string *name;
};

/* The code is strict mode code (this is not replaced; a failed assignment throws). */
#define VM_CODE_STRICT		0x1U

/* A call makes an arguments object in arguments_register. */
#define VM_CODE_ARGUMENTS	0x2U

/* The function can be called with new: its closures get a prototype object. */
#define VM_CODE_CONSTRUCTOR	0x4U

/* The function's length is length, not its parameter count (a default or a rest parameter comes before the end). */
#define VM_CODE_LENGTH		0x8U

/* A class's constructor: a call without new is a TypeError, and its closure's prototype is made by class_setup. */
#define VM_CODE_CLASS		0x10U

/* A derived class's constructor: new gives it no this; its super call makes it. */
#define VM_CODE_DERIVED		0x20U

/* A generator function: a call makes a generator, which runs the code up to each yield (ws074-p086). */
#define VM_CODE_GENERATOR	0x40U

/* An async function: a call runs the code up to its first await and returns a promise (ws074-p086). */
#define VM_CODE_ASYNC		0x80U

/*
 * An environment: the variables of one function (or script) that the
 * functions made inside it can see, since those outlive the frame.
 *
 * The count slots follow the header (vm_env_slots finds them); parent is
 * the environment of the code around it (NULL for the outermost).  The
 * compiler knows which slot and how many hops outwards each captured
 * variable is.
 */
struct vm_env {
	struct vm_cell cell;
	struct vm_env *parent;
	uint32_t count;
	uint32_t reserved;
};

/* One immutable import's indirect reference to a module environment slot. */
struct vm_import_binding {
	struct vm_cell cell;
	struct vm_env *env;
	uint32_t slot;
};

/* The opcodes' table (code.c). */
extern const struct vm_opcode_info vm_opcodes[VM_OPCODE_COUNT];

/* Code units (code.c). */
extern const struct vm_cell_type vm_code_type;
int vm_code_create(struct vm_heap *heap, const struct vm_code *model, struct vm_code **code, uint32_t *bad_offset);
int vm_code_dump(const struct vm_code *code, struct wb_buffer *out);
int vm_code_position(const struct vm_code *code, uint32_t offset, uint32_t *line, uint32_t *column);

/* Bytecode functions and environments (function.c). */
extern const struct vm_cell_type vm_env_type;
extern const struct vm_cell_type vm_import_binding_type;
struct vm_function *vm_function_create(struct vm_realm *realm, struct vm_code *code);
struct vm_function *vm_closure_create(struct vm_realm *realm, struct vm_code *code, struct vm_env *env);
struct vm_env *vm_env_create(struct vm_heap *heap, struct vm_env *parent, uint32_t count);
vm_value *vm_env_slots(struct vm_env *env);
struct vm_import_binding *vm_import_binding_create(struct vm_heap *heap, struct vm_env *env, uint32_t slot);

#endif
