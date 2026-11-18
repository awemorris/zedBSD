/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's render passes: the tiler context and heap, the
 * framebuffer and its render targets, each draw as an indexed vertex
 * and tiling run, then the pass's fragment run.
 *
 * A pass has one subpass of colour attachments and at most one depth
 * and stencil attachment, whose aspects the tile buffer clears, writes
 * back and preloads as its ops say (vk-surface.c).  Its tiles start with
 * the attachments' clear colours, and a cleared target writes back every
 * tile.  A colour target 0 the pass loads is read back into the tiles a
 * primitive touches by a pre-frame draw (vk-preload.c); the tiles no
 * primitive touched are not written back, so they keep the attachment's
 * pixels.  A load of another target, or one the pre-frame draw cannot
 * make, draws like a clear to transparent black, said once a
 * session.  The tiler uses one heap chunk of its session's tiler
 * memory, used afresh by every pass (each pass's work ends before the
 * next starts).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/mali/vk/vk-build.h"
#include "drivers/gpu/mali/vk/vk-desc.h"
#include "drivers/gpu/mali/vk/vk-frame.h"
#include "drivers/gpu/mali/vk/vk-meta.h"
#include "drivers/gpu/mali/vk/vk-preload.h"

/*
 * The session's tiler memory: the heap chunk, the geometry buffer, and
 * the sample positions table, in one buffer object.
 */
#define TILER_HEAP_BYTES	(8U << 20)
#define TILER_GEOMETRY_BYTES	(256U << 10)
#define TILER_SAMPLES_BYTES	256U
#define TILER_GEOMETRY_AT	TILER_HEAP_BYTES
#define TILER_SAMPLES_AT	(TILER_HEAP_BYTES + TILER_GEOMETRY_BYTES)
#define TILER_BYTES		(TILER_SAMPLES_AT + 4096U)

/* The tile: 16 by 16 pixels, four bytes of tile buffer a pixel and target. */
#define TILE_PIXELS		256U
#define TILE_BYTES_PER_PIXEL	4U

/*
 * The sample positions of one sample: the pixel's centre, at 1/256 of a
 * pixel, as the first position and the origin (entry 32).
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/lib/pan_samples.c (struct
 * mali_sample_positions, the single-sampled entry of
 * sample_position_lut).
 */
#define SAMPLE_CENTRE		128U
#define SAMPLE_ORIGIN_ENTRY	32U

/*
 * The draw flags: the pixel kill and depth update operations, the faces
 * and the culls.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "DCD Flags 0": pixel
 * kill 2..3, ZS update 4..5, front face CCW 16, cull front 17, cull
 * back 18; enum "Pixel Kill": weak early 2, force late 3).
 */
#define KILL_WEAK_EARLY		2U
#define KILL_FORCE_LATE		3U
#define FLAG_FRONT_CCW		(1U << 16)
#define FLAG_CULL_FRONT		(1U << 17)
#define FLAG_CULL_BACK		(1U << 18)

/*
 * A draw flag: the fragment program changes its coverage (struct "DCD
 * Flags 0", shader modifies coverage at bit 20).
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "DCD Flags 0").
 */
#define FLAG_SHADER_COVERAGE	(1U << 20)

/*
 * A draw flag: the scissor box bounds the primitives (struct "DCD Flags
 * 0", scissor to bounding box at bit 23), set for every draw.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "DCD Flags 0") and
 * src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c (prepare_dcd:
 * scissor_to_bounding_box set for every draw).
 */
#define FLAG_SCISSOR_BOX	(1U << 23)

/*
 * The render targets a draw writes, in its third flags word (struct
 * "DCD Flags 2": read mask 0..11, write mask 12..23); no draw reads a
 * target through the tile buffer.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "DCD Flags 2") and
 * src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c (prepare_dcd: the write
 * mask the targets written, the read mask those read).
 */
#define DCD2_WRITE_SHIFT	12U

/* The tile buffer's depth of 24 bits (enum "Z Internal Format" D24). */
#define Z_INTERNAL_D24		1U

/*
 * A clear inside a pass: the four corners of its rectangle as a strip of
 * two triangles, one instance.
 */
#define CLEAR_VERTICES		4U

/* The float bits of zero, for the offsets of the clear's identity viewport transform. */
#define FLOAT_ZERO		0U

/*
 * The words of one attachment of a clear's record: its aspects, its
 * colour attachment, the clear value's tag and its four words.
 */
#define CLEAR_WORDS		7U
#define CLEAR_ASPECTS		0U
#define CLEAR_TARGET		1U
#define CLEAR_TAG		2U
#define CLEAR_COLOR		3U

/*
 * A clear value's tag: a colour's four words, or a depth's float bits
 * and a stencil value (in the first two of the four).
 */
#define CLEAR_TAG_COLOR		0U
#define CLEAR_TAG_DEPTH_STENCIL	1U

/*
 * The tile buffer's format of a colour target without an attachment (one
 * written nothing, as a pass of only a depth and stencil attachment has):
 * R8G8B8A8.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum "Color Buffer Internal
 * Format": R8G8B8A8 1); src/panfrost/lib/pan_desc.c (pan_emit_rt: a
 * render target without a view).
 */
#define UNUSED_INTERNAL		1U

/* A primitive flag: a varying (secondary) shader runs (struct "Primitive Flags" bit 18). */
#define PRIMITIVE_SECONDARY	(1U << 18)

/*
 * A depth and stencil descriptor that passes everything and writes
 * nothing: type 7, both compares always, the value masks full, depth
 * cull, clamp to [0, 1], the fixed-function depth, the depth function
 * always.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "Depth/stencil";
 * enums "Func" always 7, "Depth Clamp Mode" [0, 1] 1, "Depth Source"
 * fixed function 2).
 */
#define DEPTH_WORD0		(7U | (7U << 4) | (7U << 16))
#define DEPTH_WORD1		0xffff0000U
#define DEPTH_WORD4		((1U << 22) | (1U << 23) | (2U << 25) | (7U << 29))

/*
 * The same descriptor for a fragment program that writes its depth: the
 * depth from the shader ("Depth Source" shader 3 at 4:25).
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (struct "Depth/stencil": depth
 * source 4:25; enum "Depth Source").
 */
#define DEPTH_WORD4_SHADER	((1U << 22) | (1U << 23) | (3U << 25) | (7U << 29))

/* The blend state of a replace, as the pipeline packs it: ONE and ZERO, added, for colour and alpha. */
#define REPLACE_EQUATION	(1U | (1U << 13))

/*
 * The blend's memory format of the 8-bit formats: the tile buffer's
 * RGBA8 ("Format" RGBA8 TB 237 in bits 12..19, the RGBA order 0); the
 * pixel word's sRGB bit and its component order's field.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum "Format": RGBA8 TB 237;
 * enum "RGB Component Order": RGBA 0) and src/panfrost/lib/pan_format.h
 * (MALI_PACK_FMT: the sRGB bit 20).
 */
#define BLEND_RGBA8_TB		(237U << 12)
#define PIXEL_SRGB		(1U << 20)
#define PIXEL_ORDER_MASK	0xfffU

/* The float 1.0's bits, for the depth clamp and the point size. */
#define FLOAT_ONE		0x3f800000U

/*
 * One pass being built: its targets, its descriptors and its draws, and
 * whether it has a depth and stencil attachment (zs) with its depth and
 * stencil aspects.
 */
struct pass_state {
	const struct mali_vk_render_pass *pass;
	const struct mali_vk_framebuffer *framebuffer;
	uint32_t area[4];
	uint64_t tiler;
	uint64_t storage;
	uint32_t targets;
	uint32_t target_mask;
	unsigned draws;
	bool zs;
	bool has_depth;
	bool has_stencil;
};

static int tiler_memory(struct mali_vk_build *build, uint64_t *address);
static int pass_begin(struct mali_vk_build *build, const struct mali_vk_record *entry, struct pass_state *state);
static bool pass_targets(const struct mali_vk_record *entry, struct pass_state *state, volatile uint8_t *targets_view);
static bool pass_depth(const struct mali_vk_record *entry, struct pass_state *state, uint32_t *framebuffer_words, volatile uint8_t *extension_view);
static void draw_depth(const struct mali_vk_build *build, const struct pass_state *state, const struct mali_vk_graphics *graphics, const struct mali_shader_program *fragment, uint32_t *words, uint32_t *flags);
static void depth_range(const VkViewport *viewport, uint32_t *low, uint32_t *high);
static int pass_preload(struct mali_vk_build *build, struct pass_state *state, uint64_t *draws);
static void pass_end(struct mali_vk_build *build, struct pass_state *state, uint64_t fbd);
static void draw(struct mali_vk_build *build, struct pass_state *state, const struct mali_vk_record *entry);
static int clear_attachments(struct mali_vk_build *build, struct pass_state *state, const struct mali_vk_record *entry);
static int clear_rect(struct mali_vk_build *build, struct pass_state *state, const uint32_t *clear, const VkClearRect *rect);
static void viewport_words(const VkViewport *viewport, uint32_t *words);
static int64_t fixed_from_float(uint32_t bits);
static uint32_t float_from_fixed(int64_t value);
static uint32_t hierarchy_mask(uint32_t tiler_features, uint32_t width, uint32_t height);
static uint32_t draw_mode(uint32_t topology);
static uint32_t blend_format(const struct mali_vk_format *format);
static uint32_t pack_clear(const struct mali_vk_format *format, const uint32_t *words);
static uint32_t to_unorm8(uint32_t bits);
static void store(volatile uint8_t *view, const uint32_t *words, unsigned count);

