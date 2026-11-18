/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's session and objects: wire identities of a kind and what
 * they stand for, and what every create and destroy command shares.
 *
 * An identity is published once, found by kind and identity, and removed
 * when the client destroys it; its payload lives on while a dependent
 * object still holds it.  They are one list, and found through buckets
 * by the hash of their kind and identity (the compositor's session holds
 * hundreds).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/vk/vk.h"

static unsigned object_bucket(enum i915_vk_object_kind kind, uint64_t identity);

/*
 * Opens an executor session for a render node session, with its decode
 * scratch.
 */
int
powervr_vk_open(
	struct powervr_session *owner,
	struct powervr_vk_session **result)
{
	struct powervr_vk_session *session;

	/* The session. */
	*result = NULL;
	session = kern_calloc(1, sizeof(*session));
	if (session == NULL)
		return ENOMEM;

	/* The scratch each command decodes its records into. */
	session->arena.base = kern_calloc(1, POWERVR_VK_ARENA_BYTES);
	if (session->arena.base == NULL) {
		kern_free(session);
		return ENOMEM;
	}

	session->arena.size = POWERVR_VK_ARENA_BYTES;
	session->owner = owner;

	/* Succeeded: the session has no object yet. */
	*result = session;
	return 0;
}

/*
 * Closes an executor session: every identity is removed, every payload
 * destroyed, and the executor's own memory returned (or left with the
 * core when a submission was abandoned).
 */
void
powervr_vk_close(
	struct powervr_vk_session *session)
{
	struct powervr_vk_object *object;
	struct powervr_vk_object *next;

	/* No session. */
	if (session == NULL)
		return;

	/* Nothing is published from now on; the list is taken whole, and the buckets emptied. */
	session->closing = true;
	object = session->objects;
	session->objects = NULL;
	kern_memset(session->buckets, 0, sizeof(session->buckets));

	/* Lets each registry reference go; the next entry is taken first, the release may free this one. */
	while (object != NULL) {
		next = object->next;
		object->next = NULL;
		object->published = false;
		(void)powervr_vk_object_release(object);
		object = next;
	}

	/* The executor's own memory, then the scratch and the session. */
	powervr_vk_executor_free(session);
	kern_free(session->arena.base);
	kern_free(session);
}

/*
 * Publishes an identity of a kind for a payload with its destructor.
 *
 * Returns EEXIST for an identity of the kind already published,
 * ECANCELED once the session closes, ENOBUFS past the session's limit
 * (a kernel object not had); a refused payload stays the caller's.
 */
int
powervr_vk_object_publish(
	struct powervr_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	void *payload,
	int (*destroy)(struct powervr_vk_session *, void *))
{
	struct powervr_vk_object *existing;
	struct powervr_vk_object *created;
	unsigned bucket;

	/* A null identity, an unknown kind or a missing payload names nothing. */
	if (identity == 0 || payload == NULL || destroy == NULL)
		return EINVAL;
	if (kind <= I915_VK_OBJ_NONE || kind >= I915_VK_OBJ_KIND_COUNT)
		return EINVAL;

	/* Nothing is published while the session closes or past its limit. */
	if (session->closing)
		return ECANCELED;
	if (session->live_objects >= POWERVR_VK_OBJECT_LIMIT)
		return ENOBUFS;

	/* An identity of the kind is published once. */
	existing = powervr_vk_object_find(session, kind, identity);
	if (existing != NULL)
		return EEXIST;

	/* The entry with the registry's reference. */
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->session = session;
	created->kind = kind;
	created->identity = identity;
	created->references = 1;
	created->published = true;
	created->payload = payload;
	created->destroy = destroy;

	/* Links it at the head of the list and of its bucket. */
	bucket = object_bucket(kind, identity);
	created->next = session->objects;
	session->objects = created;
	created->bucket_next = session->buckets[bucket];
	session->buckets[bucket] = created;
	session->live_objects++;

	/* Succeeded: the identity is found from now on. */
	return 0;
}

