/*
 * GCC-compatible compiler ABI wrappers for the zedBSD IEEE 754 core.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */

#include <stdint.h>

#include "src/softfloat/zed-softfloat.h"

static uint32_t
float_bits(float value)
{
	union { float value; uint32_t bits; } shape = { value };
	return shape.bits;
}

static float
bits_float(uint32_t bits)
{
	union { uint32_t bits; float value; } shape = { bits };
	return shape.value;
}

static uint64_t
double_bits(double value)
{
	union { double value; uint64_t bits; } shape = { value };
	return shape.bits;
}

static double
bits_double(uint64_t bits)
{
	union { uint64_t bits; double value; } shape = { bits };
	return shape.value;
}

float __addsf3(float a, float b)
{ return bits_float(zsf32_add(float_bits(a), float_bits(b))); }
float __subsf3(float a, float b)
{ return bits_float(zsf32_sub(float_bits(a), float_bits(b))); }
float __mulsf3(float a, float b)
{ return bits_float(zsf32_mul(float_bits(a), float_bits(b))); }
float __divsf3(float a, float b)
{ return bits_float(zsf32_div(float_bits(a), float_bits(b))); }

double __adddf3(double a, double b)
{ return bits_double(zsf64_add(double_bits(a), double_bits(b))); }
double __subdf3(double a, double b)
{ return bits_double(zsf64_sub(double_bits(a), double_bits(b))); }
double __muldf3(double a, double b)
{ return bits_double(zsf64_mul(double_bits(a), double_bits(b))); }
double __divdf3(double a, double b)
{ return bits_double(zsf64_div(double_bits(a), double_bits(b))); }

double __extendsfdf2(float value)
{ return bits_double(zsf32_to_64(float_bits(value))); }
float __truncdfsf2(double value)
{ return bits_float(zsf64_to_32(double_bits(value))); }

float __floatsisf(int value)
{ return bits_float(zsf_i64_to_32(value)); }
float __floatdisf(int64_t value)
{ return bits_float(zsf_i64_to_32(value)); }
float __floatunsisf(unsigned int value)
{ return bits_float(zsf_u64_to_32(value)); }
float __floatundisf(uint64_t value)
{ return bits_float(zsf_u64_to_32(value)); }
double __floatsidf(int value)
{ return bits_double(zsf_i64_to_64(value)); }
double __floatdidf(int64_t value)
{ return bits_double(zsf_i64_to_64(value)); }
double __floatunsidf(unsigned int value)
{ return bits_double(zsf_u64_to_64(value)); }
double __floatundidf(uint64_t value)
{ return bits_double(zsf_u64_to_64(value)); }

int __fixsfsi(float value)
{ return (int)zsf32_to_i64(float_bits(value), 32U, 0); }
int64_t __fixsfdi(float value)
{ return zsf32_to_i64(float_bits(value), 64U, 0); }
unsigned int __fixunssfsi(float value)
{ return (unsigned int)zsf32_to_i64(float_bits(value), 32U, 1); }
uint64_t __fixunssfdi(float value)
{ return (uint64_t)zsf32_to_i64(float_bits(value), 64U, 1); }
int __fixdfsi(double value)
{ return (int)zsf64_to_i64(double_bits(value), 32U, 0); }
int64_t __fixdfdi(double value)
{ return zsf64_to_i64(double_bits(value), 64U, 0); }
unsigned int __fixunsdfsi(double value)
{ return (unsigned int)zsf64_to_i64(double_bits(value), 32U, 1); }
uint64_t __fixunsdfdi(double value)
{ return (uint64_t)zsf64_to_i64(double_bits(value), 64U, 1); }

static int
compare32(float a, float b, int unordered_value)
{
	int unordered;
	int result = zsf32_compare(float_bits(a), float_bits(b), &unordered);
	return unordered ? unordered_value : result;
}

static int
compare64(double a, double b, int unordered_value)
{
	int unordered;
	int result = zsf64_compare(double_bits(a), double_bits(b), &unordered);
	return unordered ? unordered_value : result;
}

int __eqsf2(float a, float b) { return compare32(a, b, 1) != 0; }
int __nesf2(float a, float b) { return compare32(a, b, 1) != 0; }
int __gesf2(float a, float b) { return compare32(a, b, -1); }
int __gtsf2(float a, float b) { return compare32(a, b, -1); }
int __lesf2(float a, float b) { return compare32(a, b, 1); }
int __ltsf2(float a, float b) { return compare32(a, b, 1); }
int __cmpsf2(float a, float b) { return compare32(a, b, 1); }
int __unordsf2(float a, float b)
{
	int unordered;
	(void)zsf32_compare(float_bits(a), float_bits(b), &unordered);
	return unordered;
}

int __eqdf2(double a, double b) { return compare64(a, b, 1) != 0; }
int __nedf2(double a, double b) { return compare64(a, b, 1) != 0; }
int __gedf2(double a, double b) { return compare64(a, b, -1); }
int __gtdf2(double a, double b) { return compare64(a, b, -1); }
int __ledf2(double a, double b) { return compare64(a, b, 1); }
int __ltdf2(double a, double b) { return compare64(a, b, 1); }
int __cmpdf2(double a, double b) { return compare64(a, b, 1); }
int __unorddf2(double a, double b)
{
	int unordered;
	(void)zsf64_compare(double_bits(a), double_bits(b), &unordered);
	return unordered;
}
