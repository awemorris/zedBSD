/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD find userland command.
 *
 * GNU's extensions (ws045): -maxdepth, -mindepth, -regextype, -iname,
 * -ipath, -wholename, -iwholename, -regex, -iregex, -empty, -executable,
 * -readable, -writable, -false, -amin, -cmin, -mmin, -delete, -printf,
 * -quit, and the operators -and and -or.
 *
 * ws001-p042: -exec ... {} + gathers the pathnames into as few
 * invocations as the argument limit allows, -perm takes a symbolic mode,
 * -L and -H give a dangling link's own information, and a directory that
 * would start a loop is diagnosed without being tested.
 */

#include <dirent.h>
#include <errno.h>
#include "include/libc/fnmatch.h"
#include "userland/base/chmod/mode.h"
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum node_kind {
	NODE_AND,
	NODE_OR,
	NODE_NOT,
	NODE_TRUE,
	NODE_NAME,
	NODE_PATH,
	NODE_TYPE,
	NODE_PERM,
	NODE_USER,
	NODE_GROUP,
	NODE_NOUSER,
	NODE_NOGROUP,
	NODE_LINKS,
	NODE_SIZE,
	NODE_ATIME,
	NODE_CTIME,
	NODE_MTIME,
	NODE_NEWER,
	NODE_PRINT,
	NODE_PRINT0,
	NODE_FPRINT0,
	NODE_PRUNE,
	NODE_EXEC,
	NODE_FALSE,
	NODE_INAME,
	NODE_IPATH,
	NODE_REGEX,
	NODE_IREGEX,
	NODE_EMPTY,
	NODE_EXECUTABLE,
	NODE_READABLE,
	NODE_WRITABLE,
	NODE_AMIN,
	NODE_CMIN,
	NODE_MMIN,
	NODE_DELETE,
	NODE_PRINTF,
	NODE_QUIT,
};

struct number {
	unsigned long long value;
	int comparison;
};

struct node {
	enum node_kind kind;
	struct node *left;
	struct node *right;
	char *text;
	char **arguments;
	int argument_count;
	struct number number;
	mode_t mode;
	uid_t uid;
	gid_t gid;
	char type;
	struct stat reference;
	/* An explicit file-output action owns this stream until finalization. */
	FILE *output;
	/* -regex and -iregex own their compiled regex. */
	regex_t *regex;
	/* -exec ... {} +: the pathnames gathered, their count and their bytes (with a pointer each). */
	char **batch;
	int batch_count;
	size_t batch_bytes;
};

struct parser {
	int argc;
	char **argv;
	int index;
	int failed;
	int depth_first;
	int same_device;
	/* GNU's -maxdepth (-1 for none), -mindepth and -regextype. */
	long max_depth;
	long min_depth;
	int regex_extended;
};

/* The bytes POSIX keeps free of {ARG_MAX} for the utility's own use. */
#define FIND_ARG_RESERVE 2048U

struct walk_state {
	dev_t ancestors_dev[128];
	ino_t ancestors_ino[128];
	unsigned depth;
	dev_t root_device;
	int have_root_device;
	int follow;
	int follow_root;
	int depth_first;
	int same_device;
	int prune;
	int errors;
	time_t now;
	/* GNU's depth limits, -quit, and the starting point being walked (%P). */
	long max_depth;
	long min_depth;
	int quit;
	const char *start;
};

/*
 * One of GNU's primaries: its name, the node it makes (NODE_TRUE for an
 * option of the walk), and whether an operand follows it.
 */
struct gnu_primary {
	const char *name;
	enum node_kind kind;
	int takes_operand;
};

/*
 * GNU's primaries this find knows.
 *
 * The table is read by parse_gnu_primary only.  -wholename and
 * -iwholename are GNU's older names of -path and -ipath.
 */
static const struct gnu_primary gnu_primaries[] = {
	{"-maxdepth", NODE_TRUE, 1},
	{"-mindepth", NODE_TRUE, 1},
	{"-regextype", NODE_TRUE, 1},
	{"-iname", NODE_INAME, 1},
	{"-ipath", NODE_IPATH, 1},
	{"-wholename", NODE_PATH, 1},
	{"-iwholename", NODE_IPATH, 1},
	{"-regex", NODE_REGEX, 1},
	{"-iregex", NODE_IREGEX, 1},
	{"-empty", NODE_EMPTY, 0},
	{"-executable", NODE_EXECUTABLE, 0},
	{"-readable", NODE_READABLE, 0},
	{"-writable", NODE_WRITABLE, 0},
	{"-false", NODE_FALSE, 0},
	{"-amin", NODE_AMIN, 1},
	{"-cmin", NODE_CMIN, 1},
	{"-mmin", NODE_MMIN, 1},
	{"-delete", NODE_DELETE, 0},
	{"-printf", NODE_PRINTF, 1},
	{"-quit", NODE_QUIT, 0},
	{NULL, NODE_TRUE, 0}
};

static int is_expression(const char *text);
static struct node *new_node(enum node_kind kind);
static struct node *parse_or(struct parser *parser);
static struct node *parse_and(struct parser *parser);
static struct node *parse_not(struct parser *parser);
static struct node *parse_primary(struct parser *parser);
static char *take_operand(struct parser *parser, const char *option);
static int parse_number(const char *text, struct number *number);
static void usage(void);
static void free_expression(struct node *node);
static int expression_has_action(const struct node *node);
static int finish_outputs(struct node *node);
static void scan_walk_options(struct node *node, struct walk_state *state);
static int walk_path(const char *path, struct node *expression, struct walk_state *state);
static int evaluate(struct node *node, const char *path, const char *name, const struct stat *status, struct walk_state *state);
static int file_type(mode_t mode, char type);
static int number_matches(const struct number *number, unsigned long long value);
static int run_command(const struct node *node, const char *path);
static char *replace_braces(const char *argument, const char *path);
static int run_vector(char **arguments);
static void batch_add(struct node *node, const char *path, struct walk_state *state);
static void batch_run(struct node *node, struct walk_state *state);
static void batch_finish(struct node *node, struct walk_state *state);
static size_t batch_room(const struct node *node);
static int parse_perm(const char *text, struct node *node);
static int stat_file(const char *path, int follow, struct stat *status);
static struct node *parse_gnu_primary(struct parser *parser, const char *token, int *handled);
static void parse_gnu_option(struct parser *parser, const char *token, const char *operand);
static int compile_find_regex(struct parser *parser, struct node *node);
static int evaluate_gnu(struct node *node, const char *path, const char *name, const struct stat *status, struct walk_state *state);
static int file_empty(const char *path, const struct stat *status);
static int delete_file(const char *path, const struct stat *status, struct walk_state *state);
static void print_format(const char *format, const char *path, const char *name, const struct stat *status, struct walk_state *state);
static void print_escape(char letter);
static void print_directive(char letter, char next, const char *path, const char *name, const struct stat *status, struct walk_state *state);
static void print_time(char letter, char next, const struct stat *status);
static char type_letter(mode_t mode);
static void mode_text(mode_t mode, char *text);
static int is_operator(const struct parser *parser, const char *letter_form, const char *word_form);

/*
 * Runs the find command.
 */
