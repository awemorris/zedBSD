/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * JavaScript statements and declarations (ES2024 §14, §15, §16): function
 * bodies with their directive prologue, the statements, variable and
 * lexical declarations with binding patterns, classes, and a module's
 * import and export declarations.
 *
 * The early errors checked in this first pass are the common ones: strict
 * code's restrictions (with, reserved words, eval and arguments, octal
 * literals), where return, break, continue and labels may be, the forms a
 * declaration may take, and a class's constructor.  The rest (duplicate
 * lexical names, the full set of class and module rules) arrive with the
 * compiler's scope analysis.
 */

#include "js/internal.h"

#include <string.h>

static struct js_node *stmt_statement(struct js_parser *parser);
static struct js_node *stmt_block(struct js_parser *parser);
static struct js_node *stmt_variables(struct js_parser *parser, int kind, int in_for);
static struct js_node *stmt_if(struct js_parser *parser);
static struct js_node *stmt_for(struct js_parser *parser);
static struct js_node *stmt_loop_body(struct js_parser *parser);
static struct js_node *stmt_while(struct js_parser *parser);
static struct js_node *stmt_do(struct js_parser *parser);
static struct js_node *stmt_jump(struct js_parser *parser, int kind);
static struct js_node *stmt_return(struct js_parser *parser);
static struct js_node *stmt_with(struct js_parser *parser);
static struct js_node *stmt_switch(struct js_parser *parser);
static struct js_node *stmt_throw(struct js_parser *parser);
static struct js_node *stmt_try(struct js_parser *parser);
static struct js_node *stmt_labeled(struct js_parser *parser);
static struct js_node *stmt_expression(struct js_parser *parser);
static struct js_node *stmt_function_declaration(struct js_parser *parser, int async_function);
static struct js_node *stmt_class_member(struct js_parser *parser, int *constructors);
static struct js_node *stmt_static_block(struct js_parser *parser);
static struct js_node *stmt_import(struct js_parser *parser);
static struct js_node *stmt_import_specifier(struct js_parser *parser, int named);
static struct js_node *stmt_export(struct js_parser *parser);
static struct js_node *stmt_module_name(struct js_parser *parser);
static struct js_node *stmt_module_string(struct js_parser *parser);
static void stmt_import_attributes(struct js_parser *parser);
static int stmt_is_lexical(struct js_parser *parser);
static int stmt_next_is(struct js_parser *parser, int punctuator);
static int stmt_is_async_function(struct js_parser *parser);
static struct js_node *stmt_binding_array(struct js_parser *parser);
static struct js_node *stmt_binding_object(struct js_parser *parser);
static struct js_node *stmt_binding_element(struct js_parser *parser);
static struct js_node *stmt_binding_identifier(struct js_parser *parser);

/*
 * Parses a function's body (with its braces) or, for a NULL function, the
 * program to its end: the directive prologue (a "use strict" makes the
 * code strict), then the statements.  Returns the list of statements and,
 * for a function, sets its body and checks its parameters.
 */
static struct js_node *parse_function_body_inner(struct js_parser *parser, struct js_node *function);

struct js_node *
js_parse_function_body(
	struct js_parser *parser,
	struct js_node *function)
{
	struct js_node *body;
	int no_in;

	/* The in operator is allowed again inside a body, even one in a for statement's initializer. */
	no_in = parser->no_in;
	parser->no_in = 0;
	body = parse_function_body_inner(parser, function);
	parser->no_in = no_in;
	return body;
}

static struct js_node *
parse_function_body_inner(
	struct js_parser *parser,
	struct js_node *function)
{
	struct js_node *first;
	struct js_node *last;
	struct js_node *item;
	struct js_token *token;
	uint32_t string_start;
	int prologue;
	int use_strict;
	int this_use_strict;
	int this_octal;
	int octal_seen;
	int directive;
	int spells_use_strict;

	/* A function's body is between braces. */
	if (function != NULL)
		js_expect(parser, JS_P_LBRACE);
	js_enter(parser);

	/* The statements, the leading strings among them being directives. */
	first = NULL;
	last = NULL;
	prologue = 1;
	use_strict = 0;
	octal_seen = 0;
	token = &parser->lexer.token;
	for (;;) {
		/* The end: } of a function, the end of the program. */
		js_next_regexp(parser);
		if (function != NULL && parser->lexer.token.punctuator == JS_P_RBRACE)
			break;
		if (function == NULL && token->kind == JS_TOKEN_END)
			break;

		/* Anything but a string ends the prologue. */
		if (token->kind != JS_TOKEN_STRING)
			prologue = 0;

		/* A string in the prologue: its spelling decides before the statement is parsed. */
		this_use_strict = 0;
		this_octal = 0;
		string_start = token->start;
		if (prologue) {
			spells_use_strict = js_text_is(token->text, token->text_length, "use strict");
			if (!token->escaped && spells_use_strict)
				this_use_strict = 1;
			this_octal = token->legacy_octal;
		}

		/* The statement (or a module's item). */
		if (parser->module && function == NULL) {
			item = js_parse_module_item(parser);
		} else {
			item = js_parse_statement_list_item(parser);
		}

		/* The item joins the list. */
		js_append(&first, &last, item);

		/* A statement that is the string alone is a directive; anything else ends the prologue. */
		directive = 0;
		if (prologue && item->kind == JS_NODE_EXPRESSION_STATEMENT && item->first->kind == JS_NODE_STRING &&
		    item->first->offset == string_start)
			directive = 1;
		if (!directive) {
			prologue = 0;
			continue;
		}

		/* "use strict" makes the code strict; an octal escape in the prologue is an error in strict code. */
		if (this_use_strict) {
			use_strict = 1;
			parser->context.strict = 1;
			if (octal_seen)
				js_fail(parser, "an octal escape before \"use strict\"");
		}

		/* An octal escape in the prologue is an error once the code is strict. */
		if (this_octal) {
			octal_seen = 1;
			if (parser->context.strict)
				js_fail(parser, "octal escapes are not allowed in strict code");
		}
	}

	/* The nesting of the body ends. */
	js_leave(parser);

	/* A function's closing brace, its body, and the check of its parameters. */
	if (function != NULL) {
		js_next(parser);
		function->second = first;
		if (parser->context.strict)
			function->flags |= JS_FLAG_STRICT;
		js_check_parameters(parser, function, use_strict);
	}

	/* Succeeded: the statements. */
	return first;
}

/*
 * Parses a statement or a declaration (function, class, let, const).
 */
struct js_node *
js_parse_statement_list_item(
	struct js_parser *parser)
{
	struct js_node *node;
	int lexical;
	int async_function;

	/* A function declaration. */
	js_next_regexp(parser);
	if (parser->lexer.token.keyword == JS_W_FUNCTION) {
		node = stmt_function_declaration(parser, 0);
		return node;
	}

	/* An async function declaration. */
	async_function = stmt_is_async_function(parser);
	if (async_function) {
		node = stmt_function_declaration(parser, 1);
		return node;
	}

	/* A class declaration. */
	if (parser->lexer.token.keyword == JS_W_CLASS) {
		node = js_parse_class(parser, 1);
		return node;
	}

	/* A let or const declaration. */
	lexical = stmt_is_lexical(parser);
	if (lexical) {
		node = stmt_variables(parser, lexical, 0);
		js_semicolon(parser);
		return node;
	}

	/* Otherwise a statement. */
	node = stmt_statement(parser);

