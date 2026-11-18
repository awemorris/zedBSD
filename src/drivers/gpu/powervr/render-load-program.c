/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The attachment load background of a PowerVR render.  See
 * render-load-program.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libc/vulkan/vulkan_core.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/data-program.h"
#include "drivers/gpu/powervr/image-state.h"
#include "drivers/gpu/powervr/render-load-program.h"
#include "drivers/gpu/powervr/render-program.h"
#include "drivers/gpu/powervr/compiler/shader-private.h"
#include "drivers/gpu/powervr/compiler/usc-encode.h"

/*
 * The program's registers: the image state in shared registers 0 to 3,
 * the sampler state in 4 to 7; the pixel's position in temporaries 0
 * and 1, the sampled words in 4 to 7, the packed pixel in 8.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_usc.c (pvr_uscgen_loadop with
 * one loaded render target: the coordinates the position's x and y, the
 * image and sampler state from shared registers, the sample with
 * unnormalised coordinates, the result stored to the target's output
 * register in the attachment's format).
 */
#define SHARED_IMAGE		0U
#define SHARED_SAMPLER		4U
#define TEMP_POSITION		0U
#define TEMP_SAMPLE		4U
#define TEMP_PIXEL		8U
#define LOAD_TEMPS		9U

/*
 * The special registers of the pixel's position at the pixel rate, the
 * first pixel output register, the sample's 2D lookup of four channels,
 * and the pack of four 8-bit unorm channels with its scale.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/pco/pco_isa.py (special registers x_p 97,
 * y_p 100, pixout 32; F_DIM 2d 0b10; F_PCK_FMT u8888 0, the scale
 * flag); the compiler back end's pack (shader-backend.c PACK_U8888 0,
 * PACK_SCALE 1).
 */
#define SPECIAL_X		97U
#define SPECIAL_Y		100U
#define SPECIAL_PIXEL_OUTPUT	32U
#define SAMPLE_2D		2U
#define SAMPLE_CHANNELS		4U
#define PACK_U8888		0U
#define PACK_SCALE		1U

/*
 * The sampler word's detail adjust and w wrap fields.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/csbgen/rogue/texstate.xml (SAMPLER_WORD0
 * dadjust 0..12, addrmode_w 56..58).
 */
#define SAMPLER_DADJUST_MASK	0x1fffULL
#define SAMPLER_WRAP_W_MASK	(7ULL << 56)

/*
 * The partial render's load: its shared words (the registers' region's
 * image state at SHARED_IMAGE and the sampler at SHARED_SAMPLER as the
 * load's, then for each tile buffer eight words, its region's image
 * state, its address and two words of zero); its temporaries (the
 * position in 0 and 1, the lookup word in 2, the sample number and its
 * scale in 3 and 4, each region's four sampled words from 8); the
 * special registers of the sample number, of zero and of the first
 * tiled store offset; the tiled form of a store; the most operations.
 *
 * Values referenced: Mesa 26.2.6 src/imagination/pco/pco_isa.py
 * (special registers samp_num 53, tiled_st_comp0 58; the constant 0 in
 * special register 0); zedBSD src/drivers/gpu/powervr/compiler/
 * shader-backend.c (STORE_TILED 1, SPECIAL_ZERO 0).
 */
#define PARTIAL_SHARED_BUFFER		8U
#define PARTIAL_SHARED_BUFFER_WORDS	8U
#define PARTIAL_TEMP_LOOKUP		2U
#define PARTIAL_TEMP_NUMBER		3U
#define PARTIAL_TEMP_SCALE		4U
#define PARTIAL_TEMP_REGIONS		8U
#define SPECIAL_SAMPLE_NUMBER		53U
#define SPECIAL_ZERO			0U
#define SPECIAL_TILED_STORE		58U
#define SAMPLE_NUMBER_SHIFT		16U
#define STORE_TILED			1U
#define PARTIAL_OPERATIONS		128U

static const struct shader_machine_register no_register = { 0xff, 0 };

static uint32_t partial_format(uint32_t words);
static void blank_operation(struct shader_machine_operation *operation, unsigned kind);
static struct shader_machine_register machine_register(unsigned bank, unsigned index);

