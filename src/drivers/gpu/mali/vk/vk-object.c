/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's session and its registry of objects: wire identities
 * of a kind and what they stand for, and what every create and destroy
 * command shares.
 *
 * An identity is published once, found through the bucket of its kind
 * and identity, and removed when the client destroys it; its payload
 * lives on while a dependent object still holds it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/mali/vk/vk.h"

static unsigned bucket_of(enum i915_vk_object_kind kind, uint64_t identity);

/*
 * Opens an executor session for a render node session, with its decode
 * scratch.
 */
int
mali_vk_open(
	struct mali_session *owner,
	struct mali_vk_session **result)
{
	struct mali_vk_session *session;

	/* The session. */
	*result = NULL;
	session = kern_calloc(1, sizeof(*session));
	if (session == NULL)
		return ENOMEM;

	/* The scratch each command decodes its records into. */
	session->arena.base = kern_calloc(1, MALI_VK_ARENA_BYTES);
	if (session->arena.base == NULL) {
		kern_free(session);
		return ENOMEM;
	}

	session->arena.size = MALI_VK_ARENA_BYTES;
	session->owner = owner;

	/* Succeeded: the session has no object yet. */
	*result = session;
	return 0;
}

/*
 * Closes an executor session: every identity is removed and every
 * payload destroyed.
 */
void
mali_vk_close(
	struct mali_vk_session *session)
{
	struct mali_vk_object *object;
	struct mali_vk_object *next;

	/* No session. */
	if (session == NULL)
		return;

	/* Nothing is published from now on; the list is taken whole. */
	session->closing = true;
	object = session->objects;
	session->objects = NULL;
	kern_memset(session->buckets, 0, sizeof(session->buckets));

	/* Each registry reference goes; the next entry is taken first, as the release may free this one. */
	while (object != NULL) {
		next = object->next;
		object->next = NULL;
		object->published = false;
		mali_vk_object_release(object);
		object = next;
	}

	/* The tiler's memory, which no pass uses any more. */
	if (session->tiler_resource != NULL)
		mali_resource_unmake(session->owner, session->tiler_resource);
	if (session->tiler != NULL)
		mali_bo_put(session->owner->render, session->tiler);

	/* The scratch and the session. */
	kern_free(session->arena.base);
	kern_free(session);
}

/* Reports whether a queue of the session was bound to a timeline. */
bool
mali_vk_timeline_owned(
	const struct mali_vk_session *session,
	uint32_t timeline)
{
	uint64_t timelines;

	/* The timelines the device's queues were fetched with. */
	if (timeline >= 64U)
		return false;
	timelines = __atomic_load_n(&session->timelines, __ATOMIC_ACQUIRE);
	if ((timelines & (1ULL << timeline)) == 0U)
		return false;

	/* Succeeded: the timeline is the session's. */
	return true;
}

/* Reports the opcode of the last refused command. */
uint32_t
mali_vk_failed_opcode(
	const struct mali_vk_session *session)
{
	return session->failed_opcode;
}

/*
 * Publishes an identity of a kind for a payload with its destructor.
 * A refused payload stays the caller's.
 */
int
mali_vk_object_publish(
	struct mali_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	void *payload,
	int (*destroy)(struct mali_vk_session *, void *))
{
	struct mali_vk_object *existing;
	struct mali_vk_object *created;
	unsigned bucket;

	/* A null identity, an unknown kind or a missing payload names nothing. */
	if (identity == 0U || payload == NULL || destroy == NULL)
		return EINVAL;
	if (kind <= I915_VK_OBJ_NONE || kind >= I915_VK_OBJ_KIND_COUNT)
		return EINVAL;

	/* Nothing is published while the session closes or past its limit. */
	if (session->closing)
		return ECANCELED;
	if (session->live_objects >= MALI_VK_OBJECT_LIMIT)
		return ENOSPC;

	/* An identity of the kind is published once. */
	existing = mali_vk_object_find(session, kind, identity);
	if (existing != NULL)
		return EEXIST;

	/* The entry with the registry's reference. */
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->session = session;
	created->kind = kind;
	created->identity = identity;
	created->references = 1U;
	created->published = true;
	created->payload = payload;
	created->destroy = destroy;

	/* Linked at the head of the list and of its bucket. */
	bucket = bucket_of(kind, identity);
	created->next = session->objects;
	session->objects = created;
	created->bucket_next = session->buckets[bucket];
	session->buckets[bucket] = created;
	session->live_objects++;

	/* Succeeded: the identity is found from now on. */
	return 0;
}