	/* Succeeded: the statement. */
	return node;
}

/*
 * Parses a binding target: a name, or an array or object binding pattern.
 */
struct js_node *
js_parse_binding_target(
	struct js_parser *parser)
{
	struct js_node *node;

	/* The three forms. */
	if (parser->lexer.token.punctuator == JS_P_LBRACKET) {
		node = stmt_binding_array(parser);
		return node;
	}

	/* An object binding pattern. */
	if (parser->lexer.token.punctuator == JS_P_LBRACE) {
		node = stmt_binding_object(parser);
		return node;
	}

	/* A plain name. */
	node = stmt_binding_identifier(parser);

	/* Succeeded: the name. */
	return node;
}

/*
 * Parses a class (a declaration needs a name unless it is a module's
 * default export): its heritage and its members, all strict code.
 */
struct js_node *
js_parse_class(
	struct js_parser *parser,
	int declaration)
{
	struct js_node *node;
	struct js_node *name;
	struct js_node *member;
	struct js_node *last;
	int strict;
	int derived;
	int constructors;

	/* The node; a class is strict code from its name on. */
	node = js_node_new(parser, JS_NODE_CLASS);
	if (declaration)
		node->kind = JS_NODE_CLASS_DECLARATION;
	js_next(parser);
	strict = parser->context.strict;
	parser->context.strict = 1;

	/* The name. */
	if (parser->lexer.token.kind == JS_TOKEN_IDENTIFIER && parser->lexer.token.keyword != JS_W_EXTENDS) {
		name = stmt_binding_identifier(parser);
		node->text = name->text;
		node->word = name->word;
		node->text_length = name->text_length;
	} else if (declaration && declaration != 2) {
		js_fail(parser, "a class declaration needs a name");
	}

	/* The heritage. */
	derived = parser->derived;
	parser->derived = 0;
	if (parser->lexer.token.keyword == JS_W_EXTENDS) {
		js_next(parser);
		node->first = js_parse_lhs(parser);
		parser->derived = 1;
	}

	/* The members. */
	js_expect(parser, JS_P_LBRACE);
	last = NULL;
	constructors = 0;
	while (parser->lexer.token.punctuator != JS_P_RBRACE) {
		if (parser->lexer.token.punctuator == JS_P_SEMICOLON) {
			js_next(parser);
			continue;
		}

		/* A method, a field or a static block. */
		member = stmt_class_member(parser, &constructors);
		js_append(&node->second, &last, member);
	}

	/* Past the closing brace. */
	js_next(parser);

	/* The context outside the class. */
	parser->derived = derived;
	parser->context.strict = strict;

	/* Succeeded: the class. */
	return node;
}

/*
 * Parses a module's item: an import or export declaration, or a statement.
 */
struct js_node *
js_parse_module_item(
	struct js_parser *parser)
{
	struct js_node *node;
	int call;

	/* import, unless it is import() or import.meta. */
	js_next_regexp(parser);
	if (parser->lexer.token.keyword == JS_W_IMPORT) {
		call = stmt_next_is(parser, JS_P_LPAREN);
		if (!call)
			call = stmt_next_is(parser, JS_P_DOT);
		if (!call) {
			node = stmt_import(parser);
			return node;
		}
	}

	/* export. */
	if (parser->lexer.token.keyword == JS_W_EXPORT) {
		node = stmt_export(parser);
		return node;
	}

	/* Anything else. */
	node = js_parse_statement_list_item(parser);

	/* Succeeded: the item. */
	return node;
}

/* Parses a statement (not a declaration). */
static struct js_node *
stmt_statement(
	struct js_parser *parser)
{
	struct js_node *node;
	int labeled;

	/* Each statement by its first token. */
	js_enter(parser);
	js_next_regexp(parser);
	if (parser->lexer.token.punctuator == JS_P_LBRACE) {
		node = stmt_block(parser);
	} else if (parser->lexer.token.punctuator == JS_P_SEMICOLON) {
		node = js_node_new(parser, JS_NODE_EMPTY);
		js_next(parser);
	} else if (parser->lexer.token.keyword == JS_W_VAR) {
		node = stmt_variables(parser, JS_P_VAR, 0);
		js_semicolon(parser);
	} else if (parser->lexer.token.keyword == JS_W_IF) {
		node = stmt_if(parser);
	} else if (parser->lexer.token.keyword == JS_W_FOR) {
		node = stmt_for(parser);
	} else if (parser->lexer.token.keyword == JS_W_WHILE) {
		node = stmt_while(parser);
	} else if (parser->lexer.token.keyword == JS_W_DO) {
		node = stmt_do(parser);
	} else if (parser->lexer.token.keyword == JS_W_CONTINUE) {
		node = stmt_jump(parser, JS_NODE_CONTINUE);
	} else if (parser->lexer.token.keyword == JS_W_BREAK) {
		node = stmt_jump(parser, JS_NODE_BREAK);
	} else if (parser->lexer.token.keyword == JS_W_RETURN) {
		node = stmt_return(parser);
	} else if (parser->lexer.token.keyword == JS_W_WITH) {
		node = stmt_with(parser);
	} else if (parser->lexer.token.keyword == JS_W_SWITCH) {
		node = stmt_switch(parser);
	} else if (parser->lexer.token.keyword == JS_W_THROW) {
		node = stmt_throw(parser);
	} else if (parser->lexer.token.keyword == JS_W_TRY) {
		node = stmt_try(parser);
	} else if (parser->lexer.token.keyword == JS_W_DEBUGGER) {
		node = js_node_new(parser, JS_NODE_DEBUGGER);
		js_next(parser);
		js_semicolon(parser);
	} else {
		/* A label, or an expression. */
		labeled = 0;
		if (parser->lexer.token.kind == JS_TOKEN_IDENTIFIER)
			labeled = stmt_next_is(parser, JS_P_COLON);
		if (labeled) {
			node = stmt_labeled(parser);
		} else {
			node = stmt_expression(parser);
		}
	}

	/* The nesting of the statement ends. */
	js_leave(parser);

	/* Succeeded: the statement. */
	return node;
}

/* Parses { statements }. */
static struct js_node *
stmt_block(
	struct js_parser *parser)
{
	struct js_node *block;
	struct js_node *last;

	/* The statements between the braces. */
	block = js_node_new(parser, JS_NODE_BLOCK);
	js_expect(parser, JS_P_LBRACE);
	last = NULL;
	for (;;) {
		js_next_regexp(parser);
		if (parser->lexer.token.punctuator == JS_P_RBRACE)
			break;
		js_append(&block->first, &last, js_parse_statement_list_item(parser));
	}

	/* Past the closing brace. */
	js_next(parser);

	/* Succeeded: the block. */
	return block;
}

/*
 * Parses var, let or const declarators; in a for head (in_for) an
 * initializer may be missing where the loop is for-in or for-of.
 */
