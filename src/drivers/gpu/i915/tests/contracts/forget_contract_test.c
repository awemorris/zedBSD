/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's object lifetime contract, checked on the host (BUG-260).
 *
 * Runs render/forget.c, render/object.c and render/descriptor.c's pool
 * free with objects that name each other, destroyed against the Vulkan
 * rules before what names them: an image before its views (and the set and
 * the framebuffer that name a view of it), a view, a buffer before its
 * buffer view and set, a buffer view, a sampler, and the objects command
 * buffers recorded.  Every holder lets go of the object first, so that no
 * pointer to a freed record is left; a view without its image is unknown
 * to a later lookup; each recorded operation's objects are found.  The
 * command buffers are stand-ins (a list of operations each) behind
 * drv_i915_gfx_command_forget, which command.c implements the same way.
 */

#include "contract.h"
#include "host_render.h"

#include "../../render/descriptor.h"
#include "../../render/forget.h"
#include "../../render/gfx.h"
#include "../../render/internal.h"
#include "../../render/object.h"

#include <kern/lock.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* How many stand-in command buffers there are, and operations each. */
#define FORGET_BUFFERS		2U
#define FORGET_OPS		4U

/*
 * A stand-in command buffer: its operations, how many, and whether it was
 * emptied as command.c's would be (stale).
 */
struct forget_cmdbuf {
	struct i915_gfx_op ops[FORGET_OPS];
	uint32_t count;
	int stale;
};

/* The stand-in command buffers of the test's one session. */
static struct forget_cmdbuf forget_cmdbufs[FORGET_BUFFERS];

static void forget_check_image(struct i915_render_session *session);
static void forget_check_buffer(struct i915_render_session *session);
static void forget_check_views(struct i915_render_session *session);
static void forget_check_commands(struct i915_render_session *session);
static void forget_check_op_names(void);
static void forget_check_set_free(struct i915_render_session *session);
static void *forget_make(struct i915_render_session *session, enum i915_vk_object_kind kind, uint64_t identity, size_t size);
static void forget_destroy(struct i915_render_session *session, enum i915_vk_object_kind kind, uint64_t identity, void *object);

/*
 * Runs the object lifetime checks.
 */