/*
 * Builds the load program for an attachment of four 8-bit unorm
 * channels: the position from the special registers, the attachment
 * sampled there with unnormalised coordinates, the four channels packed
 * into pixel output register 0 under the overlap check, ending the
 * program.  Other formats are not taken yet (ENOTSUP).
 *
 * The groups are the compiler back end's (shader_backend_group); the
 * sample is rebuilt with unnormalised coordinates.
 */
int
powervr_render_load_usc(
	const struct powervr_format *format,
	struct powervr_render_usc *program)
{
	int error;

	/* The usual form. */
	error = powervr_render_load_usc_probe(format, POWERVR_LOAD_PROBE_USUAL, program);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Builds the load program in one of its probe forms: the usual one, the
 * sample at (0, 0) instead of the pixel's position, or the usual one
 * with a wait group after each group.
 */
int
powervr_render_load_usc_probe(
	const struct powervr_format *format,
	unsigned probe,
	struct powervr_render_usc *program)
{
	struct shader_machine_operation operations[6];
	struct powervr_usc_group groups[12];
	size_t length;
	size_t index;
	size_t count;
	size_t sample;
	int error;

	/* Refuses missing input or result, an unknown form, and formats other than four 8-bit unorm channels. */
	if (format == NULL || program == NULL || probe > POWERVR_LOAD_PROBE_SPACED)
		return EINVAL;
	if (!powervr_format_is_unorm8x4(format))
		return ENOTSUP;

	/* The position's x and y, or zero for the origin probe. */
	if (probe == POWERVR_LOAD_PROBE_ORIGIN) {
		blank_operation(&operations[0], MACHINE_IMMEDIATE);
		operations[0].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_POSITION);
		blank_operation(&operations[1], MACHINE_IMMEDIATE);
		operations[1].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_POSITION + 1U);
	} else {
		blank_operation(&operations[0], MACHINE_MOVE_SPECIAL);
		operations[0].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_POSITION);
		operations[0].source[0] = machine_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_X);
		blank_operation(&operations[1], MACHINE_MOVE_SPECIAL);
		operations[1].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_POSITION + 1U);
		operations[1].source[0] = machine_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_Y);
	}

	/* The sample there, and the wait for its data. */
	blank_operation(&operations[2], MACHINE_SAMPLE);
	operations[2].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE);
	operations[2].source[0] = machine_register(POWERVR_USC_BANK_SHARED, SHARED_IMAGE);
	operations[2].source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_POSITION);
	operations[2].source[2] = machine_register(POWERVR_USC_BANK_SHARED, SHARED_SAMPLER);
	blank_operation(&operations[3], MACHINE_WAIT);

	/* The four channels packed. */
	blank_operation(&operations[4], MACHINE_PACK);
	operations[4].destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_PIXEL);
	operations[4].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_SAMPLE);
	operations[4].variant = PACK_U8888;
	operations[4].immediate = 4;
	operations[4].immediate_2 = PACK_SCALE;

	/* The pixel to the output under the overlap check. */
	blank_operation(&operations[5], MACHINE_MOVE);
	operations[5].destination[0] = machine_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_PIXEL_OUTPUT);
	operations[5].source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_PIXEL);
	operations[5].variant = 1;

	/* The back end's group of each, with a wait group after each but the last for the spaced probe. */
	count = 0;
	sample = 0;
	for (index = 0; index < 6; index++) {
		if (index == 2)
			sample = count;
		error = shader_backend_group(&operations[index], 0, &groups[count]);
		if (error != 0)
			return error;
		count++;
		if (probe == POWERVR_LOAD_PROBE_SPACED && index < 5U) {
			error = shader_backend_group(&operations[3], 0, &groups[count]);
			if (error != 0)
				return error;
			count++;
		}
	}

	/* The sample with unnormalised coordinates, the last group ending. */
	error = powervr_usc_sample(0, 0, SAMPLE_2D, SAMPLE_CHANNELS, 0, &groups[sample].operation[POWERVR_USC_PHASE_BACKEND]);
	if (error != 0)
		return error;
	groups[count - 1U].end = 1;

	/* Encodes them. */
	error = powervr_usc_program_encode(groups, count, program->code, sizeof(program->code), &length);
	if (error != 0)
		return error;

	/* Succeeded: the program and its temporaries. */
	program->length = length;
	program->temps = LOAD_TEMPS;
	return 0;
}