/* Finds a published identity of a kind, or NULL. */
struct mali_vk_object *
mali_vk_object_find(
	struct mali_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct mali_vk_object *object;

	/* The null identity is never published. */
	if (identity == 0U)
		return NULL;

	/* Walks the bucket of the kind and identity. */
	for (object = session->buckets[bucket_of(kind, identity)]; object != NULL; object = object->bucket_next) {
		if (object->kind == kind && object->identity == identity)
			return object;
	}

	/* No such identity of the kind. */
	return NULL;
}

/* Takes one more reference on an object for a dependent object. */
int
mali_vk_object_retain(
	struct mali_vk_object *object)
{
	/* Only a held object can be held once more. */
	if (object == NULL || object->references == 0U || object->references == 0xffffffffU)
		return EINVAL;

	/* One more holder keeps the payload alive. */
	object->references++;

	/* Succeeded: the caller releases this reference later. */
	return 0;
}

/*
 * Lets one reference go; the last one destroys the payload and the
 * entry.
 */
void
mali_vk_object_release(
	struct mali_vk_object *object)
{
	struct mali_vk_session *session;

	/* A missing holder lets nothing go. */
	if (object == NULL || object->references == 0U)
		return;

	/* The count falls; zero means nothing holds the payload any more. */
	object->references--;
	if (object->references != 0U)
		return;

	/* The payload, then the entry. */
	session = object->session;
	(void)object->destroy(session, object->payload);
	session->live_objects--;
	kern_free(object);
}

/* Removes a published identity of a kind and lets the registry's reference go; ENOENT when none. */
int
mali_vk_object_remove(
	struct mali_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct mali_vk_object **link;
	struct mali_vk_object **bucket_link;
	struct mali_vk_object *object;

	/* The object, then its link in the list and in its bucket. */
	object = mali_vk_object_find(session, kind, identity);
	if (object == NULL)
		return ENOENT;
	link = &session->objects;
	while (*link != NULL && *link != object)
		link = &(*link)->next;
	bucket_link = &session->buckets[bucket_of(kind, identity)];
	while (*bucket_link != NULL && *bucket_link != object)
		bucket_link = &(*bucket_link)->bucket_next;

	/* Unlinked from both, then the registry's reference goes. */
	if (*link == object)
		*link = object->next;
	if (*bucket_link == object)
		*bucket_link = object->bucket_next;
	object->next = NULL;
	object->bucket_next = NULL;
	object->published = false;
	mali_vk_object_release(object);

	/* Succeeded: the identity is no longer found. */
	return 0;
}

/* Reports the payload a published identity of a kind stands for, or NULL. */
void *
mali_vk_payload(
	struct mali_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct mali_vk_object *object;

	/* The published object of the kind. */
	object = mali_vk_object_find(session, kind, identity);
	if (object == NULL)
		return NULL;

	/* Succeeded: what the identity stands for. */
	return object->payload;
}

/* Takes cleared bytes of the command's scratch, or NULL when it is spent. */
void *
mali_vk_arena_take(
	struct mali_vk_session *session,
	size_t bytes)
{
	struct i915_wire_arena *arena;
	uint8_t *taken;
	size_t rounded;

	/* Whole eight-byte steps, within what is left. */
	arena = &session->arena;
	rounded = (bytes + 7U) & ~(size_t)7U;
	if (rounded < bytes || rounded > arena->size - arena->used)
		return NULL;

	/* The bytes, cleared. */
	taken = arena->base + arena->used;
	kern_memset(taken, 0, rounded);
	arena->used += rounded;

	/* Succeeded: the bytes are the command's until its end. */
	return taken;
}

