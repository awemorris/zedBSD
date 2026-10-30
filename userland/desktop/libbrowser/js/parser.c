/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JavaScript parser's entry and its shared parts: the program, the
 * directive prologue, the tokens as the grammar reads them (a / is read
 * as division and read again as a regular expression where an expression
 * starts), automatic semicolon insertion, nodes and errors.
 *
 * A syntax error ends the parse at once: js_fail records it and jumps back
 * to js_parse (setjmp and longjmp), since every level of the recursive
 * descent would otherwise have to check.  Everything the parse made lives
 * in the arena, so nothing leaks by the jump.
 */

#include "js/internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The deepest nesting of expressions and statements parsed (deeper is an error, not a crash). */
#define PARSER_DEPTH_MAX	1000U

/* The block size of a program's arena. */
#define PARSER_ARENA_BLOCK	(64U * 1024U)

static struct js_node *parser_program(struct js_parser *parser);

/*
 * Parses a script (or a module, with JS_PARSE_MODULE) into a tree.
 * Returns 0, EINVAL with *error describing the syntax error, or ENOMEM.
 */
int
js_parse(
	const uint16_t *source,
	size_t length,
	unsigned how,
	struct js_program *program,
	struct js_syntax_error *error)
{
	struct js_parser *parser;
	int jumped;

	/* An empty tree and no error yet. */
	memset(program, 0, sizeof(*program));
	memset(error, 0, sizeof(*error));
	wb_arena_init(&program->arena, PARSER_ARENA_BLOCK);

	/* The parser lives in the arena too (its jump buffer must outlive setjmp's frame). */
	parser = wb_arena_zalloc(&program->arena, sizeof(*parser));
	if (parser == NULL) {
		wb_arena_release(&program->arena);
		return ENOMEM;
	}

	/* Its arena and error, and whether the source is a module. */
	parser->arena = &program->arena;
	parser->error = error;
	parser->module = 0;
	if ((how & JS_PARSE_MODULE) != 0U)
		parser->module = 1;

	/* Modules and strict scripts are strict code; a module is async at its top (await is a keyword). */
	parser->context.strict = 0;
	if ((how & (JS_PARSE_STRICT | JS_PARSE_MODULE)) != 0U)
		parser->context.strict = 1;
	if (parser->module)
		parser->context.in_async = 1;

	/* The lexer, before the jump point (its buffers are freed on both ways out). */
	js_lexer_init(&parser->lexer, parser, source, length, parser->module);

	/* A syntax error comes back here. */
	jumped = setjmp(parser->failure);
	if (jumped) {
		js_lexer_release(&parser->lexer);
		wb_arena_release(&program->arena);
		memset(&program->arena, 0, sizeof(program->arena));
		return EINVAL;
	}

	/* The first token (where an expression may start), then the program. */
	js_lexer_next(&parser->lexer, 1);
	program->root = parser_program(parser);
	js_lexer_release(&parser->lexer);

	/* Succeeded: the tree. */
	return 0;
}

/*
 * Frees a program's tree.
 */
void
js_program_release(
	struct js_program *program)
{
	/* Every node and string is in the arena. */
	wb_arena_release(&program->arena);
	memset(program, 0, sizeof(*program));
}

/*
 * Fails the parse at the current token with a message.
 */
void
js_fail(
	struct js_parser *parser,
	const char *message)
{
	/* The token's place. */
	js_fail_at(parser, parser->lexer.token.line, parser->lexer.token.column, message);
}

/*
 * Fails the parse at a place with a message.
 */
void
js_fail_at(
	struct js_parser *parser,
	uint32_t line,
	uint32_t column,
	const char *message)
{
	/* The error, then back to js_parse. */
	parser->error->line = line;
	parser->error->column = column;
	snprintf(parser->error->message, sizeof(parser->error->message), "%s", message);
	longjmp(parser->failure, 1);
}

/*
 * Tells whether an export node wraps a declaration of this module (a
 * variable, function or class declaration, or a default export of a named
 * function or class), not specifiers, a re-export or an anonymous default.
 */
int
js_export_is_declaration(
	const struct js_node *node)
{
	/* A re-export or specifiers are not. */
	if ((node->flags & JS_FLAG_ALL) != 0U || node->second != NULL)
		return 0;
	if (node->first == NULL || node->first->kind == JS_NODE_EXPORT_SPECIFIER)
		return 0;

	/* A default export of a function or class needs a name. */
	if ((node->flags & JS_FLAG_DEFAULT) != 0U && node->first->text == NULL)
		return 0;
	return 1;
}

/*
 * Makes a node of a kind at the current token's place.
 */
struct js_node *
js_node_new(
	struct js_parser *parser,
	int kind)
{
	struct js_node *node;

	/* A zeroed node; running out of memory ends the parse. */
	node = wb_arena_zalloc(parser->arena, sizeof(*node));
	if (node == NULL)
		js_fail(parser, "out of memory");
	node->kind = kind;
	node->offset = parser->lexer.token.start;
	node->line = parser->lexer.token.line;
	node->column = parser->lexer.token.column;

	/* Succeeded: the node. */
	return node;
}

/*
 * Copies bytes into the parse's arena.
 */
void *
js_arena_copy(
	struct js_parser *parser,
	const void *data,
	size_t size)
{
	void *copy;

	/* The copy (at least one byte, so an empty text has an address). */
	copy = wb_arena_alloc(parser->arena, size + 1U);
	if (copy == NULL)
		js_fail(parser, "out of memory");
	if (size != 0)
		memcpy(copy, data, size);

	/* Succeeded: the copy. */
	return copy;
}

