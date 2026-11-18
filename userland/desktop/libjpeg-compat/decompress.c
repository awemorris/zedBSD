/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decompression calls: making and ending the object, the header and
 * its defaults, decoding the image (at jpeg_start_decompress), and the
 * output rows.  The output may be scaled down by 1/2, 1/4 or 1/8
 * (scale_num / scale_denom, chosen as libjpeg chooses): each block is
 * then decoded smaller by the reduced inverse DCT (idct.c), and the
 * chroma of a subsampled image is decoded less reduced where that makes
 * its upsampling a whole ratio smaller, as libjpeg-turbo does.  Each
 * output row takes each component's row upsampled to the output's width
 * (libjpeg's triangle filter, "fancy upsampling", for
 * the 2:1 ratios by default) and converts its colour with libjpeg's
 * fixed-point tables, so that the samples match libjpeg's.  A CMYK image
 * is given as it is, and a YCCK one is made CMYK as libjpeg makes it
 * (neither becomes RGB: libjpeg does not convert them either).
 */

#include "internal.h"

#include <string.h>

/* The fraction bits of the colour tables, and a half in them. */
#define COLOR_SCALEBITS		16
#define COLOR_ONE_HALF		((int32_t)1 << (COLOR_SCALEBITS - 1))

/* libjpeg's colour constants: FIX(x) = x * 2^16, rounded. */
#define FIX_1_40200		((int32_t)91881)
#define FIX_1_77200		((int32_t)116130)
#define FIX_0_71414		((int32_t)46802)
#define FIX_0_34414		((int32_t)22554)
#define FIX_0_29900		((int32_t)19595)
#define FIX_0_58700		((int32_t)38470)
#define FIX_0_11400		((int32_t)7471)

/* How a row's colour is made from the components. */
enum color_conversion {
	COLOR_COPY,
	COLOR_YCC_RGB,
	COLOR_GRAY_RGB,
	COLOR_RGB_RGB,
	COLOR_YCC_GRAY,
	COLOR_RGB_GRAY,
	COLOR_YCCK_CMYK
};

/*
 * Where red, green, blue and the filler (alpha, or X) go in an output
 * pixel, and its size; filler is -1 when there is none.
 */
struct color_layout {
	int red;
	int green;
	int blue;
	int filler;
	int size;
};

static void decompress_defaults(j_decompress_ptr cinfo);
static void decompress_geometry(j_decompress_ptr cinfo);
static void decompress_color_space(j_decompress_ptr cinfo);
static void decompress_scale(j_decompress_ptr cinfo);
static void decompress_component_ratios(j_decompress_ptr cinfo, int index, JDIMENSION *ratio_h, JDIMENSION *ratio_v);
static void decompress_start_image(j_decompress_ptr cinfo);
static enum color_conversion decompress_conversion(j_decompress_ptr cinfo);
static int decompress_layout(J_COLOR_SPACE space, struct color_layout *layout);
static void decompress_tables(struct jpeg_decomp_master *master);
static void decompress_row(j_decompress_ptr cinfo, JDIMENSION y, JSAMPROW out);
static void decompress_upsample(j_decompress_ptr cinfo, int index, JDIMENSION y);
static void decompress_h2v1(const JSAMPLE *in, JDIMENSION width, JSAMPLE *out);
static void decompress_h2v2(const JSAMPLE *near, const JSAMPLE *far, JDIMENSION width, JSAMPLE *out);
static void decompress_convert(j_decompress_ptr cinfo, JSAMPROW out);
static JSAMPLE decompress_clamp(int value);
static void decompress_state(j_decompress_ptr cinfo, int state);

/*
 * Makes a decompression object (jpeg_create_decompress): the program has
 * set cinfo->err; everything else starts cleared.
 */
void
jpeg_CreateDecompress(
	j_decompress_ptr cinfo,
	int version,
	size_t structsize)
{
	struct jpeg_error_mgr *err;
	void *client_data;

	/* The program must be built against this interface and this structure. */
	cinfo->mem = NULL;
	if (version != JPEG_LIB_VERSION)
		jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_BAD_LIB_VERSION, JPEG_LIB_VERSION, version);
	if (structsize != sizeof(*cinfo))
		jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_BAD_STRUCT_SIZE, (int)sizeof(*cinfo), (int)structsize);

	/* Everything cleared but the error manager and the program's own pointer. */
	err = cinfo->err;
	client_data = cinfo->client_data;
	memset(cinfo, 0, sizeof(*cinfo));
	cinfo->err = err;
	cinfo->client_data = client_data;
	cinfo->is_decompressor = TRUE;

	/* The memory manager and the decoder's state. */
	jpeg_compat_memory_init((j_common_ptr)cinfo);
	cinfo->global_state = JPEG_STATE_START;
}

/*
 * Ends a decompression object and frees all it holds.
 */