/*
 * Reports the VkResult an errno becomes: the device's memory run out
 * (ENOSPC) is out of device memory, a kernel allocation (ENOMEM, ENOBUFS)
 * out of host memory, a feature the executor lacks or more than it holds
 * (ENOTSUP, E2BIG) a missing feature, and work the GPU did not finish
 * (EIO, ETIMEDOUT) a lost device; anything else failed the object's
 * initialization.
 */
uint32_t
mali_vk_result(
	int error)
{
	/* Success. */
	if (error == 0)
		return (uint32_t)VK_SUCCESS;

	/* The device's memory, then the kernel's. */
	if (error == ENOSPC)
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
	if (error == ENOMEM || error == ENOBUFS)
		return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Something the executor does not implement, or more than it holds. */
	if (error == ENOTSUP || error == E2BIG)
		return (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;

	/* Work the GPU did not finish. */
	if (error == EIO || error == ETIMEDOUT)
		return (uint32_t)VK_ERROR_DEVICE_LOST;

	/* Anything else failed the object's initialization. */
	return (uint32_t)VK_ERROR_INITIALIZATION_FAILED;
}

/* Reads the tail every create shares, [allocator][present][identity], and reports the identity. */
uint64_t
mali_vk_create_tail(
	struct i915_wire_reader *reader)
{
	uint64_t identity;

	/* The allocator (always absent) and the output's presence word. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);

	/* The new object's identity, or 0 with the reader's error latched. */
	identity = drv_i915_wire_read_u64(reader);
	return identity;
}

/*
 * Publishes a created payload and writes the create's reply: [result],
 * and on success [present][identity].  A payload that is not published
 * is destroyed here.
 */
void
mali_vk_create_reply(
	struct mali_vk_session *session,
	struct i915_wire_writer *reply,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	void *payload,
	int (*destroy)(struct mali_vk_session *, void *),
	int error)
{
	/* A payload the caller could not allocate is out of memory. */
	if (error == 0 && payload == NULL)
		error = ENOMEM;

	/* Publishes an accepted payload under its identity. */
	if (error == 0)
		error = mali_vk_object_publish(session, kind, identity, payload, destroy);

	/* A payload made but not published goes now. */
	if (error != 0 && payload != NULL)
		(void)destroy(session, payload);

	/* The result, and a published object's identity behind a presence word. */
	drv_i915_wire_reply_u32(reply, mali_vk_result(error));
	if (error == 0) {
		drv_i915_wire_reply_u64(reply, 1);
		drv_i915_wire_reply_u64(reply, identity);
	}
}

/*
 * Runs a destroy of the form [device][identity][allocator] with no reply
 * body; an unknown identity is not an error.
 */
int
mali_vk_destroy_plain(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	enum i915_vk_object_kind kind)
{
	uint64_t identity;

	/* The identity between the device and the allocator. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Unpublishes it; one never published is already gone. */
	(void)mali_vk_object_remove(session, kind, identity);

	/* Succeeded: the identity is gone. */
	return 0;
}

/* Destroys a payload that holds nothing but its own storage. */
int
mali_vk_free_payload(
	struct mali_vk_session *session,
	void *payload)
{
	(void)session;
	kern_free(payload);
	return 0;
}

/* Reports the wire identity a decoded handle field carries. */
uint64_t
mali_vk_handle(
	const void *handle)
{
	return (uint64_t)(uintptr_t)handle;
}

/* Gives the bucket of a kind and identity: the identity's bits folded with the kind. */
static unsigned
bucket_of(
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	uint64_t folded;

	/* The halves and the kind mixed into the low bits. */
	folded = identity ^ (identity >> 32) ^ (identity >> 16) ^ ((uint64_t)kind * 0x9e37U);
	return (unsigned)(folded & (MALI_VK_OBJECT_BUCKETS - 1U));
}
