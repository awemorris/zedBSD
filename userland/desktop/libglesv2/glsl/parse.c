/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GLSL compiler's parser: the preprocessed tokens become the syntax
 * tree the checker reads.  A recursive descent over GLSL's grammar, with
 * expressions by precedence climbing.
 *
 * Identifiers are classified here: keywords and built-in type names by
 * the version, struct type names by the parser's own scopes (a
 * declaration and an expression statement start the same way otherwise).
 * A syntax error ends the compile.
 */

#include "internal.h"

#include <string.h>

/* The binary operators' precedence levels, lowest first (0: not a binary operator). */
#define PARSE_LEVEL_OR		1U
#define PARSE_LEVEL_XOR		2U
#define PARSE_LEVEL_AND		3U
#define PARSE_LEVEL_BIT_OR	4U
#define PARSE_LEVEL_BIT_XOR	5U
#define PARSE_LEVEL_BIT_AND	6U
#define PARSE_LEVEL_EQUALITY	7U
#define PARSE_LEVEL_RELATION	8U
#define PARSE_LEVEL_SHIFT	9U
#define PARSE_LEVEL_ADD		10U
#define PARSE_LEVEL_MULTIPLY	11U

/*
 * A keyword's spelling, its value and the versions it is a keyword in.
 */
struct parse_keyword {
	const char *text;
	unsigned keyword;
	unsigned versions;
};

/*
 * The keywords that are not type names, and the words reserved for the
 * future (GLSL_K_RESERVED), with the versions each is one in.
 */
static const struct parse_keyword parse_keywords[] = {
	{ "attribute", GLSL_K_ATTRIBUTE, GLSL_IN_LEGACY },
	{ "const", GLSL_K_CONST, GLSL_IN_ALL },
	{ "uniform", GLSL_K_UNIFORM, GLSL_IN_ALL },
	{ "varying", GLSL_K_VARYING, GLSL_IN_LEGACY },
	{ "break", GLSL_K_BREAK, GLSL_IN_ALL },
	{ "continue", GLSL_K_CONTINUE, GLSL_IN_ALL },
	{ "do", GLSL_K_DO, GLSL_IN_ALL },
	{ "for", GLSL_K_FOR, GLSL_IN_ALL },
	{ "while", GLSL_K_WHILE, GLSL_IN_ALL },
	{ "if", GLSL_K_IF, GLSL_IN_ALL },
	{ "else", GLSL_K_ELSE, GLSL_IN_ALL },
	{ "in", GLSL_K_IN, GLSL_IN_ALL },
	{ "out", GLSL_K_OUT, GLSL_IN_ALL },
	{ "inout", GLSL_K_INOUT, GLSL_IN_ALL },
	{ "true", GLSL_K_TRUE, GLSL_IN_ALL },
	{ "false", GLSL_K_FALSE, GLSL_IN_ALL },
	{ "lowp", GLSL_K_LOWP, GLSL_IN_ALL },
	{ "mediump", GLSL_K_MEDIUMP, GLSL_IN_ALL },
	{ "highp", GLSL_K_HIGHP, GLSL_IN_ALL },
	{ "precision", GLSL_K_PRECISION, GLSL_IN_ALL },
	{ "invariant", GLSL_K_INVARIANT, GLSL_IN_ALL },
	{ "discard", GLSL_K_DISCARD, GLSL_IN_ALL },
	{ "return", GLSL_K_RETURN, GLSL_IN_ALL },
	{ "struct", GLSL_K_STRUCT, GLSL_IN_ALL },
	{ "centroid", GLSL_K_CENTROID, GLSL_IN_120_UP },
	{ "switch", GLSL_K_SWITCH, GLSL_IN_130_UP },
	{ "case", GLSL_K_CASE, GLSL_IN_130_UP },
	{ "default", GLSL_K_DEFAULT, GLSL_IN_130_UP },
	{ "flat", GLSL_K_FLAT, GLSL_IN_130_UP },
	{ "smooth", GLSL_K_SMOOTH, GLSL_IN_130_UP },
	{ "noperspective", GLSL_K_NOPERSPECTIVE, GLSL_IN_DESKTOP_130_UP },
	{ "layout", GLSL_K_LAYOUT, GLSL_IN_140_UP },
	{ "buffer", GLSL_K_BUFFER, GLSL_IN_ES310 },
	{ "shared", GLSL_K_SHARED, GLSL_IN_ES310 },
	{ "coherent", GLSL_K_COHERENT, GLSL_IN_ES310 },
	{ "volatile", GLSL_K_VOLATILE, GLSL_IN_ES310 },
	{ "restrict", GLSL_K_RESTRICT, GLSL_IN_ES310 },
	{ "readonly", GLSL_K_READONLY, GLSL_IN_ES310 },
	{ "writeonly", GLSL_K_WRITEONLY, GLSL_IN_ES310 },
	{ "patch", GLSL_K_PATCH, GLSL_IN_TESS },
	{ "precise", GLSL_K_PRECISE, GLSL_IN_SHADER5 },
	{ "subroutine", GLSL_K_SUBROUTINE, GLSL_IN_SUBROUTINE },
	{ "attribute", GLSL_K_RESERVED, GLSL_IN_ES300 },
	{ "varying", GLSL_K_RESERVED, GLSL_IN_ES300 },
	{ "noperspective", GLSL_K_RESERVED, GLSL_IN_ES300 },
	{ "coherent", GLSL_K_RESERVED, GLSL_IN_ES300 },
	{ "restrict", GLSL_K_RESERVED, GLSL_IN_ES300 },
	{ "readonly", GLSL_K_RESERVED, GLSL_IN_ES300 },
	{ "writeonly", GLSL_K_RESERVED, GLSL_IN_ES300 },
	{ "resource", GLSL_K_RESERVED, GLSL_IN_ES300 },
	{ "atomic_uint", GLSL_K_RESERVED, GLSL_IN_ES300 },
	{ "patch", GLSL_K_RESERVED, GLSL_IN_ES300 },
	{ "sample", GLSL_K_RESERVED, GLSL_IN_ES300 },
	{ "subroutine", GLSL_K_RESERVED, GLSL_IN_ES300 },
	{ "common", GLSL_K_RESERVED, GLSL_IN_ES300 },
	{ "partition", GLSL_K_RESERVED, GLSL_IN_ES300 },
	{ "active", GLSL_K_RESERVED, GLSL_IN_ES300 },
	{ "filter", GLSL_K_RESERVED, GLSL_IN_ES300 },
	{ "switch", GLSL_K_RESERVED, GLSL_IN_OLD },
	{ "default", GLSL_K_RESERVED, GLSL_IN_OLD },
	{ "flat", GLSL_K_RESERVED, GLSL_IN_ES100 },
	{ "asm", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "class", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "union", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "enum", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "typedef", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "template", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "this", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "packed", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "goto", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "inline", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "noinline", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "volatile", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "public", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "static", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "extern", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "external", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "interface", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "long", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "short", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "double", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "half", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "fixed", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "unsigned", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "superp", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "input", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "output", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "hvec2", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "hvec3", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "hvec4", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "dvec2", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "dvec3", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "dvec4", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "fvec2", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "fvec3", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "fvec4", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "sampler2DRect", GLSL_K_RESERVED, GLSL_IN_ALL & ~GLSL_IN_DESKTOP_140_UP },
	{ "sampler3DRect", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "sampler2DRectShadow", GLSL_K_RESERVED, GLSL_IN_ALL & ~GLSL_IN_DESKTOP_140_UP },
	{ "sampler1D", GLSL_K_RESERVED, GLSL_IN_ES },
	{ "sampler3D", GLSL_K_RESERVED, GLSL_IN_ES100 },
	{ "sampler1DShadow", GLSL_K_RESERVED, GLSL_IN_ES },
	{ "sampler2DShadow", GLSL_K_RESERVED, GLSL_IN_ES100 },
	{ "sizeof", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "cast", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "namespace", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "using", GLSL_K_RESERVED, GLSL_IN_ALL },
	{ "layout", GLSL_K_RESERVED, GLSL_IN_ES100 | GLSL_IN_110 | GLSL_IN_120 | GLSL_IN_130 }
};

/*
 * A struct type name the parser knows in a scope.
 */
struct glsl_parse_name {
	const char *name;
	unsigned length;
	struct glsl_parse_name *next;
};

/*
 * A scope of struct type names: a block, a function, or the whole shader.
 */
struct glsl_parse_scope {
	struct glsl_parse_name *names;
	struct glsl_parse_scope *parent;
};

static unsigned parse_classify(struct glsl_shader *shader, struct glsl_token *token);
static struct glsl_token *parse_peek(struct glsl_shader *shader, unsigned ahead);
static struct glsl_token *parse_take(struct glsl_shader *shader);
static unsigned parse_keyword_at(struct glsl_shader *shader, unsigned ahead);
static int parse_punct_at(struct glsl_shader *shader, unsigned ahead, unsigned punct);
static void parse_expect(struct glsl_shader *shader, unsigned punct, const char *what);
static void parse_unexpected(struct glsl_shader *shader, const char *what);
static const char *parse_identifier(struct glsl_shader *shader, const char *what);
static struct glsl_node *parse_node(struct glsl_shader *shader, unsigned kind, unsigned line);
static void parse_append(struct glsl_node **first, struct glsl_node **last, struct glsl_node *node);
static void parse_push_scope(struct glsl_shader *shader);
static void parse_pop_scope(struct glsl_shader *shader);
static void parse_add_type_name(struct glsl_shader *shader, const char *name);
static int parse_is_type_name(struct glsl_shader *shader, const struct glsl_token *token);
static int parse_starts_type(struct glsl_shader *shader, unsigned ahead);
static int parse_starts_declaration(struct glsl_shader *shader);
static int parse_is_qualifier(unsigned keyword);
static struct glsl_node *parse_external(struct glsl_shader *shader);
static struct glsl_node *parse_precision(struct glsl_shader *shader);
static struct glsl_node *parse_invariant(struct glsl_shader *shader);
static struct glsl_node *parse_subroutine(struct glsl_shader *shader);
static struct glsl_node *parse_fully_specified_type(struct glsl_shader *shader);
static void parse_qualifiers(struct glsl_shader *shader, struct glsl_node *type);
static void parse_layout(struct glsl_shader *shader, struct glsl_node *type);
static unsigned parse_primitive(const struct glsl_token *token);
static unsigned parse_local_size(const struct glsl_token *token);
static unsigned parse_image_format(const struct glsl_token *token);
static unsigned parse_spacing(const struct glsl_token *token);
static unsigned parse_order(const struct glsl_token *token);
static int parse_starts_block(struct glsl_shader *shader);
static struct glsl_node *parse_interface(struct glsl_shader *shader);
static void parse_type_specifier(struct glsl_shader *shader, struct glsl_node *type);
static void parse_struct(struct glsl_shader *shader, struct glsl_node *type);
static struct glsl_node *parse_array_size(struct glsl_shader *shader, struct glsl_node *owner);
static struct glsl_node *parse_declaration_rest(struct glsl_shader *shader, struct glsl_node *type, const char *first_name, unsigned line);
static struct glsl_node *parse_function(struct glsl_shader *shader, struct glsl_node *type, const char *name, unsigned line);
static struct glsl_node *parse_parameter(struct glsl_shader *shader);
static struct glsl_node *parse_statement(struct glsl_shader *shader);
static struct glsl_node *parse_compound(struct glsl_shader *shader, int new_scope);
static struct glsl_node *parse_declaration_statement(struct glsl_shader *shader);
static struct glsl_node *parse_if(struct glsl_shader *shader);
static struct glsl_node *parse_for(struct glsl_shader *shader);
static struct glsl_node *parse_while(struct glsl_shader *shader);
static struct glsl_node *parse_do(struct glsl_shader *shader);
static struct glsl_node *parse_switch(struct glsl_shader *shader);
static struct glsl_node *parse_jump(struct glsl_shader *shader, unsigned keyword);
static struct glsl_node *parse_expression(struct glsl_shader *shader);
static struct glsl_node *parse_assignment(struct glsl_shader *shader);
static struct glsl_node *parse_conditional(struct glsl_shader *shader);
static struct glsl_node *parse_binary(struct glsl_shader *shader, unsigned level);
static unsigned parse_binary_level(const struct glsl_token *token);
static struct glsl_node *parse_unary(struct glsl_shader *shader);
static struct glsl_node *parse_postfix(struct glsl_shader *shader);
static struct glsl_node *parse_primary(struct glsl_shader *shader);
static struct glsl_node *parse_constructor(struct glsl_shader *shader);
static struct glsl_node *parse_arguments(struct glsl_shader *shader, struct glsl_node *call);
static int parse_void_list(struct glsl_shader *shader);

