/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pieces the camera tools share: completing an MJPEG frame that lacks
 * its Huffman tables, turning YUYV into RGB, writing a PPM image, and
 * asking a /dev/camN node what it is and what it can deliver.
 *
 * A USB camera commonly sends MJPEG frames without a DHT segment and
 * leaves the decoder to assume the tables of the JPEG specification;
 * a decoder that insists on the tables is given them before the frame's
 * SOF segment.
 */

#include "userland/tests/camcommon/camcommon.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

/* The JPEG markers the frame scan recognizes. */
#define CAMCOMMON_JPEG_PREFIX		0xffU
#define CAMCOMMON_JPEG_SOI		0xd8U
#define CAMCOMMON_JPEG_EOI		0xd9U
#define CAMCOMMON_JPEG_SOS		0xdaU
#define CAMCOMMON_JPEG_DHT		0xc4U
#define CAMCOMMON_JPEG_TEM		0x01U
#define CAMCOMMON_JPEG_RST0		0xd0U
#define CAMCOMMON_JPEG_RST7		0xd7U
#define CAMCOMMON_JPEG_SOF_FIRST	0xc0U
#define CAMCOMMON_JPEG_SOF_LAST		0xcfU
#define CAMCOMMON_JPEG_JPG		0xc8U
#define CAMCOMMON_JPEG_DAC		0xccU

/* The number of code lengths a Huffman table lists, 1 to 16 bits. */
#define CAMCOMMON_HUFFMAN_LENGTHS	16U

/* The number of standard Huffman tables inserted into a frame. */
#define CAMCOMMON_HUFFMAN_TABLES	4U

/* The fixed-point scale of the YUV coefficients, and half of it for rounding. */
#define CAMCOMMON_FIXED_ONE		65536
#define CAMCOMMON_FIXED_HALF		32768

/* The coefficient denominator of the luma weights below (parts per 10000). */
#define CAMCOMMON_WEIGHT_ONE		10000

/*
 * One Huffman table of a DHT segment.
 *
 * An instance names a table class and destination, the number of codes of
 * each length and the symbols in code order; it points at constant data
 * that lives as long as the program.
 */
struct camcommon_huffman_table {
	uint8_t class_and_destination;
	const uint8_t *code_counts;
	const uint8_t *symbols;
	size_t symbol_count;
};

/*
 * Values referenced: ITU-T T.81 (JPEG) Annex K.3, the typical Huffman
 * tables (Tables K.3 to K.6).
 *
 * Each pair below is one table: the number of codes of each length from
 * 1 to 16 bits (BITS), then the symbols in order of increasing code
 * (HUFFVAL).  The DC tables have 12 symbols each, the AC tables 162.
 */

/* The code counts of the DC luminance table (Table K.3). */
static const uint8_t camcommon_dc_luminance_counts[CAMCOMMON_HUFFMAN_LENGTHS] = {
	0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* The symbols of the DC luminance table (Table K.3). */
static const uint8_t camcommon_dc_luminance_symbols[12] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b
};

/* The code counts of the DC chrominance table (Table K.4). */
static const uint8_t camcommon_dc_chrominance_counts[CAMCOMMON_HUFFMAN_LENGTHS] = {
	0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* The symbols of the DC chrominance table (Table K.4). */
static const uint8_t camcommon_dc_chrominance_symbols[12] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b
};

/* The code counts of the AC luminance table (Table K.5). */
static const uint8_t camcommon_ac_luminance_counts[CAMCOMMON_HUFFMAN_LENGTHS] = {
	0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03,
	0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7d
};

/* The symbols of the AC luminance table (Table K.5). */
static const uint8_t camcommon_ac_luminance_symbols[162] = {
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
	0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
	0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
	0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
	0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
	0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
	0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
	0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
	0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
	0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
	0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
	0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
	0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
	0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
	0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
	0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
	0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
	0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
	0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa
};

/* The code counts of the AC chrominance table (Table K.6). */
static const uint8_t camcommon_ac_chrominance_counts[CAMCOMMON_HUFFMAN_LENGTHS] = {
	0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04,
	0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77
};

