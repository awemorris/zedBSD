/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shell's parser (POSIX XCU 2.3, 2.9 and 2.10).
 *
 * The parser reads characters from the input a token at a time and builds
 * the tree of one complete command, which is run before the next is read.
 * Reserved words and aliases are recognized where the grammar allows them,
 * which the caller of next_token says with its flags.  A word keeps the text
 * it was written with; a command substitution in it is parsed here, by a
 * parser of its own over the same input, to find where it ends and to report
 * its syntax errors where they are written, and its text is kept in the word
 * to be parsed again when the substitution runs.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/alias.h"
#include "userland/base/sh/vars.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The tokens. */
#define T_EOF		0
#define T_NEWLINE	1
#define T_SEMI		2
#define T_DSEMI		3
#define T_AMP		4
#define T_AND		5
#define T_OR		6
#define T_PIPE		7
#define T_PIPEAMP	8	/* |&, a bash extension (a syntax error in POSIX) */
#define T_LPAREN	9
#define T_RPAREN	10
#define T_LESS		11
#define T_GREAT		12
#define T_DGREAT	13
#define T_CLOBBER	14
#define T_LESSGREAT	15
#define T_LESSAND	16
#define T_GREATAND	17
#define T_DLESS		18
#define T_DLESSDASH	19
#define T_TLESS		20	/* <<<, a bash extension (a syntax error in POSIX) */
#define T_WORD		21

/* The first and last tokens that start a redirection. */
#define T_REDIRECT_FIRST	T_LESS
#define T_REDIRECT_LAST		T_TLESS

/* The reserved words, recognized from a word where the grammar allows. */
#define T_IF		22
#define T_THEN		23
#define T_ELSE		24
#define T_ELIF		25
#define T_FI		26
#define T_DO		27
#define T_DONE		28
#define T_CASE		29
#define T_ESAC		30
#define T_WHILE		31
#define T_UNTIL		32
#define T_FOR		33
#define T_IN		34
#define T_LBRACE	35
#define T_RBRACE	36
#define T_BANG		37

/*
 * [[ and function, which XCU 2.4 lets a shell reserve with results it
 * leaves unspecified; bash gives them their meaning here.
 */
#define T_DLBRACKET	38
#define T_FUNCTION	39

/* What next_token may do with the word it reads (its flags). */
#define CHECK_NEWLINE	0x01	/* skip newlines first */
#define CHECK_KEYWORD	0x02	/* recognize a reserved word */
#define CHECK_ALIAS	0x04	/* expand an alias */

/* How parse_list ends. */
#define LIST_TOP	0	/* at a newline, which is left for the caller */
#define LIST_COMPOUND	1	/* at a word that closes a construct */

/*
 * A here-document whose body has not been read yet: it is read at the next
 * newline token, into the word of its redirection.
 */
struct heredoc {
	struct heredoc *next;
	struct sh_redirection *redirection;
	char *delimiter;
	int quoted;
	int strip_tabs;
};

/* A growing buffer of the characters of one word. */
struct word_buffer {
	char *text;
	size_t length;
	size_t capacity;
};

/*
 * The state of one parse.  A command substitution is parsed with a parser of
 * its own, sharing the input and the arena.
 */
struct parser {
	struct sh_arena *arena;

	/* The last token read, which push_token gives back to be read again. */
	int pushed;
	int token;
	struct sh_token *word;
	int io_number;
	int dash;
	int line;

	/* Set once the word of the last token has been checked for an alias. */
	int alias_checked;

	/* The here-documents waiting for the next newline, in order. */
	struct heredoc *heredocs;

	/* Set inside $( ... ), where aliases are not expanded. */
	int substitution;
};

/* A reserved word and its token. */
struct keyword {
	const char *text;
	int token;
};

/* The reserved words (XCU 2.4). */
static const struct keyword keywords[] = {
	{ "!", T_BANG },
	{ "[[", T_DLBRACKET },
	{ "case", T_CASE },
	{ "do", T_DO },
	{ "done", T_DONE },
	{ "elif", T_ELIF },
	{ "else", T_ELSE },
	{ "esac", T_ESAC },
	{ "fi", T_FI },
	{ "for", T_FOR },
	{ "function", T_FUNCTION },
	{ "if", T_IF },
	{ "in", T_IN },
	{ "then", T_THEN },
	{ "until", T_UNTIL },
	{ "while", T_WHILE },
	{ "{", T_LBRACE },
	{ "}", T_RBRACE },
	{ NULL, 0 }
};

/* How each token is named in a syntax error. */
static const char *const token_names[] = {
	"end of file", "newline", ";", ";;", "&", "&&", "||", "|", "|&", "(",
	")", "<", ">", ">>", ">|", "<>", "<&", ">&", "<<", "<<-", "<<<", "word",
	"if", "then", "else", "elif", "fi", "do", "done", "case", "esac",
	"while", "until", "for", "in", "{", "}", "!", "[[", "function"
};

static struct sh_node *parse_list(struct parser *parser, int mode, int allow_empty);
static struct sh_node *parse_and_or(struct parser *parser);
static struct sh_node *parse_pipeline(struct parser *parser);
static struct sh_node *parse_command(struct parser *parser);
static struct sh_node *parse_simple(struct parser *parser);
static struct sh_node *parse_function(struct parser *parser, struct sh_token *name);
static struct sh_node *parse_if(struct parser *parser);
static struct sh_node *parse_loop(struct parser *parser, enum sh_node_kind kind);
static struct sh_node *parse_for(struct parser *parser);
static struct sh_node *parse_case(struct parser *parser);
static struct sh_node *parse_group(struct parser *parser, enum sh_node_kind kind, int closing);
static void parse_trailing_redirections(struct parser *parser, struct sh_node *node);
static struct sh_redirection *parse_redirection(struct parser *parser, int token);
static void expect(struct parser *parser, int token);
static void add_stderr_to_pipe(struct parser *parser, struct sh_node *command);
static struct sh_node *parse_function_keyword(struct parser *parser);
static struct sh_cond *parse_cond_or(struct parser *parser, int first);
static struct sh_cond *parse_cond_and(struct parser *parser);
static struct sh_cond *parse_cond_not(struct parser *parser);
static struct sh_cond *parse_cond_primary(struct parser *parser);
static int cond_closing(struct parser *parser, int token);
static const char *cond_binary_operator(struct parser *parser, int token);
static struct sh_token *lex_regex_word(struct parser *parser);
static struct sh_node *parse_arith_command(struct parser *parser);
static struct sh_node *parse_arith_for(struct parser *parser);
static char *read_arith_text(int *complete);
static void syntax_error(struct parser *parser, int token) __attribute__((noreturn));
static struct sh_node *node_new(struct parser *parser, enum sh_node_kind kind);
static struct sh_node *node_binary(struct parser *parser, enum sh_node_kind kind, struct sh_node *left, struct sh_node *right);
static void array_add(struct parser *parser, void ***array, size_t *count, void *item);
static int next_token(struct parser *parser, int flags);
static void push_token(struct parser *parser);
static int keyword_of(const struct sh_token *word);
static int is_plain_word(const struct sh_token *word, const char *text);
static int lex(struct parser *parser);
static int lex_operator(struct parser *parser, int value);
static int lex_word(struct parser *parser, int value);
static void lex_single(struct word_buffer *buffer);
static int lex_process_substitution(struct parser *parser, int direction);
static void lex_dollar_single(struct word_buffer *buffer);
static int dollar_single_escape(unsigned long *code);
static void buffer_add_quoted(struct word_buffer *buffer, int value);
static void buffer_add_utf8(struct word_buffer *buffer, unsigned long code);
static void lex_double(struct parser *parser, struct word_buffer *buffer);
static void lex_dollar(struct parser *parser, struct word_buffer *buffer, int in_double);
static void lex_brace(struct parser *parser, struct word_buffer *buffer, int in_double);
static void lex_arithmetic(struct parser *parser, struct word_buffer *buffer);
static void lex_substitution(struct parser *parser, struct word_buffer *buffer);
static void lex_backquote(struct parser *parser, struct word_buffer *buffer);
static void check_syntax(struct parser *parser, const char *text, size_t length);
static int getc_continued(void);
static int is_operator_start(int value);
static void unterminated(const char *what) __attribute__((noreturn));
static void buffer_add(struct word_buffer *buffer, int value);
static struct sh_token *make_word(struct parser *parser, const char *raw, size_t length);
static const char *skip_rough(const char *text, const char *end);
static void read_heredocs(struct parser *parser);
static void read_heredoc(struct parser *parser, struct heredoc *heredoc);
static int is_assignment(const struct sh_token *word);
static const char *skip_single(const char *text);
static const char *skip_double(const char *text);
static const char *skip_backquote(const char *text);
static const char *skip_brace(const char *text, int in_double);
static const char *skip_arithmetic(const char *text);
static const char *skip_command(const char *text);
static const char *skip_expansion(const char *text, int in_double);

/*
 * Parses one complete command from the input.
 *
 * Returns the tree of the command, allocated in the arena, or NULL for a line
 * with no command on it; *eof is set when the input has ended.  A syntax error
 * is thrown.
 */
struct sh_node *
sh_parse_command(
	struct sh_arena *arena,
	int *eof)
{
	struct parser parser;
	struct sh_node *node;
	int token;

	/* Starts a parser with nothing read. */
	memset(&parser, 0, sizeof(parser));
	parser.arena = arena;
	*eof = 0;

	/* An empty line holds no command, and the end holds none either. */
	token = next_token(&parser, CHECK_KEYWORD | CHECK_ALIAS);
	if (token == T_EOF) {
		*eof = 1;
		return NULL;
	}

	/* An empty line is no command. */
	if (token == T_NEWLINE)
		return NULL;
	push_token(&parser);

	/* Reads the list up to the end of its line. */
	node = parse_list(&parser, LIST_TOP, 0);

	/* The list ends at the end of a line or of the input, and nowhere else. */
	token = next_token(&parser, 0);
	if (token != T_NEWLINE && token != T_EOF)
		syntax_error(&parser, token);

	/* Here-documents still owed at the end of the input read what is left. */
	if (parser.heredocs != NULL)
		read_heredocs(&parser);

	/* Succeeded: the command. */
	return node;
}

/*
 * Reports whether a word is an assignment: an unquoted name, then =.
 */
int
sh_token_is_assignment(
	const struct sh_token *word)
{
	int assignment;

	/* The parser's own test. */
	assignment = is_assignment(word);

	/* Succeeded: whether it is one. */
	return assignment;
}

/*
 * Finds where a single quotation ends.
 */
const char *
sh_skip_single(
	const char *text)
{
	const char *end;

	/* Scans for the closing quote. */
	end = skip_single(text);

	/* Succeeded: the character after it, or NULL. */
	return end;
}

/*
 * Finds where a double quotation ends.
 */
const char *
sh_skip_double(
	const char *text)
{
	const char *end;

	/* Scans for the closing quote past any expansion. */
	end = skip_double(text);

	/* Succeeded: the character after it, or NULL. */
	return end;
}

