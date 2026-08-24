/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

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

static void
usage(void)
{
	fprintf(stderr, "usage: find [-H|-L] [path ...] [expression]\n");
}

static struct node *
new_node(enum node_kind kind)
{
	struct node *node = calloc(1, sizeof(*node));

	if (node != NULL)
		node->kind = kind;
	return node;
}

static int
is_expression(const char *text)
{
	return text[0] == '-' || strcmp(text, "!") == 0 ||
	       strcmp(text, "(") == 0;
}

static int
parse_number(const char *text, struct number *number)
{
	char *end;

	number->comparison = 0;
	if (*text == '+' || *text == '-') {
		number->comparison = *text == '+' ? 1 : -1;
		text++;
	}
	if (*text == '\0')
		return 0;
	errno = 0;
	number->value = strtoull(text, &end, 10);
	return errno == 0 && *end == '\0';
}

static int
number_matches(const struct number *number, unsigned long long value)
{
	if (number->comparison > 0)
		return value > number->value;
	if (number->comparison < 0)
		return value < number->value;
	return value == number->value;
}

static char *
take_operand(struct parser *parser, const char *option)
{
	if (parser->index >= parser->argc) {
		fprintf(stderr, "find: %s: missing operand\n", option);
		parser->failed = 1;
		return NULL;
	}
	return parser->argv[parser->index++];
}

static struct node *parse_or(struct parser *);

static struct node *
parse_primary(struct parser *parser)
{
	struct node *node;
	char *token;

	if (parser->index >= parser->argc)
		return NULL;
	token = parser->argv[parser->index++];
	if (strcmp(token, "(") == 0) {
		node = parse_or(parser);
		if (parser->index >= parser->argc ||
		    strcmp(parser->argv[parser->index++], ")") != 0) {
			fprintf(stderr, "find: missing ')'\n");
			parser->failed = 1;
		}
		return node;
	}
	if (strcmp(token, "-true") == 0)
		return new_node(NODE_TRUE);
	if (strcmp(token, "-depth") == 0) {
		parser->depth_first = 1;
		return new_node(NODE_TRUE);
	}
	if (strcmp(token, "-xdev") == 0) {
		parser->same_device = 1;
		return new_node(NODE_TRUE);
	}
	if (strcmp(token, "-print") == 0)
		return new_node(NODE_PRINT);
	if (strcmp(token, "-prune") == 0)
		return new_node(NODE_PRUNE);
	if (strcmp(token, "-name") == 0 || strcmp(token, "-path") == 0) {
		node = new_node(strcmp(token, "-name") == 0 ? NODE_NAME
							    : NODE_PATH);
		if (node != NULL)
			node->text = take_operand(parser, token);
		return node;
	}
	if (strcmp(token, "-type") == 0) {
		char *type;

		node = new_node(NODE_TYPE);
		type = take_operand(parser, token);
		if (node != NULL && type != NULL && type[0] != '\0' &&
		    type[1] == '\0' && strchr("bcdflps", type[0]) != NULL)
			node->type = type[0];
		else
			parser->failed = 1;
		return node;
	}
	if (strcmp(token, "-perm") == 0) {
		char *value = take_operand(parser, token);
		char *end;
		unsigned long mode;

		node = new_node(NODE_PERM);
		if (value == NULL || node == NULL)
			return node;
		node->number.comparison = *value == '-' ? -1 : 0;
		if (*value == '-')
			value++;
		errno = 0;
		mode = strtoul(value, &end, 8);
		if (errno != 0 || *value == '\0' || *end != '\0' ||
		    mode > 07777)
			parser->failed = 1;
		node->mode = (mode_t)mode;
		return node;
	}
	if (strcmp(token, "-user") == 0 || strcmp(token, "-group") == 0) {
		char *value = take_operand(parser, token);
		char *end;
		unsigned long id;

		node = new_node(strcmp(token, "-user") == 0 ? NODE_USER
							    : NODE_GROUP);
		if (value == NULL || node == NULL)
			return node;
		errno = 0;
		id = strtoul(value, &end, 10);
		if (errno == 0 && *value != '\0' && *end == '\0') {
			if (node->kind == NODE_USER)
				node->uid = (uid_t)id;
			else
				node->gid = (gid_t)id;
		} else if (node->kind == NODE_USER) {
			struct passwd *account = getpwnam(value);

			if (account == NULL)
				parser->failed = 1;
			else
				node->uid = account->pw_uid;
		} else {
			struct group *group = getgrnam(value);

			if (group == NULL)
				parser->failed = 1;
			else
				node->gid = group->gr_gid;
		}
		return node;
	}
	if (strcmp(token, "-nouser") == 0)
		return new_node(NODE_NOUSER);
	if (strcmp(token, "-nogroup") == 0)
		return new_node(NODE_NOGROUP);
	if (strcmp(token, "-links") == 0 || strcmp(token, "-size") == 0 ||
	    strcmp(token, "-atime") == 0 || strcmp(token, "-ctime") == 0 ||
	    strcmp(token, "-mtime") == 0) {
		char *value = take_operand(parser, token);
		size_t length;

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
		if (node == NULL || value == NULL)
			return node;
		length = strlen(value);
		if (node->kind == NODE_SIZE && length != 0 &&
		    value[length - 1U] == 'c') {
			node->type = 'c';
			value[length - 1U] = '\0';
		}
		if (!parse_number(value, &node->number))
			parser->failed = 1;
		return node;
	}
	if (strcmp(token, "-newer") == 0) {
		char *path = take_operand(parser, token);

		node = new_node(NODE_NEWER);
		if (node != NULL &&
		    (path == NULL || stat(path, &node->reference) != 0)) {
			fprintf(stderr, "find: %s: %s\n",
				path != NULL ? path : token, strerror(errno));
			parser->failed = 1;
		}
		return node;
	}
	if (strcmp(token, "-exec") == 0 || strcmp(token, "-ok") == 0) {
		int begin = parser->index;
		int prompt = strcmp(token, "-ok") == 0;

		while (parser->index < parser->argc &&
		       strcmp(parser->argv[parser->index], ";") != 0 &&
		       strcmp(parser->argv[parser->index], "+") != 0)
			parser->index++;
		if (parser->index == begin || parser->index == parser->argc) {
			fprintf(stderr, "find: %s: missing terminator\n",
				token);
			parser->failed = 1;
			return NULL;
		}
		node = new_node(NODE_EXEC);
		if (node != NULL) {
			node->arguments = &parser->argv[begin];
			node->argument_count = parser->index - begin;
			node->type = prompt ? 'o' : 'e';
		}
		parser->index++;
		return node;
	}
	fprintf(stderr, "find: unknown expression primary: %s\n", token);
	parser->failed = 1;
	return NULL;
}

