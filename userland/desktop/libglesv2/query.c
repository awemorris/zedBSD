/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Query and sync objects of zedBSD's OpenGL ES 3.0 (WS068 p027).
 *
 * An occlusion query (GL_ANY_SAMPLES_PASSED and its conservative form)
 * may stay active across render passes and frames, but a Vulkan query
 * lives inside one render pass.  So each run of draws inside one pass
 * while the query is active is a segment of its own: a slot of the
 * context's query pool, begun before the run's first draw and ended
 * before the pass ends (gles_queries_suspend, which every place that ends
 * a pass calls first).  The query's result is whether any segment saw a
 * sample pass (desktop GL's GL_SAMPLES_PASSED in libGL: the sum of the
 * segments' counts).  A slot is reset by the upload queue when it is
 * taken.  Conditional rendering (desktop GL 3.0) waits for its query's
 * result when it starts, and draws and clears check it.
 *
 * A fence sync is the frame being recorded when it was made: it is
 * signalled once that frame is done (state->frame has moved past it);
 * waiting on it submits the frame so far and waits for it.
 *
 * Desktop GL 3.3's timer queries (libGL) are timestamps the frame's
 * command buffer writes into a second pool, in or out of a pass:
 * GL_TIME_ELAPSED one at its begin and one at its end, glQueryCounter
 * one.  The results are the queue's ticks, the bits it does not write
 * masked off, turned into nanoseconds by the device's period.
 * glGetInteger64v(GL_TIMESTAMP) submits the frame so far and has the
 * upload queue, the same queue, write one at once.
 *
 * Desktop GL 4.0's indexed queries (GL_ARB_transform_feedback3, libGL)
 * give each vertex stream its own active transform feedback query, which
 * counts the primitives written into that stream's buffers.  The other
 * targets have index 0 only.
 *
 * Desktop GL's GL_PRIMITIVES_GENERATED (3.0; of each vertex stream from
 * 4.0, libGL) counts the primitives the vertex stage gives: a draw
 * without geometry or tessellation shaders is counted on the CPU from
 * its mode and vertices (stream 0); one of a program whose geometry
 * shader captures, from the counts of each stream it keeps in its
 * capture buffer (feedback.c); any other one by a pipeline statistics
 * query of its own around it (a slot of a third pool, on a device that
 * has those queries), which counts the primitives the geometry shader
 * emitted, or without one the tessellator's that reached clipping
 * (stream 0; the others count 0).
 */

#include "gles.h"

#include <stdlib.h>
#include <string.h>

/* How many slots each of the context's query pools has. */
#define QUERY_SLOTS		1024U

/*
 * The context's query pools: the occlusion pool, the timestamp pool of
 * the timer queries (desktop GL 3.3), and the pipeline statistics pool of
 * GL_PRIMITIVES_GENERATED (desktop GL).
 */
#define QUERY_POOL_OCCLUSION	0U
#define QUERY_POOL_STAMPS	1U
#define QUERY_POOL_STATISTICS	2U
#define QUERY_POOLS		3U

/* The statistics a slot of the statistics pool counts: the primitives geometry shaders emitted, the primitives clipping took. */
#define QUERY_STATISTICS	(VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT)

/*
 * The context's query objects' shared state: the pools (the timestamp
 * pool made at the first timer query), which slots of each are free, the
 * active occlusion query, transform feedback query of each vertex stream
 * and timer query, the segment open in a pass, and the fence syncs made.
 * It lives in gles_state.queries and is made at the first query.
 */
struct gles_queries {
	VkQueryPool pools[QUERY_POOLS];
	unsigned char used[QUERY_POOLS][QUERY_SLOTS];

	/* The last frame that wrote each slot: a slot given back is taken again once that frame is done (ws068-p009). */
	uint64_t written[QUERY_POOLS][QUERY_SLOTS];
	struct gles_query *occlusion;
	struct gles_query *feedback[GLES_VERTEX_STREAMS];
	struct gles_query *elapsed;

	/* The active GL_PRIMITIVES_GENERATED query of each vertex stream (desktop GL). */
	struct gles_query *generated[GLES_VERTEX_STREAMS];

	/* The slot of the statistics query begun around a draw (gles_queries_generated). */
	uint32_t generated_slot;

	/* The segment open (NULL: none): the query, its slot and the command buffer it was begun in. */
	struct gles_query *open;
	uint32_t open_slot;
	VkCommandBuffer open_command;

	/* The fence syncs made and not deleted. */
	struct gles_sync *syncs;
};

static struct gles_queries *query_state(struct zegl_context *context, struct gles_state *state);
static struct gles_query *query_named(struct zegl_context *context, struct gles_state *state, GLuint name);
static struct gles_query **query_active(const struct zegl_display *display, struct gles_queries *queries, GLenum target);
static struct gles_query **query_target(struct zegl_context *context, struct gles_queries *queries, GLenum target, GLuint index);
static void query_begin(struct zegl_context *context, struct gles_state *state, struct gles_queries *queries, GLenum target, struct gles_query **active, GLuint id);
static void query_end(struct zegl_context *context, struct gles_state *state, struct gles_queries *queries, GLenum target, struct gles_query **active);
static void query_get(struct zegl_context *context, struct gles_state *state, GLenum target, struct gles_query **active, GLenum pname, GLint *params);
static int query_slot_take(struct gles_state *state, struct gles_queries *queries, unsigned pool, uint32_t *slot);
static void query_slots_free(struct gles_queries *queries, struct gles_query *query);
static int query_finish(struct zegl_context *context, struct gles_state *state, uint64_t frame);
static int query_result(struct zegl_context *context, struct gles_state *state, struct gles_query *query, uint64_t *result);
static int query_is_active(const struct gles_queries *queries, const struct gles_query *query);
static int query_stamps_ready(struct zegl_context *context, struct gles_state *state, struct gles_queries *queries);
static int query_statistics_ready(struct gles_state *state, struct gles_queries *queries);
static int query_keep_slot(struct gles_query *query, uint32_t slot, uint64_t frame);
static uint32_t query_vertices_primitives(uint32_t topology, uint32_t vertices, uint32_t patch_vertices);
static void query_stamp(struct zegl_context *context, struct gles_state *state, struct gles_queries *queries, struct gles_query *query);
static uint64_t query_stamp_ns(const struct gles_state *state, uint64_t ticks);
static uint64_t query_timer_result(struct gles_state *state, const struct gles_query *query);
static void query_object_result(GLuint id, GLenum pname, uint64_t *value, int *found);
static struct gles_sync *query_sync(struct gles_state *state, GLsync sync);
static int query_signalled(struct zegl_context *context, struct gles_state *state, const struct gles_sync *fence);

/*
 * Opens a segment of the active occlusion query in the pass a draw is
 * about to record into, unless one is open.  Called before each draw.
 */
