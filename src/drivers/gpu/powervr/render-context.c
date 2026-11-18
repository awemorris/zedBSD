/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR render node's render contexts.
 *
 * A session's first render makes its render context: a geometry context
 * and a fragment context, each with a command buffer and a timeline of
 * its own, under one record of the firmware that also holds the state
 * the firmware switches the geometry work with.  The switch stores and
 * resumes the shared registers and the persistent temporaries through
 * programs and buffers in the session's windows, two sets of them, one
 * per buffer of the switch.  A compute-only session makes none.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/render-private.h"
#include "drivers/gpu/powervr/interface.h"
#include "drivers/gpu/powervr/compiler/pds-encode.h"
#include "drivers/gpu/powervr/render-sr-program.h"
#include "drivers/gpu/powervr/vk/vk.h"

/*
 * Where the switch's memory sits in the general window: the VDM call
 * stack, then the VDM and TA state buffers, each on 16 bytes.
 */
#define GENERAL_CALL_STACK		0x0U
#define GENERAL_VDM_STATE		0x10U
#define GENERAL_TA_STATE		0x70U
#define GENERAL_BYTES			0x260U

/*
 * Where one set's memory sits in the data sequencer's window: the shared
 * register state, the persistent temporaries, then the four data
 * sequencer programs, each data then code, on 16 bytes, and last the
 * stand-in background program (used from the first set only).  The
 * shared register programs' code is 64 bytes after their data, the
 * persistent temporaries' right after theirs.
 */
#define SET_SHARED_STATE		0x0U
#define SET_PERSISTENT			0x4040U
#define SET_SHARED_STORE		0x40c0U
#define SET_SHARED_LOAD			0x4110U
#define SET_PERSISTENT_STORE		0x4160U
#define SET_PERSISTENT_RESUME		0x41a0U
#define SET_BACKGROUND			0x41e0U
#define SET_SHARED_CODE			0x40U
#define SET_PERSISTENT_CODE		0x20U
#define SET_BYTES			0x41f0U

/* Where the shading cluster programs sit in theirs: the store, then the load. */
#define SHADER_STORE			0x0U
#define SHADER_LOAD			POWERVR_SHARED_USC_BYTES_MAX
#define SHADER_BYTES			(2U * POWERVR_SHARED_USC_BYTES_MAX)

_Static_assert(GENERAL_VDM_STATE >= GENERAL_CALL_STACK + POWERVR_SWITCH_CALL_STACK_BYTES, "call stack");
_Static_assert(GENERAL_TA_STATE >= GENERAL_VDM_STATE + POWERVR_SWITCH_VDM_BYTES, "vdm state");
_Static_assert(GENERAL_BYTES >= GENERAL_TA_STATE + POWERVR_SWITCH_TA_BYTES, "ta state");
_Static_assert(SET_PERSISTENT >= SET_SHARED_STATE + POWERVR_SWITCH_SHARED_BYTES, "shared state");
_Static_assert(SET_SHARED_STORE >= SET_PERSISTENT + POWERVR_SWITCH_PERSISTENT_BYTES, "persistent state");
_Static_assert(SET_SHARED_CODE >= POWERVR_SHARED_PDS_DATA_WORDS * 4U, "shared program data");
_Static_assert(SET_SHARED_LOAD >= SET_SHARED_STORE + SET_SHARED_CODE + POWERVR_SHARED_PDS_CODE_WORDS * 4U, "shared store program");
_Static_assert(SET_PERSISTENT_STORE >= SET_SHARED_LOAD + SET_SHARED_CODE + POWERVR_SHARED_PDS_CODE_WORDS * 4U, "shared load program");
_Static_assert(SET_PERSISTENT_CODE == POWERVR_PERSISTENT_DATA_WORDS * 4U, "persistent program data");
_Static_assert(SET_PERSISTENT_RESUME >= SET_PERSISTENT_STORE + SET_PERSISTENT_CODE + POWERVR_PERSISTENT_STORE_CODE_WORDS * 4U, "persistent store program");
_Static_assert(SET_BACKGROUND >= SET_PERSISTENT_RESUME + SET_PERSISTENT_CODE + POWERVR_PERSISTENT_RESUME_CODE_WORDS * 4U, "persistent resume program");
_Static_assert(SET_BYTES >= SET_BACKGROUND + 4U, "background program");
_Static_assert(sizeof(((struct powervr_switch_shaders *)0)->store) == POWERVR_SHARED_USC_BYTES_MAX, "shader room");

