/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Texture formats of zedBSD's OpenGL ES (WS068 p025): OpenGL ES 3.0's
 * sized internal formats, the Vulkan format each is kept in, and the
 * conversions of texels into that form.
 *
 * A texture level keeps its texels on the CPU exactly as a buffer to
 * image copy of its Vulkan format lays them out (the depth aspect's
 * layout for a depth format), so an upload is one copy.  Three-component
 * formats are kept in the four-component format of the same kind, whose
 * sampling every Vulkan device supports.  The application's texels are
 * read into a texel of four channels (floats, or integers' bits for an
 * integer format) and written in the kept form; mipmaps are made the same
 * way from the kept form.  OpenGL ES 2's unsized formats stay RGBA8 and
 * are converted by texture.c as before.
 */

#include "gles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The largest finite value of a shared-exponent RGB9_E5 texel. */
#define FORMAT_RGB9E5_MAX	65408.0f

/*
 * One channel set of a texel on its way from the application to the kept
 * form: floats for a normalized or float format (the depth in f[0]), and
 * the integers' bits for an integer format.
 */
struct format_texel {
	float f[4];
	uint32_t u[4];
};

/*
 * The formats a texture level can be kept in.  A sized internal format
 * may have two entries; the first whose Vulkan format the device samples
 * is taken (DEPTH_COMPONENT24 and DEPTH24_STENCIL8 fall back to 32-bit
 * float depth).  A depth and stencil format keeps 8 bytes a texel: the
 * depth as the depth aspect's copy has it (4 bytes), then the stencil
 * index and 3 bytes of 0 (texture.c and gles_texture_fetch copy the two
 * aspects apart).  The table never changes.
 */
