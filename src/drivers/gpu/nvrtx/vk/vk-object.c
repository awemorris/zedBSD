/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's objects: wire identities of a kind and what they stand
 * for.
 *
 * An identity is published once, found by kind and identity, and removed
 * when the client destroys it; what it stands for (the payload) lives on
 * while a dependent object or submitted work still holds it, and is
 * destroyed by its kind's destructor when the last holder lets go.  The
 * node's lock serializes every call.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/vk/nvrtx-vk.h"

/*
 * The multiplier of the index's hash: 2^64 divided by the golden ratio,
 * which spreads the consecutive identities libvulkan hands out over the
 * buckets; the top ten bits name the bucket.
 */
#define OBJECT_HASH			0x9e3779b97f4a7c15ULL
#define OBJECT_BUCKET_BITS		10U

static uint32_t object_bucket(uint64_t identity);
static void object_unlink(struct nvrtx_vk_session *session, struct nvrtx_vk_object *object);

/*
 * Publishes an identity of a kind for a payload with its destructor.
 *
 * The registry holds the one reference the object starts with.  Returns
 * EEXIST for an identity of the kind already published, ECANCELED once
 * the session is closing, ENOSPC past the session's limit; a refused
 * payload stays the caller's.
 */
int
nvrtx_vk_object_publish(
	struct nvrtx_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	void *payload,
	int (*destroy)(struct nvrtx_vk_session *, void *),
	struct nvrtx_vk_object **object)
{
	struct nvrtx_vk_object *existing;
	struct nvrtx_vk_object *created;
	uint32_t bucket;

	*object = NULL;

	/* A null identity, an unknown kind or a missing payload names nothing. */
	if (identity == 0U || payload == NULL || destroy == NULL)
		return EINVAL;
	if (kind <= I915_VK_OBJ_NONE || kind >= I915_VK_OBJ_KIND_COUNT)
		return EINVAL;

	/* Nothing is published while the session closes or past its limit. */
	if (session->closing)
		return ECANCELED;
	if (session->live_objects >= NVRTX_VK_OBJECT_LIMIT)
		return ENOSPC;

	/* An identity of the kind is published once. */
	existing = nvrtx_vk_object_find(session, kind, identity);
	if (existing != NULL)
		return EEXIST;

	/* Allocates the entry. */
	created = kern_calloc(1U, sizeof(*created));
	if (created == NULL)
		return ENOMEM;

	/* The registry's reference, the payload and its destructor. */
	created->session = session;
	created->kind = kind;
	created->identity = identity;
	created->references = 1U;
	created->published = true;
	created->payload = payload;
	created->destroy = destroy;

	/* Links it at the head of the list and of its bucket. */
	created->next = session->objects;
	if (session->objects != NULL)
		session->objects->link = &created->next;
	created->link = &session->objects;
	session->objects = created;
	bucket = object_bucket(identity);
	created->bucket_next = session->buckets[bucket];
	session->buckets[bucket] = created;

	/* Counts it as published and live. */
	session->object_count++;
	session->live_objects++;

	/* Succeeded: the identity is found from now on. */
	*object = created;
	return 0;
}

/*
 * Finds a published identity of a kind, or NULL.
 */
struct nvrtx_vk_object *
nvrtx_vk_object_find(
	struct nvrtx_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct nvrtx_vk_object *object;
	uint32_t bucket;

	/* The null identity is never published. */
	if (identity == 0U)
		return NULL;

	/* Only the identity's bucket can hold it; the kind is matched too. */
	bucket = object_bucket(identity);
	for (object = session->buckets[bucket]; object != NULL; object = object->bucket_next) {
		if (object->kind == kind && object->identity == identity)
			return object;
	}

	/* No such identity of the kind. */
	return NULL;
}

/*
 * Takes one more reference on an object for a dependent object or submitted work.
 */
int
nvrtx_vk_object_retain(
	struct nvrtx_vk_object *object)
{
	/* Only a held object can be held once more, up to the count's end. */
	if (object->references == 0U)
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
 *
 * The registry's own reference is let go only by the removal (EBUSY
 * otherwise).  Returns the destructor's error when it reports one.
 */
