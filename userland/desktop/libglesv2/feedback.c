/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Transform feedback of zedBSD's OpenGL ES 3.0 (WS068 p030).
 *
 * libvulkan offers no transform feedback extension, so a program that
 * captures outputs has its vertex shader write them into a storage buffer
 * (the GLSL compiler's capture, glsl.h): a record per vertex the draw
 * fetches, placed by the instance and the vertex number.  After the draw
 * the records are copied into the transform feedback buffers in the order
 * GL writes vertices (the draw's list of vertices, strips and fans made
 * lists), outside the render pass.  The buffers' device copies are then
 * newer than their bytes on the CPU (gpu_written) until gles_buffer_fetch
 * reads them back.
 *
 * Desktop GL (libGL) adds GL_ARB_transform_feedback2 and 3: the vertices
 * a capture wrote are counted on the CPU (a draw that does not fit is
 * refused, so every draw's vertices are written whole) and kept by the
 * object when the capture ends, for glDrawTransformFeedback to draw as
 * glDrawArrays from 0.  Interleaved names may go into several buffers
 * (gl_NextBuffer) and skip components (gl_SkipComponents), whose bytes
 * the copies leave as they are.
 *
 * A program with a geometry shader captures what it emits (desktop GL
 * 3.2 and 4.0's vertex streams; after a tessellation evaluation shader
 * the link adds one that passes the primitives on).  How many vertices
 * it emits is known only once it ran, so it writes a record of each
 * vertex at the place an atomic counter gives, with a key of GL's order
 * (glsl.h), into an area of the draw's own for each instance (the draw is
 * made one instance at a time, as the shader cannot tell them apart).
 * After the draw the frame is submitted and waited for, and the CPU
 * sorts the records by their keys, makes each stream's strips into
 * primitives of the capture's kind, and writes those that fit into the
 * stream's buffers' bytes (a primitive that does not fit is left out, as
 * GL leaves it); the areas' counts of generated primitives go to the
 * queries.  With tessellation the number of primitives is not known
 * beforehand, so the room for records is twice what the buffers take:
 * a capture that overflows them may keep other primitives than GL's
 * first ones.
 */

#include "gles.h"
#include "glsl/glsl.h"

#include <stdlib.h>
#include <string.h>

/* The most bytes of a draw's geometry capture areas (the records past it are left out). */
#define FEEDBACK_AREAS_MAX	((size_t)128U * 1024U * 1024U)

/* A record of a geometry shader's capture as the CPU sorts them: its key (glsl.h) and its words. */
struct feedback_record {
	uint32_t key[GLSL_CAPTURE_KEY];
	const uint32_t *words;
};

static struct gles_feedback *feedback_named(struct gles_state *state, GLuint name);
static unsigned feedback_vertices(GLenum mode);
static size_t feedback_room(const struct gles_buffer_range *range, size_t written);
static int feedback_copy(struct gles_state *state, VkCommandBuffer command, const struct gles_capture_target *capture, unsigned binding, GLint first, GLsizei instances, const uint32_t *list, uint32_t expanded);
static void feedback_draw(GLenum mode, GLuint id, GLuint stream, GLsizei instances);
static uint32_t feedback_geometry_room(struct gles_state *state, const struct gles_program *program, uint32_t expanded);
static int feedback_geometry_buffers(struct zegl_context *context, struct gles_state *state);
static void feedback_geometry_write(struct gles_state *state, const uint32_t *header, uint32_t room);
static int feedback_geometry_order(const void *left, const void *right);
static void feedback_geometry_primitive(struct gles_state *state, unsigned stream, const uint32_t *const *vertices, unsigned count);

/*
 * Checks a draw against the active transform feedback: a mode of the
 * capture's kind only, and in OpenGL ES 3.0 glDrawArrays only (desktop
 * GL captures indexed draws too).  *capturing is nonzero when the draw
 * captures (active, not paused, the program that began it).  Returns 0,
 * or -1 with the error recorded.
 */
int
gles_feedback_check(
	struct zegl_context *context,
	struct gles_state *state,
	GLenum mode,
	GLenum type,
	int *capturing)
{
	struct gles_feedback *feedback;
	unsigned wanted;
	unsigned kind;

	/* Nothing captured while transform feedback is not active or paused. */
	*capturing = 0;
	feedback = state->feedback;
	if (!feedback->active || feedback->paused)
		return 0;

	/* The mode is of the capture's kind (a capturing geometry shader's output primitive is). */
	wanted = feedback_vertices(mode);
	if (state->program != NULL &&
	    state->program->capture_stage == GLSL_STAGE_GEOMETRY &&
	    state->program->capture_count != 0U)
		wanted = feedback_vertices(state->program->capture_primitive);
	kind = feedback_vertices(feedback->primitive_mode);
	if (wanted != kind) {
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}

	/* OpenGL ES 3.0 draws no indices while capturing. */
	if (type != GL_NONE && gles_fixed == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}

	/* Succeeded: the draw captures with the program that began it. */
	if (state->program == feedback->program && feedback->program->capture_count != 0U)
		*capturing = 1;
	return 0;
}

/*
 * Makes the capture buffer of a draw with a program that captures
 * outputs, in the stream (its header says how many vertices an instance
 * has: one past the largest vertex number the draw reads, first + count
 * for glDrawArrays; the vertex shader writes it whether or not the draw
 * captures), after checking, for a draw that captures, that the transform
 * feedback buffers have room for the vertices it writes.  Returns 0, or
 * -1 with the error recorded.
 */
int
gles_feedback_prepare(
	struct zegl_context *context,
	struct gles_state *state,
	int capturing,
	uint32_t numbers,
	GLsizei instances,
	uint32_t expanded,
	struct gles_capture_target *capture)
{
	struct gles_program *program;
	uint32_t *header;
	size_t vertices;
	size_t bytes;
	size_t room;
	size_t alignment;
	unsigned binding;

	/* The vertices written. */
	program = state->program;
	vertices = (size_t)expanded * (size_t)instances;

	/* Room when capturing in each binding point written: a vertex takes its buffer's words (skipped ones too). */
	if (capturing) {
		for (binding = 0U; binding < program->capture_buffers; binding++) {
			bytes = vertices * program->capture_buffer_words[binding] * 4U;
			room = feedback_room(&state->feedback_ranges[binding], state->feedback->written[binding]);
			if (bytes > room) {
				gles_error(context, GL_INVALID_OPERATION);
				return -1;
			}
		}
	}

	/* The capture buffer: the header, then a record for every vertex number of every instance. */
	capture->vertices = numbers;
	bytes = ((size_t)GLES_CAPTURE_HEADER + (size_t)instances * capture->vertices * program->capture_stride) * 4U;
	alignment = (size_t)state->limits.minStorageBufferOffsetAlignment;
	if (alignment < 16U)
		alignment = 16U;
	header = gles_stream(state, bytes, alignment, &capture->buffer.buffer, &capture->buffer.offset);
	if (header == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* The header: the vertices an instance has. */
	memset(header, 0, GLES_CAPTURE_HEADER * 4U);
	header[0] = capture->vertices;
	capture->buffer.range = bytes;

	/* Succeeded: the capture buffer. */
	return 0;
}

/*
 * Copies a capturing draw's records into the transform feedback buffers
 * in GL's order (the draw's list of vertices, or first on), outside the
 * render pass, and counts what was written.
 */
void
gles_feedback_record(
	struct zegl_context *context,
	struct gles_state *state,
	const struct gles_capture_target *capture,
	GLint first,
	GLsizei instances,
	const uint32_t *list,
	uint32_t expanded)
{
	struct zegl_surface *surface;
	struct gles_program *program;
	struct gles_feedback *feedback;
	VkMemoryBarrier barrier;
	size_t vertices;
	unsigned binding;
	unsigned primitive;
	int status;

	/* The frame's command buffer, with no pass open (copies are made outside one). */
	surface = context->draw;
	program = state->feedback->program;
	feedback = state->feedback;
	gles_queries_suspend(state);
	gles_target_close(state);
	zegl_frame_leave_pass(surface);

	/* The vertex shader's writes, seen by the copies. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(surface->command, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			     0U, 1U, &barrier, 0U, NULL, 0U, NULL);

	/* Each binding point written: the outputs that go into it, after what was written (skipped components too). */
	vertices = (size_t)expanded * (size_t)instances;
	for (binding = 0U; binding < program->capture_buffers; binding++) {
		if (program->capture_buffer_words[binding] == 0U)
			continue;
		status = feedback_copy(state, surface->command, capture, binding, first, instances, list, expanded);
		if (status == 0)
			feedback->written[binding] += vertices * program->capture_buffer_words[binding] * 4U;
	}

	/* The copies seen by what reads the buffers after (vertices, indices, uniforms, copies). */
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
				VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(surface->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
			     0U, 1U, &barrier, 0U, NULL, 0U, NULL);
	surface->recorded = 1;

	/* The vertices captured (into stream 0), for glDrawTransformFeedback, and the primitives written, for the query of them. */
	feedback->vertices[0] += vertices;
	primitive = feedback_vertices(feedback->primitive_mode);
	gles_query_primitives(state, 0U, (GLuint)(expanded / primitive) * (GLuint)instances);
}

/*
 * Makes the capture areas of a draw whose geometry shader captures
 * outputs, one per instance in the stream (glsl.h): a header with the
 * room for records (none when the draw does not capture: then every
 * record goes to the one past the room) and the counts at 0, and that
 * one record more.  Returns 0, or -1 with the error recorded.
 */
int
gles_feedback_geometry_prepare(
	struct zegl_context *context,
	struct gles_state *state,
	int capturing,
	uint32_t expanded,
	GLsizei instances,
	struct gles_capture_target *capture)
{
	struct gles_program *program;
	unsigned char *bytes;
	uint32_t *header;
	size_t record;
	size_t area;
	size_t alignment;
	size_t most;
	uint32_t room;
	GLsizei instance;

	/* The room for records of each instance, and a record's bytes. */
	program = state->program;
	room = 0U;
	if (capturing)
		room = feedback_geometry_room(state, program, expanded);
	record = ((size_t)GLSL_CAPTURE_KEY + program->capture_stride) * 4U;

	/* No more than the areas' most bytes in all. */
	most = FEEDBACK_AREAS_MAX / (size_t)instances / record;
	if (most < 2U) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}
	if ((size_t)room > most - 2U)
		room = (uint32_t)(most - 2U);

	/* An area: the header, the records with room and the one past it, at the alignment of a storage buffer's offset. */
	alignment = (size_t)state->limits.minStorageBufferOffsetAlignment;
	if (alignment < 16U)
		alignment = 16U;
	area = (size_t)GLSL_CAPTURE_GEOMETRY_HEADER * 4U + ((size_t)room + 1U) * record;
	area = (area + alignment - 1U) / alignment * alignment;
	bytes = gles_stream(state, area * (size_t)instances, alignment, &capture->buffer.buffer, &capture->buffer.offset);
	if (bytes == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* Each instance's header: the room, nothing counted. */
	for (instance = 0; instance < instances; instance++) {
		header = (uint32_t *)(bytes + (size_t)instance * area);
		memset(header, 0, GLSL_CAPTURE_GEOMETRY_HEADER * 4U);
		header[GLSL_CAPTURE_ROOM] = room;
	}

	/* Succeeded: the areas, the first one's range described. */
	capture->buffer.range = area;
	capture->vertices = 0U;
	capture->areas = (uint32_t *)bytes;
	capture->area = area;
	capture->room = room;
	return 0;
}

/* Describes the capture area of an instance of a draw whose geometry shader captures. */
void
gles_feedback_geometry_area(
	const struct gles_capture_target *capture,
	GLsizei instance,
	VkDescriptorBufferInfo *area)
{
	/* The first one's range, instance areas on. */
	*area = capture->buffer;
	area->offset += (VkDeviceSize)instance * capture->area;
}

/*
 * After a draw whose geometry shader captures, when it captured or a
 * GL_PRIMITIVES_GENERATED query is active: the frame submitted and
 * waited for, then each instance's records written into the transform
 * feedback buffers in GL's order (when capturing) and the primitives
 * each stream generated counted for the queries.
 */
void
gles_feedback_geometry_record(
	struct zegl_context *context,
	struct gles_state *state,
	const struct gles_capture_target *capture,
	int capturing,
	GLsizei instances)
{
	struct zegl_surface *surface;
	VkMemoryBarrier barrier;
	const uint32_t *header;
	GLuint generated[GLES_VERTEX_STREAMS];
	GLsizei instance;
	unsigned stream;
	int generating;
	int status;

	/* Nothing waited for when nothing reads the areas. */
	generating = gles_queries_generating(state);
	if (!capturing && !generating)
		return;

	/* The frame's command buffer, with no pass open. */
	surface = context->draw;
	gles_queries_suspend(state);
	gles_target_close(state);
	zegl_frame_leave_pass(surface);

	/* The geometry shader's writes, seen by the CPU once the frame is done. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(surface->command, VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
			     0U, 1U, &barrier, 0U, NULL, 0U, NULL);
	surface->recorded = 1;

	/* The frame done. */
	status = gles_frame_wait(context, state, state->frame);
	if (status != 0)
		return;

	/* The transform feedback buffers' bytes up to date and writable on the CPU. */
	if (capturing) {
		status = feedback_geometry_buffers(context, state);
		if (status != 0) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}
	}

	/* Each instance's area in turn: its records, and its counts of primitives generated. */
	memset(generated, 0, sizeof(generated));
	for (instance = 0; instance < instances; instance++) {
		header = (const uint32_t *)((const unsigned char *)capture->areas + (size_t)instance * capture->area);
		if (capturing)
			feedback_geometry_write(state, header, capture->room);
		for (stream = 0U; stream < GLES_VERTEX_STREAMS; stream++)
			generated[stream] += header[GLSL_CAPTURE_GENERATED + stream];
	}

	/* The queries of the primitives generated. */
	for (stream = 0U; stream < GLES_VERTEX_STREAMS; stream++)
		gles_query_generated(state, stream, generated[stream]);
}

/*
 * Brings a buffer's bytes up to date with its device copy when transform
 * feedback wrote it, waiting for the frame that did.  Returns 0, or -1
 * with the error recorded.
 */
int
gles_buffer_fetch(
	struct zegl_context *context,
	struct gles_buffer *buffer)
{
	struct gles_state *state;
	uint64_t started;
	int status;

	/* Nothing written by the device. */
	if (!buffer->gpu_written)
		return 0;

	/* The frame that wrote it done. */
	state = gles_state(context);
	if (state == NULL)
		return -1;
	started = gles_time_begin();
	status = gles_frame_wait(context, state, buffer->used);
	gles_time_end("frame-wait", started, 0U);
	if (status != 0)
		return -1;

	/* The device copy's bytes (host visible) are the buffer's, unless they live in it (ws101-p017). */
	started = gles_time_begin();
	if (!buffer->on_device && buffer->mapped != NULL && buffer->device_size >= buffer->size && buffer->size != 0U)
		memcpy(buffer->data, buffer->mapped, buffer->size);
	gles_time_end("readback", started, buffer->size);

	/* Succeeded: the bytes are the newest again. */
	buffer->gpu_written = 0;
	return 0;
}

/*
 * Frees a context's transform feedback objects and their namespace.
 */
void
gles_feedbacks_release(
	struct gles_state *state)
{
	GLuint name;

	/* Each object. */
	for (name = 1U; name < state->feedbacks.capacity; name++)
		free(state->feedbacks.objects[name]);

	/* The namespace. */
	free(state->feedbacks.objects);
	memset(&state->feedbacks, 0, sizeof(state->feedbacks));
	state->feedback = &state->default_feedback;
}

/*
 * Makes names for transform feedback objects.
 */
GL_APICALL void GL_APIENTRY
glGenTransformFeedbacks(
	GLsizei n,
	GLuint *ids)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_feedback *feedback;
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
		feedback = calloc(1U, sizeof(*feedback));
		if (feedback == NULL) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* Under the first free name. */
		feedback->name = gles_names_free(&state->feedbacks);
		status = gles_names_add(&state->feedbacks, feedback->name, feedback);
		if (status != 0) {
			free(feedback);
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* The name goes back to the application. */
		ids[index] = feedback->name;
	}
}

