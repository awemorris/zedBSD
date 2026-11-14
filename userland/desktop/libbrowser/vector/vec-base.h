/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The common ground of the vector drawing: points, rectangles, the 2D
 * affine matrix, the limits every vector module keeps and the meaning of
 * the error values they return.
 *
 * The vector modules (vector/vec-*.h) are pure computation: they take
 * numbers and memory from their caller, never touch a file, a thread, the
 * DOM or the GPU, and are built and tested on the host alone.
 *
 * Error values, the same in every vector module:
 *   EINVAL   the input is malformed: a NaN or an infinity, a coordinate
 *            beyond VEC_COORD_MAX, a negative size where none is allowed,
 *            a singular matrix that has to be inverted, an empty input
 *            where one is required.
 *   ENOBUFS  a fixed limit of this header or of the module's own header
 *            is exceeded (too many points, too large a raster).
 *   ENOMEM   an allocation failed.
 *   ENOTSUP  the input is well formed but asks for something the module
 *            does not do.
 * A function that fails leaves its output as its comment says (most leave
 * it as it was, or reset to empty); none of them reads or writes outside
 * the memory it was given, whatever the input.
 *
 * Arithmetic: every value is a float in single precision.  A function
 * checks its inputs are finite (vec_finite) before it uses them, and
 * keeps intermediate values finite (a division is guarded by a test of
 * its divisor, a square root by a test of its argument).  Integers that
 * come from floats are clamped to their range before the conversion, as
 * converting an out-of-range float to an integer is undefined in C.
 */

#ifndef KEILAND_BROWSER_VECTOR_BASE_H
#define KEILAND_BROWSER_VECTOR_BASE_H

#include <stddef.h>
#include <stdint.h>

/*
 * The largest magnitude of a coordinate, in any space (user units or
 * device pixels), that the modules take: 2^24, where a float still holds
 * every integer exactly, so that one pixel stays distinguishable.  A
 * point beyond it is refused with EINVAL.
 */
#define VEC_COORD_MAX		16777216.0f

/*
 * The flattening tolerance in device pixels that a caller uses when it
 * has no reason to choose another: the greatest distance between a curve
 * and the line segments that stand for it.  A tenth of a pixel keeps the
 * difference under one step of an 8-bit coverage.
 */
#define VEC_TOLERANCE_DEFAULT	0.1f

/* The ratio of a circle's circumference to its diameter, in single precision. */
#define VEC_PI			3.14159265358979323846f

/*
 * A point or a vector: x to the right, y down (the CSS and SVG
 * coordinate systems both point y down).
 */
struct vec_point {
	float x;
	float y;
};

/*
 * An axis-aligned rectangle by its edges: it holds the points with
 * left <= x < right and top <= y < bottom.  It is empty when
 * right <= left or bottom <= top.
 */
struct vec_rect {
	float left;
	float top;
	float right;
	float bottom;
};

/*
 * An axis-aligned rectangle of whole pixels by its edges (the same
 * half-open meaning as struct vec_rect).
 */
struct vec_irect {
	int32_t left;
	int32_t top;
	int32_t right;
	int32_t bottom;
};

/*
 * A 2D affine transform in the form SVG 2 §8.5 and CSS Transforms 1 §9.1
 * and §14 (the matrix() function) give it:
 *
 *   | a c e |                 x' = a * x + c * y + e
 *   | b d f |   applied as    y' = b * x + d * y + f
 *   | 0 0 1 |
 *
 * The identity is a = d = 1, the rest 0.
 */
struct vec_matrix {
	float a;
	float b;
	float c;
	float d;
	float e;
	float f;
};

/*
 * Tells whether a float is a finite number (not a NaN and not an
 * infinity).
 *
 * Returns 1 for a finite value, 0 otherwise.  Never fails.
 */
int vec_finite(float value);

/*
 * Tells whether a point is usable as a coordinate: both components are
 * finite and their magnitudes are at most VEC_COORD_MAX.
 *
 * Returns 1 when it is, 0 otherwise.  Never fails.
 */
int vec_point_valid(struct vec_point point);

/*
 * Makes the identity matrix.
 *
 * Input: matrix must not be NULL.  Output: *matrix is the identity.
 */
void vec_matrix_identity(struct vec_matrix *matrix);

/*
 * Tells whether a matrix is usable: its six entries are finite and the
 * magnitude of each is at most VEC_COORD_MAX.
 *
 * Returns 1 when it is, 0 otherwise.  Never fails.
 */
int vec_matrix_valid(const struct vec_matrix *matrix);

/*
 * Composes two transforms: out applies first, then then.  In the
 * matrix product of SVG 2 §8.5 this is out = then x first, so that
 * vec_matrix_apply(out, p) == vec_matrix_apply(then, vec_matrix_apply(first, p))
 * up to rounding.
 *
 * Input: none of the pointers is NULL; out may be the same object as
 * first or then (the result is computed before it is stored).
 * Output: *out is the product.  Never fails; a product of valid matrices
 * may still be invalid (overflow), which the caller checks with
 * vec_matrix_valid where it matters.
 */
