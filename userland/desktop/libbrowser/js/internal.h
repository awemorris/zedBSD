/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The inside of the JavaScript parser, shared by the files of js/.
 */

#ifndef KEILAND_BROWSER_JS_INTERNAL_H
#define KEILAND_BROWSER_JS_INTERNAL_H

#include "js/js.h"

#include <setjmp.h>

/* The kinds of token. */
enum js_token_kind {
	JS_TOKEN_END,
	JS_TOKEN_IDENTIFIER,
	JS_TOKEN_PRIVATE_NAME,
	JS_TOKEN_NUMBER,
	JS_TOKEN_BIGINT,
	JS_TOKEN_STRING,
	JS_TOKEN_TEMPLATE,
	JS_TOKEN_REGEXP,
	JS_TOKEN_PUNCTUATOR
};

/* The punctuators, and the operators the tree's op fields name. */
enum js_punctuator {
	JS_P_NONE,
	JS_P_LBRACE, JS_P_RBRACE, JS_P_LPAREN, JS_P_RPAREN, JS_P_LBRACKET, JS_P_RBRACKET,
	JS_P_DOT, JS_P_ELLIPSIS, JS_P_SEMICOLON, JS_P_COMMA,
	JS_P_LT, JS_P_GT, JS_P_LE, JS_P_GE, JS_P_EQ, JS_P_NE, JS_P_STRICT_EQ, JS_P_STRICT_NE,
	JS_P_PLUS, JS_P_MINUS, JS_P_STAR, JS_P_SLASH, JS_P_PERCENT, JS_P_POWER,
	JS_P_INCREMENT, JS_P_DECREMENT,
	JS_P_SHL, JS_P_SAR, JS_P_SHR, JS_P_AMP, JS_P_BAR, JS_P_CARET,
	JS_P_NOT, JS_P_TILDE, JS_P_AND, JS_P_OR, JS_P_NULLISH,
	JS_P_QUESTION, JS_P_OPTIONAL, JS_P_COLON,
	JS_P_ASSIGN, JS_P_PLUS_ASSIGN, JS_P_MINUS_ASSIGN, JS_P_STAR_ASSIGN, JS_P_SLASH_ASSIGN, JS_P_PERCENT_ASSIGN,
	JS_P_POWER_ASSIGN, JS_P_SHL_ASSIGN, JS_P_SAR_ASSIGN, JS_P_SHR_ASSIGN, JS_P_AMP_ASSIGN, JS_P_BAR_ASSIGN,
	JS_P_CARET_ASSIGN, JS_P_AND_ASSIGN, JS_P_OR_ASSIGN, JS_P_NULLISH_ASSIGN,
	JS_P_ARROW,

	/* Operators that are words, for the tree's op fields. */
	JS_P_TYPEOF, JS_P_VOID, JS_P_DELETE, JS_P_IN, JS_P_INSTANCEOF,

	/* The declaration kinds of JS_NODE_VARIABLES. */
	JS_P_VAR, JS_P_LET, JS_P_CONST, JS_P_USING,

	JS_PUNCTUATORS
};

/*
 * The words the grammar and its early errors look for, found once by the
 * lexer (js_word_of) so the parser compares numbers.
 */
