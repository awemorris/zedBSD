/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Fragment program scheduling.
 *
 * The lowering records one operation or signal at a time, with conservative
 * idle words after every operation whose result arrives late.  A fragment
 * program runs once per pixel, so its records are scheduled here instead:
 * one native word carries an add ALU operation, a multiply ALU operation and
 * a signal together, and the idle words become explicit distances between
 * the records that need them.  The timing rules are hardware facts of V3D
 * 4.2; this file applies them
 * independently.  A finished schedule is checked again by simulating the
 * value each source reads; a program the rules do not cover, or a schedule
 * the check refuses, keeps the source-order words.
 */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/compiler/shader-private.h"

/* Register-file entries keep their numbers; accumulators zero to five and the lane flags follow them. */
#define SCHEDULE_ACCUMULATOR 64U
#define SCHEDULE_R5 69U
#define SCHEDULE_FLAGS 70U
#define SCHEDULE_RESOURCES 71U

/* A record reads at most two sources and the lane flags; it writes at most a register, accumulator five and the flags. */
#define SCHEDULE_ACCESSES 3U

/* A special function's result is read three words after the function starts, and the next one starts no sooner. */
#define SCHEDULE_SPECIAL_DISTANCE 3U

/* A thread switch takes effect after its two delay words; what must follow it waits three words. */
#define SCHEDULE_SWITCH_DISTANCE 3U

/* Accumulators zero to three take short-lived values. */
#define SCHEDULE_ACCUMULATORS 4U

/*
 * Schedules tried for each program: registers only, then accumulators for
 * values living at most 4, 12, 24 and 1024 records (spans in the table).
 */
#define SCHEDULE_ATTEMPTS 5U

/* The words a schedule may use for each record before it is refused, besides a fixed tail. */
#define SCHEDULE_WORDS_PER_NODE 4U
#define SCHEDULE_TAIL_WORDS 16U

/*
 * One recorded operation or signal taking part in the schedule.
 *
 * Nodes follow the source order of their records, which is a topological
 * order of the dependence edges: every edge leads from an earlier node to a
 * later one.
 */
struct schedule_node {
	uint32_t op;
	/* The ALU the operation needs; none for a signal. */
	enum bcm2711_qpu_unit unit;
	uint32_t is_signal;
	uint32_t special;
	uint32_t peripheral;
	uint32_t ordered;
	uint32_t sensitive;
	uint32_t uniform;
	uint32_t texture_read;
	uint32_t texture_write;
	uint32_t texture_config;
	uint32_t tile_write;
	enum bcm2711_shader_switch role;
	/* The last switch pair and the tile writes are placed after the body of the program. */
	uint32_t tail;

	/* Resources read, and resources written with the words after issue at which each write lands. */
	uint32_t reads[SCHEDULE_ACCESSES];
	uint32_t read_count;
	uint32_t writes[SCHEDULE_ACCESSES];
	uint32_t landing[SCHEDULE_ACCESSES];
	uint32_t write_count;

	/* The word the node is placed in, or BCM2711_SHADER_ABSENT. */
	uint32_t word;
	uint32_t priority;
	uint32_t waiting;
	uint32_t earliest;
	uint32_t first_successor;
	uint32_t successor_count;
};

/* One dependence: the later node is placed at least distance words after the earlier one. */
struct schedule_edge {
	uint32_t from;
	uint32_t to;
	uint32_t distance;
};

/* One native word being filled: the node in each slot, or BCM2711_SHADER_ABSENT. */
struct schedule_word {
	uint32_t add;
	uint32_t multiply;
	uint32_t signal;
	uint32_t thread_switch;
};

/* One read of a resource since the resource was last written, kept in a per-resource list. */
struct schedule_reader {
	uint32_t node;
	uint32_t next;
};

/* One read of a register value that may move to an accumulator: the record and which source. */
struct promote_use {
	uint32_t op;
	uint32_t source;
	uint32_t next;
};

/*
 * One register value from its write to its last read, a candidate for an accumulator.
 *
 * Positions count non-idle records in source order.
 */
struct promote_value {
	uint32_t op;
	uint32_t first;
	uint32_t last;
	uint32_t segment;
	uint32_t eligible;
	uint32_t uses;
};

/*
 * One scheduling attempt's storage.
 *
 * Everything is allocated by bcm2711_shader_schedule and released before it
 * returns; nothing is published until the whole schedule has been checked.
 */
struct schedule_state {
	struct bcm2711_shader_compiler *compiler;
	struct schedule_node *nodes;
	uint32_t node_count;
	struct schedule_edge *edges;
	uint32_t edge_count;
	uint32_t edge_capacity;
	uint32_t *successors;
	struct schedule_word *words;
	uint32_t word_count;
	uint32_t word_capacity;
	struct schedule_reader *readers;
	uint32_t reader_count;
	uint32_t reader_capacity;
	uint32_t reader_head[SCHEDULE_RESOURCES];
	uint32_t last_writer[SCHEDULE_RESOURCES];
	uint32_t *pending_sensitive;
	uint32_t pending_count;
	uint32_t last_first;
	uint32_t last_second;
	uint32_t end;
	/* The two tile writes, red-green then blue-alpha. */
	uint32_t tile_nodes[2];
};

static int schedule_attempt(struct bcm2711_shader_compiler *compiler, uint32_t span);
static int promote_accumulators(struct bcm2711_shader_compiler *compiler, uint32_t span);
static int promote_close(struct promote_value *open, struct promote_value *values, uint32_t *value_count, uint32_t capacity, uint32_t span);
static void promote_assign(struct bcm2711_shader_compiler *compiler, struct promote_value *values, uint32_t value_count, const struct promote_use *uses);
static uint32_t peripherals_clash(const struct schedule_node *first, const struct schedule_node *second);
static uint32_t texture_pair(const struct schedule_node *first, const struct schedule_node *second);
static int classify(struct schedule_state *state);
static int classify_node(struct schedule_state *state, struct schedule_node *node);
static int build_edges(struct schedule_state *state);
static int add_edge(struct schedule_state *state, uint32_t from, uint32_t to, uint32_t distance);
static int resource_edges(struct schedule_state *state, uint32_t index);
static int order_edges(struct schedule_state *state, uint32_t index, uint32_t *last_special, uint32_t *last_ordered, uint32_t *after_switch);
static int link_successors(struct schedule_state *state);
static void set_priorities(struct schedule_state *state);
static int schedule_body(struct schedule_state *state);
static void place(struct schedule_state *state, uint32_t index, uint32_t word, uint32_t slot);
static uint32_t place_sample_switch(struct schedule_state *state, uint32_t launch, uint32_t word);
static uint32_t switch_lowest(const struct schedule_state *state, uint32_t target, uint32_t launch);
static uint32_t finishes_in_window(const struct schedule_node *node);
static int fits(const struct schedule_state *state, const struct schedule_word *word, uint32_t index, uint32_t *slot);
static int word_encode(const struct schedule_state *state, const struct schedule_word *word, uint64_t *encoded);
static uint32_t word_node_count(const struct schedule_word *word, uint32_t *nodes);
static int schedule_tail(struct schedule_state *state);
static int tail_trial(struct schedule_state *state, uint32_t first, uint32_t *tiles, uint32_t *end, uint32_t *length);
static uint32_t ready_word(const struct schedule_state *state, uint32_t index);
static void trial_word(const struct schedule_state *state, uint32_t word, const uint32_t *tiles, uint32_t tile_count, struct schedule_word *trial);
static int switch_fits(const struct schedule_state *state, uint32_t word, uint32_t index);
static int ends_in(const struct schedule_state *state, uint32_t end, uint32_t used, const uint32_t *tiles);
static int node_allows_end(const struct schedule_state *state, uint32_t index, uint32_t slot);
static int grow_words(struct schedule_state *state, uint32_t count);
static int verify(struct schedule_state *state);
static int verify_values(struct schedule_state *state);
static int verify_rules(struct schedule_state *state);
static int emit_words(struct schedule_state *state);
static void release_state(struct schedule_state *state);

/*
 * Schedules a recorded fragment program into paired native words.
 *
 * The program is scheduled with values in registers only and with
 * short-lived values in accumulators, for several lifetimes counted as
 * short, and the shortest program is kept: an accumulator takes no read
 * port, so more operations pair, but it cannot cross a thread switch and
 * reusing one orders the operations around it.  A nonzero result means the
 * program keeps its source-order words: the program uses a transaction these
 * rules do not describe, or storage ran out, or every finished schedule
 * failed its independent check.
 */
int
bcm2711_shader_schedule(
	struct bcm2711_shader_compiler *compiler)
{
	static const uint32_t spans[SCHEDULE_ATTEMPTS] = {0, 4, 12, 24, 1024};
	struct bcm2711_shader_binary *binary;
	struct bcm2711_qpu_instruction *saved;
	uint64_t *best_code;
	struct bcm2711_shader_uniform *best_uniforms;
	uint32_t best_count;
	uint32_t attempt;
	uint32_t index;
	int outcome;
	int error;

	/* The records are kept so each attempt, and a refusal, start from the source-order program. */
	binary = compiler->binary;
	saved = kern_calloc(compiler->op_count, sizeof(*saved));
	if (saved == NULL)
		return ENOMEM;
	for (index = 0; index < compiler->op_count; index++)
		saved[index] = compiler->ops[index].instruction;

	/* The best program so far is kept aside while the other attempts run. */
	best_code = kern_calloc(compiler->code_capacity, sizeof(*best_code));
	best_uniforms = kern_calloc(compiler->uniform_capacity, sizeof(*best_uniforms));
	if (best_code == NULL || best_uniforms == NULL) {
		kern_free(best_uniforms);
		kern_free(best_code);
		kern_free(saved);
		return ENOMEM;
	}

