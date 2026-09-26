/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host fixture for synchronization: fences (render/fence.c) and semaphores
 * (render/sync.c), and the refusal of the sync commands that were not
 * ported (render/dispatch.c).
 *
 * A fence is a latch: vkQueueSubmit runs its work to the end before it
 * replies and then signals the fence, so a fence is either signalled or
 * not, and vkGetFenceStatus reads the latch.
 */

#include "i915-vk-render-stubs.inc"

#include "../../../src/drivers/gpu/i915/render/fence.h"

/* The wire opcodes the fixture sends, as libvulkan numbers them. */
#define FIXTURE_CREATE_FENCE		35U
#define FIXTURE_DESTROY_FENCE		36U
#define FIXTURE_RESET_FENCES		37U
#define FIXTURE_GET_FENCE_STATUS	38U
#define FIXTURE_WAIT_FOR_FENCES		39U
#define FIXTURE_CREATE_SEMAPHORE	40U
#define FIXTURE_DESTROY_SEMAPHORE	41U
#define FIXTURE_CREATE_QUERY_POOL	47U

/* The structure type of VkFenceCreateInfo and its signalled flag. */
#define FIXTURE_FENCE_CREATE_INFO	8U
#define FIXTURE_FENCE_SIGNALED		1U

/* The wire identities the fixture gives its objects. */
#define FIXTURE_DEVICE		0xd0ULL
#define FIXTURE_FENCE		0x55ULL
#define FIXTURE_SIGNALED	0x56ULL
#define FIXTURE_UNKNOWN		0x57ULL
#define FIXTURE_SEMAPHORE	0x60ULL
#define FIXTURE_QUERY_POOL	0x70ULL

/* The stream every command is built in. */
static struct stub_wire fixture_wire;

static void fixture_create_fence(uint64_t identity, uint32_t structure_type, uint32_t flags);
static void fixture_destroy(uint32_t opcode, uint64_t identity);
static uint32_t fixture_fence_status(uint64_t identity);
static void test_fence(void);
static void test_semaphore(void);
static void test_unported(void);

/*
 * Runs the synchronization checks.
 */
int
main(void)
{
	/* Checks fences, semaphores, then the commands that are refused. */
	test_fence();
	test_semaphore();
	test_unported();

	/* Succeeded: every check held. */
	printf("i915 vk sync host test PASS\n");
	return 0;
}

/* Appends vkCreateFence: [35][reply][device][present][sType][pNext][flags][pAllocator][present][identity]. */
static void
fixture_create_fence(
	uint64_t identity,
	uint32_t structure_type,
	uint32_t flags)
{
	/* The header, the device and the create info behind its presence marker. */
	stub_put32(&fixture_wire, FIXTURE_CREATE_FENCE);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, structure_type);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, flags);

	/* No allocator, then the identity behind its presence marker. */
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, identity);
}

/* Appends a generic destroy: [opcode][reply][device][identity][pAllocator]. */
static void
fixture_destroy(
	uint32_t opcode,
	uint64_t identity)
{
	/* The command has no reply body; the reply is its echoed opcode. */
	stub_put32(&fixture_wire, opcode);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, identity);
	stub_put64(&fixture_wire, 0U);
}

/* Asks vkGetFenceStatus of a fence and reports its VkResult. */
static uint32_t
fixture_fence_status(
	uint64_t identity)
{
	size_t reply_bytes;
	uint32_t status;

	/* [38][reply][device][fence] -> [38][VkResult]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_GET_FENCE_STATUS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, identity);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);
	assert(stub_get32(stub_reply, 0U) == FIXTURE_GET_FENCE_STATUS);

	/* Reports the fence's result. */
	status = stub_get32(stub_reply, 4U);
	return status;
}

/*
 * A fence is created signalled or not, is signalled by a finished
 * submission, is reset, and is destroyed.
 */
