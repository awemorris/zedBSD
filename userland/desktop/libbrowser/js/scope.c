/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compiler's scope pass: for the program and each function, the
 * bindings its scopes declare and which of them a nested function refers
 * to (captured).
 *
 * A function's declarations are found first (vars and function
 * declarations anywhere in its body, hoisted, but not inside nested
 * functions), then every name its code uses is resolved outwards through
 * the scopes: the first scope that declares it wins; a name that reaches a
 * function's own scope as "arguments" without a declaration gets that
 * function's arguments object; a name nothing declares is a global.  The
 * program's vars and function declarations are globals too (properties of
 * the global object), listed for the code pass to declare.
 *
 * let and const (ws074-p078) are bindings of the scope of the block, the
 * for statement's head or the switch's cases they are written in, or of
 * the function's scope at the top level of its body; at the top level of
 * the program they are listed for the realm's shared record of them.  A
 * function declaration is hoisted to its function like a var (the ES5
 * practice); inside a block that has a scope it is made when the block is
 * entered, so that it sees the block's bindings, and its var then takes
 * it (Annex B).  class is left for the code pass to refuse.
 */

#include "js/compile.h"

#include <stdio.h>
#include <string.h>

/* The length of the name arguments. */
#define SCOPE_ARGUMENTS_LENGTH	9U

/* The name arguments, as the tree's texts are written (a constant for the life of the program). */
static const uint16_t scope_arguments_name[SCOPE_ARGUMENTS_LENGTH] = {
	'a', 'r', 'g', 'u', 'm', 'e', 'n', 't', 's'
};

static struct js_function_info *scope_function(struct js_compiler *compiler, struct js_scope *parent, struct js_node *node, int program);
static struct js_scope *scope_new(struct js_compiler *compiler, struct js_scope *parent, struct js_function_info *function, int kind);
static struct js_binding *scope_find(const struct js_scope *scope, const uint16_t *name, size_t length);
static struct js_binding *scope_declare(struct js_compiler *compiler, struct js_scope *scope, const uint16_t *name, size_t length, int kind);
static void scope_declare_var(struct js_compiler *compiler, struct js_function_info *info, const uint16_t *name, size_t length);
static void scope_declarations(struct js_compiler *compiler, struct js_function_info *info, struct js_node *list);
static void scope_declare_function(struct js_compiler *compiler, struct js_function_info *info, struct js_node *node);
static void scope_declare_target(struct js_compiler *compiler, struct js_function_info *info, struct js_node *target);
static void scope_visit_list(struct js_compiler *compiler, struct js_scope *scope, struct js_node *list);
static void scope_visit(struct js_compiler *compiler, struct js_scope *scope, struct js_node *node);
static void scope_visit_try(struct js_compiler *compiler, struct js_scope *scope, struct js_node *node);
static int scope_reference(struct js_compiler *compiler, struct js_scope *scope, const uint16_t *name, size_t length);
static int scope_has_own_arguments(const struct js_function_info *info);
static void scope_arrow_this(struct js_compiler *compiler, struct js_scope *scope);
static void scope_arrow_home(struct js_compiler *compiler, struct js_scope *scope);
static void scope_visit_class(struct js_compiler *compiler, struct js_scope *scope, struct js_node *node);
static struct js_node *scope_default_constructor(struct js_compiler *compiler, struct js_node *node);
static struct js_node *scope_new_node(struct js_compiler *compiler, int kind, const struct js_node *place);
static const uint16_t *scope_field_key_name(struct js_compiler *compiler, struct js_node *member, uint32_t ordinal);
static void scope_check_private_pair(struct js_compiler *compiler, const struct js_node *class_node, const struct js_node *member);
static struct js_scope *scope_lexical_block(struct js_compiler *compiler, struct js_scope *parent, struct js_node *list, int switch_cases);
static int scope_declare_lexicals(struct js_compiler *compiler, struct js_scope *scope, struct js_node *list);
static void scope_declare_lexical(struct js_compiler *compiler, struct js_scope *scope, struct js_node *target, int kind);
static void scope_global_lexical(struct js_compiler *compiler, struct js_function_info *info, struct js_node *target, int is_const);
static void scope_visit_block(struct js_compiler *compiler, struct js_scope *scope, struct js_node *node);
static void scope_visit_for(struct js_compiler *compiler, struct js_scope *scope, struct js_node *node);
static void scope_visit_switch(struct js_compiler *compiler, struct js_scope *scope, struct js_node *node);
static int scope_is_lexical(const struct js_node *node);
static void scope_visit_parameters(struct js_compiler *compiler, struct js_function_info *info);
static void scope_redeclared(struct js_compiler *compiler, const struct js_node *target);
static void scope_check_vars(struct js_compiler *compiler, const struct js_scope *block, struct js_node *list, int direct);
static void scope_check_var_target(struct js_compiler *compiler, const struct js_scope *block, struct js_node *target);
static void scope_block_function(struct js_compiler *compiler, struct js_scope *block, struct js_node *node);

/*
 * The hidden binding of a function's this that its arrow functions read:
 * "#this", which no identifier can be.  A constant for the life of the
 * program.
 */
const uint16_t js_this_name[JS_THIS_NAME_LENGTH] = {
	'#', 't', 'h', 'i', 's'
};

/*
 * The hidden binding of a method's home object that its arrow functions
 * read for super: "#home".  A constant for the life of the program.
 */
const uint16_t js_home_name[JS_HOME_NAME_LENGTH] = {
	'#', 'h', 'o', 'm', 'e'
};

/* The name of a derived class's default constructor's rest parameter (a constant for the life of the program). */
static const uint16_t scope_args_name[4] = { 'a', 'r', 'g', 's' };
static void scope_arguments_var(struct js_function_info *info);

/*
 * Runs the scope pass over a program: every function node, the program
 * and every catch clause gets its scope in node->scope; reports the
 * program's information.
 */
struct js_function_info *
js_scope_analyze(
	struct js_compiler *compiler,
	struct js_node *program)
{
	struct js_function_info *info;

	/* The program is the outermost function. */
	info = scope_function(compiler, NULL, program, 1);

	/* Succeeded: the program's information. */
	return info;
}

/*
 * Resolves a name from the scope the code pass is in: the binding that
 * declares it (NULL for a global) and, for a captured one, how many
 * environments outwards from the running function's its slot is.
 */
struct js_binding *
js_scope_resolve(
	const struct js_function_compiler *fc,
	const uint16_t *name,
	size_t length,
	uint32_t *hops)
{
	const struct js_scope *scope;
	const struct js_function_info *function;
	struct js_binding *binding;

	/* Outwards through the scopes, counting the environments passed. */
	*hops = 0;
	function = fc->info;
	for (scope = fc->scope; scope != NULL; scope = scope->parent) {
		/* Leaving a function for the one around it passes its environment, when it has one. */
		if (scope->function != function) {
			if (function->has_env)
				(*hops)++;
			function = scope->function;
		}

		/* The first scope that declares the name. */
		binding = scope_find(scope, name, length);
		if (binding != NULL)
			return binding;

		/* Leaving a block with an environment of its own passes that environment. */
		if (scope->kind == JS_SCOPE_BLOCK && scope->has_env)
			(*hops)++;
	}

	/* No scope declares it: a global. */
	return NULL;
}

