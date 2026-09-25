/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fixed 3D draws the hardware tests submit (see draw-fixture.h).
 *
 * One state page holds everything a draw reads: the surface heap at +0
 * (binding table, render target and texture surface states), the dynamic
 * heap state at +512 (colour calculator, blend, viewport, coarse pixel
 * state, sampler), the pixel shader at +1024 followed by a carpet of
 * thread-ending instructions, the vertex data at +2048 and the markers at
 * +3072.  STATE_BASE_ADDRESS points every heap at the start of the page, so
 * every offset below is also the pointer a state packet carries.
 *
 * The command list follows the order of Mesa's BLORP, the smallest sequence
 * known to draw on this hardware: vertex state, then the pipeline, then the
 * primitive.  The batch and the state bytes are compared by hash with the
 * ones earlier hardware runs used, so every dword here is kept exactly as it
 * was first programmed.
 */

#include "draw-fixture.h"
#include <kern/kcrt.h>

#include "../../render/heap.h"

#include <stdint.h>

#include "../../intel/commands.h"
#include "../../intel/genxml.h"

/* The size of the state page. */
#define I915_DRAW_STATE_PAGE_BYTES	4096U

/* The render target's row pitch: 32 pixels of four bytes. */
#define I915_DRAW_PITCH			(I915_DRAW_FIXTURE_WIDTH * 4U)

/* Offsets within the surface heap. */
#define I915_DRAW_BINDING_TABLE_OFFSET	0U
#define I915_DRAW_SURFACE_STATE_OFFSET	64U

/* Offsets within the dynamic heap; each state is 64-byte aligned. */
#define I915_DRAW_COLOR_CALC_OFFSET	512U
#define I915_DRAW_BLEND_OFFSET		576U
#define I915_DRAW_CC_VIEWPORT_OFFSET	640U
#define I915_DRAW_CPS_STATE_OFFSET	768U

/* How many bytes of the dynamic heap are cleared from the colour calculator on. */
#define I915_DRAW_COLOR_CALC_BYTES	256U

/* Offsets of the vertex data. */
#define I915_DRAW_POSITION_OFFSET	2048U
#define I915_DRAW_VUE_HEADER_OFFSET	2112U

/* How many bytes of vertex data are cleared before the positions are written. */
#define I915_DRAW_POSITION_BYTES	128U

/* The room between the pixel shader and the vertex data. */
#define I915_DRAW_KERNEL_ROOM		(I915_DRAW_POSITION_OFFSET - I915_DRAW_FIXTURE_PS_OFFSET)

/* The markers' offsets from the marker area. */
#define I915_DRAW_MARKER_BEFORE_SLOT	0U
#define I915_DRAW_MARKER_AFTER_SLOT	4U
#define I915_DRAW_MARKER_MIDDRAW_SLOT	8U

/* The vertex stage's URB allocation: start chunk 4, one chunk. */
#define I915_DRAW_URB_VS_ALLOCATION	((4U << 10) | (4U << 21))

/* The whole URB for the vertex stage, as Mesa's intel_get_urb_config gives it. */
#define I915_DRAW_URB_VS_ENTRIES	3576U

/* The other stages' URB allocation: past the vertex chunk. */
#define I915_DRAW_URB_OTHER_ALLOCATION	((5U << 10) | (5U << 21))

/* A STATE_BASE_ADDRESS size field that covers the whole address space, with its modify bit. */
#define I915_DRAW_SBA_FULL_SIZE		(1U | (0xfffffU << 12))

/* The bindless surface state size: one page of 64-byte surface states. */
#define I915_DRAW_SBA_BINDLESS_SIZE	((4096U / 64U - 1U) << 12)

/* The IEEE-754 bit patterns of the target sizes the rectangles cover. */
#define I915_DRAW_F32_32		0x42000000U
#define I915_DRAW_F32_1920		0x44f00000U
#define I915_DRAW_F32_1080		0x44870000U

/*
 * What differs between the command lists of the fixtures.
 *
 * One instance describes one fixture's pixel shader dispatch, its sampler
 * and the extent of its render target; the command list is otherwise the
 * same for every fixture.  The instances are constant tables.
 */
struct i915_draw_options {
	/* 3DSTATE_PS dword 3: sampler count and binding table entry count. */
	uint32_t ps_dw3;

	/* 3DSTATE_PS dword 7: the dispatch GRF start the compiler chose. */
	uint32_t ps_dw7;

	/* 3DSTATE_PS_EXTRA dword 1: valid, UAV, source depth and W as the compiler requires. */
	uint32_t ps_extra_dw1;

	/* Nonzero when the draw names a sampler with 3DSTATE_SAMPLER_STATE_POINTERS_PS. */
	unsigned sampler_pointers;

	/* The two dwords of 3DSTATE_SAMPLER_STATE_POINTERS_PS. */
	uint32_t ssp_dw0;
	uint32_t ssp_dw1;

	/* The render target's size, which the drawing rectangle clips to. */
	uint32_t rt_w;
	uint32_t rt_h;
};

/*
 * A batch under construction.
 *
 * It lives on the stack of the builder; the caller owns the dword array and
 * reads the count once the batch is complete.
 */
struct i915_draw_batch {
	/* The caller's dword array. */
	uint32_t *cmds;

	/* How many dwords have been written, never more than the capacity. */
	unsigned count;

	/* How many dwords the array holds. */
	unsigned capacity;
};

/*
 * The generated textured-draw words: the sampling pixel shaders, the
 * texture and render target surface states and the samplers, with the
 * packet words that dispatch them.  Mesa's compiler, isl and genxml made
 * them; the files carry their provenance and are never edited by hand.
 */
#include "tex-fixture-gen.inc"
#include "tex-fixture-fhd-gen.inc"

/*
 * The constant opaque-red fragment shader of the single-colour draw.
 *
 * The real Intel compiler (brw_compile_fs, Alder Lake-P) made it: the SIMD8
 * variant at byte 0 and the SIMD16 variant at byte 128, with the dispatch
 * GRF start 2.  Each variant first stores the entry marker 0xc0ffee01 to
 * 0x100400c10 with an A64 untyped write, then writes the colour to render
 * target 0 with a split send (a one-register payload and a three-register
 * extension), the form this hardware requires, and retires.
 */
static const uint32_t i915_draw_const_color_ps[] = {
	0x80000061U, 0x31010110U, 0x000001e4U, 0x00000000U,
	0x00030061U, 0x06054220U, 0x00000000U, 0xc0ffee01U,
	0x00030061U, 0x7f054660U, 0x00000000U, 0x3f800000U,
	0x00030061U, 0x7c054660U, 0x00000000U, 0x00000000U,
	0x00030061U, 0x7d054660U, 0x00000000U, 0x00000000U,
	0x00030061U, 0x7e054660U, 0x00000000U, 0x3f800000U,
	0x80030061U, 0x02264aa0U, 0x00000000U, 0x00000001U,
	0x80030161U, 0x02064aa0U, 0x00000000U, 0x00400c10U,
	0x80000101U, 0x00000000U, 0x00000000U, 0x00000000U,
	0x00030061U, 0x04260660U, 0x00000224U, 0x00000000U,
	0x00030161U, 0x04060660U, 0x00000204U, 0x00000000U,
	0x01839031U, 0x00000000U, 0xcdfa0414U, 0x019a060cU,
	0x00030132U, 0x00000004U, 0x58007f0cU, 0x00c47c1cU,
	0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
	0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
	0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
	0x80000061U, 0x31010110U, 0x000001e4U, 0x00000000U,
	0x00040061U, 0x08054220U, 0x00000000U, 0xc0ffee01U,
	0x00040061U, 0x7e054660U, 0x00000000U, 0x3f800000U,
	0x00040061U, 0x78054660U, 0x00000000U, 0x00000000U,
	0x00040061U, 0x7a054660U, 0x00000000U, 0x00000000U,
	0x00040061U, 0x7c054660U, 0x00000000U, 0x3f800000U,
	0x80030061U, 0x02264aa0U, 0x00000000U, 0x00000001U,
	0x80030161U, 0x02064aa0U, 0x00000000U, 0x00400c10U,
	0x80000101U, 0x00000000U, 0x00000000U, 0x00000000U,
	0x00030061U, 0x04260660U, 0x00000224U, 0x00000000U,
	0x00130061U, 0x06260660U, 0x00000224U, 0x00000000U,
	0x00030261U, 0x04060660U, 0x00000204U, 0x00000000U,
	0x00130261U, 0x06060660U, 0x00000204U, 0x00000000U,
	0x01849031U, 0x00000000U, 0xcdfa0424U, 0x01960814U,
	0x00040132U, 0x00000004U, 0x50007e14U, 0x00c47834U,
};

/*
 * A thread-ending instruction with no side effect: a render target write
 * marked null.
 *
 * The room after a pixel shader is carpeted with it, so a thread the
 * execution units start there by mistake retires at once instead of running
 * whatever the page holds.
 */
static const uint32_t i915_draw_eot_only[4] = {
	0x00030032U, 0x00001004U, 0x58007024U, 0x00c40000U,
};

/*
 * The push constant packets of every stage, in pipeline order.
 *
 * Each is emitted empty, so no stage reads constants left over in the
 * context.
 */
