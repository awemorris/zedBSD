/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fixed programs and state words of a PowerVR render: the end of
 * tile, the clear background, the pixel back end's words, and the
 * kernel's test triangle.  See render-program.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/data-program.h"
#include "drivers/gpu/powervr/fixed-area.h"
#include "drivers/gpu/powervr/image-state.h"
#include "drivers/gpu/powervr/compiler/pds-encode.h"
#include "drivers/gpu/powervr/render-program.h"
#include "drivers/gpu/powervr/compiler/shader.h"
#include "drivers/gpu/powervr/compiler/usc-encode.h"

/*
 * The shading cluster's registers and fields these programs name: the
 * first pixel output register (a special register), the bitwise
 * immediate's count and shift bypasses, the feed-through selectors, and
 * the vertex output signals.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_map.py (the pixout register class
 * on the special bank) and src/imagination/pco/pco_isa.py (special
 * register pixout 32; F_COUNT_SRC s2 0b0; F_COUNT_OP byp 0b10;
 * F_SHIFT1_OP byp 0b00; F_IS4_SEL ft0 0b00, fte 0b11; F_UVSW_OP write
 * 0b000, emit_endtask 0b101).
 */
#define USC_PIXEL_OUTPUT	32U
#define USC_COUNT_FROM_S2	0U
#define USC_COUNT_PASS		2U
#define USC_SHIFT_PASS		0U
#define USC_SELECT_FT0		0U
#define USC_SELECT_FTE		3U
#define USC_UVSW_WRITE		0U
#define USC_UVSW_EMIT_END	5U

/* The bits of 1.0f, the test vertex's w. */
#define FLOAT_ONE		0x3f800000U

/*
 * The shared register the background's colour is written to, and the
 * data sequencer's instance sample rate.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_usc.c (pvr_uscgen_loadop: the
 * clear values from shared register 0 up);
 * src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_DOUTU_SAMPLE_RATE_INSTANCE 0).
 */
#define CLEAR_SHARED		0U
#define SAMPLE_RATE_INSTANCE	0U

/*
 * The fields of the pixel back end's state and register words.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/pbestate.xml (STATE_WORD0
 * address_low 0..31 shifted by 2; STATE_WORD1 address_high 0..5 shifted
 * by 34, packmode 7..12, norm 13, source_pos 14..15, source_format 19;
 * REG_WORD0 swiz_chan0 20..22, _chan1 23..25, _chan2 26..28, _chan3
 * 29..31, minclip_x 6..19, gamma 4, memlayout 32..33, linestride 34..49
 * in units of 2, tilerelative 57; REG_WORD1 maxclip_y 0..13, size_y
 * 28..31, maxclip_x 32..45, minclip_y 46..59, size_x 60..63; SWIZ
 * SOURCE_CHAN0 0 to SOURCE_CHAN3 3, ONE 4, ZERO 5; MEMLAYOUT LINEAR 0,
 * TWIDDLE_2D 1; SOURCE_FORMAT F16_PER_CHANNEL 0, 8_PER_CHANNEL 1).
 */
#define PBE_CLIP_MAX		0x3fffU
#define PBE_LINESTRIDE_MAX	0xffffU
#define PBE_SWIZ_ONE		4U
#define PBE_SWIZ_ZERO		5U
#define PBE_LAYOUT_LINEAR	0U
#define PBE_LAYOUT_TWIDDLE	1U
#define PBE_SOURCE_F16		0U
#define PBE_SOURCE_8		1U
#define PBE_NO_PACKMODE		0xffU

/*
 * The fields of the background's data sequencer registers.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/cr.xml (PDS_BGRND0_BASE
 * shader_addr 4..31 and texunicode_addr 36..63, each shifted by 4;
 * PDS_BGRND1_BASE texturedata_addr 36..63 shifted by 4;
 * PDS_BGRND3_SIZEINFO pds_tempsize 0..3 in units of 4,
 * pds_texturestatesize 16..22 in units of 4, usc_sharedsize 55..63 in
 * units of 16; EVENT_PIXEL_PDS_CODE and _DATA addr 4..31 shifted by 4;
 * EVENT_PIXEL_PDS_INFO const_size 0..4 in units of 4, temp_stride 5..8,
 * usc_sr_size 9..14 in units of 16).
 */
#define OFFSET_MASK		0xfffffff0U
#define TEMPS_MAX		0xfU
#define STATE_SIZE_MAX		0x7fU
#define SHARED_SIZE_MAX		0x1ffU

/*
 * The words of the primitive state block.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/ppp.xml (STATE_HEADER
 * pres_ispctl 0, _fa 1, _fb 2, pres_pds_state_ptr0 6, _ptr1 7, _ptr2 8,
 * pres_region_clip 10, pres_wclamp 16, pres_outselects 17,
 * pres_varying_word0 18, _word1 19, _word2 20, pres_ppp_ctrl 21,
 * pres_stream_out_size 22; STATE_ISPCTL bpres 18; STATE_ISPA objtype
 * 28..31 TRIANGLE 0, passtype 24..26 TRANSLUCENT 1, dwritedisable 21,
 * dcmpmode 17..19 ALWAYS 7; STATE_ISPB scmpmode 25..27 ALWAYS 7;
 * STATE_PDS_SIZEINFO1 pds_tempsize 0..3, pds_texturestatesize 16..22;
 * STATE_PDS_SIZEINFO2 usc_sharedsize 23..31; REGION_CLIP0 mode 31
 * OUTSIDE 1, left 16..24, right 0..8; REGION_CLIP1 top 16..24, bottom
 * 0..8; REGION_CLIP_MAX 0x1ff; OUTPUT_SEL vtxsize 24..31, rhw_pres 16;
 * STATE_PPP_CTRL pretransform 6, cullmode 0..1 NO_CULLING 0).
 */
#define PPP_HEADER_PRESENT	0x007f05c7U
#define PPP_ISPCTL_BPRES	(1U << 18)
#define PPP_ISPA_TRANSLUCENT	(1U << 24)
#define PPP_ISPA_NO_DEPTH_WRITE	(1U << 21)
#define PPP_ISPA_ALWAYS		(7U << 17)
#define PPP_ISPB_ALWAYS		(7U << 25)
#define PPP_CLIP_OUTSIDE	(1U << 31)
#define PPP_CLIP_MAX		0x1ffU
#define PPP_OUTPUT_VERTEX_WORDS	4U
#define PPP_OUTPUT_RHW		(1U << 16)
#define PPP_PRETRANSFORM	(1U << 6)