/*
 * Reports the name of the hidden binding that holds a private name's key:
 * "#" and the name (the node's text is without it), made once and kept in
 * the node's raw text.
 */
const uint16_t *
js_private_name(
	struct js_compiler *compiler,
	struct js_node *node,
	size_t *length)
{
	uint16_t *text;

	/* Made already. */
	if (node->raw != NULL) {
		*length = node->raw_length;
		return node->raw;
	}

	/* "#" and the name, in the arena. */
	text = wb_arena_alloc(compiler->arena, (node->text_length + 1U) * sizeof(uint16_t));
	if (text == NULL)
		js_compile_out_of_memory(compiler);
	text[0] = '#';
	memcpy(text + 1, node->text, node->text_length * sizeof(uint16_t));
	node->raw = text;
	node->raw_length = node->text_length + 1U;

	/* Succeeded: the binding's name. */
	*length = node->raw_length;
	return text;
}

/* Makes the information of a function (or the program): its scopes, its declarations, and the resolution of its names. */
static struct js_function_info *
scope_function(
	struct js_compiler *compiler,
	struct js_scope *parent,
	struct js_node *node,
	int program)
{
	struct js_function_info *info;
	struct js_scope *outer;
	struct js_scope *callee;
	struct js_node *parameter;
	struct js_binding *binding;
	uint32_t index;
	int expression;
	int counting;

	/* The information. */
	info = wb_arena_zalloc(compiler->arena, sizeof(*info));
	if (info == NULL)
		js_compile_out_of_memory(compiler);
	info->node = node;
	info->program = program;
	if (parent != NULL)
		info->parent = parent->function;
	if ((node->flags & JS_FLAG_STRICT) != 0U)
		info->strict = 1;

	/* A named function expression sees its own name in a scope between it and the code around it. */
	outer = parent;
	expression = 0;
	if (node->kind == JS_NODE_FUNCTION && (node->flags & JS_FLAG_METHOD) == 0U && node->text != NULL)
		expression = 1;
	if (expression) {
		callee = scope_new(compiler, parent, info, JS_SCOPE_CALLEE);
		scope_declare(compiler, callee, node->text, node->text_length, JS_BINDING_CALLEE);
		outer = callee;
	}

	/* The function's own scope, noted on its node. */
	info->scope = scope_new(compiler, outer, info, JS_SCOPE_FUNCTION);
	node->scope = info->scope;

	/* A declaration in a block that has a scope is made when the block is entered. */
	if (node->kind == JS_NODE_FUNCTION_DECLARATION && parent != NULL && parent->kind == JS_SCOPE_BLOCK) {
		node->flags |= JS_FLAG_BLOCK_FUNCTION;
		scope_block_function(compiler, parent, node);
	}

	/*
	 * The parameters, in order (a repeated name is the last one's).  A
	 * plain name is bound to its argument; the names of a default, a
	 * pattern or a rest are vars the prologue binds (ws074-p079).  The
	 * length counts the parameters before the first default or rest.
	 */
	index = 0;
	info->simple_parameters = 1;
	info->length = 0;
	counting = 1;
	parameter = NULL;
	if (!program)
		parameter = node->first;
	for (;
	     parameter != NULL;
	     parameter = parameter->next) {
		/* A rest takes the arguments left; it is not one of the arguments' registers. */
		if (parameter->kind == JS_NODE_REST) {
			info->simple_parameters = 0;
			info->rest = parameter;
			counting = 0;
			scope_declare_target(compiler, info, parameter->first);
			continue;
		}

		/* The length stops at the first default. */
		if (parameter->kind == JS_NODE_ASSIGNMENT_PATTERN)
			counting = 0;
		if (counting)
			info->length++;

		/* A default or a pattern: its names are vars. */
		if (parameter->kind != JS_NODE_IDENTIFIER) {
			info->simple_parameters = 0;
			scope_declare_target(compiler, info, parameter);
			index++;
			continue;
		}

		/* The name, a binding of the function's scope. */
		binding = scope_declare(compiler, info->scope, parameter->text, parameter->text_length, JS_BINDING_PARAMETER);
		binding->parameter = index;
		index++;
	}

	/* The declarations of the body (the program's let and const are the realm's), then the names it uses. */
	if (program) {
		scope_declarations(compiler, info, node->first);
		scope_declare_lexicals(compiler, info->scope, node->first);
		scope_visit_list(compiler, info->scope, node->first);
	} else if ((node->flags & JS_FLAG_EXPRESSION_BODY) != 0U) {
		scope_arguments_var(info);
		scope_visit_parameters(compiler, info);
		scope_visit(compiler, info->scope, node->second);
	} else {
		scope_declarations(compiler, info, node->second);
		scope_declare_lexicals(compiler, info->scope, node->second);
		scope_arguments_var(info);
		scope_visit_parameters(compiler, info);
		scope_visit_list(compiler, info->scope, node->second);
	}

	/* Succeeded: the function's information. */
	return info;
}

/* Makes a scope of a function inside a parent scope. */
static struct js_scope *
scope_new(
	struct js_compiler *compiler,
	struct js_scope *parent,
	struct js_function_info *function,
	int kind)
{
	struct js_scope *scope;

	/* The scope, in the arena. */
	scope = wb_arena_zalloc(compiler->arena, sizeof(*scope));
	if (scope == NULL)
		js_compile_out_of_memory(compiler);
	scope->parent = parent;
	scope->function = function;
	scope->kind = kind;

	/* The function's list of its scopes, for the code pass to give each binding its place. */
	scope->next_in_function = function->scopes;
	function->scopes = scope;

	/* Succeeded: the scope. */
	return scope;
}

/* Finds the binding a scope declares for a name. */
static struct js_binding *
scope_find(
	const struct js_scope *scope,
	const uint16_t *name,
	size_t length)
{
	struct js_binding *binding;
	int same;

	/* Each binding of the scope. */
	for (binding = scope->bindings; binding != NULL; binding = binding->next) {
		same = js_text_equal(binding->name, binding->length, name, length);
		if (same)
			return binding;
	}

	/* The scope does not declare it. */
	return NULL;
}

