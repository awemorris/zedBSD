/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's render passes, framebuffers and render jobs.  See
 * render-pass.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libc/vulkan/vulkan_core.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/image-state.h"
#include "drivers/gpu/powervr/render-pass.h"
#include "drivers/gpu/powervr/render-program.h"

/*
 * The default depth clear value, 1.0.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_sub_cmd_gfx_job_init: default_ds_clear_value depth 1.0f).
 */
#define DEPTH_ONE	0x3f800000U

static int pass_attachment(const VkAttachmentDescription *description, struct powervr_pass_attachment *attachment);
static int target_surface(const struct powervr_view_shape *view, struct powervr_render_surface *surface);
static uint32_t view_samples(const struct powervr_view_shape *view);
static bool area_covers(const struct powervr_render_area *area, const VkClearRect *rect);
static int job_clear_draws(struct powervr_render_job *job, const struct powervr_pass_clear_attachment *attachments, uint32_t count, const VkClearRect *rects, uint32_t rect_count);
static int clear_values(const struct powervr_render_job *job, const struct powervr_pass_clear_attachment *attachments, uint32_t count, uint32_t *mask, uint8_t push[POWERVR_DRAW_PUSH_BYTES], uint32_t *stencil);
static bool clear_rect_clip(const struct powervr_render_area *area, const VkClearRect *rect, VkRect2D *clipped);
static void put_word(uint8_t *bytes, uint32_t value);

/*
 * Keeps what a VkRenderPass of the supported shape needs: one subpass,
 * at most four colour attachments and one depth attachment, formats the
 * executor takes, one or four samples (the same for all of them, EINVAL
 * otherwise), a resolve attachment of one sample and the same format for
 * a colour attachment of four, no input or preserved attachment.
 *
 * Any other shape is refused with ENOBUFS, which the reply turns into
 * VK_ERROR_OUT_OF_HOST_MEMORY (one of the failures vkCreateRenderPass
 * may report).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_pass.c (pvr_CreateRenderPass2:
 * the attachments' formats and operations, the subpasses' colour and
 * depth references); Mesa 26.2.6 src/imagination/vulkan/pvr_arch_pass.c
 * (the subpass's sample count from its attachments, the resolve
 * attachments); the Vulkan 1.3 specification, VkSubpassDescription
 * valid usage (the colour and depth attachments of one sample count, a
 * resolve attachment of one sample and its colour attachment's format,
 * its colour attachment of more than one).
 */
int
powervr_pass_build(
	const VkRenderPassCreateInfo *info,
	struct powervr_pass *pass)
{
	const VkSubpassDescription *subpass;
	uint32_t reference;
	uint32_t index;
	int error;

	/* Refuses missing input or result. */
	if (info == NULL || pass == NULL)
		return EINVAL;

	/* One subpass of at most four colour attachments, and no more attachments than a pass holds. */
	if (info->subpassCount != 1 || info->pSubpasses == NULL)
		return ENOBUFS;
	if (info->attachmentCount > POWERVR_PASS_ATTACHMENTS)
		return ENOBUFS;
	subpass = &info->pSubpasses[0];
	if (subpass->pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS || subpass->colorAttachmentCount > POWERVR_PASS_COLORS)
		return ENOBUFS;
	if (subpass->inputAttachmentCount != 0 || subpass->preserveAttachmentCount != 0)
		return ENOBUFS;

	/* Each attachment's format and operations. */
	pass->attachment_count = info->attachmentCount;
	for (index = 0; index < info->attachmentCount; index++) {
		error = pass_attachment(&info->pAttachments[index], &pass->attachments[index]);
		if (error != 0)
			return error;
	}

	/* The colour references, each a colour attachment of the pass or unused. */
	pass->color_count = subpass->colorAttachmentCount;
	for (index = 0; index < subpass->colorAttachmentCount; index++) {
		reference = subpass->pColorAttachments[index].attachment;
		if (reference != VK_ATTACHMENT_UNUSED && reference >= info->attachmentCount)
			return EINVAL;
		pass->colors[index] = reference == VK_ATTACHMENT_UNUSED ? POWERVR_PASS_UNUSED : reference;
	}

