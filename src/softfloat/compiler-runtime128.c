/* SPARC V9 binary128 compiler ABI wrappers.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdint.h>
#include "src/softfloat/zed-softfloat128.h"

static struct zsf128 tf_bits(long double value)
{
	union { long double f; uint64_t limb[2]; } v = { value };
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return (struct zsf128){ v.limb[1], v.limb[0] };
#else
	return (struct zsf128){ v.limb[0], v.limb[1] };
#endif
}
static long double bits_tf(struct zsf128 bits)
{
	union { uint64_t limb[2]; long double f; } v;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	v.limb[0] = bits.low;
	v.limb[1] = bits.high;
#else
	v.limb[0] = bits.high;
	v.limb[1] = bits.low;
#endif
	return v.f;
}
static uint64_t df_bits(double value)
{ union { double f; uint64_t u; } v={value};return v.u; }
static double bits_df(uint64_t bits)
{ union { uint64_t u; double f; } v={bits};return v.f; }
static uint32_t sf_bits(float value)
{ union { float f; uint32_t u; } v={value};return v.u; }
static float bits_sf(uint32_t bits)
{ union { uint32_t u; float f; } v={bits};return v.f; }

long double __addtf3(long double a,long double b){return bits_tf(zsf128_add(tf_bits(a),tf_bits(b)));}
long double __subtf3(long double a,long double b){return bits_tf(zsf128_sub(tf_bits(a),tf_bits(b)));}
long double __multf3(long double a,long double b){return bits_tf(zsf128_mul(tf_bits(a),tf_bits(b)));}
long double __divtf3(long double a,long double b){return bits_tf(zsf128_div(tf_bits(a),tf_bits(b)));}
long double __extendsftf2(float x){return bits_tf(zsf32_to_128(sf_bits(x)));}
long double __extenddftf2(double x){return bits_tf(zsf64_to_128(df_bits(x)));}
float __trunctfsf2(long double x){return bits_sf(zsf128_to_32(tf_bits(x)));}
double __trunctfdf2(long double x){return bits_df(zsf128_to_64(tf_bits(x)));}
long double __floatsitf(int x){return bits_tf(zsf_i64_to_128(x));}
long double __floatunsitf(unsigned int x){return bits_tf(zsf_u64_to_128(x));}
long double __floatditf(long long x){return bits_tf(zsf_i64_to_128(x));}
long double __floatunditf(unsigned long long x){return bits_tf(zsf_u64_to_128(x));}

static uint64_t tf_to_uint(long double value, int is_unsigned)
{
	struct zsf128 bits = tf_bits(value);
	uint64_t sig_high, result;
	unsigned int sign = (unsigned int)(bits.high >> 63);
	unsigned int exponent = (unsigned int)((bits.high >> 48) & 0x7fffU);
	int e = (int)exponent - 16383;
	unsigned int shift;

	if (exponent == 0 || e < 0 || (sign && is_unsigned))
		return 0;
	if (exponent == 0x7fffU || e > 63)
		return is_unsigned ? UINT64_MAX : (sign ? 0x8000000000000000ULL : 0x7fffffffffffffffULL);
	sig_high = (bits.high & 0x0000ffffffffffffULL) | 0x0001000000000000ULL;
	shift = (unsigned int)(112 - e);
	if (shift >= 64)
		result = sig_high >> (shift - 64);
	else if (shift == 0)
		result = bits.low;
	else
		result = (bits.low >> shift) | (sig_high << (64 - shift));
	return sign ? (uint64_t)(0 - result) : result;
}

int __fixtfsi(long double x){return (int)tf_to_uint(x,0);}
long long __fixtfdi(long double x){return (long long)tf_to_uint(x,0);}
unsigned int __fixunstfsi(long double x){return (unsigned int)tf_to_uint(x,1);}
unsigned long long __fixunstfdi(long double x){return tf_to_uint(x,1);}

static int cmp(long double a,long double b,int nan){int u,r=zsf128_compare(tf_bits(a),tf_bits(b),&u);return u?nan:r;}
int __eqtf2(long double a,long double b){return cmp(a,b,1)!=0;}
int __netf2(long double a,long double b){return cmp(a,b,1)!=0;}
int __getf2(long double a,long double b){return cmp(a,b,-1);}
int __gttf2(long double a,long double b){return cmp(a,b,-1);}
int __letf2(long double a,long double b){return cmp(a,b,1);}
int __lttf2(long double a,long double b){return cmp(a,b,1);}
int __cmptf2(long double a,long double b){return cmp(a,b,1);}
int __unordtf2(long double a,long double b){int u;(void)zsf128_compare(tf_bits(a),tf_bits(b),&u);return u;}
