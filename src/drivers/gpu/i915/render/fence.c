/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Vulkan fences, and occlusion query pools (at the end).
 *
 * A fence is a latch: vkQueueSubmit runs every batch of a submission to its
 * end before it replies, then signals the submission's fence.  The client
 * waits for a fence by polling vkGetFenceStatus, so no wait reaches the
 * executor.  XXX: a fence is never armed on an engine seqno; that belongs to
 * an asynchronous submission, which does not exist yet.
 */

#include "fence.h"
#include "batch.h"
#include "codec.h"
#include "draw.h"
#include "forget.h"
#include "gfx.h"
#include "internal.h"
#include "object.h"

#include "../i915.h"
#include "../memory.h"
#include "../intel/commands.h"
#include "../intel/genxml.h"

#include <kern/klog.h>
#include <kern/kmem.h>

#include <uapi/errno.h>
#include <stddef.h>

/* VK_STRUCTURE_TYPE_FENCE_CREATE_INFO. */
#define I915_FENCE_CREATE_INFO		8U

/* VK_FENCE_CREATE_SIGNALED_BIT. */
#define I915_FENCE_CREATE_SIGNALED	1U

/* VK_SUCCESS, VK_NOT_READY and VK_ERROR_INITIALIZATION_FAILED on the wire. */
#define I915_FENCE_VK_SUCCESS		0U
#define I915_FENCE_VK_NOT_READY		1U
#define I915_FENCE_VK_FAILED		((uint32_t)-3)

/* The most fences one vkResetFences may name. */
#define I915_FENCE_RESET_MAX		64U

/*
 * One Vulkan fence.
 *
 * The object table owns it from vkCreateFence to vkDestroyFence; the
 * submission that names it signals it.
 */
struct i915_vk_fence {
	/* The session the fence was created in. */
	struct i915_render_session *session;

	/* Nonzero once the fence is signaled; vkResetFences clears it. */
	unsigned signaled;
};

