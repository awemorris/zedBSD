/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.265 video wire round trip's two sides: libvulkan's encoders write
 * the bytes (h265-wire-encode.c), the executor's readers read them
 * (h265-wire-decode.c).
 */

#ifndef PLAN_WS324_TESTS_H265_WIRE_TEST_H
#define PLAN_WS324_TESTS_H265_WIRE_TEST_H

#include <stddef.h>
#include <stdint.h>

/* Encodes an H.265 parameters create with one VPS, SPS and PPS, every pointer filled; reports the length, or 0. */
size_t wire_encode_h265_parameters(uint8_t *out, size_t capacity);

/* Encodes an H.265 decode with its picture, slice segments, setup slot and references; reports the length, or 0. */
size_t wire_encode_h265_decode(uint8_t *out, size_t capacity);

/* Encodes an H.265 decode profile naming profile_idc; reports the length, or 0. */
size_t wire_encode_h265_profile(uint8_t *out, size_t capacity, uint32_t profile_idc);

#endif
