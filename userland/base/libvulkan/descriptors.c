/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Owns descriptor layouts, pools and sets with independent layout snapshots.
 */

#include "internal.h"
#include "codec.h"

#include <string.h>

/* Binding metadata survives the source layout only through a set's own copy. */
struct descriptor_binding {
	uint32_t number;
	uint32_t count;
	VkDescriptorType type;
	VkBool32 immutable;
};

/* A layout's single callback allocation includes its binding array. */
struct descriptor_layout {
	struct vulkan_object object;
	uint32_t count;
	struct descriptor_binding *bindings;
};

/* A pool owns all its descriptor-set children through the common object list. */
struct descriptor_pool {
	struct vulkan_object object;
	VkDescriptorPoolCreateFlags flags;
};

/* A set never dereferences its source layout after successful allocation. */
struct descriptor_set {
	struct vulkan_object object;
	uint32_t count;
	struct descriptor_binding *bindings;
};

static void descriptor_pool_clear(struct descriptor_pool *pool, const VkAllocationCallbacks *allocator);
static VkResult descriptor_set_prepare(struct VkDevice_T *device, struct descriptor_pool *pool, struct descriptor_layout *layout, struct descriptor_set **result);
static VkBool32 descriptor_immutable(struct descriptor_set *set, uint32_t binding, uint64_t element);
static void descriptor_write(struct vulkan_writer *writer, const VkWriteDescriptorSet *write);

/*
 * Creates a native layout and a compact snapshot of immutable-sampler semantics.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorSetLayout(
	VkDevice device,
	const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkDescriptorSetLayout *pSetLayout)
{
	struct VkDevice_T *owner;
	struct vulkan_object *object;
	struct descriptor_layout *layout;
	struct vulkan_writer writer;
	const VkDescriptorSetLayoutBinding *binding;
	size_t bytes;
	size_t maximum;
	uint32_t index;
	VkResult status;

	/* Reserve one ownership block before the renderer can accept the layout. */
	owner = vulkan_device(device);
	*pSetLayout = VK_NULL_HANDLE;
	maximum = (SIZE_MAX - sizeof(*layout)) / sizeof(*layout->bindings);
	if ((size_t)pCreateInfo->bindingCount > maximum)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Allocate layout metadata and its binding snapshot as one callback-owned object. */
	bytes = sizeof(*layout) + (size_t)pCreateInfo->bindingCount * sizeof(*layout->bindings);
	status = vulkan_object_alloc(
		bytes,
		sizeof(uint64_t),
		VULKAN_OBJECT_DESCRIPTOR_SET_LAYOUT,
		&owner->object,
		owner->object.context,
		pAllocator,
		VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
		&object);
	if (status != VK_SUCCESS)
		return status;

	/* The inline binding array has exactly the lifetime of this layout handle. */
	layout = (struct descriptor_layout *)object;
	layout->count = pCreateInfo->bindingCount;
	layout->bindings = (struct descriptor_binding *)(layout + 1);

	/* Only sampler bindings interpret pImmutableSamplers; ignored pointers stay untouched. */
	for (index = 0; index < layout->count; index++) {
		binding = &pCreateInfo->pBindings[index];
		layout->bindings[index].number = binding->binding;
		layout->bindings[index].count = binding->descriptorCount;
		layout->bindings[index].type = binding->descriptorType;
		layout->bindings[index].immutable = VK_FALSE;

		/* Only the two sampler descriptor types can capture immutable samplers. */
		if (binding->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
		    binding->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
			/* The pointer's presence selects immutable state without dereferencing it. */
			if (binding->pImmutableSamplers != NULL)
				layout->bindings[index].immutable = VK_TRUE;
		}
	}

	/* Reserve a wire identity before any native creation can consume this object. */
	status = vulkan_object_reserve_id(object);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);

		/* Preserve the failure after returning this unpublished allocation. */
		return status;
	}

	/* Create native state with the effective object allocator owning command storage. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkCreateDescriptorSetLayout);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_pointer(&writer, pCreateInfo);
	vulkan_encode_VkDescriptorSetLayoutCreateInfo(&writer, pCreateInfo);
	status = vulkan_object_create_complete(owner, object, &writer, VULKAN_OPCODE_vkDestroyDescriptorSetLayout);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);

		/* Preserve the failure after returning this unpublished allocation. */
		return status;
	}

	/* Succeeded: future sets copy metadata without extending this allocation's lifetime. */
	*pSetLayout = (VkDescriptorSetLayout)(uintptr_t)vulkan_nondispatchable_handle(object);
	return VK_SUCCESS;
}

