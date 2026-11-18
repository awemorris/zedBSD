/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libjpeg-compat: zedBSD's own implementation of the IJG libjpeg
 * decompression interface for its base programs (plan/ws074/design.md
 * §10, following plan/ws035/ws.md D1 to D6).  It holds no libjpeg code;
 * the names and values are libjpeg's (libjpeg-turbo's for the JCS_EXT_*
 * colour spaces), and JPEG_LIB_VERSION is 62 like libjpeg-turbo's default
 * build.  The structures are written here and are not laid out like the
 * real ones: only programs built against this header use the library
 * (its SONAME is libjpeg-compat.so).  Programs of the packages use the
 * real libjpeg (/usr/include/jpeglib.h); base programs include
 * <compat/jpeglib.h>.
 *
 * It decodes baseline, extended sequential and progressive Huffman JPEG
 * (8-bit samples) with any sampling factors and restart intervals:
 * grayscale, YCbCr and RGB images into gray, RGB and the JCS_EXT_*
 * orders, and CMYK and YCCK images into CMYK (ws074-p019, p020).  The
 * APPn and COM markers can be kept (jpeg_save_markers).  Scaling,
 * buffered-image mode, raw data, arithmetic coding, 12-bit samples and
 * compression are not provided.
 *
 * As in libjpeg, an error calls err->error_exit, which must not return
 * (the caller longjmps out of it); the default one prints the message and
 * exits.
 */

#ifndef KERN_COMPAT_JPEGLIB_H
#define KERN_COMPAT_JPEGLIB_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The interface's version (D1): libjpeg 6b's, as libjpeg-turbo builds it by default. */
#define JPEG_LIB_VERSION		62
#define JPEG_LIB_VERSION_MAJOR		6
#define JPEG_LIB_VERSION_MINOR		2

/* libjpeg-turbo's extended and alpha colour spaces are provided. */
#define JCS_EXTENSIONS			1
#define JCS_ALPHA_EXTENSIONS		1

/* The sizes libjpeg names. */
#define DCTSIZE				8
#define DCTSIZE2			64
#define NUM_QUANT_TBLS			4
#define NUM_HUFF_TBLS			4
#define MAX_COMPS_IN_SCAN		4
#define MAX_COMPONENTS			10
#define BITS_IN_JSAMPLE			8
#define MAXJSAMPLE			255
#define CENTERJSAMPLE			128
#define JPEG_MAX_DIMENSION		65500L

/* The integer and sample types. */
#ifndef HAVE_BOOLEAN
typedef int boolean;
#endif
#ifndef FALSE
#define FALSE				0
#endif
#ifndef TRUE
#define TRUE				1
#endif
typedef unsigned char JSAMPLE;
typedef JSAMPLE *JSAMPROW;
typedef JSAMPROW *JSAMPARRAY;
typedef JSAMPARRAY *JSAMPIMAGE;
typedef short JCOEF;
typedef JCOEF JBLOCK[DCTSIZE2];
typedef JBLOCK *JBLOCKROW;
typedef JBLOCKROW *JBLOCKARRAY;
typedef unsigned char JOCTET;
typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef short INT16;
typedef unsigned int JDIMENSION;

/* The colour spaces (libjpeg-turbo's values). */
typedef enum {
	JCS_UNKNOWN,
	JCS_GRAYSCALE,
	JCS_RGB,
	JCS_YCbCr,
	JCS_CMYK,
	JCS_YCCK,
	JCS_EXT_RGB,
	JCS_EXT_RGBX,
	JCS_EXT_BGR,
	JCS_EXT_BGRX,
	JCS_EXT_XBGR,
	JCS_EXT_XRGB,
	JCS_EXT_RGBA,
	JCS_EXT_BGRA,
	JCS_EXT_ABGR,
	JCS_EXT_ARGB,
	JCS_RGB565
} J_COLOR_SPACE;

/* The inverse DCTs a program may ask for (every one is decoded with the accurate integer IDCT). */
typedef enum {
	JDCT_ISLOW,
	JDCT_IFAST,
	JDCT_FLOAT
} J_DCT_METHOD;
#define JDCT_DEFAULT			JDCT_ISLOW
#define JDCT_FASTEST			JDCT_IFAST

/* The dithering of colour quantization (quantization is not provided). */
typedef enum {
	JDITHER_NONE,
	JDITHER_ORDERED,
	JDITHER_FS
} J_DITHER_MODE;

/* What jpeg_read_header reports. */
#define JPEG_SUSPENDED			0
#define JPEG_HEADER_OK			1
#define JPEG_HEADER_TABLES_ONLY		2

/* The memory pools of the memory manager. */
#define JPOOL_PERMANENT			0
#define JPOOL_IMAGE			1
#define JPOOL_NUMPOOLS			2

/* The markers jpeg_save_markers names. */
#define JPEG_RST0			0xD0
#define JPEG_EOI			0xD9
#define JPEG_APP0			0xE0
#define JPEG_COM			0xFE

