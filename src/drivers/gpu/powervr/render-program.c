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

/*
 * The registers and fields of the tile buffers' programs: the tiled
 * offset special registers of a pixel's words (loads, stores), the
 * indexed bank's encodings of index register 0 itself and of the pixel
 * output registers from index register 0, the main unit's source
 * selector of s1, the 64-bit add of a 32-bit word, a 32-bit store, and
 * the tile's words one indexed load takes and the burst register's code
 * of the most.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_isa.py (SR tiled_ld_comp0 54,
 * tiled_st_comp0 58; F_IDXBANK idx 0b101, pixout 0b111 and the offset
 * from bit 3; F_IS0_SEL s1 0b100; F_INT32_64_OP add6432 0b00; F_DSIZE
 * 32bit 0b10), pco_map.h.py (pco_map_reg_index: an indexed register's
 * bank and offset << 3) and pco_trans_nir.c (trans_flush_tile_buffer:
 * the burst of 1024 words as 0).
 */
#define USC_TILED_LOAD		54U
#define USC_TILED_STORE		58U
#define USC_INDEX_0_ITSELF	5U
#define USC_INDEXED_PIXEL_OUTPUT	7U
#define USC_MUX_S1		4U
#define USC_SIZE_32		2U
#define TILE_LOAD_WORDS_MAX	1024U

/*
 * The pixels of a tile of the BXE-4-32, whose tile buffer words the end
 * of the tile loads per pixel output register (one shading cluster a
 * tile with pbe2_in_xe; the samples' words take no more room with one
 * sample a pixel in the ISP).
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/common/device_info/bxe-4-32.h
 * (tile_size_x 16, tile_size_y 16, has_pbe2_in_xe,
 * isp_samples_per_pixel 1) and src/imagination/vulkan/pvr_usc.c
 * (pvr_usc_eot: tile_size_x * tile_size_y * num_output_regs /
 * rogue_num_uscs_per_tile words, the same with several samples when the
 * ISP takes one sample a pixel).
 */
#define TILE_PIXELS		256U

/* The bits of 1.0f, the test vertex's w. */
#define FLOAT_ONE		0x3f800000U

/*
 * The shared register the background's colour is written to, and the
 * data sequencer's instance and full sample rates and the rate's place
 * in the task word.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_usc.c (pvr_uscgen_loadop: the
 * clear values from shared register 0 up);
 * src/imagination/vulkan/pds/pvr_rogue_pds_defs.h
 * (PVR_ROGUE_PDSINST_DOUTU_SAMPLE_RATE_INSTANCE 0, _FULL 2,
 * PVR_ROGUE_PDSINST_DOUT_FIELDS_DOUTU_SRC0_SAMPLE_RATE_SHIFT 33).
 */
#define CLEAR_SHARED		0U
#define SAMPLE_RATE_INSTANCE	0U
#define SAMPLE_RATE_FULL	2U
#define SAMPLE_RATE_SHIFT	33U

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
 * TWIDDLE_2D 1; SOURCE_FORMAT F16_PER_CHANNEL 0, 8_PER_CHANNEL 1) and
 * Mesa 26.2.6 src/imagination/csbgen/rogue/pbestate.xml (REG_WORD0
 * size_z 52..55; REG_WORD1 zslice 14..27; MEMLAYOUT TWIDDLE_3D 2;
 * STATE_WORD1 emptytile 6, source_pos 14..15, mrt_index 16..18).
 */
#define PBE_CLIP_MAX		0x3fffU
#define PBE_LINESTRIDE_MAX	0xffffU
#define PBE_SWIZ_ONE		4U
#define PBE_SWIZ_ZERO		5U
#define PBE_LAYOUT_LINEAR	0U
#define PBE_LAYOUT_TWIDDLE	1U
#define PBE_LAYOUT_TWIDDLE_3D	2U
#define PBE_SLICE_MAX		0x3fffU
#define PBE_SOURCE_F16		0U
#define PBE_SOURCE_8		1U
#define PBE_NO_PACKMODE		0xffU
#define PBE_SOURCE_POS_SHIFT	14U
#define PBE_SOURCE_POS_MAX	3U
#define PBE_MRT_INDEX_SHIFT	16U
#define PBE_MRT_INDEX_MAX	7U
#define PBE_EMPTY_TILE		(1U << 6)

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
static int special_move_group(struct powervr_usc_group *group, unsigned special, unsigned destination, unsigned overlap);
static int add64_group(struct powervr_usc_group *group, unsigned bank, unsigned low, unsigned high, unsigned offset, unsigned destination);
static int store_tiled_group(struct powervr_usc_group *group, unsigned data, unsigned address, unsigned mask);
static int save_mask_group(struct powervr_usc_group *group, unsigned mode, unsigned destination);
static int control_group(struct powervr_usc_group *group, unsigned control, unsigned end);
static int tile_load_groups(struct powervr_usc_group *groups, uint64_t address, uint32_t registers, unsigned temp, size_t *at);
static int move_group(struct powervr_usc_group *group, unsigned bank, unsigned source, unsigned destination_bank, unsigned destination);
static int vertex_write_group(struct powervr_usc_group *group, unsigned bank, unsigned source, unsigned address);
static void temporary(struct powervr_usc_register *operand, unsigned index);
static int encode(const struct powervr_usc_group *groups, size_t count, unsigned temps, struct powervr_render_usc *program);
static uint32_t round_units(uint32_t value, uint32_t unit);
static int pbe_source(const struct powervr_format *format, unsigned *source_format, unsigned *gamma);
static int pbe_swizzle(const struct powervr_format *format, uint64_t *word);
static unsigned log2_ceiling(uint32_t value);

