/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The render context record, the kick commands of a render and the
 * reading of a context reset notice.  Nothing here locks or allocates, so
 * the host test builds it as it is.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/render-kick-layout.h"
#include "drivers/gpu/powervr/interface.h"

/* The command's data follows its type and flags. */
#define COMMAND_DATA	8U

/*
 * The record and the command fields fit together as the firmware's own
 * structures do.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif_shared_check.h
 * (struct rogue_fwif_geom_registers_caswitch 184 bytes,
 * geom_state[1].geom_reg_vdm_context_store_task0 104,
 * geom_state[0].geom_reg_vdm_context_resume_task4 96; struct
 * rogue_fwif_static_rendercontext_state 368 bytes);
 * pvr_rogue_fwif_check.h (struct rogue_fwif_kccb_cmd, 88 bytes).
 */
_Static_assert(POWERVR_SWITCH_BUFFER + 2U * POWERVR_SWITCH_BUFFER_BYTES == POWERVR_SWITCH_CORE_BYTES, "switch registers");
_Static_assert(POWERVR_SWITCH_BUFFER + POWERVR_SWITCH_BUFFER_BYTES == 104U, "second buffer");
_Static_assert(POWERVR_SWITCH_BUFFER + POWERVR_SWITCH_RESUME_4 == 96U, "resume task 4");
_Static_assert(POWERVR_SWITCH_CORES * POWERVR_SWITCH_CORE_BYTES == 368U, "static render state");
_Static_assert(POWERVR_RENDER_CONTEXT_STATIC + 368U + 8U == POWERVR_RENDER_CONTEXT_BYTES, "render context");
_Static_assert(COMMAND_DATA + 2U * POWERVR_KICK_DATA_BYTES <= POWERVR_KICK_COMMAND_WORDS * 4U, "combined kick");
_Static_assert(POWERVR_KICK_CLEANUP_ADDRESSES + 4U * POWERVR_KICK_CLEANUPS_MAX == POWERVR_KICK_WORK_ESTIMATE, "cleanup counts");
_Static_assert(POWERVR_FRAGMENT_PARTIAL_BACKGROUND + 24U <= POWERVR_FRAGMENT_BYTES, "partial render background");

static void put32(uint8_t *bytes, uint32_t offset, uint32_t value);
static void put64(uint8_t *bytes, uint32_t offset, uint64_t value);
static uint32_t get32(const uint8_t *bytes, uint32_t offset);
static int kick_data(const struct powervr_kick *kick, uint32_t *data);
static void copy_bytes(uint8_t *to, const uint8_t *from, uint32_t bytes);
static void partial_background(uint8_t *fragment, const uint64_t *words);
static int reached(uint32_t now, uint32_t value);

/* Writes a little-endian 32-bit word. */
static void
put32(
	uint8_t *bytes,
	uint32_t offset,
	uint32_t value)
{
	/* Low byte first, as the firmware reads it. */
	bytes[offset + 0U] = (uint8_t)value;
	bytes[offset + 1U] = (uint8_t)(value >> 8);
	bytes[offset + 2U] = (uint8_t)(value >> 16);
	bytes[offset + 3U] = (uint8_t)(value >> 24);
}

/* Writes a little-endian 64-bit word. */
static void
put64(
	uint8_t *bytes,
	uint32_t offset,
	uint64_t value)
{
	/* The low half, then the high. */
	put32(bytes, offset, (uint32_t)value);
	put32(bytes, offset + 4U, (uint32_t)(value >> 32));
}

/* Reads a little-endian 32-bit word. */
static uint32_t
get32(
	const uint8_t *bytes,
	uint32_t offset)
{
	/* Low byte first. */
	return (uint32_t)bytes[offset] | (uint32_t)bytes[offset + 1U] << 8 |
	    (uint32_t)bytes[offset + 2U] << 16 | (uint32_t)bytes[offset + 3U] << 24;
}

/*
 * Fills the eight words of one kick's data.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_cccb.c
 * (pvr_cccb_send_kccb_kick: the context, the write offset, the wrap
 * mask, then the render target data set's cleanup count when the job has
 * one, and no work estimate).
 */
