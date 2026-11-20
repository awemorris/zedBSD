/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The words and fixed programs of a PowerVR transfer job (copy and
 * clear).  See transfer-program.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libc/vulkan/vulkan_core.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/data-program.h"
#include "drivers/gpu/powervr/image-state.h"
#include "drivers/gpu/powervr/render-program.h"
#include "drivers/gpu/powervr/compiler/shader-private.h"
#include "drivers/gpu/powervr/transfer-program.h"
#include "drivers/gpu/powervr/compiler/usc-encode.h"

/*
 * The copy program's registers: the shared registers of the image state
 * (0 to 3), the sampler state (4 to 7), the coordinate floats (8 to
 * 11, x scale and offset, y scale and offset) and the coordinates'
 * limits (12 to 15, x least and most, y least and most); the
 * temporaries of the pixel's position (0, 1), the source coordinates
 * (2, 3), the sampled words (4 to 7) and the coordinate floats, then
 * the limits (8 to 11; a multiply-add takes no two shared registers, so
 * they are moved first).  The coordinates are clamped to the centres of
 * the source rectangle's edge texels: a transfer shades whole tiles, and
 * a pixel outside the destination's rectangle must not read past a
 * linear source's memory (unclamped, a read went 4032 bytes past a
 * 1920x1080 image).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_usc.c (pvr_uscgen_tq with
 * layer_floats PVR_INT_COORD_SET_FLOATS_4 and iterated false: the
 * position's x and y, coords = position * (sh[0], sh[2]) + (sh[1],
 * sh[3]) from the dynamic constants, the sample from the combined image
 * sampler's image and sampler state, the raw words to the output).
 */
#define SHARED_IMAGE		0U
#define SHARED_SAMPLER		4U
#define SHARED_FLOATS		8U
#define SHARED_LIMITS		12U
#define TEMP_POSITION		0U
#define TEMP_COORDINATES	2U
#define TEMP_SAMPLE		4U
#define TEMP_FLOATS		8U
#define TEMP_LIMITS		8U
#define TEST_GREATER		5U
#define TEST_LESS		8U
#define TYPE_F32		0U
#define TEMP_PIXEL		12U
#define COPY_TEMPS		12U
#define MAPPED_INDEX		8U
#define SAMPLE_INDEX		16U

/*
 * The copy program of a volume: the slice's coordinate (an immediate)
 * after the two others in temporary 4, the sample in temporaries 12 to 15
 * (where the two-dimensional program samples into 4 to 7), its sample one
 * operation later.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_usc.c (pvr_uscgen_tq_frag_coords:
 * for a 3D source the depth is a third coordinate after x and y;
 * pvr_uscgen_tq_frag_load: the sample of GLSL_SAMPLER_DIM_3D) and
 * src/imagination/vulkan/rogue/pvr_blit.c (pvr_setup_transfer_surface:
 * a 3D-twiddled image's slice as the z position, other images' layers by
 * their address).
 */
#define TEMP_SLICE		4U
#define TEMP_SAMPLE_VOLUME	12U
#define VOLUME_TEMPS		16U

/*
 * The copy program's sources (build_program): a flat image, a volume's
 * slice, or a flat image of four samples a pixel that a program run for
 * each sample reads sample by sample.  The last puts the sample's number
 * in bits 16 to 18 of the lookup word after the coordinates (temporary
 * 4, the number and its scale through 5 and 6) and samples into 12 to 15
 * as the volume does; the number comes from samp_num, the multiply-add's
 * addend the special register of zero.
 *
 * Values referenced: Mesa 26.2.6 src/imagination/pco/pco_isa.py (special
 * registers samp_num 53, the constant 0 in 0) and pco_nir_tex.c (the
 * lookup word with sno, the number at ROGUE_SMP_LOOKUP_MS_NUM_START 16).
 */
#define SOURCE_FLAT		0U
#define SOURCE_VOLUME		1U
#define SOURCE_SAMPLES		2U
#define TEMP_SAMPLE_LOOKUP	4U
#define TEMP_SAMPLE_NUMBER	5U
#define TEMP_SAMPLE_SCALE	6U
#define SPECIAL_SAMPLE_NUMBER	53U
#define SPECIAL_ZERO		0U
#define COPY_OPERATIONS		28U

/*
 * The pack of four 8-bit unorm channels with its scale (the blit).
 *
 * Values referenced:
 * the compiler back end's pack (shader-backend.c PACK_U8888 0,
 * PACK_SCALE 1), from Mesa 26.2.5 src/imagination/pco/pco_isa.py.
 */
#define PACK_U8888		0U
#define PACK_SCALE		1U

/*
 * The blit's packs (build_program): none (a copy's raw words), four
 * 8-bit unorm channels into one word, or the four channels as half
 * floats, two a word, into two (an sRGB destination, which the pixel
 * back end converts with its gamma).
 *
 * Values referenced: the compiler back end's pack (shader-backend.c
 * PACK_F16F16 14, two channels a word), from Mesa 26.2.5
 * src/imagination/pco/pco_isa.py.
 */
#define PACK_NONE		0U
#define PACK_UNORM8		1U
#define PACK_HALVES		2U
#define PACK_F16F16		14U

/*
 * The special registers of the pixel's position at the pixel rate, and
 * the first pixel output register.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py (special registers x_p 97,
 * y_p 100, pixout 32); src/imagination/pco/pco_trans_nir.c
 * (trans_load_input: VARYING_SLOT_POS x and y from x_p and y_p when
 * single sampled).
 */
#define SPECIAL_X		97U
#define SPECIAL_Y		100U
#define SPECIAL_PIXEL_OUTPUT	32U

/*
 * The sample's fields: a 2D lookup of four channels.  The coordinates are
 * unnormalised by the sampler state (its non_normalized_coords); the
 * sample's own fcnorm flag (the first argument of powervr_usc_sample)
 * says whether fixed-point channels come back converted to floats: off
 * for a copy's raw words, on for a blit, whose floats are packed again
 * (with it off, a blit's 8-bit unorm channels come back as their
 * integers, tiny floats the pack writes as 0).
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py (F_DIM 2d 0b10, the
 * backend's fcnorm bit); src/imagination/pco/pco_nir_tex.c (fcnorm set
 * when the sample's destination type is float).
 */
#define SAMPLE_2D		2U
#define SAMPLE_3D		3U
#define SAMPLE_CHANNELS		4U

/*
 * The raw formats a same-format copy moves texels as.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_transfer.c
 * (pvr_3d_copy_blit: a point-sampled copy between one format moves
 * 32-bit texels as VK_FORMAT_R32_UINT); src/imagination/vulkan/
 * pvr_arch_formats.c (R32_UINT: texture format U32 24, pixel back end
 * pack mode U32 0x34); src/imagination/vulkan/pvr_formats.c (the
 * R32_UINT swizzle X001).
 */
