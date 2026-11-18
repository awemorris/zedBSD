/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libjpeg-compat's private declarations: the decoder's state behind
 * cinfo->master, the message codes, and the functions the files share.
 *
 * The decoder reads the whole image when jpeg_start_decompress is called:
 * each component's blocks go through the inverse DCT into a plane of
 * samples (padded to whole MCUs; scaled down by 1/2, 1/4 or 1/8 when
 * scale_num / scale_denom asks for it, each block then made smaller by
 * a reduced inverse DCT).  A progressive image keeps each
 * component's coefficients until its last scan, and goes through the
 * inverse DCT then.  jpeg_read_scanlines then upsamples each
 * component's row (libjpeg's "fancy" triangle filter by default) and
 * converts the colour, one row at a time.
 */

#ifndef KERN_LIBJPEG_COMPAT_INTERNAL_H
#define KERN_LIBJPEG_COMPAT_INTERNAL_H

#include <compat/jpeglib.h>

#include <stdint.h>

/* The states of a decompression object (global_state). */
#define JPEG_STATE_START	200
#define JPEG_STATE_READY	202
#define JPEG_STATE_SCANNING	205

/* The most components a frame may have (gray, YCbCr, RGB, CMYK or YCCK). */
#define JPEG_COMPONENTS_MAX	4

/* The markers jpeg_save_markers can keep: APP0 to APP15, then COM. */
#define JPEG_SAVED_KINDS	17

/* The bits of the fast Huffman lookup. */
#define JPEG_FAST_BITS		9

/* The largest image decoded, in samples of all its planes. */
#define JPEG_SAMPLES_MAX	((size_t)1024U * 1024U * 1024U)

/* The messages (their texts are error.c's jpeg_compat_messages, in this order). */
enum jpeg_compat_message {
	JMSG_NOMESSAGE,
	JERR_BAD_LIB_VERSION,
	JERR_BAD_STRUCT_SIZE,
	JERR_BAD_STATE,
	JERR_OUT_OF_MEMORY,
	JERR_NO_SOI,
	JERR_NO_IMAGE,
	JERR_NO_SOURCE,
	JERR_INPUT_EMPTY,
	JERR_BAD_LENGTH,
	JERR_BAD_PRECISION,
	JERR_BAD_SIZE,
	JERR_IMAGE_TOO_BIG,
	JERR_BAD_COMPONENTS,
	JERR_BAD_SAMPLING,
	JERR_BAD_TABLE,
	JERR_NO_TABLE,
	JERR_BAD_SCAN,
	JERR_SOS_NO_SOF,
	JERR_ARITHMETIC,
	JERR_UNSUPPORTED_SOF,
	JERR_CONVERSION,
	JERR_TOO_LITTLE_DATA,
	JERR_UNKNOWN_MARKER,
	JWRN_JPEG_EOF,
	JWRN_EXTRANEOUS_DATA,
	JWRN_HUFF_BAD_CODE,
	JWRN_HIT_MARKER,
	JWRN_MUST_RESYNC,
	JMSG_LASTMSGCODE
};

/*
 * A Huffman table ready for decoding: a lookup of the first
 * JPEG_FAST_BITS bits (the code's length above the value, or 0 for a
 * longer code), and, per length, the largest code and the offset of its
 * values.
 */
struct jpeg_huffman {
	int defined;
	uint16_t fast[1U << JPEG_FAST_BITS];
	int32_t maxcode[18];
	int32_t valoffset[18];
	uint8_t values[256];
};

/*
 * One component's decoded samples: the plane (stride bytes a row,
 * padded to whole MCUs), its rows, and the DC prediction of the scan.
 * Each block of the component is block_size by block_size samples of
 * the plane (8, or 4, 2 or 1 when the output is scaled down: the
 * component's DCT_scaled_size), blocks_across by blocks_down blocks.
 * A progressive image also keeps the coefficients of each block of the
 * plane (64 a block, blocks row by row) and the quantization table the
 * component's first scan found in force (libjpeg latches it there too).
 */
struct jpeg_plane {
	JSAMPLE *samples;
	size_t stride;
	size_t rows;
	int block_size;
	size_t blocks_across;
	size_t blocks_down;
	int prediction;
	int decoded;
	JCOEF *coefficients;
	JQUANT_TBL quant;
	int quant_latched;
};

/*
 * An allocation of a pool, followed by the caller's bytes (aligned for
 * any object).
 */