static struct js_node *
stmt_variables(
	struct js_parser *parser,
	int kind,
	int in_for)
{
	struct js_node *node;
	struct js_node *declarator;
	struct js_node *last;
	int initialized;

	/* The declaration and its first declarator. */
	node = js_node_new(parser, JS_NODE_VARIABLES);
	node->op = kind;
	js_next(parser);
	last = NULL;
	for (;;) {
		declarator = js_node_new(parser, JS_NODE_DECLARATOR);
		declarator->first = js_parse_binding_target(parser);

		/* let and const cannot bind the name let. */
		if (kind != JS_P_VAR && declarator->first->kind == JS_NODE_IDENTIFIER &&
		    declarator->first->word == JS_W_LET)
			js_fail_at(parser, declarator->line, declarator->column, "let cannot be a lexically bound name");

		/* The initializer, required for const and patterns outside a for-in or for-of head. */
		initialized = js_eat(parser, JS_P_ASSIGN);
		if (initialized)
			declarator->second = js_parse_assignment(parser);
		if (!initialized && !in_for && (kind == JS_P_CONST || declarator->first->kind != JS_NODE_IDENTIFIER))
			js_fail(parser, "missing initializer in a declaration");
		js_append(&node->first, &last, declarator);

		/* A comma, or the end. */
		if (parser->lexer.token.punctuator != JS_P_COMMA)
			break;
		js_next(parser);
	}

	/* Succeeded: the declaration. */
	return node;
}

/* Parses if (test) consequent [else alternate] (a function declaration is allowed as a branch in sloppy code). */
static struct js_node *
stmt_if(
	struct js_parser *parser)
{
	struct js_node *node;

	/* The test. */
	node = js_node_new(parser, JS_NODE_IF);
	js_next(parser);
	js_expect(parser, JS_P_LPAREN);
	node->first = js_parse_expression(parser);
	js_expect(parser, JS_P_RPAREN);

	/* The consequent. */
	js_next_regexp(parser);
	if (parser->lexer.token.keyword == JS_W_FUNCTION && !parser->context.strict) {
		node->second = stmt_function_declaration(parser, 0);
	} else {
		node->second = stmt_statement(parser);
	}

	/* The alternate. */
	if (parser->lexer.token.keyword == JS_W_ELSE) {
		js_next(parser);
		js_next_regexp(parser);
		if (parser->lexer.token.keyword == JS_W_FUNCTION && !parser->context.strict) {
			node->third = stmt_function_declaration(parser, 0);
		} else {
			node->third = stmt_statement(parser);
		}
	}

	/* Succeeded: the if. */
	return node;
}

/* Parses the for statements: for (;;), for-in, for-of and for await. */
static struct js_node *
stmt_for(
	struct js_parser *parser)
{
	struct js_node *node;
	struct js_node *init;
	int lexical;
	int awaiting;
	int starts_async;
	int starts_let;
	int no_in;
	int declarators;
	int covers;
	int expression_init;
	int becomes_target;
	int arrow_after;

	/* for, maybe await, then the head. */
	node = js_node_new(parser, JS_NODE_FOR);
	js_next(parser);
	awaiting = 0;
	if (parser->lexer.token.keyword == JS_W_AWAIT) {
		if (!parser->context.in_async && !(parser->module && !parser->context.in_function))
			js_fail(parser, "for await is only allowed in async functions");
		awaiting = 1;
		js_next(parser);
	}

	/* The head's parenthesis. */
	js_expect(parser, JS_P_LPAREN);
	js_next_regexp(parser);

	/* The init: nothing, var, let or const, or an expression (in is not an operator there). */
	init = NULL;
	expression_init = 0;
	covers = 0;
	starts_async = 0;
	starts_let = parser->lexer.token.keyword == JS_W_LET;
	no_in = parser->no_in;
	parser->no_in = 1;
	lexical = stmt_is_lexical(parser);
	if (parser->lexer.token.punctuator == JS_P_SEMICOLON) {
		init = NULL;
	} else if (parser->lexer.token.keyword == JS_W_VAR) {
		init = stmt_variables(parser, JS_P_VAR, 1);
	} else if (lexical) {
		init = stmt_variables(parser, lexical, 1);
	} else {
		arrow_after = stmt_next_is(parser, JS_P_ARROW);
		if (parser->lexer.token.keyword == JS_W_ASYNC && !arrow_after)
			starts_async = 1;
		covers = parser->cover_pending;
		parser->cover_pending = 0;
		init = js_parse_expression_cover(parser);
		expression_init = 1;
	}

	/* in is an operator again after the head. */
	parser->no_in = no_in;

	/* An expression head's pending shorthands are only valid when it becomes a for-in or for-of target. */
	if (expression_init) {
		becomes_target = 0;
		if (parser->lexer.token.keyword == JS_W_OF || parser->lexer.token.keyword == JS_W_IN)
			becomes_target = 1;
		if (parser->cover_pending && !becomes_target)
			js_fail_at(parser, parser->cover_line, parser->cover_column, "invalid shorthand property initializer");
		parser->cover_pending = covers;
	}

	/* for (... of ...): one declarator without an initializer, or a target. */
	if (init != NULL && parser->lexer.token.keyword == JS_W_OF) {
		if (init->kind == JS_NODE_VARIABLES) {
			declarators = 0;
			if (init->first != NULL && init->first->next == NULL && init->first->second == NULL)
				declarators = 1;
			if (!declarators)
				js_fail(parser, "for-of takes one declaration without an initializer");
		} else {
			if (starts_async && !awaiting && init->kind == JS_NODE_IDENTIFIER)
				js_fail(parser, "for (async of ...) is not allowed");
			if (starts_let)
				js_fail(parser, "for (let of ...) is not allowed");
			parser->no_in = 2;
			init = js_to_pattern(parser, init, 0);
			parser->no_in = no_in;
		}

		/* The for-of statement. */
		node->kind = JS_NODE_FOR_OF;
		if (awaiting)
			node->flags |= JS_FLAG_AWAIT;
		js_next(parser);
		node->first = init;
		node->second = js_parse_assignment(parser);
		js_expect(parser, JS_P_RPAREN);
		node->fourth = stmt_loop_body(parser);
		return node;
	}

	/* await makes only for-of. */
	if (awaiting)
		js_fail(parser, "for await needs of");

	/* for (... in ...). */
	if (init != NULL && parser->lexer.token.keyword == JS_W_IN) {
		if (init->kind == JS_NODE_VARIABLES) {
			if (init->first == NULL || init->first->next != NULL)
				js_fail(parser, "for-in takes one declaration");
			if (init->first->second != NULL &&
			    (parser->context.strict || init->op != JS_P_VAR || init->first->first->kind != JS_NODE_IDENTIFIER))
				js_fail(parser, "for-in declaration cannot have an initializer");
		} else {
			parser->no_in = 2;
			init = js_to_pattern(parser, init, 0);
			parser->no_in = no_in;
		}

		/* The for-in statement. */
		node->kind = JS_NODE_FOR_IN;
		js_next(parser);
		node->first = init;
		node->second = js_parse_expression(parser);
		js_expect(parser, JS_P_RPAREN);
		node->fourth = stmt_loop_body(parser);
		return node;
	}

	/* The classic for: const and patterns need their initializers here. */
	if (init != NULL && init->kind == JS_NODE_VARIABLES) {
		for (node->first = init->first; node->first != NULL; node->first = node->first->next) {
			if (node->first->second == NULL && (init->op == JS_P_CONST || node->first->first->kind != JS_NODE_IDENTIFIER))
				js_fail(parser, "missing initializer in a declaration");
		}
	}

	/* The init, then the test and the update. */
	node->first = init;
	js_expect(parser, JS_P_SEMICOLON);
	if (parser->lexer.token.punctuator != JS_P_SEMICOLON)
		node->second = js_parse_expression(parser);
	js_expect(parser, JS_P_SEMICOLON);
	if (parser->lexer.token.punctuator != JS_P_RPAREN)
		node->third = js_parse_expression(parser);
	js_expect(parser, JS_P_RPAREN);
	node->fourth = stmt_loop_body(parser);