void
gles_queries_draw(
	struct gles_state *state,
	const struct gles_target *target)
{
	struct gles_queries *queries;
	struct gles_query *query;
	uint32_t *slots;
	uint64_t *frames;
	uint32_t slot;
	VkQueryControlFlags control;
	int status;

	/* An active occlusion query without a segment open. */
	queries = state->queries;
	if (queries == NULL || queries->occlusion == NULL || queries->open != NULL)
		return;
	query = queries->occlusion;

	/* Room in its list of slots. */
	if (query->slot_count == query->slot_capacity) {
		slots = realloc(query->slots, (query->slot_capacity * 2U + 4U) * sizeof(*slots));
		if (slots == NULL)
			return;
		query->slots = slots;
		frames = realloc(query->frames, (query->slot_capacity * 2U + 4U) * sizeof(*frames));
		if (frames == NULL)
			return;
		query->frames = frames;
		query->slot_capacity = query->slot_capacity * 2U + 4U;
	}

	/* A reset slot. */
	status = query_slot_take(state, queries, QUERY_POOL_OCCLUSION, &slot);
	if (status != 0)
		return;

	/* Begun in the pass (counting exactly for desktop GL's count when the device can), and kept with the frame that records it. */
	control = 0U;
	if (query->target == GL_SAMPLES_PASSED && state->display->features.occlusionQueryPrecise)
		control = VK_QUERY_CONTROL_PRECISE_BIT;
	vkCmdBeginQuery(target->command, queries->pools[QUERY_POOL_OCCLUSION], slot, control);
	query->slots[query->slot_count] = slot;
	query->frames[query->slot_count] = state->frame;
	query->slot_count++;
	queries->open = query;
	queries->open_slot = slot;
	queries->open_command = target->command;
}

/*
 * Ends the open segment of an occlusion query, if there is one, before
 * the pass it is in ends (every place that ends a pass calls it first).
 */
void
gles_queries_suspend(
	struct gles_state *state)
{
	struct gles_queries *queries;

	/* A segment open. */
	queries = state->queries;
	if (queries == NULL || queries->open == NULL)
		return;

	/* Ended in the command buffer it was begun in. */
	vkCmdEndQuery(queries->open_command, queries->pools[QUERY_POOL_OCCLUSION], queries->open_slot);
	queries->open = NULL;
}

/*
 * Makes sure a frame is done, submitting and waiting for the frame being
 * recorded when it is that frame (glReadPixels's way).  Returns 0, or -1
 * with the error recorded.
 */
int
gles_frame_wait(
	struct zegl_context *context,
	struct gles_state *state,
	uint64_t frame)
{
	int status;

	/* The frame done. */
	status = query_finish(context, state, frame);
	if (status != 0)
		return -1;

	/* Succeeded: it is done. */
	return 0;
}

/*
 * Counts primitives a draw wrote into a vertex stream's transform
 * feedback buffers for the stream's active
 * GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN query, if there is one.
 */
void
gles_query_primitives(
	struct gles_state *state,
	GLuint stream,
	GLuint primitives)
{
	/* The active query of the target and stream. */
	if (state->queries == NULL ||
	    stream >= GLES_VERTEX_STREAMS ||
	    state->queries->feedback[stream] == NULL)
		return;

	/* Its count. */
	state->queries->feedback[stream]->primitives += primitives;
}

/*
 * Counts primitives a draw's geometry shader generated on a vertex
 * stream (counted in its capture buffer) for the stream's active
 * GL_PRIMITIVES_GENERATED query, if there is one.
 */
void
gles_query_generated(
	struct gles_state *state,
	GLuint stream,
	GLuint primitives)
{
	/* The active query of the target and stream. */
	if (state->queries == NULL ||
	    stream >= GLES_VERTEX_STREAMS ||
	    state->queries->generated[stream] == NULL)
		return;

	/* Its count. */
	state->queries->generated[stream]->primitives += primitives;
}

/* Reports whether a GL_PRIMITIVES_GENERATED query of any vertex stream is active. */
int
gles_queries_generating(
	const struct gles_state *state)
{
	unsigned stream;

	/* No queries yet. */
	if (state->queries == NULL)
		return 0;

	/* Each stream's. */
	for (stream = 0U; stream < GLES_VERTEX_STREAMS; stream++) {
		if (state->queries->generated[stream] != NULL)
			return 1;
	}

	/* None. */
	return 0;
}

/*
 * Counts a draw's primitives for the active GL_PRIMITIVES_GENERATED query
 * of stream 0 (desktop GL), if there is one, before the draw is recorded:
 * a draw without geometry or tessellation shaders (shaded zero) on the
 * CPU, from its topology (after strips became lists), vertices and
 * instances; one with them by a statistics query begun now in the
 * target's command buffer.  Returns nonzero when the statistics query was
 * begun (gles_queries_generated_end ends it after the draw).
 */
int
gles_queries_generated(
	struct gles_state *state,
	const struct gles_target *target,
	int shaded,
	uint32_t topology,
	uint32_t vertices,
	uint32_t instances,
	uint32_t patch_vertices)
{
	struct gles_queries *queries;
	struct gles_query *query;
	uint32_t slot;
	int ready;
	int status;

	/* An active query. */
	queries = state->queries;
	if (queries == NULL || queries->generated[0] == NULL)
		return 0;
	query = queries->generated[0];

	/* Without shaders after the vertex shader: the primitives of the vertices, of each instance. */
	if (!shaded) {
		query->primitives += query_vertices_primitives(topology, vertices, patch_vertices) * instances;
		return 0;
	}

	/* The statistics pool, and a slot of it the query keeps. */
	ready = query_statistics_ready(state, queries);
	if (!ready)
		return 0;
	status = query_slot_take(state, queries, QUERY_POOL_STATISTICS, &slot);
	if (status != 0)
		return 0;
	status = query_keep_slot(query, slot, state->frame);
	if (status != 0) {
		queries->used[QUERY_POOL_STATISTICS][slot] = 0U;
		return 0;
	}

	/* Succeeded: begun around the draw. */
	vkCmdBeginQuery(target->command, queries->pools[QUERY_POOL_STATISTICS], slot, 0U);
	queries->generated_slot = slot;
	return 1;
}

/* Ends the statistics query gles_queries_generated began around a draw. */
void
gles_queries_generated_end(
	struct gles_state *state,
	const struct gles_target *target)
{
	/* Ended in the command buffer the draw was recorded in. */
	vkCmdEndQuery(target->command, state->queries->pools[QUERY_POOL_STATISTICS], state->queries->generated_slot);
}

/*
 * Frees a context's queries, fence syncs and query pool (nothing may
 * still run).
 */
void
gles_queries_release(
	struct gles_state *state)
{
	struct gles_queries *queries;
	struct gles_query *query;
	struct gles_sync *sync;
	GLuint name;
	unsigned pool;

	/* Each query object. */
	for (name = 1U; name < state->query_objects.capacity; name++) {
		query = state->query_objects.objects[name];
		if (query == NULL)
			continue;

		/* Its slots, then the object. */
		free(query->slots);
		free(query->frames);
		free(query);
	}

	/* The namespace. */
	free(state->query_objects.objects);
	memset(&state->query_objects, 0, sizeof(state->query_objects));

	/* The fence syncs and the pool. */
	queries = state->queries;
	if (queries == NULL)
		return;
	while (queries->syncs != NULL) {
		sync = queries->syncs;
		queries->syncs = sync->next;
		free(sync);
	}

