/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Runs one render job of a render pass instance.
 *
 * A job without draws runs an empty geometry stream; its fragment clears
 * or loads every tile of the render area through the background object
 * and stores it through the end-of-tile program.  The programs, the
 * shared words and the stream are written into memory of the job's own,
 * mapped in the session's windows, and returned once the render is done.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/render-job.h"
#include "drivers/gpu/powervr/interface.h"
#include "drivers/gpu/powervr/compiler/pds-encode.h"
#include "drivers/gpu/powervr/render-command.h"
#include "drivers/gpu/powervr/render-kick-layout.h"
#include "drivers/gpu/powervr/render-load-program.h"
#include "drivers/gpu/powervr/transfer-program.h"
#include "drivers/gpu/powervr/transfer-job.h"
#include "drivers/gpu/powervr/render-program.h"
#include "drivers/gpu/powervr/data-program.h"
#include "drivers/gpu/powervr/image-state.h"
#include "drivers/gpu/powervr/render-draw.h"
#include "drivers/gpu/powervr/render-depth.h"
#include "drivers/gpu/powervr/vk/vk-draw.h"
#include "drivers/gpu/powervr/vk/vk.h"
#include "drivers/gpu/powervr/descriptor-set.h"
#include "drivers/gpu/powervr/compiler/shader.h"
#include "drivers/gpu/powervr/load-draw-shader.h"
#include "drivers/gpu/powervr/clear-draw-shader.h"

/* How long a render may take. */
#define JOB_WAIT_MS			2000U

/*
 * The bytes of one tile buffer of the BXE-4-32: the partition space's
 * words (16x16 tiles, ten partitions, the core's least two output
 * registers a pixel) for its one cluster, as words of four bytes, for
 * each of the cores a multicore core may have.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_device.c
 * (pvr_get_tile_buffer_size_per_core: the partition space times the
 * clusters, rounded up to a power of two without tile_per_usc, times
 * four bytes; pvr_get_tile_buffer_size: times rogue_get_max_num_cores),
 * src/imagination/vulkan/winsys/pvrsrvkm/pvr_srv.c
 * (pvr_srv_get_total_reserved_partition_size: tile_size_x * tile_size_y *
 * max_partitions * usc_min_output_registers_per_pix words for 16x16
 * tiles), src/imagination/include/hwdef/rogue_hw_utils.h
 * (rogue_get_max_num_cores: xpu_max_slaves + 1 with
 * gpu_multicore_support) and src/imagination/common/device_info/
 * bxe-4-32.h (tile_size_x 16, tile_size_y 16, max_partitions 10,
 * usc_min_output_registers_per_pix 2, num_clusters 1, xpu_max_slaves 3,
 * has_gpu_multicore_support).
 */
#define TILE_BUFFER_BYTES		(16U * 16U * 10U * 2U * 1U * 4U * (3U + 1U))

/*
 * The job's memory: in the general window the stream (the word that
 * ends it) and the load's shared words; in the data sequencer's window
 * the end-of-tile data, the background's task program (data, then code
 * after eight words) and its shared register program's code and data,
 * then the same of a partial render (its end of tile and the background
 * of the 3D after it, job_partial_programs); in the shading cluster's
 * the end-of-tile and background programs, then the partial render's.
 */
#define GENERAL_STREAM			0x0U
#define GENERAL_LOAD_WORDS		0xa180U
#define GENERAL_DBIAS			0xa200U
#define GENERAL_SCISSORS		0xa300U
#define GENERAL_TERMINATE_PPP		0xb400U
#define GENERAL_VISIBILITY		0xb800U
#define GENERAL_PARTIAL_WORDS		0xbc00U
#define GENERAL_BYTES			0xc000U

/*
 * The visibility registers at GENERAL_VISIBILITY: a 32-bit count each,
 * on the 16-byte boundary the occlusion query base takes.
 *
 * Values referenced: Mesa 26.2.6 src/imagination/csbgen/rogue/cr.xml
 * (ISP_OCLQRY_BASE addr 4..39) and src/imagination/vulkan/pvr_query.c
 * (an occlusion pool's result buffer: a uint32_t count a query).
 */
_Static_assert((GENERAL_VISIBILITY & 0xfU) == 0U, "the visibility registers' boundary");
_Static_assert(GENERAL_VISIBILITY + POWERVR_RENDER_VISIBILITY_MAX * 4U <= GENERAL_PARTIAL_WORDS, "the visibility registers' room");
_Static_assert(GENERAL_PARTIAL_WORDS + POWERVR_RENDER_PARTIAL_WORDS_MAX * 4U <= GENERAL_BYTES, "the partial render load's shared words' room");

/*
 * The depth bias table's entries between GENERAL_DBIAS and the scissors:
 * three floats each (the constant factor, the slope factor, the clamp).
 *
 * Values referenced: Mesa 26.2.5 src/imagination/vulkan/pvr_cmd_buffer.h
 * (struct pvr_depth_bias_state: constant_factor, slope_factor, clamp).
 */
#define DBIAS_ENTRY_BYTES		12U
#define DBIAS_ENTRIES			((GENERAL_SCISSORS - GENERAL_DBIAS) / DBIAS_ENTRY_BYTES)

/*
 * The edge of the tiles the depth load and store writes whole: a depth
 * target whose physical edges are smaller is rendered from a buffer of
 * edges rounded up to it (job_depth_scratch).
 *
 * Values referenced: Mesa 26.2.6 src/imagination/common/
 * rogue_hw_utils.h (rogue_get_zls_tile_size_xy: 32 x 32 for a core of
 * 16 x 16 tiles without zls_subtile) and
 * src/imagination/common/device_info/bxe-4-32.h (tile_size_x 16, no
 * zls_subtile).
 */
#define ZLS_TILE_PIXELS			32U

/* The most pixels of a job the timing counts as small (256 x 256). */
#define JOB_SMALL_PIXELS		65536U

/*
 * Each draw's memory: in its slot of the draws' general buffer the
 * primitive state block, the push constants, the blend constants, then
 * its descriptor sets' copies after every slot; in its slot of the
 * data sequencer buffer the vertex program's data and code, the vertex
 * and fragment shared register programs' code and data, the fragment's
 * task program and the coefficient program (data, then code); its
 * shaders' code one after another in the shading cluster buffer.
 */
#define DRAW_SLOT			0x1000U

/* The least bytes a job's draw buffer is made with (job_draw_memory_room). */
#define DRAW_MEMORY_LEAST		0x10000U
#define DRAW_PPP			0x000U
#define DRAW_PUSH			0x100U
#define DRAW_BLEND			0x200U
#define DRAW_DEFAULTS			0x300U
#define ATTRIBUTE_NO_DEFAULTS		0xffffffffU
#define DRAW_PDS_VERTEX_DATA		0x000U
#define DRAW_PDS_VERTEX_CODE		0x400U
#define DRAW_PDS_VSHARED_CODE		0x500U
#define DRAW_PDS_VSHARED_DATA		0x600U
#define DRAW_PDS_KICK			0x800U
#define DRAW_PDS_FSHARED_CODE		0x880U
#define DRAW_PDS_FSHARED_DATA		0x980U
#define DRAW_PDS_COEFFICIENT		0xb80U
#define DRAW_STREAM_ROOM		(GENERAL_LOAD_WORDS - GENERAL_STREAM)

_Static_assert(DRAW_STREAM_ROOM >= ((POWERVR_RENDER_DRAWS + POWERVR_PASS_COLORS) * POWERVR_DRAW_VDM_WORDS + POWERVR_RENDER_TERMINATE_STREAM_WORDS) * 4U, "the stream's room, a load draw an attachment included");
_Static_assert(POWERVR_DRAW_PPP_WORDS * 4U <= DRAW_PUSH, "the primitive state block's room");
_Static_assert(POWERVR_DRAW_PUSH_BYTES <= DRAW_BLEND - DRAW_PUSH, "the push constants' room");
_Static_assert(POWERVR_DATA_VERTEX_DATA_MAX * 4U <= DRAW_PDS_VERTEX_CODE, "the vertex program's data room");
_Static_assert(POWERVR_DATA_VERTEX_CODE_MAX * 4U <= DRAW_PDS_VSHARED_CODE - DRAW_PDS_VERTEX_CODE, "the vertex program's code room");
_Static_assert(POWERVR_DATA_SHARED_CODE_MAX * 4U <= DRAW_PDS_VSHARED_DATA - DRAW_PDS_VSHARED_CODE, "shared code room");
_Static_assert(POWERVR_DATA_SHARED_DATA_MAX * 4U <= DRAW_PDS_KICK - DRAW_PDS_VSHARED_DATA, "shared data room");
_Static_assert(POWERVR_DATA_SHARED_CODE_MAX * 4U <= DRAW_PDS_FSHARED_DATA - DRAW_PDS_FSHARED_CODE, "fragment shared code room");
_Static_assert(POWERVR_DATA_SHARED_DATA_MAX * 4U <= DRAW_PDS_COEFFICIENT - DRAW_PDS_FSHARED_DATA, "fragment shared data room");
_Static_assert((POWERVR_DRAW_COEFFICIENT_DATA + 3U) / 4U * 16U + POWERVR_DRAW_COEFFICIENT_CODE * 4U <= DRAW_SLOT - DRAW_PDS_COEFFICIENT, "coefficient room");
_Static_assert((POWERVR_RENDER_DRAWS + POWERVR_PASS_COLORS) * POWERVR_DRAW_SCISSOR_WORDS * 4U <= GENERAL_TERMINATE_PPP - GENERAL_SCISSORS, "scissor table room, a load draw an attachment included");
_Static_assert(POWERVR_PASS_COLORS <= POWERVR_RENDER_EMITS_MAX && POWERVR_PASS_COLORS <= POWERVR_FRAGMENT_EMITS, "an emit for each colour attachment");
#define PDS_EVENT			0x000U
#define PDS_KICK			0x040U
#define PDS_SHARED_CODE			0x080U
#define PDS_SHARED_DATA			0x200U
#define PDS_PARTIAL_EVENT		0x1000U
#define PDS_PARTIAL_KICK		0x1040U
#define PDS_PARTIAL_SHARED_CODE		0x1080U
#define PDS_PARTIAL_SHARED_DATA		0x1200U
#define PDS_BYTES			0x2000U
#define PDS_SHARED_CODE_ROOM		(PDS_SHARED_DATA - PDS_SHARED_CODE)
#define PDS_SHARED_DATA_ROOM		(PDS_PARTIAL_EVENT - PDS_SHARED_DATA)
#define PDS_PARTIAL_SHARED_CODE_ROOM	(PDS_PARTIAL_SHARED_DATA - PDS_PARTIAL_SHARED_CODE)
#define PDS_PARTIAL_SHARED_DATA_ROOM	(PDS_BYTES - PDS_PARTIAL_SHARED_DATA)
#define USC_EOT				0x000U
#define USC_BACKGROUND			0x400U
#define USC_PARTIAL_EOT			0x800U
#define USC_PARTIAL_BACKGROUND		0xc00U
#define USC_BYTES			0x1000U

_Static_assert(POWERVR_RENDER_USC_MAX <= USC_BACKGROUND - USC_EOT && POWERVR_RENDER_USC_MAX <= USC_PARTIAL_EOT - USC_BACKGROUND, "the end of tile's and the background's rooms");
_Static_assert(POWERVR_RENDER_USC_MAX <= USC_PARTIAL_BACKGROUND - USC_PARTIAL_EOT && POWERVR_RENDER_USC_MAX <= USC_BYTES - USC_PARTIAL_BACKGROUND, "the partial render's rooms");
_Static_assert(POWERVR_RENDER_EVENT_DATA_WORDS * 4U <= PDS_PARTIAL_KICK - PDS_PARTIAL_EVENT && (POWERVR_RENDER_KICK_DATA_WORDS + POWERVR_RENDER_KICK_CODE_WORDS) * 4U <= PDS_PARTIAL_SHARED_CODE - PDS_PARTIAL_KICK, "the partial render's task rooms");

/*
 * The load operations of a colour attachment the job reads.
 *
 * Values referenced:
 * Khronos Vulkan 1.x vulkan_core.h (VK_ATTACHMENT_LOAD_OP_LOAD 0,
 * VK_ATTACHMENT_LOAD_OP_CLEAR 1).
 */
#define LOAD_OP_LOAD			0U
#define LOAD_OP_CLEAR			1U
#define LOAD_OP_DONT_CARE		2U

/*
 * A colour attachment that is LOADed is drawn over by a full-screen
 * draw that samples what the job before stored (with the probe 0, the
 * default), its background left as contents that do not matter; on the
 * board a background object that samples from its shared registers read
 * 0 whatever its program (2026-10-21), while a draw's sampling reads the
 * image.  POWERVR_RENDER_LOAD_PROBE other than 0 keeps the background
 * forms the board was probed with.
 *
 * Values referenced:
 * Khronos Vulkan 1.x vulkan_core.h (VK_ATTACHMENT_LOAD_OP_DONT_CARE 2,
 * VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST 3, VK_FORMAT_R32G32_SFLOAT 103).
 */
#define LOAD_DRAW_TOPOLOGY		3U
#define LOAD_DRAW_POSITION_FORMAT	103U
#define LOAD_DRAW_VERTICES		6U
#define LOAD_DRAW_STRIDE		16U
#define LOAD_DRAW_PIPELINES		16U
#define LOAD_DRAW_CODE_BYTES		0x2000U

/*
 * The clear draw's pipelines a render node keeps (one for each shape of
 * attachments and mask met), and the depth comparison it draws with.
 *
 * Values referenced:
 * Khronos Vulkan 1.x vulkan_core.h (VK_COMPARE_OP_ALWAYS 7).
 */
#define CLEAR_DRAW_PIPELINES		16U
#define CLEAR_DRAW_COMPARE_ALWAYS	7U

/*
 * What a clear draw's pipeline is made for: the job's colour attachments'
 * formats, whether it has a depth target, and the clear's mask.
 */
struct clear_key {
	uint32_t formats[POWERVR_PASS_COLORS];
	uint32_t color_count;
	bool has_depth;
	uint32_t mask;
};

/*
 * What a load draw's pipeline is made for: the job's colour attachments'
 * formats (which place each one's pixel output registers) and the
 * location of the one it loads.
 */
struct load_key {
	uint32_t formats[POWERVR_PASS_COLORS];
	uint32_t location;
	uint32_t samples;
};

/*
 * The load draws' compiled shaders, one pipeline per load_key, and the
 * clear draw's, one per clear_key; kept by the render node.
 */
struct powervr_load_draws {
	struct load_key keys[LOAD_DRAW_PIPELINES];
	struct powervr_vk_graphics *graphics[LOAD_DRAW_PIPELINES];
	struct clear_key clear_keys[CLEAR_DRAW_PIPELINES];
	struct powervr_vk_graphics *clear_graphics[CLEAR_DRAW_PIPELINES];
};

/* One job's load draws, one a loaded colour attachment: the draws, their sets and the sets' bytes. */
struct load_draw {
	struct powervr_render_draw draws[POWERVR_PASS_COLORS];
	struct powervr_vk_descriptor_set sets[POWERVR_PASS_COLORS];
	uint32_t count;
};

/*
 * The places of a job's colour attachments: each one's first pixel
 * output register (POWERVR_SHADER_NO_REG for an unused one), or, for one
 * in a tile buffer (tile_buffer not POWERVR_SHADER_NO_REG), its first
 * word among a pixel's words of that buffer; the registers a pixel takes
 * (as far as the furthest word of either), the tile buffers taken, and
 * the attachments used.
 */
struct job_outputs {
	uint32_t regs[POWERVR_PASS_COLORS];
	uint32_t tile_buffer[POWERVR_PASS_COLORS];
	uint32_t registers;
	uint32_t tile_buffers;
	uint32_t used;
};

/*
 * A render job whose 3D may still run: its memory, the copy of the job
 * the load draw took and the load draw, when it was kicked (for the
 * timing) and whether it was small; one of the session's list, oldest
 * first (powervr_render_jobs_reap).
 */
struct powervr_job_done {
	struct powervr_job_done *next;
	struct job_memory *memory;
	struct powervr_render_job *work;
	struct load_draw *load;
	uint64_t kicked;
	int small;

	/*
	 * The submission timing's marks: the 3D value the geometry waited
	 * for (0 none), and when the TA and the 3D were seen done (0 not yet).
	 */
	uint32_t waited_fragment;
	uint64_t ta_seen;
	uint64_t fragment_seen;

	/* The job's visibility registers' report, when its draws count for queries. */
	struct powervr_render_visibility *visibility;
};

/* The job's memory and programs while it runs. */
struct job_memory {
	struct powervr_resource *general;
	struct powervr_buffer *pds;
	struct powervr_buffer *usc;
	uint64_t pds_place;
	uint64_t usc_place;
	struct powervr_render_usc program;
	struct powervr_render_kick kick;
	struct powervr_data_shared_program shared;
	struct powervr_fragment_setup fragment_setup;
	struct powervr_geometry_setup geometry_setup;
	uint8_t geometry[POWERVR_GEOMETRY_BYTES];
	uint8_t fragment[POWERVR_FRAGMENT_BYTES];
	struct powervr_render_marks marks;

	/*
	 * The partial render's: its fragment command, the scratch buffer's
	 * address (0 for a job without colour outputs, whose partial render
	 * runs the fragment's command), the plan of its regions, its end of
	 * tile's emit words and data and size words (job_partial_programs).
	 */
	uint8_t partial[POWERVR_FRAGMENT_BYTES];
	uint64_t partial_scratch;
	struct powervr_render_partial partial_plan;
	uint64_t partial_pbe[POWERVR_FRAGMENT_EMITS][3];
	uint32_t partial_event_data;
	uint32_t partial_event_info;
	struct powervr_resource *draw_general;
	struct powervr_buffer *draw_pds;
	struct powervr_buffer *draw_usc;
	uint64_t draw_pds_place;
	uint64_t draw_usc_place;
	struct powervr_data_load loads[POWERVR_DATA_LOADS_MAX];
	struct powervr_data_vertex_dma dmas[POWERVR_DATA_VERTEX_DMAS_MAX];
	struct powervr_data_vertex_program vertex;
	uint32_t ppp[POWERVR_DRAW_PPP_WORDS];
	uint32_t vdm[POWERVR_DRAW_VDM_WORDS];
	/*
	 * The shaders' code in the draws' shading cluster buffer, once a
	 * pipeline (a desktop's frame draws a few pipelines many times, and a
	 * copy of the code for each draw cost that many copies): the
	 * pipelines met, where each one's code is, and where each draw's is.
	 */
	const struct powervr_vk_graphics *code_pipelines[POWERVR_RENDER_DRAWS + POWERVR_PASS_COLORS];
	uint64_t code_pipeline_places[POWERVR_RENDER_DRAWS + POWERVR_PASS_COLORS];
	uint32_t code_pipeline_count;
	uint64_t code_places[POWERVR_RENDER_DRAWS + POWERVR_PASS_COLORS];

	/* The pipeline each draw draws with: its own, or a clear draw's (job_draws). */
	const struct powervr_vk_graphics *graphics[POWERVR_RENDER_DRAWS + POWERVR_PASS_COLORS];

	/*
	 * The job's depth bias table as written at GENERAL_DBIAS: each entry's
	 * constant factor (scaled for the depth format), slope factor and
	 * clamp, one entry for each different bias the draws use.
	 */
	uint32_t dbias[DBIAS_ENTRIES][3];
	uint32_t dbias_count;

	/* The device addresses of the job's tile buffers (0 for one it does not take), for the draws, the background and the end of tile. */
	uint64_t tile_buffer_addresses[POWERVR_SHADER_TILE_BUFFERS];
};

/*
 * The two triangles over the framebuffer the load and clear draws draw:
 * x, y in device coordinates and u, v, as float bits (-1.0, 0, 1.0).
 * Constant; the job's general memory gets a copy at GENERAL_LOAD_WORDS.
 */
static const uint32_t load_draw_vertices[LOAD_DRAW_VERTICES * 4U] = {
	0xbf800000U, 0xbf800000U, 0x00000000U, 0x00000000U,
	0x3f800000U, 0xbf800000U, 0x3f800000U, 0x00000000U,
	0xbf800000U, 0x3f800000U, 0x00000000U, 0x3f800000U,
	0x3f800000U, 0xbf800000U, 0x3f800000U, 0x00000000U,
	0x3f800000U, 0x3f800000U, 0x3f800000U, 0x3f800000U,
	0xbf800000U, 0x3f800000U, 0x00000000U, 0x3f800000U,
};

