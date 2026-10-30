/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the game mode's rules on a rotated output (ws217-p002a,
 * userland/desktop/wayland/scanout-rules.c): an unrotated output keeps the
 * rules' order (each reason in its turn, and straight to the display when
 * none holds); an output the display hardware turns by 180 and one the
 * compositor's turn pass turns by 90 or 270 are composed, after the
 * window, its fullscreen, its content type and its GPU buffer, before the
 * size, whatever follows; and the answers' words.
 */

#include "scanout-rules.h"

#include <stdio.h>
#include <string.h>

/* The number of failed checks. */
static unsigned failures;

static void check(int condition, const char *what);
static void test_facts(struct kwl_scanout_facts *facts);
static void test_unrotated(void);
static void test_rotated(void);
static void test_names(void);

/* Runs every check. */
int
main(void)
{
	/* Each part. */
	test_unrotated();
	test_rotated();
	test_names();

	/* The verdict. */
	if (failures != 0U) {
		printf("host-scanout-rules: %u failures\n", failures);
		return 1;
	}

	/* Succeeded. */
	printf("host-scanout-rules: PASS\n");
	return 0;
}

/* Counts a failed check and names it. */
static void
check(
	int condition,
	const char *what)
{
	/* A check that holds says nothing. */
	if (condition)
		return;

	/* One that fails is named. */
	printf("FAIL: %s\n", what);
	failures++;
}

/* Makes the facts of a pass that goes straight to the display: a still fullscreen game of the output's size. */
static void
test_facts(
	struct kwl_scanout_facts *facts)
{
	/* Everything the rules ask for. */
	memset(facts, 0, sizeof(*facts));
	facts->window = 1U;
	facts->fullscreen = 1U;
	facts->content_type = KWL_SCANOUT_CONTENT_GAME;
	facts->gpu_buffer = 1U;
	facts->size_matches = 1U;
	facts->pointer_still_ms = KWL_SCANOUT_POINTER_IDLE_MS;
}

/* The rules' order on an unrotated output, as before WS217. */
static void
test_unrotated(void)
{
	struct kwl_scanout_facts facts;
	unsigned answer;

	/* Straight to the display. */
	test_facts(&facts);
	answer = kwl_scanout_decide(&facts);
	check(answer == KWL_SCANOUT_DIRECT, "a still fullscreen game goes straight");
	facts.content_type = KWL_SCANOUT_CONTENT_VIDEO;
	answer = kwl_scanout_decide(&facts);
	check(answer == KWL_SCANOUT_DIRECT, "a still fullscreen video goes straight");

	/* Each reason, the first in the order. */
	test_facts(&facts);
	facts.window = 0U;
	answer = kwl_scanout_decide(&facts);
	check(answer == KWL_SCANOUT_NO_WINDOW, "no window");
	test_facts(&facts);
	facts.fullscreen = 0U;
	answer = kwl_scanout_decide(&facts);
	check(answer == KWL_SCANOUT_NOT_FULLSCREEN, "not fullscreen");
	test_facts(&facts);
	facts.content_type = 0U;
	answer = kwl_scanout_decide(&facts);
	check(answer == KWL_SCANOUT_CONTENT, "no content type");
	test_facts(&facts);
	facts.gpu_buffer = 0U;
	answer = kwl_scanout_decide(&facts);
	check(answer == KWL_SCANOUT_BUFFER, "shared memory");
	test_facts(&facts);
	facts.size_matches = 0U;
	answer = kwl_scanout_decide(&facts);
	check(answer == KWL_SCANOUT_SIZE, "another size");
	test_facts(&facts);
	facts.overlay = 1U;
	answer = kwl_scanout_decide(&facts);
	check(answer == KWL_SCANOUT_OVERLAY, "an overlay");
	test_facts(&facts);
	facts.shot = 1U;
	answer = kwl_scanout_decide(&facts);
	check(answer == KWL_SCANOUT_SHOT, "a shot");
	test_facts(&facts);
	facts.content_type = KWL_SCANOUT_CONTENT_VIDEO;
	facts.pointer_still_ms = KWL_SCANOUT_POINTER_IDLE_MS - 1U;
	answer = kwl_scanout_decide(&facts);
	check(answer == KWL_SCANOUT_POINTER, "a moving pointer over a video");
	test_facts(&facts);
	facts.pointer_still_ms = 0U;
	answer = kwl_scanout_decide(&facts);
	check(answer == KWL_SCANOUT_DIRECT, "a moving pointer over a game, which shows no cursor");
	test_facts(&facts);
	facts.refused = 1U;
	answer = kwl_scanout_decide(&facts);
	check(answer == KWL_SCANOUT_REFUSED, "refused");
}

/* A rotated output is composed: after the GPU buffer, before the size. */
static void
test_rotated(void)
{
	static const unsigned scanouts[4] = { 180U, 0U, 0U, 0U };
	static const unsigned turns[4] = { 0U, 90U, 270U, 180U };
	struct kwl_scanout_facts facts;
	unsigned answer;
	unsigned index;

	/* The hardware's 180, the turn's 90 and 270, and the test policy's 180 by the turn. */
	for (index = 0U; index < 4U; index++) {
		/* Everything else would go straight: composed. */
		test_facts(&facts);
		facts.scanout_rotation = scanouts[index];
		facts.turn_rotation = turns[index];
		answer = kwl_scanout_decide(&facts);
		check(answer == KWL_SCANOUT_ROTATED, "a rotated output is composed");

		/* Before the size and everything after it. */
		facts.size_matches = 0U;
		facts.overlay = 1U;
		facts.shot = 1U;
		facts.pointer_still_ms = 0U;
		facts.refused = 1U;
		answer = kwl_scanout_decide(&facts);
		check(answer == KWL_SCANOUT_ROTATED, "the rotation comes before the size");

		/* After the window, its fullscreen, its content type and its buffer. */
		test_facts(&facts);
		facts.scanout_rotation = scanouts[index];
		facts.turn_rotation = turns[index];
		facts.gpu_buffer = 0U;
		answer = kwl_scanout_decide(&facts);
		check(answer == KWL_SCANOUT_BUFFER, "the buffer comes before the rotation");
		facts.content_type = 0U;
		answer = kwl_scanout_decide(&facts);
		check(answer == KWL_SCANOUT_CONTENT, "the content type comes before the rotation");
		facts.fullscreen = 0U;
		answer = kwl_scanout_decide(&facts);
		check(answer == KWL_SCANOUT_NOT_FULLSCREEN, "fullscreen comes before the rotation");
	}
}

/* The answers' words for the log. */
static void
test_names(void)
{
	const char *name;
	int differs;

	/* The new answer and the ones around it. */
	name = kwl_scanout_reason_name(KWL_SCANOUT_ROTATED);
	differs = strcmp(name, "rotated");
	check(differs == 0, "the rotation's word");
	name = kwl_scanout_reason_name(KWL_SCANOUT_REFUSED);
	differs = strcmp(name, "refused");
	check(differs == 0, "the refusal's word");
	name = kwl_scanout_reason_name(KWL_SCANOUT_ROTATED + 1U);
	differs = strcmp(name, "unknown");
	check(differs == 0, "a number past the answers");
}