static int
kick_data(
	const struct powervr_kick *kick,
	uint32_t *data)
{
	unsigned index;

	/* Refuses more cleanup counts than the command holds. */
	if (kick->cleanup_count > POWERVR_KICK_CLEANUPS_MAX)
		return EINVAL;

	/* The context and its buffer's offsets, then the cleanup counts; the rest stays zero. */
	data[0] = kick->context;
	data[1] = kick->write_offset;
	data[2] = kick->wrap_mask;
	data[3] = kick->cleanup_count;
	for (index = 0; index < POWERVR_KICK_CLEANUPS_MAX; index++)
		data[4U + index] = index < kick->cleanup_count ? kick->cleanups[index] : 0U;
	data[7] = 0;

	/* Succeeded: the data. */
	return 0;
}

/* Builds the kick command of one context. */
int
powervr_kick_command(
	const struct powervr_kick *kick,
	uint32_t command[POWERVR_KICK_COMMAND_WORDS])
{
	unsigned index;
	int error;

	/* A zeroed command of the kick's type. */
	for (index = 0; index < POWERVR_KICK_COMMAND_WORDS; index++)
		command[index] = 0;
	command[0] = 0x2abc0000U | 101U;

	/* The kick's data. */
	error = kick_data(kick, &command[COMMAND_DATA / 4U]);
	if (error != 0)
		return error;

	/* Succeeded: the command. */
	return 0;
}

/*
 * Builds the combined kick of a geometry context and its partial render.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_cccb.c
 * (pvr_cccb_send_kccb_combined_kick: the geometry's kick data, then the
 * fragment's; the caller gives the partial render no cleanup count, as
 * the firmware attaches none to it).
 */
int
powervr_kick_combined_command(
	const struct powervr_kick *geometry,
	const struct powervr_kick *fragment,
	uint32_t command[POWERVR_KICK_COMMAND_WORDS])
{
	unsigned index;
	int error;

	/* A zeroed command of the combined type. */
	for (index = 0; index < POWERVR_KICK_COMMAND_WORDS; index++)
		command[index] = 0;
	command[0] = POWERVR_COMMAND_COMBINED_KICK;

	/* The geometry's data, then the fragment's. */
	error = kick_data(geometry, &command[COMMAND_DATA / 4U]);
	if (error != 0)
		return error;

	error = kick_data(fragment, &command[(COMMAND_DATA + POWERVR_KICK_DATA_BYTES) / 4U]);
	if (error != 0)
		return error;

	/* Succeeded: the command. */
	return 0;
}

/*
 * Puts the render target and the partial render test of a geometry
 * command.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_queue.c
 * (pvr_queue_submit_job_to_cccb: a paired geometry's
 * partial_render_geom_frag_fence is the geometry queue's timeline with
 * the job's sequence number minus one); pvr_job.c (the render target
 * data set's address in the shared part).
 */
void
powervr_geometry_partial_fence(
	uint8_t *command,
	uint32_t target_data,
	uint32_t timeline,
	uint32_t value)
{
	/* The data set, then the fence. */
	put32(command, POWERVR_SHARED_TARGET_DATA, target_data);
	put32(command, POWERVR_GEOMETRY_PARTIAL_FENCE, timeline);
	put32(command, POWERVR_GEOMETRY_PARTIAL_FENCE + 4U, value - 1U);
}

/*
 * Builds the words of a background object's programs.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_spm.c
 * (pvr_spm_init_bgobj_state: BGRND0 from the pixel and uniform programs'
 * places, BGRND1 from the texture data's, BGRND3 from the sizes).
 */
void
powervr_background_words(
	uint32_t pixel_code,
	uint32_t uniform_code,
	uint32_t texture_data,
	uint64_t words[POWERVR_BACKGROUND_WORDS])
{
	/* Each place keeps its bits 4 to 31; the sizes stay zero. */
	words[0] = (uint64_t)(pixel_code & 0xfffffff0U);
	words[0] |= (uint64_t)(uniform_code & 0xfffffff0U) << 32;
	words[1] = (uint64_t)(texture_data & 0xfffffff0U) << 32;
	words[2] = 0;
}

/* Puts the VDM control stream a geometry command runs, as its register's value. */
void
powervr_geometry_stream(
	uint8_t *command,
	uint64_t stream)
{
	/* The register keeps the address's bits 2 to 39. */
	put64(command, POWERVR_GEOMETRY_STREAM_BASE, stream & POWERVR_STREAM_BASE_MASK);
}