static void job_visibility_report(struct powervr_job_done *done, int lost);
static void zero_words(struct powervr_buffer *buffer, uint32_t offset, unsigned count);
static int job_check(const struct powervr_render_job *job, struct job_outputs *outputs);
static int job_depth_scratch(struct powervr_render *render, struct powervr_session *session, const struct powervr_render_job *job, struct powervr_render_depth *depth, bool *copy_back);
static int depth_scratch_copy(struct powervr_render *render, struct powervr_session *session, const struct powervr_render_depth *image, const struct powervr_render_depth *scratch, bool into_scratch);
static int job_outputs(const struct powervr_render_job *job, struct job_outputs *outputs);
static int job_tile_buffers(struct powervr_render *render, struct powervr_session *session, const struct job_outputs *outputs, struct job_memory *memory);
static int job_emits(const struct powervr_render_job *job, const struct job_outputs *outputs, uint32_t states[POWERVR_RENDER_EMITS_MAX][2], uint32_t *count, uint64_t reg[POWERVR_FRAGMENT_EMITS][3], uint32_t tiles[POWERVR_RENDER_EMITS_MAX]);
static void load_draw_free(struct powervr_render_job *work, struct load_draw *load);
static bool job_skips_empty_tiles(const struct powervr_render_job *job);
static bool job_geometry_meets_images(const struct powervr_render_job *job);
static bool job_needs_no_background(const struct powervr_render_job *job, uint32_t load_mask);
static uint32_t float_bits(uint32_t value);
static int load_graphics(struct powervr_render *render, const struct powervr_render_job *job, uint32_t location, struct powervr_vk_graphics **result);
static int load_graphics_compile(struct powervr_vk_graphics *graphics, uint32_t location, uint32_t vk_format);
static int load_graphics_shaders(struct powervr_vk_graphics *graphics, uint32_t location, uint32_t vk_format, struct powervr_shader_workspace *workspace);
static int load_draw_build(struct powervr_render *render, const struct powervr_render_job *job, uint32_t load_mask, struct job_memory *memory, struct load_draw *load);
static int load_draw_one(struct powervr_render *render, const struct powervr_render_job *job, uint32_t location, struct job_memory *memory, struct powervr_render_draw *draw, struct powervr_vk_descriptor_set *set);
static bool format_is_signed_integer(uint32_t vk_format);
static int clear_graphics(struct powervr_render *render, const struct powervr_render_job *job, uint32_t mask, const struct powervr_vk_graphics **result);
static int clear_graphics_compile(struct powervr_vk_graphics *graphics, uint32_t outputs);
static int job_map(struct powervr_render *render, struct powervr_session *session, struct job_memory *memory);
static int job_programs(const struct powervr_render_job *job, const struct job_outputs *outputs, uint32_t load_mask, struct job_memory *memory, uint32_t *shared_registers);
static int job_partial_scratch(struct powervr_render *render, struct powervr_session *session, const struct powervr_render_job *job, const struct job_outputs *outputs, struct job_memory *memory);
static int job_partial_programs(const struct powervr_render_job *job, struct job_memory *memory, uint32_t pds_base, uint32_t usc_base, uint32_t *shared_registers);
static void job_partial_command(struct job_memory *memory);
/*
 * The LOAD background's probe (for the board): 0 the usual load program,
 * 1 the sample at (0, 0) (POWERVR_LOAD_PROBE_ORIGIN), 2 the program with a
 * wait group after each group (POWERVR_LOAD_PROBE_SPACED), 3 no sample at
 * all: the background a constant magenta as a clear's, 4 the usual
 * program with its eight shared words written by the data sequencer as
 * constants (DOUTW) instead of copied from memory (DOUTD), 5 the
 * transfer's blit program and shared words (one to one over the
 * attachment) as the background.
 */
#ifndef POWERVR_RENDER_LOAD_PROBE
#define POWERVR_RENDER_LOAD_PROBE	0
#endif
#define LOAD_PROBE_CONSTANT		3
#define LOAD_PROBE_WRITTEN		4
#define LOAD_PROBE_BLIT			5

/* The load program's form: the probe's own up to SPACED, the usual one for the later probes. */
#if POWERVR_RENDER_LOAD_PROBE <= POWERVR_LOAD_PROBE_SPACED
#define LOAD_USC_PROBE_FORM		POWERVR_RENDER_LOAD_PROBE
#else
#define LOAD_USC_PROBE_FORM		POWERVR_LOAD_PROBE_USUAL
#endif

/* Reads a little-endian 32-bit word. */
static uint32_t
get_le32(
	const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static int job_background(const struct powervr_render_job *job, const struct job_outputs *outputs, uint32_t load_mask, struct job_memory *memory, uint32_t pds_base, uint32_t usc_base, uint32_t *shared_registers);
static int job_background_tiles(const struct powervr_render_job *job, const struct job_outputs *outputs, const uint32_t *value, uint32_t count, struct job_memory *memory, uint32_t *shared_registers);
static int job_draws(struct powervr_render *render, struct powervr_session *session, const struct powervr_render_job *job, const struct powervr_render_draw *prefix, uint32_t prefix_count, struct job_memory *memory, uint32_t *stream_words, uint32_t *vertex_shareds);
static int draw_place(const struct powervr_render_job *job, const struct powervr_render_draw *draw, uint32_t index, struct job_memory *memory, uint64_t sets_place, uint64_t usc_place, uint32_t stream_at, uint32_t *words);
static int draw_set_offsets(const struct powervr_render_draw *draw, const struct powervr_vk_descriptor_set *set, struct job_memory *memory, uint32_t at, uint32_t *taken);
static int draw_depth_bias(const struct powervr_render_job *job, const struct powervr_vk_graphics *graphics, const struct powervr_render_draw *draw, struct job_memory *memory, uint32_t *index);
static bool format_has_stencil(uint32_t vk_format);
static uint32_t float_scaled_down(uint32_t bits, uint32_t power);
static const struct powervr_render_draw *job_draw(const struct powervr_render_job *job, const struct powervr_render_draw *prefix, uint32_t prefix_count, uint32_t index);
static int draw_shared(const struct powervr_shader_program *program, uint32_t usc, const struct powervr_data_state *state, struct job_memory *memory, uint32_t code_at, uint32_t data_at, uint32_t *data_words);
static void job_release(struct powervr_render *render, struct powervr_session *session, struct job_memory *memory);
static void job_free(struct powervr_render *render, struct powervr_session *session, struct job_memory *memory);
static struct job_memory *job_keep_take(struct powervr_render *render, struct powervr_session *session);
static void job_buffer_clear(struct powervr_buffer *buffer);
static bool job_draw_memory_fits(const struct job_memory *memory, uint64_t general_bytes, uint64_t pds_bytes, uint64_t usc_bytes);
static bool job_draw_memory_swap(struct powervr_render *render, struct powervr_session *session, struct job_memory *memory, uint64_t general_bytes, uint64_t pds_bytes, uint64_t usc_bytes);
static void job_draw_memory_free(struct powervr_render *render, struct powervr_session *session, struct job_memory *memory);
static uint64_t job_draw_memory_room(uint64_t bytes);
static int job_draw_memory_make(struct powervr_render *render, struct powervr_session *session, struct job_memory *memory, uint64_t general_bytes, uint64_t pds_bytes, uint64_t usc_bytes);
static int attribute_words(uint32_t format, uint32_t components, uint32_t *words, uint32_t *defaults);
static void put_words(struct powervr_buffer *buffer, uint32_t offset, const uint32_t *words, unsigned count);
static void put_bytes(struct powervr_buffer *buffer, uint32_t offset, const uint8_t *bytes, size_t count);
static void job_timing(struct powervr_session *session, struct powervr_job_done *done);

/* The components a vertex format lacks: (0, 0, 0, 1.0) for a float format, then (0, 0, 0, 1) for an integer one, little-endian. */
static const uint8_t attribute_defaults[32] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0x80, 0x3f,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0x00, 0x00,
};

/*
 * Runs a render job without draws.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_sub_cmd_gfx_job_init: an empty geometry stream for a render pass
 * instance without draws, the load operations as the background, the
 * store through the pixel event).
 */
int
powervr_render_job_run(
	struct powervr_render *render,
	struct powervr_session *session,
	const struct powervr_render_job *job)
{
	struct powervr_render_context *context;
	struct powervr_target_shape shape;
	struct powervr_target *target;
	const struct powervr_target_layout *layout;
	struct job_memory *memory;
	struct powervr_render_zls zls;
	uint32_t shared_registers;
	uint32_t vertex_shareds;
	uint32_t stream_words;
	uint32_t terminate[POWERVR_RENDER_TERMINATE_STREAM_WORDS];
	uint32_t terminate_state[POWERVR_RENDER_TERMINATE_STATE_WORDS];
	const struct powervr_render_job *given;
	struct powervr_render_job *work;
	const struct powervr_render_draw *prefix;
	uint32_t prefix_count;
	struct load_draw *load;
	struct powervr_job_done *done;
	struct powervr_sync previous[2];
	struct powervr_sync transfer_wait;
	unsigned wait_count;
	unsigned fragment_wait_count;
	unsigned half;
	uint32_t half_fragment;
	unsigned long enabled;
	uint64_t draws_started;
	uint64_t started;
	uint32_t at;
	struct job_outputs outputs;
	struct powervr_render_depth depth;
	bool depth_copy_back;
	uint32_t load_mask;
	uint32_t index;
	int error;

	/* When the job started, for the submission's timing. */
	started = powervr_timing_now();

	/* Refuses what the job runner does not build yet; the attachments' pixel output registers. */
	error = job_check(job, &outputs);
	if (error != 0)
		return error;

	/* The depth target the ZLS loads and stores: the attachment, or the scratch a small one is copied into first. */
	error = job_depth_scratch(render, session, job, &depth, &depth_copy_back);
	if (error != 0)
		return error;

	/*
	 * Each loaded colour attachment: the job runs with its background's
	 * contents not mattering there and a load draw of it first (the probe
	 * 0).  The attachments are taken as not loaded where the background
	 * is made (load_mask), without a copy of the job (a job of 512 draws'
	 * room is about 144 KB).
	 */
	given = job;
	work = NULL;
	load = NULL;
	load_mask = 0;
	for (index = 0; index < job->color_count && POWERVR_RENDER_LOAD_PROBE == 0; index++) {
		if ((outputs.used & (1U << index)) != 0 && job->colors[index].load_op == LOAD_OP_LOAD)
			load_mask |= 1U << index;
	}
	if (load_mask != 0) {
		load = kern_calloc(1, sizeof(*load));
		if (load == NULL)
			return ENOMEM;
	}

	/* The job's shape and its first colour target's load and store, with the diagnostics. */
	if (POWERVR_DIAGNOSTICS && job->color_count != 0) {
		kern_logf("powervr: render job: %ux%u, %u draws, colour load %u store %u at 0x%llx (%s, pitch %u, padded %u)\n",
			  (unsigned)job->width, (unsigned)job->height, (unsigned)job->draw_count,
			  (unsigned)job->colors[0].load_op, (unsigned)job->colors[0].store_op,
			  (unsigned long long)job->colors[0].surface.address, job->colors[0].surface.linear ? "linear" : "twiddled",
			  (unsigned)job->colors[0].surface.row_pitch, (unsigned)job->colors[0].surface.padded_width);
	}

	/* The session's render context and a target of the frame. */
	error = powervr_session_render_context(render, session, &context);
	if (error == 0) {
		shape.width = job->width;
		shape.height = job->height;
		shape.samples = job->samples == 4U ? 4U : 1U;
		shape.layers = 1;
		error = powervr_session_target(render, session, &shape, &target);
	}

	/* The job's memory and its programs: kept mapped from a finished job, or new. */
	memory = NULL;
	if (error == 0) {
		memory = job_keep_take(render, session);
		if (memory == NULL)
			memory = kern_calloc(1, sizeof(*memory));
		if (memory == NULL)
			error = ENOMEM;
	}

	if (error != 0) {
		load_draw_free(work, load);
		return error;
	}

	/* The job's memory, timed on its own. */
	powervr_timing_add(POWERVR_TIMING_JOB_SETUP, started);
	started = powervr_timing_now();
	error = job_map(render, session, memory);
	powervr_timing_add(POWERVR_TIMING_JOB_MAP, started);

	/* The tile buffers of attachments the pixel output registers have no room for. */
	if (error == 0)
		error = job_tile_buffers(render, session, &outputs, memory);

	/* The scratch buffer a partial render stores the tiles to. */
	if (error == 0)
		error = job_partial_scratch(render, session, job, &outputs, memory);

	/* The background's programs and the load draw, the first part of the build timed alone. */
	started = powervr_timing_now();
	if (error == 0)
		error = job_programs(job, &outputs, load_mask, memory, &shared_registers);
	if (error == 0 && load != NULL)
		error = load_draw_build(render, given, load_mask, memory, load);
	powervr_timing_add(POWERVR_TIMING_BUILD_PROGRAMS, started);
	if (error != 0) {
		job_release(render, session, memory);
		load_draw_free(work, load);
		return error;
	}

	/* The draws' programs and streams (the load draws first), then the stream's end. */
	stream_words = 0;
	vertex_shareds = 0;
	if (job->draw_count != 0 || load != NULL) {
		/* The load draws, when there are any, go before the job's own. */
		prefix = NULL;
		prefix_count = 0;
		if (load != NULL) {
			prefix = load->draws;
			prefix_count = load->count;
		}
		draws_started = powervr_timing_now();
		error = job_draws(render, session, job, prefix, prefix_count, memory, &stream_words, &vertex_shareds);
		powervr_timing_add(POWERVR_TIMING_BUILD_DRAWS, draws_started);
		if (error != 0) {
			job_release(render, session, memory);
			load_draw_free(work, load);
			return error;
		}

		/* Diagnostics: the session keeps a note of its newest job for a reset's report. */
		powervr_session_note_job(session, job, prefix, prefix_count);
	}

	/* The shared registers the job takes: the background's or the draws', whichever is more. */
	if (vertex_shareds > shared_registers)
		shared_registers = vertex_shareds;

	/* The stream's end as Mesa ends a render: the terminate state's block, then the end. */
	powervr_render_terminate_state(job->width, job->height, terminate_state);
	put_words(memory->general->buffer, GENERAL_TERMINATE_PPP, terminate_state, POWERVR_RENDER_TERMINATE_STATE_WORDS);
	error = powervr_render_terminate_stream(memory->general->address + GENERAL_TERMINATE_PPP, terminate);
	if (error != 0) {
		job_release(render, session, memory);
		load_draw_free(work, load);
		return error;
	}

	put_words(memory->general->buffer, GENERAL_STREAM + stream_words * 4U, terminate, POWERVR_RENDER_TERMINATE_STREAM_WORDS);

	/* The commands: the stream, the fragment of the target's tiles with the draws' tables. */
	memory->fragment_setup.scissor_table = memory->general->address + GENERAL_SCISSORS;
	memory->fragment_setup.dbias_table = memory->general->address + GENERAL_DBIAS;

	/* The visibility registers of the job's queries, from zero. */
	memory->fragment_setup.occlusion_base = 0;
	if (job->visibility != NULL) {
		zero_words(memory->general->buffer, GENERAL_VISIBILITY, job->visibility->count);
		memory->fragment_setup.occlusion_base = memory->general->address + GENERAL_VISIBILITY;
	}

	/* A depth target's loads, stores and background depth. */
	if (job->depth.present) {
		error = powervr_render_zls_words(&depth, &zls);
		if (error != 0) {
			job_release(render, session, memory);
			load_draw_free(work, load);
			return error;
		}
		memory->fragment_setup.has_depth = 1;
		memory->fragment_setup.depth_bias_integer = job->depth.format != NULL &&
		    (job->depth.format->vk_format == VK_FORMAT_D16_UNORM || job->depth.format->vk_format == VK_FORMAT_D24_UNORM_S8_UINT);
		memory->fragment_setup.zls_control = zls.control;
		memory->fragment_setup.zls_base = zls.base;
		memory->fragment_setup.background_depth = zls.background_depth;
		memory->fragment_setup.zls_stride = zls.stride;
		memory->fragment_setup.has_stencil = zls.has_stencil;
		memory->fragment_setup.stencil_base = zls.stencil_base;
		memory->fragment_setup.stencil_stride = zls.stencil_stride;
		memory->fragment_setup.background_stencil = zls.background_stencil;
	}

	/* The target's tiles and the border colour table, then the fragment command. */
	layout = powervr_target_layout_of(target);
	memory->fragment_setup.tiles_x = layout->tiles_x;
	memory->fragment_setup.tiles_y = layout->tiles_y;
	memory->fragment_setup.samples = layout->samples;
	memory->fragment_setup.skip_empty_tiles = job_skips_empty_tiles(job);
	/*
	 * The background object stays on for a job with a scratch buffer: the
	 * 3D after a partial render loads the tiles back by it (Mesa 26.2.6
	 * src/imagination/vulkan/pvr_arch_cmd_buffer.c pvr_sub_cmd_gfx_job_init:
	 * enable_bg_tag for the load from the scratch buffer).
	 */
	memory->fragment_setup.no_background = !memory->fragment_setup.scratch_buffer && job_needs_no_background(job, load_mask);
	memory->fragment_setup.border_table = powervr_session_border_table(session);
	memory->geometry_setup.border_table = memory->fragment_setup.border_table;
	powervr_fragment_command(&memory->fragment_setup, memory->fragment);
	job_partial_command(memory);

	/* The diagnostics: the fragment command's words, eight a line, to set a loading job beside a clearing one. */
	if (POWERVR_DIAGNOSTICS) {
		for (at = 0; at < POWERVR_FRAGMENT_BYTES; at += 32U) {
			kern_logf("powervr: render job: fragment +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", (unsigned)at,
				  (unsigned)get_le32(memory->fragment + at), (unsigned)get_le32(memory->fragment + at + 4U),
				  (unsigned)get_le32(memory->fragment + at + 8U), (unsigned)get_le32(memory->fragment + at + 12U),
				  (unsigned)get_le32(memory->fragment + at + 16U), (unsigned)get_le32(memory->fragment + at + 20U),
				  (unsigned)get_le32(memory->fragment + at + 24U), (unsigned)get_le32(memory->fragment + at + 28U));
		}
	}
	memory->geometry_setup.stream = memory->general->address + GENERAL_STREAM;
	memory->geometry_setup.region_header_bytes = layout->region_header_bytes;
	memory->geometry_setup.shared_registers = shared_registers;
	powervr_geometry_command(&memory->geometry_setup, memory->geometry);

	/* The record that keeps the job's memory until its 3D finished. */
	powervr_timing_add(POWERVR_TIMING_JOB_BUILD, started);
	done = kern_calloc(1, sizeof(*done));
	if (done == NULL) {
		job_release(render, session, memory);
		load_draw_free(work, load);
		return ENOMEM;
	}

	/* The kicks: the geometry, the partial render's command, the fragment's. */
	started = powervr_timing_now();
	mutex_lock(&render->core->lock);

	/*
	 * The geometry waits for the 3D of the last render on the same half
	 * of the target (the half's parameter manager state is rebuilt by
	 * the geometry, which a 3D still reading it must not see).  Renders
	 * alternate between the halves, so a render's geometry runs while
	 * the previous render's 3D reads the other half.
	 *
	 * Procedure referenced:
	 * Linux v7.3 drivers/gpu/drm/imagination/pvr_job.c (prepare_job_syncs,
	 * update_job_resvs: the geometry job writes the HWRT data's region
	 * headers and so waits for every job on that one HWRT data, the
	 * fragment job reads them; the set's other HWRT data is not waited
	 * for) and Mesa 26.2.5
	 * src/imagination/vulkan/pvr_arch_job_render.c (pvr_arch_render_job_submit:
	 * the data set's two render target datas taken in turn).
	 */
	half = powervr_target_take_half(target);
	session->trace_pixels = (uint64_t)job->area.width * job->area.height;
	session->noted_target = target;
	session->noted_half = half;
	powervr_target_catalogues(target, half, session->noted_catalogues);
	half_fragment = powervr_target_half_fragment(target, half);
	wait_count = 0;
	if (session->render_context != NULL && half_fragment != 0U) {
		previous[wait_count].address = powervr_object_address(&session->render_context->fragment.timeline, 0);
		previous[wait_count].value = half_fragment;
		done->waited_fragment = half_fragment;
		wait_count++;
	}

	/*
	 * A render after transfers left running: its 3D waits for the last of
	 * them (it may read what they wrote, or draw over what they read).
	 * Its geometry waits for the newest that wrote a buffer (the vertex,
	 * index and uniform buffers are read there), and for the last when a
	 * vertex shader samples an image or stores; otherwise the geometry
	 * runs beside the transfers (a frame's geometry beside the copy that
	 * presents the frame before).
	 */
	fragment_wait_count = 0;
	if (session->last_kind == POWERVR_POINT_TRANSFER) {
		if (job_geometry_meets_images(job)) {
			wait_count += powervr_session_point_sync(session, session->last_kind, session->last_value, &previous[wait_count]);
		} else {
			if (session->transfer_wrote_buffer)
				wait_count += powervr_session_point_sync(session, session->last_kind, session->transfer_buffer_value, &previous[wait_count]);
			fragment_wait_count = powervr_session_point_sync(session, session->last_kind, session->last_value, &transfer_wait);
		}
	}
	error = powervr_session_kick_render(render, session, target, half, memory->geometry, memory->partial, memory->fragment, previous, wait_count,
					    &transfer_wait, fragment_wait_count, &memory->marks);

	mutex_unlock(&render->core->lock);

	if (error != 0) {
		kern_free(done);
		job_release(render, session, memory);
		load_draw_free(work, load);
		return error;
	}

	/*
	 * The job runs on: its memory waits in the session's list until its
	 * 3D point is reached (powervr_render_jobs_reap), and the session's
	 * later callbacks and fences complete at that point.
	 */
	powervr_timing_add(POWERVR_TIMING_JOB_KICK, started);
	done->memory = memory;
	done->visibility = job->visibility;
	done->work = work;
	done->load = load;
	done->kicked = powervr_timing_now();
	done->small = job->width * job->height <= JOB_SMALL_PIXELS;
	enabled = spin_lock_irqsave(&render->queue_lock);

	if (session->jobs_done_tail != NULL) {
		session->jobs_done_tail->next = done;
	} else {
		session->jobs_done = done;
	}
	session->jobs_done_tail = done;

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	powervr_session_point_set(session, POWERVR_POINT_FRAGMENT, memory->marks.fragment_value);

	/* The half's 3D value, which the half's next render's geometry waits for. */
	powervr_target_half_note(target, half, memory->marks.fragment_value);

	/* A small depth target rendered from the scratch: what the render stored copied back after its 3D. */
	if (depth_copy_back) {
		error = depth_scratch_copy(render, session, &job->depth, &depth, false);
		if (error != 0)
			return error;
	}

	/* Succeeded: the render was kicked. */
	return 0;
}