/*
 * Destroys the layout while already allocated sets keep their independent snapshots.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyDescriptorSetLayout(
	VkDevice device,
	VkDescriptorSetLayout descriptorSetLayout,
	const VkAllocationCallbacks *pAllocator)
{
	struct VkDevice_T *owner;
	struct vulkan_object *object;
	VkResult status;

	/* Null layouts have no native or callback ownership. */
	if (descriptorSetLayout == VK_NULL_HANDLE)
		return;

	/* Native destruction does not affect snapshots already owned by allocated sets. */
	owner = vulkan_device(device);
	object = vulkan_nondispatchable_object((uint64_t)(uintptr_t)descriptorSetLayout);

	/* The current compatible callbacks own both command temporaries and the final layout release. */
	if (pAllocator != NULL) {
		object->allocator.callbacks = *pAllocator;
		object->allocator.has_callbacks = VK_TRUE;
	}

	/* A failed void destruction leaves uncertain native state owned by terminal context cleanup. */
	status = vulkan_object_destroy_remote(owner, object, VULKAN_OPCODE_vkDestroyDescriptorSetLayout);
	if (status != VK_SUCCESS)
		__atomic_store_n(&owner->object.context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);

	/* The public destroy consumes local storage even when the native namespace is lost. */
	vulkan_object_free_with_allocator(object, pAllocator);

	/* Succeeded: this layout retains neither native state nor callback storage. */
	return;
}

/*
 * Creates a pool whose local children follow its native reset and destroy operations.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorPool(
	VkDevice device,
	const VkDescriptorPoolCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkDescriptorPool *pDescriptorPool)
{
	struct VkDevice_T *owner;
	struct descriptor_pool *pool;
	struct vulkan_object *object;
	struct vulkan_writer writer;
	VkResult status;

	/* Keeps the pool unpublished until its remote identity has been checked. */
	owner = vulkan_device(device);
	*pDescriptorPool = VK_NULL_HANDLE;
	status = vulkan_object_alloc(
		sizeof(*pool),
		sizeof(uint64_t),
		VULKAN_OBJECT_DESCRIPTOR_POOL,
		&owner->object,
		owner->object.context,
		pAllocator,
		VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
		&object);
	if (status != VK_SUCCESS)
		return status;

	/* The local pool mirrors native flags and owns all later set metadata. */
	pool = (struct descriptor_pool *)object;
	pool->flags = pCreateInfo->flags;

	/* Reserve a wire identity before any native creation can consume this object. */
	status = vulkan_object_reserve_id(object);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);

		/* Preserve the failure after returning this unpublished allocation. */
		return status;
	}

	/* Create native state with the effective object allocator owning command storage. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkCreateDescriptorPool);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_pointer(&writer, pCreateInfo);
	vulkan_encode_VkDescriptorPoolCreateInfo(&writer, pCreateInfo);
	status = vulkan_object_create_complete(owner, object, &writer, VULKAN_OPCODE_vkDestroyDescriptorPool);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);

		/* Preserve the failure after returning this unpublished allocation. */
		return status;
	}

	/* Succeeded: the caller receives a pool with checked native identity. */
	*pDescriptorPool = (VkDescriptorPool)(uintptr_t)vulkan_nondispatchable_handle(object);
	return VK_SUCCESS;
}

/*
 * Native pool destruction consumes every set before callback-owned storage is freed.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyDescriptorPool(
	VkDevice device,
	VkDescriptorPool descriptorPool,
	const VkAllocationCallbacks *pAllocator)
{
	struct VkDevice_T *owner;
	struct descriptor_pool *pool;
	VkResult status;

	/* Destruction is externally synchronized against this pool and all its children. */
	if (descriptorPool == VK_NULL_HANDLE)
		return;

	/* Native pool destruction consumes all native sets in a single operation. */
	owner = vulkan_device(device);
	pool = (struct descriptor_pool *)vulkan_nondispatchable_object((uint64_t)(uintptr_t)descriptorPool);

	/* The supplied compatible policy also applies to this command's temporary allocations. */
	if (pAllocator != NULL) {
		pool->object.allocator.callbacks = *pAllocator;
		pool->object.allocator.has_callbacks = VK_TRUE;
	}

	/* A failed native destroy cannot leave apparently usable local handles into an uncertain pool. */
	status = vulkan_object_destroy_remote(owner, &pool->object, VULKAN_OPCODE_vkDestroyDescriptorPool);
	if (status != VK_SUCCESS)
		__atomic_store_n(&owner->object.context->error, VK_ERROR_DEVICE_LOST, __ATOMIC_RELEASE);

	/* Consume local sets through the compatible allocator supplied at destruction. */
	descriptor_pool_clear(pool, pAllocator);
	vulkan_object_free_with_allocator(&pool->object, pAllocator);

	/* Succeeded: no child metadata retains the destroyed pool. */
	return;
}