	/* Each attempt starts from the saved records; a shorter program replaces the best one. */
	best_count = 0;
	outcome = ENOTSUP;
	error = 0;
	for (attempt = 0; attempt < SCHEDULE_ATTEMPTS && error != ENOMEM; attempt++) {
		error = schedule_attempt(compiler, spans[attempt]);
		for (index = 0; index < compiler->op_count; index++)
			compiler->ops[index].instruction = saved[index];
		if (error != 0)
			continue;
		if (best_count == 0 || binary->code_count < best_count) {
			best_count = binary->code_count;
			kern_memcpy(best_code, binary->code, best_count * sizeof(*best_code));
			kern_memcpy(best_uniforms, binary->uniforms, binary->uniform_count * sizeof(*best_uniforms));
			outcome = 0;
		}
	}

	/* The best program returns to the binary; every attempt consumed the same uniform count. */
	if (outcome == 0 && error != ENOMEM) {
		kern_memcpy(binary->code, best_code, best_count * sizeof(*best_code));
		kern_memcpy(binary->uniforms, best_uniforms, binary->uniform_count * sizeof(*best_uniforms));
		binary->code_count = best_count;
	}

	/* The kept copies and saved records retire. */
	kern_free(best_uniforms);
	kern_free(best_code);
	kern_free(saved);

	/* Running out of memory fails the compile; refused attempts leave the source-order words to the caller. */
	if (error == ENOMEM)
		return ENOMEM;
	if (outcome != 0)
		return outcome;

	/* Succeeded: the program's words and uniform stream are scheduled. */
	return 0;
}

/* Runs one scheduling attempt, writing the program's words and uniforms when it succeeds. */
static int
schedule_attempt(
	struct bcm2711_shader_compiler *compiler,
	uint32_t span)
{
	struct schedule_state state;
	int error;

	/* All storage of the attempt hangs off one state, released on every path. */
	kern_memset(&state, 0, sizeof(state));
	state.compiler = compiler;
	state.last_first = BCM2711_SHADER_ABSENT;
	state.last_second = BCM2711_SHADER_ABSENT;
	state.end = BCM2711_SHADER_ABSENT;

	/* Values living at most span records may move to accumulators, which take no read port; then every record is classified. */
	error = 0;
	if (span != 0)
		error = promote_accumulators(compiler, span);
	if (error == 0)
		error = classify(&state);
	if (error == 0)
		error = build_edges(&state);
	if (error == 0)
		error = link_successors(&state);

	/* Places the body by priority, then the last switch, tile writes and thread end. */
	if (error == 0) {
		set_priorities(&state);
		error = schedule_body(&state);
	}

	/* The tail follows the placed body. */
	if (error == 0)
		error = schedule_tail(&state);

	/* The finished schedule must pass the independent check before any word is written. */
	if (error == 0)
		error = verify(&state);
	if (error == 0)
		error = emit_words(&state);

	/* The attempt's storage retires whatever the outcome. */
	release_state(&state);

	/* A refused attempt reports why. */
	if (error != 0)
		return error;

	/* Succeeded: the program's words and uniform stream follow this attempt. */
	return 0;
}

/*
 * Moves short-lived register values to accumulators zero to three.
 *
 * A value qualifies when an unpredicated ALU operation other than a special
 * function writes it, plain ALU operations read it, its last read is close
 * in source order, and no thread switch lies between: accumulators are lost
 * at a switch.  The schedule's edges treat accumulators like registers, so
 * the rewritten program keeps its meaning however the words are ordered.
 */
static int
promote_accumulators(
	struct bcm2711_shader_compiler *compiler,
	uint32_t span)
{
	struct bcm2711_shader_op *op;
	const struct bcm2711_qpu_instruction *instruction;
	const struct bcm2711_qpu_source *sources[2];
	struct promote_value open[64];
	struct promote_value *values;
	struct promote_use *uses;
	uint32_t value_count;
	uint32_t use_count;
	uint32_t index;
	uint32_t source;
	uint32_t source_count;
	uint32_t number;
	uint32_t position;
	uint32_t segment;
	int error;

	/* At most one value per record and two reads per record. */
	values = kern_calloc(compiler->op_count, sizeof(*values));
	if (values == NULL)
		return ENOMEM;
	uses = kern_calloc((size_t)compiler->op_count * 2U, sizeof(*uses));
	if (uses == NULL) {
		kern_free(values);
		return ENOMEM;
	}

	/* No register has a current value yet. */
	for (number = 0; number < 64; number++)
		open[number].op = BCM2711_SHADER_ABSENT;

	/* Walks the records, following each register's current value. */
	value_count = 0;
	use_count = 0;
	position = 0;
	segment = 0;
	error = 0;
	for (index = 0; index < compiler->op_count && error == 0; index++) {
		op = &compiler->ops[index];
		instruction = &op->instruction;
		if (instruction->operation == BCM2711_QPU_IDLE && instruction->signal == BCM2711_QPU_SIGNAL_NONE)
			continue;
		position++;

		/* A switch ends the segment in which a value may live in an accumulator. */
		if (instruction->signal == BCM2711_QPU_SIGNAL_SWITCH)
			segment++;

		/* Each register read joins its value; a value read across a switch or by a peripheral write stays in its register. */
		if (instruction->operation != BCM2711_QPU_IDLE) {
			sources[0] = &instruction->left;
			sources[1] = &instruction->right;
			source_count = bcm2711_qpu_operation_sources(instruction->operation);
			for (source = 0; source < source_count; source++) {
				if (sources[source]->kind != BCM2711_QPU_REGISTER || sources[source]->number >= 64)
					continue;
				number = sources[source]->number;
				if (open[number].op == BCM2711_SHADER_ABSENT)
					continue;
				if (open[number].segment != segment || instruction->destination.peripheral != 0)
					open[number].eligible = 0;
				uses[use_count].op = index;
				uses[use_count].source = source;
				uses[use_count].next = open[number].uses;
				open[number].uses = use_count;
				use_count++;
				open[number].last = position;
			}
		}

		/* A predicated write merges with the old value, which therefore stays in its register. */
		if (instruction->operation != BCM2711_QPU_IDLE &&
		    instruction->destination.peripheral == 0 &&
		    instruction->predicate != 0) {
			number = instruction->destination.number;
			if (number < 64 && open[number].op != BCM2711_SHADER_ABSENT)
				open[number].eligible = 0;
			continue;
		}

		/* A write starts a new value; only a plain ALU result may move to an accumulator. */
		number = 64;
		if (instruction->operation != BCM2711_QPU_IDLE && instruction->destination.peripheral == 0)
			number = instruction->destination.number;
		if (instruction->operation == BCM2711_QPU_IDLE &&
		    instruction->signal != BCM2711_QPU_SIGNAL_NONE &&
		    instruction->signal_destination.peripheral == 0 &&
		    (instruction->signal == BCM2711_QPU_SIGNAL_UNIFORM_REGISTER ||
		     instruction->signal == BCM2711_QPU_SIGNAL_TEXTURE ||
		     instruction->signal == BCM2711_QPU_SIGNAL_VARYING))
			number = instruction->signal_destination.number;
		if (number >= 64)
			continue;
		error = promote_close(&open[number], values, &value_count, compiler->op_count, span);
		open[number].op = index;
		open[number].first = position;
		open[number].last = position;
		open[number].segment = segment;
		open[number].uses = BCM2711_SHADER_ABSENT;
		open[number].eligible = 1;
		if (instruction->operation == BCM2711_QPU_IDLE)
			open[number].eligible = 0;
		switch (instruction->operation) {
		case BCM2711_QPU_FLOAT_RECIPROCAL:
		case BCM2711_QPU_FLOAT_INVERSE_ROOT:
		case BCM2711_QPU_FLOAT_EXPONENT:
		case BCM2711_QPU_FLOAT_LOGARITHM:
		case BCM2711_QPU_FLOAT_SINE:
			open[number].eligible = 0;
			break;
		default:
			break;
		}
	}

	/* The values still open at the end of the program close too. */
	for (number = 0; number < 64 && error == 0; number++)
		error = promote_close(&open[number], values, &value_count, compiler->op_count, span);

	/* Qualifying values take accumulators in source order. */
	if (error == 0)
		promote_assign(compiler, values, value_count, uses);
	kern_free(uses);
	kern_free(values);

	/* A full value table refuses the schedule. */
	if (error != 0)
		return error;

	/* Succeeded: short-lived values read accumulators. */
	return 0;
}

/* Keeps one closed register value when it qualifies for an accumulator. */
static int
promote_close(
	struct promote_value *open,
	struct promote_value *values,
	uint32_t *value_count,
	uint32_t capacity,
	uint32_t span)
{
	/* An unwritten register has no value to close. */
	if (open->op == BCM2711_SHADER_ABSENT)
		return 0;

	/* A value with a read, all of it close and inside one segment, qualifies. */
	if (open->eligible != 0 &&
	    open->uses != BCM2711_SHADER_ABSENT &&
	    open->last - open->first <= span) {
		if (*value_count == capacity)
			return E2BIG;
		values[*value_count] = *open;
		(*value_count)++;
	}

	/* The register's value is closed either way. */
	open->op = BCM2711_SHADER_ABSENT;

	/* Succeeded: the value is closed. */
	return 0;
}