static const struct powervr_format raw_32 = {
	VK_FORMAT_R32_UINT, 4, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 24, 0x34,
	{ POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	0, 0, 0
};

/*
 * The raw formats of 8- and 16-bit texels: one channel of the texel's
 * size, R8_UINT (texture format U8 0, pack mode U8 0x2f) and R16_UINT
 * (U16 9, 0x31), its integer the low bits of the one word the sample
 * returns and the pixel back end stores.  Mesa moves a 16-bit texel as
 * R8G8_UINT; one 16-bit channel keeps the texel in the one word without
 * relying on how the unit returns two integer channels.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/vulkan/rogue/pvr_blit.c
 * (pvr_get_raw_copy_format: an 8-bit texel as R8_UINT);
 * src/imagination/vulkan/pvr_arch_formats.c (R8_UINT: U8 and U8;
 * R16_UINT: U16 and U16); src/imagination/vulkan/pvr_arch_job_transfer.c
 * (pvr_pbe_src_format_raw: a texel of up to 32 bits is RAW32, one
 * output register); src/imagination/csbgen/rogue/texstate.xml (FORMAT
 * U8 0, U16 9) and pbestate.xml (PACKMODE U8 0x2f, U16 0x31).
 */
static const struct powervr_format raw_8 = {
	VK_FORMAT_R8_UINT, 1, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 0, 0x2f,
	{ POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	0, 0, 0
};

static const struct powervr_format raw_16 = {
	VK_FORMAT_R16_UINT, 2, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 9, 0x31,
	{ POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	0, 0, 0
};

/*
 * The raw formats of 64- and 128-bit texels: R32G32_UINT (texture
 * format U32U32 35, pack mode U32U32 0x28, swizzle XY01) and
 * R32G32B32A32_UINT (U32U32U32U32 62, 0x0b, XYZW).
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_formats.c (R32G32_UINT
 * and R32G32B32A32_UINT: their texture formats and pack modes);
 * src/imagination/csbgen/rogue/texstate.xml (FORMAT U32U32 35,
 * U32U32U32U32 62); src/imagination/csbgen/rogue/pbestate.xml (PACKMODE
 * U32U32 0x28, U32U32U32U32 0xb); src/imagination/vulkan/
 * pvr_arch_job_transfer.c (pvr_get_raw_copy_format: a 64-bit texel as
 * R32G32_UINT, a 128-bit one as R32G32B32A32_UINT).
 */
static const struct powervr_format raw_64 = {
	VK_FORMAT_R32G32_UINT, 8, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 35, 0x28,
	{ POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	0, 0, 0
};

static const struct powervr_format raw_128 = {
	VK_FORMAT_R32G32B32A32_UINT, 16, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 62, 0x0b,
	{ POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	0, 0, 0
};

/*
 * The fields of the transfer command's registers.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/cr.xml (ISP_RENDER mode_type
 * 0..1 FAST_SCALE 3, dir_type 2..3 TL2BR 0; ISP_RENDER_ORIGIN and
 * ISP_MTILE_SIZE x 16..25, y 0..9; ISP_BGOBJVALS enablebgtag 9; ISP_CTL
 * process_empty_tiles 17, pipe_enable 12..15; USC_PIXEL_OUTPUT_CTRL
 * width 0..1 2REGISTERS 0, partition_mask 3..20; FRAG_SCREEN xmax 0..14,
 * ymax 16..30).
 */
#define RENDER_FAST_SCALE	3U
#define BGOBJ_ENABLE_TAG	(1U << 9)
#define ISP_PROCESS_EMPTY	(1U << 17)
#define ISP_PIPE_SHIFT		12U
#define PIXEL_PARTITION_SHIFT	3U
#define PIXEL_WIDTH_4_REGISTERS	1U
#define TILE_FIELD_MAX		0x3ffU
#define ISP_AA_4X		2U
#define SCREEN_FIELD_MAX	0x7fffU

/*
 * The tile and partition figures of the BXE-4-32.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/common/device_info/bxe-4-32.h
 * (tile_size_x 16, tile_size_y 16, max_partitions 10,
 * usc_min_output_registers_per_pix 2, isp_max_tiles_in_flight 4,
 * num_raster_pipes 1, usc_pixel_partition_mask,
 * simple_parameter_format_version 2);
 * src/imagination/vulkan/winsys/pvrsrvkm/pvr_srv.c
 * (pvr_srv_get_total_reserved_partition_size: 16 x 16 tiles x 10
 * partitions x 2 registers).
 */
#define TILE_SIZE		16U
#define MAX_PARTITIONS		10U
#define MIN_OUTPUT_REGISTERS	2U
#define ISP_TILES_IN_FLIGHT	4U
#define RESERVED_PARTITIONS	(TILE_SIZE * TILE_SIZE * MAX_PARTITIONS * MIN_OUTPUT_REGISTERS)

/*
 * The sampler word's detail adjust and w wrap fields.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/texstate.xml (SAMPLER_WORD0
 * dadjust 0..12, addrmode_w 56..58); src/imagination/vulkan/
 * pvr_arch_job_transfer.c (pvr_sampler_state_for_surface: dadjust
 * MIN_UINT 0, addrmode_w only for a 3D source).
 */
#define SAMPLER_DADJUST_MASK	0x1fffULL
#define SAMPLER_WRAP_W_MASK	(7ULL << 56)

/*
 * The merging copy's registers (build_merge_program): the destination's
 * image state in shared registers 16 to 19 after the copy's sixteen; the
 * source's sample in temporaries 4 to 7 as the copy's, the destination's
 * in 12 to 15 (looked up at the pixel's position in temporaries 0 and 1),
 * the source's bits mask in 16, the destination's in 17, the shift in
 * 18; the bitwise unit's AND and OR and its left shift.
 *
 * The extracting copy (build_extract_program) shifts the source's sample
 * right by the shift in temporary 8, the limits' first, free after the
 * clamp.
 *
 * Values referenced: the compiler back end's logic and shift variants
 * (shader-backend.c LOGIC_OR 0, LOGIC_AND 1, SHIFT_LEFT 0, SHIFT_RIGHT
 * 1), from Mesa 26.2.6 src/imagination/pco/pco_isa.py.
 */
#define SHARED_MERGE_IMAGE	16U
#define TEMP_MERGE_SAMPLE	12U
#define TEMP_MERGE_MASK		16U
#define TEMP_MERGE_KEEP		17U
#define TEMP_MERGE_SHIFT	18U
#define MERGE_TEMPS		19U
#define MERGE_OPERATIONS	32U
#define MERGE_OR		0U
#define MERGE_AND		1U
#define MERGE_SHIFT_LEFT	0U
#define EXTRACT_SHIFT_RIGHT	1U
#define TEMP_EXTRACT_SHIFT	8U


static const struct shader_machine_register no_register = { 0xff, 0 };

static void blank_operation(struct shader_machine_operation *operation, unsigned kind);
static struct shader_machine_register machine_register(unsigned bank, unsigned index);
static int build_program(unsigned packed, unsigned words, unsigned source, uint32_t slice_bits, struct powervr_render_usc *program);
static void mapped_coordinates(struct shader_machine_operation operations[MAPPED_INDEX]);
static void copy_coordinates(struct shader_machine_operation operations[SAMPLE_INDEX]);
static int build_merge_program(uint32_t shift, uint32_t mask, struct powervr_render_usc *program);
static int build_extract_program(uint32_t shift, struct powervr_render_usc *program);
static bool resolve_pack_find(const struct powervr_format *format, unsigned *pack, unsigned *words);
static int build_resolve_program(unsigned pack, unsigned words, unsigned samples, struct powervr_render_usc *program);
static int fill_shared(const struct powervr_view_shape *view, const struct powervr_render_area *source, const struct powervr_render_area *area, bool flip_x, bool flip_y, bool linear, uint32_t words[POWERVR_TRANSFER_SHARED_WORDS]);
static uint32_t float_from_ratio(int64_t numerator, int64_t denominator);
static int build_groups(const struct shader_machine_operation *operations, size_t count, struct powervr_usc_group *groups);
static void tiles_in_flight(uint32_t registers, uint32_t *isp_ctl, uint32_t *pixel_output_ctrl);

/* Returns the raw format a same-format copy of texel_bytes texels moves them as, NULL when there is none. */
const struct powervr_format *
powervr_transfer_raw_format(
	uint32_t texel_bytes)
{
	/* One or two bytes, or one, two or four 32-bit words a texel. */
	if (texel_bytes == 1)
		return &raw_8;
	if (texel_bytes == 2)
		return &raw_16;
	if (texel_bytes == 4)
		return &raw_32;
	if (texel_bytes == 8)
		return &raw_64;
	if (texel_bytes == 16)
		return &raw_128;

	/* No raw format of that size. */
	return NULL;
}

/*
 * Says whether a linear surface's rectangle must be written one row a
 * job: the pixel back end's line stride counts pairs of pixels, so an
 * odd row in pixels cannot be named when the rectangle has more than one
 * row.
 */
bool
powervr_transfer_needs_rows(
	const struct powervr_render_surface *surface,
	const struct powervr_render_area *area)
{
	uint32_t stride;

	/* Twiddled surfaces and single rows never need it. */
	if (!surface->linear || area->height <= 1U || surface->format == NULL || surface->format->texel_bytes == 0)
		return false;

	/* An odd row in pixels does. */
	stride = surface->row_pitch / surface->format->texel_bytes;
	if ((stride & 1U) != 0)
		return true;

	/* An even row does not. */
	return false;
}

/*
 * Builds the copy program of a format's texels: the pixel's position
 * from the special registers, scaled and offset by the shared floats into
 * the source's unnormalised coordinates, the source texel sampled with
 * the shared image and sampler state (as the raw format of its size), its
 * one, two or four raw words into pixel output registers 0 to 3 under the
 * overlap check, ending the program.  Refuses a texel of no raw format
 * (ENOTSUP).
 *
 * The groups are the compiler back end's (shader_backend_group); the
 * sample is rebuilt with unnormalised coordinates.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_usc.c (pvr_uscgen_tq,
 * pvr_uscgen_tq_frag_coords, pvr_uscgen_tq_frag_load with nncoords and
 * the uint32 type of a raw format, the store of each word to its output).
 */
int
powervr_transfer_copy_usc(
	const struct powervr_format *format,
	struct powervr_render_usc *program)
{
	const struct powervr_format *raw;
	int error;

	/* Refuses missing input or result, and texels of no raw format. */
	if (format == NULL || program == NULL)
		return EINVAL;
	raw = powervr_transfer_raw_format(format->texel_bytes);
	if (raw == NULL)
		return ENOTSUP;

	/* The raw words (one for a texel of up to 32 bits), not packed. */
	error = build_program(PACK_NONE, (raw->texel_bytes + 3U) / 4U, SOURCE_FLAT, 0, program);
	if (error != 0)
		return error;

	/* Succeeded: the copy program. */
	return 0;
}

/*
 * Builds the copy program of a slice of a volume's texels: as
 * powervr_transfer_copy_usc, with the slice's centre (slice + 0.5, an
 * unnormalised coordinate) as the third coordinate of a
 * three-dimensional sample.  Refuses a texel of no raw format (ENOTSUP)
 * and a slice past the texture unit's depth (EINVAL).
 */
int
powervr_transfer_copy_usc_volume(
	const struct powervr_format *format,
	uint32_t slice,
	struct powervr_render_usc *program)
{
	const struct powervr_format *raw;
	int error;

	/* Refuses missing input or result, texels of no raw format and a slice past the depth. */
	if (format == NULL || program == NULL || slice >= POWERVR_IMAGE_LAYERS_MAX)
		return EINVAL;
	raw = powervr_transfer_raw_format(format->texel_bytes);
	if (raw == NULL)
		return ENOTSUP;

	/* The raw words of the slice, not packed. */
	error = build_program(PACK_NONE, (raw->texel_bytes + 3U) / 4U, SOURCE_VOLUME, float_from_ratio(2 * (int64_t)slice + 1, 2), program);
	if (error != 0)
		return error;

	/* Succeeded: the copy program. */
	return 0;
}

/*
 * Builds the copy program between two images of four samples a pixel,
 * run for each sample (powervr_render_fragment_kick_samples): as
 * powervr_transfer_copy_usc, the source's texel looked up by the number
 * of the sample the program runs for, so each sample of the rectangle is
 * written from the same sample of the source's.  Refuses texels of no
 * raw format (ENOTSUP).
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_job_transfer.c
 * (pvr_msaa_state: a multisampled source and destination, M -> M, the
 * shader at the full rate, the layer's sample count on the texture unit)
 * and pvr_usc.c (pvr_uscgen_tq_frag_load: at the full rate the lookup by
 * the sample id).
 */
int
powervr_transfer_copy_usc_samples(
	const struct powervr_format *format,
	struct powervr_render_usc *program)
{
	const struct powervr_format *raw;
	int error;

	/* Refuses missing input or result, and texels of no raw format. */
	if (format == NULL || program == NULL)
		return EINVAL;
	raw = powervr_transfer_raw_format(format->texel_bytes);
	if (raw == NULL)
		return ENOTSUP;

	/* The raw words of the run's sample, not packed. */
	error = build_program(PACK_NONE, (raw->texel_bytes + 3U) / 4U, SOURCE_SAMPLES, 0, program);
	if (error != 0)
		return error;

	/* Succeeded: the copy program. */
	return 0;
}

/*
 * Builds the blit program of a destination format: as the copy program,
 * but the sampled channels (filtered as the sampler says, as floats) are
 * packed: into one word for a four-channel 8-bit unorm destination, as
 * half floats into two for an sRGB one (the pixel back end's F16 source
 * with gamma, as a render pass stores an sRGB attachment).  Other
 * formats are not taken (ENOTSUP).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_transfer.c
 * (pvr_pbe_src_format_normal: an R8G8B8A8 or B8G8R8A8 unorm destination
 * takes PVR_TRANSFER_PBE_PIXEL_SRC_F16_U8); src/imagination/vulkan/
 * pvr_usc.c (pvr_uscgen_tq_frag_pack: F16_U8 is nir_pack_unorm_4x8);
 * Mesa 26.2.6 src/imagination/vulkan/pvr_usc.c (F16F16 is
 * nir_pack_half_2x16 of channels 0 and 1, then 2 and 3) and
 * pvr_arch_job_common.c (pvr_arch_pbe_get_src_format_and_gamma: an sRGB
 * format F16_PER_CHANNEL with gamma).
 */
int
powervr_transfer_blit_usc(
	const struct powervr_format *format,
	struct powervr_render_usc *program)
{
	int error;

	/* Refuses missing input or result, and formats other than four 8-bit unorm channels. */
	if (format == NULL || program == NULL)
		return EINVAL;
	if (powervr_format_is_unorm8x4(format)) {
		/* The channels packed into one word. */
		error = build_program(PACK_UNORM8, 1, SOURCE_FLAT, 0, program);
	} else if ((format->kind & POWERVR_FORMAT_SRGB) != 0 && format->texel_bytes == 4) {
		/* The channels as half floats, two words. */
		error = build_program(PACK_HALVES, 2, SOURCE_FLAT, 0, program);
	} else {
		return ENOTSUP;
	}
	if (error != 0)
		return error;

	/* Succeeded: the blit program. */
	return 0;
}

/*
 * The resolve's program (build_resolve_program): the lookup word of each
 * sample's number after the coordinates in temporaries 2 and 3, the four
 * samples in temporaries 12 to 27, a quarter in temporary 28, the packed
 * pixel in 29 and 30; the samples' numbers in bits 16 to 18 of the
 * lookup word, which the sample's extension marks.  The packs of four
 * 8-bit signed normalised channels and of two 16-bit ones, unsigned or
 * signed, scale as the 8-bit unsigned one does; RESOLVE_WORDS is no pack
 * (the 32-bit float channels' words, or sample 0's raw words, to the
 * pixel output as they are).  A resolve of sample 0 alone needs the
 * temporaries up to its sample's.
 *
 * Values referenced: Mesa 26.2.6 src/imagination/pco/pco_nir_tex.c (the
 * lookup word after the coordinates with sno set, the number at
 * ROGUE_SMP_LOOKUP_MS_NUM_START 16),
 * src/imagination/include/hwdef/rogue_hw_defs.h and
 * src/imagination/pco/pco_isa.py (F_PCK_FORMAT s8888 0b00001, u1616
 * 0b00011, s1616 0b00100).
 */
#define TEMP_LOOKUP		4U
#define TEMP_SAMPLES		12U
#define TEMP_QUARTER		28U
#define TEMP_RESOLVED		29U
#define RESOLVE_TEMPS		31U
#define SAMPLE_ZERO_TEMPS	16U
#define RESOLVE_SAMPLES		4U
#define SAMPLE_NUMBER_SHIFT	16U
#define FLOAT_QUARTER		0x3e800000U
#define RESOLVE_OPERATIONS	64U
#define PACK_S8888		1U
#define PACK_U1616		3U
#define PACK_S1616		4U
#define RESOLVE_WORDS		0xffU

/*
 * How the resolve's program leaves the average of a colour format's
 * samples in the pixel output for the pixel back end -- as a render
 * pass's fragment program leaves the format's colour, so the destination
 * is stored by the format's own pack mode: the pack of its four channels
 * (RESOLVE_WORDS: its 32-bit float channels a word each) and the words.
 * The formats of four 8-bit unorm channels and the 32-bit sRGB ones are
 * found by their kind (resolve_pack_find), the others here.
 *
 * Values referenced:
 * Mesa 26.2.6 src/imagination/pco/pco_nir_pvfio.c (to_pbe_format and
 * pack_to_format: the 32-bit channels a register each; the 8-bit
 * normalised channels packed four to a register by u8888 or s8888, the
 * 16-bit ones two by u1616 or s1616; the 16-bit float channels,
 * A2B10G10R10_UNORM and B10G11R11_UFLOAT as half floats two to a
 * register by f16f16) and src/imagination/vulkan/pvr_arch_formats.c
 * (each format's pbe accumulation format and so its registers).
 */
struct resolve_pack {
	uint32_t vk_format;
	uint8_t pack;
	uint8_t words;
};

static const struct resolve_pack resolve_packs[] = {
	{ VK_FORMAT_R8_UNORM, PACK_U8888, 1 },
	{ VK_FORMAT_R8_SNORM, PACK_S8888, 1 },
	{ VK_FORMAT_R8G8_UNORM, PACK_U8888, 1 },
	{ VK_FORMAT_R8G8_SNORM, PACK_S8888, 1 },
	{ VK_FORMAT_R8G8B8A8_SNORM, PACK_S8888, 1 },
	{ VK_FORMAT_A2B10G10R10_UNORM_PACK32, PACK_F16F16, 2 },
	{ VK_FORMAT_R16_UNORM, PACK_U1616, 1 },
	{ VK_FORMAT_R16_SNORM, PACK_S1616, 1 },
	{ VK_FORMAT_R16_SFLOAT, PACK_F16F16, 1 },
	{ VK_FORMAT_R16G16_UNORM, PACK_U1616, 1 },
	{ VK_FORMAT_R16G16_SNORM, PACK_S1616, 1 },
	{ VK_FORMAT_R16G16_SFLOAT, PACK_F16F16, 1 },
	{ VK_FORMAT_R16G16B16A16_UNORM, PACK_U1616, 2 },
	{ VK_FORMAT_R16G16B16A16_SNORM, PACK_S1616, 2 },
	{ VK_FORMAT_R16G16B16A16_SFLOAT, PACK_F16F16, 2 },
	{ VK_FORMAT_R32_SFLOAT, RESOLVE_WORDS, 1 },
	{ VK_FORMAT_R32G32_SFLOAT, RESOLVE_WORDS, 2 },
	{ VK_FORMAT_R32G32B32A32_SFLOAT, RESOLVE_WORDS, 4 },
	{ VK_FORMAT_B10G11R11_UFLOAT_PACK32, PACK_F16F16, 2 },
};

/*
 * Says how many samples of each pixel the resolve's program reads of a
 * format: four, averaged, for a float or normalised colour format the
 * program packs (resolve_pack_find); one, sample 0, for an integer colour
 * format or a depth format (a render pass's resolve of sample 0, its
 * stencil with it) of a raw format's size, moved as raw texels; 0 for a
 * format the transfer does not resolve.
 *
 * Procedure referenced: Mesa 26.2.6 src/imagination/vulkan/rogue/
 * pvr_blit.c (pvr_copy_or_resolve_image_region: PVR_RESOLVE_BLEND, but
 * PVR_RESOLVE_SAMPLE0 for an integer format, whose blend is not
 * defined) and pvr_arch_cmd_buffer.c
 * (pvr_resolve_unemitted_resolve_attachments: a subpass's depth and
 * stencil resolved by the transfer, PVR_RESOLVE_SAMPLE0 for
 * VK_RESOLVE_MODE_SAMPLE_ZERO_BIT); the Vulkan 1.3 specification,
 * vkCmdResolveImage (an integer format's pixel takes a single sample's
 * value).
 */
unsigned
powervr_transfer_resolve_samples(
	const struct powervr_format *format)
{
	const struct powervr_format *raw;
	unsigned pack;
	unsigned words;
	bool averaged;

	/* An average the program packs. */
	averaged = resolve_pack_find(format, &pack, &words);
	if (averaged)
		return RESOLVE_SAMPLES;

	/* Neither an integer colour format nor a depth format: not resolved. */
	if (format == NULL)
		return 0;
	if ((format->kind & POWERVR_FORMAT_DEPTH) == 0 &&
	    ((format->kind & POWERVR_FORMAT_COLOR) == 0 || (format->kind & POWERVR_FORMAT_INTEGER) == 0))
		return 0;

	/* Of a raw format's size: its sample 0 as raw texels. */
	raw = powervr_transfer_raw_format(format->texel_bytes);
	if (raw == NULL)
		return 0;

	/* Succeeded: sample 0. */
	return 1;
}

/* Reports whether the resolve's program averages a format's samples (powervr_transfer_resolve_samples). */
bool
powervr_transfer_resolves(
	const struct powervr_format *format)
{
	/* The averaged formats. */
	return powervr_transfer_resolve_samples(format) == RESOLVE_SAMPLES;
}

/*
 * Builds the resolve program of a format (powervr_transfer_resolve_samples):
 * as the blit's, each pixel's four samples of a multisampled source
 * looked up by their numbers as floats, added and scaled by a quarter,
 * and left as a render pass's fragment program leaves the format's
 * colour (resolve_pack_find); or, of an integer format, sample 0 alone
 * looked up as the raw words of its texel (the source read as its raw
 * format, powervr_transfer_raw_blit_words) and moved to the pixel output
 * as they are.  Other formats are not taken (ENOTSUP).
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_usc.c (pvr_uscgen_tq_frag_load:
 * a sample of each number when the layer is multisampled and the shader
 * runs at the instance rate, the number of the resolve's sample for
 * PVR_RESOLVE_SAMPLE0; resolve_samples with PVR_RESOLVE_BLEND: the
 * samples, as many channels as the format's, times one over their count
 * added) and pvr_arch_job_transfer.c (pvr_msaa_state: a multisampled
 * source and a single-sampled destination resolve, the layer's sample
 * count for a format the shader filters; Mesa lets the pixel back end's
 * downscale average the formats it filters, this program averages every
 * format as Mesa's shader does the others).
 */
int
powervr_transfer_resolve_usc(
	const struct powervr_format *format,
	struct powervr_render_usc *program)
{
	const struct powervr_format *raw;
	unsigned samples;
	unsigned pack;
	unsigned words;
	bool averaged;
	int error;

	/* Refuses missing input or result. */
	if (format == NULL || program == NULL)
		return EINVAL;

	/* Refuses a format not resolved. */
	samples = powervr_transfer_resolve_samples(format);
	if (samples == 0)
		return ENOTSUP;

	/* The average, packed; else sample 0's raw words. */
	averaged = resolve_pack_find(format, &pack, &words);
	if (averaged) {
		error = build_resolve_program(pack, words, RESOLVE_SAMPLES, program);
	} else {
		raw = powervr_transfer_raw_format(format->texel_bytes);
		error = build_resolve_program(RESOLVE_WORDS, (raw->texel_bytes + 3U) / 4U, 1, program);
	}
	if (error != 0)
		return error;

	/* Succeeded: the resolve program. */
	return 0;
}

/*
 * Finds how the resolve's program leaves a format's average: four 8-bit
 * unorm channels packed into one word, a 32-bit sRGB format's channels
 * as half floats into two (the pixel back end's gamma converts them, as
 * the blit's), else the format's entry of resolve_packs.  False for a
 * format the program does not average (integer and depth formats among
 * them).
 */
static bool
resolve_pack_find(
	const struct powervr_format *format,
	unsigned *pack,
	unsigned *words)
{
	size_t index;
	bool unorm8x4;

	/* Colour formats only, not integer. */
	if (format == NULL || (format->kind & POWERVR_FORMAT_COLOR) == 0 || (format->kind & POWERVR_FORMAT_INTEGER) != 0)
		return false;

	/* Four 8-bit unorm channels: one word. */
	unorm8x4 = powervr_format_is_unorm8x4(format);
	if (unorm8x4) {
		*pack = PACK_U8888;
		*words = 1;
		return true;
	}

	/* sRGB: half floats, two words. */
	if ((format->kind & POWERVR_FORMAT_SRGB) != 0 && format->texel_bytes == 4) {
		*pack = PACK_F16F16;
		*words = 2;
		return true;
	}

	/* The others by their entry. */
	for (index = 0; index < sizeof(resolve_packs) / sizeof(resolve_packs[0]); index++) {
		if (resolve_packs[index].vk_format == format->vk_format) {
			*pack = resolve_packs[index].pack;
			*words = resolve_packs[index].words;
			return true;
		}
	}

	/* Not averaged. */
	return false;
}

/*
 * Builds the resolve program: the mapped coordinates (the position, its
 * scale and offset from the shared floats), samples samples by number
 * (four, or sample 0 alone), four samples' average of the channels the
 * pack takes (each word's channel of a format of RESOLVE_WORDS), and
 * the pack: four 8-bit channels into one word, two 16-bit ones a word,
 * else the words as they are.
 */
static int
build_resolve_program(
	unsigned pack,
	unsigned words,
	unsigned samples,
	struct powervr_render_usc *program)
{
	struct shader_machine_operation operations[RESOLVE_OPERATIONS];
	struct powervr_usc_group groups[RESOLVE_OPERATIONS];
	size_t sample_at[RESOLVE_SAMPLES];
	size_t length;
	size_t count;
	unsigned channels;
	unsigned sample;
	unsigned channel;
	unsigned word;
	unsigned output;
	int error;

	/*
	 * The coordinates in temporaries 2 and 3.  A resolve's rectangles lie
	 * inside both images (vk-transfer.c), and the point sampler clamps to
	 * the edge what the tiles past the rectangle look up, so the limits
	 * are not applied.
	 */
	if (samples != 1U && samples != RESOLVE_SAMPLES)
		return EINVAL;
	mapped_coordinates(operations);
	count = MAPPED_INDEX;

	/* Each sample by its number after the coordinates, waited for before the lookup word changes. */
	for (sample = 0; sample < samples; sample++) {
		blank_operation(&operations[count], MACHINE_IMMEDIATE);
		operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_LOOKUP);
		operations[count].immediate = sample << SAMPLE_NUMBER_SHIFT;
		count++;
		sample_at[sample] = count;
		blank_operation(&operations[count], MACHINE_SAMPLE);
		operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLES + sample * 4U);
		operations[count].source[0] = machine_register(POWERVR_USC_BANK_SHARED, SHARED_IMAGE);
		operations[count].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_COORDINATES);
		operations[count].source[2] = machine_register(POWERVR_USC_BANK_SHARED, SHARED_SAMPLER);
		count++;
		blank_operation(&operations[count], MACHINE_WAIT);
		count++;
	}

	/* Four samples: each channel, samples 0 and 1 added, 2 and 3, the two sums, times a quarter, into sample 0's word. */
	channels = SAMPLE_CHANNELS;
	if (pack == RESOLVE_WORDS)
		channels = words;
	if (samples == RESOLVE_SAMPLES) {
		blank_operation(&operations[count], MACHINE_IMMEDIATE);
		operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_QUARTER);
		operations[count].immediate = FLOAT_QUARTER;
		count++;
		for (channel = 0; channel < channels; channel++) {
			for (sample = 0; sample < RESOLVE_SAMPLES; sample += 2U) {
				blank_operation(&operations[count], MACHINE_FLOAT_ADD);
				operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLES + sample * 4U + channel);
				operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLES + sample * 4U + channel);
				operations[count].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLES + (sample + 1U) * 4U + channel);
				count++;
			}
			blank_operation(&operations[count], MACHINE_FLOAT_ADD);
			operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLES + channel);
			operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLES + channel);
			operations[count].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLES + 8U + channel);
			count++;
			blank_operation(&operations[count], MACHINE_FLOAT_MULTIPLY);
			operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLES + channel);
			operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLES + channel);
			operations[count].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_QUARTER);
			count++;
		}
	}

	/* The pack: four 8-bit channels into one word, two 16-bit ones into each word (scaled but for half floats). */
	output = TEMP_RESOLVED;
	if (pack == PACK_U8888 || pack == PACK_S8888) {
		blank_operation(&operations[count], MACHINE_PACK);
		operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_RESOLVED);
		operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLES);
		operations[count].variant = (uint8_t)pack;
		operations[count].immediate = 4;
		operations[count].immediate_2 = PACK_SCALE;
		count++;
	} else if (pack == PACK_U1616 || pack == PACK_S1616 || pack == PACK_F16F16) {
		for (word = 0; word < words; word++) {
			blank_operation(&operations[count], MACHINE_PACK);
			operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_RESOLVED + word);
			operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLES + word * 2U);
			operations[count].variant = (uint8_t)pack;
			operations[count].immediate = 2;
			if (pack != PACK_F16F16)
				operations[count].immediate_2 = PACK_SCALE;
			count++;
		}
	} else if (pack == RESOLVE_WORDS) {
		output = TEMP_SAMPLES;
	} else {
		return EINVAL;
	}

	/* Each word to its pixel output under the overlap check. */
	for (word = 0; word < words; word++) {
		blank_operation(&operations[count], MACHINE_MOVE);
		operations[count].destination[0] = machine_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_PIXEL_OUTPUT + word);
		operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, output + word);
		operations[count].variant = 1;
		count++;
	}

	/* The groups, each sample with its number (its floats converted for an average), the last group ending. */
	error = build_groups(operations, count, groups);
	if (error != 0)
		return error;
	for (sample = 0; sample < samples; sample++) {
		error = powervr_usc_sample_extended(samples == RESOLVE_SAMPLES ? 1U : 0U, 0, SAMPLE_2D, SAMPLE_CHANNELS, 0, 0, 0, 0, 1, 0,
						    &groups[sample_at[sample]].operation[POWERVR_USC_PHASE_BACKEND]);
		if (error != 0)
			return error;
	}
	groups[count - 1U].end = 1;

	/* Encodes them. */
	error = powervr_usc_program_encode(groups, count, program->code, sizeof(program->code), &length);
	if (error != 0)
		return error;

	/* Succeeded: the program and its temporaries. */
	program->length = length;
	program->temps = RESOLVE_TEMPS;
	if (samples == 1U)
		program->temps = SAMPLE_ZERO_TEMPS;
	return 0;
}