void
jpeg_destroy_decompress(
	j_decompress_ptr cinfo)
{
	/* The common destruction. */
	jpeg_destroy((j_common_ptr)cinfo);
}

/*
 * Abandons the image in progress (the object can read another).
 */
void
jpeg_abort_decompress(
	j_decompress_ptr cinfo)
{
	/* The common abort. */
	jpeg_abort((j_common_ptr)cinfo);
}

/*
 * Reads the markers up to the first scan: the image's size, components
 * and colour space, and sets the output's defaults.  Returns
 * JPEG_HEADER_OK, or JPEG_HEADER_TABLES_ONLY for a file of tables alone
 * when require_image is FALSE.
 */
int
jpeg_read_header(
	j_decompress_ptr cinfo,
	boolean require_image)
{
	struct jpeg_decomp_master *master;
	int marker;

	/* Only a new object, or one whose last image ended. */
	if (cinfo->global_state != JPEG_STATE_START)
		jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_BAD_STATE, cinfo->global_state, 0);
	if (cinfo->src == NULL)
		jpeg_compat_fail((j_common_ptr)cinfo, JERR_NO_SOURCE);

	/* A clean image: no frame, no marker pending or kept, no JFIF or Adobe facts (the tables stay, as in libjpeg). */
	master = cinfo->master;
	master->frame_seen = 0;
	master->last_saved = NULL;
	cinfo->marker_list = NULL;
	master->marker = 0;
	master->eof_warned = 0;
	cinfo->input_scan_number = 0;
	cinfo->restart_interval = 0;
	cinfo->saw_JFIF_marker = FALSE;
	cinfo->saw_Adobe_marker = FALSE;
	cinfo->Adobe_transform = 0;
	cinfo->err->reset_error_mgr((j_common_ptr)cinfo);
	cinfo->src->init_source(cinfo);

	/* The markers up to SOS or EOI. */
	marker = jpeg_compat_read_markers(cinfo, 0);
	if (marker == JPEG_EOI) {
		if (require_image)
			jpeg_compat_fail((j_common_ptr)cinfo, JERR_NO_IMAGE);
		jpeg_abort((j_common_ptr)cinfo);
		return JPEG_HEADER_TABLES_ONLY;
	}

	/* The image's geometry and the output's defaults. */
	decompress_geometry(cinfo);
	decompress_defaults(cinfo);
	decompress_state(cinfo, JPEG_STATE_READY);
	jpeg_calc_output_dimensions(cinfo);

	/* Succeeded: the header is read. */
	return JPEG_HEADER_OK;
}

/*
 * Sets the output's size and components from the parameters: the image's
 * size scaled by the 1/8, 1/4 or 1/2 that scale_num / scale_denom asks
 * for (the smallest of them not smaller than the scale asked, else the
 * image's size), each side rounded up.
 */
void
jpeg_calc_output_dimensions(
	j_decompress_ptr cinfo)
{
	/* After the header, before the decoding. */
	if (cinfo->global_state != JPEG_STATE_READY)
		jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_BAD_STATE, cinfo->global_state, 0);

	/* The scale, the size it gives, and the components of the colour space asked for. */
	decompress_scale(cinfo);
	decompress_color_space(cinfo);
	cinfo->output_components = cinfo->out_color_components;
	cinfo->rec_outbuf_height = 1;
}

/*
 * Decodes the image: every scan up to EOI, into the component planes.
 * Returns TRUE (a source cannot suspend here).
 */
boolean
jpeg_start_decompress(
	j_decompress_ptr cinfo)
{
	int marker;

	/* After the header. */
	if (cinfo->global_state != JPEG_STATE_READY)
		jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_BAD_STATE, cinfo->global_state, 0);

	/* The output's size and a conversion that exists, then the planes and rows. */
	jpeg_calc_output_dimensions(cinfo);
	decompress_conversion(cinfo);
	decompress_start_image(cinfo);

	/* Each scan, then the markers to the next scan or the end. */
	for (;;) {
		jpeg_compat_decode_scan(cinfo);
		marker = jpeg_compat_read_markers(cinfo, 1);
		if (marker == JPEG_EOI)
			break;
	}

	/* A progressive image's coefficients are complete only now. */
	if (cinfo->progressive_mode)
		jpeg_compat_finish_progressive(cinfo);

	/* Succeeded: the rows can be read. */
	cinfo->output_scanline = 0;
	decompress_state(cinfo, JPEG_STATE_SCANNING);
	return TRUE;
}

/*
 * Writes up to max_lines output rows into the program's rows; returns how
 * many (0 once every row has been read).
 */