/*
 * Deletes transform feedback objects (not an active one); a bound one
 * gives way to the default.
 */
GL_APICALL void GL_APIENTRY
glDeleteTransformFeedbacks(
	GLsizei n,
	const GLuint *ids)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_feedback *feedback;
	GLsizei index;

	/* A context with its state and a count. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (n < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each name that is an object, not active. */
	for (index = 0; index < n; index++) {
		feedback = feedback_named(state, ids[index]);
		if (feedback == NULL || feedback == &state->default_feedback)
			continue;
		if (feedback->active) {
			gles_error(context, GL_INVALID_OPERATION);
			continue;
		}

		/* The default one bound in its place. */
		if (state->feedback == feedback) {
			memcpy(state->feedback_ranges, state->default_feedback.ranges, sizeof(state->feedback_ranges));
			state->feedback = &state->default_feedback;
		}

		/* The name and the object go. */
		gles_names_remove(&state->feedbacks, ids[index]);
		free(feedback);
	}
}

/*
 * Reports whether a name is a transform feedback object (one bound at
 * least once).
 */
GL_APICALL GLboolean GL_APIENTRY
glIsTransformFeedback(
	GLuint id)
{
	struct gles_state *state;
	struct gles_feedback *feedback;

	/* A context with its state. */
	state = gles_state(gles_context());
	if (state == NULL || id == 0U)
		return GL_FALSE;

	/* A name bound once. */
	feedback = feedback_named(state, id);
	if (feedback == NULL || !feedback->bound)
		return GL_FALSE;

	/* Succeeded: it is one. */
	return GL_TRUE;
}

