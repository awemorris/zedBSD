/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's layouts: shader modules, descriptor set layouts,
 * pipeline layouts and caches, descriptor pools and sets and their
 * updates, render passes and framebuffers.
 *
 * A descriptor set keeps, for each binding, the objects its first
 * descriptor names (a buffer's range, an image view and its sampler),
 * held; the hardware's descriptors are written from them when a draw or
 * a dispatch runs (vk-queue.c, vk-draw.c).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/mali/vk/vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The most items of one array of a layout command. */
#define LAYOUT_ITEMS_MAX	256U

/* The largest SPIR-V module the executor keeps (4 MiB). */
#define SHADER_BYTES_MAX	(4U << 20)

static int create_shader(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_set_layout(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_pipeline_layout(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_cache(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_pool(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int pool_destroy(struct mali_vk_session *session, void *payload);
static int destroy_pool(struct mali_vk_session *session, struct i915_wire_reader *reader);
static int reset_pool(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void pool_sets_remove(struct mali_vk_session *session, const void *pool);
static int allocate_sets(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int set_destroy(struct mali_vk_session *session, void *payload);
static int free_sets(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int update_sets(struct mali_vk_session *session, struct i915_wire_reader *reader);
static int update_write(struct mali_vk_session *session, struct i915_wire_reader *reader);
static struct mali_vk_descriptor *set_slot(struct mali_vk_descriptor_set *set, uint32_t binding, uint32_t element);
static void slot_clear(struct mali_vk_descriptor *slot);
static void slot_hold(struct mali_vk_descriptor *slot, struct mali_vk_object *object, struct mali_vk_object **place);
static int create_render_pass(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_framebuffer(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int framebuffer_destroy(struct mali_vk_session *session, void *payload);

/* Runs the shader module, layout, pool, set, pass and framebuffer commands. */
int
mali_vk_layout_route(
	struct mali_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another handler. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_SHADER_MODULE:
		error = create_shader(session, reader, reply);
		break;
	case GPU_OP_DESTROY_SHADER_MODULE:
		error = mali_vk_destroy_plain(session, reader, I915_VK_OBJ_SHADER_MODULE);
		break;
	case GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT:
		error = create_set_layout(session, reader, reply);
		break;
	case GPU_OP_DESTROY_DESCRIPTOR_SET_LAYOUT:
		error = mali_vk_destroy_plain(session, reader, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT);
		break;
	case GPU_OP_CREATE_PIPELINE_LAYOUT:
		error = create_pipeline_layout(session, reader, reply);
		break;
	case GPU_OP_DESTROY_PIPELINE_LAYOUT:
		error = mali_vk_destroy_plain(session, reader, I915_VK_OBJ_PIPELINE_LAYOUT);
		break;
	case GPU_OP_CREATE_PIPELINE_CACHE:
		error = create_cache(session, reader, reply);
		break;
	case GPU_OP_DESTROY_PIPELINE_CACHE:
		error = mali_vk_destroy_plain(session, reader, I915_VK_OBJ_PIPELINE);
		break;
	case GPU_OP_CREATE_DESCRIPTOR_POOL:
		error = create_pool(session, reader, reply);
		break;
	case GPU_OP_DESTROY_DESCRIPTOR_POOL:
		error = destroy_pool(session, reader);
		break;
	case GPU_OP_RESET_DESCRIPTOR_POOL:
		error = reset_pool(session, reader, reply);
		break;
	case GPU_OP_ALLOCATE_DESCRIPTOR_SETS:
		error = allocate_sets(session, reader, reply);
		break;
	case GPU_OP_FREE_DESCRIPTOR_SETS:
		error = free_sets(session, reader, reply);
		break;
	case GPU_OP_UPDATE_DESCRIPTOR_SETS:
		error = update_sets(session, reader);
		break;
	case GPU_OP_CREATE_RENDER_PASS:
		error = create_render_pass(session, reader, reply);
		break;
	case GPU_OP_DESTROY_RENDER_PASS:
		error = mali_vk_destroy_plain(session, reader, I915_VK_OBJ_RENDER_PASS);
		break;
	case GPU_OP_CREATE_FRAMEBUFFER:
		error = create_framebuffer(session, reader, reply);
		break;
	case GPU_OP_DESTROY_FRAMEBUFFER:
		error = mali_vk_destroy_plain(session, reader, I915_VK_OBJ_FRAMEBUFFER);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/* vkCreateShaderModule: the module's words are kept for the pipelines that compile them. */
static int
create_shader(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkShaderModuleCreateInfo info;
	struct mali_vk_shader *shader;
	uint64_t present;
	uint64_t created;
	int error;

	/* The device, the info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkShaderModuleCreateInfo(reader, &session->arena, &info);
	created = mali_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Whole words within the bound. */
	error = 0;
	shader = NULL;
	if (info.pCode == NULL || info.codeSize == 0U || (info.codeSize & 3U) != 0U || info.codeSize > SHADER_BYTES_MAX)
		error = EINVAL;

	/* The words after the structure. */
	if (error == 0) {
		shader = kern_malloc(sizeof(*shader) + info.codeSize);
		if (shader != NULL) {
			shader->word_count = (uint32_t)(info.codeSize / 4U);
			shader->words = (uint32_t *)(void *)(shader + 1);
			kern_memcpy(shader->words, info.pCode, info.codeSize);
		}
	}

	/* Succeeded: the reply publishes it. */
	mali_vk_create_reply(session, reply, I915_VK_OBJ_SHADER_MODULE, created, shader, mali_vk_free_payload, error);
	return 0;
}

/* vkCreateDescriptorSetLayout: the bindings, each of one descriptor of a taken type. */
static int
create_set_layout(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDescriptorSetLayoutCreateInfo info;
	struct mali_vk_set_layout *layout;
	const VkDescriptorSetLayoutBinding *binding;
	uint64_t present;
	uint64_t created;
	uint32_t index;
	int error;

	/* The device, the info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkDescriptorSetLayoutCreateInfo(reader, &session->arena, &info);
	created = mali_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The bindings the executor takes. */
	error = 0;
	layout = NULL;
	if (info.bindingCount > MALI_VK_BINDINGS_MAX)
		error = ENOTSUP;
	for (index = 0; error == 0 && index < info.bindingCount; index++) {
		binding = &info.pBindings[index];
		if (binding->descriptorCount > 1U) {
			error = ENOTSUP;
		} else if (binding->descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
			   binding->descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
			   binding->descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
			   binding->descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
			error = ENOTSUP;
		}
	}

	/* The payload. */
	if (error == 0) {
		layout = kern_calloc(1, sizeof(*layout));
		if (layout != NULL) {
			layout->count = info.bindingCount;
			for (index = 0; index < info.bindingCount; index++) {
				layout->bindings[index].binding = info.pBindings[index].binding;
				layout->bindings[index].type = info.pBindings[index].descriptorType;
				layout->bindings[index].count = info.pBindings[index].descriptorCount;
			}
		}
	}

	/* Succeeded: the reply publishes it. */
	mali_vk_create_reply(session, reply, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT, created, layout, mali_vk_free_payload, error);
	return 0;
}

/* vkCreatePipelineLayout: only the set count matters; the push constants fit the bound. */
static int
create_pipeline_layout(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPipelineLayoutCreateInfo info;
	struct mali_vk_pipeline_layout *layout;
	uint64_t present;
	uint64_t created;
	uint32_t index;
	int error;

	/* The device, the info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkPipelineLayoutCreateInfo(reader, &session->arena, &info);
	created = mali_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Sets and push constants within the bounds. */
	error = 0;
	layout = NULL;
	if (info.setLayoutCount > MALI_VK_SETS_MAX)
		error = ENOTSUP;
	for (index = 0; error == 0 && index < info.pushConstantRangeCount; index++) {
		if (info.pPushConstantRanges[index].offset + info.pPushConstantRanges[index].size > MALI_VK_PUSH_BYTES)
			error = ENOTSUP;
	}

	if (error == 0) {
		layout = kern_calloc(1, sizeof(*layout));
		if (layout != NULL)
			layout->set_count = info.setLayoutCount;
	}

	/* Succeeded: the reply publishes it. */
	mali_vk_create_reply(session, reply, I915_VK_OBJ_PIPELINE_LAYOUT, created, layout, mali_vk_free_payload, error);
	return 0;
}

/* vkCreatePipelineCache: an empty cache, kept under the pipeline kind (nothing compares the two). */
static int
create_cache(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPipelineCacheCreateInfo info;
	struct mali_vk_pipeline *cache;
	uint64_t present;
	uint64_t created;

	/* The device, the info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkPipelineCacheCreateInfo(reader, &session->arena, &info);
	created = mali_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* An empty pipeline record stands for it. */
	cache = kern_calloc(1, sizeof(*cache));

	/* Succeeded: the reply publishes it. */
	mali_vk_create_reply(session, reply, I915_VK_OBJ_PIPELINE, created, cache, mali_vk_free_payload, 0);
	return 0;
}

/* vkCreateDescriptorPool: only its set count is kept. */
static int
create_pool(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDescriptorPoolCreateInfo info;
	struct mali_vk_descriptor_pool *pool;
	uint64_t present;
	uint64_t created;

	/* The device, the info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkDescriptorPoolCreateInfo(reader, &session->arena, &info);
	created = mali_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The payload. */
	pool = kern_calloc(1, sizeof(*pool));
	if (pool != NULL)
		pool->max_sets = info.maxSets;

	/* Succeeded: the reply publishes it. */
	mali_vk_create_reply(session, reply, I915_VK_OBJ_DESCRIPTOR_POOL, created, pool, pool_destroy, 0);
	return 0;
}

/* Destroys a pool's payload; its sets went before. */
static int
pool_destroy(
	struct mali_vk_session *session,
	void *payload)
{
	(void)session;
	kern_free(payload);
	return 0;
}

/* vkDestroyDescriptorPool: the destroy form; every set of the pool goes with it. */
static int
destroy_pool(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader)
{
	uint64_t gone;
	void *pool;

	/* The pool. */
	(void)drv_i915_wire_read_u64(reader);
	gone = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Its sets, then itself. */
	pool = mali_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_POOL, gone);
	if (pool != NULL)
		pool_sets_remove(session, pool);
	(void)mali_vk_object_remove(session, I915_VK_OBJ_DESCRIPTOR_POOL, gone);
	return 0;
}

/* vkResetDescriptorPool: [device][pool][flags] -> [result]; every set of the pool goes. */
static int
reset_pool(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t named;
	void *pool;

	/* The pool and the flags. */
	(void)drv_i915_wire_read_u64(reader);
	named = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Its sets. */
	pool = mali_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_POOL, named);
	if (pool != NULL)
		pool_sets_remove(session, pool);

	/* Succeeded: the result. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	return 0;
}

/* Removes every published set of a pool. */
static void
pool_sets_remove(
	struct mali_vk_session *session,
	const void *pool)
{
	struct mali_vk_object *object;
	struct mali_vk_descriptor_set *set;
	uint64_t gone;
	bool found;

	/* One set at a time, as each removal changes the list. */
	do {
		found = false;
		for (object = session->objects; object != NULL; object = object->next) {
			if (object->kind != I915_VK_OBJ_DESCRIPTOR_SET)
				continue;
			set = object->payload;
			if (set->pool != pool)
				continue;
			gone = object->identity;
			found = true;
			break;
		}

		if (found)
			(void)mali_vk_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, gone);
	} while (found);
}

/*
 * vkAllocateDescriptorSets: [device][present][info][count][sets] ->
 * [result][count][sets].
 */
static int
allocate_sets(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDescriptorSetAllocateInfo info;
	struct mali_vk_descriptor_set *set;
	struct mali_vk_set_layout *layout;
	void *pool;
	uint64_t *identities;
	uint64_t present;
	uint64_t count;
	uint64_t index;
	int error;

	/* The device, the info and the identities. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkDescriptorSetAllocateInfo(reader, &session->arena, &info);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > LAYOUT_ITEMS_MAX || count != info.descriptorSetCount)
		return EINVAL;
	identities = mali_vk_arena_take(session, (size_t)count * sizeof(uint64_t));
	if (count != 0U && identities == NULL)
		return EINVAL;
	for (index = 0; index < count; index++)
		identities[index] = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Each set, of its layout, from the pool. */
	pool = mali_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_POOL, mali_vk_handle(info.descriptorPool));
	error = 0;
	if (pool == NULL)
		error = EINVAL;
	for (index = 0; error == 0 && index < count; index++) {
		layout = mali_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT, mali_vk_handle(info.pSetLayouts[index]));
		if (layout == NULL) {
			error = EINVAL;
			break;
		}

		set = kern_calloc(1, sizeof(*set));
		if (set == NULL) {
			error = ENOMEM;
			break;
		}

		set->pool = pool;
		set->layout = *layout;
		error = mali_vk_object_publish(session, I915_VK_OBJ_DESCRIPTOR_SET, identities[index], set, set_destroy);
		if (error != 0)
			kern_free(set);
	}

	/* A failure takes back the sets made before it. */
	if (error != 0) {
		while (index > 0U) {
			index--;
			(void)mali_vk_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, identities[index]);
		}
		drv_i915_wire_reply_u32(reply, mali_vk_result(error));
		drv_i915_wire_reply_u64(reply, 0);
		return 0;
	}

	/* Succeeded: the identities echoed. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, count);
	for (index = 0; index < count; index++)
		drv_i915_wire_reply_u64(reply, identities[index]);
	return 0;
}

/* Destroys a set: its holds on its descriptors' objects go. */
static int
set_destroy(
	struct mali_vk_session *session,
	void *payload)
{
	struct mali_vk_descriptor_set *set;
	uint32_t index;

	(void)session;
	set = payload;
	for (index = 0; index < MALI_VK_BINDINGS_MAX; index++)
		slot_clear(&set->slots[index]);
	kern_free(set);
	return 0;
}

/* vkFreeDescriptorSets: [device][pool][count][count][sets] -> [result]. */
static int
free_sets(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t count;
	uint64_t index;
	uint64_t gone;

	/* The device, the pool and the counts. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > LAYOUT_ITEMS_MAX)
		return EINVAL;

	/* Each set. */
	for (index = 0; index < count; index++) {
		gone = drv_i915_wire_read_u64(reader);
		(void)mali_vk_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, gone);
	}

	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the result. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	return 0;
}

/* vkUpdateDescriptorSets: [device][count][count][writes][count][count][copies]; copies are not taken. */
static int
update_sets(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader)
{
	VkCopyDescriptorSet copy;
	uint64_t writes;
	uint64_t copies;
	uint64_t index;
	int error;

	/* The writes. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	writes = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || writes > LAYOUT_ITEMS_MAX)
		return EINVAL;
	for (index = 0; index < writes; index++) {
		error = update_write(session, reader);
		if (error != 0)
			return error;
	}

	/* The copies, read to their end. */
	(void)drv_i915_wire_read_u32(reader);
	copies = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || copies > LAYOUT_ITEMS_MAX)
		return EINVAL;
	for (index = 0; index < copies; index++)
		i915_vkc_dec_VkCopyDescriptorSet(reader, &session->arena, &copy);
	if (reader->error != 0)
		return EINVAL;
	if (copies != 0U)
		return ENOTSUP;

	/* Succeeded: the sets are written. */
	return 0;
}

/*
 * One VkWriteDescriptorSet: [sType][chain][set][binding][element][count]
 * [type], its images [count][sampler][view][layout]..., its buffers
 * [count][buffer info]..., its texel views [count][view]....
 */
static int
update_write(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader)
{
	VkDescriptorBufferInfo info;
	struct mali_vk_descriptor_set *set;
	struct mali_vk_descriptor *slot;
	uint64_t sampler;
	uint64_t view;
	uint64_t set_identity;
	uint64_t count;
	uint64_t item;
	uint32_t binding;
	uint32_t element;
	uint32_t type;

	/* The head. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	set_identity = drv_i915_wire_read_u64(reader);
	binding = drv_i915_wire_read_u32(reader);
	element = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > LAYOUT_ITEMS_MAX)
		return EINVAL;
	set = mali_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_SET, set_identity);

	/* The images: a view with its sampler. */
	for (item = 0; item < count; item++) {
		sampler = drv_i915_wire_read_u64(reader);
		view = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;
		slot = NULL;
		if (set != NULL)
			slot = set_slot(set, binding, element + (uint32_t)item);
		if (slot != NULL) {
			slot_clear(slot);
			slot->type = type;
			slot_hold(slot, mali_vk_object_find(session, I915_VK_OBJ_IMAGE_VIEW, view), &slot->view);
			slot_hold(slot, mali_vk_object_find(session, I915_VK_OBJ_SAMPLER, sampler), &slot->sampler);
		}
	}

	/* The buffers: a range. */
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > LAYOUT_ITEMS_MAX)
		return EINVAL;
	for (item = 0; item < count; item++) {
		kern_memset(&info, 0, sizeof(info));
		i915_vkc_dec_VkDescriptorBufferInfo(reader, &session->arena, &info);
		if (reader->error != 0)
			return EINVAL;
		slot = NULL;
		if (set != NULL)
			slot = set_slot(set, binding, element + (uint32_t)item);
		if (slot != NULL) {
			slot_clear(slot);
			slot->type = type;
			slot_hold(slot, mali_vk_object_find(session, I915_VK_OBJ_BUFFER, mali_vk_handle(info.buffer)), &slot->buffer);
			slot->offset = info.offset;
			slot->range = info.range;
		}
	}

	/* The texel buffer views, read and not taken. */
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > LAYOUT_ITEMS_MAX)
		return EINVAL;
	for (item = 0; item < count; item++)
		(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the write is applied. */
	return 0;
}

/* Finds the slot of a binding's first descriptor in a set, or NULL. */
static struct mali_vk_descriptor *
set_slot(
	struct mali_vk_descriptor_set *set,
	uint32_t binding,
	uint32_t element)
{
	uint32_t index;

	/* Only element 0 exists (every binding is of one descriptor). */
	if (element != 0U)
		return NULL;

	/* The binding's place in the layout. */
	for (index = 0; index < set->layout.count; index++) {
		if (set->layout.bindings[index].binding == binding)
			return &set->slots[index];
	}

	/* No such binding. */
	return NULL;
}

/* Lets go of what a slot holds. */
static void
slot_clear(
	struct mali_vk_descriptor *slot)
{
	mali_vk_object_release(slot->buffer);
	mali_vk_object_release(slot->view);
	mali_vk_object_release(slot->sampler);
	slot->buffer = NULL;
	slot->view = NULL;
	slot->sampler = NULL;
	slot->offset = 0;
	slot->range = 0;
}

/* Holds an object in a place of a slot; a missing object leaves the place empty. */
static void
slot_hold(
	struct mali_vk_descriptor *slot,
	struct mali_vk_object *object,
	struct mali_vk_object **place)
{
	int error;

	/* Nothing to hold. */
	(void)slot;
	*place = NULL;
	if (object == NULL)
		return;

	/* The hold. */
	error = mali_vk_object_retain(object);
	if (error == 0)
		*place = object;
}

/*
 * vkCreateRenderPass: one subpass of colour attachments of taken formats
 * and at most one depth and stencil attachment the tile buffer takes.
 */
static int
create_render_pass(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkRenderPassCreateInfo info;
	struct mali_vk_render_pass *pass;
	const VkSubpassDescription *subpass;
	uint64_t present;
	uint64_t created;
	uint32_t index;
	uint32_t attachment;
	int error;

	/* The device, the info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkRenderPassCreateInfo(reader, &session->arena, &info);
	created = mali_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* One subpass, within the bounds, without input attachments. */
	error = 0;
	pass = NULL;
	if (info.subpassCount != 1U || info.attachmentCount > MALI_VK_ATTACHMENTS_MAX) {
		error = ENOTSUP;
	} else {
		subpass = &info.pSubpasses[0];
		if (subpass->colorAttachmentCount > MALI_VK_ATTACHMENTS_MAX || subpass->inputAttachmentCount != 0U)
			error = ENOTSUP;
	}

	/* The payload. */
	if (error == 0) {
		pass = kern_calloc(1, sizeof(*pass));
		if (pass == NULL)
			error = ENOMEM;
	}

	if (error == 0) {
		pass->attachment_count = info.attachmentCount;
		for (index = 0; index < info.attachmentCount; index++) {
			pass->format[index] = mali_vk_format_find(info.pAttachments[index].format);
			pass->load_op[index] = info.pAttachments[index].loadOp;
			pass->store_op[index] = info.pAttachments[index].storeOp;
			pass->stencil_load_op[index] = info.pAttachments[index].stencilLoadOp;
			pass->stencil_store_op[index] = info.pAttachments[index].stencilStoreOp;
			if (pass->format[index] == NULL)
				error = ENOTSUP;
			else if (!pass->format[index]->render && mali_surface_zs_find(pass->format[index]->vk) == NULL)
				error = ENOTSUP;
		}

		/* The colour references; a pass of none draws to one target without an attachment. */
		subpass = &info.pSubpasses[0];
		pass->color[0] = VK_ATTACHMENT_UNUSED;
		pass->color_count = subpass->colorAttachmentCount;
		for (index = 0; index < subpass->colorAttachmentCount; index++) {
			attachment = subpass->pColorAttachments[index].attachment;
			if (attachment != VK_ATTACHMENT_UNUSED && attachment >= info.attachmentCount)
				error = EINVAL;
			else if (attachment != VK_ATTACHMENT_UNUSED && pass->format[attachment] != NULL && !pass->format[attachment]->render)
				error = ENOTSUP;
			pass->color[index] = attachment;
		}

		/* The depth and stencil attachment, of a format the tile buffer takes. */
		pass->depth = VK_ATTACHMENT_UNUSED;
		if (subpass->pDepthStencilAttachment != NULL)
			pass->depth = subpass->pDepthStencilAttachment->attachment;
		if (pass->depth != VK_ATTACHMENT_UNUSED && pass->depth >= info.attachmentCount) {
			error = EINVAL;
		} else if (pass->depth != VK_ATTACHMENT_UNUSED && pass->format[pass->depth] != NULL) {
			pass->zs_format = mali_surface_zs_find(pass->format[pass->depth]->vk);
			if (pass->zs_format == NULL)
				error = ENOTSUP;
		}

		if (error != 0) {
			kern_free(pass);
			pass = NULL;
		}
	}

	/* Succeeded: the reply publishes it. */
	mali_vk_create_reply(session, reply, I915_VK_OBJ_RENDER_PASS, created, pass, mali_vk_free_payload, error);
	return 0;
}

/* vkCreateFramebuffer: its views, held, and its extent. */
static int
create_framebuffer(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkFramebufferCreateInfo info;
	struct mali_vk_framebuffer *framebuffer;
	struct mali_vk_object *view;
	uint64_t present;
	uint64_t created;
	uint32_t index;
	int error;

	/* The device, the info and the tail. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkFramebufferCreateInfo(reader, &session->arena, &info);
	created = mali_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The payload. */
	error = 0;
	framebuffer = NULL;
	if (info.attachmentCount > MALI_VK_ATTACHMENTS_MAX || info.width == 0U || info.height == 0U)
		error = ENOTSUP;
	if (error == 0) {
		framebuffer = kern_calloc(1, sizeof(*framebuffer));
		if (framebuffer == NULL)
			error = ENOMEM;
	}

	/* Each view, held. */
	if (error == 0) {
		framebuffer->width = info.width;
		framebuffer->height = info.height;
		for (index = 0; index < info.attachmentCount; index++) {
			view = mali_vk_object_find(session, I915_VK_OBJ_IMAGE_VIEW, mali_vk_handle(info.pAttachments[index]));
			if (view == NULL || mali_vk_object_retain(view) != 0) {
				error = EINVAL;
				break;
			}
			framebuffer->views[index] = view;
			framebuffer->count++;
		}

		if (error != 0) {
			(void)framebuffer_destroy(session, framebuffer);
			framebuffer = NULL;
		}
	}

	/* Succeeded: the reply publishes it. */
	mali_vk_create_reply(session, reply, I915_VK_OBJ_FRAMEBUFFER, created, framebuffer, framebuffer_destroy, error);
	return 0;
}

/* Destroys a framebuffer: its holds on its views go. */
static int
framebuffer_destroy(
	struct mali_vk_session *session,
	void *payload)
{
	struct mali_vk_framebuffer *framebuffer;
	uint32_t index;

	(void)session;
	framebuffer = payload;
	for (index = 0; index < framebuffer->count; index++)
		mali_vk_object_release(framebuffer->views[index]);
	kern_free(framebuffer);
	return 0;
}