static int i915_fence_create(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_fence_destroy(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_fence_reset(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_fence_status(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_query_pool_create(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_query_pool_destroy(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_query_pool_results(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void i915_query_reply_zeros(struct i915_wire_writer *reply, const uint8_t *zeros, uint64_t bytes);

/*
 * Runs one fence command.
 *
 * Returns ENOTSUP for a synchronization command other than the four fence
 * and the three query pool commands, after poisoning the reader: the
 * command's length is unknown, so nothing after it can be decoded.
 */
int
drv_i915_render_fence_dispatch(
	struct i915_render_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	int error;

	/* Picks the handler of the command. */
	switch (opcode) {
	case GPU_OP_CREATE_FENCE:
		/* vkCreateFence */
		error = i915_fence_create(session, reader, reply);
		break;
	case GPU_OP_DESTROY_FENCE:
		/* vkDestroyFence */
		error = i915_fence_destroy(session, reader, reply);
		break;
	case GPU_OP_RESET_FENCES:
		/* vkResetFences */
		error = i915_fence_reset(session, reader, reply);
		break;
	case GPU_OP_GET_FENCE_STATUS:
		/* vkGetFenceStatus */
		error = i915_fence_status(session, reader, reply);
		break;
	case GPU_OP_CREATE_QUERY_POOL:
		/* vkCreateQueryPool */
		error = i915_query_pool_create(session, reader, reply);
		break;
	case GPU_OP_DESTROY_QUERY_POOL:
		/* vkDestroyQueryPool */
		error = i915_query_pool_destroy(session, reader, reply);
		break;
	case GPU_OP_GET_QUERY_POOL_RESULTS:
		/* vkGetQueryPoolResults */
		error = i915_query_pool_results(session, reader, reply);
		break;
	default:
		/* XXX: semaphores and events are not implemented; say so at the entry. */
		kern_logf("i915: vk: XXX unimplemented opcode %u (sync)\n", opcode);
		reader->error = 1;
		return ENOTSUP;
	}

	/* Reports a command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran and its reply is written. */
	return 0;
}

/*
 * Signals a fence whose submission has run to its end.
 */
void
drv_i915_fence_signal(
	struct i915_vk_fence *fence)
{
	/* Latches the fence until vkResetFences. */
	fence->signaled = 1U;
}

/*
 * Frees a fence the object table no longer names.
 */
void
drv_i915_fence_free(
	struct i915_vk_fence *fence)
{
	/* A missing fence has nothing to free. */
	if (fence == NULL)
		return;

	kern_free(fence);
}

/*
 * vkCreateFence: [device][pCreateInfo present][sType][pNext present][flags]
 * [pAllocator present][pFence present][fence wire_id] -> [result]{[present][identity]}.
 */
static int
i915_fence_create(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_vk_fence *fence;
	i915_vk_handle handle;
	uint32_t structure_type;
	uint32_t flags;
	int error;

	/* Decodes the command; only the type, the flags and the identity matter. */
	(void)drv_i915_wire_read_handle(reader);
	(void)drv_i915_wire_read_u64(reader);
	structure_type = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	handle = drv_i915_wire_read_handle(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Only a plain fence create structure is accepted. */
	if (structure_type != I915_FENCE_CREATE_INFO)
		return EINVAL;

	/* Allocates the fence, already signaled when the client asked for it. */
	error = 0;
	fence = kern_calloc(1U, sizeof(*fence));
	if (fence == NULL) {
		error = ENOMEM;
	} else {
		fence->session = session;
		if ((flags & I915_FENCE_CREATE_SIGNALED) != 0U)
			fence->signaled = 1U;

		/* Publishes the fence under the client's identity. */
		error = drv_i915_object_insert(session, I915_VK_OBJ_FENCE, handle, fence);
		if (error != 0)
			drv_i915_fence_free(fence);
	}

	/* A failed create is reported on the wire, not as a decode failure. */
	if (error != 0) {
		drv_i915_wire_reply_u32(reply, I915_FENCE_VK_FAILED);
		return 0;
	}

	/* Replies with success, the present flag and the identity. */
	drv_i915_wire_reply_u32(reply, I915_FENCE_VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u64(reply, handle);

	/* Succeeded: the fence exists. */
	return 0;
}

/*
 * vkDestroyFence: [device][fence][pAllocator present].  It returns void, so the
 * reply is the echoed opcode alone.
 */
static int
i915_fence_destroy(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_vk_fence *fence;
	i915_vk_handle handle;

	UNUSED_PARAMETER(reply);

	/* Decodes the fence identity. */
	(void)drv_i915_wire_read_handle(reader);
	handle = drv_i915_wire_read_handle(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Unpublishes and frees a fence the session knows; an unknown one is ignored. */
	fence = drv_i915_object_lookup(session, I915_VK_OBJ_FENCE, handle);
	if (fence != NULL) {
		drv_i915_object_remove(session, I915_VK_OBJ_FENCE, handle);
		drv_i915_fence_free(fence);
	}

	/* Succeeded: the fence is gone. */
	return 0;
}

/*
 * vkResetFences: [device][fenceCount][count][count x fence] -> [result].
 */
static int
i915_fence_reset(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_vk_fence *fence;
	i915_vk_handle handle;
	uint64_t count;
	uint64_t index;

	/* Decodes the fence count. */
	(void)drv_i915_wire_read_handle(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Refuses a list longer than the executor decodes. */
	if (count > I915_FENCE_RESET_MAX)
		return EINVAL;

	/* Clears every named fence the session knows. */
	for (index = 0U; index < count; index++) {
		handle = drv_i915_wire_read_handle(reader);
		fence = drv_i915_object_lookup(session, I915_VK_OBJ_FENCE, handle);
		if (fence != NULL)
			fence->signaled = 0U;
	}

	/* Refuses a list that ran past the command. */
	if (reader->error != 0)
		return EINVAL;

	/* Replies with success. */
	drv_i915_wire_reply_u32(reply, I915_FENCE_VK_SUCCESS);

	/* Succeeded: the fences are unsignaled. */
	return 0;
}

/*
 * vkGetFenceStatus: [device][fence] -> [VK_SUCCESS when signaled, VK_NOT_READY otherwise].
 */
static int
i915_fence_status(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_vk_fence *fence;
	i915_vk_handle handle;

	/* Decodes the fence identity. */
	(void)drv_i915_wire_read_handle(reader);
	handle = drv_i915_wire_read_handle(reader);
	if (reader->error != 0)
		return EINVAL;

	/* An unknown fence is reported on the wire. */
	fence = drv_i915_object_lookup(session, I915_VK_OBJ_FENCE, handle);
	if (fence == NULL) {
		drv_i915_wire_reply_u32(reply, I915_FENCE_VK_FAILED);
		return 0;
	}

	/* Reports the latch. */
	if (fence->signaled != 0U) {
		drv_i915_wire_reply_u32(reply, I915_FENCE_VK_SUCCESS);
	} else {
		drv_i915_wire_reply_u32(reply, I915_FENCE_VK_NOT_READY);
	}

	/* Succeeded: the status is written. */
	return 0;
}

/*
 * ------------------------------------------------------------------------
 * Occlusion queries.
 *
 * A query pool is a GPU object of its queries' counters: query q has the
 * pixel pipe's depth count at its begin at byte 16 q and at its end at
 * 16 q + 8, and its availability word at 16 count + 8 q.  vkCmdBeginQuery
 * and vkCmdEndQuery write the depth count with a PIPE_CONTROL (anv's
 * emit_ps_depth_count()); the end then writes the availability, and
 * vkCmdResetQueryPool clears it.  A submission has run to its end before
 * vkQueueSubmit replies, so vkGetQueryPoolResults reads the counters on the
 * CPU.  XXX: only occlusion pools; timestamp and pipeline statistics pools
 * are refused.
 * ------------------------------------------------------------------------
 */

/* VK_QUERY_TYPE_OCCLUSION, and the result flags vkGetQueryPoolResults takes. */
#define I915_QUERY_TYPE_OCCLUSION	0U
#define I915_QUERY_RESULT_64		1U
#define I915_QUERY_RESULT_AVAILABILITY	4U

/* The most queries one pool holds, and the most bytes one vkGetQueryPoolResults replies. */
#define I915_QUERY_MAX			4096U
#define I915_QUERY_REPLY_MAX		65536U

/* PIPE_CONTROL's post-sync operation: write the PS depth count (2 in bits 15:14). */
#define I915_QUERY_PC_DEPTH_COUNT	(2U << 14)

/*
 * One occlusion query pool.
 *
 * The object table owns it from vkCreateQueryPool to vkDestroyQueryPool.
 */
struct i915_gfx_query_pool {
	/* The GPU object of the counters, and how many queries it holds. */
	struct i915_gem_object *object;
	uint32_t count;
};

/*
 * vkCreateQueryPool: [device][present][sType][pNext][flags][queryType]
 * [queryCount][pipelineStatistics][pAllocator][present][wire_id] ->
 * [result]{[present][identity]}.
 */
static int
i915_query_pool_create(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_gfx_query_pool *pool;
	i915_vk_handle handle;
	uint32_t type;
	uint32_t count;
	uint64_t bytes;
	int error;

	/* Decodes the type, the count and the identity. */
	(void)drv_i915_wire_read_handle(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	handle = drv_i915_wire_read_handle(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Only an occlusion pool of a bounded number of queries. */
	error = 0;
	pool = NULL;
	if (type != I915_QUERY_TYPE_OCCLUSION || count == 0U || count > I915_QUERY_MAX) {
		kern_logf("i915: vk: XXX vkCreateQueryPool refused: type %u, %u queries (occlusion only)\n", type, count);
		error = ENOTSUP;
	} else {
		pool = kern_calloc(1U, sizeof(*pool));
		if (pool == NULL)
			error = ENOMEM;
	}

	/* Makes the counters' object, every query unavailable. */
	if (pool != NULL) {
		pool->count = count;
		bytes = ((uint64_t)count * 24U + 4095U) & ~(uint64_t)4095U;
		error = drv_i915_gfx_object_create(session, bytes, &pool->object);
		if (error == 0) {
			kern_memset(pool->object->address, 0, (size_t)bytes);
			drv_i915_gt_clflush(pool->object->address, (size_t)bytes);
			error = drv_i915_object_insert(session, I915_VK_OBJ_QUERY_POOL, handle, pool);
		}
		if (error != 0) {
			drv_i915_gfx_query_pool_free(session, pool);
			pool = NULL;
		}
	}

	/* A failed create is reported on the wire. */
	if (error != 0) {
		drv_i915_wire_reply_u32(reply, I915_FENCE_VK_FAILED);
		return 0;
	}

	/* Replies with success, the present flag and the identity. */
	drv_i915_wire_reply_u32(reply, I915_FENCE_VK_SUCCESS);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u64(reply, handle);

	/* Succeeded: the pool exists. */
	return 0;
}

/* vkDestroyQueryPool: [device][pool][pAllocator present]; the reply is the echoed opcode alone. */
static int
i915_query_pool_destroy(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_gfx_query_pool *pool;
	i915_vk_handle handle;

	UNUSED_PARAMETER(reply);

	/* Decodes the pool identity. */
	(void)drv_i915_wire_read_handle(reader);
	handle = drv_i915_wire_read_handle(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Unpublishes a pool the session knows, lets the command buffers that recorded it go of it (BUG-260) and frees it; an unknown one is ignored. */
	pool = drv_i915_object_lookup(session, I915_VK_OBJ_QUERY_POOL, handle);
	if (pool != NULL) {
		drv_i915_object_remove(session, I915_VK_OBJ_QUERY_POOL, handle);
		drv_i915_gfx_forget(session, I915_VK_OBJ_QUERY_POOL, pool);
		drv_i915_gfx_query_pool_free(session, pool);
	}

	/* Succeeded: the pool is gone. */
	return 0;
}

/*
 * vkGetQueryPoolResults: [device][pool][first][count][dataSize][bytes]
 * [stride][flags] -> [result][bytes]{bytes}.  Each query's record is its
 * sample count (end less begin) and, with WITH_AVAILABILITY, its
 * availability, in words of 4 or 8 bytes (RESULT_64), `stride` apart.
 * VK_NOT_READY when a query is not available.
 */
static int
i915_query_pool_results(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_gfx_query_pool *pool;
	const uint64_t *counters;
	i915_vk_handle handle;
	uint64_t bytes;
	uint64_t stride;
	uint64_t record;
	uint64_t value;
	uint64_t available;
	uint32_t first;
	uint32_t count;
	uint32_t flags;
	uint32_t width;
	uint32_t query;
	uint32_t result;
	uint64_t written;
	uint64_t end;
	uint8_t data[16];
	uint8_t zeros[64];

	/* Decodes the range, the size, the stride and the flags. */
	(void)drv_i915_wire_read_handle(reader);
	handle = drv_i915_wire_read_handle(reader);
	first = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	bytes = drv_i915_wire_read_u64(reader);
	stride = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A record is one or two words of four or eight bytes. */
	width = 4U;
	if ((flags & I915_QUERY_RESULT_64) != 0U)
		width = 8U;
	record = width;
	if ((flags & I915_QUERY_RESULT_AVAILABILITY) != 0U)
		record += width;

	/* Refuses an unknown pool, a range past it, and records that do not fit the reply. */
	pool = drv_i915_object_lookup(session, I915_VK_OBJ_QUERY_POOL, handle);
	if (pool == NULL || first > pool->count || count > pool->count - first ||
	    bytes > I915_QUERY_REPLY_MAX || stride < record ||
	    (count != 0U && (uint64_t)(count - 1U) * stride + record > bytes)) {
		drv_i915_wire_reply_u32(reply, I915_FENCE_VK_FAILED);
		drv_i915_wire_reply_u64(reply, 0U);
		return 0;
	}

	/* Reads what the GPU wrote. */
	counters = pool->object->address;
	drv_i915_gt_clflush(counters, (size_t)pool->count * 24U);

	/* The result: NOT_READY when any query of the range is unavailable. */
	result = I915_FENCE_VK_SUCCESS;
	for (query = first; query < first + count; query++) {
		if (counters[2U * pool->count + query] == 0U)
			result = I915_FENCE_VK_NOT_READY;
	}
	drv_i915_wire_reply_u32(reply, result);
	drv_i915_wire_reply_u64(reply, bytes);

	/* Each record at its stride, then zeros to the end of its stride and of the reply. */
	written = 0U;
	kern_memset(zeros, 0, sizeof(zeros));
	for (query = 0U; query < count; query++) {
		/* The record's two words, little-endian: the samples and the availability. */
		value = counters[2U * (first + query) + 1U] - counters[2U * (first + query)];
		available = counters[2U * pool->count + first + query];
		kern_memset(data, 0, sizeof(data));
		kern_memcpy(data, &value, width);
		kern_memcpy(data + width, &available, width);
		drv_i915_wire_reply_bytes(reply, data, (size_t)record);
		written += record;

		/* The padding up to the next record, or to the end of the reply. */
		end = written - record + stride;
		if (end > bytes)
			end = bytes;
		i915_query_reply_zeros(reply, zeros, end - written);
		written = end;
	}

	/* Anything past the last record is zeros. */
	i915_query_reply_zeros(reply, zeros, bytes - written);

	/* Succeeded: the records are written. */
	return 0;
}

/*
 * Frees a query pool and its counters' object.
 */
void
drv_i915_gfx_query_pool_free(
	struct i915_render_session *session,
	struct i915_gfx_query_pool *pool)
{
	/* A missing pool has nothing to free. */
	if (pool == NULL)
		return;

	/* The counters' object, then the record. */
	if (pool->object != NULL)
		drv_i915_gfx_object_destroy(session, pool->object);
	kern_free(pool);
}

/*
 * Writes one recorded query command into the batch: the depth count at a
 * begin or an end, then at the end the availability; a reset clears the
 * availability of each query of its range.  Returns EINVAL for a query
 * past its pool, or what taking the batch reports.
 */
int
drv_i915_gfx_query_execute(
	struct i915_render_session *session,
	const struct i915_gfx_op *op)
{
	struct i915_gfx_query_pool *pool;
	struct i915_gfx_session *work;
	struct i915_gfx_op_space space;
	uint64_t va;
	uint32_t query;
	int error;

	/* Refuses a query past its pool. */
	pool = op->u.query.pool;
	if (pool == NULL || op->u.query.first > pool->count || op->u.query.count > pool->count - op->u.query.first)
		return EINVAL;

	/* Takes the batch. */
	work = drv_i915_gfx_session_get(session);
	if (work == NULL)
		return ENOMEM;
	error = drv_i915_gfx_op_begin(session, work, &space);
	if (error != 0)
		return error;

	/* A begin or an end writes the depth count after the draws before it (depth stall). */
	query = op->u.query.first;
	if (op->kind == I915_GFX_OP_QUERY_BEGIN || op->kind == I915_GFX_OP_QUERY_END) {
		va = pool->object->va + 16U * (uint64_t)query;
		if (op->kind == I915_GFX_OP_QUERY_END)
			va += 8U;
		drv_i915_batch_emit(space.batch, GFX_OP_PIPE_CONTROL(6));
		drv_i915_batch_emit(space.batch, PIPE_CONTROL_DEPTH_STALL_ENABLE | I915_QUERY_PC_DEPTH_COUNT);
		drv_i915_batch_emit(space.batch, (uint32_t)va);
		drv_i915_batch_emit(space.batch, (uint32_t)(va >> 32));
		drv_i915_batch_emit(space.batch, 0U);
		drv_i915_batch_emit(space.batch, 0U);
	}

	/* An end makes the query available; a reset makes each of its range unavailable. */
	if (op->kind == I915_GFX_OP_QUERY_END || op->kind == I915_GFX_OP_QUERY_RESET) {
		for (query = op->u.query.first; query < op->u.query.first + op->u.query.count; query++) {
			va = pool->object->va + 16U * (uint64_t)pool->count + 8U * (uint64_t)query;
			drv_i915_batch_emit(space.batch, GFX_OP_PIPE_CONTROL(6));
			drv_i915_batch_emit(space.batch, PIPE_CONTROL_CS_STALL | PIPE_CONTROL_QW_WRITE);
			drv_i915_batch_emit(space.batch, (uint32_t)va);
			drv_i915_batch_emit(space.batch, (uint32_t)(va >> 32));
			drv_i915_batch_emit(space.batch, (uint32_t)(op->kind == I915_GFX_OP_QUERY_END));
			drv_i915_batch_emit(space.batch, 0U);
		}
	}

	/* Keeps the commands in the batch. */
	error = drv_i915_gfx_op_end(session, work, 0);
	return error;
}

/* Writes `bytes` zero bytes to a reply, a block of `zeros` at a time. */
static void
i915_query_reply_zeros(
	struct i915_wire_writer *reply,
	const uint8_t *zeros,
	uint64_t bytes)
{
	uint64_t chunk;

	/* Up to 64 bytes a time. */
	while (bytes != 0U) {
		chunk = bytes;
		if (chunk > 64U)
			chunk = 64U;
		drv_i915_wire_reply_bytes(reply, zeros, (size_t)chunk);
		bytes -= chunk;
	}
}
