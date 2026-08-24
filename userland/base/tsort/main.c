/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

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

static char *
read_token(FILE *stream)
{
	char *text;
	size_t length = 0;
	size_t capacity = 32;
	int character;

	do {
		character = fgetc(stream);
	} while (character != EOF && isspace((unsigned char)character));
	if (character == EOF)
		return NULL;
	text = malloc(capacity);
	if (text == NULL)
		return NULL;
	do {
		if (length + 1 >= capacity) {
			char *larger;
			capacity *= 2;
			larger = realloc(text, capacity);
			if (larger == NULL) {
				free(text);
				return NULL;
			}
			text = larger;
		}
		text[length++] = (char)character;
		character = fgetc(stream);
	} while (character != EOF && !isspace((unsigned char)character));
	text[length] = '\0';
	return text;
}

static int
node_index(struct graph *graph, const char *name, unsigned *result)
{
	size_t index;

	for (index = 0; index < graph->count; index++) {
		if (strcmp(graph->nodes[index].name, name) == 0) {
			*result = (unsigned)index;
			return 1;
		}
	}
	if (graph->count == graph->capacity) {
		size_t capacity =
		    graph->capacity == 0 ? 16 : graph->capacity * 2;
		struct node *larger =
		    realloc(graph->nodes, capacity * sizeof(*larger));
		if (larger == NULL)
			return 0;
		graph->nodes = larger;
		graph->capacity = capacity;
	}
	graph->nodes[graph->count].name = malloc(strlen(name) + 1);
	if (graph->nodes[graph->count].name == NULL)
		return 0;
	strcpy(graph->nodes[graph->count].name, name);
	graph->nodes[graph->count].edges = NULL;
	graph->nodes[graph->count].edge_count = 0;
	graph->nodes[graph->count].edge_capacity = 0;
	graph->nodes[graph->count].indegree = 0;
	graph->nodes[graph->count].emitted = 0;
	*result = (unsigned)graph->count++;
	return 1;
}

static int
add_edge(struct graph *graph, unsigned from, unsigned to)
{
	struct node *node = &graph->nodes[from];
	size_t index;

	if (from == to)
		return 1;
	for (index = 0; index < node->edge_count; index++) {
		if (node->edges[index] == to)
			return 1;
	}
	if (node->edge_count == node->edge_capacity) {
		size_t capacity =
		    node->edge_capacity == 0 ? 4 : node->edge_capacity * 2;
		unsigned *larger =
		    realloc(node->edges, capacity * sizeof(*larger));
		if (larger == NULL)
			return 0;
		node->edges = larger;
		node->edge_capacity = capacity;
	}
	node->edges[node->edge_count++] = to;
	graph->nodes[to].indegree++;
	return 1;
}

static void
remove_edge(struct graph *graph, unsigned from, size_t edge)
{
	struct node *node = &graph->nodes[from];
	unsigned to = node->edges[edge];

	graph->nodes[to].indegree--;
	memmove(&node->edges[edge], &node->edges[edge + 1],
		(node->edge_count - edge - 1) * sizeof(*node->edges));
	node->edge_count--;
}

static int
find_cycle(struct graph *graph, unsigned current, unsigned *state,
	   unsigned *stack, size_t *depth, unsigned *cycle_start,
	   unsigned *cycle_end)
{
	struct node *node = &graph->nodes[current];
	size_t edge;

	state[current] = 1;
	stack[(*depth)++] = current;
	for (edge = 0; edge < node->edge_count; edge++) {
		unsigned next = node->edges[edge];
		if (graph->nodes[next].emitted)
			continue;
		if (state[next] == 0 &&
		    find_cycle(graph, next, state, stack, depth, cycle_start,
			       cycle_end))
			return 1;
		if (state[next] == 1) {
			size_t index;
			for (index = 0; index < *depth && stack[index] != next;
			     index++)
				;
			*cycle_start = (unsigned)index;
			*cycle_end = current;
			return 1;
		}
	}
	(*depth)--;
	state[current] = 2;
	return 0;
}

