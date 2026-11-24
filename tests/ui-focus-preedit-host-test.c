/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "userland/desktop/include/keiland/keiland_ui.h"
#include "userland/desktop/include/keiland/keiland_window.h"
#include "userland/desktop/libkeiland/ui/internal.h"

int
kl_scroll_init(
	struct kl_scroll *scroll,
	unsigned axes)
{
	(void)scroll;
	(void)axes;
	return 0;
}

void
kl_scroll_release(
	struct kl_scroll *scroll)
{
	(void)scroll;
}

struct kl_gesture *
kl_gesture_create(void)
{
	static int dummy;

	return (struct kl_gesture *)&dummy;
}

void
kl_gesture_destroy(
	struct kl_gesture *gesture)
{
	(void)gesture;
}

static int
want_all_keys(
	uint32_t code,
	unsigned modifiers)
{
	(void)code;
	(void)modifiers;
	return 1;
}

int
main(void)
{
	struct kl_ui *ui;
	struct kl_window_event event;
	struct keiui_input input;
	const char *preedit;
	int32_t begin;
	int32_t end;

	ui = kl_ui_create();
	assert(ui != NULL);

	/* Composing on widget 1 then moving focus to widget 2 commits "abc" to widget 1. */
	kl_ui_set_focus(ui, 1U, 0U);
	memset(&event, 0, sizeof(event));
	event.kind = KL_WINDOW_TEXT_PREEDIT;
	snprintf(event.text, sizeof(event.text), "%s", "abc");
	event.begin = 3;
	event.end = 3;
	assert(kl_ui_text(ui, &event) == 1);

	preedit = keiui_ui_preedit(ui, 1U, 0U, 1, &begin, &end);
	assert(preedit != NULL && strcmp(preedit, "abc") == 0);

	kl_ui_set_focus(ui, 2U, 0U);
	assert(keiui_ui_preedit(ui, 2U, 0U, 1, &begin, &end) == NULL);
	assert(keiui_ui_take_input(ui, 2U, 0U, want_all_keys, &input) == 0);
	assert(keiui_ui_take_input(ui, 1U, 0U, want_all_keys, &input) == 1);
	assert(input.kind == KEIUI_INPUT_COMMIT);
	assert(strcmp(input.text, "abc") == 0);

	/* Composing on widget 2 then clearing focus commits "xyz" to widget 2. */
	memset(&event, 0, sizeof(event));
	event.kind = KL_WINDOW_TEXT_PREEDIT;
	snprintf(event.text, sizeof(event.text), "%s", "xyz");
	event.begin = 3;
	event.end = 3;
	assert(kl_ui_text(ui, &event) == 1);
	kl_ui_clear_focus(ui);
	assert(keiui_ui_preedit(ui, 2U, 0U, 1, &begin, &end) == NULL);
	assert(keiui_ui_take_input(ui, 2U, 0U, want_all_keys, &input) == 1);
	assert(input.kind == KEIUI_INPUT_COMMIT);
	assert(strcmp(input.text, "xyz") == 0);

	kl_ui_destroy(ui);
	printf("ui-focus-preedit-host-test: PASS\n");
	return 0;
}
