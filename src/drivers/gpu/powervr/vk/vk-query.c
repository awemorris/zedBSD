/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's query pools: VkQueryPool of result status queries
 * (VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR), the status a video decode
 * reports into, and the three commands that reset, begin and end them.
 *
 * A pool's statuses live in the executor: a submission runs its decodes
 * before it returns (the video engine is waited for), so a status is
 * final once the submission that ended its query ran, and
 * vkGetQueryPoolResults has nothing to wait for.  The commands run in the
 * order of the command buffer's records (vk-queue.c), on the processor:
 * a reset makes queries unavailable, a begin opens one, which the decodes
 * after it report into (powervr_vk_query_report), and an end makes it
 * available.  Other query types are refused when a pool is made.
 *
 * Procedure referenced: zedBSD src/drivers/gpu/nvrtx/vk/vk-video.c
 * (video_pool_create, video_pool_results, video_record_query and the
 * query arms of nvrtx_vk_video_run: the same wire and the same order).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/powervr/video-state.h"
#include "drivers/gpu/powervr/vk/vk.h"

/* The most queries of a pool, and the most bytes of one reply's records. */
#define QUERY_POOL_MAX			256U
#define QUERY_REPLY_MAX			(QUERY_POOL_MAX * POWERVR_VIDEO_QUERY_RECORD_MAX)

/* The bytes of zeros one reply writes at a time between and after the records. */
#define QUERY_ZEROS			64U

/*
 * A VkQueryPool of result status queries: its count, and each query's
 * status (VkQueryResultStatusKHR: 0 not ready, 1 complete, negative a
 * failure) and whether it is available.  Written only by the commands of
 * the session's submissions and read by vkGetQueryPoolResults, both under
 * the session's serialized ioctls.
 */
struct query_pool {
	uint32_t count;
	int32_t status[QUERY_POOL_MAX];
	uint8_t available[QUERY_POOL_MAX];
};