static const uint32_t i915_draw_constant_opcodes[] = {
	GEN12_CMD_3DSTATE_CONSTANT_VS,
	GEN12_CMD_3DSTATE_CONSTANT_HS,
	GEN12_CMD_3DSTATE_CONSTANT_DS,
	GEN12_CMD_3DSTATE_CONSTANT_GS,
	GEN12_CMD_3DSTATE_CONSTANT_PS,
};

/*
 * The single-colour draw's command list options.
 *
 * One binding table entry (the render target), the dispatch GRF start 2, a
 * valid pixel shader with a UAV (the marker store), no sampler, and the
 * 32x32 target.
 */
static const struct i915_draw_options i915_draw_color_options = {
	1U << 18,
	2U << 16,
	(1U << 31) | (1U << 2),
	0U,
	0U,
	0U,
	I915_DRAW_FIXTURE_WIDTH,
	I915_DRAW_FIXTURE_HEIGHT,
};

/*
 * The 32x32 textured draw's command list options, taken from the
 * generated packet words.
 */
static const struct i915_draw_options i915_draw_texture_options = {
	TEXFIX_3DSTATE_PS_DW3,
	TEXFIX_3DSTATE_PS_DW7,
	TEXFIX_3DSTATE_PS_EXTRA_DW1,
	1U,
	TEXFIX_SAMPLER_POINTERS_PS_DW0,
	TEXFIX_SAMPLER_POINTERS_PS_DW1,
	I915_DRAW_FIXTURE_WIDTH,
	I915_DRAW_FIXTURE_HEIGHT,
};

/*
 * The full-HD textured draw's command list options, taken from the
 * generated packet words.
 */
static const struct i915_draw_options i915_draw_fhd_options = {
	TEXFHD_3DSTATE_PS_DW3,
	TEXFHD_3DSTATE_PS_DW7,
	TEXFHD_3DSTATE_PS_EXTRA_DW1,
	1U,
	TEXFHD_SAMPLER_POINTERS_PS_DW0,
	TEXFHD_SAMPLER_POINTERS_PS_DW1,
	TEXFHD_RT_WIDTH,
	TEXFHD_RT_HEIGHT,
};

/* The header must describe the single-colour draw's shader and page. */
_Static_assert(I915_DRAW_FIXTURE_PS_BYTES == sizeof(i915_draw_const_color_ps),
	"draw-fixture.h must give the single-colour shader's size");
_Static_assert(I915_DRAW_FIXTURE_MARKER_OFFSET + 16U + 4U <= I915_DRAW_STATE_PAGE_BYTES,
	"the markers must sit in the state page");

/* The sampling shaders must fit between the kernel offset and the vertex data. */
_Static_assert(TEXFIX_PS_BYTES <= I915_DRAW_KERNEL_ROOM,
	"the sampling pixel shader must fit before the vertex data");
_Static_assert(TEXFHD_PS_BYTES <= I915_DRAW_KERNEL_ROOM,
	"the full-HD pixel shader must fit before the vertex data");

/*
 * The batch dispatches the SIMD8 kernel at offset 0 and provides no
 * varyings, position offsets, scratch or push constants.  The payload layout
 * (GRF start, source depth and W) is not assumed: it comes from the
 * generated 3DSTATE_PS dword 7 and 3DSTATE_PS_EXTRA words.
 */
_Static_assert(TEXFIX_PS_DISPATCH_8 == 1U, "the sampling shader must have a SIMD8 kernel");
_Static_assert(TEXFIX_PS_NUM_VARYING == 0U, "the sampling shader must read no varyings");
_Static_assert(TEXFIX_PS_USES_POS_OFFSET == 0U, "the sampling shader must use no position offsets");
_Static_assert(TEXFIX_PS_TOTAL_SCRATCH == 0U, "the sampling shader must use no scratch");
_Static_assert(TEXFIX_PS_PUSH_SIZE0 == 0U, "the sampling shader must use no push constants");
_Static_assert(TEXFIX_3DSTATE_PS_EXTRA_DW0 == 0x784f0000U, "the generated words must be 3DSTATE_PS_EXTRA's");
_Static_assert((TEXFIX_3DSTATE_PS_DW7 >> 16) == TEXFIX_PS_GRF_START_8,
	"3DSTATE_PS dword 7 must carry the SIMD8 GRF start");
_Static_assert(TEXFHD_PS_DISPATCH_8 == 1U, "the full-HD shader must have a SIMD8 kernel");
_Static_assert(TEXFHD_PS_NUM_VARYING == 0U, "the full-HD shader must read no varyings");
_Static_assert(TEXFHD_PS_USES_POS_OFFSET == 0U, "the full-HD shader must use no position offsets");
_Static_assert(TEXFHD_PS_TOTAL_SCRATCH == 0U, "the full-HD shader must use no scratch");
_Static_assert(TEXFHD_PS_PUSH_SIZE0 == 0U, "the full-HD shader must use no push constants");
_Static_assert(TEXFHD_3DSTATE_PS_EXTRA_DW0 == 0x784f0000U, "the generated words must be 3DSTATE_PS_EXTRA's");
_Static_assert((TEXFHD_3DSTATE_PS_DW7 >> 16) == TEXFHD_PS_GRF_START_8,
	"3DSTATE_PS dword 7 must carry the SIMD8 GRF start");

/* The header must describe the generated texture fixture. */
_Static_assert(TEXFIX_TEX_VA_PLACEHOLDER == I915_TEX_FIXTURE_TEX_VA, "the texture address must match");
_Static_assert(TEXFIX_TEX_WIDTH == I915_TEX_FIXTURE_TEX_W, "the texture width must match");
_Static_assert(TEXFIX_TEX_HEIGHT == I915_TEX_FIXTURE_TEX_H, "the texture height must match");
_Static_assert(TEXFIX_TEX_SIZE == I915_TEX_FIXTURE_TEX_BYTES, "the texture size must match");
_Static_assert(TEXFIX_TEX_ROW_PITCH == 4U * I915_TEX_FIXTURE_TEX_W, "the texture must be linear");
_Static_assert(TEXFIX_SAMPLER_OFFSET == I915_TEX_FIXTURE_SAMPLER_OFFSET, "the sampler offset must match");

/* The header must describe the generated full-HD fixture. */
_Static_assert(TEXFHD_RT_WIDTH == I915_TEX_FHD_WIDTH, "the full-HD width must match");
_Static_assert(TEXFHD_RT_HEIGHT == I915_TEX_FHD_HEIGHT, "the full-HD height must match");
_Static_assert(TEXFHD_RT_ROW_PITCH == I915_TEX_FHD_PITCH, "the full-HD pitch must match");
_Static_assert(TEXFHD_RT_SIZE == I915_TEX_FHD_RT_BYTES, "the full-HD target size must match");
_Static_assert(TEXFHD_RT_VA_PLACEHOLDER == I915_TEX_FHD_RT_VA, "the full-HD target address must match");
_Static_assert(TEXFHD_TEX_VA_PLACEHOLDER == I915_TEX_FIXTURE_TEX_VA, "the full-HD texture address must match");
_Static_assert(TEXFHD_SAMPLER_OFFSET == I915_TEX_FIXTURE_SAMPLER_OFFSET, "the full-HD sampler offset must match");

/* The texture states must not overlap the draw's own states and must keep their alignment. */
_Static_assert(I915_TEX_FIXTURE_TEX_RSS_OFFSET >= I915_DRAW_SURFACE_STATE_OFFSET + 64U,
	"texture A's surface state must follow the render target's");
_Static_assert(I915_TEX_FIXTURE_TEX_RSS_OFFSET + 64U <= I915_DRAW_COLOR_CALC_OFFSET,
	"texture A's surface state must end before the dynamic state");
_Static_assert((I915_TEX_FIXTURE_TEX_RSS_OFFSET & 63U) == 0U, "texture A's surface state must be 64-byte aligned");
_Static_assert(I915_TEX_FIXTURE_TEX_B_RSS_OFFSET >= I915_TEX_FIXTURE_TEX_RSS_OFFSET + 64U,
	"texture B's surface state must follow texture A's");
_Static_assert(I915_TEX_FIXTURE_TEX_B_RSS_OFFSET + 64U <= I915_DRAW_COLOR_CALC_OFFSET,
	"texture B's surface state must end before the dynamic state");
_Static_assert((I915_TEX_FIXTURE_TEX_B_RSS_OFFSET & 63U) == 0U, "texture B's surface state must be 64-byte aligned");
_Static_assert(I915_TEX_FIXTURE_SAMPLER_OFFSET >= I915_DRAW_CPS_STATE_OFFSET + GEN12_CPS_STATE_DWORDS * 4U,
	"the sampler must follow the coarse pixel state");
_Static_assert(I915_TEX_FIXTURE_SAMPLER_OFFSET + sizeof(texfix_sampler) <= I915_DRAW_FIXTURE_PS_OFFSET,
	"the sampler must end before the pixel shader");
_Static_assert((I915_TEX_FIXTURE_SAMPLER_OFFSET & 31U) == 0U, "the sampler must be 32-byte aligned");

/* The generated surface states are whole RENDER_SURFACE_STATEs. */
_Static_assert(sizeof(texfix_tex_rss) == GEN12_RENDER_SURFACE_STATE_DWORDS * 4U,
	"the texture's surface state must be one RENDER_SURFACE_STATE");
