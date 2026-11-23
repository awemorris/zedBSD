/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The title bar on Android: a strip at the top of the window that libkeiland
 * draws, with a button for the window's menu, the tabs the program gave in
 * place of the title (as the Windows backend's title bar shows them), and
 * the new tab's button.  Tabs that do not fit scroll with a finger's swipe
 * along the strip, and a button lists the ones out of sight.
 *
 * The strip is laid out and drawn on M into a picture of its own (the window
 * width by the strip's height), which present.c writes above the program's
 * frame.  The window the program draws in is the native window less the
 * insets and less the strip, so the program has nothing to follow.
 *
 * A finger on the strip is the strip's, from its press to its release (it
 * never reaches the program): a release on the part that was pressed chooses
 * it; a hold of half a second on a tab opens that tab's menu.  The menu and
 * the other lists are Java's PopupMenu (Menus.java), asked for with a request
 * number and answered with the item's position.  The menu's shortcuts are
 * matched here, for a hardware keyboard and the key bar's Ctrl and Alt.
 *
 * The program's controls (kl_window_set_controls and the like) are taken and
 * not shown: a program of this layer has none.
 */

#include "apps/libkeiland/android/android.h"
#include "apps/libkeiland/android/bridge-jni.h"

#include <android/keycodes.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The XKB keysyms of the keys a shortcut names that are not letters or digits. */
#define TITLEBAR_KEYSYM_PLUS	0x2bU
#define TITLEBAR_KEYSYM_MINUS	0x2dU
#define TITLEBAR_KEYSYM_EQUAL	0x3dU

/* The icons' sides and the tab's insets at 96 dpi. */
#define TITLEBAR_ICON		20U
#define TITLEBAR_TAB_ICON	14U
#define TITLEBAR_TAB_INSET	12U
#define TITLEBAR_TAB_MARK	7U
#define TITLEBAR_RADIUS		8U

/* How far a finger may wander from where it pressed and still be a press, at 96 dpi. */
#define TITLEBAR_SLOP		12U

/* The popup menu's flags the Java side reads. */
#define TITLEBAR_FLAG_ENABLED	1
#define TITLEBAR_FLAG_CHECKABLE	2
#define TITLEBAR_FLAG_CHECKED	4
#define TITLEBAR_FLAG_SEPARATOR	8

/* One item of a popup as it is asked of Java: its label, its parent's position (-1 for none), its flags, and what choosing it does. */
struct titlebar_entry {
	const char *label;
	int32_t parent;
	int32_t flags;
	struct android_popup_item item;
};

/* The request number of the next popup (never 0), and the paths of the strip's font companions (the text library keeps the pointers). */
static uint32_t titlebar_next_request = 1U;
static char titlebar_companion_bold[512];
static char titlebar_companion_mono[512];

static uint32_t titlebar_keysym(int32_t keycode);
static unsigned titlebar_state(const struct kl_window *window, uint32_t action);
static unsigned titlebar_pixels(const struct kl_window *window);
static void titlebar_font(struct kl_window *window);
static void titlebar_menu_free(struct kl_window *window);
static void titlebar_layout(struct kl_window *window);
static void titlebar_paint(struct kl_window *window);
static void titlebar_refresh(struct kl_window *window);
static void titlebar_tab_draw(struct kl_window *window, struct kl_canvas *canvas, unsigned index);
static void titlebar_button_draw(struct kl_window *window, struct kl_canvas *canvas, const struct android_rect *place, unsigned part, enum kl_icon icon, uint32_t size, int live);
static unsigned titlebar_part_at(const struct kl_window *window, int32_t x, int32_t y);
static unsigned titlebar_active(const struct kl_window *window);
static unsigned titlebar_part_kept(const struct kl_window *window, const struct android_tab *tabs, unsigned count, unsigned part);
static void titlebar_activate(struct kl_window *window, unsigned part);
static void titlebar_tab_input(struct kl_window *window, unsigned code, uint32_t id);
static void titlebar_tab_choose(struct kl_window *window, uint32_t id);
static void titlebar_scroll_by(struct kl_window *window, int steps);
static void titlebar_open_menu(struct kl_window *window);
static void titlebar_open_tab_menu(struct kl_window *window, unsigned index);
static void titlebar_open_list(struct kl_window *window);
static int titlebar_popup(struct kl_window *window, unsigned kind, const struct titlebar_entry *entries, size_t count, int x, int y);
static int titlebar_menu_entries(const struct kl_window *window, const struct android_menu_item *items, size_t count, struct titlebar_entry **entries, size_t *shown);
static int titlebar_menu_copy(const struct kl_menu_entry *entries, size_t count, struct android_menu_item **items);
static void titlebar_press_clear(struct kl_window *window);
static void titlebar_copy_text(char *to, size_t size, const char *from);

/* Takes the controls; none is shown. */
int
kl_window_set_controls(
	struct kl_window *window,
	const struct kl_control_entry *entries,
	size_t count)
{
	(void)window;
	(void)entries;
	(void)count;

	/* Nothing is shown. */
	return 0;
}

/* Keeps an action's state, which the menu's items and the popups show. */
int
kl_window_set_action_state(
	struct kl_window *window,
	uint32_t action,
	unsigned state)
{
	size_t index;

	/* A window. */
	if (window == NULL)
		return EINVAL;

	/* The action's place: its row when it has one. */
	for (index = 0U; index < window->bar.action_count; index++) {
		if (window->bar.actions[index].action == action)
			break;
	}
	if (index == window->bar.action_count) {
		if (index >= ANDROID_ACTIONS)
			return E2BIG;
		window->bar.actions[index].action = action;
		window->bar.action_count++;
	}
	window->bar.actions[index].state = state;

	/* Succeeded: the next popup shows it. */
	return 0;
}

/* Takes a control's text; none is shown. */
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

	/* Nothing is shown. */
	return 0;
}

/* Takes a control's parts; none is shown. */
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

	/* Nothing is shown. */
	return 0;
}

/* Takes a control's value; none is shown. */
int
kl_window_set_control_value(
	struct kl_window *window,
	uint32_t id,
	unsigned value)
{
	(void)window;
	(void)id;
	(void)value;

	/* Nothing is shown. */
	return 0;
}

/* Takes the way a control takes the focus; none is shown. */
int
kl_window_focus_control_mode(
	struct kl_window *window,
	uint32_t id,
	unsigned mode)
{
	(void)window;
	(void)id;
	(void)mode;

	/* Nothing is shown. */
	return 0;
}

