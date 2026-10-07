/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's descriptor set layouts, pools and sets (see
 * descriptor.h).
 *
 * Every command is decoded exactly as libvulkan encodes it: the records
 * through the generated codec, the framing around them as read from the
 * library's own sender (descriptors.c of libvulkan).
 */

#include "descriptor.h"
#include "codec.h"
#include "forget.h"
#include "gfx.h"
#include "internal.h"
#include "object.h"
#include "reply.h"
#include <kern/kcrt.h>

#include <kern/klog.h>
#include <kern/kmem.h>

#include <libc/vulkan/vulkan_core.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "vulkan-codec.inc"

/* How many sets one vkAllocateDescriptorSets may allocate. */
#define I915_GFX_MAX_ALLOCATED_SETS	8U

/* How many writes, copies and descriptors of one kind one update may carry. */
#define I915_GFX_MAX_UPDATE_ITEMS	64U

static int i915_gfx_update_write(struct i915_render_session *session, struct i915_wire_reader *reader);
static void i915_gfx_update_unknown_set(uint32_t binding, uint64_t identity);
static void i915_gfx_update_unknown_image(uint32_t binding, uint64_t sampler, int sampler_found, uint64_t view, int view_found);
static int i915_dset_of_pool(void *object, void *argument);
static void i915_dpool_sets_free(struct i915_render_session *session, void *pool);

/*
 * Creates a VkDescriptorSetLayout: vkCreateDescriptorSetLayout, a generic
 * create.
 *
 * The layout keeps each binding's number, type and stages.  A layout with
 * more bindings than a set holds is refused.
 */
int
drv_i915_gfx_create_dsl(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDescriptorSetLayoutCreateInfo info;
	struct i915_gfx_dsl *dsl;
	uint64_t identity;
	uint32_t index;
	int error;

	/* Decodes the create info behind the device and its presence marker. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkDescriptorSetLayoutCreateInfo(reader, &session->arena, &info);
	identity = drv_i915_gfx_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Refuses more bindings than a layout lists, or a binding number past the ones a set holds. */
	dsl = NULL;
	error = 0;
	if (info.bindingCount > I915_GFX_MAX_LAYOUT_BINDINGS)
		error = ENOTSUP;
	for (index = 0U; error == 0 && index < info.bindingCount; index++) {
		if (info.pBindings[index].binding >= I915_GFX_MAX_BINDINGS)
			error = ENOTSUP;
	}

	/* Allocates a layout that fits. */
	if (error == 0)
		dsl = kern_calloc(1U, sizeof(*dsl));

	/* Keeps each binding's number, descriptor type and stages. */
	if (dsl != NULL) {
		dsl->count = info.bindingCount;
		for (index = 0U; index < info.bindingCount; index++) {
			dsl->bindings[index].binding = info.pBindings[index].binding;
			dsl->bindings[index].type = info.pBindings[index].descriptorType;
			dsl->bindings[index].stages = info.pBindings[index].stageFlags;
		}
	}

	/* Publishes the layout and answers; a refused or failed layout is reported there. */
	drv_i915_gfx_create_reply(session, reply, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT, identity, dsl, error);

	/* Succeeded: the reply carries the result of the create. */
	return 0;
}

/*
 * Creates a VkDescriptorPool: vkCreateDescriptorPool, a generic create.
 *
 * A descriptor pool bounds nothing here, because sets are small host
 * objects; the pool keeps only its maxSets.
 */
int
drv_i915_gfx_create_dpool(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDescriptorPoolCreateInfo info;
	uint64_t identity;
	uint32_t *pool;

	/* Decodes the create info behind the device and its presence marker. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkDescriptorPoolCreateInfo(reader, &session->arena, &info);
	identity = drv_i915_gfx_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Allocates the pool, which is its maxSets and nothing else. */
	pool = kern_calloc(1U, sizeof(*pool));
	if (pool != NULL)
		*pool = info.maxSets;

	/* Publishes the pool and answers; a failed allocation is reported there. */
	drv_i915_gfx_create_reply(session, reply, I915_VK_OBJ_DESCRIPTOR_POOL, identity, pool, 0);

	/* Succeeded: the reply carries the result of the create. */
	return 0;
}

/*
 * Allocates descriptor sets: vkAllocateDescriptorSets.
 *
 * The command is [device][present][VkDescriptorSetAllocateInfo][count]
 * [identities] and the reply [result][count][identities].  Each set records
 * the layout it was allocated with; an unknown layout leaves it NULL.
 */
