/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's render passes, framebuffers and render jobs: what a
 * VkRenderPass keeps, the attachments a framebuffer names, and one job of
 * a render pass instance (the attachments with their load and store
 * operations, clear values and the render area) in the form the render
 * kick takes.  Nothing here holds a lock or memory; the host check
 * builds it unchanged.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_RENDER_PASS_H
#define KERN_DRIVERS_GPU_POWERVR_RENDER_PASS_H

#include <stdbool.h>
#include <stdint.h>

#include <libc/vulkan/vulkan_core.h>

#include "drivers/gpu/powervr/image-state.h"
#include "drivers/gpu/powervr/render-program.h"

/* The colour attachments of a subpass and the attachments of a pass (four colour, a resolve of each, one depth). */
#define POWERVR_PASS_COLORS		4U
#define POWERVR_PASS_ATTACHMENTS	9U

/* The clear rectangles one vkCmdClearAttachments takes. */
#define POWERVR_PASS_CLEAR_RECTS	8U

/*
 * The draws one render job keeps, the vertex buffers and descriptor sets
 * of a draw, its push constant bytes.  Each job's 3D pass takes about the
 * same time whatever its draws (a full-frame compositor job about 37 ms
 * on the board at 396 MHz), and each job past an instance's first loads
 * the whole render area again, so a pass split into fewer jobs is faster
 * (a three-window desktop frame is about 263 draws, which 512 keeps in
 * one job).  250 full-frame 1080p draws would fill the 16 MiB parameter
 * buffer of the global free list (about 64 KiB of control stream a
 * draw); a compositor's draws cover far less.
 */
#define POWERVR_RENDER_DRAWS		512U
#define POWERVR_DRAW_VERTEX_BUFFERS	4U
#define POWERVR_DRAW_SETS		4U
#define POWERVR_DRAW_PUSH_BYTES		128U

/*
 * The dynamic offsets one draw carries over all its bound sets: the most
 * dynamic uniform buffers a pipeline layout may have, as the device
 * reports them (maxDescriptorSetUniformBuffersDynamic).
 */
#define POWERVR_DRAW_DYNAMIC_OFFSETS	8U

/*
 * A clear draw's mask (a bit per colour attachment from bit 0, the
 * depth's bit and the stencil's) and where its push constants hold the
 * values: each colour attachment's four words at 16 bytes an attachment,
 * the depth's float bits after them.  The stencil's value is the draw's
 * stencil reference, not a push constant.
 */
#define POWERVR_DRAW_CLEAR_DEPTH	0x10U
#define POWERVR_DRAW_CLEAR_STENCIL	0x20U
#define POWERVR_DRAW_CLEAR_COLOR_BYTES	16U
#define POWERVR_DRAW_CLEAR_DEPTH_OFFSET	64U

/* No attachment. */
#define POWERVR_PASS_UNUSED		0xffffffffU

/* One attachment of a pass: its format, its samples and its operations (VkAttachmentLoadOp, VkAttachmentStoreOp). */
struct powervr_pass_attachment {
	uint32_t format;
	uint32_t samples;
	uint32_t load_op;
	uint32_t store_op;
	uint32_t stencil_load_op;
	uint32_t stencil_store_op;
};

/*
 * A VkRenderPass of one subpass: its attachments, the subpass's colour
 * attachments, the resolve attachment of each (POWERVR_PASS_UNUSED for
 * none), its depth attachment and the attachment its sample 0 is resolved
 * into (depth and stencil alike, POWERVR_PASS_UNUSED for none), the
 * samples of its colour and depth attachments (1 or 4), and the subpass's
 * views under multiview (bit n: view n; 0 for one view).
 */
struct powervr_pass {
	uint32_t attachment_count;
	struct powervr_pass_attachment attachments[POWERVR_PASS_ATTACHMENTS];
	uint32_t color_count;
	uint32_t colors[POWERVR_PASS_COLORS];
	uint32_t resolves[POWERVR_PASS_COLORS];
	uint32_t depth;
	uint32_t depth_resolve;
	uint32_t samples;
	uint32_t view_mask;
};

