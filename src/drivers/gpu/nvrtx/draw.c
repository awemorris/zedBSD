/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The first draw (the draw-test stage, nvrtx.draw=1).
 *
 * After the first compute launch (compute-test) has run on its graphics
 * channel, the driver makes a 3D object (TURING_A) on the same channel, with
 * the 3D and compute classes on one channel (subchannels 0 and 1), and draws
 * a quad three times, checking each result from the CPU:
 *
 *   1. into a 256x256 pitch-linear A8B8G8R8 colour target in system
 *      memory: a clear of the whole target, then a quad of one colour over
 *      its middle 128x128 (the programs the nvrtx compiler makes from the
 *      IR of the compositor's quad, the colour from the push constants);
 *   2. the same into a colour target in VRAM (a buffer object of the
 *      VRAM allocator, read back through the CPU's window; without the
 *      allocator the compute arena's last MiB, read back at a few points
 *      through the register window), so one run says whether the raster
 *      operations write system memory, VRAM or both;
 *   3. the quad again into the system-memory target, textured: a 4x4
 *      pitch-linear texture of four colours sampled with nearest filtering
 *      through its texture header and sampler pools.
 *
 * Each draw is its own submission on the compute channel's ring (entries
 * 1 to 3) with its own fence.  The compute launch's tables map the
 * draw's system-memory pages after its own and the VRAM target
 * (compute.c).  The stop frees the 3D object before the compute object
 * and the channel.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-draw.h"
#include "drivers/gpu/nvrtx/nvrtx-gpu.h"
#include "drivers/gpu/nvrtx/compiler/sm75/sm75.h"

/* The draw's system-memory pages in the range, in order (NVRTX_DRAW_PAGES in all). */
#define DRAW_PUSH_PAGE			0U
#define DRAW_PUSH_PAGES			4U
#define DRAW_PROGRAM_PAGE		4U
#define DRAW_PROGRAMS			4U
#define DRAW_CONSTANTS_PAGE		8U
#define DRAW_VERTICES_PAGE		9U
#define DRAW_TEXTURE_PAGE		10U
#define DRAW_HEADERS_PAGE		11U
#define DRAW_SAMPLERS_PAGE		12U
#define DRAW_ZERO_PAGE			13U
#define DRAW_TARGET_PAGE		14U
#define DRAW_TARGET_PAGES		64U

/* The four programs, one a page: the quad in one colour and textured. */
#define DRAW_VERTEX			0U
#define DRAW_FRAGMENT			1U
#define DRAW_TEXTURED_VERTEX		2U
#define DRAW_TEXTURED_FRAGMENT		3U

/* A program's header in front of its code. */
#define DRAW_HEADER_BYTES		(NVRTX_SM75_SPH_WORDS * 4U)

/* The three submissions: their ring entries, their parts of the push buffer, their fences. */
#define DRAW_SYSTEM_ENTRY		1U
#define DRAW_VRAM_ENTRY			2U
#define DRAW_TEXTURED_ENTRY		3U
#define DRAW_SYSTEM_PUSH_PAGE		0U
#define DRAW_SYSTEM_PUSH_PAGES		2U
#define DRAW_VRAM_PUSH_PAGE		2U
#define DRAW_TEXTURED_PUSH_PAGE		3U
#define DRAW_SYSTEM_FENCE		0x4243U
#define DRAW_VRAM_FENCE			0x4244U
#define DRAW_TEXTURED_FENCE		0x4245U

/* The colour targets: 256x256 pixels of four bytes, and the quad over the middle 128x128. */
#define DRAW_SIZE			256U
#define DRAW_PITCH			(DRAW_SIZE * 4U)
#define DRAW_QUAD_LOW			64U
#define DRAW_QUAD_HIGH			192U

/* Constant buffer 0 as the selection gives it to the programs: one page. */
#define DRAW_CONSTANTS_BYTES		0x1000U

/* The six corners of the quad's two triangles, two floats each. */
#define DRAW_VERTEX_COUNT		6U
#define DRAW_VERTEX_STRIDE		8U

/* The texture: 4x4 texels of four bytes, its rows 64 bytes apart (a multiple of 32). */
#define DRAW_TEXELS			4U
#define DRAW_TEXTURE_PITCH		64U

/*
 * The words the target holds (A8B8G8R8: red in the lowest byte): what was
 * there before, the clear's cyan, the quad's yellow, and the texture's
 * red, green, blue and white.
 */
#define DRAW_PATTERN			0xdeadbeefU
#define DRAW_CLEAR_WORD			0xffffff00U
#define DRAW_QUAD_WORD			0xff00ffffU
#define DRAW_RED			0xff0000ffU
#define DRAW_GREEN			0xff00ff00U
#define DRAW_BLUE			0xffff0000U
#define DRAW_WHITE			0xffffffffU

