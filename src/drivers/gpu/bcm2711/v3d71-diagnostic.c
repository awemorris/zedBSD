/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The V3D 7.1 engine's self-test (stages V5 to V9).
 *
 * After identification, the MMU and the interrupts are up, the self-test
 * runs one job of every kind the engine has and checks what each wrote:
 *
 *   V5  a shader-free 1x1 bin/render job (both completion interrupts)
 *   V6  a 64x64 RGBA8 clear stored to memory (every pixel)
 *   V7  a texture formatting job, raster to UB-linear (every texel), then
 *       raster to strict UIF for the shapes and texel sizes textures
 *       have; when every texel lands where the CPU conversion puts it,
 *       texture conversion uses the unit (v3d.tfu=0 keeps the CPU's)
 *   V8  a compute dispatch that does nothing, then one that stores a word
 *   V9  a bin list that branches to itself: the hang is detected, the
 *       engine is reset through its sequencer and power manager, and the
 *       noop job runs again
 *
 * Each stage logs one line with its result, and a last line sums them up,
 * so the outcome can be read with dmesg.  A failed stage stops the later
 * ones; the engine is reset when a job failed after its launch, and the
 * storage is released only when no job can still reach it.
 */

#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/buffer.h"
#include "drivers/gpu/bcm2711/native-state.h"
#include "drivers/gpu/bcm2711/v3d-job.h"
#include "drivers/gpu/bcm2711/v3d71-private.h"

/* One mapped run keeps the command lists, tile state, pool, overflow, targets and shaders together. */
#define DIAGNOSTIC_ADDRESS 0x100000U
#define DIAGNOSTIC_BYTES 0x200000U
#define DIAGNOSTIC_RENDER_OFFSET 0x1000U
#define DIAGNOSTIC_TILE_OFFSET 0x2000U
#define DIAGNOSTIC_STATE_OFFSET 0x3000U
#define DIAGNOSTIC_POOL_OFFSET 0x4000U
#define DIAGNOSTIC_OVERFLOW_OFFSET 0x90000U
#define DIAGNOSTIC_TARGET_OFFSET 0x100000U
#define DIAGNOSTIC_TFU_OFFSET 0x140000U
#define DIAGNOSTIC_SHADER_OFFSET 0x1c0000U
#define DIAGNOSTIC_UNIFORM_OFFSET 0x1c1000U
#define DIAGNOSTIC_STORE_OFFSET 0x1c2000U

/* The bytes after a UIF conversion's output that must keep their fill. */
#define DIAGNOSTIC_TFU_GUARD 64U

/* The clear colour and the word the compute shader stores. */
#define DIAGNOSTIC_COLOR 0xff317ce0U
#define DIAGNOSTIC_STORE_VALUE 0x5a17c0deU

/*
 * The compute dispatch's configuration: one workgroup of 16 invocations in
 * one supergroup, a shader that runs in a single segment with four threads.
 *
 * Values referenced: Mesa 25.3.6 src/broadcom/common/v3d_csd.h (the
 * fields of configuration words 0 to 5) and vulkan/v3dv_cmd_buffer.c lines
 * 4300-4361 (the supergroup, the batch count, which 7.1.6 and later do not
 * write minus one, and the reserved NaN bit of 7.x).
 */
#define CSD_ONE_WORKGROUP 0x10000U
#define CSD_SUPERGROUP_ONE_BATCH_SIXTEEN 0x110U
#define CSD_SINGLE_SEGMENT 0x2U
#define CSD_FOUR_THREADS 0x1U
#define CSD_BATCH_MINUS_ONE_BEFORE 6U

/*
 * The texture formatting job: raster RGBA8 in, UB-linear one UIF block
 * wide out.
 *
 * Values referenced: Mesa 25.3.6 src/broadcom/common/v3d_tfu.h lines 50-67
 * (the 7.1 field positions) and cle/v3d_packet.xml line 337 (RGBA8 is
 * texture type 4).
 */
#define TFU_TEXTURE_RGBA8 (4U << 16)
#define TFU_INPUT_RASTER (0U << 23)
#define TFU_OUTPUT_UBLINEAR_ONE_COLUMN (4U << 12)

/* The stage results the summary line shows, in stage order. */
enum diagnostic_step {
	STEP_NOOP,
	STEP_CLEAR,
	STEP_TFU,
	STEP_CSD,
	STEP_RESET,
	STEP_COUNT
};

/* The names of the steps in the summary line. */
static const char *const diagnostic_step_names[STEP_COUNT] = {
	"noop", "clear", "tfu", "csd", "reset"
};

static int prepare_storage(struct bcm2711_v3d *engine);
static int retire_storage(struct bcm2711_v3d *engine);
static void lend_lists(struct bcm2711_v3d *engine, struct bcm2711_v3d_noop *lists);
static void cl_job(const struct bcm2711_v3d_noop *lists, struct bcm2711_v3d_job *job);
static int run_noop(struct bcm2711_v3d *engine, const char *stage);
static int run_clear(struct bcm2711_v3d *engine);
static int run_tfu(struct bcm2711_v3d *engine);
static void check_tfu_uif(struct bcm2711_v3d *engine);
static int run_tfu_uif(struct bcm2711_v3d *engine, enum bcm2711_native_texel texel, uint32_t width, uint32_t height, uint32_t pitch, uint32_t offset);
static int run_compute(struct bcm2711_v3d *engine, bool store);
static int run_timeout(struct bcm2711_v3d *engine);
static uint32_t pattern_pixel(uint32_t x, uint32_t y);
static void summary(const int *results);

/*
 * Runs the self-test jobs before any client can submit work.
 *
 * Returns 0 when every stage passed.  A failure after a launch resets the
 * engine (boot owns all of its DMA here) and leaves the diagnostic storage
 * retained when the reset fails.
 */
int
bcm2711_v3d_diagnostic(
	struct bcm2711_v3d *engine)
{
	int results[STEP_COUNT];
	unsigned step;
	bool allowed;
	int error;
	int reset_error;
	int retired;

	/* Marks every step as not run. */
	for (step = 0; step < STEP_COUNT; step++)
		results[step] = ECANCELED;

	/* Honors the ordinary stop before any storage is mapped. */
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V5");
	if (!allowed)
		return ECANCELED;
	if (!engine->hardware.ready || engine->hardware.diagnostic != NULL)
		return EBUSY;
	error = prepare_storage(engine);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V5 storage unavailable (%d)", error);
		return error;
	}

	/* Runs the noop job, which proves both command-list queues and their interrupts. */
	error = run_noop(engine, "V5");
	results[STEP_NOOP] = error;

	/* Runs the clear, which proves the tile buffer's store to memory. */
	if (error == 0) {
		allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V6");
		error = ECANCELED;
		if (allowed)
			error = run_clear(engine);
		results[STEP_CLEAR] = error;
	}

	/* Runs the texture formatting unit, which completes on the hub's line. */
	if (error == 0) {
		allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V7");
		error = ECANCELED;
		if (allowed)
			error = run_tfu(engine);
		results[STEP_TFU] = error;

		/* The unit's UIF output decides whether textures are converted by it; a mismatch is not a failure. */
		if (error == 0)
			check_tfu_uif(engine);
	}

	/* Runs the empty compute dispatch, then the one that stores a word. */
	if (error == 0) {
		allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V8");
		error = ECANCELED;
		if (allowed)
			error = run_compute(engine, false);
		if (error == 0)
			error = run_compute(engine, true);
		results[STEP_CSD] = error;
	}

	/* Hangs a bin list on purpose and proves the reset. */
	if (error == 0) {
		allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V9");
		error = ECANCELED;
		if (allowed)
			error = run_timeout(engine);
		results[STEP_RESET] = error;
	}

	/* Resets an engine left faulted by a failed job before its storage goes. */
	if (error != 0 && error != ECANCELED) {
		/* A failed power reset supplies no proof the storage is unreachable. */
		if (!engine->power.ready) {
			bcm2711_stage_mark(BCM2711_FAMILY_V3D, "self-test: power reset failed; storage retained");
			summary(results);
			return error;
		}

		/* Boot owns the whole engine here, so a reset cannot disturb other work. */
		reset_error = bcm2711_v3d_hardware_reset(engine);
		if (reset_error != 0) {
			bcm2711_stage_mark(BCM2711_FAMILY_V3D, "self-test: error %d, reset %d; storage retained", error, reset_error);
			summary(results);
			return error;
		}
	}

	/* Unmaps and releases the storage once no job or cached translation can reach it. */
	retired = retire_storage(engine);
	summary(results);
	if (retired != 0)
		return retired;
	if (error != 0)
		return error;

	/* Succeeded: every job ran, completed by interrupt, and wrote what it should. */
	return 0;
}