/* The most views a multiview subpass renders (maxMultiviewViewCount): view masks of bits 0 to 7. */
#define POWERVR_PASS_VIEWS		8U

/*
 * One attachment of a framebuffer: the image view's shape (its format,
 * layout, address, level and layer), and the layers of the view, of which
 * a multiview render pass renders its views'.
 */
struct powervr_pass_view {
	struct powervr_view_shape shape;
	uint32_t layers;
};

/* A VkFramebuffer: its extent and layers, and its attachments. */
struct powervr_pass_framebuffer {
	uint32_t width;
	uint32_t height;
	uint32_t layers;
	uint32_t view_count;
	struct powervr_pass_view views[POWERVR_PASS_ATTACHMENTS];
};

/* One clear value: four colour words, or a depth's bits and a stencil. */
struct powervr_pass_clear {
	bool is_depth;
	uint32_t words[4];
};

/*
 * One colour target of a render job: the surface stored to, the view
 * loaded from, its operations, and its clear value as the words of the
 * pixel output registers it takes (powervr_format_output_words: one
 * packed word for four 8-bit unorm channels), which the clear background
 * writes.  A target of four samples with a resolve attachment has
 * resolve set and that attachment's surface, which the job's end writes
 * the samples' average to.
 */
struct powervr_render_target {
	struct powervr_render_surface surface;
	struct powervr_view_shape view;
	uint32_t load_op;
	uint32_t store_op;
	uint32_t clear[4];
	bool resolve;
	struct powervr_render_surface resolve_surface;
};

/*
 * The depth target of a render job, when there is one: its format, the
 * address of its layer, its edges and its padded (physical) edges, its
 * layout and layer size, its operations, its clear value's bits (1.0
 * when it is not cleared), and whether the render area misses part of
 * it (a stored depth is then loaded first, render-depth.c).  A format
 * with a stencil (POWERVR_FORMAT_STENCIL) has the stencil's operations
 * and clear value too.  Samples is its samples a pixel (1 or 4; 0 is
 * taken as 1).
 */
struct powervr_render_depth {
	bool present;
	const struct powervr_format *format;
	uint32_t samples;
	uint64_t address;
	uint32_t width;
	uint32_t height;
	uint32_t physical_width;
	uint32_t physical_height;
	bool twiddled;
	uint64_t layer_bytes;
	uint32_t load_op;
	uint32_t store_op;
	uint32_t clear_depth;
	bool partial;
	uint32_t stencil_load_op;
	uint32_t stencil_store_op;
	uint32_t clear_stencil;
};

/*
 * The dynamic state a command buffer sets for the draws after it, as the
 * set commands give it: the blend constants' float bits, the depth bias
 * (constant factor, clamp and slope factor, float bits), and the stencil
 * compare masks, write masks and references of the front and back faces.
 * A draw's pipeline reads each one only when it made that state dynamic.
 */
struct powervr_draw_dynamic {
	uint32_t blend_constants[4];
	uint32_t depth_bias[3];
	uint32_t stencil_compare_mask[2];
	uint32_t stencil_write_mask[2];
	uint32_t stencil_reference[2];
};

/*
 * One draw of a render job: the graphics pipeline (the executor's
 * powervr_vk_graphics) and descriptor sets bound with their dynamic
 * offsets (in the order of the sets, then of each set's dynamic
 * descriptors by binding and element), the vertex buffers' addresses by
 * binding, the index buffer, the counts and the first instance, the
 * viewport and scissor in force, the other dynamic state, the push
 * constants, and the visibility register it counts in plus one (0 when
 * no occlusion query counts it).
 *
 * A clear draw (clear_mask not 0) is one rectangle of
 * vkCmdClearAttachments inside a render pass instance: it has no
 * pipeline of the client's, the kick draws two triangles over the
 * framebuffer with its own pipeline for the job's attachments, the
 * scissor the rectangle, the colours and depth in the push constants
 * (POWERVR_DRAW_CLEAR_*), writing only the attachments the mask names.
 *
 * An indirect draw (indirect not NULL: the executor's buffer, and the
 * place of its record in it) takes its counts, its first vertex or index
 * and vertex offset, and its first instance from the record once the
 * work before it has finished; until then they are not the draw's.
 */
