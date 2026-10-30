/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's dispatcher: each command goes to the one handler that owns its
 * opcode.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/klog.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/vk/nvrtx-vk-objects.h"

/* The refusals shown so far (only the first ones are; the node's lock guards the count). */
#define DISPATCH_SHOWN_MAX		32U

static unsigned dispatch_shown;

static void dispatch_refused(uint32_t opcode, int error);

/*
 * The handlers, each owning a group of opcodes: the recorded commands
 * (first: a frame's commands are mostly theirs, and no other handler owns
 * one), the instance and device, the objects and the queue.  The list is
 * fixed for the node's life.
 */
static const nvrtx_vk_route dispatch_routes[] = {
	nvrtx_vk_record_route,
	nvrtx_vk_device_route,
	nvrtx_vk_memory_route,
	nvrtx_vk_resource_route,
	nvrtx_vk_layout_route,
	nvrtx_vk_target_route,
	nvrtx_vk_pipeline_route,
	nvrtx_vk_command_route,
	nvrtx_vk_queue_route,
	nvrtx_vk_video_route
};

/*
 * Runs one command through the handler that owns its opcode.
 *
 * Returns ENOTSUP for an opcode no handler owns (its body cannot be
 * skipped: commands carry no length), or the handler's error.
 */
int
nvrtx_vk_dispatch(
	struct nvrtx_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	size_t index;
	int handled;
	int error;

	/* Asks each handler until one takes the command; one that refuses it is shown. */
	handled = 0;
	for (index = 0U; index < sizeof(dispatch_routes) / sizeof(dispatch_routes[0]); index++) {
		error = dispatch_routes[index](session, opcode, requested, reader, reply, &handled);
		if (error != 0) {
			dispatch_refused(opcode, error);
			return error;
		}
		if (handled != 0)
			break;
	}

	/* Refuses, and shows, a command no handler owns. */
	if (handled == 0) {
		dispatch_refused(opcode, ENOTSUP);
		return ENOTSUP;
	}

	/* Succeeded: one handler ran the command. */
	return 0;
}

/* Shows a command the executor refused (the frame fails with it), the first ones only. */
static void
dispatch_refused(
	uint32_t opcode,
	int error)
{
	/* Only the first ones. */
	if (dispatch_shown >= DISPATCH_SHOWN_MAX)
		return;
	dispatch_shown++;

	/* The opcode and the error. */
	kern_logf("nvrtx: vk: command %u refused (%d)\n", (unsigned)opcode, error);
}
