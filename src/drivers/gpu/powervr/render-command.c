/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The register words of a render's geometry and fragment commands.
 */

#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/powervr/render-command.h"

/*
 * The pixel output control and the tile control of the BXE-4-32 for
 * single-sample attachments: two output registers a pixel (the core's
 * least) or four (width 4REGISTERS 1, for attachments of three or four
 * registers), a partition mask of every one of its ten partitions, and
 * four tiles in flight on its one pipe (with four registers a pixel the
 * 5120 words of partition space hold five partitions of 16x16 tiles,
 * still more than the four tiles in flight).
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/cr.xml
 * (USC_PIXEL_OUTPUT_CTRL: partition_mask 3..20, width 0..1 with
 * 2REGISTERS 0 and 4REGISTERS 1; ISP_CTL: skip_init_hdrs 31, process_empty_tiles 17,
 * sample_pos 16, pipe_enable 12..15 with PIPE_ONE 0);
 * src/imagination/common/device_info/bxe-4-32.h
 * (usc_min_output_registers_per_pix 2, max_partitions 10,
 * isp_max_tiles_in_flight 4, tile_size_x 16, num_raster_pipes 1,
 * simple_parameter_format_version 2, has_usc_pixel_partition_mask,
 * has_gpu_multicore_support, quirks 72168 and 72463).
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_common.c
 * (pvr_arch_setup_tiles_in_flight: the width rounded up to the least
 * registers, the partitions of 16x16 tiles, the ISP tiles in flight no
 * more than the partitions; the mask of every partition; the pipes less
 * one); pvr_arch_job_render.c (pvr_frag_state_stream_init: the sample
 * positions, empty tiles processed, the header skip of a multicore core
 * unless the quirks' tile shapes forbid it); Linux v7.3
 * drivers/gpu/drm/imagination/pvr_drv.c
 * (rogue_get_common_store_partition_space_size).
 */
#define PIXEL_OUTPUT_TWO_REGISTERS	(0x3ffU << 3)
#define PIXEL_OUTPUT_FOUR_REGISTERS	((0x3ffU << 3) | 1U)
#define ISP_SKIP_INIT_HEADERS		(1U << 31)
#define ISP_PROCESS_EMPTY_TILES		(1U << 17)
#define ISP_DEPTH_BIAS_INTEGER		(1U << 20)
#define ISP_SAMPLE_POSITIONS		(1U << 16)
#define ISP_TILES_IN_FLIGHT		(3U << 12)

/*
 * The ISP's antialiasing mode of a render of four samples (none for one).
 *
 * Values referenced: Mesa 26.2.6 src/imagination/csbgen/rogue/cr.xml
 * (ISP_AA: mode 0..1, ISP_AA_MODE_TYPE AA_NONE 0, AA_4X 2);
 * src/imagination/vulkan/pvr_arch_job_render.c (pvr_frag_state_stream_init:
 * CR_ISP_AA from the job's samples).
 */
#define ISP_ANTIALIAS_FOUR		2U

/*
 * The background object's depth (1.0 as a 32-bit float, the depth a
 * frame without a depth attachment clears to) and its values (the
 * background tag on, the stencil mask on).
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/cr.xml (ISP_BGOBJDEPTH value
 * 0..31; ISP_BGOBJVALS: enablebgtag 9, mask 8, stencil 0..7).
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_render.c
 * (pvr_frag_state_stream_init: the depth's clear value in the F32Z
 * form, the mask set, the tag when a background runs);
 * pvr_arch_cmd_buffer.c (enable_bg_tag only with an attachment loaded
 * or cleared); Mesa 26.2.6 pvr_arch_job_render.c (BGOBJVALS stencil
 * the stencil's clear value).
 */
#define BACKGROUND_DEPTH_ONE		0x3f800000U
#define BACKGROUND_TAG_AND_MASK		((1U << 9) | (1U << 8))
#define BACKGROUND_MASK			(1U << 8)