/*
 * Binds a transform feedback object (0: the default one); its binding
 * points become the context's.
 */
GL_APICALL void GL_APIENTRY
glBindTransformFeedback(
	GLenum target,
	GLuint id)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_feedback *feedback;

	/* A context with its state, the one target, and no capture going on. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (target != GL_TRANSFORM_FEEDBACK) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Not while capturing. */
	if (state->feedback->active && !state->feedback->paused) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A name glGenTransformFeedbacks made. */
	feedback = feedback_named(state, id);
	if (feedback == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The bound one keeps its binding points; the new one's are the context's. */
	memcpy(state->feedback->ranges, state->feedback_ranges, sizeof(state->feedback_ranges));
	memcpy(state->feedback_ranges, feedback->ranges, sizeof(state->feedback_ranges));
	feedback->bound = 1;
	state->feedback = feedback;
}

/*
 * Starts capturing primitives of a mode (points, lines or triangles) with
 * the current program into the bound transform feedback buffers.
 */
GL_APICALL void GL_APIENTRY
glBeginTransformFeedback(
	GLenum primitiveMode)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_program *program;
	unsigned binding;

	/* A context with its state and a mode. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (primitiveMode != GL_POINTS && primitiveMode != GL_LINES && primitiveMode != GL_TRIANGLES) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Not active yet, with a linked program that captures outputs (the current one, or a program pipeline object's). */
	program = gles_program_current(context, state);
	if (state->feedback->active || program == NULL || !program->linked || program->capture_count == 0U) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A buffer at every binding point the capture writes (one gl_NextBuffer passed over without a component needs none). */
	for (binding = 0U; binding < program->capture_buffers; binding++) {
		if (program->capture_buffer_words[binding] == 0U)
			continue;
		if (state->feedback_ranges[binding].buffer == NULL) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}
	}

	/* Active, writing from the ranges' starts, with no vertex captured yet. */
	state->feedback->active = 1;
	state->feedback->paused = 0;
	state->feedback->primitive_mode = primitiveMode;
	state->feedback->program = program;
	memset(state->feedback->written, 0, sizeof(state->feedback->written));
	memset(state->feedback->vertices, 0, sizeof(state->feedback->vertices));
}