int
main(
	int argc,
	char **argv)
{
	struct node *print;
	struct node *both;
	struct parser parser = {.argc = argc, .argv = argv, .index = 1};
	struct walk_state state = {0};
	struct node *expression;
	int path_begin;
	int path_end;
	int index;
	int has_action;
	int output_error;

	has_action = 0;
	parser.max_depth = -1;

	/* Process each remaining command-line operand. */
	while (parser.index < argc && (strcmp(argv[parser.index], "-H") == 0 ||
				       strcmp(argv[parser.index], "-L") == 0)) {
		state.follow = strcmp(argv[parser.index], "-L") == 0;
		state.follow_root = strcmp(argv[parser.index], "-H") == 0;
		parser.index++;
	}

	/* Process each remaining command-line operand. */
	path_begin = parser.index;
	while (parser.index < argc && !is_expression(argv[parser.index]))
		parser.index++;
	path_end = parser.index;

	/* Handles the path begin condition. */
	if (path_begin == path_end)
		path_begin = -1;
	/* Implicit printing is appended once, after inspecting the parsed tree. */
	if (parser.index == argc)
		expression = new_node(NODE_TRUE);
	else
		expression = parse_or(&parser);

	/* Validates the command-line arguments. */
	if (expression == NULL || parser.failed || parser.index != argc) {
		usage();
		free_expression(expression);

		/* Reports operation failure. */
		return 2;
	}

	/* Process each remaining command-line operand. */
	state.depth_first = parser.depth_first;
	state.same_device = parser.same_device;
	state.max_depth = parser.max_depth;
	state.min_depth = parser.min_depth;
	has_action = expression_has_action(expression);

	/* Handles the action condition. */
	if (!has_action) {
		print = new_node(NODE_PRINT);
		both = new_node(NODE_AND);

		/* Handles the print availability. */
		if (print == NULL || both == NULL) {
			free(print);
			free(both);
			free_expression(expression);

			/* Reports operation failure. */
			return 1;
		}
		both->left = expression;
		both->right = print;
		expression = both;
	}
	state.now = time(NULL);
	scan_walk_options(expression, &state);

	/* Handles the path begin condition. */
	if (path_begin < 0) {
		state.start = ".";
		(void)walk_path(".", expression, &state);
	} else {
		/* Each starting point, until -quit. */
		for (index = path_begin; index < path_end && !state.quit;
		     index++) {
			state.have_root_device = 0;
			state.start = argv[index];
			(void)walk_path(argv[index], expression, &state);
		}
	}

	/* The pathnames -exec ... {} + still holds. */
	batch_finish(expression, &state);

	/* Buffered manifest failures must be observed before declaring success. */
	output_error = finish_outputs(expression);
	if (output_error != 0)
		state.errors = 1;

	free_expression(expression);
	output_error = fclose(stdout);
	if (output_error != 0) {
		fprintf(stderr, "find: write error\n");
		state.errors = 1;
	}

	/* Refuses to report an incomplete enumeration as successful. */
	if (state.errors)
		return 1;

	/* Succeeded: traversal and all output streams completed. */
	return 0;
}