/* The floats the push constants and vertices take, by their bits. */
#define DRAW_FLOAT_ZERO			0x00000000U
#define DRAW_FLOAT_ONE			0x3f800000U
#define DRAW_FLOAT_HALF			0x3f000000U
#define DRAW_FLOAT_MINUS_HALF		0xbf000000U

/* The points of the VRAM target read back: the corners of the quad and of the target, its middle. */
#define DRAW_VRAM_POINTS		9U

static int draw_object(struct nvrtx_gpu *gpu);
static int draw_programs(struct nvrtx_gpu *gpu, uint32_t *gprs);
static int draw_compile(struct nvrtx_gpu *gpu, uint32_t index, const struct nvrtx_draw_ir *program, uint32_t *gprs);
static void draw_data(struct nvrtx_gpu *gpu);
static int draw_system(struct nvrtx_gpu *gpu, const uint32_t *gprs);
static int draw_vram(struct nvrtx_gpu *gpu);
static int draw_check_vram(struct nvrtx_gpu *gpu);
static int draw_textured(struct nvrtx_gpu *gpu, const uint32_t *gprs);
static int draw_run(struct nvrtx_gpu *gpu, uint32_t entry, uint32_t page, uint32_t pages, size_t length, uint32_t fence, const char *what);
static int draw_check_system(struct nvrtx_gpu *gpu, int textured, const char *what);
static uint32_t draw_expected(uint32_t x, uint32_t y, int textured);
static void draw_fill_system(struct nvrtx_gpu *gpu);
static uint64_t draw_page_va(uint32_t page);
static uint8_t *draw_push(struct nvrtx_gpu *gpu, uint32_t page);

/*
 * Allocates the draw's buffers: in system memory the push buffer, the
 * programs, constant buffer 0, the vertices, the texture with its header
 * and sampler pools, the zero page and the colour target; in VRAM the
 * colour target, a buffer object mapped into the CPU's window when the
 * VRAM allocator and the window are up, else arena_target.
 *
 * Called with the compute launch's buffers, before its tables are built.
 */
int
nvrtx_draw_buffers(
	struct nvrtx_gpu *gpu,
	uint64_t arena_target)
{
	struct nvrtx_draw_state *draw;
	int error;

	draw = &gpu->draw;

	/* The push buffer and the programs, several pages each. */
	error = nvrtx_buffer_alloc(gpu, DRAW_PUSH_PAGES * NVRTX_GSP_PAGE_SIZE, &draw->push);
	if (error != 0)
		return error;

	error = nvrtx_buffer_alloc(gpu, DRAW_PROGRAMS * NVRTX_GSP_PAGE_SIZE, &draw->programs);
	if (error != 0)
		return error;

	/* One page each for the constants, the vertices, the texture, its pools and the zero page. */
	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &draw->constants);
	if (error != 0)
		return error;

	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &draw->vertices);
	if (error != 0)
		return error;

	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &draw->texture);
	if (error != 0)
		return error;

	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &draw->headers);
	if (error != 0)
		return error;

	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &draw->samplers);
	if (error != 0)
		return error;

	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &draw->zero);
	if (error != 0)
		return error;

	/* The colour target, 256 KiB. */
	error = nvrtx_buffer_alloc(gpu, DRAW_TARGET_PAGES * NVRTX_GSP_PAGE_SIZE, &draw->target);
	if (error != 0)
		return error;

	/* The VRAM target: a buffer object when the allocator gives one, else the arena's. */
	draw->vram_target = arena_target;
	error = nvrtx_bo_alloc(gpu, NVRTX_DRAW_VRAM_BYTES, NVRTX_GSP_PAGE_SIZE, &draw->vram_bo);
	if (error == 0) {
		draw->vram_bo_made = 1;
		draw->vram_target = draw->vram_bo.vram;
	}

	/* The object in the CPU's window; without one the readback goes word by word. */
	if (draw->vram_bo_made != 0)
		(void)nvrtx_bo_map(gpu, &draw->vram_bo);

	/* Succeeded: the buffers are clear, the VRAM target chosen. */
	return 0;
}

/*
 * Lists the device addresses of the draw's system-memory pages in their
 * order in the range (NVRTX_DRAW_PAGES of them), for the compute launch's
 * tables.
 */
