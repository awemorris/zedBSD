/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements ordinary pipeline objects, partial batch creation and native cache data.
 */

#include "internal.h"

#include <string.h>

/* A pipeline retains only metadata needed to validate derivative base ownership. */
struct vulkan_pipeline {
	struct vulkan_object object;
	VkPipelineCreateFlags flags;
	VkPipelineBindPoint bind_point;
	VkBool32 created;
};

static VkResult pipeline_single_create(struct VkDevice_T *device, enum vulkan_object_kind kind, uint32_t opcode, uint32_t destroy_opcode, const void *info, const VkAllocationCallbacks *allocator, uint64_t *result);
static void pipeline_destroy(struct VkDevice_T *device, uint64_t handle, enum vulkan_object_kind kind, uint32_t opcode, const VkAllocationCallbacks *allocator);
static struct vulkan_object *pipeline_object(struct VkDevice_T *device, uint64_t handle, enum vulkan_object_kind kind);
static VkResult pipeline_batch(struct VkDevice_T *device, VkPipelineCache cache, uint32_t count, const void *infos, VkPipelineBindPoint bind_point, const VkAllocationCallbacks *allocator, VkPipeline *results);
static void pipeline_encode_graphics(struct vulkan_writer *writer, struct VkDevice_T *device, uint32_t index, const VkGraphicsPipelineCreateInfo *info);
static void pipeline_encode_compute(struct vulkan_writer *writer, struct VkDevice_T *device, uint32_t index, const VkComputePipelineCreateInfo *info);
static VkResult pipeline_base(struct VkDevice_T *device, VkPipelineCreateFlags flags, VkPipelineBindPoint bind_point, uint32_t index, VkPipeline *base, int32_t *base_index);
static VkBool32 pipeline_dynamic(const VkPipelineDynamicStateCreateInfo *info, VkDynamicState state);
static void pipeline_encode_viewport(struct vulkan_writer *writer, const VkPipelineViewportStateCreateInfo *info, const VkPipelineDynamicStateCreateInfo *dynamic);
static void pipeline_encode_rasterization(struct vulkan_writer *writer, const VkPipelineRasterizationStateCreateInfo *info, const VkPipelineDynamicStateCreateInfo *dynamic);
static void pipeline_encode_depth(struct vulkan_writer *writer, const VkPipelineDepthStencilStateCreateInfo *info, const VkPipelineDynamicStateCreateInfo *dynamic);
static void pipeline_encode_blend(struct vulkan_writer *writer, const VkPipelineColorBlendStateCreateInfo *info, const VkPipelineDynamicStateCreateInfo *dynamic);
static VkResult pipeline_lost(struct VkDevice_T *device);

/*
 * Creates a native cache whose opaque bytes remain owned by its Vulkan device.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreatePipelineCache(
	VkDevice device_handle,
	const VkPipelineCacheCreateInfo *info,
	const VkAllocationCallbacks *allocator,
	VkPipelineCache *result)
{
	struct VkDevice_T *device;
	uint64_t handle;
	VkResult error;

	/* A failed call never exposes a partially initialized cache handle. */
	if (result == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;
	*result = VK_NULL_HANDLE;
	device = vulkan_device(device_handle);
	if (device == NULL || info == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Refuses a cache request whose layout cannot be encoded as a Vulkan cache record. */
	if (info->sType != VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO)
		return VK_ERROR_INITIALIZATION_FAILED;
	error = pipeline_single_create(
		device,
		VULKAN_OBJECT_PIPELINE_CACHE,
		VULKAN_OPCODE_vkCreatePipelineCache,
		VULKAN_OPCODE_vkDestroyPipelineCache,
		info,
		allocator,
		&handle);
	if (error != VK_SUCCESS)
		return error;
	*result = (VkPipelineCache)handle;

	/* Succeeded: later data and merge operations address a real renderer cache. */
	return VK_SUCCESS;
}

/*
 * Destroys a pipeline cache without retaining it outside a modifying API call.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyPipelineCache(
	VkDevice device_handle,
	VkPipelineCache cache,
	const VkAllocationCallbacks *allocator)
{
	struct VkDevice_T *device;

	/* Null destruction is valid even when no cache was created. */
	if (cache == VK_NULL_HANDLE)
		return;
	device = vulkan_device(device_handle);
	if (device == NULL)
		return;
	pipeline_destroy(
		device,
		(uint64_t)cache,
		VULKAN_OBJECT_PIPELINE_CACHE,
		VULKAN_OPCODE_vkDestroyPipelineCache,
		allocator);

	/* Succeeded: the cache's local and renderer lifetimes have retired. */
	return;
}