/*
 * The smallest w the primitive state clamps to.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/rogue/pvr_clear.c
 * (pvr_device_setup_graphics_static_clear_ppp_base: wclamp
 * fui(1.0e-13f), 0x29e12e13).
 */
#define PPP_WCLAMP		0x29e12e13U

/*
 * The words of the vertex control stream.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/vdm.xml (BLOCK_TYPE at
 * 29..31: PPP_STATE_UPDATE 0, VDM_STATE_UPDATE 2, INDEX_LIST 3,
 * STREAM_TERMINATE 6; PPP_STATE0 word_count 8..15, addrmsb 0..7 shifted
 * by 32; PPP_STATE1 addrlsb 2..31; VDM_STATE0 vs_data_addr_present 27,
 * vs_other_present 26, cam_size 7..14, uvs_scratch_size_select FIVE 0
 * at 6, flatshade_control VERTEX_0 0 at 1..2; VDM_STATE2 and _STATE3
 * addresses 4..31 shifted by 4; VDM_STATE4 vs_output_size 0..7 in units
 * of 4 bytes; VDM_STATE5 vs_max_instances 25..29, vs_usc_unified_size
 * 10..15 in units of 16 bytes, vs_pds_temp_size 6..9 in units of 16,
 * vs_pds_data_size 0..5 in units of 16; INDEX_LIST0
 * index_count_present 27, primitive_topology 8..11 TRI_LIST 6;
 * INDEX_LIST2 index_count).
 */
#define VDM_BLOCK_SHIFT		29U
#define VDM_PPP_STATE		0U
#define VDM_VDM_STATE		2U
#define VDM_INDEX_LIST		3U
#define VDM_TERMINATE		6U
#define TERMINATE_PRESENT	(1U << 25)
#define TERMINATE_BLOCK_PIXELS	32U
#define TERMINATE_RIGHT_SHIFT	18U
#define TERMINATE_CLIP_MASK	0x1ffU
#define VDM_DATA_PRESENT	(1U << 27)
#define VDM_OTHER_PRESENT	(1U << 26)
#define VDM_COUNT_PRESENT	(1U << 27)
#define VDM_TRIANGLE_LIST	(6U << 8)
#define VDM_UNIT_BYTES		16U
#define VDM_FIELD6_MAX		0x3fU
#define VDM_FIELD4_MAX		0xfU

/*
 * The vertex cache entries and the instances of a task for the test
 * vertex (four output words), on the BXE-4-32.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_arch_calculate_vertex_cam_size: with
 * simple_internal_parameter_format and vdm_cam_size below 96, a vertex
 * vector of (4 + 1 + 4) * 4 = 36 words, below 14 * 4, gives cam_size
 * MIN2(31, vdm_cam_size - 1) and 16 instances);
 * src/imagination/common/device_info/bxe-4-32.h (vdm_cam_size 64,
 * simple_internal_parameter_format).
 */
#define VDM_CAM_SIZE		31U
#define VDM_MAX_INSTANCES	16U

/* The three vertices of the test triangle. */
#define TRIANGLE_VERTICES	3U

/*
 * A group with no operation and no operand and a repeat count of one:
 * the start every group here is filled in from.
 */
static const struct powervr_usc_group blank_group = {
	.repeat = 1
};

static int immediate_group(struct powervr_usc_group *group, unsigned destination, uint32_t value);
static int move_group(struct powervr_usc_group *group, unsigned bank, unsigned source, unsigned destination_bank, unsigned destination);
static int vertex_write_group(struct powervr_usc_group *group, unsigned bank, unsigned source, unsigned address);
static void temporary(struct powervr_usc_register *operand, unsigned index);
static int encode(const struct powervr_usc_group *groups, size_t count, unsigned temps, struct powervr_render_usc *program);
static uint32_t round_units(uint32_t value, uint32_t unit);
static int pbe_source(const struct powervr_format *format, unsigned *source_format, unsigned *gamma);
static int pbe_swizzle(const struct powervr_format *format, uint64_t *word);
static unsigned log2_ceiling(uint32_t value);

/*
 * Builds the end-of-tile shading cluster program: the two state words of
 * the pixel back end moved into temporaries, then the emission of the
 * tile with them, freeing the partition and ending the program.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_usc.c (pvr_usc_eot with one
 * emit and the state words as immediates: state0 and state1 from
 * nir_imm_int, emitpix with freep on the last emit, then return;
 * olchk_skip); src/imagination/pco/pco_map.py (group_map O_MOVI32: a
 * bitwise group, phase 0, the first write; group_map O_EMITPIX: a main
 * group with the back end only, s0 and s2, the end flag).
 */
int
powervr_render_eot_usc(
	const uint32_t state[2],
	struct powervr_render_usc *program)
{
	struct powervr_usc_group groups[3];
	int error;

	/* Refuses missing words or result. */
	if (state == NULL || program == NULL)
		return EINVAL;

	/* The two state words into temporaries 0 and 1. */
	error = immediate_group(&groups[0], 0, state[0]);
	if (error != 0)
		return error;
	error = immediate_group(&groups[1], 1, state[1]);
	if (error != 0)
		return error;

	/* The emission from s0 and s2, the last one, freeing the partition. */
	groups[2] = blank_group;
	groups[2].unit = POWERVR_USC_UNIT_MAIN;
	groups[2].end = 1;
	temporary(&groups[2].source[0], 0);
	temporary(&groups[2].source[2], 1);
	error = powervr_usc_emit_pixel(1, &groups[2].operation[POWERVR_USC_PHASE_BACKEND]);
	if (error != 0)
		return error;

	/* Encodes the three groups with their two temporaries. */
	error = encode(groups, 3, 2, program);
	if (error != 0)
		return error;

	/* Succeeded: the end-of-tile program. */
	return 0;
}