/*
 * Builds the render pass whose begin record is at *index, through its
 * end record, and leaves *index at the end record.
 */
int
mali_vk_draw_pass(
	struct mali_vk_build *build,
	const struct mali_vk_command_buffer *command,
	uint32_t *index)
{
	struct pass_state state;
	const struct mali_vk_record *entry;
	uint64_t fbd;
	int error;

	/* The pass's targets, framebuffer and tiler context. */
	kern_memset(&state, 0, sizeof(state));
	error = pass_begin(build, &command->records[*index], &state);
	if (error != 0)
		return error;
	fbd = state.tiler;
	state.tiler = 0;

	/* Each record to the pass's end. */
	for (*index = *index + 1U; *index < command->record_count && build->error == 0; (*index)++) {
		entry = &command->records[*index];
		if (entry->opcode == GPU_OP_CMD_END_RENDER_PASS)
			break;
		if (entry->opcode == GPU_OP_CMD_DRAW || entry->opcode == GPU_OP_CMD_DRAW_INDEXED) {
			draw(build, &state, entry);
		} else if (entry->opcode == GPU_OP_CMD_CLEAR_ATTACHMENTS) {
			error = clear_attachments(build, &state, entry);
			if (error == ENOTSUP && !build->load_warned)
				kern_logf("mali: vk: a clear of attachments inside a pass is not run\n");
			if (error == ENOTSUP)
				build->load_warned = true;
			else if (error != 0 && build->error == 0)
				build->error = error;
		} else {
			mali_vk_build_bind(build, entry);
		}
	}

	/* The tiling's end and the fragment run. */
	pass_end(build, &state, fbd);
	if (build->error != 0)
		return build->error;

	/* Succeeded: the pass is in the stream. */
	return 0;
}

/* Makes the session's tiler memory once, and reports its address. */
static int
tiler_memory(
	struct mali_vk_build *build,
	uint64_t *address)
{
	struct mali_vk_session *session;
	volatile uint16_t *positions;
	int error;

	/* Made already. */
	session = build->session;
	if (session->tiler_resource != NULL) {
		*address = session->tiler_resource->gpu_address;
		return 0;
	}

	/* The object, mapped. */
	error = mali_bo_create(session->owner->render, TILER_BYTES, &session->tiler);
	if (error != 0)
		return error;
	error = mali_resource_make(session->owner, session->tiler, false, &session->tiler_resource);
	if (error != 0) {
		mali_bo_put(session->owner->render, session->tiler);
		session->tiler = NULL;
		return error;
	}

	/* The sample positions: the centre, first and as the origin. */
	positions = (volatile uint16_t *)(volatile void *)(session->tiler->buffer.view + TILER_SAMPLES_AT);
	positions[0] = SAMPLE_CENTRE;
	positions[1] = SAMPLE_CENTRE;
	positions[SAMPLE_ORIGIN_ENTRY * 2U] = SAMPLE_CENTRE;
	positions[SAMPLE_ORIGIN_ENTRY * 2U + 1U] = SAMPLE_CENTRE;

	/* Succeeded: the memory is the session's. */
	*address = session->tiler_resource->gpu_address;
	return 0;
}

/*
 * Starts a pass: the heap descriptor, the tiler context, the thread
 * storage, the framebuffer descriptor with its render targets (their
 * clear colours) and, when the pass loads its colour attachment, the
 * pre-frame draw that reads it back.  Reports the framebuffer
 * descriptor's address in state->tiler (the caller moves it out) and
 * keeps the tiler context's.
 */
static int
pass_begin(
	struct mali_vk_build *build,
	const struct mali_vk_record *entry,
	struct pass_state *state)
{
	uint32_t words[32];
	volatile uint8_t *heap_view;
	volatile uint8_t *tiler_view;
	volatile uint8_t *fbd_view;
	volatile uint8_t *storage_view;
	uint64_t memory;
	uint64_t heap;
	uint64_t tiler;
	uint64_t fbd;
	uint64_t draws;
	uint64_t extension_bytes;
	uint32_t width;
	uint32_t height;
	uint32_t mode;
	bool clean_tiles;
	bool clean_depth;
	int error;

	/* The pass and the framebuffer. */
	if (entry->objects[0] == NULL || entry->objects[1] == NULL)
		return EINVAL;
	state->pass = entry->objects[0]->payload;
	state->framebuffer = entry->objects[1]->payload;
	kern_memcpy(state->area, entry->values, sizeof(state->area));
	width = state->framebuffer->width;
	height = state->framebuffer->height;
	state->targets = state->pass->color_count;
	if (state->targets == 0U && state->pass->depth != VK_ATTACHMENT_UNUSED)
		state->targets = 1U;
	if (state->targets == 0U || state->targets > MALI_VK_ATTACHMENTS_MAX)
		return ENOTSUP;

	/* The tiler's memory. */
	error = tiler_memory(build, &memory);
	if (error != 0)
		return error;

	/* The heap descriptor of the one chunk, and the tiler context. */
	heap_view = mali_vk_build_take(build, MALI_DESC_HEAP_BYTES, 64U, &heap);
	tiler_view = mali_vk_build_take(build, MALI_DESC_TILER_BYTES, 64U, &tiler);
	if (heap_view == NULL || tiler_view == NULL)
		return build->error;
	mali_desc_tiler_heap(words, memory, TILER_HEAP_BYTES);
	store(heap_view, words, 8);
	mali_desc_tiler(words, width, height, heap, memory + TILER_GEOMETRY_AT, TILER_GEOMETRY_BYTES,
			hierarchy_mask(build->session->owner->render->device->tiler_features, width, height));
	store(tiler_view, words, 32);

	/* The thread storage every draw's programs share, the pre-frame draw's among them. */
	storage_view = mali_vk_build_take(build, MALI_DESC_BYTES, 64U, &state->storage);
	if (storage_view == NULL)
		return build->error;
	mali_desc_local_storage(words);
	store(storage_view, words, 8);

	/* The framebuffer descriptor, its depth and stencil extension, and its targets after them. */
	state->zs = state->pass->depth != VK_ATTACHMENT_UNUSED && state->pass->depth < state->framebuffer->count && state->pass->zs_format != NULL;
	extension_bytes = 0;
	if (state->zs)
		extension_bytes = MALI_SURFACE_ZS_BYTES;
	fbd_view = mali_vk_build_take(build, MALI_DESC_FBD_BYTES + extension_bytes + (uint64_t)state->targets * MALI_DESC_RT_BYTES, 64U, &fbd);
	if (fbd_view == NULL)
		return build->error;

	/* Each colour target, and whether a cleared one writes back every tile. */
	clean_tiles = pass_targets(entry, state, fbd_view + MALI_DESC_FBD_BYTES + extension_bytes);

	/* The pre-frame draw of a loaded colour attachment. */
	error = pass_preload(build, state, &draws);
	if (error != 0)
		return error;

	/*
	 * The framebuffer, with the sample positions and the frame shader
	 * draws.  A pre-frame draw runs on the tiles a primitive touches;
	 * when a cleared target writes back every tile, on every tile.
	 *
	 * Procedure referenced:
	 * Mesa 26.2.6 src/panfrost/lib/pan_desc.h (pan_fix_frame_shader_mode:
	 * an intersecting frame shader runs always when a clean tile is
	 * written); src/panfrost/vulkan/panvk_vX_cmd_frame_shaders.c
	 * (cmd_preload_color_attachments: a loaded colour target's preload
	 * intersects).
	 */
	mali_desc_framebuffer(words, width, height, state->targets, TILE_PIXELS,
			      (TILE_PIXELS * TILE_BYTES_PER_PIXEL * state->targets + 1023U) & ~1023U, tiler);
	words[4] = (uint32_t)(memory + TILER_SAMPLES_AT);
	words[5] = (uint32_t)((memory + TILER_SAMPLES_AT) >> 32);

	/*
	 * Without a depth and stencil attachment the tile buffer's depth is
	 * of 24 bits (word 12 bits 16..17, "Z Internal Format" D24 1).
	 *
	 * Values referenced:
	 * Mesa 26.2.6 src/panfrost/lib/pan_desc.c (pan_emit_fbd: D24 when
	 * there is no depth surface) and src/panfrost/genxml/v10.xml (enum
	 * "Z Internal Format").
	 */
	if (!state->zs)
		words[12] |= Z_INTERNAL_D24 << 16;