/*
 * Where the compute switch's memory sits in the data sequencer's window:
 * per buffer the shared register state, the store program and the load
 * program (data, code 64 bytes after); after both, the fence terminate
 * program (data, code 16 bytes after).  The shading cluster programs
 * are the render switch's two, store then load.
 */
#define COMPUTE_SET_BYTES		0x4140U
#define COMPUTE_SET_STORE		0x4040U
#define COMPUTE_SET_LOAD		0x40c0U
#define COMPUTE_TERMINATE		(2U * COMPUTE_SET_BYTES)
#define COMPUTE_TERMINATE_CODE		0x10U
#define COMPUTE_PDS_BYTES		(COMPUTE_TERMINATE + 0x30U)
#define COMPUTE_STATIC			152U

_Static_assert(COMPUTE_SET_STORE >= POWERVR_SWITCH_SHARED_BYTES, "compute shared state");
_Static_assert(COMPUTE_SET_LOAD >= COMPUTE_SET_STORE + SET_SHARED_CODE + POWERVR_COMPUTE_SHARED_CODE_WORDS * 4U, "compute store program");
_Static_assert(COMPUTE_SET_BYTES >= COMPUTE_SET_LOAD + SET_SHARED_CODE + POWERVR_COMPUTE_SHARED_CODE_WORDS * 4U, "compute load program");
_Static_assert(COMPUTE_TERMINATE_CODE >= POWERVR_COMPUTE_TERMINATE_DATA_WORDS * 4U, "terminate data");
_Static_assert(COMPUTE_PDS_BYTES >= COMPUTE_TERMINATE + COMPUTE_TERMINATE_CODE + POWERVR_COMPUTE_TERMINATE_CODE_WORDS * 4U, "terminate program");

/* The contexts powervr_session_next_cleanup() names. */
#define CLEAN_COMPUTE			0U
#define CLEAN_GEOMETRY			1U
#define CLEAN_FRAGMENT			2U
#define CLEAN_TRANSFER			3U

static int context_memory(struct powervr_render *render, struct powervr_session *session, struct powervr_render_context *context);
static int context_records(struct powervr_render *render, struct powervr_session *session, struct powervr_render_context *context);
static int client_make(struct powervr_heap *heap, struct powervr_client *client);
static void client_destroy(struct powervr_heap *heap, struct powervr_client *client);
static void client_drop(struct powervr_client *client);
static void client_fill(struct powervr_client *client);
static void common_fill(struct powervr_session *session, struct powervr_render_context *context, uint32_t place, struct powervr_client *client, struct powervr_object *state, uint32_t master);
static int set_write(struct powervr_buffer *buffer, uint64_t place, uint64_t shader_place, struct powervr_switch_tasks *tasks);
static int background_write(struct powervr_render_context *context);
static void view_put32(struct powervr_buffer *buffer, uint32_t offset, uint32_t value);
static void view_words(struct powervr_buffer *buffer, uint32_t offset, const uint32_t *words, unsigned count);
static void view_bytes(struct powervr_buffer *buffer, uint32_t offset, const uint8_t *bytes, size_t count);
static int memory_buffer(struct powervr_render *render, uint32_t bytes, struct powervr_buffer **result);

/*
 * Builds the shading cluster programs of a render context's switch, once
 * while the node starts.
 */
int
powervr_render_switch_shaders(
	struct powervr_render *render)
{
	struct powervr_switch_shaders *shaders;
	int error;

	/* The store, then the load. */
	shaders = &render->switch_shaders;
	error = powervr_shared_usc_program(0, shaders->store, sizeof(shaders->store), &shaders->store_bytes);
	if (error != 0)
		return error;

	error = powervr_shared_usc_program(1, shaders->load, sizeof(shaders->load), &shaders->load_bytes);
	if (error != 0)
		return error;

	/* Succeeded: render contexts can be made. */
	shaders->built = 1;
	return 0;
}

/*
 * Finds or makes a session's render context.
 *
 * The memory the switch uses is made and mapped first, without the
 * core's lock; then the firmware's records, filled under it.  What was
 * made is undone when a later part fails; nothing was kicked yet.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_context.c
 * (pvr_context_create, init_render_fw_objs: the render context's record
 * with its static state, then its geometry and fragment queues);
 * pvr_queue.c (pvr_queue_create: each queue's command buffer, its state
 * block, its common context and its timeline); Mesa 26.2.5
 * src/imagination/vulkan/pvr_arch_job_context.c (pvr_render_ctx_create:
 * the VDM call stack, then the switch's buffers and programs).
 */