/*
 * Builds the transfer program of powervr_transfer_copy_usc, with the
 * four sampled channels packed as 8-bit unorm into one word
 * (PACK_UNORM8) or as half floats into two (PACK_HALVES), else
 * (PACK_NONE) the first words of the sample (one, two or four), each
 * word to its pixel output.
 * A volume's program samples in three dimensions, its third coordinate
 * the float of slice_bits; a program of four samples (SOURCE_SAMPLES)
 * looks the texel up by the run's sample number.
 */
static int
build_program(
	unsigned packed,
	unsigned words,
	unsigned source,
	uint32_t slice_bits,
	struct powervr_render_usc *program)
{
	struct shader_machine_operation operations[COPY_OPERATIONS];
	struct powervr_usc_group groups[COPY_OPERATIONS];
	bool volume;
	unsigned word;
	unsigned sample_at;
	unsigned sampled;
	size_t length;
	size_t count;
	int error;

	/* The source's coordinates, clamped, in temporaries 2 and 3. */
	volume = source == SOURCE_VOLUME;
	copy_coordinates(operations);

	/* A volume's slice, the third coordinate. */
	sample_at = SAMPLE_INDEX;
	sampled = TEMP_SAMPLE;
	if (volume) {
		blank_operation(&operations[sample_at], MACHINE_IMMEDIATE);
		operations[sample_at].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SLICE);
		operations[sample_at].immediate = slice_bits;
		sample_at++;
		sampled = TEMP_SAMPLE_VOLUME;
	}

	/* A source of four samples: the lookup word, the run's sample number times 2^16. */
	if (source == SOURCE_SAMPLES) {
		blank_operation(&operations[sample_at], MACHINE_MOVE_SPECIAL);
		operations[sample_at].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE_NUMBER);
		operations[sample_at].source[0] = machine_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_SAMPLE_NUMBER);
		sample_at++;
		blank_operation(&operations[sample_at], MACHINE_IMMEDIATE);
		operations[sample_at].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE_SCALE);
		operations[sample_at].immediate = 1U << SAMPLE_NUMBER_SHIFT;
		sample_at++;
		blank_operation(&operations[sample_at], MACHINE_INTEGER);
		operations[sample_at].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE_LOOKUP);
		operations[sample_at].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE_NUMBER);
		operations[sample_at].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE_SCALE);
		operations[sample_at].source[2] = machine_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_ZERO);
		sample_at++;
		sampled = TEMP_SAMPLE_VOLUME;
	}

	/* The sample, and the wait for its data. */
	blank_operation(&operations[sample_at], MACHINE_SAMPLE);
	operations[sample_at].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, sampled);
	operations[sample_at].source[0] = machine_register(POWERVR_USC_BANK_SHARED, SHARED_IMAGE);
	operations[sample_at].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_COORDINATES);
	operations[sample_at].source[2] = machine_register(POWERVR_USC_BANK_SHARED, SHARED_SAMPLER);
	blank_operation(&operations[sample_at + 1], MACHINE_WAIT);

	/* The four channels packed into one word, or as half floats into two, for the blit. */
	count = sample_at + 2;
	if (packed == PACK_UNORM8) {
		blank_operation(&operations[count], MACHINE_PACK);
		operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_PIXEL);
		operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, sampled);
		operations[count].variant = PACK_U8888;
		operations[count].immediate = 4;
		operations[count].immediate_2 = PACK_SCALE;
		count++;
	} else if (packed == PACK_HALVES) {
		for (word = 0; word < 2; word++) {
			blank_operation(&operations[count], MACHINE_PACK);
			operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_PIXEL + word);
			operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, sampled + word * 2U);
			operations[count].variant = PACK_F16F16;
			operations[count].immediate = 2;
			count++;
		}
	}

	/* Each word to its pixel output under the overlap check: the packed one, or the sample's raw words. */
	for (word = 0; word < words; word++) {
		blank_operation(&operations[count], MACHINE_MOVE);
		operations[count].destination[0] = machine_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_PIXEL_OUTPUT + word);
		if (packed != PACK_NONE) {
			operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_PIXEL + word);
		} else {
			operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, sampled + word);
		}
		operations[count].variant = 1;
		count++;
	}

	/* The groups, the sample (its floats converted for a blit), the last one ending. */
	error = build_groups(operations, count, groups);
	if (error != 0)
		return error;
	if (source == SOURCE_SAMPLES)
		error = powervr_usc_sample_extended(packed != PACK_NONE ? 1U : 0U, 0, SAMPLE_2D, SAMPLE_CHANNELS, 0, 0, 0, 0, 1, 0, &groups[sample_at].operation[POWERVR_USC_PHASE_BACKEND]);
	else
		error = powervr_usc_sample(packed != PACK_NONE ? 1U : 0U, 0, volume ? SAMPLE_3D : SAMPLE_2D, SAMPLE_CHANNELS, 0, &groups[sample_at].operation[POWERVR_USC_PHASE_BACKEND]);
	if (error != 0)
		return error;
	groups[count - 1U].end = 1;

	/* Encodes them. */
	error = powervr_usc_program_encode(groups, count, program->code, sizeof(program->code), &length);
	if (error != 0)
		return error;

	/* Succeeded: the program and its temporaries. */
	program->length = length;
	program->temps = COPY_TEMPS;
	if (packed == PACK_UNORM8)
		program->temps = COPY_TEMPS + 1U;
	else if (packed == PACK_HALVES)
		program->temps = COPY_TEMPS + 2U;
	if (source != SOURCE_FLAT)
		program->temps = VOLUME_TEMPS;
	return 0;
}