/*
 * Returns native cache data with two-call sizing and standard short-buffer semantics.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPipelineCacheData(
	VkDevice device_handle,
	VkPipelineCache handle,
	size_t *size,
	void *data)
{
	struct VkDevice_T *device;
	struct vulkan_object *cache;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	size_t capacity;
	size_t reply_bytes;
	uint64_t returned;
	uint64_t count;
	VkBool32 present;
	VkResult error;

	/* Cache data is opaque; only bounded framing and ownership are interpreted locally. */
	device = vulkan_device(device_handle);
	if (device == NULL || size == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;
	cache = pipeline_object(device, (uint64_t)handle, VULKAN_OBJECT_PIPELINE_CACHE);
	if (cache == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;
	capacity = 0U;
	if (data != NULL)
		capacity = *size;

	/* Reserves enough reply space for both native cache size fields and padded opaque bytes. */
	if (capacity > SIZE_MAX - 35U)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	reply_bytes = 32U + ((capacity + 3U) & ~(size_t)3U);

	/* A size-only request must not read the application's uninitialized size input. */
	vulkan_writer_init_for_object(&writer, cache);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkGetPipelineCacheData);
	vulkan_write_u64(&writer, device->object.wire_id);
	vulkan_write_u64(&writer, cache->wire_id);
	vulkan_write_u64(&writer, 1U);
	vulkan_write_u64(&writer, capacity);
	vulkan_write_u64(&writer, capacity);
	error = vulkan_command_execute(device->object.context, &writer, reply_bytes, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE) {
		error = vulkan_reply_finish(device->object.context, &reader, error);
		return error;
	}

	/* Size and opaque-array count are distinct protocol fields and must agree. */
	present = vulkan_reply_pointer(&reader);
	returned = vulkan_read_u64(&reader);
	count = vulkan_read_u64(&reader);
	if (present == VK_FALSE || returned > SIZE_MAX) {
		reader.error = VK_ERROR_DEVICE_LOST;
	} else if (capacity != 0U) {
		/* Rejects native cache counts that would cross the application buffer interval. */
		if (returned > capacity || count != returned)
			reader.error = VK_ERROR_DEVICE_LOST;
		else
			vulkan_read_bytes(&reader, data, (size_t)count);
	} else if (count != 0U) {
		reader.error = VK_ERROR_DEVICE_LOST;
	}

	error = vulkan_reply_finish(device->object.context, &reader, error);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* A nonnull zero-capacity buffer cannot be mistaken for the size-only public query. */
	if (data != NULL && capacity == 0U) {
		*size = 0U;

		/* Distinguishes a nonempty native cache from an empty zero-capacity public result. */
		if (returned != 0U)
			return VK_INCOMPLETE;
	} else {
		*size = (size_t)returned;
	}

	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: every returned byte is native cache data suitable for later initialization. */
	return VK_SUCCESS;
}

/*
 * Merges real cache contents through the renderer's ordinary Vulkan operation.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkMergePipelineCaches(
	VkDevice device_handle,
	VkPipelineCache destination,
	uint32_t count,
	const VkPipelineCache *sources)
{
	struct VkDevice_T *device;
	struct vulkan_object *cache;
	struct vulkan_object *source;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint32_t index;
	VkResult error;

	/* Valid cache operations cannot cross logical devices or object kinds. */
	device = vulkan_device(device_handle);
	if (device == NULL ||
	    sources == NULL ||
	    count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;
	cache = pipeline_object(device, (uint64_t)destination, VULKAN_OBJECT_PIPELINE_CACHE);
	if (cache == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Serializing the complete wire transaction also serializes native cache updates. */
	vulkan_writer_init_for_object(&writer, cache);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkMergePipelineCaches);
	vulkan_write_u64(&writer, device->object.wire_id);
	vulkan_write_u64(&writer, cache->wire_id);
	vulkan_write_u32(&writer, count);
	vulkan_write_u64(&writer, count);

	/* Resolves each source cache before passing its native identity to the merge operation. */
	for (index = 0U; index < count; index++) {
		source = pipeline_object(device, (uint64_t)sources[index], VULKAN_OBJECT_PIPELINE_CACHE);

		/* Rejects cache identities which cannot participate in this device-local merge. */
		if (source == NULL || source == cache) {
			writer.error = VK_ERROR_INITIALIZATION_FAILED;
			break;
		}

		vulkan_write_u64(&writer, source->wire_id);
	}

	error = vulkan_command_execute(device->object.context, &writer, 8U, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);
	error = vulkan_reply_finish(device->object.context, &reader, error);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the destination contains the renderer's merged cache state. */
	return VK_SUCCESS;
}

/*
 * Creates every requested graphics pipeline and preserves any partial native successes.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateGraphicsPipelines(
	VkDevice device_handle,
	VkPipelineCache cache,
	uint32_t count,
	const VkGraphicsPipelineCreateInfo *infos,
	const VkAllocationCallbacks *allocator,
	VkPipeline *results)
{
	struct VkDevice_T *device;
	VkResult error;

	/* The family encoder selects only statically relevant Vulkan 1.0 state. */
	device = vulkan_device(device_handle);
	if (device == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;
	error = pipeline_batch(
		device,
		cache,
		count,
		infos,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		allocator,
		results);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: all requested graphics pipelines have independently owned handles. */
	return VK_SUCCESS;
}