static const struct gles_format format_table[] = {
	/* OpenGL ES 2's unsized formats: RGBA8, converted by texture.c. */
	{ GL_RGBA, GL_RGBA, VK_FORMAT_R8G8B8A8_UNORM, 4U, 4U, GLES_TEXEL_NORM, 1, 1, 0, 1 },
	{ GL_RGB, GL_RGB, VK_FORMAT_R8G8B8A8_UNORM, 4U, 4U, GLES_TEXEL_NORM, 1, 1, 0, 1 },
	{ GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, VK_FORMAT_R8G8B8A8_UNORM, 4U, 4U, GLES_TEXEL_NORM, 1, 1, 0, 0 },
	{ GL_LUMINANCE, GL_LUMINANCE, VK_FORMAT_R8G8B8A8_UNORM, 4U, 4U, GLES_TEXEL_NORM, 1, 1, 0, 0 },
	{ GL_ALPHA, GL_ALPHA, VK_FORMAT_R8G8B8A8_UNORM, 4U, 4U, GLES_TEXEL_NORM, 1, 1, 0, 0 },
	{ GL_BGRA_EXT, GL_BGRA_EXT, VK_FORMAT_R8G8B8A8_UNORM, 4U, 4U, GLES_TEXEL_NORM, 1, 1, 0, 1 },

	/* Normalized formats. */
	{ GL_R8, GL_RED, VK_FORMAT_R8_UNORM, 1U, 1U, GLES_TEXEL_NORM, 1, 0, 0, 1 },
	{ GL_R8_SNORM, GL_RED, VK_FORMAT_R8_SNORM, 1U, 1U, GLES_TEXEL_NORM, 1, 0, 0, 0 },
	{ GL_RG8, GL_RG, VK_FORMAT_R8G8_UNORM, 2U, 2U, GLES_TEXEL_NORM, 1, 0, 0, 1 },
	{ GL_RG8_SNORM, GL_RG, VK_FORMAT_R8G8_SNORM, 2U, 2U, GLES_TEXEL_NORM, 1, 0, 0, 0 },
	{ GL_RGB8, GL_RGB, VK_FORMAT_R8G8B8A8_UNORM, 4U, 4U, GLES_TEXEL_NORM, 1, 0, 0, 1 },
	{ GL_SRGB8, GL_RGB, VK_FORMAT_R8G8B8A8_SRGB, 4U, 4U, GLES_TEXEL_NORM, 1, 0, 0, 0 },
	{ GL_RGB565, GL_RGB, VK_FORMAT_R8G8B8A8_UNORM, 4U, 4U, GLES_TEXEL_NORM, 1, 0, 0, 1 },
	{ GL_RGB8_SNORM, GL_RGB, VK_FORMAT_R8G8B8A8_SNORM, 4U, 4U, GLES_TEXEL_NORM, 1, 0, 0, 0 },
	{ GL_RGBA8, GL_RGBA, VK_FORMAT_R8G8B8A8_UNORM, 4U, 4U, GLES_TEXEL_NORM, 1, 0, 0, 1 },
	{ GL_SRGB8_ALPHA8, GL_RGBA, VK_FORMAT_R8G8B8A8_SRGB, 4U, 4U, GLES_TEXEL_NORM, 1, 0, 0, 1 },
	{ GL_RGBA8_SNORM, GL_RGBA, VK_FORMAT_R8G8B8A8_SNORM, 4U, 4U, GLES_TEXEL_NORM, 1, 0, 0, 0 },
	{ GL_RGB5_A1, GL_RGBA, VK_FORMAT_R8G8B8A8_UNORM, 4U, 4U, GLES_TEXEL_NORM, 1, 0, 0, 1 },
	{ GL_RGBA4, GL_RGBA, VK_FORMAT_R8G8B8A8_UNORM, 4U, 4U, GLES_TEXEL_NORM, 1, 0, 0, 1 },
	{ GL_RGB10_A2, GL_RGBA, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4U, 4U, GLES_TEXEL_NORM, 1, 0, 0, 1 },

	/* Float formats (32-bit floats cannot be filtered in OpenGL ES 3). */
	{ GL_R16F, GL_RED, VK_FORMAT_R16_SFLOAT, 2U, 1U, GLES_TEXEL_FLOAT, 1, 0, 0, 1 },
	{ GL_R32F, GL_RED, VK_FORMAT_R32_SFLOAT, 4U, 1U, GLES_TEXEL_FLOAT, 0, 0, 0, 1 },
	{ GL_RG16F, GL_RG, VK_FORMAT_R16G16_SFLOAT, 4U, 2U, GLES_TEXEL_FLOAT, 1, 0, 0, 1 },
	{ GL_RG32F, GL_RG, VK_FORMAT_R32G32_SFLOAT, 8U, 2U, GLES_TEXEL_FLOAT, 0, 0, 0, 1 },
	{ GL_R11F_G11F_B10F, GL_RGB, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4U, 3U, GLES_TEXEL_FLOAT, 1, 0, 0, 1 },
	{ GL_RGB9_E5, GL_RGB, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4U, 3U, GLES_TEXEL_FLOAT, 1, 0, 0, 0 },
	{ GL_RGB16F, GL_RGB, VK_FORMAT_R16G16B16A16_SFLOAT, 8U, 4U, GLES_TEXEL_FLOAT, 1, 0, 0, 0 },
	{ GL_RGB32F, GL_RGB, VK_FORMAT_R32G32B32A32_SFLOAT, 16U, 4U, GLES_TEXEL_FLOAT, 0, 0, 0, 0 },
	{ GL_RGBA16F, GL_RGBA, VK_FORMAT_R16G16B16A16_SFLOAT, 8U, 4U, GLES_TEXEL_FLOAT, 1, 0, 0, 1 },
	{ GL_RGBA32F, GL_RGBA, VK_FORMAT_R32G32B32A32_SFLOAT, 16U, 4U, GLES_TEXEL_FLOAT, 0, 0, 0, 1 },

	/* Unsigned integer formats. */
	{ GL_R8UI, GL_RED_INTEGER, VK_FORMAT_R8_UINT, 1U, 1U, GLES_TEXEL_UINT, 0, 0, 0, 1 },
	{ GL_R16UI, GL_RED_INTEGER, VK_FORMAT_R16_UINT, 2U, 1U, GLES_TEXEL_UINT, 0, 0, 0, 1 },
	{ GL_R32UI, GL_RED_INTEGER, VK_FORMAT_R32_UINT, 4U, 1U, GLES_TEXEL_UINT, 0, 0, 0, 1 },
	{ GL_RG8UI, GL_RG_INTEGER, VK_FORMAT_R8G8_UINT, 2U, 2U, GLES_TEXEL_UINT, 0, 0, 0, 1 },
	{ GL_RG16UI, GL_RG_INTEGER, VK_FORMAT_R16G16_UINT, 4U, 2U, GLES_TEXEL_UINT, 0, 0, 0, 1 },
	{ GL_RG32UI, GL_RG_INTEGER, VK_FORMAT_R32G32_UINT, 8U, 2U, GLES_TEXEL_UINT, 0, 0, 0, 1 },
	{ GL_RGB8UI, GL_RGB_INTEGER, VK_FORMAT_R8G8B8A8_UINT, 4U, 4U, GLES_TEXEL_UINT, 0, 0, 0, 0 },
	{ GL_RGB16UI, GL_RGB_INTEGER, VK_FORMAT_R16G16B16A16_UINT, 8U, 4U, GLES_TEXEL_UINT, 0, 0, 0, 0 },
	{ GL_RGB32UI, GL_RGB_INTEGER, VK_FORMAT_R32G32B32A32_UINT, 16U, 4U, GLES_TEXEL_UINT, 0, 0, 0, 0 },
	{ GL_RGBA8UI, GL_RGBA_INTEGER, VK_FORMAT_R8G8B8A8_UINT, 4U, 4U, GLES_TEXEL_UINT, 0, 0, 0, 1 },
	{ GL_RGB10_A2UI, GL_RGBA_INTEGER, VK_FORMAT_A2B10G10R10_UINT_PACK32, 4U, 4U, GLES_TEXEL_UINT, 0, 0, 0, 1 },
	{ GL_RGBA16UI, GL_RGBA_INTEGER, VK_FORMAT_R16G16B16A16_UINT, 8U, 4U, GLES_TEXEL_UINT, 0, 0, 0, 1 },
	{ GL_RGBA32UI, GL_RGBA_INTEGER, VK_FORMAT_R32G32B32A32_UINT, 16U, 4U, GLES_TEXEL_UINT, 0, 0, 0, 1 },

	/* Signed integer formats. */
	{ GL_R8I, GL_RED_INTEGER, VK_FORMAT_R8_SINT, 1U, 1U, GLES_TEXEL_INT, 0, 0, 0, 1 },
	{ GL_R16I, GL_RED_INTEGER, VK_FORMAT_R16_SINT, 2U, 1U, GLES_TEXEL_INT, 0, 0, 0, 1 },
	{ GL_R32I, GL_RED_INTEGER, VK_FORMAT_R32_SINT, 4U, 1U, GLES_TEXEL_INT, 0, 0, 0, 1 },
	{ GL_RG8I, GL_RG_INTEGER, VK_FORMAT_R8G8_SINT, 2U, 2U, GLES_TEXEL_INT, 0, 0, 0, 1 },
	{ GL_RG16I, GL_RG_INTEGER, VK_FORMAT_R16G16_SINT, 4U, 2U, GLES_TEXEL_INT, 0, 0, 0, 1 },
	{ GL_RG32I, GL_RG_INTEGER, VK_FORMAT_R32G32_SINT, 8U, 2U, GLES_TEXEL_INT, 0, 0, 0, 1 },
	{ GL_RGB8I, GL_RGB_INTEGER, VK_FORMAT_R8G8B8A8_SINT, 4U, 4U, GLES_TEXEL_INT, 0, 0, 0, 0 },
	{ GL_RGB16I, GL_RGB_INTEGER, VK_FORMAT_R16G16B16A16_SINT, 8U, 4U, GLES_TEXEL_INT, 0, 0, 0, 0 },
	{ GL_RGB32I, GL_RGB_INTEGER, VK_FORMAT_R32G32B32A32_SINT, 16U, 4U, GLES_TEXEL_INT, 0, 0, 0, 0 },
	{ GL_RGBA8I, GL_RGBA_INTEGER, VK_FORMAT_R8G8B8A8_SINT, 4U, 4U, GLES_TEXEL_INT, 0, 0, 0, 1 },
	{ GL_RGBA16I, GL_RGBA_INTEGER, VK_FORMAT_R16G16B16A16_SINT, 8U, 4U, GLES_TEXEL_INT, 0, 0, 0, 1 },
	{ GL_RGBA32I, GL_RGBA_INTEGER, VK_FORMAT_R32G32B32A32_SINT, 16U, 4U, GLES_TEXEL_INT, 0, 0, 0, 1 },

	/* Depth formats, as their depth aspect is copied (24-bit depth in the low bits of a word). */
	{ GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, VK_FORMAT_D16_UNORM, 2U, 1U, GLES_TEXEL_DEPTH, 0, 0, 0, 1 },
	{ GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, VK_FORMAT_X8_D24_UNORM_PACK32, 4U, 1U, GLES_TEXEL_DEPTH, 0, 0, 0, 1 },
	{ GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, VK_FORMAT_D32_SFLOAT, 4U, 1U, GLES_TEXEL_DEPTH, 0, 0, 0, 1 },
	{ GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, VK_FORMAT_D32_SFLOAT, 4U, 1U, GLES_TEXEL_DEPTH, 0, 0, 0, 1 },
	{ GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, VK_FORMAT_D24_UNORM_S8_UINT, 8U, 1U, GLES_TEXEL_DEPTH, 0, 0, 1, 1 },
	{ GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, VK_FORMAT_D32_SFLOAT_S8_UINT, 8U, 1U, GLES_TEXEL_DEPTH, 0, 0, 1, 1 },
	{ GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, VK_FORMAT_D32_SFLOAT_S8_UINT, 8U, 1U, GLES_TEXEL_DEPTH, 0, 0, 1, 1 }
};

/*
 * Whether this process has reported an image the device does not make.
 *
 * The report is printed once, at the first such image, so that a program
 * that keeps asking for one does not fill its error output; zero until
 * then, set for the rest of the process.
 */
static int format_unmade_reported;

static int format_valid(const struct gles_format *storage, GLenum format, GLenum type, size_t *texel);
static size_t format_type_size(GLenum type);
static unsigned format_components(GLenum format);
static int format_integer_format(GLenum format);
static void format_decode(GLenum format, GLenum type, int integer, const unsigned char *source, struct format_texel *texel);
static void format_place(GLenum format, int integer, const float *values, const uint32_t *bits, unsigned count, struct format_texel *texel);
static void format_encode(const struct gles_format *storage, const struct format_texel *texel, unsigned char *out);
static void format_encode_stencil(const struct gles_format *storage, const struct format_texel *texel, unsigned char *out);
static void format_read_kept(const struct gles_format *storage, const unsigned char *kept, float *values);
static void format_read_integers(const struct gles_format *storage, const unsigned char *kept, uint32_t *values);
static uint16_t format_half(float value);
static uint32_t format_small_float(float value, unsigned mantissa_bits);
static float format_from_small_float(uint32_t bits, unsigned mantissa_bits);
static uint32_t format_rgb9e5(const float *values);
static float format_power(int exponent);
static float format_clamp(float value, float low, float high);
static uint32_t format_round(float value);

