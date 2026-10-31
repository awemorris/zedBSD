/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * JavaScript expressions (ES2024 §13, §15): assignment and its patterns,
 * arrow functions, yield, the conditional, the binary operators by
 * precedence, the unary and update operators, calls, members, optional
 * chains, new, super, import(), and the primary expressions (literals,
 * array and object literals, templates, function and class expressions,
 * parentheses).
 *
 * Arrow parameters and destructuring assignment targets are parsed first
 * as expressions (the cover grammars) and turned into patterns when => or
 * = shows what they were.  An object literal's `a = 1` shorthand is only
 * valid in such a pattern; the first one seen is remembered in the parser
 * until the literal is turned into a pattern, and is an error otherwise.
 */

#include "js/internal.h"
#include "js/regexp.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The precedence of the binary operators (0: not a binary operator). */
enum expr_precedence {
	EXPR_NONE,
	EXPR_NULLISH,
	EXPR_OR,
	EXPR_AND,
	EXPR_BIT_OR,
	EXPR_BIT_XOR,
	EXPR_BIT_AND,
	EXPR_EQUALITY,
	EXPR_RELATIONAL,
	EXPR_SHIFT,
	EXPR_ADDITIVE,
	EXPR_MULTIPLICATIVE,
	EXPR_EXPONENT
};

static struct js_node *expr_assignment_cover(struct js_parser *parser);
static void expr_check_regexp(struct js_parser *parser, const struct js_token *token);
static struct js_node *expr_arrow(struct js_parser *parser, struct js_node *parameters, uint32_t flags, uint32_t start, uint32_t line, uint32_t column);
static struct js_node *expr_yield(struct js_parser *parser);
static struct js_node *expr_conditional(struct js_parser *parser);
static struct js_node *expr_binary(struct js_parser *parser, int minimum);
static int expr_precedence(const struct js_parser *parser, int *op);
static struct js_node *expr_unary(struct js_parser *parser);
static struct js_node *expr_postfix(struct js_parser *parser);
static struct js_node *expr_lhs(struct js_parser *parser);
static struct js_node *expr_new(struct js_parser *parser);
static struct js_node *expr_tail(struct js_parser *parser, struct js_node *node, int allow_call);
static struct js_node *expr_member_name(struct js_parser *parser, struct js_node *object, uint32_t flags);
static struct js_node *expr_super(struct js_parser *parser);
static struct js_node *expr_import(struct js_parser *parser);
static struct js_node *expr_primary(struct js_parser *parser);
static struct js_node *expr_word(struct js_parser *parser);
static struct js_node *expr_arguments(struct js_parser *parser);
static struct js_node *expr_parenthesized(struct js_parser *parser);
static struct js_node *expr_array(struct js_parser *parser);
static struct js_node *expr_object(struct js_parser *parser);
static struct js_node *expr_object_property(struct js_parser *parser);
static struct js_node *expr_template(struct js_parser *parser, int tagged);
static struct js_node *expr_literal(struct js_parser *parser, int kind);
static int expr_peek_is_arrow(struct js_parser *parser);
static void expr_check_simple_target(struct js_parser *parser, const struct js_node *target, int binding);
static int expr_is_simple_parameters(const struct js_node *parameters);
static void expr_collect_names(struct js_parser *parser, const struct js_node *pattern, struct wb_vector *names);

/*
 * Parses an expression: assignments separated by commas.
 */
struct js_node *
js_parse_expression(
	struct js_parser *parser)
{
	struct js_node *first;
	struct js_node *last;
	struct js_node *sequence;

	/* The first assignment. */
	first = js_parse_assignment(parser);
	if (parser->lexer.token.punctuator != JS_P_COMMA)
		return first;

	/* More after commas make a sequence. */
	sequence = js_node_new(parser, JS_NODE_SEQUENCE);
	sequence->offset = first->offset;
	sequence->line = first->line;
	sequence->column = first->column;
	last = NULL;
	js_append(&sequence->first, &last, first);
	while (parser->lexer.token.punctuator == JS_P_COMMA) {
		js_next(parser);
		js_append(&sequence->first, &last, js_parse_assignment(parser));
	}

	/* Succeeded: the sequence. */
	return sequence;
}

/*
 * Parses an expression that may still become a pattern (a for-in or
 * for-of head): its pending `{ a = 1 }` shorthands stay in the parser for
 * the caller to settle.
 */
struct js_node *
js_parse_expression_cover(
	struct js_parser *parser)
{
	struct js_node *first;
	struct js_node *last;
	struct js_node *sequence;

	/* The first assignment. */
	first = expr_assignment_cover(parser);
	if (parser->lexer.token.punctuator != JS_P_COMMA)
		return first;

	/* More after commas make a sequence (never a pattern, so the covers are errors). */
	sequence = js_node_new(parser, JS_NODE_SEQUENCE);
	sequence->offset = first->offset;
	sequence->line = first->line;
	sequence->column = first->column;
	last = NULL;
	js_append(&sequence->first, &last, first);
	while (parser->lexer.token.punctuator == JS_P_COMMA) {
		js_next(parser);
		js_append(&sequence->first, &last, expr_assignment_cover(parser));
	}

	/* Succeeded: the sequence. */
	return sequence;
}

/*
 * Parses a left-hand side expression (a class's heritage).
 */
struct js_node *
js_parse_lhs(
	struct js_parser *parser)
{
	struct js_node *node;

	/* The expression. */
	js_next_regexp(parser);
	node = expr_lhs(parser);

	/* Succeeded: the expression. */
	return node;
}

/*
 * Parses an assignment expression where no pattern can follow: a pending
 * `{ a = 1 }` shorthand is an error.
 */
struct js_node *
js_parse_assignment(
	struct js_parser *parser)
{
	struct js_node *node;
	int outer;

	/* The expression, with the covers of this expression alone. */
	outer = parser->cover_pending;
	parser->cover_pending = 0;
	node = expr_assignment_cover(parser);

	/* A shorthand with an initializer left over was not in a pattern. */
	if (parser->cover_pending)
		js_fail_at(parser, parser->cover_line, parser->cover_column, "invalid shorthand property initializer");
	parser->cover_pending = outer;

	/* Succeeded: the expression. */
	return node;
}

/*
 * Parses a function after the function keyword (and its *): its name
 * (required for a declaration unless name_optional), parameters and body.
 */
struct js_node *
js_parse_function(
	struct js_parser *parser,
	uint32_t flags,
	int declaration,
	int name_optional)
{
	struct js_context saved;
	struct js_node *function;
	struct js_node *name;

	/* The node, a declaration or an expression. */
	function = js_node_new(parser, JS_NODE_FUNCTION);
	if (declaration)
		function->kind = JS_NODE_FUNCTION_DECLARATION;
	function->flags = flags;

	/* The name, checked where the function is declared, or (an expression's) in the function itself. */
	if (parser->lexer.token.punctuator != JS_P_LPAREN) {
		if (declaration) {
			name = js_parse_identifier_reference(parser);
			js_check_binding_name(parser, name);
		} else {
			js_enter_function(parser, &saved, flags, 0);
			name = js_parse_identifier_reference(parser);
			js_check_binding_name(parser, name);
			parser->context = saved;
		}

		/* The function takes the name. */
		function->text = name->text;
		function->word = name->word;
		function->text_length = name->text_length;
	} else if (declaration && !name_optional) {
		js_fail(parser, "a function declaration needs a name");
	}

	/* The parameters and the body in the function's own context. */
	js_enter_function(parser, &saved, flags, 0);
	js_parse_parameters(parser, function);
	js_parse_function_body(parser, function);
	parser->context = saved;

	/* Succeeded: the function. */
	return function;
}

/*
 * Parses a method (of an object literal or a class) after its key: its
 * parameters and body; kind is a js_property_kind.
 */
struct js_node *
js_parse_method(
	struct js_parser *parser,
	struct js_node *key,
	int kind,
	uint32_t flags)
{
	struct js_context saved;
	struct js_node *function;
	int parameters;

	/* The function, named after a plain key (a method never repeats a parameter name). */
	function = js_node_new(parser, JS_NODE_FUNCTION);
	function->flags = flags | JS_FLAG_METHOD;
	if (key->kind == JS_NODE_IDENTIFIER || key->kind == JS_NODE_STRING) {
		function->text = key->text;
		function->word = key->word;
		function->text_length = key->text_length;
	}

	/* Its parameters and body, with super; only a derived class's constructor calls super(). */
	js_enter_function(parser, &saved, flags, 1);
	if (kind == JS_PROPERTY_CONSTRUCTOR && parser->derived)
		parser->context.super_call = 1;
	js_parse_parameters(parser, function);
	js_parse_function_body(parser, function);
	parser->context = saved;

	/* A getter takes no parameter and a setter one (not a rest). */
	parameters = 0;
	for (key = function->first; key != NULL; key = key->next)
		parameters++;
	if (kind == JS_PROPERTY_GET && parameters != 0)
		js_fail_at(parser, function->line, function->column, "a getter takes no parameters");
	if (kind == JS_PROPERTY_SET && (parameters != 1 || function->first->kind == JS_NODE_REST))
		js_fail_at(parser, function->line, function->column, "a setter takes exactly one parameter");

	/* Succeeded: the method's function. */
	return function;
}