/*
 * Resets local set ownership only after the native pool reset succeeds.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkResetDescriptorPool(
	VkDevice device,
	VkDescriptorPool descriptorPool,
	VkDescriptorPoolResetFlags flags)
{
	struct VkDevice_T *owner;
	struct descriptor_pool *pool;
	struct vulkan_writer writer;
	struct vulkan_reader reply;
	VkResult status;

	/* A failed reset leaves all earlier application handles and metadata intact. */
	owner = vulkan_device(device);
	pool = (struct descriptor_pool *)vulkan_nondispatchable_object((uint64_t)(uintptr_t)descriptorPool);
	vulkan_writer_init_for_object(&writer, &pool->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkResetDescriptorPool);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, pool->object.wire_id);
	vulkan_write_u32(&writer, flags);
	status = vulkan_command_execute(owner->object.context, &writer, 8U, &reply, VK_TRUE);
	vulkan_writer_finish(&writer);
	status = vulkan_reply_finish(owner->object.context, &reply, status);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: native reset permits consuming every local set handle. */
	descriptor_pool_clear(pool, NULL);
	return VK_SUCCESS;
}

/*
 * Allocates a complete set batch with local rollback before exposing any native result.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateDescriptorSets(
	VkDevice device,
	const VkDescriptorSetAllocateInfo *pAllocateInfo,
	VkDescriptorSet *pDescriptorSets)
{
	struct VkDevice_T *owner;
	struct descriptor_pool *pool;
	struct descriptor_layout *layout;
	struct descriptor_set *set;
	struct vulkan_writer writer;
	struct vulkan_reader reply;
	uint32_t index;
	uint32_t prepared;
	uint64_t count;
	uint64_t identity;
	size_t reply_bytes;
	size_t maximum;
	VkResult status;

	/* Output nulling also makes cleanup of any partial local batch deterministic. */
	owner = vulkan_device(device);
	pool = (struct descriptor_pool *)vulkan_nondispatchable_object((uint64_t)(uintptr_t)pAllocateInfo->descriptorPool);
	for (index = 0; index < pAllocateInfo->descriptorSetCount; index++)
		pDescriptorSets[index] = VK_NULL_HANDLE;

	/* The complete identity reply must fit in one addressable allocation. */
	maximum = (SIZE_MAX - 16U) / sizeof(uint64_t);
	if ((size_t)pAllocateInfo->descriptorSetCount > maximum)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Prepare all local identities first so native success cannot be followed by host OOM. */
	reply_bytes = 16U + (size_t)pAllocateInfo->descriptorSetCount * sizeof(uint64_t);
	prepared = 0;
	status = VK_SUCCESS;
	for (index = 0; index < pAllocateInfo->descriptorSetCount; index++) {
		/* Copy the source layout while it is still an externally synchronized live handle. */
		layout = (struct descriptor_layout *)vulkan_nondispatchable_object((uint64_t)(uintptr_t)pAllocateInfo->pSetLayouts[index]);
		status = descriptor_set_prepare(owner, pool, layout, &set);
		if (status != VK_SUCCESS)
			break;

		/* Track every prepared set so a later failure can null and release the whole batch. */
		pDescriptorSets[index] = (VkDescriptorSet)(uintptr_t)vulkan_nondispatchable_handle(&set->object);
		prepared++;
	}

	/* Roll back only local preparation when no native request has been emitted. */
	if (status != VK_SUCCESS)
		goto finished;

	/* Local publication cannot fail after native allocation; pool access is externally synchronized. */
	vulkan_writer_init_for_object(&writer, &pool->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkAllocateDescriptorSets);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_pointer(&writer, pAllocateInfo);
	vulkan_encode_VkDescriptorSetAllocateInfo(&writer, pAllocateInfo);
	vulkan_write_u64(&writer, pAllocateInfo->descriptorSetCount);

	/* Seed every native output slot with its already reserved local identity. */
	for (index = 0; index < prepared; index++)
		vulkan_encode_handle(&writer, (uint64_t)(uintptr_t)pDescriptorSets[index]);

	/* The renderer must accept the entire batch before any set is usable. */
	status = vulkan_command_execute(owner->object.context, &writer, reply_bytes, &reply, VK_TRUE);
	vulkan_writer_finish(&writer);

	/* Verify the exact batch length before interpreting renderer output identities. */
	if (status == VK_SUCCESS) {
		count = vulkan_read_u64(&reply);
		if (count != prepared)
			reply.error = VK_ERROR_DEVICE_LOST;

		/* A mismatched identity invalidates the namespace without publishing foreign handles. */
		for (index = 0; index < prepared && reply.error == VK_SUCCESS; index++) {
			set = (struct descriptor_set *)vulkan_nondispatchable_object((uint64_t)(uintptr_t)pDescriptorSets[index]);
			identity = vulkan_read_u64(&reply);
			if (identity != set->object.wire_id)
				reply.error = VK_ERROR_DEVICE_LOST;
		}
	}

	/* Retire reply storage and publish any malformed response as a context-wide loss. */
	status = vulkan_reply_finish(owner->object.context, &reply, status);

