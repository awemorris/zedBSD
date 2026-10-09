/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Actual native registry ownership is exercised independently of any Vulkan decoder or physical job. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-private.h"
#include "drivers/gpu/bcm2711/render-device.h"

/* One fixture payload optionally owns an independent dependency and one injected native-retirement outcome. */
struct test_payload {
	struct bcm2711_vulkan_object *dependency;
	uint32_t identity;
	int error;
};

/* Actual destructor observations distinguish registry removal from final retained ownership retirement. */
static uint32_t destroyed[16];

/* Actual registry allocation count reaches zero after every fixture-held object is finally released. */
static uint32_t allocations;

/* One injected registry allocator refusal preserves the caller's unpublished typed payload. */
static uint32_t refuse_allocation;

/* A selected ordinary allocation attempt can fail independently during session/arena construction. */
static uint32_t allocation_attempt;

static struct bcm2711_vulkan_object *publish(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, uint64_t identity, uint32_t payload_identity, struct bcm2711_vulkan_object *dependency, int retirement_error);
static int destroy_payload(struct bcm2711_vulkan_session *session, void *payload);

/*
 * Supplies the actual registry's ordinary allocator with observable storage ownership.
 */
void *
kern_calloc(
	size_t count,
	size_t bytes)
{
	void *pointer;

	/* An injected ordinary heap refusal changes no production path or payload ownership. */
	allocation_attempt++;
	if (refuse_allocation == allocation_attempt)
		return NULL;

	/* Registry memory stays visible to host allocation instrumentation. */
	pointer = calloc(count, bytes);
	if (pointer != NULL)
		allocations++;

	/* The exact ordinary heap ownership is returned to production source. */
	return pointer;
}

/*
 * Releases actual production registry storage through the ordinary host allocator.
 */
void
kern_free(
	void *pointer)
{
	/* Partial construction may release no object at all. */
	if (pointer == NULL)
		return;

	/* A registry descriptor has one actual host allocation and exactly one final release. */
	assert(allocations != 0);
	allocations--;
	free(pointer);
}

/*
 * Checks typed namespace isolation, retained old identities, dependent ownership and close refusal.
 */