/* The symbols of the AC chrominance table (Table K.6). */
static const uint8_t camcommon_ac_chrominance_symbols[162] = {
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
	0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
	0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
	0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
	0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
	0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
	0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
	0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
	0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
	0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
	0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
	0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
	0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
	0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
	0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
	0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
	0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
	0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
	0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa
};

/*
 * The four standard tables in the order they are written into the DHT
 * segment: DC luminance (class 0, destination 0), DC chrominance (class 0,
 * destination 1), AC luminance (class 1, destination 0) and AC chrominance
 * (class 1, destination 1), the assignment a baseline decoder assumes when
 * a frame omits its tables.
 */
static const struct camcommon_huffman_table camcommon_standard_tables[CAMCOMMON_HUFFMAN_TABLES] = {
	{ 0x00, camcommon_dc_luminance_counts, camcommon_dc_luminance_symbols, sizeof(camcommon_dc_luminance_symbols) },
	{ 0x01, camcommon_dc_chrominance_counts, camcommon_dc_chrominance_symbols, sizeof(camcommon_dc_chrominance_symbols) },
	{ 0x10, camcommon_ac_luminance_counts, camcommon_ac_luminance_symbols, sizeof(camcommon_ac_luminance_symbols) },
	{ 0x11, camcommon_ac_chrominance_counts, camcommon_ac_chrominance_symbols, sizeof(camcommon_ac_chrominance_symbols) }
};

static int camcommon_mjpeg_scan(const uint8_t *frame, size_t length, size_t *sof_offset, int *has_tables);
static int camcommon_jpeg_is_sof(unsigned marker);
static int camcommon_jpeg_is_standalone(unsigned marker);
static void camcommon_mjpeg_write_dht(uint8_t *output);
static int64_t camcommon_fixed_ratio(int64_t numerator, int64_t denominator);
static uint8_t camcommon_clamp(int64_t value);
static void camcommon_print_interval(uint32_t interval_100ns);
static int camcommon_print_mode(int descriptor, uint32_t format, uint32_t mode_index);
static void camcommon_record_init(void *record, size_t size);
static int camcommon_errno(void);

/*
 * Reports how many bytes the standard Huffman table segment adds to a frame.
 *
 * The segment is the two marker bytes, the two length bytes, and for each
 * table one class byte, sixteen code counts and its symbols.
 */
size_t
camcommon_mjpeg_dht_size(void)
{
	size_t size;
	unsigned index;

	/* Counts the marker and the length field. */
	size = 4;

	/* Adds the header and the symbols of every standard table. */
	for (index = 0; index < CAMCOMMON_HUFFMAN_TABLES; index++)
		size += 1 + CAMCOMMON_HUFFMAN_LENGTHS + camcommon_standard_tables[index].symbol_count;

	/* Reports the size of the whole segment. */
	return size;
}

/*
 * Copies an MJPEG frame, inserting the standard Huffman tables when it has none.
 *
 * The frame runs from SOI to EOI.  When no DHT segment precedes its first
 * scan, the standard tables are written just before the SOF segment and
 * *inserted is set; otherwise the frame is copied unchanged.  The output
 * needs room for the frame plus camcommon_mjpeg_dht_size() bytes.  EINVAL
 * tells that the frame has no SOI, no SOF or a broken segment before its
 * scan; ENOSPC that the output is too small.
 */
int
camcommon_mjpeg_complete(
	const uint8_t *frame,
	size_t length,
	uint8_t *output,
	size_t capacity,
	size_t *output_length,
	int *inserted)
{
	size_t sof_offset;
	size_t table_size;
	int has_tables;
	int error;

	/* Finds where the frame's SOF segment starts and whether it has tables. */
	error = camcommon_mjpeg_scan(frame, length, &sof_offset, &has_tables);
	if (error != 0)
		return error;

	/* A frame with its own tables is copied as it is. */
	if (has_tables) {
		if (capacity < length)
			return ENOSPC;
		memcpy(output, frame, length);
		*output_length = length;
		*inserted = 0;
		return 0;
	}

	/* Refuses an output that cannot hold the frame and the tables. */
	table_size = camcommon_mjpeg_dht_size();
	if (capacity < length + table_size)
		return ENOSPC;

	/* Copies the segments before SOF, the standard tables, then the rest. */
	memcpy(output, frame, sof_offset);
	camcommon_mjpeg_write_dht(output + sof_offset);
	memcpy(output + sof_offset + table_size, frame + sof_offset, length - sof_offset);

	/* Publishes the completed frame's length and that tables were added. */
	*output_length = length + table_size;
	*inserted = 1;

	/* Succeeded: the output is a frame a strict decoder accepts. */
	return 0;
}