/*
 * Finds where an expansion ends.
 */
const char *
sh_skip_expansion(
	const char *text,
	int in_double)
{
	const char *end;

	/* Scans the expansion by its kind. */
	end = skip_expansion(text, in_double);

	/* Succeeded: the character after it, or NULL. */
	return end;
}

/*
 * Parses a list: and-or lists joined by ;, & and newlines.
 *
 * At the top the list ends at a newline or the end of the input, which are
 * left for the caller.  In a compound command it ends at a word that closes
 * the construct (then, fi, done, esac, }, ) and the like), and it may not be
 * empty unless the construct allows that (a case arm).
 */
static struct sh_node *
parse_list(
	struct parser *parser,
	int mode,
	int allow_empty)
{
	struct sh_node *list;
	struct sh_node *node;
	int token;

	/* Reads and-or lists until something ends the list. */
	list = NULL;
	for (;;) {
		/* In a compound command, newlines separate. */
		if (mode == LIST_COMPOUND) {
			token = next_token(parser, CHECK_NEWLINE | CHECK_KEYWORD |
					   CHECK_ALIAS);
		} else {
			token = next_token(parser, CHECK_KEYWORD | CHECK_ALIAS);
		}

		/* The token is read again by the command. */
		push_token(parser);

		/* A word that closes a construct, or the end, ends the list. */
		if (token == T_EOF || token == T_NEWLINE || token == T_THEN ||
		    token == T_ELSE || token == T_ELIF || token == T_FI ||
		    token == T_DO || token == T_DONE || token == T_ESAC ||
		    token == T_RBRACE || token == T_RPAREN ||
		    token == T_DSEMI)
			break;

		/* One and-or list, run in the background by a following &. */
		node = parse_and_or(parser);
		token = next_token(parser, 0);
		if (token == T_AMP)
			node = node_binary(parser, SH_NODE_BACKGROUND, node, NULL);

		/* Adds it to the sequence. */
		if (list == NULL)
			list = node;
		else
			list = node_binary(parser, SH_NODE_SEQUENCE, list, node);

		/* A separator goes on; anything else ends the list. */
		if (token == T_SEMI || token == T_AMP) {
			/* At the top, a separator at the end of the line ends it. */
			if (mode == LIST_TOP) {
				token = next_token(parser, 0);
				push_token(parser);
				if (token == T_NEWLINE || token == T_EOF)
					break;
			}

			continue;
		}

		/* Inside a compound command, a newline only separates. */
		if (token == T_NEWLINE && mode == LIST_COMPOUND)
			continue;
		push_token(parser);
		break;
	}

	/* A construct's list must hold a command (unless it may be empty). */
	if (list == NULL && !allow_empty) {
		token = next_token(parser, 0);
		syntax_error(parser, token);
	}

	/* Succeeded: the list. */
	return list;
}

/* Parses pipelines joined by && and ||. */
static struct sh_node *
parse_and_or(
	struct parser *parser)
{
	struct sh_node *node;
	struct sh_node *right;
	int token;

	/* Joins each following pipeline, left to right. */
	node = parse_pipeline(parser);
	for (;;) {
		token = next_token(parser, 0);
		if (token != T_AND && token != T_OR) {
			push_token(parser);
			break;
		}

		/* The pipeline on the right. */
		right = parse_pipeline(parser);
		if (token == T_AND)
			node = node_binary(parser, SH_NODE_AND, node, right);
		else
			node = node_binary(parser, SH_NODE_OR, node, right);
	}

	/* Succeeded: the and-or list. */
	return node;
}

/* Parses a pipeline, with a leading ! that turns its status round. */
static struct sh_node *
parse_pipeline(
	struct parser *parser)
{
	struct sh_node *pipeline;
	struct sh_node *command;
	int negated;
	int token;

	/* A leading ! negates; newlines may come first after && and ||. */
	negated = 0;
	token = next_token(parser, CHECK_NEWLINE | CHECK_KEYWORD | CHECK_ALIAS);
	if (token == T_BANG)
		negated = 1;
	else
		push_token(parser);

	/* Reads the commands joined by |. */
	command = parse_command(parser);
	pipeline = NULL;
	for (;;) {
		token = next_token(parser, 0);
		if (token != T_PIPE && token != T_PIPEAMP) {
			push_token(parser);
			break;
		}

		/* |& also sends the standard error of the command before it (2>&1). */
		if (token == T_PIPEAMP)
			add_stderr_to_pipe(parser, command);

		/* The first | makes the pipeline of the command before it. */
		if (pipeline == NULL) {
			pipeline = node_new(parser, SH_NODE_PIPELINE);
			array_add(parser,
				  (void ***)&pipeline->u.pipeline.commands,
				  &pipeline->u.pipeline.count, command);
		}

		/* A newline may follow the |. */
		(void)next_token(parser, CHECK_NEWLINE | CHECK_KEYWORD |
				 CHECK_ALIAS);
		push_token(parser);
		command = parse_command(parser);
		array_add(parser, (void ***)&pipeline->u.pipeline.commands,
			  &pipeline->u.pipeline.count, command);
	}

	/* A single command is no pipeline. */
	if (pipeline == NULL)
		pipeline = command;

	/* Negation wraps the whole pipeline. */
	if (negated)
		pipeline = node_binary(parser, SH_NODE_NOT, pipeline, NULL);

	/* Succeeded: the pipeline. */
	return pipeline;
}

/* Parses one command: compound, a function definition, or simple. */
static struct sh_node *
parse_command(
	struct parser *parser)
{
	struct sh_node *node;
	int token;

	/* The first word decides the kind of command. */
	token = next_token(parser, CHECK_KEYWORD | CHECK_ALIAS);

	/* Dispatches the compound commands; anything else is simple. */
	switch (token) {
	case T_IF:
		node = parse_if(parser);
		break;
	case T_WHILE:
		node = parse_loop(parser, SH_NODE_WHILE);
		break;
	case T_UNTIL:
		node = parse_loop(parser, SH_NODE_UNTIL);
		break;
	case T_FOR:
		node = parse_for(parser);
		break;
	case T_CASE:
		node = parse_case(parser);
		break;
	case T_LBRACE:
		node = parse_group(parser, SH_NODE_GROUP, T_RBRACE);
		break;
	case T_LPAREN:
		node = parse_arith_command(parser);
		if (node == NULL)
			node = parse_group(parser, SH_NODE_SUBSHELL, T_RPAREN);
		break;
	case T_DLBRACKET:
		node = node_new(parser, SH_NODE_COND);
		node->u.cond = parse_cond_or(parser, 1);
		token = next_token(parser, 0);
		if (!cond_closing(parser, token))
			syntax_error(parser, token);
		break;
	case T_FUNCTION:
		node = parse_function_keyword(parser);
		return node;
	case T_WORD:
	case T_LESS:
	case T_GREAT:
	case T_DGREAT:
	case T_CLOBBER:
	case T_LESSGREAT:
	case T_LESSAND:
	case T_GREATAND:
	case T_DLESS:
	case T_DLESSDASH:
	case T_TLESS:
		push_token(parser);
		node = parse_simple(parser);
		return node;
	default:
		syntax_error(parser, token);
	}

	/* Redirections after a compound command apply to all of it. */
	parse_trailing_redirections(parser, node);

	/* Succeeded: the compound command. */
	return node;
}

/*
 * Parses a simple command: assignments, words and redirections in any order,
 * or a function definition when its first word is followed by ().
 */
static struct sh_node *
parse_simple(
	struct parser *parser)
{
	struct sh_redirection **tail;
	struct sh_redirection *redirection;
	struct sh_token *name;
	struct sh_node *node;
	int assignment;
	int flags;
	int token;

	/* Reads the parts of the command. */
	node = node_new(parser, SH_NODE_SIMPLE);
	tail = &node->redirections;
	flags = CHECK_ALIAS;
	for (;;) {
		token = next_token(parser, flags);

		/* A redirection may stand anywhere among the words. */
		if (token >= T_REDIRECT_FIRST && token <= T_REDIRECT_LAST) {
			redirection = parse_redirection(parser, token);
			*tail = redirection;
			tail = &redirection->next;
			continue;
		}

		/* Anything but a word ends the simple command. */
		if (token != T_WORD) {
			push_token(parser);
			break;
		}

		/* A word is an assignment until the command name is seen. */
		assignment = 0;
		if (node->u.simple.word_count == 0)
			assignment = is_assignment(parser->word);
		if (assignment) {
			array_add(parser, (void ***)&node->u.simple.assignments,
				  &node->u.simple.assignment_count,
				  parser->word);
			continue;
		}

		/* A name and () alone define a function. */
		name = parser->word;
		if (node->u.simple.word_count == 0 &&
		    node->u.simple.assignment_count == 0 &&
		    node->redirections == NULL) {
			token = next_token(parser, 0);
			if (token == T_LPAREN) {
				node = parse_function(parser, name);
				return node;
			}

			/* Not a function definition: the word is read as the name. */
			push_token(parser);
		}

		/* The word is the command's name or an argument. */
		array_add(parser, (void ***)&node->u.simple.words,
			  &node->u.simple.word_count, name);
		flags = 0;
	}

	/* Succeeded: the simple command. */
	return node;
}

/* Parses the rest of a function definition, after name and (. */
static struct sh_node *
parse_function(
	struct parser *parser,
	struct sh_token *name)
{
	const struct sh_builtin *builtin;
	struct sh_node *node;
	int token;
	int valid;

	/* The name must be a name, written plainly. */
	valid = is_plain_word(name, NULL);
	if (valid)
		valid = sh_var_name(name->raw);
	if (!valid)
		sh_error("syntax error: bad function name");

	/* A special builtin cannot be redefined. */
	builtin = sh_builtin_find(name->raw);
	if (builtin != NULL && (builtin->flags & SH_BUILTIN_SPECIAL) != 0)
		sh_error("%s: is a special builtin", name->raw);

	/* () is followed by the body, after any newlines. */
	token = next_token(parser, 0);
	if (token != T_RPAREN)
		syntax_error(parser, token);
	(void)next_token(parser, CHECK_NEWLINE | CHECK_KEYWORD | CHECK_ALIAS);
	push_token(parser);
	node = node_new(parser, SH_NODE_FUNCTION);
	node->u.function.name = name->raw;
	node->u.function.body = parse_command(parser);

	/* Succeeded: the definition. */
	return node;
}

/* Parses an if command, after the if. */
static struct sh_node *
parse_if(
	struct parser *parser)
{
	struct sh_node *node;
	int token;

	/* The condition and what it selects. */
	node = node_new(parser, SH_NODE_IF);
	node->u.branch.condition = parse_list(parser, LIST_COMPOUND, 0);
	expect(parser, T_THEN);
	node->u.branch.then_part = parse_list(parser, LIST_COMPOUND, 0);

