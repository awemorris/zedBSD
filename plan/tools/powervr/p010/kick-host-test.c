/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host check of ws269-p010n: the kick and combined kick commands against
 * the byte offsets of Linux v7.3 pvr_rogue_fwif_check.h, the geometry
 * command's partial render fence and the fragment's data set, the render
 * context's static state against pvr_rogue_fwif_shared_check.h, the
 * reading of a context reset notice into its stage, and the stage lines
 * that carry a fault's address or a reset's reason.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/render-kick-layout.h"
#include "drivers/gpu/powervr/stage.h"

static int failures;
static int passes;

/* One expectation. */
static void
expect(
	int good,
	const char *what)
{
	if (good) {
		passes++;
		return;
	}
	failures++;
	printf("FAIL %s\n", what);
}

/* Reads a little-endian word of a byte record. */
static uint64_t
read64(
	const uint8_t *bytes,
	unsigned offset)
{
	uint64_t value;
	unsigned index;

	value = 0;
	for (index = 0; index < 8U; index++)
		value |= (uint64_t)bytes[offset + index] << (8U * index);
	return value;
}

static uint32_t
read32(
	const uint8_t *bytes,
	unsigned offset)
{
	return (uint32_t)read64(bytes, offset);
}

/* Writes a little-endian word into a frame. */
static void
write32(
	uint8_t *bytes,
	unsigned offset,
	uint32_t value)
{
	unsigned index;

	for (index = 0; index < 4U; index++)
		bytes[offset + index] = (uint8_t)(value >> (8U * index));
}

static void
kicks(void)
{
	struct powervr_kick geometry = { 0x1000, 0x40, 0x7fff, { 0x2140 }, 1 };
	struct powervr_kick fragment = { 0x1098, 0x80, 0x7fff, { 0 }, 0 };
	struct powervr_kick wrong = { 0, 0, 0, { 0 }, 4 };
	uint32_t command[POWERVR_KICK_COMMAND_WORDS];

	/* The kick: type 101 with the magic, the data at 8 (kick_data offsets 0, 4, 8, 12, 16, 28). */
	expect(powervr_kick_command(&geometry, command) == 0, "kick");
	expect(command[0] == 0x2abc0065U, "kick type");
	expect(command[2] == 0x1000U && command[3] == 0x40U && command[4] == 0x7fffU, "kick context, offset, mask");
	expect(command[5] == 1U && command[6] == 0x2140U && command[7] == 0 && command[8] == 0, "kick cleanup counts");
	expect(command[9] == 0, "kick work estimate");
	expect(powervr_kick_command(&wrong, command) == EINVAL, "too many cleanups");

	/* The combined kick: type 117, the geometry's data at 8, the fragment's at 40. */
	expect(powervr_kick_combined_command(&geometry, &fragment, command) == 0, "combined");
	expect(command[0] == 0x2abc0075U, "combined type");
	expect(command[2] == 0x1000U && command[5] == 1U && command[6] == 0x2140U, "combined geometry");
	expect(command[10] == 0x1098U && command[11] == 0x80U && command[12] == 0x7fffU, "combined fragment");
	expect(command[13] == 0 && command[14] == 0, "partial render has no cleanup");
	expect(command[18] == 0 && command[21] == 0, "rest zero");
}

static void
commands(void)
{
	uint8_t geometry[POWERVR_GEOMETRY_BYTES];
	uint8_t fragment[POWERVR_FRAGMENT_BYTES];

	/* rogue_fwif_cmd_geom: shared hwrt_data_fw_addr 4, partial_render_geom_frag_fence 84 (addr, value). */
	memset(geometry, 0, sizeof(geometry));
	powervr_geometry_partial_fence(geometry, 0xabcd00U, 0x5000U, 7U);
	expect(read32(geometry, 4) == 0xabcd00U, "geometry data set");
	expect(read32(geometry, 84) == 0x5000U && read32(geometry, 88) == 6U, "partial fence is seqno - 1");
	expect(read32(geometry, 0) == 0 && read32(geometry, 92) == 0, "geometry rest");

	memset(fragment, 0, sizeof(fragment));
	powervr_fragment_target(fragment, 0xabcd00U);
	expect(read32(fragment, 4) == 0xabcd00U, "fragment data set");
	expect(POWERVR_FRAGMENT_PARTIAL_BACKGROUND == 416U, "pds_pr_bgnd at 16 + 400");
	expect(POWERVR_CLIENT_FENCE_PARTIAL == 0x2abc00d7U, "fence pr 215");
	expect(POWERVR_CLIENT_GEOMETRY == 0x2abc80c9U && POWERVR_CLIENT_PARTIAL_RENDER == 0x2abc80ccU, "geom 201, frag pr 204");
}

