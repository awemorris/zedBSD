/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Drawing in zedBSD's OpenGL ES (WS068 p008): vertex arrays, pipelines,
 * glDrawArrays and glDrawElements, glClear and glReadPixels, recorded
 * into the draw surface's frame (libEGL, vulkan.c).
 *
 * A draw reads its indices on the CPU (buffer objects keep their bytes
 * there): triangle strips and fans, line strips and loops become lists,
 * byte indices become 32-bit ones, and the largest index bounds the
 * client arrays copied into the stream.  An attribute whose format the
 * device cannot fetch is converted to floats.  Each draw gets a
 * descriptor set with a copy of the uniform block and the samplers.
 */

#include "gles.h"

#include <stdlib.h>
#include <string.h>

/* The descriptor sets and descriptors of one pool. */
#define DRAW_POOL_SETS		256U

static void draw_primitives(GLenum mode, GLint first, GLsizei count, GLenum type, const void *indices);
static int draw_indices(struct zegl_context *context, GLsizei count, GLenum type, const void *indices, uint32_t **out, uint32_t *largest);
static uint32_t *draw_expand(GLenum mode, const uint32_t *indices, uint32_t first, GLsizei count, int rotate, uint32_t *expanded);
static void draw_program(GLenum mode, GLint first, GLsizei count, GLenum type, const void *indices, int flat);
static int draw_topology(GLenum mode, uint32_t *topology, int *strip);
static int draw_vertices(struct gles_state *state, uint32_t vertices, struct gles_vertex_layout *layout, VkBuffer *buffers, VkDeviceSize *offsets);
static VkFormat draw_format(GLint size, GLenum type, GLboolean normalized, size_t *bytes);
static int draw_format_ok(struct gles_state *state, VkFormat format);
static float draw_component(const unsigned char *source, GLenum type, GLboolean normalized);
static void draw_raster(struct gles_state *state, uint32_t topology, struct gles_raster *raster);
static VkPipeline draw_pipeline(struct gles_state *state, struct zegl_surface *surface, const struct gles_raster *raster, const struct gles_vertex_layout *layout);
static VkDescriptorSet draw_descriptors(struct gles_state *state, uint32_t *offset);
static void draw_dynamic(struct gles_state *state, struct zegl_context *context, struct zegl_surface *surface);
static VkRect2D draw_scissor_rect(struct gles_state *state, struct zegl_surface *surface);
static VkBlendFactor draw_blend_factor(GLenum factor);
static VkBlendOp draw_blend_op(GLenum equation);
static VkStencilOp draw_stencil_op(GLenum op);

/*
 * Forgets the pipelines made for a program's link (they wait for the
 * frame).
 */
void
gles_pipelines_forget(
	struct gles_state *state,
	uint64_t program)
{
	struct gles_pipeline **link;
	struct gles_pipeline *pipeline;
	struct gles_garbage objects;

	/* Each pipeline of the program leaves the list. */
	link = &state->pipelines;
	while (*link != NULL) {
		pipeline = *link;
		if (pipeline->key.program != program) {
			link = &pipeline->next;
			continue;
		}

		/* It waits for the frame. */
		*link = pipeline->next;
		memset(&objects, 0, sizeof(objects));
		objects.pipeline = pipeline->pipeline;
		gles_garbage_keep(state, &objects);
		free(pipeline);
	}
}

/*
 * Reads a rectangle of the read surface as RGBA8 rows from the bottom up,
 * waiting for what the frame drew.  Pixels outside the surface are
 * black.  Returns 0, or -1 with the error recorded.
 */