	/* The pools, then the state. */
	for (pool = 0U; pool < QUERY_POOLS; pool++) {
		if (queries->pools[pool] != VK_NULL_HANDLE)
			vkDestroyQueryPool(state->device, queries->pools[pool], NULL);
	}

	/* The state. */
	free(queries);
	state->queries = NULL;
}

/*
 * Reports the GPU's time in nanoseconds once the commands so far have
 * reached it (desktop GL 3.3's glGetInteger64v(GL_TIMESTAMP), libGL): the
 * frame so far is submitted, and the upload queue, the same queue,
 * writes a timestamp after it.  Returns 0 with *value set (0 when the
 * queue writes no timestamps), or -1 with the error recorded.
 */
int
gles_query_timestamp(
	struct zegl_context *context,
	struct gles_state *state,
	GLint64 *value)
{
	struct gles_queries *queries;
	uint64_t stamp;
	uint64_t mask;
	uint32_t slot;
	VkResult result;
	int ready;
	int status;

	/* The queries' state with its timestamp pool. */
	*value = 0;
	queries = query_state(context, state);
	if (queries == NULL)
		return -1;
	ready = query_stamps_ready(context, state, queries);
	if (!ready)
		return 0;

	/* The commands recorded so far, submitted, so that the time is after them. */
	status = query_finish(context, state, state->frame);
	if (status != 0)
		return -1;

	/* A reset slot. */
	status = query_slot_take(state, queries, QUERY_POOL_STAMPS, &slot);
	if (status != 0) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* The timestamp, written by the upload queue and waited for. */
	status = gles_upload_begin(state);
	if (status == 0) {
		vkCmdWriteTimestamp(state->upload, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries->pools[QUERY_POOL_STAMPS], slot);
		status = gles_upload_end(state);
	}

	/* The slot is free again: nothing in flight writes it. */
	queries->used[QUERY_POOL_STAMPS][slot] = 0U;
	if (status != 0) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* The ticks it wrote. */
	stamp = 0U;
	result = vkGetQueryPoolResults(state->device, queries->pools[QUERY_POOL_STAMPS], slot, 1U, sizeof(stamp), &stamp,
				       sizeof(stamp), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
	if (result != VK_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* The bits the queue writes. */
	mask = ~(uint64_t)0;
	if (state->display->timestamp_bits < 64U)
		mask = ((uint64_t)1 << state->display->timestamp_bits) - 1U;

	/* Succeeded: the time in nanoseconds. */
	*value = (GLint64)query_stamp_ns(state, stamp & mask);
	return 0;
}

/*
 * Makes names for queries; each becomes a query at its first
 * glBeginQuery.
 */
GL_APICALL void GL_APIENTRY
glGenQueries(
	GLsizei n,
	GLuint *ids)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_query *query;
	GLsizei index;
	int status;

	/* A context with its state and a count. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (n < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each name gets its object now, so the next name is another. */
	for (index = 0; index < n; index++) {
		query = calloc(1U, sizeof(*query));
		if (query == NULL) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* Under the first free name. */
		query->name = gles_names_free(&state->query_objects);
		status = gles_names_add(&state->query_objects, query->name, query);
		if (status != 0) {
			free(query);
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* The name goes back to the application. */
		ids[index] = query->name;
	}
}

/*
 * Deletes queries (an active one ends first).
 */
GL_APICALL void GL_APIENTRY
glDeleteQueries(
	GLsizei n,
	const GLuint *ids)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_queries *queries;
	struct gles_query *query;
	GLsizei index;
	unsigned stream;

	/* A context with its state and a count. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (n < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each name that is a query. */
	queries = state->queries;
	for (index = 0; index < n; index++) {
		query = gles_names_get(&state->query_objects, ids[index]);
		if (query == NULL)
			continue;

		/* No longer active or open. */
		if (queries != NULL && queries->open == query)
			gles_queries_suspend(state);
		if (queries != NULL && queries->occlusion == query)
			queries->occlusion = NULL;
		for (stream = 0U; queries != NULL && stream < GLES_VERTEX_STREAMS; stream++) {
			if (queries->feedback[stream] == query)
				queries->feedback[stream] = NULL;
		}
		if (queries != NULL && queries->elapsed == query)
			queries->elapsed = NULL;
		for (stream = 0U; queries != NULL && stream < GLES_VERTEX_STREAMS; stream++) {
			if (queries->generated[stream] == query)
				queries->generated[stream] = NULL;
		}

		/* Its slots, the name and the object go. */
		if (queries != NULL)
			query_slots_free(queries, query);
		gles_names_remove(&state->query_objects, ids[index]);
		free(query->slots);
		free(query->frames);
		free(query);
	}
}

/*
 * Reports whether a name is a query (one begun at least once).
 */
GL_APICALL GLboolean GL_APIENTRY
glIsQuery(
	GLuint id)
{
	struct gles_state *state;
	struct gles_query *query;

	/* A context with its state. */
	state = gles_state(gles_context());
	if (state == NULL)
		return GL_FALSE;

	/* A name whose object has a target. */
	query = gles_names_get(&state->query_objects, id);
	if (query == NULL || query->target == 0U)
		return GL_FALSE;

	/* Succeeded: it is one. */
	return GL_TRUE;
}

/*
 * Starts a query of a target: an occlusion query counts from here, a
 * transform feedback one the primitives written from here.
 */
GL_APICALL void GL_APIENTRY
glBeginQuery(
	GLenum target,
	GLuint id)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_queries *queries;
	struct gles_query **active;

	/* A context with the queries' state, and a target. */
	context = gles_context();
	state = gles_state(context);
	queries = query_state(context, state);
	if (queries == NULL)
		return;
	active = query_active(context->display, queries, target);
	if (active == NULL) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The query begun. */
	query_begin(context, state, queries, target, active, id);
}

/*
 * Starts a query of a target and an index: a transform feedback one of a
 * vertex stream, the others of index 0 (desktop GL 4.0,
 * GL_ARB_transform_feedback3, libGL).
 */
GL_APICALL void GL_APIENTRY
glBeginQueryIndexed(
	GLenum target,
	GLuint index,
	GLuint id)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_queries *queries;
	struct gles_query **active;

	/* A context with the queries' state, and a target and index. */
	context = gles_context();
	state = gles_state(context);
	queries = query_state(context, state);
	if (queries == NULL)
		return;
	active = query_target(context, queries, target, index);
	if (active == NULL)
		return;

	/* The query begun. */
	query_begin(context, state, queries, target, active, id);
}

/*
 * Ends the active query of a target.
 */
GL_APICALL void GL_APIENTRY
glEndQuery(
	GLenum target)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_queries *queries;
	struct gles_query **active;

	/* A context with the queries' state, and a target. */
	context = gles_context();
	state = gles_state(context);
	queries = query_state(context, state);
	if (queries == NULL)
		return;
	active = query_active(context->display, queries, target);
	if (active == NULL) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The query ended. */
	query_end(context, state, queries, target, active);
}

/*
 * Ends the active query of a target and an index (desktop GL 4.0,
 * GL_ARB_transform_feedback3, libGL).
 */