static void
switch_state(void)
{
	static uint8_t record[POWERVR_RENDER_CONTEXT_BYTES];
	const struct powervr_switch_tasks tasks[2] = {
		{ { 0x11, 0x12, 0x13 }, { 0x21, 0x22, 0x23 } },
		{ { 0x31, 0x32, 0x33 }, { 0x41, 0x42, 0x43 } },
	};
	unsigned base;

	/*
	 * ctxswitch_regs[0] at 304: 0, 8, 16, then geom_state[0] store 0-2 at
	 * 24..40, resume 0-2 at 48..64, store 3-4 at 72, 80, resume 3-4 at 88,
	 * 96; geom_state[1] from 104.  ctxswitch_regs[1] at 488 stays zero.
	 */
	memset(record, 0, sizeof(record));
	powervr_render_switch_state(record, 0x8000100000ULL, 0x8000200000ULL, tasks);
	base = 304U;
	expect(read64(record, base + 0) == 0x8000100000ULL, "vdm state base");
	expect(read64(record, base + 8) == 0, "vdm resume");
	expect(read64(record, base + 16) == 0x8000200000ULL, "ta state base");
	expect(read64(record, base + 24) == 0x11 && read64(record, base + 32) == 0x12 && read64(record, base + 40) == 0x13, "store 0-2");
	expect(read64(record, base + 48) == 0x21 && read64(record, base + 56) == 0x22 && read64(record, base + 64) == 0x23, "resume 0-2");
	expect(read64(record, base + 72) == 0 && read64(record, base + 80) == 0, "store 3-4 zero");
	expect(read64(record, base + 88) == 0 && read64(record, base + 96) == 0, "resume 3-4 zero");
	expect(read64(record, base + 104) == 0x31 && read64(record, base + 120) == 0x33, "second buffer store");
	expect(read64(record, base + 128) == 0x41 && read64(record, base + 144) == 0x43, "second buffer resume");
	expect(read64(record, 488) == 0 && read64(record, 488 + 24) == 0 && read64(record, 664) == 0, "second core untouched");
	expect(read64(record, 0) == 0 && read64(record, 672) == 0, "common contexts and flags untouched");
}

static void
resets(void)
{
	struct powervr_reset_notice notice;
	struct powervr_reset_stage stage;
	uint8_t data[POWERVR_RESET_BYTES];

	/* rogue_fwif_fwccb_cmd_context_reset_data: id 0, reason 4, dm 8, flags 16, fault 32. */
	memset(data, 0, sizeof(data));
	write32(data, 0, 7);
	write32(data, 4, 1);
	write32(data, 8, 2);
	write32(data, 16, 1);
	write32(data, 32, 0x80001234U);
	write32(data, 36, 0x80U);
	powervr_reset_notice_read(data, &notice);
	expect(notice.context == 7 && notice.reason == 1 && notice.data_master == 2 && notice.flags == 1, "notice fields");
	expect(notice.fault_address == 0x8080001234ULL, "fault address");

	/* A render context's geometry fault is the TA stage with its address. */
	powervr_reset_stage_of(&notice, POWERVR_CONTEXT_RENDER, &stage);
	expect(stage.stage == POWERVR_STAGE_TA && stage.reason == POWERVR_REASON_FAULT && stage.value == 0x8080001234ULL, "ta fault");

	/* The fragment master without a fault is the 3D stage with the reason. */
	write32(data, 8, 3);
	write32(data, 16, 0);
	powervr_reset_notice_read(data, &notice);
	powervr_reset_stage_of(&notice, POWERVR_CONTEXT_RENDER, &stage);
	expect(stage.stage == POWERVR_STAGE_3D && stage.reason == POWERVR_REASON_RESET && stage.value == 1, "3d reset");

	/* A transfer on the fragment master is the transfer stage; compute is compute. */
	powervr_reset_stage_of(&notice, POWERVR_CONTEXT_TRANSFER, &stage);
	expect(stage.stage == POWERVR_STAGE_TRANSFER, "transfer");
	powervr_reset_stage_of(&notice, POWERVR_CONTEXT_COMPUTE, &stage);
	expect(stage.stage == POWERVR_STAGE_COMPUTE, "compute");
}

