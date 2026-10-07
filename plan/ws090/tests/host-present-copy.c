/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BUG-221: the host test of a frame copied into the presenter's canvas by
 * its changed part (userland/desktop/libkeiland/ui/present-copy.c).  For
 * many random frames, each a change of the last inside a random part, the
 * canvas copied by the part alone is the canvas copied whole, byte for
 * byte (the canvas rows are wider than the frame, as a linear image's
 * pitch is); and the part's clipping to the frame.
 *
 *     host-present-copy        prints "host-present-copy: N checks, F failures"
 */

#include "userland/desktop/libkeiland/ui/present-copy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_WIDTH	301U
#define TEST_HEIGHT	197U
#define TEST_STRIDE	320U
#define TEST_PITCH	(1280U + 64U)
#define TEST_FRAMES	2000U

static int checks;
static int failures;

int main(void);
static void check(int condition, const char *what);
static uint32_t random_word(void);

int
main(void)
{
	static uint32_t frame[TEST_STRIDE * TEST_HEIGHT];
	static unsigned char partial[TEST_PITCH * TEST_HEIGHT];
	static unsigned char whole[TEST_PITCH * TEST_HEIGHT];
	struct kl_rect all;
	struct kl_rect part;
	struct kl_rect area;
	unsigned step;
	int differs;
	int inside;
	int x;
	int y;

	/* The first frame, copied whole into both canvases (their padding apart). */
	srand(221);
	for (x = 0; x < (int)(TEST_STRIDE * TEST_HEIGHT); x++)
		frame[x] = random_word();
	memset(partial, 0xA5, sizeof(partial));
	memset(whole, 0xA5, sizeof(whole));
	all.x = 0;
	all.y = 0;
	all.width = (int)TEST_WIDTH;
	all.height = (int)TEST_HEIGHT;
	keiui_present_copy(partial, TEST_PITCH, frame, TEST_STRIDE, &all);
	keiui_present_copy(whole, TEST_PITCH, frame, TEST_STRIDE, &all);

	/* Each frame changes a random part (partly off the frame at times); copied by the part, and whole. */
	differs = 0;
	for (step = 0U; step < TEST_FRAMES; step++) {
		part.x = (int)(random_word() % (TEST_WIDTH + 40U)) - 20;
		part.y = (int)(random_word() % (TEST_HEIGHT + 40U)) - 20;
		part.width = (int)(random_word() % 120U);
		part.height = (int)(random_word() % 90U);
		inside = keiui_present_part_clip(TEST_WIDTH, TEST_HEIGHT, &part, &area);
		if (!inside)
			area = all;

		/* The change: only inside the part. */
		for (y = area.y; y < area.y + area.height; y++) {
			for (x = area.x; x < area.x + area.width; x++)
				frame[(size_t)y * TEST_STRIDE + (size_t)x] = random_word();
		}

		/* By the part, and whole: the same canvas. */
		keiui_present_copy(partial, TEST_PITCH, frame, TEST_STRIDE, &area);
		keiui_present_copy(whole, TEST_PITCH, frame, TEST_STRIDE, &all);
		if (memcmp(partial, whole, sizeof(partial)) != 0)
			differs++;
	}
	check(differs == 0, "every frame copied by its part is the frame copied whole");

	/* The clipping. */
	part.x = -10;
	part.y = -5;
	part.width = 30;
	part.height = 20;
	inside = keiui_present_part_clip(TEST_WIDTH, TEST_HEIGHT, &part, &area);
	check(inside && area.x == 0 && area.y == 0 && area.width == 20 && area.height == 15, "a part over the top left corner is clipped");
	part.x = (int)TEST_WIDTH - 5;
	part.y = (int)TEST_HEIGHT - 5;
	inside = keiui_present_part_clip(TEST_WIDTH, TEST_HEIGHT, &part, &area);
	check(inside && area.width == 5 && area.height == 5, "a part over the bottom right corner is clipped");
	part.x = (int)TEST_WIDTH;
	inside = keiui_present_part_clip(TEST_WIDTH, TEST_HEIGHT, &part, &area);
	check(!inside, "a part right of the frame is nothing");
	part.x = 10;
	part.width = 0;
	inside = keiui_present_part_clip(TEST_WIDTH, TEST_HEIGHT, &part, &area);
	check(!inside, "an empty part is nothing");

	/* The verdict. */
	printf("host-present-copy: %d checks, %d failures\n", checks, failures);
	if (failures != 0)
		return 1;
	return 0;
}

/* Counts a check and reports a failed one. */
static void
check(
	int condition,
	const char *what)
{
	checks++;
	if (condition)
		return;
	failures++;
	printf("FAIL: %s\n", what);
}

/* A random word of 32 bits. */
static uint32_t
random_word(void)
{
	uint32_t high;
	uint32_t low;

	high = (uint32_t)rand() & 0xffffU;
	low = (uint32_t)rand() & 0xffffU;
	return (high << 16) | low;
}
