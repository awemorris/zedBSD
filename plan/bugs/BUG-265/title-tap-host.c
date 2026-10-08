/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BUG-265's host test: the double click on a floating title bar decided at
 * its release (userland/desktop/wayland/title-tap.c).
 */

#include "userland/desktop/wayland/title-tap.h"

#include <stdio.h>

static int failures;

#define CHECK(condition, what) do { if (!(condition)) { failures++; fprintf(stderr, "FAIL: %s\n", what); } } while (0)

int
main(void)
{
	struct kwl_title_tap tap = { NULL, 0, 0 };
	const void *window;
	int one;
	int two;

	/* No double click: a release is a move's end. */
	CHECK(kwl_title_tap_release(&tap, 10, 10, &window) == TITLE_TAP_NONE && window == NULL, "no wait");

	/* A double click let go where it was pressed: the window docks, and nothing waits after. */
	kwl_title_tap_press(&tap, &one, 100, 40);
	CHECK(kwl_title_tap_still(&tap, &one, 100, 40) == 1, "still at the press");
	CHECK(kwl_title_tap_release(&tap, 100, 40, &window) == TITLE_TAP_DOCK && window == &one, "double click docks");
	CHECK(kwl_title_tap_release(&tap, 100, 40, &window) == TITLE_TAP_NONE, "once");

	/* A finger's jitter within the slop is still a double click. */
	kwl_title_tap_press(&tap, &one, 100, 40);
	CHECK(kwl_title_tap_still(&tap, &one, 105, 45) == 1, "jitter still");
	CHECK(kwl_title_tap_release(&tap, 105, 45, &window) == TITLE_TAP_DOCK, "jitter docks");

	/* A tap and drag: past the slop it is a move, and the window moves. */
	kwl_title_tap_press(&tap, &one, 100, 40);
	CHECK(kwl_title_tap_still(&tap, &one, 109, 40) == 0, "moved past the slop");
	CHECK(kwl_title_tap_still(&tap, &two, 100, 40) == 0, "another window is not waited on");
	CHECK(kwl_title_tap_release(&tap, 300, 200, &window) == TITLE_TAP_MOVED && window == &one, "tap and drag moves");

	/* Moved away and back before the release: decided where it is let go. */
	kwl_title_tap_press(&tap, &one, 100, 40);
	CHECK(kwl_title_tap_release(&tap, 102, 41, &window) == TITLE_TAP_DOCK, "decided at the release");

	/* A window that goes is forgotten; another's going is not. */
	kwl_title_tap_press(&tap, &one, 100, 40);
	kwl_title_tap_forget(&tap, &two);
	CHECK(tap.window == &one, "another's going keeps it");
	kwl_title_tap_forget(&tap, &one);
	CHECK(kwl_title_tap_release(&tap, 100, 40, &window) == TITLE_TAP_NONE, "the window's going forgets it");
	kwl_title_tap_press(&tap, &one, 100, 40);
	kwl_title_tap_forget(&tap, NULL);
	CHECK(tap.window == NULL, "forget all");

	/* Far coordinates do not overflow. */
	kwl_title_tap_press(&tap, &one, -2000000000, -2000000000);
	CHECK(kwl_title_tap_release(&tap, 2000000000, 2000000000, &window) == TITLE_TAP_MOVED, "no overflow");

	if (failures != 0)
		return 1;
	printf("title-tap-host: PASS\n");
	return 0;
}
