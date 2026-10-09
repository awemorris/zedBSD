/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host checks of N1's display-list relocation preparation.
 *
 * These checks exercise exact raw-word preservation, occupied current and
 * pending lists, filter reservations and SRAM exhaustion.  Ordinary arrays
 * replace an already captured snapshot; no hardware register is accessed.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/* The stable SRAM image, rebuilt for each independent check in this process. */
static uint32_t snapshot[BCM2711_LIST_WORDS];

/* The source image retained to detect any change to live firmware words. */
static uint32_t original[BCM2711_LIST_WORDS];

/* The caller-owned relocation image, with canaries beyond its usable words. */
static uint32_t image[BCM2711_LIST_WORDS];

/* The number of violated expectations, accumulated until process exit. */
static unsigned failures;

static void check(bool condition, const char *what);
static void fill_source(uint32_t start, unsigned planes);

/*
 * Checks raw copying and placement without touching the captured firmware SRAM.
 */
int
main(
	void)
{
	struct bcm2711_list_range reserved[4];
	struct bcm2711_list_copy copy;
	bool prepared;
	int changed;
	uint32_t index;

	/* Protects unsorted current/next lists and overlapping filter intervals. */
	fill_source(40, 1);
	reserved[0].first = 80;
	reserved[0].words = 9;
	reserved[1].first = 0;
	reserved[1].words = 32;
	reserved[2].first = 49;
	reserved[2].words = 11;
	reserved[3].first = 54;
	reserved[3].words = 18;
	prepared = bcm2711_list_copy_prepare(
		snapshot,
		40,
		reserved,
		4,
		image,
		9,
		&copy);
	check(prepared, "an independent nine-word gap exists");
	check(copy.destination == 89, "current, pending and filter runs are skipped");
	check(copy.source == 40, "source position is retained");
	check(copy.words == 9, "the end marker belongs to the copied run");

	/* Checks the entire source SRAM and every opaque word, including the end. */
	changed = memcmp(snapshot, original, sizeof(snapshot));
	check(changed == 0, "preparation leaves firmware SRAM unchanged");
	changed = memcmp(image, original + 40, 9 * sizeof(uint32_t));
	check(changed == 0, "context and end-marker bits are preserved exactly");
	check(image[9] == 0xa5a5a5a5U, "the image's capacity boundary is untouched");

	/* Copies more planes than the readout's eight-plane summary can retain. */
	fill_source(160, BCM2711_LIST_PLANES + 1U);
	snapshot[160] &= ~0x00008000U;
	snapshot[160] += 1U << 24;

	/* Supplies the ninth word of a scaled first plane without rebuilding it. */
	for (index = 232; index >= 168; index--)
		snapshot[index + 1] = snapshot[index];

	/* Retains the additional opaque scaling word in the captured source. */
	snapshot[168] = 0xabcdef01U;
	memcpy(original, snapshot, sizeof(snapshot));
	reserved[0].first = 0;
	reserved[0].words = 64;
	prepared = bcm2711_list_copy_prepare(
		snapshot,
		160,
		reserved,
		1,
		image,
		BCM2711_LIST_WORDS,
		&copy);
	check(prepared, "a scaled list with nine planes can be copied");
	check(copy.destination == 64, "a gap ending before source is usable");
	check(copy.words == 74, "planes outside the readout summary are retained");
	changed = memcmp(image, original + 160, 74 * sizeof(uint32_t));
	check(changed == 0, "opaque scaling and later planes remain identical");

	/* Refuses relocation when reservations and the live source fill SRAM. */
	fill_source(40, 1);
	reserved[0].first = 0;
	reserved[0].words = 40;
	reserved[1].first = 49;
	reserved[1].words = BCM2711_LIST_WORDS - 49U;
	prepared = bcm2711_list_copy_prepare(
		snapshot,
		40,
		reserved,
		2,
		image,
		BCM2711_LIST_WORDS,
		&copy);
	check(!prepared, "source cannot serve as its own free destination");
	check(copy.words == 0, "exhaustion leaves no publishable relocation");
	check(image[0] == 0xa5a5a5a5U, "exhaustion leaves the image unchanged");

	/* Fits the end marker exactly in the final word of SRAM. */
	reserved[0].words = BCM2711_LIST_WORDS - 9U;
	prepared = bcm2711_list_copy_prepare(
		snapshot,
		40,
		reserved,
		1,
		image,
		9,
		&copy);
	check(prepared, "a gap at SRAM's upper boundary fits exactly");
	check(
		copy.destination == BCM2711_LIST_WORDS - 9U,
		"the last complete run is selected");

	/* Refuses a destination image that omits the closing word. */
	fill_source(40, 1);
	prepared = bcm2711_list_copy_prepare(
		snapshot,
		40,
		NULL,
		0,
		image,
		8,
		&copy);
	check(!prepared, "capacity must include the end marker");
	check(copy.words == 0, "short capacity leaves no publishable relocation");
	check(image[0] == 0xa5a5a5a5U, "short capacity leaves the image unchanged");

	/* Rejects an overflowing reservation before it can create a false gap. */
	reserved[0].first = 10;
	reserved[0].words = 0xffffffffU;
	prepared = bcm2711_list_copy_prepare(
		snapshot,
		40,
		reserved,
		1,
		image,
		BCM2711_LIST_WORDS,
		&copy);
	check(!prepared, "an overflowing occupied interval is refused");
	check(copy.words == 0, "a malformed reservation invalidates the old plan");
	check(image[0] == 0xa5a5a5a5U, "bad reservations leave the image unchanged");

	/* Rejects reservations with no SRAM word or a start outside SRAM. */
	reserved[0].words = 0;
	prepared = bcm2711_list_copy_prepare(
		snapshot,
		40,
		reserved,
		1,
		image,
		BCM2711_LIST_WORDS,
		&copy);
	check(!prepared, "empty reservations are refused");
	reserved[0].first = BCM2711_LIST_WORDS;
	reserved[0].words = 1;
	prepared = bcm2711_list_copy_prepare(
		snapshot,
		40,
		reserved,
		1,
		image,
		BCM2711_LIST_WORDS,
		&copy);
	check(!prepared, "out-of-SRAM reservations are refused");

	/* Refuses incomplete and out-of-SRAM source lists before copying anything. */
	snapshot[48] = 0;
	prepared = bcm2711_list_copy_prepare(
		snapshot,
		40,
		NULL,
		0,
		image,
		BCM2711_LIST_WORDS,
		&copy);
	check(!prepared, "a source without an end marker is refused");
	check(image[0] == 0xa5a5a5a5U, "a malformed source leaves the image unchanged");
	prepared = bcm2711_list_copy_prepare(
		snapshot,
		BCM2711_LIST_WORDS,
		NULL,
		0,
		image,
		BCM2711_LIST_WORDS,
		&copy);
	check(!prepared, "an out-of-SRAM source is refused");

	/* Preserves an empty list that still carries a closing word. */
	fill_source(0, 0);
	prepared = bcm2711_list_copy_prepare(
		snapshot,
		0,
		NULL,
		0,
		image,
		1,
		&copy);
	check(prepared, "an end-only list has a separate destination");
	check(copy.destination == 1, "the live end-only source is protected");
	check(copy.words == 1, "an empty list still occupies one word");
	check(image[0] == 0x80000123U, "the complete closing word is copied");

	/* Reports the failed expectations without masking a partial failure. */
	if (failures != 0) {
		printf("list-copy-host-test: FAIL (%u)\n", failures);
		return 1;
	}

	/* Succeeded: placement and unchanged images satisfy every checked case. */
	printf("list-copy-host-test: PASS\n");
	return 0;
}