/*
 * Fills the sixteen words the copy program's shared registers hold: the
 * source's image state (as its raw format), a point sampler that clamps
 * and takes unnormalised coordinates, and the floats that map a
 * destination pixel (x, y) to the source texel (x - area.x + source_x,
 * y - area.y + source_y): scale 1, offset source - destination.
 *
 * Refuses a source whose texels have no raw format (ENOTSUP) and
 * offsets beyond 2^24 (E2BIG).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_transfer.c
 * (pvr_image_state_set_codegen_defaults: one level, the raw format;
 * pvr_sampler_state_for_surface: point filters, clamp to edge,
 * non_normalized_coords; pvr_uv_space: FLOATS_4 when the source and
 * destination corners differ; pvr_dma_texture_floats).
 */
int
powervr_transfer_shared_words(
	const struct powervr_view_shape *source,
	uint32_t source_x,
	uint32_t source_y,
	const struct powervr_render_area *area,
	uint32_t words[POWERVR_TRANSFER_SHARED_WORDS])
{
	struct powervr_view_shape view;
	struct powervr_render_area rectangle;
	int error;

	/* Refuses missing input or result. */
	if (source == NULL || source->format == NULL || area == NULL || words == NULL)
		return EINVAL;

	/* The source as its raw format. */
	view = *source;
	view.format = powervr_transfer_raw_format(source->format->texel_bytes);
	if (view.format == NULL)
		return ENOTSUP;
	view.mapping[0] = VK_COMPONENT_SWIZZLE_IDENTITY;
	view.mapping[1] = VK_COMPONENT_SWIZZLE_IDENTITY;
	view.mapping[2] = VK_COMPONENT_SWIZZLE_IDENTITY;
	view.mapping[3] = VK_COMPONENT_SWIZZLE_IDENTITY;

	/* The source rectangle of the destination's size, point sampled. */
	rectangle.x = source_x;
	rectangle.y = source_y;
	rectangle.width = area->width;
	rectangle.height = area->height;
	error = fill_shared(&view, &rectangle, area, false, false, false, words);
	if (error != 0)
		return error;

	/* Succeeded: the sixteen words. */
	return 0;
}