JDIMENSION
jpeg_read_scanlines(
	j_decompress_ptr cinfo,
	JSAMPARRAY scanlines,
	JDIMENSION max_lines)
{
	JDIMENSION count;

	/* After jpeg_start_decompress. */
	if (cinfo->global_state != JPEG_STATE_SCANNING)
		jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_BAD_STATE, cinfo->global_state, 0);

	/* Each row, while the image has rows. */
	for (count = 0; count < max_lines && cinfo->output_scanline < cinfo->output_height; count++) {
		decompress_row(cinfo, cinfo->output_scanline, scanlines[count]);
		cinfo->output_scanline++;
	}

	/* Succeeded: the rows written. */
	return count;
}

/*
 * Ends the image once every row has been read: the source is told, the
 * image's memory freed, and the object can read another image.
 */
boolean
jpeg_finish_decompress(
	j_decompress_ptr cinfo)
{
	/* Every row read. */
	if (cinfo->global_state != JPEG_STATE_SCANNING)
		jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_BAD_STATE, cinfo->global_state, 0);
	if (cinfo->output_scanline < cinfo->output_height)
		jpeg_compat_fail((j_common_ptr)cinfo, JERR_TOO_LITTLE_DATA);

	/* The source's end, then the image's memory. */
	cinfo->src->term_source(cinfo);
	jpeg_abort((j_common_ptr)cinfo);

	/* Succeeded: the image is finished. */
	return TRUE;
}

/* Sets libjpeg's defaults of the output parameters for the image read. */
static void
decompress_defaults(
	j_decompress_ptr cinfo)
{
	/* The colour space of the file, and the output's: gray for gray, CMYK for four components, else RGB. */
	cinfo->jpeg_color_space = JCS_YCbCr;
	cinfo->out_color_space = JCS_RGB;
	if (cinfo->num_components == 1) {
		cinfo->jpeg_color_space = JCS_GRAYSCALE;
		cinfo->out_color_space = JCS_GRAYSCALE;
	} else if (cinfo->num_components == 4) {
		/* Adobe's transform 2 is YCCK; anything else is CMYK. */
		cinfo->jpeg_color_space = JCS_CMYK;
		cinfo->out_color_space = JCS_CMYK;
		if (cinfo->saw_Adobe_marker && cinfo->Adobe_transform == 2)
			cinfo->jpeg_color_space = JCS_YCCK;
	} else if (cinfo->saw_JFIF_marker) {
		/* JFIF is YCbCr. */
		cinfo->jpeg_color_space = JCS_YCbCr;
	} else if (cinfo->saw_Adobe_marker) {
		/* Adobe says: transform 0 is RGB, anything else YCbCr here. */
		if (cinfo->Adobe_transform == 0)
			cinfo->jpeg_color_space = JCS_RGB;
	} else if (cinfo->comp_info[0].component_id == 'R' && cinfo->comp_info[1].component_id == 'G' && cinfo->comp_info[2].component_id == 'B') {
		/* Components named R, G and B are RGB. */
		cinfo->jpeg_color_space = JCS_RGB;
	}

	/* The rest as libjpeg sets them (most are not used by this decoder). */
	cinfo->scale_num = 1;
	cinfo->scale_denom = 1;
	cinfo->output_gamma = 1.0;
	cinfo->buffered_image = FALSE;
	cinfo->raw_data_out = FALSE;
	cinfo->dct_method = JDCT_ISLOW;
	cinfo->do_fancy_upsampling = TRUE;
	cinfo->do_block_smoothing = TRUE;
	cinfo->quantize_colors = FALSE;
	cinfo->dither_mode = JDITHER_FS;
	cinfo->two_pass_quantize = TRUE;
	cinfo->desired_number_of_colors = 256;
	cinfo->colormap = NULL;
	cinfo->enable_1pass_quant = FALSE;
	cinfo->enable_external_quant = FALSE;
	cinfo->enable_2pass_quant = FALSE;
}

/*
 * Works out the frame's geometry: the largest sampling factors, each
 * component's size in samples (unscaled; decompress_scale sets them at
 * the output's scale) and blocks, and the MCUs.  A lone component
 * is one block per MCU whatever its factors; the others must divide the
 * largest (libjpeg upsamples by whole ratios only).
 */
