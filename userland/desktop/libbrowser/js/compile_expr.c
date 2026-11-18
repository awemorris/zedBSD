/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JavaScript compiler's expressions: each one is computed into a
 * register its caller gives (always a temporary, so the expression may
 * use it for its own steps), using the registers above it for its parts.
 * Names are read and written where the scope pass placed them: a register,
 * a slot of an environment some hops out, or a property of the global
 * object.
 */

#include "js/compile.h"

#include <string.h>

static void expr_bigint(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_function(struct js_function_compiler *fc, struct js_node *node, uint32_t target, const uint16_t *name, size_t length);
static void expr_array(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_object(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static uint32_t expr_property_key(struct js_function_compiler *fc, const struct js_node *key);
static int expr_is_proto(const struct js_node *property);
static int expr_is_anonymous_function(const struct js_node *node);
static void expr_unary(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_typeof(struct js_function_compiler *fc, struct js_node *operand, uint32_t target);
static void expr_delete(struct js_function_compiler *fc, struct js_node *operand, uint32_t target);
static void expr_update(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static uint32_t expr_binary_opcode(int op, int *negate);
static void expr_binary(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_logical(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_nullish_jump(struct js_function_compiler *fc, uint32_t value, uint32_t label);
static void expr_conditional(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_assign(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_assign_logical(struct js_function_compiler *fc, struct js_node *node, struct js_node *left, uint32_t target);
static void expr_member_parts(struct js_function_compiler *fc, struct js_node *member, uint32_t object, uint32_t key);
static void expr_member_get(struct js_function_compiler *fc, struct js_node *member, uint32_t object, uint32_t key, uint32_t target);
static void expr_member_put(struct js_function_compiler *fc, struct js_node *member, uint32_t object, uint32_t key, uint32_t source);
static void expr_member(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_call(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_new(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static uint32_t expr_arguments(struct js_function_compiler *fc, struct js_node *list, uint32_t *count);
static void expr_throw_text(struct js_function_compiler *fc, const char *text);
static struct js_node *expr_unwrap(struct js_node *node);
static void expr_template(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_this(struct js_function_compiler *fc, uint32_t target);
static void expr_home(struct js_function_compiler *fc, const struct js_node *node, uint32_t target);
static void expr_super_call(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_new_target(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_class_method(struct js_function_compiler *fc, struct js_node *member, uint32_t constructor, uint32_t prototype);
static void expr_private_names(struct js_function_compiler *fc, struct js_node *class_node);
static void expr_private_key(struct js_function_compiler *fc, struct js_node *name, uint32_t target);
static int expr_private_seen(const struct js_node *class_node, const struct js_node *member);
static void expr_bind_leaf(struct js_function_compiler *fc, struct js_node *target, uint32_t value, int mode);
static void expr_bind_default(struct js_function_compiler *fc, struct js_node *pattern, uint32_t value, int mode);
static void expr_bind_object(struct js_function_compiler *fc, struct js_node *pattern, uint32_t value, int mode);
static void expr_bind_array(struct js_function_compiler *fc, struct js_node *pattern, uint32_t value, int mode);
static int expr_has_spread(const struct js_node *list);
static void expr_spread_list(struct js_function_compiler *fc, struct js_node *list, uint32_t array);
static void expr_optional_chain(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_optional_check(struct js_function_compiler *fc, const struct js_node *node, uint32_t value);
static void expr_tagged_template(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_delegate(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_yield(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_await(struct js_function_compiler *fc, struct js_node *node, uint32_t target);
static void expr_template_strings(struct js_function_compiler *fc, struct js_node *template, uint32_t target);
static void expr_unsupported(struct js_function_compiler *fc, struct js_node *node);

/*
 * Compiles an expression into a register.
 */
void
js_compile_expression(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	/* No name for an anonymous function to take. */
	js_compile_expression_named(fc, node, target, NULL, 0);
}

/*
 * Compiles an expression into a register; an anonymous function in it
 * takes the name (what a var, a property or an assignment names it).
 */
void
js_compile_expression_named(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target,
	const uint16_t *name,
	size_t length)
{
	uint32_t saved_line;
	uint32_t saved_column;
	uint32_t constant;

	/* The expression's instructions carry its source position. */
	js_emit_at(fc, node, &saved_line, &saved_column);

	/* Each kind of expression. */
	switch (node->kind) {
	case JS_NODE_NUMBER:
		js_load_number(fc, target, node->number);
		break;
	case JS_NODE_STRING:
		constant = js_constant_string(fc, node->text, node->text_length);
		js_emit2(fc, VM_OP_LOAD_CONST, target, constant);
		break;
	case JS_NODE_BIGINT:
		expr_bigint(fc, node, target);
		break;
	case JS_NODE_REGEXP:
		/* A new object at each evaluation, from the pattern (the body) and the flags (the raw text after it). */
		constant = js_constant_string(fc, node->text, node->text_length);
		js_emit3(fc, VM_OP_NEW_REGEXP, target, constant, js_constant_string(fc, node->raw, node->raw_length));
		break;
	case JS_NODE_TRUE:
		js_load_value(fc, target, VM_VALUE_TRUE);
		break;
	case JS_NODE_FALSE:
		js_load_value(fc, target, VM_VALUE_FALSE);
		break;
	case JS_NODE_NULL:
		js_load_value(fc, target, VM_VALUE_NULL);
		break;
	case JS_NODE_THIS:
		expr_this(fc, target);
		break;
	case JS_NODE_CLASS:
		js_compile_class(fc, node, target, name, length);
		break;
	case JS_NODE_META_PROPERTY:
		expr_new_target(fc, node, target);
		break;
	case JS_NODE_IDENTIFIER:
		js_load_binding(fc, node->text, node->text_length, target);
		break;
	case JS_NODE_PARENTHESIZED:
		js_compile_expression_named(fc, node->first, target, name, length);
		break;
	case JS_NODE_FUNCTION:
		expr_function(fc, node, target, name, length);
		break;
	case JS_NODE_ARRAY:
		expr_array(fc, node, target);
		break;
	case JS_NODE_OBJECT:
		expr_object(fc, node, target);
		break;
	case JS_NODE_UNARY:
		expr_unary(fc, node, target);
		break;
	case JS_NODE_UPDATE:
		expr_update(fc, node, target);
		break;
	case JS_NODE_BINARY:
		expr_binary(fc, node, target);
		break;
	case JS_NODE_LOGICAL:
		expr_logical(fc, node, target);
		break;
	case JS_NODE_CONDITIONAL:
		expr_conditional(fc, node, target);
		break;
	case JS_NODE_ASSIGN:
		expr_assign(fc, node, target);
		break;
	case JS_NODE_CALL:
		expr_call(fc, node, target);
		break;
	case JS_NODE_NEW:
		expr_new(fc, node, target);
		break;
	case JS_NODE_MEMBER:
		expr_member(fc, node, target);
		break;
	case JS_NODE_TEMPLATE:
		expr_template(fc, node, target);
		break;
	case JS_NODE_OPTIONAL_CHAIN:
		expr_optional_chain(fc, node, target);
		break;
	case JS_NODE_TAGGED_TEMPLATE:
		expr_tagged_template(fc, node, target);
		break;
	case JS_NODE_YIELD:
		expr_yield(fc, node, target);
		break;
	case JS_NODE_AWAIT:
		expr_await(fc, node, target);
		break;
	case JS_NODE_SEQUENCE:
		/* Each expression in order; the last one's value stays. */
		for (node = node->first; node != NULL; node = node->next)
			js_compile_expression(fc, node, target);
		break;
	default:
		expr_unsupported(fc, node);
	}

	/* The enclosing expression's later instructions carry its own position again. */
	fc->line = saved_line;
	fc->column = saved_column;
}

static void expr_load_binding_plain(struct js_function_compiler *fc, const uint16_t *name, size_t length, uint32_t target, int typeof_mode);
static void expr_load_binding_with(struct js_function_compiler *fc, const uint16_t *name, size_t length, uint32_t target, int typeof_mode);
static void expr_store_binding_plain(struct js_function_compiler *fc, const struct js_node *node, const uint16_t *name, size_t length, uint32_t source);

/* Reads a name, searching active with objects before its static binding. */
void
js_load_binding(
	struct js_function_compiler *fc,
	const uint16_t *name,
	size_t length,
	uint32_t target)
{
	expr_load_binding_with(fc, name, length, target, 0);
}

/* Reads a name from its static binding, optionally allowing a missing global for typeof. */
static void
expr_load_binding_plain(
	struct js_function_compiler *fc,
	const uint16_t *name,
	size_t length,
	uint32_t target,
	int typeof_mode)
{
	struct js_binding *binding;
	uint32_t hops;
	uint32_t key;
	uint32_t opcode;

	/* A global (or a script's top-level let or const, which the global lookup checks). */
	binding = js_scope_resolve(fc, name, length, &hops);
	if (binding == NULL) {
		key = js_constant_key(fc, name, length);
		opcode = VM_OP_GET_GLOBAL;
		if (typeof_mode)
			opcode = VM_OP_GET_GLOBAL_TYPEOF;
		js_emit2(fc, opcode, target, key);
		return;
	}
	if (binding->kind == JS_BINDING_IMPORT) {
		/* The module's slot holds a link, which reads the export's current value. */
		js_emit4(fc, VM_OP_GET_ENV, target, fc->env_register, hops, binding->location);
		js_emit2(fc, VM_OP_GET_IMPORT, target, target);
		return;
	}

	/* A captured binding in an environment, or a binding of this function's frame. */
	if (binding->in_env) {
		js_emit4(fc, VM_OP_GET_ENV, target, fc->env_register, hops, binding->location);
	} else {
		js_emit2(fc, VM_OP_MOV, target, binding->location);
	}

	/* A let or const cannot be read before its declaration runs. */
	if (binding->kind == JS_BINDING_LET || binding->kind == JS_BINDING_CONST) {
		key = js_constant_key(fc, name, length);
		js_emit2(fc, VM_OP_CHECK_INIT, target, key);
	}
}

/* Reads a name through the innermost with object that has it, then its static binding. */
static void
expr_load_binding_with(
	struct js_function_compiler *fc,
	const uint16_t *name,
	size_t length,
	uint32_t target,
	int typeof_mode)
{
	struct js_with *active;
	uint32_t mark;
	uint32_t key;
	uint32_t key_value;
	uint32_t found;
	uint32_t next;
	uint32_t end;

	/* Without a with statement, the ordinary static path is enough. */
	if (fc->with == NULL) {
		expr_load_binding_plain(fc, name, length, target, typeof_mode);
		return;
	}

	/* Each with object is searched from the inside out. */
	mark = fc->temp_top;
	key = js_constant_key(fc, name, length);
	key_value = js_temp(fc);
	found = js_temp(fc);
	js_emit2(fc, VM_OP_LOAD_CONST, key_value, key);
	end = js_label_new(fc);
	for (active = fc->with; active != NULL; active = active->outer) {
		next = js_label_new(fc);
		js_emit3(fc, VM_OP_IN, found, key_value, active->object);
		js_emit_jump(fc, VM_OP_JUMP_IF_FALSE, found, next);
		js_emit3(fc, VM_OP_GET_PROP, target, active->object, key);
		js_emit_jump(fc, VM_OP_JUMP, 0, end);
		js_label_place(fc, next);
	}

	/* A name absent from every with object comes from its static binding. */
	expr_load_binding_plain(fc, name, length, target, typeof_mode);
	js_label_place(fc, end);
	fc->temp_top = mark;
}

/*
 * Writes a register's value to a name: its register, its environment's
 * slot, or the global object.  A named function expression's own name
 * cannot be written (silently in sloppy code, a TypeError in strict code);
 * a let or const cannot be written before its declaration runs, and a
 * const not at all.
 */
void
js_store_binding(
	struct js_function_compiler *fc,
	const struct js_node *node,
	const uint16_t *name,
	size_t length,
	uint32_t source)
{
	struct js_with *active;
	uint32_t mark;
	uint32_t key;
	uint32_t key_value;
	uint32_t found;
	uint32_t next;
	uint32_t end;

	/* A matching property of the innermost with object receives the value. */
	if (fc->with == NULL) {
		expr_store_binding_plain(fc, node, name, length, source);
		return;
	}

	/* The shared key is tested against each active object. */
	mark = fc->temp_top;
	key = js_constant_key(fc, name, length);
	key_value = js_temp(fc);
	found = js_temp(fc);
	js_emit2(fc, VM_OP_LOAD_CONST, key_value, key);
	end = js_label_new(fc);
	for (active = fc->with; active != NULL; active = active->outer) {
		next = js_label_new(fc);
		js_emit3(fc, VM_OP_IN, found, key_value, active->object);
		js_emit_jump(fc, VM_OP_JUMP_IF_FALSE, found, next);
		js_emit3(fc, VM_OP_PUT_PROP, active->object, key, source);
		js_emit_jump(fc, VM_OP_JUMP, 0, end);
		js_label_place(fc, next);
	}

	/* A name absent from every with object is written to its static binding. */
	expr_store_binding_plain(fc, node, name, length, source);
	js_label_place(fc, end);
	fc->temp_top = mark;
}

/* Writes a register to the name's static binding. */
static void
expr_store_binding_plain(
	struct js_function_compiler *fc,
	const struct js_node *node,
	const uint16_t *name,
	size_t length,
	uint32_t source)
{
	struct js_binding *binding;
	uint32_t hops;
	uint32_t key;
	uint32_t mark;
	uint32_t current;

	UNUSED_PARAMETER(node);

	/* A global (or a script's top-level let or const, which the global assignment checks). */
	binding = js_scope_resolve(fc, name, length, &hops);
	if (binding == NULL) {
		key = js_constant_key(fc, name, length);
		js_emit2(fc, VM_OP_PUT_GLOBAL, key, source);
		return;
	}
	if (binding->kind == JS_BINDING_IMPORT) {
		js_emit_throw_error(fc, VM_ERROR_TYPE, "Assignment to import binding.");
		return;
	}

	/* A function expression's own name stays the function. */
	if (binding->kind == JS_BINDING_CALLEE) {
		if (fc->info->strict)
			expr_throw_text(fc, "TypeError: Assignment to constant variable.");
		return;
	}

	/* A let or const: its declaration must have run, and a const is never assigned. */
	if (binding->kind == JS_BINDING_LET || binding->kind == JS_BINDING_CONST) {
		mark = fc->temp_top;
		current = js_temp(fc);
		js_load_binding(fc, name, length, current);
		fc->temp_top = mark;
		if (binding->kind == JS_BINDING_CONST) {
			js_emit_throw_error(fc, VM_ERROR_TYPE, "Assignment to constant variable.");
			return;
		}
	}

	/* A captured binding in an environment. */
	if (binding->in_env) {
		js_emit4(fc, VM_OP_PUT_ENV, fc->env_register, hops, binding->location, source);
		return;
	}

	/* A binding of this function's frame. */
	js_emit2(fc, VM_OP_MOV, binding->location, source);
}

/*
 * Runs the declaration of a let or const: the name takes its first value
 * (a const too), in its register, its environment's slot, or the realm's
 * record of the scripts' top-level ones.
 */
void
js_init_binding(
	struct js_function_compiler *fc,
	const uint16_t *name,
	size_t length,
	uint32_t source)
{
	struct js_binding *binding;
	uint32_t hops;
	uint32_t key;

	/* The program's top level: the realm's record. */
	binding = js_scope_resolve(fc, name, length, &hops);
	if (binding == NULL) {
		key = js_constant_key(fc, name, length);
		js_emit2(fc, VM_OP_INIT_GLOBAL_LEXICAL, key, source);
		return;
	}

	/* A captured binding in an environment. */
	if (binding->in_env) {
		js_emit4(fc, VM_OP_PUT_ENV, fc->env_register, hops, binding->location, source);
		return;
	}

	/* A binding of this function's frame. */
	js_emit2(fc, VM_OP_MOV, binding->location, source);
}

/*
 * Binds the names of a pattern (an array or object pattern, a name, or
 * for an assignment any target) to the parts of a register's value.
 */
void
js_bind_pattern(
	struct js_function_compiler *fc,
	struct js_node *target,
	uint32_t value,
	int mode)
{
	struct js_node *place;

	/* The kind of target. */
	place = expr_unwrap(target);
	switch (place->kind) {
	case JS_NODE_ARRAY_PATTERN:
		expr_bind_array(fc, place, value, mode);
		break;
	case JS_NODE_OBJECT_PATTERN:
		expr_bind_object(fc, place, value, mode);
		break;
	case JS_NODE_ASSIGNMENT_PATTERN:
		expr_bind_default(fc, place, value, mode);
		break;
	default:
		expr_bind_leaf(fc, place, value, mode);
		break;
	}
}

/*
 * Compiles a class (a declaration's or an expression's) into a register:
 * its constructor with its prototype, its methods and accessors, its own
 * name for a named expression, then its static fields and blocks; name
 * is the name an anonymous class takes.
 */
void
js_compile_class(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target,
	const uint16_t *name,
	size_t length)
{
	struct js_scope *saved_scope;
	struct vm_code *code;
	struct js_node *member;
	uint32_t saved_env;
	uint32_t mark;
	uint32_t parent;
	uint32_t constructor;
	uint32_t prototype;
	uint32_t statics;
	uint32_t constant;

	/* The heritage, or the empty value without one. */
	mark = fc->temp_top;
	parent = js_temp(fc);
	constructor = js_temp(fc);
	prototype = js_temp(fc);
	statics = js_temp(fc);
	if (node->first != NULL) {
		js_compile_expression(fc, node->first, parent);
	} else {
		js_emit1(fc, VM_OP_LOAD_EMPTY, parent);
	}

	/* A named class expression's scope, with its own name. */
	saved_scope = fc->scope;
	saved_env = fc->env_register;
	if (node->scope != NULL)
		js_scope_enter(fc, node->scope, &saved_scope, &saved_env);

	/* The private names, each a new one at each evaluation of the class. */
	expr_private_names(fc, node);

	/* The constructor, named after the class. */
	if (node->text != NULL) {
		name = node->text;
		length = node->text_length;
	}

	/* The constructor's code. */
	code = js_compile_function(fc->compiler, fc, node->third, name, length);
	constant = js_constant(fc, vm_value_cell(code));
	js_emit3(fc, VM_OP_NEW_CLOSURE, constructor, constant, fc->env_register);

	/* The prototype from the heritage, and the constructor's own prototype. */
	js_emit3(fc, VM_OP_CLASS_SETUP, prototype, constructor, parent);

	/* In order: the methods and accessors (on the prototype or, static, on the constructor) and the computed field names. */
	for (member = node->second; member != NULL; member = member->next) {
		if (member->kind == JS_NODE_FIELD && (member->flags & JS_FLAG_COMPUTED) != 0U) {
			js_compile_expression(fc, member->first, statics);
			js_emit2(fc, VM_OP_TO_PROPERTY_KEY, statics, statics);
			js_init_binding(fc, member->raw, member->raw_length, statics);
			continue;
		}

		/* Only methods are left (the constructor was made above). */
		if (member->kind != JS_NODE_METHOD || member->op == JS_PROPERTY_CONSTRUCTOR)
			continue;
		expr_class_method(fc, member, constructor, prototype);
	}

	/* A named class's own inner name takes the class. */
	if (node->text != NULL)
		js_init_binding(fc, node->text, node->text_length, constructor);

	/* The static fields and blocks run with the class as this. */
	if (node->fourth != NULL) {
		code = js_compile_function(fc->compiler, fc, node->fourth, NULL, 0);
		constant = js_constant(fc, vm_value_cell(code));
		js_emit3(fc, VM_OP_NEW_CLOSURE, statics, constant, fc->env_register);
		js_emit5(fc, VM_OP_CALL, statics, statics, constructor, fc->temp_top, 0);
	}

	/* The class is the value; the scope around it again. */
	js_emit2(fc, VM_OP_MOV, target, constructor);
	if (node->scope != NULL)
		js_scope_leave(fc, saved_scope, saved_env);
	fc->temp_top = mark;
}

/*
 * Defines a class's fields on this: the instance fields in a constructor,
 * or (statics) the static fields in the static function, where the static
 * blocks run in order among them.
 */
void
js_compile_fields(
	struct js_function_compiler *fc,
	struct js_node *class_node,
	int statics)
{
	struct js_node *member;
	uint32_t mark;
	uint32_t object;
	uint32_t value;
	uint32_t key;
	const uint16_t *name;
	size_t length;
	uint32_t constant;
	int is_static;
	int anonymous;
	int named;
	int seen;

	/* this, the object the fields go on. */
	mark = fc->temp_top;
	object = js_temp(fc);
	value = js_temp(fc);
	key = js_temp(fc);
	expr_this(fc, object);

	/* An instance first gets the private methods and accessors the prototype (the constructor's home object) keeps. */
	for (member = class_node->second; member != NULL && !statics; member = member->next) {
		if (member->kind != JS_NODE_METHOD || (member->flags & JS_FLAG_STATIC) != 0U)
			continue;
		if (member->first->kind != JS_NODE_PRIVATE_NAME)
			continue;
		seen = expr_private_seen(class_node, member);
		if (seen)
			continue;
		expr_private_key(fc, member->first, key);
		js_emit1(fc, VM_OP_LOAD_HOME, value);
		js_emit3(fc, VM_OP_PRIVATE_COPY, object, value, key);
	}

	/* Each field (and static block) of the kind asked for, in order. */
	for (member = class_node->second; member != NULL; member = member->next) {
		is_static = 0;
		if ((member->flags & JS_FLAG_STATIC) != 0U || member->kind == JS_NODE_STATIC_BLOCK)
			is_static = 1;
		if (is_static != statics)
			continue;

		/* A static block's statements. */
		if (member->kind == JS_NODE_STATIC_BLOCK) {
			js_compile_statements(fc, member->first);
			continue;
		}

		/* Only fields are left. */
		if (member->kind != JS_NODE_FIELD)
			continue;

		/* The value (an anonymous function takes a plain or private name now, a computed or numeric one when it runs), or undefined. */
		named = 0;
		if ((member->flags & JS_FLAG_COMPUTED) == 0U &&
		    (member->first->kind == JS_NODE_IDENTIFIER || member->first->kind == JS_NODE_STRING || member->first->kind == JS_NODE_PRIVATE_NAME))
			named = 1;
		anonymous = 0;
		if (member->second != NULL) {
			anonymous = expr_is_anonymous_function(member->second);
			if (named && member->first->kind == JS_NODE_PRIVATE_NAME) {
				name = js_private_name(fc->compiler, member->first, &length);
				js_compile_expression_named(fc, member->second, value, name, length);
			} else if (named) {
				js_compile_expression_named(fc, member->second, value, member->first->text, member->first->text_length);
			} else {
				js_compile_expression(fc, member->second, value);
			}
		} else {
			js_load_value(fc, value, VM_VALUE_UNDEFINED);
		}

		/* A computed name's field: the key the class evaluated. */
		if ((member->flags & JS_FLAG_COMPUTED) != 0U) {
			js_load_binding(fc, member->raw, member->raw_length, key);
			if (anonymous)
				js_emit3(fc, VM_OP_SET_FUNCTION_NAME, value, key, 0);
			js_emit3(fc, VM_OP_DEFINE_ELEM, object, key, value);
			continue;
		}

		/* A private field on this. */
		if (member->first->kind == JS_NODE_PRIVATE_NAME) {
			expr_private_key(fc, member->first, key);
			js_emit3(fc, VM_OP_PRIVATE_DEFINE, object, key, value);
			continue;
		}

		/* A data property on this; a numeric key names an anonymous function when it runs. */
		constant = expr_property_key(fc, member->first);
		if (!named && anonymous) {
			js_emit2(fc, VM_OP_LOAD_CONST, key, constant);
			js_emit3(fc, VM_OP_SET_FUNCTION_NAME, value, key, 0);
		}
		js_emit3(fc, VM_OP_DEFINE_PROP, object, constant, value);
	}

	/* The temporaries are free again. */
	fc->temp_top = mark;
}

/*
 * Throws a new error of a kind (enum vm_error_kind) with a message when
 * the code runs.
 */
void
js_emit_throw_error(
	struct js_function_compiler *fc,
	int kind,
	const char *text)
{
	struct vm_string *string;
	uint32_t constant;

	/* The message as a constant. */
	string = vm_string_from_utf8(fc->compiler->realm->heap, text, strlen(text));
	if (string == NULL)
		js_compile_out_of_memory(fc->compiler);
	constant = js_constant(fc, vm_value_cell(string));

	/* The throw. */
	js_emit2(fc, VM_OP_THROW_ERROR, (uint32_t)kind, constant);
}

/*
 * Writes a register's value to an assignment target: a name or a
 * property.  Any other target (a call, in sloppy code) is a ReferenceError
 * when the assignment runs.
 */
void
js_store_target(
	struct js_function_compiler *fc,
	struct js_node *target,
	uint32_t source)
{
	struct js_node *place;
	uint32_t mark;
	uint32_t object;
	uint32_t key;

	/* A name. */
	place = expr_unwrap(target);
	if (place->kind == JS_NODE_IDENTIFIER) {
		js_store_binding(fc, place, place->text, place->text_length, source);
		return;
	}

	/* A property: its object and key, then the write. */
	if (place->kind == JS_NODE_MEMBER) {
		mark = fc->temp_top;
		object = js_temp(fc);
		key = js_temp(fc);
		expr_member_parts(fc, place, object, key);
		expr_member_put(fc, place, object, key, source);
		fc->temp_top = mark;
		return;
	}

	/* A pattern takes the parts of the value. */
	if (place->kind == JS_NODE_ARRAY_PATTERN || place->kind == JS_NODE_OBJECT_PATTERN) {
		js_bind_pattern(fc, place, source, JS_BIND_ASSIGN);
		return;
	}

	/* Anything else is evaluated, then cannot be assigned. */
	mark = fc->temp_top;
	object = js_temp(fc);
	js_compile_expression(fc, place, object);
	expr_throw_text(fc, "ReferenceError: Invalid left-hand side in assignment");
	fc->temp_top = mark;
}

/* Reads this: an arrow function's is the one of the function around it (its hidden binding). */
static void
expr_this(
	struct js_function_compiler *fc,
	uint32_t target)
{
	/* An arrow function reads the this of the function around it. */
	if ((fc->info->node->flags & JS_FLAG_ARROW) != 0U && !fc->info->program) {
		js_load_binding(fc, js_this_name, JS_THIS_NAME_LENGTH, target);
		return;
	}

	/* Any other function its own. */
	js_emit1(fc, VM_OP_LOAD_THIS, target);
}

/* Reads the home object for super: a method's own, or for an arrow function the method's around it. */
static void
expr_home(
	struct js_function_compiler *fc,
	const struct js_node *node,
	uint32_t target)
{
	UNUSED_PARAMETER(node);

	/* An arrow function reads the hidden binding of the method around it. */
	if ((fc->info->node->flags & JS_FLAG_ARROW) != 0U && !fc->info->program) {
		js_load_binding(fc, js_home_name, JS_HOME_NAME_LENGTH, target);
		return;
	}

	/* A method its own. */
	js_emit1(fc, VM_OP_LOAD_HOME, target);
}

/*
 * Compiles super(...) in a derived class's constructor: the parent
 * constructs, the object becomes this (and the hidden this of the arrow
 * functions inside), then the instance fields are defined on it.
 */
static void
expr_super_call(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	struct js_binding *binding;
	uint32_t mark;
	uint32_t arguments;
	uint32_t first;
	uint32_t count;
	uint32_t hops;
	int spread;

	/* Only a derived class's constructor itself (an arrow function's super call comes later). */
	if (fc->info->class_node == NULL || fc->info->class_node->first == NULL)
		js_compile_unsupported(fc->compiler, node, "super outside a derived class's constructor");

	/* The construction, with the arguments in registers or, with a spread, an array. */
	mark = fc->temp_top;
	spread = expr_has_spread(node->second);
	if (spread) {
		arguments = js_temp(fc);
		expr_spread_list(fc, node->second, arguments);
		js_emit2(fc, VM_OP_SUPER_CONSTRUCT_ARRAY, target, arguments);
	} else {
		first = expr_arguments(fc, node->second, &count);
		js_emit3(fc, VM_OP_SUPER_CONSTRUCT, target, first, count);
	}

	/* The temporaries are free again. */
	fc->temp_top = mark;

	/* The arrow functions inside read this through the hidden binding, set now. */
	binding = js_scope_resolve(fc, js_this_name, JS_THIS_NAME_LENGTH, &hops);
	if (binding != NULL && binding->kind == JS_BINDING_THIS)
		js_store_binding(fc, node, js_this_name, JS_THIS_NAME_LENGTH, target);

	/* The instance fields, on the object just made. */
	js_compile_fields(fc, fc->info->class_node, 0);
}

/* Reads new.target (import.meta comes with modules). */
static void
expr_new_target(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	int is_target;

	/* Only new.target, in a function that is not an arrow function. */
	is_target = js_text_is(node->text, node->text_length, "target");
	if (!is_target)
		expr_unsupported(fc, node);
	if ((fc->info->node->flags & JS_FLAG_ARROW) != 0U || fc->info->program)
		js_compile_unsupported(fc->compiler, node, "new.target outside a function");

	/* The frame's new.target. */
	js_emit1(fc, VM_OP_LOAD_NEW_TARGET, target);
}

/* Makes a class's private names (one per name, shared by a getter and a setter) and binds them. */
static void
expr_private_names(
	struct js_function_compiler *fc,
	struct js_node *class_node)
{
	struct js_node *member;
	const uint16_t *text;
	size_t length;
	uint32_t mark;
	uint32_t made;
	uint32_t constant;
	int seen;

	/* Each member with a private name not seen before it. */
	mark = fc->temp_top;
	made = js_temp(fc);
	for (member = class_node->second; member != NULL; member = member->next) {
		if (member->kind != JS_NODE_METHOD && member->kind != JS_NODE_FIELD)
			continue;
		if (member->first->kind != JS_NODE_PRIVATE_NAME)
			continue;
		seen = expr_private_seen(class_node, member);
		if (seen)
			continue;

		/* A new private name described as #x, bound to the hidden const. */
		text = js_private_name(fc->compiler, member->first, &length);
		constant = js_constant_string(fc, text, length);
		js_emit2(fc, VM_OP_NEW_PRIVATE_NAME, made, constant);
		js_init_binding(fc, text, length, made);
	}

	/* The temporaries are free again. */
	fc->temp_top = mark;
}

/* Loads a private name's key (the value of its class's hidden const) into a register. */
static void
expr_private_key(
	struct js_function_compiler *fc,
	struct js_node *name,
	uint32_t target)
{
	const uint16_t *text;
	size_t length;

	/* The hidden binding "#x". */
	text = js_private_name(fc->compiler, name, &length);
	js_load_binding(fc, text, length, target);
}

/* Tells whether a class member's private name was declared by an earlier member (a getter and its setter). */
static int
expr_private_seen(
	const struct js_node *class_node,
	const struct js_node *member)
{
	const struct js_node *earlier;
	int same;

	/* Each member before it with a private name. */
	for (earlier = class_node->second; earlier != member; earlier = earlier->next) {
		if (earlier->kind != JS_NODE_METHOD && earlier->kind != JS_NODE_FIELD)
			continue;
		if (earlier->first->kind != JS_NODE_PRIVATE_NAME)
			continue;
		same = js_text_equal(earlier->first->text, earlier->first->text_length, member->first->text, member->first->text_length);
		if (same)
			return 1;
	}

	/* None has it. */
	return 0;
}

/* Compiles one method, getter or setter of a class onto its prototype or (static) its constructor. */
static void
expr_class_method(
	struct js_function_compiler *fc,
	struct js_node *member,
	uint32_t constructor,
	uint32_t prototype)
{
	struct vm_code *code;
	const uint16_t *name;
	size_t length;
	uint32_t mark;
	uint32_t key;
	uint32_t function;
	uint32_t home;
	uint32_t kind;
	uint32_t constant;

	/* The key: computed into a register, or a constant loaded into it. */
	mark = fc->temp_top;
	key = js_temp(fc);
	function = js_temp(fc);
	name = NULL;
	length = 0;
	if ((member->flags & JS_FLAG_COMPUTED) != 0U) {
		js_compile_expression(fc, member->first, key);
		js_emit2(fc, VM_OP_TO_PROPERTY_KEY, key, key);
	} else if (member->first->kind == JS_NODE_PRIVATE_NAME) {
		expr_private_key(fc, member->first, key);
		name = js_private_name(fc->compiler, member->first, &length);
	} else {
		constant = expr_property_key(fc, member->first);
		js_emit2(fc, VM_OP_LOAD_CONST, key, constant);
		if (member->first->kind == JS_NODE_IDENTIFIER || member->first->kind == JS_NODE_STRING) {
			name = member->first->text;
			length = member->first->text_length;
		}
	}

	/* The function, named after a plain key. */
	code = js_compile_function(fc->compiler, fc, member->second, name, length);
	constant = js_constant(fc, vm_value_cell(code));
	js_emit3(fc, VM_OP_NEW_CLOSURE, function, constant, fc->env_register);

	/* What it is: a method, or a getter's or setter's half. */
	kind = 0;
	if (member->op == JS_PROPERTY_GET)
		kind = 1;
	else if (member->op == JS_PROPERTY_SET)
		kind = 2;

	/* A computed or numeric key's method, and an accessor's half ("get " or "set " before it), take the key's name when they run. */
	if (name == NULL || kind != 0U)
		js_emit3(fc, VM_OP_SET_FUNCTION_NAME, function, key, kind);

	/* Where it goes. */
	home = prototype;
	if ((member->flags & JS_FLAG_STATIC) != 0U)
		home = constructor;
	js_emit4(fc, VM_OP_DEFINE_METHOD, home, key, function, kind);
	fc->temp_top = mark;
}

/*
 * Compiles an optional chain: its value, or undefined when a value before
 * one of its ?. is undefined or null (the rest of the chain is skipped).
 */
static void
expr_optional_chain(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	uint32_t saved;
	uint32_t end;

	/* The chain's own place to leave to (a chain inside it has its own). */
	saved = fc->chain_label;
	fc->chain_label = js_label_new(fc);
	end = js_label_new(fc);

	/* The chain's value. */
	js_compile_expression(fc, node->first, target);
	js_emit_jump(fc, VM_OP_JUMP, 0, end);

	/* Leaving it early gives undefined. */
	js_label_place(fc, fc->chain_label);
	js_load_value(fc, target, VM_VALUE_UNDEFINED);
	js_label_place(fc, end);
	fc->chain_label = saved;
}

/* Leaves the optional chain when a node has ?. and the value before it is undefined or null. */
static void
expr_optional_check(
	struct js_function_compiler *fc,
	const struct js_node *node,
	uint32_t value)
{
	uint32_t mark;
	uint32_t test;

	/* Only a node written with ?. inside a chain. */
	if ((node->flags & JS_FLAG_OPTIONAL) == 0U)
		return;
	if (fc->chain_label == JS_LABEL_UNPLACED)
		return;

	/* == null holds exactly for undefined and null. */
	mark = fc->temp_top;
	test = js_temp(fc);
	js_load_value(fc, test, VM_VALUE_NULL);
	js_emit3(fc, VM_OP_LOOSE_EQ, test, value, test);
	js_emit_jump(fc, VM_OP_JUMP_IF_TRUE, test, fc->chain_label);
	fc->temp_top = mark;
}

/*
 * Compiles a template literal: its strings and its substitutions (each
 * converted with ToString) joined in order.
 */
static void
expr_template(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	struct js_node *part;
	uint32_t mark;
	uint32_t joined;
	uint32_t piece;
	uint32_t constant;
	int first;

	/* The text so far and the next piece, in temporaries of their own. */
	mark = fc->temp_top;
	joined = js_temp(fc);
	piece = js_temp(fc);

	/* Each part in turn: a string as it is, a substitution converted to a string. */
	first = 1;
	for (part = node->first; part != NULL; part = part->next) {
		if (part->kind == JS_NODE_TEMPLATE_STRING) {
			constant = js_constant_string(fc, part->text, part->text_length);
			js_emit2(fc, VM_OP_LOAD_CONST, piece, constant);
		} else {
			js_compile_expression(fc, part, piece);
			js_emit2(fc, VM_OP_TO_STRING, piece, piece);
		}

		/* The first piece starts the text; each later one is joined to it. */
		if (first) {
			js_emit2(fc, VM_OP_MOV, joined, piece);
			first = 0;
		} else {
			js_emit3(fc, VM_OP_ADD, joined, joined, piece);
		}
	}

	/* A template without parts is the empty string. */
	if (first) {
		constant = js_constant_string(fc, node->text, 0);
		js_emit2(fc, VM_OP_LOAD_CONST, joined, constant);
	}

	/* The text is the value. */
	js_emit2(fc, VM_OP_MOV, target, joined);
	fc->temp_top = mark;
}

/*
 * Compiles a tagged template: the tag is called (with a property's object
 * as this) with the array of the strings, whose raw property is the array
 * of their raw texts, and then each substitution's value.
 */
static void
expr_tagged_template(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	struct js_node *tag;
	struct js_node *part;
	uint32_t mark;
	uint32_t function;
	uint32_t this_value;
	uint32_t key;
	uint32_t first;
	uint32_t count;

	/* The function and this: a property's object, or undefined. */
	mark = fc->temp_top;
	function = js_temp(fc);
	this_value = js_temp(fc);
	tag = expr_unwrap(node->first);
	if (tag->kind == JS_NODE_MEMBER) {
		key = js_temp(fc);
		expr_member_parts(fc, tag, this_value, key);
		expr_member_get(fc, tag, this_value, key, function);
	} else {
		js_compile_expression(fc, node->first, function);
		js_load_value(fc, this_value, VM_VALUE_UNDEFINED);
	}

	/* The arguments' registers, which follow each other: the strings, then one per substitution. */
	first = fc->temp_top;
	count = 1;
	js_temp(fc);
	for (part = node->second->first; part != NULL; part = part->next) {
		if (part->kind == JS_NODE_TEMPLATE_STRING)
			continue;
		js_temp(fc);
		count++;
	}

	/* The strings, then each substitution's value. */
	expr_template_strings(fc, node->second, first);
	count = 1;
	for (part = node->second->first; part != NULL; part = part->next) {
		if (part->kind == JS_NODE_TEMPLATE_STRING)
			continue;
		js_compile_expression(fc, part, first + count);
		count++;
	}

	/* The call. */
	js_emit5(fc, VM_OP_CALL, target, function, this_value, first, count);
	fc->temp_top = mark;
}

/*
 * Makes a tagged template's array of strings: the cooked strings
 * (undefined for one whose escapes are not valid), with the array of the
 * raw texts as its raw property.  A new array is made at each call (the
 * standard keeps one per place in the source, frozen).
 */
static void
expr_template_strings(
	struct js_function_compiler *fc,
	struct js_node *template,
	uint32_t target)
{
	static const uint16_t raw_name[] = { 'r', 'a', 'w' };
	struct js_node *part;
	uint32_t mark;
	uint32_t raw;
	uint32_t value;
	uint32_t constant;

	/* The two arrays. */
	mark = fc->temp_top;
	raw = js_temp(fc);
	value = js_temp(fc);
	js_emit1(fc, VM_OP_NEW_ARRAY, target);
	js_emit1(fc, VM_OP_NEW_ARRAY, raw);

	/* Each string's cooked and raw text. */
	for (part = template->first; part != NULL; part = part->next) {
		if (part->kind != JS_NODE_TEMPLATE_STRING)
			continue;

		/* The cooked text, or undefined. */
		if ((part->flags & JS_FLAG_INVALID_COOKED) != 0U) {
			js_load_value(fc, value, VM_VALUE_UNDEFINED);
		} else {
			constant = js_constant_string(fc, part->text, part->text_length);
			js_emit2(fc, VM_OP_LOAD_CONST, value, constant);
		}

		/* At the end of the strings. */
		js_emit2(fc, VM_OP_ARRAY_PUSH, target, value);

		/* The raw text. */
		constant = js_constant_string(fc, part->raw, part->raw_length);
		js_emit2(fc, VM_OP_LOAD_CONST, value, constant);
		js_emit2(fc, VM_OP_ARRAY_PUSH, raw, value);
	}

	/* The raw texts on the strings. */
	constant = js_constant_key(fc, raw_name, 3);
	js_emit3(fc, VM_OP_DEFINE_PROP, target, constant, raw);
	fc->temp_top = mark;
}

/* Writes a value to one name (or, for an assignment, one target) of a pattern. */
static void
expr_bind_leaf(
	struct js_function_compiler *fc,
	struct js_node *target,
	uint32_t value,
	int mode)
{
	/* A let's or const's name is declared. */
	if (mode == JS_BIND_INIT && target->kind == JS_NODE_IDENTIFIER) {
		js_init_binding(fc, target->text, target->text_length, value);
		return;
	}

	/* A var's name, or any target of an assignment. */
	js_store_target(fc, target, value);
}

/* Binds a pattern with a default: the default replaces a value that is undefined. */
static void
expr_bind_default(
	struct js_function_compiler *fc,
	struct js_node *pattern,
	uint32_t value,
	int mode)
{
	struct js_node *inner;
	uint32_t mark;
	uint32_t chosen;
	uint32_t test;
	uint32_t given;

	/* The value, kept apart from the register it came in. */
	mark = fc->temp_top;
	chosen = js_temp(fc);
	test = js_temp(fc);
	js_emit2(fc, VM_OP_MOV, chosen, value);

	/* Only undefined takes the default (an anonymous function takes a name's name). */
	given = js_label_new(fc);
	js_load_value(fc, test, VM_VALUE_UNDEFINED);
	js_emit3(fc, VM_OP_STRICT_EQ, test, chosen, test);
	js_emit_jump(fc, VM_OP_JUMP_IF_FALSE, test, given);
	inner = expr_unwrap(pattern->first);
	if (inner->kind == JS_NODE_IDENTIFIER) {
		js_compile_expression_named(fc, pattern->second, chosen, inner->text, inner->text_length);
	} else {
		js_compile_expression(fc, pattern->second, chosen);
	}

	/* The default was taken, or the value was not undefined. */
	js_label_place(fc, given);

	/* The target takes the value chosen. */
	js_bind_pattern(fc, pattern->first, chosen, mode);
	fc->temp_top = mark;
}

/*
 * Binds an object pattern: each property's target takes the value's
 * property, and a rest takes a new object of the other own enumerable
 * properties.  undefined and null cannot be destructured.
 */
static void
expr_bind_object(
	struct js_function_compiler *fc,
	struct js_node *pattern,
	uint32_t value,
	int mode)
{
	struct js_node *property;
	uint32_t mark;
	uint32_t excluded;
	uint32_t key;
	uint32_t part;
	uint32_t constant;
	int has_rest;

	/* undefined and null have no properties to take. */
	js_emit1(fc, VM_OP_CHECK_COERCIBLE, value);

	/* With a rest, the keys taken are listed to leave them out of it. */
	mark = fc->temp_top;
	excluded = js_temp(fc);
	key = js_temp(fc);
	part = js_temp(fc);
	has_rest = 0;
	for (property = pattern->first; property != NULL; property = property->next) {
		if (property->kind == JS_NODE_REST)
			has_rest = 1;
	}

	/* A rest needs the list of the keys taken. */
	if (has_rest)
		js_emit1(fc, VM_OP_NEW_ARRAY, excluded);

	/* Each property in order. */
	for (property = pattern->first; property != NULL; property = property->next) {
		/* The rest: the properties not taken. */
		if (property->kind == JS_NODE_REST) {
			js_emit3(fc, VM_OP_OBJECT_REST, part, value, excluded);
			js_bind_pattern(fc, property->first, part, mode);
			continue;
		}

		/* The key: computed into a register, or a constant. */
		if ((property->flags & JS_FLAG_COMPUTED) != 0U) {
			js_compile_expression(fc, property->first, key);
			js_emit2(fc, VM_OP_TO_PROPERTY_KEY, key, key);
			js_emit3(fc, VM_OP_GET_ELEM, part, value, key);
		} else {
			constant = expr_property_key(fc, property->first);
			js_emit2(fc, VM_OP_LOAD_CONST, key, constant);
			js_emit3(fc, VM_OP_GET_PROP, part, value, constant);
		}

		/* A key taken is left out of the rest. */
		if (has_rest)
			js_emit2(fc, VM_OP_ARRAY_PUSH, excluded, key);

		/* The property's target takes its value. */
		js_bind_pattern(fc, property->second, part, mode);
	}

	/* The temporaries are free again. */
	fc->temp_top = mark;
}

/*
 * Binds an array pattern: the value is iterated, each element's target
 * takes the next value (undefined past the end), a hole skips one, and a
 * rest takes an array of the values left.
 */
static void
expr_bind_array(
	struct js_function_compiler *fc,
	struct js_node *pattern,
	uint32_t value,
	int mode)
{
	struct js_node *element;
	uint32_t mark;
	uint32_t iterator;
	uint32_t part;
	uint32_t caught;
	uint32_t start;
	uint32_t end;
	uint32_t after;
	uint32_t landing;

	/* The iteration of the value. */
	mark = fc->temp_top;
	iterator = js_temp(fc);
	part = js_temp(fc);
	caught = js_temp(fc);
	js_emit2(fc, VM_OP_ITER_START, iterator, value);
	start = js_here(fc);

	/* Each element in order. */
	for (element = pattern->first; element != NULL; element = element->next) {
		/* The rest: an array of the values left. */
		if (element->kind == JS_NODE_REST) {
			js_emit2(fc, VM_OP_ITER_REST, part, iterator);
			js_bind_pattern(fc, element->first, part, mode);
			continue;
		}

		/* The next value, which a hole skips. */
		js_emit2(fc, VM_OP_ITER_NEXT, part, iterator);
		if (element->kind == JS_NODE_HOLE)
			continue;

		/* The element's target takes it. */
		js_bind_pattern(fc, element, part, mode);
	}

	/* The end of what the targets did. */
	end = js_here(fc);

	/* An iteration the pattern did not finish is closed (ws074-p087). */
	js_emit2(fc, VM_OP_ITER_CLOSE, iterator, 0);
	after = js_label_new(fc);
	js_emit_jump(fc, VM_OP_JUMP, 0, after);

	/* An exception while the targets took their values closes it too, quietly, and goes on. */
	landing = js_label_new(fc);
	js_label_place(fc, landing);
	js_emit_handler(fc, start, end, landing, caught);
	js_emit2(fc, VM_OP_ITER_CLOSE, iterator, 1);
	js_emit1(fc, VM_OP_THROW, caught);
	js_label_place(fc, after);

	/* The temporaries are free again. */
	fc->temp_top = mark;
}

/* Tells whether a list of arguments has a spread. */
static int
expr_has_spread(
	const struct js_node *list)
{
	/* Each argument. */
	for (; list != NULL; list = list->next) {
		if (list->kind == JS_NODE_SPREAD)
			return 1;
	}

	/* None is a spread. */
	return 0;
}

/* Gathers a list of arguments with spreads into a new array. */
static void
expr_spread_list(
	struct js_function_compiler *fc,
	struct js_node *list,
	uint32_t array)
{
	struct js_node *argument;
	uint32_t mark;
	uint32_t value;

	/* The array. */
	js_emit1(fc, VM_OP_NEW_ARRAY, array);

	/* Each argument: a spread's values, or the value. */
	mark = fc->temp_top;
	value = js_temp(fc);
	for (argument = list; argument != NULL; argument = argument->next) {
		if (argument->kind == JS_NODE_SPREAD) {
			js_compile_expression(fc, argument->first, value);
			js_emit2(fc, VM_OP_ARRAY_SPREAD, array, value);
		} else {
			js_compile_expression(fc, argument, value);
			js_emit2(fc, VM_OP_ARRAY_PUSH, array, value);
		}
	}

	/* The temporaries are free again. */
	fc->temp_top = mark;
}

/* Compiles a function expression: its code unit, and a closure over the running environment. */
static void
expr_function(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target,
	const uint16_t *name,
	size_t length)
{
	struct vm_code *code;
	uint32_t constant;

	/* A function with its own name keeps it; an anonymous one takes the given name. */
	if (node->text != NULL) {
		name = NULL;
		length = 0;
	}

	/* The code unit, a constant of this one. */
	code = js_compile_function(fc->compiler, fc, node, name, length);
	constant = js_constant(fc, vm_value_cell(code));

	/* The closure. */
	js_emit3(fc, VM_OP_NEW_CLOSURE, target, constant, fc->env_register);
}

/* Compiles an array literal: each element pushed in order, a hole leaving a gap. */
static void
expr_array(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	struct js_node *element;
	uint32_t mark;
	uint32_t value;

	/* The array. */
	js_emit1(fc, VM_OP_NEW_ARRAY, target);

	/* Each element. */
	mark = fc->temp_top;
	value = js_temp(fc);
	for (element = node->first; element != NULL; element = element->next) {
		if (element->kind == JS_NODE_HOLE) {
			js_emit1(fc, VM_OP_ARRAY_HOLE, target);
			continue;
		}

		/* A spread appends every value of its iterable. */
		if (element->kind == JS_NODE_SPREAD) {
			js_compile_expression(fc, element->first, value);
			js_emit2(fc, VM_OP_ARRAY_SPREAD, target, value);
			continue;
		}

		/* The value at the end. */
		js_compile_expression(fc, element, value);
		js_emit2(fc, VM_OP_ARRAY_PUSH, target, value);
	}

	/* The temporaries are free again. */
	fc->temp_top = mark;
}

/* Compiles an object literal: each property defined in order (accessors in halves, __proto__ as the prototype). */
static void
expr_object(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	struct js_node *property;
	struct js_node *key_node;
	uint32_t mark;
	uint32_t key;
	uint32_t value;
	uint32_t constant;
	uint32_t prefix;
	int computed;
	int anonymous;
	int proto;
	int named;

	/* The object. */
	js_emit1(fc, VM_OP_NEW_OBJECT, target);

	/* Each property. */
	mark = fc->temp_top;
	key = js_temp(fc);
	value = js_temp(fc);
	for (property = node->first; property != NULL; property = property->next) {
		/* A spread copies the own enumerable properties of its value. */
		if (property->op == JS_PROPERTY_SPREAD) {
			js_compile_expression(fc, property->second, value);
			js_emit2(fc, VM_OP_COPY_DATA, target, value);
			continue;
		}

		/* The key: computed into a register, or a constant (loaded when an instruction needs it in one). */
		key_node = property->first;
		computed = 0;
		if ((property->flags & JS_FLAG_COMPUTED) != 0U)
			computed = 1;
		constant = 0;
		if (computed) {
			js_compile_expression(fc, key_node, key);
			js_emit2(fc, VM_OP_TO_PROPERTY_KEY, key, key);
		} else {
			constant = expr_property_key(fc, key_node);
			js_emit2(fc, VM_OP_LOAD_CONST, key, constant);
		}

		/* The value; an anonymous function takes a plain key's name. */
		named = 0;
		if (!computed && (key_node->kind == JS_NODE_IDENTIFIER || key_node->kind == JS_NODE_STRING))
			named = 1;
		if (named) {
			js_compile_expression_named(fc, property->second, value, key_node->text, key_node->text_length);
		} else {
			js_compile_expression(fc, property->second, value);
		}

		/* An anonymous function under any other key, and an accessor's half ("get " or "set " before it), take the key's name when they run. */
		prefix = 0;
		if (property->op == JS_PROPERTY_GET)
			prefix = 1;
		else if (property->op == JS_PROPERTY_SET)
			prefix = 2;
		anonymous = expr_is_anonymous_function(property->second);
		if (prefix != 0U || (!named && anonymous))
			js_emit3(fc, VM_OP_SET_FUNCTION_NAME, value, key, prefix);

		/* An accessor's half. */
		if (property->op == JS_PROPERTY_GET) {
			js_emit3(fc, VM_OP_DEFINE_GETTER, target, key, value);
			continue;
		}

		/* A setter's half. */
		if (property->op == JS_PROPERTY_SET) {
			js_emit3(fc, VM_OP_DEFINE_SETTER, target, key, value);
			continue;
		}

		/* __proto__: value sets the prototype instead of defining a property. */
		proto = expr_is_proto(property);
		if (proto) {
			js_emit2(fc, VM_OP_SET_PROTO, target, value);
			continue;
		}

		/* A data property. */
		if (computed) {
			js_emit3(fc, VM_OP_DEFINE_ELEM, target, key, value);
		} else {
			js_emit3(fc, VM_OP_DEFINE_PROP, target, constant, value);
		}
	}

	/* The temporaries are free again. */
	fc->temp_top = mark;
}

/* Makes the constant key of a property's plain key: a name, a string or a number (its numeral). */
static uint32_t
expr_property_key(
	struct js_function_compiler *fc,
	const struct js_node *key)
{
	vm_value property_key;
	uint32_t constant;
	int status;

	/* A name or a string is its text. */
	if (key->kind == JS_NODE_IDENTIFIER || key->kind == JS_NODE_STRING) {
		constant = js_constant_key(fc, key->text, key->text_length);
		return constant;
	}

	/* A number is its string's key. */
	if (key->kind != JS_NODE_NUMBER)
		js_compile_unsupported(fc->compiler, key, "this kind of property key");
	status = vm_to_key(fc->compiler->realm, vm_value_number(key->number), &property_key);
	if (status != 0)
		js_compile_out_of_memory(fc->compiler);

	/* Reports its constant. */
	constant = js_constant(fc, property_key);
	return constant;
}

/*
 * Tells whether an expression is an anonymous function definition (a
 * function, an arrow or a class without its own name, in any parentheses),
 * which takes the name of what it is defined as.
 */
static int
expr_is_anonymous_function(
	const struct js_node *node)
{
	/* Parentheses keep the definition anonymous. */
	while (node->kind == JS_NODE_PARENTHESIZED)
		node = node->first;

	/* Only a function or a class, and only without a name. */
	if (node->kind != JS_NODE_FUNCTION && node->kind != JS_NODE_CLASS)
		return 0;
	if (node->text != NULL)
		return 0;

	/* It is. */
	return 1;
}

/* Tells whether a property of an object literal is __proto__: value (which sets the prototype). */
static int
expr_is_proto(
	const struct js_node *property)
{
	const struct js_node *key;
	int spelled;

	/* Only a plain key: value property. */
	if (property->op != JS_PROPERTY_INIT)
		return 0;
	if ((property->flags & (JS_FLAG_COMPUTED | JS_FLAG_SHORTHAND)) != 0U)
		return 0;

	/* The key spells __proto__ as a name or a string. */
	key = property->first;
	if (key->kind != JS_NODE_IDENTIFIER && key->kind != JS_NODE_STRING)
		return 0;
	spelled = js_text_is(key->text, key->text_length, "__proto__");
	if (!spelled)
		return 0;

	/* It is. */
	return 1;
}

/* Compiles a unary operator. */
static void
expr_unary(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	uint32_t opcode;

	/* typeof and delete look at their operand's form. */
	if (node->op == JS_P_TYPEOF) {
		expr_typeof(fc, node->first, target);
		return;
	}

	/* delete looks at what it deletes. */
	if (node->op == JS_P_DELETE) {
		expr_delete(fc, node->first, target);
		return;
	}

	/* The operand. */
	js_compile_expression(fc, node->first, target);

	/* void is undefined after it. */
	if (node->op == JS_P_VOID) {
		js_load_value(fc, target, VM_VALUE_UNDEFINED);
		return;
	}

	/* The operator on the value. */
	switch (node->op) {
	case JS_P_NOT:
		opcode = VM_OP_NOT;
		break;
	case JS_P_MINUS:
		opcode = VM_OP_NEG;
		break;
	case JS_P_PLUS:
		opcode = VM_OP_TO_NUMBER;
		break;
	case JS_P_TILDE:
		opcode = VM_OP_BIT_NOT;
		break;
	default:
		js_compile_unsupported(fc->compiler, node, "this unary operator");
	}

	/* The operator on the value, in place. */
	js_emit2(fc, opcode, target, target);
}

/* Compiles typeof: a name no scope declares is undefined rather than a ReferenceError. */
static void
expr_typeof(
	struct js_function_compiler *fc,
	struct js_node *operand,
	uint32_t target)
{
	struct js_binding *binding;
	struct js_node *place;
	uint32_t hops;
	uint32_t key;

	/* A global name is read without the error. */
	place = expr_unwrap(operand);
	binding = NULL;
	if (place->kind == JS_NODE_IDENTIFIER)
		binding = js_scope_resolve(fc, place->text, place->text_length, &hops);
	if (place->kind == JS_NODE_IDENTIFIER && fc->with != NULL) {
		expr_load_binding_with(fc, place->text, place->text_length, target, 1);
	} else if (place->kind == JS_NODE_IDENTIFIER && binding == NULL) {
		key = js_constant_key(fc, place->text, place->text_length);
		js_emit2(fc, VM_OP_GET_GLOBAL_TYPEOF, target, key);
	} else {
		js_compile_expression(fc, operand, target);
	}

	/* The type's name. */
	js_emit2(fc, VM_OP_TYPEOF, target, target);
}

/* Compiles delete: of a property, of a global name, or of anything else (true). */
static void
expr_delete(
	struct js_function_compiler *fc,
	struct js_node *operand,
	uint32_t target)
{
	struct js_binding *binding;
	struct js_node *place;
	uint32_t mark;
	uint32_t object;
	uint32_t key;
	uint32_t hops;
	uint32_t constant;

	/* A property. */
	place = expr_unwrap(operand);
	if (place->kind == JS_NODE_MEMBER) {
		if (place->second->kind == JS_NODE_PRIVATE_NAME)
			js_compile_fail(fc->compiler, place, "Private fields can not be deleted");
		if ((place->flags & JS_FLAG_OPTIONAL) != 0U)
			expr_unsupported(fc, place);
		mark = fc->temp_top;
		object = js_temp(fc);
		js_compile_expression(fc, place->first, object);
		if ((place->flags & JS_FLAG_COMPUTED) != 0U) {
			key = js_temp(fc);
			js_compile_expression(fc, place->second, key);
			js_emit3(fc, VM_OP_DELETE_ELEM, target, object, key);
		} else {
			constant = js_constant_key(fc, place->second->text, place->second->text_length);
			js_emit3(fc, VM_OP_DELETE_PROP, target, object, constant);
		}

		/* The temporaries are free again. */
		fc->temp_top = mark;
		return;
	}

	/* A name: a global is deleted from the global object; a declared one stays (false). */
	if (place->kind == JS_NODE_IDENTIFIER) {
		binding = js_scope_resolve(fc, place->text, place->text_length, &hops);
		if (binding != NULL) {
			js_load_value(fc, target, VM_VALUE_FALSE);
			return;
		}

		/* A global is deleted from the global object. */
		constant = js_constant_key(fc, place->text, place->text_length);
		js_emit2(fc, VM_OP_DELETE_GLOBAL, target, constant);
		return;
	}

	/* Anything else is evaluated and deleted trivially. */
	js_compile_expression(fc, operand, target);
	js_load_value(fc, target, VM_VALUE_TRUE);
}

/* Compiles ++ and --: the new number is written back; the value is the new one (prefix) or the old number (postfix). */
static void
expr_update(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	struct js_node *place;
	uint32_t mark;
	uint32_t object;
	uint32_t key;
	uint32_t updated;
	uint32_t opcode;
	int is_member;

	/* The operand's current value (a property's object and key are kept for the write). */
	place = expr_unwrap(node->first);
	mark = fc->temp_top;
	object = js_temp(fc);
	key = js_temp(fc);
	updated = js_temp(fc);
	is_member = 0;
	if (place->kind == JS_NODE_MEMBER) {
		is_member = 1;
		expr_member_parts(fc, place, object, key);
		expr_member_get(fc, place, object, key, target);
	} else if (place->kind == JS_NODE_IDENTIFIER) {
		js_load_binding(fc, place->text, place->text_length, target);
	} else {
		js_compile_expression(fc, place, target);
		expr_throw_text(fc, "ReferenceError: Invalid left-hand side expression in update operation");
		fc->temp_top = mark;
		return;
	}

	/* The old value as a number, and the new one. */
	opcode = VM_OP_INC;
	if (node->op == JS_P_DECREMENT)
		opcode = VM_OP_DEC;
	js_emit2(fc, VM_OP_TO_NUMERIC, target, target);
	js_emit2(fc, opcode, updated, target);

	/* The new value written back; the prefix form's value is the new one. */
	if (is_member) {
		expr_member_put(fc, place, object, key, updated);
	} else {
		js_store_binding(fc, place, place->text, place->text_length, updated);
	}

	/* The prefix form's value is the new one. */
	if ((node->flags & JS_FLAG_PREFIX) != 0U)
		js_emit2(fc, VM_OP_MOV, target, updated);
	fc->temp_top = mark;
}

/* Reports the opcode of a binary operator (and whether its result is negated: != and !==). */
static uint32_t
expr_binary_opcode(
	int op,
	int *negate)
{
	/* The operators by their punctuator (the compound assignments' too). */
	*negate = 0;
	switch (op) {
	case JS_P_PLUS:
	case JS_P_PLUS_ASSIGN:
		return VM_OP_ADD;
	case JS_P_MINUS:
	case JS_P_MINUS_ASSIGN:
		return VM_OP_SUB;
	case JS_P_STAR:
	case JS_P_STAR_ASSIGN:
		return VM_OP_MUL;
	case JS_P_SLASH:
	case JS_P_SLASH_ASSIGN:
		return VM_OP_DIV;
	case JS_P_PERCENT:
	case JS_P_PERCENT_ASSIGN:
		return VM_OP_MOD;
	case JS_P_POWER:
	case JS_P_POWER_ASSIGN:
		return VM_OP_EXP;
	case JS_P_SHL:
	case JS_P_SHL_ASSIGN:
		return VM_OP_SHL;
	case JS_P_SAR:
	case JS_P_SAR_ASSIGN:
		return VM_OP_SAR;
	case JS_P_SHR:
	case JS_P_SHR_ASSIGN:
		return VM_OP_SHR;
	case JS_P_AMP:
	case JS_P_AMP_ASSIGN:
		return VM_OP_BIT_AND;
	case JS_P_BAR:
	case JS_P_BAR_ASSIGN:
		return VM_OP_BIT_OR;
	case JS_P_CARET:
	case JS_P_CARET_ASSIGN:
		return VM_OP_BIT_XOR;
	case JS_P_LT:
		return VM_OP_LESS;
	case JS_P_GT:
		return VM_OP_GREATER;
	case JS_P_LE:
		return VM_OP_LESS_EQ;
	case JS_P_GE:
		return VM_OP_GREATER_EQ;
	case JS_P_EQ:
		return VM_OP_LOOSE_EQ;
	case JS_P_NE:
		*negate = 1;
		return VM_OP_LOOSE_EQ;
	case JS_P_STRICT_EQ:
		return VM_OP_STRICT_EQ;
	case JS_P_STRICT_NE:
		*negate = 1;
		return VM_OP_STRICT_EQ;
	case JS_P_INSTANCEOF:
		return VM_OP_INSTANCEOF;
	case JS_P_IN:
		return VM_OP_IN;
	default:
		break;
	}

	/* No such operator. */
	return VM_OPCODE_COUNT;
}

/* Compiles a binary operator: the left, then the right, then the operator. */
static void
expr_binary(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	uint32_t mark;
	uint32_t right;
	uint32_t opcode;
	int negate;

	/* #x in o: whether the object has the private member. */
	if (node->first->kind == JS_NODE_PRIVATE_NAME) {
		mark = fc->temp_top;
		right = js_temp(fc);
		expr_private_key(fc, node->first, target);
		js_compile_expression(fc, node->second, right);
		js_emit3(fc, VM_OP_PRIVATE_IN, target, target, right);
		fc->temp_top = mark;
		return;
	}

	/* The operator. */
	opcode = expr_binary_opcode(node->op, &negate);
	if (opcode == VM_OPCODE_COUNT)
		expr_unsupported(fc, node);

	/* The operands in order. */
	mark = fc->temp_top;
	js_compile_expression(fc, node->first, target);
	right = js_temp(fc);
	js_compile_expression(fc, node->second, right);

	/* The operator, negated for != and !==. */
	js_emit3(fc, opcode, target, target, right);
	if (negate)
		js_emit2(fc, VM_OP_NOT, target, target);
	fc->temp_top = mark;
}

/* Compiles &&, || and ??: the right side runs only when the left does not decide. */
static void
expr_logical(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	uint32_t end;

	/* The left side decides first. */
	js_compile_expression(fc, node->first, target);
	end = js_label_new(fc);
	if (node->op == JS_P_AND) {
		js_emit_jump(fc, VM_OP_JUMP_IF_FALSE, target, end);
	} else if (node->op == JS_P_OR) {
		js_emit_jump(fc, VM_OP_JUMP_IF_TRUE, target, end);
	} else {
		expr_nullish_jump(fc, target, end);
	}

	/* Otherwise the right side's value. */
	js_compile_expression(fc, node->second, target);
	js_label_place(fc, end);
}

/* Jumps to a label unless a value is undefined or null (what ?? keeps). */
static void
expr_nullish_jump(
	struct js_function_compiler *fc,
	uint32_t value,
	uint32_t label)
{
	uint32_t mark;
	uint32_t test;

	/* == null holds exactly for undefined and null. */
	mark = fc->temp_top;
	test = js_temp(fc);
	js_load_value(fc, test, VM_VALUE_NULL);
	js_emit3(fc, VM_OP_LOOSE_EQ, test, value, test);
	js_emit_jump(fc, VM_OP_JUMP_IF_FALSE, test, label);
	fc->temp_top = mark;
}

/* Compiles the conditional operator. */
static void
expr_conditional(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	uint32_t otherwise;
	uint32_t end;

	/* The test. */
	js_compile_expression(fc, node->first, target);
	otherwise = js_label_new(fc);
	end = js_label_new(fc);
	js_emit_jump(fc, VM_OP_JUMP_IF_FALSE, target, otherwise);

	/* The consequent, then around the alternate. */
	js_compile_expression(fc, node->second, target);
	js_emit_jump(fc, VM_OP_JUMP, 0, end);

	/* The alternate. */
	js_label_place(fc, otherwise);
	js_compile_expression(fc, node->third, target);
	js_label_place(fc, end);
}

/*
 * Compiles an assignment: plain (=), compound (+= and the others: read,
 * operate, write) or logical (&&=, ||=, ??=).  Its value is the value
 * written.
 */
static void
expr_assign(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	struct js_node *left;
	uint32_t mark;
	uint32_t object;
	uint32_t key;
	uint32_t right;
	uint32_t opcode;
	int negate;
	int is_member;

	/* The logical assignments short-circuit. */
	left = expr_unwrap(node->first);
	if (node->op == JS_P_AND_ASSIGN || node->op == JS_P_OR_ASSIGN || node->op == JS_P_NULLISH_ASSIGN) {
		expr_assign_logical(fc, node, left, target);
		return;
	}

	/* A pattern takes the parts of the value, which is the assignment's value. */
	if (left->kind == JS_NODE_ARRAY_PATTERN || left->kind == JS_NODE_OBJECT_PATTERN) {
		mark = fc->temp_top;
		right = js_temp(fc);
		js_compile_expression(fc, node->second, right);
		js_bind_pattern(fc, left, right, JS_BIND_ASSIGN);
		js_emit2(fc, VM_OP_MOV, target, right);
		fc->temp_top = mark;
		return;
	}

	/* A call cannot be assigned. */
	if (left->kind != JS_NODE_IDENTIFIER && left->kind != JS_NODE_MEMBER) {
		js_compile_expression(fc, node->second, target);
		js_store_target(fc, left, target);
		return;
	}

	/* A property's object and key first. */
	mark = fc->temp_top;
	object = js_temp(fc);
	key = js_temp(fc);
	right = js_temp(fc);
	is_member = 0;
	if (left->kind == JS_NODE_MEMBER) {
		is_member = 1;
		expr_member_parts(fc, left, object, key);
	}

	/* A plain assignment: the value (an anonymous function takes a name's name). */
	if (node->op == JS_P_ASSIGN) {
		if (is_member) {
			js_compile_expression(fc, node->second, target);
		} else {
			js_compile_expression_named(fc, node->second, target, left->text, left->text_length);
		}
	} else {
		/* A compound one: the current value, the right side, the operator. */
		opcode = expr_binary_opcode(node->op, &negate);
		if (opcode == VM_OPCODE_COUNT)
			expr_unsupported(fc, node);
		if (is_member) {
			expr_member_get(fc, left, object, key, target);
		} else {
			js_load_binding(fc, left->text, left->text_length, target);
		}

		/* The right side, then the operator. */
		js_compile_expression(fc, node->second, right);
		js_emit3(fc, opcode, target, target, right);
	}

	/* The write. */
	if (is_member) {
		expr_member_put(fc, left, object, key, target);
	} else {
		js_store_binding(fc, left, left->text, left->text_length, target);
	}

	/* The temporaries are free again. */
	fc->temp_top = mark;
}

/* Compiles &&=, ||= and ??=: the write happens only when the current value does not decide. */
static void
expr_assign_logical(
	struct js_function_compiler *fc,
	struct js_node *node,
	struct js_node *left,
	uint32_t target)
{
	uint32_t mark;
	uint32_t object;
	uint32_t key;
	uint32_t end;
	int is_member;

	/* Only a name or a property. */
	if (left->kind != JS_NODE_IDENTIFIER && left->kind != JS_NODE_MEMBER)
		expr_unsupported(fc, left);

	/* The current value. */
	mark = fc->temp_top;
	object = js_temp(fc);
	key = js_temp(fc);
	is_member = 0;
	if (left->kind == JS_NODE_MEMBER) {
		is_member = 1;
		expr_member_parts(fc, left, object, key);
		expr_member_get(fc, left, object, key, target);
	} else {
		js_load_binding(fc, left->text, left->text_length, target);
	}

	/* The current value decides first. */
	end = js_label_new(fc);
	if (node->op == JS_P_AND_ASSIGN) {
		js_emit_jump(fc, VM_OP_JUMP_IF_FALSE, target, end);
	} else if (node->op == JS_P_OR_ASSIGN) {
		js_emit_jump(fc, VM_OP_JUMP_IF_TRUE, target, end);
	} else {
		expr_nullish_jump(fc, target, end);
	}

	/* Otherwise the right side, written. */
	if (is_member) {
		js_compile_expression(fc, node->second, target);
		expr_member_put(fc, left, object, key, target);
	} else {
		js_compile_expression_named(fc, node->second, target, left->text, left->text_length);
		js_store_binding(fc, left, left->text, left->text_length, target);
	}

	/* The end, where the current value decided. */
	js_label_place(fc, end);
	fc->temp_top = mark;
}

/* Computes a property access's object and (when computed) its key into registers. */
static void
expr_member_parts(
	struct js_function_compiler *fc,
	struct js_node *member,
	uint32_t object,
	uint32_t key)
{
	/* super: the object is this, and the key is always in its register. */
	if (member->first->kind == JS_NODE_SUPER) {
		expr_this(fc, object);
		if ((member->flags & JS_FLAG_COMPUTED) != 0U) {
			js_compile_expression(fc, member->second, key);
			js_emit2(fc, VM_OP_TO_PROPERTY_KEY, key, key);
		} else {
			js_emit2(fc, VM_OP_LOAD_CONST, key, js_constant_key(fc, member->second->text, member->second->text_length));
		}

		/* super's key is ready. */
		return;
	}

	/* The object (?. leaves the chain when it is undefined or null), then a computed key or a private name's. */
	js_compile_expression(fc, member->first, object);
	expr_optional_check(fc, member, object);
	if ((member->flags & JS_FLAG_COMPUTED) != 0U)
		js_compile_expression(fc, member->second, key);
	if (member->second->kind == JS_NODE_PRIVATE_NAME)
		expr_private_key(fc, member->second, key);
}

/* Reads a property whose object and key are in registers. */
static void
expr_member_get(
	struct js_function_compiler *fc,
	struct js_node *member,
	uint32_t object,
	uint32_t key,
	uint32_t target)
{
	uint32_t constant;
	uint32_t mark;
	uint32_t home;

	/* super: the property of the home object's prototype, with this (the object) as the receiver. */
	if (member->first->kind == JS_NODE_SUPER) {
		mark = fc->temp_top;
		home = js_temp(fc);
		expr_home(fc, member, home);
		js_emit4(fc, VM_OP_GET_SUPER, target, home, key, object);
		fc->temp_top = mark;
		return;
	}

	/* A private member: only the object's own. */
	if (member->second->kind == JS_NODE_PRIVATE_NAME) {
		js_emit3(fc, VM_OP_PRIVATE_GET, target, object, key);
		return;
	}

	/* A computed key is in its register; a name is a constant. */
	if ((member->flags & JS_FLAG_COMPUTED) != 0U) {
		js_emit3(fc, VM_OP_GET_ELEM, target, object, key);
		return;
	}

	/* A name is a constant. */
	constant = js_constant_key(fc, member->second->text, member->second->text_length);
	js_emit3(fc, VM_OP_GET_PROP, target, object, constant);
}

/* Writes a property whose object and key are in registers. */
static void
expr_member_put(
	struct js_function_compiler *fc,
	struct js_node *member,
	uint32_t object,
	uint32_t key,
	uint32_t source)
{
	uint32_t constant;

	/* Assigning through super comes later. */
	if (member->first->kind == JS_NODE_SUPER)
		js_compile_unsupported(fc->compiler, member->first, "assignment to a super property");

	/* A private member: only the object's own. */
	if (member->second->kind == JS_NODE_PRIVATE_NAME) {
		js_emit3(fc, VM_OP_PRIVATE_SET, object, key, source);
		return;
	}

	/* A computed key is in its register; a name is a constant. */
	if ((member->flags & JS_FLAG_COMPUTED) != 0U) {
		js_emit3(fc, VM_OP_PUT_ELEM, object, key, source);
		return;
	}

	/* A name is a constant. */
	constant = js_constant_key(fc, member->second->text, member->second->text_length);
	js_emit3(fc, VM_OP_PUT_PROP, object, constant, source);
}

/* Compiles a property access. */
static void
expr_member(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	uint32_t mark;
	uint32_t key;

	/* The object into the target itself, a computed key above it, then the read. */
	mark = fc->temp_top;
	key = js_temp(fc);
	expr_member_parts(fc, node, target, key);
	expr_member_get(fc, node, target, key, target);
	fc->temp_top = mark;
}

/* Compiles a call: a property's call has the object as this; any other callee has undefined. */
static void
expr_call(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	struct js_node *callee;
	struct js_node *chained;
	uint32_t mark;
	uint32_t function;
	uint32_t this_value;
	uint32_t key;
	uint32_t first;
	uint32_t count;
	uint32_t arguments;
	uint32_t saved_chain;
	uint32_t end;
	int spread;

	/* super(...) constructs with the parent. */
	callee = expr_unwrap(node->first);
	if (callee->kind == JS_NODE_SUPER) {
		expr_super_call(fc, node, target);
		return;
	}

	/* The function and this: a property's object, or undefined. */
	mark = fc->temp_top;
	function = js_temp(fc);
	this_value = js_temp(fc);
	chained = NULL;
	if (callee->kind == JS_NODE_OPTIONAL_CHAIN)
		chained = expr_unwrap(callee->first);
	if (callee->kind == JS_NODE_MEMBER) {
		key = js_temp(fc);
		expr_member_parts(fc, callee, this_value, key);
		expr_member_get(fc, callee, this_value, key, function);
	} else if (chained != NULL && chained->kind == JS_NODE_MEMBER) {
		/* (a?.b)(): the chain's property keeps its object as this; a chain that stopped calls undefined. */
		key = js_temp(fc);
		saved_chain = fc->chain_label;
		fc->chain_label = js_label_new(fc);
		end = js_label_new(fc);
		expr_member_parts(fc, chained, this_value, key);
		expr_member_get(fc, chained, this_value, key, function);
		js_emit_jump(fc, VM_OP_JUMP, 0, end);
		js_label_place(fc, fc->chain_label);
		js_load_value(fc, function, VM_VALUE_UNDEFINED);
		js_load_value(fc, this_value, VM_VALUE_UNDEFINED);
		js_label_place(fc, end);
		fc->chain_label = saved_chain;
	} else {
		js_compile_expression(fc, node->first, function);
		js_load_value(fc, this_value, VM_VALUE_UNDEFINED);
	}

	/* ?.() leaves the chain when the function is undefined or null. */
	expr_optional_check(fc, node, function);

	/* With a spread, the arguments are gathered in an array. */
	spread = expr_has_spread(node->second);
	if (spread) {
		arguments = js_temp(fc);
		expr_spread_list(fc, node->second, arguments);
		js_emit4(fc, VM_OP_CALL_ARRAY, target, function, this_value, arguments);
		fc->temp_top = mark;
		return;
	}

	/* The arguments, then the call. */
	first = expr_arguments(fc, node->second, &count);
	js_emit5(fc, VM_OP_CALL, target, function, this_value, first, count);
	fc->temp_top = mark;
}

/* Compiles new: the constructor, the arguments, then the construction. */
static void
expr_new(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	uint32_t mark;
	uint32_t constructor;
	uint32_t arguments;
	uint32_t first;
	uint32_t count;
	int spread;

	/* The constructor (it is also new.target). */
	mark = fc->temp_top;
	constructor = js_temp(fc);
	js_compile_expression(fc, node->first, constructor);

	/* With a spread, the arguments are gathered in an array. */
	spread = expr_has_spread(node->second);
	if (spread) {
		arguments = js_temp(fc);
		expr_spread_list(fc, node->second, arguments);
		js_emit3(fc, VM_OP_CONSTRUCT_ARRAY, target, constructor, arguments);
		fc->temp_top = mark;
		return;
	}

	/* The arguments, then the construction. */
	first = expr_arguments(fc, node->second, &count);
	js_emit5(fc, VM_OP_CONSTRUCT, target, constructor, constructor, first, count);
	fc->temp_top = mark;
}

/* Computes a call's arguments into consecutive registers; reports the first and the count. */
static uint32_t
expr_arguments(
	struct js_function_compiler *fc,
	struct js_node *list,
	uint32_t *count)
{
	struct js_node *argument;
	uint32_t first;
	uint32_t index;

	/* One register each, taken together so they follow each other. */
	*count = 0;
	first = fc->temp_top;
	for (argument = list; argument != NULL; argument = argument->next) {
		js_temp(fc);
		(*count)++;
	}

	/* Each argument into its register, in order. */
	index = 0;
	for (argument = list; argument != NULL; argument = argument->next) {
		js_compile_expression(fc, argument, first + index);
		index++;
	}

	/* The first register (with no arguments, where they would start, which must still be one of the frame's). */
	if (fc->register_count <= first)
		fc->register_count = first + 1U;
	return first;
}

/* Throws an error text (the engine's errors are strings until the Error objects exist). */
static void
expr_throw_text(
	struct js_function_compiler *fc,
	const char *text)
{
	struct vm_string *string;
	uint32_t mark;
	uint32_t value;
	uint32_t constant;

	/* The text as a constant. */
	string = vm_string_from_utf8(fc->compiler->realm->heap, text, strlen(text));
	if (string == NULL)
		js_compile_out_of_memory(fc->compiler);
	constant = js_constant(fc, vm_value_cell(string));

	/* Loaded, then thrown. */
	mark = fc->temp_top;
	value = js_temp(fc);
	js_emit2(fc, VM_OP_LOAD_CONST, value, constant);
	js_emit1(fc, VM_OP_THROW, value);
	fc->temp_top = mark;
}

/* Finds the expression inside parentheses. */
static struct js_node *
expr_unwrap(
	struct js_node *node)
{
	/* Through each pair of parentheses. */
	while (node->kind == JS_NODE_PARENTHESIZED)
		node = node->first;

	/* The expression inside. */
	return node;
}

/* Lowers synchronous yield* using native iterator records and ordinary suspension/finally paths. */
static void
expr_delegate(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	uint32_t value;
	uint32_t state;
	uint32_t sent;
	uint32_t how;
	uint32_t outcome;
	uint32_t test;
	uint32_t loop;
	uint32_t complete;
	uint32_t finished;

	/* Evaluate the operand once and cache the actual iterator and its next method. */
	value = js_temp(fc);
	state = js_temp(fc);
	sent = js_temp(fc);
	how = js_temp(fc);
	outcome = js_temp(fc);
	test = js_temp(fc);
	js_compile_expression(fc, node->first, value);
	js_emit2(fc, VM_OP_DELEGATE_START, state, value);
	js_load_value(fc, sent, VM_VALUE_UNDEFINED);
	js_emit2(fc, VM_OP_LOAD_INT, how, VM_RESUME_NEXT);

	/* Each resumption forwards its completion to the iterator before choosing a suspension or exit. */
	loop = js_label_new(fc);
	complete = js_label_new(fc);
	finished = js_label_new(fc);
	js_label_place(fc, loop);
	js_emit5(fc, VM_OP_DELEGATE_STEP, target, state, sent, how, outcome);
	js_emit2(fc, VM_OP_LOAD_INT, test, -1);
	js_emit3(fc, VM_OP_STRICT_EQ, test, outcome, test);
	js_emit_jump(fc, VM_OP_JUMP_IF_FALSE, test, complete);
	js_emit3(fc, VM_OP_SUSPEND_RESULT, target, sent, how);
	js_emit_jump(fc, VM_OP_JUMP, 0, loop);

	/* A completed return crosses surrounding finally blocks; other completions become the expression value. */
	js_label_place(fc, complete);
	js_emit2(fc, VM_OP_LOAD_INT, test, VM_RESUME_RETURN);
	js_emit3(fc, VM_OP_STRICT_EQ, test, outcome, test);
	js_emit_jump(fc, VM_OP_JUMP_IF_FALSE, test, finished);
	js_emit_return(fc, target);
	js_label_place(fc, finished);
}

/*
 * Compiles yield in a generator (ws074-p086): the generator suspends with
 * the value; resumed, the expression's value is what next sent, a throw
 * throws what came, and a return returns it through the finally blocks
 * around.  Delegating yield uses the synchronous iterator protocol below.
 */
static void
expr_yield(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	uint32_t value;
	uint32_t how;
	uint32_t test;
	uint32_t goes_on;
	uint32_t returns;

	/* Delegation has its own completion loop and preserves iterator result identity. */
	if ((node->flags & JS_FLAG_DELEGATE) != 0U) {
		expr_delegate(fc, node, target);
		return;
	}

	/* The value yielded (undefined without one). */
	value = js_temp(fc);
	how = js_temp(fc);
	if (node->first != NULL) {
		js_compile_expression(fc, node->first, value);
	} else {
		js_load_value(fc, value, VM_VALUE_UNDEFINED);
	}

	/* The suspension; resumed, target has what came and how says how it came. */
	js_emit3(fc, VM_OP_SUSPEND, value, target, how);

	/* next goes on with the value (how is 0, which is false). */
	goes_on = js_label_new(fc);
	js_emit_jump(fc, VM_OP_JUMP_IF_FALSE, how, goes_on);

	/* return returns the value; throw throws it. */
	test = js_temp(fc);
	returns = js_label_new(fc);
	js_emit2(fc, VM_OP_LOAD_INT, test, VM_RESUME_RETURN);
	js_emit3(fc, VM_OP_STRICT_EQ, test, how, test);
	js_emit_jump(fc, VM_OP_JUMP_IF_TRUE, test, returns);
	js_emit1(fc, VM_OP_THROW, target);
	js_label_place(fc, returns);
	js_emit_return(fc, target);

	/* The generator goes on here with the value in target. */
	js_label_place(fc, goes_on);
}

/*
 * Compiles await in an async function (ws074-p086): the function suspends
 * waiting for the value; resumed, the expression's value is what the
 * promise was fulfilled with, or its reason is thrown here.
 */
static void
expr_await(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	uint32_t value;
	uint32_t how;
	uint32_t goes_on;

	/* The value awaited. */
	value = js_temp(fc);
	how = js_temp(fc);
	js_compile_expression(fc, node->first, value);

	/* The suspension; resumed, target has the value or the reason. */
	js_emit3(fc, VM_OP_SUSPEND, value, target, how);

	/* A fulfilled promise goes on with its value; a rejected one throws its reason. */
	goes_on = js_label_new(fc);
	js_emit_jump(fc, VM_OP_JUMP_IF_FALSE, how, goes_on);
	js_emit1(fc, VM_OP_THROW, target);

	/* The function goes on here with the value in target. */
	js_label_place(fc, goes_on);
}

/* Refuses an expression the compiler does not support yet, naming what it is. */
static void
expr_unsupported(
	struct js_function_compiler *fc,
	struct js_node *node)
{
	const char *what;

	/* The construct's name. */
	switch (node->kind) {
	case JS_NODE_TEMPLATE:
	case JS_NODE_TAGGED_TEMPLATE:
		what = "template literals";
		break;
	case JS_NODE_REGEXP:
		what = "regular expressions";
		break;
	case JS_NODE_BIGINT:
		what = "BigInt";
		break;
	case JS_NODE_CLASS:
		what = "classes";
		break;
	case JS_NODE_SUPER:
		what = "super";
		break;
	case JS_NODE_YIELD:
		what = "generators";
		break;
	case JS_NODE_AWAIT:
		what = "async functions";
		break;
	case JS_NODE_META_PROPERTY:
		what = "new.target and import.meta";
		break;
	case JS_NODE_IMPORT_CALL:
		what = "import()";
		break;
	case JS_NODE_SPREAD:
		what = "spread";
		break;
	case JS_NODE_OPTIONAL_CHAIN:
		what = "optional chaining";
		break;
	case JS_NODE_PRIVATE_NAME:
		what = "private names";
		break;
	case JS_NODE_ARRAY_PATTERN:
	case JS_NODE_OBJECT_PATTERN:
	case JS_NODE_ASSIGNMENT_PATTERN:
		what = "destructuring";
		break;
	default:
		what = js_node_kind_name(node->kind);
		break;
	}

	/* The failure. */
	js_compile_unsupported(fc->compiler, node, what);
}

/* Loads a BigInt literal: its digits (a 0x, 0o or 0b prefix, separators, the final n) become a BigInt constant. */
static void
expr_bigint(
	struct js_function_compiler *fc,
	struct js_node *node,
	uint32_t target)
{
	char digits[512];
	vm_value value;
	size_t index;
	size_t count;
	size_t length;
	int radix;
	int error;

	/* The text without the final n. */
	length = node->text_length;
	if (length > 0 && node->text[length - 1] == 'n')
		length--;
	index = 0;
	radix = 10;
	if (length > 2 && node->text[0] == '0') {
		if ((node->text[1] | 0x20U) == 'x')
			radix = 16;
		if ((node->text[1] | 0x20U) == 'o')
			radix = 8;
		if ((node->text[1] | 0x20U) == 'b')
			radix = 2;
		if (radix != 10)
			index = 2;
	}

	/* The digits without separators. */
	count = 0;
	for (; index < length && count + 1U < sizeof(digits); index++) {
		if (node->text[index] == '_')
			continue;
		digits[count++] = (char)node->text[index];
	}
	error = vm_bigint_from_digits(fc->compiler->realm->heap, digits, count, radix, 0, &value);
	if (error != 0)
		js_compile_out_of_memory(fc->compiler);
	js_load_value(fc, target, value);
}
