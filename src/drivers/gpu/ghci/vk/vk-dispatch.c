/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The dispatch of the Vulkan executor: each command goes to the handler
 * of its group, and one no handler owns ends the frame.  A command
 * carries no length, so a number the executor does not know leaves no
 * way to the next command.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/klog.h>
#include <uapi/errno.h>

#include "vk.h"

/* The refused commands the log shows of one device (only the first ones). */
#define DISPATCH_SHOWN_MAX		32U

static void dispatch_refused(struct ghci_vk_session *session, uint32_t opcode, int error);

/*
 * The handlers, each owning a group of commands: the instance, the
 * physical device, the device and its queues; memory, buffers and
 * images; command pools, command buffers and the recorded commands; the
 * submission with fences, semaphores and events; samplers, views,
 * descriptor sets and their layouts; shader modules, pipeline caches
 * and pipelines; render passes and framebuffers; and query pools.  The
 * list is fixed for the kernel's life.
 */
static const ghci_vk_route dispatch_routes[] = {
	ghci_vk_device_route,
	ghci_vk_memory_route,
	ghci_vk_command_route,
	ghci_vk_queue_route,
	ghci_vk_layout_route,
	ghci_vk_pipeline_route,
	ghci_vk_pass_route,
	ghci_vk_query_route
};

/*
 * Runs one command through the handler that owns its opcode.  Returns 0,
 * ENOTSUP for an opcode no handler owns, or the handler's error.
 */
int
ghci_vk_dispatch(
	struct ghci_vk_session *session,
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
			dispatch_refused(session, opcode, error);
			return error;
		}
		if (handled)
			break;
	}

	/* Refuses, and shows, a command no handler owns. */
	if (!handled) {
		dispatch_refused(session, opcode, ENOTSUP);
		return ENOTSUP;
	}

	/* Succeeded: one handler ran the command. */
	return 0;
}

/* Shows a command the executor refused (the frame ends with it), the first ones of the device only. */
static void
dispatch_refused(
	struct ghci_vk_session *session,
	uint32_t opcode,
	int error)
{
	struct ghci_vk *vk;

	/* Only the first ones. */
	vk = session->device->vk;
	if (vk->refusals >= DISPATCH_SHOWN_MAX)
		return;
	vk->refusals++;

	/* The opcode and the error. */
	kern_logf("ghci: %s: vk: command %u refused (%d)\n", session->device->name, (unsigned)opcode, error);
}