/*
 * Parses a shader's preprocessed tokens into its translation unit (the
 * list of external declarations at shader->unit).
 */
void
glsl_parse(
	struct glsl_shader *shader)
{
	struct glsl_node *first;
	struct glsl_node *last;
	struct glsl_node *declaration;
	struct glsl_token *token;

	/* The global scope of type names. */
	shader->position = 0U;
	parse_push_scope(shader);

	/* External declarations up to the end. */
	first = NULL;
	last = NULL;
	for (;;) {
		token = parse_peek(shader, 0U);
		if (token->kind == GLSL_TOKEN_EOF)
			break;

		/* A stray semicolon at global scope is allowed. */
		if (token->kind == GLSL_TOKEN_PUNCT && token->punct == GLSL_P_SEMICOLON) {
			(void)parse_take(shader);
			continue;
		}

		/* The declaration, appended to the unit. */
		declaration = parse_external(shader);
		parse_append(&first, &last, declaration);
	}

	/* The unit. */
	shader->unit = first;
}

/*
 * Classifies an identifier token once: a keyword of the version, a
 * built-in type name (GLSL_K_TYPE), a reserved word, or none.
 */
static unsigned
parse_classify(
	struct glsl_shader *shader,
	struct glsl_token *token)
{
	const struct glsl_type *type;
	unsigned versions;
	unsigned mask;
	size_t index;
	int same;

	/* Only identifiers, and only once. */
	if (token->kind != GLSL_TOKEN_IDENTIFIER)
		return GLSL_K_NONE;
	if (token->classified)
		return token->keyword;
	token->classified = 1U;
	token->keyword = GLSL_K_NONE;
	mask = glsl_version_mask(shader);

	/* A built-in type name of this version. */
	type = glsl_type_named(token->text, token->length, &versions);
	if (type != NULL && (versions & mask) != 0U) {
		token->keyword = GLSL_K_TYPE;
		token->keyword_type = type;
		return token->keyword;
	}

	/* A keyword or a reserved word of this version. */
	for (index = 0U; index < sizeof(parse_keywords) / sizeof(parse_keywords[0]); index++) {
		if ((parse_keywords[index].versions & mask) == 0U)
			continue;
		same = glsl_token_is(token, parse_keywords[index].text);
		if (!same)
			continue;

		/* The keyword. */
		token->keyword = parse_keywords[index].keyword;
		break;
	}

	/* Succeeded: the classification. */
	return token->keyword;
}

/* Returns the token some places ahead of the current one (the EOF token past the end). */
static struct glsl_token *
parse_peek(
	struct glsl_shader *shader,
	unsigned ahead)
{
	unsigned at;

	/* The token, or the EOF that ends the list. */
	at = shader->position + ahead;
	if (at > shader->token_count)
		at = shader->token_count;

	/* The token. */
	return &shader->tokens[at];
}

/* Takes the current token and moves past it. */
static struct glsl_token *
parse_take(
	struct glsl_shader *shader)
{
	struct glsl_token *token;

	/* The current token; the position stays on the EOF at the end. */
	token = parse_peek(shader, 0U);
	if (shader->position < shader->token_count)
		shader->position++;

	/* Succeeded: the token taken. */
	return token;
}

/* Returns the keyword of the token some places ahead (GLSL_K_NONE for anything else). */
static unsigned
parse_keyword_at(
	struct glsl_shader *shader,
	unsigned ahead)
{
	struct glsl_token *token;
	unsigned keyword;

	/* The token's classification. */
	token = parse_peek(shader, ahead);
	keyword = parse_classify(shader, token);

	/* Succeeded: the keyword. */
	return keyword;
}

/* Reports whether the token some places ahead is a given punctuator. */
static int
parse_punct_at(
	struct glsl_shader *shader,
	unsigned ahead,
	unsigned punct)
{
	struct glsl_token *token;

	/* The token. */
	token = parse_peek(shader, ahead);
	if (token->kind == GLSL_TOKEN_PUNCT && token->punct == punct)
		return 1;

	/* Something else. */
	return 0;
}

/* Takes a punctuator that must come next, or ends the compile with a syntax error. */
static void
parse_expect(
	struct glsl_shader *shader,
	unsigned punct,
	const char *what)
{
	int is_punct;

	/* The punctuator, taken. */
	is_punct = parse_punct_at(shader, 0U, punct);
	if (is_punct) {
		(void)parse_take(shader);
		return;
	}

	/* Anything else is a syntax error. */
	parse_unexpected(shader, what);
}

/* Ends the compile with a syntax error at the current token, saying what was expected. */
static void
parse_unexpected(
	struct glsl_shader *shader,
	const char *what)
{
	struct glsl_token *token;

	/* The end of the source. */
	token = parse_peek(shader, 0U);
	if (token->kind == GLSL_TOKEN_EOF)
		glsl_fatal(shader, token->line, "syntax error: expected %s at the end of the shader", what);

	/* A token out of place. */
	glsl_fatal(shader, token->line, "syntax error: expected %s before '%.*s'", what, (int)token->length, token->text);
}

/* Takes an identifier that must come next and returns its name (arena), or ends the compile. */
static const char *
parse_identifier(
	struct glsl_shader *shader,
	const char *what)
{
	struct glsl_token *token;
	unsigned keyword;
	char *name;

	/* An identifier that is not a keyword. */
	token = parse_peek(shader, 0U);
	keyword = parse_classify(shader, token);
	if (token->kind != GLSL_TOKEN_IDENTIFIER)
		parse_unexpected(shader, what);

	/* A reserved word cannot be a name. */
	if (keyword == GLSL_K_RESERVED)
		glsl_fatal(shader, token->line, "'%.*s' is a reserved word", (int)token->length, token->text);
	if (keyword != GLSL_K_NONE)
		parse_unexpected(shader, what);

	/* The name. */
	(void)parse_take(shader);
	name = glsl_strndup(&shader->arena, token->text, token->length);

	/* Succeeded: the name. */
	return name;
}

/* Makes a node of a kind on a line. */
static struct glsl_node *
parse_node(
	struct glsl_shader *shader,
	unsigned kind,
	unsigned line)
{
	struct glsl_node *node;

	/* The node (zeroed by the arena). */
	node = glsl_alloc(&shader->arena, sizeof(*node));
	node->kind = kind;
	node->line = line;

	/* Succeeded: the node. */
	return node;
}

/* Appends a node to a list given by its first and last nodes. */
static void
parse_append(
	struct glsl_node **first,
	struct glsl_node **last,
	struct glsl_node *node)
{
	/* The node becomes the first, or follows the last. */
	if (*last == NULL) {
		*first = node;
	} else {
		(*last)->next = node;
	}

	/* It is the last now. */
	*last = node;
}

/* Opens a scope of type names. */
static void
parse_push_scope(
	struct glsl_shader *shader)
{
	struct glsl_parse_scope *scope;

	/* The scope, inside the current one. */
	scope = glsl_alloc(&shader->arena, sizeof(*scope));
	scope->parent = shader->type_scope;
	shader->type_scope = scope;
}

/* Closes the innermost scope of type names. */
static void
parse_pop_scope(
	struct glsl_shader *shader)
{
	/* The scope around it becomes current. */
	shader->type_scope = shader->type_scope->parent;
}

/* Makes a struct name a type name in the innermost scope. */
static void
parse_add_type_name(
	struct glsl_shader *shader,
	const char *name)
{
	struct glsl_parse_name *entry;

	/* The entry, first in the scope's list. */
	entry = glsl_alloc(&shader->arena, sizeof(*entry));
	entry->name = name;
	entry->length = (unsigned)strlen(name);
	entry->next = shader->type_scope->names;
	shader->type_scope->names = entry;
}

/* Reports whether an identifier token names a struct type in scope. */
static int
parse_is_type_name(
	struct glsl_shader *shader,
	const struct glsl_token *token)
{
	struct glsl_parse_scope *scope;
	struct glsl_parse_name *entry;
	int differs;

	/* Only identifiers name types. */
	if (token->kind != GLSL_TOKEN_IDENTIFIER)
		return 0;

	/* The scopes from the innermost out. */
	for (scope = shader->type_scope; scope != NULL; scope = scope->parent) {
		for (entry = scope->names; entry != NULL; entry = entry->next) {
			if (entry->length != token->length)
				continue;
			differs = memcmp(entry->name, token->text, token->length);
			if (differs == 0)
				return 1;
		}
	}

	/* Not a type name. */
	return 0;
}

/* Reports whether the token some places ahead starts a type specifier (a built-in type, struct, or a struct name). */
static int
parse_starts_type(
	struct glsl_shader *shader,
	unsigned ahead)
{
	struct glsl_token *token;
	unsigned keyword;
	int named;

	/* A built-in type or the struct keyword. */
	token = parse_peek(shader, ahead);
	keyword = parse_classify(shader, token);
	if (keyword == GLSL_K_TYPE || keyword == GLSL_K_STRUCT)
		return 1;

	/* A struct type's name. */
	if (keyword != GLSL_K_NONE)
		return 0;
	named = parse_is_type_name(shader, token);
	if (named)
		return 1;

	/* Anything else. */
	return 0;
}

/*
 * Reports whether a statement starting at the current token is a
 * declaration rather than an expression: a qualifier, or a type followed
 * by a name (a type followed by "(" or "[...](" is a constructor).
 */
static int
parse_starts_declaration(
	struct glsl_shader *shader)
{
	unsigned keyword;
	unsigned ahead;
	unsigned depth;
	int qualifier;
	int starts_type;
	int bracket;
	struct glsl_token *token;

	/* A qualifier, or the precision statement. */
	keyword = parse_keyword_at(shader, 0U);
	qualifier = parse_is_qualifier(keyword);
	if (qualifier || keyword == GLSL_K_PRECISION || keyword == GLSL_K_STRUCT)
		return 1;

