/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parser of awk: a recursive descent over the tokens, which builds the
 * rules and the functions of the program as trees of nodes.
 *
 * The levels of expressions, from the loosest: assignment, ?:, ||, &&, in,
 * ~ and !~, the relational operators, | getline, concatenation, + and -,
 * * / and %, the unary operators, ^, ++ and -- after an lvalue, and the
 * primary expressions ($, ++ and -- before an lvalue, grouping, names,
 * calls, constants and getline).  Inside the arguments of print and printf
 * a > outside parentheses is where the output goes, not a comparison.
 */

#include "userland/base/awk/awk.h"

#include <stdlib.h>
#include <string.h>

/* The fewest and the most arguments of each built-in function. */
struct builtin_arity {
	size_t fewest;
	size_t most;
};

/*
 * The arity of the built-in functions, indexed by BUILTIN_*.  The table is
 * constant.
 */
static const struct builtin_arity builtin_arities[] = {
	{ 0, 1 },	/* length */
	{ 2, 3 },	/* substr */
	{ 2, 2 },	/* index */
	{ 2, 3 },	/* split */
	{ 2, 3 },	/* sub */
	{ 2, 3 },	/* gsub */
	{ 2, 2 },	/* match */
	{ 1, 1000000 },	/* sprintf */
	{ 1, 1 },	/* sin */
	{ 1, 1 },	/* cos */
	{ 2, 2 },	/* atan2 */
	{ 1, 1 },	/* exp */
	{ 1, 1 },	/* log */
	{ 1, 1 },	/* sqrt */
	{ 1, 1 },	/* int */
	{ 0, 0 },	/* rand */
	{ 0, 1 },	/* srand */
	{ 1, 1 },	/* tolower */
	{ 1, 1 },	/* toupper */
	{ 1, 1 },	/* system */
	{ 1, 1 },	/* close */
	{ 0, 1 }	/* fflush */
};

/*
 * The token being looked at, and the one after it when the parser has
 * peeked.  They live while the program is parsed.
 */
static struct token current;
static struct token lookahead;
static int have_lookahead;

/*
 * Set while the arguments of print or printf are parsed outside any
 * parentheses, where > is where the output goes.
 */
static int in_print_arguments;

/* The function being parsed, NULL outside every function. */
static struct function *parsing_function;

/* How many loops the statement being parsed is inside. */
static int loop_depth;

/* The kind of rule being parsed, for next, which BEGIN and END refuse. */
static int parsing_rule_kind;

static void advance(void);
static int peek_kind(void);
static void expect(int kind, const char *what);
static void syntax_error(void);
static void skip_newlines(void);
static void skip_terminators(void);
static struct node *new_node(int kind);
static void add_rule(int kind, struct node *pattern, struct node *pattern_end, struct node *action);
static void parse_item(void);
static void parse_function(void);
static struct function *find_function(const char *name);
static struct node *parse_block(void);
static struct node *parse_statement(void);
static struct node *parse_body(void);
static struct node *parse_if(void);
static struct node *parse_while(void);
static struct node *parse_do(void);
static struct node *parse_for(void);
static struct node *parse_simple_statement(void);
static struct node *parse_print(void);
static struct node *parse_delete(void);
static void parse_terminator(void);
static int is_terminator(int kind);
static struct node *parse_expression(void);
static struct node *parse_expression_list(int closing, size_t *count);
static struct node *parse_ternary(void);
static struct node *parse_or(void);
static struct node *parse_and(void);
static struct node *parse_in(void);
static struct node *parse_match(void);
static struct node *parse_relational(void);
static struct node *parse_pipe_getline(void);
static struct node *parse_concatenation(void);
static int starts_concatenation(int kind);
static struct node *parse_additive(void);
static struct node *parse_multiplicative(void);
static struct node *parse_unary(void);
static struct node *parse_power(void);
static struct node *parse_postfix(void);
static struct node *parse_primary(void);
static struct node *parse_grouping(void);
static struct node *parse_regex(void);
static struct node *parse_name(void);
static struct node *parse_array_name(void);
static struct node *parse_call(void);
static struct node *parse_builtin(void);
static struct node *parse_getline(void);
static struct node *parse_getline_lvalue(void);
static struct node *name_node(char *name);
static int is_lvalue(const struct node *node);
static void require_lvalue(const struct node *node);

/*
 * Parses a program into the rules and the functions of awk.
 */
void
parse_program(
	const char *source,
	size_t length)
{
	struct function *function;

	/* The first token. */
	lex_start(source, length);
	advance();

	/* Every item: a function or a rule, then one ; at most, and newlines. */
	awk.parsing = 1;
	skip_newlines();
	while (current.kind != TOKEN_EOF) {
		parse_item();
		if (current.kind == TOKEN_SEMICOLON)
			advance();
		skip_newlines();
	}

	/* Errors are no longer in the program text. */
	awk.parsing = 0;

	/* Every function called must be defined. */
	for (function = awk.functions; function != NULL; function = function->next) {
		if (!function->defined) {
			awk_fatal("calling undefined function %s at source line %d",
				  function->name,
				  function->first_use_line);
		}
	}
}