_Static_assert(sizeof(texfhd_tex_rss) == GEN12_RENDER_SURFACE_STATE_DWORDS * 4U,
	"the full-HD texture's surface state must be one RENDER_SURFACE_STATE");
_Static_assert(sizeof(texfhd_rt_rss) == GEN12_RENDER_SURFACE_STATE_DWORDS * 4U,
	"the full-HD target's surface state must be one RENDER_SURFACE_STATE");

static void i915_draw_fill_eot(void *page, unsigned offset, unsigned length);
static void i915_draw_write_kernel(void *page, const uint32_t *kernel, unsigned kernel_bytes);
static void i915_draw_write_surface_state(uint32_t *heap, uint64_t rt_va, uint32_t mocs);
static void i915_draw_write_dynamic_state(uint32_t *heap);
static void i915_draw_write_vertices(uint32_t *page, uint32_t width_bits, uint32_t height_bits);
static void i915_tex_write_surface_state(uint32_t *heap, unsigned offset, const uint32_t *template_state, uint64_t tex_va);
static void i915_draw_emit(struct i915_draw_batch *batch, uint32_t dword);
static void i915_draw_emit_zeroes(struct i915_draw_batch *batch, unsigned count);
static void i915_draw_emit_disabled(struct i915_draw_batch *batch, uint32_t opcode, uint32_t dwords);
static void i915_draw_emit_marker(struct i915_draw_batch *batch, uint64_t address, uint32_t value);
static void i915_draw_emit_pipe_control(struct i915_draw_batch *batch, uint32_t flags);
static void i915_draw_emit_state_base_address(struct i915_draw_batch *batch, uint64_t state_va, uint32_t mocs);
static void i915_draw_emit_context_defaults(struct i915_draw_batch *batch);
static void i915_draw_emit_vertex_state(struct i915_draw_batch *batch, uint64_t vb_va, uint32_t mocs);
static void i915_draw_emit_urb(struct i915_draw_batch *batch);
static void i915_draw_emit_constants(struct i915_draw_batch *batch, uint32_t mocs);
static void i915_draw_emit_state_pointers(struct i915_draw_batch *batch, uint32_t mocs);
static void i915_draw_emit_geometry_stages(struct i915_draw_batch *batch);
static void i915_draw_emit_raster_state(struct i915_draw_batch *batch, const struct i915_draw_options *options);
static void i915_draw_emit_pixel_shader(struct i915_draw_batch *batch, const struct i915_draw_options *options);
static void i915_draw_emit_depth_state(struct i915_draw_batch *batch);
static void i915_draw_emit_primitive(struct i915_draw_batch *batch);
static unsigned i915_draw_build_batch(uint32_t *cmds, unsigned capacity, uint64_t state_va, uint32_t mocs, const struct i915_draw_options *options);

/*
 * Writes the single-colour draw's state page.
 *
 * The page is cleared, then gets the binding table and the render target's
 * surface state (naming rt_va with mocs), the colour calculator, blend,
 * viewport and coarse pixel states, the constant-colour pixel shader with
 * its carpet of thread-ending instructions, and a rectangle over the whole
 * 32x32 target.
 */
void
drv_i915_draw_fixture_write_state(
	void *state_page,
	uint64_t rt_va,
	uint32_t mocs)
{
	/* Starts from a page of zeroes, which every state not written below relies on. */
	kern_memset(state_page, 0, I915_DRAW_STATE_PAGE_BYTES);

	/* Names the render target. */
	i915_draw_write_surface_state(state_page, rt_va, mocs);

	/* Writes the states the pipeline points at in the dynamic heap. */
	i915_draw_write_dynamic_state(state_page);

	/* Places the constant-colour shader and carpets the room after it. */
	i915_draw_write_kernel(state_page, i915_draw_const_color_ps, sizeof(i915_draw_const_color_ps));

	/* Covers the 32x32 target with the rectangle. */
	i915_draw_write_vertices(state_page, I915_DRAW_F32_32, I915_DRAW_F32_32);
}

/*
 * Builds the single-colour draw's batch and returns its dword count.
 *
 * Every heap and the vertex buffer are at state_va; mocs is the cache
 * setting of every state but the instruction heap.  A count above capacity
 * never happens: dwords past the capacity are dropped, so the caller
 * compares the count with what it expects.
 */
unsigned
drv_i915_draw_fixture_build_batch(
	uint32_t *cmds,
	unsigned capacity,
	uint64_t state_va,
	uint32_t mocs)
{
	unsigned count;

	/* Builds the command list with the single-colour pixel shader. */
	count = i915_draw_build_batch(cmds, capacity, state_va, mocs, &i915_draw_color_options);

	/* Succeeded: reports how many dwords were written. */
	return count;
}

/*
 * Reports the cache setting every fixture uses for its states.
 *
 * It is the uncached MOCS table entry, as the first hardware draw used.
 */
uint32_t
drv_i915_draw_fixture_mocs(void)
{
	/* The uncached entry, shifted into the MOCS field's form. */
	return GEN12_MOCS(I915_MOCS_UNCACHED_INDEX);
}

/*
 * Writes the 32x32 textured draw's state page.
 *
 * It is the single-colour page with the sampling pixel shader in place of
 * the constant-colour one, binding table entry 1 naming the texture's
 * surface state (tex_va patched in) and the nearest sampler.  The generated
 * surface state carries its own MOCS; mocs sets the render target's.
 */
void
drv_i915_tex_fixture_write_state(
	void *state_page,
	uint64_t rt_va,
	uint64_t tex_va,
	uint32_t mocs)
{
	uint32_t *heap;

	heap = state_page;

	/* Writes the single-colour page the textured draw is built on. */
	drv_i915_draw_fixture_write_state(state_page, rt_va, mocs);

	/* Replaces the constant-colour shader with the sampling one. */
	kern_memset((uint8_t *)state_page + I915_DRAW_FIXTURE_PS_OFFSET, 0, I915_DRAW_KERNEL_ROOM);
	i915_draw_write_kernel(state_page, texfix_ps, TEXFIX_PS_BYTES);

	/* Points binding table entry 1 at the texture's surface state. */
	heap[I915_DRAW_BINDING_TABLE_OFFSET / 4U + 1U] = I915_TEX_FIXTURE_TEX_RSS_OFFSET;
	i915_tex_write_surface_state(heap, I915_TEX_FIXTURE_TEX_RSS_OFFSET, texfix_tex_rss, tex_va);

	/* Writes the nearest sampler. */
	kern_memcpy(&heap[I915_TEX_FIXTURE_SAMPLER_OFFSET / 4U], texfix_sampler, sizeof(texfix_sampler));
}

/*
 * Builds the 32x32 textured draw's batch and returns its dword count.
 *
 * It is the single-colour batch with the sampling shader's dispatch words
 * and 3DSTATE_SAMPLER_STATE_POINTERS_PS added before the binding table.
 */
unsigned
drv_i915_tex_fixture_build_batch(
	uint32_t *cmds,
	unsigned capacity,
	uint64_t state_va,
	uint32_t mocs)
{
	unsigned count;

	/* Builds the command list with the sampling pixel shader. */
	count = i915_draw_build_batch(cmds, capacity, state_va, mocs, &i915_draw_texture_options);

	/* Succeeded: reports how many dwords were written. */
	return count;
}

/*
 * Fills one of the test images.
 *
 * rgba receives 8x8 RGBA texels in memory order; variant selects the image
 * (see draw-fixture.h), and any variant above 2 gives image 3.
 */
void
drv_i915_tex_fixture_pattern(
	uint8_t *rgba,
	unsigned variant)
{
	uint8_t *texel;
	unsigned u;
	unsigned v;

	/* Computes every texel of the image from its coordinates. */
	for (v = 0U; v < I915_TEX_FIXTURE_TEX_H; v++) {
		for (u = 0U; u < I915_TEX_FIXTURE_TEX_W; u++) {
			texel = &rgba[(v * I915_TEX_FIXTURE_TEX_W + u) * 4U];

			/* Picks the colour channels of the requested image. */
			if (variant == 0U) {
				texel[0] = (uint8_t)(16U + 32U * u);
				texel[1] = (uint8_t)(16U + 32U * v);
				texel[2] = (uint8_t)(16U + 32U * ((u + 3U * v) & 7U));
			} else if (variant == 1U) {
				texel[0] = (uint8_t)(239U - 32U * v);
				texel[1] = (uint8_t)(16U + 32U * u);
				texel[2] = (uint8_t)(16U + 32U * ((3U * u + v) & 7U));
			} else if (variant == 2U) {
				texel[0] = (uint8_t)(240U - 32U * u);
				texel[1] = (uint8_t)(240U - 32U * v);
				texel[2] = (uint8_t)(16U + 32U * ((u ^ v) & 7U));
			} else {
				texel[0] = (uint8_t)(64U * (u & 3U));
				texel[1] = (uint8_t)(64U * (v & 3U));
				texel[2] = (uint8_t)(64U * ((u >> 2) + 2U * (v >> 2)));
			}

			/* Every image is opaque. */
			texel[3] = 255U;
		}
	}
}

/*
 * Computes what the 32x32 target reads back as at pixel (x, y) after the
 * nearest-sampled textured draw.
 *
 * With uv = (pixel + 0.5) / 32 over an 8x8 texture the texel is pixel / 4.
 * The value is the little-endian dword of the B8G8R8A8 pixel.
 */
