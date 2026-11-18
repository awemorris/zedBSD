/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A picture drawn into a bitmap on the CPU: for pictures the page uses as
 * images (an SVG in a CSS background, a favicon) and for the tests that
 * compare the vector path with a known picture.  It is done once when the
 * content changes, never in a frame.
 *
 * The compositing is Compositing and Blending 1 §5.1's and §9.1.4's
 * source-over on premultiplied colors.  The bitmap and every layer hold
 * premultiplied 8-bit channels (0xAARRGGBB); each fill is composited in
 * single precision per pixel and the result rounded to the nearest 8-bit
 * value at once: for each pixel and each fill,
 *   coverage  = alpha of the fill's coverage (vec_rasterize, at the
 *               pixel phase 0) / 255, times the clip's coverage / 255
 *               when clipped;
 *   source    = the paint's premultiplied color (a color, or
 *               vec_gradient_sample at the pixel's centre with a ramp of
 *               vec_gradient_ramp_width(vec_gradient_line_length over
 *               the bitmap's rectangle)) times alpha times coverage;
 *   result    = source + result * (1 - source alpha).
 * A group (SVG 2 §3.6.1, Compositing and Blending 1 §8.2's isolated
 * group) is drawn into a cleared layer of the bitmap's size and
 * composited back with its opacity (the layer's pixels times opacity,
 * source-over) when it ends.  A group deeper than SVG_BITMAP_LAYERS_MAX,
 * or whose layer would take the layers' memory beyond
 * SVG_BITMAP_LAYER_BYTES_MAX, is drawn into its parent with its opacity
 * multiplied into each of its fills' alpha (the approximation the display
 * list uses).
 */

#ifndef KEILAND_BROWSER_SVG_BITMAP_H
#define KEILAND_BROWSER_SVG_BITMAP_H

#include "svg/svg-draw.h"

/* The largest side of a bitmap drawn (the page's images keep to this). */
#define SVG_BITMAP_SIDE_MAX	4096

/* The deepest nesting of groups drawn as layers (deeper groups are drawn into their parent, their opacity applied to each fill). */
#define SVG_BITMAP_LAYERS_MAX	16

/* The most bytes all the layers alive at once take (four bytes a pixel): 64 MiB, four layers of the largest bitmap. */
#define SVG_BITMAP_LAYER_BYTES_MAX	(64UL << 20)

/*
 * Draws a picture into a bitmap: width by height pixels, rows top to
 * bottom, stride pixels apart, each 0xAARRGGBB with straight
 * (not premultiplied) alpha as the page's images are (struct img_bitmap);
 * the bitmap's previous contents are replaced (it starts transparent).
 * placement maps the picture's space to the bitmap's pixels.
 *
 * Input: picture built; pixels room for stride * height; width and height
 * 1 .. SVG_BITMAP_SIDE_MAX; stride >= width; placement valid.
 * Errors: EINVAL for sizes or a placement out of range (pixels untouched);
 * ENOMEM (pixels then cleared to transparent).
 */
int svg_picture_render(const struct svg_picture *picture, const struct vec_matrix *placement, uint32_t *pixels, int width, int height, int stride);

#endif