	/* The depth and stencil attachment's words and extension; a stored clear of it writes back every tile too. */
	if (state->zs) {
		clean_depth = pass_depth(entry, state, words, fbd_view + MALI_DESC_FBD_BYTES);
		if (clean_depth)
			clean_tiles = true;
	}

	if (draws != 0U) {
		mode = MALI_FRAME_INTERSECT;
		if (clean_tiles)
			mode = MALI_FRAME_ALWAYS;
		mali_frame_shaders(words, mode, draws);
	}

	/* The parameters and their padding of zeros. */
	kern_memset(&words[16], 0, 16U * sizeof(words[0]));
	store(fbd_view, words, 32);

	/* The tiler context in its staging register for the draws, and the framebuffer for the end. */
	mali_cs_move64(&build->cs, MALI_CS_IDVS_TILER_CONTEXT, tiler);
	state->tiler = fbd;

	/* Succeeded: the pass is started. */
	return 0;
}

/*
 * Writes a pass's colour targets after its framebuffer descriptor: each
 * one's memory (its view's level and layer), format, clear and whether
 * it is stored.  A cleared
 * target that is stored writes back the tiles no primitive touched too
 * (else they would keep the old pixels); a loaded or undefined one only
 * those drawn on.  Says whether any target writes back every tile.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/lib/pan_desc.c (pan_get_clean_tile_info: a
 * cleared render target's clean tile write).
 */
static bool
pass_targets(
	const struct mali_vk_record *entry,
	struct pass_state *state,
	volatile uint8_t *targets_view)
{
	struct mali_desc_target target;
	const struct mali_vk_image_view *view;
	const struct mali_vk_image *image;
	const uint32_t *clears;
	uint32_t words[16];
	uint32_t index;
	uint32_t attachment;
	bool cleared;
	bool clean_tiles;

	/* Each colour target in turn. */
	clears = entry->data;
	clean_tiles = false;
	for (index = 0; index < state->targets; index++) {
		kern_memset(&target, 0, sizeof(target));
		target.internal = UNUSED_INTERNAL;
		cleared = false;
		attachment = state->pass->color[index];
		if (attachment != VK_ATTACHMENT_UNUSED && attachment < state->framebuffer->count) {
			view = state->framebuffer->views[attachment]->payload;
			image = view->image->payload;
			target.internal = view->format->internal;
			target.writeback = view->format->writeback;
			target.swizzle = view->format->swizzle;
			target.srgb = (view->format->pixel & (1U << 20)) != 0U;
			target.base = mali_vk_image_level_address(image, view->base_level, view->base_layer);
			target.row_stride = mali_vk_image_level_pitch(image, view->base_level);
			target.write = state->pass->store_op[attachment] == VK_ATTACHMENT_STORE_OP_STORE && target.base != 0U;
			if (state->pass->load_op[attachment] == VK_ATTACHMENT_LOAD_OP_CLEAR && clears != NULL && attachment < entry->values[7]) {
				target.clear[0] = pack_clear(view->format, &clears[attachment * 5U + 1U]);
				cleared = true;
			}

			target.clear[1] = target.clear[0];
			target.clear[2] = target.clear[0];
			target.clear[3] = target.clear[0];
			if (target.write)
				state->target_mask |= 1U << index;
		}

		/* The descriptor, with every tile written back for a stored clear. */
		target.internal_offset = index * TILE_PIXELS * TILE_BYTES_PER_PIXEL;
		mali_desc_target(words, &target);
		if (cleared && target.write) {
			mali_frame_clean_tile(words);
			clean_tiles = true;
		}

		store(targets_view + (uint64_t)index * MALI_DESC_RT_BYTES, words, 16);
	}

	/* Succeeded: whether a target writes back every tile. */
	return clean_tiles;
}

/*
 * Makes the pre-frame draw of a pass whose colour target 0 loads its
 * attachment, and reports the frame shader draws' address (zero for a
 * pass without one).  A load the draw does not take -- of another
 * target, or when the program or its code is not there -- draws on a
 * cleared tile, said once a session.
 */
static int
pass_preload(
	struct mali_vk_build *build,
	struct pass_state *state,
	uint64_t *draws)
{
	struct mali_vk_preload preload;
	const struct mali_vk_image_view *view;
	const struct mali_vk_image *image;
	uint64_t memory;
	uint32_t index;
	uint32_t attachment;
	uint32_t equation;
	bool supported;
	bool loaded;
	bool unloaded;
	int error;

	/* Which targets load: target 0 by the pre-frame draw, any other not at all. */
	*draws = 0;
	loaded = false;
	unloaded = false;
	view = NULL;
	for (index = 0; index < state->targets; index++) {
		attachment = state->pass->color[index];
		if (attachment == VK_ATTACHMENT_UNUSED || attachment >= state->framebuffer->count)
			continue;
		if (state->pass->load_op[attachment] != VK_ATTACHMENT_LOAD_OP_LOAD)
			continue;
		if (index == 0U) {
			view = state->framebuffer->views[attachment]->payload;
			loaded = true;
		} else {
			unloaded = true;
		}
	}

	/* A loaded view the texture can read: one of a bound image. */
	if (loaded) {
		image = view->image->payload;
		memory = mali_vk_image_address(image);
		if (memory == 0U) {
			loaded = false;
			unloaded = true;
		}
	}

	/* The pre-frame draw, which writes colour 0 into target 0 as a replace of every component. */
	if (loaded) {
		kern_memset(&preload, 0, sizeof(preload));
		preload.view = view;
		preload.storage = state->storage;
		equation = mali_desc_blend_equation(REPLACE_EQUATION, 0xfU, &supported);
		mali_desc_blend(preload.blend, false, equation, 0xfU, 0U, blend_format(view->format));
		error = mali_vk_preload_draws(build, &preload, draws);
		if (error == ENOTSUP) {
			unloaded = true;
		} else if (error != 0) {
			return error;
		}
	}

	/* Says once a session that a load draws on a cleared tile. */
	if (unloaded && !build->session->owner->load_warned) {
		kern_logf("mali: vk: a pass that loads its attachment draws on a cleared tile\n");
		build->session->owner->load_warned = true;
	}

	/* Succeeded: the draws, or none. */
	return 0;
}

/*
 * Ends a pass: the tiling's end, then the fragment run over the render
 * area of the framebuffer descriptor, each waited for.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c (the end of a
 * render pass: FINISH_TILING, the framebuffer pointer and bounding box
 * registers, RUN_FRAGMENT).
 */
static void
pass_end(
	struct mali_vk_build *build,
	struct pass_state *state,
	uint64_t fbd)
{
	uint32_t pointer;
	uint32_t max_x;
	uint32_t max_y;

	/* Nothing started. */
	if (fbd == 0U)
		return;

	/* The tiler has seen every draw. */
	mali_cs_emit(&build->cs, mali_cs_finish_tiling());
	mali_cs_emit(&build->cs, mali_cs_wait(MALI_VK_WAIT_ALL));

	/* The framebuffer and the box of tiles to render, inclusive. */
	pointer = mali_desc_framebuffer_pointer(fbd, state->targets);
	if (state->zs)
		pointer |= mali_surface_zs_pointer();
	max_x = state->area[0] + state->area[2] - 1U;
	max_y = state->area[1] + state->area[3] - 1U;
	if (state->area[2] == 0U || state->area[3] == 0U) {
		max_x = state->framebuffer->width - 1U;
		max_y = state->framebuffer->height - 1U;
	}

	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_FRAGMENT_FRAMEBUFFER, pointer));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_FRAGMENT_FRAMEBUFFER + 1U, (uint32_t)(fbd >> 32)));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_FRAGMENT_BOX_MIN, (state->area[0] & 0xffffU) | ((state->area[1] & 0xffffU) << 16)));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_FRAGMENT_BOX_MAX, (max_x & 0xffffU) | ((max_y & 0xffffU) << 16)));

	/* The fragment run, waited for. */
	mali_cs_emit(&build->cs, mali_cs_run_fragment(false, 0));
	mali_cs_emit(&build->cs, mali_cs_wait(MALI_VK_WAIT_ALL));

	/*
	 * The texture caches invalidated, waited for: a later read of what
	 * the pass wrote in the same stream (a sampled image, a pass's
	 * preload) does not find the texels from before.  The flush names
	 * flush id zero, so it is never skipped as done already.
	 *
	 * Procedure referenced:
	 * Mesa 26.2.6 src/panfrost/vulkan/csf/panvk_vX_cmd_buffer.c
	 * (add_memory_dependency: the read-only caches in front of the
	 * texture unit are invalidated after a write a later sample reads).
	 */
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_VK_SCRATCH_REGISTER, 0));
	mali_cs_emit(&build->cs, mali_cs_flush_cache(MALI_CS_FLUSH_NONE, MALI_CS_FLUSH_NONE, MALI_CS_FLUSH_OTHER_INVALIDATE, 0, MALI_VK_SCRATCH_REGISTER, MALI_VK_SLOT_LOAD_STORE));
	mali_cs_emit(&build->cs, mali_cs_wait(1U << MALI_VK_SLOT_LOAD_STORE));
}

