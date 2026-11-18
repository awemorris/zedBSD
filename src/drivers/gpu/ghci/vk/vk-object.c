/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's side of a node session: its open and close, the table
 * of its Vulkan objects by kind and identity, what each object lets go
 * of when it is destroyed, the budget of host bytes its objects keep,
 * and the scratch a command decodes into.
 *
 * A VkDevice's objects go before the device; the device's GHCI session
 * closes last, so that the memory its mappings hold is unmapped first.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#include "vk.h"

/* The multiplier that spreads the identities over the buckets (2^64 / the golden ratio). */
#define OBJECT_HASH			0x9e3779b97f4a7c15ULL

static uint32_t bucket_of(enum ghci_vk_kind kind, uint64_t identity);
static void destroy_matching(struct ghci_vk_session *session, struct ghci_vk_device *device, int devices);
static void device_release(struct ghci_vk_session *session, struct ghci_vk_object *object);

/*
 * Opens the executor's side of a node session: the object table, the
 * arena, the job markers, and the session on the executor's list.
 * Returns 0, or ENOMEM with nothing left behind.
 */
int
ghci_vk_session_open(
	struct ghci_device *device,
	struct ghci_session *owner)
{
	struct ghci_vk_session *session;
	struct ghci_vk *vk;
	unsigned long irq;

	/* The record. */
	vk = device->vk;
	session = kern_calloc(1U, sizeof(*session));
	if (session == NULL)
		return ENOMEM;

	/* The scratch each command decodes its records into. */
	session->arena.base = kern_malloc(GHCI_VK_ARENA_BYTES);
	if (session->arena.base == NULL) {
		kern_free(session);
		return ENOMEM;
	}
	session->arena.size = GHCI_VK_ARENA_BYTES;
	session->device = device;
	session->owner = owner;
	(void)mutex_init(&session->hold_lock, LOCK_RANK_DEVICE, "ghci vk hold");
	ghci_jobs_init(&session->jobs);

	/* The service thread sees the session from here. */
	irq = spin_lock_irqsave(&device->irq_lock);

	session->next = vk->sessions;
	vk->sessions = session;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: the node session runs commands. */
	owner->vk = session;
	return 0;
}

/*
 * Closes the executor's side of a node session; it cannot fail.  Its
 * queued completions end first (the core drained them; any left end
 * now), then every object goes, the devices last.
 */
void
ghci_vk_session_close(
	struct ghci_device *device,
	struct ghci_session *owner)
{
	struct ghci_vk_session *session;
	struct ghci_vk_session **position;
	struct ghci_vk *vk;
	unsigned long irq;

	/* The session's side. */
	vk = device->vk;
	session = owner->vk;

	/* Every completion of the session ends. */
	ghci_vk_drain(device, session);

	/* Every object of a device, then the devices with their GHCI sessions. */
	destroy_matching(session, NULL, 0);
	destroy_matching(session, NULL, 1);

	/* The session leaves the executor's list. */
	irq = spin_lock_irqsave(&device->irq_lock);

	position = &vk->sessions;
	while (*position != NULL && *position != session)
		position = &(*position)->next;
	if (*position == session)
		*position = session->next;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* The record and its scratch go. */
	kern_free(session->arena.base);
	kern_free(session);
	owner->vk = NULL;
}

/* Finds an object of a kind by its identity; NULL when the session has none. */
struct ghci_vk_object *
ghci_vk_object_find(
	struct ghci_vk_session *session,
	enum ghci_vk_kind kind,
	uint64_t identity)
{
	struct ghci_vk_object *object;
	uint32_t bucket;

	/* The null handle names nothing. */
	if (identity == 0U)
		return NULL;

	/* The bucket of the kind and identity, walked to the match. */
	bucket = bucket_of(kind, identity);
	for (object = session->buckets[bucket]; object != NULL; object = object->next) {
		if (object->kind == kind && object->identity == identity)
			return object;
	}

	/* Succeeded: none. */
	return NULL;
}

/*
 * Adds a cleared object of a kind with an identity libvulkan chose, made
 * on device.  Returns 0, EINVAL for an identity of that kind the session
 * already has (libvulkan never reuses one), EBUSY when the session keeps
 * as many objects as it may, or ENOMEM.
 */
int
ghci_vk_object_add(
	struct ghci_vk_session *session,
	enum ghci_vk_kind kind,
	uint64_t identity,
	struct ghci_vk_device *device,
	struct ghci_vk_object **result)
{
	struct ghci_vk_object *object;
	uint32_t bucket;