int
main(void)
{
	struct i915_render_device vk;
	struct i915_render_session session;
	int failures;
	int error;

	/* The checks' executor and session (no GPU node: nothing here reaches it). */
	contract_begin("i915 render object lifetime contract (BUG-260)");
	memset(&vk, 0, sizeof(vk));
	memset(&session, 0, sizeof(session));
	error = drv_i915_object_table_create(&vk.objects);
	contract_check(error == 0, "the object table is made");
	if (error != 0)
		return 1;
	(void)mutex_init(&vk.memories_lock, LOCK_RANK_DEVICE, "test memories");
	session.vk = &vk;

	/* The checks. */
	forget_check_image(&session);
	forget_check_buffer(&session);
	forget_check_views(&session);
	forget_check_commands(&session);
	forget_check_op_names();
	forget_check_set_free(&session);
	contract_check(host_render_lock_depth == 0, "every lock taken is let go");

	/* The table goes. */
	drv_i915_object_forget(&session);
	drv_i915_object_table_destroy(vk.objects);

	/* Reports the verdict. */
	failures = contract_end();
	if (failures != 0)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/*
 * An image destroyed before its view, which a set and a framebuffer name:
 * the view keeps no image, the set and the framebuffer let go of it, a
 * view of another image stays, and a later lookup of the view finds none.
 */
static void
forget_check_image(
	struct i915_render_session *session)
{
	struct i915_gfx_image *image;
	struct i915_gfx_image *other_image;
	struct i915_gfx_view *view;
	struct i915_gfx_view *other_view;
	struct i915_gfx_dset *set;
	struct i915_gfx_framebuffer *framebuffer;

	/* The section the results are listed under. */
	contract_section("an image destroyed before its views");

	/* Two images with a view each; a set and a framebuffer name both views. */
	image = forget_make(session, I915_VK_OBJ_IMAGE, 101U, sizeof(*image));
	other_image = forget_make(session, I915_VK_OBJ_IMAGE, 102U, sizeof(*other_image));
	view = forget_make(session, I915_VK_OBJ_IMAGE_VIEW, 111U, sizeof(*view));
	other_view = forget_make(session, I915_VK_OBJ_IMAGE_VIEW, 112U, sizeof(*other_view));
	set = forget_make(session, I915_VK_OBJ_DESCRIPTOR_SET, 121U, sizeof(*set));
	framebuffer = forget_make(session, I915_VK_OBJ_FRAMEBUFFER, 131U, sizeof(*framebuffer));
	if (image == NULL || other_image == NULL || view == NULL || other_view == NULL || set == NULL || framebuffer == NULL)
		return;
	view->image = image;
	other_view->image = other_image;
	set->slots[0].view = view;
	set->slots[1].view = other_view;
	framebuffer->view_count = 2U;
	framebuffer->views[0] = view;
	framebuffer->views[1] = other_view;
	contract_check(drv_i915_gfx_view_lookup(session, 111U) == view, "a view with its image is found");

	/* The first image goes. */
	host_render_log_lines = 0U;
	forget_destroy(session, I915_VK_OBJ_IMAGE, 101U, image);

	/* Its view keeps no image, and what named the view lets go of it. */
	contract_check(view->image == NULL, "the image's view keeps no image");
	contract_check(set->slots[0].view == NULL && framebuffer->views[0] == NULL, "the set and the framebuffer let go of that view");
	contract_check(set->slots[1].view == other_view && framebuffer->views[1] == other_view && other_view->image == other_image,
		       "the other image's view, and what names it, stay");
	contract_check(drv_i915_gfx_view_lookup(session, 111U) == NULL, "the view without its image is unknown to a lookup");
	contract_check(host_render_log_lines == 2U, "the destroy and the later lookup are logged");

	/* The rest goes. */
	forget_destroy(session, I915_VK_OBJ_IMAGE_VIEW, 111U, view);
	forget_destroy(session, I915_VK_OBJ_IMAGE_VIEW, 112U, other_view);
	forget_destroy(session, I915_VK_OBJ_IMAGE, 102U, other_image);
	forget_destroy(session, I915_VK_OBJ_FRAMEBUFFER, 131U, framebuffer);
	drv_i915_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, 121U);
	free(set);
}

/*
 * A buffer destroyed before its buffer view and the set that names it:
 * both let go of it, and a set naming another buffer keeps it.
 */
static void
forget_check_buffer(
	struct i915_render_session *session)
{
	struct i915_gfx_buffer *buffer;
	struct i915_gfx_buffer *other;
	struct i915_gfx_buffer_view *texel;
	struct i915_gfx_dset *set;

	/* The section the results are listed under. */
	contract_section("a buffer destroyed before its buffer view and set");

	/* Two buffers, a buffer view of the first, and a set naming both. */
	buffer = forget_make(session, I915_VK_OBJ_BUFFER, 201U, sizeof(*buffer));
	other = forget_make(session, I915_VK_OBJ_BUFFER, 202U, sizeof(*other));
	texel = forget_make(session, I915_VK_OBJ_BUFFER_VIEW, 211U, sizeof(*texel));
	set = forget_make(session, I915_VK_OBJ_DESCRIPTOR_SET, 221U, sizeof(*set));
	if (buffer == NULL || other == NULL || texel == NULL || set == NULL)
		return;
	texel->buffer = buffer;
	set->slots[3].buffer = buffer;
	set->slots[4].buffer = other;

	/* The first buffer goes. */
	forget_destroy(session, I915_VK_OBJ_BUFFER, 201U, buffer);
	contract_check(texel->buffer == NULL, "the buffer view lets go of the buffer");
	contract_check(set->slots[3].buffer == NULL && set->slots[4].buffer == other, "the set lets go of it and keeps the other");

	/* The rest goes. */
	forget_destroy(session, I915_VK_OBJ_BUFFER_VIEW, 211U, texel);
	forget_destroy(session, I915_VK_OBJ_BUFFER, 202U, other);
	drv_i915_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, 221U);
	free(set);
}

