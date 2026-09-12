/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Owns local Vulkan identities without imposing a fixed object count.
 */

#include <stdlib.h>
#include <string.h>
#include "internal.h"

/* Protects parent lists and identities for the lifetime of this library. */
static pthread_mutex_t vulkan_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Reserves zero for NULL and never recycles a renderer identity in this process. */
static uint64_t vulkan_next_wire_id = 1;

static void vulkan_registry_lock(void);
static void vulkan_registry_unlock(void);

/*
 * Allocates host storage with the caller's specified allocation policy.
 */
void *
vulkan_allocate(
	const struct vulkan_allocator *allocator,
	size_t bytes,
	size_t alignment,
	VkSystemAllocationScope scope)
{
	void *allocation;
	uintptr_t address;
	size_t extra;

	/* Refuses an alignment that cannot describe a power-of-two boundary. */
	if (alignment == 0)
		return NULL;

	/* Refuses inconsistent alignment before calling an application allocator. */
	if ((alignment & (alignment - 1)) != 0)
		return NULL;

	/* Preserves the application's callback arguments and returned address. */
	if (allocator != NULL && allocator->has_callbacks) {
		/* A valid callback pair keeps creation and destruction compatible. */
		if (allocator->callbacks.pfnAllocation == NULL)
			return NULL;

		/* Refuses storage that could not later be returned to its owner. */
		if (allocator->callbacks.pfnFree == NULL)
			return NULL;

		/* Requests exactly this allocation scope from the application. */
		allocation = allocator->callbacks.pfnAllocation(allocator->callbacks.pUserData, bytes, alignment, scope);
		if (allocation == NULL)
			return NULL;

		/* Succeeded: preserves the callback's aligned allocation. */
		return allocation;
	}

	/* Leaves room before default allocations to retain the malloc address. */
	if (alignment < sizeof(void *))
		alignment = sizeof(void *);

	/* Refuses overflow in the alignment padding and ownership prefix. */
	if (alignment > SIZE_MAX - sizeof(void *))
		return NULL;

	/* Measures the prefix independently of the caller's payload length. */
	extra = alignment - 1 + sizeof(void *);
	if (bytes > SIZE_MAX - extra)
		return NULL;

	/* Allocates backing before publishing an aligned view of it. */
	allocation = malloc(bytes + extra);
	if (allocation == NULL)
		return NULL;

	/* Retains the original pointer for the corresponding default free. */
	address = (uintptr_t)allocation + sizeof(void *);
	address = (address + alignment - 1) & ~(uintptr_t)(alignment - 1);
	((void **)address)[-1] = allocation;

	/* Succeeded: returns the requested aligned payload. */
	return (void *)address;
}

/*
 * Returns host storage through the allocator that originally supplied it.
 */
void
vulkan_free(
	const struct vulkan_allocator *allocator,
	void *allocation)
{
	/* Accepts the null allocations allowed by Vulkan destruction paths. */
	if (allocation == NULL)
		return;

	/* Returns callback storage without interpreting a private prefix. */
	if (allocator != NULL && allocator->has_callbacks) {
		allocator->callbacks.pfnFree(allocator->callbacks.pUserData, allocation);

		/* Succeeded: the application has reclaimed its own allocation. */
		return;
	}

	/* Reclaims the backing retained before the default aligned payload. */
	free(((void **)allocation)[-1]);

	/* Succeeded: the default allocation is no longer owned by the library. */
	return;
}

/*
 * Creates an unpublished local object with its own saved allocator.
 */
VkResult
vulkan_object_alloc(
	size_t bytes,
	size_t alignment,
	enum vulkan_object_kind kind,
	struct vulkan_object *parent,
	struct vulkan_context *context,
	const VkAllocationCallbacks *allocator,
	VkSystemAllocationScope scope,
	struct vulkan_object **result)
{
	struct vulkan_allocator policy;
	struct vulkan_object *object;

