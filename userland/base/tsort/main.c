/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD tsort userland command.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CYCLE_STATUS_MAX 124U

struct node {
	char *name;
	unsigned *edges;
	size_t edge_count;
	size_t edge_capacity;
	unsigned indegree;
	unsigned emitted : 1;
};

struct graph {
	struct node *nodes;
	size_t count;
	size_t capacity;
};

static char *read_token(FILE *stream);
static int node_index(struct graph *graph, const char *name, unsigned *result);
static int add_edge(struct graph *graph, unsigned from, unsigned to);
static int break_cycle(struct graph *graph);
static int find_cycle(struct graph *graph, unsigned current, unsigned *state, unsigned *stack, size_t *depth, unsigned *cycle_start, unsigned *cycle_end);
static void remove_edge(struct graph *graph, unsigned from, size_t edge);
static void free_graph(struct graph *graph);

/*
 * Runs the tsort command.
 */
int
main(
	int argc,
	char **argv)
{
	char *left;
	char *right;
	unsigned from;
	unsigned to;
	struct node *node;
	size_t edge;
	size_t node_index_value;
	struct graph graph = {0};
	FILE *input;
	const char *path;
	unsigned cycles;
	int count_cycles;
	int index;
	int status;
	size_t emitted;

	input = stdin;
	path = NULL;
	cycles = 0;
	count_cycles = 0;
	index = 1;
	status = 0;
	emitted = 0;

	/* Handles the selected command-line operation. */
	if (index < argc && strcmp(argv[index], "-w") == 0) {
		count_cycles = 1;
		index++;
	}

	/* Handles the selected command-line operation. */
	if (index < argc && strcmp(argv[index], "--") == 0)
		index++;

	/* Validates the command-line arguments. */
	if (argc - index > 1) {
		fprintf(stderr, "usage: tsort [-w] [file]\n");

		/* Returns the computed result. */
		return 125;
	}

	/* Handles the selected command-line operation. */
	if (index < argc && strcmp(argv[index], "-") != 0) {
		path = argv[index];
		input = fopen(path, "r");

		/* Handles the input availability. */
		if (input == NULL) {
			fprintf(stderr, "tsort: %s: %s\n", path,
				strerror(errno));

			/* Returns the computed result. */
			return 125;
		}
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;) {

		left = read_token(input);

		/* Handles the left availability. */
		if (left == NULL)
			break;
		right = read_token(input);

		/* Handles the right availability. */
		if (right == NULL) {
			fprintf(stderr, "tsort: odd number of input fields\n");
			free(left);
			status = 125;
			goto done;
		}

		/* Handles a failed node index operation. */
		if (!node_index(&graph, left, &from) ||
		    !node_index(&graph, right, &to) ||
		    !add_edge(&graph, from, to)) {
			fprintf(stderr, "tsort: out of memory\n");
			free(left);
			free(right);
			status = 125;
			goto done;
		}
		free(left);
		free(right);
	}

	/* Handles an operation failure. */
	if (ferror(input)) {
		fprintf(stderr, "tsort: input error\n");
		status = 125;
		goto done;
	}
	while (emitted < graph.count) {
		/* Process each remaining element. */
		for (node_index_value = 0; node_index_value < graph.count;
		     node_index_value++) {
						node = &graph.nodes[node_index_value];

			/* Handles the node condition. */
			if (node->emitted || node->indegree != 0)
				continue;
			puts(node->name);
			node->emitted = 1;
			emitted++;

			/* Process each remaining element. */
			for (edge = 0; edge < node->edge_count; edge++)
				graph.nodes[node->edges[edge]].indegree--;
			break;
		}

		/* Handles the node index value condition. */
		if (node_index_value == graph.count) {
			/* Handles a failed break cycle operation. */
			if (!break_cycle(&graph)) {
				fprintf(stderr,
					"tsort: cannot resolve input graph\n");
				status = 125;
				goto done;
			}

			/* Handles the cycles condition. */
			if (cycles < CYCLE_STATUS_MAX)
				cycles++;
		}
	}

	/* Handles an operation failure. */
	if (ferror(stdout))
		status = 125;
	else if (cycles != 0)
		status = count_cycles ? (int)cycles : 1;
done:

	/* Handles a failed fclose operation. */
	if (input != stdin && fclose(input) != 0 && status == 0)
		status = 125;
	free_graph(&graph);

	/* Returns the computed result. */
	return status;
}

