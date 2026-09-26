/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Buffer objects and the device memory under all of zedBSD's OpenGL ES
 * (WS068 p008).
 *
 * A buffer object keeps its bytes on the CPU; a draw copies them into a
 * host-visible device buffer when they changed.  The stream is memory for
 * one frame (client arrays, converted indices, uniform values), handed
 * out front to back and reset when the frame is done, as are the
 * descriptor pools; the garbage holds device objects a frame still uses.
 * Uploads to images go through one command buffer that is submitted and
 * waited for at once.
 */

#include "gles.h"

#include <stdlib.h>
#include <string.h>

/* How long an upload may take, in nanoseconds. */
#define GLES_UPLOAD_TIMEOUT	10000000000ULL

static struct gles_buffer *buffer_bound(struct zegl_context *context, GLenum target);
static struct gles_chunk *buffer_chunk(struct gles_state *state, size_t size);

/*
 * Returns the index of a memory type allowed by a set that has the
 * properties asked for, or UINT32_MAX when none has them.
 */
uint32_t
gles_memory_type(
	struct gles_state *state,
	uint32_t bits,
	VkMemoryPropertyFlags flags)
{
	uint32_t index;

	/* The first allowed type with every property. */
	for (index = 0U; index < state->memory.memoryTypeCount; index++) {
		if ((bits & (1U << index)) == 0U)
			continue;
		if ((state->memory.memoryTypes[index].propertyFlags & flags) == flags)
			return index;
	}

	/* None. */
	return UINT32_MAX;
}

/*
 * Makes a host-visible, coherent device buffer of a size, and maps it.
 * Returns 0, or -1 (nothing made) when the device cannot give one.
 */