static struct node *
parse_not(struct parser *parser)
{
	struct node *node;

	if (parser->index < parser->argc &&
	    (strcmp(parser->argv[parser->index], "!") == 0 ||
	     strcmp(parser->argv[parser->index], "-not") == 0)) {
		parser->index++;
		node = new_node(NODE_NOT);
		if (node != NULL)
			node->left = parse_not(parser);
		return node;
	}
	return parse_primary(parser);
}

static struct node *
parse_and(struct parser *parser)
{
	struct node *left = parse_not(parser);

	while (!parser->failed && parser->index < parser->argc &&
	       strcmp(parser->argv[parser->index], ")") != 0 &&
	       strcmp(parser->argv[parser->index], "-o") != 0) {
		struct node *parent = new_node(NODE_AND);

		if (strcmp(parser->argv[parser->index], "-a") == 0)
			parser->index++;
		if (parent == NULL)
			return NULL;
		parent->left = left;
		parent->right = parse_not(parser);
		left = parent;
	}
	return left;
}

static struct node *
parse_or(struct parser *parser)
{
	struct node *left = parse_and(parser);

	while (!parser->failed && parser->index < parser->argc &&
	       strcmp(parser->argv[parser->index], "-o") == 0) {
		struct node *parent = new_node(NODE_OR);

		parser->index++;
		if (parent == NULL)
			return NULL;
		parent->left = left;
		parent->right = parse_and(parser);
		left = parent;
	}
	return left;
}

static int
file_type(mode_t mode, char type)
{
	switch (type) {
	case 'b':
		return S_ISBLK(mode);
	case 'c':
		return S_ISCHR(mode);
	case 'd':
		return S_ISDIR(mode);
	case 'f':
		return S_ISREG(mode);
	case 'l':
		return S_ISLNK(mode);
	case 'p':
		return S_ISFIFO(mode);
	case 's':
		return S_ISSOCK(mode);
	default:
		return 0;
	}
}