/* Builds and runs the 1x1 noop job, logging both queues' completions. */
static int
run_noop(
	struct bcm2711_v3d *engine,
	const char *stage)
{
	struct bcm2711_v3d_noop lists;
	struct bcm2711_v3d_job job;
	struct bcm2711_v3d_job_result result;
	uint8_t *storage;
	int error;

	/* Clears the pool and the tile state, then builds the lists into the storage. */
	storage = engine->hardware.diagnostic->address;
	kern_memset(storage + DIAGNOSTIC_POOL_OFFSET, 0, BCM2711_V3D_NOOP_POOL_BYTES);
	kern_memset(storage + DIAGNOSTIC_STATE_OFFSET, 0, 4096);
	lend_lists(engine, &lists);
	error = bcm2712_v3d_noop_prepare(&lists);
	if (error != 0)
		return error;

	/* Describes the job, then makes the lists and the zeroes visible to the engine. */
	cl_job(&lists, &job);
	kern_dcache_clean_range(storage, DIAGNOSTIC_BYTES);
	kern_io_write_barrier();
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "%s noop bin/render begin", stage);
	error = bcm2711_v3d_job_run(engine, &job, &result);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "%s noop failed (%d)", stage, error);
		return error;
	}

	/* Shows the interrupts and the frame counters the job left. */
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"%s noop ok IRQ %x/%x BFC %u RFC %u overflow %u",
		stage,
		result.core_events,
		result.hub_events,
		result.bin_counter,
		result.render_counter,
		result.overflow_used);

	/* Succeeded: both queues completed by interrupt. */
	return 0;
}