	/* elif is an if of its own in the else part. */
	token = next_token(parser, CHECK_KEYWORD);
	if (token == T_ELIF) {
		node->u.branch.else_part = parse_if(parser);
		return node;
	}

	/* else: the list after it. */
	if (token == T_ELSE) {
		node->u.branch.else_part = parse_list(parser, LIST_COMPOUND, 0);
		token = next_token(parser, CHECK_KEYWORD);
	}

	/* The if ends with fi. */
	if (token != T_FI)
		syntax_error(parser, token);

	/* Succeeded: the if command. */
	return node;
}

/* Parses a while or an until loop, after the first word. */
static struct sh_node *
parse_loop(
	struct parser *parser,
	enum sh_node_kind kind)
{
	struct sh_node *node;

	/* The condition, then the body between do and done. */
	node = node_new(parser, kind);
	node->u.loop.condition = parse_list(parser, LIST_COMPOUND, 0);
	expect(parser, T_DO);
	node->u.loop.body = parse_list(parser, LIST_COMPOUND, 0);
	expect(parser, T_DONE);

	/* Succeeded: the loop. */
	return node;
}

/* Parses a for loop, after the for. */
static struct sh_node *
parse_for(
	struct parser *parser)
{
	struct sh_node *node;
	int token;
	int valid;

	/* for (( ... )) is the arithmetic loop. */
	node = parse_arith_for(parser);
	if (node != NULL)
		return node;

	/* The variable must be a name, written plainly. */
	node = node_new(parser, SH_NODE_FOR);
	token = next_token(parser, 0);
	valid = 0;
	if (token == T_WORD)
		valid = is_plain_word(parser->word, NULL);
	if (valid)
		valid = sh_var_name(parser->word->raw);
	if (!valid)
		sh_error("syntax error: bad for loop variable");
	node->u.iterate.name = parser->word;

	/* "in" and its words, or ; or a newline, come before do. */
	token = next_token(parser, CHECK_NEWLINE | CHECK_KEYWORD);
	if (token == T_IN) {
		node->u.iterate.has_in = 1;
		for (;;) {
			token = next_token(parser, 0);
			if (token != T_WORD)
				break;
			array_add(parser, (void ***)&node->u.iterate.words,
				  &node->u.iterate.word_count, parser->word);
		}

		/* The words end with a newline or a semicolon. */
		if (token != T_NEWLINE && token != T_SEMI)
			syntax_error(parser, token);
		token = next_token(parser, CHECK_NEWLINE | CHECK_KEYWORD);
	} else if (token == T_SEMI) {
		token = next_token(parser, CHECK_NEWLINE | CHECK_KEYWORD);
	}

	/* do starts the body. */
	if (token != T_DO)
		syntax_error(parser, token);

	/* The body. */
	node->u.iterate.body = parse_list(parser, LIST_COMPOUND, 0);
	expect(parser, T_DONE);

	/* Succeeded: the loop. */
	return node;
}

/* Parses a case command, after the case. */
static struct sh_node *
parse_case(
	struct parser *parser)
{
	struct sh_case_item **tail;
	struct sh_case_item *item;
	struct sh_node *node;
	int token;

	/* The word, then in. */
	node = node_new(parser, SH_NODE_CASE);
	token = next_token(parser, 0);
	if (token != T_WORD)
		syntax_error(parser, token);
	node->u.select.word = parser->word;
	token = next_token(parser, CHECK_NEWLINE | CHECK_KEYWORD);
	if (token != T_IN)
		syntax_error(parser, token);

	/* Reads arms until esac. */
	tail = &node->u.select.items;
	for (;;) {
		token = next_token(parser, CHECK_NEWLINE | CHECK_KEYWORD);
		if (token == T_ESAC)
			break;

		/* An arm may open with (. */
		if (token == T_LPAREN)
			token = next_token(parser, 0);

		/* Reads the patterns, joined by |, up to ). */
		item = sh_arena_alloc(parser->arena, sizeof(*item));
		for (;;) {
			if (token != T_WORD && token < T_IF)
				syntax_error(parser, token);
			array_add(parser, (void ***)&item->patterns,
				  &item->pattern_count, parser->word);
			token = next_token(parser, 0);
			if (token == T_RPAREN)
				break;
			if (token != T_PIPE)
				syntax_error(parser, token);
			token = next_token(parser, 0);
		}

		/* The list the arm runs, which may be empty. */
		item->body = parse_list(parser, LIST_COMPOUND, 1);
		*tail = item;
		tail = &item->next;

		/* ;; ends the arm; esac may follow the last arm directly. */
		token = next_token(parser, CHECK_NEWLINE | CHECK_KEYWORD);
		if (token == T_ESAC)
			break;
		if (token != T_DSEMI)
			syntax_error(parser, token);
	}

	/* Succeeded: the case command. */
	return node;
}

/* Parses a brace group or a subshell, after its opening token. */
static struct sh_node *
parse_group(
	struct parser *parser,
	enum sh_node_kind kind,
	int closing)
{
	struct sh_node *node;

	/* The list, then the closing token. */
	node = node_new(parser, kind);
	node->u.body = parse_list(parser, LIST_COMPOUND, 0);
	expect(parser, closing);

	/* Succeeded: the group. */
	return node;
}

/* Reads redirections written after a compound command. */
static void
parse_trailing_redirections(
	struct parser *parser,
	struct sh_node *node)
{
	struct sh_redirection **tail;
	int token;

	/* Appends each redirection that follows. */
	tail = &node->redirections;
	while (*tail != NULL)
		tail = &(*tail)->next;
	for (;;) {
		token = next_token(parser, 0);
		if (token < T_REDIRECT_FIRST || token > T_REDIRECT_LAST) {
			push_token(parser);
			break;
		}

		/* The redirection joins the list. */
		*tail = parse_redirection(parser, token);
		tail = &(*tail)->next;
	}
}

/* Parses a redirection, after its operator. */
static struct sh_redirection *
parse_redirection(
	struct parser *parser,
	int token)
{
	struct sh_redirection *redirection;
	struct heredoc *heredoc;
	struct heredoc **tail;
	size_t index;
	int io_number;
	int dash;
	const char *backquote;
	const char *substitution;
	char value;

	/* The operator decides what is done and to which descriptor. */
	io_number = parser->io_number;
	dash = parser->dash;
	redirection = sh_arena_alloc(parser->arena, sizeof(*redirection));

	/* Maps the operator. */
	switch (token) {
	case T_LESS:
		redirection->op = SH_REDIR_INPUT;
		redirection->descriptor = 0;
		break;
	case T_GREAT:
		redirection->op = SH_REDIR_OUTPUT;
		redirection->descriptor = 1;
		break;
	case T_DGREAT:
		redirection->op = SH_REDIR_APPEND;
		redirection->descriptor = 1;
		break;
	case T_CLOBBER:
		redirection->op = SH_REDIR_CLOBBER;
		redirection->descriptor = 1;
		break;
	case T_LESSGREAT:
		redirection->op = SH_REDIR_READ_WRITE;
		redirection->descriptor = 0;
		break;
	case T_LESSAND:
		redirection->op = SH_REDIR_DUP_INPUT;
		redirection->descriptor = 0;
		break;
	case T_GREATAND:
		redirection->op = SH_REDIR_DUP_OUTPUT;
		redirection->descriptor = 1;
		break;
	case T_TLESS:
		redirection->op = SH_REDIR_HERESTRING;
		redirection->descriptor = 0;
		break;
	default:
		redirection->op = SH_REDIR_HEREDOC;
		redirection->descriptor = 0;
		break;
	}

	/* An IO number chooses the descriptor. */
	if (io_number >= 0)
		redirection->descriptor = io_number;

	/* The word after it. */
	token = next_token(parser, 0);
	if (token != T_WORD)
		syntax_error(parser, token);
	redirection->word = parser->word;
	if (redirection->op != SH_REDIR_HEREDOC)
		return redirection;

	/* A here-document's body is read at the next newline. */
	heredoc = sh_arena_alloc(parser->arena, sizeof(*heredoc));
	heredoc->redirection = redirection;
	heredoc->strip_tabs = dash;
	heredoc->delimiter = sh_arena_strndup(parser->arena, parser->word->text,
					      parser->word->length);

	/* A delimiter is a word, not an expansion. */
	backquote = memchr(parser->word->raw, '`', parser->word->raw_length);
	substitution = strstr(parser->word->raw, "$(");
	if (backquote != NULL || substitution != NULL)
		sh_error("syntax error: bad here-document delimiter");

	/* Any quoting of the delimiter keeps the body from being expanded. */
	for (index = 0; index < parser->word->raw_length; index++) {
		value = parser->word->raw[index];
		if (value == '\'' || value == '"' || value == '\\')
			heredoc->quoted = 1;
	}

	/* Queues it after the others the line owes. */
	tail = &parser->heredocs;
	while (*tail != NULL)
		tail = &(*tail)->next;
	*tail = heredoc;

	/* Succeeded: the redirection. */
	return redirection;
}

/*
 * Parses a function definition written with the reserved word function
 * (bash): function name { ... }, or function name() and a command.
 */
static struct sh_node *
parse_function_keyword(
	struct parser *parser)
{
	struct sh_token *name;
	struct sh_node *node;
	int token;
	int valid;

	/* The name, written plainly. */
	token = next_token(parser, 0);
	valid = 0;
	if (token == T_WORD)
		valid = is_plain_word(parser->word, NULL);
	if (valid)
		valid = sh_var_name(parser->word->raw);
	if (!valid)
		sh_error("syntax error: bad function name");
	name = parser->word;

	/* () may follow; parse_function reads from after the (. */
	token = next_token(parser, 0);
	if (token == T_LPAREN) {
		node = parse_function(parser, name);
		return node;
	}
	push_token(parser);

	/* Without (), the body follows after any newlines. */
	(void)next_token(parser, CHECK_NEWLINE | CHECK_KEYWORD | CHECK_ALIAS);
	push_token(parser);
	node = node_new(parser, SH_NODE_FUNCTION);
	node->u.function.name = name->raw;
	node->u.function.body = parse_command(parser);

	/* Succeeded: the definition. */
	return node;
}

/*
 * Parses the || level of a [[ ... ]] expression.  Newlines may stand
 * before an operand.
 */
static struct sh_cond *
parse_cond_or(
	struct parser *parser,
	int first)
{
	struct sh_cond *node;
	struct sh_cond *joined;
	int token;

	/* The first operand, then each one joined by ||. */
	(void)first;
	node = parse_cond_and(parser);
	for (;;) {
		token = next_token(parser, 0);
		if (token != T_OR) {
			push_token(parser);
			break;
		}
		joined = sh_arena_alloc(parser->arena, sizeof(*joined));
		joined->kind = SH_COND_OR;
		joined->first = node;
		joined->second = parse_cond_and(parser);
		node = joined;
	}

	/* Succeeded. */
	return node;
}

