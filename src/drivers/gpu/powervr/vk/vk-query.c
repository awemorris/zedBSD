/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's query pools: VkQueryPool of result status queries
 * (VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR), the status a video decode
 * reports into, and of occlusion queries (VK_QUERY_TYPE_OCCLUSION), the
 * samples a render pass's draws let through, and the three commands that
 * reset, begin and end them.
 *
 * A pool's statuses live in the executor: a submission runs its decodes
 * before it returns (the video engine is waited for), so a status is
 * final once the submission that ended its query ran, and
 * vkGetQueryPoolResults has nothing to wait for.  The commands run in the
 * order of the command buffer's records (vk-queue.c), on the processor:
 * a reset makes queries unavailable, a begin opens one, which the decodes
 * after it report into (powervr_vk_query_report), and an end makes it
 * available.
 *
 * An occlusion query's draws count their samples in a visibility register
 * of their render job (powervr_vk_query_visibility, while the render jobs
 * are built from the records); when the job runs the query waits for it
 * (powervr_vk_query_visibility_start), and once the job's 3D finished the
 * job's reap adds the register's count to the query (visibility_report).
 * An ended query is available once no job it waits for is left; its
 * results wait for the jobs when asked to.  The counts live in a block of
 * their own, which the pool and each job holding a register keep: a job
 * may finish after its pool or its session is gone.  A reset starts a new
 * generation, so a job of the old one adds nothing.  Other query types
 * are refused when a pool is made.
 *
 * Procedure referenced: zedBSD src/drivers/gpu/nvrtx/vk/vk-video.c
 * (video_pool_create, video_pool_results, video_record_query and the
 * query arms of nvrtx_vk_video_run: the same wire and the same order);
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_cmd_query.c
 * (CmdBeginQuery: the query as the visibility register of the draws until
 * its end) and pvr_query.c (the occlusion counts a 32-bit word a query,
 * zeroed by a reset); the Vulkan 1.3 specification, vkGetQueryPoolResults
 * (the 32- or 64-bit values, the availability word after them, WAIT and
 * PARTIAL).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/powervr/render-pass.h"
#include "drivers/gpu/powervr/render-private.h"
#include "drivers/gpu/powervr/video-state.h"
#include "drivers/gpu/powervr/vk/vk.h"

/*
 * The most queries of a pool, and the most bytes of one reply's records.
 * A pool's queries are counts the kernel keeps (a job counts in visibility
 * registers of its own and the counts are added to the queries when it
 * ends), so the core does not bound the pool; the bound is the most the
 * ISP control's visibility register field can name, as Mesa makes each
 * query its own register.  A larger pool is refused as host memory the
 * kernel will not give.
 *
 * Values referenced: Mesa 26.2.6 src/imagination/csbgen/rogue/ppp.xml
 * (STATE_ISPCTL visreg 0..13) and src/imagination/vulkan/
 * pvr_arch_cmd_query.c (the query's index as the visibility register).
 */
#define QUERY_POOL_MAX			16384U
#define QUERY_REPLY_MAX			(QUERY_POOL_MAX * (POWERVR_VIDEO_QUERY_RECORD_MAX + OCCLUSION_RECORD_MAX))

/* The bytes of zeros one reply writes at a time between and after the records. */
#define QUERY_ZEROS			64U

/* The bytes of one occlusion query's record at most: a 64-bit value and a 64-bit availability. */
#define OCCLUSION_RECORD_MAX		16U

/*
 * The counts of an occlusion pool: who holds the block (the pool and each
 * job's visibility register), and each query's samples, its generation
 * (counted up by each reset) and the jobs it waits for, the arrays in the
 * block behind it.  Written by the session's ioctls (the commands' run,
 * the reaps), one at a time.
 */
struct occlusion_counts {
	uint32_t references;
	uint64_t *samples;
	uint32_t *generation;
	uint32_t *pending;
};

/*
 * A VkQueryPool: its type and count, each query's status (a result status
 * query's VkQueryResultStatusKHR: 0 not ready, 1 complete, negative a
 * failure) and whether it is available (an occlusion query's: ended), the
 * arrays in the block behind it, and an occlusion pool's counts.  Written
 * only by the commands of the session's submissions and read by
 * vkGetQueryPoolResults, both under the session's serialized ioctls.
 */