/*
 * Picks the depth target a job's ZLS loads and stores.  One whose
 * physical edges reach a ZLS tile (or that the ZLS neither loads nor
 * stores) is the attachment itself.  A smaller one would have the ZLS
 * write whole tiles past its memory: the job renders from the session's
 * scratch buffer instead, linear rows of the edges rounded up to the
 * tile (four samples: twice the rows of twice the length, as the ZLS
 * scales the edges by two for them), its texels copied in by a transfer
 * before the render when the
 * ZLS loads them (*copy_back tells the caller to copy them back after
 * the render when it stores them).  Both transfers are ordered with the
 * render by the firmware: the render's 3D waits for the copy in, the
 * copy back for the render's 3D.  The scratch is kept by the session and
 * grown when a job needs more (the old one unmapped once the session's
 * work no longer reaches it).
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_sub_cmd_gfx_requires_ds_subtile_alignment: a physical extent
 * below the ZLS tile without zls_subtile, when the ZLS loads or stores;
 * pvr_sub_cmd_gfx_align_ds_subtiles: a buffer of the extent aligned to
 * the ZLS tile, of the layer size times the ISP's sample scale,
 * rogue_get_isp_scale_xy_from_samples, the image copied into it with the
 * raw copy format and rows of the aligned width before the render when
 * loaded, back after it when stored, the render's ZLS at the buffer with
 * the aligned extent)
 * and pvr_arch_job_render.c (pvr_frag_state_stream_init: such a buffer
 * loaded and stored untwiddled).
 */
static int
job_depth_scratch(
	struct powervr_render *render,
	struct powervr_session *session,
	const struct powervr_render_job *job,
	struct powervr_render_depth *depth,
	bool *copy_back)
{
	struct powervr_buffer *buffer;
	uint32_t width;
	uint32_t height;
	uint32_t scale;
	uint64_t bytes;
	bool stencil;
	bool load;
	bool store;
	int error;

	/* The attachment itself, unless it is small and the ZLS reaches it. */
	*depth = job->depth;
	*copy_back = false;
	if (!job->depth.present || job->depth.format == NULL)
		return 0;
	if (job->depth.physical_width >= ZLS_TILE_PIXELS && job->depth.physical_height >= ZLS_TILE_PIXELS)
		return 0;

	/* What the ZLS loads and stores (as powervr_render_zls_words works it out). */
	stencil = (job->depth.format->kind & POWERVR_FORMAT_STENCIL) != 0;
	store = job->depth.store_op == VK_ATTACHMENT_STORE_OP_STORE || (stencil && job->depth.stencil_store_op == VK_ATTACHMENT_STORE_OP_STORE);
	load = job->depth.load_op == VK_ATTACHMENT_LOAD_OP_LOAD || (stencil && job->depth.stencil_load_op == VK_ATTACHMENT_LOAD_OP_LOAD) ||
	    (store && job->depth.partial);
	if (!load && !store)
		return 0;

	/* The scratch's edges and bytes: the physical edges rounded up to the tile, each scaled by two for four samples. */
	scale = job->depth.samples == 4U ? 2U : 1U;
	width = (job->depth.physical_width + ZLS_TILE_PIXELS - 1U) / ZLS_TILE_PIXELS * ZLS_TILE_PIXELS;
	height = (job->depth.physical_height + ZLS_TILE_PIXELS - 1U) / ZLS_TILE_PIXELS * ZLS_TILE_PIXELS;
	bytes = (uint64_t)width * scale * height * scale * job->depth.format->texel_bytes;
	if (bytes == 0)
		return EINVAL;

	/* The session's scratch, made or grown (the old one unmapped once the session's work no longer reaches it). */
	if (session->depth_scratch == NULL || session->depth_scratch_bytes < bytes) {
		if (session->depth_scratch != NULL) {
			powervr_resource_unmake(session, session->depth_scratch);
			session->depth_scratch = NULL;
			session->depth_scratch_bytes = 0;
		}
		error = powervr_buffer_create(render, bytes, &buffer);
		if (error != 0)
			return error;
		error = powervr_resource_make(session, buffer, 1, 0, &session->depth_scratch);
		powervr_buffer_put(render, buffer);
		if (error != 0) {
			session->depth_scratch = NULL;
			return error;
		}
		session->depth_scratch_bytes = bytes;
	}

	/* The ZLS's target: the scratch, linear rows of the rounded edges. */
	depth->address = session->depth_scratch->address;
	depth->physical_width = width;
	depth->physical_height = height;
	depth->twiddled = false;
	depth->layer_bytes = bytes;

	/* The attachment's texels in first when the ZLS loads them. */
	if (load) {
		error = depth_scratch_copy(render, session, &job->depth, depth, true);
		if (error != 0)
			return error;
	}

	/* Succeeded: the scratch, copied back after the render when stored. */
	*copy_back = store;
	return 0;
}

/*
 * Copies a depth target's texels (both aspects of a packed depth and
 * stencil, raw) between the attachment and the scratch buffer of rows of
 * its rounded width, ordered after the session's work and left running.
 *
 * Procedure referenced: zedBSD src/drivers/gpu/powervr/vk/vk-transfer.c
 * (buffer_image_run: a linear buffer as the source view or the
 * destination surface of a copy of an image's level).
 */
static int
depth_scratch_copy(
	struct powervr_render *render,
	struct powervr_session *session,
	const struct powervr_render_depth *image,
	const struct powervr_render_depth *scratch,
	bool into_scratch)
{
	struct powervr_transfer_request request;
	struct powervr_image_layout layout;
	struct powervr_view_shape view;
	const struct powervr_render_depth *from;
	const struct powervr_render_depth *to;
	uint32_t texel;
	uint32_t scale;
	int error;

	/*
	 * The source as a view of its own layout and the destination as a
	 * surface, each of the attachment's edges, rows of its physical
	 * width.  Four samples are copied as one sample of edges twice as
	 * long: a twiddled image keeps each texel's four samples together,
	 * which is the twiddled order of the doubled edges, and the ZLS reads
	 * and writes the doubled edges.
	 */
	from = into_scratch ? image : scratch;
	to = into_scratch ? scratch : image;
	texel = image->format->texel_bytes;
	scale = image->samples == 4U ? 2U : 1U;
	kern_memset(&layout, 0, sizeof(layout));
	layout.row_pitch = from->physical_width * scale * texel;
	layout.padded_width = from->physical_width * scale;
	layout.padded_height = from->physical_height * scale;
	layout.padded_depth = 1;
	layout.layer_bytes = from->layer_bytes;
	layout.bytes = from->layer_bytes;
	kern_memset(&view, 0, sizeof(view));
	view.format = image->format;
	view.layout = &layout;
	view.address = from->address;
	view.width = image->width * scale;
	view.height = image->height * scale;
	view.image_levels = 1;
	view.level_count = 1;
	view.layer_count = 1;
	view.linear = !from->twiddled;

	/* The copy of the attachment's edges, from the origin. */
	kern_memset(&request, 0, sizeof(request));
	request.kind = POWERVR_TRANSFER_COPY;
	request.destination.format = image->format;
	request.destination.address = to->address;
	request.destination.width = image->width * scale;
	request.destination.height = image->height * scale;
	request.destination.linear = !to->twiddled;
	request.destination.row_pitch = to->physical_width * scale * texel;
	request.destination.padded_width = to->physical_width * scale;
	request.source = &view;
	request.area.width = image->width * scale;
	request.area.height = image->height * scale;
	error = powervr_transfer_run_async(render, session, &request);
	if (error != 0)
		return error;

	/* Succeeded: the copy is the core's. */
	return 0;
}

/*
 * Releases the memory of a session's render jobs whose 3D points were
 * reached, oldest first.  With wait 1 each job is waited for, bounded by
 * POWERVR_PROGRESS_MS; with wait 2 every job is released (the session's
 * work is over).  Nothing is released of a lost or isolated session, or
 * on a faulted device, before then: the core may still reach it.
 */
void
powervr_render_jobs_reap(
	struct powervr_render *render,
	struct powervr_session *session,
	int wait)
{
	struct powervr_job_done *done;
	unsigned long enabled;
	uint64_t started;
	int reached;
	int halted;
	int error;

	/* Oldest first, while there is one to release. */
	for (;;) {
		/* The oldest job, and whether its point was reached. */
		reached = 0;
		halted = 0;
		enabled = spin_lock_irqsave(&render->queue_lock);

		done = session->jobs_done;
		if (done != NULL) {
			reached = powervr_session_point_reached(session, POWERVR_POINT_FRAGMENT, done->memory->marks.fragment_value);
			if (render->faulted || session->lost || session->isolated)
				halted = 1;
		}

		spin_unlock_irqrestore(&render->queue_lock, enabled);

		/* Nothing more. */
		if (done == NULL)
			return;

		/* The core may still reach it, until the session's work is over. */
		if (halted && wait != 2)
			return;

		/* Not reached: left for later, or waited for. */
		if (!reached && wait == 0)
			return;
		if (!reached && wait == 1) {
			error = powervr_session_point_wait(session, POWERVR_POINT_FRAGMENT, done->memory->marks.fragment_value, POWERVR_PROGRESS_MS);
			if (error != 0)
				return;
		}

		/* Takes it off the list. */
		enabled = spin_lock_irqsave(&render->queue_lock);

		session->jobs_done = done->next;
		if (session->jobs_done == NULL)
			session->jobs_done_tail = NULL;
		if (POWERVR_SUBMIT_TIMING && !halted)
			job_timing(session, done);

		spin_unlock_irqrestore(&render->queue_lock, enabled);

		/* Its stages and timing (seen at this point, after the 3D finished). */
		if (!halted) {
			powervr_render_retired(render, session, &done->memory->marks);
			powervr_timing_add(POWERVR_TIMING_JOB_WAIT, done->kicked);
			if (done->small)
				powervr_timing_add(POWERVR_TIMING_WAIT_SMALL, done->kicked);
		}

		/* Its queries' counts, read before its memory goes (lost when the job did not finish). */
		if (done->visibility != NULL)
			job_visibility_report(done, halted || (!reached && wait == 2));

		/* Its memory. */
		started = powervr_timing_now();
		job_release(render, session, done->memory);
		load_draw_free(done->work, done->load);
		powervr_timing_add(POWERVR_TIMING_JOB_RELEASE, started);
		kern_free(done);
	}
}

/*
 * Notes when the TA and the 3D of a session's running render jobs were
 * seen done, for the submission timing: a job whose geometry or fragment
 * timeline reached its value takes now as that part's end.  The queue
 * lock is held; the service calls it after each wakeup.
 */
void
powervr_render_jobs_stamp(
	struct powervr_session *session,
	uint64_t now)
{
	struct powervr_render_context *context;
	struct powervr_job_done *done;
	uint32_t geometry_now;
	uint32_t fragment_now;

	/*
	 * Nothing to note without a running job: a session's render context
	 * may still be being made (its timelines not yet mapped) until its
	 * first job is kicked.
	 */
	if (session->jobs_done == NULL)
		return;
	context = session->render_context;
	if (context == NULL || !context->kicked)
		return;

	/* The timelines' values now. */
	geometry_now = powervr_object_get32(&context->geometry.timeline, POWERVR_SYNC_ADDRESS);
	fragment_now = powervr_object_get32(&context->fragment.timeline, POWERVR_SYNC_ADDRESS);

	/* Each running job whose TA or 3D reached its value since the last look. */
	for (done = session->jobs_done; done != NULL; done = done->next) {
		/* Its TA, once the geometry's value is reached. */
		if (done->ta_seen == 0U && (int32_t)(geometry_now - done->memory->marks.geometry_value) >= 0)
			done->ta_seen = now;

		/* Its 3D, once the fragment's value is reached. */
		if (done->fragment_seen == 0U && (int32_t)(fragment_now - done->memory->marks.fragment_value) >= 0)
			done->fragment_seen = now;
	}
}

/*
 * Frees the job memory a session kept mapped for its next jobs, the
 * small depth targets' scratch and the tile buffers, when the session
 * ends: their buffers are unmapped and freed.
 */
void
powervr_render_jobs_forget(
	struct powervr_render *render,
	struct powervr_session *session)
{
	struct job_memory *memory;

	/* Each kept memory, until none is left. */
	for (;;) {
		memory = job_keep_take(render, session);
		if (memory == NULL)
			break;
		job_free(render, session, memory);
	}

	/* The small depth targets' scratch. */
	if (session->depth_scratch != NULL) {
		powervr_resource_unmake(session, session->depth_scratch);
		session->depth_scratch = NULL;
		session->depth_scratch_bytes = 0;
	}

	/* The tile buffers. */
	if (session->tile_buffers != NULL) {
		powervr_resource_unmake(session, session->tile_buffers);
		session->tile_buffers = NULL;
		session->tile_buffer_count = 0;
	}

	/* The partial renders' scratch buffer. */
	if (session->partial_scratch != NULL) {
		powervr_resource_unmake(session, session->partial_scratch);
		session->partial_scratch = NULL;
		session->partial_scratch_bytes = 0;
	}
}

/*
 * Finds or compiles the load draw's pipeline for the colour attachment
 * at a location of a job: the vertex shader passing a position and a
 * texture coordinate (one binding of two R32G32 attributes), the
 * fragment shader sampling set 0, binding 0 there into the output at the
 * location (of floats, or of the attachment's integers), keyed with
 * every attachment's format so its pixel output registers are those the
 * job's other draws use; a triangle list without culling or depth, the
 * viewport and scissor given by the draw.  With more than one colour
 * attachment the draw is translucent: an opaque one would hide what the
 * background and the other load draws wrote to the other attachments.
 * Kept by the render node for every later job; the caller holds the
 * core's lock.
 */
static int
load_graphics(
	struct powervr_render *render,
	const struct powervr_render_job *job,
	uint32_t location,
	struct powervr_vk_graphics **result)
{
	struct powervr_vk_graphics *graphics;
	struct powervr_draw_fixed *fixed;
	struct load_key key;
	uint32_t index;
	uint32_t slot;
	uint32_t used;
	int error;

	/* The shape the pipeline is for. */
	kern_memset(&key, 0, sizeof(key));
	used = 0;
	for (index = 0; index < job->color_count; index++) {
		if (job->colors[index].surface.format == NULL)
			continue;
		key.formats[index] = job->colors[index].surface.format->vk_format;
		used++;
	}
	key.location = location;
	key.samples = job->samples == 4U ? 4U : 1U;

	/* The node's table, made once. */
	if (render->load_draws == NULL) {
		render->load_draws = kern_calloc(1, sizeof(*render->load_draws));
		if (render->load_draws == NULL)
			return ENOMEM;
	}

	/* A pipeline already compiled for the shape. */
	for (slot = 0; slot < LOAD_DRAW_PIPELINES; slot++) {
		if (render->load_draws->graphics[slot] == NULL)
			continue;
		if (kern_memcmp(&render->load_draws->keys[slot], &key, sizeof(key)) != 0)
			continue;

		*result = render->load_draws->graphics[slot];
		return 0;
	}

	/* A free slot for a new one. */
	for (slot = 0; slot < LOAD_DRAW_PIPELINES; slot++) {
		if (render->load_draws->graphics[slot] == NULL)
			break;
	}

	if (slot == LOAD_DRAW_PIPELINES)
		return ENOBUFS;

	/* The pipeline's record. */
	graphics = kern_calloc(1, sizeof(*graphics));
	if (graphics == NULL)
		return ENOMEM;

	/* The fixed state and the keys: every attachment's format, the location's written whole. */
	fixed = &graphics->fixed;
	fixed->topology = LOAD_DRAW_TOPOLOGY;
	fixed->dynamic_viewport = true;
	fixed->dynamic_scissor = true;
	fixed->binding_count = 1;
	fixed->bindings[0].stride = LOAD_DRAW_STRIDE;
	fixed->attribute_count = 2;
	fixed->attributes[0].location = 0;
	fixed->attributes[0].format = LOAD_DRAW_POSITION_FORMAT;
	fixed->attributes[1].location = 1;
	fixed->attributes[1].format = LOAD_DRAW_POSITION_FORMAT;
	fixed->attributes[1].offset = 8;
	fixed->vertex_key.attribute_formats[0] = LOAD_DRAW_POSITION_FORMAT;
	fixed->vertex_key.attribute_formats[1] = LOAD_DRAW_POSITION_FORMAT;
	fixed->vertex_key.samples = 1;
	for (index = 0; index < POWERVR_PASS_COLORS; index++)
		fixed->fragment_key.output_formats[index] = key.formats[index];
	fixed->fragment_key.blend[location].write_mask = 0xf;
	fixed->fragment_key.samples = key.samples;
	fixed->blending = used > 1U;

	/* The two shaders and the varyings between them; what was allocated goes on a failure. */
	error = load_graphics_compile(graphics, location, key.formats[location]);
	if (error != 0) {
		kern_free(graphics->vertex_code);
		kern_free(graphics->fragment_code);
		kern_free(graphics);
		return error;
	}

	/* Succeeded: the pipeline, kept. */
	render->load_draws->keys[slot] = key;
	render->load_draws->graphics[slot] = graphics;
	*result = graphics;
	return 0;
}

/*
 * Compiles the load draw's two shaders for the attachment of a format at
 * a location into code rooms of their own and links their varyings; the
 * caller frees the code rooms on a failure.
 */
static int
load_graphics_compile(
	struct powervr_vk_graphics *graphics,
	uint32_t location,
	uint32_t vk_format)
{
	struct powervr_shader_workspace *workspace;
	int error;

	/* The vertex shader's code room. */
	graphics->vertex_code = kern_malloc(LOAD_DRAW_CODE_BYTES);
	if (graphics->vertex_code == NULL)
		return ENOMEM;

	/* The fragment shader's code room. */
	graphics->fragment_code = kern_malloc(LOAD_DRAW_CODE_BYTES);
	if (graphics->fragment_code == NULL)
		return ENOMEM;

	/* The compiler's working memory. */
	workspace = kern_malloc(powervr_shader_workspace_size());
	if (workspace == NULL)
		return ENOMEM;

	/* The shaders and their varyings. */
	error = load_graphics_shaders(graphics, location, vk_format, workspace);

	/* The working memory goes, whatever the outcome. */
	kern_free(workspace);

	/* Reports a shader that did not compile. */
	if (error != 0)
		return error;

	/* Succeeded: both shaders are compiled and linked. */
	return 0;
}

/*
 * Compiles the load draw's vertex shader and the fragment shader of the
 * location and of the format's kind (floats, unsigned or signed
 * integers), and builds their varyings.
 */
