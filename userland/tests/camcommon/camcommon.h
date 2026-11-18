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
 */

#ifndef USERLAND_TESTS_CAMCOMMON_H
#define USERLAND_TESTS_CAMCOMMON_H

#include <stddef.h>
#include <stdint.h>
#include <uapi/camera.h>

/* The matrices a YUV frame may have been encoded with. */
#define CAMCOMMON_MATRIX_BT601		1U
#define CAMCOMMON_MATRIX_BT709		2U

/* The ranges a YUV frame's samples may cover. */
#define CAMCOMMON_RANGE_LIMITED		1U
#define CAMCOMMON_RANGE_FULL		2U

/*
 * The fixed-point coefficients that turn one YUV sample triple into RGB.
 *
 * One instance stands for one choice of matrix and range; it is filled
 * once by camcommon_yuv_coefficients_init and then read for every pixel.
 * Every coefficient is scaled by 65536.
 */
struct camcommon_yuv_coefficients {
	int32_t y_offset;
	int64_t y_scale;
	int64_t cr_to_red;
	int64_t cb_to_green;
	int64_t cr_to_green;
	int64_t cb_to_blue;
};

/*
 * Reports how many bytes the standard Huffman table segment adds to a frame.
 */
size_t camcommon_mjpeg_dht_size(void);

/*
 * Copies an MJPEG frame, inserting the standard Huffman tables when it has none.
 */
int camcommon_mjpeg_complete(const uint8_t *frame, size_t length, uint8_t *output, size_t capacity, size_t *output_length, int *inserted);

/*
 * Fills the YUV to RGB coefficients of one matrix and one range.
 */
int camcommon_yuv_coefficients_init(struct camcommon_yuv_coefficients *coefficients, uint32_t matrix, uint32_t range);

/*
 * Converts a YUYV image into packed 8-bit RGB.
 */
int camcommon_yuyv_to_rgb(const uint8_t *yuyv, uint32_t width, uint32_t height, uint32_t stride, const struct camcommon_yuv_coefficients *coefficients, uint8_t *rgb);

/*
 * Writes packed 8-bit RGB as a binary PPM (P6) file.
 */
int camcommon_write_ppm(const char *path, const uint8_t *rgb, uint32_t width, uint32_t height);

/*
 * Writes a byte run to a file, replacing what the file held.
 */
int camcommon_write_file(const char *path, const uint8_t *data, size_t length);

/*
 * Names a camera pixel format.
 */
const char *camcommon_format_name(uint32_t format);

/*
 * Asks a camera node what it is.
 */
int camcommon_get_info(int descriptor, struct cam_info *info);

/*
 * Prints what a camera node is.
 */
void camcommon_print_info(const struct cam_info *info);

/*
 * Prints every format, mode and interval of a camera node.
 */
int camcommon_print_formats(int descriptor, const struct cam_info *info);

/*
 * Asks a camera node for its counters and prints them.
 */
int camcommon_print_counters(int descriptor);

#endif