	/* Otherwise a type. */
	starts_type = parse_starts_type(shader, 0U);
	if (!starts_type)
		return 0;

	/* A type then "[ ... ]": a declaration when a name follows the brackets. */
	ahead = 1U;
	bracket = parse_punct_at(shader, 1U, GLSL_P_LBRACKET);
	if (bracket) {
		depth = 0U;
		for (;;) {
			token = parse_peek(shader, ahead);
			if (token->kind == GLSL_TOKEN_EOF)
				return 0;
			if (token->kind == GLSL_TOKEN_PUNCT && token->punct == GLSL_P_LBRACKET)
				depth++;
			if (token->kind == GLSL_TOKEN_PUNCT && token->punct == GLSL_P_RBRACKET)
				depth--;
			ahead++;
			if (depth == 0U)
				break;
		}
	}

	/* A name after the type makes a declaration. */
	token = parse_peek(shader, ahead);
	keyword = parse_classify(shader, token);
	if (token->kind == GLSL_TOKEN_IDENTIFIER && keyword == GLSL_K_NONE)
		return 1;

	/* Anything else (a constructor's "(") is an expression. */
	return 0;
}

/* Reports whether a keyword is a qualifier of a declaration. */
static int
parse_is_qualifier(
	unsigned keyword)
{
	/* The storage, interpolation, precision and invariance qualifiers. */
	switch (keyword) {
	case GLSL_K_CONST:
	case GLSL_K_ATTRIBUTE:
	case GLSL_K_UNIFORM:
	case GLSL_K_VARYING:
	case GLSL_K_IN:
	case GLSL_K_OUT:
	case GLSL_K_INOUT:
	case GLSL_K_CENTROID:
	case GLSL_K_FLAT:
	case GLSL_K_SMOOTH:
	case GLSL_K_NOPERSPECTIVE:
	case GLSL_K_INVARIANT:
	case GLSL_K_LOWP:
	case GLSL_K_MEDIUMP:
	case GLSL_K_HIGHP:
	case GLSL_K_BUFFER:
	case GLSL_K_SHARED:
	case GLSL_K_COHERENT:
	case GLSL_K_VOLATILE:
	case GLSL_K_RESTRICT:
	case GLSL_K_READONLY:
	case GLSL_K_WRITEONLY:
	case GLSL_K_PATCH:
	case GLSL_K_PRECISE:
		return 1;
	default:
		break;
	}

	/* Anything else. */
	return 0;
}

/*
 * Parses an external declaration: a precision statement, an invariant
 * redeclaration, a function prototype or definition, or a declaration.
 */
static struct glsl_node *
parse_external(
	struct glsl_shader *shader)
{
	struct glsl_node *type;
	struct glsl_node *declaration;
	struct glsl_token *token;
	const char *name;
	unsigned keyword;
	unsigned line;
	int is_function;
	int alone;
	int block;

	/* The precision statement. */
	keyword = parse_keyword_at(shader, 0U);
	if (keyword == GLSL_K_PRECISION) {
		declaration = parse_precision(shader);
		return declaration;
	}

	/* "invariant NAME, ...;" redeclares varyings invariant, "precise NAME, ...;" variables precise. */
	if (keyword == GLSL_K_INVARIANT || keyword == GLSL_K_PRECISE) {
		token = parse_peek(shader, 1U);
		keyword = parse_classify(shader, token);
		if (token->kind == GLSL_TOKEN_IDENTIFIER && keyword == GLSL_K_NONE) {
			declaration = parse_invariant(shader);
			return declaration;
		}
	}

	/* A subroutine type, a subroutine, or subroutine uniforms (GLSL 4.00, GL_ARB_shader_subroutine). */
	keyword = parse_keyword_at(shader, 0U);
	if (keyword == GLSL_K_SUBROUTINE) {
		declaration = parse_subroutine(shader);
		return declaration;
	}

	/* An interface block. */
	block = parse_starts_block(shader);
	if (block == 1) {
		declaration = parse_interface(shader);
		return declaration;
	}

	/* Qualifiers alone: a default layout, which changes nothing here. */
	if (block == 2) {
		token = parse_peek(shader, 0U);
		declaration = parse_node(shader, GLSL_N_EMPTY, token->line);
		declaration->child[0] = parse_node(shader, GLSL_N_TYPE, token->line);
		parse_qualifiers(shader, declaration->child[0]);
		parse_expect(shader, GLSL_P_SEMICOLON, "';'");
		return declaration;
	}

	/* A type with its qualifiers. */
	token = parse_peek(shader, 0U);
	line = token->line;
	type = parse_fully_specified_type(shader);

	/* The type alone: a struct's definition. */
	alone = parse_punct_at(shader, 0U, GLSL_P_SEMICOLON);
	if (alone) {
		(void)parse_take(shader);
		declaration = parse_node(shader, GLSL_N_DECLARATION, line);
		declaration->child[0] = type;
		return declaration;
	}

	/* A name, then "(" for a function. */
	token = parse_peek(shader, 0U);
	line = token->line;
	name = parse_identifier(shader, "a name");
	is_function = parse_punct_at(shader, 0U, GLSL_P_LPAREN);
	if (is_function) {
		declaration = parse_function(shader, type, name, line);
		return declaration;
	}

	/* A declaration of variables. */
	declaration = parse_declaration_rest(shader, type, name, line);
	return declaration;
}

/*
 * Parses what follows "subroutine" (GLSL 4.00, GL_ARB_shader_subroutine):
 * "uniform T NAME ...;" declares subroutine uniforms of the subroutine
 * type T (the type node's name), "(T, ...)" before a function definition
 * makes it a subroutine of those types, and a prototype alone declares a
 * subroutine type of its name.
 */
static struct glsl_node *
parse_subroutine(
	struct glsl_shader *shader)
{
	struct glsl_node *type;
	struct glsl_node *declaration;
	struct glsl_node *names;
	struct glsl_node *last;
	struct glsl_node *named;
	struct glsl_token *token;
	const char *name;
	unsigned keyword;
	int listed;
	int ends;
	int is_function;

	/* The keyword. */
	token = parse_take(shader);

	/* Subroutine uniforms: their type is the subroutine type's name. */
	keyword = parse_keyword_at(shader, 0U);
	if (keyword == GLSL_K_UNIFORM) {
		(void)parse_take(shader);
		type = parse_node(shader, GLSL_N_TYPE, token->line);
		type->storage = GLSL_STORAGE_UNIFORM;
		type->name = parse_identifier(shader, "a subroutine type");
		token = parse_peek(shader, 0U);
		name = parse_identifier(shader, "a name");
		declaration = parse_declaration_rest(shader, type, name, token->line);
		declaration->flags |= GLSL_NODE_SUBROUTINE_UNIFORM;
		return declaration;
	}

	/* The subroutine types a subroutine is one of, separated by commas. */
	names = NULL;
	last = NULL;
	listed = parse_punct_at(shader, 0U, GLSL_P_LPAREN);
	if (listed) {
		(void)parse_take(shader);
		for (;;) {
			token = parse_peek(shader, 0U);
			named = parse_node(shader, GLSL_N_IDENTIFIER, token->line);
			named->name = parse_identifier(shader, "a subroutine type");
			parse_append(&names, &last, named);

			/* A comma goes on, ")" ends. */
			ends = parse_punct_at(shader, 0U, GLSL_P_RPAREN);
			if (ends)
				break;
			parse_expect(shader, GLSL_P_COMMA, "',' or ')'");
		}

		/* The ")". */
		(void)parse_take(shader);
	}

	/* The function: its type, its name and "(". */
	type = parse_fully_specified_type(shader);
	token = parse_peek(shader, 0U);
	name = parse_identifier(shader, "a name");
	is_function = parse_punct_at(shader, 0U, GLSL_P_LPAREN);
	if (!is_function)
		parse_unexpected(shader, "'(' of a subroutine");
	declaration = parse_function(shader, type, name, token->line);

	/* A subroutine of the types listed. */
	if (listed) {
		declaration->flags |= GLSL_NODE_SUBROUTINE;
		declaration->child[3] = names;
		return declaration;
	}

	/* Succeeded: a subroutine type's declaration. */
	declaration->flags |= GLSL_NODE_SUBROUTINE_TYPE;
	return declaration;
}

/* Parses "precision QUALIFIER TYPE;". */
static struct glsl_node *
parse_precision(
	struct glsl_shader *shader)
{
	struct glsl_node *node;
	struct glsl_token *token;
	unsigned keyword;

	/* The keyword. */
	token = parse_take(shader);
	node = parse_node(shader, GLSL_N_PRECISION, token->line);

	/* The precision. */
	keyword = parse_keyword_at(shader, 0U);
	if (keyword == GLSL_K_LOWP) {
		node->precision = GLSL_PRECISION_LOW;
	} else if (keyword == GLSL_K_MEDIUMP) {
		node->precision = GLSL_PRECISION_MEDIUM;
	} else if (keyword == GLSL_K_HIGHP) {
		node->precision = GLSL_PRECISION_HIGH;
	} else {
		parse_unexpected(shader, "a precision qualifier");
	}

	/* The type it applies to, and the end. */
	(void)parse_take(shader);
	node->child[0] = parse_node(shader, GLSL_N_TYPE, token->line);
	parse_type_specifier(shader, node->child[0]);
	parse_expect(shader, GLSL_P_SEMICOLON, "';'");

	/* Succeeded: the statement. */
	return node;
}

/* Parses "invariant NAME, NAME ...;" or "precise NAME, NAME ...;" (the node's precise says which). */
static struct glsl_node *
parse_invariant(
	struct glsl_shader *shader)
{
	struct glsl_node *node;
	struct glsl_node *name;
	struct glsl_node *last;
	struct glsl_token *token;
	unsigned keyword;
	int more;

	/* The keyword; anything precise keeps the whole shader's operations uncontracted. */
	keyword = parse_keyword_at(shader, 0U);
	token = parse_take(shader);
	node = parse_node(shader, GLSL_N_INVARIANT, token->line);
	if (keyword == GLSL_K_PRECISE) {
		node->precise = 1U;
		shader->precise = 1U;
	}

	/* The names, separated by commas. */
	last = NULL;
	for (;;) {
		token = parse_peek(shader, 0U);
		name = parse_node(shader, GLSL_N_IDENTIFIER, token->line);
		name->name = parse_identifier(shader, "a name");
		parse_append(&node->child[0], &last, name);

		/* A comma goes on. */
		more = parse_punct_at(shader, 0U, GLSL_P_COMMA);
		if (!more)
			break;
		(void)parse_take(shader);
	}

	/* The end. */
	parse_expect(shader, GLSL_P_SEMICOLON, "';'");
	return node;
}

/* Parses qualifiers and a type specifier into a GLSL_N_TYPE node. */
static struct glsl_node *
parse_fully_specified_type(
	struct glsl_shader *shader)
{
	struct glsl_node *type;
	struct glsl_token *token;

	/* The node on the first token's line. */
	token = parse_peek(shader, 0U);
	type = parse_node(shader, GLSL_N_TYPE, token->line);