/* Declares a name in a scope (a name declared already keeps its binding). */
static struct js_binding *
scope_declare(
	struct js_compiler *compiler,
	struct js_scope *scope,
	const uint16_t *name,
	size_t length,
	int kind)
{
	struct js_binding *binding;

	/* A second declaration of the name is the same binding. */
	binding = scope_find(scope, name, length);
	if (binding != NULL)
		return binding;

	/* A new binding at the front of the scope's list. */
	binding = wb_arena_zalloc(compiler->arena, sizeof(*binding));
	if (binding == NULL)
		js_compile_out_of_memory(compiler);
	binding->name = name;
	binding->length = length;
	binding->kind = kind;
	binding->next = scope->bindings;
	scope->bindings = binding;

	/* Succeeded: the binding. */
	return binding;
}

/* Declares a var: a binding of a function's scope, or a global of the program. */
static void
scope_declare_var(
	struct js_compiler *compiler,
	struct js_function_info *info,
	const uint16_t *name,
	size_t length)
{
	struct js_global_name *global;
	struct js_global_name *listed;
	int same;

	/* A function's or module's var is a binding, not a global property. */
	if (!info->program || (info->node->flags & JS_FLAG_MODULE) != 0U) {
		scope_declare(compiler, info->scope, name, length, JS_BINDING_VAR);
		return;
	}

	/* The program lists each name once. */
	for (listed = info->global_vars; listed != NULL; listed = listed->next) {
		same = js_text_equal(listed->name, listed->length, name, length);
		if (same)
			return;
	}

	/* A new global name. */
	global = wb_arena_zalloc(compiler->arena, sizeof(*global));
	if (global == NULL)
		js_compile_out_of_memory(compiler);
	global->name = name;
	global->length = length;

	/* It joins the end of the list, so the globals are made in source order. */
	if (info->global_vars_last == NULL) {
		info->global_vars = global;
	} else {
		info->global_vars_last->next = global;
	}

	/* The name is the last now. */
	info->global_vars_last = global;
}

/* Finds the declarations of a list of statements, down through blocks but not into functions. */
static void
scope_declarations(
	struct js_compiler *compiler,
	struct js_function_info *info,
	struct js_node *list)
{
	struct js_node *node;
	struct js_node *declarator;

	/* Each statement by its kind. */
	for (node = list; node != NULL; node = node->next) {
		switch (node->kind) {
		case JS_NODE_IMPORT:
			/* Each local import name is an immutable binding of the module. */
			for (declarator = node->first; declarator != NULL; declarator = declarator->next) {
				if (declarator->second == NULL || declarator->second->kind != JS_NODE_IDENTIFIER)
					js_compile_unsupported(compiler, declarator, "this kind of module import");
				if (scope_find(info->scope, declarator->second->text, declarator->second->text_length) != NULL)
					scope_redeclared(compiler, declarator->second);
				scope_declare(compiler, info->scope, declarator->second->text,
				    declarator->second->text_length, JS_BINDING_IMPORT);
			}
			break;
		case JS_NODE_EXPORT:
			/* A declaration with export still declares its local names. */
			if (js_export_is_declaration(node))
				scope_declarations(compiler, info, node->first);
			break;
		case JS_NODE_VARIABLES:
			/* Only var hoists (let and const are refused by the code pass). */
			if (node->op != JS_P_VAR)
				break;
			for (declarator = node->first; declarator != NULL; declarator = declarator->next)
				scope_declare_target(compiler, info, declarator->first);
			break;
		case JS_NODE_FUNCTION_DECLARATION:
			scope_declare_function(compiler, info, node);
			break;
		case JS_NODE_BLOCK:
			scope_declarations(compiler, info, node->first);
			break;
		case JS_NODE_IF:
			scope_declarations(compiler, info, node->second);
			scope_declarations(compiler, info, node->third);
			break;
		case JS_NODE_FOR:
		case JS_NODE_FOR_IN:
		case JS_NODE_FOR_OF:
			/* The head's declaration, then the body. */
			scope_declarations(compiler, info, node->first);
			scope_declarations(compiler, info, node->fourth);
			break;
		case JS_NODE_WHILE:
		case JS_NODE_DO_WHILE:
		case JS_NODE_WITH:
		case JS_NODE_LABELED:
			scope_declarations(compiler, info, node->fourth);
			break;
		case JS_NODE_TRY:
			scope_declarations(compiler, info, node->first);
			scope_declarations(compiler, info, node->third);
			scope_declarations(compiler, info, node->fourth);
			break;
		case JS_NODE_SWITCH:
			/* Each case's statements. */
			for (declarator = node->second; declarator != NULL; declarator = declarator->next)
				scope_declarations(compiler, info, declarator->second);
			break;
		default:
			break;
		}
	}
}

/* Declares a function declaration: its name as a binding (or global) and its node to hoist. */
static void
scope_declare_function(
	struct js_compiler *compiler,
	struct js_function_info *info,
	struct js_node *node)
{
	struct js_hoisted *hoisted;

	/* The name, like a var (a function's binding records that it is a function). */
	if (info->program && (info->node->flags & JS_FLAG_MODULE) == 0U) {
		scope_declare_var(compiler, info, node->text, node->text_length);
	} else {
		scope_declare(compiler, info->scope, node->text, node->text_length, JS_BINDING_FUNCTION);
	}

	/* The node joins the list of declarations to hoist, in source order. */
	hoisted = wb_arena_zalloc(compiler->arena, sizeof(*hoisted));
	if (hoisted == NULL)
		js_compile_out_of_memory(compiler);
	hoisted->node = node;
	if (info->hoisted_last == NULL) {
		info->hoisted = hoisted;
	} else {
		info->hoisted_last->next = hoisted;
	}

	/* The node is the last now. */
	info->hoisted_last = hoisted;
}

/* Declares the names of a var's target: an identifier, or the identifiers inside a pattern. */
static void
scope_declare_target(
	struct js_compiler *compiler,
	struct js_function_info *info,
	struct js_node *target)
{
	struct js_node *element;

	/* Nothing to declare. */
	if (target == NULL)
		return;

	/* A name. */
	if (target->kind == JS_NODE_IDENTIFIER) {
		scope_declare_var(compiler, info, target->text, target->text_length);
		return;
	}

	/* A pattern's elements, a default's or a rest's target. */
	for (element = target->first; element != NULL; element = element->next) {
		if (element->kind == JS_NODE_PROPERTY) {
			scope_declare_target(compiler, info, element->second);
		} else {
			scope_declare_target(compiler, info, element);
		}
	}

	/* A default's target, a rest's target. */
	if (target->kind == JS_NODE_ASSIGNMENT_PATTERN)
		scope_declare_target(compiler, info, target->first);
}

/* Resolves the names of each node of a list. */
static void
scope_visit_list(
	struct js_compiler *compiler,
	struct js_scope *scope,
	struct js_node *list)
{
	struct js_node *node;

	/* Each member in order. */
	for (node = list; node != NULL; node = node->next)
		scope_visit(compiler, scope, node);
}

/* Resolves the names a node uses, and makes the scopes of the functions and catch clauses under it. */
static void
scope_visit(
	struct js_compiler *compiler,
	struct js_scope *scope,
	struct js_node *node)
{
	const uint16_t *private_text;
	size_t private_length;
	int found;