/*
 * Builds the data of the end-of-tile data sequencer program: the task
 * word that starts the end-of-tile shading cluster program at its
 * offset with its temporaries at the instance rate.  The code is the
 * fixed area's single task issue from the first constant.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_sub_cmd_gfx_per_job_fragment_programs_create_and_upload:
 * pvr_pds_setup_doutu with the end-of-tile program's address, its
 * temporaries and the instance sample rate; only the data segment, the
 * code being the static one); src/imagination/vulkan/pds/pvr_pds.c
 * (pvr_pds_generate_pixel_event: the task word in constants 0 and 1).
 */
int
powervr_render_event_data(
	uint32_t usc_offset,
	unsigned temps,
	uint32_t data[POWERVR_RENDER_EVENT_DATA_WORDS])
{
	uint64_t task;
	int error;

	/* Refuses a missing result. */
	if (data == NULL)
		return EINVAL;

	/* The task word. */
	error = powervr_pds_task_word(usc_offset, temps, SAMPLE_RATE_INSTANCE, 0, &task);
	if (error != 0)
		return error;

	/* Its two halves, the low one first. */
	data[0] = (uint32_t)task;
	data[1] = (uint32_t)(task >> 32);

	/* Succeeded: the data words. */
	return 0;
}

/*
 * Packs the fragment command's three words of the end-of-tile data
 * sequencer program: the code at the fixed area's end-of-tile offset,
 * the data at data_offset, and the sizes (the data words, no
 * temporary stride, the two state words of the shared registers).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_render.c
 * (pvr_frag_state_stream_init: CR_EVENT_PIXEL_PDS_INFO const_size from
 * the pixel event data size, temp_stride 0, usc_sr_size from
 * PVR_STATE_PBE_DWORDS 2; CR_EVENT_PIXEL_PDS_DATA the data's offset).
 */
int
powervr_render_event_words(
	uint32_t data_offset,
	uint32_t *code_word,
	uint32_t *data_word,
	uint32_t *info_word)
{
	/* Refuses missing results and data not on a 16-byte boundary. */
	if (code_word == NULL || data_word == NULL || info_word == NULL)
		return EINVAL;
	if ((data_offset & ~OFFSET_MASK) != 0)
		return EINVAL;

	/* The code's and the data's offsets. */
	*code_word = POWERVR_FIXED_TILE_END_OFFSET & OFFSET_MASK;
	*data_word = data_offset;

	/* The sizes: the data in units of four words, the two state words in units of sixteen. */
	*info_word = round_units(POWERVR_RENDER_EVENT_DATA_WORDS, 4U);
	*info_word |= round_units(2U, 16U) << 9;

	/* Succeeded: the three words. */
	return 0;
}

/*
 * A test switch (0 as built): immediate groups the clear
 * background's program runs before its moves, to time a background
 * program as long as a draw's fragment program (14 makes 15 groups, the
 * length of vkmatrix's immediate.frag).
 */
#ifndef POWERVR_CLEAR_TRIAL_PAD
#define POWERVR_CLEAR_TRIAL_PAD	0U
#endif

/*
 * Builds the clear background's shading cluster program for a clear of
 * one word (one packed colour) or two (four half floats, the sRGB and
 * 16-bit float formats): the words in shared registers 0 and 1 moved
 * into pixel output registers 0 and 1, the first under the overlap
 * check, ending the program.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_usc.c (pvr_uscgen_loadop with
 * one cleared render target: each of its words from the shared
 * registers, nir_load_preamble, stored to the output registers);
 * src/imagination/pco/pco_map.py (group_map O_MBYP: phase 0 passes s0,
 * the first write, is4 from ft0).
 */
int
powervr_render_clear_usc_words(
	uint32_t words,
	struct powervr_render_usc *program)
{
	struct powervr_usc_group groups[POWERVR_CLEAR_TRIAL_PAD + 2U];
	uint32_t pads;
	uint32_t pad;
	uint32_t word;
	int error;

	/* Refuses a missing result and a clear of other than one or two words. */
	if (program == NULL || words == 0 || words > 2U)
		return EINVAL;

	/* The test switch's padding: immediates into temporary 0, which nothing reads. */
	pads = POWERVR_CLEAR_TRIAL_PAD;
	for (pad = 0; pad < pads; pad++) {
		error = immediate_group(&groups[pad], 0, pad);
		if (error != 0)
			return error;
	}

	/* A move a word from its shared register to its pixel output, the first checked, the last ending. */
	for (word = 0; word < words; word++) {
		error = move_group(&groups[pad + word], POWERVR_USC_BANK_SHARED, CLEAR_SHARED + word, POWERVR_USC_BANK_SPECIAL, USC_PIXEL_OUTPUT + word);
		if (error != 0)
			return error;
	}
	groups[pad].overlap_check = 1;
	groups[pad + words - 1U].end = 1;

	/* Encodes them; they need no temporary but the padding's. */
	error = encode(groups, pad + words, pads != 0 ? 1U : 0U, program);
	if (error != 0)
		return error;

	/* Succeeded: the clear program. */
	return 0;
}

/* Builds the clear background's shading cluster program for a clear of one packed word. */
int
powervr_render_clear_usc(
	struct powervr_render_usc *program)
{
	int error;

	/* The program of one word. */
	error = powervr_render_clear_usc_words(1, program);
	if (error != 0)
		return error;

	/* Succeeded: the clear program. */
	return 0;
}

/*
 * Builds the data sequencer program that writes a clear's one or two
 * words (powervr_format_clear_words) to shared registers 0 and 1 for the
 * clear background.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_load_op_constants_create_and_upload: the clear words as the
 * shared registers' constants, written by the texture and uniform
 * program).
 */
int
powervr_render_clear_shared_words(
	const uint32_t *colour,
	uint32_t words,
	struct powervr_data_shared_program *program)
{
	struct powervr_data_load load;
	int error;

	/* Refuses a missing input or result and a clear of other than one or two words. */
	if (colour == NULL || program == NULL || words == 0 || words > 2U)
		return EINVAL;

	/* The words written to the clear's even shared register, the first in the low half. */
	load.kind = POWERVR_DATA_LOAD_WRITE;
	load.shared = CLEAR_SHARED;
	load.words = (uint16_t)words;
	load.source = colour[0];
	if (words == 2U)
		load.source |= (uint64_t)colour[1] << 32;
	error = powervr_data_shared_build(&load, 1, program);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/* Builds the data sequencer program that writes a clear of one packed word to shared register 0. */
int
powervr_render_clear_shared(
	uint32_t colour,
	struct powervr_data_shared_program *program)
{
	int error;

