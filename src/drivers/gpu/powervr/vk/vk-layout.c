/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's shader modules, layouts, descriptors and compute
 * pipelines.
 *
 * A shader module keeps its SPIR-V and is read once, when it is created,
 * by the shader compiler's reader; a module without a compute entry the
 * compiler takes is kept with the reason, and a compute pipeline made
 * from it is refused.  A pipeline keeps its own copy of the SPIR-V and
 * is compiled for each workgroup when it is dispatched.  A descriptor set binds
 * storage buffers; other descriptors are read and refused when a
 * pipeline would need them.
 *
 * Procedure referenced: zedBSD src/drivers/gpu/bcm2711/vk/vulkan-layout.c,
 * vulkan-descriptor-sets.c, vulkan-descriptor-update.c and
 * vulkan-pipeline-decode.c (the wire of each command).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/powervr/vk/vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The largest module, the sets and command items one call carries, and the pipelines one create makes. */
#define LAYOUT_SHADER_BYTES		(64U * 1024U)
#define LAYOUT_ITEMS_MAX		64U
#define LAYOUT_PIPELINES_MAX		8U

static int create_shader(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_set_layout(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_pipeline_layout(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int create_cache(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy_cache(struct i915_wire_reader *reader);
static int create_pool(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy_pool(struct powervr_vk_session *session, struct i915_wire_reader *reader);
static int reset_pool(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int pool_sets_free(struct powervr_vk_session *session, const void *pool);
static int allocate_sets(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int free_sets(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int update_sets(struct powervr_vk_session *session, struct i915_wire_reader *reader);
static int update_write(struct powervr_vk_session *session, struct i915_wire_reader *reader);
static int slot_buffer(struct powervr_vk_descriptor_slot *slot, struct powervr_vk_object *buffer, uint64_t offset, uint64_t range);
static void set_buffer(struct powervr_vk_session *session, struct powervr_vk_descriptor_set *set, uint32_t binding, uint32_t element, const VkDescriptorBufferInfo *info);
static void set_image(struct powervr_vk_session *session, struct powervr_vk_descriptor_set *set, uint32_t binding, uint32_t element, uint64_t sampler_id, uint64_t view_id);
static int set_destroy(struct powervr_vk_session *session, void *payload);
static int create_pipelines(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply, bool graphics);
static int decode_pipeline(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct powervr_vk_pipeline *pipeline);
static int pipeline_destroy(struct powervr_vk_session *session, void *payload);

/*
 * Runs a shader module, layout, descriptor or pipeline command; *handled
 * stays clear for another handler's opcode.
 */
int
powervr_vk_layout_route(
	struct powervr_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another handler. */
	(void)requested;
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_SHADER_MODULE:
		error = create_shader(session, reader, reply);
		break;
	case GPU_OP_DESTROY_SHADER_MODULE:
		error = powervr_vk_destroy_plain(session, reader, I915_VK_OBJ_SHADER_MODULE);
		break;
	case GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT:
		error = create_set_layout(session, reader, reply);
		break;
	case GPU_OP_DESTROY_DESCRIPTOR_SET_LAYOUT:
		error = powervr_vk_destroy_plain(session, reader, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT);
		break;
	case GPU_OP_CREATE_PIPELINE_LAYOUT:
		error = create_pipeline_layout(session, reader, reply);
		break;
	case GPU_OP_DESTROY_PIPELINE_LAYOUT:
		error = powervr_vk_destroy_plain(session, reader, I915_VK_OBJ_PIPELINE_LAYOUT);
		break;
	case GPU_OP_CREATE_PIPELINE_CACHE:
		error = create_cache(session, reader, reply);
		break;
	case GPU_OP_DESTROY_PIPELINE_CACHE:
		error = destroy_cache(reader);
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
	case GPU_OP_CREATE_COMPUTE_PIPELINES:
		error = create_pipelines(session, reader, reply, false);
		break;
	case GPU_OP_CREATE_GRAPHICS_PIPELINES:
		error = create_pipelines(session, reader, reply, true);
		break;
	case GPU_OP_DESTROY_PIPELINE:
		error = powervr_vk_destroy_plain(session, reader, I915_VK_OBJ_PIPELINE);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran and wrote its reply. */
	return 0;
}

/*
 * vkCreateShaderModule: [device][present][VkShaderModuleCreateInfo] and
 * the create's tail -> the create's reply.  The module keeps what its
 * SPIR-V does, or why it is outside the subset.
 */
static int
create_shader(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkShaderModuleCreateInfo info;
	struct powervr_shader_workspace *workspace;
	struct powervr_shader_program *program;
	struct powervr_ir_shader *ir;
	struct powervr_vk_shader *shader;
	uint64_t identity;
	uint32_t index;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkShaderModuleCreateInfo(reader, &session->arena, &info);
	identity = powervr_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Whole words of code, some of them, within the largest module. */
	error = 0;
	if (info.pCode == NULL || info.codeSize == 0 || (info.codeSize & 3U) != 0) {
		error = EINVAL;
	} else if (info.codeSize > LAYOUT_SHADER_BYTES) {
		error = ENOTSUP;
	}

	/* The module, its words after it in the same allocation. */
	shader = NULL;
	if (error == 0) {
		shader = kern_calloc(1, sizeof(*shader) + info.codeSize);
		if (shader == NULL)
			error = ENOMEM;
	}
	if (error == 0) {
		shader->words = (uint32_t *)(void *)(shader + 1);
		shader->word_count = (uint32_t)(info.codeSize / 4U);
		for (index = 0; index < shader->word_count; index++)
			shader->words[index] = info.pCode[index];
	}

	/* Whether the compiler reads a compute entry from it, in a workspace of its own. */
	if (error == 0) {
		shader->status = ENOMEM;
		workspace = kern_malloc(powervr_shader_workspace_size());
		program = kern_malloc(sizeof(*program));
		if (workspace != NULL && program != NULL)
			shader->status = powervr_shader_read(shader->words, shader->word_count, POWERVR_SHADER_COMPUTE, NULL, workspace, program, &ir);
		kern_free(program);
		kern_free(workspace);
		/* Only a module the compiler cannot read at all is worth a line; a graphics module has no compute entry. */
		if (shader->status != 0 && shader->status != ENOTSUP)
			kern_logf("powervr: vk: a shader module of %u words has no compute entry the compiler takes (%d)\n", shader->word_count, shader->status);
	}

	/* Publishes the module and answers. */
	powervr_vk_create_reply(session, reply, I915_VK_OBJ_SHADER_MODULE, identity, shader, powervr_vk_free_payload, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * vkCreateDescriptorSetLayout: [device][present]
 * [VkDescriptorSetLayoutCreateInfo] and the create's tail -> the
 * create's reply.  Binding numbers past a set's are refused.
 */
static int
create_set_layout(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDescriptorSetLayoutCreateInfo info;
	struct powervr_set_binding_info bindings[POWERVR_SET_BINDINGS_MAX];
	struct powervr_vk_set_layout *layout;
	uint64_t identity;
	uint32_t index;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkDescriptorSetLayoutCreateInfo(reader, &session->arena, &info);
	identity = powervr_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* No binding number past a set's. */
	error = 0;
	if (info.bindingCount != 0 && info.pBindings == NULL)
		error = EINVAL;
	if (info.bindingCount > POWERVR_SET_BINDINGS_MAX)
		error = ENOTSUP;
	for (index = 0; error == 0 && index < info.bindingCount; index++) {
		if (info.pBindings[index].binding >= POWERVR_VK_BINDINGS_MAX)
			error = ENOTSUP;
		bindings[index].binding = info.pBindings[index].binding;
		bindings[index].type = info.pBindings[index].descriptorType;
		bindings[index].count = info.pBindings[index].descriptorCount;
	}

	/* The layout keeps its count and the bindings' places in a set's memory. */
	layout = NULL;
	if (error == 0) {
		layout = kern_calloc(1, sizeof(*layout));
		if (layout == NULL)
			error = ENOMEM;
	}
	if (error == 0) {
		layout->count = info.bindingCount;
		error = powervr_set_layout_build(bindings, info.bindingCount, &layout->layout);
		if (error != 0) {
			kern_free(layout);
			layout = NULL;
		}
	}

	/* Publishes the layout and answers. */
	powervr_vk_create_reply(session, reply, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT, identity, layout, powervr_vk_free_payload, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * vkCreatePipelineLayout: [device][present][VkPipelineLayoutCreateInfo]
 * and the create's tail -> the create's reply.
 */
static int
create_pipeline_layout(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPipelineLayoutCreateInfo info;
	struct powervr_vk_pipeline_layout *layout;
	uint64_t identity;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkPipelineLayoutCreateInfo(reader, &session->arena, &info);
	identity = powervr_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* No more sets than a pipeline binds; push constants are not read by the subset. */
	error = 0;
	if (info.setLayoutCount > POWERVR_VK_SETS_MAX)
		error = ENOTSUP;

	/* The layout. */
	layout = NULL;
	if (error == 0) {
		layout = kern_calloc(1, sizeof(*layout));
		if (layout != NULL)
			layout->set_count = info.setLayoutCount;
	}

	/* Publishes the layout and answers. */
	powervr_vk_create_reply(session, reply, I915_VK_OBJ_PIPELINE_LAYOUT, identity, layout, powervr_vk_free_payload, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * vkCreatePipelineCache: [device][present][VkPipelineCacheCreateInfo] and
 * the create's tail -> the create's reply.  No cache is kept: the
 * identity is answered and not recorded.
 */
static int
create_cache(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPipelineCacheCreateInfo info;
	uint64_t identity;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkPipelineCacheCreateInfo(reader, &session->arena, &info);
	identity = powervr_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Success with the identity. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/* vkDestroyPipelineCache: [device][identity][allocator], no reply body; nothing was kept. */
static int
destroy_cache(
	struct i915_wire_reader *reader)
{
	/* The device, the identity and the allocator. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: there is nothing to destroy. */
	return 0;
}

/* vkCreateDescriptorPool: [device][present][VkDescriptorPoolCreateInfo] and the create's tail -> the create's reply. */
static int
create_pool(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDescriptorPoolCreateInfo info;
	struct powervr_vk_descriptor_pool *pool;
	uint64_t identity;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkDescriptorPoolCreateInfo(reader, &session->arena, &info);
	identity = powervr_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The pool keeps its most sets. */
	pool = kern_calloc(1, sizeof(*pool));
	if (pool != NULL)
		pool->max_sets = info.maxSets;

	/* Publishes the pool and answers. */
	powervr_vk_create_reply(session, reply, I915_VK_OBJ_DESCRIPTOR_POOL, identity, pool, powervr_vk_free_payload, 0);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/* vkDestroyDescriptorPool: [device][identity][allocator], no reply body.  The pool's sets go with it. */
static int
destroy_pool(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader)
{
	const struct powervr_vk_descriptor_pool *pool;
	uint64_t identity;
	int error;

	/* The identity between the device and the allocator. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* An unknown pool is already gone. */
	pool = powervr_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_POOL, identity);
	if (pool == NULL)
		return 0;

	/* The pool's sets, then the pool. */
	error = pool_sets_free(session, pool);
	if (error == 0)
		error = powervr_vk_object_remove(session, I915_VK_OBJ_DESCRIPTOR_POOL, identity);
	if (error != 0)
		return error;

	/* Succeeded: the pool and its sets are gone. */
	return 0;
}

/* vkResetDescriptorPool: [device][identity][flags] -> [result].  The pool's sets go; the pool stays. */
static int
reset_pool(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	const struct powervr_vk_descriptor_pool *pool;
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
	pool = powervr_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_POOL, identity);
	if (pool != NULL)
		error = pool_sets_free(session, pool);

	/* The reset's result. */
	result = powervr_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the reply carries the result. */
	return 0;
}

/* Removes every published set of a pool; the walk starts over after each removal. */
static int
pool_sets_free(
	struct powervr_vk_session *session,
	const void *pool)
{
	struct powervr_vk_object *object;
	const struct powervr_vk_descriptor_set *set;
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
			error = powervr_vk_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, object->identity);
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
 * [count][identities] -> [result][count][identities].  All of them or
 * none.
 */
static int
allocate_sets(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDescriptorSetAllocateInfo info;
	uint64_t identities[LAYOUT_ITEMS_MAX];
	const struct powervr_vk_descriptor_pool *pool;
	const struct powervr_vk_set_layout *layout;
	struct powervr_vk_descriptor_set *set;
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
	if (reader->error != 0 || count > LAYOUT_ITEMS_MAX || count != info.descriptorSetCount)
		return EINVAL;

	/* The identity libvulkan chose for each set. */
	for (index = 0; index < count; index++)
		identities[index] = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Sets of a known pool. */
	error = 0;
	pool = powervr_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_POOL, (uint64_t)(uintptr_t)info.descriptorPool);
	if (pool == NULL)
		error = EINVAL;
	if (error == 0 && count != 0 && info.pSetLayouts == NULL)
		error = EINVAL;

	/* Publishes each set with its layout's memory, up to the first failure. */
	published = 0;
	for (index = 0; error == 0 && index < count; index++) {
		layout = powervr_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT, (uint64_t)(uintptr_t)info.pSetLayouts[index]);
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
		set->layout = layout->layout;
		if (layout->layout.bytes != 0) {
			set->bytes = kern_calloc(1, layout->layout.bytes);
			if (set->bytes == NULL) {
				kern_free(set);
				error = ENOMEM;
				break;
			}
		}

		error = powervr_vk_object_publish(session, I915_VK_OBJ_DESCRIPTOR_SET, identities[index], set, set_destroy);
		if (error != 0) {
			kern_free(set->bytes);
			kern_free(set);
			break;
		}

		published++;
	}

	/* A batch that failed part-way leaves no set. */
	if (error != 0) {
		for (index = 0; index < published; index++)
			(void)powervr_vk_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, identities[index]);
	}

	/* The result, and on success every set's identity. */
	result = powervr_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);
	if (error == 0) {
		drv_i915_wire_reply_u64(reply, count);
		for (index = 0; index < count; index++)
			drv_i915_wire_reply_u64(reply, identities[index]);
	}

	/* Succeeded: the reply carries the allocation's result. */
	return 0;
}

/* vkFreeDescriptorSets: [device][pool][count][count]{identity} -> [result]. */
static int
free_sets(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t count;
	uint64_t index;
	uint64_t identity;
	uint32_t result;
	int failed;
	int error;

	/* The count behind the device, the pool and the count's first form. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > LAYOUT_ITEMS_MAX)
		return EINVAL;

	/* Removes each set; an unknown one is already gone. */
	failed = 0;
	for (index = 0; index < count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			return EINVAL;
		error = powervr_vk_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, identity);
		if (error != 0 && error != ENOENT && failed == 0)
			failed = error;
	}

	/* The free's result. */
	result = powervr_vk_result(failed);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the reply carries the result. */
	return 0;
}

/*
 * vkUpdateDescriptorSets: [device][n][n]{write}[m][m]{copy}, no reply
 * body.  Copies are read and refused.
 */
static int
update_sets(
	struct powervr_vk_session *session,
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
	if (reader->error != 0 || writes > LAYOUT_ITEMS_MAX)
		return EINVAL;

	/* Applies each write. */
	for (index = 0; index < writes; index++) {
		error = update_write(session, reader);
		if (error != 0)
			return error;
	}

	/* The copies, read. */
	(void)drv_i915_wire_read_u32(reader);
	copies = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || copies > LAYOUT_ITEMS_MAX)
		return EINVAL;
	for (index = 0; index < copies; index++)
		i915_vkc_dec_VkCopyDescriptorSet(reader, &session->arena, &copy);
	if (reader->error != 0)
		return EINVAL;

	/* A copy is not applied. */
	if (copies != 0)
		return ENOTSUP;

	/* Succeeded: the writes are applied. */
	return 0;
}

/*
 * Reads one descriptor write, writes its descriptors into the set's
 * memory and binds its storage buffers for the compute path:
 * [sType][pNext][set][binding][element][count][type][images]
 * {[sampler][view][layout]}[buffers]{VkDescriptorBufferInfo}[texel
 * views]{identity}.  Element 0 of each binding is the one the compute
 * path binds.
 */
static int
update_write(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader)
{
	VkDescriptorBufferInfo buffer;
	struct powervr_vk_descriptor_set *set;
	struct powervr_vk_object *bound;
	uint64_t sampler_id;
	uint64_t view_id;
	uint64_t set_id;
	uint64_t count;
	uint64_t item;
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
	if (reader->error != 0 || count > LAYOUT_ITEMS_MAX)
		return EINVAL;
	set = powervr_vk_payload(session, I915_VK_OBJ_DESCRIPTOR_SET, set_id);

	/* The image descriptors, written into the set's memory from the element on. */
	for (item = 0; item < count; item++) {
		sampler_id = drv_i915_wire_read_u64(reader);
		view_id = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		if (reader->error != 0)
			return EINVAL;
		if (set != NULL)
			set_image(session, set, binding, element + (uint32_t)item, sampler_id, view_id);
	}

	/* The buffer descriptors; the storage buffers are bound from the element on. */
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > LAYOUT_ITEMS_MAX)
		return EINVAL;
	for (item = 0; item < count; item++) {
		kern_memset(&buffer, 0, sizeof(buffer));
		i915_vkc_dec_VkDescriptorBufferInfo(reader, &session->arena, &buffer);
		if (reader->error != 0)
			return EINVAL;

		/* The buffer descriptor in the set's memory. */
		if (set != NULL)
			set_buffer(session, set, binding, element + (uint32_t)item, &buffer);

		/* Only a storage buffer of a known set at a binding the set has; others stay unbound. */
		if (set == NULL || type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || element != 0)
			continue;
		if (binding + item >= POWERVR_VK_BINDINGS_MAX)
			continue;

		/* The binding writes this buffer's range from now on. */
		bound = powervr_vk_object_find(session, I915_VK_OBJ_BUFFER, (uint64_t)(uintptr_t)buffer.buffer);
		error = slot_buffer(&set->slots[binding + item], bound, buffer.offset, buffer.range);
		if (error != 0)
			return error;
	}

	/* The texel buffer views are read; none is bound. */
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

/* Points a binding at a buffer's range (the buffer may be NULL): the new one is held, the old one let go. */
static int
slot_buffer(
	struct powervr_vk_descriptor_slot *slot,
	struct powervr_vk_object *buffer,
	uint64_t offset,
	uint64_t range)
{
	int error;

	/* Holds the new one first: it may be the old one. */
	if (buffer != NULL) {
		error = powervr_vk_object_retain(buffer);
		if (error != 0)
			return error;
	}

	/* Lets the old one go, and names the new range. */
	error = powervr_vk_object_release(slot->buffer);
	slot->buffer = buffer;
	slot->offset = offset;
	slot->range = range;
	if (error != 0)
		return error;

	/* Succeeded: the binding names the buffer. */
	return 0;
}

/*
 * Writes a buffer descriptor into a set's memory: the range's address
 * and size, or zeros for a null buffer or one not bound to memory.  A
 * binding or element the set's layout does not have is left alone, as
 * is a descriptor that is not a buffer's.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_descriptor_set.c
 * (write_buffer: zeros for VK_NULL_HANDLE, else the buffer's address
 * moved by the offset and vk_buffer_range's size).
 */
static void
set_buffer(
	struct powervr_vk_session *session,
	struct powervr_vk_descriptor_set *set,
	uint32_t binding,
	uint32_t element,
	const VkDescriptorBufferInfo *info)
{
	const struct powervr_vk_buffer *buffer;
	uint64_t address;
	uint64_t range;
	uint32_t offset;
	int error;

	/* The descriptor's place; nothing to write where the layout has none. */
	error = powervr_set_descriptor_offset(&set->layout, binding, element, POWERVR_SHADER_PART_BUFFER_ADDRESS, &offset);
	if (error != 0)
		return;

	/* The buffer's range: VK_WHOLE_SIZE reaches its end. */
	address = 0;
	range = 0;
	buffer = powervr_vk_payload(session, I915_VK_OBJ_BUFFER, (uint64_t)(uintptr_t)info->buffer);
	if (buffer != NULL && info->offset <= buffer->size) {
		error = powervr_vk_buffer_address(buffer, &address);
		if (error != 0) {
			address = 0;
		} else {
			address += info->offset;
			range = info->range == VK_WHOLE_SIZE ? buffer->size - info->offset : info->range;
			if (range > UINT32_MAX)
				range = UINT32_MAX;
		}
	}

	/* The descriptor's bytes. */
	powervr_descriptor_buffer(&set->bytes[offset], address, (uint32_t)range);
}

/*
 * Writes an image, sampler or combined image and sampler descriptor into
 * a set's memory, by the binding's type: the view's state words and the
 * words the shader reads, the sampler's words.  A view or sampler that is
 * not known is left as it was, as is a binding the layout does not have.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_descriptor_set.c
 * (write_sampled_image, write_sampler, write_image_sampler: the image's
 * descriptor, then the sampler's at 32 bytes for a combined one).
 */
static void
set_image(
	struct powervr_vk_session *session,
	struct powervr_vk_descriptor_set *set,
	uint32_t binding,
	uint32_t element,
	uint64_t sampler_id,
	uint64_t view_id)
{
	const struct powervr_vk_image_view *view;
	const struct powervr_vk_sampler *sampler;
	uint32_t offset;
	int error;

	/* The view's descriptor, where the binding has one. */
	error = powervr_set_descriptor_offset(&set->layout, binding, element, POWERVR_SHADER_PART_IMAGE_STATE, &offset);
	view = powervr_vk_payload(session, I915_VK_OBJ_IMAGE_VIEW, view_id);
	if (error == 0 && view != NULL && view->image != NULL)
		powervr_descriptor_image(&set->bytes[offset], view->words, (uint32_t)view->layer_bytes, powervr_pack_info(view->format->vk_format));

	/* The sampler's descriptor, where the binding has one; no comparison. */
	error = powervr_set_descriptor_offset(&set->layout, binding, element, POWERVR_SHADER_PART_SAMPLER_STATE, &offset);
	sampler = powervr_vk_payload(session, I915_VK_OBJ_SAMPLER, sampler_id);
	if (error == 0 && sampler != NULL)
		powervr_descriptor_sampler(&set->bytes[offset], sampler->words, VK_COMPARE_OP_NEVER);
}

/* Destroys a set nothing holds: its bindings let their buffers go. */
static int
set_destroy(
	struct powervr_vk_session *session,
	void *payload)
{
	struct powervr_vk_descriptor_set *set;
	uint32_t binding;
	int first_error;
	int error;

	/* Every binding, then the set. */
	(void)session;
	set = payload;
	first_error = 0;
	for (binding = 0; binding < POWERVR_VK_BINDINGS_MAX; binding++) {
		error = powervr_vk_object_release(set->slots[binding].buffer);
		if (error != 0 && first_error == 0)
			first_error = error;
	}

	kern_free(set->bytes);
	kern_free(set);
	if (first_error != 0)
		return first_error;

	/* Succeeded: the set is gone. */
	return 0;
}

/*
 * vkCreateComputePipelines and vkCreateGraphicsPipelines (graphics):
 * [device][cache][count][count]{create info}[allocator][count]
 * [identities] -> [result][count][identities].  Each pipeline's modules
 * must be of the subset (a graphics one is compiled in vk-draw.c); on
 * the first failure none is left and every identity is answered as
 * null.
 */
static int
create_pipelines(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	bool graphics)
{
	struct powervr_vk_pipeline *pipelines[LAYOUT_PIPELINES_MAX];
	uint64_t identities[LAYOUT_PIPELINES_MAX];
	uint64_t count;
	uint64_t answered;
	uint64_t index;
	uint64_t published;
	uint32_t result;
	int refused;
	int decoded;
	int error;

	/* How many pipelines follow, behind the device, the cache and the count's first form. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count == 0 || count > LAYOUT_PIPELINES_MAX)
		return EINVAL;

	/* Decodes every record; one that does not decode leaves the stream unreadable. */
	kern_memset(pipelines, 0, sizeof(pipelines));
	error = 0;
	refused = 0;
	for (index = 0; index < count; index++) {
		pipelines[index] = kern_calloc(1, sizeof(*pipelines[index]));
		if (pipelines[index] == NULL) {
			error = ENOMEM;
			break;
		}

		if (graphics) {
			decoded = powervr_vk_graphics_decode(session, reader, pipelines[index]);
		} else {
			decoded = decode_pipeline(session, reader, pipelines[index]);
		}
		if (decoded == EINVAL) {
			error = EINVAL;
			break;
		}
		if (decoded != 0 && refused == 0)
			refused = decoded;
	}

	/* The identities behind the allocator, one per pipeline. */
	if (error == 0) {
		(void)drv_i915_wire_read_u64(reader);
		answered = drv_i915_wire_read_u64(reader);
		if (answered != count)
			error = EINVAL;
		for (index = 0; error == 0 && index < count; index++)
			identities[index] = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			error = EINVAL;
	}

	/* A command that could not be read to its end fails the frame. */
	if (error != 0) {
		for (index = 0; index < count; index++)
			(void)pipeline_destroy(session, pipelines[index]);
		return error;
	}

	/* Publishes each one unless a record was refused, up to the first failure. */
	error = refused;
	published = 0;
	for (index = 0; error == 0 && index < count; index++) {
		error = powervr_vk_object_publish(session, I915_VK_OBJ_PIPELINE, identities[index], pipelines[index], pipeline_destroy);
		if (error != 0)
			break;
		pipelines[index] = NULL;
		published++;
	}

	/* A create that failed leaves no pipeline. */
	if (error != 0) {
		for (index = 0; index < published; index++)
			(void)powervr_vk_object_remove(session, I915_VK_OBJ_PIPELINE, identities[index]);
	}

	/* What was not published goes. */
	for (index = 0; index < count; index++)
		(void)pipeline_destroy(session, pipelines[index]);

	/* The result and the count, then each identity, or null for each when the create failed. */
	result = powervr_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);
	drv_i915_wire_reply_u64(reply, count);
	for (index = 0; index < count; index++) {
		answered = 0;
		if (error == 0)
			answered = identities[index];
		drv_i915_wire_reply_u64(reply, answered);
	}

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * Decodes one VkComputePipelineCreateInfo: the compute stage of a known
 * module of the subset, without specialization.  EINVAL when the record
 * did not decode, ENOTSUP for one the executor does not run.
 */
static int
decode_pipeline(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct powervr_vk_pipeline *pipeline)
{
	VkComputePipelineCreateInfo info;
	const struct powervr_vk_shader *shader;

	/* The record. */
	kern_memset(&info, 0, sizeof(info));
	i915_vkc_dec_VkComputePipelineCreateInfo(reader, &session->arena, &info);
	if (reader->error != 0)
		return EINVAL;

	/* The compute stage of a known module, without specialization constants. */
	if (info.stage.stage != VK_SHADER_STAGE_COMPUTE_BIT)
		return ENOTSUP;
	if (info.stage.pSpecializationInfo != NULL && info.stage.pSpecializationInfo->mapEntryCount != 0)
		return ENOTSUP;
	shader = powervr_vk_payload(session, I915_VK_OBJ_SHADER_MODULE, (uint64_t)(uintptr_t)info.stage.module);
	if (shader == NULL || shader->status != 0)
		return ENOTSUP;

	/* The pipeline's own copy of the module's words. */
	pipeline->words = kern_malloc((size_t)shader->word_count * 4U);
	if (pipeline->words == NULL)
		return ENOMEM;
	kern_memcpy(pipeline->words, shader->words, (size_t)shader->word_count * 4U);
	pipeline->word_count = shader->word_count;

	/* Succeeded: the pipeline runs its module's compute entry. */
	return 0;
}

/* Destroys a pipeline and its copy of the SPIR-V; a missing one is nothing to do. */
static int
pipeline_destroy(
	struct powervr_vk_session *session,
	void *payload)
{
	struct powervr_vk_pipeline *pipeline;

	/* Its words, then itself. */
	(void)session;
	pipeline = payload;
	if (pipeline == NULL)
		return 0;
	kern_free(pipeline->words);
	powervr_vk_graphics_free(pipeline->graphics);
	kern_free(pipeline);

	/* Succeeded: the pipeline is gone. */
	return 0;
}
