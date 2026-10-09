/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Boot diagnostics execute before a render device or any other job owner exists. */
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/buffer.h"
#include "drivers/gpu/bcm2711/v3d-job.h"

/* One mapped diagnostic run keeps CLs, tile state, pool and overflow together. */
#define DIAGNOSTIC_ADDRESS 0x100000U
#define DIAGNOSTIC_BYTES 0x200000U
#define DIAGNOSTIC_POOL_OFFSET 0x4000U
#define DIAGNOSTIC_OVERFLOW_OFFSET 0x90000U
#define DIAGNOSTIC_TARGET_OFFSET 0x100000U
#define DIAGNOSTIC_COLOR 0xff317ce0U
#define DIAGNOSTIC_TFU_OFFSET 0x140000U

static int prepare_storage(struct bcm2711_v3d *engine);
static int retire_storage(struct bcm2711_v3d *engine);
static int run_noop(struct bcm2711_v3d *engine);
static int run_clear(struct bcm2711_v3d *engine);
static int run_tfu(struct bcm2711_v3d *engine);
static int run_timeout(struct bcm2711_v3d *engine);
static uint32_t pattern_pixel(uint32_t x, uint32_t y);

/*
 * Executes the real shader-free bin/render diagnostic before render registration.
 * A failure leaves uncertain storage in the persistent engine; reset is allowed
 * here because no other clients, resource owners or submitted jobs exist yet.
 */
int
bcm2711_v3d_diagnostic(
	struct bcm2711_v3d *engine)
{
	bool allowed;
	int error;
	int reset_error;
	int retired;

	/* Honors the ordinary native stop before any diagnostic memory publication. */
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V7");
	if (!allowed)
		return ECANCELED;
	if (!engine->hardware.ready || engine->hardware.diagnostic != NULL)
		return EBUSY;
	error = prepare_storage(engine);
	if (error != 0)
		return error;

	/* A printed launch is not a pass: the job waits for the actual serviced IRQs. */
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V7 noop begin");
	bcm2711_stage_pause(BCM2711_FAMILY_V3D, "V7");
	error = run_noop(engine);
	if (error == 0) {
		/* The ordinary stop keeps a successful V7 from implicitly authorizing later stages. */
		allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V8");
		if (allowed)
			error = run_clear(engine);
		else
			error = ECANCELED;
	}

	/* TFU remains independent of display and runs only after the clear's output was verified. */
	if (error == 0) {
		allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V9");
		if (allowed)
			error = run_tfu(engine);
		else
			error = ECANCELED;
	}

	/* Intentional timeout occurs before client registration, with one retained boot owner. */
	if (error == 0) {
		allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V10");
		if (allowed)
			error = run_timeout(engine);
		else
			error = ECANCELED;
	}

	/* A normal stop has no failed native execution requiring recovery. */
	if (error != 0 && error != ECANCELED) {
		/* A failed provider reset already supplied no accessible engine or retirement proof. */
		if (!engine->power.ready) {
			bcm2711_stage_mark(BCM2711_FAMILY_V3D, "native reset failed; diagnostic retained");
			return error;
		}

		/* Before publication to clients, boot owns the entire execution domain. */
		reset_error = bcm2711_v3d_hardware_reset(engine);
		if (reset_error != 0) {
			bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V7 error %d reset %d retained", error, reset_error);
			return error;
		}
	}

	/* Even completed jobs keep their physical owner until PTE removal is flushed. */
	retired = retire_storage(engine);
	if (retired != 0)
		return retired;
	if (error != 0)
		return error;

	/* Succeeded: actual bin/render IRQ completion and safe storage retirement occurred. */
	return 0;
}