	/* Succeeded: the for. */
	return node;
}

/* Parses a loop's body, inside the loop for break and continue. */
static struct js_node *
stmt_loop_body(
	struct js_parser *parser)
{
	struct js_node *body;
	int in_iteration;

	/* The body counts as inside a loop. */
	in_iteration = parser->context.in_iteration;
	parser->context.in_iteration = 1;
	js_next_regexp(parser);
	if (parser->lexer.token.keyword == JS_W_FUNCTION || parser->lexer.token.keyword == JS_W_CLASS)
		js_fail(parser, "a declaration cannot be a loop's body");
	body = stmt_statement(parser);
	parser->context.in_iteration = in_iteration;

	/* Succeeded: the body. */
	return body;
}

/* Parses while (test) body. */
static struct js_node *
stmt_while(
	struct js_parser *parser)
{
	struct js_node *node;

	/* The test and the body. */
	node = js_node_new(parser, JS_NODE_WHILE);
	js_next(parser);
	js_expect(parser, JS_P_LPAREN);
	node->first = js_parse_expression(parser);
	js_expect(parser, JS_P_RPAREN);
	node->fourth = stmt_loop_body(parser);

	/* Succeeded: the while. */
	return node;
}

/* Parses do body while (test) [;] (the semicolon may always be inserted after it). */
static struct js_node *
stmt_do(
	struct js_parser *parser)
{
	struct js_node *node;

	/* The body, then the test. */
	node = js_node_new(parser, JS_NODE_DO_WHILE);
	js_next(parser);
	node->fourth = stmt_loop_body(parser);
	if (parser->lexer.token.keyword != JS_W_WHILE)
		js_fail(parser, "while is expected after do's body");
	js_next(parser);
	js_expect(parser, JS_P_LPAREN);
	node->first = js_parse_expression(parser);
	js_expect(parser, JS_P_RPAREN);
	js_eat(parser, JS_P_SEMICOLON);

	/* Succeeded: the do-while. */
	return node;
}

/* Parses break or continue [label], checking that its target exists. */
static struct js_node *
stmt_jump(
	struct js_parser *parser,
	int kind)
{
	struct js_node *node;
	struct js_label *label;
	struct js_token *token;
	int same;

	/* The statement. */
	node = js_node_new(parser, kind);
	js_next(parser);
	token = &parser->lexer.token;

	/* A label on the same line. */
	if (token->kind == JS_TOKEN_IDENTIFIER && !token->newline_before) {
		node->text = token->text;
		node->word = token->word;
		node->text_length = token->text_length;
		for (label = parser->context.labels; label != NULL; label = label->outer) {
			same = js_text_equal(label->name, label->length, token->text, token->text_length);
			if (same)
				break;
		}

		/* The label must be in force. */
		if (label == NULL)
			js_fail(parser, "undefined label");
		if (kind == JS_NODE_CONTINUE && !label->loop)
			js_fail(parser, "continue must target a loop's label");
		js_next(parser);
	} else if (kind == JS_NODE_CONTINUE && !parser->context.in_iteration) {
		js_fail(parser, "continue must be inside a loop");
	} else if (kind == JS_NODE_BREAK && !parser->context.in_iteration && !parser->context.in_switch) {
		js_fail(parser, "break must be inside a loop or a switch");
	}

	/* The end of the statement. */
	js_semicolon(parser);

	/* Succeeded: the jump. */
	return node;
}

/* Parses return [value] inside a function. */
static struct js_node *
stmt_return(
	struct js_parser *parser)
{
	struct js_node *node;
	struct js_token *token;

	/* Only a function returns. */
	if (!parser->context.in_function)
		js_fail(parser, "return outside a function");
	node = js_node_new(parser, JS_NODE_RETURN);
	js_next(parser);

	/* The value, when one is on the same line. */
	token = &parser->lexer.token;
	if (!token->newline_before && token->kind != JS_TOKEN_END && parser->lexer.token.punctuator != JS_P_SEMICOLON &&
	    parser->lexer.token.punctuator != JS_P_RBRACE)
		node->first = js_parse_expression(parser);
	js_semicolon(parser);

	/* Succeeded: the return. */
	return node;
}

/* Parses with (object) body, which strict code forbids. */
static struct js_node *
stmt_with(
	struct js_parser *parser)
{
	struct js_node *node;

	/* Not in strict code. */
	if (parser->context.strict)
		js_fail(parser, "with is not allowed in strict code");

	/* The object and the body. */
	node = js_node_new(parser, JS_NODE_WITH);
	js_next(parser);
	js_expect(parser, JS_P_LPAREN);
	node->first = js_parse_expression(parser);
	js_expect(parser, JS_P_RPAREN);
	js_next_regexp(parser);
	if (parser->lexer.token.keyword == JS_W_FUNCTION || parser->lexer.token.keyword == JS_W_CLASS)
		js_fail(parser, "a declaration cannot be with's body");
	node->fourth = stmt_statement(parser);

	/* Succeeded: the with. */
	return node;
}

/* Parses switch (value) { cases } (one default at most). */
static struct js_node *
stmt_switch(
	struct js_parser *parser)
{
	struct js_node *node;
	struct js_node *clause;
	struct js_node *last;
	struct js_node *last_statement;
	int defaults;
	int in_switch;

	/* The discriminant. */
	node = js_node_new(parser, JS_NODE_SWITCH);
	js_next(parser);
	js_expect(parser, JS_P_LPAREN);
	node->first = js_parse_expression(parser);
	js_expect(parser, JS_P_RPAREN);
	js_expect(parser, JS_P_LBRACE);

	/* The clauses, inside the switch for break. */
	in_switch = parser->context.in_switch;
	parser->context.in_switch = 1;
	last = NULL;
	defaults = 0;
	while (parser->lexer.token.punctuator != JS_P_RBRACE) {
		/* case test: or default:. */
		clause = js_node_new(parser, JS_NODE_CASE);
		if (parser->lexer.token.keyword == JS_W_CASE) {
			js_next(parser);
			clause->first = js_parse_expression(parser);
		} else if (parser->lexer.token.keyword == JS_W_DEFAULT) {
			defaults++;
			if (defaults > 1)
				js_fail(parser, "more than one default in a switch");
			js_next(parser);
		} else {
			js_fail(parser, "case or default is expected");
		}

		/* The clause's colon. */
		js_expect(parser, JS_P_COLON);

		/* Its statements, up to the next clause or the end. */
		last_statement = NULL;
		for (;;) {
			js_next_regexp(parser);
			if (parser->lexer.token.punctuator == JS_P_RBRACE || parser->lexer.token.keyword == JS_W_CASE || parser->lexer.token.keyword == JS_W_DEFAULT)
				break;
			js_append(&clause->second, &last_statement, js_parse_statement_list_item(parser));
		}

		/* The clause joins the switch. */
		js_append(&node->second, &last, clause);
	}

	/* Past the closing brace, outside the switch again. */
	js_next(parser);
	parser->context.in_switch = in_switch;

	/* Succeeded: the switch. */
	return node;
}

/* Parses throw value (on the same line). */
static struct js_node *
stmt_throw(
	struct js_parser *parser)
{
	struct js_node *node;

