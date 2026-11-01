/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The video encoder's self-test (nvenc-test, nvrtx.nvenc.test=1).
 *
 * Once the encoder's channel runs, the driver encodes two H.264 pictures
 * of a known pattern and writes what the encoder produced into the kernel
 * log, so the bitstream can be checked by a decoder outside the machine:
 *
 *   1. system memory for two input pictures, two reconstructed pictures,
 *      two bitstream buffers, the statuses, the setups, the push buffers,
 *      the history, the co-located data and the rate control's state, all
 *      mapped into the work channel's space;
 *   2. the input pictures written by the CPU: NV12 of 256x128 in 16x16
 *      tiles, every 16x16 block of luma one value and the chroma one
 *      colour, the second picture the first moved right by one block (the
 *      right column coming in on the left);
 *   3. an IDR picture of the first, then a P picture of the second that
 *      refers to the first's reconstruction, both at a constant QP of 26;
 *   4. each picture's status, its bitstream and the start of its
 *      reconstruction written to the log in lines of 32 bytes, each line
 *      with its offset and its FNV-1a hash, and each run of lines closed
 *      by its length and the hash of the whole.
 *
 * The values the description of the encoder's interface leaves open are
 * tried as a short list of choices (the picture control's codec field,
 * then the firmware's errors returned to the channel); the test stops at
 * the first choice under which both pictures came back without an error,
 * or at an encode that did not finish (the channel is then out of use).
 * The memory is given back afterwards, unless an encode did not finish
 * (the engine may still write it).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gpu.h"
#include "drivers/gpu/nvrtx/nvrtx-nvenc.h"

/*
 * The pictures: their size, their planes' pitch, where the chroma plane
 * starts in a picture's buffer and the bytes of the buffer.
 */
#define TEST_WIDTH			256U
#define TEST_HEIGHT			128U
#define TEST_PITCH			256U
#define TEST_CHROMA_AT			(TEST_PITCH * TEST_HEIGHT)
#define TEST_PICTURE_BYTES		(TEST_CHROMA_AT + TEST_PITCH * TEST_HEIGHT / 2U)

/* A 16x16 tile's side and bytes. */
#define TEST_TILE			16U
#define TEST_TILE_BYTES			256U

/* The pattern: the luma of the blocks and the one chroma colour. */
#define TEST_LUMA_BASE			32U
#define TEST_LUMA_RANGE			192U
#define TEST_LUMA_STEP_X		37U
#define TEST_LUMA_STEP_Y		71U
#define TEST_CB				100U
#define TEST_CR				150U

/* The bitstream buffers' and the history's bytes. */
#define TEST_BITSTREAM_BYTES		0x10000U
#define TEST_HISTORY_BYTES		0x10000U

/*
 * The work buffer: the two setups, the two statuses, the two push buffers,
 * the rate control's state and the two pictures' co-located data (64
 * bytes a macroblock).
 */
#define TEST_WORK_BYTES			0x8000U
#define TEST_SETUP_AT			0x0000U
#define TEST_SETUP_STRIDE		0x0800U
#define TEST_STATUS_AT			0x1000U
#define TEST_STATUS_STRIDE		0x0100U
#define TEST_PUSH_AT			0x2000U
#define TEST_PUSH_STRIDE		0x0100U
#define TEST_RC_AT			0x3000U
#define TEST_RC_DATA_AT			0x3400U
#define TEST_COLOC_AT			0x4000U
#define TEST_COLOC_STRIDE		0x2000U

/* The byte a status is filled with before the encode, so one the encoder did not write shows. */
#define TEST_STATUS_UNWRITTEN		0xa5U

/* How long one encode may take. */
#define TEST_ENCODE_US			2000000U

/* The most bitstream bytes one picture writes to the log, and the reconstruction's bytes it shows. */
#define TEST_DUMP_BITSTREAM_MAX		0x2000U
#define TEST_DUMP_LUMA_BYTES		(2U * TEST_TILE_BYTES)
#define TEST_DUMP_CHROMA_BYTES		TEST_TILE_BYTES

/* The bytes of one log line. */
#define TEST_LINE_BYTES			32U

/* The picture's coding: Main profile, level 3.0, the sizes' log2 less 4, the frames' picture order distance. */
#define TEST_PROFILE_MAIN		77U
#define TEST_LEVEL			30U
#define TEST_QP				26U
#define TEST_POC_STEP			2U

/* FNV-1a's start and multiplier. */
#define TEST_FNV_START			2166136261U
#define TEST_FNV_PRIME			16777619U

/* The pictures of one attempt: the IDR picture, then the P picture. */
#define TEST_PICTURES			2U

/* The buffers of the test, by what they hold. */
enum test_buffer {
	TEST_INPUT_I,
	TEST_INPUT_P,
	TEST_OUTPUT_I,
	TEST_OUTPUT_P,
	TEST_BITS_I,
	TEST_BITS_P,
	TEST_WORK,
	TEST_HISTORY,
	TEST_BUFFERS
};

/*
 * How a choice gives the encoder its buffers: without the optional ones
 * (the rate control's state, the co-located output), and the chroma
 * planes as an offset in the setup from the luma planes' addresses (the
 * chroma methods then name the luma planes) instead of by their own
 * methods.
 */
#define TEST_NO_OPTIONAL		(1U << 0)
#define TEST_CHROMA_OFFSET		(1U << 1)

/*
 * Which buffers a choice takes from VRAM instead of system memory: the
 * pictures (the inputs and the reconstructions), or every buffer.
 */
#define TEST_VRAM_PICTURES		(1U << 2)
#define TEST_VRAM_ALL			(1U << 3)
#define TEST_PICTURE_BUFFERS		((1U << TEST_INPUT_I) | (1U << TEST_INPUT_P) | (1U << TEST_OUTPUT_I) | (1U << TEST_OUTPUT_P))
#define TEST_ALL_BUFFERS		((1U << TEST_BUFFERS) - 1U)

/* A choice that also gives the encoder the rate control's data (a zeroed page). */
#define TEST_RC_DATA			(1U << 4)

/*
 * One word of a setup a choice writes over what the setup's builder made:
 * its byte offset in the setup and its value.  The self-test tries values
 * the interface's description leaves open with these.
 */
struct test_poke {
	uint32_t at;
	uint32_t value;
};

/*
 * One choice of the values the interface's description leaves open: the
 * picture control's codec field, the control parameters' extra bits (or
 * none at all when control_mask clears them), the buffers' flags, the
 * engine's watchdog (0 for none), the setup's words written over, and
 * one further method with its word (method 0 for none).
 */
struct test_attempt {
	uint32_t extra_method;
	uint32_t extra_data;
	uint32_t codec_field;
	uint32_t control_extra;
	uint32_t control_mask;
	uint32_t flags;
	uint32_t watchdog;
	const struct test_poke *pokes;
	uint32_t poke_count;
};

/*
 * One buffer of the test, twice: in system memory and in VRAM (seen by
 * the CPU through the BAR1 window), each with its address in the work
 * space and whether each step was made.
 */
struct test_memory {
	struct nvrtx_buffer buffer;
	struct nvrtx_bo bo;
	uint8_t *bo_cpu;
	uint64_t va;
	uint64_t bo_va;
	uint32_t bytes;
	int mapped;
	int bo_made;
	int bo_mapped;
};

/*
 * The test's buffers, kept while the test runs, and which of them the
 * running choice takes from VRAM (bit n for buffer n).
 */
struct test_state {
	struct test_memory memory[TEST_BUFFERS];
	uint32_t vram;
};

/*
 * The picture control's split encoding: one strip in the frame (byte 251
 * of the picture control, after the strip's number), and the macroblock
 * rows the slice encoding covers (from row 0, the picture's eight rows; the
 * 16-bit row count at 254).
 */
#define TEST_STRIPS_AT			(200U + 248U)
#define TEST_STRIPS_ONE			(1U << 24)
#define TEST_ROWS_AT			(200U + 252U)
#define TEST_ROWS_ALL			((TEST_HEIGHT / TEST_TILE) << 16)

/* The picture control's macroblock count before the encoder's statistics: one row. */
#define TEST_MPEC_AT			(200U + 232U)
#define TEST_MPEC_ROW			(TEST_WIDTH / TEST_TILE)

/* One strip and every row. */
static const struct test_poke test_strip_rows[] = {
	{ TEST_STRIPS_AT, TEST_STRIPS_ONE },
	{ TEST_ROWS_AT, TEST_ROWS_ALL },
};

/* One strip, every row, and the statistics' threshold of one row. */
static const struct test_poke test_strip_rows_mpec[] = {
	{ TEST_STRIPS_AT, TEST_STRIPS_ONE },
	{ TEST_ROWS_AT, TEST_ROWS_ALL },
	{ TEST_MPEC_AT, TEST_MPEC_ROW },
};

/* One strip, every row, the application timer and the motion search's cache. */
static const struct test_poke test_strip_rows_more[] = {
	{ TEST_STRIPS_AT, TEST_STRIPS_ONE },
	{ TEST_ROWS_AT, TEST_ROWS_ALL },
	{ 504U, 81000000U },
	{ 508U, 81000000U },
	{ 768U + 144U, 16U },
	{ 768U + 148U, 1U },
};

/* Every row alone. */
static const struct test_poke test_rows[] = {
	{ TEST_ROWS_AT, TEST_ROWS_ALL },
};

/* One strip alone. */
static const struct test_poke test_strip[] = {
	{ TEST_STRIPS_AT, TEST_STRIPS_ONE },
};

/*
 * Which buffers a choice leaves out of the encode (its method is not
 * sent): the bitstream, the history, the input picture.
 */
#define TEST_NO_BITSTREAM		(1U << 5)
#define TEST_NO_HISTORY			(1U << 6)
#define TEST_NO_INPUT			(1U << 7)

/* The number of words in a list of pokes. */
#define TEST_POKES(list)		((uint32_t)(sizeof(list) / sizeof((list)[0])))

/*
 * The choices tried, in order: the split encoding filled in (one strip,
 * every row); with the statistics' threshold; with the application timer
 * and the motion search's cache; every row alone; one strip alone.  A
 * choice whose encode does not finish stops the channel, which is made
 * again for the next.
 */
static const struct test_attempt test_attempts[] = {
	{ 0U, 0U, 0U, 0U, 0xffffffffU, 0U, 0U, test_strip_rows, TEST_POKES(test_strip_rows) },
	{ 0U, 0U, 0U, 0U, 0xffffffffU, 0U, 0U, test_strip_rows_mpec, TEST_POKES(test_strip_rows_mpec) },
	{ 0U, 0U, 0U, NVRTX_NVENC_CONTROL_GPTIMER, 0xffffffffU, 0U, 0U, test_strip_rows_more, TEST_POKES(test_strip_rows_more) },
	{ 0U, 0U, 0U, 0U, 0xffffffffU, 0U, 0U, test_rows, TEST_POKES(test_rows) },
	{ 0U, 0U, 0U, 0U, 0xffffffffU, 0U, 0U, test_strip, TEST_POKES(test_strip) },
};

static const uint32_t test_buffer_bytes[TEST_BUFFERS] = {
	TEST_PICTURE_BYTES,
	TEST_PICTURE_BYTES,
	TEST_PICTURE_BYTES,
	TEST_PICTURE_BYTES,
	TEST_BITSTREAM_BYTES,
	TEST_BITSTREAM_BYTES,
	TEST_WORK_BYTES,
	TEST_HISTORY_BYTES,
};

static int test_memory_make(struct nvrtx_gpu *gpu, struct test_state *state);
static void test_memory_free(struct nvrtx_gpu *gpu, struct test_state *state);
static void test_pattern(uint8_t *picture, uint32_t shift);
static uint32_t test_tiled(uint32_t x, uint32_t y);
static int test_attempt(struct nvrtx_gpu *gpu, struct test_state *state, uint32_t number, const struct test_attempt *attempt, bool *clean);
static int test_encode(struct nvrtx_gpu *gpu, struct test_state *state, uint32_t number, const struct test_attempt *attempt, uint32_t index, bool *clean);
static void test_dump(const char *tag, const uint8_t *bytes, size_t length);
static uint8_t *test_cpu(struct test_state *state, enum test_buffer buffer);
static uint64_t test_va(const struct test_state *state, enum test_buffer buffer);

/*
 * Runs nvenc-test: encodes the two pictures under each choice in turn
 * until one gives both without an error, and logs what came back.
 *
 * Needs the encoder's channel.  Returns ENODEV without it, ETIMEDOUT
 * when an encode did not finish, EIO when no choice gave both pictures
 * without an error, another error when the memory could not be made.
 */
int
nvrtx_nvenc_test(
	struct nvrtx_gpu *gpu)
{
	struct test_state *state;
	uint8_t *picture;
	uint32_t before;
	uint32_t number;
	bool clean;
	bool stopped;
	bool ready;
	int error;

	/* Marks the start of the test in the stage log. */
	nvrtx_stage_mark("nvenc-test begin");

	/* The test runs on the encoder's channel. */
	ready = nvrtx_nvenc_ready(gpu);
	if (!ready) {
		nvrtx_stage_mark("nvenc-test fail: no encoder");
		return ENODEV;
	}

	/* The buffers' state, kept until the end of the test. */
	state = kern_calloc(1U, sizeof(*state));
	if (state == NULL)
		return ENOMEM;

	/* The channel errors reported before the test are not its own. */
	before = nvrtx_nvenc_notices(gpu);

	/* Makes and maps the buffers. */
	error = test_memory_make(gpu, state);

	/* Writes the two input pictures, the second the first moved right by one block, in system memory, then in VRAM. */
	if (error == 0) {
		state->vram = 0U;
		picture = test_cpu(state, TEST_INPUT_I);
		test_pattern(picture, 0U);
		picture = test_cpu(state, TEST_INPUT_P);
		test_pattern(picture, 1U);
		state->vram = TEST_ALL_BUFFERS;
		picture = test_cpu(state, TEST_INPUT_I);
		test_pattern(picture, 0U);
		picture = test_cpu(state, TEST_INPUT_P);
		test_pattern(picture, 1U);
	}

	/* Tries each choice until one gives both pictures clean; a channel a choice stopped is made again. */
	clean = false;
	stopped = false;
	for (number = 0U;
	     error == 0 && number < sizeof(test_attempts) / sizeof(test_attempts[0]);
	     number++) {
		error = test_attempt(gpu, state, number + 1U, &test_attempts[number], &clean);
		if (clean)
			break;

		/* The channel stopped: noted, so the memory stays, and made again for the next choice. */
		if (error == ETIMEDOUT) {
			stopped = true;
			nvrtx_nvenc_report_notices(gpu, before, "nvenc-test");
			before = nvrtx_nvenc_notices(gpu);
			error = nvrtx_nvenc_restart(gpu);
		}
	}

	/* Reads what the GSP sent meanwhile and counts the channel errors. */
	nvrtx_nvenc_report_notices(gpu, before, "nvenc-test");

	/* Leaves the memory to an engine that may still write it. */
	if (stopped) {
		nvrtx_stage_mark("nvenc-test: an encode did not finish; the test's memory stays");
	} else {
		/* Gives the memory back. */
		test_memory_free(gpu, state);
		kern_free(state);
	}

	/* Propagates a failure to make the memory or the channel again. */
	if (error != 0) {
		nvrtx_stage_mark("nvenc-test fail: %d", error);
		return error;
	}

	/* No choice gave both pictures without an error. */
	if (!clean) {
		nvrtx_stage_mark("nvenc-test fail: no choice encoded both pictures cleanly");
		return EIO;
	}

	/* Succeeded: both pictures were encoded without an error. */
	nvrtx_stage_mark("nvenc-test ok: choice %u", (unsigned)number + 1U);
	return 0;
}

/*
 * Makes the test's buffers in system memory and maps each into the work
 * channel's space.  What was made stays recorded for test_memory_free().
 */
static int
test_memory_make(
	struct nvrtx_gpu *gpu,
	struct test_state *state)
{
	struct test_memory *memory;
	uint64_t *pages;
	uint32_t count;
	uint32_t page;
	uint32_t index;
	int error;

	/* Makes and maps each buffer in turn. */
	for (index = 0U; index < TEST_BUFFERS; index++) {
		memory = &state->memory[index];
		memory->bytes = test_buffer_bytes[index];

		/* The buffer, cleared. */
		error = nvrtx_buffer_alloc(gpu, memory->bytes, &memory->buffer);
		if (error != 0) {
			nvrtx_stage_mark("nvenc-test fail: no memory for buffer %u (%d)", (unsigned)index, error);
			return error;
		}

		/* The buffer's pages. */
		count = (memory->bytes + NVRTX_GSP_PAGE_SIZE - 1U) / NVRTX_GSP_PAGE_SIZE;
		pages = kern_calloc(count, sizeof(*pages));
		if (pages == NULL)
			return ENOMEM;

		/* Each page's device address, in order. */
		for (page = 0U; page < count; page++)
			pages[page] = memory->buffer.dma.device_address + (uint64_t)page * NVRTX_GSP_PAGE_SIZE;

		/* Maps the pages into the work space. */
		error = nvrtx_work_map_system(gpu, pages, count, &memory->va);
		kern_free(pages);
		if (error != 0) {
			nvrtx_stage_mark("nvenc-test fail: buffer %u not mapped (%d)", (unsigned)index, error);
			return error;
		}

		/* Marks the buffer mapped, so the free unmaps it. */
		memory->mapped = 1;

		/* The buffer's twin in VRAM. */
		error = nvrtx_bo_alloc(gpu, memory->buffer.dma.size, NVRTX_GSP_PAGE_SIZE, &memory->bo);
		if (error != 0) {
			nvrtx_stage_mark("nvenc-test fail: no VRAM for buffer %u (%d)", (unsigned)index, error);
			return error;
		}

		/* Marks the twin made, so the free gives it back. */
		memory->bo_made = 1;

		/* The CPU's view of the twin, through the window. */
		error = nvrtx_bo_map(gpu, &memory->bo);
		if (error != 0) {
			nvrtx_stage_mark("nvenc-test fail: buffer %u's VRAM not in the window (%d)", (unsigned)index, error);
			return error;
		}
		memory->bo_cpu = (uint8_t *)nvrtx_bo_cpu(gpu, &memory->bo);
		kern_memset(memory->bo_cpu, 0, memory->buffer.dma.size);

		/* Maps the twin into the work space. */
		error = nvrtx_work_map_vram(gpu, memory->bo.vram, memory->bo.size, &memory->bo_va);
		if (error != 0) {
			nvrtx_stage_mark("nvenc-test fail: buffer %u's VRAM not mapped (%d)", (unsigned)index, error);
			return error;
		}

		/* Marks the twin mapped, so the free unmaps it. */
		memory->bo_mapped = 1;
	}

	/* Succeeded: every buffer is in the work space. */
	return 0;
}

/* Unmaps and frees the buffers test_memory_make() made. */
static void
test_memory_free(
	struct nvrtx_gpu *gpu,
	struct test_state *state)
{
	struct test_memory *memory;
	uint32_t index;
	int error;

	/* Each buffer: its mapping, then its memory. */
	for (index = 0U; index < TEST_BUFFERS; index++) {
		memory = &state->memory[index];

		/* Gives the addresses back; on a failure the memory stays, as the work space may still name it. */
		if (memory->mapped != 0) {
			error = nvrtx_work_unmap(gpu, memory->va, (uint64_t)memory->buffer.dma.size);
			if (error != 0) {
				nvrtx_stage_mark("nvenc-test: buffer %u not unmapped (%d); its memory stays", (unsigned)index, error);
				continue;
			}
			memory->mapped = 0;
		}

		/* The memory. */
		nvrtx_buffer_free(gpu, &memory->buffer);

		/* The twin's addresses, then its VRAM; on a failure the VRAM stays. */
		if (memory->bo_mapped != 0) {
			error = nvrtx_work_unmap(gpu, memory->bo_va, memory->bo.size);
			if (error != 0) {
				nvrtx_stage_mark("nvenc-test: buffer %u's VRAM not unmapped (%d); it stays", (unsigned)index, error);
				continue;
			}
			memory->bo_mapped = 0;
		}
		if (memory->bo_made != 0) {
			nvrtx_bo_free(gpu, &memory->bo);
			memory->bo_made = 0;
		}
	}
}

/*
 * Writes one input picture in 16x16 tiles: the luma of each block from
 * its place, the blocks moved right by shift blocks (the ones past the
 * right edge coming in on the left), and the one chroma colour.
 */
static void
test_pattern(
	uint8_t *picture,
	uint32_t shift)
{
	uint32_t blocks_x;
	uint32_t source;
	uint32_t value;
	uint32_t x;
	uint32_t y;

	/* Every luma pixel takes the value of the block it came from. */
	blocks_x = TEST_WIDTH / TEST_TILE;
	for (y = 0U; y < TEST_HEIGHT; y++) {
		for (x = 0U; x < TEST_WIDTH; x++) {
			source = (x / TEST_TILE + blocks_x - shift % blocks_x) % blocks_x;
			value = TEST_LUMA_BASE + (source * TEST_LUMA_STEP_X + (y / TEST_TILE) * TEST_LUMA_STEP_Y) % TEST_LUMA_RANGE;
			picture[test_tiled(x, y)] = (uint8_t)value;
		}
	}

	/* Every chroma pair, Cb then Cr. */
	for (y = 0U; y < TEST_HEIGHT / 2U; y++) {
		for (x = 0U; x < TEST_WIDTH; x += 2U) {
			picture[TEST_CHROMA_AT + test_tiled(x, y)] = TEST_CB;
			picture[TEST_CHROMA_AT + test_tiled(x + 1U, y)] = TEST_CR;
		}
	}
}

/*
 * Reports where the byte of column x and row y of a plane lies in 16x16
 * tiles: the tiles in rows of the pitch, each tile's 256 bytes row by row.
 */
static uint32_t
test_tiled(
	uint32_t x,
	uint32_t y)
{
	uint32_t tile;
	uint32_t inside;

	/* The tile's start, then the byte's place in it. */
	tile = (y / TEST_TILE) * TEST_PITCH * TEST_TILE + (x / TEST_TILE) * TEST_TILE_BYTES;
	inside = (y % TEST_TILE) * TEST_TILE + x % TEST_TILE;

	/* Succeeded: the byte's offset in the plane. */
	return tile + inside;
}

/*
 * Runs one choice: the IDR picture, then the P picture (unless the IDR
 * picture's encode did not finish).  clean is set when both came back
 * without an error and with a bitstream.
 */
static int
test_attempt(
	struct nvrtx_gpu *gpu,
	struct test_state *state,
	uint32_t number,
	const struct test_attempt *attempt,
	bool *clean)
{
	bool clean_i;
	bool clean_p;
	int error;

	/* Names the choice in the stage log. */
	nvrtx_stage_mark("nvenc-test choice %u: codec %u, control %08x/%08x, flags %x, watchdog %x, %u words", (unsigned)number,
	    (unsigned)attempt->codec_field, (unsigned)attempt->control_extra, (unsigned)attempt->control_mask, (unsigned)attempt->flags,
	    (unsigned)attempt->watchdog, (unsigned)attempt->poke_count);

	/* The buffers the choice takes from VRAM. */
	state->vram = 0U;
	if ((attempt->flags & TEST_VRAM_PICTURES) != 0U)
		state->vram = TEST_PICTURE_BUFFERS;
	if ((attempt->flags & TEST_VRAM_ALL) != 0U)
		state->vram = TEST_ALL_BUFFERS;

	/* The IDR picture. */
	clean_i = false;
	error = test_encode(gpu, state, number, attempt, 0U, &clean_i);
	if (error != 0)
		return error;

	/* The P picture, which refers to the IDR picture's reconstruction. */
	clean_p = false;
	error = test_encode(gpu, state, number, attempt, 1U, &clean_p);
	if (error != 0)
		return error;

	/* Both pictures came back clean. */
	if (clean_i && clean_p)
		*clean = true;

	/* Succeeded: both encodes finished. */
	return 0;
}

/*
 * Encodes one picture of a choice (index 0 the IDR picture, 1 the P
 * picture), then logs its status, its bitstream and the start of its
 * reconstruction.  clean is set when the status shows no error and a
 * bitstream.
 */
static int
test_encode(
	struct nvrtx_gpu *gpu,
	struct test_state *state,
	uint32_t number,
	const struct test_attempt *attempt,
	uint32_t index,
	bool *clean)
{
	struct nvrtx_nvenc_h264_picture picture;
	struct nvrtx_nvenc_frame frame;
	enum test_buffer input;
	enum test_buffer output;
	enum test_buffer bits;
	const uint8_t *status;
	uint8_t *bitstream;
	uint8_t *reconstruction;
	uint8_t *work;
	uint32_t nvenc_class;
	uint32_t magic;
	uint32_t poke;
	char tag[24];
	char kind;
	uint32_t error_word;
	uint32_t total_bits;
	uint32_t start;
	uint32_t length;
	uint32_t at;
	int error;

	/* The picture's buffers. */
	input = TEST_INPUT_I;
	output = TEST_OUTPUT_I;
	bits = TEST_BITS_I;
	kind = 'I';
	if (index != 0U) {
		input = TEST_INPUT_P;
		output = TEST_OUTPUT_P;
		bits = TEST_BITS_P;
		kind = 'P';
	}

	/* The picture: 256x128 at QP 26, the IDR picture first, both references for what follows. */
	kern_memset(&picture, 0, sizeof(picture));
	picture.width = TEST_WIDTH;
	picture.height = TEST_HEIGHT;
	picture.luma_pitch = TEST_PITCH;
	picture.chroma_pitch = TEST_PITCH;
	picture.chroma_units = 0U;
	if ((attempt->flags & TEST_CHROMA_OFFSET) != 0U)
		picture.chroma_units = TEST_CHROMA_AT / TEST_TILE_BYTES;
	picture.profile_idc = TEST_PROFILE_MAIN;
	picture.level_idc = TEST_LEVEL;
	picture.frame_num = index;
	picture.poc_lsb = index * TEST_POC_STEP;
	picture.idr_pic_id = 0U;
	picture.poc_distance = TEST_POC_STEP;
	picture.qp = TEST_QP;
	picture.history_bytes = TEST_HISTORY_BYTES;
	picture.bitstream_bytes = TEST_BITSTREAM_BYTES;
	picture.codec_field = attempt->codec_field;
	picture.idr = index == 0U;
	picture.reference = true;

	/* The setup, which starts with the magic word of the channel's class. */
	nvenc_class = nvrtx_nvenc_class_of(gpu);
	magic = nvrtx_nvenc_magic(nvenc_class);
	work = test_cpu(state, TEST_WORK);
	nvrtx_nvenc_h264_setup(work + TEST_SETUP_AT + index * TEST_SETUP_STRIDE, magic, &picture);

	/* The choice's own words over the builder's. */
	for (poke = 0U; poke < attempt->poke_count; poke++)
		nvrtx_put32(work + TEST_SETUP_AT + index * TEST_SETUP_STRIDE, attempt->pokes[poke].at, attempt->pokes[poke].value);

	/* An unwritten status and an empty bitstream buffer. */
	kern_memset(work + TEST_STATUS_AT + index * TEST_STATUS_STRIDE, TEST_STATUS_UNWRITTEN, NVRTX_NVENC_STATUS_BYTES);
	bitstream = test_cpu(state, bits);
	kern_memset(bitstream, 0, TEST_BITSTREAM_BYTES);

	/* The encode's addresses: the input, the reconstruction and, for the P picture, the IDR picture's. */
	kern_memset(&frame, 0, sizeof(frame));
	frame.setup = test_va(state, TEST_WORK) + TEST_SETUP_AT + index * TEST_SETUP_STRIDE;
	frame.status = test_va(state, TEST_WORK) + TEST_STATUS_AT + index * TEST_STATUS_STRIDE;
	frame.bitstream = test_va(state, bits);
	frame.history = test_va(state, TEST_HISTORY);
	frame.rc_process = test_va(state, TEST_WORK) + TEST_RC_AT;
	frame.coloc_out = test_va(state, TEST_WORK) + TEST_COLOC_AT + index * TEST_COLOC_STRIDE;
	frame.input_luma = test_va(state, input);
	frame.input_chroma = test_va(state, input) + TEST_CHROMA_AT;
	frame.output_luma = test_va(state, output);
	frame.output_chroma = test_va(state, output) + TEST_CHROMA_AT;
	if (index != 0U) {
		frame.reference_luma = test_va(state, TEST_OUTPUT_I);
		frame.reference_chroma = test_va(state, TEST_OUTPUT_I) + TEST_CHROMA_AT;
	}
	frame.serial = (number - 1U) * TEST_PICTURES + index;
	frame.control = (NVRTX_NVENC_CONTROL_H264 | NVRTX_NVENC_CONTROL_OUT_PIC | attempt->control_extra) & attempt->control_mask;

	/* The choice's way of giving the buffers. */
	if ((attempt->flags & TEST_NO_OPTIONAL) != 0U) {
		frame.rc_process = 0U;
		frame.coloc_out = 0U;
	}
	frame.watchdog = attempt->watchdog;
	if ((attempt->flags & TEST_NO_BITSTREAM) != 0U)
		frame.bitstream = 0U;
	if ((attempt->flags & TEST_NO_HISTORY) != 0U)
		frame.history = 0U;
	if ((attempt->flags & TEST_NO_INPUT) != 0U) {
		frame.input_luma = 0U;
		frame.input_chroma = 0U;
	}
	if ((attempt->flags & TEST_RC_DATA) != 0U)
		frame.rc_data = test_va(state, TEST_WORK) + TEST_RC_DATA_AT;
	if ((attempt->flags & TEST_CHROMA_OFFSET) != 0U) {
		frame.input_chroma = frame.input_luma;
		frame.output_chroma = frame.output_luma;
		frame.reference_chroma = frame.reference_luma;
	}

	/* The push buffer, then the encode and the wait for it. */
	length = (uint32_t)nvrtx_nvenc_push_encode(work + TEST_PUSH_AT + index * TEST_PUSH_STRIDE, &frame);
	error = nvrtx_nvenc_run(gpu, test_va(state, TEST_WORK) + TEST_PUSH_AT + index * TEST_PUSH_STRIDE, length, TEST_ENCODE_US);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);

	/* The status's words. */
	status = work + TEST_STATUS_AT + index * TEST_STATUS_STRIDE;
	error_word = nvrtx_get32(status, NVRTX_NVENC_STATUS_ERROR_AT);
	total_bits = nvrtx_get32(status, NVRTX_NVENC_STATUS_BITS_AT);
	start = nvrtx_get32(status, NVRTX_NVENC_STATUS_START_AT);
	nvrtx_stage_mark("nvenc-test %u %c: run %d, error %08x, %u bits from %u", (unsigned)number, kind, error, (unsigned)error_word,
	    (unsigned)total_bits, (unsigned)start);

	/* The status's bytes. */
	(void)kern_snprintf(tag, sizeof(tag), "c%u-%c-status", (unsigned)number, kind);
	test_dump(tag, status, NVRTX_NVENC_STATUS_BYTES);

	/* An encode that did not finish leaves nothing more to read. */
	if (error != 0)
		return error;

	/* The bitstream: as long as the status says, else up to its last byte that is not zero; at most the log's share. */
	length = 0U;
	if (start < TEST_BITSTREAM_BYTES && total_bits / 8U <= TEST_BITSTREAM_BYTES - start)
		length = start + (total_bits + 7U) / 8U;
	if (length == 0U || total_bits == 0U) {
		/* The status gave no size: the bytes up to the last one that is not zero. */
		for (at = TEST_BITSTREAM_BYTES; at > 0U; at--) {
			if (bitstream[at - 1U] != 0U)
				break;
		}
		length = at;
	}
	if (length > TEST_DUMP_BITSTREAM_MAX)
		length = TEST_DUMP_BITSTREAM_MAX;
	(void)kern_snprintf(tag, sizeof(tag), "c%u-%c-bits", (unsigned)number, kind);
	test_dump(tag, bitstream, length);

	/* The start of the reconstruction: the first two luma tiles and the first chroma tile. */
	reconstruction = test_cpu(state, output);
	(void)kern_snprintf(tag, sizeof(tag), "c%u-%c-luma", (unsigned)number, kind);
	test_dump(tag, reconstruction, TEST_DUMP_LUMA_BYTES);
	(void)kern_snprintf(tag, sizeof(tag), "c%u-%c-chroma", (unsigned)number, kind);
	test_dump(tag, reconstruction + TEST_CHROMA_AT, TEST_DUMP_CHROMA_BYTES);

	/* A clean picture: a written status without an error, and a bitstream. */
	if (error_word == 0U &&
	    total_bits != 0U &&
	    length != 0U)
		*clean = true;

	/* Succeeded: the encode finished. */
	return 0;
}