/*
 * Fills the sixteen shared words of a copy that writes every step-th row
 * of a source rectangle of height total, from row phase on: destination
 * row k (from area->y) reads source row source_y + phase + step * k, and
 * the rows are clamped to the whole rectangle.  A linear destination of
 * odd rows is written as two jobs of doubled line stride, the even rows
 * and the odd ones.
 *
 * Procedure referenced: Mesa 26.2.5 src/imagination/vulkan/
 * pvr_arch_job_transfer.c (pvr_double_stride: an odd destination stride
 * doubled, the height halved, each half sampling every second source row).
 */
int
powervr_transfer_shared_words_rows(
	const struct powervr_view_shape *source,
	uint32_t source_x,
	uint32_t source_y,
	const struct powervr_render_area *area,
	uint32_t total,
	uint32_t step,
	uint32_t phase,
	uint32_t words[POWERVR_TRANSFER_SHARED_WORDS])
{
	int error;

	/* The copy's words at one row a row, then the rows' scale, offset and limits. */
	if (step == 0 || total == 0 || phase >= step)
		return EINVAL;
	error = powervr_transfer_shared_words(source, source_x, source_y, area, words);
	if (error != 0)
		return error;
	words[SHARED_FLOATS + 2] = float_from_ratio((int64_t)step, 1);
	words[SHARED_FLOATS + 3] = float_from_ratio(2 * ((int64_t)source_y + (int64_t)phase) + 1 - (int64_t)step * (2 * (int64_t)area->y + 1), 2);
	words[SHARED_LIMITS + 2] = float_from_ratio(2 * (int64_t)source_y + 1, 2);
	words[SHARED_LIMITS + 3] = float_from_ratio(2 * ((int64_t)source_y + (int64_t)total) - 1, 2);

	/* Succeeded: the sixteen words. */
	return 0;
}

/*
 * Fills the sixteen shared words of a blit: the source's image state as
 * the view gives it, a sampler that clamps and takes unnormalised
 * coordinates (linear when linear and the rectangles differ in size,
 * else point), and the floats that map the destination rectangle onto
 * the source rectangle, mirrored along an axis whose flip is set.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_transfer.c
 * (pvr_transfer_set_filter: point filtering when the sizes match;
 * pvr_sampler_state_for_surface; pvr_dma_texture_floats with
 * PVR_INT_COORD_SET_FLOATS_4).
 */
int
powervr_transfer_blit_words(
	const struct powervr_view_shape *source,
	const struct powervr_render_area *source_area,
	const struct powervr_render_area *area,
	bool flip_x,
	bool flip_y,
	bool linear,
	uint32_t words[POWERVR_TRANSFER_SHARED_WORDS])
{
	int error;

	/* Refuses missing input or result. */
	if (source == NULL || source->format == NULL || source_area == NULL || area == NULL || words == NULL)
		return EINVAL;

	/* Point filtering when no scaling is applied. */
	if (source_area->width == area->width && source_area->height == area->height)
		linear = false;

	/* The words. */
	error = fill_shared(source, source_area, area, flip_x, flip_y, linear, words);
	if (error != 0)
		return error;

	/* Succeeded: the sixteen words. */
	return 0;
}

/*
 * Reports whether a blit between two formats moves raw texels: the same
 * format, one the blit's program does not pack (powervr_transfer_blit_usc
 * takes four 8-bit unorm channels and sRGB ones), with a raw format of
 * its texel size.  Such a blit is point sampled: a depth format has no
 * linear filter, and a blit of one is between two images of that format.
 *
 * Procedure referenced: Mesa 26.2.6
 * src/imagination/vulkan/pvr_formats.c (pvr_get_image_format_features2:
 * a depth or stencil format a blit's destination, a sampled one its
 * source) and pvr_arch_job_transfer.c (pvr_pbe_src_format_ds: a depth
 * format onto itself, point sampled, as the raw format of its size, D32
 * as F32 of the same bits; pvr_pbe_src_format_raw); the Vulkan 1.3
 * specification, vkCmdBlitImage (a depth or stencil
 * blit between images of the same format, filtered by NEAREST).
 */
bool
powervr_transfer_blits_raw(
	const struct powervr_format *source,
	const struct powervr_format *destination)
{
	/* The same format, which the blit's program does not pack. */
	if (source == NULL || source != destination)
		return false;
	if (powervr_format_is_unorm8x4(source))
		return false;
	if ((source->kind & POWERVR_FORMAT_SRGB) != 0 && source->texel_bytes == 4)
		return false;

	/* A raw format of its size. */
	return powervr_transfer_raw_format(source->texel_bytes) != NULL;
}

/*
 * Fills the sixteen shared words of a blit that moves raw texels
 * (powervr_transfer_blits_raw): as powervr_transfer_blit_words, point
 * sampled, the source read as the raw format of its texel size with its
 * channels as they lie (the copy program's view).
 */
int
powervr_transfer_raw_blit_words(
	const struct powervr_view_shape *source,
	const struct powervr_render_area *source_area,
	const struct powervr_render_area *area,
	bool flip_x,
	bool flip_y,
	uint32_t words[POWERVR_TRANSFER_SHARED_WORDS])
{
	struct powervr_view_shape view;
	int error;

	/* Refuses missing input or result. */
	if (source == NULL || source->format == NULL || source_area == NULL || area == NULL || words == NULL)
		return EINVAL;

	/* The source as its raw format. */
	view = *source;
	view.format = powervr_transfer_raw_format(source->format->texel_bytes);
	if (view.format == NULL)
		return ENOTSUP;
	view.mapping[0] = VK_COMPONENT_SWIZZLE_IDENTITY;
	view.mapping[1] = VK_COMPONENT_SWIZZLE_IDENTITY;
	view.mapping[2] = VK_COMPONENT_SWIZZLE_IDENTITY;
	view.mapping[3] = VK_COMPONENT_SWIZZLE_IDENTITY;

	/* The words, point sampled. */
	error = fill_shared(&view, source_area, area, flip_x, flip_y, false, words);
	if (error != 0)
		return error;

	/* Succeeded: the sixteen words. */
	return 0;
}

/*
 * Builds the program of a copy into one aspect of a packed depth and
 * stencil destination: the source's word sampled as the copy's (as its
 * raw format, at the clamped mapped coordinates), shifted left by shift
 * (24 puts an 8-bit stencil source in a D24_UNORM_S8_UINT texel's top
 * byte), its bits inside mask kept and the destination texel's own bits
 * outside mask (sampled at the pixel's position) added, the word to
 * pixel output 0.  The other aspect is kept as it was: a transfer stores
 * whole texels, so the destination is read first.  Refuses a shift past
 * the word and an empty or full mask (EINVAL).
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_job_transfer.c
 * (pvr_pbe_src_format_pick_depth and _pick_stencil: a depth aspect onto
 * D24_UNORM_S8_UINT is DMRG_D24S8_D24S8, a stencil one SMRG_D24S8_D24S8
 * or, from an 8-bit stencil, SMRG_S8_D24S8) and src/imagination/vulkan/
 * pvr_usc.c (merge_depth_stencil: the source and the mask 0x00ffffff for
 * the depth or 0xff000000 for the stencil, ORed with the destination
 * and the mask's complement; Mesa reads the destination through the
 * tile's load, this program through the texture unit).
 */
int
powervr_transfer_merge_usc(
	uint32_t shift,
	uint32_t mask,
	struct powervr_render_usc *program)
{
	int error;

	/* Refuses a missing result, a shift past the word and a mask that keeps nothing or everything. */
	if (program == NULL || shift > 31U || mask == 0 || mask == 0xFFFFFFFFU)
		return EINVAL;

	/* The program. */
	error = build_merge_program(shift, mask, program);
	if (error != 0)
		return error;

	/* Succeeded: the merging copy's program. */
	return 0;
}

/*
 * Fills the twenty shared words of a merging copy: the copy's sixteen
 * (powervr_transfer_shared_words: the source as its raw format, the
 * point sampler, the floats and limits of the mapping), then the
 * destination's image state as the raw format of its texels, which the
 * program looks up with the same sampler.  Refuses a destination of no
 * raw format (ENOTSUP).
 */