/* Puts the render target data set of a fragment or partial render command. */
void
powervr_fragment_target(
	uint8_t *command,
	uint32_t target_data)
{
	/* The shared part's data set. */
	put32(command, POWERVR_SHARED_TARGET_DATA, target_data);
}

/*
 * Fills the static state of a render context: the first core's
 * registers, with each buffer's own store and resume task words.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_context.c
 * (pvr_render_ctx_ws_static_state_init: the VDM and TA state buffers'
 * addresses, then for each of the two buffers its own programs' store
 * and resume task words, tasks 3 and 4 left zero); Linux v7.3
 * drivers/gpu/drm/imagination/pvr_stream_defs.c
 * (rogue_fwif_static_render_context_state_stream: the fields by name,
 * the resume address zero) and pvr_context.c (init_render_fw_objs: the
 * stream fills ctxswitch_regs[0] only).
 */
void
powervr_render_switch_state(
	uint8_t *record,
	uint64_t vdm_state,
	uint64_t ta_state,
	const struct powervr_switch_tasks tasks[2])
{
	static const uint32_t store_at[3] = {
		POWERVR_SWITCH_STORE_0, POWERVR_SWITCH_STORE_1, POWERVR_SWITCH_STORE_2,
	};
	static const uint32_t resume_at[3] = {
		POWERVR_SWITCH_RESUME_0, POWERVR_SWITCH_RESUME_1, POWERVR_SWITCH_RESUME_2,
	};
	uint32_t buffer;
	uint32_t base;
	unsigned task;

	/* The first core's state buffers. */
	base = POWERVR_RENDER_CONTEXT_STATIC;
	put64(record, base + POWERVR_SWITCH_VDM_STATE, vdm_state);
	put64(record, base + POWERVR_SWITCH_VDM_RESUME, 0);
	put64(record, base + POWERVR_SWITCH_TA_STATE, ta_state);

	/* Each buffer's tasks 0 to 2; tasks 3 and 4 stay zero. */
	for (buffer = 0; buffer < 2U; buffer++) {
		for (task = 0; task < 3U; task++) {
			put64(record, base + POWERVR_SWITCH_BUFFER + buffer * POWERVR_SWITCH_BUFFER_BYTES + store_at[task], tasks[buffer].store[task]);
			put64(record, base + POWERVR_SWITCH_BUFFER + buffer * POWERVR_SWITCH_BUFFER_BYTES + resume_at[task], tasks[buffer].resume[task]);
		}
	}
}

/* Reads a context reset notice's data. */
void
powervr_reset_notice_read(
	const uint8_t *data,
	struct powervr_reset_notice *notice)
{
	/* The fields the stage line and the session's lookup use. */
	notice->context = get32(data, POWERVR_RESET_CONTEXT);
	notice->reason = get32(data, POWERVR_RESET_REASON);
	notice->data_master = get32(data, POWERVR_RESET_DATA_MASTER);
	notice->flags = get32(data, POWERVR_RESET_FLAGS);
	notice->fault_address = (uint64_t)get32(data, POWERVR_RESET_FAULT_ADDRESS) |
	    (uint64_t)get32(data, POWERVR_RESET_FAULT_ADDRESS + 4U) << 32;
}

/* The stage a reset names and what its line says. */
void
powervr_reset_stage_of(
	const struct powervr_reset_notice *notice,
	enum powervr_context_kind kind,
	struct powervr_reset_stage *result)
{
	/* The stage: the context's kind, a render context's master. */
	if (kind == POWERVR_CONTEXT_TRANSFER)
		result->stage = POWERVR_STAGE_TRANSFER;
	else if (kind == POWERVR_CONTEXT_COMPUTE)
		result->stage = POWERVR_STAGE_COMPUTE;
	else if (notice->data_master == POWERVR_DATA_MASTER_GEOMETRY)
		result->stage = POWERVR_STAGE_TA;
	else
		result->stage = POWERVR_STAGE_3D;

	/* A page fault names its address; any other reset its reason. */
	if ((notice->flags & POWERVR_RESET_FLAG_FAULT) != 0U) {
		result->reason = POWERVR_REASON_FAULT;
		result->value = notice->fault_address;
	} else {
		result->reason = POWERVR_REASON_RESET;
		result->value = notice->reason;
	}
}