	/* The program of one word. */
	error = powervr_render_clear_shared_words(&colour, 1, program);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Builds the test triangle's fragment program: the packed colour moved
 * into a temporary, then into pixel output register 0 under the
 * overlap check, ending the program.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_map.py (group_map O_MOVI32 and
 * O_MBYP).
 */
int
powervr_render_colour_usc(
	uint32_t colour,
	struct powervr_render_usc *program)
{
	struct powervr_usc_group groups[2];
	int error;

	/* Refuses a missing result. */
	if (program == NULL)
		return EINVAL;

	/* The colour into temporary 0. */
	error = immediate_group(&groups[0], 0, colour);
	if (error != 0)
		return error;

	/* Temporary 0 into the pixel output, the last group. */
	error = move_group(&groups[1], POWERVR_USC_BANK_TEMPORARY, 0, POWERVR_USC_BANK_SPECIAL, USC_PIXEL_OUTPUT);
	if (error != 0)
		return error;
	groups[1].overlap_check = 1;
	groups[1].end = 1;

	/* Encodes them with their one temporary. */
	error = encode(groups, 2, 1, program);
	if (error != 0)
		return error;

	/* Succeeded: the fragment program. */
	return 0;
}

/*
 * Builds the test triangle's vertex program: x, y and z from vertex
 * input registers 0 to 2 written to vertex outputs 0 to 2, 1.0 to output
 * 3, then the emission of the vertex that ends the task.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/usclib/common.cl
 * (vs_passthrough_common: uvsw_write 0 of vtxin 0..2, uvsw_write 3 of
 * 1.0f); src/imagination/pco/pco_end.c (a vertex program ends with the
 * emit and end-of-task signal); src/imagination/vulkan/rogue/pvr_clear.c
 * (the clear's vertex: three coordinates).
 */
int
powervr_render_vertex_usc(
	struct powervr_render_usc *program)
{
	struct powervr_usc_group groups[6];
	unsigned index;
	int error;

	/* Refuses a missing result. */
	if (program == NULL)
		return EINVAL;

	/* The three coordinates, each written to its output. */
	for (index = 0; index < POWERVR_RENDER_VERTEX_WORDS; index++) {
		error = vertex_write_group(&groups[index], POWERVR_USC_BANK_VERTEX_INPUT, index, index);
		if (error != 0)
			return error;
	}

	/* 1.0 into temporary 0 and written as the w. */
	error = immediate_group(&groups[3], 0, FLOAT_ONE);
	if (error != 0)
		return error;
	error = vertex_write_group(&groups[4], POWERVR_USC_BANK_TEMPORARY, 0, 3);
	if (error != 0)
		return error;

	/* The emission that ends the task and the program. */
	groups[5] = blank_group;
	groups[5].unit = POWERVR_USC_UNIT_MAIN;
	groups[5].end = 1;
	error = powervr_usc_vertex_output(USC_UVSW_EMIT_END, 0, 0, 0, 0, &groups[5].operation[POWERVR_USC_PHASE_BACKEND]);
	if (error != 0)
		return error;

	/* Encodes the six groups with their one temporary. */
	error = encode(groups, 6, 1, program);
	if (error != 0)
		return error;

	/* Succeeded: the vertex program. */
	return 0;
}

/*
 * Builds the test triangle's vertex data sequencer program: the three
 * coordinates of each vertex loaded from vertices (12 bytes a vertex)
 * into vertex input registers 0 to 2, then the vertex program's task.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/rogue/pvr_clear.c
 * (pvr_pds_clear_vertex_shader_program_init_base: one stream of three
 * coordinates a vertex, the task at the instance rate).
 */
int
powervr_render_vertex_pds(
	uint64_t vertices,
	uint32_t usc_offset,
	unsigned temps,
	struct powervr_data_vertex_program *program)
{
	struct powervr_data_vertex_dma dma;
	uint64_t task;
	int error;

	/* Refuses a missing result. */
	if (program == NULL)
		return EINVAL;

	/* The vertex program's task word. */
	error = powervr_pds_task_word(usc_offset, temps, SAMPLE_RATE_INSTANCE, 0, &task);
	if (error != 0)
		return error;

	/* One load of three words a vertex into register 0, no index registers. */
	dma.address = vertices;
	dma.stride = POWERVR_RENDER_VERTEX_WORDS * 4U;
	dma.words = POWERVR_RENDER_VERTEX_WORDS;
	dma.reg = 0;
	dma.instance = false;
	error = powervr_data_vertex_build(&dma, 1, POWERVR_SHADER_NO_REG, POWERVR_SHADER_NO_REG, task, program);
	if (error != 0)
		return error;

	/* Succeeded: the vertex data sequencer program. */
	return 0;
}

/*
 * Builds a data sequencer program that only starts a shading cluster
 * task (a fragment or background program): the task word in the first
 * two of the eight data words, the single task issue that ends the
 * program after them.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pds.c (pvr_pds_kick_usc:
 * the task word in constants 0 and 1 of eight, one DOUTU from constant
 * 0 with the end); src/imagination/vulkan/rogue/pvr_clear.c
 * (pvr_device_init_clear_attachment_programs: the instance sample rate).
 */
int
powervr_render_kick_build(
	uint32_t usc_offset,
	unsigned temps,
	struct powervr_render_kick *kick)
{
	uint64_t task;
	unsigned index;
	int error;

	/* Refuses a missing result. */
	if (kick == NULL)
		return EINVAL;

	/* The task word. */
	error = powervr_pds_task_word(usc_offset, temps, SAMPLE_RATE_INSTANCE, 0, &task);
	if (error != 0)
		return error;

	/* The data: the task word, then zeros. */
	kick->data[0] = (uint32_t)task;
	kick->data[1] = (uint32_t)(task >> 32);
	for (index = 2; index < POWERVR_RENDER_KICK_DATA_WORDS; index++)
		kick->data[index] = 0;

