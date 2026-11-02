/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inside of the JavaScript compiler, shared by scope.c, emit.c,
 * compile.c and compile_expr.c.
 *
 * The compiler works in two passes over a parsed program.  The scope pass
 * (scope.c) finds every function's bindings (parameters, vars, function
 * declarations, catch parameters, a named function expression's own name,
 * arguments) and marks those a nested function refers to as captured.  The
 * code pass then writes one code unit a function: a captured binding lives
 * in a slot of the function's environment, any other in a register, and a
 * name no function declares is a global.  Errors (a construct not
 * supported yet, out of memory) jump back to js_compile with longjmp, like
 * the parser's, so everything the passes make lives in the program's arena
 * or is reachable from the compiler for cleanup.
 */

#ifndef KEILAND_BROWSER_JS_COMPILE_H
#define KEILAND_BROWSER_JS_COMPILE_H

#include "js/internal.h"
#include "vm/bytecode.h"

#include <setjmp.h>

/* A label not placed yet. */
#define JS_LABEL_UNPLACED	0xffffffffU

/* The completion codes of a try statement's finally block: normal, throw, return, then the jumps out. */
#define JS_COMPLETION_NORMAL	0
#define JS_COMPLETION_THROW	1
#define JS_COMPLETION_RETURN	2
#define JS_COMPLETION_JUMPS	3

/*
 * How js_bind_pattern writes the names of a pattern: as an assignment's
 * targets, as a var's (or a parameter's or a catch clause's) names, or as
 * a let's or const's names being declared.
 */
enum js_bind_mode {
	JS_BIND_ASSIGN,
	JS_BIND_VAR,
	JS_BIND_INIT
};

/* The kinds of binding. */
enum js_binding_kind {
	JS_BINDING_PARAMETER,
	JS_BINDING_VAR,
	JS_BINDING_FUNCTION,
	JS_BINDING_CATCH,
	JS_BINDING_CALLEE,
	JS_BINDING_ARGUMENTS,
	JS_BINDING_THIS,
	JS_BINDING_LET,
	JS_BINDING_CONST,
	JS_BINDING_IMPORT,
	JS_BINDING_HOME
};

/* The name of the hidden binding that keeps a method's home object for the arrow functions inside it, and its length. */
#define JS_HOME_NAME_LENGTH	5U
extern const uint16_t js_home_name[JS_HOME_NAME_LENGTH];

/*
 * The name of the hidden binding that keeps a function's this for the
 * arrow functions inside it (a name no identifier can have), and its
 * length.
 */
#define JS_THIS_NAME_LENGTH	5U
extern const uint16_t js_this_name[JS_THIS_NAME_LENGTH];

/* The kinds of scope. */
enum js_scope_kind {
	JS_SCOPE_FUNCTION,
	JS_SCOPE_CATCH,
	JS_SCOPE_CALLEE,
	JS_SCOPE_BLOCK
};

/*
 * One name a scope declares, and where the code pass keeps its value: a
 * register of the frame, or a slot of the function's environment when a
 * nested function refers to it (captured).
 */
struct js_binding {
	struct js_binding *next;
	const uint16_t *name;
	size_t length;
	int kind;
	int captured;
	uint32_t parameter;
	int in_env;
	uint32_t location;
};

/*
 * One scope: a function's own (its parameters, vars and the let and const
 * of its body), a catch clause's (its parameter), a named function
 * expression's (its name), or a block's (the let and const of a block, a
 * for statement's head or a switch's cases).  A scope belongs to the
 * function whose code runs in it; the chain of parents reaches the
 * program's scope.  Scopes live in the program's arena.
 *
 * A block scope whose bindings a nested function captures has an
 * environment of its own (has_env), made each time the block is entered
 * (so each run of a loop's body has its own), with env_count slots, kept
 * in the register env_register; functions lists the function
 * declarations made when the block is entered.
 */
struct js_scope {
	struct js_scope *parent;
	struct js_scope *next_in_function;
	struct js_function_info *function;
	struct js_binding *bindings;
	/* How many bindings there are, and the hash index of them a scope of many has (NULL until then; mask + 1 slots). */
	uint32_t binding_count;
	uint32_t index_mask;
	struct js_binding **index;
	struct js_hoisted *functions;
	struct js_hoisted *functions_last;
	int kind;
	int has_env;
	uint32_t env_count;
	uint32_t env_register;
};

/*
 * A function declaration hoisted to the top of its function (or of the
 * program), in source order.
 */
struct js_hoisted {
	struct js_hoisted *next;
	struct js_node *node;
};

/*
 * A var of the program, which is a property of the global object rather
 * than a binding of a scope.
 */