	/* No line break before the value. */
	node = js_node_new(parser, JS_NODE_THROW);
	js_next(parser);
	if (parser->lexer.token.newline_before)
		js_fail(parser, "no line break is allowed after throw");
	node->first = js_parse_expression(parser);
	js_semicolon(parser);

	/* Succeeded: the throw. */
	return node;
}

/* Parses try block [catch [(parameter)] block] [finally block] (a catch or a finally at least). */
static struct js_node *
stmt_try(
	struct js_parser *parser)
{
	struct js_node *node;

	/* The block. */
	node = js_node_new(parser, JS_NODE_TRY);
	js_next(parser);
	node->first = stmt_block(parser);

	/* The catch, with its optional parameter. */
	if (parser->lexer.token.keyword == JS_W_CATCH) {
		js_next(parser);
		if (parser->lexer.token.punctuator == JS_P_LPAREN) {
			js_next(parser);
			node->second = js_parse_binding_target(parser);
			js_expect(parser, JS_P_RPAREN);
		}

		/* The catch block. */
		node->third = stmt_block(parser);
	}

	/* The finally. */
	if (parser->lexer.token.keyword == JS_W_FINALLY) {
		js_next(parser);
		node->fourth = stmt_block(parser);
	}

	/* One of the two must be there. */
	if (node->third == NULL && node->fourth == NULL)
		js_fail(parser, "try needs catch or finally");

	/* Succeeded: the try. */
	return node;
}

/* Parses label: body, the label in force inside it. */
static struct js_node *
stmt_labeled(
	struct js_parser *parser)
{
	struct js_node *node;
	struct js_node *name;
	struct js_label *label;
	struct js_label *outer;
	int same;

	/* The label, which must not repeat one in force. */
	node = js_node_new(parser, JS_NODE_LABELED);
	name = js_parse_identifier_reference(parser);
	node->text = name->text;
	node->word = name->word;
	node->text_length = name->text_length;
	for (outer = parser->context.labels; outer != NULL; outer = outer->outer) {
		same = js_text_equal(outer->name, outer->length, name->text, name->text_length);
		if (same)
			js_fail_at(parser, name->line, name->column, "duplicate label");
	}

	/* The colon after the label. */
	js_expect(parser, JS_P_COLON);

	/* The label, a loop's when the body is one. */
	label = wb_arena_zalloc(parser->arena, sizeof(*label));
	if (label == NULL)
		js_fail(parser, "out of memory");
	label->name = name->text;
	label->length = name->text_length;
	js_next_regexp(parser);
	if (parser->lexer.token.keyword == JS_W_FOR || parser->lexer.token.keyword == JS_W_WHILE || parser->lexer.token.keyword == JS_W_DO)
		label->loop = 1;
	label->outer = parser->context.labels;
	parser->context.labels = label;

	/* The body (a function declaration only in sloppy code). */
	if (parser->lexer.token.keyword == JS_W_FUNCTION) {
		if (parser->context.strict)
			js_fail(parser, "a labeled function declaration in strict code");
		node->fourth = stmt_function_declaration(parser, 0);
	} else {
		node->fourth = stmt_statement(parser);
	}

	/* The label is out of force after its body. */
	parser->context.labels = label->outer;

	/* Succeeded: the labeled statement. */
	return node;
}

/* Parses an expression statement (which cannot start like a declaration or an object). */
static struct js_node *
stmt_expression(
	struct js_parser *parser)
{
	struct js_node *node;
	int bracket_after;
	int async_function;

	/* The forms that would be read as declarations. */
	if (parser->lexer.token.keyword == JS_W_FUNCTION || parser->lexer.token.keyword == JS_W_CLASS)
		js_fail(parser, "a declaration is not allowed here");
	bracket_after = stmt_next_is(parser, JS_P_LBRACKET);
	if (parser->lexer.token.keyword == JS_W_LET && bracket_after)
		js_fail(parser, "a lexical declaration is not allowed here");
	async_function = stmt_is_async_function(parser);
	if (async_function)
		js_fail(parser, "an async function declaration is not allowed here");

	/* The expression and the end of the statement. */
	node = js_node_new(parser, JS_NODE_EXPRESSION_STATEMENT);
	node->first = js_parse_expression(parser);
	js_semicolon(parser);

	/* Succeeded: the statement. */
	return node;
}

/* Parses a function declaration from its function keyword (after async for an async one). */
static struct js_node *
stmt_function_declaration(
	struct js_parser *parser,
	int async_function)
{
	struct js_node *node;
	uint32_t flags;

	/* async, function, and *. */
	flags = 0;
	if (async_function) {
		flags |= JS_FLAG_ASYNC;
		js_next(parser);
	}

	/* function, then maybe * for a generator. */
	js_next(parser);
	if (parser->lexer.token.punctuator == JS_P_STAR) {
		js_next(parser);
		flags |= JS_FLAG_GENERATOR;
	}

	/* The rest. */
	node = js_parse_function(parser, flags, 1, 0);

	/* Succeeded: the declaration. */
	return node;
}