finished:
	/* Native failure allocates no sets; lost contexts retire uncertain native IDs at close. */
	if (status != VK_SUCCESS) {
		/* Return every prepared handle and restore the required all-null failure outputs. */
		for (index = 0; index < prepared; index++) {
			set = (struct descriptor_set *)vulkan_nondispatchable_object((uint64_t)(uintptr_t)pDescriptorSets[index]);
			vulkan_object_free(&set->object);
			pDescriptorSets[index] = VK_NULL_HANDLE;
		}

		/* Report the failed batch with no local child ownership left behind. */
		return status;
	}

	/* Succeeded: every returned set has both native state and an independent layout snapshot. */
	return VK_SUCCESS;
}

/*
 * Frees only the selected set handles after the renderer accepts the whole request.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkFreeDescriptorSets(
	VkDevice device,
	VkDescriptorPool descriptorPool,
	uint32_t descriptorSetCount,
	const VkDescriptorSet *pDescriptorSets)
{
	struct VkDevice_T *owner;
	struct descriptor_pool *pool;
	struct vulkan_object *object;
	struct vulkan_writer writer;
	struct vulkan_reader reply;
	uint32_t index;
	VkResult status;

	/* The application externally synchronizes this pool and supplies live owned sets. */
	owner = vulkan_device(device);
	pool = (struct descriptor_pool *)vulkan_nondispatchable_object((uint64_t)(uintptr_t)descriptorPool);
	vulkan_writer_init_for_object(&writer, &pool->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkFreeDescriptorSets);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, pool->object.wire_id);
	vulkan_write_u32(&writer, descriptorSetCount);
	vulkan_write_u64(&writer, descriptorSetCount);

	/* Name only the sets selected for this pool operation. */
	for (index = 0; index < descriptorSetCount; index++)
		vulkan_encode_handle(&writer, (uint64_t)(uintptr_t)pDescriptorSets[index]);

	/* Consume local handles only after the native free request has been accepted. */
	status = vulkan_command_execute(owner->object.context, &writer, 8U, &reply, VK_TRUE);
	vulkan_writer_finish(&writer);
	status = vulkan_reply_finish(owner->object.context, &reply, status);
	if (status != VK_SUCCESS)
		return status;

	/* Native success consumes each selected local identity and binding snapshot. */
	for (index = 0; index < descriptorSetCount; index++) {
		object = vulkan_nondispatchable_object((uint64_t)(uintptr_t)pDescriptorSets[index]);
		vulkan_object_free(object);
	}

	/* Succeeded: unselected sets retain their handles and storage. */
	return VK_SUCCESS;
}

/*
 * Encodes writes before copies, selecting only fields relevant to each descriptor type.
 */