/* Finds a published identity of a kind, or NULL. */
struct powervr_vk_object *
powervr_vk_object_find(
	struct powervr_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct powervr_vk_object *object;

	/* The null identity is never published. */
	if (identity == 0)
		return NULL;

	/* Walks the bucket of the kind and identity. */
	for (object = session->buckets[object_bucket(kind, identity)]; object != NULL; object = object->bucket_next) {
		if (object->kind == kind && object->identity == identity)
			return object;
	}

	/* No such identity of the kind. */
	return NULL;
}

/* Takes one more reference on an object for a dependent object. */
int
powervr_vk_object_retain(
	struct powervr_vk_object *object)
{
	/* Only a held object can be held once more, up to the count's end. */
	if (object->references == 0)
		return EINVAL;
	if (object->references == 0xffffffffU)
		return EOVERFLOW;

	/* One more holder keeps the payload alive. */
	object->references++;

	/* Succeeded: the caller releases this reference later. */
	return 0;
}

/*
 * Lets one reference go; the last one destroys the payload and the entry.
 * A published object keeps the registry's reference until its removal.
 */
int
powervr_vk_object_release(
	struct powervr_vk_object *object)
{
	struct powervr_vk_session *session;
	int error;

	/* A missing holder lets nothing go. */
	if (object == NULL)
		return 0;
	if (object->references == 0)
		return EINVAL;
	if (object->published && object->references == 1)
		return EBUSY;

	/* The count falls; zero means nothing holds the payload any more. */
	object->references--;
	if (object->references != 0)
		return 0;

	/* Destroys the payload, then the entry. */
	session = object->session;
	error = object->destroy(session, object->payload);
	session->live_objects--;
	kern_free(object);
	if (error != 0)
		return error;

	/* Succeeded: the object is gone. */
	return 0;
}

/* Removes a published identity of a kind and lets the registry's reference go; ENOENT when none. */
int
powervr_vk_object_remove(
	struct powervr_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct powervr_vk_object **link;
	struct powervr_vk_object **bucket_link;
	struct powervr_vk_object *object;
	int error;

	/* Finds the link that points at the identity, in the list and in its bucket. */
	object = NULL;
	for (link = &session->objects; *link != NULL; link = &(*link)->next) {
		if ((*link)->kind == kind && (*link)->identity == identity) {
			object = *link;
			break;
		}
	}

	if (object == NULL)
		return ENOENT;

	bucket_link = &session->buckets[object_bucket(kind, identity)];
	while (*bucket_link != NULL && *bucket_link != object)
		bucket_link = &(*bucket_link)->bucket_next;

	/* Unlinks it from both, then lets the registry's reference go. */
	*link = object->next;
	object->next = NULL;
	if (*bucket_link == object)
		*bucket_link = object->bucket_next;
	object->bucket_next = NULL;
	object->published = false;
	error = powervr_vk_object_release(object);
	if (error != 0)
		return error;

	/* Succeeded: the identity is no longer found. */
	return 0;
}

/* Reports the payload a published identity of a kind stands for, or NULL. */
void *
powervr_vk_payload(
	struct powervr_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct powervr_vk_object *object;

	/* The published object of the kind. */
	object = powervr_vk_object_find(session, kind, identity);
	if (object == NULL)
		return NULL;

	/* Succeeded: what the identity stands for. */
	return object->payload;
}

/* Takes cleared bytes of the command's scratch, or NULL when it is spent. */
void *
powervr_vk_arena_take(
	struct powervr_vk_session *session,
	size_t bytes)
{
	struct i915_wire_arena *arena;
	uint8_t *taken;
	size_t rounded;
	size_t index;

	/* Whole eight-byte steps, within what is left. */
	arena = &session->arena;
	rounded = (bytes + 7U) & ~(size_t)7U;
	if (rounded < bytes || rounded > arena->size - arena->used)
		return NULL;

	/* Clears the bytes taken. */
	taken = arena->base + arena->used;
	for (index = 0; index < rounded; index++)
		taken[index] = 0;
	arena->used += rounded;

	/* Succeeded: the bytes are the command's until its end. */
	return taken;
}

