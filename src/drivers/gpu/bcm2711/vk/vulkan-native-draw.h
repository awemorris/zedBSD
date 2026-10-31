/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* FIFO native preparation owns every uploaded input independently of public Vulkan identities. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_NATIVE_DRAW_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_NATIVE_DRAW_H

#include "drivers/gpu/bcm2711/native-storage.h"
#include "drivers/gpu/bcm2711/native-bin.h"
#include "drivers/gpu/bcm2711/v3d-job.h"
#include "drivers/gpu/bcm2711/vk/vulkan-uniform.h"

/* Each canonical slot needs at most one image and one descriptor allocation, plus three code/uniform pairs and fetch/default/shader storage. */
#define BCM2711_VULKAN_DRAW_STORAGE (BCM2711_VULKAN_PIPELINE_SETS * BCM2711_VULKAN_LAYOUT_BINDINGS * 2U + 9U)

/* One converted raster belongs to the draw that allocated it; a pass may borrow it until whole-pass retirement. */
struct bcm2711_vulkan_native_texture_copy {
	const struct bcm2711_vulkan_resource *image;
	/* Where in the image the texture's raster starts: a picture's planes are two textures of one image. */
	uint64_t offset;
	struct bcm2711_native_storage *pixels;
	/* A space-owned snapshot lends pixels to this draw, which holds one use of it until it retires. */
	struct bcm2711_native_snapshot *snapshot;
	/* The texture formatting unit fills pixels from the raster image when the pass runs, before its draws. */
	bool convert;
	struct bcm2711_v3d_tfu_job tfu;
	/* The mip levels converted: level 0 by tfu, the smaller ones by bcm2711_vulkan_native_mip_job. */
	uint32_t levels;
};

/* Draws one chunk of a pass's draw pool holds. */
#define BCM2711_VULKAN_DRAW_CHUNK 64U

struct bcm2711_vulkan_native_draw_chunk;
struct bcm2711_vulkan_native_upload_chunk;

/*
 * A pass's draws come from chunks of BCM2711_VULKAN_DRAW_CHUNK draws instead
 * of one kernel allocation each, and their small uploads (texture and shader
 * records, uniform streams, defaults, vertex fetch) are carved from shared
 * upload chunks instead of one mapped page each.  The pass releases both
 * after releasing every draw.  Only the controller-serialized pass owner
 * touches it.
 */
struct bcm2711_vulkan_native_draw_pool {
	struct bcm2711_vulkan_native_draw_chunk *chunks;
	struct bcm2711_vulkan_native_upload_chunk *uploads;
};