/* Supports the read token operation. */
static char *
read_token(
	FILE *stream)
{
	char *larger;
	char *text;
	size_t length;
	size_t capacity;
	int character;

	length = 0;
	capacity = 32;

	do {
		character = fgetc(stream);
	} while (character != EOF && isspace((unsigned char)character));

	/* Handles the end-of-file condition. */
	if (character == EOF)
		return NULL;
	text = malloc(capacity);

	/* Handles the text availability. */
	if (text == NULL)
		return NULL;
	do {
		/* Checks the current data length. */
		if (length + 1 >= capacity) {

			capacity *= 2;
			larger = realloc(text, capacity);

			/* Handles the larger availability. */
			if (larger == NULL) {
				free(text);

				/* Reports that no result is available. */
				return NULL;
			}
			text = larger;
		}
		text[length++] = (char)character;
		character = fgetc(stream);
	} while (character != EOF && !isspace((unsigned char)character));
	text[length] = '\0';

	/* Returns the computed result. */
	return text;
}

/* Supports the node index operation. */
static int
node_index(
	struct graph *graph,
	const char *name,
	unsigned *result)
{
	size_t capacity;
	struct node *larger;
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < graph->count; index++) {
		/* Selects the matching value. */
		if (strcmp(graph->nodes[index].name, name) == 0) {
			*result = (unsigned)index;
			/* Reports operation failure. */
			return 1;
		}
	}

	/* Handles the graph condition. */
	if (graph->count == graph->capacity) {
				capacity = graph->capacity == 0 ? 16 : graph->capacity * 2;
				larger = realloc(graph->nodes, capacity * sizeof(*larger));

		/* Handles the larger availability. */
		if (larger == NULL)
			return 0;
		graph->nodes = larger;
		graph->capacity = capacity;
	}
	graph->nodes[graph->count].name = malloc(strlen(name) + 1);

	/* Handles the name availability. */
	if (graph->nodes[graph->count].name == NULL)
		return 0;
	strcpy(graph->nodes[graph->count].name, name);
	graph->nodes[graph->count].edges = NULL;
	graph->nodes[graph->count].edge_count = 0;
	graph->nodes[graph->count].edge_capacity = 0;
	graph->nodes[graph->count].indegree = 0;
	graph->nodes[graph->count].emitted = 0;
	*result = (unsigned)graph->count++;
	/* Reports operation failure. */
	return 1;
}

/* Supports the add edge operation. */
static int
add_edge(
	struct graph *graph,
	unsigned from,
	unsigned to)
{
	size_t capacity;
	unsigned *larger;
	struct node *node = &graph->nodes[from];
	size_t index;

	/* Handles the from condition. */
	if (from == to)
		return 1;

	/* Process each remaining element. */
	for (index = 0; index < node->edge_count; index++) {
		/* Handles the node condition. */
		if (node->edges[index] == to)
			return 1;
	}

	/* Handles the node condition. */
	if (node->edge_count == node->edge_capacity) {
				capacity = node->edge_capacity == 0 ? 4 : node->edge_capacity * 2;
				larger = realloc(node->edges, capacity * sizeof(*larger));

		/* Handles the larger availability. */
		if (larger == NULL)
			return 0;
		node->edges = larger;
		node->edge_capacity = capacity;
	}
	node->edges[node->edge_count++] = to;
	graph->nodes[to].indegree++;

	/* Reports operation failure. */
	return 1;
}