/* Parses one member of a class: a method, a field or a static block. */
static struct js_node *
stmt_class_member(
	struct js_parser *parser,
	int *constructors)
{
	struct js_node *member;
	struct js_node *key;
	struct js_context saved;
	uint32_t flags;
	uint32_t function_flags;
	int kind;
	int is_static;
	int named;
	int broken;
	int is_constructor;

	/* static, when a name (or a block) follows it. */
	member = js_node_new(parser, JS_NODE_METHOD);
	is_static = 0;
	if (parser->lexer.token.keyword == JS_W_STATIC) {
		named = js_next_is_name(parser, 1);
		if (!named)
			named = stmt_next_is(parser, JS_P_LBRACE);
		if (named) {
			is_static = 1;
			js_next(parser);
			if (parser->lexer.token.punctuator == JS_P_LBRACE) {
				member = stmt_static_block(parser);
				return member;
			}
		}
	}

	/* The modifiers: async, *, get, set. */
	kind = JS_PROPERTY_METHOD;
	function_flags = 0;
	if (parser->lexer.token.keyword == JS_W_ASYNC) {
		named = js_next_is_name(parser, 1);
		broken = stmt_next_is(parser, -2);
		if (named && !broken) {
			js_next(parser);
			function_flags |= JS_FLAG_ASYNC;
		}
	}

	/* * makes a generator method. */
	if (parser->lexer.token.punctuator == JS_P_STAR) {
		js_next(parser);
		function_flags |= JS_FLAG_GENERATOR;
	}

	/* get and set make accessors. */
	if (function_flags == 0 && (parser->lexer.token.keyword == JS_W_GET || parser->lexer.token.keyword == JS_W_SET)) {
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
	if (is_static)
		flags |= JS_FLAG_STATIC;
	key = js_parse_property_name(parser, &flags);
	member->first = key;
	member->flags = flags;
	if (key->kind == JS_NODE_PRIVATE_NAME && key->word == JS_W_CONSTRUCTOR)
		js_fail_at(parser, key->line, key->column, "#constructor is not allowed");

	/* A method; the constructor is a plain method named constructor (at most one). */
	if (parser->lexer.token.punctuator == JS_P_LPAREN) {
		is_constructor = 0;
		if (!is_static && (flags & JS_FLAG_COMPUTED) == 0U && (key->kind == JS_NODE_IDENTIFIER || key->kind == JS_NODE_STRING) &&
		    key->word == JS_W_CONSTRUCTOR)
			is_constructor = 1;
		if (is_constructor) {
			if (kind != JS_PROPERTY_METHOD || function_flags != 0)
				js_fail_at(parser, key->line, key->column, "a class constructor cannot be a getter, setter, generator or async");
			(*constructors)++;
			if (*constructors > 1)
				js_fail_at(parser, key->line, key->column, "a class may only have one constructor");
			kind = JS_PROPERTY_CONSTRUCTOR;
		}

		/* A static method cannot be named prototype. */
		if (is_static && (flags & JS_FLAG_COMPUTED) == 0U && (key->kind == JS_NODE_IDENTIFIER || key->kind == JS_NODE_STRING) &&
		    key->word == JS_W_PROTOTYPE)
			js_fail_at(parser, key->line, key->column, "a static method cannot be named prototype");
		member->op = kind;
		member->second = js_parse_method(parser, key, kind, function_flags);
		return member;
	}

	/* Otherwise a field, which takes no modifier and cannot be named constructor (or prototype, when static). */
	if (kind != JS_PROPERTY_METHOD || function_flags != 0)
		js_fail(parser, "a method's parameters are expected");
	member->kind = JS_NODE_FIELD;
	if ((flags & JS_FLAG_COMPUTED) == 0U && (key->kind == JS_NODE_IDENTIFIER || key->kind == JS_NODE_STRING)) {
		if (key->word == JS_W_CONSTRUCTOR)
			js_fail_at(parser, key->line, key->column, "a field cannot be named constructor");
		if (is_static && key->word == JS_W_PROTOTYPE)
			js_fail_at(parser, key->line, key->column, "a static field cannot be named prototype");
	}

	/* The initializer, in a function-like context of its own. */
	if (parser->lexer.token.punctuator == JS_P_ASSIGN) {
		js_next(parser);
		js_enter_function(parser, &saved, 0, 1);
		parser->context.in_function = 0;
		parser->context.in_class_field = 1;
		parser->context.no_arguments = 1;
		member->second = js_parse_assignment(parser);
		parser->context = saved;
	}

	/* The end of the field. */
	js_semicolon(parser);

	/* Succeeded: the field. */
	return member;
}

/* Parses static { statements }: a function-like body without return, await or arguments. */
static struct js_node *
stmt_static_block(
	struct js_parser *parser)
{
	struct js_context saved;
	struct js_node *block;
	struct js_node *last;

	/* The block in its own context. */
	block = js_node_new(parser, JS_NODE_STATIC_BLOCK);
	block->flags = JS_FLAG_STATIC;
	js_enter_function(parser, &saved, 0, 1);
	parser->context.in_function = 0;
	parser->context.in_static_block = 1;
	parser->context.no_arguments = 1;
	js_expect(parser, JS_P_LBRACE);
	last = NULL;
	for (;;) {
		js_next_regexp(parser);
		if (parser->lexer.token.punctuator == JS_P_RBRACE)
			break;
		js_append(&block->first, &last, js_parse_statement_list_item(parser));
	}

	/* Past the closing brace, in the context outside again. */
	js_next(parser);
	parser->context = saved;

	/* Succeeded: the static block. */
	return block;
}

/* Parses an import declaration. */
static struct js_node *
stmt_import(
	struct js_parser *parser)
{
	struct js_node *node;
	struct js_node *last;
	struct js_node *specifier;
	int more;
	int default_binding;

	/* import "module"; */
	node = js_node_new(parser, JS_NODE_IMPORT);
	js_next(parser);
	if (parser->lexer.token.kind == JS_TOKEN_STRING) {
		node->second = stmt_module_string(parser);
		stmt_import_attributes(parser);
		js_semicolon(parser);
		return node;
	}

	/* The default binding (a name, which may even be from: import from from "m"). */
	last = NULL;
	more = 1;
	default_binding = 0;
	if (parser->lexer.token.kind == JS_TOKEN_IDENTIFIER && parser->lexer.token.keyword != JS_W_FROM)
		default_binding = 1;
	if (parser->lexer.token.kind == JS_TOKEN_IDENTIFIER && parser->lexer.token.keyword == JS_W_FROM)
		default_binding = stmt_next_is(parser, -3);
	if (default_binding) {
		specifier = js_node_new(parser, JS_NODE_IMPORT_SPECIFIER);
		specifier->second = stmt_binding_identifier(parser);
		js_append(&node->first, &last, specifier);
		more = js_eat(parser, JS_P_COMMA);
	}

	/* * as name, or { specifiers }. */
	if (!more) {
		/* Only the default binding. */
	} else if (parser->lexer.token.punctuator == JS_P_STAR) {
		js_next(parser);
		if (parser->lexer.token.keyword != JS_W_AS)
			js_fail(parser, "as is expected after import *");
		js_next(parser);
		specifier = stmt_import_specifier(parser, 0);
		specifier->flags |= JS_FLAG_ALL;
		js_append(&node->first, &last, specifier);
	} else if (parser->lexer.token.punctuator == JS_P_LBRACE) {
		js_next(parser);
		while (parser->lexer.token.punctuator != JS_P_RBRACE) {
			specifier = stmt_import_specifier(parser, 1);
			js_append(&node->first, &last, specifier);
			if (parser->lexer.token.punctuator != JS_P_COMMA)
				break;
			js_next(parser);
		}

		/* The closing brace. */
		js_expect(parser, JS_P_RBRACE);
	} else {
		js_fail(parser, "an import clause is expected");
	}

	/* from "module" and its attributes. */
	if (parser->lexer.token.keyword != JS_W_FROM)
		js_fail(parser, "from is expected");
	js_next(parser);
	node->second = stmt_module_string(parser);
	stmt_import_attributes(parser);
	js_semicolon(parser);

	/* Succeeded: the import. */
	return node;
}

/* Parses one import specifier: name [as local] in braces, or the local name of a namespace import. */
static struct js_node *
stmt_import_specifier(
	struct js_parser *parser,
	int named)
{
	struct js_node *specifier;
	struct js_node *imported;
	int reserved;

	/* A namespace's local name. */
	specifier = js_node_new(parser, JS_NODE_IMPORT_SPECIFIER);
	if (!named) {
		specifier->second = stmt_binding_identifier(parser);
		return specifier;
	}

	/* The imported name (any name or a string), then maybe as and the local name. */
	imported = stmt_module_name(parser);
	specifier->first = imported;
	if (parser->lexer.token.keyword == JS_W_AS) {
		js_next(parser);
		specifier->second = stmt_binding_identifier(parser);
		return specifier;
	}

	/* Without as, the imported name must be a binding name itself. */
	reserved = js_word_is_reserved(imported->text, imported->text_length, 1);
	if (imported->kind != JS_NODE_IDENTIFIER || reserved)
		js_fail_at(parser, imported->line, imported->column, "an imported name needs as a local name");
	specifier->second = imported;

	/* Succeeded: the specifier. */
	return specifier;
}

/* The name of the hidden binding that holds the value of export default expression. */
static const uint16_t stmt_default_name[] = { '*', 'd', 'e', 'f', 'a', 'u', 'l', 't', '*' };

/* Parses an export declaration. */
static struct js_node *
stmt_export(
	struct js_parser *parser)
{
	struct js_node *node;
	struct js_node *last;
	struct js_node *specifier;
	struct js_node *declarator;
	uint32_t flags;
	int lexical;
	int async_function;

	/* export * [as name] from "module". */
	node = js_node_new(parser, JS_NODE_EXPORT);
	js_next(parser);
	if (parser->lexer.token.punctuator == JS_P_STAR) {
		js_next(parser);
		node->flags |= JS_FLAG_ALL;
		if (parser->lexer.token.keyword == JS_W_AS) {
			js_next(parser);
			node->first = stmt_module_name(parser);
		}

		/* from and the module. */
		if (parser->lexer.token.keyword != JS_W_FROM)
			js_fail(parser, "from is expected");
		js_next(parser);
		node->second = stmt_module_string(parser);
		stmt_import_attributes(parser);
		js_semicolon(parser);
		return node;
	}

	/* export default: a function or class declaration, or an expression. */
	if (parser->lexer.token.keyword == JS_W_DEFAULT) {
		node->flags |= JS_FLAG_DEFAULT;
		js_next(parser);
		js_next_regexp(parser);
		async_function = stmt_is_async_function(parser);
		flags = 0;
		if (parser->lexer.token.keyword == JS_W_FUNCTION) {
			js_next(parser);
			if (parser->lexer.token.punctuator == JS_P_STAR) {
				js_next(parser);
				flags |= JS_FLAG_GENERATOR;
			}

			/* The function (a default export may have no name). */
			node->first = js_parse_function(parser, flags, 1, 1);
		} else if (async_function) {
			js_next(parser);
			js_next(parser);
			flags |= JS_FLAG_ASYNC;
			if (parser->lexer.token.punctuator == JS_P_STAR) {
				js_next(parser);
				flags |= JS_FLAG_GENERATOR;
			}

			/* The async function. */
			node->first = js_parse_function(parser, flags, 1, 1);
		} else if (parser->lexer.token.keyword == JS_W_CLASS) {
			node->first = js_parse_class(parser, 2);
		} else {
			/* An expression is the value of a hidden constant that is exported as default. */
			node->flags &= ~JS_FLAG_DEFAULT;
			declarator = js_node_new(parser, JS_NODE_DECLARATOR);
			declarator->first = js_node_new(parser, JS_NODE_IDENTIFIER);
			declarator->first->text = stmt_default_name;
			declarator->first->text_length = sizeof(stmt_default_name) / sizeof(stmt_default_name[0]);
			declarator->second = js_parse_assignment(parser);
			js_semicolon(parser);
			node->first = js_node_new(parser, JS_NODE_VARIABLES);
			node->first->op = JS_P_CONST;
			node->first->first = declarator;
		}

		/* The default export. */
		return node;
	}

	/* export { specifiers } [from "module"]. */
	if (parser->lexer.token.punctuator == JS_P_LBRACE) {
		js_next(parser);
		last = NULL;
		while (parser->lexer.token.punctuator != JS_P_RBRACE) {
			specifier = js_node_new(parser, JS_NODE_EXPORT_SPECIFIER);
			specifier->first = stmt_module_name(parser);
			specifier->second = specifier->first;
			if (parser->lexer.token.keyword == JS_W_AS) {
				js_next(parser);
				specifier->second = stmt_module_name(parser);
			}

			/* The specifier joins the list. */
			js_append(&node->first, &last, specifier);
			if (parser->lexer.token.punctuator != JS_P_COMMA)
				break;
			js_next(parser);
		}

		/* The closing brace, then maybe the module re-exported from. */
		js_expect(parser, JS_P_RBRACE);
		if (parser->lexer.token.keyword == JS_W_FROM) {
			js_next(parser);
			node->second = stmt_module_string(parser);
			stmt_import_attributes(parser);
		}

		/* The end of the export. */
		js_semicolon(parser);
		return node;
	}

	/* export and a declaration. */
	js_next_regexp(parser);
	lexical = stmt_is_lexical(parser);
	async_function = stmt_is_async_function(parser);
	if (parser->lexer.token.keyword == JS_W_VAR) {
		node->first = stmt_variables(parser, JS_P_VAR, 0);
		js_semicolon(parser);
	} else if (lexical) {
		node->first = stmt_variables(parser, lexical, 0);
		js_semicolon(parser);
	} else if (parser->lexer.token.keyword == JS_W_FUNCTION) {
		node->first = stmt_function_declaration(parser, 0);
	} else if (async_function) {
		node->first = stmt_function_declaration(parser, 1);
	} else if (parser->lexer.token.keyword == JS_W_CLASS) {
		node->first = js_parse_class(parser, 1);
	} else {
		js_fail(parser, "a declaration is expected after export");
	}

	/* Succeeded: the export. */
	return node;
}

/* Parses a name of a module's binding: any identifier name, or a string. */
static struct js_node *
stmt_module_name(
	struct js_parser *parser)
{
	struct js_node *node;
	struct js_token *token;

	/* A word or a string. */
	token = &parser->lexer.token;
	if (token->kind == JS_TOKEN_IDENTIFIER) {
		node = js_node_new(parser, JS_NODE_IDENTIFIER);
	} else if (token->kind == JS_TOKEN_STRING) {
		node = js_node_new(parser, JS_NODE_STRING);
	} else {
		js_fail(parser, "a name is expected");
		return NULL;
	}

	/* The name's text and word. */
	node->text = token->text;
	node->word = token->word;
	node->text_length = token->text_length;
	js_next(parser);

	/* Succeeded: the name. */
	return node;
}

/* Parses a module specifier: a string. */
static struct js_node *
stmt_module_string(
	struct js_parser *parser)
{
	struct js_node *node;

	/* A string. */
	if (parser->lexer.token.kind != JS_TOKEN_STRING)
		js_fail(parser, "a module specifier string is expected");
	node = js_node_new(parser, JS_NODE_STRING);
	node->text = parser->lexer.token.text;
	node->word = parser->lexer.token.word;
	node->text_length = parser->lexer.token.text_length;
	js_next(parser);

	/* Succeeded: the specifier. */
	return node;
}

/* Skips an import's attributes: with { key: "value", ... }. */
static void
stmt_import_attributes(
	struct js_parser *parser)
{
	uint32_t flags;

	/* Only with and a brace (a line break may come before with). */
	if (parser->lexer.token.keyword != JS_W_WITH)
		return;
	js_next(parser);
	js_expect(parser, JS_P_LBRACE);
	while (parser->lexer.token.punctuator != JS_P_RBRACE) {
		flags = 0;
		(void)js_parse_property_name(parser, &flags);
		js_expect(parser, JS_P_COLON);
		(void)stmt_module_string(parser);
		if (parser->lexer.token.punctuator != JS_P_COMMA)
			break;
		js_next(parser);
	}

	/* The closing brace. */
	js_expect(parser, JS_P_RBRACE);
}

/*
 * Tells whether a let or const declaration starts here: const always;
 * let when followed by a name, [ or { (in sloppy code let is otherwise a
 * name).  Reports JS_P_LET, JS_P_CONST or 0.
 */
static int
stmt_is_lexical(
	struct js_parser *parser)
{
	struct js_lexer saved;
	struct js_token *token;
	int lexical;

	/* const. */
	if (parser->lexer.token.keyword == JS_W_CONST)
		return JS_P_CONST;
	if (parser->lexer.token.keyword != JS_W_LET)
		return 0;

	/* let, by what follows it. */
	js_lexer_save(&parser->lexer, &saved);
	js_next(parser);
	token = &parser->lexer.token;
	lexical = 0;
	if (parser->lexer.token.punctuator == JS_P_LBRACKET || parser->lexer.token.punctuator == JS_P_LBRACE)
		lexical = JS_P_LET;
	if (token->kind == JS_TOKEN_IDENTIFIER && (!token->newline_before || token->keyword != JS_W_IN))
		lexical = JS_P_LET;
	if (token->kind == JS_TOKEN_IDENTIFIER && (token->keyword == JS_W_IN || token->keyword == JS_W_INSTANCEOF || token->keyword == JS_W_OF))
		lexical = 0;
	if (parser->context.strict && lexical == 0 && token->kind == JS_TOKEN_IDENTIFIER)
		lexical = JS_P_LET;
	js_lexer_restore(&parser->lexer, &saved);

	/* Reports it. */
	return lexical;
}

/*
 * Tells whether the token after the current one is a punctuator; the
 * special values ask other questions: -1 whether it is function on the
 * same line (async function), -2 whether a line break comes before it,
 * -3 whether it is a comma or the word from (import from from "m").
 */
static int
stmt_next_is(
	struct js_parser *parser,
	int punctuator)
{
	struct js_lexer saved;
	int found;

	/* Looks one token ahead and comes back. */
	js_lexer_save(&parser->lexer, &saved);
	js_next(parser);
	found = 0;
	if (punctuator == -1 && parser->lexer.token.keyword == JS_W_FUNCTION && !parser->lexer.token.newline_before)
		found = 1;
	if (punctuator == -2 && parser->lexer.token.newline_before)
		found = 1;
	if (punctuator == -3 && (parser->lexer.token.punctuator == JS_P_COMMA || parser->lexer.token.keyword == JS_W_FROM))
		found = 1;
	if (punctuator >= 0 && parser->lexer.token.punctuator == punctuator)
		found = 1;
	js_lexer_restore(&parser->lexer, &saved);

	/* Reports what was there. */
	return found;
}

/* Tells whether an async function starts here: async, then function on the same line. */
static int
stmt_is_async_function(
	struct js_parser *parser)
{
	int function_after;

	/* The word async. */
	if (parser->lexer.token.keyword != JS_W_ASYNC)
		return 0;

	/* function after it, without a line break. */
	function_after = stmt_next_is(parser, -1);

	/* Reports it. */
	return function_after;
}

/* Parses [ binding elements ]. */
static struct js_node *
stmt_binding_array(
	struct js_parser *parser)
{
	struct js_node *pattern;
	struct js_node *last;
	struct js_node *element;

	/* The elements: holes, a rest last, targets with defaults. */
	pattern = js_node_new(parser, JS_NODE_ARRAY_PATTERN);
	js_next(parser);
	last = NULL;
	while (parser->lexer.token.punctuator != JS_P_RBRACKET) {
		if (parser->lexer.token.punctuator == JS_P_COMMA) {
			element = js_node_new(parser, JS_NODE_HOLE);
			js_next(parser);
			js_append(&pattern->first, &last, element);
			continue;
		}

		/* A rest element ends the pattern. */
		if (parser->lexer.token.punctuator == JS_P_ELLIPSIS) {
			element = js_node_new(parser, JS_NODE_REST);
			js_next(parser);
			element->first = js_parse_binding_target(parser);
			js_append(&pattern->first, &last, element);
			if (parser->lexer.token.punctuator != JS_P_RBRACKET)
				js_fail(parser, "a rest element must be last");
			break;
		}

		/* A target with its default. */
		element = stmt_binding_element(parser);
		js_append(&pattern->first, &last, element);
		if (parser->lexer.token.punctuator != JS_P_COMMA)
			break;
		js_next(parser);
	}

	/* The closing bracket. */
	js_expect(parser, JS_P_RBRACKET);

	/* Succeeded: the pattern. */
	return pattern;
}

/* Parses { binding properties }. */
static struct js_node *
stmt_binding_object(
	struct js_parser *parser)
{
	struct js_node *pattern;
	struct js_node *last;
	struct js_node *property;
	struct js_node *key;
	struct js_node *target;
	struct js_node *with_default;
	uint32_t flags;
	int reserved;

	/* The properties: a rest name last, key: element, or a shorthand name with a default. */
	pattern = js_node_new(parser, JS_NODE_OBJECT_PATTERN);
	js_next(parser);
	last = NULL;
	while (parser->lexer.token.punctuator != JS_P_RBRACE) {
		if (parser->lexer.token.punctuator == JS_P_ELLIPSIS) {
			property = js_node_new(parser, JS_NODE_REST);
			js_next(parser);
			property->first = stmt_binding_identifier(parser);
			js_append(&pattern->first, &last, property);
			if (parser->lexer.token.punctuator != JS_P_RBRACE)
				js_fail(parser, "a rest property must be last");
			break;
		}

		/* The key, then the target: after :, or the shorthand name itself. */
		property = js_node_new(parser, JS_NODE_PROPERTY);
		flags = 0;
		key = js_parse_property_name(parser, &flags);
		property->first = key;
		property->flags = flags;
		if (key->kind == JS_NODE_PRIVATE_NAME)
			js_fail_at(parser, key->line, key->column, "a private name in a pattern");
		if (parser->lexer.token.punctuator == JS_P_COLON) {
			js_next(parser);
			property->second = stmt_binding_element(parser);
		} else {
			if (key->kind != JS_NODE_IDENTIFIER || (flags & JS_FLAG_COMPUTED) != 0U)
				js_fail(parser, "a binding property needs a target");
			reserved = js_word_is_reserved(key->text, key->text_length, parser->context.strict);
			if (reserved)
				js_fail_at(parser, key->line, key->column, "a reserved word cannot be bound");
			js_check_binding_name(parser, key);
			target = js_node_new(parser, JS_NODE_IDENTIFIER);
			*target = *key;
			target->next = NULL;
			property->flags |= JS_FLAG_SHORTHAND;
			if (parser->lexer.token.punctuator == JS_P_ASSIGN) {
				js_next(parser);
				with_default = js_node_new(parser, JS_NODE_ASSIGNMENT_PATTERN);
				with_default->first = target;
				with_default->second = js_parse_assignment(parser);
				target = with_default;
			}

			/* The shorthand's target. */
			property->second = target;
		}

		/* The property joins the pattern. */
		js_append(&pattern->first, &last, property);
		if (parser->lexer.token.punctuator != JS_P_COMMA)
			break;
		js_next(parser);
	}

	/* The closing brace. */
	js_expect(parser, JS_P_RBRACE);

	/* Succeeded: the pattern. */
	return pattern;
}

/* Parses a binding target with an optional default. */
static struct js_node *
stmt_binding_element(
	struct js_parser *parser)
{
	struct js_node *target;
	struct js_node *element;

	/* The target. */
	target = js_parse_binding_target(parser);
	if (parser->lexer.token.punctuator != JS_P_ASSIGN)
		return target;

	/* Its default. */
	element = js_node_new(parser, JS_NODE_ASSIGNMENT_PATTERN);
	js_next(parser);
	element->first = target;
	element->second = js_parse_assignment(parser);

	/* Succeeded: the target with its default. */
	return element;
}

/* Parses a binding name: an identifier that can be bound here. */
static struct js_node *
stmt_binding_identifier(
	struct js_parser *parser)
{
	struct js_node *name;

	/* The identifier, not eval or arguments in strict code. */
	name = js_parse_identifier_reference(parser);
	js_check_binding_name(parser, name);

	/* Succeeded: the name. */
	return name;
}
