/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The textures of zedBSD's OpenGL ES (WS068 p008, p023, p025, p028): 2D
 * textures, cube maps, 3D textures and 2D array textures, and sampler
 * objects.
 *
 * Each level keeps its texels on the CPU in the kept form of its format
 * (format.c; OpenGL ES 2's unsized formats as RGBA8, which
 * texture_convert makes), a 3D texture's slices and a 2D array's layers
 * one after another; the first draw that samples a changed texture makes
 * a new device image of the levels from the base level (a chain for a
 * mipmapping minification filter, else one level) and uploads them.  The
 * application's texels are found by the pixel store and the pixel unpack
 * buffer (pixels.c).  Samplers are made once per set of sampling state
 * and level count; a sampler object bound to a unit replaces its
 * textures' own sampling state.
 *
 * Desktop GL (libGL, WS068 p031) adds rectangle textures (a 2D texture of
 * one level under its own target) and buffer textures: a buffer object's
 * bytes read as texels of a format through a view of its device copy
 * (a Vulkan uniform texel buffer).
 */

#include "gles.h"

#include <stdlib.h>
#include <string.h>

/* Desktop GL's 16-bit normalized formats, which a buffer texture may have (OpenGL ES has none). */
#ifndef GL_R16
#define GL_R16			0x822A
#define GL_RG16			0x822C
#define GL_RGBA16		0x805B
#endif

/* The largest level of detail GL starts with (MAX_LOD and MAX_LEVEL's initial values). */
#define TEXTURE_LOD_MAX		1000.0f
#define TEXTURE_LEVEL_MAX	1000

/*
 * The targets a call takes, as bits: GL_TEXTURE_2D, a cube map's faces
 * (the calls that give a face's level texels), GL_TEXTURE_CUBE_MAP as a
 * whole, GL_TEXTURE_3D, GL_TEXTURE_2D_ARRAY, desktop GL's
 * GL_TEXTURE_RECTANGLE (libGL), and GL_TEXTURE_2D_MULTISAMPLE (desktop
 * GL 3.2, OpenGL ES 3.1).
 */
#define TEXTURE_TAKES_2D	1U
#define TEXTURE_TAKES_FACES	2U
#define TEXTURE_TAKES_CUBE	4U
#define TEXTURE_TAKES_3D	8U
#define TEXTURE_TAKES_ARRAY	16U
#define TEXTURE_TAKES_RECT	32U
#define TEXTURE_TAKES_MS	64U

/*
 * The targets of the calls that give a 2D image, a 3D image, fixed 2D
 * levels, of the calls on a texture's parameters, and of mipmaps (not a
 * rectangle texture's: it has one level).
 */
#define TEXTURE_IMAGE_2D	(TEXTURE_TAKES_2D | TEXTURE_TAKES_FACES | TEXTURE_TAKES_RECT)
#define TEXTURE_IMAGE_3D	(TEXTURE_TAKES_3D | TEXTURE_TAKES_ARRAY)
#define TEXTURE_STORAGE_2D	(TEXTURE_TAKES_2D | TEXTURE_TAKES_CUBE | TEXTURE_TAKES_RECT)
#define TEXTURE_WHOLE		(TEXTURE_TAKES_2D | TEXTURE_TAKES_CUBE | TEXTURE_TAKES_3D | TEXTURE_TAKES_ARRAY | TEXTURE_TAKES_RECT)
#define TEXTURE_MIPMAPS		(TEXTURE_TAKES_2D | TEXTURE_TAKES_CUBE | TEXTURE_TAKES_3D | TEXTURE_TAKES_ARRAY)

/*
 * The targets of the calls on a texture's parameters (a multisample
 * texture's too), and of the queries of one level of a texture (a face,
 * not a whole cube map).
 */
#define TEXTURE_PARAMETERS	(TEXTURE_WHOLE | TEXTURE_TAKES_MS)
#define TEXTURE_LEVEL_QUERY	(TEXTURE_TAKES_2D | TEXTURE_TAKES_FACES | TEXTURE_TAKES_3D | TEXTURE_TAKES_ARRAY | TEXTURE_TAKES_RECT | TEXTURE_TAKES_MS)

/* Desktop GL's level parameters OpenGL ES does not have (libGL). */
#ifndef GL_TEXTURE_LUMINANCE_SIZE
#define GL_TEXTURE_LUMINANCE_SIZE	0x8060
#endif
#ifndef GL_TEXTURE_LUMINANCE_TYPE
#define GL_TEXTURE_LUMINANCE_TYPE	0x8C14
#endif
#ifndef GL_TEXTURE_COMPRESSED_IMAGE_SIZE
#define GL_TEXTURE_COMPRESSED_IMAGE_SIZE	0x86A0
#endif

/* The channels texture_level_bits reports, in the order of its arrays. */
#define TEXTURE_BITS_RED	0U
#define TEXTURE_BITS_GREEN	1U
#define TEXTURE_BITS_BLUE	2U
#define TEXTURE_BITS_ALPHA	3U
#define TEXTURE_BITS_DEPTH	4U
#define TEXTURE_BITS_STENCIL	5U
#define TEXTURE_BITS_SHARED	6U
#define TEXTURE_BITS_LUMINANCE	7U
#define TEXTURE_BITS		8U

static struct gles_texture *texture_bound(struct zegl_context *context, GLenum target, unsigned takes, unsigned *face);
static struct gles_texture *texture_changing(struct zegl_context *context, GLenum target, unsigned takes, unsigned *face);
static unsigned char *texture_compressed(struct zegl_context *context, const struct gles_format *format, GLsizei width, GLsizei height, GLsizei depth, GLsizei imageSize, const void *data);
static int texture_compressed_rectangle(const struct gles_level *level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth);
static struct gles_texture *texture_new(GLuint name, GLenum target);
static void texture_level_set(struct gles_texture *texture, unsigned face, GLint level, int width, int height, int depth, unsigned char *pixels, const struct gles_format *format);
static void texture_sampling_initial(struct gles_sampling *sampling);
static unsigned texture_shape(const struct gles_texture *texture);
static void texture_check(struct zegl_context *context, struct gles_texture *texture, unsigned face, GLint level);
static uint32_t texture_faces(const struct gles_texture *texture);
static int texture_mipmaps(struct gles_texture *texture, unsigned face, GLint last);
static int texture_mipmaps_kept(struct gles_texture *texture, unsigned face, GLint last);
static void texture_views_free(struct gles_state *state, struct gles_texture *texture);
static int texture_buffer_format(GLenum internal, VkFormat *format, unsigned *bytes, unsigned *kind);
static VkBufferView texture_black_buffer(struct gles_state *state, unsigned kind);
static int texture_clear_samples(struct gles_state *state, VkImage image, VkImageAspectFlags aspects, const struct gles_format *format);
static void texture_multisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations, int immutable);
static void texture_opaque(const struct gles_format *format, VkComponentMapping *components);
static unsigned char *texture_convert(struct zegl_context *context, GLenum format, GLenum type, GLsizei width, GLsizei height, const void *pixels);
static unsigned char *texture_texels(struct zegl_context *context, const struct gles_format *storage, GLenum format, GLenum type, GLsizei width, GLsizei height, GLsizei depth, const void *pixels);
static unsigned char *texture_from_framebuffer(struct zegl_context *context, const struct gles_format *storage, GLint x, GLint y, GLsizei width, GLsizei height);
static void texture_rows_place(struct gles_level *destination, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, const unsigned char *rows);
static void texture_split_stencil(unsigned char *out, const unsigned char *kept, size_t texels);
static int texture_volume_size(struct gles_state *state, const struct gles_texture *texture, GLsizei width, GLsizei height, GLsizei depth);
static int texture_parameter(struct zegl_context *context, struct gles_texture *texture, GLenum pname, GLint value, GLfloat number);
static int texture_sampling_parameter(struct zegl_context *context, struct gles_sampling *sampling, GLenum pname, GLint value, GLfloat number);
static int texture_get(const struct gles_state *state, struct gles_texture *texture, GLenum pname, GLfloat *value);
static int texture_level_get(struct zegl_context *context, GLenum target, GLint level, GLenum pname, GLint *value);
static void texture_level_bits(const struct gles_format *format, GLint *bits, GLenum *types);
static void texture_compressed_bits(GLenum internal, GLint *bits, GLenum *types);
static int texture_sampling_get(const struct gles_sampling *sampling, GLenum pname, GLfloat *value);
static struct gles_sampler_object *texture_sampler_object(struct zegl_context *context, GLuint name);
static int texture_mipmapped(GLenum filter);
static int texture_nearest(const struct gles_sampling *sampling);
static uint32_t texture_levels(struct gles_texture *texture);
static VkFilter texture_filter(GLenum filter);
static VkSamplerAddressMode texture_wrap(GLenum wrap);
static VkComponentSwizzle texture_swizzle(GLenum source, VkComponentSwizzle identity);