/*
 * Writes one draw: the programs' descriptors and resources, the blends,
 * the depth descriptor, the staging registers of an indexed vertex and
 * tiling run, and the run.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c (prepare_draw,
 * prepare_vs, prepare_fs, set_tiler_idvs_flags, prepare_dcd, the
 * cs_run_idvs call with its resource selections (0, 0, 1, 0) and
 * (2, 2, 2, 0)).
 */
static void
draw(
	struct mali_vk_build *build,
	struct pass_state *state,
	const struct mali_vk_record *entry)
{
	const struct mali_vk_pipeline *pipeline;
	const struct mali_vk_graphics *graphics;
	const struct mali_shader_program *vertex;
	const struct mali_shader_program *fragment;
	const struct mali_vk_render_pass *pass;
	const struct mali_vk_image_view *view;
	const VkViewport *viewport;
	const VkRect2D *rect;
	uint32_t transform[6];
	uint32_t words[8];
	bool supported;
	bool indexed;
	volatile uint8_t *blend_view;
	volatile uint8_t *depth_view;
	uint64_t vertex_fau;
	uint64_t vertex_table;
	uint64_t fragment_fau;
	uint64_t fragment_table;
	uint64_t position_shader;
	uint64_t varying_shader;
	uint64_t fragment_shader;
	uint64_t blends;
	uint64_t depth;
	uint64_t storage;
	uint64_t index_address;
	uint32_t index_bytes;
	uint32_t tls_bytes;
	uint32_t index_type;
	uint32_t flags;
	uint32_t primitive;
	uint32_t equation;
	uint32_t attachment;
	uint32_t target;
	uint32_t depth_low;
	uint32_t depth_high;
	int32_t scissor[4];
	int error;

	/* A bound graphics pipeline. */
	pipeline = build->graphics;
	if (pipeline == NULL || pipeline->graphics == NULL) {
		build->error = EINVAL;
		return;
	}
	graphics = pipeline->graphics;
	vertex = graphics->vertex;
	fragment = graphics->fragment;
	pass = state->pass;

	/* The viewport transform the vertex program applies. */
	viewport = &graphics->viewport;
	if (build->viewport_set)
		viewport = &build->viewport;
	viewport_words(viewport, transform);

	/* The programs' FAU words and resource tables. */
	error = mali_vk_build_resources(build, vertex, build->graphics_sets, graphics, transform, 0, &vertex_fau, &vertex_table);
	if (error == 0)
		error = mali_vk_build_resources(build, fragment, build->graphics_sets, NULL, NULL, 0, &fragment_fau, &fragment_table);
	if (error != 0) {
		build->error = error;
		return;
	}

	/* The three programs' descriptors. */
	position_shader = mali_vk_build_shader(build, MALI_DESC_STAGE_VERTEX, vertex->work_registers, vertex->preload, pipeline->vertex_address);
	varying_shader = 0;
	if (pipeline->varying_address != 0U)
		varying_shader = mali_vk_build_shader(build, MALI_DESC_STAGE_VERTEX | MALI_DESC_STAGE_VARYING, vertex->varying_work_registers, vertex->varying_preload, pipeline->varying_address);
	fragment_shader = mali_vk_build_shader(build, MALI_DESC_STAGE_FRAGMENT, fragment->work_registers, fragment->preload, pipeline->fragment_address);

	/* One blend a colour target. */
	blend_view = mali_vk_build_take(build, (uint64_t)state->targets * MALI_DESC_BLEND_BYTES, 64U, &blends);
	depth_view = mali_vk_build_take(build, MALI_DESC_BYTES, 64U, &depth);
	if (blend_view == NULL || depth_view == NULL || position_shader == 0U || fragment_shader == 0U)
		return;
	for (target = 0; target < state->targets; target++) {
		attachment = pass->color[target];
		view = NULL;
		if (attachment != VK_ATTACHMENT_UNUSED && attachment < state->framebuffer->count)
			view = state->framebuffer->views[attachment]->payload;
		equation = mali_desc_blend_equation(REPLACE_EQUATION, 0xfU, &supported);
		if (target < graphics->blend_count && graphics->blend_enable[target])
			equation = mali_desc_blend_equation(graphics->blend_equation[target], graphics->write_mask[target], &supported);
		else if (target < graphics->blend_count)
			equation = mali_desc_blend_equation(REPLACE_EQUATION, graphics->write_mask[target], &supported);
		if (!supported && !build->load_warned) {
			kern_logf("mali: vk: a blend the fixed function does not take is drawn as a replace\n");
			build->load_warned = true;
		}
		mali_desc_blend(words,
				target < graphics->blend_count && graphics->blend_enable[target],
				equation,
				target < graphics->blend_count ? graphics->write_mask[target] : 0xfU,
				target,
				view != NULL ? blend_format(view->format) : 0U);
		store(blend_view + (uint64_t)target * MALI_DESC_BLEND_BYTES, words, 4);
	}

	/*
	 * The depth and stencil descriptor and the draw's kill and update
	 * operations: of the pipeline's tests in a pass with a depth and
	 * stencil attachment; else one that passes everything (the depth
	 * from a program that writes it), the kill late for a program that
	 * discards or writes its depth, and the kill late for a program that
	 * writes memory (it runs even where a later pixel covers it).
	 *
	 * Procedure referenced:
	 * Mesa 26.2.6 src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c
	 * (prepare_dcd: a depth written by the shader forces late kill and
	 * update; a written sample mask modifies the coverage) and
	 * src/panfrost/lib/pan_earlyzs.c (analyze: a shader with side effects
	 * is killed late).
	 */
	kern_memset(words, 0, sizeof(words));
	words[0] = DEPTH_WORD0;
	words[1] = DEPTH_WORD1;
	words[4] = DEPTH_WORD4;
	if (fragment->writes_depth != 0U)
		words[4] = DEPTH_WORD4_SHADER;
	flags = (KILL_WEAK_EARLY << 2) | (KILL_WEAK_EARLY << 4);
	if (fragment->discards != 0U)
		flags = (KILL_FORCE_LATE << 2) | (KILL_FORCE_LATE << 4);
	if (fragment->writes_depth != 0U)
		flags = (KILL_FORCE_LATE << 2) | (KILL_FORCE_LATE << 4);
	if (fragment->writes_memory != 0U)
		flags = (flags & ~(3U << 2)) | (KILL_FORCE_LATE << 2);
	if (state->zs)
		draw_depth(build, state, graphics, fragment, words, &flags);
	store(depth_view, words, 8);

	/* The scissor box, the faces, the culls, and whether the program changes its coverage. */
	flags |= FLAG_SCISSOR_BOX;
	if (fragment->writes_sample_mask != 0U)
		flags |= FLAG_SHADER_COVERAGE;
	if (graphics->front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE)
		flags |= FLAG_FRONT_CCW;
	if ((graphics->cull_mode & VK_CULL_MODE_FRONT_BIT) != 0U)
		flags |= FLAG_CULL_FRONT;
	if ((graphics->cull_mode & VK_CULL_MODE_BACK_BIT) != 0U)
		flags |= FLAG_CULL_BACK;

	/* The index buffer of an indexed draw: its address, its bytes and its type. */
	indexed = entry->opcode == GPU_OP_CMD_DRAW_INDEXED;
	index_address = 0;
	index_bytes = 0;
	index_type = 0;
	if (indexed) {
		if (build->index_buffer == NULL || mali_vk_buffer_address(build->index_buffer) == 0U) {
			build->error = EINVAL;
			return;
		}
		index_address = mali_vk_buffer_address(build->index_buffer) + build->index_offset;
		index_bytes = (uint32_t)(build->index_buffer->size - build->index_offset);
		index_type = 2U;
		if (build->index_type == VK_INDEX_TYPE_UINT32)
			index_type = 3U;
	}

	/* The primitive flags, with the varying program when there is one. */
	primitive = mali_desc_primitive(draw_mode(graphics->topology), index_type);
	if (varying_shader != 0U)
		primitive |= PRIMITIVE_SECONDARY;

	/* The scissor: the render area, within a scissor set or fixed. */
	scissor[0] = (int32_t)state->area[0];
	scissor[1] = (int32_t)state->area[1];
	scissor[2] = (int32_t)(state->area[0] + state->area[2]);
	scissor[3] = (int32_t)(state->area[1] + state->area[3]);
	if (build->scissor_set || graphics->scissor_static) {
		rect = &graphics->scissor;
		if (build->scissor_set)
			rect = &build->scissor;
		if (rect->offset.x > scissor[0])
			scissor[0] = rect->offset.x;
		if (rect->offset.y > scissor[1])
			scissor[1] = rect->offset.y;
		if (rect->offset.x + (int32_t)rect->extent.width < scissor[2])
			scissor[2] = rect->offset.x + (int32_t)rect->extent.width;
		if (rect->offset.y + (int32_t)rect->extent.height < scissor[3])
			scissor[3] = rect->offset.y + (int32_t)rect->extent.height;
	}

	if (scissor[2] <= scissor[0] || scissor[3] <= scissor[1])
		return;

	/* The vertex stage's tables, FAU, programs and storage (sets 0 and 1), the fragment's (set 2). */
	mali_cs_move64(&build->cs, MALI_CS_IDVS_SRT, vertex_table);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_SRT + 4U, fragment_table);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_FAU, vertex_fau);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_FAU + 4U, fragment_fau);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_SPD, position_shader);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_SPD + 2U, varying_shader);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_SPD + 4U, fragment_shader);

	/* The draw's thread storage: the pass's, or one with the programs' thread local storage. */
	storage = state->storage;
	tls_bytes = vertex->tls_bytes;
	if (fragment->tls_bytes > tls_bytes)
		tls_bytes = fragment->tls_bytes;
	if (tls_bytes != 0U) {
		storage = mali_vk_build_thread_storage(build, tls_bytes);
		if (storage == 0U)
			return;
	}

	mali_cs_move64(&build->cs, MALI_CS_IDVS_TSD, storage);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_TSD + 4U, storage);

	/* The draw's counts and offsets. */
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_GLOBAL_OFFSET, 0));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_INDEX_COUNT, entry->values[0]));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_INSTANCE_COUNT, entry->values[1]));
	if (indexed) {
		mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_INDEX_OFFSET, entry->values[2]));
		mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_VERTEX_OFFSET, entry->values[3]));
		mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_INSTANCE_OFFSET, entry->values[4]));
	} else {
		mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_INDEX_OFFSET, 0));
		mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_VERTEX_OFFSET, entry->values[2]));
		mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_INSTANCE_OFFSET, entry->values[3]));
	}

	/* The targets written, the index buffer, the scissor box and the depth clamps. */
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_DCD2, (state->target_mask & 0xffU) << DCD2_WRITE_SHIFT));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_INDEX_BUFFER_SIZE, index_bytes));
	mali_cs_move64(&build->cs, MALI_CS_IDVS_INDEX_BUFFER, index_address);
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_SCISSOR, mali_desc_scissor_low((uint32_t)scissor[0], (uint32_t)scissor[1])));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_SCISSOR + 1U, mali_desc_scissor_high((uint32_t)scissor[2] - 1U, (uint32_t)scissor[3] - 1U)));
	depth_range(viewport, &depth_low, &depth_high);
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_DEPTH_LOW, depth_low));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_DEPTH_HIGH, depth_high));
	mali_cs_move64(&build->cs, MALI_CS_IDVS_OCCLUSION, 0);

	/* The varying record's size, the blends, the depth, the flags and the point size. */
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_VARYING_SIZE, vertex->varying_bytes));
	mali_cs_move64(&build->cs, MALI_CS_IDVS_BLEND, (blends & ~0xfULL) | state->targets);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_DEPTH_STENCIL, depth);
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_TILER_FLAGS, primitive));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_DCD0, flags));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_DCD1, 0xffffU | ((state->target_mask & 0xffU) << 16)));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_PRIMITIVE_SIZE, FLOAT_ONE));

	/* The run: the vertex stages from sets 0 and 1 with the heap's varyings, the fragment's from set 2. */
	mali_cs_emit(&build->cs, mali_cs_run_idvs(0, true, false, false, false, true, false));
	state->draws++;
}