/* The longest formatted message, and the longest string parameter of one. */
#define JMSG_LENGTH_MAX			200
#define JMSG_STR_PARM_MAX		80

/* A quantization table in zigzag order, and a Huffman table as the file gives it. */
typedef struct {
	UINT16 quantval[DCTSIZE2];
	boolean sent_table;
} JQUANT_TBL;
typedef struct {
	UINT8 bits[17];
	UINT8 huffval[256];
	boolean sent_table;
} JHUFF_TBL;

/*
 * One component of the frame: its identity and sampling, its tables, and
 * its size in blocks and in samples.
 */
typedef struct {
	int component_id;
	int component_index;
	int h_samp_factor;
	int v_samp_factor;
	int quant_tbl_no;
	int dc_tbl_no;
	int ac_tbl_no;
	JDIMENSION width_in_blocks;
	JDIMENSION height_in_blocks;
	int DCT_scaled_size;
	JDIMENSION downsampled_width;
	JDIMENSION downsampled_height;
	boolean component_needed;
	JQUANT_TBL *quant_table;
} jpeg_component_info;

/* A saved marker (jpeg_save_markers): its code, its whole length, and the bytes kept of it. */
typedef struct jpeg_marker_struct *jpeg_saved_marker_ptr;
struct jpeg_marker_struct {
	jpeg_saved_marker_ptr next;
	UINT8 marker;
	unsigned int original_length;
	unsigned int data_length;
	JOCTET *data;
};

/* The objects, declared here for the pointers below. */
struct jpeg_common_struct;
struct jpeg_decompress_struct;
typedef struct jpeg_common_struct *j_common_ptr;
typedef struct jpeg_decompress_struct *j_decompress_ptr;

/*
 * The error manager: the program may replace error_exit (which must not
 * return), emit_message and output_message.  msg_code and msg_parm hold
 * the last message; format_message writes it.
 */
struct jpeg_error_mgr {
	void (*error_exit)(j_common_ptr cinfo);
	void (*emit_message)(j_common_ptr cinfo, int msg_level);
	void (*output_message)(j_common_ptr cinfo);
	void (*format_message)(j_common_ptr cinfo, char *buffer);
	void (*reset_error_mgr)(j_common_ptr cinfo);
	int msg_code;
	union {
		int i[8];
		char s[JMSG_STR_PARM_MAX];
	} msg_parm;
	int trace_level;
	long num_warnings;
	const char *const *jpeg_message_table;
	int last_jpeg_message;
	const char *const *addon_message_table;
	int first_addon_message;
	int last_addon_message;
};

/* The progress monitor (kept for programs that set one; never called). */
struct jpeg_progress_mgr {
	void (*progress_monitor)(j_common_ptr cinfo);
	long pass_counter;
	long pass_limit;
	int completed_passes;
	int total_passes;
};

/*
 * The data source: the bytes in hand, and how to get more.  A program
 * may supply its own (fill_input_buffer returning FALSE, suspension, is
 * treated as the end of the data).
 */
struct jpeg_source_mgr {
	const JOCTET *next_input_byte;
	size_t bytes_in_buffer;
	void (*init_source)(j_decompress_ptr cinfo);
	boolean (*fill_input_buffer)(j_decompress_ptr cinfo);
	void (*skip_input_data)(j_decompress_ptr cinfo, long num_bytes);
	boolean (*resync_to_restart)(j_decompress_ptr cinfo, int desired);
	void (*term_source)(j_decompress_ptr cinfo);
};

/*
 * The memory manager: allocations in a pool, freed together (JPOOL_IMAGE
 * at the end of an image, JPOOL_PERMANENT when the object is destroyed).
 * An allocation that fails is an error.
 */
struct jpeg_memory_mgr {
	void *(*alloc_small)(j_common_ptr cinfo, int pool_id, size_t sizeofobject);
	void *(*alloc_large)(j_common_ptr cinfo, int pool_id, size_t sizeofobject);
	JSAMPARRAY (*alloc_sarray)(j_common_ptr cinfo, int pool_id, JDIMENSION samplesperrow, JDIMENSION numrows);
	JBLOCKARRAY (*alloc_barray)(j_common_ptr cinfo, int pool_id, JDIMENSION blocksperrow, JDIMENSION numrows);
	void (*free_pool)(j_common_ptr cinfo, int pool_id);
	void (*self_destruct)(j_common_ptr cinfo);
	long max_memory_to_use;
	long max_alloc_chunk;
};

/* The fields every libjpeg object starts with. */
#define jpeg_common_fields \
	struct jpeg_error_mgr *err; \
	struct jpeg_memory_mgr *mem; \
	struct jpeg_progress_mgr *progress; \
	void *client_data; \
	boolean is_decompressor; \
	int global_state

/* The part of a compression or decompression object both share. */
struct jpeg_common_struct {
	jpeg_common_fields;
};

