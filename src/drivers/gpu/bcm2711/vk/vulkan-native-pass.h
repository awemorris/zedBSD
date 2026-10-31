/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* One FIFO pass owns every mapped draw input, command list and framebuffer hold through native DMA uncertainty. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_NATIVE_PASS_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_NATIVE_PASS_H

#include "drivers/gpu/bcm2711/native-pass.h"
#include "drivers/gpu/bcm2711/v3d-job.h"
#include "drivers/gpu/bcm2711/vk/vulkan-native-draw.h"

#define BCM2711_VULKAN_PASS_STORAGE 9U

struct bcm2711_v3d_perf;

/* The enclosing submission keeps its prepared primary pending; this root retains independent GPU owners and copied numerical target/clear state. */
struct bcm2711_vulkan_native_pass {
	struct bcm2711_v3d_space *space;
	struct bcm2711_v3d_view *output;
	/* A TLB copy's source view is retained like the output through native retirement. */
	struct bcm2711_v3d_view *source;
	/*
	 * A loaded or stored depth/stencil attachment's view is retained like
	 * the output (ws141-p017); depth_target is its image, whose layout the
	 * pass checks and commits like the colour target's.  Both are NULL for a
	 * pass without one; depth alone is NULL when the attachment stays in the
	 * tile buffer.
	 */
	struct bcm2711_v3d_view *depth;
	struct bcm2711_vulkan_resource *depth_target;
	VkImageLayout depth_initial_layout;
	VkImageLayout depth_final_layout;
	void *cpu;
	/* The enclosing pending primary retains this logical image through implicit pass layout transitions. */
	struct bcm2711_vulkan_resource *target;
	/* The target's mip level the pass writes, whose layout it moves. */
	uint32_t target_level;
	VkImageLayout initial_layout;
	VkImageLayout final_layout;
	struct bcm2711_vulkan_native_draw *first;
	struct bcm2711_vulkan_native_draw *last;
	/* The chunks the pass's draws come from, freed after every draw is released. */
	struct bcm2711_vulkan_native_draw_pool draw_pool;
	struct bcm2711_native_storage *storage[BCM2711_VULKAN_PASS_STORAGE];
	uint64_t bytes;
	uint32_t count;
	uint32_t draws;
	struct bcm2711_native_pass state;
	struct bcm2711_v3d_job job;
	/* Clear is performed only at execution after the whole pass has prepared successfully, never during allocation or OOM rollback. */
	VkRect2D area;
	VkFormat format;
	VkAttachmentLoadOp load;
	uint32_t clear[4];
	/* Single-use execution refuses replay and preserves the last native DMA-retirement result. */
	bool executed;
	bool retired;
	/* Every texture conversion of the launch completed; the finish marks their snapshots filled. */
	bool converted;
	/*
	 * The bin queue already ran, while the pass before it rendered
	 * (bcm2711_vulkan_native_pass_prebin), for bin_ticks: the launch runs
	 * only the render queue.
	 */
	bool binned;
	uint64_t bin_ticks;
	/* A whole-image copy records which source state its output now equals, for later incremental copies. */
	bool sync;
	uint64_t sync_identity;
	uint64_t sync_offset;
	uint64_t sync_generation;
};

/* Earlier queue writes must have completed and become CPU-visible; next borrows the event after this pass's END from the enclosing prepared primary. */
int bcm2711_vulkan_native_pass_create(struct bcm2711_v3d_space *space, const struct bcm2711_vulkan_prepared_event *begin, uint64_t *available, struct bcm2711_vulkan_native_pass **pass, const struct bcm2711_vulkan_prepared_event **next);

/* A full-image clear uses the native tile clear colour and store without CPU target writes or user graphics state. */
int bcm2711_vulkan_native_clear_create(struct bcm2711_v3d_space *space, const struct bcm2711_vulkan_prepared_event *event, uint64_t *available, struct bcm2711_vulkan_native_pass **pass);

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

/*
 * The launch in two halves: the first runs the texture conversions and the
 * bin queue and launches the render queue, which flight then holds
 * (flight->active), so that the caller may prepare and bin the next pass
 * meanwhile (bcm2711_vulkan_native_pass_prebin); the second waits for it.
 * The second half always follows the first; error is the first half's.
 */
int bcm2711_vulkan_native_pass_launch_begin(struct bcm2711_vulkan_native_pass *pass, struct bcm2711_v3d_job_result *result, struct bcm2711_v3d_flight *flight);
int bcm2711_vulkan_native_pass_launch_end(struct bcm2711_vulkan_native_pass *pass, struct bcm2711_v3d_job_result *result, struct bcm2711_v3d_flight *flight, int error);

/*
 * Runs a prepared pass's bin queue while the flight's render runs.  A
 * refusal leaves the pass to bin as usual; a failure after the launch
 * leaves it uncertain (executed, not retired).
 */
int bcm2711_vulkan_native_pass_prebin(struct bcm2711_vulkan_native_pass *pass, struct bcm2711_v3d_flight *flight);

/* Whether a pass samples an image another pass renders into (its colour or depth target). */
bool bcm2711_vulkan_native_pass_samples(const struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_native_pass *other);

/* Adds the draws' and lists' diagnostic preparation times, draw count and staged bytes to a report under the controller mutex. */
void bcm2711_vulkan_native_pass_costs(const struct bcm2711_vulkan_native_pass *pass, struct bcm2711_v3d_perf *perf);

/* false preserves the complete root, including output and every draw; a controller quarantine transfer is still required before worker payload disposal. */
int bcm2711_vulkan_native_pass_release(struct bcm2711_vulkan_native_pass **pass, bool retired);

#endif