int
powervr_transfer_merge_words(
	const struct powervr_view_shape *source,
	uint32_t source_x,
	uint32_t source_y,
	const struct powervr_render_area *area,
	const struct powervr_view_shape *destination,
	uint32_t words[POWERVR_TRANSFER_MERGE_WORDS])
{
	struct powervr_view_shape view;
	uint64_t image_words[2];
	int error;

	/* Refuses missing input or result. */
	if (source == NULL || area == NULL || destination == NULL || destination->format == NULL || words == NULL)
		return EINVAL;

	/* The copy's words. */
	error = powervr_transfer_shared_words(source, source_x, source_y, area, words);
	if (error != 0)
		return error;

	/* The destination as its raw format. */
	view = *destination;
	view.format = powervr_transfer_raw_format(destination->format->texel_bytes);
	if (view.format == NULL)
		return ENOTSUP;
	view.mapping[0] = VK_COMPONENT_SWIZZLE_IDENTITY;
	view.mapping[1] = VK_COMPONENT_SWIZZLE_IDENTITY;
	view.mapping[2] = VK_COMPONENT_SWIZZLE_IDENTITY;
	view.mapping[3] = VK_COMPONENT_SWIZZLE_IDENTITY;
	error = powervr_view_words(&view, image_words);
	if (error != 0)
		return error;

	/* Its image state, low words first. */
	words[SHARED_MERGE_IMAGE + 0] = (uint32_t)image_words[0];
	words[SHARED_MERGE_IMAGE + 1] = (uint32_t)(image_words[0] >> 32);
	words[SHARED_MERGE_IMAGE + 2] = (uint32_t)image_words[1];
	words[SHARED_MERGE_IMAGE + 3] = (uint32_t)(image_words[1] >> 32);

	/* Succeeded: the twenty words. */
	return 0;
}

/*
 * Builds the program of a copy that moves one aspect out of a packed
 * depth and stencil source: each pixel's source word (sampled as the
 * copy's, as its raw format at the clamped mapped coordinates) shifted
 * right by shift (24 leaves a D24_UNORM_S8_UINT texel's stencil in the
 * low byte) to pixel output 0, which the destination's raw format of its
 * size stores (a stencil's bytes).  Its shared words are the copy's.
 * Refuses a shift of 0 or past the word (EINVAL).
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_job_transfer.c
 * (pvr_pbe_src_format_ds: a source of a 24-bit depth onto a format
 * without one, the stencil's S8_UINT, is
 * PVR_TRANSFER_PBE_PIXEL_SRC_SWAP_LMSB) and pvr_usc.c
 * (pvr_uscgen_tq_frag_conv: SWAP_LMSB is the source word shifted right
 * by 24).
 */
int
powervr_transfer_extract_usc(
	uint32_t shift,
	struct powervr_render_usc *program)
{
	int error;

	/* Refuses a missing result and a shift of nothing or past the word. */
	if (program == NULL || shift == 0 || shift > 31U)
		return EINVAL;

	/* The program. */
	error = build_extract_program(shift, program);
	if (error != 0)
		return error;

	/* Succeeded: the extracting copy's program. */
	return 0;
}

/*
 * Fills the sixteen shared words: the view's image state, the clamping
 * unnormalised sampler, and for each axis the scale source / destination
 * and the offset (source corner * destination - source size *
 * destination corner) / destination, the destination size negated and
 * its far edge used when the axis flips; each float the correctly
 * rounded value of its ratio.  Refuses empty rectangles and sizes past
 * 2^15 (EINVAL, E2BIG).
 */
static int
fill_shared(
	const struct powervr_view_shape *view,
	const struct powervr_render_area *source,
	const struct powervr_render_area *area,
	bool flip_x,
	bool flip_y,
	bool linear,
	uint32_t words[POWERVR_TRANSFER_SHARED_WORDS])
{
	struct powervr_sampler_shape sampler;
	uint64_t image_words[2];
	uint64_t sampler_words[2];
	int64_t destination_size;
	int64_t constant;
	int error;

	/* Refuses empty and oversized rectangles. */
	if (source->width == 0 || source->height == 0 || area->width == 0 || area->height == 0)
		return EINVAL;
	if (source->x > 0x8000U || source->y > 0x8000U || source->width > 0x8000U || source->height > 0x8000U ||
	    area->x > 0x8000U || area->y > 0x8000U || area->width > 0x8000U || area->height > 0x8000U)
		return E2BIG;

	/* The source's image state. */
	error = powervr_view_words(view, image_words);
	if (error != 0)
		return error;

	/* The sampler with unnormalised coordinates. */
	sampler.mag_filter = VK_FILTER_NEAREST;
	sampler.min_filter = VK_FILTER_NEAREST;
	if (linear) {
		sampler.mag_filter = VK_FILTER_LINEAR;
		sampler.min_filter = VK_FILTER_LINEAR;
	}
	sampler.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.address_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.address_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.address_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.lod_bias_bits = 0;
	sampler.min_lod_bits = 0;
	sampler.max_lod_bits = 0;
	sampler.unnormalized = true;
	sampler.anisotropy = false;
	sampler.max_anisotropy_bits = 0;
	error = powervr_sampler_words(&sampler, sampler_words);
	if (error != 0)
		return error;

	/* As the transfer's sampler: the detail adjust at its minimum, and no w wrap but for a volume. */
	sampler_words[0] &= ~SAMPLER_DADJUST_MASK;
	if (view->kind != POWERVR_VIEW_VOLUME)
		sampler_words[0] &= ~SAMPLER_WRAP_W_MASK;

	/* The image and sampler state, low words first. */
	words[SHARED_IMAGE + 0] = (uint32_t)image_words[0];
	words[SHARED_IMAGE + 1] = (uint32_t)(image_words[0] >> 32);
	words[SHARED_IMAGE + 2] = (uint32_t)image_words[1];
	words[SHARED_IMAGE + 3] = (uint32_t)(image_words[1] >> 32);
	words[SHARED_SAMPLER + 0] = (uint32_t)sampler_words[0];
	words[SHARED_SAMPLER + 1] = (uint32_t)(sampler_words[0] >> 32);
	words[SHARED_SAMPLER + 2] = (uint32_t)sampler_words[1];
	words[SHARED_SAMPLER + 3] = (uint32_t)(sampler_words[1] >> 32);

	/* x: the scale and the offset. */
	destination_size = (int64_t)area->width;
	constant = (int64_t)source->x * destination_size - (int64_t)source->width * (int64_t)area->x;
	if (flip_x) {
		destination_size = -destination_size;
		constant = (int64_t)source->x * destination_size - (int64_t)source->width * ((int64_t)area->x + (int64_t)area->width);
	}
	words[SHARED_FLOATS + 0] = float_from_ratio((int64_t)source->width, destination_size);
	words[SHARED_FLOATS + 1] = float_from_ratio(constant, destination_size);

	/* y: the scale and the offset. */
	destination_size = (int64_t)area->height;
	constant = (int64_t)source->y * destination_size - (int64_t)source->height * (int64_t)area->y;
	if (flip_y) {
		destination_size = -destination_size;
		constant = (int64_t)source->y * destination_size - (int64_t)source->height * ((int64_t)area->y + (int64_t)area->height);
	}
	words[SHARED_FLOATS + 2] = float_from_ratio((int64_t)source->height, destination_size);
	words[SHARED_FLOATS + 3] = float_from_ratio(constant, destination_size);

	/* The limits: the centres of the source rectangle's first and last texels on each axis. */
	words[SHARED_LIMITS + 0] = float_from_ratio(2 * (int64_t)source->x + 1, 2);
	words[SHARED_LIMITS + 1] = float_from_ratio(2 * ((int64_t)source->x + (int64_t)source->width) - 1, 2);
	words[SHARED_LIMITS + 2] = float_from_ratio(2 * (int64_t)source->y + 1, 2);
	words[SHARED_LIMITS + 3] = float_from_ratio(2 * ((int64_t)source->y + (int64_t)source->height) - 1, 2);

	/* Succeeded: the sixteen words. */
	return 0;
}

/*
 * Builds the data sequencer program that copies the sixteen shared words
 * from words_address into shared registers 0 to 15.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_transfer.c
 * (pvr_3d_copy_blit_core: one texture DMA kick of the driver's shared
 * words to common store 0, pvr_pds_encode_dma_burst).
 */
int
powervr_transfer_shared_program(
	uint64_t words_address,
	struct powervr_data_shared_program *program)
{
	/* The copy's sixteen words. */
	return powervr_transfer_shared_program_words(words_address, POWERVR_TRANSFER_SHARED_WORDS, program);
}

/*
 * Builds the data sequencer program that copies count shared words (the
 * copy's sixteen, or a merging copy's twenty) from words_address into
 * shared registers 0 up.  Refuses other counts (EINVAL).
 */
int
powervr_transfer_shared_program_words(
	uint64_t words_address,
	uint32_t count,
	struct powervr_data_shared_program *program)
{
	struct powervr_data_load load;
	int error;

	/* Refuses a missing result and a count of neither program. */
	if (program == NULL || (count != POWERVR_TRANSFER_SHARED_WORDS && count != POWERVR_TRANSFER_MERGE_WORDS))
		return EINVAL;