/* The decoder's own state (private to the library). */
struct jpeg_decomp_master;

/*
 * A decompression object.  jpeg_read_header fills the image's
 * description and the defaults of the output; the program may change the
 * output parameters before jpeg_start_decompress, which fills the output's
 * size.
 */
struct jpeg_decompress_struct {
	jpeg_common_fields;

	/* The source of the data. */
	struct jpeg_source_mgr *src;

	/* The image, from the file. */
	JDIMENSION image_width;
	JDIMENSION image_height;
	int num_components;
	J_COLOR_SPACE jpeg_color_space;

	/* The output the program asks for. */
	J_COLOR_SPACE out_color_space;
	unsigned int scale_num;
	unsigned int scale_denom;
	double output_gamma;
	boolean buffered_image;
	boolean raw_data_out;
	J_DCT_METHOD dct_method;
	boolean do_fancy_upsampling;
	boolean do_block_smoothing;
	boolean quantize_colors;
	J_DITHER_MODE dither_mode;
	boolean two_pass_quantize;
	int desired_number_of_colors;
	boolean enable_1pass_quant;
	boolean enable_external_quant;
	boolean enable_2pass_quant;

	/* The output's size and layout. */
	JDIMENSION output_width;
	JDIMENSION output_height;
	int out_color_components;
	int output_components;
	int rec_outbuf_height;
	int actual_number_of_colors;
	JSAMPARRAY colormap;

	/* The next output row. */
	JDIMENSION output_scanline;

	/* The progress of the input. */
	int input_scan_number;
	JDIMENSION input_iMCU_row;
	int output_scan_number;
	JDIMENSION output_iMCU_row;

	/* The tables, as the file gave them. */
	JQUANT_TBL *quant_tbl_ptrs[NUM_QUANT_TBLS];
	JHUFF_TBL *dc_huff_tbl_ptrs[NUM_HUFF_TBLS];
	JHUFF_TBL *ac_huff_tbl_ptrs[NUM_HUFF_TBLS];

	/* The frame. */
	int data_precision;
	jpeg_component_info *comp_info;
	boolean progressive_mode;
	boolean arith_code;
	unsigned int restart_interval;

	/* What the APP0 (JFIF) and APP14 (Adobe) markers said. */
	boolean saw_JFIF_marker;
	UINT8 JFIF_major_version;
	UINT8 JFIF_minor_version;
	UINT8 density_unit;
	UINT16 X_density;
	UINT16 Y_density;
	boolean saw_Adobe_marker;
	UINT8 Adobe_transform;
	boolean CCIR601_sampling;

	/* The markers saved by jpeg_save_markers, in the file's order (in the image's pool). */
	jpeg_saved_marker_ptr marker_list;

	/* The frame's sampling and its MCUs. */
	int max_h_samp_factor;
	int max_v_samp_factor;
	int min_DCT_scaled_size;
	JDIMENSION total_iMCU_rows;

	/* The decoder's own state. */
	struct jpeg_decomp_master *master;
};

/* Makes the standard error manager. */
struct jpeg_error_mgr *jpeg_std_error(struct jpeg_error_mgr *err);

/* Makes and ends a decompression object. */
void jpeg_CreateDecompress(j_decompress_ptr cinfo, int version, size_t structsize);
#define jpeg_create_decompress(cinfo) \
	jpeg_CreateDecompress((cinfo), JPEG_LIB_VERSION, (size_t)sizeof(struct jpeg_decompress_struct))
void jpeg_destroy_decompress(j_decompress_ptr cinfo);
void jpeg_abort_decompress(j_decompress_ptr cinfo);
void jpeg_destroy(j_common_ptr cinfo);
void jpeg_abort(j_common_ptr cinfo);

/* The standard sources: a stdio stream, or bytes in memory. */
void jpeg_stdio_src(j_decompress_ptr cinfo, FILE *infile);
void jpeg_mem_src(j_decompress_ptr cinfo, const unsigned char *inbuffer, unsigned long insize);

/* Decompression. */
int jpeg_read_header(j_decompress_ptr cinfo, boolean require_image);
void jpeg_calc_output_dimensions(j_decompress_ptr cinfo);
boolean jpeg_start_decompress(j_decompress_ptr cinfo);
JDIMENSION jpeg_read_scanlines(j_decompress_ptr cinfo, JSAMPARRAY scanlines, JDIMENSION max_lines);
boolean jpeg_finish_decompress(j_decompress_ptr cinfo);

/* Keeps up to length_limit bytes of each APPn or COM marker of a kind in marker_list (0 keeps none). */
void jpeg_save_markers(j_decompress_ptr cinfo, int marker_code, unsigned int length_limit);

/* The default recovery after a missing restart marker (for a program's own source). */
boolean jpeg_resync_to_restart(j_decompress_ptr cinfo, int desired);

#ifdef __cplusplus
}
#endif

#endif
