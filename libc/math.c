/*
 * zedBSD mathematical library.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * This is a clean implementation based on the identities documented beside
 * each approximation.  It deliberately uses only public zedBSD interfaces.
 */

#include <errno.h>
#include <fenv.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>

#include "src/softfloat/zed-softfloat.h"

#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif
#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif

#define ZM_PI 3.14159265358979323846264338327950288
#define ZM_PI_2 1.57079632679489661923132169163975144
#define ZM_PI_4 0.78539816339744830961566084581987572
#define ZM_LN2 0.69314718055994530941723212145817657
#define ZM_SQRT2 1.41421356237309504880168872420969808

int signgam;

static uint64_t double_bits(double x)
{ union { double f; uint64_t u; } v = { x }; return v.u; }
static double bits_double(uint64_t x)
{ union { uint64_t u; double f; } v = { x }; return v.f; }
static uint32_t float_bits(float x)
{ union { float f; uint32_t u; } v = { x }; return v.u; }
static float bits_float(uint32_t x)
{ union { uint32_t u; float f; } v = { x }; return v.f; }

int
__fpclassify(double value)
{
	uint64_t bits = double_bits(value) & UINT64_C(0x7fffffffffffffff);
	if (bits > UINT64_C(0x7ff0000000000000)) return FP_NAN;
	if (bits == UINT64_C(0x7ff0000000000000)) return FP_INFINITE;
	if (bits == 0U) return FP_ZERO;
	if (bits < UINT64_C(0x0010000000000000)) return FP_SUBNORMAL;
	return FP_NORMAL;
}

int
__fpclassifyf(float value)
{
	uint32_t bits = float_bits(value) & UINT32_C(0x7fffffff);
	if (bits > UINT32_C(0x7f800000)) return FP_NAN;
	if (bits == UINT32_C(0x7f800000)) return FP_INFINITE;
	if (bits == 0U) return FP_ZERO;
	if (bits < UINT32_C(0x00800000)) return FP_SUBNORMAL;
	return FP_NORMAL;
}

int __fpclassifyl(long double value) { return __fpclassify((double)value); }
int __signbit(double value) { return (int)(double_bits(value) >> 63); }
int __signbitf(float value) { return (int)(float_bits(value) >> 31); }
int __signbitl(long double value) { return __signbit((double)value); }

double fabs(double x) { return bits_double(double_bits(x) & UINT64_C(0x7fffffffffffffff)); }
float fabsf(float x) { return bits_float(float_bits(x) & UINT32_C(0x7fffffff)); }
long double fabsl(long double x) { return x < 0 ? -x : x; }
double copysign(double x, double y)
{ return bits_double((double_bits(x) & UINT64_C(0x7fffffffffffffff)) | (double_bits(y) & UINT64_C(0x8000000000000000))); }
float copysignf(float x, float y)
{ return bits_float((float_bits(x) & UINT32_C(0x7fffffff)) | (float_bits(y) & UINT32_C(0x80000000))); }
long double copysignl(long double x, long double y)
{ return __signbitl(x) == __signbitl(y) ? x : -x; }

double
trunc(double x)
{
	uint64_t bits = double_bits(x);
	int exponent = (int)((bits >> 52) & 0x7ffU) - 1023;
	uint64_t mask;
	if (exponent < 0) return bits_double(bits & UINT64_C(0x8000000000000000));
	if (exponent >= 52) return x;
	mask = (UINT64_C(1) << (52 - (unsigned int)exponent)) - 1U;
	return bits_double(bits & ~mask);
}

float
truncf(float x)
{
	uint32_t bits = float_bits(x);
	int exponent = (int)((bits >> 23) & 0xffU) - 127;
	uint32_t mask;
	if (exponent < 0) return bits_float(bits & UINT32_C(0x80000000));
	if (exponent >= 23) return x;
	mask = (UINT32_C(1) << (23 - (unsigned int)exponent)) - 1U;
	return bits_float(bits & ~mask);
}
long double truncl(long double x) { return (long double)trunc((double)x); }