	/* A second object of one identity, or one past the bound. */
	object = ghci_vk_object_find(session, kind, identity);
	if (object != NULL || identity == 0U)
		return EINVAL;
	if (session->object_count >= GHCI_VK_OBJECTS_MAX)
		return EBUSY;

	/* The cleared record. */
	object = kern_calloc(1U, sizeof(*object));
	if (object == NULL)
		return ENOMEM;

	/* Its name, its device and its bucket. */
	object->kind = kind;
	object->identity = identity;
	object->device = device;
	bucket = bucket_of(kind, identity);
	object->next = session->buckets[bucket];
	session->buckets[bucket] = object;
	session->object_count++;

	/* Succeeded: the object is the session's. */
	*result = object;
	return 0;
}

/*
 * Destroys an object: it lets go of what it holds (a device first
 * destroys its own objects and closes its GHCI session) and leaves the
 * table.
 */
void
ghci_vk_object_destroy(
	struct ghci_vk_session *session,
	struct ghci_vk_object *object)
{
	struct ghci_vk_object **position;
	uint32_t bucket;

	/* What the object of each kind holds. */
	switch (object->kind) {
	case GHCI_VK_DEVICE:
		/* A device whose open failed has no record yet. */
		if (object->device != NULL)
			device_release(session, object);
		break;
	case GHCI_VK_MEMORY:
		ghci_vk_memory_release(session, object);
		break;
	case GHCI_VK_EVENT:
		/* Work placed before may still write the word. */
		ghci_vk_heap_retire(object->device, object->as.event.block, object->as.event.offset, NULL);
		break;
	case GHCI_VK_COMMAND_BUFFER:
		ghci_vk_commands_release(object);
		break;
	case GHCI_VK_SET_LAYOUT:
	case GHCI_VK_PIPELINE_LAYOUT:
	case GHCI_VK_DESCRIPTOR_SET:
		ghci_vk_layout_release(session, object);
		break;
	case GHCI_VK_SHADER_MODULE:
	case GHCI_VK_PIPELINE:
		ghci_vk_pipeline_release(session, object);
		break;
	case GHCI_VK_RENDER_PASS:
	case GHCI_VK_FRAMEBUFFER:
		ghci_vk_pass_release(object);
		break;
	case GHCI_VK_QUERY_POOL:
		/* Work placed before may still write the slots. */
		ghci_vk_heap_retire(object->device, object->as.query_pool.block, object->as.query_pool.offset, NULL);
		break;
	default:
		break;
	}

	/* What it took of the budget. */
	ghci_vk_budget_give(session, object->charged);

	/* The object leaves its bucket. */
	bucket = bucket_of(object->kind, object->identity);
	position = &session->buckets[bucket];
	while (*position != NULL && *position != object)
		position = &(*position)->next;
	if (*position == object)
		*position = object->next;
	session->object_count--;

	/* The record goes. */
	kern_free(object);
}

/* Destroys every object made on a device, but not the device itself. */
void
ghci_vk_objects_of_device(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device)
{
	/* The objects whose device it is. */
	destroy_matching(session, device, 0);
}

/*
 * Takes cleared bytes of the scratch for the current command; NULL when
 * the scratch is used up.
 */
void *
ghci_vk_arena_take(
	struct ghci_vk_session *session,
	size_t bytes)
{
	struct i915_wire_reader none;
	void *taken;

	/* The codec's own allocator (no array length), with a reader that only latches its own refusal. */
	kern_memset(&none, 0, sizeof(none));
	none.size = bytes;
	taken = (i915_vkc_array)(&none, &session->arena, 1U, bytes);

	/* Succeeded: the bytes, or NULL. */
	return taken;
}

/*
 * Takes the run of an array a record decoder reads for the current
 * command (the codec's allocator), with the array's element count in the
 * eight bytes of the scratch just before it, which
 * ghci_vk_decoded_count() reads.  The arena is always a session's.
 * Returns the array, or NULL with the reader's error latched.
 */
void *
ghci_vk_decode_array(
	struct i915_wire_reader *reader,
	struct i915_wire_arena *arena,
	uint64_t count,
	size_t element)
{
	uint64_t *header;
	uint8_t *array;

	/* The count's word, then the array right behind it (both runs are eight-byte aligned). */
	header = (i915_vkc_array)(reader, arena, 1U, sizeof(*header));
	if (header == NULL)
		return NULL;
	array = (i915_vkc_array)(reader, arena, count, element);
	if (array == NULL)
		return NULL;
	if (array != (uint8_t *)(header + 1)) {
		reader->error = 1;
		return NULL;
	}

	/* Succeeded: the array, its count kept. */
	*header = count;
	return array;
}

