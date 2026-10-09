/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Complete graphics batches publish only independently compiled native pipeline owners and preserve legitimate partial successes. */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-pipeline-record.h"

/* Four self-contained records keep command metadata finite within the kernel stack while Keiland creates one pipeline per call. */
#define VULKAN_PIPELINE_BATCH 4U

static int create_pipelines(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy_pipeline(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader);
static VkResult pipeline_status(int error);

/*
 * Routes finite complete graphics creation batches and independent pipeline identity retirement.
 */
int
bcm2711_vulkan_pipeline_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Compute and cache commands remain outside the admitted Keiland graphics execution path. */
	*handled = 1;
	if (opcode != GPU_OP_CREATE_GRAPHICS_PIPELINES && opcode != GPU_OP_DESTROY_PIPELINE) {
		*handled = 0;
		return 0;
	}

	/* Ordinary void destruction may request an opcode echo; creation always needs a complete result vector. */
	if (requested > 1)
		return EINVAL;
	if (opcode == GPU_OP_CREATE_GRAPHICS_PIPELINES) {
		if (requested != 1)
			return EINVAL;
		error = create_pipelines(session, reader, reply);
	} else {
		error = destroy_pipeline(session, reader);
	}

	/* A failed transport or ownership retirement never receives a fictitious completion trailer. */
	if (error != 0)
		return error;

	/* Succeeded: the complete acknowledged pipeline outcome belongs to this exact session namespace. */
	return 0;
}

/* Consumes all selected records and fresh output identities before compiling or publishing any native pipeline. */
static int
create_pipelines(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_pipeline_record records[VULKAN_PIPELINE_BATCH];
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_pipeline *pipeline;
	uint64_t identifiers[VULKAN_PIPELINE_BATCH];
	uint64_t outputs[VULKAN_PIPELINE_BATCH];
	uint64_t identity;
	uint64_t cache;
	uint64_t array;
	uint64_t allocator;
	uint32_t count;
	uint32_t index;
	uint32_t previous;
	VkResult status;
	VkResult failure;
	int error;
	int retired;

	/* The actual client names one exact logical device, an optional cache, and a count-selected graphics record array. */
	identity = drv_i915_wire_read_u64(reader);
	cache = drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || cache != 0 || count == 0 || count > VULKAN_PIPELINE_BATCH || array != count)
		return ENOTSUP;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, identity);
	if (device == NULL)
		return EINVAL;
	for (index = 0; index < count; index++) {
		error = bcm2711_vulkan_pipeline_decode(reader, &records[index]);
		if (error != 0)
			return error;
	}

	/* Output identities follow the complete input record array, with the client's ordinary null allocator marker. */
	allocator = drv_i915_wire_read_u64(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || allocator != 0 || array != count)
		return EINVAL;
	for (index = 0; index < count; index++)
		identifiers[index] = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The whole fresh identity vector is validated before any legitimate partial success can enter the registry. */
	kern_memset(outputs, 0, sizeof(outputs));
	for (index = 0; index < count; index++) {
		if (identifiers[index] == 0)
			return EINVAL;
		object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PIPELINE, identifiers[index]);
		if (object != NULL)
			return EEXIST;
		for (previous = 0; previous < index; previous++) {
			if (identifiers[previous] == identifiers[index])
				return EINVAL;
		}
	}

	/* Vulkan allows partial graphics batch success; each member has an independently complete compiled graph or a null output. */
	status = VK_SUCCESS;
	for (index = 0; index < count; index++) {
		pipeline = NULL;
		error = bcm2711_vulkan_pipeline_build(session, device, &records[index].info, &pipeline);
		if (error == 0) {
			error = bcm2711_vulkan_object_publish(session, I915_VK_OBJ_PIPELINE, identifiers[index], pipeline, bcm2711_vulkan_pipeline_release, &object);
			if (error != 0) {
				retired = bcm2711_vulkan_pipeline_release(session, pipeline);
				if (retired != 0)
					return retired;
			}
		}

		/* Only successful exact registry publication acknowledges the reserved ID, preserving the first ordinary failed member result. */
		if (error == 0) {
			outputs[index] = identifiers[index];
		} else {
			failure = pipeline_status(error);
			if (status == VK_SUCCESS)
				status = failure;
		}
	}

	/* The client validates the complete vector before publishing any local pipeline handle, including legitimate partial successes. */
	drv_i915_wire_reply_u32(reply, (uint32_t)status);
	drv_i915_wire_reply_u64(reply, count);
	for (index = 0; index < count; index++)
		drv_i915_wire_reply_u64(reply, outputs[index]);

	/* Succeeded: every batch member has a recoverable exact independently owned outcome. */
	return 0;
}

/* Withdraws one same-device typed pipeline identity while recorded and prepared draws retain their compiled graph. */
static int
destroy_pipeline(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_pipeline *pipeline;
	uint64_t device_id;
	uint64_t identity;
	uint64_t allocator;
	int error;

	/* Ordinary client pipeline destruction carries the exact device, pipeline ID and null allocator. */
	device_id = drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	allocator = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || allocator != 0)
		return EINVAL;
	if (identity == 0)
		return 0;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_PIPELINE, identity);
	if (device == NULL || object == NULL)
		return EINVAL;
	pipeline = object->payload;
	if (pipeline->owner.device != device)
		return EINVAL;
	error = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_PIPELINE, identity);
	if (error != 0)
		return error;

	/* Succeeded: only the public identity reference retired, preserving all independent compiled draw owners. */
	return 0;
}

/* Maps ordinary native construction refusals to exact Vulkan member failures without a false pipeline success. */
static VkResult
pipeline_status(
	int error)
{
	/* Heap or registry exhaustion is an ordinary graphics object allocation failure. */
	if (error == ENOMEM || error == ENOSPC)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	if (error == ENOTSUP || error == E2BIG)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	if (error != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Succeeded: a completely constructed and published native member has no refusal. */
	return VK_SUCCESS;
}