static int
load_graphics_shaders(
	struct powervr_vk_graphics *graphics,
	uint32_t location,
	uint32_t vk_format,
	struct powervr_shader_workspace *workspace)
{
	static const uint32_t *const fragments[2][POWERVR_PASS_COLORS][3] = {
		{
			{ powervr_load_draw_frag0_float, powervr_load_draw_frag0_unsigned, powervr_load_draw_frag0_signed },
			{ powervr_load_draw_frag1_float, powervr_load_draw_frag1_unsigned, powervr_load_draw_frag1_signed },
			{ powervr_load_draw_frag2_float, powervr_load_draw_frag2_unsigned, powervr_load_draw_frag2_signed },
			{ powervr_load_draw_frag3_float, powervr_load_draw_frag3_unsigned, powervr_load_draw_frag3_signed },
		},
		{
			{ powervr_load_draw_frag0_float_multisampled, powervr_load_draw_frag0_unsigned_multisampled, powervr_load_draw_frag0_signed_multisampled },
			{ powervr_load_draw_frag1_float_multisampled, powervr_load_draw_frag1_unsigned_multisampled, powervr_load_draw_frag1_signed_multisampled },
			{ powervr_load_draw_frag2_float_multisampled, powervr_load_draw_frag2_unsigned_multisampled, powervr_load_draw_frag2_signed_multisampled },
			{ powervr_load_draw_frag3_float_multisampled, powervr_load_draw_frag3_unsigned_multisampled, powervr_load_draw_frag3_signed_multisampled },
		},
	};
	static const uint32_t fragment_words[2][POWERVR_PASS_COLORS][3] = {
		{
			{ sizeof(powervr_load_draw_frag0_float) / 4U, sizeof(powervr_load_draw_frag0_unsigned) / 4U, sizeof(powervr_load_draw_frag0_signed) / 4U },
			{ sizeof(powervr_load_draw_frag1_float) / 4U, sizeof(powervr_load_draw_frag1_unsigned) / 4U, sizeof(powervr_load_draw_frag1_signed) / 4U },
			{ sizeof(powervr_load_draw_frag2_float) / 4U, sizeof(powervr_load_draw_frag2_unsigned) / 4U, sizeof(powervr_load_draw_frag2_signed) / 4U },
			{ sizeof(powervr_load_draw_frag3_float) / 4U, sizeof(powervr_load_draw_frag3_unsigned) / 4U, sizeof(powervr_load_draw_frag3_signed) / 4U },
		},
		{
			{ sizeof(powervr_load_draw_frag0_float_multisampled) / 4U, sizeof(powervr_load_draw_frag0_unsigned_multisampled) / 4U,
			  sizeof(powervr_load_draw_frag0_signed_multisampled) / 4U },
			{ sizeof(powervr_load_draw_frag1_float_multisampled) / 4U, sizeof(powervr_load_draw_frag1_unsigned_multisampled) / 4U,
			  sizeof(powervr_load_draw_frag1_signed_multisampled) / 4U },
			{ sizeof(powervr_load_draw_frag2_float_multisampled) / 4U, sizeof(powervr_load_draw_frag2_unsigned_multisampled) / 4U,
			  sizeof(powervr_load_draw_frag2_signed_multisampled) / 4U },
			{ sizeof(powervr_load_draw_frag3_float_multisampled) / 4U, sizeof(powervr_load_draw_frag3_unsigned_multisampled) / 4U,
			  sizeof(powervr_load_draw_frag3_signed_multisampled) / 4U },
		},
	};
	const struct powervr_format *format;
	struct powervr_draw_fixed *fixed;
	uint32_t multisampled;
	uint32_t kind;
	int error;

	/* The fragment shader's kind: floats, or the integers of an integer format. */
	if (location >= POWERVR_PASS_COLORS)
		return EINVAL;
	format = powervr_format_find(vk_format);
	if (format == NULL)
		return EINVAL;
	kind = 0;
	if ((format->kind & POWERVR_FORMAT_INTEGER) != 0)
		kind = format_is_signed_integer(vk_format) ? 2U : 1U;

	/* The vertex shader. */
	fixed = &graphics->fixed;
	error = powervr_shader_compile(powervr_load_draw_vert, sizeof(powervr_load_draw_vert) / 4U, POWERVR_SHADER_VERTEX, &fixed->vertex_key, workspace, &graphics->vertex, graphics->vertex_code, LOAD_DRAW_CODE_BYTES);
	if (error != 0)
		return error;

	/* The fragment shader: of several samples, the one run for each sample that fetches its own. */
	multisampled = fixed->fragment_key.samples == 4U ? 1U : 0U;
	error = powervr_shader_compile(fragments[multisampled][location][kind], fragment_words[multisampled][location][kind], POWERVR_SHADER_FRAGMENT, &fixed->fragment_key,
				       workspace, &graphics->fragment, graphics->fragment_code, LOAD_DRAW_CODE_BYTES);
	if (error != 0)
		return error;

	/* The varyings between them. */
	error = powervr_draw_varyings_build(&graphics->vertex, &graphics->fragment, &graphics->varyings);
	if (error != 0)
		return error;

	/* Succeeded: the shaders and their varyings. */
	return 0;
}

/* Tells whether a format is one of signed integers (the others of POWERVR_FORMAT_INTEGER are unsigned). */
static bool
format_is_signed_integer(
	uint32_t vk_format)
{
	/* The signed integer formats of the executor's table. */
	switch (vk_format) {
	case VK_FORMAT_R8_SINT:
	case VK_FORMAT_R8G8_SINT:
	case VK_FORMAT_R8G8B8A8_SINT:
	case VK_FORMAT_R16_SINT:
	case VK_FORMAT_R16G16_SINT:
	case VK_FORMAT_R16G16B16A16_SINT:
	case VK_FORMAT_R32_SINT:
	case VK_FORMAT_R32G32_SINT:
	case VK_FORMAT_R32G32B32A32_SINT:
		return true;
	default:
		return false;
	}
}

/*
 * Builds a job's load draws, one for each colour attachment of
 * load_mask, in the order of the attachments: two triangles over the
 * framebuffer with texture coordinates 0 to 1, in the job's general
 * memory (shared by the draws).
 */
static int
load_draw_build(
	struct powervr_render *render,
	const struct powervr_render_job *job,
	uint32_t load_mask,
	struct job_memory *memory,
	struct load_draw *load)
{
	uint32_t index;
	int error;

	/* The vertices, in the job's general memory. */
	_Static_assert(sizeof(load_draw_vertices) <= GENERAL_DBIAS - GENERAL_LOAD_WORDS, "the load draw's vertices' room");
	put_words(memory->general->buffer, GENERAL_LOAD_WORDS, load_draw_vertices, LOAD_DRAW_VERTICES * 4U);

	/* Each loaded attachment's draw. */
	load->count = 0;
	for (index = 0; index < job->color_count; index++) {
		if ((load_mask & (1U << index)) == 0)
			continue;
		error = load_draw_one(render, job, index, memory, &load->draws[load->count], &load->sets[load->count]);
		if (error != 0)
			return error;
		load->count++;
	}

	/* Succeeded: the draws. */
	return 0;
}

/*
 * Builds the load draw of the colour attachment at a location: a set of
 * one combined image sampler, the attachment's view and a nearest
 * sampler clamped to the edge; the framebuffer's viewport and the render
 * area's scissor.  Sampled at each pixel's centre, a coordinate names
 * that pixel's own texel.
 *
 * Procedure referenced:
 * zedBSD src/drivers/gpu/powervr/vk/vk-layout.c (set_image: the image's
 * descriptor and the sampler's in a combined binding).
 */
static int
load_draw_one(
	struct powervr_render *render,
	const struct powervr_render_job *job,
	uint32_t location,
	struct job_memory *memory,
	struct powervr_render_draw *draw,
	struct powervr_vk_descriptor_set *set)
{
	const struct powervr_render_target *color;
	struct powervr_set_binding_info binding;
	struct powervr_sampler_shape sampler;
	struct powervr_vk_graphics *graphics;
	uint64_t view_words[2];
	uint64_t sampler_words[2];
	uint32_t offset;
	uint32_t bits;
	int error;

	/* The pipeline for the attachment's location among the job's formats. */
	color = &job->colors[location];
	mutex_lock(&render->core->lock);

	error = load_graphics(render, job, location, &graphics);

	mutex_unlock(&render->core->lock);

	/* Reports a pipeline that could not be made. */
	if (error != 0)
		return error;

	/* The set: one combined image sampler. */
	binding.binding = 0;
	binding.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.count = 1;
	error = powervr_set_layout_build(&binding, 1, &set->layout);
	if (error != 0)
		return error;
	set->bytes = kern_calloc(1, set->layout.bytes);
	if (set->bytes == NULL)
		return ENOMEM;
	error = powervr_view_words(&color->view, view_words);
	if (error != 0)
		return error;
	error = powervr_set_descriptor_offset(&set->layout, 0, 0, POWERVR_SHADER_PART_IMAGE_STATE, &offset);
	if (error != 0)
		return error;
	powervr_descriptor_image(&set->bytes[offset], view_words, (uint32_t)color->view.layout->layer_bytes, powervr_pack_info(color->surface.format->vk_format));
	kern_memset(&sampler, 0, sizeof(sampler));
	sampler.mag_filter = VK_FILTER_NEAREST;
	sampler.min_filter = VK_FILTER_NEAREST;
	sampler.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.address_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.address_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.address_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	error = powervr_sampler_words(&sampler, sampler_words);
	if (error != 0)
		return error;
	error = powervr_set_descriptor_offset(&set->layout, 0, 0, POWERVR_SHADER_PART_SAMPLER_STATE, &offset);
	if (error != 0)
		return error;
	powervr_descriptor_sampler(&set->bytes[offset], sampler_words, VK_COMPARE_OP_NEVER);

	/* The draw: the six vertices once, the framebuffer's viewport, the render area's scissor. */
	draw->pipeline = graphics;
	draw->sets[0] = set;
	draw->vertex_addresses[0] = memory->general->address + GENERAL_LOAD_WORDS;
	draw->count = LOAD_DRAW_VERTICES;
	draw->instances = 1;
	bits = float_bits(job->width);
	kern_memcpy(&draw->viewport.width, &bits, sizeof(bits));
	bits = float_bits(job->height);
	kern_memcpy(&draw->viewport.height, &bits, sizeof(bits));
	bits = float_bits(1);
	kern_memcpy(&draw->viewport.maxDepth, &bits, sizeof(bits));
	draw->scissor.offset.x = (int32_t)job->area.x;
	draw->scissor.offset.y = (int32_t)job->area.y;
	draw->scissor.extent.width = job->area.width;
	draw->scissor.extent.height = job->area.height;

	/* Succeeded: the draw. */
	return 0;
}

/* The bits of a whole number below 2^24 as a 32-bit float, without the processor's floating point. */
static uint32_t
float_bits(
	uint32_t value)
{
	uint32_t exponent;

	/* Zero is all zero bits. */
	if (value == 0)
		return 0;

	/* The highest set bit gives the exponent; the bits below it, shifted up, the mantissa. */
	exponent = 31U - (uint32_t)__builtin_clz(value);
	return ((127U + exponent) << 23) | ((value << (23U - exponent)) & 0x7fffffU);
}

/*
 * Finds or compiles the clear draw's pipeline for a job's attachments and
 * a clear's mask: the vertex shader placing the two triangles (one
 * binding of R32G32 positions, the load draw's) at the push constants'
 * depth, the fragment shader writing the push constants' colours to each
 * of the job's colour attachments with every channel written where the
 * mask names the attachment and none where it does not, the depth test
 * always passing and writing when the mask names the depth, the viewport
 * and scissor given by the draw.  A clear that leaves an attachment
 * alone is drawn in the translucent pass, which reads the attachment
 * back.  Kept by the render node for every later job; the caller holds
 * the core's lock.
 */
static int
clear_graphics(
	struct powervr_render *render,
	const struct powervr_render_job *job,
	uint32_t mask,
	const struct powervr_vk_graphics **result)
{
	struct powervr_vk_graphics *graphics;
	struct powervr_draw_fixed *fixed;
	struct clear_key key;
	uint32_t slot;
	uint32_t index;
	int error;

	/* The shape the pipeline is for. */
	kern_memset(&key, 0, sizeof(key));
	key.color_count = job->color_count;
	for (index = 0; index < job->color_count; index++) {
		if (job->colors[index].surface.format != NULL)
			key.formats[index] = job->colors[index].surface.format->vk_format;
	}
	key.has_depth = job->depth.present;
	key.mask = mask;

	/* The node's table, made once. */
	if (render->load_draws == NULL) {
		render->load_draws = kern_calloc(1, sizeof(*render->load_draws));
		if (render->load_draws == NULL)
			return ENOMEM;
	}

	/* A pipeline already compiled for the shape. */
	for (slot = 0; slot < CLEAR_DRAW_PIPELINES; slot++) {
		if (render->load_draws->clear_graphics[slot] == NULL)
			continue;
		if (kern_memcmp(&render->load_draws->clear_keys[slot], &key, sizeof(key)) != 0)
			continue;

		*result = render->load_draws->clear_graphics[slot];
		return 0;
	}

	/* A free slot for a new one. */
	for (slot = 0; slot < CLEAR_DRAW_PIPELINES; slot++) {
		if (render->load_draws->clear_graphics[slot] == NULL)
			break;
	}

	if (slot == CLEAR_DRAW_PIPELINES)
		return ENOBUFS;

	/* The pipeline's record. */
	graphics = kern_calloc(1, sizeof(*graphics));
	if (graphics == NULL)
		return ENOMEM;

	/* The triangles' one binding of positions. */
	fixed = &graphics->fixed;
	fixed->topology = LOAD_DRAW_TOPOLOGY;
	fixed->dynamic_viewport = true;
	fixed->dynamic_scissor = true;
	fixed->binding_count = 1;
	fixed->bindings[0].stride = LOAD_DRAW_STRIDE;
	fixed->attribute_count = 1;
	fixed->attributes[0].location = 0;
	fixed->attributes[0].format = LOAD_DRAW_POSITION_FORMAT;
	fixed->vertex_key.attribute_formats[0] = LOAD_DRAW_POSITION_FORMAT;
	fixed->vertex_key.samples = 1;

	/* Each colour attachment's format, all channels written where the mask names it, none where not. */
	for (index = 0; index < job->color_count; index++) {
		fixed->fragment_key.output_formats[index] = key.formats[index];
		if ((mask & (1U << index)) != 0)
			fixed->fragment_key.blend[index].write_mask = 0xf;
		else
			fixed->blending = true;
	}
	fixed->fragment_key.samples = 1;

	/* The depth written as it is where the mask names it. */
	if ((mask & POWERVR_DRAW_CLEAR_DEPTH) != 0) {
		fixed->depth_test = true;
		fixed->depth_write = true;
		fixed->depth_compare = CLEAR_DRAW_COMPARE_ALWAYS;
	}

	/*
	 * The stencil replaced by the draw's reference, always and whatever
	 * the depth test does, all eight bits, where the mask names it
	 * (render-pass.c sets the clear draw's reference to the value).
	 */
	if ((mask & POWERVR_DRAW_CLEAR_STENCIL) != 0) {
		fixed->stencil_test = true;
		fixed->dynamic_stencil_reference = true;
		for (index = 0; index < 2U; index++) {
			fixed->stencil[index].fail_op = VK_STENCIL_OP_REPLACE;
			fixed->stencil[index].pass_op = VK_STENCIL_OP_REPLACE;
			fixed->stencil[index].depth_fail_op = VK_STENCIL_OP_REPLACE;
			fixed->stencil[index].compare_op = CLEAR_DRAW_COMPARE_ALWAYS;
			fixed->stencil[index].compare_mask = 0xffU;
			fixed->stencil[index].write_mask = 0xffU;
		}
	}

	/* The two shaders and the varyings between them; what was allocated goes on a failure. */
	error = clear_graphics_compile(graphics, job->color_count);
	if (error != 0) {
		kern_free(graphics->vertex_code);
		kern_free(graphics->fragment_code);
		kern_free(graphics);
		return error;
	}

	/* Succeeded: the pipeline, kept. */
	render->load_draws->clear_keys[slot] = key;
	render->load_draws->clear_graphics[slot] = graphics;
	*result = graphics;
	return 0;
}

/*
 * Compiles the clear draw's vertex shader and its fragment shader of
 * outputs colours into code rooms of their own and links their
 * varyings; the caller frees the code rooms on a failure.
 */
static int
clear_graphics_compile(
	struct powervr_vk_graphics *graphics,
	uint32_t outputs)
{
	static const uint32_t *const fragments[POWERVR_PASS_COLORS + 1U] = {
		powervr_clear_draw_frag0,
		powervr_clear_draw_frag1,
		powervr_clear_draw_frag2,
		powervr_clear_draw_frag3,
		powervr_clear_draw_frag4,
	};
	static const uint32_t fragment_words[POWERVR_PASS_COLORS + 1U] = {
		sizeof(powervr_clear_draw_frag0) / 4U,
		sizeof(powervr_clear_draw_frag1) / 4U,
		sizeof(powervr_clear_draw_frag2) / 4U,
		sizeof(powervr_clear_draw_frag3) / 4U,
		sizeof(powervr_clear_draw_frag4) / 4U,
	};
	struct powervr_shader_workspace *workspace;
	struct powervr_draw_fixed *fixed;
	int error;

	/* A fragment shader of the outputs there are. */
	if (outputs > POWERVR_PASS_COLORS)
		return EINVAL;

	/* The two code rooms. */
	graphics->vertex_code = kern_malloc(LOAD_DRAW_CODE_BYTES);
	if (graphics->vertex_code == NULL)
		return ENOMEM;

	graphics->fragment_code = kern_malloc(LOAD_DRAW_CODE_BYTES);
	if (graphics->fragment_code == NULL)
		return ENOMEM;

	/* The compiler's working memory. */
	workspace = kern_malloc(powervr_shader_workspace_size());
	if (workspace == NULL)
		return ENOMEM;

	/* The vertex shader, the fragment shader, then the varyings between them. */
	fixed = &graphics->fixed;
	error = powervr_shader_compile(powervr_clear_draw_vert, sizeof(powervr_clear_draw_vert) / 4U, POWERVR_SHADER_VERTEX, &fixed->vertex_key, workspace, &graphics->vertex, graphics->vertex_code, LOAD_DRAW_CODE_BYTES);
	if (error == 0)
		error = powervr_shader_compile(fragments[outputs], fragment_words[outputs], POWERVR_SHADER_FRAGMENT, &fixed->fragment_key, workspace, &graphics->fragment, graphics->fragment_code, LOAD_DRAW_CODE_BYTES);
	if (error == 0)
		error = powervr_draw_varyings_build(&graphics->vertex, &graphics->fragment, &graphics->varyings);

	/* The working memory goes, whatever the outcome. */
	kern_free(workspace);

	/* Reports a shader that did not compile. */
	if (error != 0) {
		kern_logf("powervr: clear draw: the shaders did not compile (%d)\n", error);
		return error;
	}

	/* Succeeded: both shaders are compiled and linked. */
	return 0;
}

/* Frees a job's working copy and its load draws' sets. */
static void
load_draw_free(
	struct powervr_render_job *work,
	struct load_draw *load)
{
	uint32_t index;

	/* Each set's bytes (a draw that failed may have some without being counted). */
	if (load != NULL) {
		for (index = 0; index < POWERVR_PASS_COLORS; index++)
			kern_free(load->sets[index].bytes);
	}
	kern_free(load);
	kern_free(work);
}

/* The draw at an index of a job: the prefix draws first when there are any, then the job's own. */
static const struct powervr_render_draw *
job_draw(
	const struct powervr_render_job *job,
	const struct powervr_render_draw *prefix,
	uint32_t prefix_count,
	uint32_t index)
{
	/* The prefix comes first. */
	if (index < prefix_count)
		return &prefix[index];

	/* Succeeded: the job's draw after the prefix. */
	return &job->draws[index - prefix_count];
}

/*
 * Refuses a job the runner does not build yet, and finds its colour
 * attachments' places (job_outputs).
 */
static int
job_check(
	const struct powervr_render_job *job,
	struct job_outputs *outputs)
{
	uint32_t index;
	int error;

	/* At most an emit a colour attachment, draws each with a pipeline, visibility registers the memory holds. */
	if (job->color_count > POWERVR_PASS_COLORS || job->draw_count > POWERVR_RENDER_DRAWS)
		return ENOTSUP;
	if (job->visibility != NULL && (job->visibility->count == 0 || job->visibility->count > POWERVR_RENDER_VISIBILITY_MAX))
		return ENOTSUP;