/*
 * The geometry's primitive and tiling controls: the w clamp and the
 * fixed point format 1; completion when the stream terminates, and the
 * region header stride in units of 4096 bytes; the resume task size as
 * a data sequencer state word with the common store the shared
 * registers take, in units of 64 bytes.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/cr.xml (PPP_CTRL:
 * fixed_point_format 9, wclampen 1; TE_PSG: completeonterminate 17,
 * region_stride 0..10 in units of 4096); vdm.xml (PDS_STATE0: block_type
 * 29..31 with PDS_STATE_UPDATE 1, usc_common_size 16..24 in units of
 * 64).
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_render.c
 * (pvr_geom_state_stream_init: PPP_CTRL, TE_PSG for the last geometry,
 * the resume program's common size from the shared registers).
 */
#define PPP_WCLAMP_FIXED_POINT		((1U << 9) | (1U << 1))
#define TE_COMPLETE_ON_TERMINATE	(1U << 17)
#define TE_REGION_STRIDE_UNIT		4096U
#define TE_REGION_STRIDE_MAX		0x7ffU
#define RESUME_STATE_UPDATE		(1U << 29)
#define RESUME_COMMON_SHIFT		16U
#define RESUME_COMMON_UNIT		64U

static void put32(uint8_t *bytes, uint32_t offset, uint32_t value);
static void put64(uint8_t *bytes, uint32_t offset, uint64_t value);

/* Whether the geometry and fragment commands carry the single-core flags (see render-command.h). */
static int single_core;

/*
 * Whether the commands carry the single-core flag on a core with
 * multicore support (1: the firmware does not wait for other cores).
 * It is 0 as in Mesa 25.0.0 src/imagination/vulkan/pvr_job_render.c,
 * whose render jobs set use_single_core only for a fragment that uses
 * atomics (frag_uses_atomic_ops), and the vendor driver's captured
 * geometry command on the board leaves the bit clear.  And whether the geometry command sets its
 * resume size (1, Mesa's VDMCTRL_PDS_STATE0 of the shared registers; the
 * vendor driver's command on the board left it zero).
 */
#ifndef POWERVR_RENDER_SINGLE_CORE_FLAG
#define POWERVR_RENDER_SINGLE_CORE_FLAG	0
#endif
#ifndef POWERVR_GEOMETRY_RESUME_SIZE_SET
#define POWERVR_GEOMETRY_RESUME_SIZE_SET	1
#endif

/*
 * Turns the single-core flags on or off, once the firmware image's
 * features are known.
 */
void
powervr_render_command_single_core(
	int on)
{
	/* Remembers the choice for every command built after it; the switch can keep it off. */
	single_core = 0;
	if (on && POWERVR_RENDER_SINGLE_CORE_FLAG)
		single_core = 1;
}

/*
 * Fills the registers of a fragment command.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_render.c
 * (pvr_frag_state_stream_init: the pixel back end words, the background
 * and the background after a partial render, no clear registers, the
 * pixel output control, the background depth and values, the
 * antialiasing of the samples, the tile control, the end-of-tile
 * program's size and data; nothing for depth or stencil).
 */