/*
 * Creates ordinary compute pipelines with standard specialization and derivative state.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateComputePipelines(
	VkDevice device_handle,
	VkPipelineCache cache,
	uint32_t count,
	const VkComputePipelineCreateInfo *infos,
	const VkAllocationCallbacks *allocator,
	VkPipeline *results)
{
	struct VkDevice_T *device;
	VkResult error;

	/* Compute shares the same dynamic batch and partial-result ownership contract. */
	device = vulkan_device(device_handle);
	if (device == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;
	error = pipeline_batch(
		device,
		cache,
		count,
		infos,
		VK_PIPELINE_BIND_POINT_COMPUTE,
		allocator,
		results);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: all requested compute pipelines have independently owned handles. */
	return VK_SUCCESS;
}

/*
 * Destroys one pipeline independently of its creation batch or original cache.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyPipeline(
	VkDevice device_handle,
	VkPipeline handle,
	const VkAllocationCallbacks *allocator)
{
	struct VkDevice_T *device;

	/* Null entries from partial creation need no remote or local cleanup. */
	if (handle == VK_NULL_HANDLE)
		return;
	device = vulkan_device(device_handle);
	if (device == NULL)
		return;
	pipeline_destroy(
		device,
		(uint64_t)handle,
		VULKAN_OBJECT_PIPELINE,
		VULKAN_OPCODE_vkDestroyPipeline,
		allocator);

	/* Succeeded: no batch, cache or derivative relationship retains this object. */
	return;
}

/*
 * Creates a standard pipeline layout from descriptor layouts and push-constant ranges.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreatePipelineLayout(
	VkDevice device_handle,
	const VkPipelineLayoutCreateInfo *info,
	const VkAllocationCallbacks *allocator,
	VkPipelineLayout *result)
{
	struct VkDevice_T *device;
	uint64_t handle;
	VkResult error;

	/* Descriptor-set layout handles are encoded through the common local-to-wire mapping. */
	if (result == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;
	*result = VK_NULL_HANDLE;
	device = vulkan_device(device_handle);
	if (device == NULL || info == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Refuses a layout record whose fields do not have the standard pipeline layout grammar. */
	if (info->sType != VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO)
		return VK_ERROR_INITIALIZATION_FAILED;
	error = pipeline_single_create(
		device,
		VULKAN_OBJECT_PIPELINE_LAYOUT,
		VULKAN_OPCODE_vkCreatePipelineLayout,
		VULKAN_OPCODE_vkDestroyPipelineLayout,
		info,
		allocator,
		&handle);
	if (error != VK_SUCCESS)
		return error;
	*result = (VkPipelineLayout)handle;

	/* Succeeded: this ordinary layout can be used by either graphics or compute pipelines. */
	return VK_SUCCESS;
}

/*
 * Releases a standard pipeline layout through compatible current allocation callbacks.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyPipelineLayout(
	VkDevice device_handle,
	VkPipelineLayout handle,
	const VkAllocationCallbacks *allocator)
{
	struct VkDevice_T *device;

	/* No creation batch or descriptor layout retains a local ownership reference. */
	if (handle == VK_NULL_HANDLE)
		return;
	device = vulkan_device(device_handle);
	if (device == NULL)
		return;
	pipeline_destroy(
		device,
		(uint64_t)handle,
		VULKAN_OBJECT_PIPELINE_LAYOUT,
		VULKAN_OPCODE_vkDestroyPipelineLayout,
		allocator);

	/* Succeeded: the local and native layout objects have retired. */
	return;
}

/* Acquires a single ordinary pipeline-family object through shared creation helpers. */
static VkResult
pipeline_single_create(
	struct VkDevice_T *device,
	enum vulkan_object_kind kind,
	uint32_t opcode,
	uint32_t destroy_opcode,
	const void *info,
	const VkAllocationCallbacks *allocator,
	uint64_t *result)
{
	struct vulkan_object *object;
	struct vulkan_writer writer;
	VkResult error;

	/* Local allocation and a never-reused ID precede every renderer-side acquisition. */
	error = vulkan_object_alloc(
		sizeof(*object),
		sizeof(uint64_t),
		kind,
		&device->object,
		device->object.context,
		allocator,
		VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
		&object);
	if (error != VK_SUCCESS)
		return error;
	error = vulkan_object_reserve_id(object);
	if (error != VK_SUCCESS) {
		vulkan_object_free(object);
		return error;
	}

	/* Typed inputs remain separate from common allocator and output-handle framing. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, opcode);
	vulkan_write_u64(&writer, device->object.wire_id);
	vulkan_write_u64(&writer, 1U);
	if (kind == VULKAN_OBJECT_PIPELINE_CACHE)
		vulkan_encode_VkPipelineCacheCreateInfo(&writer, info);
	else
		vulkan_encode_VkPipelineLayoutCreateInfo(&writer, info);
	error = vulkan_object_create_complete(device, object, &writer, destroy_opcode);
	vulkan_writer_finish(&writer);
	if (error != VK_SUCCESS) {
		vulkan_object_free(object);
		return error;
	}

	*result = vulkan_nondispatchable_handle(object);

	/* Succeeded: common publication owns one complete local and renderer identity. */
	return VK_SUCCESS;
}