/*
 * Fills the YUV to RGB coefficients of one matrix and one range.
 *
 * With Kr and Kb the matrix's red and blue luma weights and Kg the rest,
 * R = Y' + 2(1-Kr)Cr, B = Y' + 2(1-Kb)Cb and
 * G = Y' - 2Kb(1-Kb)/Kg Cb - 2Kr(1-Kr)/Kg Cr; the limited range first
 * stretches luma from 16..235 and chroma from 16..240 to the full 8 bits.
 * EINVAL tells an unknown matrix or range.
 */
int
camcommon_yuv_coefficients_init(
	struct camcommon_yuv_coefficients *coefficients,
	uint32_t matrix,
	uint32_t range)
{
	int64_t red_weight;
	int64_t blue_weight;
	int64_t green_weight;
	int64_t chroma_numerator;
	int64_t chroma_denominator;

	/* Chooses the luma weights of the matrix, in parts per 10000. */
	if (matrix == CAMCOMMON_MATRIX_BT601) {
		red_weight = 2990;
		blue_weight = 1140;
	} else if (matrix == CAMCOMMON_MATRIX_BT709) {
		red_weight = 2126;
		blue_weight = 722;
	} else {
		return EINVAL;
	}

	/* Chooses how far luma and chroma are stretched for the range. */
	if (range == CAMCOMMON_RANGE_LIMITED) {
		coefficients->y_offset = 16;
		coefficients->y_scale = camcommon_fixed_ratio(255, 219);
		chroma_numerator = 255;
		chroma_denominator = 224;
	} else if (range == CAMCOMMON_RANGE_FULL) {
		coefficients->y_offset = 0;
		coefficients->y_scale = CAMCOMMON_FIXED_ONE;
		chroma_numerator = 1;
		chroma_denominator = 1;
	} else {
		return EINVAL;
	}

	/* The green weight is what red and blue leave of the whole. */
	green_weight = CAMCOMMON_WEIGHT_ONE - red_weight - blue_weight;

	/* Red takes only Cr, scaled by 2(1-Kr). */
	coefficients->cr_to_red = camcommon_fixed_ratio(
		2 * (CAMCOMMON_WEIGHT_ONE - red_weight) * chroma_numerator,
		CAMCOMMON_WEIGHT_ONE * chroma_denominator);

	/* Blue takes only Cb, scaled by 2(1-Kb). */
	coefficients->cb_to_blue = camcommon_fixed_ratio(
		2 * (CAMCOMMON_WEIGHT_ONE - blue_weight) * chroma_numerator,
		CAMCOMMON_WEIGHT_ONE * chroma_denominator);

	/* Green gives back the blue and red parts luma carried. */
	coefficients->cb_to_green = camcommon_fixed_ratio(
		2 * blue_weight * (CAMCOMMON_WEIGHT_ONE - blue_weight) * chroma_numerator,
		CAMCOMMON_WEIGHT_ONE * green_weight * chroma_denominator);
	coefficients->cr_to_green = camcommon_fixed_ratio(
		2 * red_weight * (CAMCOMMON_WEIGHT_ONE - red_weight) * chroma_numerator,
		CAMCOMMON_WEIGHT_ONE * green_weight * chroma_denominator);

	/* Succeeded: the coefficients describe the matrix and the range. */
	return 0;
}

/*
 * Converts a YUYV image into packed 8-bit RGB.
 *
 * Each four bytes Y0 U Y1 V give two pixels that share their chroma.  The
 * width must be even, the stride at least two bytes per pixel, and the
 * output must hold width * height * 3 bytes.  EINVAL tells a size that
 * cannot be YUYV.
 */
