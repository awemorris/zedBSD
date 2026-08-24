/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/c_parser.h"

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct node {
	char *name;
	char *file;
	size_t line;
	size_t *edges;
	size_t edge_count;
	int defined;
	int called;
};

static struct node *nodes;
static size_t node_count;
static unsigned max_depth = 64;
static int reverse_graph;

static size_t
node_find(const char *name)
{
	for (size_t i = 0; i < node_count; i++)
		if (!strcmp(nodes[i].name, name))
			return i;
	return SIZE_MAX;
}

static size_t
node_get(const char *name)
{
	struct node *replacement;
	size_t found = node_find(name);
	if (found != SIZE_MAX)
		return found;
	if (node_count == SIZE_MAX / sizeof(*nodes))
		return SIZE_MAX;
	replacement = realloc(nodes, (node_count + 1) * sizeof(*nodes));
	if (!replacement)
		return SIZE_MAX;
	nodes = replacement;
	memset(&nodes[node_count], 0, sizeof(nodes[node_count]));
	nodes[node_count].name = strdup(name);
	if (!nodes[node_count].name)
		return SIZE_MAX;
	return node_count++;
}

static int
edge_add(size_t from, size_t to)
{
	size_t *replacement;
	for (size_t i = 0; i < nodes[from].edge_count; i++)
		if (nodes[from].edges[i] == to)
			return 0;
	if (nodes[from].edge_count == SIZE_MAX / sizeof(*replacement))
		return -1;
	replacement = realloc(nodes[from].edges, (nodes[from].edge_count + 1) *
						     sizeof(*replacement));
	if (!replacement)
		return -1;
	nodes[from].edges = replacement;
	nodes[from].edges[nodes[from].edge_count++] = to;
	nodes[to].called = 1;
	return 0;
}

static int
consume(const struct c_parse_result *result)
{
	for (size_t i = 0; i < result->count; i++) {
		const struct c_symbol_event *event = &result->events[i];
		if (event->kind == C_SYMBOL_FUNCTION) {
			size_t node = node_get(event->name);
			if (node == SIZE_MAX)
				return -1;
			nodes[node].defined = 1;
			if (!nodes[node].file) {
				nodes[node].file = strdup(event->file);
				nodes[node].line = event->line;
				if (!nodes[node].file)
					return -1;
			}
		} else if (event->kind == C_SYMBOL_CALL && event->function) {
			size_t caller = node_get(event->function);
			size_t callee = node_get(event->name);
			if (caller == SIZE_MAX || callee == SIZE_MAX)
				return -1;
			if (reverse_graph) {
				if (edge_add(callee, caller))
					return -1;
			} else if (edge_add(caller, callee))
				return -1;
		}
	}
	return 0;
}

static void
print_node(size_t index, unsigned depth, unsigned char *active)
{
	for (unsigned i = 0; i < depth; i++)
		fputs("    ", stdout);
	printf("%s", nodes[index].name);
	if (nodes[index].defined)
		printf(" <%s:%zu>", nodes[index].file, nodes[index].line);
	else
		fputs(" <>", stdout);
	if (active[index]) {
		puts(" [recursive]");
		return;
	}
	putchar('\n');
	if (depth >= max_depth)
		return;
	active[index] = 1;
	for (size_t i = 0; i < nodes[index].edge_count; i++)
		print_node(nodes[index].edges[i], depth + 1, active);
	active[index] = 0;
}

static void
usage(void)
{
	fprintf(stderr,
		"usage: cflow [-r] [-d depth] [-D name[=value]] [-I dir] "
		"[-U name] [file ...]\n");
}

int
main(int argc, char **argv)
{
	int ch, failed = 0;
	while ((ch = getopt(argc, argv, "d:D:i:I:rU:")) != -1) {
		switch (ch) {
		case 'd': {
			char *end;
			unsigned long value = strtoul(optarg, &end, 10);
			if (*end || !value || value > 1024) {
				usage();
				return 2;
			}
			max_depth = (unsigned)value;
			break;
		}
		case 'D':
		case 'I':
		case 'U':
			/* Directives are tokenized internally; these
			 * compatibility options do not invoke a host
			 * preprocessor. */
			break;
		case 'i':
			if (strcmp(optarg, "_") && strcmp(optarg, "x")) {
				usage();
				return 2;
			}
			break;
		case 'r':
			reverse_graph = 1;
			break;
		default:
			usage();
			return 2;
		}
	}
	if (optind == argc) {
		struct c_parse_result result;
		if (c_parse_stream("<stdin>", STDIN_FILENO, &result)) {
			fprintf(stderr, "cflow: stdin: %s\n", strerror(errno));
			failed = 1;
		} else {
			failed |= consume(&result) != 0;
			c_parse_free(&result);
		}
	} else {
		for (int i = optind; i < argc; i++) {
			struct c_parse_result result;
			if (c_parse_path(argv[i], &result)) {
				fprintf(stderr, "cflow: %s: %s\n", argv[i],
					strerror(errno));
				failed = 1;
				continue;
			}
			failed |= consume(&result) != 0;
			c_parse_free(&result);
		}
	}
	if (!failed && node_count) {
		unsigned char *active = calloc(node_count, 1);
		if (!active)
			failed = 1;
		else {
			for (size_t i = 0; i < node_count; i++)
				if (nodes[i].defined && !nodes[i].called)
					print_node(i, 0, active);
			for (size_t i = 0; i < node_count; i++)
				if (nodes[i].defined && nodes[i].called &&
				    !nodes[i].edge_count)
					print_node(i, 0, active);
			free(active);
		}
	}
	for (size_t i = 0; i < node_count; i++) {
		free(nodes[i].name);
		free(nodes[i].file);
		free(nodes[i].edges);
	}
	free(nodes);
	return failed;
}
