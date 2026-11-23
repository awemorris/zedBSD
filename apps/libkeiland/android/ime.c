/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text input on Android: not made yet (the calls are taken and do
 * nothing), so the soft keyboard does not come up and no text arrives.
 */

#include "apps/libkeiland/android/android.h"

/* Takes whether the window wants text; nothing is done yet. */
void
kl_window_text_input(
	struct kl_window *window,
	int enabled)
{
	(void)window;
	(void)enabled;

	/* Nothing is done yet. */
}

/* Takes where the caret is; nothing is done yet. */
void
kl_window_text_cursor(
	struct kl_window *window,
	int x,
	int y,
	int width,
	int height)
{
	(void)window;
	(void)x;
	(void)y;
	(void)width;
	(void)height;

	/* Nothing is done yet. */
}

/* Takes the text around the caret; nothing is done yet. */
void
kl_window_text_context(
	struct kl_window *window,
	const char *text,
	size_t cursor,
	size_t anchor,
	unsigned hint,
	unsigned purpose)
{
	(void)window;
	(void)text;
	(void)cursor;
	(void)anchor;
	(void)hint;
	(void)purpose;

	/* Nothing is done yet. */
}

/* Takes the text the input method sent; nothing is done with it yet. */
void
android_ime_bridge(
	struct kl_app *app,
	const struct keiland_bridge_event *event)
{
	(void)app;
	(void)event;

	/* Nothing is done yet. */
}