	/* The qualifiers, then the type. */
	parse_qualifiers(shader, type);
	parse_type_specifier(shader, type);

	/* Succeeded: the type. */
	return type;
}

/* Parses the qualifiers before a type into a type node. */
static void
parse_qualifiers(
	struct glsl_shader *shader,
	struct glsl_node *type)
{
	struct glsl_token *token;
	unsigned keyword;
	unsigned storage;

	/* Qualifiers in any order; the checker judges their combination. */
	for (;;) {
		token = parse_peek(shader, 0U);
		keyword = parse_classify(shader, token);
		storage = GLSL_STORAGE_NONE;

		/* The keyword's meaning. */
		switch (keyword) {
		case GLSL_K_CONST:
			storage = GLSL_STORAGE_CONST;
			break;
		case GLSL_K_ATTRIBUTE:
			storage = GLSL_STORAGE_ATTRIBUTE;
			break;
		case GLSL_K_UNIFORM:
			storage = GLSL_STORAGE_UNIFORM;
			break;
		case GLSL_K_VARYING:
			storage = GLSL_STORAGE_VARYING;
			break;
		case GLSL_K_IN:
			storage = GLSL_STORAGE_IN;
			break;
		case GLSL_K_OUT:
			storage = GLSL_STORAGE_OUT;
			break;
		case GLSL_K_INOUT:
			storage = GLSL_STORAGE_INOUT;
			break;
		case GLSL_K_CENTROID:
			type->centroid = 1U;
			break;
		case GLSL_K_FLAT:
			type->interpolation = GLSL_INTERP_FLAT;
			break;
		case GLSL_K_SMOOTH:
			type->interpolation = GLSL_INTERP_SMOOTH;
			break;
		case GLSL_K_NOPERSPECTIVE:
			type->interpolation = GLSL_INTERP_NOPERSPECTIVE;
			break;
		case GLSL_K_INVARIANT:
			type->invariant = 1U;
			break;
		case GLSL_K_PRECISE:
			/* The shader's operations are then not contracted. */
			type->precise = 1U;
			shader->precise = 1U;
			break;
		case GLSL_K_LOWP:
			type->precision = GLSL_PRECISION_LOW;
			break;
		case GLSL_K_MEDIUMP:
			type->precision = GLSL_PRECISION_MEDIUM;
			break;
		case GLSL_K_HIGHP:
			type->precision = GLSL_PRECISION_HIGH;
			break;
		case GLSL_K_BUFFER:
			storage = GLSL_STORAGE_BUFFER;
			break;
		case GLSL_K_SHARED:
			storage = GLSL_STORAGE_SHARED;
			break;
		case GLSL_K_COHERENT:
			type->memory |= GLSL_MEMORY_COHERENT;
			break;
		case GLSL_K_VOLATILE:
			type->memory |= GLSL_MEMORY_VOLATILE;
			break;
		case GLSL_K_RESTRICT:
			type->memory |= GLSL_MEMORY_RESTRICT;
			break;
		case GLSL_K_READONLY:
			type->memory |= GLSL_MEMORY_READONLY;
			break;
		case GLSL_K_WRITEONLY:
			type->memory |= GLSL_MEMORY_WRITEONLY;
			break;
		case GLSL_K_PATCH:
			type->patch = 1U;
			break;
		case GLSL_K_LAYOUT:
			parse_layout(shader, type);
			continue;
		default:
			return;
		}

		/* A second storage qualifier (other than "const in") is an error. */
		if (storage != GLSL_STORAGE_NONE) {
			if (type->storage == GLSL_STORAGE_CONST && storage == GLSL_STORAGE_IN) {
				type->flags |= GLSL_NODE_CONST_PARAM;
			} else if (type->storage != GLSL_STORAGE_NONE) {
				glsl_error(shader, token->line, "more than one storage qualifier");
			} else {
				type->storage = storage;
			}
		}

		/* The qualifier is taken. */
		(void)parse_take(shader);
	}
}

/*
 * Parses "layout ( qualifier [= value], ... )" into a type node: the
 * block layouts, the matrix orders, a location, a geometry shader's
 * primitives, most vertices, invocations and outputs' stream, and GLSL ES 3.10's std430, binding and a
 * compute shader's workgroup size (ws101-p008).
 */
static void
parse_layout(
	struct glsl_shader *shader,
	struct glsl_node *type)
{
	struct glsl_token *token;
	struct glsl_token *value;
	int is_std140;
	int is_shared;
	int is_packed;
	int is_row;
	int is_column;
	int is_location;
	int is_index;
	int is_max;
	int is_std430;
	int is_binding;
	int is_vertices;
	int is_point_mode;
	int is_invocations;
	int is_stream;
	int closes;
	unsigned spacing;
	unsigned order;
	unsigned primitive;
	unsigned axis;
	unsigned format;

	/* The keyword and "(". */
	(void)parse_take(shader);
	parse_expect(shader, GLSL_P_LPAREN, "'('");

	/* The qualifiers separated by commas. */
	for (;;) {
		token = parse_take(shader);
		if (token->kind != GLSL_TOKEN_IDENTIFIER)
			parse_unexpected(shader, "a layout qualifier");

		/* Which one it is. */
		is_std140 = glsl_token_is(token, "std140");
		is_shared = glsl_token_is(token, "shared");
		is_packed = glsl_token_is(token, "packed");
		is_row = glsl_token_is(token, "row_major");
		is_column = glsl_token_is(token, "column_major");
		is_location = glsl_token_is(token, "location");
		is_index = glsl_token_is(token, "index");
		is_max = glsl_token_is(token, "max_vertices");
		is_std430 = glsl_token_is(token, "std430");
		is_binding = glsl_token_is(token, "binding");
		is_vertices = glsl_token_is(token, "vertices");
		is_point_mode = glsl_token_is(token, "point_mode");
		is_invocations = glsl_token_is(token, "invocations");
		is_stream = glsl_token_is(token, "stream");
		spacing = parse_spacing(token);
		order = parse_order(token);
		axis = parse_local_size(token);
		primitive = parse_primitive(token);
		format = parse_image_format(token);
		if (format != 0U) {
			/* An image's format (GLSL ES 3.10). */
			type->layout |= GLSL_LAYOUT_FORMAT;
			type->image_format = format;
		} else if (axis < 3U) {
			/* local_size_x, _y or _z = an integer (ws101-p008). */
			parse_expect(shader, GLSL_P_ASSIGN, "'='");
			value = parse_take(shader);
			if (value->kind != GLSL_TOKEN_INT && value->kind != GLSL_TOKEN_UINT)
				glsl_fatal(shader, value->line, "a workgroup size must be an integer");
			type->layout |= GLSL_LAYOUT_LOCAL_SIZE;
			type->local_size[axis] = value->integer;
		} else if (is_binding) {
			/* binding = an integer (ws101-p008). */
			parse_expect(shader, GLSL_P_ASSIGN, "'='");
			value = parse_take(shader);
			if (value->kind != GLSL_TOKEN_INT && value->kind != GLSL_TOKEN_UINT)
				glsl_fatal(shader, value->line, "a layout binding must be an integer");
			type->layout |= GLSL_LAYOUT_BINDING;
			type->binding = value->integer;
		} else if (is_vertices) {
			/* vertices = an integer (a tessellation control shader's output patch). */
			parse_expect(shader, GLSL_P_ASSIGN, "'='");
			value = parse_take(shader);
			if (value->kind != GLSL_TOKEN_INT && value->kind != GLSL_TOKEN_UINT)
				glsl_fatal(shader, value->line, "vertices must be an integer");
			type->layout |= GLSL_LAYOUT_VERTICES;
			type->vertices = value->integer;
		} else if (spacing != 0U) {
			/* A tessellation evaluation shader's spacing. */
			type->layout |= GLSL_LAYOUT_SPACING;
			type->spacing = spacing;
		} else if (order != 0U) {
			/* A tessellation evaluation shader's vertex order. */
			type->layout |= GLSL_LAYOUT_ORDER;
			type->order = order;
		} else if (is_invocations) {
			/* invocations = an integer (a geometry shader's, GL_ARB_gpu_shader5). */
			parse_expect(shader, GLSL_P_ASSIGN, "'='");
			value = parse_take(shader);
			if (value->kind != GLSL_TOKEN_INT && value->kind != GLSL_TOKEN_UINT)
				glsl_fatal(shader, value->line, "invocations must be an integer");
			type->layout |= GLSL_LAYOUT_INVOCATIONS;
			type->invocations = value->integer;
		} else if (is_stream) {
			/* stream = an integer (a geometry shader's outputs', GL_ARB_gpu_shader5). */
			parse_expect(shader, GLSL_P_ASSIGN, "'='");
			value = parse_take(shader);
			if (value->kind != GLSL_TOKEN_INT && value->kind != GLSL_TOKEN_UINT)
				glsl_fatal(shader, value->line, "stream must be an integer");
			type->layout |= GLSL_LAYOUT_STREAM;
			type->stream = value->integer;
		} else if (is_point_mode) {
			type->layout |= GLSL_LAYOUT_POINT_MODE;
			type->point_mode = 1U;
		} else if (is_std430) {
			type->layout |= GLSL_LAYOUT_STD430;
		} else if (primitive != GLSL_PRIMITIVE_NONE) {
			/* A geometry shader's input or output primitive. */
			type->layout |= GLSL_LAYOUT_PRIMITIVE;
			type->primitive = primitive;
		} else if (is_max) {
			/* max_vertices = an integer. */
			parse_expect(shader, GLSL_P_ASSIGN, "'='");
			value = parse_take(shader);
			if (value->kind != GLSL_TOKEN_INT && value->kind != GLSL_TOKEN_UINT)
				glsl_fatal(shader, value->line, "max_vertices must be an integer");
			type->layout |= GLSL_LAYOUT_MAX_VERTICES;
			type->max_vertices = value->integer;
		} else if (is_std140) {
			type->layout |= GLSL_LAYOUT_STD140;
		} else if (is_shared) {
			type->layout |= GLSL_LAYOUT_SHARED;
		} else if (is_packed) {
			type->layout |= GLSL_LAYOUT_PACKED;
		} else if (is_row) {
			type->layout |= GLSL_LAYOUT_ROW_MAJOR;
		} else if (is_column) {
			type->layout |= GLSL_LAYOUT_COLUMN_MAJOR;
		} else if (is_location) {
			/* location = an integer. */
			parse_expect(shader, GLSL_P_ASSIGN, "'='");
			value = parse_take(shader);
			if (value->kind != GLSL_TOKEN_INT && value->kind != GLSL_TOKEN_UINT)
				glsl_fatal(shader, value->line, "a layout location must be an integer");
			type->layout |= GLSL_LAYOUT_LOCATION;
			type->location = value->integer;
		} else if (is_index) {
			/* index = an integer (desktop GLSL 3.30's dual-source blending). */
			parse_expect(shader, GLSL_P_ASSIGN, "'='");
			value = parse_take(shader);
			if (value->kind != GLSL_TOKEN_INT && value->kind != GLSL_TOKEN_UINT)
				glsl_fatal(shader, value->line, "a layout index must be an integer");
			type->layout |= GLSL_LAYOUT_INDEX;
			type->output_index = value->integer;
		} else {
			glsl_error(shader, token->line, "unknown layout qualifier '%.*s'", (int)token->length, token->text);
		}

		/* A comma goes on, ")" ends. */
		closes = parse_punct_at(shader, 0U, GLSL_P_RPAREN);
		if (closes)
			break;
		parse_expect(shader, GLSL_P_COMMA, "',' or ')'");
	}