/*
 * A view, a buffer view and a sampler destroyed before the set (and the
 * framebuffer) that name them: each lets go.
 */
static void
forget_check_views(
	struct i915_render_session *session)
{
	struct i915_gfx_image *image;
	struct i915_gfx_view *view;
	struct i915_gfx_buffer_view *texel;
	struct i915_gfx_sampler *sampler;
	struct i915_gfx_dset *set;
	struct i915_gfx_framebuffer *framebuffer;

	/* The section the results are listed under. */
	contract_section("a view, a buffer view and a sampler destroyed before the set");

	/* An image's view, a buffer view and a sampler, all named by a set; the view by a framebuffer too. */
	image = forget_make(session, I915_VK_OBJ_IMAGE, 301U, sizeof(*image));
	view = forget_make(session, I915_VK_OBJ_IMAGE_VIEW, 311U, sizeof(*view));
	texel = forget_make(session, I915_VK_OBJ_BUFFER_VIEW, 312U, sizeof(*texel));
	sampler = forget_make(session, I915_VK_OBJ_SAMPLER, 313U, sizeof(*sampler));
	set = forget_make(session, I915_VK_OBJ_DESCRIPTOR_SET, 321U, sizeof(*set));
	framebuffer = forget_make(session, I915_VK_OBJ_FRAMEBUFFER, 331U, sizeof(*framebuffer));
	if (image == NULL || view == NULL || texel == NULL || sampler == NULL || set == NULL || framebuffer == NULL)
		return;
	view->image = image;
	set->slots[0].view = view;
	set->slots[0].sampler = sampler;
	set->slots[2].texel = texel;
	framebuffer->view_count = 1U;
	framebuffer->views[0] = view;

	/* Each goes in turn. */
	forget_destroy(session, I915_VK_OBJ_IMAGE_VIEW, 311U, view);
	contract_check(set->slots[0].view == NULL && framebuffer->views[0] == NULL, "the set and the framebuffer let go of the view");
	forget_destroy(session, I915_VK_OBJ_SAMPLER, 313U, sampler);
	contract_check(set->slots[0].sampler == NULL, "the set lets go of the sampler");
	forget_destroy(session, I915_VK_OBJ_BUFFER_VIEW, 312U, texel);
	contract_check(set->slots[2].texel == NULL, "the set lets go of the buffer view");

	/* The rest goes. */
	forget_destroy(session, I915_VK_OBJ_IMAGE, 301U, image);
	forget_destroy(session, I915_VK_OBJ_FRAMEBUFFER, 331U, framebuffer);
	drv_i915_object_remove(session, I915_VK_OBJ_DESCRIPTOR_SET, 321U);
	free(set);
}

/*
 * A pipeline and a buffer that command buffers recorded, destroyed before
 * them: each command buffer that named one is emptied, the other is not.
 */
static void
forget_check_commands(
	struct i915_render_session *session)
{
	struct i915_gfx_pipeline *pipeline;
	struct i915_gfx_buffer *buffer;

	/* The section the results are listed under. */
	contract_section("objects destroyed before the command buffers that recorded them");

	/* A pipeline bound by the first command buffer, a buffer copied by the second. */
	pipeline = forget_make(session, I915_VK_OBJ_PIPELINE, 401U, sizeof(*pipeline));
	buffer = forget_make(session, I915_VK_OBJ_BUFFER, 402U, sizeof(*buffer));
	if (pipeline == NULL || buffer == NULL)
		return;
	memset(forget_cmdbufs, 0, sizeof(forget_cmdbufs));
	forget_cmdbufs[0].ops[0].kind = I915_GFX_OP_BIND_PIPELINE;
	forget_cmdbufs[0].ops[0].u.pipeline = pipeline;
	forget_cmdbufs[0].ops[1].kind = I915_GFX_OP_DRAW;
	forget_cmdbufs[0].count = 2U;
	forget_cmdbufs[1].ops[0].kind = I915_GFX_OP_COPY_BUFFER;
	forget_cmdbufs[1].ops[0].u.buffer_copy.src = buffer;
	forget_cmdbufs[1].count = 1U;