	/*
	 * One sample or four; with four, a resolved colour of a format the
	 * back end averages (not of integers).
	 */
	if (job->samples > 1U && job->samples != 4U)
		return ENOTSUP;
	for (index = 0; job->samples == 4U && index < job->color_count; index++) {
		if (job->colors[index].resolve && job->colors[index].surface.format != NULL &&
		    (job->colors[index].surface.format->kind & POWERVR_FORMAT_INTEGER) != 0)
			return ENOTSUP;
	}

	/* The attachments' formats in the pixel output registers. */
	error = job_outputs(job, outputs);
	if (error != 0)
		return error;

	/* Succeeded: the job can run. */
	return 0;
}

/*
 * Places a job's colour attachments in the pixel output registers and
 * the tile buffers as the compiler places a fragment shader's outputs
 * for the same formats (powervr_shader_pixel_places), so the
 * background, the load draws and the end of tile name the places the
 * job's draws write.  Refuses (ENOTSUP) attachments past the tile
 * buffers and a format whose registers the compiler and the clear words
 * (powervr_format_output_word_count) count apart.
 */
static int
job_outputs(
	const struct powervr_render_job *job,
	struct job_outputs *outputs)
{
	uint32_t formats[POWERVR_SHADER_OUTPUTS];
	struct powervr_shader_pixel_place places[POWERVR_SHADER_OUTPUTS];
	uint32_t alone[POWERVR_SHADER_OUTPUTS];
	uint32_t alone_regs[POWERVR_SHADER_OUTPUTS];
	const struct powervr_format *format;
	uint32_t registers;
	uint32_t index;
	int error;

	/* The used attachments' formats. */
	kern_memset(formats, 0, sizeof(formats));
	outputs->used = 0;
	for (index = 0; index < job->color_count; index++) {
		format = job->colors[index].surface.format;
		if (format == NULL)
			continue;
		formats[index] = format->vk_format;
		outputs->used |= 1U << index;
	}

	/* Their places. */
	error = powervr_shader_pixel_places(formats, places, &outputs->registers, &outputs->tile_buffers);
	if (error != 0)
		return error;

	/* Each one placed, of as many registers as its clear words. */
	for (index = 0; index < POWERVR_PASS_COLORS; index++) {
		outputs->regs[index] = places[index].reg;
		outputs->tile_buffer[index] = places[index].tile_buffer;
		if ((outputs->used & (1U << index)) == 0)
			continue;
		if (places[index].reg == POWERVR_SHADER_NO_REG)
			return ENOTSUP;
		kern_memset(alone, 0, sizeof(alone));
		alone[0] = formats[index];
		error = powervr_shader_pixel_layout(alone, alone_regs, &registers);
		if (error != 0)
			return error;
		if (registers != powervr_format_output_word_count(job->colors[index].surface.format))
			return ENOTSUP;
	}

	/* Succeeded: the registers. */
	return 0;
}

/*
 * Packs the job's emits: for each used colour attachment its pixel back
 * end state words (for the end-of-tile program) and register words (for
 * the fragment command), stored from its first pixel output register as
 * its emit's index (a resolved attachment of four samples also, or only,
 * its resolve attachment's, downscaled); the attachments in the pixel
 * output registers first, in order, then those in tile buffers, each
 * stored from its first word, which the end of tile loads into the
 * registers first (tiles gives each emit's tile buffer,
 * POWERVR_SHADER_NO_REG for none); for a job without one (the depth
 * alone) a single emit that stores nothing.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_setup_emit_state: an emit for each end-of-tile surface, the
 * output register ones first, then the tile buffer ones with their tile
 * buffer, each pvr_setup_pbe_state with the emit's index and its
 * resource's register or offset in the buffer; one emit of a z-only
 * render without surfaces).
 */
static int
job_emits(
	const struct powervr_render_job *job,
	const struct job_outputs *outputs,
	uint32_t states[POWERVR_RENDER_EMITS_MAX][2],
	uint32_t *count,
	uint64_t reg[POWERVR_FRAGMENT_EMITS][3],
	uint32_t tiles[POWERVR_RENDER_EMITS_MAX])
{
	uint32_t index;
	uint32_t emit;
	uint32_t pass;
	bool tiled;
	bool resolve;
	int error;

	/*
	 * Each used attachment's emit, those in the registers in the first
	 * pass and those in tile buffers in the second; with four samples, a
	 * resolved one's emit to its resolve attachment, the samples
	 * averaged by the back end's downscale, besides its own when it is
	 * stored.
	 *
	 * Procedure referenced: Mesa 26.2.6 src/imagination/vulkan/
	 * pvr_arch_hw_pass.c (an end-of-tile surface for the resolve, of
	 * PVR_RESOLVE_TYPE_PBE, and one for the colour only when it is
	 * stored) and pvr_arch_cmd_buffer.c (pvr_setup_emit_state:
	 * pvr_setup_pbe_state with down_scale for the resolve).
	 */
	emit = 0;
	for (pass = 0; pass < 2U; pass++) {
		for (index = 0; index < job->color_count; index++) {
			if ((outputs->used & (1U << index)) == 0)
				continue;
			tiled = outputs->tile_buffer[index] != POWERVR_SHADER_NO_REG;
			if (tiled != (pass == 1U))
				continue;
			resolve = job->samples == 4U && job->colors[index].resolve;
			if (resolve) {
				if (emit >= POWERVR_RENDER_EMITS_MAX || emit >= POWERVR_FRAGMENT_EMITS)
					return ENOTSUP;
				error = powervr_render_pbe_words(&job->colors[index].resolve_surface, &job->area, states[emit], reg[emit]);
				if (error != 0)
					return error;
				powervr_render_pbe_resolve(reg[emit]);
				error = powervr_render_pbe_place(states[emit], outputs->regs[index], emit);
				if (error != 0)
					return error;
				tiles[emit] = outputs->tile_buffer[index];
				emit++;
				if (job->colors[index].store_op != VK_ATTACHMENT_STORE_OP_STORE)
					continue;
			}
			if (emit >= POWERVR_RENDER_EMITS_MAX || emit >= POWERVR_FRAGMENT_EMITS)
				return ENOTSUP;
			error = powervr_render_pbe_words(&job->colors[index].surface, &job->area, states[emit], reg[emit]);
			if (error != 0)
				return error;
			error = powervr_render_pbe_place(states[emit], outputs->regs[index], emit);
			if (error != 0)
				return error;
			tiles[emit] = outputs->tile_buffer[index];
			emit++;
		}
	}

	/* No colour: one emit of nothing. */
	if (emit == 0) {
		powervr_render_pbe_empty(states[0], reg[0]);
		tiles[0] = POWERVR_SHADER_NO_REG;
		emit = 1;
	}

	/* Succeeded: the emits. */
	*count = emit;
	return 0;
}

/*
 * Gives a job's tile buffers their memory: the session's, made or grown
 * to the buffers the job takes (the old memory unmapped once the
 * session's work no longer reaches it), each buffer's address in the
 * job's memory (0 past those the job takes).  A job without tile buffers
 * takes none.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_device.c
 * (pvr_device_tile_buffer_ensure_cap: buffers kept by the device, more
 * made when a render needs more, each pvr_get_tile_buffer_size bytes on
 * the cache line; never initialised) and pvr_arch_cmd_buffer.c (the
 * render's load and end-of-tile programs given the buffers' addresses).
 */
static int
job_tile_buffers(
	struct powervr_render *render,
	struct powervr_session *session,
	const struct job_outputs *outputs,
	struct job_memory *memory)
{
	struct powervr_buffer *buffer;
	uint32_t index;
	int error;

	/* No buffer's address yet. */
	for (index = 0; index < POWERVR_SHADER_TILE_BUFFERS; index++)
		memory->tile_buffer_addresses[index] = 0;
	if (outputs->tile_buffers == 0)
		return 0;
	if (outputs->tile_buffers > POWERVR_SHADER_TILE_BUFFERS)
		return ENOTSUP;

	/* The session's buffers, made or grown. */
	if (session->tile_buffers == NULL || session->tile_buffer_count < outputs->tile_buffers) {
		if (session->tile_buffers != NULL) {
			powervr_resource_unmake(session, session->tile_buffers);
			session->tile_buffers = NULL;
			session->tile_buffer_count = 0;
		}
		error = powervr_buffer_create(render, (uint64_t)TILE_BUFFER_BYTES * outputs->tile_buffers, &buffer);
		if (error != 0)
			return error;
		error = powervr_resource_make(session, buffer, 1, 0, &session->tile_buffers);
		powervr_buffer_put(render, buffer);
		if (error != 0) {
			session->tile_buffers = NULL;
			return error;
		}
		session->tile_buffer_count = outputs->tile_buffers;
	}

	/* Each buffer the job takes, side by side. */
	for (index = 0; index < outputs->tile_buffers; index++)
		memory->tile_buffer_addresses[index] = session->tile_buffers->address + (uint64_t)index * TILE_BUFFER_BYTES;

	/* Succeeded: the buffers. */
	return 0;
}

/*
 * Plans the job's partial render save (powervr_render_partial_plan) and
 * takes the session's scratch buffer for it, made or grown: a job with
 * colour outputs stores its tiles there when the parameter buffer runs
 * out and the render is split.  A job without colour outputs has no
 * scratch (its depth is the firmware's to keep across the split).
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_framebuffer.c and
 * pvr_spm.c (the scratch buffer taken for every framebuffer, shared and
 * grown: pvr_spm_scratch_buffer_get_buffer) and pvr_arch_cmd_buffer.c
 * (pvr_sub_cmd_gfx_job_init: "there can only ever be one frag job
 * running on the hardware at any one time", so jobs share it).
 */
static int
job_partial_scratch(
	struct powervr_render *render,
	struct powervr_session *session,
	const struct powervr_render_job *job,
	const struct job_outputs *outputs,
	struct job_memory *memory)
{
	struct powervr_buffer *buffer;
	int error;

	/* No colour output, no scratch. */
	memory->partial_scratch = 0;
	memory->fragment_setup.scratch_buffer = false;
	if (outputs->registers == 0)
		return 0;

	/* The regions of the job's tile. */
	error = powervr_render_partial_plan(job->width, job->height, job->samples == 4U ? 4U : 1U, outputs->registers, outputs->tile_buffers,
					   &memory->partial_plan);
	if (error != 0)
		return error;

	/* The session's scratch, made or grown (the old one unmapped once the session's work no longer reaches it). */
	if (session->partial_scratch == NULL || session->partial_scratch_bytes < memory->partial_plan.bytes) {
		if (session->partial_scratch != NULL) {
			powervr_resource_unmake(session, session->partial_scratch);
			session->partial_scratch = NULL;
			session->partial_scratch_bytes = 0;
		}
		error = powervr_buffer_create(render, memory->partial_plan.bytes, &buffer);
		if (error != 0)
			return error;
		error = powervr_resource_make(session, buffer, 1, 0, &session->partial_scratch);
		powervr_buffer_put(render, buffer);
		if (error != 0) {
			session->partial_scratch = NULL;
			return error;
		}
		session->partial_scratch_bytes = memory->partial_plan.bytes;
	}

	/* Succeeded: the job's scratch. */
	memory->partial_scratch = session->partial_scratch->address;
	memory->fragment_setup.scratch_buffer = true;
	return 0;
}

/*
 * Writes a partial render's programs: the end of tile that stores the
 * pixel output registers and each tile buffer to the scratch buffer's
 * regions (its emits' back end words kept for job_partial_command), and
 * the background that the 3D after it runs to load them back
 * (powervr_render_partial_load_usc, its shared words from the job's
 * general memory), as the fragment command's background after a partial
 * render.  A job without a scratch buffer runs its own background again.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_spm.c
 * (pvr_arch_spm_init_eot_state: the end of tile of the regions' emits,
 * a tile buffer's words loaded first, and its pixel event program;
 * pvr_arch_spm_init_bgobj_state: the load's constants by one DMA, the
 * background words of the load program) and pvr_arch_job_render.c
 * (pvr_render_job_ws_fragment_pr_init_based_on_fragment_state: the
 * partial render's command the fragment's with the scratch buffer's
 * back end words and end of tile).
 */
static int
job_partial_programs(
	const struct powervr_render_job *job,
	struct job_memory *memory,
	uint32_t pds_base,
	uint32_t usc_base,
	uint32_t *shared_registers)
{
	const struct powervr_render_partial *plan;
	struct powervr_render_background background;
	uint32_t states[POWERVR_RENDER_EMITS_MAX][2];
	uint64_t tile_addresses[POWERVR_RENDER_EMITS_MAX];
	uint32_t words[POWERVR_RENDER_PARTIAL_WORDS_MAX];
	uint32_t event[POWERVR_RENDER_EVENT_DATA_WORDS];
	uint32_t code_word;
	uint32_t region;
	uint32_t count;
	int error;

	/* Without a scratch buffer, the background again. */
	if (memory->partial_scratch == 0) {
		kern_memcpy(memory->fragment_setup.partial_background, memory->fragment_setup.background, sizeof(memory->fragment_setup.background));
		return 0;
	}
	plan = &memory->partial_plan;

	/* Each region's emit: the registers', then each tile buffer's after its words are loaded. */
	kern_memset(memory->partial_pbe, 0, sizeof(memory->partial_pbe));
	for (region = 0; region <= plan->buffers; region++) {
		error = powervr_render_partial_pbe(plan, memory->partial_scratch, region, states[region], memory->partial_pbe[region]);
		if (error != 0)
			return error;
		tile_addresses[region] = region == 0 ? 0 : memory->tile_buffer_addresses[region - 1U];
		if (region != 0 && tile_addresses[region] == 0)
			return EINVAL;
	}
	if (plan->buffers == 0)
		error = powervr_render_eot_usc_emits((const uint32_t (*)[2])states, 1, &memory->program);
	else
		error = powervr_render_eot_usc_tiles((const uint32_t (*)[2])states, tile_addresses, plan->buffers + 1U, plan->tile_words, &memory->program);
	if (error != 0)
		return error;

	put_bytes(memory->usc, USC_PARTIAL_EOT, memory->program.code, memory->program.length);
	error = powervr_render_event_data(usc_base + USC_PARTIAL_EOT, memory->program.temps, event);
	if (error != 0)
		return error;

	put_words(memory->pds, PDS_PARTIAL_EVENT, event, POWERVR_RENDER_EVENT_DATA_WORDS);
	error = powervr_render_event_words(pds_base + PDS_PARTIAL_EVENT, &code_word, &memory->partial_event_data, &memory->partial_event_info);
	if (error != 0)
		return error;

	/* The load: its program at the sample rate with four samples, its words and the program that copies them in. */
	error = powervr_render_partial_load_usc(plan, &memory->program);
	if (error != 0)
		return error;

	error = powervr_render_partial_words(plan, memory->partial_scratch, &tile_addresses[1], words, &count);
	if (error != 0)
		return error;

	put_words(memory->general->buffer, GENERAL_PARTIAL_WORDS, words, count);
	error = powervr_render_partial_shared(memory->general->address + GENERAL_PARTIAL_WORDS, count, &memory->shared);
	if (error != 0)
		return error;
	if (memory->shared.code_words * 4U > PDS_PARTIAL_SHARED_CODE_ROOM || memory->shared.data_words * 4U > PDS_PARTIAL_SHARED_DATA_ROOM)
		return E2BIG;

	put_bytes(memory->usc, USC_PARTIAL_BACKGROUND, memory->program.code, memory->program.length);
	error = powervr_render_fragment_kick_samples(usc_base + USC_PARTIAL_BACKGROUND, memory->program.temps, job->samples == 4U ? 4U : 1U, &memory->kick);
	if (error != 0)
		return error;

	put_words(memory->pds, PDS_PARTIAL_KICK, memory->kick.data, POWERVR_RENDER_KICK_DATA_WORDS);
	put_words(memory->pds, PDS_PARTIAL_KICK + POWERVR_RENDER_KICK_DATA_WORDS * 4U, memory->kick.code, POWERVR_RENDER_KICK_CODE_WORDS);
	put_words(memory->pds, PDS_PARTIAL_SHARED_CODE, memory->shared.code, memory->shared.code_words);
	put_words(memory->pds, PDS_PARTIAL_SHARED_DATA, memory->shared.data, memory->shared.data_words);

	/* The background words of the 3D after a partial render, and the shared registers it takes. */
	background.kick_offset = pds_base + PDS_PARTIAL_KICK;
	background.shared_code_offset = pds_base + PDS_PARTIAL_SHARED_CODE;
	background.shared_data_offset = pds_base + PDS_PARTIAL_SHARED_DATA;
	background.shared_data_words = memory->shared.data_words;
	background.shared_registers = count;
	error = powervr_render_background_words(&background, memory->fragment_setup.partial_background);
	if (error != 0)
		return error;
	if (count > *shared_registers)
		*shared_registers = count;

	/* Succeeded: the partial render's programs. */
	return 0;
}

/*
 * Builds the partial render's fragment command: the fragment's (its
 * backgrounds, tiles and depth), with the scratch buffer's emits and end
 * of tile, every tile processed (the background after a partial render
 * loads every tile from the scratch buffer); the fragment's own command
 * for a job without a scratch buffer.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_job_render.c
 * (pvr_render_job_ws_fragment_pr_init_based_on_fragment_state: the
 * fragment's state with the partial render's PBE words and pixel event
 * data) and pvr_arch_cmd_buffer.c (the job loading back from the
 * scratch buffer processes its empty tiles).
 */
static void
job_partial_command(
	struct job_memory *memory)
{
	struct powervr_fragment_setup setup;

	/* No scratch buffer: the fragment's command. */
	if (memory->partial_scratch == 0) {
		kern_memcpy(memory->partial, memory->fragment, POWERVR_FRAGMENT_BYTES);
		return;
	}

	/* The fragment's with the scratch buffer's emits and end of tile, over every tile. */
	setup = memory->fragment_setup;
	kern_memcpy(setup.pbe, memory->partial_pbe, sizeof(setup.pbe));
	setup.event_data = memory->partial_event_data;
	setup.event_info = memory->partial_event_info;
	setup.skip_empty_tiles = false;
	kern_memset(memory->partial, 0, POWERVR_FRAGMENT_BYTES);
	powervr_fragment_command(&setup, memory->partial);
}

/* Makes and maps the job's memory. */
static int
job_map(
	struct powervr_render *render,
	struct powervr_session *session,
	struct job_memory *memory)
{
	struct powervr_buffer *buffer;
	int error;

	/* Memory kept from a finished job is mapped already: only its old bytes go. */
	if (memory->general != NULL) {
		job_buffer_clear(memory->general->buffer);
		job_buffer_clear(memory->pds);
		job_buffer_clear(memory->usc);
		return 0;
	}

	/* The general memory. */
	error = powervr_buffer_create(render, GENERAL_BYTES, &buffer);
	if (error != 0)
		return error;

	error = powervr_resource_make(session, buffer, POWERVR_PROGRAM_SLC_CACHED ? 0 : 1, 0, &memory->general);
	powervr_buffer_put(render, buffer);
	if (error != 0) {
		memory->general = NULL;
		return error;
	}

	/* The program buffers, in their windows. */
	error = powervr_buffer_create(render, PDS_BYTES, &memory->pds);
	if (error != 0) {
		memory->pds = NULL;
		return error;
	}

	error = powervr_session_program_map(session, POWERVR_PROGRAM_PDS, memory->pds, &memory->pds_place);
	if (error != 0) {
		memory->pds_place = 0;
		return error;
	}

	error = powervr_buffer_create(render, USC_BYTES, &memory->usc);
	if (error != 0) {
		memory->usc = NULL;
		return error;
	}

	error = powervr_session_program_map(session, POWERVR_PROGRAM_USC, memory->usc, &memory->usc_place);
	if (error != 0) {
		memory->usc_place = 0;
		return error;
	}

	/* Succeeded: the memory is mapped. */
	return 0;
}

/*
 * Writes the job's programs: the end-of-tile program storing the colour
 * attachments over the render area, and the background (whose contents
 * do not matter for the colour attachments the load draws load,
 * load_mask).
 */