/* Consumes a checked object after remote destruction or terminal namespace loss. */
static void
pipeline_destroy(
	struct VkDevice_T *device,
	uint64_t handle,
	enum vulkan_object_kind kind,
	uint32_t opcode,
	const VkAllocationCallbacks *allocator)
{
	struct vulkan_object *object;
	VkResult error;

	/* Foreign handles cannot affect another object's native or local lifetime. */
	object = pipeline_object(device, handle, kind);
	if (object == NULL)
		return;

	/* Uses the compatible callback policy supplied for this specific API operation. */
	if (allocator != NULL) {
		object->allocator.callbacks = *allocator;
		object->allocator.has_callbacks = VK_TRUE;
	}

	/* Current compatible callbacks own both destruction temporaries and final local free. */
	error = vulkan_object_destroy_remote(device, object, opcode);
	if (error != VK_SUCCESS)
		pipeline_lost(device);
	vulkan_object_free_with_allocator(object, allocator);

	/* Succeeded: no local handle remains usable after this destruction call. */
	return;
}

/* Checks both logical-device ownership and a standard object's family kind. */
static struct vulkan_object *
pipeline_object(
	struct VkDevice_T *device,
	uint64_t handle,
	enum vulkan_object_kind kind)
{
	struct vulkan_object *object;

	/* A null handle is returned only for callers which explicitly permit its absence. */
	object = vulkan_nondispatchable_object(handle);
	if (object == NULL)
		return NULL;

	/* Prevents another device or object family from entering this pipeline operation. */
	if (object->kind != kind || object->parent != &device->object)
		return NULL;

	/* Succeeded: the local object belongs to the same renderer namespace as its device. */
	return object;
}

