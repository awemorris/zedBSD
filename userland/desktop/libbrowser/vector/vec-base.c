/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The common ground of the vector drawing: the affine matrix, the checks of points and rectangles, and the
 * arctangent and exponential that the GPU shaders compute the same way.
 */

#include "vector/vec-base.h"

#include <errno.h>
#include <float.h>
#include <math.h>

/*
 * The smallest determinant magnitude a matrix may have to be inverted
 * (the contract of vec_matrix_invert).
 */
#define BASE_DETERMINANT_MIN	1e-12f

/*
 * Under this magnitude in both components a vector has no direction that
 * vec_atan2 can tell (the contract of vec_atan2).
 */
#define BASE_ATAN_ZERO_LIMIT	1e-20f

/* The conversion factor from degrees to radians. */
#define BASE_DEGREES_TO_RADIANS	(VEC_PI / 180.0f)

/* The binary logarithm of e, the factor that turns e^x into 2^y. */
#define BASE_LOG2_E		1.44269504f

/* The arguments of vec_exp below which the value is 0 and above which it is held. */
#define BASE_EXP_LOW		(-87.0f)
#define BASE_EXP_HIGH		88.0f

/*
 * The odd-power coefficients of the arctangent polynomial of vec_atan2,
 * from the first power up, so that for 0 <= a <= 1
 * arctan a = a * (c[0] + z * (c[1] + z * (c[2] + z * (c[3] + z * c[4]))))
 * with z = a * a.
 *
 * Values referenced: Abramowitz and Stegun, Handbook of Mathematical
 * Functions, formula 4.4.47 (a1, a3, a5, a7, a9; error under 1e-5).
 */
static const float base_atan_coefficients[5] = {
	0.9998660f,
	-0.3302995f,
	0.1801410f,
	-0.0851330f,
	0.0208351f
};

/* The forward declarations of the static functions. */
static int base_sin_cos_degrees(float degrees, float *sine, float *cosine);
static int base_tan_degrees(float degrees, float *tangent);

/*
 * Tells whether a float is a finite number.
 *
 * A NaN is not equal to itself, and an infinity lies beyond the largest
 * finite float, so two comparisons separate them from the finite values.
 */
int
vec_finite(
	float value)
{
	/* Refuses a NaN, the only value that differs from itself. */
	if (value != value)
		return 0;

	/* Refuses the infinities, which lie beyond the largest finite float. */
	if (value > FLT_MAX || value < -FLT_MAX)
		return 0;

	/* Succeeded: the value is a finite number. */
	return 1;
}

/*
 * Tells whether a point is usable as a coordinate.
 *
 * Both components must be finite and within VEC_COORD_MAX in magnitude.
 */
int
vec_point_valid(
	struct vec_point point)
{
	int finite;

	/* Refuses a point whose x is a NaN or an infinity. */
	finite = vec_finite(point.x);
	if (!finite)
		return 0;

	/* Refuses a point whose y is a NaN or an infinity. */
	finite = vec_finite(point.y);
	if (!finite)
		return 0;

	/* Refuses an x beyond the coordinate range. */
	if (point.x > VEC_COORD_MAX || point.x < -VEC_COORD_MAX)
		return 0;

	/* Refuses a y beyond the coordinate range. */
	if (point.y > VEC_COORD_MAX || point.y < -VEC_COORD_MAX)
		return 0;

	/* Succeeded: the point is a usable coordinate. */
	return 1;
}

/*
 * Makes the identity matrix.
 */
void
vec_matrix_identity(
	struct vec_matrix *matrix)
{
	/* Writes the entries of the matrix that maps every point to itself. */
	matrix->a = 1.0f;
	matrix->b = 0.0f;
	matrix->c = 0.0f;
	matrix->d = 1.0f;
	matrix->e = 0.0f;
	matrix->f = 0.0f;
}

/*
 * Tells whether a matrix is usable.
 *
 * Each of the six entries must be finite and at most VEC_COORD_MAX in
 * magnitude.
 */