/*
 * Enters a function's context (saving the one outside): return is
 * allowed, yield and await follow the function's kind, and a method sees
 * super properties.
 */
void
js_enter_function(
	struct js_parser *parser,
	struct js_context *saved,
	uint32_t flags,
	int method)
{
	/* The context outside, kept for the way back. */
	*saved = parser->context;

	/* The function's own. */
	parser->context.in_function = 1;
	parser->context.in_generator = 0;
	if ((flags & JS_FLAG_GENERATOR) != 0U)
		parser->context.in_generator = 1;
	parser->context.in_async = 0;
	if ((flags & JS_FLAG_ASYNC) != 0U)
		parser->context.in_async = 1;
	parser->context.in_class_field = 0;
	parser->context.in_static_block = 0;
	parser->context.no_arguments = 0;
	parser->context.super_call = 0;
	parser->context.super_property = method;
	parser->context.new_target = 1;
	parser->context.in_iteration = 0;
	parser->context.in_switch = 0;
	parser->context.labels = NULL;
}

/*
 * Parses a function's parameters: ( elements with defaults, a rest last ).
 */
void
js_parse_parameters(
	struct js_parser *parser,
	struct js_node *function)
{
	struct js_node *last;
	struct js_node *parameter;
	struct js_node *element;

	/* The list between the parentheses. */
	js_expect(parser, JS_P_LPAREN);
	last = NULL;
	while (parser->lexer.token.punctuator != JS_P_RPAREN) {
		/* A rest parameter ends the list. */
		if (parser->lexer.token.punctuator == JS_P_ELLIPSIS) {
			js_next(parser);
			parameter = js_node_new(parser, JS_NODE_REST);
			parameter->first = js_parse_binding_target(parser);
			js_append(&function->first, &last, parameter);
			if (parser->lexer.token.punctuator != JS_P_RPAREN)
				js_fail(parser, "a rest parameter must be last");
			break;
		}

		/* A parameter, with its default. */
		element = js_parse_binding_target(parser);
		if (parser->lexer.token.punctuator == JS_P_ASSIGN) {
			parameter = js_node_new(parser, JS_NODE_ASSIGNMENT_PATTERN);
			js_next(parser);
			parameter->first = element;
			parameter->second = js_parse_assignment(parser);
			element = parameter;
		}

		/* The parameter joins the list. */
		js_append(&function->first, &last, element);

		/* A comma, or the end. */
		if (parser->lexer.token.punctuator != JS_P_COMMA)
			break;
		js_next(parser);
	}

	/* The closing parenthesis. */
	js_expect(parser, JS_P_RPAREN);
}

/*
 * Parses an identifier used as a reference or a binding: not a reserved
 * word, and yield and await only where they are not operators.
 */
struct js_node *
js_parse_identifier_reference(
	struct js_parser *parser)
{
	struct js_node *node;
	struct js_token *token;
	int reserved;
	int word;

	/* It must be an identifier. */
	token = &parser->lexer.token;
	if (token->kind != JS_TOKEN_IDENTIFIER)
		js_fail(parser, "an identifier is expected");

	/* A reserved word is no identifier (strict code reserves more). */
	reserved = js_token_is_reserved(token, parser->context.strict);
	if (reserved)
		js_fail(parser, "a reserved word cannot be an identifier");

	/* yield inside a generator, await inside an async function, a module or a static block. */
	word = token->word == JS_W_YIELD;
	if (word && parser->context.in_generator)
		js_fail(parser, "yield cannot be an identifier in a generator");
	word = token->word == JS_W_AWAIT;
	if (word && (parser->context.in_async || parser->module || parser->context.in_static_block))
		js_fail(parser, "await cannot be an identifier here");
	word = token->word == JS_W_ARGUMENTS;
	if (word && parser->context.no_arguments)
		js_fail(parser, "arguments cannot be used in a class field or a static block");

	/* The node. */
	node = js_node_new(parser, JS_NODE_IDENTIFIER);
	node->text = token->text;
	node->word = token->word;
	node->text_length = token->text_length;
	js_next(parser);

	/* Succeeded: the identifier. */
	return node;
}

/*
 * Tells whether the current token can be a binding identifier.
 */
int
js_is_binding_identifier(
	const struct js_parser *parser)
{
	const struct js_token *token;
	int reserved;

	/* An identifier that is not reserved. */
	token = &parser->lexer.token;
	if (token->kind != JS_TOKEN_IDENTIFIER)
		return 0;
	reserved = js_token_is_reserved(token, parser->context.strict);
	if (reserved)
		return 0;

	/* It can. */
	return 1;
}

/*
 * Refuses the names strict code cannot bind (eval and arguments).
 */
void
js_check_binding_name(
	struct js_parser *parser,
	const struct js_node *name)
{
	int word;

	/* Only strict code. */
	if (!parser->context.strict)
		return;

	/* eval and arguments. */
	word = name->word == JS_W_EVAL;
	if (!word)
		word = name->word == JS_W_ARGUMENTS;
	if (word)
		js_fail_at(parser, name->line, name->column, "eval and arguments cannot be bound in strict code");
}

/*
 * Parses a property name: an identifier name (reserved words included), a
 * string, a number, a computed [expression], or (for classes) a private
 * name; *flags gets JS_FLAG_COMPUTED for a computed one.
 */
struct js_node *
js_parse_property_name(
	struct js_parser *parser,
	uint32_t *flags)
{
	struct js_node *key;
	struct js_token *token;
	int no_in;

	/* A computed name (in is always the operator inside it). */
	token = &parser->lexer.token;
	if (parser->lexer.token.punctuator == JS_P_LBRACKET) {
		js_next(parser);
		*flags |= JS_FLAG_COMPUTED;
		no_in = parser->no_in;
		parser->no_in = 0;
		key = js_parse_assignment(parser);
		parser->no_in = no_in;
		js_expect(parser, JS_P_RBRACKET);
		return key;
	}

	/* A word, a private name, a string, a number or a BigInt. */
	switch (token->kind) {
	case JS_TOKEN_IDENTIFIER:
		key = js_node_new(parser, JS_NODE_IDENTIFIER);
		break;
	case JS_TOKEN_PRIVATE_NAME:
		key = js_node_new(parser, JS_NODE_PRIVATE_NAME);
		break;
	case JS_TOKEN_STRING:
		if (token->legacy_octal && parser->context.strict)
			js_fail(parser, "octal escapes are not allowed in strict code");
		key = js_node_new(parser, JS_NODE_STRING);
		break;
	case JS_TOKEN_NUMBER:
		if (token->legacy_octal && parser->context.strict)
			js_fail(parser, "octal literals are not allowed in strict code");
		key = js_node_new(parser, JS_NODE_NUMBER);
		key->number = token->number;
		break;
	case JS_TOKEN_BIGINT:
		key = js_node_new(parser, JS_NODE_BIGINT);
		break;
	default:
		js_fail(parser, "a property name is expected");
		return NULL;
	}

	/* The name's text. */
	key->text = token->text;
	key->word = token->word;
	key->text_length = token->text_length;
	js_next(parser);

	/* Succeeded: the key. */
	return key;
}

/*
 * Turns an expression parsed by the cover grammar into a pattern: a
 * binding pattern (declarations, parameters) or an assignment target.
 */
struct js_node *
js_to_pattern(
	struct js_parser *parser,
	struct js_node *node,
	int binding)
{
	struct js_node *item;
	struct js_node *pattern;

	/* Each kind of expression that can be a pattern. */
	switch (node->kind) {
	case JS_NODE_IDENTIFIER:
		/* A name (eval and arguments cannot be targets in strict code). */
		js_check_binding_name(parser, node);
		return node;
	case JS_NODE_MEMBER:
		/* A member is an assignment target, never a binding. */
		if (binding || (node->flags & JS_FLAG_OPTIONAL) != 0U)
			js_fail_at(parser, node->line, node->column, "invalid destructuring target");
		return node;
	case JS_NODE_CALL:
		/* A call only as a sloppy for-in or for-of head (Annex B), never in a destructuring. */
		if (binding || parser->context.strict || (node->flags & JS_FLAG_OPTIONAL) != 0U || parser->no_in != 2)
			js_fail_at(parser, node->line, node->column, "invalid destructuring target");
		return node;
	case JS_NODE_PARENTHESIZED:
		/* A parenthesized simple target, only in an assignment. */
		if (binding)
			js_fail_at(parser, node->line, node->column, "invalid destructuring target");
		expr_check_simple_target(parser, node->first, 0);
		return node;
	case JS_NODE_ARRAY:
		/* The elements become patterns; a spread becomes the rest, which must be last. */
		node->kind = JS_NODE_ARRAY_PATTERN;
		for (item = node->first; item != NULL; item = item->next) {
			if (item->kind == JS_NODE_HOLE)
				continue;
			if (item->kind == JS_NODE_SPREAD) {
				if (item->next != NULL || (item->flags & JS_FLAG_DEFAULT) != 0U)
					js_fail_at(parser, item->line, item->column, "a rest element must be last");
				item->kind = JS_NODE_REST;
				item->first = js_to_pattern(parser, item->first, binding);
				if (item->first->kind == JS_NODE_ASSIGNMENT_PATTERN)
					js_fail_at(parser, item->line, item->column, "a rest element cannot have a default");
				continue;
			}

			/* Any other element becomes a target. */
			pattern = js_to_pattern(parser, item, binding);
			if (pattern != item)
				*item = *pattern;
		}

		/* The array is a pattern now. */
		return node;
	case JS_NODE_OBJECT:
		/* Each property's value becomes a target; a spread becomes the rest, a plain name, last. */
		node->kind = JS_NODE_OBJECT_PATTERN;
		for (item = node->first; item != NULL; item = item->next) {
			if (item->op == JS_PROPERTY_SPREAD) {
				if (item->next != NULL)
					js_fail_at(parser, item->line, item->column, "a rest property must be last");
				if (item->second->kind != JS_NODE_IDENTIFIER && (binding || item->second->kind != JS_NODE_MEMBER))
					js_fail_at(parser, item->line, item->column, "invalid rest property");
				item->kind = JS_NODE_REST;
				item->first = js_to_pattern(parser, item->second, binding);
				continue;
			}

			/* Only a key: value property becomes a target. */
			if (item->op != JS_PROPERTY_INIT)
				js_fail_at(parser, item->line, item->column, "invalid destructuring target");
			item->second = js_to_pattern(parser, item->second, binding);
		}

		/* The object is a pattern now. */
		return node;
	case JS_NODE_ASSIGN:
		/* target = default becomes a pattern with a default. */
		if (node->op != JS_P_ASSIGN)
			js_fail_at(parser, node->line, node->column, "invalid destructuring target");
		node->kind = JS_NODE_ASSIGNMENT_PATTERN;
		node->first = js_to_pattern(parser, node->first, binding);
		return node;
	case JS_NODE_ASSIGNMENT_PATTERN:
	case JS_NODE_ARRAY_PATTERN:
	case JS_NODE_OBJECT_PATTERN:
		/* Already a pattern (an assignment's target turned before). */
		return node;
	default:
		break;
	}

	/* Anything else is no target. */
	js_fail_at(parser, node->line, node->column, "invalid destructuring target");
	return NULL;
}