/*
 * Writes a run of bytes to the kernel log in lines of 32: the tag, the
 * line's offset, its bytes in hexadecimal and its FNV-1a hash; then the
 * run's length and the hash of the whole run.
 */
static void
test_dump(
	const char *tag,
	const uint8_t *bytes,
	size_t length)
{
	char hex[TEST_LINE_BYTES * 2U + 1U];
	static const char digits[] = "0123456789abcdef";
	uint32_t line_hash;
	uint32_t hash;
	size_t at;
	size_t end;
	size_t index;

	/* Each line of up to 32 bytes, and the whole run's hash. */
	hash = TEST_FNV_START;
	for (at = 0U; at < length; at += TEST_LINE_BYTES) {
		end = at + TEST_LINE_BYTES;
		if (end > length)
			end = length;

		/* The line's bytes in hexadecimal, and both hashes over them. */
		line_hash = TEST_FNV_START;
		for (index = at; index < end; index++) {
			hex[(index - at) * 2U] = digits[bytes[index] >> 4];
			hex[(index - at) * 2U + 1U] = digits[bytes[index] & 0xfU];
			line_hash = (line_hash ^ bytes[index]) * TEST_FNV_PRIME;
			hash = (hash ^ bytes[index]) * TEST_FNV_PRIME;
		}
		hex[(end - at) * 2U] = '\0';

		kern_logf("nvrtx: nvenc-dump %s %05x %s %08x\n", tag, (unsigned)at, hex, (unsigned)line_hash);
	}

	/* Closes the run with its length and hash. */
	kern_logf("nvrtx: nvenc-sum %s %u %08x\n", tag, (unsigned)length, (unsigned)hash);
}

/* Reports the CPU's address of one of the test's buffers. */
static uint8_t *
test_cpu(
	struct test_state *state,
	enum test_buffer buffer)
{
	/* The VRAM twin, when the running choice takes the buffer from VRAM. */
	if (((state->vram >> buffer) & 1U) != 0U)
		return state->memory[buffer].bo_cpu;

	/* Succeeded: the coherent memory's address. */
	return (uint8_t *)state->memory[buffer].buffer.dma.address;
}

/* Reports the work space's address of one of the test's buffers. */
static uint64_t
test_va(
	const struct test_state *state,
	enum test_buffer buffer)
{
	/* The VRAM twin, when the running choice takes the buffer from VRAM. */
	if (((state->vram >> buffer) & 1U) != 0U)
		return state->memory[buffer].bo_va;

	/* Succeeded: where the buffer is mapped. */
	return state->memory[buffer].va;
}