GL_APICALL void GL_APIENTRY
glEndQueryIndexed(
	GLenum target,
	GLuint index)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_queries *queries;
	struct gles_query **active;

	/* A context with the queries' state, and a target and index. */
	context = gles_context();
	state = gles_state(context);
	queries = query_state(context, state);
	if (queries == NULL)
		return;
	active = query_target(context, queries, target, index);
	if (active == NULL)
		return;

	/* The query ended. */
	query_end(context, state, queries, target, active);
}

/*
 * Reports the active query of a target (GL_CURRENT_QUERY), or the bits of
 * its counter (desktop GL's GL_QUERY_COUNTER_BITS).
 */
GL_APICALL void GL_APIENTRY
glGetQueryiv(
	GLenum target,
	GLenum pname,
	GLint *params)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_queries *queries;
	struct gles_query **active;

	/* A context with the queries' state. */
	context = gles_context();
	state = gles_state(context);
	queries = query_state(context, state);
	if (queries == NULL)
		return;

	/* glQueryCounter's target has a counter and is never active (desktop GL 3.3, a queue that writes timestamps). */
	if (target == GL_TIMESTAMP &&
	    gles_fixed != NULL &&
	    state->display->timestamp_bits != 0U) {
		if (pname == GL_QUERY_COUNTER_BITS) {
			*params = (GLint)state->display->timestamp_bits;
		} else if (pname == GL_CURRENT_QUERY) {
			*params = 0;
		} else {
			gles_error(context, GL_INVALID_ENUM);
		}
		return;
	}

	/* A target a query begins on. */
	active = query_active(context->display, queries, target);
	if (active == NULL) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* What it reports of it. */
	query_get(context, state, target, active, pname, params);
}

/*
 * Reports the active query of a target and an index (GL_CURRENT_QUERY),
 * or the bits of its counter (desktop GL 4.0, GL_ARB_transform_feedback3,
 * libGL).
 */
GL_APICALL void GL_APIENTRY
glGetQueryIndexediv(
	GLenum target,
	GLuint index,
	GLenum pname,
	GLint *params)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_queries *queries;
	struct gles_query **active;

	/* A context with the queries' state, and a target and index (glQueryCounter's has index 0 only, as glGetQueryiv reports it). */
	context = gles_context();
	state = gles_state(context);
	queries = query_state(context, state);
	if (queries == NULL)
		return;
	if (target == GL_TIMESTAMP) {
		if (index != 0U) {
			gles_error(context, GL_INVALID_VALUE);
			return;
		}
		glGetQueryiv(target, pname, params);
		return;
	}

	/* A target and index a query begins on. */
	active = query_target(context, queries, target, index);
	if (active == NULL)
		return;

	/* What it reports of it. */
	query_get(context, state, target, active, pname, params);
}

/*
 * Reports an ended query's result, or whether it is available (both are:
 * the frames that recorded it are submitted and waited for first).
 */
GL_APICALL void GL_APIENTRY
glGetQueryObjectuiv(
	GLuint id,
	GLenum pname,
	GLuint *params)
{
	uint64_t value;
	int found;

	/* The answer, cut to 32 bits (a timer's nanoseconds wrap). */
	query_object_result(id, pname, &value, &found);
	if (found)
		*params = (GLuint)value;
}

/*
 * Reports an ended query's result, or whether it is available, as a
 * 64-bit integer (desktop GL 3.3's timer queries, libGL).
 */
GL_APICALL void GL_APIENTRY
glGetQueryObjectui64v(
	GLuint id,
	GLenum pname,
	GLuint64 *params)
{
	uint64_t value;
	int found;

	/* The answer as it is. */
	query_object_result(id, pname, &value, &found);
	if (found)
		*params = value;
}

/*
 * Reports an ended query's result, or whether it is available, as a
 * signed 64-bit integer (desktop GL 3.3, libGL).
 */
GL_APICALL void GL_APIENTRY
glGetQueryObjecti64v(
	GLuint id,
	GLenum pname,
	GLint64 *params)
{
	uint64_t value;
	int found;

	/* The answer, as GL's signed type. */
	query_object_result(id, pname, &value, &found);
	if (found)
		*params = (GLint64)value;
}

/*
 * Records the GPU's time into a query once the commands so far are done
 * (desktop GL 3.3's glQueryCounter, libGL).
 */
GL_APICALL void GL_APIENTRY
glQueryCounter(
	GLuint id,
	GLenum target)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_queries *queries;
	struct gles_query *query;
	int active_now;

	/* A context with the queries' state, and the one target. */
	context = gles_context();
	state = gles_state(context);
	queries = query_state(context, state);
	if (queries == NULL)
		return;
	if (target != GL_TIMESTAMP ||
	    gles_fixed == NULL ||
	    state->display->timestamp_bits == 0U) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* A query name, not active, of this target or none yet. */
	query = query_named(context, state, id);
	if (query == NULL)
		return;
	active_now = query_is_active(queries, query);
	if (active_now || (query->target != 0U && query->target != GL_TIMESTAMP)) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Started afresh, and ended at once with its one timestamp. */
	query_slots_free(queries, query);
	query->target = GL_TIMESTAMP;
	query->ended = 1;
	query_stamp(context, state, queries, query);
}

/*
 * Starts conditional rendering (desktop GL 3.0, libGL): while it is on,
 * draws and clears do nothing when an ended occlusion query saw no sample
 * pass.  Every mode waits for the query's result.
 */
GL_APICALL void GL_APIENTRY
glBeginConditionalRender(
	GLuint id,
	GLenum mode)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_queries *queries;
	struct gles_query *query;
	uint64_t result;
	int status;

	/* A context with the queries' state, and a mode. */
	context = gles_context();
	state = gles_state(context);
	queries = query_state(context, state);
	if (queries == NULL)
		return;
	if (mode != GL_QUERY_WAIT &&
	    mode != GL_QUERY_NO_WAIT &&
	    mode != GL_QUERY_BY_REGION_WAIT &&
	    mode != GL_QUERY_BY_REGION_NO_WAIT) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Not on already, and an occlusion query begun and not active. */
	query = gles_names_get(&state->query_objects, id);
	if (state->conditional_active ||
	    query == NULL ||
	    query->target == 0U ||
	    query->target == GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN ||
	    query->target == GL_PRIMITIVES_GENERATED ||
	    query->target == GL_TIME_ELAPSED ||
	    query->target == GL_TIMESTAMP ||
	    query == queries->occlusion) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Its result, waited for. */
	status = query_result(context, state, query, &result);
	if (status != 0)
		return;

	/* Succeeded: on, skipping what follows when nothing passed. */
	state->conditional_active = 1;
	state->conditional_skip = 0;
	if (result == 0U)
		state->conditional_skip = 1;
}

/*
 * Ends conditional rendering.
 */
GL_APICALL void GL_APIENTRY
glEndConditionalRender(void)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state, rendering conditionally. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (!state->conditional_active) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Off. */
	state->conditional_active = 0;
	state->conditional_skip = 0;
}

/*
 * Makes a fence sync: signalled once the commands so far are done.
 */