/* Clears one 64x64 raster tile and checks every pixel the engine wrote. */
static int
run_clear(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_clear clear;
	struct bcm2711_v3d_job job;
	struct bcm2711_v3d_job_result result;
	uint8_t *storage;
	uint32_t *target;
	uint32_t index;
	uint32_t pixel;
	int error;

	/* A sentinel target makes a missing store distinguishable from the colour. */
	storage = engine->hardware.diagnostic->address;
	target = (uint32_t *)(void *)(storage + DIAGNOSTIC_TARGET_OFFSET);
	kern_memset(storage + DIAGNOSTIC_POOL_OFFSET, 0, BCM2711_V3D_NOOP_POOL_BYTES);
	kern_memset(storage + DIAGNOSTIC_STATE_OFFSET, 0, 4096);
	kern_memset(target, 0xa5, 16384);

	/* Builds the clear job into the storage. */
	kern_memset(&clear, 0, sizeof(clear));
	lend_lists(engine, &clear.lists);
	clear.width = 64;
	clear.height = 64;
	clear.stride = 256;
	clear.target_address = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_TARGET_OFFSET;
	clear.target_bytes = 16384;
	clear.color = DIAGNOSTIC_COLOR;
	error = bcm2712_v3d_clear_prepare(&clear);
	if (error != 0)
		return error;

	/* Runs the job with every list and the sentinel in RAM. */
	cl_job(&clear.lists, &job);
	job.command.cl.clean_output = true;
	kern_dcache_clean_range(storage, DIAGNOSTIC_BYTES);
	kern_io_write_barrier();
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V6 clear/store begin");
	error = bcm2711_v3d_job_run(engine, &job, &result);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V6 clear failed (%d)", error);
		return error;
	}

	/* Reads the target back past the CPU caches and checks every pixel. */
	kern_dcache_invalidate_range(target, 16384);
	kern_io_read_barrier();
	for (index = 0; index < 4096; index++) {
		/* Refuses the first pixel that is not the colour. */
		pixel = target[index];
		if (pixel != DIAGNOSTIC_COLOR) {
			bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V6 clear mismatch pixel %u %08x", index, pixel);
			return EIO;
		}
	}

	/* Shows the pass only after every pixel was inspected. */
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V6 clear ok RGBA8 64x64 all 4096 pixels %08x", DIAGNOSTIC_COLOR);

	/* Succeeded: the store, the interrupt and CPU visibility agree. */
	return 0;
}