/* Gives qualifying values free accumulators in source order and rewrites their writes and reads. */
static void
promote_assign(
	struct bcm2711_shader_compiler *compiler,
	struct promote_value *values,
	uint32_t value_count,
	const struct promote_use *uses)
{
	struct promote_value moved;
	struct bcm2711_qpu_instruction *instruction;
	uint32_t free_from[SCHEDULE_ACCUMULATORS];
	uint32_t index;
	uint32_t later;
	uint32_t accumulator;
	uint32_t chosen;
	uint32_t entry;

	/* Values close in an arbitrary order; assignment walks them by their write. */
	for (index = 1; index < value_count; index++) {
		moved = values[index];
		later = index;
		while (later > 0 && values[later - 1].first > moved.first) {
			values[later] = values[later - 1];
			later--;
		}

		/* The moved value takes the gap. */
		values[later] = moved;
	}

	/* Each accumulator is free from its last value's last read; the one free longest is chosen. */
	for (accumulator = 0; accumulator < SCHEDULE_ACCUMULATORS; accumulator++)
		free_from[accumulator] = 0;
	for (index = 0; index < value_count; index++) {
		chosen = SCHEDULE_ACCUMULATORS;
		for (accumulator = 0; accumulator < SCHEDULE_ACCUMULATORS; accumulator++) {
			if (free_from[accumulator] > values[index].first)
				continue;
			if (chosen == SCHEDULE_ACCUMULATORS || free_from[accumulator] < free_from[chosen])
				chosen = accumulator;
		}

		/* A value with no free accumulator stays in its register. */
		if (chosen == SCHEDULE_ACCUMULATORS)
			continue;
		free_from[chosen] = values[index].last;

		/* The write goes to the accumulator and every read of the value reads it. */
		instruction = &compiler->ops[values[index].op].instruction;
		instruction->destination.number = chosen;
		instruction->destination.peripheral = 1;
		entry = values[index].uses;
		while (entry != BCM2711_SHADER_ABSENT) {
			instruction = &compiler->ops[uses[entry].op].instruction;
			if (uses[entry].source == 0) {
				instruction->left.kind = BCM2711_QPU_ACCUMULATOR;
				instruction->left.number = chosen;
			} else {
				instruction->right.kind = BCM2711_QPU_ACCUMULATOR;
				instruction->right.number = chosen;
			}

			/* Follows the value's next read. */
			entry = uses[entry].next;
		}
	}

	/* Succeeded: qualifying values live in accumulators. */
	return;
}

/*
 * Tells whether two nodes' peripheral transactions cannot share a word.
 *
 * A word starts one transaction of the TMU, special functions or the tile
 * buffer, except that a texture configuration may accompany a TMU coordinate
 * write other than the configuration register's.
 */
static uint32_t
peripherals_clash(
	const struct schedule_node *first,
	const struct schedule_node *second)
{
	uint32_t pair;

	/* Two nodes clash only when both start a transaction. */
	if (first->peripheral == 0 || second->peripheral == 0)
		return 0;

	/* The one permitted pair. */
	pair = texture_pair(first, second);
	if (pair != 0)
		return 0;

	/* Any other pair of transactions needs two words. */
	return 1;
}

/* Tells whether two nodes are a texture configuration and a TMU coordinate write, which may share a word. */
static uint32_t
texture_pair(
	const struct schedule_node *first,
	const struct schedule_node *second)
{
	/* Either order of the two transactions. */
	if (first->texture_config != 0 && second->texture_write != 0)
		return 1;
	if (first->texture_write != 0 && second->texture_config != 0)
		return 1;

	/* Any other two nodes are not that pair. */
	return 0;
}

/* Creates one node per non-idle record and checks the program's switch structure. */
static int
classify(
	struct schedule_state *state)
{
	struct bcm2711_shader_compiler *compiler;
	struct bcm2711_shader_op *op;
	struct schedule_node *node;
	uint32_t index;
	uint32_t after_pair;
	int error;

	/* Node storage has one entry per record at most. */
	compiler = state->compiler;
	if (compiler->op_count == 0)
		return EINVAL;
	state->nodes = kern_calloc(compiler->op_count, sizeof(*state->nodes));
	if (state->nodes == NULL)
		return ENOMEM;

	/* Idle records are the lowering's conservative delays; the schedule replaces them by distances. */
	after_pair = 0;
	for (index = 0; index < compiler->op_count; index++) {
		op = &compiler->ops[index];
		if (op->instruction.operation == BCM2711_QPU_IDLE && op->instruction.signal == BCM2711_QPU_SIGNAL_NONE)
			continue;
		node = &state->nodes[state->node_count];
		node->op = index;
		node->word = BCM2711_SHADER_ABSENT;
		error = classify_node(state, node);
		if (error != 0)
			return error;

		/* The thread end is the last record; nothing may follow it. */
		if (state->end != BCM2711_SHADER_ABSENT)
			return ENOTSUP;

		/* The two switches of the last pair are consecutive records, each the only one of its kind. */
		if (node->role == BCM2711_SHADER_SWITCH_LAST_FIRST) {
			if (state->last_first != BCM2711_SHADER_ABSENT)
				return ENOTSUP;
			state->last_first = state->node_count;
			node->tail = 1;
			after_pair = 1;
		} else if (node->role == BCM2711_SHADER_SWITCH_LAST_SECOND) {
			if (state->last_first != state->node_count - 1 || state->last_second != BCM2711_SHADER_ABSENT)
				return ENOTSUP;
			state->last_second = state->node_count;
			node->tail = 1;
		} else if (node->role == BCM2711_SHADER_SWITCH_END) {
			state->end = state->node_count;
		} else if (node->tile_write != 0) {
			/* Tile writes come only after the last switch pair. */
			if (state->last_second == BCM2711_SHADER_ABSENT)
				return ENOTSUP;
			node->tail = 1;
		} else if (after_pair != 0 &&
			   (node->sensitive != 0 ||
			    node->ordered != 0)) {
			/* After the last pair, only plain operations may join the body. */
			return ENOTSUP;
		}

		/* The node joins the schedule. */
		state->node_count++;
	}

	/* A fragment program has exactly one last pair and ends with its thread end. */
	if (state->last_second == BCM2711_SHADER_ABSENT || state->end == BCM2711_SHADER_ABSENT)
		return ENOTSUP;

	/* Succeeded: every record has a node and the switch structure is the one these rules describe. */
	return 0;
}

/* Fills one node's resource accesses and transaction classes from its record. */
static int
classify_node(
	struct schedule_state *state,
	struct schedule_node *node)
{
	const struct bcm2711_shader_op *op;
	const struct bcm2711_qpu_instruction *instruction;
	const struct bcm2711_qpu_source *sources[2];
	uint32_t source_count;
	uint32_t index;
	uint32_t number;

	/* The record's operation or signal decides the node's classes. */
	op = &state->compiler->ops[node->op];
	instruction = &op->instruction;
	node->role = op->role;
	node->uniform = op->has_uniform;

	/* A record is either one ALU operation or one signal on an idle word. */
	if (instruction->operation != BCM2711_QPU_IDLE && instruction->signal != BCM2711_QPU_SIGNAL_NONE)
		return ENOTSUP;

	/* Signals: switches, addressed loads and texture configuration are described; others are not. */
	if (instruction->operation == BCM2711_QPU_IDLE) {
		node->is_signal = 1;
		node->unit = BCM2711_QPU_UNIT_NONE;
		switch (instruction->signal) {
		case BCM2711_QPU_SIGNAL_SWITCH:
			if (op->role == BCM2711_SHADER_SWITCH_NONE)
				return ENOTSUP;
			node->ordered = 1;
			break;
		case BCM2711_QPU_SIGNAL_UNIFORM_REGISTER:
			break;
		case BCM2711_QPU_SIGNAL_TEXTURE_CONFIG:
			node->peripheral = 1;
			node->ordered = 1;
			node->texture_config = 1;
			break;
		case BCM2711_QPU_SIGNAL_TEXTURE:
			node->peripheral = 1;
			node->ordered = 1;
			node->texture_read = 1;
			break;
		case BCM2711_QPU_SIGNAL_VARYING:
			node->sensitive = 1;
			break;
		default:
			return ENOTSUP;
		}

		/* An addressed load writes its register when its word ends. */
		if (instruction->signal == BCM2711_QPU_SIGNAL_UNIFORM_REGISTER ||
		    instruction->signal == BCM2711_QPU_SIGNAL_TEXTURE ||
		    instruction->signal == BCM2711_QPU_SIGNAL_VARYING) {
			if (instruction->signal_destination.peripheral != 0 || instruction->signal_destination.number >= 64)
				return ENOTSUP;
			node->writes[node->write_count] = instruction->signal_destination.number;
			node->landing[node->write_count] = 0;
			node->write_count++;
		}

		/* A varying also fills accumulator five a word later. */
		if (instruction->signal == BCM2711_QPU_SIGNAL_VARYING) {
			node->writes[node->write_count] = SCHEDULE_R5;
			node->landing[node->write_count] = 1;
			node->write_count++;
		}

		/* Succeeded: the signal's accesses and classes are known. */
		return 0;
	}

	/* Vertex pipeline and texture wait operations have no rules here. */
	switch (instruction->operation) {
	case BCM2711_QPU_VPM_LOAD:
	case BCM2711_QPU_VPM_STORE:
	case BCM2711_QPU_VPM_WAIT:
	case BCM2711_QPU_TEXTURE_WAIT:
		return ENOTSUP;
	case BCM2711_QPU_FLOAT_RECIPROCAL:
	case BCM2711_QPU_FLOAT_INVERSE_ROOT:
	case BCM2711_QPU_FLOAT_EXPONENT:
	case BCM2711_QPU_FLOAT_LOGARITHM:
	case BCM2711_QPU_FLOAT_SINE:
		node->special = 1;
		node->peripheral = 1;
		node->sensitive = 1;
		break;
	case BCM2711_QPU_SAMPLE_MASK:
		/* The coverage it clears must be set before the tile writes that it masks, which keep the order of transactions. */
		node->ordered = 1;
		break;
	default:
		break;
	}

	/* The operation's ALU decides its slot. */
	node->unit = bcm2711_qpu_operation_unit(instruction->operation);
	if (node->unit == BCM2711_QPU_UNIT_NONE)
		return ENOTSUP;

