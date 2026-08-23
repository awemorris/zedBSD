/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_ZED_SOFTFLOAT128_H
#define ZEDBSD_ZED_SOFTFLOAT128_H

#include <stdint.h>

struct zsf128 { uint64_t high, low; };

struct zsf128 zsf128_add(struct zsf128, struct zsf128);
struct zsf128 zsf128_sub(struct zsf128, struct zsf128);
struct zsf128 zsf128_mul(struct zsf128, struct zsf128);
struct zsf128 zsf128_div(struct zsf128, struct zsf128);
int zsf128_compare(struct zsf128, struct zsf128, int *);
struct zsf128 zsf32_to_128(uint32_t);
struct zsf128 zsf64_to_128(uint64_t);
uint32_t zsf128_to_32(struct zsf128);
uint64_t zsf128_to_64(struct zsf128);
struct zsf128 zsf_i64_to_128(int64_t);
struct zsf128 zsf_u64_to_128(uint64_t);

#endif