/* Parses an assignment expression that may still be turned into a pattern (a pending shorthand stays). */
static struct js_node *
expr_assignment_cover(
	struct js_parser *parser)
{
	struct js_node *target;
	struct js_node *assign;
	struct js_token *token;
	uint32_t start;
	int outer;
	int op;
	int no_in;

	/* A yield expression in a generator. */
	js_enter(parser);
	token = &parser->lexer.token;
	if (parser->context.in_generator && parser->lexer.token.keyword == JS_W_YIELD) {
		assign = expr_yield(parser);
		js_leave(parser);
		return assign;
	}

	/* The left side; an arrow function may start here (the primary that begins here reads the mark first). */
	js_next_regexp(parser);
	start = token->start;
	parser->arrow_start = start;
	outer = parser->cover_pending;
	target = expr_conditional(parser);

	/* An arrow function was the whole expression. */
	if (target->kind == JS_NODE_FUNCTION && (target->flags & JS_FLAG_ARROW) != 0U && target->offset == start) {
		js_leave(parser);
		return target;
	}

	/* An assignment operator makes it a target. */
	if (token->kind != JS_TOKEN_PUNCTUATOR || token->punctuator < JS_P_ASSIGN || token->punctuator > JS_P_NULLISH_ASSIGN) {
		js_leave(parser);
		return target;
	}

	/* = takes a pattern (the literal's pending shorthands are part of it); the others a simple target. */
	op = token->punctuator;
	if (op == JS_P_ASSIGN && (target->kind == JS_NODE_ARRAY || target->kind == JS_NODE_OBJECT)) {
		target = js_to_pattern(parser, target, 0);
		parser->cover_pending = outer;
	} else {
		expr_check_simple_target(parser, target, 0);
	}

	/* The assignment and its value. */
	assign = js_node_new(parser, JS_NODE_ASSIGN);
	assign->offset = target->offset;
	assign->line = target->line;
	assign->column = target->column;
	assign->op = op;
	js_next(parser);
	no_in = parser->no_in;
	assign->first = target;
	assign->second = js_parse_assignment(parser);
	parser->no_in = no_in;

	/* Succeeded: the assignment. */
	js_leave(parser);
	return assign;
}

/*
 * Makes an arrow function from its parameters (patterns already) and
 * parses its body: a block, or one assignment expression.
 */
static struct js_node *
expr_arrow(
	struct js_parser *parser,
	struct js_node *parameters,
	uint32_t flags,
	uint32_t start,
	uint32_t line,
	uint32_t column)
{
	struct js_context saved;
	struct js_node *function;
	struct js_node *body;
	struct js_node *last;
	int super_property;
	int super_call;
	int new_target;
	int field;
	int no_arguments;
	int no_in;

	/* No line terminator may come before =>. */
	if (parser->lexer.token.newline_before)
		js_fail(parser, "no line break is allowed before =>");
	js_expect(parser, JS_P_ARROW);

	/* The function, which keeps the super, new.target and field context of where it is. */
	function = js_node_new(parser, JS_NODE_FUNCTION);
	function->flags = flags | JS_FLAG_ARROW;
	function->offset = start;
	function->line = line;
	function->column = column;
	function->first = parameters;
	super_property = parser->context.super_property;
	super_call = parser->context.super_call;
	new_target = parser->context.new_target;
	field = parser->context.in_class_field;
	no_arguments = parser->context.no_arguments;
	js_enter_function(parser, &saved, flags, super_property);
	parser->context.super_call = super_call;
	parser->context.new_target = new_target;
	parser->context.in_class_field = field;
	parser->context.no_arguments = no_arguments;

	/* A block body, or an expression. */
	if (parser->lexer.token.punctuator == JS_P_LBRACE) {
		js_parse_function_body(parser, function);
	} else {
		no_in = parser->no_in;
		body = js_parse_assignment(parser);
		parser->no_in = no_in;
		last = NULL;
		js_append(&function->second, &last, body);
		function->flags |= JS_FLAG_EXPRESSION_BODY;
		js_check_parameters(parser, function, 0);
	}

	/* The context outside the arrow. */
	parser->context = saved;

	/* The arrow's start, for the caller's check. */
	function->offset = start;

	/* Succeeded: the arrow function. */
	return function;
}

/* Parses yield [*] [expression]. */
static struct js_node *
expr_yield(
	struct js_parser *parser)
{
	struct js_node *node;
	struct js_token *token;
	int ends;

	/* The yield. */
	node = js_node_new(parser, JS_NODE_YIELD);
	js_next(parser);
	token = &parser->lexer.token;

	/* No argument after a line break or before what cannot start one. */
	ends = 0;
	if (token->newline_before || token->kind == JS_TOKEN_END)
		ends = 1;
	if (token->kind == JS_TOKEN_PUNCTUATOR) {
		switch (token->punctuator) {
		case JS_P_RPAREN:
		case JS_P_RBRACKET:
		case JS_P_RBRACE:
		case JS_P_COMMA:
		case JS_P_SEMICOLON:
		case JS_P_COLON:
		case JS_P_QUESTION:
			ends = 1;
			break;
		default:
			break;
		}
	}

	/* Nor before in and of (a for head's yield). */
	if (parser->lexer.token.keyword == JS_W_IN || parser->lexer.token.keyword == JS_W_OF)
		ends = 1;
	if (ends && parser->lexer.token.punctuator != JS_P_STAR)
		return node;

	/* yield* delegates. */
	if (parser->lexer.token.punctuator == JS_P_STAR) {
		js_next(parser);
		node->flags |= JS_FLAG_DELEGATE;
	}

	/* The argument, where an expression starts. */
	js_next_regexp(parser);
	node->first = js_parse_assignment(parser);

	/* Succeeded: the yield. */
	return node;
}

/* Parses test ? consequent : alternate, or what binds tighter. */
static struct js_node *
expr_conditional(
	struct js_parser *parser)
{
	struct js_node *test;
	struct js_node *node;
	int no_in;

	/* The test. */
	test = expr_binary(parser, EXPR_NONE);
	if (parser->lexer.token.punctuator != JS_P_QUESTION)
		return test;

	/* The two branches (in is allowed in the first even in a for head). */
	node = js_node_new(parser, JS_NODE_CONDITIONAL);
	node->offset = test->offset;
	node->line = test->line;
	node->column = test->column;
	js_next(parser);
	node->first = test;
	no_in = parser->no_in;
	parser->no_in = 0;
	node->second = js_parse_assignment(parser);
	parser->no_in = no_in;
	js_expect(parser, JS_P_COLON);
	node->third = js_parse_assignment(parser);

	/* Succeeded: the conditional. */
	return node;
}