void
powervr_fragment_command(
	const struct powervr_fragment_setup *setup,
	uint8_t *command)
{
	uint32_t background_values;
	uint32_t pixel_output;
	uint32_t isp_control;
	uint32_t flags;
	unsigned index;
	unsigned emit;

	/* Each emit's back end words. */
	for (emit = 0; emit < POWERVR_FRAGMENT_EMITS; emit++) {
		for (index = 0; index < 3U; index++)
			put64(command, POWERVR_FRAGMENT_PBE + emit * POWERVR_FRAGMENT_PBE_EMIT_BYTES + index * 8U, setup->pbe[emit][index]);
	}

	/* The background, and the one after a partial render. */
	for (index = 0; index < POWERVR_BACKGROUND_WORDS; index++) {
		put64(command, POWERVR_FRAGMENT_BACKGROUND + index * 8U, setup->background[index]);
		put64(command, POWERVR_FRAGMENT_PARTIAL_BACKGROUND + index * 8U, setup->partial_background[index]);
	}

	/* The tile's pixel output, background object and tile controls. */
	pixel_output = PIXEL_OUTPUT_TWO_REGISTERS;
	if (setup->pixel_registers > 2U)
		pixel_output = PIXEL_OUTPUT_FOUR_REGISTERS;
	put32(command, POWERVR_FRAGMENT_PIXEL_OUTPUT, pixel_output);
	put32(command, POWERVR_FRAGMENT_BACKGROUND_DEPTH, setup->has_depth ? setup->background_depth : BACKGROUND_DEPTH_ONE);
	background_values = BACKGROUND_TAG_AND_MASK;
	if (setup->no_background)
		background_values = BACKGROUND_MASK;
	if (setup->has_depth && setup->has_stencil)
		background_values |= setup->background_stencil & 0xffU;
	put32(command, POWERVR_FRAGMENT_BACKGROUND_VALUES, background_values);
	put32(command, POWERVR_FRAGMENT_ANTIALIAS, setup->samples == 4U ? ISP_ANTIALIAS_FOUR : 0U);
	isp_control = powervr_fragment_isp_control(setup->tiles_x, setup->tiles_y);

	/*
	 * A render that clears nothing leaves the tiles no primitive reaches
	 * as they are in memory, as Mesa runs the background on empty tiles
	 * only for a clear: a small render area's job then
	 * processes its own tiles, not the whole target's.
	 *
	 * Procedure referenced:
	 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
	 * (process_empty_tiles only when a colour attachment's load op is
	 * VK_ATTACHMENT_LOAD_OP_CLEAR, or a clear is not by the index list).
	 */
	if (setup->skip_empty_tiles)
		isp_control &= ~ISP_PROCESS_EMPTY_TILES;

	/*
	 * An integer depth target takes its depth bias as integers, one being
	 * one step of the depth value.
	 *
	 * Values referenced: Mesa 26.2.5 src/imagination/csbgen/rogue/cr.xml
	 * (ISP_CTL dbias_is_int 20); pvr_arch_job_render.c
	 * (pvr_frag_state_stream_init: dbias_is_int with ERN 42307 for an
	 * integer ZLS format).
	 */
	if (setup->depth_bias_integer)
		isp_control |= ISP_DEPTH_BIAS_INTEGER;
	put32(command, POWERVR_FRAGMENT_ISP_CONTROL, isp_control);

	/*
	 * The scissor and depth bias tables, each register keeping the
	 * address's bits 2 to 39.
	 *
	 * Values referenced:
	 * Mesa 26.2.5 src/imagination/csbgen/rogue/cr.xml (ISP_SCISSOR_BASE,
	 * ISP_DBIAS_BASE: addr 2..39 shifted by 2).
	 */
	put64(command, POWERVR_FRAGMENT_SCISSOR_BASE, setup->scissor_table & 0xfffffffffcULL);
	put64(command, POWERVR_FRAGMENT_BORDER_TABLE, setup->border_table & 0xfffffffffcULL);
	put64(command, POWERVR_FRAGMENT_DBIAS_BASE, setup->dbias_table & 0xfffffffffcULL);

	/*
	 * Where the visibility registers of an occlusion query are written,
	 * the address's bits 4 to 39 (none, 0, without a query).
	 *
	 * Values referenced:
	 * Mesa 26.2.6 src/imagination/csbgen/rogue/cr.xml (ISP_OCLQRY_BASE:
	 * addr 4..39 shifted by 4) and src/imagination/vulkan/
	 * pvr_arch_job_render.c (the fragment state's CR_ISP_OCLQRY_BASE: the
	 * query pool's results, or an invalid address).
	 */
	put64(command, POWERVR_FRAGMENT_OCCLUSION_BASE, setup->occlusion_base & 0xfffffffff0ULL);

	/*
	 * A depth target's loads and stores, a stencil's beside it, and the
	 * flags: the depth buffer, the stencil buffer, one core, the
	 * visibility results collected for an occlusion query, and the
	 * scratch buffer a partial render stores the tiles to.  The
	 * stencil's base keeps the address's bits 4 to 39, its enable (a
	 * buffer without depth) off.
	 *
	 * Values referenced: Mesa 26.2.6 src/imagination/csbgen/rogue/cr.xml
	 * (ISP_STENCIL_LOAD_BASE addr 4..39 shifted by 4, enable 0);
	 * src/imagination/vulkan/winsys/powervr/pvr_drm_job_render.c (the
	 * fragment command's STENCILBUFFER flag with a stencil attachment).
	 */
	flags = 0U;
	if (single_core)
		flags = POWERVR_FRAGMENT_SINGLE_CORE;
	if (setup->has_depth) {
		put64(command, POWERVR_FRAGMENT_ZLS_CONTROL, setup->zls_control);
		put64(command, POWERVR_FRAGMENT_ZLS_BASE, setup->zls_base);
		put32(command, POWERVR_FRAGMENT_ZLS_STRIDE, setup->zls_stride);
		flags |= POWERVR_FRAGMENT_DEPTH_BUFFER;
		if (setup->has_stencil) {
			put64(command, POWERVR_FRAGMENT_STENCIL_BASE, setup->stencil_base & 0xfffffffff0ULL);
			put32(command, POWERVR_FRAGMENT_SLS_STRIDE, setup->stencil_stride);
			flags |= POWERVR_FRAGMENT_STENCIL_BUFFER;
		}
	}
	if (POWERVR_FRAGMENT_PIXEL_MERGE_OFF)
		flags |= POWERVR_FRAGMENT_DISABLE_PIXEL_MERGE;
	if (setup->occlusion_base != 0)
		flags |= POWERVR_FRAGMENT_GET_VISIBILITY;
	if (setup->scratch_buffer)
		flags |= POWERVR_FRAGMENT_SCRATCH_BUFFER;

	put32(command, POWERVR_FRAGMENT_FLAGS, flags);

	/* The end-of-tile program. */
	put32(command, POWERVR_FRAGMENT_EVENT_INFO, setup->event_info);
	put32(command, POWERVR_FRAGMENT_EVENT_DATA, setup->event_data);
}

