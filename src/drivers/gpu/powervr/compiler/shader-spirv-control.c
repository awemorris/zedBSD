/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPIR-V reader's control flow: inlined calls, structured
 * selections and loops, the edges between blocks and their phis.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/compiler/shader.h"
#include "drivers/gpu/powervr/compiler/shader-ir.h"
#include "drivers/gpu/powervr/compiler/shader-private.h"
#include "drivers/gpu/powervr/compiler/shader-spirv.h"

static int reader_region(struct shader_spirv_reader *reader, uint32_t start, uint32_t stop, uint32_t through_first, uint32_t *returned);
static int reader_loop(struct shader_spirv_reader *reader, uint32_t header);
static int reader_arm(struct shader_spirv_reader *reader, uint32_t from, uint32_t target, uint32_t merge);
static int reader_edge(struct shader_spirv_reader *reader, uint32_t from, uint32_t target);
static int reader_exit(struct shader_spirv_reader *reader, uint32_t target, uint32_t *exited);
static int reader_phi_moves(struct shader_spirv_reader *reader, uint32_t from, uint32_t target);
static int reader_block_body(struct shader_spirv_reader *reader, uint32_t label, uint32_t *terminator);
static int reader_kill_only(struct shader_spirv_reader *reader, uint32_t label);

/*
 * Inlines one function: its parameters are already bound; its blocks
 * are walked from the first; its return value lands in result.
 */
int
shader_spirv_reader_function(
	struct shader_spirv_reader *reader,
	uint32_t function,
	uint32_t *result)
{
	uint32_t at;
	uint32_t length;
	uint32_t opcode;
	uint32_t first_label;
	uint32_t returned;
	uint32_t saved_loops;
	int error;

	/* Refuses calls nested deeper than the reader allows. */
	if (reader->calls >= SHADER_SPIRV_CALLS)
		return ENOTSUP;
	if (function >= reader->bound || reader->ids[function].kind != ID_FUNCTION)
		return EINVAL;

	/* Finds the first block past the function and its parameters. */
	at = reader->ids[function].word;
	first_label = 0;
	while (at < reader->count) {
		length = reader->words[at] >> 16;
		opcode = reader->words[at] & 0xffffU;
		if (length == 0)
			return EINVAL;
		if (opcode == OP_LABEL) {
			first_label = reader->words[at + 1];
			break;
		}
		at += length;
	}
	if (first_label == 0)
		return EINVAL;

	/* Walks the function's blocks; its loops are its own. */
	reader->calls++;
	saved_loops = reader->loop_count;
	returned = 0;
	error = reader_region(reader, first_label, 0, 0, &returned);
	reader->loop_count = saved_loops;
	reader->calls--;
	if (error != 0)
		return error;

	/* Refuses a function that does not end by returning. */
	if (returned == 0)
		return ENOTSUP;

	/* Succeeded: the function's result id, if it returns a value. */
	*result = reader->returned;
	return 0;
}

/*
 * Walks blocks in a straight line from start until a branch reaches
 * stop.  A loop header is walked as its whole loop; a selection as its
 * if; a branch to the innermost loop's merge or continue target that is
 * not the stop is a break or continue.  Through_first walks the first
 * block even when it is the stop (a loop whose header continues itself).
 */