/* Parses the binary operators tighter than minimum, by precedence climbing. */
static struct js_node *
expr_binary(
	struct js_parser *parser,
	int minimum)
{
	struct js_node *left;
	struct js_node *right;
	struct js_node *node;
	int precedence;
	int op;
	int right_minimum;

	/* The left operand: a unary expression, or a private name before in (#x in o). */
	if (parser->lexer.token.kind == JS_TOKEN_PRIVATE_NAME) {
		left = js_node_new(parser, JS_NODE_PRIVATE_NAME);
		left->text = parser->lexer.token.text;
		left->word = parser->lexer.token.word;
		left->text_length = parser->lexer.token.text_length;
		js_next(parser);
		if (parser->lexer.token.keyword != JS_W_IN || minimum >= EXPR_RELATIONAL)
			js_fail(parser, "a private name here must be followed by in");
	} else {
		left = expr_unary(parser);
	}

	/* Each operator that binds tighter than the caller's. */
	for (;;) {
		precedence = expr_precedence(parser, &op);
		if (precedence == EXPR_NONE || precedence <= minimum)
			break;

		/* ** cannot have an unparenthesized unary operand on its left. */
		if (op == JS_P_POWER && (left->kind == JS_NODE_UNARY || left->kind == JS_NODE_AWAIT))
			js_fail(parser, "a unary expression before ** must be parenthesized");

		/* The right operand: ** is right-associative. */
		js_next(parser);
		js_next_regexp(parser);
		right_minimum = precedence;
		if (op == JS_P_POWER)
			right_minimum = precedence - 1;
		right = expr_binary(parser, right_minimum);

		/* ?? cannot be mixed with || or && without parentheses. */
		if (op == JS_P_NULLISH &&
		    ((left->kind == JS_NODE_LOGICAL && left->op != JS_P_NULLISH) ||
		     (right->kind == JS_NODE_LOGICAL && right->op != JS_P_NULLISH)))
			js_fail(parser, "?? cannot be mixed with || or && without parentheses");
		if ((op == JS_P_OR || op == JS_P_AND) &&
		    ((left->kind == JS_NODE_LOGICAL && left->op == JS_P_NULLISH) ||
		     (right->kind == JS_NODE_LOGICAL && right->op == JS_P_NULLISH)))
			js_fail(parser, "?? cannot be mixed with || or && without parentheses");

		/* The node: logical operators apart from the others. */
		node = js_node_new(parser, JS_NODE_BINARY);
		if (op == JS_P_OR || op == JS_P_AND || op == JS_P_NULLISH)
			node->kind = JS_NODE_LOGICAL;
		node->op = op;
		node->offset = left->offset;
		node->line = left->line;
		node->column = left->column;
		node->first = left;
		node->second = right;
		left = node;
	}

	/* Succeeded: the expression. */
	return left;
}

/* Reports the current token's binary precedence and operator (in counts unless no_in forbids it). */
static int
expr_precedence(
	const struct js_parser *parser,
	int *op)
{
	const struct js_token *token;

	/* The words in and instanceof. */
	token = &parser->lexer.token;
	*op = JS_P_NONE;
	if (parser->lexer.token.keyword == JS_W_INSTANCEOF) {
		*op = JS_P_INSTANCEOF;
		return EXPR_RELATIONAL;
	}

	/* in, unless a for head forbids it. */
	if (parser->lexer.token.keyword == JS_W_IN && !parser->no_in) {
		*op = JS_P_IN;
		return EXPR_RELATIONAL;
	}

	/* The rest are punctuators. */
	if (token->kind != JS_TOKEN_PUNCTUATOR)
		return EXPR_NONE;

	/* The punctuators. */
	*op = token->punctuator;
	switch (token->punctuator) {
	case JS_P_NULLISH:
		return EXPR_NULLISH;
	case JS_P_OR:
		return EXPR_OR;
	case JS_P_AND:
		return EXPR_AND;
	case JS_P_BAR:
		return EXPR_BIT_OR;
	case JS_P_CARET:
		return EXPR_BIT_XOR;
	case JS_P_AMP:
		return EXPR_BIT_AND;
	case JS_P_EQ:
	case JS_P_NE:
	case JS_P_STRICT_EQ:
	case JS_P_STRICT_NE:
		return EXPR_EQUALITY;
	case JS_P_LT:
	case JS_P_GT:
	case JS_P_LE:
	case JS_P_GE:
		return EXPR_RELATIONAL;
	case JS_P_SHL:
	case JS_P_SAR:
	case JS_P_SHR:
		return EXPR_SHIFT;
	case JS_P_PLUS:
	case JS_P_MINUS:
		return EXPR_ADDITIVE;
	case JS_P_STAR:
	case JS_P_SLASH:
	case JS_P_PERCENT:
		return EXPR_MULTIPLICATIVE;
	case JS_P_POWER:
		return EXPR_EXPONENT;
	default:
		break;
	}

	/* Not a binary operator. */
	*op = JS_P_NONE;
	return EXPR_NONE;
}

/* Parses a unary expression: the prefix operators, await, and the postfix ones below. */
static struct js_node *
expr_unary(
	struct js_parser *parser)
{
	struct js_node *node;
	struct js_token *token;
	int op;

	/* Where an expression starts, a / is a regular expression. */
	js_next_regexp(parser);
	token = &parser->lexer.token;
	op = JS_P_NONE;

	/* The word operators. */
	if (parser->lexer.token.keyword == JS_W_DELETE)
		op = JS_P_DELETE;
	if (parser->lexer.token.keyword == JS_W_VOID)
		op = JS_P_VOID;
	if (parser->lexer.token.keyword == JS_W_TYPEOF)
		op = JS_P_TYPEOF;

	/* await in an async function or a module's top level. */
	if (parser->lexer.token.keyword == JS_W_AWAIT && (parser->context.in_async || (parser->module && !parser->context.in_function))) {
		if (parser->context.in_class_field || parser->context.in_static_block)
			js_fail(parser, "await is not allowed here");
		node = js_node_new(parser, JS_NODE_AWAIT);
		if (parser->module && !parser->context.in_function)
			parser->top_await = 1;
		js_next(parser);
		node->first = expr_unary(parser);
		return node;
	}

	/* The punctuator operators. */
	if (token->kind == JS_TOKEN_PUNCTUATOR) {
		switch (token->punctuator) {
		case JS_P_PLUS:
		case JS_P_MINUS:
		case JS_P_TILDE:
		case JS_P_NOT:
			op = token->punctuator;
			break;
		case JS_P_INCREMENT:
		case JS_P_DECREMENT:
			/* A prefix update: its operand must be a simple target. */
			node = js_node_new(parser, JS_NODE_UPDATE);
			node->op = token->punctuator;
			node->flags = JS_FLAG_PREFIX;
			js_next(parser);
			node->first = expr_unary(parser);
			expr_check_simple_target(parser, node->first, 0);
			return node;
		default:
			break;
		}
	}

	/* Not a prefix operator: a postfix expression. */
	if (op == JS_P_NONE) {
		node = expr_postfix(parser);
		return node;
	}

	/* The operator and its operand. */
	node = js_node_new(parser, JS_NODE_UNARY);
	node->op = op;
	js_next(parser);
	node->first = expr_unary(parser);

	/* Strict code cannot delete a plain name, or a private member. */
	if (op == JS_P_DELETE && parser->context.strict && node->first->kind == JS_NODE_IDENTIFIER)
		js_fail_at(parser, node->line, node->column, "delete of an unqualified identifier in strict code");
	if (op == JS_P_DELETE && node->first->kind == JS_NODE_MEMBER && node->first->second->kind == JS_NODE_PRIVATE_NAME)
		js_fail_at(parser, node->line, node->column, "a private member cannot be deleted");

	/* Succeeded: the unary expression. */
	return node;
}

/* Parses a left-hand side expression and a postfix ++ or -- on the same line. */
static struct js_node *
expr_postfix(
	struct js_parser *parser)
{
	struct js_node *operand;
	struct js_node *node;
	struct js_token *token;

	/* The operand. */
	operand = expr_lhs(parser);
	token = &parser->lexer.token;
	if (token->kind != JS_TOKEN_PUNCTUATOR || token->newline_before)
		return operand;
	if (token->punctuator != JS_P_INCREMENT && token->punctuator != JS_P_DECREMENT)
		return operand;

	/* The update. */
	expr_check_simple_target(parser, operand, 0);
	node = js_node_new(parser, JS_NODE_UPDATE);
	node->op = token->punctuator;
	node->offset = operand->offset;
	node->line = operand->line;
	node->column = operand->column;
	node->first = operand;
	js_next(parser);

	/* Succeeded: the postfix update. */
	return node;
}

/* Parses a left-hand side expression: new, super, import, or a primary, then members and calls. */
static struct js_node *
expr_lhs(
	struct js_parser *parser)
{
	struct js_node *node;

	/* The start. */
	if (parser->lexer.token.keyword == JS_W_NEW) {
		node = expr_new(parser);
	} else if (parser->lexer.token.keyword == JS_W_SUPER) {
		node = expr_super(parser);
	} else if (parser->lexer.token.keyword == JS_W_IMPORT) {
		node = expr_import(parser);
	} else {
		node = expr_primary(parser);
	}

	/* An arrow function takes no member or call after it. */
	if (node->kind == JS_NODE_FUNCTION && (node->flags & JS_FLAG_ARROW) != 0U)
		return node;

	/* The members and calls after it. */
	node = expr_tail(parser, node, 1);

	/* Succeeded: the expression. */
	return node;
}

/* Parses new: new.target, or new with a member expression and optional arguments. */
static struct js_node *
expr_new(
	struct js_parser *parser)
{
	struct js_node *node;
	struct js_node *callee;

	/* new.target inside a function. */
	node = js_node_new(parser, JS_NODE_NEW);
	js_next(parser);
	if (parser->lexer.token.punctuator == JS_P_DOT) {
		js_next(parser);
		if (parser->lexer.token.keyword != JS_W_TARGET)
			js_fail(parser, "new. must be followed by target");
		if (!parser->context.new_target)
			js_fail(parser, "new.target is only allowed in functions");
		node->kind = JS_NODE_META_PROPERTY;
		node->text = parser->lexer.token.text;
		node->word = parser->lexer.token.word;
		node->text_length = parser->lexer.token.text_length;
		js_next(parser);
		return node;
	}