GL_APICALL GLsync GL_APIENTRY
glFenceSync(
	GLenum condition,
	GLbitfield flags)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_queries *queries;
	struct gles_sync *sync;

	/* A context with the queries' state, the one condition and no flags. */
	context = gles_context();
	state = gles_state(context);
	queries = query_state(context, state);
	if (queries == NULL)
		return NULL;
	if (condition != GL_SYNC_GPU_COMMANDS_COMPLETE) {
		gles_error(context, GL_INVALID_ENUM);
		return NULL;
	}

	/* No flags. */
	if (flags != 0U) {
		gles_error(context, GL_INVALID_VALUE);
		return NULL;
	}

	/* The object, for the frame being recorded. */
	sync = calloc(1U, sizeof(*sync));
	if (sync == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return NULL;
	}

	/* The frame it waits for, in the context's list. */
	sync->frame = state->frame;
	sync->next = queries->syncs;
	queries->syncs = sync;

	/* Succeeded: the sync. */
	return (GLsync)sync;
}

/*
 * Reports whether a handle is a fence sync.
 */
GL_APICALL GLboolean GL_APIENTRY
glIsSync(
	GLsync sync)
{
	struct gles_state *state;
	struct gles_sync *fence;

	/* A context with its state. */
	state = gles_state(gles_context());
	if (state == NULL)
		return GL_FALSE;

	/* One of the context's. */
	fence = query_sync(state, sync);
	if (fence == NULL)
		return GL_FALSE;

	/* Succeeded: it is one. */
	return GL_TRUE;
}

/*
 * Deletes a fence sync (0 is ignored).
 */
GL_APICALL void GL_APIENTRY
glDeleteSync(
	GLsync sync)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_sync **link;
	struct gles_sync *fence;

	/* A context with its state; 0 is nothing. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL || sync == NULL)
		return;

	/* One of the context's. */
	fence = query_sync(state, sync);
	if (fence == NULL) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Out of the list, and freed. */
	for (link = &state->queries->syncs; *link != NULL; link = &(*link)->next) {
		if (*link == fence) {
			*link = fence->next;
			break;
		}
	}

	/* Freed. */
	free(fence);
}

/*
 * Waits for a fence sync: signalled already, signalled within the time
 * (the commands so far are submitted and waited for), or not (a time of
 * 0 does not wait).
 */
GL_APICALL GLenum GL_APIENTRY
glClientWaitSync(
	GLsync sync,
	GLbitfield flags,
	GLuint64 timeout)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_sync *fence;
	int signalled;
	int finished;

	/* A context with its state, a sync and flags it knows. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return GL_WAIT_FAILED;
	fence = query_sync(state, sync);
	if (fence == NULL) {
		gles_error(context, GL_INVALID_VALUE);
		return GL_WAIT_FAILED;
	}

	/* Flags it knows. */
	if ((flags & ~(GLbitfield)GL_SYNC_FLUSH_COMMANDS_BIT) != 0U) {
		gles_error(context, GL_INVALID_VALUE);
		return GL_WAIT_FAILED;
	}

	/* Signalled already. */
	signalled = query_signalled(context, state, fence);
	if (signalled)
		return GL_ALREADY_SIGNALED;

	/* No time to wait. */
	if (timeout == 0U)
		return GL_TIMEOUT_EXPIRED;

	/* The frame so far submitted and waited for. */
	finished = query_finish(context, state, fence->frame);
	if (finished != 0)
		return GL_WAIT_FAILED;

	/* Succeeded: signalled now. */
	return GL_CONDITION_SATISFIED;
}

/*
 * Makes the server wait for a fence sync: its commands run in order on
 * one queue already, so nothing is done.
 */
GL_APICALL void GL_APIENTRY
glWaitSync(
	GLsync sync,
	GLbitfield flags,
	GLuint64 timeout)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_sync *fence;

	/* A context with its state, a sync, no flags and no time. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	fence = query_sync(state, sync);
	if (fence == NULL || flags != 0U || timeout != GL_TIMEOUT_IGNORED)
		gles_error(context, GL_INVALID_VALUE);
}

/*
 * Reports a property of a fence sync.
 */
GL_APICALL void GL_APIENTRY
glGetSynciv(
	GLsync sync,
	GLenum pname,
	GLsizei bufSize,
	GLsizei *length,
	GLint *values)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_sync *fence;
	GLint value;
	int signalled;

	/* A context with its state, and a sync. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	fence = query_sync(state, sync);
	if (fence == NULL) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The property. */
	switch (pname) {
	case GL_OBJECT_TYPE:
		value = GL_SYNC_FENCE;
		break;
	case GL_SYNC_STATUS:
		signalled = query_signalled(context, state, fence);
		value = GL_UNSIGNALED;
		if (signalled)
			value = GL_SIGNALED;
		break;
	case GL_SYNC_CONDITION:
		value = GL_SYNC_GPU_COMMANDS_COMPLETE;
		break;
	case GL_SYNC_FLAGS:
		value = 0;
		break;
	default:
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Succeeded: the one value, when there is room. */
	if (length != NULL)
		*length = 0;
	if (bufSize < 1)
		return;
	values[0] = value;
	if (length != NULL)
		*length = 1;
}

/* Returns the context's queries' state, making it (and the query pool) at the first use; NULL with the error recorded. */
static struct gles_queries *
query_state(
	struct zegl_context *context,
	struct gles_state *state)
{
	VkQueryPoolCreateInfo create;
	struct gles_queries *queries;
	VkResult result;

	/* A context with its state, whose queries' state exists. */
	if (state == NULL)
		return NULL;
	if (state->queries != NULL)
		return state->queries;

	/* The state. */
	queries = calloc(1U, sizeof(*queries));
	if (queries == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return NULL;
	}

	/* The occlusion query pool. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	create.queryType = VK_QUERY_TYPE_OCCLUSION;
	create.queryCount = QUERY_SLOTS;
	result = vkCreateQueryPool(state->device, &create, NULL, &queries->pools[QUERY_POOL_OCCLUSION]);
	if (result != VK_SUCCESS) {
		free(queries);
		gles_error(context, GL_OUT_OF_MEMORY);
		return NULL;
	}

	/* Succeeded: kept for the context. */
	state->queries = queries;
	return queries;
}