int
powervr_session_render_context(
	struct powervr_render *render,
	struct powervr_session *session,
	struct powervr_render_context **result)
{
	struct powervr_render_context *context;
	uint64_t started;
	int error;

	/* The session's own, made by its first render. */
	*result = session->render_context;
	if (*result != NULL)
		return 0;

	/* Without the switch's programs the firmware could not switch the context. */
	if (!render->switch_shaders.built)
		return EIO;

	/* The bookkeeping, kept by the session from here so a failure is undone by the destroy. */
	context = kern_calloc(1, sizeof(*context));
	if (context == NULL)
		return ENOMEM;
	session->render_context = context;

	/* The switch's memory in the session's windows, timed with the records. */
	started = powervr_timing_now();
	error = context_memory(render, session, context);
	if (error != 0)
		goto fail;

	/* The firmware's records, filled under the core's lock. */
	mutex_lock(&render->core->lock);

	error = context_records(render, session, context);

	mutex_unlock(&render->core->lock);
	powervr_timing_add(POWERVR_TIMING_CONTEXT_MAKE, started);

	if (error != 0)
		goto fail;

	/* Succeeded: the render context is the session's. */
	*result = context;
	return 0;

fail:
	/* Destroys the part-made context and reports the failure. */
	powervr_render_context_destroy(render, session);
	return error;
}

/*
 * Returns a session's render context once the firmware no longer uses
 * it: its records, then the switch's memory.
 */
void
powervr_render_context_destroy(
	struct powervr_render *render,
	struct powervr_session *session)
{
	struct powervr_render_context *context;
	struct powervr_heap *heap;
	unsigned set;

	/* Nothing made. */
	context = session->render_context;
	if (context == NULL)
		return;

	/* The records, unmapped from the firmware's processor. */
	heap = &render->core->firmware.heap;
	mutex_lock(&render->core->lock);

	client_destroy(heap, &context->fragment);
	client_destroy(heap, &context->geometry);
	powervr_object_destroy(heap, &context->fragment_state);
	powervr_object_destroy(heap, &context->geometry_state);
	powervr_object_destroy(heap, &context->record);

	mutex_unlock(&render->core->lock);

	/* The programs, the sets and the general memory. */
	if (context->shaders != NULL) {
		if (context->shader_place != 0U)
			powervr_session_program_unmap(session, POWERVR_PROGRAM_USC, context->shaders, context->shader_place);
		powervr_buffer_put(render, context->shaders);
	}

	for (set = 0; set < 2U; set++) {
		if (context->programs[set] == NULL)
			continue;
		if (context->program_places[set] != 0U)
			powervr_session_program_unmap(session, POWERVR_PROGRAM_PDS, context->programs[set], context->program_places[set]);
		powervr_buffer_put(render, context->programs[set]);
	}

	if (context->switch_memory != NULL)
		powervr_resource_unmake(session, context->switch_memory);

	/* The bookkeeping. */
	kern_free(context);
	session->render_context = NULL;
}

/*
 * Returns a kept session's render context once the firmware that knew it
 * is gone; the core's lock is held.
 *
 * The records' old places in the firmware's heap and the mappings, whose
 * tables go with the session, are not touched.
 */
void
powervr_render_context_discard(
	struct powervr_session *session)
{
	struct powervr_render_context *context;
	struct powervr_render *render;
	unsigned set;

	/* Nothing made. */
	render = session->render;
	context = session->render_context;
	if (context == NULL)
		return;

	/* The records' runs. */
	client_drop(&context->fragment);
	client_drop(&context->geometry);
	powervr_object_drop(&context->fragment_state);
	powervr_object_drop(&context->geometry_state);
	powervr_object_drop(&context->record);

	/* The memory: each buffer's hold, and the general memory's handle. */
	if (context->shaders != NULL)
		powervr_buffer_put(render, context->shaders);
	for (set = 0; set < 2U; set++) {
		if (context->programs[set] != NULL)
			powervr_buffer_put(render, context->programs[set]);
	}

	if (context->switch_memory != NULL) {
		powervr_buffer_put(render, context->switch_memory->buffer);
		session->resources--;
		kern_free(context->switch_memory);
	}

	/* The bookkeeping. */
	kern_free(context);
	session->render_context = NULL;
}

/*
 * Writes the compute context's static state.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif.h (struct
 * rogue_fwif_fwcomputecontext: the static compute context state after
 * the 152-byte common context).
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_context.c
 * (pvr_arch_compute_ctx_create: two sets of shared register programs,
 * the fence terminate program, the static state from them).
 */