/*
 * Ends capturing.
 */
GL_APICALL void GL_APIENTRY
glEndTransformFeedback(void)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state, capturing. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (!state->feedback->active) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Not active; the vertices captured are the ones glDrawTransformFeedback draws. */
	state->feedback->active = 0;
	state->feedback->paused = 0;
	state->feedback->program = NULL;
	memcpy(state->feedback->ended_vertices, state->feedback->vertices, sizeof(state->feedback->ended_vertices));
	state->feedback->ended = 1;
}

/*
 * Pauses capturing.
 */
GL_APICALL void GL_APIENTRY
glPauseTransformFeedback(void)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state, capturing and not paused. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (!state->feedback->active || state->feedback->paused) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Paused. */
	state->feedback->paused = 1;
}

/*
 * Resumes paused capturing.
 */
GL_APICALL void GL_APIENTRY
glResumeTransformFeedback(void)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state, capturing and paused. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (!state->feedback->active || !state->feedback->paused) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Capturing again. */
	state->feedback->paused = 0;
}

/*
 * Draws the vertices the last capture that ended in a transform feedback
 * object wrote (desktop GL 4.0, GL_ARB_transform_feedback2, libGL).
 */
GL_APICALL void GL_APIENTRY
glDrawTransformFeedback(
	GLenum mode,
	GLuint id)
{
	/* Stream 0, once. */
	feedback_draw(mode, id, 0U, 1);
}