	/* Register and accumulator five sources are reads; small constants are not. */
	sources[0] = &instruction->left;
	sources[1] = &instruction->right;
	source_count = bcm2711_qpu_operation_sources(instruction->operation);
	for (index = 0; index < source_count; index++) {
		if (sources[index]->kind == BCM2711_QPU_REGISTER) {
			if (sources[index]->number >= 64)
				return ENOTSUP;
			node->reads[node->read_count] = sources[index]->number;
			node->read_count++;
		} else if (sources[index]->kind == BCM2711_QPU_ACCUMULATOR) {
			/* Accumulators zero to three hold short-lived values; five holds the varying coefficient. */
			if (sources[index]->number == 4 || sources[index]->number > 5)
				return ENOTSUP;
			node->reads[node->read_count] = SCHEDULE_ACCUMULATOR + sources[index]->number;
			node->read_count++;
			node->sensitive = 1;
		}
	}

	/* A predicated write reads the lane flags. */
	if (instruction->predicate != 0) {
		node->reads[node->read_count] = SCHEDULE_FLAGS;
		node->read_count++;
		node->sensitive = 1;
	}

	/* A register destination is written when the word ends, or two words later for a special function. */
	if (instruction->destination.peripheral == 0) {
		node->writes[node->write_count] = instruction->destination.number;
		node->landing[node->write_count] = 0;
		if (node->special != 0)
			node->landing[node->write_count] = SCHEDULE_SPECIAL_DISTANCE - 1;
		node->write_count++;
	} else {
		/* Peripheral destinations: accumulators, the null address, the tile buffer and the TMU are described. */
		number = instruction->destination.number;
		if (number <= 3) {
			/* An accumulator is written when the word ends and is lost at a thread switch. */
			if (node->special != 0)
				return ENOTSUP;
			node->writes[node->write_count] = SCHEDULE_ACCUMULATOR + number;
			node->landing[node->write_count] = 0;
			node->write_count++;
			node->sensitive = 1;
		} else if (number == 7 || number == 8) {
			node->peripheral = 1;
			node->ordered = 1;
			node->tile_write = 1;
		} else if (number >= 32 && number <= 44) {
			node->peripheral = 1;
			node->ordered = 1;
			if (number != 32)
				node->texture_write = 1;
		} else if (number != 6) {
			return ENOTSUP;
		}
	}

	/* A flag push writes the lane flags when the word ends. */
	if (instruction->push_flags != 0) {
		node->writes[node->write_count] = SCHEDULE_FLAGS;
		node->landing[node->write_count] = 0;
		node->write_count++;
		node->sensitive = 1;
	}

	/* Succeeded: the operation's accesses and classes are known. */
	return 0;
}

/* Builds the dependence edges, counting them first so one allocation holds them all. */
static int
build_edges(
	struct schedule_state *state)
{
	uint32_t pass;
	uint32_t index;
	uint32_t last_special;
	uint32_t last_ordered;
	uint32_t after_switch;
	int error;

	/* Reader lists and pending sensitive nodes hold at most one entry per access and per node. */
	state->reader_capacity = state->node_count * SCHEDULE_ACCESSES;
	state->readers = kern_calloc(state->reader_capacity, sizeof(*state->readers));
	if (state->readers == NULL)
		return ENOMEM;
	state->pending_sensitive = kern_calloc(state->node_count, sizeof(*state->pending_sensitive));
	if (state->pending_sensitive == NULL)
		return ENOMEM;