int
powervr_session_compute_switch(
	struct powervr_render *render,
	struct powervr_session *session)
{
	struct powervr_compute_switch_programs programs;
	uint64_t words[POWERVR_COMPUTE_SWITCH_WORDS];
	uint32_t data[POWERVR_SHARED_PDS_DATA_WORDS];
	uint32_t code[POWERVR_COMPUTE_SHARED_CODE_WORDS];
	uint64_t usc;
	uint64_t place;
	uint32_t base;
	unsigned set;
	unsigned index;
	int error;

	/* Without the switch's programs the static state stays zero. */
	if (!render->switch_shaders.built)
		return EIO;

	/* The programs' buffers, mapped. */
	error = memory_buffer(render, COMPUTE_PDS_BYTES, &session->compute_switch_pds);
	if (error != 0)
		return error;

	error = powervr_session_program_map(session, POWERVR_PROGRAM_PDS, session->compute_switch_pds, &session->compute_switch_pds_place);
	if (error != 0) {
		session->compute_switch_pds_place = 0;
		return error;
	}

	error = memory_buffer(render, SHADER_BYTES, &session->compute_switch_usc);
	if (error != 0)
		return error;

	view_bytes(session->compute_switch_usc, SHADER_STORE, render->switch_shaders.store, render->switch_shaders.store_bytes);
	view_bytes(session->compute_switch_usc, SHADER_LOAD, render->switch_shaders.load, render->switch_shaders.load_bytes);
	error = powervr_session_program_map(session, POWERVR_PROGRAM_USC, session->compute_switch_usc, &session->compute_switch_usc_place);
	if (error != 0) {
		session->compute_switch_usc_place = 0;
		return error;
	}

	/* Each buffer's store and load programs. */
	place = session->compute_switch_pds_place;
	base = (uint32_t)(place - POWERVR_PDS_HEAP_BASE);
	usc = session->compute_switch_usc_place - POWERVR_USC_HEAP_BASE;
	for (set = 0; set < 2U; set++) {
		error = powervr_compute_shared_pds_program(place + set * COMPUTE_SET_BYTES, usc + SHADER_STORE, POWERVR_SHARED_USC_TEMPORARIES, data, code);
		if (error != 0)
			return error;

		view_words(session->compute_switch_pds, set * COMPUTE_SET_BYTES + COMPUTE_SET_STORE, data, POWERVR_SHARED_PDS_DATA_WORDS);
		view_words(session->compute_switch_pds, set * COMPUTE_SET_BYTES + COMPUTE_SET_STORE + SET_SHARED_CODE, code, POWERVR_COMPUTE_SHARED_CODE_WORDS);
		error = powervr_compute_shared_pds_program(place + set * COMPUTE_SET_BYTES, usc + SHADER_LOAD, POWERVR_SHARED_USC_TEMPORARIES, data, code);
		if (error != 0)
			return error;

		view_words(session->compute_switch_pds, set * COMPUTE_SET_BYTES + COMPUTE_SET_LOAD, data, POWERVR_SHARED_PDS_DATA_WORDS);
		view_words(session->compute_switch_pds, set * COMPUTE_SET_BYTES + COMPUTE_SET_LOAD + SET_SHARED_CODE, code, POWERVR_COMPUTE_SHARED_CODE_WORDS);
		programs.store_data[set] = base + set * COMPUTE_SET_BYTES + COMPUTE_SET_STORE;
		programs.store_code[set] = programs.store_data[set] + SET_SHARED_CODE;
		programs.load_data[set] = base + set * COMPUTE_SET_BYTES + COMPUTE_SET_LOAD;
		programs.load_code[set] = programs.load_data[set] + SET_SHARED_CODE;
	}

	/* The fence terminate program. */
	error = powervr_compute_terminate_program(data, code);
	if (error != 0)
		return error;

	view_words(session->compute_switch_pds, COMPUTE_TERMINATE, data, POWERVR_COMPUTE_TERMINATE_DATA_WORDS);
	view_words(session->compute_switch_pds, COMPUTE_TERMINATE + COMPUTE_TERMINATE_CODE, code, POWERVR_COMPUTE_TERMINATE_CODE_WORDS);
	programs.terminate_data = base + COMPUTE_TERMINATE;
	programs.terminate_code = base + COMPUTE_TERMINATE + COMPUTE_TERMINATE_CODE;

	/* The static state, in the context's record. */
	powervr_compute_switch_words(&programs, words);
	mutex_lock(&render->core->lock);

	for (index = 0; index < POWERVR_COMPUTE_SWITCH_WORDS; index++)
		powervr_object_put64(&session->context, COMPUTE_STATIC + index * 8U, words[index]);