uint32_t
drv_i915_tex_fixture_expected_pixel(
	const uint8_t *rgba,
	unsigned x,
	unsigned y)
{
	const uint8_t *texel;
	uint32_t pixel;

	/* Finds the texel the pixel samples. */
	texel = &rgba[((y / 4U) * I915_TEX_FIXTURE_TEX_W + (x / 4U)) * 4U];

	/* Reorders the RGBA texel into the B, G, R, A bytes of the target. */
	pixel = (uint32_t)texel[2];
	pixel |= (uint32_t)texel[1] << 8;
	pixel |= (uint32_t)texel[0] << 16;
	pixel |= (uint32_t)texel[3] << 24;

	/* Reports the expected pixel. */
	return pixel;
}

/*
 * Writes the textured draw's state page with two textures and one of them
 * bound.
 *
 * Texture A (tex_a_va) keeps its surface state at +128, texture B
 * (tex_b_va) gets one at +192; binding table entry 1 names B when bind_b is
 * nonzero and A otherwise.  Nothing else differs between the two bindings.
 */
void
drv_i915_tex_fixture_write_state_ab(
	void *state_page,
	uint64_t rt_va,
	uint64_t tex_a_va,
	uint64_t tex_b_va,
	unsigned bind_b,
	uint32_t mocs)
{
	uint32_t *heap;

	heap = state_page;

	/* Writes the textured page, which names texture A. */
	drv_i915_tex_fixture_write_state(state_page, rt_va, tex_a_va, mocs);

	/* Adds texture B's surface state. */
	i915_tex_write_surface_state(heap, I915_TEX_FIXTURE_TEX_B_RSS_OFFSET, texfix_tex_rss, tex_b_va);

	/* Binds the requested texture through binding table entry 1. */
	if (bind_b != 0U) {
		heap[I915_DRAW_BINDING_TABLE_OFFSET / 4U + 1U] = I915_TEX_FIXTURE_TEX_B_RSS_OFFSET;
	} else {
		heap[I915_DRAW_BINDING_TABLE_OFFSET / 4U + 1U] = I915_TEX_FIXTURE_TEX_RSS_OFFSET;
	}
}

/*
 * Writes the two-texture state page with the nearest or the bilinear
 * sampler.
 *
 * With linear nonzero the sampler filters linearly for both minification
 * and magnification, with address rounding on as anv and BLORP program it;
 * the page is otherwise the one drv_i915_tex_fixture_write_state_ab()
 * writes.
 */
void
drv_i915_tex_fixture_write_state_ab_filter(
	void *state_page,
	uint64_t rt_va,
	uint64_t tex_a_va,
	uint64_t tex_b_va,
	unsigned bind_b,
	unsigned linear,
	uint32_t mocs)
{
	uint32_t *heap;

	heap = state_page;

	/* Writes the two-texture page with the nearest sampler. */
	drv_i915_tex_fixture_write_state_ab(state_page, rt_va, tex_a_va, tex_b_va, bind_b, mocs);

	/* Replaces the sampler with the bilinear one when asked to. */
	if (linear != 0U) {
		kern_memcpy(&heap[I915_TEX_FIXTURE_SAMPLER_OFFSET / 4U],
		       texfix_sampler_linear,
		       sizeof(texfix_sampler_linear));
	}
}

/*
 * Computes what the 32x32 target reads back as at pixel (x, y) after the
 * bilinear textured draw.
 *
 * With uv = (pixel + 0.5) / 32 over an 8x8 texture the texel-space
 * coordinate is (2 * pixel - 3) / 8, so each axis has two weights that are
 * multiples of 1/8 and every channel is sum(weight * texel) / 64 with
 * integer weights.  Neighbours outside the texture clamp to the edge texel.
 * When inexact is not NULL it is set to 1 if a channel's sum is not a
 * multiple of 64, that is, if the hardware has a rounding decision to make;
 * it is left alone otherwise.  With image 3 no such decision exists and the
 * result is compared for exact equality.
 */
uint32_t
drv_i915_tex_fixture_expected_pixel_linear(
	const uint8_t *rgba,
	unsigned x,
	unsigned y,
	int *inexact)
{
	int coordinate_x;
	int coordinate_y;
	int texel_x0;
	int texel_y0;
	unsigned fraction_x;
	unsigned fraction_y;
	int texel_x[2];
	int texel_y[2];
	unsigned weight[2][2];
	unsigned channel;
	unsigned row;
	unsigned column;
	unsigned sum;
	unsigned texel_index;
	uint32_t value[4];
	uint32_t pixel;

	/* The texel-space coordinates in eighths: (2 * pixel + 1) / 8 - 1 / 2. */
	coordinate_x = 2 * (int)x - 3;
	coordinate_y = 2 * (int)y - 3;

	/* The upper-left neighbour, which lies left of or above the texture for the first pixel. */
	if (coordinate_x >= 0) {
		texel_x0 = coordinate_x / 8;
	} else {
		texel_x0 = -1;
	}

	if (coordinate_y >= 0) {
		texel_y0 = coordinate_y / 8;
	} else {
		texel_y0 = -1;
	}

	/* How far, in eighths, the sample lies past the upper-left neighbour. */
	fraction_x = (unsigned)(coordinate_x - 8 * texel_x0);
	fraction_y = (unsigned)(coordinate_y - 8 * texel_y0);

	/* The two neighbours on each axis. */
	texel_x[0] = texel_x0;
	texel_x[1] = texel_x0 + 1;
	texel_y[0] = texel_y0;
	texel_y[1] = texel_y0 + 1;

	/* Clamps every neighbour to the edge of the texture. */
	for (column = 0U; column < 2U; column++) {
		if (texel_x[column] < 0)
			texel_x[column] = 0;
		if (texel_x[column] > (int)I915_TEX_FIXTURE_TEX_W - 1)
			texel_x[column] = (int)I915_TEX_FIXTURE_TEX_W - 1;
		if (texel_y[column] < 0)
			texel_y[column] = 0;
		if (texel_y[column] > (int)I915_TEX_FIXTURE_TEX_H - 1)
			texel_y[column] = (int)I915_TEX_FIXTURE_TEX_H - 1;
	}

	/* The four neighbours' weights, in sixty-fourths. */
	weight[0][0] = (8U - fraction_x) * (8U - fraction_y);
	weight[0][1] = fraction_x * (8U - fraction_y);
	weight[1][0] = (8U - fraction_x) * fraction_y;
	weight[1][1] = fraction_x * fraction_y;

	/* Filters every channel and notes a sum that needs rounding. */
	for (channel = 0U; channel < 4U; channel++) {
		sum = 0U;
		for (row = 0U; row < 2U; row++) {
			for (column = 0U; column < 2U; column++) {
				texel_index = (unsigned)texel_y[row] * I915_TEX_FIXTURE_TEX_W + (unsigned)texel_x[column];
				sum += weight[row][column] * rgba[texel_index * 4U + channel];
			}
		}

		/* A sum between two UNORM8 steps leaves the rounding to the hardware. */
		if (inexact != NULL && (sum & 63U) != 0U)
			*inexact = 1;

		value[channel] = (sum + 32U) / 64U;
	}

	/* Reorders the RGBA channels into the B, G, R, A bytes of the target. */
	pixel = value[2];
	pixel |= value[1] << 8;
	pixel |= value[0] << 16;
	pixel |= value[3] << 24;

	/* Reports the expected pixel. */
	return pixel;
}

/*
 * Writes the full-HD textured draw's state page.
 *
 * It is the single-colour page with the generated render target surface
 * state (1920x1080, pitch 7680, rt_va patched in), the full-HD sampling
 * shader, binding table entry 1 naming the texture's surface state (tex_va
 * patched in), the nearest sampler and a rectangle over the whole target.
 * The generated surface states carry their own MOCS, the uncached entry.
 */
void
drv_i915_tex_fixture_fhd_write_state(
	void *state_page,
	uint64_t rt_va,
	uint64_t tex_va,
	uint32_t mocs)
{
	uint32_t *heap;

	heap = state_page;

	/* Writes the single-colour page the full-HD draw is built on. */
	drv_i915_draw_fixture_write_state(state_page, rt_va, mocs);

	/* Replaces the render target's surface state with the full-HD one. */
	i915_tex_write_surface_state(heap, I915_DRAW_SURFACE_STATE_OFFSET, texfhd_rt_rss, rt_va);

	/* Replaces the constant-colour shader with the full-HD sampling one. */
	kern_memset((uint8_t *)state_page + I915_DRAW_FIXTURE_PS_OFFSET, 0, I915_DRAW_KERNEL_ROOM);
	i915_draw_write_kernel(state_page, texfhd_ps, TEXFHD_PS_BYTES);

	/* Points binding table entry 1 at the texture's surface state. */
	heap[I915_DRAW_BINDING_TABLE_OFFSET / 4U + 1U] = I915_TEX_FIXTURE_TEX_RSS_OFFSET;
	i915_tex_write_surface_state(heap, I915_TEX_FIXTURE_TEX_RSS_OFFSET, texfhd_tex_rss, tex_va);

	/* Writes the nearest sampler. */
	kern_memcpy(&heap[I915_TEX_FIXTURE_SAMPLER_OFFSET / 4U], texfhd_sampler, sizeof(texfhd_sampler));

	/* Covers the whole 1920x1080 target with the rectangle. */
	i915_draw_write_vertices(state_page, I915_DRAW_F32_1920, I915_DRAW_F32_1080);
}

