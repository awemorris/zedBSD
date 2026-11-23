/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The title bar's controls, menu and tabs on Android: not made yet (the
 * calls are taken and do nothing), so a program that sets them goes on
 * without.
 */

#include "apps/libkeiland/android/android.h"

#include <errno.h>

/* Takes the controls; none is shown yet. */
int
kl_window_set_controls(
	struct kl_window *window,
	const struct kl_control_entry *entries,
	size_t count)
{
	(void)window;
	(void)entries;
	(void)count;

	/* Nothing is shown yet. */
	return 0;
}

/* Takes an action's state; none is shown yet. */
int
kl_window_set_action_state(
	struct kl_window *window,
	uint32_t action,
	unsigned state)
{
	(void)window;
	(void)action;
	(void)state;

	/* Nothing is shown yet. */
	return 0;
}

/* Takes a control's text; none is shown yet. */
int
kl_window_set_control_text(
	struct kl_window *window,
	uint32_t id,
	const char *text,
	const char *placeholder)
{
	(void)window;
	(void)id;
	(void)text;
	(void)placeholder;

	/* Nothing is shown yet. */
	return 0;
}

/* Takes a control's parts; none is shown yet. */
int
kl_window_set_control_parts(
	struct kl_window *window,
	uint32_t id,
	const char *const *parts,
	size_t count)
{
	(void)window;
	(void)id;
	(void)parts;
	(void)count;

	/* Nothing is shown yet. */
	return 0;
}

/* Takes a control's value; none is shown yet. */
int
kl_window_set_control_value(
	struct kl_window *window,
	uint32_t id,
	unsigned value)
{
	(void)window;
	(void)id;
	(void)value;

	/* Nothing is shown yet. */
	return 0;
}

/* Takes the way a control takes the focus; none is shown yet. */
int
kl_window_focus_control_mode(
	struct kl_window *window,
	uint32_t id,
	unsigned mode)
{
	(void)window;
	(void)id;
	(void)mode;

	/* Nothing is shown yet. */
	return 0;
}

/* Gives a control the focus; none is shown yet. */
int
kl_window_focus_control(
	struct kl_window *window,
	uint32_t id)
{
	(void)window;
	(void)id;

	/* Nothing is shown yet. */
	return 0;
}

/* Takes the window's menu; none is shown yet. */
int
kl_window_set_menu(
	struct kl_window *window,
	const struct kl_menu_entry *entries,
	size_t count)
{
	(void)window;
	(void)entries;
	(void)count;

	/* Nothing is shown yet. */
	return 0;
}

/* Takes a popup menu; none is shown yet. */
int
kl_window_popup_menu(
	struct kl_window *window,
	const struct kl_menu_entry *entries,
	size_t count,
	int x,
	int y)
{
	(void)window;
	(void)entries;
	(void)count;
	(void)x;
	(void)y;

	/* Nothing is shown yet. */
	return 0;
}

/* Takes the window's tabs; none is shown yet. */
int
kl_window_set_tabs(
	struct kl_window *window,
	const struct kl_tab_entry *tabs,
	size_t count,
	unsigned options)
{
	(void)window;
	(void)tabs;
	(void)count;
	(void)options;

	/* Nothing is shown yet. */
	return 0;
}

/* Says whether the strip took a press: it has none yet, so the press is the content's. */
int
android_titlebar_hit(
	struct kl_window *window,
	double x,
	double y,
	int pressed)
{
	(void)window;
	(void)x;
	(void)y;
	(void)pressed;

	/* Not taken. */
	return 0;
}