int
drv_i915_gfx_allocate_dsets(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkDescriptorSetAllocateInfo info;
	uint64_t identities[I915_GFX_MAX_ALLOCATED_SETS];
	const struct i915_gfx_dsl *layout;
	struct i915_gfx_dset *dset;
	uint64_t count;
	uint64_t index;
	uint64_t layout_id;
	uint32_t result;
	int error;

	/* Decodes the allocate info behind the device and its presence marker. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkDescriptorSetAllocateInfo(reader, &session->arena, &info);

	/* Reads how many identities follow; they must match the sets asked for. */
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count > I915_GFX_MAX_ALLOCATED_SETS)
		return EINVAL;
	if (count != info.descriptorSetCount)
		return EINVAL;

	/* Reads the identity libvulkan chose for each set. */
	for (index = 0U; index < count; index++)
		identities[index] = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/*
	 * Allocates and publishes each set with its layout, up to the first
	 * failure.  XXX: a batch that fails part-way leaves its earlier sets
	 * allocated (happy path only).
	 */
	error = 0;
	for (index = 0U; index < count; index++) {
		dset = kern_calloc(1U, sizeof(*dset));
		if (dset == NULL) {
			error = ENOMEM;
			break;
		}

		/*
		 * Records a copy of the layout the set was allocated with (the
		 * application may destroy the layout while the set is used,
		 * BUG-260).
		 */
		layout_id = (uint64_t)(uintptr_t)info.pSetLayouts[index];
		layout = drv_i915_object_lookup(session, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT, layout_id);
		dset->layout = NULL;
		if (layout != NULL) {
			kern_memcpy(&dset->layout_copy, layout, sizeof(dset->layout_copy));
			dset->layout = &dset->layout_copy;
		}

		/* The pool it came from. */
		dset->pool = drv_i915_object_lookup(session, I915_VK_OBJ_DESCRIPTOR_POOL, (uint64_t)(uintptr_t)info.descriptorPool);

		/* Publishes the set; one that cannot be published is freed. */
		error = drv_i915_object_insert(session, I915_VK_OBJ_DESCRIPTOR_SET, identities[index], dset);
		if (error != 0) {
			kern_free(dset);
			break;
		}
	}

	/* Writes the result of the allocation. */
	result = drv_i915_gfx_result(error);
	drv_i915_wire_reply_u32(reply, result);

	/* Answers every set's identity when all of them were allocated. */
	if (error == 0) {
		drv_i915_wire_reply_u64(reply, count);
		for (index = 0U; index < count; index++)
			drv_i915_wire_reply_u64(reply, identities[index]);
	}

	/* Succeeded: the reply carries the result of the allocation. */
	return 0;
}

/*
 * Updates descriptor sets: vkUpdateDescriptorSets.
 *
 * The command is [device][n][n]{write}[m][m]{copy}; its writes are laid out
 * as i915_gfx_update_write reads them.  There is no reply body.  Only the
 * first image or uniform buffer (plain or dynamic) descriptor of a write is
 * applied; other buffer descriptors, texel views and copies are read and
 * reported as not applied.
 */
int
drv_i915_gfx_update_dsets(
	struct i915_render_session *session,
	struct i915_wire_reader *reader)
{
	VkCopyDescriptorSet copy;
	uint64_t writes;
	uint64_t count;
	uint64_t index;
	int error;

	/* Reads how many writes follow, behind the device and the write count's first form. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	writes = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (writes > I915_GFX_MAX_UPDATE_ITEMS)
		return EINVAL;

	/* Applies each write; a write that does not decode fails the command. */
	for (index = 0U; index < writes; index++) {
		error = i915_gfx_update_write(session, reader);
		if (error != 0)
			return error;
	}

	/* Reads how many copies follow, behind the copy count's first form. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count > I915_GFX_MAX_UPDATE_ITEMS)
		return EINVAL;

	/* Decodes each copy; none is applied. */
	for (index = 0U; index < count; index++)
		i915_vkc_dec_VkCopyDescriptorSet(reader, &session->arena, &copy);

	/* Reports the copies that were not applied. */
	if (count != 0U)
		kern_logf("i915: vk: XXX vkUpdateDescriptorSets: descriptor copies are not applied\n");

	/* Fails a command whose copies did not decode. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the writes are applied. */
	return 0;
}


/* Counts the diagnostics below: only the first 16 are logged (BUG-117). */
static unsigned i915_gfx_update_said;

/* Logs a descriptor write to an identity that names no descriptor set of the session. */
static void
i915_gfx_update_unknown_set(
	uint32_t binding,
	uint64_t identity)
{
	/* Only the first ones. */
	if (i915_gfx_update_said >= 16U)
		return;
	i915_gfx_update_said++;
	kern_logf("i915: vk: vkUpdateDescriptorSets: 0x%llx (binding %u) is not a descriptor set of the session\n",
		  (unsigned long long)identity,
		  binding);
}

