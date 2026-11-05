/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Typed protocol identities and native payload ownership remain separate throughout Vulkan object retirement. */
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vk/vulkan-private.h"

/* One session has a finite object namespace independent of descriptor, job-slot and GPU VA capacities. */
#define VULKAN_SESSION_OBJECT_LIMIT 4096U

/*
 * The multiplier of the index's Fibonacci hash: 2^64 divided by the golden
 * ratio.  libvulkan numbers identities one after another, and this spreads
 * consecutive numbers evenly over the buckets.
 */
#define VULKAN_OBJECT_HASH UINT64_C(0x9e3779b97f4a7c15)

/* The bits of the hashed identity that name a bucket (1024 buckets). */
#define VULKAN_OBJECT_BUCKET_BITS 10U

static uint32_t object_bucket(uint64_t identity);

/*
 * Publishes one typed Vulkan identity with a separately owned registry reference.
 */
int
bcm2711_vulkan_object_publish(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	void *payload,
	int (*destroy)(struct bcm2711_vulkan_session *, void *),
	struct bcm2711_vulkan_object **object)
{
	struct bcm2711_vulkan_object *existing;
	struct bcm2711_vulkan_object *created;
	uint32_t bucket;

	/* Refusal owns neither payload nor destructor responsibility. */
	if (object == NULL)
		return EINVAL;
	*object = NULL;
	if (session == NULL ||
	    payload == NULL ||
	    destroy == NULL ||
	    identity == 0 ||
	    kind <= I915_VK_OBJ_NONE ||
	    kind >= I915_VK_OBJ_KIND_COUNT)
		return EINVAL;

	/* Stop publication prevents a decoder from adding objects behind final-close retirement. */
	if (session->closing)
		return ECANCELED;
	if (session->live_objects >= VULKAN_SESSION_OBJECT_LIMIT)
		return ENOSPC;

	/* Reused live identities are rejected rather than overwriting an object still referenced by native work. */
	existing = bcm2711_vulkan_object_find(session, kind, identity);
	if (existing != NULL)
		return EEXIST;

	/* A registry entry is allocated independently of its caller-owned typed payload. */
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;

	/* The registry initially owns exactly one reference; dependent objects and jobs explicitly acquire more. */
	created->session = session;
	created->kind = kind;
	created->identity = identity;
	created->references = 1;
	created->published = true;
	created->payload = payload;
	created->destroy = destroy;

	/* One controller-mutex publication makes the list, the index and the ownership visible together. */
	created->next = session->objects;
	if (session->objects != NULL)
		session->objects->link = &created->next;
	created->link = &session->objects;
	session->objects = created;

	/* The index finds the identity in its bucket from now on. */
	bucket = object_bucket(identity);
	created->bucket_next = session->buckets[bucket];
	session->buckets[bucket] = created;

	/* The registry counts the published identity and its live payload. */
	session->object_count++;
	session->live_objects++;
	*object = created;

	/* The caller can now look up this typed identity while its registry reference is live. */
	return 0;
}

/*
 * Finds a borrowed typed object inside one controller-serialized session namespace.
 */
struct bcm2711_vulkan_object *
bcm2711_vulkan_object_find(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct bcm2711_vulkan_object *object;
	uint32_t bucket;

	/* No session or null wire identity can select an object of another namespace. */
	if (session == NULL || identity == 0)
		return NULL;

	/*
	 * Only the identity's bucket can hold it; lookups remain typed even
	 * when an application numbers different object kinds identically.
	 */
	bucket = object_bucket(identity);
	for (object = session->buckets[bucket]; object != NULL; object = object->bucket_next) {
		if (object->kind == kind && object->identity == identity)
			return object;
	}

	/* An absent typed identity grants no payload ownership. */
	return NULL;
}

/*
 * Retains an independently owned Vulkan object reference for a dependent object or prepared native payload.
 */
int
bcm2711_vulkan_object_retain(
	struct bcm2711_vulkan_object *object)
{
	/* Only a currently held logical object can acquire another reference. */
	if (object == NULL || object->references == 0)
		return EINVAL;
	if (object->references == 0xffffffffU)
		return EOVERFLOW;

	/* The controller mutex protects reference increments from registry removal and native disposal. */
	object->references++;

	/* The owner must release this exact reference independently of identity removal. */
	return 0;
}