static int
reader_region(
	struct shader_spirv_reader *reader,
	uint32_t start,
	uint32_t stop,
	uint32_t through_first,
	uint32_t *returned)
{
	const uint32_t *words;
	struct powervr_ir_operand condition;
	struct powervr_ir_operand discard;
	struct powervr_ir_operand all_ones;
	struct shader_spirv_id *label;
	uint32_t block;
	uint32_t at;
	uint32_t length;
	uint32_t opcode;
	uint32_t merge;
	uint32_t exited_true;
	uint32_t exited_false;
	int killed_true;
	int killed_false;
	uint32_t first;
	int error;

	/* Each block in turn. */
	block = start;
	first = through_first;
	for (;;) {
		/* The region ends where a branch reaches its stop. */
		if (block == stop && first == 0)
			return 0;
		first = 0;
		if (block >= reader->bound || reader->ids[block].kind != ID_LABEL)
			return EINVAL;
		label = &reader->ids[block];

		/* A loop header not yet entered is its whole loop, after which the merge follows. */
		if ((label->flags & FLAG_LOOP) != 0 && (label->flags & FLAG_ENTERED) == 0) {
			error = reader_loop(reader, block);
			if (error != 0)
				return error;
			block = label->merge;
			continue;
		}

		/* The block's instructions up to its terminator. */
		error = reader_block_body(reader, block, &at);
		if (error != 0)
			return error;
		words = &reader->words[at];
		length = words[0] >> 16;
		opcode = words[0] & 0xffffU;

		/* The terminator decides what follows. */
		if (opcode == OP_BRANCH) {
			/* An edge to the stop ends the region; an exit ends it too. */
			if (length < 2)
				return EINVAL;
			error = reader_phi_moves(reader, block, words[1]);
			if (error != 0)
				return error;
			if (words[1] == stop)
				return 0;
			error = reader_exit(reader, words[1], &exited_true);
			if (error != 0)
				return error;
			if (exited_true != 0)
				return 0;
			block = words[1];
		} else if (opcode == OP_BRANCH_CONDITIONAL) {
			/* The condition word. */
			if (length < 4)
				return EINVAL;
			error = shader_spirv_operand_word(reader, words[1], 0, &condition);
			if (error != 0)
				return error;
			merge = label->merge;
			if ((label->flags & FLAG_LOOP) != 0)
				merge = 0;

			/*
			 * A selection whose one arm only kills and whose other arm is
			 * the merge: the discard is selected, without branches (a
			 * fragment shader's alpha test, which every fragment runs).
			 */
			killed_true = 0;
			killed_false = 0;
			if (merge != 0 && reader->discard != 0 && words[3] == merge)
				killed_true = reader_kill_only(reader, words[2]);
			if (merge != 0 && reader->discard != 0 && words[2] == merge)
				killed_false = reader_kill_only(reader, words[3]);
			if (killed_true != 0 || killed_false != 0) {
				discard = shader_spirv_value_operand(reader->discard, 0);
				all_ones = shader_spirv_constant_operand(UINT32_MAX);
				if (killed_true != 0) {
					shader_spirv_emit(reader, POWERVR_IR_SELECT, reader->discard, &condition, &all_ones, &discard, 0, 0);
				} else {
					shader_spirv_emit(reader, POWERVR_IR_SELECT, reader->discard, &condition, &discard, &all_ones, 0, 0);
				}
				error = reader_phi_moves(reader, block, merge);
				if (error != 0)
					return error;
				if (merge == stop)
					return reader->error;
				block = merge;
			} else if (merge != 0) {
				/* A selection: an if over the two arms, then the merge. */
				shader_spirv_emit(reader, POWERVR_IR_IF, 0, &condition, NULL, NULL, 0, 0);
				error = reader_arm(reader, block, words[2], merge);
				if (error != 0)
					return error;
				shader_spirv_emit(reader, POWERVR_IR_ELSE, 0, NULL, NULL, NULL, 0, 0);
				error = reader_arm(reader, block, words[3], merge);
				if (error != 0)
					return error;
				shader_spirv_emit(reader, POWERVR_IR_END_IF, 0, NULL, NULL, NULL, 0, 0);
				if (merge == stop)
					return reader->error;
				block = merge;
			} else {
				/* A loop's conditional exit: the exiting arm in an if, the other one goes on. */
				if (reader->loop_count == 0)
					return ENOTSUP;
				exited_true = 0;
				exited_false = 0;
				if (words[2] == reader->loops[reader->loop_count - 1U].merge ||
				    words[2] == reader->loops[reader->loop_count - 1U].continue_target)
					exited_true = 1;
				if (words[3] == reader->loops[reader->loop_count - 1U].merge ||
				    words[3] == reader->loops[reader->loop_count - 1U].continue_target)
					exited_false = 1;
				if (exited_true == 0 && exited_false == 0)
					return ENOTSUP;
				if (exited_true != 0 && exited_false != 0) {
					shader_spirv_emit(reader, POWERVR_IR_IF, 0, &condition, NULL, NULL, 0, 0);
					error = reader_edge(reader, block, words[2]);
					if (error != 0)
						return error;
					shader_spirv_emit(reader, POWERVR_IR_ELSE, 0, NULL, NULL, NULL, 0, 0);
					error = reader_edge(reader, block, words[3]);
					if (error != 0)
						return error;
					shader_spirv_emit(reader, POWERVR_IR_END_IF, 0, NULL, NULL, NULL, 0, 0);
					return reader->error;
				}
				shader_spirv_emit(reader, POWERVR_IR_IF, 0, &condition, NULL, NULL, 0, 0);
				if (exited_true != 0) {
					error = reader_edge(reader, block, words[2]);
					if (error != 0)
						return error;
					shader_spirv_emit(reader, POWERVR_IR_END_IF, 0, NULL, NULL, NULL, 0, 0);
					error = reader_phi_moves(reader, block, words[3]);
					if (error != 0)
						return error;
					block = words[3];
				} else {
					shader_spirv_emit(reader, POWERVR_IR_ELSE, 0, NULL, NULL, NULL, 0, 0);
					error = reader_edge(reader, block, words[3]);
					if (error != 0)
						return error;
					shader_spirv_emit(reader, POWERVR_IR_END_IF, 0, NULL, NULL, NULL, 0, 0);
					error = reader_phi_moves(reader, block, words[2]);
					if (error != 0)
						return error;
					block = words[2];
				}
				if (block == stop)
					return reader->error;
			}
		} else if (opcode == OP_KILL || opcode == OP_TERMINATE_INVOCATION) {
			/*
			 * A kill of a fragment shader: the discard is set and the
			 * fragment goes on as a demoted one (its outputs are
			 * dropped by the feedback at the end).  Its arm ends here;
			 * at the outermost level the function returns.
			 *
			 * Procedure referenced:
			 * Mesa 26.2.5 src/imagination/pco/pco_nir.c
			 * (discard_is_demote, nir_lower_terminate_to_demote) and
			 * pco_nir_pvfio.c (lower_isp_fb: a demote stores true to
			 * the discard).
			 */
			if (reader->discard == 0 && reader->stage != POWERVR_SHADER_FRAGMENT)
				return ENOTSUP;
			if (reader->discard != 0) {
				condition = shader_spirv_constant_operand(UINT32_MAX);
				shader_spirv_emit(reader, POWERVR_IR_MOVE, reader->discard, &condition, NULL, NULL, 0, 0);
			}
			if (stop == 0 && reader->loop_count == 0) {
				reader->returned = 0;
				*returned = 1;
			}
			return reader->error;
		} else if (opcode == OP_RETURN || opcode == OP_RETURN_VALUE) {
			/* A return only ends the function from its outermost level. */
			if (reader->loop_count != 0 || stop != 0)
				return ENOTSUP;
			reader->returned = 0;
			if (opcode == OP_RETURN_VALUE) {
				if (length < 2)
					return EINVAL;
				reader->returned = words[1];
			}
			*returned = 1;
			return reader->error;
		} else {
			/* Switches and unreachable blocks are outside the subset. */
			return ENOTSUP;
		}

		/* Stops on an emission error. */
		if (reader->error != 0)
			return reader->error;
	}
}