/*
 * Returns the format a texture level of an internal format given with a
 * type is kept in (an unsized DEPTH_COMPONENT or DEPTH_STENCIL takes its
 * size from the type), the first the device samples; NULL when the
 * internal format is not one or the device has none of its formats.
 */
const struct gles_format *
gles_format_find(
	struct gles_state *state,
	GLenum internal,
	GLenum type)
{
	uint32_t features;
	unsigned index;
	int unit_format;

	/* An unsized depth format: sized by its type. */
	if (internal == GL_DEPTH_COMPONENT) {
		internal = GL_DEPTH_COMPONENT24;
		if (type == GL_UNSIGNED_SHORT)
			internal = GL_DEPTH_COMPONENT16;
		if (type == GL_FLOAT)
			internal = GL_DEPTH_COMPONENT32F;
	} else if (internal == GL_DEPTH_STENCIL) {
		internal = GL_DEPTH24_STENCIL8;
		if (type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV)
			internal = GL_DEPTH32F_STENCIL8;
	}

	/* The first entry of the format whose Vulkan format the device samples. */
	for (index = 0U; index < sizeof(format_table) / sizeof(format_table[0]); index++) {
		if (format_table[index].internal != internal)
			continue;
		features = gles_image_features(state, format_table[index].vk);
		if ((features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0U)
			return &format_table[index];
	}

	/*
	 * Else, while the context has image units, the first entry of the
	 * format an image unit takes that the device stores (a texture draws
	 * read as black, texture.c).
	 */
	for (index = 0U; state->image_units != 0U && index < sizeof(format_table) / sizeof(format_table[0]); index++) {
		if (format_table[index].internal != internal)
			continue;
		unit_format = gles_image_unit_format(format_table[index].vk);
		if (!unit_format)
			continue;
		features = gles_image_features(state, format_table[index].vk);
		if ((features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0U)
			return &format_table[index];
	}

	/* None. */
	return NULL;
}

/*
 * Returns OpenGL ES 2's RGBA8 format (unsized RGBA), which a level has
 * when nothing else gives one.
 */
const struct gles_format *
gles_format_rgba8(void)
{
	/* The table's first entry. */
	return &format_table[0];
}

/*
 * Returns the device's optimal-tiling features of a Vulkan format, asking
 * the device once per format (0 for a format outside the core ones).
 */
uint32_t
gles_image_features(
	struct gles_state *state,
	VkFormat format)
{
	VkFormatProperties properties;

	/* Only the core formats are cached. */
	if (format == VK_FORMAT_UNDEFINED || (unsigned)format >= GLES_FORMATS)
		return 0U;

	/* Asked once. */
	if (!state->image_asked[format]) {
		vkGetPhysicalDeviceFormatProperties(state->display->physical, format, &properties);
		state->image_features[format] = properties.optimalTilingFeatures;
		state->image_asked[format] = 1U;
	}

	/* The features. */
	return state->image_features[format];
}

/*
 * Reports whether the device makes an image of a description: its format,
 * type, tiling, usage and flags, with an extent, levels, layers and
 * samples within what the device allows of them.  Returns 1 or 0.
 *
 * Asked before an image is made, so that an image the device does not
 * have is a texture that reads as black or a framebuffer that is
 * unsupported, rather than a failed Vulkan create.
 */
int
gles_image_supported(
	struct gles_state *state,
	const VkImageCreateInfo *create)
{
	VkImageFormatProperties properties;
	VkResult result;

	/* What the device allows of the format in this type, tiling, usage and flags. */
	memset(&properties, 0, sizeof(properties));
	result = vkGetPhysicalDeviceImageFormatProperties(
		state->display->physical,
		create->format,
		create->imageType,
		create->tiling,
		create->usage,
		create->flags,
		&properties);
	if (result != VK_SUCCESS)
		return 0;

	/* The extent within the largest. */
	if (create->extent.width > properties.maxExtent.width)
		return 0;
	if (create->extent.height > properties.maxExtent.height)
		return 0;
	if (create->extent.depth > properties.maxExtent.depth)
		return 0;

	/* The levels and the layers within the most. */
	if (create->mipLevels > properties.maxMipLevels)
		return 0;
	if (create->arrayLayers > properties.maxArrayLayers)
		return 0;

	/* A sample count the device has. */
	if ((properties.sampleCounts & (VkSampleCountFlags)create->samples) == 0U)
		return 0;

	/* Succeeded: the device makes the image. */
	return 1;
}

/*
 * Reports, once a process, that the device makes no image of a Vulkan
 * format for what the application asked (a texture, a renderbuffer, a
 * blit): the call fails with an error and the scene goes on without it.
 */
void
gles_image_unmade(
	VkFormat format,
	const char *what)
{
	/* Only the first is told. */
	if (format_unmade_reported)
		return;
	format_unmade_reported = 1;

	/* Succeeded: the report. */
	fprintf(stderr, "libGLESv2: the device makes no image of Vulkan format %d for %s; the call fails\n", (int)format, what);
	return;
}

/*
 * Converts a rectangle of the application's texels (a format and a type,
 * rows at an alignment; NULL pixels give zeros) into a format's kept
 * form.  Returns GL_NO_ERROR with the new texels in *out, or the error:
 * GL_INVALID_OPERATION for a format and type the kept form cannot come
 * from, GL_OUT_OF_MEMORY.
 */
GLenum
gles_texels_convert(
	const struct gles_format *storage,
	GLenum format,
	GLenum type,
	GLsizei width,
	GLsizei height,
	GLint alignment,
	const void *pixels,
	unsigned char **out)
{
	struct format_texel texel;
	const unsigned char *row;
	unsigned char *converted;
	size_t size;
	size_t stride;
	GLsizei x;
	GLsizei y;
	int valid;
	int integer;

	/* A format and type this kept form comes from, and the bytes of one of their texels. */
	valid = format_valid(storage, format, type, &size);
	if (!valid)
		return GL_INVALID_OPERATION;

	/* The kept texels (zeros without pixels). */
	converted = calloc((size_t)width * (size_t)height * storage->bytes + 1U, 1U);
	if (converted == NULL)
		return GL_OUT_OF_MEMORY;
	*out = converted;
	if (pixels == NULL)
		return GL_NO_ERROR;

	/* Each row at the alignment, each texel read and written in the kept form. */
	integer = format_integer_format(format);
	stride = ((size_t)width * size + (size_t)alignment - 1U) / (size_t)alignment * (size_t)alignment;
	for (y = 0; y < height; y++) {
		row = (const unsigned char *)pixels + (size_t)y * stride;
		for (x = 0; x < width; x++) {
			format_decode(format, type, integer, row + (size_t)x * size, &texel);
			format_encode(storage, &texel, converted + ((size_t)y * (size_t)width + (size_t)x) * storage->bytes);
		}
	}

	/* Succeeded: the kept texels. */
	return GL_NO_ERROR;
}

/*
 * Converts RGBA8 texels (a framebuffer's) into a format's kept form.
 * Returns GL_NO_ERROR with the new texels in *out, or the error:
 * GL_INVALID_OPERATION for an integer or depth format, GL_OUT_OF_MEMORY.
 */
GLenum
gles_texels_from_rgba8(
	const struct gles_format *storage,
	const unsigned char *rgba,
	size_t count,
	unsigned char **out)
{
	struct format_texel texel;
	unsigned char *converted;
	size_t index;
	unsigned channel;

	/* A normalized or float format only. */
	if (storage->kind != GLES_TEXEL_NORM && storage->kind != GLES_TEXEL_FLOAT)
		return GL_INVALID_OPERATION;

	/* The kept texels. */
	converted = malloc(count * storage->bytes + 1U);
	if (converted == NULL)
		return GL_OUT_OF_MEMORY;

	/* Each texel's four bytes as normalized channels. */
	memset(&texel, 0, sizeof(texel));
	for (index = 0U; index < count; index++) {
		for (channel = 0U; channel < 4U; channel++)
			texel.f[channel] = (float)rgba[index * 4U + channel] / 255.0f;
		format_encode(storage, &texel, converted + index * storage->bytes);
	}

	/* Succeeded: the kept texels. */
	*out = converted;
	return GL_NO_ERROR;
}

/*
 * Makes the level below one of a normalized or float format by halving it
 * with a box filter (the level is at least 1x1).  The level has slices
 * one after another: a 3D texture's are halved too (halve_depth nonzero),
 * a 2D array's layers each on their own.  Returns 0 with the new texels
 * and size, or -1 when there is no memory.
 */
int
gles_texels_halve(
	const struct gles_format *format,
	const unsigned char *source,
	int source_width,
	int source_height,
	int source_depth,
	int halve_depth,
	unsigned char **out,
	int *width,
	int *height,
	int *depth)
{
	struct format_texel texel;
	const unsigned char *read;
	unsigned char *halved;
	float values[4];
	float sum[4];
	unsigned channel;
	int x;
	int y;
	int z;
	int dx;
	int dy;
	int dz;
	int sx;
	int sy;
	int sz;
	int slices;

	/* The halved size, at least 1; layers of an array stay as many. */
	*width = source_width / 2;
	if (*width < 1)
		*width = 1;
	*height = source_height / 2;
	if (*height < 1)
		*height = 1;
	*depth = source_depth;
	slices = 1;
	if (halve_depth) {
		*depth = source_depth / 2;
		if (*depth < 1)
			*depth = 1;
		slices = 2;
	}

	/* Its texels. */
	halved = malloc((size_t)*width * (size_t)*height * (size_t)*depth * format->bytes + 1U);
	if (halved == NULL)
		return -1;

	/* Each texel: the mean of the (up to) four, or eight in 3D, above it, read from and written in the kept form. */
	memset(&texel, 0, sizeof(texel));
	for (z = 0; z < *depth; z++) {
		for (y = 0; y < *height; y++) {
			for (x = 0; x < *width; x++) {
				memset(sum, 0, sizeof(sum));
				for (dz = 0; dz < slices; dz++) {
					for (dy = 0; dy < 2; dy++) {
						for (dx = 0; dx < 2; dx++) {
							/* The texel above, clamped to the level's edges. */
							sx = x * 2 + dx;
							sy = y * 2 + dy;
							sz = z * slices + dz;
							if (sx >= source_width)
								sx = source_width - 1;
							if (sy >= source_height)
								sy = source_height - 1;
							if (sz >= source_depth)
								sz = source_depth - 1;

							/* Its channels into the sum. */
							read = source + (((size_t)sz * (size_t)source_height + (size_t)sy) * (size_t)source_width + (size_t)sx) * format->bytes;
							format_read_kept(format, read, values);
							for (channel = 0U; channel < 4U; channel++)
								sum[channel] += values[channel];
						}
					}
				}

				/* The mean. */
				for (channel = 0U; channel < 4U; channel++)
					texel.f[channel] = sum[channel] / (float)(4 * slices);
				format_encode(format, &texel, halved + (((size_t)z * (size_t)*height + (size_t)y) * (size_t)*width + (size_t)x) * format->bytes);
			}
		}
	}

	/* Succeeded: the level. */
	*out = halved;
	return 0;
}

/*
 * Converts a float to a half float, rounding to the nearest (etc2.c's
 * 11-bit channels are kept so).
 */
uint16_t
gles_half_from_float(
	float value)
{
	uint16_t half;

	/* The conversion the texel formats use. */
	half = format_half(value);

	/* Succeeded: the half float. */
	return half;
}

/*
 * Converts a half float (IEEE binary16) to a float.
 */
float
gles_half_float(
	uint16_t half)
{
	uint32_t sign;
	uint32_t exponent;
	uint32_t mantissa;
	uint32_t bits;
	float value;

	/* The three fields. */
	sign = (uint32_t)(half >> 15) << 31;
	exponent = (half >> 10) & 0x1fU;
	mantissa = half & 0x3ffU;

	/* Zero and the subnormals: the mantissa scaled by 2^-24. */
	if (exponent == 0U) {
		value = (float)mantissa / 16777216.0f;
		if (sign != 0U)
			value = -value;
		return value;
	}

	/* Infinity and NaN keep their mantissa; the rest move the exponent's bias from 15 to 127. */
	if (exponent == 0x1fU) {
		bits = sign | 0x7f800000U | (mantissa << 13);
	} else {
		bits = sign | ((exponent + 112U) << 23) | (mantissa << 13);
	}

	/* The float of those bits. */
	memcpy(&value, &bits, 4U);
	return value;
}

/*
 * Returns the bytes one of the application's pixels of a format and type
 * takes (a packed type's whole word, else the components' bytes); 0 when
 * the format or the type is not one.
 */
size_t
gles_pixel_size(
	GLenum format,
	GLenum type)
{
	unsigned components;

	/* A packed type holds a whole pixel. */
	switch (type) {
	case GL_UNSIGNED_SHORT_5_6_5:
	case GL_UNSIGNED_SHORT_4_4_4_4:
	case GL_UNSIGNED_SHORT_5_5_5_1:
		return 2U;
	case GL_UNSIGNED_INT_2_10_10_10_REV:
	case GL_UNSIGNED_INT_10F_11F_11F_REV:
	case GL_UNSIGNED_INT_5_9_9_9_REV:
	case GL_UNSIGNED_INT_24_8:
		return 4U;
	case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
		return 8U;
	default:
		break;
	}

	/* A component type: one per component of the format. */
	components = format_components(format);

	/* Succeeded: the components' bytes (0 when either is not one). */
	return format_type_size(type) * components;
}

/*
 * Returns the format a renderbuffer of an internal format is kept in: the
 * first entry a framebuffer object may draw into whose Vulkan format the
 * device can attach (as colour, or as depth and stencil); NULL when the
 * internal format is not a renderable one or the device has none.
 */
const struct gles_format *
gles_format_renderable(
	struct gles_state *state,
	GLenum internal)
{
	uint32_t features;
	uint32_t wanted;
	unsigned index;

	/* The first renderable entry of the format the device attaches. */
	for (index = 0U; index < sizeof(format_table) / sizeof(format_table[0]); index++) {
		if (format_table[index].internal != internal || !format_table[index].renderable)
			continue;

		/* A colour attachment, or a depth and stencil one. */
		wanted = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
		if (format_table[index].kind == GLES_TEXEL_DEPTH)
			wanted = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
		features = gles_image_features(state, format_table[index].vk);
		if ((features & wanted) != 0U)
			return &format_table[index];
	}

	/* None. */
	return NULL;
}

/*
 * Reports whether glReadPixels may read texels of a kept format as a
 * format and type: normalized ones as RGBA and unsigned bytes (RGB10_A2
 * also packed), float ones as RGBA floats, integer ones as RGBA_INTEGER
 * ints or unsigned ints.
 */
int
gles_read_format_ok(
	const struct gles_format *storage,
	GLenum format,
	GLenum type)
{
	GLenum own_format;
	GLenum own_type;

	/* Normalized texels as unsigned bytes. */
	if (storage->kind == GLES_TEXEL_NORM && format == GL_RGBA && type == GL_UNSIGNED_BYTE)
		return 1;

	/* The format's own pair. */
	gles_read_format(storage, &own_format, &own_type);
	if (format == own_format && type == own_type)
		return 1;

	/* Any other pair is refused. */
	return 0;
}

/*
 * Returns the format and type glReadPixels reads a kept format's texels
 * as besides RGBA and unsigned bytes (GL_IMPLEMENTATION_COLOR_READ_FORMAT
 * and _TYPE).
 */
void
gles_read_format(
	const struct gles_format *storage,
	GLenum *format,
	GLenum *type)
{
	/* Normalized texels: bytes, or RGB10_A2's own packing. */
	*format = GL_RGBA;
	*type = GL_UNSIGNED_BYTE;
	if (storage->vk == VK_FORMAT_A2B10G10R10_UNORM_PACK32)
		*type = GL_UNSIGNED_INT_2_10_10_10_REV;

	/* Floats, and integers as 32-bit ones. */
	if (storage->kind == GLES_TEXEL_FLOAT)
		*type = GL_FLOAT;
	if (storage->kind == GLES_TEXEL_INT) {
		*format = GL_RGBA_INTEGER;
		*type = GL_INT;
	}

	/* Unsigned integers as 32-bit ones. */
	if (storage->kind == GLES_TEXEL_UINT) {
		*format = GL_RGBA_INTEGER;
		*type = GL_UNSIGNED_INT;
	}
}

/*
 * Converts kept texels into the application's pixels of a format and type
 * glReadPixels may read them as (gles_read_format_ok): RGBA unsigned
 * bytes, packed 2_10_10_10_REV, floats, or 32-bit integers.
 */
void
gles_texels_read(
	const struct gles_format *storage,
	const unsigned char *kept,
	size_t count,
	GLenum format,
	GLenum type,
	unsigned char *out)
{
	float values[4];
	uint32_t integers[4];
	uint32_t word;
	size_t index;
	unsigned channel;

	/* Each texel. */
	(void)format;
	for (index = 0U; index < count; index++) {
		/* Integers as 32-bit words. */
		if (type == GL_INT || type == GL_UNSIGNED_INT) {
			format_read_integers(storage, kept + index * storage->bytes, integers);
			memcpy(out + index * 16U, integers, 16U);
			continue;
		}

		/* The others through floats (a format without alpha reads it as 1, though four channels are kept). */
		format_read_kept(storage, kept + index * storage->bytes, values);
		if (storage->base == GL_RGB)
			values[3] = 1.0f;
		switch (type) {
		case GL_FLOAT:
			memcpy(out + index * 16U, values, 16U);
			break;
		case GL_UNSIGNED_INT_2_10_10_10_REV:
			word = format_round(format_clamp(values[0], 0.0f, 1.0f) * 1023.0f);
			word |= format_round(format_clamp(values[1], 0.0f, 1.0f) * 1023.0f) << 10;
			word |= format_round(format_clamp(values[2], 0.0f, 1.0f) * 1023.0f) << 20;
			word |= format_round(format_clamp(values[3], 0.0f, 1.0f) * 3.0f) << 30;
			memcpy(out + index * 4U, &word, 4U);
			break;
		default:
			for (channel = 0U; channel < 4U; channel++)
				out[index * 4U + channel] = (unsigned char)format_round(format_clamp(values[channel], 0.0f, 1.0f) * 255.0f);
			break;
		}
	}
}

/*
 * Reports whether a kept form can come from a format and type (integers
 * from an integer format, depth from a depth format, colours from a
 * colour format; packed types with the formats they pack), and the bytes
 * of one of their texels.
 */
static int
format_valid(
	const struct gles_format *storage,
	GLenum format,
	GLenum type,
	size_t *texel)
{
	unsigned components;
	int integer;

	/* The format's kind must be the kept form's. */
	components = format_components(format);
	integer = format_integer_format(format);
	if (components == 0U)
		return 0;
	if (storage->kind == GLES_TEXEL_INT || storage->kind == GLES_TEXEL_UINT) {
		if (!integer)
			return 0;
	} else if (storage->kind == GLES_TEXEL_DEPTH) {
		if (format != GL_DEPTH_COMPONENT && format != GL_DEPTH_STENCIL)
			return 0;
	} else if (integer || format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL) {
		return 0;
	}

	/* A packed type: its size, with the formats it packs. */
	switch (type) {
	case GL_UNSIGNED_SHORT_5_6_5:
		*texel = 2U;
		if (format != GL_RGB)
			return 0;
		return 1;
	case GL_UNSIGNED_SHORT_4_4_4_4:
	case GL_UNSIGNED_SHORT_5_5_5_1:
		*texel = 2U;
		if (format != GL_RGBA)
			return 0;
		return 1;
	case GL_UNSIGNED_INT_2_10_10_10_REV:
		*texel = 4U;
		if (format != GL_RGBA && format != GL_RGBA_INTEGER)
			return 0;
		return 1;
	case GL_UNSIGNED_INT_10F_11F_11F_REV:
	case GL_UNSIGNED_INT_5_9_9_9_REV:
		*texel = 4U;
		if (format != GL_RGB)
			return 0;
		return 1;
	case GL_UNSIGNED_INT_24_8:
		*texel = 4U;
		if (format != GL_DEPTH_STENCIL)
			return 0;
		return 1;
	case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
		*texel = 8U;
		if (format != GL_DEPTH_STENCIL)
			return 0;
		return 1;
	default:
		break;
	}

	/* A depth and stencil format takes only the packed types. */
	if (format == GL_DEPTH_STENCIL)
		return 0;

	/* A component type: floats are not integers. */
	*texel = format_type_size(type) * components;
	if (*texel == 0U)
		return 0;
	if (integer && (type == GL_FLOAT || type == GL_HALF_FLOAT))
		return 0;

	/* Succeeded: the kept form comes from them. */
	return 1;
}

/* Returns the bytes of one component of a component type, 0 for any other type. */
static size_t
format_type_size(
	GLenum type)
{
	/* The component types. */
	switch (type) {
	case GL_UNSIGNED_BYTE:
	case GL_BYTE:
		return 1U;
	case GL_UNSIGNED_SHORT:
	case GL_SHORT:
	case GL_HALF_FLOAT:
		return 2U;
	case GL_UNSIGNED_INT:
	case GL_INT:
	case GL_FLOAT:
		return 4U;
	default:
		break;
	}

	/* Not a component type. */
	return 0U;
}

/* Returns how many components an application's format has, 0 for a name that is not a format. */
static unsigned
format_components(
	GLenum format)
{
	/* The formats of OpenGL ES 3 (and BGRA). */
	switch (format) {
	case GL_RED:
	case GL_RED_INTEGER:
	case GL_LUMINANCE:
	case GL_ALPHA:
	case GL_DEPTH_COMPONENT:
	case GL_DEPTH_STENCIL:
		return 1U;
	case GL_RG:
	case GL_RG_INTEGER:
	case GL_LUMINANCE_ALPHA:
		return 2U;
	case GL_RGB:
	case GL_RGB_INTEGER:
		return 3U;
	case GL_RGBA:
	case GL_RGBA_INTEGER:
	case GL_BGRA_EXT:
		return 4U;
	default:
		break;
	}

	/* Not a format. */
	return 0U;
}

/* Reports whether an application's format holds integers read as they are. */
static int
format_integer_format(
	GLenum format)
{
	/* The integer formats. */
	switch (format) {
	case GL_RED_INTEGER:
	case GL_RG_INTEGER:
	case GL_RGB_INTEGER:
	case GL_RGBA_INTEGER:
		return 1;
	default:
		break;
	}

	/* The others are normalized, float or depth. */
	return 0;
}

/*
 * Reads one of the application's texels of a format and type (valid for
 * each other, format_valid) into four channels.
 */
static void
format_decode(
	GLenum format,
	GLenum type,
	int integer,
	const unsigned char *source,
	struct format_texel *texel)
{
	float values[4];
	uint32_t bits[4];
	uint32_t word;
	uint16_t half;
	int16_t signed_short;
	int32_t signed_int;
	int8_t signed_byte;
	float scale;
	unsigned count;
	unsigned index;

	/* Nothing read yet. */
	memset(values, 0, sizeof(values));
	memset(bits, 0, sizeof(bits));
	count = format_components(format);

	/* A packed type holds its own channels. */
	switch (type) {
	case GL_UNSIGNED_SHORT_5_6_5:
		memcpy(&half, source, 2U);
		values[0] = (float)((half >> 11) & 31U) / 31.0f;
		values[1] = (float)((half >> 5) & 63U) / 63.0f;
		values[2] = (float)(half & 31U) / 31.0f;
		format_place(format, 0, values, bits, 3U, texel);
		return;
	case GL_UNSIGNED_SHORT_4_4_4_4:
		memcpy(&half, source, 2U);
		for (index = 0U; index < 4U; index++)
			values[index] = (float)((half >> (12U - index * 4U)) & 15U) / 15.0f;
		format_place(format, 0, values, bits, 4U, texel);
		return;
	case GL_UNSIGNED_SHORT_5_5_5_1:
		memcpy(&half, source, 2U);
		for (index = 0U; index < 3U; index++)
			values[index] = (float)((half >> (11U - index * 5U)) & 31U) / 31.0f;
		values[3] = (float)(half & 1U);
		format_place(format, 0, values, bits, 4U, texel);
		return;
	case GL_UNSIGNED_INT_2_10_10_10_REV:
		memcpy(&word, source, 4U);
		for (index = 0U; index < 3U; index++) {
			bits[index] = (word >> (index * 10U)) & 1023U;
			values[index] = (float)bits[index] / 1023.0f;
		}

		/* The two-bit alpha. */
		bits[3] = word >> 30;
		values[3] = (float)bits[3] / 3.0f;
		format_place(format, integer, values, bits, 4U, texel);
		return;
	case GL_UNSIGNED_INT_10F_11F_11F_REV:
		memcpy(&word, source, 4U);
		values[0] = format_from_small_float(word & 0x7ffU, 6U);
		values[1] = format_from_small_float((word >> 11) & 0x7ffU, 6U);
		values[2] = format_from_small_float(word >> 22, 5U);
		format_place(format, 0, values, bits, 3U, texel);
		return;
	case GL_UNSIGNED_INT_5_9_9_9_REV:
		memcpy(&word, source, 4U);
		scale = format_power((int)(word >> 27) - 15 - 9);
		for (index = 0U; index < 3U; index++)
			values[index] = (float)((word >> (index * 9U)) & 511U) * scale;
		format_place(format, 0, values, bits, 3U, texel);
		return;
	case GL_UNSIGNED_INT_24_8:
		/* The depth in the top 24 bits, the stencil index in the low 8 (kept in u[1]). */
		memcpy(&word, source, 4U);
		values[0] = (float)(word >> 8) / 16777215.0f;
		format_place(format, 0, values, bits, 1U, texel);
		texel->u[1] = word & 0xffU;
		return;
	case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
		/* A float depth, then a word with the stencil index in its low 8 bits. */
		memcpy(&values[0], source, 4U);
		format_place(format, 0, values, bits, 1U, texel);
		memcpy(&word, source + 4, 4U);
		texel->u[1] = word & 0xffU;
		return;
	default:
		break;
	}

	/* Component types: each component, normalized unless the format holds integers. */
	for (index = 0U; index < count; index++) {
		switch (type) {
		case GL_UNSIGNED_BYTE:
			bits[index] = source[index];
			values[index] = (float)source[index] / 255.0f;
			break;
		case GL_BYTE:
			memcpy(&signed_byte, source + index, 1U);
			bits[index] = (uint32_t)(int32_t)signed_byte;
			values[index] = format_clamp((float)signed_byte / 127.0f, -1.0f, 1.0f);
			break;
		case GL_UNSIGNED_SHORT:
			memcpy(&half, source + index * 2U, 2U);
			bits[index] = half;
			values[index] = (float)half / 65535.0f;
			break;
		case GL_SHORT:
			memcpy(&signed_short, source + index * 2U, 2U);
			bits[index] = (uint32_t)(int32_t)signed_short;
			values[index] = format_clamp((float)signed_short / 32767.0f, -1.0f, 1.0f);
			break;
		case GL_UNSIGNED_INT:
			memcpy(&bits[index], source + index * 4U, 4U);
			values[index] = (float)((double)bits[index] / 4294967295.0);
			break;
		case GL_INT:
			memcpy(&signed_int, source + index * 4U, 4U);
			bits[index] = (uint32_t)signed_int;
			values[index] = format_clamp((float)((double)signed_int / 2147483647.0), -1.0f, 1.0f);
			break;
		case GL_HALF_FLOAT:
			memcpy(&half, source + index * 2U, 2U);
			values[index] = gles_half_float(half);
			break;
		default:
			memcpy(&values[index], source + index * 4U, 4U);
			break;
		}
	}

	/* The components placed in their channels. */
	format_place(format, integer, values, bits, count, texel);
}

/*
 * Places read components in the channels a format gives them: red,
 * green, blue, alpha in order, luminance in all three colours, alpha
 * alone, BGRA turned round; missing colours are 0 and a missing alpha 1.
 */
static void
format_place(
	GLenum format,
	int integer,
	const float *values,
	const uint32_t *bits,
	unsigned count,
	struct format_texel *texel)
{
	unsigned index;

	/* Missing colours 0, alpha 1. */
	memset(texel, 0, sizeof(*texel));
	texel->f[3] = 1.0f;
	texel->u[3] = 1U;

	/* The channels the format names. */
	switch (format) {
	case GL_LUMINANCE:
	case GL_LUMINANCE_ALPHA:
		for (index = 0U; index < 3U; index++)
			texel->f[index] = values[0];
		if (format == GL_LUMINANCE_ALPHA)
			texel->f[3] = values[1];
		return;
	case GL_ALPHA:
		texel->f[3] = values[0];
		return;
	case GL_BGRA_EXT:
		texel->f[0] = values[2];
		texel->f[1] = values[1];
		texel->f[2] = values[0];
		texel->f[3] = values[3];
		return;
	default:
		break;
	}

	/* Red, green, blue, alpha in order: floats, or the integers' bits. */
	for (index = 0U; index < count && index < 4U; index++) {
		texel->f[index] = values[index];
		if (integer)
			texel->u[index] = bits[index];
	}
}

/* Writes a texel's channels in a format's kept form. */
static void
format_encode(
	const struct gles_format *storage,
	const struct format_texel *texel,
	unsigned char *out)
{
	uint16_t half;
	uint16_t depth16;
	uint32_t word;
	uint32_t channel_bits;
	int32_t signed_value;
	float value;
	unsigned index;

	/* The kept format. */
	switch (storage->vk) {
	case VK_FORMAT_R8_UNORM:
	case VK_FORMAT_R8G8_UNORM:
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
		for (index = 0U; index < storage->components; index++)
			out[index] = (unsigned char)format_round(format_clamp(texel->f[index], 0.0f, 1.0f) * 255.0f);
		return;
	case VK_FORMAT_R8_SNORM:
	case VK_FORMAT_R8G8_SNORM:
	case VK_FORMAT_R8G8B8A8_SNORM:
		for (index = 0U; index < storage->components; index++) {
			value = format_clamp(texel->f[index], -1.0f, 1.0f) * 127.0f;

			/* Rounded away from zero on either side. */
			if (value < 0.0f) {
				signed_value = -(int32_t)format_round(-value);
			} else {
				signed_value = (int32_t)format_round(value);
			}

			/* Stored as a signed byte. */
			out[index] = (unsigned char)(int8_t)signed_value;
		}

		/* Converted. */
		return;
	case VK_FORMAT_R16_SFLOAT:
	case VK_FORMAT_R16G16_SFLOAT:
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		for (index = 0U; index < storage->components; index++) {
			half = format_half(texel->f[index]);
			memcpy(out + index * 2U, &half, 2U);
		}

		/* Converted. */
		return;
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_R32G32_SFLOAT:
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		memcpy(out, texel->f, storage->components * 4U);
		return;
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		word = format_small_float(texel->f[0], 6U);
		word |= format_small_float(texel->f[1], 6U) << 11;
		word |= format_small_float(texel->f[2], 5U) << 22;
		memcpy(out, &word, 4U);
		return;
	case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
		word = format_rgb9e5(texel->f);
		memcpy(out, &word, 4U);
		return;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		word = 0U;
		for (index = 0U; index < 3U; index++)
			word |= format_round(format_clamp(texel->f[index], 0.0f, 1.0f) * 1023.0f) << (index * 10U);
		word |= format_round(format_clamp(texel->f[3], 0.0f, 1.0f) * 3.0f) << 30;
		memcpy(out, &word, 4U);
		return;
	case VK_FORMAT_A2B10G10R10_UINT_PACK32:
		word = 0U;
		for (index = 0U; index < 3U; index++)
			word |= (texel->u[index] & 1023U) << (index * 10U);
		word |= (texel->u[3] & 3U) << 30;
		memcpy(out, &word, 4U);
		return;
	case VK_FORMAT_D16_UNORM:
		depth16 = (uint16_t)format_round(format_clamp(texel->f[0], 0.0f, 1.0f) * 65535.0f);
		memcpy(out, &depth16, 2U);
		return;
	case VK_FORMAT_X8_D24_UNORM_PACK32:
	case VK_FORMAT_D24_UNORM_S8_UINT:
		word = format_round(format_clamp(texel->f[0], 0.0f, 1.0f) * 16777215.0f);
		memcpy(out, &word, 4U);
		format_encode_stencil(storage, texel, out);
		return;
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		value = format_clamp(texel->f[0], 0.0f, 1.0f);
		memcpy(out, &value, 4U);
		format_encode_stencil(storage, texel, out);
		return;
	default:
		break;
	}

	/* An integer format: each channel's bits cut to the channel's size (8, 16 or 32 bits). */
	for (index = 0U; index < storage->components; index++) {
		channel_bits = texel->u[index];
		if (storage->bytes == storage->components) {
			out[index] = (unsigned char)channel_bits;
		} else if (storage->bytes == storage->components * 2U) {
			half = (uint16_t)channel_bits;
			memcpy(out + index * 2U, &half, 2U);
		} else {
			memcpy(out + index * 4U, &channel_bits, 4U);
		}
	}
}

/* Writes a texel's stencil index after its depth in a depth and stencil format's kept form (nothing for a depth format). */
static void
format_encode_stencil(
	const struct gles_format *storage,
	const struct format_texel *texel,
	unsigned char *out)
{
	/* A depth format has no stencil. */
	if (!storage->stencil)
		return;

	/* The index, then 3 bytes of 0. */
	out[4] = (unsigned char)(texel->u[1] & 0xffU);
	out[5] = 0U;
	out[6] = 0U;
	out[7] = 0U;
}

/* Reads a kept texel of a normalized or float format as four floats (missing colours 0, alpha 1). */
static void
format_read_kept(
	const struct gles_format *storage,
	const unsigned char *kept,
	float *values)
{
	uint16_t half;
	uint32_t word;
	float scale;
	int8_t signed_byte;
	unsigned index;

	/* Missing colours 0, alpha 1. */
	values[0] = 0.0f;
	values[1] = 0.0f;
	values[2] = 0.0f;
	values[3] = 1.0f;

	/* The kept format. */
	switch (storage->vk) {
	case VK_FORMAT_R8_SNORM:
	case VK_FORMAT_R8G8_SNORM:
	case VK_FORMAT_R8G8B8A8_SNORM:
		for (index = 0U; index < storage->components; index++) {
			memcpy(&signed_byte, kept + index, 1U);
			values[index] = format_clamp((float)signed_byte / 127.0f, -1.0f, 1.0f);
		}

		/* Converted. */
		return;
	case VK_FORMAT_R16_SFLOAT:
	case VK_FORMAT_R16G16_SFLOAT:
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		for (index = 0U; index < storage->components; index++) {
			memcpy(&half, kept + index * 2U, 2U);
			values[index] = gles_half_float(half);
		}

		/* Converted. */
		return;
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_R32G32_SFLOAT:
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		memcpy(values, kept, storage->components * 4U);
		return;
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		memcpy(&word, kept, 4U);
		values[0] = format_from_small_float(word & 0x7ffU, 6U);
		values[1] = format_from_small_float((word >> 11) & 0x7ffU, 6U);
		values[2] = format_from_small_float(word >> 22, 5U);
		return;
	case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
		memcpy(&word, kept, 4U);
		scale = format_power((int)(word >> 27) - 15 - 9);
		for (index = 0U; index < 3U; index++)
			values[index] = (float)((word >> (index * 9U)) & 511U) * scale;
		return;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		memcpy(&word, kept, 4U);
		for (index = 0U; index < 3U; index++)
			values[index] = (float)((word >> (index * 10U)) & 1023U) / 1023.0f;
		values[3] = (float)(word >> 30) / 3.0f;
		return;
	default:
		break;
	}

	/* An 8-bit unsigned normalized format (sRGB kept as its bytes). */
	for (index = 0U; index < storage->components; index++)
		values[index] = (float)kept[index] / 255.0f;
}

/* Reads a kept texel of an integer format as four 32-bit integers (missing colours 0, alpha 1; signed ones extended). */
static void
format_read_integers(
	const struct gles_format *storage,
	const unsigned char *kept,
	uint32_t *values)
{
	uint16_t half_word;
	uint32_t word;
	int16_t signed_half;
	int8_t signed_byte;
	unsigned size;
	unsigned index;

	/* Missing colours 0, alpha 1. */
	values[0] = 0U;
	values[1] = 0U;
	values[2] = 0U;
	values[3] = 1U;

	/* The packed 10-bit format. */
	if (storage->vk == VK_FORMAT_A2B10G10R10_UINT_PACK32) {
		memcpy(&word, kept, 4U);
		for (index = 0U; index < 3U; index++)
			values[index] = (word >> (index * 10U)) & 1023U;
		values[3] = word >> 30;
		return;
	}

	/* Each component of 8, 16 or 32 bits (a three-component format keeps four: the fourth is not GL's). */
	size = storage->bytes / storage->components;
	for (index = 0U; index < storage->components; index++) {
		if (size == 1U && storage->kind == GLES_TEXEL_INT) {
			memcpy(&signed_byte, kept + index, 1U);
			values[index] = (uint32_t)(int32_t)signed_byte;
		} else if (size == 1U) {
			values[index] = kept[index];
		} else if (size == 2U && storage->kind == GLES_TEXEL_INT) {
			memcpy(&signed_half, kept + index * 2U, 2U);
			values[index] = (uint32_t)(int32_t)signed_half;
		} else if (size == 2U) {
			memcpy(&half_word, kept + index * 2U, 2U);
			values[index] = half_word;
		} else {
			memcpy(&values[index], kept + index * 4U, 4U);
		}
	}

	/* A format without alpha reads it as 1. */
	if (storage->base == GL_RGB_INTEGER)
		values[3] = 1U;
}

/* Converts a float to a half float, rounding to the nearest. */
static uint16_t
format_half(
	float value)
{
	uint32_t bits;
	uint32_t sign;
	uint32_t mantissa;
	int32_t exponent;
	uint32_t half;
	uint32_t shift;

	/* The float's fields. */
	memcpy(&bits, &value, 4U);
	sign = (bits >> 16) & 0x8000U;
	exponent = (int32_t)((bits >> 23) & 0xffU);
	mantissa = bits & 0x7fffffU;

	/* Infinity and NaN. */
	if (exponent == 255) {
		half = sign | 0x7c00U;
		if (mantissa != 0U)
			half |= 0x200U;
		return (uint16_t)half;
	}

	/* Too large: infinity. */
	exponent = exponent - 127 + 15;
	if (exponent >= 31)
		return (uint16_t)(sign | 0x7c00U);

	/* Too small for a subnormal: zero. */
	if (exponent < -10)
		return (uint16_t)sign;

	/* A subnormal: the mantissa with its hidden bit, shifted and rounded. */
	if (exponent <= 0) {
		mantissa |= 0x800000U;
		shift = (uint32_t)(14 - exponent);
		half = mantissa >> shift;
		if (((mantissa >> (shift - 1U)) & 1U) != 0U)
			half++;
		return (uint16_t)(sign | half);
	}

	/* A normal number, rounded (a carry moves into the exponent as it should). */
	half = ((uint32_t)exponent << 10) | (mantissa >> 13);
	if ((mantissa & 0x1000U) != 0U)
		half++;
	return (uint16_t)(sign | half);
}

/*
 * Converts a float to an unsigned small float of a 5-bit exponent and a
 * mantissa of a number of bits (6 for 11-bit, 5 for 10-bit floats):
 * negatives and NaN become 0, values too large the largest finite one.
 */
static uint32_t
format_small_float(
	float value,
	unsigned mantissa_bits)
{
	uint32_t bits;
	uint32_t mantissa;
	uint32_t largest;
	uint32_t result;
	int32_t exponent;
	uint32_t shift;

	/* The largest finite value: exponent 30, every mantissa bit. */
	largest = (30U << mantissa_bits) | ((1U << mantissa_bits) - 1U);

	/* Negatives, zero and NaN. */
	if (!(value > 0.0f))
		return 0U;

	/* The float's fields. */
	memcpy(&bits, &value, 4U);
	exponent = (int32_t)((bits >> 23) & 0xffU) - 127 + 15;
	mantissa = bits & 0x7fffffU;

	/* Too large (or infinite). */
	if (exponent >= 31)
		return largest;

	/* A subnormal, or too small for one. */
	if (exponent <= 0) {
		if (exponent < -(int32_t)mantissa_bits)
			return 0U;
		mantissa |= 0x800000U;
		shift = 23U - mantissa_bits + (uint32_t)(1 - exponent);
		result = mantissa >> shift;
		if (((mantissa >> (shift - 1U)) & 1U) != 0U)
			result++;
		return result;
	}

	/* A normal number, rounded. */
	shift = 23U - mantissa_bits;
	result = ((uint32_t)exponent << mantissa_bits) | (mantissa >> shift);
	if (((mantissa >> (shift - 1U)) & 1U) != 0U)
		result++;
	if (result > largest)
		result = largest;
	return result;
}

/* Converts an unsigned small float of a 5-bit exponent and a mantissa of a number of bits to a float. */
static float
format_from_small_float(
	uint32_t bits,
	unsigned mantissa_bits)
{
	uint32_t exponent;
	uint32_t mantissa;
	float fraction;

	/* The fields. */
	exponent = bits >> mantissa_bits;
	mantissa = bits & ((1U << mantissa_bits) - 1U);
	fraction = (float)mantissa / (float)(1U << mantissa_bits);

	/* A subnormal. */
	if (exponent == 0U)
		return fraction * format_power(-14);

	/* Infinity (and NaN) read as the largest float. */
	if (exponent == 31U)
		return 3.402823466e38f;

	/* A normal number. */
	return (1.0f + fraction) * format_power((int)exponent - 15);
}

/*
 * Packs three floats into the shared-exponent RGB9_E5 form
 * (EXT_texture_shared_exponent): the exponent of the largest channel,
 * and each channel's 9-bit mantissa at that exponent.
 */
static uint32_t
format_rgb9e5(
	const float *values)
{
	float channels[3];
	float largest;
	float scale;
	uint32_t bits;
	uint32_t mantissa;
	int32_t shared;
	uint32_t word;
	unsigned index;

	/* Each channel clamped to what the form holds, and the largest. */
	largest = 0.0f;
	for (index = 0U; index < 3U; index++) {
		channels[index] = format_clamp(values[index], 0.0f, FORMAT_RGB9E5_MAX);
		if (!(channels[index] >= 0.0f))
			channels[index] = 0.0f;
		if (channels[index] > largest)
			largest = channels[index];
	}

	/* The shared exponent: floor(log2(largest)) + 1 + 15, at least 0 (read from the float's exponent). */
	memcpy(&bits, &largest, 4U);
	shared = (int32_t)((bits >> 23) & 0xffU) - 127;
	if (shared < -16)
		shared = -16;
	shared += 1 + 15;

	/* A largest mantissa that rounds up to 512 takes the next exponent. */
	scale = format_power(shared - 15 - 9);
	mantissa = format_round(largest / scale);
	if (mantissa == 512U) {
		shared++;
		scale = format_power(shared - 15 - 9);
	}

	/* Each channel's mantissa at the shared exponent. */
	word = (uint32_t)shared << 27;
	for (index = 0U; index < 3U; index++) {
		mantissa = format_round(channels[index] / scale);
		if (mantissa > 511U)
			mantissa = 511U;
		word |= mantissa << (index * 9U);
	}

	/* The packed texel. */
	return word;
}

/* Returns 2 to a power between -126 and 127, made from its bits. */
static float
format_power(
	int exponent)
{
	uint32_t bits;
	float value;

	/* The exponent within the normal floats. */
	if (exponent < -126)
		exponent = -126;
	if (exponent > 127)
		exponent = 127;

	/* A float of that exponent and no mantissa. */
	bits = (uint32_t)(exponent + 127) << 23;
	memcpy(&value, &bits, 4U);
	return value;
}

/* Returns a value clamped between two others (NaN becomes the low one). */
static float
format_clamp(
	float value,
	float low,
	float high)
{
	/* Below (or NaN). */
	if (!(value >= low))
		return low;

	/* Above. */
	if (value > high)
		return high;

	/* Inside. */
	return value;
}

/* Rounds a value that is not negative to the nearest integer. */
static uint32_t
format_round(
	float value)
{
	/* Half up. */
	return (uint32_t)(value + 0.5f);
}