int
vec_matrix_valid(
	const struct vec_matrix *matrix)
{
	struct vec_point pair;
	int valid;

	/* Checks the linear entries a and b as the two components of a point. */
	pair.x = matrix->a;
	pair.y = matrix->b;
	valid = vec_point_valid(pair);
	if (!valid)
		return 0;

	/* Checks the linear entries c and d the same way. */
	pair.x = matrix->c;
	pair.y = matrix->d;
	valid = vec_point_valid(pair);
	if (!valid)
		return 0;

	/* Checks the translation entries e and f the same way. */
	pair.x = matrix->e;
	pair.y = matrix->f;
	valid = vec_point_valid(pair);
	if (!valid)
		return 0;

	/* Succeeded: all six entries are usable. */
	return 1;
}

/*
 * Composes two transforms: out applies first, then then.
 *
 * The product then x first is computed into a temporary before it is
 * stored, so out may be either input.
 */
void
vec_matrix_multiply(
	struct vec_matrix *out,
	const struct vec_matrix *first,
	const struct vec_matrix *then)
{
	struct vec_matrix product;

	/* Multiplies the linear parts: the columns of first are carried through then. */
	product.a = then->a * first->a + then->c * first->b;
	product.b = then->b * first->a + then->d * first->b;
	product.c = then->a * first->c + then->c * first->d;
	product.d = then->b * first->c + then->d * first->d;

	/* Carries the translation of first through then and adds the translation of then. */
	product.e = then->a * first->e + then->c * first->f + then->e;
	product.f = then->b * first->e + then->d * first->f + then->f;

	/* Stores the product, which is safe even when out is one of the inputs. */
	*out = product;
}

/*
 * Inverts a matrix.
 *
 * The inverse of the linear part is the adjugate over the determinant; the
 * inverse translation is the linear inverse applied to the negated
 * translation.  out is changed only on success.
 */
int
vec_matrix_invert(
	struct vec_matrix *out,
	const struct vec_matrix *matrix)
{
	struct vec_matrix inverse;
	float determinant;
	float magnitude;
	int valid;

	/* Refuses a matrix with a NaN, an infinity or an entry beyond the coordinate range. */
	valid = vec_matrix_valid(matrix);
	if (!valid)
		return EINVAL;

	/* Computes the determinant of the linear part. */
	determinant = matrix->a * matrix->d - matrix->b * matrix->c;

	/* Refuses a determinant that is not finite. */
	valid = vec_finite(determinant);
	if (!valid)
		return EINVAL;

	/* Refuses a matrix too close to singular to invert, which also guards the divisions below. */
	magnitude = determinant;
	if (magnitude < 0.0f)
		magnitude = -magnitude;
	if (magnitude < BASE_DETERMINANT_MIN)
		return EINVAL;

	/* Computes the inverse of the linear part (the adjugate divided by the determinant). */
	inverse.a = matrix->d / determinant;
	inverse.b = -matrix->b / determinant;
	inverse.c = -matrix->c / determinant;
	inverse.d = matrix->a / determinant;

	/* Computes the inverse translation: the linear inverse applied to (-e, -f). */
	inverse.e = (matrix->c * matrix->f - matrix->d * matrix->e) / determinant;
	inverse.f = (matrix->b * matrix->e - matrix->a * matrix->f) / determinant;

	/* Refuses an inverse that overflowed or left the coordinate range. */
	valid = vec_matrix_valid(&inverse);
	if (!valid)
		return EINVAL;

	/* Publishes the inverse. */
	*out = inverse;

	/* Succeeded: *out holds the inverse. */
	return 0;
}

/*
 * Applies a matrix to a point, with its translation.
 */
struct vec_point
vec_matrix_apply(
	const struct vec_matrix *matrix,
	struct vec_point point)
{
	struct vec_point result;

	/* Maps the point by x' = a x + c y + e and y' = b x + d y + f. */
	result.x = matrix->a * point.x + matrix->c * point.y + matrix->e;
	result.y = matrix->b * point.x + matrix->d * point.y + matrix->f;

	/* Reports the mapped point. */
	return result;
}

/*
 * Applies a matrix to a vector, without its translation.
 */
