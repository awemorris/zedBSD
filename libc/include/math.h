/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_MATH_H
#define ZEDBSD_MATH_H

typedef float float_t;
typedef double double_t;

extern int signgam;

#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define HUGE_VALF INFINITY
#define HUGE_VAL ((double)INFINITY)
#define HUGE_VALL ((long double)INFINITY)

#define MATH_ERRNO 1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERREXCEPT

#define FP_ILOGBNAN (-1 - 0x7fffffff)
#define FP_ILOGB0 FP_ILOGBNAN
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4

#define M_E        2.71828182845904523536
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

int __fpclassify(double);
int __fpclassifyf(float);
int __fpclassifyl(long double);
int __signbit(double);
int __signbitf(float);
int __signbitl(long double);

static __inline unsigned int
__zedbsd_float_bits(float value)
{
	union { float value; unsigned int bits; } shape = { value };
	return shape.bits;
}

static __inline unsigned long long
__zedbsd_double_bits(double value)
{
	union { double value; unsigned long long bits; } shape = { value };
	return shape.bits;
}

#define fpclassify(x) (sizeof(x) == sizeof(float) ? __fpclassifyf(x) : \
	(sizeof(x) == sizeof(double) ? __fpclassify(x) : __fpclassifyl(x)))
#define isinf(x) (sizeof(x) == sizeof(float) ? \
	((__zedbsd_float_bits(x) & 0x7fffffffU) == 0x7f800000U) : \
	(sizeof(x) == sizeof(double) ? \
	((__zedbsd_double_bits(x) & 0x7fffffffffffffffULL) == \
	0x7ff0000000000000ULL) : __fpclassifyl(x) == FP_INFINITE))
#define isnan(x) (sizeof(x) == sizeof(float) ? \
	((__zedbsd_float_bits(x) & 0x7fffffffU) > 0x7f800000U) : \
	(sizeof(x) == sizeof(double) ? \
	((__zedbsd_double_bits(x) & 0x7fffffffffffffffULL) > \
	0x7ff0000000000000ULL) : __fpclassifyl(x) == FP_NAN))
#define isfinite(x) (sizeof(x) == sizeof(float) ? \
	((__zedbsd_float_bits(x) & 0x7fffffffU) < 0x7f800000U) : \
	(sizeof(x) == sizeof(double) ? \
	((__zedbsd_double_bits(x) & 0x7fffffffffffffffULL) < \
	0x7ff0000000000000ULL) : __fpclassifyl(x) > FP_INFINITE))
#define isnormal(x) (fpclassify(x) == FP_NORMAL)
#define signbit(x) (sizeof(x) == sizeof(float) ? \
	(int)(__zedbsd_float_bits(x) >> 31) : (sizeof(x) == sizeof(double) ? \
	(int)(__zedbsd_double_bits(x) >> 63) : __signbitl(x)))
#define isunordered(x, y) (isnan(x) || isnan(y))
#define isgreater(x, y) (!isunordered((x), (y)) && (x) > (y))
#define isgreaterequal(x, y) (!isunordered((x), (y)) && (x) >= (y))
#define isless(x, y) (!isunordered((x), (y)) && (x) < (y))
#define islessequal(x, y) (!isunordered((x), (y)) && (x) <= (y))
#define islessgreater(x, y) (!isunordered((x), (y)) && (x) != (y))

#define ZEDBSD_MATH_UNARY(name) \
	double name(double); float name##f(float); long double name##l(long double)
#define ZEDBSD_MATH_BINARY(name) \
	double name(double, double); float name##f(float, float); \
	long double name##l(long double, long double)

ZEDBSD_MATH_UNARY(acos);
ZEDBSD_MATH_UNARY(acosh);
ZEDBSD_MATH_UNARY(asin);
ZEDBSD_MATH_UNARY(asinh);
ZEDBSD_MATH_UNARY(atan);
ZEDBSD_MATH_UNARY(atanh);
ZEDBSD_MATH_UNARY(cbrt);
ZEDBSD_MATH_UNARY(ceil);
ZEDBSD_MATH_UNARY(cos);
ZEDBSD_MATH_UNARY(cosh);
ZEDBSD_MATH_UNARY(erf);
ZEDBSD_MATH_UNARY(erfc);
ZEDBSD_MATH_UNARY(exp);
ZEDBSD_MATH_UNARY(exp2);
ZEDBSD_MATH_UNARY(expm1);
ZEDBSD_MATH_UNARY(fabs);
ZEDBSD_MATH_UNARY(floor);
ZEDBSD_MATH_UNARY(lgamma);
ZEDBSD_MATH_UNARY(log);
ZEDBSD_MATH_UNARY(log10);
ZEDBSD_MATH_UNARY(log1p);
ZEDBSD_MATH_UNARY(log2);
ZEDBSD_MATH_UNARY(logb);
ZEDBSD_MATH_UNARY(nearbyint);
ZEDBSD_MATH_UNARY(rint);
ZEDBSD_MATH_UNARY(round);
ZEDBSD_MATH_UNARY(sin);
ZEDBSD_MATH_UNARY(sinh);
ZEDBSD_MATH_UNARY(sqrt);
ZEDBSD_MATH_UNARY(tan);
ZEDBSD_MATH_UNARY(tanh);
ZEDBSD_MATH_UNARY(tgamma);
ZEDBSD_MATH_UNARY(trunc);

ZEDBSD_MATH_BINARY(atan2);
ZEDBSD_MATH_BINARY(copysign);
ZEDBSD_MATH_BINARY(fdim);
ZEDBSD_MATH_BINARY(fmax);
ZEDBSD_MATH_BINARY(fmin);
ZEDBSD_MATH_BINARY(fmod);
ZEDBSD_MATH_BINARY(hypot);
ZEDBSD_MATH_BINARY(nextafter);
ZEDBSD_MATH_BINARY(pow);
ZEDBSD_MATH_BINARY(remainder);

double fma(double, double, double);
float fmaf(float, float, float);
long double fmal(long double, long double, long double);
double frexp(double, int *);
float frexpf(float, int *);
long double frexpl(long double, int *);
int ilogb(double);
int ilogbf(float);
int ilogbl(long double);
double ldexp(double, int);
float ldexpf(float, int);
long double ldexpl(long double, int);
long long llrint(double);
long long llrintf(float);
long long llrintl(long double);
long long llround(double);
long long llroundf(float);
long long llroundl(long double);
long lrint(double);
long lrintf(float);
long lrintl(long double);
long lround(double);
long lroundf(float);
long lroundl(long double);
double modf(double, double *);
float modff(float, float *);
long double modfl(long double, long double *);
double nan(const char *);
float nanf(const char *);
long double nanl(const char *);
double nexttoward(double, long double);
float nexttowardf(float, long double);
long double nexttowardl(long double, long double);
double remquo(double, double, int *);
float remquof(float, float, int *);
long double remquol(long double, long double, int *);
double scalbln(double, long);
float scalblnf(float, long);
long double scalblnl(long double, long);
double scalbn(double, int);
float scalbnf(float, int);
long double scalbnl(long double, int);
double j0(double);
double j1(double);
double jn(int, double);
double y0(double);
double y1(double);
double yn(int, double);
float j0f(float);
float j1f(float);
float jnf(int, float);
float y0f(float);
float y1f(float);
float ynf(int, float);

#undef ZEDBSD_MATH_UNARY
#undef ZEDBSD_MATH_BINARY

#endif