static int create_pool(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int pool_results(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void reply_zeros(struct i915_wire_writer *reply, uint64_t bytes);

/*
 * Runs a query pool command; *handled stays clear for another handler's
 * opcode.
 */
int
powervr_vk_query_route(
	struct powervr_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another handler. */
	(void)requested;
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_QUERY_POOL:
		error = create_pool(session, reader, reply);
		break;
	case GPU_OP_DESTROY_QUERY_POOL:
		error = powervr_vk_destroy_plain(session, reader, I915_VK_OBJ_QUERY_POOL);
		break;
	case GPU_OP_GET_QUERY_POOL_RESULTS:
		error = pool_results(session, reader, reply);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran and wrote its reply. */
	return 0;
}

/* Reports whether an opcode is one of the recorded query commands. */
bool
powervr_vk_query_recorded(
	uint32_t opcode)
{
	/* The reset, the begin and the end. */
	switch (opcode) {
	case GPU_OP_CMD_RESET_QUERY_POOL:
	case GPU_OP_CMD_BEGIN_QUERY:
	case GPU_OP_CMD_END_QUERY:
		return true;
	default:
		break;
	}

	/* Another handler's. */
	return false;
}

/*
 * Records one query command: vkCmdResetQueryPool [pool][first][count],
 * vkCmdBeginQuery [pool][query][flags] or vkCmdEndQuery [pool][query].
 * The record holds the pool as object 0 and keeps the first query and the
 * count in its values.  EINVAL when the arguments did not decode; ENOENT
 * for an unknown pool and EINVAL for queries outside it, which fail the
 * recording.
 */
int
powervr_vk_record_query(
	struct powervr_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct powervr_vk_record *entry)
{
	struct powervr_vk_object *object;
	const struct query_pool *pool;
	uint64_t identity;
	uint32_t first;
	uint32_t count;
	int error;

	/* The pool and the first query, then the count, the flags or nothing. */
	identity = drv_i915_wire_read_u64(reader);
	first = drv_i915_wire_read_u32(reader);
	count = 1;
	if (opcode == GPU_OP_CMD_RESET_QUERY_POOL) {
		count = drv_i915_wire_read_u32(reader);
	} else if (opcode == GPU_OP_CMD_BEGIN_QUERY) {
		(void)drv_i915_wire_read_u32(reader);
	}
	if (reader->error != 0)
		return EINVAL;

	/* A pool of the session. */
	object = powervr_vk_object_find(session, I915_VK_OBJ_QUERY_POOL, identity);
	if (object == NULL)
		return ENOENT;
	pool = object->payload;

	/* Queries inside it (a reset of none is allowed). */
	if (first > pool->count || count > pool->count - first)
		return EINVAL;
	if (opcode != GPU_OP_CMD_RESET_QUERY_POOL && first >= pool->count)
		return EINVAL;

	/* The record holds the pool while it may run. */
	error = powervr_vk_object_retain(object);
	if (error != 0)
		return error;
	entry->objects[0] = object;
	entry->object_count = 1;
	entry->values[0] = first;
	entry->values[1] = count;

	/* Succeeded: the command is recorded. */
	return 0;
}

/*
 * Runs one recorded query command in its submission's order: a reset
 * makes its queries unavailable and not ready, a begin opens its query
 * (not ready until a decode reports), an end makes its query available
 * and closes it.
 */
void
powervr_vk_query_run(
	struct powervr_vk_session *session,
	const struct powervr_vk_record *entry)
{
	struct query_pool *pool;
	uint32_t query;

	/* The pool the record holds. */
	pool = entry->objects[0]->payload;

	/* Each command's change. */
	if (entry->opcode == GPU_OP_CMD_RESET_QUERY_POOL) {
		/* The queries of the range are unavailable, their statuses not ready. */
		for (query = entry->values[0]; query < entry->values[0] + entry->values[1]; query++) {
			pool->status[query] = 0;
			pool->available[query] = 0;
		}
	} else if (entry->opcode == GPU_OP_CMD_BEGIN_QUERY) {
		/*
		 * The decodes from here to the end report into this query: the
		 * session names it until the end, the record holding its pool.
		 */
		pool->status[entry->values[0]] = 0;
		pool->available[entry->values[0]] = 0;
		session->query_pool = entry->objects[0];
		session->query = entry->values[0];
	} else {
		/* The query is available with what the decodes reported, and no decode reports into it any more. */
		pool->available[entry->values[0]] = 1;
		session->query_pool = NULL;
	}
}

/*
 * Reports a video decode's status into the query its submission has
 * open, if any: a failure stays (a later success does not hide it), any
 * other status replaces the one before.
 */
void
powervr_vk_query_report(
	struct powervr_vk_session *session,
	int32_t status)
{
	struct query_pool *pool;

	/* No query is open. */
	if (session->query_pool == NULL)
		return;

	/* A failure already reported stays. */
	pool = session->query_pool->payload;
	if (pool->status[session->query] < 0)
		return;

	/* The decode's status is the query's. */
	pool->status[session->query] = status;
}

/*
 * vkCreateQueryPool: [device][present][type][chain 0][flags][query type]
 * [count][statistics] and the create's tail -> the create's reply.  Only a
 * pool of at most QUERY_POOL_MAX result status queries is made; another
 * kind is refused as a missing feature.
 */
static int
create_pool(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct query_pool *pool;
	uint64_t identity;
	uint32_t type;
	uint32_t count;
	int error;

	/* The device, the record's header and flags, the type, the count and the statistics, then the identity. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	identity = powervr_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Only a result status pool of a bounded size. */
	error = 0;
	if (type != VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR || count == 0 || count > QUERY_POOL_MAX) {
		kern_logf("powervr: vk: vkCreateQueryPool refused: type %u, %u queries (result status only)\n", (unsigned)type, (unsigned)count);
		error = ENOTSUP;
	}

	/* The pool, every query unavailable and not ready. */
	pool = NULL;
	if (error == 0) {
		pool = kern_calloc(1, sizeof(*pool));
		if (pool == NULL)
			error = ENOMEM;
	}
	if (error == 0)
		pool->count = count;

	/* Publishes the pool and answers; a refused one is freed there. */
	powervr_vk_create_reply(session, reply, I915_VK_OBJ_QUERY_POOL, identity, pool, powervr_vk_free_payload, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * vkGetQueryPoolResults: [device][pool][first][count][data size][bytes]
 * [stride][flags] -> [result][bytes]{bytes}.  Each query's record
 * (video-state.c, powervr_video_query_record) at its stride, zeros
 * between and after the records.  VK_NOT_READY when a query of the range
 * is unavailable; the statuses are final once their submissions ran, so
 * a wait has nothing to wait for.  A range or a size outside the pool or
 * the reply answers the device lost, with no bytes.
 */
static int
pool_results(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	const struct query_pool *pool;
	uint8_t record[POWERVR_VIDEO_QUERY_RECORD_MAX];
	uint64_t identity;
	uint64_t bytes;
	uint64_t stride;
	uint64_t written;
	uint64_t end;
	uint32_t record_bytes;
	uint32_t first;
	uint32_t count;
	uint32_t flags;
	uint32_t query;
	uint32_t result;

	/* The device, the pool, the range, the sizes, the stride and the flags. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	first = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	bytes = drv_i915_wire_read_u64(reader);
	stride = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The bytes of one record, as its flags lay it out. */
	record_bytes = powervr_video_query_record(0, 0, flags, record);

	/* Refuses an unknown pool, a range past it, and records that do not fit the reply. */
	pool = powervr_vk_payload(session, I915_VK_OBJ_QUERY_POOL, identity);
	if (pool == NULL ||
	    first > pool->count ||
	    count > pool->count - first ||
	    bytes > QUERY_REPLY_MAX ||
	    stride < record_bytes ||
	    (count != 0 && (uint64_t)(count - 1U) * stride + record_bytes > bytes)) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_DEVICE_LOST);
		drv_i915_wire_reply_u64(reply, 0);
		return 0;
	}

	/* VK_NOT_READY when any query of the range is unavailable. */
	result = (uint32_t)VK_SUCCESS;
	for (query = first; query < first + count; query++) {
		if (pool->available[query] == 0)
			result = (uint32_t)VK_NOT_READY;
	}

	/* The result, then the bytes the records take. */
	drv_i915_wire_reply_u32(reply, result);
	drv_i915_wire_reply_u64(reply, bytes);

	/* Each record, then zeros to the next record's start (or the data's end). */
	written = 0;
	for (query = first; query < first + count; query++) {
		record_bytes = powervr_video_query_record(pool->status[query], pool->available[query], flags, record);
		drv_i915_wire_reply_bytes(reply, record, record_bytes);
		end = written + stride;
		if (end > bytes)
			end = bytes;
		reply_zeros(reply, end - written - record_bytes);
		written = end;
	}

	/* Zeros to the end of the data. */
	reply_zeros(reply, bytes - written);

	/* Succeeded: the records are written. */
	return 0;
}

/* Writes bytes of zeros to a reply, a chunk at a time. */
static void
reply_zeros(
	struct i915_wire_writer *reply,
	uint64_t bytes)
{
	static const uint8_t zeros[QUERY_ZEROS];
	uint64_t chunk;

	/* The zeros in chunks of at most QUERY_ZEROS bytes. */
	while (bytes != 0) {
		chunk = bytes;
		if (chunk > QUERY_ZEROS)
			chunk = QUERY_ZEROS;
		drv_i915_wire_reply_bytes(reply, zeros, (size_t)chunk);
		bytes -= chunk;
	}
}