struct vec_point
vec_matrix_apply_vector(
	const struct vec_matrix *matrix,
	struct vec_point vector)
{
	struct vec_point result;

	/* Maps the vector by the linear part alone, as a difference of two points maps. */
	result.x = matrix->a * vector.x + matrix->c * vector.y;
	result.y = matrix->b * vector.x + matrix->d * vector.y;

	/* Reports the mapped vector. */
	return result;
}

/*
 * Reports how much a matrix stretches a length at most.
 *
 * The largest singular value of [[a, c], [b, d]] is half the sum of
 * sqrt((a + d)^2 + (b - c)^2) and sqrt((a - d)^2 + (b + c)^2); the
 * smallest is half their difference.  Both radicands are sums of squares,
 * so the square roots are of non-negative values.
 */
float
vec_matrix_scale_max(
	const struct vec_matrix *matrix)
{
	float sum_term;
	float difference_term;
	float scale;
	int valid;

	/* Reports 0 for a matrix that is not valid. */
	valid = vec_matrix_valid(matrix);
	if (!valid)
		return 0.0f;

	/* Computes the two terms, whose radicands are sums of two squares. */
	sum_term = sqrtf((matrix->a + matrix->d) * (matrix->a + matrix->d) +
	    (matrix->b - matrix->c) * (matrix->b - matrix->c));
	difference_term = sqrtf((matrix->a - matrix->d) * (matrix->a - matrix->d) +
	    (matrix->b + matrix->c) * (matrix->b + matrix->c));

	/* Takes half their sum, the largest singular value. */
	scale = (sum_term + difference_term) * 0.5f;

	/* Reports the factor, which is never negative. */
	return scale;
}

/*
 * Makes the matrix of translate(tx, ty).
 */
int
vec_matrix_translate(
	struct vec_matrix *out,
	float tx,
	float ty)
{
	int finite;

	/* Refuses a translation that is not a finite number. */
	finite = vec_finite(tx);
	if (!finite)
		return EINVAL;
	finite = vec_finite(ty);
	if (!finite)
		return EINVAL;

	/* Writes the identity with the translation in e and f. */
	vec_matrix_identity(out);
	out->e = tx;
	out->f = ty;

	/* Succeeded: *out is the translation. */
	return 0;
}

/*
 * Makes the matrix of scale(sx, sy).
 */
int
vec_matrix_scale(
	struct vec_matrix *out,
	float sx,
	float sy)
{
	int finite;

	/* Refuses a scale that is not a finite number. */
	finite = vec_finite(sx);
	if (!finite)
		return EINVAL;
	finite = vec_finite(sy);
	if (!finite)
		return EINVAL;

	/* Writes the identity with the scale factors on the diagonal. */
	vec_matrix_identity(out);
	out->a = sx;
	out->d = sy;

	/* Succeeded: *out is the scale. */
	return 0;
}

/*
 * Makes the matrix of rotate(degrees).
 *
 * The angle is reduced to one turn first and a multiple of 90 degrees
 * gets exact entries, so that a quarter turn maps (1, 0) to exactly
 * (0, 1).
 */
int
vec_matrix_rotate(
	struct vec_matrix *out,
	float degrees)
{
	float sine;
	float cosine;
	int error;

	/* Finds the sine and cosine of the angle, refusing an angle that is not finite. */
	error = base_sin_cos_degrees(degrees, &sine, &cosine);
	if (error != 0)
		return error;

	/* Writes the rotation: a = cos, b = sin, c = -sin, d = cos. */
	vec_matrix_identity(out);
	out->a = cosine;
	out->b = sine;
	out->c = -sine;
	out->d = cosine;

	/* Succeeded: *out is the rotation. */
	return 0;
}

/*
 * Makes the matrix of skewX(degrees).
 */
int
vec_matrix_skew_x(
	struct vec_matrix *out,
	float degrees)
{
	float tangent;
	int error;

	/* Finds the tangent of the angle, refusing an angle whose tangent is unusable. */
	error = base_tan_degrees(degrees, &tangent);
	if (error != 0)
		return error;

	/* Writes the identity with the tangent in c, which shears x by y. */
	vec_matrix_identity(out);
	out->c = tangent;

	/* Succeeded: *out is the skew along x. */
	return 0;
}