/* Converts a raster pattern to one UB-linear block and checks every texel. */
static int
run_tfu(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_job job;
	struct bcm2711_v3d_job_result result;
	uint8_t *storage;
	uint32_t *input;
	uint32_t *output;
	uint32_t x;
	uint32_t y;
	uint32_t offset;
	uint32_t expected;
	int error;

	/* Fills eight raster rows whose pixels carry their own coordinates. */
	storage = engine->hardware.diagnostic->address;
	input = (uint32_t *)(void *)(storage + DIAGNOSTIC_TARGET_OFFSET);
	output = (uint32_t *)(void *)(storage + DIAGNOSTIC_TFU_OFFSET);
	for (y = 0; y < 8; y++) {
		/* Writes one row. */
		for (x = 0; x < 8; x++)
			input[y * 8U + x] = pattern_pixel(x, y);
	}

	/* The output sentinel cannot be mistaken for a conversion. */
	kern_memset(output, 0xa5, 256);

	/* Describes an 8x8 RGBA8 raster in, a UB-linear block out, with the 7.1 words. */
	kern_memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_TFU;
	job.command.tfu.input = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_TARGET_OFFSET;
	job.command.tfu.input_bytes = 256;
	job.command.tfu.output = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_TFU_OFFSET;
	job.command.tfu.output_bytes = 256;
	job.command.tfu.output_format = TFU_OUTPUT_UBLINEAR_ONE_COLUMN;
	job.command.tfu.stride = 8;
	job.command.tfu.size = 0x00080008;
	job.command.tfu.configuration = TFU_TEXTURE_RGBA8 | TFU_INPUT_RASTER;

	/* Runs the conversion with the input and the sentinel in RAM. */
	kern_dcache_clean_range(input, 256);
	kern_dcache_clean_range(output, 256);
	kern_io_write_barrier();
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V7 TFU raster to UB-linear begin");
	error = bcm2711_v3d_job_run(engine, &job, &result);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V7 TFU failed (%d)", error);
		return error;
	}

	/* Checks every texel at its place in the block's four 4x4 utiles. */
	kern_dcache_invalidate_range(output, 256);
	kern_io_read_barrier();
	for (y = 0; y < 8; y++) {
		/* Inspects one row through the tiled layout. */
		for (x = 0; x < 8; x++) {
			/* Refuses the first texel out of place. */
			offset = ((y >> 2) * 2U + (x >> 2)) * 16U;
			offset += (y & 3U) * 4U + (x & 3U);
			expected = pattern_pixel(x, y);
			if (output[offset] != expected) {
				bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V7 TFU mismatch %u,%u %08x", x, y, output[offset]);
				return EIO;
			}
		}
	}

	/* Shows the hub's completion and the checked layout. */
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V7 TFU ok IRQ %x, all 64 tiled texels", result.hub_events);

	/* Succeeded: the conversion matched for every texel. */
	return 0;
}

/*
 * Lets texture conversion use the unit only after its strict UIF output
 * matches the CPU conversion for every shape the runtime gives it: whole and
 * partial columns and block rows, a pitch wider than the row and not a
 * multiple of 64 bytes, a single pixel, an image starting inside its run, and
 * the one- and two-byte texels of a picture's planes.  A mismatch is reported
 * and leaves the CPU conversion in charge without failing the self-test; an
 * engine a failed job left faulted is reset (boot owns all of its DMA here).
 */
static void
check_tfu_uif(
	struct bcm2711_v3d *engine)
{
	int converted;
	int error;

	/* Three whole columns and five block rows with a padded pitch, then partial ones packed at 132 bytes. */
	converted = run_tfu_uif(engine, BCM2711_NATIVE_TEXEL_RGBA8, 96U, 40U, 512U, 0U);
	if (converted == 0)
		converted = run_tfu_uif(engine, BCM2711_NATIVE_TEXEL_RGBA8, 33U, 9U, 132U, 0U);

	/* One pixel, and an image 64 bytes into its run, partial in both directions. */
	if (converted == 0)
		converted = run_tfu_uif(engine, BCM2711_NATIVE_TEXEL_RGBA8, 1U, 1U, 4U, 0U);
	if (converted == 0)
		converted = run_tfu_uif(engine, BCM2711_NATIVE_TEXEL_RGBA8, 200U, 150U, 1024U, 64U);

	/* The largest the diagnostic run holds, which also gives the unit's speed. */
	if (converted == 0)
		converted = run_tfu_uif(engine, BCM2711_NATIVE_TEXEL_RGBA8, 256U, 255U, 1024U, 0U);

	/* A picture's planes: one-byte and two-byte texels, partial blocks. */
	if (converted == 0)
		converted = run_tfu_uif(engine, BCM2711_NATIVE_TEXEL_R8, 70U, 37U, 128U, 0U);
	if (converted == 0)
		converted = run_tfu_uif(engine, BCM2711_NATIVE_TEXEL_RG8, 35U, 19U, 128U, 0U);