int
camcommon_yuyv_to_rgb(
	const uint8_t *yuyv,
	uint32_t width,
	uint32_t height,
	uint32_t stride,
	const struct camcommon_yuv_coefficients *coefficients,
	uint8_t *rgb)
{
	const uint8_t *row;
	uint8_t *pixel;
	uint32_t line;
	uint32_t pair;
	int64_t luma[2];
	int64_t blue_difference;
	int64_t red_difference;
	int64_t red_part;
	int64_t green_part;
	int64_t blue_part;
	unsigned half;

	/* Refuses a size that does not split into whole pixel pairs. */
	if (width == 0 || height == 0)
		return EINVAL;
	if ((width & 1U) != 0)
		return EINVAL;
	if (stride < width * 2U)
		return EINVAL;

	/* Converts every line, two pixels at a time. */
	pixel = rgb;
	for (line = 0; line < height; line++) {
		row = yuyv + (size_t)line * stride;
		for (pair = 0; pair < width / 2U; pair++) {
			/* Reads the two lumas and the shared chroma of the pair. */
			luma[0] = ((int64_t)row[pair * 4U] - coefficients->y_offset) * coefficients->y_scale;
			blue_difference = (int64_t)row[pair * 4U + 1U] - 128;
			luma[1] = ((int64_t)row[pair * 4U + 2U] - coefficients->y_offset) * coefficients->y_scale;
			red_difference = (int64_t)row[pair * 4U + 3U] - 128;

			/* The chroma part of each primary is the same for both pixels. */
			red_part = coefficients->cr_to_red * red_difference;
			green_part = -coefficients->cb_to_green * blue_difference - coefficients->cr_to_green * red_difference;
			blue_part = coefficients->cb_to_blue * blue_difference;

			/* Writes the two pixels as red, green and blue bytes. */
			for (half = 0; half < 2U; half++) {
				pixel[0] = camcommon_clamp(luma[half] + red_part);
				pixel[1] = camcommon_clamp(luma[half] + green_part);
				pixel[2] = camcommon_clamp(luma[half] + blue_part);
				pixel += 3;
			}
		}
	}

	/* Succeeded: the output holds the whole image. */
	return 0;
}

/*
 * Writes packed 8-bit RGB as a binary PPM (P6) file.
 *
 * Reports an errno when the file cannot be created or written.
 */
int
camcommon_write_ppm(
	const char *path,
	const uint8_t *rgb,
	uint32_t width,
	uint32_t height)
{
	FILE *file;
	size_t length;
	size_t written;
	int printed;
	int closed;
	int error;

	/* Creates the file, replacing an older one. */
	errno = 0;
	file = fopen(path, "wb");
	if (file == NULL) {
		error = camcommon_errno();
		return error;
	}

	/* Writes the header: the magic, the size and the largest sample. */
	printed = fprintf(file, "P6\n%u %u\n255\n", (unsigned)width, (unsigned)height);
	if (printed < 0) {
		error = camcommon_errno();
		fclose(file);
		return error;
	}

	/* Writes the pixels, three bytes each, line after line. */
	length = (size_t)width * height * 3U;
	written = fwrite(rgb, 1, length, file);
	if (written != length) {
		error = camcommon_errno();
		fclose(file);
		return error;
	}

	/* Closes the file, which flushes what is still buffered. */
	closed = fclose(file);
	if (closed != 0) {
		error = camcommon_errno();
		return error;
	}

	/* Succeeded: the file holds the whole image. */
	return 0;
}

/*
 * Writes a byte run to a file, replacing what the file held.
 *
 * Reports an errno when the file cannot be created or written.
 */
int
camcommon_write_file(
	const char *path,
	const uint8_t *data,
	size_t length)
{
	FILE *file;
	size_t written;
	int closed;
	int error;

	/* Creates the file, replacing an older one. */
	errno = 0;
	file = fopen(path, "wb");
	if (file == NULL) {
		error = camcommon_errno();
		return error;
	}

	/* Writes the whole run. */
	written = fwrite(data, 1, length, file);
	if (written != length) {
		error = camcommon_errno();
		fclose(file);
		return error;
	}

	/* Closes the file, which flushes what is still buffered. */
	closed = fclose(file);
	if (closed != 0) {
		error = camcommon_errno();
		return error;
	}

	/* Succeeded: the file holds the run. */
	return 0;
}

