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
 * The compositing is Compositing and Blending 1 §5.1's and §9.1.4's source-over on
 * premultiplied colors, in single precision per channel, with the result
 * rounded to 8 bits once at the end: for each pixel and each fill,
 *   coverage  = alpha of the fill's coverage (vec_rasterize, at the
 *               pixel phase 0) / 255, times the clip's coverage / 255
 *               when clipped;
 *   source    = the paint's premultiplied color (a color, or
 *               vec_gradient_sample with a ramp of
 *               vec_gradient_ramp_width(the gradient line's device
 *               length)) times alpha times coverage;
 *   result    = source + result * (1 - source alpha).
 * A group (SVG 2 §3.6.1, Compositing and Blending 1 §8.2's isolated group) is drawn
 * into a cleared layer of the bitmap's size and composited back with its
 * opacity when it ends.
 */

#ifndef KEILAND_BROWSER_SVG_BITMAP_H
#define KEILAND_BROWSER_SVG_BITMAP_H

#include "svg/svg-draw.h"

/* The largest side of a bitmap drawn (the page's images keep to this). */
#define SVG_BITMAP_SIDE_MAX	4096

/* The deepest nesting of groups drawn as layers (deeper groups are drawn into their parent, their opacity applied to each fill). */
#define SVG_BITMAP_LAYERS_MAX	16

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