	/* The code: the task issue from the first 64-bit constant, ending the program. */
	error = powervr_pds_output(0, 1, POWERVR_PDS_OUTPUT_SHADER_TASK, 0, 0, &kick->code[0]);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * A test switch: the temporaries a draw's fragment task asks for,
 * at least this many (0: the program's own).  32 or 48 measures how much
 * the number of instances the shading cluster keeps resident matters.
 */
#ifndef POWERVR_FRAGMENT_TEMPS_FORCE
#define POWERVR_FRAGMENT_TEMPS_FORCE	0
#endif

/* Builds a draw's fragment task program: powervr_render_kick_build with the test switch's temporaries. */
int
powervr_render_fragment_kick_build(
	uint32_t usc_offset,
	unsigned temps,
	struct powervr_render_kick *kick)
{
	/* The program's temporaries, raised by the test switch. */
#if POWERVR_FRAGMENT_TEMPS_FORCE > 0
	if (temps < POWERVR_FRAGMENT_TEMPS_FORCE)
		temps = POWERVR_FRAGMENT_TEMPS_FORCE;
#endif
	return powervr_render_kick_build(usc_offset, temps, kick);
}

/*
 * Packs the fragment command's three background words (the pds_bgnd
 * and pds_pr_bgnd registers): the task program's and the shared
 * register program's code, the shared register program's data, and the
 * sizes.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_pds_bgnd_pack_state: CR_PDS_BGRND0_BASE shader_addr the
 * fragment program's data offset and texunicode_addr the shared
 * program's code, CR_PDS_BGRND1_BASE texturedata_addr its data,
 * CR_PDS_BGRND3_SIZEINFO usc_sharedsize, pds_texturestatesize and
 * pds_tempsize rounded up to their units).
 */
int
powervr_render_background_words(
	const struct powervr_render_background *background,
	uint64_t words[3])
{
	uint32_t states;
	uint32_t shared;

	/* Refuses missing state or result, and offsets not on 16-byte boundaries. */
	if (background == NULL || words == NULL)
		return EINVAL;
	if ((background->kick_offset & ~OFFSET_MASK) != 0 ||
	    (background->shared_code_offset & ~OFFSET_MASK) != 0 ||
	    (background->shared_data_offset & ~OFFSET_MASK) != 0)
		return EINVAL;

	/* Refuses sizes the fields cannot hold. */
	states = round_units(background->shared_data_words, 4U);
	shared = round_units(background->shared_registers, 16U);
	if (states > STATE_SIZE_MAX || shared > SHARED_SIZE_MAX)
		return EINVAL;

	/* The two code offsets, the data offset, the sizes (no data sequencer temporaries). */
	words[0] = (uint64_t)background->kick_offset | ((uint64_t)background->shared_code_offset << 32);
	words[1] = (uint64_t)background->shared_data_offset << 32;
	words[2] = ((uint64_t)states << 16) | ((uint64_t)shared << 55);

	/* Succeeded: the three words. */
	return 0;
}

/*
 * Packs the pixel back end's two state words (which the end-of-tile
 * program emits) and three register words (which go in the fragment
 * command) of a colour attachment stored from pixel output register 0,
 * clipped to the attachment's edges and the render area.
 *
 * Refuses a format the back end does not store, a depth format and an
 * area that leaves no pixel of the attachment (EINVAL), and a stride
 * the field cannot hold (E2BIG).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_setup_pbe_state: the clip from the render area within the
 * attachment, max = MIN2(edge - 1, offset + extent - 1); the stride in
 * pixels from the level's pitch; the source at bit 0 for output register
 * 0; with usc_f16sop_u8, a packed channel for unorm formats);
 * src/imagination/vulkan/pvr_arch_job_common.c
 * (pvr_arch_pbe_pack_state: tilerelative, the memory layout,
 * linestride = (stride - 1) / 2, the swizzle by source channel, gamma
 * for sRGB, size_x and size_y log2 of the edges when twiddled;
 * pvr_arch_pbe_get_src_format_and_gamma: an sRGB format F16 with gamma,
 * an 8-bit unorm format with a packed channel 8 per channel).
 */
int
powervr_render_pbe_words(
	const struct powervr_render_surface *surface,
	const struct powervr_render_area *area,
	uint32_t state[2],
	uint64_t reg[3])
{
	const struct powervr_format *format;
	uint64_t swizzle;
	uint32_t stride;
	uint32_t min_x;
	uint32_t min_y;
	uint32_t max_x;
	uint32_t max_y;
	unsigned source_format;
	unsigned gamma;
	int error;

	/* Refuses missing input or results, and a format the back end does not store. */
	if (surface == NULL || area == NULL || state == NULL || reg == NULL)
		return EINVAL;
	format = surface->format;
	if (format == NULL || format->store_code == PBE_NO_PACKMODE)
		return EINVAL;

	/* Refuses an address that is not word aligned or past the 40 bits. */
	if ((surface->address & 3U) != 0 || surface->address >= (1ULL << 40))
		return EINVAL;

	/* Refuses an empty attachment or area and an area that misses the attachment. */
	if (surface->width == 0 || surface->height == 0 || area->width == 0 || area->height == 0)
		return EINVAL;
	if (area->x >= surface->width || area->y >= surface->height)
		return EINVAL;

	/* The clip: the area within the attachment, inclusive. */
	min_x = area->x;
	min_y = area->y;
	max_x = surface->width - 1U;
	if (area->width - 1U < max_x - area->x)
		max_x = area->x + area->width - 1U;
	max_y = surface->height - 1U;
	if (area->height - 1U < max_y - area->y)
		max_y = area->y + area->height - 1U;
	if (max_x > PBE_CLIP_MAX || max_y > PBE_CLIP_MAX)
		return E2BIG;

	/* The source's format and the gamma. */
	error = pbe_source(format, &source_format, &gamma);
	if (error != 0)
		return error;

	/* The stride in pixels: the linear row, or the twiddled padded edge. */
	stride = surface->padded_width;
	if (surface->linear) {
		if (format->texel_bytes == 0 || surface->row_pitch % format->texel_bytes != 0)
			return EINVAL;
		stride = surface->row_pitch / format->texel_bytes;
	}
	if (stride == 0 || (stride - 1U) / 2U > PBE_LINESTRIDE_MAX)
		return E2BIG;

	/* The swizzle of the format's channels. */
	error = pbe_swizzle(format, &swizzle);
	if (error != 0)
		return error;

	/* State word 0: the address's bits 2 to 33. */
	state[0] = (uint32_t)(surface->address >> 2);

	/* State word 1: the address's bits 34 to 39, the pack mode, normalised, source at bit 0. */
	state[1] = (uint32_t)((surface->address >> 34) & 0x3fU);
	state[1] |= (uint32_t)format->store_code << 7;
	state[1] |= 1U << 13;
	state[1] |= (uint32_t)source_format << 19;

	/* Register word 0: the swizzle, the left clip, gamma, the layout, the stride, tile relative. */
	reg[0] = swizzle;
	reg[0] |= (uint64_t)min_x << 6;
	reg[0] |= (uint64_t)gamma << 4;
	if (surface->linear) {
		reg[0] |= (uint64_t)PBE_LAYOUT_LINEAR << 32;
	} else {
		reg[0] |= (uint64_t)PBE_LAYOUT_TWIDDLE << 32;
	}
	reg[0] |= (uint64_t)((stride - 1U) / 2U) << 34;
	reg[0] |= 1ULL << 57;

	/* Register word 1: the other clips and, when twiddled, the log2 edges. */
	reg[1] = (uint64_t)max_y;
	reg[1] |= (uint64_t)max_x << 32;
	reg[1] |= (uint64_t)min_y << 46;
	if (!surface->linear) {
		reg[1] |= (uint64_t)log2_ceiling(surface->height) << 28;
		reg[1] |= (uint64_t)log2_ceiling(surface->width) << 60;
	}

	/* Register word 2: nothing. */
	reg[2] = 0;

	/* Succeeded: the five words. */
	return 0;
}

/*
 * Packs the test triangle's primitive state block: the clear template
 * for a colour attachment (a translucent triangle that always passes the
 * depth test and writes no depth, no stencil), the fragment's data
 * sequencer state, no region clip, the four-word pretransformed vertex
 * with its w, no varyings, no culling and no stream output.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/rogue/pvr_clear.c
 * (pvr_device_setup_graphics_static_clear_ppp_base,
 * pvr_device_setup_graphics_static_clear_ppp_templates for the colour
 * variant, pvr_emit_ppp_from_template: the header, ISPCTL, ISPA, ISPB,
 * the six data sequencer words, the two region clip words, the w clamp,
 * the output select, the three varying words, the control and the
 * stream output, in that order); src/imagination/vulkan/
 * pvr_arch_cmd_buffer.c (pvr_cs_write_load_op_for_view: the six data
 * sequencer words: SHADERBASE the fragment program's data, TEXUNICODEBASE
 * the shared program's code, SIZEINFO1 its data size and the
 * temporaries, SIZEINFO2 the shared registers, VARYINGBASE 0,
 * TEXTUREDATABASE the shared program's data).
 */
int
powervr_render_triangle_ppp(
	const struct powervr_render_triangle *triangle,
	uint32_t words[POWERVR_RENDER_PPP_WORDS])
{
	const struct powervr_render_background *fragment;
	uint32_t states;
	uint32_t shared;
	unsigned index;

	/* Refuses missing input or result, and offsets not on 16-byte boundaries. */
	if (triangle == NULL || words == NULL)
		return EINVAL;
	fragment = &triangle->fragment;
	if ((fragment->kick_offset & ~OFFSET_MASK) != 0 ||
	    (fragment->shared_code_offset & ~OFFSET_MASK) != 0 ||
	    (fragment->shared_data_offset & ~OFFSET_MASK) != 0)
		return EINVAL;

	/* Refuses sizes the fields cannot hold. */
	states = round_units(fragment->shared_data_words, 4U);
	shared = round_units(fragment->shared_registers, 16U);
	if (states > STATE_SIZE_MAX || shared > SHARED_SIZE_MAX)
		return EINVAL;

	/* The header and the three depth and stencil words. */
	index = 0;
	words[index++] = PPP_HEADER_PRESENT;
	words[index++] = PPP_ISPCTL_BPRES;
	words[index++] = PPP_ISPA_TRANSLUCENT | PPP_ISPA_NO_DEPTH_WRITE | PPP_ISPA_ALWAYS;
	words[index++] = PPP_ISPB_ALWAYS;

	/* The six data sequencer words. */
	words[index++] = fragment->kick_offset;
	words[index++] = fragment->shared_code_offset;
	words[index++] = states << 16;
	words[index++] = shared << 23;
	words[index++] = 0;
	words[index++] = fragment->shared_data_offset;

	/* The region clip that clips nothing away. */
	words[index++] = PPP_CLIP_OUTSIDE | PPP_CLIP_MAX;
	words[index++] = PPP_CLIP_MAX;

	/* The w clamp and the output select: four words a vertex with the w. */
	words[index++] = PPP_WCLAMP;
	words[index++] = (PPP_OUTPUT_VERTEX_WORDS << 24) | PPP_OUTPUT_RHW;

	/* No varyings. */
	words[index++] = 0;
	words[index++] = 0;
	words[index++] = 0;

	/* The control (pretransformed, no culling) and no stream output. */
	words[index++] = PPP_PRETRANSFORM;
	words[index++] = 0;

	/* Succeeded: the block, all POWERVR_RENDER_PPP_WORDS words. */
	return 0;
}

/*
 * Fills a render's terminate state: the header with only the terminate
 * present, the clip's right and bottom in 32-pixel blocks less one, and
 * render target 0 with the clip's left at 0.
 *
 * Procedure referenced:
 * Mesa 25.0.0 src/imagination/vulkan/pvr_device.c
 * (pvr_framebuffer_create_ppp_state).
 * Values referenced:
 * Mesa 25.0.7 src/imagination/csbgen/rogue_ppp.xml (STATE_HEADER
 * pres_terminate bit 25; STATE_TERMINATE0 clip_right bits 18..26 and
 * clip_bottom 0..8, BLOCK_SIZE_IN_PIXELS 32; STATE_TERMINATE1).
 */
void
powervr_render_terminate_state(
	uint32_t width,
	uint32_t height,
	uint32_t state[POWERVR_RENDER_TERMINATE_STATE_WORDS])
{
	uint32_t right;
	uint32_t bottom;

	/* The clip in whole 32-pixel blocks, less one. */
	right = (width + TERMINATE_BLOCK_PIXELS - 1U) / TERMINATE_BLOCK_PIXELS - 1U;
	bottom = (height + TERMINATE_BLOCK_PIXELS - 1U) / TERMINATE_BLOCK_PIXELS - 1U;
	state[0] = TERMINATE_PRESENT;
	state[1] = ((right & TERMINATE_CLIP_MASK) << TERMINATE_RIGHT_SHIFT) | (bottom & TERMINATE_CLIP_MASK);
	state[2] = 0;
}

/*
 * Packs the end of a render's vertex control stream: the primitive state
 * block naming the terminate state, then the stream's end.
 *
 * Procedure referenced:
 * Mesa 25.0.0 src/imagination/vulkan/pvr_cmd_buffer.c
 * (pvr_sub_cmd_gfx_build_terminate_ctrl_stream and
 * pvr_cmd_buffer_emit_ppp_state: PPP_STATE0 with the word count and the
 * address's high byte, PPP_STATE1 its low word, then the terminate).
 */
int
powervr_render_terminate_stream(
	uint64_t state_address,
	uint32_t words[POWERVR_RENDER_TERMINATE_STREAM_WORDS])
{
	/* Refuses a misaligned or out-of-range state. */
	if ((state_address & 3U) != 0 || state_address >= (1ULL << 40))
		return EINVAL;

	/* The block, then the end. */
	words[0] = (VDM_PPP_STATE << VDM_BLOCK_SHIFT) | (POWERVR_RENDER_TERMINATE_STATE_WORDS << 8) | (uint32_t)((state_address >> 32) & 0xffU);
	words[1] = (uint32_t)state_address & ~3U;
	words[2] = VDM_TERMINATE << VDM_BLOCK_SHIFT;

	/* Succeeded: the three words. */
	return 0;
}

/*
 * Packs the test triangle's vertex control stream: the primitive state
 * block, the vertex state (its data sequencer program and the sizes of
 * a four-word output vertex of three input words), a list of three
 * vertices without an index buffer, and the stream's end (the terminate
 * state's block, then the end).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/rogue/pvr_clear.c
 * (pvr_emit_ppp_from_template: PPP_STATE0 word_count and the high
 * address, PPP_STATE1 the low address; pvr_pack_clear_vdm_state:
 * VDM_STATE0 with the data address and others present, the cam size,
 * scratch FIVE, flat shading from vertex 0; VDM_STATE2 the data,
 * VDM_STATE3 the code, VDM_STATE4 the output size, VDM_STATE5 the
 * instances, the unified store of the input vertex, the temporaries and
 * the data; INDEX_LIST0 with the count present, INDEX_LIST2 the count);
 * src/imagination/vulkan/pvr_arch_csb.c (pvr_arch_csb_emit_terminate:
 * VDMCTRL_STREAM_TERMINATE).
 */
int
powervr_render_triangle_vdm(
	const struct powervr_render_triangle *triangle,
	uint32_t words[POWERVR_RENDER_VDM_WORDS])
{
	uint32_t unified;
	uint32_t temps;
	uint32_t data;
	unsigned index;
	int error;

	/* Refuses missing input or result, a misaligned block and misaligned programs. */
	if (triangle == NULL || words == NULL)
		return EINVAL;
	if ((triangle->ppp_address & 3U) != 0 || triangle->ppp_address >= (1ULL << 40))
		return EINVAL;
	if ((triangle->vertex_data_offset & ~OFFSET_MASK) != 0 || (triangle->vertex_code_offset & ~OFFSET_MASK) != 0)
		return EINVAL;

	/* The sizes in their units, refused when the fields cannot hold them. */
	unified = round_units(POWERVR_RENDER_VERTEX_WORDS * 4U, VDM_UNIT_BYTES);
	temps = round_units(triangle->vertex_temps, VDM_UNIT_BYTES);
	data = round_units(triangle->vertex_data_words * 4U, VDM_UNIT_BYTES);
	if (temps > VDM_FIELD4_MAX || data > VDM_FIELD6_MAX)
		return EINVAL;

	/* The primitive state block: its words and address. */
	index = 0;
	words[index++] = (VDM_PPP_STATE << VDM_BLOCK_SHIFT) | (POWERVR_RENDER_PPP_WORDS << 8) | (uint32_t)((triangle->ppp_address >> 32) & 0xffU);
	words[index++] = (uint32_t)triangle->ppp_address & ~3U;

	/* The vertex state. */
	words[index++] = (VDM_VDM_STATE << VDM_BLOCK_SHIFT) | VDM_DATA_PRESENT | VDM_OTHER_PRESENT | (VDM_CAM_SIZE << 7);
	words[index++] = triangle->vertex_data_offset;
	words[index++] = triangle->vertex_code_offset;
	words[index++] = PPP_OUTPUT_VERTEX_WORDS;
	words[index++] = (VDM_MAX_INSTANCES << 25) | (unified << 10) | (temps << 6) | data;

	/* Three vertices as a triangle list, no index buffer. */
	words[index++] = (VDM_INDEX_LIST << VDM_BLOCK_SHIFT) | VDM_COUNT_PRESENT | VDM_TRIANGLE_LIST;
	words[index++] = TRIANGLE_VERTICES;

	/* The end of the stream: the terminate state, then the end. */
	error = powervr_render_terminate_stream(triangle->terminate_address, &words[index]);
	if (error != 0)
		return error;

	/* Succeeded: the stream, all POWERVR_RENDER_VDM_WORDS words. */
	return 0;
}

/*
 * Builds a bitwise group that moves a 32-bit immediate into a temporary.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_map.py (group_map O_MOVI32: a
 * bitwise group with phase 0 only, the first write enabled, the
 * immediate bypass with the count from s2).
 */
static int
immediate_group(
	struct powervr_usc_group *group,
	unsigned destination,
	uint32_t value)
{
	int error;

	/* A bitwise group writing one temporary. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_BITWISE;
	group->write_0 = 1;
	temporary(&group->destination[0], destination);

	/* The first phase carries the immediate. */
	error = powervr_usc_bit_immediate(value, 1, USC_COUNT_FROM_S2, USC_COUNT_PASS, USC_SHIFT_PASS, &group->operation[POWERVR_USC_PHASE_0]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Builds a main group that copies one register into another.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_map.py (group_map O_MBYP: phase 0
 * passes s0, the first write enabled, is4 from ft0).
 */
static int
move_group(
	struct powervr_usc_group *group,
	unsigned bank,
	unsigned source,
	unsigned destination_bank,
	unsigned destination)
{
	int error;

	/* A main group from one source to one destination. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_MAIN;
	group->write_0 = 1;
	group->source[0].used = 1;
	group->source[0].bank = (uint8_t)bank;
	group->source[0].index = (uint16_t)source;
	group->destination[0].used = 1;
	group->destination[0].bank = (uint8_t)destination_bank;
	group->destination[0].index = (uint16_t)destination;
	group->internal[4] = USC_SELECT_FT0;

	/* The first phase passes the source through. */
	error = powervr_usc_single(POWERVR_USC_SINGLE_PASS, 0, 0, &group->operation[POWERVR_USC_PHASE_0]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Builds a main group that writes one register to a vertex output.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_map.py (group_map
 * O_UVSW_WRITE: the back end only, s0 through the source selector to
 * is4 from fte, the write with an immediate output address).
 */
static int
vertex_write_group(
	struct powervr_usc_group *group,
	unsigned bank,
	unsigned source,
	unsigned address)
{
	int error;

	/* s0 reaches the back end through the source selector and fte. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_MAIN;
	group->source[0].used = 1;
	group->source[0].bank = (uint8_t)bank;
	group->source[0].index = (uint16_t)source;
	group->mux_used = 1;
	group->mux = 0;
	group->internal[4] = USC_SELECT_FTE;

	/* The write to the output at its address. */
	error = powervr_usc_vertex_output(USC_UVSW_WRITE, 0, 0, 1, address, &group->operation[POWERVR_USC_PHASE_BACKEND]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/* Names a temporary as an operand. */
static void
temporary(
	struct powervr_usc_register *operand,
	unsigned index)
{
	/* The temporary bank and the register's number. */
	operand->used = 1;
	operand->bank = POWERVR_USC_BANK_TEMPORARY;
	operand->index = (uint16_t)index;
}

/* Encodes groups into a program and records its temporaries. */
static int
encode(
	const struct powervr_usc_group *groups,
	size_t count,
	unsigned temps,
	struct powervr_render_usc *program)
{
	size_t length;
	int error;

	/* Encodes, padded for the instruction cache. */
	error = powervr_usc_program_encode(groups, count, program->code, sizeof(program->code), &length);
	if (error != 0)
		return error;

	/* Succeeded: the bytes and the temporaries. */
	program->length = length;
	program->temps = temps;
	return 0;
}

/* Rounds a size up to whole units. */
static uint32_t
round_units(
	uint32_t value,
	uint32_t unit)
{
	/* The units that hold the size. */
	return (value + unit - 1U) / unit;
}

/*
 * Picks the pixel back end's source format and gamma for a colour
 * format: sRGB from 16-bit floats with gamma, a 16-bit float format from
 * its half floats, a raw format of 64 or 128 bits (the transfer's
 * copies, integer words) from its words, an 8-bit unorm channel from the
 * packed 8 bits.  Refuses depth formats and colour formats of other
 * channel widths (ENOTSUP).
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_formats.c
 * (pvr_arch_pbe_get_src_format_and_gamma: a 16-bit float channel takes
 * F16_PER_CHANNEL; a format with 32-bit channels, or an integer one,
 * 8_PER_CHANNEL: the source words taken whole); on the board a
 * raw R32G32/R32G32B32A32 store with F16_PER_CHANNEL repeated the first
 * half of the source.
 */
static int
pbe_source(
	const struct powervr_format *format,
	unsigned *source_format,
	unsigned *gamma)
{
	/* Refuses a format that is not colour. */
	if ((format->kind & POWERVR_FORMAT_COLOR) == 0)
		return ENOTSUP;

	/* sRGB: 16-bit floats with gamma. */
	if ((format->kind & POWERVR_FORMAT_SRGB) != 0) {
		*source_format = PBE_SOURCE_F16;
		*gamma = 1;
		return 0;
	}

	/* The 16-bit float colour: its half floats, two pixel output registers. */
	if (format->vk_format == VK_FORMAT_R16G16B16A16_SFLOAT) {
		*source_format = PBE_SOURCE_F16;
		*gamma = 0;
		return 0;
	}

	/* Another format of two or four words (a raw copy's, or 32-bit channels): the words as they are, 8_PER_CHANNEL. */
	if (format->texel_bytes == 8 || format->texel_bytes == 16) {
		*source_format = PBE_SOURCE_8;
		*gamma = 0;
		return 0;
	}

	/* Refuses other than one byte a channel (the table's unorm colour formats). */
	if (format->texel_bytes == 0 || format->texel_bytes > 4)
		return ENOTSUP;

	/* Succeeded: an 8-bit unorm channel, packed. */
	*source_format = PBE_SOURCE_8;
	*gamma = 0;
	return 0;
}

/*
 * Places the swizzle fields of register word 0: each source channel's
 * place in the stored pixel, and the constants of the channels the
 * format lacks.
 */
static int
pbe_swizzle(
	const struct powervr_format *format,
	uint64_t *word)
{
	uint64_t fields[4];
	unsigned source;
	unsigned channel;

	/* Every stored channel takes source channel 0 until a source names it. */
	for (channel = 0; channel < 4; channel++)
		fields[channel] = 0;

	/* Each source channel to the stored channel the format maps it to. */
	for (source = 0; source < 4; source++) {
		channel = format->channels[source];
		if (channel <= POWERVR_CHANNEL_3) {
			fields[channel] = source;
		} else if (channel == POWERVR_CHANNEL_ZERO) {
			fields[source] = PBE_SWIZ_ZERO;
		} else if (channel == POWERVR_CHANNEL_ONE) {
			fields[source] = PBE_SWIZ_ONE;
		} else {
			return ENOTSUP;
		}
	}

	/* The four fields in their places. */
	*word = (fields[0] << 20) | (fields[1] << 23) | (fields[2] << 26) | (fields[3] << 29);

	/* Succeeded: the swizzle. */
	return 0;
}

/* The log2 of a value rounded up to a power of two. */
static unsigned
log2_ceiling(
	uint32_t value)
{
	unsigned bits;

	/* Counts the doublings that reach the value. */
	bits = 0;
	while (bits < 31 && (1U << bits) < value)
		bits++;

	/* The count. */
	return bits;
}