	/* The outcome, which the boot line's v3d.tfu=0 overrides. */
	if (converted == 0 && bcm2711_stage_tfu_off()) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V7 TFU UIF self-test passed; v3d.tfu=0 keeps conversion on the CPU");
	} else if (converted == 0) {
		engine->hardware.tfu_uif_ready = true;
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V7 TFU UIF self-test passed; texture conversion uses the unit");
	} else if (engine->hardware.faulted) {
		error = bcm2711_v3d_hardware_reset(engine);
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V7 TFU UIF error %d; CPU conversion, reset %d", converted, error);
	} else {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V7 TFU UIF unavailable %d; CPU conversion", converted);
	}
}

/*
 * Converts one raster image of a texel size to strict UIF with the unit
 * and checks every visible texel at the place the CPU conversion puts it
 * (bcm2711_native_texture_offset) and the guard after the output.
 */
static int
run_tfu_uif(
	struct bcm2711_v3d *engine,
	enum bcm2711_native_texel texel,
	uint32_t width,
	uint32_t height,
	uint32_t pitch,
	uint32_t offset)
{
	struct bcm2711_native_conversion conversion;
	struct bcm2711_v3d_job job;
	struct bcm2711_v3d_job_result result;
	uint8_t *storage;
	uint8_t *input;
	uint8_t *output;
	uint32_t texel_bytes;
	uint32_t type;
	uint32_t bytes;
	uint32_t place;
	uint32_t block_height;
	uint32_t x;
	uint32_t y;
	uint32_t index;
	uint64_t started;
	uint64_t finished;
	uint64_t frequency;
	bool started_valid;
	bool finished_valid;
	int error;

	/* The texel's size and type, and the output's size; both images and the guard fit the diagnostic run. */
	error = bcm2711_native_texel_layout(texel, &texel_bytes, &type);
	if (error != 0)
		return error;
	error = bcm2711_native_texture_size_texel(width, height, texel_bytes, &bytes);
	if (error != 0)
		return error;
	if (offset + pitch * height > DIAGNOSTIC_TFU_OFFSET - DIAGNOSTIC_TARGET_OFFSET)
		return EINVAL;
	if (bytes + DIAGNOSTIC_TFU_GUARD > DIAGNOSTIC_SHADER_OFFSET - DIAGNOSTIC_TFU_OFFSET)
		return EINVAL;

	/* Each byte depends on its texel's coordinates and its place in the texel, so a misplaced one cannot match. */
	storage = engine->hardware.diagnostic->address;
	input = storage + DIAGNOSTIC_TARGET_OFFSET + offset;
	output = storage + DIAGNOSTIC_TFU_OFFSET;
	for (y = 0; y < height; y++) {
		/* Fills the visible part of each row. */
		for (x = 0; x < width * texel_bytes; x++)
			input[y * pitch + x] = (uint8_t)(x * 7U + y * 13U + 1U);
	}
	kern_memset(output, 0xa5, bytes + DIAGNOSTIC_TFU_GUARD);

	/* Raster in, strict UIF without XOR out over the image's whole block rows, as texture sampling uses it. */
	kern_memset(&conversion, 0, sizeof(conversion));
	conversion.type = type;
	conversion.texel_bytes = texel_bytes;
	conversion.tiling = BCM2711_NATIVE_TILING_UIF;
	conversion.height = height;
	block_height = texel_bytes == 1U ? 16U : 8U;
	conversion.padded_height = (height + block_height - 1U) / block_height * block_height;
	kern_memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_TFU;
	job.command.tfu.input = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_TARGET_OFFSET + offset;
	job.command.tfu.input_bytes = (height - 1U) * pitch + width * texel_bytes;
	job.command.tfu.output = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_TFU_OFFSET;
	job.command.tfu.output_bytes = bytes;
	job.command.tfu.stride = pitch / texel_bytes;
	job.command.tfu.size = (height << 16) | width;
	error = bcm2711_native_conversion_words(&conversion, &job.command.tfu.output_format, &job.command.tfu.configuration);
	if (error != 0)
		return error;
	kern_dcache_clean_range(input, pitch * height);
	kern_dcache_clean_range(output, bytes + DIAGNOSTIC_TFU_GUARD);
	kern_io_write_barrier();

	/* The counter brackets only the unit's job, including its interrupt wait. */
	started_valid = kern_rtc_read_counter(&started, &frequency);
	error = bcm2711_v3d_job_run(engine, &job, &result);
	if (error != 0)
		return error;
	finished_valid = kern_rtc_read_counter(&finished, &frequency);
	kern_dcache_invalidate_range(output, bytes + DIAGNOSTIC_TFU_GUARD);
	kern_io_read_barrier();
	if (started_valid && finished_valid && frequency != 0) {
		bcm2711_stage_mark(
			BCM2711_FAMILY_V3D,
			"V7 TFU UIF %ux%u x%u us=%llu",
			width,
			height,
			texel_bytes,
			(unsigned long long)((finished - started) * 1000000U / frequency));
	}

	/* The bytes after the UIF size keep their fill. */
	for (index = 0; index < DIAGNOSTIC_TFU_GUARD; index++) {
		if (output[bytes + index] != 0xa5U) {
			bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V7 TFU UIF %ux%u x%u wrote past %u bytes", width, height, texel_bytes, bytes);
			return EBADMSG;
		}
	}

	/* Every visible texel's bytes at the CPU conversion's place. */
	for (y = 0; y < height; y++) {
		/* One row's texels. */
		for (x = 0; x < width; x++) {
			/* Refuses the first texel out of place. */
			error = bcm2711_native_texture_offset(x, y, height, texel_bytes, &place);
			if (error != 0)
				return error;
			if (kern_memcmp(output + place, input + y * pitch + x * texel_bytes, texel_bytes) != 0) {
				bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V7 TFU UIF %ux%u x%u mismatch at %u,%u", width, height, texel_bytes, x, y);
				return EBADMSG;
			}
		}
	}

	/* Succeeded: the unit's layout is the CPU's for this shape. */
	return 0;
}