/*
 * Walks one loop from its header: the body to the continue target, the
 * continue construct back to the header, between the loop markers.
 */
static int
reader_loop(
	struct shader_spirv_reader *reader,
	uint32_t header)
{
	struct shader_spirv_loop *loop;
	struct shader_spirv_id *label;
	uint32_t returned;
	int error;

	/* Refuses loops nested deeper than the reader allows. */
	if (reader->loop_count >= SHADER_SPIRV_LOOPS)
		return ENOTSUP;
	label = &reader->ids[header];

	/* Opens the loop. */
	shader_spirv_emit(reader, POWERVR_IR_LOOP, 0, NULL, NULL, NULL, 0, 0);
	loop = &reader->loops[reader->loop_count++];
	loop->header = header;
	loop->merge = label->merge;
	loop->continue_target = label->continue_target;
	loop->ifs = 0;
	label->flags |= FLAG_ENTERED;

	/* The body, then the continue construct back to the header. */
	returned = 0;
	if (loop->continue_target == header) {
		error = reader_region(reader, header, header, 1, &returned);
	} else {
		error = reader_region(reader, header, loop->continue_target, 0, &returned);
		if (error == 0) {
			/* The continued lanes come back where the continue construct starts. */
			shader_spirv_emit(reader, POWERVR_IR_LOOP_CONTINUE, 0, NULL, NULL, NULL, 0, 0);
			error = reader_region(reader, loop->continue_target, header, 0, &returned);
		}
	}
	label->flags &= (uint8_t)~FLAG_ENTERED;
	reader->loop_count--;
	if (error != 0)
		return error;

	/* Closes the loop. */
	shader_spirv_emit(reader, POWERVR_IR_END_LOOP, 0, NULL, NULL, NULL, 0, 0);
	return reader->error;
}

