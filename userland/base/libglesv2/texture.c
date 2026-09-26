/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * 2D textures of zedBSD's OpenGL ES (WS068 p008).
 *
 * glTexImage2D converts what the application gives into RGBA8 and keeps
 * it per level on the CPU; the first draw that samples a changed texture
 * makes a new device image of the levels (a full chain for a mipmapping
 * minification filter, else level 0) and uploads them.  Samplers are made
 * once per set of filters, wraps and level count.
 */

#include "gles.h"

#include <stdlib.h>
#include <string.h>

static struct gles_texture *texture_bound(struct zegl_context *context, GLenum target);
static struct gles_texture *texture_new(GLuint name);
static unsigned char *texture_convert(struct zegl_context *context, GLenum format, GLenum type, GLsizei width, GLsizei height, const void *pixels);
static int texture_parameter(struct zegl_context *context, struct gles_texture *texture, GLenum pname, GLint value);
static int texture_mipmapped(GLenum filter);
static uint32_t texture_levels(struct gles_texture *texture);
static VkFilter texture_filter(GLenum filter);
static VkSamplerAddressMode texture_wrap(GLenum wrap);

/*
 * Brings a texture's device image up to date with its levels.  Returns 0,
 * or -1 when the device has no memory for it.
 */
int
gles_texture_sync(
	struct gles_state *state,
	struct gles_texture *texture)
{
	VkImageCreateInfo create;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkImageViewCreateInfo view;
	VkImageMemoryBarrier barrier;
	VkBufferImageCopy copies[GLES_LEVELS];
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkImage image;
	VkDeviceMemory memory;
	VkImageView image_view;
	unsigned char *mapped;
	void *pointer;
	size_t total;
	size_t bytes;
	uint32_t levels;
	uint32_t level;
	uint32_t type;
	VkResult result;
	int status;

	/* An image that is up to date stays. */
	if (!texture->dirty && texture->image != VK_NULL_HANDLE)
		return 0;

	/* The levels the image has, and the bytes they take. */
	levels = texture_levels(texture);
	total = 0U;
	for (level = 0U; level < levels; level++)
		total += (size_t)texture->levels[level].width * (size_t)texture->levels[level].height * 4U;

	/* The image. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = VK_FORMAT_R8G8B8A8_UNORM;
	create.extent.width = (uint32_t)texture->levels[0].width;
	create.extent.height = (uint32_t)texture->levels[0].height;
	create.extent.depth = 1U;
	create.mipLevels = levels;
	create.arrayLayers = 1U;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_OPTIMAL;
	create.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	result = vkCreateImage(state->device, &create, NULL, &image);
	if (result != VK_SUCCESS)
		return -1;

	/* Its memory on the device. */
	vkGetImageMemoryRequirements(state->device, image, &requirements);
	type = gles_memory_type(state, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type == UINT32_MAX)
		type = gles_memory_type(state, requirements.memoryTypeBits, 0U);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	result = vkAllocateMemory(state->device, &allocate, NULL, &memory);
	if (result != VK_SUCCESS) {
		vkDestroyImage(state->device, image, NULL);
		return -1;
	}

	/* Bound, with a view of every level. */
	result = vkBindImageMemory(state->device, image, memory, 0U);
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_R8G8B8A8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = levels;
	view.subresourceRange.layerCount = 1U;
	image_view = VK_NULL_HANDLE;
	if (result == VK_SUCCESS)
		result = vkCreateImageView(state->device, &view, NULL, &image_view);
	if (result != VK_SUCCESS) {
		vkDestroyImage(state->device, image, NULL);
		vkFreeMemory(state->device, memory, NULL);
		return -1;
	}

	/* The staging buffer with the levels one after another. */
	status = gles_device_buffer(state, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &staging_memory, &pointer);
	if (status != 0) {
		gles_throw_away(state, VK_NULL_HANDLE, image, image_view, memory);
		return -1;
	}

	/* The levels one after another. */
	mapped = pointer;
	total = 0U;
	memset(copies, 0, sizeof(copies));
	for (level = 0U; level < levels; level++) {
		bytes = (size_t)texture->levels[level].width * (size_t)texture->levels[level].height * 4U;
		memcpy(mapped + total, texture->levels[level].pixels, bytes);
		copies[level].bufferOffset = total;
		copies[level].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copies[level].imageSubresource.mipLevel = level;
		copies[level].imageSubresource.layerCount = 1U;
		copies[level].imageExtent.width = (uint32_t)texture->levels[level].width;
		copies[level].imageExtent.height = (uint32_t)texture->levels[level].height;
		copies[level].imageExtent.depth = 1U;
		total += bytes;
	}