/*
 * Dispatches one workgroup of the empty shader, or of the shader that
 * stores a word, and checks the stored word.
 */
static int
run_compute(
	struct bcm2711_v3d *engine,
	bool store)
{
	struct bcm2712_v3d *v3d;
	struct bcm2711_v3d_job job;
	struct bcm2711_v3d_job_result result;
	const uint64_t *shader;
	uint32_t shader_words;
	uint8_t *storage;
	uint64_t *code;
	uint32_t *uniforms;
	volatile uint32_t *stored;
	uint32_t index;
	uint32_t value;
	int error;

	/* Picks the shader. */
	v3d = bcm2712_v3d_of(engine);
	shader = bcm2712_v3d_empty_shader;
	shader_words = BCM2712_V3D_EMPTY_SHADER_WORDS;
	if (store) {
		shader = bcm2712_v3d_store_shader;
		shader_words = BCM2712_V3D_STORE_SHADER_WORDS;
	}

	/* Copies the shader, writes its uniforms (the address and the value) and clears the word. */
	storage = engine->hardware.diagnostic->address;
	code = (uint64_t *)(void *)(storage + DIAGNOSTIC_SHADER_OFFSET);
	uniforms = (uint32_t *)(void *)(storage + DIAGNOSTIC_UNIFORM_OFFSET);
	stored = (volatile uint32_t *)(void *)(storage + DIAGNOSTIC_STORE_OFFSET);
	kern_memset(code, 0, 4096);
	for (index = 0; index < shader_words; index++)
		code[index] = shader[index];
	uniforms[0] = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_STORE_OFFSET;
	uniforms[1] = DIAGNOSTIC_STORE_VALUE;
	*stored = 0;

	/* Describes one workgroup of 16 invocations in a single segment with four threads. */
	kern_memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_COMPUTE;
	job.command.compute.configuration[0] = CSD_ONE_WORKGROUP;
	job.command.compute.configuration[1] = CSD_ONE_WORKGROUP;
	job.command.compute.configuration[2] = CSD_ONE_WORKGROUP;
	job.command.compute.configuration[3] = CSD_SUPERGROUP_ONE_BATCH_SIXTEEN;
	job.command.compute.configuration[4] = 1U;
	if (v3d->revision < CSD_BATCH_MINUS_ONE_BEFORE)
		job.command.compute.configuration[4] = 0;
	job.command.compute.configuration[5] = (DIAGNOSTIC_ADDRESS + DIAGNOSTIC_SHADER_OFFSET) | CSD_SINGLE_SEGMENT | CSD_FOUR_THREADS;
	job.command.compute.configuration[6] = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_UNIFORM_OFFSET;
	job.command.compute.shader_bytes = shader_words * 8U;
	job.command.compute.uniform_bytes = 8;

	/* Runs the dispatch with the code, the uniforms and the cleared word in RAM. */
	kern_dcache_clean_range(storage + DIAGNOSTIC_SHADER_OFFSET, 0x3000);
	kern_io_write_barrier();
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V8 CSD %s dispatch begin", store ? "store" : "empty");
	error = bcm2711_v3d_job_run(engine, &job, &result);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V8 CSD %s failed (%d)", store ? "store" : "empty", error);
		return error;
	}

	/* The empty shader proves completion; nothing was written. */
	if (!store) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V8 CSD empty ok IRQ %x", result.core_events);
		return 0;
	}

	/* Reads the word back past the CPU caches. */
	kern_dcache_invalidate_range((void *)(storage + DIAGNOSTIC_STORE_OFFSET), 64);
	kern_io_read_barrier();
	value = *stored;
	if (value != DIAGNOSTIC_STORE_VALUE) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V8 CSD store wrote %08x, not %08x", value, DIAGNOSTIC_STORE_VALUE);
		return EIO;
	}

	/* Shows the pass. */
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V8 CSD store ok IRQ %x word %08x", result.core_events, value);

	/* Succeeded: the QPUs ran the shader and its TMU write reached RAM. */
	return 0;
}