/*
 * Draws the vertices the last capture that ended in a transform feedback
 * object wrote into a stream (desktop GL 4.0, GL_ARB_transform_feedback3,
 * libGL).
 */
GL_APICALL void GL_APIENTRY
glDrawTransformFeedbackStream(
	GLenum mode,
	GLuint id,
	GLuint stream)
{
	/* Once. */
	feedback_draw(mode, id, stream, 1);
}

/*
 * Draws instances of the vertices the last capture that ended in a
 * transform feedback object wrote (desktop GL 4.2's
 * GL_ARB_transform_feedback_instanced, libGL).
 */
GL_APICALL void GL_APIENTRY
glDrawTransformFeedbackInstanced(
	GLenum mode,
	GLuint id,
	GLsizei instancecount)
{
	/* Stream 0. */
	feedback_draw(mode, id, 0U, instancecount);
}

/*
 * Draws instances of the vertices the last capture that ended in a
 * transform feedback object wrote into a stream (desktop GL 4.2's
 * GL_ARB_transform_feedback_instanced, libGL).
 */
GL_APICALL void GL_APIENTRY
glDrawTransformFeedbackStreamInstanced(
	GLenum mode,
	GLuint id,
	GLuint stream,
	GLsizei instancecount)
{
	/* As it is. */
	feedback_draw(mode, id, stream, instancecount);
}

/*
 * Draws instances of the vertices the last ended capture of an object
 * wrote into a stream as glDrawArraysInstanced from vertex 0.  The name
 * must be an object's and a capture must have ended in it.
 */
static void
feedback_draw(
	GLenum mode,
	GLuint id,
	GLuint stream,
	GLsizei instances)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_feedback *feedback;
	size_t vertices;

	/* A context with its state, and a stream there is. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (stream >= GLES_VERTEX_STREAMS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A transform feedback object. */
	feedback = feedback_named(state, id);
	if (feedback == NULL) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* One a capture has ended in. */
	if (!feedback->ended) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The stream's vertices, no more than a draw takes. */
	vertices = feedback->ended_vertices[stream];
	if (vertices > 0x7fffffffU)
		vertices = 0x7fffffffU;

	/* The draw (which checks the mode, the instances and the state). */
	glDrawArraysInstanced(mode, 0, (GLsizei)vertices, instances);
}

/* Returns the transform feedback object of a name (0: the default one), or NULL. */
static struct gles_feedback *
feedback_named(
	struct gles_state *state,
	GLuint name)
{
	struct gles_feedback *feedback;

	/* Name 0 is the default one. */
	if (name == 0U)
		return &state->default_feedback;

	/* The name's object. */
	feedback = gles_names_get(&state->feedbacks, name);
	if (feedback == NULL)
		return NULL;

	/* Succeeded: the object. */
	return feedback;
}