void vec_matrix_multiply(struct vec_matrix *out, const struct vec_matrix *first, const struct vec_matrix *then);

/*
 * Inverts a matrix.
 *
 * Input: matrix and out are not NULL; out may be the same object.
 * Output: on success *out is the inverse, so that applying matrix then
 * out gives the point back up to rounding.
 * Errors: EINVAL when matrix is not valid (vec_matrix_valid), or when
 * its determinant a * d - b * c has a magnitude under 1e-12 or is not
 * finite, or the inverse is not valid; *out is then unchanged.
 */
int vec_matrix_invert(struct vec_matrix *out, const struct vec_matrix *matrix);

/*
 * Applies a matrix to a point (with its translation).
 *
 * Returns the transformed point.  Never fails (the caller checks the
 * result with vec_point_valid where it matters).
 */
struct vec_point vec_matrix_apply(const struct vec_matrix *matrix, struct vec_point point);

/*
 * Applies a matrix to a vector (without its translation): a direction
 * or a difference of two points.
 *
 * Returns the transformed vector.  Never fails.
 */
struct vec_point vec_matrix_apply_vector(const struct vec_matrix *matrix, struct vec_point vector);

/*
 * Reports how much a matrix stretches a length at most: the largest
 * singular value of its 2x2 part, sqrt of the larger eigenvalue of
 * M^T M.  A caller divides a tolerance in device pixels by it to get a
 * tolerance in user units.
 *
 * Returns the factor, at least 0; 0 for a matrix that is not valid.
 */
float vec_matrix_scale_max(const struct vec_matrix *matrix);

/*
 * Computes the angle of the vector (x, y) from the positive x axis, in
 * radians in (-pi, pi], by a polynomial that a GPU shader evaluates with
 * the same steps (the project's own shader compilers have no arctangent
 * instruction, so neither the CPU reference renderer nor the shader uses
 * the C library's or GLSL's atan):
 *
 *   with a = min(|x|, |y|) / max(|x|, |y|) in [0, 1], z = a * a,
 *   p = a * (0.9998660 + z * (-0.3302995 + z * (0.1801410 +
 *       z * (-0.0851330 + z * 0.0208351))))
 *   (Abramowitz and Stegun, Handbook of Mathematical Functions, formula
 *   4.4.47: arctan a for 0 <= a <= 1 with an error under 1e-5);
 *   then p = pi / 2 - p when |y| > |x|; p = pi - p when x < 0; and the
 *   result p for y >= 0, -p for y < 0 (so the angle of (-1, 0) is pi).
 *
 * Evaluated in that order, in single precision.  When both |x| and |y|
 * are under 1e-20 the angle is 0.  A NaN in either gives 0.
 *
 * Returns the angle, within 1e-5 radians of the exact one (plus the
 * rounding of single precision).  Never fails.
 */
float vec_atan2(float y, float x);

/*
 * Computes e^x as 2^(x * 1.44269504f) (1.44269504 = log2 e), the form a
 * GPU shader uses (some of the project's shader compilers have exp2 but
 * not exp); the CPU side calls exp2f with that product.
 *
 * Returns the value: 0 for a NaN and for x below -87; x above 88 gives
 * 2^(88 * 1.44269504) computed as for 88 (no infinity escapes).  Never
 * fails.
 */
float vec_exp(float x);

/*
 * The matrices of the transform functions (CSS Transforms 1 §9.1 "2D
 * Transform Functions" and §14, which SVG 2 §8.5 and CSS Transforms 1
 * §7.3 use for the transform attribute): each writes the matrix of one function into *out.
 *
 * translate(tx, ty): a = d = 1, e = tx, f = ty.
 * scale(sx, sy): a = sx, d = sy.
 * rotate(degrees): a = cos, b = sin, c = -sin, d = cos of the angle
 *   (positive turns from the x axis towards the y axis, clockwise on the
 *   screen).
 * skew_x(degrees): c = tan of the angle.  skew_y(degrees): b = tan.
 *
 * Errors: EINVAL when an argument is not finite, or the tangent of a
 * skew is not finite or beyond VEC_COORD_MAX (an angle of 90 degrees);
 * *out is then unchanged.
 */
int vec_matrix_translate(struct vec_matrix *out, float tx, float ty);
int vec_matrix_scale(struct vec_matrix *out, float sx, float sy);
int vec_matrix_rotate(struct vec_matrix *out, float degrees);
int vec_matrix_skew_x(struct vec_matrix *out, float degrees);
int vec_matrix_skew_y(struct vec_matrix *out, float degrees);

/*
 * Grows a rectangle to hold a point: a rectangle with right < left is
 * the empty start, which the first point turns into that point's zero
 * size rectangle.
 *
 * Input: rect not NULL; the point is valid.  Never fails.
 */
void vec_rect_include(struct vec_rect *rect, struct vec_point point);

/*
 * Makes the empty start for vec_rect_include: left = top = +VEC_COORD_MAX,
 * right = bottom = -VEC_COORD_MAX.
 */
void vec_rect_empty(struct vec_rect *rect);

#endif