/*
 * Fills the eight shared words: the attachment's image state and a
 * point sampler that clamps and takes unnormalised coordinates.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_load_op_constants_create_and_upload: the attachment's image
 * descriptor and a nearest sampler); src/imagination/vulkan/
 * pvr_arch_job_transfer.c (pvr_sampler_state_for_surface: the point,
 * clamped, unnormalised sampler with dadjust MIN_UINT and no w wrap).
 */
int
powervr_render_load_words(
	const struct powervr_view_shape *attachment,
	uint32_t words[POWERVR_RENDER_LOAD_SHARED_WORDS])
{
	struct powervr_sampler_shape sampler;
	uint64_t image_words[2];
	uint64_t sampler_words[2];
	int error;

	/* Refuses missing input or result. */
	if (attachment == NULL || words == NULL)
		return EINVAL;

	/* The attachment's image state. */
	error = powervr_view_words(attachment, image_words);
	if (error != 0)
		return error;

	/* The point sampler with unnormalised coordinates. */
	sampler.mag_filter = VK_FILTER_NEAREST;
	sampler.min_filter = VK_FILTER_NEAREST;
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
	sampler_words[0] &= ~(SAMPLER_DADJUST_MASK | SAMPLER_WRAP_W_MASK);

	/* The words, low halves first. */
	words[SHARED_IMAGE + 0] = (uint32_t)image_words[0];
	words[SHARED_IMAGE + 1] = (uint32_t)(image_words[0] >> 32);
	words[SHARED_IMAGE + 2] = (uint32_t)image_words[1];
	words[SHARED_IMAGE + 3] = (uint32_t)(image_words[1] >> 32);
	words[SHARED_SAMPLER + 0] = (uint32_t)sampler_words[0];
	words[SHARED_SAMPLER + 1] = (uint32_t)(sampler_words[0] >> 32);
	words[SHARED_SAMPLER + 2] = (uint32_t)sampler_words[1];
	words[SHARED_SAMPLER + 3] = (uint32_t)(sampler_words[1] >> 32);

	/* Succeeded: the eight words. */
	return 0;
}

/*
 * Builds the data sequencer program that copies the eight shared words
 * from words_address into shared registers 0 to 7.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_load_op_pds_data_create_and_upload: one texture DMA of the
 * constants to common store 0).
 */
int
powervr_render_load_shared(
	uint64_t words_address,
	struct powervr_data_shared_program *program)
{
	struct powervr_data_load load;
	int error;

	/* Refuses a missing result. */
	if (program == NULL)
		return EINVAL;

	/* One DMA of the eight words to shared register 0. */
	load.kind = POWERVR_DATA_LOAD_DMA;
	load.shared = 0;
	load.words = POWERVR_RENDER_LOAD_SHARED_WORDS;
	load.source = words_address;
	error = powervr_data_shared_build(&load, 1, program);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Plans the scratch buffer of a render's partial renders (see
 * render-load-program.h): registers is the pixel's output registers
 * (1 to 4), buffers its tile buffers.  Each region is laid out as an
 * image of the render's edges, linear for one sample and twiddled with
 * four samples a texel for four, on the image boundary after the one
 * before.  Returns EINVAL for an empty render, another sample count or
 * no register, and ENOTSUP for more tile buffers than the emits after
 * the registers' one.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_spm.c
 * (pvr_arch_spm_init_eot_state, pvr_spm_setup_pbe_state: a linear
 * region of the framebuffer's edges for each, sample_count samples a
 * texel, the next region after it; pvr_arch_spm_init_bgobj_state:
 * MIN2(output_regs_count, 4) words, four with tile buffers).  Four
 * samples are laid out as the driver lays out a four-sample attachment
 * (powervr_image_lay_out_samples), which the pixel back end stores and
 * the texture unit reads by sample number: the texture unit takes the
 * sample count of a twiddled image alone (powervr_view_words).
 */
int
powervr_render_partial_plan(
	uint32_t width,
	uint32_t height,
	uint32_t samples,
	uint32_t registers,
	uint32_t buffers,
	struct powervr_render_partial *partial)
{
	struct powervr_image_shape shape;
	uint64_t offset;
	uint32_t region;
	uint32_t kind;
	int error;