static void
decompress_geometry(
	j_decompress_ptr cinfo)
{
	struct jpeg_decomp_master *master;
	jpeg_component_info *component;
	JDIMENSION max_h;
	JDIMENSION max_v;
	JDIMENSION h;
	JDIMENSION v;
	int index;

	/* The largest factors. */
	master = cinfo->master;
	cinfo->max_h_samp_factor = 1;
	cinfo->max_v_samp_factor = 1;
	for (index = 0; index < cinfo->num_components; index++) {
		component = &master->components[index];
		if (component->h_samp_factor > cinfo->max_h_samp_factor)
			cinfo->max_h_samp_factor = component->h_samp_factor;
		if (component->v_samp_factor > cinfo->max_v_samp_factor)
			cinfo->max_v_samp_factor = component->v_samp_factor;
	}

	/* A lone component counts as 1 by 1. */
	max_h = (JDIMENSION)cinfo->max_h_samp_factor;
	max_v = (JDIMENSION)cinfo->max_v_samp_factor;
	if (cinfo->num_components == 1) {
		max_h = 1;
		max_v = 1;
	}

	/* The MCUs across and down. */
	master->mcus_across = (cinfo->image_width + max_h * DCTSIZE - 1U) / (max_h * DCTSIZE);
	master->mcus_down = (cinfo->image_height + max_v * DCTSIZE - 1U) / (max_v * DCTSIZE);
	cinfo->total_iMCU_rows = master->mcus_down;
	cinfo->min_DCT_scaled_size = DCTSIZE;

	/* Each component: its whole ratio, its size in samples and blocks. */
	for (index = 0; index < cinfo->num_components; index++) {
		component = &master->components[index];
		h = (JDIMENSION)component->h_samp_factor;
		v = (JDIMENSION)component->v_samp_factor;
		if (cinfo->num_components == 1) {
			h = 1;
			v = 1;
		}

		/* Its ratios to the largest factors must be whole. */
		if (max_h % h != 0 || max_v % v != 0)
			jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_SAMPLING);
		component->downsampled_width = (JDIMENSION)(((uint64_t)cinfo->image_width * h + max_h - 1U) / max_h);
		component->downsampled_height = (JDIMENSION)(((uint64_t)cinfo->image_height * v + max_v - 1U) / max_v);
		component->width_in_blocks = (component->downsampled_width + DCTSIZE - 1U) / DCTSIZE;
		component->height_in_blocks = (component->downsampled_height + DCTSIZE - 1U) / DCTSIZE;
		component->DCT_scaled_size = DCTSIZE;
	}
}

/*
 * Chooses the output's scale (min_DCT_scaled_size: 8, or 4, 2 or 1 for
 * 1/2, 1/4 or 1/8), the output's size, and each component's block size
 * and size in samples at that scale.  A subsampled component's blocks are
 * made larger (by doubling, up to 8) while its upsampling stays a whole
 * ratio, so that its samples come from the transform rather than from
 * the upsampling (libjpeg-turbo's choice).
 */
static void
decompress_scale(
	j_decompress_ptr cinfo)
{
	jpeg_component_info *component;
	JDIMENSION max_h;
	JDIMENSION max_v;
	JDIMENSION h;
	JDIMENSION v;
	JDIMENSION scaled;
	JDIMENSION size;
	int index;

	/* The scale: the smallest of 1/8, 1/4 and 1/2 that is not below the one asked, else 1. */
	if (cinfo->scale_num * 8U <= cinfo->scale_denom) {
		scaled = 1;
	} else if (cinfo->scale_num * 4U <= cinfo->scale_denom) {
		scaled = 2;
	} else if (cinfo->scale_num * 2U <= cinfo->scale_denom) {
		scaled = 4;
	} else {
		scaled = DCTSIZE;
	}

	/* The output's size, each side rounded up. */
	cinfo->min_DCT_scaled_size = (int)scaled;
	cinfo->output_width = (JDIMENSION)(((uint64_t)cinfo->image_width * scaled + DCTSIZE - 1U) / DCTSIZE);
	cinfo->output_height = (JDIMENSION)(((uint64_t)cinfo->image_height * scaled + DCTSIZE - 1U) / DCTSIZE);

	/* A lone component counts as 1 by 1, as decompress_geometry counts it. */
	max_h = (JDIMENSION)cinfo->max_h_samp_factor;
	max_v = (JDIMENSION)cinfo->max_v_samp_factor;
	if (cinfo->num_components == 1) {
		max_h = 1;
		max_v = 1;
	}

	/* Each component's block size and its size in samples at the scale. */
	for (index = 0; index < cinfo->num_components; index++) {
		component = &cinfo->master->components[index];
		h = (JDIMENSION)component->h_samp_factor;
		v = (JDIMENSION)component->v_samp_factor;
		if (cinfo->num_components == 1) {
			h = 1;
			v = 1;
		}

		/* Doubled while the upsampling across and down stays whole at the doubled size. */
		size = scaled;
		while (size < DCTSIZE) {
			if ((max_h * scaled) % (h * size * 2U) != 0)
				break;
			if ((max_v * scaled) % (v * size * 2U) != 0)
				break;
			size *= 2U;
		}

		/* The block size, and the samples it gives across and down. */
		component->DCT_scaled_size = (int)size;
		component->downsampled_width = (JDIMENSION)(((uint64_t)cinfo->image_width * h * size + max_h * DCTSIZE - 1U) / (max_h * DCTSIZE));
		component->downsampled_height = (JDIMENSION)(((uint64_t)cinfo->image_height * v * size + max_v * DCTSIZE - 1U) / (max_v * DCTSIZE));
	}
}

