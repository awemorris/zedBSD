/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Typed protocol identities and native payload ownership remain separate throughout Vulkan object retirement. */
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-private.h"

/* One session has a finite object namespace independent of descriptor, job-slot and GPU VA capacities. */
#define VULKAN_SESSION_OBJECT_LIMIT 4096U

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

	/* One controller-mutex publication makes both the index and its ownership visible together. */
	created->next = session->objects;
	session->objects = created;
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

	/* No session or null wire identity can select an object of another namespace. */
	if (session == NULL || identity == 0)
		return NULL;

	/* Lookups remain typed even when an application numbers different object kinds identically. */
	for (object = session->objects; object != NULL; object = object->next) {
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
	int error;

	/* A nonexistent session cannot unlink any namespace. */
	if (session == NULL)
		return EINVAL;

	/* The registry reference keeps each traversal target alive under the controller mutex. */
	for (link = &session->objects; *link != NULL; link = &(*link)->next) {
		object = *link;
		if (object->kind == kind && object->identity == identity) {
			/* Namespace removal precedes any destructor that may release another dependent object. */
			*link = object->next;
			object->next = NULL;
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

	/* Each unvisited entry keeps its registry reference, so dependency releases cannot invalidate this traversal. */
	while (object != NULL) {
		next = object->next;
		object->next = NULL;
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