	/* The ")". */
	(void)parse_take(shader);
}

/* Returns the axis (0 x, 1 y, 2 z) a workgroup size layout qualifier names, 3 for another qualifier (ws101-p008). */
static unsigned
parse_local_size(
	const struct glsl_token *token)
{
	int same;

	/* local_size_x, local_size_y, local_size_z. */
	same = glsl_token_is(token, "local_size_x");
	if (same)
		return 0U;
	same = glsl_token_is(token, "local_size_y");
	if (same)
		return 1U;
	same = glsl_token_is(token, "local_size_z");
	if (same)
		return 2U;

	/* Not a workgroup size. */
	return 3U;
}

/* Returns the image format (GLSL_IMAGE_FORMAT_*) a layout qualifier names, 0 for another qualifier (GLSL ES 3.10). */
static unsigned
parse_image_format(
	const struct glsl_token *token)
{
	static const struct {
		const char *name;
		unsigned format;
	} formats[] = {
		{ "rgba32f", GLSL_IMAGE_FORMAT_RGBA32F },
		{ "rgba16f", GLSL_IMAGE_FORMAT_RGBA16F },
		{ "r32f", GLSL_IMAGE_FORMAT_R32F },
		{ "rgba8", GLSL_IMAGE_FORMAT_RGBA8 },
		{ "rgba8_snorm", GLSL_IMAGE_FORMAT_RGBA8_SNORM },
		{ "rgba32i", GLSL_IMAGE_FORMAT_RGBA32I },
		{ "rgba16i", GLSL_IMAGE_FORMAT_RGBA16I },
		{ "rgba8i", GLSL_IMAGE_FORMAT_RGBA8I },
		{ "r32i", GLSL_IMAGE_FORMAT_R32I },
		{ "rgba32ui", GLSL_IMAGE_FORMAT_RGBA32UI },
		{ "rgba16ui", GLSL_IMAGE_FORMAT_RGBA16UI },
		{ "rgba8ui", GLSL_IMAGE_FORMAT_RGBA8UI },
		{ "r32ui", GLSL_IMAGE_FORMAT_R32UI }
	};
	size_t index;
	int same;

	/* The names of GLSL ES 3.10's formats. */
	for (index = 0U; index < sizeof(formats) / sizeof(formats[0]); index++) {
		same = glsl_token_is(token, formats[index].name);
		if (same)
			return formats[index].format;
	}

	/* Not a format. */
	return 0U;
}

/* Returns the geometry primitive (GLSL_PRIMITIVE_*) a layout qualifier names, GLSL_PRIMITIVE_NONE for another qualifier. */
static unsigned
parse_primitive(
	const struct glsl_token *token)
{
	/*
	 * One primitive's layout name and number.
	 */
	static const struct {
		const char *name;
		unsigned primitive;
	} primitives[] = {
		{ "points", GLSL_PRIMITIVE_POINTS },
		{ "lines", GLSL_PRIMITIVE_LINES },
		{ "lines_adjacency", GLSL_PRIMITIVE_LINES_ADJACENCY },
		{ "triangles", GLSL_PRIMITIVE_TRIANGLES },
		{ "triangles_adjacency", GLSL_PRIMITIVE_TRIANGLES_ADJACENCY },
		{ "line_strip", GLSL_PRIMITIVE_LINE_STRIP },
		{ "triangle_strip", GLSL_PRIMITIVE_TRIANGLE_STRIP },
		{ "quads", GLSL_PRIMITIVE_QUADS },
		{ "isolines", GLSL_PRIMITIVE_ISOLINES }
	};
	unsigned index;
	int same;

	/* The name among them. */
	for (index = 0U; index < sizeof(primitives) / sizeof(primitives[0]); index++) {
		same = glsl_token_is(token, primitives[index].name);
		if (same)
			return primitives[index].primitive;
	}

	/* Not a primitive. */
	return GLSL_PRIMITIVE_NONE;
}

/* Returns the spacing (GLSL_SPACING_*) a tessellation layout qualifier names, 0 for another qualifier. */
static unsigned
parse_spacing(
	const struct glsl_token *token)
{
	int same;

	/* equal_spacing, fractional_even_spacing, fractional_odd_spacing. */
	same = glsl_token_is(token, "equal_spacing");
	if (same)
		return GLSL_SPACING_EQUAL;
	same = glsl_token_is(token, "fractional_even_spacing");
	if (same)
		return GLSL_SPACING_FRACTIONAL_EVEN;
	same = glsl_token_is(token, "fractional_odd_spacing");
	if (same)
		return GLSL_SPACING_FRACTIONAL_ODD;

	/* Not a spacing. */
	return 0U;
}

/* Returns the vertex order (GLSL_ORDER_*) a tessellation layout qualifier names, 0 for another qualifier. */
static unsigned
parse_order(
	const struct glsl_token *token)
{
	int same;

	/* cw or ccw. */
	same = glsl_token_is(token, "cw");
	if (same)
		return GLSL_ORDER_CW;
	same = glsl_token_is(token, "ccw");
	if (same)
		return GLSL_ORDER_CCW;

	/* Not an order. */
	return 0U;
}

/*
 * Reports what a declaration starting at the current token is, looking
 * past its qualifiers: 1 an interface block ("NAME {"), 2 qualifiers
 * alone ("layout(std140) uniform;"), 0 anything else.
 */
static int
parse_starts_block(
	struct glsl_shader *shader)
{
	struct glsl_token *token;
	unsigned keyword;
	unsigned ahead;
	unsigned depth;
	int qualifier;
	int opens;

	/* Past the qualifiers and layouts. */
	ahead = 0U;
	for (;;) {
		keyword = parse_keyword_at(shader, ahead);
		qualifier = parse_is_qualifier(keyword);
		if (qualifier) {
			ahead++;
			continue;
		}

		/* Anything but a layout ends the qualifiers. */
		if (keyword != GLSL_K_LAYOUT)
			break;

		/* A layout's parenthesized list. */
		ahead++;
		depth = 0U;
		for (;;) {
			token = parse_peek(shader, ahead);
			if (token->kind == GLSL_TOKEN_EOF)
				return 0;
			ahead++;
			if (token->kind == GLSL_TOKEN_PUNCT && token->punct == GLSL_P_LPAREN)
				depth++;
			if (token->kind == GLSL_TOKEN_PUNCT && token->punct == GLSL_P_RPAREN)
				depth--;
			if (depth == 0U)
				break;
		}
	}

	/* Nothing after the qualifiers but ";". */
	if (ahead > 0U) {
		opens = parse_punct_at(shader, ahead, GLSL_P_SEMICOLON);
		if (opens)
			return 2;
	}

	/* A name that is not a type, then "{". */
	token = parse_peek(shader, ahead);
	keyword = parse_classify(shader, token);
	if (ahead == 0U || token->kind != GLSL_TOKEN_IDENTIFIER || keyword != GLSL_K_NONE)
		return 0;
	opens = parse_punct_at(shader, ahead + 1U, GLSL_P_LBRACE);
	if (!opens)
		return 0;

	/* Succeeded: a block. */
	return 1;
}

/* Parses an interface block: qualifiers, its name, "{ members }", an optional instance name and array size, ";". */
static struct glsl_node *
parse_interface(
	struct glsl_shader *shader)
{
	struct glsl_node *node;
	struct glsl_node *member;
	struct glsl_node *member_type;
	struct glsl_node *last;
	struct glsl_token *token;
	const char *name;
	int ends;
	int bracket;

	/* The qualifiers and the name. */
	token = parse_peek(shader, 0U);
	node = parse_node(shader, GLSL_N_INTERFACE, token->line);
	node->child[0] = parse_node(shader, GLSL_N_TYPE, token->line);
	parse_qualifiers(shader, node->child[0]);
	node->name = parse_identifier(shader, "a block name");

	/* The members up to "}". */
	parse_expect(shader, GLSL_P_LBRACE, "'{'");
	last = NULL;
	for (;;) {
		ends = parse_punct_at(shader, 0U, GLSL_P_RBRACE);
		if (ends)
			break;
		token = parse_peek(shader, 0U);
		member_type = parse_fully_specified_type(shader);
		name = parse_identifier(shader, "a member name");
		member = parse_declaration_rest(shader, member_type, name, token->line);
		parse_append(&node->child[1], &last, member);
	}

	/* The "}". */
	(void)parse_take(shader);

	/* An instance name, with an array size. */
	token = parse_peek(shader, 0U);
	if (token->kind == GLSL_TOKEN_IDENTIFIER) {
		node->child[2] = parse_node(shader, GLSL_N_VARIABLE, token->line);
		node->child[2]->name = parse_identifier(shader, "an instance name");
		bracket = parse_punct_at(shader, 0U, GLSL_P_LBRACKET);
		if (bracket)
			node->child[2]->child[0] = parse_array_size(shader, node->child[2]);
	}

	/* The end. */
	parse_expect(shader, GLSL_P_SEMICOLON, "';'");
	return node;
}

/* Parses a type specifier (a built-in type, a struct definition or a struct name, then optional "[size]"). */
static void
parse_type_specifier(
	struct glsl_shader *shader,
	struct glsl_node *type)
{
	struct glsl_token *token;
	unsigned keyword;
	int named;
	int bracket;

	/* A built-in type. */
	token = parse_peek(shader, 0U);
	keyword = parse_classify(shader, token);
	if (keyword == GLSL_K_TYPE) {
		(void)parse_take(shader);
		type->type = token->keyword_type;
	} else if (keyword == GLSL_K_STRUCT) {
		/* A struct definition. */
		parse_struct(shader, type);
	} else {
		/* A struct's name (a reserved word is said to be one). */
		named = 0;
		if (keyword == GLSL_K_NONE)
			named = parse_is_type_name(shader, token);
		if (keyword == GLSL_K_RESERVED)
			glsl_fatal(shader, token->line, "'%.*s' is a reserved word", (int)token->length, token->text);
		if (!named)
			parse_unexpected(shader, "a type");
		(void)parse_take(shader);
		type->name = glsl_strndup(&shader->arena, token->text, token->length);
	}

	/* "[size]" after the type makes an array type (1.20 on). */
	bracket = parse_punct_at(shader, 0U, GLSL_P_LBRACKET);
	if (bracket)
		type->child[1] = parse_array_size(shader, type);
}

/* Parses "struct [NAME] { members }" into a type node. */
static void
parse_struct(
	struct glsl_shader *shader,
	struct glsl_node *type)
{
	struct glsl_node *member;
	struct glsl_node *last;
	struct glsl_node *member_type;
	struct glsl_token *token;
	const char *name;
	int ends;

	/* The keyword and the optional name. */
	(void)parse_take(shader);
	type->flags |= GLSL_NODE_STRUCT_DEFINITION;
	token = parse_peek(shader, 0U);
	if (token->kind == GLSL_TOKEN_IDENTIFIER) {
		type->name = parse_identifier(shader, "a struct name");
		parse_add_type_name(shader, type->name);
	}