	/* The kinds whose children are not all names in use. */
	switch (node->kind) {
	case JS_NODE_IDENTIFIER:
		scope_reference(compiler, scope, node->text, node->text_length);
		return;
	case JS_NODE_FUNCTION:
	case JS_NODE_FUNCTION_DECLARATION:
		scope_function(compiler, scope, node, 0);
		return;
	case JS_NODE_TRY:
		scope_visit_try(compiler, scope, node);
		return;
	case JS_NODE_THIS:
		scope_arrow_this(compiler, scope);
		return;
	case JS_NODE_BLOCK:
		scope_visit_block(compiler, scope, node);
		return;
	case JS_NODE_CLASS:
	case JS_NODE_CLASS_DECLARATION:
		scope_visit_class(compiler, scope, node);
		return;
	case JS_NODE_SUPER:
		scope_arrow_home(compiler, scope);
		return;
	case JS_NODE_FOR:
	case JS_NODE_FOR_IN:
	case JS_NODE_FOR_OF:
		scope_visit_for(compiler, scope, node);
		return;
	case JS_NODE_SWITCH:
		scope_visit_switch(compiler, scope, node);
		return;
	case JS_NODE_MEMBER:
		/* The object; the property only when computed or private. */
		scope_visit(compiler, scope, node->first);
		if ((node->flags & JS_FLAG_COMPUTED) != 0U || node->second->kind == JS_NODE_PRIVATE_NAME)
			scope_visit(compiler, scope, node->second);
		return;
	case JS_NODE_PROPERTY:
	case JS_NODE_METHOD:
	case JS_NODE_FIELD:
		/* The key only when computed; the value. */
		if (node->first != NULL && (node->flags & JS_FLAG_COMPUTED) != 0U)
			scope_visit(compiler, scope, node->first);
		if (node->second != NULL)
			scope_visit(compiler, scope, node->second);
		return;
	case JS_NODE_PRIVATE_NAME:
		/* #x (in #x in o, or a member's key): its class's hidden binding, which an enclosing class must declare. */
		private_text = js_private_name(compiler, node, &private_length);
		found = scope_reference(compiler, scope, private_text, private_length);
		if (!found)
			js_compile_fail(compiler, node, "Private field must be declared in an enclosing class");
		return;
	case JS_NODE_META_PROPERTY:
	case JS_NODE_IMPORT_SPECIFIER:
	case JS_NODE_EXPORT_SPECIFIER:
		return;
	default:
		break;
	}

	/* Every other node: its children in order. */
	scope_visit_list(compiler, scope, node->first);
	scope_visit_list(compiler, scope, node->second);
	scope_visit_list(compiler, scope, node->third);
	scope_visit_list(compiler, scope, node->fourth);
}

/* Resolves the names of a try statement, whose catch clause opens a scope with its parameter. */
static void
scope_visit_try(
	struct js_compiler *compiler,
	struct js_scope *scope,
	struct js_node *node)
{
	struct js_scope *clause;

	/* The protected block. */
	scope_visit_list(compiler, scope, node->first);

	/* The catch clause's scope and its parameter (a pattern is left for the code pass to refuse). */
	if (node->third != NULL) {
		clause = scope_new(compiler, scope, scope->function, JS_SCOPE_CATCH);
		node->scope = clause;
		if (node->second != NULL && node->second->kind == JS_NODE_IDENTIFIER)
			scope_declare(compiler, clause, node->second->text, node->second->text_length, JS_BINDING_CATCH);
		if (node->second != NULL && node->second->kind != JS_NODE_IDENTIFIER) {
			scope_declare_lexical(compiler, clause, node->second, JS_BINDING_CATCH);
			scope_visit(compiler, clause, node->second);
		}

		/* The clause's block, in the clause's scope. */
		scope_visit_list(compiler, clause, node->third);
	}

	/* The finally block. */
	scope_visit_list(compiler, scope, node->fourth);
}

/* Resolves a name used in a scope, marking a binding of an outer function captured; reports whether a scope declares it. */
static int
scope_reference(
	struct js_compiler *compiler,
	struct js_scope *scope,
	const uint16_t *name,
	size_t length)
{
	struct js_scope *search;
	struct js_binding *binding;
	int is_arguments;
	int own_arguments;

	/* Outwards through the scopes. */
	is_arguments = js_text_is(name, length, "arguments");
	for (search = scope; search != NULL; search = search->parent) {
		binding = scope_find(search, name, length);

		/* A function's own scope gives arguments to a function that has an arguments object. */
		if (binding == NULL && is_arguments && search->kind == JS_SCOPE_FUNCTION) {
			own_arguments = scope_has_own_arguments(search->function);
			if (own_arguments) {
				binding = scope_declare(compiler, search, name, length, JS_BINDING_ARGUMENTS);
				search->function->arguments = binding;
			}
		}

		/* The scope that declares it; a function's binding used by another is captured (in the block's environment for a block's). */
		if (binding != NULL) {
			if (search->function != scope->function) {
				binding->captured = 1;
				if (search->kind == JS_SCOPE_BLOCK) {
					search->has_env = 1;
				} else {
					search->function->has_env = 1;
				}
			}

			/* Resolved. */
			return 1;
		}
	}

	/* No scope declares it: a global. */
	return 0;
}

/*
 * Declares the let and const written directly in a list of statements in
 * a scope: the program's go to the realm's record instead.  Reports
 * whether there was any.
 */
static int
scope_declare_lexicals(
	struct js_compiler *compiler,
	struct js_scope *scope,
	struct js_node *list)
{
	struct js_node *node;
	struct js_node *declarator;
	int is_const;
	int kind;
	int found;
	int lexical;

	/* Each statement that is a let, const or class declaration. */
	found = 0;
	for (node = list; node != NULL; node = node->next) {
		/* A named export declaration makes the same lexical binding as the declaration alone. */
		if (node->kind == JS_NODE_EXPORT && js_export_is_declaration(node)) {
			if (scope_declare_lexicals(compiler, scope, node->first))
				found = 1;
			continue;
		}

		/* A class declaration's name is a let. */
		if (node->kind == JS_NODE_CLASS_DECLARATION && node->text != NULL) {
			found = 1;
			if (scope->kind == JS_SCOPE_FUNCTION && scope->function->program &&
			    (scope->function->node->flags & JS_FLAG_MODULE) == 0U) {
				scope_global_lexical(compiler, scope->function, node, 0);
			} else {
				scope_declare_lexical(compiler, scope, node, JS_BINDING_LET);
			}

			/* The next statement. */
			continue;
		}

		/* A let or const declaration. */
		lexical = scope_is_lexical(node);
		if (!lexical)
			continue;
		found = 1;

		/* The kind of its names. */
		is_const = 0;
		kind = JS_BINDING_LET;
		if (node->op == JS_P_CONST) {
			is_const = 1;
			kind = JS_BINDING_CONST;
		}

		/* Each declarator's names: the realm's for the program's top level, the scope's otherwise. */
		for (declarator = node->first; declarator != NULL; declarator = declarator->next) {
			if (scope->kind == JS_SCOPE_FUNCTION && scope->function->program &&
			    (scope->function->node->flags & JS_FLAG_MODULE) == 0U) {
				scope_global_lexical(compiler, scope->function, declarator->first, is_const);
			} else {
				scope_declare_lexical(compiler, scope, declarator->first, kind);
			}
		}
	}

