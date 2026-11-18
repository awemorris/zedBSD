/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR render node's transfer contexts: a session's first
 * transfer makes one, a common context of the fragment master with a
 * command buffer and a timeline of its own.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/render-private.h"
#include "drivers/gpu/powervr/interface.h"

/*
 * The transfer context's record (one common context, aligned to 8) and
 * its state block (the fragment state's room, the transfer's ISP store
 * words fitting in it).
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif.h (struct
 * rogue_fwif_fwtransfercontext: tq_context only); pvr_rogue_fwif_check.h
 * (struct rogue_fwif_fwcommoncontext 152 bytes); pvr_queue.c
 * (get_xfer_ctx_state_size: the fragment state with its ISP store words;
 * get_dm_type: a transfer queue runs on the fragment master).
 */
#define TRANSFER_RECORD_BYTES		152U

/*
 * Finds or makes a session's transfer context.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_context.c
 * (pvr_context_create for DRM_PVR_CTX_TYPE_TRANSFER_FRAG: no static
 * state, one queue); pvr_queue.c (pvr_queue_create, init_fw_context).
 */
int
powervr_session_transfer_context(
	struct powervr_render *render,
	struct powervr_session *session,
	struct powervr_transfer_context **result)
{
	struct powervr_transfer_context *context;
	struct powervr_heap *heap;
	struct powervr_object *record;
	int error;

	/* The session's own, made by its first transfer. */
	*result = session->transfer_context;
	if (*result != NULL)
		return 0;

	/* The bookkeeping, kept by the session from here so a failure is undone by the destroy. */
	context = kern_calloc(1, sizeof(*context));
	if (context == NULL)
		return ENOMEM;
	session->transfer_context = context;

	/* The record, the state, the command buffer, under the core's lock. */
	heap = &render->core->firmware.heap;
	mutex_lock(&render->core->lock);

	error = powervr_object_create(heap, TRANSFER_RECORD_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &context->record);
	if (error == 0)
		error = powervr_object_create(heap, POWERVR_FRAGMENT_STATE_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &context->state);
	if (error == 0)
		error = powervr_object_create(heap, POWERVR_CLIENT_CONTROL_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &context->client.control);
	if (error == 0)
		error = powervr_object_create(heap, POWERVR_CLIENT_CCB_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &context->client.buffer);
	if (error == 0)
		error = powervr_object_create(heap, POWERVR_SYNC_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &context->client.timeline);

	/* The control: empty, with its wrap mask; the common context names it all. */
	if (error == 0) {
		powervr_object_put32(&context->client.control, POWERVR_CLIENT_WRAP_MASK, POWERVR_CLIENT_CCB_BYTES - 1U);
		context->identity = render->next_identity++;
		record = &context->record;
		powervr_object_put32(record, POWERVR_CONTEXT_CCB_CONTROL, powervr_object_address(&context->client.control, 0));
		powervr_object_put32(record, POWERVR_CONTEXT_CCB, powervr_object_address(&context->client.buffer, 0));
		powervr_object_put32(record, POWERVR_CONTEXT_STATE, powervr_object_address(&context->state, 0));
		powervr_object_put32(record, POWERVR_CONTEXT_PRIORITY, 0);
		powervr_object_put32(record, POWERVR_CONTEXT_DATA_MASTER, POWERVR_DATA_MASTER_FRAGMENT);
		powervr_object_put32(record, POWERVR_CONTEXT_DEADLINE, POWERVR_DEADLINE_MS);
		powervr_object_put32(record, POWERVR_CONTEXT_MEMORY, powervr_object_address(&session->memory_context, 0));
		powervr_object_put32(record, POWERVR_CONTEXT_IDENTITY, context->identity);
	}

	mutex_unlock(&render->core->lock);

	/* A record that could not be made: what was made goes. */
	if (error != 0) {
		powervr_transfer_context_destroy(render, session);
		return error;
	}

	/* Succeeded: the transfer context is the session's. */
	*result = context;
	return 0;
}

/* Returns a session's transfer context once the firmware no longer uses it. */
void
powervr_transfer_context_destroy(
	struct powervr_render *render,
	struct powervr_session *session)
{
	struct powervr_transfer_context *context;
	struct powervr_heap *heap;

	/* Nothing made. */
	context = session->transfer_context;
	if (context == NULL)
		return;

	/* The records, in the reverse of their making. */
	heap = &render->core->firmware.heap;
	mutex_lock(&render->core->lock);

	powervr_object_destroy(heap, &context->client.timeline);
	powervr_object_destroy(heap, &context->client.buffer);
	powervr_object_destroy(heap, &context->client.control);
	powervr_object_destroy(heap, &context->state);
	powervr_object_destroy(heap, &context->record);

	mutex_unlock(&render->core->lock);

	/* The bookkeeping. */
	kern_free(context);
	session->transfer_context = NULL;
}

/* Returns a kept session's transfer context once the firmware is gone; its old places are not touched. */
void
powervr_transfer_context_discard(
	struct powervr_session *session)
{
	struct powervr_transfer_context *context;

	/* Nothing made. */
	context = session->transfer_context;
	if (context == NULL)
		return;

	/* The records' runs, then the bookkeeping. */
	powervr_object_drop(&context->client.timeline);
	powervr_object_drop(&context->client.buffer);
	powervr_object_drop(&context->client.control);
	powervr_object_drop(&context->state);
	powervr_object_drop(&context->record);
	kern_free(context);
	session->transfer_context = NULL;
}