	/* The callee: another new, super, import.meta, or a primary, with members but no calls. */
	js_next_regexp(parser);
	if (parser->lexer.token.keyword == JS_W_NEW) {
		callee = expr_new(parser);
	} else if (parser->lexer.token.keyword == JS_W_SUPER) {
		callee = expr_super(parser);
	} else if (parser->lexer.token.keyword == JS_W_IMPORT) {
		callee = expr_import(parser);
		if (callee->kind == JS_NODE_IMPORT_CALL)
			js_fail(parser, "import() cannot be a constructor");
	} else {
		callee = expr_primary(parser);
		if (callee->kind == JS_NODE_FUNCTION && (callee->flags & JS_FLAG_ARROW) != 0U)
			js_fail(parser, "an arrow function cannot be a constructor");
	}

	/* The callee's members, without calls. */
	callee = expr_tail(parser, callee, 0);
	node->first = callee;

	/* The arguments, if any. */
	if (parser->lexer.token.punctuator == JS_P_LPAREN)
		node->second = expr_arguments(parser);

	/* Succeeded: the new expression. */
	return node;
}

/* Parses the members, calls, optional chains and tagged templates after an expression. */
static struct js_node *
expr_tail(
	struct js_parser *parser,
	struct js_node *node,
	int allow_call)
{
	struct js_node *next;
	struct js_node *chain;
	int optional;

	/* One piece at a time. */
	optional = 0;
	for (;;) {
		/* .name or .#private. */
		if (parser->lexer.token.punctuator == JS_P_DOT) {
			js_next(parser);
			node = expr_member_name(parser, node, 0);
			continue;
		}

		/* ?. starts an optional chain (not in a new's callee). */
		if (parser->lexer.token.punctuator == JS_P_OPTIONAL) {
			if (!allow_call)
				js_fail(parser, "an optional chain cannot be a constructor");
			js_next(parser);
			optional = 1;
			if (parser->lexer.token.punctuator == JS_P_LPAREN) {
				next = js_node_new(parser, JS_NODE_CALL);
				next->flags = JS_FLAG_OPTIONAL;
				next->first = node;
				next->second = expr_arguments(parser);
				node = next;
			} else if (parser->lexer.token.punctuator == JS_P_LBRACKET) {
				js_next(parser);
				next = js_node_new(parser, JS_NODE_MEMBER);
				next->flags = JS_FLAG_COMPUTED | JS_FLAG_OPTIONAL;
				next->first = node;
				next->second = js_parse_expression(parser);
				js_expect(parser, JS_P_RBRACKET);
				node = next;
			} else if (parser->lexer.token.kind == JS_TOKEN_TEMPLATE) {
				js_fail(parser, "a template cannot follow an optional chain");
			} else {
				node = expr_member_name(parser, node, JS_FLAG_OPTIONAL);
			}

			/* The chain goes on. */
			continue;
		}

		/* [computed]. */
		if (parser->lexer.token.punctuator == JS_P_LBRACKET) {
			js_next(parser);
			next = js_node_new(parser, JS_NODE_MEMBER);
			next->flags = JS_FLAG_COMPUTED;
			next->offset = node->offset;
			next->line = node->line;
			next->column = node->column;
			next->first = node;
			next->second = js_parse_expression(parser);
			js_expect(parser, JS_P_RBRACKET);
			node = next;
			continue;
		}

		/* A call. */
		if (allow_call && parser->lexer.token.punctuator == JS_P_LPAREN) {
			next = js_node_new(parser, JS_NODE_CALL);
			next->offset = node->offset;
			next->line = node->line;
			next->column = node->column;
			next->first = node;
			next->second = expr_arguments(parser);
			node = next;
			continue;
		}

		/* A tagged template (never inside an optional chain). */
		if (parser->lexer.token.kind == JS_TOKEN_TEMPLATE) {
			if (optional)
				js_fail(parser, "a template cannot follow an optional chain");
			next = js_node_new(parser, JS_NODE_TAGGED_TEMPLATE);
			next->first = node;
			next->second = expr_template(parser, 1);
			node = next;
			continue;
		}

		/* Nothing more. */
		break;
	}

	/* An optional chain's short-circuit ends here. */
	if (optional) {
		chain = js_node_new(parser, JS_NODE_OPTIONAL_CHAIN);
		chain->offset = node->offset;
		chain->line = node->line;
		chain->column = node->column;
		chain->first = node;
		node = chain;
	}

	/* Succeeded: the expression. */
	return node;
}

/* Parses the name after . or ?.: any identifier name, or a private name. */
static struct js_node *
expr_member_name(
	struct js_parser *parser,
	struct js_node *object,
	uint32_t flags)
{
	struct js_node *member;
	struct js_node *name;
	struct js_token *token;

	/* The member, starting where its object does. */
	member = js_node_new(parser, JS_NODE_MEMBER);
	member->flags = flags;
	member->offset = object->offset;
	member->line = object->line;
	member->column = object->column;
	member->first = object;

	/* The name: a word (reserved words included) or a private name. */
	token = &parser->lexer.token;
	if (token->kind == JS_TOKEN_IDENTIFIER) {
		name = js_node_new(parser, JS_NODE_IDENTIFIER);
	} else if (token->kind == JS_TOKEN_PRIVATE_NAME) {
		name = js_node_new(parser, JS_NODE_PRIVATE_NAME);
	} else {
		js_fail(parser, "a property name is expected after .");
		return NULL;
	}

	/* The name's text and word. */
	name->text = token->text;
	name->word = token->word;
	name->text_length = token->text_length;
	member->second = name;
	js_next(parser);

	/* Succeeded: the member. */
	return member;
}

/* Parses super( ... ), super.name or super[expression] where each is allowed. */
static struct js_node *
expr_super(
	struct js_parser *parser)
{
	struct js_node *node;

	/* The super. */
	node = js_node_new(parser, JS_NODE_SUPER);
	js_next(parser);

	/* A call, in a derived class's constructor. */
	if (parser->lexer.token.punctuator == JS_P_LPAREN) {
		if (!parser->context.super_call)
			js_fail(parser, "super() is only allowed in a derived class's constructor");
		return node;
	}

	/* A property, in a method. */
	if (parser->lexer.token.punctuator == JS_P_DOT || parser->lexer.token.punctuator == JS_P_LBRACKET) {
		if (!parser->context.super_property)
			js_fail(parser, "super properties are only allowed in methods");
		return node;
	}

	/* Anything else. */
	js_fail(parser, "super must be followed by (, . or [");
	return NULL;
}

/* Parses import(specifier[, options]) or import.meta (in a module). */
static struct js_node *
expr_import(
	struct js_parser *parser)
{
	struct js_node *node;
	int comma;
	int no_in;

	/* import.meta. */
	node = js_node_new(parser, JS_NODE_IMPORT_CALL);
	js_next(parser);
	if (parser->lexer.token.punctuator == JS_P_DOT) {
		js_next(parser);
		if (parser->lexer.token.keyword != JS_W_META || !parser->module)
			js_fail(parser, "import.meta is only allowed in modules");
		node->kind = JS_NODE_META_PROPERTY;
		node->text = parser->lexer.token.text;
		node->word = parser->lexer.token.word;
		node->text_length = parser->lexer.token.text_length;
		js_next(parser);
		return node;
	}

	/* import(): one specifier, maybe options, maybe a trailing comma (in is the operator inside). */
	js_expect(parser, JS_P_LPAREN);
	if (parser->lexer.token.punctuator == JS_P_RPAREN)
		js_fail(parser, "import() needs a specifier");
	no_in = parser->no_in;
	parser->no_in = 0;
	node->first = js_parse_assignment(parser);
	comma = js_eat(parser, JS_P_COMMA);
	if (comma && parser->lexer.token.punctuator != JS_P_RPAREN) {
		node->second = js_parse_assignment(parser);
		js_eat(parser, JS_P_COMMA);
	}

	/* in is as before, then the closing parenthesis. */
	parser->no_in = no_in;
	js_expect(parser, JS_P_RPAREN);

	/* Succeeded: the dynamic import. */
	return node;
}

/*
 * Checks a regular expression literal's flags and pattern (ws074-p027):
 * an invalid one is an early error, as the specification wants it.
 */
static void
expr_check_regexp(
	struct js_parser *parser,
	const struct js_token *token)
{
	struct js_regexp_program *program;
	const char *message;
	char text[120];
	unsigned flags;
	int error;

	/* The flags. */
	error = js_regexp_parse_flags(token->raw, token->raw_length, &flags);
	if (error != 0)
		js_fail(parser, "Invalid regular expression flags");

	/* The pattern compiles (the program itself is made again when the literal runs). */
	error = js_regexp_compile(token->text, token->text_length, flags, &program, &message);
	if (error == ENOMEM)
		js_fail(parser, "out of memory");
	if (error != 0) {
		if (message == NULL)
			message = "syntax error";
		snprintf(text, sizeof(text), "Invalid regular expression: %s", message);
		js_fail(parser, text);
	}

	/* The check's program is not kept. */
	js_regexp_free(program);
}

