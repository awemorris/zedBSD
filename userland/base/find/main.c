/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD find userland command.
 */

#include <dirent.h>
#include <errno.h>
#include "libc/include/fnmatch.h"
#include <grp.h>
#include <limits.h>
#include <pwd.h>
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
	NODE_PRUNE,
	NODE_EXEC,
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
};

struct parser {
	int argc;
	char **argv;
	int index;
	int failed;
	int depth_first;
	int same_device;
};

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
static void scan_walk_options(struct node *node, struct walk_state *state);
static int walk_path(const char *path, struct node *expression, struct walk_state *state);
static int evaluate(struct node *node, const char *path, const char *name, const struct stat *status, struct walk_state *state);
static int file_type(mode_t mode, char type);
static int number_matches(const struct number *number, unsigned long long value);
static int run_command(const struct node *node, const char *path);

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

	has_action = 0;

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
	expression =
	    parser.index == argc ? new_node(NODE_PRINT) : parse_or(&parser);

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
	for (index = 0; index < argc; index++) {
		/* Handles the selected command-line operation. */
		if (strcmp(argv[index], "-print") == 0 ||
		    strcmp(argv[index], "-exec") == 0 ||
		    strcmp(argv[index], "-ok") == 0)
			has_action = 1;
	}

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
	if (path_begin < 0)
		(void)walk_path(".", expression, &state);
	else

		/* Process each remaining element. */
		for (index = path_begin; index < path_end; index++) {
			state.have_root_device = 0;
			(void)walk_path(argv[index], expression, &state);
		}
	free_expression(expression);

	/* Handles an operation failure. */
	if (ferror(stdout)) {
		fprintf(stderr, "find: write error\n");
		state.errors = 1;
	}

	/* Returns the computed result. */
	return state.errors ? 1 : 0;
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

/* Supports the parse or operation. */
static struct node *
parse_or(
	struct parser *parser)
{
	struct node *parent;
	struct node *left;

	left = parse_and(parser);

	/* Process each remaining command-line operand. */
	while (!parser->failed && parser->index < parser->argc &&
	       strcmp(parser->argv[parser->index], "-o") == 0) {
		parent = new_node(NODE_OR);

		parser->index++;

		/* Handles the parent availability. */
		if (parent == NULL)
			return NULL;
		parent->left = left;
		parent->right = parse_and(parser);
		left = parent;
	}

	/* Returns the computed result. */
	return left;
}

/* Supports the parse and operation. */
static struct node *
parse_and(
	struct parser *parser)
{
	struct node *parent;
	struct node *left;

	left = parse_not(parser);

