/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's descriptor set layouts, pools and sets (see
 * vk-descriptor.h).
 *
 * Every command is decoded exactly as libvulkan encodes it: the records
 * through the generated codec, the framing around them as read from the
 * library's own sender (descriptors.c of libvulkan).
 */

#include "vk-descriptor.h"
#include "vk-codec.h"
#include "vk-forget.h"
#include "vk-gfx.h"
#include "vk-internal.h"
#include "vk-object.h"
#include "vk-reply.h"
#include <kern/kcrt.h>

#include <kern/klog.h>
#include <kern/kmem.h>

#include <libc/vulkan/vulkan_core.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "vk-codec.inc"

/* How many sets one vkAllocateDescriptorSets may allocate. */
#define I915_GFX_MAX_ALLOCATED_SETS	8U

/* How many writes, copies and descriptors of one kind one update may carry. */
#define I915_GFX_MAX_UPDATE_ITEMS	64U


static int i915_gfx_update_write(struct i915_render_session *session, struct i915_wire_reader *reader);
static void i915_gfx_update_copy(struct i915_render_session *session, const VkCopyDescriptorSet *copy);
static struct i915_gfx_descriptor *i915_gfx_dset_walk(const struct i915_gfx_dset *set, uint32_t *binding, uint32_t *element);
static uint32_t i915_gfx_dsl_descriptors(const struct i915_gfx_dsl *layout, uint32_t binding);
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
	uint32_t total;
	int error;

	/* Decodes the create info behind the device and its presence marker. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkDescriptorSetLayoutCreateInfo(reader, &session->arena, &info);
	identity = drv_i915_gfx_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/*
	 * Refuses more bindings than a layout lists, a binding number past the
	 * ones a set holds, and more descriptors in all than a set holds.
	 */
	dsl = NULL;
	error = 0;
	total = 0U;
	if (info.bindingCount > I915_GFX_MAX_LAYOUT_BINDINGS)
		error = ENOTSUP;
	for (index = 0U; error == 0 && index < info.bindingCount; index++) {
		if (info.pBindings[index].binding >= I915_GFX_MAX_BINDINGS)
			error = ENOTSUP;
		total += info.pBindings[index].descriptorCount;
		if (total > I915_GFX_MAX_SET_DESCRIPTORS) {
			kern_logf("i915: vk: XXX descriptor set layout of more than %u descriptors\n", I915_GFX_MAX_SET_DESCRIPTORS);
			error = ENOTSUP;
		}
	}

	/* Allocates a layout that fits. */
	if (error == 0)
		dsl = kern_calloc(1U, sizeof(*dsl));

	/*
	 * Keeps each binding's number, descriptor type, stages and array
	 * elements; a binding of no descriptors is only a reserved number,
	 * which nothing names and an update steps over, so it is not kept.
	 */
	if (dsl != NULL) {
		dsl->count = 0U;
		for (index = 0U; index < info.bindingCount; index++) {
			if (info.pBindings[index].descriptorCount == 0U)
				continue;
			dsl->bindings[dsl->count].binding = info.pBindings[index].binding;
			dsl->bindings[dsl->count].type = info.pBindings[index].descriptorType;
			dsl->bindings[dsl->count].stages = info.pBindings[index].stageFlags;
			dsl->bindings[dsl->count].descriptors = info.pBindings[index].descriptorCount;
			dsl->count++;
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
	uint32_t binding;
	uint32_t elements;
	uint32_t extra;
	uint32_t place;
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
		/* The layout the set is allocated with; an unknown one leaves the set's bindings one descriptor each. */
		layout_id = (uint64_t)(uintptr_t)info.pSetLayouts[index];
		layout = drv_i915_object_lookup(session, I915_VK_OBJ_DESCRIPTOR_SET_LAYOUT, layout_id);

		/* Counts the array elements past the first of every binding, which the set keeps behind itself. */
		extra = 0U;
		for (binding = 0U; layout != NULL && binding < I915_GFX_MAX_BINDINGS; binding++) {
			elements = i915_gfx_dsl_descriptors(layout, binding);
			if (elements > 1U)
				extra += elements - 1U;
		}

		/* Allocates the set with room for them. */
		dset = kern_calloc(1U, sizeof(*dset) + (size_t)extra * sizeof(struct i915_gfx_descriptor));
		if (dset == NULL) {
			error = ENOMEM;
			break;
		}

		/*
		 * Records a copy of the layout the set was allocated with (the
		 * application may destroy the layout while the set is used).
		 */
		dset->layout = NULL;
		if (layout != NULL) {
			kern_memcpy(&dset->layout_copy, layout, sizeof(dset->layout_copy));
			dset->layout = &dset->layout_copy;
		}

		/* Places each binding's other elements in the room behind the set, in binding order. */
		dset->extra = NULL;
		if (extra != 0U)
			dset->extra = (struct i915_gfx_descriptor *)(dset + 1);
		dset->extra_total = extra;
		place = 0U;
		for (binding = 0U; layout != NULL && binding < I915_GFX_MAX_BINDINGS; binding++) {
			elements = i915_gfx_dsl_descriptors(layout, binding);
			dset->extra_first[binding] = (uint16_t)place;
			dset->extra_count[binding] = 0U;
			if (elements > 1U) {
				dset->extra_count[binding] = (uint16_t)(elements - 1U);
				place += elements - 1U;
			}
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
 * as i915_gfx_update_write reads them.  There is no reply body.  The writes
 * are applied in order, then the copies; a buffer descriptor of another type
 * than a uniform or storage buffer is read and reported as not applied.
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

	/* Decodes and applies each copy, in order. */
	for (index = 0U; index < count; index++) {
		kern_memset(&copy, 0, sizeof(copy));
		i915_vkc_dec_VkCopyDescriptorSet(reader, &session->arena, &copy);
		if (reader->error != 0)
			break;
		i915_gfx_update_copy(session, &copy);
	}

	/* Fails a command whose copies did not decode. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the writes and the copies are applied. */
	return 0;
}

/*
 * Finds the descriptor of array element `element` of a set's binding: the
 * binding's own slot for element 0, one of the elements kept behind the set
 * for the others.  NULL for a binding past the ones a set holds, or an
 * element past the binding's.
 */
struct i915_gfx_descriptor *
drv_i915_gfx_dset_descriptor(
	const struct i915_gfx_dset *set,
	uint32_t binding,
	uint32_t element)
{
	struct i915_gfx_dset *writable;

	/* A binding the set does not hold has no descriptor. */
	if (set == NULL || binding >= I915_GFX_MAX_BINDINGS)
		return NULL;

	/* Element 0 is the binding's own slot. */
	writable = (struct i915_gfx_dset *)(uintptr_t)set;
	if (element == 0U)
		return &writable->slots[binding];

	/* An element past the binding's is none. */
	if (writable->extra == NULL || element - 1U >= writable->extra_count[binding])
		return NULL;

	/* Succeeded: the element behind the set. */
	return &writable->extra[writable->extra_first[binding] + element - 1U];
}

/*
 * Applies one descriptor copy: descriptorCount descriptors from the source
 * set's binding and element on to the destination's, each running on into
 * the next binding when its binding's elements end (as an update does).
 * A copy that names an unknown set, or runs past the descriptors a set
 * has, copies what it reaches and says so.
 */
static void
i915_gfx_update_copy(
	struct i915_render_session *session,
	const VkCopyDescriptorSet *copy)
{
	const struct i915_gfx_descriptor *source;
	struct i915_gfx_descriptor *destination;
	struct i915_gfx_dset *src_set;
	struct i915_gfx_dset *dst_set;
	uint32_t src_binding;
	uint32_t src_element;
	uint32_t dst_binding;
	uint32_t dst_element;
	uint32_t index;

	/* The two sets. */
	src_set = drv_i915_object_lookup(session, I915_VK_OBJ_DESCRIPTOR_SET, (uint64_t)(uintptr_t)copy->srcSet);
	dst_set = drv_i915_object_lookup(session, I915_VK_OBJ_DESCRIPTOR_SET, (uint64_t)(uintptr_t)copy->dstSet);
	if (src_set == NULL || dst_set == NULL) {
		kern_logf("i915: vk: vkUpdateDescriptorSets: a copy names a set that is not one of the session's\n");
		return;
	}

	/* Copies each descriptor, the source and the destination each stepping on to its next element. */
	src_binding = copy->srcBinding;
	src_element = copy->srcArrayElement;
	dst_binding = copy->dstBinding;
	dst_element = copy->dstArrayElement;
	for (index = 0U; index < copy->descriptorCount; index++) {
		source = i915_gfx_dset_walk(src_set, &src_binding, &src_element);
		destination = i915_gfx_dset_walk(dst_set, &dst_binding, &dst_element);
		if (source == NULL || destination == NULL) {
			kern_logf("i915: vk: vkUpdateDescriptorSets: a copy of %u descriptors runs past a set's\n", copy->descriptorCount);
			return;
		}
		*destination = *source;
		src_element++;
		dst_element++;
	}
}

/*
 * Finds the descriptor an update or a copy reaches at a binding and an
 * element, moving on to the next binding of the set's layout (from its
 * element 0) while the element is past its binding's: the rule by which
 * one write or copy of several descriptors continues into the following
 * bindings.  The binding and the element are left at the descriptor
 * found; NULL when the set has no more.
 */
static struct i915_gfx_descriptor *
i915_gfx_dset_walk(
	const struct i915_gfx_dset *set,
	uint32_t *binding,
	uint32_t *element)
{
	uint32_t elements;

	/* Moves past the bindings the element does not fall in. */
	while (*binding < I915_GFX_MAX_BINDINGS) {
		/* A set without a layout has one descriptor to each binding. */
		elements = 1U;
		if (set->layout != NULL)
			elements = i915_gfx_dsl_descriptors(set->layout, *binding);
		if (*element < elements)
			break;
		*element -= elements;
		*binding += 1U;
	}

	/* Succeeded: the descriptor, or NULL past the set's. */
	return drv_i915_gfx_dset_descriptor(set, *binding, *element);
}

/* Counts the array elements a layout gives a binding number: 0 for one it does not list. */
static uint32_t
i915_gfx_dsl_descriptors(
	const struct i915_gfx_dsl *layout,
	uint32_t binding)
{
	uint32_t entry;

	/* Looks the binding up among the layout's. */
	for (entry = 0U; entry < layout->count && entry < I915_GFX_MAX_LAYOUT_BINDINGS; entry++) {
		if (layout->bindings[entry].binding != binding)
			continue;

		/* A layout filled by hand gives none, which is one. */
		if (layout->bindings[entry].descriptors == 0U)
			return 1U;
		return layout->bindings[entry].descriptors;
	}

	/* Succeeded: the layout does not list the binding. */
	return 0U;
}


/* Counts the diagnostics below: only the first 16 are logged. */
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
 * Reads one descriptor write and applies each of its descriptors to the
 * array element it reaches: from dstArrayElement of dstBinding on, running
 * into the following bindings when the binding's elements end.  An image
 * descriptor names a view and a sampler; a buffer descriptor of a uniform
 * or storage buffer, plain or dynamic, a range; a texel buffer view a
 * uniform texel buffer.
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
	struct i915_gfx_descriptor *descriptor;
	struct i915_gfx_dset *dset;
	uint64_t dset_id;
	uint64_t count;
	uint64_t item;
	uint64_t sampler;
	uint64_t view;
	uint64_t buffer_id;
	uint64_t texel;
	uint32_t binding;
	uint32_t first_element;
	uint32_t element;
	uint32_t at;
	uint32_t type;

	/* Reads the set, the binding, the first element and the descriptor type behind sType and pNext. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	dset_id = drv_i915_wire_read_u64(reader);
	dset = drv_i915_object_lookup(session, I915_VK_OBJ_DESCRIPTOR_SET, dset_id);
	binding = drv_i915_wire_read_u32(reader);
	first_element = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);

	/* Says so when the set is not one of the session's: the write is not applied. */
	if (dset == NULL)
		i915_gfx_update_unknown_set(binding, dset_id);

	/* Reads how many image descriptors follow. */
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count > I915_GFX_MAX_UPDATE_ITEMS)
		return EINVAL;

	/* Reads every image descriptor and applies it to the element it reaches in a known set. */
	for (item = 0U; item < count; item++) {
		sampler = drv_i915_wire_read_u64(reader);
		view = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);

		/* The element the descriptor reaches; none past the set's. */
		at = binding;
		element = first_element + (uint32_t)item;
		descriptor = NULL;
		if (dset != NULL)
			descriptor = i915_gfx_dset_walk(dset, &at, &element);
		if (descriptor == NULL)
			continue;

		/* The element samples this view with this sampler from here on, and no texel buffer. */
		descriptor->sampler = drv_i915_object_lookup(session, I915_VK_OBJ_SAMPLER, sampler);
		descriptor->view = drv_i915_gfx_view_lookup(session, view);
		descriptor->texel = NULL;

		/* Says so when a handle names no object of the session: the draws that sample the element are refused. */
		if ((descriptor->sampler == NULL && sampler != 0U) ||
		    (descriptor->view == NULL && view != 0U))
			i915_gfx_update_unknown_image(at,
						      sampler,
						      descriptor->sampler != NULL,
						      view,
						      descriptor->view != NULL);
	}

	/* Reads how many buffer descriptors follow. */
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count > I915_GFX_MAX_UPDATE_ITEMS)
		return EINVAL;

	/* Reads every buffer descriptor and applies a uniform or storage buffer to the element it reaches in a known set. */
	for (item = 0U; item < count; item++) {
		kern_memset(&buffer_info, 0, sizeof(buffer_info));
		i915_vkc_dec_VkDescriptorBufferInfo(reader, &session->arena, &buffer_info);

		/* The element the descriptor reaches; none past the set's. */
		at = binding;
		element = first_element + (uint32_t)item;
		descriptor = NULL;
		if (dset != NULL)
			descriptor = i915_gfx_dset_walk(dset, &at, &element);
		if (descriptor == NULL)
			continue;

		/* XXX: a buffer descriptor of another type than a uniform or storage buffer is not bound to anything. */
		if (type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
		    type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
		    type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
		    type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
			kern_logf("i915: vk: XXX vkUpdateDescriptorSets: buffer descriptors (type %u) are not bound to anything\n", type);
			continue;
		}

		/* The element reads this range of this buffer from here on. */
		buffer_id = (uint64_t)(uintptr_t)buffer_info.buffer;
		descriptor->buffer = drv_i915_object_lookup(session, I915_VK_OBJ_BUFFER, buffer_id);
		descriptor->offset = buffer_info.offset;
		descriptor->range = buffer_info.range;

		/* A dynamic buffer's range moves by the dynamic offset of each bind. */
		descriptor->dynamic = 0;
		if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
			descriptor->dynamic = 1;
	}

	/* Reads how many texel buffer views follow. */
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count > I915_GFX_MAX_UPDATE_ITEMS)
		return EINVAL;

	/* Reads every texel buffer view and applies it to the element it reaches in a known set. */
	for (item = 0U; item < count; item++) {
		texel = drv_i915_wire_read_u64(reader);

		/* The element the descriptor reaches; none past the set's. */
		at = binding;
		element = first_element + (uint32_t)item;
		descriptor = NULL;
		if (dset != NULL)
			descriptor = i915_gfx_dset_walk(dset, &at, &element);
		if (descriptor == NULL)
			continue;

		/* The element reads this view's texels from here on, and no image. */
		descriptor->texel = drv_i915_object_lookup(session, I915_VK_OBJ_BUFFER_VIEW, texel);
		descriptor->view = NULL;
		descriptor->sampler = NULL;

		/* Says so when the handle names no buffer view of the session: the draws that read the element are refused. */
		if (descriptor->texel == NULL && texel != 0U)
			kern_logf("i915: vk: vkUpdateDescriptorSets: binding %u: texel buffer view 0x%llx is not known\n",
				  at,
				  (unsigned long long)texel);
	}

	/* Succeeded: the write is read and each of its descriptors applied. */
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

		/* Command buffers that bound it let go of it, then it is freed. */
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
 * Frees descriptor sets: vkFreeDescriptorSets.
 *
 * The command is [device][pool][count][count]{identity} and the reply
 * [result].  Each set of the pool named is withdrawn: command buffers that
 * bound it let go of it, then it is freed.  An unknown set, or one of
 * another pool, is not an error.
 */
int
drv_i915_gfx_free_dsets(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_gfx_dset *dset;
	void *pool;
	uint64_t pool_id;
	uint64_t identity;
	uint64_t count;
	uint64_t index;

	/* The device, the pool and the count twice. */
	(void)drv_i915_wire_read_u64(reader);
	pool_id = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > (uint64_t)(reader->size - reader->offset))
		return EINVAL;

	/* Each set of the pool. */
	pool = drv_i915_object_lookup(session, I915_VK_OBJ_DESCRIPTOR_POOL, pool_id);
	for (index = 0U; index < count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			return EINVAL;
		dset = drv_i915_object_lookup(session, I915_VK_OBJ_DESCRIPTOR_SET, identity);
		if (dset == NULL || pool == NULL || dset->pool != pool)
			continue;
		drv_i915_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, identity);
		drv_i915_gfx_forget(session, I915_VK_OBJ_DESCRIPTOR_SET, dset);
		kern_free(dset);
	}

	/* Succeeded: the reply carries VK_SUCCESS. */
	drv_i915_wire_reply_u32(reply, 0U);
	return 0;
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
