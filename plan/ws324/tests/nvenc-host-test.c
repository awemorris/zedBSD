/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the nvrtx video encoder's channel parts that touch no
 * hardware: the classes, the push buffers of the bring-up (the class with
 * a no-operation, the encoder's own semaphore release), the resource
 * manager's parameters (the object, the promote of the context) and the
 * device table's pick of the encoder beside the decoder.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"
#include "drivers/gpu/nvrtx/nvrtx-nvdec.h"
#include "drivers/gpu/nvrtx/nvrtx-nvenc.h"

static void check(int ok, const char *what);
static uint32_t word(const uint8_t *bytes, size_t at);
static void test_class(void);
static void test_push(void);
static void test_rm(void);
static void test_pick(void);

/* The failed checks so far. */
static int failures;

/* The checks run so far. */
static int checks;

/* Runs every test and reports the count. */
int
main(void)
{
	/* Runs every group of checks. */
	test_class();
	test_push();
	test_rm();
	test_pick();

	/* Tells the caller how many checks ran and how many failed. */
	printf("nvenc host test: %d checks, %d failed\n", checks, failures);

	/* Fails the run when any check failed. */
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Counts one check and reports it when it fails. */
static void
check(
	int ok,
	const char *what)
{
	/* Counts the check, so a run that skipped checks shows in the total. */
	checks++;

	/* A passing check needs nothing more. */
	if (ok)
		return;

	/* Counts the failure and names the check that failed. */
	failures++;
	printf("FAIL: %s\n", what);
}

/* Reads a little-endian word. */
static uint32_t
word(
	const uint8_t *bytes,
	size_t at)
{
	/* Reads the word through the driver's own accessor. */
	return nvrtx_get32(bytes, at);
}

/* Each family's class, and none for a family without an encoder the driver drives. */
static void
test_class(void)
{
	/* The published classes. */
	check(nvrtx_nvenc_class(NVRTX_FAMILY_TURING) == 0xc4b7U, "class: Turing");
	check(nvrtx_nvenc_class(NVRTX_FAMILY_AMPERE) == 0xc7b7U, "class: Ampere");
	check(nvrtx_nvenc_class(NVRTX_FAMILY_ADA) == 0xc9b7U, "class: Ada");
	check(nvrtx_nvenc_class(NVRTX_FAMILY_HOPPER) == 0U && nvrtx_nvenc_class(NVRTX_FAMILY_UNKNOWN) == 0U, "class: none for the others");
}

/*
 * The bring-up's push buffers: method headers on subchannel 4 (incrementing,
 * count, subchannel, word address), SET_OBJECT, NOP 0x100, SEMAPHORE_A..C
 * 0x240..0x248 and SEMAPHORE_D 0x304.
 */
static void
test_push(void)
{
	uint8_t push[64];
	size_t length;

	/* The class, then the no-operation. */
	memset(push, 0xa5, sizeof(push));
	length = nvrtx_nvenc_push_object(push, 0xc4b7U);
	check(length == 16U && length <= NVRTX_NVENC_OBJECT_PUSH_BYTES, "push: object length");
	check(word(push, 0) == ((1U << 29) | (1U << 16) | (4U << 13) | 0x0000U) && word(push, 4) == 0xc4b7U, "push: SET_OBJECT on subchannel 4");
	check(word(push, 8) == ((1U << 29) | (1U << 16) | (4U << 13) | (0x100U >> 2)) && word(push, 12) == 0U, "push: NOP");
	check(push[16] == 0xa5U, "push: object writes nothing after its length");

	/* The release at an address above 4 GiB, so the upper part shows. */
	memset(push, 0xa5, sizeof(push));
	length = nvrtx_nvenc_push_release(push, 0x1234567810ULL, 0x4e56454eU);
	check(length == 24U && length <= NVRTX_NVENC_RELEASE_PUSH_BYTES, "push: release length");
	check(word(push, 0) == ((1U << 29) | (3U << 16) | (4U << 13) | (0x240U >> 2)), "push: SEMAPHORE_A, three words");
	check(word(push, 4) == 0x12U && word(push, 8) == 0x34567810U && word(push, 12) == 0x4e56454eU, "push: upper, lower, payload");
	check(word(push, 16) == ((1U << 29) | (1U << 16) | (4U << 13) | (0x304U >> 2)) && word(push, 20) == 0U, "push: SEMAPHORE_D release of one word");
	check(push[24] == 0xa5U, "push: release writes nothing after its length");

	/* The upper part keeps eight bits only. */
	length = nvrtx_nvenc_push_release(push, 0xff1234567800ULL, 1U);
	check(length == 24U && word(push, 4) == 0x12U && word(push, 8) == 0x34567800U, "push: upper eight bits");
}

/* The object's parameters and the promote of the encoder's context. */
static void
test_rm(void)
{
	uint8_t params[NVRTX_NVENC_PROMOTE_BYTES];
	size_t index;
	int zero;

	/* The object: size 12, shared, the first encoder. */
	memset(params, 0xa5, sizeof(params));
	nvrtx_nvenc_alloc_params(params);
	check(word(params, 0) == 12U && word(params, 4) == 0U && word(params, 8) == 0U, "rm: encoder object parameters");
	check(params[12] == 0xa5U, "rm: object parameters stay in 12 bytes");

	/* The promote: the encoder's engine 0x1b, the client, channel 49 and its handle, the context. */
	memset(params, 0xa5, sizeof(params));
	nvrtx_nvenc_promote_params(params, 0xc1d00000U, 49U, 0xf1f00031U, 0x28300000ULL, 0x4000ULL);
	check(word(params, 0) == 0x1bU && word(params, 4) == 0xc1d00000U && word(params, 8) == 49U, "rm: promote engine, client, chid");
	check(word(params, 12) == 0xc1d00000U && word(params, 16) == 0xf1f00031U && word(params, 20) == 0U, "rm: promote channel");
	check(word(params, 24) == 0x28300000U && word(params, 28) == 0U && word(params, 32) == 0x4000U && word(params, 36) == 0U && word(params, 40) == 0U,
	    "rm: promote address, size, no entries");

	/* Everything after the fixed words is cleared. */
	zero = 1;
	for (index = 44U; index < sizeof(params); index++) {
		if (params[index] != 0U)
			zero = 0;
	}
	check(zero, "rm: promote's entries cleared");
}

/* The device table: the encoder (0x25) and the decoder (0x1d) on runlists of their own, each found. */
static void
test_pick(void)
{
	struct nvrtx_engine_pick pick;
	uint8_t table[NVRTX_DEVINFO_PARAMS_BYTES];
	int error;

	/* Graphics on runlist 0 with copy 1, copy 2 on runlist 3, the decoder on runlist 5, two encoders on 7 and 8. */
	memset(table, 0, sizeof(table));
	nvrtx_put32(table, 4U, 6U);
	nvrtx_put32(table, 12U + 0U * 100U + 8U, 0x01U);
	nvrtx_put32(table, 12U + 0U * 100U + 12U, 0U);
	nvrtx_put32(table, 12U + 1U * 100U + 8U, 0x0aU);
	nvrtx_put32(table, 12U + 1U * 100U + 12U, 0U);
	nvrtx_put32(table, 12U + 2U * 100U + 8U, 0x0bU);
	nvrtx_put32(table, 12U + 2U * 100U + 12U, 3U);
	nvrtx_put32(table, 12U + 3U * 100U + 0U, 0x84U);
	nvrtx_put32(table, 12U + 3U * 100U + 8U, 0x1dU);
	nvrtx_put32(table, 12U + 3U * 100U + 12U, 5U);
	nvrtx_put32(table, 12U + 4U * 100U + 0U, 0x90U);
	nvrtx_put32(table, 12U + 4U * 100U + 8U, 0x25U);
	nvrtx_put32(table, 12U + 4U * 100U + 12U, 7U);
	nvrtx_put32(table, 12U + 5U * 100U + 0U, 0x91U);
	nvrtx_put32(table, 12U + 5U * 100U + 8U, 0x25U);
	nvrtx_put32(table, 12U + 5U * 100U + 12U, 8U);
	error = nvrtx_devinfo_pick(table, sizeof(table), &pick);
	check(error == 0 && pick.engine == 0x0bU && pick.runlist == 3U, "pick: the copy engine of its own runlist");
	check(pick.nvdec_found == 1U && pick.nvdec_runlist == 5U && pick.nvdec_descriptor == 0x84U, "pick: the decoder");
	check(pick.nvenc_found == 1U && pick.nvenc_runlist == 7U && pick.nvenc_descriptor == 0x90U, "pick: the first encoder");

	/* A table without an encoder: not found, its fields cleared. */
	nvrtx_put32(table, 4U, 4U);
	pick.nvenc_runlist = 99U;
	error = nvrtx_devinfo_pick(table, sizeof(table), &pick);
	check(error == 0 && pick.nvenc_found == 0U && pick.nvenc_runlist == 0U && pick.nvenc_descriptor == 0U, "pick: no encoder");
}