enum js_word {
	JS_W_NONE,
	JS_W_AS,
	JS_W_ASYNC,
	JS_W_AWAIT,
	JS_W_BREAK,
	JS_W_CASE,
	JS_W_CATCH,
	JS_W_CLASS,
	JS_W_CONST,
	JS_W_CONTINUE,
	JS_W_DEBUGGER,
	JS_W_DEFAULT,
	JS_W_DELETE,
	JS_W_DO,
	JS_W_ELSE,
	JS_W_EXPORT,
	JS_W_EXTENDS,
	JS_W_FALSE,
	JS_W_FINALLY,
	JS_W_FOR,
	JS_W_FROM,
	JS_W_FUNCTION,
	JS_W_GET,
	JS_W_IF,
	JS_W_IMPORT,
	JS_W_IN,
	JS_W_INSTANCEOF,
	JS_W_LET,
	JS_W_META,
	JS_W_NEW,
	JS_W_NULL,
	JS_W_OF,
	JS_W_RETURN,
	JS_W_SET,
	JS_W_STATIC,
	JS_W_SUPER,
	JS_W_SWITCH,
	JS_W_TARGET,
	JS_W_THIS,
	JS_W_THROW,
	JS_W_TRUE,
	JS_W_TRY,
	JS_W_TYPEOF,
	JS_W_VAR,
	JS_W_VOID,
	JS_W_WHILE,
	JS_W_WITH,
	JS_W_YIELD,
	JS_W_EVAL,
	JS_W_ARGUMENTS,
	JS_W_CONSTRUCTOR,
	JS_W_PROTOTYPE,
	JS_W_PROTO,
	JS_WORDS
};

/*
 * One token: its kind, the punctuator it is, its place in the source,
 * whether a line terminator comes before it, and its value: the cooked
 * characters of an identifier, a string or a template part (and the raw
 * ones of a template part or a regular expression's flags), or a number.
 * word is the js_word its characters spell (identifiers and strings);
 * keyword is the same for an identifier written without escapes, the only
 * way a keyword is a keyword, and JS_W_NONE otherwise.
 */
struct js_token {
	int kind;
	int punctuator;
	int word;
	int keyword;
	uint32_t start;
	uint32_t end;
	uint32_t line;
	uint32_t column;
	int newline_before;
	int escaped;
	int legacy_octal;
	int template_tail;
	int invalid_cooked;
	const uint16_t *text;
	size_t text_length;
	const uint16_t *raw;
	size_t raw_length;
	double number;
};

/*
 * The lexer: the source, where it is, the line it is on and where that
 * line starts, the token just read, and whether the source is a module
 * (which forbids HTML-like comments).
 *
 * cooked and raw are the buffers a token's decoded characters are built
 * in before they are copied to the arena; they belong to the lexer (not
 * to a function that a syntax error could jump out of), survive a save
 * and restore of the rest, and are freed by js_parse.
 */
struct js_lexer {
	const uint16_t *source;
	uint32_t length;
	uint32_t position;
	uint32_t line;
	uint32_t line_start;
	int module;
	int token_on_line;
	struct js_token token;
	struct js_parser *parser;
	struct wb_units cooked;
	struct wb_units raw;
};

/*
 * What the parser knows of the function it is in: whether yield and await
 * are operators there, whether return is allowed, whether super calls and
 * super properties are, whether the body is strict.
 */
struct js_context {
	int in_function;
	int in_generator;
	int in_async;
	int in_class_field;
	int in_static_block;
	int no_arguments;
	int strict;
	int super_call;
	int super_property;
	int new_target;
	int in_iteration;
	int in_switch;
	struct js_label *labels;
};

/*
 * A label in force: its name, whether it labels a loop, and the one
 * outside it.
 */
struct js_label {
	const uint16_t *name;
	size_t length;
	int loop;
	struct js_label *outer;
};

/*
 * The parser: the lexer, the program's arena, the context of the function
 * being parsed, and where to go on a syntax error (the parse unwinds to
 * js_parse with longjmp, since every level would otherwise check).
 *
 * no_in forbids the in operator (a for statement's head).  The cover
 * fields remember the first `{ a = 1 }` shorthand seen in an object
 * literal that is not yet known to be a pattern: it is an error unless the
 * literal becomes the target of an assignment or an arrow's parameters.
 * arrow_start is where an arrow function may begin (the start of the
 * assignment expression being parsed).  derived says the class being
 * parsed has a heritage, so its constructor may call super.
 */