static int
job_programs(
	const struct powervr_render_job *job,
	const struct job_outputs *outputs,
	uint32_t load_mask,
	struct job_memory *memory,
	uint32_t *shared_registers)
{
	uint32_t states[POWERVR_RENDER_EMITS_MAX][2];
	uint32_t tiles[POWERVR_RENDER_EMITS_MAX];
	uint64_t tile_addresses[POWERVR_RENDER_EMITS_MAX];
	uint32_t event[POWERVR_RENDER_EVENT_DATA_WORDS];
	uint32_t pds_base;
	uint32_t usc_base;
	uint32_t code_word;
	uint32_t emits;
	uint32_t emit;
	uint32_t width;
	int error;

	/* The places as offsets from the windows' bases. */
	pds_base = (uint32_t)(memory->pds_place - POWERVR_PDS_HEAP_BASE);
	usc_base = (uint32_t)(memory->usc_place - POWERVR_USC_HEAP_BASE);

	/* The back end words of each emit over the render area, and the end-of-tile program that emits them. */
	error = job_emits(job, outputs, states, &emits, memory->fragment_setup.pbe, tiles);
	if (error != 0)
		return error;

	if (outputs->tile_buffers == 0) {
		error = powervr_render_eot_usc_emits((const uint32_t (*)[2])states, emits, &memory->program);
	} else {
		/* With tile buffers: each such emit's buffer loaded first, a pixel's registers as the fragment command sets them. */
		for (emit = 0; emit < emits; emit++) {
			tile_addresses[emit] = 0;
			if (tiles[emit] != POWERVR_SHADER_NO_REG)
				tile_addresses[emit] = memory->tile_buffer_addresses[tiles[emit]];
		}
		width = outputs->registers <= 2U ? 2U : 4U;
		error = powervr_render_eot_usc_tiles((const uint32_t (*)[2])states, tile_addresses, emits, width, &memory->program);
	}
	if (error != 0)
		return error;

	put_bytes(memory->usc, USC_EOT, memory->program.code, memory->program.length);
	error = powervr_render_event_data(usc_base + USC_EOT, memory->program.temps, event);
	if (error != 0)
		return error;

	put_words(memory->pds, PDS_EVENT, event, POWERVR_RENDER_EVENT_DATA_WORDS);
	error = powervr_render_event_words(pds_base + PDS_EVENT, &code_word, &memory->fragment_setup.event_data, &memory->fragment_setup.event_info);
	if (error != 0)
		return error;

	/* The pixel output registers a pixel takes. */
	memory->fragment_setup.pixel_registers = outputs->registers;

	/* The background. */
	error = job_background(job, outputs, load_mask, memory, pds_base, usc_base, shared_registers);
	if (error != 0)
		return error;

	/* The partial render's end of tile and the background of the 3D after it. */
	error = job_partial_programs(job, memory, pds_base, usc_base, shared_registers);
	if (error != 0)
		return error;

	/* Succeeded: the programs are written. */
	return 0;
}

/*
 * Writes the background: each colour attachment cleared to its clear
 * value for CLEAR and to zero where its contents do not matter (DONT_CARE,
 * or loaded by its load draw), each at its pixel output registers (one
 * in a tile buffer cleared there for CLEAR alone, job_background_tiles).  The
 * probes other than 0 (for the board) load or mark the first colour
 * attachment by the background alone.
 */
static int
job_background(
	const struct powervr_render_job *job,
	const struct job_outputs *outputs,
	uint32_t load_mask,
	struct job_memory *memory,
	uint32_t pds_base,
	uint32_t usc_base,
	uint32_t *shared_registers)
{
	const struct powervr_render_target *color;
	struct powervr_render_background background;
	struct powervr_data_load loads[POWERVR_RENDER_LOAD_SHARED_WORDS / 2U];
	struct powervr_render_area whole;
	uint32_t blit[POWERVR_TRANSFER_SHARED_WORDS];
	uint32_t words[POWERVR_RENDER_LOAD_SHARED_WORDS];
	uint32_t value[POWERVR_RENDER_CLEAR_WORDS_MAX];
	uint32_t clear_words;
	uint32_t index;
	uint32_t count;
	uint32_t word;
	uint32_t pair;
	bool probe_load;
	int error;

	/* The first colour attachment, which a probe other than 0 loads by the background. */
	color = &job->colors[0];
	probe_load = job->color_count != 0 && (outputs->used & 1U) != 0 && (load_mask & 1U) == 0 && color->load_op == LOAD_OP_LOAD;

	/* The background's program and the shared register program that feeds it. */
	if (probe_load && POWERVR_RENDER_LOAD_PROBE == LOAD_PROBE_BLIT) {
		/* The transfer's blit (coordinates from the position by a scale and an offset, filtered sample, packed), one to one over the attachment. */
		error = powervr_transfer_blit_usc(color->surface.format, &memory->program);
		if (error != 0)
			return error;

		/* Its words over the whole attachment, one to one. */
		whole.x = 0;
		whole.y = 0;
		whole.width = color->view.width;
		whole.height = color->view.height;
		error = powervr_transfer_blit_words(&color->view, &whole, &whole, false, false, false, blit);
		if (error != 0)
			return error;

		/* The words in the job's memory and the program that loads them. */
		put_words(memory->general->buffer, GENERAL_LOAD_WORDS, blit, POWERVR_TRANSFER_SHARED_WORDS);
		error = powervr_transfer_shared_program(memory->general->address + GENERAL_LOAD_WORDS, &memory->shared);
		if (error != 0)
			return error;

		/* The blit's shared words. */
		*shared_registers = POWERVR_TRANSFER_SHARED_WORDS;
	} else if (probe_load && POWERVR_RENDER_LOAD_PROBE != LOAD_PROBE_CONSTANT) {
		/* The attachment's image and sampler words, and the program that samples it. */
		error = powervr_render_load_usc_probe(color->surface.format, LOAD_USC_PROBE_FORM, &memory->program);
		if (error != 0)
			return error;

		error = powervr_render_load_words(&color->view, words);
		if (error != 0)
			return error;

		/* The diagnostics: where the load reads and its words. */
		if (POWERVR_DIAGNOSTICS) {
			kern_logf("powervr: render job: load from 0x%llx (%s, %ux%u, view mapping %u %u %u %u), image words %08x %08x %08x %08x, sampler %08x %08x\n",
				  (unsigned long long)color->view.address, color->view.linear ? "linear" : "twiddled",
				  (unsigned)color->view.width, (unsigned)color->view.height,
				  (unsigned)color->view.mapping[0], (unsigned)color->view.mapping[1], (unsigned)color->view.mapping[2], (unsigned)color->view.mapping[3],
				  (unsigned)words[0], (unsigned)words[1], (unsigned)words[2], (unsigned)words[3], (unsigned)words[4], (unsigned)words[5]);
		}

		/* The words in the job's memory, and the program that loads them (or writes them as constants for the WRITTEN probe). */
		put_words(memory->general->buffer, GENERAL_LOAD_WORDS, words, POWERVR_RENDER_LOAD_SHARED_WORDS);
		if (POWERVR_RENDER_LOAD_PROBE == LOAD_PROBE_WRITTEN) {
			/* The words as four pairs of constants. */
			for (pair = 0; pair < POWERVR_RENDER_LOAD_SHARED_WORDS / 2U; pair++) {
				loads[pair].kind = POWERVR_DATA_LOAD_WRITE;
				loads[pair].shared = (uint16_t)(pair * 2U);
				loads[pair].words = 2;
				loads[pair].source = (uint64_t)words[pair * 2U] | (uint64_t)words[pair * 2U + 1U] << 32;
			}

			error = powervr_data_shared_build(loads, POWERVR_RENDER_LOAD_SHARED_WORDS / 2U, &memory->shared);
		} else {
			error = powervr_render_load_shared(memory->general->address + GENERAL_LOAD_WORDS, &memory->shared);
		}

		/* Reports a program that could not be built. */
		if (error != 0)
			return error;

		*shared_registers = POWERVR_RENDER_LOAD_SHARED_WORDS;
	} else {
		/*
		 * Each attachment's clear words at its pixel output registers
		 * (powervr_format_output_words, kept by the job), zero for
		 * contents that do not matter, the constant probe's magenta
		 * (opaque, as a packed clear's word) for a first attachment
		 * the background loads; at least one word.
		 */
		kern_memset(value, 0, sizeof(value));
		count = outputs->registers;
		if (count == 0)
			count = 1;
		if (count > POWERVR_RENDER_CLEAR_WORDS_MAX)
			return ENOTSUP;
		for (index = 0; index < job->color_count; index++) {
			if ((outputs->used & (1U << index)) == 0 || outputs->tile_buffer[index] != POWERVR_SHADER_NO_REG)
				continue;
			if (job->colors[index].load_op == LOAD_OP_CLEAR) {
				clear_words = powervr_format_output_word_count(job->colors[index].surface.format);
				for (word = 0; word < clear_words && outputs->regs[index] + word < count; word++)
					value[outputs->regs[index] + word] = job->colors[index].clear[word];
			}
		}
		if (probe_load)
			value[outputs->regs[0]] = 0xffff00ffU;

		/* The program that writes the words, and the tile buffers' clears after them. */
		if (outputs->tile_buffers != 0) {
			error = job_background_tiles(job, outputs, value, count, memory, shared_registers);
			if (error != 0)
				return error;
		} else {
			error = powervr_render_clear_usc_words(count, &memory->program);
			if (error != 0)
				return error;

			error = powervr_render_clear_shared_words(value, count, &memory->shared);
			if (error != 0)
				return error;

			*shared_registers = count;
		}
	}

	/* The program, its task program and its shared register program. */
	if (memory->shared.code_words * 4U > PDS_SHARED_CODE_ROOM || memory->shared.data_words * 4U > PDS_SHARED_DATA_ROOM)
		return E2BIG;

	put_bytes(memory->usc, USC_BACKGROUND, memory->program.code, memory->program.length);
	error = powervr_render_kick_build(usc_base + USC_BACKGROUND, memory->program.temps, &memory->kick);
	if (error != 0)
		return error;

	put_words(memory->pds, PDS_KICK, memory->kick.data, POWERVR_RENDER_KICK_DATA_WORDS);
	put_words(memory->pds, PDS_KICK + POWERVR_RENDER_KICK_DATA_WORDS * 4U, memory->kick.code, POWERVR_RENDER_KICK_CODE_WORDS);
	put_words(memory->pds, PDS_SHARED_CODE, memory->shared.code, memory->shared.code_words);
	put_words(memory->pds, PDS_SHARED_DATA, memory->shared.data, memory->shared.data_words);

	/* The background's words. */
	background.kick_offset = pds_base + PDS_KICK;
	background.shared_code_offset = pds_base + PDS_SHARED_CODE;
	background.shared_data_offset = pds_base + PDS_SHARED_DATA;
	background.shared_data_words = memory->shared.data_words;
	background.shared_registers = *shared_registers;
	error = powervr_render_background_words(&background, memory->fragment_setup.background);
	if (error != 0)
		return error;

	/* Succeeded: the background is written. */
	return 0;
}

/*
 * Writes the background's programs of a job with tile buffers: the
 * pixel output registers' count words value (as the clear background
 * without them), then each attachment in a tile buffer that the job
 * clears stored with its clear value into the pixel's words of its
 * buffer (for the covered samples with four); those not cleared are
 * left as the buffer holds them (DONT_CARE, or loaded by their load
 * draw, which writes every pixel).  Gives the shared registers it takes.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_usc.c (pvr_uscgen_loadop: the
 * load op, the background object's program, stores each cleared render
 * target to its output registers or tile buffer).
 */
static int
job_background_tiles(
	const struct powervr_render_job *job,
	const struct job_outputs *outputs,
	const uint32_t *value,
	uint32_t count,
	struct job_memory *memory,
	uint32_t *shared_registers)
{
	struct powervr_render_tile_clear clears[POWERVR_PASS_COLORS];
	uint32_t clear_count;
	uint32_t index;
	uint32_t word;
	int error;

	/* Each cleared attachment in a tile buffer: its buffer, its first word, its clear words. */
	clear_count = 0;
	for (index = 0; index < job->color_count; index++) {
		if ((outputs->used & (1U << index)) == 0 || outputs->tile_buffer[index] == POWERVR_SHADER_NO_REG)
			continue;
		if (job->colors[index].load_op != LOAD_OP_CLEAR)
			continue;
		clears[clear_count].buffer = outputs->tile_buffer[index];
		clears[clear_count].offset = outputs->regs[index];
		clears[clear_count].words = powervr_format_output_word_count(job->colors[index].surface.format);
		if (clears[clear_count].words == 0 || clears[clear_count].words > POWERVR_RENDER_CLEAR_WORDS_MAX)
			return ENOTSUP;
		for (word = 0; word < POWERVR_RENDER_CLEAR_WORDS_MAX; word++)
			clears[clear_count].value[word] = job->colors[index].clear[word];
		clear_count++;
	}

	/* The program, then the shared register program of its words and the buffers' addresses. */
	error = powervr_render_clear_usc_tiles(count, clears, clear_count, job->samples == 4U ? 4U : 1U, &memory->program);
	if (error != 0)
		return error;

	error = powervr_render_clear_shared_tiles(value, count, clears, clear_count, memory->tile_buffer_addresses, &memory->shared, shared_registers);
	if (error != 0)
		return error;

	/* Succeeded: the background's programs. */
	return 0;
}

/*
 * Makes the draws' memory and places every draw's programs, tables and
 * stream: the shading cluster code of each draw's two shaders, then for
 * each draw its slot of data sequencer programs and general memory, its
 * scissor and its part of the stream.  Reports the stream's words and
 * the most shared registers a vertex shader takes.
 */
static int
job_draws(
	struct powervr_render *render,
	struct powervr_session *session,
	const struct powervr_render_job *job,
	const struct powervr_render_draw *prefix,
	uint32_t prefix_count,
	struct job_memory *memory,
	uint32_t *stream_words,
	uint32_t *vertex_shareds)
{
	const struct powervr_vk_graphics *graphics;
	const struct powervr_vk_descriptor_set *set;
	const struct powervr_render_draw *draw;
	uint64_t sets_bytes;
	uint64_t usc_bytes;
	uint64_t sets_place;
	uint64_t usc_place;
	uint64_t general_bytes;
	uint64_t pds_bytes;
	uint64_t usc_total;
	uint32_t draw_count;
	uint32_t index;
	uint64_t part_started;
	uint32_t code;
	uint32_t slot;
	uint32_t words;
	bool clears;
	bool fits;
	int error;

	/* The job's draws after the prefix draws, when there are any. */
	draw_count = job->draw_count + prefix_count;

	/* No depth bias in the job's table yet. */
	memory->dbias_count = 0;

	/* Each draw's pipeline: its own, or for a clear draw the one made for its shape. */
	clears = false;
	for (index = 0; index < draw_count; index++) {
		draw = job_draw(job, prefix, prefix_count, index);
		memory->graphics[index] = draw->pipeline;
		if (draw->clear_mask == 0)
			continue;

		/* A clear draw's pipeline, found or compiled under the core's lock as the load draw's. */
		mutex_lock(&render->core->lock);

		error = clear_graphics(render, job, draw->clear_mask, &memory->graphics[index]);

		mutex_unlock(&render->core->lock);

		if (error != 0)
			return error;
		clears = true;
	}

	/* The clear draws' triangles, at the place the load draw's are. */
	if (clears)
		put_words(memory->general->buffer, GENERAL_LOAD_WORDS, load_draw_vertices, LOAD_DRAW_VERTICES * 4U);

	/* What the draws take: their pipelines' code once each and their sets' copies, each on 16 bytes. */
	sets_bytes = 0;
	usc_bytes = 0;
	memory->code_pipeline_count = 0;
	for (index = 0; index < draw_count; index++) {
		draw = job_draw(job, prefix, prefix_count, index);
		graphics = memory->graphics[index];
		if (graphics == NULL)
			return EINVAL;

		/* The pipeline's code where an earlier draw's put it, else after the code so far. */
		for (code = 0; code < memory->code_pipeline_count; code++) {
			if (memory->code_pipelines[code] == graphics)
				break;
		}
		if (code == memory->code_pipeline_count) {
			memory->code_pipelines[code] = graphics;
			memory->code_pipeline_places[code] = usc_bytes;
			memory->code_pipeline_count++;
			usc_bytes += (graphics->vertex.length + 15U) & ~(uint64_t)15U;
			usc_bytes += (graphics->fragment.length + 15U) & ~(uint64_t)15U;
		}
		memory->code_places[index] = memory->code_pipeline_places[code];
		for (slot = 0; slot < POWERVR_DRAW_SETS; slot++) {
			set = draw->sets[slot];
			if (set != NULL)
				sets_bytes += (set->layout.bytes + 15U) & ~15U;
		}
	}

	/* The draws' memory kept from a finished job, when it is large enough; else made anew. */
	general_bytes = (uint64_t)draw_count * DRAW_SLOT + sets_bytes + 16U;
	pds_bytes = (uint64_t)draw_count * DRAW_SLOT;
	usc_total = usc_bytes + 16U;
	part_started = powervr_timing_now();
	fits = job_draw_memory_fits(memory, general_bytes, pds_bytes, usc_total);
	if (!fits)
		fits = job_draw_memory_swap(render, session, memory, general_bytes, pds_bytes, usc_total);
	if (!fits) {
		job_draw_memory_free(render, session, memory);
		error = job_draw_memory_make(render, session, memory, general_bytes, pds_bytes, usc_total);
		if (error != 0)
			return error;
	}

	/* Each pipeline's two shaders' code, once (the memory's and the code's times apart). */
	powervr_timing_add(POWERVR_TIMING_DRAW_MEMORY, part_started);
	part_started = powervr_timing_now();
	for (code = 0; code < memory->code_pipeline_count; code++) {
		graphics = memory->code_pipelines[code];
		usc_place = memory->code_pipeline_places[code];
		put_bytes(memory->draw_usc, (uint32_t)usc_place, graphics->vertex_code, graphics->vertex.length);
		put_bytes(memory->draw_usc, (uint32_t)(usc_place + ((graphics->vertex.length + 15U) & ~(size_t)15U)), graphics->fragment_code, graphics->fragment.length);
	}

	powervr_timing_add(POWERVR_TIMING_DRAW_CODE, part_started);

	/* Each draw in order, its stream after the one before, its code where its pipeline's is. */
	*stream_words = 0;
	*vertex_shareds = 0;
	sets_place = (uint64_t)draw_count * DRAW_SLOT;
	for (index = 0; index < draw_count; index++) {
		draw = job_draw(job, prefix, prefix_count, index);
		graphics = memory->graphics[index];
		error = draw_place(job, draw, index, memory, sets_place, memory->code_places[index], *stream_words, &words);
		if (error != 0)
			return error;

		*stream_words += words;
		if (graphics->vertex.shareds > *vertex_shareds)
			*vertex_shareds = graphics->vertex.shareds;
		for (slot = 0; slot < POWERVR_DRAW_SETS; slot++) {
			set = draw->sets[slot];
			if (set != NULL)
				sets_place += (set->layout.bytes + 15U) & ~15U;
		}
	}

	/* Succeeded: every draw is placed. */
	return 0;
}

/*
 * Places one draw: its sets' and push constants' copies, its vertex,
 * shared register, task and coefficient programs (pointing at its
 * pipeline's code at usc_place, which job_draws put there once), its
 * scissor, its primitive state block and its part of the stream.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_emit_dirty_pds_state, pvr_emit_dirty_ppp_state,
 * pvr_emit_vdm_index_list: the descriptor programs of both stages, the
 * fragment's task and coefficient programs in the primitive state, the
 * vertex program and the index list in the stream); zedBSD
 * src/drivers/gpu/powervr/render-check.c (the same placing for the
 * kernel's own triangle).
 */