/* Hangs a bin list that branches to itself, resets the engine, then runs the noop job again. */
static int
run_timeout(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_job job;
	struct bcm2711_v3d_job_result result;
	uint8_t *storage;
	uint32_t index;
	int error;

	/* A branch to its own packet never ends and waits on nothing. */
	storage = engine->hardware.diagnostic->address;
	storage[0] = 16;
	for (index = 0; index < 4; index++)
		storage[index + 1] = (uint8_t)(DIAGNOSTIC_ADDRESS >> (index * 8U));

	/* Describes a bin queue of that one packet; render never starts. */
	kern_memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_CL;
	job.command.cl.bin_start = DIAGNOSTIC_ADDRESS;
	job.command.cl.bin_end = DIAGNOSTIC_ADDRESS + 5;
	job.command.cl.render_start = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_RENDER_OFFSET;
	job.command.cl.render_end = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_RENDER_OFFSET + BCM2712_V3D_NOOP_RENDER_BYTES;
	job.command.cl.pool_address = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_POOL_OFFSET;
	job.command.cl.pool_bytes = BCM2711_V3D_NOOP_POOL_BYTES;
	job.command.cl.state_address = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_STATE_OFFSET;
	job.command.cl.state_bytes = 256;

	/* Runs it and requires the hang to be detected, not a completion. */
	kern_dcache_clean_range(storage, 4096);
	kern_io_write_barrier();
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V9 self-branch hang begin");
	error = bcm2711_v3d_job_run(engine, &job, &result);
	if (error != ETIMEDOUT || result.retired) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V9 unexpected result %d retired %u", error, (unsigned)result.retired);
		return EIO;
	}
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V9 hang detected; reset begin");

	/* Resets the engine; success also checks it identifies as before. */
	error = bcm2711_v3d_hardware_reset(engine);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V9 reset failed (%d)", error);
		return error;
	}

	/* Runs the noop job again, which proves the reset engine works. */
	error = run_noop(engine, "V9");
	if (error != 0)
		return error;
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"V9 reset ok (%llu resets), noop after reset complete",
		(unsigned long long)engine->hardware.resets);

	/* Succeeded: the hang, the reset and a fresh job all happened. */
	return 0;
}

/* Allocates one contiguous run and maps the whole diagnostic interval. */
static int
prepare_storage(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_hardware *hardware;
	int error;

	/* Records the owner before any mapping can expose its RAM. */
	hardware = &engine->hardware;
	error = bcm2711_buffer_create(
		DIAGNOSTIC_BYTES,
		0x3fffffff,
		BCM2711_V3D_PAGE_BYTES,
		&hardware->diagnostic);
	if (error != 0)
		return error;

	/* A refused software mapping changes no PTE and permits an immediate unwind. */
	error = bcm2711_v3d_pages_map(
		hardware->pages->address,
		DIAGNOSTIC_ADDRESS,
		hardware->diagnostic->memory.paddr,
		DIAGNOSTIC_BYTES);
	if (error != 0) {
		bcm2711_buffer_release(hardware->diagnostic);
		hardware->diagnostic = NULL;
		return error;
	}

	/* Publishes the mapping to the engine; a failure keeps the run and its interval. */
	hardware->diagnostic_address = DIAGNOSTIC_ADDRESS;
	error = bcm2711_v3d_hardware_pages_sync(
		engine,
		hardware->diagnostic_address,
		DIAGNOSTIC_BYTES);
	if (error != 0)
		return error;

	/* Succeeded: every diagnostic span is translated. */
	return 0;
}

