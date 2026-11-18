/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* One FIFO pass owns every mapped draw input, command list and framebuffer hold through native DMA uncertainty. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_NATIVE_PASS_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_NATIVE_PASS_H

#include "drivers/gpu/videocore/native-pass.h"
#include "drivers/gpu/videocore/v3d-job.h"
#include "drivers/gpu/videocore/vk/vulkan-native-draw.h"
#include "drivers/gpu/videocore/vk/vulkan-occlusion.h"

/*
 * A pass's storages: its three lists, tile pool, tile state and four
 * overflow reserves, and an occlusion pass's counters after them.
 */
#define BCM2711_VULKAN_PASS_LISTS 9U
#define BCM2711_VULKAN_PASS_STORAGE 10U

/*
 * One colour target of a native pass: the view of its image's backing,
 * retained through native retirement, with its coherent CPU alias; the
 * image (which the enclosing pending primary retains through the implicit
 * pass layout transitions), the mip level and array layer the pass writes,
 * whose layout it moves, and that level's size; the attachment's format
 * (swap set for a BGRA one, whose red and blue the shaders exchange), load
 * and clear value (raw API words).
 *
 * A pass that continues its render pass after a vkCmdClearAttachments
 * clears rects of the target (inside the render area) to rect_clear on
 * the CPU before it loads its tiles; a clear of the whole target is a tile
 * clear instead, and none clears no rectangle.
 */
struct bcm2711_vulkan_native_target {
	struct bcm2711_v3d_view *output;
	void *cpu;
	struct bcm2711_vulkan_resource *image;
	uint32_t level;
	uint32_t layer;
	uint32_t width;
	uint32_t height;
	VkImageLayout initial_layout;
	VkImageLayout final_layout;
	VkFormat format;
	uint32_t swap;
	VkAttachmentLoadOp load;
	uint32_t clear[4];
	VkRect2D rects[BCM2711_VULKAN_CLEAR_RECTS];
	uint32_t rect_count;
	uint32_t rect_clear[4];
};

/*
 * The enclosing submission keeps its prepared primary pending; this root retains independent GPU owners and copied numerical target/clear state.
 *
 * targets holds each colour slot's target (output NULL for an unused
 * slot), as many as state.targets; an internal transfer's output is
 * targets[0].
 */
struct bcm2711_vulkan_native_pass {
	struct bcm2711_v3d_space *space;
	struct bcm2711_vulkan_native_target targets[BCM2711_NATIVE_PASS_TARGETS];
	/* A TLB copy's source view is retained like the output through native retirement. */
	struct bcm2711_v3d_view *source;
	/*
	 * A loaded or stored depth/stencil attachment's view is retained like
	 * the output; depth_target is its image, whose layout at the array
	 * layer depth_layer (the one the framebuffer's view names) the pass
	 * checks and commits like the colour targets'.  Both are NULL for a
	 * pass without one; depth alone is NULL when the attachment stays in
	 * the tile buffer.
	 */
	struct bcm2711_v3d_view *depth;
	struct bcm2711_vulkan_resource *depth_target;
	uint32_t depth_layer;
	VkImageLayout depth_initial_layout;
	VkImageLayout depth_final_layout;
	struct bcm2711_vulkan_native_draw *first;
	struct bcm2711_vulkan_native_draw *last;
	/* The chunks the pass's draws come from, freed after every draw is released. */
	struct bcm2711_vulkan_native_draw_pool draw_pool;
	struct bcm2711_native_storage *storage[BCM2711_VULKAN_PASS_STORAGE];
	/*
	 * The occlusion queries the draws count into, each with its counter
	 * (one of the group in occlusion, which is one of the storages above),
	 * added to the query once the pass has completed.
	 */
	struct bcm2711_native_storage *occlusion;
	struct bcm2711_vulkan_occlusion_pool *occlusion_pools[BCM2711_VULKAN_OCCLUSION_COUNTERS];
	uint32_t occlusion_queries[BCM2711_VULKAN_OCCLUSION_COUNTERS];
	uint32_t occlusion_count;
	uint64_t bytes;
	uint32_t count;
	uint32_t draws;
	struct bcm2711_native_pass state;
	struct bcm2711_v3d_job job;
	/* Clear is performed only at execution after the whole pass has prepared successfully, never during allocation or OOM rollback. */
	VkRect2D area;
	/*
	 * A continued pass's clear of rectangles of the depth/stencil image,
	 * written on the CPU through its coherent alias depth_cpu before the
	 * tiles load: each pixel of the rectangles takes depth_value's bits
	 * that depth_mask selects (depth in bits 8 to 31, stencil in bits 0 to
	 * 7, the tile buffer's d24s8 memory format).
	 */
	void *depth_cpu;
	VkRect2D depth_rects[BCM2711_VULKAN_CLEAR_RECTS];
	uint32_t depth_rect_count;
	uint32_t depth_value;
	uint32_t depth_mask;
	/* Single-use execution refuses replay and preserves the last native DMA-retirement result. */
	bool executed;
	bool retired;
	/* Every texture conversion of the launch completed; the finish marks their snapshots filled. */
	bool converted;
	/* A whole-image copy records which source state its output now equals, for later incremental copies. */
	bool sync;
	uint64_t sync_identity;
	uint64_t sync_offset;
	uint64_t sync_generation;
};

