/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Occlusion query pools (vulkan-occlusion.h), included by vulkan-query.c.
 * The begin, end and reset are ordinary records (vulkan-record.c); the
 * draw walk keeps the active query, the native pass gives its draws a
 * counter, and the job adds the counters and runs the ends and resets.
 */

static int occlusion_create(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int occlusion_destroy(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader);
static int occlusion_results(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int occlusion_release(struct bcm2711_vulkan_session *session, void *payload);
static void occlusion_reply_refusal(struct i915_wire_writer *reply, VkResult result);
static struct bcm2711_vulkan_occlusion_pool *occlusion_pool(struct bcm2711_vulkan_session *session, uint64_t identity);
static void occlusion_copy(const struct bcm2711_vulkan_occlusion_pool *pool, const struct bcm2711_vulkan_record *record);
static uint32_t occlusion_record_bytes(uint32_t flags);

/* The vkGetQueryPoolResults flags the reply honours: 64-bit words and the availability word. */
#define OCCLUSION_RESULT_64		0x1U
#define OCCLUSION_RESULT_AVAILABILITY	0x4U

/* The most bytes of results one reply carries: every query of the largest pool, two 64-bit words each. */
#define OCCLUSION_REPLY_BYTES		(BCM2711_VULKAN_OCCLUSION_QUERIES * 16U)

/*
 * Routes vkCreateQueryPool when it makes an occlusion pool, and
 * vkDestroyQueryPool and vkGetQueryPoolResults when they name one; any
 * other query pool is left to the video's routes.
 */
int
bcm2711_vulkan_occlusion_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	struct i915_wire_reader peek;
	uint64_t identity;
	uint32_t type;
	int error;

	/* Only the pool commands, and only for an occlusion pool. */
	*handled = 0;
	if (opcode != GPU_OP_CREATE_QUERY_POOL && opcode != GPU_OP_DESTROY_QUERY_POOL && opcode != GPU_OP_GET_QUERY_POOL_RESULTS)
		return 0;

	/* A create's query type follows [device][present][type][chain][flags]; the others name the pool after the device. */
	peek = *reader;
	(void)drv_i915_wire_read_u64(&peek);
	if (opcode == GPU_OP_CREATE_QUERY_POOL) {
		(void)drv_i915_wire_read_u64(&peek);
		(void)drv_i915_wire_read_u32(&peek);
		(void)drv_i915_wire_read_u64(&peek);
		(void)drv_i915_wire_read_u32(&peek);
		type = drv_i915_wire_read_u32(&peek);
		if (peek.error != 0 || type != VK_QUERY_TYPE_OCCLUSION)
			return 0;
	} else {
		identity = drv_i915_wire_read_u64(&peek);
		if (peek.error != 0 || occlusion_pool(session, identity) == NULL)
			return 0;
	}
	*handled = 1;

	/* Runs the command. */
	(void)requested;
	if (opcode == GPU_OP_CREATE_QUERY_POOL)
		error = occlusion_create(session, reader, reply);
	else if (opcode == GPU_OP_DESTROY_QUERY_POOL)
		error = occlusion_destroy(session, reader);
	else
		error = occlusion_results(session, reader, reply);
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/*
 * Reports whether a recorded query command names an occlusion pool: the
 * command buffer, then the pool, at the reader's position.
 */
bool
bcm2711_vulkan_occlusion_named(
	struct bcm2711_vulkan_session *session,
	const struct i915_wire_reader *reader)
{
	struct i915_wire_reader peek;
	uint64_t identity;

	/* The pool after the command buffer. */
	peek = *reader;
	(void)drv_i915_wire_read_u64(&peek);
	identity = drv_i915_wire_read_u64(&peek);
	if (peek.error != 0)
		return false;

	/* Succeeded: whether it is an occlusion pool. */
	return occlusion_pool(session, identity) != NULL;
}

/*
 * The occlusion pool a recorded begin, end, reset or copy names.  Those records
 * keep the pool in their first object; the video's query records keep
 * theirs elsewhere and leave it empty.
 */
struct bcm2711_vulkan_occlusion_pool *
bcm2711_vulkan_occlusion_record(
	const struct bcm2711_vulkan_record *record)
{
	const struct bcm2711_vulkan_object *object;

	/* One of the four query commands. */
	if (record == NULL ||
	    (record->opcode != GPU_OP_CMD_BEGIN_QUERY &&
	     record->opcode != GPU_OP_CMD_END_QUERY &&
	     record->opcode != GPU_OP_CMD_RESET_QUERY_POOL &&
	     record->opcode != GPU_OP_CMD_COPY_QUERY_POOL_RESULTS))
		return NULL;

	/* Whose first object is an occlusion pool. */
	object = record->objects[0];
	if (object == NULL || object->kind != I915_VK_OBJ_QUERY_POOL || object->destroy != occlusion_release)
		return NULL;

	/* Succeeded: the pool. */
	return object->payload;
}

/*
 * Runs a reset, an end or a copy in the native job, after every earlier
 * pass: a reset clears its queries' samples and makes them unavailable;
 * an end makes its query available (every draw before it has been
 * counted); a copy writes its queries' records into its buffer
 * (occlusion_copy).
 */
void
bcm2711_vulkan_occlusion_run(
	const struct bcm2711_vulkan_record *record)
{
	struct bcm2711_vulkan_occlusion_pool *pool;
	uint32_t query;

	/* Recording bounded the range to the pool. */
	pool = bcm2711_vulkan_occlusion_record(record);
	if (pool == NULL)
		return;
	if (record->opcode == GPU_OP_CMD_RESET_QUERY_POOL) {
		for (query = record->first; query < record->first + record->count; query++) {
			__atomic_store_n(&pool->available[query], 0U, __ATOMIC_RELEASE);
			pool->samples[query] = 0U;
		}
	} else if (record->opcode == GPU_OP_CMD_END_QUERY) {
		/* The samples are written before a reader can see the query available. */
		__atomic_store_n(&pool->available[record->first], 1U, __ATOMIC_RELEASE);
	} else if (record->opcode == GPU_OP_CMD_COPY_QUERY_POOL_RESULTS) {
		occlusion_copy(pool, record);
	}

	/* Succeeded: the command ran. */
	return;
}

/*
 * Runs the end of a query in a multiview render pass: the query and the
 * views - 1 after it become available.  The draws of every view counted into
 * the first; Vulkan leaves how the views' samples spread over the queries
 * to the implementation, so the others hold none.
 */
void
bcm2711_vulkan_occlusion_end_views(
	const struct bcm2711_vulkan_record *record,
	uint32_t views)
{
	struct bcm2711_vulkan_occlusion_pool *pool;
	uint32_t query;

	/* An end of an occlusion query; recording bounded its first query to the pool. */
	pool = bcm2711_vulkan_occlusion_record(record);
	if (pool == NULL || record->opcode != GPU_OP_CMD_END_QUERY)
		return;

	/* The queries of the views after the first, then the first, whose samples are all in. */
	for (query = record->first + 1U; query < record->first + views && query < pool->count; query++)
		__atomic_store_n(&pool->available[query], 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&pool->available[record->first], 1U, __ATOMIC_RELEASE);

	/* Succeeded: the views' queries are available. */
	return;
}

/* Adds a completed pass's counter to its query. */
void
bcm2711_vulkan_occlusion_add(
	struct bcm2711_vulkan_occlusion_pool *pool,
	uint32_t query,
	uint32_t samples)
{
	/* The pass's draws named only queries of the pool. */
	if (pool == NULL || query >= pool->count)
		return;
	pool->samples[query] += samples;

	/* Succeeded: the query counts the pass's samples. */
	return;
}

/*
 * Checks a recorded vkCmdCopyQueryPoolResults (objects 0 the pool, 1 the
 * buffer, offsets 0 and 1 where the records go and how far apart, words[0]
 * the flags): a buffer of the pool's device, bound and written by
 * transfers; the offset and the stride of whole words (of eight bytes for
 * 64-bit results), records apart by a record at least, all within the
 * buffer.
 *
 * Values referenced: the Vulkan 1.0 specification
 * (vkCmdCopyQueryPoolResults: dstOffset and stride multiples of 4, or of
 * 8 with VK_QUERY_RESULT_64_BIT; the buffer's VK_BUFFER_USAGE_TRANSFER_DST_BIT;
 * the records within the buffer).
 */
int
bcm2711_vulkan_occlusion_copy_validate(
	const struct bcm2711_vulkan_record *record)
{
	const struct bcm2711_vulkan_occlusion_pool *pool;
	const struct bcm2711_vulkan_resource *buffer;
	uint64_t last;
	uint32_t width;
	uint32_t bytes;

	/* A copy of an occlusion pool's results into a buffer. */
	pool = bcm2711_vulkan_occlusion_record(record);
	if (pool == NULL || record->opcode != GPU_OP_CMD_COPY_QUERY_POOL_RESULTS)
		return EINVAL;
	if (record->objects[1] == NULL || record->objects[1]->kind != I915_VK_OBJ_BUFFER || record->objects[1]->payload == NULL)
		return EINVAL;

	/* The pool's device's buffer, bound, written by transfers. */
	buffer = record->objects[1]->payload;
	if (buffer->device != pool->device || buffer->memory == NULL || (buffer->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) == 0)
		return EINVAL;

	/* Whole words, records apart by a record at least, within the buffer. */
	width = 4U;
	if ((record->words[0] & OCCLUSION_RESULT_64) != 0U)
		width = 8U;
	bytes = occlusion_record_bytes(record->words[0]);
	if ((record->offsets[0] & (width - 1U)) != 0U || (record->offsets[1] & (width - 1U)) != 0U)
		return EINVAL;
	if (record->count > 1U && record->offsets[1] < bytes)
		return EINVAL;
	if (record->count != 0U) {
		if (record->offsets[1] > buffer->bytes)
			return EINVAL;
		last = record->offsets[0] + (uint64_t)(record->count - 1U) * record->offsets[1];
		if (record->offsets[0] > buffer->bytes || last > buffer->bytes || bytes > buffer->bytes - last)
			return EINVAL;
	}

	/* Succeeded: the copy may run. */
	return 0;
}

/*
 * Writes a copy's records into its buffer's coherent memory, as
 * vkGetQueryPoolResults lays them out: each query's samples when it is
 * available or partial results are asked for, then its availability word
 * when asked for, each a word of 4 or 8 bytes.  A buffer that is no longer
 * as validated is not written.
 *
 * XXX A query not available when the copy runs is not waited for, even
 * with VK_QUERY_RESULT_WAIT_BIT: every pass before the copy has run, so
 * such a query is one no recorded work ends.
 *
 * Procedure referenced: the Vulkan 1.0 specification
 * (vkCmdCopyQueryPoolResults: without VK_QUERY_RESULT_PARTIAL_BIT the
 * result of an unavailable query is not written, the availability word
 * is).
 */
static void
occlusion_copy(
	const struct bcm2711_vulkan_occlusion_pool *pool,
	const struct bcm2711_vulkan_record *record)
{
	struct bcm2711_vulkan_resource *buffer;
	struct bcm2711_v3d_view *view;
	uint64_t words[2];
	uint32_t address;
	uint32_t width;
	uint32_t query;
	uint32_t index;
	uint8_t *at;
	void *cpu;
	int error;

	/* The buffer, still as validated, and its coherent memory. */
	if (bcm2711_vulkan_occlusion_copy_validate(record) != 0)
		return;
	buffer = record->objects[1]->payload;
	error = bcm2711_vulkan_resource_backing(buffer, 0, buffer->bytes, &view, &address, &cpu);
	if (error != 0 || !view->buffer->uncached)
		return;
	width = 4U;
	if ((record->words[0] & OCCLUSION_RESULT_64) != 0U)
		width = 8U;

	/* Each query's record. */
	for (query = 0U; query < record->count; query++) {
		words[1] = __atomic_load_n(&pool->available[record->first + query], __ATOMIC_ACQUIRE);
		words[0] = pool->samples[record->first + query];
		at = (uint8_t *)cpu + record->offsets[0] + (uint64_t)query * record->offsets[1];
		if (words[1] != 0U || (record->words[0] & VK_QUERY_RESULT_PARTIAL_BIT) != 0U) {
			for (index = 0U; index < width; index++)
				at[index] = (uint8_t)(words[0] >> (8U * index));
		}
		if ((record->words[0] & OCCLUSION_RESULT_AVAILABILITY) != 0U) {
			for (index = 0U; index < width; index++)
				at[width + index] = (uint8_t)(words[1] >> (8U * index));
		}
	}

	/* The bytes are out before the next CPU read or native launch; the snapshots of the memory learn of an unknown extent. */
	kern_io_write_barrier();
	bcm2711_v3d_memory_written(view->buffer, address - view->address, NULL);
}

/* Gives the bytes of one query's record: its result, and its availability when asked for. */
static uint32_t
occlusion_record_bytes(
	uint32_t flags)
{
	uint32_t width;

	/* A word of 4 or 8 bytes, twice with the availability. */
	width = 4U;
	if ((flags & OCCLUSION_RESULT_64) != 0U)
		width = 8U;
	if ((flags & OCCLUSION_RESULT_AVAILABILITY) != 0U)
		return 2U * width;

	/* Succeeded: the result alone. */
	return width;
}

/*
 * vkCreateQueryPool of an occlusion pool: [device][present][type][chain 0]
 * [flags][query type][count][statistics][allocator][present][identity] ->
 * [result][present][pool].  Every query starts unavailable with no samples.
 */
static int
occlusion_create(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_occlusion_pool *pool;
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	uint64_t device_id;
	uint64_t identity;
	uint32_t count;
	int error;
	int released;

	/* The device, the count and the identity. */
	device_id = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	if (reader->error != 0 || device == NULL || identity == 0 || count == 0U)
		return EINVAL;

	/* A pool larger than the executor keeps is refused as a feature it lacks. */
	if (count > BCM2711_VULKAN_OCCLUSION_QUERIES) {
		kern_logf("v3d: vk: vkCreateQueryPool refused: %u occlusion queries (at most %u)\n", (unsigned)count,
			  (unsigned)BCM2711_VULKAN_OCCLUSION_QUERIES);
		occlusion_reply_refusal(reply, VK_ERROR_FEATURE_NOT_PRESENT);
		return 0;
	}

	/* The pool and its queries' samples and availability. */
	pool = kern_calloc(1U, sizeof(*pool));
	if (pool != NULL) {
		pool->count = count;
		pool->samples = kern_calloc(count, sizeof(*pool->samples));
		pool->available = kern_calloc(count, sizeof(*pool->available));
	}
	if (pool == NULL || pool->samples == NULL || pool->available == NULL) {
		if (pool != NULL) {
			kern_free(pool->samples);
			kern_free(pool->available);
			kern_free(pool);
		}
		occlusion_reply_refusal(reply, VK_ERROR_OUT_OF_HOST_MEMORY);
		return 0;
	}

	/* The pool keeps its device. */
	error = bcm2711_vulkan_object_retain(device);
	if (error != 0) {
		kern_free(pool->samples);
		kern_free(pool->available);
		kern_free(pool);
		return error;
	}
	pool->device = device;

	/* Publishes the pool under its identity. */
	error = bcm2711_vulkan_object_publish(session, I915_VK_OBJ_QUERY_POOL, identity, pool, occlusion_release, &object);
	if (error != 0) {
		released = occlusion_release(session, pool);
		if (released != 0)
			return released;
		if (error != ENOMEM)
			return error;
		occlusion_reply_refusal(reply, VK_ERROR_OUT_OF_HOST_MEMORY);
		return 0;
	}

	/* Replies VK_SUCCESS and the identity. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the pool exists. */
	return 0;
}

/* vkDestroyQueryPool: [device][pool][allocator]; the records that name the pool keep it until they go. */
static int
occlusion_destroy(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader)
{
	uint64_t identity;
	int error;

	/* Skips the device, reads the pool and skips pAllocator. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The identity goes. */
	error = bcm2711_vulkan_object_remove(session, I915_VK_OBJ_QUERY_POOL, identity);
	if (error != 0)
		return error;

	/* Succeeded: the pool is no longer named. */
	return 0;
}

/*
 * vkGetQueryPoolResults: [device][pool][first][count][dataSize][bytes]
 * [stride][flags] -> [result][bytes]{bytes}.  Each query's record is its
 * samples and its availability, in words of 4 or 8 bytes, stride apart;
 * NOT_READY while any of them is unavailable (libvulkan waits).
 */
static int
occlusion_results(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_occlusion_pool *pool;
	uint64_t identity;
	uint64_t bytes;
	uint64_t stride;
	uint64_t record;
	uint64_t written;
	uint64_t end;
	uint64_t value;
	uint64_t available;
	uint32_t first;
	uint32_t count;
	uint32_t flags;
	uint32_t width;
	uint32_t query;
	uint32_t result;
	uint8_t data[16];
	uint8_t zeros[64];

	/* The range, the size, the stride and the flags. */
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

	/* A record is one or two words of four or eight bytes. */
	width = 4U;
	if ((flags & OCCLUSION_RESULT_64) != 0U)
		width = 8U;
	record = width;
	if ((flags & OCCLUSION_RESULT_AVAILABILITY) != 0U)
		record += width;

	/* An unknown pool, a range past it, and records that do not fit the reply are refused. */
	pool = occlusion_pool(session, identity);
	if (pool == NULL || first > pool->count || count > pool->count - first ||
	    bytes > OCCLUSION_REPLY_BYTES || stride < record || stride > bytes ||
	    (count != 0U && (uint64_t)(count - 1U) * stride + record > bytes)) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_DEVICE_LOST);
		drv_i915_wire_reply_u64(reply, 0U);
		return 0;
	}

	/* NOT_READY when any query of the range is unavailable. */
	result = 0U;
	for (query = first; query < first + count; query++) {
		if (__atomic_load_n(&pool->available[query], __ATOMIC_ACQUIRE) == 0U)
			result = (uint32_t)VK_NOT_READY;
	}

	/* The result, then the bytes the records take. */
	drv_i915_wire_reply_u32(reply, result);
	drv_i915_wire_reply_u64(reply, bytes);

	/* Each record at its stride (a 32-bit count saturates), then zeros to the next record and to the end. */
	written = 0U;
	kern_memset(zeros, 0, sizeof(zeros));
	for (query = 0U; query < count; query++) {
		available = __atomic_load_n(&pool->available[first + query], __ATOMIC_ACQUIRE);
		value = pool->samples[first + query];
		if (width == 4U && value > 0xffffffffULL)
			value = 0xffffffffULL;
		kern_memset(data, 0, sizeof(data));
		kern_memcpy(data, &value, width);
		kern_memcpy(data + width, &available, width);
		drv_i915_wire_reply_bytes(reply, data, (size_t)record);
		written += record;
		end = written - record + stride;
		if (end > bytes)
			end = bytes;
		while (written < end) {
			value = end - written;
			if (value > sizeof(zeros))
				value = sizeof(zeros);
			drv_i915_wire_reply_bytes(reply, zeros, (size_t)value);
			written += value;
		}
	}
	while (written < bytes) {
		value = bytes - written;
		if (value > sizeof(zeros))
			value = sizeof(zeros);
		drv_i915_wire_reply_bytes(reply, zeros, (size_t)value);
		written += value;
	}

	/* Succeeded: the records are written. */
	return 0;
}