/* Walks one arm of a selection from its first block to the merge. */
static int
reader_arm(
	struct shader_spirv_reader *reader,
	uint32_t from,
	uint32_t target,
	uint32_t merge)
{
	uint32_t exited;
	uint32_t returned;
	int error;

	/* An arm straight to the merge only moves the merge's phis. */
	if (target == merge) {
		error = reader_phi_moves(reader, from, target);
		if (error != 0)
			return error;

		/* Succeeded: the merge's phis are moved. */
		return 0;
	}

	/* An arm that leaves the loop. */
	error = reader_phi_moves(reader, from, target);
	if (error != 0)
		return error;
	error = reader_exit(reader, target, &exited);
	if (error != 0)
		return error;
	if (exited != 0)
		return 0;

	/* Otherwise the blocks up to the merge. */
	returned = 0;
	error = reader_region(reader, target, merge, 0, &returned);
	if (error != 0)
		return error;

	/* Succeeded: the arm's blocks are read. */
	return 0;
}

/* Takes an edge that must leave the innermost loop: its phis, then the break or continue. */
static int
reader_edge(
	struct shader_spirv_reader *reader,
	uint32_t from,
	uint32_t target)
{
	uint32_t exited;
	int error;

	/* The target's phis on this edge. */
	error = reader_phi_moves(reader, from, target);
	if (error != 0)
		return error;

	/* The exit itself. */
	error = reader_exit(reader, target, &exited);
	if (error != 0)
		return error;
	if (exited == 0)
		return ENOTSUP;

	/* Succeeded: the edge is taken. */
	return 0;
}

/* Writes a break or continue when a branch target leaves the innermost loop. */
static int
reader_exit(
	struct shader_spirv_reader *reader,
	uint32_t target,
	uint32_t *exited)
{
	struct shader_spirv_loop *loop;

	/* Outside a loop, nothing exits. */
	*exited = 0;
	if (reader->loop_count == 0)
		return 0;
	loop = &reader->loops[reader->loop_count - 1U];

	/* The merge is a break, the continue target a continue. */
	if (target == loop->merge) {
		shader_spirv_emit(reader, POWERVR_IR_BREAK, 0, NULL, NULL, NULL, 0, 0);
		*exited = 1;
	} else if (target == loop->continue_target && target != loop->header) {
		shader_spirv_emit(reader, POWERVR_IR_CONTINUE, 0, NULL, NULL, NULL, 0, 0);
		*exited = 1;
	}

	/* Reports what the exit came to. */
	return reader->error;
}

/*
 * Moves the incoming values of target's phis on the edge from a block,
 * through copies so phis that read each other see the old values.
 */