/* Supports the is expression operation. */
static int
is_expression(
	const char *text)
{
	int function_result;

	/* Computes the function result. */
	function_result = text[0] == '-' || strcmp(text, "!") == 0 ||
	       strcmp(text, "(") == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the new node operation. */
static struct node *
new_node(
	enum node_kind kind)
{
	struct node *node;

	node = calloc(1, sizeof(*node));

	/* Handles the node availability. */
	if (node != NULL)
		node->kind = kind;

	/* Returns the computed result. */
	return node;
}

/* Parses the expressions joined by -o (or -or). */
static struct node *
parse_or(
	struct parser *parser)
{
	struct node *parent;
	struct node *left;
	int joined;

	/* The first operand. */
	left = parse_and(parser);

	/* Each further operand after an -o. */
	for (;;) {
		/* Whether an -o follows. */
		joined = !parser->failed && is_operator(parser, "-o", "-or");
		if (!joined)
			break;
		parser->index++;

		/* The node that joins the two. */
		parent = new_node(NODE_OR);
		if (parent == NULL)
			return NULL;
		parent->left = left;
		parent->right = parse_and(parser);
		left = parent;
	}

	/* Returns the computed result. */
	return left;
}

/* Parses the expressions joined by -a (or -and), or by nothing. */
static struct node *
parse_and(
	struct parser *parser)
{
	struct node *parent;
	struct node *left;
	int stops;
	int joined;

	/* The first operand. */
	left = parse_not(parser);

	/* Each further operand, until the end, a ")" or an -o. */
	for (;;) {
		/* Whether the expression ends here. */
		stops = parser->failed || parser->index >= parser->argc ||
		    is_operator(parser, ")", ")") || is_operator(parser, "-o", "-or");
		if (stops)
			break;

		/* An explicit -a is skipped; juxtaposition means the same. */
		joined = is_operator(parser, "-a", "-and");
		if (joined)
			parser->index++;

		/* The node that joins the two. */
		parent = new_node(NODE_AND);
		if (parent == NULL)
			return NULL;
		parent->left = left;
		parent->right = parse_not(parser);
		left = parent;
	}

	/* Returns the computed result. */
	return left;
}

/* Supports the parse not operation. */
static struct node *
parse_not(
	struct parser *parser)
{
	struct node *function_result;
	struct node *node;

	/* Handles the selected command-line operation. */
	if (parser->index < parser->argc &&
	    (strcmp(parser->argv[parser->index], "!") == 0 ||
	     strcmp(parser->argv[parser->index], "-not") == 0)) {
		parser->index++;
		node = new_node(NODE_NOT);

		/* Handles the node availability. */
		if (node != NULL)
			node->left = parse_not(parser);

		/* Returns the computed result. */
		return node;
	}

	/* Obtains the parse primary result. */
	function_result = parse_primary(parser);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the parse primary operation. */
static struct node *
parse_primary(
	struct parser *parser)
{
	struct node *function_result;
	char *value_local;
	char *value_local1;
	char *end_local2;
	char *value_local3;
	char *type;
	struct passwd *account;
	struct group *group;
	unsigned long id;
	size_t length;
	char *path;
	int begin;
	int prompt;
	int handled;
	int error;
	int batched;
	int compared;
	int plus;
	int braces;
	struct node *node;
	char *token;

	/* Validates the command-line arguments. */
	if (parser->index >= parser->argc)
		return NULL;
	token = parser->argv[parser->index++];

	/* One of GNU's primaries. */
	node = parse_gnu_primary(parser, token, &handled);
	if (handled)
		return node;

	/* Selects the matching value. */
	if (strcmp(token, "(") == 0) {
		node = parse_or(parser);

		/* Handles the selected command-line operation. */
		if (parser->index >= parser->argc ||
		    strcmp(parser->argv[parser->index++], ")") != 0) {
			fprintf(stderr, "find: missing ')'\n");
			parser->failed = 1;
		}

		/* Returns the computed result. */
		return node;
	}

	/* Selects the matching value. */
	if (strcmp(token, "-true") == 0) {
		/* Obtains the new node result. */
		function_result = new_node(NODE_TRUE);

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (strcmp(token, "-depth") == 0) {
		parser->depth_first = 1;

		/* Obtains the new node result. */
		function_result = new_node(NODE_TRUE);

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (strcmp(token, "-xdev") == 0) {
		parser->same_device = 1;

		/* Obtains the new node result. */
		function_result = new_node(NODE_TRUE);

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (strcmp(token, "-print") == 0) {
		/* Obtains the new node result. */
		function_result = new_node(NODE_PRINT);

		/* Returns the computed result. */
		return function_result;
	}

	/* NUL terminators preserve every legal filename byte in the manifest. */
	if (strcmp(token, "-print0") == 0) {
		node = new_node(NODE_PRINT0);

		/* Succeeded: the action writes filename-safe stdout records. */
		return node;
	}

	/* A file action bypasses terminal output transformations. */
	if (strcmp(token, "-fprint0") == 0) {
		path = take_operand(parser, token);
		if (path == NULL)
			return NULL;

		node = new_node(NODE_FPRINT0);
		if (node == NULL)
			return NULL;

		node->text = path;
		node->output = fopen(path, "wb");
		if (node->output == NULL) {
			fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
			free(node);
			parser->failed = 1;
			return NULL;
		}

		/* Succeeded: the expression now owns the output stream. */
		return node;
	}

	/* Selects the matching value. */
	if (strcmp(token, "-prune") == 0) {
		/* Obtains the new node result. */
		function_result = new_node(NODE_PRUNE);

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (strcmp(token, "-name") == 0 || strcmp(token, "-path") == 0) {
		node = new_node(strcmp(token, "-name") == 0 ? NODE_NAME
							    : NODE_PATH);

		/* Handles the node availability. */
		if (node != NULL)
			node->text = take_operand(parser, token);

		/* Returns the computed result. */
		return node;
	}

	/* Selects the matching value. */
	if (strcmp(token, "-type") == 0) {
		node = new_node(NODE_TYPE);
		type = take_operand(parser, token);

		/* Handles a failed strchr operation. */
		if (node != NULL && type != NULL && type[0] != '\0' &&
		    type[1] == '\0' && strchr("bcdflps", type[0]) != NULL)
			node->type = type[0];
		else
			parser->failed = 1;

		/* Returns the computed result. */
		return node;
	}

	/* Selects the matching value. */
	if (strcmp(token, "-perm") == 0) {
		value_local = take_operand(parser, token);

		node = new_node(NODE_PERM);

		/* Handles the value local availability. */
		if (value_local == NULL || node == NULL)
			return node;
		error = parse_perm(value_local, node);
		if (error != 0) {
			fprintf(stderr, "find: -perm: invalid mode: %s\n", value_local);
			parser->failed = 1;
		}

		/* Returns the computed result. */
		return node;
	}

	/* Selects the matching value. */
	if (strcmp(token, "-user") == 0 || strcmp(token, "-group") == 0) {
		value_local1 = take_operand(parser, token);

		node = new_node(strcmp(token, "-user") == 0 ? NODE_USER
							    : NODE_GROUP);

		/* Handles the value local1 availability. */
		if (value_local1 == NULL || node == NULL)
			return node;
		errno = 0;
		id = strtoul(value_local1, &end_local2, 10);

		/* Handles the reported system error. */
		if (errno == 0 && *value_local1 != '\0' && *end_local2 == '\0') {
			/* Handles the node condition. */
			if (node->kind == NODE_USER)
				node->uid = (uid_t)id;
			else
				node->gid = (gid_t)id;
		} else if (node->kind == NODE_USER) {
			account = getpwnam(value_local1);

			/* Handles the account availability. */
			if (account == NULL)
				parser->failed = 1;
			else
				node->uid = account->pw_uid;
		} else {
			group = getgrnam(value_local1);

			/* Handles the group availability. */
			if (group == NULL)
				parser->failed = 1;
			else
				node->gid = group->gr_gid;
		}

		/* Returns the computed result. */
		return node;
	}

	/* Selects the matching value. */
	if (strcmp(token, "-nouser") == 0) {
		/* Obtains the new node result. */
		function_result = new_node(NODE_NOUSER);

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (strcmp(token, "-nogroup") == 0) {
		/* Obtains the new node result. */
		function_result = new_node(NODE_NOGROUP);

		/* Returns the computed result. */
		return function_result;
	}

	/* Selects the matching value. */
	if (strcmp(token, "-links") == 0 || strcmp(token, "-size") == 0 ||
	    strcmp(token, "-atime") == 0 || strcmp(token, "-ctime") == 0 ||
	    strcmp(token, "-mtime") == 0) {
		value_local3 = take_operand(parser, token);

		/* Selects the matching value. */
		if (strcmp(token, "-links") == 0)
			node = new_node(NODE_LINKS);
		else if (strcmp(token, "-size") == 0)
			node = new_node(NODE_SIZE);
		else if (strcmp(token, "-atime") == 0)
			node = new_node(NODE_ATIME);
		else if (strcmp(token, "-ctime") == 0)
			node = new_node(NODE_CTIME);
		else
			node = new_node(NODE_MTIME);

		/* Handles the node availability. */
		if (node == NULL || value_local3 == NULL)
			return node;
		length = strlen(value_local3);

		/* Handles the node condition. */
		if (node->kind == NODE_SIZE && length != 0 &&
		    value_local3[length - 1U] == 'c') {
			node->type = 'c';
			value_local3[length - 1U] = '\0';
		}

		/* Handles a failed parse number operation. */
		if (!parse_number(value_local3, &node->number))
			parser->failed = 1;

		/* Returns the computed result. */
		return node;
	}

	/* Selects the matching value. */
	if (strcmp(token, "-newer") == 0) {
		path = take_operand(parser, token);

		node = new_node(NODE_NEWER);

		/* Handles a failed stat operation. */
		if (node != NULL &&
		    (path == NULL || stat(path, &node->reference) != 0)) {
			fprintf(stderr, "find: %s: %s\n",
				path != NULL ? path : token, strerror(errno));
			parser->failed = 1;
		}

		/* Returns the computed result. */
		return node;
	}

	/* Selects the matching value. */
	if (strcmp(token, "-exec") == 0 || strcmp(token, "-ok") == 0) {
		begin = parser->index;
		prompt = strcmp(token, "-ok") == 0;

		/* Up to ";", or for -exec a "+" right after a "{}" of its own. */
		batched = 0;
		while (parser->index < parser->argc) {
			/* ";" ends either. */
			compared = strcmp(parser->argv[parser->index], ";");
			if (compared == 0)
				break;

			/* "+" ends -exec when "{}" is just before it (and the utility before that). */
			plus = strcmp(parser->argv[parser->index], "+");
			braces = strcmp(parser->argv[parser->index - 1], "{}");
			if (!prompt && parser->index > begin + 1 && plus == 0 && braces == 0) {
				batched = 1;
				break;
			}

			/* An argument. */
			parser->index++;
		}

		/* Validates the command-line arguments. */
		if (parser->index == begin || parser->index == parser->argc) {
			fprintf(stderr, "find: %s: missing terminator\n",
				token);
			parser->failed = 1;

			/* Reports that no result is available. */
			return NULL;
		}
		node = new_node(NODE_EXEC);

		/* The utility and its arguments; with "+" the last, "{}", is where the pathnames go. */
		if (node != NULL) {
			node->arguments = &parser->argv[begin];
			node->argument_count = parser->index - begin;
			node->type = 'e';
			if (prompt)
				node->type = 'o';
			if (batched)
				node->type = '+';
		}
		parser->index++;

		/* Returns the computed result. */
		return node;
	}
	fprintf(stderr, "find: unknown expression primary: %s\n", token);
	parser->failed = 1;

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the take operand operation. */
static char *
take_operand(
	struct parser *parser,
	const char *option)
{
	/* Validates the command-line arguments. */
	if (parser->index >= parser->argc) {
		fprintf(stderr, "find: %s: missing operand\n", option);
		parser->failed = 1;

		/* Reports that no result is available. */
		return NULL;
	}

	/* Returns the computed result. */
	return parser->argv[parser->index++];
}

/* Supports the parse number operation. */
static int
parse_number(
	const char *text,
	struct number *number)
{
	char *end;

	number->comparison = 0;

	/* Validates the current text. */
	if (*text == '+' || *text == '-') {
		number->comparison = *text == '+' ? 1 : -1;
		text++;
	}

	/* Validates the current text. */
	if (*text == '\0')
		return 0;
	errno = 0;
	number->value = strtoull(text, &end, 10);

	/* Returns the computed result. */
	return errno == 0 && *end == '\0';
}

/* Supports the usage operation. */
static void
usage(
	void)
{
	fprintf(stderr, "usage: find [-H|-L] [path ...] [expression]\n");
}

/* Supports the free expression operation. */
static void
free_expression(
	struct node *node)
{
	int index;

	/* Handles the node availability. */
	if (node == NULL)
		return;
	free_expression(node->left);
	free_expression(node->right);

	/* Parse failures also release streams opened by earlier actions. */
	if (node->output != NULL)
		(void)fclose(node->output);

	/* The regex of -regex. */
	if (node->regex != NULL) {
		regfree(node->regex);
		free(node->regex);
	}

	/* The pathnames of -exec ... {} + not run (after a failure). */
	for (index = 0; index < node->batch_count; index++)
		free(node->batch[index]);
	free(node->batch);

	free(node);
}

/* Recognizes actions in syntax nodes rather than coincidental operand text. */
static int
expression_has_action(
	const struct node *node)
{
	int found;

	/* Empty children contain no action. */
	if (node == NULL)
		return 0;

	/* These explicit actions suppress the default print operation. */
	switch (node->kind) {
	case NODE_PRINT:
	case NODE_PRINT0:
	case NODE_FPRINT0:
	case NODE_EXEC:
	case NODE_DELETE:
	case NODE_PRINTF:
		return 1;
	default:
		break;
	}

	/* Search both expression branches without interpreting their operands. */
	found = expression_has_action(node->left);
	if (found)
		return 1;

	found = expression_has_action(node->right);

	/* Succeeded: reports whether any explicit action is present. */
	return found;
}

/* Flushes and closes every owned manifest even if another close has failed. */
static int
finish_outputs(
	struct node *node)
{
	int failed;
	int status;

	/* Empty children own no streams. */
	if (node == NULL)
		return 0;

	/* Visit both branches, including actions skipped by short circuiting. */
	failed = finish_outputs(node->left);
	status = finish_outputs(node->right);
	if (status != 0)
		failed = 1;

	/* Clear ownership after close, including its error path. */
	if (node->output != NULL) {
		status = fclose(node->output);
		node->output = NULL;
		if (status != 0) {
			fprintf(stderr, "find: %s: write error\n", node->text);
			failed = 1;
		}
	}

	/* A buffered failure invalidates the complete manifest. */
	if (failed)
		return 1;

	/* Succeeded: all owned output streams have closed. */
	return 0;
}

/* Supports the scan walk options operation. */
static void
scan_walk_options(
	struct node *node,
	struct walk_state *state)
{
	/* Handles the node availability. */
	if (node == NULL)
		return;

	/* Handles the node condition. */
	if (node->kind == NODE_PRUNE)
		return;
	scan_walk_options(node->left, state);
	scan_walk_options(node->right, state);
}

/* Supports the walk path operation. */
static int
walk_path(
	const char *path,
	struct node *expression,
	struct walk_state *state)
{
	char child[PATH_MAX + 1U];
	int length;
	DIR *stream;
	struct dirent *entry;
	unsigned ancestor;
	struct stat status;
	const char *name;
	const char *format;
	size_t path_length;
	int directory;
	int in_depth;
	int result;
	int follow;
	int error;

	/* -quit ends the walk. */
	if (state->quit)
		return 1;

	name = strrchr(path, '/');
	result = 1;

	name = name != NULL && name[1] != '\0' ? name + 1 : path;

	/* The file's information: what a link names under -L (and -H for an operand). */
	follow = state->follow || (state->follow_root && state->depth == 0);
	error = stat_file(path, follow, &status);
	if (error != 0) {
		fprintf(stderr, "find: %s: %s\n", path, strerror(error));
		state->errors = 1;

		/* Reports successful completion. */
		return 0;
	}
	directory = S_ISDIR(status.st_mode);
	path_length = strlen(path);

	/* A directory that is one of its own ancestors (a loop through a link) is diagnosed, not tested. */
	for (ancestor = 0; directory && ancestor < state->depth; ancestor++) {
		if (state->ancestors_dev[ancestor] == status.st_dev && state->ancestors_ino[ancestor] == status.st_ino) {
			fprintf(stderr, "find: %s: directory cycle\n", path);
			state->errors = 1;
			return 0;
		}
	}

	/* Handles the state condition. */
	if (!state->have_root_device) {
		state->root_device = status.st_dev;
		state->have_root_device = 1;
	}
	state->prune = 0;

	/* GNU's -mindepth: files above it are walked through, not tested. */
	in_depth = 1;
	if ((long)state->depth < state->min_depth)
		in_depth = 0;

	/* GNU's -maxdepth: a directory at it is not gone into. */
	if (state->max_depth >= 0 && (long)state->depth >= state->max_depth)
		directory = 0;

	/* Handles the state condition. */
	if (!state->depth_first && in_depth)
		result = evaluate(expression, path, name, &status, state);

	/* -quit stops before going into the directory. */
	if (state->quit)
		directory = 0;

	/* Handles the directory condition. */
	if (directory && !state->prune &&
	    (!state->same_device || status.st_dev == state->root_device)) {
		/* Handles the state condition. */
		if (state->depth == sizeof(state->ancestors_dev) /
					sizeof(state->ancestors_dev[0])) {
			fprintf(stderr, "find: %s: nesting limit exceeded\n",
				path);
			state->errors = 1;

			/* Reports successful completion. */
			return 0;
		}
		state->ancestors_dev[state->depth] = status.st_dev;
		state->ancestors_ino[state->depth++] = status.st_ino;
		stream = opendir(path);

		/* Handles the stream availability. */
		if (stream == NULL) {
			fprintf(stderr, "find: %s: %s\n", path,
				strerror(errno));
			state->errors = 1;
		} else {
			/* Distinguish a complete directory from a failed enumeration. */
			while (1) {
				errno = 0;
				entry = readdir(stream);
				if (entry == NULL) {
					if (errno != 0) {
						fprintf(stderr, "find: %s: %s\n", path,
							strerror(errno));
						state->errors = 1;
					}
					break;
				}

				/* Selects the matching value. */
				if (strcmp(entry->d_name, ".") == 0 ||
				    strcmp(entry->d_name, "..") == 0)
					continue;

				/* The entry's pathname, with no second slash after one the operand ends with. */
				format = "%s/%s";
				if (path[path_length - 1U] == '/')
					format = "%s%s";
				length = snprintf(child, sizeof(child), format, path, entry->d_name);

				/* Checks the current data length. */
				if (length < 0 ||
				    (size_t)length >= sizeof(child)) {
					fprintf(stderr,
						"find: path too long: %s/%s\n",
						path, entry->d_name);
					state->errors = 1;
					continue;
				}
				(void)walk_path(child, expression, state);

				/* -quit leaves the directory at once. */
				if (state->quit)
					break;
			}

			/* Handles a failed closedir operation. */
			if (closedir(stream) != 0)
				state->errors = 1;
		}
		state->depth--;
	}

	/* Handles the state condition. */
	if (state->depth_first && in_depth && !state->quit)
		result = evaluate(expression, path, name, &status, state);

	/* Handles an operation failure. */
	if (!result && ferror(stdout))
		state->errors = 1;

	/* Returns the computed result. */
	return result;
}

/* Supports the evaluate operation. */
static int
evaluate(
	struct node *node,
	const char *path,
	const char *name,
	const struct stat *status,
	struct walk_state *state)
{
	int function_result;
	int gnu_answer;
	FILE *output;
	size_t length;
	size_t written;
	int terminator;
	time_t stamp;
	unsigned long long value;

	/* Handles the node availability. */
	if (node == NULL)
		return 1;

	/* Dispatch the selected syntax or record type. */
	switch (node->kind) {
	case NODE_AND:
		/* Computes the function result. */
		function_result = evaluate(node->left, path, name, status, state) &&
		       evaluate(node->right, path, name, status, state);

		/* Returns the computed result. */
		return function_result;
	case NODE_OR:
		/* Computes the function result. */
		function_result = evaluate(node->left, path, name, status, state) ||
		       evaluate(node->right, path, name, status, state);

		/* Returns the computed result. */
		return function_result;
	case NODE_NOT:
		/* Computes the function result. */
		function_result = !evaluate(node->left, path, name, status, state);

		/* Returns the computed result. */
		return function_result;
	case NODE_TRUE:
		/* Reports operation failure. */
		return 1;
	case NODE_NAME:
		/* Computes the function result. */
		function_result = fnmatch(node->text, name, 0) == 0;

		/* Returns the computed result. */
		return function_result;
	case NODE_PATH:
		/* Computes the function result. */
		function_result = fnmatch(node->text, path, 0) == 0;

		/* Returns the computed result. */
		return function_result;
	case NODE_TYPE:
		/* Obtains the file type result. */
		function_result = file_type(status->st_mode, node->type);

		/* Returns the computed result. */
		return function_result;
	case NODE_PERM:
		/* Returns the computed result. */
		return node->number.comparison < 0
			   ? (status->st_mode & node->mode) == node->mode
			   : (status->st_mode & 07777) == node->mode;
	case NODE_USER:
		/* Returns the computed result. */
		return status->st_uid == node->uid;
	case NODE_GROUP:
		/* Returns the computed result. */
		return status->st_gid == node->gid;
	case NODE_NOUSER:
		/* Computes the function result. */
		function_result = getpwuid(status->st_uid) == NULL;

		/* Returns the computed result. */
		return function_result;
	case NODE_NOGROUP:
		/* Computes the function result. */
		function_result = getgrgid(status->st_gid) == NULL;

		/* Returns the computed result. */
		return function_result;
	case NODE_LINKS:
		/* Obtains the number matches result. */
		function_result = number_matches(&node->number, status->st_nlink);

		/* Returns the computed result. */
		return function_result;
	case NODE_SIZE:
		value = status->st_size < 0
			    ? 0
			    : (unsigned long long)status->st_size;

		/* Handles the node condition. */
		if (node->type != 'c')
			value = (value + 511U) / 512U;

		/* Obtains the number matches result. */
		function_result = number_matches(&node->number, value);

		/* Returns the computed result. */
		return function_result;
	case NODE_ATIME:
	case NODE_CTIME:
	case NODE_MTIME:
				stamp = node->kind == NODE_ATIME	  ? status->st_atime
		       : node->kind == NODE_CTIME ? status->st_ctime
						  : status->st_mtime;

	value = stamp > state->now
		    ? 0
		    : (unsigned long long)(state->now - stamp) / 86400U;

	/* Obtains the number matches result. */
	function_result = number_matches(&node->number, value);

	/* Returns the computed result. */
	return function_result;
	case NODE_NEWER:
		/* Returns the computed result. */
		return status->st_mtime > node->reference.st_mtime;
	case NODE_PRINT:
	case NODE_PRINT0:
	case NODE_FPRINT0:
		/* Explicit streams bypass PTYs; every action detects its own failure. */
		output = stdout;
		if (node->kind == NODE_FPRINT0)
			output = node->output;

		terminator = '\0';
		if (node->kind == NODE_PRINT)
			terminator = '\n';

		length = strlen(path);
		written = fwrite(path, 1, length, output);
		if (written != length) {
			state->errors = 1;
			return 0;
		}

		function_result = fputc(terminator, output);
		if (function_result == EOF) {
			state->errors = 1;
			return 0;
		}

		/* Succeeded: the record was written; final close checks buffering. */
		return 1;
	case NODE_PRUNE:
		state->prune = 1;

		/* Reports operation failure. */
		return 1;
	case NODE_EXEC:
		/* With "+" the pathname is gathered and the primary is true. */
		if (node->type == '+') {
			batch_add(node, path, state);
			return 1;
		}

		/* Obtains the run command result. */
		function_result = run_command(node, path);

		/* Returns the computed result. */
		return function_result;
	case NODE_FALSE:
	case NODE_INAME:
	case NODE_IPATH:
	case NODE_REGEX:
	case NODE_IREGEX:
	case NODE_EMPTY:
	case NODE_EXECUTABLE:
	case NODE_READABLE:
	case NODE_WRITABLE:
	case NODE_AMIN:
	case NODE_CMIN:
	case NODE_MMIN:
	case NODE_DELETE:
	case NODE_PRINTF:
	case NODE_QUIT:
		/* One of GNU's tests and actions. */
		gnu_answer = evaluate_gnu(node, path, name, status, state);
		return gnu_answer;
	default:
		/* Reports successful completion. */
		return 0;
	}
}

/*
 * Parses one of GNU's primaries: the depth options (-maxdepth, -mindepth,
 * -regextype), the tests (-iname, -ipath, -wholename, -iwholename, -regex,
 * -iregex, -empty, -executable, -readable, -writable, -false, -amin,
 * -cmin, -mmin) and the actions (-delete, -printf, -quit).  Sets handled
 * when the token is one of them; the node is NULL after a message then.
 */
static struct node *
parse_gnu_primary(
	struct parser *parser,
	const char *token,
	int *handled)
{
	const struct gnu_primary *primary;
	struct node *node;
	char *operand;
	size_t index;
	int differs;
	int valid;

	/* The primary of the token. */
	*handled = 0;
	primary = NULL;
	for (index = 0; gnu_primaries[index].name != NULL; index++) {
		differs = strcmp(token, gnu_primaries[index].name);
		if (differs == 0) {
			primary = &gnu_primaries[index];
			break;
		}
	}

	/* Not one of GNU's. */
	if (primary == NULL)
		return NULL;
	*handled = 1;

	/* The operand, for the primaries that take one. */
	operand = NULL;
	if (primary->takes_operand) {
		operand = take_operand(parser, token);
		if (operand == NULL)
			return NULL;
	}

	/* The options of the walk, which test nothing. */
	if (primary->kind == NODE_TRUE) {
		parse_gnu_option(parser, token, operand);
		node = new_node(NODE_TRUE);
		return node;
	}

	/* The node. */
	node = new_node(primary->kind);
	if (node == NULL) {
		parser->failed = 1;
		return NULL;
	}

	/* The operand is the node's text (a pattern, a regex or a format). */
	node->text = operand;

	/* The minutes of -amin, -cmin and -mmin. */
	if (primary->kind == NODE_AMIN || primary->kind == NODE_CMIN ||
	    primary->kind == NODE_MMIN) {
		valid = parse_number(operand, &node->number);
		if (!valid) {
			fprintf(stderr, "find: %s: invalid number\n", operand);
			parser->failed = 1;
		}

		/* The test, with its number. */
		return node;
	}

	/* A regex, compiled once. */
	if (primary->kind == NODE_REGEX || primary->kind == NODE_IREGEX) {
		differs = compile_find_regex(parser, node);
		if (differs)
			parser->failed = 1;
		return node;
	}

	/* -delete walks depth first, so that a directory is emptied first. */
	if (primary->kind == NODE_DELETE)
		parser->depth_first = 1;

	/* Succeeded. */
	return node;
}

/* Applies -maxdepth, -mindepth or -regextype to the parser. */
static void
parse_gnu_option(
	struct parser *parser,
	const char *token,
	const char *operand)
{
	struct number number;
	int valid;
	int differs;

	/* -regextype: posix-extended and egrep are ERE; the rest BRE. */
	differs = strcmp(token, "-regextype");
	if (differs == 0) {
		parser->regex_extended = 0;
		differs = strcmp(operand, "posix-extended");
		if (differs != 0)
			differs = strcmp(operand, "egrep");
		if (differs == 0)
			parser->regex_extended = 1;
		return;
	}

	/* -maxdepth and -mindepth take a count without a sign. */
	valid = parse_number(operand, &number);
	if (!valid || number.comparison != 0) {
		fprintf(stderr, "find: %s: invalid depth\n", operand);
		parser->failed = 1;
		return;
	}

	/* The depth. */
	differs = strcmp(token, "-maxdepth");
	if (differs == 0)
		parser->max_depth = (long)number.value;
	else
		parser->min_depth = (long)number.value;
}

/*
 * Compiles the regex of -regex or -iregex, which must match the whole
 * path.  Returns nonzero after a message.
 */
static int
compile_find_regex(
	struct parser *parser,
	struct node *node)
{
	char message[256];
	int flags;
	int error;

	/* The kind of regex, and case. */
	flags = 0;
	if (parser->regex_extended)
		flags |= REG_EXTENDED;
	if (node->kind == NODE_IREGEX)
		flags |= REG_ICASE;

	/* The regex, kept in the node. */
	node->regex = malloc(sizeof(*node->regex));
	if (node->regex == NULL)
		return 1;
	error = regcomp(node->regex, node->text, flags);
	if (error != 0) {
		regerror(error, node->regex, message, sizeof(message));
		fprintf(stderr, "find: %s: %s\n", node->text, message);
		free(node->regex);
		node->regex = NULL;
		return 1;
	}

	/* Succeeded. */
	return 0;
}

/* Evaluates one of GNU's tests and actions. */
static int
evaluate_gnu(
	struct node *node,
	const char *path,
	const char *name,
	const struct stat *status,
	struct walk_state *state)
{
	regmatch_t match;
	time_t stamp;
	unsigned long long minutes;
	size_t length;
	int result;

	/* The test or action of its kind. */
	switch (node->kind) {
	case NODE_FALSE:
		return 0;
	case NODE_INAME:
		result = fnmatch(node->text, name, FNM_CASEFOLD);
		break;
	case NODE_IPATH:
		result = fnmatch(node->text, path, FNM_CASEFOLD);
		break;
	case NODE_REGEX:
	case NODE_IREGEX:
		/* The match must be the whole path. */
		result = regexec(node->regex, path, 1, &match, 0);
		if (result != 0)
			return 0;
		length = strlen(path);
		if (match.rm_so != 0 || (size_t)match.rm_eo != length)
			return 0;
		return 1;
	case NODE_EMPTY:
		result = file_empty(path, status);
		return result;
	case NODE_EXECUTABLE:
		result = access(path, X_OK);
		break;
	case NODE_READABLE:
		result = access(path, R_OK);
		break;
	case NODE_WRITABLE:
		result = access(path, W_OK);
		break;
	case NODE_AMIN:
	case NODE_CMIN:
	case NODE_MMIN:
		/* The minutes since the time, rounded up as GNU's -mmin counts. */
		stamp = status->st_mtime;
		if (node->kind == NODE_AMIN)
			stamp = status->st_atime;
		if (node->kind == NODE_CMIN)
			stamp = status->st_ctime;
		minutes = 0;
		if (state->now > stamp)
			minutes = ((unsigned long long)(state->now - stamp) + 59U) / 60U;
		result = number_matches(&node->number, minutes);
		return result;
	case NODE_DELETE:
		result = delete_file(path, status, state);
		return result;
	case NODE_PRINTF:
		print_format(node->text, path, name, status, state);
		return 1;
	case NODE_QUIT:
		state->quit = 1;
		return 1;
	default:
		return 0;
	}

	/* The tests that report 0 for a match. */
	if (result == 0)
		return 1;
	return 0;
}

/* Reports whether a file is empty: a regular file of no bytes, or a directory with no entries. */
static int
file_empty(
	const char *path,
	const struct stat *status)
{
	struct dirent *entry;
	DIR *directory;
	int dot;
	int dotdot;
	int empty;

	/* A regular file: its size. */
	if ((status->st_mode & S_IFMT) == S_IFREG) {
		if (status->st_size == 0)
			return 1;
		return 0;
	}

	/* Anything but a directory is not empty. */
	if ((status->st_mode & S_IFMT) != S_IFDIR)
		return 0;

	/* A directory: any entry but . and .. */
	directory = opendir(path);
	if (directory == NULL)
		return 0;
	empty = 1;
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL)
			break;
		dot = strcmp(entry->d_name, ".");
		dotdot = strcmp(entry->d_name, "..");
		if (dot != 0 && dotdot != 0) {
			empty = 0;
			break;
		}
	}

	/* Succeeded: the directory is done with. */
	closedir(directory);
	return empty;
}

/* -delete: removes a file, or an (emptied) directory.  Returns 0 on failure. */
static int
delete_file(
	const char *path,
	const struct stat *status,
	struct walk_state *state)
{
	int result;

	/* The starting point . is never removed. */
	result = strcmp(path, ".");
	if (result == 0)
		return 1;

	/* A directory, or anything else. */
	if ((status->st_mode & S_IFMT) == S_IFDIR)
		result = rmdir(path);
	else
		result = unlink(path);
	if (result != 0) {
		fprintf(stderr, "find: cannot delete '%s': %s\n", path,
			strerror(errno));
		state->errors = 1;
		return 0;
	}

	/* Succeeded. */
	return 1;
}

/*
 * -printf: writes a format with the file's details: %p the path, %f the
 * name, %h the directory, %P the path below the starting point, %s the
 * size, %m the permissions in octal, %M in the way ls writes them, %d the
 * depth, %y the type, %u and %g the owner's and group's names, %U and %G
 * their numbers, %T@ and %A@ and %C@ the times in seconds, %%; and the
 * escapes \n, \t, \0 and \\.
 */
static void
print_format(
	const char *format,
	const char *path,
	const char *name,
	const struct stat *status,
	struct walk_state *state)
{
	const char *cursor;
	char next;

	/* Each character of the format. */
	for (cursor = format; *cursor != '\0'; cursor++) {
		/* An escape. */
		if (*cursor == '\\' && cursor[1] != '\0') {
			cursor++;
			print_escape(*cursor);
			continue;
		}

		/* An ordinary character. */
		if (*cursor != '%' || cursor[1] == '\0') {
			putchar(*cursor);
			continue;
		}

		/* A directive, and the letter of %T@ and its kin. */
		cursor++;
		next = cursor[1];
		print_directive(*cursor, next, path, name, status, state);
		if ((*cursor == 'T' || *cursor == 'A' || *cursor == 'C') &&
		    next == '@')
			cursor++;
	}
}

/* Writes the character of a -printf escape. */
static void
print_escape(
	char letter)
{
	/* The escapes, and any other character as itself. */
	switch (letter) {
	case 'n':
		putchar('\n');
		break;
	case 't':
		putchar('\t');
		break;
	case '0':
		putchar('\0');
		break;
	default:
		putchar(letter);
		break;
	}
}

/* Writes one -printf directive; next is the character after its letter. */
static void
print_directive(
	char letter,
	char next,
	const char *path,
	const char *name,
	const struct stat *status,
	struct walk_state *state)
{
	struct passwd *account;
	struct group *group;
	const char *slash;
	size_t length;
	char mode[11];
	int differs;

	/* The directive of its letter. */
	switch (letter) {
	case 'p':
		fputs(path, stdout);
		break;
	case 'f':
		fputs(name, stdout);
		break;
	case 'h':
		/* The directory part of the path (. when there is none). */
		slash = strrchr(path, '/');
		if (slash == NULL)
			fputs(".", stdout);
		else if (slash == path)
			fputs("/", stdout);
		else
			fwrite(path, 1, (size_t)(slash - path), stdout);
		break;
	case 'P':
		/* The path below the starting point. */
		length = strlen(state->start);
		differs = strncmp(path, state->start, length);
		if (differs == 0) {
			path += length;
			if (*path == '/')
				path++;
		}

		/* What is left of the path. */
		fputs(path, stdout);
		break;
	case 's':
		printf("%lld", (long long)status->st_size);
		break;
	case 'm':
		printf("%o", (unsigned)(status->st_mode & 07777));
		break;
	case 'M':
		mode_text(status->st_mode, mode);
		fputs(mode, stdout);
		break;
	case 'd':
		printf("%u", state->depth);
		break;
	case 'y':
		putchar(type_letter(status->st_mode));
		break;
	case 'u':
		account = getpwuid(status->st_uid);
		if (account != NULL)
			fputs(account->pw_name, stdout);
		else
			printf("%lu", (unsigned long)status->st_uid);
		break;
	case 'g':
		group = getgrgid(status->st_gid);
		if (group != NULL)
			fputs(group->gr_name, stdout);
		else
			printf("%lu", (unsigned long)status->st_gid);
		break;
	case 'U':
		printf("%lu", (unsigned long)status->st_uid);
		break;
	case 'G':
		printf("%lu", (unsigned long)status->st_gid);
		break;
	case 'T':
	case 'A':
	case 'C':
		print_time(letter, next, status);
		break;
	case '%':
		putchar('%');
		break;
	default:
		/* A directive not known is written as it is. */
		putchar('%');
		putchar(letter);
		break;
	}
}

/* Writes a time of %T@, %A@ or %C@ (seconds since the epoch). */
static void
print_time(
	char letter,
	char next,
	const struct stat *status)
{
	time_t stamp;

	/* Only the @ form, the seconds, is written. */
	if (next != '@')
		return;

	/* The modification, access or change time. */
	stamp = status->st_mtime;
	if (letter == 'A')
		stamp = status->st_atime;
	if (letter == 'C')
		stamp = status->st_ctime;
	printf("%lld", (long long)stamp);
}

/* Returns the letter -type and %y use for a mode. */
static char
type_letter(
	mode_t mode)
{
	/* The kind of file. */
	switch (mode & S_IFMT) {
	case S_IFDIR:
		return 'd';
	case S_IFLNK:
		return 'l';
	case S_IFBLK:
		return 'b';
	case S_IFCHR:
		return 'c';
	case S_IFIFO:
		return 'p';
	case S_IFSOCK:
		return 's';
	default:
		break;
	}

	/* A regular file. */
	return 'f';
}

/* Writes a mode as ls -l does (%M): the type and nine permission letters. */
static void
mode_text(
	mode_t mode,
	char *text)
{
	static const char letters[] = "rwxrwxrwx";
	size_t index;

	/* The type; a regular file is -. */
	text[0] = type_letter(mode);
	if (text[0] == 'f')
		text[0] = '-';

	/* Each permission, or -. */
	for (index = 0; index < 9U; index++) {
		text[1U + index] = '-';
		if ((mode & (0400U >> index)) != 0)
			text[1U + index] = letters[index];
	}

	/* Succeeded: the text, ended. */
	text[10] = '\0';
}

/*
 * Reports whether the word the parser is at is an operator: its POSIX form
 * (-a, -o) or GNU's word (-and, -or).
 */
static int
is_operator(
	const struct parser *parser,
	const char *letter_form,
	const char *word_form)
{
	const char *word;
	int differs;

	/* The word, when there is one. */
	if (parser->index >= parser->argc)
		return 0;
	word = parser->argv[parser->index];

	/* The POSIX form. */
	differs = strcmp(word, letter_form);
	if (differs == 0)
		return 1;

	/* GNU's word. */
	differs = strcmp(word, word_form);
	if (differs == 0)
		return 1;

	/* Neither. */
	return 0;
}

/* Supports the file type operation. */
static int
file_type(
	mode_t mode,
	char type)
{
	int function_result;

	/* Dispatch the selected syntax or record type. */
	switch (type) {
	case 'b':
		/* Obtains the S ISBLK result. */
		function_result = S_ISBLK(mode);

		/* Returns the computed result. */
		return function_result;
	case 'c':
		/* Obtains the S ISCHR result. */
		function_result = S_ISCHR(mode);

		/* Returns the computed result. */
		return function_result;
	case 'd':
		/* Obtains the S ISDIR result. */
		function_result = S_ISDIR(mode);

		/* Returns the computed result. */
		return function_result;
	case 'f':
		/* Obtains the S ISREG result. */
		function_result = S_ISREG(mode);

		/* Returns the computed result. */
		return function_result;
	case 'l':
		/* Obtains the S ISLNK result. */
		function_result = S_ISLNK(mode);

		/* Returns the computed result. */
		return function_result;
	case 'p':
		/* Obtains the S ISFIFO result. */
		function_result = S_ISFIFO(mode);

		/* Returns the computed result. */
		return function_result;
	case 's':
		/* Obtains the S ISSOCK result. */
		function_result = S_ISSOCK(mode);

		/* Returns the computed result. */
		return function_result;
	default:
		/* Reports successful completion. */
		return 0;
	}
}

/* Supports the number matches operation. */
static int
number_matches(
	const struct number *number,
	unsigned long long value)
{
	/* Handles the number condition. */
	if (number->comparison > 0)
		return value > number->value;

	/* Handles the number condition. */
	if (number->comparison < 0)
		return value < number->value;

	/* Returns the computed result. */
	return value == number->value;
}

/*
 * Runs the utility of -exec or -ok (";") for one pathname: each "{}" in
 * its arguments, alone or inside one, is the pathname.  -ok asks first on
 * standard error and runs it on an answer starting with y.  Returns 1 when
 * the utility exited with 0, else 0.
 */
static int
run_command(
	const struct node *node,
	const char *path)
{
	char **arguments;
	int answer;
	int next;
	int index;
	int made;
	int passed;

	/* The arguments with the pathname in them. */
	arguments = calloc((size_t)node->argument_count + 1U, sizeof(*arguments));
	if (arguments == NULL)
		return 0;
	made = 1;
	for (index = 0; index < node->argument_count; index++) {
		arguments[index] = replace_braces(node->arguments[index], path);
		if (arguments[index] == NULL)
			made = 0;
	}

	/* -ok: the question and the answer's line. */
	answer = 'y';
	if (made && node->type == 'o') {
		fprintf(stderr, "< %s ... %s > ? ", arguments[0], path);
		(void)fflush(stderr);
		answer = getchar();
		next = answer;
		while (next != '\n' && next != EOF)
			next = getchar();
	}

	/* Run when made and agreed to. */
	passed = 0;
	if (made && (answer == 'y' || answer == 'Y'))
		passed = run_vector(arguments);

	/* The arguments made. */
	for (index = 0; index < node->argument_count; index++)
		free(arguments[index]);
	free(arguments);
	return passed;
}

/* A copy of an argument with each "{}" in it the pathname; NULL without memory. */
static char *
replace_braces(
	const char *argument,
	const char *path)
{
	const char *from;
	const char *found;
	char *copy;
	char *to;
	size_t count;
	size_t size;

	/* How many "{}" there are. */
	count = 0;
	for (from = strstr(argument, "{}"); from != NULL; from = strstr(from + 2, "{}"))
		count++;

	/* The copy's room. */
	size = strlen(argument) + count * strlen(path) + 1U;
	copy = malloc(size);
	if (copy == NULL)
		return NULL;

	/* The text between them, and the pathname for each. */
	to = copy;
	from = argument;
	for (;;) {
		found = strstr(from, "{}");
		if (found == NULL)
			break;
		memcpy(to, from, (size_t)(found - from));
		to += found - from;
		memcpy(to, path, strlen(path));
		to += strlen(path);
		from = found + 2;
	}

	/* The rest. */
	memcpy(to, from, strlen(from) + 1U);
	return copy;
}

/* Runs a utility with its arguments (a NULL at the end) and waits; 1 when it exited with 0, else 0. */
static int
run_vector(
	char **arguments)
{
	pid_t child;
	pid_t ended;
	int status;

	/* Its process. */
	(void)fflush(stdout);
	child = fork();
	if (child == 0) {
		execvp(arguments[0], arguments);
		fprintf(stderr, "find: %s: %s\n", arguments[0], strerror(errno));
		_exit(127);
	}

	/* Not started. */
	if (child < 0)
		return 0;

	/* Its end. */
	for (;;) {
		ended = waitpid(child, &status, 0);
		if (ended >= 0 || errno != EINTR)
			break;
	}

	/* Not collected. */
	if (ended < 0)
		return 0;

	/* Ended by a signal, or exited with another status. */
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return 0;

	/* Succeeded: exited with 0. */
	return 1;
}

/*
 * Gathers a pathname for -exec ... {} +, running the utility first with
 * those gathered when this one would take them past the argument limit.
 */
static void
batch_add(
	struct node *node,
	const char *path,
	struct walk_state *state)
{
	char **grown;
	char *copy;
	size_t bytes;
	size_t room;

	/* Room for it, else the ones gathered are run first. */
	bytes = strlen(path) + 1U + sizeof(char *);
	room = batch_room(node);
	if (node->batch_count > 0 && node->batch_bytes + bytes > room)
		batch_run(node, state);

	/* Kept. */
	copy = strdup(path);
	grown = realloc(node->batch, ((size_t)node->batch_count + 1U) * sizeof(*grown));
	if (copy == NULL || grown == NULL) {
		free(copy);
		if (grown != NULL)
			node->batch = grown;
		fprintf(stderr, "find: %s: %s\n", path, strerror(ENOMEM));
		state->errors = 1;
		return;
	}

	/* At the end of the set. */
	node->batch = grown;
	node->batch[node->batch_count++] = copy;
	node->batch_bytes += bytes;
}

/* Runs the utility of -exec ... {} + with the pathnames gathered; a failure makes find's status nonzero. */
static void
batch_run(
	struct node *node,
	struct walk_state *state)
{
	char **arguments;
	int fixed;
	int index;
	int passed;

	/* Nothing gathered. */
	if (node->batch_count == 0)
		return;

	/* The utility, its arguments before the "{}", then the pathnames. */
	fixed = node->argument_count - 1;
	arguments = calloc((size_t)fixed + (size_t)node->batch_count + 1U, sizeof(*arguments));
	passed = 0;
	if (arguments != NULL) {
		for (index = 0; index < fixed; index++)
			arguments[index] = node->arguments[index];
		for (index = 0; index < node->batch_count; index++)
			arguments[fixed + index] = node->batch[index];
		passed = run_vector(arguments);
	}

	/* A failure is find's too. */
	if (!passed)
		state->errors = 1;

	/* The next set starts empty. */
	free(arguments);
	for (index = 0; index < node->batch_count; index++)
		free(node->batch[index]);
	node->batch_count = 0;
	node->batch_bytes = 0;
}

/* Runs every -exec ... {} + of an expression with what it still holds. */
static void
batch_finish(
	struct node *node,
	struct walk_state *state)
{
	/* Each node of the tree. */
	if (node == NULL)
		return;
	batch_finish(node->left, state);
	batch_finish(node->right, state);
	if (node->kind == NODE_EXEC && node->type == '+')
		batch_run(node, state);
}

/*
 * The bytes the pathnames of one invocation may take: {ARG_MAX} less the
 * 2048 bytes POSIX keeps free, the environment, and the utility and its
 * other arguments (each string with a pointer, as the kernel counts them).
 */
static size_t
batch_room(
	const struct node *node)
{
	extern char **environ;
	char **entry;
	size_t taken;
	long limit;
	int index;

	/* The limit, or the POSIX minimum when it is not known. */
	limit = sysconf(_SC_ARG_MAX);
	if (limit <= 0)
		limit = _POSIX_ARG_MAX;

	/* What is taken already. */
	taken = FIND_ARG_RESERVE + sizeof(char *);
	for (entry = environ; entry != NULL && *entry != NULL; entry++)
		taken += strlen(*entry) + 1U + sizeof(char *);
	for (index = 0; index < node->argument_count - 1; index++)
		taken += strlen(node->arguments[index]) + 1U + sizeof(char *);

	/* What is left, never nothing (one pathname is always run). */
	if ((size_t)limit <= taken)
		return 0;
	return (size_t)limit - taken;
}

/*
 * Reads the mode of -perm: an octal number, or a symbolic mode applied to
 * a template with no bits set (the file mode creation mask applies to +
 * without who letters, as for chmod), after an optional "-" that asks for
 * at least those bits.  Returns 0, or EINVAL.
 */
static int
parse_perm(
	const char *text,
	struct node *node)
{
	mode_t mask;
	mode_t mode;
	int error;

	/* "-": at least the bits. */
	node->number.comparison = 0;
	if (text[0] == '-') {
		node->number.comparison = -1;
		text++;
	}

	/* The bits, as chmod gives them to a file that had none. */
	if (text[0] == '\0' || text[0] == '-')
		return EINVAL;
	mask = umask(0);
	(void)umask(mask);
	error = mode_apply(text, 0, mask, 0, &mode);
	if (error != 0)
		return EINVAL;
	node->mode = mode & 07777;
	return 0;
}

/*
 * Reads a file's information: when following, what a link names, else
 * the link itself; a link that names nothing gives its own.  Returns 0 or
 * an errno value.
 */
static int
stat_file(
	const char *path,
	int follow,
	struct stat *status)
{
	int error;

	/* The link itself. */
	if (!follow) {
		error = lstat(path, status);
		if (error != 0)
			return errno;
		return 0;
	}

	/* What it names, or the link when that is not there. */
	error = stat(path, status);
	if (error == 0)
		return 0;
	if (errno != ENOENT)
		return errno;
	error = lstat(path, status);
	if (error != 0)
		return errno;
	return 0;
}