/* Sets out_color_components for the colour space asked for. */
static void
decompress_color_space(
	j_decompress_ptr cinfo)
{
	struct color_layout layout;
	int known;

	/* Gray is one component, the RGB orders their pixel's size, and a space kept as it is its components. */
	cinfo->out_color_components = cinfo->num_components;
	if (cinfo->out_color_space == JCS_GRAYSCALE) {
		cinfo->out_color_components = 1;
		return;
	}

	/* An RGB order has its pixel's size. */
	known = decompress_layout(cinfo->out_color_space, &layout);
	if (known)
		cinfo->out_color_components = layout.size;
}

/* Allocates the planes (at the centre sample, 128, for a component no scan fills) and the upsampled rows. */
static void
decompress_start_image(
	j_decompress_ptr cinfo)
{
	struct jpeg_decomp_master *master;
	jpeg_component_info *component;
	struct jpeg_plane *plane;
	uint64_t total;
	uint64_t size;
	int index;
	int h;
	int v;

	/* Each plane covers whole MCUs (whole blocks for a lone component), each block at the component's scaled size. */
	master = cinfo->master;
	total = 0;
	for (index = 0; index < cinfo->num_components; index++) {
		component = &master->components[index];
		plane = &master->planes[index];
		h = component->h_samp_factor;
		v = component->v_samp_factor;
		if (cinfo->num_components == 1) {
			h = 1;
			v = 1;
		}

		/* Its blocks and size, and a start without a prediction, samples, coefficients or table. */
		plane->block_size = component->DCT_scaled_size;
		plane->blocks_across = (size_t)master->mcus_across * (size_t)h;
		plane->blocks_down = (size_t)master->mcus_down * (size_t)v;
		plane->stride = plane->blocks_across * (size_t)plane->block_size;
		plane->rows = plane->blocks_down * (size_t)plane->block_size;
		plane->prediction = 0;
		plane->decoded = 0;
		plane->coefficients = NULL;
		plane->quant_latched = 0;

		/* Its bytes: the samples, and a progressive image's coefficients of every block at full size. */
		size = (uint64_t)plane->stride * plane->rows;
		if (cinfo->progressive_mode)
			size += (uint64_t)plane->blocks_across * plane->blocks_down * DCTSIZE2 * sizeof(JCOEF);
		total += size;
		if (total > JPEG_SAMPLES_MAX)
			jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_IMAGE_TOO_BIG, (int)cinfo->image_width, (int)cinfo->image_height);
	}

	/* The samples, and a row per component at the output's width (and a few more for the 2:1 filters). */
	for (index = 0; index < cinfo->num_components; index++) {
		plane = &master->planes[index];
		plane->samples = jpeg_compat_alloc(cinfo, JPOOL_IMAGE, plane->stride * plane->rows);
		memset(plane->samples, CENTERJSAMPLE, plane->stride * plane->rows);
		master->upsampled[index] = jpeg_compat_alloc(cinfo, JPOOL_IMAGE, (size_t)cinfo->output_width + 2U * DCTSIZE * 4U);

		/* A progressive image keeps every coefficient of the plane's blocks, starting at zero. */
		if (cinfo->progressive_mode) {
			size = (uint64_t)plane->blocks_across * plane->blocks_down * DCTSIZE2 * sizeof(JCOEF);
			plane->coefficients = jpeg_compat_alloc(cinfo, JPOOL_IMAGE, (size_t)size);
			memset(plane->coefficients, 0, (size_t)size);
		}
	}

	/* The colour tables. */
	decompress_tables(master);
}

/* Chooses how the colour is converted; a conversion libjpeg-compat does not make is an error. */
static enum color_conversion
decompress_conversion(
	j_decompress_ptr cinfo)
{
	struct color_layout layout;
	J_COLOR_SPACE in;
	J_COLOR_SPACE out;
	int rgb;

	/* To gray: from gray, YCbCr (its Y) or RGB. */
	in = cinfo->jpeg_color_space;
	out = cinfo->out_color_space;
	if (out == JCS_GRAYSCALE) {
		if (in == JCS_GRAYSCALE)
			return COLOR_COPY;
		if (in == JCS_YCbCr)
			return COLOR_YCC_GRAY;
		if (in == JCS_RGB)
			return COLOR_RGB_GRAY;
		jpeg_compat_fail((j_common_ptr)cinfo, JERR_CONVERSION);
	}

	/* To an RGB order: from YCbCr, gray or RGB. */
	rgb = decompress_layout(out, &layout);
	if (rgb) {
		if (in == JCS_YCbCr)
			return COLOR_YCC_RGB;
		if (in == JCS_GRAYSCALE)
			return COLOR_GRAY_RGB;
		if (in == JCS_RGB)
			return COLOR_RGB_RGB;
		jpeg_compat_fail((j_common_ptr)cinfo, JERR_CONVERSION);
	}

	/* CMYK: from CMYK as it is, or from YCCK. */
	if (out == JCS_CMYK) {
		if (in == JCS_CMYK)
			return COLOR_COPY;
		if (in == JCS_YCCK)
			return COLOR_YCCK_CMYK;
		jpeg_compat_fail((j_common_ptr)cinfo, JERR_CONVERSION);
	}

	/* The file's own space, as it is. */
	if (out == in)
		return COLOR_COPY;
	jpeg_compat_fail((j_common_ptr)cinfo, JERR_CONVERSION);
	return COLOR_COPY;
}

