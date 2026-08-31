/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD cflow userland command.
 */

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

static void usage(void);
static int consume(const struct c_parse_result *result);
static size_t node_get(const char *name);
static size_t node_find(const char *name);
static int edge_add(size_t from, size_t to);
static void print_node(size_t index, unsigned depth, unsigned char *active);

/*
 * Runs the cflow command.
 */
int
main(
	int argc,
	char **argv)
{
	struct c_parse_result result_local;
	struct c_parse_result result_local1;
	char *end;
	unsigned long value;
	unsigned char *active;
	int i_index_for;
	size_t i_index_for1;
	size_t i_index_for2;
	size_t i_index_for3;
	int ch, failed;

	/* Parse each command-line option. */
	failed = 0;
	while ((ch = getopt(argc, argv, "d:D:i:I:rU:")) != -1) {
		/* Dispatch the selected operation case. */
		switch (ch) {
		case 'd':

		value = strtoul(optarg, &end, 10);

		/* Checks the current endpoint. */
		if (*end || !value || value > 1024) {
			usage();

			/* Reports operation failure. */
			return 2;
		}
		max_depth = (unsigned)value;
		break;
		case 'D':
		case 'I':
		case 'U':
			/*
 * Directives are tokenized internally; these
			 * compatibility options do not invoke a host
			 * preprocessor. */
			break;
		case 'i':
			/* Selects the matching value. */
			if (strcmp(optarg, "_") && strcmp(optarg, "x")) {
				usage();

				/* Reports operation failure. */
				return 2;
			}
			break;
		case 'r':
			reverse_graph = 1;
			break;
		default:
			usage();

			/* Reports operation failure. */
			return 2;
		}
	}

	/* Validates the command-line arguments. */
	if (optind == argc) {
		/* Handles a failed c parse stream operation. */
		if (c_parse_stream("<stdin>", STDIN_FILENO, &result_local)) {
			fprintf(stderr, "cflow: stdin: %s\n", strerror(errno));
			failed = 1;
		} else {
			failed |= consume(&result_local) != 0;
			c_parse_free(&result_local);
		}
	} else {
		/* Process each remaining command-line operand. */
		for (i_index_for = optind; i_index_for < argc; i_index_for++) {
			/* Validates the command-line arguments. */
			if (c_parse_path(argv[i_index_for], &result_local1)) {
				fprintf(stderr, "cflow: %s: %s\n", argv[i_index_for],
					strerror(errno));
				failed = 1;
				continue;
			}
			failed |= consume(&result_local1) != 0;
			c_parse_free(&result_local1);
		}
	}

	/* Handles an operation failure. */
	if (!failed && node_count) {
				active = calloc(node_count, 1);

		/* Handles the active condition. */
		if (!active)
			failed = 1;
		else {
			/* Process each remaining element. */
			for (i_index_for1 = 0; i_index_for1 < node_count; i_index_for1++)

				/* Handles the nodes condition. */
				if (nodes[i_index_for1].defined && !nodes[i_index_for1].called)
					print_node(i_index_for1, 0, active);

			/* Process each remaining element. */
			for (i_index_for2 = 0; i_index_for2 < node_count; i_index_for2++)

				/* Handles the nodes condition. */
				if (nodes[i_index_for2].defined && nodes[i_index_for2].called &&
				    !nodes[i_index_for2].edge_count)
					print_node(i_index_for2, 0, active);
			free(active);
		}
	}

	/* Process each remaining element. */
	for (i_index_for3 = 0; i_index_for3 < node_count; i_index_for3++) {
		free(nodes[i_index_for3].name);
		free(nodes[i_index_for3].file);
		free(nodes[i_index_for3].edges);
	}
	free(nodes);

	/* Returns the computed result. */
	return failed;
}

/* Supports the usage operation. */
static void
usage(
	void)
{
	fprintf(stderr,
		"usage: cflow [-r] [-d depth] [-D name[=value]] [-I dir] "
		"[-U name] [file ...]\n");
}