	/* The pipeline goes: the first command buffer is emptied. */
	forget_destroy(session, I915_VK_OBJ_PIPELINE, 401U, pipeline);
	contract_check(forget_cmdbufs[0].stale && forget_cmdbufs[0].count == 0U, "the command buffer that bound the pipeline is emptied");
	contract_check(!forget_cmdbufs[1].stale && forget_cmdbufs[1].count == 1U, "the other command buffer is kept");

	/* The buffer goes: the second is emptied. */
	forget_destroy(session, I915_VK_OBJ_BUFFER, 402U, buffer);
	contract_check(forget_cmdbufs[1].stale && forget_cmdbufs[1].count == 0U, "the command buffer that copied the buffer is emptied");
}

/*
 * Each recorded operation is found to name the objects it was recorded
 * with, and nothing else.
 */
static void
forget_check_op_names(void)
{
	struct i915_gfx_op op;
	int object_a;
	int object_b;
	int other;
	int names;

	/* The section the results are listed under. */
	contract_section("the objects of each recorded operation");

	/* Copies, blits, clears and passes. */
	memset(&op, 0, sizeof(op));
	op.kind = I915_GFX_OP_COPY_BUFFER_TO_IMAGE;
	op.u.copy.buffer = (struct i915_gfx_buffer *)(void *)&object_a;
	op.u.copy.image = (struct i915_gfx_image *)(void *)&object_b;
	names = drv_i915_gfx_op_names(&op, &object_a) + drv_i915_gfx_op_names(&op, &object_b) + drv_i915_gfx_op_names(&op, &other);
	contract_check(names == 2, "a buffer-image copy names its buffer and image alone");
	memset(&op, 0, sizeof(op));
	op.kind = I915_GFX_OP_RESOLVE_IMAGE;
	op.u.image_copy.src = (struct i915_gfx_image *)(void *)&object_a;
	op.u.image_copy.dst = (struct i915_gfx_image *)(void *)&object_b;
	names = drv_i915_gfx_op_names(&op, &object_a) + drv_i915_gfx_op_names(&op, &object_b) + drv_i915_gfx_op_names(&op, &other);
	contract_check(names == 2, "a resolve names its two images alone");
	memset(&op, 0, sizeof(op));
	op.kind = I915_GFX_OP_BEGIN_PASS;
	op.u.begin.pass = (struct i915_gfx_pass *)(void *)&object_a;
	op.u.begin.framebuffer = (struct i915_gfx_framebuffer *)(void *)&object_b;
	names = drv_i915_gfx_op_names(&op, &object_a) + drv_i915_gfx_op_names(&op, &object_b) + drv_i915_gfx_op_names(&op, &other);
	contract_check(names == 2, "a pass's begin names its pass and framebuffer alone");

	/* Binds and the indirect dispatch. */
	memset(&op, 0, sizeof(op));
	op.kind = I915_GFX_OP_BIND_DESCRIPTOR_SET;
	op.u.descriptor.dset = (struct i915_gfx_dset *)(void *)&object_a;
	contract_check(drv_i915_gfx_op_names(&op, &object_a) == 1 && drv_i915_gfx_op_names(&op, &other) == 0, "a set's bind names its set alone");
	memset(&op, 0, sizeof(op));
	op.kind = I915_GFX_OP_BIND_INDEX_BUFFER;
	op.u.index.buffer = (struct i915_gfx_buffer *)(void *)&object_a;
	contract_check(drv_i915_gfx_op_names(&op, &object_a) == 1, "an index buffer's bind names its buffer");
	memset(&op, 0, sizeof(op));
	op.kind = I915_GFX_OP_DISPATCH_INDIRECT;
	op.u.dispatch_indirect.buffer = (struct i915_gfx_buffer *)(void *)&object_a;
	contract_check(drv_i915_gfx_op_names(&op, &object_a) == 1, "an indirect dispatch names its buffer");
	memset(&op, 0, sizeof(op));
	op.kind = I915_GFX_OP_QUERY_RESET;
	op.u.query.pool = (struct i915_gfx_query_pool *)(void *)&object_a;
	contract_check(drv_i915_gfx_op_names(&op, &object_a) == 1, "a query reset names its pool");

	/* An operation without objects names none. */
	memset(&op, 0, sizeof(op));
	op.kind = I915_GFX_OP_DRAW;
	contract_check(drv_i915_gfx_op_names(&op, NULL) == 0 && drv_i915_gfx_op_names(&op, &object_a) == 0, "a draw names no object");
}

