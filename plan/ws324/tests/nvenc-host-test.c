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
 * manager's parameters (the object, the promote of the context), the
 * device table's pick of the encoder beside the decoder, an H.264
 * picture's setup (the byte and bit places the encoder's interface
 * describes, the same in both of its published layouts) and the push
 * buffer of one encode.
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
static void test_setup(void);
static void test_encode(void);
static void setup_picture(struct nvrtx_nvenc_h264_picture *picture, bool idr);

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
	test_setup();
	test_encode();

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

/* Fills the test's picture: 256x128 at QP 26, Main at level 3.0, the IDR picture or the P picture after it. */
static void
setup_picture(
	struct nvrtx_nvenc_h264_picture *picture,
	bool idr)
{
	/* The size, the pitches and the coding. */
	memset(picture, 0, sizeof(*picture));
	picture->width = 256U;
	picture->height = 128U;
	picture->luma_pitch = 256U;
	picture->chroma_pitch = 256U;
	picture->profile_idc = 77U;
	picture->level_idc = 30U;
	picture->qp = 26U;
	picture->history_bytes = 0x10000U;
	picture->bitstream_bytes = 0x10000U;
	picture->poc_distance = 2U;
	picture->reference = true;
	picture->idr = idr;

	/* The P picture follows the IDR picture. */
	if (!idr) {
		picture->frame_num = 1U;
		picture->poc_lsb = 2U;
	}
}

/*
 * An H.264 picture's setup: the magic word, the surfaces (16x16 tiles),
 * the sequence and picture words, the rate control's QPs, the picture
 * control's lists, numbers, offsets and picture word, the slice, motion
 * search and mode decision controls; the bytes the two published layouts
 * place differently stay zero.  The places were checked against both
 * layouts of the interface's description compiled on the host.
 */
static void
test_setup(void)
{
	struct nvrtx_nvenc_h264_picture picture;
	uint8_t setup[NVRTX_NVENC_H264_SETUP_BYTES];
	size_t index;
	int zero;

	/* The magic word of each class. */
	check(nvrtx_nvenc_magic(0xc4b7U) == 0xc4b70006U && nvrtx_nvenc_magic(0xc7b7U) == 0xc7b70006U, "setup: magic words");

	/* The IDR picture. */
	setup_picture(&picture, true);
	memset(setup, 0xa5, sizeof(setup));
	nvrtx_nvenc_h264_setup(setup, 0xc4b70006U, &picture);
	check(word(setup, 0) == 0xc4b70006U, "setup: magic");
	check(word(setup, 4) == (255U | (127U << 16)) && word(setup, 8) == (256U | (256U << 16)), "setup: reference surface size and pitches");
	check(word(setup, 36 + 28) == 0x80U && word(setup, 68 + 28) == 0x80U && word(setup, 4 + 28) == 0x80U, "setup: every surface in 16x16 tiles");
	check(word(setup, 36 + 20) == 0U, "setup: chroma at its own method's address");
	check(word(setup, 100) == (77U | (30U << 8) | (1U << 16) | (1U << 28)), "setup: sequence word");
	check(word(setup, 104) == 0U && word(setup, 108) == 0U, "setup: picture words at QP 26");
	check(setup[113] == 26U && setup[114] == 26U && setup[115] == 26U && setup[119] == 51U && word(setup, 160) == 0xffffffffU, "setup: rate control");
	check(setup[200] == 0xffU && setup[207] == 0xffU && setup[208] == 0xffU && setup[216] == 0U, "setup: IDR lists empty");
	check(word(setup, 396) == 512U && word(setup, 400) == 768U && word(setup, 404) == 1024U && word(setup, 408) == 1280U, "setup: control offsets");
	check(word(setup, 412) == 0x10000U && word(setup, 416) == 0x10000U && word(setup, 372) == 0x10000U, "setup: buffer sizes");
	check(word(setup, 424) == ((3U << 2) | (1U << 4) | (1U << 5)), "setup: IDR picture word");
	check(word(setup, 512) == (128U | (26U << 19)) && word(setup, 520) == ((51U << 16) | (1U << 24)), "setup: IDR slice");
	check(word(setup, 768) == 2U && word(setup, 780) == 0x38000000U && word(setup, 788) == 0xe0000000U, "setup: motion search sources");
	check(word(setup, 812) == 0xffffffffU && word(setup, 816) == 0xffffffffU, "setup: motion search pattern");
	check(word(setup, 768 + 152) == ((1U << 19) | (2U << 22) | (3U << 25) | (4U << 28)), "setup: hint order");
	check(word(setup, 1028) == (0x1ffU | (0xfU << 18) | (0xfU << 22)) && word(setup, 1032) == (0xfU | (1U << 27)), "setup: mode decision");
	check(word(setup, 1076) == ((3U << 19) | (5U << 26)), "setup: intra decision");

	/* The bytes the layouts place differently, and the quantization control, stay zero. */
	zero = 1;
	for (index = 472U; index < 512U; index++) {
		if (setup[index] != 0U)
			zero = 0;
	}
	for (index = 185U; index < 189U; index++) {
		if (setup[index] != 0U)
			zero = 0;
	}
	for (index = 1280U; index < NVRTX_NVENC_H264_SETUP_BYTES; index++) {
		if (setup[index] != 0U)
			zero = 0;
	}
	check(zero, "setup: layout-dependent bytes and quantization control zero");

	/* The P picture: slot 0 in list 0, its distance, its numbers, P and not all intra. */
	setup_picture(&picture, false);
	picture.codec_field = 5U;
	picture.chroma_units = 128U;
	nvrtx_nvenc_h264_setup(setup, 0xc4b70006U, &picture);
	check(setup[200] == 0U && setup[201] == 0xffU && setup[216] == 2U, "setup: P list 0");
	check(word(setup, 380) == (1U | (2U << 16)) && setup[384] == 0U, "setup: P frame_num and picture order");
	check(word(setup, 424) == ((1U << 4) | (1U << 5)) && word(setup, 428) == (5U << 16), "setup: P picture word and codec field");
	check(word(setup, 520) == (51U << 16), "setup: P slice not all intra");
	check(word(setup, 36 + 20) == 128U && word(setup, 68 + 20) == 128U, "setup: chroma offset");
}