/* Moves to the next token, freeing what the last one still held. */
static void
advance(
	void)
{
	/* The text no node took. */
	free(current.text);
	current.text = NULL;

	/* The peeked token, or a new one. */
	if (have_lookahead) {
		current = lookahead;
		have_lookahead = 0;
		return;
	}

	/* A new token. */
	lex_next(&current);
}

/* Returns the kind of the token after the current one. */
static int
peek_kind(
	void)
{
	/* The token after, read once. */
	if (!have_lookahead) {
		lex_next(&lookahead);
		have_lookahead = 1;
	}

	/* Succeeded. */
	return lookahead.kind;
}

/* Moves past a token that must be there. */
static void
expect(
	int kind,
	const char *what)
{
	/* Anything else is an error. */
	if (current.kind != kind) {
		awk_fatal("syntax error at source line %d: expected %s",
			  current.line,
			  what);
	}

	/* Succeeded. */
	advance();
}

/* Reports a token that cannot be where it is. */
static void
syntax_error(
	void)
{
	/* The line, and the text of the token when it has one. */
	if (current.text != NULL) {
		awk_fatal("syntax error at source line %d near '%s'",
			  current.line,
			  current.text);
	}

	/* A token without text: the line alone. */
	awk_fatal("syntax error at source line %d", current.line);
}

/* Skips newlines, where the grammar lets a line go on. */
static void
skip_newlines(
	void)
{
	/* Every newline. */
	while (current.kind == TOKEN_NEWLINE)
		advance();
}

/* Skips newlines and semicolons between statements and items. */
static void
skip_terminators(
	void)
{
	/* Every newline and semicolon. */
	while (current.kind == TOKEN_NEWLINE || current.kind == TOKEN_SEMICOLON)
		advance();
}

/* Makes a node of a kind, on the current line. */
static struct node *
new_node(
	int kind)
{
	struct node *node;

	/* The node, zeroed. */
	node = awk_allocate(sizeof(*node));
	node->kind = kind;
	node->line = current.line;

	/* Succeeded. */
	return node;
}

/* Adds a rule at the end of the rules of its kind. */
static void
add_rule(
	int kind,
	struct node *pattern,
	struct node *pattern_end,
	struct node *action)
{
	struct rule *rule;
	struct rule **link;

	/* The rule. */
	rule = awk_allocate(sizeof(*rule));
	rule->kind = kind;
	rule->pattern = pattern;
	rule->pattern_end = pattern_end;
	rule->action = action;

	/* The list of its kind. */
	link = &awk.main_rules;
	if (kind == RULE_BEGIN)
		link = &awk.begin_rules;
	else if (kind == RULE_END)
		link = &awk.end_rules;

	/* Succeeded: at the end of the list. */
	while (*link != NULL)
		link = &(*link)->next;
	*link = rule;
}

/* Parses an item: a function, BEGIN, END, or a pattern and an action. */
static void
parse_item(
	void)
{
	struct node *pattern;
	struct node *pattern_end;
	struct node *action;
	int kind;

	/* A function. */
	if (current.kind == TOKEN_FUNCTION) {
		parse_function();
		return;
	}

	/* BEGIN and END, which take an action. */
	if (current.kind == TOKEN_BEGIN || current.kind == TOKEN_END) {
		kind = RULE_BEGIN;
		if (current.kind == TOKEN_END)
			kind = RULE_END;
		advance();
		if (current.kind != TOKEN_LEFT_BRACE)
			syntax_error();
		parsing_rule_kind = kind;
		action = parse_block();
		add_rule(kind, NULL, NULL, action);
		return;
	}

	/* A pattern, or two for a range, then the action if there is one. */
	parsing_rule_kind = RULE_MAIN;
	pattern = NULL;
	pattern_end = NULL;
	if (current.kind != TOKEN_LEFT_BRACE) {
		pattern = parse_expression();
		if (current.kind == TOKEN_COMMA) {
			advance();
			skip_newlines();
			pattern_end = parse_expression();
		}
	}

	/* The action, which makes the rule. */
	action = NULL;
	if (current.kind == TOKEN_LEFT_BRACE) {
		action = parse_block();
		add_rule(RULE_MAIN, pattern, pattern_end, action);
		return;
	}

	/* Succeeded: a pattern alone, which a newline or ; must end. */
	if (current.kind != TOKEN_NEWLINE &&
	    current.kind != TOKEN_SEMICOLON &&
	    current.kind != TOKEN_EOF)
		syntax_error();
	add_rule(RULE_MAIN, pattern, pattern_end, NULL);
}