/* Tells where the channels of an RGB order go; 0 for a space that is not one. */
static int
decompress_layout(
	J_COLOR_SPACE space,
	struct color_layout *layout)
{
	/* RGB's positions unless the space says otherwise. */
	*layout = (struct color_layout){ 0, 1, 2, -1, 3 };

	/* The order's positions (libjpeg-turbo's JCS_EXT_* spaces; X is filled as alpha is). */
	switch (space) {
	case JCS_RGB:
	case JCS_EXT_RGB:
		*layout = (struct color_layout){ 0, 1, 2, -1, 3 };
		return 1;
	case JCS_EXT_RGBX:
	case JCS_EXT_RGBA:
		*layout = (struct color_layout){ 0, 1, 2, 3, 4 };
		return 1;
	case JCS_EXT_BGR:
		*layout = (struct color_layout){ 2, 1, 0, -1, 3 };
		return 1;
	case JCS_EXT_BGRX:
	case JCS_EXT_BGRA:
		*layout = (struct color_layout){ 2, 1, 0, 3, 4 };
		return 1;
	case JCS_EXT_XBGR:
	case JCS_EXT_ABGR:
		*layout = (struct color_layout){ 3, 2, 1, 0, 4 };
		return 1;
	case JCS_EXT_XRGB:
	case JCS_EXT_ARGB:
		*layout = (struct color_layout){ 1, 2, 3, 0, 4 };
		return 1;
	default:
		break;
	}

	/* Not an RGB order. */
	return 0;
}

/* Builds the YCbCr to RGB tables as libjpeg does (jdcolor.c). */
static void
decompress_tables(
	struct jpeg_decomp_master *master)
{
	int32_t x;
	int index;

	/* Each chroma value, centred. */
	for (index = 0; index <= MAXJSAMPLE; index++) {
		x = (int32_t)index - CENTERJSAMPLE;
		master->cr_r[index] = (int)((FIX_1_40200 * x + COLOR_ONE_HALF) >> COLOR_SCALEBITS);
		master->cb_b[index] = (int)((FIX_1_77200 * x + COLOR_ONE_HALF) >> COLOR_SCALEBITS);
		master->cr_g[index] = -FIX_0_71414 * x;
		master->cb_g[index] = -FIX_0_34414 * x + COLOR_ONE_HALF;
	}
}

/* Makes one output row: each component upsampled, then the colour. */
static void
decompress_row(
	j_decompress_ptr cinfo,
	JDIMENSION y,
	JSAMPROW out)
{
	int index;

	/* The components at the output's width. */
	for (index = 0; index < cinfo->num_components; index++)
		decompress_upsample(cinfo, index, y);

	/* The colour. */
	decompress_convert(cinfo, out);
}

/*
 * Upsamples one component's samples for output row y into its row
 * buffer: 1:1 copied, 2:1 across and 2:1 both ways with libjpeg's fancy
 * filters (when the component is wider than 2), 1:2 down with its
 * vertical filter, and any other whole ratio by replication.  The rows
 * above the first and below the last real row repeat them, as libjpeg's
 * context rows do.
 */