int
nvrtx_vk_object_release(
	struct nvrtx_vk_object *object)
{
	struct nvrtx_vk_session *session;
	int error;

	/* A missing holder lets nothing go. */
	if (object == NULL)
		return 0;
	if (object->references == 0U)
		return EINVAL;

	/* A published object keeps the registry's reference until it is removed. */
	if (object->published && object->references == 1U)
		return EBUSY;

	/* The count falls; zero means nothing holds the payload any more. */
	object->references--;
	if (object->references != 0U)
		return 0;

	/* Destroys the payload, then the entry. */
	session = object->session;
	error = object->destroy(session, object->payload);
	session->live_objects--;
	kern_free(object);

	/* Reports a payload whose destruction failed. */
	if (error != 0)
		return error;

	/* Succeeded: the object is gone. */
	return 0;
}

/*
 * Removes a published identity of a kind and lets the registry's reference go.
 *
 * Returns ENOENT for an identity not published.
 */
int
nvrtx_vk_object_remove(
	struct nvrtx_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct nvrtx_vk_object *object;
	int error;

	/* Finds the identity. */
	object = nvrtx_vk_object_find(session, kind, identity);
	if (object == NULL)
		return ENOENT;

	/* Unlinks it, then lets the registry's reference go. */
	object_unlink(session, object);
	error = nvrtx_vk_object_release(object);
	if (error != 0)
		return error;

	/* Succeeded: the identity is no longer found. */
	return 0;
}

/*
 * Removes every identity of a closing session and lets each registry reference go.
 *
 * Returns the first destructor error, or EBUSY when payloads are still
 * held by something else.
 */
int
nvrtx_vk_objects_close(
	struct nvrtx_vk_session *session)
{
	struct nvrtx_vk_object *object;
	struct nvrtx_vk_object *next;
	uint32_t bucket;
	int first_error;
	int error;

	/*
	 * Nothing is published from now on; the list is taken whole and the
	 * index emptied, so no destructor finds an identity being closed.
	 */
	session->closing = true;
	object = session->objects;
	session->objects = NULL;
	session->object_count = 0U;
	for (bucket = 0U; bucket < NVRTX_VK_OBJECT_BUCKETS; bucket++)
		session->buckets[bucket] = NULL;

	/* Lets each registry reference go; the next entry is taken first, the release may free this one. */
	first_error = 0;
	while (object != NULL) {
		next = object->next;
		object->next = NULL;
		object->link = NULL;
		object->bucket_next = NULL;
		object->published = false;
		error = nvrtx_vk_object_release(object);
		if (error != 0 && first_error == 0)
			first_error = error;
		object = next;
	}

	/* Reports a destructor that failed, then payloads still held. */
	if (first_error != 0)
		return first_error;
	if (session->live_objects != 0U)
		return EBUSY;

	/* Succeeded: nothing of the session remains. */
	return 0;
}

/* Names the bucket of the index an identity is in. */
static uint32_t
object_bucket(
	uint64_t identity)
{
	uint64_t hashed;

	/* The top bits of the product mix every bit of the identity. */
	hashed = identity * OBJECT_HASH;

	/* The bucket's number. */
	return (uint32_t)(hashed >> (64U - OBJECT_BUCKET_BITS)) & (NVRTX_VK_OBJECT_BUCKETS - 1U);
}

/* Takes a published object out of its bucket and the session's list. */
static void
object_unlink(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_object *object)
{
	struct nvrtx_vk_object **link;
	uint32_t bucket;

	/* Out of its bucket. */
	bucket = object_bucket(object->identity);
	for (link = &session->buckets[bucket]; *link != NULL; link = &(*link)->bucket_next) {
		if (*link == object) {
			*link = object->bucket_next;
			break;
		}
	}

	/* Out of the list. */
	*object->link = object->next;
	if (object->next != NULL)
		object->next->link = object->link;

	/* No longer published or counted. */
	object->next = NULL;
	object->link = NULL;
	object->bucket_next = NULL;
	object->published = false;
	session->object_count--;
}