/* Parses a function definition. */
static void
parse_function(
	void)
{
	struct function *function;
	int compare;
	size_t index;

	/* The name. */
	advance();
	if (current.kind != TOKEN_NAME && current.kind != TOKEN_FUNCTION_NAME)
		syntax_error();
	function = find_function(current.text);
	if (function->defined) {
		awk_fatal("syntax error at source line %d: function %s is defined twice",
			  current.line,
			  function->name);
	}

	/* Past the name. */
	advance();

	/* The parameters, names separated by commas. */
	expect(TOKEN_LEFT_PAREN, "(");
	skip_newlines();
	while (current.kind != TOKEN_RIGHT_PAREN) {
		if (current.kind != TOKEN_NAME)
			syntax_error();
		for (index = 0; index < function->parameter_count; index++) {
			compare = strcmp(function->parameters[index], current.text);
			if (compare == 0) {
				awk_fatal("syntax error at source line %d: parameter %s is repeated",
					  current.line,
					  current.text);
			}
		}

		/* The parameter, added. */
		function->parameters = awk_reallocate(
			function->parameters,
			sizeof(*function->parameters) * (function->parameter_count + 1U));
		function->parameters[function->parameter_count] = current.text;
		function->parameter_count++;
		current.text = NULL;
		advance();
		if (current.kind != TOKEN_COMMA)
			break;
		advance();
		skip_newlines();
	}

	/* The closing parenthesis, and newlines before the body. */
	expect(TOKEN_RIGHT_PAREN, ")");
	skip_newlines();

	/* Succeeded: the body, with the parameters in scope. */
	if (current.kind != TOKEN_LEFT_BRACE)
		syntax_error();
	parsing_function = function;
	function->body = parse_block();
	function->defined = 1;
	parsing_function = NULL;
}

/* Finds a function by name, making an undefined one the first time. */
static struct function *
find_function(
	const char *name)
{
	struct function *function;
	struct function **link;
	int compare;

	/* A function already named. */
	link = &awk.functions;
	for (function = awk.functions; function != NULL; function = function->next) {
		compare = strcmp(function->name, name);
		if (compare == 0)
			return function;
		link = &function->next;
	}

	/* Succeeded: a new function at the end of the list. */
	function = awk_allocate(sizeof(*function));
	function->name = awk_copy(name, strlen(name));
	function->first_use_line = current.line;
	*link = function;
	return function;
}

/* Parses { statements }. */
static struct node *
parse_block(
	void)
{
	struct node *block;
	struct node *statement;
	struct node **link;

	/* The brace. */
	block = new_node(NODE_BLOCK);
	expect(TOKEN_LEFT_BRACE, "{");

	/* The statements up to the closing brace. */
	link = &block->body;
	skip_terminators();
	while (current.kind != TOKEN_RIGHT_BRACE) {
		if (current.kind == TOKEN_EOF)
			syntax_error();
		statement = parse_statement();
		if (statement != NULL) {
			*link = statement;
			link = &statement->next;
		}

		/* The separators before the next statement. */
		skip_terminators();
	}

	/* Succeeded: past the brace. */
	advance();
	return block;
}

/* Parses a statement; returns NULL for an empty one. */
static struct node *
parse_statement(
	void)
{
	struct node *statement;

	/* The statement its first token starts. */
	switch (current.kind) {
	case TOKEN_LEFT_BRACE:
		statement = parse_block();
		break;
	case TOKEN_IF:
		statement = parse_if();
		break;
	case TOKEN_WHILE:
		statement = parse_while();
		break;
	case TOKEN_DO:
		statement = parse_do();
		break;
	case TOKEN_FOR:
		statement = parse_for();
		break;
	case TOKEN_SEMICOLON:
		advance();
		statement = NULL;
		break;
	default:
		statement = parse_simple_statement();
		parse_terminator();
		break;
	}

	/* Succeeded. */
	return statement;
}

/*
 * Parses the statement a control statement controls, after its ): a ;
 * alone is an empty statement.
 */
static struct node *
parse_body(
	void)
{
	struct node *body;

	/* An empty statement. */
	if (current.kind == TOKEN_SEMICOLON) {
		advance();
		return NULL;
	}

	/* Succeeded: a statement, which may start on the next line. */
	skip_newlines();
	body = parse_statement();
	return body;
}

/* Parses if (condition) statement [else statement]. */
static struct node *
parse_if(
	void)
{
	struct node *statement;

	/* The condition and the statement. */
	statement = new_node(NODE_IF);
	advance();
	expect(TOKEN_LEFT_PAREN, "(");
	statement->condition = parse_expression();
	expect(TOKEN_RIGHT_PAREN, ")");
	statement->then = parse_body();

	/* An else, perhaps after newlines and a semicolon. */
	skip_terminators();
	if (current.kind == TOKEN_ELSE) {
		advance();
		statement->otherwise = parse_body();
	}

	/* Succeeded. */
	return statement;
}

/* Parses while (condition) statement. */
static struct node *
parse_while(
	void)
{
	struct node *statement;

	/* The condition. */
	statement = new_node(NODE_WHILE);
	advance();
	expect(TOKEN_LEFT_PAREN, "(");
	statement->condition = parse_expression();
	expect(TOKEN_RIGHT_PAREN, ")");

	/* Succeeded: the body, inside the loop. */
	loop_depth++;
	statement->body = parse_body();
	loop_depth--;
	return statement;
}

/* Parses do statement while (condition). */
static struct node *
parse_do(
	void)
{
	struct node *statement;

	/* The body, inside the loop. */
	statement = new_node(NODE_DO);
	advance();
	loop_depth++;
	statement->body = parse_body();
	loop_depth--;

	/* The condition. */
	skip_terminators();
	expect(TOKEN_WHILE, "while");
	expect(TOKEN_LEFT_PAREN, "(");
	statement->condition = parse_expression();
	expect(TOKEN_RIGHT_PAREN, ")");

	/* Succeeded: the end of the statement. */
	parse_terminator();
	return statement;
}