void
nvrtx_draw_pages(
	const struct nvrtx_gpu *gpu,
	uint64_t *pages)
{
	const struct nvrtx_draw_state *draw;
	uint32_t page;

	draw = &gpu->draw;

	/* The push buffer and the programs. */
	for (page = 0U; page < DRAW_PUSH_PAGES; page++)
		pages[DRAW_PUSH_PAGE + page] = draw->push.dma.device_address + (uint64_t)page * NVRTX_GSP_PAGE_SIZE;
	for (page = 0U; page < DRAW_PROGRAMS; page++)
		pages[DRAW_PROGRAM_PAGE + page] = draw->programs.dma.device_address + (uint64_t)page * NVRTX_GSP_PAGE_SIZE;

	/* The single pages. */
	pages[DRAW_CONSTANTS_PAGE] = draw->constants.dma.device_address;
	pages[DRAW_VERTICES_PAGE] = draw->vertices.dma.device_address;
	pages[DRAW_TEXTURE_PAGE] = draw->texture.dma.device_address;
	pages[DRAW_HEADERS_PAGE] = draw->headers.dma.device_address;
	pages[DRAW_SAMPLERS_PAGE] = draw->samplers.dma.device_address;
	pages[DRAW_ZERO_PAGE] = draw->zero.dma.device_address;

	/* The colour target. */
	for (page = 0U; page < DRAW_TARGET_PAGES; page++)
		pages[DRAW_TARGET_PAGE + page] = draw->target.dma.device_address + (uint64_t)page * NVRTX_GSP_PAGE_SIZE;
}

/*
 * Runs the draw-test stage: the 3D object, the programs, the data, then the
 * draw into system memory, into VRAM, and textured, each checked from the CPU.
 *
 * Needs the compute launch's channel (compute-test) and its tables with the
 * draw's memory (nvrtx_draw_buffers() and nvrtx_draw_pages()).  Every draw runs
 * while the fences arrive, so one run reports each; returns the first failure,
 * or 0 when all three drew what they should.
 */
int
nvrtx_draw_start(
	struct nvrtx_gpu *gpu)
{
	uint32_t gprs[DRAW_PROGRAMS];
	int failure;
	int error;

	/* Marks the stage and says where the VRAM target is. */
	nvrtx_stage_mark("draw-test begin");
	nvrtx_stage_mark("draw-test VRAM target at %llx (%s)", (unsigned long long)gpu->draw.vram_target, (gpu->draw.vram_bo_made != 0) ? "buffer object" : "compute arena");
	failure = 0;

	/* The 3D object and the programs. */
	error = draw_object(gpu);
	if (error != 0)
		return error;

	error = draw_programs(gpu, gprs);
	if (error != 0)
		return error;

	/* What the programs read: the constants, the vertices, the texture. */
	draw_data(gpu);

	/* The draw into system memory; a missing fence ends the stage. */
	error = draw_system(gpu, gprs);
	if (error == ETIMEDOUT)
		return error;
	if (error != 0)
		failure = error;

	/* The draw into VRAM. */
	error = draw_vram(gpu);
	if (error == ETIMEDOUT)
		return error;
	if (error != 0 && failure == 0)
		failure = error;

	/* The textured draw. */
	error = draw_textured(gpu, gprs);
	if (error == ETIMEDOUT)
		return error;
	if (error != 0 && failure == 0)
		failure = error;

	/* Reports the first draw that did not draw what it should. */
	if (failure != 0)
		return failure;

	/* Succeeded: the GPU drew with the driver's programs. */
	nvrtx_stage_mark("draw-test ok: three draws checked");
	return 0;
}

/*
 * Frees the 3D object, after the channel is off the scheduler and before
 * the compute object; reports the number of frees that failed.
 */
int
nvrtx_draw_stop(
	struct nvrtx_gpu *gpu)
{
	int error;

	/* Only a made object is freed. */
	if (gpu->draw.threed_made == 0)
		return 0;

	/* The object, forgotten whatever the reply. */
	error = nvrtx_rm_free(gpu, NVRTX_HANDLE_DRAW);
	gpu->draw.threed_made = 0;
	if (error != 0)
		return 1;

	/* Succeeded: nothing failed. */
	return 0;
}

/*
 * Gives back the draw's system-memory buffers.
 *
 * Only once the GPU no longer reads them: after the GSP is stopped.
 */
void
nvrtx_draw_release(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_draw_state *draw;

	draw = &gpu->draw;

	/* Frees each buffer that was allocated. */
	nvrtx_buffer_free(gpu, &draw->push);
	nvrtx_buffer_free(gpu, &draw->programs);
	nvrtx_buffer_free(gpu, &draw->constants);
	nvrtx_buffer_free(gpu, &draw->vertices);
	nvrtx_buffer_free(gpu, &draw->texture);
	nvrtx_buffer_free(gpu, &draw->headers);
	nvrtx_buffer_free(gpu, &draw->samplers);
	nvrtx_buffer_free(gpu, &draw->zero);
	nvrtx_buffer_free(gpu, &draw->target);

	/* The VRAM target's buffer object. */
	if (draw->vram_bo_made != 0) {
		nvrtx_bo_free(gpu, &draw->vram_bo);
		draw->vram_bo_made = 0;
	}
}

/* Makes the 3D object on the compute launch's channel. */
static int
draw_object(
	struct nvrtx_gpu *gpu)
{
	int error;

