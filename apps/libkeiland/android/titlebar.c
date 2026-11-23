/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The title bar's controls, menu and tabs on Android: not shown yet (the
 * calls are taken and do nothing), so a program that sets them goes on
 * without.  The menu's shortcuts work, though: a key pressed with Ctrl, Alt or
 * Super held (on a keyboard, or the key bar's Ctrl and Alt) chooses the item
 * that has it, so Edit > Copy and Paste can be reached.
 */

#include "apps/libkeiland/android/android.h"

#include <android/keycodes.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The XKB keysyms of the keys a shortcut names that are not letters or digits. */
#define TITLEBAR_KEYSYM_PLUS	0x2bU
#define TITLEBAR_KEYSYM_MINUS	0x2dU
#define TITLEBAR_KEYSYM_EQUAL	0x3dU

static uint32_t titlebar_keysym(int32_t keycode);

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

/* Takes the window's menu: none is shown yet, but the items that have a shortcut are kept for the keys. */
int
kl_window_set_menu(
	struct kl_window *window,
	const struct kl_menu_entry *entries,
	size_t count)
{
	struct android_shortcut *shortcuts;
	size_t kept;
	size_t index;

	/* A window. */
	if (window == NULL)
		return EINVAL;

	/* The items that have a shortcut. */
	shortcuts = NULL;
	kept = 0U;
	if (entries != NULL && count != 0U) {
		shortcuts = malloc(count * sizeof(*shortcuts));
		if (shortcuts == NULL)
			return ENOMEM;
		for (index = 0U; index < count; index++) {
			if (entries[index].keysym == 0U)
				continue;
			shortcuts[kept].keysym = entries[index].keysym;
			shortcuts[kept].modifiers = entries[index].modifiers;
			shortcuts[kept].id = entries[index].id;
			shortcuts[kept].action = entries[index].action;
			kept++;
		}
	}

	/* They replace the earlier ones. */
	free(window->shortcuts);
	window->shortcuts = shortcuts;
	window->shortcut_count = kept;

	/* Nothing is shown yet. */
	return 0;
}

/* Lets the shortcuts kept for a window go. */
void
android_titlebar_release(
	struct kl_window *window)
{
	free(window->shortcuts);
	window->shortcuts = NULL;
	window->shortcut_count = 0U;
}

/* Turns a key pressed with Ctrl, Alt or Super held into the action of the menu item that has it. */
int
android_titlebar_shortcut(
	struct kl_window *window,
	int32_t keycode,
	int32_t meta)
{
	struct kl_window_event input;
	uint32_t keysym;
	uint32_t wanted;
	unsigned held;
	size_t index;

	/* Only a key with Ctrl, Alt or Super is a shortcut; a plain key is typed. */
	held = 0U;
	if ((meta & AMETA_SHIFT_ON) != 0)
		held |= KL_MENU_SHIFT;
	if ((meta & AMETA_CTRL_ON) != 0)
		held |= KL_MENU_CTRL;
	if ((meta & AMETA_ALT_ON) != 0)
		held |= KL_MENU_ALT;
	if ((meta & AMETA_META_ON) != 0)
		held |= KL_MENU_SUPER;
	if ((held & (KL_MENU_CTRL | KL_MENU_ALT | KL_MENU_SUPER)) == 0U)
		return 0;

	/* The key's keysym, a letter in its small form. */
	keysym = titlebar_keysym(keycode);
	if (keysym == 0U)
		return 0;

	/* The item with the same modifiers and key; the plus is also the equal sign (the key without Shift). */
	for (index = 0U; index < window->shortcut_count; index++) {
		if (window->shortcuts[index].modifiers != held)
			continue;
		wanted = window->shortcuts[index].keysym;
		if (wanted >= 'A' && wanted <= 'Z')
			wanted += 'a' - 'A';
		if (wanted == TITLEBAR_KEYSYM_PLUS && keysym == TITLEBAR_KEYSYM_EQUAL)
			wanted = TITLEBAR_KEYSYM_EQUAL;
		if (wanted != keysym)
			continue;

		/* The item is chosen. */
		memset(&input, 0, sizeof(input));
		input.kind = KL_WINDOW_ACTION;
		input.code = window->shortcuts[index].action;
		input.id = window->shortcuts[index].id;
		(void)android_window_queue(window, &input);
		return 1;
	}

	/* No item has it. */
	return 0;
}

/* Gives the keysym of an Android key that a shortcut can name: a letter (small), a digit, the minus and the equal sign; 0 for others. */
static uint32_t
titlebar_keysym(
	int32_t keycode)
{
	if (keycode >= AKEYCODE_A && keycode <= AKEYCODE_Z)
		return (uint32_t)('a' + (keycode - AKEYCODE_A));
	if (keycode >= AKEYCODE_0 && keycode <= AKEYCODE_9)
		return (uint32_t)('0' + (keycode - AKEYCODE_0));
	if (keycode == AKEYCODE_MINUS)
		return TITLEBAR_KEYSYM_MINUS;
	if (keycode == AKEYCODE_EQUALS)
		return TITLEBAR_KEYSYM_EQUAL;
	return 0U;
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