/* Returns the vertices of a primitive of a mode's kind: 1 for points, 2 for lines, 3 for triangles (0 for a name that is not a mode). */
static unsigned
feedback_vertices(
	GLenum mode)
{
	/* The modes by kind. */
	switch (mode) {
	case GL_POINTS:
		return 1U;
	case GL_LINES:
	case GL_LINE_LOOP:
	case GL_LINE_STRIP:
		return 2U;
	case GL_TRIANGLES:
	case GL_TRIANGLE_STRIP:
	case GL_TRIANGLE_FAN:
		return 3U;
	default:
		break;
	}

	/* Not a mode. */
	return 0U;
}

/* Returns the bytes left in a binding point's range after what was written (a range of size 0 runs to the buffer's end). */
static size_t
feedback_room(
	const struct gles_buffer_range *range,
	size_t written)
{
	size_t size;

	/* No buffer has no room. */
	if (range->buffer == NULL || range->offset > range->buffer->size)
		return 0U;

	/* The range's size, or the rest of the buffer. */
	size = range->size;
	if (size == 0U || range->offset + size > range->buffer->size)
		size = range->buffer->size - range->offset;
	if (written >= size)
		return 0U;

	/* Succeeded: what is left. */
	return size - written;
}

/*
 * Copies a draw's records into a binding point's buffer after what was
 * written, in GL's order of vertices, instance after instance: each
 * vertex takes the buffer's words, and each output that goes into the
 * buffer is copied to its place there (skipped components are left as
 * they are).  Returns 0, or -1 when the buffer has no device copy or
 * there is no memory.
 */
static int
feedback_copy(
	struct gles_state *state,
	VkCommandBuffer command,
	const struct gles_capture_target *capture,
	unsigned binding,
	GLint first,
	GLsizei instances,
	const uint32_t *list,
	uint32_t expanded)
{
	struct gles_buffer_range *range;
	struct gles_buffer *buffer;
	struct gles_program *program;
	const struct gles_capture *output;
	VkBufferCopy *regions;
	VkDeviceSize record;
	VkDeviceSize source;
	VkDeviceSize target;
	VkDeviceSize stride;
	VkDeviceSize size;
	uint32_t count;
	uint32_t vertex;
	uint32_t item;
	unsigned outputs;
	unsigned index;
	GLsizei instance;
	int status;

	/* The buffer's device copy, up to date with its bytes. */
	range = &state->feedback_ranges[binding];
	buffer = range->buffer;
	program = state->feedback->program;
	status = gles_buffer_sync(state, buffer);
	if (status != 0 || buffer->buffer == VK_NULL_HANDLE)
		return -1;

	/* The outputs that go into it. */
	outputs = 0U;
	for (index = 0U; index < program->capture_count; index++) {
		if (program->captures[index].buffer == binding && program->captures[index].words != 0U)
			outputs++;
	}

	/* A region per output of each vertex at most. */
	regions = malloc(((size_t)expanded * (size_t)instances * outputs + 1U) * sizeof(*regions));
	if (regions == NULL)
		return -1;

	/* Each vertex written, at its place after the ones before it. */
	count = 0U;
	stride = (VkDeviceSize)program->capture_buffer_words[binding] * 4U;
	target = range->offset + state->feedback->written[binding];
	for (instance = 0; instance < instances; instance++) {
		for (item = 0U; item < expanded; item++) {
			vertex = (uint32_t)first + item;
			if (list != NULL)
				vertex = list[item];
			record = capture->buffer.offset +
				 ((VkDeviceSize)GLES_CAPTURE_HEADER +
				  ((VkDeviceSize)instance * capture->vertices + vertex) * program->capture_stride) * 4U;

			/* Each output of the buffer: its words of the record (a region that follows the one before in both buffers joins it). */
			for (index = 0U; index < program->capture_count; index++) {
				output = &program->captures[index];
				if (output->buffer != binding || output->words == 0U)
					continue;
				source = record + (VkDeviceSize)output->offset * 4U;
				size = (VkDeviceSize)output->words * 4U;
				if (count != 0U &&
				    regions[count - 1U].srcOffset + regions[count - 1U].size == source &&
				    regions[count - 1U].dstOffset + regions[count - 1U].size == target + (VkDeviceSize)output->place * 4U) {
					regions[count - 1U].size += size;
				} else {
					regions[count].srcOffset = source;
					regions[count].dstOffset = target + (VkDeviceSize)output->place * 4U;
					regions[count].size = size;
					count++;
				}
			}

			/* The next vertex's place. */
			target += stride;
		}
	}

	/* The copies. */
	if (count != 0U)
		vkCmdCopyBuffer(command, capture->buffer.buffer, buffer->buffer, count, regions);
	free(regions);

	/*
	 * The device copy now has what the CPU's bytes lack (gles_buffer_fetch
	 * reads it back), and waits for the frame before it may go.
	 */
	buffer->gpu_written = 1;
	buffer->used = state->frame;

	/* Succeeded: the copies are recorded. */
	return 0;
}