/* Supports the break cycle operation. */
static int
break_cycle(
	struct graph *graph)
{
	size_t edge;
	unsigned *state;
	unsigned *stack;
	unsigned cycle_start;
	unsigned cycle_end;
	size_t depth;
	size_t node_index_value;
	int found;

	state = calloc(graph->count, sizeof(*state));
	stack = malloc(graph->count * sizeof(*stack));
	cycle_start = 0;
	cycle_end = 0;
	depth = 0;
	found = 0;

	/* Handles the state availability. */
	if (state == NULL || stack == NULL) {
		free(state);
		free(stack);

		/* Reports successful completion. */
		return 0;
	}

	/* Process each remaining element. */
	for (node_index_value = 0; node_index_value < graph->count;
	     node_index_value++) {
		/* Handles a failed find cycle operation. */
		if (!graph->nodes[node_index_value].emitted &&
		    state[node_index_value] == 0 &&
		    find_cycle(graph, (unsigned)node_index_value, state, stack,
			       &depth, &cycle_start, &cycle_end)) {
			found = 1;
			break;
		}
	}

	/* Handles the found condition. */
	if (found) {

		fprintf(stderr, "tsort: input contains a cycle:\n");

		/* Process each remaining element. */
		for (node_index_value = cycle_start; node_index_value < depth;
		     node_index_value++)
			fprintf(stderr, "tsort: %s\n",
				graph->nodes[stack[node_index_value]].name);

		/* Process each remaining element. */
		for (edge = 0; edge < graph->nodes[cycle_end].edge_count;
		     edge++) {
			/* Handles the graph condition. */
			if (graph->nodes[cycle_end].edges[edge] ==
			    stack[cycle_start]) {
				remove_edge(graph, cycle_end, edge);
				break;
			}
		}
	}
	free(state);
	free(stack);

	/* Returns the computed result. */
	return found;
}

/* Supports the find cycle operation. */
static int
find_cycle(
	struct graph *graph,
	unsigned current,
	unsigned *state,
	unsigned *stack,
	size_t *depth,
	unsigned *cycle_start,
	unsigned *cycle_end)
{
	size_t index;
	unsigned next;
	struct node *node = &graph->nodes[current];
	size_t edge;

	/* Process each remaining element. */
	state[current] = 1;
	stack[(*depth)++] = current;
	for (edge = 0; edge < node->edge_count; edge++) {
				next = node->edges[edge];

		/* Handles the graph condition. */
		if (graph->nodes[next].emitted)
			continue;

		/* Handles a failed find cycle operation. */
		if (state[next] == 0 &&
		    find_cycle(graph, next, state, stack, depth, cycle_start,
			       cycle_end))

			/* Reports operation failure. */
			return 1;

		/* Handles the state condition. */
		if (state[next] == 1) {
			/* Process each linked entry. */
			for (index = 0; index < *depth && stack[index] != next;
			     index++)
				;
			*cycle_start = (unsigned)index;
			*cycle_end = current;
			/* Reports operation failure. */
			return 1;
		}
	}
	(*depth)--;
	state[current] = 2;

	/* Reports successful completion. */
	return 0;
}

/* Supports the remove edge operation. */
static void
remove_edge(
	struct graph *graph,
	unsigned from,
	size_t edge)
{
	struct node *node = &graph->nodes[from];
	unsigned to = node->edges[edge];

	graph->nodes[to].indegree--;
	memmove(&node->edges[edge], &node->edges[edge + 1],
		(node->edge_count - edge - 1) * sizeof(*node->edges));
	node->edge_count--;
}

/* Supports the free graph operation. */
static void
free_graph(
	struct graph *graph)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < graph->count; index++) {
		free(graph->nodes[index].name);
		free(graph->nodes[index].edges);
	}
	free(graph->nodes);
}