	mutex_unlock(&render->core->lock);

	/* Succeeded: the context can be switched. */
	return 0;
}

/* Returns the compute switch's memory; a discard leaves its mappings, whose tables go with the session. */
void
powervr_compute_switch_destroy(
	struct powervr_render *render,
	struct powervr_session *session,
	unsigned discard)
{
	/* The shading cluster programs. */
	if (session->compute_switch_usc != NULL) {
		if (!discard && session->compute_switch_usc_place != 0U)
			powervr_session_program_unmap(session, POWERVR_PROGRAM_USC, session->compute_switch_usc, session->compute_switch_usc_place);
		powervr_buffer_put(render, session->compute_switch_usc);
		session->compute_switch_usc = NULL;
	}

	/* The data sequencer programs and the state. */
	if (session->compute_switch_pds != NULL) {
		if (!discard && session->compute_switch_pds_place != 0U)
			powervr_session_program_unmap(session, POWERVR_PROGRAM_PDS, session->compute_switch_pds, session->compute_switch_pds_place);
		powervr_buffer_put(render, session->compute_switch_pds);
		session->compute_switch_pds = NULL;
	}
}

/*
 * Reports the firmware address of the next context of a session the
 * firmware still has to clean up, or zero: the compute context, the
 * render context's geometry and fragment contexts, the transfer context.
 *
 * A context that never ran needs no cleanup.  A render context's
 * geometry context lies at the start of its record, its fragment context
 * after it.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_queue.c (get_ctx_offset:
 * offsetof(struct rogue_fwif_fwrendercontext, geom_context) and
 * frag_context).
 */
uint32_t
powervr_session_next_cleanup(
	const struct powervr_session *session,
	unsigned *which)
{
	const struct powervr_render_context *context;
	uint32_t address;

	/* The compute context first. */
	if (session->kicked && !session->cleaned) {
		*which = CLEAN_COMPUTE;
		address = powervr_object_address(&session->context, 0);
		return address;
	}

	/* Then the render context's two, once it ran. */
	context = session->render_context;
	if (context != NULL && context->kicked && !context->geometry_cleaned) {
		*which = CLEAN_GEOMETRY;
		address = powervr_object_address(&context->record, POWERVR_RENDER_CONTEXT_GEOMETRY);
		return address;
	}

	if (context != NULL && context->kicked && !context->fragment_cleaned) {
		*which = CLEAN_FRAGMENT;
		address = powervr_object_address(&context->record, POWERVR_RENDER_CONTEXT_FRAGMENT);
		return address;
	}

	/* Then the transfer context, once it ran. */
	if (session->transfer_context != NULL && session->transfer_context->kicked && !session->transfer_context->cleaned) {
		*which = CLEAN_TRANSFER;
		address = powervr_object_address(&session->transfer_context->record, 0);
		return address;
	}

	/* Succeeded: none is left. */
	return 0;
}

/* Notes that the firmware cleaned up a context of a session. */
void
powervr_session_cleaned(
	struct powervr_session *session,
	unsigned which)
{
	/* The flag of the context named; the firmware forgot it. */
	if (which == CLEAN_COMPUTE) {
		session->cleaned = 1;
	} else if (which == CLEAN_GEOMETRY) {
		session->render_context->geometry_cleaned = 1;
	} else if (which == CLEAN_FRAGMENT) {
		session->render_context->fragment_cleaned = 1;
	} else {
		session->transfer_context->cleaned = 1;
	}
}

/*
 * Makes and maps the memory the switch uses: the general memory, the two
 * sets with their programs, the shading cluster programs.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_context.c
 * (pvr_render_ctx_switch_init: the VDM and geometry state buffers in the
 * general heap, uncached for the core; pvr_render_ctx_switch_programs_setup
 * for each of the two sets: the persistent temporaries' buffer and the
 * shared registers' buffer in the data sequencer's heap, their programs;
 * pvr_render_ctx_create: the VDM call stack in the general heap).
 */
static int
context_memory(
	struct powervr_render *render,
	struct powervr_session *session,
	struct powervr_render_context *context)
{
	struct powervr_buffer *buffer;
	int error;

	/* The general memory: the call stack and the VDM and TA state. */
	error = memory_buffer(render, GENERAL_BYTES, &buffer);
	if (error != 0)
		return error;

	error = powervr_resource_make(session, buffer, 1, 0, &context->switch_memory);
	powervr_buffer_put(render, buffer);
	if (error != 0)
		return error;