/*
 * Returns the room for records of each instance of a draw whose geometry
 * shader captures: every vertex its invocations may emit for the draw's
 * input primitives, or with tessellation (whose primitives are not known
 * beforehand) twice the vertices the transform feedback buffers take.
 */
static uint32_t
feedback_geometry_room(
	struct gles_state *state,
	const struct gles_program *program,
	uint32_t expanded)
{
	uint64_t room;
	size_t bytes;
	unsigned binding;
	unsigned per;

	/* With tessellation: the vertices each buffer takes (each stream's fewest, at most), twice, and an invocation's more. */
	if (program->tess_control_module != VK_NULL_HANDLE) {
		room = 0U;
		for (binding = 0U; binding < program->capture_buffers; binding++) {
			if (program->capture_buffer_words[binding] == 0U)
				continue;
			bytes = feedback_room(&state->feedback_ranges[binding], state->feedback->written[binding]);
			room += bytes / ((size_t)program->capture_buffer_words[binding] * 4U);
		}
		room = room * 2U + (uint64_t)program->capture_vertices * program->capture_invocations;
		if (room > 0x7fffffffU)
			room = 0x7fffffffU;
		return (uint32_t)room;
	}

	/* The vertices of an input primitive. */
	per = 1U;
	if (program->geometry_input == GL_LINES)
		per = 2U;
	if (program->geometry_input == GL_TRIANGLES)
		per = 3U;
	if (program->geometry_input == GL_LINES_ADJACENCY)
		per = 4U;
	if (program->geometry_input == GL_TRIANGLES_ADJACENCY)
		per = 6U;

	/* Each input primitive's invocations, each emitting its most vertices. */
	room = (uint64_t)(expanded / per) * program->capture_invocations * program->capture_vertices;
	if (room > 0x7fffffffU)
		room = 0x7fffffffU;

	/* Succeeded: the room. */
	return (uint32_t)room;
}

/*
 * Brings the bytes of the buffers bound where a geometry shader's capture
 * writes up to date on the CPU and makes them writable (the frame that
 * may read them is done).  Returns 0, or -1 when there is no memory.
 */
static int
feedback_geometry_buffers(
	struct zegl_context *context,
	struct gles_state *state)
{
	struct gles_program *program;
	struct gles_buffer *buffer;
	unsigned binding;
	int status;

	/* Each binding point written. */
	program = state->feedback->program;
	for (binding = 0U; binding < program->capture_buffers; binding++) {
		if (program->capture_buffer_words[binding] == 0U)
			continue;
		buffer = state->feedback_ranges[binding].buffer;

		/* What the device wrote read back, and the bytes on the CPU when they live in a copy still in use. */
		status = gles_buffer_fetch(context, buffer);
		if (status != 0)
			return -1;
		status = gles_buffer_writable(state, buffer);
		if (status != 0)
			return -1;
	}

	/* Succeeded: the bytes may be written. */
	return 0;
}

/*
 * Writes an instance's records of a geometry shader's capture into the
 * transform feedback buffers: those there was room for, sorted into GL's
 * order by their keys, each stream's strips made primitives of the
 * capture's kind (a strip of triangles turns every other one round, as
 * GL does) one after another.
 */