/*
 * Makes the matrix of skewY(degrees).
 */
int
vec_matrix_skew_y(
	struct vec_matrix *out,
	float degrees)
{
	float tangent;
	int error;

	/* Finds the tangent of the angle, refusing an angle whose tangent is unusable. */
	error = base_tan_degrees(degrees, &tangent);
	if (error != 0)
		return error;

	/* Writes the identity with the tangent in b, which shears y by x. */
	vec_matrix_identity(out);
	out->b = tangent;

	/* Succeeded: *out is the skew along y. */
	return 0;
}

/*
 * Grows a rectangle to hold a point.
 *
 * A rectangle that is inverted (the empty start of vec_rect_empty) becomes
 * the point's zero size rectangle.  A point that is not valid is ignored.
 */
void
vec_rect_include(
	struct vec_rect *rect,
	struct vec_point point)
{
	int valid;

	/* Ignores a point that is not a usable coordinate, so a NaN never enters a rectangle. */
	valid = vec_point_valid(point);
	if (!valid)
		return;

	/* Starts from the point when the rectangle is still the empty start. */
	if (rect->right < rect->left || rect->bottom < rect->top) {
		rect->left = point.x;
		rect->top = point.y;
		rect->right = point.x;
		rect->bottom = point.y;
		return;
	}

	/* Grows the left and right edges to hold the x of the point. */
	if (point.x < rect->left)
		rect->left = point.x;
	if (point.x > rect->right)
		rect->right = point.x;

	/* Grows the top and bottom edges to hold the y of the point. */
	if (point.y < rect->top)
		rect->top = point.y;
	if (point.y > rect->bottom)
		rect->bottom = point.y;
}

/*
 * Makes the empty start for vec_rect_include.
 */
void
vec_rect_empty(
	struct vec_rect *rect)
{
	/* Writes an inverted rectangle that any first point replaces. */
	rect->left = VEC_COORD_MAX;
	rect->top = VEC_COORD_MAX;
	rect->right = -VEC_COORD_MAX;
	rect->bottom = -VEC_COORD_MAX;
}

/*
 * Computes the angle of the vector (x, y) by the polynomial of Abramowitz
 * and Stegun 4.4.47.
 *
 * The ratio of the smaller magnitude to the larger lies in [0, 1], where
 * the polynomial holds; the quadrant is then restored by reflections.  A
 * NaN or an infinity in either component gives 0.
 */
float
vec_atan2(
	float y,
	float x)
{
	float abs_x;
	float abs_y;
	float larger;
	float smaller;
	float ratio;
	float square;
	float angle;
	int finite;

	/* Reports 0 for a NaN or an infinity, which have no usable direction. */
	finite = vec_finite(x);
	if (!finite)
		return 0.0f;
	finite = vec_finite(y);
	if (!finite)
		return 0.0f;

	/* Takes the magnitudes and orders them. */
	abs_x = x;
	if (abs_x < 0.0f)
		abs_x = -abs_x;
	abs_y = y;
	if (abs_y < 0.0f)
		abs_y = -abs_y;
	larger = abs_x;
	smaller = abs_y;
	if (abs_y > abs_x) {
		larger = abs_y;
		smaller = abs_x;
	}

	/* Reports 0 when the vector is too short to have a direction, which also guards the division. */
	if (larger < BASE_ATAN_ZERO_LIMIT)
		return 0.0f;

	/* Evaluates the polynomial in the ratio, in the order the shader does. */
	ratio = smaller / larger;
	square = ratio * ratio;
	angle = ratio * (base_atan_coefficients[0] + square * (base_atan_coefficients[1] +
	    square * (base_atan_coefficients[2] + square * (base_atan_coefficients[3] +
	    square * base_atan_coefficients[4]))));

	/* Reflects the angle about pi / 4 when the vector is steeper than 45 degrees. */
	if (abs_y > abs_x)
		angle = VEC_PI / 2.0f - angle;

	/* Reflects the angle about the y axis when the vector points to the left. */
	if (x < 0.0f)
		angle = VEC_PI - angle;

	/* Negates the angle when y is below 0, so that the angle keeps the sign of y. */
	if (y < 0.0f)
		angle = -angle;

	/* Reports the angle in (-pi, pi]. */
	return angle;
}