/* Parses for (initial; condition; step) statement and for (name in array). */
static struct node *
parse_for(
	void)
{
	struct node *statement;
	struct node *initial;
	int single_name;

	/* The first part, which may be name in array. */
	statement = new_node(NODE_FOR);
	advance();
	expect(TOKEN_LEFT_PAREN, "(");
	initial = NULL;
	if (current.kind != TOKEN_SEMICOLON)
		initial = parse_expression();

	/* for (name in array). */
	single_name = 0;
	if (initial != NULL && initial->kind == NODE_IN) {
		if (initial->arguments->next == NULL &&
		    (initial->arguments->kind == NODE_VARIABLE ||
		     initial->arguments->kind == NODE_LOCAL))
			single_name = 1;
	}

	/* It is when ) follows. */
	if (single_name && current.kind == TOKEN_RIGHT_PAREN) {
		advance();
		statement->kind = NODE_FOR_IN;
		statement->left = initial->arguments;
		statement->array = initial->array;
		loop_depth++;
		statement->body = parse_body();
		loop_depth--;
		return statement;
	}

	/* The condition and the step. */
	statement->initial = initial;
	expect(TOKEN_SEMICOLON, ";");
	skip_newlines();
	if (current.kind != TOKEN_SEMICOLON)
		statement->condition = parse_expression();
	expect(TOKEN_SEMICOLON, ";");
	skip_newlines();
	if (current.kind != TOKEN_RIGHT_PAREN)
		statement->step = parse_expression();
	expect(TOKEN_RIGHT_PAREN, ")");

	/* Succeeded: the body, inside the loop. */
	loop_depth++;
	statement->body = parse_body();
	loop_depth--;
	return statement;
}

/* Parses a statement that a newline or a semicolon ends. */
static struct node *
parse_simple_statement(
	void)
{
	struct node *statement;
	int kind;
	int terminator;

	/* The statement its first token starts. */
	terminator = 0;
	switch (current.kind) {
	case TOKEN_PRINT:
	case TOKEN_PRINTF:
		statement = parse_print();
		break;
	case TOKEN_NEXT:
	case TOKEN_NEXTFILE:
		if (parsing_rule_kind != RULE_MAIN && parsing_function == NULL) {
			awk_fatal("syntax error at source line %d: next used in BEGIN or END",
				  current.line);
		}

		/* next or nextfile. */
		kind = NODE_NEXT;
		if (current.kind == TOKEN_NEXTFILE)
			kind = NODE_NEXTFILE;
		statement = new_node(kind);
		advance();
		break;
	case TOKEN_EXIT:
		statement = new_node(NODE_EXIT);
		advance();
		terminator = is_terminator(current.kind);
		if (!terminator)
			statement->left = parse_expression();
		break;
	case TOKEN_RETURN:
		if (parsing_function == NULL) {
			awk_fatal("syntax error at source line %d: return outside a function",
				  current.line);
		}

		/* The value, if one is given. */
		statement = new_node(NODE_RETURN);
		advance();
		terminator = is_terminator(current.kind);
		if (!terminator)
			statement->left = parse_expression();
		break;
	case TOKEN_BREAK:
	case TOKEN_CONTINUE:
		if (loop_depth == 0) {
			awk_fatal("syntax error at source line %d: break or continue outside a loop",
				  current.line);
		}

		/* break or continue. */
		kind = NODE_BREAK;
		if (current.kind == TOKEN_CONTINUE)
			kind = NODE_CONTINUE;
		statement = new_node(kind);
		advance();
		break;
	case TOKEN_DELETE:
		statement = parse_delete();
		break;
	default:
		statement = new_node(NODE_EXPRESSION);
		statement->left = parse_expression();
		break;
	}

	/* Succeeded. */
	return statement;
}

/* Parses print and printf, their arguments and where the output goes. */
static struct node *
parse_print(
	void)
{
	struct node *statement;
	struct node *arguments;
	size_t count;
	int kind;
	int terminator;

	/* The statement. */
	kind = NODE_PRINT;
	if (current.kind == TOKEN_PRINTF)
		kind = NODE_PRINTF;
	statement = new_node(kind);
	advance();

	/* The arguments, unless the statement ends or redirects at once. */
	terminator = is_terminator(current.kind);
	if (!terminator &&
	    current.kind != TOKEN_GREATER &&
	    current.kind != TOKEN_APPEND &&
	    current.kind != TOKEN_PIPE) {
		in_print_arguments = 1;
		arguments = parse_expression_list(TOKEN_EOF, &count);
		in_print_arguments = 0;

		/* print (a, b) is the list a, b. */
		if (count == 1 && arguments->kind == NODE_GROUPING) {
			count = arguments->argument_count;
			arguments = arguments->arguments;
		}

		/* The arguments of the statement. */
		statement->arguments = arguments;
		statement->argument_count = count;
	}

	/* printf needs a format. */
	if (kind == NODE_PRINTF && statement->argument_count == 0) {
		awk_fatal("syntax error at source line %d: printf without a format",
			  statement->line);
	}

	/* Where the output goes. */
	statement->output = OUTPUT_STANDARD;
	if (current.kind == TOKEN_GREATER)
		statement->output = OUTPUT_FILE;
	else if (current.kind == TOKEN_APPEND)
		statement->output = OUTPUT_APPEND;
	else if (current.kind == TOKEN_PIPE)
		statement->output = OUTPUT_PIPE;
	if (statement->output != OUTPUT_STANDARD) {
		advance();
		in_print_arguments = 1;
		statement->destination = parse_concatenation();
		in_print_arguments = 0;
	}

	/* Succeeded. */
	return statement;
}