struct powervr_render_draw {
	const void *pipeline;
	uint32_t clear_mask;
	const void *sets[POWERVR_DRAW_SETS];
	uint32_t dynamic_offset_count;
	uint32_t dynamic_offsets[POWERVR_DRAW_DYNAMIC_OFFSETS];
	uint64_t vertex_addresses[POWERVR_DRAW_VERTEX_BUFFERS];
	bool indexed;
	uint64_t index_address;
	uint32_t index_bytes;
	uint32_t count;
	uint32_t instances;
	uint32_t first_instance;
	uint32_t vertex_offset;
	const void *indirect;
	uint64_t indirect_offset;
	VkViewport viewport;
	VkRect2D scissor;
	struct powervr_draw_dynamic dynamic;
	uint32_t push_bytes;
	uint8_t push[POWERVR_DRAW_PUSH_BYTES];
	uint32_t visibility;
};

/*
 * The visibility registers of a job's draws, the ones an occlusion query
 * counts in (a draw's visibility is its register plus one, 0 for none):
 * how many the job uses, and what is told their counts once the job's 3D
 * finished (counts NULL when they are lost: the job did not finish or
 * never ran).  report owns context and is called once.
 *
 * Value referenced: Mesa 26.2.6 src/imagination/csbgen/rogue/ppp.xml
 * (STATE_ISPCTL visreg 0..13: a register of up to 14 bits; the executor
 * keeps its own smaller room).
 */
#define POWERVR_RENDER_VISIBILITY_MAX	64U

struct powervr_render_visibility {
	uint32_t count;
	void (*report)(struct powervr_render_visibility *visibility, const uint32_t *counts);
};

/*
 * One render job (a render pass instance, or a part of one): the render
 * area, the colour targets in the subpass's order, the depth target, and
 * the draws recorded in it.  Continued is set on a job that goes on with
 * the instance of the job before it (powervr_render_job_continue), so the
 * jobs of one instance are the first one and the continued ones after
 * it.  The kick takes it as it is; a job whose draws count for queries
 * hands its visibility to the kick, which reports it.  Samples is the
 * samples a pixel its attachments and draws have (1 or 4; 0 is taken as
 * 1).  View is the view of a multiview render pass the job renders (the
 * draws' gl_ViewIndex; 0 without multiview), each attachment at its
 * view's layer plus the view.  A job whose depth target is resolved has
 * depth_resolve set, the depth target's view (of four samples) and the
 * surface of one sample its sample 0 goes to over the area once the job
 * has run (only the instance's last job of each view resolves).
 */
struct powervr_render_job {
	struct powervr_render_area area;
	bool continued;
	uint32_t view;
	uint32_t width;
	uint32_t height;
	uint32_t samples;
	uint32_t color_count;
	struct powervr_render_target colors[POWERVR_PASS_COLORS];
	struct powervr_render_depth depth;
	bool depth_resolve;
	struct powervr_view_shape depth_view;
	struct powervr_render_surface depth_resolve_surface;
	uint32_t draw_count;
	struct powervr_render_draw draws[POWERVR_RENDER_DRAWS];
	struct powervr_render_visibility *visibility;
};

/* One attachment of vkCmdClearAttachments: its aspects, its colour index and value. */
struct powervr_pass_clear_attachment {
	uint32_t aspects;
	uint32_t color_index;
	struct powervr_pass_clear value;
};

int powervr_pass_build(const VkRenderPassCreateInfo *info, struct powervr_pass *pass);
int powervr_render_job_begin(const struct powervr_pass *pass, const struct powervr_pass_framebuffer *framebuffer, const struct powervr_render_area *area, const struct powervr_pass_clear *clears, uint32_t clear_count, struct powervr_render_job *job);
int powervr_render_job_continue(struct powervr_render_job *job, struct powervr_render_job *next);
int powervr_render_job_clear(struct powervr_render_job *job, const struct powervr_pass_clear_attachment *attachments, uint32_t count, const VkClearRect *rects, uint32_t rect_count);

#endif /* KERN_DRIVERS_GPU_POWERVR_RENDER_PASS_H */