/*
 * Computes e^x as 2^(x * log2 e), held in range so that no infinity escapes.
 */
float
vec_exp(
	float x)
{
	float scaled;
	float value;

	/* Reports 0 for a NaN. */
	if (x != x)
		return 0.0f;

	/* Reports 0 below the range where the value is a normal float. */
	if (x < BASE_EXP_LOW)
		return 0.0f;

	/* Holds the argument at the top of the range so that the value stays finite. */
	if (x > BASE_EXP_HIGH)
		x = BASE_EXP_HIGH;

	/* Computes 2 to the power x * log2 e, the form the shader evaluates. */
	scaled = x * BASE_LOG2_E;
	value = exp2f(scaled);

	/* Reports e^x. */
	return value;
}

/*
 * Finds the sine and cosine of an angle in degrees.
 *
 * The angle is reduced to [0, 360); a multiple of 90 degrees gets the exact
 * values 0, 1 or -1, other angles go through the C library.  Returns EINVAL
 * for an angle that is not finite.
 */
static int
base_sin_cos_degrees(
	float degrees,
	float *sine,
	float *cosine)
{
	float reduced;
	float radians;
	int finite;

	/* Refuses an angle that is not a finite number. */
	finite = vec_finite(degrees);
	if (!finite)
		return EINVAL;

	/* Reduces the angle to [0, 360); fmodf is exact. */
	reduced = fmodf(degrees, 360.0f);
	if (reduced < 0.0f)
		reduced += 360.0f;
	if (reduced >= 360.0f)
		reduced = 0.0f;

	/* Gives the exact values of the four axis directions. */
	if (reduced == 0.0f) {
		*sine = 0.0f;
		*cosine = 1.0f;
		return 0;
	}
	if (reduced == 90.0f) {
		*sine = 1.0f;
		*cosine = 0.0f;
		return 0;
	}
	if (reduced == 180.0f) {
		*sine = 0.0f;
		*cosine = -1.0f;
		return 0;
	}
	if (reduced == 270.0f) {
		*sine = -1.0f;
		*cosine = 0.0f;
		return 0;
	}

	/* Takes the sine and cosine of any other angle from the C library. */
	radians = reduced * BASE_DEGREES_TO_RADIANS;
	*sine = sinf(radians);
	*cosine = cosf(radians);

	/* Succeeded: the sine and cosine are stored. */
	return 0;
}

/*
 * Finds the tangent of an angle in degrees for a skew.
 *
 * The tangent has period 180 degrees, so the angle is reduced to
 * [-90, 90].  Returns EINVAL for an angle that is not finite, for one
 * whose tangent is infinite (90 degrees) and for one whose tangent passes
 * VEC_COORD_MAX.
 */
static int
base_tan_degrees(
	float degrees,
	float *tangent)
{
	float reduced;
	float value;
	int finite;

	/* Refuses an angle that is not a finite number. */
	finite = vec_finite(degrees);
	if (!finite)
		return EINVAL;

	/* Reduces the angle to [-90, 90] by the period of the tangent. */
	reduced = fmodf(degrees, 180.0f);
	if (reduced > 90.0f)
		reduced -= 180.0f;
	if (reduced < -90.0f)
		reduced += 180.0f;

	/* Refuses the angles where the tangent is infinite. */
	if (reduced == 90.0f || reduced == -90.0f)
		return EINVAL;

	/* Takes the tangent of the reduced angle. */
	value = tanf(reduced * BASE_DEGREES_TO_RADIANS);

	/* Refuses a tangent that is not finite or is beyond the coordinate range. */
	finite = vec_finite(value);
	if (!finite)
		return EINVAL;
	if (value > VEC_COORD_MAX || value < -VEC_COORD_MAX)
		return EINVAL;

	/* Publishes the tangent. */
	*tangent = value;

	/* Succeeded: the tangent is stored. */
	return 0;
}