	/* The depth reference, a depth attachment of the pass or none. */
	pass->depth = POWERVR_PASS_UNUSED;
	if (subpass->pDepthStencilAttachment != NULL && subpass->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED) {
		reference = subpass->pDepthStencilAttachment->attachment;
		if (reference >= info->attachmentCount)
			return EINVAL;
		pass->depth = reference;
	}

	/* The samples of the colour and depth attachments, one count for all of them. */
	pass->samples = 0;
	for (index = 0; index <= subpass->colorAttachmentCount; index++) {
		reference = index < subpass->colorAttachmentCount ? pass->colors[index] : pass->depth;
		if (reference == POWERVR_PASS_UNUSED)
			continue;
		if (pass->samples != 0 && pass->attachments[reference].samples != pass->samples)
			return EINVAL;
		pass->samples = pass->attachments[reference].samples;
	}
	if (pass->samples == 0)
		pass->samples = 1;

	/* The resolve references: each of one sample and its colour attachment's format, its colour attachment of four; the depth's is the caller's. */
	pass->depth_resolve = POWERVR_PASS_UNUSED;
	for (index = 0; index < POWERVR_PASS_COLORS; index++)
		pass->resolves[index] = POWERVR_PASS_UNUSED;
	for (index = 0; subpass->pResolveAttachments != NULL && index < subpass->colorAttachmentCount; index++) {
		reference = subpass->pResolveAttachments[index].attachment;
		if (reference == VK_ATTACHMENT_UNUSED)
			continue;
		if (reference >= info->attachmentCount || pass->colors[index] == POWERVR_PASS_UNUSED)
			return EINVAL;
		if (pass->attachments[reference].samples != 1U || pass->samples == 1U ||
		    pass->attachments[reference].format != pass->attachments[pass->colors[index]].format)
			return EINVAL;
		pass->resolves[index] = reference;
	}

	/* Succeeded: the pass is kept. */
	return 0;
}

/*
 * Starts a render job: the pass's attachments in the framebuffer, the
 * render area and the clear values (indexed by attachment).
 *
 * The area must lie in the framebuffer (EINVAL).  A colour attachment
 * takes a partial area (the pixel back end clips to it); so does a depth
 * attachment, whose store writes whole tiles: the job marks it partial
 * and the depth is loaded before it is stored, as Mesa does
 * (render-depth.c).  A depth view of a level
 * other than 0 is not taken yet (ENOTSUP).  A cleared attachment without
 * its clear value is refused (EINVAL).  A depth attachment the pass
 * resolves is stored whatever its store operations, and the job keeps its
 * view and its resolve attachment's surface (vk-transfer.c resolves it
 * after the job).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_cmd_buffer_begin_render_pass and pvr_setup_pbe_state: each
 * colour attachment's image view level and layer as the surface).
 */
int
powervr_render_job_begin(
	const struct powervr_pass *pass,
	const struct powervr_pass_framebuffer *framebuffer,
	const struct powervr_render_area *area,
	const struct powervr_pass_clear *clears,
	uint32_t clear_count,
	struct powervr_render_job *job)
{
	const struct powervr_pass_attachment *attachment;
	const struct powervr_view_shape *view;
	struct powervr_render_target *target;
	uint32_t reference;
	uint32_t index;
	int error;

	/* Refuses missing input or result. */
	if (pass == NULL || framebuffer == NULL || area == NULL || job == NULL || (clear_count != 0 && clears == NULL))
		return EINVAL;

	/* The area: not empty, inside the framebuffer. */
	if (area->width == 0 || area->height == 0)
		return EINVAL;
	if (area->x >= framebuffer->width || area->width > framebuffer->width - area->x)
		return EINVAL;
	if (area->y >= framebuffer->height || area->height > framebuffer->height - area->y)
		return EINVAL;