	/* Reports whether the list declared any. */
	return found;
}

/* Declares the names of a let or const target (a name, or the names inside a pattern) in a scope. */
static void
scope_declare_lexical(
	struct js_compiler *compiler,
	struct js_scope *scope,
	struct js_node *target,
	int kind)
{
	struct js_binding *declared;
	struct js_node *element;

	/* Nothing to declare. */
	if (target == NULL)
		return;

	/* A name (or a class declaration's), which the scope may not declare already (a parameter, a var, a function or another let or const). */
	if (target->kind == JS_NODE_IDENTIFIER || target->kind == JS_NODE_CLASS_DECLARATION) {
		declared = scope_find(scope, target->text, target->text_length);
		if (declared != NULL)
			scope_redeclared(compiler, target);
		scope_declare(compiler, scope, target->text, target->text_length, kind);
		return;
	}

	/* A pattern's elements, a default's or a rest's target. */
	for (element = target->first; element != NULL; element = element->next) {
		if (element->kind == JS_NODE_PROPERTY) {
			scope_declare_lexical(compiler, scope, element->second, kind);
		} else {
			scope_declare_lexical(compiler, scope, element, kind);
		}
	}
}

/* Lists the let or const names of the program's top level (a name, or the names inside a pattern) for the realm's record. */
static void
scope_global_lexical(
	struct js_compiler *compiler,
	struct js_function_info *info,
	struct js_node *target,
	int is_const)
{
	struct js_global_lexical *lexical;
	struct js_global_lexical *listed;
	struct js_global_name *global;
	struct js_node *element;
	int same;

	/* Nothing to list. */
	if (target == NULL)
		return;

	/* A pattern's elements, a default's or a rest's target (a class declaration is listed by its name). */
	if (target->kind != JS_NODE_IDENTIFIER && target->kind != JS_NODE_CLASS_DECLARATION) {
		for (element = target->first; element != NULL; element = element->next) {
			if (element->kind == JS_NODE_PROPERTY) {
				scope_global_lexical(compiler, info, element->second, is_const);
			} else {
				scope_global_lexical(compiler, info, element, is_const);
			}
		}

		/* Nothing more for a pattern. */
		return;
	}

	/* The script may not declare the name twice, nor as a var or a function too. */
	for (listed = info->global_lexicals; listed != NULL; listed = listed->next) {
		same = js_text_equal(listed->name, listed->length, target->text, target->text_length);
		if (same)
			scope_redeclared(compiler, target);
	}

	/* The script's vars and functions. */
	for (global = info->global_vars; global != NULL; global = global->next) {
		same = js_text_equal(global->name, global->length, target->text, target->text_length);
		if (same)
			scope_redeclared(compiler, target);
	}

	/* The entry, at the front of the list (the order does not matter). */
	lexical = wb_arena_zalloc(compiler->arena, sizeof(*lexical));
	if (lexical == NULL)
		js_compile_out_of_memory(compiler);
	lexical->name = target->text;
	lexical->length = target->text_length;
	lexical->is_const = is_const;
	lexical->next = info->global_lexicals;
	info->global_lexicals = lexical;
}

/*
 * Makes the scope of a block, a for statement's head or a switch's cases
 * when it declares a let or const (the parent scope otherwise); for a
 * switch the list is the cases, whose statements are searched.
 */
static struct js_scope *
scope_lexical_block(
	struct js_compiler *compiler,
	struct js_scope *parent,
	struct js_node *list,
	int switch_cases)
{
	struct js_scope *block;
	struct js_node *clause;
	int found;
	int any;

	/* A block scope, kept only when something is declared in it. */
	block = scope_new(compiler, parent, parent->function, JS_SCOPE_BLOCK);
	any = 0;
	if (switch_cases) {
		for (clause = list; clause != NULL; clause = clause->next) {
			found = scope_declare_lexicals(compiler, block, clause->second);
			if (found)
				any = 1;
		}
	} else {
		any = scope_declare_lexicals(compiler, block, list);
	}

	/* A scope that declares nothing is not needed (it stays on the function's list, empty). */
	if (!any)
		return parent;

	/* A var inside the block, or a function declared in it, may not have the name of one of its let or const. */
	if (switch_cases) {
		for (clause = list; clause != NULL; clause = clause->next)
			scope_check_vars(compiler, block, clause->second, 1);
	} else {
		scope_check_vars(compiler, block, list, 1);
	}

	/* Succeeded: the block's scope. */
	return block;
}

/* Resolves the names of a block, in a scope of its own when it declares a let or const. */
static void
scope_visit_block(
	struct js_compiler *compiler,
	struct js_scope *scope,
	struct js_node *node)
{
	struct js_scope *block;

	/* The block's scope, noted on the node when it has one. */
	block = scope_lexical_block(compiler, scope, node->first, 0);
	node->scope = NULL;
	if (block != scope)
		node->scope = block;

	/* The statements in it. */
	scope_visit_list(compiler, block, node->first);
}

/*
 * Resolves the names of a for, for-in or for-of statement: a let or const
 * in its head has a scope of its own around the head and the body (the
 * object of for-in and for-of is evaluated outside it).
 */
static void
scope_visit_for(
	struct js_compiler *compiler,
	struct js_scope *scope,
	struct js_node *node)
{
	struct js_scope *head;
	int lexical;

	/* The head's scope, noted on the node when it has one. */
	node->scope = NULL;
	head = scope;
	lexical = 0;
	if (node->first != NULL)
		lexical = scope_is_lexical(node->first);
	if (lexical) {
		head = scope_lexical_block(compiler, scope, node->first, 0);
		node->scope = head;
	}

	/* A plain for statement: every part in the head's scope. */
	if (node->kind == JS_NODE_FOR) {
		if (node->first != NULL)
			scope_visit(compiler, head, node->first);
		if (node->second != NULL)
			scope_visit(compiler, head, node->second);
		if (node->third != NULL)
			scope_visit(compiler, head, node->third);
		scope_visit(compiler, head, node->fourth);
		return;
	}

	/* for-in and for-of: the object outside, the left side and the body inside. */
	scope_visit(compiler, scope, node->second);
	scope_visit(compiler, head, node->first);
	scope_visit(compiler, head, node->fourth);
}