static int
run_command(const struct node *node, const char *path)
{
	char **arguments =
	    calloc((size_t)node->argument_count + 1U, sizeof(*arguments));
	pid_t child;
	int status;
	int index;

	if (arguments == NULL)
		return 0;
	for (index = 0; index < node->argument_count; index++)
		arguments[index] = strcmp(node->arguments[index], "{}") == 0
				       ? (char *)path
				       : node->arguments[index];
	if (node->type == 'o') {
		int answer;

		fprintf(stderr, "< %s ... %s > ? ", arguments[0], path);
		(void)fflush(stderr);
		answer = getchar();
		while (answer != '\n' && answer != EOF) {
			int next = getchar();

			if (next == '\n' || next == EOF)
				break;
		}
		if (answer != 'y' && answer != 'Y') {
			free(arguments);
			return 0;
		}
	}
	child = fork();
	if (child == 0) {
		execvp(arguments[0], arguments);
		fprintf(stderr, "find: %s: %s\n", arguments[0],
			strerror(errno));
		_exit(127);
	}
	free(arguments);
	if (child < 0)
		return 0;
	while (waitpid(child, &status, 0) < 0)
		if (errno != EINTR)
			return 0;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int
evaluate(struct node *node, const char *path, const char *name,
	 const struct stat *status, struct walk_state *state)
{
	unsigned long long value;

	if (node == NULL)
		return 1;
	switch (node->kind) {
	case NODE_AND:
		return evaluate(node->left, path, name, status, state) &&
		       evaluate(node->right, path, name, status, state);
	case NODE_OR:
		return evaluate(node->left, path, name, status, state) ||
		       evaluate(node->right, path, name, status, state);
	case NODE_NOT:
		return !evaluate(node->left, path, name, status, state);
	case NODE_TRUE:
		return 1;
	case NODE_NAME:
		return fnmatch(node->text, name, 0) == 0;
	case NODE_PATH:
		return fnmatch(node->text, path, 0) == 0;
	case NODE_TYPE:
		return file_type(status->st_mode, node->type);
	case NODE_PERM:
		return node->number.comparison < 0
			   ? (status->st_mode & node->mode) == node->mode
			   : (status->st_mode & 07777) == node->mode;
	case NODE_USER:
		return status->st_uid == node->uid;
	case NODE_GROUP:
		return status->st_gid == node->gid;
	case NODE_NOUSER:
		return getpwuid(status->st_uid) == NULL;
	case NODE_NOGROUP:
		return getgrgid(status->st_gid) == NULL;
	case NODE_LINKS:
		return number_matches(&node->number, status->st_nlink);
	case NODE_SIZE:
		value = status->st_size < 0
			    ? 0
			    : (unsigned long long)status->st_size;
		if (node->type != 'c')
			value = (value + 511U) / 512U;
		return number_matches(&node->number, value);
	case NODE_ATIME:
	case NODE_CTIME:
	case NODE_MTIME: {
		time_t stamp = node->kind == NODE_ATIME	  ? status->st_atime
			       : node->kind == NODE_CTIME ? status->st_ctime
							  : status->st_mtime;

		value = stamp > state->now
			    ? 0
			    : (unsigned long long)(state->now - stamp) / 86400U;
		return number_matches(&node->number, value);
	}
	case NODE_NEWER:
		return status->st_mtime > node->reference.st_mtime;
	case NODE_PRINT:
		return puts(path) != EOF;
	case NODE_PRUNE:
		state->prune = 1;
		return 1;
	case NODE_EXEC:
		return run_command(node, path);
	default:
		return 0;
	}
}

static int
walk_path(const char *path, struct node *expression, struct walk_state *state)
{
	struct stat status;
	const char *name = strrchr(path, '/');
	int directory;
	int result = 1;

	name = name != NULL && name[1] != '\0' ? name + 1 : path;
	if ((state->follow || (state->follow_root && state->depth == 0)
		 ? stat(path, &status)
		 : lstat(path, &status)) != 0) {
		fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
		state->errors = 1;
		return 0;
	}
	directory = S_ISDIR(status.st_mode);
	if (!state->have_root_device) {
		state->root_device = status.st_dev;
		state->have_root_device = 1;
	}
	state->prune = 0;
	if (!state->depth_first)
		result = evaluate(expression, path, name, &status, state);
	if (directory && !state->prune &&
	    (!state->same_device || status.st_dev == state->root_device)) {
		DIR *stream;
		struct dirent *entry;
		unsigned ancestor;

		for (ancestor = 0; ancestor < state->depth; ancestor++)
			if (state->ancestors_dev[ancestor] == status.st_dev &&
			    state->ancestors_ino[ancestor] == status.st_ino) {
				fprintf(stderr, "find: %s: directory cycle\n",
					path);
				state->errors = 1;
				return 0;
			}
		if (state->depth == sizeof(state->ancestors_dev) /
					sizeof(state->ancestors_dev[0])) {
			fprintf(stderr, "find: %s: nesting limit exceeded\n",
				path);
			state->errors = 1;
			return 0;
		}
		state->ancestors_dev[state->depth] = status.st_dev;
		state->ancestors_ino[state->depth++] = status.st_ino;
		stream = opendir(path);
		if (stream == NULL) {
			fprintf(stderr, "find: %s: %s\n", path,
				strerror(errno));
			state->errors = 1;
		} else {
			while ((entry = readdir(stream)) != NULL) {
				char child[PATH_MAX + 1U];
				int length;

				if (strcmp(entry->d_name, ".") == 0 ||
				    strcmp(entry->d_name, "..") == 0)
					continue;
				length = snprintf(
				    child, sizeof(child),
				    strcmp(path, "/") == 0 ? "%s%s" : "%s/%s",
				    path, entry->d_name);
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
			if (closedir(stream) != 0)
				state->errors = 1;
		}
		state->depth--;
	}
	if (state->depth_first)
		result = evaluate(expression, path, name, &status, state);
	if (!result && ferror(stdout))
		state->errors = 1;
	return result;
}

static void
free_expression(struct node *node)
{
	if (node == NULL)
		return;
	free_expression(node->left);
	free_expression(node->right);
	free(node);
}

static void
scan_walk_options(struct node *node, struct walk_state *state)
{
	if (node == NULL)
		return;
	if (node->kind == NODE_PRUNE)
		return;
	scan_walk_options(node->left, state);
	scan_walk_options(node->right, state);
}

int
main(int argc, char **argv)
{
	struct parser parser = {.argc = argc, .argv = argv, .index = 1};
	struct walk_state state = {0};
	struct node *expression;
	int path_begin;
	int path_end;
	int index;
	int has_action = 0;

	while (parser.index < argc && (strcmp(argv[parser.index], "-H") == 0 ||
				       strcmp(argv[parser.index], "-L") == 0)) {
		state.follow = strcmp(argv[parser.index], "-L") == 0;
		state.follow_root = strcmp(argv[parser.index], "-H") == 0;
		parser.index++;
	}
	path_begin = parser.index;
	while (parser.index < argc && !is_expression(argv[parser.index]))
		parser.index++;
	path_end = parser.index;
	if (path_begin == path_end)
		path_begin = -1;
	expression =
	    parser.index == argc ? new_node(NODE_PRINT) : parse_or(&parser);
	if (expression == NULL || parser.failed || parser.index != argc) {
		usage();
		free_expression(expression);
		return 2;
	}
	state.depth_first = parser.depth_first;
	state.same_device = parser.same_device;
	for (index = 0; index < argc; index++)
		if (strcmp(argv[index], "-print") == 0 ||
		    strcmp(argv[index], "-exec") == 0 ||
		    strcmp(argv[index], "-ok") == 0)
			has_action = 1;
	if (!has_action) {
		struct node *print = new_node(NODE_PRINT);
		struct node *both = new_node(NODE_AND);

		if (print == NULL || both == NULL) {
			free(print);
			free(both);
			free_expression(expression);
			return 1;
		}
		both->left = expression;
		both->right = print;
		expression = both;
	}
	state.now = time(NULL);
	scan_walk_options(expression, &state);
	if (path_begin < 0)
		(void)walk_path(".", expression, &state);
	else
		for (index = path_begin; index < path_end; index++) {
			state.have_root_device = 0;
			(void)walk_path(argv[index], expression, &state);
		}
	free_expression(expression);
	if (ferror(stdout)) {
		fprintf(stderr, "find: write error\n");
		state.errors = 1;
	}
	return state.errors ? 1 : 0;
}