	/* One DMA of the words to shared register 0. */
	load.kind = POWERVR_DATA_LOAD_DMA;
	load.shared = 0;
	load.words = count;
	load.source = words_address;
	error = powervr_data_shared_build(&load, 1, program);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Packs a clear colour (four float bit patterns) for the clear registers
 * as the format's clear words (powervr_format_clear_words).
 */
int
powervr_transfer_clear_colour(
	const struct powervr_format *format,
	const uint32_t colour[4],
	uint32_t packed[4])
{
	int error;

	/* The format's clear words. */
	error = powervr_format_clear_words(format, colour, packed);
	if (error != 0)
		return error;

	/* Succeeded: the packed colour. */
	return 0;
}

/*
 * Fills the registers of one transfer job and the pixel back end's two
 * state words its end-of-tile program emits.
 *
 * The copy runs a background object over the rectangle's tiles; the
 * clear runs none and lets the tiles' pixels start from the clear
 * registers.  The pixel back end clips to the rectangle within the
 * destination, and an odd linear row with more than one row is refused
 * (EINVAL: write it one row a job, powervr_transfer_needs_rows).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_transfer.c
 * (pvr_3d_copy_blit_core: the fill without a shader or background, the
 * clear registers from the packed colour; the copy with the background
 * tag enabled; pvr_setup_hwbg_object: CR_PDS_BGRND0/1/3; ISP_RENDER
 * FAST_SCALE with the scan direction; FRAG_SCREEN the destination's
 * edges less one; pvr_pbe_setup_codegen_defaults: the clip from the
 * scissor, the tiles from the clip; pvr_pbe_setup_swizzle: 8 bits a
 * channel for a fill of R8G8B8A8 or B8G8R8A8 unorm and for the F16_U8
 * pixel of a blit, as the render's words already have; pvr_isp_tiles: ISP_MTILE_SIZE, ISP_RENDER_ORIGIN,
 * ISP_CTL process_empty_tiles with the tiles in flight;
 * pvr_pbe_setup_emit: the end-of-tile program's sizes and addresses);
 * src/imagination/vulkan/pvr_arch_job_common.c
 * (pvr_arch_setup_tiles_in_flight).
 */
int
powervr_transfer_regs_build(
	const struct powervr_transfer_job *job,
	struct powervr_transfer_regs *regs,
	uint32_t state[2])
{
	struct powervr_render_background background;
	struct powervr_render_surface destination;
	uint64_t pbe[3];
	uint32_t min_x;
	uint32_t min_y;
	uint32_t max_x;
	uint32_t max_y;
	uint32_t origin_x;
	uint32_t origin_y;
	uint32_t tiles_x;
	uint32_t tiles_y;
	unsigned index;
	uint32_t registers;
	int error;
	bool rows;

	/* Refuses missing input or results, an unknown kind, and an odd linear row of several rows. */
	if (job == NULL || regs == NULL || state == NULL)
		return EINVAL;
	if (job->kind != POWERVR_TRANSFER_CLEAR && job->kind != POWERVR_TRANSFER_COPY && job->kind != POWERVR_TRANSFER_BLIT)
		return EINVAL;
	rows = powervr_transfer_needs_rows(&job->destination, &job->area);
	if (rows)
		return EINVAL;

	/* Starts from zero: no background, no clear words, no region list. */
	for (index = 0; index < 3; index++)
		regs->pds_bgnd[index] = 0;
	regs->isp_mtile_base = 0;
	for (index = 0; index < POWERVR_TRANSFER_PBE_WORDS; index++)
		regs->pbe[index] = 0;
	regs->isp_bgobjvals = 0;
	for (index = 0; index < 4; index++)
		regs->usc_clear[index] = 0;
	regs->isp_aa = 0;
	regs->isp_rgn = 0;

	/*
	 * A copy stores the destination as the raw format of its texels.  A
	 * clear writes the colour packed for the format's own pack mode, or,
	 * for a format no packed clear fills and for a depth format (which the
	 * pixel back end stores only as raw texels), the texel's bits as the
	 * raw format of its size.
	 */
	destination = job->destination;
	if (destination.format == NULL)
		return EINVAL;
	if (job->kind == POWERVR_TRANSFER_COPY) {
		destination.format = powervr_transfer_raw_format(destination.format->texel_bytes);
		if (destination.format == NULL)
			return ENOTSUP;
	} else if (job->kind == POWERVR_TRANSFER_CLEAR) {
		error = ENOTSUP;
		if ((destination.format->kind & POWERVR_FORMAT_DEPTH) == 0)
			error = powervr_transfer_clear_colour(destination.format, job->colour, regs->usc_clear);
		if (error == ENOTSUP) {
			error = powervr_format_texel_words(destination.format, job->colour, regs->usc_clear);
			destination.format = powervr_transfer_raw_format(destination.format->texel_bytes);
			if (error == 0 && destination.format == NULL)
				error = ENOTSUP;
		}
		if (error != 0)
			return error;
	}

	/* The pixel back end's words of the destination clipped to the rectangle (a raw format's integers not normalised). */
	error = powervr_render_pbe_words(&destination, &job->area, state, pbe);
	if (error != 0)
		return error;
	regs->pbe[0] = pbe[0];
	regs->pbe[1] = pbe[1];
	regs->pbe[2] = pbe[2];

	/* The copy's and the blit's background object (a clear has none). */
	if (job->kind != POWERVR_TRANSFER_CLEAR) {
		background.kick_offset = job->copy_kick_offset;
		background.shared_code_offset = job->shared_code_offset;
		background.shared_data_offset = job->shared_data_offset;
		background.shared_data_words = job->shared_data_words;
		background.shared_registers = POWERVR_TRANSFER_SHARED_WORDS;
		if (job->shared_registers != 0)
			background.shared_registers = job->shared_registers;
		error = powervr_render_background_words(&background, regs->pds_bgnd);
		if (error != 0)
			return error;
		regs->isp_bgobjvals = BGOBJ_ENABLE_TAG;
	}

	/* The clip again, for the tiles it covers. */
	min_x = (uint32_t)((pbe[0] >> 6) & 0x3fffU);
	max_y = (uint32_t)(pbe[1] & 0x3fffU);
	max_x = (uint32_t)((pbe[1] >> 32) & 0x3fffU);
	min_y = (uint32_t)((pbe[1] >> 46) & 0x3fffU);
	origin_x = min_x / TILE_SIZE;
	origin_y = min_y / TILE_SIZE;
	tiles_x = (max_x + TILE_SIZE) / TILE_SIZE - origin_x;
	tiles_y = (max_y + TILE_SIZE) / TILE_SIZE - origin_y;

	/*
	 * Four samples a pixel: the ISP's 4x antialiasing, whose tile of one
	 * sample a pixel covers 8 x 8 pixels, so the tiles' origin and count
	 * are twice as many across and down.
	 *
	 * Procedure referenced: Mesa 26.2.6 src/imagination/vulkan/
	 * pvr_arch_job_transfer.c (pvr_msaa_state: CR_ISP_AA mode of the
	 * samples for M -> M; pvr_isp_tiles: with one ISP sample a pixel, 4X
	 * doubles origin_x, origin_y, width and height); csbgen/rogue/cr.xml
	 * (ISP_AA_MODE_TYPE AA_4X 2).
	 */
	if (job->samples == 4U) {
		regs->isp_aa = ISP_AA_4X;
		origin_x <<= 1;
		origin_y <<= 1;
		tiles_x <<= 1;
		tiles_y <<= 1;
	} else if (job->samples > 1U) {
		return EINVAL;
	}
	if (tiles_x > TILE_FIELD_MAX || tiles_y > TILE_FIELD_MAX)
		return E2BIG;
	regs->isp_mtile_size = (tiles_x << 16) | tiles_y;
	regs->isp_render_origin = (origin_x << 16) | origin_y;

	/* The tiles in flight and the pixel output's partitions, for the destination's registers a pixel (four for 128-bit texels). */
	registers = MIN_OUTPUT_REGISTERS;
	if (destination.format != NULL && destination.format->texel_bytes > 8)
		registers = 4;
	tiles_in_flight(registers, &regs->isp_ctl, &regs->usc_pixel_output_ctrl);
	regs->isp_ctl |= ISP_PROCESS_EMPTY;

	/* The end-of-tile program's words. */
	error = powervr_render_event_words(job->event_data_offset, &regs->event_pixel_pds_code, &regs->event_pixel_pds_data, &regs->event_pixel_pds_info);
	if (error != 0)
		return error;

	/* A fast scaled render scanning from the top left, over the destination's screen. */
	regs->isp_render = RENDER_FAST_SCALE;
	if (job->destination.width - 1U > SCREEN_FIELD_MAX || job->destination.height - 1U > SCREEN_FIELD_MAX)
		return E2BIG;
	regs->frag_screen = (job->destination.width - 1U) | ((job->destination.height - 1U) << 16);

	/* Succeeded: the registers and the state words. */
	return 0;
}

/* Starts a machine operation that names no register. */
static void
blank_operation(
	struct shader_machine_operation *operation,
	unsigned kind)
{
	unsigned index;

	/* Everything zero but the kind, and no register in any slot. */
	*operation = (struct shader_machine_operation){ 0 };
	operation->kind = (uint8_t)kind;
	for (index = 0; index < 2; index++)
		operation->destination[index] = no_register;
	for (index = 0; index < 4; index++)
		operation->source[index] = no_register;
}

/* Names a register of a bank. */
static struct shader_machine_register
machine_register(
	unsigned bank,
	unsigned index)
{
	struct shader_machine_register value;

	/* The bank and the number. */
	value.bank = (uint8_t)bank;
	value.index = (uint16_t)index;
	return value;
}

/* Builds the compiler back end's group of each operation. */
static int
build_groups(
	const struct shader_machine_operation *operations,
	size_t count,
	struct powervr_usc_group *groups)
{
	size_t index;
	int error;

	/* One group an operation; none branches. */
	for (index = 0; index < count; index++) {
		error = shader_backend_group(&operations[index], 0, &groups[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the groups. */
	return 0;
}

/*
 * The bits of the float nearest numerator / denominator (round to
 * nearest, ties to even), for a nonzero denominator and a quotient in
 * the normal range; both below 2^40 in magnitude.
 */
static uint32_t
float_from_ratio(
	int64_t numerator,
	int64_t denominator)
{
	uint64_t dividend;
	uint64_t divisor;
	uint64_t quotient;
	uint64_t remainder;
	uint64_t mantissa;
	uint32_t sign;
	int32_t exponent;
	unsigned sticky;

	/* Zero, and the sign of the quotient. */
	if (numerator == 0 || denominator == 0)
		return 0;
	sign = 0;
	if ((numerator < 0) != (denominator < 0))
		sign = 0x80000000U;
	dividend = (uint64_t)(numerator < 0 ? -numerator : numerator);
	divisor = (uint64_t)(denominator < 0 ? -denominator : denominator);

	/* The quotient to 26 significant bits (24, a round bit and a guard bit) and whether anything is left. */
	quotient = dividend / divisor;
	remainder = dividend % divisor;
	exponent = 25;
	sticky = 0;
	while (quotient >= (1ULL << 26)) {
		sticky |= (unsigned)(quotient & 1U);
		quotient >>= 1;
		exponent++;
	}
	while (quotient < (1ULL << 25)) {
		remainder <<= 1;
		quotient <<= 1;
		if (remainder >= divisor) {
			quotient |= 1U;
			remainder -= divisor;
		}
		exponent--;
	}
	if (remainder != 0)
		sticky = 1;

	/* Rounds the 24-bit mantissa to nearest, ties to even. */
	mantissa = quotient >> 2;
	if ((quotient & 2U) != 0 && ((quotient & 1U) != 0 || sticky != 0 || (mantissa & 1U) != 0))
		mantissa++;
	if (mantissa == (1ULL << 24)) {
		mantissa >>= 1;
		exponent++;
	}

	/* The bits: the exponent of the leading one, the 23 bits below it. */
	return sign | ((uint32_t)(exponent + 127) << 23) | ((uint32_t)mantissa & 0x7fffffU);
}

/*
 * Works out the BXE-4-32's tiles in flight and pixel output control for
 * a pixel of the given output registers (two or four; the core's least
 * is two): partitions of 256 pixels of that width in the reserved space,
 * no more than ten, and no more tiles in flight than partitions (four
 * at most).
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_common.c
 * (pvr_arch_setup_tiles_in_flight: the width rounded up to the least and
 * to a power of two, the partition a 16x16 tile of it, the partitions
 * available, the ISP tiles no more than they); src/imagination/csbgen/
 * rogue/cr.xml (USC_PIXEL_OUTPUT_CTRL width: 2REGISTERS 0, 4REGISTERS 1;
 * partition_mask 3..20).
 */
static void
tiles_in_flight(
	uint32_t registers,
	uint32_t *isp_ctl,
	uint32_t *pixel_output_ctrl)
{
	uint32_t partition_size;
	uint32_t partitions;
	uint32_t tiles;

	/* Partitions of a tile of that width within the reserved space. */
	partition_size = registers * TILE_SIZE * TILE_SIZE;
	partitions = RESERVED_PARTITIONS / partition_size;
	if (partitions > MAX_PARTITIONS)
		partitions = MAX_PARTITIONS;

	/* The ISP's tiles in flight, no more than the partitions. */
	tiles = ISP_TILES_IN_FLIGHT;
	if (tiles > partitions)
		tiles = partitions;

	/* The pipes enabled, the width (two registers 0, four 1) with every partition in the mask. */
	*isp_ctl = (tiles - 1U) << ISP_PIPE_SHIFT;
	*pixel_output_ctrl = ((1U << MAX_PARTITIONS) - 1U) << PIXEL_PARTITION_SHIFT;
	if (registers == 4)
		*pixel_output_ctrl |= PIXEL_WIDTH_4_REGISTERS;
}

/*
 * Fills the first MAPPED_INDEX operations of a program that reads its
 * source at mapped coordinates: the pixel's position into temporaries 0
 * and 1, the source's coordinates (the position times the shared scale
 * plus the shared offset) into temporaries 2 and 3 (the floats go
 * through temporaries 8 to 11; a multiply-add reads one shared register
 * at most).
 */
static void
mapped_coordinates(
	struct shader_machine_operation operations[MAPPED_INDEX])
{
	unsigned axis;

	/* The position's x and y into temporaries 0 and 1. */
	for (axis = 0; axis < 2; axis++) {
		blank_operation(&operations[axis], MACHINE_MOVE_SPECIAL);
		operations[axis].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_POSITION + axis);
		if (axis == 0) {
			operations[axis].source[0] = machine_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_X);
		} else {
			operations[axis].source[0] = machine_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_Y);
		}
	}