/* Creates a dynamic batch while keeping every successful native result independently owned. */
static VkResult
pipeline_batch(
	struct VkDevice_T *device,
	VkPipelineCache cache_handle,
	uint32_t count,
	const void *infos,
	VkPipelineBindPoint bind_point,
	const VkAllocationCallbacks *allocator,
	VkPipeline *results)
{
	struct vulkan_pipeline **objects;
	struct vulkan_object *object;
	struct vulkan_object *cache;
	struct vulkan_allocator policy;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	const VkGraphicsPipelineCreateInfo *graphics;
	const VkComputePipelineCreateInfo *compute;
	uint64_t returned;
	uint64_t identifier;
	size_t bytes;
	size_t reply_bytes;
	uint32_t index;
	uint32_t opcode;
	VkResult error;
	VkResult native_result;

	/* All output handles start null, including failures before any native command. */
	if (results == NULL ||
	    infos == NULL ||
	    count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Clears every requested public output before allocation or native execution can fail. */
	for (index = 0U; index < count; index++)
		results[index] = VK_NULL_HANDLE;
	cache = NULL;

	/* Resolves the optional native cache only when this batch actually names one. */
	if (cache_handle != VK_NULL_HANDLE) {
		cache = pipeline_object(device, (uint64_t)cache_handle, VULKAN_OBJECT_PIPELINE_CACHE);
		if (cache == NULL)
			return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* The explicit allocation policy, or effective device policy, owns batch temporaries. */
	policy = device->object.allocator;

	/* Uses the compatible callback policy supplied for this specific API operation. */
	if (allocator != NULL) {
		policy.callbacks = *allocator;
		policy.has_callbacks = VK_TRUE;
	}

	bytes = (size_t)count * sizeof(*objects);
	if (bytes / sizeof(*objects) != count)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	reply_bytes = (size_t)count * 8U;
	if (reply_bytes / 8U != count || reply_bytes > SIZE_MAX - 16U)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	reply_bytes += 16U;
	objects = vulkan_allocate(&policy, bytes, sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	if (objects == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	memset(objects, 0, bytes);
	graphics = infos;
	compute = infos;

	/* Preallocation permits native partial success without a later local allocation failure. */
	for (index = 0U; index < count; index++) {
		error = vulkan_object_alloc(
			sizeof(*objects[index]),
			sizeof(uint64_t),
			VULKAN_OBJECT_PIPELINE,
			&device->object,
			device->object.context,
			allocator,
			VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
			&object);
		if (error != VK_SUCCESS)
			goto cleanup;
		objects[index] = (struct vulkan_pipeline *)object;
		objects[index]->bind_point = bind_point;

		/* Selects the metadata and encoder belonging to this pipeline family. */
		if (bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS)
			objects[index]->flags = graphics[index].flags;
		else
			objects[index]->flags = compute[index].flags;
		error = vulkan_object_reserve_id(object);
		if (error != VK_SUCCESS)
			goto cleanup;
	}

	/* One native command preserves derivative indices within this exact batch. */
	opcode = VULKAN_OPCODE_vkCreateGraphicsPipelines;
	if (bind_point == VK_PIPELINE_BIND_POINT_COMPUTE)
		opcode = VULKAN_OPCODE_vkCreateComputePipelines;
	vulkan_writer_init(&writer);
	writer.allocator = policy;
	vulkan_command_begin(&writer, opcode);
	vulkan_write_u64(&writer, device->object.wire_id);
	vulkan_write_u64(&writer, vulkan_object_wire_id(cache));
	vulkan_write_u32(&writer, count);
	vulkan_write_u64(&writer, count);

	/* Visits every batch member without imposing a fixed pipeline-object quota. */
	for (index = 0U; index < count; index++) {
		/* Stops before accessing later application inputs after the first encoding failure. */
		if (writer.error != VK_SUCCESS)
			break;

		/* Selects the metadata and encoder belonging to this pipeline family. */
		if (bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS)
			pipeline_encode_graphics(&writer, device, index, &graphics[index]);
		else
			pipeline_encode_compute(&writer, device, index, &compute[index]);
	}

	vulkan_write_u64(&writer, 0U);
	vulkan_write_u64(&writer, count);

	/* Supplies a distinct reserved native identity for every possible successful pipeline. */
	for (index = 0U; index < count; index++)
		vulkan_write_u64(&writer, objects[index]->object.wire_id);

	/* Executes the batch once so derivative indices retain their original input ordering. */
	native_result = vulkan_command_execute(
		device->object.context,
		&writer,
		reply_bytes,
		&reader,
		VK_TRUE);
	vulkan_writer_finish(&writer);

	/* Only a completed command/result header can carry legitimate partial successes. */
	error = native_result;
	if (reader.data == NULL ||
	    reader.cursor != 8U ||
	    reader.error != VK_SUCCESS) {
		error = vulkan_reply_finish(device->object.context, &reader, error);
		goto cleanup;
	}

	/* Validate the entire result vector before publishing any newly created handle. */
	returned = vulkan_read_u64(&reader);
	if (returned != count)
		reader.error = VK_ERROR_DEVICE_LOST;

	/* Visits every batch member without imposing a fixed pipeline-object quota. */
	for (index = 0U; index < count; index++) {
		identifier = vulkan_read_u64(&reader);

		/* Retires a preallocated local slot when the native batch did not create its pipeline. */
		if (identifier == 0U) {
			/* Treats a missing pipeline in a successful batch as an invalid native reply. */
			if (native_result == VK_SUCCESS)
				reader.error = VK_ERROR_DEVICE_LOST;
			vulkan_object_free(&objects[index]->object);
			objects[index] = NULL;
		} else if (identifier != objects[index]->object.wire_id) {
			reader.error = VK_ERROR_DEVICE_LOST;
		} else {
			objects[index]->created = VK_TRUE;
		}
	}

	error = vulkan_reply_finish(device->object.context, &reader, native_result);
	if (error == VK_ERROR_DEVICE_LOST) {
		pipeline_lost(device);
		goto cleanup;
	}

	/* A negative native result can still return ordinary successful pipeline handles. */
	for (index = 0U; index < count; index++) {
		/* Skips a slot whose ownership was already freed or transferred to its public handle. */
		if (objects[index] == NULL)
			continue;
		error = vulkan_object_publish(&objects[index]->object);
		if (error != VK_SUCCESS) {
			error = pipeline_lost(device);
			goto cleanup;
		}

		results[index] = (VkPipeline)vulkan_nondispatchable_handle(&objects[index]->object);
		objects[index] = NULL;
	}

	/* Preserves native partial success while sharing the same final ownership cleanup. */
	error = native_result;

cleanup:
	/* Only untransferred objects remain in this vector after partial publication. */
	for (index = 0U; index < count; index++) {
		/* Skips a slot whose ownership was already freed or transferred to its public handle. */
		if (objects[index] == NULL)
			continue;

		/* Destroys only native identities that this failed publication actually acquired. */
		if (objects[index]->created != VK_FALSE)
			vulkan_object_destroy_remote(device, &objects[index]->object, VULKAN_OPCODE_vkDestroyPipeline);
		vulkan_object_free(&objects[index]->object);
	}

	/* Releases the transaction vector after every remaining private object has retired. */
	vulkan_free(&policy, objects);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: all native pipelines are published with separate local ownership. */
	return VK_SUCCESS;
}

/* Encodes ordinary compute state after canonicalizing ignored derivative fields. */
static void
pipeline_encode_compute(
	struct vulkan_writer *writer,
	struct VkDevice_T *device,
	uint32_t index,
	const VkComputePipelineCreateInfo *info)
{
	VkComputePipelineCreateInfo copy;
	VkResult error;

	/* The input structure remains immutable throughout local encoding. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Refuses compute input that cannot have the required embedded shader-stage grammar. */
	if (info->sType != VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO) {
		writer->error = VK_ERROR_INITIALIZATION_FAILED;
		return;
	}

	copy = *info;
	error = pipeline_base(
		device,
		copy.flags,
		VK_PIPELINE_BIND_POINT_COMPUTE,
		index,
		&copy.basePipelineHandle,
		&copy.basePipelineIndex);
	if (error != VK_SUCCESS) {
		writer->error = error;
		return;
	}

	vulkan_encode_VkComputePipelineCreateInfo(writer, &copy);

	/* Succeeded: ignored handles never entered local lookup or the renderer decoder. */
	return;
}

/* Encodes graphics state while never dereferencing statically ignored pointers. */
static void
pipeline_encode_graphics(
	struct vulkan_writer *writer,
	struct VkDevice_T *device,
	uint32_t index,
	const VkGraphicsPipelineCreateInfo *info)
{
	VkPipeline base;
	int32_t base_index;
	VkShaderStageFlags stages;
	VkBool32 rasterize;
	VkBool32 color;
	VkBool32 depth;
	VkBool32 tessellate;
	VkPipelineMultisampleStateCreateInfo multisample;
	uint32_t stage;
	VkResult error;

	/* Graphics state selection depends on shader stages, rasterization and subpass use. */
	if (writer->error != VK_SUCCESS)
		return;

	/* Requires the always-relevant graphics prefix before selecting optional state. */
	if (info->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO ||
	    info->pRasterizationState == NULL ||
	    info->pStages == NULL ||
	    info->stageCount == 0U) {
		writer->error = VK_ERROR_INITIALIZATION_FAILED;
		return;
	}

	stages = 0U;

	/* Collects shader roles before deciding whether tessellation state can be read. */
	for (stage = 0U; stage < info->stageCount; stage++)
		stages |= info->pStages[stage].stage;
	tessellate = VK_FALSE;

	/* Enables tessellation state only when the pipeline includes a tessellation control stage. */
	if ((stages & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) != 0U)
		tessellate = VK_TRUE;
	rasterize = VK_FALSE;

	/* Selects fragment processing only when primitives survive rasterizer discard. */
	if (info->pRasterizationState->rasterizerDiscardEnable == VK_FALSE)
		rasterize = VK_TRUE;
	color = VK_FALSE;
	depth = VK_FALSE;

	/* Accesses downstream rasterization state only while that state affects generated fragments. */
	if (rasterize != VK_FALSE) {
		error = vulkan_render_pass_subpass(info->renderPass, info->subpass, &color, &depth);
		if (error != VK_SUCCESS) {
			writer->error = error;
			return;
		}
	}

	/* Derivative indices remain local to the batch; ignored handles become canonical null. */
	base = info->basePipelineHandle;
	base_index = info->basePipelineIndex;
	error = pipeline_base(
		device,
		info->flags,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		index,
		&base,
		&base_index);
	if (error != VK_SUCCESS) {
		writer->error = error;
		return;
	}

	/* Fixed-width protocol fields are encoded independently of native C structure padding. */
	vulkan_write_u32(writer, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
	vulkan_write_u64(writer, 0U);
	vulkan_write_u32(writer, info->flags);
	vulkan_write_u32(writer, info->stageCount);
	vulkan_write_u64(writer, info->stageCount);

	/* Encodes each selected shader module, entry point and specialization payload. */
	for (stage = 0U; stage < info->stageCount; stage++)
		vulkan_encode_VkPipelineShaderStageCreateInfo(writer, &info->pStages[stage]);

	/* Vulkan 1.0 vertex and input assembly state remain present even when fragments discard. */
	vulkan_write_pointer(writer, info->pVertexInputState);

	/* Encodes vertex fetch declarations only when their input marker is present. */
	if (info->pVertexInputState != NULL)
		vulkan_encode_VkPipelineVertexInputStateCreateInfo(writer, info->pVertexInputState);
	vulkan_write_pointer(writer, info->pInputAssemblyState);

	/* Encodes primitive assembly state selected by its input marker. */
	if (info->pInputAssemblyState != NULL)
		vulkan_encode_VkPipelineInputAssemblyStateCreateInfo(writer, info->pInputAssemblyState);

	/* Unused tessellation state may contain an invalid pointer and must never be read. */
	if (tessellate != VK_FALSE) {
		vulkan_write_pointer(writer, info->pTessellationState);

		/* Encodes patch state only after both shader selection and pointer presence permit access. */
		if (info->pTessellationState != NULL)
			vulkan_encode_VkPipelineTessellationStateCreateInfo(writer, info->pTessellationState);
	} else {
		vulkan_write_u64(writer, 0U);
	}

	/* Viewport arrays selected dynamically are omitted without changing their counts. */
	if (rasterize != VK_FALSE) {
		vulkan_write_pointer(writer, info->pViewportState);

		/* Encodes statically selected viewport state while respecting dynamic array omission. */
		if (info->pViewportState != NULL)
			pipeline_encode_viewport(writer, info->pViewportState, info->pDynamicState);
	} else {
		vulkan_write_u64(writer, 0U);
	}

	vulkan_write_u64(writer, 1U);
	pipeline_encode_rasterization(writer, info->pRasterizationState, info->pDynamicState);

	/* Multisampling is ignored when rasterization discards every primitive. */
	if (rasterize != VK_FALSE) {
		vulkan_write_pointer(writer, info->pMultisampleState);

		/* Reads multisample state only when rasterization makes it relevant. */
		if (info->pMultisampleState != NULL) {
			multisample = *info->pMultisampleState;

			/* Removes the ignored minimum shading fraction when per-sample shading is disabled. */
			if (multisample.sampleShadingEnable == VK_FALSE)
				multisample.minSampleShading = 0.0f;
			vulkan_encode_VkPipelineMultisampleStateCreateInfo(writer, &multisample);
		}
	} else {
		vulkan_write_u64(writer, 0U);
	}

	/* Subpasses without depth/stencil or color use must not dereference those state pointers. */
	if (rasterize != VK_FALSE && depth != VK_FALSE) {
		vulkan_write_pointer(writer, info->pDepthStencilState);

		/* Encodes depth and stencil state only for a subpass that uses those attachments. */
		if (info->pDepthStencilState != NULL)
			pipeline_encode_depth(writer, info->pDepthStencilState, info->pDynamicState);
	} else {
		vulkan_write_u64(writer, 0U);
	}

	/* Selects color blending only for surviving fragments and a subpass with color outputs. */
	if (rasterize != VK_FALSE && color != VK_FALSE) {
		vulkan_write_pointer(writer, info->pColorBlendState);

		/* Encodes the selected static blend state without inspecting an absent record. */
		if (info->pColorBlendState != NULL)
			pipeline_encode_blend(writer, info->pColorBlendState, info->pDynamicState);
	} else {
		vulkan_write_u64(writer, 0U);
	}

	/* Dynamic state and compatible object handles complete the selected graphics contract. */
	vulkan_write_pointer(writer, info->pDynamicState);

	/* Preserves the command-supplied state list when the application provided one. */
	if (info->pDynamicState != NULL)
		vulkan_encode_VkPipelineDynamicStateCreateInfo(writer, info->pDynamicState);
	vulkan_encode_handle(writer, (uint64_t)info->layout);
	vulkan_encode_handle(writer, (uint64_t)info->renderPass);
	vulkan_write_u32(writer, info->subpass);
	vulkan_encode_handle(writer, (uint64_t)base);
	vulkan_write_u32(writer, (uint32_t)base_index);

	/* Succeeded: only relevant graphics state was passed to the renderer. */
	return;
}

/* Canonicalizes unused derivative inputs before any local handle dereference. */
static VkResult
pipeline_base(
	struct VkDevice_T *device,
	VkPipelineCreateFlags flags,
	VkPipelineBindPoint bind_point,
	uint32_t index,
	VkPipeline *base,
	int32_t *base_index)
{
	struct vulkan_pipeline *pipeline;

	/* Non-derivative pipelines ignore both base fields, even if they contain stale bits. */
	if ((flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) == 0U) {
		*base = VK_NULL_HANDLE;
		*base_index = -1;
		return VK_SUCCESS;
	}

	/* Batch indices identify earlier input records without looking up public handles. */
	if (*base_index != -1) {
		/* Rejects derivative indices that do not identify an earlier member of this batch. */
		if (*base_index < 0 || (uint32_t)*base_index >= index)
			return VK_ERROR_INITIALIZATION_FAILED;
		*base = VK_NULL_HANDLE;
		return VK_SUCCESS;
	}

	/* An external base must belong to this device and allow the same pipeline family. */
	pipeline = (struct vulkan_pipeline *)pipeline_object(device,
	    (uint64_t)*base, VULKAN_OBJECT_PIPELINE);
	if (pipeline == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Requires the external base to support derivatives in the same graphics or compute family. */
	if (pipeline->bind_point != bind_point ||
	    (pipeline->flags & VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT) == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Succeeded: only a relevant valid public base handle may enter the wire encoder. */
	return VK_SUCCESS;
}

/* Tests a finite dynamic-state list without reading unrelated pipeline state arrays. */
static VkBool32
pipeline_dynamic(
	const VkPipelineDynamicStateCreateInfo *info,
	VkDynamicState state)
{
	uint32_t index;

	/* Absence means every applicable state is statically supplied. */
	if (info == NULL)
		return VK_FALSE;

	/* Searches the application dynamic-state list without consulting unrelated static fields. */
	for (index = 0U; index < info->dynamicStateCount; index++) {
		/* Identifies the command-supplied state which makes its corresponding static value ignored. */
		if (info->pDynamicStates[index] == state)
			return VK_TRUE;
	}

	/* Succeeded: the requested state is not dynamically supplied by this pipeline. */
	return VK_FALSE;
}

/* Omits dynamically supplied arrays while preserving their declared viewport counts. */
static void
pipeline_encode_viewport(
	struct vulkan_writer *writer,
	const VkPipelineViewportStateCreateInfo *info,
	const VkPipelineDynamicStateCreateInfo *dynamic)
{
	VkPipelineViewportStateCreateInfo copy;
	VkBool32 selected;

	/* Only pointers selected for static state may be dereferenced by the generic codec. */
	copy = *info;
	selected = pipeline_dynamic(dynamic, VK_DYNAMIC_STATE_VIEWPORT);
	if (selected != VK_FALSE)
		copy.pViewports = NULL;
	selected = pipeline_dynamic(dynamic, VK_DYNAMIC_STATE_SCISSOR);
	if (selected != VK_FALSE)
		copy.pScissors = NULL;
	vulkan_encode_VkPipelineViewportStateCreateInfo(writer, &copy);

	/* Succeeded: invalid ignored array pointers never reach memory access or the wire. */
	return;
}

/* Canonicalizes ignored rasterization scalar values before generic typed encoding. */
static void
pipeline_encode_rasterization(
	struct vulkan_writer *writer,
	const VkPipelineRasterizationStateCreateInfo *info,
	const VkPipelineDynamicStateCreateInfo *dynamic)
{
	VkPipelineRasterizationStateCreateInfo copy;
	VkBool32 selected;

	/* Dynamic line width does not read or transmit a meaningless static value. */
	copy = *info;
	selected = pipeline_dynamic(dynamic, VK_DYNAMIC_STATE_LINE_WIDTH);
	if (selected != VK_FALSE)
		copy.lineWidth = 1.0f;

	/* Disabled or dynamic depth bias does not constrain the three ignored floats. */
	selected = pipeline_dynamic(dynamic, VK_DYNAMIC_STATE_DEPTH_BIAS);
	if (selected != VK_FALSE || copy.depthBiasEnable == VK_FALSE) {
		copy.depthBiasConstantFactor = 0.0f;
		copy.depthBiasClamp = 0.0f;
		copy.depthBiasSlopeFactor = 0.0f;
	}

	vulkan_encode_VkPipelineRasterizationStateCreateInfo(writer, &copy);

	/* Succeeded: selected static rasterization state has one deterministic encoding. */
	return;
}

/* Canonicalizes dynamically provided depth and stencil scalar fields. */
static void
pipeline_encode_depth(
	struct vulkan_writer *writer,
	const VkPipelineDepthStencilStateCreateInfo *info,
	const VkPipelineDynamicStateCreateInfo *dynamic)
{
	VkPipelineDepthStencilStateCreateInfo copy;
	VkBool32 selected;

	/* Disabled tests have no meaningful compare/write or bound values. */
	copy = *info;
	if (copy.depthTestEnable == VK_FALSE) {
		copy.depthWriteEnable = VK_FALSE;
		copy.depthCompareOp = VK_COMPARE_OP_ALWAYS;
	}

	selected = pipeline_dynamic(dynamic, VK_DYNAMIC_STATE_DEPTH_BOUNDS);
	if (selected != VK_FALSE || copy.depthBoundsTestEnable == VK_FALSE) {
		copy.minDepthBounds = 0.0f;
		copy.maxDepthBounds = 1.0f;
	}

	/* Inactive stencil tests do not impose constraints on either ignored face state. */
	if (copy.stencilTestEnable == VK_FALSE) {
		memset(&copy.front, 0, sizeof(copy.front));
		memset(&copy.back, 0, sizeof(copy.back));
		copy.front.compareOp = VK_COMPARE_OP_ALWAYS;
		copy.back.compareOp = VK_COMPARE_OP_ALWAYS;
	}

	selected = pipeline_dynamic(dynamic, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK);
	if (selected != VK_FALSE) {
		copy.front.compareMask = 0U;
		copy.back.compareMask = 0U;
	}

	selected = pipeline_dynamic(dynamic, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK);
	if (selected != VK_FALSE) {
		copy.front.writeMask = 0U;
		copy.back.writeMask = 0U;
	}

	selected = pipeline_dynamic(dynamic, VK_DYNAMIC_STATE_STENCIL_REFERENCE);
	if (selected != VK_FALSE) {
		copy.front.reference = 0U;
		copy.back.reference = 0U;
	}

	vulkan_encode_VkPipelineDepthStencilStateCreateInfo(writer, &copy);

	/* Succeeded: only applicable static depth and stencil values affect the native pipeline. */
	return;
}

/* Encodes color blending without reading unused dynamically selected blend constants. */
static void
pipeline_encode_blend(
	struct vulkan_writer *writer,
	const VkPipelineColorBlendStateCreateInfo *info,
	const VkPipelineDynamicStateCreateInfo *dynamic)
{
	VkPipelineColorBlendStateCreateInfo copy;
	VkBool32 selected;

	/* Constant blending inputs are supplied by commands when this state is dynamic. */
	copy = *info;
	selected = pipeline_dynamic(dynamic, VK_DYNAMIC_STATE_BLEND_CONSTANTS);
	if (selected != VK_FALSE)
		memset(copy.blendConstants, 0, sizeof(copy.blendConstants));

	/* Removes the ignored logical operation when color logic operations are disabled. */
	if (copy.logicOpEnable == VK_FALSE)
		copy.logicOp = VK_LOGIC_OP_COPY;
	vulkan_encode_VkPipelineColorBlendStateCreateInfo(writer, &copy);

	/* Succeeded: the typed codec receives only semantically applicable static state. */
	return;
}

/* Publishes terminal namespace loss to both device and shared context API paths. */
static VkResult
pipeline_lost(
	struct VkDevice_T *device)
{
	/* Uncertain or malformed native object ownership cannot be treated as a local retry. */
	__atomic_store_n(&device->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);
	__atomic_store_n(&device->object.context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);

	/* The caller must report a terminal error rather than expose a foreign native handle. */
	return VK_ERROR_DEVICE_LOST;
}
