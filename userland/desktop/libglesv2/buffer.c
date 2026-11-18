/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Buffer objects and the device memory under all of zedBSD's OpenGL ES
 * (WS068 p008), and OpenGL ES 3's buffer and vertex array calls (WS068
 * p024).
 *
 * A buffer object keeps its bytes on the CPU; a draw copies them into a
 * host-visible device buffer when they changed.  The stream is memory for
 * one frame (client arrays, converted indices, uniform values), handed
 * out front to back and reset when the frame is done, as are the
 * descriptor pools; the garbage holds device objects a frame still uses.
 * Uploads to images go through one command buffer that is submitted and
 * waited for at once.
 *
 * glMapBufferRange hands out the CPU bytes themselves (what the GPU
 * wrote into the device copy, by transform feedback or a compute shader's
 * storage blocks, is read back first: gles_buffer_fetch), and unmapping
 * marks the device copy stale like glBufferSubData.  A vertex
 * array object is a saved copy of the attributes' arrays and the element
 * buffer: binding one saves the context's into the one bound before and
 * loads its own.
 */

#include "gles.h"

#include <stdlib.h>
#include <string.h>

/* How long an upload may take, in nanoseconds. */
#define GLES_UPLOAD_TIMEOUT	10000000000ULL

static struct gles_buffer **buffer_slot(struct gles_state *state, GLenum target);
static void timed_delete_buffers(GLsizei n, const GLuint *buffers);
static int buffer_device_copy(struct gles_state *state, size_t size, VkBuffer *device_buffer, VkDeviceMemory *memory, void **mapped);
static void buffer_throw_copy_away(struct gles_state *state, struct gles_buffer *buffer);
static int buffer_spare_keep(struct gles_state *state, const struct gles_garbage *objects);
static void buffer_spare_free(struct gles_state *state, unsigned index);
static int buffer_data_on_device(struct gles_state *state, struct gles_buffer *buffer, size_t size, const void *data);
static void timed_buffer_data(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
static void *timed_map_buffer_range(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
static GLboolean timed_unmap_buffer(GLenum target);
static struct gles_buffer *buffer_bound(struct zegl_context *context, GLenum target);
static struct gles_buffer *buffer_named(struct zegl_context *context, struct gles_state *state, GLuint name);
static int buffer_parameter(struct zegl_context *context, GLenum target, GLenum pname, GLint64 *value);
static void buffer_bind_range(GLenum target, GLuint index, GLuint name, GLintptr offset, GLsizeiptr size, int whole);
static int buffer_indexed(struct zegl_context *context, GLenum target, GLuint index, GLint64 *value);
static void buffer_forget(struct gles_attrib *attribs, struct gles_buffer **element_buffer, struct gles_buffer *buffer);
static struct gles_chunk *buffer_chunk(struct gles_state *state, size_t size, int cached);
static void *buffer_stream(struct gles_state *state, size_t size, size_t alignment, int cached, VkBuffer *buffer, VkDeviceSize *offset);
static int buffer_device_typed(struct gles_state *state, size_t size, VkBufferUsageFlags usage, int cached, VkBuffer *buffer,
			       VkDeviceMemory *memory, void **mapped);

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
	/* The first host-visible, coherent type. */
	return buffer_device_typed(state, size, usage, 0, buffer, memory, mapped);
}

/*
 * Makes a host-visible, coherent device buffer of a size, and maps it: of
 * a type the CPU also caches when cached is set and the device has one
 * (what the CPU reads back), else of the first host-visible, coherent
 * type.  Returns 0, or -1 (nothing made) when the device cannot give one.
 */
static int
buffer_device_typed(
	struct gles_state *state,
	size_t size,
	VkBufferUsageFlags usage,
	int cached,
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

	/* Memory the CPU writes (or reads, through its cache when asked) and the GPU sees without flushes. */
	vkGetBufferMemoryRequirements(state->device, *buffer, &requirements);
	type = UINT32_MAX;
	if (cached)
		type = gles_memory_type(state, requirements.memoryTypeBits,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
	if (type == UINT32_MAX)
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
	/* The stream the CPU writes and the GPU reads. */
	return buffer_stream(state, size, alignment, 0, buffer, offset);
}

/*
 * Hands out stream memory the GPU writes and the CPU then reads (a read
 * of pixels): of a type the CPU caches when the device has one, which the
 * CPU reads far faster than a device's uncached window; the ordinary
 * stream when the device cannot give that memory.  Returns as
 * gles_stream() does.
 */
void *
gles_stream_read(
	struct gles_state *state,
	size_t size,
	size_t alignment,
	VkBuffer *buffer,
	VkDeviceSize *offset)
{
	void *mapped;

	/* The stream the CPU reads back, else the ordinary one. */
	mapped = buffer_stream(state, size, alignment, 1, buffer, offset);
	if (mapped == NULL)
		mapped = buffer_stream(state, size, alignment, 0, buffer, offset);
	return mapped;
}

/* Hands out stream memory from the chunks of one kind (cached or not), as gles_stream() describes. */
static void *
buffer_stream(
	struct gles_state *state,
	size_t size,
	size_t alignment,
	int cached,
	VkBuffer *buffer,
	VkDeviceSize *offset)
{
	struct gles_chunk *chunk;
	size_t start;

	/* The first chunk of the kind with room at the alignment that no frame in flight reads. */
	if (alignment == 0U)
		alignment = 4U;
	for (chunk = state->chunks; chunk != NULL; chunk = chunk->next) {
		if (chunk->cached != cached)
			continue;
		if (chunk->frame != state->frame && chunk->frame > state->done)
			continue;
		start = (chunk->used + alignment - 1U) / alignment * alignment;
		if (start + size <= chunk->size)
			break;
	}

	/* None: a new chunk at least that large. */
	if (chunk == NULL) {
		chunk = buffer_chunk(state, size, cached);
		if (chunk == NULL)
			return NULL;
		start = 0U;
	}

	/* Succeeded: the place, taken for the frame being recorded. */
	chunk->used = start + size;
	chunk->frame = state->frame;
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
	    objects->set_layout == VK_NULL_HANDLE && objects->modules[0] == VK_NULL_HANDLE && objects->modules[1] == VK_NULL_HANDLE &&
	    objects->modules[2] == VK_NULL_HANDLE && objects->pass == VK_NULL_HANDLE && objects->framebuffer == VK_NULL_HANDLE)
		return;

	/* The entry. */
	garbage = malloc(sizeof(*garbage));
	if (garbage == NULL) {
		(void)vkDeviceWaitIdle(state->device);
		gles_garbage_destroy(state, objects);
		return;
	}

	/* Succeeded: waiting for the frame being recorded (and those before it) to be done. */
	*garbage = *objects;
	garbage->frame = state->frame;
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
	unsigned index;
	int kept;

	/* A buffer object's device copy may be kept for reuse instead (ws101-p017). */
	device = state->device;
	kept = buffer_spare_keep(state, objects);
	if (kept)
		return;

	/* Views before images, objects before their memory. */
	if (objects->buffer_view != VK_NULL_HANDLE)
		vkDestroyBufferView(device, objects->buffer_view, NULL);
	if (objects->pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(device, objects->pipeline, NULL);
	if (objects->layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(device, objects->layout, NULL);
	if (objects->set_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(device, objects->set_layout, NULL);
	for (index = 0U; index < sizeof(objects->modules) / sizeof(objects->modules[0]); index++) {
		if (objects->modules[index] != VK_NULL_HANDLE)
			vkDestroyShaderModule(device, objects->modules[index], NULL);
	}

	/* A framebuffer object's framebuffer and pass. */
	if (objects->framebuffer != VK_NULL_HANDLE)
		vkDestroyFramebuffer(device, objects->framebuffer, NULL);
	if (objects->pass != VK_NULL_HANDLE)
		vkDestroyRenderPass(device, objects->pass, NULL);
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
 * Moves on to the next frame once the one being recorded was submitted:
 * finished nonzero when it is done too (a flush, a pbuffer), zero when it
 * may still run on the GPU and only the frames before it are done (a
 * window's present, ws068-p009).  What the frames done held is freed.
 */
void
gles_frame_finished(
	struct gles_state *state,
	int finished)
{
	/* The next frame, and the last one done. */
	state->frame++;
	state->done = state->frame - 2U;
	if (finished) {
		state->done = state->frame - 1U;
		state->flight = NULL;
	}

	/* What the frames done held. */
	gles_collect(state);
}

/*
 * Waits for every frame submitted, the ones in flight too (glFinish, and
 * a wait for a frame in flight), and frees what they held.  Returns 0, or
 * -1 with the error recorded.
 */
int
gles_frame_retire(
	struct zegl_context *context,
	struct gles_state *state)
{
	EGLint error;

	/* The queue idle. */
	error = zegl_retire(state->display);
	if (error != EGL_SUCCESS) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return -1;
	}

	/* Every frame before the one being recorded is done. */
	state->done = state->frame - 1U;
	state->flight = NULL;
	gles_collect(state);
	return 0;
}

/*
 * Frees what the frames that are done held (state->done): their garbage,
 * their stream (one empty chunk is kept) and their descriptor sets.  What
 * a frame in flight or the frame being recorded holds stays.
 */
void
gles_collect(
	struct gles_state *state)
{
	struct gles_garbage **link;
	struct gles_garbage *garbage;
	struct gles_chunk **place;
	struct gles_chunk *chunk;
	struct gles_pool *pool;
	uint64_t started;
	unsigned index;
	int kept[2];

	/* The garbage of the frames done (the step is timed, ws101-p016). */
	started = gles_time_begin();
	link = &state->garbage;
	while (*link != NULL) {
		garbage = *link;
		if (garbage->frame > state->done) {
			link = &garbage->next;
			continue;
		}

		/* A frame done held it: it goes. */
		*link = garbage->next;
		gles_garbage_destroy(state, garbage);
		free(garbage);
	}

	/* The stream of the frames done: the first such chunk of each kind is kept empty, the others go. */
	kept[0] = 0;
	kept[1] = 0;
	place = &state->chunks;
	while (*place != NULL) {
		chunk = *place;
		if (chunk->frame > state->done) {
			place = &chunk->next;
			continue;
		}

		/* The first chunk of its kind a frame done held is kept, empty. */
		if (!kept[chunk->cached != 0]) {
			chunk->used = 0U;
			kept[chunk->cached != 0] = 1;
			place = &chunk->next;
			continue;
		}

		/* The others go. */
		*place = chunk->next;
		vkDestroyBuffer(state->device, chunk->buffer, NULL);
		vkFreeMemory(state->device, chunk->memory, NULL);
		free(chunk);
	}

	/* The descriptor pools of the frames done give their sets back. */
	for (pool = state->pools; pool != NULL; pool = pool->next) {
		if (pool->frame <= state->done)
			(void)vkResetDescriptorPool(state->device, pool->pool, 0U);
	}

	/* The cached set belonged to the frame that ended. */
	memset(&state->set_cache, 0, sizeof(state->set_cache));

	/* The spares not reused for GLES_SPARE_FRAMES frames go (ws101-p017). */
	index = 0U;
	while (index < state->spare_count) {
		if (state->frame - state->spares[index].frame > GLES_SPARE_FRAMES) {
			buffer_spare_free(state, index);
			continue;
		}

		/* A spare still young stays. */
		index++;
	}

	/* The step's time. */
	gles_time_end("collect", started, 0U);
}

/*
 * Returns the buffer object of a name (not 0), making it when the name is
 * new; NULL with the error recorded.
 */
struct gles_buffer *
gles_buffer_named(
	struct zegl_context *context,
	struct gles_state *state,
	GLuint name)
{
	struct gles_buffer *buffer;

	/* Succeeded: the object of the name. */
	buffer = buffer_named(context, state, name);
	return buffer;
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
	uint64_t started;
	void *mapped;
	int status;

	/* A copy that is up to date stays; bytes that live in the copy are always up to date (ws101-p017). */
	if (buffer->on_device) {
		buffer->dirty = 0;
		return 0;
	}

	/* A copy on the CPU's side that is up to date stays too. */
	if (!buffer->dirty && buffer->buffer != VK_NULL_HANDLE)
		return 0;

	/* In place: the copy is large enough and no frame that drew from it may still run (ws068-p009). */
	started = gles_time_begin();
	if (buffer->buffer != VK_NULL_HANDLE && buffer->device_size >= buffer->size && buffer->used <= state->done) {
		memcpy(buffer->mapped, buffer->data, buffer->size);
		buffer->dirty = 0;
		gles_time_end("upload-in-place", started, buffer->size);
		return 0;
	}

	/*
	 * A new device buffer (for any use a draw or a dispatch makes of a
	 * buffer object: texels, shader storage and dispatch sizes too), the
	 * old one kept for the frame.
	 */
	status = buffer_device_copy(state, buffer->size, &device_buffer, &memory, &mapped);
	if (status != 0)
		return -1;
	buffer_throw_copy_away(state, buffer);
	gles_time_end("device-buffer", started, buffer->size);

	/* Succeeded: the bytes in the new buffer. */
	started = gles_time_begin();
	if (buffer->size != 0U)
		memcpy(mapped, buffer->data, buffer->size);
	gles_time_end("upload", started, buffer->size);
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
	/* The device copy (with the bytes when they live in it), then the bytes and the object. */
	if (!buffer->on_device)
		free(buffer->data);
	buffer_throw_copy_away(state, buffer);
	free(buffer);
}

/*
 * Frees the vertex array objects and their namespace (the context's own
 * arrays are in its state).
 */
void
gles_vertex_arrays_release(
	struct gles_state *state)
{
	GLuint name;

	/* Each vertex array object. */
	for (name = 1U; name < state->vertex_arrays.capacity; name++)
		free(state->vertex_arrays.objects[name]);

	/* The namespace. */
	free(state->vertex_arrays.objects);
	state->vertex_arrays.objects = NULL;
	state->vertex_arrays.capacity = 0U;
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
 * Deletes buffer objects (timed, timed_delete_buffers).
 */
GL_APICALL void GL_APIENTRY
glDeleteBuffers(
	GLsizei n,
	const GLuint *buffers)
{
	uint64_t started;

	/* The call, timed when ZEDBSD_GLES_COMPUTE_TRACE is 2. */
	started = gles_time_begin();
	timed_delete_buffers(n, buffers);
	gles_time_end("delete-buffers", started, 0U);
}

/*
 * Deletes buffer objects, unbinding them.
 */
static void
timed_delete_buffers(
	GLsizei n,
	const GLuint *buffers)
{
	static const GLenum targets[] = {
		GL_ARRAY_BUFFER, GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, GL_UNIFORM_BUFFER,
		GL_PIXEL_PACK_BUFFER, GL_PIXEL_UNPACK_BUFFER, GL_TRANSFORM_FEEDBACK_BUFFER
	};
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_buffer *buffer;
	struct gles_buffer **slot;
	struct gles_vertex_array *array;
	struct gles_texture *texture;
	GLsizei index;
	GLuint name;
	unsigned target;
	unsigned binding;

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

		/* Unbound from the targets other than the element buffer (a vertex array's, below). */
		for (target = 0U; target < sizeof(targets) / sizeof(targets[0]); target++) {
			slot = buffer_slot(state, targets[target]);
			if (*slot == buffer)
				*slot = NULL;
		}

		/* Unbound from the indexed binding points. */
		for (binding = 0U; binding < GLES_UNIFORM_BINDINGS; binding++) {
			if (state->uniform_ranges[binding].buffer == buffer)
				memset(&state->uniform_ranges[binding], 0, sizeof(state->uniform_ranges[binding]));
		}

		/* And from the transform feedback ones. */
		for (binding = 0U; binding < GLES_FEEDBACK_BINDINGS; binding++) {
			if (state->feedback_ranges[binding].buffer == buffer)
				memset(&state->feedback_ranges[binding], 0, sizeof(state->feedback_ranges[binding]));
		}

		/* And from OpenGL ES 3.1's shader storage ones (ws101-p009). */
		for (binding = 0U; binding < GLES_STORAGE_BINDINGS; binding++) {
			if (state->storage_ranges[binding].buffer == buffer)
				memset(&state->storage_ranges[binding], 0, sizeof(state->storage_ranges[binding]));
		}

		/* And from the shader storage and dispatch targets. */
		if (state->storage_buffer == buffer)
			state->storage_buffer = NULL;
		if (state->dispatch_buffer == buffer)
			state->dispatch_buffer = NULL;
		if (state->draw_indirect_buffer == buffer)
			state->draw_indirect_buffer = NULL;

		/*
		 * Taken out of every vertex array: the bound one's (the
		 * context's), the default one's while another is bound, and
		 * every other object's, so none keeps a buffer that is gone.
		 */
		buffer_forget(state->attribs, &state->element_buffer, buffer);
		buffer_forget(state->default_array.attribs, &state->default_array.element_buffer, buffer);
		for (name = 1U; name < state->vertex_arrays.capacity; name++) {
			array = state->vertex_arrays.objects[name];
			if (array != NULL)
				buffer_forget(array->attribs, &array->element_buffer, buffer);
		}

		/* Unbound from desktop GL's buffer texture target. */
		if (state->texture_buffer == buffer)
			state->texture_buffer = NULL;

		/* Taken out of the buffer textures (desktop GL), which read nothing then. */
		for (name = 1U; name < state->textures.capacity; name++) {
			texture = state->textures.objects[name];
			if (texture != NULL && texture->texel_buffer == buffer)
				texture->texel_buffer = NULL;
		}

		/* The name and the object go. */
		gles_names_remove(&state->buffers, buffers[index]);
		gles_buffer_free(state, buffer);
	}
}

/*
 * Binds a buffer object to a target, making it when the name is new.
 */
GL_APICALL void GL_APIENTRY
glBindBuffer(
	GLenum target,
	GLuint name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_buffer *buffer;
	struct gles_buffer **slot;

	/* A context with its state, and a target. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	slot = buffer_slot(state, target);
	if (slot == NULL) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The object: none for name 0, made for a name not yet used. */
	buffer = NULL;
	if (name != 0U) {
		buffer = buffer_named(context, state, name);
		if (buffer == NULL)
			return;
	}

	/* Bound. */
	*slot = buffer;
}

/*
 * Gives the buffer bound to a target new bytes (timed, timed_buffer_data).
 */
GL_APICALL void GL_APIENTRY
glBufferData(
	GLenum target,
	GLsizeiptr size,
	const void *data,
	GLenum usage)
{
	uint64_t started;

	/* The call, timed when ZEDBSD_GLES_COMPUTE_TRACE is 2. */
	started = gles_time_begin();
	timed_buffer_data(target, size, data, usage);
	gles_time_end("buffer-data", started, (size_t)size);
}

/*
 * Gives the buffer bound to a target new bytes (or a size of undefined
 * bytes).
 */
static void
timed_buffer_data(
	GLenum target,
	GLsizeiptr size,
	const void *data,
	GLenum usage)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_buffer *buffer;
	unsigned char *bytes;
	int status;

	/* The bound buffer. */
	context = gles_context();
	buffer = buffer_bound(context, target);
	if (buffer == NULL)
		return;
	if (size < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Large bytes are written into a device copy directly (ws101-p017); without one they stay on the CPU. */
	state = gles_state(context);
	if (state != NULL && (size_t)size >= GLES_ON_DEVICE_MIN) {
		status = buffer_data_on_device(state, buffer, (size_t)size, data);
		if (status == 0) {
			buffer->usage = usage;
			return;
		}
	}

	/* The new bytes (a mapping of the old ones ends). */
	bytes = malloc((size_t)size + 1U);
	if (bytes == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/* Filled from the application's bytes when it gave some, else cleared; the extra byte is 0. */
	if (data != NULL) {
		memcpy(bytes, data, (size_t)size);
		bytes[size] = 0U;
	} else {
		memset(bytes, 0, (size_t)size + 1U);
	}

	/* Succeeded: they replace the old ones (what the device wrote too), and the device copy is stale. */
	if (!buffer->on_device)
		free(buffer->data);
	buffer->on_device = 0;
	buffer->data = bytes;
	buffer->size = (size_t)size;
	buffer->usage = usage;
	buffer->dirty = 1;
	buffer->gpu_written = 0;
	buffer->map_active = 0;
	buffer->map_access = 0U;
	buffer->map_offset = 0U;
	buffer->map_length = 0U;
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
	int status;

	/* The bound buffer and a range inside it. */
	context = gles_context();
	buffer = buffer_bound(context, target);
	if (buffer == NULL)
		return;
	if (offset < 0 || size < 0 || (size_t)offset + (size_t)size > buffer->size) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A mapped buffer is written through its mapping only. */
	if (buffer->map_active) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* What the device wrote into it (transform feedback) read back first. */
	status = gles_buffer_fetch(context, buffer);
	if (status != 0)
		return;

	/* Bytes in a device copy this frame reads move to the CPU first (ws101-p017). */
	status = gles_buffer_writable(gles_state(context), buffer);
	if (status != 0) {
		gles_error(context, GL_OUT_OF_MEMORY);
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
 * Reports a parameter of the buffer bound to a target as an integer.
 */
GL_APICALL void GL_APIENTRY
glGetBufferParameteriv(
	GLenum target,
	GLenum pname,
	GLint *params)
{
	struct zegl_context *context;
	GLint64 value;
	int status;

	/* The parameter, which fits in an integer. */
	context = gles_context();
	status = buffer_parameter(context, target, pname, &value);
	if (status != 0)
		return;

	/* Succeeded: the value. */
	*params = (GLint)value;
}

/*
 * Reports a parameter of the buffer bound to a target as a 64-bit
 * integer.
 */
GL_APICALL void GL_APIENTRY
glGetBufferParameteri64v(
	GLenum target,
	GLenum pname,
	GLint64 *params)
{
	struct zegl_context *context;
	GLint64 value;
	int status;

	/* The parameter. */
	context = gles_context();
	status = buffer_parameter(context, target, pname, &value);
	if (status != 0)
		return;

	/* Succeeded: the value. */
	*params = value;
}

/*
 * Maps a range of the buffer bound to a target (timed, timed_map_buffer_range).
 */
GL_APICALL void *GL_APIENTRY
glMapBufferRange(
	GLenum target,
	GLintptr offset,
	GLsizeiptr length,
	GLbitfield access)
{
	uint64_t started;
	void *result;

	/* The call, timed when ZEDBSD_GLES_COMPUTE_TRACE is 2. */
	started = gles_time_begin();
	result = timed_map_buffer_range(target, offset, length, access);
	gles_time_end("map-buffer", started, (size_t)length);
	return result;
}

/*
 * Maps a range of the bytes of the buffer bound to a target: the CPU
 * bytes themselves.  Returns the range's first byte, or NULL with the
 * error recorded.
 */
static void *
timed_map_buffer_range(
	GLenum target,
	GLintptr offset,
	GLsizeiptr length,
	GLbitfield access)
{
	struct zegl_context *context;
	struct gles_buffer *buffer;
	GLbitfield known;
	int status;

	/* The bound buffer. */
	context = gles_context();
	buffer = buffer_bound(context, target);
	if (buffer == NULL)
		return NULL;

	/* A range inside the buffer that is not empty, and only the access bits there are. */
	known = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT |
		GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_UNSYNCHRONIZED_BIT;
	if (offset < 0 ||
	    length <= 0 ||
	    (size_t)offset + (size_t)length > buffer->size ||
	    (access & ~known) != 0U) {
		gles_error(context, GL_INVALID_VALUE);
		return NULL;
	}

	/* A buffer mapped already, or access that neither reads nor writes. */
	if (buffer->map_active || (access & (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT)) == 0U) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Reading does not go with discarding or with leaving the GPU's use unsynchronized. */
	if ((access & GL_MAP_READ_BIT) != 0U &&
	    (access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT)) != 0U) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Explicit flushes are of writes. */
	if ((access & GL_MAP_FLUSH_EXPLICIT_BIT) != 0U && (access & GL_MAP_WRITE_BIT) == 0U) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* What the device wrote into it read back first (bytes in the device copy only wait for the device). */
	status = gles_buffer_fetch(context, buffer);
	if (status != 0)
		return NULL;

	/* A mapping to write bytes in a device copy this frame reads moves them to the CPU first (ws101-p017). */
	if ((access & GL_MAP_WRITE_BIT) != 0U) {
		status = gles_buffer_writable(gles_state(context), buffer);
		if (status != 0) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return NULL;
		}
	}

	/*
	 * The mapping is the CPU bytes: the device copy is made from them
	 * again once the mapping ends (or a range is flushed), and a frame
	 * that already drew from the old copy keeps it (gles_buffer_sync).
	 */
	buffer->map_active = 1;
	buffer->map_access = access;
	buffer->map_offset = (size_t)offset;
	buffer->map_length = (size_t)length;

	/* Succeeded: the range's first byte. */
	return buffer->data + offset;
}

/*
 * Ends the mapping of the buffer bound to a target (timed, timed_unmap_buffer).
 */
GL_APICALL GLboolean GL_APIENTRY
glUnmapBuffer(
	GLenum target)
{
	uint64_t started;
	GLboolean result;

	/* The call, timed when ZEDBSD_GLES_COMPUTE_TRACE is 2. */
	started = gles_time_begin();
	result = timed_unmap_buffer(target);
	gles_time_end("unmap-buffer", started, 0U);
	return result;
}

/*
 * Ends the mapping of the buffer bound to a target.  Returns GL_TRUE (the
 * bytes cannot have been lost), or GL_FALSE with the error recorded when
 * the buffer was not mapped.
 */
static GLboolean
timed_unmap_buffer(
	GLenum target)
{
	struct zegl_context *context;
	struct gles_buffer *buffer;

	/* The bound buffer, which must be mapped. */
	context = gles_context();
	buffer = buffer_bound(context, target);
	if (buffer == NULL)
		return GL_FALSE;
	if (!buffer->map_active) {
		gles_error(context, GL_INVALID_OPERATION);
		return GL_FALSE;
	}

	/* A mapping that wrote without explicit flushes leaves the device copy stale. */
	if ((buffer->map_access & GL_MAP_WRITE_BIT) != 0U && (buffer->map_access & GL_MAP_FLUSH_EXPLICIT_BIT) == 0U)
		buffer->dirty = 1;

	/* The mapping ends. */
	buffer->map_active = 0;
	buffer->map_access = 0U;
	buffer->map_offset = 0U;
	buffer->map_length = 0U;

	/* Succeeded: the bytes are the buffer's. */
	return GL_TRUE;
}

/*
 * Marks a range of a mapping made with GL_MAP_FLUSH_EXPLICIT_BIT as
 * written.
 */
GL_APICALL void GL_APIENTRY
glFlushMappedBufferRange(
	GLenum target,
	GLintptr offset,
	GLsizeiptr length)
{
	struct zegl_context *context;
	struct gles_buffer *buffer;

	/* The bound buffer, mapped for explicit flushes. */
	context = gles_context();
	buffer = buffer_bound(context, target);
	if (buffer == NULL)
		return;
	if (!buffer->map_active || (buffer->map_access & GL_MAP_FLUSH_EXPLICIT_BIT) == 0U) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A range inside the mapping. */
	if (offset < 0 ||
	    length < 0 ||
	    (size_t)offset + (size_t)length > buffer->map_length) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The bytes of the range reach the device with the next draw. */
	buffer->dirty = 1;
}

/*
 * Reports where the buffer bound to a target is mapped (NULL when it is
 * not).
 */
GL_APICALL void GL_APIENTRY
glGetBufferPointerv(
	GLenum target,
	GLenum pname,
	void **params)
{
	struct zegl_context *context;
	struct gles_buffer *buffer;

	/* The bound buffer and the one name. */
	context = gles_context();
	buffer = buffer_bound(context, target);
	if (buffer == NULL)
		return;
	if (pname != GL_BUFFER_MAP_POINTER) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Not mapped: NULL. */
	if (!buffer->map_active) {
		*params = NULL;
		return;
	}

	/* The mapping's first byte. */
	*params = buffer->data + buffer->map_offset;
}

/*
 * Copies a range of the bytes of the buffer bound to one target into the
 * buffer bound to another (or into another place of the same buffer).
 */
GL_APICALL void GL_APIENTRY
glCopyBufferSubData(
	GLenum readTarget,
	GLenum writeTarget,
	GLintptr readOffset,
	GLintptr writeOffset,
	GLsizeiptr size)
{
	struct zegl_context *context;
	struct gles_buffer *source;
	struct gles_buffer *destination;
	size_t from;
	size_t to;
	size_t length;
	int status;

	/* The two bound buffers. */
	context = gles_context();
	source = buffer_bound(context, readTarget);
	if (source == NULL)
		return;
	destination = buffer_bound(context, writeTarget);
	if (destination == NULL)
		return;

	/* Offsets and a size that are not negative. */
	if (readOffset < 0 ||
	    writeOffset < 0 ||
	    size < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Ranges inside both buffers. */
	from = (size_t)readOffset;
	to = (size_t)writeOffset;
	length = (size_t)size;
	if (from + length > source->size || to + length > destination->size) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Neither may be mapped. */
	if (source->map_active || destination->map_active) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Within one buffer the two ranges must not overlap. */
	if (source == destination &&
	    from < to + length &&
	    to < from + length) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* What the device wrote into either (transform feedback) read back first. */
	status = gles_buffer_fetch(context, source);
	if (status == 0)
		status = gles_buffer_fetch(context, destination);
	if (status != 0)
		return;

	/* A destination in a device copy this frame reads moves to the CPU first (ws101-p017). */
	status = gles_buffer_writable(gles_state(context), destination);
	if (status != 0) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/* The bytes are copied on the CPU; the destination's device copy is stale. */
	if (length != 0U)
		memcpy(destination->data + to, source->data + from, length);
	destination->dirty = 1;
}

/*
 * Binds a whole buffer object to an indexed binding point of a target
 * (and to the target itself).
 */
GL_APICALL void GL_APIENTRY
glBindBufferBase(
	GLenum target,
	GLuint index,
	GLuint buffer)
{
	/* The whole buffer, however large it becomes. */
	buffer_bind_range(target, index, buffer, 0, 0, 1);
}

/*
 * Binds a range of a buffer object to an indexed binding point of a
 * target (and the buffer to the target itself).
 */
GL_APICALL void GL_APIENTRY
glBindBufferRange(
	GLenum target,
	GLuint index,
	GLuint buffer,
	GLintptr offset,
	GLsizeiptr size)
{
	/* The range. */
	buffer_bind_range(target, index, buffer, offset, size, 0);
}

/*
 * Reports an indexed binding point's buffer, offset or size as integers.
 */
GL_APICALL void GL_APIENTRY
glGetIntegeri_v(
	GLenum target,
	GLuint index,
	GLint *data)
{
	struct zegl_context *context;
	GLint64 value;
	int status;

	/* The binding point's value, which fits in an integer. */
	context = gles_context();
	status = buffer_indexed(context, target, index, &value);
	if (status != 0)
		return;

	/* Succeeded: the value. */
	*data = (GLint)value;
}

/*
 * Reports an indexed binding point's buffer, offset or size as 64-bit
 * integers.
 */
GL_APICALL void GL_APIENTRY
glGetInteger64i_v(
	GLenum target,
	GLuint index,
	GLint64 *data)
{
	struct zegl_context *context;
	GLint64 value;
	int status;

	/* The binding point's value. */
	context = gles_context();
	status = buffer_indexed(context, target, index, &value);
	if (status != 0)
		return;

	/* Succeeded: the value. */
	*data = value;
}

/*
 * Makes names for vertex array objects (each an object with the initial
 * arrays, which glIsVertexArray reports once it has been bound).
 */
GL_APICALL void GL_APIENTRY
glGenVertexArrays(
	GLsizei n,
	GLuint *arrays)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_vertex_array *array;
	GLsizei index;
	unsigned attrib;
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

	/* Each name gets a vertex array with every array disabled, four floats (current values are the context's). */
	for (index = 0; index < n; index++) {
		array = calloc(1U, sizeof(*array));
		if (array == NULL) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* The arrays' initial state, each attribute on its own binding point. */
		for (attrib = 0U; attrib < GLES_ATTRIBS; attrib++) {
			array->attribs[attrib].size = 4;
			array->attribs[attrib].type = GL_FLOAT;
			array->attribs[attrib].binding = attrib;
		}

		/* The first free name. */
		array->name = gles_names_free(&state->vertex_arrays);
		status = gles_names_add(&state->vertex_arrays, array->name, array);
		if (status != 0) {
			free(array);
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* The name goes back to the application. */
		arrays[index] = array->name;
	}
}

/*
 * Deletes vertex array objects; deleting the bound one binds the default
 * one first.
 */
GL_APICALL void GL_APIENTRY
glDeleteVertexArrays(
	GLsizei n,
	const GLuint *arrays)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_vertex_array *array;
	GLsizei index;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (n < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each name that is a vertex array object. */
	for (index = 0; index < n; index++) {
		if (arrays[index] == 0U)
			continue;
		array = gles_names_get(&state->vertex_arrays, arrays[index]);
		if (array == NULL)
			continue;

		/* The bound one gives way to the default one. */
		if (state->vertex_array == arrays[index])
			glBindVertexArray(0U);

		/* The name and the object go. */
		gles_names_remove(&state->vertex_arrays, arrays[index]);
		free(array);
	}
}

/*
 * Binds a vertex array object (0: the default one): the context's arrays
 * and element buffer are saved into the one bound before, and the new
 * one's become the context's.
 */
GL_APICALL void GL_APIENTRY
glBindVertexArray(
	GLuint name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_vertex_array *array;
	struct gles_vertex_array *previous;
	float values[GLES_ATTRIBS][4];
	GLenum value_types[GLES_ATTRIBS];
	unsigned attrib;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;

	/* The new one: the default one, or an object glGenVertexArrays made. */
	array = &state->default_array;
	if (name != 0U) {
		array = gles_names_get(&state->vertex_arrays, name);
		if (array == NULL) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}
	}

	/* The one bound before, which keeps the context's arrays. */
	previous = &state->default_array;
	if (state->vertex_array != 0U)
		previous = gles_names_get(&state->vertex_arrays, state->vertex_array);

	/* Rebinding the bound one changes nothing. */
	if (previous == array)
		return;

	/* The context's arrays are saved into the one bound before. */
	if (previous != NULL) {
		memcpy(previous->attribs, state->attribs, sizeof(state->attribs));
		previous->element_buffer = state->element_buffer;
	}

	/* The current values are the context's, not a vertex array's: they stay. */
	for (attrib = 0U; attrib < GLES_ATTRIBS; attrib++) {
		memcpy(values[attrib], state->attribs[attrib].value, sizeof(values[attrib]));
		value_types[attrib] = state->attribs[attrib].value_type;
	}

	/*
	 * The new one's arrays become the context's.  Its bound flag makes
	 * glIsVertexArray report it from now on.
	 */
	memcpy(state->attribs, array->attribs, sizeof(state->attribs));
	state->element_buffer = array->element_buffer;
	state->vertex_array = name;
	array->bound = 1;

	/* With the context's current values. */
	for (attrib = 0U; attrib < GLES_ATTRIBS; attrib++) {
		memcpy(state->attribs[attrib].value, values[attrib], sizeof(values[attrib]));
		state->attribs[attrib].value_type = value_types[attrib];
	}
}

/*
 * Reports whether a name is a vertex array object that has been bound.
 */
GL_APICALL GLboolean GL_APIENTRY
glIsVertexArray(
	GLuint name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_vertex_array *array;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return GL_FALSE;

	/* The name's object, once bound. */
	if (name == 0U)
		return GL_FALSE;
	array = gles_names_get(&state->vertex_arrays, name);
	if (array == NULL || !array->bound)
		return GL_FALSE;

	/* A vertex array object. */
	return GL_TRUE;
}

/* Returns where a target's binding is kept, or NULL for a name that is not a target. */
static struct gles_buffer **
buffer_slot(
	struct gles_state *state,
	GLenum target)
{
	/* The targets of OpenGL ES 3. */
	switch (target) {
	case GL_ARRAY_BUFFER:
		return &state->array_buffer;
	case GL_ELEMENT_ARRAY_BUFFER:
		return &state->element_buffer;
	case GL_COPY_READ_BUFFER:
		return &state->copy_read_buffer;
	case GL_COPY_WRITE_BUFFER:
		return &state->copy_write_buffer;
	case GL_UNIFORM_BUFFER:
		return &state->uniform_buffer;
	case GL_PIXEL_PACK_BUFFER:
		return &state->pixel_pack_buffer;
	case GL_PIXEL_UNPACK_BUFFER:
		return &state->pixel_unpack_buffer;
	case GL_TRANSFORM_FEEDBACK_BUFFER:
		return &state->feedback_buffer;
	default:
		break;
	}

	/* Desktop GL's buffer texture target (libGL), and the indirect draws' (desktop GL 4.0, OpenGL ES 3.1). */
	if (target == GL_TEXTURE_BUFFER && gles_fixed != NULL)
		return &state->texture_buffer;
	if (target == GL_DRAW_INDIRECT_BUFFER && (gles_fixed != NULL || state->compute))
		return &state->draw_indirect_buffer;

	/* OpenGL ES 3.1's shader storage and dispatch targets, in a context that offers compute (ws101-p009). */
	if (target == GL_SHADER_STORAGE_BUFFER && state->compute)
		return &state->storage_buffer;
	if (target == GL_DISPATCH_INDIRECT_BUFFER && state->compute)
		return &state->dispatch_buffer;

	/* Not a target. */
	return NULL;
}

/* Returns the buffer bound to a target, recording the error when the target is wrong or nothing is bound. */
static struct gles_buffer *
buffer_bound(
	struct zegl_context *context,
	GLenum target)
{
	struct gles_state *state;
	struct gles_buffer **slot;

	/* A context with its state. */
	state = gles_state(context);
	if (state == NULL)
		return NULL;

	/* The target's binding. */
	slot = buffer_slot(state, target);
	if (slot == NULL) {
		gles_error(context, GL_INVALID_ENUM);
		return NULL;
	}

	/* Nothing bound. */
	if (*slot == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Succeeded: the buffer. */
	return *slot;
}

/* Returns the buffer object of a name (not 0), making it when the name is new; NULL with the error recorded. */
static struct gles_buffer *
buffer_named(
	struct zegl_context *context,
	struct gles_state *state,
	GLuint name)
{
	struct gles_buffer *buffer;
	int status;

	/* One made before. */
	buffer = gles_names_get(&state->buffers, name);
	if (buffer != NULL)
		return buffer;

	/* A new empty buffer. */
	buffer = calloc(1U, sizeof(*buffer));
	if (buffer == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return NULL;
	}

	/* The new buffer takes the name. */
	buffer->name = name;
	buffer->usage = GL_STATIC_DRAW;
	status = gles_names_add(&state->buffers, name, buffer);
	if (status != 0) {
		free(buffer);
		gles_error(context, GL_OUT_OF_MEMORY);
		return NULL;
	}

	/* Succeeded: the new buffer. */
	return buffer;
}

/* Reads a parameter of the buffer bound to a target; nonzero with the error recorded. */
static int
buffer_parameter(
	struct zegl_context *context,
	GLenum target,
	GLenum pname,
	GLint64 *value)
{
	struct gles_buffer *buffer;

	/* The bound buffer. */
	buffer = buffer_bound(context, target);
	if (buffer == NULL)
		return -1;

	/* The parameter asked for. */
	switch (pname) {
	case GL_BUFFER_SIZE:
		*value = (GLint64)buffer->size;
		return 0;
	case GL_BUFFER_USAGE:
		*value = (GLint64)buffer->usage;
		return 0;
	case GL_BUFFER_MAPPED:
		*value = buffer->map_active;
		return 0;
	case GL_BUFFER_ACCESS_FLAGS:
		*value = (GLint64)buffer->map_access;
		return 0;
	case GL_BUFFER_MAP_OFFSET:
		*value = (GLint64)buffer->map_offset;
		return 0;
	case GL_BUFFER_MAP_LENGTH:
		*value = (GLint64)buffer->map_length;
		return 0;
	default:
		break;
	}

	/* Any other is an error. */
	gles_error(context, GL_INVALID_ENUM);
	return -1;
}

/*
 * Binds a buffer (0: none) or a range of it to an indexed binding point
 * of the uniform, transform feedback or (OpenGL ES 3.1's compute) shader
 * storage target, and the buffer to the target; whole binds all of it
 * (glBindBufferBase).
 */
static void
buffer_bind_range(
	GLenum target,
	GLuint index,
	GLuint name,
	GLintptr offset,
	GLsizeiptr size,
	int whole)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_buffer_range *range;
	struct gles_buffer *buffer;
	size_t alignment;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;

	/* The binding point: a uniform buffer's at the device's offset alignment, a transform feedback buffer's at 4 bytes. */
	if (target == GL_UNIFORM_BUFFER) {
		if (index >= GLES_UNIFORM_BINDINGS) {
			gles_error(context, GL_INVALID_VALUE);
			return;
		}

		/* The uniform buffer binding point. */
		range = &state->uniform_ranges[index];
		alignment = (size_t)state->limits.minUniformBufferOffsetAlignment;
	} else if (target == GL_TRANSFORM_FEEDBACK_BUFFER) {
		if (state->feedback->active) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}

		/* A binding point there is. */
		if (index >= GLES_FEEDBACK_BINDINGS) {
			gles_error(context, GL_INVALID_VALUE);
			return;
		}

		/* The transform feedback binding point. */
		range = &state->feedback_ranges[index];
		alignment = 4U;
	} else if (target == GL_SHADER_STORAGE_BUFFER && state->compute) {
		if (index >= GLES_STORAGE_BINDINGS) {
			gles_error(context, GL_INVALID_VALUE);
			return;
		}

		/* The shader storage binding point, at the device's offset alignment (ws101-p009). */
		range = &state->storage_ranges[index];
		alignment = (size_t)state->limits.minStorageBufferOffsetAlignment;
	} else {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* A range of a buffer: at the alignment, not empty. */
	if (alignment == 0U)
		alignment = 1U;
	if (!whole && name != 0U) {
		if (offset < 0 ||
		    size <= 0 ||
		    (size_t)offset % alignment != 0U) {
			gles_error(context, GL_INVALID_VALUE);
			return;
		}

		/* A transform feedback range is whole words. */
		if (target == GL_TRANSFORM_FEEDBACK_BUFFER && (size_t)size % 4U != 0U) {
			gles_error(context, GL_INVALID_VALUE);
			return;
		}
	}

	/* The buffer: none for name 0, made for a name not yet used. */
	buffer = NULL;
	if (name != 0U) {
		buffer = buffer_named(context, state, name);
		if (buffer == NULL)
			return;
	}

	/* Bound to the point (size 0: the whole buffer from the offset) and to the target. */
	range->buffer = buffer;
	range->offset = 0U;
	range->size = 0U;
	if (!whole && buffer != NULL) {
		range->offset = (size_t)offset;
		range->size = (size_t)size;
	}

	/* The target's own binding follows. */
	if (target == GL_UNIFORM_BUFFER) {
		state->uniform_buffer = buffer;
	} else if (target == GL_SHADER_STORAGE_BUFFER) {
		state->storage_buffer = buffer;
	} else {
		state->feedback_buffer = buffer;
	}
}

/*
 * Reads an indexed binding point's buffer name, offset or size, or (in a
 * context that offers compute) a dimension of the largest workgroup count
 * or size, or an image unit's binding; nonzero with the error recorded.
 */
static int
buffer_indexed(
	struct zegl_context *context,
	GLenum target,
	GLuint index,
	GLint64 *value)
{
	struct gles_state *state;
	struct gles_buffer_range *range;
	unsigned count;
	int status;

	/* A context with its state. */
	state = gles_state(context);
	if (state == NULL)
		return -1;

	/* The vertex buffer binding points (desktop GL 4.3, OpenGL ES 3.1). */
	switch (target) {
	case GL_VERTEX_BINDING_BUFFER:
	case GL_VERTEX_BINDING_OFFSET:
	case GL_VERTEX_BINDING_STRIDE:
	case GL_VERTEX_BINDING_DIVISOR:
		if (gles_fixed == NULL && !state->compute)
			break;
		if (index >= GLES_ATTRIBS) {
			gles_error(context, GL_INVALID_VALUE);
			return -1;
		}

		/* The binding point's buffer name, offset, stride or divisor. */
		*value = (GLint64)state->attribs[index].binding_divisor;
		if (target == GL_VERTEX_BINDING_STRIDE)
			*value = (GLint64)gles_vertex_binding_stride(state, index);
		if (target == GL_VERTEX_BINDING_OFFSET)
			*value = (GLint64)state->attribs[index].binding_offset;
		if (target == GL_VERTEX_BINDING_BUFFER) {
			*value = 0;
			if (state->attribs[index].binding_buffer != NULL)
				*value = (GLint64)state->attribs[index].binding_buffer->name;
		}
		return 0;
	default:
		break;
	}

	/* A draw buffer's blend factor or equation (desktop GL 4.0). */
	status = gles_blend_indexed(context, state, target, index, value);
	if (status < 0)
		return -1;
	if (status > 0)
		return 0;

	/* The compute limits of each dimension, the device's (ws101-p009). */
	if (state->compute && (target == GL_MAX_COMPUTE_WORK_GROUP_COUNT || target == GL_MAX_COMPUTE_WORK_GROUP_SIZE)) {
		if (index >= 3U) {
			gles_error(context, GL_INVALID_VALUE);
			return -1;
		}

		/* The count, or the size. */
		*value = (GLint64)state->limits.maxComputeWorkGroupCount[index];
		if (target == GL_MAX_COMPUTE_WORK_GROUP_SIZE)
			*value = (GLint64)state->limits.maxComputeWorkGroupSize[index];
		if (*value > INT32_MAX)
			*value = INT32_MAX;
		return 0;
	}

	/* An image unit's binding (OpenGL ES 3.1). */
	status = gles_image_indexed(context, state, target, index, value);
	if (status < 0)
		return -1;
	if (status > 0)
		return 0;

	/* The binding points of the target the name asks about. */
	switch (target) {
	case GL_UNIFORM_BUFFER_BINDING:
	case GL_UNIFORM_BUFFER_START:
	case GL_UNIFORM_BUFFER_SIZE:
		range = state->uniform_ranges;
		count = GLES_UNIFORM_BINDINGS;
		break;
	case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING:
	case GL_TRANSFORM_FEEDBACK_BUFFER_START:
	case GL_TRANSFORM_FEEDBACK_BUFFER_SIZE:
		range = state->feedback_ranges;
		count = GLES_FEEDBACK_BINDINGS;
		break;
	case GL_SHADER_STORAGE_BUFFER_BINDING:
	case GL_SHADER_STORAGE_BUFFER_START:
	case GL_SHADER_STORAGE_BUFFER_SIZE:
		/* OpenGL ES 3.1's, in a context that offers compute. */
		if (!state->compute) {
			gles_error(context, GL_INVALID_ENUM);
			return -1;
		}

		/* Its binding points. */
		range = state->storage_ranges;
		count = GLES_STORAGE_BINDINGS;
		break;
	default:
		gles_error(context, GL_INVALID_ENUM);
		return -1;
	}

	/* A point there is. */
	if (index >= count) {
		gles_error(context, GL_INVALID_VALUE);
		return -1;
	}

	/* The point. */
	range = &range[index];

	/* The buffer's name, or the range (0 for a whole-buffer binding, as GL reports it). */
	*value = 0;
	if (target == GL_UNIFORM_BUFFER_BINDING || target == GL_TRANSFORM_FEEDBACK_BUFFER_BINDING ||
	    target == GL_SHADER_STORAGE_BUFFER_BINDING) {
		if (range->buffer != NULL)
			*value = (GLint64)range->buffer->name;
	} else if (target == GL_UNIFORM_BUFFER_START || target == GL_TRANSFORM_FEEDBACK_BUFFER_START ||
		   target == GL_SHADER_STORAGE_BUFFER_START) {
		*value = (GLint64)range->offset;
	} else {
		*value = (GLint64)range->size;
	}

	/* Succeeded: the value. */
	return 0;
}

/* Takes a buffer that is being deleted out of a vertex array's arrays and element buffer. */
static void
buffer_forget(
	struct gles_attrib *attribs,
	struct gles_buffer **element_buffer,
	struct gles_buffer *buffer)
{
	unsigned attrib;

	/* The element buffer. */
	if (*element_buffer == buffer)
		*element_buffer = NULL;

	/* Each attribute's array and each binding point's. */
	for (attrib = 0U; attrib < GLES_ATTRIBS; attrib++) {
		if (attribs[attrib].buffer == buffer)
			attribs[attrib].buffer = NULL;
		if (attribs[attrib].binding_buffer == buffer)
			attribs[attrib].binding_buffer = NULL;
	}
}

/*
 * Makes a stream chunk of at least a size, of memory the CPU caches when
 * cached is set (and the device has it), and puts it after the others;
 * NULL when there is no memory.
 */
static struct gles_chunk *
buffer_chunk(
	struct gles_state *state,
	size_t size,
	int cached)
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
	status = buffer_device_typed(state, chunk->size,
				     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
				     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
				     VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				     cached, &chunk->buffer, &chunk->memory, &mapped);
	if (status != 0) {
		free(chunk);
		return NULL;
	}

	/* The chunk's mapping and kind. */
	chunk->mapped = mapped;
	chunk->cached = cached;

	/* Succeeded: the chunk, last in the list. */
	for (last = &state->chunks; *last != NULL; last = &(*last)->next)
		continue;
	*last = chunk;
	return chunk;
}

/*
 * Makes sure the bytes of a buffer object may be written now: bytes that
 * live in its device copy while a frame not yet done reads that copy
 * move to the CPU (the copy is left to the frame, and a new one is made
 * when the frame next uses the buffer).  Returns 0, or -1 when there is no
 * memory.
 */
int
gles_buffer_writable(
	struct gles_state *state,
	struct gles_buffer *buffer)
{
	unsigned char *bytes;

	/* Bytes on the CPU, or a copy no frame still to finish reads, are written in place. */
	if (!buffer->on_device || state == NULL || buffer->used <= state->done)
		return 0;

	/* The bytes on the CPU; the copy keeps them for the frame. */
	bytes = malloc(buffer->size + 1U);
	if (bytes == NULL)
		return -1;
	memcpy(bytes, buffer->data, buffer->size);
	bytes[buffer->size] = 0U;
	buffer->data = bytes;
	buffer->on_device = 0;
	return 0;
}

/*
 * Frees the spare device copies (the state goes).
 */
void
gles_spares_release(
	struct gles_state *state)
{
	/* Each spare, from the last. */
	while (state->spare_count > 0U)
		buffer_spare_free(state, state->spare_count - 1U);
}

/*
 * Gives a buffer object a device copy of a size: a spare of that size, or
 * a new one.  Returns 0, or -1 when the device has no memory.
 */
static int
buffer_device_copy(
	struct gles_state *state,
	size_t size,
	VkBuffer *device_buffer,
	VkDeviceMemory *memory,
	void **mapped)
{
	struct gles_spare *spare;
	unsigned index;
	int status;

	/* A spare of the same size, taken out of the spares. */
	for (index = 0U; index < state->spare_count; index++) {
		spare = &state->spares[index];
		if (spare->size != size)
			continue;
		*device_buffer = spare->buffer;
		*memory = spare->memory;
		*mapped = spare->mapped;
		state->spare_count--;
		state->spares[index] = state->spares[state->spare_count];
		return 0;
	}

	/* A new one, for any use a draw or a dispatch makes of a buffer object: texels, shader storage and dispatch sizes too. */
	status = gles_device_buffer(state, size,
				    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
				    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
				    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
				    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
				    device_buffer, memory, mapped);
	return status;
}

/*
 * Puts a buffer object's device copy aside until the frame being recorded
 * is done, with its mapping and size so that it may become a spare then;
 * the object has no copy afterwards.
 */
static void
buffer_throw_copy_away(
	struct gles_state *state,
	struct gles_buffer *buffer)
{
	struct gles_garbage objects;

	/* The copy, kept as one entry. */
	memset(&objects, 0, sizeof(objects));
	objects.buffer = buffer->buffer;
	objects.memory = buffer->memory;
	objects.mapped = buffer->mapped;
	objects.size = buffer->device_size;
	gles_garbage_keep(state, &objects);

	/* No copy now. */
	buffer->buffer = VK_NULL_HANDLE;
	buffer->memory = VK_NULL_HANDLE;
	buffer->mapped = NULL;
	buffer->device_size = 0U;
}

/*
 * Keeps a freed buffer object's device copy as a spare when there is room (making room from the oldest spares within
 * GLES_SPARE_BYTES).  Returns 1 when it was kept, 0 when it is to be freed.
 */
static int
buffer_spare_keep(
	struct gles_state *state,
	const struct gles_garbage *objects)
{
	struct gles_spare *spare;
	size_t total;
	unsigned index;
	unsigned oldest;

	/* Only a buffer object's copy (a buffer and its mapped memory) of a size the spares hold. */
	if (objects->buffer == VK_NULL_HANDLE || objects->memory == VK_NULL_HANDLE || objects->mapped == NULL)
		return 0;
	if (objects->size == 0U || objects->size > GLES_SPARE_BYTES)
		return 0;

	/* Room: the oldest spares go while there are too many or too many bytes. */
	while (state->spare_count > 0U) {
		total = objects->size;
		oldest = 0U;
		for (index = 0U; index < state->spare_count; index++) {
			total += state->spares[index].size;
			if (state->spares[index].frame < state->spares[oldest].frame)
				oldest = index;
		}

		/* Room enough, or the oldest goes. */
		if (state->spare_count < GLES_SPARES && total <= GLES_SPARE_BYTES)
			break;
		buffer_spare_free(state, oldest);
	}

	/* Succeeded: kept from this frame. */
	spare = &state->spares[state->spare_count];
	spare->buffer = objects->buffer;
	spare->memory = objects->memory;
	spare->mapped = objects->mapped;
	spare->size = objects->size;
	spare->frame = state->frame;
	state->spare_count++;
	return 1;
}

/* Frees a spare device copy and takes it out of the spares. */
static void
buffer_spare_free(
	struct gles_state *state,
	unsigned index)
{
	/* The buffer, then its memory. */
	vkDestroyBuffer(state->device, state->spares[index].buffer, NULL);
	vkFreeMemory(state->device, state->spares[index].memory, NULL);

	/* The last spare takes its place. */
	state->spare_count--;
	state->spares[index] = state->spares[state->spare_count];
}

/*
 * Gives a buffer object new bytes of a size in a device copy (a spare or
 * a new one): the application's bytes copied in, or zeros.  The old copy
 * goes to the frame's garbage.  Returns 0, or -1 when the device has no
 * memory (the object is then as it was).
 */
static int
buffer_data_on_device(
	struct gles_state *state,
	struct gles_buffer *buffer,
	size_t size,
	const void *data)
{
	VkBuffer device_buffer;
	VkDeviceMemory memory;
	void *mapped;
	uint64_t started;
	int status;

	/* The copy. */
	started = gles_time_begin();
	status = buffer_device_copy(state, size, &device_buffer, &memory, &mapped);
	gles_time_end("data-device-buffer", started, size);
	if (status != 0)
		return -1;

	/* The bytes, written once. */
	if (data != NULL) {
		memcpy(mapped, data, size);
	} else {
		memset(mapped, 0, size);
	}

	/* The old bytes and copy go (a frame that read the copy keeps it). */
	if (!buffer->on_device)
		free(buffer->data);
	buffer_throw_copy_away(state, buffer);

	/* Succeeded: the bytes are the copy's, which is up to date. */
	buffer->data = mapped;
	buffer->on_device = 1;
	buffer->buffer = device_buffer;
	buffer->memory = memory;
	buffer->mapped = mapped;
	buffer->device_size = size;
	buffer->size = size;
	buffer->dirty = 0;
	buffer->gpu_written = 0;
	buffer->map_active = 0;
	buffer->map_access = 0U;
	buffer->map_offset = 0U;
	buffer->map_length = 0U;
	return 0;
}