/*
 * Builds the full-HD textured draw's batch and returns its dword count.
 *
 * It is the 32x32 textured batch with the drawing rectangle widened to the
 * 1920x1080 target.
 */
unsigned
drv_i915_tex_fixture_fhd_build_batch(
	uint32_t *cmds,
	unsigned capacity,
	uint64_t state_va,
	uint32_t mocs)
{
	unsigned count;

	/* Builds the command list with the full-HD sampling shader and target. */
	count = i915_draw_build_batch(cmds, capacity, state_va, mocs, &i915_draw_fhd_options);

	/* Succeeded: reports how many dwords were written. */
	return count;
}

/*
 * Reports the generated full-HD render target surface state.
 *
 * The sixteen dwords carry the placeholder address I915_TEX_FHD_RT_VA.
 */
const uint32_t *
drv_i915_tex_fixture_fhd_rt_rss(void)
{
	/* The generated words, for a comparison without the GPU. */
	return texfhd_rt_rss;
}

/*
 * Reports the size of the full-HD sampling shader in bytes.
 */
unsigned
drv_i915_tex_fixture_fhd_ps_bytes(void)
{
	/* The generated shader's size. */
	return TEXFHD_PS_BYTES;
}

/*
 * Tells whether the full-HD draw samples exactly as the 32x32 draw does.
 *
 * The texture's surface state, the sampler, the pixel shader dispatch
 * words and the sampler pointer must be the 32x32 draw's; only the scale of
 * uv and the target may differ.  Returns 1 when they are the same and 0
 * otherwise.
 */
int
drv_i915_tex_fixture_fhd_same_texture_state(void)
{
	int differs;

	/* Compares the texture's surface states. */
	differs = kern_memcmp(texfhd_tex_rss, texfix_tex_rss, sizeof(texfix_tex_rss));
	if (differs != 0)
		return 0;

	/* Compares the samplers. */
	differs = kern_memcmp(texfhd_sampler, texfix_sampler, sizeof(texfix_sampler));
	if (differs != 0)
		return 0;

	/* Compares the pixel shader's sampler and binding table counts. */
	if (TEXFHD_3DSTATE_PS_DW3 != TEXFIX_3DSTATE_PS_DW3)
		return 0;

	/* Compares the pixel shader's dispatch GRF start. */
	if (TEXFHD_3DSTATE_PS_DW7 != TEXFIX_3DSTATE_PS_DW7)
		return 0;

	/* Compares the pixel shader's payload requirements. */
	if (TEXFHD_3DSTATE_PS_EXTRA_DW1 != TEXFIX_3DSTATE_PS_EXTRA_DW1)
		return 0;

	/* Compares the sampler pointers. */
	if (TEXFHD_SAMPLER_POINTERS_PS_DW1 != TEXFIX_SAMPLER_POINTERS_PS_DW1)
		return 0;

	/* Succeeded: the two draws sample the same texture the same way. */
	return 1;
}

/* Carpets length bytes of the page from offset on with thread-ending instructions. */
static void
i915_draw_fill_eot(
	void *page,
	unsigned offset,
	unsigned length)
{
	unsigned at;

	/* Writes one instruction per 16 bytes while a whole one still fits. */
	for (at = offset;
	     at + sizeof(i915_draw_eot_only) <= offset + length;
	     at += sizeof(i915_draw_eot_only))
		kern_memcpy((uint8_t *)page + at, i915_draw_eot_only, sizeof(i915_draw_eot_only));
}

/* Copies a pixel shader to the kernel offset and carpets the room after it. */
static void
i915_draw_write_kernel(
	void *page,
	const uint32_t *kernel,
	unsigned kernel_bytes)
{
	/* Places the kernel where 3DSTATE_PS names it. */
	kern_memcpy((uint8_t *)page + I915_DRAW_FIXTURE_PS_OFFSET, kernel, kernel_bytes);

	/* Makes a stray thread in the rest of the room retire at once. */
	i915_draw_fill_eot(page, I915_DRAW_FIXTURE_PS_OFFSET + kernel_bytes, I915_DRAW_KERNEL_ROOM - kernel_bytes);
}

/* Writes the binding table and the 32x32 render target's RENDER_SURFACE_STATE. */
static void
i915_draw_write_surface_state(
	uint32_t *heap,
	uint64_t rt_va,
	uint32_t mocs)
{
	uint32_t *surface;

	/* Binding table entry 0 is the surface state's offset from the surface base. */
	heap[I915_DRAW_BINDING_TABLE_OFFSET / 4U] = I915_DRAW_SURFACE_STATE_OFFSET;

	/* Starts the surface state from zeroes. */
	surface = &heap[I915_DRAW_SURFACE_STATE_OFFSET / 4U];
	kern_memset(surface, 0, GEN12_RENDER_SURFACE_STATE_DWORDS * 4U);

	/* A linear two-dimensional BGRA target; the alignment fields are unused when linear. */
	surface[0] = (GEN12_SURFTYPE_2D << 29) |
		(GEN12_FORMAT_B8G8R8A8_UNORM << 18) |
		(GEN12_SURFACE_ALIGN_4 << 16) |
		(GEN12_SURFACE_ALIGN_4 << 14) |
		(GEN12_TILEMODE_LINEAR << 12);

	/*
	 * Dword 1 carries MOCS (30:24), the QPitch, and bit 31 "Enable Unorm
	 * Path In Color Pipe", which the colour pipe needs for a UNORM render
	 * target: isl sets it, and without it the render target write never
	 * completes.
	 */
	surface[1] = (1U << 31) | (mocs << 24) | 8U;

	/* The size and the pitch, each stored less one. */
	surface[2] = (I915_DRAW_FIXTURE_WIDTH - 1U) | ((I915_DRAW_FIXTURE_HEIGHT - 1U) << 16);
	surface[3] = I915_DRAW_PITCH - 1U;

	/* The mip-tail start LOD isl programs for a single-level surface. */
	surface[5] = 0x00000100U;

	/* Channels are taken straight from the target's own components. */
	surface[7] = (4U << 25) | (5U << 22) | (6U << 19) | (7U << 16);

	/* The target's address. */
	surface[8] = (uint32_t)rt_va;
	surface[9] = (uint32_t)(rt_va >> 32);
}

/* Writes the colour calculator, blend, viewport and coarse pixel states. */
static void
i915_draw_write_dynamic_state(
	uint32_t *heap)
{
	uint32_t *blend;
	uint32_t *viewport;

	/* A zeroed colour calculator state, and a zeroed CPS_STATE, which disables coarse pixel shading. */
	kern_memset(&heap[I915_DRAW_COLOR_CALC_OFFSET / 4U], 0, I915_DRAW_COLOR_CALC_BYTES);
	kern_memset(&heap[I915_DRAW_CPS_STATE_OFFSET / 4U], 0, GEN12_CPS_STATE_DWORDS * 4U);

	/* BLEND_STATE is one dword of global controls, then one entry per target. */
	blend = &heap[I915_DRAW_BLEND_OFFSET / 4U];
	blend[0] = 0U;
	blend[1] = 0U;
	blend[2] = 1U | (1U << 1) | (GEN12_COLORCLAMP_RTFORMAT << 2);

	/* The depth range is the full unit interval even though depth is unused. */
	viewport = &heap[I915_DRAW_CC_VIEWPORT_OFFSET / 4U];
	viewport[0] = GEN12_F32_0;
	viewport[1] = GEN12_F32_1;
}

/* Writes the three screen-space vertices of a RECTLIST covering the target. */
static void
i915_draw_write_vertices(
	uint32_t *page,
	uint32_t width_bits,
	uint32_t height_bits)
{
	uint32_t *position;

	/* Starts the vertex data from zeroes; the VUE header buffer stays zero. */
	kern_memset(&page[I915_DRAW_POSITION_OFFSET / 4U], 0, I915_DRAW_POSITION_BYTES);
	position = &page[I915_DRAW_POSITION_OFFSET / 4U];

	/*
	 * A rectangle is three vertices and the fourth corner is implied:
	 * (width, height), (0, height) and (0, 0), each with z = 0.
	 */
	position[0] = width_bits;
	position[1] = height_bits;
	position[2] = GEN12_F32_0;
	position[3] = GEN12_F32_0;
	position[4] = height_bits;
	position[5] = GEN12_F32_0;
	position[6] = GEN12_F32_0;
	position[7] = GEN12_F32_0;
	position[8] = GEN12_F32_0;
}

/* Copies a generated RENDER_SURFACE_STATE to offset in the surface heap and patches its address. */
static void
i915_tex_write_surface_state(
	uint32_t *heap,
	unsigned offset,
	const uint32_t *template_state,
	uint64_t tex_va)
{
	uint32_t *surface;

	/* Copies the sixteen generated dwords. */
	surface = &heap[offset / 4U];
	kern_memcpy(surface, template_state, GEN12_RENDER_SURFACE_STATE_DWORDS * 4U);

	/* Replaces the placeholder address with the surface's own. */
	surface[8] = (uint32_t)tex_va;
	surface[9] = (uint32_t)(tex_va >> 32);
}