/* Parses the && level of a [[ ... ]] expression. */
static struct sh_cond *
parse_cond_and(
	struct parser *parser)
{
	struct sh_cond *node;
	struct sh_cond *joined;
	int token;

	/* The first operand, then each one joined by &&. */
	node = parse_cond_not(parser);
	for (;;) {
		token = next_token(parser, 0);
		if (token != T_AND) {
			push_token(parser);
			break;
		}
		joined = sh_arena_alloc(parser->arena, sizeof(*joined));
		joined->kind = SH_COND_AND;
		joined->first = node;
		joined->second = parse_cond_not(parser);
		node = joined;
	}

	/* Succeeded. */
	return node;
}

/* Parses ! before a primary of a [[ ... ]] expression. */
static struct sh_cond *
parse_cond_not(
	struct parser *parser)
{
	struct sh_cond *node;
	int token;

	/* ! negates what follows. */
	token = next_token(parser, CHECK_NEWLINE);
	if (token == T_WORD && is_plain_word(parser->word, "!")) {
		node = sh_arena_alloc(parser->arena, sizeof(*node));
		node->kind = SH_COND_NOT;
		node->first = parse_cond_not(parser);
		return node;
	}
	push_token(parser);

	/* Succeeded: a primary. */
	return parse_cond_primary(parser);
}

/*
 * Parses a primary of a [[ ... ]] expression: ( expression ), a unary
 * test, a binary test, or a word.  The words are not split or globbed.
 */
static struct sh_cond *
parse_cond_primary(
	struct parser *parser)
{
	struct sh_cond *node;
	struct sh_token *word;
	struct sh_token *operand;
	const char *op;
	int token;
	int unary;

	/* ( expression ). */
	token = next_token(parser, CHECK_NEWLINE);
	if (token == T_LPAREN) {
		node = parse_cond_or(parser, 0);
		token = next_token(parser, CHECK_NEWLINE);
		if (token != T_RPAREN)
			syntax_error(parser, token);
		return node;
	}

	/* Otherwise a word, which may not be the closing ]]. */
	if (token != T_WORD || cond_closing(parser, token))
		syntax_error(parser, token);
	word = parser->word;
	node = sh_arena_alloc(parser->arena, sizeof(*node));
	node->left = word;

	/*
	 * A binary operator after the word, unless the word is a unary
	 * operator and nothing follows the second word: [[ -f == ]] tests
	 * a file named ==, as in bash.
	 */
	unary = is_plain_word(word, NULL) && word->raw_length == 2 &&
		word->raw[0] == '-' &&
		strchr("abcdefghknoprstuvwxzGLNOSR", word->raw[1]) != NULL;
	token = next_token(parser, 0);
	op = cond_binary_operator(parser, token);
	if (op != NULL && unary && token == T_WORD) {
		operand = parser->word;
		token = next_token(parser, 0);
		push_token(parser);
		if (token != T_WORD || cond_closing(parser, token)) {
			node->kind = SH_COND_UNARY;
			snprintf(node->op, sizeof(node->op), "%s", word->raw);
			node->left = operand;
			return node;
		}
	}
	if (op != NULL) {
		node->kind = SH_COND_BINARY;
		snprintf(node->op, sizeof(node->op), "%s", op);
		if (strcmp(op, "=~") == 0) {
			node->right = lex_regex_word(parser);
			return node;
		}
		token = next_token(parser, 0);
		if (token != T_WORD || cond_closing(parser, token))
			syntax_error(parser, token);
		node->right = parser->word;
		return node;
	}

	/* A unary operator and its operand. */
	if (unary && token == T_WORD && !cond_closing(parser, token)) {
		node->kind = SH_COND_UNARY;
		snprintf(node->op, sizeof(node->op), "%s", word->raw);
		node->left = parser->word;
		return node;
	}

	/* A word alone. */
	push_token(parser);
	node->kind = SH_COND_WORD;
	return node;
}

/* Reports whether a token is the ]] that closes [[. */
static int
cond_closing(
	struct parser *parser,
	int token)
{
	/* ]] written plainly. */
	if (token != T_WORD)
		return 0;
	return is_plain_word(parser->word, "]]");
}

/* Returns the binary operator a token is inside [[ ... ]], or NULL. */
static const char *
cond_binary_operator(
	struct parser *parser,
	int token)
{
	static const char *const operators[] = {
		"==", "=", "!=", "=~", "-eq", "-ne", "-lt", "-le", "-gt", "-ge",
		"-nt", "-ot", "-ef", NULL
	};
	int index;

	/* < and > are read as redirection operators. */
	if (token == T_LESS && parser->io_number < 0)
		return "<";
	if (token == T_GREAT && parser->io_number < 0)
		return ">";
	if (token != T_WORD)
		return NULL;

	/* The word operators. */
	for (index = 0; operators[index] != NULL; index++) {
		if (is_plain_word(parser->word, operators[index]))
			return operators[index];
	}

	/* Not one. */
	return NULL;
}

/*
 * Reads the regular expression after =~ as one word: parentheses and |
 * belong to it, and it ends at a blank outside parentheses.
 */
static struct sh_token *
lex_regex_word(
	struct parser *parser)
{
	struct sh_token *word;
	struct word_buffer buffer;
	int value;
	int depth;

	/* Skips the blanks before it. */
	do
		value = getc_continued();
	while (value == ' ' || value == '\t');

	/* The characters up to a blank or a newline at depth 0. */
	memset(&buffer, 0, sizeof(buffer));
	depth = 0;
	for (;;) {
		if (value == EOF || value == '\n')
			break;
		if ((value == ' ' || value == '\t') && depth == 0)
			break;
		if (value == ')' && depth == 0)
			break;
		if (value == '(') {
			depth++;
			buffer_add(&buffer, value);
		} else if (value == ')') {
			depth--;
			buffer_add(&buffer, value);
		} else if (value == '\\') {
			buffer_add(&buffer, value);
			value = sh_input_getc();
			if (value == EOF)
				break;
			buffer_add(&buffer, value);
		} else if (value == '\'') {
			buffer_add(&buffer, value);
			lex_single(&buffer);
		} else if (value == '"') {
			buffer_add(&buffer, value);
			lex_double(parser, &buffer);
		} else if (value == '$') {
			lex_dollar(parser, &buffer, 0);
		} else {
			buffer_add(&buffer, value);
		}
		value = getc_continued();
	}
	sh_input_ungetc(value);

	/* The word. */
	if (buffer.text == NULL)
		sh_error("syntax error: missing regular expression after =~");
	word = make_word(parser, buffer.text, buffer.length);
	free(buffer.text);

	/* Succeeded. */
	return word;
}

/*
 * Parses (( expression )) after the first (, when the second follows at
 * once (bash).  Returns NULL, having given the text back, when what
 * follows is not one arithmetic expression, so that it is read as
 * nested subshells.
 */
static struct sh_node *
parse_arith_command(
	struct parser *parser)
{
	struct sh_node *node;
	char *text;
	int value;
	int complete;

	/* ( must be followed by ( directly. */
	value = sh_input_getc();
	if (value != '(') {
		sh_input_ungetc(value);
		return NULL;
	}

	/* The text up to )). */
	text = read_arith_text(&complete);
	if (!complete) {
		sh_input_give_back_text(text, strlen(text));
		sh_input_give_back_text("(", 1);
		free(text);
		return NULL;
	}

	/* The command. */
	node = node_new(parser, SH_NODE_ARITH);
	node->u.arith = sh_arena_strndup(parser->arena, text, strlen(text));
	free(text);

	/* Succeeded. */
	return node;
}

/*
 * Parses for (( init; test; step )) and its body (bash), after for, or
 * returns NULL when for is followed by anything else.
 */
static struct sh_node *
parse_arith_for(
	struct parser *parser)
{
	struct sh_node *node;
	char *text;
	char *parts[3];
	char *cursor;
	int value;
	int second;
	int complete;
	int depth;
	int index;
	int token;

	/* for, blanks, then (( directly. */
	if (parser->pushed)
		return NULL;
	do
		value = getc_continued();
	while (value == ' ' || value == '\t');
	if (value != '(') {
		sh_input_ungetc(value);
		return NULL;
	}
	second = sh_input_getc();
	if (second != '(') {
		sh_input_ungetc(second);
		sh_input_ungetc(value);
		return NULL;
	}
	text = read_arith_text(&complete);
	if (!complete)
		sh_error("syntax error: bad for loop");

	/* The three expressions, split at the semicolons outside parentheses. */
	parts[0] = text;
	index = 1;
	depth = 0;
	for (cursor = text; *cursor != '\0'; cursor++) {
		if (*cursor == '(')
			depth++;
		else if (*cursor == ')')
			depth--;
		else if (*cursor == ';' && depth == 0 && index < 3) {
			*cursor = '\0';
			parts[index++] = cursor + 1;
		}
	}
	if (index != 3)
		sh_error("syntax error: bad for loop");
	node = node_new(parser, SH_NODE_ARITH_FOR);
	node->u.arith_for.init = sh_arena_strndup(parser->arena, parts[0], strlen(parts[0]));
	node->u.arith_for.test = sh_arena_strndup(parser->arena, parts[1], strlen(parts[1]));
	node->u.arith_for.step = sh_arena_strndup(parser->arena, parts[2], strlen(parts[2]));
	free(text);

	/* ; or newlines, then do ... done or { ... }. */
	token = next_token(parser, CHECK_NEWLINE | CHECK_KEYWORD);
	if (token == T_SEMI)
		token = next_token(parser, CHECK_NEWLINE | CHECK_KEYWORD);
	if (token == T_LBRACE) {
		node->u.arith_for.body = parse_group(parser, SH_NODE_GROUP, T_RBRACE);
		return node;
	}
	if (token != T_DO)
		syntax_error(parser, token);
	node->u.arith_for.body = parse_list(parser, LIST_COMPOUND, 0);
	expect(parser, T_DONE);

	/* Succeeded. */
	return node;
}

/*
 * Reads the text of an arithmetic command after ((, up to the )) that
 * closes it, counting parentheses.  *complete is cleared when a ) at
 * depth 0 is not followed by another; the text read, with that ) and
 * what followed, is returned all the same, for the caller to give back.
 */
static char *
read_arith_text(
	int *complete)
{
	struct word_buffer buffer;
	int value;
	int depth;

	/* The characters, keeping count of parentheses. */
	memset(&buffer, 0, sizeof(buffer));
	depth = 0;
	*complete = 0;
	for (;;) {
		value = getc_continued();
		if (value == SH_INPUT_END_OF_ALIAS)
			continue;
		if (value == EOF)
			unterminated("arithmetic command");
		if (value == '(') {
			depth++;
		} else if (value == ')' && depth > 0) {
			depth--;
		} else if (value == ')') {
			value = sh_input_getc();
			if (value == ')') {
				*complete = 1;
				break;
			}
			buffer_add(&buffer, ')');
			if (value != EOF)
				buffer_add(&buffer, value);
			break;
		}
		buffer_add(&buffer, value);
	}

	/* Succeeded: the text, which the caller frees. */
	if (buffer.text == NULL)
		return sh_strdup("");
	return buffer.text;
}