int
main(
	void)
{
	struct bcm2711_vulkan_session first;
	struct bcm2711_vulkan_session second;
	struct bcm2711_vulkan_session third;
	struct bcm2711_vulkan_session *created;
	struct bcm2711_render_session render;
	struct bcm2711_vulkan_object *old;
	struct bcm2711_vulkan_object *replacement;
	struct bcm2711_vulkan_object *foreign;
	struct bcm2711_vulkan_object *different_kind;
	struct bcm2711_vulkan_object *dependency;
	struct bcm2711_vulkan_object *dependent;
	struct bcm2711_vulkan_object *retained;
	struct bcm2711_vulkan_object *found;
	struct bcm2711_vulkan_object *refused;
	struct test_payload unused;
	uint32_t refusal;
	int error;

	/* Each real registry uses a separate session even when protocol clients choose identical numeric identities. */
	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));
	memset(&third, 0, sizeof(third));
	memset(&render, 0, sizeof(render));
	memset(&unused, 0, sizeof(unused));
	old = publish(&first, I915_VK_OBJ_BUFFER, 7, 1, NULL, 0);
	foreign = publish(&second, I915_VK_OBJ_BUFFER, 7, 2, NULL, 0);
	different_kind = publish(&first, I915_VK_OBJ_IMAGE, 7, 3, NULL, 0);
	found = bcm2711_vulkan_object_find(&first, I915_VK_OBJ_BUFFER, 7);
	assert(found == old && found != foreign && found != different_kind);
	found = bcm2711_vulkan_object_find(&first, I915_VK_OBJ_IMAGE, 7);
	assert(found == different_kind);

	/* A live reused identity cannot silently overwrite a descriptor or take ownership of its caller's payload. */
	refused = (void *)1;
	error = bcm2711_vulkan_object_publish(&first, I915_VK_OBJ_BUFFER, 7, &unused, destroy_payload, &refused);
	assert(error == EEXIST && refused == NULL && first.object_count == 2);
	error = bcm2711_vulkan_object_release(old);
	assert(error == EBUSY && old->references == 1);

	/* A prepared native owner keeps the removed object alive independently of its registry identity. */
	error = bcm2711_vulkan_object_retain(old);
	assert(error == 0);
	error = bcm2711_vulkan_object_remove(&first, I915_VK_OBJ_BUFFER, 7);
	assert(error == 0 && destroyed[1] == 0 && first.live_objects == 2);
	found = bcm2711_vulkan_object_find(&first, I915_VK_OBJ_BUFFER, 7);
	assert(found == NULL);
	replacement = publish(&first, I915_VK_OBJ_BUFFER, 7, 4, NULL, 0);
	assert(replacement != old && old->references == 1);
	error = bcm2711_vulkan_object_release(old);
	assert(error == 0 && destroyed[1] == 1 && destroyed[4] == 0);

	/* An ordinary registry allocation refusal leaves the unpublished payload and all existing objects untouched. */
	refuse_allocation = 1;
	allocation_attempt = 0;
	error = bcm2711_vulkan_object_publish(&first, I915_VK_OBJ_BUFFER, 8, &unused, destroy_payload, &refused);
	assert(error == ENOMEM && refused == NULL && first.object_count == 2);
	refuse_allocation = 0;

	/* Dependent objects explicitly own their referenced payloads even after those payloads lose their wire identities. */
	dependency = publish(&third, I915_VK_OBJ_MEMORY, 20, 5, NULL, 0);
	error = bcm2711_vulkan_object_retain(dependency);
	assert(error == 0);
	dependent = publish(&third, I915_VK_OBJ_BUFFER, 21, 6, dependency, 0);
	assert(dependent->references == 1);
	error = bcm2711_vulkan_object_remove(&third, I915_VK_OBJ_MEMORY, 20);
	assert(error == 0 && destroyed[5] == 0);
	error = bcm2711_vulkan_objects_close(&third);
	assert(error == 0 && destroyed[5] == 1 && destroyed[6] == 1 && third.live_objects == 0);

	/* Final close withdraws the namespace but refuses to let its session storage retire behind a retained native owner. */
	retained = publish(&first, I915_VK_OBJ_PIPELINE, 30, 7, NULL, 0);
	error = bcm2711_vulkan_object_retain(retained);
	assert(error == 0);
	error = bcm2711_vulkan_objects_close(&first);
	assert(error == EBUSY && first.closing && first.objects == NULL && first.object_count == 0);
	assert(first.live_objects == 1 && destroyed[7] == 0 && destroyed[3] == 1 && destroyed[4] == 1);
	error = bcm2711_vulkan_object_publish(&first, I915_VK_OBJ_BUFFER, 31, &unused, destroy_payload, &refused);
	assert(error == ECANCELED && refused == NULL);
	error = bcm2711_vulkan_object_release(retained);
	assert(error == 0 && destroyed[7] == 1);
	error = bcm2711_vulkan_objects_close(&first);
	assert(error == 0 && first.live_objects == 0);

	/* One native retirement error cannot prevent the remaining registry metadata from being fully cleaned up. */
	retained = publish(&second, I915_VK_OBJ_IMAGE, 40, 8, NULL, EIO);
	assert(retained->references == 1);
	error = bcm2711_vulkan_objects_close(&second);
	assert(error == EIO && second.live_objects == 0 && second.object_count == 0);
	assert(destroyed[2] == 1 && destroyed[8] == 1 && allocations == 0);

	/* Session and arena allocation failures independently unwind all unpublished protocol storage. */
	for (refusal = 1; refusal <= 2; refusal++) {
		allocation_attempt = 0;
		refuse_allocation = refusal;
		created = (void *)1;
		error = bcm2711_vulkan_session_open(&render, &created);
		assert(error == ENOMEM && created == NULL && allocations == 0);
	}

	/* Ordinary session construction owns both namespace and a bounded command-record arena. */
	refuse_allocation = 0;
	error = bcm2711_vulkan_session_open(&render, &created);
	assert(error == 0 && created != NULL && created->render == &render);
	assert(created->arena.base != NULL && created->arena.size == 256U * 1024U && allocations == 2);
	retained = publish(created, I915_VK_OBJ_COMMAND_BUFFER, 50, 9, NULL, 0);
	error = bcm2711_vulkan_object_retain(retained);
	assert(error == 0);
	error = bcm2711_vulkan_session_close(&created);
	assert(error == EBUSY && created != NULL && created->arena.base != NULL && destroyed[9] == 0);

	/* A retained native owner's final release permits a later close to clear the exact renderer owner pointer. */
	error = bcm2711_vulkan_object_release(retained);
	assert(error == 0 && destroyed[9] == 1);
	error = bcm2711_vulkan_session_close(&created);
	assert(error == 0 && created == NULL && allocations == 0);
	error = bcm2711_vulkan_session_close(&created);
	assert(error == 0);

	/* These ownership observations use actual production registry code, without simulating physical GPU retirement. */
	puts("WS141 Vulkan object host test: PASS (typed isolation, retained identities, dependencies, close and refusal)");
	return 0;
}

/* Publishes one fixture payload through the actual private native registry. */
static struct bcm2711_vulkan_object *
publish(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	uint32_t payload_identity,
	struct bcm2711_vulkan_object *dependency,
	int retirement_error)
{
	struct test_payload *payload;
	struct bcm2711_vulkan_object *object;
	int error;

	/* Typed payload storage belongs to the fixture destructor, separately from production registry memory. */
	payload = calloc(1, sizeof(*payload));
	assert(payload != NULL);
	payload->identity = payload_identity;
	payload->dependency = dependency;
	payload->error = retirement_error;
	error = bcm2711_vulkan_object_publish(session, kind, identity, payload, destroy_payload, &object);
	assert(error == 0);

	/* The registry owns its initial reference; the returned pointer is borrowed while that reference lives. */
	return object;
}

/* Observes exact payload destruction and releases one independently retained dependency. */
static int
destroy_payload(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct test_payload *typed;
	int error;
	int retirement_error;

	/* A payload destructor observes the same live session until every dependent/native object has retired. */
	assert(session != NULL && session->live_objects != 0);
	typed = payload;
	assert(typed->identity < 16 && destroyed[typed->identity] == 0);
	destroyed[typed->identity]++;
	retirement_error = typed->error;
	error = bcm2711_vulkan_object_release(typed->dependency);
	free(typed);
	if (error != 0)
		return error;

	/* The fixture's injected native-storage outcome does not retain logical host metadata. */
	if (retirement_error != 0)
		return retirement_error;

	/* The payload and its sole dependency ownership edge have retired exactly once. */
	return 0;
}