struct js_global_name {
	struct js_global_name *next;
	const uint16_t *name;
	size_t length;
};

/*
 * A let or const at the top level of the program: a binding of the
 * realm's record the scripts share rather than of a scope.
 */
struct js_global_lexical {
	struct js_global_lexical *next;
	const uint16_t *name;
	size_t length;
	int is_const;
};

/*
 * What the scope pass learned of one function (or of the program): its
 * scopes, the declarations to hoist, and whether it needs an environment.
 */
struct js_function_info {
	struct js_node *node;
	struct js_function_info *parent;
	struct js_scope *scope;
	struct js_scope *scopes;
	struct js_binding *arguments;
	struct js_hoisted *hoisted;
	struct js_hoisted *hoisted_last;
	struct js_global_name *global_vars;
	struct js_global_name *global_vars_last;
	struct js_global_lexical *global_lexicals;
	struct js_node *rest;
	struct js_node *class_node;
	int simple_parameters;
	uint32_t length;
	int program;
	int strict;
	int has_env;
};

/*
 * A statement that break (and for a loop, continue) can leave, with the
 * labels written before it.  Targets live in the arena and chain
 * outwards.
 */
struct js_target {
	struct js_target *outer;
	struct js_label_name *labels;
	uint32_t break_label;
	uint32_t continue_label;
	uint32_t finally_depth;
	int unlabeled;
};

/*
 * One label written before a statement.
 */
struct js_label_name {
	struct js_label_name *next;
	const uint16_t *name;
	size_t length;
};

/*
 * A jump out of a try statement's blocks that must run its finally block
 * first: the completion code that stands for it and where it goes after.
 */
struct js_exit {
	struct js_exit *next;
	int code;
	uint32_t label;
	uint32_t finally_depth;
};

/*
 * A try statement with a finally block whose blocks are being compiled:
 * the finally block's label, the registers of the completion code and its
 * value (the exception or the return value), and the jumps out that pass
 * through it.
 */
struct js_finally {
	struct js_finally *outer;
	uint32_t entry_label;
	uint32_t kind_register;
	uint32_t value_register;
	struct js_exit *exits;
	int next_code;
};

/* One active sloppy-mode with statement and its object register. */
struct js_with {
	struct js_with *outer;
	uint32_t object;
};

/*
 * A jump or a handler to fix up when the code is finished: the word to
 * write, the instruction it belongs to and the label it goes to.
 */
struct js_patch {
	uint32_t word;
	uint32_t instruction;
	uint32_t label;
};

/*
 * The code pass's state for one function: the code unit being written
 * (words, constants, handlers, labels), the registers, where names are
 * looked up, and the statements break, continue and return must leave.
 * line and column are the source position of the expression or statement
 * being compiled, which the next instruction is recorded at (positions);
 * chain_label is where an optional chain being compiled goes when a
 * value before ?. is undefined or null (JS_LABEL_UNPLACED outside one).
 *
 * It lives in the arena; its vectors are malloc'd and freed when the
 * function is finished, or by js_compile after a failure (the chain of
 * functions being compiled is kept by parent).
 */
struct js_function_compiler {
	struct js_compiler *compiler;
	struct js_function_compiler *parent;
	struct js_function_info *info;
	struct wb_vector words;
	struct wb_vector constants;
	struct wb_vector handlers;
	struct wb_vector labels;
	struct wb_vector patches;
	struct wb_vector positions;
	uint32_t line;
	uint32_t column;
	uint32_t chain_label;
	uint32_t *constant_index;
	uint32_t constant_capacity;
	struct js_scope *scope;
	uint32_t register_count;
	uint32_t local_count;
	uint32_t temp_top;
	uint32_t env_register;
	uint32_t completion_register;
	uint32_t parameter_count;
	uint32_t arguments_register;
	struct js_target *targets;
	struct js_label_name *pending_labels;
	struct js_finally *finally;
	struct js_with *with;
	uint32_t finally_depth;
	int released;
};

/*
 * One compilation: the realm the code is for, the arena of the parse, the
 * error to fill, the jump back on failure, and the functions being
 * compiled (for the collector's tracer and for cleanup).
 */
struct js_compiler {
	struct vm_realm *realm;
	struct wb_arena *arena;
	struct js_syntax_error *error;
	jmp_buf failure;
	int status;
	struct js_function_compiler *current;
	int module_instance;
	uint32_t module_env_count;
};

/* Failing (compile.c). */
void js_compile_fail(struct js_compiler *compiler, const struct js_node *node, const char *message) __attribute__((noreturn));
void js_compile_unsupported(struct js_compiler *compiler, const struct js_node *node, const char *what) __attribute__((noreturn));
void js_compile_out_of_memory(struct js_compiler *compiler) __attribute__((noreturn));