/* Logs an image descriptor whose sampler or view names no object of the session. */
static void
i915_gfx_update_unknown_image(
	uint32_t binding,
	uint64_t sampler,
	int sampler_found,
	uint64_t view,
	int view_found)
{
	const char *sampler_state;
	const char *view_state;

	/* Only the first ones. */
	if (i915_gfx_update_said >= 16U)
		return;
	i915_gfx_update_said++;
	sampler_state = "unknown";
	if (sampler_found)
		sampler_state = "found";
	view_state = "unknown";
	if (view_found)
		view_state = "found";
	kern_logf("i915: vk: vkUpdateDescriptorSets: binding %u: sampler 0x%llx %s, view 0x%llx %s\n",
		  binding,
		  (unsigned long long)sampler,
		  sampler_state,
		  (unsigned long long)view,
		  view_state);
}

/*
 * Reads one descriptor write and applies its first image descriptor, its
 * first buffer descriptor when it is a uniform or storage buffer, plain or
 * dynamic, or its first texel buffer view (a uniform texel buffer).
 *
 * A write is [sType][pNext][set][binding][element][count][type][images]
 * {[sampler][view][layout]}[buffers]{VkDescriptorBufferInfo}[texel views]
 * {identity}.
 */
static int
i915_gfx_update_write(
	struct i915_render_session *session,
	struct i915_wire_reader *reader)
{
	VkDescriptorBufferInfo buffer_info;
	struct i915_gfx_dset *dset;
	uint64_t dset_id;
	uint64_t count;
	uint64_t item;
	uint64_t sampler;
	uint64_t view;
	uint64_t buffer_id;
	uint64_t texel;
	uint32_t binding;
	uint32_t type;

	/* Reads the set, the binding and the descriptor type behind sType and pNext. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	dset_id = drv_i915_wire_read_u64(reader);
	dset = drv_i915_object_lookup(session, I915_VK_OBJ_DESCRIPTOR_SET, dset_id);
	binding = drv_i915_wire_read_u32(reader);

	/* Says so when the set is not one of the session's (BUG-117): the write is not applied. */
	if (dset == NULL)
		i915_gfx_update_unknown_set(binding, dset_id);

	/* Skips dstArrayElement and descriptorCount.  XXX: arrays of descriptors are not laid out. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);

	/* Reads how many image descriptors follow. */
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count > I915_GFX_MAX_UPDATE_ITEMS)
		return EINVAL;

	/* Reads every image descriptor and applies the first to a known set's binding. */
	for (item = 0U; item < count; item++) {
		sampler = drv_i915_wire_read_u64(reader);
		view = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);

		/* Only the first descriptor of a known set within the set's bindings is applied. */
		if (item != 0U ||
		    dset == NULL ||
		    binding >= I915_GFX_MAX_BINDINGS)
			continue;

		/* The binding samples this view with this sampler from here on, and no texel buffer. */
		dset->slots[binding].sampler = drv_i915_object_lookup(session, I915_VK_OBJ_SAMPLER, sampler);
		dset->slots[binding].view = drv_i915_gfx_view_lookup(session, view);
		dset->slots[binding].texel = NULL;

		/* Says so when a handle names no object of the session (BUG-117): the draws that sample the binding are refused. */
		if ((dset->slots[binding].sampler == NULL && sampler != 0U) ||
		    (dset->slots[binding].view == NULL && view != 0U))
			i915_gfx_update_unknown_image(binding,
						      sampler,
						      dset->slots[binding].sampler != NULL,
						      view,
						      dset->slots[binding].view != NULL);
	}

	/* Reads how many buffer descriptors follow. */
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count > I915_GFX_MAX_UPDATE_ITEMS)
		return EINVAL;

	/* Reads every buffer descriptor and applies the first uniform buffer to a known set's binding. */
	for (item = 0U; item < count; item++) {
		kern_memset(&buffer_info, 0, sizeof(buffer_info));
		i915_vkc_dec_VkDescriptorBufferInfo(reader, &session->arena, &buffer_info);

		/* Only the first descriptor of a known set within the set's bindings is applied. */
		if (item != 0U ||
		    dset == NULL ||
		    binding >= I915_GFX_MAX_BINDINGS)
			continue;

		/* XXX: a buffer descriptor of another type than a uniform or storage buffer is not bound to anything. */
		if (type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
		    type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
		    type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
		    type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
			kern_logf("i915: vk: XXX vkUpdateDescriptorSets: buffer descriptors (type %u) are not bound to anything\n", type);
			continue;
		}

		/* The binding reads this range of this buffer from here on. */
		buffer_id = (uint64_t)(uintptr_t)buffer_info.buffer;
		dset->slots[binding].buffer = drv_i915_object_lookup(session, I915_VK_OBJ_BUFFER, buffer_id);
		dset->slots[binding].offset = buffer_info.offset;
		dset->slots[binding].range = buffer_info.range;

		/* A dynamic buffer's range moves by the dynamic offset of each bind. */
		dset->slots[binding].dynamic = 0;
		if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
			dset->slots[binding].dynamic = 1;
	}

	/* Reads how many texel buffer views follow. */
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count > I915_GFX_MAX_UPDATE_ITEMS)
		return EINVAL;

	/* Reads every texel buffer view and applies the first to a known set's binding. */
	for (item = 0U; item < count; item++) {
		texel = drv_i915_wire_read_u64(reader);

		/* Only the first descriptor of a known set within the set's bindings is applied. */
		if (item != 0U ||
		    dset == NULL ||
		    binding >= I915_GFX_MAX_BINDINGS)
			continue;

		/* The binding reads this view's texels from here on, and no image. */
		dset->slots[binding].texel = drv_i915_object_lookup(session, I915_VK_OBJ_BUFFER_VIEW, texel);
		dset->slots[binding].view = NULL;
		dset->slots[binding].sampler = NULL;

		/* Says so when the handle names no buffer view of the session: the draws that read the binding are refused. */
		if (dset->slots[binding].texel == NULL && texel != 0U)
			kern_logf("i915: vk: vkUpdateDescriptorSets: binding %u: texel buffer view 0x%llx is not known\n",
				  binding,
				  (unsigned long long)texel);
	}

	/* Succeeded: the write is read and its first image, uniform buffer or texel buffer descriptor applied. */
	return 0;
}