	/* The shading cluster programs, mapped in the cluster's window. */
	error = memory_buffer(render, SHADER_BYTES, &context->shaders);
	if (error != 0)
		return error;

	view_bytes(context->shaders, SHADER_STORE, render->switch_shaders.store, render->switch_shaders.store_bytes);
	view_bytes(context->shaders, SHADER_LOAD, render->switch_shaders.load, render->switch_shaders.load_bytes);
	error = powervr_session_program_map(session, POWERVR_PROGRAM_USC, context->shaders, &context->shader_place);
	if (error != 0) {
		context->shader_place = 0;
		return error;
	}

	/* The two sets, each mapped in the data sequencer's window; their programs are written once their places are known. */
	error = memory_buffer(render, SET_BYTES, &context->programs[0]);
	if (error != 0)
		return error;

	error = powervr_session_program_map(session, POWERVR_PROGRAM_PDS, context->programs[0], &context->program_places[0]);
	if (error != 0) {
		context->program_places[0] = 0;
		return error;
	}

	error = memory_buffer(render, SET_BYTES, &context->programs[1]);
	if (error != 0)
		return error;

	error = powervr_session_program_map(session, POWERVR_PROGRAM_PDS, context->programs[1], &context->program_places[1]);
	if (error != 0) {
		context->program_places[1] = 0;
		return error;
	}

	/* Succeeded: the memory is mapped. */
	return 0;
}

/*
 * Makes and fills the firmware's records of a render context; the core's
 * lock is held.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_queue.c (reg_state_init:
 * the VDM call stack's address in the first geometry core's initial
 * pointer; init_fw_context: each common context's command buffer, data
 * master, priority, deadline, the context's identity, the memory context
 * and the state block) and pvr_context.c (one identity for the render
 * context's two queues).
 */
static int
context_records(
	struct powervr_render *render,
	struct powervr_session *session,
	struct powervr_render_context *context)
{
	struct powervr_switch_tasks tasks[2];
	struct powervr_heap *heap;
	uint8_t record[POWERVR_RENDER_CONTEXT_BYTES];
	uint64_t general;
	unsigned set;
	int error;

	/* The record, the two state blocks and the two command buffers. */
	heap = &render->core->firmware.heap;
	error = powervr_object_create(heap, POWERVR_RENDER_CONTEXT_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &context->record);
	if (error != 0)
		return error;

	error = powervr_object_create(heap, POWERVR_GEOMETRY_STATE_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &context->geometry_state);
	if (error != 0)
		return error;

	error = powervr_object_create(heap, POWERVR_FRAGMENT_STATE_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &context->fragment_state);
	if (error != 0)
		return error;

	error = client_make(heap, &context->geometry);
	if (error != 0)
		return error;

	error = client_make(heap, &context->fragment);
	if (error != 0)
		return error;

	/* Each set's programs at their places, and the task words that name them. */
	for (set = 0; set < 2U; set++) {
		error = set_write(context->programs[set], context->program_places[set], context->shader_place, &tasks[set]);
		if (error != 0)
			return error;
	}

	/* The stand-in background: a program that only ends, as both the pixel and the uniform program. */
	error = background_write(context);
	if (error != 0)
		return error;

	/* The static state, written whole (or left zero): the state buffers and both sets' tasks. */
	kern_memset(record, 0, sizeof(record));
	general = context->switch_memory->address;
	if (POWERVR_RENDER_SWITCH_STATE)
		powervr_render_switch_state(record, powervr_switch_base_word(general + GENERAL_VDM_STATE), powervr_switch_base_word(general + GENERAL_TA_STATE), tasks);
	powervr_object_fill(&context->record, 0, record, sizeof(record));

	/* The geometry state's initial call stack pointer. */
	powervr_object_put64(&context->geometry_state, POWERVR_GEOMETRY_STATE_CALL_STACK, general + GENERAL_CALL_STACK);

	/* The identity, the command buffers' controls and the two common contexts. */
	context->identity = render->next_identity++;
	client_fill(&context->geometry);
	client_fill(&context->fragment);
	common_fill(session, context, POWERVR_RENDER_CONTEXT_GEOMETRY, &context->geometry, &context->geometry_state, POWERVR_DATA_MASTER_GEOMETRY);
	common_fill(session, context, POWERVR_RENDER_CONTEXT_FRAGMENT, &context->fragment, &context->fragment_state, POWERVR_DATA_MASTER_FRAGMENT);

	/* Succeeded: the firmware can run the context's jobs once kicked. */
	return 0;
}