/* Returns the query object of a name glGenQueries made, recording GL_INVALID_OPERATION when it is not one. */
static struct gles_query *
query_named(
	struct zegl_context *context,
	struct gles_state *state,
	GLuint name)
{
	struct gles_query *query;

	/* The name's object (0 is none). */
	query = NULL;
	if (name != 0U)
		query = gles_names_get(&state->query_objects, name);
	if (query == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Succeeded: the query. */
	return query;
}

/*
 * Returns where the active query of a target is kept, NULL for a name that
 * is not a query target (a timer's on a display whose queue writes no
 * timestamps).
 */
static struct gles_query **
query_active(
	const struct zegl_display *display,
	struct gles_queries *queries,
	GLenum target)
{
	/* The occlusion targets share one; transform feedback has its own (stream 0's). */
	switch (target) {
	case GL_ANY_SAMPLES_PASSED:
	case GL_ANY_SAMPLES_PASSED_CONSERVATIVE:
		return &queries->occlusion;
	case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN:
		return &queries->feedback[0];
	case GL_SAMPLES_PASSED:
		/* Desktop GL's count of samples (libGL). */
		if (gles_fixed == NULL)
			return NULL;
		return &queries->occlusion;
	case GL_TIME_ELAPSED:
		/* Desktop GL 3.3's timer (libGL), of a queue that writes timestamps. */
		if (gles_fixed == NULL || display->timestamp_bits == 0U)
			return NULL;
		return &queries->elapsed;
	case GL_PRIMITIVES_GENERATED:
		/* Desktop GL's primitives generated, of stream 0 (libGL). */
		if (gles_fixed == NULL)
			return NULL;
		return &queries->generated[0];
	default:
		break;
	}

	/* Not a query target. */
	return NULL;
}

/*
 * Returns where the active query of a target and an index is kept: a
 * transform feedback query's of a vertex stream, another target's of
 * index 0 only.  Returns NULL with the error recorded (a name that is not
 * a query target: GL_INVALID_ENUM; an index there is not:
 * GL_INVALID_VALUE).
 */
static struct gles_query **
query_target(
	struct zegl_context *context,
	struct gles_queries *queries,
	GLenum target,
	GLuint index)
{
	struct gles_query **active;

	/* Transform feedback: one of the vertex streams. */
	if (target == GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN) {
		if (index >= GLES_VERTEX_STREAMS) {
			gles_error(context, GL_INVALID_VALUE);
			return NULL;
		}
		return &queries->feedback[index];
	}

	/* The primitives generated (desktop GL): one of the vertex streams. */
	if (target == GL_PRIMITIVES_GENERATED && gles_fixed != NULL) {
		if (index >= GLES_VERTEX_STREAMS) {
			gles_error(context, GL_INVALID_VALUE);
			return NULL;
		}
		return &queries->generated[index];
	}

	/* Another target a query begins on. */
	active = query_active(context->display, queries, target);
	if (active == NULL) {
		gles_error(context, GL_INVALID_ENUM);
		return NULL;
	}

	/* Of index 0 only. */
	if (index != 0U) {
		gles_error(context, GL_INVALID_VALUE);
		return NULL;
	}

	/* Succeeded: where it is kept. */
	return active;
}

/*
 * Begins a query object on a target where its active query is kept: the
 * name a query, not active, of this target or none yet, and no query
 * active there.
 */
static void
query_begin(
	struct zegl_context *context,
	struct gles_state *state,
	struct gles_queries *queries,
	GLenum target,
	struct gles_query **active,
	GLuint id)
{
	struct gles_query *query;
	int active_now;

	/* A query name, not active, of this target or none yet, and no query of the target active. */
	query = query_named(context, state, id);
	if (query == NULL)
		return;
	active_now = query_is_active(queries, query);
	if (*active != NULL ||
	    active_now ||
	    (query->target != 0U && query->target != target)) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Started afresh. */
	query_slots_free(queries, query);
	query->target = target;
	query->primitives = 0U;
	query->ended = 0;
	*active = query;

	/* A timer query's first timestamp, after the commands so far. */
	if (target == GL_TIME_ELAPSED)
		query_stamp(context, state, queries, query);
}

/* Ends the active query kept at a target's place, which must be there. */
static void
query_end(
	struct zegl_context *context,
	struct gles_state *state,
	struct gles_queries *queries,
	GLenum target,
	struct gles_query **active)
{
	/* A query of the target active. */
	if (*active == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Its open segment ends, and a timer query's last timestamp is written after the commands it timed. */
	if (queries->open == *active)
		gles_queries_suspend(state);
	if (target == GL_TIME_ELAPSED)
		query_stamp(context, state, queries, *active);

	/* It is no longer active. */
	(*active)->ended = 1;
	*active = NULL;
}

/* Reports the active query kept at a target's place (GL_CURRENT_QUERY), or the bits of its counter (desktop GL). */
static void
query_get(
	struct zegl_context *context,
	struct gles_state *state,
	GLenum target,
	struct gles_query **active,
	GLenum pname,
	GLint *params)
{
	/* The counter's bits (desktop GL): a timer's the queue's, a count's 32. */
	if (pname == GL_QUERY_COUNTER_BITS && gles_fixed != NULL) {
		*params = 32;
		if (target == GL_TIME_ELAPSED)
			*params = (GLint)state->display->timestamp_bits;
		return;
	}

	/* The one other parameter. */
	if (pname != GL_CURRENT_QUERY) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Succeeded: its name, 0 for none. */
	*params = 0;
	if (*active != NULL)
		*params = (GLint)(*active)->name;
}

/* Takes a free slot of a pool and resets it through the upload queue; nonzero when none is free or the reset failed. */
static int
query_slot_take(
	struct gles_state *state,
	struct gles_queries *queries,
	unsigned pool,
	uint32_t *slot)
{
	uint32_t index;
	int status;

	/* The first free slot no frame still to finish writes. */
	for (index = 0U; index < QUERY_SLOTS; index++) {
		if (!queries->used[pool][index] && queries->written[pool][index] <= state->done)
			break;
	}

	/* None free. */
	if (index == QUERY_SLOTS)
		return -1;

	/* Reset before the frame that begins it runs (the upload is submitted and waited for now). */
	status = gles_upload_begin(state);
	if (status != 0)
		return -1;
	vkCmdResetQueryPool(state->upload, queries->pools[pool], index, 1U);
	status = gles_upload_end(state);
	if (status != 0)
		return -1;

	/* Succeeded: the slot is taken. */
	queries->used[pool][index] = 1U;
	*slot = index;
	return 0;
}

/* Gives a query's slots back to the pool and forgets them. */
static void
query_slots_free(
	struct gles_queries *queries,
	struct gles_query *query)
{
	unsigned index;
	unsigned pool;

	/* Each slot (of the statistics pool for the primitives generated), with the frame that wrote it. */
	pool = QUERY_POOL_OCCLUSION;
	if (query->target == GL_PRIMITIVES_GENERATED)
		pool = QUERY_POOL_STATISTICS;
	for (index = 0U; index < query->slot_count; index++) {
		queries->used[pool][query->slots[index]] = 0U;
		queries->written[pool][query->slots[index]] = query->frames[index];
	}

	/* Each timestamp's slot likewise. */
	for (index = 0U; index < query->stamp_count; index++) {
		queries->used[QUERY_POOL_STAMPS][query->stamp_slots[index]] = 0U;
		queries->written[QUERY_POOL_STAMPS][query->stamp_slots[index]] = query->stamp_frames[index];
	}

	/* The query has none. */
	query->slot_count = 0U;
	query->stamp_count = 0U;
}

/*
 * Makes sure a frame is done: the frame being recorded is submitted and
 * waited for when it is that frame and has commands; a frame in flight
 * (ws068-p009) is waited for; an earlier frame is done already.  Returns
 * 0, or -1 with the error recorded.
 */
static int
query_finish(
	struct zegl_context *context,
	struct gles_state *state,
	uint64_t frame)
{
	struct zegl_surface *surface;
	EGLint error;
	int status;

	/* A frame known to be done. */
	if (frame <= state->done)
		return 0;

	/* A frame in flight: everything submitted is waited for. */
	if (frame < state->frame) {
		status = gles_frame_retire(context, state);
		return status;
	}

	/* Nothing recorded to submit. */
	surface = context->draw;
	if (surface == NULL || !surface->frame_open)
		return 0;

	/* The passes end, and what was recorded is submitted and waited for. */
	gles_queries_suspend(state);
	gles_target_close(state);
	zegl_frame_leave_pass(surface);
	surface->recorded = 1;
	error = zegl_frame_flush(surface);
	if (error != EGL_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* Everything recorded before is done: the frame's resources are free again. */
	gles_frame_finished(state, 1);

	/* Succeeded: the frame is done. */
	return 0;
}

/*
 * Finds an ended query's result, the frames that recorded it done first:
 * the primitives a transform feedback query counted, the samples that
 * passed for desktop GL's GL_SAMPLES_PASSED (exact when the device counts
 * exactly), or whether any passed, or a timer query's nanoseconds.
 * Returns 0, or -1 with the error recorded.
 */
static int
query_result(
	struct zegl_context *context,
	struct gles_state *state,
	struct gles_query *query,
	uint64_t *result)
{
	uint64_t latest;
	uint32_t samples;
	uint32_t statistics[2];
	unsigned index;
	VkResult status;
	int finished;

	/* The latest frame that recorded a segment or a timestamp. */
	latest = 0U;
	for (index = 0U; index < query->slot_count; index++) {
		if (query->frames[index] > latest)
			latest = query->frames[index];
	}
	for (index = 0U; index < query->stamp_count; index++) {
		if (query->stamp_frames[index] > latest)
			latest = query->stamp_frames[index];
	}

	/* That frame done: submitted and waited for when it is the one being recorded. */
	finished = query_finish(context, state, latest);
	if (finished != 0)
		return -1;

	/* A transform feedback query's count. */
	if (query->target == GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN) {
		*result = query->primitives;
		return 0;
	}

	/*
	 * The primitives generated: those counted on the CPU, and of each
	 * draw with a statistics query the geometry shader's (or, without one,
	 * those clipping took).
	 */
	if (query->target == GL_PRIMITIVES_GENERATED) {
		*result = query->primitives;
		for (index = 0U; index < query->slot_count; index++) {
			memset(statistics, 0, sizeof(statistics));
			status = vkGetQueryPoolResults(state->device, state->queries->pools[QUERY_POOL_STATISTICS], query->slots[index], 1U,
						       sizeof(statistics), statistics, sizeof(statistics), VK_QUERY_RESULT_WAIT_BIT);
			if (status != VK_SUCCESS)
				continue;
			if (statistics[0] != 0U) {
				*result += statistics[0];
			} else {
				*result += statistics[1];
			}
		}
		return 0;
	}

	/* A timer query's nanoseconds. */
	if (query->target == GL_TIME_ELAPSED || query->target == GL_TIMESTAMP) {
		*result = query_timer_result(state, query);
		return 0;
	}

	/* An occlusion query: the samples of every segment. */
	*result = 0U;
	for (index = 0U; index < query->slot_count; index++) {
		samples = 0U;
		status = vkGetQueryPoolResults(state->device, state->queries->pools[QUERY_POOL_OCCLUSION], query->slots[index], 1U,
					       sizeof(samples), &samples, sizeof(samples), VK_QUERY_RESULT_WAIT_BIT);
		if (status == VK_SUCCESS)
			*result += samples;
	}

	/* Succeeded: a count, or whether any passed. */
	if (query->target != GL_SAMPLES_PASSED && *result != 0U)
		*result = GL_TRUE;
	return 0;
}

/* Reports whether a query is the active one of a target. */
static int
query_is_active(
	const struct gles_queries *queries,
	const struct gles_query *query)
{
	unsigned stream;

	/* The active occlusion and timer queries. */
	if (query == queries->occlusion)
		return 1;
	if (query == queries->elapsed)
		return 1;

	/* The active transform feedback and primitives generated query of each vertex stream. */
	for (stream = 0U; stream < GLES_VERTEX_STREAMS; stream++) {
		if (query == queries->feedback[stream])
			return 1;
		if (query == queries->generated[stream])
			return 1;
	}

	/* Not active. */
	return 0;
}

/*
 * Makes the pipeline statistics pool at the first draw with geometry or
 * tessellation shaders a GL_PRIMITIVES_GENERATED query counts, when the
 * device has the queries.  Returns nonzero when the pool is there.
 */
static int
query_statistics_ready(
	struct gles_state *state,
	struct gles_queries *queries)
{
	VkQueryPoolCreateInfo create;
	VkResult result;

	/* Made already, or never: the device has no such queries. */
	if (queries->pools[QUERY_POOL_STATISTICS] != VK_NULL_HANDLE)
		return 1;
	if (!state->display->features.pipelineStatisticsQuery)
		return 0;

	/* The pool. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	create.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
	create.queryCount = QUERY_SLOTS;
	create.pipelineStatistics = QUERY_STATISTICS;
	result = vkCreateQueryPool(state->device, &create, NULL, &queries->pools[QUERY_POOL_STATISTICS]);
	if (result != VK_SUCCESS) {
		queries->pools[QUERY_POOL_STATISTICS] = VK_NULL_HANDLE;
		return 0;
	}

	/* Succeeded: the draws can be counted. */
	return 1;
}

/* Adds a slot and the frame that writes it to a query's list; nonzero without memory. */
static int
query_keep_slot(
	struct gles_query *query,
	uint32_t slot,
	uint64_t frame)
{
	uint32_t *slots;
	uint64_t *frames;

	/* Room in the list. */
	if (query->slot_count == query->slot_capacity) {
		slots = realloc(query->slots, (query->slot_capacity * 2U + 4U) * sizeof(*slots));
		if (slots == NULL)
			return -1;
		query->slots = slots;
		frames = realloc(query->frames, (query->slot_capacity * 2U + 4U) * sizeof(*frames));
		if (frames == NULL)
			return -1;
		query->frames = frames;
		query->slot_capacity = query->slot_capacity * 2U + 4U;
	}

	/* Succeeded: the slot kept. */
	query->slots[query->slot_count] = slot;
	query->frames[query->slot_count] = frame;
	query->slot_count++;
	return 0;
}

/*
 * Returns how many primitives some vertices of a topology make (whole
 * primitives only): a list's per primitive, a strip's with adjacency, a
 * patch list's of its vertices a patch.
 */
static uint32_t
query_vertices_primitives(
	uint32_t topology,
	uint32_t vertices,
	uint32_t patch_vertices)
{
	/* The topologies a draw records (strips, loops and fans became lists, but for those with adjacency). */
	switch (topology) {
	case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
		return vertices;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
		return vertices / 2U;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
		return vertices / 3U;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
		return vertices / 4U;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
		return vertices / 6U;
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
		if (vertices < 4U)
			return 0U;
		return vertices - 3U;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
		if (vertices < 6U)
			return 0U;
		return (vertices - 4U) / 2U;
	case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
		if (patch_vertices == 0U)
			return 0U;
		return vertices / patch_vertices;
	default:
		break;
	}

	/* A topology a draw does not record. */
	return 0U;
}

/*
 * Makes the timestamp pool at the first timer query, when the queue
 * writes timestamps.  Returns nonzero when the pool is there; zero when
 * the queue writes none (the timers read 0) or the pool could not be made
 * (GL_OUT_OF_MEMORY recorded).
 */
static int
query_stamps_ready(
	struct zegl_context *context,
	struct gles_state *state,
	struct gles_queries *queries)
{
	VkQueryPoolCreateInfo create;
	VkResult result;

	/* Made already, or never: the queue writes no timestamps. */
	if (queries->pools[QUERY_POOL_STAMPS] != VK_NULL_HANDLE)
		return 1;
	if (state->display->timestamp_bits == 0U)
		return 0;

	/* The pool. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	create.queryType = VK_QUERY_TYPE_TIMESTAMP;
	create.queryCount = QUERY_SLOTS;
	result = vkCreateQueryPool(state->device, &create, NULL, &queries->pools[QUERY_POOL_STAMPS]);
	if (result != VK_SUCCESS) {
		queries->pools[QUERY_POOL_STAMPS] = VK_NULL_HANDLE;
		gles_error(context, GL_OUT_OF_MEMORY);
		return 0;
	}

	/* Succeeded: timestamps can be written. */
	return 1;
}

/*
 * Has the draw surface's frame write a timestamp of a timer query once
 * the commands recorded so far are done.  Without a surface, a queue that
 * writes timestamps, or a free slot, nothing is written and the result
 * is 0.
 */
static void
query_stamp(
	struct zegl_context *context,
	struct gles_state *state,
	struct gles_queries *queries,
	struct gles_query *query)
{
	struct zegl_surface *surface;
	uint32_t slot;
	EGLint error;
	int ready;
	int status;

	/* A surface whose frame records it, and room for the timestamp. */
	surface = context->draw;
	if (surface == NULL || query->stamp_count >= 2U)
		return;

	/* The timestamp pool. */
	ready = query_stamps_ready(context, state, queries);
	if (!ready)
		return;

	/* A reset slot. */
	status = query_slot_take(state, queries, QUERY_POOL_STAMPS, &slot);
	if (status != 0)
		return;

	/* The frame, opened when this is its first command. */
	error = zegl_frame_begin(surface);
	if (error != EGL_SUCCESS) {
		queries->used[QUERY_POOL_STAMPS][slot] = 0U;
		gles_report("the frame", error);
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/*
	 * Written once everything before it is done, in a pass or out of one;
	 * recorded makes the frame be submitted even when nothing else was.
	 */
	vkCmdWriteTimestamp(surface->command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries->pools[QUERY_POOL_STAMPS], slot);
	surface->recorded = 1;
	query->stamp_slots[query->stamp_count] = slot;
	query->stamp_frames[query->stamp_count] = state->frame;
	query->stamp_count++;
}

/* Returns the nanoseconds of a count of the queue's ticks (the device's timestampPeriod each). */
static uint64_t
query_stamp_ns(
	const struct gles_state *state,
	uint64_t ticks)
{
	double nanoseconds;

	/* The ticks at the device's period. */
	nanoseconds = (double)ticks * (double)state->limits.timestampPeriod;

	/* Succeeded: whole nanoseconds. */
	return (uint64_t)nanoseconds;
}

/*
 * Returns a timer query's nanoseconds once its frames are done: the time
 * between GL_TIME_ELAPSED's two timestamps, or glQueryCounter's one; 0
 * when its timestamps were not written.
 */
static uint64_t
query_timer_result(
	struct gles_state *state,
	const struct gles_query *query)
{
	uint64_t stamps[2];
	uint64_t mask;
	unsigned wanted;
	unsigned index;
	VkResult status;

	/* Every timestamp the query needs. */
	wanted = 2U;
	if (query->target == GL_TIMESTAMP)
		wanted = 1U;
	if (query->stamp_count < wanted)
		return 0U;

	/* Each one, waited for. */
	for (index = 0U; index < wanted; index++) {
		stamps[index] = 0U;
		status = vkGetQueryPoolResults(state->device, state->queries->pools[QUERY_POOL_STAMPS], query->stamp_slots[index], 1U,
					       sizeof(stamps[index]), &stamps[index], sizeof(stamps[index]),
					       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
		if (status != VK_SUCCESS)
			return 0U;
	}

	/* The bits the queue writes. */
	mask = ~(uint64_t)0;
	if (state->display->timestamp_bits < 64U)
		mask = ((uint64_t)1 << state->display->timestamp_bits) - 1U;

	/* glQueryCounter's time. */
	if (wanted == 1U)
		return query_stamp_ns(state, stamps[0] & mask);

	/* Succeeded: the time between the two (a counter that wrapped once stays right under the mask). */
	return query_stamp_ns(state, (stamps[1] - stamps[0]) & mask);
}

/*
 * Finds an ended query's result or whether it is available
 * (glGetQueryObject*v): *found is nonzero with *value set, zero with the
 * error recorded.  Both wait for the frames that recorded the query.
 */
static void
query_object_result(
	GLuint id,
	GLenum pname,
	uint64_t *value,
	int *found)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_queries *queries;
	struct gles_query *query;
	uint64_t result;
	int active_now;
	int finished;

	/* A context with the queries' state, and a parameter. */
	*found = 0;
	context = gles_context();
	state = gles_state(context);
	queries = query_state(context, state);
	if (queries == NULL)
		return;
	if (pname != GL_QUERY_RESULT && pname != GL_QUERY_RESULT_AVAILABLE) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* A query begun and not active. */
	query = gles_names_get(&state->query_objects, id);
	if (query == NULL || query->target == 0U) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Not active. */
	active_now = query_is_active(queries, query);
	if (active_now) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The result, once the frames that recorded it are done. */
	finished = query_result(context, state, query, &result);
	if (finished != 0)
		return;

	/* Available: the frames are done. */
	*found = 1;
	if (pname == GL_QUERY_RESULT_AVAILABLE) {
		*value = GL_TRUE;
		return;
	}

	/* Succeeded: the result. */
	*value = result;
}

/* Returns the context's fence sync a handle names, or NULL. */
static struct gles_sync *
query_sync(
	struct gles_state *state,
	GLsync sync)
{
	struct gles_sync *fence;

	/* The list of the context's. */
	if (state->queries == NULL)
		return NULL;
	for (fence = state->queries->syncs; fence != NULL; fence = fence->next) {
		if ((GLsync)fence == sync)
			return fence;
	}

	/* Not one. */
	return NULL;
}

/* Reports whether a fence sync is signalled: its frame is done, or nothing of it waits to be submitted. */
static int
query_signalled(
	struct zegl_context *context,
	struct gles_state *state,
	const struct gles_sync *fence)
{
	struct zegl_surface *surface;
	int settled;

	/* A frame known to be done. */
	if (fence->frame <= state->done)
		return 1;

	/* A frame in flight (ws068-p009) is done once the surface that presented it has none left; the done frame moves on then. */
	surface = context->draw;
	if (fence->frame < state->frame) {
		settled = 0;
		if (surface != NULL && surface == state->flight)
			settled = zegl_surface_settled(surface);
		if (settled)
			state->done = state->frame - 1U;
		return settled;
	}

	/* The frame being recorded with nothing recorded is done too. */
	if (surface == NULL || !surface->frame_open)
		return 1;

	/* Not yet. */
	return 0;
}