static int
draw_place(
	const struct powervr_render_job *job,
	const struct powervr_render_draw *draw,
	uint32_t index,
	struct job_memory *memory,
	uint64_t sets_place,
	uint64_t usc_place,
	uint32_t stream_at,
	uint32_t *words)
{
	const struct powervr_vk_graphics *graphics;
	const struct powervr_vk_descriptor_set *set;
	const struct powervr_draw_attribute *attribute;
	const struct powervr_shader_input *input;
	const VkViewport *draw_viewport;
	VkViewport clear_viewport;
	uint32_t dma_words;
	uint32_t defaults;
	uint32_t bits;
	const VkRect2D *draw_scissor;
	struct powervr_data_state state;
	struct powervr_draw_ppp ppp;
	struct powervr_draw_vdm vdm;
	uint32_t viewport[POWERVR_DRAW_VIEWPORT_WORDS];
	uint32_t scissor[POWERVR_DRAW_SCISSOR_WORDS];
	uint64_t general;
	uint64_t task;
	uint32_t pds_base;
	uint32_t usc_base;
	uint32_t pds_slot;
	uint32_t vertex_usc;
	uint32_t fragment_usc;
	uint32_t vertex_shared_words;
	uint32_t fragment_shared_words;
	uint32_t ppp_words;
	uint32_t dma_count;
	uint32_t dynamic_taken;
	uint32_t slot;
	uint32_t at;
	int error;

	/* Where the draw's parts go. */
	graphics = memory->graphics[index];
	general = memory->draw_general->address + (uint64_t)index * DRAW_SLOT;
	pds_slot = index * DRAW_SLOT;

	/* A client's rasterising pipeline has the job's samples (the job's 0 taken as 1). */
	if (draw->clear_mask == 0 && !graphics->fixed.rasterizer_discard && graphics->fixed.samples != 0U &&
	    graphics->fixed.samples != (job->samples == 4U ? 4U : 1U))
		return EINVAL;
	pds_base = (uint32_t)(memory->draw_pds_place - POWERVR_PDS_HEAP_BASE) + pds_slot;
	usc_base = (uint32_t)(memory->draw_usc_place - POWERVR_USC_HEAP_BASE);
	vertex_usc = usc_base + (uint32_t)usc_place;
	fragment_usc = vertex_usc + (uint32_t)((graphics->vertex.length + 15U) & ~(size_t)15U);

	/* The bound sets' copies, each dynamic descriptor moved on by its offset. */
	kern_memset(&state, 0, sizeof(state));
	at = (uint32_t)sets_place;
	dynamic_taken = 0;
	for (slot = 0; slot < POWERVR_DRAW_SETS; slot++) {
		set = draw->sets[slot];
		if (set == NULL)
			continue;
		put_bytes(memory->draw_general->buffer, at, set->bytes, set->layout.bytes);
		error = draw_set_offsets(draw, set, memory, at, &dynamic_taken);
		if (error != 0)
			return error;
		state.layouts[slot] = &set->layout;
		state.set_addresses[slot] = memory->draw_general->address + at;
		at += (set->layout.bytes + 15U) & ~15U;
	}

	/* The push constants, and the blend constants: the command buffer's when the pipeline made them dynamic, else the pipeline's. */
	put_bytes(memory->draw_general->buffer, index * DRAW_SLOT + DRAW_PUSH, draw->push, draw->push_bytes);
	if (graphics->fixed.dynamic_blend_constants)
		put_words(memory->draw_general->buffer, index * DRAW_SLOT + DRAW_BLEND, draw->dynamic.blend_constants, 4U);
	else
		put_words(memory->draw_general->buffer, index * DRAW_SLOT + DRAW_BLEND, graphics->fixed.blend_constants, 4U);
	state.push_address = general + DRAW_PUSH;
	state.blend_address = general + DRAW_BLEND;
	kern_memcpy(state.tile_buffer_addresses, memory->tile_buffer_addresses, sizeof(state.tile_buffer_addresses));
	state.view = job->view;

	/* The two shared register programs. */
	error = draw_shared(&graphics->vertex, vertex_usc, &state, memory, pds_slot + DRAW_PDS_VSHARED_CODE, pds_slot + DRAW_PDS_VSHARED_DATA, &vertex_shared_words);
	if (error != 0)
		return error;

	error = draw_shared(&graphics->fragment, fragment_usc, &state, memory, pds_slot + DRAW_PDS_FSHARED_CODE, pds_slot + DRAW_PDS_FSHARED_DATA, &fragment_shared_words);
	if (error != 0)
		return error;

	/* The vertex program: a DMA per attribute into its input registers, the defaults (0, 0, 0, 1.0) beside them. */
	put_bytes(memory->draw_general->buffer, index * DRAW_SLOT + DRAW_DEFAULTS, attribute_defaults, sizeof(attribute_defaults));
	dma_count = 0;
	for (slot = 0; slot < graphics->fixed.attribute_count; slot++) {
		attribute = &graphics->fixed.attributes[slot];
		input = NULL;
		for (at = 0; at < graphics->vertex.input_count; at++) {
			if (graphics->vertex.inputs[at].location == attribute->location)
				input = &graphics->vertex.inputs[at];
		}
		if (input == NULL)
			continue;
		if (attribute->binding >= POWERVR_DRAW_VERTEX_BUFFERS)
			return ENOTSUP;
		memory->dmas[dma_count].address = draw->vertex_addresses[attribute->binding] + attribute->offset;
		if (draw->clear_mask != 0)
			memory->dmas[dma_count].address = memory->general->address + GENERAL_LOAD_WORDS + attribute->offset;
		memory->dmas[dma_count].stride = 0;
		memory->dmas[dma_count].instance = false;
		for (at = 0; at < graphics->fixed.binding_count; at++) {
			if (graphics->fixed.bindings[at].binding != attribute->binding)
				continue;
			memory->dmas[dma_count].stride = graphics->fixed.bindings[at].stride;
			memory->dmas[dma_count].instance = graphics->fixed.bindings[at].instance;
		}

		/* An attribute stepped per instance starts at the draw's first instance. */
		if (memory->dmas[dma_count].instance)
			memory->dmas[dma_count].address += (uint64_t)draw->first_instance * memory->dmas[dma_count].stride;

		error = attribute_words(attribute->format, input->components, &dma_words, &defaults);
		if (error != 0)
			return error;

		memory->dmas[dma_count].words = dma_words;
		memory->dmas[dma_count].reg = input->reg;
		dma_count++;

		/*
		 * Procedure referenced: Mesa 25.0.7 src/imagination/vulkan/pvr_pipeline.c
		 * (the vertex DMA's size_in_dwords is the format's block bits / 32)
		 * and the Vulkan 1.0 specification, "Vertex Input Extraction" (a
		 * format with fewer components than the shader's input gives 0
		 * for G and B and 1 for A) -- the components past the format's
		 * are a stride-0 load from the slot's defaults.
		 */
		if (dma_words < input->components && defaults != ATTRIBUTE_NO_DEFAULTS) {
			if (dma_count == POWERVR_DATA_VERTEX_DMAS_MAX)
				return ENOTSUP;
			memory->dmas[dma_count].address = general + DRAW_DEFAULTS + defaults + dma_words * 4U;
			memory->dmas[dma_count].stride = 0;
			memory->dmas[dma_count].instance = false;
			memory->dmas[dma_count].words = input->components - dma_words;
			memory->dmas[dma_count].reg = input->reg + dma_words;
			dma_count++;
		}
	}
	/*
	 * The instance index the vertex program writes counts from 0, so a
	 * shader that reads it cannot see a first instance other than 0
	 * (the base instance is not added here).
	 */
	if (draw->first_instance != 0 && graphics->vertex.instance_index_reg != POWERVR_SHADER_NO_REG)
		return ENOTSUP;

	error = powervr_pds_task_word(vertex_usc, graphics->vertex.temps, 0, 0, &task);
	if (error != 0)
		return error;

	error = powervr_data_vertex_build(memory->dmas, dma_count, graphics->vertex.vertex_index_reg, graphics->vertex.instance_index_reg, task, &memory->vertex);
	if (error != 0)
		return error;

	put_words(memory->draw_pds, pds_slot + DRAW_PDS_VERTEX_DATA, memory->vertex.data, memory->vertex.data_words);
	put_words(memory->draw_pds, pds_slot + DRAW_PDS_VERTEX_CODE, memory->vertex.code, memory->vertex.code_words);

	/* The fragment's task program, and the coefficient program: data, then code after it. */
	error = powervr_render_fragment_kick_samples(fragment_usc, graphics->fragment.temps, job->samples, &memory->kick);
	if (error != 0)
		return error;

	put_words(memory->draw_pds, pds_slot + DRAW_PDS_KICK, memory->kick.data, POWERVR_RENDER_KICK_DATA_WORDS);
	put_words(memory->draw_pds, pds_slot + DRAW_PDS_KICK + POWERVR_RENDER_KICK_DATA_WORDS * 4U, memory->kick.code, POWERVR_RENDER_KICK_CODE_WORDS);
	put_words(memory->draw_pds, pds_slot + DRAW_PDS_COEFFICIENT, graphics->varyings.data, graphics->varyings.data_words);
	put_words(memory->draw_pds, pds_slot + DRAW_PDS_COEFFICIENT + (graphics->varyings.data_words + 3U) / 4U * 16U, graphics->varyings.code, graphics->varyings.code_words);

	/*
	 * The viewport and the scissor, the scissor in its entry of the
	 * table: the pipeline's own unless it made them dynamic, then the
	 * ones the command buffer set.
	 */
	draw_viewport = &draw->viewport;
	if (!graphics->fixed.dynamic_viewport)
		draw_viewport = &graphics->fixed.viewport;
	draw_scissor = &draw->scissor;
	if (!graphics->fixed.dynamic_scissor)
		draw_scissor = &graphics->fixed.scissor;

	/* A clear draw's viewport is the whole framebuffer, depth 0 to 1. */
	if (draw->clear_mask != 0) {
		kern_memset(&clear_viewport, 0, sizeof(clear_viewport));
		bits = float_bits(job->width);
		kern_memcpy(&clear_viewport.width, &bits, sizeof(bits));
		bits = float_bits(job->height);
		kern_memcpy(&clear_viewport.height, &bits, sizeof(bits));
		bits = float_bits(1);
		kern_memcpy(&clear_viewport.maxDepth, &bits, sizeof(bits));
		draw_viewport = &clear_viewport;
	}

	error = powervr_draw_viewport_words(draw_viewport, viewport);
	if (error != 0)
		return error;

	kern_memset(&ppp, 0, sizeof(ppp));
	error = powervr_draw_scissor_words(draw_viewport, draw_scissor, scissor, ppp.region_clip);
	if (error != 0)
		return error;

	put_words(memory->general->buffer, GENERAL_SCISSORS + index * POWERVR_DRAW_SCISSOR_WORDS * 4U, scissor, POWERVR_DRAW_SCISSOR_WORDS);

	/* The primitive state block. */
	ppp.fixed = &graphics->fixed;
	ppp.varyings = &graphics->varyings;
	ppp.has_depth = job->depth.present;
	ppp.fragment_kick_offset = pds_base + DRAW_PDS_KICK;
	ppp.shared_code_offset = pds_base + DRAW_PDS_FSHARED_CODE;
	ppp.shared_data_offset = pds_base + DRAW_PDS_FSHARED_DATA;
	ppp.shared_data_words = fragment_shared_words;
	ppp.fragment_shareds = graphics->fragment.shareds;
	ppp.coefficient_offset = pds_base + DRAW_PDS_COEFFICIENT;
	kern_memcpy(ppp.viewport, viewport, sizeof(viewport));
	ppp.scissor_index = index;
	ppp.has_stencil = job->depth.present && job->depth.format != NULL && format_has_stencil(job->depth.format->vk_format);
	ppp.dynamic = &draw->dynamic;
	ppp.visibility = draw->visibility;
	if (graphics->fixed.depth_bias && job->depth.present) {
		error = draw_depth_bias(job, graphics, draw, memory, &ppp.depth_bias_index);
		if (error != 0)
			return error;
	}
	error = powervr_draw_ppp_build(&ppp, memory->ppp, &ppp_words);
	if (error != 0)
		return error;

	put_words(memory->draw_general->buffer, index * DRAW_SLOT + DRAW_PPP, memory->ppp, ppp_words);

	/* The stream: the block, the vertex program and the index list. */
	kern_memset(&vdm, 0, sizeof(vdm));
	vdm.ppp_address = general + DRAW_PPP;
	vdm.ppp_words = ppp_words;
	vdm.vertex_data_offset = pds_base + DRAW_PDS_VERTEX_DATA;
	vdm.vertex_code_offset = pds_base + DRAW_PDS_VERTEX_CODE;
	vdm.vertex_data_words = memory->vertex.data_words;
	vdm.vertex_temps = memory->vertex.temps;
	vdm.shared_code_offset = pds_base + DRAW_PDS_VSHARED_CODE;
	vdm.shared_data_offset = pds_base + DRAW_PDS_VSHARED_DATA;
	vdm.shared_data_words = vertex_shared_words;
	vdm.vertex_shareds = graphics->vertex.shareds;
	vdm.vertex_inputs = graphics->vertex.vertex_inputs;
	vdm.vertex_outputs = graphics->vertex.vertex_outputs;
	vdm.topology = graphics->fixed.topology;
	vdm.indexed = draw->indexed;
	vdm.index_address = draw->index_address;
	vdm.index_bytes = draw->index_bytes;
	vdm.count = draw->count;
	vdm.instances = draw->instances;
	vdm.vertex_offset = draw->vertex_offset;
	error = powervr_draw_vdm_build(&vdm, memory->vdm, words);
	if (error != 0)
		return error;

	put_words(memory->general->buffer, GENERAL_STREAM + stream_at * 4U, memory->vdm, *words);

	/* Succeeded: the draw is placed. */
	return 0;
}

/*
 * Moves each dynamic descriptor of a set's copy (placed at at in the
 * draws' general buffer) on by the draw's next dynamic offset: the
 * address the set holds plus the offset, the range as it is.  *taken
 * counts the draw's offsets used by the sets before this one.  Refuses
 * a draw that carries fewer offsets than its sets have dynamic
 * descriptors (EINVAL).
 *
 * Procedure referenced: Khronos Vulkan 1.3 specification, "Descriptor
 * Set Binding" (a dynamic buffer's effective offset is the descriptor's
 * offset plus the dynamic offset).
 */
static int
draw_set_offsets(
	const struct powervr_render_draw *draw,
	const struct powervr_vk_descriptor_set *set,
	struct job_memory *memory,
	uint32_t at,
	uint32_t *taken)
{
	uint8_t moved[8];
	uint64_t address;
	uint32_t count;
	uint32_t index;
	uint32_t place;
	uint32_t byte;
	int error;

	/* The set's dynamic descriptors, each with an offset of the draw's. */
	count = powervr_set_dynamic_count(&set->layout);
	if (*taken + count > draw->dynamic_offset_count)
		return EINVAL;

	/* Each one's address, read from the set, moved on and written into the copy. */
	for (index = 0; index < count; index++) {
		error = powervr_set_dynamic_place(&set->layout, index, &place);
		if (error != 0)
			return error;

		address = 0;
		for (byte = 0; byte < 8U; byte++)
			address |= (uint64_t)set->bytes[place + byte] << (byte * 8U);
		address += draw->dynamic_offsets[*taken + index];
		for (byte = 0; byte < 8U; byte++)
			moved[byte] = (uint8_t)(address >> (byte * 8U));
		put_bytes(memory->draw_general->buffer, at + place, moved, sizeof(moved));
	}

	/* Succeeded: the offsets are used. */
	*taken += count;
	return 0;
}

/*
 * Finds or adds a draw's depth bias in the job's table and reports its
 * index: the pipeline's factors, or the command buffer's where it made
 * them dynamic, the constant factor scaled for the depth format.
 * Refuses more different biases than the table holds (E2BIG).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_setup_isp_depth_bias_scissor_state: an entry of the constant
 * factor, the slope factor and the clamp, its index in ISPDBSC;
 * pvr_calculate_final_depth_bias_contant_factor with ERN 42307, which the
 * BXE-4-32 has: a D16_UNORM constant divided by 2^15, a D32_SFLOAT one as
 * it is; Mesa 26.2.6: a D24_UNORM_S8_UINT one divided by 2^23).
 */
static int
draw_depth_bias(
	const struct powervr_render_job *job,
	const struct powervr_vk_graphics *graphics,
	const struct powervr_render_draw *draw,
	struct job_memory *memory,
	uint32_t *index)
{
	uint32_t entry[3];
	uint32_t found;

	/* The factors as the pipeline or the command buffer gives them: constant, clamp, slope. */
	if (graphics->fixed.dynamic_depth_bias) {
		entry[0] = draw->dynamic.depth_bias[0];
		entry[1] = draw->dynamic.depth_bias[2];
		entry[2] = draw->dynamic.depth_bias[1];
	} else {
		entry[0] = graphics->fixed.depth_bias_factors[0];
		entry[1] = graphics->fixed.depth_bias_factors[2];
		entry[2] = graphics->fixed.depth_bias_factors[1];
	}

	/* The constant in the depth format's units (the 24-bit depth's: Mesa's division by 2^23). */
	if (job->depth.format != NULL && job->depth.format->vk_format == VK_FORMAT_D16_UNORM)
		entry[0] = float_scaled_down(entry[0], 15);
	if (job->depth.format != NULL && job->depth.format->vk_format == VK_FORMAT_D24_UNORM_S8_UINT)
		entry[0] = float_scaled_down(entry[0], 23);

	/* The same bias already in the table. */
	for (found = 0; found < memory->dbias_count; found++) {
		if (memory->dbias[found][0] == entry[0] && memory->dbias[found][1] == entry[1] && memory->dbias[found][2] == entry[2]) {
			*index = found;
			return 0;
		}
	}

	/* A new entry, while the table has room. */
	if (memory->dbias_count == DBIAS_ENTRIES)
		return E2BIG;
	found = memory->dbias_count;
	memory->dbias[found][0] = entry[0];
	memory->dbias[found][1] = entry[1];
	memory->dbias[found][2] = entry[2];
	memory->dbias_count++;
	put_words(memory->general->buffer, GENERAL_DBIAS + found * DBIAS_ENTRY_BYTES, entry, 3U);

	/* Succeeded: the entry's index. */
	*index = found;
	return 0;
}

/* Whether a depth format has a stencil aspect. */
static bool
format_has_stencil(
	uint32_t vk_format)
{
	/* The formats with stencil. */
	switch (vk_format) {
	case VK_FORMAT_S8_UINT:
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return true;
	default:
		break;
	}

	/* A depth alone. */
	return false;
}

/*
 * Divides a float, as its bits, by 2 to a power without the processor's
 * floating point: the exponent made smaller, a result too small for a
 * normal float flushed to zero of the same sign, zero, an infinity and a
 * NaN left as they are.
 */
static uint32_t
float_scaled_down(
	uint32_t bits,
	uint32_t power)
{
	uint32_t exponent;

	/* Zero, a denormal, an infinity or a NaN as it is (a denormal to zero). */
	exponent = (bits >> 23) & 0xffU;
	if (exponent == 0)
		return bits & 0x80000000U;
	if (exponent == 0xffU)
		return bits;

	/* Too small for a normal float after the division: zero of the sign. */
	if (exponent <= power)
		return bits & 0x80000000U;

	/* Succeeded: the smaller exponent. */
	return (bits & ~(0xffU << 23)) | ((exponent - power) << 23);
}

/*
 * Builds and places a shader's shared register program from the bound
 * state; one halt when it takes no shared registers.  A program with a
 * shared program (its code at usc, the program's place in the shader
 * heap, plus shared_offset) has it issued after the loads.
 */
static int
draw_shared(
	const struct powervr_shader_program *program,
	uint32_t usc,
	const struct powervr_data_state *state,
	struct job_memory *memory,
	uint32_t code_at,
	uint32_t data_at,
	uint32_t *data_words)
{
	const uint64_t *issue;
	uint64_t task;
	uint32_t halt;
	uint32_t count;
	int error;

	/* No request: a program that only ends. */
	*data_words = 0;
	if (program->request_count == 0) {
		error = powervr_pds_halt(0, &halt);
		if (error != 0)
			return error;

		put_words(memory->draw_pds, code_at, &halt, 1);
		return 0;
	}

	/* The loads the requests ask for. */
	error = powervr_data_resolve(program, state, memory->loads, POWERVR_DATA_LOADS_MAX, &count);
	if (error != 0)
		return error;

	/* The shared program's task word: its code, its temporaries, one instance a task. */
	issue = NULL;
	if (program->shared_length != 0) {
		error = powervr_pds_task_word(usc + (uint32_t)program->shared_offset, program->shared_temps, 0, 0, &task);
		if (error != 0)
			return error;
		issue = &task;
	}

	/* The program, its code and data at their places. */
	error = powervr_data_shared_build_task(memory->loads, count, issue, &memory->shared);
	if (error != 0)
		return error;

	put_words(memory->draw_pds, code_at, memory->shared.code, memory->shared.code_words);
	put_words(memory->draw_pds, data_at, memory->shared.data, memory->shared.data_words);
	*data_words = memory->shared.data_words;

	/* Succeeded: the program is placed. */
	return 0;
}

/*
 * Gives a job's memory back once its 3D finished, or when it was never
 * kicked: kept mapped for the session's next job while there is room and
 * the session and the core are well, else freed.
 */
static void
job_release(
	struct powervr_render *render,
	struct powervr_session *session,
	struct job_memory *memory)
{
	unsigned long enabled;
	bool kept;

	/* Only whole memory of a well session on a well core is kept. */
	kept = false;
	if (memory->general != NULL &&
	    memory->pds != NULL &&
	    memory->usc != NULL &&
	    memory->pds_place != 0U &&
	    memory->usc_place != 0U) {
		enabled = spin_lock_irqsave(&render->queue_lock);

		if (!render->faulted &&
		    !session->lost &&
		    !session->isolated &&
		    session->job_keep_count < POWERVR_JOB_KEEP) {
			session->job_keep[session->job_keep_count] = memory;
			session->job_keep_count++;
			kept = true;
		}

		spin_unlock_irqrestore(&render->queue_lock, enabled);
	}