/*
 * Draws a clear of attachments inside a pass: for each attachment it
 * names (a colour attachment with a colour value, or the depth and
 * stencil attachment with a depth and stencil value) and each of its
 * rectangles, a draw of the rectangle by the executor's clear programs.
 * ENOTSUP when the programs are not there, or a rectangle is of a layer
 * past the first (the pass draws one layer); the draws written before
 * stay.
 */
static int
clear_attachments(
	struct mali_vk_build *build,
	struct pass_state *state,
	const struct mali_vk_record *entry)
{
	const uint32_t *words;
	const uint32_t *attachment;
	const VkClearRect *rects;
	uint32_t attachments;
	uint32_t count;
	uint32_t index;
	uint32_t rect;
	int refused;
	int error;

	/* The attachments' words, then the rectangles after them. */
	words = entry->data;
	attachments = entry->values[7];
	count = entry->values[6];
	if (words == NULL || attachments == 0U || count == 0U)
		return 0;
	rects = (const VkClearRect *)(const void *)&words[attachments * CLEAR_WORDS];

	/* Each attachment whose value is of its aspects, each rectangle; a refusal is kept and the others go on. */
	refused = 0;
	for (index = 0; index < attachments; index++) {
		attachment = &words[index * CLEAR_WORDS];
		if ((attachment[CLEAR_ASPECTS] & VK_IMAGE_ASPECT_COLOR_BIT) != 0U && attachment[CLEAR_TAG] != CLEAR_TAG_COLOR)
			continue;
		if ((attachment[CLEAR_ASPECTS] & VK_IMAGE_ASPECT_COLOR_BIT) == 0U && attachment[CLEAR_TAG] != CLEAR_TAG_DEPTH_STENCIL)
			continue;

		/* The rectangles of the attachment. */
		for (rect = 0; rect < count; rect++) {
			error = clear_rect(build, state, attachment, &rects[rect]);
			if (error == ENOTSUP) {
				refused = ENOTSUP;
				continue;
			}

			if (error != 0)
				return error;
		}
	}

	/* A part left out. */
	if (refused != 0)
		return refused;

	/* Succeeded: every clear is in the stream. */
	return 0;
}

/*
 * Draws one clear rectangle of one attachment: the vertex program
 * places the rectangle's corners in pixels (the viewport transform is
 * the identity, its depth offset the cleared depth), a fragment program
 * of the clear's writes the colour.  A colour attachment's target
 * blends a replace of every component, the other targets' blends write
 * nothing, and the depth and stencil pass untouched.  The depth and
 * stencil attachment's clear writes no colour: its depth test always
 * passes and writes the vertex's depth, its stencil test always passes
 * and replaces the stencil with the clear's value, each only for the
 * aspects the clear names and the pass has.  The scissor is the
 * rectangle inside the render area.  The registers of the run are
 * those of a pass's own draw, every one written, so the next draw finds
 * nothing of the clear's.  Nothing for a colour target the pass does not
 * store, an aspect the pass does not have, or a rectangle outside the
 * area.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c (the draw's
 * staging registers, as draw() takes them); src/vulkan/runtime/
 * vk_meta_clear.c (a depth and stencil clear as a rectangle at the
 * clear's depth, depth always passing and written, stencil always
 * passing and replaced by the reference with full masks); the Vulkan
 * specification 1.0 (vkCmdClearAttachments: the rectangles of the
 * attachment cleared as if drawn, regardless of the pipeline's state).
 */