static void
lines(void)
{
	struct powervr_stage_record record;
	char line[POWERVR_STAGE_LINE_MAX];

	memset(&record, 0, sizeof(record));
	expect(powervr_stage_note_fail_value(&record, POWERVR_STAGE_TA, POWERVR_REASON_FAULT, 0x80001234ULL, line, sizeof(line)), "fault printed");
	expect(strcmp(line, "powervr: ta: fail(fault 0x80001234)") == 0, "fault line");
	expect(powervr_stage_note_fail_value(&record, POWERVR_STAGE_3D, POWERVR_REASON_RESET, 1, line, sizeof(line)), "reset printed");
	expect(strcmp(line, "powervr: 3d: fail(reset lockup)") == 0, "reset line");
	(void)powervr_stage_note_fail_value(&record, POWERVR_STAGE_3D, POWERVR_REASON_RESET, 99, line, sizeof(line));
	expect(strcmp(line, "powervr: 3d: fail(reset 0x63)") == 0, "unknown reset");
	(void)powervr_stage_note_fail_value(&record, POWERVR_STAGE_3D, POWERVR_REASON_FAULT, 0, line, sizeof(line));
	expect(strcmp(line, "powervr: 3d: fail(fault 0x0)") == 0, "fault zero");
	(void)powervr_stage_note_fail(&record, POWERVR_STAGE_COMPUTE, POWERVR_REASON_RESET, line, sizeof(line));
	expect(strcmp(line, "powervr: compute: fail(reset)") == 0, "reset without value");
	(void)powervr_stage_note_fail_value(&record, POWERVR_STAGE_TRANSFER, POWERVR_REASON_FAULT, UINT64_MAX - 1U, line, sizeof(line));
	expect(strcmp(line, "powervr: transfer: fail(fault 0xfffffffffffffffe)") == 0, "longest line fits");
}

/* The command at a place of a sequence matches a type, a size and a word. */
static int
command_is(
	const struct powervr_client_command *command,
	uint32_t type,
	uint32_t size)
{
	return command->type == type && command->size == size;
}