static void
decompress_upsample(
	j_decompress_ptr cinfo,
	int index,
	JDIMENSION y)
{
	struct jpeg_decomp_master *master;
	jpeg_component_info *component;
	struct jpeg_plane *plane;
	const JSAMPLE *near;
	const JSAMPLE *far;
	JSAMPLE *out;
	JDIMENSION ratio_h;
	JDIMENSION ratio_v;
	JDIMENSION width;
	JDIMENSION row;
	JDIMENSION other;
	JDIMENSION x;
	int lower;
	int bias;
	int sum;
	int fancy;

	/* The component's ratios to the output, and whether the filters are used (not at 1/8, as in libjpeg). */
	master = cinfo->master;
	component = &master->components[index];
	plane = &master->planes[index];
	out = master->upsampled[index];
	width = component->downsampled_width;
	decompress_component_ratios(cinfo, index, &ratio_h, &ratio_v);
	fancy = 0;
	if (cinfo->do_fancy_upsampling && cinfo->min_DCT_scaled_size > 1)
		fancy = 1;

	/* The plane's row, and for a 1:2 ratio the neighbour the filter leans to (clamped to the real rows). */
	row = y / ratio_v;
	near = plane->samples + (size_t)row * plane->stride;
	lower = (int)(y % 2U);
	other = row;
	if (ratio_v == 2 && lower && row + 1U < component->downsampled_height)
		other = row + 1U;
	if (ratio_v == 2 && !lower && row > 0)
		other = row - 1U;
	far = plane->samples + (size_t)other * plane->stride;

	/* Decide by the ratios. */
	if (ratio_h == 1 && ratio_v == 1) {
		/* The same size. */
		memcpy(out, near, cinfo->output_width);
	} else if (ratio_h == 2 && ratio_v == 1 && fancy && width > 2U) {
		/* 2:1 across: the triangle filter. */
		decompress_h2v1(near, width, out);
	} else if (ratio_h == 1 && ratio_v == 2 && fancy) {
		/* 1:2 down: 3/4 of the nearer row and 1/4 of the other. */
		bias = 1;
		if (lower)
			bias = 2;
		for (x = 0; x < width; x++) {
			sum = near[x] * 3 + far[x];
			out[x] = (JSAMPLE)((sum + bias) >> 2);
		}
	} else if (ratio_h == 2 && ratio_v == 2 && fancy && width > 2U) {
		/* 2:1 both ways: the triangle filter in both directions. */
		decompress_h2v2(near, far, width, out);
	} else {
		/* Any other whole ratio: each sample repeated. */
		for (x = 0; x < cinfo->output_width; x++)
			out[x] = near[x / ratio_h];
	}
}

/*
 * Works out a component's whole upsampling ratios across and down: the
 * largest factor at the output's block size over its own at its block
 * size (1 for a lone component).
 */
static void
decompress_component_ratios(
	j_decompress_ptr cinfo,
	int index,
	JDIMENSION *ratio_h,
	JDIMENSION *ratio_v)
{
	jpeg_component_info *component;
	JDIMENSION scaled;
	JDIMENSION size;

	/* A lone component is the output's own samples. */
	*ratio_h = 1;
	*ratio_v = 1;
	if (cinfo->num_components == 1)
		return;

	/* The ratios, whole by decompress_geometry's check and decompress_scale's choice. */
	component = &cinfo->master->components[index];
	scaled = (JDIMENSION)cinfo->min_DCT_scaled_size;
	size = (JDIMENSION)component->DCT_scaled_size;
	*ratio_h = ((JDIMENSION)cinfo->max_h_samp_factor * scaled) / ((JDIMENSION)component->h_samp_factor * size);
	*ratio_v = ((JDIMENSION)cinfo->max_v_samp_factor * scaled) / ((JDIMENSION)component->v_samp_factor * size);
}

/* libjpeg's h2v1 fancy upsampling of one row (jdsample.c): 3/4 of the nearer sample and 1/4 of the further. */
static void
decompress_h2v1(
	const JSAMPLE *in,
	JDIMENSION width,
	JSAMPLE *out)
{
	JDIMENSION column;
	int value;

	/* The first column. */
	out[0] = in[0];
	out[1] = (JSAMPLE)((in[0] * 3 + in[1] + 2) >> 2);

	/* The columns between. */
	for (column = 1; column + 1U < width; column++) {
		value = in[column] * 3;
		out[column * 2U] = (JSAMPLE)((value + in[column - 1U] + 1) >> 2);
		out[column * 2U + 1U] = (JSAMPLE)((value + in[column + 1U] + 2) >> 2);
	}

	/* The last column. */
	out[column * 2U] = (JSAMPLE)((in[column] * 3 + in[column - 1U] + 1) >> 2);
	out[column * 2U + 1U] = in[column];
}

/*
 * libjpeg's h2v2 fancy upsampling of one output row (jdsample.c): the
 * column sums of 3/4 of the nearer input row and 1/4 of the other, then
 * the same across, with libjpeg's alternating rounding.
 */
static void
decompress_h2v2(
	const JSAMPLE *near,
	const JSAMPLE *far,
	JDIMENSION width,
	JSAMPLE *out)
{
	JDIMENSION column;
	int this_sum;
	int last_sum;
	int next_sum;

	/* The first column. */
	this_sum = near[0] * 3 + far[0];
	next_sum = near[1] * 3 + far[1];
	out[0] = (JSAMPLE)((this_sum * 4 + 8) >> 4);
	out[1] = (JSAMPLE)((this_sum * 3 + next_sum + 7) >> 4);
	last_sum = this_sum;
	this_sum = next_sum;

	/* The columns between. */
	for (column = 1; column + 1U < width; column++) {
		next_sum = near[column + 1U] * 3 + far[column + 1U];
		out[column * 2U] = (JSAMPLE)((this_sum * 3 + last_sum + 8) >> 4);
		out[column * 2U + 1U] = (JSAMPLE)((this_sum * 3 + next_sum + 7) >> 4);
		last_sum = this_sum;
		this_sum = next_sum;
	}

	/* The last column. */
	out[column * 2U] = (JSAMPLE)((this_sum * 3 + last_sum + 8) >> 4);
	out[column * 2U + 1U] = (JSAMPLE)((this_sum * 4 + 7) >> 4);
}