/* Appends one dword to the batch, dropping it when the batch is full. */
static void
i915_draw_emit(
	struct i915_draw_batch *batch,
	uint32_t dword)
{
	/* A full batch drops the write; the caller checks the count afterwards. */
	if (batch->count >= batch->capacity)
		return;

	/* Stores the dword and counts it. */
	batch->cmds[batch->count] = dword;
	batch->count++;
}

/* Appends count zero dwords to the batch. */
static void
i915_draw_emit_zeroes(
	struct i915_draw_batch *batch,
	unsigned count)
{
	unsigned index;

	/* Emits each zero dword. */
	for (index = 0U; index < count; index++)
		i915_draw_emit(batch, 0U);
}

/* Appends a packet header followed by zero dwords: a stage or state left disabled. */
static void
i915_draw_emit_disabled(
	struct i915_draw_batch *batch,
	uint32_t opcode,
	uint32_t dwords)
{
	/* The header names the packet's full length. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(opcode, dwords));

	/* Every dword after the header is zero. */
	i915_draw_emit_zeroes(batch, dwords - 1U);
}

/* Appends an MI_STORE_DWORD_IMM of value to address in the batch's address space. */
static void
i915_draw_emit_marker(
	struct i915_draw_batch *batch,
	uint64_t address,
	uint32_t value)
{
	/* The store, the address in two halves, then the value. */
	i915_draw_emit(batch, MI_STORE_DWORD_IMM_GEN4);
	i915_draw_emit(batch, (uint32_t)address);
	i915_draw_emit(batch, (uint32_t)(address >> 32));
	i915_draw_emit(batch, value);
}

/* Appends a six-dword PIPE_CONTROL with flags and no post-sync write. */
static void
i915_draw_emit_pipe_control(
	struct i915_draw_batch *batch,
	uint32_t flags)
{
	uint32_t header;

	/* A render target flush also needs the HDC pipeline flush on this generation. */
	header = GFX_OP_PIPE_CONTROL(6);
	if ((flags & PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH) != 0U)
		header |= PIPE_CONTROL0_HDC_PIPELINE_FLUSH;

	/* The header, the flags, and no address or data. */
	i915_draw_emit(batch, header);
	i915_draw_emit(batch, flags);
	i915_draw_emit_zeroes(batch, 4U);
}

/* Appends STATE_BASE_ADDRESS with every heap anchored at the state page. */
static void
i915_draw_emit_state_base_address(
	struct i915_draw_batch *batch,
	uint64_t state_va,
	uint32_t mocs)
{
	uint32_t page_low;
	uint32_t page_high;

	/* The state page's address, page aligned, in two halves. */
	page_low = (uint32_t)state_va & 0xfffff000U;
	page_high = (uint32_t)(state_va >> 32);

	/* The header. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_STATE_BASE_ADDRESS, GEN12_STATE_BASE_ADDRESS_DWORDS));

	/* The general state base: address 0, modified, and the stateless MOCS. */
	i915_draw_emit(batch, 1U | (mocs << 4));
	i915_draw_emit(batch, 0U);
	i915_draw_emit(batch, mocs << 16);

	/* The surface state base. */
	i915_draw_emit(batch, 1U | (mocs << 4) | page_low);
	i915_draw_emit(batch, page_high);

	/* The dynamic state base. */
	i915_draw_emit(batch, 1U | (mocs << 4) | page_low);
	i915_draw_emit(batch, page_high);

	/* The indirect object base: address 0, modified. */
	i915_draw_emit(batch, 1U | (mocs << 4));
	i915_draw_emit(batch, 0U);

	/* Instructions are fetched through the L3 into the instruction cache: write-back. */
	i915_draw_emit(batch, 1U | (GEN12_MOCS(I915_MOCS_WRITEBACK_INDEX) << 4) | page_low);
	i915_draw_emit(batch, page_high);

	/* The general, dynamic, indirect and instruction sizes cover everything. */
	i915_draw_emit(batch, I915_DRAW_SBA_FULL_SIZE);
	i915_draw_emit(batch, I915_DRAW_SBA_FULL_SIZE);
	i915_draw_emit(batch, I915_DRAW_SBA_FULL_SIZE);
	i915_draw_emit(batch, I915_DRAW_SBA_FULL_SIZE);

	/* Bindless surface state shares the surface heap. */
	i915_draw_emit(batch, 1U | (mocs << 4) | page_low);
	i915_draw_emit(batch, page_high);
	i915_draw_emit(batch, I915_DRAW_SBA_BINDLESS_SIZE);

	/* Bindless samplers are null. */
	i915_draw_emit(batch, 1U | (mocs << 4));
	i915_draw_emit(batch, 0U);
	i915_draw_emit(batch, 0U);
}

/* Appends the once-per-context state anv programs before its first draw. */
static void
i915_draw_emit_context_defaults(
	struct i915_draw_batch *batch)
{
	unsigned index;

	/*
	 * The depth-clear override is cleared: a fresh context does not
	 * guarantee it zero and it is a known source of hangs.
	 */
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_WM_HZ_OP, GEN12_3DSTATE_WM_HZ_OP_DWORDS);

	/* Antialiased lines, chroma key, stipples: all off. */
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_AA_LINE_PARAMETERS, GEN12_3DSTATE_AA_LINE_PARAMETERS_DWORDS);
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_WM_CHROMAKEY, GEN12_3DSTATE_WM_CHROMAKEY_DWORDS);
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_POLY_STIPPLE_OFFSET, GEN12_3DSTATE_POLY_STIPPLE_OFFSET_DWORDS);
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_LINE_STIPPLE, GEN12_3DSTATE_LINE_STIPPLE_DWORDS);

	/* The single sample sits at the pixel centre; dword 8 holds the one-sample position. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_SAMPLE_PATTERN, GEN12_3DSTATE_SAMPLE_PATTERN_DWORDS));
	for (index = 1U; index < GEN12_3DSTATE_SAMPLE_PATTERN_DWORDS; index++) {
		if (index == 8U) {
			i915_draw_emit(batch, GEN12_SAMPLE_PATTERN_1X_CENTRE);
		} else {
			i915_draw_emit(batch, 0U);
		}
	}

	/* Depth bounds are off. */
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_DEPTH_BOUNDS, GEN12_3DSTATE_DEPTH_BOUNDS_DWORDS);

	/* The other stages' binding tables are cleared. */
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_BINDING_TABLE_POINTERS_VS, GEN12_3DSTATE_POINTERS_DWORDS);
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_BINDING_TABLE_POINTERS_HS, GEN12_3DSTATE_POINTERS_DWORDS);
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_BINDING_TABLE_POINTERS_DS, GEN12_3DSTATE_POINTERS_DWORDS);
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_BINDING_TABLE_POINTERS_GS, GEN12_3DSTATE_POINTERS_DWORDS);
}

/* Appends the vertex fetch state: two buffers, two elements and the topology. */
static void
i915_draw_emit_vertex_state(
	struct i915_draw_batch *batch,
	uint64_t vb_va,
	uint32_t mocs)
{
	uint64_t position_va;
	uint64_t header_va;

	/*
	 * With the vertex shader disabled the clipper reads each VUE straight
	 * from the URB, so the fetcher builds the VUE header from a zero-filled
	 * buffer and the position from the vertex data.
	 */
	position_va = vb_va + I915_DRAW_POSITION_OFFSET;
	header_va = vb_va + I915_DRAW_VUE_HEADER_OFFSET;

	/* Two VERTEX_BUFFER_STATE structures follow the header dword. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VERTEX_BUFFERS,
					       1U + 2U * GEN12_VERTEX_BUFFER_STATE_DWORDS));

	/* Buffer 0 holds three positions of three floats each: pitch 12, 36 bytes. */
	i915_draw_emit(batch, (0U << 26) | GEN12_VERTEX_BUFFER_L3_BYPASS_DISABLE | (mocs << 16) | (1U << 14) | 12U);
	i915_draw_emit(batch, (uint32_t)position_va);
	i915_draw_emit(batch, (uint32_t)(position_va >> 32));
	i915_draw_emit(batch, 36U);

	/* Buffer 1 holds the zeroed VUE header, read with a zero pitch by every vertex. */
	i915_draw_emit(batch, (1U << 26) | GEN12_VERTEX_BUFFER_L3_BYPASS_DISABLE | (mocs << 16) | (1U << 14) | 0U);
	i915_draw_emit(batch, (uint32_t)header_va);
	i915_draw_emit(batch, (uint32_t)(header_va >> 32));
	i915_draw_emit(batch, 16U);

	/* Two VERTEX_ELEMENT_STATE structures follow the header dword. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VERTEX_ELEMENTS,
					       1U + 2U * GEN12_VERTEX_ELEMENT_STATE_DWORDS));

	/* Element 0 is the VUE header; only its first dword comes from memory. */
	i915_draw_emit(batch, (1U << 26) | (1U << 25) | (GEN12_FORMAT_R32G32B32A32_FLOAT << 16) | 0U);
	i915_draw_emit(batch, (GEN12_VFCOMP_STORE_SRC << 28) |
			      (GEN12_VFCOMP_STORE_0 << 24) |
			      (GEN12_VFCOMP_STORE_0 << 20) |
			      (GEN12_VFCOMP_STORE_0 << 16));

	/* Element 1 is the position; W is supplied as one because the buffer holds XYZ. */
	i915_draw_emit(batch, (0U << 26) | (1U << 25) | (GEN12_FORMAT_R32G32B32_FLOAT << 16) | 0U);
	i915_draw_emit(batch, (GEN12_VFCOMP_STORE_SRC << 28) |
			      (GEN12_VFCOMP_STORE_SRC << 24) |
			      (GEN12_VFCOMP_STORE_SRC << 20) |
			      (GEN12_VFCOMP_STORE_1_FP << 16));

	/* Statistics are enabled; cut index, SGVS and the second SGVS are off. */
	i915_draw_emit(batch, (GEN12_CMD_3DSTATE_VF_STATISTICS << 16) | 1U);
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_VF, GEN12_3DSTATE_VF_DWORDS);
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_VF_SGVS, GEN12_3DSTATE_VF_SGVS_DWORDS);
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_VF_SGVS_2, GEN12_3DSTATE_VF_SGVS_2_DWORDS);

	/* Instancing is per element and survives in the context, so elements 0 and 1 are cleared. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VF_INSTANCING, GEN12_3DSTATE_VF_INSTANCING_DWORDS));
	i915_draw_emit(batch, 0U);
	i915_draw_emit(batch, 0U);
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VF_INSTANCING, GEN12_3DSTATE_VF_INSTANCING_DWORDS));
	i915_draw_emit(batch, 1U);
	i915_draw_emit(batch, 0U);

	/* The primitive is a rectangle list. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VF_TOPOLOGY, GEN12_3DSTATE_VF_TOPOLOGY_DWORDS));
	i915_draw_emit(batch, GEN12_3DPRIM_RECTLIST);
}

/* Appends the push constant and URB allocation. */
static void
i915_draw_emit_urb(
	struct i915_draw_batch *batch)
{
	uint32_t opcode;