/* Supports the consume operation. */
static int
consume(
	const struct c_parse_result *result)
{
	size_t node;
	size_t caller;
	size_t callee;
	const struct c_symbol_event *event;
	size_t i_index_for;

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < result->count; i_index_for++) {
				event = &result->events[i_index_for];

		/* Handles the event condition. */
		if (event->kind == C_SYMBOL_FUNCTION) {
						node = node_get(event->name);

			/* Handles the node condition. */
			if (node == SIZE_MAX)
				return -1;
			nodes[node].defined = 1;

			/* Handles the nodes condition. */
			if (!nodes[node].file) {
				nodes[node].file = strdup(event->file);
				nodes[node].line = event->line;

				/* Handles the nodes condition. */
				if (!nodes[node].file)
					return -1;
			}
		} else if (event->kind == C_SYMBOL_CALL && event->function) {
						caller = node_get(event->function);
						callee = node_get(event->name);

			/* Handles the caller condition. */
			if (caller == SIZE_MAX || callee == SIZE_MAX)
				return -1;

			/* Handles the reverse graph condition. */
			if (reverse_graph) {
				/* Handles the edge add condition. */
				if (edge_add(callee, caller))
					return -1;
			} else if (edge_add(caller, callee))

				/* Reports operation failure. */
				return -1;
		}
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the node get operation. */
static size_t
node_get(
	const char *name)
{
	struct node *replacement;
	size_t found;

	found = node_find(name);

	/* Handles the found condition. */
	if (found != SIZE_MAX)
		return found;

	/* Handles the node count condition. */
	if (node_count == SIZE_MAX / sizeof(*nodes))
		return SIZE_MAX;
	replacement = realloc(nodes, (node_count + 1) * sizeof(*nodes));

	/* Handles the replacement condition. */
	if (!replacement)
		return SIZE_MAX;
	nodes = replacement;
	memset(&nodes[node_count], 0, sizeof(nodes[node_count]));
	nodes[node_count].name = strdup(name);

	/* Handles the nodes condition. */
	if (!nodes[node_count].name)
		return SIZE_MAX;

	/* Returns the computed result. */
	return node_count++;
}

/* Supports the node find operation. */
static size_t
node_find(
	const char *name)
{
	size_t i_index_for;

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < node_count; i_index_for++)

		/* Selects the matching value. */
		if (!strcmp(nodes[i_index_for].name, name))
			return i_index_for;

	/* Returns the computed result. */
	return SIZE_MAX;
}

/* Supports the edge add operation. */
static int
edge_add(
	size_t from,
	size_t to)
{
	size_t i_index_for;
	size_t *replacement;

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < nodes[from].edge_count; i_index_for++)

		/* Handles the nodes condition. */
		if (nodes[from].edges[i_index_for] == to)
			return 0;

	/* Handles the nodes condition. */
	if (nodes[from].edge_count == SIZE_MAX / sizeof(*replacement))
		return -1;
	replacement = realloc(nodes[from].edges, (nodes[from].edge_count + 1) *
						     sizeof(*replacement));

	/* Handles the replacement condition. */
	if (!replacement)
		return -1;
	nodes[from].edges = replacement;
	nodes[from].edges[nodes[from].edge_count++] = to;
	nodes[to].called = 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the print node operation. */
static void
print_node(
	size_t index,
	unsigned depth,
	unsigned char *active)
{
	unsigned i_index_for;
	size_t i_index_for1;

	/* Process each element required by the operation. */
	for (i_index_for = 0; i_index_for < depth; i_index_for++)
		fputs("    ", stdout);
	printf("%s", nodes[index].name);

	/* Handles the nodes condition. */
	if (nodes[index].defined)
		printf(" <%s:%zu>", nodes[index].file, nodes[index].line);
	else
		fputs(" <>", stdout);

	/* Handles the active condition. */
	if (active[index]) {
		puts(" [recursive]");

		/* Returns the computed result. */
		return;
	}
	putchar('\n');

	/* Handles the depth condition. */
	if (depth >= max_depth)
		return;

	/* Process each remaining element. */
	active[index] = 1;
	for (i_index_for1 = 0; i_index_for1 < nodes[index].edge_count; i_index_for1++)
		print_node(nodes[index].edges[i_index_for1], depth + 1, active);
	active[index] = 0;
}