/* Removes the diagnostic PTEs before returning the run to the allocator. */
static int
retire_storage(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_hardware *hardware;
	int error;

	/* The caller calls only after completion or a proven reset. */
	hardware = &engine->hardware;
	error = bcm2711_v3d_pages_unmap(
		hardware->pages->address,
		hardware->diagnostic_address,
		DIAGNOSTIC_BYTES);
	if (error != 0)
		return error;

	/* A cached older PTE still owns the run until both translation flushes finish. */
	error = bcm2711_v3d_hardware_pages_sync(
		engine,
		hardware->diagnostic_address,
		DIAGNOSTIC_BYTES);
	if (error != 0)
		return error;
	bcm2711_buffer_release(hardware->diagnostic);
	hardware->diagnostic = NULL;
	hardware->diagnostic_address = 0;

	/* Succeeded: nothing can address the old diagnostic run. */
	return 0;
}

/* Lends the diagnostic run's three command-list areas and its pool to a job's lists. */
static void
lend_lists(
	struct bcm2711_v3d *engine,
	struct bcm2711_v3d_noop *lists)
{
	uint8_t *storage;

	/* Each list gets its own page of the run, and the pool its interval. */
	storage = engine->hardware.diagnostic->address;
	kern_memset(lists, 0, sizeof(*lists));
	lists->bin.bytes = storage;
	lists->bin.capacity = 4096;
	lists->bin.address = DIAGNOSTIC_ADDRESS;
	lists->render.bytes = storage + DIAGNOSTIC_RENDER_OFFSET;
	lists->render.capacity = 4096;
	lists->render.address = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_RENDER_OFFSET;
	lists->tile.bytes = storage + DIAGNOSTIC_TILE_OFFSET;
	lists->tile.capacity = 4096;
	lists->tile.address = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_TILE_OFFSET;
	lists->pool_address = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_POOL_OFFSET;
	lists->pool_bytes = BCM2711_V3D_NOOP_POOL_BYTES;
}

/* Describes the bin/render job of prepared lists, with the tile state and one overflow pool. */
static void
cl_job(
	const struct bcm2711_v3d_noop *lists,
	struct bcm2711_v3d_job *job)
{
	/* Fills the job from the lists' addresses and lengths. */
	kern_memset(job, 0, sizeof(*job));
	job->kind = BCM2711_V3D_JOB_CL;
	job->command.cl.bin_start = lists->bin.address;
	job->command.cl.bin_end = lists->bin.address + lists->bin.used;
	job->command.cl.render_start = lists->render.address;
	job->command.cl.render_end = lists->render.address + lists->render.used;
	job->command.cl.pool_address = lists->pool_address;
	job->command.cl.pool_bytes = lists->pool_bytes;
	job->command.cl.state_address = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_STATE_OFFSET;
	job->command.cl.state_bytes = 256;
	job->command.cl.overflow_count = 1;
	job->command.cl.overflow[0].address = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_OVERFLOW_OFFSET;
	job->command.cl.overflow[0].bytes = 0x40000;
}

/* Packs a row- and column-distinct RGBA8 pattern without floating point. */
static uint32_t
pattern_pixel(
	uint32_t x,
	uint32_t y)
{
	uint32_t pixel;

	/* Independent channel ramps expose axis swaps and an unchanged raster layout. */
	pixel = 0xff000000U;
	pixel |= x * 29U + 7U;
	pixel |= (y * 31U + 5U) << 8;
	pixel |= ((x + y) * 13U + 11U) << 16;

	/* Succeeded: every channel stays in its byte for eight rows. */
	return pixel;
}

/* Writes the self-test's summary line: each step's result, "ok" or its error. */
static void
summary(
	const int *results)
{
	char line[BCM2711_STAGE_LINE_COLUMNS + 1U];
	size_t used;
	unsigned step;
	int written;

	/* Starts the line with the identification and the MMU, which passed to get here. */
	written = kern_snprintf(line, sizeof(line), "self-test ident ok mmu ok irq ok");
	if (written < 0)
		return;
	used = (size_t)written;

	/* Adds every step's result. */
	for (step = 0; step < STEP_COUNT && used < sizeof(line); step++) {
		/* Writes "ok", "-" for a step that did not run, or the error number. */
		if (results[step] == 0) {
			written = kern_snprintf(line + used, sizeof(line) - used, " %s ok", diagnostic_step_names[step]);
		} else if (results[step] == ECANCELED) {
			written = kern_snprintf(line + used, sizeof(line) - used, " %s -", diagnostic_step_names[step]);
		} else {
			written = kern_snprintf(line + used, sizeof(line) - used, " %s err%d", diagnostic_step_names[step], results[step]);
		}
		if (written < 0)
			return;
		used += (size_t)written;
	}

	/* Publishes the line. */
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "%s", line);
}