/* Gives a control the focus; none is shown. */
int
kl_window_focus_control(
	struct kl_window *window,
	uint32_t id)
{
	(void)window;
	(void)id;

	/* Nothing is shown. */
	return 0;
}

/*
 * Declares the window's menu (count 0 takes it away): shown from its button
 * at the strip's left, and its items that have a shortcut are kept for the keys.
 */
int
kl_window_set_menu(
	struct kl_window *window,
	const struct kl_menu_entry *entries,
	size_t count)
{
	struct android_menu_item *items;
	int error;

	/* A window, and a table that fits. */
	if (window == NULL || (count != 0U && entries == NULL))
		return EINVAL;
	if (count > ANDROID_MENU_ITEMS)
		return E2BIG;

	/* The table's copy (none for no menu). */
	items = NULL;
	if (count != 0U) {
		error = titlebar_menu_copy(entries, count, &items);
		if (error != 0)
			return error;
	}

	/* It replaces the menu shown. */
	titlebar_menu_free(window);
	window->bar.menu = items;
	window->bar.menu_count = count;

	/* The strip grows or goes with the menu, or is only drawn again. */
	titlebar_refresh(window);

	/* Succeeded. */
	return 0;
}

/*
 * Declares the window's tabs (count 0 takes them away, and the title comes
 * back), with KL_TABS_* options.
 */
int
kl_window_set_tabs(
	struct kl_window *window,
	const struct kl_tab_entry *tabs,
	size_t count,
	unsigned options)
{
	struct android_tab *kept;
	uint32_t active;
	unsigned pressed;
	size_t index;

	/* A window, and tabs that fit, each with an ID. */
	if (window == NULL || (count != 0U && tabs == NULL))
		return EINVAL;
	if (count > ANDROID_TABS)
		return E2BIG;
	for (index = 0U; index < count; index++) {
		if (tabs[index].id == 0U)
			return EINVAL;
	}

	/* The tabs' copy (none for no tabs). */
	kept = NULL;
	if (count != 0U) {
		kept = calloc(count, sizeof(*kept));
		if (kept == NULL)
			return ENOMEM;
	}
	for (index = 0U; index < count; index++) {
		kept[index].id = tabs[index].id;
		titlebar_copy_text(kept[index].title, sizeof(kept[index].title), tabs[index].title);
		kept[index].flags = tabs[index].flags;
	}

	/* The active tab's ID (the first marked; 0 when none is). */
	active = 0U;
	for (index = 0U; index < count; index++) {
		if ((kept[index].flags & KL_TAB_ACTIVE) != 0U && active == 0U)
			active = kept[index].id;
	}

	/* Tabs that came or went bring the active tab into sight. */
	if ((unsigned)count != window->bar.tab_count)
		window->bar.reveal = 1;

	/* Another active tab is brought into sight; a change of titles alone leaves the strip where it was. */
	if (active != window->bar.revealed)
		window->bar.reveal = 1;

	/* What is held stays: the same button, or the tab of the same ID. */
	pressed = titlebar_part_kept(window, kept, (unsigned)count, window->bar.pressed);

	/* They replace the tabs shown. */
	free(window->bar.tabs);
	window->bar.tabs = kept;
	window->bar.tab_count = (unsigned)count;
	window->bar.tab_options = options;
	window->bar.pressed = pressed;
	if (pressed == ANDROID_PART_NONE)
		window->bar.finger = -1;

	/* The strip grows or goes with the tabs, or is only drawn again. */
	titlebar_refresh(window);

	/* Succeeded. */
	return 0;
}

/*
 * Opens a context menu of a table at a point of the window's content, and
 * answers it with the item's action, then KL_WINDOW_POPUP_DONE.
 */
int
kl_window_popup_menu(
	struct kl_window *window,
	const struct kl_menu_entry *entries,
	size_t count,
	int x,
	int y)
{
	struct android_menu_item *items;
	struct titlebar_entry *shown;
	size_t kept;
	size_t index;
	int error;

	/* A window and a table that fits. */
	if (window == NULL || entries == NULL || count == 0U)
		return EINVAL;
	if (count > ANDROID_MENU_ITEMS)
		return E2BIG;

	/* The table's copy, the items left to show (hidden ones are left out), and the popup. */
	error = titlebar_menu_copy(entries, count, &items);
	if (error != 0)
		return error;
	error = titlebar_menu_entries(window, items, count, &shown, &kept);
	if (error == 0) {
		error = titlebar_popup(window, ANDROID_POPUP_CONTEXT, shown, kept, x, y + (int)window->bar.height);
		free(shown);
	}

	/* The labels were Java's to read during the call only. */
	for (index = 0U; index < count; index++)
		free(items[index].label);
	free(items);
	return error;
}

/* Gives the height of the strip the window shows in pixels: 0 when it has neither a menu nor tabs. */
uint32_t
android_titlebar_height(
	const struct kl_window *window)
{
	/* The height the last layout settled on. */
	return window->bar.height;
}

/*
 * Makes the strip's picture fit the window: its height by the menu and the
 * tabs, its width by the window's, drawn again.  The caller has set the
 * window's width.
 */
void
android_titlebar_resize(
	struct kl_window *window)
{
	struct android_titlebar *bar;
	uint32_t *pixels;
	size_t words;

	/* The height and the scale now. */
	bar = &window->bar;
	bar->height = android_titlebar_wanted(window);
	bar->dpi = window->app->scale;

	/* No strip: no picture. */
	if (bar->height == 0U) {
		free(bar->pixels);
		bar->pixels = NULL;
		bar->width = 0U;
		return;
	}

	/* A picture of the size (the old one's size may differ in either way). */
	words = (size_t)window->width * (size_t)bar->height;
	pixels = realloc(bar->pixels, words * sizeof(uint32_t));
	if (pixels == NULL) {
		free(bar->pixels);
		bar->pixels = NULL;
		bar->width = 0U;
		bar->height = 0U;
		return;
	}
	bar->pixels = pixels;
	bar->width = window->width;

	/* Laid out and drawn. */
	titlebar_layout(window);
	titlebar_paint(window);
}

/* Draws every window's strip again (the theme changed) and shows the one on top. */
void
android_titlebar_restyle(
	struct kl_app *app)
{
	struct kl_window *window;

	/* Each window. */
	for (window = app->windows; window != NULL; window = window->next) {
		if (window->bar.height != 0U)
			titlebar_paint(window);
	}