/*
 * Adds 2>&1 after the redirections of a command that |& joins to the next,
 * as bash does: its standard error goes where its standard output goes,
 * into the pipe.
 */
static void
add_stderr_to_pipe(
	struct parser *parser,
	struct sh_node *command)
{
	struct sh_redirection *redirection;
	struct sh_redirection **tail;

	/* 2>&1, at the end of the command's redirections. */
	redirection = sh_arena_alloc(parser->arena, sizeof(*redirection));
	redirection->op = SH_REDIR_DUP_OUTPUT;
	redirection->descriptor = 2;
	redirection->word = make_word(parser, "1", 1);
	tail = &command->redirections;
	while (*tail != NULL)
		tail = &(*tail)->next;
	*tail = redirection;
}

/* Reads a token that must be the one given, as a reserved word if it is one. */
static void
expect(
	struct parser *parser,
	int token)
{
	int read;

	/* A different token is a syntax error. */
	read = next_token(parser, CHECK_NEWLINE | CHECK_KEYWORD);
	if (read != token)
		syntax_error(parser, read);
}

/* Reports an unexpected token and throws the error. */
static void
syntax_error(
	struct parser *parser,
	int token)
{
	/* The end of the input is reported as such; a word by its text. */
	if (token == T_EOF)
		sh_error("syntax error: unexpected end of file");
	if (token == T_WORD)
		sh_error("syntax error: unexpected word \"%s\"", parser->word->raw);
	sh_error("syntax error: unexpected \"%s\"", token_names[token]);
}

/* Makes a node of a kind, at the line of the last token read. */
static struct sh_node *
node_new(
	struct parser *parser,
	enum sh_node_kind kind)
{
	struct sh_node *node;

	/* Allocates it zeroed in the arena. */
	node = sh_arena_alloc(parser->arena, sizeof(*node));
	node->kind = kind;
	node->line = parser->line;

	/* Succeeded: the node. */
	return node;
}

/* Makes a node that joins two others (or wraps one). */
static struct sh_node *
node_binary(
	struct parser *parser,
	enum sh_node_kind kind,
	struct sh_node *left,
	struct sh_node *right)
{
	struct sh_node *node;

	/* Wrapping nodes keep one child as their body. */
	node = node_new(parser, kind);
	if (kind == SH_NODE_BACKGROUND || kind == SH_NODE_NOT) {
		node->u.body = left;
		return node;
	}

	/* The two sides of the operator. */
	node->u.binary.left = left;
	node->u.binary.right = right;

	/* Succeeded: the node. */
	return node;
}

/* Adds an item to an array in the arena, doubling it when it is full. */
static void
array_add(
	struct parser *parser,
	void ***array,
	size_t *count,
	void *item)
{
	void **grown;
	size_t capacity;

	/* The capacity is the least power of two, from 4, that holds count. */
	capacity = 4;
	while (capacity < *count)
		capacity *= 2U;

	/* A new array, or a full one, is copied to one twice the size. */
	if (*count == 0 || *count == capacity) {
		if (*count == 0)
			capacity = 4;
		else
			capacity *= 2U;
		grown = sh_arena_alloc(parser->arena,
				       capacity * sizeof(*grown));
		if (*count != 0)
			memcpy(grown, *array, *count * sizeof(*grown));
		*array = grown;
	}

	/* The item goes at the end. */
	(*array)[(*count)++] = item;
}

/*
 * Reads the next token.
 *
 * A token given back is read again first.  With CHECK_NEWLINE newlines are
 * skipped; with CHECK_KEYWORD a word that is a reserved word is read as one;
 * with CHECK_ALIAS a word that names an alias is replaced by the alias's text,
 * which is read in its place.
 */
static int
next_token(
	struct parser *parser,
	int flags)
{
	const char *value;
	int token;
	int plain;
	int active;

	/* Reads tokens until one is not replaced by an alias. */
	for (;;) {
		if (parser->pushed) {
			parser->pushed = 0;
			token = parser->token;
		} else {
			token = lex(parser);
			parser->token = token;
		}

		/* Newlines are skipped where a command may start after them. */
		if ((flags & CHECK_NEWLINE) != 0) {
			while (token == T_NEWLINE) {
				token = lex(parser);
				parser->token = token;
			}
		}

		/* The word after an alias ending in a blank is checked too. */
		if (sh_input_alias_blank) {
			sh_input_alias_blank = 0;
			if (token == T_WORD)
				flags |= CHECK_ALIAS;
		}

		/* Only a word can be a keyword or an alias. */
		if (token != T_WORD)
			return token;

		/* A reserved word where one may stand. */
		if ((flags & CHECK_KEYWORD) != 0) {
			token = keyword_of(parser->word);
			if (token != T_WORD) {
				parser->token = token;
				return token;
			}
		}

		/* An alias, unless being read or inside a substitution. */
		if ((flags & CHECK_ALIAS) == 0 || parser->alias_checked ||
		    parser->substitution > 0)
			return T_WORD;
		parser->alias_checked = 1;
		plain = is_plain_word(parser->word, NULL);
		if (!plain)
			return T_WORD;
		value = sh_alias_get(parser->word->raw);
		if (value == NULL)
			return T_WORD;
		active = sh_input_alias_active(parser->word->raw);
		if (active)
			return T_WORD;
		sh_input_push_alias(value, parser->word->raw);
		flags &= ~CHECK_NEWLINE;
	}
}

/* Gives the last token back, to be read again. */
static void
push_token(
	struct parser *parser)
{
	/* One token of lookahead is all the grammar needs. */
	parser->pushed = 1;
}

/* Reports the reserved word a word is, or T_WORD. */
static int
keyword_of(
	const struct sh_token *word)
{
	int index;
	int matched;

	/* A reserved word is written plainly. */
	for (index = 0; keywords[index].text != NULL; index++) {
		matched = is_plain_word(word, keywords[index].text);
		if (matched)
			return keywords[index].token;
	}

	/* Not reserved. */
	return T_WORD;
}

/*
 * Reports whether a word is written without quotes or expansions, and, with
 * text, whether it is exactly that text.
 */
static int
is_plain_word(
	const struct sh_token *word,
	const char *text)
{
	size_t index;
	size_t length;
	int compare;
	char value;

	/* Compares the text when one is given. */
	if (text != NULL) {
		length = strlen(text);
		if (length != word->raw_length)
			return 0;
		compare = memcmp(word->raw, text, word->raw_length);
		if (compare != 0)
			return 0;
		return 1;
	}

	/* Otherwise looks for a quote or an expansion. */
	for (index = 0; index < word->raw_length; index++) {
		value = word->raw[index];
		if (value == '\'' || value == '"' || value == '\\' ||
		    value == '$' || value == '`')
			return 0;
	}

	/* Succeeded: a plain word. */
	return 1;
}

/*
 * Reads the next token from the input: an operator, a newline, a word, or
 * the end.  Blanks and comments are skipped; the here-documents a line owes
 * are read after its newline.
 */
static int
lex(
	struct parser *parser)
{
	int value;
	int next;
	int operator;
	int token;

	/* Skips blanks, the ends of aliases, line continuations and comments. */
	parser->io_number = -1;
	parser->dash = 0;
	parser->alias_checked = 0;
	for (;;) {
		value = getc_continued();
		if (value == ' ' || value == '\t' ||
		    value == SH_INPUT_END_OF_ALIAS)
			continue;
		if (value == '#') {
			do
				value = sh_input_getc();
			while (value != '\n' && value != EOF);
		}

		break;
	}

	/* The token starts on this line. */
	parser->line = sh_input_line();
	sh_command_line = parser->line;

	/* The end, a newline, an operator, or a word. */
	if (value == EOF)
		return T_EOF;
	if (value == '\n') {
		parser->line--;
		if (parser->heredocs != NULL)
			read_heredocs(parser);
		return T_NEWLINE;
	}

	/*
	 * <( and >( start a process substitution (bash), which POSIX reads
	 * as a redirection followed by a syntax error.
	 */
	if (value == '<' || value == '>') {
		next = getc_continued();
		if (next == '(') {
			token = lex_process_substitution(parser, value);
			return token;
		}
		sh_input_ungetc(next);
	}

	/* An operator. */
	operator = is_operator_start(value);
	if (operator) {
		token = lex_operator(parser, value);
		return token;
	}

	/* Succeeded: a word. */
	token = lex_word(parser, value);
	return token;
}

/* Reads an operator that starts with value. */
static int
lex_operator(
	struct parser *parser,
	int value)
{
	int next;

	/* Single-character operators need no look ahead. */
	if (value == '(')
		return T_LPAREN;
	if (value == ')')
		return T_RPAREN;

	/* The rest may be one character or two. */
	next = getc_continued();
	switch (value) {
	case ';':
		if (next == ';')
			return T_DSEMI;
		sh_input_ungetc(next);
		return T_SEMI;
	case '&':
		if (next == '&')
			return T_AND;
		sh_input_ungetc(next);
		return T_AMP;
	case '|':
		if (next == '|')
			return T_OR;
		if (next == '&')
			return T_PIPEAMP;
		sh_input_ungetc(next);
		return T_PIPE;
	case '<':
		if (next == '<') {
			next = getc_continued();
			if (next == '-') {
				parser->dash = 1;
				return T_DLESSDASH;
			}

			/* <<<: a here-string. */
			if (next == '<')
				return T_TLESS;

			/* << alone. */
			sh_input_ungetc(next);
			return T_DLESS;
		}

		/* <&. */
		if (next == '&')
			return T_LESSAND;
		if (next == '>')
			return T_LESSGREAT;
		sh_input_ungetc(next);
		return T_LESS;
	default:
		if (next == '>')
			return T_DGREAT;
		if (next == '&')
			return T_GREATAND;
		if (next == '|')
			return T_CLOBBER;
		sh_input_ungetc(next);
		return T_GREAT;
	}
}

/*
 * Reads a word whose first character is value.  A word that is a single
 * digit directly before < or > is the descriptor of a redirection instead.
 */