/*
 * Builds the commands of a render's three kicks.
 *
 * The geometry waits for the render's syncs, runs with its partial
 * render test on the geometry timeline at one less than its own value,
 * and writes its value.  The partial render waits for the same syncs,
 * the syncs of the 3D alone and then the geometry's value, last, runs,
 * and writes one less than the fragment's value.  The fragment waits for
 * the syncs of the 3D alone and the geometry's value, last, runs, and
 * writes its own.  Every fence is a partial render fence.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_queue.c
 * (pvr_queue_submit_job_to_cccb: the dependencies in FENCE_PR commands,
 * the paired geometry's completion as the fragment's last fence, the
 * geometry's partial_render_geom_frag_fence, then the command and the
 * update of the queue's timeline); pvr_job.c (the data set's address in
 * each render command's shared part); Mesa 26.2.5
 * src/imagination/vulkan/winsys/powervr/pvr_drm_job_render.c
 * (pvr_drm_winsys_render_submit: the geometry, its partial render and
 * the fragment, in that order).
 */
int
powervr_render_sequence_make(
	const struct powervr_render_request *request,
	struct powervr_render_sequence *sequence)
{
	struct powervr_client_command *command;
	unsigned index;
	unsigned waits;

	/* Refuses more waits than the partial render's fence holds with the geometry's. */
	if (request->wait_count > POWERVR_RENDER_WAITS_MAX)
		return EINVAL;
	if (request->fragment_wait_count > POWERVR_RENDER_WAITS_MAX - request->wait_count)
		return EINVAL;

	/* The firmware commands, each with the data set and the geometry with its test. */
	copy_bytes(sequence->geometry, request->geometry, POWERVR_GEOMETRY_BYTES);
	copy_bytes(sequence->partial, request->partial, POWERVR_FRAGMENT_BYTES);
	copy_bytes(sequence->fragment, request->fragment, POWERVR_FRAGMENT_BYTES);
	powervr_geometry_partial_fence(sequence->geometry, request->target_data, request->geometry_timeline, request->geometry_value);
	powervr_fragment_target(sequence->partial, request->target_data);
	powervr_fragment_target(sequence->fragment, request->target_data);
	if (request->partial_background != NULL)
		partial_background(sequence->fragment, request->partial_background);

	/*
	 * The syncs: the render's waits for the geometry and the partial
	 * render, the 3D's own for the partial render and the fragment, the
	 * geometry's completion last.
	 */
	for (index = 0; index < request->wait_count; index++) {
		sequence->geometry_waits[index] = request->waits[index];
		sequence->partial_waits[index] = request->waits[index];
	}
	for (index = 0; index < request->fragment_wait_count; index++) {
		sequence->partial_waits[request->wait_count + index] = request->fragment_waits[index];
		sequence->fragment_waits[index] = request->fragment_waits[index];
	}
	waits = request->wait_count + request->fragment_wait_count;
	sequence->partial_waits[waits].address = request->geometry_timeline;
	sequence->partial_waits[waits].value = request->geometry_value;
	sequence->fragment_waits[request->fragment_wait_count] = sequence->partial_waits[waits];

	/* The three updates: the geometry's value, one less than the fragment's, the fragment's. */
	sequence->updates[0].address = request->geometry_timeline;
	sequence->updates[0].value = request->geometry_value;
	sequence->updates[1].address = request->fragment_timeline;
	sequence->updates[1].value = request->fragment_value - 1U;
	sequence->updates[2].address = request->fragment_timeline;
	sequence->updates[2].value = request->fragment_value;

	/* The geometry's buffer: its fence when it waits, the command, the update. */
	sequence->geometry_count = 0;
	if (request->wait_count != 0U) {
		command = &sequence->geometry_commands[sequence->geometry_count++];
		command->type = POWERVR_CLIENT_FENCE_PARTIAL;
		command->size = request->wait_count * 8U;
		command->data = (const uint8_t *)sequence->geometry_waits;
	}
	command = &sequence->geometry_commands[sequence->geometry_count++];
	command->type = POWERVR_CLIENT_GEOMETRY;
	command->size = POWERVR_GEOMETRY_BYTES;
	command->data = sequence->geometry;
	command = &sequence->geometry_commands[sequence->geometry_count++];
	command->type = POWERVR_CLIENT_UPDATE;
	command->size = 8U;
	command->data = (const uint8_t *)&sequence->updates[0];