double floor(double x) { double t = trunc(x); return x < t ? t - 1.0 : t; }
float floorf(float x) { float t = truncf(x); return x < t ? t - 1.0f : t; }
long double floorl(long double x) { long double t = truncl(x); return x < t ? t - 1.0L : t; }
double ceil(double x) { double t = trunc(x); return x > t ? t + 1.0 : t; }
float ceilf(float x) { float t = truncf(x); return x > t ? t + 1.0f : t; }
long double ceill(long double x) { long double t = truncl(x); return x > t ? t + 1.0L : t; }
double round(double x) { return x < 0 ? ceil(x - 0.5) : floor(x + 0.5); }
float roundf(float x) { return x < 0 ? ceilf(x - 0.5f) : floorf(x + 0.5f); }
long double roundl(long double x) { return x < 0 ? ceill(x - 0.5L) : floorl(x + 0.5L); }
double rint(double x)
{ double f = floor(x), d = x - f; return d < 0.5 ? f : d > 0.5 ? f + 1.0 : ((long long)f & 1) ? f + 1.0 : f; }
float rintf(float x) { return (float)rint(x); }
long double rintl(long double x) { return (long double)rint((double)x); }
double nearbyint(double x) { return rint(x); }
float nearbyintf(float x) { return rintf(x); }
long double nearbyintl(long double x) { return rintl(x); }

long lrint(double x) { return (long)rint(x); }
long lrintf(float x) { return (long)rintf(x); }
long lrintl(long double x) { return (long)rintl(x); }
long long llrint(double x) { return (long long)rint(x); }
long long llrintf(float x) { return (long long)rintf(x); }
long long llrintl(long double x) { return (long long)rintl(x); }
long lround(double x) { return (long)round(x); }
long lroundf(float x) { return (long)roundf(x); }
long lroundl(long double x) { return (long)roundl(x); }
long long llround(double x) { return (long long)round(x); }
long long llroundf(float x) { return (long long)roundf(x); }
long long llroundl(long double x) { return (long long)roundl(x); }

double
frexp(double x, int *exponent)
{
	uint64_t bits = double_bits(x);
	uint64_t magnitude = bits & UINT64_C(0x7fffffffffffffff);
	uint64_t fraction;
	int value_exponent;
	if (magnitude == 0U || magnitude >= UINT64_C(0x7ff0000000000000)) {
		*exponent = 0;
		return x;
	}
	fraction = magnitude & UINT64_C(0x000fffffffffffff);
	value_exponent = (int)(magnitude >> 52);
	if (value_exponent == 0) {
		value_exponent = -1022;
		while ((fraction & UINT64_C(0x0010000000000000)) == 0U) {
			fraction <<= 1;
			value_exponent--;
		}
		fraction &= UINT64_C(0x000fffffffffffff);
	} else value_exponent -= 1023;
	*exponent = value_exponent + 1;
	return bits_double((bits & UINT64_C(0x8000000000000000)) |
	    UINT64_C(0x3fe0000000000000) | fraction);
}

float frexpf(float x, int *e) { double v = frexp(x, e); return (float)v; }
long double frexpl(long double x, int *e) { return (long double)frexp((double)x, e); }

double
scalbn(double x, int n)
{
	uint64_t bits = double_bits(x);
	uint64_t magnitude = bits & UINT64_C(0x7fffffffffffffff);
	uint64_t fraction = magnitude & UINT64_C(0x000fffffffffffff);
	int exponent = (int)(magnitude >> 52);
	unsigned int sign = (unsigned int)(bits >> 63);
	if (magnitude == 0U || exponent == 0x7ff) return x;
	if (exponent == 0) {
		exponent = -1022;
		while ((fraction & UINT64_C(0x0010000000000000)) == 0U) {
			fraction <<= 1;
			exponent--;
		}
	} else {
		fraction |= UINT64_C(0x0010000000000000);
		exponent -= 1023;
	}
	return bits_double(zsf64_round_pack(sign, exponent + n, fraction << 3));
}

float
scalbnf(float x, int n)
{
	uint32_t bits = float_bits(x);
	uint32_t magnitude = bits & UINT32_C(0x7fffffff);
	uint32_t fraction = magnitude & UINT32_C(0x007fffff);
	int exponent = (int)(magnitude >> 23);
	unsigned int sign = bits >> 31;
	if (magnitude == 0U || exponent == 0xff) return x;
	if (exponent == 0) {
		exponent = -126;
		while ((fraction & UINT32_C(0x00800000)) == 0U) {
			fraction <<= 1;
			exponent--;
		}
	} else {
		fraction |= UINT32_C(0x00800000);
		exponent -= 127;
	}
	return bits_float(zsf32_round_pack(sign, exponent + n,
	    (uint64_t)fraction << 3));
}
long double scalbnl(long double x, int n) { return (long double)scalbn((double)x, n); }
double ldexp(double x, int n) { return scalbn(x, n); }
float ldexpf(float x, int n) { return scalbnf(x, n); }
long double ldexpl(long double x, int n) { return scalbnl(x, n); }
double scalbln(double x, long n) { return scalbn(x, n > INT_MAX ? INT_MAX : n < INT_MIN ? INT_MIN : (int)n); }
float scalblnf(float x, long n) { return scalbnf(x, n > INT_MAX ? INT_MAX : n < INT_MIN ? INT_MIN : (int)n); }
long double scalblnl(long double x, long n) { return scalbnl(x, n > INT_MAX ? INT_MAX : n < INT_MIN ? INT_MIN : (int)n); }