VKAPI_ATTR void VKAPI_CALL
vkUpdateDescriptorSets(
	VkDevice device,
	uint32_t descriptorWriteCount,
	const VkWriteDescriptorSet *pDescriptorWrites,
	uint32_t descriptorCopyCount,
	const VkCopyDescriptorSet *pDescriptorCopies)
{
	struct VkDevice_T *owner;
	struct vulkan_writer writer;
	struct vulkan_reader reply;
	uint32_t index;
	VkResult status;

	/* App synchronization is per updated set; independent sets share only a short wire transaction. */
	owner = vulkan_device(device);
	vulkan_writer_init_for_object(&writer, &owner->object);
	vulkan_command_begin(&writer, VULKAN_OPCODE_vkUpdateDescriptorSets);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u32(&writer, descriptorWriteCount);
	vulkan_write_u64(&writer, descriptorWriteCount);

	/* Descriptor writes precede copies so this batch preserves Vulkan update ordering. */
	for (index = 0; index < descriptorWriteCount && writer.error == VK_SUCCESS; index++)
		descriptor_write(&writer, &pDescriptorWrites[index]);

	/* Copy descriptors only after every preceding write has been encoded. */
	vulkan_write_u32(&writer, descriptorCopyCount);
	vulkan_write_u64(&writer, descriptorCopyCount);
	for (index = 0; index < descriptorCopyCount && writer.error == VK_SUCCESS; index++)
		vulkan_encode_VkCopyDescriptorSet(&writer, &pDescriptorCopies[index]);

	/* Submit the complete update without exposing application pointers to the renderer. */
	status = vulkan_command_execute(owner->object.context, &writer, 4U, &reply, VK_FALSE);
	vulkan_writer_finish(&writer);
	status = vulkan_reply_finish(owner->object.context, &reply, status);
	if (status != VK_SUCCESS)
		return;

	/* Succeeded: the native descriptor state reflects the complete ordered update. */
	return;
}

/* Frees all pool children after native reset/destroy has invalidated their handles. */
static void
descriptor_pool_clear(
	struct descriptor_pool *pool,
	const VkAllocationCallbacks *allocator)
{
	struct vulkan_object *object;

	/* No other thread may allocate/free through an externally synchronized pool here. */
	while (pool->object.first_child != NULL) {
		object = pool->object.first_child;
		vulkan_object_free_with_allocator(object, allocator);
	}

	/* Succeeded: the pool retains no descriptor-set child metadata. */
	return;
}

/* Copies the layout into a set allocation so destroying that layout frees all its storage. */
static VkResult
descriptor_set_prepare(
	struct VkDevice_T *device,
	struct descriptor_pool *pool,
	struct descriptor_layout *layout,
	struct descriptor_set **result)
{
	struct descriptor_set *set;
	struct vulkan_object *object;
	size_t bytes;
	VkResult status;

	/* The original layout allocation proved this identical-size binding array representable. */
	bytes = sizeof(*set) + (size_t)layout->count * sizeof(*set->bindings);
	status = vulkan_object_alloc(
		bytes,
		sizeof(uint64_t),
		VULKAN_OBJECT_DESCRIPTOR_SET,
		&pool->object,
		device->object.context,
		NULL,
		VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
		&object);
	if (status != VK_SUCCESS)
		return status;

	/* Copy binding semantics into storage whose lifetime is independent of the source layout. */
	set = (struct descriptor_set *)object;
	set->count = layout->count;
	set->bindings = (struct descriptor_binding *)(set + 1);
	memcpy(set->bindings, layout->bindings, (size_t)layout->count * sizeof(*set->bindings));

	/* Reserve a wire identity before any native creation can consume this object. */
	status = vulkan_object_reserve_id(object);
	if (status == VK_SUCCESS)
		status = vulkan_object_publish(object);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);

		/* Preserve the failure after returning this unpublished allocation. */
		return status;
	}

	/* Succeeded: the caller can roll back this published local identity without native work. */
	*result = set;
	return VK_SUCCESS;
}

/* Follows consecutive binding storage, including empty entries, for one write element. */
static VkBool32
descriptor_immutable(
	struct descriptor_set *set,
	uint32_t binding,
	uint64_t element)
{
	const struct descriptor_binding *current;
	const struct descriptor_binding *next;
	uint32_t index;

	/* Layout declaration order need not match the numeric binding order. */
	current = NULL;
	for (index = 0; index < set->count; index++) {
		/* Start from the binding named by the application, regardless of declaration order. */
		if (set->bindings[index].number == binding) {
			current = &set->bindings[index];
			break;
		}
	}

	/* Descriptor writes may spill into consecutive bindings, skipping zero-count entries. */
	while (current != NULL) {
		/* The containing binding determines whether this element owns an immutable sampler. */
		if (element < current->count)
			return current->immutable;

		/* Find the next numeric binding after consuming this binding's storage. */
		element -= current->count;
		next = NULL;
		for (index = 0; index < set->count; index++) {
			/* Retain the smallest later binding while accepting unsorted source declarations. */
			if (set->bindings[index].number > current->number &&
			    (next == NULL ||
			     set->bindings[index].number < next->number))
				next = &set->bindings[index];
		}

		/* Continue the rollover using the next binding's own descriptor count. */
		current = next;
	}

	/* Succeeded: no stored binding supplies an immutable sampler for this element. */
	return VK_FALSE;
}