	/* The members: declarations of variables without qualifiers but precision. */
	parse_expect(shader, GLSL_P_LBRACE, "'{'");
	last = NULL;
	for (;;) {
		ends = parse_punct_at(shader, 0U, GLSL_P_RBRACE);
		if (ends)
			break;

		/* The member's type, then its names. */
		token = parse_peek(shader, 0U);
		member_type = parse_fully_specified_type(shader);
		if (member_type->storage != GLSL_STORAGE_NONE || member_type->interpolation != GLSL_INTERP_NONE || member_type->invariant)
			glsl_error(shader, token->line, "struct members cannot have qualifiers");
		name = parse_identifier(shader, "a member name");
		member = parse_declaration_rest(shader, member_type, name, token->line);

		/* The member declaration, appended. */
		parse_append(&type->child[0], &last, member);
	}

	/* The end, and at least one member. */
	token = parse_take(shader);
	if (type->child[0] == NULL)
		glsl_error(shader, token->line, "a struct needs at least one member");
}

/* Parses "[ size ]" (or "[]"), marking the owner an array; returns the size expression or NULL. */
static struct glsl_node *
parse_array_size(
	struct glsl_shader *shader,
	struct glsl_node *owner)
{
	struct glsl_node *size;
	int unsized;

	/* The "[". */
	(void)parse_take(shader);
	owner->flags |= GLSL_NODE_ARRAY;

	/* "[]" leaves the size to an initializer. */
	unsized = parse_punct_at(shader, 0U, GLSL_P_RBRACKET);
	if (unsized) {
		(void)parse_take(shader);
		owner->flags |= GLSL_NODE_UNSIZED;
		return NULL;
	}

	/* The size and the "]". */
	size = parse_conditional(shader);
	parse_expect(shader, GLSL_P_RBRACKET, "']'");

	/* Succeeded: the size expression. */
	return size;
}

/*
 * Parses the rest of a declaration after its type and first name: array
 * sizes, initializers, further names, and the ";".
 */
static struct glsl_node *
parse_declaration_rest(
	struct glsl_shader *shader,
	struct glsl_node *type,
	const char *first_name,
	unsigned line)
{
	struct glsl_node *declaration;
	struct glsl_node *variable;
	struct glsl_node *last;
	struct glsl_token *token;
	const char *name;
	int more;
	int present;

	/* The declaration with its type. */
	declaration = parse_node(shader, GLSL_N_DECLARATION, line);
	declaration->child[0] = type;

	/* Each declared name. */
	name = first_name;
	last = NULL;
	for (;;) {
		variable = parse_node(shader, GLSL_N_VARIABLE, line);
		variable->name = name;

		/* An array size. */
		present = parse_punct_at(shader, 0U, GLSL_P_LBRACKET);
		if (present)
			variable->child[0] = parse_array_size(shader, variable);

		/* An initializer. */
		present = parse_punct_at(shader, 0U, GLSL_P_ASSIGN);
		if (present) {
			(void)parse_take(shader);
			variable->child[1] = parse_assignment(shader);
		}

		/* The variable, appended. */
		parse_append(&declaration->child[1], &last, variable);

		/* A comma goes on to the next name. */
		more = parse_punct_at(shader, 0U, GLSL_P_COMMA);
		if (!more)
			break;
		(void)parse_take(shader);
		token = parse_peek(shader, 0U);
		line = token->line;
		name = parse_identifier(shader, "a name");
	}

	/* The end. */
	parse_expect(shader, GLSL_P_SEMICOLON, "';' or ','");
	return declaration;
}

/* Parses a function's parameters and its body (a definition) or ";" (a prototype). */
static struct glsl_node *
parse_function(
	struct glsl_shader *shader,
	struct glsl_node *type,
	const char *name,
	unsigned line)
{
	struct glsl_node *function;
	struct glsl_node *parameter;
	struct glsl_node *last;
	int ends;
	int is_void;
	int prototype;

	/* The function node. */
	function = parse_node(shader, GLSL_N_FUNCTION, line);
	function->name = name;
	function->child[0] = type;

	/* "(" and the parameters' scope of type names. */
	parse_expect(shader, GLSL_P_LPAREN, "'('");
	parse_push_scope(shader);

	/* "(void)" is no parameters. */
	is_void = parse_void_list(shader);
	if (is_void) {
		(void)parse_take(shader);
		function->flags |= GLSL_NODE_VOID_PARAMS;
	}

	/* The parameters, separated by commas, up to ")". */
	last = NULL;
	ends = parse_punct_at(shader, 0U, GLSL_P_RPAREN);
	while (!ends) {
		parameter = parse_parameter(shader);
		parse_append(&function->child[1], &last, parameter);

		/* A comma goes on; anything but ")" is an error. */
		ends = parse_punct_at(shader, 0U, GLSL_P_RPAREN);
		if (!ends)
			parse_expect(shader, GLSL_P_COMMA, "',' or ')'");
	}

	/* The ")". */
	parse_expect(shader, GLSL_P_RPAREN, "')'");

	/* A prototype ends here. */
	prototype = parse_punct_at(shader, 0U, GLSL_P_SEMICOLON);
	if (prototype) {
		(void)parse_take(shader);
		parse_pop_scope(shader);
		return function;
	}

	/* A definition's body shares the parameters' scope. */
	function->child[2] = parse_compound(shader, 0);
	parse_pop_scope(shader);

	/* Succeeded: the definition. */
	return function;
}

/* Parses one parameter: qualifiers, a type, an optional name and array size. */
static struct glsl_node *
parse_parameter(
	struct glsl_shader *shader)
{
	struct glsl_node *parameter;
	struct glsl_token *token;
	unsigned keyword;
	int bracket;

	/* The parameter's type with its qualifiers. */
	token = parse_peek(shader, 0U);
	parameter = parse_node(shader, GLSL_N_PARAMETER, token->line);
	parameter->child[0] = parse_fully_specified_type(shader);

	/* The name, when there is one. */
	token = parse_peek(shader, 0U);
	keyword = parse_classify(shader, token);
	if (token->kind == GLSL_TOKEN_IDENTIFIER && keyword == GLSL_K_NONE)
		parameter->name = parse_identifier(shader, "a parameter name");

	/* An array size after the name. */
	bracket = parse_punct_at(shader, 0U, GLSL_P_LBRACKET);
	if (bracket)
		parameter->child[1] = parse_array_size(shader, parameter);

	/* Succeeded: the parameter. */
	return parameter;
}

/* Parses a statement. */
static struct glsl_node *
parse_statement(
	struct glsl_shader *shader)
{
	struct glsl_node *node;
	struct glsl_token *token;
	unsigned keyword;
	int declaration;

	/* A compound statement opens a scope. */
	token = parse_peek(shader, 0U);
	if (token->kind == GLSL_TOKEN_PUNCT && token->punct == GLSL_P_LBRACE) {
		node = parse_compound(shader, 1);
		return node;
	}

	/* The empty statement. */
	if (token->kind == GLSL_TOKEN_PUNCT && token->punct == GLSL_P_SEMICOLON) {
		(void)parse_take(shader);
		node = parse_node(shader, GLSL_N_EMPTY, token->line);
		return node;
	}

	/* The statements that start with a keyword. */
	keyword = parse_classify(shader, token);
	switch (keyword) {
	case GLSL_K_IF:
		node = parse_if(shader);
		return node;
	case GLSL_K_FOR:
		node = parse_for(shader);
		return node;
	case GLSL_K_WHILE:
		node = parse_while(shader);
		return node;
	case GLSL_K_DO:
		node = parse_do(shader);
		return node;
	case GLSL_K_SWITCH:
		node = parse_switch(shader);
		return node;
	case GLSL_K_CASE:
	case GLSL_K_DEFAULT:
	case GLSL_K_RETURN:
	case GLSL_K_BREAK:
	case GLSL_K_CONTINUE:
	case GLSL_K_DISCARD:
		node = parse_jump(shader, keyword);
		return node;
	case GLSL_K_RESERVED:
		glsl_fatal(shader, token->line, "'%.*s' is a reserved word", (int)token->length, token->text);
		break;
	default:
		break;
	}

	/* A declaration. */
	declaration = parse_starts_declaration(shader);
	if (declaration) {
		node = parse_declaration_statement(shader);
		return node;
	}

	/* An expression statement. */
	node = parse_node(shader, GLSL_N_EXPRESSION, token->line);
	node->child[0] = parse_expression(shader);
	parse_expect(shader, GLSL_P_SEMICOLON, "';'");
	return node;
}

/* Parses "{ statements }", opening a scope of type names when asked. */
static struct glsl_node *
parse_compound(
	struct glsl_shader *shader,
	int new_scope)
{
	struct glsl_node *block;
	struct glsl_node *statement;
	struct glsl_node *last;
	struct glsl_token *token;
	int ends;

	/* The "{". */
	token = parse_peek(shader, 0U);
	parse_expect(shader, GLSL_P_LBRACE, "'{'");
	block = parse_node(shader, GLSL_N_BLOCK, token->line);
	if (new_scope) {
		block->flags |= GLSL_NODE_SCOPE;
		parse_push_scope(shader);
	}

	/* The statements up to "}". */
	last = NULL;
	for (;;) {
		ends = parse_punct_at(shader, 0U, GLSL_P_RBRACE);
		if (ends)
			break;
		statement = parse_statement(shader);
		parse_append(&block->child[0], &last, statement);
	}

	/* The "}" closes the scope. */
	(void)parse_take(shader);
	if (new_scope)
		parse_pop_scope(shader);

	/* Succeeded: the block. */
	return block;
}

/* Parses a declaration statement (or a precision statement) inside a function. */
static struct glsl_node *
parse_declaration_statement(
	struct glsl_shader *shader)
{
	struct glsl_node *type;
	struct glsl_node *declaration;
	struct glsl_token *token;
	const char *name;
	unsigned keyword;
	int alone;

	/* The precision statement. */
	keyword = parse_keyword_at(shader, 0U);
	if (keyword == GLSL_K_PRECISION) {
		declaration = parse_precision(shader);
		return declaration;
	}

	/* The type, then a struct definition alone or the variables. */
	token = parse_peek(shader, 0U);
	type = parse_fully_specified_type(shader);
	alone = parse_punct_at(shader, 0U, GLSL_P_SEMICOLON);
	if (alone) {
		(void)parse_take(shader);
		declaration = parse_node(shader, GLSL_N_DECLARATION, token->line);
		declaration->child[0] = type;
		return declaration;
	}

	/* The variables. */
	name = parse_identifier(shader, "a name");
	declaration = parse_declaration_rest(shader, type, name, token->line);
	return declaration;
}

/* Parses "if (condition) statement [else statement]". */
static struct glsl_node *
parse_if(
	struct glsl_shader *shader)
{
	struct glsl_node *node;
	struct glsl_token *token;
	unsigned keyword;

	/* The keyword and the condition. */
	token = parse_take(shader);
	node = parse_node(shader, GLSL_N_IF, token->line);
	parse_expect(shader, GLSL_P_LPAREN, "'('");
	node->child[0] = parse_expression(shader);
	parse_expect(shader, GLSL_P_RPAREN, "')'");