int ilogb(double x)
{ int e; if (x == 0.0) return FP_ILOGB0; if (isnan(x)) return FP_ILOGBNAN; if (isinf(x)) return INT_MAX; (void)frexp(x, &e); return e - 1; }
int ilogbf(float x) { return ilogb(x); }
int ilogbl(long double x) { return ilogb((double)x); }
double logb(double x) { return (double)ilogb(x); }
float logbf(float x) { return (float)ilogbf(x); }
long double logbl(long double x) { return (long double)ilogbl(x); }

double modf(double x, double *integer) { *integer = trunc(x); return x - *integer; }
float modff(float x, float *integer) { *integer = truncf(x); return x - *integer; }
long double modfl(long double x, long double *integer) { *integer = truncl(x); return x - *integer; }

double
nextafter(double x, double y)
{
	uint64_t bits;
	if (isnan(x) || isnan(y)) return x + y;
	if (x == y) return y;
	if (x == 0.0) return bits_double((double_bits(y) & UINT64_C(0x8000000000000000)) | 1U);
	bits = double_bits(x);
	if ((x < y) == (x > 0.0)) bits++; else bits--;
	return bits_double(bits);
}
float nextafterf(float x, float y)
{ uint32_t b; if (isnan(x)||isnan(y)) return x+y; if(x==y)return y; if(x==0)return bits_float((float_bits(y)&UINT32_C(0x80000000))|1U); b=float_bits(x); if((x<y)==(x>0))b++;else b--;return bits_float(b); }
long double nextafterl(long double x, long double y) { return (long double)nextafter((double)x, (double)y); }
double nexttoward(double x, long double y) { return nextafter(x, (double)y); }
float nexttowardf(float x, long double y) { return nextafterf(x, (float)y); }
long double nexttowardl(long double x, long double y) { return nextafterl(x, y); }

double nan(const char *tag) { (void)tag; return bits_double(UINT64_C(0x7ff8000000000000)); }
float nanf(const char *tag) { (void)tag; return bits_float(UINT32_C(0x7fc00000)); }
long double nanl(const char *tag) { return (long double)nan(tag); }

double fmin(double x, double y) { if (isnan(x)) return y; if (isnan(y)) return x; return x < y ? x : y; }
float fminf(float x,float y){return (float)fmin(x,y);} long double fminl(long double x,long double y){return x<y?x:y;}
double fmax(double x, double y) { if (isnan(x)) return y; if (isnan(y)) return x; return x > y ? x : y; }
float fmaxf(float x,float y){return (float)fmax(x,y);} long double fmaxl(long double x,long double y){return x>y?x:y;}
double fdim(double x,double y){return x>y?x-y:0.0;} float fdimf(float x,float y){return x>y?x-y:0.0f;} long double fdiml(long double x,long double y){return x>y?x-y:0.0L;}

double
sqrt(double x)
{
	double estimate;
	int index;
	if (x < 0.0) { errno = EDOM; (void)feraiseexcept(FE_INVALID); return NAN; }
	if (x == 0.0 || isinf(x) || isnan(x)) return x;
	estimate = bits_double((double_bits(x) >> 1) + UINT64_C(0x1ff8000000000000));
	for (index = 0; index < 8; index++) estimate = 0.5 * (estimate + x / estimate);
	return estimate;
}
float sqrtf(float x) { return (float)sqrt(x); }
long double sqrtl(long double x) { return (long double)sqrt((double)x); }

/* exp(x) = 2^k exp(r), |r| <= ln(2)/2; exp(r) is evaluated by its
 * absolutely convergent Taylor series. */