	/* The framebuffer must hold the pass's attachments. */
	if (framebuffer->view_count < pass->attachment_count)
		return EINVAL;

	/* An empty job over the area. */
	*job = (struct powervr_render_job){ 0 };
	job->area = *area;
	job->width = framebuffer->width;
	job->height = framebuffer->height;
	job->samples = pass->samples;

	/* The colour targets in the subpass's order. */
	job->color_count = pass->color_count;
	for (index = 0; index < pass->color_count; index++) {
		target = &job->colors[index];
		reference = pass->colors[index];
		target->load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		target->store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		if (reference == POWERVR_PASS_UNUSED)
			continue;
		attachment = &pass->attachments[reference];
		view = &framebuffer->views[reference].shape;
		if (view->format == NULL || view->format->vk_format != attachment->format || (view->format->kind & POWERVR_FORMAT_COLOR) == 0)
			return EINVAL;
		if (view_samples(view) != attachment->samples)
			return EINVAL;
		error = target_surface(view, &target->surface);
		if (error != 0)
			return error;
		target->view = *view;

		/* Its resolve attachment: a colour view of one sample, of the same format. */
		if (pass->resolves[index] != POWERVR_PASS_UNUSED) {
			view = &framebuffer->views[pass->resolves[index]].shape;
			if (view->format == NULL || view->format->vk_format != attachment->format || view_samples(view) != 1U)
				return EINVAL;
			error = target_surface(view, &target->resolve_surface);
			if (error != 0)
				return error;
			target->resolve = true;
			view = &framebuffer->views[reference].shape;
		}
		target->load_op = attachment->load_op;
		target->store_op = attachment->store_op;
		if (attachment->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
			if (reference >= clear_count || clears[reference].is_depth)
				return EINVAL;

			/* The clear value as the format's pixel output register words, which the background writes. */
			error = powervr_format_output_words(view->format, clears[reference].words, target->clear);
			if (error != 0)
				return error;
		}
	}

