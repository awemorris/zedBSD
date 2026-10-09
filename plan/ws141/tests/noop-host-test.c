/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host checks of the initial V3D command images and refusal behavior.
 *
 * The successful byte images are printed for an independent XML-based
 * packet oracle.  Later refusal cases retain those images for comparison.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/* Three separate command buffers whose unused tails contain canaries. */
static uint8_t bin_bytes[64];
static uint8_t render_bytes[64];
static uint8_t tile_bytes[64];

/* The successful images retained through all subsequent refusal cases. */
static uint8_t original[3][64];

/* The number of failed expectations accumulated until process exit. */
static unsigned failures;

static void check(bool condition, const char *what);
static void check_refusal(struct bcm2711_v3d_noop *job, int expected, const char *what);
static void print_image(const char *name, const uint8_t *bytes, uint32_t used);

/*
 * Checks a complete noop image and refusals that preserve every command byte.
 */
int
main(
	void)
{
	struct bcm2711_v3d_noop job;
	struct bcm2711_v3d_noop saved;
	int error;

	/* Gives the three buffers distinct CPU and GPU storage with unused tails. */
	memset(bin_bytes, 0xa5, sizeof(bin_bytes));
	memset(render_bytes, 0xa5, sizeof(render_bytes));
	memset(tile_bytes, 0xa5, sizeof(tile_bytes));
	job.bin.bytes = bin_bytes;
	job.bin.capacity = sizeof(bin_bytes);
	job.bin.address = 0x1000;
	job.bin.used = 0;
	job.render.bytes = render_bytes;
	job.render.capacity = sizeof(render_bytes);
	job.render.address = 0x2000;
	job.render.used = 0;
	job.tile.bytes = tile_bytes;
	job.tile.capacity = sizeof(tile_bytes);
	job.tile.address = 0x3abcd013U;
	job.tile.used = 0;
	job.pool_address = 0x45678000U;
	job.pool_bytes = BCM2711_V3D_NOOP_POOL_BYTES;

	/* Builds a job whose indirect packet address is intentionally unaligned. */
	error = bcm2711_v3d_noop_prepare(&job);
	check(error == 0, "a complete unaligned-address noop job is accepted");
	check(job.bin.used == 14, "the bin stream ends after its flush");
	check(job.render.used == 56, "the render stream ends after rendering");
	check(job.tile.used == 19, "the tile stream includes its return");
	check(bin_bytes[14] == 0xa5, "bin capacity after its image is unchanged");
	check(render_bytes[56] == 0xa5, "render capacity after its image is unchanged");
	check(tile_bytes[19] == 0xa5, "tile capacity after its image is unchanged");

	/* Exposes complete literal byte images to the independent packet oracle. */
	print_image("bin", bin_bytes, job.bin.used);
	print_image("render", render_bytes, job.render.used);
	print_image("tile", tile_bytes, job.tile.used);

	/* Retains the successful buffers and lengths before testing refusals. */
	memcpy(original[0], bin_bytes, sizeof(bin_bytes));
	memcpy(original[1], render_bytes, sizeof(render_bytes));
	memcpy(original[2], tile_bytes, sizeof(tile_bytes));
	saved = job;

	/* A late capacity failure must preserve already valid bin/render images. */
	job.tile.capacity = BCM2711_V3D_NOOP_TILE_BYTES - 1U;
	check_refusal(&job, ENOSPC, "a short final buffer refuses the complete job");

	/* A sub-list end cannot wrap through address zero. */
	job = saved;
	job.tile.address = 0xfffffff0U;
	check_refusal(&job, EINVAL, "an indirect exclusive-end overflow is refused");

	/* Commands and tile pools both preserve the reserved zero VA page. */
	job = saved;
	job.bin.address = 1;
	check_refusal(&job, EINVAL, "a command in the zero page is refused");
	job = saved;
	job.pool_address = 0;
	check_refusal(&job, EINVAL, "a pool in the zero page is refused");

	/* A pool's low six address bits belong to the tile-list-set packet. */
	job = saved;
	job.pool_address++;
	check_refusal(&job, EINVAL, "an unaligned pool base is refused");

	/* Checks both overflow headroom and the pool's whole GPU reservation. */
	job = saved;
	job.pool_bytes--;
	check_refusal(&job, EINVAL, "an undersized tile pool is refused");
	job = saved;
	job.pool_address = 0xfffff000U;
	check_refusal(&job, EINVAL, "a pool crossing the VA limit is refused");

	/* Unused command capacity still belongs to its buffer's owner. */
	job = saved;
	job.render.address = 0x1010;
	check_refusal(&job, EINVAL, "overlap in unused command capacity is refused");

	/* GPU tile allocation must never overwrite a live command image. */
	job = saved;
	job.pool_address = 0x1000;
	check_refusal(&job, EINVAL, "a pool overlapping commands is refused");

	/* Missing CPU storage refuses the job before changing other images. */
	job = saved;
	job.render.bytes = NULL;
	check_refusal(&job, EINVAL, "a missing command buffer is refused");

	/* Adjacent reserved buffers may meet at their exclusive endpoints. */
	job = saved;
	job.render.address = job.bin.address + job.bin.capacity;
	error = bcm2711_v3d_noop_prepare(&job);
	check(error == 0, "adjacent GPU reservations remain disjoint");

	/* Reports any failed expectation to the test runner. */
	if (failures != 0) {
		printf("noop-host-test: FAIL (%u)\n", failures);
		return 1;
	}

	/* Succeeded: the oracle images and all checked refusal contracts are ready. */
	printf("noop-host-test: PASS\n");
	return 0;
}

/* Accumulates failed expectations for the process's final verdict. */
static void
check(
	bool condition,
	const char *what)
{
	/* Keeps successful checks quiet so failed expectations stand out. */
	if (condition)
		return;

	/* Records an expectation that main must report as a process failure. */
	printf("FAIL: %s\n", what);
	failures++;
}

/* Checks that a refused rebuild cannot publish or partially change an image. */
static void
check_refusal(
	struct bcm2711_v3d_noop *job,
	int expected,
	const char *what)
{
	int error;
	int changed;

	/* Rebuilds the supplied job through the default production generator. */
	error = bcm2711_v3d_noop_prepare(job);
	check(error == expected, what);
	check(job->bin.used == 0, "a refused rebuild unpublishes bin");
	check(job->render.used == 0, "a refused rebuild unpublishes render");
	check(job->tile.used == 0, "a refused rebuild unpublishes the tile list");

	/* Compares complete buffers, including their unused capacity. */
	changed = memcmp(bin_bytes, original[0], sizeof(bin_bytes));
	check(changed == 0, "refusal preserves bin bytes");
	changed = memcmp(render_bytes, original[1], sizeof(render_bytes));
	check(changed == 0, "refusal preserves render bytes");
	changed = memcmp(tile_bytes, original[2], sizeof(tile_bytes));
	check(changed == 0, "refusal preserves tile bytes");
}

/* Prints one generated stream for the independent XML field interpreter. */
static void
print_image(
	const char *name,
	const uint8_t *bytes,
	uint32_t used)
{
	uint32_t index;

	/* Emits a complete hex image without host byte-order reinterpretation. */
	printf("noop-image %s ", name);
	for (index = 0; index < used; index++)
		printf("%02x", bytes[index]);

	/* Closes this image record before the next test or verdict line. */
	printf("\n");
}