/* Converts a distinctive raster pattern to one UIF block and checks every tiled pixel. */
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

	/* Eight raster rows and four 4x4 utiles make a visible difference in memory layout. */
	storage = engine->hardware.diagnostic->address;
	input = (uint32_t *)(storage + DIAGNOSTIC_TARGET_OFFSET);
	output = (uint32_t *)(storage + DIAGNOSTIC_TFU_OFFSET);
	for (y = 0; y < 8; y++) {
		/* Each source pixel carries separate column and row information. */
		for (x = 0; x < 8; x++)
			input[y * 8U + x] = pattern_pixel(x, y);
	}

	/* The output sentinel cannot be mistaken for successful texture conversion. */
	kern_memset(output, 0xa5, 256);
	kern_memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_TFU;
	job.command.tfu.input = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_TARGET_OFFSET;
	job.command.tfu.input_bytes = 256;
	job.command.tfu.output = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_TFU_OFFSET;
	job.command.tfu.output_bytes = 256;
	job.command.tfu.output_format = 4;
	job.command.tfu.stride = 8;
	job.command.tfu.size = 0x00080008;
	job.command.tfu.configuration = 4U << 9;
	kern_dcache_clean_range(input, 256);
	kern_dcache_clean_range(output, 256);
	kern_io_write_barrier();
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V9 TFU raster to UB-linear begin");
	bcm2711_stage_pause(BCM2711_FAMILY_V3D, "V9");
	error = bcm2711_v3d_job_run(engine, &job, &result);
	if (error != 0)
		return error;

	/* A 32-bpp UIF block holds four raster 4x4 utiles in row-major order. */
	kern_dcache_invalidate_range(output, 256);
	kern_io_read_barrier();
	for (y = 0; y < 8; y++) {
		/* Inspects every column using the tiled byte layout, not the source raster order. */
		for (x = 0; x < 8; x++) {
			offset = ((y >> 2) * 2U + (x >> 2)) * 16U;
			offset += (y & 3U) * 4U + (x & 3U);
			expected = pattern_pixel(x, y);
			if (output[offset] != expected) {
				bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V9 mismatch %u,%u %08x", x, y, output[offset]);
				return EIO;
			}
		}
	}

	/* Hub completion and exact output layout both contribute to the physical diagnostic. */
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V9 TFU IRQ %x all 64 tiled pixels", result.hub_events);

	/* Succeeded: CPU visibility and raster-to-tiled transfer agree for every pattern pixel. */
	return 0;
}

/* Hangs a private bin list, proves native reset, then reruns the exact noop diagnostic. */
static int
run_timeout(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_job job;
	struct bcm2711_v3d_job_result result;
	uint8_t *storage;
	uint32_t index;
	int error;

	/* A branch to its own packet has no termination or semaphore dependency. */
	storage = engine->hardware.diagnostic->address;
	storage[0] = 16;
	for (index = 0; index < 4; index++)
		storage[index + 1] = (uint8_t)(DIAGNOSTIC_ADDRESS >> (index * 8U));
	kern_memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_CL;
	job.command.cl.bin_start = DIAGNOSTIC_ADDRESS;
	job.command.cl.bin_end = DIAGNOSTIC_ADDRESS + 5;
	job.command.cl.render_start = DIAGNOSTIC_ADDRESS + 0x1000;
	job.command.cl.render_end = DIAGNOSTIC_ADDRESS + 0x106a;
	job.command.cl.pool_address = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_POOL_OFFSET;
	job.command.cl.pool_bytes = BCM2711_V3D_NOOP_POOL_BYTES;
	job.command.cl.state_address = DIAGNOSTIC_ADDRESS + 0x3000;
	job.command.cl.state_bytes = 256;
	kern_dcache_clean_range(storage, 4096);
	kern_io_write_barrier();
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V10 self-branch timeout/reset begin");
	bcm2711_stage_pause(BCM2711_FAMILY_V3D, "V10");
	error = bcm2711_v3d_job_run(engine, &job, &result);
	if (error != ETIMEDOUT || result.retired) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V10 unexpected result %d retired %u", error, (unsigned)result.retired);
		return EIO;
	}

	/* Boot owns all DMA here; successful reset also verifies the original identifiers. */
	error = bcm2711_v3d_hardware_reset(engine);
	if (error != 0)
		return error;
	error = run_noop(engine);
	if (error != 0)
		return error;

	/* Actual noop IRQ completion proves fresh execution after the intentional hang. */
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"V10 reset %llu post-reset noop complete",
		(unsigned long long)engine->hardware.resets);

	/* Succeeded: timeout, verified global reset and a fresh native job all occurred. */
	return 0;
}