struct query_pool {
	uint32_t type;
	uint32_t count;
	int32_t *status;
	uint8_t *available;
	struct occlusion_counts *occlusion;
};

/*
 * A render job's visibility registers: the job's report (first, the
 * render job's view of it), each register's pool counts, query and the
 * generation it counts for, and whether the queries wait for the job.
 */
struct query_visibility {
	struct powervr_render_visibility base;
	struct occlusion_counts *pools[POWERVR_RENDER_VISIBILITY_MAX];
	uint32_t queries[POWERVR_RENDER_VISIBILITY_MAX];
	uint32_t generations[POWERVR_RENDER_VISIBILITY_MAX];
	bool started;
};

static int create_pool(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static struct query_pool *pool_make(uint32_t type, uint32_t count);
static int pool_destroy(struct powervr_vk_session *session, void *payload);
static int pool_results(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static uint32_t occlusion_record(const struct query_pool *pool, uint32_t query, uint32_t flags, uint8_t *record, bool *available);
static void reply_zeros(struct i915_wire_writer *reply, uint64_t bytes);
static void counts_release(struct occlusion_counts *counts);
static void visibility_report(struct powervr_render_visibility *base, const uint32_t *counts);

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
 * Records vkCmdCopyQueryPoolResults: [pool][first][count][buffer][offset]
 * [stride][flags].  The record holds the pool as object 0 and the buffer
 * as object 1, and keeps the first query and the count (values 0 and 1),
 * the offset and the stride (values 2 and 3, 4 and 5, low word first) and
 * the flags (value 6).  EINVAL when the arguments did not decode; ENOENT
 * for an unknown pool or buffer; EINVAL for queries outside the pool, a
 * buffer not written by transfers, an offset or a stride not of whole
 * words (of eight bytes for 64-bit results), records closer than one
 * record, or records past the buffer's end, which fail the recording.
 *
 * Values referenced: the Vulkan 1.0 specification
 * (vkCmdCopyQueryPoolResults: dstOffset and stride multiples of 4, or of
 * 8 with VK_QUERY_RESULT_64_BIT; VK_BUFFER_USAGE_TRANSFER_DST_BIT; the
 * records within the buffer).
 */
int
powervr_vk_record_query_copy(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct powervr_vk_record *entry)
{
	struct powervr_vk_object *objects[2];
	const struct powervr_vk_buffer *buffer;
	const struct query_pool *pool;
	uint64_t identities[2];
	uint64_t offset;
	uint64_t stride;
	uint64_t last;
	uint32_t first;
	uint32_t count;
	uint32_t flags;
	uint32_t width;
	uint32_t bytes;
	uint32_t index;
	int error;

	/* The arguments. */
	identities[0] = drv_i915_wire_read_u64(reader);
	first = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	identities[1] = drv_i915_wire_read_u64(reader);
	offset = drv_i915_wire_read_u64(reader);
	stride = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A pool and a buffer of the session. */
	objects[0] = powervr_vk_object_find(session, I915_VK_OBJ_QUERY_POOL, identities[0]);
	objects[1] = powervr_vk_object_find(session, I915_VK_OBJ_BUFFER, identities[1]);
	if (objects[0] == NULL || objects[1] == NULL)
		return ENOENT;
	pool = objects[0]->payload;
	buffer = objects[1]->payload;

	/* Queries inside the pool, a buffer written by transfers. */
	if (first > pool->count || count > pool->count - first)
		return EINVAL;
	if ((buffer->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) == 0U)
		return EINVAL;

	/* Records of whole words, apart by a record at least, within the buffer. */
	width = (flags & VK_QUERY_RESULT_64_BIT) != 0U ? 8U : 4U;
	bytes = width;
	if ((flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0U)
		bytes += width;
	if ((offset & (width - 1U)) != 0U || (stride & (width - 1U)) != 0U)
		return EINVAL;
	if (count > 1U && stride < bytes)
		return EINVAL;
	if (count != 0U) {
		if (stride > buffer->size)
			return EINVAL;
		last = offset + (uint64_t)(count - 1U) * stride;
		if (offset > buffer->size || last > buffer->size || bytes > buffer->size - last)
			return EINVAL;
	}

	/* The record holds the pool and the buffer while it may run. */
	for (index = 0; index < 2U; index++) {
		error = powervr_vk_object_retain(objects[index]);
		if (error != 0)
			return error;
		entry->objects[index] = objects[index];
		entry->object_count = index + 1U;
	}
	entry->values[0] = first;
	entry->values[1] = count;
	entry->values[2] = (uint32_t)offset;
	entry->values[3] = (uint32_t)(offset >> 32);
	entry->values[4] = (uint32_t)stride;
	entry->values[5] = (uint32_t)(stride >> 32);
	entry->values[6] = flags;

	/* Succeeded: the copy is recorded. */
	return 0;
}

/*
 * Runs a recorded vkCmdCopyQueryPoolResults once the session's earlier
 * work has finished (the caller waits): an occlusion pool's finished jobs
 * report first; then each query's record as vkGetQueryPoolResults lays it
 * out is written into the buffer through its uncached view: the result
 * when the query is available or partial results are asked for, the
 * availability word when asked for.  EIO for a buffer without a view.
 *
 * XXX A query not available when the copy runs is not waited for, even
 * with VK_QUERY_RESULT_WAIT_BIT: the work before it has finished, so such
 * a query is one no submitted work ends.
 *
 * Procedure referenced: the Vulkan 1.0 specification
 * (vkCmdCopyQueryPoolResults: without VK_QUERY_RESULT_PARTIAL_BIT the
 * result of an unavailable query is not written, the availability word
 * is).
 */
int
powervr_vk_query_copy_run(
	struct powervr_vk_session *session,
	const struct powervr_vk_record *entry)
{
	uint8_t record[POWERVR_VIDEO_QUERY_RECORD_MAX + OCCLUSION_RECORD_MAX];
	const struct query_pool *pool;
	volatile uint8_t *view;
	volatile uint8_t *at;
	uint64_t offset;
	uint64_t stride;
	uint32_t record_bytes;
	uint32_t flags;
	uint32_t width;
	uint32_t query;
	uint32_t index;
	uint32_t from;
	bool ready;
	int error;

	/* The pool, the buffer's view, where the records go. */
	pool = entry->objects[0]->payload;
	error = powervr_vk_buffer_view(entry->objects[1]->payload, &view);
	if (error != 0)
		return error;
	offset = (uint64_t)entry->values[2] | ((uint64_t)entry->values[3] << 32);
	stride = (uint64_t)entry->values[4] | ((uint64_t)entry->values[5] << 32);
	flags = entry->values[6];
	width = (flags & VK_QUERY_RESULT_64_BIT) != 0U ? 8U : 4U;

	/* An occlusion pool's finished jobs report. */
	if (pool->type == VK_QUERY_TYPE_OCCLUSION)
		powervr_render_jobs_reap(session->owner->render, session->owner, 1);

	/* Each query's record, what of it is written. */
	for (query = entry->values[0]; query < entry->values[0] + entry->values[1]; query++) {
		if (pool->type == VK_QUERY_TYPE_OCCLUSION) {
			record_bytes = occlusion_record(pool, query, flags, record, &ready);
		} else {
			record_bytes = powervr_video_query_record(pool->status[query], pool->available[query], flags, record);
			ready = pool->available[query] != 0;
		}
		from = 0U;
		if (!ready && (flags & VK_QUERY_RESULT_PARTIAL_BIT) == 0U)
			from = record_bytes > width ? width : record_bytes;
		at = view + offset + (uint64_t)(query - entry->values[0]) * stride;
		for (index = from; index < record_bytes; index++)
			at[index] = record[index];
	}

	/* Succeeded: the records are written. */
	return 0;
}

/*
 * Has a recorded begin or end of an occlusion query inside a multiview
 * render pass instance of views views name the query and the views - 1
 * after it (inside the pool): Vulkan gives such a query a query of the
 * pool for each view.  The draws of every view count in the first; the
 * others count nothing, and all of them become available at the end.
 */
void
powervr_vk_query_views(
	struct powervr_vk_record *entry,
	uint32_t views)
{
	const struct query_pool *pool;

	/* A begin or an end of an occlusion query. */
	if (views <= 1U || entry->object_count == 0 || entry->objects[0] == NULL)
		return;
	if (entry->opcode != GPU_OP_CMD_BEGIN_QUERY && entry->opcode != GPU_OP_CMD_END_QUERY)
		return;
	pool = entry->objects[0]->payload;
	if (pool->type != VK_QUERY_TYPE_OCCLUSION)
		return;

	/* The views' queries, as many as the pool has from the first. */
	entry->values[1] = views;
	if (entry->values[1] > pool->count - entry->values[0])
		entry->values[1] = pool->count - entry->values[0];
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

	/* An occlusion query's: a reset zeroes it in a new generation, a begin opens it, an end ends it. */
	if (pool->type == VK_QUERY_TYPE_OCCLUSION) {
		for (query = entry->values[0]; query < entry->values[0] + entry->values[1]; query++) {
			if (entry->opcode == GPU_OP_CMD_RESET_QUERY_POOL) {
				pool->occlusion->samples[query] = 0;
				pool->occlusion->generation[query]++;
				pool->occlusion->pending[query] = 0;
				pool->available[query] = 0;
			} else {
				pool->available[query] = entry->opcode == GPU_OP_CMD_END_QUERY;
			}
		}
		return;
	}

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

/* Reports whether a query pool object is an occlusion pool. */
bool
powervr_vk_query_occlusion(
	const struct powervr_vk_object *pool)
{
	const struct query_pool *payload;

	/* The pool's type. */
	payload = pool->payload;
	return payload->type == VK_QUERY_TYPE_OCCLUSION;
}

/*
 * Gives a draw of a render job the visibility register an occlusion query
 * of a pool counts in: the job's register of that query, else a new one
 * (the job's registers made at its first), *visibility the register plus
 * one.  ENOTSUP past the job's registers, ENOMEM without memory.
 */
int
powervr_vk_query_visibility(
	struct powervr_render_job *job,
	const struct powervr_vk_object *pool,
	uint32_t query,
	uint32_t *visibility)
{
	struct query_visibility *registers;
	struct occlusion_counts *counts;
	uint32_t index;

	/* The job's registers, made at its first. */
	counts = ((const struct query_pool *)pool->payload)->occlusion;
	registers = (struct query_visibility *)job->visibility;
	if (registers == NULL) {
		registers = kern_calloc(1, sizeof(*registers));
		if (registers == NULL)
			return ENOMEM;
		registers->base.report = visibility_report;
		job->visibility = &registers->base;
	}

	/* The query's register when the job has one. */
	for (index = 0; index < registers->base.count; index++) {
		if (registers->pools[index] == counts && registers->queries[index] == query) {
			*visibility = index + 1U;
			return 0;
		}
	}

	/* Else a new one, which holds the pool's counts. */
	if (registers->base.count == POWERVR_RENDER_VISIBILITY_MAX)
		return ENOTSUP;
	index = registers->base.count++;
	registers->pools[index] = counts;
	registers->queries[index] = query;
	counts->references++;
	*visibility = index + 1U;

	/* Succeeded: the draw's register. */
	return 0;
}

/*
 * Has the queries of a render job's registers wait for the job, which is
 * about to run: each counts it in its present generation.
 */
void
powervr_vk_query_visibility_start(
	struct powervr_render_job *job)
{
	struct query_visibility *registers;
	uint32_t index;

	/* A job without registers. */
	registers = (struct query_visibility *)job->visibility;
	if (registers == NULL)
		return;

	/* Each register's query waits for the job. */
	for (index = 0; index < registers->base.count; index++) {
		registers->generations[index] = registers->pools[index]->generation[registers->queries[index]];
		registers->pools[index]->pending[registers->queries[index]]++;
	}
	registers->started = true;
}

/* Lets a render job's registers go when the job does not run (or its run failed): their counts are lost. */
void
powervr_vk_query_visibility_drop(
	struct powervr_render_job *job)
{
	/* A job without registers. */
	if (job->visibility == NULL)
		return;

	/* The report without counts. */
	job->visibility->report(job->visibility, NULL);
	job->visibility = NULL;
}

/*
 * vkCreateQueryPool: [device][present][type][chain 0][flags][query type]
 * [count][statistics] and the create's tail -> the create's reply.  Only a
 * pool of result status or occlusion queries is made; another kind is
 * refused as a missing feature, and more than QUERY_POOL_MAX queries as
 * host memory not had.
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

	/* Only a result status or occlusion pool of some queries, and of a bounded size. */
	error = 0;
	if ((type != VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR && type != VK_QUERY_TYPE_OCCLUSION) || count == 0) {
		kern_logf("powervr: vk: vkCreateQueryPool refused: type %u, %u queries (result status and occlusion only)\n", (unsigned)type, (unsigned)count);
		error = ENOTSUP;
	} else if (count > QUERY_POOL_MAX) {
		kern_logf("powervr: vk: vkCreateQueryPool refused: %u queries (at most %u)\n", (unsigned)count, QUERY_POOL_MAX);
		error = ENOMEM;
	}

	/* The pool, every query unavailable and not ready (an occlusion pool's counts held by the pool). */
	pool = NULL;
	if (error == 0) {
		pool = pool_make(type, count);
		if (pool == NULL)
			error = ENOMEM;
	}

	/* Publishes the pool and answers; a refused one is destroyed there. */
	powervr_vk_create_reply(session, reply, I915_VK_OBJ_QUERY_POOL, identity, pool, pool_destroy, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * Makes a pool of count queries of a type, every query unavailable and
 * not ready, its arrays behind it in one block; an occlusion pool's counts
 * in a block of their own, held by the pool.  NULL without the memory.
 */
static struct query_pool *
pool_make(
	uint32_t type,
	uint32_t count)
{
	struct query_pool *pool;
	struct occlusion_counts *counts;
	uint8_t *place;

	/* The pool, then its statuses and availabilities (count is at most QUERY_POOL_MAX: no overflow). */
	place = kern_calloc(1, sizeof(*pool) + (size_t)count * (sizeof(*pool->status) + sizeof(*pool->available)));
	if (place == NULL)
		return NULL;
	pool = (struct query_pool *)place;
	place += sizeof(*pool);
	pool->status = (int32_t *)place;
	place += (size_t)count * sizeof(*pool->status);
	pool->available = place;
	pool->type = type;
	pool->count = count;
	if (type != VK_QUERY_TYPE_OCCLUSION)
		return pool;

	/* An occlusion pool's counts: the samples, then the generations and the jobs waited for. */
	place = kern_calloc(1, sizeof(*counts) + (size_t)count * (sizeof(*counts->samples) + sizeof(*counts->generation) + sizeof(*counts->pending)));
	if (place == NULL) {
		kern_free(pool);
		return NULL;
	}
	counts = (struct occlusion_counts *)place;
	place += sizeof(*counts);
	counts->samples = (uint64_t *)place;
	place += (size_t)count * sizeof(*counts->samples);
	counts->generation = (uint32_t *)place;
	place += (size_t)count * sizeof(*counts->generation);
	counts->pending = (uint32_t *)place;
	counts->references = 1;
	pool->occlusion = counts;

	/* Succeeded: the pool. */
	return pool;
}

/* Destroys a pool nothing holds: its counts stay while a job still holds them. */
static int
pool_destroy(
	struct powervr_vk_session *session,
	void *payload)
{
	struct query_pool *pool;

	/* The counts' hold, then the pool. */
	(void)session;
	pool = payload;
	if (pool->occlusion != NULL)
		counts_release(pool->occlusion);
	kern_free(pool);

	/* Succeeded: the pool is gone. */
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
	uint8_t record[POWERVR_VIDEO_QUERY_RECORD_MAX + OCCLUSION_RECORD_MAX];
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
	bool ready;

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

	/* The pool, and the bytes of one record of it, as its flags lay it out. */
	pool = powervr_vk_payload(session, I915_VK_OBJ_QUERY_POOL, identity);
	record_bytes = powervr_video_query_record(0, 0, flags, record);
	if (pool != NULL && pool->type == VK_QUERY_TYPE_OCCLUSION)
		record_bytes = occlusion_record(pool, 0, flags, record, &ready);

	/* An occlusion pool's finished jobs report first, all of them waited for when asked to. */
	if (pool != NULL && pool->type == VK_QUERY_TYPE_OCCLUSION)
		powervr_render_jobs_reap(session->owner->render, session->owner, (flags & VK_QUERY_RESULT_WAIT_BIT) != 0 ? 1 : 0);

	/* Refuses an unknown pool, a range past it, and records that do not fit the reply. */
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

	/* VK_NOT_READY when any query of the range is unavailable (an occlusion query's ended and waiting for no job). */
	result = (uint32_t)VK_SUCCESS;
	for (query = first; query < first + count; query++) {
		ready = pool->available[query] != 0;
		if (pool->type == VK_QUERY_TYPE_OCCLUSION)
			(void)occlusion_record(pool, query, flags, record, &ready);
		if (!ready)
			result = (uint32_t)VK_NOT_READY;
	}

	/* The result, then the bytes the records take. */
	drv_i915_wire_reply_u32(reply, result);
	drv_i915_wire_reply_u64(reply, bytes);

	/* Each record, then zeros to the next record's start (or the data's end). */
	written = 0;
	for (query = first; query < first + count; query++) {
		if (pool->type == VK_QUERY_TYPE_OCCLUSION)
			record_bytes = occlusion_record(pool, query, flags, record, &ready);
		else
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

/*
 * Writes an occlusion query's record as the flags lay it out -- its
 * samples as a 32-bit word (the low bits) or a 64-bit one with
 * VK_QUERY_RESULT_64_BIT, then with VK_QUERY_RESULT_WITH_AVAILABILITY_BIT
 * its availability in a word of the same size -- and reports whether it
 * is available: ended, and waiting for no job.  Returns the record's
 * bytes.
 */
static uint32_t
occlusion_record(
	const struct query_pool *pool,
	uint32_t query,
	uint32_t flags,
	uint8_t *record,
	bool *available)
{
	uint64_t words[2];
	uint32_t count;
	uint32_t word;
	uint32_t index;
	uint32_t byte;
	uint32_t at;

	/* Available once ended and every job it waited for reported. */
	*available = pool->available[query] != 0 && pool->occlusion->pending[query] == 0;

	/* The samples, then the availability when asked for. */
	words[0] = pool->occlusion->samples[query];
	words[1] = *available ? 1U : 0U;
	count = (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0 ? 2U : 1U;

	/* Each word, of 64 or 32 bits, low byte first. */
	word = (flags & VK_QUERY_RESULT_64_BIT) != 0 ? 8U : 4U;
	at = 0;
	for (index = 0; index < count; index++) {
		for (byte = 0; byte < word; byte++)
			record[at++] = (uint8_t)(words[index] >> (8U * byte));
	}

	/* Succeeded: the record's bytes. */
	return at;
}

/* Lets a hold of an occlusion pool's counts go; the last frees them. */
static void
counts_release(
	struct occlusion_counts *counts)
{
	/* One holder fewer; the last frees the block. */
	counts->references--;
	if (counts->references == 0)
		kern_free(counts);
}

/*
 * A render job's report of its visibility registers: each register's
 * count added to its query when the job counted for the query's present
 * generation, which waits for it no more; nothing added when the counts
 * were lost (NULL).  The registers' holds go, then the registers.
 */
static void
visibility_report(
	struct powervr_render_visibility *base,
	const uint32_t *counts)
{
	struct query_visibility *registers;
	struct occlusion_counts *pool;
	uint32_t query;
	uint32_t index;

	/* Each register: its count into its query, of the generation it counted for. */
	registers = (struct query_visibility *)base;
	for (index = 0; index < registers->base.count; index++) {
		pool = registers->pools[index];
		query = registers->queries[index];
		if (registers->started && pool->generation[query] == registers->generations[index]) {
			if (counts != NULL)
				pool->samples[query] += counts[index];
			if (pool->pending[query] != 0)
				pool->pending[query]--;
		}
		counts_release(pool);
	}

	/* The registers go. */
	kern_free(registers);
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