/* Parses delete name[subscripts] and delete name. */
static struct node *
parse_delete(
	void)
{
	struct node *statement;
	size_t count;

	/* The array. */
	statement = new_node(NODE_DELETE);
	advance();
	if (current.kind != TOKEN_NAME)
		syntax_error();
	statement->array = parse_array_name();

	/* The subscripts, when an element is deleted. */
	if (current.kind == TOKEN_LEFT_BRACKET) {
		advance();
		statement->arguments = parse_expression_list(TOKEN_RIGHT_BRACKET, &count);
		statement->argument_count = count;
		expect(TOKEN_RIGHT_BRACKET, "]");
	}

	/* Succeeded. */
	return statement;
}

/* Moves past what ends a simple statement: ;, a newline, or before }. */
static void
parse_terminator(
	void)
{
	/* A semicolon or a newline is taken. */
	if (current.kind == TOKEN_SEMICOLON || current.kind == TOKEN_NEWLINE) {
		advance();
		return;
	}

	/* A closing brace and the end are left for the caller. */
	if (current.kind == TOKEN_RIGHT_BRACE || current.kind == TOKEN_EOF)
		return;

	/* Anything else cannot follow a statement. */
	syntax_error();
}

/* Returns whether a token ends a simple statement. */
static int
is_terminator(
	int kind)
{
	/* ;, a newline, } and the end. */
	if (kind == TOKEN_SEMICOLON || kind == TOKEN_NEWLINE)
		return 1;
	if (kind == TOKEN_RIGHT_BRACE || kind == TOKEN_EOF)
		return 1;
	return 0;
}

/* Parses an expression, with assignment at the loosest level. */
static struct node *
parse_expression(
	void)
{
	struct node *expression;
	struct node *assignment;

	/* The left side. */
	expression = parse_ternary();

	/* An assignment operator makes it an lvalue. */
	switch (current.kind) {
	case TOKEN_ASSIGN:
	case TOKEN_ADD_ASSIGN:
	case TOKEN_SUBTRACT_ASSIGN:
	case TOKEN_MULTIPLY_ASSIGN:
	case TOKEN_DIVIDE_ASSIGN:
	case TOKEN_MODULO_ASSIGN:
	case TOKEN_POWER_ASSIGN:
		break;
	default:
		return expression;
	}

	/* The left side must be an lvalue. */
	require_lvalue(expression);
	assignment = new_node(NODE_ASSIGN);
	assignment->operator = current.kind;
	assignment->left = expression;
	advance();
	skip_newlines();
	assignment->right = parse_expression();

	/* Succeeded. */
	return assignment;
}

/*
 * Parses expressions separated by commas, up to a closing token that is
 * left for the caller (TOKEN_EOF for none).  > is a comparison again
 * inside a list closed by ) or ].
 */
static struct node *
parse_expression_list(
	int closing,
	size_t *count)
{
	struct node *first;
	struct node *expression;
	struct node **link;
	int saved_print;

	/* Nothing before the closing token. */
	*count = 0;
	first = NULL;
	if (closing != TOKEN_EOF && current.kind == closing)
		return NULL;

	/* > inside brackets and parentheses compares. */
	saved_print = in_print_arguments;
	if (closing != TOKEN_EOF)
		in_print_arguments = 0;

	/* Each expression and the comma after it. */
	link = &first;
	skip_newlines();
	for (;;) {
		expression = parse_expression();
		*link = expression;
		link = &expression->next;
		(*count)++;
		if (closing != TOKEN_EOF)
			skip_newlines();
		if (current.kind != TOKEN_COMMA)
			break;
		advance();
		skip_newlines();
	}

	/* Succeeded. */
	in_print_arguments = saved_print;
	return first;
}

/* Parses condition ? then : otherwise. */
static struct node *
parse_ternary(
	void)
{
	struct node *condition;
	struct node *conditional;

	/* The condition. */
	condition = parse_or();
	if (current.kind != TOKEN_QUESTION)
		return condition;

	/* Succeeded: the two arms. */
	conditional = new_node(NODE_CONDITIONAL);
	conditional->condition = condition;
	advance();
	skip_newlines();
	conditional->then = parse_ternary();
	skip_newlines();
	expect(TOKEN_COLON, ":");
	skip_newlines();
	conditional->otherwise = parse_ternary();
	return conditional;
}

/* Parses ||. */
static struct node *
parse_or(
	void)
{
	struct node *left;
	struct node *node;

	/* Operands joined left to right. */
	left = parse_and();
	while (current.kind == TOKEN_OR) {
		node = new_node(NODE_OR);
		advance();
		skip_newlines();
		node->left = left;
		node->right = parse_and();
		left = node;
	}

	/* Succeeded. */
	return left;
}