int
gles_device_buffer(
	struct gles_state *state,
	size_t size,
	VkBufferUsageFlags usage,
	VkBuffer *buffer,
	VkDeviceMemory *memory,
	void **mapped)
{
	VkBufferCreateInfo create;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	uint32_t type;
	VkResult result;

	/* The buffer. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	create.size = size;
	if (create.size == 0U)
		create.size = 4U;
	create.usage = usage;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = vkCreateBuffer(state->device, &create, NULL, buffer);
	if (result != VK_SUCCESS)
		return -1;

	/* Memory the CPU writes and the GPU sees without flushes. */
	vkGetBufferMemoryRequirements(state->device, *buffer, &requirements);
	type = gles_memory_type(state, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (type == UINT32_MAX) {
		vkDestroyBuffer(state->device, *buffer, NULL);
		return -1;
	}

	/* The allocation. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	result = vkAllocateMemory(state->device, &allocate, NULL, memory);
	if (result != VK_SUCCESS) {
		vkDestroyBuffer(state->device, *buffer, NULL);
		return -1;
	}

	/* Bound and mapped for good. */
	result = vkBindBufferMemory(state->device, *buffer, *memory, 0U);
	if (result == VK_SUCCESS)
		result = vkMapMemory(state->device, *memory, 0U, VK_WHOLE_SIZE, 0U, mapped);
	if (result != VK_SUCCESS) {
		vkDestroyBuffer(state->device, *buffer, NULL);
		vkFreeMemory(state->device, *memory, NULL);
		return -1;
	}

	/* Succeeded: the buffer, mapped. */
	return 0;
}

/*
 * Hands out stream memory for the frame: a place to write size bytes at
 * an alignment, and the device buffer and offset a command reads it at.
 * Returns NULL when there is no memory.
 */
void *
gles_stream(
	struct gles_state *state,
	size_t size,
	size_t alignment,
	VkBuffer *buffer,
	VkDeviceSize *offset)
{
	struct gles_chunk *chunk;
	size_t start;

	/* The first chunk with room at the alignment. */
	if (alignment == 0U)
		alignment = 4U;
	for (chunk = state->chunks; chunk != NULL; chunk = chunk->next) {
		start = (chunk->used + alignment - 1U) / alignment * alignment;
		if (start + size <= chunk->size)
			break;
	}

	/* None: a new chunk at least that large. */
	if (chunk == NULL) {
		chunk = buffer_chunk(state, size);
		if (chunk == NULL)
			return NULL;
		start = 0U;
	}

	/* Succeeded: the place, taken. */
	chunk->used = start + size;
	*buffer = chunk->buffer;
	*offset = start;
	return chunk->mapped + start;
}

/*
 * Puts device objects aside until the frame being recorded is done.
 */
void
gles_throw_away(
	struct gles_state *state,
	VkBuffer buffer,
	VkImage image,
	VkImageView view,
	VkDeviceMemory memory)
{
	struct gles_garbage objects;

	/* The objects, kept as one entry. */
	memset(&objects, 0, sizeof(objects));
	objects.buffer = buffer;
	objects.image = image;
	objects.view = view;
	objects.memory = memory;
	gles_garbage_keep(state, &objects);
}

/*
 * Puts a set of Vulkan objects aside until the frame being recorded is
 * done (without memory for the entry, the device waits and they go now).
 */
void
gles_garbage_keep(
	struct gles_state *state,
	const struct gles_garbage *objects)
{
	struct gles_garbage *garbage;

	/* Nothing to keep. */
	if (objects->buffer == VK_NULL_HANDLE && objects->image == VK_NULL_HANDLE && objects->view == VK_NULL_HANDLE &&
	    objects->memory == VK_NULL_HANDLE && objects->pipeline == VK_NULL_HANDLE && objects->layout == VK_NULL_HANDLE &&
	    objects->set_layout == VK_NULL_HANDLE && objects->modules[0] == VK_NULL_HANDLE && objects->modules[1] == VK_NULL_HANDLE)
		return;

	/* The entry. */
	garbage = malloc(sizeof(*garbage));
	if (garbage == NULL) {
		(void)vkDeviceWaitIdle(state->device);
		gles_garbage_destroy(state, objects);
		return;
	}

	/* Succeeded: waiting for the frame. */
	*garbage = *objects;
	garbage->next = state->garbage;
	state->garbage = garbage;
}

/*
 * Destroys a set of Vulkan objects nothing uses any more.
 */
void
gles_garbage_destroy(
	struct gles_state *state,
	const struct gles_garbage *objects)
{
	VkDevice device;

	/* Views before images, objects before their memory. */
	device = state->device;
	if (objects->pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(device, objects->pipeline, NULL);
	if (objects->layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(device, objects->layout, NULL);
	if (objects->set_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(device, objects->set_layout, NULL);
	if (objects->modules[0] != VK_NULL_HANDLE)
		vkDestroyShaderModule(device, objects->modules[0], NULL);
	if (objects->modules[1] != VK_NULL_HANDLE)
		vkDestroyShaderModule(device, objects->modules[1], NULL);
	if (objects->view != VK_NULL_HANDLE)
		vkDestroyImageView(device, objects->view, NULL);
	if (objects->image != VK_NULL_HANDLE)
		vkDestroyImage(device, objects->image, NULL);
	if (objects->buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(device, objects->buffer, NULL);
	if (objects->memory != VK_NULL_HANDLE)
		vkFreeMemory(device, objects->memory, NULL);
}

/*
 * Frees what the frame that just finished held: the garbage, the stream
 * (all but its first chunk) and the descriptor sets.
 */
void
gles_collect(
	struct gles_state *state)
{
	struct gles_garbage *garbage;
	struct gles_chunk *chunk;
	struct gles_pool *pool;

	/* The garbage. */
	while (state->garbage != NULL) {
		garbage = state->garbage;
		state->garbage = garbage->next;
		gles_garbage_destroy(state, garbage);
		free(garbage);
	}

	/* The stream: the chunks after the first go, the first starts empty. */
	while (state->chunks != NULL && state->chunks->next != NULL) {
		chunk = state->chunks->next;
		state->chunks->next = chunk->next;
		vkDestroyBuffer(state->device, chunk->buffer, NULL);
		vkFreeMemory(state->device, chunk->memory, NULL);
		free(chunk);
	}

	/* The first chunk is empty again. */
	if (state->chunks != NULL)
		state->chunks->used = 0U;

	/* The descriptor pools give their sets back; the last draw's set is gone with them. */
	for (pool = state->pools; pool != NULL; pool = pool->next)
		(void)vkResetDescriptorPool(state->device, pool->pool, 0U);
	memset(&state->set_cache, 0, sizeof(state->set_cache));
}

/*
 * Brings a buffer object's device copy up to date with its bytes: in
 * place when no draw of this frame reads it, else in a new device buffer.
 * Returns 0, or -1 when the device has no memory for it.
 */
int
gles_buffer_sync(
	struct gles_state *state,
	struct gles_buffer *buffer)
{
	VkBuffer device_buffer;
	VkDeviceMemory memory;
	void *mapped;
	int status;

	/* A copy that is up to date stays. */
	if (!buffer->dirty && buffer->buffer != VK_NULL_HANDLE)
		return 0;

	/* In place: the copy is large enough and this frame has not drawn from it. */
	if (buffer->buffer != VK_NULL_HANDLE && buffer->device_size >= buffer->size && buffer->used != state->frame) {
		memcpy(buffer->mapped, buffer->data, buffer->size);
		buffer->dirty = 0;
		return 0;
	}

	/* A new device buffer, the old one kept for the frame. */
	status = gles_device_buffer(state, buffer->size,
				    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
				    &device_buffer, &memory, &mapped);
	if (status != 0)
		return -1;
	gles_throw_away(state, buffer->buffer, VK_NULL_HANDLE, VK_NULL_HANDLE, buffer->memory);

	/* Succeeded: the bytes in the new buffer. */
	if (buffer->size != 0U)
		memcpy(mapped, buffer->data, buffer->size);
	buffer->buffer = device_buffer;
	buffer->memory = memory;
	buffer->mapped = mapped;
	buffer->device_size = buffer->size;
	buffer->dirty = 0;
	return 0;
}

/*
 * Frees a buffer object; its device copy waits for the frame.
 */
void
gles_buffer_free(
	struct gles_state *state,
	struct gles_buffer *buffer)
{
	/* The device copy, then the bytes and the object. */
	gles_throw_away(state, buffer->buffer, VK_NULL_HANDLE, VK_NULL_HANDLE, buffer->memory);
	free(buffer->data);
	free(buffer);
}

/*
 * Starts recording an upload.  Returns 0, or -1 when the command buffer
 * cannot record.
 */
int
gles_upload_begin(
	struct gles_state *state)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo allocate;
	VkFenceCreateInfo fence;
	VkCommandBufferBeginInfo begin;
	VkResult result;

	/* The pool, the command buffer and the fence, made at the first upload. */
	if (state->upload_pool == VK_NULL_HANDLE) {
		memset(&pool, 0, sizeof(pool));
		pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pool.queueFamilyIndex = state->display->family;
		result = vkCreateCommandPool(state->device, &pool, NULL, &state->upload_pool);
		if (result != VK_SUCCESS)
			return -1;

		/* The command buffer. */
		memset(&allocate, 0, sizeof(allocate));
		allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocate.commandPool = state->upload_pool;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 1U;
		result = vkAllocateCommandBuffers(state->device, &allocate, &state->upload);
		if (result != VK_SUCCESS)
			return -1;

		/* The fence. */
		memset(&fence, 0, sizeof(fence));
		fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		result = vkCreateFence(state->device, &fence, NULL, &state->upload_fence);
		if (result != VK_SUCCESS)
			return -1;
	}

	/* Recording. */
	(void)vkResetCommandBuffer(state->upload, 0U);
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(state->upload, &begin);
	if (result != VK_SUCCESS)
		return -1;

	/* Succeeded: the upload records. */
	return 0;
}

/*
 * Submits the upload and waits for it.  Returns 0, or -1 when it failed.
 */
int
gles_upload_end(
	struct gles_state *state)
{
	VkSubmitInfo submit;
	VkResult result;

	/* The recording ends. */
	result = vkEndCommandBuffer(state->upload);
	if (result != VK_SUCCESS)
		return -1;

	/* Submitted on the display's queue. */
	result = vkResetFences(state->device, 1U, &state->upload_fence);
	if (result != VK_SUCCESS)
		return -1;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &state->upload;
	result = vkQueueSubmit(state->display->queue, 1U, &submit, state->upload_fence);
	if (result != VK_SUCCESS)
		return -1;

	/* Done before anything reads what it wrote. */
	result = vkWaitForFences(state->device, 1U, &state->upload_fence, VK_TRUE, GLES_UPLOAD_TIMEOUT);
	if (result != VK_SUCCESS)
		return -1;

	/* Succeeded: the upload is on the device. */
	return 0;
}

/*
 * Makes names for buffer objects.
 */
GL_APICALL void GL_APIENTRY
glGenBuffers(
	GLsizei n,
	GLuint *buffers)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_buffer *buffer;
	GLsizei index;
	int status;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (n < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each name gets an empty buffer object. */
	for (index = 0; index < n; index++) {
		buffer = calloc(1U, sizeof(*buffer));
		if (buffer == NULL) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* The first free name. */
		buffer->name = gles_names_free(&state->buffers);
		buffer->usage = GL_STATIC_DRAW;
		status = gles_names_add(&state->buffers, buffer->name, buffer);
		if (status != 0) {
			free(buffer);
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* The name goes back to the application. */
		buffers[index] = buffer->name;
	}
}

/*
 * Deletes buffer objects, unbinding them.
 */
GL_APICALL void GL_APIENTRY
glDeleteBuffers(
	GLsizei n,
	const GLuint *buffers)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_buffer *buffer;
	GLsizei index;
	unsigned attrib;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (n < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each name that is a buffer. */
	for (index = 0; index < n; index++) {
		buffer = gles_names_get(&state->buffers, buffers[index]);
		if (buffer == NULL)
			continue;

		/* Unbound from the targets and the attributes. */
		if (state->array_buffer == buffer)
			state->array_buffer = NULL;
		if (state->element_buffer == buffer)
			state->element_buffer = NULL;
		for (attrib = 0U; attrib < GLES_ATTRIBS; attrib++) {
			if (state->attribs[attrib].buffer == buffer)
				state->attribs[attrib].buffer = NULL;
		}

		/* The name and the object go. */
		gles_names_remove(&state->buffers, buffers[index]);
		gles_buffer_free(state, buffer);
	}
}

/*
 * Binds a buffer object to GL_ARRAY_BUFFER or GL_ELEMENT_ARRAY_BUFFER,
 * making it when the name is new.
 */
GL_APICALL void GL_APIENTRY
glBindBuffer(
	GLenum target,
	GLuint name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_buffer *buffer;
	int status;

	/* A context with its state, and a target. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The object: none for name 0, made for a name not yet used. */
	buffer = NULL;
	if (name != 0U) {
		buffer = gles_names_get(&state->buffers, name);
		if (buffer == NULL) {
			buffer = calloc(1U, sizeof(*buffer));
			if (buffer == NULL) {
				gles_error(context, GL_OUT_OF_MEMORY);
				return;
			}

			/* The new buffer takes the name. */
			buffer->name = name;
			buffer->usage = GL_STATIC_DRAW;
			status = gles_names_add(&state->buffers, name, buffer);
			if (status != 0) {
				free(buffer);
				gles_error(context, GL_OUT_OF_MEMORY);
				return;
			}
		}
	}

	/* Bound. */
	if (target == GL_ARRAY_BUFFER) {
		state->array_buffer = buffer;
	} else {
		state->element_buffer = buffer;
	}
}

/*
 * Gives the buffer bound to a target new bytes (or a size of undefined
 * bytes).
 */
GL_APICALL void GL_APIENTRY
glBufferData(
	GLenum target,
	GLsizeiptr size,
	const void *data,
	GLenum usage)
{
	struct zegl_context *context;
	struct gles_buffer *buffer;
	unsigned char *bytes;

	/* The bound buffer. */
	context = gles_context();
	buffer = buffer_bound(context, target);
	if (buffer == NULL)
		return;
	if (size < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The new bytes. */
	bytes = malloc((size_t)size + 1U);
	if (bytes == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/* Cleared, then filled from the application's bytes when it gave some. */
	memset(bytes, 0, (size_t)size + 1U);
	if (data != NULL)
		memcpy(bytes, data, (size_t)size);

	/* Succeeded: they replace the old ones, and the device copy is stale. */
	free(buffer->data);
	buffer->data = bytes;
	buffer->size = (size_t)size;
	buffer->usage = usage;
	buffer->dirty = 1;
}

/*
 * Replaces a range of the bytes of the buffer bound to a target.
 */
GL_APICALL void GL_APIENTRY
glBufferSubData(
	GLenum target,
	GLintptr offset,
	GLsizeiptr size,
	const void *data)
{

	struct zegl_context *context;
	struct gles_buffer *buffer;

	/* The bound buffer and a range inside it. */
	context = gles_context();
	buffer = buffer_bound(context, target);
	if (buffer == NULL)
		return;
	if (offset < 0 || size < 0 || (size_t)offset + (size_t)size > buffer->size) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The bytes change; the device copy is stale. */
	if (size != 0 && data != NULL)
		memcpy(buffer->data + offset, data, (size_t)size);
	buffer->dirty = 1;
}

/*
 * Reports whether a name is a buffer object.
 */
GL_APICALL GLboolean GL_APIENTRY
glIsBuffer(
	GLuint name)
{
	struct zegl_context *context;
	struct gles_state *state;
	void *object;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return GL_FALSE;

	/* The name's object. */
	object = gles_names_get(&state->buffers, name);
	if (object == NULL)
		return GL_FALSE;
	return GL_TRUE;
}

/*
 * Reports the size or usage of the buffer bound to a target.
 */
GL_APICALL void GL_APIENTRY
glGetBufferParameteriv(
	GLenum target,
	GLenum pname,
	GLint *params)
{
	struct zegl_context *context;
	struct gles_buffer *buffer;

	/* The bound buffer. */
	context = gles_context();
	buffer = buffer_bound(context, target);
	if (buffer == NULL)
		return;

	/* The parameter asked for. */
	switch (pname) {
	case GL_BUFFER_SIZE:
		*params = (GLint)buffer->size;
		return;
	case GL_BUFFER_USAGE:
		*params = (GLint)buffer->usage;
		return;
	default:
		break;
	}

	/* Any other is an error. */
	gles_error(context, GL_INVALID_ENUM);
}

/* Returns the buffer bound to a target, recording the error when the target is wrong or nothing is bound. */
static struct gles_buffer *
buffer_bound(
	struct zegl_context *context,
	GLenum target)
{
	struct gles_state *state;

	/* A context with its state. */
	state = gles_state(context);
	if (state == NULL)
		return NULL;

	/* The target's buffer. */
	switch (target) {
	case GL_ARRAY_BUFFER:
		if (state->array_buffer == NULL)
			gles_error(context, GL_INVALID_OPERATION);
		return state->array_buffer;
	case GL_ELEMENT_ARRAY_BUFFER:
		if (state->element_buffer == NULL)
			gles_error(context, GL_INVALID_OPERATION);
		return state->element_buffer;
	default:
		break;
	}

	/* Any other target. */
	gles_error(context, GL_INVALID_ENUM);
	return NULL;
}

/* Makes a stream chunk of at least a size and puts it after the others; NULL when there is no memory. */
static struct gles_chunk *
buffer_chunk(
	struct gles_state *state,
	size_t size)
{
	struct gles_chunk *chunk;
	struct gles_chunk **last;
	void *mapped;
	int status;

	/* The entry. */
	chunk = calloc(1U, sizeof(*chunk));
	if (chunk == NULL)
		return NULL;

	/* Its device buffer, for every use a draw makes of the stream. */
	chunk->size = GLES_STREAM_CHUNK;
	if (size > chunk->size)
		chunk->size = size;
	status = gles_device_buffer(state, chunk->size,
				    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
				    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
				    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				    &chunk->buffer, &chunk->memory, &mapped);
	if (status != 0) {
		free(chunk);
		return NULL;
	}

	/* The chunk's mapping. */
	chunk->mapped = mapped;

	/* Succeeded: the chunk, last in the list. */
	for (last = &state->chunks; *last != NULL; last = &(*last)->next)
		continue;
	*last = chunk;
	return chunk;
}