static int
clear_rect(
	struct mali_vk_build *build,
	struct pass_state *state,
	const uint32_t *clear,
	const VkClearRect *rect)
{
	const struct mali_shader_program *vertex;
	const struct mali_shader_program *fragment;
	const struct mali_vk_image_view *view;
	struct mali_surface_zs_state zs;
	struct mali_surface_kill kill;
	uint32_t words[MALI_VK_META_FAU_WORDS];
	uint32_t blend[4];
	uint32_t index;
	uint32_t attachment;
	uint32_t equation;
	uint32_t target;
	uint32_t target_mask;
	uint32_t flags;
	bool color;
	bool depth_aspect;
	bool stencil_aspect;
	bool supported;
	int32_t box[4];
	volatile uint8_t *blend_view;
	volatile uint8_t *depth_view;
	uint64_t vertex_code;
	uint64_t fragment_code;
	uint64_t vertex_fau;
	uint64_t fragment_fau;
	uint64_t table;
	uint64_t position_shader;
	uint64_t fragment_shader;
	uint64_t blends;
	uint64_t depth;
	int error;

	/* A colour clear: a colour target of the pass that is stored, drawn by its location's program. */
	color = false;
	if ((clear[CLEAR_ASPECTS] & VK_IMAGE_ASPECT_COLOR_BIT) != 0U)
		color = true;
	depth_aspect = false;
	stencil_aspect = false;
	target = 0;
	if (color) {
		target = clear[CLEAR_TARGET];
		if (target >= state->targets || target >= MALI_VK_ATTACHMENTS_MAX)
			return 0;
		attachment = state->pass->color[target];
		if (attachment == VK_ATTACHMENT_UNUSED || attachment >= state->framebuffer->count)
			return 0;
		if ((state->target_mask & (1U << target)) == 0U)
			return 0;
	} else {
		/* A depth and stencil clear: the aspects it names that the pass's attachment has, drawn by location 0's program. */
		if (state->zs && state->has_depth && (clear[CLEAR_ASPECTS] & VK_IMAGE_ASPECT_DEPTH_BIT) != 0U)
			depth_aspect = true;
		if (state->zs && state->has_stencil && (clear[CLEAR_ASPECTS] & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U)
			stencil_aspect = true;
		if (!depth_aspect && !stencil_aspect)
			return 0;
	}

	/* The pass's one layer. */
	if (rect->layerCount == 0U)
		return 0;
	if (rect->baseArrayLayer != 0U)
		return ENOTSUP;

	/* The rectangle inside the render area. */
	box[0] = rect->rect.offset.x;
	box[1] = rect->rect.offset.y;
	box[2] = rect->rect.offset.x + (int32_t)rect->rect.extent.width;
	box[3] = rect->rect.offset.y + (int32_t)rect->rect.extent.height;
	if (box[0] < (int32_t)state->area[0])
		box[0] = (int32_t)state->area[0];
	if (box[1] < (int32_t)state->area[1])
		box[1] = (int32_t)state->area[1];
	if (state->area[2] != 0U && box[2] > (int32_t)(state->area[0] + state->area[2]))
		box[2] = (int32_t)(state->area[0] + state->area[2]);
	if (state->area[3] != 0U && box[3] > (int32_t)(state->area[1] + state->area[3]))
		box[3] = (int32_t)(state->area[1] + state->area[3]);
	if (box[2] > (int32_t)state->framebuffer->width)
		box[2] = (int32_t)state->framebuffer->width;
	if (box[3] > (int32_t)state->framebuffer->height)
		box[3] = (int32_t)state->framebuffer->height;
	if (box[2] <= box[0] || box[3] <= box[1])
		return 0;

	/* The two programs and their code. */
	error = mali_vk_meta_program(build, MALI_VK_META_CLEAR_VERTEX, &vertex, &vertex_code);
	if (error != 0)
		return ENOTSUP;
	error = mali_vk_meta_program(build, MALI_VK_META_CLEAR_COLOR + target, &fragment, &fragment_code);
	if (error != 0)
		return ENOTSUP;

	/*
	 * The vertex program's FAU words: the rectangle, then the identity
	 * viewport transform (scales one, offsets zero); the vertex's depth
	 * is zero, so the depth offset is the depth the rectangle is drawn
	 * at, the cleared depth's float bits for a depth clear.
	 */
	kern_memset(words, 0, sizeof(words));
	words[0] = (uint32_t)box[0];
	words[1] = (uint32_t)box[1];
	words[2] = (uint32_t)box[2];
	words[3] = (uint32_t)box[3];
	words[vertex->viewport_fau_word] = FLOAT_ONE;
	words[vertex->viewport_fau_word + 1U] = FLOAT_ONE;
	words[vertex->viewport_fau_word + 2U] = FLOAT_ONE;
	words[vertex->viewport_fau_word + 3U] = FLOAT_ZERO;
	words[vertex->viewport_fau_word + 4U] = FLOAT_ZERO;
	words[vertex->viewport_fau_word + 5U] = FLOAT_ZERO;
	if (depth_aspect)
		words[vertex->viewport_fau_word + 5U] = clear[CLEAR_COLOR];

	/* The words into the stage. */
	error = mali_vk_meta_fau(build, words, vertex->fau_words, &vertex_fau);
	if (error != 0)
		return error;

	/* The fragment program's: the colour's four words (a depth and stencil clear's colour is not written). */
	kern_memset(words, 0, sizeof(words));
	if (color) {
		words[0] = clear[CLEAR_COLOR];
		words[1] = clear[CLEAR_COLOR + 1U];
		words[2] = clear[CLEAR_COLOR + 2U];
		words[3] = clear[CLEAR_COLOR + 3U];
	}

	error = mali_vk_meta_fau(build, words, fragment->fau_words, &fragment_fau);
	if (error != 0)
		return error;

	/* The empty tables both read, and the two program descriptors. */
	table = mali_vk_meta_tables(build);
	position_shader = mali_vk_build_shader(build, MALI_DESC_STAGE_VERTEX, vertex->work_registers, vertex->preload, vertex_code);
	fragment_shader = mali_vk_build_shader(build, MALI_DESC_STAGE_FRAGMENT, fragment->work_registers, fragment->preload, fragment_code);
	if (table == 0U || position_shader == 0U || fragment_shader == 0U)
		return build->error;

	/* One blend a colour target: a colour clear's target replaced, every other target not written. */
	blend_view = mali_vk_build_take(build, (uint64_t)state->targets * MALI_DESC_BLEND_BYTES, 64U, &blends);
	depth_view = mali_vk_build_take(build, MALI_DESC_BYTES, 64U, &depth);
	if (blend_view == NULL || depth_view == NULL)
		return build->error;
	for (index = 0; index < state->targets; index++) {
		attachment = state->pass->color[index];
		view = NULL;
		if (attachment != VK_ATTACHMENT_UNUSED && attachment < state->framebuffer->count)
			view = state->framebuffer->views[attachment]->payload;
		if (color && index == target) {
			equation = mali_desc_blend_equation(REPLACE_EQUATION, 0xfU, &supported);
			mali_desc_blend(blend, false, equation, 0xfU, index, view != NULL ? blend_format(view->format) : 0U);
		} else {
			equation = mali_desc_blend_equation(REPLACE_EQUATION, 0U, &supported);
			mali_desc_blend(blend, false, equation, 0U, index, view != NULL ? blend_format(view->format) : 0U);
		}

		store(blend_view + (uint64_t)index * MALI_DESC_BLEND_BYTES, blend, 4);
	}

	/*
	 * The depth and stencil descriptor and the kill and update
	 * operations: a colour clear's passes everything and is killed weak
	 * early; a depth and stencil clear's tests always pass, write the
	 * depth and replace the stencil of the aspects it clears.
	 */
	kern_memset(&zs, 0, sizeof(zs));
	zs.has_depth = depth_aspect;
	zs.has_stencil = stencil_aspect;
	zs.depth_test = depth_aspect;
	zs.depth_write = depth_aspect;
	zs.depth_compare = VK_COMPARE_OP_ALWAYS;
	zs.stencil_test = stencil_aspect;
	zs.front.fail_op = VK_STENCIL_OP_REPLACE;
	zs.front.pass_op = VK_STENCIL_OP_REPLACE;
	zs.front.depth_fail_op = VK_STENCIL_OP_REPLACE;
	zs.front.compare_op = VK_COMPARE_OP_ALWAYS;
	zs.front.compare_mask = 0xffU;
	zs.front.write_mask = 0xffU;
	zs.front.reference = clear[CLEAR_COLOR + 1U] & 0xffU;
	zs.back = zs.front;
	kern_memset(words, 0, sizeof(words));
	words[0] = DEPTH_WORD0;
	words[1] = DEPTH_WORD1;
	words[4] = DEPTH_WORD4;
	flags = (KILL_WEAK_EARLY << 2) | (KILL_WEAK_EARLY << 4);
	target_mask = 1U << target;
	if (!color) {
		/* The descriptor of the clear's tests, and the operations they allow. */
		mali_surface_depth_stencil(words, &zs);
		kern_memset(&kill, 0, sizeof(kill));
		kill.state = &zs;
		flags = mali_surface_kill_flags(&kill);
		target_mask = 0;
	}

	store(depth_view, words, 8);

	/* The programs, their tables and FAU, the pass's storage. */
	mali_cs_move64(&build->cs, MALI_CS_IDVS_SRT, table);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_SRT + 4U, table);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_FAU, vertex_fau);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_FAU + 4U, fragment_fau);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_SPD, position_shader);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_SPD + 2U, 0);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_SPD + 4U, fragment_shader);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_TSD, state->storage);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_TSD + 4U, state->storage);

	/* Four vertices of one instance, no index buffer. */
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_GLOBAL_OFFSET, 0));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_INDEX_COUNT, CLEAR_VERTICES));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_INSTANCE_COUNT, 1));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_INDEX_OFFSET, 0));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_VERTEX_OFFSET, 0));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_INSTANCE_OFFSET, 0));

	/* The target written (or none), no index buffer, the scissor of the rectangle, the depth clamps. */
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_DCD2, target_mask << DCD2_WRITE_SHIFT));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_INDEX_BUFFER_SIZE, 0));
	mali_cs_move64(&build->cs, MALI_CS_IDVS_INDEX_BUFFER, 0);
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_SCISSOR, mali_desc_scissor_low((uint32_t)box[0], (uint32_t)box[1])));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_SCISSOR + 1U, mali_desc_scissor_high((uint32_t)box[2] - 1U, (uint32_t)box[3] - 1U)));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_DEPTH_LOW, 0));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_DEPTH_HIGH, FLOAT_ONE));
	mali_cs_move64(&build->cs, MALI_CS_IDVS_OCCLUSION, 0);

	/* No varyings, the blends, the depth, a strip without culls, the kill, the colour target alone or none. */
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_VARYING_SIZE, vertex->varying_bytes));
	mali_cs_move64(&build->cs, MALI_CS_IDVS_BLEND, (blends & ~0xfULL) | state->targets);
	mali_cs_move64(&build->cs, MALI_CS_IDVS_DEPTH_STENCIL, depth);
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_TILER_FLAGS, mali_desc_primitive(MALI_DESC_DRAW_TRIANGLE_STRIP, 0)));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_DCD0, flags | FLAG_SCISSOR_BOX));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_DCD1, 0xffffU | (target_mask << 16)));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_IDVS_PRIMITIVE_SIZE, FLOAT_ONE));

	/* The run, as a pass's own draw. */
	mali_cs_emit(&build->cs, mali_cs_run_idvs(0, true, false, false, false, true, false));
	state->draws++;

	/* Succeeded: the rectangle is drawn. */
	return 0;
}