/* Packs a row/column-distinct RGBA8 pattern without floating-point conversion. */
static uint32_t
pattern_pixel(
	uint32_t x,
	uint32_t y)
{
	uint32_t pixel;

	/* Independent channel ramps expose axis swaps and accidentally unchanged raster layout. */
	pixel = 0xff000000U;
	pixel |= x * 29U + 7U;
	pixel |= (y * 31U + 5U) << 8;
	pixel |= ((x + y) * 13U + 11U) << 16;

	/* Succeeded: all channel values remain representable for the diagnostic's eight rows. */
	return pixel;
}

/* Clears one raster tile and validates the actual GPU-written bytes after CPU invalidation. */
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

	/* A sentinel target makes a missing GPU store distinguishable from the requested color. */
	storage = engine->hardware.diagnostic->address;
	target = (uint32_t *)(storage + DIAGNOSTIC_TARGET_OFFSET);
	kern_memset(storage + DIAGNOSTIC_POOL_OFFSET, 0, BCM2711_V3D_NOOP_POOL_BYTES);
	kern_memset(storage + 0x3000, 0, 4096);
	kern_memset(target, 0xa5, 16384);
	kern_memset(&clear, 0, sizeof(clear));
	clear.lists.bin.bytes = storage;
	clear.lists.bin.capacity = 4096;
	clear.lists.bin.address = DIAGNOSTIC_ADDRESS;
	clear.lists.render.bytes = storage + 0x1000;
	clear.lists.render.capacity = 4096;
	clear.lists.render.address = DIAGNOSTIC_ADDRESS + 0x1000;
	clear.lists.tile.bytes = storage + 0x2000;
	clear.lists.tile.capacity = 4096;
	clear.lists.tile.address = DIAGNOSTIC_ADDRESS + 0x2000;
	clear.lists.pool_address = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_POOL_OFFSET;
	clear.lists.pool_bytes = BCM2711_V3D_NOOP_POOL_BYTES;
	clear.width = 64;
	clear.height = 64;
	clear.stride = 256;
	clear.target_address = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_TARGET_OFFSET;
	clear.target_bytes = 16384;
	clear.color = DIAGNOSTIC_COLOR;
	error = bcm2711_v3d_clear_prepare(&clear);
	if (error != 0)
		return error;

	/* Every generated list and destination sentinel reaches RAM before native execution. */
	kern_memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_CL;
	job.command.cl.bin_start = clear.lists.bin.address;
	job.command.cl.bin_end = clear.lists.bin.address + clear.lists.bin.used;
	job.command.cl.render_start = clear.lists.render.address;
	job.command.cl.render_end = clear.lists.render.address + clear.lists.render.used;
	job.command.cl.pool_address = clear.lists.pool_address;
	job.command.cl.pool_bytes = clear.lists.pool_bytes;
	job.command.cl.state_address = DIAGNOSTIC_ADDRESS + 0x3000;
	job.command.cl.state_bytes = 256;
	job.command.cl.overflow_count = 1;
	job.command.cl.overflow[0].address = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_OVERFLOW_OFFSET;
	job.command.cl.overflow[0].bytes = 0x40000;
	kern_dcache_clean_range(storage, DIAGNOSTIC_BYTES);
	kern_io_write_barrier();
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V8 clear/store begin");
	bcm2711_stage_pause(BCM2711_FAMILY_V3D, "V8");
	error = bcm2711_v3d_job_run(engine, &job, &result);
	if (error != 0)
		return error;

	/* TLB stores are drained by render completion; CPU cache still needs invalidation. */
	kern_dcache_invalidate_range(target, 16384);
	kern_io_read_barrier();
	for (index = 0; index < 4096; index++) {
		pixel = target[index];
		if (pixel != DIAGNOSTIC_COLOR) {
			bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V8 mismatch pixel %u %08x", index, pixel);
			return EIO;
		}
	}

	/* The physical runtime prints pass only after all GPU output pixels were inspected. */
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V8 RGBA8 64x64 all 4096 pixels %08x", DIAGNOSTIC_COLOR);

	/* Succeeded: actual raster store, IRQ retirement and CPU visibility agree. */
	return 0;
}