static void
sequence(void)
{
	static struct powervr_render_sequence seq;
	static uint8_t geometry[POWERVR_GEOMETRY_BYTES];
	static uint8_t partial[POWERVR_FRAGMENT_BYTES];
	static uint8_t fragment[POWERVR_FRAGMENT_BYTES];
	struct powervr_render_request request;
	struct powervr_sync waits[POWERVR_RENDER_WAITS_MAX + 1U];
	struct powervr_sync fragment_waits[2];
	uint64_t background[POWERVR_BACKGROUND_WORDS];
	unsigned index;

	/*
	 * A render with two waits: geometry timeline 0x7000 value 5, fragment
	 * timeline 0x7100 value 9, data set 0x9000.  Geometry: FENCE_PR (2
	 * waits, 16 bytes), GEOM (112), UPDATE {0x7000, 5}; the GEOM carries
	 * the data set at 4 and the fence {0x7000, 4} at 84.  Partial render:
	 * FENCE_PR (the waits and {0x7000, 5} last, 24 bytes), FRAG_PR (480,
	 * data set at 4), UPDATE {0x7100, 8}.  Fragment: FENCE_PR {0x7000, 5},
	 * FRAG (480), UPDATE {0x7100, 9}.
	 */
	memset(geometry, 0xa5, sizeof(geometry));
	memset(partial, 0x5a, sizeof(partial));
	memset(fragment, 0x3c, sizeof(fragment));
	background[0] = 0x111;
	background[1] = 0x222;
	background[2] = 0x333;
	waits[0].address = 0x6000;
	waits[0].value = 1;
	waits[1].address = 0x6100;
	waits[1].value = 2;
	request.geometry_timeline = 0x7000;
	request.geometry_value = 5;
	request.fragment_timeline = 0x7100;
	request.fragment_value = 9;
	request.target_data = 0x9000;
	request.waits = waits;
	request.wait_count = 2;
	request.fragment_waits = NULL;
	request.fragment_wait_count = 0;
	request.geometry = geometry;
	request.partial = partial;
	request.fragment = fragment;
	request.partial_background = background;
	expect(powervr_render_sequence_make(&request, &seq) == 0, "sequence builds");
	expect(seq.geometry_count == 3 && seq.partial_count == 3 && seq.fragment_count == 3, "three commands each");
	expect(command_is(&seq.geometry_commands[0], 0x2abc00d7U, 16) && command_is(&seq.geometry_commands[1], 0x2abc80c9U, 112) && command_is(&seq.geometry_commands[2], 0x2abc00d5U, 8), "geometry commands");
	expect(read32(seq.geometry_commands[0].data, 0) == 0x6000 && read32(seq.geometry_commands[0].data, 12) == 2, "geometry waits");
	expect(read32(seq.geometry_commands[1].data, 4) == 0x9000 && read32(seq.geometry_commands[1].data, 84) == 0x7000 && read32(seq.geometry_commands[1].data, 88) == 4, "geometry data set and partial fence");
	expect(read32(seq.geometry_commands[1].data, 0) == 0xa5a5a5a5U && read32(seq.geometry_commands[1].data, 108) == 0xa5a5a5a5U, "geometry rest kept");
	expect(read32(seq.geometry_commands[2].data, 0) == 0x7000 && read32(seq.geometry_commands[2].data, 4) == 5, "geometry update");
	expect(command_is(&seq.partial_commands[0], 0x2abc00d7U, 24) && command_is(&seq.partial_commands[1], 0x2abc80ccU, 480) && command_is(&seq.partial_commands[2], 0x2abc00d5U, 8), "partial commands");
	expect(read32(seq.partial_commands[0].data, 8) == 0x6100 && read32(seq.partial_commands[0].data, 16) == 0x7000 && read32(seq.partial_commands[0].data, 20) == 5, "partial waits, geometry last");
	expect(read32(seq.partial_commands[1].data, 4) == 0x9000 && read32(seq.partial_commands[1].data, 8) == 0x5a5a5a5aU, "partial data set");
	expect(read32(seq.partial_commands[2].data, 0) == 0x7100 && read32(seq.partial_commands[2].data, 4) == 8, "partial update one less");
	expect(command_is(&seq.fragment_commands[0], 0x2abc00d7U, 8) && command_is(&seq.fragment_commands[1], 0x2abc80cbU, 480) && command_is(&seq.fragment_commands[2], 0x2abc00d5U, 8), "fragment commands");
	expect(read32(seq.fragment_commands[0].data, 0) == 0x7000 && read32(seq.fragment_commands[0].data, 4) == 5, "fragment waits for the geometry");
	expect(read32(seq.fragment_commands[1].data, 4) == 0x9000, "fragment data set");
	expect(read32(seq.fragment_commands[2].data, 0) == 0x7100 && read32(seq.fragment_commands[2].data, 4) == 9, "fragment update");
	expect(read64(seq.fragment_commands[1].data, 416) == 0x3c3c3c3c3c3c3c3cULL, "fragment's own background kept");

	/* A fragment naming no background takes the default at 16 + 400; the partial render does not. */
	memset(fragment, 0, sizeof(fragment));
	memset(partial, 0, sizeof(partial));
	expect(powervr_render_sequence_make(&request, &seq) == 0, "sequence with a default background");
	expect(read64(seq.fragment_commands[1].data, 416) == 0x111 && read64(seq.fragment_commands[1].data, 424) == 0x222 && read64(seq.fragment_commands[1].data, 432) == 0x333, "default background");
	expect(read64(seq.partial_commands[1].data, 416) == 0, "partial render background untouched");
	request.partial_background = NULL;
	expect(powervr_render_sequence_make(&request, &seq) == 0 && read64(seq.fragment_commands[1].data, 416) == 0, "no default: zero stays");

	/* BGRND0: shader 4..31, texunicode 36..63; BGRND1: texture data 36..63; BGRND3 zero. */
	powervr_background_words(0x123450, 0x678900, 0xabc00, background);
	expect(background[0] == 0x0067890000123450ULL && background[1] == 0x000abc0000000000ULL && background[2] == 0, "background words");

	/* The geometry's stream base at 16, bits 2..39. */
	memset(geometry, 0, sizeof(geometry));
	powervr_geometry_stream(geometry, 0x18000001237ULL);
	expect(read64(geometry, 16) == 0x8000001234ULL && read64(geometry, 24) == 0, "geometry stream base");

	/* No waits: the geometry has no fence; the partial render waits for the geometry alone. */
	request.wait_count = 0;
	expect(powervr_render_sequence_make(&request, &seq) == 0 && seq.geometry_count == 2, "no waits: geometry without a fence");
	expect(command_is(&seq.geometry_commands[0], 0x2abc80c9U, 112) && command_is(&seq.partial_commands[0], 0x2abc00d7U, 8), "no waits: partial fence of one");

	/* The most waits, and one more. */
	for (index = 0; index <= POWERVR_RENDER_WAITS_MAX; index++) {
		waits[index].address = 0x6000 + index * 8U;
		waits[index].value = index;
	}
	request.wait_count = POWERVR_RENDER_WAITS_MAX;
	expect(powervr_render_sequence_make(&request, &seq) == 0 && seq.partial_commands[0].size == 32U * 8U, "31 waits: a fence of 32");
	request.wait_count = POWERVR_RENDER_WAITS_MAX + 1U;
	expect(powervr_render_sequence_make(&request, &seq) == EINVAL, "32 waits refused");

	/*
	 * Waits of the 3D alone (a transfer the geometry runs beside): the
	 * geometry keeps its own; the partial render waits for both and the
	 * geometry last; the fragment for the 3D's and the geometry last.
	 */
	fragment_waits[0].address = 0x6800;
	fragment_waits[0].value = 7;
	fragment_waits[1].address = 0x6900;
	fragment_waits[1].value = 8;
	request.wait_count = 1;
	request.fragment_waits = fragment_waits;
	request.fragment_wait_count = 2;
	expect(powervr_render_sequence_make(&request, &seq) == 0, "3D waits: sequence builds");
	expect(command_is(&seq.geometry_commands[0], 0x2abc00d7U, 8) && read32(seq.geometry_commands[0].data, 0) == 0x6000, "3D waits: geometry keeps its one");
	expect(command_is(&seq.partial_commands[0], 0x2abc00d7U, 32) && read32(seq.partial_commands[0].data, 8) == 0x6800 &&
	       read32(seq.partial_commands[0].data, 16) == 0x6900 && read32(seq.partial_commands[0].data, 24) == 0x7000, "3D waits: partial render waits for all, geometry last");
	expect(command_is(&seq.fragment_commands[0], 0x2abc00d7U, 24) && read32(seq.fragment_commands[0].data, 0) == 0x6800 &&
	       read32(seq.fragment_commands[0].data, 12) == 8 && read32(seq.fragment_commands[0].data, 16) == 0x7000 && read32(seq.fragment_commands[0].data, 20) == 5,
	       "3D waits: fragment waits for the 3D's and the geometry last");
	request.wait_count = POWERVR_RENDER_WAITS_MAX - 1U;
	expect(powervr_render_sequence_make(&request, &seq) == EINVAL, "3D waits past the most refused");
	request.wait_count = POWERVR_RENDER_WAITS_MAX - 2U;
	expect(powervr_render_sequence_make(&request, &seq) == 0 && seq.partial_commands[0].size == 32U * 8U, "3D waits at the most: a fence of 32");
	expect(POWERVR_VDM_TERMINATE == 0xc0000000U, "VDM terminate block 6 << 29");
}