/*
 * Builds the end-of-tile shading cluster program of one emit: the two
 * state words of the pixel back end moved into temporaries, then the
 * emission of the tile with them, freeing the partition and ending the
 * program (powervr_render_eot_usc_emits of one).
 */
int
powervr_render_eot_usc(
	const uint32_t state[2],
	struct powervr_render_usc *program)
{
	int error;

	/* Refuses missing words or result. */
	if (state == NULL || program == NULL)
		return EINVAL;

	/* The program of the one emit. */
	error = powervr_render_eot_usc_emits((const uint32_t (*)[2])state, 1, program);
	if (error != 0)
		return error;

	/* Succeeded: the end-of-tile program. */
	return 0;
}

/*
 * Builds the end-of-tile shading cluster program of count emits (one a
 * colour attachment): for each, a wait for the emit before it to leave
 * the output (but for the first), its two state words of the pixel back
 * end moved into temporaries of its own, then the emission of the tile
 * with them; the last emit frees the partition and ends the program.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_usc.c (pvr_usc_eot: for each
 * emit after the first a wop, then state0 and state1 as immediates and
 * emitpix, freep on the last emit, then return; olchk_skip);
 * src/imagination/pco/pco_map.py (group_map O_MOVI32: a bitwise group,
 * phase 0, the first write; group_map O_EMITPIX: a main group with the
 * back end only, s0 and s2, the end flag; group_map O_WOP: a control
 * group naming wop, no operation bytes).
 */
int
powervr_render_eot_usc_emits(
	const uint32_t (*states)[2],
	uint32_t count,
	struct powervr_render_usc *program)
{
	struct powervr_usc_group groups[POWERVR_RENDER_EMITS_MAX * 4U];
	uint32_t emit;
	size_t at;
	int error;

	/* Refuses missing words or result, and no emit or more than the core's. */
	if (states == NULL || program == NULL || count == 0 || count > POWERVR_RENDER_EMITS_MAX)
		return EINVAL;

	/* Each emit after a wait for the one before, from temporaries 2 * emit and 2 * emit + 1. */
	at = 0;
	for (emit = 0; emit < count; emit++) {
		/* The wait for the emit before to leave the output. */
		if (emit != 0) {
			groups[at] = blank_group;
			groups[at].unit = POWERVR_USC_UNIT_CONTROL;
			groups[at].control = POWERVR_USC_CONTROL_WAIT_OUTPUT;
			error = powervr_usc_empty(&groups[at].operation[POWERVR_USC_PHASE_0]);
			if (error != 0)
				return error;
			at++;
		}

		/* The two state words into the emit's temporaries. */
		error = immediate_group(&groups[at], emit * 2U, states[emit][0]);
		if (error != 0)
			return error;
		at++;
		error = immediate_group(&groups[at], emit * 2U + 1U, states[emit][1]);
		if (error != 0)
			return error;
		at++;

		/* The emission from s0 and s2; the last one frees the partition and ends the program. */
		groups[at] = blank_group;
		groups[at].unit = POWERVR_USC_UNIT_MAIN;
		groups[at].end = emit + 1U == count;
		temporary(&groups[at].source[0], emit * 2U);
		temporary(&groups[at].source[2], emit * 2U + 1U);
		error = powervr_usc_emit_pixel(emit + 1U == count, &groups[at].operation[POWERVR_USC_PHASE_BACKEND]);
		if (error != 0)
			return error;
		at++;
	}