/* Resolves the names of a switch statement, whose cases share a scope when they declare a let or const. */
static void
scope_visit_switch(
	struct js_compiler *compiler,
	struct js_scope *scope,
	struct js_node *node)
{
	struct js_scope *cases;
	struct js_node *clause;

	/* The discriminant, outside. */
	scope_visit(compiler, scope, node->first);

	/* The cases' scope, noted on the node when it has one. */
	cases = scope_lexical_block(compiler, scope, node->second, 1);
	node->scope = NULL;
	if (cases != scope)
		node->scope = cases;

	/* Each case's test and statements. */
	for (clause = node->second; clause != NULL; clause = clause->next) {
		if (clause->first != NULL)
			scope_visit(compiler, cases, clause->first);
		scope_visit_list(compiler, cases, clause->second);
	}
}

/* Resolves the names of the defaults and patterns of a function's parameters (plain names need nothing). */
static void
scope_visit_parameters(
	struct js_compiler *compiler,
	struct js_function_info *info)
{
	struct js_node *parameter;

	/* A list of plain names. */
	if (info->simple_parameters)
		return;

	/* Each parameter that is not a plain name. */
	for (parameter = info->node->first; parameter != NULL; parameter = parameter->next) {
		if (parameter->kind != JS_NODE_IDENTIFIER)
			scope_visit(compiler, info->scope, parameter);
	}
}

/* Tells whether a statement is a let or const declaration. */
static int
scope_is_lexical(
	const struct js_node *node)
{
	/* Only a declaration of variables can be. */
	if (node->kind != JS_NODE_VARIABLES)
		return 0;

	/* var is not. */
	if (node->op == JS_P_VAR)
		return 0;

	/* let or const. */
	return 1;
}

/*
 * Fails the compilation when a var in a list of statements (down through
 * blocks, not into functions), or a function declared directly in it
 * (direct), has the name of a let or const of a block scope.
 */
static void
scope_check_vars(
	struct js_compiler *compiler,
	const struct js_scope *block,
	struct js_node *list,
	int direct)
{
	struct js_node *node;
	struct js_node *declarator;
	struct js_binding *declared;

	/* Each statement by its kind. */
	for (node = list; node != NULL; node = node->next) {
		switch (node->kind) {
		case JS_NODE_VARIABLES:
			/* A var's names (a let or const was checked when declared). */
			if (node->op != JS_P_VAR)
				break;
			for (declarator = node->first; declarator != NULL; declarator = declarator->next)
				scope_check_var_target(compiler, block, declarator->first);
			break;
		case JS_NODE_FUNCTION_DECLARATION:
			/* A function declared in the block itself. */
			if (!direct)
				break;
			declared = scope_find(block, node->text, node->text_length);
			if (declared != NULL)
				scope_redeclared(compiler, node);
			break;
		case JS_NODE_BLOCK:
			scope_check_vars(compiler, block, node->first, 0);
			break;
		case JS_NODE_IF:
			scope_check_vars(compiler, block, node->second, 0);
			scope_check_vars(compiler, block, node->third, 0);
			break;
		case JS_NODE_FOR:
		case JS_NODE_FOR_IN:
		case JS_NODE_FOR_OF:
			scope_check_vars(compiler, block, node->first, 0);
			scope_check_vars(compiler, block, node->fourth, 0);
			break;
		case JS_NODE_WHILE:
		case JS_NODE_DO_WHILE:
		case JS_NODE_WITH:
		case JS_NODE_LABELED:
			scope_check_vars(compiler, block, node->fourth, 0);
			break;
		case JS_NODE_TRY:
			scope_check_vars(compiler, block, node->first, 0);
			scope_check_vars(compiler, block, node->third, 0);
			scope_check_vars(compiler, block, node->fourth, 0);
			break;
		case JS_NODE_SWITCH:
			for (declarator = node->second; declarator != NULL; declarator = declarator->next)
				scope_check_vars(compiler, block, declarator->second, 0);
			break;
		default:
			break;
		}
	}
}

/* Fails the compilation when a var's target (a name, or the names of a pattern) names a let or const of a block scope. */
static void
scope_check_var_target(
	struct js_compiler *compiler,
	const struct js_scope *block,
	struct js_node *target)
{
	struct js_binding *declared;
	struct js_node *element;

	/* Nothing to check. */
	if (target == NULL)
		return;

	/* A name. */
	if (target->kind == JS_NODE_IDENTIFIER) {
		declared = scope_find(block, target->text, target->text_length);
		if (declared != NULL)
			scope_redeclared(compiler, target);
		return;
	}

	/* A pattern's elements. */
	for (element = target->first; element != NULL; element = element->next) {
		if (element->kind == JS_NODE_PROPERTY) {
			scope_check_var_target(compiler, block, element->second);
		} else {
			scope_check_var_target(compiler, block, element);
		}
	}
}

/* Fails the compilation with the SyntaxError of a name a scope declares twice. */
static void
scope_redeclared(
	struct js_compiler *compiler,
	const struct js_node *target)
{
	char message[200];
	char name[128];
	size_t index;

	/* The name in ASCII (other characters as ?), cut short when long. */
	for (index = 0; index < target->text_length && index + 1U < sizeof(name); index++) {
		name[index] = '?';
		if (target->text[index] < 0x80U)
			name[index] = (char)target->text[index];
	}

	/* Ends the copy as a C string. */
	name[index] = '\0';

	/* The early error, as Chromium words it. */
	snprintf(message, sizeof(message), "Identifier '%s' has already been declared", name);
	js_compile_fail(compiler, target, message);
}

/* Lists a function declaration a block makes when it is entered. */
static void
scope_block_function(
	struct js_compiler *compiler,
	struct js_scope *block,
	struct js_node *node)
{
	struct js_hoisted *entry;

	/* The entry, at the end of the block's list (source order). */
	entry = wb_arena_zalloc(compiler->arena, sizeof(*entry));
	if (entry == NULL)
		js_compile_out_of_memory(compiler);
	entry->node = node;
	if (block->functions_last == NULL) {
		block->functions = entry;
	} else {
		block->functions_last->next = entry;
	}

	/* The declaration is the last now. */
	block->functions_last = entry;
}

/*
 * Resolves the names of a class and makes the scopes of its functions: its
 * heritage (outside), a scope with its own name for a named class
 * expression, its methods, its constructor (the default one made when it
 * has none) whose scope the instance fields' initializers are resolved in,
 * and a function for its static fields and blocks.  The constructor is
 * noted in the class node's third, the static function in its fourth.
 */
static void
scope_visit_class(
	struct js_compiler *compiler,
	struct js_scope *scope,
	struct js_node *node)
{
	struct js_scope *inner;
	struct js_node *member;
	struct js_node *constructor;
	struct js_node *statics;
	struct js_function_info *info;
	struct js_binding *declared;
	const uint16_t *private_text;
	size_t private_length;
	uint32_t computed;
	int has_static;
	int has_private;
	int named;