	/* TURING_A, no parameters (as the golden context's). */
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_COMPUTE_CHANNEL, NVRTX_HANDLE_DRAW, gpu->identity.chip.profile->class_threed, NULL, 0U);
	if (error != 0) {
		nvrtx_stage_mark("draw-test fail: the 3D object was refused (%d)", error);
		return error;
	}

	/* Succeeded: the stop frees it. */
	gpu->draw.threed_made = 1;
	nvrtx_stage_mark("draw-test ok: 3D object on the compute channel");
	return 0;
}

/* Builds the four programs' IR and compiles each into its page. */
static int
draw_programs(
	struct nvrtx_gpu *gpu,
	uint32_t *gprs)
{
	struct nvrtx_draw_ir *program;
	int error;

	/* The IR's record, too large for the stack. */
	program = kern_malloc(sizeof(*program));
	if (program == NULL)
		return ENOMEM;

	/* The quad in one colour. */
	nvrtx_draw_vertex_ir(program, 0);
	error = draw_compile(gpu, DRAW_VERTEX, program, gprs);
	if (error == 0) {
		nvrtx_draw_fragment_ir(program, 0);
		error = draw_compile(gpu, DRAW_FRAGMENT, program, gprs);
	}

	/* The textured quad. */
	if (error == 0) {
		nvrtx_draw_vertex_ir(program, 1);
		error = draw_compile(gpu, DRAW_TEXTURED_VERTEX, program, gprs);
	}
	if (error == 0) {
		nvrtx_draw_fragment_ir(program, 1);
		error = draw_compile(gpu, DRAW_TEXTURED_FRAGMENT, program, gprs);
	}
	kern_free(program);
	if (error != 0)
		return error;

	/* Succeeded: the programs are in place. */
	return 0;
}

/*
 * Compiles one program and copies its header and code into its page: the
 * header first, the code after it, where the program's address points.
 */
static int
draw_compile(
	struct nvrtx_gpu *gpu,
	uint32_t index,
	const struct nvrtx_draw_ir *program,
	uint32_t *gprs)
{
	struct nvrtx_sm75_binary *binary;
	struct nvrtx_sm75_diagnostic diagnostic;
	uint8_t *page;
	size_t bytes;
	int error;

	/* Compiled for the generation's shader model. */
	binary = NULL;
	kern_memset(&diagnostic, 0, sizeof(diagnostic));
	error = nvrtx_sm75_compile_ir_sm(&program->ir, gpu->identity.chip.profile->sm, &binary, &diagnostic);
	if (error != 0) {
		nvrtx_stage_mark("draw-test fail: program %u did not compile (%d: %s)", (unsigned)index, error, (diagnostic.reason != NULL) ? diagnostic.reason : "?");
		return error;
	}

	/* The header and the code must fit the page. */
	bytes = (size_t)binary->code_words * 4U;
	if (binary->graphics == 0U || DRAW_HEADER_BYTES + bytes > NVRTX_GSP_PAGE_SIZE) {
		nvrtx_stage_mark("draw-test fail: program %u takes %u bytes", (unsigned)index, (unsigned)bytes);
		nvrtx_sm75_binary_free(binary);
		return EINVAL;
	}

	/* Copies them; the page is coherent memory, so the GPU sees them as written. */
	page = (uint8_t *)gpu->draw.programs.dma.address + (size_t)index * NVRTX_GSP_PAGE_SIZE;
	kern_memcpy(page, binary->sph, DRAW_HEADER_BYTES);
	kern_memcpy(page + DRAW_HEADER_BYTES, binary->code, bytes);
	gprs[index] = binary->gpr_count;
	nvrtx_stage_mark("draw-test program %u: %u instructions, %u registers", (unsigned)index, (unsigned)(binary->code_words / 4U), (unsigned)binary->gpr_count);
	nvrtx_sm75_binary_free(binary);

	/* Succeeded: the program is in place. */
	return 0;
}

/*
 * Writes what the programs read: constant buffer 0 (the quad's corners in
 * normalized coordinates, the whole texture, the quad's colour, the
 * texture's handle), the corners of the two triangles, the texture's four
 * quadrants, its header and its sampler.
 */