/* One encode's push buffer: the methods, in order, each address shifted right by 8. */
static void
test_encode(void)
{
	struct nvrtx_nvenc_frame frame;
	uint8_t push[NVRTX_NVENC_ENCODE_PUSH_BYTES + 8U];
	uint32_t header;
	size_t length;

	/* A P picture with every buffer. */
	memset(&frame, 0, sizeof(frame));
	frame.setup = 0x1000000100ULL;
	frame.status = 0x200;
	frame.bitstream = 0x300;
	frame.history = 0x400;
	frame.rc_process = 0x500;
	frame.coloc_out = 0x600;
	frame.input_luma = 0x700;
	frame.input_chroma = 0x800;
	frame.output_luma = 0x900;
	frame.output_chroma = 0xa00;
	frame.reference_luma = 0xb00;
	frame.reference_chroma = 0xc00;
	frame.serial = 7U;
	frame.control = NVRTX_NVENC_CONTROL_H264 | NVRTX_NVENC_CONTROL_OUT_PIC;
	memset(push, 0xa5, sizeof(push));
	length = nvrtx_nvenc_push_encode(push, &frame);
	header = (1U << 29) | (1U << 16) | (4U << 13);
	check(length == 16U * 8U && length <= NVRTX_NVENC_ENCODE_PUSH_BYTES && push[length] == 0xa5U, "encode: length");
	check(word(push, 0) == (header | (0x200U >> 2)) && word(push, 4) == 1U, "encode: application H.264");
	check(word(push, 8) == (header | (0x700U >> 2)) && word(push, 12) == 0x103U, "encode: control parameters");
	check(word(push, 16) == (header | (0x704U >> 2)) && word(push, 20) == 7U, "encode: picture index");
	check(word(push, 24) == (header | (0x710U >> 2)) && word(push, 28) == 0x10000001U, "encode: setup address shifted");
	check(word(push, 32) == (header | (0x718U >> 2)) && word(push, 40) == (header | (0x71cU >> 2)) && word(push, 48) == (header | (0x720U >> 2)),
	    "encode: status, bitstream, history");
	check(word(push, 56) == (header | (0x724U >> 2)) && word(push, 60) == 5U, "encode: rate control state");
	check(word(push, 64) == (header | (0x734U >> 2)) && word(push, 72) == (header | (0x740U >> 2)) && word(push, 76) == 8U, "encode: input planes");
	check(word(push, 80) == (header | (0x730U >> 2)) && word(push, 88) == (header | (0x74cU >> 2)), "encode: reconstruction planes");
	check(word(push, 96) == (header | (0x72cU >> 2)) && word(push, 100) == 6U, "encode: co-located output");
	check(word(push, 104) == (header | (0x400U >> 2)) && word(push, 112) == (header | (0x440U >> 2)) && word(push, 116) == 0xcU, "encode: reference planes");
	check(word(push, 120) == (header | (0x300U >> 2)) && word(push, 124) == 0U, "encode: execute");

	/* An IDR picture without the rate control's state, the co-located output or a reference. */
	frame.rc_process = 0U;
	frame.coloc_out = 0U;
	frame.reference_luma = 0U;
	frame.reference_chroma = 0U;
	length = nvrtx_nvenc_push_encode(push, &frame);
	check(length == 12U * 8U && word(push, 88) == (header | (0x300U >> 2)), "encode: IDR without the optional methods");
}