/*
 * Reports the element count a record decoder of the current command gave
 * an array of a record; 0 for an absent array (NULL) or one that is not
 * such an array.
 */
uint64_t
ghci_vk_decoded_count(
	const struct ghci_vk_session *session,
	const void *array)
{
	const uint8_t *at;
	uint64_t count;
	size_t offset;

	/* An absent array, or one outside the scratch's used bytes. */
	at = array;
	if (at == NULL || session->arena.base == NULL)
		return 0U;
	if (at < session->arena.base + sizeof(count) || at > session->arena.base + session->arena.used)
		return 0U;
	offset = (size_t)(at - session->arena.base);
	if ((offset & (sizeof(count) - 1U)) != 0U)
		return 0U;

	/* Succeeded: the count kept just before the array. */
	kern_memcpy(&count, at - sizeof(count), sizeof(count));
	return count;
}

/*
 * Takes bytes of the session's budget for an object's host memory.
 * Returns 0, or ENOMEM when the budget would be passed (nothing taken).
 */
int
ghci_vk_budget_take(
	struct ghci_vk_session *session,
	uint64_t bytes)
{
	/* Within the budget. */
	if (bytes > GHCI_VK_BUDGET_BYTES - session->budget_used)
		return ENOMEM;

	/* Succeeded: the bytes are the object's. */
	session->budget_used += bytes;
	return 0;
}

/* Gives bytes of the session's budget back. */
void
ghci_vk_budget_give(
	struct ghci_vk_session *session,
	uint64_t bytes)
{
	/* Never below nothing. */
	if (bytes > session->budget_used)
		bytes = session->budget_used;
	session->budget_used -= bytes;
}

/* Reports the bucket of a kind and identity. */
static uint32_t
bucket_of(
	enum ghci_vk_kind kind,
	uint64_t identity)
{
	uint64_t mixed;

	/* The identity and the kind, spread by the multiplier, the high bits taken. */
	mixed = (identity ^ (uint64_t)kind) * OBJECT_HASH;

	/* Succeeded: the bucket. */
	return (uint32_t)(mixed >> 56) % GHCI_VK_BUCKETS;
}

/*
 * Destroys the objects of a device (every device for NULL): the devices
 * themselves when devices is set, every other kind when it is not.
 */
static void
destroy_matching(
	struct ghci_vk_session *session,
	struct ghci_vk_device *device,
	int devices)
{
	struct ghci_vk_object **position;
	struct ghci_vk_object *object;
	uint32_t bucket;
	int match;

	/* Each bucket, walked again from where an object left it. */
	for (bucket = 0U; bucket < GHCI_VK_BUCKETS; bucket++) {
		position = &session->buckets[bucket];
		while (*position != NULL) {
			/* The kind the pass takes, and the device it names. */
			object = *position;
			match = 0;
			if (devices && object->kind == GHCI_VK_DEVICE)
				match = 1;
			else if (!devices && object->kind != GHCI_VK_DEVICE)
				match = 1;
			if (device != NULL && object->device != device)
				match = 0;

			/* A match leaves the bucket, so the same place is looked at again. */
			if (match) {
				ghci_vk_object_destroy(session, object);
				continue;
			}
			position = &object->next;
		}
	}
}

/*
 * Lets a VkDevice go: its objects, its executor heap, its place among
 * the session's devices and timelines, and its GHCI session, which
 * closes once the heap's unmaps are placed.
 */
static void
device_release(
	struct ghci_vk_session *session,
	struct ghci_vk_object *object)
{
	struct ghci_vk_device *device;
	struct ghci_vk_device **position;
	unsigned long irq;
	uint32_t timeline;

	/* What the device holds back fails; then its own objects, then its heap. */
	device = object->device;
	ghci_vk_holds_fail(session, device);
	ghci_vk_objects_of_device(session, device);
	ghci_vk_heap_fini(session, device);

	/* The device leaves the session's list and its timelines, which the job ioctls read. */
	irq = spin_lock_irqsave(&session->device->irq_lock);

	position = &session->devices;
	while (*position != NULL && *position != device)
		position = &(*position)->next;
	if (*position == device)
		*position = device->next;
	for (timeline = 0U; timeline < GHCI_VK_TIMELINES; timeline++) {
		if (session->timeline_device[timeline] == device)
			session->timeline_device[timeline] = NULL;
	}

	spin_unlock_irqrestore(&session->device->irq_lock, irq);

	/* The GHCI session closes; its number goes back once the close's fence passes. */
	ghci_vm_close(session->device, device->vm);
	kern_free(device);
}