static void
test_fence(void)
{
	struct i915_vk_fence *fence;
	size_t reply_bytes;
	uint32_t status;
	int error;

	/* Opens the fixture session. */
	stub_session_open(NULL);

	/* An unsignalled and a signalled fence: [35][VK_SUCCESS][present][identity] each. */
	stub_wire_begin(&fixture_wire);
	fixture_create_fence(FIXTURE_FENCE, FIXTURE_FENCE_CREATE_INFO, 0U);
	fixture_create_fence(FIXTURE_SIGNALED, FIXTURE_FENCE_CREATE_INFO, FIXTURE_FENCE_SIGNALED);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 48U);
	assert(stub_get32(stub_reply, 0U) == FIXTURE_CREATE_FENCE);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get64(stub_reply, 8U) == 1U);
	assert(stub_get64(stub_reply, 16U) == FIXTURE_FENCE);
	assert(stub_get64(stub_reply, 40U) == FIXTURE_SIGNALED);

	/* The unsignalled fence is not ready; the signalled one is. */
	status = fixture_fence_status(FIXTURE_FENCE);
	assert(status == VK_NOT_READY);
	status = fixture_fence_status(FIXTURE_SIGNALED);
	assert(status == VK_SUCCESS);

	/* A fence the session never created has no status. */
	status = fixture_fence_status(FIXTURE_UNKNOWN);
	assert(status == (uint32_t)VK_ERROR_INITIALIZATION_FAILED);

	/* A finished submission signals its fence (render/command.c calls this). */
	fence = drv_i915_object_lookup(stub_session, I915_VK_OBJ_FENCE, FIXTURE_FENCE);
	assert(fence != NULL);
	drv_i915_fence_signal(fence);
	status = fixture_fence_status(FIXTURE_FENCE);
	assert(status == VK_SUCCESS);

	/* vkResetFences of both fences, and of one that does not exist: [37][VK_SUCCESS]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_RESET_FENCES);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put32(&fixture_wire, 3U);
	stub_put64(&fixture_wire, 3U);
	stub_put64(&fixture_wire, FIXTURE_FENCE);
	stub_put64(&fixture_wire, FIXTURE_SIGNALED);
	stub_put64(&fixture_wire, FIXTURE_UNKNOWN);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	status = fixture_fence_status(FIXTURE_FENCE);
	assert(status == VK_NOT_READY);
	status = fixture_fence_status(FIXTURE_SIGNALED);
	assert(status == VK_NOT_READY);

	/* A create info of another structure type fails the stream and creates nothing. */
	stub_wire_begin(&fixture_wire);
	fixture_create_fence(FIXTURE_UNKNOWN, FIXTURE_FENCE_CREATE_INFO + 1U, 0U);
	error = stub_execute(&fixture_wire, &reply_bytes);
	assert(error == EINVAL);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_FENCE, FIXTURE_UNKNOWN) == NULL);

	/* vkDestroyFence forgets both fences; the reply is the echoed opcode alone. */
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_DESTROY_FENCE, FIXTURE_FENCE);
	fixture_destroy(FIXTURE_DESTROY_FENCE, FIXTURE_SIGNALED);
	fixture_destroy(FIXTURE_DESTROY_FENCE, FIXTURE_UNKNOWN);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 12U);
	assert(stub_get32(stub_reply, 0U) == FIXTURE_DESTROY_FENCE);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_FENCE, FIXTURE_FENCE) == NULL);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_FENCE, FIXTURE_SIGNALED) == NULL);

	/* Closes the session; nothing stays allocated. */
	stub_session_close();
	assert(stub_live == 0U);
}

/*
 * A semaphore is published by vkCreateSemaphore and forgotten by
 * vkDestroySemaphore; nothing waits on it, because every submission has
 * finished by its reply.
 */
static void
test_semaphore(void)
{
	size_t reply_bytes;

	/* Opens the fixture session. */
	stub_session_open(NULL);

	/* vkCreateSemaphore: [40][reply][device][present][sType 9][pNext][flags][pAllocator][present][identity]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_SEMAPHORE);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 9U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_SEMAPHORE);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get64(stub_reply, 16U) == FIXTURE_SEMAPHORE);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_SEMAPHORE, FIXTURE_SEMAPHORE) != NULL);

	/* vkDestroySemaphore: the echoed opcode alone. */
	stub_wire_begin(&fixture_wire);
	fixture_destroy(FIXTURE_DESTROY_SEMAPHORE, FIXTURE_SEMAPHORE);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 4U);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_SEMAPHORE, FIXTURE_SEMAPHORE) == NULL);

	/* Closes the session; nothing stays allocated. */
	stub_session_close();
	assert(stub_live == 0U);
}

/*
 * The sync commands libvulkan's vkdemo never sends were not ported:
 * vkWaitForFences and the query pools are refused by name, the stream
 * stops, and no reply length is published.
 */
static void
test_unported(void)
{
	size_t reply_bytes;
	int error;

	/* Opens the fixture session with one fence. */
	stub_session_open(NULL);
	stub_wire_begin(&fixture_wire);
	fixture_create_fence(FIXTURE_FENCE, FIXTURE_FENCE_CREATE_INFO, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);

	/* vkWaitForFences, then a destroy that must not run. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_WAIT_FOR_FENCES);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_FENCE);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1000000U);
	fixture_destroy(FIXTURE_DESTROY_FENCE, FIXTURE_FENCE);
	error = stub_execute(&fixture_wire, &reply_bytes);
	assert(error == ENOTSUP);
	assert(reply_bytes == STUB_REPLY_BYTES);
	assert(strcmp(stub_log, "i915: vk: XXX unimplemented opcode 39 (sync)\n") == 0);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_FENCE, FIXTURE_FENCE) != NULL);

	/* vkCreateQueryPool is refused the same way. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_QUERY_POOL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 11U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 4U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_QUERY_POOL);
	error = stub_execute(&fixture_wire, &reply_bytes);
	assert(error == ENOTSUP);
	assert(strcmp(stub_log, "i915: vk: XXX unimplemented opcode 47 (sync)\n") == 0);

	/* Closes the session with the fence alive: the close frees it and nothing stays allocated. */
	stub_session_close();
	assert(stub_live == 0U);
}