/*
 * Brings a texture's device image up to date with its levels from the
 * base level, in the Vulkan format the levels are kept in: a 2D image (a
 * cube map's with a layer per face, a 2D array's with a layer per layer)
 * or a 3D image.  Returns 0, or -1 when the device does not make such an
 * image or has no memory for it.
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
	VkBufferImageCopy copies[GLES_FACES * GLES_LEVELS * 2U];
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkImage image;
	VkDeviceMemory memory;
	VkImageView image_view;
	VkImageAspectFlags copy_aspect;
	VkImageAspectFlags whole_aspect;
	const struct gles_format *format;
	const struct gles_level *source;
	unsigned char *mapped;
	void *pointer;
	size_t total;
	size_t bytes;
	uint32_t features;
	uint32_t levels;
	uint32_t faces;
	uint32_t layers;
	uint32_t level;
	uint32_t face;
	uint32_t count;
	uint32_t type;
	uint32_t base;
	unsigned shape;
	VkResult result;
	int storage;
	int sampled;
	int unit_format;
	int mutable_format;
	int alias;
	int status;

	/* An image that is up to date stays. */
	if (!texture->dirty && texture->image != VK_NULL_HANDLE)
		return 0;

	/* The levels and faces the image has, their format, and the bytes they take (every slice of every level). */
	base = (uint32_t)texture->base_level;
	levels = texture_levels(texture);
	faces = texture_faces(texture);
	shape = texture_shape(texture);
	format = texture->levels[base].format;
	if (format == NULL)
		return -1;
	total = 0U;
	for (face = 0U; face < faces; face++) {
		for (level = 0U; level < levels; level++) {
			source = &texture->levels[face * GLES_LEVELS + base + level];
			total += (size_t)source->width * (size_t)source->height * (size_t)source->depth * format->bytes;
		}
	}

	/* The image's layers: a cube map's faces, a 2D array's layers (a cube map array's layer-faces), one otherwise. */
	layers = faces;
	if (shape == GLES_SHAPE_ARRAY || shape == GLES_SHAPE_CUBE_ARRAY)
		layers = (uint32_t)texture->levels[base].depth;

	/* A depth format is copied into its depth aspect; a barrier names every aspect it has. */
	copy_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	whole_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	if (format->kind == GLES_TEXEL_DEPTH) {
		copy_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
		whole_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
		if (format->stencil)
			whole_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}

	/* The image (a cube map's can be viewed as one, a 3D texture's is 3D). */
	features = gles_image_features(state, format->vk);
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	if (shape == GLES_SHAPE_CUBE || shape == GLES_SHAPE_CUBE_ARRAY)
		create.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = format->vk;
	create.extent.width = (uint32_t)texture->levels[base].width;
	create.extent.height = (uint32_t)texture->levels[base].height;
	create.extent.depth = 1U;
	if (shape == GLES_SHAPE_3D) {
		create.imageType = VK_IMAGE_TYPE_3D;
		create.extent.depth = (uint32_t)texture->levels[base].depth;
	}

	/*
	 * Its levels and layers, sampled and copied; a format an image unit
	 * takes that the device stores but does not sample is only stored
	 * while the context has image units (draws read it as black).
	 */
	create.mipLevels = levels;
	create.arrayLayers = layers;
	create.samples = (VkSampleCountFlagBits)texture->samples;
	create.tiling = VK_IMAGE_TILING_OPTIMAL;
	create.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	sampled = 1;
	unit_format = gles_image_unit_format(format->vk);
	if (state->image_units != 0U &&
	    texture->samples <= 1U &&
	    (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0U &&
	    (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0U &&
	    unit_format) {
		create.usage &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_SAMPLED_BIT;
		sampled = 0;
	}
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	/* A 2D texture, a cube map or a 2D array is drawn into when its format can be (a 3D texture's slices cannot be viewed as 2D images). */
	if (shape != GLES_SHAPE_3D) {
		if (format->kind != GLES_TEXEL_DEPTH && (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0U)
			create.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		if (format->kind == GLES_TEXEL_DEPTH && (features & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U)
			create.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	}

	/*
	 * A single-sampled image of a format an image unit takes, which the
	 * device stores, is a storage image too while the context has image
	 * units (as long as the device makes it so).
	 */
	storage = 0;
	mutable_format = 0;
	if (state->image_units != 0U &&
	    texture->samples <= 1U &&
	    (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0U &&
	    unit_format) {
		create.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
		storage = 1;

		/* A unit of another format of its size may view it (OpenGL ES 3.1's compatibility by size): mutable. */
		alias = gles_image_unit_alias(state, format->internal);
		if (alias) {
			create.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
			mutable_format = 1;
		}

		/* Mutable as long as the device makes it so, and a storage image as long as the device makes that. */
		status = gles_image_supported(state, &create);
		if (!status && mutable_format) {
			create.flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
			mutable_format = 0;
			status = gles_image_supported(state, &create);
		}
		if (!status) {
			create.usage &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_STORAGE_BIT;
			storage = 0;
		}
	}
	if (!sampled && !storage)
		return -1;

	/* An image the device does not make is not asked for: its specification failed with an error, and the caller reads it as black. */
	status = gles_image_supported(state, &create);
	if (!status)
		return -1;

	/* Made. */
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

	/* Bound, with a view of every level in the texture's shape whose channels follow the swizzle. */
	result = vkBindImageMemory(state->device, image, memory, 0U);
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	if (shape == GLES_SHAPE_CUBE)
		view.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
	if (shape == GLES_SHAPE_3D)
		view.viewType = VK_IMAGE_VIEW_TYPE_3D;
	if (shape == GLES_SHAPE_ARRAY)
		view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	if (shape == GLES_SHAPE_CUBE_ARRAY)
		view.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
	view.format = format->vk;
	view.components.r = texture_swizzle(texture->swizzle[0], VK_COMPONENT_SWIZZLE_R);
	view.components.g = texture_swizzle(texture->swizzle[1], VK_COMPONENT_SWIZZLE_G);
	view.components.b = texture_swizzle(texture->swizzle[2], VK_COMPONENT_SWIZZLE_B);
	view.components.a = texture_swizzle(texture->swizzle[3], VK_COMPONENT_SWIZZLE_A);
	texture_opaque(format, &view.components);
	view.subresourceRange.aspectMask = copy_aspect;
	view.subresourceRange.levelCount = levels;
	view.subresourceRange.layerCount = layers;
	image_view = VK_NULL_HANDLE;
	if (result == VK_SUCCESS)
		result = vkCreateImageView(state->device, &view, NULL, &image_view);
	if (result != VK_SUCCESS) {
		vkDestroyImage(state->device, image, NULL);
		vkFreeMemory(state->device, memory, NULL);
		return -1;
	}

	/* A multisample texture has no texels on the CPU: its image is cleared to zero instead. */
	if (texture->samples > 1U) {
		status = texture_clear_samples(state, image, whole_aspect, format);
		if (status != 0) {
			gles_throw_away(state, VK_NULL_HANDLE, image, image_view, memory);
			return -1;
		}

		/* The new image replaces the old one. */
		gles_throw_away(state, VK_NULL_HANDLE, texture->image, texture->view, texture->memory);
		texture_views_free(state, texture);
		texture->image = image;
		texture->memory = memory;
		texture->view = image_view;
		texture->level_count = 1U;
		texture->image_format = format;
		texture->storage = storage;
		texture->sampled = sampled;
		texture->mutable_format = mutable_format;
		texture->image_usage = create.usage;
		texture->dirty = 0;
		return 0;
	}

	/* The staging buffer with the levels one after another. */
	status = gles_device_buffer(state, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &staging_memory, &pointer);
	if (status != 0) {
		gles_throw_away(state, VK_NULL_HANDLE, image, image_view, memory);
		return -1;
	}

	/*
	 * Each face's levels one after another (the image's level 0 is the
	 * base level): a level's slices are a 3D image's depth, a 2D array's
	 * its layers.
	 */
	mapped = pointer;
	total = 0U;
	count = 0U;
	memset(copies, 0, sizeof(copies));
	for (face = 0U; face < faces; face++) {
		for (level = 0U; level < levels; level++) {
			source = &texture->levels[face * GLES_LEVELS + base + level];
			bytes = (size_t)source->width * (size_t)source->height * (size_t)source->depth * format->bytes;

			/* A level not specified yet (a cube map's later face) has no texels to copy: a region of no extent is invalid. */
			if (bytes == 0U || source->pixels == NULL)
				continue;

			/* The texels as the copy reads them (a depth and stencil format's two aspects one after the other). */
			if (format->stencil) {
				texture_split_stencil(mapped + total, source->pixels, bytes / format->bytes);
			} else {
				memcpy(mapped + total, source->pixels, bytes);
			}
			copies[count].bufferOffset = total;
			copies[count].imageSubresource.aspectMask = copy_aspect;
			copies[count].imageSubresource.mipLevel = level;
			copies[count].imageSubresource.baseArrayLayer = face;
			copies[count].imageSubresource.layerCount = 1U;
			copies[count].imageExtent.width = (uint32_t)source->width;
			copies[count].imageExtent.height = (uint32_t)source->height;
			copies[count].imageExtent.depth = 1U;
			if (shape == GLES_SHAPE_3D)
				copies[count].imageExtent.depth = (uint32_t)source->depth;
			if (shape == GLES_SHAPE_ARRAY || shape == GLES_SHAPE_CUBE_ARRAY)
				copies[count].imageSubresource.layerCount = (uint32_t)source->depth;
			count++;

			/* A depth and stencil format's stencil aspect from after the depths (4 bytes a texel). */
			if (format->stencil) {
				copies[count] = copies[count - 1U];
				copies[count].bufferOffset = total + bytes / format->bytes * 4U;
				copies[count].imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
				count++;
			}
			total += bytes;
		}
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
		barrier.subresourceRange.aspectMask = whole_aspect;
		barrier.subresourceRange.levelCount = levels;
		barrier.subresourceRange.layerCount = layers;
		vkCmdPipelineBarrier(state->upload, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
		vkCmdCopyBufferToImage(state->upload, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count, copies);
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

	/* The new image replaces the old one, which waits for the frame with the views of it framebuffer objects drew into. */
	gles_throw_away(state, VK_NULL_HANDLE, texture->image, texture->view, texture->memory);
	texture_views_free(state, texture);
	texture->image = image;
	texture->memory = memory;
	texture->view = image_view;
	texture->level_count = levels;
	texture->image_format = format;
	texture->storage = storage;
	texture->sampled = sampled;
	texture->mutable_format = mutable_format;
	texture->image_usage = create.usage;
	texture->dirty = 0;

	/* Succeeded: the image has the levels. */
	return 0;
}

/*
 * Reports whether a texture can be sampled with a sampling state (NULL:
 * its own): its base level is specified (a cube map's on every face,
 * square, all the same size and format), and a format that cannot be
 * filtered (integers, 32-bit floats, depth without comparison) is read
 * with nearest filters only.
 */
int
gles_texture_complete(
	struct gles_texture *texture,
	const struct gles_sampling *sampling)
{
	const struct gles_level *level;
	const struct gles_level *first;
	const struct gles_format *format;
	unsigned faces;
	unsigned face;
	int nearest;

	/* A texture, bound to a target at least once, whose base level is not above its most. */
	if (texture == NULL || texture->target == 0U)
		return 0;
	if (texture->base_level > texture->max_level)
		return 0;

	/* Its own sampling unless a sampler object's is given. */
	if (sampling == NULL)
		sampling = &texture->sampling;

	/* The base level of each face, as large as the first face's and of its format. */
	faces = texture_faces(texture);
	first = &texture->levels[texture->base_level];
	for (face = 0U; face < faces; face++) {
		level = &texture->levels[face * GLES_LEVELS + (unsigned)texture->base_level];
		if (level->width <= 0 || level->height <= 0 || level->format == NULL)
			return 0;
		if (level->width != first->width || level->height != first->height)
			return 0;
		if (level->format != first->format)
			return 0;
	}

	/* A cube map's faces are square. */
	if (faces > 1U && first->width != first->height)
		return 0;

	/* A format a linear filter cannot read, read with one. */
	format = first->format;
	nearest = texture_nearest(sampling);
	if (!nearest && (format->kind == GLES_TEXEL_INT || format->kind == GLES_TEXEL_UINT))
		return 0;
	if (!nearest && format->kind != GLES_TEXEL_DEPTH && !format->filterable)
		return 0;
	if (!nearest && format->kind == GLES_TEXEL_DEPTH && sampling->compare_mode == GL_NONE)
		return 0;

	/* A stencil index is read with nearest filters only (OpenGL ES 3.1 section 8.16). */
	if (!nearest && format->stencil && texture->stencil_mode == GL_STENCIL_INDEX)
		return 0;

	/* Succeeded: it can be sampled. */
	return 1;
}

/*
 * Reports whether samplers read a texture's stencil indices: a depth and
 * stencil format's texture whose GL_DEPTH_STENCIL_TEXTURE_MODE is
 * GL_STENCIL_INDEX (as unsigned integers, with nearest filters and no
 * comparison).
 */
int
gles_texture_reads_stencil(
	const struct gles_texture *texture)
{
	/* A depth and stencil format's image. */
	if (texture->image_format == NULL || !texture->image_format->stencil)
		return 0;

	/* Succeeded: whether it reads the stencil index. */
	if (texture->stencil_mode == GL_STENCIL_INDEX)
		return 1;
	return 0;
}

/*
 * Returns the view of a depth and stencil texture's image that reads its
 * stencil aspect (every level and layer the image has, the texture's
 * swizzle), made at its first use; VK_NULL_HANDLE when it cannot be made.
 */
VkImageView
gles_texture_stencil_view(
	struct gles_state *state,
	struct gles_texture *texture)
{
	VkImageViewCreateInfo create;
	VkResult result;
	unsigned shape;
	uint32_t layers;

	/* One made before, or no image. */
	if (texture->stencil_view != VK_NULL_HANDLE || texture->image == VK_NULL_HANDLE)
		return texture->stencil_view;

	/* The image's shape and layers, as its own view has them. */
	shape = texture_shape(texture);
	layers = texture_faces(texture);
	if (shape == GLES_SHAPE_ARRAY || shape == GLES_SHAPE_CUBE_ARRAY)
		layers = (uint32_t)texture->levels[texture->base_level].depth;

	/* The stencil aspect, swizzled as the texture is. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create.image = texture->image;
	create.viewType = VK_IMAGE_VIEW_TYPE_2D;
	if (shape == GLES_SHAPE_CUBE)
		create.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
	if (shape == GLES_SHAPE_ARRAY)
		create.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	if (shape == GLES_SHAPE_CUBE_ARRAY)
		create.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
	create.format = texture->image_format->vk;
	create.components.r = texture_swizzle(texture->swizzle[0], VK_COMPONENT_SWIZZLE_R);
	create.components.g = texture_swizzle(texture->swizzle[1], VK_COMPONENT_SWIZZLE_G);
	create.components.b = texture_swizzle(texture->swizzle[2], VK_COMPONENT_SWIZZLE_B);
	create.components.a = texture_swizzle(texture->swizzle[3], VK_COMPONENT_SWIZZLE_A);
	create.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
	create.subresourceRange.levelCount = texture->level_count;
	create.subresourceRange.layerCount = layers;
	result = vkCreateImageView(state->device, &create, NULL, &texture->stencil_view);
	if (result != VK_SUCCESS) {
		texture->stencil_view = VK_NULL_HANDLE;
		return VK_NULL_HANDLE;
	}

	/* Succeeded: the view. */
	return texture->stencil_view;
}

/*
 * Returns the sampler for a sampling state (NULL: the texture's own) and
 * the texture's level count, making it the first time; VK_NULL_HANDLE
 * when it cannot be made.
 */
VkSampler
gles_sampler_get(
	struct gles_state *state,
	struct gles_texture *texture,
	const struct gles_sampling *sampling)
{
	VkSamplerCreateInfo create;
	struct gles_sampler *sampler;
	VkResult result;
	float levels;
	int mipmapped;
	int differs;

	/* Its own sampling unless a sampler object's is given. */
	if (sampling == NULL)
		sampling = &texture->sampling;

	/* One already made for the same state and levels. */
	for (sampler = state->samplers; sampler != NULL; sampler = sampler->next) {
		differs = memcmp(&sampler->sampling, sampling, sizeof(*sampling));
		if (differs == 0 && sampler->levels == texture->level_count)
			return sampler->sampler;
	}

	/* A new one: the filters, the mipmap mode and the wraps. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	create.magFilter = texture_filter(sampling->mag_filter);
	create.minFilter = texture_filter(sampling->min_filter);
	create.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	if (sampling->min_filter == GL_NEAREST_MIPMAP_LINEAR || sampling->min_filter == GL_LINEAR_MIPMAP_LINEAR)
		create.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	create.addressModeU = texture_wrap(sampling->wrap_s);
	create.addressModeV = texture_wrap(sampling->wrap_t);
	create.addressModeW = texture_wrap(sampling->wrap_r);
	create.maxAnisotropy = 1.0f;

	/* The depth comparison (GL's compare functions are Vulkan's in the same order). */
	create.compareOp = VK_COMPARE_OP_NEVER;
	if (sampling->compare_mode == GL_COMPARE_REF_TO_TEXTURE) {
		create.compareEnable = VK_TRUE;
		create.compareOp = (VkCompareOp)(sampling->compare_func - GL_NEVER);
	}

	/* A filter without mipmaps samples the first level only (the 0.25 keeps the magnification test right). */
	create.maxLod = 0.25f;
	mipmapped = texture_mipmapped(sampling->min_filter);
	if (mipmapped) {
		levels = (float)texture->level_count;
		create.minLod = sampling->min_lod;
		if (create.minLod < 0.0f)
			create.minLod = 0.0f;
		if (create.minLod > levels)
			create.minLod = levels;
		create.maxLod = sampling->max_lod;
		if (create.maxLod > levels)
			create.maxLod = levels;
		if (create.maxLod < create.minLod)
			create.maxLod = create.minLod;
	}

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
	sampler->sampling = *sampling;
	sampler->levels = texture->level_count;
	sampler->next = state->samplers;
	state->samplers = sampler;
	return sampler->sampler;
}

/*
 * Returns the black texture of a shape (GLES_SHAPE_*) and a kind (0
 * float, 1 int, 2 unsigned, 3 depth) sampled where a unit has no complete
 * texture of the kind its sampler reads, making it at its first use; NULL
 * when there is no memory or no format.
 */
struct gles_texture *
gles_texture_black(
	struct gles_state *state,
	unsigned shape,
	unsigned kind)
{
	static const GLenum internals[GLES_BLACK_KINDS] = { GL_RGBA, GL_RGBA8I, GL_RGBA8UI, GL_DEPTH_COMPONENT16 };
	static const GLenum types[GLES_BLACK_KINDS] = { GL_UNSIGNED_BYTE, GL_BYTE, GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT };
	static const GLenum targets[GLES_SHAPES] = { GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_3D, GL_TEXTURE_2D_ARRAY };
	const struct gles_format *format;
	struct gles_texture *texture;
	struct gles_texture **kept;
	unsigned char *pixels;
	unsigned faces;
	unsigned face;

	/* Made once per shape and kind (a 3D texture has no depth one: no sampler compares it); a cube map array's six layer-faces. */
	if (kind >= GLES_BLACK_KINDS)
		kind = 0U;
	if (shape == GLES_SHAPE_CUBE_ARRAY) {
		texture = gles_texture_black_cube_array(state, kind, internals[kind], types[kind]);
		return texture;
	}
	if (shape >= GLES_SHAPES)
		shape = GLES_SHAPE_2D;
	if (shape == GLES_SHAPE_3D && kind == 3U)
		kind = 0U;
	kept = &state->blacks[shape][kind];

	/* Already made. */
	if (*kept != NULL)
		return *kept;

	/* The kind's format. */
	format = gles_format_find(state, internals[kind], types[kind]);
	if (format == NULL)
		return NULL;

	/* The texture, read with nearest filters (integers and depth cannot be filtered). */
	texture = texture_new(0U, targets[shape]);
	if (texture == NULL)
		return NULL;
	texture->sampling.min_filter = GL_NEAREST;
	texture->sampling.mag_filter = GL_NEAREST;

	/* One black texel on each face, or in the one slice or layer (opaque: alpha 255, or 1 for the integers). */
	faces = texture_faces(texture);
	for (face = 0U; face < faces; face++) {
		pixels = calloc(format->bytes, 1U);
		if (pixels == NULL) {
			gles_texture_free(state, texture);
			return NULL;
		}

		/* Opaque. */
		if (kind == 0U)
			pixels[3] = 255U;
		if (kind == 1U || kind == 2U)
			pixels[3] = 1U;
		texture_level_set(texture, face, 0, 1, 1, 1, pixels, format);
	}

	/* Succeeded: the texture, kept. */
	*kept = texture;
	return texture;
}

/*
 * Returns the view of a buffer texture's buffer that a draw reads as texels
 * of a kind (0 float, 1 int, 2 unsigned), the buffer's device copy brought
 * up to date and marked as read by this frame; a view of four zero words
 * when the unit has no buffer texture, its buffer is empty or gone, its
 * texels are of another kind, or the device cannot read its format.
 * VK_NULL_HANDLE when even that cannot be made.
 */
VkBufferView
gles_texture_buffer_view(
	struct gles_state *state,
	struct gles_texture *texture,
	unsigned kind)
{
	VkBufferViewCreateInfo create;
	struct gles_garbage objects;
	struct gles_buffer *buffer;
	VkDeviceSize range;
	VkDeviceSize largest;
	VkBufferView black;
	VkResult result;
	int status;

	/* A buffer texture with a buffer of texels of the kind the sampler reads, in a format the device reads. */
	black = texture_black_buffer(state, kind);
	if (texture == NULL || texture->texel_buffer == NULL)
		return black;
	if (texture->texel_kind != kind || texture->texel_vk == VK_FORMAT_UNDEFINED)
		return black;

	/* The buffer's device copy, up to date, read by this frame. */
	buffer = texture->texel_buffer;
	status = gles_buffer_sync(state, buffer);
	if (status != 0 || buffer->buffer == VK_NULL_HANDLE)
		return black;
	buffer->used = state->frame;

	/* Whole texels, at most as many as the device reads. */
	range = (VkDeviceSize)buffer->size / texture->texel_bytes * texture->texel_bytes;
	largest = (VkDeviceSize)state->limits.maxTexelBufferElements * texture->texel_bytes;
	if (range > largest)
		range = largest;
	if (range == 0U)
		return black;

	/* The view made for this device buffer and range stays. */
	if (texture->texel_view != VK_NULL_HANDLE &&
	    texture->texel_view_buffer == buffer->buffer &&
	    texture->texel_view_range == range)
		return texture->texel_view;

	/* The old view waits for the frame. */
	if (texture->texel_view != VK_NULL_HANDLE) {
		memset(&objects, 0, sizeof(objects));
		objects.buffer_view = texture->texel_view;
		gles_garbage_keep(state, &objects);
		texture->texel_view = VK_NULL_HANDLE;
	}

	/* A view of the buffer's texels. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
	create.buffer = buffer->buffer;
	create.format = texture->texel_vk;
	create.offset = 0U;
	create.range = range;
	result = vkCreateBufferView(state->device, &create, NULL, &texture->texel_view);
	if (result != VK_SUCCESS) {
		texture->texel_view = VK_NULL_HANDLE;
		return black;
	}

	/* Succeeded: the view, kept with what it was made for. */
	texture->texel_view_buffer = buffer->buffer;
	texture->texel_view_range = range;
	return texture->texel_view;
}

/*
 * Reports whether a display's device reads buffer textures (desktop GL
 * 3.1, GL_ARB_texture_buffer_object): the four-component 32-bit float,
 * integer and unsigned formats as uniform texel buffers, which every
 * sampler of a unit without a buffer texture reads (texture_black_buffer);
 * with rgb32, whether it reads the three-component 32-bit ones too
 * (GL_ARB_texture_buffer_object_rgb32).
 */
int
gles_texel_buffers(
	const struct zegl_display *display,
	int rgb32)
{
	static const VkFormat four[3] = {
		VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_UINT
	};
	static const VkFormat three[3] = {
		VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32_UINT
	};
	VkFormatProperties properties;
	const VkFormat *formats;
	unsigned index;

	/* The formats asked about. */
	formats = four;
	if (rgb32)
		formats = three;

	/* Each one, read from a buffer by the sampler. */
	for (index = 0U; index < 3U; index++) {
		memset(&properties, 0, sizeof(properties));
		vkGetPhysicalDeviceFormatProperties(display->physical, formats[index], &properties);
		if ((properties.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) == 0U)
			return 0;
	}

	/* Succeeded: the device reads them. */
	return 1;
}

/*
 * Returns the cube map array texture of a kind (0 float, 1 int, 2
 * unsigned, 3 depth) sampled where a unit has no complete one: one cube
 * of black 1x1 faces (opaque) in an internal format and type, made at its
 * first use; NULL when there is no memory or no format.
 */
struct gles_texture *
gles_texture_black_cube_array(
	struct gles_state *state,
	unsigned kind,
	GLenum internal,
	GLenum type)
{
	const struct gles_format *format;
	struct gles_texture *texture;
	unsigned char *pixels;
	unsigned char alpha;
	unsigned face;

	/* Made once per kind. */
	if (state->black_cube_arrays[kind] != NULL)
		return state->black_cube_arrays[kind];

	/* The kind's format. */
	format = gles_format_find(state, internal, type);
	if (format == NULL)
		return NULL;

	/* The texture, read with nearest filters (integers and depth cannot be filtered). */
	texture = texture_new(0U, GL_TEXTURE_CUBE_MAP_ARRAY);
	if (texture == NULL)
		return NULL;
	texture->sampling.min_filter = GL_NEAREST;
	texture->sampling.mag_filter = GL_NEAREST;

	/* Six black layer-faces (opaque: alpha 255, or 1 for the integers). */
	pixels = calloc(GLES_FACES, format->bytes);
	if (pixels == NULL) {
		gles_texture_free(state, texture);
		return NULL;
	}
	alpha = 0U;
	if (kind == 0U)
		alpha = 255U;
	if (kind == 1U || kind == 2U)
		alpha = 1U;

	/* Each face's alpha (a depth format has none). */
	for (face = 0U; face < GLES_FACES; face++) {
		if (alpha != 0U)
			pixels[face * format->bytes + 3U] = alpha;
	}
	texture_level_set(texture, 0U, 0, 1, 1, (int)GLES_FACES, pixels, format);

	/* Succeeded: the texture, kept. */
	state->black_cube_arrays[kind] = texture;
	return texture;
}

/*
 * Returns the multisample texture of a kind (0 float, 1 int, 2 unsigned)
 * a multisample sampler reads where its unit has none: one zero texel of
 * the first of the kind's formats that has a count above one as a
 * multisample texture (an integer one's of the device's counts of
 * integers), of the fewest such samples, made at the first use; NULL when
 * none can be made.
 */
struct gles_texture *
gles_texture_black_ms(
	struct gles_state *state,
	unsigned kind)
{
	static const GLenum internals[3][3] = {
		{ GL_RGBA8, GL_RGBA16F, GL_RGBA32F },
		{ GL_RGBA8I, GL_RGBA16I, GL_RGBA32I },
		{ GL_RGBA8UI, GL_RGBA16UI, GL_RGBA32UI }
	};
	const struct gles_format *format;
	struct gles_texture *texture;
	uint32_t samples;
	unsigned index;

	/* Made once per kind. */
	if (kind > 2U)
		kind = 0U;
	if (state->black_ms[kind] != NULL)
		return state->black_ms[kind];

	/* The kind's first format of a count above one. */
	format = NULL;
	samples = 1U;
	for (index = 0U; index < 3U && samples < 2U; index++) {
		format = gles_format_renderable(state, internals[kind][index]);
		if (format == NULL)
			continue;
		samples = gles_format_samples_for(state, GL_TEXTURE_2D_MULTISAMPLE, internals[kind][index], 2U);
	}
	if (format == NULL || samples < 2U)
		return NULL;

	/* The texture: one texel of the fewest samples, cleared when its image is made. */
	texture = texture_new(0U, GL_TEXTURE_2D_MULTISAMPLE);
	if (texture == NULL)
		return NULL;
	texture->samples = samples;
	texture_level_set(texture, 0U, 0, 1, 1, 1, NULL, format);

	/* Succeeded: the texture, kept. */
	state->black_ms[kind] = texture;
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
	struct gles_garbage objects;
	unsigned level;

	/* The device image and the views of its faces. */
	gles_throw_away(state, VK_NULL_HANDLE, texture->image, texture->view, texture->memory);
	texture_views_free(state, texture);

	/* A buffer texture's view of its buffer. */
	if (texture->texel_view != VK_NULL_HANDLE) {
		memset(&objects, 0, sizeof(objects));
		objects.buffer_view = texture->texel_view;
		gles_garbage_keep(state, &objects);
	}

	/* The levels of every face, then the object. */
	for (level = 0U; level < GLES_FACES * GLES_LEVELS; level++)
		free(texture->levels[level].pixels);
	free(texture);
}

/*
 * Gives a level of a texture's face (0 for a 2D texture) new texels of one
 * image kept in a format, which the texture takes over.
 */
void
gles_texture_define(
	struct gles_texture *texture,
	unsigned face,
	GLint level,
	int width,
	int height,
	unsigned char *pixels,
	const struct gles_format *format)
{
	/* One slice (none when the level is let go). */
	if (width == 0 || height == 0) {
		texture_level_set(texture, face, level, width, height, 0, pixels, format);
		return;
	}

	/* The level's one image. */
	texture_level_set(texture, face, level, width, height, 1, pixels, format);
}

/*
 * Gives a level of a 3D or 2D array texture new texels of its slices or
 * layers, one after another, kept in a format, which the texture takes
 * over.
 */
void
gles_texture_define_volume(
	struct gles_texture *texture,
	GLint level,
	int width,
	int height,
	int depth,
	unsigned char *pixels,
	const struct gles_format *format)
{
	/* The level of the only face. */
	texture_level_set(texture, 0U, level, width, height, depth, pixels, format);
}

/*
 * Returns the view of one level and one layer (a cube map's face, a 2D
 * array's layer) of a texture's image that a framebuffer object draws
 * into, making it at its first use; VK_NULL_HANDLE when the image has no
 * such level or layer, or the device refused.  The level is the image's
 * (0: the base level).
 */
VkImageView
gles_texture_attach_view(
	struct gles_state *state,
	struct gles_texture *texture,
	uint32_t level,
	uint32_t layer)
{
	VkImageViewCreateInfo create;
	struct gles_attach_view *entry;
	VkResult result;

	/* An image with the level. */
	if (texture->image == VK_NULL_HANDLE || level >= texture->level_count)
		return VK_NULL_HANDLE;

	/* One made before (not a storage image's). */
	for (entry = texture->attach_views; entry != NULL; entry = entry->next) {
		if (entry->level == level &&
		    entry->layer == layer &&
		    entry->storage_format == VK_FORMAT_UNDEFINED)
			return entry->view;
	}

	/* A new entry. */
	entry = calloc(1U, sizeof(*entry));
	if (entry == NULL)
		return VK_NULL_HANDLE;

	/* A 2D view of the level and layer (a 2D array view of every layer for GLES_LAYER_ALL), every aspect the format draws. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create.image = texture->image;
	create.viewType = VK_IMAGE_VIEW_TYPE_2D;
	if (layer == GLES_LAYER_ALL)
		create.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	create.format = texture->image_format->vk;
	create.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	if (texture->image_format->kind == GLES_TEXEL_DEPTH)
		create.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	if (texture->image_format->stencil)
		create.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
	create.subresourceRange.baseMipLevel = level;
	create.subresourceRange.levelCount = 1U;
	create.subresourceRange.baseArrayLayer = layer;
	create.subresourceRange.layerCount = 1U;
	if (layer == GLES_LAYER_ALL) {
		create.subresourceRange.baseArrayLayer = 0U;
		create.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
	}

	/* The view. */
	result = vkCreateImageView(state->device, &create, NULL, &entry->view);
	if (result != VK_SUCCESS) {
		free(entry);
		return VK_NULL_HANDLE;
	}

	/* Succeeded: the view, kept with the image. */
	entry->level = level;
	entry->layer = layer;
	entry->next = texture->attach_views;
	texture->attach_views = entry;
	return entry->view;
}

/*
 * Returns a storage image view of a level (the image's: 0 is the base
 * level) of a texture's image, one layer or every one (GLES_LAYER_ALL), of
 * a view type and a format (image load and store), made at its first use
 * and kept with the image's other views; VK_NULL_HANDLE when the image has
 * no such level or the device refused.
 */
VkImageView
gles_texture_storage_view(
	struct gles_state *state,
	struct gles_texture *texture,
	uint32_t level,
	uint32_t layer,
	VkImageViewType type,
	VkFormat format)
{
	VkImageViewCreateInfo create;
	struct gles_attach_view *entry;
	VkResult result;

	/* A storage image with the level. */
	if (texture->image == VK_NULL_HANDLE || !texture->storage || level >= texture->level_count)
		return VK_NULL_HANDLE;

	/* One made before. */
	for (entry = texture->attach_views; entry != NULL; entry = entry->next) {
		if (entry->level == level &&
		    entry->layer == layer &&
		    entry->storage_type == type &&
		    entry->storage_format == format)
			return entry->view;
	}

	/* A new entry. */
	entry = calloc(1U, sizeof(*entry));
	if (entry == NULL)
		return VK_NULL_HANDLE;

	/* The view of the level: of the one layer, or of every one. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create.image = texture->image;
	create.viewType = type;
	create.format = format;
	create.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	create.subresourceRange.baseMipLevel = level;
	create.subresourceRange.levelCount = 1U;
	create.subresourceRange.baseArrayLayer = layer;
	create.subresourceRange.layerCount = 1U;
	if (layer == GLES_LAYER_ALL) {
		create.subresourceRange.baseArrayLayer = 0U;
		create.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
	}

	/* The view. */
	result = vkCreateImageView(state->device, &create, NULL, &entry->view);
	if (result != VK_SUCCESS) {
		gles_report("vkCreateImageView of a storage image", (int)result);
		free(entry);
		return VK_NULL_HANDLE;
	}

	/* Succeeded: the view, kept with the image. */
	entry->level = level;
	entry->layer = layer;
	entry->storage_type = type;
	entry->storage_format = format;
	entry->next = texture->attach_views;
	texture->attach_views = entry;
	return entry->view;
}

/*
 * Returns the 1x1 texture (one layer, or six faces) of a target and an
 * internal format an image unit takes, which a dispatch's image uniform
 * reads where its unit's binding does not fit it, making it at its first
 * use; NULL when there is no memory or no such format.
 */
struct gles_texture *
gles_texture_blank(
	struct gles_state *state,
	GLenum target,
	GLenum internal)
{
	static const GLenum targets[GLES_SHAPES] = { GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_3D, GL_TEXTURE_2D_ARRAY };
	const struct gles_format *format;
	struct gles_texture *texture;
	struct gles_texture **kept;
	unsigned char *pixels;
	unsigned shape;
	unsigned slot;
	unsigned faces;
	unsigned face;

	/* The slot of the shape and the format. */
	for (shape = 0U; shape < GLES_SHAPES; shape++) {
		if (targets[shape] == target)
			break;
	}
	format = gles_format_find(state, internal, GL_NONE);
	if (shape == GLES_SHAPES || format == NULL)
		return NULL;
	for (slot = 0U; slot < GLES_IMAGE_FORMATS; slot++) {
		kept = &state->image_blanks[shape][slot];
		if (*kept == NULL || (*kept)->levels[0].format == format)
			break;
	}
	if (slot == GLES_IMAGE_FORMATS)
		return NULL;

	/* Already made. */
	if (*kept != NULL)
		return *kept;

	/* The texture: zero texels on each face, or in the one slice or layer, immutable. */
	texture = texture_new(0U, target);
	if (texture == NULL)
		return NULL;
	texture->sampling.min_filter = GL_NEAREST;
	texture->sampling.mag_filter = GL_NEAREST;
	texture->immutable = 1;
	texture->immutable_levels = 1;
	faces = texture_faces(texture);
	for (face = 0U; face < faces; face++) {
		pixels = calloc(format->bytes, 1U);
		if (pixels == NULL) {
			gles_texture_free(state, texture);
			return NULL;
		}
		texture_level_set(texture, face, 0, 1, 1, 1, pixels, format);
	}

	/* Succeeded: the texture, kept. */
	*kept = texture;
	return texture;
}

/*
 * Frees the sampler objects and their namespace.
 */
void
gles_samplers_release(
	struct gles_state *state)
{
	GLuint name;

	/* Each sampler object. */
	for (name = 1U; name < state->sampler_objects.capacity; name++)
		free(state->sampler_objects.objects[name]);

	/* The namespace. */
	free(state->sampler_objects.objects);
	state->sampler_objects.objects = NULL;
	state->sampler_objects.capacity = 0U;
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
		texture = texture_new(name, 0U);
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
			if (state->cube_units[unit] == texture)
				state->cube_units[unit] = NULL;
			if (state->volume_units[unit] == texture)
				state->volume_units[unit] = NULL;
			if (state->array_units[unit] == texture)
				state->array_units[unit] = NULL;
			if (state->rect_units[unit] == texture)
				state->rect_units[unit] = NULL;
			if (state->buffer_units[unit] == texture)
				state->buffer_units[unit] = NULL;
			if (state->ms_units[unit] == texture)
				state->ms_units[unit] = NULL;
			if (state->cube_array_units[unit] == texture)
				state->cube_array_units[unit] = NULL;
		}

		/* Unbound from every image unit. */
		gles_image_forget(state, texture);

		/* Detached from the bound framebuffer object; the name and the texture go (its image waits for the frame). */
		gles_framebuffers_forget(state, GLES_ATTACH_TEXTURE, textures[index]);
		gles_names_remove(&state->textures, textures[index]);
		gles_texture_free(state, texture);
	}
}

/*
 * Binds a texture to the active unit's GL_TEXTURE_2D,
 * GL_TEXTURE_CUBE_MAP, GL_TEXTURE_3D or GL_TEXTURE_2D_ARRAY, making it
 * when the name is new; a texture keeps the target it was first bound to.
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

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;

	/* One of the four targets, or desktop GL's rectangle and buffer textures (libGL). */
	switch (target) {
	case GL_TEXTURE_2D:
	case GL_TEXTURE_CUBE_MAP:
	case GL_TEXTURE_3D:
	case GL_TEXTURE_2D_ARRAY:
		break;
	case GL_TEXTURE_RECTANGLE:
	case GL_TEXTURE_BUFFER:
		if (gles_fixed != NULL)
			break;
		gles_error(context, GL_INVALID_ENUM);
		return;
	case GL_TEXTURE_2D_MULTISAMPLE:
		/* Desktop GL's (libGL), and OpenGL ES 3.1's (a context that offers compute). */
		if (gles_fixed != NULL || state->compute)
			break;
		gles_error(context, GL_INVALID_ENUM);
		return;
	case GL_TEXTURE_CUBE_MAP_ARRAY:
		/* Desktop GL 4.0's cube map arrays (libGL), on a device that has them. */
		if (gles_fixed != NULL && state->display->features.imageCubeArray)
			break;
		gles_error(context, GL_INVALID_ENUM);
		return;
	default:
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The texture: none for name 0, made for a name not yet used. */
	texture = NULL;
	if (name != 0U) {
		texture = gles_names_get(&state->textures, name);
		if (texture == NULL) {
			texture = texture_new(name, target);
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

	/* A texture made by glGenTextures takes the target now; one of the other target cannot be bound here. */
	if (texture != NULL && texture->target == 0U)
		texture->target = target;
	if (texture != NULL && texture->target != target) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Bound to the active unit's target. */
	switch (target) {
	case GL_TEXTURE_CUBE_MAP:
		state->cube_units[state->active_unit] = texture;
		break;
	case GL_TEXTURE_3D:
		state->volume_units[state->active_unit] = texture;
		break;
	case GL_TEXTURE_2D_ARRAY:
		state->array_units[state->active_unit] = texture;
		break;
	case GL_TEXTURE_RECTANGLE:
		state->rect_units[state->active_unit] = texture;
		break;
	case GL_TEXTURE_BUFFER:
		state->buffer_units[state->active_unit] = texture;
		break;
	case GL_TEXTURE_2D_MULTISAMPLE:
		state->ms_units[state->active_unit] = texture;
		break;
	case GL_TEXTURE_CUBE_MAP_ARRAY:
		state->cube_array_units[state->active_unit] = texture;
		break;
	default:
		state->units[state->active_unit] = texture;
		break;
	}
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
 * Specifies a level of the bound texture in an internal format.
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
	const struct gles_format *storage;
	unsigned char *converted;
	unsigned face;

	/* The bound texture (a cube map's face), a level and a size it can have. */
	context = gles_context();
	texture = texture_changing(context, target, TEXTURE_IMAGE_2D, &face);
	if (texture == NULL)
		return;
	if (level < 0 ||
	    level >= (GLint)GLES_LEVELS ||
	    width < 0 ||
	    height < 0 ||
	    width > 16384 ||
	    height > 16384 ||
	    border != 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A cube map's faces are square. */
	if (texture->target == GL_TEXTURE_CUBE_MAP && width != height) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* glTexStorage2D fixed the levels. */
	if (texture->immutable) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The format the level is kept in. */
	storage = gles_format_find(gles_state(context), (GLenum)internalformat, type);
	if (storage == NULL) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The pixels in the kept form (NULL pixels: zeros). */
	converted = texture_texels(context, storage, format, type, width, height, 0, pixels);
	if (converted == NULL)
		return;

	/* Succeeded: the level, which fails with an error when the device makes no such image. */
	gles_texture_define(texture, face, level, width, height, converted, storage);
	texture_check(context, texture, face, level);
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
	size_t bytes;
	unsigned face;
	GLsizei row;

	/* The bound texture (a cube map's face) and a rectangle inside a specified level. */
	context = gles_context();
	texture = texture_changing(context, target, TEXTURE_IMAGE_2D, &face);
	if (texture == NULL)
		return;
	if (level < 0 || level >= (GLint)GLES_LEVELS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The level. */
	destination = &texture->levels[face * GLES_LEVELS + (unsigned)level];
	if (xoffset < 0 || yoffset < 0 || width < 0 || height < 0 ||
	    xoffset + width > destination->width || yoffset + height > destination->height) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A level that has texels. */
	if (destination->format == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The pixels in the level's kept form. */
	converted = texture_texels(context, destination->format, format, type, width, height, 0, pixels);
	if (converted == NULL)
		return;

	/* Each row into place. */
	bytes = destination->format->bytes;
	for (row = 0; row < height; row++) {
		memcpy(destination->pixels + ((size_t)(yoffset + row) * (size_t)destination->width + (size_t)xoffset) * bytes,
		       converted + (size_t)row * (size_t)width * bytes, (size_t)width * bytes);
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
	const struct gles_format *storage;
	unsigned char *pixels;
	unsigned face;

	/* The bound texture (a cube map's face), a level and a size. */
	context = gles_context();
	texture = texture_changing(context, target, TEXTURE_IMAGE_2D, &face);
	if (texture == NULL)
		return;
	if (level < 0 || level >= (GLint)GLES_LEVELS || width <= 0 || height <= 0 || border != 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A cube map's faces are square. */
	if (texture->target == GL_TEXTURE_CUBE_MAP && width != height) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* glTexStorage2D fixed the levels. */
	if (texture->immutable) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The format the level is kept in (unsized formats: RGBA8). */
	storage = gles_format_find(gles_state(context), internalformat, GL_UNSIGNED_BYTE);
	if (storage == NULL) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The framebuffer's pixels in the kept form. */
	pixels = texture_from_framebuffer(context, storage, x, y, width, height);
	if (pixels == NULL)
		return;

	/* Succeeded: the level, which fails with an error when the device makes no such image. */
	gles_texture_define(texture, face, level, width, height, pixels, storage);
	texture_check(context, texture, face, level);
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
	size_t bytes;
	unsigned face;
	GLsizei row;

	/* The bound texture (a cube map's face) and a rectangle inside a specified level. */
	context = gles_context();
	texture = texture_changing(context, target, TEXTURE_IMAGE_2D, &face);
	if (texture == NULL)
		return;
	if (level < 0 || level >= (GLint)GLES_LEVELS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The level. */
	destination = &texture->levels[face * GLES_LEVELS + (unsigned)level];
	if (xoffset < 0 || yoffset < 0 || width <= 0 || height <= 0 ||
	    xoffset + width > destination->width || yoffset + height > destination->height) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A level that has texels. */
	if (destination->format == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The framebuffer's pixels in the level's kept form. */
	pixels = texture_from_framebuffer(context, destination->format, x, y, width, height);
	if (pixels == NULL)
		return;

	/* Each row into place. */
	bytes = destination->format->bytes;
	for (row = 0; row < height; row++) {
		memcpy(destination->pixels + ((size_t)(yoffset + row) * (size_t)destination->width + (size_t)xoffset) * bytes,
		       pixels + (size_t)row * (size_t)width * bytes, (size_t)width * bytes);
	}

	/* Succeeded: the image is stale. */
	free(pixels);
	texture->dirty = 1;
}

/*
 * Gives the bound texture a fixed set of levels of a sized internal
 * format (every face's, halving from the size given), whose texels are
 * zeros until glTexSubImage2D gives them.
 */
GL_APICALL void GL_APIENTRY
glTexStorage2D(
	GLenum target,
	GLsizei levels,
	GLenum internalformat,
	GLsizei width,
	GLsizei height)
{
	struct zegl_context *context;
	struct gles_texture *texture;
	const struct gles_format *storage;
	unsigned char *pixels;
	unsigned faces;
	unsigned face;
	GLsizei level;
	GLsizei largest;
	int level_width;
	int level_height;

	/* The bound texture as a whole. */
	context = gles_context();
	texture = texture_changing(context, target, TEXTURE_STORAGE_2D, &face);
	if (texture == NULL)
		return;

	/* A size and at least one level. */
	if (levels < 1 || width < 1 || height < 1 || width > 16384 || height > 16384) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A sized internal format (an unsized one is not an enum this call takes). */
	storage = gles_format_find(gles_state(context), internalformat, GL_NONE);
	if (storage == NULL || storage->legacy) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* No more levels than halving the larger side gives, square faces, and fixed only once. */
	largest = width;
	if (height > largest)
		largest = height;
	for (level = 1; (largest >> level) > 0; level++)
		continue;
	if (levels > level || levels > (GLsizei)GLES_LEVELS || texture->immutable) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A cube map's faces are square. */
	if (texture->target == GL_TEXTURE_CUBE_MAP && width != height) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each face's levels: the ones asked for with zero texels, none beyond them. */
	faces = texture_faces(texture);
	for (face = 0U; face < faces; face++) {
		level_width = width;
		level_height = height;
		for (level = 0; level < (GLsizei)GLES_LEVELS; level++) {
			pixels = NULL;
			if (level < levels) {
				pixels = calloc((size_t)level_width * (size_t)level_height * storage->bytes + 1U, 1U);
				if (pixels == NULL) {
					gles_error(context, GL_OUT_OF_MEMORY);
					return;
				}
			}

			/* The level (a level beyond them is not specified). */
			if (level < levels) {
				gles_texture_define(texture, face, level, level_width, level_height, pixels, storage);
			} else {
				gles_texture_define(texture, face, level, 0, 0, NULL, NULL);
			}

			/* The next level's size. */
			level_width /= 2;
			if (level_width < 1)
				level_width = 1;
			level_height /= 2;
			if (level_height < 1)
				level_height = 1;
		}
	}

	/* Succeeded: the levels are fixed, and fail with an error when the device makes no such image. */
	texture->immutable = 1;
	texture->immutable_levels = levels;
	texture_check(context, texture, 0U, 0);
}

/*
 * Specifies a level of the bound texture from compressed blocks (ETC2 and
 * EAC, decoded into the format's kept form).
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
	struct gles_texture *texture;
	const struct gles_format *storage;
	unsigned char *texels;
	unsigned face;

	/* The bound texture (a cube map's face; not a rectangle one), a level and a size it can have. */
	context = gles_context();
	texture = texture_changing(context, target, TEXTURE_IMAGE_2D, &face);
	if (texture == NULL)
		return;
	if (texture->target == GL_TEXTURE_RECTANGLE) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}
	if (level < 0 ||
	    level >= (GLint)GLES_LEVELS ||
	    width < 0 ||
	    height < 0 ||
	    width > 16384 ||
	    height > 16384 ||
	    border != 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A cube map's faces are square, and glTexStorage2D's levels are fixed. */
	if (texture->target == GL_TEXTURE_CUBE_MAP && width != height) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}
	if (texture->immutable) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A compressed format. */
	storage = gles_compressed_format(internalformat);
	if (storage == NULL) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Its blocks decoded. */
	texels = texture_compressed(context, storage, width, height, 1, imageSize, data);
	if (texels == NULL)
		return;

	/* Succeeded: the level, which fails with an error when the device makes no such image. */
	gles_texture_define(texture, face, level, width, height, texels, storage);
	texture_check(context, texture, face, level);
}

/*
 * Replaces a rectangle of a compressed level of the bound texture: whole
 * blocks (offsets of 4, sizes of 4 unless they reach the level's edge)
 * of the level's format.
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
	struct gles_texture *texture;
	struct gles_level *destination;
	unsigned char *texels;
	unsigned face;
	int blocks;

	/* The bound texture (a cube map's face) and a level. */
	context = gles_context();
	texture = texture_changing(context, target, TEXTURE_IMAGE_2D, &face);
	if (texture == NULL)
		return;
	if (level < 0 || level >= (GLint)GLES_LEVELS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A level of the compressed format given. */
	destination = &texture->levels[face * GLES_LEVELS + (unsigned)level];
	if (destination->format == NULL || destination->format->internal != format) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A rectangle of whole blocks inside the level. */
	blocks = texture_compressed_rectangle(destination, xoffset, yoffset, 0, width, height, 1);
	if (!blocks) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Its blocks decoded. */
	texels = texture_compressed(context, destination->format, width, height, 1, imageSize, data);
	if (texels == NULL)
		return;

	/* Each row into place. */
	texture_rows_place(destination, xoffset, yoffset, 0, width, height, 1, texels);

	/* Succeeded: the image is stale. */
	free(texels);
	texture->dirty = 1;
}

/*
 * Specifies a level of the bound 3D or 2D array texture in an internal
 * format: its slices or layers, one image after another.
 */
GL_APICALL void GL_APIENTRY
glTexImage3D(
	GLenum target,
	GLint level,
	GLint internalformat,
	GLsizei width,
	GLsizei height,
	GLsizei depth,
	GLint border,
	GLenum format,
	GLenum type,
	const void *pixels)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_texture *texture;
	const struct gles_format *storage;
	unsigned char *converted;
	unsigned face;
	int fits;

	/* The bound 3D or 2D array texture, a level and a border of 0. */
	context = gles_context();
	state = gles_state(context);
	texture = texture_changing(context, target, TEXTURE_IMAGE_3D, &face);
	if (texture == NULL)
		return;
	if (level < 0 || level >= (GLint)GLES_LEVELS || border != 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A size the shape allows. */
	fits = texture_volume_size(state, texture, width, height, depth);
	if (!fits) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* glTexStorage3D fixed the levels. */
	if (texture->immutable) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The format the level is kept in. */
	storage = gles_format_find(state, (GLenum)internalformat, type);
	if (storage == NULL) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A 3D texture holds no depth. */
	if (texture->target == GL_TEXTURE_3D && storage->kind == GLES_TEXEL_DEPTH) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The texels of every slice in the kept form (none: zeros). */
	converted = texture_texels(context, storage, format, type, width, height, depth, pixels);
	if (converted == NULL)
		return;

	/* Succeeded: the level, which fails with an error when the device makes no such image. */
	gles_texture_define_volume(texture, level, width, height, depth, converted, storage);
	texture_check(context, texture, 0U, level);
}

/*
 * Replaces a box of a level of the bound 3D or 2D array texture.
 */
GL_APICALL void GL_APIENTRY
glTexSubImage3D(
	GLenum target,
	GLint level,
	GLint xoffset,
	GLint yoffset,
	GLint zoffset,
	GLsizei width,
	GLsizei height,
	GLsizei depth,
	GLenum format,
	GLenum type,
	const void *pixels)
{
	struct zegl_context *context;
	struct gles_texture *texture;
	struct gles_level *destination;
	unsigned char *converted;
	unsigned face;

	/* The bound 3D or 2D array texture and a level. */
	context = gles_context();
	texture = texture_changing(context, target, TEXTURE_IMAGE_3D, &face);
	if (texture == NULL)
		return;
	if (level < 0 || level >= (GLint)GLES_LEVELS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A box inside the level. */
	destination = &texture->levels[level];
	if (xoffset < 0 ||
	    yoffset < 0 ||
	    zoffset < 0 ||
	    width < 0 ||
	    height < 0 ||
	    depth < 0 ||
	    xoffset + width > destination->width ||
	    yoffset + height > destination->height ||
	    zoffset + depth > destination->depth) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A level that has texels. */
	if (destination->format == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The texels in the level's kept form. */
	converted = texture_texels(context, destination->format, format, type, width, height, depth, pixels);
	if (converted == NULL)
		return;

	/* Each row of each slice into place. */
	texture_rows_place(destination, xoffset, yoffset, zoffset, width, height, depth, converted);

	/* Succeeded: the image is stale. */
	free(converted);
	texture->dirty = 1;
}

/*
 * Replaces a rectangle of one slice or layer of a level of the bound 3D
 * or 2D array texture with one of the framebuffer.
 */
GL_APICALL void GL_APIENTRY
glCopyTexSubImage3D(
	GLenum target,
	GLint level,
	GLint xoffset,
	GLint yoffset,
	GLint zoffset,
	GLint x,
	GLint y,
	GLsizei width,
	GLsizei height)
{
	struct zegl_context *context;
	struct gles_texture *texture;
	struct gles_level *destination;
	unsigned char *pixels;
	unsigned face;

	/* The bound 3D or 2D array texture and a level. */
	context = gles_context();
	texture = texture_changing(context, target, TEXTURE_IMAGE_3D, &face);
	if (texture == NULL)
		return;
	if (level < 0 || level >= (GLint)GLES_LEVELS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A rectangle inside one slice of the level. */
	destination = &texture->levels[level];
	if (xoffset < 0 ||
	    yoffset < 0 ||
	    zoffset < 0 ||
	    width <= 0 ||
	    height <= 0 ||
	    xoffset + width > destination->width ||
	    yoffset + height > destination->height ||
	    zoffset >= destination->depth) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A level that has texels. */
	if (destination->format == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The framebuffer's pixels in the level's kept form. */
	pixels = texture_from_framebuffer(context, destination->format, x, y, width, height);
	if (pixels == NULL)
		return;

	/* Each row into place in the slice. */
	texture_rows_place(destination, xoffset, yoffset, zoffset, width, height, 1, pixels);

	/* Succeeded: the image is stale. */
	free(pixels);
	texture->dirty = 1;
}

/*
 * Gives the bound 3D or 2D array texture a fixed set of levels of a sized
 * internal format (halving from the size given; a 2D array keeps its
 * layers), whose texels are zeros until glTexSubImage3D gives them.
 */
GL_APICALL void GL_APIENTRY
glTexStorage3D(
	GLenum target,
	GLsizei levels,
	GLenum internalformat,
	GLsizei width,
	GLsizei height,
	GLsizei depth)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_texture *texture;
	const struct gles_format *storage;
	unsigned char *pixels;
	unsigned face;
	GLsizei level;
	GLsizei largest;
	int level_width;
	int level_height;
	int level_depth;
	int fits;

	/* The bound 3D or 2D array texture. */
	context = gles_context();
	state = gles_state(context);
	texture = texture_changing(context, target, TEXTURE_IMAGE_3D, &face);
	if (texture == NULL)
		return;

	/* At least one level, and a size the shape allows. */
	fits = texture_volume_size(state, texture, width, height, depth);
	if (levels < 1 || width < 1 || height < 1 || depth < 1 || !fits) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A sized internal format (an unsized one is not an enum this call takes). */
	storage = gles_format_find(state, internalformat, GL_NONE);
	if (storage == NULL || storage->legacy) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* A 3D texture holds no depth. */
	if (texture->target == GL_TEXTURE_3D && storage->kind == GLES_TEXEL_DEPTH) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* No more levels than halving the largest side gives (a 2D array's layers are not halved), and fixed only once. */
	largest = width;
	if (height > largest)
		largest = height;
	if (texture->target == GL_TEXTURE_3D && depth > largest)
		largest = depth;
	for (level = 1; (largest >> level) > 0; level++)
		continue;
	if (levels > level || levels > (GLsizei)GLES_LEVELS || texture->immutable) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The levels asked for with zero texels, none beyond them. */
	level_width = width;
	level_height = height;
	level_depth = depth;
	for (level = 0; level < (GLsizei)GLES_LEVELS; level++) {
		pixels = NULL;
		if (level < levels) {
			pixels = calloc((size_t)level_width * (size_t)level_height * (size_t)level_depth * storage->bytes + 1U, 1U);
			if (pixels == NULL) {
				gles_error(context, GL_OUT_OF_MEMORY);
				return;
			}
		}

		/* The level (a level beyond them is not specified). */
		if (level < levels) {
			gles_texture_define_volume(texture, level, level_width, level_height, level_depth, pixels, storage);
		} else {
			gles_texture_define_volume(texture, level, 0, 0, 0, NULL, NULL);
		}

		/* The next level's size: a 3D texture's depth halves too. */
		level_width /= 2;
		if (level_width < 1)
			level_width = 1;
		level_height /= 2;
		if (level_height < 1)
			level_height = 1;
		if (texture->target == GL_TEXTURE_3D) {
			level_depth /= 2;
			if (level_depth < 1)
				level_depth = 1;
		}
	}

	/* Succeeded: the levels are fixed, and fail with an error when the device makes no such image. */
	texture->immutable = 1;
	texture->immutable_levels = levels;
	texture_check(context, texture, 0U, 0);
}

/*
 * Specifies a level of the bound 2D array texture from compressed blocks
 * (ETC2 and EAC; a 3D texture takes none), its layers one after another.
 */
GL_APICALL void GL_APIENTRY
glCompressedTexImage3D(
	GLenum target,
	GLint level,
	GLenum internalformat,
	GLsizei width,
	GLsizei height,
	GLsizei depth,
	GLint border,
	GLsizei imageSize,
	const void *data)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_texture *texture;
	const struct gles_format *storage;
	unsigned char *texels;
	unsigned face;
	int fits;

	/* The bound 2D array texture (or 3D, which ETC2 does not take), a level and a border of 0. */
	context = gles_context();
	state = gles_state(context);
	texture = texture_changing(context, target, TEXTURE_IMAGE_3D, &face);
	if (texture == NULL)
		return;
	if (level < 0 || level >= (GLint)GLES_LEVELS || border != 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A size the shape allows, and levels not fixed. */
	fits = texture_volume_size(state, texture, width, height, depth);
	if (!fits) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}
	if (texture->immutable) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A compressed format, of a 2D array texture. */
	storage = gles_compressed_format(internalformat);
	if (storage == NULL) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}
	if (texture->target == GL_TEXTURE_3D) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The blocks of every layer decoded. */
	texels = texture_compressed(context, storage, width, height, depth, imageSize, data);
	if (texels == NULL)
		return;

	/* Succeeded: the level, which fails with an error when the device makes no such image. */
	gles_texture_define_volume(texture, level, width, height, depth, texels, storage);
	texture_check(context, texture, 0U, level);
}

/*
 * Replaces a box of a compressed level of the bound 2D array texture:
 * whole blocks of the level's format in each layer.
 */
GL_APICALL void GL_APIENTRY
glCompressedTexSubImage3D(
	GLenum target,
	GLint level,
	GLint xoffset,
	GLint yoffset,
	GLint zoffset,
	GLsizei width,
	GLsizei height,
	GLsizei depth,
	GLenum format,
	GLsizei imageSize,
	const void *data)
{
	struct zegl_context *context;
	struct gles_texture *texture;
	struct gles_level *destination;
	unsigned char *texels;
	unsigned face;
	int blocks;

	/* The bound 2D array texture and a level. */
	context = gles_context();
	texture = texture_changing(context, target, TEXTURE_IMAGE_3D, &face);
	if (texture == NULL)
		return;
	if (level < 0 || level >= (GLint)GLES_LEVELS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A level of the compressed format given. */
	destination = &texture->levels[level];
	if (destination->format == NULL || destination->format->internal != format) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A box of whole blocks inside the level. */
	blocks = texture_compressed_rectangle(destination, xoffset, yoffset, zoffset, width, height, depth);
	if (!blocks) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Its blocks decoded. */
	texels = texture_compressed(context, destination->format, width, height, depth, imageSize, data);
	if (texels == NULL)
		return;

	/* Each row of each layer into place. */
	texture_rows_place(destination, xoffset, yoffset, zoffset, width, height, depth, texels);

	/* Succeeded: the image is stale. */
	free(texels);
	texture->dirty = 1;
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
	unsigned face;

	/* The bound texture takes it. */
	context = gles_context();
	texture = texture_bound(context, target, TEXTURE_PARAMETERS, &face);
	if (texture == NULL)
		return;
	(void)texture_parameter(context, texture, pname, param, (GLfloat)param);
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
	struct zegl_context *context;
	struct gles_texture *texture;
	unsigned face;

	/* The bound texture takes it (an enum or a level as the integer, a level of detail as the float). */
	context = gles_context();
	texture = texture_bound(context, target, TEXTURE_PARAMETERS, &face);
	if (texture == NULL)
		return;
	(void)texture_parameter(context, texture, pname, (GLint)param, param);
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
	glTexParameterf(target, pname, params[0]);
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
	struct gles_state *state;
	struct gles_texture *texture;
	GLfloat value;
	unsigned face;
	int status;

	/* The bound texture's parameter. */
	context = gles_context();
	texture = texture_bound(context, target, TEXTURE_PARAMETERS, &face);
	if (texture == NULL)
		return;
	state = gles_state(context);
	status = texture_get(state, texture, pname, &value);
	if (status != 0) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Succeeded: the value, rounded. */
	*params = (GLint)value;
	if (value > 0.0f)
		*params = (GLint)(value + 0.5f);
	if (value < 0.0f)
		*params = (GLint)(value - 0.5f);
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
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_texture *texture;
	GLfloat value;
	unsigned face;
	int status;

	/* The bound texture's parameter. */
	context = gles_context();
	texture = texture_bound(context, target, TEXTURE_PARAMETERS, &face);
	if (texture == NULL)
		return;
	state = gles_state(context);
	status = texture_get(state, texture, pname, &value);
	if (status != 0) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Succeeded: the value. */
	*params = value;
}

/*
 * Reports a parameter of one level of the bound texture of a target (a
 * cube map's face): its size, internal format, samples, the resolution
 * and type of each component, and whether it is compressed.
 */
GL_APICALL void GL_APIENTRY
glGetTexLevelParameteriv(
	GLenum target,
	GLint level,
	GLenum pname,
	GLint *params)
{
	struct zegl_context *context;
	GLint value;
	int status;

	/* The level's parameter. */
	context = gles_context();
	status = texture_level_get(context, target, level, pname, &value);
	if (status != 0)
		return;

	/* Succeeded: the value. */
	params[0] = value;
}

/*
 * Reports a parameter of one level of the bound texture as a float.
 */
GL_APICALL void GL_APIENTRY
glGetTexLevelParameterfv(
	GLenum target,
	GLint level,
	GLenum pname,
	GLfloat *params)
{
	struct zegl_context *context;
	GLint value;
	int status;

	/* The level's parameter. */
	context = gles_context();
	status = texture_level_get(context, target, level, pname, &value);
	if (status != 0)
		return;

	/* Succeeded: the value as a float. */
	params[0] = (GLfloat)value;
}

/*
 * Makes a buffer object's bytes the texels of the bound buffer texture
 * (desktop GL 3.1, libGL): texels of an internal format, or none for
 * buffer 0.
 */
GL_APICALL void GL_APIENTRY
glTexBuffer(
	GLenum target,
	GLenum internalformat,
	GLuint buffer)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_texture *texture;
	struct gles_buffer *object;
	VkFormatProperties properties;
	VkFormat format;
	unsigned bytes;
	unsigned kind;
	int status;

	/* A context with its state, and the buffer texture target. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (target != GL_TEXTURE_BUFFER || gles_fixed == NULL || !state->texel_buffers) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* The unit's buffer texture. */
	texture = state->buffer_units[state->active_unit];
	if (texture == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A format a buffer texture may have. */
	status = texture_buffer_format(internalformat, &format, &bytes, &kind);
	if (status != 0) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* A buffer object, or none. */
	object = NULL;
	if (buffer != 0U) {
		object = gles_names_get(&state->buffers, buffer);
		if (object == NULL) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}
	}

	/* The device reads the format from a buffer, or the texture reads as zeros. */
	memset(&properties, 0, sizeof(properties));
	vkGetPhysicalDeviceFormatProperties(state->display->physical, format, &properties);
	if ((properties.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) == 0U)
		format = VK_FORMAT_UNDEFINED;

	/* Succeeded: the buffer and its texels' format (the view is made at the next draw that reads it). */
	texture->texel_buffer = object;
	texture->texel_internal = internalformat;
	texture->texel_vk = format;
	texture->texel_bytes = bytes;
	texture->texel_kind = kind;
}

/*
 * Gives the bound multisample texture storage of a format and samples a
 * pixel (desktop GL 3.2, libGL); its samples start at zero.
 */
GL_APICALL void GL_APIENTRY
glTexImage2DMultisample(
	GLenum target,
	GLsizei samples,
	GLenum internalformat,
	GLsizei width,
	GLsizei height,
	GLboolean fixedsamplelocations)
{
	/* Storage that may be given again. */
	texture_multisample(target, samples, internalformat, width, height, fixedsamplelocations, 0);
}

/*
 * Gives the bound multisample texture storage that cannot change (GL
 * 4.3's, taken with the 3.2 call's rules).
 */
GL_APICALL void GL_APIENTRY
glTexStorage2DMultisample(
	GLenum target,
	GLsizei samples,
	GLenum internalformat,
	GLsizei width,
	GLsizei height,
	GLboolean fixedsamplelocations)
{
	/* Immutable storage. */
	texture_multisample(target, samples, internalformat, width, height, fixedsamplelocations, 1);
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
 * Makes every level of the bound texture below its base level by halving
 * it with a box filter.
 */
GL_APICALL void GL_APIENTRY
glGenerateMipmap(
	GLenum target)
{
	struct zegl_context *context;
	struct gles_texture *texture;
	const struct gles_format *format;
	struct gles_sampling nearest;
	unsigned faces;
	unsigned face;
	unsigned shape;
	GLint last;
	int complete;
	int status;

	/* The bound texture (a cube map as a whole) with a base level. */
	context = gles_context();
	texture = texture_changing(context, target, TEXTURE_MIPMAPS, &face);
	if (texture == NULL)
		return;
	format = texture->levels[texture->base_level].format;
	if (format == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A format of integers or depth, or one that cannot be filtered, has no mipmaps made for it. */
	if (format->kind == GLES_TEXEL_INT ||
	    format->kind == GLES_TEXEL_UINT ||
	    format->kind == GLES_TEXEL_DEPTH ||
	    !format->filterable) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The base levels must match (a cube map's faces), whatever the filters. */
	nearest = texture->sampling;
	nearest.min_filter = GL_NEAREST;
	nearest.mag_filter = GL_NEAREST;
	complete = gles_texture_complete(texture, &nearest);
	if (!complete) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The last level made: the most the texture has (glTexStorage2D's, MAX_LEVEL). */
	last = (GLint)GLES_LEVELS - 1;
	if (texture->max_level < last)
		last = texture->max_level;
	if (texture->immutable && texture->immutable_levels - 1 < last)
		last = texture->immutable_levels - 1;

	/*
	 * Each face's chain (the other shapes have one face): RGBA8 images
	 * averaged byte by byte, other formats and every 3D or 2D array
	 * texture through floats.
	 */
	faces = texture_faces(texture);
	shape = texture_shape(texture);
	for (face = 0U; face < faces; face++) {
		if (format->vk == VK_FORMAT_R8G8B8A8_UNORM && shape != GLES_SHAPE_3D && shape != GLES_SHAPE_ARRAY &&
		    shape != GLES_SHAPE_CUBE_ARRAY) {
			status = texture_mipmaps(texture, face, last);
		} else {
			status = texture_mipmaps_kept(texture, face, last);
		}

		/* Without memory the chain stops. */
		if (status != 0) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}
	}
}

/*
 * Makes names for sampler objects, each with GL's initial sampling state.
 */
GL_APICALL void GL_APIENTRY
glGenSamplers(
	GLsizei count,
	GLuint *samplers)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_sampler_object *sampler;
	GLsizei index;
	int status;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (count < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each name gets a sampler object. */
	for (index = 0; index < count; index++) {
		sampler = calloc(1U, sizeof(*sampler));
		if (sampler == NULL) {
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* GL's initial state under the first free name. */
		texture_sampling_initial(&sampler->sampling);
		sampler->name = gles_names_free(&state->sampler_objects);
		status = gles_names_add(&state->sampler_objects, sampler->name, sampler);
		if (status != 0) {
			free(sampler);
			gles_error(context, GL_OUT_OF_MEMORY);
			return;
		}

		/* The name goes back to the application. */
		samplers[index] = sampler->name;
	}
}

/*
 * Deletes sampler objects, unbinding them from every unit.
 */
GL_APICALL void GL_APIENTRY
glDeleteSamplers(
	GLsizei count,
	const GLuint *samplers)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_sampler_object *sampler;
	GLsizei index;
	unsigned unit;

	/* A context with its state. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (count < 0) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* Each name that is a sampler object. */
	for (index = 0; index < count; index++) {
		sampler = gles_names_get(&state->sampler_objects, samplers[index]);
		if (sampler == NULL)
			continue;

		/* Unbound from every unit. */
		for (unit = 0U; unit < GLES_UNITS; unit++) {
			if (state->unit_samplers[unit] == sampler)
				state->unit_samplers[unit] = NULL;
		}

		/* The name and the object go. */
		gles_names_remove(&state->sampler_objects, samplers[index]);
		free(sampler);
	}
}

/*
 * Binds a sampler object to a texture unit (0: the unit's textures use
 * their own sampling state again).
 */
GL_APICALL void GL_APIENTRY
glBindSampler(
	GLuint unit,
	GLuint sampler)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_sampler_object *object;

	/* A context with its state, and a unit it has. */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (unit >= GLES_UNITS) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The object: none for name 0, else one glGenSamplers made. */
	object = NULL;
	if (sampler != 0U) {
		object = gles_names_get(&state->sampler_objects, sampler);
		if (object == NULL) {
			gles_error(context, GL_INVALID_OPERATION);
			return;
		}
	}

	/* Bound to the unit. */
	state->unit_samplers[unit] = object;
}

/*
 * Reports whether a name is a sampler object.
 */
GL_APICALL GLboolean GL_APIENTRY
glIsSampler(
	GLuint sampler)
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
	object = gles_names_get(&state->sampler_objects, sampler);
	if (object == NULL)
		return GL_FALSE;
	return GL_TRUE;
}

/*
 * Sets an integer parameter of a sampler object.
 */
GL_APICALL void GL_APIENTRY
glSamplerParameteri(
	GLuint sampler,
	GLenum pname,
	GLint param)
{
	struct zegl_context *context;
	struct gles_sampler_object *object;
	int status;

	/* The sampler object takes it. */
	context = gles_context();
	object = texture_sampler_object(context, sampler);
	if (object == NULL)
		return;
	status = texture_sampling_parameter(context, &object->sampling, pname, param, (GLfloat)param);
	if (status > 0)
		gles_error(context, GL_INVALID_ENUM);
}

/*
 * Sets a parameter of a sampler object from a float.
 */
GL_APICALL void GL_APIENTRY
glSamplerParameterf(
	GLuint sampler,
	GLenum pname,
	GLfloat param)
{
	struct zegl_context *context;
	struct gles_sampler_object *object;
	int status;

	/* The sampler object takes it. */
	context = gles_context();
	object = texture_sampler_object(context, sampler);
	if (object == NULL)
		return;
	status = texture_sampling_parameter(context, &object->sampling, pname, (GLint)param, param);
	if (status > 0)
		gles_error(context, GL_INVALID_ENUM);
}

/*
 * Sets an integer parameter of a sampler object from an array.
 */
GL_APICALL void GL_APIENTRY
glSamplerParameteriv(
	GLuint sampler,
	GLenum pname,
	const GLint *param)
{
	/* The first value. */
	glSamplerParameteri(sampler, pname, param[0]);
}

/*
 * Sets a parameter of a sampler object from an array of floats.
 */
GL_APICALL void GL_APIENTRY
glSamplerParameterfv(
	GLuint sampler,
	GLenum pname,
	const GLfloat *param)
{
	/* The first value. */
	glSamplerParameterf(sampler, pname, param[0]);
}

/*
 * Reports a parameter of a sampler object.
 */
GL_APICALL void GL_APIENTRY
glGetSamplerParameteriv(
	GLuint sampler,
	GLenum pname,
	GLint *params)
{
	GLfloat value;

	/* The float, rounded. */
	value = 0.0f;
	glGetSamplerParameterfv(sampler, pname, &value);
	*params = (GLint)value;
	if (value > 0.0f)
		*params = (GLint)(value + 0.5f);
	if (value < 0.0f)
		*params = (GLint)(value - 0.5f);
}

/*
 * Reports a parameter of a sampler object as a float.
 */
GL_APICALL void GL_APIENTRY
glGetSamplerParameterfv(
	GLuint sampler,
	GLenum pname,
	GLfloat *params)
{
	struct zegl_context *context;
	struct gles_sampler_object *object;
	GLfloat value;
	int status;

	/* The sampler object's parameter. */
	context = gles_context();
	object = texture_sampler_object(context, sampler);
	if (object == NULL)
		return;
	status = texture_sampling_get(&object->sampling, pname, &value);
	if (status != 0) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/* Succeeded: the value. */
	*params = value;
}

/*
 * Returns the texture a target names on the active unit, and the face it
 * means, when the call takes the target (TEXTURE_TAKES_* bits):
 * GL_TEXTURE_2D, a cube map's face (the calls that give a level texels),
 * GL_TEXTURE_CUBE_MAP as a whole (parameters, storage and mipmaps),
 * GL_TEXTURE_3D, GL_TEXTURE_2D_ARRAY, and a multisample texture's (desktop
 * GL, or an OpenGL ES 3.1 context's).  NULL with the error recorded when
 * the target is wrong or nothing is bound.
 */
static struct gles_texture *
texture_bound(
	struct zegl_context *context,
	GLenum target,
	unsigned takes,
	unsigned *face)
{
	struct gles_state *state;
	struct gles_texture *texture;
	unsigned taken;

	/* A context with its state. */
	*face = 0U;
	state = gles_state(context);
	if (state == NULL)
		return NULL;

	/* The unit's texture of the target, and which of the targets it is. */
	texture = NULL;
	taken = 0U;
	if (target == GL_TEXTURE_2D) {
		texture = state->units[state->active_unit];
		taken = TEXTURE_TAKES_2D;
	} else if (target == GL_TEXTURE_CUBE_MAP) {
		texture = state->cube_units[state->active_unit];
		taken = TEXTURE_TAKES_CUBE;
	} else if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) {
		texture = state->cube_units[state->active_unit];
		*face = target - GL_TEXTURE_CUBE_MAP_POSITIVE_X;
		taken = TEXTURE_TAKES_FACES;
	} else if (target == GL_TEXTURE_3D) {
		texture = state->volume_units[state->active_unit];
		taken = TEXTURE_TAKES_3D;
	} else if (target == GL_TEXTURE_2D_ARRAY) {
		texture = state->array_units[state->active_unit];
		taken = TEXTURE_TAKES_ARRAY;
	} else if (target == GL_TEXTURE_CUBE_MAP_ARRAY && gles_fixed != NULL) {
		texture = state->cube_array_units[state->active_unit];
		taken = TEXTURE_TAKES_ARRAY;
	} else if (target == GL_TEXTURE_RECTANGLE && gles_fixed != NULL) {
		texture = state->rect_units[state->active_unit];
		taken = TEXTURE_TAKES_RECT;
	} else if (target == GL_TEXTURE_2D_MULTISAMPLE && (gles_fixed != NULL || state->compute)) {
		texture = state->ms_units[state->active_unit];
		taken = TEXTURE_TAKES_MS;
	}

	/* A target the call does not take. */
	if ((taken & takes) == 0U) {
		*face = 0U;
		gles_error(context, GL_INVALID_ENUM);
		return NULL;
	}

	/* Name 0 is not a texture that can be changed. */
	if (texture == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Succeeded: the texture. */
	return texture;
}

/*
 * Decodes the compressed blocks of a box (depth images of a size, one
 * after another) of a format: from the pixel unpack buffer at an offset
 * when one is bound, else from the application's memory (NULL: the
 * texels are zeros).  imageSize must be the box's blocks' bytes.  Returns
 * the texels in the format's kept form (malloc'ed), or NULL with the
 * error recorded.
 */
static unsigned char *
texture_compressed(
	struct zegl_context *context,
	const struct gles_format *format,
	GLsizei width,
	GLsizei height,
	GLsizei depth,
	GLsizei imageSize,
	const void *data)
{
	struct gles_state *state;
	const unsigned char *bytes;
	unsigned char *texels;
	size_t expected;
	size_t image_bytes;
	size_t texel_bytes;
	size_t offset;
	GLsizei image;

	/* The blocks' bytes are the size given. */
	state = gles_state(context);
	expected = gles_compressed_size(format->internal, width, height, depth);
	if (imageSize < 0 || (size_t)imageSize != expected) {
		gles_error(context, GL_INVALID_VALUE);
		return NULL;
	}

	/* The blocks: at an offset of the pixel unpack buffer, inside it, or the application's. */
	bytes = data;
	if (state->pixel_unpack_buffer != NULL) {
		offset = (size_t)(uintptr_t)data;
		if (offset > state->pixel_unpack_buffer->size || expected > state->pixel_unpack_buffer->size - offset) {
			gles_error(context, GL_INVALID_OPERATION);
			return NULL;
		}
		bytes = state->pixel_unpack_buffer->data + offset;
	}

	/* The texels, zeros when there are no blocks. */
	texel_bytes = (size_t)width * (size_t)height * (size_t)depth * format->bytes;
	texels = calloc(1U, texel_bytes + 1U);
	if (texels == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return NULL;
	}
	if (bytes == NULL)
		return texels;

	/* Each image decoded after the one before. */
	image_bytes = gles_compressed_size(format->internal, width, height, 1);
	for (image = 0; image < depth; image++) {
		gles_compressed_decode(format, bytes + (size_t)image * image_bytes, width, height,
				       texels + (size_t)image * (size_t)width * (size_t)height * format->bytes);
	}

	/* Succeeded: the texels. */
	return texels;
}

/*
 * Reports whether a box is whole compressed blocks inside a level: its
 * offsets multiples of 4 and its sizes too, unless they reach the level's
 * edge.
 */
static int
texture_compressed_rectangle(
	const struct gles_level *level,
	GLint xoffset,
	GLint yoffset,
	GLint zoffset,
	GLsizei width,
	GLsizei height,
	GLsizei depth)
{
	/* Inside the level. */
	if (xoffset < 0 ||
	    yoffset < 0 ||
	    zoffset < 0 ||
	    width < 0 ||
	    height < 0 ||
	    depth < 0 ||
	    xoffset + width > level->width ||
	    yoffset + height > level->height ||
	    zoffset + depth > level->depth)
		return 0;

	/* On block corners. */
	if ((xoffset % 4) != 0 || (yoffset % 4) != 0)
		return 0;

	/* Of whole blocks, but at the edges. */
	if ((width % 4) != 0 && xoffset + width != level->width)
		return 0;
	if ((height % 4) != 0 && yoffset + height != level->height)
		return 0;

	/* Succeeded: whole blocks. */
	return 1;
}

/*
 * Returns the bound texture for a call that changes it on the CPU, what
 * framebuffer objects drew into it read back first; NULL with the error
 * recorded.
 */
static struct gles_texture *
texture_changing(
	struct zegl_context *context,
	GLenum target,
	unsigned takes,
	unsigned *face)
{
	struct gles_texture *texture;
	int status;

	/* The bound texture. */
	texture = texture_bound(context, target, takes, face);
	if (texture == NULL)
		return NULL;

	/* What the device drew into it, on the CPU before the change. */
	status = gles_texture_fetch(context, texture);
	if (status != 0)
		return NULL;

	/* Succeeded: the texture, newest on the CPU. */
	return texture;
}

/* Makes a texture of a target (0: taken at its first bind) with GL's initial sampling state and no levels; NULL when there is no memory. */
static struct gles_texture *
texture_new(
	GLuint name,
	GLenum target)
{
	struct gles_texture *texture;

	/* The object. */
	texture = calloc(1U, sizeof(*texture));
	if (texture == NULL)
		return NULL;

	/* GL's initial state: the sampling, every level from 0, each channel its own. */
	texture->name = name;
	texture->target = target;
	texture_sampling_initial(&texture->sampling);
	texture->base_level = 0;
	texture->max_level = TEXTURE_LEVEL_MAX;
	texture->swizzle[0] = GL_RED;
	texture->swizzle[1] = GL_GREEN;
	texture->swizzle[2] = GL_BLUE;
	texture->swizzle[3] = GL_ALPHA;

	/* One sample a pixel until a multisample texture is given more. */
	texture->samples = 1U;

	/* A multisample texture has one level, read without filters. */
	if (target == GL_TEXTURE_2D_MULTISAMPLE) {
		texture->sampling.min_filter = GL_NEAREST;
		texture->sampling.mag_filter = GL_NEAREST;
	}

	/* A rectangle texture has one level, filtered linearly, clamped to its edges. */
	if (target == GL_TEXTURE_RECTANGLE) {
		texture->sampling.min_filter = GL_LINEAR;
		texture->sampling.wrap_s = GL_CLAMP_TO_EDGE;
		texture->sampling.wrap_t = GL_CLAMP_TO_EDGE;
		texture->sampling.wrap_r = GL_CLAMP_TO_EDGE;
	}

	/* Succeeded: the texture. */
	return texture;
}

/* Gives a level of a face new texels (the texture takes them over) of a size and slices, kept in a format; the image is stale. */
static void
texture_level_set(
	struct gles_texture *texture,
	unsigned face,
	GLint level,
	int width,
	int height,
	int depth,
	unsigned char *pixels,
	const struct gles_format *format)
{
	struct gles_level *defined;

	/* The old pixels go; the image is stale. */
	defined = &texture->levels[face * GLES_LEVELS + (unsigned)level];
	free(defined->pixels);
	defined->pixels = pixels;
	defined->width = width;
	defined->height = height;
	defined->depth = depth;
	defined->format = format;
	texture->dirty = 1;
}

/* Gives a sampling state GL's initial values. */
static void
texture_sampling_initial(
	struct gles_sampling *sampling)
{
	/* Mipmapped minification, linear magnification, repeating, every level, no comparison. */
	memset(sampling, 0, sizeof(*sampling));
	sampling->min_filter = GL_NEAREST_MIPMAP_LINEAR;
	sampling->mag_filter = GL_LINEAR;
	sampling->wrap_s = GL_REPEAT;
	sampling->wrap_t = GL_REPEAT;
	sampling->wrap_r = GL_REPEAT;
	sampling->min_lod = -TEXTURE_LOD_MAX;
	sampling->max_lod = TEXTURE_LOD_MAX;
	sampling->compare_mode = GL_NONE;
	sampling->compare_func = GL_LEQUAL;
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

/*
 * Converts the application's pixels of a format and type into a level's
 * kept form (OpenGL ES 2's unsized formats by texture_convert, the rest
 * by format.c): one image, or depth slices or layers (depth 0 for a 2D
 * call), found by the pixel store and the pixel unpack buffer.  Returns
 * the texels, or NULL with the error recorded.
 */
static unsigned char *
texture_texels(
	struct zegl_context *context,
	const struct gles_format *storage,
	GLenum format,
	GLenum type,
	GLsizei width,
	GLsizei height,
	GLsizei depth,
	const void *pixels)
{
	struct gles_state *state;
	const void *packed;
	unsigned char *owned;
	unsigned char *converted;
	GLsizei rows;
	GLenum error;

	/* The texels as tight rows, wherever the pixel store and the unpack buffer put them. */
	state = gles_state(context);
	error = gles_unpack(state, format, type, width, height, depth, pixels, &packed, &owned);
	if (error != GL_NO_ERROR) {
		gles_error(context, error);
		return NULL;
	}

	/* Every slice's rows, one after another. */
	rows = height;
	if (depth > 1)
		rows = height * depth;

	/* OpenGL ES 2's formats and types into RGBA8. */
	if (storage->legacy) {
		converted = texture_convert(context, format, type, width, rows, packed);
		free(owned);
		return converted;
	}

	/* Any other format through four channels. */
	converted = NULL;
	error = gles_texels_convert(storage, format, type, width, rows, state->unpack_alignment, packed, &converted);
	free(owned);
	if (error != GL_NO_ERROR) {
		gles_error(context, error);
		return NULL;
	}

	/* Succeeded: the kept texels. */
	return converted;
}

/*
 * Reads a rectangle of the framebuffer (RGBA8) into a level's kept form.
 * Returns the texels, or NULL with the error recorded (an integer or
 * depth format cannot be copied from a colour framebuffer).
 */
static unsigned char *
texture_from_framebuffer(
	struct zegl_context *context,
	const struct gles_format *storage,
	GLint x,
	GLint y,
	GLsizei width,
	GLsizei height)
{
	unsigned char *pixels;
	unsigned char *converted;
	GLenum error;
	int status;

	/* The framebuffer's pixels as RGBA8. */
	pixels = malloc((size_t)width * (size_t)height * 4U);
	if (pixels == NULL) {
		gles_error(context, GL_OUT_OF_MEMORY);
		return NULL;
	}

	/* The framebuffer's pixels. */
	status = gles_read_rgba(context, x, y, width, height, pixels);
	if (status != 0) {
		free(pixels);
		return NULL;
	}

	/* RGBA8 kept texels are the pixels. */
	if (storage->vk == VK_FORMAT_R8G8B8A8_UNORM)
		return pixels;

	/* Any other kept form through four channels. */
	converted = NULL;
	error = gles_texels_from_rgba8(storage, pixels, (size_t)width * (size_t)height, &converted);
	free(pixels);
	if (error != GL_NO_ERROR) {
		gles_error(context, error);
		return NULL;
	}

	/* Succeeded: the kept texels. */
	return converted;
}

/* Copies tight rows of kept texels, slice by slice, into a box of a level at an offset (the caller checked the box fits). */
static void
texture_rows_place(
	struct gles_level *destination,
	GLint xoffset,
	GLint yoffset,
	GLint zoffset,
	GLsizei width,
	GLsizei height,
	GLsizei depth,
	const unsigned char *rows)
{
	size_t bytes;
	size_t target;
	size_t source;
	GLsizei slice;
	GLsizei row;

	/* Each row of each slice into place. */
	bytes = destination->format->bytes;
	for (slice = 0; slice < depth; slice++) {
		for (row = 0; row < height; row++) {
			target = (((size_t)(zoffset + slice) * (size_t)destination->height + (size_t)(yoffset + row)) * (size_t)destination->width + (size_t)xoffset) * bytes;
			source = ((size_t)slice * (size_t)height + (size_t)row) * (size_t)width * bytes;
			memcpy(destination->pixels + target, rows + source, (size_t)width * bytes);
		}
	}
}

/* Reports whether a 3D texture (every side up to the most) or a 2D array (its sides and layers up to theirs) may have a level of a size. */
static int
texture_volume_size(
	struct gles_state *state,
	const struct gles_texture *texture,
	GLsizei width,
	GLsizei height,
	GLsizei depth)
{
	GLsizei side;
	GLsizei layers;

	/* No side is negative. */
	if (width < 0 || height < 0 || depth < 0)
		return 0;

	/* A 3D texture's sides: GL's most, and the device's. */
	if (texture->target == GL_TEXTURE_3D) {
		side = GLES_MAX_3D_SIZE;
		if ((GLsizei)state->limits.maxImageDimension3D < side)
			side = (GLsizei)state->limits.maxImageDimension3D;
		if (width > side || height > side || depth > side)
			return 0;
		return 1;
	}

	/* A cube map array's faces are square, its layer-faces whole cubes. */
	if (texture->target == GL_TEXTURE_CUBE_MAP_ARRAY && (width != height || depth % 6 != 0))
		return 0;

	/* A 2D array's sides are a 2D texture's, its layers up to the most. */
	side = 16384;
	if ((GLsizei)state->limits.maxImageDimension2D < side)
		side = (GLsizei)state->limits.maxImageDimension2D;
	layers = GLES_MAX_LAYERS;
	if ((GLsizei)state->limits.maxImageArrayLayers < layers)
		layers = (GLsizei)state->limits.maxImageArrayLayers;
	if (width > side || height > side || depth > layers)
		return 0;

	/* Succeeded: the size fits. */
	return 1;
}

/*
 * Sets one parameter of a texture: its base and most levels and swizzle,
 * or its sampling state.  Returns 0, or nonzero with the error recorded
 * when the parameter or its value is not one.
 */
static int
texture_parameter(
	struct zegl_context *context,
	struct gles_texture *texture,
	GLenum pname,
	GLint value,
	GLfloat number)
{
	struct gles_state *state;
	GLenum mode;
	int status;

	/* The value as an enum. */
	mode = (GLenum)value;

	/* A multisample texture has one level and no sampling state (OpenGL ES 3.1 section 8.10, desktop GL 4.x section 8.10). */
	if (texture->target == GL_TEXTURE_2D_MULTISAMPLE) {
		/* Its base level stays 0. */
		if (pname == GL_TEXTURE_BASE_LEVEL && value != 0) {
			gles_error(context, GL_INVALID_OPERATION);
			return -1;
		}

		/* Filters, wraps, levels of detail and comparison are a sampler's, which it is not read by. */
		if (pname != GL_TEXTURE_BASE_LEVEL &&
		    pname != GL_TEXTURE_MAX_LEVEL &&
		    pname != GL_DEPTH_STENCIL_TEXTURE_MODE &&
		    (pname < GL_TEXTURE_SWIZZLE_R || pname > GL_TEXTURE_SWIZZLE_A)) {
			gles_error(context, GL_INVALID_ENUM);
			return -1;
		}
	}

	/* The texture's own parameters change which levels the image has, or its view: what the device drew is read back first. */
	switch (pname) {
	case GL_TEXTURE_BASE_LEVEL:
	case GL_TEXTURE_MAX_LEVEL:
		if (value < 0) {
			gles_error(context, GL_INVALID_VALUE);
			return -1;
		}

		/* The levels. */
		status = gles_texture_fetch(context, texture);
		if (status != 0)
			return -1;
		if (pname == GL_TEXTURE_BASE_LEVEL)
			texture->base_level = value;
		if (pname == GL_TEXTURE_MAX_LEVEL)
			texture->max_level = value;
		if (texture->base_level >= (GLint)GLES_LEVELS)
			texture->base_level = (GLint)GLES_LEVELS - 1;
		texture->dirty = 1;
		return 0;
	case GL_TEXTURE_SWIZZLE_R:
	case GL_TEXTURE_SWIZZLE_G:
	case GL_TEXTURE_SWIZZLE_B:
	case GL_TEXTURE_SWIZZLE_A:
		if (mode != GL_RED &&
		    mode != GL_GREEN &&
		    mode != GL_BLUE &&
		    mode != GL_ALPHA &&
		    mode != GL_ZERO &&
		    mode != GL_ONE) {
			gles_error(context, GL_INVALID_ENUM);
			return -1;
		}

		/* The channel's source, in a new view. */
		status = gles_texture_fetch(context, texture);
		if (status != 0)
			return -1;
		texture->swizzle[pname - GL_TEXTURE_SWIZZLE_R] = mode;
		texture->dirty = 1;
		return 0;
	case GL_DEPTH_STENCIL_TEXTURE_MODE:
		/* What a depth and stencil format reads (OpenGL ES 3.1; another context has no such parameter). */
		state = gles_state(context);
		if (gles_fixed != NULL || state == NULL || !state->compute) {
			gles_error(context, GL_INVALID_ENUM);
			return -1;
		}
		if (mode != GL_DEPTH_COMPONENT && mode != GL_STENCIL_INDEX) {
			gles_error(context, GL_INVALID_ENUM);
			return -1;
		}

		/* The depth or the stencil index (the image stays; draws pick the view). */
		texture->stencil_mode = mode;
		return 0;
	default:
		break;
	}

	/* The sampling state. */
	status = texture_sampling_parameter(context, &texture->sampling, pname, value, number);
	if (status > 0) {
		gles_error(context, GL_INVALID_ENUM);
		return -1;
	}

	/* Reports a value the parameter does not take. */
	if (status < 0)
		return -1;

	/* Succeeded: the parameter is set. */
	return 0;
}

/*
 * Sets one parameter of a sampling state.  Returns 0; -1 with the error
 * recorded for a value the parameter does not take; 1 (nothing recorded)
 * when the name is not a sampling parameter.
 */
static int
texture_sampling_parameter(
	struct zegl_context *context,
	struct gles_sampling *sampling,
	GLenum pname,
	GLint value,
	GLfloat number)
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
		sampling->min_filter = mode;
		return 0;
	case GL_TEXTURE_MAG_FILTER:
		if (mode != GL_NEAREST && mode != GL_LINEAR)
			break;
		sampling->mag_filter = mode;
		return 0;
	case GL_TEXTURE_WRAP_S:
	case GL_TEXTURE_WRAP_T:
	case GL_TEXTURE_WRAP_R:
		if (mode != GL_REPEAT && mode != GL_CLAMP_TO_EDGE && mode != GL_MIRRORED_REPEAT)
			break;
		if (pname == GL_TEXTURE_WRAP_S)
			sampling->wrap_s = mode;
		if (pname == GL_TEXTURE_WRAP_T)
			sampling->wrap_t = mode;
		if (pname == GL_TEXTURE_WRAP_R)
			sampling->wrap_r = mode;
		return 0;
	case GL_TEXTURE_MIN_LOD:
		sampling->min_lod = number;
		return 0;
	case GL_TEXTURE_MAX_LOD:
		sampling->max_lod = number;
		return 0;
	case GL_TEXTURE_COMPARE_MODE:
		if (mode != GL_NONE && mode != GL_COMPARE_REF_TO_TEXTURE)
			break;
		sampling->compare_mode = mode;
		return 0;
	case GL_TEXTURE_COMPARE_FUNC:
		if (mode < GL_NEVER || mode > GL_ALWAYS)
			break;
		sampling->compare_func = mode;
		return 0;
	default:
		return 1;
	}

	/* A value the parameter does not take. */
	gles_error(context, GL_INVALID_ENUM);
	return -1;
}

/*
 * Reads a parameter of one level of the bound texture of a target
 * (OpenGL ES 3.1 section 8.10.3, desktop GL 4.x section 8.11.3): a level
 * not specified reads as 0 by 0 of GL_RGBA without components.  Returns
 * 0, or -1 with the error recorded: a target that is not one image
 * (GL_INVALID_ENUM), a level there cannot be (GL_INVALID_VALUE), a name
 * that is not a level's parameter (GL_INVALID_ENUM), or an OpenGL ES
 * context before 3.1 (GL_INVALID_OPERATION, the call is not offered).
 */
static int
texture_level_get(
	struct zegl_context *context,
	GLenum target,
	GLint level,
	GLenum pname,
	GLint *value)
{
	GLint bits[TEXTURE_BITS];
	GLenum types[TEXTURE_BITS];
	struct gles_state *state;
	struct gles_texture *texture;
	const struct gles_level *image;
	const struct gles_format *compressed;
	GLint most;
	unsigned face;

	/* A context with its state; OpenGL ES offers the call from 3.1. */
	state = gles_state(context);
	if (state == NULL)
		return -1;
	if (gles_fixed == NULL && !state->compute) {
		gles_error(context, GL_INVALID_OPERATION);
		return -1;
	}

	/* The texture of one image's target (a whole cube map is not one). */
	texture = texture_bound(context, target, TEXTURE_LEVEL_QUERY, &face);
	if (texture == NULL)
		return -1;

	/* A level there can be: a multisample or rectangle texture has level 0 only. */
	most = (GLint)GLES_LEVELS - 1;
	if (target == GL_TEXTURE_2D_MULTISAMPLE || target == GL_TEXTURE_RECTANGLE)
		most = 0;
	if (level < 0 || level > most) {
		gles_error(context, GL_INVALID_VALUE);
		return -1;
	}

	/* The level's components: what each keeps, or none for a level not specified. */
	image = &texture->levels[face * GLES_LEVELS + (unsigned)level];
	memset(bits, 0, sizeof(bits));
	memset(types, 0, sizeof(types));
	compressed = NULL;
	if (image->width != 0 && image->format != NULL) {
		texture_level_bits(image->format, bits, types);
		compressed = gles_compressed_format(image->format->internal);
		if (compressed != image->format)
			compressed = NULL;
	}

	/* The parameter asked for. */
	switch (pname) {
	case GL_TEXTURE_WIDTH:
		*value = image->width;
		return 0;
	case GL_TEXTURE_HEIGHT:
		*value = image->height;
		return 0;
	case GL_TEXTURE_DEPTH:
		*value = 0;
		if (image->width != 0)
			*value = image->depth;
		return 0;
	case GL_TEXTURE_INTERNAL_FORMAT:
		*value = GL_RGBA;
		if (image->width != 0 && image->format != NULL)
			*value = (GLint)image->format->internal;
		return 0;
	case GL_TEXTURE_SAMPLES:
		*value = 0;
		if (texture->target == GL_TEXTURE_2D_MULTISAMPLE && image->width != 0)
			*value = (GLint)texture->samples;
		return 0;
	case GL_TEXTURE_FIXED_SAMPLE_LOCATIONS:
		*value = GL_TRUE;
		if (texture->target == GL_TEXTURE_2D_MULTISAMPLE && image->width != 0)
			*value = texture->fixed_locations;
		return 0;
	case GL_TEXTURE_RED_SIZE:
		*value = bits[TEXTURE_BITS_RED];
		return 0;
	case GL_TEXTURE_GREEN_SIZE:
		*value = bits[TEXTURE_BITS_GREEN];
		return 0;
	case GL_TEXTURE_BLUE_SIZE:
		*value = bits[TEXTURE_BITS_BLUE];
		return 0;
	case GL_TEXTURE_ALPHA_SIZE:
		*value = bits[TEXTURE_BITS_ALPHA];
		return 0;
	case GL_TEXTURE_DEPTH_SIZE:
		*value = bits[TEXTURE_BITS_DEPTH];
		return 0;
	case GL_TEXTURE_STENCIL_SIZE:
		*value = bits[TEXTURE_BITS_STENCIL];
		return 0;
	case GL_TEXTURE_SHARED_SIZE:
		*value = bits[TEXTURE_BITS_SHARED];
		return 0;
	case GL_TEXTURE_RED_TYPE:
		*value = (GLint)types[TEXTURE_BITS_RED];
		return 0;
	case GL_TEXTURE_GREEN_TYPE:
		*value = (GLint)types[TEXTURE_BITS_GREEN];
		return 0;
	case GL_TEXTURE_BLUE_TYPE:
		*value = (GLint)types[TEXTURE_BITS_BLUE];
		return 0;
	case GL_TEXTURE_ALPHA_TYPE:
		*value = (GLint)types[TEXTURE_BITS_ALPHA];
		return 0;
	case GL_TEXTURE_DEPTH_TYPE:
		*value = (GLint)types[TEXTURE_BITS_DEPTH];
		return 0;
	case GL_TEXTURE_COMPRESSED:
		*value = GL_FALSE;
		if (compressed != NULL)
			*value = GL_TRUE;
		return 0;
	default:
		break;
	}

	/* Desktop GL's own: a luminance format's component, and a compressed level's bytes. */
	if (gles_fixed != NULL) {
		/* The parameter asked for. */
		switch (pname) {
		case GL_TEXTURE_LUMINANCE_SIZE:
			*value = bits[TEXTURE_BITS_LUMINANCE];
			return 0;
		case GL_TEXTURE_LUMINANCE_TYPE:
			*value = (GLint)types[TEXTURE_BITS_LUMINANCE];
			return 0;
		case GL_TEXTURE_COMPRESSED_IMAGE_SIZE:
			if (compressed == NULL) {
				gles_error(context, GL_INVALID_OPERATION);
				return -1;
			}
			*value = (GLint)gles_compressed_size(compressed->internal, image->width, image->height, image->depth);
			return 0;
		default:
			break;
		}
	}

	/* Not a level's parameter. */
	gles_error(context, GL_INVALID_ENUM);
	return -1;
}

/*
 * Reports the resolution in bits and the type (GL_UNSIGNED_NORMALIZED,
 * GL_SIGNED_NORMALIZED, GL_FLOAT, GL_INT, GL_UNSIGNED_INT, or GL_NONE) of
 * each component a level's format has (TEXTURE_BITS_*, which the caller
 * starts at 0 and GL_NONE, both zero): the resolutions the texels are kept in (OpenGL ES 2's
 * RGB565 is kept as 8 bits a channel), and for a compressed format those
 * of the uncompressed format of about its quality.
 */
static void
texture_level_bits(
	const struct gles_format *format,
	GLint *bits,
	GLenum *types)
{
	const struct gles_format *compressed;
	GLint kept[TEXTURE_BITS];
	GLenum colour;
	GLenum depth;
	GLint each;
	unsigned channel;
	unsigned first;
	unsigned last;

	/* A compressed format: the components of its uncompressed match. */
	compressed = gles_compressed_format(format->internal);
	if (compressed == format) {
		texture_compressed_bits(format->internal, bits, types);
		return;
	}

	/* The bits of each component the Vulkan format keeps (the same for every colour component of most). */
	memset(kept, 0, sizeof(kept));
	each = 0;
	if (format->components != 0U)
		each = (GLint)(format->bytes * 8U / format->components);
	depth = GL_UNSIGNED_NORMALIZED;

	/* The packed and the depth and stencil formats. */
	switch (format->vk) {
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
	case VK_FORMAT_A2B10G10R10_UINT_PACK32:
		kept[TEXTURE_BITS_RED] = 10;
		kept[TEXTURE_BITS_GREEN] = 10;
		kept[TEXTURE_BITS_BLUE] = 10;
		kept[TEXTURE_BITS_ALPHA] = 2;
		break;
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		kept[TEXTURE_BITS_RED] = 11;
		kept[TEXTURE_BITS_GREEN] = 11;
		kept[TEXTURE_BITS_BLUE] = 10;
		break;
	case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
		kept[TEXTURE_BITS_RED] = 9;
		kept[TEXTURE_BITS_GREEN] = 9;
		kept[TEXTURE_BITS_BLUE] = 9;
		kept[TEXTURE_BITS_SHARED] = 5;
		break;
	case VK_FORMAT_D16_UNORM:
		kept[TEXTURE_BITS_DEPTH] = 16;
		break;
	case VK_FORMAT_X8_D24_UNORM_PACK32:
		kept[TEXTURE_BITS_DEPTH] = 24;
		break;
	case VK_FORMAT_D24_UNORM_S8_UINT:
		kept[TEXTURE_BITS_DEPTH] = 24;
		kept[TEXTURE_BITS_STENCIL] = 8;
		break;
	case VK_FORMAT_D32_SFLOAT:
		kept[TEXTURE_BITS_DEPTH] = 32;
		depth = GL_FLOAT;
		break;
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		kept[TEXTURE_BITS_DEPTH] = 32;
		kept[TEXTURE_BITS_STENCIL] = 8;
		depth = GL_FLOAT;
		break;
	default:
		kept[TEXTURE_BITS_RED] = each;
		kept[TEXTURE_BITS_GREEN] = each;
		kept[TEXTURE_BITS_BLUE] = each;
		kept[TEXTURE_BITS_ALPHA] = each;
		kept[TEXTURE_BITS_LUMINANCE] = each;
		break;
	}

	/* The type of the colour components: by the kind of texels, signed normalized for the SNORM formats. */
	colour = GL_UNSIGNED_NORMALIZED;
	if (format->kind == GLES_TEXEL_FLOAT)
		colour = GL_FLOAT;
	if (format->kind == GLES_TEXEL_INT)
		colour = GL_INT;
	if (format->kind == GLES_TEXEL_UINT)
		colour = GL_UNSIGNED_INT;
	if (format->vk == VK_FORMAT_R8_SNORM ||
	    format->vk == VK_FORMAT_R8G8_SNORM ||
	    format->vk == VK_FORMAT_R8G8B8A8_SNORM)
		colour = GL_SIGNED_NORMALIZED;

	/* The colour components the base format has: the first and the last of red to alpha, or depth and stencil. */
	first = TEXTURE_BITS_RED;
	last = TEXTURE_BITS_RED;
	switch (format->base) {
	case GL_RED:
	case GL_RED_INTEGER:
		break;
	case GL_RG:
	case GL_RG_INTEGER:
		last = TEXTURE_BITS_GREEN;
		break;
	case GL_RGB:
	case GL_RGB_INTEGER:
		last = TEXTURE_BITS_BLUE;
		break;
	case GL_ALPHA:
	case GL_LUMINANCE_ALPHA:
		first = TEXTURE_BITS_ALPHA;
		last = TEXTURE_BITS_ALPHA;
		break;
	case GL_LUMINANCE:
	case GL_DEPTH_COMPONENT:
	case GL_DEPTH_STENCIL:
		first = TEXTURE_BITS;
		last = 0U;
		break;
	default:
		last = TEXTURE_BITS_ALPHA;
		break;
	}

	/* The colour components it has, with their type. */
	for (channel = first; channel <= last && channel <= TEXTURE_BITS_ALPHA; channel++) {
		bits[channel] = kept[channel];
		types[channel] = colour;
	}

	/* A shared exponent goes with its components. */
	bits[TEXTURE_BITS_SHARED] = kept[TEXTURE_BITS_SHARED];

	/* A luminance format's luminance (desktop GL). */
	if (format->base == GL_LUMINANCE || format->base == GL_LUMINANCE_ALPHA) {
		bits[TEXTURE_BITS_LUMINANCE] = kept[TEXTURE_BITS_LUMINANCE];
		types[TEXTURE_BITS_LUMINANCE] = colour;
	}

	/* Depth and stencil. */
	if (format->kind == GLES_TEXEL_DEPTH) {
		bits[TEXTURE_BITS_DEPTH] = kept[TEXTURE_BITS_DEPTH];
		types[TEXTURE_BITS_DEPTH] = depth;
		bits[TEXTURE_BITS_STENCIL] = kept[TEXTURE_BITS_STENCIL];
	}
}

/*
 * Reports the components of a compressed format as those of the
 * uncompressed format of about its quality (OpenGL ES 3.1 section
 * 8.10.3): ETC2 as 8 bits a channel (an alpha of 1 bit for punch-through
 * alpha), EAC as 11 bits, normalized (signed for the signed EAC ones).
 * The components it does not have are left as the caller started them.
 */
static void
texture_compressed_bits(
	GLenum internal,
	GLint *bits,
	GLenum *types)
{
	GLenum type;
	GLint each;
	unsigned last;
	unsigned channel;

	/* The colour components, their resolution and type, and an alpha of its own. */
	each = 8;
	type = GL_UNSIGNED_NORMALIZED;
	last = TEXTURE_BITS_ALPHA;
	switch (internal) {
	case GL_COMPRESSED_SIGNED_R11_EAC:
		type = GL_SIGNED_NORMALIZED;
		each = 11;
		last = TEXTURE_BITS_RED;
		break;
	case GL_COMPRESSED_R11_EAC:
		each = 11;
		last = TEXTURE_BITS_RED;
		break;
	case GL_COMPRESSED_SIGNED_RG11_EAC:
		type = GL_SIGNED_NORMALIZED;
		each = 11;
		last = TEXTURE_BITS_GREEN;
		break;
	case GL_COMPRESSED_RG11_EAC:
		each = 11;
		last = TEXTURE_BITS_GREEN;
		break;
	case GL_COMPRESSED_RGB8_ETC2:
	case GL_COMPRESSED_SRGB8_ETC2:
	case GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2:
	case GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2:
		last = TEXTURE_BITS_BLUE;
		break;
	default:
		break;
	}

	/* Each colour component it has. */
	for (channel = TEXTURE_BITS_RED; channel <= last; channel++) {
		bits[channel] = each;
		types[channel] = type;
	}

	/* Punch-through alpha is one bit. */
	if (internal == GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2 || internal == GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2) {
		bits[TEXTURE_BITS_ALPHA] = 1;
		types[TEXTURE_BITS_ALPHA] = type;
	}
}

/* Reads one parameter of a texture as a float; nonzero when the name is not one. */
static int
texture_get(
	const struct gles_state *state,
	struct gles_texture *texture,
	GLenum pname,
	GLfloat *value)
{
	int status;

	/*
	 * How an image unit's format matches the texture's (OpenGL ES 3.1
	 * section 8.22): by size, for every texture GL makes.  Only a 3.1
	 * context has image units.
	 */
	if (pname == GL_IMAGE_FORMAT_COMPATIBILITY_TYPE) {
		if (gles_fixed != NULL || state == NULL || !state->compute)
			return -1;
		*value = (GLfloat)GL_IMAGE_FORMAT_COMPATIBILITY_BY_SIZE;
		return 0;
	}

	/* The texture's own parameters. */
	switch (pname) {
	case GL_TEXTURE_BASE_LEVEL:
		*value = (GLfloat)texture->base_level;
		return 0;
	case GL_TEXTURE_MAX_LEVEL:
		*value = (GLfloat)texture->max_level;
		return 0;
	case GL_TEXTURE_SWIZZLE_R:
	case GL_TEXTURE_SWIZZLE_G:
	case GL_TEXTURE_SWIZZLE_B:
	case GL_TEXTURE_SWIZZLE_A:
		*value = (GLfloat)texture->swizzle[pname - GL_TEXTURE_SWIZZLE_R];
		return 0;
	case GL_TEXTURE_IMMUTABLE_FORMAT:
		*value = (GLfloat)texture->immutable;
		return 0;
	case GL_TEXTURE_IMMUTABLE_LEVELS:
		*value = (GLfloat)texture->immutable_levels;
		return 0;
	case GL_DEPTH_STENCIL_TEXTURE_MODE:
		/* What a depth and stencil format reads (OpenGL ES 3.1). */
		if (gles_fixed != NULL || state == NULL || !state->compute)
			return -1;
		*value = (GLfloat)GL_DEPTH_COMPONENT;
		if (texture->stencil_mode == GL_STENCIL_INDEX)
			*value = (GLfloat)GL_STENCIL_INDEX;
		return 0;
	default:
		break;
	}

	/* The sampling state. */
	status = texture_sampling_get(&texture->sampling, pname, value);
	if (status != 0)
		return -1;

	/* Succeeded: the value. */
	return 0;
}

/* Reads one parameter of a sampling state as a float; nonzero when the name is not one. */
static int
texture_sampling_get(
	const struct gles_sampling *sampling,
	GLenum pname,
	GLfloat *value)
{
	/* The parameter. */
	switch (pname) {
	case GL_TEXTURE_MIN_FILTER:
		*value = (GLfloat)sampling->min_filter;
		return 0;
	case GL_TEXTURE_MAG_FILTER:
		*value = (GLfloat)sampling->mag_filter;
		return 0;
	case GL_TEXTURE_WRAP_S:
		*value = (GLfloat)sampling->wrap_s;
		return 0;
	case GL_TEXTURE_WRAP_T:
		*value = (GLfloat)sampling->wrap_t;
		return 0;
	case GL_TEXTURE_WRAP_R:
		*value = (GLfloat)sampling->wrap_r;
		return 0;
	case GL_TEXTURE_MIN_LOD:
		*value = sampling->min_lod;
		return 0;
	case GL_TEXTURE_MAX_LOD:
		*value = sampling->max_lod;
		return 0;
	case GL_TEXTURE_COMPARE_MODE:
		*value = (GLfloat)sampling->compare_mode;
		return 0;
	case GL_TEXTURE_COMPARE_FUNC:
		*value = (GLfloat)sampling->compare_func;
		return 0;
	default:
		break;
	}

	/* Not a parameter. */
	return -1;
}

/* Returns the sampler object of a name, recording GL_INVALID_OPERATION when the name is not one. */
static struct gles_sampler_object *
texture_sampler_object(
	struct zegl_context *context,
	GLuint name)
{
	struct gles_state *state;
	struct gles_sampler_object *sampler;

	/* A context with its state. */
	state = gles_state(context);
	if (state == NULL)
		return NULL;

	/* The name's object. */
	sampler = gles_names_get(&state->sampler_objects, name);
	if (sampler == NULL) {
		gles_error(context, GL_INVALID_OPERATION);
		return NULL;
	}

	/* Succeeded: the sampler object. */
	return sampler;
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

/* Reports whether a sampling state reads nearest texels only (a format that cannot be filtered may be read so). */
static int
texture_nearest(
	const struct gles_sampling *sampling)
{
	/* The magnification filter. */
	if (sampling->mag_filter != GL_NEAREST)
		return 0;

	/* The minification filter, which may choose the nearest level too. */
	if (sampling->min_filter != GL_NEAREST && sampling->min_filter != GL_NEAREST_MIPMAP_NEAREST)
		return 0;

	/* Nearest only. */
	return 1;
}

/*
 * Returns how many levels a texture's image has: from the base level, the
 * chain of halving sizes (on every face, of the base level's format, up
 * to the most level; a 3D texture's depth halves too, a 2D array's layers
 * stay as many).  The image has the chain whatever the filter, so a
 * framebuffer object can draw into any of its levels; a sampler without
 * mipmaps reads the first only.
 */
static uint32_t
texture_levels(
	struct gles_texture *texture)
{
	const struct gles_level *level;
	const struct gles_level *base;
	uint32_t levels;
	uint32_t limit;
	uint32_t faces;
	uint32_t face;
	int width;
	int height;
	int depth;
	int halve_depth;
	int whole;

	/* No more than the levels from the base to the most one. */
	limit = GLES_LEVELS - (uint32_t)texture->base_level;
	if (texture->max_level >= texture->base_level && (uint32_t)(texture->max_level - texture->base_level) + 1U < limit)
		limit = (uint32_t)(texture->max_level - texture->base_level) + 1U;

	/* Each next level whose size is the halved one on every face, and whose format is the base level's. */
	faces = texture_faces(texture);
	base = &texture->levels[texture->base_level];
	width = base->width;
	height = base->height;
	depth = base->depth;
	halve_depth = 0;
	if (texture->target == GL_TEXTURE_3D)
		halve_depth = 1;
	for (levels = 1U; levels < limit; levels++) {
		if (width == 1 && height == 1 && (!halve_depth || depth == 1))
			break;
		width = width / 2;
		if (width < 1)
			width = 1;
		height = height / 2;
		if (height < 1)
			height = 1;
		if (halve_depth) {
			depth = depth / 2;
			if (depth < 1)
				depth = 1;
		}

		/* The level of every face, of that size and format. */
		whole = 1;
		for (face = 0U; face < faces; face++) {
			level = &texture->levels[face * GLES_LEVELS + (uint32_t)texture->base_level + levels];
			if (level->width != width ||
			    level->height != height ||
			    level->depth != depth ||
			    level->pixels == NULL ||
			    level->format != base->format)
				whole = 0;
		}

		/* The chain ends where a face lacks the level. */
		if (!whole)
			break;
	}

	/* Succeeded: the chain found. */
	return levels;
}

/* Returns how many faces a texture has: six for a cube map, one for the other shapes. */
static uint32_t
texture_faces(
	const struct gles_texture *texture)
{
	/* A cube map. */
	if (texture->target == GL_TEXTURE_CUBE_MAP)
		return GLES_FACES;

	/* Succeeded: one face. */
	return 1U;
}

/* Returns a texture's shape (GLES_SHAPE_*) by its target (a texture not bound yet is 2D). */
static unsigned
texture_shape(
	const struct gles_texture *texture)
{
	/* The target it was bound to. */
	switch (texture->target) {
	case GL_TEXTURE_CUBE_MAP:
		return GLES_SHAPE_CUBE;
	case GL_TEXTURE_3D:
		return GLES_SHAPE_3D;
	case GL_TEXTURE_2D_ARRAY:
		return GLES_SHAPE_ARRAY;
	case GL_TEXTURE_CUBE_MAP_ARRAY:
		return GLES_SHAPE_CUBE_ARRAY;
	default:
		break;
	}

	/* Succeeded: a 2D texture. */
	return GLES_SHAPE_2D;
}

/* Makes every level of an RGBA8 face below its base level by halving it with a box filter, until 1x1 or the last level; nonzero when there is no memory. */
static int
texture_mipmaps(
	struct gles_texture *texture,
	unsigned face,
	GLint last)
{
	const struct gles_level *source;
	unsigned char *pixels;
	unsigned sum;
	unsigned channel;
	int width;
	int height;
	int x;
	int y;
	int dx;
	int dy;
	int sx;
	int sy;
	GLint level;

	/* Each level from the one above it, until 1x1. */
	for (level = texture->base_level + 1; level <= last; level++) {
		source = &texture->levels[face * GLES_LEVELS + (unsigned)level - 1U];
		if (source->width == 1 && source->height == 1)
			break;

		/* The halved size, at least 1. */
		width = source->width / 2;
		if (width < 1)
			width = 1;
		height = source->height / 2;
		if (height < 1)
			height = 1;

		/* Its pixels. */
		pixels = malloc((size_t)width * (size_t)height * 4U);
		if (pixels == NULL)
			return -1;

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
		gles_texture_define(texture, face, level, width, height, pixels, source->format);
	}

	/* Succeeded: the chain. */
	return 0;
}

/*
 * Makes every level of a face of any normalized or float format below its
 * base level, through floats (format.c): a 3D texture's slices halve too,
 * a 2D array's layers each on their own.  Nonzero when there is no memory.
 */
static int
texture_mipmaps_kept(
	struct gles_texture *texture,
	unsigned face,
	GLint last)
{
	const struct gles_level *source;
	unsigned char *pixels;
	int width;
	int height;
	int depth;
	int halve_depth;
	int status;
	GLint level;

	/* A 3D texture's depth halves with its sides. */
	halve_depth = 0;
	if (texture->target == GL_TEXTURE_3D)
		halve_depth = 1;

	/* Each level from the one above it, until 1x1 (and one slice in 3D). */
	for (level = texture->base_level + 1; level <= last; level++) {
		source = &texture->levels[face * GLES_LEVELS + (unsigned)level - 1U];
		if (source->width == 1 && source->height == 1 && (!halve_depth || source->depth == 1))
			break;

		/* The halved level. */
		status = gles_texels_halve(source->format, source->pixels, source->width, source->height, source->depth, halve_depth,
					   &pixels, &width, &height, &depth);
		if (status != 0)
			return -1;

		/* The level. */
		texture_level_set(texture, face, level, width, height, depth, pixels, source->format);
	}

	/* Succeeded: the chain. */
	return 0;
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
	/* The three wraps of OpenGL ES. */
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

/* Returns Vulkan's component swizzle for a GL swizzle source, the identity one when it is the channel's own. */
static VkComponentSwizzle
texture_swizzle(
	GLenum source,
	VkComponentSwizzle identity)
{
	VkComponentSwizzle swizzle;

	/* The source channel, or a constant. */
	switch (source) {
	case GL_RED:
		swizzle = VK_COMPONENT_SWIZZLE_R;
		break;
	case GL_GREEN:
		swizzle = VK_COMPONENT_SWIZZLE_G;
		break;
	case GL_BLUE:
		swizzle = VK_COMPONENT_SWIZZLE_B;
		break;
	case GL_ALPHA:
		swizzle = VK_COMPONENT_SWIZZLE_A;
		break;
	case GL_ZERO:
		return VK_COMPONENT_SWIZZLE_ZERO;
	default:
		return VK_COMPONENT_SWIZZLE_ONE;
	}

	/* A channel's own source is the identity. */
	if (swizzle == identity)
		return VK_COMPONENT_SWIZZLE_IDENTITY;

	/* Another channel. */
	return swizzle;
}

/* Lets the views of a texture's image framebuffer objects drew into go (they wait for the frame), leaving none. */
static void
texture_views_free(
	struct gles_state *state,
	struct gles_texture *texture)
{
	struct gles_attach_view *entry;

	/* Each view, then its entry. */
	while (texture->attach_views != NULL) {
		entry = texture->attach_views;
		texture->attach_views = entry->next;
		gles_throw_away(state, VK_NULL_HANDLE, VK_NULL_HANDLE, entry->view, VK_NULL_HANDLE);
		free(entry);
	}

	/* The view of the stencil aspect. */
	if (texture->stencil_view != VK_NULL_HANDLE) {
		gles_throw_away(state, VK_NULL_HANDLE, VK_NULL_HANDLE, texture->stencil_view, VK_NULL_HANDLE);
		texture->stencil_view = VK_NULL_HANDLE;
	}
}

/*
 * Makes a view read alpha as 1 where it would read the kept alpha of a
 * format GL has none for (an RGB format kept in four channels, which a
 * framebuffer object may have drawn alpha into).
 */
static void
texture_opaque(
	const struct gles_format *format,
	VkComponentMapping *components)
{
	VkComponentSwizzle *channels[4];
	unsigned index;

	/* Only the formats with three channels of GL's kept in four. */
	if (format->base != GL_RGB && format->base != GL_RGB_INTEGER)
		return;
	if (format->components != 4U)
		return;

	/* Each channel that reads alpha (its own, the identity, for the fourth) reads 1. */
	channels[0] = &components->r;
	channels[1] = &components->g;
	channels[2] = &components->b;
	channels[3] = &components->a;
	for (index = 0U; index < 4U; index++) {
		if (*channels[index] == VK_COMPONENT_SWIZZLE_A)
			*channels[index] = VK_COMPONENT_SWIZZLE_ONE;
		if (index == 3U && *channels[index] == VK_COMPONENT_SWIZZLE_IDENTITY)
			*channels[index] = VK_COMPONENT_SWIZZLE_ONE;
	}
}

/*
 * Finds the Vulkan format, texel bytes and kind (0 float, 1 int, 2
 * unsigned) of an internal format a buffer texture may have; nonzero for
 * one it may not.
 */
static int
texture_buffer_format(
	GLenum internal,
	VkFormat *format,
	unsigned *bytes,
	unsigned *kind)
{
	/*
	 * One internal format's texels in a buffer: its Vulkan format, its
	 * bytes and kind.
	 */
	static const struct {
		GLenum internal;
		VkFormat format;
		unsigned bytes;
		unsigned kind;
	} formats[] = {
		{ GL_R8, VK_FORMAT_R8_UNORM, 1U, 0U },
		{ GL_R16, VK_FORMAT_R16_UNORM, 2U, 0U },
		{ GL_R16F, VK_FORMAT_R16_SFLOAT, 2U, 0U },
		{ GL_R32F, VK_FORMAT_R32_SFLOAT, 4U, 0U },
		{ GL_R8I, VK_FORMAT_R8_SINT, 1U, 1U },
		{ GL_R16I, VK_FORMAT_R16_SINT, 2U, 1U },
		{ GL_R32I, VK_FORMAT_R32_SINT, 4U, 1U },
		{ GL_R8UI, VK_FORMAT_R8_UINT, 1U, 2U },
		{ GL_R16UI, VK_FORMAT_R16_UINT, 2U, 2U },
		{ GL_R32UI, VK_FORMAT_R32_UINT, 4U, 2U },
		{ GL_RG8, VK_FORMAT_R8G8_UNORM, 2U, 0U },
		{ GL_RG16, VK_FORMAT_R16G16_UNORM, 4U, 0U },
		{ GL_RG16F, VK_FORMAT_R16G16_SFLOAT, 4U, 0U },
		{ GL_RG32F, VK_FORMAT_R32G32_SFLOAT, 8U, 0U },
		{ GL_RG8I, VK_FORMAT_R8G8_SINT, 2U, 1U },
		{ GL_RG16I, VK_FORMAT_R16G16_SINT, 4U, 1U },
		{ GL_RG32I, VK_FORMAT_R32G32_SINT, 8U, 1U },
		{ GL_RG8UI, VK_FORMAT_R8G8_UINT, 2U, 2U },
		{ GL_RG16UI, VK_FORMAT_R16G16_UINT, 4U, 2U },
		{ GL_RG32UI, VK_FORMAT_R32G32_UINT, 8U, 2U },
		{ GL_RGB32F, VK_FORMAT_R32G32B32_SFLOAT, 12U, 0U },
		{ GL_RGB32I, VK_FORMAT_R32G32B32_SINT, 12U, 1U },
		{ GL_RGB32UI, VK_FORMAT_R32G32B32_UINT, 12U, 2U },
		{ GL_RGBA8, VK_FORMAT_R8G8B8A8_UNORM, 4U, 0U },
		{ GL_RGBA16, VK_FORMAT_R16G16B16A16_UNORM, 8U, 0U },
		{ GL_RGBA16F, VK_FORMAT_R16G16B16A16_SFLOAT, 8U, 0U },
		{ GL_RGBA32F, VK_FORMAT_R32G32B32A32_SFLOAT, 16U, 0U },
		{ GL_RGBA8I, VK_FORMAT_R8G8B8A8_SINT, 4U, 1U },
		{ GL_RGBA16I, VK_FORMAT_R16G16B16A16_SINT, 8U, 1U },
		{ GL_RGBA32I, VK_FORMAT_R32G32B32A32_SINT, 16U, 1U },
		{ GL_RGBA8UI, VK_FORMAT_R8G8B8A8_UINT, 4U, 2U },
		{ GL_RGBA16UI, VK_FORMAT_R16G16B16A16_UINT, 8U, 2U },
		{ GL_RGBA32UI, VK_FORMAT_R32G32B32A32_UINT, 16U, 2U }
	};
	unsigned index;

	/* The internal format's entry. */
	for (index = 0U; index < sizeof(formats) / sizeof(formats[0]); index++) {
		if (formats[index].internal != internal)
			continue;
		*format = formats[index].format;
		*bytes = formats[index].bytes;
		*kind = formats[index].kind;
		return 0;
	}

	/* Not a buffer texture's format. */
	return -1;
}

/*
 * Returns the view of four zero words as texels of a kind (0 float, 1
 * int, 2 unsigned), which a sampler of a unit without a buffer texture
 * reads, making the buffer and its views at the first use;
 * VK_NULL_HANDLE when they cannot be made.
 */
static VkBufferView
texture_black_buffer(
	struct gles_state *state,
	unsigned kind)
{
	static const VkFormat formats[3] = {
		VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_UINT
	};
	VkBufferViewCreateInfo create;
	VkResult result;
	void *mapped;
	int status;

	/* One of the three kinds, made already. */
	if (kind > 2U)
		kind = 0U;
	if (state->black_buffer_views[kind] != VK_NULL_HANDLE)
		return state->black_buffer_views[kind];

	/* The buffer of four zero words, made once. */
	if (state->black_buffer == VK_NULL_HANDLE) {
		status = gles_device_buffer(state, 16U, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, &state->black_buffer,
					    &state->black_buffer_memory, &mapped);
		if (status != 0)
			return VK_NULL_HANDLE;
		memset(mapped, 0, 16U);
	}

	/* The kind's view of it. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
	create.buffer = state->black_buffer;
	create.format = formats[kind];
	create.offset = 0U;
	create.range = 16U;
	result = vkCreateBufferView(state->device, &create, NULL, &state->black_buffer_views[kind]);
	if (result != VK_SUCCESS) {
		state->black_buffer_views[kind] = VK_NULL_HANDLE;
		return VK_NULL_HANDLE;
	}

	/* Succeeded: the view. */
	return state->black_buffer_views[kind];
}

/*
 * Clears a new multisample image to zero (every aspect of a depth one) on
 * the upload queue and leaves it ready to be sampled or drawn into.
 * Returns 0, or -1 when the upload failed.
 */
static int
texture_clear_samples(
	struct gles_state *state,
	VkImage image,
	VkImageAspectFlags aspects,
	const struct gles_format *format)
{
	VkImageMemoryBarrier barrier;
	VkClearColorValue colour;
	VkClearDepthStencilValue depth;
	VkImageSubresourceRange range;
	int status;

	/* The upload's command buffer. */
	status = gles_upload_begin(state);
	if (status != 0)
		return -1;

	/* Ready to be written. */
	memset(&range, 0, sizeof(range));
	range.aspectMask = aspects;
	range.levelCount = 1U;
	range.layerCount = 1U;
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange = range;
	vkCmdPipelineBarrier(state->upload, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* Zero, as a colour or a depth. */
	memset(&colour, 0, sizeof(colour));
	memset(&depth, 0, sizeof(depth));
	if (format->kind == GLES_TEXEL_DEPTH) {
		vkCmdClearDepthStencilImage(state->upload, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depth, 1U, &range);
	} else {
		vkCmdClearColorImage(state->upload, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1U, &range);
	}

	/* Ready to be sampled. */
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(state->upload, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* Submitted and waited for. */
	status = gles_upload_end(state);
	if (status != 0)
		return -1;

	/* Succeeded: the image is zero. */
	return 0;
}

/*
 * Gives the bound multisample texture storage (glTexImage2DMultisample,
 * glTexStorage2DMultisample): a colour format framebuffers draw into, or a
 * depth one; the samples rounded up to a count the device has.
 */
static void
texture_multisample(
	GLenum target,
	GLsizei samples,
	GLenum internalformat,
	GLsizei width,
	GLsizei height,
	GLboolean fixedsamplelocations,
	int immutable)
{
	struct zegl_context *context;
	struct gles_state *state;
	struct gles_texture *texture;
	const struct gles_format *format;
	uint32_t count;
	uint32_t most;

	/*
	 * A context with its state, the multisample target, and a texture
	 * bound: desktop GL's, or OpenGL ES 3.1's immutable one (a context
	 * that offers compute; no samples is GL_INVALID_VALUE there).
	 */
	context = gles_context();
	state = gles_state(context);
	if (state == NULL)
		return;
	if (target != GL_TEXTURE_2D_MULTISAMPLE || (gles_fixed == NULL && (!state->compute || !immutable))) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}
	if (gles_fixed == NULL && samples < 1) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* The unit's multisample texture, not fixed already. */
	texture = state->ms_units[state->active_unit];
	if (texture == NULL || texture->immutable) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* Samples there can be (no more than any format's: the device's most). */
	most = gles_samples_max(state);
	if (samples < 1 || (uint32_t)samples > most) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* A size there can be. */
	if (width < 1 ||
	    height < 1 ||
	    width > (GLsizei)state->limits.maxImageDimension2D ||
	    height > (GLsizei)state->limits.maxImageDimension2D) {
		gles_error(context, GL_INVALID_VALUE);
		return;
	}

	/* A format a framebuffer draws into. */
	format = gles_format_renderable(state, internalformat);
	if (format == NULL) {
		gles_error(context, GL_INVALID_ENUM);
		return;
	}

	/*
	 * No more samples than the format's most (its largest count of
	 * glGetInternalformativ: those the device samples of colours, of depth
	 * and stencil, or of integers -- GL_MAX_COLOR_TEXTURE_SAMPLES,
	 * GL_MAX_DEPTH_TEXTURE_SAMPLES, GL_MAX_INTEGER_SAMPLES -- of which it
	 * makes an image of the format).
	 */
	most = gles_format_samples_max(state, GL_TEXTURE_2D_MULTISAMPLE, internalformat);
	if ((uint32_t)samples > most) {
		gles_error(context, GL_INVALID_OPERATION);
		return;
	}

	/* The format's count, at least the samples asked for. */
	count = gles_format_samples_for(state, GL_TEXTURE_2D_MULTISAMPLE, internalformat, (uint32_t)samples);

	/* Succeeded: level 0 without texels on the CPU, of the samples; the image is made (zero) at the next use. */
	texture_level_set(texture, 0U, 0, width, height, 1, NULL, format);
	texture->samples = count;
	texture->fixed_locations = fixedsamplelocations;
	texture->immutable = immutable;
	texture->immutable_levels = 1;
}

/*
 * Checks that the device makes the image a level just specified needs (the
 * texture's shape, the level's format and size, sampled and copied, one
 * level), as texture syncing will ask for it: a format an image unit takes
 * that the device stores but does not sample is stored and copied instead
 * while the context has image units.  When it does not, the call
 * fails with GL_OUT_OF_MEMORY -- the error OpenGL ES gives when it cannot
 * make the storage asked for; GL_INVALID_OPERATION would say the
 * application's arguments were wrong -- and the first such image is
 * reported once a process.  The texture then reads as an incomplete one.
 */
static void
texture_check(
	struct zegl_context *context,
	struct gles_texture *texture,
	unsigned face,
	GLint level)
{
	VkImageCreateInfo create;
	const struct gles_level *source;
	struct gles_state *state;
	uint32_t features;
	unsigned shape;
	int supported;

	/* A level with a format and a size. */
	if (level < 0 || level >= (GLint)GLES_LEVELS || face >= GLES_FACES)
		return;
	source = &texture->levels[face * GLES_LEVELS + (unsigned)level];
	if (source->format == NULL || source->width <= 0 || source->height <= 0)
		return;

	/* The image as texture syncing makes it, one level of it. */
	state = gles_state(context);
	shape = texture_shape(texture);
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = source->format->vk;
	create.extent.width = (uint32_t)source->width;
	create.extent.height = (uint32_t)source->height;
	create.extent.depth = 1U;
	create.mipLevels = 1U;
	create.arrayLayers = texture_faces(texture);
	if (shape == GLES_SHAPE_CUBE || shape == GLES_SHAPE_CUBE_ARRAY)
		create.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	if (shape == GLES_SHAPE_ARRAY || shape == GLES_SHAPE_CUBE_ARRAY)
		create.arrayLayers = (uint32_t)source->depth;
	if (shape == GLES_SHAPE_3D) {
		create.imageType = VK_IMAGE_TYPE_3D;
		create.extent.depth = (uint32_t)source->depth;
	}
	create.samples = (VkSampleCountFlagBits)texture->samples;
	if (texture->samples == 0U)
		create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_OPTIMAL;
	create.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	/* A format only stored (texture syncing's choice, gles_texture_sync): a storage image instead of a sampled one. */
	features = gles_image_features(state, create.format);
	if (state->image_units != 0U &&
	    create.samples == VK_SAMPLE_COUNT_1_BIT &&
	    (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0U &&
	    (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0U &&
	    gles_image_unit_format(create.format))
		create.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	/* Made by the device, or the call's error and the report. */
	supported = gles_image_supported(state, &create);
	if (supported)
		return;
	gles_image_unmade(create.format, "a texture");
	gles_error(context, GL_OUT_OF_MEMORY);
}

/*
 * Writes a depth and stencil format's kept texels (8 bytes each: the
 * depth's 4, the stencil index, 3 of 0) as a copy of each aspect reads
 * them: every depth (4 bytes), then every stencil index (1 byte).
 */
static void
texture_split_stencil(
	unsigned char *out,
	const unsigned char *kept,
	size_t texels)
{
	size_t index;

	/* Each texel's depth, then its stencil index after all of them. */
	for (index = 0U; index < texels; index++) {
		memcpy(out + index * 4U, kept + index * 8U, 4U);
		out[texels * 4U + index] = kept[index * 8U + 4U];
	}
}