	/* Refuses a missing result, an empty render, another sample count, no register and too many tile buffers. */
	if (partial == NULL || width == 0 || height == 0)
		return EINVAL;
	if (samples != 1U && samples != 4U)
		return EINVAL;
	if (registers == 0 || registers > 4U)
		return EINVAL;
	if (buffers > POWERVR_RENDER_PARTIAL_REGIONS_MAX - 1U)
		return ENOTSUP;
	*partial = (struct powervr_render_partial){ 0 };
	partial->width = width;
	partial->height = height;
	partial->samples = samples;
	partial->buffers = buffers;

	/* The words a pixel: the registers' as 1, 2 or 4; a tile buffer's as the end of tile loads it, 2 or 4. */
	partial->words = registers <= 2U ? registers : 4U;
	partial->tile_words = registers <= 2U ? 2U : 4U;

	/* The two kinds of region: the registers', a tile buffer's. */
	shape.width = width;
	shape.height = height;
	shape.depth = 1;
	shape.levels = 1;
	shape.layers = 1;
	shape.linear = samples == 1U;
	shape.volume = false;
	for (kind = 0; kind < 2U; kind++) {
		partial->formats[kind] = powervr_format_find(partial_format(kind == 0 ? partial->words : partial->tile_words));
		if (partial->formats[kind] == NULL)
			return ENOTSUP;
		error = powervr_image_lay_out_samples(&shape, partial->formats[kind], samples, &partial->layouts[kind]);
		if (error != 0)
			return error;
	}

	/* Each region on the image boundary after the one before. */
	offset = 0;
	for (region = 0; region <= buffers; region++) {
		partial->offsets[region] = offset;
		offset += partial->layouts[region == 0 ? 0 : 1].bytes;
		offset = (offset + POWERVR_IMAGE_ALIGN - 1U) & ~(uint64_t)(POWERVR_IMAGE_ALIGN - 1U);
	}
	partial->bytes = offset;

	/* Succeeded: the plan. */
	return 0;
}

/*
 * Packs the pixel back end words of a partial render's emit of a region
 * (0 the output registers, 1 up the tile buffers) of the scratch buffer
 * at scratch: the region's raw texels over the whole render, from pixel
 * output register 0 (where the end of tile loads a tile buffer's words
 * first), its index the region's.
 */
int
powervr_render_partial_pbe(
	const struct powervr_render_partial *partial,
	uint64_t scratch,
	uint32_t region,
	uint32_t state[2],
	uint64_t reg[3])
{
	struct powervr_render_surface surface;
	struct powervr_render_area area;
	uint32_t kind;
	int error;

	/* Refuses missing input or results and a region the plan has not. */
	if (partial == NULL || state == NULL || reg == NULL || region > partial->buffers)
		return EINVAL;
	kind = region == 0 ? 0U : 1U;

	/* The region as a surface of the render's edges, all of it written. */
	surface.format = partial->formats[kind];
	surface.address = scratch + partial->offsets[region];
	surface.width = partial->width;
	surface.height = partial->height;
	surface.linear = partial->samples == 1U;
	surface.row_pitch = partial->layouts[kind].row_pitch;
	surface.padded_width = partial->layouts[kind].padded_width;
	surface.depth = 0;
	surface.slice = 0;
	area.x = 0;
	area.y = 0;
	area.width = partial->width;
	area.height = partial->height;
	error = powervr_render_pbe_words(&surface, &area, state, reg);
	if (error != 0)
		return error;

	/* From register 0, the region's emit. */
	error = powervr_render_pbe_place(state, 0, region);
	if (error != 0)
		return error;

	/* Succeeded: the region's words. */
	return 0;
}

/*
 * Fills the shared words of the partial render's load (*count of them):
 * the registers' region's image state and the point sampler with
 * unnormalised coordinates (powervr_render_load_words), then for each
 * tile buffer its region's image state, the buffer's address and two
 * words of zero.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_spm.c
 * (pvr_arch_spm_init_bgobj_state: a point, clamped, unnormalised
 * sampler; each region's texture state, R32, R32G32 or R32G32B32A32
 * UINT, strided over the framebuffer, sample_count samples; each tile
 * buffer's address) and pvr_usc.h (pvr_uscgen_spm_buffer_data).
 */
