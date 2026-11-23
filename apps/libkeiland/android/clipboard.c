/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The clipboard on Android: not made yet (the calls are taken and do
 * nothing), so nothing is copied and nothing can be pasted.
 */

#include "apps/libkeiland/android/android.h"

/* Takes text to copy; nothing is done yet. */
void
kl_window_copy(
	struct kl_window *window,
	const char *text,
	size_t length)
{
	(void)window;
	(void)text;
	(void)length;

	/* Nothing is done yet. */
}

/* Gives the text to paste: none yet. */
size_t
kl_window_paste(
	struct kl_window *window,
	char *text,
	size_t size)
{
	(void)window;
	(void)text;
	(void)size;

	/* None. */
	return 0U;
}

/* Says whether there is text to paste: not yet. */
int
kl_window_can_paste(
	const struct kl_window *window)
{
	(void)window;

	/* None. */
	return 0;
}

/* Takes text selected; nothing is done yet. */
void
kl_window_select(
	struct kl_window *window,
	const char *text,
	size_t length)
{
	(void)window;
	(void)text;
	(void)length;

	/* Nothing is done yet. */
}

/* Gives the selected text to paste: none yet. */
size_t
kl_window_paste_primary(
	struct kl_window *window,
	char *text,
	size_t size)
{
	(void)window;
	(void)text;
	(void)size;

	/* None. */
	return 0U;
}

/* Says whether the window owns a selection: not yet. */
int
kl_window_selection_own(
	const struct kl_window *window,
	unsigned which)
{
	(void)window;
	(void)which;

	/* None. */
	return 0;
}

/* Takes the clipboard's text the system told; nothing is done with it yet. */
void
android_clipboard_bridge(
	struct kl_app *app,
	const struct keiland_bridge_event *event)
{
	(void)app;
	(void)event;

	/* Nothing is done yet. */
}