/*
 * Names a camera pixel format.
 *
 * An unknown format is named "unknown".
 */
const char *
camcommon_format_name(
	uint32_t format)
{
	/* Picks the name the tools print and accept. */
	switch (format) {
	case CAM_FORMAT_RAW8:
		return "raw8";
	case CAM_FORMAT_RAW10P:
		return "raw10p";
	case CAM_FORMAT_RAW10:
		return "raw10";
	case CAM_FORMAT_RAW12P:
		return "raw12p";
	case CAM_FORMAT_RAW12:
		return "raw12";
	case CAM_FORMAT_RAW16:
		return "raw16";
	case CAM_FORMAT_YUYV:
		return "yuyv";
	case CAM_FORMAT_UYVY:
		return "uyvy";
	case CAM_FORMAT_NV12:
		return "nv12";
	case CAM_FORMAT_YUV420P:
		return "yuv420p";
	case CAM_FORMAT_RGB565:
		return "rgb565";
	case CAM_FORMAT_RGB888:
		return "rgb888";
	case CAM_FORMAT_XRGB8888:
		return "xrgb8888";
	case CAM_FORMAT_MJPEG:
		return "mjpeg";
	case CAM_FORMAT_H264:
		return "h264";
	default:
		break;
	}

	/* Reports a format the tools do not know. */
	return "unknown";
}

/*
 * Asks a camera node what it is.
 *
 * Reports the errno of a refused request.
 */
int
camcommon_get_info(
	int descriptor,
	struct cam_info *info)
{
	int status;
	int error;

	/* Asks the node, with an empty record of the current version. */
	camcommon_record_init(info, sizeof(*info));
	status = ioctl(descriptor, CAM_GET_INFO, info);
	if (status < 0) {
		error = camcommon_errno();
		return error;
	}

	/* Succeeded: the record describes the camera. */
	return 0;
}

/*
 * Prints what a camera node is.
 *
 * The text fields are printed up to their size even when the node left
 * them without a terminating zero.
 */
void
camcommon_print_info(
	const struct cam_info *info)
{
	/* Prints who drives the camera, what it is and where it is attached. */
	printf("info driver=%.*s name=\"%.*s\" location=%.*s\n",
	       (int)strnlen(info->driver, sizeof(info->driver)),
	       info->driver,
	       (int)strnlen(info->name, sizeof(info->name)),
	       info->name,
	       (int)strnlen(info->location, sizeof(info->location)),
	       info->location);

	/* Prints what it can deliver and what the transport takes. */
	printf("info capabilities=0x%08x max_buffers=%u format_count=%u clock_hz=%u host_payload_max=%u\n",
	       (unsigned)info->capabilities,
	       (unsigned)info->max_buffers,
	       (unsigned)info->format_count,
	       (unsigned)info->clock_hz,
	       (unsigned)info->host_payload_max);
}

/*
 * Prints every format, mode and interval of a camera node.
 *
 * Intervals past the eight a mode reports inline are asked for one at a
 * time.  Reports the errno of the first refused request.
 */
