/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Clear packet output is checked independently by the pinned 4.2 XML oracle. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/* Caller-owned command storage has canary tails outside every published used length. */
static uint8_t test_bin[64];
static uint8_t test_render[128];
static uint8_t test_tile[64];
static uint8_t test_saved[3][128];

static void print_image(const char *name, const struct bcm2711_v3d_cl *list);
static void refused(struct bcm2711_v3d_clear *job, int expected);

/*
 * Checks complete shader-free clear/store images and atomic refusal boundaries.
 */
int
main(
	void)
{
	struct bcm2711_v3d_clear job;
	struct bcm2711_v3d_clear saved;
	int error;

	/* Each command reservation and the target occupy distinct GPU storage. */
	memset(&job, 0, sizeof(job));
	memset(test_bin, 0xa5, sizeof(test_bin));
	memset(test_render, 0xa5, sizeof(test_render));
	memset(test_tile, 0xa5, sizeof(test_tile));
	job.lists.bin.bytes = test_bin;
	job.lists.bin.capacity = sizeof(test_bin);
	job.lists.bin.address = 0x10000;
	job.lists.render.bytes = test_render;
	job.lists.render.capacity = sizeof(test_render);
	job.lists.render.address = 0x20000;
	job.lists.tile.bytes = test_tile;
	job.lists.tile.capacity = sizeof(test_tile);
	job.lists.tile.address = 0x30000;
	job.lists.pool_address = 0x40000;
	job.lists.pool_bytes = 0x83000;
	job.width = 64;
	job.height = 64;
	job.stride = 256;
	job.target_address = 0x100000;
	job.target_bytes = 16384;
	job.color = 0xff317ce0;
	error = bcm2711_v3d_clear_prepare(&job);
	assert(error == 0);
	assert(job.lists.bin.used == 14 && job.lists.render.used == 106 && job.lists.tile.used == 19);
	assert(test_bin[14] == 0xa5 && test_render[106] == 0xa5 && test_tile[19] == 0xa5);
	print_image("bin", &job.lists.bin);
	print_image("render", &job.lists.render);
	print_image("tile", &job.lists.tile);

	/* Retains the full prior reservations, not merely their published packet prefixes. */
	memcpy(test_saved[0], test_bin, sizeof(test_bin));
	memcpy(test_saved[1], test_render, sizeof(test_render));
	memcpy(test_saved[2], test_tile, sizeof(test_tile));
	saved = job;

	/* Short clear capacity, oversized geometry and raster aliasing all preserve every byte. */
	job.lists.render.capacity = 105;
	refused(&job, ENOSPC);
	job = saved;
	job.width = 65;
	refused(&job, EINVAL);
	job = saved;
	job.target_bytes = 16383;
	refused(&job, EINVAL);
	job = saved;
	job.target_address = job.lists.pool_address;
	refused(&job, EINVAL);
	job = saved;
	job.stride = 255;
	refused(&job, EINVAL);
	job = saved;
	job.target_address = 0xffffc004;
	refused(&job, EINVAL);

	/* Succeeded: the separate oracle can now verify every packet and relocated field. */
	puts("clear-host-test PASS");
	return 0;
}

/* Prints only completed images for the primary-XML oracle, without native execution claims. */
static void
print_image(
	const char *name,
	const struct bcm2711_v3d_cl *list)
{
	uint32_t index;

	/* A whole byte string preserves packet lengths and inter-packet ordering. */
	printf("clear-image %s ", name);
	for (index = 0; index < list->used; index++)
		printf("%02x", list->bytes[index]);
	putchar('\n');
}

/* Confirms failed generation changes neither complete reservations nor published lengths. */
static void
refused(
	struct bcm2711_v3d_clear *job,
	int expected)
{
	int error;
	int different;

	/* A refusal cannot leave a mixed old/new stream eligible for native submission. */
	error = bcm2711_v3d_clear_prepare(job);
	assert(error == expected);
	assert(job->lists.bin.used == 0 && job->lists.render.used == 0 && job->lists.tile.used == 0);
	different = memcmp(test_saved[0], test_bin, sizeof(test_bin));
	assert(different == 0);
	different = memcmp(test_saved[1], test_render, sizeof(test_render));
	assert(different == 0);
	different = memcmp(test_saved[2], test_tile, sizeof(test_tile));
	assert(different == 0);
}