/* Encodes only active union-like descriptor fields; ignored pointers may be inaccessible. */
static void
descriptor_write(
	struct vulkan_writer *writer,
	const VkWriteDescriptorSet *write)
{
	struct descriptor_set *set;
	const VkDescriptorImageInfo *image;
	VkDescriptorType type;
	VkBool32 images;
	VkBool32 buffers;
	VkBool32 texels;
	VkBool32 immutable;
	uint32_t index;
	uint32_t count;
	VkImageLayout layout;

	/* Resolve local metadata before deciding whether any image-info field is relevant. */
	set = (struct descriptor_set *)vulkan_nondispatchable_object((uint64_t)(uintptr_t)write->dstSet);
	type = write->descriptorType;
	images = VK_FALSE;
	buffers = VK_FALSE;
	texels = VK_FALSE;

	/* The descriptor type selects exactly one payload family. */
	switch (type) {
	case VK_DESCRIPTOR_TYPE_SAMPLER:
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
	case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
	case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
		images = VK_TRUE;
		break;
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
		buffers = VK_TRUE;
		break;
	case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
	case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
		texels = VK_TRUE;
		break;
	default:
		/* Unknown descriptor kinds have no legal payload in the selected Vulkan API. */
		writer->error = VK_ERROR_INITIALIZATION_FAILED;
		return;
	}

	/* Encodes the write destination independently of its selected payload. */
	vulkan_write_u32(writer, write->sType);
	vulkan_write_u64(writer, 0);
	vulkan_write_u64(writer, set->object.wire_id);
	vulkan_write_u32(writer, write->dstBinding);
	vulkan_write_u32(writer, write->dstArrayElement);
	vulkan_write_u32(writer, write->descriptorCount);
	vulkan_write_u32(writer, type);

	/* Immutable sampler writes may ignore the entire image-info pointer. */
	count = 0;
	if (images)
		count = write->descriptorCount;
	vulkan_write_u64(writer, count);

	/* Encode only image descriptors selected by this write's type. */
	for (index = 0; index < count && writer->error == VK_SUCCESS; index++) {
		/* Immutable samplers come from the copied layout rather than application image info. */
		immutable = descriptor_immutable(set, write->dstBinding, (uint64_t)write->dstArrayElement + index);
		image = NULL;
		if (type != VK_DESCRIPTOR_TYPE_SAMPLER || !immutable)
			image = &write->pImageInfo[index];

		/* Only a non-immutable sampler-bearing type reads the application sampler handle. */
		if ((type == VK_DESCRIPTOR_TYPE_SAMPLER ||
		     type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) &&
		    !immutable) {
			vulkan_encode_handle(writer, (uint64_t)(uintptr_t)image->sampler);
		} else {
			vulkan_write_u64(writer, 0);
		}

		/* Sampler-only descriptors must not read the ignored image view or layout fields. */
		if (type == VK_DESCRIPTOR_TYPE_SAMPLER) {
			vulkan_write_u64(writer, 0);
			vulkan_write_u32(writer, VK_IMAGE_LAYOUT_UNDEFINED);
		} else {
			vulkan_encode_handle(writer, (uint64_t)(uintptr_t)image->imageView);
			layout = vulkan_wire_image_layout(image->imageLayout);
			vulkan_write_u32(writer, layout);
		}
	}

	/* Buffer descriptors interpret only their own selected payload array. */
	count = 0;
	if (buffers)
		count = write->descriptorCount;
	vulkan_write_u64(writer, count);

	/* Preserve native buffer handles and full-width offsets for each selected descriptor. */
	for (index = 0; index < count && writer->error == VK_SUCCESS; index++)
		vulkan_encode_VkDescriptorBufferInfo(writer, &write->pBufferInfo[index]);

	/* Texel buffers have a separate array that is absent for every other descriptor type. */
	count = 0;
	if (texels)
		count = write->descriptorCount;
	vulkan_write_u64(writer, count);

	/* Resolve only the selected texel-buffer views into their native identities. */
	for (index = 0; index < count && writer->error == VK_SUCCESS; index++)
		vulkan_encode_handle(writer, (uint64_t)(uintptr_t)write->pTexelBufferView[index]);

	/* Succeeded: the writer owns the descriptor payload or its precise encoding error. */
	return;
}