	/* The statement, and the else branch. */
	node->child[1] = parse_statement(shader);
	keyword = parse_keyword_at(shader, 0U);
	if (keyword == GLSL_K_ELSE) {
		(void)parse_take(shader);
		node->child[2] = parse_statement(shader);
	}

	/* Succeeded: the if. */
	return node;
}

/* Parses "for (init; condition; increment) statement". */
static struct glsl_node *
parse_for(
	struct glsl_shader *shader)
{
	struct glsl_node *node;
	struct glsl_token *token;
	int declaration;
	int empty;

	/* The keyword, and the scope of its init declaration. */
	token = parse_take(shader);
	node = parse_node(shader, GLSL_N_FOR, token->line);
	parse_expect(shader, GLSL_P_LPAREN, "'('");
	parse_push_scope(shader);

	/* The init: a declaration, an expression or nothing. */
	empty = parse_punct_at(shader, 0U, GLSL_P_SEMICOLON);
	declaration = parse_starts_declaration(shader);
	if (empty) {
		(void)parse_take(shader);
	} else if (declaration) {
		node->child[0] = parse_declaration_statement(shader);
	} else {
		node->child[0] = parse_node(shader, GLSL_N_EXPRESSION, token->line);
		node->child[0]->child[0] = parse_expression(shader);
		parse_expect(shader, GLSL_P_SEMICOLON, "';'");
	}

	/* The condition. */
	empty = parse_punct_at(shader, 0U, GLSL_P_SEMICOLON);
	if (!empty)
		node->child[1] = parse_expression(shader);
	parse_expect(shader, GLSL_P_SEMICOLON, "';'");

	/* The increment. */
	empty = parse_punct_at(shader, 0U, GLSL_P_RPAREN);
	if (!empty)
		node->child[2] = parse_expression(shader);
	parse_expect(shader, GLSL_P_RPAREN, "')'");

	/* The body, then the scope closes. */
	node->child[3] = parse_statement(shader);
	parse_pop_scope(shader);

	/* Succeeded: the loop. */
	return node;
}

/* Parses "while (condition) statement". */
static struct glsl_node *
parse_while(
	struct glsl_shader *shader)
{
	struct glsl_node *node;
	struct glsl_token *token;

	/* The keyword, the condition, the body. */
	token = parse_take(shader);
	node = parse_node(shader, GLSL_N_WHILE, token->line);
	parse_expect(shader, GLSL_P_LPAREN, "'('");
	node->child[0] = parse_expression(shader);
	parse_expect(shader, GLSL_P_RPAREN, "')'");
	node->child[1] = parse_statement(shader);

	/* Succeeded: the loop. */
	return node;
}

/* Parses "do statement while (condition);". */
static struct glsl_node *
parse_do(
	struct glsl_shader *shader)
{
	struct glsl_node *node;
	struct glsl_token *token;
	unsigned keyword;

	/* The keyword and the body. */
	token = parse_take(shader);
	node = parse_node(shader, GLSL_N_DO, token->line);
	node->child[0] = parse_statement(shader);

	/* "while", the condition and ";". */
	keyword = parse_keyword_at(shader, 0U);
	if (keyword != GLSL_K_WHILE)
		parse_unexpected(shader, "'while'");
	(void)parse_take(shader);
	parse_expect(shader, GLSL_P_LPAREN, "'('");
	node->child[1] = parse_expression(shader);
	parse_expect(shader, GLSL_P_RPAREN, "')'");
	parse_expect(shader, GLSL_P_SEMICOLON, "';'");

	/* Succeeded: the loop. */
	return node;
}

/* Parses "switch (selector) { statements with case labels }". */
static struct glsl_node *
parse_switch(
	struct glsl_shader *shader)
{
	struct glsl_node *node;
	struct glsl_token *token;

	/* The keyword and the selector. */
	token = parse_take(shader);
	node = parse_node(shader, GLSL_N_SWITCH, token->line);
	parse_expect(shader, GLSL_P_LPAREN, "'('");
	node->child[0] = parse_expression(shader);
	parse_expect(shader, GLSL_P_RPAREN, "')'");

	/* The body, whose labels are statements of their own. */
	node->child[1] = parse_compound(shader, 1);
	return node;
}

/* Parses the jumps and labels: case, default, return, break, continue, discard. */
static struct glsl_node *
parse_jump(
	struct glsl_shader *shader,
	unsigned keyword)
{
	struct glsl_node *node;
	struct glsl_token *token;
	int ends;

	/* The keyword. */
	token = parse_take(shader);

	/* A case label and its value. */
	if (keyword == GLSL_K_CASE) {
		node = parse_node(shader, GLSL_N_CASE, token->line);
		node->child[0] = parse_expression(shader);
		parse_expect(shader, GLSL_P_COLON, "':'");
		return node;
	}

	/* The default label. */
	if (keyword == GLSL_K_DEFAULT) {
		node = parse_node(shader, GLSL_N_DEFAULT, token->line);
		parse_expect(shader, GLSL_P_COLON, "':'");
		return node;
	}

	/* return with an optional value. */
	if (keyword == GLSL_K_RETURN) {
		node = parse_node(shader, GLSL_N_RETURN, token->line);
		ends = parse_punct_at(shader, 0U, GLSL_P_SEMICOLON);
		if (!ends)
			node->child[0] = parse_expression(shader);
		parse_expect(shader, GLSL_P_SEMICOLON, "';'");
		return node;
	}

	/* break, continue and discard. */
	if (keyword == GLSL_K_BREAK) {
		node = parse_node(shader, GLSL_N_BREAK, token->line);
	} else if (keyword == GLSL_K_CONTINUE) {
		node = parse_node(shader, GLSL_N_CONTINUE, token->line);
	} else {
		node = parse_node(shader, GLSL_N_DISCARD, token->line);
	}

	/* The ";". */
	parse_expect(shader, GLSL_P_SEMICOLON, "';'");

	/* Succeeded: the jump. */
	return node;
}

/* Parses an expression, with the comma operator. */
static struct glsl_node *
parse_expression(
	struct glsl_shader *shader)
{
	struct glsl_node *left;
	struct glsl_node *comma;
	struct glsl_token *token;
	int more;

	/* Assignments separated by commas, left to right. */
	left = parse_assignment(shader);
	for (;;) {
		more = parse_punct_at(shader, 0U, GLSL_P_COMMA);
		if (!more)
			break;
		token = parse_take(shader);
		comma = parse_node(shader, GLSL_N_COMMA, token->line);
		comma->child[0] = left;
		comma->child[1] = parse_assignment(shader);
		left = comma;
	}

	/* Succeeded: the expression. */
	return left;
}

/* Parses an assignment expression (right associative) or a conditional one. */
static struct glsl_node *
parse_assignment(
	struct glsl_shader *shader)
{
	struct glsl_node *left;
	struct glsl_node *node;
	struct glsl_token *token;

	/* The left side. */
	left = parse_conditional(shader);

	/* An assignment operator makes it an assignment. */
	token = parse_peek(shader, 0U);
	if (token->kind != GLSL_TOKEN_PUNCT)
		return left;
	switch (token->punct) {
	case GLSL_P_ASSIGN:
	case GLSL_P_ADD_ASSIGN:
	case GLSL_P_SUB_ASSIGN:
	case GLSL_P_MUL_ASSIGN:
	case GLSL_P_DIV_ASSIGN:
	case GLSL_P_MOD_ASSIGN:
	case GLSL_P_SHL_ASSIGN:
	case GLSL_P_SHR_ASSIGN:
	case GLSL_P_AND_ASSIGN:
	case GLSL_P_OR_ASSIGN:
	case GLSL_P_XOR_ASSIGN:
		break;
	default:
		return left;
	}

	/* The operator and the right side. */
	(void)parse_take(shader);
	node = parse_node(shader, GLSL_N_ASSIGN, token->line);
	node->op = token->punct;
	node->child[0] = left;
	node->child[1] = parse_assignment(shader);

	/* Succeeded: the assignment. */
	return node;
}

/* Parses "condition ? expression : assignment", or a binary expression. */
static struct glsl_node *
parse_conditional(
	struct glsl_shader *shader)
{
	struct glsl_node *condition;
	struct glsl_node *node;
	struct glsl_token *token;
	int is_question;

	/* The condition. */
	condition = parse_binary(shader, PARSE_LEVEL_OR);
	is_question = parse_punct_at(shader, 0U, GLSL_P_QUESTION);
	if (!is_question)
		return condition;

	/* The two choices. */
	token = parse_take(shader);
	node = parse_node(shader, GLSL_N_TERNARY, token->line);
	node->child[0] = condition;
	node->child[1] = parse_expression(shader);
	parse_expect(shader, GLSL_P_COLON, "':'");
	node->child[2] = parse_assignment(shader);

	/* Succeeded: the conditional. */
	return node;
}

/* Parses binary operators of a precedence level and above (precedence climbing). */
static struct glsl_node *
parse_binary(
	struct glsl_shader *shader,
	unsigned level)
{
	struct glsl_node *left;
	struct glsl_node *node;
	struct glsl_token *token;
	unsigned token_level;

	/* The left operand. */
	left = parse_unary(shader);

	/* Operators at this level or above, left associative. */
	for (;;) {
		token = parse_peek(shader, 0U);
		token_level = parse_binary_level(token);
		if (token_level == 0U || token_level < level)
			break;
		(void)parse_take(shader);

		/* The operation with the right operand, which binds the next level up. */
		node = parse_node(shader, GLSL_N_BINARY, token->line);
		node->op = token->punct;
		node->child[0] = left;
		node->child[1] = parse_binary(shader, token_level + 1U);
		left = node;
	}

	/* Succeeded: the expression. */
	return left;
}

/* Returns a token's binary precedence level, 0 when it is not a binary operator. */
static unsigned
parse_binary_level(
	const struct glsl_token *token)
{
	/* Only punctuators. */
	if (token->kind != GLSL_TOKEN_PUNCT)
		return 0U;

	/* GLSL's levels. */
	switch (token->punct) {
	case GLSL_P_OR_OR:
		return PARSE_LEVEL_OR;
	case GLSL_P_XOR_XOR:
		return PARSE_LEVEL_XOR;
	case GLSL_P_AND_AND:
		return PARSE_LEVEL_AND;
	case GLSL_P_BAR:
		return PARSE_LEVEL_BIT_OR;
	case GLSL_P_CARET:
		return PARSE_LEVEL_BIT_XOR;
	case GLSL_P_AMP:
		return PARSE_LEVEL_BIT_AND;
	case GLSL_P_EQ:
	case GLSL_P_NE:
		return PARSE_LEVEL_EQUALITY;
	case GLSL_P_LT:
	case GLSL_P_GT:
	case GLSL_P_LE:
	case GLSL_P_GE:
		return PARSE_LEVEL_RELATION;
	case GLSL_P_SHL:
	case GLSL_P_SHR:
		return PARSE_LEVEL_SHIFT;
	case GLSL_P_PLUS:
	case GLSL_P_MINUS:
		return PARSE_LEVEL_ADD;
	case GLSL_P_STAR:
	case GLSL_P_SLASH:
	case GLSL_P_PERCENT:
		return PARSE_LEVEL_MULTIPLY;
	default:
		break;
	}

	/* Not a binary operator. */
	return 0U;
}