/*
 * Fills the registers of a geometry command: its stream, its primitive
 * and tiling controls, its resume size and its flags (the render's
 * first and last geometry).
 */
void
powervr_geometry_command(
	const struct powervr_geometry_setup *setup,
	uint8_t *command)
{
	uint32_t flags;
	uint64_t stride;
	uint32_t common;

	/* The stream, the texture unit's border colours and the primitive control. */
	powervr_geometry_stream(command, setup->stream);
	put64(command, POWERVR_GEOMETRY_BORDER_TABLE, setup->border_table & 0xfffffffffcULL);
	put32(command, POWERVR_GEOMETRY_PPP_CONTROL, PPP_WCLAMP_FIXED_POINT);

	/* The tiling control: done at the stream's end, the region header stride. */
	stride = setup->region_header_bytes / TE_REGION_STRIDE_UNIT;
	if (stride > TE_REGION_STRIDE_MAX)
		stride = TE_REGION_STRIDE_MAX;
	put32(command, POWERVR_GEOMETRY_TE_PSG, TE_COMPLETE_ON_TERMINATE | (uint32_t)stride);

	/* The resume size: the common store of the shared registers. */
	common = (setup->shared_registers * 4U + RESUME_COMMON_UNIT - 1U) / RESUME_COMMON_UNIT;
	if (POWERVR_GEOMETRY_RESUME_SIZE_SET)
		put32(command, POWERVR_GEOMETRY_RESUME_SIZE, RESUME_STATE_UPDATE | (common << RESUME_COMMON_SHIFT));

	/* The only geometry of its render, on one core when chosen. */
	flags = POWERVR_GEOMETRY_FIRST_AND_LAST;
	if (single_core)
		flags |= POWERVR_GEOMETRY_SINGLE_CORE;
	put32(command, POWERVR_GEOMETRY_FLAGS, flags);
}