	/*
	 * No stage uses push constants, and the vertex through geometry stages
	 * get no push constant space.
	 */
	for (opcode = GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_VS;
	     opcode < GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_PS;
	     opcode++)
		i915_draw_emit_disabled(batch, opcode, GEN12_3DSTATE_PUSH_CONSTANT_ALLOC_DWORDS);

	/* The pixel stage keeps the push constant space. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_PS,
					       GEN12_3DSTATE_PUSH_CONSTANT_ALLOC_DWORDS));
	i915_draw_emit(batch, GEN12_PUSH_CONSTANT_KB);

	/*
	 * The URB is handed out in 8 KiB chunks in pipeline order; the vertex
	 * stage owns chunk 4 and every URB entry.
	 */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_URB_ALLOC_VS, GEN12_3DSTATE_URB_ALLOC_DWORDS));
	i915_draw_emit(batch, I915_DRAW_URB_VS_ALLOCATION);
	i915_draw_emit(batch, I915_DRAW_URB_VS_ENTRIES | (I915_DRAW_URB_VS_ENTRIES << 16));

	/* The other stages start past the vertex chunk and receive no entries. */
	for (opcode = GEN12_CMD_3DSTATE_URB_ALLOC_HS;
	     opcode <= GEN12_CMD_3DSTATE_URB_ALLOC_GS;
	     opcode++) {
		i915_draw_emit(batch, GEN12_CMD_HEADER(opcode, GEN12_3DSTATE_URB_ALLOC_DWORDS));
		i915_draw_emit(batch, I915_DRAW_URB_OTHER_ALLOCATION);
		i915_draw_emit(batch, 0U);
	}
}

/* Appends an empty push constant packet for every stage. */
static void
i915_draw_emit_constants(
	struct i915_draw_batch *batch,
	uint32_t mocs)
{
	unsigned stage;

	/* Declares each stage's constants empty rather than leaving them undefined. */
	for (stage = 0U; stage < sizeof(i915_draw_constant_opcodes) / sizeof(i915_draw_constant_opcodes[0]); stage++) {
		i915_draw_emit(batch, GEN12_CMD_HEADER(i915_draw_constant_opcodes[stage], GEN12_3DSTATE_CONSTANT_DWORDS) |
				      (mocs << 8));
		i915_draw_emit_zeroes(batch, GEN12_3DSTATE_CONSTANT_DWORDS - 1U);
	}
}

/* Appends the dynamic heap's state pointers, the binding table pool and the sample state. */
static void
i915_draw_emit_state_pointers(
	struct i915_draw_batch *batch,
	uint32_t mocs)
{
	/* The colour calculator and blend states, each pointer with its valid bit. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_CC_STATE_POINTERS, GEN12_3DSTATE_POINTERS_DWORDS));
	i915_draw_emit(batch, I915_DRAW_COLOR_CALC_OFFSET | 1U);
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_BLEND_STATE_POINTERS, GEN12_3DSTATE_POINTERS_DWORDS));
	i915_draw_emit(batch, I915_DRAW_BLEND_OFFSET | 1U);

	/* The colour calculator viewport and the coarse pixel state. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VIEWPORT_STATE_POINTERS_CC,
					       GEN12_3DSTATE_POINTERS_DWORDS));
	i915_draw_emit(batch, I915_DRAW_CC_VIEWPORT_OFFSET);
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_CPS_POINTERS, GEN12_3DSTATE_POINTERS_DWORDS));
	i915_draw_emit(batch, I915_DRAW_CPS_STATE_OFFSET);

	/* The binding table pool is disabled, but its MOCS must still be valid. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_BINDING_TABLE_POOL_ALLOC, 4U));
	i915_draw_emit(batch, mocs);
	i915_draw_emit(batch, 0U);
	i915_draw_emit(batch, 0U);

	/* One sample per pixel, and that sample enabled. */
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_MULTISAMPLE, GEN12_3DSTATE_MULTISAMPLE_DWORDS);
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_SAMPLE_MASK, GEN12_3DSTATE_SAMPLE_MASK_DWORDS));
	i915_draw_emit(batch, 1U);
}

/* Appends the disabled geometry stages, so the vertex fetcher feeds the clipper. */
static void
i915_draw_emit_geometry_stages(
	struct i915_draw_batch *batch)
{
	unsigned index;

	/* The vertex shader is off; dword 7 sets only its statistics enable. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_VS, GEN12_3DSTATE_VS_DWORDS));
	for (index = 1U; index < GEN12_3DSTATE_VS_DWORDS; index++) {
		if (index == 7U) {
			i915_draw_emit(batch, 1U << 10);
		} else {
			i915_draw_emit(batch, 0U);
		}
	}

	/* Tessellation, streamout, geometry shading and primitive replication are off. */
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_HS, GEN12_3DSTATE_HS_DWORDS);
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_TE, GEN12_3DSTATE_TE_DWORDS);
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_DS, GEN12_3DSTATE_DS_DWORDS);
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_STREAMOUT, GEN12_3DSTATE_STREAMOUT_DWORDS);
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_GS, GEN12_3DSTATE_GS_DWORDS);
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_PRIMITIVE_REPLICATION,
				GEN12_3DSTATE_PRIMITIVE_REPLICATION_DWORDS);
}

/* Appends the clipper, setup, rasterizer, windower and pixel shader state. */
static void
i915_draw_emit_raster_state(
	struct i915_draw_batch *batch,
	const struct i915_draw_options *options)
{
	unsigned index;

	/*
	 * A screen-space rectangle needs no viewport transform and no
	 * perspective divide: the clipper consumes the URB entries directly,
	 * with statistics enabled.
	 */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_CLIP, GEN12_3DSTATE_CLIP_DWORDS));
	i915_draw_emit(batch, 1U << 10);
	i915_draw_emit(batch, 1U << 9);
	i915_draw_emit(batch, 0U);

	/* Viewport mapping is disabled, as a rectangle list requires; the deref block size matches the URB. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_SF, GEN12_3DSTATE_SF_DWORDS));
	i915_draw_emit(batch, 1U << 10);
	i915_draw_emit(batch, GEN12_URB_DEREF_BLOCK_SIZE_32 << 29);
	i915_draw_emit(batch, 0U);

	/*
	 * Both faces are filled solid and neither is culled.  The cull mode has
	 * to be named: its zero value is CULLMODE_BOTH, which discards
	 * everything.
	 */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_RASTER, GEN12_3DSTATE_RASTER_DWORDS));
	i915_draw_emit(batch, GEN12_CULLMODE_NONE << 16);
	i915_draw_emit_zeroes(batch, GEN12_3DSTATE_RASTER_DWORDS - 2U);

	/* The setup stage forwards no attributes, no point sprite coordinates and no constant interpolation. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_SBE, GEN12_3DSTATE_SBE_DWORDS));
	i915_draw_emit(batch, (1U << 5) | (1U << 11) | (1U << 28) | (1U << 29));
	i915_draw_emit(batch, 0U);
	i915_draw_emit(batch, 0U);

	/* Sixteen two-bit component slots per dword, every one of them all four components. */
	for (index = 4U; index < GEN12_3DSTATE_SBE_DWORDS; index++)
		i915_draw_emit(batch, 0xffffffffU);

	/* The windower: statistics on, thread dispatch forced on, early depth/stencil as the shader executes. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_WM, GEN12_3DSTATE_WM_DWORDS));
	i915_draw_emit(batch, (1U << 31) | (2U << 19) | (1U << 21));

	/* The pixel shader and its extra dispatch state. */
	i915_draw_emit_pixel_shader(batch, options);

	/* The target is writeable, so the colour pipe is not short-circuited. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_PS_BLEND, GEN12_3DSTATE_PS_BLEND_DWORDS));
	i915_draw_emit(batch, 1U << 30);

	/* Depth and stencil tests are off. */
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_WM_DEPTH_STENCIL, GEN12_3DSTATE_WM_DEPTH_STENCIL_DWORDS);
}