/* Parses &&. */
static struct node *
parse_and(
	void)
{
	struct node *left;
	struct node *node;

	/* Operands joined left to right. */
	left = parse_in();
	while (current.kind == TOKEN_AND) {
		node = new_node(NODE_AND);
		advance();
		skip_newlines();
		node->left = left;
		node->right = parse_in();
		left = node;
	}

	/* Succeeded. */
	return left;
}

/* Parses subscript in array. */
static struct node *
parse_in(
	void)
{
	struct node *left;
	struct node *node;

	/* Membership tests joined left to right. */
	left = parse_match();
	while (current.kind == TOKEN_IN) {
		node = new_node(NODE_IN);
		advance();
		if (current.kind != TOKEN_NAME)
			syntax_error();
		node->array = parse_array_name();
		node->arguments = left;
		node->argument_count = 1;
		if (left->kind == NODE_GROUPING) {
			node->arguments = left->arguments;
			node->argument_count = left->argument_count;
		}

		/* The test is the left operand of the next one. */
		left = node;
	}

	/* Succeeded. */
	return left;
}

/* Parses ~ and !~. */
static struct node *
parse_match(
	void)
{
	struct node *left;
	struct node *node;

	/* Matches joined left to right. */
	left = parse_relational();
	while (current.kind == TOKEN_TILDE || current.kind == TOKEN_NOT_TILDE) {
		node = new_node(NODE_MATCH);
		node->operator = current.kind;
		advance();
		node->left = left;
		node->right = parse_relational();
		left = node;
	}

	/* Succeeded. */
	return left;
}

/* Parses the relational operators, which do not chain. */
static struct node *
parse_relational(
	void)
{
	struct node *left;
	struct node *node;

	/* The left operand. */
	left = parse_pipe_getline();

	/* An operator; > in print's arguments is where the output goes. */
	switch (current.kind) {
	case TOKEN_LESS:
	case TOKEN_LESS_EQUAL:
	case TOKEN_EQUAL:
	case TOKEN_NOT_EQUAL:
	case TOKEN_GREATER_EQUAL:
		break;
	case TOKEN_GREATER:
		if (in_print_arguments)
			return left;
		break;
	default:
		return left;
	}

	/* Succeeded: the comparison. */
	node = new_node(NODE_COMPARE);
	node->operator = current.kind;
	advance();
	node->left = left;
	node->right = parse_pipe_getline();
	return node;
}

/* Parses command | getline [lvalue]. */
static struct node *
parse_pipe_getline(
	void)
{
	struct node *left;
	struct node *node;
	int following;

	/* A command, piped into getline as many times as written. */
	left = parse_concatenation();
	for (;;) {
		if (current.kind != TOKEN_PIPE)
			break;
		following = peek_kind();
		if (following != TOKEN_GETLINE)
			break;
		node = new_node(NODE_GETLINE);
		node->source = GETLINE_COMMAND;
		advance();
		advance();
		node->right = left;
		node->left = parse_getline_lvalue();
		left = node;
	}

	/* Succeeded. */
	return left;
}

/* Parses expressions written side by side. */
static struct node *
parse_concatenation(
	void)
{
	struct node *left;
	struct node *node;
	int starts;

	/* Operands joined left to right while one follows. */
	left = parse_additive();
	for (;;) {
		starts = starts_concatenation(current.kind);
		if (!starts)
			break;
		node = new_node(NODE_CONCATENATE);
		node->left = left;
		node->right = parse_additive();
		left = node;
	}

	/* Succeeded. */
	return left;
}

/*
 * Returns whether a token starts the next operand of a concatenation.
 * + - and ! do not, since they are taken as operators.
 */
static int
starts_concatenation(
	int kind)
{
	/* Constants, names, calls, fields, grouping and increments. */
	switch (kind) {
	case TOKEN_NUMBER:
	case TOKEN_STRING:
	case TOKEN_NAME:
	case TOKEN_FUNCTION_NAME:
	case TOKEN_BUILTIN:
	case TOKEN_DOLLAR:
	case TOKEN_LEFT_PAREN:
	case TOKEN_INCREMENT:
	case TOKEN_DECREMENT:
		return 1;
	default:
		return 0;
	}
}

/* Parses + and -. */
static struct node *
parse_additive(
	void)
{
	struct node *left;
	struct node *node;

	/* Operands joined left to right. */
	left = parse_multiplicative();
	while (current.kind == TOKEN_PLUS || current.kind == TOKEN_MINUS) {
		node = new_node(NODE_ARITHMETIC);
		node->operator = current.kind;
		advance();
		node->left = left;
		node->right = parse_multiplicative();
		left = node;
	}

	/* Succeeded. */
	return left;
}

/* Parses *, / and %. */
static struct node *
parse_multiplicative(
	void)
{
	struct node *left;
	struct node *node;

	/* Operands joined left to right. */
	left = parse_unary();
	while (current.kind == TOKEN_STAR ||
	       current.kind == TOKEN_SLASH ||
	       current.kind == TOKEN_PERCENT) {
		node = new_node(NODE_ARITHMETIC);
		node->operator = current.kind;
		advance();
		node->left = left;
		node->right = parse_unary();
		left = node;
	}

	/* Succeeded. */
	return left;
}

