/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The three sides of the H.265 round trip of ps-host-test (ws322-p006),
 * each its own translation unit because each takes its own headers:
 * the stream's (libmedia's parse, ps-host-test.c), libvulkan's encoders
 * (encode.c), the executor's readers and the engine's parameter set
 * writers (executor.c).
 */

#ifndef PLAN_WS322_TESTS_P006_PS_TEST_H
#define PLAN_WS322_TESTS_P006_PS_TEST_H

#include <stddef.h>
#include <stdint.h>

/* The most bytes of one encoded command, and of one written parameter set. */
#define TEST_WIRE_BYTES		(1024U * 1024U)
#define TEST_UNIT_BYTES		8192U

/* The NAL units the executor's sets were written into: the first set of each kind, and whether its writer failed. */
struct test_units {
	uint32_t vps_count;
	uint32_t sps_count;
	uint32_t pps_count;
	int vps_error;
	int sps_error;
	int pps_error;
	size_t vps_bytes;
	size_t sps_bytes;
	size_t pps_bytes;
	uint8_t vps[TEST_UNIT_BYTES];
	uint8_t sps[TEST_UNIT_BYTES];
	uint8_t pps[TEST_UNIT_BYTES];
};

/* What the executor read of a decode: the H.265 picture record, the setup slot and the references' slots. */
struct test_picture {
	int has_picture;
	uint32_t flags;
	uint32_t vps_id;
	uint32_t sps_id;
	uint32_t pps_id;
	int32_t poc;
	int32_t setup;
	uint32_t reference_count;
	int32_t references[16];
};

/*
 * Encodes a parameters create with the H.265 create record carrying add
 * (a VkVideoDecodeH265SessionParametersAddInfoKHR), as
 * vkCreateVideoSessionParametersKHR does; the length, or 0.
 */
size_t test_encode_parameters(const void *add, uint8_t *out, size_t capacity);

/* Encodes [present][a decode] (a VkVideoDecodeInfoKHR) as vkCmdDecodeVideoKHR records it; the length, or 0. */
size_t test_encode_decode(const void *decode, uint8_t *out, size_t capacity);

/* Reads an encoded parameters create as the executor does and writes the sets it took; 0, or the step that failed. */
int test_executor_sets(const uint8_t *wire, size_t length, struct test_units *units);

/* Reads an encoded decode as the executor records it; 0, or the step that failed. */
int test_executor_decode(const uint8_t *wire, size_t length, struct test_picture *picture);

#endif
