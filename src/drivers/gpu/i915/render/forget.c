/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a session's objects let go of when one of them is destroyed (see
 * forget.h, BUG-260).
 *
 * Each holder is found by a walk of the session's objects of its kind
 * (drv_i915_object_each, under the table's lock); a walk only clears the
 * holder's pointer, so it takes no other lock and frees nothing but a
 * command buffer's recorded video records (command.c).
 */

#include "forget.h"
#include "object.h"

#include <kern/klog.h>

#include <stddef.h>
#include <stdint.h>

/*
 * An object being destroyed and how many holders let go of it, for one
 * walk of the holders of one kind.  It lives on the stack of the destroy.
 */
struct forget_walk {
	const void *object;
	unsigned count;
};

static void forget_buffer_view_buffer(void *holder, void *argument);
static void forget_view_image(void *holder, void *argument);
static void forget_set(void *holder, void *argument);
static void forget_set_orphans(void *holder, void *argument);
static void forget_framebuffer(void *holder, void *argument);
static void forget_framebuffer_orphans(void *holder, void *argument);
static unsigned forget_kind(struct i915_render_session *session, enum i915_vk_object_kind kind, void (*visit)(void *holder, void *argument), const void *object);

/*
 * Lets every holder of an object of the session go of it, before the
 * object is freed.
 *
 * A buffer leaves its buffer views, the sets that name it and the command
 * buffers that recorded it; an image leaves its views (which then keep no
 * image, and are let go of by the sets and framebuffers that name them)
 * and the command buffers; a view, a buffer view or a sampler leaves the
 * sets (a view the framebuffers too); a set, a pipeline, a render pass, a
 * framebuffer or a query pool leaves the command buffers that recorded it.
 * Any other kind is named by nothing.  What let go of the object is
 * logged, since only an application that breaks the Vulkan rules leaves a
 * holder behind.
 */
void
drv_i915_gfx_forget(
	struct i915_render_session *session,
	enum i915_vk_object_kind kind,
	void *object)
{
	unsigned views;
	unsigned sets;
	unsigned framebuffers;
	unsigned commands;

	/* Nothing named. */
	if (object == NULL)
		return;

	/* Each kind's holders. */
	views = 0U;
	sets = 0U;
	framebuffers = 0U;
	commands = 0U;
	switch (kind) {
	case I915_VK_OBJ_BUFFER:
		views = forget_kind(session, I915_VK_OBJ_BUFFER_VIEW, forget_buffer_view_buffer, object);
		sets = forget_kind(session, I915_VK_OBJ_DESCRIPTOR_SET, forget_set, object);
		commands = drv_i915_gfx_command_forget(session, object);
		break;
	case I915_VK_OBJ_IMAGE:
		/* Its views keep no image; the sets and framebuffers that name such a view let go of it. */
		views = forget_kind(session, I915_VK_OBJ_IMAGE_VIEW, forget_view_image, object);
		sets = forget_kind(session, I915_VK_OBJ_DESCRIPTOR_SET, forget_set_orphans, NULL);
		framebuffers = forget_kind(session, I915_VK_OBJ_FRAMEBUFFER, forget_framebuffer_orphans, NULL);
		commands = drv_i915_gfx_command_forget(session, object);
		break;
	case I915_VK_OBJ_IMAGE_VIEW:
		sets = forget_kind(session, I915_VK_OBJ_DESCRIPTOR_SET, forget_set, object);
		framebuffers = forget_kind(session, I915_VK_OBJ_FRAMEBUFFER, forget_framebuffer, object);
		break;
	case I915_VK_OBJ_BUFFER_VIEW:
	case I915_VK_OBJ_SAMPLER:
		sets = forget_kind(session, I915_VK_OBJ_DESCRIPTOR_SET, forget_set, object);
		break;
	case I915_VK_OBJ_DESCRIPTOR_SET:
	case I915_VK_OBJ_PIPELINE:
	case I915_VK_OBJ_RENDER_PASS:
	case I915_VK_OBJ_FRAMEBUFFER:
	case I915_VK_OBJ_QUERY_POOL:
		commands = drv_i915_gfx_command_forget(session, object);
		break;
	default:
		/* Named by nothing (a set copies its layout when it is allocated). */
		break;
	}

	/* An application that left holders behind is told in the log. */
	if (views != 0U ||
	    sets != 0U ||
	    framebuffers != 0U ||
	    commands != 0U)
		kern_logf("i915: vk: an object (kind %u) was destroyed before what names it: %u views, %u descriptor sets, %u framebuffers and %u command buffers let go of it\n",
			  (unsigned)kind, views, sets, framebuffers, commands);
}

/*
 * Finds a live image view of the session by its identity: NULL for an
 * unknown one, and for one whose image was destroyed (it keeps no image,
 * and nothing may name it again).
 */
struct i915_gfx_view *
drv_i915_gfx_view_lookup(
	struct i915_render_session *session,
	uint64_t identity)
{
	struct i915_gfx_view *view;

	/* The view. */
	view = drv_i915_object_lookup(session, I915_VK_OBJ_IMAGE_VIEW, identity);
	if (view == NULL)
		return NULL;

	/* One without its image is as good as gone. */
	if (view->image == NULL) {
		kern_logf("i915: vk: an image view whose image was destroyed is named again; it is taken as unknown\n");
		return NULL;
	}

	/* Succeeded: the view. */
	return view;
}

/*
 * Tells whether a recorded operation names an object: a buffer, an image,
 * a descriptor set, a pipeline, a render pass, a framebuffer or a query
 * pool it was recorded with.  Returns 1 when it does, 0 otherwise (a video
 * operation names its own record alone).
 */
int
drv_i915_gfx_op_names(
	const struct i915_gfx_op *op,
	const void *object)
{
	/* Each kind's objects. */
	switch (op->kind) {
	case I915_GFX_OP_COPY_BUFFER_TO_IMAGE:
	case I915_GFX_OP_COPY_IMAGE_TO_BUFFER:
		if (op->u.copy.buffer == object)
			return 1;
		if (op->u.copy.image == object)
			return 1;
		break;
	case I915_GFX_OP_COPY_IMAGE:
	case I915_GFX_OP_RESOLVE_IMAGE:
		if (op->u.image_copy.src == object)
			return 1;
		if (op->u.image_copy.dst == object)
			return 1;
		break;
	case I915_GFX_OP_BLIT_IMAGE:
		if (op->u.blit.src == object)
			return 1;
		if (op->u.blit.dst == object)
			return 1;
		break;
	case I915_GFX_OP_CLEAR_IMAGE:
		if (op->u.clear_image.image == object)
			return 1;
		break;
	case I915_GFX_OP_BEGIN_PASS:
		if (op->u.begin.pass == object)
			return 1;
		if (op->u.begin.framebuffer == object)
			return 1;
		break;
	case I915_GFX_OP_BIND_PIPELINE:
		if (op->u.pipeline == object)
			return 1;
		break;
	case I915_GFX_OP_BIND_VERTEX_BUFFER:
		if (op->u.vertex.buffer == object)
			return 1;
		break;
	case I915_GFX_OP_BIND_DESCRIPTOR_SET:
		if (op->u.descriptor.dset == object)
			return 1;
		break;
	case I915_GFX_OP_BIND_INDEX_BUFFER:
		if (op->u.index.buffer == object)
			return 1;
		break;
	case I915_GFX_OP_DISPATCH_INDIRECT:
		if (op->u.dispatch_indirect.buffer == object)
			return 1;
		break;
	case I915_GFX_OP_COPY_BUFFER:
		if (op->u.buffer_copy.src == object)
			return 1;
		if (op->u.buffer_copy.dst == object)
			return 1;
		break;
	case I915_GFX_OP_QUERY_BEGIN:
	case I915_GFX_OP_QUERY_END:
	case I915_GFX_OP_QUERY_RESET:
		if (op->u.query.pool == object)
			return 1;
		break;
	default:
		/* The other operations name no object. */
		break;
	}

	/* Succeeded: the operation does not name it. */
	return 0;
}

/* Lets a buffer view of the buffer being destroyed go of it (it then reads nothing). */
static void
forget_buffer_view_buffer(
	void *holder,
	void *argument)
{
	struct i915_gfx_buffer_view *view;
	struct forget_walk *walk;

	/* Only a view of that buffer. */
	view = holder;
	walk = argument;
	if (view->buffer != walk->object)
		return;

	/* No buffer from here on. */
	view->buffer = NULL;
	walk->count++;
}

/* Lets a view of the image being destroyed go of it: it keeps no image. */
static void
forget_view_image(
	void *holder,
	void *argument)
{
	struct i915_gfx_view *view;
	struct forget_walk *walk;

	/* Only a view of that image. */
	view = holder;
	walk = argument;
	if (view->image != walk->object)
		return;

	/* No image from here on. */
	view->image = NULL;
	walk->count++;
}

/* Lets a descriptor set go of the view, buffer view, sampler or buffer being destroyed, in every binding. */
static void
forget_set(
	void *holder,
	void *argument)
{
	struct i915_gfx_dset *set;
	struct forget_walk *walk;
	uint32_t binding;
	int named;

	/* Each binding that names it lets go of it. */
	set = holder;
	walk = argument;
	named = 0;
	for (binding = 0U; binding < I915_GFX_MAX_BINDINGS; binding++) {
		/* A view. */
		if ((const void *)set->slots[binding].view == walk->object) {
			set->slots[binding].view = NULL;
			named = 1;
		}

		/* A buffer view. */
		if ((const void *)set->slots[binding].texel == walk->object) {
			set->slots[binding].texel = NULL;
			named = 1;
		}

		/* A sampler. */
		if ((const void *)set->slots[binding].sampler == walk->object) {
			set->slots[binding].sampler = NULL;
			named = 1;
		}

		/* A buffer. */
		if ((const void *)set->slots[binding].buffer == walk->object) {
			set->slots[binding].buffer = NULL;
			named = 1;
		}
	}

	/* One more set that named it. */
	if (named)
		walk->count++;
}

/* Lets a descriptor set go of every view it names that keeps no image (its image was destroyed). */
static void
forget_set_orphans(
	void *holder,
	void *argument)
{
	struct i915_gfx_dset *set;
	struct forget_walk *walk;
	uint32_t binding;
	int named;

	/* Each binding with such a view lets go of it. */
	set = holder;
	walk = argument;
	named = 0;
	for (binding = 0U; binding < I915_GFX_MAX_BINDINGS; binding++) {
		if (set->slots[binding].view == NULL)
			continue;
		if (set->slots[binding].view->image != NULL)
			continue;
		set->slots[binding].view = NULL;
		named = 1;
	}

	/* One more set that named one. */
	if (named)
		walk->count++;
}

/* Lets a framebuffer go of the view being destroyed, in every attachment. */
static void
forget_framebuffer(
	void *holder,
	void *argument)
{
	struct i915_gfx_framebuffer *framebuffer;
	struct forget_walk *walk;
	uint32_t index;
	int named;

	/* Each attachment that names it has no view. */
	framebuffer = holder;
	walk = argument;
	named = 0;
	for (index = 0U; index < I915_GFX_MAX_ATTACHMENTS; index++) {
		if ((const void *)framebuffer->views[index] != walk->object)
			continue;
		framebuffer->views[index] = NULL;
		named = 1;
	}

	/* One more framebuffer that named it. */
	if (named)
		walk->count++;
}

/* Lets a framebuffer go of every view it names that keeps no image (its image was destroyed). */
static void
forget_framebuffer_orphans(
	void *holder,
	void *argument)
{
	struct i915_gfx_framebuffer *framebuffer;
	struct forget_walk *walk;
	uint32_t index;
	int named;

	/* Each attachment with such a view has no view. */
	framebuffer = holder;
	walk = argument;
	named = 0;
	for (index = 0U; index < I915_GFX_MAX_ATTACHMENTS; index++) {
		if (framebuffer->views[index] == NULL)
			continue;
		if (framebuffer->views[index]->image != NULL)
			continue;
		framebuffer->views[index] = NULL;
		named = 1;
	}

	/* One more framebuffer that named one. */
	if (named)
		walk->count++;
}

/* Walks the session's objects of one kind with a visit about an object; reports how many let go of it. */
static unsigned
forget_kind(
	struct i915_render_session *session,
	enum i915_vk_object_kind kind,
	void (*visit)(void *holder, void *argument),
	const void *object)
{
	struct forget_walk walk;

	/* The walk, under the table's lock. */
	walk.object = object;
	walk.count = 0U;
	drv_i915_object_each(session, kind, visit, &walk);

	/* Succeeded: how many holders let go. */
	return walk.count;
}
