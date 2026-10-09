/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Vulkan session storage remains live until every namespace, dependency and prepared native owner has retired. */
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-private.h"

/* One command's generated records use a bounded arena reset between decoded protocol commands. */
#define VULKAN_COMMAND_ARENA_BYTES (256U << 10)

/*
 * Opens one private Vulkan namespace and its bounded generated-record arena.
 */
int
bcm2711_vulkan_session_open(
	struct bcm2711_render_session *render,
	struct bcm2711_vulkan_session **session)
{
	struct bcm2711_vulkan_session *created;

	/* An ordinary renderer owner is required before Vulkan protocol state can be created. */
	if (session == NULL)
		return EINVAL;
	*session = NULL;
	if (render == NULL)
		return EINVAL;

	/* Namespace storage remains private until both it and its record arena exist. */
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;

	/* Each logical session owns its record arena independently of every other client open. */
	created->arena.base = kern_calloc(1, VULKAN_COMMAND_ARENA_BYTES);
	if (created->arena.base == NULL) {
		kern_free(created);
		return ENOMEM;
	}

	/* No initial object or native payload is implied by opening protocol storage. */
	created->render = render;
	created->arena.size = VULKAN_COMMAND_ARENA_BYTES;
	*session = created;

	/* The renderer owns an empty independent namespace with bounded command-decode storage. */
	return 0;
}

/*
 * Closes one drained Vulkan namespace and clears its owner pointer only after all storage can retire.
 */
int
bcm2711_vulkan_session_close(
	struct bcm2711_vulkan_session **session)
{
	struct bcm2711_vulkan_session *closing;
	int error;

	/* The renderer's owner pointer is the only publication this operation may withdraw. */
	if (session == NULL)
		return EINVAL;
	closing = *session;
	if (closing == NULL)
		return 0;

	/* The caller has joined its worker/common callbacks before withdrawing protocol identities. */
	error = bcm2711_vulkan_objects_close(closing);
	if (closing->live_objects != 0) {
		/* Existing owners still require the same session for their eventual destructor. */
		if (error != 0)
			return error;
		return EBUSY;
	}

	/* A native storage error may survive, but all logical owners and generated record storage have retired. */
	kern_free(closing->arena.base);
	kern_free(closing);
	*session = NULL;

	/* The exact native retirement outcome remains visible after the owner pointer was safely cleared. */
	if (error != 0)
		return error;

	/* No protocol state remains behind the renderer's cleared session pointer. */
	return 0;
}
