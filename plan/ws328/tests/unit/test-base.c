/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of vec-base: the finite and range checks, the affine
 * matrix (composition, inverse, the stretch of a length, the transform
 * functions), the rectangle growth, and the arctangent and exponential
 * that the shaders compute the same way.  Every expected value is a hand
 * calculation or a standard value, never the output of another implementation
 * of the same function.
 */

#include "check.h"

#include <errno.h>
#include <float.h>
#include <math.h>

#include "vector/vec-base.h"

/* The ratio of a circle's circumference to its diameter, in double precision, for the expected values. */
#define TEST_PI	3.14159265358979323846

/* Builds a matrix from its six entries. */
static struct vec_matrix
make_matrix(
	float a,
	float b,
	float c,
	float d,
	float e,
	float f)
{
	struct vec_matrix matrix;

	matrix.a = a;
	matrix.b = b;
	matrix.c = c;
	matrix.d = d;
	matrix.e = e;
	matrix.f = f;
	return matrix;
}

/* Builds a point from its two components. */
static struct vec_point
make_point(
	float x,
	float y)
{
	struct vec_point point;

	point.x = x;
	point.y = y;
	return point;
}

/* Checks the finite test and the coordinate test on the edge values. */
static void
test_validity(void)
{
	struct vec_matrix matrix;
	float nan_value;
	float infinity;

	nan_value = nanf("");
	infinity = INFINITY;

	/* Finite values, including the extremes of the format. */
	CHECK_LONG(vec_finite(0.0f), 1);
	CHECK_LONG(vec_finite(-0.0f), 1);
	CHECK_LONG(vec_finite(1.0e30f), 1);
	CHECK_LONG(vec_finite(FLT_MAX), 1);
	CHECK_LONG(vec_finite(-FLT_MAX), 1);
	CHECK_LONG(vec_finite(FLT_MIN / 4.0f), 1);

	/* A NaN and the two infinities are not finite. */
	CHECK_LONG(vec_finite(nan_value), 0);
	CHECK_LONG(vec_finite(infinity), 0);
	CHECK_LONG(vec_finite(-infinity), 0);

	/* The coordinate range is +-2^24 inclusive; 2^24 + 2 is the next float above it. */
	CHECK_LONG(vec_point_valid(make_point(0.0f, 0.0f)), 1);
	CHECK_LONG(vec_point_valid(make_point(16777216.0f, -16777216.0f)), 1);
	CHECK_LONG(vec_point_valid(make_point(16777218.0f, 0.0f)), 0);
	CHECK_LONG(vec_point_valid(make_point(0.0f, -16777218.0f)), 0);
	CHECK_LONG(vec_point_valid(make_point(nan_value, 0.0f)), 0);
	CHECK_LONG(vec_point_valid(make_point(0.0f, infinity)), 0);

	/* A matrix is valid when all six entries are in range; one bad entry makes it invalid. */
	matrix = make_matrix(1.0f, 0.0f, 0.0f, 1.0f, 16777216.0f, -16777216.0f);
	CHECK_LONG(vec_matrix_valid(&matrix), 1);
	matrix.a = nan_value;
	CHECK_LONG(vec_matrix_valid(&matrix), 0);
	matrix = make_matrix(1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
	matrix.b = 1.0e9f;
	CHECK_LONG(vec_matrix_valid(&matrix), 0);
	matrix.b = 0.0f;
	matrix.f = infinity;
	CHECK_LONG(vec_matrix_valid(&matrix), 0);
}

/* Checks the identity, apply and apply_vector on values worked by hand. */
static void
test_apply(void)
{
	struct vec_matrix matrix;
	struct vec_point result;

	/* The identity leaves a point alone. */
	vec_matrix_identity(&matrix);
	CHECK_NEAR(matrix.a, 1.0, 0.0);
	CHECK_NEAR(matrix.b, 0.0, 0.0);
	CHECK_NEAR(matrix.c, 0.0, 0.0);
	CHECK_NEAR(matrix.d, 1.0, 0.0);
	CHECK_NEAR(matrix.e, 0.0, 0.0);
	CHECK_NEAR(matrix.f, 0.0, 0.0);
	result = vec_matrix_apply(&matrix, make_point(3.5f, -2.25f));
	CHECK_NEAR(result.x, 3.5, 0.0);
	CHECK_NEAR(result.y, -2.25, 0.0);

	/*
	 * M = [2 -1 10; 0.5 3 20] on (1, 2): x' = 2 * 1 + (-1) * 2 + 10 = 10,
	 * y' = 0.5 * 1 + 3 * 2 + 20 = 26.5.  Without the translation: 0 and 6.5.
	 */
	matrix = make_matrix(2.0f, 0.5f, -1.0f, 3.0f, 10.0f, 20.0f);
	result = vec_matrix_apply(&matrix, make_point(1.0f, 2.0f));
	CHECK_NEAR(result.x, 10.0, 1e-6);
	CHECK_NEAR(result.y, 26.5, 1e-6);
	result = vec_matrix_apply_vector(&matrix, make_point(1.0f, 2.0f));
	CHECK_NEAR(result.x, 0.0, 1e-6);
	CHECK_NEAR(result.y, 6.5, 1e-6);
}

/* Checks the order of composition and the aliasing of the output. */
static void
test_multiply(void)
{
	struct vec_matrix scale;
	struct vec_matrix translate;
	struct vec_matrix rotate;
	struct vec_matrix out;
	struct vec_matrix left;
	struct vec_matrix right;
	struct vec_point result;
	struct vec_point step;
	struct vec_point expected;
	uint32_t seed;
	int i;

	CHECK_LONG(vec_matrix_scale(&scale, 2.0f, 3.0f), 0);
	CHECK_LONG(vec_matrix_translate(&translate, 10.0f, 20.0f), 0);
	CHECK_LONG(vec_matrix_rotate(&rotate, 90.0f), 0);

	/* Scale first, then translate: (1, 1) -> (2, 3) -> (12, 23). */
	vec_matrix_multiply(&out, &scale, &translate);
	result = vec_matrix_apply(&out, make_point(1.0f, 1.0f));
	CHECK_NEAR(result.x, 12.0, 1e-6);
	CHECK_NEAR(result.y, 23.0, 1e-6);

	/* Translate first, then scale: (1, 1) -> (11, 21) -> (22, 63). */
	vec_matrix_multiply(&out, &translate, &scale);
	result = vec_matrix_apply(&out, make_point(1.0f, 1.0f));
	CHECK_NEAR(result.x, 22.0, 1e-6);
	CHECK_NEAR(result.y, 63.0, 1e-6);

	/* Rotate by 90 degrees first, then translate by (10, 20): (1, 0) -> (0, 1) -> (10, 21). */
	vec_matrix_multiply(&out, &rotate, &translate);
	result = vec_matrix_apply(&out, make_point(1.0f, 0.0f));
	CHECK_NEAR(result.x, 10.0, 1e-5);
	CHECK_NEAR(result.y, 21.0, 1e-5);

	/* The output may be the first input: scale then translate stored over scale. */
	out = scale;
	vec_matrix_multiply(&out, &out, &translate);
	result = vec_matrix_apply(&out, make_point(1.0f, 1.0f));
	CHECK_NEAR(result.x, 12.0, 1e-6);
	CHECK_NEAR(result.y, 23.0, 1e-6);

	/* The output may be the second input: scale then translate stored over translate. */
	out = translate;
	vec_matrix_multiply(&out, &scale, &out);
	result = vec_matrix_apply(&out, make_point(1.0f, 1.0f));
	CHECK_NEAR(result.x, 12.0, 1e-6);
	CHECK_NEAR(result.y, 23.0, 1e-6);

	/* A random composition equals the two steps taken one after the other. */
	seed = 12345;
	for (i = 0; i < 1000; i++) {
		left = make_matrix(check_random_float(&seed, -3.0f, 3.0f), check_random_float(&seed, -3.0f, 3.0f),
		    check_random_float(&seed, -3.0f, 3.0f), check_random_float(&seed, -3.0f, 3.0f),
		    check_random_float(&seed, -50.0f, 50.0f), check_random_float(&seed, -50.0f, 50.0f));
		right = make_matrix(check_random_float(&seed, -3.0f, 3.0f), check_random_float(&seed, -3.0f, 3.0f),
		    check_random_float(&seed, -3.0f, 3.0f), check_random_float(&seed, -3.0f, 3.0f),
		    check_random_float(&seed, -50.0f, 50.0f), check_random_float(&seed, -50.0f, 50.0f));
		vec_matrix_multiply(&out, &left, &right);
		step = vec_matrix_apply(&left, make_point(7.0f, -4.0f));
		expected = vec_matrix_apply(&right, step);
		result = vec_matrix_apply(&out, make_point(7.0f, -4.0f));
		CHECK_NEAR(result.x, expected.x, 1e-3);
		CHECK_NEAR(result.y, expected.y, 1e-3);
	}
}

/* Checks the inverse on a matrix worked by hand, on random ones, and on every refusal. */
static void
test_invert(void)
{
	struct vec_matrix matrix;
	struct vec_matrix inverse;
	struct vec_matrix product;
	struct vec_matrix sentinel;
	struct vec_point result;
	uint32_t seed;
	float nan_value;
	int i;

	nan_value = nanf("");

	/*
	 * M = [2 0 6; 0 4 8]: x' = 2x + 6, y' = 4y + 8, so the inverse is
	 * x = (x' - 6) / 2 = 0.5 x' - 3 and y = 0.25 y' - 2.
	 */
	matrix = make_matrix(2.0f, 0.0f, 0.0f, 4.0f, 6.0f, 8.0f);
	CHECK_LONG(vec_matrix_invert(&inverse, &matrix), 0);
	CHECK_NEAR(inverse.a, 0.5, 1e-7);
	CHECK_NEAR(inverse.b, 0.0, 1e-7);
	CHECK_NEAR(inverse.c, 0.0, 1e-7);
	CHECK_NEAR(inverse.d, 0.25, 1e-7);
	CHECK_NEAR(inverse.e, -3.0, 1e-6);
	CHECK_NEAR(inverse.f, -2.0, 1e-6);

	/*
	 * A rotation by 90 degrees with a translation: x' = -y + 5, y' = x + 7;
	 * so x = y' - 7 and y = -(x' - 5) = -x' + 5.
	 */
	matrix = make_matrix(0.0f, 1.0f, -1.0f, 0.0f, 5.0f, 7.0f);
	CHECK_LONG(vec_matrix_invert(&inverse, &matrix), 0);
	CHECK_NEAR(inverse.a, 0.0, 1e-7);
	CHECK_NEAR(inverse.b, -1.0, 1e-7);
	CHECK_NEAR(inverse.c, 1.0, 1e-7);
	CHECK_NEAR(inverse.d, 0.0, 1e-7);
	CHECK_NEAR(inverse.e, -7.0, 1e-6);
	CHECK_NEAR(inverse.f, 5.0, 1e-6);

	/* The output may be the input itself. */
	matrix = make_matrix(2.0f, 0.0f, 0.0f, 4.0f, 6.0f, 8.0f);
	CHECK_LONG(vec_matrix_invert(&matrix, &matrix), 0);
	CHECK_NEAR(matrix.a, 0.5, 1e-7);
	CHECK_NEAR(matrix.f, -2.0, 1e-6);

	/* A random matrix times its inverse maps points back to themselves. */
	seed = 999;
	for (i = 0; i < 1000; i++) {
		matrix = make_matrix(check_random_float(&seed, -4.0f, 4.0f), check_random_float(&seed, -4.0f, 4.0f),
		    check_random_float(&seed, -4.0f, 4.0f), check_random_float(&seed, -4.0f, 4.0f),
		    check_random_float(&seed, -100.0f, 100.0f), check_random_float(&seed, -100.0f, 100.0f));
		if (vec_matrix_invert(&inverse, &matrix) != 0)
			continue;
		vec_matrix_multiply(&product, &matrix, &inverse);
		result = vec_matrix_apply(&product, make_point(13.0f, -21.0f));
		CHECK_NEAR(result.x, 13.0, 0.05);
		CHECK_NEAR(result.y, -21.0, 0.05);
	}

	/* A singular matrix (second row twice the first) is refused and the output is left alone. */
	sentinel = make_matrix(9.0f, 8.0f, 7.0f, 6.0f, 5.0f, 4.0f);
	inverse = sentinel;
	matrix = make_matrix(1.0f, 2.0f, 2.0f, 4.0f, 0.0f, 0.0f);
	CHECK_LONG(vec_matrix_invert(&inverse, &matrix), EINVAL);
	CHECK(memcmp(&inverse, &sentinel, sizeof(inverse)) == 0);

	/* A determinant of 1e-14 is under the limit of 1e-12. */
	matrix = make_matrix(1.0e-7f, 0.0f, 0.0f, 1.0e-7f, 0.0f, 0.0f);
	CHECK_LONG(vec_matrix_invert(&inverse, &matrix), EINVAL);
	CHECK(memcmp(&inverse, &sentinel, sizeof(inverse)) == 0);

	/* A determinant of 1e-10 is over the limit and inverts to 1e5 on the diagonal. */
	matrix = make_matrix(1.0e-5f, 0.0f, 0.0f, 1.0e-5f, 0.0f, 0.0f);
	CHECK_LONG(vec_matrix_invert(&inverse, &matrix), 0);
	CHECK_NEAR(inverse.a / 1.0e5, 1.0, 1e-5);

	/* A NaN, an infinity and an entry beyond the coordinate range are refused. */
	matrix = make_matrix(nan_value, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
	CHECK_LONG(vec_matrix_invert(&inverse, &matrix), EINVAL);
	matrix = make_matrix(1.0f, 0.0f, 0.0f, INFINITY, 0.0f, 0.0f);
	CHECK_LONG(vec_matrix_invert(&inverse, &matrix), EINVAL);
	matrix = make_matrix(1.0f, 0.0f, 0.0f, 1.0f, 1.0e9f, 0.0f);
	CHECK_LONG(vec_matrix_invert(&inverse, &matrix), EINVAL);

	/*
	 * An inverse that leaves the coordinate range is refused: the scale
	 * 1e-3 inverts to 1e3, but the translation 20000 inverts to
	 * -20000 / 1e-3 = -2e7, beyond 2^24.
	 */
	inverse = sentinel;
	matrix = make_matrix(1.0e-3f, 0.0f, 0.0f, 1.0e-3f, 20000.0f, 0.0f);
	CHECK_LONG(vec_matrix_invert(&inverse, &matrix), EINVAL);
	CHECK(memcmp(&inverse, &sentinel, sizeof(inverse)) == 0);
}

/* Checks the largest stretch against hand-computed singular values. */
static void
test_scale_max(void)
{
	struct vec_matrix matrix;
	struct vec_matrix rotate;
	struct vec_matrix scale;
	struct vec_matrix product;

	/* The identity stretches by 1, a scale by its larger factor in magnitude. */
	vec_matrix_identity(&matrix);
	CHECK_NEAR(vec_matrix_scale_max(&matrix), 1.0, 1e-6);
	CHECK_LONG(vec_matrix_scale(&matrix, 3.0f, 2.0f), 0);
	CHECK_NEAR(vec_matrix_scale_max(&matrix), 3.0, 1e-6);
	CHECK_LONG(vec_matrix_scale(&matrix, -5.0f, 2.0f), 0);
	CHECK_NEAR(vec_matrix_scale_max(&matrix), 5.0, 1e-6);

	/* A translation does not stretch. */
	CHECK_LONG(vec_matrix_translate(&matrix, 100.0f, -50.0f), 0);
	CHECK_NEAR(vec_matrix_scale_max(&matrix), 1.0, 1e-6);

	/* A rotation by 30 degrees does not stretch. */
	CHECK_LONG(vec_matrix_rotate(&matrix, 30.0f), 0);
	CHECK_NEAR(vec_matrix_scale_max(&matrix), 1.0, 1e-6);

	/* A scale by (4, 1) followed by a rotation keeps its largest stretch of 4. */
	CHECK_LONG(vec_matrix_scale(&scale, 4.0f, 1.0f), 0);
	CHECK_LONG(vec_matrix_rotate(&rotate, 37.0f), 0);
	vec_matrix_multiply(&product, &scale, &rotate);
	CHECK_NEAR(vec_matrix_scale_max(&product), 4.0, 1e-5);

	/*
	 * The shear [1 1; 0 1] has M^T M = [1 1; 1 2], whose eigenvalues are
	 * (3 +- sqrt(5)) / 2, so the largest singular value is
	 * sqrt((3 + sqrt(5)) / 2) = (1 + sqrt(5)) / 2 = 1.6180340.
	 */
	matrix = make_matrix(1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f);
	CHECK_NEAR(vec_matrix_scale_max(&matrix), 1.6180340, 1e-6);

	/* The shear along y is the transpose and has the same singular values. */
	matrix = make_matrix(1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f);
	CHECK_NEAR(vec_matrix_scale_max(&matrix), 1.6180340, 1e-6);

	/* A mirror with a rotation: [0 2; 3 0] has singular values 3 and 2. */
	matrix = make_matrix(0.0f, 3.0f, 2.0f, 0.0f, 0.0f, 0.0f);
	CHECK_NEAR(vec_matrix_scale_max(&matrix), 3.0, 1e-6);

	/* A singular matrix [1 2; 2 4] has the single singular value sqrt(1 + 4 + 4 + 16) = 5. */
	matrix = make_matrix(1.0f, 2.0f, 2.0f, 4.0f, 0.0f, 0.0f);
	CHECK_NEAR(vec_matrix_scale_max(&matrix), 5.0, 1e-5);

	/* The zero matrix stretches by 0, and an invalid matrix reports 0. */
	matrix = make_matrix(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
	CHECK_NEAR(vec_matrix_scale_max(&matrix), 0.0, 0.0);
	matrix = make_matrix(nanf(""), 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
	CHECK_NEAR(vec_matrix_scale_max(&matrix), 0.0, 0.0);
	matrix = make_matrix(1.0f, 0.0f, 0.0f, 1.0f, 1.0e9f, 0.0f);
	CHECK_NEAR(vec_matrix_scale_max(&matrix), 0.0, 0.0);
}

/* Checks the transform functions on values worked by hand and every refusal. */
static void
test_transforms(void)
{
	struct vec_matrix matrix;
	struct vec_matrix sentinel;
	struct vec_point result;
	float nan_value;

	nan_value = nanf("");

	/* translate(3, 4) maps (1, 1) to (4, 5). */
	CHECK_LONG(vec_matrix_translate(&matrix, 3.0f, 4.0f), 0);
	result = vec_matrix_apply(&matrix, make_point(1.0f, 1.0f));
	CHECK_NEAR(result.x, 4.0, 0.0);
	CHECK_NEAR(result.y, 5.0, 0.0);

	/* scale(2, -3) maps (1, 1) to (2, -3). */
	CHECK_LONG(vec_matrix_scale(&matrix, 2.0f, -3.0f), 0);
	result = vec_matrix_apply(&matrix, make_point(1.0f, 1.0f));
	CHECK_NEAR(result.x, 2.0, 0.0);
	CHECK_NEAR(result.y, -3.0, 0.0);

	/* rotate(90): a = 0, b = 1, c = -1, d = 0 exactly, and (1, 0) goes to (0, 1). */
	CHECK_LONG(vec_matrix_rotate(&matrix, 90.0f), 0);
	CHECK_NEAR(matrix.a, 0.0, 0.0);
	CHECK_NEAR(matrix.b, 1.0, 0.0);
	CHECK_NEAR(matrix.c, -1.0, 0.0);
	CHECK_NEAR(matrix.d, 0.0, 0.0);
	result = vec_matrix_apply(&matrix, make_point(1.0f, 0.0f));
	CHECK_NEAR(result.x, 0.0, 0.0);
	CHECK_NEAR(result.y, 1.0, 0.0);

	/* rotate(-90) sends (1, 0) to (0, -1); rotate(180) to (-1, 0); rotate(270) to (0, -1). */
	CHECK_LONG(vec_matrix_rotate(&matrix, -90.0f), 0);
	result = vec_matrix_apply(&matrix, make_point(1.0f, 0.0f));
	CHECK_NEAR(result.x, 0.0, 0.0);
	CHECK_NEAR(result.y, -1.0, 0.0);
	CHECK_LONG(vec_matrix_rotate(&matrix, 180.0f), 0);
	result = vec_matrix_apply(&matrix, make_point(1.0f, 0.0f));
	CHECK_NEAR(result.x, -1.0, 0.0);
	CHECK_NEAR(result.y, 0.0, 0.0);
	CHECK_LONG(vec_matrix_rotate(&matrix, 270.0f), 0);
	result = vec_matrix_apply(&matrix, make_point(1.0f, 0.0f));
	CHECK_NEAR(result.x, 0.0, 0.0);
	CHECK_NEAR(result.y, -1.0, 0.0);

	/* A whole turn and a turn and a quarter are the identity and the quarter. */
	CHECK_LONG(vec_matrix_rotate(&matrix, 360.0f), 0);
	CHECK_NEAR(matrix.a, 1.0, 0.0);
	CHECK_NEAR(matrix.b, 0.0, 0.0);
	CHECK_LONG(vec_matrix_rotate(&matrix, 450.0f), 0);
	CHECK_NEAR(matrix.a, 0.0, 0.0);
	CHECK_NEAR(matrix.b, 1.0, 0.0);

	/* rotate(45): cos = sin = sqrt(1/2) = 0.70710678; rotate(30): cos = 0.8660254, sin = 0.5. */
	CHECK_LONG(vec_matrix_rotate(&matrix, 45.0f), 0);
	CHECK_NEAR(matrix.a, 0.70710678, 1e-6);
	CHECK_NEAR(matrix.b, 0.70710678, 1e-6);
	CHECK_NEAR(matrix.c, -0.70710678, 1e-6);
	CHECK_NEAR(matrix.d, 0.70710678, 1e-6);
	CHECK_LONG(vec_matrix_rotate(&matrix, 30.0f), 0);
	CHECK_NEAR(matrix.a, 0.8660254, 1e-6);
	CHECK_NEAR(matrix.b, 0.5, 1e-6);
	CHECK_LONG(vec_matrix_rotate(&matrix, -30.0f), 0);
	CHECK_NEAR(matrix.b, -0.5, 1e-6);

	/* skew_x(45): c = tan 45 = 1; skew_x(30): c = 0.57735027; the rest is the identity. */
	CHECK_LONG(vec_matrix_skew_x(&matrix, 45.0f), 0);
	CHECK_NEAR(matrix.a, 1.0, 0.0);
	CHECK_NEAR(matrix.b, 0.0, 0.0);
	CHECK_NEAR(matrix.c, 1.0, 1e-6);
	CHECK_NEAR(matrix.d, 1.0, 0.0);
	CHECK_LONG(vec_matrix_skew_x(&matrix, 30.0f), 0);
	CHECK_NEAR(matrix.c, 0.57735027, 1e-6);
	CHECK_LONG(vec_matrix_skew_x(&matrix, -45.0f), 0);
	CHECK_NEAR(matrix.c, -1.0, 1e-6);

	/* skew_y(45): b = 1 and the others are the identity's; skew_y(135) has tan = -1. */
	CHECK_LONG(vec_matrix_skew_y(&matrix, 45.0f), 0);
	CHECK_NEAR(matrix.a, 1.0, 0.0);
	CHECK_NEAR(matrix.b, 1.0, 1e-6);
	CHECK_NEAR(matrix.c, 0.0, 0.0);
	CHECK_NEAR(matrix.d, 1.0, 0.0);
	CHECK_LONG(vec_matrix_skew_y(&matrix, 135.0f), 0);
	CHECK_NEAR(matrix.b, -1.0, 1e-5);

	/* skew by 0 and by 180 degrees is the identity. */
	CHECK_LONG(vec_matrix_skew_x(&matrix, 0.0f), 0);
	CHECK_NEAR(matrix.c, 0.0, 0.0);
	CHECK_LONG(vec_matrix_skew_x(&matrix, 180.0f), 0);
	CHECK_NEAR(matrix.c, 0.0, 0.0);

	/* An angle near 90 degrees whose tangent (572957.8) is in range is accepted. */
	CHECK_LONG(vec_matrix_skew_x(&matrix, 89.9999f), 0);
	CHECK(matrix.c > 5.0e5f && matrix.c < 6.0e5f);

	/* 90, -90 and 270 degrees have no finite tangent: refused, output unchanged. */
	vec_matrix_identity(&sentinel);
	sentinel.e = 77.0f;
	matrix = sentinel;
	CHECK_LONG(vec_matrix_skew_x(&matrix, 90.0f), EINVAL);
	CHECK_LONG(vec_matrix_skew_x(&matrix, -90.0f), EINVAL);
	CHECK_LONG(vec_matrix_skew_y(&matrix, 270.0f), EINVAL);
	CHECK_LONG(vec_matrix_skew_y(&matrix, 90.0f), EINVAL);
	CHECK(memcmp(&matrix, &sentinel, sizeof(matrix)) == 0);

	/* A NaN or an infinity in any argument is refused and the output is left alone. */
	CHECK_LONG(vec_matrix_translate(&matrix, nan_value, 0.0f), EINVAL);
	CHECK_LONG(vec_matrix_translate(&matrix, 0.0f, INFINITY), EINVAL);
	CHECK_LONG(vec_matrix_scale(&matrix, nan_value, 1.0f), EINVAL);
	CHECK_LONG(vec_matrix_scale(&matrix, 1.0f, -INFINITY), EINVAL);
	CHECK_LONG(vec_matrix_rotate(&matrix, nan_value), EINVAL);
	CHECK_LONG(vec_matrix_rotate(&matrix, INFINITY), EINVAL);
	CHECK_LONG(vec_matrix_skew_x(&matrix, nan_value), EINVAL);
	CHECK_LONG(vec_matrix_skew_y(&matrix, INFINITY), EINVAL);
	CHECK(memcmp(&matrix, &sentinel, sizeof(matrix)) == 0);
}

/* Checks the growth of a rectangle by points. */
static void
test_rect(void)
{
	struct vec_rect rect;

	/* The empty start is the inverted range. */
	vec_rect_empty(&rect);
	CHECK_NEAR(rect.left, 16777216.0, 0.0);
	CHECK_NEAR(rect.top, 16777216.0, 0.0);
	CHECK_NEAR(rect.right, -16777216.0, 0.0);
	CHECK_NEAR(rect.bottom, -16777216.0, 0.0);

	/* The first point makes a zero size rectangle at the point. */
	vec_rect_include(&rect, make_point(3.0f, 4.0f));
	CHECK_NEAR(rect.left, 3.0, 0.0);
	CHECK_NEAR(rect.top, 4.0, 0.0);
	CHECK_NEAR(rect.right, 3.0, 0.0);
	CHECK_NEAR(rect.bottom, 4.0, 0.0);

	/* A point above and to the right grows the right and top edges only. */
	vec_rect_include(&rect, make_point(8.0f, 1.0f));
	CHECK_NEAR(rect.left, 3.0, 0.0);
	CHECK_NEAR(rect.top, 1.0, 0.0);
	CHECK_NEAR(rect.right, 8.0, 0.0);
	CHECK_NEAR(rect.bottom, 4.0, 0.0);

	/* A point inside changes nothing, and one below and to the left grows the other two edges. */
	vec_rect_include(&rect, make_point(5.0f, 2.0f));
	CHECK_NEAR(rect.left, 3.0, 0.0);
	CHECK_NEAR(rect.right, 8.0, 0.0);
	vec_rect_include(&rect, make_point(-2.0f, 9.0f));
	CHECK_NEAR(rect.left, -2.0, 0.0);
	CHECK_NEAR(rect.top, 1.0, 0.0);
	CHECK_NEAR(rect.right, 8.0, 0.0);
	CHECK_NEAR(rect.bottom, 9.0, 0.0);

	/* A point with a NaN or beyond the range is ignored. */
	vec_rect_include(&rect, make_point(nanf(""), 0.0f));
	vec_rect_include(&rect, make_point(0.0f, 1.0e20f));
	CHECK_NEAR(rect.left, -2.0, 0.0);
	CHECK_NEAR(rect.top, 1.0, 0.0);
	CHECK_NEAR(rect.right, 8.0, 0.0);
	CHECK_NEAR(rect.bottom, 9.0, 0.0);

	/* A NaN as the first point leaves the empty start empty. */
	vec_rect_empty(&rect);
	vec_rect_include(&rect, make_point(0.0f, nanf("")));
	CHECK(rect.right < rect.left);
}

/* Checks the arctangent on the eight directions, the special cases, and against the exact one. */
static void
test_atan2(void)
{
	static const double directions[8][3] = {
		/* y, x, angle: the eight directions, from the positive x axis through the lower half. */
		{ 0.0, 1.0, 0.0 },
		{ 1.0, 1.0, TEST_PI / 4.0 },
		{ 1.0, 0.0, TEST_PI / 2.0 },
		{ 1.0, -1.0, 3.0 * TEST_PI / 4.0 },
		{ 0.0, -1.0, TEST_PI },
		{ -1.0, -1.0, -3.0 * TEST_PI / 4.0 },
		{ -1.0, 0.0, -TEST_PI / 2.0 },
		{ -1.0, 1.0, -TEST_PI / 4.0 }
	};
	uint32_t seed;
	double exact;
	float x;
	float y;
	int i;

	/* The eight unit directions are within 2e-5 of their exact angles. */
	for (i = 0; i < 8; i++) {
		CHECK_NEAR(vec_atan2((float)directions[i][0], (float)directions[i][1]), directions[i][2], 2e-5);
	}

	/* The angle of (-1, 0) is pi, taken from the +0 side of y. */
	CHECK_NEAR(vec_atan2(0.0f, -1.0f), TEST_PI, 2e-6);
	CHECK(vec_atan2(0.0f, -1.0f) > 3.14f);

	/* The length does not matter: a short and a long vector in the same direction. */
	CHECK_NEAR(vec_atan2(3.0e-10f, 3.0e-10f), TEST_PI / 4.0, 2e-5);
	CHECK_NEAR(vec_atan2(5.0e7f, 5.0e7f), TEST_PI / 4.0, 2e-5);

	/* The zero vector, a vector under 1e-20, and a NaN or infinity give 0. */
	CHECK_NEAR(vec_atan2(0.0f, 0.0f), 0.0, 0.0);
	CHECK_NEAR(vec_atan2(1.0e-21f, -1.0e-21f), 0.0, 0.0);
	CHECK_NEAR(vec_atan2(nanf(""), 1.0f), 0.0, 0.0);
	CHECK_NEAR(vec_atan2(1.0f, nanf("")), 0.0, 0.0);
	CHECK_NEAR(vec_atan2(INFINITY, 1.0f), 0.0, 0.0);
	CHECK_NEAR(vec_atan2(1.0f, -INFINITY), 0.0, 0.0);

	/* A hundred thousand random vectors are within 2e-5 of the double precision arctangent. */
	seed = 424242;
	for (i = 0; i < 100000; i++) {
		x = check_random_float(&seed, -100.0f, 100.0f);
		y = check_random_float(&seed, -100.0f, 100.0f);
		exact = atan2((double)y, (double)x);
		CHECK_NEAR(vec_atan2(y, x), exact, 2e-5);
	}
}

/* Checks the exponential at known values, at the edges and against the exact one. */
static void
test_exp(void)
{
	uint32_t seed;
	double exact;
	float x;
	float value;
	int i;

	/* e^0 = 1 and e^1 = 2.718281828 within a relative 1e-6. */
	CHECK_NEAR(vec_exp(0.0f), 1.0, 1e-6);
	CHECK_NEAR(vec_exp(1.0f) / 2.718281828459045, 1.0, 1e-6);

	/* e^-1 = 0.367879441 and e^10 = 22026.4658. */
	CHECK_NEAR(vec_exp(-1.0f) / 0.36787944117144233, 1.0, 1e-6);
	CHECK_NEAR(vec_exp(10.0f) / 22026.465794806718, 1.0, 1e-5);

	/* Under -87 and for a NaN the value is 0. */
	CHECK_NEAR(vec_exp(-87.5f), 0.0, 0.0);
	CHECK_NEAR(vec_exp(-1.0e30f), 0.0, 0.0);
	CHECK_NEAR(vec_exp(-INFINITY), 0.0, 0.0);
	CHECK_NEAR(vec_exp(nanf("")), 0.0, 0.0);

	/* At -87 the value is a small positive number, about 1.6e-38. */
	value = vec_exp(-87.0f);
	CHECK(value > 0.0f && value < 1.0e-37f);

	/* At 88 the value is finite, about 1.65e38, and above 88 it is held at that value. */
	value = vec_exp(88.0f);
	CHECK(vec_finite(value));
	CHECK_NEAR(value / 1.6516362549940018e38, 1.0, 1e-4);
	CHECK_NEAR(vec_exp(89.0f), (double)value, 0.0);
	CHECK_NEAR(vec_exp(1.0e30f), (double)value, 0.0);
	CHECK_NEAR(vec_exp(INFINITY), (double)value, 0.0);

	/* Random arguments agree with the double precision exponential within a relative 1e-5. */
	seed = 77;
	for (i = 0; i < 100000; i++) {
		x = check_random_float(&seed, -80.0f, 80.0f);
		exact = exp((double)x);
		CHECK_NEAR(vec_exp(x) / exact, 1.0, 1e-5);
	}
}

int
main(void)
{
	/* Runs every group of checks. */
	test_validity();
	test_apply();
	test_multiply();
	test_invert();
	test_scale_max();
	test_transforms();
	test_rect();
	test_atan2();
	test_exp();

	/* Reports the result. */
	return check_finish("base");
}