	/* Not kept: unmapped and freed. */
	if (!kept)
		job_free(render, session, memory);
}

/* Unmaps and frees a job's memory and programs. */
static void
job_free(
	struct powervr_render *render,
	struct powervr_session *session,
	struct job_memory *memory)
{
	/* The program buffers. */
	if (memory->usc != NULL) {
		if (memory->usc_place != 0U)
			powervr_session_program_unmap(session, POWERVR_PROGRAM_USC, memory->usc, memory->usc_place);
		powervr_buffer_put(render, memory->usc);
	}

	if (memory->pds != NULL) {
		if (memory->pds_place != 0U)
			powervr_session_program_unmap(session, POWERVR_PROGRAM_PDS, memory->pds, memory->pds_place);
		powervr_buffer_put(render, memory->pds);
	}

	/* The draws' program buffers and memory. */
	job_draw_memory_free(render, session, memory);

	/* The general memory and the bookkeeping. */
	if (memory->general != NULL)
		powervr_resource_unmake(session, memory->general);
	kern_free(memory);
}

/*
 * The words a vertex attribute's DMA loads, the format's bytes but no
 * more than the shader's input takes, and where its defaults are: a
 * format of 32-bit channels lacks the components past its own (the
 * defaults fill them); a packed one's words are the shader's to unpack,
 * so only 32-bit-channel formats take defaults.
 */
static int
attribute_words(
	uint32_t format,
	uint32_t components,
	uint32_t *words,
	uint32_t *defaults)
{
	const struct powervr_format *found;
	uint32_t format_words;

	/* A format the table does not know: the shader's input, as the pipeline let it through. */
	found = powervr_format_find(format);
	*defaults = ATTRIBUTE_NO_DEFAULTS;
	if (found == NULL || found->texel_bytes == 0) {
		*words = components;
		return 0;
	}

	/* The format's bytes in whole words; the defaults of an integer 32-bit-channel format after the float ones. */
	format_words = ((uint32_t)found->texel_bytes + 3U) / 4U;
	if (format >= VK_FORMAT_R32_UINT && format <= VK_FORMAT_R32G32B32A32_SFLOAT) {
		*defaults = 0;
		if (format != VK_FORMAT_R32_SFLOAT && format != VK_FORMAT_R32G32_SFLOAT && format != VK_FORMAT_R32G32B32_SFLOAT && format != VK_FORMAT_R32G32B32A32_SFLOAT)
			*defaults = 16;
	}
	*words = format_words < components ? format_words : components;

	/* Succeeded: the words and the defaults' offset. */
	return 0;
}

/*
 * Writes words through a buffer's uncached view, little-endian: two words
 * a store where the place is a doubleword's, else a word a store where it
 * is a word's (each uncached store is a bus write, so narrower stores
 * cost more), else a byte at a time.
 */
static void
put_words(
	struct powervr_buffer *buffer,
	uint32_t offset,
	const uint32_t *words,
	unsigned count)
{
	volatile uint64_t *pairs;
	volatile uint32_t *place;
	unsigned index;

	/* A place of whole words: a word up to a doubleword's place, then pairs, then the word left. */
	if ((offset & 3U) == 0U) {
		place = (volatile uint32_t *)(buffer->view + offset);
		index = 0;
		if ((offset & 7U) != 0U && count != 0) {
			place[0] = words[0];
			index = 1;
		}
		pairs = (volatile uint64_t *)(buffer->view + offset + index * 4U);
		for (; index + 2U <= count; index += 2U) {
			*pairs = (uint64_t)words[index] | (uint64_t)words[index + 1U] << 32;
			pairs++;
		}
		if (index < count)
			place[index] = words[index];
		return;
	}

	/* Otherwise one byte at a time, low byte first. */
	for (index = 0; index < count; index++) {
		buffer->view[offset + index * 4U + 0U] = (uint8_t)words[index];
		buffer->view[offset + index * 4U + 1U] = (uint8_t)(words[index] >> 8);
		buffer->view[offset + index * 4U + 2U] = (uint8_t)(words[index] >> 16);
		buffer->view[offset + index * 4U + 3U] = (uint8_t)(words[index] >> 24);
	}
}

/*
 * Writes bytes through a buffer's uncached view: the bytes up to a
 * doubleword's place one at a time, then whole doublewords assembled
 * from them (one store each), then the bytes left.
 */
static void
put_bytes(
	struct powervr_buffer *buffer,
	uint32_t offset,
	const uint8_t *bytes,
	size_t count)
{
	volatile uint64_t *place;
	uint64_t pair;
	size_t index;
	unsigned shift;

	/* The bytes before a doubleword's place. */
	index = 0;
	while (index < count && ((offset + index) & 7U) != 0U) {
		buffer->view[offset + index] = bytes[index];
		index++;
	}

	/* Whole doublewords, little-endian. */
	place = (volatile uint64_t *)(buffer->view + offset + index);
	while (index + 8U <= count) {
		pair = 0;
		for (shift = 0; shift < 8U; shift++)
			pair |= (uint64_t)bytes[index + shift] << (shift * 8U);
		*place = pair;
		place++;
		index += 8U;
	}

	/* The bytes left. */
	while (index < count) {
		buffer->view[offset + index] = bytes[index];
		index++;
	}
}

/*
 * Whether a job's geometry meets what an image transfer touched: a vertex
 * shader that samples an image (or names any descriptor other than a
 * buffer's address) or stores to memory.
 */
static bool
job_geometry_meets_images(
	const struct powervr_render_job *job)
{
	const struct powervr_vk_graphics *graphics;
	const struct powervr_shader_request *request;
	uint32_t draw;
	uint32_t index;

	/* Each draw's vertex program. */
	for (draw = 0; draw < job->draw_count; draw++) {
		graphics = job->draws[draw].pipeline;
		if (graphics == NULL)
			continue;
		if (graphics->vertex.stores)
			return true;
		for (index = 0; index < graphics->vertex.request_count; index++) {
			request = &graphics->vertex.requests[index];
			if (request->kind == POWERVR_SHADER_REQUEST_DESCRIPTOR && request->part != POWERVR_SHADER_PART_BUFFER_ADDRESS)
				return true;
		}
	}

	/* Succeeded: the vertex programs read buffers alone. */
	return false;
}

/*
 * Tells whether a job's fragment may leave the tiles no primitive reaches
 * untouched: no colour target is cleared there and there is
 * no depth target, so such a tile's memory is already what the render
 * leaves in it (a loaded target's pixels; a target whose old contents do
 * not matter).  A job loading through its load draws has those draws over
 * its render area only (its scissor's region clip), so the tiles outside
 * the area stay empty and are not processed at all.
 */
static bool
job_skips_empty_tiles(
	const struct powervr_render_job *job)
{
	uint32_t index;

	/* A depth target's tiles keep the depth processing they had. */
	if (job->depth.present)
		return false;

	/*
	 * A cleared colour target needs its background in every tile, and a
	 * resolved one its every tile written to the resolve attachment
	 * (Mesa 26.2.6 src/imagination/vulkan/pvr_arch_cmd_buffer.c
	 * pvr_setup_emit_state: process_empty_tiles for a resolve).
	 */
	for (index = 0; index < job->color_count; index++) {
		if (job->colors[index].load_op == LOAD_OP_CLEAR)
			return false;
		if (job->samples == 4U && job->colors[index].resolve)
			return false;
	}

	/* Succeeded: nothing is cleared, the empty tiles are left as they are. */
	return true;
}

/*
 * Tells whether a job's fragment needs no background object:
 * no depth target, and every colour target's old contents do not matter
 * (a target loaded through its load draw, in load_mask, is one, the draw
 * covering its render area), so no tile's pixels come from the background.
 */
static bool
job_needs_no_background(
	const struct powervr_render_job *job,
	uint32_t load_mask)
{
	uint32_t index;

	/* A depth target keeps its background depth. */
	if (job->depth.present)
		return false;

	/* A colour target loaded or cleared by the background needs it (a load draw's is not). */
	for (index = 0; index < job->color_count; index++) {
		if ((load_mask & (1U << index)) != 0)
			continue;
		if (job->colors[index].load_op != LOAD_OP_DONT_CARE)
			return false;
	}

	/* Succeeded: the background is left out. */
	return true;
}

/*
 * Takes job memory a finished job left mapped, its buffers' places kept
 * and everything else of it cleared, or NULL when the session keeps none.
 */
static struct job_memory *
job_keep_take(
	struct powervr_render *render,
	struct powervr_session *session)
{
	struct job_memory *memory;
	struct powervr_resource *general;
	struct powervr_buffer *pds;
	struct powervr_buffer *usc;
	uint64_t pds_place;
	uint64_t usc_place;
	struct powervr_resource *draw_general;
	struct powervr_buffer *draw_pds;
	struct powervr_buffer *draw_usc;
	uint64_t draw_pds_place;
	uint64_t draw_usc_place;
	unsigned long enabled;

	/* The newest kept memory, off the session's list. */
	memory = NULL;
	enabled = spin_lock_irqsave(&render->queue_lock);

	if (session->job_keep_count != 0U) {
		session->job_keep_count--;
		memory = session->job_keep[session->job_keep_count];
		session->job_keep[session->job_keep_count] = NULL;
	}

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	/* None kept. */
	if (memory == NULL)
		return NULL;

	/*
	 * Its mapped buffers stay; the rest is cleared for the new job (in
	 * place: the memory is too large for a copy on the kernel stack).
	 */
	general = memory->general;
	pds = memory->pds;
	usc = memory->usc;
	pds_place = memory->pds_place;
	usc_place = memory->usc_place;
	draw_general = memory->draw_general;
	draw_pds = memory->draw_pds;
	draw_usc = memory->draw_usc;
	draw_pds_place = memory->draw_pds_place;
	draw_usc_place = memory->draw_usc_place;
	kern_memset(memory, 0, sizeof(*memory));
	memory->general = general;
	memory->pds = pds;
	memory->usc = usc;
	memory->pds_place = pds_place;
	memory->usc_place = usc_place;
	memory->draw_general = draw_general;
	memory->draw_pds = draw_pds;
	memory->draw_usc = draw_usc;
	memory->draw_pds_place = draw_pds_place;
	memory->draw_usc_place = draw_usc_place;

	/* Succeeded: memory ready to be written again. */
	return memory;
}

/* Clears a buffer through its uncached view, as a new one is (powervr_buffer_create). */
static void
job_buffer_clear(
	struct powervr_buffer *buffer)
{
	volatile uint64_t *words;
	uint64_t index;

	/* Every word of it (a last part word too), so the core reads nothing of the job before. */
	words = (volatile uint64_t *)buffer->view;
	for (index = 0; index < (buffer->bytes + 7U) / 8U; index++)
		words[index] = 0;
}

/* Tells whether a job memory's draw buffers are there and large enough for the draws' bytes. */
static bool
job_draw_memory_fits(
	const struct job_memory *memory,
	uint64_t general_bytes,
	uint64_t pds_bytes,
	uint64_t usc_bytes)
{
	/* All three there. */
	if (memory->draw_general == NULL || memory->draw_pds == NULL || memory->draw_usc == NULL)
		return false;
	if (memory->draw_pds_place == 0U || memory->draw_usc_place == 0U)
		return false;

	/* Each as large as asked. */
	if (memory->draw_general->buffer->bytes < general_bytes)
		return false;
	if (memory->draw_pds->bytes < pds_bytes)
		return false;
	if (memory->draw_usc->bytes < usc_bytes)
		return false;

	/* Succeeded: they fit. */
	return true;
}

/*
 * Trades the job's draw buffers for those of a memory the session keeps
 * that are large enough, when one has them (the jobs of a frame differ
 * in size, and the newest kept memory a job takes may be a smaller
 * job's: making the buffers again cost a mapping a job).  Reports
 * whether the job's buffers fit now.
 */
static bool
job_draw_memory_swap(
	struct powervr_render *render,
	struct powervr_session *session,
	struct job_memory *memory,
	uint64_t general_bytes,
	uint64_t pds_bytes,
	uint64_t usc_bytes)
{
	struct powervr_resource *general;
	struct powervr_buffer *pds;
	struct powervr_buffer *usc;
	struct job_memory *kept;
	unsigned long enabled;
	uint64_t pds_place;
	uint64_t usc_place;
	unsigned index;
	bool fits;

	/* The first kept memory whose draw buffers fit, traded under the queue lock. */
	fits = false;
	enabled = spin_lock_irqsave(&render->queue_lock);

	for (index = 0; index < session->job_keep_count; index++) {
		kept = session->job_keep[index];
		if (kept == NULL || !job_draw_memory_fits(kept, general_bytes, pds_bytes, usc_bytes))
			continue;
		general = kept->draw_general;
		pds = kept->draw_pds;
		usc = kept->draw_usc;
		pds_place = kept->draw_pds_place;
		usc_place = kept->draw_usc_place;
		kept->draw_general = memory->draw_general;
		kept->draw_pds = memory->draw_pds;
		kept->draw_usc = memory->draw_usc;
		kept->draw_pds_place = memory->draw_pds_place;
		kept->draw_usc_place = memory->draw_usc_place;
		memory->draw_general = general;
		memory->draw_pds = pds;
		memory->draw_usc = usc;
		memory->draw_pds_place = pds_place;
		memory->draw_usc_place = usc_place;
		fits = true;
		break;
	}

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	/* Succeeded: whether the job's buffers fit. */
	return fits;
}

/* Unmaps and frees a job memory's draw buffers, leaving none. */
static void
job_draw_memory_free(
	struct powervr_render *render,
	struct powervr_session *session,
	struct job_memory *memory)
{
	/* The shading cluster's programs. */
	if (memory->draw_usc != NULL) {
		if (memory->draw_usc_place != 0U)
			powervr_session_program_unmap(session, POWERVR_PROGRAM_USC, memory->draw_usc, memory->draw_usc_place);
		powervr_buffer_put(render, memory->draw_usc);
	}

	/* The data sequencer's programs. */
	if (memory->draw_pds != NULL) {
		if (memory->draw_pds_place != 0U)
			powervr_session_program_unmap(session, POWERVR_PROGRAM_PDS, memory->draw_pds, memory->draw_pds_place);
		powervr_buffer_put(render, memory->draw_pds);
	}

	/* The general memory. */
	if (memory->draw_general != NULL)
		powervr_resource_unmake(session, memory->draw_general);

	/* None left. */
	memory->draw_usc = NULL;
	memory->draw_usc_place = 0;
	memory->draw_pds = NULL;
	memory->draw_pds_place = 0;
	memory->draw_general = NULL;
}

/* Makes and maps a job memory's draw buffers of the bytes asked (general, data sequencer, shading cluster). */
static int
job_draw_memory_make(
	struct powervr_render *render,
	struct powervr_session *session,
	struct job_memory *memory,
	uint64_t general_bytes,
	uint64_t pds_bytes,
	uint64_t usc_bytes)
{
	struct powervr_buffer *buffer;
	int error;

	/* Each made larger than asked, so that the next jobs of more draws fit without mapping again. */
	general_bytes = job_draw_memory_room(general_bytes);
	pds_bytes = job_draw_memory_room(pds_bytes);
	usc_bytes = job_draw_memory_room(usc_bytes);

	/* The general memory: a slot per draw, then the sets. */
	error = powervr_buffer_create(render, general_bytes, &buffer);
	if (error != 0)
		return error;

	error = powervr_resource_make(session, buffer, POWERVR_PROGRAM_SLC_CACHED ? 0 : 1, 0, &memory->draw_general);
	powervr_buffer_put(render, buffer);
	if (error != 0) {
		memory->draw_general = NULL;
		return error;
	}

	/* The program buffers, in their windows. */
	error = powervr_buffer_create(render, pds_bytes, &memory->draw_pds);
	if (error != 0) {
		memory->draw_pds = NULL;
		return error;
	}

	error = powervr_session_program_map(session, POWERVR_PROGRAM_PDS, memory->draw_pds, &memory->draw_pds_place);
	if (error != 0) {
		memory->draw_pds_place = 0;
		return error;
	}

	error = powervr_buffer_create(render, usc_bytes, &memory->draw_usc);
	if (error != 0) {
		memory->draw_usc = NULL;
		return error;
	}

	error = powervr_session_program_map(session, POWERVR_PROGRAM_USC, memory->draw_usc, &memory->draw_usc_place);
	if (error != 0) {
		memory->draw_usc_place = 0;
		return error;
	}

	/* Succeeded: the draws' memory is mapped. */
	return 0;
}

/*
 * Gives the bytes a job's draw buffer is made with for bytes asked: the
 * next power of two, at least DRAW_MEMORY_LEAST, so that kept memory
 * (job_keep_take) fits the following jobs and is seldom made again (each
 * making maps three buffers, each map waiting for the firmware's MMU
 * cache flush).
 */
static uint64_t
job_draw_memory_room(
	uint64_t bytes)
{
	uint64_t room;

	/* Doubled from the least until it holds them. */
	room = DRAW_MEMORY_LEAST;
	while (room < bytes)
		room *= 2U;

	/* Succeeded: the room. */
	return room;
}

/*
 * Sums a reaped render job's GPU time for the submission timing: its TA
 * from when it could start (its kick, the previous TA's end and the end
 * of the 3D its geometry waited for) to its end, its 3D from the later of
 * its TA's end and the previous 3D's end to its own end, and the time
 * between the previous 3D's end and its own.  A part not seen done takes
 * its end now.  The queue lock is held.
 */
static void
job_timing(
	struct powervr_session *session,
	struct powervr_job_done *done)
{
	uint64_t now;
	uint64_t ta_start;
	uint64_t fragment_start;
	unsigned index;

	/* The parts the service did not see done end now. */
	now = powervr_timing_now();
	if (done->ta_seen == 0U)
		done->ta_seen = now;
	if (done->fragment_seen == 0U)
		done->fragment_seen = now;

	/* The TA's start: its kick, or later the previous TA's end or the end of the 3D its geometry waited for. */
	ta_start = done->kicked;
	if (session->timing_ta_done > ta_start)
		ta_start = session->timing_ta_done;
	for (index = 0; index < 2U; index++) {
		/* Not the 3D the geometry waited for. */
		if (done->waited_fragment == 0U || session->timing_fragment_values[index] != done->waited_fragment)
			continue;

		/* That 3D ended after the start so far. */
		if (session->timing_fragment_ends[index] > ta_start)
			ta_start = session->timing_fragment_ends[index];
	}

	powervr_timing_add_span(POWERVR_TIMING_GPU_TA, ta_start, done->ta_seen);

	/* The 3D's start: its TA's end, or later the previous 3D's end. */
	fragment_start = done->ta_seen;
	if (session->timing_fragment_done > fragment_start)
		fragment_start = session->timing_fragment_done;
	powervr_timing_add_span(POWERVR_TIMING_GPU_3D, fragment_start, done->fragment_seen);

	/* The time from the previous 3D's end, once there was one. */
	if (session->timing_fragment_done != 0U)
		powervr_timing_add_span(POWERVR_TIMING_GPU_FRAME, session->timing_fragment_done, done->fragment_seen);

	/* This job's ends for the next one; its 3D value and end replace the older of the two kept. */
	session->timing_ta_done = done->ta_seen;
	session->timing_fragment_done = done->fragment_seen;
	session->timing_fragment_values[session->timing_fragment_next] = done->memory->marks.fragment_value;
	session->timing_fragment_ends[session->timing_fragment_next] = done->fragment_seen;
	session->timing_fragment_next ^= 1U;
}

/*
 * Reports the counts of a finished job's visibility registers to its
 * queries, read from the job's memory (NULL when they are lost: the job
 * was halted or let go before it finished); the report is done with them.
 */
static void
job_visibility_report(
	struct powervr_job_done *done,
	int lost)
{
	uint32_t counts[POWERVR_RENDER_VISIBILITY_MAX];
	const volatile uint32_t *place;
	uint32_t index;

	/* A job the core did not finish has no counts. */
	if (lost) {
		done->visibility->report(done->visibility, NULL);
		done->visibility = NULL;
		return;
	}

	/* Each register's count, as the 3D wrote it. */
	place = (const volatile uint32_t *)(done->memory->general->buffer->view + GENERAL_VISIBILITY);
	for (index = 0; index < done->visibility->count; index++)
		counts[index] = place[index];
	done->visibility->report(done->visibility, counts);
	done->visibility = NULL;
}

/* Writes count zero words to a buffer at a word-aligned offset. */
static void
zero_words(
	struct powervr_buffer *buffer,
	uint32_t offset,
	unsigned count)
{
	volatile uint32_t *place;
	unsigned index;

	/* Each word. */
	place = (volatile uint32_t *)(buffer->view + offset);
	for (index = 0; index < count; index++)
		place[index] = 0;
}