int
powervr_render_partial_words(
	const struct powervr_render_partial *partial,
	uint64_t scratch,
	const uint64_t *tile_addresses,
	uint32_t words[POWERVR_RENDER_PARTIAL_WORDS_MAX],
	uint32_t *count)
{
	struct powervr_view_shape view;
	uint64_t image_words[2];
	uint32_t region;
	uint32_t base;
	uint32_t kind;
	int error;

	/* Refuses missing input or results. */
	if (partial == NULL || words == NULL || count == NULL || (partial->buffers != 0 && tile_addresses == NULL))
		return EINVAL;

	/* Each region's view: flat, one level, one layer, the channels as they are. */
	for (region = 0; region <= partial->buffers; region++) {
		kind = region == 0 ? 0U : 1U;
		view = (struct powervr_view_shape){ 0 };
		view.format = partial->formats[kind];
		view.layout = &partial->layouts[kind];
		view.address = scratch + partial->offsets[region];
		view.width = partial->width;
		view.height = partial->height;
		view.depth = 1;
		view.kind = POWERVR_VIEW_FLAT;
		view.image_levels = 1;
		view.base_level = 0;
		view.level_count = 1;
		view.base_layer = 0;
		view.layer_count = 1;
		view.linear = partial->samples == 1U;

		/* The registers' region with the sampler; a tile buffer's with its buffer's address. */
		if (region == 0) {
			error = powervr_render_load_words(&view, words);
			if (error != 0)
				return error;
			continue;
		}
		error = powervr_view_words(&view, image_words);
		if (error != 0)
			return error;
		if (tile_addresses[region - 1U] == 0)
			return EINVAL;
		base = PARTIAL_SHARED_BUFFER + (region - 1U) * PARTIAL_SHARED_BUFFER_WORDS;
		words[base + 0U] = (uint32_t)image_words[0];
		words[base + 1U] = (uint32_t)(image_words[0] >> 32);
		words[base + 2U] = (uint32_t)image_words[1];
		words[base + 3U] = (uint32_t)(image_words[1] >> 32);
		words[base + 4U] = (uint32_t)tile_addresses[region - 1U];
		words[base + 5U] = (uint32_t)(tile_addresses[region - 1U] >> 32);
		words[base + 6U] = 0;
		words[base + 7U] = 0;
	}

	/* Succeeded: the words. */
	*count = PARTIAL_SHARED_BUFFER + partial->buffers * PARTIAL_SHARED_BUFFER_WORDS;
	return 0;
}

/*
 * Builds the data sequencer program that copies count shared words from
 * words_address into shared registers 0 up: one DMA.
 */
int
powervr_render_partial_shared(
	uint64_t words_address,
	uint32_t count,
	struct powervr_data_shared_program *program)
{
	struct powervr_data_load load;
	int error;

	/* Refuses a missing result and no word or more than the load's. */
	if (program == NULL || count == 0 || count > POWERVR_RENDER_PARTIAL_WORDS_MAX)
		return EINVAL;