static int
break_cycle(struct graph *graph)
{
	unsigned *state = calloc(graph->count, sizeof(*state));
	unsigned *stack = malloc(graph->count * sizeof(*stack));
	unsigned cycle_start = 0;
	unsigned cycle_end = 0;
	size_t depth = 0;
	size_t node_index_value;
	int found = 0;

	if (state == NULL || stack == NULL) {
		free(state);
		free(stack);
		return 0;
	}
	for (node_index_value = 0; node_index_value < graph->count;
	     node_index_value++) {
		if (!graph->nodes[node_index_value].emitted &&
		    state[node_index_value] == 0 &&
		    find_cycle(graph, (unsigned)node_index_value, state, stack,
			       &depth, &cycle_start, &cycle_end)) {
			found = 1;
			break;
		}
	}
	if (found) {
		size_t edge;
		fprintf(stderr, "tsort: input contains a cycle:\n");
		for (node_index_value = cycle_start; node_index_value < depth;
		     node_index_value++)
			fprintf(stderr, "tsort: %s\n",
				graph->nodes[stack[node_index_value]].name);
		for (edge = 0; edge < graph->nodes[cycle_end].edge_count;
		     edge++) {
			if (graph->nodes[cycle_end].edges[edge] ==
			    stack[cycle_start]) {
				remove_edge(graph, cycle_end, edge);
				break;
			}
		}
	}
	free(state);
	free(stack);
	return found;
}

static void
free_graph(struct graph *graph)
{
	size_t index;

	for (index = 0; index < graph->count; index++) {
		free(graph->nodes[index].name);
		free(graph->nodes[index].edges);
	}
	free(graph->nodes);
}

int
main(int argc, char **argv)
{
	struct graph graph = {0};
	FILE *input = stdin;
	const char *path = NULL;
	unsigned cycles = 0;
	int count_cycles = 0;
	int index = 1;
	int status = 0;
	size_t emitted = 0;

	if (index < argc && strcmp(argv[index], "-w") == 0) {
		count_cycles = 1;
		index++;
	}
	if (index < argc && strcmp(argv[index], "--") == 0)
		index++;
	if (argc - index > 1) {
		fprintf(stderr, "usage: tsort [-w] [file]\n");
		return 125;
	}
	if (index < argc && strcmp(argv[index], "-") != 0) {
		path = argv[index];
		input = fopen(path, "r");
		if (input == NULL) {
			fprintf(stderr, "tsort: %s: %s\n", path,
				strerror(errno));
			return 125;
		}
	}
	for (;;) {
		char *left = read_token(input);
		char *right;
		unsigned from;
		unsigned to;

		if (left == NULL)
			break;
		right = read_token(input);
		if (right == NULL) {
			fprintf(stderr, "tsort: odd number of input fields\n");
			free(left);
			status = 125;
			goto done;
		}
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
	if (ferror(input)) {
		fprintf(stderr, "tsort: input error\n");
		status = 125;
		goto done;
	}
	while (emitted < graph.count) {
		size_t node_index_value;
		for (node_index_value = 0; node_index_value < graph.count;
		     node_index_value++) {
			struct node *node = &graph.nodes[node_index_value];
			size_t edge;
			if (node->emitted || node->indegree != 0)
				continue;
			puts(node->name);
			node->emitted = 1;
			emitted++;
			for (edge = 0; edge < node->edge_count; edge++)
				graph.nodes[node->edges[edge]].indegree--;
			break;
		}
		if (node_index_value == graph.count) {
			if (!break_cycle(&graph)) {
				fprintf(stderr,
					"tsort: cannot resolve input graph\n");
				status = 125;
				goto done;
			}
			if (cycles < CYCLE_STATUS_MAX)
				cycles++;
		}
	}
	if (ferror(stdout))
		status = 125;
	else if (cycles != 0)
		status = count_cycles ? (int)cycles : 1;
done:
	if (input != stdin && fclose(input) != 0 && status == 0)
		status = 125;
	free_graph(&graph);
	return status;
}