static void
feedback_geometry_write(
	struct gles_state *state,
	const uint32_t *header,
	uint32_t room)
{
	struct gles_program *program;
	struct feedback_record *records;
	const uint32_t *strips[GLES_VERTEX_STREAMS][2];
	const uint32_t *primitive[3];
	const uint32_t *words;
	uint32_t strip_vertices[GLES_VERTEX_STREAMS];
	uint32_t count;
	uint32_t index;
	uint32_t key;
	unsigned kind;
	unsigned stream;
	unsigned made;

	/* The records written, no more than there was room for. */
	program = state->feedback->program;
	count = header[GLSL_CAPTURE_RECORDS];
	if (count > room)
		count = room;
	if (count == 0U)
		return;
	records = malloc((size_t)count * sizeof(*records));
	if (records == NULL)
		return;

	/* Each record with its key, then in GL's order. */
	words = header + GLSL_CAPTURE_GEOMETRY_HEADER;
	for (index = 0U; index < count; index++) {
		memcpy(records[index].key, words, sizeof(records[index].key));
		records[index].words = words + GLSL_CAPTURE_KEY;
		words += GLSL_CAPTURE_KEY + program->capture_stride;
	}
	qsort(records, count, sizeof(*records), feedback_geometry_order);

	/* Each record's vertex added to its stream's strip, a primitive written once the strip makes one. */
	kind = feedback_vertices(program->capture_primitive);
	memset(strip_vertices, 0, sizeof(strip_vertices));
	memset(strips, 0, sizeof(strips));
	for (index = 0U; index < count; index++) {
		key = records[index].key[3];
		stream = (key >> GLSL_CAPTURE_STREAM_SHIFT) & GLSL_CAPTURE_STREAM_MASK;
		if ((key & GLSL_CAPTURE_STRIP_START) != 0U)
			strip_vertices[stream] = 0U;
		strip_vertices[stream]++;

		/* The primitive the vertex ends: itself, a segment, or a triangle (an odd one of the strip turned round). */
		made = 0U;
		primitive[kind - 1U] = records[index].words;
		if (kind == 2U && strip_vertices[stream] >= 2U) {
			primitive[0] = strips[stream][1];
			made = 1U;
		} else if (kind == 3U && strip_vertices[stream] >= 3U) {
			primitive[0] = strips[stream][0];
			primitive[1] = strips[stream][1];
			if (((strip_vertices[stream] - 3U) & 1U) != 0U) {
				primitive[0] = strips[stream][1];
				primitive[1] = strips[stream][0];
			}
			made = 1U;
		} else if (kind == 1U) {
			made = 1U;
		}

		/* Written when it fits; the strip moves on. */
		if (made)
			feedback_geometry_primitive(state, stream, primitive, kind);
		strips[stream][0] = strips[stream][1];
		strips[stream][1] = records[index].words;
	}
	free(records);
}

/* Orders two records of a geometry shader's capture as GL has their vertices: by primitive, invocation, invocation's number, place. */
static int
feedback_geometry_order(
	const void *left,
	const void *right)
{
	const struct feedback_record *first;
	const struct feedback_record *second;
	uint32_t a;
	uint32_t b;
	unsigned part;

	/* The key's words in turn (the last one's place only). */
	first = left;
	second = right;
	for (part = 0U; part < GLSL_CAPTURE_KEY; part++) {
		a = first->key[part];
		b = second->key[part];
		if (part == GLSL_CAPTURE_KEY - 1U) {
			a &= GLSL_CAPTURE_SEQUENCE_MASK;
			b &= GLSL_CAPTURE_SEQUENCE_MASK;
		}
		if (a < b)
			return -1;
		if (a > b)
			return 1;
	}

	/* The same vertex. */
	return 0;
}

/*
 * Writes a primitive of a geometry shader's capture on a stream into the
 * stream's transform feedback buffers after what was written, when it
 * fits into every one of them (else it is left out, as GL leaves it):
 * each vertex takes a buffer's words, each output of the buffer copied to
 * its place.  The primitive and its vertices are counted.
 */
static void
feedback_geometry_primitive(
	struct gles_state *state,
	unsigned stream,
	const uint32_t *const *vertices,
	unsigned count)
{
	struct gles_program *program;
	struct gles_feedback *feedback;
	struct gles_buffer_range *range;
	const struct gles_capture *output;
	unsigned char *bytes;
	size_t room;
	size_t needed;
	unsigned binding;
	unsigned vertex;
	unsigned index;

	/* Room in each buffer of the stream. */
	feedback = state->feedback;
	program = feedback->program;
	for (binding = 0U; binding < program->capture_buffers; binding++) {
		if (program->capture_buffer_words[binding] == 0U || program->capture_buffer_streams[binding] != stream)
			continue;
		needed = (size_t)count * program->capture_buffer_words[binding] * 4U;
		room = feedback_room(&state->feedback_ranges[binding], feedback->written[binding]);
		if (needed > room)
			return;
	}

	/* Each buffer of the stream: each vertex after what was written, its outputs at their places. */
	for (binding = 0U; binding < program->capture_buffers; binding++) {
		if (program->capture_buffer_words[binding] == 0U || program->capture_buffer_streams[binding] != stream)
			continue;
		range = &state->feedback_ranges[binding];
		bytes = range->buffer->data + range->offset + feedback->written[binding];
		for (vertex = 0U; vertex < count; vertex++) {
			for (index = 0U; index < program->capture_count; index++) {
				output = &program->captures[index];
				if (output->buffer != binding || output->words == 0U)
					continue;
				memcpy(bytes + (size_t)output->place * 4U, vertices[vertex] + output->offset, (size_t)output->words * 4U);
			}
			bytes += (size_t)program->capture_buffer_words[binding] * 4U;
		}

		/* Written: the device copy is behind the bytes (unless they live in it). */
		feedback->written[binding] += (size_t)count * program->capture_buffer_words[binding] * 4U;
		if (!range->buffer->on_device)
			range->buffer->dirty = 1;
	}

	/* The vertices captured into the stream, and the primitive written. */
	feedback->vertices[stream] += count;
	gles_query_primitives(state, stream, 1U);
}