/* Picks the sets allocated from the pool `argument`. */
static int
i915_dset_of_pool(
	void *object,
	void *argument)
{
	struct i915_gfx_dset *dset;

	/* A set belongs to the pool it records. */
	dset = object;
	return dset->pool == argument;
}

/*
 * Frees every set still allocated from a descriptor pool (their identities
 * are withdrawn here), keeping the pool.
 */
static void
i915_dpool_sets_free(
	struct i915_render_session *session,
	void *pool)
{
	struct i915_gfx_dset *dset;

	/* Takes and frees the pool's sets one by one. */
	for (;;) {
		dset = drv_i915_object_take(session, I915_VK_OBJ_DESCRIPTOR_SET, i915_dset_of_pool, pool);
		if (dset == NULL)
			break;

		/* Command buffers that bound it let go of it (BUG-260), then it is freed. */
		drv_i915_gfx_forget(session, I915_VK_OBJ_DESCRIPTOR_SET, dset);
		kern_free(dset);
	}
}

/*
 * Frees a descriptor pool whose identity is withdrawn, with every set still
 * allocated from it (their identities are withdrawn here).
 */
void
drv_i915_gfx_dpool_free(
	struct i915_render_session *session,
	void *pool)
{
	/* The sets, then the pool itself. */
	i915_dpool_sets_free(session, pool);
	kern_free(pool);
}

/*
 * Resets a VkDescriptorPool: vkResetDescriptorPool.
 *
 * The command is [device][identity][flags] and the reply [result].  The
 * sets allocated from the pool are freed; the pool stays.  An unknown
 * identity is not an error.
 */
int
drv_i915_gfx_reset_dpool(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	void *pool;
	uint64_t identity;

	/* Reads the identity between the device and the flags (which have no bits). */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Frees a known pool's sets. */
	pool = drv_i915_object_lookup(session, I915_VK_OBJ_DESCRIPTOR_POOL, identity);
	if (pool != NULL)
		i915_dpool_sets_free(session, pool);

	/* Succeeded: the reply carries VK_SUCCESS. */
	drv_i915_wire_reply_u32(reply, 0U);
	return 0;
}

/*
 * Destroys a VkDescriptorPool: vkDestroyDescriptorPool.
 *
 * The command is [device][identity][pAllocator] and has no reply body.  The
 * sets allocated from the pool go with it.  An unknown identity is not an
 * error.
 */
int
drv_i915_gfx_destroy_dpool(
	struct i915_render_session *session,
	struct i915_wire_reader *reader)
{
	void *pool;
	uint64_t identity;

	/* Reads the identity between the device and the allocator. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Unpublishes a known pool, then frees it with its sets. */
	pool = drv_i915_object_lookup(session, I915_VK_OBJ_DESCRIPTOR_POOL, identity);
	if (pool != NULL) {
		drv_i915_object_remove(session, I915_VK_OBJ_DESCRIPTOR_POOL, identity);
		drv_i915_gfx_dpool_free(session, pool);
	}

	/* Succeeded: the pool and its sets are gone. */
	return 0;
}