/* Converts the upsampled components of a row into the output's colour and order. */
static void
decompress_convert(
	j_decompress_ptr cinfo,
	JSAMPROW out)
{
	struct jpeg_decomp_master *master;
	struct color_layout layout;
	enum color_conversion conversion;
	JSAMPLE *pixel;
	JDIMENSION x;
	int luma;
	int red;
	int green;
	int blue;
	int cb;
	int cr;
	int index;

	/* The conversion and the output's order. */
	master = cinfo->master;
	conversion = decompress_conversion(cinfo);
	decompress_layout(cinfo->out_color_space, &layout);

	/* The components interleaved as they are. */
	if (conversion == COLOR_COPY) {
		for (x = 0; x < cinfo->output_width; x++) {
			for (index = 0; index < cinfo->num_components; index++)
				out[(size_t)x * (size_t)cinfo->num_components + (size_t)index] = master->upsampled[index][x];
		}

		/* Succeeded: the row is the file's components. */
		return;
	}

	/* Gray from Y, or from RGB with libjpeg's weights. */
	if (conversion == COLOR_YCC_GRAY) {
		memcpy(out, master->upsampled[0], cinfo->output_width);
		return;
	}

	/* Gray from RGB, weighted. */
	if (conversion == COLOR_RGB_GRAY) {
		for (x = 0; x < cinfo->output_width; x++) {
			luma = (int)((FIX_0_29900 * master->upsampled[0][x] + FIX_0_58700 * master->upsampled[1][x] + FIX_0_11400 * master->upsampled[2][x] + COLOR_ONE_HALF) >> COLOR_SCALEBITS);
			out[x] = (JSAMPLE)luma;
		}

		/* Succeeded: the row is gray. */
		return;
	}

	/* YCCK to CMYK (libjpeg's ycck_cmyk_convert): the YCbCr part made RGB and inverted, K as it is. */
	if (conversion == COLOR_YCCK_CMYK) {
		for (x = 0; x < cinfo->output_width; x++) {
			luma = master->upsampled[0][x];
			cb = master->upsampled[1][x];
			cr = master->upsampled[2][x];
			pixel = out + (size_t)x * 4U;
			pixel[0] = decompress_clamp(MAXJSAMPLE - (luma + master->cr_r[cr]));
			pixel[1] = decompress_clamp(MAXJSAMPLE - (luma + (int)((master->cb_g[cb] + master->cr_g[cr]) >> COLOR_SCALEBITS)));
			pixel[2] = decompress_clamp(MAXJSAMPLE - (luma + master->cb_b[cb]));
			pixel[3] = master->upsampled[3][x];
		}

		/* Succeeded: the row is CMYK. */
		return;
	}

	/* An RGB order: each pixel's channels, and the filler opaque. */
	for (x = 0; x < cinfo->output_width; x++) {
		luma = master->upsampled[0][x];
		red = luma;
		green = luma;
		blue = luma;
		if (conversion == COLOR_YCC_RGB) {
			/* libjpeg's YCbCr to RGB. */
			cb = master->upsampled[1][x];
			cr = master->upsampled[2][x];
			red = luma + master->cr_r[cr];
			green = luma + (int)((master->cb_g[cb] + master->cr_g[cr]) >> COLOR_SCALEBITS);
			blue = luma + master->cb_b[cb];
		} else if (conversion == COLOR_RGB_RGB) {
			/* The file's RGB. */
			green = master->upsampled[1][x];
			blue = master->upsampled[2][x];
		}

		/* The channels in the output's order. */
		pixel = out + (size_t)x * (size_t)layout.size;
		pixel[layout.red] = decompress_clamp(red);
		pixel[layout.green] = decompress_clamp(green);
		pixel[layout.blue] = decompress_clamp(blue);
		if (layout.filler >= 0)
			pixel[layout.filler] = MAXJSAMPLE;
	}
}

/* Clamps a value to a sample. */
static JSAMPLE
decompress_clamp(
	int value)
{
	/* Below 0 and above 255 saturate. */
	if (value < 0)
		return 0;
	if (value > MAXJSAMPLE)
		return MAXJSAMPLE;

	/* A value in range. */
	return (JSAMPLE)value;
}

/* Moves the object to a state (the calls check it). */
static void
decompress_state(
	j_decompress_ptr cinfo,
	int state)
{
	/* The new state. */
	cinfo->global_state = state;
}