/* Parses the unary !, - and +. */
static struct node *
parse_unary(
	void)
{
	struct node *node;
	struct node *power;
	int kind;

	/* The operator, if there is one. */
	if (current.kind == TOKEN_NOT)
		kind = NODE_NOT;
	else if (current.kind == TOKEN_MINUS)
		kind = NODE_NEGATE;
	else if (current.kind == TOKEN_PLUS)
		kind = NODE_UNARY_PLUS;
	else
		kind = -1;

	/* No operator: a power. */
	if (kind < 0) {
		power = parse_power();
		return power;
	}

	/* Succeeded: the operator on what follows. */
	node = new_node(kind);
	advance();
	node->left = parse_unary();
	return node;
}

/* Parses ^, which goes right to left and binds its right side loosely. */
static struct node *
parse_power(
	void)
{
	struct node *left;
	struct node *node;

	/* The base. */
	left = parse_postfix();
	if (current.kind != TOKEN_CARET)
		return left;

	/* Succeeded: the exponent, which may be signed. */
	node = new_node(NODE_ARITHMETIC);
	node->operator = TOKEN_CARET;
	advance();
	node->left = left;
	node->right = parse_unary();
	return node;
}

/* Parses lvalue++ and lvalue--. */
static struct node *
parse_postfix(
	void)
{
	struct node *operand;
	struct node *node;
	int lvalue;

	/* The operand. */
	operand = parse_primary();
	if (current.kind != TOKEN_INCREMENT && current.kind != TOKEN_DECREMENT)
		return operand;
	lvalue = is_lvalue(operand);
	if (!lvalue)
		return operand;

	/* Succeeded: the increment after it. */
	node = new_node(NODE_POST_INCREMENT);
	node->operator = current.kind;
	node->left = operand;
	advance();
	return node;
}

/* Parses a primary expression. */
static struct node *
parse_primary(
	void)
{
	struct node *node;

	/* The expression its first token starts. */
	switch (current.kind) {
	case TOKEN_NUMBER:
		node = new_node(NODE_NUMBER);
		node->number = current.number;
		advance();
		break;
	case TOKEN_STRING:
		node = new_node(NODE_STRING);
		node->text = current.text;
		node->length = current.length;
		current.text = NULL;
		advance();
		break;
	case TOKEN_SLASH:
	case TOKEN_DIVIDE_ASSIGN:
		node = parse_regex();
		break;
	case TOKEN_LEFT_PAREN:
		node = parse_grouping();
		break;
	case TOKEN_NOT:
	case TOKEN_MINUS:
	case TOKEN_PLUS:
		node = parse_unary();
		break;
	case TOKEN_DOLLAR:
		node = new_node(NODE_FIELD);
		advance();
		if (current.kind == TOKEN_INCREMENT || current.kind == TOKEN_DECREMENT)
			node->left = parse_primary();
		else if (current.kind == TOKEN_MINUS)
			node->left = parse_unary();
		else
			node->left = parse_primary();
		break;
	case TOKEN_INCREMENT:
	case TOKEN_DECREMENT:
		node = new_node(NODE_PRE_INCREMENT);
		node->operator = current.kind;
		advance();
		node->left = parse_primary();
		require_lvalue(node->left);
		break;
	case TOKEN_NAME:
		node = parse_name();
		break;
	case TOKEN_FUNCTION_NAME:
		node = parse_call();
		break;
	case TOKEN_BUILTIN:
		node = parse_builtin();
		break;
	case TOKEN_GETLINE:
		node = parse_getline();
		break;
	default:
		syntax_error();
		node = NULL;
		break;
	}

	/* Succeeded. */
	return node;
}

/*
 * Parses ( expression ) and ( expression, expression... ), which is a
 * grouping for print and a list of subscripts for in.
 */
static struct node *
parse_grouping(
	void)
{
	struct node *grouping;
	struct node *list;
	size_t count;

	/* The expressions inside the parentheses. */
	grouping = new_node(NODE_GROUPING);
	advance();
	list = parse_expression_list(TOKEN_RIGHT_PAREN, &count);
	expect(TOKEN_RIGHT_PAREN, ")");
	if (count == 0)
		syntax_error();

	/* One expression is itself. */
	if (count == 1)
		return list;

	/* Succeeded: a list, for in or for print. */
	grouping->arguments = list;
	grouping->argument_count = count;
	return grouping;
}

/* Parses /ERE/ from the / the current token is. */
static struct node *
parse_regex(
	void)
{
	struct node *node;

	/* The text after the /, read again as a regex. */
	have_lookahead = 0;
	free(lookahead.text);
	lookahead.text = NULL;
	lex_regex(&current, current.start + 1U);

	/* Succeeded: the regex, compiled once for the program. */
	node = new_node(NODE_REGEX);
	node->text = current.text;
	node->length = current.length;
	current.text = NULL;
	node->regex = regex_compile(node->text, node->length, 0);
	advance();
	return node;
}