static void
draw_data(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_draw_state *draw;
	struct nvrtx_draw_texture texture;
	static const uint32_t corners[DRAW_VERTEX_COUNT * 2U] = {
		DRAW_FLOAT_ZERO, DRAW_FLOAT_ZERO, DRAW_FLOAT_ONE, DRAW_FLOAT_ZERO, DRAW_FLOAT_ZERO, DRAW_FLOAT_ONE,
		DRAW_FLOAT_ONE, DRAW_FLOAT_ZERO, DRAW_FLOAT_ONE, DRAW_FLOAT_ONE, DRAW_FLOAT_ZERO, DRAW_FLOAT_ONE
	};
	uint32_t *words;
	uint32_t *row;
	uint32_t index;
	uint32_t x;
	uint32_t y;

	draw = &gpu->draw;

	/* The quad from (-0.5, -0.5) to (0.5, 0.5), the texture from (0, 0) to (1, 1), yellow, handle 0. */
	words = (uint32_t *)draw->constants.dma.address;
	words[(NVRTX_SM75_CB_PUSH + NVRTX_DRAW_PUSH_RECT) / 4U] = DRAW_FLOAT_MINUS_HALF;
	words[(NVRTX_SM75_CB_PUSH + NVRTX_DRAW_PUSH_RECT) / 4U + 1U] = DRAW_FLOAT_MINUS_HALF;
	words[(NVRTX_SM75_CB_PUSH + NVRTX_DRAW_PUSH_RECT) / 4U + 2U] = DRAW_FLOAT_HALF;
	words[(NVRTX_SM75_CB_PUSH + NVRTX_DRAW_PUSH_RECT) / 4U + 3U] = DRAW_FLOAT_HALF;
	words[(NVRTX_SM75_CB_PUSH + NVRTX_DRAW_PUSH_TEXTURE) / 4U] = DRAW_FLOAT_ZERO;
	words[(NVRTX_SM75_CB_PUSH + NVRTX_DRAW_PUSH_TEXTURE) / 4U + 1U] = DRAW_FLOAT_ZERO;
	words[(NVRTX_SM75_CB_PUSH + NVRTX_DRAW_PUSH_TEXTURE) / 4U + 2U] = DRAW_FLOAT_ONE;
	words[(NVRTX_SM75_CB_PUSH + NVRTX_DRAW_PUSH_TEXTURE) / 4U + 3U] = DRAW_FLOAT_ONE;
	words[(NVRTX_SM75_CB_PUSH + NVRTX_DRAW_PUSH_COLOR) / 4U] = DRAW_FLOAT_ONE;
	words[(NVRTX_SM75_CB_PUSH + NVRTX_DRAW_PUSH_COLOR) / 4U + 1U] = DRAW_FLOAT_ONE;
	words[(NVRTX_SM75_CB_PUSH + NVRTX_DRAW_PUSH_COLOR) / 4U + 2U] = DRAW_FLOAT_ZERO;
	words[(NVRTX_SM75_CB_PUSH + NVRTX_DRAW_PUSH_COLOR) / 4U + 3U] = DRAW_FLOAT_ONE;
	words[NVRTX_SM75_CB_TEXTURES / 4U] = 0U;

	/* The corners: (0, 0), (1, 0), (0, 1) and (1, 0), (1, 1), (0, 1). */
	words = (uint32_t *)draw->vertices.dma.address;
	for (index = 0U; index < DRAW_VERTEX_COUNT * 2U; index++)
		words[index] = corners[index];

	/* The texture: red, green above blue, white. */
	for (y = 0U; y < DRAW_TEXELS; y++) {
		row = (uint32_t *)((uint8_t *)draw->texture.dma.address + (size_t)y * DRAW_TEXTURE_PITCH);
		for (x = 0U; x < DRAW_TEXELS; x++) {
			if (y < DRAW_TEXELS / 2U && x < DRAW_TEXELS / 2U) {
				row[x] = DRAW_RED;
			} else if (y < DRAW_TEXELS / 2U) {
				row[x] = DRAW_GREEN;
			} else if (x < DRAW_TEXELS / 2U) {
				row[x] = DRAW_BLUE;
			} else {
				row[x] = DRAW_WHITE;
			}
		}
	}

	/* Its header (entry 0 of the pool) and its sampler (entry 0). */
	texture.address = draw_page_va(DRAW_TEXTURE_PAGE);
	texture.width = DRAW_TEXELS;
	texture.height = DRAW_TEXELS;
	texture.pitch = DRAW_TEXTURE_PITCH;
	nvrtx_draw_texture_header((uint32_t *)draw->headers.dma.address, &texture);
	nvrtx_draw_sampler((uint32_t *)draw->samplers.dma.address);
}

/*
 * Draws into the system-memory target: the 3D state a queue starts with,
 * constant buffer 0, the texture pools, the target, the one-colour
 * programs, the vertices, the clear and the quad; then checks every pixel.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_queue.c
 * (nvk_queue_init_context_state) and src/nouveau/vulkan/nvk_cmd_draw.c
 * (nvk_push_draw_state_init).
 */