/*
 * A descriptor pool's sets freed (vkResetDescriptorPool, vkDestroyDescriptorPool)
 * while a command buffer binds one: the command buffer is emptied.
 */
static void
forget_check_set_free(
	struct i915_render_session *session)
{
	struct i915_gfx_dset *set;
	void *pool;

	/* The section the results are listed under. */
	contract_section("a pool's sets freed before the command buffer that binds one");

	/* A pool record (descriptor.c frees it), a set of it, bound by a command buffer. */
	pool = malloc(16U);
	set = forget_make(session, I915_VK_OBJ_DESCRIPTOR_SET, 501U, sizeof(*set));
	if (pool == NULL || set == NULL)
		return;
	set->pool = pool;
	memset(forget_cmdbufs, 0, sizeof(forget_cmdbufs));
	forget_cmdbufs[0].ops[0].kind = I915_GFX_OP_BIND_DESCRIPTOR_SET;
	forget_cmdbufs[0].ops[0].u.descriptor.dset = set;
	forget_cmdbufs[0].count = 1U;

	/* The pool goes with its sets: the command buffer that bound the set is emptied. */
	drv_i915_gfx_dpool_free(session, pool);
	contract_check(forget_cmdbufs[0].stale && forget_cmdbufs[0].count == 0U, "the command buffer that bound a freed set is emptied");
	contract_check(drv_i915_object_lookup(session, I915_VK_OBJ_DESCRIPTOR_SET, 501U) == NULL, "the set leaves the table");
}

/* Makes a zeroed object of a kind and publishes it under an identity. */
static void *
forget_make(
	struct i915_render_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	size_t size)
{
	void *object;
	int error;

	/* The record. */
	object = calloc(1U, size);
	if (object == NULL) {
		contract_check(0, "an object is made");
		return NULL;
	}

	/* In the table. */
	error = drv_i915_object_insert(session, kind, identity, object);
	if (error != 0) {
		free(object);
		contract_check(0, "an object is published");
		return NULL;
	}

	/* Succeeded: the object is published. */
	return object;
}

/* Destroys an object as a destroy command does: unpublished, forgotten by what names it, freed. */
static void
forget_destroy(
	struct i915_render_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	void *object)
{
	/* Out of the table, out of its holders, then freed. */
	drv_i915_object_remove(session, kind, identity);
	drv_i915_gfx_forget(session, kind, object);
	free(object);
}

/*
 * Stands in for command.c's walk of the session's command buffers: each
 * stand-in whose operations name the object is emptied and marked stale.
 * Reports how many were.
 */
unsigned
drv_i915_gfx_command_forget(
	struct i915_render_session *session,
	const void *object)
{
	struct forget_cmdbuf *cmdbuf;
	unsigned count;
	uint32_t index;
	uint32_t buffer;
	int names;

	UNUSED_PARAMETER(session);

	/* Each stand-in. */
	count = 0U;
	for (buffer = 0U; buffer < FORGET_BUFFERS; buffer++) {
		cmdbuf = &forget_cmdbufs[buffer];
		names = 0;
		for (index = 0U; index < cmdbuf->count && !names; index++)
			names = drv_i915_gfx_op_names(&cmdbuf->ops[index], object);
		if (!names)
			continue;

		/* Emptied, and not to run. */
		cmdbuf->count = 0U;
		cmdbuf->stale = 1;
		count++;
	}

	/* Succeeded: how many were emptied. */
	return count;
}