/* Parses a primary expression. */
static struct js_node *
expr_primary(
	struct js_parser *parser)
{
	struct js_node *node;
	struct js_token *token;

	/* Where an expression starts, a / is a regular expression. */
	js_next_regexp(parser);
	token = &parser->lexer.token;
	switch (token->kind) {
	case JS_TOKEN_IDENTIFIER:
		node = expr_word(parser);
		return node;
	case JS_TOKEN_NUMBER:
		if (token->legacy_octal && parser->context.strict)
			js_fail(parser, "octal literals are not allowed in strict code");
		node = expr_literal(parser, JS_NODE_NUMBER);
		return node;
	case JS_TOKEN_BIGINT:
		node = expr_literal(parser, JS_NODE_BIGINT);
		return node;
	case JS_TOKEN_STRING:
		if (token->legacy_octal && parser->context.strict)
			js_fail(parser, "octal escapes are not allowed in strict code");
		node = expr_literal(parser, JS_NODE_STRING);
		return node;
	case JS_TOKEN_TEMPLATE:
		node = expr_template(parser, 0);
		return node;
	case JS_TOKEN_REGEXP:
		expr_check_regexp(parser, token);
		node = js_node_new(parser, JS_NODE_REGEXP);
		node->text = token->text;
		node->word = token->word;
		node->text_length = token->text_length;
		node->raw = token->raw;
		node->raw_length = token->raw_length;
		js_next(parser);
		return node;
	case JS_TOKEN_PUNCTUATOR:
		break;
	default:
		js_fail(parser, "unexpected token");
		return NULL;
	}

	/* The bracketed forms. */
	switch (token->punctuator) {
	case JS_P_LPAREN:
		node = expr_parenthesized(parser);
		return node;
	case JS_P_LBRACKET:
		node = expr_array(parser);
		return node;
	case JS_P_LBRACE:
		node = expr_object(parser);
		return node;
	default:
		break;
	}

	/* Anything else cannot start an expression. */
	js_fail(parser, "unexpected token");
	return NULL;
}

/* Parses a primary expression that starts with a word: a keyword's form, an arrow, or a name. */
static struct js_node *
expr_word(
	struct js_parser *parser)
{
	struct js_node *node;
	struct js_node *parameter;
	struct js_node *arguments;
	struct js_node *item;
	struct js_node *next;
	struct js_node *last;
	struct js_token *token;
	uint32_t line;
	uint32_t column;
	uint32_t start;
	int can_arrow;
	int arrow;

	/* The literal words. */
	token = &parser->lexer.token;
	if (parser->lexer.token.keyword == JS_W_THIS) {
		node = expr_literal(parser, JS_NODE_THIS);
		return node;
	}

	/* null. */
	if (parser->lexer.token.keyword == JS_W_NULL) {
		node = expr_literal(parser, JS_NODE_NULL);
		return node;
	}

	/* true. */
	if (parser->lexer.token.keyword == JS_W_TRUE) {
		node = expr_literal(parser, JS_NODE_TRUE);
		return node;
	}

	/* false. */
	if (parser->lexer.token.keyword == JS_W_FALSE) {
		node = expr_literal(parser, JS_NODE_FALSE);
		return node;
	}

	/* function and class expressions. */
	if (parser->lexer.token.keyword == JS_W_FUNCTION) {
		js_next(parser);
		if (parser->lexer.token.punctuator == JS_P_STAR) {
			js_next(parser);
			node = js_parse_function(parser, JS_FLAG_GENERATOR, 0, 1);
			return node;
		}

		/* A plain function expression. */
		node = js_parse_function(parser, 0, 0, 1);
		return node;
	}

	/* A class expression. */
	if (parser->lexer.token.keyword == JS_W_CLASS) {
		node = js_parse_class(parser, 0);
		return node;
	}

	/* async function, async arrow functions, or the name async; an arrow may only start an assignment. */
	line = token->line;
	column = token->column;
	start = token->start;
	can_arrow = 0;
	if (start == parser->arrow_start)
		can_arrow = 1;
	if (parser->lexer.token.keyword == JS_W_ASYNC) {
		node = js_parse_identifier_reference(parser);
		if (parser->lexer.token.keyword == JS_W_FUNCTION && !parser->lexer.token.newline_before) {
			js_next(parser);
			if (parser->lexer.token.punctuator == JS_P_STAR) {
				js_next(parser);
				node = js_parse_function(parser, JS_FLAG_ASYNC | JS_FLAG_GENERATOR, 0, 1);
				return node;
			}

			/* An async function expression. */
			node = js_parse_function(parser, JS_FLAG_ASYNC, 0, 1);
			return node;
		}

		/* async x => ... (the parameter cannot be await). */
		if (parser->lexer.token.kind == JS_TOKEN_IDENTIFIER && !parser->lexer.token.newline_before && can_arrow) {
			arrow = expr_peek_is_arrow(parser);
			if (arrow) {
				if (parser->lexer.token.keyword == JS_W_AWAIT)
					js_fail(parser, "await cannot be an async arrow's parameter");
				parameter = js_parse_identifier_reference(parser);
				js_check_binding_name(parser, parameter);
				node = expr_arrow(parser, parameter, JS_FLAG_ASYNC, start, line, column);
				return node;
			}
		}

		/* async (...) => ...: the arguments of a call become the parameters. */
		if (parser->lexer.token.punctuator == JS_P_LPAREN && !parser->lexer.token.newline_before && can_arrow) {
			arguments = expr_arguments(parser);
			if (parser->lexer.token.punctuator == JS_P_ARROW && !parser->lexer.token.newline_before) {
				last = NULL;
				parameter = NULL;
				for (item = arguments; item != NULL; item = next) {
					next = item->next;
					item->next = NULL;
					if (item->kind == JS_NODE_SPREAD) {
						item->kind = JS_NODE_REST;
						item->first = js_to_pattern(parser, item->first, 1);
					} else {
						item = js_to_pattern(parser, item, 1);
					}

					/* The parameter joins the list. */
					js_append(&parameter, &last, item);
				}

				/* The async arrow function. */
				node = expr_arrow(parser, parameter, JS_FLAG_ASYNC, start, line, column);
				return node;
			}

			/* A call of a function named async. */
			item = js_node_new(parser, JS_NODE_CALL);
			item->offset = node->offset;
			item->line = node->line;
			item->column = node->column;
			item->first = node;
			item->second = arguments;
			return item;
		}

		/* The name async itself. */
		return node;
	}

	/* A name, and maybe an arrow function with it as the only parameter. */
	node = js_parse_identifier_reference(parser);
	if (parser->lexer.token.punctuator == JS_P_ARROW && can_arrow) {
		js_check_binding_name(parser, node);
		node = expr_arrow(parser, node, 0, start, line, column);
		return node;
	}

	/* Succeeded: the identifier. */
	return node;
}

/* Parses ( arguments ): assignments and spreads, a trailing comma allowed. */
static struct js_node *
expr_arguments(
	struct js_parser *parser)
{
	struct js_node *first;
	struct js_node *last;
	struct js_node *argument;
	struct js_node *spread;
	int no_in;

	/* Between the parentheses, in is always the operator. */
	js_expect(parser, JS_P_LPAREN);
	no_in = parser->no_in;
	parser->no_in = 0;
	first = NULL;
	last = NULL;
	while (parser->lexer.token.punctuator != JS_P_RPAREN) {
		/* A spread, or an argument. */
		if (parser->lexer.token.punctuator == JS_P_ELLIPSIS) {
			spread = js_node_new(parser, JS_NODE_SPREAD);
			js_next(parser);
			spread->first = js_parse_assignment(parser);
			argument = spread;
		} else {
			argument = js_parse_assignment(parser);
		}

		/* The argument joins the list. */
		js_append(&first, &last, argument);

		/* A comma, or the end. */
		if (parser->lexer.token.punctuator != JS_P_COMMA)
			break;
		js_next(parser);
	}

	/* The closing parenthesis, and in as before. */
	js_expect(parser, JS_P_RPAREN);
	parser->no_in = no_in;

	/* Succeeded: the list (NULL for none). */
	return first;
}

/*
 * Parses ( ... ): a parenthesized expression, or the parameters of an
 * arrow function when => follows.
 */
static struct js_node *
expr_parenthesized(
	struct js_parser *parser)
{
	struct js_node *first;
	struct js_node *last;
	struct js_node *element;
	struct js_node *rest;
	struct js_node *node;
	struct js_node *next;
	struct js_node *parameters;
	uint32_t line;
	uint32_t column;
	uint32_t start;
	int must_be_arrow;
	int can_arrow;
	int outer;
	int no_in;
	int count;