struct jpeg_pool_block {
	struct jpeg_pool_block *next;
	max_align_t align;
};

/*
 * The decoder's state (cinfo->master): the pools, the tables, the frame
 * and its planes, the scan being read and its bits, and the output rows.
 */
struct jpeg_decomp_master {
	/* The allocations of each pool. */
	struct jpeg_pool_block *pools[JPOOL_NUMPOOLS];

	/* The tables as the file gave them, and the Huffman tables ready for decoding. */
	JQUANT_TBL quant[NUM_QUANT_TBLS];
	JHUFF_TBL dc_tables[NUM_HUFF_TBLS];
	JHUFF_TBL ac_tables[NUM_HUFF_TBLS];
	struct jpeg_huffman dc[NUM_HUFF_TBLS];
	struct jpeg_huffman ac[NUM_HUFF_TBLS];

	/* The frame. */
	int frame_seen;
	jpeg_component_info components[JPEG_COMPONENTS_MAX];
	struct jpeg_plane planes[JPEG_COMPONENTS_MAX];
	JDIMENSION mcus_across;
	JDIMENSION mcus_down;

	/* The scan: its components (indexes into components) and spectral selection. */
	int scan_count;
	int scan_components[MAX_COMPS_IN_SCAN];
	int spectral_start;
	int spectral_end;
	int approximation;

	/*
	 * The entropy-coded bits: the accumulator, how many bits it holds, how
	 * many of those at the bottom are zeros put after a marker, and the
	 * marker met in the data (or 0).  insufficient is set once a code used
	 * those zeros: the data ran out, and the rest of the segment's MCUs are
	 * left at zero (uniform gray), as libjpeg leaves them.
	 */
	uint64_t bits;
	int bit_count;
	int pad_bits;
	int marker;
	int insufficient;
	int eof_warned;

	/* The restart interval's MCUs left, and the number of the next RSTn. */
	unsigned int restarts_left;
	int next_restart;

	/* The blocks a progressive AC scan still has to skip (an end-of-band run). */
	unsigned int eobrun;

	/*
	 * How many bytes of each kind of marker jpeg_save_markers asked to
	 * keep (APP0 to APP15, then COM; 0 keeps none), and the last marker
	 * kept, which the next one is linked after.
	 */
	unsigned int save_limit[JPEG_SAVED_KINDS];
	jpeg_saved_marker_ptr last_saved;

	/* The two bytes read when a source has no more (a fake EOI). */
	JOCTET fake_eoi[2];

	/* The rows of each component upsampled to the output's width, and the output's own colour. */
	JSAMPLE *upsampled[JPEG_COMPONENTS_MAX];

	/* The YCbCr to RGB tables (libjpeg's, 16 fraction bits). */
	int cr_r[256];
	int cb_b[256];
	int32_t cr_g[256];
	int32_t cb_g[256];
};

/* Errors and warnings (error.c). */
void jpeg_compat_fail(j_common_ptr cinfo, int code);
void jpeg_compat_fail_number(j_common_ptr cinfo, int code, int first, int second);
void jpeg_compat_warn(j_common_ptr cinfo, int code);

/* Memory (memory.c). */
void jpeg_compat_memory_init(j_common_ptr cinfo);
void *jpeg_compat_alloc(j_decompress_ptr cinfo, int pool, size_t size);

/* The natural position of each coefficient in zigzag order, with 16 guard entries (marker.c). */
extern const int jpeg_compat_natural_order[DCTSIZE2 + 16];

/* The markers and the bytes of the source (marker.c). */
int jpeg_compat_read_byte(j_decompress_ptr cinfo);
int jpeg_compat_read_markers(j_decompress_ptr cinfo, int in_image);
int jpeg_compat_next_marker(j_decompress_ptr cinfo);

/* Entropy decoding (huffman.c). */
void jpeg_compat_build_huffman(j_decompress_ptr cinfo, const JHUFF_TBL *table, struct jpeg_huffman *huffman);
void jpeg_compat_decode_scan(j_decompress_ptr cinfo);
void jpeg_compat_finish_progressive(j_decompress_ptr cinfo);

/* The inverse DCT (idct.c). */
void jpeg_compat_idct(const JCOEF *coefficients, const UINT16 *quantization, JSAMPLE *out, size_t stride);
void jpeg_compat_idct_reduced(const JCOEF *coefficients, const UINT16 *quantization, int size, JSAMPLE *out, size_t stride);

#endif