/* One complete draw owns its uploads and borrows earlier immutable pass inputs; the pass retains all draws through uncertainty. */
struct bcm2711_vulkan_native_draw {
	/* The enclosing pass links fully owned draw roots; individual release never traverses this link. */
	struct bcm2711_vulkan_native_draw *next;
	/* The pass pool whose chunk holds this draw, or NULL for a draw allocated on its own. */
	struct bcm2711_vulkan_native_draw_pool *pool;
	struct bcm2711_v3d_space *space;
	struct bcm2711_native_storage *storage[BCM2711_VULKAN_DRAW_STORAGE];
	struct bcm2711_vulkan_native_binding bindings[BCM2711_VULKAN_PIPELINE_SETS][BCM2711_VULKAN_LAYOUT_BINDINGS];
	/* Earlier draws belong to the same unsubmitted pass; this construction-only link is cleared before publication. */
	const struct bcm2711_vulkan_native_draw *earlier;
	struct bcm2711_vulkan_native_texture_copy textures[BCM2711_VULKAN_PIPELINE_SETS * BCM2711_VULKAN_LAYOUT_BINDINGS];
	uint32_t texture_count;
	/* Exact compiled objects identify immutable code; storage belongs to this or an earlier retained draw. */
	const struct bcm2711_shader_binary *programs[3];
	struct bcm2711_native_storage *code[3];
	/* All draws use the same immutable sixteen float (0,0,0,1) defaults. */
	struct bcm2711_native_storage *defaults;
	/* Preparation diagnostics include scheduler delay and never affect admission. */
	uint64_t texture_ticks;
	uint64_t copy_ticks;
	uint64_t program_ticks;
	uint64_t record_ticks;
	/* How each new texture of the draw was obtained: reused or converted snapshot, mapped source, or other private copy. */
	uint32_t snapshot_hits;
	uint32_t snapshot_fills;
	uint32_t snapshot_updates;
	uint32_t mapped_textures;
	uint32_t private_textures;
	uint64_t mapped_bytes;
	/* The largest texture the draw converted, as width << 16 | height, and how ('f', 'm' or 'p'). */
	uint32_t largest_size;
	char largest_kind;
	uint64_t bytes;
	uint32_t count;
	uint32_t shader;
	uint32_t attributes;
	/* The VCM Cache Size packet's byte for its shader (bcm2711_native_shader_vcm). */
	uint32_t vcm;
	/* The draw's vertices or indices, instances and bases, in bcm2711_native_bin's meanings. */
	uint32_t vertices;
	uint32_t instances;
	uint32_t first;
	uint32_t base_vertex;
	uint32_t base_instance;
	/* One: an indexed draw reading index_bytes of index_size indices at the GPU's index_address. */
	uint32_t indexed;
	uint32_t index_address;
	uint32_t index_bytes;
	uint32_t index_size;
	/*
	 * The draw's clip window in the framebuffer's pixels (x, y, width,
	 * height): the drawable, render area, viewport and scissor together.
	 * No pixel outside it is written; the pass renders only the tiles
	 * its draws' windows meet.
	 */
	uint32_t window[4];
	/* Numerical BCL state refers only to this draw or retained earlier draws in the same pass. */
	uint32_t bin_bytes;
	uint8_t bin[BCM2711_NATIVE_BIN_BYTES];
};

/* One unpublished internal texture quad borrows an actual retained source and owns copied geometry/programs until native upload finishes. */
struct bcm2711_vulkan_meta_draw {
	struct bcm2711_vulkan_pipeline pipeline;
	struct bcm2711_shader_key key;
	struct bcm2711_vulkan_resource *image;
	/* The source's mip level as an image of its own, which image points to; the primary retains the real image. */
	struct bcm2711_vulkan_resource level;
	uint32_t vertices[36];
	uint32_t viewport[6];
	uint32_t width;
	uint32_t height;
	VkRect2D area;
	VkFilter filter;
	/* Copy preserves raw channel bytes; blit instead uses the source's canonical colour interpretation. */
	bool raw;
};

int bcm2711_vulkan_native_meta_draw_create(struct bcm2711_v3d_space *space, const struct bcm2711_vulkan_meta_draw *meta, uint64_t *available, struct bcm2711_vulkan_native_draw **draw);

/*
 * Controller serialization and prior queue visibility are required; available is the remaining padded job budget.
 * Earlier draws must belong to this same pass and remain alive until the entire pass retires; NULL gives an independent draw.
 */
int bcm2711_vulkan_native_draw_create(struct bcm2711_v3d_space *space, const struct bcm2711_vulkan_prepared_event *event, uint64_t *available, const struct bcm2711_vulkan_native_draw *earlier, struct bcm2711_vulkan_native_draw_pool *pool, struct bcm2711_vulkan_native_draw **draw);

/* Frees a pass pool's chunks and releases its upload chunks; every draw from it has been released and its DMA retired. */
int bcm2711_vulkan_native_draw_pool_release(struct bcm2711_vulkan_native_draw_pool *pool);

/* true consumes/nulls all storage even when translation teardown fails; false keeps the whole root unchanged. */
int bcm2711_vulkan_native_draw_release(struct bcm2711_vulkan_native_draw **draw, bool retired);

/* Internal lowering runs only while the unpublished draw root and prepared event are protected by the controller mutex. */
int bcm2711_vulkan_native_bin_prepare(const struct bcm2711_vulkan_prepared_event *event, struct bcm2711_vulkan_native_draw *draw);

int bcm2711_vulkan_native_mip_job(const struct bcm2711_vulkan_native_texture_copy *entry, uint32_t index, struct bcm2711_v3d_tfu_job *tfu);

#endif