double
exp(double x)
{
	int k, i;
	double r, term = 1.0, sum = 1.0;
	if (isnan(x)) return x;
	if (x > 709.782712893384) { errno=ERANGE; (void)feraiseexcept(FE_OVERFLOW); return INFINITY; }
	if (x < -745.133219101941) { errno=ERANGE; (void)feraiseexcept(FE_UNDERFLOW); return 0.0; }
	k = (int)(x / ZM_LN2 + (x >= 0.0 ? 0.5 : -0.5));
	r = x - k * ZM_LN2;
	for (i = 1; i <= 18; i++) { term *= r / i; sum += term; }
	return scalbn(sum, k);
}
float expf(float x){return (float)exp(x);} long double expl(long double x){return (long double)exp((double)x);}
double exp2(double x){return exp(x*ZM_LN2);} float exp2f(float x){return (float)exp2(x);} long double exp2l(long double x){return (long double)exp2((double)x);}
double expm1(double x){if(fabs(x)<1e-5){double t=x,s=x;int i;for(i=2;i<16;i++){t*=x/i;s+=t;}return s;}return exp(x)-1.0;}
float expm1f(float x){return (float)expm1(x);} long double expm1l(long double x){return (long double)expm1((double)x);}

/* log(m) = 2*(z + z^3/3 + ...), z=(m-1)/(m+1), with m in
 * [sqrt(1/2),sqrt(2)] after frexp reduction. */
double
log(double x)
{
	int e, i;
	double m, z, z2, term, sum;
	if (x < 0.0) { errno=EDOM; (void)feraiseexcept(FE_INVALID); return NAN; }
	if (x == 0.0) { errno=ERANGE; (void)feraiseexcept(FE_DIVBYZERO); return -INFINITY; }
	if (isinf(x) || isnan(x)) return x;
	m = frexp(x, &e);
	if (m < 0.7071067811865475244) { m *= 2.0; e--; }
	z = (m - 1.0) / (m + 1.0); z2 = z*z; term=z; sum=z;
	for(i=3;i<=39;i+=2){term*=z2;sum+=term/i;}
	return 2.0*sum + e*ZM_LN2;
}
float logf(float x){return (float)log(x);} long double logl(long double x){return (long double)log((double)x);}
double log2(double x){return log(x)/ZM_LN2;} float log2f(float x){return (float)log2(x);} long double log2l(long double x){return (long double)log2((double)x);}
double log10(double x){return log(x)/2.30258509299404568402;} float log10f(float x){return (float)log10(x);} long double log10l(long double x){return (long double)log10((double)x);}
double log1p(double x){if(fabs(x)<1e-4){double t=x,s=0;int i;for(i=1;i<30;i++){s+=(i&1?1.0:-1.0)*t/i;t*=x;}return s;}return log(1.0+x);}
float log1pf(float x){return (float)log1p(x);} long double log1pl(long double x){return (long double)log1p((double)x);}

static double
reduce_angle(double x, int *quadrant)
{
	double q = rint(x / ZM_PI_2);
	*quadrant = (int)q & 3;
	return x - q * ZM_PI_2;
}

static double
sin_kernel(double x)
{
	double x2=x*x;
	return x*(1.0+x2*(-1.0/6.0+x2*(1.0/120.0+x2*(-1.0/5040.0+
	    x2*(1.0/362880.0+x2*(-1.0/39916800.0+x2/6227020800.0))))));
}
static double
cos_kernel(double x)
{
	double x2=x*x;
	return 1.0+x2*(-1.0/2.0+x2*(1.0/24.0+x2*(-1.0/720.0+
	    x2*(1.0/40320.0+x2*(-1.0/3628800.0+x2/479001600.0)))));
}
double sin(double x){int q;double r;if(isnan(x))return x;if(isinf(x)){errno=EDOM;return NAN;}r=reduce_angle(x,&q);return q==0?sin_kernel(r):q==1?cos_kernel(r):q==2?-sin_kernel(r):-cos_kernel(r);}
double cos(double x){int q;double r;if(isnan(x))return x;if(isinf(x)){errno=EDOM;return NAN;}r=reduce_angle(x,&q);return q==0?cos_kernel(r):q==1?-sin_kernel(r):q==2?-cos_kernel(r):sin_kernel(r);}
double tan(double x){return sin(x)/cos(x);} float sinf(float x){return (float)sin(x);} float cosf(float x){return (float)cos(x);} float tanf(float x){return (float)tan(x);} long double sinl(long double x){return (long double)sin((double)x);} long double cosl(long double x){return (long double)cos((double)x);} long double tanl(long double x){return (long double)tan((double)x);}