/* Parses a name: a variable, a parameter, or an element of an array. */
static struct node *
parse_name(
	void)
{
	struct node *array;
	struct node *element;
	size_t count;

	/* The variable. */
	array = name_node(current.text);
	current.text = NULL;
	advance();
	if (current.kind != TOKEN_LEFT_BRACKET)
		return array;

	/* Succeeded: an element and its subscripts. */
	element = new_node(NODE_ELEMENT);
	element->array = array;
	advance();
	element->arguments = parse_expression_list(TOKEN_RIGHT_BRACKET, &count);
	element->argument_count = count;
	if (count == 0)
		syntax_error();
	expect(TOKEN_RIGHT_BRACKET, "]");
	return element;
}

/* Parses the name of an array. */
static struct node *
parse_array_name(
	void)
{
	struct node *array;

	/* The variable or the parameter. */
	array = name_node(current.text);
	current.text = NULL;
	advance();

	/* Succeeded. */
	return array;
}

/* Parses a call of a function of the program. */
static struct node *
parse_call(
	void)
{
	struct node *call;
	size_t count;

	/* The function, defined now or later. */
	call = new_node(NODE_CALL);
	call->function = find_function(current.text);
	advance();

	/* Succeeded: the arguments. */
	expect(TOKEN_LEFT_PAREN, "(");
	call->arguments = parse_expression_list(TOKEN_RIGHT_PAREN, &count);
	call->argument_count = count;
	expect(TOKEN_RIGHT_PAREN, ")");
	return call;
}

/* Parses a call of a built-in function; length may go without (). */
static struct node *
parse_builtin(
	void)
{
	struct node *call;
	const struct builtin_arity *arity;
	size_t count;

	/* The function. */
	call = new_node(NODE_BUILTIN);
	call->builtin = current.builtin;
	advance();

	/* The arguments, if written. */
	count = 0;
	if (current.kind == TOKEN_LEFT_PAREN) {
		advance();
		call->arguments = parse_expression_list(TOKEN_RIGHT_PAREN, &count);
		expect(TOKEN_RIGHT_PAREN, ")");
	} else if (call->builtin != BUILTIN_LENGTH) {
		syntax_error();
	}

	/* How many were given. */
	call->argument_count = count;

	/* As many as the function takes. */
	arity = &builtin_arities[call->builtin];
	if (count < arity->fewest || count > arity->most) {
		awk_fatal("syntax error at source line %d: wrong number of arguments",
			  call->line);
	}

	/* split takes an array, and sub and gsub an lvalue to change. */
	if (call->builtin == BUILTIN_SPLIT) {
		if (call->arguments->next->kind != NODE_VARIABLE &&
		    call->arguments->next->kind != NODE_LOCAL)
			syntax_error();
	}

	/* Succeeded. */
	return call;
}

/* Parses getline [lvalue] [< file]. */
static struct node *
parse_getline(
	void)
{
	struct node *node;

	/* The lvalue to read into, if any. */
	node = new_node(NODE_GETLINE);
	node->source = GETLINE_MAIN;
	advance();
	node->left = parse_getline_lvalue();

	/* The file, which is a primary expression. */
	if (current.kind == TOKEN_LESS) {
		advance();
		node->source = GETLINE_FILE;
		node->right = parse_primary();
	}

	/* Succeeded. */
	return node;
}

/* Parses the lvalue after getline, if there is one. */
static struct node *
parse_getline_lvalue(
	void)
{
	struct node *field;

	/* A variable or an element. */
	if (current.kind == TOKEN_NAME) {
		field = parse_name();
		return field;
	}

	/* A field. */
	if (current.kind == TOKEN_DOLLAR) {
		field = new_node(NODE_FIELD);
		advance();
		field->left = parse_primary();
		return field;
	}

	/* None. */
	return NULL;
}

/*
 * Makes the node of a name: a parameter of the function being parsed, or
 * a global variable.  The node takes the name.
 */
static struct node *
name_node(
	char *name)
{
	struct node *node;
	size_t index;
	int compare;

	/* A parameter, by its index in the call. */
	if (parsing_function != NULL) {
		for (index = 0; index < parsing_function->parameter_count; index++) {
			compare = strcmp(parsing_function->parameters[index], name);
			if (compare != 0)
				continue;
			node = new_node(NODE_LOCAL);
			node->local = index;
			node->text = name;
			return node;
		}
	}

	/* Succeeded: a global variable. */
	node = new_node(NODE_VARIABLE);
	node->variable = variable_find(name, 1);
	node->text = name;
	return node;
}

/* Returns whether a node can be assigned to. */
static int
is_lvalue(
	const struct node *node)
{
	/* Variables, parameters, elements and fields. */
	switch (node->kind) {
	case NODE_VARIABLE:
	case NODE_LOCAL:
	case NODE_ELEMENT:
	case NODE_FIELD:
		return 1;
	default:
		return 0;
	}
}

/* Refuses a node that cannot be assigned to where one must be. */
static void
require_lvalue(
	const struct node *node)
{
	int lvalue;

	/* Anything but an lvalue is an error. */
	lvalue = is_lvalue(node);
	if (!lvalue) {
		awk_fatal("syntax error at source line %d: assignment to a non-lvalue",
			  node->line);
	}
}