static int
draw_system(
	struct nvrtx_gpu *gpu,
	const uint32_t *gprs)
{
	struct nvrtx_draw_target target;
	static const uint32_t clear[4] = { DRAW_FLOAT_ZERO, DRAW_FLOAT_ONE, DRAW_FLOAT_ONE, DRAW_FLOAT_ONE };
	uint8_t *push;
	size_t at;
	int error;

	/* The target holds the pattern before. */
	draw_fill_system(gpu);

	/* The submission's push buffer. */
	target.address = draw_page_va(DRAW_TARGET_PAGE);
	target.width = DRAW_SIZE;
	target.height = DRAW_SIZE;
	target.pitch = DRAW_PITCH;
	push = draw_push(gpu, DRAW_SYSTEM_PUSH_PAGE);
	at = nvrtx_push_draw_init_class(push, 0U, draw_page_va(DRAW_ZERO_PAGE), gpu->identity.chip.profile->class_threed);
	at = nvrtx_push_draw_constants(push, at, draw_page_va(DRAW_CONSTANTS_PAGE), DRAW_CONSTANTS_BYTES);
	at = nvrtx_push_draw_textures(push, at, draw_page_va(DRAW_HEADERS_PAGE), 1U, draw_page_va(DRAW_SAMPLERS_PAGE), 1U);
	at = nvrtx_push_draw_target(push, at, &target);
	at = nvrtx_push_draw_invalidate(push, at);
	at = nvrtx_push_draw_pipeline(push, at, draw_page_va(DRAW_PROGRAM_PAGE + DRAW_VERTEX), gprs[DRAW_VERTEX], draw_page_va(DRAW_PROGRAM_PAGE + DRAW_FRAGMENT), gprs[DRAW_FRAGMENT]);
	at = nvrtx_push_draw_vertices(push, at, draw_page_va(DRAW_VERTICES_PAGE), DRAW_VERTEX_COUNT * DRAW_VERTEX_STRIDE, DRAW_VERTEX_STRIDE);
	at = nvrtx_push_draw_clear(push, at, clear, DRAW_SIZE, DRAW_SIZE);
	at = nvrtx_push_draw_triangles(push, at, 0U, DRAW_VERTEX_COUNT);

	/* Runs it to its fence. */
	error = draw_run(gpu, DRAW_SYSTEM_ENTRY, DRAW_SYSTEM_PUSH_PAGE, DRAW_SYSTEM_PUSH_PAGES, at, DRAW_SYSTEM_FENCE, "system memory");
	if (error != 0)
		return error;

	/* Every pixel. */
	error = draw_check_system(gpu, 0, "system memory");
	if (error != 0)
		return error;

	/* Succeeded: the quad is in system memory. */
	return 0;
}

/*
 * Draws the same into the VRAM target (the pipeline and the vertices stay
 * bound), and reads it back.
 */
static int
draw_vram(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_draw_target target;
	static const uint32_t clear[4] = { DRAW_FLOAT_ZERO, DRAW_FLOAT_ONE, DRAW_FLOAT_ONE, DRAW_FLOAT_ONE };
	uint8_t *push;
	size_t at;
	int error;

	/* The target holds the pattern before. */
	error = nvrtx_vram_fill(gpu, gpu->draw.vram_target, DRAW_PATTERN, NVRTX_DRAW_VRAM_BYTES);
	if (error != 0) {
		nvrtx_stage_mark("draw-test fail: the VRAM target could not be filled (%d)", error);
		return error;
	}

	/* The submission: the VRAM target, the clear and the quad. */
	target.address = NVRTX_DRAW_VA_VRAM;
	target.width = DRAW_SIZE;
	target.height = DRAW_SIZE;
	target.pitch = DRAW_PITCH;
	push = draw_push(gpu, DRAW_VRAM_PUSH_PAGE);
	at = nvrtx_push_draw_target(push, 0U, &target);
	at = nvrtx_push_draw_clear(push, at, clear, DRAW_SIZE, DRAW_SIZE);
	at = nvrtx_push_draw_triangles(push, at, 0U, DRAW_VERTEX_COUNT);

	/* Runs it to its fence. */
	error = draw_run(gpu, DRAW_VRAM_ENTRY, DRAW_VRAM_PUSH_PAGE, 1U, at, DRAW_VRAM_FENCE, "VRAM");
	if (error != 0)
		return error;

	/* The pixels, through the CPU's window or word by word. */
	error = draw_check_vram(gpu);
	if (error != 0)
		return error;

	/* Succeeded: the quad is in VRAM. */
	return 0;
}

/*
 * Checks the VRAM target: every pixel through the CPU's window when the
 * buffer object is mapped there, else the quad's corner pixels, the
 * target's corners and its middle through the register window.  Shows the
 * first wrong one.
 */
static int
draw_check_vram(
	struct nvrtx_gpu *gpu)
{
	static const uint16_t points[DRAW_VRAM_POINTS][2] = {
		{ 0U, 0U }, { 63U, 63U }, { 64U, 64U }, { 191U, 64U }, { 128U, 128U },
		{ 64U, 191U }, { 191U, 191U }, { 192U, 192U }, { 255U, 255U }
	};
	const volatile uint32_t *pixels;
	uint32_t value;
	uint32_t want;
	uint32_t index;
	uint32_t count;
	uint32_t right;
	int error;

	/* The window shows every pixel; the register window only the points. */
	pixels = NULL;
	count = DRAW_VRAM_POINTS;
	if (gpu->draw.vram_bo_made != 0 && gpu->draw.vram_bo.bar1_mapped != 0) {
		pixels = (const volatile uint32_t *)nvrtx_bo_cpu(gpu, &gpu->draw.vram_bo);
		count = DRAW_SIZE * DRAW_SIZE;
	}