/*
 * Writes a pass's depth and stencil attachment: its words of the
 * framebuffer parameters (the tile buffer's depth format, the clears,
 * the write backs of the stored aspects and the preloads of the loaded
 * ones) and the extension of its targets, at the view's level and layer.
 * Sets the pass's aspects.  Says whether a cleared aspect that is stored
 * writes back every tile.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/lib/pan_desc.c (pan_emit_fbd: the depth
 * format, the clear values, the write enables of the aspects not
 * discarded; pan_emit_zs_crc_ext; pan_get_clean_tile_info).
 */
static bool
pass_depth(
	const struct mali_vk_record *entry,
	struct pass_state *state,
	uint32_t *framebuffer_words,
	volatile uint8_t *extension_view)
{
	struct mali_surface_zs_target target;
	const struct mali_vk_image_view *view;
	const struct mali_vk_image *image;
	const uint32_t *clears;
	uint32_t words[MALI_SURFACE_ZS_BYTES / 4U];
	uint32_t attachment;
	bool cleared;

	/* The attachment's view at its level and layer. */
	attachment = state->pass->depth;
	view = state->framebuffer->views[attachment]->payload;
	image = view->image->payload;
	kern_memset(&target, 0, sizeof(target));
	target.format = state->pass->zs_format;
	target.base = mali_vk_image_level_address(image, view->base_level, view->base_layer);
	target.pitch = mali_vk_image_level_pitch(image, view->base_level);
	target.surface_bytes = image->level[view->base_level].bytes;
	state->has_depth = (target.format->aspects & MALI_SURFACE_ASPECT_DEPTH) != 0U;
	state->has_stencil = (target.format->aspects & MALI_SURFACE_ASPECT_STENCIL) != 0U;

	/* The ops of each aspect; nothing is written to an image not bound. */
	target.depth_clear = state->pass->load_op[attachment] == VK_ATTACHMENT_LOAD_OP_CLEAR;
	target.depth_load = state->pass->load_op[attachment] == VK_ATTACHMENT_LOAD_OP_LOAD;
	target.depth_store = state->pass->store_op[attachment] == VK_ATTACHMENT_STORE_OP_STORE && target.base != 0U;
	target.stencil_clear = state->pass->stencil_load_op[attachment] == VK_ATTACHMENT_LOAD_OP_CLEAR;
	target.stencil_load = state->pass->stencil_load_op[attachment] == VK_ATTACHMENT_LOAD_OP_LOAD;
	target.stencil_store = state->pass->stencil_store_op[attachment] == VK_ATTACHMENT_STORE_OP_STORE && target.base != 0U;
	if (target.base == 0U) {
		target.depth_load = false;
		target.stencil_load = false;
	}

	/* The clear value: the depth's float bits and the stencil's byte. */
	clears = entry->data;
	if (clears != NULL && attachment < entry->values[7]) {
		target.depth_clear_bits = clears[attachment * 5U + 1U];
		target.stencil_clear_value = clears[attachment * 5U + 2U];
	}

	/* The framebuffer's words and the extension. */
	mali_surface_zs_framebuffer(framebuffer_words, &target);
	mali_surface_zs_extension(words, &target);
	store(extension_view, words, MALI_SURFACE_ZS_BYTES / 4U);

	/* Whether a stored clear writes back every tile. */
	cleared = false;
	if (state->has_depth && target.depth_clear && target.depth_store)
		cleared = true;
	if (state->has_stencil && target.stencil_clear && target.stencil_store)
		cleared = true;
	return cleared;
}

/*
 * Writes a draw's depth and stencil descriptor and its kill and update
 * operations in a pass with a depth and stencil attachment: the
 * pipeline's tests on the pass's aspects, with the bias, masks and
 * references the commands set when the pipeline takes them from the
 * commands, and the program's own depth and stencil.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c (prepare_dcd
 * and the depth and stencil descriptor of a draw, with the dynamic
 * state).
 */
static void
draw_depth(
	const struct mali_vk_build *build,
	const struct pass_state *state,
	const struct mali_vk_graphics *graphics,
	const struct mali_shader_program *fragment,
	uint32_t *words,
	uint32_t *flags)
{
	struct mali_surface_zs_state zs;
	struct mali_surface_kill kill;
	uint32_t direction;

	/* The pipeline's state on the pass's aspects, and the program's writes. */
	zs = graphics->zs;
	zs.has_depth = state->has_depth;
	zs.has_stencil = state->has_stencil;
	zs.shader_depth = fragment->writes_depth != 0U;
	zs.shader_stencil = fragment->writes_stencil != 0U;

	/* The state the commands set. */
	if (graphics->dynamic_bias) {
		zs.bias_constant = build->bias[0];
		zs.bias_clamp = build->bias[1];
		zs.bias_slope = build->bias[2];
	}

	if (graphics->dynamic_compare_mask) {
		zs.front.compare_mask = build->compare_mask[0];
		zs.back.compare_mask = build->compare_mask[1];
	}

	if (graphics->dynamic_write_mask) {
		zs.front.write_mask = build->write_mask[0];
		zs.back.write_mask = build->write_mask[1];
	}

	if (graphics->dynamic_reference) {
		zs.front.reference = build->reference[0];
		zs.back.reference = build->reference[1];
	}

	/* The descriptor. */
	mali_surface_depth_stencil(words, &zs);

	/* The kill and update operations, keeping the other flags. */
	kill.state = &zs;
	kill.coverage = fragment->discards != 0U || fragment->writes_sample_mask != 0U;
	kill.writes_memory = fragment->writes_memory != 0U;
	direction = mali_surface_kill_flags(&kill);
	*flags = (*flags & ~0x3cU) | direction;
}

/*
 * Gives a draw's depth clamps: the lesser and the greater of the
 * viewport's depth range (each within [0, 1], whose float bits order as
 * the floats do; a negative one is zero).
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c (the low and
 * high depth clamps of the viewport's minDepth and maxDepth).
 */
static void
depth_range(
	const VkViewport *viewport,
	uint32_t *low,
	uint32_t *high)
{
	uint32_t near;
	uint32_t far;

	/* The bits, a negative one as zero. */
	kern_memcpy(&near, &viewport->minDepth, sizeof(near));
	kern_memcpy(&far, &viewport->maxDepth, sizeof(far));
	if ((near & 0x80000000U) != 0U)
		near = 0U;
	if ((far & 0x80000000U) != 0U)
		far = 0U;

	/* The lesser first. */
	*low = near;
	*high = far;
	if (far < near) {
		*low = far;
		*high = near;
	}
}

/*
 * Keeps the depth bias or a stencil mask or reference a record sets:
 * the bias's three factors, or the value for each face its face mask
 * names (front 1, back 2).
 *
 * Values referenced: the Vulkan specification 1.0 (VkStencilFaceFlagBits).
 */
void
mali_vk_draw_set_state(
	struct mali_vk_build *build,
	const struct mali_vk_record *entry)
{
	uint32_t *values;
	uint32_t face;

	/* The bias. */
	if (entry->opcode == GPU_OP_CMD_SET_DEPTH_BIAS) {
		build->bias[0] = entry->values[0];
		build->bias[1] = entry->values[1];
		build->bias[2] = entry->values[2];
		return;
	}

	/* The stencil value of the command. */
	if (entry->opcode == GPU_OP_CMD_SET_STENCIL_COMPARE_MASK) {
		values = build->compare_mask;
	} else if (entry->opcode == GPU_OP_CMD_SET_STENCIL_WRITE_MASK) {
		values = build->write_mask;
	} else if (entry->opcode == GPU_OP_CMD_SET_STENCIL_REFERENCE) {
		values = build->reference;
	} else {
		return;
	}

	/* Each face the mask names. */
	for (face = 0; face < 2U; face++) {
		if ((entry->values[0] & (1U << face)) != 0U)
			values[face] = entry->values[1];
	}
}