int
gles_read_rgba(
	struct zegl_context *context,
	GLint x,
	GLint y,
	GLsizei width,
	GLsizei height,
	unsigned char *rows)
{
	struct zegl_surface *surface;
	struct gles_state *state;
	VkImageMemoryBarrier barrier;
	VkBufferImageCopy copy;
	VkBuffer buffer;
	VkDeviceSize offset;
	unsigned char *mapped;
	unsigned char *pixel;
	const unsigned char *source;
	GLint left;
	GLint bottom;
	GLint right;
	GLint top;
	GLint row;
	GLint column;
	int swizzle;
	EGLint error;

	/* A read surface whose images can be copied from. */
	state = gles_state(context);
	surface = context->read;
	if (state == NULL || surface == NULL || state->framebuffer != 0U) {
		gles_error(context, GL_INVALID_FRAMEBUFFER_OPERATION);
		return -1;
	}

	/* The images must be copyable. */
	if (!surface->readable) {
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}

	/* Everything starts black; only the part inside the surface is copied. */
	memset(rows, 0, (size_t)width * (size_t)height * 4U);
	left = x;
	if (left < 0)
		left = 0;
	bottom = y;
	if (bottom < 0)
		bottom = 0;
	right = x + width;
	if (right > (GLint)surface->extent.width)
		right = (GLint)surface->extent.width;
	top = y + height;
	if (top > (GLint)surface->extent.height)
		top = (GLint)surface->extent.height;
	if (left >= right || bottom >= top)
		return 0;

	/* The frame, with its image drawn by at least one pass (a first pass clears it). */
	error = zegl_frame_begin(surface);
	if (error != EGL_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* The pass ends so the image can be copied. */
	zegl_frame_pass(surface, NULL);
	zegl_frame_leave_pass(surface);

	/* Room in the stream for the copy. */
	mapped = gles_stream(state, (size_t)(right - left) * (size_t)(top - bottom) * 4U, 16U, &buffer, &offset);
	if (mapped == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* The image copied out between two layout changes (Vulkan's rows go down from the top). */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.oldLayout = surface->rest_layout;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = surface->images[surface->image];
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(surface->command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			     0U, 0U, NULL, 0U, NULL, 1U, &barrier);
	memset(&copy, 0, sizeof(copy));
	copy.bufferOffset = offset;
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1U;
	copy.imageOffset.x = left;
	copy.imageOffset.y = (int32_t)surface->extent.height - top;
	copy.imageExtent.width = (uint32_t)(right - left);
	copy.imageExtent.height = (uint32_t)(top - bottom);
	copy.imageExtent.depth = 1U;
	vkCmdCopyImageToBuffer(surface->command, surface->images[surface->image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1U, &copy);
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = 0U;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.newLayout = surface->rest_layout;
	vkCmdPipelineBarrier(surface->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			     0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* Done before the bytes are read. */
	error = zegl_frame_flush(surface);
	if (error != EGL_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* Each row into GL's order (bottom up), each pixel as RGBA. */
	swizzle = 0;
	if (surface->format == VK_FORMAT_B8G8R8A8_UNORM)
		swizzle = 1;
	for (row = bottom; row < top; row++) {
		source = mapped + (size_t)(top - 1 - row) * (size_t)(right - left) * 4U;
		for (column = left; column < right; column++) {
			pixel = rows + ((size_t)(row - y) * (size_t)width + (size_t)(column - x)) * 4U;
			pixel[0] = source[0];
			pixel[1] = source[1];
			pixel[2] = source[2];
			pixel[3] = source[3];
			if (swizzle) {
				pixel[0] = source[2];
				pixel[2] = source[0];
			}

			/* The next pixel. */
			source += 4;
		}
	}

	/* Everything recorded before is done: the frame's resources are free again. */
	state->frame++;
	gles_collect(state);

	/* Succeeded: the rows. */
	return 0;
}

/*
 * Draws primitives from the enabled arrays, vertices first to first +
 * count - 1.
 */
GL_APICALL void GL_APIENTRY
glDrawArrays(
	GLenum mode,
	GLint first,
	GLsizei count)
{
	/* No indices. */
	draw_primitives(mode, first, count, GL_NONE, NULL);
}

/*
 * Draws primitives from the enabled arrays with count indices.
 */
GL_APICALL void GL_APIENTRY
glDrawElements(
	GLenum mode,
	GLsizei count,
	GLenum type,
	const void *indices)
{
	/* The indices of the type. */
	if (type == GL_NONE)
		type = GL_INVALID_ENUM;
	draw_primitives(mode, 0, count, type, indices);
}

/*
 * Clears the buffers of the frame in the mask (inside the scissor box
 * when the scissor test is on).
 */
GL_APICALL void GL_APIENTRY
glClear(
	GLbitfield mask)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct zegl_surface *surface;
	VkClearAttachment attachments[2];
	VkClearValue values[2];
	VkClearRect rect;
	uint32_t count;
	EGLint error;
	int whole;

	/* A context with its state, and a mask of the three buffers. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if ((mask & ~(GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) != 0U) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A window surface to clear. */
	surface = context->draw;
	if (surface == NULL || state->framebuffer != 0U) {
		gles_error(context, GL_INVALID_FRAMEBUFFER_OPERATION);
		return;
	}

	/* The frame, opened when this is its first command. */
	error = zegl_frame_begin(surface);
	if (error != EGL_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/* The values. */
	memset(values, 0, sizeof(values));
	memcpy(values[0].color.float32, context->gles.clear_color, 4U * sizeof(float));
	values[1].depthStencil.depth = state->clear_depth;
	values[1].depthStencil.stencil = (uint32_t)state->clear_stencil & 0xffU;

	/* A whole clear at the frame's start is the first pass's own. */
	whole = 0;
	if (surface->passes == 0U && !state->scissor_test && state->color_mask[0] && state->color_mask[1] &&
	    state->color_mask[2] && state->color_mask[3])
		whole = 1;
	if (whole) {
		zegl_frame_pass(surface, values);
		return;
	}

	/* Otherwise the attachments in the mask, inside the pass. */
	count = 0U;
	memset(attachments, 0, sizeof(attachments));
	if ((mask & GL_COLOR_BUFFER_BIT) != 0U) {
		attachments[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		attachments[count].colorAttachment = 0U;
		attachments[count].clearValue = values[0];
		count++;
	}

	/* The depth and stencil aspects in the mask, when the surface has a depth buffer. */
	if (surface->depth_format != VK_FORMAT_UNDEFINED && (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) != 0U) {
		if ((mask & GL_DEPTH_BUFFER_BIT) != 0U && state->depth_mask)
			attachments[count].aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
		if ((mask & GL_STENCIL_BUFFER_BIT) != 0U)
			attachments[count].aspectMask |= surface->depth_aspects & VK_IMAGE_ASPECT_STENCIL_BIT;
		attachments[count].clearValue = values[1];
		if (attachments[count].aspectMask != 0U)
			count++;
	}

	/* Nothing in the mask the surface has. */
	if (count == 0U)
		return;

	/* The clear over the scissor box or the whole surface. */
	zegl_frame_pass(surface, NULL);
	memset(&rect, 0, sizeof(rect));
	rect.rect = draw_scissor_rect(state, surface);
	rect.layerCount = 1U;
	if (rect.rect.extent.width == 0U || rect.rect.extent.height == 0U)
		return;
	vkCmdClearAttachments(surface->command, count, attachments, 1U, &rect);
}

/*
 * Reads a rectangle of the framebuffer as RGBA bytes.
 */
GL_APICALL void GL_APIENTRY
glReadPixels(
	GLint x,
	GLint y,
	GLsizei width,
	GLsizei height,
	GLenum format,
	GLenum type,
	void *pixels)
{
	struct zegl_context *context;
	struct gles_state *state;
	unsigned char *rows;
	size_t stride;
	size_t alignment;
	GLsizei row;
	int status;

	/* A context with its state, RGBA bytes, and a size. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (format != GL_RGBA || type != GL_UNSIGNED_BYTE) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A size that is not negative. */
	if (width < 0 || height < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Nothing to read. */
	if (width == 0 || height == 0)
		return;

	/* The pixels, tightly packed. */
	rows = malloc((size_t)width * (size_t)height * 4U);
	if (rows == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/* The rectangle. */
	status = gles_read_rgba(context, x, y, width, height, rows);
	if (status != 0) {
		free(rows);
		return;
	}

	/* Each row at the pack alignment. */
	alignment = (size_t)state->pack_alignment;
	stride = ((size_t)width * 4U + alignment - 1U) / alignment * alignment;
	for (row = 0; row < height; row++)
		memcpy((unsigned char *)pixels + (size_t)row * stride, rows + (size_t)row * (size_t)width * 4U, (size_t)width * 4U);
	free(rows);
}

/*
 * Describes a vertex attribute's array.
 */
GL_APICALL void GL_APIENTRY
glVertexAttribPointer(
	GLuint index,
	GLint size,
	GLenum type,
	GLboolean normalized,
	GLsizei stride,
	const void *pointer)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_attrib *attrib;

	/* A context with its state, an attribute, a size and a stride. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (index >= GLES_ATTRIBS || size < 1 || size > 4 || stride < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A type OpenGL ES 2 has. */
	switch (type) {
	case GL_BYTE:
	case GL_UNSIGNED_BYTE:
	case GL_SHORT:
	case GL_UNSIGNED_SHORT:
	case GL_FIXED:
	case GL_FLOAT:
		break;
	default:
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The array, in the bound array buffer or the application's memory. */
	attrib = &state->attribs[index];
	attrib->size = size;
	attrib->type = type;
	attrib->normalized = normalized;
	attrib->stride = stride;
	attrib->pointer = pointer;
	attrib->buffer = state->array_buffer;
}

/*
 * Makes an attribute read its array.
 */
GL_APICALL void GL_APIENTRY
glEnableVertexAttribArray(
	GLuint index)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state and an attribute. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (index >= GLES_ATTRIBS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Enabled. */
	state->attribs[index].enabled = 1;
}

/*
 * Makes an attribute take its current value instead of its array.
 */
GL_APICALL void GL_APIENTRY
glDisableVertexAttribArray(
	GLuint index)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state and an attribute. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (index >= GLES_ATTRIBS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Disabled. */
	state->attribs[index].enabled = 0;
}

/*
 * Sets an attribute's current value from four floats (the rest of the
 * glVertexAttrib calls come here).
 */
GL_APICALL void GL_APIENTRY
glVertexAttrib4f(
	GLuint index,
	GLfloat x,
	GLfloat y,
	GLfloat z,
	GLfloat w)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state and an attribute. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (index >= GLES_ATTRIBS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The value. */
	state->attribs[index].value[0] = x;
	state->attribs[index].value[1] = y;
	state->attribs[index].value[2] = z;
	state->attribs[index].value[3] = w;
}

GL_APICALL void GL_APIENTRY
glVertexAttrib1f(
	GLuint index,
	GLfloat x)
{
	/* y, z and w are 0, 0 and 1. */
	glVertexAttrib4f(index, x, 0.0f, 0.0f, 1.0f);
}

GL_APICALL void GL_APIENTRY
glVertexAttrib2f(
	GLuint index,
	GLfloat x,
	GLfloat y)
{
	/* z and w are 0 and 1. */
	glVertexAttrib4f(index, x, y, 0.0f, 1.0f);
}

GL_APICALL void GL_APIENTRY
glVertexAttrib3f(
	GLuint index,
	GLfloat x,
	GLfloat y,
	GLfloat z)
{
	/* w is 1. */
	glVertexAttrib4f(index, x, y, z, 1.0f);
}

GL_APICALL void GL_APIENTRY
glVertexAttrib1fv(
	GLuint index,
	const GLfloat *v)
{
	/* One value. */
	glVertexAttrib4f(index, v[0], 0.0f, 0.0f, 1.0f);
}

GL_APICALL void GL_APIENTRY
glVertexAttrib2fv(
	GLuint index,
	const GLfloat *v)
{
	/* Two values. */
	glVertexAttrib4f(index, v[0], v[1], 0.0f, 1.0f);
}

GL_APICALL void GL_APIENTRY
glVertexAttrib3fv(
	GLuint index,
	const GLfloat *v)
{
	/* Three values. */
	glVertexAttrib4f(index, v[0], v[1], v[2], 1.0f);
}

GL_APICALL void GL_APIENTRY
glVertexAttrib4fv(
	GLuint index,
	const GLfloat *v)
{
	/* Four values. */
	glVertexAttrib4f(index, v[0], v[1], v[2], v[3]);
}

/*
 * Reports an attribute's array state or current value as floats.
 */
GL_APICALL void GL_APIENTRY
glGetVertexAttribfv(
	GLuint index,
	GLenum pname,
	GLfloat *params)
{
	GLint value;
	struct zegl_context *context;
	struct gles_state *state;

	/* The current value is floats already. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (index >= GLES_ATTRIBS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The rest as an integer. */
	if (pname == GL_CURRENT_VERTEX_ATTRIB) {
		memcpy(params, state->attribs[index].value, 4U * sizeof(float));
		return;
	}

	/* The rest as an integer. */
	value = 0;
	glGetVertexAttribiv(index, pname, &value);
	params[0] = (GLfloat)value;
}

/*
 * Reports an attribute's array state or current value as integers.
 */
GL_APICALL void GL_APIENTRY
glGetVertexAttribiv(
	GLuint index,
	GLenum pname,
	GLint *params)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_attrib *attrib;
	unsigned component;

	/* A context with its state and an attribute. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (index >= GLES_ATTRIBS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The attribute. */
	attrib = &state->attribs[index];

	/* The state asked for. */
	switch (pname) {
	case GL_VERTEX_ATTRIB_ARRAY_ENABLED:
		*params = attrib->enabled;
		return;
	case GL_VERTEX_ATTRIB_ARRAY_SIZE:
		*params = attrib->size;
		return;
	case GL_VERTEX_ATTRIB_ARRAY_STRIDE:
		*params = attrib->stride;
		return;
	case GL_VERTEX_ATTRIB_ARRAY_TYPE:
		*params = (GLint)attrib->type;
		return;
	case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:
		*params = attrib->normalized;
		return;
	case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:
		*params = 0;
		if (attrib->buffer != NULL)
			*params = (GLint)attrib->buffer->name;
		return;
	case GL_CURRENT_VERTEX_ATTRIB:
		for (component = 0U; component < 4U; component++)
			params[component] = (GLint)attrib->value[component];
		return;
	default:
		break;
	}

	/* Any other is an error. */
	gles_error(context, GL_INVALID_ENUM);
}

/*
 * Reports an attribute's array pointer.
 */
GL_APICALL void GL_APIENTRY
glGetVertexAttribPointerv(
	GLuint index,
	GLenum pname,
	void **pointer)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state, an attribute and the one name. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (index >= GLES_ATTRIBS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Only the pointer. */
	if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The pointer (an offset for a buffer object). */
	*pointer = (void *)state->attribs[index].pointer;
}

/*
 * Draws with the current program, or, without one, with the
 * fixed-function layer's program for this draw (libGL).
 */
static void
draw_primitives(
	GLenum mode,
	GLint first,
	GLsizei count,
	GLenum type,
	const void *indices)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_program *program;
	int flat;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;

	/* A program the application made draws as it is (OpenGL ES has nothing else). */
	if (state->program != NULL || gles_fixed == NULL) {
		draw_program(mode, first, count, type, indices, 0);
		return;
	}

	/* Without one, the fixed-function layer's, current for this draw only. */
	flat = 0;
	program = gles_fixed->program(context, &flat);
	if (program == NULL) {
		state->program = NULL;
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Current for this draw only. */
	state->program = program;
	draw_program(mode, first, count, type, indices, flat);
	state->program = NULL;
}

/*
 * Draws with the current program: the indices read and turned into a
 * list when needed (always, and each primitive turned to start at GL's
 * provoking vertex, when the shading is flat), the vertices and uniforms
 * put where the GPU reads them, the pipeline and descriptors bound, and
 * the draw recorded into the frame.
 */
static void
draw_program(
	GLenum mode,
	GLint first,
	GLsizei count,
	GLenum type,
	const void *indices,
	int flat)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct zegl_surface *surface;
	struct gles_raster raster;
	struct gles_vertex_layout layout;
	VkBuffer buffers[GLES_ATTRIBS];
	VkDeviceSize offsets[GLES_ATTRIBS];
	VkBuffer index_buffer;
	VkDeviceSize index_offset;
	VkPipeline pipeline;
	VkDescriptorSet set;
	uint32_t dynamic_offset;
	uint32_t dynamic_count;
	uint32_t *read;
	uint32_t *list;
	uint32_t *stream;
	uint32_t largest;
	uint32_t expanded;
	uint32_t topology;
	EGLint error;
	int strip;
	int status;

	/* A context with its state, a linked program and a window surface. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (first < 0 || count < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A mode. */
	status = draw_topology(mode, &topology, &strip);
	if (status != 0) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* A linked program. */
	if (state->program == NULL || !state->program->linked) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A window surface. */
	surface = context->draw;
	if (surface == NULL || state->framebuffer != 0U) {
		gles_error(context, GL_INVALID_FRAMEBUFFER_OPERATION);
		return;
	}

	/* Nothing to draw. */
	if (count == 0)
		return;

	/* The indices: read for glDrawElements, the range for glDrawArrays. */
	read = NULL;
	largest = (uint32_t)first + (uint32_t)count - 1U;
	if (type != GL_NONE) {
		status = draw_indices(context, count, type, indices, &read, &largest);
		if (status != 0)
			return;
	}

	/* A strip, loop or fan (or any mode, shaded flat) becomes a list; indices that were read become 32-bit ones. */
	list = read;
	expanded = (uint32_t)count;
	if (strip || flat) {
		list = draw_expand(mode, read, (uint32_t)first, count, flat, &expanded);
		free(read);
		read = NULL;
		if (list == NULL) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}
	}

	/* A list too short for one primitive draws nothing. */
	if (expanded == 0U) {
		free(list);
		return;
	}

	/* The indices in the stream. */
	index_buffer = VK_NULL_HANDLE;
	index_offset = 0U;
	if (list != NULL) {
		stream = gles_stream(state, expanded * sizeof(uint32_t), 4U, &index_buffer, &index_offset);
		if (stream == NULL) {
			free(list);
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* Copied. */
		memcpy(stream, list, expanded * sizeof(uint32_t));
		free(list);
	}

	/* The vertices each attribute reads. */
	status = draw_vertices(state, largest + 1U, &layout, buffers, offsets);
	if (status != 0) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/* The frame, open and in a pass. */
	error = zegl_frame_begin(surface);
	if (error != EGL_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/* Its first pass, or the one open. */
	zegl_frame_pass(surface, NULL);

	/* The pipeline for the state, and the descriptors. */
	draw_raster(state, topology, &raster);
	pipeline = draw_pipeline(state, surface, &raster, &layout);
	if (pipeline == VK_NULL_HANDLE) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/* The descriptors. */
	set = draw_descriptors(state, &dynamic_offset);
	if (set == VK_NULL_HANDLE) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/* The recording: pipeline, dynamic state, descriptors, vertices, indices, the draw. */
	vkCmdBindPipeline(surface->command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	draw_dynamic(state, context, surface);
	dynamic_count = 0U;
	if (state->program->uniform_data != NULL)
		dynamic_count = 1U;
	vkCmdBindDescriptorSets(surface->command, VK_PIPELINE_BIND_POINT_GRAPHICS, state->program->layout, 0U, 1U, &set,
				dynamic_count, &dynamic_offset);
	if (layout.count != 0U)
		vkCmdBindVertexBuffers(surface->command, 0U, layout.count, buffers, offsets);
	if (index_buffer != VK_NULL_HANDLE) {
		vkCmdBindIndexBuffer(surface->command, index_buffer, index_offset, VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed(surface->command, expanded, 1U, 0U, 0, 0U);
	} else {
		vkCmdDraw(surface->command, (uint32_t)count, 1U, (uint32_t)first, 0U);
	}
}

/*
 * Reads glDrawElements' indices (from the element buffer, or the
 * application's memory) as 32-bit ones, and the largest.  Returns 0, or
 * -1 with the error recorded.
 */
static int
draw_indices(
	struct zegl_context *context,
	GLsizei count,
	GLenum type,
	const void *indices,
	uint32_t **out,
	uint32_t *largest)
{
	struct gles_state *state;
	const unsigned char *bytes;
	uint32_t *read;
	uint16_t half;
	size_t size;
	size_t offset;
	GLsizei index;

	/* The index size. */
	state = gles_state(context);
	size = 0U;
	if (type == GL_UNSIGNED_BYTE)
		size = 1U;
	if (type == GL_UNSIGNED_SHORT)
		size = 2U;
	if (type == GL_UNSIGNED_INT)
		size = 4U;
	if (size == 0U) {
		gles_error(context, GL_INVALID_ENUM);
		return -1;
	}

	/* Where they are: an offset into the element buffer, or a pointer. */
	bytes = indices;
	if (state->element_buffer != NULL) {
		offset = (size_t)(uintptr_t)indices;
		if (offset + (size_t)count * size > state->element_buffer->size) {
			gles_error(context, GL_INVALID_OPERATION);
			return -1;
		}

		/* The bytes at the offset. */
		bytes = state->element_buffer->data + offset;
	}

	/* Somewhere to read them from. */
	if (bytes == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}

	/* Each one, as 32 bits. */
	read = malloc((size_t)count * sizeof(uint32_t));
	if (read == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* The largest seen so far. */
	*largest = 0U;
	for (index = 0; index < count; index++) {
		if (size == 1U) {
			read[index] = bytes[index];
		} else if (size == 2U) {
			memcpy(&half, bytes + (size_t)index * 2U, 2U);
			read[index] = half;
		} else {
			memcpy(&read[index], bytes + (size_t)index * 4U, 4U);
		}

		/* The largest. */
		if (read[index] > *largest)
			*largest = read[index];
	}

	/* Succeeded: the indices. */
	*out = read;
	return 0;
}

/*
 * Turns count vertices of a mode (indices, or first up when there are
 * none) into a list of triangles, lines or points.  With rotate, each
 * primitive starts with GL's provoking vertex (the last of a primitive,
 * the first of a polygon), which Vulkan's flat shading takes from the
 * first; the winding is kept.  Returns the list and its length, or NULL
 * when there is no memory.
 */
static uint32_t *
draw_expand(
	GLenum mode,
	const uint32_t *indices,
	uint32_t first,
	GLsizei count,
	int rotate,
	uint32_t *expanded)
{
	uint32_t *list;
	uint32_t n;
	uint32_t total;
	uint32_t index;
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t d;

	/* At most six indices per vertex (quads). */
	n = (uint32_t)count;
	list = malloc(((size_t)n * 6U + 6U) * sizeof(uint32_t));
	if (list == NULL)
		return NULL;

	/* Each primitive of the mode, as vertex numbers from 0. */
	total = 0U;
	for (index = 0U; index < n; index++) {
		switch (mode) {
		case GL_POINTS:
			list[total++] = index;
			break;
		case GL_LINES:
		case GL_LINE_STRIP:
		case GL_LINE_LOOP:
			/* A segment: every pair of a list, every vertex and the next of a strip, and a loop's closing one. */
			a = index;
			b = index + 1U;
			if (mode == GL_LINES && (index & 1U) != 0U)
				break;
			if (b == n && mode == GL_LINE_LOOP && n > 1U)
				b = 0U;
			if (b >= n || (b == 0U && mode != GL_LINE_LOOP))
				break;
			list[total++] = a;
			list[total++] = b;
			if (rotate) {
				list[total - 2U] = b;
				list[total - 1U] = a;
			}

			/* The segment is in. */
			break;
		case GL_TRIANGLES:
		case GL_TRIANGLE_STRIP:
		case GL_TRIANGLE_FAN:
		case GL_POLYGON:
			/* A triangle: every three of a list, each vertex of a strip (every other one turned round), a fan's or polygon's. */
			if (index + 2U >= n || (mode == GL_TRIANGLES && index % 3U != 0U))
				break;
			a = index;
			b = index + 1U;
			c = index + 2U;
			if (mode == GL_TRIANGLE_STRIP && (index & 1U) != 0U) {
				a = index + 1U;
				b = index;
			}

			/* A fan or polygon turns about the first vertex. */
			if (mode == GL_TRIANGLE_FAN || mode == GL_POLYGON)
				a = 0U;

			/* GL's provoking vertex is the third of a triangle (the first of a polygon's): it goes first, cyclically. */
			if (rotate && mode != GL_POLYGON) {
				d = c;
				c = b;
				b = a;
				a = d;
			}

			/* The triangle. */
			list[total++] = a;
			list[total++] = b;
			list[total++] = c;
			break;
		default:
			/* Quads and quad strips: two triangles each, both starting at the provoking (last) vertex. */
			if (mode == GL_QUADS && (index % 4U != 0U || index + 3U >= n))
				break;
			if (mode == GL_QUAD_STRIP && ((index & 1U) != 0U || index + 3U >= n))
				break;
			a = index;
			b = index + 1U;
			c = index + 2U;
			d = index + 3U;
			if (mode == GL_QUAD_STRIP) {
				c = index + 3U;
				d = index + 2U;
			}

			/* The quad a b c d, from its provoking vertex: d a b and d b c (a quad strip's provoking vertex is c). */
			if (mode == GL_QUAD_STRIP) {
				list[total++] = c;
				list[total++] = d;
				list[total++] = a;
				list[total++] = c;
				list[total++] = a;
				list[total++] = b;
				break;
			}

			/* The quad from its provoking vertex. */
			list[total++] = d;
			list[total++] = a;
			list[total++] = b;
			list[total++] = d;
			list[total++] = b;
			list[total++] = c;
			break;
		}
	}

	/* The vertex numbers become vertices: the indices given, or first up. */
	for (index = 0U; index < total; index++) {
		if (indices != NULL)
			list[index] = indices[list[index]];
		else
			list[index] += first;
	}

	/* Succeeded: the list. */
	*expanded = total;
	return list;
}

/* Returns Vulkan's topology for a GL mode, and whether the mode must become a list first; nonzero for a mode that is not one. */
static int
draw_topology(
	GLenum mode,
	uint32_t *topology,
	int *strip)
{
	/* Lists are drawn as they are; strips, loops and fans are turned into lists. */
	*strip = 0;
	switch (mode) {
	case GL_POINTS:
		*topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
		return 0;
	case GL_LINES:
		*topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		return 0;
	case GL_LINE_STRIP:
	case GL_LINE_LOOP:
		*topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		*strip = 1;
		return 0;
	case GL_TRIANGLES:
		*topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		return 0;
	case GL_TRIANGLE_STRIP:
	case GL_TRIANGLE_FAN:
		*topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		*strip = 1;
		return 0;
	default:
		break;
	}

	/* Desktop GL's quads, quad strips and polygons, with the fixed-function layer. */
	if (gles_fixed != NULL && (mode == GL_QUADS || mode == GL_QUAD_STRIP || mode == GL_POLYGON)) {
		*topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		*strip = 1;
		return 0;
	}

	/* Not a mode. */
	return -1;
}

/*
 * Puts each active attribute's vertices (0 to vertices - 1) where the GPU
 * reads them, and describes the layout: a buffer object's device copy as
 * it is, client arrays and unfetchable formats copied into the stream,
 * and a disabled array's current value as one vertex with stride 0.
 */
static int
draw_vertices(
	struct gles_state *state,
	uint32_t vertices,
	struct gles_vertex_layout *layout,
	VkBuffer *buffers,
	VkDeviceSize *offsets)
{
	struct gles_program *program;
	struct gles_attrib *attrib;
	const unsigned char *source;
	unsigned char *target;
	float *converted;
	VkFormat format;
	size_t bytes;
	size_t stride;
	size_t needed;
	uint32_t vertex;
	uint32_t index;
	GLint component;
	int fetchable;
	int status;

	/* One binding per active attribute. */
	program = state->program;
	memset(layout, 0, sizeof(*layout));
	for (index = 0U; index < program->attribute_count; index++) {
		attrib = &state->attribs[program->attributes[index].location];
		layout->locations[index] = program->attributes[index].location;

		/* A disabled array: the current value, the same for every vertex. */
		if (!attrib->enabled) {
			target = gles_stream(state, 4U * sizeof(float), 16U, &buffers[index], &offsets[index]);
			if (target == NULL)
				return -1;
			memcpy(target, attrib->value, 4U * sizeof(float));
			layout->formats[index] = VK_FORMAT_R32G32B32A32_SFLOAT;
			layout->strides[index] = 0U;
			continue;
		}

		/* The array's format and stride. */
		format = draw_format(attrib->size, attrib->type, attrib->normalized, &bytes);
		fetchable = draw_format_ok(state, format);
		stride = (size_t)attrib->stride;
		if (stride == 0U)
			stride = bytes;
		needed = (size_t)(vertices - 1U) * stride + bytes;

		/* A buffer object the device can fetch from as it is. */
		if (attrib->buffer != NULL) {
			if ((size_t)(uintptr_t)attrib->pointer + needed > attrib->buffer->size)
				return -1;
			if (fetchable) {
				status = gles_buffer_sync(state, attrib->buffer);
				if (status != 0)
					return -1;
				attrib->buffer->used = state->frame;
				buffers[index] = attrib->buffer->buffer;
				offsets[index] = (VkDeviceSize)(uintptr_t)attrib->pointer;
				layout->formats[index] = (uint32_t)format;
				layout->strides[index] = (uint32_t)stride;
				continue;
			}
		}

		/* The bytes: in the buffer object, or the application's memory. */
		source = attrib->pointer;
		if (attrib->buffer != NULL)
			source = attrib->buffer->data + (size_t)(uintptr_t)attrib->pointer;
		if (source == NULL)
			return -1;

		/* Fetchable: copied as they are. */
		if (fetchable) {
			target = gles_stream(state, needed, 16U, &buffers[index], &offsets[index]);
			if (target == NULL)
				return -1;
			memcpy(target, source, needed);
			layout->formats[index] = (uint32_t)format;
			layout->strides[index] = (uint32_t)stride;
			continue;
		}

		/* Not fetchable (fixed point, three bytes, ...): converted to floats. */
		converted = gles_stream(state, (size_t)vertices * (size_t)attrib->size * sizeof(float), 16U, &buffers[index], &offsets[index]);
		if (converted == NULL)
			return -1;
		for (vertex = 0U; vertex < vertices; vertex++) {
			for (component = 0; component < attrib->size; component++) {
				converted[(size_t)vertex * (size_t)attrib->size + (size_t)component] =
					draw_component(source + (size_t)vertex * stride + (size_t)component * (bytes / (size_t)attrib->size),
						       attrib->type, attrib->normalized);
			}
		}

		/* The floats' format. */
		layout->formats[index] = (uint32_t)draw_format(attrib->size, GL_FLOAT, GL_FALSE, &bytes);
		layout->strides[index] = (uint32_t)((size_t)attrib->size * sizeof(float));
	}

	/* Succeeded: every attribute has its vertices. */
	layout->count = program->attribute_count;
	return 0;
}

/* Returns the Vulkan format of an array's components and the bytes of one vertex's; VK_FORMAT_UNDEFINED for fixed point. */
static VkFormat
draw_format(
	GLint size,
	GLenum type,
	GLboolean normalized,
	size_t *bytes)
{
	static const VkFormat floats[4] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
	static const VkFormat ubyte_norm[4] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
	static const VkFormat ubyte[4] = { VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED };
	static const VkFormat byte_norm[4] = { VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM };
	static const VkFormat byte[4] = { VK_FORMAT_R8_SSCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8A8_SSCALED };
	static const VkFormat ushort_norm[4] = { VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM };
	static const VkFormat ushort[4] = { VK_FORMAT_R16_USCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16A16_USCALED };
	static const VkFormat short_norm[4] = { VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM };
	static const VkFormat shorts[4] = { VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED };

	/* The type's table. */
	switch (type) {
	case GL_FLOAT:
		*bytes = (size_t)size * 4U;
		return floats[size - 1];
	case GL_UNSIGNED_BYTE:
		*bytes = (size_t)size;
		if (normalized)
			return ubyte_norm[size - 1];
		return ubyte[size - 1];
	case GL_BYTE:
		*bytes = (size_t)size;
		if (normalized)
			return byte_norm[size - 1];
		return byte[size - 1];
	case GL_UNSIGNED_SHORT:
		*bytes = (size_t)size * 2U;
		if (normalized)
			return ushort_norm[size - 1];
		return ushort[size - 1];
	case GL_SHORT:
		*bytes = (size_t)size * 2U;
		if (normalized)
			return short_norm[size - 1];
		return shorts[size - 1];
	default:
		break;
	}

	/* GL_FIXED: four bytes a component, no Vulkan format. */
	*bytes = (size_t)size * 4U;
	return VK_FORMAT_UNDEFINED;
}

/* Reports whether the device fetches vertices of a format, asking the device once per format. */
static int
draw_format_ok(
	struct gles_state *state,
	VkFormat format)
{
	VkFormatProperties properties;
	unsigned char answer;

	/* Fixed point has no format. */
	if (format == VK_FORMAT_UNDEFINED || (unsigned)format >= GLES_FORMATS)
		return 0;

	/* The device's buffer features for it, the first time. */
	answer = state->vertex_formats[format];
	if (answer == 0U) {
		vkGetPhysicalDeviceFormatProperties(state->display->physical, format, &properties);
		answer = 2U;
		if ((properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0U)
			answer = 1U;
		state->vertex_formats[format] = answer;
	}

	/* The answer. */
	if (answer != 1U)
		return 0;
	return 1;
}

/* Converts one component of an array to a float, as GL reads it. */
static float
draw_component(
	const unsigned char *source,
	GLenum type,
	GLboolean normalized)
{
	int8_t signed_byte;
	uint16_t unsigned_short;
	int16_t signed_short;
	int32_t fixed;
	float value;

	/* The component's type. */
	switch (type) {
	case GL_UNSIGNED_BYTE:
		value = (float)source[0];
		if (normalized)
			value /= 255.0f;
		return value;
	case GL_BYTE:
		memcpy(&signed_byte, source, 1U);
		value = (float)signed_byte;
		if (normalized)
			value = (value * 2.0f + 1.0f) / 255.0f;
		return value;
	case GL_UNSIGNED_SHORT:
		memcpy(&unsigned_short, source, 2U);
		value = (float)unsigned_short;
		if (normalized)
			value /= 65535.0f;
		return value;
	case GL_SHORT:
		memcpy(&signed_short, source, 2U);
		value = (float)signed_short;
		if (normalized)
			value = (value * 2.0f + 1.0f) / 65535.0f;
		return value;
	case GL_FIXED:
		memcpy(&fixed, source, 4U);
		return (float)fixed / 65536.0f;
	default:
		break;
	}

	/* A float. */
	memcpy(&value, source, 4U);
	return value;
}

/* Fills the fixed-function part of a pipeline's key from the state. */
static void
draw_raster(
	struct gles_state *state,
	uint32_t topology,
	struct gles_raster *raster)
{
	unsigned face;

	/* Everything not set is 0, so equal states compare equal. */
	memset(raster, 0, sizeof(*raster));
	raster->topology = topology;

	/* Blending. */
	raster->blend = (uint32_t)state->blend;
	if (state->blend) {
		raster->blend_src_rgb = state->blend_src_rgb;
		raster->blend_dst_rgb = state->blend_dst_rgb;
		raster->blend_src_alpha = state->blend_src_alpha;
		raster->blend_dst_alpha = state->blend_dst_alpha;
		raster->blend_equation_rgb = state->blend_equation_rgb;
		raster->blend_equation_alpha = state->blend_equation_alpha;
	}

	/* The colour mask as four bits. */
	for (face = 0U; face < 4U; face++) {
		if (state->color_mask[face])
			raster->color_mask |= 1U << face;
	}

	/* Depth. */
	raster->depth_test = (uint32_t)state->depth_test;
	if (state->depth_test) {
		raster->depth_func = state->depth_func;
		raster->depth_write = state->depth_mask;
	}

	/* Stencil, both faces. */
	raster->stencil_test = (uint32_t)state->stencil_test;
	for (face = 0U; state->stencil_test && face < 2U; face++) {
		raster->stencil_func[face] = state->stencil_func[face];
		raster->stencil_fail[face] = state->stencil_fail[face];
		raster->stencil_zfail[face] = state->stencil_zfail[face];
		raster->stencil_zpass[face] = state->stencil_zpass[face];
	}

	/* Culling (triangles only). */
	raster->front_face = state->front_face;
	if (state->cull && topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) {
		raster->cull = 1U;
		raster->cull_mode = state->cull_mode;
	}

	/* Polygon offset. */
	raster->polygon_offset = (uint32_t)state->polygon_offset;
}

/* Returns the pipeline for the program, the surface's pass, the state and the vertex layout, making it the first time. */
static VkPipeline
draw_pipeline(
	struct gles_state *state,
	struct zegl_surface *surface,
	const struct gles_raster *raster,
	const struct gles_vertex_layout *layout)
{
	static const VkDynamicState dynamic_states[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
		VK_DYNAMIC_STATE_LINE_WIDTH,
		VK_DYNAMIC_STATE_DEPTH_BIAS,
		VK_DYNAMIC_STATE_BLEND_CONSTANTS,
		VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
		VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
		VK_DYNAMIC_STATE_STENCIL_REFERENCE
	};
	struct gles_pipeline_key key;
	struct gles_pipeline *entry;
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription bindings[GLES_ATTRIBS];
	VkVertexInputAttributeDescription attributes[GLES_ATTRIBS];
	VkPipelineVertexInputStateCreateInfo vertex;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo rasterization;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo depth;
	VkStencilOpState *faces[2];
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineDynamicStateCreateInfo dynamic;
	VkGraphicsPipelineCreateInfo create;
	unsigned index;
	unsigned face;
	int differs;
	VkResult result;

	/* The key. */
	memset(&key, 0, sizeof(key));
	key.program = state->program->serial;
	key.pass = surface->pass;
	key.raster = *raster;
	key.vertex = *layout;

	/* One made before for the same key. */
	for (entry = state->pipelines; entry != NULL; entry = entry->next) {
		differs = memcmp(&entry->key, &key, sizeof(key));
		if (differs == 0)
			return entry->pipeline;
	}

	/* The two stages. */
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = state->program->vertex_module;
	stages[0].pName = "main";
	stages[1] = stages[0];
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = state->program->fragment_module;

	/* One binding per attribute, the attribute at offset 0 of it. */
	memset(bindings, 0, sizeof(bindings));
	memset(attributes, 0, sizeof(attributes));
	for (index = 0U; index < layout->count; index++) {
		bindings[index].binding = index;
		bindings[index].stride = layout->strides[index];
		bindings[index].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		attributes[index].location = layout->locations[index];
		attributes[index].binding = index;
		attributes[index].format = (VkFormat)layout->formats[index];
	}

	/* The vertex input. */
	memset(&vertex, 0, sizeof(vertex));
	vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex.vertexBindingDescriptionCount = layout->count;
	vertex.pVertexBindingDescriptions = bindings;
	vertex.vertexAttributeDescriptionCount = layout->count;
	vertex.pVertexAttributeDescriptions = attributes;

	/* The topology, and one dynamic viewport and scissor. */
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = (VkPrimitiveTopology)raster->topology;
	memset(&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1U;
	viewport.scissorCount = 1U;

	/* Rasterization: culling and the front face (the rewritten gl_Position keeps GL's winding), polygon offset. */
	memset(&rasterization, 0, sizeof(rasterization));
	rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	if (raster->cull) {
		rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
		if (raster->cull_mode == GL_FRONT)
			rasterization.cullMode = VK_CULL_MODE_FRONT_BIT;
		if (raster->cull_mode == GL_FRONT_AND_BACK)
			rasterization.cullMode = VK_CULL_MODE_FRONT_AND_BACK;
	}

	/* The front face. */
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	if (raster->front_face == GL_CW)
		rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rasterization.depthBiasEnable = raster->polygon_offset;
	rasterization.lineWidth = 1.0f;

	/* One sample, alpha to coverage as GL has it. */
	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	multisample.alphaToCoverageEnable = (VkBool32)state->sample_alpha_to_coverage;

	/* Depth and stencil (GL's compare functions are Vulkan's in the same order). */
	memset(&depth, 0, sizeof(depth));
	depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth.depthTestEnable = raster->depth_test;
	depth.depthWriteEnable = raster->depth_write;
	depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
	if (raster->depth_test)
		depth.depthCompareOp = (VkCompareOp)(raster->depth_func - GL_NEVER);
	depth.stencilTestEnable = raster->stencil_test;
	faces[0] = &depth.front;
	faces[1] = &depth.back;
	for (face = 0U; raster->stencil_test && face < 2U; face++) {
		faces[face]->failOp = draw_stencil_op(raster->stencil_fail[face]);
		faces[face]->passOp = draw_stencil_op(raster->stencil_zpass[face]);
		faces[face]->depthFailOp = draw_stencil_op(raster->stencil_zfail[face]);
		faces[face]->compareOp = (VkCompareOp)(raster->stencil_func[face] - GL_NEVER);
	}

	/* The depth bounds test is off. */
	depth.maxDepthBounds = 1.0f;

	/* Blending and the colour mask. */
	memset(&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.blendEnable = raster->blend;
	blend_attachment.srcColorBlendFactor = draw_blend_factor(raster->blend_src_rgb);
	blend_attachment.dstColorBlendFactor = draw_blend_factor(raster->blend_dst_rgb);
	blend_attachment.colorBlendOp = draw_blend_op(raster->blend_equation_rgb);
	blend_attachment.srcAlphaBlendFactor = draw_blend_factor(raster->blend_src_alpha);
	blend_attachment.dstAlphaBlendFactor = draw_blend_factor(raster->blend_dst_alpha);
	blend_attachment.alphaBlendOp = draw_blend_op(raster->blend_equation_alpha);
	blend_attachment.colorWriteMask = raster->color_mask;
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1U;
	blend.pAttachments = &blend_attachment;

	/* The dynamic states. */
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = sizeof(dynamic_states) / sizeof(dynamic_states[0]);
	dynamic.pDynamicStates = dynamic_states;

	/* The pipeline. */
	entry = calloc(1U, sizeof(*entry));
	if (entry == NULL)
		return VK_NULL_HANDLE;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	create.stageCount = 2U;
	create.pStages = stages;
	create.pVertexInputState = &vertex;
	create.pInputAssemblyState = &assembly;
	create.pViewportState = &viewport;
	create.pRasterizationState = &rasterization;
	create.pMultisampleState = &multisample;
	create.pDepthStencilState = &depth;
	create.pColorBlendState = &blend;
	create.pDynamicState = &dynamic;
	create.layout = state->program->layout;
	create.renderPass = surface->pass;
	result = vkCreateGraphicsPipelines(state->device, VK_NULL_HANDLE, 1U, &create, NULL, &entry->pipeline);
	if (result != VK_SUCCESS) {
		free(entry);
		return VK_NULL_HANDLE;
	}

	/* Succeeded: kept for the next draw with the same key. */
	entry->key = key;
	entry->next = state->pipelines;
	state->pipelines = entry;
	return entry->pipeline;
}

/*
 * Returns a descriptor set for the current program: its uniform block
 * (dynamic: the offset of this draw's copy in the stream is returned in
 * *offset), and each sampler's texture (black when the unit has none that
 * can be sampled).  A draw with the same program, stream buffer and
 * textures as the one before reuses its set.  VK_NULL_HANDLE when there is
 * no memory.
 */
static VkDescriptorSet
draw_descriptors(
	struct gles_state *state,
	uint32_t *offset)
{
	VkDescriptorPoolSize sizes[2];
	VkDescriptorPoolCreateInfo create;
	VkDescriptorSetAllocateInfo allocate;
	VkWriteDescriptorSet writes[GLES_UNITS + 1U];
	VkDescriptorBufferInfo block;
	VkDescriptorImageInfo images[GLES_UNITS];
	uint32_t bindings[GLES_UNITS];
	struct gles_set_cache *cache;
	struct gles_program *program;
	struct gles_texture *texture;
	struct gles_pool *pool;
	VkDescriptorSet set;
	VkDeviceSize place;
	void *data;
	uint32_t count;
	uint32_t samplers;
	unsigned index;
	int status;
	int same;
	int differs;
	VkResult result;

	/* This draw's copy of the uniform block. */
	program = state->program;
	cache = &state->set_cache;
	*offset = 0U;
	memset(&block, 0, sizeof(block));
	if (program->uniform_data != NULL) {
		data = gles_stream(state, program->uniform_size, (size_t)state->limits.minUniformBufferOffsetAlignment,
				   &block.buffer, &place);
		if (data == NULL)
			return VK_NULL_HANDLE;
		memcpy(data, program->uniform_data, program->uniform_size);
		*offset = (uint32_t)place;
		block.range = program->uniform_size;
	}

	/* Each sampler's texture, up to date, with its sampler. */
	samplers = 0U;
	memset(images, 0, sizeof(images));
	for (index = 0U; index < program->uniform_count && samplers < GLES_UNITS; index++) {
		if (!program->uniforms[index].sampler)
			continue;
		texture = NULL;
		if (program->uniforms[index].unit >= 0 && (unsigned)program->uniforms[index].unit < GLES_UNITS)
			texture = state->units[program->uniforms[index].unit];
		status = gles_texture_complete(texture);
		if (!status)
			texture = gles_texture_black(state);
		if (texture == NULL)
			return VK_NULL_HANDLE;
		status = gles_texture_sync(state, texture);
		if (status != 0)
			return VK_NULL_HANDLE;
		texture->used = state->frame;

		/* The image and its sampler. */
		images[samplers].sampler = gles_sampler_get(state, texture);
		images[samplers].imageView = texture->view;
		images[samplers].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		bindings[samplers] = program->uniforms[index].binding;
		if (images[samplers].sampler == VK_NULL_HANDLE)
			return VK_NULL_HANDLE;
		samplers++;
	}

	/* The set of the draw before, when it describes the same things. */
	same = 0;
	if (cache->set != VK_NULL_HANDLE && cache->program == program->serial && cache->block == block.buffer &&
	    cache->count == samplers)
		same = 1;
	differs = 0;
	if (same && samplers != 0U)
		differs = memcmp(cache->images, images, samplers * sizeof(images[0]));
	if (differs != 0)
		same = 0;
	if (same)
		return cache->set;

	/* A set from the first pool with room, else from a new pool. */
	set = VK_NULL_HANDLE;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorSetCount = 1U;
	allocate.pSetLayouts = &program->set_layout;
	result = VK_ERROR_OUT_OF_POOL_MEMORY;
	for (pool = state->pools; pool != NULL; pool = pool->next) {
		allocate.descriptorPool = pool->pool;
		result = vkAllocateDescriptorSets(state->device, &allocate, &set);
		if (result == VK_SUCCESS)
			break;
	}

	/* No room: a new pool at the front. */
	if (result != VK_SUCCESS) {
		pool = calloc(1U, sizeof(*pool));
		if (pool == NULL)
			return VK_NULL_HANDLE;
		sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		sizes[0].descriptorCount = DRAW_POOL_SETS;
		sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		sizes[1].descriptorCount = DRAW_POOL_SETS * GLES_UNITS;
		memset(&create, 0, sizeof(create));
		create.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		create.maxSets = DRAW_POOL_SETS;
		create.poolSizeCount = 2U;
		create.pPoolSizes = sizes;
		result = vkCreateDescriptorPool(state->device, &create, NULL, &pool->pool);
		if (result != VK_SUCCESS) {
			free(pool);
			return VK_NULL_HANDLE;
		}

		/* The pool goes to the front of the list. */
		pool->next = state->pools;
		state->pools = pool;
		allocate.descriptorPool = pool->pool;
		result = vkAllocateDescriptorSets(state->device, &allocate, &set);
		if (result != VK_SUCCESS)
			return VK_NULL_HANDLE;
	}

	/* The uniform block (its offset is the draw's dynamic offset). */
	count = 0U;
	memset(writes, 0, sizeof(writes));
	if (program->uniform_data != NULL) {
		writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[count].dstSet = set;
		writes[count].dstBinding = program->uniform_binding;
		writes[count].descriptorCount = 1U;
		writes[count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		writes[count].pBufferInfo = &block;
		count++;
	}

	/* The samplers. */
	for (index = 0U; index < samplers; index++) {
		writes[count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[count].dstSet = set;
		writes[count].dstBinding = bindings[index];
		writes[count].descriptorCount = 1U;
		writes[count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[count].pImageInfo = &images[index];
		count++;
	}

	/* Written. */
	if (count != 0U)
		vkUpdateDescriptorSets(state->device, count, writes, 0U, NULL);

	/* Succeeded: the set, kept for the next draw like this one. */
	cache->set = set;
	cache->program = program->serial;
	cache->block = block.buffer;
	cache->count = samplers;
	memcpy(cache->images, images, sizeof(images));
	return set;
}

/* Records the dynamic state: viewport (GL's, turned over), scissor, line width, depth bias, blend colour, stencil values. */
static void
draw_dynamic(
	struct gles_state *state,
	struct zegl_context *context,
	struct zegl_surface *surface)
{
	VkViewport viewport;
	VkRect2D scissor;
	float width;
	float height;

	/* The viewport, from the top (a zero size is made one pixel so Vulkan takes it). */
	width = (float)context->gles.viewport[2];
	height = (float)context->gles.viewport[3];
	if (width < 1.0f)
		width = 1.0f;
	if (height < 1.0f)
		height = 1.0f;
	viewport.x = (float)context->gles.viewport[0];
	viewport.y = (float)surface->extent.height - (float)context->gles.viewport[1] - height;
	viewport.width = width;
	viewport.height = height;
	viewport.minDepth = state->depth_near;
	viewport.maxDepth = state->depth_far;
	vkCmdSetViewport(surface->command, 0U, 1U, &viewport);

	/* The scissor box, or the whole surface. */
	scissor = draw_scissor_rect(state, surface);
	vkCmdSetScissor(surface->command, 0U, 1U, &scissor);

	/* Lines are one pixel wide (the device is made without wide lines). */
	vkCmdSetLineWidth(surface->command, 1.0f);

	/* The polygon offset, the blend colour, and the stencil values of both faces. */
	vkCmdSetDepthBias(surface->command, state->polygon_units, 0.0f, state->polygon_factor);
	vkCmdSetBlendConstants(surface->command, state->blend_color);
	vkCmdSetStencilCompareMask(surface->command, VK_STENCIL_FACE_FRONT_BIT, state->stencil_value_mask[0]);
	vkCmdSetStencilCompareMask(surface->command, VK_STENCIL_FACE_BACK_BIT, state->stencil_value_mask[1]);
	vkCmdSetStencilWriteMask(surface->command, VK_STENCIL_FACE_FRONT_BIT, state->stencil_write_mask[0]);
	vkCmdSetStencilWriteMask(surface->command, VK_STENCIL_FACE_BACK_BIT, state->stencil_write_mask[1]);
	vkCmdSetStencilReference(surface->command, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)state->stencil_ref[0]);
	vkCmdSetStencilReference(surface->command, VK_STENCIL_FACE_BACK_BIT, (uint32_t)state->stencil_ref[1]);
}

/* Returns the scissor box in Vulkan's coordinates inside the surface, or the whole surface when the test is off. */
static VkRect2D
draw_scissor_rect(
	struct gles_state *state,
	struct zegl_surface *surface)
{
	VkRect2D rect;
	int32_t left;
	int32_t right;
	int32_t top;
	int32_t bottom;

	/* The whole surface. */
	rect.offset.x = 0;
	rect.offset.y = 0;
	rect.extent = surface->extent;
	if (!state->scissor_test)
		return rect;

	/* The box from GL's bottom-left origin to Vulkan's top-left, clipped to the surface. */
	left = state->scissor[0];
	right = state->scissor[0] + state->scissor[2];
	top = (int32_t)surface->extent.height - (state->scissor[1] + state->scissor[3]);
	bottom = (int32_t)surface->extent.height - state->scissor[1];
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (right > (int32_t)surface->extent.width)
		right = (int32_t)surface->extent.width;
	if (bottom > (int32_t)surface->extent.height)
		bottom = (int32_t)surface->extent.height;
	if (right < left)
		right = left;
	if (bottom < top)
		bottom = top;

	/* The clipped box. */
	rect.offset.x = left;
	rect.offset.y = top;
	rect.extent.width = (uint32_t)(right - left);
	rect.extent.height = (uint32_t)(bottom - top);
	return rect;
}

/* Returns Vulkan's blend factor for a GL one. */
static VkBlendFactor
draw_blend_factor(
	GLenum factor)
{
	/* The factors of OpenGL ES 2. */
	switch (factor) {
	case GL_ZERO:
		return VK_BLEND_FACTOR_ZERO;
	case GL_SRC_COLOR:
		return VK_BLEND_FACTOR_SRC_COLOR;
	case GL_ONE_MINUS_SRC_COLOR:
		return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case GL_DST_COLOR:
		return VK_BLEND_FACTOR_DST_COLOR;
	case GL_ONE_MINUS_DST_COLOR:
		return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case GL_SRC_ALPHA:
		return VK_BLEND_FACTOR_SRC_ALPHA;
	case GL_ONE_MINUS_SRC_ALPHA:
		return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case GL_DST_ALPHA:
		return VK_BLEND_FACTOR_DST_ALPHA;
	case GL_ONE_MINUS_DST_ALPHA:
		return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	case GL_CONSTANT_COLOR:
		return VK_BLEND_FACTOR_CONSTANT_COLOR;
	case GL_ONE_MINUS_CONSTANT_COLOR:
		return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
	case GL_CONSTANT_ALPHA:
		return VK_BLEND_FACTOR_CONSTANT_ALPHA;
	case GL_ONE_MINUS_CONSTANT_ALPHA:
		return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
	case GL_SRC_ALPHA_SATURATE:
		return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
	default:
		break;
	}

	/* GL_ONE (and a pipeline without blending). */
	return VK_BLEND_FACTOR_ONE;
}

/* Returns Vulkan's blend operation for a GL equation. */
static VkBlendOp
draw_blend_op(
	GLenum equation)
{
	/* The three equations of OpenGL ES 2, and the two of EXT_blend_minmax. */
	switch (equation) {
	case GL_FUNC_SUBTRACT:
		return VK_BLEND_OP_SUBTRACT;
	case GL_FUNC_REVERSE_SUBTRACT:
		return VK_BLEND_OP_REVERSE_SUBTRACT;
	case GL_MIN_EXT:
		return VK_BLEND_OP_MIN;
	case GL_MAX_EXT:
		return VK_BLEND_OP_MAX;
	default:
		break;
	}

	/* GL_FUNC_ADD. */
	return VK_BLEND_OP_ADD;
}

/* Returns Vulkan's stencil operation for a GL one. */
static VkStencilOp
draw_stencil_op(
	GLenum op)
{
	/* The operations of OpenGL ES 2. */
	switch (op) {
	case GL_ZERO:
		return VK_STENCIL_OP_ZERO;
	case GL_REPLACE:
		return VK_STENCIL_OP_REPLACE;
	case GL_INCR:
		return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
	case GL_DECR:
		return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
	case GL_INVERT:
		return VK_STENCIL_OP_INVERT;
	case GL_INCR_WRAP:
		return VK_STENCIL_OP_INCREMENT_AND_WRAP;
	case GL_DECR_WRAP:
		return VK_STENCIL_OP_DECREMENT_AND_WRAP;
	default:
		break;
	}

	/* GL_KEEP. */
	return VK_STENCIL_OP_KEEP;
}