	/* Requires enough storage for the common ownership record. */
	if (bytes < sizeof(*object) || result == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Captures callback state before the application's allocation may run. */
	memset(&policy, 0, sizeof(policy));
	if (parent != NULL)
		policy = parent->allocator;

	/* Uses a more specific policy supplied by this creation call. */
	if (allocator != NULL) {
		policy.callbacks = *allocator;
		policy.has_callbacks = VK_TRUE;
	}

	/* Allocates exactly one local object without creating a remote object. */
	object = vulkan_allocate(&policy, bytes, alignment, scope);
	if (object == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Initializes all subtype storage before assigning common ownership. */
	memset(object, 0, bytes);
	object->kind = kind;
	object->parent = parent;
	object->context = context;
	object->allocator = policy;
	object->scope = scope;

	/* Publishes only a fully initialized local allocation to this caller. */
	*result = object;

	/* Succeeded: the caller owns an unpublished local object. */
	return VK_SUCCESS;
}

/*
 * Destroys local storage after the caller has destroyed all owned children.
 */
void
vulkan_object_free(
	struct vulkan_object *object)
{
	struct vulkan_allocator policy;

	/* Accepts absent objects in partial-creation rollback. */
	if (object == NULL)
		return;

	/* Withdraws parent visibility before releasing callback-owned storage. */
	vulkan_object_unpublish(object);
	policy = object->allocator;
	vulkan_free(&policy, object);

	/* Succeeded: the local identity no longer owns storage. */
	return;
}

/*
 * Assigns a renderer identity that cannot alias a prior object.
 */
VkResult
vulkan_object_reserve_id(
	struct vulkan_object *object)
{
	/* Requires a renderer-backed object without a previous identity. */
	if (object == NULL || object->wire_id != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Reserves identities across all live and future library contexts. */
	vulkan_registry_lock();

	/* Exhaustion leaves zero reserved permanently for the null handle. */
	if (vulkan_next_wire_id == 0) {
		vulkan_registry_unlock();
		return VK_ERROR_TOO_MANY_OBJECTS;
	}

	/* Advances the generation even if later remote creation fails. */
	object->wire_id = vulkan_next_wire_id;
	vulkan_next_wire_id++;

	vulkan_registry_unlock();

	/* Succeeded: this object's wire identity will never be reassigned. */
	return VK_SUCCESS;
}

/*
 * Links a fully created object to the parent that owns its lifetime.
 */
VkResult
vulkan_object_publish(
	struct vulkan_object *object)
{
	/* Requires a local object prepared by the common allocator. */
	if (object == NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Makes parent enumeration and publication one registry transaction. */
	vulkan_registry_lock();

	/* Prevents a repeated publication from creating a cyclic child list. */
	if (object->published) {
		vulkan_registry_unlock();
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Links ordinary children while keeping root objects parentless. */
	if (object->parent != NULL) {
		object->next_sibling = object->parent->first_child;
		object->parent->first_child = object;
	}

	/* Publication means the parent's lifetime traversal can find this object. */
	object->published = VK_TRUE;

	vulkan_registry_unlock();

	/* Succeeded: parent teardown can now find the completed object. */
	return VK_SUCCESS;
}

/*
 * Removes parent visibility before a local object's storage is reclaimed.
 */
void
vulkan_object_unpublish(
	struct vulkan_object *object)
{
	struct vulkan_object **link;

	/* Accepts objects absent from a partial creation path. */
	if (object == NULL)
		return;

	/* Serializes child removal with publication by other host threads. */
	vulkan_registry_lock();

	/* Unpublished allocations have never transferred ownership to a parent. */
	if (!object->published) {
		vulkan_registry_unlock();
		return;
	}

	/* Removes the exact child without disturbing siblings or their identities. */
	if (object->parent != NULL) {
		link = &object->parent->first_child;

		/* Finds the parent's link that owns this child's list membership. */
		while (*link != NULL) {
			/* Withdraws the object once its owning link is reached. */
			if (*link == object) {
				*link = object->next_sibling;
				break;
			}

			/* Advances through other live children of the same parent. */
			link = &(*link)->next_sibling;
		}
	}

	/* Marks the local allocation as private to its destruction caller. */
	object->next_sibling = NULL;
	object->published = VK_FALSE;

	vulkan_registry_unlock();

	/* Succeeded: parent traversal no longer retains this local object. */
	return;
}

/*
 * Returns the renderer identity associated with a non-null local object.
 */
uint64_t
vulkan_object_wire_id(
	const struct vulkan_object *object)
{
	/* Preserves the wire protocol's null-handle representation. */
	if (object == NULL)
		return 0;

	/* Succeeded: returns the identity reserved for this object. */
	return object->wire_id;
}

/*
 * Resolves the ABI-neutral representation of a non-dispatchable handle.
 */
struct vulkan_object *
vulkan_nondispatchable_object(
	uint64_t handle)
{
	/* Succeeded: converts the standard handle to its local ownership record. */
	return (struct vulkan_object *)(uintptr_t)handle;
}

/*
 * Encodes a local ownership record as a non-dispatchable handle.
 */
uint64_t
vulkan_nondispatchable_handle(
	struct vulkan_object *object)
{
	/* Succeeded: preserves pointer bits in the standard 64-bit handle carrier. */
	return (uint64_t)(uintptr_t)object;
}

/*
 * Resolves the local instance already represented by a dispatchable handle.
 */
struct VkInstance_T *
vulkan_instance(
	VkInstance instance)
{
	/* Succeeded: preserves the standard opaque instance identity. */
	return instance;
}

/*
 * Resolves the local physical device owned by an instance.
 */
struct VkPhysicalDevice_T *
vulkan_physical_device(
	VkPhysicalDevice physical)
{
	/* Succeeded: preserves the standard opaque physical-device identity. */
	return physical;
}

/*
 * Resolves the local logical device represented by its dispatchable handle.
 */
struct VkDevice_T *
vulkan_device(
	VkDevice device)
{
	/* Succeeded: preserves the standard opaque device identity. */
	return device;
}

/*
 * Resolves the queue identity retained by its creating device.
 */
struct VkQueue_T *
vulkan_queue(
	VkQueue queue)
{
	/* Succeeded: preserves the standard opaque queue identity. */
	return queue;
}

/*
 * Resolves the image metadata behind a standard non-dispatchable handle.
 */
struct vulkan_image *
vulkan_image(
	VkImage image)
{
	/* Succeeded: preserves the local image object's address on either ABI. */
	return (struct vulkan_image *)(uintptr_t)image;
}

/*
 * Resolves the allocation metadata behind a standard memory handle.
 */
struct vulkan_memory *
vulkan_memory(
	VkDeviceMemory memory)
{
	/* Succeeded: preserves the local memory object's address on either ABI. */
	return (struct vulkan_memory *)(uintptr_t)memory;
}

/*
 * Frees an object through the compatible allocator supplied to destruction.
 */
void
vulkan_object_free_with_allocator(
	struct vulkan_object *object,
	const VkAllocationCallbacks *allocator)
{
	/* Accepts the absent handles permitted by destruction commands. */
	if (object == NULL)
		return;

	/* Retains inherited policy when this command has no object-specific callbacks. */
	if (allocator != NULL) {
		object->allocator.callbacks = *allocator;
		object->allocator.has_callbacks = VK_TRUE;
	}

	/* Uses the current callback data after withdrawing parent visibility. */
	vulkan_object_free(object);

	/* Succeeded: no library allocation retains the destroyed local object. */
	return;
}

/*
 * Completes a single ordinary creation whose typed input is already encoded.
 */
VkResult
vulkan_object_create_complete(
	struct VkDevice_T *device,
	struct vulkan_object *object,
	struct vulkan_writer *writer,
	uint32_t destroy_opcode)
{
	struct vulkan_reader reader;
	VkResult status;
	VkResult cleanup;
	uint64_t identity;
	VkBool32 present;

	/* Keeps application allocation callbacks local and supplies a fresh output identity. */
	vulkan_write_u64(writer, 0);
	vulkan_write_u64(writer, 1);
	vulkan_write_u64(writer, object->wire_id);
	status = vulkan_command_execute(device->object.context, writer, 24, &reader, VK_TRUE);
	if (status != VK_SUCCESS) {
		status = vulkan_reply_finish(device->object.context, &reader, status);
		return status;
	}

	/* Rejects a missing or foreign object identity before exposing a public handle. */
	present = vulkan_reply_pointer(&reader);
	identity = vulkan_read_u64(&reader);
	if (!present || identity != object->wire_id)
		reader.error = VK_ERROR_DEVICE_LOST;

	/* Makes a malformed creation reply terminal for its remote namespace. */
	status = vulkan_reply_finish(device->object.context, &reader, VK_SUCCESS);
	if (status != VK_SUCCESS)
		return status;

	/* Publishes ownership only after both local and renderer creation succeeded. */
	status = vulkan_object_publish(object);
	if (status != VK_SUCCESS) {
		cleanup = vulkan_object_destroy_remote(device, object, destroy_opcode);
		(void)cleanup;
		return status;
	}

	/* Succeeded: the parent owns one complete local and renderer object. */
	return VK_SUCCESS;
}

/*
 * Destroys an ordinary renderer object without changing local ownership.
 */
VkResult
vulkan_object_destroy_remote(
	struct VkDevice_T *device,
	struct vulkan_object *object,
	uint32_t opcode)
{
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;

	/* A null Vulkan handle has no renderer lifetime to consume. */
	if (object == NULL)
		return VK_SUCCESS;

	/* Uses the object's effective allocator for this destruction command. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, opcode);
	vulkan_write_u64(&writer, device->object.wire_id);
	vulkan_write_u64(&writer, object->wire_id);
	vulkan_write_u64(&writer, 0);
	status = vulkan_command_execute(device->object.context, &writer, 4, &reader, VK_FALSE);
	vulkan_writer_finish(&writer);
	status = vulkan_reply_finish(device->object.context, &reader, status);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: local cleanup may release the object's remaining host storage. */
	return VK_SUCCESS;
}

/* Serializes registry invariants that have no application-visible failure mode. */
static void
vulkan_registry_lock(
	void)
{
	int error;

	/* A statically initialized live mutex cannot legitimately reject its owner. */
	error = pthread_mutex_lock(&vulkan_registry_mutex);
	if (error != 0)
		abort();

	/* Succeeded: the caller exclusively owns the registry transaction. */
	return;
}

/* Completes a registry transaction before callbacks or wire operations run. */
static void
vulkan_registry_unlock(
	void)
{
	int error;

	/* Refuses to continue after an internal mutex-ownership violation. */
	error = pthread_mutex_unlock(&vulkan_registry_mutex);
	if (error != 0)
		abort();

	/* Succeeded: other registry users may now proceed. */
	return;
}