/*
 * Moves to the next token, reading a / as division.
 */
void
js_next(
	struct js_parser *parser)
{
	/* The next token. */
	js_lexer_next(&parser->lexer, 0);
}

/*
 * Reads the current token again as the start of an expression: a / or /=
 * there starts a regular expression.
 */
void
js_next_regexp(
	struct js_parser *parser)
{
	struct js_lexer *lexer;
	int newline_before;

	/* A token read as something else first is read again from its start. */
	lexer = &parser->lexer;
	if (lexer->token.kind == JS_TOKEN_PUNCTUATOR &&
	    (lexer->token.punctuator == JS_P_SLASH || lexer->token.punctuator == JS_P_SLASH_ASSIGN)) {
		newline_before = lexer->token.newline_before;
		lexer->position = lexer->token.start;
		js_lexer_next(lexer, 1);
		lexer->token.newline_before = newline_before;
	}
}

/*
 * Tells whether the current token is a punctuator.
 */
int
js_is_punctuator(
	const struct js_parser *parser,
	int punctuator)
{
	/* The kind and the punctuator. */
	if (parser->lexer.token.kind != JS_TOKEN_PUNCTUATOR)
		return 0;
	if (parser->lexer.token.punctuator != punctuator)
		return 0;

	/* It is. */
	return 1;
}

/*
 * Tells whether the current token is an unescaped word.
 */
int
js_is_word(
	const struct js_parser *parser,
	const char *word)
{
	int same;

	/* The word, spelled without escapes. */
	same = js_token_is(&parser->lexer.token, word);

	/* Reports it. */
	return same;
}

/*
 * Requires a punctuator and moves past it.
 */
void
js_expect(
	struct js_parser *parser,
	int punctuator)
{
	int found;

	/* The punctuator must be there. */
	found = js_is_punctuator(parser, punctuator);
	if (!found)
		js_fail(parser, "unexpected token");

	/* Past it. */
	js_next(parser);
}

/*
 * Moves past a punctuator when it is there; reports whether it was.
 */
int
js_eat(
	struct js_parser *parser,
	int punctuator)
{
	int found;

	/* Only that punctuator. */
	found = js_is_punctuator(parser, punctuator);
	if (!found)
		return 0;

	/* Past it. */
	js_next(parser);
	return 1;
}

/*
 * Ends a statement: a semicolon, or one inserted before }, the end or a
 * line terminator (automatic semicolon insertion).
 */
void
js_semicolon(
	struct js_parser *parser)
{
	int eaten;

	/* A written semicolon. */
	eaten = js_eat(parser, JS_P_SEMICOLON);
	if (eaten)
		return;

	/* An inserted one. */
	if (parser->lexer.token.punctuator == JS_P_RBRACE || parser->lexer.token.kind == JS_TOKEN_END)
		return;
	if (parser->lexer.token.newline_before)
		return;

	/* Anything else cannot follow. */
	js_fail(parser, "missing ; before statement");
}

/*
 * Appends a node to a list kept by its first and last members.
 */
void
js_append(
	struct js_node **first,
	struct js_node **last,
	struct js_node *node)
{
	/* The first member, or after the last. */
	if (*first == NULL) {
		*first = node;
	} else {
		(*last)->next = node;
	}

	/* The node is the last now. */
	*last = node;
}

/*
 * Tells whether a text is an ASCII word.
 */
int
js_text_is(
	const uint16_t *text,
	size_t length,
	const char *word)
{
	size_t word_length;
	size_t index;

	/* The lengths must agree. */
	word_length = strlen(word);
	if (word_length != length)
		return 0;

	/* Then every character. */
	for (index = 0; index < length; index++) {
		if (text[index] != (uint16_t)(unsigned char)word[index])
			return 0;
	}

	/* The same word. */
	return 1;
}

/*
 * Tells whether two texts are the same characters.
 */
int
js_text_equal(
	const uint16_t *left,
	size_t left_length,
	const uint16_t *right,
	size_t right_length)
{
	int differs;

	/* The lengths must agree. */
	if (left_length != right_length)
		return 0;

	/* Then the characters. */
	differs = memcmp(left, right, left_length * sizeof(uint16_t));
	if (differs != 0)
		return 0;

	/* The same text. */
	return 1;
}

/*
 * Counts one more level of nesting; too deep a source is an error, not a
 * stack overflow.
 */
void
js_enter(
	struct js_parser *parser)
{
	/* The limit. */
	parser->depth++;
	if (parser->depth > PARSER_DEPTH_MAX)
		js_fail(parser, "the source nests too deeply");
}

/*
 * Counts one level of nesting less.
 */
void
js_leave(
	struct js_parser *parser)
{
	/* One level out. */
	parser->depth--;
}

/* Parses the program: its directives (a "use strict" makes it strict), then its statements to the end. */
static struct js_node *
parser_program(
	struct js_parser *parser)
{
	struct js_node *program;

	/* The program node. */
	program = js_node_new(parser, JS_NODE_PROGRAM);

	/* The statements (or module items) to the end, the directive prologue among them. */
	program->first = js_parse_function_body(parser, NULL);

	/* The program's flags. */
	if (parser->context.strict)
		program->flags |= JS_FLAG_STRICT;
	if (parser->module)
		program->flags |= JS_FLAG_MODULE;

	/* Succeeded: the program. */
	return program;
}