	/* The four floats into temporaries 8 to 11. */
	for (axis = 0; axis < 4; axis++) {
		blank_operation(&operations[2 + axis], MACHINE_MOVE);
		operations[2 + axis].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_FLOATS + axis);
		operations[2 + axis].source[0] = machine_register(POWERVR_USC_BANK_SHARED, SHARED_FLOATS + axis);
	}

	/* Each coordinate: the position times its scale plus its offset. */
	for (axis = 0; axis < 2; axis++) {
		blank_operation(&operations[6 + axis], MACHINE_FLOAT_MULTIPLY_ADD);
		operations[6 + axis].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_COORDINATES + axis);
		operations[6 + axis].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_POSITION + axis);
		operations[6 + axis].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_FLOATS + axis * 2U);
		operations[6 + axis].source[2] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_FLOATS + axis * 2U + 1U);
	}
}

/*
 * Fills the copy's first SAMPLE_INDEX operations: the source's
 * coordinates of mapped_coordinates, each clamped to the shared limits
 * (the limits go through temporaries 8 to 11, over the floats).
 */
static void
copy_coordinates(
	struct shader_machine_operation operations[SAMPLE_INDEX])
{
	unsigned axis;

	/* The mapped coordinates in temporaries 2 and 3. */
	mapped_coordinates(operations);

	/* The four limits into temporaries 8 to 11, over the floats the multiply-adds took. */
	for (axis = 0; axis < 4; axis++) {
		blank_operation(&operations[8 + axis], MACHINE_MOVE);
		operations[8 + axis].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_LIMITS + axis);
		operations[8 + axis].source[0] = machine_register(POWERVR_USC_BANK_SHARED, SHARED_LIMITS + axis);
	}

	/* Each coordinate no less than its least and no more than its most. */
	for (axis = 0; axis < 2; axis++) {
		blank_operation(&operations[12 + axis * 2], MACHINE_MINMAX);
		operations[12 + axis * 2].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_COORDINATES + axis);
		operations[12 + axis * 2].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_COORDINATES + axis);
		operations[12 + axis * 2].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_LIMITS + axis * 2U);
		operations[12 + axis * 2].variant = TEST_GREATER;
		operations[12 + axis * 2].immediate = TYPE_F32;
		blank_operation(&operations[13 + axis * 2], MACHINE_MINMAX);
		operations[13 + axis * 2].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_COORDINATES + axis);
		operations[13 + axis * 2].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_COORDINATES + axis);
		operations[13 + axis * 2].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_LIMITS + axis * 2U + 1U);
		operations[13 + axis * 2].variant = TEST_LESS;
		operations[13 + axis * 2].immediate = TYPE_F32;
	}
}

/*
 * Builds the merging copy's program (powervr_transfer_merge_usc): the
 * copy's coordinates and source sample, the destination's sample at the
 * pixel's position, the source's word shifted, the two words masked and
 * ORed, the word to pixel output 0 under the overlap check, ending the
 * program.
 */
static int
build_merge_program(
	uint32_t shift,
	uint32_t mask,
	struct powervr_render_usc *program)
{
	struct shader_machine_operation operations[MERGE_OPERATIONS];
	struct powervr_usc_group groups[MERGE_OPERATIONS];
	size_t sample_at[2];
	size_t length;
	size_t count;
	unsigned sample;
	int error;

	/* The source's coordinates, clamped, in temporaries 2 and 3. */
	copy_coordinates(operations);
	count = SAMPLE_INDEX;

	/* The source's texel at its coordinates, the destination's at the pixel's position, each waited for. */
	for (sample = 0; sample < 2U; sample++) {
		sample_at[sample] = count;
		blank_operation(&operations[count], MACHINE_SAMPLE);
		if (sample == 0) {
			operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE);
			operations[count].source[0] = machine_register(POWERVR_USC_BANK_SHARED, SHARED_IMAGE);
			operations[count].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_COORDINATES);
		} else {
			operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_MERGE_SAMPLE);
			operations[count].source[0] = machine_register(POWERVR_USC_BANK_SHARED, SHARED_MERGE_IMAGE);
			operations[count].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_POSITION);
		}
		operations[count].source[2] = machine_register(POWERVR_USC_BANK_SHARED, SHARED_SAMPLER);
		count++;
		blank_operation(&operations[count], MACHINE_WAIT);
		count++;
	}

	/* The masks: the source's bits and the destination's. */
	blank_operation(&operations[count], MACHINE_IMMEDIATE);
	operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_MERGE_MASK);
	operations[count].immediate = mask;
	count++;
	blank_operation(&operations[count], MACHINE_IMMEDIATE);
	operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_MERGE_KEEP);
	operations[count].immediate = ~mask;
	count++;

	/* The source's word moved to its aspect's bits. */
	if (shift != 0) {
		blank_operation(&operations[count], MACHINE_IMMEDIATE);
		operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_MERGE_SHIFT);
		operations[count].immediate = shift;
		count++;
		blank_operation(&operations[count], MACHINE_SHIFT);
		operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE);
		operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE);
		operations[count].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_MERGE_SHIFT);
		operations[count].variant = MERGE_SHIFT_LEFT;
		count++;
	}

	/* The source's bits, the destination's other bits, the two together. */
	blank_operation(&operations[count], MACHINE_LOGIC);
	operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE);
	operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE);
	operations[count].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_MERGE_MASK);
	operations[count].variant = MERGE_AND;
	count++;
	blank_operation(&operations[count], MACHINE_LOGIC);
	operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_MERGE_SAMPLE);
	operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_MERGE_SAMPLE);
	operations[count].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_MERGE_KEEP);
	operations[count].variant = MERGE_AND;
	count++;
	blank_operation(&operations[count], MACHINE_LOGIC);
	operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE);
	operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE);
	operations[count].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_MERGE_SAMPLE);
	operations[count].variant = MERGE_OR;
	count++;

	/* The word to pixel output 0 under the overlap check. */
	blank_operation(&operations[count], MACHINE_MOVE);
	operations[count].destination[0] = machine_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_PIXEL_OUTPUT);
	operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE);
	operations[count].variant = 1;
	count++;

	/* The groups, the two samples' raw words, the last one ending. */
	error = build_groups(operations, count, groups);
	if (error != 0)
		return error;
	for (sample = 0; sample < 2U; sample++) {
		error = powervr_usc_sample(0, 0, SAMPLE_2D, SAMPLE_CHANNELS, 0, &groups[sample_at[sample]].operation[POWERVR_USC_PHASE_BACKEND]);
		if (error != 0)
			return error;
	}
	groups[count - 1U].end = 1;

	/* Encodes them. */
	error = powervr_usc_program_encode(groups, count, program->code, sizeof(program->code), &length);
	if (error != 0)
		return error;

	/* Succeeded: the program and its temporaries. */
	program->length = length;
	program->temps = MERGE_TEMPS;
	return 0;
}

/*
 * Builds the extracting copy's program: the copy's clamped coordinates,
 * the source's raw sample, the shift right, the word to pixel output 0.
 */
static int
build_extract_program(
	uint32_t shift,
	struct powervr_render_usc *program)
{
	struct shader_machine_operation operations[COPY_OPERATIONS];
	struct powervr_usc_group groups[COPY_OPERATIONS];
	size_t length;
	size_t count;
	int error;

	/* The source's coordinates, clamped, in temporaries 2 and 3. */
	copy_coordinates(operations);
	count = SAMPLE_INDEX;

	/* The source's texel at its coordinates, waited for. */
	blank_operation(&operations[count], MACHINE_SAMPLE);
	operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE);
	operations[count].source[0] = machine_register(POWERVR_USC_BANK_SHARED, SHARED_IMAGE);
	operations[count].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_COORDINATES);
	operations[count].source[2] = machine_register(POWERVR_USC_BANK_SHARED, SHARED_SAMPLER);
	count++;
	blank_operation(&operations[count], MACHINE_WAIT);
	count++;

	/* The word shifted right to the aspect's bits. */
	blank_operation(&operations[count], MACHINE_IMMEDIATE);
	operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_EXTRACT_SHIFT);
	operations[count].immediate = shift;
	count++;
	blank_operation(&operations[count], MACHINE_SHIFT);
	operations[count].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE);
	operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE);
	operations[count].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_EXTRACT_SHIFT);
	operations[count].variant = EXTRACT_SHIFT_RIGHT;
	count++;

	/* The word to pixel output 0 under the overlap check. */
	blank_operation(&operations[count], MACHINE_MOVE);
	operations[count].destination[0] = machine_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_PIXEL_OUTPUT);
	operations[count].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE);
	operations[count].variant = 1;
	count++;

	/* The groups, the sample's raw words, the last one ending. */
	error = build_groups(operations, count, groups);
	if (error != 0)
		return error;
	error = powervr_usc_sample(0, 0, SAMPLE_2D, SAMPLE_CHANNELS, 0, &groups[SAMPLE_INDEX].operation[POWERVR_USC_PHASE_BACKEND]);
	if (error != 0)
		return error;
	groups[count - 1U].end = 1;

	/* Encodes them. */
	error = powervr_usc_program_encode(groups, count, program->code, sizeof(program->code), &length);
	if (error != 0)
		return error;

	/* Succeeded: the program and its temporaries. */
	program->length = length;
	program->temps = COPY_TEMPS;
	return 0;
}