	/* The heritage, in the scope around the class. */
	if (node->first != NULL)
		scope_visit(compiler, scope, node->first);

	/* A named class sees its own name as a const (apart from a declaration's outer let), and the class's code its private names. */
	inner = scope;
	node->scope = NULL;
	named = 0;
	if (node->text != NULL)
		named = 1;
	has_private = 0;
	for (member = node->second; member != NULL; member = member->next) {
		if ((member->kind == JS_NODE_METHOD || member->kind == JS_NODE_FIELD) && member->first->kind == JS_NODE_PRIVATE_NAME)
			has_private = 1;
		if (member->kind == JS_NODE_FIELD && (member->flags & JS_FLAG_COMPUTED) != 0U)
			has_private = 1;
	}

	/* The class's scope, when it needs one. */
	if (named || has_private) {
		inner = scope_new(compiler, scope, scope->function, JS_SCOPE_BLOCK);
		node->scope = inner;
	}

	/* Its own name. */
	if (named)
		scope_declare(compiler, inner, node->text, node->text_length, JS_BINDING_CONST);

	/*
	 * Each private name is a const of the class's scope (a getter and a
	 * setter share one), and so is each computed field name, which the
	 * class evaluates once for its constructor to use.
	 */
	computed = 0;
	for (member = node->second; member != NULL && has_private; member = member->next) {
		if (member->kind != JS_NODE_METHOD && member->kind != JS_NODE_FIELD)
			continue;
		if (member->kind == JS_NODE_FIELD && (member->flags & JS_FLAG_COMPUTED) != 0U) {
			private_text = scope_field_key_name(compiler, member, computed);
			computed++;
			scope_declare(compiler, inner, private_text, member->raw_length, JS_BINDING_CONST);
			continue;
		}

		/* Only private names are left. */
		if (member->first->kind != JS_NODE_PRIVATE_NAME)
			continue;
		private_text = js_private_name(compiler, member->first, &private_length);
		declared = scope_find(inner, private_text, private_length);
		if (declared != NULL)
			scope_check_private_pair(compiler, node, member);
		scope_declare(compiler, inner, private_text, private_length, JS_BINDING_CONST);
	}

	/* Each method (a computed key in the class's scope), and the constructor found. */
	constructor = NULL;
	has_static = 0;
	for (member = node->second; member != NULL; member = member->next) {
		if (member->kind == JS_NODE_METHOD) {
			if ((member->flags & JS_FLAG_COMPUTED) != 0U)
				scope_visit(compiler, inner, member->first);
			if (member->op == JS_PROPERTY_CONSTRUCTOR) {
				constructor = member->second;
				continue;
			}

			/* Any other method has its own function's scope. */
			scope_function(compiler, inner, member->second, 0);
			continue;
		}

		/* A computed field key is evaluated with the class; a static field or block runs in the static function. */
		if (member->kind == JS_NODE_FIELD && (member->flags & JS_FLAG_COMPUTED) != 0U)
			scope_visit(compiler, inner, member->first);
		if ((member->flags & JS_FLAG_STATIC) != 0U || member->kind == JS_NODE_STATIC_BLOCK)
			has_static = 1;
	}

	/* The constructor, the default one when the class has none. */
	if (constructor == NULL)
		constructor = scope_default_constructor(compiler, node);
	node->third = constructor;
	info = scope_function(compiler, inner, constructor, 0);
	info->class_node = node;

	/*
	 * The instance fields' initializers run in the constructor, which also
	 * uses the private names of the instance fields and methods (a use the
	 * code pass makes, so the names are captured).
	 */
	for (member = node->second; member != NULL; member = member->next) {
		if ((member->flags & JS_FLAG_STATIC) != 0U)
			continue;
		if (member->kind != JS_NODE_FIELD && member->kind != JS_NODE_METHOD)
			continue;
		if (member->first->kind == JS_NODE_PRIVATE_NAME)
			scope_visit(compiler, info->scope, member->first);
		if (member->kind == JS_NODE_FIELD && (member->flags & JS_FLAG_COMPUTED) != 0U)
			scope_reference(compiler, info->scope, member->raw, member->raw_length);
		if (member->kind == JS_NODE_FIELD && member->second != NULL)
			scope_visit(compiler, info->scope, member->second);
	}

	/* Without static fields or blocks, that is all. */
	node->fourth = NULL;
	if (!has_static)
		return;

	/* The static function (a method, whose this is the class). */
	statics = scope_new_node(compiler, JS_NODE_FUNCTION, node);
	statics->flags = JS_FLAG_METHOD | JS_FLAG_STRICT | JS_FLAG_STATIC_INIT;
	node->fourth = statics;
	info = scope_function(compiler, inner, statics, 0);
	info->class_node = node;

	/* Its fields' private names and values, and its blocks' statements (whose vars are the function's). */
	for (member = node->second; member != NULL; member = member->next) {
		if (member->kind == JS_NODE_FIELD && (member->flags & JS_FLAG_STATIC) != 0U && member->first->kind == JS_NODE_PRIVATE_NAME)
			scope_visit(compiler, info->scope, member->first);
		if (member->kind == JS_NODE_FIELD && (member->flags & JS_FLAG_STATIC) != 0U && (member->flags & JS_FLAG_COMPUTED) != 0U)
			scope_reference(compiler, info->scope, member->raw, member->raw_length);
		if (member->kind == JS_NODE_FIELD && (member->flags & JS_FLAG_STATIC) != 0U && member->second != NULL)
			scope_visit(compiler, info->scope, member->second);
		if (member->kind != JS_NODE_STATIC_BLOCK)
			continue;
		scope_declarations(compiler, info, member->first);
		scope_declare_lexicals(compiler, info->scope, member->first);
		scope_visit_list(compiler, info->scope, member->first);
	}
}

/*
 * Makes the default constructor of a class that has none: constructor()
 * {}, or for a derived class constructor(...args) { super(...args); }.
 */
static struct js_node *
scope_default_constructor(
	struct js_compiler *compiler,
	struct js_node *node)
{
	struct js_node *function;
	struct js_node *rest;
	struct js_node *name;
	struct js_node *statement;
	struct js_node *call;
	struct js_node *spread;

	/* The function, a strict method. */
	function = scope_new_node(compiler, JS_NODE_FUNCTION, node);
	function->flags = JS_FLAG_METHOD | JS_FLAG_STRICT;
	if (node->first == NULL)
		return function;

	/* A derived class's: the rest parameter args. */
	rest = scope_new_node(compiler, JS_NODE_REST, node);
	name = scope_new_node(compiler, JS_NODE_IDENTIFIER, node);
	name->text = scope_args_name;
	name->text_length = 4;
	rest->first = name;
	function->first = rest;