	/* The one shown. */
	window = android_window_top(app);
	if (window != NULL)
		android_present_strip(window);
}

/* The window's title changed: the strip shows it when there are no tabs. */
void
android_titlebar_changed(
	struct kl_window *window)
{
	/* Only a strip with a title to show. */
	if (window->bar.height == 0U || window->bar.tab_count != 0U)
		return;
	titlebar_paint(window);
	android_present_strip(window);
}

/* Starts the strip's state of a new window: nothing is held. */
void
android_titlebar_init(
	struct kl_window *window)
{
	/* No part held, no finger. */
	titlebar_press_clear(window);
}

/* Lets everything the strip holds go: the menu, the tabs, the picture, the font and the popup. */
void
android_titlebar_release(
	struct kl_window *window)
{
	struct android_titlebar *bar;

	/* Each part. */
	bar = &window->bar;
	titlebar_menu_free(window);
	free(bar->tabs);
	free(bar->pixels);
	free(bar->popup_items);
	if (bar->text_open)
		kl_text_close(&bar->text);
	memset(bar, 0, sizeof(*bar));
}

/*
 * Turns a key pressed with Ctrl, Alt or Super held into the action of the
 * window's menu item that has it as its shortcut (a KL_WINDOW_ACTION input).
 * Returns 1 when an item had it, 0 when none did.
 */
int
android_titlebar_shortcut(
	struct kl_window *window,
	int32_t keycode,
	int32_t meta)
{
	const struct android_menu_item *item;
	struct kl_window_event input;
	unsigned held;
	uint32_t keysym;
	uint32_t wanted;
	unsigned state;
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

	/* The usable item with the same modifiers and key; the plus is also the equal sign (the key without Shift). */
	for (index = 0U; index < window->bar.menu_count; index++) {
		item = &window->bar.menu[index];
		if (item->keysym == 0U || item->modifiers != held)
			continue;
		wanted = item->keysym;
		if (wanted >= 'A' && wanted <= 'Z')
			wanted += 'a' - 'A';
		if (wanted == TITLEBAR_KEYSYM_PLUS && keysym == TITLEBAR_KEYSYM_EQUAL)
			wanted = TITLEBAR_KEYSYM_EQUAL;
		if (wanted != keysym)
			continue;

		/* An item hidden or disabled is not usable: the key is left to the program. */
		state = titlebar_state(window, item->action);
		if ((state & (KL_ACTION_HIDDEN | KL_ACTION_DISABLED)) != 0U)
			continue;

		/* The item is chosen. */
		memset(&input, 0, sizeof(input));
		input.kind = KL_WINDOW_ACTION;
		input.code = item->action;
		input.id = (int32_t)item->id;
		(void)android_window_queue(window, &input);
		return 1;
	}

	/* No item has it. */
	return 0;
}

/*
 * Takes a finger's or a mouse button's touch of the strip (x and y in the
 * window's pixels, the strip at the top; phase 1 down, 2 moved, 0 up, 3
 * cancelled).  Returns 1 when the strip has it: a press on the strip, and
 * the rest of that press, is the strip's and never the program's.
 */
int
android_titlebar_hit(
	struct kl_window *window,
	double x,
	double y,
	int32_t finger,
	int phase)
{
	struct android_titlebar *bar;
	unsigned part;
	unsigned held;
	int32_t px;
	int32_t py;
	int32_t step;
	int32_t steps;
	int64_t dx;
	int64_t dy;
	int64_t slop;

	/* No strip. */
	bar = &window->bar;
	if (bar->height == 0U)
		return 0;
	px = (int32_t)x;
	py = (int32_t)y;

	/* A press: the strip's when it is on the strip. */
	if (phase == 1) {
		if (py < 0 || (uint32_t)py >= bar->height)
			return 0;
		part = titlebar_part_at(window, px, py);
		bar->pressed = part;
		bar->finger = finger;
		bar->press_us = kl_clock_us();
		bar->long_done = 0;
		bar->press_x = px;
		bar->press_y = py;
		bar->swipe_x = px;
		titlebar_paint(window);
		android_present_strip(window);
		return 1;
	}

	/* Another finger's: the program's. */
	if (bar->finger != finger)
		return 0;

	/* A move far from the press is not a press any more, and along a scrolling strip it moves the tabs (a step for each half of a tab's least width). */
	if (phase == 2) {
		dx = (int64_t)px - (int64_t)bar->press_x;
		dy = (int64_t)py - (int64_t)bar->press_y;
		slop = (int64_t)android_scale(TITLEBAR_SLOP, window->app->scale);
		if (bar->pressed != ANDROID_PART_NONE && dx * dx + dy * dy > slop * slop) {
			bar->pressed = ANDROID_PART_NONE;
			titlebar_paint(window);
			android_present_strip(window);
		}
		step = (int32_t)android_scale(ANDROID_TAB_LEAST, window->app->scale) / 2;
		if (bar->scroll.mode == ANDROID_STRIP_SCROLL && step > 0 && bar->pressed == ANDROID_PART_NONE) {
			steps = (px - bar->swipe_x) / step;
			if (steps != 0) {
				bar->swipe_x += steps * step;
				titlebar_scroll_by(window, -steps);
			}
		}
		return 1;
	}

	/* The release: a press that stayed where it began, and was not a long press, chooses its part. */
	held = bar->pressed;
	titlebar_press_clear(window);
	titlebar_paint(window);
	android_present_strip(window);
	if (phase == 0 && held != ANDROID_PART_NONE && !bar->long_done) {
		part = titlebar_part_at(window, px, py);
		if (part == held)
			titlebar_activate(window, held);
	}
	return 1;
}

/* Tells how long, in milliseconds, until a held tab's long press is due; -1 when none is waiting. */
int
android_titlebar_wait(
	struct kl_app *app)
{
	struct android_titlebar *bar;
	struct kl_window *window;
	uint64_t now;
	uint64_t due;

	/* A tab held on the window shown, whose long press has not come. */
	window = android_window_top(app);
	if (window == NULL)
		return -1;
	bar = &window->bar;
	if (bar->finger == -1 || bar->long_done || bar->pressed < ANDROID_PART_TAB || bar->pressed >= ANDROID_PART_CLOSE)
		return -1;

	/* The time left. */
	now = kl_clock_us();
	due = bar->press_us + (uint64_t)ANDROID_LONG_PRESS_MS * 1000U;
	if (now >= due)
		return 0;
	return (int)((due - now + 999U) / 1000U);
}