struct js_parser {
	struct js_lexer lexer;
	struct wb_arena *arena;
	struct js_context context;
	struct js_syntax_error *error;
	jmp_buf failure;
	int module;
	/* A module's top level awaits (its program runs as an async function). */
	int top_await;
	int no_in;
	int cover_pending;
	uint32_t cover_line;
	uint32_t cover_column;
	uint32_t arrow_start;
	int derived;
	uint32_t depth;
};

/* The lexer (lexer.c). */
void js_lexer_init(struct js_lexer *lexer, struct js_parser *parser, const uint16_t *source, size_t length, int module);
void js_lexer_next(struct js_lexer *lexer, int regexp_allowed);
void js_lexer_template_continue(struct js_lexer *lexer);
void js_lexer_save(const struct js_lexer *lexer, struct js_lexer *saved);
void js_lexer_restore(struct js_lexer *lexer, const struct js_lexer *saved);
void js_lexer_release(struct js_lexer *lexer);
int js_token_is(const struct js_token *token, const char *word);
int js_token_is_reserved(const struct js_token *token, int strict);
int js_word_is_reserved(const uint16_t *text, size_t length, int strict);
int js_word_of(const uint16_t *text, size_t length);

/* The parser's shared parts (parser.c). */
void js_fail(struct js_parser *parser, const char *message);
void js_fail_at(struct js_parser *parser, uint32_t line, uint32_t column, const char *message);
struct js_node *js_node_new(struct js_parser *parser, int kind);
int js_export_is_declaration(const struct js_node *node);
void *js_arena_copy(struct js_parser *parser, const void *data, size_t size);
void js_next(struct js_parser *parser);
void js_next_regexp(struct js_parser *parser);
int js_is_punctuator(const struct js_parser *parser, int punctuator);
int js_is_word(const struct js_parser *parser, const char *word);
void js_expect(struct js_parser *parser, int punctuator);
int js_eat(struct js_parser *parser, int punctuator);
void js_semicolon(struct js_parser *parser);
void js_append(struct js_node **first, struct js_node **last, struct js_node *node);
int js_text_is(const uint16_t *text, size_t length, const char *word);
int js_text_equal(const uint16_t *left, size_t left_length, const uint16_t *right, size_t right_length);
void js_enter(struct js_parser *parser);
void js_leave(struct js_parser *parser);

/* Statements (parse_stmt.c). */
struct js_node *js_parse_statement_list_item(struct js_parser *parser);
struct js_node *js_parse_function_body(struct js_parser *parser, struct js_node *function);
struct js_node *js_parse_binding_target(struct js_parser *parser);
struct js_node *js_parse_class(struct js_parser *parser, int declaration);
struct js_node *js_parse_module_item(struct js_parser *parser);

/* Expressions (parse_expr.c). */
struct js_node *js_parse_expression(struct js_parser *parser);
struct js_node *js_parse_assignment(struct js_parser *parser);
struct js_node *js_parse_lhs(struct js_parser *parser);
struct js_node *js_parse_expression_cover(struct js_parser *parser);
int js_next_is_name(struct js_parser *parser, int star);
struct js_node *js_parse_function(struct js_parser *parser, uint32_t flags, int declaration, int name_optional);
struct js_node *js_parse_method(struct js_parser *parser, struct js_node *key, int kind, uint32_t flags);
struct js_node *js_parse_identifier_reference(struct js_parser *parser);
int js_is_binding_identifier(const struct js_parser *parser);
void js_enter_function(struct js_parser *parser, struct js_context *saved, uint32_t flags, int method);
struct js_node *js_parse_property_name(struct js_parser *parser, uint32_t *flags);
struct js_node *js_to_pattern(struct js_parser *parser, struct js_node *node, int binding);
void js_parse_parameters(struct js_parser *parser, struct js_node *function);
void js_check_binding_name(struct js_parser *parser, const struct js_node *name);
void js_check_parameters(struct js_parser *parser, struct js_node *function, int use_strict);

#endif
