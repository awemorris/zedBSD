/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GLSL compiler's checker: scopes and symbols, the types of every
 * declaration and expression, implicit conversions, overloads, lvalues,
 * the rules of each version and stage, and the constant values of
 * constant expressions.
 *
 * An expression that has an error gets the error type, and nothing is
 * reported again about an expression whose operand has the error type,
 * so one mistake gives one message.  After the checks, the code main
 * reaches is walked to mark the globals it uses (GL's "active"), to find
 * static recursion, and to find functions called but never defined.
 */

#include "internal.h"

#include <stdio.h>
#include <string.h>

/* The longest swizzle. */
#define CHECK_SWIZZLE_MAX	4U

/* The deepest nesting of loops and switches the checker tracks. */
#define CHECK_MAX_BREAKABLE	64U

/* What a break or continue leaves: a loop or a switch. */
#define CHECK_BREAKABLE_LOOP	1U
#define CHECK_BREAKABLE_SWITCH	2U

/*
 * The kinds of an argument's implicit conversion (check_conversion_rank):
 * none, float to double, an integer to float, an integer to double, int
 * to uint.
 */
#define CHECK_CONVERT_NONE	0U
#define CHECK_CONVERT_DOUBLE	1U
#define CHECK_CONVERT_FLOAT	2U
#define CHECK_CONVERT_INT_DOUBLE 3U
#define CHECK_CONVERT_UINT	4U

/* The most signatures of one built-in that fit a call with conversions. */
#define CHECK_MAX_CANDIDATES	16U

/* The most invocations of a geometry shader (GL's GL_MAX_GEOMETRY_SHADER_INVOCATIONS and Vulkan's least maximum). */
#define CHECK_MAX_INVOCATIONS	32U

/*
 * A built-in's signature that fits a call's arguments with conversions:
 * the built-in, its parameter types and its result type.
 */
struct check_candidate {
	const struct glsl_builtin *builtin;
	const struct glsl_type *parameters[8];
	const struct glsl_type *result;
};

/*
 * The loops and switches around the statement being checked, innermost
 * last (a continue needs a loop; one inside a switch is not supported).
 */
struct check_breakables {
	unsigned kinds[CHECK_MAX_BREAKABLE];
	unsigned count;
};

static void check_push_scope(struct glsl_shader *shader);
static void check_add_symbol(struct glsl_shader *shader, struct glsl_symbol *symbol, unsigned line);
static void check_pop_scope(struct glsl_shader *shader);
static struct glsl_symbol *check_lookup(struct glsl_shader *shader, const char *name);
static struct glsl_symbol *check_lookup_local(struct glsl_shader *shader, const char *name);
static int check_reserved_name(struct glsl_shader *shader, const char *name, unsigned line);
static void check_external(struct glsl_shader *shader, struct glsl_node *node, struct check_breakables *breakables);
static void check_precision(struct glsl_shader *shader, struct glsl_node *node);
static void check_invariant(struct glsl_shader *shader, struct glsl_node *node);
static void check_interface(struct glsl_shader *shader, struct glsl_node *node);
static void check_default_layout(struct glsl_shader *shader, struct glsl_node *node);
static const struct glsl_type *check_geometry_input(struct glsl_shader *shader, const struct glsl_type *type, unsigned line);
static int check_invocations(struct glsl_shader *shader, struct glsl_node *node);
static int check_stream(struct glsl_shader *shader, const struct glsl_node *type_node, unsigned line);
static int check_stream_call(struct glsl_shader *shader, struct glsl_node *node);
static void check_subroutine_type(struct glsl_shader *shader, struct glsl_node *node);
static void check_subroutine(struct glsl_shader *shader, struct glsl_node *node, struct glsl_function *function);
static int check_subroutine_matches(const struct glsl_function *type, const struct glsl_function *function);
static void check_subroutine_uniforms(struct glsl_shader *shader, struct glsl_node *node);
static const char *check_subroutine_hidden_name(struct glsl_shader *shader, const char *name);
static const struct glsl_type *check_subroutine_call(struct glsl_shader *shader, struct glsl_node *node, struct glsl_symbol *symbol, const struct glsl_type **types, unsigned count);
static void check_use_subroutines(struct glsl_shader *shader, struct glsl_node *node);
static void check_tess_layout(struct glsl_shader *shader, struct glsl_node *node);
static int check_tess_arrayed(const struct glsl_shader *shader, unsigned storage, unsigned patch);
static const struct glsl_type *check_tess_array(struct glsl_shader *shader, const struct glsl_type *type, unsigned storage, unsigned line);
static void check_patch(struct glsl_shader *shader, struct glsl_node *type_node, unsigned line);
static void check_uniform_block(struct glsl_shader *shader, struct glsl_node *node);
static void check_storage_block(struct glsl_shader *shader, struct glsl_node *node);
static void check_local_size(struct glsl_shader *shader, struct glsl_node *node);
static void check_compute_call(struct glsl_shader *shader, struct glsl_node *node);
static unsigned check_image_where(struct glsl_shader *shader, struct glsl_node *type_node, const struct glsl_type *type, unsigned line);
static void check_image_call(struct glsl_shader *shader, struct glsl_node *node);
static struct glsl_symbol *check_memory_root(struct glsl_node *node);
static int check_buffer_readonly(const struct glsl_symbol *symbol);
static const struct glsl_type *check_block_type(struct glsl_shader *shader, struct glsl_node *node, unsigned storage);
static void check_block_variable(struct glsl_shader *shader, const char *name, const struct glsl_type *type, unsigned where, unsigned interpolation, unsigned line);
static void check_location(struct glsl_shader *shader, struct glsl_node *type_node, struct glsl_symbol *symbol, unsigned line);
static void check_output_index(struct glsl_shader *shader, struct glsl_node *type_node, struct glsl_symbol *symbol, unsigned line);
static const struct glsl_type *check_type(struct glsl_shader *shader, struct glsl_node *type_node);
static const struct glsl_type *check_struct(struct glsl_shader *shader, struct glsl_node *type_node);
static const struct glsl_type *check_array(struct glsl_shader *shader, const struct glsl_type *element, struct glsl_node *owner, struct glsl_node *size, unsigned line);
static unsigned check_array_size(struct glsl_shader *shader, struct glsl_node *size, unsigned line);
static void check_declaration(struct glsl_shader *shader, struct glsl_node *node, int global);
static void check_variable(struct glsl_shader *shader, struct glsl_node *type_node, const struct glsl_type *base_type, struct glsl_node *variable, int global);
static unsigned check_where(struct glsl_shader *shader, struct glsl_node *type_node, const struct glsl_type *type, int global, unsigned line);
static void check_float_precision(struct glsl_shader *shader, unsigned precision, const struct glsl_type *type, unsigned line);
static void check_initializer(struct glsl_shader *shader, struct glsl_symbol *symbol, struct glsl_node *variable, int global);
static void check_function(struct glsl_shader *shader, struct glsl_node *node, struct check_breakables *breakables);
static struct glsl_function *check_prototype(struct glsl_shader *shader, struct glsl_node *node);
static struct glsl_symbol *check_parameter(struct glsl_shader *shader, struct glsl_node *parameter);
static int check_same_parameters(const struct glsl_function *function, struct glsl_symbol **parameters, unsigned count);
static void check_early_return(struct glsl_function *function);
static void check_statement(struct glsl_shader *shader, struct glsl_node *node, struct check_breakables *breakables);
static void check_block(struct glsl_shader *shader, struct glsl_node *node, int new_scope, struct check_breakables *breakables);
static void check_scoped(struct glsl_shader *shader, struct glsl_node *node, struct check_breakables *breakables);
static void check_condition(struct glsl_shader *shader, struct glsl_node *condition);
static void check_loop(struct glsl_shader *shader, struct glsl_node *node, struct check_breakables *breakables);
static void check_switch(struct glsl_shader *shader, struct glsl_node *node, struct check_breakables *breakables);
static void check_jump(struct glsl_shader *shader, struct glsl_node *node, struct check_breakables *breakables);
static void check_return(struct glsl_shader *shader, struct glsl_node *node);
static const struct glsl_type *check_expression(struct glsl_shader *shader, struct glsl_node *node);
static const struct glsl_type *check_expression_kind(struct glsl_shader *shader, struct glsl_node *node);
static const struct glsl_type *check_identifier(struct glsl_shader *shader, struct glsl_node *node);
static const struct glsl_type *check_unary(struct glsl_shader *shader, struct glsl_node *node);
static const struct glsl_type *check_increment(struct glsl_shader *shader, struct glsl_node *node);
static const struct glsl_type *check_binary(struct glsl_shader *shader, struct glsl_node *node);
static const struct glsl_type *check_operator(struct glsl_shader *shader, struct glsl_node *node, unsigned op, struct glsl_node **left, struct glsl_node **right);
static const struct glsl_type *check_arithmetic(unsigned op, const struct glsl_type *left, const struct glsl_type *right);
static void check_balance(struct glsl_shader *shader, struct glsl_node **left, struct glsl_node **right);
static const struct glsl_type *check_assign(struct glsl_shader *shader, struct glsl_node *node);
static const struct glsl_type *check_ternary(struct glsl_shader *shader, struct glsl_node *node);
static const struct glsl_type *check_call(struct glsl_shader *shader, struct glsl_node *node);
static unsigned check_arguments(struct glsl_shader *shader, struct glsl_node *node, const struct glsl_type **types, unsigned capacity, int *error);
static const struct glsl_type *check_constructor(struct glsl_shader *shader, struct glsl_node *node);
static int check_construct_components(struct glsl_shader *shader, struct glsl_node *node, const struct glsl_type *type);
static int check_construct_aggregate(struct glsl_shader *shader, struct glsl_node *node, const struct glsl_type *type);
static const struct glsl_type *check_user_call(struct glsl_shader *shader, struct glsl_node *node, struct glsl_symbol *symbol, const struct glsl_type **types, unsigned count);
static struct glsl_function *check_overload(struct glsl_shader *shader, struct glsl_function *functions, const struct glsl_type **types, unsigned count, int convert, int *ambiguous);
static const struct glsl_type *check_builtin_call(struct glsl_shader *shader, struct glsl_node *node, const struct glsl_type **types, unsigned count);
static const struct glsl_type *check_field(struct glsl_shader *shader, struct glsl_node *node);
static int check_swizzle(struct glsl_shader *shader, struct glsl_node *node, const struct glsl_type *type);
static const struct glsl_type *check_index(struct glsl_shader *shader, struct glsl_node *node);
static const struct glsl_type *check_length(struct glsl_shader *shader, struct glsl_node *node);
static struct glsl_node *check_convert(struct glsl_shader *shader, struct glsl_node *node, const struct glsl_type *target);
static int check_can_convert(const struct glsl_shader *shader, const struct glsl_type *from, const struct glsl_type *to);
static unsigned check_conversion_rank(const struct glsl_type *from, const struct glsl_type *to);
static int check_overload_fits(const struct glsl_shader *shader, const struct glsl_function *function, const struct glsl_type **types, unsigned count, int convert);
static int check_better_overload(struct glsl_function *first, struct glsl_function *second, const struct glsl_type **types, unsigned count);
static int check_better_conversion(const struct glsl_type *from, const struct glsl_type *first, const struct glsl_type *second);
static struct check_candidate *check_best_builtin(const struct glsl_shader *shader, struct glsl_node *node, const struct glsl_type **types, unsigned count, struct check_candidate *candidates, int *ambiguous);
static int check_better_candidate(const struct check_candidate *first, const struct check_candidate *second, const struct glsl_type **types, unsigned count);
static const struct glsl_type *check_builtin_found(struct glsl_shader *shader, struct glsl_node *node, const struct glsl_builtin *builtin, const struct glsl_type **parameters, const struct glsl_type *result);
static int check_gather(struct glsl_shader *shader, struct glsl_node *node, const struct glsl_builtin *builtin);
static int check_texture_offset(struct glsl_shader *shader, struct glsl_node *node, const struct glsl_builtin *builtin);
static int check_interpolant(struct glsl_shader *shader, struct glsl_node *argument, const struct glsl_type *parameter);
static void check_flat_error(struct glsl_shader *shader, const struct glsl_type *type, unsigned line);
static void check_convert_arguments(struct glsl_shader *shader, struct glsl_node *call, const struct glsl_type **parameters);
static int check_lvalue(struct glsl_shader *shader, struct glsl_node *node, const char *what);
static void check_type_error(struct glsl_shader *shader, unsigned line, const char *format, const struct glsl_type *first, const struct glsl_type *second);
static void check_mark_side_effects(struct glsl_node *node);
static void check_use(struct glsl_shader *shader, struct glsl_node *node);
static struct glsl_symbol *check_block_array(struct glsl_node *node);
static uint32_t check_all_elements(const struct glsl_type *type);
static void check_use_function(struct glsl_shader *shader, struct glsl_function *function, unsigned line);

/*
 * Checks a parsed shader: every declaration, function and statement, main,
 * and which globals the code main reaches uses.
 */
void
glsl_check(
	struct glsl_shader *shader)
{
	struct check_breakables breakables;
	struct glsl_node *node;
	struct glsl_symbol *symbol;
	struct glsl_function *function;
	unsigned mask;

	/* A compute shader needs GLSL 4.30 or OpenGL ES 3.10. */
	mask = glsl_version_mask(shader);
	if (shader->stage == GLSL_STAGE_COMPUTE && (mask & GLSL_IN_ES310) == 0U) {
		glsl_error(shader, 0U, "compute shaders need GLSL 4.30 or OpenGL ES 3.10");
		return;
	}

	/* A tessellation shader needs GLSL 4.00 or GL_ARB_tessellation_shader. */
	if ((shader->stage == GLSL_STAGE_TESS_CONTROL || shader->stage == GLSL_STAGE_TESS_EVALUATION) && (mask & GLSL_IN_TESS) == 0U) {
		glsl_error(shader, 0U, "tessellation shaders need GLSL 4.00 or GL_ARB_tessellation_shader");
		return;
	}

	/* The global scope with the built-ins; OpenGL ES fragment shaders have no default float precision. */
	check_push_scope(shader);
	shader->scope->float_precision = GLSL_PRECISION_HIGH;
	if (shader->es && shader->stage == GLSL_STAGE_FRAGMENT)
		shader->scope->float_precision = GLSL_PRECISION_NONE;
	glsl_builtin_variables(shader);

	/* Each external declaration in order. */
	memset(&breakables, 0, sizeof(breakables));
	for (node = shader->unit; node != NULL; node = node->next)
		check_external(shader, node, &breakables);

	/* main: void main() with a body. */
	symbol = check_lookup(shader, "main");
	function = NULL;
	if (symbol != NULL && symbol->kind == GLSL_SYMBOL_FUNCTION)
		function = symbol->functions;
	if (function == NULL || function->body == NULL) {
		glsl_error(shader, 0U, "the shader has no main function");
		return;
	}

	/* Succeeded: main. */
	shader->main = function;

	/* A compute shader says its workgroup size (ws101-p008). */
	if (shader->stage == GLSL_STAGE_COMPUTE && shader->local_size[0] == 0U)
		glsl_error(shader, 0U, "a compute shader needs its workgroup size ('layout(local_size_x = 64) in;')");

	/* A tessellation control shader says its output patch's vertices, an evaluation shader its domain. */
	if (shader->stage == GLSL_STAGE_TESS_CONTROL && shader->tess_vertices == 0U)
		glsl_error(shader, 0U, "a tessellation control shader needs its output vertices ('layout(vertices = 3) out;')");
	if (shader->stage == GLSL_STAGE_TESS_EVALUATION && shader->tess_domain == GLSL_PRIMITIVE_NONE)
		glsl_error(shader, 0U, "a tessellation evaluation shader needs its domain ('layout(triangles) in;')");

	/* A geometry shader says its primitives and how many vertices it emits. */
	if (shader->stage == GLSL_STAGE_GEOMETRY) {
		if (shader->geometry_input == GLSL_PRIMITIVE_NONE)
			glsl_error(shader, 0U, "a geometry shader needs an input primitive layout ('layout(triangles) in;')");
		if (shader->geometry_output == GLSL_PRIMITIVE_NONE || shader->max_vertices == 0U)
			glsl_error(shader, 0U, "a geometry shader needs an output layout with max_vertices");

		/* Streams other than 0 come with points only (GLSL 4.00's rule). */
		if ((shader->geometry_streams & ~1U) != 0U && shader->geometry_output != GLSL_PRIMITIVE_POINTS)
			glsl_error(shader, 0U, "a geometry shader with vertex streams other than 0 must output points");
	}

	/* The code main reaches: the globals it uses, and the functions it calls. */
	if (shader->errors == 0U)
		check_use_function(shader, function, function->line);
}

/*
 * Declares a name in the current scope.  A name already declared in the
 * same scope is an error (the old symbol is returned then, so the caller
 * goes on without a second error).
 */
struct glsl_symbol *
glsl_declare(
	struct glsl_shader *shader,
	const char *name,
	unsigned kind,
	unsigned line)
{
	struct glsl_symbol *symbol;
	struct glsl_scope_entry *entry;

	/* One declaration per scope. */
	symbol = check_lookup_local(shader, name);
	if (symbol != NULL) {
		glsl_error(shader, line, "'%s' is already declared in this scope", name);
		return symbol;
	}

	/* The symbol, without a location of its own. */
	symbol = glsl_alloc(&shader->arena, sizeof(*symbol));
	symbol->name = name;
	symbol->kind = kind;
	symbol->line = line;
	symbol->explicit_location = GLSL_NO_LOCATION;

	/* Its entry, first in the scope's list. */
	entry = glsl_alloc(&shader->arena, sizeof(*entry));
	entry->symbol = symbol;
	entry->next = shader->scope->entries;
	shader->scope->entries = entry;

	/* Succeeded: the new symbol. */
	return symbol;
}

/*
 * Appends a symbol to the shader's globals, in declaration order.
 */
void
glsl_add_global(
	struct glsl_shader *shader,
	struct glsl_symbol *symbol)
{
	/* The first, or after the last. */
	if (shader->last_global == NULL) {
		shader->globals = symbol;
	} else {
		shader->last_global->next_global = symbol;
	}

	/* It is the last now. */
	shader->last_global = symbol;
}

/* Opens a scope inside the current one; it starts with the enclosing default precision. */
static void
check_push_scope(
	struct glsl_shader *shader)
{
	struct glsl_scope *scope;

	/* The scope. */
	scope = glsl_alloc(&shader->arena, sizeof(*scope));
	scope->parent = shader->scope;
	if (scope->parent != NULL)
		scope->float_precision = scope->parent->float_precision;
	shader->scope = scope;
}

/* Puts an existing symbol (a parameter) into the current scope. */
static void
check_add_symbol(
	struct glsl_shader *shader,
	struct glsl_symbol *symbol,
	unsigned line)
{
	struct glsl_scope_entry *entry;
	struct glsl_symbol *existing;

	/* One declaration per scope. */
	existing = check_lookup_local(shader, symbol->name);
	if (existing != NULL) {
		glsl_error(shader, line, "'%s' is already declared in this scope", symbol->name);
		return;
	}

	/* The entry, first in the scope's list. */
	entry = glsl_alloc(&shader->arena, sizeof(*entry));
	entry->symbol = symbol;
	entry->next = shader->scope->entries;
	shader->scope->entries = entry;
}

/* Closes the current scope. */
static void
check_pop_scope(
	struct glsl_shader *shader)
{
	/* The enclosing scope becomes current. */
	shader->scope = shader->scope->parent;
}

/* Finds a name in the scopes from the innermost out. */
static struct glsl_symbol *
check_lookup(
	struct glsl_shader *shader,
	const char *name)
{
	struct glsl_scope *scope;
	struct glsl_scope_entry *entry;
	int differs;

	/* Each scope, innermost first. */
	for (scope = shader->scope; scope != NULL; scope = scope->parent) {
		for (entry = scope->entries; entry != NULL; entry = entry->next) {
			differs = strcmp(entry->symbol->name, name);
			if (differs == 0)
				return entry->symbol;
		}
	}

	/* Not declared. */
	return NULL;
}

/* Finds a name in the current scope only. */
static struct glsl_symbol *
check_lookup_local(
	struct glsl_shader *shader,
	const char *name)
{
	struct glsl_scope_entry *entry;
	int differs;

	/* The current scope's entries. */
	for (entry = shader->scope->entries; entry != NULL; entry = entry->next) {
		differs = strcmp(entry->symbol->name, name);
		if (differs == 0)
			return entry->symbol;
	}

	/* Not declared here. */
	return NULL;
}

/* Reports (and returns nonzero for) a name the shader may not declare: one starting with gl_. */
static int
check_reserved_name(
	struct glsl_shader *shader,
	const char *name,
	unsigned line)
{
	int differs;

	/* The gl_ prefix is the implementation's. */
	differs = strncmp(name, "gl_", 3U);
	if (differs != 0)
		return 0;

	/* Such a name is refused. */
	glsl_error(shader, line, "names starting with 'gl_' are reserved ('%s')", name);
	return 1;
}

/* Checks one external declaration. */
static void
check_external(
	struct glsl_shader *shader,
	struct glsl_node *node,
	struct check_breakables *breakables)
{
	/* The kinds of external declarations. */
	switch (node->kind) {
	case GLSL_N_PRECISION:
		check_precision(shader, node);
		break;
	case GLSL_N_INVARIANT:
		check_invariant(shader, node);
		break;
	case GLSL_N_FUNCTION:
		if ((node->flags & GLSL_NODE_SUBROUTINE_TYPE) != 0U) {
			check_subroutine_type(shader, node);
		} else {
			check_function(shader, node, breakables);
		}
		break;
	case GLSL_N_DECLARATION:
		if ((node->flags & GLSL_NODE_SUBROUTINE_UNIFORM) != 0U) {
			check_subroutine_uniforms(shader, node);
		} else {
			check_declaration(shader, node, 1);
		}
		break;
	case GLSL_N_INTERFACE:
		check_interface(shader, node);
		break;
	case GLSL_N_EMPTY:
		check_default_layout(shader, node);
		break;
	default:
		glsl_error(shader, node->line, "unexpected declaration");
		break;
	}
}

/* Checks a precision statement and sets the scope's default float precision. */
static void
check_precision(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	const struct glsl_type *type;

	/* The type: float, int or a sampler. */
	type = node->child[0]->type;
	if (type == NULL || (type->kind != GLSL_KIND_SCALAR && type->kind != GLSL_KIND_SAMPLER)) {
		glsl_error(shader, node->line, "precision statements apply to float, int and samplers");
		return;
	}

	/* Of the scalars, float and int only. */
	if (type->kind == GLSL_KIND_SCALAR && type->base != GLSL_BASE_FLOAT && type->base != GLSL_BASE_INT) {
		glsl_error(shader, node->line, "precision statements apply to float, int and samplers");
		return;
	}

	/* Float's default in this scope. */
	if (type->kind == GLSL_KIND_SCALAR && type->base == GLSL_BASE_FLOAT)
		shader->scope->float_precision = node->precision;
}

/*
 * Checks "invariant NAME, ..." at global scope: each name an output of
 * the vertex shader; or "precise NAME, ...": each name a variable (the
 * parser already made the shader's operations uncontracted).
 */
static void
check_invariant(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	struct glsl_node *name;
	struct glsl_symbol *symbol;

	/* Each name. */
	for (name = node->child[0]; name != NULL; name = name->next) {
		symbol = check_lookup(shader, name->name);
		if (symbol == NULL || symbol->kind != GLSL_SYMBOL_VARIABLE) {
			glsl_error(shader, name->line, "'%s' is not declared", name->name);
			continue;
		}

		/* Any variable can be precise. */
		if (node->precise)
			continue;

		/* Only outputs can be invariant. */
		if (symbol->where != GLSL_VAR_OUTPUT) {
			glsl_error(shader, name->line, "'%s' is not an output and cannot be invariant", name->name);
			continue;
		}

		/* The output is invariant. */
		symbol->invariant = 1U;
	}
}

/*
 * Checks an interface block: a uniform block (uniform_block), or an in or
 * out block of desktop GLSL 1.50 (its members are one struct variable
 * when it has an instance name, separate variables otherwise).
 */
static void
check_interface(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	struct glsl_node *type_node;
	struct glsl_node *instance;
	const struct glsl_type *type;
	const struct glsl_type *array;
	unsigned storage;
	unsigned where;
	unsigned index;
	int allowed;
	int arrayed;

	/* A uniform block. */
	type_node = node->child[0];
	storage = type_node->storage;
	if (storage == GLSL_STORAGE_UNIFORM) {
		check_uniform_block(shader, node);
		return;
	}

	/* A shader storage block (ws101-p008). */
	if (storage == GLSL_STORAGE_BUFFER) {
		check_storage_block(shader, node);
		return;
	}

	/* in and out blocks came with desktop GLSL 1.50: out of the vertex shader, into the fragment shader. */
	allowed = glsl_since(shader, GLSL_VERSION_150, 0U);
	if (!allowed) {
		glsl_error(shader, node->line, "in and out blocks need desktop GLSL 1.50");
		return;
	}

	/* Out of the vertex shader, into the fragment shader, into and out of the geometry shader. */
	where = GLSL_VAR_OUTPUT;
	if (storage == GLSL_STORAGE_IN)
		where = GLSL_VAR_INPUT;
	if ((shader->stage == GLSL_STAGE_VERTEX && storage != GLSL_STORAGE_OUT) ||
	    (shader->stage == GLSL_STAGE_FRAGMENT && storage != GLSL_STORAGE_IN)) {
		glsl_error(shader, node->line, "a block can only be the vertex shader's output or the fragment shader's input");
		return;
	}

	/* The block's struct, and the stream of a geometry shader's output block. */
	type = check_block_type(shader, node, storage);
	if (type->kind == GLSL_KIND_ERROR)
		return;
	if ((type_node->layout & GLSL_LAYOUT_STREAM) != 0U)
		(void)check_stream(shader, type_node, node->line);

	/* A geometry shader's input block is an array of the vertices, with an instance name. */
	instance = node->child[2];
	if (shader->stage == GLSL_STAGE_GEOMETRY && storage == GLSL_STORAGE_IN) {
		if (instance == NULL || (instance->flags & GLSL_NODE_ARRAY) == 0U) {
			glsl_error(shader, node->line, "a geometry shader's input block needs an instance name and \"[]\"");
			return;
		}

		/* The array of the vertices' blocks. */
		array = check_array(shader, type, instance, instance->child[0], node->line);
		array = check_geometry_input(shader, array, node->line);
		check_block_variable(shader, instance->name, array, where, type_node->interpolation, node->line);
		return;
	}

	/* A tessellation shader's per-vertex block is an array of the patch's vertices, with an instance name (no patch blocks). */
	if (type_node->patch) {
		glsl_error(shader, node->line, "patch blocks are not supported (declare patch variables)");
		return;
	}
	arrayed = check_tess_arrayed(shader, storage, 0U);
	if (arrayed) {
		if (instance == NULL || (instance->flags & GLSL_NODE_ARRAY) == 0U) {
			glsl_error(shader, node->line, "a tessellation shader's per-vertex block needs an instance name and \"[]\"");
			return;
		}

		/* The array of the vertices' blocks. */
		array = check_array(shader, type, instance, instance->child[0], node->line);
		array = check_tess_array(shader, array, storage, node->line);
		check_block_variable(shader, instance->name, array, where, type_node->interpolation, node->line);
		return;
	}

	/* With an instance name: one variable of the struct. */
	if (instance != NULL) {
		if ((instance->flags & GLSL_NODE_ARRAY) != 0U)
			glsl_error(shader, node->line, "arrays of in and out blocks are not supported");
		check_block_variable(shader, instance->name, type, where, type_node->interpolation, node->line);
		return;
	}

	/* Without one: each member a variable of its own. */
	for (index = 0U; index < type->field_count; index++)
		check_block_variable(shader, type->fields[index].name, type->fields[index].type, where, type->fields[index].interpolation, node->line);
}

/*
 * Checks qualifiers alone: a geometry shader's input primitive ("layout(
 * triangles) in;", which sizes gl_in and the unsized inputs after it),
 * or its output primitive, most vertices and stream ("layout(
 * triangle_strip, max_vertices = 3) out;").  Any other (a uniform block's
 * default layout) changes nothing: std140 is the only layout.
 */
static void
check_default_layout(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	static const unsigned vertices[6] = { 0U, 1U, 2U, 4U, 3U, 6U };
	struct glsl_node *type_node;
	struct glsl_symbol *in;
	unsigned primitive;
	int status;

	/* The stream the outputs after it go to (a geometry shader's, GL_ARB_gpu_shader5). */
	type_node = node->child[0];
	if ((type_node->layout & GLSL_LAYOUT_STREAM) != 0U) {
		status = check_stream(shader, type_node, node->line);
		if (status != 0)
			return;
		shader->default_stream = type_node->stream;
	}

	/* A compute shader's workgroup size (ws101-p008). */
	if ((type_node->layout & GLSL_LAYOUT_LOCAL_SIZE) != 0U) {
		check_local_size(shader, node);
		return;
	}

	/* A tessellation shader's patch layouts. */
	if (shader->stage == GLSL_STAGE_TESS_CONTROL || shader->stage == GLSL_STAGE_TESS_EVALUATION) {
		check_tess_layout(shader, node);
		return;
	}

	/* The tessellation layouts elsewhere. */
	if ((type_node->layout & (GLSL_LAYOUT_VERTICES | GLSL_LAYOUT_SPACING | GLSL_LAYOUT_ORDER | GLSL_LAYOUT_POINT_MODE)) != 0U) {
		glsl_error(shader, node->line, "vertices, spacing, vertex order and point_mode layouts are for tessellation shaders");
		return;
	}

	/* A layout of primitives, vertices or invocations, in a geometry shader. */
	if ((type_node->layout & (GLSL_LAYOUT_PRIMITIVE | GLSL_LAYOUT_MAX_VERTICES | GLSL_LAYOUT_INVOCATIONS)) == 0U)
		return;
	if (shader->stage != GLSL_STAGE_GEOMETRY) {
		glsl_error(shader, node->line, "primitive and invocations layouts are for geometry shaders");
		return;
	}

	/* The invocations of each primitive: an input layout's (GL_ARB_gpu_shader5). */
	if ((type_node->layout & GLSL_LAYOUT_INVOCATIONS) != 0U) {
		status = check_invocations(shader, node);
		if (status != 0)
			return;
	}

	/* The input primitive: one of the five, once (an input layout may give only the invocations). */
	primitive = type_node->primitive;
	if (type_node->storage == GLSL_STORAGE_IN) {
		if ((type_node->layout & GLSL_LAYOUT_PRIMITIVE) == 0U && (type_node->layout & GLSL_LAYOUT_MAX_VERTICES) == 0U)
			return;
		if (primitive == GLSL_PRIMITIVE_NONE ||
		    primitive > GLSL_PRIMITIVE_TRIANGLES_ADJACENCY ||
		    (type_node->layout & GLSL_LAYOUT_MAX_VERTICES) != 0U) {
			glsl_error(shader, node->line, "an input layout names points, lines, lines_adjacency, triangles or triangles_adjacency");
			return;
		}

		/* Declared once, or again the same. */
		if (shader->geometry_input != GLSL_PRIMITIVE_NONE && shader->geometry_input != primitive) {
			glsl_error(shader, node->line, "the input primitive is declared twice, differently");
			return;
		}

		/* It, its vertices, and gl_in sized by them. */
		shader->geometry_input = primitive;
		shader->geometry_vertices = vertices[primitive];
		in = check_lookup(shader, "gl_in");
		if (in != NULL && in->type->kind == GLSL_KIND_ARRAY)
			in->type = glsl_type_array(&shader->arena, in->type->element, shader->geometry_vertices);
		return;
	}

	/* Otherwise an output: points, line_strip or triangle_strip, and the most vertices. */
	if (type_node->storage != GLSL_STORAGE_OUT) {
		glsl_error(shader, node->line, "primitive layouts qualify in or out");
		return;
	}

	/* The output primitive, when given. */
	if ((type_node->layout & GLSL_LAYOUT_PRIMITIVE) != 0U) {
		if (primitive != GLSL_PRIMITIVE_POINTS &&
		    primitive != GLSL_PRIMITIVE_LINE_STRIP &&
		    primitive != GLSL_PRIMITIVE_TRIANGLE_STRIP) {
			glsl_error(shader, node->line, "an output layout names points, line_strip or triangle_strip");
			return;
		}

		/* Kept. */
		shader->geometry_output = primitive;
	}

	/* Succeeded: the most vertices, when given. */
	if ((type_node->layout & GLSL_LAYOUT_MAX_VERTICES) != 0U) {
		if (type_node->max_vertices == 0U)
			glsl_error(shader, node->line, "max_vertices must be greater than 0");
		shader->max_vertices = type_node->max_vertices;
	}
}

/*
 * Checks a geometry shader's "layout(invocations = N) in;": GLSL 4.00 or
 * GL_ARB_gpu_shader5, an input layout, 1 to 32 invocations (GL's and
 * Vulkan's least maximum), declared once or again the same.  Returns 0
 * when it is kept, -1 after an error.
 */
static int
check_invocations(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	struct glsl_node *type_node;
	unsigned mask;

	/* The version. */
	type_node = node->child[0];
	mask = glsl_version_mask(shader);
	if ((mask & GLSL_IN_SHADER5) == 0U) {
		glsl_error(shader, node->line, "the invocations layout needs GLSL 4.00 or GL_ARB_gpu_shader5");
		return -1;
	}

	/* An input's layout. */
	if (type_node->storage != GLSL_STORAGE_IN) {
		glsl_error(shader, node->line, "invocations is an input layout");
		return -1;
	}

	/* 1 to 32. */
	if (type_node->invocations == 0U || type_node->invocations > CHECK_MAX_INVOCATIONS) {
		glsl_error(shader, node->line, "a geometry shader has 1 to %u invocations", CHECK_MAX_INVOCATIONS);
		return -1;
	}

	/* Declared once, or again the same. */
	if (shader->geometry_invocations != 0U && shader->geometry_invocations != type_node->invocations) {
		glsl_error(shader, node->line, "the invocations are declared twice, differently");
		return -1;
	}

	/* Succeeded: the invocations are kept. */
	shader->geometry_invocations = type_node->invocations;
	return 0;
}

/*
 * Returns the type of a geometry shader's input: an array of the input
 * primitive's vertices (an unsized one takes their number, a sized one
 * must have it); an error type when it is not an array or the input
 * layout is not declared yet.
 */
static const struct glsl_type *
check_geometry_input(
	struct glsl_shader *shader,
	const struct glsl_type *type,
	unsigned line)
{
	/* An error stays one. */
	if (type->kind == GLSL_KIND_ERROR)
		return type;

	/* An array, after the input layout. */
	if (type->kind != GLSL_KIND_ARRAY) {
		glsl_error(shader, line, "a geometry shader's inputs are arrays of the vertices ('in vec4 v[];')");
		return glsl_type_error();
	}

	/* The vertices' number is known once the input layout came. */
	if (shader->geometry_vertices == 0U) {
		glsl_error(shader, line, "declare the input primitive ('layout(triangles) in;') before the inputs");
		return glsl_type_error();
	}

	/* An unsized array takes the vertices' number. */
	if (type->length == 0U) {
		type = glsl_type_array(&shader->arena, type->element, shader->geometry_vertices);
		return type;
	}

	/* A sized one must have it. */
	if (type->length != shader->geometry_vertices) {
		glsl_error(shader, line, "the input array's size is not the input primitive's %u vertices", shader->geometry_vertices);
		return glsl_type_error();
	}

	/* Succeeded: the array. */
	return type;
}

/*
 * Checks the declaration of a subroutine type (GLSL 4.00,
 * GL_ARB_shader_subroutine): a prototype of a name not declared before,
 * which becomes the type's name.
 */
static void
check_subroutine_type(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	struct glsl_symbol *symbol;
	struct glsl_function *function;

	/* A new name, and a prototype without a body. */
	symbol = check_lookup_local(shader, node->name);
	if (symbol != NULL) {
		glsl_error(shader, node->line, "'%s' is already declared in this scope", node->name);
		return;
	}
	if (node->child[2] != NULL) {
		glsl_error(shader, node->line, "subroutine type '%s' cannot have a body (a subroutine names its types: 'subroutine(T)')",
			   node->name);
		return;
	}

	/* The prototype, declared under the name. */
	function = check_prototype(shader, node);
	if (function == NULL)
		return;

	/* Succeeded: the name is the subroutine type's. */
	symbol = check_lookup_local(shader, node->name);
	if (symbol != NULL)
		symbol->kind = GLSL_SYMBOL_SUBROUTINE;
}

/*
 * Makes a function a subroutine of the subroutine types it lists, each a
 * type of its return type and parameters, and gives it the next index.
 */
static void
check_subroutine(
	struct glsl_shader *shader,
	struct glsl_node *node,
	struct glsl_function *function)
{
	struct glsl_function **types;
	struct glsl_symbol *symbol;
	struct glsl_node *named;
	unsigned count;
	int matches;

	/* The types' list. */
	count = 0U;
	for (named = node->child[3]; named != NULL; named = named->next)
		count++;
	types = glsl_alloc(&shader->arena, (count + 1U) * sizeof(*types));

	/* Each type: a subroutine type the function matches. */
	count = 0U;
	for (named = node->child[3]; named != NULL; named = named->next) {
		symbol = check_lookup(shader, named->name);
		if (symbol == NULL || symbol->kind != GLSL_SYMBOL_SUBROUTINE) {
			glsl_error(shader, named->line, "'%s' is not a subroutine type", named->name);
			return;
		}
		matches = check_subroutine_matches(symbol->functions, function);
		if (!matches) {
			glsl_error(shader, named->line, "subroutine '%s' does not have the return type and parameters of '%s'", node->name,
				   named->name);
			return;
		}
		types[count] = symbol->functions;
		count++;
	}

	/* Room for its index. */
	if (shader->subroutine_count >= GLSL_MAX_SUBROUTINES) {
		glsl_error(shader, node->line, "a shader has at most %u subroutines", GLSL_MAX_SUBROUTINES);
		return;
	}

	/* Succeeded: the subroutine, last of the shader's. */
	function->subroutine_types = types;
	function->subroutine_type_count = count;
	function->subroutine_index = shader->subroutine_count;
	shader->subroutine_count++;
	if (shader->last_subroutine == NULL) {
		shader->subroutines = function;
	} else {
		shader->last_subroutine->next_subroutine = function;
	}
	shader->last_subroutine = function;
}

/* Reports whether a function has a subroutine type's return type and parameters (types and qualifiers). */
static int
check_subroutine_matches(
	const struct glsl_function *type,
	const struct glsl_function *function)
{
	unsigned index;
	int same;

	/* The same return type and number of parameters. */
	same = glsl_type_equal(type->return_type, function->return_type);
	if (!same)
		return 0;
	if (type->parameter_count != function->parameter_count)
		return 0;

	/* Each parameter of the same type and qualifier. */
	for (index = 0U; index < type->parameter_count; index++) {
		same = glsl_type_equal(type->parameters[index]->type, function->parameters[index]->type);
		if (!same)
			return 0;
		if (type->parameters[index]->storage != function->parameters[index]->storage)
			return 0;
	}

	/* Succeeded: the function is of the type. */
	return 1;
}

/*
 * Checks a declaration of subroutine uniforms: each a name of a
 * subroutine type (its type node's name), without an initializer, with
 * its hidden uniform: a uint (or an array of them) libGLESv2 fills with
 * the index of the subroutine a call runs.
 */
static void
check_subroutine_uniforms(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	struct glsl_symbol *type_symbol;
	struct glsl_symbol *symbol;
	struct glsl_symbol *hidden;
	struct glsl_node *variable;
	const struct glsl_type *type;
	unsigned locations;
	unsigned size;

	/* The subroutine type. */
	type_symbol = check_lookup(shader, node->child[0]->name);
	if (type_symbol == NULL || type_symbol->kind != GLSL_SYMBOL_SUBROUTINE) {
		glsl_error(shader, node->line, "'%s' is not a subroutine type", node->child[0]->name);
		return;
	}

	/* The locations the shader's subroutine uniforms have so far. */
	locations = 0U;
	for (symbol = shader->subroutine_uniforms; symbol != NULL; symbol = symbol->next_subroutine) {
		size = 1U;
		if (symbol->type->kind == GLSL_KIND_ARRAY)
			size = symbol->type->length;
		locations += size;
	}

	/* Each uniform. */
	for (variable = node->child[1]; variable != NULL; variable = variable->next) {
		if (variable->child[1] != NULL)
			glsl_error(shader, variable->line, "subroutine uniform '%s' cannot have an initializer", variable->name);

		/* A uint, or an array of them of a size. */
		type = glsl_type_scalar(GLSL_BASE_UINT);
		if ((variable->flags & GLSL_NODE_ARRAY) != 0U)
			type = check_array(shader, type, variable, variable->child[0], variable->line);
		if (type->kind == GLSL_KIND_ERROR)
			continue;
		if (type->kind == GLSL_KIND_ARRAY && type->length == 0U) {
			glsl_error(shader, variable->line, "array '%s' has no size", variable->name);
			continue;
		}

		/* Room for its locations. */
		size = 1U;
		if (type->kind == GLSL_KIND_ARRAY)
			size = type->length;
		if (locations + size > GLSL_MAX_SUBROUTINE_LOCATIONS) {
			glsl_error(shader, variable->line, "a shader's subroutine uniforms have at most %u locations", GLSL_MAX_SUBROUTINE_LOCATIONS);
			return;
		}
		locations += size;

		/* The uniform's symbol, of the subroutine type. */
		check_reserved_name(shader, variable->name, variable->line);
		symbol = glsl_declare(shader, variable->name, GLSL_SYMBOL_SUBROUTINE_UNIFORM, variable->line);
		if (symbol->kind != GLSL_SYMBOL_SUBROUTINE_UNIFORM || symbol->functions != NULL)
			continue;
		symbol->functions = type_symbol->functions;
		symbol->type = type;

		/* Its hidden uniform, among the globals (the link finds it once a call uses it). */
		hidden = glsl_alloc(&shader->arena, sizeof(*hidden));
		hidden->name = check_subroutine_hidden_name(shader, variable->name);
		hidden->kind = GLSL_SYMBOL_VARIABLE;
		hidden->line = variable->line;
		hidden->type = type;
		hidden->storage = GLSL_STORAGE_UNIFORM;
		hidden->where = GLSL_VAR_UNIFORM;
		hidden->precision = GLSL_PRECISION_HIGH;
		hidden->explicit_location = GLSL_NO_LOCATION;
		glsl_add_global(shader, hidden);
		symbol->hidden = hidden;

		/* Succeeded: the uniform, last of the shader's. */
		shader->subroutine_uniform_count++;
		if (shader->last_subroutine_uniform == NULL) {
			shader->subroutine_uniforms = symbol;
		} else {
			shader->last_subroutine_uniform->next_subroutine = symbol;
		}
		shader->last_subroutine_uniform = symbol;
		variable->symbol = symbol;
	}
}

/* Returns the name of a subroutine uniform's hidden uniform: gl_ZedSubroutine, the stage's number, '_' and the name. */
static const char *
check_subroutine_hidden_name(
	struct glsl_shader *shader,
	const char *name)
{
	char *hidden;
	size_t size;

	/* The name's room. */
	size = strlen(name) + sizeof("gl_ZedSubroutine0_");
	hidden = glsl_alloc(&shader->arena, size);

	/* Succeeded: the name. */
	(void)snprintf(hidden, size, "gl_ZedSubroutine%u_%s", shader->stage, name);
	return hidden;
}

/*
 * Checks a call through a subroutine uniform (an element of an array of
 * them for "NAME[i](...)"): arguments its subroutine type takes, as a
 * call of the type's prototype; the call's child[0] becomes the hidden
 * uniform (or its element) whose value selects the subroutine that runs.
 */
static const struct glsl_type *
check_subroutine_call(
	struct glsl_shader *shader,
	struct glsl_node *node,
	struct glsl_symbol *symbol,
	const struct glsl_type **types,
	unsigned count)
{
	const struct glsl_type *result;
	const struct glsl_type *index_type;
	struct glsl_node *selector;
	struct glsl_node *element;
	int32_t value;

	/* The arguments, as the type's prototype takes them. */
	result = check_user_call(shader, node, symbol, types, count);
	if (result->kind == GLSL_KIND_ERROR)
		return result;

	/* The hidden uniform. */
	selector = glsl_alloc(&shader->arena, sizeof(*selector));
	selector->kind = GLSL_N_IDENTIFIER;
	selector->line = node->line;
	selector->name = symbol->hidden->name;
	selector->symbol = symbol->hidden;
	selector->type = symbol->hidden->type;

	/* A single uniform is called by its name. */
	if (symbol->type->kind != GLSL_KIND_ARRAY) {
		if (node->child[2] != NULL) {
			glsl_error(shader, node->line, "subroutine uniform '%s' is not an array", node->name);
			return glsl_type_error();
		}
		node->child[0] = selector;
		node->flags |= GLSL_NODE_SUBROUTINE_CALL;
		return result;
	}

	/* An array's element is called by its index: an integer inside the array. */
	if (node->child[2] == NULL) {
		glsl_error(shader, node->line, "an array of subroutine uniforms is called by element ('%s[i](...)')", node->name);
		return glsl_type_error();
	}
	index_type = check_expression(shader, node->child[2]);
	if (index_type->kind == GLSL_KIND_ERROR)
		return glsl_type_error();
	if (index_type->kind != GLSL_KIND_SCALAR || (index_type->base != GLSL_BASE_INT && index_type->base != GLSL_BASE_UINT)) {
		check_type_error(shader, node->line, "an index must be an integer, not %s", index_type, NULL);
		return glsl_type_error();
	}
	if (node->child[2]->constant != NULL) {
		value = node->child[2]->constant->values[0].i;
		if (value < 0 || (uint32_t)value >= symbol->type->length) {
			glsl_error(shader, node->line, "index %d is outside the %u elements", (int)value, symbol->type->length);
			return glsl_type_error();
		}
	}

	/* The element of the hidden array. */
	element = glsl_alloc(&shader->arena, sizeof(*element));
	element->kind = GLSL_N_INDEX;
	element->line = node->line;
	element->child[0] = selector;
	element->child[1] = node->child[2];
	element->type = glsl_type_scalar(GLSL_BASE_UINT);
	node->child[2] = NULL;

	/* Succeeded: the call selects by the element. */
	node->child[0] = element;
	node->flags |= GLSL_NODE_SUBROUTINE_CALL;
	return result;
}

/*
 * Checks a stream layout (GL_ARB_gpu_shader5): a geometry shader's
 * output, in GLSL 4.00 or with the extension, of a stream 0 to 3, which
 * the shader is then known to name.  Returns 0, or -1 after an error.
 */
static int
check_stream(
	struct glsl_shader *shader,
	const struct glsl_node *type_node,
	unsigned line)
{
	unsigned mask;

	/* GLSL 4.00 or GL_ARB_gpu_shader5. */
	mask = glsl_version_mask(shader);
	if ((mask & GLSL_IN_SHADER5) == 0U) {
		glsl_error(shader, line, "the stream layout needs GLSL 4.00 or GL_ARB_gpu_shader5");
		return -1;
	}

	/* A geometry shader's output. */
	if (shader->stage != GLSL_STAGE_GEOMETRY || type_node->storage != GLSL_STORAGE_OUT) {
		glsl_error(shader, line, "stream is a geometry shader's output layout");
		return -1;
	}

	/* A stream there is. */
	if (type_node->stream >= GLSL_MAX_VERTEX_STREAMS) {
		glsl_error(shader, line, "a vertex stream is 0 to %u", GLSL_MAX_VERTEX_STREAMS - 1U);
		return -1;
	}

	/* Succeeded: the shader names the stream. */
	shader->geometry_streams |= 1U << type_node->stream;
	return 0;
}

/*
 * Checks the stream of EmitStreamVertex or EndStreamPrimitive: a constant
 * of a stream there is, which the shader is then known to name.  Returns
 * 0, or -1 after an error.
 */
static int
check_stream_call(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	struct glsl_node *argument;
	int32_t stream;

	/* A constant. */
	argument = node->child[1];
	if (argument->constant == NULL) {
		glsl_error(shader, argument->line, "%s's stream must be a constant expression", node->builtin->name);
		return -1;
	}

	/* A stream there is. */
	stream = argument->constant->values[0].i;
	if (stream < 0 || stream >= (int32_t)GLSL_MAX_VERTEX_STREAMS) {
		glsl_error(shader, argument->line, "a vertex stream is 0 to %u", GLSL_MAX_VERTEX_STREAMS - 1U);
		return -1;
	}

	/* Succeeded: the shader names the stream. */
	shader->geometry_streams |= 1U << (unsigned)stream;
	return 0;
}

/*
 * Checks a tessellation shader's qualifiers alone: a control shader's
 * output vertices ("layout(vertices = 3) out;", which sizes gl_out and
 * the unsized per-vertex outputs after it), or an evaluation shader's
 * domain, spacing, vertex order and point mode ("layout(triangles,
 * equal_spacing, ccw) in;").  Each may be declared again only the same.
 */
static void
check_tess_layout(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	struct glsl_node *type_node;
	struct glsl_symbol *out;
	unsigned layout;

	/* The layouts given, and only a stage's own. */
	type_node = node->child[0];
	layout = type_node->layout;
	if ((layout & (GLSL_LAYOUT_PRIMITIVE | GLSL_LAYOUT_VERTICES | GLSL_LAYOUT_SPACING | GLSL_LAYOUT_ORDER | GLSL_LAYOUT_POINT_MODE)) == 0U)
		return;
	if ((layout & GLSL_LAYOUT_MAX_VERTICES) != 0U) {
		glsl_error(shader, node->line, "max_vertices is a geometry shader's layout");
		return;
	}

	/* A control shader: "layout(vertices = N) out;" and nothing else. */
	if (shader->stage == GLSL_STAGE_TESS_CONTROL) {
		if (type_node->storage != GLSL_STORAGE_OUT || layout != GLSL_LAYOUT_VERTICES) {
			glsl_error(shader, node->line, "a tessellation control shader's layout is 'layout(vertices = N) out;'");
			return;
		}
		if (type_node->vertices == 0U || type_node->vertices > GLSL_MAX_PATCH_VERTICES) {
			glsl_error(shader, node->line, "vertices must be 1 to %u", GLSL_MAX_PATCH_VERTICES);
			return;
		}
		if (shader->tess_vertices != 0U && shader->tess_vertices != type_node->vertices) {
			glsl_error(shader, node->line, "the output vertices are declared twice, differently");
			return;
		}

		/* The vertices, and gl_out sized by them. */
		shader->tess_vertices = type_node->vertices;
		out = check_lookup(shader, "gl_out");
		if (out != NULL && out->type->kind == GLSL_KIND_ARRAY)
			out->type = glsl_type_array(&shader->arena, out->type->element, shader->tess_vertices);
		return;
	}

	/* An evaluation shader: input layouts of its domain and how it is cut. */
	if (type_node->storage != GLSL_STORAGE_IN || (layout & GLSL_LAYOUT_VERTICES) != 0U) {
		glsl_error(shader, node->line, "a tessellation evaluation shader's layouts qualify 'in'");
		return;
	}

	/* The domain: triangles, quads or isolines, once. */
	if ((layout & GLSL_LAYOUT_PRIMITIVE) != 0U) {
		if (type_node->primitive != GLSL_PRIMITIVE_TRIANGLES &&
		    type_node->primitive != GLSL_PRIMITIVE_QUADS &&
		    type_node->primitive != GLSL_PRIMITIVE_ISOLINES) {
			glsl_error(shader, node->line, "a tessellation domain is triangles, quads or isolines");
			return;
		}
		if (shader->tess_domain != GLSL_PRIMITIVE_NONE && shader->tess_domain != type_node->primitive) {
			glsl_error(shader, node->line, "the domain is declared twice, differently");
			return;
		}

		/* Kept. */
		shader->tess_domain = type_node->primitive;
	}

	/* The spacing and the vertex order, each once. */
	if ((layout & GLSL_LAYOUT_SPACING) != 0U) {
		if (shader->tess_spacing != 0U && shader->tess_spacing != type_node->spacing)
			glsl_error(shader, node->line, "the spacing is declared twice, differently");
		shader->tess_spacing = type_node->spacing;
	}
	if ((layout & GLSL_LAYOUT_ORDER) != 0U) {
		if (shader->tess_order != 0U && shader->tess_order != type_node->order)
			glsl_error(shader, node->line, "the vertex order is declared twice, differently");
		shader->tess_order = type_node->order;
	}

	/* Points instead of the domain's primitives. */
	if ((layout & GLSL_LAYOUT_POINT_MODE) != 0U)
		shader->tess_point_mode = 1U;
}

/*
 * Reports whether a tessellation shader's input or output of a storage
 * is per vertex (an array of the patch's vertices): every input of both
 * stages and every output of a control shader, but not a patch variable.
 */
static int
check_tess_arrayed(
	const struct glsl_shader *shader,
	unsigned storage,
	unsigned patch)
{
	/* A patch variable is one per patch. */
	if (patch)
		return 0;

	/* Both stages' inputs, a control shader's outputs. */
	if (shader->stage == GLSL_STAGE_TESS_CONTROL && (storage == GLSL_STORAGE_IN || storage == GLSL_STORAGE_OUT))
		return 1;
	if (shader->stage == GLSL_STAGE_TESS_EVALUATION && storage == GLSL_STORAGE_IN)
		return 1;

	/* Anything else is not. */
	return 0;
}

/*
 * Returns the type of a tessellation shader's per-vertex input or output:
 * an array of the patch's vertices.  An unsized input takes
 * gl_MaxPatchVertices; an unsized control shader's output the output
 * vertices (declared before it), a sized one must have them.  An error
 * type when it is not an array.
 */
static const struct glsl_type *
check_tess_array(
	struct glsl_shader *shader,
	const struct glsl_type *type,
	unsigned storage,
	unsigned line)
{
	/* An error stays one; anything else must be an array. */
	if (type->kind == GLSL_KIND_ERROR)
		return type;
	if (type->kind != GLSL_KIND_ARRAY) {
		glsl_error(shader, line, "a tessellation shader's per-vertex inputs and outputs are arrays ('in vec4 v[];')");
		return glsl_type_error();
	}

	/* An input: unsized is gl_MaxPatchVertices. */
	if (storage == GLSL_STORAGE_IN) {
		if (type->length == 0U)
			type = glsl_type_array(&shader->arena, type->element, GLSL_MAX_PATCH_VERTICES);
		return type;
	}

	/* An output: the vertices' number is known once the layout came. */
	if (shader->tess_vertices == 0U) {
		glsl_error(shader, line, "declare the output vertices ('layout(vertices = 3) out;') before the outputs");
		return glsl_type_error();
	}

	/* An unsized one takes it, a sized one must have it. */
	if (type->length == 0U) {
		type = glsl_type_array(&shader->arena, type->element, shader->tess_vertices);
		return type;
	}
	if (type->length != shader->tess_vertices) {
		glsl_error(shader, line, "the output array's size is not the %u output vertices", shader->tess_vertices);
		return glsl_type_error();
	}

	/* Succeeded: the array. */
	return type;
}

/* Checks a patch qualifier: only a tessellation control shader's output or an evaluation shader's input has one. */
static void
check_patch(
	struct glsl_shader *shader,
	struct glsl_node *type_node,
	unsigned line)
{
	/* A control shader's output. */
	if (shader->stage == GLSL_STAGE_TESS_CONTROL && type_node->storage == GLSL_STORAGE_OUT)
		return;

	/* An evaluation shader's input. */
	if (shader->stage == GLSL_STAGE_TESS_EVALUATION && type_node->storage == GLSL_STORAGE_IN)
		return;

	/* Anything else. */
	glsl_error(shader, line, "patch qualifies a tessellation control shader's outputs and an evaluation shader's inputs");
}

/*
 * Checks a uniform block (GLSL 1.40, OpenGL ES 3.00): its struct, and its
 * instance name (GLSL 1.50 and OpenGL ES 3.00 on), or its members as
 * names of their own that read from it.
 */
static void
check_uniform_block(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	struct glsl_node *instance;
	struct glsl_symbol *block;
	struct glsl_symbol *member;
	const struct glsl_type *type;
	unsigned index;
	int allowed;

	/* The version. */
	allowed = glsl_since(shader, GLSL_VERSION_140, GLSL_VERSION_ES300);
	if (!allowed) {
		glsl_error(shader, node->line, "uniform blocks need GLSL 1.40 or OpenGL ES 3.00");
		return;
	}

	/*
	 * An instance name, and with it an array of blocks, came with GLSL
	 * 1.50's interface blocks: GLSL 1.40's uniform block has neither.
	 */
	instance = node->child[2];
	allowed = glsl_since(shader, GLSL_VERSION_150, GLSL_VERSION_ES300);
	if (instance != NULL && !allowed) {
		glsl_error(shader, node->line, "a uniform block's instance name needs GLSL 1.50 or OpenGL ES 3.00");
		return;
	}

	/* The struct, and an array of blocks when the instance name has a size (each element a block of its own). */
	type = check_block_type(shader, node, GLSL_STORAGE_UNIFORM);
	if (instance != NULL && (instance->flags & GLSL_NODE_ARRAY) != 0U) {
		if ((instance->flags & GLSL_NODE_UNSIZED) != 0U) {
			glsl_error(shader, node->line, "an array of uniform blocks needs a size");
			return;
		}
		type = check_array(shader, type, instance, instance->child[0], node->line);
		if (type->kind == GLSL_KIND_ERROR)
			return;
	}

	/* The block's symbol: the instance, or one without a scope named by the block. */
	if (instance != NULL) {
		check_reserved_name(shader, instance->name, node->line);
		block = glsl_declare(shader, instance->name, GLSL_SYMBOL_VARIABLE, node->line);
		if (block->type != NULL)
			return;
	} else {
		block = glsl_alloc(&shader->arena, sizeof(*block));
		block->name = node->name;
		block->kind = GLSL_SYMBOL_VARIABLE;
		block->line = node->line;
		block->explicit_location = GLSL_NO_LOCATION;
	}

	/* A uniform read from its own buffer. */
	block->type = type;
	block->where = GLSL_VAR_BLOCK;
	block->storage = GLSL_STORAGE_UNIFORM;
	glsl_add_global(shader, block);
	if (instance != NULL)
		return;

	/* Each member a name of its own, reading from the block. */
	for (index = 0U; index < type->field_count; index++) {
		check_reserved_name(shader, type->fields[index].name, node->line);
		member = glsl_declare(shader, type->fields[index].name, GLSL_SYMBOL_VARIABLE, node->line);
		if (member->type != NULL)
			continue;
		member->type = type->fields[index].type;
		member->where = GLSL_VAR_BLOCK_MEMBER;
		member->storage = GLSL_STORAGE_UNIFORM;
		member->block = block;
		member->member = index;
	}
}

/* Makes the struct type of an interface block's members, with their matrix orders and interpolations. */
static const struct glsl_type *
check_block_type(
	struct glsl_shader *shader,
	struct glsl_node *node,
	unsigned storage)
{
	struct glsl_type *type;
	struct glsl_node *member;
	struct glsl_node *variable;
	const struct glsl_type *member_type;
	const struct glsl_type *field_type;
	unsigned count;
	unsigned index;
	unsigned row_major;
	unsigned interpolation;
	int has_sampler;

	/* How many members. */
	count = 0U;
	for (member = node->child[1]; member != NULL; member = member->next) {
		for (variable = member->child[1]; variable != NULL; variable = variable->next)
			count++;
	}

	/* The struct, named by the block. */
	type = glsl_alloc(&shader->arena, sizeof(*type));
	type->kind = GLSL_KIND_STRUCT;
	type->name = node->name;
	type->block = storage;
	type->fields = glsl_alloc(&shader->arena, (count + 1U) * sizeof(*type->fields));

	/* Each member: its type, its matrix order (the block's unless its own), its interpolation. */
	index = 0U;
	for (member = node->child[1]; member != NULL; member = member->next) {
		member_type = check_type(shader, member->child[0]);
		row_major = ((node->child[0]->layout | member->child[0]->layout) & GLSL_LAYOUT_ROW_MAJOR) != 0U;
		if ((member->child[0]->layout & GLSL_LAYOUT_COLUMN_MAJOR) != 0U)
			row_major = 0U;
		interpolation = member->child[0]->interpolation;
		if (interpolation == GLSL_INTERP_NONE)
			interpolation = node->child[0]->interpolation;
		for (variable = member->child[1]; variable != NULL; variable = variable->next) {
			field_type = member_type;
			if ((variable->flags & GLSL_NODE_ARRAY) != 0U)
				field_type = check_array(shader, member_type, variable, variable->child[0], variable->line);

			/* No samplers, no bools out of a stage, integers flat into a fragment shader. */
			has_sampler = glsl_type_contains_sampler(field_type);
			if (has_sampler)
				glsl_error(shader, variable->line, "samplers cannot be block members");
			if (storage != GLSL_STORAGE_UNIFORM &&
			    storage != GLSL_STORAGE_BUFFER &&
			    field_type->kind != GLSL_KIND_ARRAY &&
			    field_type->base == GLSL_BASE_BOOL)
				glsl_error(shader, variable->line, "inputs and outputs cannot be bools");

			/* A run-time array only as a storage block's last member (ws101-p008). */
			if (field_type->kind == GLSL_KIND_ARRAY && field_type->length == 0U &&
			    (storage != GLSL_STORAGE_BUFFER || variable->next != NULL || member->next != NULL))
				glsl_error(shader, variable->line, "only a storage block's last member can be an array without a size");

			/* Memory qualifiers qualify the block, not its members (ws101-p008). */
			if (member->child[0]->memory != 0U)
				glsl_error(shader, variable->line, "memory qualifiers on block members are not supported (qualify the block)");
			if (storage == GLSL_STORAGE_IN &&
			    shader->stage == GLSL_STAGE_FRAGMENT &&
			    field_type->kind != GLSL_KIND_ARRAY &&
			    field_type->base != GLSL_BASE_FLOAT &&
			    interpolation != GLSL_INTERP_FLAT)
				check_flat_error(shader, field_type, variable->line);

			/* The field. */
			type->fields[index].name = variable->name;
			type->fields[index].type = field_type;
			type->fields[index].row_major = row_major;
			type->fields[index].interpolation = interpolation;
			index++;
		}
	}

	/* Succeeded: the struct. */
	type->field_count = index;
	return type;
}

/* Declares a global variable of an interface block (its instance, or one member). */
static void
check_block_variable(
	struct glsl_shader *shader,
	const char *name,
	const struct glsl_type *type,
	unsigned where,
	unsigned interpolation,
	unsigned line)
{
	struct glsl_symbol *symbol;

	/* The name, then the symbol among the globals. */
	check_reserved_name(shader, name, line);
	symbol = glsl_declare(shader, name, GLSL_SYMBOL_VARIABLE, line);
	if (symbol->type != NULL)
		return;
	symbol->type = type;
	symbol->where = where;
	symbol->interpolation = interpolation;
	symbol->storage = GLSL_STORAGE_IN;
	if (where == GLSL_VAR_OUTPUT)
		symbol->storage = GLSL_STORAGE_OUT;
	glsl_add_global(shader, symbol);
}

/* Checks a layout location: only a vertex shader's input or a fragment shader's output takes one (3.30, ES 3.00). */
static void
check_location(
	struct glsl_shader *shader,
	struct glsl_node *type_node,
	struct glsl_symbol *symbol,
	unsigned line)
{
	int allowed;

	/* The version. */
	allowed = glsl_since(shader, GLSL_VERSION_330, GLSL_VERSION_ES300);
	if (!allowed) {
		glsl_error(shader, line, "layout(location) needs GLSL 3.30 or OpenGL ES 3.00");
		return;
	}

	/* A vertex input or a fragment output. */
	if (!(shader->stage == GLSL_STAGE_VERTEX && symbol->where == GLSL_VAR_INPUT) &&
	    !(shader->stage == GLSL_STAGE_FRAGMENT && symbol->where == GLSL_VAR_OUTPUT)) {
		glsl_error(shader, line, "layout(location) is only for vertex shader inputs and fragment shader outputs");
		return;
	}

	/* The location. */
	symbol->explicit_location = type_node->location;
}

/* Checks a layout index: a desktop GLSL 3.30 fragment shader's output with a location, 0 or 1 (which colour of a dual-source blend). */
static void
check_output_index(
	struct glsl_shader *shader,
	struct glsl_node *type_node,
	struct glsl_symbol *symbol,
	unsigned line)
{
	int allowed;

	/* The version: desktop GLSL 3.30. */
	allowed = glsl_since(shader, GLSL_VERSION_330, 0U);
	if (!allowed) {
		glsl_error(shader, line, "layout(index) needs GLSL 3.30");
		return;
	}

	/* A fragment output. */
	if (shader->stage != GLSL_STAGE_FRAGMENT || symbol->where != GLSL_VAR_OUTPUT) {
		glsl_error(shader, line, "layout(index) is only for fragment shader outputs");
		return;
	}

	/* With a location of its own. */
	if ((type_node->layout & GLSL_LAYOUT_LOCATION) == 0U) {
		glsl_error(shader, line, "layout(index) needs a layout(location) too");
		return;
	}

	/* The first or the second colour. */
	if (type_node->output_index > 1U) {
		glsl_error(shader, line, "layout(index) must be 0 or 1");
		return;
	}

	/* The index. */
	symbol->explicit_index = type_node->output_index;
}

/* Resolves a type specifier node to a type: a built-in, a struct defined or named, and an array of it. */
static const struct glsl_type *
check_type(
	struct glsl_shader *shader,
	struct glsl_node *type_node)
{
	const struct glsl_type *type;
	struct glsl_symbol *symbol;

	/* A struct definition, a struct's name, or a built-in type. */
	if ((type_node->flags & GLSL_NODE_STRUCT_DEFINITION) != 0U) {
		type = check_struct(shader, type_node);
	} else if (type_node->type != NULL) {
		type = type_node->type;
	} else {
		/* The struct of the name. */
		symbol = check_lookup(shader, type_node->name);
		if (symbol == NULL || symbol->kind != GLSL_SYMBOL_TYPE) {
			glsl_error(shader, type_node->line, "'%s' is not a type", type_node->name);
			return glsl_type_error();
		}

		/* The struct's type. */
		type = symbol->type;
	}

	/* "[size]" on the type (1.20 on). */
	if ((type_node->flags & GLSL_NODE_ARRAY) != 0U)
		type = check_array(shader, type, type_node, type_node->child[1], type_node->line);

	/* Succeeded: the type. */
	return type;
}

/* Makes the struct type a definition declares, and declares its name. */
static const struct glsl_type *
check_struct(
	struct glsl_shader *shader,
	struct glsl_node *type_node)
{
	struct glsl_type *type;
	struct glsl_node *member;
	struct glsl_node *variable;
	const struct glsl_type *member_type;
	const struct glsl_type *field_type;
	struct glsl_symbol *symbol;
	unsigned count;
	unsigned index;
	unsigned other;
	int differs;
	int has_sampler;

	/* A definition is resolved once. */
	if (type_node->symbol != NULL)
		return type_node->symbol->type;

	/* How many fields. */
	count = 0U;
	for (member = type_node->child[0]; member != NULL; member = member->next) {
		for (variable = member->child[1]; variable != NULL; variable = variable->next)
			count++;
	}

	/* The type. */
	type = glsl_alloc(&shader->arena, sizeof(*type));
	type->kind = GLSL_KIND_STRUCT;
	type->name = type_node->name;
	if (type->name == NULL)
		type->name = "<anonymous>";
	type->fields = glsl_alloc(&shader->arena, (count + 1U) * sizeof(*type->fields));

	/* Each member's fields. */
	index = 0U;
	for (member = type_node->child[0]; member != NULL; member = member->next) {
		member_type = check_type(shader, member->child[0]);
		check_float_precision(shader, member->child[0]->precision, member_type, member->line);
		for (variable = member->child[1]; variable != NULL; variable = variable->next) {
			field_type = member_type;
			if ((variable->flags & GLSL_NODE_ARRAY) != 0U)
				field_type = check_array(shader, member_type, variable, variable->child[0], variable->line);

			/* Samplers in structs, and initializers, are not supported. */
			has_sampler = glsl_type_contains_sampler(field_type);
			if (has_sampler)
				glsl_error(shader, variable->line, "samplers in structs are not supported");
			if (variable->child[1] != NULL)
				glsl_error(shader, variable->line, "struct members cannot have initializers");

			/* The name must be new in the struct. */
			for (other = 0U; other < index; other++) {
				differs = strcmp(type->fields[other].name, variable->name);
				if (differs == 0)
					glsl_error(shader, variable->line, "member '%s' repeated", variable->name);
			}

			/* The field. */
			type->fields[index].name = variable->name;
			type->fields[index].type = field_type;
			index++;
		}
	}

	/* The fields are all in. */
	type->field_count = index;

	/* The struct's name becomes a type in this scope. */
	symbol = NULL;
	if (type_node->name != NULL) {
		check_reserved_name(shader, type_node->name, type_node->line);
		symbol = glsl_declare(shader, type_node->name, GLSL_SYMBOL_TYPE, type_node->line);
		symbol->type = type;
	} else {
		/* An anonymous struct keeps its type on a symbol of no scope. */
		symbol = glsl_alloc(&shader->arena, sizeof(*symbol));
		symbol->kind = GLSL_SYMBOL_TYPE;
		symbol->type = type;
	}

	/* Succeeded: the struct type, remembered on the definition. */
	type_node->symbol = symbol;
	return type;
}

/* Makes an array type of an element, from "[size]" on a node (or "[]" left unsized). */
static const struct glsl_type *
check_array(
	struct glsl_shader *shader,
	const struct glsl_type *element,
	struct glsl_node *owner,
	struct glsl_node *size,
	unsigned line)
{
	unsigned length;

	/* An error stays one. */
	if (element->kind == GLSL_KIND_ERROR)
		return element;

	/* Arrays of arrays came with GLSL 4.30. */
	if (element->kind == GLSL_KIND_ARRAY) {
		glsl_error(shader, line, "arrays of arrays are not supported");
		return glsl_type_error();
	}

	/* Nor can void be an element. */
	if (element->kind == GLSL_KIND_VOID) {
		glsl_error(shader, line, "an array of void");
		return glsl_type_error();
	}

	/* "[]": the size comes from an initializer. */
	if ((owner->flags & GLSL_NODE_UNSIZED) != 0U) {
		length = 0U;
	} else {
		length = check_array_size(shader, size, line);
		if (length == 0U)
			return glsl_type_error();
	}

	/* Succeeded: the array type. */
	return glsl_type_array(&shader->arena, element, length);
}

/* Returns the value of an array size: a constant integer expression above 0 (0 after an error). */
static unsigned
check_array_size(
	struct glsl_shader *shader,
	struct glsl_node *size,
	unsigned line)
{
	const struct glsl_type *type;

	/* The expression. */
	type = check_expression(shader, size);
	if (type->kind == GLSL_KIND_ERROR)
		return 0U;

	/* A constant scalar integer. */
	if (type->kind != GLSL_KIND_SCALAR || (type->base != GLSL_BASE_INT && type->base != GLSL_BASE_UINT) || size->constant == NULL) {
		glsl_error(shader, line, "an array size must be a constant integer expression");
		return 0U;
	}

	/* Above 0. */
	if (type->base == GLSL_BASE_INT && size->constant->values[0].i <= 0) {
		glsl_error(shader, line, "an array size must be greater than 0");
		return 0U;
	}

	/* An unsigned size is above 0 unless it is 0. */
	if (size->constant->values[0].u == 0U) {
		glsl_error(shader, line, "an array size must be greater than 0");
		return 0U;
	}

	/* Succeeded: the size. */
	return size->constant->values[0].u;
}

/* Checks a declaration (global or local): its type, and each variable it declares. */
static void
check_declaration(
	struct glsl_shader *shader,
	struct glsl_node *node,
	int global)
{
	struct glsl_node *type_node;
	struct glsl_node *variable;
	const struct glsl_type *type;

	/* The type (a struct definition declares its name here). */
	type_node = node->child[0];
	type = check_type(shader, type_node);

	/* A struct alone declares nothing else, but qualifiers need a variable. */
	if (node->child[1] == NULL) {
		if (type_node->storage != GLSL_STORAGE_NONE || type_node->invariant || type_node->precise)
			glsl_error(shader, node->line, "qualifiers without a variable");
		if ((type_node->flags & GLSL_NODE_STRUCT_DEFINITION) == 0U)
			glsl_error(shader, node->line, "a declaration declares nothing");
		return;
	}

	/* Each variable. */
	for (variable = node->child[1]; variable != NULL; variable = variable->next)
		check_variable(shader, type_node, type, variable, global);
}

/* Checks one declared variable and adds its symbol. */
static void
check_variable(
	struct glsl_shader *shader,
	struct glsl_node *type_node,
	const struct glsl_type *base_type,
	struct glsl_node *variable,
	int global)
{
	struct glsl_symbol *symbol;
	const struct glsl_type *type;
	const struct glsl_type *element;
	unsigned where;
	int arrayed;
	int status;

	/* The variable's type: the declaration's, or an array of it. */
	type = base_type;
	if ((variable->flags & GLSL_NODE_ARRAY) != 0U)
		type = check_array(shader, base_type, variable, variable->child[0], variable->line);
	if (type->kind == GLSL_KIND_VOID) {
		glsl_error(shader, variable->line, "variable '%s' of type void", variable->name);
		type = glsl_type_error();
	}

	/* A geometry shader's input: an array of the input primitive's vertices. */
	if (global && shader->stage == GLSL_STAGE_GEOMETRY && type_node->storage == GLSL_STORAGE_IN)
		type = check_geometry_input(shader, type, variable->line);

	/* A tessellation shader's patch variable, or a per-vertex one: an array of the patch's vertices. */
	if (type_node->patch)
		check_patch(shader, type_node, variable->line);
	arrayed = check_tess_arrayed(shader, type_node->storage, type_node->patch);
	if (global && arrayed)
		type = check_tess_array(shader, type, type_node->storage, variable->line);

	/* The name. */
	check_reserved_name(shader, variable->name, variable->line);

	/* Where it lives, as its qualifiers say (and whether they are allowed). */
	where = check_where(shader, type_node, type, global, variable->line);
	check_float_precision(shader, type_node->precision, type, variable->line);

	/* The symbol. */
	symbol = glsl_declare(shader, variable->name, GLSL_SYMBOL_VARIABLE, variable->line);
	if (symbol->kind != GLSL_SYMBOL_VARIABLE || symbol->type != NULL)
		return;
	symbol->type = type;
	symbol->storage = type_node->storage;
	symbol->interpolation = type_node->interpolation;
	symbol->precision = type_node->precision;
	symbol->centroid = type_node->centroid;
	symbol->invariant = type_node->invariant;
	symbol->patch = type_node->patch;
	symbol->where = where;
	variable->symbol = symbol;
	if (global)
		glsl_add_global(shader, symbol);

	/*
	 * An image uniform: its type of its format, its memory qualifiers, its
	 * unit (0 without a binding; an array's first element's, the others'
	 * the next ones).
	 */
	if (where == GLSL_VAR_IMAGE) {
		/* An array's elements are of the format. */
		if (type->kind == GLSL_KIND_ARRAY) {
			element = glsl_type_image_format(&shader->arena, type->element, type_node->image_format);
			symbol->type = glsl_type_array(&shader->arena, element, type->length);
		} else {
			symbol->type = glsl_type_image_format(&shader->arena, type, type_node->image_format);
		}
		symbol->memory = type_node->memory;
		symbol->layout_binding = 0U;
		if ((type_node->layout & GLSL_LAYOUT_BINDING) != 0U)
			symbol->layout_binding = type_node->binding;
	}

	/* A location a layout gave. */
	if ((type_node->layout & GLSL_LAYOUT_LOCATION) != 0U)
		check_location(shader, type_node, symbol, variable->line);

	/* An index a layout gave. */
	if ((type_node->layout & GLSL_LAYOUT_INDEX) != 0U)
		check_output_index(shader, type_node, symbol, variable->line);

	/* A geometry shader output's stream: one a layout gave, else the default one. */
	symbol->stream = 0U;
	if (shader->stage == GLSL_STAGE_GEOMETRY && where == GLSL_VAR_OUTPUT)
		symbol->stream = shader->default_stream;
	if ((type_node->layout & GLSL_LAYOUT_STREAM) != 0U) {
		status = check_stream(shader, type_node, variable->line);
		if (status == 0)
			symbol->stream = type_node->stream;
	}

	/* The initializer, which also sizes an unsized array. */
	check_initializer(shader, symbol, variable, global);

	/* An array still unsized is an error. */
	if (symbol->type->kind == GLSL_KIND_ARRAY && symbol->type->length == 0U) {
		glsl_error(shader, variable->line, "array '%s' has no size", variable->name);
		symbol->type = glsl_type_error();
	}
}

/*
 * Returns where a variable lives (GLSL_VAR_*) from its qualifiers, and
 * reports qualifiers the version, the stage or the scope does not allow.
 */
static unsigned
check_where(
	struct glsl_shader *shader,
	struct glsl_node *type_node,
	const struct glsl_type *type,
	int global,
	unsigned line)
{
	const struct glsl_type *element;
	unsigned storage;
	unsigned vertex;
	unsigned fragment;
	int has_sampler;
	int allowed;

	/* Samplers are uniforms (or parameters). */
	storage = type_node->storage;
	vertex = (shader->stage == GLSL_STAGE_VERTEX);
	fragment = (shader->stage == GLSL_STAGE_FRAGMENT);
	has_sampler = glsl_type_contains_sampler(type);
	if (has_sampler && storage != GLSL_STORAGE_UNIFORM)
		glsl_error(shader, line, "samplers must be uniforms");

	/* Interpolation and invariance are for varyings. */
	if (type_node->interpolation != GLSL_INTERP_NONE || type_node->centroid) {
		if (storage != GLSL_STORAGE_VARYING && storage != GLSL_STORAGE_IN && storage != GLSL_STORAGE_OUT)
			glsl_error(shader, line, "interpolation qualifiers apply to inputs and outputs");
	}

	/* A local variable is plain or const. */
	if (!global) {
		if (storage != GLSL_STORAGE_NONE && storage != GLSL_STORAGE_CONST) {
			glsl_error(shader, line, "local variables can only be const");
			return GLSL_VAR_LOCAL;
		}

		/* A const local is a constant; any other a local variable. */
		if (storage == GLSL_STORAGE_CONST)
			return GLSL_VAR_CONST;
		return GLSL_VAR_LOCAL;
	}

	/* An image uniform has its own memory qualifiers, binding and format (GLSL ES 3.10). */
	element = type;
	if (element->kind == GLSL_KIND_ARRAY)
		element = element->element;
	if (element->kind == GLSL_KIND_SAMPLER && element->image && storage == GLSL_STORAGE_UNIFORM)
		return check_image_where(shader, type_node, type, line);
	if ((type_node->layout & GLSL_LAYOUT_FORMAT) != 0U)
		glsl_error(shader, line, "format layout qualifiers apply to image uniforms");

	/* Memory qualifiers and a binding are for storage blocks (ws101-p008). */
	if (type_node->memory != 0U)
		glsl_error(shader, line, "memory qualifiers apply to buffer blocks");
	if ((type_node->layout & GLSL_LAYOUT_BINDING) != 0U)
		glsl_error(shader, line, "layout(binding) is supported on buffer blocks only");

	/* A compute shader's shared variable (ws101-p008); a compute shader has no inputs or outputs of its own. */
	if (storage == GLSL_STORAGE_SHARED) {
		if (shader->stage != GLSL_STAGE_COMPUTE)
			glsl_error(shader, line, "shared variables exist only in compute shaders");
		if (glsl_type_contains_sampler(type))
			glsl_error(shader, line, "a shared variable cannot hold a sampler");
		return GLSL_VAR_SHARED;
	}
	if (storage == GLSL_STORAGE_BUFFER) {
		glsl_error(shader, line, "buffer variables must be members of a buffer block");
		return GLSL_VAR_GLOBAL;
	}
	if (shader->stage == GLSL_STAGE_COMPUTE && (storage == GLSL_STORAGE_IN || storage == GLSL_STORAGE_OUT)) {
		glsl_error(shader, line, "a compute shader has no inputs or outputs");
		return GLSL_VAR_GLOBAL;
	}

	/* The storage qualifiers at global scope. */
	switch (storage) {
	case GLSL_STORAGE_NONE:
		return GLSL_VAR_GLOBAL;
	case GLSL_STORAGE_CONST:
		return GLSL_VAR_CONST;
	case GLSL_STORAGE_UNIFORM:
		return GLSL_VAR_UNIFORM;
	case GLSL_STORAGE_ATTRIBUTE:
		if (!vertex)
			glsl_error(shader, line, "attributes exist only in vertex shaders");
		if (type->kind == GLSL_KIND_ARRAY || type->kind == GLSL_KIND_STRUCT || type->base != GLSL_BASE_FLOAT)
			glsl_error(shader, line, "attributes must be float scalars, vectors or matrices");
		return GLSL_VAR_INPUT;
	case GLSL_STORAGE_VARYING:
		if (type->kind == GLSL_KIND_STRUCT || (type->kind != GLSL_KIND_ARRAY && type->base != GLSL_BASE_FLOAT))
			glsl_error(shader, line, "varyings must be float scalars, vectors, matrices or arrays of them");
		if (!vertex && !fragment)
			glsl_error(shader, line, "a geometry shader's inputs and outputs are declared with in and out");
		if (vertex)
			return GLSL_VAR_OUTPUT;
		return GLSL_VAR_INPUT;
	case GLSL_STORAGE_IN:
	case GLSL_STORAGE_OUT:
		break;
	default:
		glsl_error(shader, line, "inout is only for parameters");
		return GLSL_VAR_GLOBAL;
	}

	/* in and out at global scope came with GLSL 1.30 and OpenGL ES 3.00. */
	allowed = glsl_since(shader, GLSL_VERSION_130, GLSL_VERSION_ES300);
	if (!allowed) {
		glsl_error(shader, line, "global in and out need GLSL 1.30 (use attribute and varying)");
		return GLSL_VAR_GLOBAL;
	}

	/* OpenGL ES 3.00: no arrays into a vertex shader; no matrices, bools or doubles out of a fragment shader. */
	if (shader->es && vertex && storage == GLSL_STORAGE_IN && type->kind == GLSL_KIND_ARRAY)
		glsl_error(shader, line, "vertex shader inputs cannot be arrays");
	element = type;
	if (element->kind == GLSL_KIND_ARRAY)
		element = element->element;
	if (fragment && storage == GLSL_STORAGE_OUT &&
	    (type->kind == GLSL_KIND_MATRIX ||
	     type->base == GLSL_BASE_BOOL ||
	     element->base == GLSL_BASE_DOUBLE))
		glsl_error(shader, line, "fragment shader outputs must be float, int or uint scalars, vectors or arrays of them");

	/* Doubles go into a vertex shader from GLSL 4.10 (GL_ARB_vertex_attrib_64bit). */
	if (vertex && storage == GLSL_STORAGE_IN && element->base == GLSL_BASE_DOUBLE && shader->version < GLSL_VERSION_410)
		glsl_error(shader, line, "double vertex shader inputs need GLSL 4.10");

	/* Structs and bools do not cross stages; integers do, flat, into the fragment shader. */
	if (type->kind == GLSL_KIND_STRUCT || (type->kind != GLSL_KIND_ARRAY && type->base == GLSL_BASE_BOOL))
		glsl_error(shader, line, "inputs and outputs cannot be structs or bools");
	if (fragment && storage == GLSL_STORAGE_IN && type->kind != GLSL_KIND_ARRAY) {
		if (type->base != GLSL_BASE_FLOAT && type_node->interpolation != GLSL_INTERP_FLAT)
			check_flat_error(shader, type, line);
	}

	/* An array of doubles into a fragment shader is flat as well. */
	if (fragment && storage == GLSL_STORAGE_IN && type->kind == GLSL_KIND_ARRAY) {
		if (element->base == GLSL_BASE_DOUBLE && type_node->interpolation != GLSL_INTERP_FLAT)
			check_flat_error(shader, element, line);
	}

	/* An input or an output. */
	if (storage == GLSL_STORAGE_IN)
		return GLSL_VAR_INPUT;
	return GLSL_VAR_OUTPUT;
}

/* Reports a float declaration of an OpenGL ES fragment shader without a precision where none is the default. */
static void
check_float_precision(
	struct glsl_shader *shader,
	unsigned precision,
	const struct glsl_type *type,
	unsigned line)
{
	/* Only OpenGL ES fragment shaders lack a default. */
	if (!shader->es || shader->stage != GLSL_STAGE_FRAGMENT)
		return;
	if (precision != GLSL_PRECISION_NONE || shader->scope->float_precision != GLSL_PRECISION_NONE)
		return;

	/* A float, float vector or matrix, or an array of one. */
	if (type->kind == GLSL_KIND_ARRAY)
		type = type->element;
	if (type->kind != GLSL_KIND_SCALAR && type->kind != GLSL_KIND_VECTOR && type->kind != GLSL_KIND_MATRIX)
		return;
	if (type->base != GLSL_BASE_FLOAT)
		return;

	/* Such a declaration needs a precision. */
	glsl_error(shader, line, "no precision specified for float (add 'precision mediump float;')");
}

/* Checks a variable's initializer against its qualifiers and type. */
static void
check_initializer(
	struct glsl_shader *shader,
	struct glsl_symbol *symbol,
	struct glsl_node *variable,
	int global)
{
	struct glsl_node *initializer;
	const struct glsl_type *type;
	int same;

	/* A const variable must have one. */
	initializer = variable->child[1];
	if (initializer == NULL) {
		if (symbol->where == GLSL_VAR_CONST)
			glsl_error(shader, variable->line, "const variable '%s' needs an initializer", symbol->name);
		return;
	}

	/* The expression. */
	type = check_expression(shader, initializer);
	if (type->kind == GLSL_KIND_ERROR || symbol->type->kind == GLSL_KIND_ERROR)
		return;

	/* Inputs and outputs have none; uniforms from desktop 1.20. */
	if (symbol->where == GLSL_VAR_INPUT || symbol->where == GLSL_VAR_OUTPUT) {
		glsl_error(shader, variable->line, "inputs and outputs cannot be initialized");
		return;
	}

	/* Nor have shared variables (ws101-p008). */
	if (symbol->where == GLSL_VAR_SHARED) {
		glsl_error(shader, variable->line, "shared variables cannot be initialized");
		return;
	}

	/* Uniform initializers came with desktop GLSL 1.20. */
	if (symbol->where == GLSL_VAR_UNIFORM && (shader->es || shader->version < GLSL_VERSION_120)) {
		glsl_error(shader, variable->line, "uniform initializers need GLSL 1.20");
		return;
	}

	/* An unsized array takes the initializer's size. */
	if (symbol->type->kind == GLSL_KIND_ARRAY && symbol->type->length == 0U && type->kind == GLSL_KIND_ARRAY) {
		same = glsl_type_equal(symbol->type->element, type->element);
		if (same)
			symbol->type = type;
	}

	/* The value converted to the variable's type. */
	initializer = check_convert(shader, initializer, symbol->type);
	if (initializer == NULL) {
		check_type_error(shader, variable->line, "cannot initialize %s with %s", symbol->type, type);
		return;
	}

	/* The converted initializer replaces the original. */
	variable->child[1] = initializer;

	/* A const variable, a global and a uniform take a constant value. */
	if (symbol->where == GLSL_VAR_CONST || global) {
		if (initializer->constant == NULL) {
			glsl_error(shader, variable->line, "the initializer of '%s' must be a constant expression", symbol->name);
			return;
		}

		/* The constant is the const's value, or a global's or uniform's first value. */
		if (symbol->where == GLSL_VAR_CONST) {
			symbol->constant = initializer->constant;
		} else {
			symbol->initial = initializer->constant;
		}
	}
}

/* Checks a function prototype or definition. */
static void
check_function(
	struct glsl_shader *shader,
	struct glsl_node *node,
	struct check_breakables *breakables)
{
	struct glsl_function *function;
	unsigned index;
	unsigned count;

	/* The function (the overload of its signature); a subroutine is defined where it is declared. */
	function = check_prototype(shader, node);
	if (function != NULL && node->child[2] == NULL && (node->flags & GLSL_NODE_SUBROUTINE) != 0U)
		glsl_error(shader, node->line, "subroutine '%s' needs its body", node->name);
	if (function == NULL || node->child[2] == NULL)
		return;

	/* One definition. */
	if (function->body != NULL) {
		glsl_error(shader, node->line, "function '%s' is already defined", node->name);
		return;
	}

	/* A subroutine of the types it lists (GLSL 4.00), with its index. */
	if ((node->flags & GLSL_NODE_SUBROUTINE) != 0U)
		check_subroutine(shader, node, function);

	/* The definition's body. */
	function->body = node->child[2];
	node->function = function;

	/* The body shares a scope with the parameters. */
	check_push_scope(shader);
	count = function->parameter_count;
	for (index = 0U; index < count; index++) {
		if (function->parameters[index]->name == NULL)
			continue;
		check_add_symbol(shader, function->parameters[index], node->line);
	}

	/* The body's statements. */
	shader->current = function;
	check_block(shader, function->body, 0, breakables);
	shader->current = NULL;
	check_pop_scope(shader);

	/* Whether it returns other than at its end. */
	check_early_return(function);
}

/* Checks a function's return type and parameters, and finds or adds its overload. */
static struct glsl_function *
check_prototype(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	struct glsl_symbol *symbol;
	struct glsl_function *function;
	struct glsl_function *existing;
	struct glsl_symbol **parameters;
	struct glsl_node *parameter;
	const struct glsl_type *return_type;
	const struct glsl_builtin *builtin;
	unsigned count;
	unsigned index;
	int same;
	int differs;

	/* The return type. */
	return_type = check_type(shader, node->child[0]);
	check_float_precision(shader, node->child[0]->precision, return_type, node->line);
	if (node->child[0]->storage != GLSL_STORAGE_NONE && node->child[0]->storage != GLSL_STORAGE_CONST)
		glsl_error(shader, node->line, "a return type cannot have a storage qualifier");
	check_reserved_name(shader, node->name, node->line);

	/* OpenGL ES does not let a shader redefine a built-in function. */
	builtin = glsl_builtin_first(node->name);
	if (builtin != NULL && shader->es) {
		glsl_error(shader, node->line, "cannot redefine built-in function '%s'", node->name);
		return NULL;
	}

	/* The parameters. */
	count = 0U;
	for (parameter = node->child[1]; parameter != NULL; parameter = parameter->next)
		count++;
	parameters = glsl_alloc(&shader->arena, (count + 1U) * sizeof(*parameters));
	index = 0U;
	for (parameter = node->child[1]; parameter != NULL; parameter = parameter->next) {
		parameters[index] = check_parameter(shader, parameter);
		index++;
	}

	/* The name's symbol: a function (a variable or type of the name is an error). */
	symbol = check_lookup_local(shader, node->name);
	if (symbol != NULL && symbol->kind != GLSL_SYMBOL_FUNCTION) {
		glsl_error(shader, node->line, "'%s' is already declared as something else", node->name);
		return NULL;
	}

	/* A new name becomes a function. */
	if (symbol == NULL) {
		symbol = glsl_declare(shader, node->name, GLSL_SYMBOL_FUNCTION, node->line);
	}

	/* The overload of these parameter types, when it was declared before. */
	for (existing = symbol->functions; existing != NULL; existing = existing->next) {
		same = check_same_parameters(existing, parameters, count);
		if (!same)
			continue;

		/* Its return type and qualifiers must be the same. */
		same = glsl_type_equal(existing->return_type, return_type);
		if (!same)
			glsl_error(shader, node->line, "function '%s' redeclared with another return type", node->name);
		for (index = 0U; index < count; index++) {
			if (existing->parameters[index]->storage != parameters[index]->storage)
				glsl_error(shader, node->line, "function '%s' redeclared with other parameter qualifiers", node->name);
			existing->parameters[index]->name = parameters[index]->name;
		}

		/* The definition's parameters are the ones used. */
		if (node->child[2] != NULL)
			existing->parameters = parameters;
		return existing;
	}

	/* A new overload. */
	function = glsl_alloc(&shader->arena, sizeof(*function));
	function->name = node->name;
	function->return_type = return_type;
	function->line = node->line;
	function->parameters = parameters;
	function->parameter_count = count;
	function->next = symbol->functions;
	symbol->functions = function;

	/* main is void main(). */
	differs = strcmp(node->name, "main");
	if (differs == 0 && (count != 0U || return_type->kind != GLSL_KIND_VOID))
		glsl_error(shader, node->line, "main must be 'void main()'");

	/* Succeeded: the new overload. */
	return function;
}

/* Checks one parameter and makes its symbol. */
static struct glsl_symbol *
check_parameter(
	struct glsl_shader *shader,
	struct glsl_node *parameter)
{
	struct glsl_symbol *symbol;
	struct glsl_node *type_node;
	const struct glsl_type *type;
	unsigned storage;
	int has_sampler;

	/* The type, and an array of it. */
	type_node = parameter->child[0];
	type = check_type(shader, type_node);
	if ((parameter->flags & GLSL_NODE_ARRAY) != 0U)
		type = check_array(shader, type, parameter, parameter->child[1], parameter->line);
	check_float_precision(shader, type_node->precision, type, parameter->line);
	if (type->kind == GLSL_KIND_VOID) {
		glsl_error(shader, parameter->line, "a parameter of type void");
		type = glsl_type_error();
	}

	/* in (the default), out or inout; const only with in. */
	storage = type_node->storage;
	if (storage == GLSL_STORAGE_NONE || (type_node->flags & GLSL_NODE_CONST_PARAM) != 0U)
		storage = GLSL_STORAGE_IN;
	if (storage == GLSL_STORAGE_CONST) {
		storage = GLSL_STORAGE_IN;
		parameter->flags |= GLSL_NODE_CONST_PARAM;
	}

	/* Nothing else is a parameter's qualifier. */
	if (storage != GLSL_STORAGE_IN && storage != GLSL_STORAGE_OUT && storage != GLSL_STORAGE_INOUT) {
		glsl_error(shader, parameter->line, "a parameter can only be in, out, inout or const");
		storage = GLSL_STORAGE_IN;
	}

	/* A sampler is only passed in. */
	has_sampler = glsl_type_contains_sampler(type);
	if (storage != GLSL_STORAGE_IN && has_sampler)
		glsl_error(shader, parameter->line, "samplers can only be in parameters");

	/* The symbol (declared in the body's scope by the definition). */
	symbol = glsl_alloc(&shader->arena, sizeof(*symbol));
	symbol->name = parameter->name;
	symbol->kind = GLSL_SYMBOL_VARIABLE;
	symbol->line = parameter->line;
	symbol->type = type;
	symbol->storage = storage;
	symbol->precision = type_node->precision;
	symbol->where = GLSL_VAR_PARAMETER;
	symbol->memory = type_node->memory;
	symbol->explicit_location = GLSL_NO_LOCATION;
	if (parameter->name != NULL)
		check_reserved_name(shader, parameter->name, parameter->line);

	/* Succeeded: the parameter. */
	parameter->symbol = symbol;
	return symbol;
}

/* Reports whether an overload has exactly these parameter types. */
static int
check_same_parameters(
	const struct glsl_function *function,
	struct glsl_symbol **parameters,
	unsigned count)
{
	unsigned index;
	int same;

	/* The same count. */
	if (function->parameter_count != count)
		return 0;

	/* The same types in order. */
	for (index = 0U; index < count; index++) {
		same = glsl_type_equal(function->parameters[index]->type, parameters[index]->type);
		if (!same)
			return 0;
	}

	/* Succeeded: the same signature. */
	return 1;
}

/* Marks a function whose body returns anywhere but as its last statement. */
static void
check_early_return(
	struct glsl_function *function)
{
	struct glsl_node *statement;
	struct glsl_node *stack[256];
	struct glsl_node *node;
	unsigned depth;
	unsigned index;

	/* Every node below the body, but the body's last statement itself. */
	depth = 0U;
	for (statement = function->body->child[0]; statement != NULL; statement = statement->next) {
		if (statement->kind == GLSL_N_RETURN && statement->next == NULL)
			continue;
		if (depth < 256U) {
			stack[depth] = statement;
			depth++;
		}
	}

	/* A return anywhere among them. */
	while (depth > 0U) {
		depth--;
		node = stack[depth];
		if (node->kind == GLSL_N_RETURN) {
			function->early_return = 1U;
			return;
		}

		/* The statements below it (expressions hold none). */
		for (index = 0U; index < 4U; index++) {
			for (statement = node->child[index]; statement != NULL; statement = statement->next) {
				if (statement->kind < GLSL_N_BLOCK || depth >= 256U)
					continue;
				stack[depth] = statement;
				depth++;
			}
		}
	}
}

/* Checks a statement. */
static void
check_statement(
	struct glsl_shader *shader,
	struct glsl_node *node,
	struct check_breakables *breakables)
{
	/* The kinds of statements. */
	switch (node->kind) {
	case GLSL_N_BLOCK:
		check_block(shader, node, 1, breakables);
		break;
	case GLSL_N_DECLARATION:
		check_declaration(shader, node, 0);
		break;
	case GLSL_N_PRECISION:
		check_precision(shader, node);
		break;
	case GLSL_N_EXPRESSION:
		(void)check_expression(shader, node->child[0]);
		break;
	case GLSL_N_IF:
		check_condition(shader, node->child[0]);
		shader->control_depth++;
		check_scoped(shader, node->child[1], breakables);
		if (node->child[2] != NULL)
			check_scoped(shader, node->child[2], breakables);
		shader->control_depth--;
		break;
	case GLSL_N_FOR:
	case GLSL_N_WHILE:
	case GLSL_N_DO:
		shader->control_depth++;
		check_loop(shader, node, breakables);
		shader->control_depth--;
		break;
	case GLSL_N_SWITCH:
		shader->control_depth++;
		check_switch(shader, node, breakables);
		shader->control_depth--;
		break;
	case GLSL_N_CASE:
	case GLSL_N_DEFAULT:
		glsl_error(shader, node->line, "a case label outside a switch");
		break;
	case GLSL_N_RETURN:
		check_return(shader, node);
		break;
	case GLSL_N_BREAK:
	case GLSL_N_CONTINUE:
	case GLSL_N_DISCARD:
		check_jump(shader, node, breakables);
		break;
	case GLSL_N_EMPTY:
		break;
	default:
		glsl_error(shader, node->line, "unexpected statement");
		break;
	}
}

/* Checks the statements of a block, in a scope of its own when asked. */
static void
check_block(
	struct glsl_shader *shader,
	struct glsl_node *node,
	int new_scope,
	struct check_breakables *breakables)
{
	struct glsl_node *statement;

	/* The scope, the statements, and the scope's end. */
	if (new_scope)
		check_push_scope(shader);
	for (statement = node->child[0]; statement != NULL; statement = statement->next)
		check_statement(shader, statement, breakables);
	if (new_scope)
		check_pop_scope(shader);
}

/* Checks a statement that is a body of its own (of an if or a loop): it has a scope even without braces. */
static void
check_scoped(
	struct glsl_shader *shader,
	struct glsl_node *node,
	struct check_breakables *breakables)
{
	/* A block makes its own scope. */
	if (node->kind == GLSL_N_BLOCK) {
		check_block(shader, node, 1, breakables);
		return;
	}

	/* Any other statement gets one. */
	check_push_scope(shader);
	check_statement(shader, node, breakables);
	check_pop_scope(shader);
}

/* Checks that a condition is a bool scalar. */
static void
check_condition(
	struct glsl_shader *shader,
	struct glsl_node *condition)
{
	const struct glsl_type *type;

	/* The expression. */
	type = check_expression(shader, condition);
	if (type->kind == GLSL_KIND_ERROR)
		return;

	/* A bool. */
	if (type->kind != GLSL_KIND_SCALAR || type->base != GLSL_BASE_BOOL)
		check_type_error(shader, condition->line, "a condition must be a bool, not %s", type, NULL);
}

/* Checks a for, while or do loop. */
static void
check_loop(
	struct glsl_shader *shader,
	struct glsl_node *node,
	struct check_breakables *breakables)
{
	/* The loop is breakable while its body is checked. */
	if (breakables->count == CHECK_MAX_BREAKABLE)
		glsl_fatal(shader, node->line, "loops nested too deeply");
	check_push_scope(shader);

	/* The parts of each kind. */
	if (node->kind == GLSL_N_FOR) {
		if (node->child[0] != NULL)
			check_statement(shader, node->child[0], breakables);
		if (node->child[1] != NULL)
			check_condition(shader, node->child[1]);
		if (node->child[2] != NULL)
			(void)check_expression(shader, node->child[2]);
		breakables->kinds[breakables->count++] = CHECK_BREAKABLE_LOOP;
		check_scoped(shader, node->child[3], breakables);
	} else if (node->kind == GLSL_N_WHILE) {
		check_condition(shader, node->child[0]);
		breakables->kinds[breakables->count++] = CHECK_BREAKABLE_LOOP;
		check_scoped(shader, node->child[1], breakables);
	} else {
		breakables->kinds[breakables->count++] = CHECK_BREAKABLE_LOOP;
		check_scoped(shader, node->child[0], breakables);
		check_condition(shader, node->child[1]);
	}

	/* The loop ends. */
	breakables->count--;
	check_pop_scope(shader);
}

/* Checks a switch: an integer selector, constant case labels of its type, one default. */
static void
check_switch(
	struct glsl_shader *shader,
	struct glsl_node *node,
	struct check_breakables *breakables)
{
	const struct glsl_type *type;
	const struct glsl_type *label_type;
	struct glsl_node *statement;
	struct glsl_node *other;
	unsigned defaults;
	int first;
	int same;

	/* The selector: an int or a uint. */
	type = check_expression(shader, node->child[0]);
	if (type->kind != GLSL_KIND_ERROR) {
		if (type->kind != GLSL_KIND_SCALAR || (type->base != GLSL_BASE_INT && type->base != GLSL_BASE_UINT))
			check_type_error(shader, node->line, "a switch needs an integer, not %s", type, NULL);
	}

	/* The body, a scope whose first statement is a label. */
	if (breakables->count == CHECK_MAX_BREAKABLE)
		glsl_fatal(shader, node->line, "switches nested too deeply");
	breakables->kinds[breakables->count++] = CHECK_BREAKABLE_SWITCH;
	check_push_scope(shader);
	defaults = 0U;
	first = 1;
	for (statement = node->child[1]->child[0]; statement != NULL; statement = statement->next) {
		/* Statements other than labels, checked as usual (one must follow a label). */
		if (statement->kind != GLSL_N_CASE && statement->kind != GLSL_N_DEFAULT) {
			if (first)
				glsl_error(shader, statement->line, "a switch body must start with a case label");
			first = 0;
			check_statement(shader, statement, breakables);
			continue;
		}

		/* A label: nothing may come before it any more. */
		first = 0;

		/* One default. */
		if (statement->kind == GLSL_N_DEFAULT) {
			defaults++;
			if (defaults > 1U)
				glsl_error(shader, statement->line, "more than one default label");
			continue;
		}

		/* A case: a constant of the selector's type, not repeated. */
		label_type = check_expression(shader, statement->child[0]);
		if (label_type->kind == GLSL_KIND_ERROR || type->kind == GLSL_KIND_ERROR)
			continue;
		same = glsl_type_equal(label_type, type);
		if (statement->child[0]->constant == NULL || !same) {
			glsl_error(shader, statement->line, "a case label must be a constant of the selector's type");
			continue;
		}

		/* Not the same value as an earlier label. */
		for (other = node->child[1]->child[0]; other != statement; other = other->next) {
			if (other->kind != GLSL_N_CASE || other->child[0]->constant == NULL)
				continue;
			if (other->child[0]->constant->values[0].u == statement->child[0]->constant->values[0].u)
				glsl_error(shader, statement->line, "case label repeated");
		}
	}

	/* The switch ends. */
	check_pop_scope(shader);
	breakables->count--;
}

/* Checks break, continue and discard. */
static void
check_jump(
	struct glsl_shader *shader,
	struct glsl_node *node,
	struct check_breakables *breakables)
{
	/* discard only in fragment shaders. */
	if (node->kind == GLSL_N_DISCARD) {
		if (shader->stage != GLSL_STAGE_FRAGMENT)
			glsl_error(shader, node->line, "discard is only for fragment shaders");
		return;
	}

	/* break needs a loop or a switch. */
	if (node->kind == GLSL_N_BREAK) {
		if (breakables->count == 0U)
			glsl_error(shader, node->line, "break outside a loop or a switch");
		return;
	}

	/* continue needs a loop, and this compiler does not take one from inside a switch. */
	if (breakables->count == 0U) {
		glsl_error(shader, node->line, "continue outside a loop");
		return;
	}

	/* A continue from inside a switch would need a flag to leave it (not supported). */
	if (breakables->kinds[breakables->count - 1U] == CHECK_BREAKABLE_SWITCH)
		glsl_error(shader, node->line, "continue inside a switch is not supported");
}

/* Checks a return against the function's return type. */
static void
check_return(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	const struct glsl_type *type;
	const struct glsl_type *expected;
	struct glsl_node *value;

	/* Inside a function. */
	if (shader->current == NULL) {
		glsl_error(shader, node->line, "return outside a function");
		return;
	}

	/* A barrier() after a return of main is an error (ws101-p008). */
	if (strcmp(shader->current->name, "main") == 0)
		shader->returned = 1U;

	/* The function's return type. */
	expected = shader->current->return_type;

	/* A void function returns nothing. */
	if (node->child[0] == NULL) {
		if (expected->kind != GLSL_KIND_VOID)
			glsl_error(shader, node->line, "function '%s' must return a value", shader->current->name);
		return;
	}

	/* Otherwise a value of its type. */
	type = check_expression(shader, node->child[0]);
	if (type->kind == GLSL_KIND_ERROR || expected->kind == GLSL_KIND_ERROR)
		return;
	if (expected->kind == GLSL_KIND_VOID) {
		glsl_error(shader, node->line, "void function '%s' cannot return a value", shader->current->name);
		return;
	}

	/* The value converted to the return type. */
	value = check_convert(shader, node->child[0], expected);
	if (value == NULL) {
		check_type_error(shader, node->line, "cannot return %s from a function returning %s", type, expected);
		return;
	}

	/* The value converted. */
	node->child[0] = value;
}

/*
 * Checks an expression: sets its type, its constant value when it has
 * one, and whether it has side effects.  Returns the type.
 */
static const struct glsl_type *
check_expression(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	const struct glsl_type *type;

	/* The kind's rules. */
	type = check_expression_kind(shader, node);
	node->type = type;

	/* Its side effects include its operands'. */
	check_mark_side_effects(node);

	/* Its value, when it is constant. */
	if (type->kind != GLSL_KIND_ERROR && node->constant == NULL)
		node->constant = glsl_fold(shader, node);

	/* Succeeded: the type. */
	return type;
}

/* Checks an expression by its kind; returns its type. */
static const struct glsl_type *
check_expression_kind(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	const struct glsl_type *type;

	/* The kinds of expressions. */
	switch (node->kind) {
	case GLSL_N_INT:
		return glsl_type_scalar(GLSL_BASE_INT);
	case GLSL_N_UINT:
		return glsl_type_scalar(GLSL_BASE_UINT);
	case GLSL_N_FLOAT:
		return glsl_type_scalar(GLSL_BASE_FLOAT);
	case GLSL_N_DOUBLE:
		return glsl_type_scalar(GLSL_BASE_DOUBLE);
	case GLSL_N_BOOL:
		return glsl_type_scalar(GLSL_BASE_BOOL);
	case GLSL_N_IDENTIFIER:
		type = check_identifier(shader, node);
		return type;
	case GLSL_N_UNARY:
		type = check_unary(shader, node);
		return type;
	case GLSL_N_PREINC:
	case GLSL_N_PREDEC:
	case GLSL_N_POSTINC:
	case GLSL_N_POSTDEC:
		type = check_increment(shader, node);
		return type;
	case GLSL_N_BINARY:
		type = check_binary(shader, node);
		return type;
	case GLSL_N_ASSIGN:
		type = check_assign(shader, node);
		return type;
	case GLSL_N_TERNARY:
		type = check_ternary(shader, node);
		return type;
	case GLSL_N_COMMA:
		(void)check_expression(shader, node->child[0]);
		type = check_expression(shader, node->child[1]);
		return type;
	case GLSL_N_CALL:
		type = check_call(shader, node);
		return type;
	case GLSL_N_FIELD:
		type = check_field(shader, node);
		return type;
	case GLSL_N_INDEX:
		type = check_index(shader, node);
		return type;
	case GLSL_N_LENGTH:
		type = check_length(shader, node);
		return type;
	case GLSL_N_CONVERT:
		return node->type;
	default:
		break;
	}

	/* Anything else is not an expression. */
	glsl_error(shader, node->line, "not an expression");
	return glsl_type_error();
}

/* Checks a name used as a variable. */
static const struct glsl_type *
check_identifier(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	struct glsl_symbol *symbol;

	/* A declared variable. */
	symbol = check_lookup(shader, node->name);
	if (symbol == NULL) {
		glsl_error(shader, node->line, "'%s' is not declared", node->name);
		return glsl_type_error();
	}

	/* A variable, not a function or a type. */
	if (symbol->kind != GLSL_SYMBOL_VARIABLE) {
		glsl_error(shader, node->line, "'%s' is not a variable", node->name);
		return glsl_type_error();
	}

	/* Succeeded: the variable's type. */
	node->symbol = symbol;
	return symbol->type;
}

/* Checks a unary operator: + and - of numbers, ! of a bool, ~ of an integer. */
static const struct glsl_type *
check_unary(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	const struct glsl_type *type;
	int numeric;
	int allowed;

	/* The operand. */
	type = check_expression(shader, node->child[0]);
	if (type->kind == GLSL_KIND_ERROR)
		return type;

	/* ! of a bool scalar. */
	if (node->op == GLSL_P_BANG) {
		if (type->kind != GLSL_KIND_SCALAR || type->base != GLSL_BASE_BOOL) {
			check_type_error(shader, node->line, "'!' needs a bool, not %s", type, NULL);
			return glsl_type_error();
		}

		/* Succeeded: a bool. */
		return type;
	}

	/* ~ of an integer scalar or vector (1.30). */
	if (node->op == GLSL_P_TILDE) {
		allowed = glsl_since(shader, GLSL_VERSION_130, GLSL_VERSION_ES300);
		if (!allowed) {
			glsl_error(shader, node->line, "'~' needs GLSL 1.30");
			return glsl_type_error();
		}

		/* Of an integer. */
		if (type->kind == GLSL_KIND_MATRIX || (type->base != GLSL_BASE_INT && type->base != GLSL_BASE_UINT)) {
			check_type_error(shader, node->line, "'~' needs an integer, not %s", type, NULL);
			return glsl_type_error();
		}

		/* Succeeded: the integer's type. */
		return type;
	}

	/* + and - of a number. */
	numeric = glsl_type_numeric(type);
	if (!numeric) {
		check_type_error(shader, node->line, "unary '+' and '-' need a number, not %s", type, NULL);
		return glsl_type_error();
	}

	/* Succeeded: the operand's type. */
	return type;
}

/* Checks ++ and --: a numeric lvalue. */
static const struct glsl_type *
check_increment(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	const struct glsl_type *type;
	int numeric;
	int status;

	/* The operand. */
	type = check_expression(shader, node->child[0]);
	if (type->kind == GLSL_KIND_ERROR)
		return type;

	/* A number that can be written. */
	numeric = glsl_type_numeric(type);
	if (!numeric) {
		check_type_error(shader, node->line, "'++' and '--' need a number, not %s", type, NULL);
		return glsl_type_error();
	}

	/* And that can be written. */
	status = check_lvalue(shader, node->child[0], "'++' or '--'");
	if (status != 0)
		return glsl_type_error();

	/* Succeeded: the operand's type, with a side effect. */
	node->flags |= GLSL_NODE_SIDE_EFFECTS;
	return type;
}

/* Checks a binary operator. */
static const struct glsl_type *
check_binary(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	const struct glsl_type *left;
	const struct glsl_type *right;
	const struct glsl_type *type;

	/* The operands. */
	left = check_expression(shader, node->child[0]);
	right = check_expression(shader, node->child[1]);
	if (left->kind == GLSL_KIND_ERROR || right->kind == GLSL_KIND_ERROR)
		return glsl_type_error();

	/* The operator's rules. */
	type = check_operator(shader, node, node->op, &node->child[0], &node->child[1]);
	return type;
}

/*
 * Returns the type of a binary operator's result on two checked operands
 * (converting one to the other's base when the version allows), or the
 * error type after reporting why the operands do not fit.
 */
static const struct glsl_type *
check_operator(
	struct glsl_shader *shader,
	struct glsl_node *node,
	unsigned op,
	struct glsl_node **left,
	struct glsl_node **right)
{
	const struct glsl_type *left_type;
	const struct glsl_type *right_type;
	const struct glsl_type *type;
	unsigned version_130;
	int same;
	int has_sampler;
	int arrays;

	/* The logical operators take bool scalars. */
	version_130 = (unsigned)glsl_since(shader, GLSL_VERSION_130, GLSL_VERSION_ES300);
	if (op == GLSL_P_AND_AND || op == GLSL_P_OR_OR || op == GLSL_P_XOR_XOR) {
		left_type = (*left)->type;
		right_type = (*right)->type;
		if (left_type->kind != GLSL_KIND_SCALAR || left_type->base != GLSL_BASE_BOOL ||
		    right_type->kind != GLSL_KIND_SCALAR || right_type->base != GLSL_BASE_BOOL) {
			check_type_error(shader, node->line, "logical operators need bools, not %s and %s", left_type, right_type);
			return glsl_type_error();
		}

		/* Succeeded: a bool. */
		return left_type;
	}

	/* Shifts: integer operands of any base; the result is the left one's. */
	if (op == GLSL_P_SHL || op == GLSL_P_SHR) {
		left_type = (*left)->type;
		right_type = (*right)->type;
		if (!version_130) {
			glsl_error(shader, node->line, "shifts need GLSL 1.30");
			return glsl_type_error();
		}

		/* Of integers, a vector shifted by a scalar or a vector of its size. */
		if (left_type->kind == GLSL_KIND_MATRIX || right_type->kind == GLSL_KIND_MATRIX ||
		    (left_type->base != GLSL_BASE_INT && left_type->base != GLSL_BASE_UINT) ||
		    (right_type->base != GLSL_BASE_INT && right_type->base != GLSL_BASE_UINT) ||
		    (right_type->kind == GLSL_KIND_VECTOR && right_type->components != left_type->components)) {
			check_type_error(shader, node->line, "cannot shift %s by %s", left_type, right_type);
			return glsl_type_error();
		}

		/* Succeeded: the shifted operand's type. */
		return left_type;
	}

	/* The other operators want one base: an integer side may become float. */
	check_balance(shader, left, right);
	left_type = (*left)->type;
	right_type = (*right)->type;

	/* Equality: the same type, without samplers (and arrays only from 1.20). */
	if (op == GLSL_P_EQ || op == GLSL_P_NE) {
		same = glsl_type_equal(left_type, right_type);
		has_sampler = glsl_type_contains_sampler(left_type);
		if (!same || has_sampler || left_type->kind == GLSL_KIND_VOID) {
			check_type_error(shader, node->line, "cannot compare %s with %s", left_type, right_type);
			return glsl_type_error();
		}

		/* Arrays compare from 1.20. */
		arrays = glsl_since(shader, GLSL_VERSION_120, GLSL_VERSION_ES300);
		if (left_type->kind == GLSL_KIND_ARRAY && !arrays) {
			glsl_error(shader, node->line, "comparing arrays needs GLSL 1.20");
			return glsl_type_error();
		}

		/* Succeeded: a bool. */
		return glsl_type_scalar(GLSL_BASE_BOOL);
	}

	/* Relations: numeric scalars of one base. */
	if (op == GLSL_P_LT || op == GLSL_P_GT || op == GLSL_P_LE || op == GLSL_P_GE) {
		if (left_type->kind != GLSL_KIND_SCALAR || right_type->kind != GLSL_KIND_SCALAR ||
		    left_type->base == GLSL_BASE_BOOL || left_type->base != right_type->base) {
			check_type_error(shader, node->line, "cannot order %s and %s (use lessThan() and the like for vectors)", left_type, right_type);
			return glsl_type_error();
		}

		/* Succeeded: a bool. */
		return glsl_type_scalar(GLSL_BASE_BOOL);
	}

	/* %, &, | and ^ are integer operators of GLSL 1.30. */
	if (op == GLSL_P_PERCENT || op == GLSL_P_AMP || op == GLSL_P_BAR || op == GLSL_P_CARET) {
		if (!version_130) {
			glsl_error(shader, node->line, "'%%' and the bitwise operators need GLSL 1.30");
			return glsl_type_error();
		}

		/* Of integers. */
		if (left_type->base != GLSL_BASE_INT && left_type->base != GLSL_BASE_UINT) {
			check_type_error(shader, node->line, "integer operators need integers, not %s and %s", left_type, right_type);
			return glsl_type_error();
		}
	}

	/* Arithmetic on the shapes. */
	type = check_arithmetic(op, left_type, right_type);
	if (type == NULL) {
		check_type_error(shader, node->line, "the operator does not apply to %s and %s", left_type, right_type);
		return glsl_type_error();
	}

	/* Succeeded: the result's type. */
	return type;
}

/*
 * Returns the type of an arithmetic operator (+ - * / % & | ^) on two
 * types of one base, or NULL when the shapes do not fit.
 */
static const struct glsl_type *
check_arithmetic(
	unsigned op,
	const struct glsl_type *left,
	const struct glsl_type *right)
{
	/* Numbers of one base. */
	if (left->base != right->base || left->base == GLSL_BASE_BOOL || left->base == GLSL_BASE_NONE)
		return NULL;
	if (left->kind != GLSL_KIND_SCALAR && left->kind != GLSL_KIND_VECTOR && left->kind != GLSL_KIND_MATRIX)
		return NULL;
	if (right->kind != GLSL_KIND_SCALAR && right->kind != GLSL_KIND_VECTOR && right->kind != GLSL_KIND_MATRIX)
		return NULL;

	/* A scalar with anything: the other's shape. */
	if (left->kind == GLSL_KIND_SCALAR)
		return right;
	if (right->kind == GLSL_KIND_SCALAR)
		return left;

	/* Two vectors of one size. */
	if (left->kind == GLSL_KIND_VECTOR && right->kind == GLSL_KIND_VECTOR) {
		if (left->components != right->components)
			return NULL;
		return left;
	}

	/* Only * takes a matrix with a vector or a matrix of another shape. */
	if (op != GLSL_P_STAR) {
		if (left == right && left->kind == GLSL_KIND_MATRIX)
			return left;
		return NULL;
	}

	/* A vector times a matrix: a row vector; a matrix times a vector: a column vector. */
	if (left->kind == GLSL_KIND_VECTOR) {
		if (left->components != right->components)
			return NULL;
		return glsl_type_vector(left->base, right->columns);
	}

	/* A matrix times a vector: a column vector. */
	if (right->kind == GLSL_KIND_VECTOR) {
		if (left->columns != right->components)
			return NULL;
		return glsl_type_vector(left->base, left->components);
	}

	/* A matrix times a matrix: the left's rows and the right's columns. */
	if (left->columns != right->components)
		return NULL;

	/* Succeeded: the product's type. */
	return glsl_type_matrix(left->base, right->columns, left->components);
}

/*
 * Converts one operand to the other's base when the version converts
 * implicitly: an integer to float, an integer or a float (a float matrix
 * too) to double, an int to uint.
 */
static void
check_balance(
	struct glsl_shader *shader,
	struct glsl_node **left,
	struct glsl_node **right)
{
	const struct glsl_type *left_type;
	const struct glsl_type *right_type;
	const struct glsl_type *target;
	struct glsl_node *converted;
	int floating;

	/* Two bases, one of them floating-point. */
	left_type = (*left)->type;
	right_type = (*right)->type;
	if (left_type->base == right_type->base)
		return;

	/* The left side becomes the right side's float or double. */
	floating = glsl_base_floating(right_type->base);
	if (floating && left_type->base != GLSL_BASE_DOUBLE) {
		target = glsl_type_with_base(left_type, right_type->base);
		if (target->kind == GLSL_KIND_ERROR)
			return;
		converted = check_convert(shader, *left, target);
		if (converted != NULL)
			*left = converted;
		return;
	}

	/* Or the right side becomes the left side's. */
	floating = glsl_base_floating(left_type->base);
	if (floating) {
		target = glsl_type_with_base(right_type, left_type->base);
		if (target->kind == GLSL_KIND_ERROR)
			return;
		converted = check_convert(shader, *right, target);
		if (converted != NULL)
			*right = converted;
		return;
	}

	/* Or, of an int and a uint, the int becomes a uint where the version converts it (4.00, GL_ARB_gpu_shader5). */
	if (left_type->base == GLSL_BASE_INT && right_type->base == GLSL_BASE_UINT) {
		target = glsl_type_with_base(left_type, GLSL_BASE_UINT);
		converted = check_convert(shader, *left, target);
		if (converted != NULL)
			*left = converted;
	} else if (left_type->base == GLSL_BASE_UINT && right_type->base == GLSL_BASE_INT) {
		target = glsl_type_with_base(right_type, GLSL_BASE_UINT);
		converted = check_convert(shader, *right, target);
		if (converted != NULL)
			*right = converted;
	}
}

/* Checks an assignment: a writable left side and a right side of its type (or of the operator's result). */
static const struct glsl_type *
check_assign(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	const struct glsl_type *left;
	const struct glsl_type *right;
	const struct glsl_type *type;
	const struct glsl_type *target;
	struct glsl_node *value;
	unsigned op;
	int status;
	int same;
	int has_sampler;
	int arrays;
	int floating;

	/* The two sides, the left one writable. */
	left = check_expression(shader, node->child[0]);
	right = check_expression(shader, node->child[1]);
	if (left->kind == GLSL_KIND_ERROR || right->kind == GLSL_KIND_ERROR)
		return glsl_type_error();
	status = check_lvalue(shader, node->child[0], "an assignment");
	if (status != 0)
		return glsl_type_error();
	node->flags |= GLSL_NODE_SIDE_EFFECTS;

	/* A plain assignment: the right side converted to the left's type (arrays from 1.20). */
	if (node->op == GLSL_P_ASSIGN) {
		arrays = glsl_since(shader, GLSL_VERSION_120, GLSL_VERSION_ES300);
		if (left->kind == GLSL_KIND_ARRAY && !arrays) {
			glsl_error(shader, node->line, "assigning arrays needs GLSL 1.20");
			return glsl_type_error();
		}

		/* Samplers cannot be assigned. */
		has_sampler = glsl_type_contains_sampler(left);
		if (has_sampler) {
			glsl_error(shader, node->line, "samplers cannot be assigned");
			return glsl_type_error();
		}

		/* The right side converted to the left side's type. */
		value = check_convert(shader, node->child[1], left);
		if (value == NULL) {
			check_type_error(shader, node->line, "cannot assign %s to %s", right, left);
			return glsl_type_error();
		}

		/* Succeeded: the left side's type. */
		node->child[1] = value;
		return left;
	}

	/* A compound assignment: the operator's result must be of the left side's type. */
	switch (node->op) {
	case GLSL_P_ADD_ASSIGN:
		op = GLSL_P_PLUS;
		break;
	case GLSL_P_SUB_ASSIGN:
		op = GLSL_P_MINUS;
		break;
	case GLSL_P_MUL_ASSIGN:
		op = GLSL_P_STAR;
		break;
	case GLSL_P_DIV_ASSIGN:
		op = GLSL_P_SLASH;
		break;
	case GLSL_P_MOD_ASSIGN:
		op = GLSL_P_PERCENT;
		break;
	case GLSL_P_SHL_ASSIGN:
		op = GLSL_P_SHL;
		break;
	case GLSL_P_SHR_ASSIGN:
		op = GLSL_P_SHR;
		break;
	case GLSL_P_AND_ASSIGN:
		op = GLSL_P_AMP;
		break;
	case GLSL_P_OR_ASSIGN:
		op = GLSL_P_BAR;
		break;
	default:
		op = GLSL_P_CARET;
		break;
	}

	/* The operator on the two sides (only the right one may be converted, to the left one's float or double). */
	value = node->child[1];
	floating = glsl_base_floating(left->base);
	if (value->type->base != left->base && floating) {
		target = glsl_type_with_base(value->type, left->base);
		value = NULL;
		if (target->kind != GLSL_KIND_ERROR)
			value = check_convert(shader, node->child[1], target);
		if (value == NULL)
			value = node->child[1];
	}

	/* The operator's result type. */
	node->child[1] = value;
	type = check_operator(shader, node, op, &node->child[0], &node->child[1]);
	if (type->kind == GLSL_KIND_ERROR)
		return type;
	same = glsl_type_equal(type, left);
	if (!same) {
		check_type_error(shader, node->line, "the result %s cannot be assigned to %s", type, left);
		return glsl_type_error();
	}

	/* Succeeded: the left side's type. */
	return left;
}

/* Checks "condition ? a : b". */
static const struct glsl_type *
check_ternary(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	const struct glsl_type *second;
	const struct glsl_type *third;
	int same;
	int has_sampler;

	/* The condition and the two choices. */
	check_condition(shader, node->child[0]);
	second = check_expression(shader, node->child[1]);
	third = check_expression(shader, node->child[2]);
	if (second->kind == GLSL_KIND_ERROR || third->kind == GLSL_KIND_ERROR)
		return glsl_type_error();

	/* The choices of one type (an integer one may become float). */
	check_balance(shader, &node->child[1], &node->child[2]);
	second = node->child[1]->type;
	third = node->child[2]->type;
	same = glsl_type_equal(second, third);
	if (!same) {
		check_type_error(shader, node->line, "the choices of '?:' differ: %s and %s", second, third);
		return glsl_type_error();
	}

	/* No arrays in OpenGL ES 1.00, no samplers anywhere. */
	if (second->kind == GLSL_KIND_ARRAY && shader->es && shader->version < GLSL_VERSION_ES300) {
		glsl_error(shader, node->line, "'?:' cannot choose arrays in OpenGL ES");
		return glsl_type_error();
	}

	/* Nor samplers or void. */
	has_sampler = glsl_type_contains_sampler(second);
	if (has_sampler || second->kind == GLSL_KIND_VOID) {
		check_type_error(shader, node->line, "'?:' cannot choose %s", second, NULL);
		return glsl_type_error();
	}

	/* Succeeded: the choices' type. */
	return second;
}

/* Checks a call: a constructor, a user function or a built-in. */
static const struct glsl_type *
check_call(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	const struct glsl_type *types[32];
	const struct glsl_type *type;
	struct glsl_symbol *symbol;
	const struct glsl_builtin *builtin;
	unsigned count;
	int error;

	/* A constructor. */
	if ((node->flags & GLSL_NODE_CONSTRUCTOR) != 0U) {
		type = check_constructor(shader, node);
		return type;
	}

	/* The arguments. */
	count = check_arguments(shader, node, types, 32U, &error);
	if (error)
		return glsl_type_error();

	/* A subroutine uniform runs the subroutine its hidden uniform selects (GLSL 4.00). */
	symbol = check_lookup(shader, node->name);
	if (symbol != NULL && symbol->kind == GLSL_SYMBOL_SUBROUTINE_UNIFORM) {
		type = check_subroutine_call(shader, node, symbol, types, count);
		return type;
	}

	/* "NAME[i](...)" calls through an array of subroutine uniforms only. */
	if (node->child[2] != NULL) {
		glsl_error(shader, node->line, "'%s' is not an array of subroutine uniforms", node->name);
		return glsl_type_error();
	}

	/* A subroutine type is not callable. */
	if (symbol != NULL && symbol->kind == GLSL_SYMBOL_SUBROUTINE) {
		glsl_error(shader, node->line, "'%s' is a subroutine type, not a function", node->name);
		return glsl_type_error();
	}

	/* A user function of the name hides the built-ins; a variable of the name is not callable. */
	if (symbol != NULL && symbol->kind == GLSL_SYMBOL_FUNCTION) {
		type = check_user_call(shader, node, symbol, types, count);
		return type;
	}

	/* A type's name is not callable. */
	if (symbol != NULL && symbol->kind == GLSL_SYMBOL_TYPE) {
		glsl_error(shader, node->line, "'%s' is a type, not a function", node->name);
		return glsl_type_error();
	}

	/* A built-in. */
	builtin = glsl_builtin_first(node->name);
	if (builtin != NULL) {
		type = check_builtin_call(shader, node, types, count);
		return type;
	}

	/* Nothing of the name. */
	glsl_error(shader, node->line, "function '%s' is not declared", node->name);
	return glsl_type_error();
}

/* Checks a call's arguments; returns how many, with their types, and whether any had an error. */
static unsigned
check_arguments(
	struct glsl_shader *shader,
	struct glsl_node *node,
	const struct glsl_type **types,
	unsigned capacity,
	int *error)
{
	struct glsl_node *argument;
	const struct glsl_type *type;
	unsigned count;

	/* Each argument. */
	*error = 0;
	count = 0U;
	for (argument = node->child[1]; argument != NULL; argument = argument->next) {
		type = check_expression(shader, argument);
		if (type->kind == GLSL_KIND_ERROR)
			*error = 1;
		if (type->kind == GLSL_KIND_VOID) {
			glsl_error(shader, argument->line, "an argument of type void");
			*error = 1;
		}

		/* Room for its type. */
		if (count == capacity) {
			glsl_error(shader, node->line, "too many arguments");
			*error = 1;
			break;
		}

		/* The type is kept. */
		types[count] = type;
		count++;
	}

	/* Succeeded: the count. */
	return count;
}

/* Checks a constructor call. */
static const struct glsl_type *
check_constructor(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	const struct glsl_type *types[64];
	const struct glsl_type *type;
	unsigned count;
	int error;
	int status;
	int has_sampler;
	int arrays;

	/* The type constructed, and the arguments. */
	type = check_type(shader, node->child[0]);
	count = check_arguments(shader, node, types, 64U, &error);
	if (error || type->kind == GLSL_KIND_ERROR)
		return glsl_type_error();
	if (count == 0U) {
		glsl_error(shader, node->line, "a constructor needs arguments");
		return glsl_type_error();
	}

	/* Samplers and void cannot be constructed. */
	has_sampler = glsl_type_contains_sampler(type);
	if (type->kind == GLSL_KIND_VOID || has_sampler) {
		check_type_error(shader, node->line, "cannot construct %s", type, NULL);
		return glsl_type_error();
	}

	/* An array of a size given by the arguments ("float[](...)"), from 1.20. */
	if (type->kind == GLSL_KIND_ARRAY) {
		arrays = glsl_since(shader, GLSL_VERSION_120, GLSL_VERSION_ES300);
		if (!arrays) {
			glsl_error(shader, node->line, "array constructors need GLSL 1.20");
			return glsl_type_error();
		}

		/* An unsized array takes the count of the arguments. */
		if (type->length == 0U)
			type = glsl_type_array(&shader->arena, type->element, count);
	}

	/* Structs and arrays take one argument per member or element. */
	node->type = type;
	if (type->kind == GLSL_KIND_STRUCT || type->kind == GLSL_KIND_ARRAY) {
		status = check_construct_aggregate(shader, node, type);
		if (status != 0)
			return glsl_type_error();
		return type;
	}

	/* Scalars, vectors and matrices take components. */
	status = check_construct_components(shader, node, type);
	if (status != 0)
		return glsl_type_error();

	/* Succeeded: the constructed type. */
	return type;
}

/* Checks a scalar, vector or matrix constructor's arguments: enough components, none left unused. */
static int
check_construct_components(
	struct glsl_shader *shader,
	struct glsl_node *node,
	const struct glsl_type *type)
{
	struct glsl_node *argument;
	const struct glsl_type *argument_type;
	unsigned needed;
	unsigned supplied;
	unsigned count;
	int has_matrix;

	/* Every argument a scalar, vector or matrix. */
	count = 0U;
	has_matrix = 0;
	for (argument = node->child[1]; argument != NULL; argument = argument->next) {
		argument_type = argument->type;
		if (argument_type->kind != GLSL_KIND_SCALAR && argument_type->kind != GLSL_KIND_VECTOR && argument_type->kind != GLSL_KIND_MATRIX) {
			check_type_error(shader, argument->line, "cannot construct %s from %s", type, argument_type);
			return -1;
		}

		/* A matrix among the arguments matters below. */
		if (argument_type->kind == GLSL_KIND_MATRIX)
			has_matrix = 1;
		count++;
	}

	/* One argument: a scalar fills a vector or a matrix's diagonal, and a matrix resizes into a matrix. */
	if (count == 1U) {
		argument_type = node->child[1]->type;
		if (argument_type->kind == GLSL_KIND_SCALAR || type->kind == GLSL_KIND_SCALAR)
			return 0;
		if (type->kind == GLSL_KIND_MATRIX && argument_type->kind == GLSL_KIND_MATRIX)
			return 0;
	}

	/* A matrix from a matrix takes that one argument only. */
	if (type->kind == GLSL_KIND_MATRIX && has_matrix) {
		glsl_error(shader, node->line, "a matrix constructed from a matrix takes only that matrix");
		return -1;
	}

	/* Components enough, each argument contributing. */
	needed = glsl_type_scalars(type);
	supplied = 0U;
	for (argument = node->child[1]; argument != NULL; argument = argument->next) {
		if (supplied >= needed) {
			glsl_error(shader, argument->line, "too many arguments to the %s constructor", type->name);
			return -1;
		}

		/* The components so far. */
		supplied += glsl_type_scalars(argument->type);
	}

	/* Enough components in all. */
	if (supplied < needed) {
		glsl_error(shader, node->line, "not enough components for the %s constructor", type->name);
		return -1;
	}

	/* Succeeded: the arguments fit. */
	return 0;
}

/* Checks a struct or array constructor's arguments: one of each member's or the element's type. */
static int
check_construct_aggregate(
	struct glsl_shader *shader,
	struct glsl_node *node,
	const struct glsl_type *type)
{
	struct glsl_node **link;
	struct glsl_node *converted;
	const struct glsl_type *expected;
	unsigned count;
	unsigned index;

	/* The count. */
	count = 0U;
	for (converted = node->child[1]; converted != NULL; converted = converted->next)
		count++;
	if (type->kind == GLSL_KIND_STRUCT && count != type->field_count) {
		glsl_error(shader, node->line, "struct %s has %u members, not %u", type->name, type->field_count, count);
		return -1;
	}

	/* An array takes one argument per element. */
	if (type->kind == GLSL_KIND_ARRAY && count != type->length) {
		glsl_error(shader, node->line, "the array has %u elements, not %u", type->length, count);
		return -1;
	}

	/* Each argument converted to its member's or element's type. */
	index = 0U;
	for (link = &node->child[1]; *link != NULL; link = &(*link)->next) {
		expected = type->element;
		if (type->kind == GLSL_KIND_STRUCT)
			expected = type->fields[index].type;
		converted = check_convert(shader, *link, expected);
		if (converted == NULL) {
			check_type_error(shader, (*link)->line, "the constructor needs %s, not %s", expected, (*link)->type);
			return -1;
		}

		/* The converted argument takes the original's place in the list. */
		*link = converted;
		index++;
	}

	/* Succeeded: the arguments fit. */
	return 0;
}

/* Checks a call of a user function: the overload, the out arguments, the conversions. */
static const struct glsl_type *
check_user_call(
	struct glsl_shader *shader,
	struct glsl_node *node,
	struct glsl_symbol *symbol,
	const struct glsl_type **types,
	unsigned count)
{
	const struct glsl_type *parameters[32];
	struct glsl_function *function;
	struct glsl_node *argument;
	unsigned index;
	int ambiguous;
	int status;

	/* The exact overload, else one with implicit conversions (desktop 1.20 on). */
	function = check_overload(shader, symbol->functions, types, count, 0, &ambiguous);
	if (function == NULL && !shader->es && shader->version >= GLSL_VERSION_120)
		function = check_overload(shader, symbol->functions, types, count, 1, &ambiguous);
	if (function == NULL) {
		if (ambiguous) {
			glsl_error(shader, node->line, "the call of '%s' is ambiguous", node->name);
		} else {
			glsl_error(shader, node->line, "no overload of '%s' takes these arguments", node->name);
		}

		/* The overload found. */
		return glsl_type_error();
	}

	/* out and inout arguments must be writable. */
	index = 0U;
	for (argument = node->child[1]; argument != NULL; argument = argument->next) {
		parameters[index] = function->parameters[index]->type;
		if (function->parameters[index]->storage != GLSL_STORAGE_IN) {
			status = check_lvalue(shader, argument, "an out argument");
			if (status != 0)
				return glsl_type_error();
		}

		/* The next argument. */
		index++;
	}

	/* The in arguments converted to their parameters' types. */
	check_convert_arguments(shader, node, parameters);

	/* Succeeded: the function's return type (a call may have any side effect). */
	node->function = function;
	node->flags |= GLSL_NODE_SIDE_EFFECTS;
	return function->return_type;
}

/*
 * Finds the overload that takes the arguments, exactly or with
 * conversions of in arguments.  Two that fit with conversions are
 * ambiguous, but where the doubles exist (GLSL 4.00) the one whose
 * conversions are better than every other fit's is taken.
 */
static struct glsl_function *
check_overload(
	struct glsl_shader *shader,
	struct glsl_function *functions,
	const struct glsl_type **types,
	unsigned count,
	int convert,
	int *ambiguous)
{
	struct glsl_function *function;
	struct glsl_function *other;
	struct glsl_function *found;
	unsigned mask;
	unsigned fits;
	int fit;
	int better;

	/* The overloads that fit. */
	*ambiguous = 0;
	found = NULL;
	fits = 0U;
	for (function = functions; function != NULL; function = function->next) {
		fit = check_overload_fits(shader, function, types, count, convert);
		if (!fit)
			continue;
		if (found == NULL)
			found = function;
		fits++;
	}

	/* None, or one. */
	if (fits <= 1U)
		return found;

	/* Without the doubles or GL_ARB_gpu_shader5 (4.00's rules), two fits are ambiguous. */
	mask = glsl_version_mask(shader);
	if ((mask & (GLSL_IN_FP64 | GLSL_IN_SHADER5)) == 0U) {
		*ambiguous = 1;
		return NULL;
	}

	/* With them, the fit better than every other fit. */
	for (function = functions; function != NULL; function = function->next) {
		fit = check_overload_fits(shader, function, types, count, convert);
		if (!fit)
			continue;
		better = 1;
		for (other = functions; other != NULL && better; other = other->next) {
			if (other == function)
				continue;
			fit = check_overload_fits(shader, other, types, count, convert);
			if (!fit)
				continue;
			better = check_better_overload(function, other, types, count);
		}

		/* The best one. */
		if (better)
			return function;
	}

	/* No fit is better than all the others. */
	*ambiguous = 1;
	return NULL;
}

/* Reports whether an overload takes the arguments: each of its parameter's type, or convertible to an in parameter's. */
static int
check_overload_fits(
	const struct glsl_shader *shader,
	const struct glsl_function *function,
	const struct glsl_type **types,
	unsigned count,
	int convert)
{
	unsigned index;
	int same;
	int convertible;

	/* The count. */
	if (function->parameter_count != count)
		return 0;

	/* Each argument. */
	for (index = 0U; index < count; index++) {
		same = glsl_type_equal(function->parameters[index]->type, types[index]);
		if (same)
			continue;
		if (!convert || function->parameters[index]->storage != GLSL_STORAGE_IN)
			return 0;
		convertible = check_can_convert(shader, types[index], function->parameters[index]->type);
		if (!convertible)
			return 0;
	}

	/* Succeeded: it fits. */
	return 1;
}

/* Checks a call of a built-in function. */
static const struct glsl_type *
check_builtin_call(
	struct glsl_shader *shader,
	struct glsl_node *node,
	const struct glsl_type **types,
	unsigned count)
{
	struct check_candidate candidates[CHECK_MAX_CANDIDATES];
	struct check_candidate *best;
	const struct glsl_type *parameters[8];
	const struct glsl_type *result;
	const struct glsl_builtin *builtin;
	int ambiguous;
	int matched;

	/* At most eight arguments. */
	if (count > 8U) {
		glsl_error(shader, node->line, "too many arguments to '%s'", node->name);
		return glsl_type_error();
	}

	/* An exact signature. */
	for (builtin = glsl_builtin_first(node->name); builtin != NULL; builtin = glsl_builtin_next(builtin)) {
		matched = glsl_builtin_match(shader, builtin, types, count, 0, parameters, &result);
		if (matched) {
			result = check_builtin_found(shader, node, builtin, parameters, result);
			return result;
		}
	}

	/* Else the one whose conversions are best (desktop 1.20 on). */
	best = NULL;
	ambiguous = 0;
	if (!shader->es && shader->version >= GLSL_VERSION_120)
		best = check_best_builtin(shader, node, types, count, candidates, &ambiguous);
	if (best != NULL) {
		result = check_builtin_found(shader, node, best->builtin, best->parameters, best->result);
		return result;
	}

	/* Several fit equally well. */
	if (ambiguous) {
		glsl_error(shader, node->line, "the call of built-in '%s' is ambiguous", node->name);
		return glsl_type_error();
	}

	/* No signature fits. */
	glsl_error(shader, node->line, "no overload of built-in '%s' takes these arguments in this shader", node->name);
	return glsl_type_error();
}

/*
 * Finds the built-in signature that fits a call's arguments with
 * conversions and whose conversions are better than every other fit's
 * (GLSL 4.00's rules; an earlier signature of the same parameters stands
 * for a later one).  Returns it, or NULL when none fits or several fit
 * equally well (ambiguous then 1).
 */
static struct check_candidate *
check_best_builtin(
	const struct glsl_shader *shader,
	struct glsl_node *node,
	const struct glsl_type **types,
	unsigned count,
	struct check_candidate *candidates,
	int *ambiguous)
{
	const struct glsl_builtin *builtin;
	unsigned found;
	unsigned index;
	unsigned other;
	int matched;
	int chosen;
	int better;

	/* The signatures that fit (the first ones, when there are very many). */
	found = 0U;
	for (builtin = glsl_builtin_first(node->name); builtin != NULL && found < CHECK_MAX_CANDIDATES; builtin = glsl_builtin_next(builtin)) {
		matched = glsl_builtin_match(shader, builtin, types, count, 1, candidates[found].parameters, &candidates[found].result);
		if (!matched)
			continue;
		candidates[found].builtin = builtin;
		found++;
	}

	/* The first fit better than each other one. */
	for (index = 0U; index < found; index++) {
		chosen = 1;
		for (other = 0U; other < found && chosen; other++) {
			if (other == index)
				continue;
			better = check_better_candidate(&candidates[index], &candidates[other], types, count);
			if (better == 0) {
				/* The other is as good or better. */
				chosen = 0;
			} else if (better < 0 && other < index) {
				/* An earlier signature of the same parameters stands for this one. */
				chosen = 0;
			}
		}

		/* The best one. */
		if (chosen)
			return &candidates[index];
	}

	/* None fits, or several fit equally well. */
	*ambiguous = 0;
	if (found != 0U)
		*ambiguous = 1;
	return NULL;
}

/*
 * Compares two built-in signatures that fit a call: 1 when the first's
 * conversions are better (some argument's better, none worse), -1 when
 * the two take the same parameters, 0 otherwise.
 */
static int
check_better_candidate(
	const struct check_candidate *first,
	const struct check_candidate *second,
	const struct glsl_type **types,
	unsigned count)
{
	unsigned index;
	int better;
	int same;
	int any_better;
	int all_same;

	/* Each argument's two conversions. */
	any_better = 0;
	all_same = 1;
	for (index = 0U; index < count; index++) {
		same = glsl_type_equal(first->parameters[index], second->parameters[index]);
		if (same)
			continue;
		all_same = 0;
		better = check_better_conversion(types[index], first->parameters[index], second->parameters[index]);
		if (better < 0)
			return 0;
		if (better > 0)
			any_better = 1;
	}

	/* The same parameters. */
	if (all_same)
		return -1;

	/* Better when one argument converts better. */
	if (any_better)
		return 1;
	return 0;
}

/*
 * Takes the built-in signature a call matched: its arguments converted,
 * the ones it writes checked writable, and a compute shader's atomics and
 * barriers checked by their rules.  Returns the call's type.
 */
static const struct glsl_type *
check_builtin_found(
	struct glsl_shader *shader,
	struct glsl_node *node,
	const struct glsl_builtin *builtin,
	const struct glsl_type **parameters,
	const struct glsl_type *result)
{
	struct glsl_node *argument;
	unsigned special;
	unsigned index;
	int writes;
	int status;

	/* The built-in. */
	node->builtin = builtin;

	/* The out arguments are writable, and the call writes them. */
	index = 0U;
	for (argument = node->child[1]; argument != NULL; argument = argument->next) {
		writes = glsl_builtin_writes(builtin, index);
		if (writes) {
			status = check_lvalue(shader, argument, "an out argument");
			if (status != 0)
				return glsl_type_error();
			node->flags |= GLSL_NODE_SIDE_EFFECTS;
		}

		/* The next argument. */
		index++;
	}

	/* An interpolation's first argument is a fragment input itself, never converted. */
	special = 0U;
	if (builtin->operation == GLSL_BI_SPECIAL)
		special = builtin->number;
	if (special >= GLSL_SPECIAL_INTERPOLATE_AT_CENTROID && special <= GLSL_SPECIAL_INTERPOLATE_AT_OFFSET) {
		status = check_interpolant(shader, node->child[1], parameters[0]);
		if (status != 0)
			return glsl_type_error();
	}

	/* The arguments converted; a compute shader's atomics and barriers have rules of their own. */
	check_convert_arguments(shader, node, parameters);
	check_compute_call(shader, node);
	check_image_call(shader, node);

	/* A gather's component and offsets are constants where its rules say so. */
	if (special == GLSL_SPECIAL_TEXTURE_GATHER ||
	    special == GLSL_SPECIAL_TEXTURE_GATHER_OFFSET ||
	    special == GLSL_SPECIAL_TEXTURE_GATHER_OFFSETS) {
		status = check_gather(shader, node, builtin);
		if (status != 0)
			return glsl_type_error();
	}

	/* EmitStreamVertex's and EndStreamPrimitive's stream is a constant of a stream there is. */
	if (special == GLSL_SPECIAL_EMIT_STREAM_VERTEX || special == GLSL_SPECIAL_END_STREAM_PRIMITIVE) {
		status = check_stream_call(shader, node);
		if (status != 0)
			return glsl_type_error();
	}

	/* textureOffset's and textureLodOffset's offset is a constant (ConstOffset) in every version. */
	if (special == GLSL_SPECIAL_TEXTURE_OFFSET ||
	    special == GLSL_SPECIAL_TEXTURE_OFFSET_BIAS ||
	    special == GLSL_SPECIAL_TEXTURE_LOD_OFFSET) {
		status = check_texture_offset(shader, node, builtin);
		if (status != 0)
			return glsl_type_error();
	}

	/* Succeeded: the result's type. */
	return result;
}

/*
 * Checks the offset of textureOffset and textureLodOffset (the argument
 * of the signature's code j or k): a constant expression, which the
 * lookup takes as its ConstOffset.  Returns 0, or -1 after an error.
 */
static int
check_texture_offset(
	struct glsl_shader *shader,
	struct glsl_node *node,
	const struct glsl_builtin *builtin)
{
	struct glsl_node *argument;
	unsigned index;
	char code;

	/* Each argument by its code. */
	index = 0U;
	for (argument = node->child[1]; argument != NULL; argument = argument->next) {
		code = builtin->signature[index + 1U];
		index++;

		/* The offset. */
		if ((code == 'j' || code == 'k') && argument->constant == NULL) {
			glsl_error(shader, argument->line, "%s's offset must be a constant expression", builtin->name);
			return -1;
		}
	}

	/* Succeeded: the offset is a constant. */
	return 0;
}

/*
 * Checks a gather's constant arguments (GL_ARB_gpu_shader5): the
 * component, a constant 0 to 3 (the signature's last code i);
 * textureGatherOffsets' four offsets, a constant; and
 * textureGatherOffset's offset, a constant unless GL_ARB_gpu_shader5
 * lets it be computed.  Returns 0, or -1 after an error.
 */
static int
check_gather(
	struct glsl_shader *shader,
	struct glsl_node *node,
	const struct glsl_builtin *builtin)
{
	struct glsl_node *argument;
	unsigned index;
	unsigned mask;
	char code;

	/* Each argument by its code. */
	mask = glsl_version_mask(shader);
	index = 0U;
	for (argument = node->child[1]; argument != NULL; argument = argument->next) {
		code = builtin->signature[index + 1U];
		index++;

		/* The component: the last argument, a constant 0 to 3. */
		if (code == 'i' && argument->next == NULL) {
			if (argument->constant == NULL) {
				glsl_error(shader, argument->line, "a gather's component must be a constant expression");
				return -1;
			}
			if (argument->constant->values[0].i < 0 || argument->constant->values[0].i > 3) {
				glsl_error(shader, argument->line, "a gather's component must be 0, 1, 2 or 3");
				return -1;
			}
			continue;
		}

		/* The four offsets. */
		if (code == 'L' && argument->constant == NULL) {
			glsl_error(shader, argument->line, "textureGatherOffsets' offsets must be a constant expression");
			return -1;
		}

		/* The one offset, computed only with GL_ARB_gpu_shader5. */
		if (code == 'j' && argument->constant == NULL && (mask & GLSL_IN_SHADER5) == 0U) {
			glsl_error(shader, argument->line, "textureGatherOffset's offset must be a constant expression");
			return -1;
		}
	}

	/* Succeeded: the arguments fit. */
	return 0;
}

/*
 * Checks the interpolant of interpolateAtCentroid, interpolateAtSample
 * and interpolateAtOffset: a float input of the fragment shader (an
 * element of an input array, a member or components of one), as it is
 * (not converted).  Returns 0, or -1 after an error.
 */
static int
check_interpolant(
	struct glsl_shader *shader,
	struct glsl_node *argument,
	const struct glsl_type *parameter)
{
	struct glsl_node *root;
	int same;

	/* The argument's type is the parameter's. */
	same = glsl_type_equal(argument->type, parameter);
	if (!same) {
		check_type_error(shader, argument->line, "an interpolant must be a float input, not %s", argument->type, NULL);
		return -1;
	}

	/* The variable it is a part of. */
	root = argument;
	while (root->kind == GLSL_N_INDEX || root->kind == GLSL_N_FIELD)
		root = root->child[0];

	/* An input the shader declares (a built-in input is not interpolated). */
	if (root->kind != GLSL_N_IDENTIFIER ||
	    root->symbol == NULL ||
	    root->symbol->where != GLSL_VAR_INPUT ||
	    root->symbol->builtin != GLSL_BUILTIN_NONE) {
		glsl_error(shader, argument->line, "an interpolant must be an input of the fragment shader");
		return -1;
	}

	/* Succeeded: an input. */
	return 0;
}

/* Checks a selection: a struct member, or a swizzle of a vector. */
static const struct glsl_type *
check_field(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	const struct glsl_type *type;
	unsigned index;
	int differs;
	int status;

	/* The object. */
	type = check_expression(shader, node->child[0]);
	if (type->kind == GLSL_KIND_ERROR)
		return type;

	/* A struct's member. */
	if (type->kind == GLSL_KIND_STRUCT) {
		for (index = 0U; index < type->field_count; index++) {
			differs = strcmp(type->fields[index].name, node->name);
			if (differs == 0) {
				node->field = index;
				return type->fields[index].type;
			}
		}

		/* No member of the name. */
		glsl_error(shader, node->line, "struct %s has no member '%s'", type->name, node->name);
		return glsl_type_error();
	}

	/* A vector's swizzle. */
	if (type->kind == GLSL_KIND_VECTOR) {
		status = check_swizzle(shader, node, type);
		if (status != 0)
			return glsl_type_error();
		node->flags |= GLSL_NODE_SWIZZLE;
		return glsl_type_vector(type->base, node->swizzle_count);
	}

	/* Nothing else has members. */
	check_type_error(shader, node->line, "%s has no members", type, NULL);
	return glsl_type_error();
}

/* Reads a swizzle's components (from one of xyzw, rgba, stpq) into the node. */
static int
check_swizzle(
	struct glsl_shader *shader,
	struct glsl_node *node,
	const struct glsl_type *type)
{
	static const char *const sets[3] = { "xyzw", "rgba", "stpq" };
	const char *name;
	const char *found;
	unsigned length;
	unsigned index;
	unsigned set;
	unsigned first_set;

	/* One to four components. */
	name = node->name;
	length = (unsigned)strlen(name);
	if (length == 0U || length > CHECK_SWIZZLE_MAX) {
		glsl_error(shader, node->line, "invalid swizzle '%s'", name);
		return -1;
	}

	/* Each component, all from the first one's set, inside the vector. */
	first_set = 3U;
	for (index = 0U; index < length; index++) {
		found = NULL;
		for (set = 0U; set < 3U; set++) {
			found = strchr(sets[set], name[index]);
			if (found != NULL)
				break;
		}

		/* The component, within one set. */
		if (found == NULL || (index > 0U && set != first_set)) {
			glsl_error(shader, node->line, "invalid swizzle '%s'", name);
			return -1;
		}

		/* The component's index. */
		first_set = set;
		node->swizzle[index] = (unsigned)(found - sets[set]);
		if (node->swizzle[index] >= type->components) {
			glsl_error(shader, node->line, "swizzle '%s' is outside the %s", name, type->name);
			return -1;
		}
	}

	/* Succeeded: the components. */
	node->swizzle_count = length;
	return 0;
}

/* Checks a subscript of an array, a matrix or a vector. */
static const struct glsl_type *
check_index(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	const struct glsl_type *type;
	const struct glsl_type *index_type;
	unsigned size;
	unsigned mask;
	int32_t value;
	int has_sampler;
	int of_blocks;

	/* The object and the index. */
	type = check_expression(shader, node->child[0]);
	index_type = check_expression(shader, node->child[1]);
	if (type->kind == GLSL_KIND_ERROR || index_type->kind == GLSL_KIND_ERROR)
		return glsl_type_error();

	/* An integer index. */
	if (index_type->kind != GLSL_KIND_SCALAR || (index_type->base != GLSL_BASE_INT && index_type->base != GLSL_BASE_UINT)) {
		check_type_error(shader, node->line, "an index must be an integer, not %s", index_type, NULL);
		return glsl_type_error();
	}

	/* Something with elements, and how many. */
	if (type->kind == GLSL_KIND_ARRAY) {
		size = type->length;
	} else if (type->kind == GLSL_KIND_MATRIX) {
		size = type->columns;
	} else if (type->kind == GLSL_KIND_VECTOR) {
		size = type->components;
	} else {
		check_type_error(shader, node->line, "%s cannot be indexed", type, NULL);
		return glsl_type_error();
	}

	/* A constant index must be inside. */
	if (node->child[1]->constant != NULL) {
		value = node->child[1]->constant->values[0].i;
		if (value < 0 || (size != 0U && (uint32_t)value >= size)) {
			glsl_error(shader, node->line, "index %d is outside the %u elements", (int)value, size);
			return glsl_type_error();
		}
	}

	/*
	 * An array of samplers takes a computed (dynamically uniform) index
	 * from GLSL 4.00 or GL_ARB_gpu_shader5; desktop 1.30 to 3.30 and
	 * OpenGL ES 3.00 and 3.10 take only constant ones (OpenGL ES 1.00
	 * keeps its own loop indices).
	 */
	has_sampler = 0;
	if (type->kind == GLSL_KIND_ARRAY && node->child[1]->constant == NULL)
		has_sampler = glsl_type_contains_sampler(type);
	if (has_sampler) {
		mask = glsl_version_mask(shader);
		if ((mask & GLSL_IN_DESKTOP_130_UP) != 0U && (mask & GLSL_IN_SHADER5) == 0U) {
			glsl_error(shader, node->line, "an array of samplers takes a constant index (GLSL 4.00 or GL_ARB_gpu_shader5 takes others)");
			return glsl_type_error();
		}
		if ((mask & GLSL_IN_ES300) != 0U && type->element->kind == GLSL_KIND_SAMPLER && type->element->image) {
			glsl_error(shader, node->line, "an array of images takes a constant index in OpenGL ES 3.10");
			return glsl_type_error();
		}
		if ((mask & GLSL_IN_ES300) != 0U) {
			glsl_error(shader, node->line, "an array of samplers takes a constant index in OpenGL ES 3.00 and 3.10");
			return glsl_type_error();
		}
	}

	/*
	 * An array of uniform blocks takes a computed (dynamically uniform)
	 * index from GLSL 4.00 or GL_ARB_gpu_shader5; desktop 1.50 to 3.30
	 * and OpenGL ES 3.00 and 3.10 take only constant ones.
	 */
	of_blocks = 0;
	if (type->kind == GLSL_KIND_ARRAY &&
	    node->child[1]->constant == NULL &&
	    type->element->kind == GLSL_KIND_STRUCT &&
	    type->element->block == GLSL_STORAGE_UNIFORM)
		of_blocks = 1;
	if (of_blocks) {
		mask = glsl_version_mask(shader);
		if ((mask & GLSL_IN_SHADER5) == 0U) {
			glsl_error(shader, node->line, "an array of uniform blocks takes a constant index (GLSL 4.00 or GL_ARB_gpu_shader5 takes others)");
			return glsl_type_error();
		}
	}

	/* Succeeded: the element's type. */
	return glsl_type_column(type);
}

/* Checks ".length()" of an array: a constant int. */
static const struct glsl_type *
check_length(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	const struct glsl_type *type;
	struct glsl_constant *value;
	int allowed;

	/* An array of a known size (1.20 on). */
	type = check_expression(shader, node->child[0]);
	if (type->kind == GLSL_KIND_ERROR)
		return type;
	allowed = glsl_since(shader, GLSL_VERSION_120, GLSL_VERSION_ES300);
	if (!allowed) {
		glsl_error(shader, node->line, "length() needs GLSL 1.20");
		return glsl_type_error();
	}

	/* Of an array. */
	if (type->kind != GLSL_KIND_ARRAY) {
		check_type_error(shader, node->line, "%s has no length()", type, NULL);
		return glsl_type_error();
	}

	/* A run-time array's length is known when the shader runs: an int, not a constant (ws101-p008). */
	if (type->length == 0U)
		return glsl_type_scalar(GLSL_BASE_INT);

	/* The length as a constant. */
	value = glsl_constant_new(&shader->arena, glsl_type_scalar(GLSL_BASE_INT));
	value->values[0].u = type->length;
	node->constant = value;

	/* Succeeded: an int. */
	return value->type;
}

/*
 * Returns an expression converted to a type: itself when it has the
 * type, a conversion node around it when the version converts it
 * implicitly, or NULL.
 */
static struct glsl_node *
check_convert(
	struct glsl_shader *shader,
	struct glsl_node *node,
	const struct glsl_type *target)
{
	struct glsl_node *converted;
	int same;
	int convertible;

	/* The same type needs nothing. */
	same = glsl_type_equal(node->type, target);
	if (same)
		return node;

	/* A conversion the version allows. */
	convertible = check_can_convert(shader, node->type, target);
	if (!convertible)
		return NULL;

	/* The conversion node, with its value when the operand is constant. */
	converted = glsl_alloc(&shader->arena, sizeof(*converted));
	converted->kind = GLSL_N_CONVERT;
	converted->line = node->line;
	converted->child[0] = node;
	converted->type = target;
	converted->flags = node->flags & GLSL_NODE_SIDE_EFFECTS;
	converted->next = node->next;
	node->next = NULL;
	converted->constant = glsl_fold(shader, converted);

	/* Succeeded: the converted expression. */
	return converted;
}

/*
 * Reports whether a type converts implicitly to another, desktop 1.20
 * on: int (and uint from 1.30) to float of the same shape; int, uint and
 * float to double of the same shape, and a float matrix to the double
 * matrix of its shape (the doubles exist only where they convert); int
 * to uint of the same shape (4.00, GL_ARB_gpu_shader5).
 */
static int
check_can_convert(
	const struct glsl_shader *shader,
	const struct glsl_type *from,
	const struct glsl_type *to)
{
	unsigned mask;

	/* OpenGL ES and desktop 1.10 convert nothing. */
	if (shader->es || shader->version < GLSL_VERSION_120)
		return 0;

	/* One shape. */
	if (from->kind != to->kind || from->components != to->components || from->columns != to->columns)
		return 0;

	/* A float matrix to a double one. */
	if (from->kind == GLSL_KIND_MATRIX) {
		if (from->base == GLSL_BASE_FLOAT && to->base == GLSL_BASE_DOUBLE)
			return 1;
		return 0;
	}

	/* Otherwise scalars and vectors: an int to a uint, 4.00 on (GL_ARB_gpu_shader5). */
	if (from->kind != GLSL_KIND_SCALAR && from->kind != GLSL_KIND_VECTOR)
		return 0;
	if (from->base == GLSL_BASE_INT && to->base == GLSL_BASE_UINT) {
		mask = glsl_version_mask(shader);
		if ((mask & GLSL_IN_SHADER5) != 0U)
			return 1;
		return 0;
	}

	/* Or to float or double. */
	if (to->base != GLSL_BASE_FLOAT && to->base != GLSL_BASE_DOUBLE)
		return 0;

	/* From int, or from uint (which only exists from 1.30). */
	if (from->base == GLSL_BASE_INT || from->base == GLSL_BASE_UINT)
		return 1;

	/* From float to double. */
	if (from->base == GLSL_BASE_FLOAT && to->base == GLSL_BASE_DOUBLE)
		return 1;

	/* Anything else. */
	return 0;
}

/*
 * Returns the kind of an argument's implicit conversion to a parameter
 * (CHECK_CONVERT_*).
 */
static unsigned
check_conversion_rank(
	const struct glsl_type *from,
	const struct glsl_type *to)
{
	int same;

	/* An exact match. */
	same = glsl_type_equal(from, to);
	if (same)
		return CHECK_CONVERT_NONE;

	/* A promotion of float to double. */
	if (from->base == GLSL_BASE_FLOAT && to->base == GLSL_BASE_DOUBLE)
		return CHECK_CONVERT_DOUBLE;

	/* An integer to float. */
	if (to->base == GLSL_BASE_FLOAT)
		return CHECK_CONVERT_FLOAT;

	/* An int to uint. */
	if (to->base == GLSL_BASE_UINT)
		return CHECK_CONVERT_UINT;

	/* An integer to double. */
	return CHECK_CONVERT_INT_DOUBLE;
}

/*
 * Compares two conversions of an argument by GLSL 4.00's rules: 1 when
 * the first is better, -1 when the second is, 0 when neither is (the same
 * kind, or int to uint against a conversion to float or double).  An
 * exact match is better than any conversion, float to double better than
 * any other conversion, an integer to float better than to double.
 */
static int
check_better_conversion(
	const struct glsl_type *from,
	const struct glsl_type *first,
	const struct glsl_type *second)
{
	unsigned first_rank;
	unsigned second_rank;

	/* The two kinds. */
	first_rank = check_conversion_rank(from, first);
	second_rank = check_conversion_rank(from, second);
	if (first_rank == second_rank)
		return 0;

	/* No conversion. */
	if (first_rank == CHECK_CONVERT_NONE)
		return 1;
	if (second_rank == CHECK_CONVERT_NONE)
		return -1;

	/* The promotion of float to double. */
	if (first_rank == CHECK_CONVERT_DOUBLE)
		return 1;
	if (second_rank == CHECK_CONVERT_DOUBLE)
		return -1;

	/* An integer to float rather than to double. */
	if (first_rank == CHECK_CONVERT_FLOAT && second_rank == CHECK_CONVERT_INT_DOUBLE)
		return 1;
	if (first_rank == CHECK_CONVERT_INT_DOUBLE && second_rank == CHECK_CONVERT_FLOAT)
		return -1;

	/* int to uint is neither better nor worse than the others. */
	return 0;
}

/*
 * Reports whether one overload is a better match for the arguments than
 * another (GLSL 4.00): some argument converts better, none worse.
 */
static int
check_better_overload(
	struct glsl_function *first,
	struct glsl_function *second,
	const struct glsl_type **types,
	unsigned count)
{
	unsigned index;
	int better;
	int any_better;

	/* Each argument's two conversions. */
	any_better = 0;
	for (index = 0U; index < count; index++) {
		better = check_better_conversion(types[index], first->parameters[index]->type, second->parameters[index]->type);
		if (better < 0)
			return 0;
		if (better > 0)
			any_better = 1;
	}

	/* Better when one argument converts better. */
	return any_better;
}

/* Reports a fragment shader input of a type that must be flat but is not: an integer's or a double's. */
static void
check_flat_error(
	struct glsl_shader *shader,
	const struct glsl_type *type,
	unsigned line)
{
	/* A double. */
	if (type->base == GLSL_BASE_DOUBLE) {
		glsl_error(shader, line, "double inputs of a fragment shader must be flat");
		return;
	}

	/* An integer. */
	glsl_error(shader, line, "integer inputs of a fragment shader must be flat");
}

/* Converts a call's arguments to the parameter types (the checks already found they convert). */
static void
check_convert_arguments(
	struct glsl_shader *shader,
	struct glsl_node *call,
	const struct glsl_type **parameters)
{
	struct glsl_node **link;
	struct glsl_node *converted;
	unsigned index;

	/* Each argument in place. */
	index = 0U;
	for (link = &call->child[1]; *link != NULL; link = &(*link)->next) {
		converted = check_convert(shader, *link, parameters[index]);
		if (converted != NULL)
			*link = converted;
		index++;
	}
}

/*
 * Checks that an expression can be written (what says by what), and
 * marks the variable written.  Returns 0, or -1 after reporting why not.
 */
static int
check_lvalue(
	struct glsl_shader *shader,
	struct glsl_node *node,
	const char *what)
{
	struct glsl_symbol *symbol;
	unsigned index;
	unsigned other;
	int status;

	/* A member or an element of something writable. */
	if (node->kind == GLSL_N_INDEX) {
		status = check_lvalue(shader, node->child[0], what);
		return status;
	}

	/* A swizzle of something writable. */
	if (node->kind == GLSL_N_FIELD) {
		/* A swizzle naming a component twice cannot be written. */
		for (index = 0U; index < node->swizzle_count; index++) {
			for (other = index + 1U; other < node->swizzle_count; other++) {
				if (node->swizzle[index] == node->swizzle[other]) {
					glsl_error(shader, node->line, "%s cannot write swizzle '%s' (a component repeats)", what, node->name);
					return -1;
				}
			}
		}

		/* The object must be writable itself. */
		status = check_lvalue(shader, node->child[0], what);
		return status;
	}

	/* Otherwise a variable. */
	if (node->kind != GLSL_N_IDENTIFIER || node->symbol == NULL) {
		glsl_error(shader, node->line, "%s needs something it can write", what);
		return -1;
	}

	/* The variable. */
	symbol = node->symbol;

	/* Constants, uniforms (in the default block or a block of their own) and inputs are read-only. */
	if (symbol->where == GLSL_VAR_CONST ||
	    symbol->where == GLSL_VAR_UNIFORM ||
	    symbol->where == GLSL_VAR_IMAGE ||
	    symbol->where == GLSL_VAR_BLOCK ||
	    symbol->where == GLSL_VAR_BLOCK_MEMBER ||
	    symbol->where == GLSL_VAR_INPUT) {
		glsl_error(shader, node->line, "%s cannot write '%s', which is read-only", what, symbol->name);
		return -1;
	}

	/* Nor is a readonly storage block (ws101-p008). */
	if ((symbol->where == GLSL_VAR_BUFFER || symbol->where == GLSL_VAR_BUFFER_MEMBER) && check_buffer_readonly(symbol)) {
		glsl_error(shader, node->line, "%s cannot write '%s', which is in a readonly buffer", what, symbol->name);
		return -1;
	}

	/* Nor are sampler parameters. */
	if (symbol->where == GLSL_VAR_PARAMETER && symbol->type->kind == GLSL_KIND_SAMPLER) {
		glsl_error(shader, node->line, "%s cannot write sampler '%s'", what, symbol->name);
		return -1;
	}

	/* Succeeded: writable. */
	symbol->written = 1U;
	return 0;
}

/* Reports an error whose message names one or two types. */
static void
check_type_error(
	struct glsl_shader *shader,
	unsigned line,
	const char *format,
	const struct glsl_type *first,
	const struct glsl_type *second)
{
	char first_name[96];
	char second_name[96];

	/* The types' spellings. */
	first_name[0] = '\0';
	second_name[0] = '\0';
	if (first != NULL)
		glsl_type_name(first, first_name, sizeof(first_name));
	if (second != NULL)
		glsl_type_name(second, second_name, sizeof(second_name));

	/* The message. */
	glsl_error(shader, line, format, first_name, second_name);
}

/* Marks an expression as having side effects when an operand has them. */
static void
check_mark_side_effects(
	struct glsl_node *node)
{
	struct glsl_node *child;
	unsigned index;

	/* The operands and the arguments. */
	for (index = 0U; index < 4U; index++) {
		for (child = node->child[index]; child != NULL; child = child->next) {
			if ((child->flags & GLSL_NODE_SIDE_EFFECTS) != 0U)
				node->flags |= GLSL_NODE_SIDE_EFFECTS;
			if (index != 1U || node->kind != GLSL_N_CALL)
				break;
		}
	}
}

/*
 * Marks what a list of nodes uses: every variable it names, and through
 * every call the function called.
 */
static void
check_use(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	struct glsl_symbol *blocks;
	uint32_t element;
	unsigned index;

	/* Each node of the list. */
	for (; node != NULL; node = node->next) {
		/* An element of an array of uniform blocks uses that element (every one for a computed index), then the index. */
		blocks = check_block_array(node);
		if (blocks != NULL) {
			blocks->used = 1U;
			if (node->child[1]->constant != NULL) {
				element = node->child[1]->constant->values[0].u;
				if (element < 32U)
					blocks->elements |= (uint32_t)1U << element;
			} else {
				blocks->elements |= check_all_elements(blocks->type);
			}
			check_use(shader, node->child[1]);
			continue;
		}

		/* A variable named (a block's member uses its block; an array of blocks named whole uses every element). */
		if (node->kind == GLSL_N_IDENTIFIER && node->symbol != NULL) {
			node->symbol->used = 1U;
			if (node->symbol->block != NULL)
				node->symbol->block->used = 1U;
			if (node->symbol->where == GLSL_VAR_BLOCK && node->symbol->type->kind == GLSL_KIND_ARRAY)
				node->symbol->elements |= check_all_elements(node->symbol->type);

			/* gl_FragCoord and gl_PointCoord read the hidden uniform that gives GL's directions (ws068-p004). */
			if ((node->symbol->builtin == GLSL_BUILTIN_FRAG_COORD || node->symbol->builtin == GLSL_BUILTIN_POINT_COORD) &&
			    shader->zed_fragment != NULL)
				shader->zed_fragment->used = 1U;
		}

		/* A subroutine uniform called: any subroutine of its type may run. */
		if (node->kind == GLSL_N_CALL && (node->flags & GLSL_NODE_SUBROUTINE_CALL) != 0U) {
			check_use_subroutines(shader, node);
		} else if (node->kind == GLSL_N_CALL && node->function != NULL) {
			/* A user function called. */
			check_use_function(shader, node->function, node->line);
		}

		/* interpolateAtOffset turns its offset's y into Vulkan's rows by the same hidden uniform. */
		if (node->kind == GLSL_N_CALL &&
		    node->builtin != NULL &&
		    node->builtin->operation == GLSL_BI_SPECIAL &&
		    node->builtin->number == GLSL_SPECIAL_INTERPOLATE_AT_OFFSET &&
		    shader->zed_fragment != NULL)
			shader->zed_fragment->used = 1U;

		/* An image load, store or atomic reads the hidden uniform of the image units it may touch. */
		if (node->kind == GLSL_N_CALL &&
		    node->builtin != NULL &&
		    node->builtin->operation == GLSL_BI_SPECIAL &&
		    node->builtin->number >= GLSL_SPECIAL_IMAGE_LOAD &&
		    node->builtin->number <= GLSL_SPECIAL_IMAGE_ATOMIC_COMP_SWAP &&
		    shader->zed_images != NULL)
			shader->zed_images->used = 1U;

		/* dFdy turns Vulkan's derivative along the target's rows into GL's by the same hidden uniform. */
		if (node->kind == GLSL_N_CALL &&
		    node->builtin != NULL &&
		    node->builtin->operation == GLSL_BI_SPECIAL &&
		    node->builtin->number == GLSL_SPECIAL_DFDY &&
		    shader->zed_fragment != NULL)
			shader->zed_fragment->used = 1U;

		/* The node's children (a constant's are not code, but naming a const does no harm). */
		for (index = 0U; index < 4U; index++) {
			if (node->child[index] != NULL)
				check_use(shader, node->child[index]);
		}
	}
}

/* Returns the array of uniform blocks a subscript indexes (b[i] of uniform B { ... } b[N]), or NULL for any other node. */
static struct glsl_symbol *
check_block_array(
	struct glsl_node *node)
{
	struct glsl_symbol *symbol;

	/* A subscript of a variable. */
	if (node->kind != GLSL_N_INDEX ||
	    node->child[0] == NULL ||
	    node->child[0]->kind != GLSL_N_IDENTIFIER)
		return NULL;
	symbol = node->child[0]->symbol;
	if (symbol == NULL)
		return NULL;

	/* The variable is an array of uniform blocks. */
	if (symbol->where != GLSL_VAR_BLOCK || symbol->type->kind != GLSL_KIND_ARRAY)
		return NULL;

	/* Succeeded: the array. */
	return symbol;
}

/* Returns the bits of every element of an array of uniform blocks (all 32 for a longer one, which the link refuses). */
static uint32_t
check_all_elements(
	const struct glsl_type *type)
{
	/* A longer array than the bits. */
	if (type->length >= 32U)
		return 0xffffffffU;

	/* Succeeded: one bit an element. */
	return ((uint32_t)1U << type->length) - 1U;
}

/*
 * Walks the subroutines a call through a subroutine uniform may run:
 * every subroutine of the uniform's type, of which there must be one.
 */
static void
check_use_subroutines(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	struct glsl_function *subroutine;
	unsigned index;
	unsigned found;

	/* Each subroutine one of whose types is the call's. */
	found = 0U;
	for (subroutine = shader->subroutines; subroutine != NULL; subroutine = subroutine->next_subroutine) {
		for (index = 0U; index < subroutine->subroutine_type_count; index++) {
			if (subroutine->subroutine_types[index] != node->function)
				continue;
			check_use_function(shader, subroutine, node->line);
			found++;
			break;
		}
	}

	/* At least one. */
	if (found == 0U)
		glsl_error(shader, node->line, "no subroutine is of the type '%s' calls", node->name);
}

/* Walks a function main reaches: its body's uses, static recursion, a missing definition. */
static void
check_use_function(
	struct glsl_shader *shader,
	struct glsl_function *function,
	unsigned line)
{
	/* A function called while it is being walked calls itself. */
	if (function->visiting) {
		glsl_error(shader, line, "function '%s' is recursive (GLSL does not allow recursion)", function->name);
		return;
	}

	/* Once each. */
	if (function->visited)
		return;
	if (function->body == NULL) {
		glsl_error(shader, line, "function '%s' is declared but not defined", function->name);
		function->visited = 1U;
		return;
	}

	/* The body. */
	function->visiting = 1U;
	check_use(shader, function->body);
	function->visiting = 0U;
	function->visited = 1U;
}

/*
 * Checks a shader storage block (GLSL ES 3.10, ws101-p008): a compute
 * shader's, std430 (the only layout), its binding below
 * GLSL_STORAGE_BINDINGS, and its instance name or its members as names of
 * their own, as a uniform block's.
 */
static void
check_storage_block(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	struct glsl_node *type_node;
	struct glsl_node *instance;
	struct glsl_symbol *block;
	struct glsl_symbol *member;
	const struct glsl_type *type;
	unsigned binding;
	unsigned index;
	int allowed;

	/* OpenGL ES 3.10's and desktop 4.30's, in a compute shader. */
	type_node = node->child[0];
	allowed = glsl_since(shader, GLSL_VERSION_430, GLSL_VERSION_ES310);
	if (!allowed) {
		glsl_error(shader, node->line, "buffer blocks need GLSL 4.30 or OpenGL ES 3.10");
		return;
	}
	if (shader->stage != GLSL_STAGE_COMPUTE) {
		glsl_error(shader, node->line, "buffer blocks are supported in compute shaders only");
		return;
	}

	/* std430 (the default of a buffer block), not std140, shared or packed, and no row-major matrices. */
	if ((type_node->layout & (GLSL_LAYOUT_STD140 | GLSL_LAYOUT_SHARED | GLSL_LAYOUT_PACKED)) != 0U)
		glsl_error(shader, node->line, "buffer blocks are laid out std430 only");
	if ((type_node->layout & GLSL_LAYOUT_ROW_MAJOR) != 0U)
		glsl_error(shader, node->line, "row-major matrices in buffer blocks are not supported");

	/* The binding, 0 unless the layout gives one. */
	binding = 0U;
	if ((type_node->layout & GLSL_LAYOUT_BINDING) != 0U)
		binding = type_node->binding;
	if (binding >= GLSL_STORAGE_BINDINGS) {
		glsl_error(shader, node->line, "a buffer block's binding must be below %u", GLSL_STORAGE_BINDINGS);
		return;
	}

	/* The struct, and no arrays of blocks. */
	type = check_block_type(shader, node, GLSL_STORAGE_BUFFER);
	instance = node->child[2];
	if (instance != NULL && (instance->flags & GLSL_NODE_ARRAY) != 0U) {
		glsl_error(shader, node->line, "arrays of buffer blocks are not supported");
		return;
	}
	for (index = 0U; index < type->field_count; index++) {
		if (type->fields[index].row_major)
			glsl_error(shader, node->line, "row-major matrices in buffer blocks are not supported");
	}

	/* The block's symbol: the instance, or one without a scope named by the block. */
	if (instance != NULL) {
		check_reserved_name(shader, instance->name, node->line);
		block = glsl_declare(shader, instance->name, GLSL_SYMBOL_VARIABLE, node->line);
		if (block->type != NULL)
			return;
	} else {
		block = glsl_alloc(&shader->arena, sizeof(*block));
		block->name = node->name;
		block->kind = GLSL_SYMBOL_VARIABLE;
		block->line = node->line;
		block->explicit_location = GLSL_NO_LOCATION;
	}

	/* A buffer at its binding, with its memory qualifiers. */
	block->type = type;
	block->where = GLSL_VAR_BUFFER;
	block->storage = GLSL_STORAGE_BUFFER;
	block->memory = type_node->memory;
	block->layout_binding = binding;
	glsl_add_global(shader, block);
	if (instance != NULL)
		return;

	/* Each member a name of its own, in the block. */
	for (index = 0U; index < type->field_count; index++) {
		check_reserved_name(shader, type->fields[index].name, node->line);
		member = glsl_declare(shader, type->fields[index].name, GLSL_SYMBOL_VARIABLE, node->line);
		if (member->type != NULL)
			continue;
		member->type = type->fields[index].type;
		member->where = GLSL_VAR_BUFFER_MEMBER;
		member->storage = GLSL_STORAGE_BUFFER;
		member->block = block;
		member->member = index;
	}
}

/*
 * Checks "layout(local_size_x = X, local_size_y = Y, local_size_z = Z)
 * in;" (ws101-p008): a compute shader's, each size at least 1 (1 when not
 * given) and at most OpenGL ES 3.10's guaranteed 128, 128 and 64 with 128
 * invocations in all, declared once (or again the same); gl_WorkGroupSize
 * takes it.
 */
static void
check_local_size(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	static const unsigned limits[3] = { 128U, 128U, 64U };
	struct glsl_node *type_node;
	struct glsl_symbol *size;
	unsigned values[3];
	unsigned axis;

	/* A compute shader's input layout. */
	type_node = node->child[0];
	if (shader->stage != GLSL_STAGE_COMPUTE || type_node->storage != GLSL_STORAGE_IN) {
		glsl_error(shader, node->line, "a workgroup size is a compute shader's input layout ('layout(local_size_x = 64) in;')");
		return;
	}

	/* Each size, 1 unless given, within the limits. */
	for (axis = 0U; axis < 3U; axis++) {
		values[axis] = type_node->local_size[axis];
		if (values[axis] == 0U)
			values[axis] = 1U;
		if (values[axis] > limits[axis]) {
			glsl_error(shader, node->line, "a workgroup size along an axis is at most 128, 128 and 64");
			return;
		}
	}
	if (values[0] * values[1] * values[2] > 128U) {
		glsl_error(shader, node->line, "a workgroup has at most 128 invocations");
		return;
	}

	/* Declared once, or again the same. */
	if (shader->local_size[0] != 0U &&
	    (shader->local_size[0] != values[0] || shader->local_size[1] != values[1] || shader->local_size[2] != values[2])) {
		glsl_error(shader, node->line, "the workgroup size is declared twice, differently");
		return;
	}

	/* The size, and gl_WorkGroupSize's value. */
	for (axis = 0U; axis < 3U; axis++)
		shader->local_size[axis] = values[axis];
	size = check_lookup(shader, "gl_WorkGroupSize");
	if (size != NULL && size->constant != NULL && size->constant->count >= 3U) {
		for (axis = 0U; axis < 3U; axis++)
			size->constant->values[axis].u = values[axis];
	}
}

/*
 * Checks the rules of a compute shader's built-in calls (ws101-p008): an
 * atomic function's first argument is an int or uint of a buffer block or
 * a shared variable, which it writes; barrier() is called in main, outside
 * every selection, loop and switch, and before any return.
 */
static void
check_compute_call(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	struct glsl_symbol *root;
	unsigned special;
	int status;

	/* Only the specials of GLSL ES 3.10. */
	if (node->builtin == NULL || node->builtin->operation != GLSL_BI_SPECIAL)
		return;
	special = node->builtin->number;
	if (special < GLSL_SPECIAL_ATOMIC_ADD || special > GLSL_SPECIAL_GROUP_MEMORY_BARRIER)
		return;

	/* barrier(): in main, outside control flow, before any return. */
	if (special == GLSL_SPECIAL_BARRIER) {
		if (shader->current == NULL || strcmp(shader->current->name, "main") != 0)
			glsl_error(shader, node->line, "barrier() is allowed only in main");
		else if (shader->control_depth != 0U)
			glsl_error(shader, node->line, "barrier() is not allowed inside a selection, a loop or a switch");
		else if (shader->returned != 0U)
			glsl_error(shader, node->line, "barrier() is not allowed after a return");
		return;
	}

	/* The other barriers have no argument. */
	if (special > GLSL_SPECIAL_ATOMIC_COMP_SWAP)
		return;

	/* An atomic's memory: a variable of a buffer block or a shared variable, which it writes. */
	root = check_memory_root(node->child[1]);
	if (root == NULL ||
	    (root->where != GLSL_VAR_BUFFER && root->where != GLSL_VAR_BUFFER_MEMBER && root->where != GLSL_VAR_SHARED)) {
		glsl_error(shader, node->line, "'%s' needs a buffer or shared variable as its first argument", node->name);
		return;
	}
	status = check_lvalue(shader, node->child[1], node->name);
	if (status != 0)
		return;

	/* The call writes memory. */
	node->flags |= GLSL_NODE_SIDE_EFFECTS;
}

/*
 * Checks an image uniform's declaration (GLSL ES 3.10) and returns
 * GLSL_VAR_IMAGE: in a compute shader, one image or a sized array of
 * images (its elements at its binding's unit and the next ones, which
 * the code indexes with constant integral expressions), a format of its
 * texels' kind, units below GLSL_IMAGE_UNITS, and in OpenGL ES an image
 * of a format other than r32f, r32i and r32ui only read or only written.
 */
static unsigned
check_image_where(
	struct glsl_shader *shader,
	struct glsl_node *type_node,
	const struct glsl_type *type,
	unsigned line)
{
	const struct glsl_type *image;
	unsigned format;
	unsigned memory;
	unsigned base;
	unsigned units;

	/* A compute shader's (a vertex or fragment shader has no image units). */
	if (shader->stage != GLSL_STAGE_COMPUTE)
		glsl_error(shader, line, "image uniforms are supported in compute shaders only");

	/* One image, or an array of them with a size (one unit an element), not an array of arrays. */
	image = type;
	units = 1U;
	if (type->kind == GLSL_KIND_ARRAY) {
		image = type->element;
		units = type->length;
		if (image->kind != GLSL_KIND_SAMPLER) {
			glsl_error(shader, line, "arrays of arrays of images are not supported");
			return GLSL_VAR_IMAGE;
		}
		if (units == 0U) {
			glsl_error(shader, line, "an array of images needs a size");
			return GLSL_VAR_IMAGE;
		}
	}

	/* A format, of the kind of texels the image's type reads. */
	format = type_node->image_format;
	base = glsl_image_format_base(format);
	if ((type_node->layout & GLSL_LAYOUT_FORMAT) == 0U || format == 0U) {
		glsl_error(shader, line, "an image uniform needs a format layout qualifier");
	} else if (base != image->base) {
		glsl_error(shader, line, "the image's format does not match the kind of its texels");
	}

	/* Units there are, for every element (GLSL ES 3.10 section 4.4.5: binding through binding + N - 1). */
	if ((type_node->layout & GLSL_LAYOUT_BINDING) != 0U && type_node->binding >= GLSL_IMAGE_UNITS) {
		glsl_error(shader, line, "image binding %u is past the last unit %u", type_node->binding, GLSL_IMAGE_UNITS - 1U);
	} else if (units > GLSL_IMAGE_UNITS || type_node->binding > GLSL_IMAGE_UNITS - units) {
		glsl_error(shader, line, "the %u images from binding %u are past the last unit %u", units, type_node->binding,
			   GLSL_IMAGE_UNITS - 1U);
	}

	/* OpenGL ES reads and writes only the 32-bit single-channel formats both ways. */
	memory = type_node->memory;
	if (shader->es &&
	    format != GLSL_IMAGE_FORMAT_R32F &&
	    format != GLSL_IMAGE_FORMAT_R32I &&
	    format != GLSL_IMAGE_FORMAT_R32UI &&
	    (memory & (GLSL_MEMORY_READONLY | GLSL_MEMORY_WRITEONLY)) == 0U)
		glsl_error(shader, line, "an image of a format other than r32f, r32i and r32ui must be readonly or writeonly");

	/* Succeeded: an image uniform. */
	return GLSL_VAR_IMAGE;
}

/*
 * Checks a call of an image function (GLSL ES 3.10): its image is an image
 * uniform or parameter that the call may read (imageLoad), write
 * (imageStore) or both (the atomics, of an r32i or r32ui image); a store
 * or an atomic writes memory.
 */
static void
check_image_call(
	struct glsl_shader *shader,
	struct glsl_node *node)
{
	const struct glsl_type *image;
	struct glsl_symbol *root;
	unsigned special;
	unsigned memory;
	unsigned format;
	unsigned base;

	/* Only the image functions. */
	if (node->builtin == NULL || node->builtin->operation != GLSL_BI_SPECIAL)
		return;
	special = node->builtin->number;
	if (special < GLSL_SPECIAL_IMAGE_SIZE || special > GLSL_SPECIAL_IMAGE_ATOMIC_COMP_SWAP)
		return;

	/* The image: a uniform's or a parameter's. */
	root = check_memory_root(node->child[1]);
	if (root == NULL || (root->where != GLSL_VAR_IMAGE && root->where != GLSL_VAR_PARAMETER)) {
		glsl_error(shader, node->line, "'%s' needs an image uniform or parameter as its first argument", node->name);
		return;
	}

	/* What the image's memory qualifiers let the call do. */
	memory = root->memory;
	if (special == GLSL_SPECIAL_IMAGE_LOAD && (memory & GLSL_MEMORY_WRITEONLY) != 0U)
		glsl_error(shader, node->line, "imageLoad cannot read '%s', which is writeonly", root->name);
	if (special == GLSL_SPECIAL_IMAGE_STORE && (memory & GLSL_MEMORY_READONLY) != 0U)
		glsl_error(shader, node->line, "imageStore cannot write '%s', which is readonly", root->name);
	if (special >= GLSL_SPECIAL_IMAGE_ATOMIC_ADD && (memory & (GLSL_MEMORY_READONLY | GLSL_MEMORY_WRITEONLY)) != 0U)
		glsl_error(shader, node->line, "'%s' cannot use '%s', which is readonly or writeonly", node->name, root->name);

	/*
	 * An atomic's image holds one 32-bit integer a texel (a float image's
	 * exchange needs a Vulkan device feature beyond the core one, and is
	 * not supported).
	 */
	image = root->type;
	if (image->kind == GLSL_KIND_ARRAY)
		image = image->element;
	format = image->image_format;
	base = image->base;
	if (special >= GLSL_SPECIAL_IMAGE_ATOMIC_ADD) {
		if (base == GLSL_BASE_FLOAT)
			glsl_error(shader, node->line, "'%s' of a float image is not supported (use r32i or r32ui)", node->name);
		else if (root->where == GLSL_VAR_IMAGE && format != GLSL_IMAGE_FORMAT_R32I && format != GLSL_IMAGE_FORMAT_R32UI)
			glsl_error(shader, node->line, "'%s' needs an image of format r32i or r32ui", node->name);
	}

	/* A store or an atomic writes memory. */
	if (special >= GLSL_SPECIAL_IMAGE_STORE)
		node->flags |= GLSL_NODE_SIDE_EFFECTS;
}

/* Returns the variable an expression is a part of (a member, element or swizzle of it), or NULL for any other expression (ws101-p008). */
static struct glsl_symbol *
check_memory_root(
	struct glsl_node *node)
{
	/* Down the members, elements and swizzles. */
	while (node != NULL && (node->kind == GLSL_N_FIELD || node->kind == GLSL_N_INDEX))
		node = node->child[0];

	/* A variable. */
	if (node == NULL || node->kind != GLSL_N_IDENTIFIER)
		return NULL;
	return node->symbol;
}

/* Reports whether a storage block, or the block of a member without an instance name, is readonly (ws101-p008). */
static int
check_buffer_readonly(
	const struct glsl_symbol *symbol)
{
	/* A member's block. */
	if (symbol->where == GLSL_VAR_BUFFER_MEMBER && symbol->block != NULL)
		symbol = symbol->block;

	/* Succeeded: the block's qualifier. */
	if ((symbol->memory & GLSL_MEMORY_READONLY) != 0U)
		return 1;
	return 0;
}
