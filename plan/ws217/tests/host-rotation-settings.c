/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of ws217-p004: the displays' angles on the wire and in
 * Settings.  It checks the table of kl_system_displays_v1 at version 34
 * (the requests set_rotation, bind_touch and listen_touch, the events
 * rotation, rotations and touch, at the opcodes kl-system-protocol.h
 * names), libkeiland's view of a snapshot's angles (system-view.c: kept
 * with each display, put into effect at done, ENODEV for a key not in the
 * snapshot or not told), the bits shared by the wire, libkeiland and the
 * compositor, and the Display page's pictures and reasons
 * (settings/rotate.c).  Built by host-rotation-settings.sh.
 */

#include "userland/desktop/libkeiland/system/system-private.h"
#include "userland/desktop/libkeiland/system/kl-system-protocol.h"
#include "userland/desktop/settings/rotate.h"
#include "userland/desktop/wayland/rotate.h"

#include <wayland-client.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The table libkeiland's system-protocol.c defines. */
extern const struct wl_interface kl_system_displays_v1_interface;

/* How many checks failed; the test passes with none. */
static unsigned test_failures;

static void check(int passed, const char *what);
static void test_wire(void);
static void test_bits(void);
static void test_view(void);
static void test_view_display(struct system_view *view, const char *key);
static void test_page(void);