	/* The first pass counts edges into a null array; the second stores them. */
	for (pass = 0; pass < 2; pass++) {
		if (pass == 1) {
			state->edge_capacity = state->edge_count;
			state->edge_count = 0;
			if (state->edge_capacity != 0) {
				state->edges = kern_calloc(state->edge_capacity, sizeof(*state->edges));
				if (state->edges == NULL)
					return ENOMEM;
			}
		}

		/* Every resource starts unwritten and unread. */
		for (index = 0; index < SCHEDULE_RESOURCES; index++) {
			state->reader_head[index] = BCM2711_SHADER_ABSENT;
			state->last_writer[index] = BCM2711_SHADER_ABSENT;
		}

		/* No reader is listed and nothing is pending yet. */
		state->reader_count = 0;
		state->pending_count = 0;
		last_special = BCM2711_SHADER_ABSENT;
		last_ordered = BCM2711_SHADER_ABSENT;
		after_switch = BCM2711_SHADER_ABSENT;

		/* Walks the nodes in source order; the thread end is placed separately and has no edges. */
		for (index = 0; index < state->node_count; index++) {
			if (index == state->end)
				continue;
			error = resource_edges(state, index);
			if (error != 0)
				return error;
			error = order_edges(state, index, &last_special, &last_ordered, &after_switch);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: every dependence of the program is an edge. */
	return 0;
}

/* Records one edge, or only counts it during the counting pass. */
static int
add_edge(
	struct schedule_state *state,
	uint32_t from,
	uint32_t to,
	uint32_t distance)
{
	struct schedule_edge *edge;

	/* The counting pass has no storage yet. */
	if (state->edges == NULL) {
		state->edge_count++;
		return 0;
	}

	/* The storing pass meets exactly the counted edges. */
	if (state->edge_count == state->edge_capacity)
		return EINVAL;
	edge = &state->edges[state->edge_count];
	edge->from = from;
	edge->to = to;
	edge->distance = distance;
	state->edge_count++;

	/* Succeeded: the dependence is stored. */
	return 0;
}

/* Adds the read-after-write, write-after-write and write-after-read edges of one node. */
static int
resource_edges(
	struct schedule_state *state,
	uint32_t index)
{
	struct schedule_node *node;
	struct schedule_node *writer;
	struct schedule_reader *reader;
	uint32_t access;
	uint32_t other;
	uint32_t resource;
	uint32_t entry;
	uint32_t distance;
	uint32_t written_at;
	int error;

	/* A read waits until the last write of its resource has landed. */
	node = &state->nodes[index];
	for (access = 0; access < node->read_count; access++) {
		resource = node->reads[access];
		if (state->last_writer[resource] != BCM2711_SHADER_ABSENT) {
			writer = &state->nodes[state->last_writer[resource]];
			written_at = 0;
			for (other = 0; other < writer->write_count; other++) {
				if (writer->writes[other] == resource)
					written_at = writer->landing[other];
			}

			/* The read waits for the write to land. */
			error = add_edge(state, state->last_writer[resource], index, written_at + 1);
			if (error != 0)
				return error;
		}

		/* The read joins the resource's readers until the next write. */
		if (state->reader_count == state->reader_capacity)
			return EINVAL;
		entry = state->reader_count;
		state->readers[entry].node = index;
		state->readers[entry].next = state->reader_head[resource];
		state->reader_head[resource] = entry;
		state->reader_count++;
	}

	/* A write lands after the previous write of its resource and after every read of the old value. */
	for (access = 0; access < node->write_count; access++) {
		resource = node->writes[access];
		if (state->last_writer[resource] != BCM2711_SHADER_ABSENT) {
			writer = &state->nodes[state->last_writer[resource]];
			written_at = 0;
			for (other = 0; other < writer->write_count; other++) {
				if (writer->writes[other] == resource)
					written_at = writer->landing[other];
			}

			/* Landing order follows source order: issue after the earlier write, later still if it lands sooner. */
			distance = 1;
			if (written_at + 1 > node->landing[access] + 1)
				distance = written_at + 1 - node->landing[access];
			error = add_edge(state, state->last_writer[resource], index, distance);
			if (error != 0)
				return error;
		}

		/*
		 * An ALU write may share the word of a read of the old value, which
		 * reads before the word writes, and so may a write that lands a word
		 * later; another signal's load and the lane flags wait one word.
		 */
		distance = 1;
		if (node->is_signal == 0 && resource != SCHEDULE_FLAGS)
			distance = 0;
		if (node->landing[access] != 0)
			distance = 0;
		entry = state->reader_head[resource];
		while (entry != BCM2711_SHADER_ABSENT) {
			reader = &state->readers[entry];
			if (reader->node != index) {
				error = add_edge(state, reader->node, index, distance);
				if (error != 0)
					return error;
			}

			/* Follows the old value's next reader. */
			entry = reader->next;
		}

		/* The node is now the resource's last writer and the old value's readers are retired. */
		state->reader_head[resource] = BCM2711_SHADER_ABSENT;
		state->last_writer[resource] = index;
	}

	/* Succeeded: the node's resource dependences are edges. */
	return 0;
}

/* Adds the special-function, transaction-order and thread-switch edges of one node. */
static int
order_edges(
	struct schedule_state *state,
	uint32_t index,
	uint32_t *last_special,
	uint32_t *last_ordered,
	uint32_t *after_switch)
{
	struct schedule_node *node;
	struct schedule_node *previous;
	uint32_t distance;
	uint32_t pending;
	uint32_t pair;
	int error;

	/* Special functions start no sooner than three words apart, so a result is never overtaken. */
	node = &state->nodes[index];
	if (node->special != 0) {
		if (*last_special != BCM2711_SHADER_ABSENT) {
			error = add_edge(state, *last_special, index, SCHEDULE_SPECIAL_DISTANCE);
			if (error != 0)
				return error;
		}

		/* This special function starts the next one's wait. */
		*last_special = index;
	}

	/*
	 * TMU, switch and tile transactions keep their source order in separate
	 * words, except a texture configuration and a TMU coordinate write, which
	 * may share one; a load after a switch waits for the switch, and so does
	 * a tile write for the first switch of the last pair, while the pair's
	 * second switch follows the first directly.
	 */
	if (node->ordered != 0) {
		if (*last_ordered != BCM2711_SHADER_ABSENT) {
			previous = &state->nodes[*last_ordered];
			distance = 1;
			if (previous->role == BCM2711_SHADER_SWITCH_SAMPLE)
				distance = SCHEDULE_SWITCH_DISTANCE;
			if (previous->role == BCM2711_SHADER_SWITCH_LAST_SECOND)
				distance = SCHEDULE_SWITCH_DISTANCE - 1;
			pair = texture_pair(previous, node);
			if (pair != 0)
				distance = 0;
			error = add_edge(state, *last_ordered, index, distance);
			if (error != 0)
				return error;
		}

		/* This transaction is the next one's predecessor. */
		*last_ordered = index;
	}

	/* Work that cannot cross a switch stays on its source side: before the switch word, or after its delay words. */
	if (node->sensitive != 0) {
		if (*after_switch != BCM2711_SHADER_ABSENT) {
			error = add_edge(state, *after_switch, index, SCHEDULE_SWITCH_DISTANCE);
			if (error != 0)
				return error;
		}

		/* The node waits for the next switch's edges. */
		state->pending_sensitive[state->pending_count] = index;
		state->pending_count++;
	}

	/* A switch follows every earlier sensitive node since the previous switch. */
	if (node->role == BCM2711_SHADER_SWITCH_SAMPLE || node->role == BCM2711_SHADER_SWITCH_LAST_FIRST) {
		for (pending = 0; pending < state->pending_count; pending++) {
			error = add_edge(state, state->pending_sensitive[pending], index, 1);
			if (error != 0)
				return error;
		}

		/* The switch starts a new run of sensitive work. */
		state->pending_count = 0;
		*after_switch = index;
	}

	/* Succeeded: the node's ordering dependences are edges. */
	return 0;
}

/* Groups the edges by their earlier node and counts every node's unplaced predecessors. */
static int
link_successors(
	struct schedule_state *state)
{
	struct schedule_edge *edge;
	uint32_t index;
	uint32_t position;

	/* One successor entry per edge. */
	if (state->edge_count != 0) {
		state->successors = kern_calloc(state->edge_count, sizeof(*state->successors));
		if (state->successors == NULL)
			return ENOMEM;
	}

	/* Counts each node's successors and predecessors. */
	for (index = 0; index < state->edge_count; index++) {
		edge = &state->edges[index];
		state->nodes[edge->from].successor_count++;
		state->nodes[edge->to].waiting++;

		/* A body node never waits for the tail, which is placed after the body. */
		if (state->nodes[edge->from].tail != 0 && state->nodes[edge->to].tail == 0)
			return ENOTSUP;
	}

	/* Each node's successors take one contiguous run of the successor array. */
	position = 0;
	for (index = 0; index < state->node_count; index++) {
		state->nodes[index].first_successor = position;
		position += state->nodes[index].successor_count;
		state->nodes[index].successor_count = 0;
	}

	/* Each edge takes the next place in its earlier node's run. */
	for (index = 0; index < state->edge_count; index++) {
		edge = &state->edges[index];
		position = state->nodes[edge->from].first_successor + state->nodes[edge->from].successor_count;
		state->successors[position] = index;
		state->nodes[edge->from].successor_count++;
	}

	/* Succeeded: every node lists its successors. */
	return 0;
}

/* Gives every node the longest distance from it to the end of the program. */
static void
set_priorities(
	struct schedule_state *state)
{
	struct schedule_node *node;
	struct schedule_edge *edge;
	uint32_t index;
	uint32_t successor;
	uint32_t length;

	/* Successors come later in source order, so a reverse walk sees them first. */
	index = state->node_count;
	while (index > 0) {
		index--;
		node = &state->nodes[index];
		node->priority = 1;
		for (successor = 0; successor < node->successor_count; successor++) {
			edge = &state->edges[state->successors[node->first_successor + successor]];
			length = edge->distance + state->nodes[edge->to].priority;
			if (length > node->priority)
				node->priority = length;
		}
	}

	/* Succeeded: longer remaining paths are placed first. */
	return;
}

/* Places the body's nodes word by word, the ready node with the longest remaining path first. */
static int
schedule_body(
	struct schedule_state *state)
{
	struct schedule_node *node;
	uint32_t body_count;
	uint32_t placed;
	uint32_t word;
	uint32_t index;
	uint32_t best;
	uint32_t best_slot;
	uint32_t slot;
	int error;

	/* The body is every node except the tail and the thread end. */
	body_count = 0;
	for (index = 0; index < state->node_count; index++) {
		if (state->nodes[index].tail == 0 && index != state->end)
			body_count++;
	}

	/* The word storage bounds every schedule; running out refuses the schedule. */
	state->word_capacity = state->node_count * SCHEDULE_WORDS_PER_NODE + SCHEDULE_TAIL_WORDS;
	state->words = kern_calloc(state->word_capacity, sizeof(*state->words));
	if (state->words == NULL)
		return ENOMEM;

	/* Each word takes ready nodes until none fits; an empty word waits for a latency. */
	placed = 0;
	word = 0;
	while (placed < body_count) {
		error = grow_words(state, word + 1);
		if (error != 0)
			return error;
		for (;;) {
			/* Chooses the best ready node that fits the word. */
			best = BCM2711_SHADER_ABSENT;
			best_slot = 0;
			for (index = 0; index < state->node_count; index++) {
				node = &state->nodes[index];
				if (node->tail != 0 ||
				    index == state->end ||
				    node->word != BCM2711_SHADER_ABSENT)
					continue;
				if (node->waiting != 0 || node->earliest > word)
					continue;
				error = fits(state, &state->words[word], index, &slot);
				if (error != 0)
					continue;
				if (best == BCM2711_SHADER_ABSENT || node->priority > state->nodes[best].priority) {
					best = index;
					best_slot = slot;
				}
			}

			/* A full word, or one no ready node fits, is finished. */
			if (best == BCM2711_SHADER_ABSENT)
				break;
			place(state, best, word, best_slot);
			placed++;

			/* A launched lookup's switch may take this word or one of the two before it. */
			if (state->nodes[best].texture_write != 0)
				placed += place_sample_switch(state, best, word);
		}

		/* The next word. */
		word++;
	}

	/* The body ends after its last used word. */
	state->word_count = word;

	/* Succeeded: every body node has a word. */
	return 0;
}

/* Puts one node in a word's slot and releases its successors' waits. */
static void
place(
	struct schedule_state *state,
	uint32_t index,
	uint32_t word,
	uint32_t slot)
{
	struct schedule_node *node;
	struct schedule_node *successor;
	struct schedule_edge *edge;
	uint32_t position;

	/* The slot records the node, and the node its word. */
	node = &state->nodes[index];
	node->word = word;
	if (slot == 0)
		state->words[word].add = index;
	else if (slot == 1)
		state->words[word].multiply = index;
	else if (slot == 2)
		state->words[word].signal = index;
	else
		state->words[word].thread_switch = index;

	/* Each successor may start its edge's distance after this word. */
	for (position = 0; position < node->successor_count; position++) {
		edge = &state->edges[state->successors[node->first_successor + position]];
		successor = &state->nodes[edge->to];
		if (successor->earliest < word + edge->distance)
			successor->earliest = word + edge->distance;
		successor->waiting--;
	}

	/* Succeeded: the node is placed. */
	return;
}

/*
 * Places the switch that waits for a lookup as soon as the lookup's launch is placed.
 *
 * A switch takes effect after its two delay words, so the switch may sit in
 * the launch's word or up to two words before it, with the launch in its
 * delay words; the lookup's results are read from three words after the
 * switch.  The words from the switch to the launch hold no special
 * function or varying load and no other transaction than this lookup's TMU
 * writes.  Reports one when the switch was placed.
 */
static uint32_t
place_sample_switch(
	struct schedule_state *state,
	uint32_t launch,
	uint32_t word)
{
	struct schedule_node *node;
	struct schedule_node *other;
	struct schedule_edge *edge;
	struct schedule_word trial;
	uint32_t present[4];
	uint32_t present_count;
	uint32_t entry;
	uint32_t position;
	uint32_t target;
	uint32_t lowest;
	uint32_t candidate;
	uint32_t later;
	uint32_t finishes;
	uint64_t encoded;
	int error;
	int usable;

	/* The launch's ordered successor must be a sample switch that waits for nothing else. */
	node = &state->nodes[launch];
	target = BCM2711_SHADER_ABSENT;
	for (position = 0; position < node->successor_count; position++) {
		edge = &state->edges[state->successors[node->first_successor + position]];
		if (state->nodes[edge->to].role == BCM2711_SHADER_SWITCH_SAMPLE)
			target = edge->to;
	}

	/* A launch without a sample switch has none to place. */
	if (target == BCM2711_SHADER_ABSENT)
		return 0;
	if (state->nodes[target].waiting != 0 || state->nodes[target].word != BCM2711_SHADER_ABSENT)
		return 0;

	/* The switch's other predecessors decide its earliest word; the launch allows two words before its own. */
	lowest = switch_lowest(state, target, launch);
	if (word >= 2 && lowest < word - 2)
		lowest = word - 2;

	/* The first word up to the launch's that takes the switch signal and holds only plain work and this lookup's writes. */
	for (candidate = lowest; candidate <= word; candidate++) {
		trial = state->words[candidate];
		if (trial.thread_switch != BCM2711_SHADER_ABSENT)
			continue;
		trial.thread_switch = target;
		error = word_encode(state, &trial, &encoded);
		if (error != 0)
			continue;
		usable = 1;
		for (later = candidate; later <= word && later <= candidate + 2; later++) {
			present_count = word_node_count(&state->words[later], present);
			for (entry = 0; entry < present_count; entry++) {
				other = &state->nodes[present[entry]];
				finishes = finishes_in_window(other);
				if (other->sensitive != 0 && finishes == 0)
					usable = 0;
				if (other->ordered != 0 &&
				    other->texture_write == 0 &&
				    other->texture_config == 0)
					usable = 0;
			}
		}

		/* A word with other work keeps the switch away. */
		if (usable == 0)
			continue;

		/* The switch is placed; its results wait three words from here. */
		place(state, target, candidate, 3);
		return 1;
	}

	/* The switch waits for the ordinary placement after the launch. */
	return 0;
}

/*
 * Reports the earliest word a switch's placed predecessors allow, ignoring one launch.
 *
 * A switch takes effect after its two delay words.  Accumulator and flag
 * work before it in source order only has to finish by then, so it may sit
 * in the switch's word or delay words; a special function or varying load
 * before it must start in an earlier word, and any other predecessor keeps
 * its edge's distance.
 */
static uint32_t
switch_lowest(
	const struct schedule_state *state,
	uint32_t target,
	uint32_t launch)
{
	const struct schedule_edge *edge;
	const struct schedule_node *from;
	uint32_t position;
	uint32_t lowest;
	uint32_t bound;
	uint32_t finishes;

	/* Every predecessor is placed. */
	lowest = 0;
	for (position = 0; position < state->edge_count; position++) {
		edge = &state->edges[position];
		if (edge->to != target || edge->from == launch)
			continue;
		from = &state->nodes[edge->from];
		bound = from->word + edge->distance;
		finishes = finishes_in_window(from);
		if (finishes != 0) {
			bound = 0;
			if (from->word >= 2)
				bound = from->word - 2;
		}

		/* The latest bound wins. */
		if (lowest < bound)
			lowest = bound;
	}

	/* Reports the first word the predecessors allow. */
	return lowest;
}

/* Tells whether a sensitive node may run in a switch's word or delay words when it comes before the switch in source order. */
static uint32_t
finishes_in_window(
	const struct schedule_node *node)
{
	/* Plain accumulator or flag work finishes when its word ends; transactions, signals and late results do not. */
	if (node->sensitive == 0)
		return 0;
	if (node->ordered != 0)
		return 0;
	if (node->special != 0)
		return 0;
	if (node->is_signal != 0)
		return 0;

	/* The work is done before the switch takes effect. */
	return 1;
}

/*
 * Tells whether one node fits a word beside the nodes already in it.
 *
 * The slot is the ALU or signal position the node takes.  The word keeps one
 * peripheral transaction and one uniform, and its encoding must succeed.
 */
static int
fits(
	const struct schedule_state *state,
	const struct schedule_word *word,
	uint32_t index,
	uint32_t *slot)
{
	const struct schedule_node *node;
	struct schedule_word trial;
	uint32_t present[4];
	uint32_t present_count;
	uint32_t entry;
	uint32_t clash;
	uint64_t encoded;
	int error;

	/* One peripheral transaction and one uniform per word. */
	node = &state->nodes[index];
	present_count = word_node_count(word, present);
	for (entry = 0; entry < present_count; entry++) {
		clash = peripherals_clash(node, &state->nodes[present[entry]]);
		if (clash != 0)
			return ENOTSUP;
		if (node->uniform != 0 && state->nodes[present[entry]].uniform != 0)
			return ENOTSUP;
	}

	/* A switch takes the switch slot, another signal the signal slot; a move prefers the multiply ALU and takes the add ALU when that is busy. */
	trial = *word;
	if (node->role != BCM2711_SHADER_SWITCH_NONE) {
		if (trial.thread_switch != BCM2711_SHADER_ABSENT)
			return ENOTSUP;
		trial.thread_switch = index;
		*slot = 3;
	} else if (node->is_signal != 0) {
		if (trial.signal != BCM2711_SHADER_ABSENT)
			return ENOTSUP;
		trial.signal = index;
		*slot = 2;
	} else if (node->unit == BCM2711_QPU_UNIT_MULTIPLY) {
		if (trial.multiply != BCM2711_SHADER_ABSENT)
			return ENOTSUP;
		trial.multiply = index;
		*slot = 1;
	} else if (node->unit == BCM2711_QPU_UNIT_ADD) {
		if (trial.add != BCM2711_SHADER_ABSENT)
			return ENOTSUP;
		trial.add = index;
		*slot = 0;
	} else if (trial.multiply == BCM2711_SHADER_ABSENT) {
		trial.multiply = index;
		*slot = 1;
	} else if (trial.add == BCM2711_SHADER_ABSENT) {
		trial.add = index;
		*slot = 0;
	} else {
		/* Both ALUs are busy. */
		return ENOTSUP;
	}

	/* The shared ports, constant, conditions and signal must encode together. */
	error = word_encode(state, &trial, &encoded);
	if (error != 0)
		return error;

	/* Succeeded: the node fits the word in the reported slot. */
	return 0;
}

/* Encodes one scheduled word from the records in its slots. */
static int
word_encode(
	const struct schedule_state *state,
	const struct schedule_word *word,
	uint64_t *encoded)
{
	struct bcm2711_qpu_bundle bundle;
	const struct bcm2711_shader_op *op;
	int error;

	/* Empty slots stay idle; the operations' own signal fields are clear because records carry one or the other. */
	kern_memset(&bundle, 0, sizeof(bundle));
	if (word->add != BCM2711_SHADER_ABSENT)
		bundle.add = state->compiler->ops[state->nodes[word->add].op].instruction;
	if (word->multiply != BCM2711_SHADER_ABSENT)
		bundle.multiply = state->compiler->ops[state->nodes[word->multiply].op].instruction;
	if (word->signal != BCM2711_SHADER_ABSENT) {
		op = &state->compiler->ops[state->nodes[word->signal].op];
		bundle.signal = op->instruction.signal;
		bundle.signal_destination = op->instruction.signal_destination;
	}

	/* A switch rides on the word's signal. */
	if (word->thread_switch != BCM2711_SHADER_ABSENT)
		bundle.thread_switch = 1;

	/* The encoder checks everything the word's fields share. */
	error = bcm2711_qpu_encode_bundle(&bundle, encoded);
	if (error != 0)
		return error;

	/* Succeeded: the word has a native encoding. */
	return 0;
}

/* Lists the nodes in a word's slots and reports how many there are. */
static uint32_t
word_node_count(
	const struct schedule_word *word,
	uint32_t *nodes)
{
	uint32_t count;

	/* Collects each occupied slot. */
	count = 0;
	if (word->add != BCM2711_SHADER_ABSENT) {
		nodes[count] = word->add;
		count++;
	}

	/* The multiply ALU's node. */
	if (word->multiply != BCM2711_SHADER_ABSENT) {
		nodes[count] = word->multiply;
		count++;
	}

	/* The signal's node. */
	if (word->signal != BCM2711_SHADER_ABSENT) {
		nodes[count] = word->signal;
		count++;
	}

	/* The switch's node. */
	if (word->thread_switch != BCM2711_SHADER_ABSENT) {
		nodes[count] = word->thread_switch;
		count++;
	}

	/* Reports the number of occupied slots. */
	return count;
}

/*
 * Places the last switch pair, the tile writes and the thread end.
 *
 * Every possible word of the pair's first switch is tried: the tile writes
 * take the first words they fit from three words after the second switch,
 * and the thread end the first word whose remaining words write no register.
 * The shortest program wins; among equal lengths the latest pair, which
 * holds the tile scoreboard for the shortest time.
 */
static int
schedule_tail(
	struct schedule_state *state)
{
	uint32_t tiles[2];
	uint32_t best_tiles[2];
	uint32_t tile_count;
	uint32_t index;
	uint32_t first;
	uint32_t lowest;
	uint32_t end;
	uint32_t length;
	uint32_t best_first;
	uint32_t best_end;
	uint32_t best_length;
	int error;

	/* The program writes the tile exactly twice, red-green then blue-alpha. */
	tile_count = 0;
	for (index = 0; index < state->node_count; index++) {
		if (state->nodes[index].tile_write != 0) {
			if (tile_count == 2)
				return ENOTSUP;
			state->tile_nodes[tile_count] = index;
			tile_count++;
		}
	}

	/* Exactly two were found. */
	if (tile_count != 2)
		return ENOTSUP;

	/* The first switch waits for its placed predecessors; accumulator and flag work may share its window. */
	if (state->nodes[state->last_first].waiting != 0)
		return EINVAL;
	lowest = switch_lowest(state, state->last_first, BCM2711_SHADER_ABSENT);

	/* Tries every first-switch word up to just past the body; the best placement wins. */
	best_first = BCM2711_SHADER_ABSENT;
	best_end = 0;
	best_length = 0;
	best_tiles[0] = 0;
	best_tiles[1] = 0;
	for (first = lowest; first <= state->word_count; first++) {
		error = tail_trial(state, first, tiles, &end, &length);
		if (error != 0)
			continue;
		if (best_first == BCM2711_SHADER_ABSENT || length <= best_length) {
			best_first = first;
			best_end = end;
			best_length = length;
			best_tiles[0] = tiles[0];
			best_tiles[1] = tiles[1];
		}
	}

	/* No word takes the tail. */
	if (best_first == BCM2711_SHADER_ABSENT)
		return ENOTSUP;

	/* Commits the chosen placement into the words. */
	error = grow_words(state, best_length);
	if (error != 0)
		return error;
	place(state, state->last_first, best_first, 3);
	place(state, state->last_second, best_first + 1, 3);
	place(state, state->tile_nodes[0], best_tiles[0], 0);
	place(state, state->tile_nodes[1], best_tiles[1], 0);
	place(state, state->end, best_end, 3);
	state->word_count = best_length;

	/* Succeeded: the program ends two words after its thread end. */
	return 0;
}

/* Places the tail for one first-switch word without changing the words, and reports the program length. */
static int
tail_trial(
	struct schedule_state *state,
	uint32_t first,
	uint32_t *tiles,
	uint32_t *end,
	uint32_t *length)
{
	struct schedule_word trial;
	uint32_t tile;
	uint32_t node;
	uint32_t word;
	uint32_t slot;
	uint32_t used;
	int error;

	/* Both switches of the pair need their words' signal slots. */
	error = switch_fits(state, first, state->last_first);
	if (error != 0)
		return error;
	error = switch_fits(state, first + 1, state->last_second);
	if (error != 0)
		return error;

	/* Each tile write takes the first word it fits once the first switch holds the scoreboard and its colour is ready. */
	used = first + 2;
	for (tile = 0; tile < 2; tile++) {
		node = state->tile_nodes[tile];
		word = ready_word(state, node);
		if (word < first + SCHEDULE_SWITCH_DISTANCE)
			word = first + SCHEDULE_SWITCH_DISTANCE;
		if (tile != 0 && word < tiles[tile - 1] + 1)
			word = tiles[tile - 1] + 1;
		for (;;) {
			trial_word(state, word, NULL, 0, &trial);
			error = fits(state, &trial, node, &slot);
			if (error == 0)
				break;
			word++;
		}

		/* The tile write takes the word. */
		tiles[tile] = word;
		if (word + 1 > used)
			used = word + 1;
	}

	/* The program is at least as long as its body. */
	if (used < state->word_count)
		used = state->word_count;

	/* The thread end follows the first switch's delay words and keeps every used word inside its window. */
	*end = first + SCHEDULE_SWITCH_DISTANCE;
	if (used > 3 && *end < used - 3)
		*end = used - 3;

	/* Moves the thread end later until no word in its window writes a register; past the used words it always fits. */
	for (;;) {
		error = ends_in(state, *end, used, tiles);
		if (error == 0)
			break;
		(*end)++;
	}

	/* The program ends with the thread end's second delay word. */
	*length = used;
	if (*end + 3 > *length)
		*length = *end + 3;

	/* Succeeded: the tail fits this first-switch word. */
	return 0;
}

/* Reports the earliest word one tail node's placed predecessors allow. */
static uint32_t
ready_word(
	const struct schedule_state *state,
	uint32_t index)
{
	const struct schedule_edge *edge;
	uint32_t position;
	uint32_t word;

	/* The body is placed, so every body predecessor has its word; tail predecessors are placed by the caller. */
	word = 0;
	for (position = 0; position < state->edge_count; position++) {
		edge = &state->edges[position];
		if (edge->to != index)
			continue;
		if (state->nodes[edge->from].tail != 0)
			continue;
		if (word < state->nodes[edge->from].word + edge->distance)
			word = state->nodes[edge->from].word + edge->distance;
	}

	/* Reports the first word the body allows. */
	return word;
}

/* Copies one word as it would be with the tile writes of a trial; a word past the body is empty. */
static void
trial_word(
	const struct schedule_state *state,
	uint32_t word,
	const uint32_t *tiles,
	uint32_t tile_count,
	struct schedule_word *trial)
{
	uint32_t tile;

	/* The body's words are fixed; later words start empty. */
	trial->add = BCM2711_SHADER_ABSENT;
	trial->multiply = BCM2711_SHADER_ABSENT;
	trial->signal = BCM2711_SHADER_ABSENT;
	trial->thread_switch = BCM2711_SHADER_ABSENT;
	if (word < state->word_count)
		*trial = state->words[word];

	/* A tile write tried in the word takes the add ALU. */
	for (tile = 0; tile < tile_count; tile++) {
		if (tiles[tile] == word)
			trial->add = state->tile_nodes[tile];
	}

	/* Succeeded: the trial word is ready. */
	return;
}

/* Tells whether one switch signal fits one word of the body or past it. */
static int
switch_fits(
	const struct schedule_state *state,
	uint32_t word,
	uint32_t index)
{
	struct schedule_word trial;
	uint64_t encoded;
	int error;

	/* The switch slot must be free. */
	trial_word(state, word, NULL, 0, &trial);
	if (trial.thread_switch != BCM2711_SHADER_ABSENT)
		return ENOTSUP;

	/* The switch must encode beside the word's operations and signal. */
	trial.thread_switch = index;
	error = word_encode(state, &trial, &encoded);
	if (error != 0)
		return error;

	/* Succeeded: the switch fits the word. */
	return 0;
}

/* Tells whether the thread end may sit in one word, with the program's used words inside its window. */
static int
ends_in(
	const struct schedule_state *state,
	uint32_t end,
	uint32_t used,
	const uint32_t *tiles)
{
	struct schedule_word trial;
	uint32_t present[4];
	uint32_t present_count;
	uint32_t entry;
	uint32_t word;
	uint64_t encoded;
	int error;

	/* Every used word from the thread end on must be the end word or one of its two delay words. */
	if (used > end + 3)
		return ENOTSUP;

	/* Each used word in the window must allow the thread end at its position. */
	for (word = end; word < used; word++) {
		trial_word(state, word, tiles, 2, &trial);
		present_count = word_node_count(&trial, present);
		for (entry = 0; entry < present_count; entry++) {
			error = node_allows_end(state, present[entry], word - end);
			if (error != 0)
				return error;
		}
	}

	/* The thread end's own word needs a free switch slot that encodes with the word's signal and any tile write there. */
	trial_word(state, end, tiles, 2, &trial);
	if (trial.thread_switch != BCM2711_SHADER_ABSENT)
		return ENOTSUP;
	trial.thread_switch = state->end;
	error = word_encode(state, &trial, &encoded);
	if (error != 0)
		return error;

	/* Succeeded: the thread end fits the word. */
	return 0;
}

/* Tells whether one node may sit in a thread end's word or delay words: it writes no register and reads no payload. */
static int
node_allows_end(
	const struct schedule_state *state,
	uint32_t index,
	uint32_t slot)
{
	const struct schedule_node *node;
	uint32_t access;

	/* No register write, varying, special function or late uniform near the end. */
	node = &state->nodes[index];
	for (access = 0; access < node->write_count; access++) {
		if (node->writes[access] < SCHEDULE_ACCUMULATOR)
			return ENOTSUP;
	}

	/* Sensitive work does not run while the thread ends. */
	if (node->sensitive != 0)
		return ENOTSUP;
	if (node->uniform != 0 && slot > 0)
		return ENOTSUP;

	/* The fragment payload registers zero to two may be refilled for the next pixel. */
	for (access = 0; access < node->read_count; access++) {
		if (node->reads[access] < 3)
			return ENOTSUP;
	}

	/* Succeeded: the node may run while the thread ends. */
	return 0;
}

/* Makes sure the word storage holds count words, all slots free beyond the used words. */
static int
grow_words(
	struct schedule_state *state,
	uint32_t count)
{
	uint32_t index;

	/* The fixed storage bounds the schedule. */
	if (count > state->word_capacity)
		return E2BIG;

	/* New words start with every slot free. */
	for (index = state->word_count; index < count; index++) {
		state->words[index].add = BCM2711_SHADER_ABSENT;
		state->words[index].multiply = BCM2711_SHADER_ABSENT;
		state->words[index].signal = BCM2711_SHADER_ABSENT;
		state->words[index].thread_switch = BCM2711_SHADER_ABSENT;
	}

	/* The used words grow to the requested count. */
	if (count > state->word_count)
		state->word_count = count;

	/* Succeeded: the words exist. */
	return 0;
}

/*
 * Checks a finished schedule independently of the edges that produced it.
 *
 * Every source must read the value its source-order writer produced, every
 * word must keep the per-word limits, and the transaction, switch and thread
 * end rules must hold at the chosen words.
 */
static int
verify(
	struct schedule_state *state)
{
	uint32_t index;
	uint32_t word;
	int error;

	/* Every node is in exactly the slot of its word that names it. */
	for (index = 0; index < state->node_count; index++) {
		word = state->nodes[index].word;
		if (word >= state->word_count)
			return EINVAL;
		if (state->words[word].add != index &&
		    state->words[word].multiply != index &&
		    state->words[word].signal != index &&
		    state->words[word].thread_switch != index)
			return EINVAL;
	}

	/* Values follow the source order. */
	error = verify_values(state);
	if (error != 0)
		return error;

	/* Words, transactions, switches and the thread end follow the hardware rules. */
	error = verify_rules(state);
	if (error != 0)
		return error;

	/* Succeeded: the schedule computes the source program under the timing rules. */
	return 0;
}

/*
 * Simulates the schedule's resource versions word by word.
 *
 * A version is the node that wrote the value.  A read must see the version
 * its source-order writer left and must not meet a write still on its way;
 * writes must land in source order.
 */
static int
verify_values(
	struct schedule_state *state)
{
	struct schedule_node *node;
	struct schedule_word *slots;
	uint32_t *expected;
	uint32_t visible[SCHEDULE_RESOURCES];
	uint32_t pending[SCHEDULE_RESOURCES];
	uint32_t lands[SCHEDULE_RESOURCES];
	uint32_t present[4];
	uint32_t present_count;
	uint32_t entry;
	uint32_t index;
	uint32_t access;
	uint32_t resource;
	uint32_t word;
	int error;

	/* Each read's source-order writer, one entry per possible read. */
	expected = kern_calloc((size_t)state->node_count * SCHEDULE_ACCESSES, sizeof(*expected));
	if (expected == NULL)
		return ENOMEM;
	for (resource = 0; resource < SCHEDULE_RESOURCES; resource++)
		visible[resource] = BCM2711_SHADER_ABSENT;
	for (index = 0; index < state->node_count; index++) {
		node = &state->nodes[index];
		for (access = 0; access < node->read_count; access++)
			expected[index * SCHEDULE_ACCESSES + access] = visible[node->reads[access]];
		for (access = 0; access < node->write_count; access++)
			visible[node->writes[access]] = index;
	}

	/* The simulation starts with the program's initial values and nothing in flight. */
	for (resource = 0; resource < SCHEDULE_RESOURCES; resource++) {
		visible[resource] = BCM2711_SHADER_ABSENT;
		pending[resource] = BCM2711_SHADER_ABSENT;
		lands[resource] = 0;
	}

	/* Runs every word, then one more step that lands the last writes. */
	error = 0;
	for (word = 0; word <= state->word_count && error == 0; word++) {
		/* Writes that landed at the end of an earlier word become visible, in source order. */
		for (resource = 0; resource < SCHEDULE_RESOURCES; resource++) {
			if (pending[resource] == BCM2711_SHADER_ABSENT || lands[resource] >= word)
				continue;
			if (visible[resource] != BCM2711_SHADER_ABSENT && visible[resource] > pending[resource])
				error = EINVAL;
			visible[resource] = pending[resource];
			pending[resource] = BCM2711_SHADER_ABSENT;
		}

		/* The step after the last word only lands writes. */
		if (word == state->word_count || error != 0)
			break;

		/* Every read in the word sees its expected version, with no write to it in flight. */
		slots = &state->words[word];
		present_count = word_node_count(slots, present);
		for (entry = 0; entry < present_count; entry++) {
			node = &state->nodes[present[entry]];
			for (access = 0; access < node->read_count; access++) {
				resource = node->reads[access];
				if (pending[resource] != BCM2711_SHADER_ABSENT)
					error = EINVAL;
				if (visible[resource] != expected[present[entry] * SCHEDULE_ACCESSES + access])
					error = EINVAL;
			}
		}

		/* The word's writes start; at most one write of a resource is ever in flight. */
		for (entry = 0; entry < present_count; entry++) {
			node = &state->nodes[present[entry]];
			for (access = 0; access < node->write_count; access++) {
				resource = node->writes[access];
				if (pending[resource] != BCM2711_SHADER_ABSENT)
					error = EINVAL;
				pending[resource] = present[entry];
				lands[resource] = word + node->landing[access];
			}
		}
	}

	/* The expected versions retire. */
	kern_free(expected);

	/* A stale read, a write overtaken in flight or a write landing out of order refuses the schedule. */
	if (error != 0)
		return error;

	/* Succeeded: every read sees the value the source order gives it. */
	return 0;
}

/* Checks the per-word limits, transaction order, switch windows, special-function spacing and thread end. */
static int
verify_rules(
	struct schedule_state *state)
{
	struct schedule_node *node;
	struct schedule_node *other;
	struct schedule_word *slots;
	uint32_t present[4];
	uint32_t present_count;
	uint32_t entry;
	uint32_t peripherals;
	uint32_t uniforms;
	uint32_t index;
	uint32_t later;
	uint32_t word;
	uint32_t previous;
	uint32_t window;
	uint32_t pair;
	uint32_t finishes;
	uint64_t encoded;
	int error;

	/* Each word encodes, starts at most one peripheral transaction and reads at most one uniform. */
	for (word = 0; word < state->word_count; word++) {
		slots = &state->words[word];
		error = word_encode(state, slots, &encoded);
		if (error != 0)
			return error;
		present_count = word_node_count(slots, present);
		peripherals = 0;
		uniforms = 0;
		for (entry = 0; entry < present_count; entry++) {
			uniforms += state->nodes[present[entry]].uniform;
			for (later = entry + 1; later < present_count; later++)
				peripherals += peripherals_clash(&state->nodes[present[entry]], &state->nodes[present[later]]);
		}

		/* A clash or a second uniform refuses the schedule. */
		if (peripherals != 0 || uniforms > 1)
			return EINVAL;
	}

	/* Transactions keep their source order in separate words; loads and tile writes wait for their switch. */
	previous = BCM2711_SHADER_ABSENT;
	for (index = 0; index < state->node_count; index++) {
		node = &state->nodes[index];
		if (node->ordered == 0 || index == state->end)
			continue;
		if (previous != BCM2711_SHADER_ABSENT) {
			other = &state->nodes[previous];
			if (node->role == BCM2711_SHADER_SWITCH_SAMPLE) {
				/* A sample switch may precede its launch by up to two words, which then sits in its delay words. */
				if (node->word + 2 < other->word)
					return EINVAL;
			} else if (node->word < other->word) {
				return EINVAL;
			} else if (node->word == other->word) {
				/* Only a texture configuration and a TMU coordinate write share a word. */
				pair = texture_pair(other, node);
				if (pair == 0)
					return EINVAL;
			}

			/* A texture result waits for its switch. */
			if (other->role == BCM2711_SHADER_SWITCH_SAMPLE && node->word < other->word + SCHEDULE_SWITCH_DISTANCE)
				return EINVAL;
		}

		/* This transaction is the next one's predecessor. */
		previous = index;
	}

	/* The last pair's switches are adjacent; the tile writes and thread end wait for the first one to take effect. */
	word = state->nodes[state->last_second].word;
	if (word != state->nodes[state->last_first].word + 1)
		return EINVAL;
	if (state->nodes[state->tile_nodes[0]].word + 1 < word + SCHEDULE_SWITCH_DISTANCE)
		return EINVAL;
	if (state->nodes[state->end].word + 1 < word + SCHEDULE_SWITCH_DISTANCE)
		return EINVAL;

	/* The program ends with the thread end's second delay word. */
	if (state->nodes[state->end].word + 3 != state->word_count)
		return EINVAL;

	/*
	 * Around every switch, sensitive work stays on its source side: work
	 * after it starts after its two delay words; work before it starts in
	 * an earlier word, except plain accumulator and flag work, which may run
	 * in the switch's word and delay words of any switch but the thread end.
	 * The second switch of the last pair takes effect with the first.
	 */
	for (index = 0; index < state->node_count; index++) {
		node = &state->nodes[index];
		if (node->role == BCM2711_SHADER_SWITCH_NONE)
			continue;
		for (later = 0; later < state->node_count && node->role != BCM2711_SHADER_SWITCH_LAST_SECOND; later++) {
			other = &state->nodes[later];
			if (other->sensitive == 0)
				continue;
			if (later > index && other->word < node->word + 3)
				return EINVAL;
			if (later < index && other->word > node->word + 2)
				return EINVAL;
			finishes = finishes_in_window(other);
			if (node->role == BCM2711_SHADER_SWITCH_END)
				finishes = 0;
			if (later < index &&
			    other->word >= node->word &&
			    finishes == 0)
				return EINVAL;
		}

		/*
		 * A switch word and its delay words read no texture result and write
		 * no tile, except the thread end's, and the last pair's mark, whose
		 * scoreboard rule is the first switch's.
		 */
		for (later = 0; later < state->node_count; later++) {
			other = &state->nodes[later];
			if (other->texture_read == 0 && other->tile_write == 0)
				continue;
			if (other->tile_write != 0 && node->role == BCM2711_SHADER_SWITCH_END)
				continue;
			if (other->tile_write != 0 && node->role == BCM2711_SHADER_SWITCH_LAST_SECOND)
				continue;
			if (other->word >= node->word && other->word <= node->word + 2)
				return EINVAL;
		}
	}

	/* Special functions start at least three words apart. */
	for (index = 0; index < state->node_count; index++) {
		node = &state->nodes[index];
		if (node->special == 0)
			continue;
		for (later = index + 1; later < state->node_count; later++) {
			other = &state->nodes[later];
			if (other->special == 0)
				continue;
			if (other->word < node->word + SCHEDULE_SPECIAL_DISTANCE && node->word < other->word + SCHEDULE_SPECIAL_DISTANCE)
				return EINVAL;
		}
	}

	/* The thread end's word and delay words write no register and read no payload. */
	word = state->nodes[state->end].word;
	for (window = 0; window < 3; window++) {
		slots = &state->words[word + window];
		present_count = word_node_count(slots, present);
		for (entry = 0; entry < present_count; entry++) {
			if (present[entry] == state->end)
				continue;
			error = node_allows_end(state, present[entry], window);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the schedule keeps every word and transaction rule. */
	return 0;
}

/* Writes the scheduled words and the uniform stream in word order. */
static int
emit_words(
	struct schedule_state *state)
{
	struct bcm2711_shader_binary *binary;
	struct schedule_word *slots;
	uint32_t present[4];
	uint32_t present_count;
	uint32_t entry;
	uint32_t word;
	uint64_t encoded;
	int error;

	/* The program's storage was sized for the source-order words; a longer schedule is refused. */
	binary = state->compiler->binary;
	if (state->word_count > state->compiler->code_capacity)
		return E2BIG;

	/* Each word is encoded, and a uniform it reads takes the next stream position. */
	binary->code_count = 0;
	binary->uniform_count = 0;
	for (word = 0; word < state->word_count; word++) {
		slots = &state->words[word];
		error = word_encode(state, slots, &encoded);
		if (error != 0)
			return error;
		binary->code[binary->code_count] = encoded;
		binary->code_count++;
		present_count = word_node_count(slots, present);
		for (entry = 0; entry < present_count; entry++) {
			if (state->nodes[present[entry]].uniform == 0)
				continue;
			if (binary->uniform_count == state->compiler->uniform_capacity)
				return E2BIG;
			binary->uniforms[binary->uniform_count] = state->compiler->ops[state->nodes[present[entry]].op].uniform;
			binary->uniform_count++;
		}
	}

	/* Succeeded: the program's words and uniform stream follow the schedule. */
	return 0;
}

/* Releases every allocation of one scheduling attempt. */
static void
release_state(
	struct schedule_state *state)
{
	/* Each array is released once; absent ones are null. */
	kern_free(state->nodes);
	kern_free(state->edges);
	kern_free(state->successors);
	kern_free(state->words);
	kern_free(state->readers);
	kern_free(state->pending_sensitive);

	/* Succeeded: the attempt owns no storage. */
	return;
}
