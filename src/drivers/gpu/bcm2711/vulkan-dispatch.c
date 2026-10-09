/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Typed Vulkan routing is shared by the public renderer and actual client codec checks. */
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-dispatch.h"
#include "drivers/gpu/bcm2711/vulkan-device.h"
#include "drivers/gpu/bcm2711/vulkan-queue.h"
#include "drivers/gpu/bcm2711/vulkan-sync.h"
#include "drivers/gpu/bcm2711/vulkan-memory.h"
#include "drivers/gpu/bcm2711/vulkan-resource.h"
#include "drivers/gpu/bcm2711/vulkan-input.h"
#include "drivers/gpu/bcm2711/vulkan-layout.h"
#include "drivers/gpu/bcm2711/vulkan-descriptor.h"
#include "drivers/gpu/bcm2711/vulkan-target.h"
#include "drivers/gpu/bcm2711/vulkan-pipeline.h"
#include "drivers/gpu/bcm2711/vulkan-command.h"
#include "drivers/gpu/bcm2711/vulkan-barrier.h"
#include "drivers/gpu/bcm2711/vulkan-buffer-copy.h"
#include "drivers/gpu/bcm2711/vulkan-record.h"
#include "drivers/gpu/bcm2711/vulkan-transfer.h"

/* Each route either owns the exact opcode or leaves both cursors unchanged. */
typedef int (*vulkan_route)(struct bcm2711_vulkan_session *, uint32_t, uint32_t, struct i915_wire_reader *, struct i915_wire_writer *, int *);

/* The immutable complete route list lives as long as the registered renderer node. */
static const vulkan_route routes[] = {
	bcm2711_vulkan_device_dispatch,
	bcm2711_vulkan_queue_dispatch,
	bcm2711_vulkan_sync_dispatch,
	bcm2711_vulkan_query_dispatch,
	bcm2711_vulkan_memory_dispatch,
	bcm2711_vulkan_resource_dispatch,
	bcm2711_vulkan_input_dispatch,
	bcm2711_vulkan_layout_dispatch,
	bcm2711_vulkan_descriptor_pool_dispatch,
	bcm2711_vulkan_descriptor_sets_dispatch,
	bcm2711_vulkan_descriptor_update_dispatch,
	bcm2711_vulkan_target_dispatch,
	bcm2711_vulkan_pipeline_dispatch,
	bcm2711_vulkan_command_pool_dispatch,
	bcm2711_vulkan_command_batch_dispatch,
	bcm2711_vulkan_command_buffer_dispatch,
	bcm2711_vulkan_barrier_dispatch,
	bcm2711_vulkan_buffer_copy_dispatch,
	bcm2711_vulkan_transfer_dispatch,
	bcm2711_vulkan_record_dispatch
};

/*
 * Routes one actual wire command through the complete implemented native Vulkan profile.
 */
int
bcm2711_vulkan_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	size_t index;
	int handled;
	int error;

	/* Unknown commands never advance input or manufacture a successful empty response. */
	handled = 0;
	for (index = 0; index < sizeof(routes) / sizeof(routes[0]); index++) {
		handled = 0;
		error = routes[index](session, opcode, requested, reader, reply, &handled);
		if (error != 0)
			return error;

		/* Exactly one typed owner consumed the whole selected command. */
		if (handled != 0)
			break;
	}

	/* No implemented route accepts this command, so transport cannot publish its later trailer. */
	if (handled == 0)
		return ENOTSUP;

	/* Succeeded: one typed native route consumed the selected command. */
	return 0;
}