	/* Encodes the groups with their temporaries. */
	error = encode(groups, at, count * 2U, program);
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
 * one to four words (the pixel output registers of the render's colour
 * attachments, powervr_format_output_words at each one's place): the
 * words in shared registers 0 up moved into pixel output registers 0 up,
 * the first under the overlap check, ending the program.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_usc.c (pvr_uscgen_loadop with
 * the cleared render targets: each of their words from the shared
 * registers, nir_load_preamble, stored to the output registers);
 * src/imagination/pco/pco_map.py (group_map O_MBYP: phase 0 passes s0,
 * the first write, is4 from ft0).
 */
int
powervr_render_clear_usc_words(
	uint32_t words,
	struct powervr_render_usc *program)
{
	struct powervr_usc_group groups[POWERVR_CLEAR_TRIAL_PAD + POWERVR_RENDER_CLEAR_WORDS_MAX];
	uint32_t pads;
	uint32_t pad;
	uint32_t word;
	int error;

	/* Refuses a missing result and a clear of no word or of more than the output registers. */
	if (program == NULL || words == 0 || words > POWERVR_RENDER_CLEAR_WORDS_MAX)
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
 * Builds the data sequencer program that writes a clear's one to four
 * words (powervr_render_clear_usc_words) to shared registers 0 up for
 * the clear background, two words a write.
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
	struct powervr_data_load loads[(POWERVR_RENDER_CLEAR_WORDS_MAX + 1U) / 2U];
	uint32_t count;
	uint32_t word;
	int error;

	/* Refuses a missing input or result and a clear of no word or of more than the output registers. */
	if (colour == NULL || program == NULL || words == 0 || words > POWERVR_RENDER_CLEAR_WORDS_MAX)
		return EINVAL;

	/* The words two a write to an even shared register, the first of a pair in the low half. */
	count = 0;
	for (word = 0; word < words; word += 2U) {
		loads[count].kind = POWERVR_DATA_LOAD_WRITE;
		loads[count].shared = (uint16_t)(CLEAR_SHARED + word);
		loads[count].words = (uint16_t)(words - word >= 2U ? 2U : 1U);
		loads[count].source = colour[word];
		if (loads[count].words == 2U)
			loads[count].source |= (uint64_t)colour[word + 1U] << 32;
		count++;
	}
	error = powervr_data_shared_build(loads, count, program);
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
 * Builds the end-of-tile program of count emits as
 * powervr_render_eot_usc_emits does, an emit whose attachment lies in a
 * tile buffer (tile_addresses[emit] its address, 0 for an emit from the
 * pixel output registers) first loading the tile's words of the buffer
 * into the pixel output registers, registers (1, 2 or 4) a pixel, and
 * waiting for them.  The emits from the registers come first (the
 * caller's order), so a load overwrites no register an emit still reads;
 * each emit after the first waits for the one before to leave the
 * output.  The state words of emit e are in temporaries 2e and 2e + 1,
 * the loads' words after them.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_usc.c (pvr_usc_eot: for each
 * emit after the first a wop; an emit with a tile buffer address first
 * flush_tile_buffer of tile_size_x * tile_size_y * num_output_regs /
 * rogue_num_uscs_per_tile words in bursts of 1024 from the address,
 * each burst's pixel output index the offset divided by
 * rogue_usc_indexed_pixel_output_index_scale; then the state words and
 * emitpix, freep on the last) and src/imagination/pco/pco_trans_nir.c
 * (trans_flush_tile_buffer: add64_32 of the address and tiled_ld_comp0,
 * movi32 of index register 0 to the burst's index, movi32 of the burst
 * length (1024 as 0), ld.regbl into pixel output 0 indexed by index
 * register 0) and pvr_arch_cmd_buffer.c
 * (pvr_setup_emit_state: the output register emits before the tile
 * buffer ones).
 */
int
powervr_render_eot_usc_tiles(
	const uint32_t (*states)[2],
	const uint64_t *tile_addresses,
	uint32_t count,
	uint32_t registers,
	struct powervr_render_usc *program)
{
	struct powervr_usc_group groups[POWERVR_RENDER_EMITS_MAX * 12U];
	unsigned temps;
	uint32_t emit;
	size_t at;
	int error;

	/* Refuses missing words or result, no emit or more than the core's, and registers a pixel has not. */
	if (states == NULL || tile_addresses == NULL || program == NULL || count == 0 || count > POWERVR_RENDER_EMITS_MAX)
		return EINVAL;
	if (registers != 1U && registers != 2U && registers != 4U)
		return EINVAL;

	/* Each emit after a wait for the one before; a tile buffer's words loaded first. */
	at = 0;
	temps = count * 2U;
	for (emit = 0; emit < count; emit++) {
		/* The wait for the emit before to leave the output. */
		if (emit != 0) {
			error = control_group(&groups[at], POWERVR_USC_CONTROL_WAIT_OUTPUT, 0);
			if (error != 0)
				return error;
			at++;
		}

		/* The tile buffer's words of the tile into the pixel output registers. */
		if (tile_addresses[emit] != 0) {
			error = tile_load_groups(groups, tile_addresses[emit], registers, count * 2U, &at);
			if (error != 0)
				return error;
			temps = count * 2U + 6U;
		}

		/* The two state words into the emit's temporaries. */
		error = immediate_group(&groups[at], emit * 2U, states[emit][0]);
		if (error != 0)
			return error;
		at++;
		error = immediate_group(&groups[at], emit * 2U + 1U, states[emit][1]);
		if (error != 0)
			return error;
		at++;

		/* The emission from s0 and s2; the last one frees the partition and ends the program. */
		groups[at] = blank_group;
		groups[at].unit = POWERVR_USC_UNIT_MAIN;
		groups[at].end = emit + 1U == count;
		temporary(&groups[at].source[0], emit * 2U);
		temporary(&groups[at].source[2], emit * 2U + 1U);
		error = powervr_usc_emit_pixel(emit + 1U == count, &groups[at].operation[POWERVR_USC_PHASE_BACKEND]);
		if (error != 0)
			return error;
		at++;
	}

	/* Encodes the groups with their temporaries. */
	error = encode(groups, at, temps, program);
	if (error != 0)
		return error;

	/* Succeeded: the end-of-tile program. */
	return 0;
}

/*
 * Builds the background's shading cluster program of a render with
 * colour attachments in tile buffers: the words pixel output registers'
 * moves from shared registers 0 up (the first under the overlap check),
 * as powervr_render_clear_usc_words; then each tile buffer clear's words
 * stored into the pixel's words of its buffer: the buffer's address into
 * temporaries, then for each word its tiled offset (under the overlap
 * check) added to the address, the word from its shared register right
 * after the address pair, stored
 * for the pixel (the instance's vector mask, or its covered samples when
 * samples is 4) and waited for before the next word reuses the address
 * and data temporaries (the store reads them after it is issued: without
 * the wait, earlier words of several were lost on the board); then the
 * end.  The clears' shared words lie as powervr_render_clear_shared_tiles
 * puts them.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_usc.c (pvr_uscgen_loadop: the
 * cleared render targets' words from the shared registers stored to
 * their output registers or tile buffers, the tile buffers' addresses in
 * shared registers after the clear words) and
 * src/imagination/pco/pco_trans_nir.c (trans_store_output_fs: a tile
 * buffer output stored by add64_32 of the buffer's address and the
 * component's tiled_st_comp, the data as the vector's third word, savmsk
 * vm for one sample and icm for several, st_tiled of 32 bits) and
 * src/imagination/pco/pco_schedule.c (pco_schedule: a wdf on the data
 * return counter after every instruction that names one, st_tiled's
 * drc 0 among them).
 */
int
powervr_render_clear_usc_tiles(
	uint32_t words,
	const struct powervr_render_tile_clear *clears,
	uint32_t count,
	uint32_t samples,
	struct powervr_render_usc *program)
{
	struct powervr_usc_group groups[POWERVR_RENDER_CLEAR_WORDS_MAX + POWERVR_RENDER_EMITS_MAX * (2U + POWERVR_RENDER_CLEAR_WORDS_MAX * 6U) + 1U];
	uint32_t clear;
	uint32_t word;
	uint32_t block;
	unsigned mode;
	size_t at;
	int error;

	/* Refuses a missing result, register words past the registers, and clears past the attachments or of no word. */
	if (program == NULL || words == 0 || words > POWERVR_RENDER_CLEAR_WORDS_MAX || count > POWERVR_RENDER_EMITS_MAX || (count != 0 && clears == NULL))
		return EINVAL;
	for (clear = 0; clear < count; clear++) {
		if (clears[clear].words == 0 || clears[clear].words > POWERVR_RENDER_CLEAR_WORDS_MAX || clears[clear].offset + clears[clear].words > POWERVR_RENDER_CLEAR_WORDS_MAX)
			return EINVAL;
	}

	/* The register words, the first move checked. */
	for (word = 0; word < words; word++) {
		error = move_group(&groups[word], POWERVR_USC_BANK_SHARED, CLEAR_SHARED + word, POWERVR_USC_BANK_SPECIAL, USC_PIXEL_OUTPUT + word);
		if (error != 0)
			return error;
	}
	groups[0].overlap_check = 1;
	at = words;

	/* The pixel's mask a tiled store writes through. */
	mode = POWERVR_USC_MASK_VECTOR;
	if (samples == 4U)
		mode = POWERVR_USC_MASK_COVERAGE;

	/* Each clear: its buffer's address into temporaries 5 and 6; each word the offset, the address, the word after the pair, the mask, the store and its wait. */
	for (clear = 0; clear < count; clear++) {
		block = ((words + 1U) & ~1U) + clear * POWERVR_RENDER_TILE_CLEAR_WORDS;
		error = move_group(&groups[at], POWERVR_USC_BANK_SHARED, block, POWERVR_USC_BANK_TEMPORARY, 5);
		if (error != 0)
			return error;
		at++;
		error = move_group(&groups[at], POWERVR_USC_BANK_SHARED, block + 1U, POWERVR_USC_BANK_TEMPORARY, 6);
		if (error != 0)
			return error;
		at++;
		for (word = 0; word < clears[clear].words; word++) {
			error = special_move_group(&groups[at], USC_TILED_STORE + clears[clear].offset + word, 0, 1);
			if (error != 0)
				return error;
			at++;
			error = add64_group(&groups[at], POWERVR_USC_BANK_TEMPORARY, 5, 6, 0, 2);
			if (error != 0)
				return error;
			at++;
			error = move_group(&groups[at], POWERVR_USC_BANK_SHARED, block + 2U + word, POWERVR_USC_BANK_TEMPORARY, 4);
			if (error != 0)
				return error;
			at++;
			error = save_mask_group(&groups[at], mode, 1);
			if (error != 0)
				return error;
			at++;
			error = store_tiled_group(&groups[at], 4, 2, 1);
			if (error != 0)
				return error;
			at++;
			error = control_group(&groups[at], POWERVR_USC_CONTROL_WAIT_DATA, 0);
			if (error != 0)
				return error;
			at++;
		}
	}

	/* The end: on the last move without stores, else after the last store's wait. */
	if (at == words) {
		groups[at - 1U].end = 1;
	} else {
		error = control_group(&groups[at], POWERVR_USC_CONTROL_NOTHING, 1);
		if (error != 0)
			return error;
		at++;
	}

	/* Encodes them with the stores' seven temporaries. */
	error = encode(groups, at, count != 0 ? 7U : 0U, program);
	if (error != 0)
		return error;

	/* Succeeded: the background's program. */
	return 0;
}

/*
 * Builds the shared register program of a background with tile buffer
 * clears (powervr_render_clear_usc_tiles): the register words from
 * shared register 0, then from the first even register after them each
 * clear's block of POWERVR_RENDER_TILE_CLEAR_WORDS, its buffer's address
 * (from tile_addresses) and its words; two words a write.  Gives the
 * shared registers it fills.
 */
int
powervr_render_clear_shared_tiles(
	const uint32_t *colour,
	uint32_t words,
	const struct powervr_render_tile_clear *clears,
	uint32_t count,
	const uint64_t *tile_addresses,
	struct powervr_data_shared_program *program,
	uint32_t *shared_registers)
{
	struct powervr_data_load loads[(POWERVR_RENDER_CLEAR_WORDS_MAX + 1U) / 2U + POWERVR_RENDER_EMITS_MAX * POWERVR_RENDER_TILE_CLEAR_WORDS / 2U];
	uint32_t block[POWERVR_RENDER_TILE_CLEAR_WORDS];
	uint64_t address;
	uint32_t loaded;
	uint32_t clear;
	uint32_t base;
	uint32_t word;
	int error;

	/* Refuses missing inputs or results, register words past the registers, and clears past the attachments. */
	if (colour == NULL || program == NULL || shared_registers == NULL || words == 0 || words > POWERVR_RENDER_CLEAR_WORDS_MAX)
		return EINVAL;
	if (count > POWERVR_RENDER_EMITS_MAX || (count != 0 && (clears == NULL || tile_addresses == NULL)))
		return EINVAL;

	/* The register words two a write to an even shared register, the first of a pair in the low half. */
	loaded = 0;
	for (word = 0; word < words; word += 2U) {
		loads[loaded].kind = POWERVR_DATA_LOAD_WRITE;
		loads[loaded].shared = (uint16_t)(CLEAR_SHARED + word);
		loads[loaded].words = (uint16_t)(words - word >= 2U ? 2U : 1U);
		loads[loaded].source = colour[word];
		if (loads[loaded].words == 2U)
			loads[loaded].source |= (uint64_t)colour[word + 1U] << 32;
		loaded++;
	}

	/* Each clear's block: its buffer's address and its words, in pairs. */
	base = (words + 1U) & ~1U;
	for (clear = 0; clear < count; clear++) {
		if (clears[clear].buffer >= POWERVR_SHADER_TILE_BUFFERS || clears[clear].words > POWERVR_RENDER_CLEAR_WORDS_MAX)
			return EINVAL;
		address = tile_addresses[clears[clear].buffer];
		if (address == 0)
			return EINVAL;
		block[0] = (uint32_t)address;
		block[1] = (uint32_t)(address >> 32);
		for (word = 0; word < POWERVR_RENDER_CLEAR_WORDS_MAX; word++)
			block[2U + word] = word < clears[clear].words ? clears[clear].value[word] : 0;
		for (word = 0; word < POWERVR_RENDER_TILE_CLEAR_WORDS; word += 2U) {
			loads[loaded].kind = POWERVR_DATA_LOAD_WRITE;
			loads[loaded].shared = (uint16_t)(base + clear * POWERVR_RENDER_TILE_CLEAR_WORDS + word);
			loads[loaded].words = 2;
			loads[loaded].source = (uint64_t)block[word] | (uint64_t)block[word + 1U] << 32;
			loaded++;
		}
	}

	/* The program of the writes. */
	error = powervr_data_shared_build(loads, loaded, program);
	if (error != 0)
		return error;

	/* Succeeded: the program and the registers it fills. */
	*shared_registers = count != 0 ? base + count * POWERVR_RENDER_TILE_CLEAR_WORDS : words;
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
 * an 8-bit unorm format with a packed channel 8 per channel); Mesa
 * 26.2.6 src/imagination/vulkan/pvr_arch_job_common.c
 * (pvr_arch_pbe_pack_state: a 3D-twiddled surface's memlayout
 * TWIDDLE_3D, size_z log2 of its depth, zslice the slice) and
 * pvr_arch_job_transfer.c (pvr_pbe_setup_codegen_defaults: the slice of
 * a 3D-twiddled destination); Mesa 26.2.6
 * src/imagination/vulkan/pvr_arch_job_common.c (pvr_arch_pbe_pack_state:
 * norm from the surface's is_normalized) and pvr_arch_job_transfer.c
 * (pvr_pbe_setup_codegen_defaults: is_normalized only for a fully
 * normalised format, so not for an integer one).
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

	/* Refuses a linear volume, and a slice past a volume's depth or the field. */
	if (surface->depth != 0 && (surface->linear || surface->slice >= surface->depth || surface->slice > PBE_SLICE_MAX))
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

	/*
	 * State word 1: the address's bits 34 to 39, the pack mode,
	 * normalised but for an integer format (stored as its integers),
	 * source at bit 0.
	 */
	state[1] = (uint32_t)((surface->address >> 34) & 0x3fU);
	state[1] |= (uint32_t)format->store_code << 7;
	if ((format->kind & POWERVR_FORMAT_INTEGER) == 0)
		state[1] |= 1U << 13;
	state[1] |= (uint32_t)source_format << 19;

	/* Register word 0: the swizzle, the left clip, gamma, the layout, the stride, tile relative. */
	reg[0] = swizzle;
	reg[0] |= (uint64_t)min_x << 6;
	reg[0] |= (uint64_t)gamma << 4;
	if (surface->linear) {
		reg[0] |= (uint64_t)PBE_LAYOUT_LINEAR << 32;
	} else if (surface->depth != 0) {
		reg[0] |= (uint64_t)PBE_LAYOUT_TWIDDLE_3D << 32;
		reg[0] |= (uint64_t)log2_ceiling(surface->depth) << 52;
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
	if (surface->depth != 0)
		reg[1] |= (uint64_t)surface->slice << 14;

	/* Register word 2: nothing. */
	reg[2] = 0;

	/* Succeeded: the five words. */
	return 0;
}

/*
 * Places a colour attachment's pixel back end state words (from
 * powervr_render_pbe_words) at its first pixel output register (0 to 3)
 * and its index among the render's emits.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/csbgen/rogue/pbestate.xml (STATE_WORD1
 * source_pos 14..15 with START_BIT0 0 to START_BIT96 3, mrt_index
 * 16..18) and src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_setup_pbe_state: the output register as the source's start, the
 * emit's index as mrt_index; pvr_arch_pbe_pack_state).
 */
int
powervr_render_pbe_place(
	uint32_t state[2],
	uint32_t source_register,
	uint32_t emit)
{
	/* Refuses missing words, a register past the fourth and an index past the field. */
	if (state == NULL || source_register > PBE_SOURCE_POS_MAX || emit > PBE_MRT_INDEX_MAX)
		return EINVAL;

	/* The two fields, in place of what was there. */
	state[1] &= ~((PBE_SOURCE_POS_MAX << PBE_SOURCE_POS_SHIFT) | (PBE_MRT_INDEX_MAX << PBE_MRT_INDEX_SHIFT));
	state[1] |= source_register << PBE_SOURCE_POS_SHIFT;
	state[1] |= emit << PBE_MRT_INDEX_SHIFT;

	/* Succeeded: the words name their registers. */
	return 0;
}

/*
 * Packs the pixel back end words of a render without a colour attachment
 * (the depth alone): the emit's state stores nothing (an empty tile), and
 * the register words are zero.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_job_common.c
 * (pvr_arch_pbe_pack_state: a z_only_render's state word 0 zero, state
 * word 1 emptytile alone, register words zero) and
 * src/imagination/csbgen/rogue/pbestate.xml (STATE_WORD1 emptytile 6).
 */
void
powervr_render_pbe_empty(
	uint32_t state[2],
	uint64_t reg[3])
{
	/* Nothing stored. */
	state[0] = 0;
	state[1] = PBE_EMPTY_TILE;
	reg[0] = 0;
	reg[1] = 0;
	reg[2] = 0;
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
 * Builds a main group that moves a special register into a temporary,
 * under the overlap check when overlap is 1.
 *
 * Procedure referenced:
 * zedBSD src/drivers/gpu/powervr/compiler/shader-backend.c
 * (MACHINE_MOVE_SPECIAL: s1 through the source selector to is4 from
 * fte, phase 2's move); Mesa 26.2.6 src/imagination/pco/pco_legalize.c
 * (a tiled offset register read under the overlap check).
 */
static int
special_move_group(
	struct powervr_usc_group *group,
	unsigned special,
	unsigned destination,
	unsigned overlap)
{
	int error;

	/* s1 reaches phase 2's move through the source selector. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_MAIN;
	group->write_0 = 1;
	group->overlap_check = (uint8_t)overlap;
	group->source[1].used = 1;
	group->source[1].bank = POWERVR_USC_BANK_SPECIAL;
	group->source[1].index = (uint16_t)special;
	temporary(&group->destination[0], destination);
	group->mux_used = 1;
	group->mux = USC_MUX_S1;
	group->internal[4] = USC_SELECT_FTE;

	/* The move of the selected source. */
	error = powervr_usc_move(USC_SELECT_FT0, USC_SELECT_FTE, 0xf, 1, 1, 1, &group->operation[POWERVR_USC_PHASE_2]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Builds a main group that adds a 32-bit temporary to a 64-bit address
 * (its low and high words in a bank) into temporaries destination and
 * destination + 1.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_map.py (group_map O_ADD64_32:
 * the sources s0 to s2, the low word from ft0 and the high from fte);
 * zedBSD src/drivers/gpu/powervr/compiler/shader-backend.c
 * (MACHINE_ADD64).
 */
static int
add64_group(
	struct powervr_usc_group *group,
	unsigned bank,
	unsigned low,
	unsigned high,
	unsigned offset,
	unsigned destination)
{
	int error;

	/* Three sources, two destinations. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_MAIN;
	group->write_0 = 1;
	group->write_1 = 1;
	group->source[0].used = 1;
	group->source[0].bank = (uint8_t)bank;
	group->source[0].index = (uint16_t)low;
	group->source[1].used = 1;
	group->source[1].bank = (uint8_t)bank;
	group->source[1].index = (uint16_t)high;
	temporary(&group->source[2], offset);
	temporary(&group->destination[0], destination);
	temporary(&group->destination[1], destination + 1U);
	group->internal[4] = USC_SELECT_FT0;
	group->internal[5] = USC_SELECT_FTE;

	/* Phase 0's 64-bit add of a 32-bit word. */
	error = powervr_usc_integer(POWERVR_USC_INTEGER_ADD_64_FROM_32, 0, 0, 0, 0, 0, 0, 0, 0, &group->operation[POWERVR_USC_PHASE_0]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Builds a main group that stores one word as a tiled store: the data
 * from a temporary (the one right after the address pair), the address
 * pair from a temporary, the mask from a temporary.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_map.py (group_map O_ST_TILED:
 * the data in s0, the address in s3, the mask in s4).
 */
static int
store_tiled_group(
	struct powervr_usc_group *group,
	unsigned data,
	unsigned address,
	unsigned mask)
{
	int error;

	/* The back end alone, from three sources. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_MAIN;
	temporary(&group->source[0], data);
	temporary(&group->source[3], address);
	temporary(&group->source[4], mask);

	/* One 32-bit word on data return counter 0. */
	error = powervr_usc_store_tiled(3, 1, 0, USC_SIZE_32, 0, 0, 4, &group->operation[POWERVR_USC_PHASE_BACKEND]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Builds a main group that saves a mask (POWERVR_USC_MASK_*) into a
 * temporary.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_map.py (group_map O_SAVMSK: the
 * back end alone, the mask written to w0).
 */
static int
save_mask_group(
	struct powervr_usc_group *group,
	unsigned mode,
	unsigned destination)
{
	int error;

	/* The back end alone, one destination. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_MAIN;
	group->write_0 = 1;
	temporary(&group->destination[0], destination);

	/* The save of the mask. */
	error = powervr_usc_save_mask(mode, &group->operation[POWERVR_USC_PHASE_BACKEND]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Builds a control group of an operation without operands (a wait, or
 * the no-operation that ends a program when end is 1).
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_map.py (group_map O_WOP, O_WDF
 * and O_NOP: a control group naming the operation, the end in its
 * miscellaneous bit); zedBSD src/drivers/gpu/powervr/compiler/
 * shader-backend.c (MACHINE_WAIT and MACHINE_END).
 */
static int
control_group(
	struct powervr_usc_group *group,
	unsigned control,
	unsigned end)
{
	int error;

	/* The control unit names the operation. */
	*group = blank_group;
	group->unit = POWERVR_USC_UNIT_CONTROL;
	group->control = (uint8_t)control;
	group->control_misc = (uint8_t)end;

	/* A no-operation carries one byte, a wait none. */
	if (control == POWERVR_USC_CONTROL_NOTHING)
		error = powervr_usc_nothing(&group->operation[POWERVR_USC_PHASE_0]);
	else
		error = powervr_usc_empty(&group->operation[POWERVR_USC_PHASE_0]);
	if (error != 0)
		return error;

	/* Succeeded: the group. */
	return 0;
}

/*
 * Builds the groups at *at that load a tile's words of a tile buffer at
 * address into the pixel output registers, registers a pixel, from the
 * temporaries temp up (six): the address's halves, the tiled offset,
 * the burst's length, the address pair (at an even temporary); then the
 * wait for the load.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_usc.c (pvr_usc_eot) and
 * src/imagination/pco/pco_trans_nir.c (trans_flush_tile_buffer), as
 * powervr_render_eot_usc_tiles cites them; pco_map.py (group_map
 * O_LD_REGBL: the address in s0, the burst in s1, the destination in
 * s3; O_MOVI32 into an index register).
 */
static int
tile_load_groups(
	struct powervr_usc_group *groups,
	uint64_t address,
	uint32_t registers,
	unsigned temp,
	size_t *at)
{
	uint32_t load_words;
	unsigned pair;
	size_t next;
	int error;

	/* The tile's words, in one load of at most 1024 (sent as 0). */
	load_words = TILE_PIXELS * registers;
	if (load_words > TILE_LOAD_WORDS_MAX)
		return ENOTSUP;
	pair = (temp + 4U + 1U) & ~1U;

	/* The address's halves and the pixel's tiled offset of word 0. */
	next = *at;
	error = immediate_group(&groups[next], temp, (uint32_t)address);
	if (error != 0)
		return error;
	next++;
	error = immediate_group(&groups[next], temp + 1U, (uint32_t)(address >> 32));
	if (error != 0)
		return error;
	next++;
	error = special_move_group(&groups[next], USC_TILED_LOAD, temp + 2U, 0);
	if (error != 0)
		return error;
	next++;

	/* The address of this shading cluster's part of the tile. */
	error = add64_group(&groups[next], POWERVR_USC_BANK_TEMPORARY, temp, temp + 1U, temp + 2U, pair);
	if (error != 0)
		return error;
	next++;

	/* Index register 0 at pixel output 0, the burst's length. */
	error = immediate_group(&groups[next], 0, 0);
	if (error != 0)
		return error;
	groups[next].destination[0].bank = POWERVR_USC_BANK_INDEXED_0;
	groups[next].destination[0].index = USC_INDEX_0_ITSELF;
	next++;
	error = immediate_group(&groups[next], temp + 3U, load_words % TILE_LOAD_WORDS_MAX);
	if (error != 0)
		return error;
	next++;

	/* The load into the pixel output registers from index register 0. */
	groups[next] = blank_group;
	groups[next].unit = POWERVR_USC_UNIT_MAIN;
	temporary(&groups[next].source[0], pair);
	temporary(&groups[next].source[1], temp + 3U);
	groups[next].source[3].used = 1;
	groups[next].source[3].bank = POWERVR_USC_BANK_INDEXED_0;
	groups[next].source[3].index = USC_INDEXED_PIXEL_OUTPUT;
	error = powervr_usc_load(0, 0, 1, 0, 0, &groups[next].operation[POWERVR_USC_PHASE_BACKEND]);
	if (error != 0)
		return error;
	next++;

	/* The wait for its words. */
	error = control_group(&groups[next], POWERVR_USC_CONTROL_WAIT_DATA, 0);
	if (error != 0)
		return error;
	next++;

	/* Succeeded: the groups. */
	*at = next;
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
 * format, as the compiler leaves the format's pixels in the pixel output
 * registers: a format of 32-bit channels, or an integer one, from its
 * words as they are (8_PER_CHANNEL); a float format (16-bit channels, the
 * 11- and 10-bit packed) from half floats; sRGB from half floats with
 * gamma; a normalised format of channels wider than 8 bits (16-bit, the
 * 10-bit packed) from half floats; an 8-bit unorm or snorm channel from
 * the packed 8 bits.  Refuses depth formats (ENOTSUP).
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_job_common.c
 * (pvr_arch_pbe_get_src_format_and_gamma: in that order, a 32-bit
 * component or an integer format 8_PER_CHANNEL, a float format
 * F16_PER_CHANNEL, sRGB F16_PER_CHANNEL with gamma, a first channel
 * wider than 8 bits F16_PER_CHANNEL, else 8_PER_CHANNEL with the packed
 * channel) and pvr_arch_cmd_buffer.c (pvr_setup_pbe_state: the packed
 * channel for unorm and snorm formats with usc_f16sop_u8, which
 * src/imagination/common/device_info/bxe-4-32.h has); on the board a
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

	/* No gamma but for sRGB. */
	*gamma = 0;

	/* An integer format and the 32-bit float channels: the words as they are. */
	if ((format->kind & POWERVR_FORMAT_INTEGER) != 0 || format->vk_format == VK_FORMAT_R32_SFLOAT ||
	    format->vk_format == VK_FORMAT_R32G32_SFLOAT || format->vk_format == VK_FORMAT_R32G32B32A32_SFLOAT) {
		*source_format = PBE_SOURCE_8;
		return 0;
	}

	/* A float format: its half floats. */
	if ((format->kind & POWERVR_FORMAT_FLOAT) != 0) {
		*source_format = PBE_SOURCE_F16;
		return 0;
	}

	/* sRGB: half floats with gamma. */
	if ((format->kind & POWERVR_FORMAT_SRGB) != 0) {
		*source_format = PBE_SOURCE_F16;
		*gamma = 1;
		return 0;
	}

	/* A normalised format of channels wider than 8 bits: half floats. */
	switch (format->vk_format) {
	case VK_FORMAT_R16_UNORM:
	case VK_FORMAT_R16_SNORM:
	case VK_FORMAT_R16G16_UNORM:
	case VK_FORMAT_R16G16_SNORM:
	case VK_FORMAT_R16G16B16A16_UNORM:
	case VK_FORMAT_R16G16B16A16_SNORM:
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		*source_format = PBE_SOURCE_F16;
		return 0;
	default:
		break;
	}

	/* Refuses a texel of other than one to four bytes. */
	if (format->texel_bytes == 0 || format->texel_bytes > 4)
		return ENOTSUP;

	/* Succeeded: 8-bit normalised channels, packed. */
	*source_format = PBE_SOURCE_8;
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

/*
 * Sets the downscale of a back end's first register word: the pixel back
 * end averages the samples of each pixel of a render of four samples and
 * stores one texel a pixel, a resolve to an attachment of one sample.
 * The format must be one the back end filters (not an integer one).
 *
 * Values referenced: Mesa 26.2.6 src/imagination/csbgen/rogue/pbestate.xml
 * (REG_WORD0 downscale 56); src/imagination/vulkan/pvr_arch_job_common.c
 * (pvr_arch_pbe_pack_state: downscale from the surface's down_scale) and
 * pvr_arch_cmd_buffer.c (pvr_setup_pbe_state: down_scale for a resolve,
 * which has more than one sample).
 */
void
powervr_render_pbe_resolve(
	uint64_t reg[3])
{
	/* The downscale bit. */
	reg[0] |= 1ULL << 56;
}

/*
 * Builds a draw's fragment task program for a render of samples samples a
 * pixel: the instance rate's, and with four samples the task word's sample
 * rate the full one (the rate field, bits 33 and 34, is the instance
 * rate's 0 before).
 *
 * Values referenced: Mesa 26.2.6 src/imagination/csbgen/rogue/pds.xml
 * (DOUTU_SRC0 sample_rate 33..34, DOUTU_SAMPLE_RATE FULL 2).
 * Procedure referenced: Mesa 26.2.6 src/imagination/vulkan/
 * pvr_arch_cmd_buffer.c (the fragment program's DOUTU sample rate FULL
 * when the rasterization samples are more than one).
 */
int
powervr_render_fragment_kick_samples(
	uint32_t usc_offset,
	unsigned temps,
	uint32_t samples,
	struct powervr_render_kick *kick)
{
	int error;

	/* The instance rate's program. */
	error = powervr_render_fragment_kick_build(usc_offset, temps, kick);
	if (error != 0)
		return error;

	/* Four samples: each sample's fragment run on its own. */
	if (samples == 4U)
		kick->data[1] |= SAMPLE_RATE_FULL << (SAMPLE_RATE_SHIFT - 32U);

	/* Succeeded: the task program. */
	return 0;
}