/* Opens the menu of a tab held long enough. */
void
android_titlebar_tick(
	struct kl_app *app)
{
	struct kl_window *window;
	unsigned index;

	/* A long press that is due. */
	if (android_titlebar_wait(app) != 0)
		return;
	window = android_window_top(app);
	window->bar.long_done = 1;
	index = window->bar.pressed - ANDROID_PART_TAB;
	if (index >= window->bar.tab_count)
		return;

	/* The press is over (the popup takes the rest of the touch), and the tab's menu is shown. */
	window->bar.pressed = ANDROID_PART_NONE;
	titlebar_paint(window);
	android_present_strip(window);
	titlebar_open_tab_menu(window, index);
}

/* Takes the answer to a popup: the item chosen (its position, -1 for none) is acted on. */
void
android_titlebar_menu_answer(
	struct kl_app *app,
	uint32_t request,
	int32_t item)
{
	struct android_popup_item chosen;
	struct kl_window *window;
	struct kl_window_event input;
	unsigned kind;
	int have;

	/* The window the popup is for. */
	for (window = app->windows; window != NULL; window = window->next) {
		if (window->bar.popup != ANDROID_POPUP_NONE && window->bar.popup_request == request)
			break;
	}
	if (window == NULL)
		return;

	/* What was asked, taken out of the window before anything is queued. */
	kind = window->bar.popup;
	have = (item >= 0 && (size_t)item < window->bar.popup_count);
	memset(&chosen, 0, sizeof(chosen));
	if (have)
		chosen = window->bar.popup_items[item];
	free(window->bar.popup_items);
	window->bar.popup_items = NULL;
	window->bar.popup_count = 0U;
	window->bar.popup = ANDROID_POPUP_NONE;
	window->bar.popup_request = 0U;

	/* The item's doing. */
	if (have && chosen.kind == ANDROID_ITEM_ACTION) {
		memset(&input, 0, sizeof(input));
		input.kind = KL_WINDOW_ACTION;
		input.code = chosen.code;
		input.id = (int32_t)chosen.id;
		(void)android_window_queue(window, &input);
	} else if (have && chosen.kind == ANDROID_ITEM_TAB) {
		titlebar_tab_input(window, chosen.code, chosen.id);
	} else if (have && chosen.kind == ANDROID_ITEM_CHOOSE) {
		titlebar_tab_choose(window, chosen.id);
	}

	/* A context menu's close is told after what it chose. */
	if (kind == ANDROID_POPUP_CONTEXT) {
		memset(&input, 0, sizeof(input));
		input.kind = KL_WINDOW_POPUP_DONE;
		(void)android_window_queue(window, &input);
	}
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

/* The state (KL_ACTION_* bits) of an action: 0 (enabled, unchecked, shown) when the program gave none. */
static unsigned
titlebar_state(
	const struct kl_window *window,
	uint32_t action)
{
	size_t index;

	/* The action's row. */
	for (index = 0U; index < window->bar.action_count; index++) {
		if (window->bar.actions[index].action == action)
			return window->bar.actions[index].state;
	}

	/* None given. */
	return 0U;
}

/* The height the strip wants: its own at the window's scale while there is a menu or a tab, else none. */
uint32_t
android_titlebar_wanted(
	const struct kl_window *window)
{
	/* A menu or tabs make a strip. */
	if (window->bar.menu_count == 0U && window->bar.tab_count == 0U)
		return 0U;
	return android_scale(ANDROID_STRIP_HEIGHT, window->app->scale);
}

/* The strip's text size in pixels. */
static unsigned
titlebar_pixels(
	const struct kl_window *window)
{
	const struct kl_theme *theme;

	/* The body's size, scaled. */
	theme = kl_theme_default();
	return android_scale(theme->text_body, window->app->scale);
}

/* Opens the strip's font, once (the bundled data's fonts; none: the strip draws no text). */
static void
titlebar_font(
	struct kl_window *window)
{
	char directory[512];
	char primary[600];
	char fallback[600];
	int error;

	/* Once, whether it worked or not. */
	if (window->bar.text_tried)
		return;
	window->bar.text_tried = 1;

	/* The bundled data's directory. */
	error = kl_data_directory(directory, sizeof(directory));
	if (error != 0)
		return;

	/* The companions (bold, and monospaced for the signs the sans font lacks), for this font and those opened after it. */
	(void)snprintf(titlebar_companion_bold, sizeof(titlebar_companion_bold), "%s/fonts/keiland-bold.ttf", directory);
	(void)snprintf(titlebar_companion_mono, sizeof(titlebar_companion_mono), "%s/fonts/keiland-fallback-mono.ttf", directory);
	kl_text_companions(titlebar_companion_bold, titlebar_companion_mono);

	/* The sans font and its fallback. */
	(void)snprintf(primary, sizeof(primary), "%s/fonts/keiland.ttf", directory);
	(void)snprintf(fallback, sizeof(fallback), "%s/fonts/keiland-fallback.ttf", directory);
	error = kl_text_open(&window->bar.text, primary, fallback);
	if (error == 0)
		window->bar.text_open = 1;
}

/* Lets the menu's table go, labels included. */
static void
titlebar_menu_free(
	struct kl_window *window)
{
	size_t index;

	/* Each label, then the table. */
	for (index = 0U; index < window->bar.menu_count; index++)
		free(window->bar.menu[index].label);
	free(window->bar.menu);
	window->bar.menu = NULL;
	window->bar.menu_count = 0U;
}

/* The menu or the tabs changed: the window is laid out again when the strip's height changed, else the strip is drawn again and shown. */
static void
titlebar_refresh(
	struct kl_window *window)
{
	/* A strip that comes or goes moves the program's window. */
	if (android_titlebar_wanted(window) != window->bar.height) {
		android_window_layout(window->app);
		return;
	}

	/* Otherwise only the strip. */
	if (window->bar.height == 0U)
		return;
	titlebar_layout(window);
	titlebar_paint(window);
	android_present_strip(window);
}

/*
 * Lays out the menu's button and the tabs in the strip's width, and keeps
 * the rectangles for the touch.  A tab or the tabs' count that changed
 * brings the active tab into sight when the strip scrolls.
 */
static void
titlebar_layout(
	struct kl_window *window)
{
	struct android_tab_place places[ANDROID_TABS];
	struct android_tab_strip strip;
	struct android_tab_scroll scroll;
	struct android_titlebar *bar;
	int closable[ANDROID_TABS];
	uint32_t button;
	uint32_t side;
	uint32_t gap;
	unsigned active;
	unsigned index;
	int32_t left;

	/* The lengths at the window's scale. */
	bar = &window->bar;
	button = android_scale(ANDROID_TAB_HEIGHT, bar->dpi);
	side = android_scale(ANDROID_STRIP_SIDE, bar->dpi);
	gap = android_scale(ANDROID_TAB_GAP, bar->dpi);

	/* The menu's button at the left, in the middle of the strip's height. */
	left = (int32_t)side;
	memset(&bar->menu_place, 0, sizeof(bar->menu_place));
	if (bar->menu_count != 0U) {
		bar->menu_place.x = (int32_t)side;
		bar->menu_place.y = ((int32_t)bar->height - (int32_t)button) / 2;
		bar->menu_place.width = button;
		bar->menu_place.height = button;
		left = bar->menu_place.x + (int32_t)button + (int32_t)gap;
	}

	/* No tabs: nothing more to place. */
	memset(&bar->new_place, 0, sizeof(bar->new_place));
	memset(&bar->scroll, 0, sizeof(bar->scroll));
	if (bar->tab_count == 0U)
		return;

	/* The tabs' room, from the menu's button to the strip's right edge. */
	memset(&strip, 0, sizeof(strip));
	strip.left = left;
	strip.right = (int32_t)bar->width - (int32_t)side;
	strip.height = bar->height;
	strip.dpi = bar->dpi;
	strip.count = bar->tab_count;
	active = titlebar_active(window);
	strip.active = active;
	for (index = 0U; index < bar->tab_count; index++)
		closable[index] = ((bar->tabs[index].flags & KL_TAB_CLOSABLE) != 0U);
	strip.new_button = ((bar->tab_options & KL_TABS_NEW_BUTTON) != 0U);
	strip.first = bar->first;

	/* A strip asked to show its active tab, or that shows another number of tabs, is moved to it. */
	strip.reveal = bar->reveal;
	(void)android_tabs_layout(&strip, closable, places, &bar->new_place, &scroll);
	if (scroll.shown != bar->shown_last) {
		strip.reveal = 1;
		(void)android_tabs_layout(&strip, closable, places, &bar->new_place, &scroll);
	}

	/* The strip as it is now is where the arrows start from, and the active tab is in sight. */
	bar->first = scroll.first;
	bar->shown_last = scroll.shown;
	bar->reveal = 0;
	bar->revealed = 0U;
	if (active < bar->tab_count)
		bar->revealed = bar->tabs[active].id;

	/* The places, kept. */
	for (index = 0U; index < bar->tab_count; index++)
		bar->tabs[index].place = places[index];
	bar->scroll = scroll;
}

/* Draws the strip into its picture: the ground, the menu's button and the tabs (or the title). */
static void
titlebar_paint(
	struct kl_window *window)
{
	const struct kl_theme *theme;
	struct android_titlebar *bar;
	struct kl_canvas canvas;
	struct kl_rect ground;
	unsigned pixels;
	unsigned index;
	int baseline;
	int start;
	int error;

	/* A picture to draw in. */
	bar = &window->bar;
	if (bar->pixels == NULL || bar->height == 0U)
		return;
	error = kl_canvas_init(&canvas, bar->pixels, bar->width, (int)bar->width, (int)bar->height);
	if (error != 0)
		return;
	titlebar_font(window);

	/* The ground. */
	theme = kl_theme_default();
	ground.x = 0;
	ground.y = 0;
	ground.width = (int)bar->width;
	ground.height = (int)bar->height;
	kl_canvas_fill(&canvas, &ground, theme->ground_top);

	/* The menu's button. */
	if (bar->menu_count != 0U)
		titlebar_button_draw(window, &canvas, &bar->menu_place, ANDROID_PART_MENU, KL_ICON_LIST, TITLEBAR_ICON, 1);

	/* The tabs, with the strip's buttons. */
	if (bar->tab_count != 0U) {
		for (index = 0U; index < bar->tab_count; index++)
			titlebar_tab_draw(window, &canvas, index);
		if (bar->new_place.width != 0U)
			titlebar_button_draw(window, &canvas, &bar->new_place, ANDROID_PART_NEW, KL_ICON_PLUS, TITLEBAR_ICON, 1);
		if (bar->scroll.list.width != 0U)
			titlebar_button_draw(window, &canvas, &bar->scroll.list, ANDROID_PART_LIST, KL_ICON_DOWN, TITLEBAR_TAB_ICON, 1);
		kl_canvas_release(&canvas);
		return;
	}

	/* Without tabs, the title after the menu's button. */
	if (window->title != NULL && bar->text_open) {
		pixels = titlebar_pixels(window);
		start = (int)android_scale(TITLEBAR_TAB_INSET, bar->dpi);
		if (bar->menu_count != 0U)
			start += bar->menu_place.x + (int)bar->menu_place.width;
		baseline = kl_text_center(pixels, 0, (int)bar->height);
		(void)kl_text_draw_fit(&bar->text, &canvas, start, baseline, window->title, pixels, 0, (int)bar->width - start, theme->text);
	}
	kl_canvas_release(&canvas);
}

/* Draws a tab: its ground (the panel's when active, the held one's), its attention mark, its title and its close button. */
static void
titlebar_tab_draw(
	struct kl_window *window,
	struct kl_canvas *canvas,
	unsigned index)
{
	const struct android_tab *tab;
	const struct kl_theme *theme;
	struct android_titlebar *bar;
	kl_color ink;
	unsigned pixels;
	int baseline;
	int inset;
	int start;
	int room;
	float radius;
	float mark;

	/* A tab not shown. */
	bar = &window->bar;
	tab = &bar->tabs[index];
	if (!tab->place.shown)
		return;
	theme = kl_theme_default();
	radius = (float)android_scale(TITLEBAR_RADIUS, bar->dpi);

	/* Its ground: the panel's when active, the hover's while held. */
	if ((tab->flags & KL_TAB_ACTIVE) != 0U) {
		kl_canvas_round(canvas, (float)tab->place.tab.x, (float)tab->place.tab.y, (float)tab->place.tab.width,
		    (float)tab->place.tab.height, radius, theme->panel);
	} else if (bar->pressed == ANDROID_PART_TAB + index) {
		kl_canvas_round(canvas, (float)tab->place.tab.x, (float)tab->place.tab.y, (float)tab->place.tab.width,
		    (float)tab->place.tab.height, radius, theme->hover);
	}

	/* The attention's mark before the title, in the accent. */
	inset = (int)android_scale(TITLEBAR_TAB_INSET, bar->dpi);
	start = tab->place.tab.x + inset;
	if ((tab->flags & KL_TAB_ATTENTION) != 0U) {
		mark = (float)android_scale(TITLEBAR_TAB_MARK, bar->dpi);
		kl_canvas_round(canvas, (float)start, (float)tab->place.tab.y + ((float)tab->place.tab.height - mark) / 2.0f, mark, mark,
		    mark / 2.0f, theme->accent);
		start += (int)mark + inset / 2;
	}

	/* The title, up to the close button. */
	room = tab->place.tab.x + (int)tab->place.tab.width - inset - start;
	if (tab->place.close.width != 0U)
		room = tab->place.close.x - inset / 2 - start;
	ink = theme->text_secondary;
	if ((tab->flags & KL_TAB_ACTIVE) != 0U)
		ink = theme->text;
	if (bar->text_open && room > 0) {
		pixels = titlebar_pixels(window);
		baseline = kl_text_center(pixels, tab->place.tab.y, (int)tab->place.tab.height);
		(void)kl_text_draw_fit(&bar->text, canvas, start, baseline, tab->title, pixels, 0, room, ink);
	}

	/* The close button. */
	if (tab->place.close.width != 0U)
		titlebar_button_draw(window, canvas, &tab->place.close, ANDROID_PART_CLOSE + index, KL_ICON_CLOSE, TITLEBAR_TAB_ICON, 1);
}

/* Draws a button of an icon: its ground while held, and the icon (size at 96 dpi) in its middle; a button that does nothing has no ground and a faint icon. */
static void
titlebar_button_draw(
	struct kl_window *window,
	struct kl_canvas *canvas,
	const struct android_rect *place,
	unsigned part,
	enum kl_icon icon,
	uint32_t size,
	int live)
{
	const struct kl_theme *theme;
	kl_color ink;
	float side;

	/* The icon's ink: the text's, or the faint one on a button that does nothing. */
	theme = kl_theme_default();
	ink = theme->text;
	if (!live)
		ink = theme->text_faint;

	/* Its ground while it is held. */
	if (live && window->bar.pressed == part) {
		kl_canvas_round(canvas, (float)place->x, (float)place->y, (float)place->width, (float)place->height,
		    (float)android_scale(TITLEBAR_RADIUS, window->bar.dpi), theme->selection);
	}

	/* The icon in its middle. */
	side = (float)android_scale(size, window->bar.dpi);
	kl_icon_draw(canvas, icon, (float)place->x + ((float)place->width - side) / 2.0f, (float)place->y + ((float)place->height - side) / 2.0f,
	    side, ink);
}

/* What of the menu's button and the tabs is at a point of the strip, or ANDROID_PART_NONE. */
static unsigned
titlebar_part_at(
	const struct kl_window *window,
	int32_t x,
	int32_t y)
{
	struct android_tab_place places[ANDROID_TABS];
	const struct android_titlebar *bar;
	unsigned index;
	unsigned part;

	/* The menu's button, when the window has a menu. */
	bar = &window->bar;
	if (bar->menu_count != 0U && android_rect_contains(&bar->menu_place, x, y))
		return ANDROID_PART_MENU;

	/* No tabs. */
	if (bar->tab_count == 0U)
		return ANDROID_PART_NONE;

	/* The tab or the button there, the strip's arrows and its list's button included. */
	for (index = 0U; index < bar->tab_count; index++)
		places[index] = bar->tabs[index].place;
	index = android_tabs_at(places, bar->tab_count, &bar->new_place, &bar->scroll, x, y, &part);
	if (index == ANDROID_TAB_NONE)
		return ANDROID_PART_NONE;

	/* Which of a tab, its close button, or one of the strip's buttons. */
	switch (part) {
	case ANDROID_TAB_PART_NEW:
		return ANDROID_PART_NEW;
	case ANDROID_TAB_PART_LIST:
		return ANDROID_PART_LIST;
	case ANDROID_TAB_PART_CLOSE:
		return ANDROID_PART_CLOSE + index;
	default:
		break;
	}

	/* The tab itself. */
	return ANDROID_PART_TAB + index;
}

/* The active tab: the first marked so, or tab_count when none is. */
static unsigned
titlebar_active(
	const struct kl_window *window)
{
	unsigned index;

	/* The first tab marked active. */
	for (index = 0U; index < window->bar.tab_count; index++) {
		if ((window->bar.tabs[index].flags & KL_TAB_ACTIVE) != 0U)
			return index;
	}

	/* None is. */
	return window->bar.tab_count;
}

/*
 * Tells what a part held is among new tabs: a button stays, a tab or its
 * close button is the one of the same ID (none when it went), anything else
 * is none.
 */
static unsigned
titlebar_part_kept(
	const struct kl_window *window,
	const struct android_tab *tabs,
	unsigned count,
	unsigned part)
{
	uint32_t id;
	unsigned base;
	unsigned index;

	/* Nothing, or no tab left to be on. */
	if (part == ANDROID_PART_NONE || count == 0U)
		return ANDROID_PART_NONE;

	/* The buttons, whose places do not depend on which tabs there are. */
	if (part >= ANDROID_PART_MENU && part <= ANDROID_PART_LIST)
		return part;

	/* A tab or its close button: the old tab's ID. */
	if (part >= ANDROID_PART_TAB && part < ANDROID_PART_TAB + window->bar.tab_count) {
		base = ANDROID_PART_TAB;
	} else if (part >= ANDROID_PART_CLOSE && part < ANDROID_PART_CLOSE + window->bar.tab_count) {
		base = ANDROID_PART_CLOSE;
	} else {
		return ANDROID_PART_NONE;
	}
	id = window->bar.tabs[part - base].id;

	/* The same part of the new tab of that ID. */
	for (index = 0U; index < count; index++) {
		if (tabs[index].id == id)
			return base + index;
	}

	/* The tab went. */
	return ANDROID_PART_NONE;
}

/* Does what choosing a part asks. */
static void
titlebar_activate(
	struct kl_window *window,
	unsigned part)
{
	struct android_titlebar *bar;
	unsigned index;

	/* By part. */
	bar = &window->bar;
	switch (part) {
	case ANDROID_PART_MENU:
		titlebar_open_menu(window);
		return;
	case ANDROID_PART_NEW:
		titlebar_tab_input(window, KL_WINDOW_TAB_NEW, 0U);
		return;
	case ANDROID_PART_LIST:
		titlebar_open_list(window);
		return;
	default:
		break;
	}

	/* A tab's close button. */
	if (part >= ANDROID_PART_CLOSE && part < ANDROID_PART_CLOSE + bar->tab_count) {
		index = part - ANDROID_PART_CLOSE;
		titlebar_tab_input(window, KL_WINDOW_TAB_CLOSE, bar->tabs[index].id);
		return;
	}

	/* A tab. */
	if (part >= ANDROID_PART_TAB && part < ANDROID_PART_TAB + bar->tab_count) {
		index = part - ANDROID_PART_TAB;
		titlebar_tab_choose(window, bar->tabs[index].id);
	}
}

/* Queues a tab's input (KL_WINDOW_TAB_*, and the tab's ID; 0 for a new one). */
static void
titlebar_tab_input(
	struct kl_window *window,
	unsigned code,
	uint32_t id)
{
	struct kl_window_event input;

	/* The input. */
	memset(&input, 0, sizeof(input));
	input.kind = KL_WINDOW_TAB;
	input.code = code;
	input.id = (int32_t)id;
	(void)android_window_queue(window, &input);
}

/* Chooses a tab by its ID as a tap on it would; the active tab, which the program would not hear of, is brought into sight. */
static void
titlebar_tab_choose(
	struct kl_window *window,
	uint32_t id)
{
	unsigned index;

	/* The tab of the ID; none when it went. */
	for (index = 0U; index < window->bar.tab_count; index++) {
		if (window->bar.tabs[index].id == id)
			break;
	}
	if (index >= window->bar.tab_count)
		return;

	/* The active tab chosen again is brought into sight. */
	if (index == titlebar_active(window)) {
		window->bar.reveal = 1;
		titlebar_layout(window);
		titlebar_paint(window);
		android_present_strip(window);
		return;
	}

	/* Another tab is the program's to make active. */
	titlebar_tab_input(window, KL_WINDOW_TAB_CHOSEN, id);
}

/* Moves a scrolling strip by tabs (after: positive); the layout keeps it within the tabs. */
static void
titlebar_scroll_by(
	struct kl_window *window,
	int steps)
{
	unsigned back;

	/* Toward the tabs before, down to the first. */
	if (steps < 0) {
		back = (unsigned)(-steps);
		if (back > window->bar.first)
			back = window->bar.first;
		window->bar.first -= back;
	}

	/* Toward the tabs after (the layout stops at the last). */
	if (steps > 0)
		window->bar.first += (unsigned)steps;

	/* The strip laid out and drawn again from there. */
	titlebar_layout(window);
	titlebar_paint(window);
	android_present_strip(window);
}

/* Opens the window's menu below its button. */
static void
titlebar_open_menu(
	struct kl_window *window)
{
	struct titlebar_entry *entries;
	size_t count;
	int error;

	/* The items left to show. */
	error = titlebar_menu_entries(window, window->bar.menu, window->bar.menu_count, &entries, &count);
	if (error != 0)
		return;

	/* The popup at the button's bottom left. */
	(void)titlebar_popup(window, ANDROID_POPUP_MENU, entries, count, window->bar.menu_place.x,
	    window->bar.menu_place.y + (int)window->bar.menu_place.height);
	free(entries);
}

/* Opens a tab's menu (close it, make a new one) at the tab. */
static void
titlebar_open_tab_menu(
	struct kl_window *window,
	unsigned index)
{
	struct titlebar_entry entries[2];
	const struct android_tab *tab;

	/* Close, and a new tab. */
	tab = &window->bar.tabs[index];
	memset(entries, 0, sizeof(entries));
	entries[0].label = "Close Tab";
	entries[0].parent = -1;
	entries[0].flags = TITLEBAR_FLAG_ENABLED;
	entries[0].item.kind = ANDROID_ITEM_TAB;
	entries[0].item.code = KL_WINDOW_TAB_CLOSE;
	entries[0].item.id = tab->id;
	entries[1].label = "New Tab";
	entries[1].parent = -1;
	entries[1].flags = TITLEBAR_FLAG_ENABLED;
	entries[1].item.kind = ANDROID_ITEM_TAB;
	entries[1].item.code = KL_WINDOW_TAB_NEW;
	(void)titlebar_popup(window, ANDROID_POPUP_TAB, entries, 2U, tab->place.tab.x, tab->place.tab.y + (int)tab->place.tab.height);
}

/* Opens the list of the tabs out of sight below its button, the active one ticked. */
static void
titlebar_open_list(
	struct kl_window *window)
{
	struct titlebar_entry *entries;
	const struct android_tab *tab;
	unsigned active;
	unsigned index;
	size_t count;

	/* Room for every tab. */
	entries = calloc(window->bar.tab_count, sizeof(*entries));
	if (entries == NULL)
		return;

	/* Each tab out of sight, in the strip's order. */
	active = titlebar_active(window);
	count = 0U;
	for (index = 0U; index < window->bar.tab_count; index++) {
		tab = &window->bar.tabs[index];
		if (tab->place.shown)
			continue;
		entries[count].label = tab->title;
		if (tab->title[0] == '\0')
			entries[count].label = "(untitled)";
		entries[count].parent = -1;
		entries[count].flags = TITLEBAR_FLAG_ENABLED;
		if (index == active)
			entries[count].flags |= TITLEBAR_FLAG_CHECKABLE | TITLEBAR_FLAG_CHECKED;
		entries[count].item.kind = ANDROID_ITEM_CHOOSE;
		entries[count].item.id = tab->id;
		count++;
	}

	/* Nothing out of sight: no list. */
	if (count != 0U) {
		(void)titlebar_popup(window, ANDROID_POPUP_LIST, entries, count, window->bar.scroll.list.x,
		    window->bar.scroll.list.y + (int)window->bar.scroll.list.height);
	}
	free(entries);
}

/*
 * Asks Java to show a popup of entries at a point of the strip's window
 * (in its pixels, the strip at the top), and keeps what each item does for
 * the answer.  Returns 0 or an error number.
 */
static int
titlebar_popup(
	struct kl_window *window,
	unsigned kind,
	const struct titlebar_entry *entries,
	size_t count,
	int x,
	int y)
{
	struct android_popup_item *items;
	const char **labels;
	int32_t *parents;
	int32_t *flags;
	uint32_t request;
	size_t index;
	int error;

	/* Something to show. */
	if (count == 0U)
		return EINVAL;

	/* The arrays Java takes, and what each item does. */
	labels = malloc(count * sizeof(*labels));
	parents = malloc(count * sizeof(*parents));
	flags = malloc(count * sizeof(*flags));
	items = malloc(count * sizeof(*items));
	if (labels == NULL || parents == NULL || flags == NULL || items == NULL) {
		free(labels);
		free(parents);
		free(flags);
		free(items);
		return ENOMEM;
	}
	for (index = 0U; index < count; index++) {
		labels[index] = entries[index].label;
		parents[index] = entries[index].parent;
		flags[index] = entries[index].flags;
		items[index] = entries[index].item;
	}

	/* The popup this answer is for replaces an earlier one. */
	request = titlebar_next_request++;
	if (titlebar_next_request == 0U)
		titlebar_next_request = 1U;
	free(window->bar.popup_items);
	window->bar.popup_items = items;
	window->bar.popup_count = count;
	window->bar.popup = kind;
	window->bar.popup_request = request;

	/* Java wants the place in the native window's pixels. */
	error = keiland_java_popup_menu(request, x + window->app->insets[ANDROID_INSET_LEFT], y + window->app->insets[ANDROID_INSET_TOP], labels,
	    parents, flags, count);
	if (error != 0) {
		free(window->bar.popup_items);
		window->bar.popup_items = NULL;
		window->bar.popup_count = 0U;
		window->bar.popup = ANDROID_POPUP_NONE;
		window->bar.popup_request = 0U;
	}
	free(labels);
	free(parents);
	free(flags);
	return error;
}

/*
 * Makes the popup's entries of a menu's items: the ones not hidden whose
 * parents are shown, in the table's order, each with its parent's position,
 * its type and its state.  Returns 0 and the entries (the caller frees
 * them; the labels are the table's), or ENOMEM.
 */
static int
titlebar_menu_entries(
	const struct kl_window *window,
	const struct android_menu_item *items,
	size_t count,
	struct titlebar_entry **entries,
	size_t *shown)
{
	struct titlebar_entry *made;
	const struct android_menu_item *item;
	size_t *table;
	size_t kept;
	size_t index;
	size_t parent;
	unsigned state;
	int found;

	/* Room for every item, and the table's index of each entry made. */
	made = calloc(count, sizeof(*made));
	table = malloc(count * sizeof(*table));
	if (made == NULL || table == NULL) {
		free(made);
		free(table);
		return ENOMEM;
	}

	/* Each item. */
	kept = 0U;
	for (index = 0U; index < count; index++) {
		item = &items[index];
		state = 0U;
		if (item->action != 0U)
			state = titlebar_state(window, item->action);
		if ((state & KL_ACTION_HIDDEN) != 0U)
			continue;

		/* The parent's position: a top-level item has none; an item whose parent is not shown is left out. */
		made[kept].parent = -1;
		if (item->parent != KL_MENU_ROOT) {
			found = 0;
			for (parent = 0U; parent < kept; parent++) {
				if (items[table[parent]].id == item->parent) {
					made[kept].parent = (int32_t)parent;
					found = 1;
					break;
				}
			}
			if (!found)
				continue;
		}

		/* The label and the flags. */
		made[kept].label = item->label;
		made[kept].flags = 0;
		if (item->type == KL_MENU_ITEM_SEPARATOR) {
			made[kept].flags = TITLEBAR_FLAG_SEPARATOR;
		} else {
			if ((state & KL_ACTION_DISABLED) == 0U)
				made[kept].flags |= TITLEBAR_FLAG_ENABLED;
			if (item->type == KL_MENU_ITEM_CHECKBOX || item->type == KL_MENU_ITEM_RADIO)
				made[kept].flags |= TITLEBAR_FLAG_CHECKABLE;
			if ((state & KL_ACTION_CHECKED) != 0U)
				made[kept].flags |= TITLEBAR_FLAG_CHECKED;
		}

		/* What choosing it does. */
		made[kept].item.kind = ANDROID_ITEM_ACTION;
		made[kept].item.code = item->action;
		made[kept].item.id = item->id;
		table[kept] = index;
		kept++;
	}
	free(table);

	/* Succeeded. */
	*entries = made;
	*shown = kept;
	return 0;
}

/* Copies a menu's table, the labels too (the program's own go away). */
static int
titlebar_menu_copy(
	const struct kl_menu_entry *entries,
	size_t count,
	struct android_menu_item **items)
{
	struct android_menu_item *copy;
	const char *label;
	size_t index;
	size_t made;

	/* Room for the items. */
	copy = calloc(count, sizeof(*copy));
	if (copy == NULL)
		return ENOMEM;

	/* Each item and its label. */
	for (index = 0U; index < count; index++) {
		label = entries[index].label;
		if (label == NULL)
			label = "";
		copy[index].label = strdup(label);
		if (copy[index].label == NULL) {
			for (made = 0U; made < index; made++)
				free(copy[made].label);
			free(copy);
			return ENOMEM;
		}
		copy[index].id = entries[index].id;
		copy[index].parent = entries[index].parent;
		copy[index].type = entries[index].type;
		copy[index].action = entries[index].action;
		copy[index].modifiers = entries[index].modifiers;
		copy[index].keysym = entries[index].keysym;
	}

	/* Succeeded. */
	*items = copy;
	return 0;
}

/* Ends the press that is held. */
static void
titlebar_press_clear(
	struct kl_window *window)
{
	/* No part, no finger. */
	window->bar.pressed = ANDROID_PART_NONE;
	window->bar.finger = -1;
}

/* Copies a title, cut at a character's start to fit the size (NUL ended); a NULL title is empty. */
static void
titlebar_copy_text(
	char *to,
	size_t size,
	const char *from)
{
	size_t length;

	/* Nothing. */
	if (from == NULL) {
		to[0] = '\0';
		return;
	}

	/* The title, cut at a character's start. */
	length = strlen(from);
	if (length > size - 1U) {
		length = size - 1U;
		while (length > 0U && ((unsigned char)from[length] & 0xc0U) == 0x80U)
			length--;
	}
	memcpy(to, from, length);
	to[length] = '\0';
}