/* Allocates one contiguous run and installs its entire diagnostic VA reservation. */
static int
prepare_storage(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_hardware *hardware;
	int error;

	/* The permanent pointer records the owner before any MMU mapping can expose its RAM. */
	hardware = &engine->hardware;
	error = bcm2711_buffer_create(
		DIAGNOSTIC_BYTES,
		0x3fffffff,
		BCM2711_V3D_PAGE_BYTES,
		&hardware->diagnostic);
	if (error != 0)
		return error;

	/* Software mapping refusal changes no PTE and permits immediate, unexposed unwind. */
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

	/* Failed translation publication retains the descriptor and occupied VA interval. */
	hardware->diagnostic_address = DIAGNOSTIC_ADDRESS;
	error = bcm2711_v3d_hardware_pages_sync(engine);
	if (error != 0)
		return error;

	/* Succeeded: all diagnostic spans are translated and retained by one permanent owner. */
	return 0;
}

/* Removes completed diagnostic PTEs before returning their physical run to allocation. */
static int
retire_storage(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_hardware *hardware;
	int error;

	/* The sole boot worker calls only after completion or a proven global reset. */
	hardware = &engine->hardware;
	error = bcm2711_v3d_pages_unmap(
		hardware->pages->address,
		hardware->diagnostic_address,
		DIAGNOSTIC_BYTES);
	if (error != 0)
		return error;

	/* A cached older PTE still owns the allocation until both translation flushes finish. */
	error = bcm2711_v3d_hardware_pages_sync(engine);
	if (error != 0)
		return error;
	bcm2711_buffer_release(hardware->diagnostic);
	hardware->diagnostic = NULL;
	hardware->diagnostic_address = 0;

	/* Succeeded: no active job or cached translation can address the old diagnostic run. */
	return 0;
}

/* Builds and executes the independently checked one-pixel command lists. */
static int
run_noop(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_noop lists;
	struct bcm2711_v3d_job job;
	struct bcm2711_v3d_job_result result;
	uint8_t *storage;
	int error;

	/* All generated commands borrow subranges of the one mapped diagnostic allocation. */
	storage = engine->hardware.diagnostic->address;
	kern_memset(storage + DIAGNOSTIC_POOL_OFFSET, 0, BCM2711_V3D_NOOP_POOL_BYTES);
	kern_memset(storage + 0x3000, 0, 4096);
	kern_memset(&lists, 0, sizeof(lists));
	lists.bin.bytes = storage;
	lists.bin.capacity = 4096;
	lists.bin.address = DIAGNOSTIC_ADDRESS;
	lists.render.bytes = storage + 0x1000;
	lists.render.capacity = 4096;
	lists.render.address = DIAGNOSTIC_ADDRESS + 0x1000;
	lists.tile.bytes = storage + 0x2000;
	lists.tile.capacity = 4096;
	lists.tile.address = DIAGNOSTIC_ADDRESS + 0x2000;
	lists.pool_address = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_POOL_OFFSET;
	lists.pool_bytes = BCM2711_V3D_NOOP_POOL_BYTES;
	error = bcm2711_v3d_noop_prepare(&lists);
	if (error != 0)
		return error;

	/* PTB overflow is preallocated and mapped before the first bin launch. */
	kern_memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_CL;
	job.command.cl.bin_start = lists.bin.address;
	job.command.cl.bin_end = lists.bin.address + lists.bin.used;
	job.command.cl.render_start = lists.render.address;
	job.command.cl.render_end = lists.render.address + lists.render.used;
	job.command.cl.pool_address = lists.pool_address;
	job.command.cl.pool_bytes = lists.pool_bytes;
	job.command.cl.state_address = DIAGNOSTIC_ADDRESS + 0x3000;
	job.command.cl.state_bytes = 256;
	job.command.cl.overflow_count = 1;
	job.command.cl.overflow[0].address = DIAGNOSTIC_ADDRESS + DIAGNOSTIC_OVERFLOW_OFFSET;
	job.command.cl.overflow[0].bytes = 0x40000;

	/* Input and pool zeroes reach RAM before the MMU-backed native queue can fetch them. */
	kern_dcache_clean_range(storage, DIAGNOSTIC_BYTES);
	kern_io_write_barrier();
	error = bcm2711_v3d_job_run(engine, &job, &result);
	if (error != 0)
		return error;

	/* Only actual IRQ observations justify the V7 result printed on physical hardware. */
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"V7 noop IRQ %x/%x BFC %u RFC %u overflow %u",
		result.core_events,
		result.hub_events,
		result.bin_counter,
		result.render_counter,
		result.overflow_used);

	/* Succeeded: the caller can proceed to visible render with these same retained owners. */
	return 0;
}