	/* The partial render's: the fence, the command, the update. */
	sequence->partial_count = 0;
	command = &sequence->partial_commands[sequence->partial_count++];
	command->type = POWERVR_CLIENT_FENCE_PARTIAL;
	command->size = (waits + 1U) * 8U;
	command->data = (const uint8_t *)sequence->partial_waits;
	command = &sequence->partial_commands[sequence->partial_count++];
	command->type = POWERVR_CLIENT_PARTIAL_RENDER;
	command->size = POWERVR_FRAGMENT_BYTES;
	command->data = sequence->partial;
	command = &sequence->partial_commands[sequence->partial_count++];
	command->type = POWERVR_CLIENT_UPDATE;
	command->size = 8U;
	command->data = (const uint8_t *)&sequence->updates[1];

	/* The fragment's: the 3D's own waits and the geometry's completion, the command, the update. */
	sequence->fragment_count = 0;
	command = &sequence->fragment_commands[sequence->fragment_count++];
	command->type = POWERVR_CLIENT_FENCE_PARTIAL;
	command->size = (request->fragment_wait_count + 1U) * 8U;
	command->data = (const uint8_t *)sequence->fragment_waits;
	command = &sequence->fragment_commands[sequence->fragment_count++];
	command->type = POWERVR_CLIENT_FRAGMENT;
	command->size = POWERVR_FRAGMENT_BYTES;
	command->data = sequence->fragment;
	command = &sequence->fragment_commands[sequence->fragment_count++];
	command->type = POWERVR_CLIENT_UPDATE;
	command->size = 8U;
	command->data = (const uint8_t *)&sequence->updates[2];

	/* Succeeded: the commands. */
	return 0;
}

/* Reports the stage a render that ran out of time stopped in. */
int
powervr_render_stalled(
	uint32_t geometry_now,
	uint32_t geometry_value,
	uint32_t fragment_now,
	uint32_t fragment_value,
	enum powervr_stage *stage)
{
	int geometry_done;
	int fragment_done;

	/* Which of the two kicks wrote its value. */
	geometry_done = reached(geometry_now, geometry_value);
	fragment_done = reached(fragment_now, fragment_value);

	/* The first kick that did not finish names the stage. */
	if (!geometry_done) {
		*stage = POWERVR_STAGE_TA;
		return 1;
	}

	if (!fragment_done) {
		*stage = POWERVR_STAGE_3D;
		return 1;
	}

	/* Succeeded: both finished. */
	return 0;
}

/*
 * Reports whether a render's free lists ran out: the firmware asked for
 * pages, or counted a partial render or an out-of-memory event.
 */
int
powervr_free_list_ran_out(
	const struct powervr_list_marks *before,
	const struct powervr_list_marks *after)
{
	int ran_out;

	/* Any of the three moved. */
	ran_out = 0;
	if (after->grow_requests != before->grow_requests)
		ran_out = 1;
	if (after->partial_renders != before->partial_renders)
		ran_out = 1;
	if (after->out_of_memory != before->out_of_memory)
		ran_out = 1;

	/* Succeeded: the answer. */
	return ran_out;
}

/* Copies bytes. */
static void
copy_bytes(
	uint8_t *to,
	const uint8_t *from,
	uint32_t bytes)
{
	uint32_t index;

	/* One byte at a time; the commands are short. */
	for (index = 0; index < bytes; index++)
		to[index] = from[index];
}

/* Puts a background after a partial render into a fragment command that names none. */
static void
partial_background(
	uint8_t *fragment,
	const uint64_t *words)
{
	unsigned index;

	/* A command that names its own keeps it. */
	for (index = 0; index < POWERVR_BACKGROUND_WORDS * 8U; index++) {
		if (fragment[POWERVR_FRAGMENT_PARTIAL_BACKGROUND + index] != 0U)
			return;
	}

	/* The default's three words. */
	for (index = 0; index < POWERVR_BACKGROUND_WORDS; index++)
		put64(fragment, POWERVR_FRAGMENT_PARTIAL_BACKGROUND + index * 8U, words[index]);
}

/* Reports whether a timeline word reached a value, across its wrap. */
static int
reached(
	uint32_t now,
	uint32_t value)
{
	int32_t distance;

	/* The word is at or past the value when the signed distance is not negative. */
	distance = (int32_t)(now - value);
	if (distance < 0)
		return 0;

	/* Succeeded: reached. */
	return 1;
}