	/* The upload: ready to be written, the copies, ready to be sampled. */
	status = gles_upload_begin(state);
	if (status == 0) {
		memset(&barrier, 0, sizeof(barrier));
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = levels;
		barrier.subresourceRange.layerCount = 1U;
		vkCmdPipelineBarrier(state->upload, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
		vkCmdCopyBufferToImage(state->upload, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, copies);
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier(state->upload, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
		status = gles_upload_end(state);
	}

	/* The staging buffer is done with (the upload was waited for). */
	vkDestroyBuffer(state->device, staging, NULL);
	vkFreeMemory(state->device, staging_memory, NULL);
	if (status != 0) {
		gles_throw_away(state, VK_NULL_HANDLE, image, image_view, memory);
		return -1;
	}

	/* Succeeded: the new image replaces the old one, which waits for the frame. */
	gles_throw_away(state, VK_NULL_HANDLE, texture->image, texture->view, texture->memory);
	texture->image = image;
	texture->memory = memory;
	texture->view = image_view;
	texture->level_count = levels;
	texture->dirty = 0;
	return 0;
}

/*
 * Reports whether a texture can be sampled: its level 0 is specified.
 */
int
gles_texture_complete(
	struct gles_texture *texture)
{
	/* Level 0 decides. */
	if (texture == NULL || texture->levels[0].width <= 0 || texture->levels[0].height <= 0)
		return 0;
	return 1;
}

/*
 * Returns the sampler for a texture's filters, wraps and level count,
 * making it the first time; VK_NULL_HANDLE when it cannot be made.
 */
VkSampler
gles_sampler_get(
	struct gles_state *state,
	struct gles_texture *texture)
{
	VkSamplerCreateInfo create;
	struct gles_sampler *sampler;
	VkResult result;
	int mipmapped;

	/* One already made for the same state. */
	for (sampler = state->samplers; sampler != NULL; sampler = sampler->next) {
		if (sampler->min_filter == texture->min_filter && sampler->mag_filter == texture->mag_filter &&
		    sampler->wrap_s == texture->wrap_s && sampler->wrap_t == texture->wrap_t &&
		    sampler->levels == texture->level_count)
			return sampler->sampler;
	}

	/* A new one: the filters, the mipmap mode and the wraps. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	create.magFilter = texture_filter(texture->mag_filter);
	create.minFilter = texture_filter(texture->min_filter);
	create.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	if (texture->min_filter == GL_NEAREST_MIPMAP_LINEAR || texture->min_filter == GL_LINEAR_MIPMAP_LINEAR)
		create.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	create.addressModeU = texture_wrap(texture->wrap_s);
	create.addressModeV = texture_wrap(texture->wrap_t);
	create.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	create.maxAnisotropy = 1.0f;
	create.compareOp = VK_COMPARE_OP_NEVER;

	/* A filter without mipmaps samples level 0 only (the 0.25 keeps the magnification test right). */
	create.maxLod = 0.25f;
	mipmapped = texture_mipmapped(texture->min_filter);
	if (mipmapped)
		create.maxLod = (float)texture->level_count;

	/* Made and kept. */
	sampler = calloc(1U, sizeof(*sampler));
	if (sampler == NULL)
		return VK_NULL_HANDLE;
	result = vkCreateSampler(state->device, &create, NULL, &sampler->sampler);
	if (result != VK_SUCCESS) {
		free(sampler);
		return VK_NULL_HANDLE;
	}

	/* Succeeded: the sampler, kept for the next texture like it. */
	sampler->min_filter = texture->min_filter;
	sampler->mag_filter = texture->mag_filter;
	sampler->wrap_s = texture->wrap_s;
	sampler->wrap_t = texture->wrap_t;
	sampler->levels = texture->level_count;
	sampler->next = state->samplers;
	state->samplers = sampler;
	return sampler->sampler;
}

/*
 * Returns the black texture sampled where a unit has no complete texture,
 * making it at its first use; NULL when there is no memory.
 */
struct gles_texture *
gles_texture_black(
	struct gles_state *state)
{
	struct gles_texture *texture;
	unsigned char *pixels;

	/* Made once. */
	if (state->black != NULL)
		return state->black;

	/* One opaque black texel. */
	texture = texture_new(0U);
	if (texture == NULL)
		return NULL;
	pixels = calloc(4U, 1U);
	if (pixels == NULL) {
		free(texture);
		return NULL;
	}

	/* Succeeded: the texture. */
	pixels[3] = 255U;
	gles_texture_define(texture, 0, 1, 1, pixels);
	state->black = texture;
	return texture;
}

/*
 * Frees a texture; its device image waits for the frame.
 */
void
gles_texture_free(
	struct gles_state *state,
	struct gles_texture *texture)
{
	unsigned level;

	/* The device image, then the levels and the object. */
	gles_throw_away(state, VK_NULL_HANDLE, texture->image, texture->view, texture->memory);
	for (level = 0U; level < GLES_LEVELS; level++)
		free(texture->levels[level].pixels);
	free(texture);
}

/*
 * Gives a texture's level new RGBA8 pixels, which the texture takes over.
 */
void
gles_texture_define(
	struct gles_texture *texture,
	GLint level,
	int width,
	int height,
	unsigned char *pixels)
{
	/* The old pixels go; the image is stale. */
	free(texture->levels[level].pixels);
	texture->levels[level].pixels = pixels;
	texture->levels[level].width = width;
	texture->levels[level].height = height;
	texture->dirty = 1;
}

/*
 * Makes names for textures.
 */
GL_APICALL void GL_APIENTRY
glGenTextures(
	GLsizei n,
	GLuint *textures)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_texture *texture;
	GLsizei index;
	GLuint name;
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

	/* Each name gets a texture with no levels. */
	for (index = 0; index < n; index++) {
		name = gles_names_free(&state->textures);
		texture = texture_new(name);
		if (texture == NULL) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* The name. */
		status = gles_names_add(&state->textures, name, texture);
		if (status != 0) {
			free(texture);
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* The name goes back to the application. */
		textures[index] = name;
	}
}

/*
 * Deletes textures, unbinding them from every unit.
 */
GL_APICALL void GL_APIENTRY
glDeleteTextures(
	GLsizei n,
	const GLuint *textures)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_texture *texture;
	GLsizei index;
	unsigned unit;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (n < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each name that is a texture. */
	for (index = 0; index < n; index++) {
		texture = gles_names_get(&state->textures, textures[index]);
		if (texture == NULL)
			continue;

		/* Unbound from every unit. */
		for (unit = 0U; unit < GLES_UNITS; unit++) {
			if (state->units[unit] == texture)
				state->units[unit] = NULL;
		}

		/* The name and the texture go. */
		gles_names_remove(&state->textures, textures[index]);
		gles_texture_free(state, texture);
	}
}

/*
 * Binds a texture to the active unit's GL_TEXTURE_2D, making it when the
 * name is new.
 */
GL_APICALL void GL_APIENTRY
glBindTexture(
	GLenum target,
	GLuint name)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_texture *texture;
	int status;

	/* A context with its state, and the 2D target (cube maps are not there yet). */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (target != GL_TEXTURE_2D) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The texture: none for name 0, made for a name not yet used. */
	texture = NULL;
	if (name != 0U) {
		texture = gles_names_get(&state->textures, name);
		if (texture == NULL) {
			texture = texture_new(name);
			if (texture == NULL) {
				gles_error(context, GL_OUT_OF_MEMORY);
				return;
			}

			/* Under the name. */
			status = gles_names_add(&state->textures, name, texture);
			if (status != 0) {
				free(texture);
				gles_error(context, GL_OUT_OF_MEMORY);
				return;
			}
		}
	}

	/* Bound to the active unit. */
	state->units[state->active_unit] = texture;
}

/*
 * Selects the texture unit glBindTexture and glTexImage2D work on.
 */
GL_APICALL void GL_APIENTRY
glActiveTexture(
	GLenum texture)
{
	struct zegl_context *context;
	struct gles_state *state;

	/* A context with its state, and a unit it has. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (texture < GL_TEXTURE0 || texture >= GL_TEXTURE0 + GLES_UNITS) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The unit. */
	state->active_unit = texture - GL_TEXTURE0;
}

/*
 * Specifies a level of the bound texture.
 */
GL_APICALL void GL_APIENTRY
glTexImage2D(
	GLenum target,
	GLint level,
	GLint internalformat,
	GLsizei width,
	GLsizei height,
	GLint border,
	GLenum format,
	GLenum type,
	const void *pixels)
{
	struct zegl_context *context;
	struct gles_texture *texture;
	unsigned char *converted;

	/* The bound texture, a level and a size it can have. */
	context = gles_context();
	texture = texture_bound(context, target);
	if (texture == NULL)
		return;
	if (level < 0 || level >= (GLint)GLES_LEVELS || width < 0 || height < 0 || width > 16384 || height > 16384 || border != 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* OpenGL ES 2 wants the internal format to be the format. */
	(void)internalformat;

	/* The pixels as RGBA8 (NULL pixels: black). */
	converted = texture_convert(context, format, type, width, height, pixels);
	if (converted == NULL)
		return;

	/* Succeeded: the level. */
	gles_texture_define(texture, level, width, height, converted);
}

/*
 * Replaces a rectangle of a level of the bound texture.
 */
GL_APICALL void GL_APIENTRY
glTexSubImage2D(
	GLenum target,
	GLint level,
	GLint xoffset,
	GLint yoffset,
	GLsizei width,
	GLsizei height,
	GLenum format,
	GLenum type,
	const void *pixels)
{
	struct zegl_context *context;
	struct gles_texture *texture;
	struct gles_level *destination;
	unsigned char *converted;
	GLsizei row;

	/* The bound texture and a rectangle inside a specified level. */
	context = gles_context();
	texture = texture_bound(context, target);
	if (texture == NULL)
		return;
	if (level < 0 || level >= (GLint)GLES_LEVELS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The level. */
	destination = &texture->levels[level];
	if (xoffset < 0 || yoffset < 0 || width < 0 || height < 0 ||
	    xoffset + width > destination->width || yoffset + height > destination->height) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The pixels as RGBA8. */
	converted = texture_convert(context, format, type, width, height, pixels);
	if (converted == NULL)
		return;

	/* Each row into place. */
	for (row = 0; row < height; row++) {
		memcpy(destination->pixels + ((size_t)(yoffset + row) * (size_t)destination->width + (size_t)xoffset) * 4U,
		       converted + (size_t)row * (size_t)width * 4U, (size_t)width * 4U);
	}

	/* Succeeded: the image is stale. */
	free(converted);
	texture->dirty = 1;
}

/*
 * Makes a level of the bound texture from a rectangle of the framebuffer.
 */
GL_APICALL void GL_APIENTRY
glCopyTexImage2D(
	GLenum target,
	GLint level,
	GLenum internalformat,
	GLint x,
	GLint y,
	GLsizei width,
	GLsizei height,
	GLint border)
{
	struct zegl_context *context;
	struct gles_texture *texture;
	unsigned char *pixels;
	int status;

	/* The bound texture, a level and a size. */
	context = gles_context();
	texture = texture_bound(context, target);
	if (texture == NULL)
		return;
	if (level < 0 || level >= (GLint)GLES_LEVELS || width <= 0 || height <= 0 || border != 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The format is RGBA8 whatever was asked. */
	(void)internalformat;

	/* The framebuffer's pixels. */
	pixels = malloc((size_t)width * (size_t)height * 4U);
	if (pixels == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/* The framebuffer's pixels. */
	status = gles_read_rgba(context, x, y, width, height, pixels);
	if (status != 0) {
		free(pixels);
		return;
	}

	/* Succeeded: the level. */
	gles_texture_define(texture, level, width, height, pixels);
}

/*
 * Replaces a rectangle of a level of the bound texture with one of the
 * framebuffer.
 */
GL_APICALL void GL_APIENTRY
glCopyTexSubImage2D(
	GLenum target,
	GLint level,
	GLint xoffset,
	GLint yoffset,
	GLint x,
	GLint y,
	GLsizei width,
	GLsizei height)
{
	struct zegl_context *context;
	struct gles_texture *texture;
	struct gles_level *destination;
	unsigned char *pixels;
	GLsizei row;
	int status;

	/* The bound texture and a rectangle inside a specified level. */
	context = gles_context();
	texture = texture_bound(context, target);
	if (texture == NULL)
		return;
	if (level < 0 || level >= (GLint)GLES_LEVELS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The level. */
	destination = &texture->levels[level];
	if (xoffset < 0 || yoffset < 0 || width <= 0 || height <= 0 ||
	    xoffset + width > destination->width || yoffset + height > destination->height) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The framebuffer's pixels. */
	pixels = malloc((size_t)width * (size_t)height * 4U);
	if (pixels == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return;
	}

	/* The framebuffer's pixels. */
	status = gles_read_rgba(context, x, y, width, height, pixels);
	if (status != 0) {
		free(pixels);
		return;
	}

	/* Each row into place. */
	for (row = 0; row < height; row++) {
		memcpy(destination->pixels + ((size_t)(yoffset + row) * (size_t)destination->width + (size_t)xoffset) * 4U,
		       pixels + (size_t)row * (size_t)width * 4U, (size_t)width * 4U);
	}

	/* Succeeded: the image is stale. */
	free(pixels);
	texture->dirty = 1;
}

/*
 * Refuses compressed textures: no compressed format is offered.
 */
GL_APICALL void GL_APIENTRY
glCompressedTexImage2D(
	GLenum target,
	GLint level,
	GLenum internalformat,
	GLsizei width,
	GLsizei height,
	GLint border,
	GLsizei imageSize,
	const void *data)
{
	struct zegl_context *context;

	/* GL_NUM_COMPRESSED_TEXTURE_FORMATS is 0. */
	(void)target;
	(void)level;
	(void)internalformat;
	(void)width;
	(void)height;
	(void)border;
	(void)imageSize;
	(void)data;
	context = gles_context();
	if (context != NULL)
		gles_error(context, GL_INVALID_ENUM);
}

/*
 * Refuses compressed textures: no compressed format is offered.
 */
GL_APICALL void GL_APIENTRY
glCompressedTexSubImage2D(
	GLenum target,
	GLint level,
	GLint xoffset,
	GLint yoffset,
	GLsizei width,
	GLsizei height,
	GLenum format,
	GLsizei imageSize,
	const void *data)
{
	struct zegl_context *context;

	/* GL_NUM_COMPRESSED_TEXTURE_FORMATS is 0. */
	(void)target;
	(void)level;
	(void)xoffset;
	(void)yoffset;
	(void)width;
	(void)height;
	(void)format;
	(void)imageSize;
	(void)data;
	context = gles_context();
	if (context != NULL)
		gles_error(context, GL_INVALID_ENUM);
}

/*
 * Sets an integer parameter of the bound texture.
 */
GL_APICALL void GL_APIENTRY
glTexParameteri(
	GLenum target,
	GLenum pname,
	GLint param)
{
	struct zegl_context *context;
	struct gles_texture *texture;

	/* The bound texture takes it. */
	context = gles_context();
	texture = texture_bound(context, target);
	if (texture == NULL)
		return;
	(void)texture_parameter(context, texture, pname, param);
}

/*
 * Sets a parameter of the bound texture from a float.
 */
GL_APICALL void GL_APIENTRY
glTexParameterf(
	GLenum target,
	GLenum pname,
	GLfloat param)
{
	/* Every parameter of a 2D texture in OpenGL ES 2 is an enum. */
	glTexParameteri(target, pname, (GLint)param);
}

/*
 * Sets an integer parameter of the bound texture from an array.
 */
GL_APICALL void GL_APIENTRY
glTexParameteriv(
	GLenum target,
	GLenum pname,
	const GLint *params)
{
	/* The first value. */
	glTexParameteri(target, pname, params[0]);
}

/*
 * Sets a parameter of the bound texture from an array of floats.
 */
GL_APICALL void GL_APIENTRY
glTexParameterfv(
	GLenum target,
	GLenum pname,
	const GLfloat *params)
{
	/* The first value. */
	glTexParameteri(target, pname, (GLint)params[0]);
}

/*
 * Reports a parameter of the bound texture.
 */
GL_APICALL void GL_APIENTRY
glGetTexParameteriv(
	GLenum target,
	GLenum pname,
	GLint *params)
{
	struct zegl_context *context;
	struct gles_texture *texture;

	/* The bound texture. */
	context = gles_context();
	texture = texture_bound(context, target);
	if (texture == NULL)
		return;

	/* The parameter asked for. */
	switch (pname) {
	case GL_TEXTURE_MIN_FILTER:
		*params = (GLint)texture->min_filter;
		return;
	case GL_TEXTURE_MAG_FILTER:
		*params = (GLint)texture->mag_filter;
		return;
	case GL_TEXTURE_WRAP_S:
		*params = (GLint)texture->wrap_s;
		return;
	case GL_TEXTURE_WRAP_T:
		*params = (GLint)texture->wrap_t;
		return;
	default:
		break;
	}

	/* Any other is an error. */
	gles_error(context, GL_INVALID_ENUM);
}

/*
 * Reports a parameter of the bound texture as a float.
 */
GL_APICALL void GL_APIENTRY
glGetTexParameterfv(
	GLenum target,
	GLenum pname,
	GLfloat *params)
{
	GLint value;

	/* The integer, converted. */
	value = 0;
	glGetTexParameteriv(target, pname, &value);
	*params = (GLfloat)value;
}

/*
 * Reports whether a name is a texture.
 */
GL_APICALL GLboolean GL_APIENTRY
glIsTexture(
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
	object = gles_names_get(&state->textures, name);
	if (object == NULL)
		return GL_FALSE;
	return GL_TRUE;
}

/*
 * Makes every level of the bound texture below level 0 by halving it with
 * a box filter.
 */
GL_APICALL void GL_APIENTRY
glGenerateMipmap(
	GLenum target)
{
	struct zegl_context *context;
	struct gles_texture *texture;
	struct gles_level *source;
	unsigned char *pixels;
	unsigned sum;
	int width;
	int height;
	int x;
	int y;
	int dx;
	int dy;
	int sx;
	int sy;
	unsigned channel;
	GLint level;
	int complete;

	/* The bound texture with a level 0. */
	context = gles_context();
	texture = texture_bound(context, target);
	if (texture == NULL)
		return;
	complete = gles_texture_complete(texture);
	if (!complete) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Each level from the one above it, until 1x1. */
	for (level = 1; level < (GLint)GLES_LEVELS; level++) {
		source = &texture->levels[level - 1];
		if (source->width == 1 && source->height == 1)
			break;
		width = source->width / 2;
		if (width < 1)
			width = 1;
		height = source->height / 2;
		if (height < 1)
			height = 1;
		pixels = malloc((size_t)width * (size_t)height * 4U);
		if (pixels == NULL) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* Each texel: the mean of the (up to) four above it. */
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++) {
				for (channel = 0U; channel < 4U; channel++) {
					sum = 0U;
					for (dy = 0; dy < 2; dy++) {
						for (dx = 0; dx < 2; dx++) {
							sx = x * 2 + dx;
							sy = y * 2 + dy;
							if (sx >= source->width)
								sx = source->width - 1;
							if (sy >= source->height)
								sy = source->height - 1;
							sum += source->pixels[((size_t)sy * (size_t)source->width + (size_t)sx) * 4U + channel];
						}
					}

					/* The mean, rounded. */
					pixels[((size_t)y * (size_t)width + (size_t)x) * 4U + channel] = (unsigned char)((sum + 2U) / 4U);
				}
			}
		}

		/* The level. */
		gles_texture_define(texture, level, width, height, pixels);
	}
}

/* Returns the texture bound to the active unit, recording the error when the target is wrong or nothing is bound. */
static struct gles_texture *
texture_bound(
	struct zegl_context *context,
	GLenum target)
{
	struct gles_state *state;

	/* A context with its state and the 2D target. */
	state = gles_state(context);
	if (state == NULL)
		return NULL;
	if (target != GL_TEXTURE_2D) {
		gles_error(context, GL_INVALID_ENUM);
		return NULL;
	}

	/* The active unit's texture (name 0 is not a texture that can be changed). */
	if (state->units[state->active_unit] == NULL)
		gles_error(context, GL_INVALID_OPERATION);
	return state->units[state->active_unit];
}

/* Makes a texture with GL's initial sampling state and no levels; NULL when there is no memory. */
static struct gles_texture *
texture_new(
	GLuint name)
{
	struct gles_texture *texture;

	/* The object. */
	texture = calloc(1U, sizeof(*texture));
	if (texture == NULL)
		return NULL;

	/* GL's initial state. */
	texture->name = name;
	texture->target = GL_TEXTURE_2D;
	texture->min_filter = GL_NEAREST_MIPMAP_LINEAR;
	texture->mag_filter = GL_LINEAR;
	texture->wrap_s = GL_REPEAT;
	texture->wrap_t = GL_REPEAT;
	return texture;
}

/*
 * Converts the application's pixels to RGBA8 rows, honouring the unpack
 * alignment; NULL pixels give black.  Returns the new rows, or NULL with
 * the error recorded.
 */
static unsigned char *
texture_convert(
	struct zegl_context *context,
	GLenum format,
	GLenum type,
	GLsizei width,
	GLsizei height,
	const void *pixels)
{
	struct gles_state *state;
	const unsigned char *source;
	const unsigned char *row_start;
	unsigned char *converted;
	unsigned char *out;
	unsigned value;
	size_t texel;
	size_t stride;
	size_t alignment;
	GLsizei x;
	GLsizei y;

	/* The bytes a source texel takes, by format and type. */
	state = gles_state(context);
	texel = 0U;
	if (type == GL_UNSIGNED_BYTE) {
		if (format == GL_RGBA || format == GL_BGRA_EXT)
			texel = 4U;
		if (format == GL_RGB)
			texel = 3U;
		if (format == GL_LUMINANCE_ALPHA)
			texel = 2U;
		if (format == GL_LUMINANCE || format == GL_ALPHA)
			texel = 1U;
	} else if (type == GL_UNSIGNED_SHORT_5_6_5 && format == GL_RGB) {
		texel = 2U;
	} else if ((type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) && format == GL_RGBA) {
		texel = 2U;
	}

	/* A combination not offered. */
	if (texel == 0U) {
		gles_error(context, GL_INVALID_ENUM);
		return NULL;
	}

	/* The rows. */
	converted = calloc((size_t)width * (size_t)height + 1U, 4U);
	if (converted == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return NULL;
	}

	/* Without pixels the level is black. */
	if (pixels == NULL)
		return converted;

	/* The source rows are aligned to the unpack alignment. */
	alignment = (size_t)state->unpack_alignment;
	stride = ((size_t)width * texel + alignment - 1U) / alignment * alignment;

	/* Each texel into RGBA. */
	for (y = 0; y < height; y++) {
		row_start = (const unsigned char *)pixels + (size_t)y * stride;
		out = converted + (size_t)y * (size_t)width * 4U;
		for (x = 0; x < width; x++) {
			source = row_start + (size_t)x * texel;
			value = 0U;
			if (texel == 2U && type != GL_UNSIGNED_BYTE)
				value = (unsigned)source[0] | ((unsigned)source[1] << 8);

			/* The channels of the format. */
			switch (format) {
			case GL_RGBA:
				if (type == GL_UNSIGNED_BYTE) {
					memcpy(out, source, 4U);
				} else if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
					out[0] = (unsigned char)(((value >> 12) & 15U) * 17U);
					out[1] = (unsigned char)(((value >> 8) & 15U) * 17U);
					out[2] = (unsigned char)(((value >> 4) & 15U) * 17U);
					out[3] = (unsigned char)((value & 15U) * 17U);
				} else {
					out[0] = (unsigned char)(((value >> 11) & 31U) * 255U / 31U);
					out[1] = (unsigned char)(((value >> 6) & 31U) * 255U / 31U);
					out[2] = (unsigned char)(((value >> 1) & 31U) * 255U / 31U);
					out[3] = (unsigned char)((value & 1U) * 255U);
				}

				break;
			case GL_BGRA_EXT:
				out[0] = source[2];
				out[1] = source[1];
				out[2] = source[0];
				out[3] = source[3];
				break;
			case GL_RGB:
				if (type == GL_UNSIGNED_BYTE) {
					memcpy(out, source, 3U);
				} else {
					out[0] = (unsigned char)(((value >> 11) & 31U) * 255U / 31U);
					out[1] = (unsigned char)(((value >> 5) & 63U) * 255U / 63U);
					out[2] = (unsigned char)((value & 31U) * 255U / 31U);
				}

				/* Opaque. */
				out[3] = 255U;
				break;
			case GL_LUMINANCE_ALPHA:
				out[0] = source[0];
				out[1] = source[0];
				out[2] = source[0];
				out[3] = source[1];
				break;
			case GL_LUMINANCE:
				out[0] = source[0];
				out[1] = source[0];
				out[2] = source[0];
				out[3] = 255U;
				break;
			default:
				out[3] = source[0];
				break;
			}

			/* The next texel. */
			out += 4;
		}
	}

	/* Succeeded: the RGBA rows. */
	return converted;
}

/* Sets one sampling parameter; nonzero with the error recorded when the parameter or its value is not one. */
static int
texture_parameter(
	struct zegl_context *context,
	struct gles_texture *texture,
	GLenum pname,
	GLint value)
{
	GLenum mode;
	int mipmapped;

	/* The value as an enum. */
	mode = (GLenum)value;

	/* The parameter. */
	switch (pname) {
	case GL_TEXTURE_MIN_FILTER:
		mipmapped = texture_mipmapped(mode);
		if (mode != GL_NEAREST && mode != GL_LINEAR && !mipmapped)
			break;
		if (texture->min_filter != mode)
			texture->dirty = 1;
		texture->min_filter = mode;
		return 0;
	case GL_TEXTURE_MAG_FILTER:
		if (mode != GL_NEAREST && mode != GL_LINEAR)
			break;
		texture->mag_filter = mode;
		return 0;
	case GL_TEXTURE_WRAP_S:
	case GL_TEXTURE_WRAP_T:
		if (mode != GL_REPEAT && mode != GL_CLAMP_TO_EDGE && mode != GL_MIRRORED_REPEAT)
			break;
		if (pname == GL_TEXTURE_WRAP_S)
			texture->wrap_s = mode;
		if (pname == GL_TEXTURE_WRAP_T)
			texture->wrap_t = mode;
		return 0;
	default:
		gles_error(context, GL_INVALID_ENUM);
		return -1;
	}

	/* A value the parameter does not take. */
	gles_error(context, GL_INVALID_ENUM);
	return -1;
}

/* Reports whether a minification filter samples mipmaps. */
static int
texture_mipmapped(
	GLenum filter)
{
	/* The four mipmap filters. */
	switch (filter) {
	case GL_NEAREST_MIPMAP_NEAREST:
	case GL_LINEAR_MIPMAP_NEAREST:
	case GL_NEAREST_MIPMAP_LINEAR:
	case GL_LINEAR_MIPMAP_LINEAR:
		return 1;
	default:
		break;
	}

	/* The other two. */
	return 0;
}

/* Returns how many levels a texture's image has: the chain from level 0 of halving sizes when it mipmaps, else 1. */
static uint32_t
texture_levels(
	struct gles_texture *texture)
{
	uint32_t levels;
	int width;
	int height;
	int mipmapped;

	/* Level 0 only, unless the filter mipmaps. */
	mipmapped = texture_mipmapped(texture->min_filter);
	if (!mipmapped)
		return 1U;

	/* Each next level whose size is the halved one. */
	width = texture->levels[0].width;
	height = texture->levels[0].height;
	for (levels = 1U; levels < GLES_LEVELS; levels++) {
		if (width == 1 && height == 1)
			break;
		width = width / 2;
		if (width < 1)
			width = 1;
		height = height / 2;
		if (height < 1)
			height = 1;
		if (texture->levels[levels].width != width || texture->levels[levels].height != height)
			break;
	}

	/* The chain found. */
	return levels;
}

/* Returns Vulkan's filter for a GL filter. */
static VkFilter
texture_filter(
	GLenum filter)
{
	/* The nearest filters, then the linear ones. */
	switch (filter) {
	case GL_NEAREST:
	case GL_NEAREST_MIPMAP_NEAREST:
	case GL_NEAREST_MIPMAP_LINEAR:
		return VK_FILTER_NEAREST;
	default:
		break;
	}

	/* The rest are linear. */
	return VK_FILTER_LINEAR;
}

/* Returns Vulkan's address mode for a GL wrap mode. */
static VkSamplerAddressMode
texture_wrap(
	GLenum wrap)
{
	/* The three wraps of OpenGL ES 2. */
	switch (wrap) {
	case GL_CLAMP_TO_EDGE:
		return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	case GL_MIRRORED_REPEAT:
		return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	default:
		break;
	}

	/* GL_REPEAT. */
	return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}