static int
lex_word(
	struct parser *parser,
	int value)
{
	struct word_buffer buffer;
	int token;
	int plain;
	int operator;

	/* Collects characters up to a blank, a newline or an operator. */
	memset(&buffer, 0, sizeof(buffer));
	plain = 1;
	for (;;) {
		if (value == EOF || value == SH_INPUT_END_OF_ALIAS ||
		    value == ' ' || value == '\t' || value == '\n')
			break;
		operator = is_operator_start(value);
		if (operator)
			break;
		if (value == '\\') {
			buffer_add(&buffer, value);
			value = sh_input_getc();
			if (value == EOF)
				break;
			buffer_add(&buffer, value);
			plain = 0;
		} else if (value == '\'') {
			buffer_add(&buffer, value);
			lex_single(&buffer);
			plain = 0;
		} else if (value == '"') {
			buffer_add(&buffer, value);
			lex_double(parser, &buffer);
			plain = 0;
		} else if (value == '$') {
			lex_dollar(parser, &buffer, 0);
			plain = 0;
		} else if (value == '`') {
			buffer_add(&buffer, value);
			lex_backquote(parser, &buffer);
			plain = 0;
		} else {
			buffer_add(&buffer, value);
		}

		/* The next character of the word. */
		value = getc_continued();
	}

	/* A lone digit before a redirection operator names its descriptor. */
	if (plain && buffer.length == 1 && buffer.text[0] >= '0' &&
	    buffer.text[0] <= '9' && (value == '<' || value == '>')) {
		token = lex_operator(parser, value);
		parser->io_number = buffer.text[0] - '0';
		free(buffer.text);
		return token;
	}

	/* The character after the word is read again. */
	sh_input_ungetc(value);

	/* Makes the word, in the arena. */
	if (buffer.text == NULL)
		parser->word = make_word(parser, "", 0);
	else
		parser->word = make_word(parser, buffer.text, buffer.length);
	free(buffer.text);

	/* Succeeded: a word. */
	return T_WORD;
}

/*
 * Reads a process substitution after <( or >(: the commands up to the
 * closing parenthesis, as a word that expansion turns into /dev/fd/N.
 */
static int
lex_process_substitution(
	struct parser *parser,
	int direction)
{
	struct word_buffer buffer;

	/* The text, as written, with its commands parsed for their errors. */
	memset(&buffer, 0, sizeof(buffer));
	buffer_add(&buffer, direction);
	buffer_add(&buffer, '(');
	lex_substitution(parser, &buffer);
	parser->word = make_word(parser, buffer.text, buffer.length);
	parser->word->process = direction;
	free(buffer.text);

	/* Succeeded: a word. */
	return T_WORD;
}

/* Reads a single quotation, after its opening quote. */
static void
lex_single(
	struct word_buffer *buffer)
{
	int value;

	/* Everything up to the closing quote is taken as it stands. */
	for (;;) {
		value = sh_input_getc();
		if (value == SH_INPUT_END_OF_ALIAS)
			continue;
		if (value == EOF)
			unterminated("quoted string");
		buffer_add(buffer, value);
		if (value == '\'')
			break;
	}
}

/*
 * Reads a dollar-single-quotation, after $': the backslash escapes of
 * XCU 2.2.4 (and bash's \e, \E, \u and \U) give the characters they
 * stand for, which go into the word as a single quotation, so that the
 * rest of the shell sees plain quoted text.  A NUL ends the text, as in
 * bash; what follows it up to the closing quote is dropped.
 */
static void
lex_dollar_single(
	struct word_buffer *buffer)
{
	unsigned long code;
	int value;
	int ended;
	int kind;

	/* The text, opened as a single quotation. */
	buffer_add(buffer, '\'');
	ended = 0;
	for (;;) {
		value = sh_input_getc();
		if (value == SH_INPUT_END_OF_ALIAS)
			continue;
		if (value == EOF)
			unterminated("quoted string");
		if (value == '\'')
			break;

		/* A plain character is itself. */
		if (value != '\\') {
			if (!ended)
				buffer_add_quoted(buffer, value);
			continue;
		}

		/* An escape: a byte, or a character written in UTF-8. */
		kind = dollar_single_escape(&code);
		if (ended)
			continue;
		if (kind == 0) {
			buffer_add_quoted(buffer, '\\');
			buffer_add_quoted(buffer, (int)code);
			continue;
		}
		if (kind == 2) {
			buffer_add_utf8(buffer, code);
			continue;
		}
		if (code == 0) {
			ended = 1;
			continue;
		}
		buffer_add_quoted(buffer, (int)code);
	}

	/* The quotation closes. */
	buffer_add(buffer, '\'');
}

/*
 * Reads the escape after a backslash in $'...'.  Returns 1 with a byte in
 * *code, 2 with a character to write in UTF-8, or 0 when the backslash
 * escapes nothing (*code is then the character after it, kept with it).
 */
static int
dollar_single_escape(
	unsigned long *code)
{
	unsigned long value;
	int digits;
	int limit;
	int base;
	int kind;
	int next;
	int digit;

	/* The character after the backslash. */
	next = sh_input_getc();
	if (next == EOF)
		unterminated("quoted string");

	/* The single-letter escapes. */
	kind = 1;
	switch (next) {
	case 'a':
		*code = 7;
		return 1;
	case 'b':
		*code = 8;
		return 1;
	case 'e':
	case 'E':
		*code = 27;
		return 1;
	case 'f':
		*code = 12;
		return 1;
	case 'n':
		*code = 10;
		return 1;
	case 'r':
		*code = 13;
		return 1;
	case 't':
		*code = 9;
		return 1;
	case 'v':
		*code = 11;
		return 1;
	case '\\':
	case '\'':
	case '"':
	case '?':
		*code = (unsigned long)next;
		return 1;
	case 'c':
		/* \cX: the control character of X. */
		next = sh_input_getc();
		if (next == EOF)
			unterminated("quoted string");
		if (next == '\\')
			(void)sh_input_getc();
		*code = (unsigned long)next & 0x1fU;
		return 1;
	default:
		break;
	}

	/* Octal (up to three digits), hexadecimal and Unicode escapes. */
	base = 0;
	limit = 0;
	if (next >= '0' && next <= '7') {
		base = 8;
		limit = 3;
		sh_input_ungetc(next);
	} else if (next == 'x') {
		base = 16;
		limit = 2;
	} else if (next == 'u') {
		base = 16;
		limit = 4;
		kind = 2;
	} else if (next == 'U') {
		base = 16;
		limit = 8;
		kind = 2;
	}

	/* Any other character is not an escape. */
	if (base == 0) {
		*code = (unsigned long)next;
		return 0;
	}

	/* The digits. */
	value = 0;
	for (digits = 0; digits < limit; digits++) {
		next = sh_input_getc();
		digit = -1;
		if (next >= '0' && next <= '9')
			digit = next - '0';
		else if (base == 16 && next >= 'a' && next <= 'f')
			digit = next - 'a' + 10;
		else if (base == 16 && next >= 'A' && next <= 'F')
			digit = next - 'A' + 10;
		if (digit < 0 || digit >= base) {
			sh_input_ungetc(next);
			break;
		}
		value = value * (unsigned long)base + (unsigned long)digit;
	}

	/* A \x or \u without digits is kept as written. */
	if (digits == 0 && base == 16) {
		*code = kind == 2 ? (limit == 4 ? 'u' : 'U') : 'x';
		return 0;
	}

	/* Succeeded. */
	*code = value & (kind == 2 ? 0x1fffffUL : 0xffUL);
	return kind;
}

/* Adds a character inside a single quotation, a quote as '\''. */
static void
buffer_add_quoted(
	struct word_buffer *buffer,
	int value)
{
	/* A quote closes the quotation, is escaped, and opens it again. */
	if (value == '\'') {
		buffer_add(buffer, '\'');
		buffer_add(buffer, '\\');
		buffer_add(buffer, '\'');
		buffer_add(buffer, '\'');
		return;
	}

	/* Anything else is itself. */
	buffer_add(buffer, value);
}

/* Adds a character, written in UTF-8, inside a single quotation. */
static void
buffer_add_utf8(
	struct word_buffer *buffer,
	unsigned long code)
{
	/* One to four bytes by the size of the code. */
	if (code < 0x80UL) {
		buffer_add_quoted(buffer, (int)code);
	} else if (code < 0x800UL) {
		buffer_add(buffer, (int)(0xc0UL | (code >> 6)));
		buffer_add(buffer, (int)(0x80UL | (code & 0x3fUL)));
	} else if (code < 0x10000UL) {
		buffer_add(buffer, (int)(0xe0UL | (code >> 12)));
		buffer_add(buffer, (int)(0x80UL | ((code >> 6) & 0x3fUL)));
		buffer_add(buffer, (int)(0x80UL | (code & 0x3fUL)));
	} else {
		buffer_add(buffer, (int)(0xf0UL | (code >> 18)));
		buffer_add(buffer, (int)(0x80UL | ((code >> 12) & 0x3fUL)));
		buffer_add(buffer, (int)(0x80UL | ((code >> 6) & 0x3fUL)));
		buffer_add(buffer, (int)(0x80UL | (code & 0x3fUL)));
	}
}

/* Reads a double quotation, after its opening quote. */
static void
lex_double(
	struct parser *parser,
	struct word_buffer *buffer)
{
	int value;

	/* Backslashes, expansions, and the closing quote are recognized. */
	for (;;) {
		value = getc_continued();
		if (value == SH_INPUT_END_OF_ALIAS)
			continue;
		if (value == EOF)
			unterminated("quoted string");
		if (value == '$') {
			lex_dollar(parser, buffer, 1);
			continue;
		}

		/* Any other character is part of the text. */
		buffer_add(buffer, value);
		if (value == '"')
			break;
		if (value == '`') {
			lex_backquote(parser, buffer);
		} else if (value == '\\') {
			value = sh_input_getc();
			if (value == EOF)
				unterminated("quoted string");
			buffer_add(buffer, value);
		}
	}
}

/*
 * Reads what a dollar sign begins: ${...}, $(...), $((...)), or nothing
 * more (a name or a special parameter is read as ordinary characters).
 */
static void
lex_dollar(
	struct parser *parser,
	struct word_buffer *buffer,
	int in_double)
{
	int value;

	/* Looks at what follows the dollar sign. */
	buffer_add(buffer, '$');
	value = getc_continued();

	/* $'...' (XCU 2.2.4, Issue 8) is written out as the quotation it means. */
	if (value == '\'' && !in_double) {
		buffer->length--;
		lex_dollar_single(buffer);
		return;
	}
	if (value == '{') {
		buffer_add(buffer, value);
		lex_brace(parser, buffer, in_double);
		return;
	}

	/* $ without ( is kept as it is. */
	if (value != '(') {
		sh_input_ungetc(value);
		return;
	}

	/* $(( is arithmetic, and $( a command substitution. */
	buffer_add(buffer, value);
	value = getc_continued();
	if (value == '(') {
		buffer_add(buffer, value);
		lex_arithmetic(parser, buffer);
		return;
	}

	/* $( alone is a command substitution. */
	sh_input_ungetc(value);
	lex_substitution(parser, buffer);
}