/*
 * Fills a transfer command from its registers, each at its place in the
 * firmware's transfer registers; the common part and the flags stay
 * zero.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_transfer.c
 * (pvr_submit_info_stream_init: the registers of one transfer).
 */
void
powervr_transfer_command(
	const struct powervr_transfer_regs *regs,
	uint8_t *command)
{
	unsigned index;

	/* The 32-bit registers. */
	put32(command, 8U + 0U, regs->isp_bgobjvals);
	put32(command, 8U + 4U, regs->usc_pixel_output_ctrl);
	for (index = 0; index < 4U; index++)
		put32(command, 8U + 8U + index * 4U, regs->usc_clear[index]);
	put32(command, 8U + 24U, regs->isp_mtile_size);
	put32(command, 8U + 28U, regs->isp_render_origin);
	put32(command, 8U + 32U, regs->isp_ctl);
	put32(command, 8U + 40U, regs->isp_aa);
	put32(command, 8U + 44U, regs->event_pixel_pds_info);
	put32(command, 8U + 48U, regs->event_pixel_pds_code);
	put32(command, 8U + 52U, regs->event_pixel_pds_data);
	put32(command, 8U + 56U, regs->isp_render);
	put32(command, 8U + 60U, regs->isp_rgn);
	put32(command, 8U + 64U, regs->frag_screen);

	/* The 64-bit ones: the background, the macrotile base, the back end words. */
	for (index = 0; index < 3U; index++)
		put64(command, 8U + 72U + index * 8U, regs->pds_bgnd[index]);
	put64(command, 8U + 96U, regs->isp_mtile_base);
	for (index = 0; index < POWERVR_TRANSFER_PBE_WORDS; index++)
		put64(command, 8U + 104U + index * 8U, regs->pbe[index]);
}

/* Reports the tile control word the fragment command takes for a frame of tiles. */
uint32_t
powervr_fragment_isp_control(
	uint32_t tiles_x,
	uint32_t tiles_y)
{
	uint32_t word;
	uint32_t groups;

	/* Sample positions, empty tiles processed, the tiles in flight, the header skip. */
	word = ISP_SAMPLE_POSITIONS | ISP_PROCESS_EMPTY_TILES | ISP_TILES_IN_FLIGHT | ISP_SKIP_INIT_HEADERS;

	/* No header skip for a frame two tiles wide or less whose tile group rows (tiles down rounded up to pairs, halved) the quirks name. */
	groups = (tiles_y + 1U) / 2U;
	if (tiles_x <= 2U && ((groups & 1U) == 1U || groups <= 2U))
		word &= ~ISP_SKIP_INIT_HEADERS;

	/* Succeeded: the word. */
	return word;
}

/* Writes a little-endian 32-bit word. */
static void
put32(
	uint8_t *bytes,
	uint32_t offset,
	uint32_t value)
{
	/* Low byte first, as the firmware reads it. */
	bytes[offset + 0U] = (uint8_t)value;
	bytes[offset + 1U] = (uint8_t)(value >> 8);
	bytes[offset + 2U] = (uint8_t)(value >> 16);
	bytes[offset + 3U] = (uint8_t)(value >> 24);
}

/* Writes a little-endian 64-bit word. */
static void
put64(
	uint8_t *bytes,
	uint32_t offset,
	uint64_t value)
{
	/* The low half, then the high. */
	put32(bytes, offset, (uint32_t)value);
	put32(bytes, offset + 4U, (uint32_t)(value >> 32));
}