/* Makes one command buffer's records: its control, the buffer and its timeline. */
static int
client_make(
	struct powervr_heap *heap,
	struct powervr_client *client)
{
	int error;

	/* The control, the buffer, the timeline word. */
	error = powervr_object_create(heap, POWERVR_CLIENT_CONTROL_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &client->control);
	if (error != 0)
		return error;

	error = powervr_object_create(heap, POWERVR_CLIENT_CCB_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &client->buffer);
	if (error != 0)
		return error;

	error = powervr_object_create(heap, POWERVR_SYNC_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &client->timeline);
	if (error != 0)
		return error;

	/* Succeeded: the records exist, zeroed. */
	return 0;
}

/* Returns one command buffer's records; one never made is skipped. */
static void
client_destroy(
	struct powervr_heap *heap,
	struct powervr_client *client)
{
	/* In the reverse of their making. */
	powervr_object_destroy(heap, &client->timeline);
	powervr_object_destroy(heap, &client->buffer);
	powervr_object_destroy(heap, &client->control);
}

/* Drops one command buffer's records' runs without touching their old places. */
static void
client_drop(
	struct powervr_client *client)
{
	/* In the reverse of their making. */
	powervr_object_drop(&client->timeline);
	powervr_object_drop(&client->buffer);
	powervr_object_drop(&client->control);
}

/*
 * Fills one command buffer's control: empty, with its wrap mask.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/imagination/pvr_cccb.c (cccb_ctrl_init: both offsets
 * and the dependency offset zero, the wrap mask).
 */
static void
client_fill(
	struct powervr_client *client)
{
	/* The offsets and the mask; the timeline starts at zero. */
	powervr_object_put32(&client->control, POWERVR_CLIENT_WRITE, 0);
	powervr_object_put32(&client->control, POWERVR_CLIENT_READ, 0);
	powervr_object_put32(&client->control, POWERVR_CLIENT_DEPENDENCY, 0);
	powervr_object_put32(&client->control, POWERVR_CLIENT_WRAP_MASK, POWERVR_CLIENT_CCB_BYTES - 1U);
	client->write = 0;
	client->submitted = 0;
}

/* Fills one common context of a render context's record. */
static void
common_fill(
	struct powervr_session *session,
	struct powervr_render_context *context,
	uint32_t place,
	struct powervr_client *client,
	struct powervr_object *state,
	uint32_t master)
{
	struct powervr_object *record;

	/* The command buffer, the state, the master, the priority, the deadline, the memory context, the identity. */
	record = &context->record;
	powervr_object_put32(record, place + POWERVR_CONTEXT_CCB_CONTROL, powervr_object_address(&client->control, 0));
	powervr_object_put32(record, place + POWERVR_CONTEXT_CCB, powervr_object_address(&client->buffer, 0));
	powervr_object_put32(record, place + POWERVR_CONTEXT_STATE, powervr_object_address(state, 0));
	powervr_object_put32(record, place + POWERVR_CONTEXT_PRIORITY, 0);
	powervr_object_put32(record, place + POWERVR_CONTEXT_DATA_MASTER, master);
	powervr_object_put32(record, place + POWERVR_CONTEXT_DEADLINE, POWERVR_DEADLINE_MS);
	powervr_object_put32(record, place + POWERVR_CONTEXT_MEMORY, powervr_object_address(&session->memory_context, 0));
	powervr_object_put32(record, place + POWERVR_CONTEXT_IDENTITY, context->identity);
}

/*
 * Writes one set's data sequencer programs at their places in its buffer
 * and builds the task words that name them.
 */
static int
set_write(
	struct powervr_buffer *buffer,
	uint64_t place,
	uint64_t shader_place,
	struct powervr_switch_tasks *tasks)
{
	struct powervr_switch_programs programs;
	uint32_t data[POWERVR_SHARED_PDS_DATA_WORDS];
	uint32_t code[POWERVR_PERSISTENT_RESUME_CODE_WORDS];
	uint64_t usc;
	uint32_t base;
	int error;

	/* The places as offsets from the data sequencer window's base. */
	base = (uint32_t)(place - POWERVR_PDS_HEAP_BASE);
	usc = shader_place - POWERVR_USC_HEAP_BASE;
	programs.shared_store_data = base + SET_SHARED_STORE;
	programs.shared_store_code = base + SET_SHARED_STORE + SET_SHARED_CODE;
	programs.shared_load_data = base + SET_SHARED_LOAD;
	programs.shared_load_code = base + SET_SHARED_LOAD + SET_SHARED_CODE;
	programs.persistent_store_data = base + SET_PERSISTENT_STORE;
	programs.persistent_resume_data = base + SET_PERSISTENT_RESUME;