	/* Each pixel or point against the picture. */
	right = 0U;
	for (index = 0U; index < count; index++) {
		if (pixels != NULL) {
			value = pixels[index];
			want = draw_expected(index % DRAW_SIZE, index / DRAW_SIZE, 0);
		} else {
			error = nvrtx_vram_read32(gpu, gpu->draw.vram_target + (uint64_t)points[index][1] * DRAW_PITCH + (uint64_t)points[index][0] * 4U, &value);
			if (error != 0)
				return error;
			want = draw_expected(points[index][0], points[index][1], 0);
		}
		if (value == want) {
			right++;
		} else if (right == index) {
			nvrtx_stage_mark("draw-test fail: VRAM target word %u is %08x, want %08x", (unsigned)index, (unsigned)value, (unsigned)want);
		}
	}

	/* Refuses a target that did not get the draw. */
	if (right != count) {
		nvrtx_stage_mark("draw-test fail: VRAM target: %u of %u %s right", (unsigned)right, (unsigned)count, (pixels != NULL) ? "pixels" : "points");
		return EIO;
	}

	/* Succeeded: the VRAM holds the picture. */
	nvrtx_stage_mark("draw-test ok: draw wrote the VRAM target (%u %s right)", (unsigned)count, (pixels != NULL) ? "pixels" : "points");
	return 0;
}

/*
 * Draws the textured quad into the system-memory target: the target again,
 * the textured programs, the clear and the quad; then checks every pixel.
 */
static int
draw_textured(
	struct nvrtx_gpu *gpu,
	const uint32_t *gprs)
{
	struct nvrtx_draw_target target;
	static const uint32_t clear[4] = { DRAW_FLOAT_ZERO, DRAW_FLOAT_ONE, DRAW_FLOAT_ONE, DRAW_FLOAT_ONE };
	uint8_t *push;
	size_t at;
	int error;

	/* The target holds the pattern before. */
	draw_fill_system(gpu);

	/* The submission. */
	target.address = draw_page_va(DRAW_TARGET_PAGE);
	target.width = DRAW_SIZE;
	target.height = DRAW_SIZE;
	target.pitch = DRAW_PITCH;
	push = draw_push(gpu, DRAW_TEXTURED_PUSH_PAGE);
	at = nvrtx_push_draw_target(push, 0U, &target);
	at = nvrtx_push_draw_invalidate(push, at);
	at = nvrtx_push_draw_pipeline(push, at, draw_page_va(DRAW_PROGRAM_PAGE + DRAW_TEXTURED_VERTEX), gprs[DRAW_TEXTURED_VERTEX], draw_page_va(DRAW_PROGRAM_PAGE + DRAW_TEXTURED_FRAGMENT), gprs[DRAW_TEXTURED_FRAGMENT]);
	at = nvrtx_push_draw_clear(push, at, clear, DRAW_SIZE, DRAW_SIZE);
	at = nvrtx_push_draw_triangles(push, at, 0U, DRAW_VERTEX_COUNT);

	/* Runs it to its fence. */
	error = draw_run(gpu, DRAW_TEXTURED_ENTRY, DRAW_TEXTURED_PUSH_PAGE, 1U, at, DRAW_TEXTURED_FENCE, "texture");
	if (error != 0)
		return error;

	/* Every pixel. */
	error = draw_check_system(gpu, 1, "texture");
	if (error != 0)
		return error;

	/* Succeeded: the texture is on the quad. */
	return 0;
}

/*
 * Appends the fence to a submission's push buffer, submits it on its ring
 * entry and waits for the fence.  Reports ENOSPC for a push buffer past
 * its pages, ETIMEDOUT for a fence that did not come.
 */
static int
draw_run(
	struct nvrtx_gpu *gpu,
	uint32_t entry,
	uint32_t page,
	uint32_t pages,
	size_t length,
	uint32_t fence,
	const char *what)
{
	uint8_t *push;
	uint32_t waited;
	int error;

	/* The fence after the draw, inside the submission's pages. */
	push = draw_push(gpu, page);
	length += nvrtx_push_fence(push + length, NVRTX_COMPUTE_SEMAPHORE_VA, fence);
	if (length > (size_t)pages * NVRTX_GSP_PAGE_SIZE) {
		nvrtx_stage_mark("draw-test fail: the %s push buffer takes %u bytes", what, (unsigned)length);
		return ENOSPC;
	}

	/* Submits it. */
	error = nvrtx_compute_submit(gpu, entry, draw_page_va(DRAW_PUSH_PAGE + page), length);
	if (error != 0)
		return error;

	/* Waits for the fence. */
	error = nvrtx_compute_wait(gpu, fence, &waited);
	if (error != 0) {
		nvrtx_stage_mark("draw-test fail: fence 0x%x of the %s draw did not come (semaphore %08x)", (unsigned)fence, what, (unsigned)*(const volatile uint32_t *)gpu->compute.semaphore.dma.address);
		return error;
	}

	/* Succeeded: the draw ran to its fence. */
	nvrtx_stage_mark("draw-test ok: fence 0x%x of the %s draw after %u us", (unsigned)fence, what, (unsigned)waited);
	return 0;
}