double
atan(double x)
{
	double sign=1.0, result, term, x2;
	int i;
	if (isnan(x))
		return x;
	if (x < 0) {
		sign = -1;
		x = -x;
	}
	if(x>1.0)return sign*(ZM_PI_2-atan(1.0/x));
	if(x>0.4142135623730950)return sign*(ZM_PI_4+atan((x-1.0)/(x+1.0)));
	x2=x*x;term=x;result=x;for(i=3;i<=39;i+=2){term*=-x2;result+=term/i;}return sign*result;
}
float atanf(float x){return (float)atan(x);} long double atanl(long double x){return (long double)atan((double)x);}
double atan2(double y,double x){if(isnan(x)||isnan(y))return NAN;if(x>0)return atan(y/x);if(x<0)return y>=0?atan(y/x)+ZM_PI:atan(y/x)-ZM_PI;if(y>0)return ZM_PI_2;if(y<0)return -ZM_PI_2;return y;}
float atan2f(float y,float x){return (float)atan2(y,x);} long double atan2l(long double y,long double x){return (long double)atan2((double)y,(double)x);}
double asin(double x){if(fabs(x)>1){errno=EDOM;return NAN;}return atan2(x,sqrt((1.0-x)*(1.0+x)));}
float asinf(float x){return (float)asin(x);} long double asinl(long double x){return (long double)asin((double)x);}
double acos(double x){return ZM_PI_2-asin(x);} float acosf(float x){return (float)acos(x);} long double acosl(long double x){return (long double)acos((double)x);}

double sinh(double x){double e=exp(x),i=1.0/e;return 0.5*(e-i);} float sinhf(float x){return (float)sinh(x);} long double sinhl(long double x){return (long double)sinh((double)x);}
double cosh(double x){double e=exp(fabs(x));return 0.5*(e+1.0/e);} float coshf(float x){return (float)cosh(x);} long double coshl(long double x){return (long double)cosh((double)x);}
double tanh(double x){if(x>20)return 1;if(x<-20)return -1;{double e=exp(2*x);return(e-1)/(e+1);}} float tanhf(float x){return (float)tanh(x);} long double tanhl(long double x){return (long double)tanh((double)x);}
double asinh(double x){return log(x+sqrt(x*x+1));} float asinhf(float x){return (float)asinh(x);} long double asinhl(long double x){return (long double)asinh((double)x);}
double acosh(double x){if(x<1){errno=EDOM;return NAN;}return log(x+sqrt((x-1)*(x+1)));} float acoshf(float x){return (float)acosh(x);} long double acoshl(long double x){return (long double)acosh((double)x);}
double atanh(double x){if(fabs(x)>=1){errno=EDOM;return x<0?-INFINITY:INFINITY;}return 0.5*log((1+x)/(1-x));} float atanhf(float x){return (float)atanh(x);} long double atanhl(long double x){return (long double)atanh((double)x);}

double cbrt(double x){double a=fabs(x),g;int i;if(a==0||isinf(a)||isnan(a))return x;g=exp(log(a)/3);for(i=0;i<4;i++)g=(2*g+a/(g*g))/3;return x<0?-g:g;}
float cbrtf(float x){return (float)cbrt(x);} long double cbrtl(long double x){return (long double)cbrt((double)x);}
double hypot(double x,double y){x=fabs(x);y=fabs(y);if(x<y){double t=x;x=y;y=t;}if(isinf(x))return x;if(x==0)return 0;y/=x;return x*sqrt(1+y*y);}
float hypotf(float x,float y){return (float)hypot(x,y);} long double hypotl(long double x,long double y){return (long double)hypot((double)x,(double)y);}
double fmod(double x,double y){if(y==0||isinf(x)){errno=EDOM;return NAN;}if(isinf(y))return x;return x-trunc(x/y)*y;}
float fmodf(float x,float y){return (float)fmod(x,y);} long double fmodl(long double x,long double y){return (long double)fmod((double)x,(double)y);}
double remainder(double x,double y){if(y==0){errno=EDOM;return NAN;}return x-rint(x/y)*y;}
float remainderf(float x,float y){return (float)remainder(x,y);} long double remainderl(long double x,long double y){return (long double)remainder((double)x,(double)y);}
double remquo(double x,double y,int*q){double n=rint(x/y);*q=(int)n&0x7f;return x-n*y;} float remquof(float x,float y,int*q){return (float)remquo(x,y,q);} long double remquol(long double x,long double y,int*q){return (long double)remquo((double)x,(double)y,q);}
double fma(double x,double y,double z){return x*y+z;} float fmaf(float x,float y,float z){return x*y+z;} long double fmal(long double x,long double y,long double z){return x*y+z;}