/* Reads a braced parameter expansion, after ${. */
static void
lex_brace(
	struct parser *parser,
	struct word_buffer *buffer,
	int in_double)
{
	size_t start;
	int value;

	/* The word of ${name#word} and the like quotes as if on its own. */
	start = buffer->length;
	for (;;) {
		value = getc_continued();
		if (value == SH_INPUT_END_OF_ALIAS)
			continue;
		if (value == EOF)
			unterminated("parameter expansion");
		if (value == '}') {
			buffer_add(buffer, value);
			break;
		}

		/* A # or % after the name starts a pattern. */
		if ((value == '#' || value == '%') && buffer->length > start &&
		    !(buffer->length == start + 1 && buffer->text[start] == '#'))
			in_double = 0;
		if (value == '\\') {
			buffer_add(buffer, value);
			value = sh_input_getc();
			if (value == EOF)
				unterminated("parameter expansion");
			buffer_add(buffer, value);
		} else if (value == '\'' && !in_double) {
			buffer_add(buffer, value);
			lex_single(buffer);
		} else if (value == '"') {
			buffer_add(buffer, value);
			lex_double(parser, buffer);
		} else if (value == '$') {
			lex_dollar(parser, buffer, in_double);
		} else if (value == '`') {
			buffer_add(buffer, value);
			lex_backquote(parser, buffer);
		} else {
			buffer_add(buffer, value);
		}
	}
}

/*
 * Reads an arithmetic expansion, after $((.  The expansion ends at the first
 * )) outside any inner parentheses.  When a ) closes the outer parenthesis on
 * its own, the text was a command substitution that begins with a subshell,
 * and it is read again as one.
 */
static void
lex_arithmetic(
	struct parser *parser,
	struct word_buffer *buffer)
{
	size_t start;
	int depth;
	int value;

	/* Scans the expression, keeping count of parentheses. */
	start = buffer->length;
	depth = 0;
	for (;;) {
		value = getc_continued();
		if (value == SH_INPUT_END_OF_ALIAS)
			continue;
		if (value == EOF)
			unterminated("arithmetic expansion");
		if (value == '$') {
			lex_dollar(parser, buffer, 0);
			continue;
		}

		/* Any other character is part of the text. */
		buffer_add(buffer, value);
		if (value == '\'') {
			lex_single(buffer);
		} else if (value == '"') {
			lex_double(parser, buffer);
		} else if (value == '`') {
			lex_backquote(parser, buffer);
		} else if (value == '\\') {
			value = sh_input_getc();
			if (value != EOF)
				buffer_add(buffer, value);
		} else if (value == '(') {
			depth++;
		} else if (value == ')') {
			if (depth > 0) {
				depth--;
				continue;
			}

			/* A ) at depth 0 must be followed by another. */
			value = getc_continued();
			if (value == ')') {
				buffer_add(buffer, value);
				return;
			}

			/* A single ) ends nothing yet. */
			sh_input_ungetc(value);
			break;
		}
	}

	/*
	 * Not arithmetic: gives back what followed "$(" and reads it as a
	 * command substitution whose first command is a subshell.
	 */
	sh_input_push_back_text(buffer->text + start - 1,
				buffer->length - start + 1);
	buffer->length = start - 1;
	lex_substitution(parser, buffer);
}

/*
 * Reads a command substitution, after $(.  A parser of its own reads the
 * commands up to the closing parenthesis, and what it read is the text of the
 * substitution.
 */
static void
lex_substitution(
	struct parser *parser,
	struct word_buffer *buffer)
{
	struct parser inner;
	char *text;
	size_t length;
	size_t index;
	int token;

	/* Parses the commands, capturing the characters they are made of. */
	memset(&inner, 0, sizeof(inner));
	inner.arena = parser->arena;
	inner.substitution = parser->substitution + 1;
	sh_input_capture_start();
	token = next_token(&inner, CHECK_NEWLINE | CHECK_KEYWORD);
	push_token(&inner);
	if (token != T_RPAREN)
		(void)parse_list(&inner, LIST_COMPOUND, 1);
	token = next_token(&inner, CHECK_NEWLINE | CHECK_KEYWORD);
	if (token != T_RPAREN)
		syntax_error(&inner, token);
	text = sh_input_capture_stop(&length);

	/* The captured text ends with the closing parenthesis. */
	for (index = 0; index < length; index++)
		buffer_add(buffer, (unsigned char)text[index]);
	free(text);
}

/*
 * Reads a backquoted command substitution, after its opening backquote.  Its
 * commands, with the backslashes before $ ` and \ removed, are parsed to
 * report their syntax errors where they are written.
 */
static void
lex_backquote(
	struct parser *parser,
	struct word_buffer *buffer)
{
	struct word_buffer inner;
	int value;

	/* A backslash protects the character after it; ` ends it. */
	memset(&inner, 0, sizeof(inner));
	for (;;) {
		value = getc_continued();
		if (value == SH_INPUT_END_OF_ALIAS)
			continue;
		if (value == EOF) {
			free(inner.text);
			unterminated("command substitution");
		}

		/* The character is part of the word. */
		buffer_add(buffer, value);
		if (value == '`')
			break;
		if (value == '\\') {
			value = sh_input_getc();
			if (value == EOF) {
				free(inner.text);
				unterminated("command substitution");
			}

			/* The escaped character is part of the word. */
			buffer_add(buffer, value);
			if (value != '$' && value != '`' && value != '\\')
				buffer_add(&inner, '\\');
		}

		/* The character is part of the inner text. */
		buffer_add(&inner, value);
	}

	/* The commands inside must parse. */
	if (inner.text != NULL)
		check_syntax(parser, inner.text, inner.length);
	free(inner.text);
}

/* Parses the text of a substitution only to report its syntax errors. */
static void
check_syntax(
	struct parser *parser,
	const char *text,
	size_t length)
{
	struct parser inner;
	int captures;
	int token;

	/* The text is read as input of its own, without being captured. */
	captures = sh_input_capture_suspend();
	sh_input_push_string(text, length, sh_input_line());
	memset(&inner, 0, sizeof(inner));
	inner.arena = parser->arena;
	inner.substitution = parser->substitution + 1;

	/* Lists up to the end of the text. */
	for (;;) {
		token = next_token(&inner, CHECK_NEWLINE | CHECK_KEYWORD);
		if (token == T_EOF)
			break;
		push_token(&inner);
		(void)parse_list(&inner, LIST_COMPOUND, 1);
		token = next_token(&inner, CHECK_NEWLINE | CHECK_KEYWORD);
		if (token != T_EOF)
			syntax_error(&inner, token);
		break;
	}

	/* The inner source is done with. */
	sh_input_pop();
	sh_input_capture_resume(captures);
}

/* Reports whether a character begins an operator. */
static int
is_operator_start(
	int value)
{
	/* Dispatches on the character. */
	switch (value) {
	case ';':
	case '&':
	case '|':
	case '<':
	case '>':
	case '(':
	case ')':
		return 1;
	default:
		return 0;
	}
}

/* Reads a character, joining lines a backslash-newline continues. */
static int
getc_continued(
	void)
{
	int value;
	int next;

	/* Skips each backslash-newline pair. */
	for (;;) {
		value = sh_input_getc();
		if (value != '\\')
			return value;
		next = sh_input_getc();
		if (next != '\n') {
			sh_input_ungetc(next);
			return value;
		}
	}
}

/* Reports input that ended inside a construct. */
static void
unterminated(
	const char *what)
{
	/* The error is thrown. */
	sh_error("syntax error: unterminated %s", what);
}

/* Adds a character to a word being read. */
static void
buffer_add(
	struct word_buffer *buffer,
	int value)
{
	size_t capacity;

	/* Grows the buffer, keeping room for a terminator. */
	if (buffer->length + 2U > buffer->capacity) {
		capacity = 32U;
		if (buffer->capacity != 0)
			capacity = buffer->capacity * 2U;
		buffer->text = sh_realloc(buffer->text, capacity);
		buffer->capacity = capacity;
	}

	/* The character, and a terminator after it. */
	buffer->text[buffer->length++] = (char)value;
	buffer->text[buffer->length] = '\0';
}

/*
 * Makes a word from its raw text: the raw text itself, and its text with
 * the quotes removed and each character's quoting.  An expansion is copied
 * into the text as it stands; the grammar only needs to know that it is not
 * a plain word.
 */
static struct sh_token *
make_word(
	struct parser *parser,
	const char *raw,
	size_t length)
{
	struct sh_token *word;
	const char *text;
	const char *end;
	const char *stop;
	size_t out;
	int in_double;
	unsigned char mark;

	/* The raw text, and room for the text, which is never longer. */
	word = sh_arena_alloc(parser->arena, sizeof(*word));
	word->raw = sh_arena_strndup(parser->arena, raw, length);
	word->raw_length = length;
	word->text = sh_arena_alloc(parser->arena, length + 1U);
	word->quote = sh_arena_alloc(parser->arena, length + 1U);

	/* Removes the quotes, marking how each character was quoted. */
	text = word->raw;
	end = text + length;
	out = 0;
	in_double = 0;
	while (text < end) {
		if (in_double)
			mark = SH_QUOTE_DOUBLE;
		else
			mark = SH_QUOTE_UNQUOTED;
		if (*text == '\\' && text + 1 < end) {
			if (in_double && text[1] != '$' && text[1] != '`' &&
			    text[1] != '"' && text[1] != '\\') {
				word->text[out] = *text++;
				word->quote[out++] = mark;
				continue;
			}

			/* The escaped character. */
			word->text[out] = text[1];
			word->quote[out++] = SH_QUOTE_ESCAPED;
			text += 2;
			continue;
		}

		/* A double quote starts or ends quoting. */
		if (*text == '"') {
			in_double = !in_double;
			text++;
			continue;
		}

		/* Single quotes quote everything up to the next one. */
		if (*text == '\'' && !in_double) {
			stop = memchr(text + 1, '\'', (size_t)(end - text - 1));
			if (stop == NULL)
				stop = end;
			for (text++; text < stop; text++) {
				word->text[out] = *text;
				word->quote[out++] = SH_QUOTE_SINGLE;
			}

			/* Past the closing quote, when there is one. */
			text = stop;
			if (stop < end)
				text++;
			continue;
		}

		/* An expansion keeps its text as it is. */
		if ((*text == '$' && text + 1 < end &&
		     (text[1] == '(' || text[1] == '{')) || *text == '`') {
			stop = skip_rough(text, end);
			while (text < stop) {
				word->text[out] = *text++;
				word->quote[out++] = mark;
			}

			continue;
		}

		/* An ordinary character. */
		word->text[out] = *text++;
		word->quote[out++] = mark;
	}

	/* The word ends with a null. */
	word->text[out] = '\0';
	word->length = out;

	/* Succeeded: the word. */
	return word;
}

/*
 * Finds roughly where an expansion in a word ends, by matching brackets and
 * stepping over quotations.  Only the text of the word uses this, to copy an
 * expansion whole; expansion itself scans exactly.
 */