/* Parses a unary expression: prefix operators, then a postfix expression. */
static struct glsl_node *
parse_unary(
	struct glsl_shader *shader)
{
	struct glsl_node *node;
	struct glsl_token *token;

	/* A prefix operator. */
	token = parse_peek(shader, 0U);
	if (token->kind != GLSL_TOKEN_PUNCT) {
		node = parse_postfix(shader);
		return node;
	}

	/* Increments and decrements. */
	if (token->punct == GLSL_P_INC || token->punct == GLSL_P_DEC) {
		(void)parse_take(shader);
		node = parse_node(shader, GLSL_N_PREINC, token->line);
		if (token->punct == GLSL_P_DEC)
			node->kind = GLSL_N_PREDEC;
		node->child[0] = parse_unary(shader);
		return node;
	}

	/* Plus, minus, not and complement. */
	if (token->punct == GLSL_P_PLUS ||
	    token->punct == GLSL_P_MINUS ||
	    token->punct == GLSL_P_BANG ||
	    token->punct == GLSL_P_TILDE) {
		(void)parse_take(shader);
		node = parse_node(shader, GLSL_N_UNARY, token->line);
		node->op = token->punct;
		node->child[0] = parse_unary(shader);
		return node;
	}

	/* Anything else starts a postfix expression. */
	node = parse_postfix(shader);
	return node;
}

/* Parses a primary expression followed by subscripts, selections, calls and postfix increments. */
static struct glsl_node *
parse_postfix(
	struct glsl_shader *shader)
{
	struct glsl_node *node;
	struct glsl_node *outer;
	struct glsl_token *token;
	int is_call;

	/* The primary expression. */
	node = parse_primary(shader);

	/* The postfix operators, left to right. */
	for (;;) {
		token = parse_peek(shader, 0U);
		if (token->kind != GLSL_TOKEN_PUNCT)
			break;

		/* A subscript. */
		if (token->punct == GLSL_P_LBRACKET) {
			(void)parse_take(shader);
			outer = parse_node(shader, GLSL_N_INDEX, token->line);
			outer->child[0] = node;
			outer->child[1] = parse_expression(shader);
			parse_expect(shader, GLSL_P_RBRACKET, "']'");
			node = outer;

			/* "NAME[i](...)": a call through an array of subroutine uniforms (the checker says whether it is one). */
			is_call = parse_punct_at(shader, 0U, GLSL_P_LPAREN);
			if (is_call && node->child[0]->kind == GLSL_N_IDENTIFIER) {
				outer = parse_node(shader, GLSL_N_CALL, node->line);
				outer->name = node->child[0]->name;
				outer->child[2] = node->child[1];
				node = parse_arguments(shader, outer);
			}
			continue;
		}

		/* A member, a swizzle, or ".length()". */
		if (token->punct == GLSL_P_DOT) {
			(void)parse_take(shader);
			token = parse_peek(shader, 0U);
			if (token->kind != GLSL_TOKEN_IDENTIFIER)
				parse_unexpected(shader, "a field name");
			(void)parse_take(shader);

			/* ".length()" on an array. */
			is_call = parse_punct_at(shader, 0U, GLSL_P_LPAREN);
			if (is_call)
				is_call = glsl_token_is(token, "length");
			if (is_call) {
				(void)parse_take(shader);
				parse_expect(shader, GLSL_P_RPAREN, "')'");
				outer = parse_node(shader, GLSL_N_LENGTH, token->line);
				outer->child[0] = node;
				node = outer;
				continue;
			}

			/* The selection by name. */
			outer = parse_node(shader, GLSL_N_FIELD, token->line);
			outer->child[0] = node;
			outer->name = glsl_strndup(&shader->arena, token->text, token->length);
			node = outer;
			continue;
		}

		/* A postfix increment or decrement. */
		if (token->punct == GLSL_P_INC || token->punct == GLSL_P_DEC) {
			(void)parse_take(shader);
			outer = parse_node(shader, GLSL_N_POSTINC, token->line);
			if (token->punct == GLSL_P_DEC)
				outer->kind = GLSL_N_POSTDEC;
			outer->child[0] = node;
			node = outer;
			continue;
		}

		/* Anything else ends the postfix expression. */
		break;
	}

	/* Succeeded: the expression. */
	return node;
}

/* Parses a primary expression: a literal, a name, a call, a constructor, or a parenthesized expression. */
static struct glsl_node *
parse_primary(
	struct glsl_shader *shader)
{
	struct glsl_node *node;
	struct glsl_token *token;
	unsigned keyword;
	unsigned mask;
	int starts_type;
	int is_call;
	int allowed;

	/* The literals. */
	token = parse_peek(shader, 0U);
	switch (token->kind) {
	case GLSL_TOKEN_INT:
		(void)parse_take(shader);
		node = parse_node(shader, GLSL_N_INT, token->line);
		node->integer = token->integer;
		return node;
	case GLSL_TOKEN_UINT:
		allowed = glsl_since(shader, GLSL_VERSION_130, GLSL_VERSION_ES300);
		if (!allowed)
			glsl_error(shader, token->line, "unsigned integer constants need GLSL 1.30");
		(void)parse_take(shader);
		node = parse_node(shader, GLSL_N_UINT, token->line);
		node->integer = token->integer;
		return node;
	case GLSL_TOKEN_FLOAT:
		/* A double (lf): 4.00, or GL_ARB_gpu_shader_fp64. */
		if (token->suffix == GLSL_SUFFIX_DOUBLE) {
			mask = glsl_version_mask(shader);
			if ((mask & GLSL_IN_FP64) == 0U)
				glsl_error(shader, token->line, "double constants need GLSL 4.00 or GL_ARB_gpu_shader_fp64");
			(void)parse_take(shader);
			node = parse_node(shader, GLSL_N_DOUBLE, token->line);
			node->number_double = token->number_double;
			return node;
		}

		/* A float (whose f suffix needs 1.20). */
		allowed = glsl_since(shader, GLSL_VERSION_120, GLSL_VERSION_ES300);
		if (token->suffix && !allowed)
			glsl_error(shader, token->line, "the f suffix of floats needs GLSL 1.20");
		(void)parse_take(shader);
		node = parse_node(shader, GLSL_N_FLOAT, token->line);
		node->number = token->number;
		return node;
	case GLSL_TOKEN_OTHER:
		glsl_fatal(shader, token->line, "invalid character '%.*s'", (int)token->length, token->text);
		break;
	default:
		break;
	}

	/* A parenthesized expression. */
	if (token->kind == GLSL_TOKEN_PUNCT && token->punct == GLSL_P_LPAREN) {
		(void)parse_take(shader);
		node = parse_expression(shader);
		parse_expect(shader, GLSL_P_RPAREN, "')'");
		return node;
	}

	/* true and false. */
	keyword = parse_classify(shader, token);
	if (keyword == GLSL_K_TRUE || keyword == GLSL_K_FALSE) {
		(void)parse_take(shader);
		node = parse_node(shader, GLSL_N_BOOL, token->line);
		if (keyword == GLSL_K_TRUE)
			node->integer = 1U;
		return node;
	}

	/* A constructor: a type followed by "(" (or "[size](" for an array). */
	starts_type = parse_starts_type(shader, 0U);
	if (starts_type) {
		node = parse_constructor(shader);
		return node;
	}

	/* A name: a variable, or a function when "(" follows. */
	if (token->kind != GLSL_TOKEN_IDENTIFIER || keyword != GLSL_K_NONE) {
		if (keyword == GLSL_K_RESERVED)
			glsl_fatal(shader, token->line, "'%.*s' is a reserved word", (int)token->length, token->text);
		parse_unexpected(shader, "an expression");
	}

	/* The name, and whether "(" follows. */
	(void)parse_take(shader);
	is_call = parse_punct_at(shader, 0U, GLSL_P_LPAREN);
	if (is_call) {
		node = parse_node(shader, GLSL_N_CALL, token->line);
		node->name = glsl_strndup(&shader->arena, token->text, token->length);
		node = parse_arguments(shader, node);
		return node;
	}

	/* The variable. */
	node = parse_node(shader, GLSL_N_IDENTIFIER, token->line);
	node->name = glsl_strndup(&shader->arena, token->text, token->length);
	return node;
}

/* Parses a constructor call: a type specifier and its arguments. */
static struct glsl_node *
parse_constructor(
	struct glsl_shader *shader)
{
	struct glsl_node *node;
	struct glsl_node *type;
	struct glsl_token *token;
	unsigned keyword;
	int is_call;

	/* A struct definition cannot be a constructor. */
	token = parse_peek(shader, 0U);
	keyword = parse_classify(shader, token);
	if (keyword == GLSL_K_STRUCT)
		parse_unexpected(shader, "an expression");

	/* The type (with its array size, if any). */
	type = parse_node(shader, GLSL_N_TYPE, token->line);
	parse_type_specifier(shader, type);

	/* The call. */
	is_call = parse_punct_at(shader, 0U, GLSL_P_LPAREN);
	if (!is_call)
		parse_unexpected(shader, "'(' after a type");
	node = parse_node(shader, GLSL_N_CALL, token->line);
	node->flags |= GLSL_NODE_CONSTRUCTOR;
	node->child[0] = type;
	node = parse_arguments(shader, node);

	/* Succeeded: the constructor. */
	return node;
}

/* Parses "( arguments )" of a call into child[1] ("(void)" is no arguments). */
static struct glsl_node *
parse_arguments(
	struct glsl_shader *shader,
	struct glsl_node *call)
{
	struct glsl_node *argument;
	struct glsl_node *last;
	int ends;
	int is_void;

	/* The "(". */
	parse_expect(shader, GLSL_P_LPAREN, "'('");

	/* "(void)". */
	is_void = parse_void_list(shader);
	if (is_void) {
		(void)parse_take(shader);
		(void)parse_take(shader);
		return call;
	}

	/* The arguments separated by commas, up to ")". */
	last = NULL;
	ends = parse_punct_at(shader, 0U, GLSL_P_RPAREN);
	while (!ends) {
		argument = parse_assignment(shader);
		parse_append(&call->child[1], &last, argument);

		/* A comma goes on, ")" ends. */
		ends = parse_punct_at(shader, 0U, GLSL_P_RPAREN);
		if (!ends)
			parse_expect(shader, GLSL_P_COMMA, "',' or ')'");
	}

	/* The ")". */
	(void)parse_take(shader);

	/* Succeeded: the call with its arguments. */
	return call;
}

/* Reports whether the next tokens are "void )", the empty list of parameters or arguments. */
static int
parse_void_list(
	struct glsl_shader *shader)
{
	struct glsl_token *token;
	unsigned keyword;
	int closes;

	/* The type void. */
	token = parse_peek(shader, 0U);
	keyword = parse_classify(shader, token);
	if (keyword != GLSL_K_TYPE || token->keyword_type->kind != GLSL_KIND_VOID)
		return 0;

	/* Then ")". */
	closes = parse_punct_at(shader, 1U, GLSL_P_RPAREN);
	if (!closes)
		return 0;

	/* Succeeded: "(void)". */
	return 1;
}