/*
 * Checks every pixel of the system-memory target against the draw's
 * picture; shows the first wrong one and the counts.
 */
static int
draw_check_system(
	struct nvrtx_gpu *gpu,
	int textured,
	const char *what)
{
	const volatile uint32_t *pixels;
	uint32_t wrong;
	uint32_t want;
	uint32_t value;
	uint32_t x;
	uint32_t y;

	/* Each pixel against what the clear and the quad leave there. */
	pixels = (const volatile uint32_t *)gpu->draw.target.dma.address;
	wrong = 0U;
	for (y = 0U; y < DRAW_SIZE; y++) {
		for (x = 0U; x < DRAW_SIZE; x++) {
			value = pixels[y * DRAW_SIZE + x];
			want = draw_expected(x, y, textured);
			if (value == want)
				continue;
			if (wrong == 0U)
				nvrtx_stage_mark("draw-test fail: %s target (%u, %u) is %08x, want %08x", what, (unsigned)x, (unsigned)y, (unsigned)value, (unsigned)want);
			wrong++;
		}
	}

	/* Refuses a target with any wrong pixel. */
	if (wrong != 0U) {
		nvrtx_stage_mark("draw-test fail: %s target: %u of %u pixels wrong, middle %08x", what, (unsigned)wrong, (unsigned)(DRAW_SIZE * DRAW_SIZE), (unsigned)pixels[(DRAW_SIZE / 2U) * DRAW_SIZE + DRAW_SIZE / 2U]);
		return EIO;
	}

	/* Succeeded: the target is the picture. */
	nvrtx_stage_mark("draw-test ok: draw wrote the %s target (%u pixels right)", what, (unsigned)(DRAW_SIZE * DRAW_SIZE));
	return 0;
}

/*
 * Reports the word a pixel of the target should hold: the clear outside
 * the quad; inside it the quad's colour, or the texel the pixel's centre
 * samples (the quad's 128 pixels take the texture's 4 texels, 32 each).
 */
static uint32_t
draw_expected(
	uint32_t x,
	uint32_t y,
	int textured)
{
	uint32_t texel_x;
	uint32_t texel_y;

	/* Outside the quad: the clear. */
	if (x < DRAW_QUAD_LOW ||
	    x >= DRAW_QUAD_HIGH ||
	    y < DRAW_QUAD_LOW ||
	    y >= DRAW_QUAD_HIGH)
		return DRAW_CLEAR_WORD;

	/* One colour. */
	if (textured == 0)
		return DRAW_QUAD_WORD;

	/* The texel, by its quadrant of the texture. */
	texel_x = (x - DRAW_QUAD_LOW) * DRAW_TEXELS / (DRAW_QUAD_HIGH - DRAW_QUAD_LOW);
	texel_y = (y - DRAW_QUAD_LOW) * DRAW_TEXELS / (DRAW_QUAD_HIGH - DRAW_QUAD_LOW);
	if (texel_y < DRAW_TEXELS / 2U && texel_x < DRAW_TEXELS / 2U)
		return DRAW_RED;
	if (texel_y < DRAW_TEXELS / 2U)
		return DRAW_GREEN;
	if (texel_x < DRAW_TEXELS / 2U)
		return DRAW_BLUE;

	/* The lower right quadrant. */
	return DRAW_WHITE;
}

/* Fills the system-memory target with the pattern, so a pixel the draw did not write shows. */
static void
draw_fill_system(
	struct nvrtx_gpu *gpu)
{
	uint32_t *pixels;
	uint32_t index;

	/* Each pixel. */
	pixels = (uint32_t *)gpu->draw.target.dma.address;
	for (index = 0U; index < DRAW_SIZE * DRAW_SIZE; index++)
		pixels[index] = DRAW_PATTERN;
}

/* Reports the address of one of the draw's system-memory pages in the range. */
static uint64_t
draw_page_va(
	uint32_t page)
{
	/* The pages follow each other from the draw's start. */
	return NVRTX_DRAW_VA_PAGES + (uint64_t)page * NVRTX_GSP_PAGE_SIZE;
}

/* Reports where a submission's part of the push buffer starts in the CPU's view. */
static uint8_t *
draw_push(
	struct nvrtx_gpu *gpu,
	uint32_t page)
{
	/* The pages follow each other in the buffer. */
	return (uint8_t *)gpu->draw.push.dma.address + (size_t)page * NVRTX_GSP_PAGE_SIZE;
}