/*
 * Releases an object reference and destroys its typed payload only after the last physical owner retires.
 */
int
bcm2711_vulkan_object_release(
	struct bcm2711_vulkan_object *object)
{
	struct bcm2711_vulkan_session *session;
	int error;

	/* A null ownership edge is harmless during partial object construction unwind. */
	if (object == NULL)
		return 0;
	if (object->references == 0)
		return EINVAL;

	/* The registry's last reference can be released only after its identity has been unlinked. */
	if (object->published && object->references == 1)
		return EBUSY;
	object->references--;
	if (object->references != 0)
		return 0;

	/* Destructors release host payloads and ownership edges; uncertain native storage remains in the VA quarantine owner. */
	session = object->session;
	error = object->destroy(session, object->payload);
	session->live_objects--;
	kern_free(object);

	/* A native retirement error remains visible after logical metadata has retired. */
	if (error != 0)
		return error;

	/* No registry, dependency or prepared native owner retains this payload. */
	return 0;
}

/*
 * Removes one typed wire identity without invalidating references already held by dependent or native work.
 */
int
bcm2711_vulkan_object_remove(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct bcm2711_vulkan_object **link;
	struct bcm2711_vulkan_object *object;
	uint32_t bucket;
	int error;

	/* A nonexistent session cannot unlink any namespace. */
	if (session == NULL)
		return EINVAL;

	/* The registry reference keeps each entry of the identity's bucket alive under the controller mutex. */
	bucket = object_bucket(identity);
	for (link = &session->buckets[bucket]; *link != NULL; link = &(*link)->bucket_next) {
		object = *link;
		if (object->kind == kind && object->identity == identity) {
			/* Namespace removal from the index and the list precedes any destructor that may release another dependent object. */
			*link = object->bucket_next;
			object->bucket_next = NULL;
			*object->link = object->next;
			if (object->next != NULL)
				object->next->link = object->link;
			object->next = NULL;
			object->link = NULL;
			object->published = false;
			session->object_count--;
			error = bcm2711_vulkan_object_release(object);
			if (error != 0)
				return error;

			/* Existing retained references keep the removed payload alive until their own retirement. */
			return 0;
		}
	}

	/* A stale destroy cannot affect an identity of another kind or session. */
	return ENOENT;
}

/*
 * Withdraws an entire session namespace after the native worker and common callbacks have been joined.
 */
int
bcm2711_vulkan_objects_close(
	struct bcm2711_vulkan_session *session)
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_object *next;
	uint32_t bucket;
	int error;
	int retirement_error;

	/* No missing session owns registry references. */
	if (session == NULL)
		return 0;

	/* Final close withdraws all wire identities before any destructor can attempt a fresh lookup. */
	session->closing = true;
	object = session->objects;
	session->objects = NULL;
	session->object_count = 0;
	retirement_error = 0;

	/* The index empties with the list. */
	for (bucket = 0; bucket < BCM2711_VULKAN_OBJECT_BUCKETS; bucket++)
		session->buckets[bucket] = NULL;

	/* Each unvisited entry keeps its registry reference, so dependency releases cannot invalidate this traversal. */
	while (object != NULL) {
		next = object->next;
		object->next = NULL;
		object->link = NULL;
		object->bucket_next = NULL;
		object->published = false;
		error = bcm2711_vulkan_object_release(object);
		if (error != 0 && retirement_error == 0)
			retirement_error = error;
		object = next;
	}

	/* All registry ownership edges retire even when a native view release requires later device recovery. */
	if (retirement_error != 0)
		return retirement_error;
	if (session->live_objects != 0)
		return EBUSY;

	/* The closed namespace grants no new protocol identity or object reference. */
	return 0;
}

/* Names the bucket of the session's index that holds an identity. */
static uint32_t
object_bucket(
	uint64_t identity)
{
	uint64_t hashed;

	/* The top bits of the Fibonacci hash, which mix every bit of the identity. */
	hashed = identity * VULKAN_OBJECT_HASH;

	/* The bucket's number, kept below BCM2711_VULKAN_OBJECT_BUCKETS even if the two sizes ever disagree. */
	return (uint32_t)(hashed >> (64U - VULKAN_OBJECT_BUCKET_BITS)) & (BCM2711_VULKAN_OBJECT_BUCKETS - 1U);
}