	/* Inside, in is the operator, and the covers are this group's; an arrow may only start an assignment. */
	line = parser->lexer.token.line;
	column = parser->lexer.token.column;
	start = parser->lexer.token.start;
	can_arrow = 0;
	if (start == parser->arrow_start)
		can_arrow = 1;
	js_next(parser);
	no_in = parser->no_in;
	parser->no_in = 0;
	outer = parser->cover_pending;
	parser->cover_pending = 0;
	first = NULL;
	last = NULL;
	must_be_arrow = 0;
	count = 0;
	while (parser->lexer.token.punctuator != JS_P_RPAREN) {
		/* A rest element: only an arrow's parameters. */
		if (parser->lexer.token.punctuator == JS_P_ELLIPSIS) {
			rest = js_node_new(parser, JS_NODE_REST);
			js_next(parser);
			rest->first = js_parse_binding_target(parser);
			js_append(&first, &last, rest);
			must_be_arrow = 1;
			if (parser->lexer.token.punctuator != JS_P_RPAREN)
				js_fail(parser, "a rest parameter must be last");
			break;
		}

		/* An element. */
		element = expr_assignment_cover(parser);
		js_append(&first, &last, element);
		count++;

		/* A comma; one before ) is only an arrow's. */
		if (parser->lexer.token.punctuator != JS_P_COMMA)
			break;
		js_next(parser);
		if (parser->lexer.token.punctuator == JS_P_RPAREN)
			must_be_arrow = 1;
	}

	/* The closing parenthesis, and in as before. */
	js_expect(parser, JS_P_RPAREN);
	parser->no_in = no_in;

	/* => makes the elements an arrow's parameters. */
	if (parser->lexer.token.punctuator == JS_P_ARROW && can_arrow) {
		parameters = NULL;
		last = NULL;
		for (element = first; element != NULL; element = next) {
			next = element->next;
			element->next = NULL;
			if (element->kind != JS_NODE_REST)
				element = js_to_pattern(parser, element, 1);
			js_append(&parameters, &last, element);
		}

		/* The covers outside are as before. */
		parser->cover_pending = outer;
		node = expr_arrow(parser, parameters, 0, start, line, column);
		return node;
	}

	/* Otherwise it was an expression: () and a trailing comma were not. */
	if (must_be_arrow || count == 0)
		js_fail(parser, "an arrow function's parameters without =>");

	/* A parenthesized literal never becomes a pattern, so its shorthands with initializers are errors. */
	if (parser->cover_pending)
		js_fail_at(parser, parser->cover_line, parser->cover_column, "invalid shorthand property initializer");
	parser->cover_pending = outer;

	/* One expression, or a sequence, in parentheses. */
	node = js_node_new(parser, JS_NODE_PARENTHESIZED);
	node->line = line;
	node->column = column;
	node->offset = start;
	node->first = first;
	if (count > 1) {
		node->first = js_node_new(parser, JS_NODE_SEQUENCE);
		node->first->first = first;
		node->first->offset = first->offset;
		node->first->line = first->line;
		node->first->column = first->column;
	}

	/* Succeeded: the parenthesized expression. */
	return node;
}

/* Parses [ elements ]: holes, spreads and assignments. */
static struct js_node *
expr_array(
	struct js_parser *parser)
{
	struct js_node *array;
	struct js_node *last;
	struct js_node *element;
	int no_in;

	/* Inside, in is the operator. */
	array = js_node_new(parser, JS_NODE_ARRAY);
	js_next(parser);
	no_in = parser->no_in;
	parser->no_in = 0;
	last = NULL;
	while (parser->lexer.token.punctuator != JS_P_RBRACKET) {
		/* A hole. */
		if (parser->lexer.token.punctuator == JS_P_COMMA) {
			element = js_node_new(parser, JS_NODE_HOLE);
			js_next(parser);
			js_append(&array->first, &last, element);
			continue;
		}

		/* A spread, or an element. */
		if (parser->lexer.token.punctuator == JS_P_ELLIPSIS) {
			element = js_node_new(parser, JS_NODE_SPREAD);
			js_next(parser);
			element->first = expr_assignment_cover(parser);
		} else {
			element = expr_assignment_cover(parser);
		}

		/* The element joins the array. */
		js_append(&array->first, &last, element);

		/* A comma, or the end; a comma after a spread keeps it from being a rest. */
		if (parser->lexer.token.punctuator != JS_P_COMMA)
			break;
		js_next(parser);
		if (element->kind == JS_NODE_SPREAD)
			element->flags |= JS_FLAG_DEFAULT;
	}

	/* The closing bracket, and in as before. */
	js_expect(parser, JS_P_RBRACKET);
	parser->no_in = no_in;

	/* Succeeded: the array literal. */
	return array;
}

/* Parses { properties }. */
static struct js_node *
expr_object(
	struct js_parser *parser)
{
	struct js_node *object;
	struct js_node *last;
	struct js_node *property;
	int no_in;
	int prototypes;

	/* Inside, in is the operator. */
	object = js_node_new(parser, JS_NODE_OBJECT);
	js_next(parser);
	no_in = parser->no_in;
	parser->no_in = 0;
	last = NULL;
	prototypes = 0;
	while (parser->lexer.token.punctuator != JS_P_RBRACE) {
		property = expr_object_property(parser);
		js_append(&object->first, &last, property);

		/* __proto__: value may be written once (not by shorthand, method or computed key). */
		if (property->op == JS_PROPERTY_INIT && (property->flags & (JS_FLAG_COMPUTED | JS_FLAG_SHORTHAND)) == 0U &&
		    (property->first->kind == JS_NODE_IDENTIFIER || property->first->kind == JS_NODE_STRING) &&
		    property->first->word == JS_W_PROTO) {
			prototypes++;
			if (prototypes > 1)
				property->flags |= JS_FLAG_ALL;
		}

		/* A comma, or the end. */
		if (parser->lexer.token.punctuator != JS_P_COMMA)
			break;
		js_next(parser);
	}

	/* The closing brace, and in as before. */
	js_expect(parser, JS_P_RBRACE);
	parser->no_in = no_in;

	/* A duplicate __proto__ is an error unless the literal becomes a pattern (then it is harmless): remembered like a cover. */
	for (property = object->first; property != NULL; property = property->next) {
		if ((property->flags & JS_FLAG_ALL) != 0U && !parser->cover_pending) {
			parser->cover_pending = 1;
			parser->cover_line = property->line;
			parser->cover_column = property->column;
		}
	}

	/* Succeeded: the object literal. */
	return object;
}

/* Parses one property of an object literal. */
static struct js_node *
expr_object_property(
	struct js_parser *parser)
{
	struct js_node *property;
	struct js_node *key;
	struct js_node *value;
	struct js_token *token;
	uint32_t flags;
	uint32_t function_flags;
	int kind;
	int named;
	int reserved;

	/* A spread. */
	property = js_node_new(parser, JS_NODE_PROPERTY);
	token = &parser->lexer.token;
	if (parser->lexer.token.punctuator == JS_P_ELLIPSIS) {
		js_next(parser);
		property->op = JS_PROPERTY_SPREAD;
		property->second = expr_assignment_cover(parser);
		return property;
	}

	/* The modifiers: async, *, get, set (each only when a name follows it). */
	kind = JS_PROPERTY_INIT;
	function_flags = 0;
	if (parser->lexer.token.keyword == JS_W_ASYNC) {
		named = js_next_is_name(parser, 1);
		if (named) {
			js_next(parser);
			if (parser->lexer.token.newline_before)
				js_fail(parser, "no line break is allowed after async");
			function_flags |= JS_FLAG_ASYNC;
			kind = JS_PROPERTY_METHOD;
		}
	}

	/* * makes a generator method. */
	if (parser->lexer.token.punctuator == JS_P_STAR) {
		js_next(parser);
		function_flags |= JS_FLAG_GENERATOR;
		kind = JS_PROPERTY_METHOD;
	}

	/* get and set make accessors when a name follows them. */
	if (kind == JS_PROPERTY_INIT && (parser->lexer.token.keyword == JS_W_GET || parser->lexer.token.keyword == JS_W_SET)) {
		named = js_next_is_name(parser, 0);
		if (named) {
			kind = JS_PROPERTY_GET;
			if (parser->lexer.token.keyword == JS_W_SET)
				kind = JS_PROPERTY_SET;
			js_next(parser);
		}
	}

	/* The key. */
	flags = 0;
	if (token->kind == JS_TOKEN_PRIVATE_NAME)
		js_fail(parser, "a private name in an object literal");
	key = js_parse_property_name(parser, &flags);
	property->first = key;
	property->flags = flags;

	/* A method. */
	if (parser->lexer.token.punctuator == JS_P_LPAREN) {
		if (kind == JS_PROPERTY_INIT)
			kind = JS_PROPERTY_METHOD;
		property->op = kind;
		property->second = js_parse_method(parser, key, kind, function_flags);
		return property;
	}

	/* A modifier must have been followed by a method. */
	if (kind != JS_PROPERTY_INIT)
		js_fail(parser, "a method's parameters are expected");
	property->op = JS_PROPERTY_INIT;

	/* key: value. */
	if (parser->lexer.token.punctuator == JS_P_COLON) {
		js_next(parser);
		property->second = expr_assignment_cover(parser);
		return property;
	}

	/* Otherwise a shorthand: a plain name that could be a reference. */
	if (key->kind != JS_NODE_IDENTIFIER || (flags & JS_FLAG_COMPUTED) != 0U)
		js_fail(parser, "a property needs a value");
	reserved = js_word_is_reserved(key->text, key->text_length, parser->context.strict);
	if (reserved)
		js_fail_at(parser, key->line, key->column, "a reserved word cannot be a shorthand property");
	if (key->word == JS_W_YIELD && (parser->context.in_generator || parser->context.strict))
		js_fail_at(parser, key->line, key->column, "yield cannot be a shorthand property here");
	if (key->word == JS_W_AWAIT && (parser->context.in_async || parser->module))
		js_fail_at(parser, key->line, key->column, "await cannot be a shorthand property here");
	property->flags |= JS_FLAG_SHORTHAND;
	value = js_node_new(parser, JS_NODE_IDENTIFIER);
	*value = *key;
	value->next = NULL;
	property->second = value;

