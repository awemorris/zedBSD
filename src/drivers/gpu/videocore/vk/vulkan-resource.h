/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Typed resources keep immutable storage requirements separate from their independently retained memory binding. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_RESOURCE_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_RESOURCE_H

#include "drivers/gpu/videocore/vk/vulkan-private.h"
#include "drivers/gpu/videocore/native-state.h"

struct bcm2711_v3d_view;

/* The most mip levels an image has: a 4096-pixel side halves twelve times. */
#define BCM2711_VULKAN_IMAGE_LEVELS 13U

/* The most array layers an image has (Vulkan's least maxImageArrayLayers). */
#define BCM2711_VULKAN_IMAGE_LAYERS 256U

/*
 * One mip level of a colour image: its raster rows, at offset from the
 * image's binding, of width texels at pitch bytes for height rows.
 */
struct bcm2711_vulkan_image_level {
	uint64_t offset;
	uint64_t bytes;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
};

/* One buffer or single-subresource colour image retains its logical device and exact bound allocation. */
struct bcm2711_vulkan_resource {
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *memory;
	uint64_t bytes;
	uint64_t required_bytes;
	uint64_t alignment;
	uint64_t offset;
	uint32_t usage;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	/* An image's bytes a texel: 4 for colour, D24S8 and X8_D24, 2 for D16. */
	uint32_t texel_bytes;
	VkFormat format;
	VkImageTiling tiling;
	/* The layout of mip level 0 (all of a one-level image). */
	VkImageLayout layout;
	/*
	 * The samples a pixel of a multisampled attachment has (4; 1 for any
	 * other image).  A pixel's four samples are a 2x2 block of its raster,
	 * which is twice as wide and as high as the image (bytes and pitch are
	 * the raster's), as the tile buffer stores all its samples.
	 */
	uint32_t samples;
	/*
	 * A mip-mapped colour image's levels follow level 0 (bytes, pitch,
	 * width and height) in its binding, smaller ones after larger, each
	 * its own raster; storage_bytes spans them all (zero: bytes does).
	 * level_layouts holds the layouts of levels 1 and up.
	 */
	uint32_t levels;
	uint64_t storage_bytes;
	VkImageLayout level_layouts[BCM2711_VULKAN_IMAGE_LEVELS];
	/*
	 * An image's array layers (zero or one: a single layer).  Each layer
	 * holds every level above, laid out as a one-layer image's, and starts
	 * layer_stride bytes after the one before it; storage_bytes then spans
	 * all the layers.  cube says six square layers may be viewed as a cube.
	 * layer_layouts holds the layouts of each level of layers 1 and up,
	 * levels to a layer; it is allocated with the published image and is
	 * NULL in a one-layer image and in a borrowed level view.
	 */
	uint32_t layers;
	uint64_t layer_stride;
	bool cube;
	VkImageLayout *layer_layouts;
	/*
	 * A 3D image's slices at level 0 (zero for any other image, which has
	 * one).  It has one array layer; its slices are kept as layers are,
	 * each holding every level and starting layer_stride bytes after the
	 * one before, and level n has its first max(depth >> n, 1) slices.  A
	 * level's layout is that of all its slices, kept as the first
	 * layer's.
	 */
	uint32_t depth;
	/*
	 * A storage image of a storage format created mutable: a storage view
	 * may read and write its texels as another storage format of the same
	 * texel size (sampling and drawing use the image's own format).
	 */
	bool mutable_format;
};

/* The most slices a 3D image has, and the most texels along each of its sides. */
#define BCM2711_VULKAN_IMAGE_DEPTH 256U

bool bcm2711_vulkan_depth_format(VkFormat format);
uint32_t bcm2711_vulkan_storage_texel(VkFormat format);

/* The output of a colour format that is not a colour attachment. */
#define BCM2711_VULKAN_NO_OUTPUT 0xffU

/*
 * A colour format an image may have: its texel's bytes, the native texture
 * format it is sampled as (BGRA ones with red and blue swapped), whether it
 * is filtered linearly, and the Output Image Format the tile buffer loads
 * and stores it in as a colour attachment (BCM2711_VULKAN_NO_OUTPUT when
 * it is not one).
 */
struct bcm2711_vulkan_colour_format {
	VkFormat format;
	enum bcm2711_native_texel texel;
	uint8_t bytes;
	uint8_t swap;
	uint8_t filter;
	uint8_t output;
};

/*
 * How the tile buffer holds a colour attachment of a format: its Output
 * Image Format, Internal Type, Internal BPP and 7.1 Render Target Type
 * Clamp, the raster's bytes a pixel, whether red and blue are swapped (a
 * BGRA format, whose fragments the shader swaps), its channels, whether
 * they are integers, whether the fragment program writes it 32 bits a
 * channel (32-bit floats and integers) rather than as halves, whether the
 * tile buffer blends it, and whether a load and a store keep its bytes.
 */
struct bcm2711_vulkan_colour_target {
	uint32_t output;
	uint32_t type;
	uint32_t bpp;
	uint32_t clamp;
	uint32_t texel;
	uint32_t components;
	bool swap;
	bool integer;
	bool wide;
	bool blend;
	bool exact;
};

/* Finds a colour format an image may have; NULL for any other format. */
const struct bcm2711_vulkan_colour_format *bcm2711_vulkan_colour_format(VkFormat format);

/* Describes a colour attachment format's place in the tile buffer; ENOTSUP for a format that is not a colour attachment. */
int bcm2711_vulkan_colour_target(VkFormat format, struct bcm2711_vulkan_colour_target *target);
int bcm2711_vulkan_resource_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);
int bcm2711_vulkan_image_level(const struct bcm2711_vulkan_resource *image, uint32_t level, struct bcm2711_vulkan_image_level *result);
int bcm2711_vulkan_image_level_view(const struct bcm2711_vulkan_resource *image, uint32_t level, uint32_t layer, struct bcm2711_vulkan_resource *view);
uint32_t bcm2711_vulkan_image_layers(const struct bcm2711_vulkan_resource *image);

/* Reports the rasters a mip level of an image has: a 3D image's slices at the level, any other image's array layers. */
uint32_t bcm2711_vulkan_image_slices(const struct bcm2711_vulkan_resource *image, uint32_t level);
VkImageLayout bcm2711_vulkan_image_level_layout(const struct bcm2711_vulkan_resource *image, uint32_t level, uint32_t layer);
void bcm2711_vulkan_image_set_level_layout(struct bcm2711_vulkan_resource *image, uint32_t level, uint32_t layer, VkImageLayout layout);
int bcm2711_vulkan_image_clear_range(const struct bcm2711_vulkan_resource *image, uint32_t range, uint32_t *level, uint32_t *levels, uint32_t *layer, uint32_t *layers);
bool bcm2711_vulkan_engine_71(const struct bcm2711_vulkan_session *session);
int bcm2711_vulkan_resource_backing(struct bcm2711_vulkan_resource *resource, uint64_t offset, uint64_t bytes, struct bcm2711_v3d_view **view, uint32_t *address, void **cpu);

#endif