	/* The depth target, when the subpass has one. */
	if (pass->depth != POWERVR_PASS_UNUSED) {
		attachment = &pass->attachments[pass->depth];
		view = &framebuffer->views[pass->depth].shape;
		if (view->format == NULL || view->format->vk_format != attachment->format || (view->format->kind & POWERVR_FORMAT_DEPTH) == 0)
			return EINVAL;
		if (view->layout == NULL || view->base_level != 0)
			return ENOTSUP;
		if (view_samples(view) != attachment->samples)
			return EINVAL;
		job->depth.present = true;
		job->depth.format = view->format;
		job->depth.samples = pass->samples;
		job->depth.address = view->address + (uint64_t)view->base_layer * view->layout->layer_bytes;
		job->depth.width = view->width;
		job->depth.height = view->height;
		job->depth.physical_width = view->layout->padded_width;
		job->depth.physical_height = view->layout->padded_height;
		job->depth.twiddled = !view->linear;
		job->depth.layer_bytes = view->layout->layer_bytes;
		job->depth.load_op = attachment->load_op;
		job->depth.store_op = attachment->store_op;
		job->depth.clear_depth = DEPTH_ONE;
		job->depth.stencil_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		job->depth.stencil_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		job->depth.clear_stencil = 0;
		if ((view->format->kind & POWERVR_FORMAT_STENCIL) != 0) {
			job->depth.stencil_load_op = attachment->stencil_load_op;
			job->depth.stencil_store_op = attachment->stencil_store_op;
		}
		if (job->depth.load_op == VK_ATTACHMENT_LOAD_OP_CLEAR || job->depth.stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
			if (pass->depth >= clear_count || !clears[pass->depth].is_depth)
				return EINVAL;
			if (job->depth.load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
				job->depth.clear_depth = clears[pass->depth].words[0];
			job->depth.clear_stencil = clears[pass->depth].words[1] & 0xffU;
		}

		/*
		 * Its resolve attachment: a depth view of one sample, of the same
		 * format.  The transfer reads sample 0 out of the stored depth (and
		 * stencil), which is stored whatever its store operation.
		 */
		if (pass->depth_resolve != POWERVR_PASS_UNUSED) {
			if (pass->samples != 4U)
				return EINVAL;
			job->depth_view = *view;
			view = &framebuffer->views[pass->depth_resolve].shape;
			if (view->format == NULL || view->format->vk_format != attachment->format || view_samples(view) != 1U)
				return EINVAL;
			error = target_surface(view, &job->depth_resolve_surface);
			if (error != 0)
				return error;
			job->depth_resolve = true;
			job->depth.store_op = VK_ATTACHMENT_STORE_OP_STORE;
			if ((job->depth.format->kind & POWERVR_FORMAT_STENCIL) != 0)
				job->depth.stencil_store_op = VK_ATTACHMENT_STORE_OP_STORE;
			view = &framebuffer->views[pass->depth].shape;
		}

		/* An area short of the depth view (as Mesa's pvr_is_render_area_tile_aligned): the depth outside it is kept by a load before the store. */
		job->depth.partial = false;
		if (area->x != 0 || area->y != 0 || area->width != view->width || area->height != view->height)
			job->depth.partial = true;
	}

	/* Succeeded: the job is started. */
	return 0;
}

/*
 * Applies vkCmdClearAttachments to a job.
 *
 * A clear of the whole render area of layer 0 before any draw is the
 * attachments' load clear.  Any other clear (part of the area, or after
 * draws) becomes a clear draw for each rectangle, in the job's order of
 * draws (job_clear_draws).  A colour index past the subpass's, an
 * unused attachment and a stencil clear of a depth target without one
 * are refused (EINVAL); a rectangle of a layer other than 0 and more
 * clear draws than the job has room for are not taken (ENOTSUP, E2BIG).
 */
int
powervr_render_job_clear(
	struct powervr_render_job *job,
	const struct powervr_pass_clear_attachment *attachments,
	uint32_t count,
	const VkClearRect *rects,
	uint32_t rect_count)
{
	struct powervr_render_target *target;
	uint32_t index;
	uint32_t rect;
	bool whole;
	bool covers;
	int error;

	/* Refuses missing input. */
	if (job == NULL || (count != 0 && attachments == NULL) || (rect_count != 0 && rects == NULL))
		return EINVAL;

	/* A load clear only before any draw, over the whole area of layer 0. */
	whole = rect_count != 0 && job->draw_count == 0;
	for (rect = 0; rect < rect_count && whole; rect++) {
		covers = area_covers(&job->area, &rects[rect]);
		if (!covers)
			whole = false;
	}

	/* Any other clear is drawn. */
	if (!whole) {
		error = job_clear_draws(job, attachments, count, rects, rect_count);
		return error;
	}

	/* Each attachment becomes a load clear with its value. */
	for (index = 0; index < count; index++) {
		if ((attachments[index].aspects & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
			if (attachments[index].color_index >= job->color_count || attachments[index].value.is_depth)
				return EINVAL;
			target = &job->colors[attachments[index].color_index];
			if (target->surface.format == NULL)
				return EINVAL;
			target->load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
			error = powervr_format_output_words(target->surface.format, attachments[index].value.words, target->clear);
			if (error != 0)
				return error;
		} else if ((attachments[index].aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0) {
			if (!job->depth.present || !attachments[index].value.is_depth)
				return EINVAL;
			if ((attachments[index].aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 &&
			    (job->depth.format == NULL || (job->depth.format->kind & POWERVR_FORMAT_STENCIL) == 0))
				return EINVAL;
			if ((attachments[index].aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
				job->depth.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
				job->depth.clear_depth = attachments[index].value.words[0];
			}
			if ((attachments[index].aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
				job->depth.stencil_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
				job->depth.clear_stencil = attachments[index].value.words[1] & 0xffU;
			}
		} else {
			return ENOTSUP;
		}
	}

	/* Succeeded: the clears are the job's load clears. */
	return 0;
}

/*
 * Continues a render job whose draws filled its room in a next job of the
 * same area and targets: the full job stores every target, and the next
 * one loads what the full one stored, then stores as the pass asked.  The
 * next job starts without draws.  A depth target is carried the same way,
 * so the depth test goes on across the two jobs.
 */
int
powervr_render_job_continue(
	struct powervr_render_job *job,
	struct powervr_render_job *next)
{
	uint32_t index;

	/* Refuses missing input and a job that is not full. */
	if (job == NULL || next == NULL || job == next || job->draw_count != POWERVR_RENDER_DRAWS)
		return EINVAL;

	/* The next job: the same area and targets, no draws. */
	__builtin_memcpy(next, job, offsetof(struct powervr_render_job, draws));
	next->draw_count = 0;
	next->continued = true;

	/* Each colour target: stored by the full job, loaded by the next one, resolved by the instance's last job only. */
	for (index = 0; index < job->color_count; index++) {
		job->colors[index].store_op = VK_ATTACHMENT_STORE_OP_STORE;
		job->colors[index].resolve = false;
		next->colors[index].load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
	}

	/* The depth target likewise, and its stencil; its resolve the last job's. */
	job->depth_resolve = false;
	if (job->depth.present) {
		job->depth.store_op = VK_ATTACHMENT_STORE_OP_STORE;
		next->depth.load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
		if (job->depth.format != NULL && (job->depth.format->kind & POWERVR_FORMAT_STENCIL) != 0) {
			job->depth.stencil_store_op = VK_ATTACHMENT_STORE_OP_STORE;
			next->depth.stencil_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
		}
	}

	/* Succeeded: the next job goes on where the full one stops. */
	return 0;
}

/* Keeps one attachment of a pass: a format the executor takes, one or four samples, the core operations. */
static int
pass_attachment(
	const VkAttachmentDescription *description,
	struct powervr_pass_attachment *attachment)
{
	const struct powervr_format *format;

	/* A format the executor renders to, one or four samples. */
	format = powervr_format_find(description->format);
	if (format == NULL || (description->samples != VK_SAMPLE_COUNT_1_BIT && description->samples != VK_SAMPLE_COUNT_4_BIT))
		return ENOBUFS;
	if ((format->kind & (POWERVR_FORMAT_COLOR | POWERVR_FORMAT_DEPTH)) == 0)
		return ENOBUFS;

	/* The load and store operations of Vulkan 1.0, of the depth or colour and of the stencil. */
	if (description->loadOp > VK_ATTACHMENT_LOAD_OP_DONT_CARE || description->storeOp > VK_ATTACHMENT_STORE_OP_DONT_CARE)
		return ENOBUFS;
	if (description->stencilLoadOp > VK_ATTACHMENT_LOAD_OP_DONT_CARE || description->stencilStoreOp > VK_ATTACHMENT_STORE_OP_DONT_CARE)
		return ENOBUFS;

	/* The format, the samples and the operations. */
	attachment->format = description->format;
	attachment->samples = description->samples;
	attachment->load_op = description->loadOp;
	attachment->store_op = description->storeOp;
	attachment->stencil_load_op = description->stencilLoadOp;
	attachment->stencil_store_op = description->stencilStoreOp;

	/* Succeeded: the attachment is kept. */
	return 0;
}

/* Reports a view's samples a texel: 4 of a multisampled image, else 1. */
static uint32_t
view_samples(
	const struct powervr_view_shape *view)
{
	/* The image layout's count. */
	if (view->layout != NULL && view->layout->samples == 4U)
		return 4U;
	return 1U;
}

/*
 * The surface the pixel back end stores a colour view to: the view's
 * level and layer of its image, its level's edges, the linear row or the
 * twiddled padded edge of that level.
 */
static int
target_surface(
	const struct powervr_view_shape *view,
	struct powervr_render_surface *surface)
{
	uint32_t level;

	/* Refuses a view without a layout or past the level table. */
	if (view->layout == NULL || view->base_level >= POWERVR_IMAGE_LEVELS_MAX)
		return EINVAL;

	/* The level's place and edges (at least one texel). */
	level = view->base_level;
	surface->format = view->format;
	surface->address = view->address + (uint64_t)view->base_layer * view->layout->layer_bytes + view->layout->level_offset[level];
	surface->width = view->width >> level;
	if (surface->width == 0)
		surface->width = 1;
	surface->height = view->height >> level;
	if (surface->height == 0)
		surface->height = 1;
	surface->linear = view->linear;
	surface->row_pitch = view->layout->row_pitch;
	surface->padded_width = view->layout->padded_width >> level;
	if (surface->padded_width == 0)
		surface->padded_width = 1;
	surface->depth = 0;
	surface->slice = 0;

	/* Succeeded: the surface. */
	return 0;
}

/* Says whether a clear rectangle covers the whole area on layer 0 alone. */
static bool
area_covers(
	const struct powervr_render_area *area,
	const VkClearRect *rect)
{
	/* Layer 0 only. */
	if (rect->baseArrayLayer != 0 || rect->layerCount != 1)
		return false;

	/* The rectangle holds the area. */
	if (rect->rect.offset.x > (int32_t)area->x || rect->rect.offset.y > (int32_t)area->y)
		return false;
	if ((int64_t)rect->rect.offset.x + rect->rect.extent.width < (int64_t)area->x + area->width)
		return false;
	if ((int64_t)rect->rect.offset.y + rect->rect.extent.height < (int64_t)area->y + area->height)
		return false;

	/* It covers it. */
	return true;
}

/*
 * Adds a clear draw to a job for each rectangle of a clear that is not
 * the attachments' load clear: the rectangle clipped to the render area
 * as its scissor, the attachments' mask and values in its push
 * constants.  A rectangle outside the area adds nothing.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_clear.c
 * (pvr_clear_attachments: a clear inside a render pass is drawn as a
 * rectangle with the clear colour and depth, through the attachments the
 * clear names, the others left alone).
 */
static int
job_clear_draws(
	struct powervr_render_job *job,
	const struct powervr_pass_clear_attachment *attachments,
	uint32_t count,
	const VkClearRect *rects,
	uint32_t rect_count)
{
	struct powervr_render_draw *draw;
	uint8_t push[POWERVR_DRAW_PUSH_BYTES];
	VkRect2D clipped;
	uint32_t stencil;
	uint32_t mask;
	uint32_t rect;
	bool inside;
	int error;

	/* The attachments the clear writes and their values. */
	error = clear_values(job, attachments, count, &mask, push, &stencil);
	if (error != 0)
		return error;

	/* Nothing to write is no draw. */
	if (mask == 0)
		return 0;

	/* Each rectangle of layer 0 as a draw, while the job has room. */
	for (rect = 0; rect < rect_count; rect++) {
		if (rects[rect].baseArrayLayer != 0)
			return ENOTSUP;

		/* The part inside the render area; none is no draw. */
		inside = clear_rect_clip(&job->area, &rects[rect], &clipped);
		if (!inside)
			continue;

		/* The job's next draw. */
		if (job->draw_count == POWERVR_RENDER_DRAWS)
			return E2BIG;
		draw = &job->draws[job->draw_count];
		__builtin_memset(draw, 0, sizeof(*draw));
		draw->clear_mask = mask;
		draw->scissor = clipped;
		draw->count = 6;
		draw->instances = 1;
		draw->push_bytes = POWERVR_DRAW_PUSH_BYTES;
		__builtin_memcpy(draw->push, push, sizeof(push));
		draw->dynamic.stencil_reference[0] = stencil;
		draw->dynamic.stencil_reference[1] = stencil;
		draw->dynamic.stencil_compare_mask[0] = 0xffU;
		draw->dynamic.stencil_compare_mask[1] = 0xffU;
		draw->dynamic.stencil_write_mask[0] = 0xffU;
		draw->dynamic.stencil_write_mask[1] = 0xffU;
		job->draw_count++;
	}

	/* Succeeded: the clear's draws are the job's. */
	return 0;
}

/*
 * Works out a clear's mask of attachments and its push constants: each
 * colour attachment's four words at its place, the depth's bits after
 * them, and the stencil's value (the clear draw's stencil reference).
 * Refuses a colour index past the subpass's, an unused attachment, a
 * depth clear without a depth target and a stencil clear without a
 * stencil (EINVAL).
 */
static int
clear_values(
	const struct powervr_render_job *job,
	const struct powervr_pass_clear_attachment *attachments,
	uint32_t count,
	uint32_t *mask,
	uint8_t push[POWERVR_DRAW_PUSH_BYTES],
	uint32_t *stencil)
{
	const struct powervr_pass_clear_attachment *attachment;
	uint32_t index;
	uint32_t place;
	uint32_t word;

	/* No attachment and no value yet. */
	*mask = 0;
	*stencil = 0;
	__builtin_memset(push, 0, POWERVR_DRAW_PUSH_BYTES);

	/* Each attachment's bit and value. */
	for (index = 0; index < count; index++) {
		attachment = &attachments[index];

		/* A colour attachment of the subpass that the job draws to. */
		if ((attachment->aspects & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
			if (attachment->color_index >= job->color_count || attachment->value.is_depth)
				return EINVAL;
			if (job->colors[attachment->color_index].surface.format == NULL)
				return EINVAL;

			*mask |= 1U << attachment->color_index;
			place = attachment->color_index * POWERVR_DRAW_CLEAR_COLOR_BYTES;
			for (word = 0; word < 4U; word++)
				put_word(&push[place + word * 4U], attachment->value.words[word]);
			continue;
		}

		/* The depth and the stencil, when the job's depth target has them. */
		if ((attachment->aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0) {
			if (!job->depth.present || !attachment->value.is_depth)
				return EINVAL;
			if ((attachment->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 &&
			    (job->depth.format == NULL || (job->depth.format->kind & POWERVR_FORMAT_STENCIL) == 0))
				return EINVAL;

			if ((attachment->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
				*mask |= POWERVR_DRAW_CLEAR_DEPTH;
				put_word(&push[POWERVR_DRAW_CLEAR_DEPTH_OFFSET], attachment->value.words[0]);
			}
			if ((attachment->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
				*mask |= POWERVR_DRAW_CLEAR_STENCIL;
				*stencil = attachment->value.words[1] & 0xffU;
			}
			continue;
		}

		/* No aspect the clear knows. */
		return ENOTSUP;
	}

	/* Succeeded: the mask and the values. */
	return 0;
}

/* Clips a clear rectangle to the render area; false when nothing of it is inside. */
static bool
clear_rect_clip(
	const struct powervr_render_area *area,
	const VkClearRect *rect,
	VkRect2D *clipped)
{
	int64_t left;
	int64_t top;
	int64_t right;
	int64_t bottom;

	/* The rectangle's edges. */
	left = rect->rect.offset.x;
	top = rect->rect.offset.y;
	right = left + rect->rect.extent.width;
	bottom = top + rect->rect.extent.height;

	/* Within the area's edges. */
	if (left < (int64_t)area->x)
		left = area->x;
	if (top < (int64_t)area->y)
		top = area->y;
	if (right > (int64_t)area->x + area->width)
		right = (int64_t)area->x + area->width;
	if (bottom > (int64_t)area->y + area->height)
		bottom = (int64_t)area->y + area->height;

	/* Nothing left of it. */
	if (right <= left || bottom <= top || rect->layerCount == 0)
		return false;

	/* Succeeded: the part inside. */
	clipped->offset.x = (int32_t)left;
	clipped->offset.y = (int32_t)top;
	clipped->extent.width = (uint32_t)(right - left);
	clipped->extent.height = (uint32_t)(bottom - top);
	return true;
}

/* Stores a 32-bit word in little-endian order. */
static void
put_word(
	uint8_t *bytes,
	uint32_t value)
{
	/* Each byte, lowest first. */
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}