/*
 * Builds the viewport transform's six floats (the scale of x, y and z,
 * then their offsets) from a viewport, in fixed point without
 * floating-point instructions: scale (w/2, h/2, max - min), offset
 * (x + w/2, y + h/2, min).
 */
static void
viewport_words(
	const VkViewport *viewport,
	uint32_t *words)
{
	uint32_t raw[6];
	int64_t x;
	int64_t y;
	int64_t width;
	int64_t height;
	int64_t low;
	int64_t high;

	/* The viewport's floats as their bits. */
	kern_memcpy(&raw[0], &viewport->x, sizeof(uint32_t));
	kern_memcpy(&raw[1], &viewport->y, sizeof(uint32_t));
	kern_memcpy(&raw[2], &viewport->width, sizeof(uint32_t));
	kern_memcpy(&raw[3], &viewport->height, sizeof(uint32_t));
	kern_memcpy(&raw[4], &viewport->minDepth, sizeof(uint32_t));
	kern_memcpy(&raw[5], &viewport->maxDepth, sizeof(uint32_t));
	x = fixed_from_float(raw[0]);
	y = fixed_from_float(raw[1]);
	width = fixed_from_float(raw[2]);
	height = fixed_from_float(raw[3]);
	low = fixed_from_float(raw[4]);
	high = fixed_from_float(raw[5]);

	/* The scales, then the offsets. */
	words[0] = float_from_fixed(width / 2);
	words[1] = float_from_fixed(height / 2);
	words[2] = float_from_fixed(high - low);
	words[3] = float_from_fixed(x + width / 2);
	words[4] = float_from_fixed(y + height / 2);
	words[5] = float_from_fixed(low);
}

/* Converts a float's bits to a signed fixed-point value of 16 fraction bits, saturating. */
static int64_t
fixed_from_float(
	uint32_t bits)
{
	int64_t magnitude;
	int32_t exponent;
	uint64_t mantissa;

	/* Zero, a denormal, and the sign. */
	exponent = (int32_t)((bits >> 23) & 0xffU) - 127;
	if (((bits >> 23) & 0xffU) == 0U)
		return 0;
	mantissa = (uint64_t)((bits & 0x7fffffU) | 0x800000U);

	/* (1.m * 2^e) * 2^16 = m * 2^(e + 16 - 23). */
	if (exponent + 16 - 23 >= 0) {
		if (exponent > 30)
			magnitude = (int64_t)1 << 46;
		else
			magnitude = (int64_t)(mantissa << (exponent + 16 - 23));
	} else if (exponent + 16 - 23 > -63) {
		magnitude = (int64_t)(mantissa >> (23 - 16 - exponent));
	} else {
		magnitude = 0;
	}

	/* The sign. */
	if ((bits & 0x80000000U) != 0U)
		return -magnitude;
	return magnitude;
}

/* Converts a signed fixed-point value of 16 fraction bits to a float's bits, truncating. */
static uint32_t
float_from_fixed(
	int64_t value)
{
	uint64_t magnitude;
	uint32_t sign;
	int32_t top;

	/* Zero, and the sign. */
	if (value == 0)
		return 0;
	sign = 0;
	magnitude = (uint64_t)value;
	if (value < 0) {
		sign = 0x80000000U;
		magnitude = (uint64_t)(-value);
	}

	/* The highest set bit names the exponent; 23 bits below it the mantissa. */
	top = 63;
	while ((magnitude & (1ULL << top)) == 0U)
		top--;
	if (top > 23)
		magnitude >>= (top - 23);
	else
		magnitude <<= (23 - top);

	/* The bits: the value is magnitude * 2^(top - 16). */
	return sign | ((uint32_t)(top - 16 + 127) << 23) | ((uint32_t)magnitude & 0x7fffffU);
}

/*
 * Chooses the tiler's hierarchy levels: the default spread for fewer
 * than eight levels, every level otherwise, moved up so the coarsest
 * covers the framebuffer.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/lib/pan_desc.c
 * (pan_select_tiler_hierarchy_mask, before its tile size step); the
 * levels from the tiler features' bits 8..11 (pan_props.c:
 * pan_query_tiler_features).
 */
static uint32_t
hierarchy_mask(
	uint32_t tiler_features,
	uint32_t width,
	uint32_t height)
{
	static const uint32_t spread[8] = { 0x00, 0x80, 0x82, 0xa2, 0xaa, 0xea, 0xee, 0xfe };
	uint32_t levels;
	uint32_t largest;
	uint32_t last;
	uint32_t mask;

	/* The levels the tiler has; fewer than two (the features not read) as every one of eight. */
	levels = (tiler_features >> 8) & 0xfU;
	if (levels < 2U)
		levels = 8U;
	if (levels < 8U) {
		mask = spread[levels];
		levels = 8U;
	} else {
		mask = (1U << levels) - 1U;
	}

	/* The level whose bins of 16 pixels cover the framebuffer. */
	largest = width;
	if (height > largest)
		largest = height;
	largest = (largest + 15U) / 16U;
	last = 0;
	while (last < 32U && (largest >> last) != 0U)
		last++;
	if (last > levels)
		mask <<= last - levels;

	/* Succeeded: the mask. */
	return mask;
}

/*
 * Gives the hardware's draw mode of a Vulkan topology.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum "Draw Mode").
 */
static uint32_t
draw_mode(
	uint32_t topology)
{
	/* One case a topology. */
	switch (topology) {
	case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
		return MALI_DESC_DRAW_POINTS;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
		return MALI_DESC_DRAW_LINES;
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
		return MALI_DESC_DRAW_LINE_STRIP;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
		return MALI_DESC_DRAW_TRIANGLE_STRIP;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
		return MALI_DESC_DRAW_TRIANGLE_FAN;
	default:
		return MALI_DESC_DRAW_TRIANGLES;
	}
}

/*
 * Gives the memory format the fixed-function blend converts a target's
 * colour to: for a format of 8-bit unsigned normalized components (one
 * to four, either order) the tile buffer's RGBA8 with the format's sRGB
 * bit; for any other the format's pixel word in the RGBA order (the
 * fixed function does not take the RGB1 order; the render target's
 * writeback orders the memory).
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/lib/pan_format.c (pan_blendable_formats:
 * BFMT_SRGB of R8, R8G8, R8G8B8A8 and B8G8R8A8 blends as
 * MALI_BLEND_PU_R8G8B8A8, RGBA8 TB in the RGBA order, with the sRGB bit
 * 20; BFMT_FLOAT as the format in the RGBA order), src/panfrost/lib/
 * pan_format.h (pan_dithered_format_from_pipe_format: a format with no
 * blendable entry blends as its own pixel word),
 * src/panfrost/vulkan/panvk_vX_blend.c (the fixed function's RGB1 order
 * forced to RGBA) and src/panfrost/genxml/v10.xml (enum "Format": RGBA8
 * TB 237).
 */
static uint32_t
blend_format(
	const struct mali_vk_format *format)
{
	/* One case a format of 8-bit unsigned normalized components; any other keeps its own format. */
	switch (format->vk) {
	case VK_FORMAT_R8_UNORM:
	case VK_FORMAT_R8G8_UNORM:
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
		return BLEND_RGBA8_TB | (format->pixel & PIXEL_SRGB);
	default:
		return format->pixel & ~PIXEL_ORDER_MASK;
	}
}

/*
 * Packs a clear colour into the tile buffer's 32-bit internal word of 8
 * bits a component: R, G, B, A from the low byte, A one for a format
 * without alpha.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/lib/pan_clear.c (pan_pack_color: the
 * R8G8B8A8 tile buffer layout, the word repeated in the four clear
 * words).
 */
static uint32_t
pack_clear(
	const struct mali_vk_format *format,
	const uint32_t *words)
{
	uint32_t alpha;

	/* A format of fewer than four components clears with an alpha of one. */
	alpha = to_unorm8(words[3]);
	if (format->bytes < 4U)
		alpha = 255U;

	/* Succeeded: the word. */
	return to_unorm8(words[0]) | (to_unorm8(words[1]) << 8) | (to_unorm8(words[2]) << 16) | (alpha << 24);
}

/* Converts a float's bits to an 8-bit unsigned normalized value, rounded, without floating-point instructions. */
static uint32_t
to_unorm8(
	uint32_t bits)
{
	int64_t fixed;

	/* The value in fixed point, clamped to [0, 1]. */
	fixed = fixed_from_float(bits);
	if (fixed <= 0)
		return 0;
	if (fixed >= (1 << 16))
		return 255;

	/* Scaled and rounded. */
	return (uint32_t)((fixed * 255 + (1 << 15)) >> 16);
}

/* Stores words through an uncached view. */
static void
store(
	volatile uint8_t *view,
	const uint32_t *words,
	unsigned count)
{
	volatile uint32_t *destination;
	unsigned index;

	destination = (volatile uint32_t *)(volatile void *)view;
	for (index = 0; index < count; index++)
		destination[index] = words[index];
}