/* Frees an occlusion pool after its last owner. */
static int
occlusion_release(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct bcm2711_vulkan_occlusion_pool *pool;
	int error;

	/* The device edge, then the pool. */
	(void)session;
	pool = payload;
	error = 0;
	if (pool->device != NULL)
		error = bcm2711_vulkan_object_release(pool->device);
	kern_free(pool->samples);
	kern_free(pool->available);
	kern_free(pool);
	if (error != 0)
		return error;

	/* Succeeded: the pool is gone. */
	return 0;
}

/* Replies a refused create: the result and an absent pool. */
static void
occlusion_reply_refusal(
	struct i915_wire_writer *reply,
	VkResult result)
{
	/* The result, then no pool. */
	drv_i915_wire_reply_u32(reply, (uint32_t)result);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u64(reply, 0U);
	return;
}

/* The occlusion pool published under an identity, or NULL. */
static struct bcm2711_vulkan_occlusion_pool *
occlusion_pool(
	struct bcm2711_vulkan_session *session,
	uint64_t identity)
{
	struct bcm2711_vulkan_object *object;

	/* A query pool whose payload is an occlusion pool. */
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_QUERY_POOL, identity);
	if (object == NULL || object->destroy != occlusion_release)
		return NULL;

	/* Succeeded: the pool. */
	return object->payload;
}