/* Counts a failed expectation while allowing independent cases to continue. */
static void
check(
	bool condition,
	const char *what)
{
	/* Keeps successful expectations quiet so failures remain visible. */
	if (condition)
		return;

	/* Accumulates the verdict consumed by main at process exit. */
	printf("FAIL: %s\n", what);
	failures++;
}

/* Builds raw unscaled planes with distinct opaque fields for copy checks. */
static void
fill_source(
	uint32_t start,
	unsigned planes)
{
	uint32_t index;
	unsigned plane;
	unsigned word;

	/* Resets both the captured SRAM and the image's out-of-capacity canaries. */
	memset(snapshot, 0, sizeof(snapshot));
	memset(image, 0xa5, sizeof(image));

	/* Gives each plane eight raw words, including two hardware context words. */
	index = start;
	for (plane = 0; plane < planes; plane++) {
		/* Fills each opaque payload distinctly to expose any reconstruction. */
		for (word = 1; word < 8; word++)
			snapshot[index + word] = 0x10203000U + index + word;

		/* Marks the run as an eight-word unscaled plane. */
		snapshot[index] = 0x4800d807U;
		index += 8;
	}

	/* Keeps non-marker bits in the end word to check literal preservation. */
	snapshot[index] = 0x80000123U;
	memcpy(original, snapshot, sizeof(snapshot));
}