	/* One DMA of the words to shared register 0. */
	load.kind = POWERVR_DATA_LOAD_DMA;
	load.shared = 0;
	load.words = (uint16_t)count;
	load.source = words_address;
	error = powervr_data_shared_build(&load, 1, program);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Builds the background program of the 3D after a partial render: the
 * pixel's position (and with four samples, the run's sample number in
 * bits 16 to 18 of the lookup word after the coordinates); each region
 * sampled there with unnormalised coordinates as raw words, waited for;
 * the registers' words to the pixel output registers under the overlap
 * check; then each tile buffer's words stored to the pixel's words of
 * the buffer (the word's tiled offset under the overlap check added to
 * the buffer's address, the word right after the address pair, the
 * instance's vector mask), each store waited for before the next word
 * takes its temporaries; then the end.  With
 * four samples the program runs at the sample rate
 * (powervr_render_fragment_kick_samples), once for each sample.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_usc.c (pvr_uscgen_spm_load:
 * uses_sample_shading when multisampled, the coordinates the position's
 * x and y, ms_index the sample id; each tile buffer's region sampled
 * with four components, each component stored by dma_st_tiled at the
 * buffer's address plus its tiled offset under savmsk vm; the output
 * registers' region sampled with output_reg_count components and stored
 * to the outputs), pvr_usc.h (pvr_uscgen_spm_buffer_data) and
 * src/imagination/pco/pco_nir_tex.c (the lookup word with sno, the
 * number at bit 16); zedBSD src/drivers/gpu/powervr/compiler/
 * shader-backend.c (the tiled store of a tile buffer word: the data
 * moved right after the address pair, the offset under the overlap
 * check, the address from shared registers through temporaries, the
 * wait after the store) and Mesa 26.2.6 src/imagination/pco/
 * pco_schedule.c (a wdf after a store whose sources are written next).
 */
int
powervr_render_partial_load_usc(
	const struct powervr_render_partial *partial,
	struct powervr_render_usc *program)
{
	struct shader_machine_operation operations[PARTIAL_OPERATIONS];
	struct powervr_usc_group groups[PARTIAL_OPERATIONS];
	size_t sample_at[POWERVR_RENDER_PARTIAL_REGIONS_MAX];
	struct shader_machine_operation *operation;
	unsigned scratch;
	uint32_t region;
	uint32_t buffer;
	uint32_t word;
	size_t length;
	size_t count;
	size_t index;
	int error;

	/* Refuses missing input or result and a plan out of its bounds. */
	if (partial == NULL || program == NULL)
		return EINVAL;
	if ((partial->samples != 1U && partial->samples != 4U) || partial->buffers > POWERVR_RENDER_PARTIAL_REGIONS_MAX - 1U)
		return EINVAL;
	if (partial->words == 0 || partial->words > 4U || (partial->tile_words != 2U && partial->tile_words != 4U))
		return EINVAL;

	/* The temporaries after the regions' samples: the offset, the mask, the address pair, the word, the buffer's address. */
	scratch = PARTIAL_TEMP_REGIONS + 4U * (partial->buffers + 1U);
	count = 0;

	/* The position's x and y. */
	for (word = 0; word < 2U; word++) {
		operation = &operations[count++];
		blank_operation(operation, MACHINE_MOVE_SPECIAL);
		operation->destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_POSITION + word);
		operation->source[0] = machine_register(POWERVR_USC_BANK_SPECIAL, word == 0 ? SPECIAL_X : SPECIAL_Y);
	}

	/* With four samples the lookup word: the sample number times 2^16. */
	if (partial->samples == 4U) {
		operation = &operations[count++];
		blank_operation(operation, MACHINE_MOVE_SPECIAL);
		operation->destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, PARTIAL_TEMP_NUMBER);
		operation->source[0] = machine_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_SAMPLE_NUMBER);
		operation = &operations[count++];
		blank_operation(operation, MACHINE_IMMEDIATE);
		operation->destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, PARTIAL_TEMP_SCALE);
		operation->immediate = 1U << SAMPLE_NUMBER_SHIFT;
		operation = &operations[count++];
		blank_operation(operation, MACHINE_INTEGER);
		operation->destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, PARTIAL_TEMP_LOOKUP);
		operation->source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, PARTIAL_TEMP_NUMBER);
		operation->source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, PARTIAL_TEMP_SCALE);
		operation->source[2] = machine_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_ZERO);
	}

	/* Each region sampled into its four temporaries, then the wait for them all. */
	for (region = 0; region <= partial->buffers; region++) {
		sample_at[region] = count;
		operation = &operations[count++];
		blank_operation(operation, MACHINE_SAMPLE);
		operation->destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, PARTIAL_TEMP_REGIONS + region * 4U);
		if (region == 0) {
			operation->source[0] = machine_register(POWERVR_USC_BANK_SHARED, SHARED_IMAGE);
		} else {
			operation->source[0] = machine_register(POWERVR_USC_BANK_SHARED, PARTIAL_SHARED_BUFFER + (region - 1U) * PARTIAL_SHARED_BUFFER_WORDS);
		}
		operation->source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, TEMP_POSITION);
		operation->source[2] = machine_register(POWERVR_USC_BANK_SHARED, SHARED_SAMPLER);
	}
	operation = &operations[count++];
	blank_operation(operation, MACHINE_WAIT);

	/* The registers' words to the pixel output under the overlap check. */
	for (word = 0; word < partial->words; word++) {
		operation = &operations[count++];
		blank_operation(operation, MACHINE_MOVE);
		operation->destination[0] = machine_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_PIXEL_OUTPUT + word);
		operation->source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, PARTIAL_TEMP_REGIONS + word);
		operation->variant = 1;
	}

	/* The instance's mask, once, for the tile buffers' stores. */
	if (partial->buffers != 0) {
		operation = &operations[count++];
		blank_operation(operation, MACHINE_SAVE_MASK);
		operation->destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, scratch + 1U);
		operation->variant = POWERVR_USC_MASK_VECTOR;
	}

	/* Each tile buffer: its address into temporaries; each word's offset, address, the word, the store. */
	for (buffer = 0; buffer < partial->buffers; buffer++) {
		for (word = 0; word < 2U; word++) {
			operation = &operations[count++];
			blank_operation(operation, MACHINE_MOVE);
			operation->destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, scratch + 5U + word);
			operation->source[0] = machine_register(POWERVR_USC_BANK_SHARED, PARTIAL_SHARED_BUFFER + buffer * PARTIAL_SHARED_BUFFER_WORDS + 4U + word);
		}
		for (word = 0; word < partial->tile_words; word++) {
			operation = &operations[count++];
			blank_operation(operation, MACHINE_MOVE_SPECIAL);
			operation->destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, scratch);
			operation->source[0] = machine_register(POWERVR_USC_BANK_SPECIAL, SPECIAL_TILED_STORE + word);
			operation->variant = 1;
			operation = &operations[count++];
			blank_operation(operation, MACHINE_ADD64);
			operation->destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, scratch + 2U);
			operation->destination[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, scratch + 3U);
			operation->source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, scratch + 5U);
			operation->source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, scratch + 6U);
			operation->source[2] = machine_register(POWERVR_USC_BANK_TEMPORARY, scratch);
			operation = &operations[count++];
			blank_operation(operation, MACHINE_MOVE);
			operation->destination[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, scratch + 4U);
			operation->source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, PARTIAL_TEMP_REGIONS + (buffer + 1U) * 4U + word);
			operation = &operations[count++];
			blank_operation(operation, MACHINE_STORE);
			operation->source[0] = machine_register(POWERVR_USC_BANK_TEMPORARY, scratch + 4U);
			operation->source[1] = machine_register(POWERVR_USC_BANK_TEMPORARY, scratch + 2U);
			operation->source[2] = machine_register(POWERVR_USC_BANK_TEMPORARY, scratch + 1U);
			operation->variant = STORE_TILED;

			/* The wait for the store before the next word takes its temporaries. */
			operation = &operations[count++];
			blank_operation(operation, MACHINE_WAIT);
		}
	}

	/* The end: the last move's group, or after the stores. */
	if (partial->buffers != 0) {
		operation = &operations[count++];
		blank_operation(operation, MACHINE_END);
	}

	/* The groups; each sample's with unnormalised coordinates and raw words, by the sample number with four. */
	for (index = 0; index < count; index++) {
		error = shader_backend_group(&operations[index], 0, &groups[index]);
		if (error != 0)
			return error;
	}
	for (region = 0; region <= partial->buffers; region++) {
		if (partial->samples == 4U) {
			error = powervr_usc_sample_extended(0, 0, SAMPLE_2D, SAMPLE_CHANNELS, 0, 0, 0, 0, 1, 0,
							    &groups[sample_at[region]].operation[POWERVR_USC_PHASE_BACKEND]);
		} else {
			error = powervr_usc_sample(0, 0, SAMPLE_2D, SAMPLE_CHANNELS, 0, &groups[sample_at[region]].operation[POWERVR_USC_PHASE_BACKEND]);
		}
		if (error != 0)
			return error;
	}
	if (partial->buffers == 0)
		groups[count - 1U].end = 1;

	/* Encodes them. */
	error = powervr_usc_program_encode(groups, count, program->code, sizeof(program->code), &length);
	if (error != 0)
		return error;

	/* Succeeded: the program and its temporaries. */
	program->length = length;
	program->temps = scratch + 7U;
	return 0;
}

/* The texel format of a region of words 32-bit words a pixel. */
static uint32_t
partial_format(
	uint32_t words)
{
	/* R32, R32G32 or R32G32B32A32 UINT. */
	if (words == 1U)
		return VK_FORMAT_R32_UINT;
	if (words == 2U)
		return VK_FORMAT_R32G32_UINT;
	return VK_FORMAT_R32G32B32A32_UINT;
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