/* Appends 3DSTATE_PS and 3DSTATE_PS_EXTRA for the fixture's pixel shader. */
static void
i915_draw_emit_pixel_shader(
	struct i915_draw_batch *batch,
	const struct i915_draw_options *options)
{
	unsigned index;

	/* The header. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_PS, GEN12_3DSTATE_PS_DWORDS));

	/* Fills every dword of the packet; the ones not named here are zero. */
	for (index = 1U; index < GEN12_3DSTATE_PS_DWORDS; index++) {
		/* Picks the dword's content. */
		switch (index) {
		case 1U:
			/* The kernel is named by its offset from the instruction base. */
			i915_draw_emit(batch, I915_DRAW_FIXTURE_PS_OFFSET);
			break;
		case 3U:
			/* The sampler and binding table entry counts. */
			i915_draw_emit(batch, options->ps_dw3);
			break;
		case 6U:
			/* The largest thread count, which must be nonzero, and SIMD8 dispatch. */
			i915_draw_emit(batch, ((GEN12_MAX_THREADS_PER_PSD - 1U) << 23) | 1U);
			break;
		case 7U:
			/* The SIMD8 kernel's dispatch GRF start. */
			i915_draw_emit(batch, options->ps_dw7);
			break;
		default:
			i915_draw_emit(batch, 0U);
			break;
		}
	}

	/* The shader is valid and has the payload and UAV the compiler asked for. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_PS_EXTRA, GEN12_3DSTATE_PS_EXTRA_DWORDS));
	i915_draw_emit(batch, options->ps_extra_dw1);
}

/* Appends the null depth, stencil and hierarchical depth buffers and no clear value. */
static void
i915_draw_emit_depth_state(
	struct i915_draw_batch *batch)
{
	/* A null depth buffer still carries a depth format. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_DEPTH_BUFFER, GEN12_3DSTATE_DEPTH_BUFFER_DWORDS));
	i915_draw_emit(batch, (GEN12_SURFTYPE_NULL << 29) | (GEN12_DEPTH_FORMAT_D32_FLOAT << 24));
	i915_draw_emit_zeroes(batch, GEN12_3DSTATE_DEPTH_BUFFER_DWORDS - 2U);

	/* The stencil buffer carries its own surface type from this generation on. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_STENCIL_BUFFER, GEN12_3DSTATE_STENCIL_BUFFER_DWORDS));
	i915_draw_emit(batch, GEN12_SURFTYPE_NULL << 29);
	i915_draw_emit_zeroes(batch, GEN12_3DSTATE_STENCIL_BUFFER_DWORDS - 2U);

	/* No hierarchical depth and no clear value. */
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_HIER_DEPTH_BUFFER, GEN12_3DSTATE_HIER_DEPTH_BUFFER_DWORDS);
	i915_draw_emit_disabled(batch, GEN12_CMD_3DSTATE_CLEAR_PARAMS, GEN12_3DSTATE_CLEAR_PARAMS_DWORDS);
}

/* Appends the 3DPRIMITIVE: three sequential vertices of one instance form the rectangle. */
static void
i915_draw_emit_primitive(
	struct i915_draw_batch *batch)
{
	/* The header and the topology. */
	i915_draw_emit(batch, GEN12_CMD_HEADER(GEN12_CMD_3DPRIMITIVE, GEN12_3DPRIMITIVE_DWORDS));
	i915_draw_emit(batch, GEN12_3DPRIM_RECTLIST);

	/* Three vertices from vertex 0, one instance from instance 0, no base vertex. */
	i915_draw_emit(batch, 3U);
	i915_draw_emit(batch, 0U);
	i915_draw_emit(batch, 1U);
	i915_draw_emit(batch, 0U);
	i915_draw_emit(batch, 0U);
}

/* Builds a fixture's whole 3D pipeline batch and returns its dword count. */
static unsigned
i915_draw_build_batch(
	uint32_t *cmds,
	unsigned capacity,
	uint64_t state_va,
	uint32_t mocs,
	const struct i915_draw_options *options)
{
	struct i915_draw_batch batch;
	uint64_t marker_va;

	/* Starts an empty batch over the caller's array. */
	batch.cmds = cmds;
	batch.count = 0U;
	batch.capacity = capacity;
	marker_va = state_va + I915_DRAW_FIXTURE_MARKER_OFFSET;

	/* A stalling flush precedes the pipeline select and the base addresses. */
	i915_draw_emit_pipe_control(&batch,
				    PIPE_CONTROL_CS_STALL |
				    PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
				    PIPE_CONTROL_DEPTH_CACHE_FLUSH |
				    PIPE_CONTROL_DC_FLUSH_ENABLE |
				    PIPE_CONTROL_FLUSH_ENABLE);
	i915_draw_emit(&batch, GEN12_PIPELINE_SELECT_DWORD(GEN12_PIPELINE_SELECT_3D));

	/* Anchors the three heaps this draw reads from at the state page. */
	i915_draw_emit_state_base_address(&batch, state_va, mocs);

	/* The state caches must be invalidated before anything reads the new bases. */
	i915_draw_emit_pipe_control(&batch,
				    PIPE_CONTROL_CS_STALL |
				    PIPE_CONTROL_STATE_CACHE_INVALIDATE |
				    PIPE_CONTROL_CONST_CACHE_INVALIDATE |
				    PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE |
				    PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE);

	/* Tells a hang before the draw state from one after it. */
	i915_draw_emit_marker(&batch, marker_va + I915_DRAW_MARKER_BEFORE_SLOT, I915_DRAW_FIXTURE_MARKER_BEFORE);

	/* Programs the once-per-context state. */
	i915_draw_emit_context_defaults(&batch);

	/* Programs the vertex fetch, the URB and the empty push constants. */
	i915_draw_emit_vertex_state(&batch, state_va, mocs);
	i915_draw_emit_urb(&batch);
	i915_draw_emit_constants(&batch, mocs);

	/* Points the pipeline at the dynamic heap's states. */
	i915_draw_emit_state_pointers(&batch, mocs);

	/* Disables every geometry stage. */
	i915_draw_emit_geometry_stages(&batch);

	/* Programs the rasterizer, the pixel shader and the null depth buffers. */
	i915_draw_emit_raster_state(&batch, options);
	i915_draw_emit_depth_state(&batch);

	/* A textured draw names its sampler state before the binding table. */
	if (options->sampler_pointers != 0U) {
		i915_draw_emit(&batch, options->ssp_dw0);
		i915_draw_emit(&batch, options->ssp_dw1);
	}

	/* The pixel shader's binding table names the render target (and the texture). */
	i915_draw_emit(&batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_BINDING_TABLE_POINTERS_PS,
						GEN12_3DSTATE_POINTERS_DWORDS));
	i915_draw_emit(&batch, I915_DRAW_BINDING_TABLE_OFFSET);

	/* Rasterization is clipped to the target's own extent. */
	i915_draw_emit(&batch, GEN12_CMD_HEADER(GEN12_CMD_3DSTATE_DRAWING_RECTANGLE,
						GEN12_3DSTATE_DRAWING_RECTANGLE_DWORDS));
	i915_draw_emit(&batch, 0U);
	i915_draw_emit(&batch, (options->rt_w - 1U) | ((options->rt_h - 1U) << 16));
	i915_draw_emit(&batch, 0U);

	/*
	 * The pixel pipeline is synced before the primitive: a command streamer
	 * stall that also waits on the pixel scoreboard settles the windower
	 * and pixel shader setup state a fresh context leaves undefined.
	 */
	i915_draw_emit_pipe_control(&batch,
				    PIPE_CONTROL_CS_STALL |
				    PIPE_CONTROL_STALL_AT_SCOREBOARD |
				    PIPE_CONTROL_DEPTH_STALL_ENABLE);

	/* Proves the command streamer cleared the pre-draw stall and issued the primitive. */
	i915_draw_emit_marker(&batch, marker_va + I915_DRAW_MARKER_MIDDRAW_SLOT, I915_DRAW_FIXTURE_MARKER_MIDDRAW);

	/* Draws the rectangle. */
	i915_draw_emit_primitive(&batch);

	/* The draw's writes are flushed before the marker reports completion. */
	i915_draw_emit_pipe_control(&batch,
				    PIPE_CONTROL_CS_STALL |
				    PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
				    PIPE_CONTROL_DC_FLUSH_ENABLE |
				    PIPE_CONTROL_FLUSH_ENABLE);
	i915_draw_emit_marker(&batch, marker_va + I915_DRAW_MARKER_AFTER_SLOT, I915_DRAW_FIXTURE_MARKER_AFTER);

	/* Ends the batch, padded to an even dword count. */
	i915_draw_emit(&batch, MI_BATCH_BUFFER_END);
	i915_draw_emit(&batch, MI_NOOP);

	/* Succeeded: reports how many dwords were written. */
	return batch.count;
}