	/* Process each remaining command-line operand. */
	while (!parser->failed && parser->index < parser->argc &&
	       strcmp(parser->argv[parser->index], ")") != 0 &&
	       strcmp(parser->argv[parser->index], "-o") != 0) {
		parent = new_node(NODE_AND);

		/* Handles the selected command-line operation. */
		if (strcmp(parser->argv[parser->index], "-a") == 0)
			parser->index++;

		/* Handles the parent availability. */
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
	char *end_local;
	char *value_local1;
	char *end_local2;
	char *value_local3;
	char *type;
	unsigned long mode;
	struct passwd *account;
	struct group *group;
	unsigned long id;
	size_t length;
	char *path;
	int begin;
	int prompt;
	struct node *node;
	char *token;

	/* Validates the command-line arguments. */
	if (parser->index >= parser->argc)
		return NULL;
	token = parser->argv[parser->index++];

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
		node->number.comparison = *value_local == '-' ? -1 : 0;

		/* Handles the value local condition. */
		if (*value_local == '-')
			value_local++;
		errno = 0;
		mode = strtoul(value_local, &end_local, 8);

		/* Handles the reported system error. */
		if (errno != 0 || *value_local == '\0' || *end_local != '\0' ||
		    mode > 07777)
			parser->failed = 1;
		node->mode = (mode_t)mode;

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

		/* Process each remaining command-line operand. */
		while (parser->index < parser->argc &&
		       strcmp(parser->argv[parser->index], ";") != 0 &&
		       strcmp(parser->argv[parser->index], "+") != 0)
			parser->index++;

		/* Validates the command-line arguments. */
		if (parser->index == begin || parser->index == parser->argc) {
			fprintf(stderr, "find: %s: missing terminator\n",
				token);
			parser->failed = 1;

			/* Reports that no result is available. */
			return NULL;
		}
		node = new_node(NODE_EXEC);

		/* Handles the node availability. */
		if (node != NULL) {
			node->arguments = &parser->argv[begin];
			node->argument_count = parser->index - begin;
			node->type = prompt ? 'o' : 'e';
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
	/* Handles the node availability. */
	if (node == NULL)
		return;
	free_expression(node->left);
	free_expression(node->right);
	free(node);
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
	int directory;
	int result;

	name = strrchr(path, '/');
	result = 1;

	name = name != NULL && name[1] != '\0' ? name + 1 : path;

	/* Handles a failed stat operation. */
	if ((state->follow || (state->follow_root && state->depth == 0)
		 ? stat(path, &status)
		 : lstat(path, &status)) != 0) {
		fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
		state->errors = 1;

		/* Reports successful completion. */
		return 0;
	}
	directory = S_ISDIR(status.st_mode);

	/* Handles the state condition. */
	if (!state->have_root_device) {
		state->root_device = status.st_dev;
		state->have_root_device = 1;
	}
	state->prune = 0;

	/* Handles the state condition. */
	if (!state->depth_first)
		result = evaluate(expression, path, name, &status, state);

	/* Handles the directory condition. */
	if (directory && !state->prune &&
	    (!state->same_device || status.st_dev == state->root_device)) {
		/* Process each element required by the operation. */
		for (ancestor = 0; ancestor < state->depth; ancestor++) {
			/* Handles the state condition. */
			if (state->ancestors_dev[ancestor] == status.st_dev &&
			    state->ancestors_ino[ancestor] == status.st_ino) {
				fprintf(stderr, "find: %s: directory cycle\n",
					path);
				state->errors = 1;

				/* Reports successful completion. */
				return 0;
			}
		}

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
			/* Process each directory entry. */
			while ((entry = readdir(stream)) != NULL) {
				/* Selects the matching value. */
				if (strcmp(entry->d_name, ".") == 0 ||
				    strcmp(entry->d_name, "..") == 0)
					continue;
				length = snprintf(
				    child, sizeof(child),
				    strcmp(path, "/") == 0 ? "%s%s" : "%s/%s",
				    path, entry->d_name);

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
			}

			/* Handles a failed closedir operation. */
			if (closedir(stream) != 0)
				state->errors = 1;
		}
		state->depth--;
	}

	/* Handles the state condition. */
	if (state->depth_first)
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
		/* Computes the function result. */
		function_result = puts(path) != EOF;

		/* Returns the computed result. */
		return function_result;
	case NODE_PRUNE:
		state->prune = 1;

		/* Reports operation failure. */
		return 1;
	case NODE_EXEC:
		/* Obtains the run command result. */
		function_result = run_command(node, path);

		/* Returns the computed result. */
		return function_result;
	default:
		/* Reports successful completion. */
		return 0;
	}
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

/* Supports the run command operation. */
static int
run_command(
	const struct node *node,
	const char *path)
{
	int function_result;
	int next;
	int answer;
	char **arguments;
	pid_t child;
	int status;
	int index;

	arguments = calloc((size_t)node->argument_count + 1U, sizeof(*arguments));

	/* Handles the arguments availability. */
	if (arguments == NULL)
		return 0;

	/* Process each remaining element. */
	for (index = 0; index < node->argument_count; index++) {
		arguments[index] = strcmp(node->arguments[index], "{}") == 0
				       ? (char *)path
				       : node->arguments[index];
	}

	/* Handles the node condition. */
	if (node->type == 'o') {
		fprintf(stderr, "< %s ... %s > ? ", arguments[0], path);
		(void)fflush(stderr);

		/* Continue while the operation condition remains true. */
		answer = getchar();
		while (answer != '\n' && answer != EOF) {
			next = getchar();

			/* Handles the end-of-file condition. */
			if (next == '\n' || next == EOF)
				break;
		}

		/* Handles the answer condition. */
		if (answer != 'y' && answer != 'Y') {
			free(arguments);

			/* Reports successful completion. */
			return 0;
		}
	}
	child = fork();

	/* Checks the child process state. */
	if (child == 0) {
		execvp(arguments[0], arguments);
		fprintf(stderr, "find: %s: %s\n", arguments[0],
			strerror(errno));
		_exit(127);
	}
	free(arguments);

	/* Checks the child process state. */
	if (child < 0)
		return 0;

	/* Continue while the operation condition remains true. */
	while (waitpid(child, &status, 0) < 0) {
		/* Handles the reported system error. */
		if (errno != EINTR)
			return 0;
	}

	/* Computes the function result. */
	function_result = WIFEXITED(status) && WEXITSTATUS(status) == 0;

	/* Returns the computed result. */
	return function_result;
}
