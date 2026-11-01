/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR core's view of images: the formats the executor takes, how
 * an image lies in memory, the two state words the texture unit reads for
 * a view and the two it reads for a sampler, and the bytes one descriptor
 * occupies in a set.  This part only computes; it holds no lock and no
 * memory, and it builds on the host too.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_IMAGE_STATE_H
#define KERN_DRIVERS_GPU_POWERVR_IMAGE_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libc/vulkan/vulkan_core.h>

/* The most mip levels of an image (a 16384-texel edge has 15). */
#define POWERVR_IMAGE_LEVELS_MAX	15U

/* The largest edge of an image, in texels. */
#define POWERVR_IMAGE_EDGE_MAX		16384U

/* The most array layers of an image. */
#define POWERVR_IMAGE_LAYERS_MAX	2048U

/*
 * The boundary an image starts on in its memory, and what its size is
 * rounded to: the core's page, the larger of the page and the buffer
 * boundary of the executor.
 */
#define POWERVR_IMAGE_ALIGN		0x1000U

/* The bytes a row of a linear image is a multiple of (the display's import takes such rows). */
#define POWERVR_IMAGE_ROW_ALIGN		32U

/* The rows a linear image's memory is rounded up to: whole tiles a transfer may touch. */
#define POWERVR_IMAGE_LINEAR_ROWS	32U

/*
 * The one switch of the shared path: whether a linear image's formats
 * report sampling (and linear filtering).  libvulkan tries a shared,
 * linear swapchain image only when linear sampling is reported; while it
 * is not, a client's window system presents through copies of optimal
 * images.  The executor takes the shared path's external images, exports
 * and imports either way.  It is on: the Wayland client WSI has no copied
 * route, so with it off every Wayland swapchain fails with
 * VK_ERROR_FORMAT_NOT_SUPPORTED.  A host test may define it to 0 to check
 * both values.
 */
#ifndef POWERVR_FORMAT_LINEAR_SAMPLED
#define POWERVR_FORMAT_LINEAR_SAMPLED	1
#endif

/* The kinds of a format's texels. */
#define POWERVR_FORMAT_COLOR		0x1U
#define POWERVR_FORMAT_DEPTH		0x2U
#define POWERVR_FORMAT_SRGB		0x4U
#define POWERVR_FORMAT_FLOAT		0x8U
#define POWERVR_FORMAT_PLANAR		0x10U

/* The most planes of a multi-planar image (NV12 has two). */
#define POWERVR_IMAGE_PLANES_MAX	2U

/* Where one output channel of a view comes from. */
enum powervr_channel {
	POWERVR_CHANNEL_0,
	POWERVR_CHANNEL_1,
	POWERVR_CHANNEL_2,
	POWERVR_CHANNEL_3,
	POWERVR_CHANNEL_ZERO,
	POWERVR_CHANNEL_ONE,
	POWERVR_CHANNEL_ABSENT
};

/*
 * One format the executor takes.
 *
 * texel_bytes is the size of one texel or vertex element.  sample_code
 * is the texture unit's format and store_code the pixel back end's, each
 * 0xFF when the unit has none for it (a format only vertex buffers
 * read has no texture format).  channels says where each of the
 * four outputs (red, green, blue, alpha) of a plain view comes from.  The
 * features are the Vulkan format features of each tiling and of buffers.
 */
struct powervr_format {
	uint32_t vk_format;
	uint8_t texel_bytes;
	uint8_t kind;
	uint8_t sample_code;
	uint8_t store_code;
	uint8_t channels[4];
	uint32_t optimal_features;
	uint32_t linear_features;
	uint32_t buffer_features;
};

/* The shape of an image as its create asks for it. */
struct powervr_image_shape {
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t levels;
	uint32_t layers;
	bool linear;
	bool volume;
};

/*
 * Where an image's texels lie.
 *
 * A linear image is rows of row_pitch bytes; an optimal one is in the
 * core's twiddled order over edges rounded up to powers of two.
 * padded_width and padded_height are those edges of level 0 (a linear
 * image's padded width is its row in texels).  level_offset gives each
 * level's start in a layer, layer_bytes the distance between layers and
 * bytes the whole image.
 */
struct powervr_image_layout {
	uint32_t row_pitch;
	uint32_t padded_width;
	uint32_t padded_height;
	uint32_t padded_depth;
	uint64_t level_offset[POWERVR_IMAGE_LEVELS_MAX];
	uint64_t layer_bytes;
	uint64_t bytes;
};

/*
 * One plane of a multi-planar image: the one-plane format a view of it
 * reads it as, its edges, where it starts in the image's memory, and how
 * its texels lie (always linear rows).
 */
struct powervr_image_plane {
	const struct powervr_format *format;
	uint32_t width;
	uint32_t height;
	uint64_t offset;
	struct powervr_image_layout layout;
};

/*
 * What a view shows of an image: the image's format, layout and address,
 * its level-0 edges and level count, the view's first level and layer and
 * their counts, and the client's component mapping (VkComponentSwizzle).
 */
struct powervr_view_shape {
	const struct powervr_format *format;
	const struct powervr_image_layout *layout;
	uint64_t address;
	uint32_t width;
	uint32_t height;
	uint32_t image_levels;
	uint32_t base_level;
	uint32_t level_count;
	uint32_t base_layer;
	uint32_t layer_count;
	bool linear;
	uint32_t mapping[4];
};

/*
 * What a sampler asks for, with its floating-point values as their 32
 * bits (the kernel does no floating-point arithmetic).
 */
struct powervr_sampler_shape {
	uint32_t mag_filter;
	uint32_t min_filter;
	uint32_t mipmap_mode;
	uint32_t address_u;
	uint32_t address_v;
	uint32_t address_w;
	uint32_t lod_bias_bits;
	uint32_t min_lod_bits;
	uint32_t max_lod_bits;
	bool unnormalized;
};

const struct powervr_format *powervr_format_find(uint32_t vk_format);
uint32_t powervr_format_linear_features(const struct powervr_format *format);
int powervr_image_lay_out(const struct powervr_image_shape *shape, const struct powervr_format *format, struct powervr_image_layout *layout);
int powervr_image_lay_out_planes(const struct powervr_image_shape *shape, const struct powervr_format *format, struct powervr_image_plane planes[POWERVR_IMAGE_PLANES_MAX], uint32_t *count, struct powervr_image_layout *layout);
int powervr_view_words(const struct powervr_view_shape *view, uint64_t words[2]);
int powervr_sampler_words(const struct powervr_sampler_shape *sampler, uint64_t words[2]);
uint32_t powervr_descriptor_bytes(uint32_t type);
uint32_t powervr_format_clear_word_count(const struct powervr_format *format);
int powervr_format_clear_words(const struct powervr_format *format, const uint32_t colour[4], uint32_t packed[4]);

#endif /* KERN_DRIVERS_GPU_POWERVR_IMAGE_STATE_H */