/* Earlier queue writes must have completed and become CPU-visible; next borrows the event after this pass's END from the enclosing prepared primary. */
int bcm2711_vulkan_native_pass_create(struct bcm2711_v3d_space *space, const struct bcm2711_vulkan_prepared_event *begin, uint64_t *available, struct bcm2711_vulkan_native_pass **pass, const struct bcm2711_vulkan_prepared_event **next);

/*
 * After a pass has completed: adds its occlusion counters to their queries
 * and makes available the queries whose end lies among the events from
 * first up to end (the events the pass ran).
 */
void bcm2711_vulkan_native_pass_counted(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_prepared_event *first, const struct bcm2711_vulkan_prepared_event *end);

/* A depth/stencil image clear writes the cleared aspects of every pixel through the image's coherent alias, after the work before it. */
int bcm2711_vulkan_native_depth_clear_run(const struct bcm2711_vulkan_record *record);

/* Finds the level and layer (or slice) a colour image clear writes in its pass number index; ENOENT past its last pass. */
int bcm2711_vulkan_native_clear_subresource(const struct bcm2711_vulkan_record *record, uint32_t index, uint32_t *level, uint32_t *layer);

/* A colour image clear's pass number subresource uses the native tile clear colour and store without CPU target writes or user graphics state. */
int bcm2711_vulkan_native_clear_create(struct bcm2711_v3d_space *space, const struct bcm2711_vulkan_prepared_event *event, uint32_t subresource, uint64_t *available, struct bcm2711_vulkan_native_pass **pass);

/* Fills a native target's tile buffer fields (type, bpp, clamp, output format, raster texel) for a colour attachment format; ENOTSUP for another format. */
int bcm2711_vulkan_native_target_format(struct bcm2711_native_pass_target *target, VkFormat format);

/* Chooses a pass's tile size from its targets, then the tile rectangle its render area covers. */
int bcm2711_vulkan_native_pass_tiles(struct bcm2711_vulkan_native_pass *pass);

/* An unpublished owner with complete output/draw state gains all independently owned lists; failure leaves its partial mappings attached for release. */
int bcm2711_vulkan_native_pass_build_lists(struct bcm2711_vulkan_native_pass *pass, uint64_t *available);

/* Run once under the controller mutex, then retain the whole parent payload when the returned native retirement proof is false. */
int bcm2711_vulkan_native_pass_run(struct bcm2711_vulkan_native_pass *pass, struct bcm2711_v3d_job_result *result);

/*
 * The same run in three parts, for the render executor's window: start
 * (validation and CPU clear) and finish (publishing the output) under the
 * controller mutex, launch (the native jobs) inside the window without it.
 * Launch touches only the pass and the engine; finish takes launch's result.
 */
int bcm2711_vulkan_native_pass_start(struct bcm2711_vulkan_native_pass *pass, struct bcm2711_v3d_job_result *result);
int bcm2711_vulkan_native_pass_launch(struct bcm2711_vulkan_native_pass *pass, struct bcm2711_v3d_job_result *result);
int bcm2711_vulkan_native_pass_finish(struct bcm2711_vulkan_native_pass *pass, struct bcm2711_v3d_job_result *result, int error);

/* false preserves the complete root, including output and every draw; a controller quarantine transfer is still required before worker payload disposal. */
int bcm2711_vulkan_native_pass_release(struct bcm2711_vulkan_native_pass **pass, bool retired);

#endif