	/* Its body: super(...args). */
	statement = scope_new_node(compiler, JS_NODE_EXPRESSION_STATEMENT, node);
	call = scope_new_node(compiler, JS_NODE_CALL, node);
	call->first = scope_new_node(compiler, JS_NODE_SUPER, node);
	spread = scope_new_node(compiler, JS_NODE_SPREAD, node);
	name = scope_new_node(compiler, JS_NODE_IDENTIFIER, node);
	name->text = scope_args_name;
	name->text_length = 4;
	spread->first = name;
	call->second = spread;
	statement->first = call;
	function->second = statement;

	/* Succeeded: the constructor. */
	return function;
}

/*
 * Names the hidden const that keeps a computed field name's key: "#" and
 * the field's ordinal among the class's computed fields (no private name
 * starts with a digit), kept in the field node's raw text.
 */
static const uint16_t *
scope_field_key_name(
	struct js_compiler *compiler,
	struct js_node *member,
	uint32_t ordinal)
{
	uint16_t *text;
	char digits[16];
	size_t length;
	size_t index;

	/* The ordinal's digits. */
	snprintf(digits, sizeof(digits), "%u", (unsigned)ordinal);
	length = strlen(digits);

	/* "#" and the digits, in the arena. */
	text = wb_arena_alloc(compiler->arena, (length + 1U) * sizeof(uint16_t));
	if (text == NULL)
		js_compile_out_of_memory(compiler);
	text[0] = '#';
	for (index = 0; index < length; index++)
		text[index + 1U] = (uint16_t)digits[index];

	/* Succeeded: the name, kept on the node. */
	member->raw = text;
	member->raw_length = length + 1U;
	return text;
}

/*
 * Fails the compilation when a private name a class declares again is not
 * the other half of a getter and setter pair (both static or neither).
 */
static void
scope_check_private_pair(
	struct js_compiler *compiler,
	const struct js_node *class_node,
	const struct js_node *member)
{
	const struct js_node *earlier;
	int same;
	int pairs;

	/* Each earlier member with the same private name. */
	pairs = 0;
	for (earlier = class_node->second; earlier != member; earlier = earlier->next) {
		if (earlier->kind != JS_NODE_METHOD && earlier->kind != JS_NODE_FIELD)
			continue;
		if (earlier->first->kind != JS_NODE_PRIVATE_NAME)
			continue;
		same = js_text_equal(earlier->first->text, earlier->first->text_length, member->first->text, member->first->text_length);
		if (!same)
			continue;

		/* Only a getter and a setter of the same kind make a pair. */
		if (earlier->kind != JS_NODE_METHOD || member->kind != JS_NODE_METHOD)
			js_compile_fail(compiler, member, "Identifier has already been declared");
		if ((earlier->flags & JS_FLAG_STATIC) != (member->flags & JS_FLAG_STATIC))
			js_compile_fail(compiler, member, "Identifier has already been declared");
		if (!((earlier->op == JS_PROPERTY_GET && member->op == JS_PROPERTY_SET) ||
		    (earlier->op == JS_PROPERTY_SET && member->op == JS_PROPERTY_GET)))
			js_compile_fail(compiler, member, "Identifier has already been declared");
		pairs++;
	}

	/* A third member of the name. */
	if (pairs > 1)
		js_compile_fail(compiler, member, "Identifier has already been declared");
}

/* Makes a node of the compiler's own, placed where another is in the source. */
static struct js_node *
scope_new_node(
	struct js_compiler *compiler,
	int kind,
	const struct js_node *place)
{
	struct js_node *node;

	/* A zeroed node in the program's arena. */
	node = wb_arena_zalloc(compiler->arena, sizeof(*node));
	if (node == NULL)
		js_compile_out_of_memory(compiler);
	node->kind = kind;
	node->line = place->line;
	node->column = place->column;
	node->offset = place->offset;

	/* Succeeded: the node. */
	return node;
}

/*
 * Gives an arrow function the home object of the method around it for
 * super: that method keeps it in a hidden binding the arrow captures.
 */
static void
scope_arrow_home(
	struct js_compiler *compiler,
	struct js_scope *scope)
{
	struct js_function_info *owner;

	/* An ordinary function reads its own home object. */
	owner = scope->function;
	if (owner->program || (owner->node->flags & JS_FLAG_ARROW) == 0U)
		return;

	/* The nearest function around it that is not an arrow function. */
	while (!owner->program && (owner->node->flags & JS_FLAG_ARROW) != 0U)
		owner = owner->parent;

	/* Its hidden binding, which the arrow's use captures, and this, the receiver of super's properties. */
	scope_declare(compiler, owner->scope, js_home_name, JS_HOME_NAME_LENGTH, JS_BINDING_HOME);
	scope_reference(compiler, scope, js_home_name, JS_HOME_NAME_LENGTH);
	scope_arrow_this(compiler, scope);
}

/*
 * Gives an arrow function the this of the function around it: that
 * function keeps its this in a hidden binding, which the arrow captures
 * (an ordinary function's own this needs nothing).
 */
static void
scope_arrow_this(
	struct js_compiler *compiler,
	struct js_scope *scope)
{
	struct js_function_info *owner;

	/* An ordinary function (or the program) reads its own this. */
	owner = scope->function;
	if (owner->program || (owner->node->flags & JS_FLAG_ARROW) == 0U)
		return;

	/* The nearest function around it that is not an arrow function (the program at the latest). */
	while (!owner->program && (owner->node->flags & JS_FLAG_ARROW) != 0U)
		owner = owner->parent;

	/* Its hidden binding, which the arrow's use captures. */
	scope_declare(compiler, owner->scope, js_this_name, JS_THIS_NAME_LENGTH, JS_BINDING_THIS);
	scope_reference(compiler, scope, js_this_name, JS_THIS_NAME_LENGTH);
}

/*
 * Makes a var named arguments the function's arguments object: the var
 * does not replace it (a parameter or a function declaration of that name
 * does, and keeps its binding).
 */
static void
scope_arguments_var(
	struct js_function_info *info)
{
	struct js_binding *binding;
	int own_arguments;

	/* A var of that name, in a function that has an arguments object. */
	binding = scope_find(info->scope, scope_arguments_name, SCOPE_ARGUMENTS_LENGTH);
	if (binding == NULL || binding->kind != JS_BINDING_VAR)
		return;
	own_arguments = scope_has_own_arguments(info);
	if (!own_arguments)
		return;

	/* The var is the arguments object's binding. */
	binding->kind = JS_BINDING_ARGUMENTS;
	info->arguments = binding;
}

/* Tells whether a function has an arguments object of its own (the program and arrow functions do not). */
static int
scope_has_own_arguments(
	const struct js_function_info *info)
{
	/* The program. */
	if (info->program)
		return 0;

	/* An arrow function sees the one around it. */
	if ((info->node->flags & JS_FLAG_ARROW) != 0U)
		return 0;

	/* A function of its own. */
	return 1;
}