/* Runs every check; returns 0 when all passed. */
int
main(void)
{
	/* Each part in turn. */
	test_wire();
	test_bits();
	test_view();
	test_page();

	/* Failed checks. */
	if (test_failures != 0U) {
		printf("host-rotation-settings: FAIL (%u)\n", test_failures);
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("host-rotation-settings: PASS\n");
	return 0;
}

/* Counts and reports a failed check. */
static void
check(
	int passed,
	const char *what)
{
	/* A passed check says nothing. */
	if (passed)
		return;
	printf("FAIL: %s\n", what);
	test_failures++;
}

/* The table of kl_system_displays_v1: version 34, its requests and events at the header's opcodes, with their arguments. */
static void
test_wire(void)
{
	const struct wl_interface *table;
	int differs;

	/* The version and the counts. */
	table = &kl_system_displays_v1_interface;
	check(table->version == (int)KL_SYSTEM_SINCE_ROTATION, "the displays' version is 34");
	check(KL_SYSTEM_SINCE_ROTATION == 34U, "the angles come with 34");
	check(KL_SYSTEM_MANAGER_VERSION >= KL_SYSTEM_SINCE_ROTATION, "the manager offers 34");
	check(table->method_count == 7, "seven requests");
	check(table->event_count == 6, "six events");

	/* The requests of 34: set_rotation(uint, string, uint), bind_touch(uint, string, string), listen_touch(uint, string). */
	differs = strcmp(table->methods[KL_SYSTEM_DISPLAYS_SET_ROTATION].name, "set_rotation");
	check(differs == 0, "request 4 is set_rotation");
	differs = strcmp(table->methods[KL_SYSTEM_DISPLAYS_SET_ROTATION].signature, "usu");
	check(differs == 0, "set_rotation is usu");
	differs = strcmp(table->methods[KL_SYSTEM_DISPLAYS_BIND_TOUCH].name, "bind_touch");
	check(differs == 0, "request 5 is bind_touch");
	differs = strcmp(table->methods[KL_SYSTEM_DISPLAYS_BIND_TOUCH].signature, "uss");
	check(differs == 0, "bind_touch is uss");
	differs = strcmp(table->methods[KL_SYSTEM_DISPLAYS_LISTEN_TOUCH].name, "listen_touch");
	check(differs == 0, "request 6 is listen_touch");
	differs = strcmp(table->methods[KL_SYSTEM_DISPLAYS_LISTEN_TOUCH].signature, "us");
	check(differs == 0, "listen_touch is us");

	/* The events of 34: rotation(string, uint), rotations(string, uint, uint), touch(string, string, uint, string). */
	differs = strcmp(table->events[KL_SYSTEM_DISPLAYS_EVENT_ROTATION].name, "rotation");
	check(differs == 0, "event 3 is rotation");
	differs = strcmp(table->events[KL_SYSTEM_DISPLAYS_EVENT_ROTATION].signature, "su");
	check(differs == 0, "rotation is su");
	differs = strcmp(table->events[KL_SYSTEM_DISPLAYS_EVENT_ROTATIONS].name, "rotations");
	check(differs == 0, "event 4 is rotations");
	differs = strcmp(table->events[KL_SYSTEM_DISPLAYS_EVENT_ROTATIONS].signature, "suu");
	check(differs == 0, "rotations is suu");
	differs = strcmp(table->events[KL_SYSTEM_DISPLAYS_EVENT_TOUCH].name, "touch");
	check(differs == 0, "event 5 is touch");
	differs = strcmp(table->events[KL_SYSTEM_DISPLAYS_EVENT_TOUCH].signature, "ssus");
	check(differs == 0, "touch is ssus");

	/* The requests and events of before keep their places. */
	differs = strcmp(table->methods[KL_SYSTEM_DISPLAYS_SET_SHOWN].name, "set_shown");
	check(differs == 0, "request 3 is still set_shown");
	differs = strcmp(table->events[KL_SYSTEM_DISPLAYS_EVENT_RESULT].name, "result");
	check(differs == 0, "event 2 is still result");
}

/* The angles' and reasons' bits are the same on the wire, in libkeiland and in the compositor. */
static void
test_bits(void)
{
	/* The angles' bits. */
	check(KL_SYSTEM_ROTATION_0 == KL_DISPLAY_ROTATION_0 && KL_DISPLAY_ROTATION_0 == KWL_ROTATION_0, "the bit of 0");
	check(KL_SYSTEM_ROTATION_90 == KL_DISPLAY_ROTATION_90 && KL_DISPLAY_ROTATION_90 == KWL_ROTATION_90, "the bit of 90");
	check(KL_SYSTEM_ROTATION_180 == KL_DISPLAY_ROTATION_180 && KL_DISPLAY_ROTATION_180 == KWL_ROTATION_180, "the bit of 180");
	check(KL_SYSTEM_ROTATION_270 == KL_DISPLAY_ROTATION_270 && KL_DISPLAY_ROTATION_270 == KWL_ROTATION_270, "the bit of 270");

	/* The angles in degrees. */
	check(KL_DISPLAY_ROTATE_90 == KWL_ROTATE_90 && KL_DISPLAY_ROTATE_270 == KWL_ROTATE_270, "the degrees");

	/* The reasons' bits. */
	check(KL_SYSTEM_ROTATE_WHY_SCANOUT == KL_DISPLAY_ROTATE_WHY_SCANOUT && KL_DISPLAY_ROTATE_WHY_SCANOUT == KWL_ROTATE_WHY_SCANOUT, "why scanout");
	check(KL_SYSTEM_ROTATE_WHY_TURN == KL_DISPLAY_ROTATE_WHY_TURN && KL_DISPLAY_ROTATE_WHY_TURN == KWL_ROTATE_WHY_TURN, "why turn");
	check(KL_SYSTEM_ROTATE_WHY_GAME == KL_DISPLAY_ROTATE_WHY_GAME && KL_DISPLAY_ROTATE_WHY_GAME == KWL_ROTATE_WHY_GAME, "why game");
	check(KL_SYSTEM_ROTATE_WHY_RESOURCE == KL_DISPLAY_ROTATE_WHY_RESOURCE && KL_DISPLAY_ROTATE_WHY_RESOURCE == KWL_ROTATE_WHY_RESOURCE, "why resource");
	check(KL_SYSTEM_ROTATE_WHY_ALONE == KL_DISPLAY_ROTATE_WHY_ALONE && KL_DISPLAY_ROTATE_WHY_ALONE == KWL_ROTATE_WHY_ALONE, "why alone");

	/* The library this header describes. */
	check(KL_VERSION >= 93U, "KL_VERSION at least 93, which brought the angles");
}

/* libkeiland's view: the angles kept with each display of a snapshot. */
static void
test_view(void)
{
	struct system_view *view;
	struct system_view_rotation told;
	int error;

	/* A view (large: from the heap). */
	view = calloc(1U, sizeof(*view));
	if (view == NULL) {
		check(0, "the view");
		return;
	}

	/* A snapshot of two displays: the anchor told 90 with all four, the other 0 alone beside it. */
	test_view_display(view, "pci:0:edp:A");
	system_view_display_rotation(view, "pci:0:edp:A", 90U);
	system_view_display_rotations(view, "pci:0:edp:A", 15U, 0U);
	test_view_display(view, "pci:0:hdmi:B");
	system_view_display_rotation(view, "pci:0:hdmi:B", 0U);
	system_view_display_rotations(view, "pci:0:hdmi:B", 1U, 16U);

	/* Nothing in effect before the done. */
	error = system_view_rotation_of(view, "pci:0:edp:A", &told);
	check(error == ENODEV, "nothing before the done");
	system_view_displays_done(view, 1U, 0U);

	/* Each display's angle after it. */
	error = system_view_rotation_of(view, "pci:0:edp:A", &told);
	check(error == 0 && told.rotation == 90U && told.mask == 15U && told.why == 0U, "the anchor's angle");
	error = system_view_rotation_of(view, "pci:0:hdmi:B", &told);
	check(error == 0 && told.rotation == 0U && told.mask == 1U && told.why == 16U, "the other's angle");
	error = system_view_rotation_of(view, "pci:0:dp:C", &told);
	check(error == ENODEV, "a key not in the snapshot");

	/* A new snapshot whose display was not told its angle (a compositor before 34), and an angle for a key it does not hold. */
	test_view_display(view, "pci:0:edp:A");
	system_view_display_rotation(view, "pci:0:dp:C", 180U);
	system_view_displays_done(view, 2U, 0U);
	error = system_view_rotation_of(view, "pci:0:edp:A", &told);
	check(error == ENODEV, "a display not told its angle");
	error = system_view_rotation_of(view, "pci:0:hdmi:B", &told);
	check(error == ENODEV, "a display gone");

	/* An angle told outside a snapshot is dropped. */
	system_view_display_rotation(view, "pci:0:edp:A", 270U);
	error = system_view_rotation_of(view, "pci:0:edp:A", &told);
	check(error == ENODEV, "an angle outside a snapshot");

	/* The view goes. */
	free(view);
}

/* Adds a display of a key to the view's snapshot being sent. */
static void
test_view_display(
	struct system_view *view,
	const char *key)
{
	struct kl_display display;

	/* The display, shown. */
	memset(&display, 0, sizeof(display));
	(void)snprintf(display.key, sizeof(display.key), "%s", key);
	display.width = 1920U;
	display.height = 1080U;
	display.flags = KL_DISPLAY_SHOWN;
	system_view_display(view, &display);
}

/* The Display page's pictures: their order, which can be pressed, and the reason said. */
static void
test_page(void)
{
	unsigned reason;
	int pressable;

	/* The order: upright, on its left side, upside down, on its right side. */
	check(se_rotate_angle(0U) == 0U, "the first picture is 0");
	check(se_rotate_angle(1U) == 90U, "the second is 90");
	check(se_rotate_angle(2U) == 180U, "the third is 180");
	check(se_rotate_angle(3U) == 270U, "the fourth is 270");
	check(se_rotate_angle(4U) == 0U, "past the last: 0");

	/* The angle chosen cannot be pressed again; an angle in the mask can; one outside cannot. */
	pressable = se_rotate_pressable(90U, 90U, 15U);
	check(pressable == 0, "the angle chosen");
	pressable = se_rotate_pressable(270U, 90U, 15U);
	check(pressable == 1, "another angle allowed");
	pressable = se_rotate_pressable(90U, 0U, 5U);
	check(pressable == 0, "an angle outside the mask");
	pressable = se_rotate_pressable(180U, 0U, 5U);
	check(pressable == 1, "180 by the hardware");
	pressable = se_rotate_pressable(45U, 0U, 15U);
	check(pressable == 0, "not an angle");

	/* The reasons, the one leaving the least choice first. */
	reason = se_rotate_reason(0U);
	check(reason == SE_ROTATE_REASON_NONE, "no reason");
	reason = se_rotate_reason(KL_DISPLAY_ROTATE_WHY_SCANOUT);
	check(reason == SE_ROTATE_REASON_FIXED, "fixed");
	reason = se_rotate_reason(KL_DISPLAY_ROTATE_WHY_SCANOUT | KL_DISPLAY_ROTATE_WHY_ALONE);
	check(reason == SE_ROTATE_REASON_FIXED, "fixed before alone");
	reason = se_rotate_reason(KL_DISPLAY_ROTATE_WHY_ALONE | KL_DISPLAY_ROTATE_WHY_TURN);
	check(reason == SE_ROTATE_REASON_ALONE, "alone before the quarter turns");
	reason = se_rotate_reason(KL_DISPLAY_ROTATE_WHY_RESOURCE | KL_DISPLAY_ROTATE_WHY_GAME);
	check(reason == SE_ROTATE_REASON_RESOURCE, "the memory before the game");
	reason = se_rotate_reason(KL_DISPLAY_ROTATE_WHY_GAME);
	check(reason == SE_ROTATE_REASON_GAME, "the game");
	reason = se_rotate_reason(KL_DISPLAY_ROTATE_WHY_TURN);
	check(reason == SE_ROTATE_REASON_QUARTER, "the quarter turns");
}