	/* { a = 1 }: only valid when the literal becomes a pattern. */
	if (parser->lexer.token.punctuator == JS_P_ASSIGN) {
		if (!parser->cover_pending) {
			parser->cover_pending = 1;
			parser->cover_line = parser->lexer.token.line;
			parser->cover_column = parser->lexer.token.column;
		}

		/* The default after =. */
		js_next(parser);
		value = js_node_new(parser, JS_NODE_ASSIGN);
		value->op = JS_P_ASSIGN;
		value->first = property->second;
		value->second = js_parse_assignment(parser);
		property->second = value;
	}

	/* Succeeded: the property. */
	return property;
}

/* Parses a template (tagged ones may have invalid escapes, whose cooked value is undefined). */
static struct js_node *
expr_template(
	struct js_parser *parser,
	int tagged)
{
	struct js_node *template_node;
	struct js_node *last;
	struct js_node *part;
	struct js_token *token;
	int no_in;

	/* The parts in turn: a string, then an expression, until the last string. */
	template_node = js_node_new(parser, JS_NODE_TEMPLATE);
	last = NULL;
	token = &parser->lexer.token;
	no_in = parser->no_in;
	parser->no_in = 0;
	for (;;) {
		/* The string part. */
		if (token->kind != JS_TOKEN_TEMPLATE)
			js_fail(parser, "a template part is expected");
		if (token->invalid_cooked && !tagged)
			js_fail(parser, "invalid escape in a template");
		part = js_node_new(parser, JS_NODE_TEMPLATE_STRING);
		part->text = token->text;
		part->word = token->word;
		part->text_length = token->text_length;
		part->raw = token->raw;
		part->raw_length = token->raw_length;
		if (token->invalid_cooked)
			part->flags |= JS_FLAG_INVALID_COOKED;
		js_append(&template_node->first, &last, part);

		/* The last part ends the template. */
		if (token->template_tail) {
			js_next(parser);
			break;
		}

		/* A substitution, then the } that ends it and the next part. */
		js_lexer_next(&parser->lexer, 1);
		js_append(&template_node->first, &last, js_parse_expression(parser));
		if (parser->lexer.token.punctuator != JS_P_RBRACE)
			js_fail(parser, "} is expected after a template substitution");
		js_lexer_template_continue(&parser->lexer);
	}

	/* in is as before. */
	parser->no_in = no_in;

	/* Succeeded: the template. */
	return template_node;
}

/* Makes a literal node of the current token and moves past it. */
static struct js_node *
expr_literal(
	struct js_parser *parser,
	int kind)
{
	struct js_node *node;
	struct js_token *token;

	/* The node with the token's value. */
	token = &parser->lexer.token;
	node = js_node_new(parser, kind);
	node->text = token->text;
	node->word = token->word;
	node->text_length = token->text_length;
	node->number = token->number;
	js_next(parser);

	/* Succeeded: the literal. */
	return node;
}

/* Tells whether the token after the current identifier is =>, on the same line. */
static int
expr_peek_is_arrow(
	struct js_parser *parser)
{
	struct js_lexer saved;
	int arrow;

	/* Looks one token ahead and comes back. */
	js_lexer_save(&parser->lexer, &saved);
	js_next(parser);
	arrow = 0;
	if (parser->lexer.token.punctuator == JS_P_ARROW && !parser->lexer.token.newline_before)
		arrow = 1;
	js_lexer_restore(&parser->lexer, &saved);

	/* Reports what was there. */
	return arrow;
}

/*
 * Tells whether a property name follows the current word (so the word is
 * a modifier such as get or static, not the name itself).
 */
int
js_next_is_name(
	struct js_parser *parser,
	int star)
{
	struct js_lexer saved;
	struct js_token *token;
	int named;

	/* Looks one token ahead and comes back. */
	js_lexer_save(&parser->lexer, &saved);
	js_next(parser);
	token = &parser->lexer.token;
	named = 0;
	if (token->kind == JS_TOKEN_IDENTIFIER || token->kind == JS_TOKEN_STRING || token->kind == JS_TOKEN_NUMBER)
		named = 1;
	if (token->kind == JS_TOKEN_BIGINT || token->kind == JS_TOKEN_PRIVATE_NAME)
		named = 1;
	if (parser->lexer.token.punctuator == JS_P_LBRACKET)
		named = 1;
	if (star && parser->lexer.token.punctuator == JS_P_STAR)
		named = 1;
	js_lexer_restore(&parser->lexer, &saved);

	/* Reports what was there. */
	return named;
}

/*
 * Requires a simple assignment target: a name (not eval or arguments in
 * strict code), a member (not an optional chain), or one of them in
 * parentheses.
 */
static void
expr_check_simple_target(
	struct js_parser *parser,
	const struct js_node *target,
	int binding)
{
	/* Parentheses around a simple target are allowed. */
	while (target->kind == JS_NODE_PARENTHESIZED)
		target = target->first;

	/* A name. */
	if (target->kind == JS_NODE_IDENTIFIER) {
		js_check_binding_name(parser, target);
		return;
	}

	/* A member, outside an optional chain. */
	if (target->kind == JS_NODE_MEMBER && !binding && (target->flags & JS_FLAG_OPTIONAL) == 0U)
		return;

	/* A call in sloppy code (it throws a ReferenceError when run: Annex B's web compatibility). */
	if (target->kind == JS_NODE_CALL && !binding && !parser->context.strict && (target->flags & JS_FLAG_OPTIONAL) == 0U)
		return;

	/* A call as a target is a runtime error only in sloppy code for web compatibility; refused here. */
	js_fail_at(parser, target->line, target->column, "invalid assignment target");
}

/*
 * Checks a function's parameters once its body is known: an arrow's, a
 * method's, strict or non-simple ones never repeat a name, and a body that
 * says "use strict" (use_strict) cannot have non-simple parameters.
 */
void
js_check_parameters(
	struct js_parser *parser,
	struct js_node *function,
	int use_strict)
{
	struct wb_vector names;
	const struct js_node **list;
	uint32_t line;
	uint32_t column;
	size_t index;
	size_t other;
	int simple;
	int same;
	int reserved;

	/* "use strict" in the body needs plain parameters. */
	simple = expr_is_simple_parameters(function->first);
	if (use_strict && !simple)
		js_fail_at(parser, function->line, function->column, "\"use strict\" in a function with non-simple parameters");

	/* Sloppy functions with plain parameters may repeat a name. */
	if ((function->flags & (JS_FLAG_ARROW | JS_FLAG_METHOD)) == 0U && simple && !parser->context.strict)
		return;

	/* The bound names; strict code (a "use strict" in the body too) takes neither eval, arguments nor a strict reserved word. */
	wb_vector_init(&names, sizeof(const struct js_node *));
	expr_collect_names(parser, function->first, &names);
	list = names.items;
	for (index = 0; parser->context.strict && index < names.count; index++) {
		reserved = js_word_is_reserved(list[index]->text, list[index]->text_length, 1);
		if (list[index]->word != JS_W_EVAL && list[index]->word != JS_W_ARGUMENTS && !reserved)
			continue;
		line = list[index]->line;
		column = list[index]->column;
		wb_vector_release(&names);
		js_fail_at(parser, line, column, "invalid parameter name in strict mode");
	}

	/* Compared pairwise. */
	for (index = 0; index < names.count; index++) {
		for (other = index + 1U; other < names.count; other++) {
			same = js_text_equal(list[index]->text, list[index]->text_length, list[other]->text, list[other]->text_length);
			if (!same)
				continue;
			line = list[other]->line;
			column = list[other]->column;
			wb_vector_release(&names);
			js_fail_at(parser, line, column, "duplicate parameter name");
		}
	}

	/* The names are no longer needed. */
	wb_vector_release(&names);
}

/* Tells whether a parameter list is simple: plain names only. */
static int
expr_is_simple_parameters(
	const struct js_node *parameters)
{
	const struct js_node *parameter;

	/* Every parameter a plain name. */
	for (parameter = parameters; parameter != NULL; parameter = parameter->next) {
		if (parameter->kind != JS_NODE_IDENTIFIER)
			return 0;
	}

	/* All plain. */
	return 1;
}

/* Collects the names a pattern binds. */
static void
expr_collect_names(
	struct js_parser *parser,
	const struct js_node *pattern,
	struct wb_vector *names)
{
	const struct js_node *item;

	/* Each element of the list. */
	for (item = pattern; item != NULL; item = item->next) {
		switch (item->kind) {
		case JS_NODE_IDENTIFIER:
			wb_vector_push(names, &item);
			break;
		case JS_NODE_ASSIGNMENT_PATTERN:
		case JS_NODE_REST:
			expr_collect_names(parser, item->first, names);
			break;
		case JS_NODE_ARRAY_PATTERN:
			expr_collect_names(parser, item->first, names);
			break;
		case JS_NODE_OBJECT_PATTERN:
			expr_collect_names(parser, item->first, names);
			break;
		case JS_NODE_PROPERTY:
			expr_collect_names(parser, item->second, names);
			break;
		default:
			break;
		}
	}
}