int
camcommon_print_formats(
	int descriptor,
	const struct cam_info *info)
{
	struct cam_format_desc desc;
	uint32_t format_index;
	uint32_t mode_index;
	int status;
	int error;

	/* Walks the formats the node counted in its information. */
	for (format_index = 0; format_index < info->format_count; format_index++) {
		/* Asks for the format at this index. */
		camcommon_record_init(&desc, sizeof(desc));
		desc.index = format_index;
		status = ioctl(descriptor, CAM_ENUM_FORMAT, &desc);
		if (status < 0) {
			error = camcommon_errno();
			printf("format index=%u error=%d (%s)\n", (unsigned)format_index, error, strerror(error));
			return error;
		}

		/* Prints the format and how its pixels are grouped. */
		printf("format index=%u %s (0x%04x) flags=0x%x bpp=%u group=%u bytes/%u pixels modes=%u\n",
		       (unsigned)desc.index,
		       camcommon_format_name(desc.format),
		       (unsigned)desc.format,
		       (unsigned)desc.flags,
		       (unsigned)desc.bits_per_pixel,
		       (unsigned)desc.bytes_per_group,
		       (unsigned)desc.pixels_per_group,
		       (unsigned)desc.mode_count);

		/* Prints every mode of the format. */
		for (mode_index = 0; mode_index < desc.mode_count; mode_index++) {
			error = camcommon_print_mode(descriptor, desc.format, mode_index);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: every format, mode and interval was printed. */
	return 0;
}

/*
 * Asks a camera node for its counters and prints them.
 *
 * Reports the errno of a refused request.
 */
int
camcommon_print_counters(
	int descriptor)
{
	struct cam_counters counters;
	int status;
	int error;

	/* Asks the node for what it counted since it was attached. */
	camcommon_record_init(&counters, sizeof(counters));
	status = ioctl(descriptor, CAM_GET_COUNTERS, &counters);
	if (status < 0) {
		error = camcommon_errno();
		printf("counters error=%d (%s)\n", error, strerror(error));
		return error;
	}

	/* Prints the frame counts the framework keeps. */
	printf("counters frames=%llu dropped_no_buffer=%llu dropped_overflow=%llu dropped_error=%llu dropped_transport=%llu\n",
	       (unsigned long long)counters.frames,
	       (unsigned long long)counters.dropped_no_buffer,
	       (unsigned long long)counters.dropped_overflow,
	       (unsigned long long)counters.dropped_error,
	       (unsigned long long)counters.dropped_transport);

	/* Prints the transport errors the source keeps. */
	printf("counters invalid_header=%llu short_header=%llu missed_packets=%llu packet_errors=%llu sequence_gaps=%llu\n",
	       (unsigned long long)counters.invalid_header,
	       (unsigned long long)counters.short_header,
	       (unsigned long long)counters.missed_packets,
	       (unsigned long long)counters.packet_errors,
	       (unsigned long long)counters.sequence_gaps);

	/* Prints the signal watchdog and the recovery counts. */
	printf("counters no_signal=%llu resubmit_failures=%llu halt_timeouts=%llu last_frame_age_ms=%u\n",
	       (unsigned long long)counters.no_signal,
	       (unsigned long long)counters.resubmit_failures,
	       (unsigned long long)counters.halt_timeouts,
	       (unsigned)counters.last_frame_age_ms);

	/* Succeeded: the counters were printed. */
	return 0;
}

/* Finds a JPEG frame's SOF segment and whether a DHT segment precedes its scan. */
static int
camcommon_mjpeg_scan(
	const uint8_t *frame,
	size_t length,
	size_t *sof_offset,
	int *has_tables)
{
	size_t position;
	size_t marker_start;
	size_t segment_length;
	unsigned marker;
	int sof_found;
	int is_sof;
	int is_standalone;

	/* Refuses a frame that does not begin with SOI. */
	if (length < 4)
		return EINVAL;
	if (frame[0] != CAMCOMMON_JPEG_PREFIX || frame[1] != CAMCOMMON_JPEG_SOI)
		return EINVAL;

	/* Walks the segments after SOI up to the first scan. */
	position = 2;
	sof_found = 0;
	*has_tables = 0;
	*sof_offset = 0;
	for (;;) {
		/* A frame that ends before its scan has nothing to decode. */
		if (position >= length)
			return EINVAL;

		/* Every segment starts with the marker prefix. */
		if (frame[position] != CAMCOMMON_JPEG_PREFIX)
			return EINVAL;

		/* Skips the fill bytes a marker may be preceded by. */
		marker_start = position;
		while (position < length && frame[position] == CAMCOMMON_JPEG_PREFIX)
			position++;

		/* Refuses a frame cut off inside a marker. */
		if (position >= length)
			return EINVAL;
		marker = frame[position];
		position++;

		/* The first scan ends the part of the frame tables may be in. */
		if (marker == CAMCOMMON_JPEG_SOS)
			break;

		/* An image that ends before any scan is broken. */
		if (marker == CAMCOMMON_JPEG_EOI)
			return EINVAL;

		/* A standalone marker has no length field to skip. */
		is_standalone = camcommon_jpeg_is_standalone(marker);
		if (is_standalone)
			continue;

		/* Reads the segment's length, which counts its own two bytes. */
		if (length - position < 2)
			return EINVAL;
		segment_length = ((size_t)frame[position] << 8) | frame[position + 1];
		if (segment_length < 2)
			return EINVAL;
		if (length - position < segment_length)
			return EINVAL;

		/* Notes a frame that brings its own Huffman tables. */
		if (marker == CAMCOMMON_JPEG_DHT)
			*has_tables = 1;

		/* Remembers where the first frame header starts. */
		is_sof = camcommon_jpeg_is_sof(marker);
		if (is_sof && !sof_found) {
			*sof_offset = marker_start;
			sof_found = 1;
		}

		/* Steps over the segment's body. */
		position += segment_length;
	}

	/* Refuses a scan with no frame header before it. */
	if (!sof_found)
		return EINVAL;

	/* Succeeded: the SOF offset and the tables' presence are known. */
	return 0;
}

/* Tells whether a marker starts a frame header (SOF0 to SOF15). */
static int
camcommon_jpeg_is_sof(
	unsigned marker)
{
	/* Markers outside C0..CF are not frame headers. */
	if (marker < CAMCOMMON_JPEG_SOF_FIRST)
		return 0;
	if (marker > CAMCOMMON_JPEG_SOF_LAST)
		return 0;

	/* DHT, JPG and DAC share the range but are not frame headers. */
	if (marker == CAMCOMMON_JPEG_DHT)
		return 0;
	if (marker == CAMCOMMON_JPEG_JPG)
		return 0;
	if (marker == CAMCOMMON_JPEG_DAC)
		return 0;

	/* Reports a frame header. */
	return 1;
}

/* Tells whether a marker stands alone, without a length field. */
static int
camcommon_jpeg_is_standalone(
	unsigned marker)
{
	/* TEM carries no segment. */
	if (marker == CAMCOMMON_JPEG_TEM)
		return 1;

	/* The restart markers RST0 to RST7 carry no segment. */
	if (marker >= CAMCOMMON_JPEG_RST0 && marker <= CAMCOMMON_JPEG_RST7)
		return 1;

	/* Reports a marker followed by a length field. */
	return 0;
}

/* Writes the DHT segment of the four standard tables. */
static void
camcommon_mjpeg_write_dht(
	uint8_t *output)
{
	size_t segment_length;
	size_t position;
	unsigned index;
	const struct camcommon_huffman_table *table;

	/* Writes the marker and the length, which leaves out the marker. */
	segment_length = camcommon_mjpeg_dht_size() - 2;
	output[0] = CAMCOMMON_JPEG_PREFIX;
	output[1] = CAMCOMMON_JPEG_DHT;
	output[2] = (uint8_t)(segment_length >> 8);
	output[3] = (uint8_t)(segment_length & 0xffU);

	/* Writes each table's class, its code counts and its symbols. */
	position = 4;
	for (index = 0; index < CAMCOMMON_HUFFMAN_TABLES; index++) {
		table = &camcommon_standard_tables[index];
		output[position] = table->class_and_destination;
		position++;
		memcpy(output + position, table->code_counts, CAMCOMMON_HUFFMAN_LENGTHS);
		position += CAMCOMMON_HUFFMAN_LENGTHS;
		memcpy(output + position, table->symbols, table->symbol_count);
		position += table->symbol_count;
	}
}

/* Turns a ratio into a fixed-point value, rounded to the nearest. */
static int64_t
camcommon_fixed_ratio(
	int64_t numerator,
	int64_t denominator)
{
	/* Reports the ratio scaled by 65536. */
	return (numerator * CAMCOMMON_FIXED_ONE + denominator / 2) / denominator;
}

/* Turns a fixed-point sample into a byte, clamped to 0..255. */
static uint8_t
camcommon_clamp(
	int64_t value)
{
	int64_t rounded;

	/* A negative sample is black in that primary. */
	if (value <= 0)
		return 0;

	/* A sample past full scale is the primary at its brightest. */
	rounded = (value + CAMCOMMON_FIXED_HALF) / CAMCOMMON_FIXED_ONE;
	if (rounded > 255)
		return 255;

	/* Reports the sample as a byte. */
	return (uint8_t)rounded;
}

/* Prints one frame interval with the frame rate it gives. */
static void
camcommon_print_interval(
	uint32_t interval_100ns)
{
	uint64_t millihertz;

	/* An interval of zero has no rate to print. */
	if (interval_100ns == 0) {
		printf(" 0");
		return;
	}

	/* Prints the interval and its rate in thousandths of a frame per second. */
	millihertz = 10000000000ULL / interval_100ns;
	printf(" %u(%llu.%03llufps)",
	       (unsigned)interval_100ns,
	       (unsigned long long)(millihertz / 1000U),
	       (unsigned long long)(millihertz % 1000U));
}

/* Prints one mode of a format with all of its intervals. */
static int
camcommon_print_mode(
	int descriptor,
	uint32_t format,
	uint32_t mode_index)
{
	struct cam_mode_desc mode;
	struct cam_interval_desc interval;
	uint32_t inline_count;
	uint32_t index;
	int status;
	int error;

	/* Asks for the mode at this index of the format. */
	camcommon_record_init(&mode, sizeof(mode));
	mode.format = format;
	mode.index = mode_index;
	status = ioctl(descriptor, CAM_ENUM_MODE, &mode);
	if (status < 0) {
		error = camcommon_errno();
		printf("  mode index=%u error=%d (%s)\n", (unsigned)mode_index, error, strerror(error));
		return error;
	}

	/* Prints the frame size and the largest frame of the mode. */
	printf("  mode index=%u %ux%u max_frame_bytes=%u",
	       (unsigned)mode.index,
	       (unsigned)mode.width,
	       (unsigned)mode.height,
	       (unsigned)mode.max_frame_bytes);

	/* A continuous mode reports its range of intervals. */
	if (mode.interval_type == CAM_INTERVAL_CONTINUOUS) {
		printf(" continuous min");
		camcommon_print_interval(mode.interval_min);
		printf(" max");
		camcommon_print_interval(mode.interval_max);
		printf(" step=%u\n", (unsigned)mode.interval_step);
		return 0;
	}

	/* Prints the intervals the mode reported inline, the default first. */
	inline_count = mode.interval_count;
	if (inline_count > CAM_MODE_INTERVALS_INLINE)
		inline_count = CAM_MODE_INTERVALS_INLINE;
	printf(" discrete count=%u intervals_100ns:", (unsigned)mode.interval_count);
	for (index = 0; index < inline_count; index++)
		camcommon_print_interval(mode.intervals[index]);

	/* Asks for each interval past the inline ones by its index in the whole list. */
	for (index = CAM_MODE_INTERVALS_INLINE; index < mode.interval_count; index++) {
		camcommon_record_init(&interval, sizeof(interval));
		interval.format = format;
		interval.mode_index = mode_index;
		interval.index = index;
		status = ioctl(descriptor, CAM_ENUM_INTERVAL, &interval);
		if (status < 0) {
			error = camcommon_errno();
			printf(" [interval %u error=%d (%s)]\n", (unsigned)index, error, strerror(error));
			return error;
		}

		/* Tells the interval. */
		camcommon_print_interval(interval.interval_100ns);
	}

	/* Succeeded: the mode and every interval were printed. */
	printf("\n");
	return 0;
}

/* Empties a request record and stamps its version and its size. */
static void
camcommon_record_init(
	void *record,
	size_t size)
{
	uint32_t header[2];

	/* Clears every field, the reserved ones included. */
	memset(record, 0, size);

	/* Stamps the version and the size the node checks first. */
	header[0] = CAM_ABI_VERSION;
	header[1] = (uint32_t)size;
	memcpy(record, header, sizeof(header));
}

/* Reports the errno a failed call left, or EIO when it left none. */
static int
camcommon_errno(void)
{
	/* A failure that did not say why is reported as an I/O error. */
	if (errno == 0)
		return EIO;

	/* Reports the failure's own errno. */
	return errno;
}