/*
 * Reports the VkResult an errno becomes: a GPU memory pool or heap that
 * ran out (ENOSPC) is out of device memory, a kernel allocation or
 * object that could not be had (ENOMEM, ENOBUFS) out of host memory,
 * something the device lacks or more than it holds (ENOTSUP, E2BIG) a
 * missing feature, and any other refusal (EINVAL, ERANGE among them) a
 * failed initialization.  A refused create never loses the device.
 */
uint32_t
powervr_vk_result(
	int error)
{
	/* Success is VK_SUCCESS. */
	if (error == 0)
		return (uint32_t)VK_SUCCESS;

	/* The device's memory, then the kernel's. */
	if (error == ENOSPC)
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
	if (error == ENOMEM || error == ENOBUFS)
		return (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Something the executor does not implement, or more than it holds, is a missing feature. */
	if (error == ENOTSUP || error == E2BIG)
		return (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT;

	/* Anything else failed the object's initialization. */
	return (uint32_t)VK_ERROR_INITIALIZATION_FAILED;
}

/* Reads the tail every create shares, [allocator][present][identity], and reports the identity. */
uint64_t
powervr_vk_create_tail(
	struct i915_wire_reader *reader)
{
	uint64_t identity;

	/* Skips the allocator (always absent) and the output's presence word. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);

	/* The new object's identity. */
	identity = drv_i915_wire_read_u64(reader);

	/* Succeeded: the identity, or 0 with the reader's error latched. */
	return identity;
}

/*
 * Publishes a created payload and writes the create's reply: [result],
 * and on success [present][identity].  A payload that is not published
 * is destroyed here.
 */
void
powervr_vk_create_reply(
	struct powervr_vk_session *session,
	struct i915_wire_writer *reply,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	void *payload,
	int (*destroy)(struct powervr_vk_session *, void *),
	int error)
{
	uint32_t result;

	/* A payload the caller could not allocate is out of memory. */
	if (error == 0 && payload == NULL)
		error = ENOMEM;

	/* Publishes an accepted payload under its identity. */
	if (error == 0)
		error = powervr_vk_object_publish(session, kind, identity, payload, destroy);

	/* A payload made but not published goes now. */
	if (error != 0 && payload != NULL)
		(void)destroy(session, payload);

	/* The result, and a published object's identity behind a presence word. */
	result = powervr_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);
	if (error == 0) {
		drv_i915_wire_reply_u64(reply, 1);
		drv_i915_wire_reply_u64(reply, identity);
	}
}

/*
 * Runs a destroy whose wire is [device][identity][allocator] with no
 * reply body; an unknown identity is not an error.
 */
int
powervr_vk_destroy_plain(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	enum i915_vk_object_kind kind)
{
	uint64_t identity;
	int error;

	/* The identity between the device and the allocator. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Unpublishes the identity; one never published is already gone. */
	error = powervr_vk_object_remove(session, kind, identity);
	if (error == ENOENT)
		error = 0;
	if (error != 0)
		return error;

	/* Succeeded: the identity is gone. */
	return 0;
}

/* Destroys a payload that holds nothing but its own storage. */
int
powervr_vk_free_payload(
	struct powervr_vk_session *session,
	void *payload)
{
	/* Its storage. */
	(void)session;
	kern_free(payload);

	/* Succeeded: the payload is gone. */
	return 0;
}

/* Gives the bucket of a kind and identity: the identity's bits folded, with the kind. */
static unsigned
object_bucket(
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	uint64_t folded;

	/* The identity's halves and its kind mixed into the low bits. */
	folded = identity ^ (identity >> 32) ^ (identity >> 16) ^ ((uint64_t)kind * 0x9e37U);

	/* Succeeded: its bucket. */
	return (unsigned)(folded & (POWERVR_VK_OBJECT_BUCKETS - 1U));
}