	/* The shared register store and load programs, each starting its shading cluster program. */
	error = powervr_shared_pds_program(place + SET_SHARED_STATE, usc + SHADER_STORE, POWERVR_SHARED_USC_TEMPORARIES, data, code);
	if (error != 0)
		return error;

	view_words(buffer, SET_SHARED_STORE, data, POWERVR_SHARED_PDS_DATA_WORDS);
	view_words(buffer, SET_SHARED_STORE + SET_SHARED_CODE, code, POWERVR_SHARED_PDS_CODE_WORDS);
	error = powervr_shared_pds_program(place + SET_SHARED_STATE, usc + SHADER_LOAD, POWERVR_SHARED_USC_TEMPORARIES, data, code);
	if (error != 0)
		return error;

	view_words(buffer, SET_SHARED_LOAD, data, POWERVR_SHARED_PDS_DATA_WORDS);
	view_words(buffer, SET_SHARED_LOAD + SET_SHARED_CODE, code, POWERVR_SHARED_PDS_CODE_WORDS);

	/* The persistent temporaries' store and resume programs, code right after data. */
	error = powervr_persistent_store_program(place + SET_PERSISTENT, data, code);
	if (error != 0)
		return error;

	view_words(buffer, SET_PERSISTENT_STORE, data, POWERVR_PERSISTENT_DATA_WORDS);
	view_words(buffer, SET_PERSISTENT_STORE + SET_PERSISTENT_CODE, code, POWERVR_PERSISTENT_STORE_CODE_WORDS);
	error = powervr_persistent_resume_program(place + SET_PERSISTENT, data, code);
	if (error != 0)
		return error;

	view_words(buffer, SET_PERSISTENT_RESUME, data, POWERVR_PERSISTENT_DATA_WORDS);
	view_words(buffer, SET_PERSISTENT_RESUME + SET_PERSISTENT_CODE, code, POWERVR_PERSISTENT_RESUME_CODE_WORDS);

	/* The task words from the places. */
	powervr_switch_task_words(&programs, tasks);

	/* Succeeded: the set is written. */
	return 0;
}

/*
 * Writes the stand-in background program of a render context, a data
 * sequencer program of one halt, and the words that name it as the
 * background after a partial render.
 */
static int
background_write(
	struct powervr_render_context *context)
{
	uint32_t halt;
	uint32_t place;
	int error;

	/* The halt, at its place in the first set. */
	error = powervr_pds_halt(0, &halt);
	if (error != 0)
		return error;

	view_put32(context->programs[0], SET_BACKGROUND, halt);

	/* The words: the same program for the pixel and the uniform pass, no texture data. */
	place = (uint32_t)(context->program_places[0] - POWERVR_PDS_HEAP_BASE) + SET_BACKGROUND;
	powervr_background_words(place, place, 0, context->background);

	/* Succeeded: the background is written. */
	return 0;
}

/* Writes a little-endian word through a buffer's uncached view. */
static void
view_put32(
	struct powervr_buffer *buffer,
	uint32_t offset,
	uint32_t value)
{
	/* Low byte first, as the core reads it. */
	buffer->view[offset + 0U] = (uint8_t)value;
	buffer->view[offset + 1U] = (uint8_t)(value >> 8);
	buffer->view[offset + 2U] = (uint8_t)(value >> 16);
	buffer->view[offset + 3U] = (uint8_t)(value >> 24);
}

/* Writes words through a buffer's uncached view. */
static void
view_words(
	struct powervr_buffer *buffer,
	uint32_t offset,
	const uint32_t *words,
	unsigned count)
{
	unsigned index;

	/* One word at a time. */
	for (index = 0; index < count; index++)
		view_put32(buffer, offset + index * 4U, words[index]);
}

/* Writes bytes through a buffer's uncached view. */
static void
view_bytes(
	struct powervr_buffer *buffer,
	uint32_t offset,
	const uint8_t *bytes,
	size_t count)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < count; index++)
		buffer->view[offset + index] = bytes[index];
}

/* Makes a zeroed buffer of the switch's memory. */
static int
memory_buffer(
	struct powervr_render *render,
	uint32_t bytes,
	struct powervr_buffer **result)
{
	int error;

	/* The buffer, zeroed by its making. */
	*result = NULL;
	error = powervr_buffer_create(render, bytes, result);
	if (error != 0) {
		*result = NULL;
		return error;
	}

	/* Succeeded: the buffer. */
	return 0;
}