static int
reader_phi_moves(
	struct shader_spirv_reader *reader,
	uint32_t from,
	uint32_t target)
{
	struct powervr_ir_operand staged[COMPOSITE_MAX * 4U];
	struct powervr_ir_operand *operands;
	struct powervr_ir_operand *incoming;
	uint32_t staged_count;
	uint32_t moved;
	uint32_t pass;
	uint32_t at;
	uint32_t length;
	uint32_t pair;
	uint32_t count;
	uint32_t phi_count;
	uint32_t word;
	int error;

	/* Two passes over the target's leading phis: stage the values, then move them in. */
	staged_count = 0;
	moved = 0;
	for (pass = 0; pass < 2; pass++) {
		error = shader_spirv_find_block(reader, target, &at);
		if (error != 0)
			return error;
		at += reader->words[at] >> 16;

		/* Each phi at the block's top. */
		while (at < reader->count && (reader->words[at] & 0xffffU) == OP_PHI) {
			length = reader->words[at] >> 16;
			if (length < 3 || at + length > reader->count || reader->words[at + 2] >= reader->bound)
				return EINVAL;

			/* The phi's own values, made on first sight. */
			if (reader->ids[reader->words[at + 2]].kind != ID_VALUE) {
				count = shader_spirv_scalar_count(reader, reader->words[at + 1]);
				if (count == 0 || count > COMPOSITE_MAX)
					return ENOTSUP;
				error = shader_spirv_result_begin(reader, reader->words[at + 2], reader->words[at + 1], count, &operands);
				if (error != 0)
					return error;
				for (word = 0; word < count; word++)
					operands[word] = shader_spirv_value_operand(shader_spirv_new_value(reader, 1), 0);
			}
			error = shader_spirv_id_operands(reader, reader->words[at + 2], &operands, &phi_count);
			if (error != 0)
				return error;

			/* The incoming value on this edge. */
			for (pair = 3; pair + 1 < length; pair += 2) {
				if (reader->words[at + pair + 1] != from)
					continue;
				error = shader_spirv_id_operands(reader, reader->words[at + pair], &incoming, &count);
				if (error != 0)
					return error;
				if (count != phi_count || staged_count + count > COMPOSITE_MAX * 4U)
					return ENOTSUP;
				for (word = 0; word < count; word++) {
					if (pass == 0) {
						staged[staged_count++] = shader_spirv_emit_value(reader, POWERVR_IR_MOVE, &incoming[word], NULL, NULL);
					} else {
						shader_spirv_emit(reader, POWERVR_IR_MOVE, operands[word].value, &staged[moved++], NULL, NULL, 0, 0);
					}
				}
				break;
			}
			at += length;
		}
	}

	/* Reports what the moves came to. */
	return reader->error;
}

/*
 * Reads one block's instructions up to its terminator, whose position
 * is reported.
 */
static int
reader_block_body(
	struct shader_spirv_reader *reader,
	uint32_t label,
	uint32_t *terminator)
{
	uint32_t at;
	uint32_t length;
	uint32_t opcode;
	int error;

	/* Starts after the label. */
	error = shader_spirv_find_block(reader, label, &at);
	if (error != 0)
		return error;
	at += reader->words[at] >> 16;

	/* Each instruction until a branch or return. */
	while (at < reader->count) {
		length = reader->words[at] >> 16;
		opcode = reader->words[at] & 0xffffU;
		if (length == 0 || at + length > reader->count)
			return EINVAL;
		if (opcode == OP_BRANCH ||
		    opcode == OP_BRANCH_CONDITIONAL ||
		    opcode == OP_RETURN ||
		    opcode == OP_RETURN_VALUE ||
		    (opcode >= 251U && opcode <= 255U)) {
			*terminator = at;
			return 0;
		}
		error = shader_spirv_reader_instruction(reader, &reader->words[at], length);
		if (error != 0)
			return error;
		at += length;
	}

	/* A block without a terminator. */
	return EINVAL;
}

/*
 * Reports whether a block holds nothing but a kill (OpKill or
 * OpTerminateInvocation right after its label).
 */
static int
reader_kill_only(
	struct shader_spirv_reader *reader,
	uint32_t label)
{
	uint32_t at;
	uint32_t opcode;
	int error;

	/* The instruction after the label. */
	error = shader_spirv_find_block(reader, label, &at);
	if (error != 0)
		return 0;
	at += reader->words[at] >> 16;
	if (at >= reader->count)
		return 0;

	/* A kill there ends the block at once. */
	opcode = reader->words[at] & 0xffffU;
	if (opcode == OP_KILL || opcode == OP_TERMINATE_INVOCATION)
		return 1;

	/* Anything else: not a kill-only block. */
	return 0;
}