/* The scope pass (scope.c). */
struct js_function_info *js_scope_analyze(struct js_compiler *compiler, struct js_node *program);
struct js_binding *js_scope_resolve(const struct js_function_compiler *fc, const uint16_t *name, size_t length, uint32_t *hops);

/* Writing code (emit.c). */
void js_emit_begin(struct js_function_compiler *fc);
void js_emit_at(struct js_function_compiler *fc, const struct js_node *node, uint32_t *saved_line, uint32_t *saved_column);
void js_emit_release(struct js_function_compiler *fc);
uint32_t js_emit(struct js_function_compiler *fc, uint32_t opcode, uint32_t operand_count, const uint32_t *operands);
uint32_t js_emit0(struct js_function_compiler *fc, uint32_t opcode);
uint32_t js_emit1(struct js_function_compiler *fc, uint32_t opcode, uint32_t first);
uint32_t js_emit2(struct js_function_compiler *fc, uint32_t opcode, uint32_t first, uint32_t second);
uint32_t js_emit3(struct js_function_compiler *fc, uint32_t opcode, uint32_t first, uint32_t second, uint32_t third);
uint32_t js_emit4(struct js_function_compiler *fc, uint32_t opcode, uint32_t first, uint32_t second, uint32_t third, uint32_t fourth);
uint32_t js_emit5(struct js_function_compiler *fc, uint32_t opcode, uint32_t first, uint32_t second, uint32_t third, uint32_t fourth, uint32_t fifth);
uint32_t js_label_new(struct js_function_compiler *fc);
void js_label_place(struct js_function_compiler *fc, uint32_t label);
uint32_t js_here(const struct js_function_compiler *fc);
void js_emit_jump(struct js_function_compiler *fc, uint32_t opcode, uint32_t test_register, uint32_t label);
void js_emit_for_in_next(struct js_function_compiler *fc, uint32_t key_register, uint32_t iterator_register, uint32_t label);
void js_emit_for_of_next(struct js_function_compiler *fc, uint32_t value_register, uint32_t iterator_register, uint32_t label);
void js_emit_handler(struct js_function_compiler *fc, uint32_t start, uint32_t end, uint32_t label, uint32_t exception_register);
uint32_t js_constant(struct js_function_compiler *fc, vm_value value);
uint32_t js_constant_string(struct js_function_compiler *fc, const uint16_t *text, size_t length);
uint32_t js_constant_key(struct js_function_compiler *fc, const uint16_t *text, size_t length);
uint32_t js_constant_number(struct js_function_compiler *fc, double number);
void js_load_number(struct js_function_compiler *fc, uint32_t target, double number);
void js_load_value(struct js_function_compiler *fc, uint32_t target, vm_value value);
uint32_t js_temp(struct js_function_compiler *fc);
struct vm_code *js_emit_finish(struct js_function_compiler *fc, const uint16_t *name, size_t name_length, uint32_t flags);

/* Functions and statements (compile.c). */
struct vm_code *js_compile_function(struct js_compiler *compiler, struct js_function_compiler *parent, struct js_node *node, const uint16_t *name, size_t name_length);
void js_compile_statements(struct js_function_compiler *fc, struct js_node *list);
void js_emit_return(struct js_function_compiler *fc, uint32_t value_register);

/* Expressions (compile_expr.c). */
void js_compile_expression(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
void js_compile_expression_named(struct js_function_compiler *fc, struct js_node *node, uint32_t target, const uint16_t *name, size_t length);
void js_load_binding(struct js_function_compiler *fc, const uint16_t *name, size_t length, uint32_t target);
void js_store_binding(struct js_function_compiler *fc, const struct js_node *node, const uint16_t *name, size_t length, uint32_t source);
void js_init_binding(struct js_function_compiler *fc, const uint16_t *name, size_t length, uint32_t source);
void js_bind_pattern(struct js_function_compiler *fc, struct js_node *target, uint32_t value, int mode);
void js_compile_class(struct js_function_compiler *fc, struct js_node *node, uint32_t target, const uint16_t *name, size_t length);
void js_compile_fields(struct js_function_compiler *fc, struct js_node *class_node, int statics);
void js_scope_enter(struct js_function_compiler *fc, struct js_scope *scope, struct js_scope **saved_scope, uint32_t *saved_env);
void js_scope_leave(struct js_function_compiler *fc, struct js_scope *saved_scope, uint32_t saved_env);
const uint16_t *js_private_name(struct js_compiler *compiler, struct js_node *node, size_t *length);
void js_emit_throw_error(struct js_function_compiler *fc, int kind, const char *text);
void js_store_target(struct js_function_compiler *fc, struct js_node *target, uint32_t source);

#endif