static const char *
skip_rough(
	const char *text,
	const char *end)
{
	char open;
	char close;
	int depth;

	/* A backquote ends at the next unescaped backquote. */
	if (*text == '`') {
		for (text++; text < end && *text != '`'; text++) {
			if (*text == '\\' && text + 1 < end)
				text++;
		}

		/* Succeeded: after the closing backquote, or the end without one. */
		if (text >= end)
			return end;
		return text + 1;
	}

	/* ${ and $( end at the bracket that balances theirs. */
	open = text[1];
	close = ')';
	if (open == '{')
		close = '}';
	depth = 0;
	for (text++; text < end; text++) {
		if (*text == '\\' && text + 1 < end) {
			text++;
		} else if (*text == '\'') {
			text = memchr(text + 1, '\'', (size_t)(end - text - 1));
			if (text == NULL)
				return end;
		} else if (*text == open) {
			depth++;
		} else if (*text == close) {
			depth--;
			if (depth == 0)
				return text + 1;
		}
	}

	/* The word ended first. */
	return end;
}

/* Reads the bodies of the here-documents the line just ended owes. */
static void
read_heredocs(
	struct parser *parser)
{
	struct heredoc *heredoc;

	/* Reads them in the order they were written. */
	while (parser->heredocs != NULL) {
		heredoc = parser->heredocs;
		parser->heredocs = heredoc->next;
		read_heredoc(parser, heredoc);
	}
}

/*
 * Reads one here-document's body: the lines up to one that is the delimiter.
 * With an unquoted delimiter a backslash-newline joins lines; with <<- the
 * leading tabs of each line are removed.
 */
static void
read_heredoc(
	struct parser *parser,
	struct heredoc *heredoc)
{
	struct word_buffer body;
	struct word_buffer line;
	struct sh_token *word;
	const char *text;
	size_t index;
	int value;
	int next;
	int at_start;
	int compare;

	/* Reads line by line. */
	memset(&body, 0, sizeof(body));
	for (;;) {
		memset(&line, 0, sizeof(line));
		at_start = 1;
		for (;;) {
			value = sh_input_getc();
			if (value == SH_INPUT_END_OF_ALIAS)
				continue;
			if (value == EOF || value == '\n')
				break;
			if (value == '\t' && at_start && heredoc->strip_tabs)
				continue;
			at_start = 0;
			if (value == '\\' && !heredoc->quoted) {
				next = sh_input_getc();
				if (next == '\n')
					continue;
				buffer_add(&line, value);
				if (next == EOF)
					break;
				buffer_add(&line, next);
				continue;
			}

			/* The character is part of the line. */
			buffer_add(&line, value);
		}

		/* The delimiter line ends the body, and is not part of it. */
		text = "";
		if (line.text != NULL)
			text = line.text;
		compare = strcmp(text, heredoc->delimiter);
		if (compare == 0) {
			free(line.text);
			break;
		}

		/* The end of the input ends the body too. */
		if (value == EOF && line.length == 0) {
			free(line.text);
			break;
		}

		/* Adds the line with its newline. */
		for (index = 0; index < line.length; index++)
			buffer_add(&body, (unsigned char)line.text[index]);
		buffer_add(&body, '\n');
		free(line.text);
		if (value == EOF)
			break;
	}

	/* The body replaces the delimiter as the redirection's word. */
	word = sh_arena_alloc(parser->arena, sizeof(*word));
	if (body.text == NULL)
		word->raw = sh_arena_strndup(parser->arena, "", 0);
	else
		word->raw = sh_arena_strndup(parser->arena, body.text, body.length);
	word->raw_length = body.length;
	word->text = word->raw;
	word->length = body.length;
	word->quote = sh_arena_alloc(parser->arena, body.length + 1U);
	if (heredoc->quoted)
		word->heredoc = SH_HEREDOC_LITERAL;
	else
		word->heredoc = SH_HEREDOC_EXPAND;
	heredoc->redirection->word = word;
	free(body.text);
}

/* Reports whether a word is an assignment: an unquoted name, then =. */
static int
is_assignment(
	const struct sh_token *word)
{
	size_t index;
	char value;

	/* The name comes first, unquoted. */
	for (index = 0; index < word->raw_length; index++) {
		value = word->raw[index];
		if (value == '=')
			return index > 0;
		if (value == '_' || (value >= 'a' && value <= 'z') ||
		    (value >= 'A' && value <= 'Z') ||
		    (index > 0 && value >= '0' && value <= '9'))
			continue;
		return 0;
	}

	/* No = at all. */
	return 0;
}

/* Skips a single quotation: text is at the opening quote. */
static const char *
skip_single(
	const char *text)
{
	const char *end;

	/* The closing quote is the first after the opening one. */
	end = strchr(text + 1, '\'');

	/* No closing quote. */
	if (end == NULL)
		return NULL;

	/* Succeeded: after the closing quote. */
	return end + 1;
}

/* Skips a double quotation: text is at the opening quote. */
static const char *
skip_double(
	const char *text)
{
	/* Steps over escapes and expansions to the closing quote. */
	text++;
	while (*text != '\0' && *text != '"') {
		if (*text == '\\' && text[1] != '\0') {
			text += 2;
			continue;
		}

		/* An expansion inside the quotes is skipped whole. */
		if (*text == '$' || *text == '`') {
			text = skip_expansion(text, 1);
			if (text == NULL)
				return NULL;
			continue;
		}

		/* Any other character. */
		text++;
	}

	/* No closing quote. */
	if (*text != '"')
		return NULL;

	/* Succeeded: after the closing quote. */
	return text + 1;
}

/* Skips a backquoted command substitution: text is at the backquote. */
static const char *
skip_backquote(
	const char *text)
{
	/* A backslash protects the character after it. */
	text++;
	while (*text != '\0' && *text != '`') {
		if (*text == '\\' && text[1] != '\0')
			text++;
		text++;
	}

	/* No closing backquote. */
	if (*text != '`')
		return NULL;

	/* Succeeded: after the closing backquote. */
	return text + 1;
}

/*
 * Skips a braced parameter expansion: text is at the dollar sign.  The word
 * in it may hold quotations and further expansions, so the closing brace is
 * the first one outside all of them.
 */
static const char *
skip_brace(
	const char *text,
	int in_double)
{
	const char *name;

	/* Finds the end of the name, to see whether a pattern follows it. */
	text += 2;
	name = text;
	if (*name == '#')
		name++;
	if (*name == '_' || (*name >= 'a' && *name <= 'z') ||
	    (*name >= 'A' && *name <= 'Z')) {
		while (*name == '_' || (*name >= 'a' && *name <= 'z') ||
		       (*name >= 'A' && *name <= 'Z') ||
		       (*name >= '0' && *name <= '9'))
			name++;
	} else if (*name >= '0' && *name <= '9') {
		while (*name >= '0' && *name <= '9')
			name++;
	} else if (*name == '@' || *name == '*' || *name == '#' ||
		   *name == '?' || *name == '-' || *name == '$' ||
		   *name == '!') {
		name++;
	}

	/* # and % after the name start a pattern, which is not in double quotes. */
	if (name != text && (*name == '#' || *name == '%'))
		in_double = 0;

	/* Steps over what the word holds to the closing brace. */
	while (*text != '\0' && *text != '}') {
		if (*text == '\\' && text[1] != '\0') {
			text += 2;
			continue;
		}

		/* Quotes and nested expansions are skipped whole. */
		if (*text == '\'' && !in_double) {
			text = skip_single(text);
		} else if (*text == '"') {
			text = skip_double(text);
		} else if (*text == '$' || *text == '`') {
			text = skip_expansion(text, in_double);
		} else {
			text++;
			continue;
		}

		/* Something inside did not close. */
		if (text == NULL)
			return NULL;
	}

	/* No closing brace. */
	if (*text != '}')
		return NULL;

	/* Succeeded: after the closing brace. */
	return text + 1;
}

/*
 * Skips an arithmetic expansion $(( ... )): text is at the dollar sign.
 * Returns NULL when the parentheses do not close as "))" together, which is
 * a command substitution that begins with a subshell instead.
 */
static const char *
skip_arithmetic(
	const char *text)
{
	int depth;

	/* Counts parentheses to the )) that closes the expansion. */
	text += 3;
	depth = 0;
	while (*text != '\0') {
		if (*text == '\\' && text[1] != '\0') {
			text += 2;
			continue;
		}

		/* Quotes and nested expansions are skipped whole. */
		if (*text == '\'') {
			text = skip_single(text);
		} else if (*text == '"') {
			text = skip_double(text);
		} else if ((*text == '$' && (text[1] == '(' || text[1] == '{')) ||
			   *text == '`') {
			text = skip_expansion(text, 0);
		} else {
			if (*text == '(') {
				depth++;
			} else if (*text == ')') {
				if (depth == 0 && text[1] != ')')
					return NULL;
				if (depth == 0)
					return text + 2;
				depth--;
			}

			/* Any other character. */
			text++;
			continue;
		}

		/* Something inside did not close. */
		if (text == NULL)
			return NULL;
	}

	/* The text ended inside the expansion. */
	return NULL;
}

/*
 * Skips a command substitution $( ... ): text is at the dollar sign.  The
 * commands are parsed, from a copy of the text pushed as input, and what the
 * parse read up to the closing parenthesis is the substitution.
 */
static const char *
skip_command(
	const char *text)
{
	struct sh_handler handler;
	struct sh_arena *arena;
	struct parser inner;
	const char *end;
	char *captured;
	size_t length;
	int token;
	int kind;

	/* Parses from the text after $(, under a handler of its own. */
	arena = sh_arena_new();
	end = NULL;
	sh_handler_push(&handler);
	kind = setjmp(handler.environment);
	if (kind != 0) {
		sh_handler_unwind(&handler);
		sh_arena_release(arena);
		return NULL;
	}

	/* The text inside the parentheses is parsed on its own. */
	sh_input_push_string(text + 2, strlen(text + 2), 1);
	memset(&inner, 0, sizeof(inner));
	inner.arena = arena;
	inner.substitution = 1;
	sh_input_capture_start();
	token = next_token(&inner, CHECK_NEWLINE | CHECK_KEYWORD);
	push_token(&inner);
	if (token != T_RPAREN)
		(void)parse_list(&inner, LIST_COMPOUND, 1);
	token = next_token(&inner, CHECK_NEWLINE | CHECK_KEYWORD);
	captured = sh_input_capture_stop(&length);
	free(captured);
	if (token == T_RPAREN)
		end = text + 2 + length;
	sh_input_pop();
	sh_handler_pop(&handler);
	sh_arena_release(arena);

	/* Succeeded: after the closing parenthesis, or NULL. */
	return end;
}

/*
 * Skips one expansion: text is at a dollar sign or a backquote.  A dollar
 * sign that begins no expansion is one character.
 */
static const char *
skip_expansion(
	const char *text,
	int in_double)
{
	const char *end;

	/* Dispatches on what follows the dollar sign. */
	if (*text == '`')
		return skip_backquote(text);
	if (text[1] == '{')
		return skip_brace(text, in_double);
	if (text[1] == '(') {
		if (text[2] == '(') {
			end = skip_arithmetic(text);
			if (end != NULL)
				return end;
		}

		/* Otherwise a command substitution. */
		end = skip_command(text);
		return end;
	}

	/* Succeeded: the lone dollar sign. */
	return text + 1;
}
