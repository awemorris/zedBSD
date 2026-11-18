/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's shader modules, layouts and descriptors: shader modules,
 * descriptor set layouts, pipeline layouts, pipeline caches, descriptor pools
 * and their sets, and the updates of the sets.
 *
 * A set's binding names the image view and the sampler of a combined image
 * sampler, the range of a uniform or storage buffer (dynamic or not: a
 * dynamic one's bind adds its offset), or the buffer view of a uniform texel
 * buffer; other kinds are read and left unbound (a draw that reads one is
 * refused).  A set holds what it names, so a view, sampler, buffer or buffer
 * view destroyed while a set names it lives on until the set lets go.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/nvrtx/vk/nvrtx-vk-objects.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The largest SPIR-V module, the most sets one allocation hands out, and the most items of one update. */
#define LAYOUT_SHADER_BYTES		(256U * 1024U)
#define LAYOUT_ALLOCATED_SETS		64U
#define LAYOUT_UPDATE_ITEMS		64U

static int layout_create_shader(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int layout_create_set_layout(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int layout_create_pipeline_layout(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int layout_create_cache(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int layout_destroy_cache(struct i915_wire_reader *reader);
static int layout_create_pool(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int layout_destroy_pool(struct nvrtx_vk_session *session, struct i915_wire_reader *reader);
static int layout_reset_pool(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int layout_pool_sets_free(struct nvrtx_vk_session *session, const void *pool);
static int layout_allocate_sets(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int layout_free_sets(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int layout_update_sets(struct nvrtx_vk_session *session, struct i915_wire_reader *reader);
static int layout_update_write(struct nvrtx_vk_session *session, struct i915_wire_reader *reader);
static int layout_update_copy(struct nvrtx_vk_session *session, const VkCopyDescriptorSet *copy);
static bool layout_type_buffer(uint32_t type);
static int layout_slot_set(struct nvrtx_vk_descriptor_slot *slot, struct nvrtx_vk_object *view, struct nvrtx_vk_object *sampler);
static int layout_slot_buffer(struct nvrtx_vk_descriptor_slot *slot, struct nvrtx_vk_object *buffer, uint64_t offset, uint64_t range);
static int layout_slot_texel(struct nvrtx_vk_descriptor_slot *slot, struct nvrtx_vk_object *view);
static int layout_slot_clear(struct nvrtx_vk_descriptor_slot *slot);
static void layout_set_dynamic(struct nvrtx_vk_descriptor_set *set, const struct nvrtx_vk_set_layout *layout);
static int layout_set_elements(struct nvrtx_vk_descriptor_set *set, const struct nvrtx_vk_set_layout *layout);
static struct nvrtx_vk_descriptor_slot *layout_element(struct nvrtx_vk_descriptor_set *set, uint32_t binding, uint32_t element);
static int layout_set_destroy(struct nvrtx_vk_session *session, void *payload);

/*
 * Runs a shader module, layout, cache or descriptor command; *handled
 * stays clear for an opcode of another handler's.
 */
int
nvrtx_vk_layout_route(
	struct nvrtx_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	UNUSED_PARAMETER(requested);

	/* Picks the command, or leaves it to another handler. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_SHADER_MODULE:
		error = layout_create_shader(session, reader, reply);
		break;
	case GPU_OP_DESTROY_SHADER_MODULE:
		error = nvrtx_vk_destroy_plain(session, reader, I915_VK_OBJ_SHADER_MODULE);
		break;
	case GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT:
		error = layout_create_set_layout(session, reader, reply);
		break;
	case GPU_OP_DESTROY_DESCRIPTOR_SET_LAYOUT:
		error = nvrtx_vk_destroy_plain(session, reader, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT);
		break;
	case GPU_OP_CREATE_PIPELINE_LAYOUT:
		error = layout_create_pipeline_layout(session, reader, reply);
		break;
	case GPU_OP_DESTROY_PIPELINE_LAYOUT:
		error = nvrtx_vk_destroy_plain(session, reader, I915_VK_OBJ_PIPELINE_LAYOUT);
		break;
	case GPU_OP_CREATE_PIPELINE_CACHE:
		error = layout_create_cache(session, reader, reply);
		break;
	case GPU_OP_DESTROY_PIPELINE_CACHE:
		error = layout_destroy_cache(reader);
		break;
	case GPU_OP_CREATE_DESCRIPTOR_POOL:
		error = layout_create_pool(session, reader, reply);
		break;
	case GPU_OP_DESTROY_DESCRIPTOR_POOL:
		error = layout_destroy_pool(session, reader);
		break;
	case GPU_OP_RESET_DESCRIPTOR_POOL:
		error = layout_reset_pool(session, reader, reply);
		break;
	case GPU_OP_ALLOCATE_DESCRIPTOR_SETS:
		error = layout_allocate_sets(session, reader, reply);
		break;
	case GPU_OP_FREE_DESCRIPTOR_SETS:
		error = layout_free_sets(session, reader, reply);
		break;
	case GPU_OP_UPDATE_DESCRIPTOR_SETS:
		error = layout_update_sets(session, reader);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* Reports a command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran and wrote its reply. */
	return 0;
}

/*
 * vkCreateShaderModule: [device][present][VkShaderModuleCreateInfo] and
 * the create's tail -> the create's reply.  The module keeps a copy of its
 * words; one not of whole words, empty or too large is refused.
 */
static int
layout_create_shader(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkShaderModuleCreateInfo info;
	struct nvrtx_vk_shader *shader;
	uint64_t identity;
	uint32_t words;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkShaderModuleCreateInfo(reader, &session->arena, &info);
	identity = nvrtx_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Whole words of code, some of them, within the largest module. */
	error = 0;
	if (info.pCode == NULL || info.codeSize == 0U) {
		error = EINVAL;
	} else if ((info.codeSize & 3U) != 0U) {
		error = EINVAL;
	} else if (info.codeSize > LAYOUT_SHADER_BYTES) {
		error = ENOTSUP;
	}

	/* The module with its words behind it (the command's scratch is emptied after the command). */
	shader = NULL;
	if (error == 0) {
		words = (uint32_t)(info.codeSize / 4U);
		shader = kern_calloc(1U, sizeof(*shader) + (size_t)words * 4U);
		if (shader != NULL) {
			shader->words = (uint32_t *)(shader + 1);
			shader->word_count = words;
			kern_memcpy(shader->words, info.pCode, (size_t)words * 4U);
		}
	}

	/* Publishes the module and answers. */
	nvrtx_vk_create_reply(session, reply, I915_VK_OBJ_SHADER_MODULE, identity, shader, nvrtx_vk_free_payload, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * vkCreateDescriptorSetLayout: [device][present]
 * [VkDescriptorSetLayoutCreateInfo] and the create's tail -> the create's
 * reply.  Binding numbers past the ones a set holds are refused.
 */
static int
layout_create_set_layout(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDescriptorSetLayoutCreateInfo info;
	struct nvrtx_vk_set_layout *layout;
	uint64_t identity;
	uint32_t index;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkDescriptorSetLayoutCreateInfo(reader, &session->arena, &info);
	identity = nvrtx_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* No more bindings, and no higher binding number, than a set holds. */
	error = 0;
	if (info.bindingCount > NVRTX_VK_BINDINGS_MAX)
		error = ENOTSUP;
	if (error == 0 && info.bindingCount != 0U && info.pBindings == NULL)
		error = EINVAL;
	for (index = 0U; error == 0 && index < info.bindingCount; index++) {
		if (info.pBindings[index].binding >= NVRTX_VK_BINDINGS_MAX) {
			kern_logf("nvrtx: vk: a set layout's binding %u is past the %u a set holds\n",
				  (unsigned)info.pBindings[index].binding, (unsigned)NVRTX_VK_BINDINGS_MAX);
			error = ENOTSUP;
		}
	}

	/*
	 * No binding of more descriptors than a set holds, and no array of
	 * dynamic buffers (a bind gives one offset for each descriptor, the
	 * executor reads a binding's first).
	 */
	for (index = 0U; error == 0 && index < info.bindingCount; index++) {
		if (info.pBindings[index].descriptorCount > NVRTX_VK_ELEMENTS_MAX)
			error = ENOTSUP;
		if ((info.pBindings[index].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
		     info.pBindings[index].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) &&
		    info.pBindings[index].descriptorCount > 1U) {
			kern_logf("nvrtx: vk: an array of %u dynamic buffers is not taken\n", (unsigned)info.pBindings[index].descriptorCount);
			error = ENOTSUP;
		}
	}

	/* The layout keeps each binding's number, type and count. */
	layout = NULL;
	if (error == 0) {
		layout = kern_calloc(1U, sizeof(*layout));
		if (layout != NULL) {
			layout->count = info.bindingCount;
			for (index = 0U; index < info.bindingCount; index++) {
				layout->bindings[index].binding = info.pBindings[index].binding;
				layout->bindings[index].type = info.pBindings[index].descriptorType;
				layout->bindings[index].count = info.pBindings[index].descriptorCount;
			}
		}
	}

	/* Publishes the layout and answers. */
	nvrtx_vk_create_reply(session, reply, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT, identity, layout, nvrtx_vk_free_payload, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * vkCreatePipelineLayout: [device][present][VkPipelineLayoutCreateInfo]
 * and the create's tail -> the create's reply.  The layout keeps its set
 * count and the bytes its push constant ranges reach.
 */
static int
layout_create_pipeline_layout(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPipelineLayoutCreateInfo info;
	struct nvrtx_vk_pipeline_layout *layout;
	uint64_t identity;
	uint32_t index;
	uint32_t end;
	uint32_t push_bytes;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkPipelineLayoutCreateInfo(reader, &session->arena, &info);
	identity = nvrtx_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The bytes the push constant ranges reach. */
	push_bytes = 0U;
	for (index = 0U; index < info.pushConstantRangeCount && info.pPushConstantRanges != NULL; index++) {
		end = info.pPushConstantRanges[index].offset + info.pPushConstantRanges[index].size;
		if (end > push_bytes)
			push_bytes = end;
	}

	/* No more sets, and no more push constant bytes, than a pipeline binds. */
	error = 0;
	if (info.setLayoutCount > NVRTX_VK_SETS_MAX) {
		error = ENOTSUP;
	} else if (push_bytes > NVRTX_VK_PUSH_BYTES) {
		error = ENOTSUP;
	}

	/* The layout. */
	layout = NULL;
	if (error == 0) {
		layout = kern_calloc(1U, sizeof(*layout));
		if (layout != NULL) {
			layout->set_count = info.setLayoutCount;
			layout->push_bytes = push_bytes;
		}
	}

	/* Publishes the layout and answers. */
	nvrtx_vk_create_reply(session, reply, I915_VK_OBJ_PIPELINE_LAYOUT, identity, layout, nvrtx_vk_free_payload, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * vkCreatePipelineCache: [device][present][VkPipelineCacheCreateInfo] and
 * the create's tail -> the create's reply.  The executor keeps no cache:
 * the identity is answered and not recorded, so its destroy finds nothing.
 */
static int
layout_create_cache(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPipelineCacheCreateInfo info;
	uint64_t identity;

	/* The create info behind the device and its presence word; its data is not kept. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkPipelineCacheCreateInfo(reader, &session->arena, &info);
	identity = nvrtx_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Success with the identity. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/* vkDestroyPipelineCache: [device][identity][allocator], no reply body; nothing was kept. */
static int
layout_destroy_cache(
	struct i915_wire_reader *reader)
{
	/* The device, the identity and the allocator are read. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: there is nothing to destroy. */
	return 0;
}

/*
 * vkCreateDescriptorPool: [device][present][VkDescriptorPoolCreateInfo]
 * and the create's tail -> the create's reply.
 */
static int
layout_create_pool(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDescriptorPoolCreateInfo info;
	struct nvrtx_vk_descriptor_pool *pool;
	uint64_t identity;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkDescriptorPoolCreateInfo(reader, &session->arena, &info);
	identity = nvrtx_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The pool keeps its maxSets. */
	pool = kern_calloc(1U, sizeof(*pool));
	if (pool != NULL)
		pool->max_sets = info.maxSets;

	/* Publishes the pool and answers. */
	nvrtx_vk_create_reply(session, reply, I915_VK_OBJ_DESCRIPTOR_POOL, identity, pool, nvrtx_vk_free_payload, 0);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * vkDestroyDescriptorPool: [device][identity][allocator], no reply body.
 * The pool's sets go with it.
 */
static int
layout_destroy_pool(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader)
{
	const struct nvrtx_vk_descriptor_pool *pool;
	uint64_t identity;
	int error;

	/* The identity between the device and the allocator. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* An unknown pool is already gone. */
	pool = nvrtx_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_POOL, identity);
	if (pool == NULL)
		return 0;

	/* The pool's sets, then the pool. */
	error = layout_pool_sets_free(session, pool);
	if (error == 0)
		error = nvrtx_vk_object_remove(session, I915_VK_OBJ_DESCRIPTOR_POOL, identity);
	if (error != 0)
		return error;

	/* Succeeded: the pool and its sets are gone. */
	return 0;
}

/*
 * vkResetDescriptorPool: [device][identity][flags] -> [result].  The
 * pool's sets go; the pool stays.
 */
static int
layout_reset_pool(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	const struct nvrtx_vk_descriptor_pool *pool;
	uint64_t identity;
	uint32_t result;
	int error;

	/* The identity between the device and the flags. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The sets of a known pool go. */
	error = 0;
	pool = nvrtx_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_POOL, identity);
	if (pool != NULL)
		error = layout_pool_sets_free(session, pool);

	/* The reset's result. */
	result = nvrtx_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the reply carries the result. */
	return 0;
}

/*
 * Removes every published set of a pool.  The list changes with each
 * removal, so the walk starts over after one.
 */
static int
layout_pool_sets_free(
	struct nvrtx_vk_session *session,
	const void *pool)
{
	struct nvrtx_vk_object *object;
	const struct nvrtx_vk_descriptor_set *set;
	bool removed;
	int error;

	/* Until a walk finds no set of the pool. */
	do {
		removed = false;
		for (object = session->objects; object != NULL; object = object->next) {
			if (object->kind != I915_VK_OBJ_DESCRIPTOR_SET)
				continue;
			set = object->payload;
			if (set->pool != pool)
				continue;

			/* Unpublishes it; whatever still holds it keeps it. */
			error = nvrtx_vk_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, object->identity);
			if (error != 0)
				return error;
			removed = true;
			break;
		}
	} while (removed);

	/* Succeeded: the pool has no set. */
	return 0;
}

/*
 * vkAllocateDescriptorSets: [device][present][VkDescriptorSetAllocateInfo]
 * [count][identities] -> [result][count][identities].  Every set of the
 * batch is published, or none.
 */
static int
layout_allocate_sets(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDescriptorSetAllocateInfo info;
	uint64_t identities[LAYOUT_ALLOCATED_SETS];
	const struct nvrtx_vk_descriptor_pool *pool;
	const struct nvrtx_vk_set_layout *set_layout;
	struct nvrtx_vk_descriptor_set *set;
	struct nvrtx_vk_object *object;
	uint64_t count;
	uint64_t index;
	uint64_t published;
	uint32_t result;
	int error;

	/* The allocate info behind the device and its presence word, then the identities. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkDescriptorSetAllocateInfo(reader, &session->arena, &info);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count > LAYOUT_ALLOCATED_SETS || count != info.descriptorSetCount)
		return EINVAL;
	if (count != 0U && info.pSetLayouts == NULL)
		return EINVAL;

	/* The identity libvulkan chose for each set. */
	for (index = 0U; index < count; index++)
		identities[index] = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Sets of a known pool. */
	error = 0;
	pool = nvrtx_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_POOL, (uint64_t)(uintptr_t)info.descriptorPool);
	if (pool == NULL)
		error = EINVAL;

	/* Publishes each set, up to the first failure. */
	published = 0U;
	for (index = 0U; error == 0 && index < count; index++) {
		set = kern_calloc(1U, sizeof(*set));
		if (set == NULL) {
			error = ENOMEM;
			break;
		}

		/* Its pool, and the bindings that take a bind's dynamic offsets, from its layout. */
		set->pool = pool;
		set_layout = nvrtx_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT, (uint64_t)(uintptr_t)info.pSetLayouts[index]);
		if (set_layout != NULL) {
			layout_set_dynamic(set, set_layout);
			error = layout_set_elements(set, set_layout);
			if (error != 0) {
				kern_free(set);
				break;
			}
		}

		/* Published. */
		error = nvrtx_vk_object_publish(session, I915_VK_OBJ_DESCRIPTOR_SET, identities[index], set, layout_set_destroy, &object);
		if (error != 0) {
			kern_free(set);
			break;
		}

		published++;
	}

	/* A batch that failed part-way leaves no set. */
	if (error != 0) {
		for (index = 0U; index < published; index++)
			(void)nvrtx_vk_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, identities[index]);
	}

	/* The result, and on success every set's identity. */
	result = nvrtx_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);
	if (error == 0) {
		drv_i915_wire_reply_u64(reply, count);
		for (index = 0U; index < count; index++)
			drv_i915_wire_reply_u64(reply, identities[index]);
	}

	/* Succeeded: the reply carries the allocation's result. */
	return 0;
}

/*
 * vkFreeDescriptorSets: [device][pool][count][count]{identity} ->
 * [result].  Each named set of the session goes.
 */
static int
layout_free_sets(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t count;
	uint64_t index;
	uint64_t identity;
	uint32_t result;
	int error;
	int failed;

	/* The count behind the device, the pool and the count's first form. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count > LAYOUT_ALLOCATED_SETS)
		return EINVAL;

	/* Removes each set; an unknown one is already gone. */
	failed = 0;
	for (index = 0U; index < count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			return EINVAL;

		error = nvrtx_vk_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, identity);
		if (error != 0 && error != ENOENT && failed == 0)
			failed = error;
	}

	/* The free's result. */
	result = nvrtx_vk_result(failed);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the reply carries the result. */
	return 0;
}

/*
 * vkUpdateDescriptorSets: [device][n][n]{write}[m][m]{copy}, no reply
 * body: the writes, then the copies, each in order.
 */
static int
layout_update_sets(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader)
{
	VkCopyDescriptorSet copy;
	uint64_t writes;
	uint64_t copies;
	uint64_t index;
	int error;

	/* How many writes follow, behind the device and the count's first form. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	writes = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (writes > LAYOUT_UPDATE_ITEMS)
		return EINVAL;

	/* Applies each write. */
	for (index = 0U; index < writes; index++) {
		error = layout_update_write(session, reader);
		if (error != 0)
			return error;
	}

	/* How many copies follow; each is applied. */
	(void)drv_i915_wire_read_u32(reader);
	copies = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (copies > LAYOUT_UPDATE_ITEMS)
		return EINVAL;
	for (index = 0U; index < copies; index++) {
		kern_memset(&copy, 0, sizeof(copy));
		i915_vkc_dec_VkCopyDescriptorSet(reader, &session->arena, &copy);
		if (reader->error != 0)
			return EINVAL;
		error = layout_update_copy(session, &copy);
		if (error != 0)
			return error;
	}

	/* Succeeded: the writes and the copies are applied. */
	return 0;
}

/*
 * Applies one descriptor copy: each of its descriptors, from the source
 * set's binding and element on to the destination's (going on into the
 * next bindings past a binding's last element), names what the source
 * names (a view and a sampler, or a buffer's range).  A set or an element
 * that is not there is passed over, as a write's is.
 *
 * Procedure referenced: Vulkan 1.3 VkCopyDescriptorSet (srcSet,
 * srcBinding, srcArrayElement, dstSet, dstBinding, dstArrayElement,
 * descriptorCount); the same as AMDRDNA's executor applies it.
 */
static int
layout_update_copy(
	struct nvrtx_vk_session *session,
	const VkCopyDescriptorSet *copy)
{
	struct nvrtx_vk_descriptor_set *source_set;
	struct nvrtx_vk_descriptor_set *destination_set;
	struct nvrtx_vk_descriptor_slot *source;
	struct nvrtx_vk_descriptor_slot *destination;
	uint32_t item;
	int error;

	/* The two sets (the decoded handles are the wire's identities). */
	source_set = nvrtx_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_SET, (uint64_t)(uintptr_t)copy->srcSet);
	destination_set = nvrtx_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_SET, (uint64_t)(uintptr_t)copy->dstSet);
	if (source_set == NULL || destination_set == NULL)
		return 0;

	/* Each descriptor: the buffer's range, the buffer view, or the view and the sampler. */
	for (item = 0U; item < copy->descriptorCount && item < LAYOUT_UPDATE_ITEMS; item++) {
		source = layout_element(source_set, copy->srcBinding, copy->srcArrayElement + item);
		destination = layout_element(destination_set, copy->dstBinding, copy->dstArrayElement + item);
		if (source == NULL || destination == NULL || source == destination)
			continue;
		if (source->buffer != NULL) {
			error = layout_slot_buffer(destination, source->buffer, source->offset, source->range);
		} else if (source->texel != NULL) {
			error = layout_slot_texel(destination, source->texel);
		} else {
			error = layout_slot_set(destination, source->view, source->sampler);
		}
		if (error != 0)
			return error;
	}

	/* Succeeded: the descriptors are copied. */
	return 0;
}

/*
 * Reads one descriptor write and applies its combined image samplers,
 * uniform buffers, storage buffers or uniform texel buffers to the set's binding from its element
 * on (going on into the next bindings past a binding's last element; one
 * past the set's last is passed over): [sType][pNext][set][binding]
 * [element][count][type][images]{[sampler][view][layout]}[buffers]
 * {VkDescriptorBufferInfo}[texel views]{identity}.
 */
static int
layout_update_write(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader)
{
	VkDescriptorBufferInfo buffer;
	struct nvrtx_vk_descriptor_set *set;
	struct nvrtx_vk_descriptor_slot *slot;
	struct nvrtx_vk_object *view;
	struct nvrtx_vk_object *sampler;
	struct nvrtx_vk_object *object;
	uint64_t set_id;
	uint64_t count;
	uint64_t item;
	uint64_t sampler_id;
	uint64_t view_id;
	uint64_t buffer_id;
	uint32_t binding;
	uint32_t element;
	uint32_t type;
	int error;

	/* The set, the binding, the element and the type behind sType and pNext. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	set_id = drv_i915_wire_read_u64(reader);
	binding = drv_i915_wire_read_u32(reader);
	element = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count > LAYOUT_UPDATE_ITEMS)
		return EINVAL;
	set = nvrtx_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_SET, set_id);

	/* The image descriptors, from the element on: each combined image sampler is bound. */
	for (item = 0U; item < count; item++) {
		sampler_id = drv_i915_wire_read_u64(reader);
		view_id = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;

		/* An element of a known set's binding, as a combined image sampler. */
		slot = NULL;
		if (set != NULL)
			slot = layout_element(set, binding, element + (uint32_t)item);
		if (slot == NULL)
			continue;
		if (type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
			if (!layout_type_buffer(type))
				kern_logf("nvrtx: vk: vkUpdateDescriptorSets: descriptors of type %u are not bound\n", (unsigned)type);
			continue;
		}

		/* The binding samples this view with this sampler from now on. */
		view = nvrtx_vk_object_find(session, I915_VK_OBJ_IMAGE_VIEW, view_id);
		sampler = nvrtx_vk_object_find(session, I915_VK_OBJ_SAMPLER, sampler_id);
		error = layout_slot_set(slot, view, sampler);
		if (error != 0)
			return error;
	}

	/* The buffer descriptors, from the element on: each uniform or storage buffer is bound. */
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count > LAYOUT_UPDATE_ITEMS)
		return EINVAL;
	for (item = 0U; item < count; item++) {
		kern_memset(&buffer, 0, sizeof(buffer));
		i915_vkc_dec_VkDescriptorBufferInfo(reader, &session->arena, &buffer);
		if (reader->error != 0)
			return EINVAL;

		/* An element of a known set's binding, as a uniform or storage buffer (dynamic or not). */
		slot = NULL;
		if (set != NULL)
			slot = layout_element(set, binding, element + (uint32_t)item);
		if (slot == NULL)
			continue;
		if (!layout_type_buffer(type))
			continue;

		/* The decoded handle is the wire's 64-bit identity; the binding reads this range from now on. */
		kern_memcpy(&buffer_id, &buffer.buffer, sizeof(buffer_id));
		object = nvrtx_vk_object_find(session, I915_VK_OBJ_BUFFER, buffer_id);
		error = layout_slot_buffer(slot, object, buffer.offset, buffer.range);
		if (error != 0)
			return error;
	}

	/* The texel buffer views, from the element on: each uniform texel buffer is bound. */
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count > LAYOUT_UPDATE_ITEMS)
		return EINVAL;
	for (item = 0U; item < count; item++) {
		view_id = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			return EINVAL;

		/* An element of a known set's binding, as a uniform texel buffer (a storage one is not bound). */
		slot = NULL;
		if (set != NULL)
			slot = layout_element(set, binding, element + (uint32_t)item);
		if (slot == NULL)
			continue;
		if (type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
			kern_logf("nvrtx: vk: vkUpdateDescriptorSets: texel buffers of type %u are not bound\n", (unsigned)type);
			continue;
		}

		/* The binding reads this view's texels from now on. */
		object = nvrtx_vk_object_find(session, I915_VK_OBJ_BUFFER_VIEW, view_id);
		error = layout_slot_texel(slot, object);
		if (error != 0)
			return error;
	}

	/* Succeeded: the write is read and its combined image samplers, uniform or storage buffers and texel buffers bound. */
	return 0;
}

/* Tells whether a descriptor type names a buffer range: a uniform or a storage buffer, dynamic or not. */
static bool
layout_type_buffer(
	uint32_t type)
{
	/* The four buffer types. */
	switch (type) {
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
		return true;
	default:
		return false;
	}
}

/*
 * Points a binding at a view and a sampler (either may be NULL): the
 * binding holds the new ones and lets the old ones go.
 */
static int
layout_slot_set(
	struct nvrtx_vk_descriptor_slot *slot,
	struct nvrtx_vk_object *view,
	struct nvrtx_vk_object *sampler)
{
	int error;

	/* Holds the new ones first: one of them may be the old one. */
	if (view != NULL) {
		error = nvrtx_vk_object_retain(view);
		if (error != 0)
			return error;
	}

	if (sampler != NULL) {
		error = nvrtx_vk_object_retain(sampler);
		if (error != 0) {
			(void)nvrtx_vk_object_release(view);
			return error;
		}
	}

	/* Lets the old ones go. */
	error = layout_slot_clear(slot);

	/* The binding names the new ones. */
	slot->view = view;
	slot->sampler = sampler;

	/* Reports an old one whose destruction failed. */
	if (error != 0)
		return error;

	/* Succeeded: the binding names the view and the sampler. */
	return 0;
}

/*
 * Points a binding at a buffer's range (the buffer may be NULL): the
 * binding holds the new buffer and lets the old one, and any view and
 * sampler, go.
 */
static int
layout_slot_buffer(
	struct nvrtx_vk_descriptor_slot *slot,
	struct nvrtx_vk_object *buffer,
	uint64_t offset,
	uint64_t range)
{
	int error;

	/* Holds the new one first: it may be the old one. */
	if (buffer != NULL) {
		error = nvrtx_vk_object_retain(buffer);
		if (error != 0)
			return error;
	}

	/* Lets the old ones go. */
	error = layout_slot_clear(slot);

	/* The binding names the new range. */
	slot->buffer = buffer;
	slot->offset = offset;
	slot->range = range;

	/* Reports an old one whose destruction failed. */
	if (error != 0)
		return error;

	/* Succeeded: the binding names the buffer's range. */
	return 0;
}

/*
 * Points a binding at a buffer view (which may be NULL): the binding holds
 * the new view and lets the old one, and any image view, sampler and
 * buffer, go.
 */
static int
layout_slot_texel(
	struct nvrtx_vk_descriptor_slot *slot,
	struct nvrtx_vk_object *view)
{
	int error;

	/* Holds the new one first: it may be the old one. */
	if (view != NULL) {
		error = nvrtx_vk_object_retain(view);
		if (error != 0)
			return error;
	}

	/* Lets the old ones go. */
	error = layout_slot_clear(slot);

	/* The binding names the new view. */
	slot->texel = view;

	/* Reports an old one whose destruction failed. */
	if (error != 0)
		return error;

	/* Succeeded: the binding names the buffer view. */
	return 0;
}

/* Lets go of a binding's view, sampler, buffer and buffer view. */
static int
layout_slot_clear(
	struct nvrtx_vk_descriptor_slot *slot)
{
	int first_error;
	int error;

	/* The view, the sampler, the buffer, then the buffer view; the first failure is reported. */
	first_error = nvrtx_vk_object_release(slot->view);
	error = nvrtx_vk_object_release(slot->sampler);
	if (first_error == 0)
		first_error = error;
	error = nvrtx_vk_object_release(slot->buffer);
	if (first_error == 0)
		first_error = error;
	error = nvrtx_vk_object_release(slot->texel);
	if (first_error == 0)
		first_error = error;
	slot->view = NULL;
	slot->sampler = NULL;
	slot->buffer = NULL;
	slot->texel = NULL;
	slot->offset = 0U;
	slot->range = 0U;

	/* Reports a destruction that failed. */
	if (first_error != 0)
		return first_error;

	/* Succeeded: the binding names nothing. */
	return 0;
}

/*
 * Marks the bindings of a new set that take a bind's dynamic offsets: each
 * dynamic uniform or storage buffer binding of its layout takes one for
 * each of its descriptors (they are given in the order of the binding
 * numbers), and lists them in that order with the offsets they take.
 */
static void
layout_set_dynamic(
	struct nvrtx_vk_descriptor_set *set,
	const struct nvrtx_vk_set_layout *layout)
{
	const struct nvrtx_vk_layout_binding *binding;
	uint32_t index;

	/* Each binding of the layout (a number past the set's was refused when the layout was made). */
	for (index = 0U; index < layout->count; index++) {
		binding = &layout->bindings[index];
		if (binding->binding >= NVRTX_VK_BINDINGS_MAX)
			continue;
		if (binding->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && binding->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
			continue;
		set->slots[binding->binding].dynamic = binding->count;
	}

	/* The dynamic bindings by number, and the offsets they take. */
	set->dynamic_binding_count = 0U;
	set->dynamic_total = 0U;
	for (index = 0U; index < NVRTX_VK_BINDINGS_MAX; index++) {
		if (set->slots[index].dynamic == 0U)
			continue;
		set->dynamic_bindings[set->dynamic_binding_count] = index;
		set->dynamic_binding_count++;
		set->dynamic_total += set->slots[index].dynamic;
	}
}

/*
 * Gives a new set the descriptors past the first of each binding of its
 * layout that is an array, in one allocation, and records where each
 * binding's are.
 */
static int
layout_set_elements(
	struct nvrtx_vk_descriptor_set *set,
	const struct nvrtx_vk_set_layout *layout)
{
	const struct nvrtx_vk_layout_binding *binding;
	uint32_t index;
	uint32_t total;

	/* Each binding's count, and where its second descriptor is (a number past the set's was refused). */
	total = 0U;
	for (index = 0U; index < layout->count; index++) {
		binding = &layout->bindings[index];
		if (binding->binding >= NVRTX_VK_BINDINGS_MAX)
			continue;
		set->count[binding->binding] = binding->count;
		set->first[binding->binding] = total;
		if (binding->count > 1U)
			total += binding->count - 1U;
	}

	/* The descriptors, none bound. */
	if (total == 0U)
		return 0;
	set->elements = kern_calloc(total, sizeof(*set->elements));
	if (set->elements == NULL)
		return ENOMEM;
	set->element_count = total;

	/* Succeeded: every element has its descriptor. */
	return 0;
}

/*
 * Finds the descriptor a write or a copy reaches from a binding's element:
 * the element of the binding, or past its last element the one as many on
 * in the next bindings of the set's layout (those of no descriptors passed
 * over); NULL past the set's last.
 *
 * Values referenced: the Vulkan 1.3 specification (VkWriteDescriptorSet
 * and VkCopyDescriptorSet: a descriptorCount past the binding's remaining
 * elements updates the next binding from its element 0, consecutive
 * binding updates).
 */
static struct nvrtx_vk_descriptor_slot *
layout_element(
	struct nvrtx_vk_descriptor_set *set,
	uint32_t binding,
	uint32_t element)
{
	/* The bindings from the one named, the element counted on through them. */
	while (binding < NVRTX_VK_BINDINGS_MAX) {
		if (element < set->count[binding])
			break;
		element -= set->count[binding];
		binding++;
	}

	/* Past the set's last binding there is none. */
	if (binding >= NVRTX_VK_BINDINGS_MAX)
		return NULL;

	/* The first descriptor, or one of the array's others. */
	if (element == 0U)
		return &set->slots[binding];

	/* Succeeded: the element's descriptor. */
	return &set->elements[set->first[binding] + element - 1U];
}

/*
 * Finds a set's descriptor of a binding and element: the binding's first,
 * or an element of its array; NULL outside the set's layout.
 */
const struct nvrtx_vk_descriptor_slot *
nvrtx_vk_descriptor_find(
	const struct nvrtx_vk_descriptor_set *set,
	uint32_t binding,
	uint32_t element)
{
	/* A binding of the set. */
	if (binding >= NVRTX_VK_BINDINGS_MAX)
		return NULL;

	/* The first descriptor, or one of the array's others. */
	if (element == 0U)
		return &set->slots[binding];
	if (element >= set->count[binding])
		return NULL;

	/* Succeeded: the element's descriptor. */
	return &set->elements[set->first[binding] + element - 1U];
}

/* Destroys a set nothing holds: its bindings let their views, samplers and buffers go. */
static int
layout_set_destroy(
	struct nvrtx_vk_session *session,
	void *payload)
{
	struct nvrtx_vk_descriptor_set *set;
	uint32_t binding;
	int first_error;
	int error;

	UNUSED_PARAMETER(session);

	set = payload;

	/* Every binding. */
	first_error = 0;
	for (binding = 0U; binding < NVRTX_VK_BINDINGS_MAX; binding++) {
		error = layout_slot_clear(&set->slots[binding]);
		if (error != 0 && first_error == 0)
			first_error = error;
	}

	/* Every other element of the arrays. */
	for (binding = 0U; binding < set->element_count; binding++) {
		error = layout_slot_clear(&set->elements[binding]);
		if (error != 0 && first_error == 0)
			first_error = error;
	}
	if (set->elements != NULL)
		kern_free(set->elements);

	kern_free(set);

	/* Reports a destruction that failed. */
	if (first_error != 0)
		return first_error;

	/* Succeeded: the set is gone. */
	return 0;
}