static int integer_value(double y,long long *value){double t=trunc(y);if(t!=y||fabs(y)>9.22e18)return 0;*value=(long long)t;return 1;}
double pow(double x,double y){long long n;if(y==0)return 1;if(x==0)return y<0?INFINITY:x;if(x<0){if(!integer_value(y,&n)){errno=EDOM;return NAN;}return (n&1)?-exp(y*log(-x)):exp(y*log(-x));}return exp(y*log(x));}
float powf(float x,float y){return (float)pow(x,y);} long double powl(long double x,long double y){return (long double)pow((double)x,(double)y);}

/* Abramowitz-Stegun 7.1.26, maximum error about 1.5e-7. */
double erf(double x){double s=x<0?-1:1,t,a;x=fabs(x);t=1/(1+0.3275911*x);a=1-(((((1.061405429*t-1.453152027)*t+1.421413741)*t-0.284496736)*t+0.254829592)*t)*exp(-x*x);return s*a;}
float erff(float x){return (float)erf(x);} long double erfl(long double x){return (long double)erf((double)x);}
double erfc(double x){return 1-erf(x);} float erfcf(float x){return (float)erfc(x);} long double erfcl(long double x){return (long double)erfc((double)x);}

/* Lanczos approximation with g=7 and nine coefficients. */
static double
gamma_lanczos(double z)
{
	static const double c[] = {0.99999999999980993,676.5203681218851,
	    -1259.1392167224028,771.32342877765313,-176.61502916214059,
	    12.507343278686905,-0.13857109526572012,9.9843695780195716e-6,
	    1.5056327351493116e-7};
	double a,t;int i;
	if(z<0.5)return ZM_PI/(sin(ZM_PI*z)*gamma_lanczos(1-z));
	z-=1;a=c[0];for(i=1;i<9;i++)a+=c[i]/(z+i);t=z+7.5;
	return 2.5066282746310005024*sqrt(t)*pow(t,z)*exp(-t)*a;
}
double tgamma(double x){if(x<=0&&x==trunc(x)){errno=EDOM;return NAN;}return gamma_lanczos(x);} float tgammaf(float x){return (float)tgamma(x);} long double tgammal(long double x){return (long double)tgamma((double)x);}
double lgamma(double x){double g=tgamma(x);signgam=g<0?-1:1;return log(fabs(g));} float lgammaf(float x){return (float)lgamma(x);} long double lgammal(long double x){return (long double)lgamma((double)x);}

/* Bessel functions use their defining power series for moderate arguments;
 * recurrence supplies integral orders. */
double j0(double x){double term=1,sum=1,q=x*x/4;int k;for(k=1;k<30;k++){term*=-q/(k*k);sum+=term;}return sum;}
double j1(double x){double term=x/2,sum=term,q=x*x/4;int k;for(k=1;k<30;k++){term*=-q/(k*(k+1.0));sum+=term;}return sum;}
double jn(int n,double x){int k;double a,b,c;if(n<0)return(n&1)?-jn(-n,x):jn(-n,x);if(n==0)return j0(x);if(n==1)return j1(x);if(x==0)return 0;a=j0(x);b=j1(x);for(k=1;k<n;k++){c=2.0*k*b/x-a;a=b;b=c;}return b;}
float j0f(float x){return (float)j0(x);} float j1f(float x){return (float)j1(x);} float jnf(int n,float x){return (float)jn(n,x);}
double y0(double x){if(x<=0){errno=EDOM;return -INFINITY;}return sqrt(2/(ZM_PI*x))*sin(x-ZM_PI_4);}
double y1(double x){if(x<=0){errno=EDOM;return -INFINITY;}return sqrt(2/(ZM_PI*x))*sin(x-3*ZM_PI_4);}
double yn(int n,double x){int k;double a,b,c;if(n==0)return y0(x);if(n==1)return y1(x);a=y0(x);b=y1(x);for(k=1;k<n;k++){c=2.0*k*b/x-a;a=b;b=c;}return b;}
float y0f(float x){return (float)y0(x);} float y1f(float x){return (float)y1(x);} float ynf(int n,float x){return (float)yn(n,x);}