static void
stalls(void)
{
	struct powervr_list_marks before;
	struct powervr_list_marks after;
	enum powervr_stage stage;

	/* The first kick that did not write its value names the stage; values wrap. */
	expect(powervr_render_stalled(4, 5, 8, 9, &stage) == 1 && stage == POWERVR_STAGE_TA, "geometry short: ta");
	expect(powervr_render_stalled(5, 5, 8, 9, &stage) == 1 && stage == POWERVR_STAGE_3D, "fragment short: 3d");
	expect(powervr_render_stalled(5, 5, 9, 9, &stage) == 0, "both reached");
	expect(powervr_render_stalled(6, 5, 10, 9, &stage) == 0, "past both");
	expect(powervr_render_stalled(0xffffffffU, 1, 0, 0, &stage) == 1 && stage == POWERVR_STAGE_TA, "geometry short across the wrap");
	expect(powervr_render_stalled(2, 1, 0xfffffffeU, 0xffffffffU, &stage) == 1 && stage == POWERVR_STAGE_3D, "fragment short before the wrap");

	/* Any of the free list reports moving says the lists ran out. */
	before.grow_requests = 1;
	before.partial_renders = 2;
	before.out_of_memory = 3;
	after = before;
	expect(powervr_free_list_ran_out(&before, &after) == 0, "nothing moved");
	after.grow_requests = 2;
	expect(powervr_free_list_ran_out(&before, &after) == 1, "a grow request");
	after = before;
	after.partial_renders = 3;
	expect(powervr_free_list_ran_out(&before, &after) == 1, "a partial render");
	after = before;
	after.out_of_memory = 4;
	expect(powervr_free_list_ran_out(&before, &after) == 1, "out of memory");
}

int
main(void)
{
	kicks();
	commands();
	switch_state();
	resets();
	lines();
	sequence();
	stalls();
	printf("kick-host-test: %d passed, %d failed\n", passes, failures);
	return failures == 0 ? 0 : 1;
}
