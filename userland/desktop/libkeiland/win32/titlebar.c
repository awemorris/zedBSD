/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The title bar's controls on Windows (kl_window_set_controls and the
 * rest), drawn by libkeiland in the frame's title bar, laid out by the
 * rules of the Keiland desktop's title bar: the navigation (back, forward,
 * up, home, reload) on the left, the breadcrumb (the location's box) after
 * it, the rest on the right before the caption buttons; squares of
 * TITLEBAR_CONTROL, a generic control a pill with its label.  When the
 * room is short, the last secondary control on the right gives way first,
 * then the last normal one; a breadcrumb shown as a box stays, and so do
 * the primary controls.  (The desktop then offers the hidden ones in an
 * overflow menu; Windows has none yet.)
 *
 * Unlike the desktop's, where the compositor moves the window by its
 * title, a Windows title bar is moved by the pixels the hit test calls the
 * caption: the box is lower than the bar (the strip below it is the
 * caption, and the one above it too while the frame is maximized and has
 * no resize band there) and takes the room up to TITLEBAR_CRUMB_MOST, leaving
 * at least TITLEBAR_DRAG_LEAST of bare title bar before the controls on
 * the right (it gives that up only to keep its own least width), so that
 * there is always a part to drag the window by, to double-click to
 * maximize it and to right-click for its system menu.
 *
 * A press on a button chooses it on its release (KL_WINDOW_ACTION); a
 * press on the box edits it in a field of one line: the keys go to the
 * field while it has the keyboard, each change is a
 * KL_WINDOW_CONTROL_TEXT, Enter ends it with KL_TEXT_SUBMITTED and Escape
 * (or a press on the content) with KL_TEXT_CANCELLED (KL_WINDOW_CONTROL_DONE
 * with the text).  The sizes are at 96 dpi and scaled to the frame's.
 *
 * A window with a menu (kl_window_set_menu) has a button for it at the
 * title bar's left: its release opens the menu as a popup of the system's
 * (TrackPopupMenuEx), each submenu a popup of its own, built from the
 * table and the actions' states (a hidden item is left out, a disabled one
 * greyed, a checked one ticked, a radio item with a dot), each item's
 * shortcut at its right.  A choice is a KL_WINDOW_ACTION.  The keys of the
 * shortcuts are matched by the window (window.c, keiui_win32_menu_shortcut).
 * kl_window_popup_menu opens a table the same way at a point of the content.
 *
 * A window with tabs (kl_window_set_tabs) shows them after the menu's
 * button in place of its controls, as the Keiland desktop does: each with
 * its title and, when closable, its close button, the active one on the
 * panel's ground, and the new tab's button after them when asked for
 * (frame.c lays them out).  A press on a tab chooses it; a release on a
 * close button or on the new tab's button asks for that
 * (KL_WINDOW_TAB).  The room left beside them stays the caption.
 */

#include "userland/desktop/libkeiland/win32/window-win32.h"
#include "userland/desktop/libkeiland/win32/frame.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The desktop title bar's sizes (96 dpi). */
#define TITLEBAR_CONTROL	30U
#define TITLEBAR_GAP		4U
#define TITLEBAR_SIDE		10U
#define TITLEBAR_ICON		16U
#define TITLEBAR_PADDING	12U
#define TITLEBAR_CRUMB_LEAST	120U
#define TITLEBAR_CRUMB_INSET	10U
#define TITLEBAR_RADIUS		6U

/* The box's height, its most width, and the least bare title bar it leaves beside it (96 dpi). */
#define TITLEBAR_CRUMB_HEIGHT	24U
#define TITLEBAR_CRUMB_MOST	800U
#define TITLEBAR_DRAG_LEAST	96U

/* How deep the submenus of a menu go at most (a table whose parents loop stops there). */
#define TITLEBAR_MENU_DEPTH	8U

/* The longest text of a menu item for Windows: its label, its '&'s doubled, a tab and the shortcut. */
#define TITLEBAR_MENU_TEXT	(2U * KEIUI_WIN32_MENU_LABEL + 48U)

/* The inset of a tab's title from its left edge, and the attention mark's side (96 dpi). */
#define TITLEBAR_TAB_INSET	10U
#define TITLEBAR_TAB_MARK	6U

/* The icon of the close button of a tab and of the new tab's button (96 dpi). */
#define TITLEBAR_TAB_ICON	12U

/* The sides of a control. */
#define TITLEBAR_LEFT		0U
#define TITLEBAR_CRUMB		1U
#define TITLEBAR_RIGHT		2U

/* The keys a field reads (evdev codes). */
#define TITLEBAR_KEY_ESC	1U
#define TITLEBAR_KEY_BACKSPACE	14U
#define TITLEBAR_KEY_ENTER	28U
#define TITLEBAR_KEY_A		30U
#define TITLEBAR_KEY_X		45U
#define TITLEBAR_KEY_C		46U
#define TITLEBAR_KEY_V		47U
#define TITLEBAR_KEY_KPENTER	96U
#define TITLEBAR_KEY_HOME	102U
#define TITLEBAR_KEY_LEFT	105U
#define TITLEBAR_KEY_RIGHT	106U
#define TITLEBAR_KEY_END	107U
#define TITLEBAR_KEY_DELETE	111U

static unsigned titlebar_side(const struct keiui_win32_control *control);
static unsigned titlebar_state(const struct kl_window *window, uint32_t action);
static int titlebar_copy_menu(const struct kl_menu_entry *entries, size_t count, struct keiui_win32_menu_item **items);
static int titlebar_popup(struct kl_window *window, const struct keiui_win32_menu_item *items, unsigned count, int32_t x, int32_t y);
static HMENU titlebar_menu_build(struct kl_window *window, const struct keiui_win32_menu_item *items, unsigned count, uint32_t parent, unsigned depth);
static int titlebar_menu_item(struct kl_window *window, HMENU menu, const struct keiui_win32_menu_item *items, unsigned count, unsigned index, unsigned depth);
static void titlebar_menu_open(struct kl_window *window);
static void titlebar_tabs_layout(struct kl_window *window, int32_t left, int32_t right, uint32_t height);
static unsigned titlebar_part_at(const struct kl_window *window, int32_t x, int32_t y);
static void titlebar_tab_input(struct kl_window *window, unsigned code, uint32_t id);
static void titlebar_menu_draw(struct kl_window *window, struct kl_canvas *canvas);
static void titlebar_tab_draw(struct kl_window *window, struct kl_canvas *canvas, unsigned index);
static void titlebar_new_draw(struct kl_window *window, struct kl_canvas *canvas);
static void titlebar_icon_button(struct kl_window *window, struct kl_canvas *canvas, const struct keiui_win32_rect *place, unsigned part, enum kl_icon icon, uint32_t size);
static uint32_t titlebar_width(struct kl_window *window, const struct keiui_win32_control *control, uint32_t size);
static unsigned titlebar_pixels(const struct kl_window *window);
static unsigned titlebar_find(const struct kl_window *window, uint32_t id);
static void titlebar_changed(struct kl_window *window);
static void titlebar_button(struct kl_window *window, struct kl_canvas *canvas, unsigned index);
static void titlebar_box(struct kl_window *window, struct kl_canvas *canvas, unsigned index);
static void titlebar_edit(struct kl_window *window, unsigned index);
static void titlebar_cut(struct kl_window *window);
static void titlebar_typed(struct kl_window *window);
static size_t titlebar_previous(const char *text, size_t at);
static size_t titlebar_next(const char *text, size_t at);
static void titlebar_copy_text(char *to, size_t size, const char *from);

/*
 * Declares the window's title bar controls (count 0 takes them away).
 */
int
kl_window_set_controls(
	struct kl_window *window,
	const struct kl_control_entry *entries,
	size_t count)
{
	struct keiui_win32_control *control;
	size_t index;

	/* A window, and controls that fit. */
	if (window == NULL || (count != 0U && entries == NULL))
		return EINVAL;
	if (count > KEIUI_WIN32_CONTROLS)
		return E2BIG;

	/* A field being edited ends with the controls it was one of. */
	if (window->editing)
		keiui_win32_field_end(window, KL_TEXT_CANCELLED);

	/* Each control, its text kept when the same ID stays. */
	for (index = 0U; index < count; index++) {
		control = &window->controls[index];
		if (index >= window->control_count || control->id != entries[index].id) {
			memset(control, 0, sizeof(*control));
		}

		/* Copies the entry into the control. */
		control->id = entries[index].id;
		control->role = entries[index].role;
		control->priority = entries[index].priority;
		control->group = entries[index].group;
		control->action = entries[index].action;
		titlebar_copy_text(control->label, sizeof(control->label), entries[index].label);
	}

	/* Takes the new controls as the window's, with none hovered or pressed. */
	window->control_count = (unsigned)count;
	window->control_hover = KEIUI_WIN32_NO_CONTROL;
	window->control_pressed = KEIUI_WIN32_NO_CONTROL;

	/* Shown from the next painting. */
	titlebar_changed(window);

	/* Succeeded. */
	return 0;
}

/*
 * Sets an action's state (KL_ACTION_* bits) for its controls.
 */
int
kl_window_set_action_state(
	struct kl_window *window,
	uint32_t action,
	unsigned state)
{
	unsigned index;

	/* A window. */
	if (window == NULL)
		return EINVAL;

	/* The action's entry, or a new one. */
	for (index = 0U; index < window->action_count; index++) {
		if (window->actions[index].action == action)
			break;
	}

	/* Adds a slot when the action has none yet. */
	if (index == window->action_count) {
		if (window->action_count == KEIUI_WIN32_ACTIONS)
			return E2BIG;
		window->action_count++;
	}

	/* Stores the action's state. */
	window->actions[index].action = action;
	window->actions[index].state = state;

	/* Shown from the next painting. */
	titlebar_changed(window);

	/* Succeeded. */
	return 0;
}

/*
 * Sets a control's text and placeholder (either NULL to leave it).
 */
int
kl_window_set_control_text(
	struct kl_window *window,
	uint32_t id,
	const char *text,
	const char *placeholder)
{
	unsigned index;

	/* A window and one of its controls. */
	if (window == NULL)
		return EINVAL;
	index = titlebar_find(window, id);
	if (index == KEIUI_WIN32_NO_CONTROL)
		return EINVAL;

	/* The text and the placeholder. */
	if (text != NULL)
		titlebar_copy_text(window->controls[index].text, sizeof(window->controls[index].text), text);
	if (placeholder != NULL)
		titlebar_copy_text(window->controls[index].placeholder, sizeof(window->controls[index].placeholder), placeholder);

	/* Shown from the next painting. */
	titlebar_changed(window);

	/* Succeeded. */
	return 0;
}

/*
 * Takes a breadcrumb's parts: kept for nothing (the box shows the text).
 */
int
kl_window_set_control_parts(
	struct kl_window *window,
	uint32_t id,
	const char *const *parts,
	size_t count)
{
	unsigned index;

	/* The parts and their number are not used by this backend. */
	(void)parts;
	(void)count;

	/* A window and one of its controls. */
	if (window == NULL)
		return EINVAL;
	index = titlebar_find(window, id);
	if (index == KEIUI_WIN32_NO_CONTROL)
		return EINVAL;

	/* Succeeded: the box shows the control's text. */
	return 0;
}

/*
 * Sets a control's value (a breadcrumb's KL_BREADCRUMB_BOXED, with a box's
 * load KL_BREADCRUMB_PROGRESS).
 */
int
kl_window_set_control_value(
	struct kl_window *window,
	uint32_t id,
	unsigned value)
{
	unsigned index;

	/* A window and one of its controls. */
	if (window == NULL)
		return EINVAL;
	index = titlebar_find(window, id);
	if (index == KEIUI_WIN32_NO_CONTROL)
		return EINVAL;

	/* The value. */
	window->controls[index].value = value;
	titlebar_changed(window);

	/* Succeeded. */
	return 0;
}

/*
 * Gives a control the keyboard: a breadcrumb or a search field is edited
 * with its whole text selected.
 */
int
kl_window_focus_control_mode(
	struct kl_window *window,
	uint32_t id,
	unsigned mode)
{
	unsigned index;
	unsigned side;

	/* The mode is not needed: a box or a search field is edited with its whole text selected. */
	(void)mode;

	/* A window and one of its controls that takes text. */
	if (window == NULL)
		return EINVAL;
	index = titlebar_find(window, id);
	if (index == KEIUI_WIN32_NO_CONTROL)
		return EINVAL;
	side = titlebar_side(&window->controls[index]);
	if (side != TITLEBAR_CRUMB && window->controls[index].role != KL_CONTROL_SEARCH)
		return EINVAL;

	/* The field. */
	titlebar_edit(window, index);

	/* Succeeded. */
	return 0;
}

/*
 * Gives a control the keyboard (kl_window_focus_control_mode's edit).
 */
int
kl_window_focus_control(
	struct kl_window *window,
	uint32_t id)
{
	int error;

	/* The edit mode. */
	error = kl_window_focus_control_mode(window, id, KL_FOCUS_EDIT);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Declares the window's menu (count 0 takes it away): shown from its
 * button at the title bar's left.
 */
int
kl_window_set_menu(
	struct kl_window *window,
	const struct kl_menu_entry *entries,
	size_t count)
{
	struct keiui_win32_menu_item *items;
	int error;

	/* A window, and a table that fits. */
	if (window == NULL ||
	    (count != 0U &&
	     entries == NULL))
		return EINVAL;
	if (count > KEIUI_WIN32_MENU_ITEMS)
		return E2BIG;

	/* The table's copy (none for no menu). */
	items = NULL;
	if (count != 0U) {
		error = titlebar_copy_menu(entries, count, &items);
		if (error != 0)
			return error;
	}

	/* It replaces the menu shown, and the button follows it. */
	free(window->menu);
	window->menu = items;
	window->menu_count = (unsigned)count;
	window->control_hover = KEIUI_WIN32_NO_CONTROL;
	window->control_pressed = KEIUI_WIN32_NO_CONTROL;
	titlebar_changed(window);

	/* Succeeded. */
	return 0;
}

/*
 * Opens a menu of a table at a point of the window's content (a context
 * menu), and queues the item chosen (KL_WINDOW_ACTION).  Returns once the
 * menu is closed.
 */
int
kl_window_popup_menu(
	struct kl_window *window,
	const struct kl_menu_entry *entries,
	size_t count,
	int x,
	int y)
{
	struct keiui_win32_menu_item *items;
	POINT point;
	BOOL turned;
	int error;

	/* A window and a table that fits. */
	if (window == NULL ||
	    entries == NULL ||
	    count == 0U)
		return EINVAL;
	if (count > KEIUI_WIN32_MENU_ITEMS)
		return E2BIG;

	/* The table's copy. */
	error = titlebar_copy_menu(entries, count, &items);
	if (error != 0)
		return error;

	/* The point on the screen. */
	point.x = x;
	point.y = y;
	turned = ClientToScreen(window->content, &point);
	if (!turned) {
		free(items);
		return EIO;
	}

	/* The menu, until it closes. */
	error = titlebar_popup(window, items, (unsigned)count, (int32_t)point.x, (int32_t)point.y);
	free(items);
	if (error != 0)
		return error;

	/* Succeeded: what was chosen is queued. */
	return 0;
}

/*
 * Declares the window's tabs (count 0 takes them away, and the controls
 * come back), with KL_TABS_* options.
 */
int
kl_window_set_tabs(
	struct kl_window *window,
	const struct kl_tab_entry *tabs,
	size_t count,
	unsigned options)
{
	struct keiui_win32_tab *kept;
	size_t index;

	/* A window, and tabs that fit, each with an ID. */
	if (window == NULL ||
	    (count != 0U &&
	     tabs == NULL))
		return EINVAL;
	if (count > KEIUI_WIN32_TABS)
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

	/* Each tab's ID, title and flags. */
	for (index = 0U; index < count; index++) {
		kept[index].id = tabs[index].id;
		titlebar_copy_text(kept[index].title, sizeof(kept[index].title), tabs[index].title);
		kept[index].flags = tabs[index].flags;
	}

	/* A field being edited ends with the controls the tabs replace. */
	if (count != 0U && window->editing)
		keiui_win32_field_end(window, KL_TEXT_CANCELLED);

	/* They replace the tabs shown. */
	free(window->tabs);
	window->tabs = kept;
	window->tab_count = (unsigned)count;
	window->tab_options = options;
	window->control_hover = KEIUI_WIN32_NO_CONTROL;
	window->control_pressed = KEIUI_WIN32_NO_CONTROL;
	titlebar_changed(window);

	/* Succeeded. */
	return 0;
}

/*
 * Chooses the menu's item whose shortcut a key press is (a keysym and
 * KL_MENU_* modifiers): its KL_WINDOW_ACTION is queued.  Returns 1 when an
 * item took the key, 0 when none did.
 */
int
keiui_win32_menu_shortcut(
	struct kl_window *window,
	uint32_t keysym,
	unsigned menu)
{
	struct kl_window_event *input;
	unsigned index;

	/* The usable item of the shortcut, if any. */
	index = keiui_win32_shortcut_find(window->menu, window->menu_count, window->actions, window->action_count, keysym, menu);
	if (index == KEIUI_WIN32_NO_ITEM)
		return 0;

	/* Its choice; a full queue loses it, but the key is still the item's. */
	input = keiui_win32_push(window, KL_WINDOW_ACTION);
	if (input != NULL) {
		input->code = window->menu[index].action;
		input->id = (int32_t)window->menu[index].id;
		input->begin = 0;
	}

	/* Succeeded: the key was the menu's. */
	return 1;
}

/*
 * Frees the window's menu and tabs (its closing).
 */
void
keiui_win32_titlebar_free(
	struct kl_window *window)
{
	/* The menu's table. */
	free(window->menu);
	window->menu = NULL;
	window->menu_count = 0U;

	/* The tabs. */
	free(window->tabs);
	window->tabs = NULL;
	window->tab_count = 0U;
}

/*
 * Lays the controls out in a title bar of a width and a height (the
 * frame's pixels), and keeps their rectangles for the hit test.
 */
void
keiui_win32_titlebar_layout(
	struct kl_window *window,
	uint32_t width,
	uint32_t height)
{
	struct keiui_win32_frame frame;
	struct keiui_win32_rect buttons[3];
	struct keiui_win32_control *control;
	uint32_t size;
	uint32_t gap;
	uint32_t room;
	uint32_t most;
	uint32_t least;
	uint32_t drag;
	int32_t margin;
	int32_t left;
	int32_t right;
	int32_t needed;
	int32_t x;
	int32_t y;
	unsigned index;
	unsigned victim;
	unsigned side;
	unsigned crumb;
	unsigned pass;
	unsigned control_side;
	unsigned control_state;

	/* The room between the left edge and the caption buttons. */
	memset(&frame, 0, sizeof(frame));
	frame.width = width;
	frame.height = height;
	frame.dpi = window->dpi;
	frame.maximized = window->maximized;
	frame.fullscreen = window->fullscreen;
	frame.caption_height = KEIUI_WIN32_CAPTION;
	keiui_win32_frame_buttons(&frame, buttons);
	size = keiui_win32_scale(TITLEBAR_CONTROL, window->dpi);
	gap = keiui_win32_scale(TITLEBAR_GAP, window->dpi);
	margin = (int32_t)keiui_win32_scale(TITLEBAR_SIDE, window->dpi);
	left = margin;
	right = buttons[KEIUI_WIN32_BUTTON_MINIMIZE_INDEX].x - margin;
	y = ((int32_t)height - (int32_t)size) / 2;

	/* The menu's button first at the left, a square like a control's. */
	memset(&window->menu_place, 0, sizeof(window->menu_place));
	if (window->menu_count != 0U) {
		window->menu_place.x = left;
		window->menu_place.y = y;
		window->menu_place.width = size;
		window->menu_place.height = size;
		left += (int32_t)(size + gap);
	}

	/* Tabs take the controls' place, up to the bare title bar left to drag the window by. */
	if (window->tab_count != 0U) {
		for (index = 0U; index < window->control_count; index++)
			window->controls[index].shown = 0;
		titlebar_tabs_layout(window, left, right, height);
		return;
	}

	/* Every control not hidden is shown at first, with its width. */
	crumb = KEIUI_WIN32_NO_CONTROL;
	for (index = 0U; index < window->control_count; index++) {
		control = &window->controls[index];
		control->shown = 1;
		control_state = titlebar_state(window, control->action);
		if (control_state & KL_ACTION_HIDDEN)
			control->shown = 0;
		control->width = titlebar_width(window, control, size);
		control->height = size;
		control->y = y;
		control_side = titlebar_side(control);
		if (control_side == TITLEBAR_CRUMB) {
			control->height = keiui_win32_scale(TITLEBAR_CRUMB_HEIGHT, window->dpi);
			control->y = ((int32_t)height - (int32_t)control->height) / 2;
		}

		/* Remembers the first box, the one that takes the room the others leave. */
		if (control_side == TITLEBAR_CRUMB && crumb == KEIUI_WIN32_NO_CONTROL)
			crumb = index;
	}

	/* The controls give way until they fit: the last secondary one on the right, then the last normal one. */
	for (pass = 0U; pass <= window->control_count; pass++) {
		needed = 0;
		for (index = 0U; index < window->control_count; index++) {
			control = &window->controls[index];
			if (!control->shown)
				continue;
			needed += (int32_t)(control->width + gap);
			if (index == crumb)
				needed += 2 * margin;
		}

		/* Stops when the controls fit. */
		if (needed <= right - left)
			break;
		victim = KEIUI_WIN32_NO_CONTROL;
		for (index = window->control_count; index > 0U && victim == KEIUI_WIN32_NO_CONTROL; index--) {
			control = &window->controls[index - 1U];
			control_side = titlebar_side(control);
			if (control->shown && control_side == TITLEBAR_RIGHT && control->priority == KL_PRIORITY_SECONDARY)
				victim = index - 1U;
		}

		/* Then looks for the last normal control on the right. */
		for (index = window->control_count; index > 0U && victim == KEIUI_WIN32_NO_CONTROL; index--) {
			control = &window->controls[index - 1U];
			control_side = titlebar_side(control);
			if (control->shown && control_side == TITLEBAR_RIGHT && control->priority == KL_PRIORITY_NORMAL)
				victim = index - 1U;
		}

		/* Stops when no control is left to hide. */
		if (victim == KEIUI_WIN32_NO_CONTROL)
			break;
		window->controls[victim].shown = 0;
	}

	/* The left side from the left edge. */
	x = left;
	for (index = 0U; index < window->control_count; index++) {
		control = &window->controls[index];
		control_side = titlebar_side(control);
		if (!control->shown || control_side != TITLEBAR_LEFT)
			continue;
		control->x = x;
		x += (int32_t)(control->width + gap);
	}

	/* Moves the left edge past the controls of the left side. */
	if (x != left)
		left = x - (int32_t)gap + (int32_t)keiui_win32_scale(TITLEBAR_SIDE, window->dpi);

	/* The right side, in its order, ending at the caption buttons. */
	needed = 0;
	for (index = 0U; index < window->control_count; index++) {
		control = &window->controls[index];
		control_side = titlebar_side(control);
		if (control->shown && control_side == TITLEBAR_RIGHT)
			needed += (int32_t)(control->width + gap);
	}

	/* Places the controls of the right side, ending at the caption buttons. */
	x = right - needed + (int32_t)gap;
	for (index = 0U; index < window->control_count; index++) {
		control = &window->controls[index];
		control_side = titlebar_side(control);
		if (!control->shown || control_side != TITLEBAR_RIGHT)
			continue;
		control->x = x;
		x += (int32_t)(control->width + gap);
	}

	/* Moves the right edge in front of the controls of the right side. */
	if (needed != 0)
		right = right - needed + (int32_t)gap - (int32_t)keiui_win32_scale(TITLEBAR_SIDE, window->dpi);

	/*
	 * The breadcrumb takes the room between, up to its most, less the
	 * bare title bar left to drag the window by (given up only down to
	 * the box's least).
	 */
	if (crumb != KEIUI_WIN32_NO_CONTROL && window->controls[crumb].shown) {
		control = &window->controls[crumb];
		control->x = left;
		room = 0U;
		if (right - left > 0)
			room = (uint32_t)(right - left);
		most = keiui_win32_scale(TITLEBAR_CRUMB_MOST, window->dpi);
		least = keiui_win32_scale(TITLEBAR_CRUMB_LEAST, window->dpi);
		drag = keiui_win32_scale(TITLEBAR_DRAG_LEAST, window->dpi);
		side = 0U;
		if (room > drag)
			side = room - drag;
		if (side < least)
			side = least;
		if (side > room)
			side = room;
		if (side > most)
			side = most;
		control->width = side;
		if (control->width == 0U)
			control->shown = 0;
	}

	/* The menu's button and the shown controls' rectangles, for the hit test. */
	window->control_place_count = 0U;
	if (window->menu_count != 0U) {
		window->control_places[0] = window->menu_place;
		window->control_place_count = 1U;
	}
	for (index = 0U; index < window->control_count; index++) {
		control = &window->controls[index];
		if (!control->shown)
			continue;
		window->control_places[window->control_place_count].x = control->x;
		window->control_places[window->control_place_count].y = control->y;
		window->control_places[window->control_place_count].width = control->width;
		window->control_places[window->control_place_count].height = control->height;
		window->control_place_count++;
	}
}

/*
 * Draws the controls (or, without any, the title) into the title bar.
 */
void
keiui_win32_titlebar_draw(
	struct kl_window *window,
	struct kl_canvas *canvas,
	uint32_t width,
	uint32_t height)
{
	const struct kl_theme *theme;
	unsigned pixels;
	unsigned index;
	unsigned shown;
	unsigned side;
	int baseline;
	int start;

	/* The layout of now. */
	keiui_win32_titlebar_layout(window, width, height);
	theme = kl_theme_default();
	pixels = titlebar_pixels(window);

	/* The menu's button. */
	if (window->menu_count != 0U)
		titlebar_menu_draw(window, canvas);

	/* The tabs in place of the controls and the title. */
	if (window->tab_count != 0U) {
		for (index = 0U; index < window->tab_count; index++)
			titlebar_tab_draw(window, canvas, index);
		titlebar_new_draw(window, canvas);
		return;
	}

	/* Each control shown. */
	shown = 0U;
	for (index = 0U; index < window->control_count; index++) {
		if (!window->controls[index].shown)
			continue;
		shown++;
		side = titlebar_side(&window->controls[index]);
		if (side == TITLEBAR_CRUMB)
			titlebar_box(window, canvas, index);
		else
			titlebar_button(window, canvas, index);
	}

	/* Without controls, the title, after the menu's button when there is one. */
	if (shown != 0U || window->title == NULL || !window->text_open)
		return;
	start = (int)keiui_win32_scale(TITLEBAR_CRUMB_INSET, window->dpi);
	if (window->menu_count != 0U)
		start += window->menu_place.x + (int)window->menu_place.width;
	baseline = kl_text_center(pixels, 0, (int)height);
	(void)kl_text_draw_fit(&window->text, canvas, start, baseline, window->title, pixels, 0, (int)width / 2, theme->text);
}

/* The control at a point of the frame's client (an index), or KEIUI_WIN32_NO_CONTROL. */
unsigned
keiui_win32_titlebar_at(
	const struct kl_window *window,
	int32_t x,
	int32_t y)
{
	const struct keiui_win32_control *control;
	unsigned index;
	unsigned part;

	/* The menu's button and the tabs. */
	part = titlebar_part_at(window, x, y);
	if (part != KEIUI_WIN32_NO_CONTROL)
		return part;

	/* Each control shown. */
	for (index = 0U; index < window->control_count; index++) {
		control = &window->controls[index];
		if (!control->shown)
			continue;
		if (x >= control->x && x < control->x + (int32_t)control->width && y >= control->y && y < control->y + (int32_t)control->height)
			return index;
	}

	/* None. */
	return KEIUI_WIN32_NO_CONTROL;
}

/* A press on a control: a button is held, the box is edited. */
void
keiui_win32_titlebar_press(
	struct kl_window *window,
	unsigned index)
{
	struct keiui_win32_control *control;
	unsigned side;
	unsigned state;

	/* A tab is chosen when it is pressed. */
	if (index >= KEIUI_WIN32_TAB_BODY && index < KEIUI_WIN32_TAB_BODY + window->tab_count) {
		titlebar_tab_input(window, KL_WINDOW_TAB_CHOSEN, window->tabs[index - KEIUI_WIN32_TAB_BODY].id);
		return;
	}

	/* The menu's button and the tabs' buttons are held until their release. */
	if (index == KEIUI_WIN32_MENU_BUTTON ||
	    index == KEIUI_WIN32_TAB_NEW_BUTTON ||
	    (index >= KEIUI_WIN32_TAB_CLOSE_BUTTON &&
	     index < KEIUI_WIN32_TAB_CLOSE_BUTTON + window->tab_count)) {
		window->control_pressed = index;
		(void)InvalidateRect(window->frame, NULL, FALSE);
		return;
	}

	/* Nowhere, or a control that is disabled. */
	if (index >= window->control_count)
		return;
	control = &window->controls[index];
	state = titlebar_state(window, control->action);
	if (state & KL_ACTION_DISABLED)
		return;

	/* The box: its field. */
	side = titlebar_side(control);
	if (side == TITLEBAR_CRUMB) {
		if (!window->editing || window->field.control != index)
			titlebar_edit(window, index);
		return;
	}

	/* A button, held. */
	window->control_pressed = index;
	(void)InvalidateRect(window->frame, NULL, FALSE);
}

/* A release: on the button pressed, its choice. */
void
keiui_win32_titlebar_release(
	struct kl_window *window,
	unsigned index)
{
	struct kl_window_event *input;
	unsigned pressed;

	/* The button pressed is let go. */
	pressed = window->control_pressed;
	window->control_pressed = KEIUI_WIN32_NO_CONTROL;
	(void)InvalidateRect(window->frame, NULL, FALSE);
	if (pressed == KEIUI_WIN32_NO_CONTROL || pressed != index)
		return;

	/* The menu's button opens the menu below it. */
	if (index == KEIUI_WIN32_MENU_BUTTON) {
		titlebar_menu_open(window);
		return;
	}

	/* The new tab's button asks for a tab. */
	if (index == KEIUI_WIN32_TAB_NEW_BUTTON) {
		titlebar_tab_input(window, KL_WINDOW_TAB_NEW, 0U);
		return;
	}

	/* A tab's close button asks for its closing. */
	if (index >= KEIUI_WIN32_TAB_CLOSE_BUTTON && index < KEIUI_WIN32_TAB_CLOSE_BUTTON + window->tab_count) {
		titlebar_tab_input(window, KL_WINDOW_TAB_CLOSE, window->tabs[index - KEIUI_WIN32_TAB_CLOSE_BUTTON].id);
		return;
	}

	/* A control's choice, while it is still one. */
	if (index >= window->control_count)
		return;
	input = keiui_win32_push(window, KL_WINDOW_ACTION);
	if (input == NULL)
		return;
	input->code = window->controls[index].action;
	input->id = (int32_t)window->controls[index].id;
	input->begin = 0;
}

/* The pointer left the title bar: nothing is under it or held. */
void
keiui_win32_titlebar_release_all(
	struct kl_window *window)
{
	/* Nothing hovered or held. */
	window->control_hover = KEIUI_WIN32_NO_CONTROL;
	window->control_pressed = KEIUI_WIN32_NO_CONTROL;
	(void)InvalidateRect(window->frame, NULL, FALSE);
}

/*
 * A key pressed while the field has the keyboard (code an evdev code,
 * text what it types).  Returns 1: the field takes every key.
 */
int
keiui_win32_field_key(
	struct kl_window *window,
	uint32_t code,
	unsigned modifiers,
	const char *text)
{
	struct keiui_win32_field *field;
	char pasted[KEIUI_WIN32_CONTROL_TEXT];
	size_t length;
	size_t low;
	size_t high;
	size_t index;
	int shift;
	int control;

	/* The field. */
	field = &window->field;
	length = strlen(field->text);
	shift = (modifiers & KL_MOD_SHIFT) != 0U;
	control = (modifiers & KL_MOD_CTRL) != 0U;
	low = field->caret;
	high = field->anchor;
	if (field->anchor < field->caret) {
		low = field->anchor;
		high = field->caret;
	}

	/* What the key does. */
	switch (code) {
	case TITLEBAR_KEY_ENTER:
	case TITLEBAR_KEY_KPENTER:
		keiui_win32_field_end(window, KL_TEXT_SUBMITTED);
		return 1;
	case TITLEBAR_KEY_ESC:
		keiui_win32_field_end(window, KL_TEXT_CANCELLED);
		return 1;
	case TITLEBAR_KEY_BACKSPACE:
		if (low == high && low != 0U)
			low = titlebar_previous(field->text, low);
		memmove(field->text + low, field->text + high, length - high + 1U);
		field->caret = low;
		field->anchor = low;
		titlebar_typed(window);
		return 1;
	case TITLEBAR_KEY_DELETE:
		if (low == high && high < length)
			high = titlebar_next(field->text, high);
		memmove(field->text + low, field->text + high, length - high + 1U);
		field->caret = low;
		field->anchor = low;
		titlebar_typed(window);
		return 1;
	case TITLEBAR_KEY_LEFT:
	case TITLEBAR_KEY_RIGHT:
	case TITLEBAR_KEY_HOME:
	case TITLEBAR_KEY_END:
		/* The caret moves, the selection with Shift. */
		index = field->caret;
		if (code == TITLEBAR_KEY_LEFT) {
			if (!shift && low != high)
				index = low;
			else
				index = titlebar_previous(field->text, field->caret);
		}

		/* Moves the caret one character right, or to the end of a selection. */
		if (code == TITLEBAR_KEY_RIGHT) {
			if (!shift && low != high)
				index = high;
			else
				index = titlebar_next(field->text, field->caret);
		}

		/* Moves the caret to the start of the text. */
		if (code == TITLEBAR_KEY_HOME)
			index = 0U;
		if (code == TITLEBAR_KEY_END)
			index = length;
		field->caret = index;
		if (!shift)
			field->anchor = index;
		(void)InvalidateRect(window->frame, NULL, FALSE);
		keiui_win32_ime_place(window);
		return 1;
	default:
		break;
	}

	/* The shortcuts with Control: all, copy, cut, paste. */
	if (control) {
		if (code == TITLEBAR_KEY_A) {
			field->anchor = 0U;
			field->caret = length;
			(void)InvalidateRect(window->frame, NULL, FALSE);
		} else if (code == TITLEBAR_KEY_C || code == TITLEBAR_KEY_X) {
			if (low != high)
				(void)keiui_win32_clipboard_put(window, field->text + low, high - low);
			if (code == TITLEBAR_KEY_X && low != high)
				titlebar_cut(window);
		} else if (code == TITLEBAR_KEY_V) {
			(void)keiui_win32_clipboard_get(window, pasted, sizeof(pasted));
			for (index = 0U; pasted[index] != '\0'; index++) {
				if (pasted[index] == '\r' || pasted[index] == '\n' || pasted[index] == '\t')
					pasted[index] = ' ';
			}

			/* Puts the pasted text in the field. */
			keiui_win32_field_insert(window, pasted);
		}

		/* Takes the key as handled. */
		return 1;
	}

	/* A character typed. */
	if (text != NULL && text[0] != '\0' && (modifiers & KL_MOD_ALT) == 0U)
		keiui_win32_field_insert(window, text);
	return 1;
}

/* Puts text in the field in place of its selection. */
void
keiui_win32_field_insert(
	struct kl_window *window,
	const char *text)
{
	struct keiui_win32_field *field;
	size_t length;
	size_t adding;
	size_t low;
	size_t high;

	/* The selection goes. */
	field = &window->field;
	low = field->caret;
	high = field->anchor;
	if (field->anchor < field->caret) {
		low = field->anchor;
		high = field->caret;
	}

	/* Measures the text left after the selection goes. */
	length = strlen(field->text);
	memmove(field->text + low, field->text + high, length - high + 1U);
	length -= high - low;

	/* The text, as much as fits in whole characters. */
	adding = strlen(text);
	while (adding != 0U && length + adding + 1U > sizeof(field->text))
		adding = titlebar_previous(text, adding);
	memmove(field->text + low + adding, field->text + low, length - low + 1U);
	memcpy(field->text + low, text, adding);
	field->caret = low + adding;
	field->anchor = field->caret;

	/* Told and drawn. */
	titlebar_typed(window);
}

/* The input method's text being composed at the caret (empty when it ends). */
void
keiui_win32_field_compose(
	struct kl_window *window,
	const char *text,
	size_t caret)
{
	/* Kept to draw. */
	titlebar_copy_text(window->field.composing, sizeof(window->field.composing), text);
	window->field.composing_caret = caret;
	(void)InvalidateRect(window->frame, NULL, FALSE);
}

/* Ends the field's editing, and how (KL_TEXT_SUBMITTED, KL_TEXT_CANCELLED). */
void
keiui_win32_field_end(
	struct kl_window *window,
	unsigned how)
{
	struct kl_window_event *input;
	unsigned index;

	/* No field. */
	if (!window->editing)
		return;
	index = window->field.control;
	window->editing = 0;
	window->field.composing[0] = '\0';

	/* The input method as the program asked for it. */
	keiui_win32_ime_place(window);
	(void)InvalidateRect(window->frame, NULL, FALSE);

	/* The end, with the text. */
	if (index >= window->control_count)
		return;
	input = keiui_win32_push(window, KL_WINDOW_CONTROL_DONE);
	if (input == NULL)
		return;
	input->id = (int32_t)window->controls[index].id;
	input->code = how;
	titlebar_copy_text(input->text, sizeof(input->text), window->field.text);
}

/* The side a control goes on: the navigation and the reload left, the breadcrumb between, the rest right. */
static unsigned
titlebar_side(
	const struct keiui_win32_control *control)
{
	/* By its role. */
	switch (control->role) {
	case KL_CONTROL_BACK:
	case KL_CONTROL_FORWARD:
	case KL_CONTROL_UP:
	case KL_CONTROL_HOME:
	case KL_CONTROL_RELOAD:
		return TITLEBAR_LEFT;
	case KL_CONTROL_BREADCRUMB:
		return TITLEBAR_CRUMB;
	default:
		break;
	}

	/* The right. */
	return TITLEBAR_RIGHT;
}

/* An action's state (0 for an action never told). */
static unsigned
titlebar_state(
	const struct kl_window *window,
	uint32_t action)
{
	unsigned state;

	/* The window's entry of it. */
	state = keiui_win32_action_state(window->actions, window->action_count, action);
	return state;
}

/* A control's width: a generic one's pill fits its label, the breadcrumb its least, the rest a square. */
static uint32_t
titlebar_width(
	struct kl_window *window,
	const struct keiui_win32_control *control,
	uint32_t size)
{
	int label;
	unsigned side;
	uint32_t result;
	size_t label_length;
	unsigned label_pixels;

	/* The breadcrumb's least. */
	side = titlebar_side(control);
	if (side == TITLEBAR_CRUMB) {
		result = keiui_win32_scale(TITLEBAR_CRUMB_LEAST, window->dpi);
		return result;
	}

	/* A generic control's pill. */
	if (control->role == KL_CONTROL_GENERIC) {
		label = (int)(strlen(control->label) * titlebar_pixels(window) / 2U);
		if (window->text_open) {
			label_length = strlen(control->label);
			label_pixels = titlebar_pixels(window);
			label = kl_text_width(&window->text, control->label, label_length, label_pixels, 0);
		}
		return (uint32_t)label + 2U * keiui_win32_scale(TITLEBAR_PADDING, window->dpi);
	}

	/* A square. */
	return size;
}

/* The title bar's text size, scaled to the dpi. */
static unsigned
titlebar_pixels(
	const struct kl_window *window)
{
	const struct kl_theme *theme;
	unsigned result;

	/* The body's size, scaled. */
	theme = kl_theme_default();
	result = keiui_win32_scale(theme->text_body, window->dpi);
	return result;
}

/* The index of the control of an ID, or KEIUI_WIN32_NO_CONTROL. */
static unsigned
titlebar_find(
	const struct kl_window *window,
	uint32_t id)
{
	unsigned index;

	/* Each control. */
	for (index = 0U; index < window->control_count; index++) {
		if (window->controls[index].id == id)
			return index;
	}

	/* None. */
	return KEIUI_WIN32_NO_CONTROL;
}

/* The title bar is painted again, its controls in the hit test from now. */
static void
titlebar_changed(
	struct kl_window *window)
{
	RECT client;

	/* The layout at the frame's size now, and the painting. */
	(void)GetClientRect(window->frame, &client);
	keiui_win32_titlebar_layout(window, (uint32_t)(client.right - client.left), keiui_win32_caption_pixels(window));
	(void)InvalidateRect(window->frame, NULL, FALSE);
}

/* Draws a button: its ground under the pointer or held, and its icon or label (faded when disabled). */
static void
titlebar_button(
	struct kl_window *window,
	struct kl_canvas *canvas,
	unsigned index)
{
	const struct keiui_win32_control *control;
	const struct kl_theme *theme;
	enum kl_icon icon;
	kl_color ink;
	unsigned pixels;
	unsigned state;
	float side;
	kl_color fill;
	int has_icon;
	int baseline;
	int width;
	size_t label_length;

	/* Its look. */
	control = &window->controls[index];
	theme = kl_theme_default();
	state = titlebar_state(window, control->action);
	ink = theme->text;
	if (state & KL_ACTION_DISABLED)
		ink = theme->text_faint;
	if (!(state & KL_ACTION_DISABLED) && (window->control_pressed == index || window->control_hover == index)) {
		fill = theme->hover;
		if (window->control_pressed == index)
			fill = theme->selection;
		kl_canvas_round(canvas, (float)control->x, (float)control->y, (float)control->width, (float)control->height,
		    (float)keiui_win32_scale(TITLEBAR_RADIUS, window->dpi), fill);
	}

	/* Its icon. */
	has_icon = 1;
	icon = KL_ICON_BACK;
	switch (control->role) {
	case KL_CONTROL_BACK:
		icon = KL_ICON_BACK;
		break;
	case KL_CONTROL_FORWARD:
		icon = KL_ICON_FORWARD;
		break;
	case KL_CONTROL_HOME:
		icon = KL_ICON_HOME;
		break;
	case KL_CONTROL_UP:
		icon = KL_ICON_UP;
		break;
	case KL_CONTROL_RELOAD:
		icon = KL_ICON_REFRESH;
		break;
	case KL_CONTROL_SEARCH:
		icon = KL_ICON_SEARCH;
		break;
	default:
		has_icon = 0;
		break;
	}

	/* Draws the icon in the middle of the button. */
	if (has_icon) {
		side = (float)keiui_win32_scale(TITLEBAR_ICON, window->dpi);
		kl_icon_draw(canvas, icon, (float)control->x + ((float)control->width - side) / 2.0f,
		    (float)control->y + ((float)control->height - side) / 2.0f, side, ink);
		return;
	}

	/* Or its label. */
	if (!window->text_open)
		return;
	pixels = titlebar_pixels(window);
	baseline = kl_text_center(pixels, control->y, (int)control->height);
	label_length = strlen(control->label);
	width = kl_text_width(&window->text, control->label, label_length, pixels, 0);
	(void)kl_text_draw(&window->text, canvas, control->x + ((int)control->width - width) / 2, baseline, control->label,
	    strlen(control->label), pixels, 0, ink);
}

/* Draws the box: its text, or the field being edited with its selection, caret and composing text. */
static void
titlebar_box(
	struct kl_window *window,
	struct kl_canvas *canvas,
	unsigned index)
{
	const struct keiui_win32_control *control;
	const struct kl_theme *theme;
	struct keiui_win32_field *field;
	struct kl_rect clip;
	struct kl_rect mark;
	unsigned pixels;
	unsigned share;
	kl_color edge;
	size_t low;
	size_t high;
	int inset;
	int baseline;
	int shift;
	int caret;
	int from;
	int to;
	int composing;
	size_t composing_length;
	size_t text_length;

	/* The box. */
	control = &window->controls[index];
	theme = kl_theme_default();
	pixels = titlebar_pixels(window);
	kl_canvas_round(canvas, (float)control->x, (float)control->y, (float)control->width, (float)control->height,
	    (float)keiui_win32_scale(TITLEBAR_RADIUS, window->dpi), theme->control);

	/* A load under way fills it from the left in the selection's colour (KL_BREADCRUMB_PROGRESS). */
	share = (control->value & KL_BREADCRUMB_PROGRESS_MASK) >> KL_BREADCRUMB_PROGRESS_SHIFT;
	if (share > 1U) {
		clip.x = control->x;
		clip.y = control->y;
		clip.width = (int)((unsigned long)control->width * (share - 1U) / 1000U);
		clip.height = (int)control->height;
		kl_canvas_clip_push(canvas, &clip);
		kl_canvas_round(canvas, (float)control->x, (float)control->y, (float)control->width, (float)control->height,
		    (float)keiui_win32_scale(TITLEBAR_RADIUS, window->dpi), theme->selection);
		kl_canvas_clip_pop(canvas);
	}

	/* Draws the border, in the accent colour while the field is edited. */
	edge = theme->control_edge;
	if (window->editing && window->field.control == index)
		edge = theme->accent;
	kl_canvas_round_border(canvas, (float)control->x, (float)control->y, (float)control->width, (float)control->height,
	    (float)keiui_win32_scale(TITLEBAR_RADIUS, window->dpi), (float)window->dpi / 96.0f, edge);
	if (!window->text_open)
		return;
	inset = (int)keiui_win32_scale(TITLEBAR_CRUMB_INSET, window->dpi);
	baseline = kl_text_center(pixels, control->y, (int)control->height);
	clip.x = control->x + inset / 2;
	clip.y = control->y;
	clip.width = (int)control->width - inset;
	clip.height = (int)control->height;

	/* Not edited: its text, or its placeholder. */
	if (!window->editing || window->field.control != index) {
		if (control->text[0] != '\0') {
			(void)kl_text_draw_fit(&window->text, canvas, control->x + inset, baseline, control->text, pixels, 0,
			    (int)control->width - 2 * inset, theme->text);
		} else {
			(void)kl_text_draw_fit(&window->text, canvas, control->x + inset, baseline, control->placeholder, pixels, 0,
			    (int)control->width - 2 * inset, theme->text_faint);
		}

		/* Stops here: the box shows its text and nothing is edited. */
		return;
	}

	/* The field: scrolled so that the caret shows. */
	field = &window->field;
	low = field->caret;
	high = field->anchor;
	if (field->anchor < field->caret) {
		low = field->anchor;
		high = field->caret;
	}

	/* Measures the text before the caret. */
	caret = kl_text_width(&window->text, field->text, field->caret, pixels, 0);
	shift = 0;
	if (caret > (int)control->width - 3 * inset)
		shift = caret - ((int)control->width - 3 * inset);
	kl_canvas_clip_push(canvas, &clip);

	/* The selection's ground, the text, the composing text after the caret, and the caret. */
	if (low != high) {
		from = kl_text_width(&window->text, field->text, low, pixels, 0);
		to = kl_text_width(&window->text, field->text, high, pixels, 0);
		mark.x = control->x + inset + from - shift;
		mark.y = control->y + (int)control->height / 6;
		mark.width = to - from;
		mark.height = (int)control->height * 2 / 3;
		kl_canvas_fill(canvas, &mark, theme->selection);
	}

	/* Draws the field's text. */
	text_length = strlen(field->text);
	(void)kl_text_draw(&window->text, canvas, control->x + inset - shift, baseline, field->text, text_length, pixels, 0, theme->text);
	composing = 0;
	if (field->composing[0] != '\0') {
		composing_length = strlen(field->composing);
		composing = kl_text_width(&window->text, field->composing, composing_length, pixels, 0);
		mark.x = control->x + inset + caret - shift;
		mark.y = control->y + (int)control->height / 6;
		mark.width = composing;
		mark.height = (int)control->height * 2 / 3;
		kl_canvas_fill(canvas, &mark, theme->control);
		composing_length = strlen(field->composing);
		(void)kl_text_draw(&window->text, canvas, mark.x, baseline, field->composing, composing_length, pixels, 0, theme->text);
		kl_canvas_line(canvas, (float)mark.x, (float)(baseline + 2), (float)(mark.x + composing), (float)(baseline + 2), 1.0f, theme->text);
		composing = kl_text_width(&window->text, field->composing, field->composing_caret, pixels, 0);
	}

	/* Places the caret's mark. */
	mark.x = control->x + inset + caret + composing - shift;
	mark.y = control->y + (int)control->height / 6;
	mark.width = (int)keiui_win32_scale(1U, window->dpi);
	mark.height = (int)control->height * 2 / 3;
	kl_canvas_fill(canvas, &mark, theme->text);
	kl_canvas_clip_pop(canvas);
}

/* Starts editing the box of a control: its text, all of it selected, with the input method. */
static void
titlebar_edit(
	struct kl_window *window,
	unsigned index)
{
	/* A field being edited elsewhere ends first. */
	if (window->editing && window->field.control != index)
		keiui_win32_field_end(window, KL_TEXT_CANCELLED);

	/* The field. */
	window->editing = 1;
	window->field.control = index;
	titlebar_copy_text(window->field.text, sizeof(window->field.text), window->controls[index].text);
	window->field.anchor = 0U;
	window->field.caret = strlen(window->field.text);
	window->field.composing[0] = '\0';

	/* The keyboard to the frame, the input method at the caret, and the drawing. */
	(void)SetFocus(window->frame);
	keiui_win32_ime_place(window);
	(void)InvalidateRect(window->frame, NULL, FALSE);
}

/* Takes the selection out of the field. */
static void
titlebar_cut(
	struct kl_window *window)
{
	/* An insertion of nothing. */
	keiui_win32_field_insert(window, "");
}

/* The field's text changed: told (KL_WINDOW_CONTROL_TEXT) and drawn. */
static void
titlebar_typed(
	struct kl_window *window)
{
	struct kl_window_event *input;

	/* Drawn, the input method at the caret. */
	(void)InvalidateRect(window->frame, NULL, FALSE);
	keiui_win32_ime_place(window);

	/* Told. */
	input = keiui_win32_push(window, KL_WINDOW_CONTROL_TEXT);
	if (input == NULL)
		return;
	input->id = (int32_t)window->controls[window->field.control].id;
	titlebar_copy_text(input->text, sizeof(input->text), window->field.text);
}

/* The start of the character before an offset of UTF-8 text. */
static size_t
titlebar_previous(
	const char *text,
	size_t at)
{
	/* Back over the continuation bytes. */
	if (at == 0U)
		return 0U;
	at--;
	while (at > 0U && ((unsigned char)text[at] & 0xc0U) == 0x80U)
		at--;
	return at;
}

/* The start of the character after an offset of UTF-8 text. */
static size_t
titlebar_next(
	const char *text,
	size_t at)
{
	/* Over the character's bytes. */
	if (text[at] == '\0')
		return at;
	at++;
	while (text[at] != '\0' && ((unsigned char)text[at] & 0xc0U) == 0x80U)
		at++;
	return at;
}

/* Copies text that fits whole characters (NULL is the empty text). */
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

	/* As much as fits, not cutting a character. */
	length = strlen(from);
	if (length + 1U > size) {
		length = size - 1U;
		while (length > 0U && ((unsigned char)from[length] & 0xc0U) == 0x80U)
			length--;
	}

	/* Copies the characters that fit and ends the text. */
	memcpy(to, from, length);
	to[length] = '\0';
}

/* Copies a menu's table, each label cut to fit; 0, or ENOMEM. */
static int
titlebar_copy_menu(
	const struct kl_menu_entry *entries,
	size_t count,
	struct keiui_win32_menu_item **items)
{
	struct keiui_win32_menu_item *copy;
	size_t index;

	/* The copy's memory. */
	copy = calloc(count, sizeof(*copy));
	if (copy == NULL)
		return ENOMEM;

	/* Each entry's fields and its label. */
	for (index = 0U; index < count; index++) {
		copy[index].id = entries[index].id;
		copy[index].parent = entries[index].parent;
		copy[index].type = entries[index].type;
		titlebar_copy_text(copy[index].label, sizeof(copy[index].label), entries[index].label);
		copy[index].action = entries[index].action;
		copy[index].role = entries[index].role;
		copy[index].modifiers = entries[index].modifiers;
		copy[index].keysym = entries[index].keysym;
	}

	/* Succeeded: the caller owns the copy. */
	*items = copy;
	return 0;
}

/*
 * Opens a menu of a table at a point of the screen (its top-left corner),
 * waits for it to close and queues the item chosen; 0, or ENOMEM or EIO
 * when it could not be made.
 */
static int
titlebar_popup(
	struct kl_window *window,
	const struct keiui_win32_menu_item *items,
	unsigned count,
	int32_t x,
	int32_t y)
{
	struct kl_window_event *input;
	HMENU menu;
	BOOL chosen;
	unsigned index;

	/* The menu of the top-level items, with its submenus. */
	menu = titlebar_menu_build(window, items, count, KL_MENU_ROOT, 0U);
	if (menu == NULL)
		return ENOMEM;

	/* Shown until a choice or a dismissal; the choice is the item's index plus one. */
	chosen = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
	    (int)x, (int)y, window->frame, NULL);
	(void)DestroyMenu(menu);

	/* The press that opened it is over, and the button is no longer held. */
	window->control_pressed = KEIUI_WIN32_NO_CONTROL;
	window->control_hover = KEIUI_WIN32_NO_CONTROL;
	(void)InvalidateRect(window->frame, NULL, FALSE);

	/* Nothing chosen. */
	if (chosen == 0)
		return 0;
	index = (unsigned)chosen - 1U;
	if (index >= count)
		return EIO;

	/* The item's choice. */
	input = keiui_win32_push(window, KL_WINDOW_ACTION);
	if (input == NULL)
		return 0;
	input->code = items[index].action;
	input->id = (int32_t)items[index].id;
	input->begin = 0;

	/* Succeeded: the choice is queued. */
	return 0;
}

/*
 * Makes the system's popup menu of a parent's items (KL_MENU_ROOT: the
 * top-level ones), each submenu's in turn, down to TITLEBAR_MENU_DEPTH.
 * Returns the menu (the caller destroys it, with its submenus), or NULL
 * without memory.
 */
static HMENU
titlebar_menu_build(
	struct kl_window *window,
	const struct keiui_win32_menu_item *items,
	unsigned count,
	uint32_t parent,
	unsigned depth)
{
	HMENU menu;
	unsigned index;
	int error;

	/* The menu. */
	menu = CreatePopupMenu();
	if (menu == NULL)
		return NULL;

	/* The parent's items, in the table's order. */
	for (index = 0U; index < count; index++) {
		if (items[index].parent != parent)
			continue;

		/* The item, or a menu that could not be made. */
		error = titlebar_menu_item(window, menu, items, count, index, depth);
		if (error != 0) {
			(void)DestroyMenu(menu);
			return NULL;
		}
	}

	/* Succeeded: the menu with its items. */
	return menu;
}

/*
 * Appends an item to a menu by its type and its action's state: a hidden
 * one is left out, a separator is a line, a submenu brings its own menu
 * (greyed when disabled, left out too deep), any other is chosen by its
 * index plus one, greyed when disabled, ticked when checked (a radio
 * item with a dot), with its shortcut.  Returns 0, or ENOMEM.
 */
static int
titlebar_menu_item(
	struct kl_window *window,
	HMENU menu,
	const struct keiui_win32_menu_item *items,
	unsigned count,
	unsigned index,
	unsigned depth)
{
	const struct keiui_win32_menu_item *item;
	MENUITEMINFOW info;
	char shortcut[48];
	char text[TITLEBAR_MENU_TEXT];
	uint16_t wide[TITLEBAR_MENU_TEXT];
	size_t units;
	HMENU submenu;
	unsigned state;
	BOOL added;
	int error;

	/* A hidden item is not shown. */
	item = &items[index];
	state = titlebar_state(window, item->action);
	if ((state & KL_ACTION_HIDDEN) != 0U)
		return 0;

	/* A separator's line. */
	if (item->type == KL_MENU_ITEM_SEPARATOR) {
		added = AppendMenuW(menu, MF_SEPARATOR, 0U, NULL);
		if (!added)
			return ENOMEM;
		return 0;
	}

	/* The text: the label, and the shortcut at the right. */
	shortcut[0] = '\0';
	if (item->type != KL_MENU_ITEM_SUBMENU)
		keiui_win32_shortcut_text(item->modifiers, item->keysym, shortcut, sizeof(shortcut));
	keiui_win32_menu_text(item->label, shortcut, text, sizeof(text));
	error = keiui_win32_utf16_from_utf8(text, strlen(text), wide, TITLEBAR_MENU_TEXT, &units);
	if (error != 0)
		return 0;

	/* The item's shape: its text and whether it is greyed or ticked. */
	memset(&info, 0, sizeof(info));
	info.cbSize = sizeof(info);
	info.fMask = MIIM_STRING | MIIM_STATE | MIIM_FTYPE | MIIM_ID;
	info.fType = MFT_STRING;
	info.dwTypeData = (LPWSTR)wide;
	info.cch = (UINT)units;
	info.fState = MFS_ENABLED;
	if ((state & KL_ACTION_DISABLED) != 0U)
		info.fState |= MFS_DISABLED;
	if ((state & KL_ACTION_CHECKED) != 0U)
		info.fState |= MFS_CHECKED;
	if (item->type == KL_MENU_ITEM_RADIO)
		info.fType |= MFT_RADIOCHECK;
	info.wID = index + 1U;

	/* A submenu's own menu, made first; one too deep is left out. */
	if (item->type == KL_MENU_ITEM_SUBMENU) {
		if (depth + 1U >= TITLEBAR_MENU_DEPTH)
			return 0;
		submenu = titlebar_menu_build(window, items, count, item->id, depth + 1U);
		if (submenu == NULL)
			return ENOMEM;
		info.fMask |= MIIM_SUBMENU;
		info.hSubMenu = submenu;
		info.wID = 0U;
	}

	/* The item at the menu's end (a submenu then belongs to the menu). */
	added = InsertMenuItemW(menu, (UINT)GetMenuItemCount(menu), TRUE, &info);
	if (!added) {
		if (item->type == KL_MENU_ITEM_SUBMENU)
			(void)DestroyMenu(info.hSubMenu);
		return ENOMEM;
	}

	/* Succeeded: the item is in the menu. */
	return 0;
}

/* Opens the window's menu below its button. */
static void
titlebar_menu_open(
	struct kl_window *window)
{
	POINT point;
	BOOL turned;

	/* The button's bottom-left corner on the screen. */
	point.x = window->menu_place.x;
	point.y = window->menu_place.y + (LONG)window->menu_place.height;
	turned = ClientToScreen(window->frame, &point);
	if (!turned)
		return;

	/* The menu, until it closes. */
	(void)titlebar_popup(window, window->menu, window->menu_count, (int32_t)point.x, (int32_t)point.y);
}

/*
 * Lays the tabs out from a left edge, leaving the bare title bar before a
 * right edge to drag the window by, and keeps the menu's button and the
 * tab strip as the hit test's client places.
 */
static void
titlebar_tabs_layout(
	struct kl_window *window,
	int32_t left,
	int32_t right,
	uint32_t height)
{
	struct keiui_win32_tab_place places[KEIUI_WIN32_TABS];
	struct keiui_win32_tab_strip strip;
	struct keiui_win32_rect *place;
	int closable[KEIUI_WIN32_TABS];
	int32_t end;
	unsigned index;

	/* The strip's room, and which tab is active. */
	memset(&strip, 0, sizeof(strip));
	strip.left = left;
	strip.right = right - (int32_t)keiui_win32_scale(TITLEBAR_DRAG_LEAST, window->dpi);
	if (strip.right < left)
		strip.right = left;
	strip.caption = height;
	strip.dpi = window->dpi;
	strip.count = window->tab_count;
	strip.active = window->tab_count;
	for (index = 0U; index < window->tab_count; index++) {
		closable[index] = 0;
		if ((window->tabs[index].flags & KL_TAB_CLOSABLE) != 0U)
			closable[index] = 1;
		if ((window->tabs[index].flags & KL_TAB_ACTIVE) != 0U)
			strip.active = index;
	}
	strip.new_button = 0;
	if ((window->tab_options & KL_TABS_NEW_BUTTON) != 0U)
		strip.new_button = 1;

	/* The tabs' places, kept with them. */
	end = keiui_win32_tabs_layout(&strip, closable, places, &window->tab_new_place);
	for (index = 0U; index < window->tab_count; index++)
		window->tabs[index].place = places[index];

	/* The hit test's client: the menu's button, and the strip from its left to its end. */
	window->control_place_count = 0U;
	if (window->menu_count != 0U) {
		window->control_places[0] = window->menu_place;
		window->control_place_count = 1U;
	}
	place = &window->control_places[window->control_place_count];
	place->x = left;
	place->y = 0;
	place->width = 0U;
	if (end > left)
		place->width = (uint32_t)(end - left);
	place->height = height;
	window->control_place_count++;
}

/* What of the menu's button and the tabs is at a point of the frame's client, or KEIUI_WIN32_NO_CONTROL. */
static unsigned
titlebar_part_at(
	const struct kl_window *window,
	int32_t x,
	int32_t y)
{
	struct keiui_win32_tab_place places[KEIUI_WIN32_TABS];
	const struct keiui_win32_rect *menu;
	unsigned index;
	unsigned part;
	int inside;

	/* The menu's button, when the window has a menu. */
	menu = &window->menu_place;
	if (window->menu_count != 0U) {
		inside = 1;
		if (x < menu->x || x >= menu->x + (int32_t)menu->width)
			inside = 0;
		if (y < menu->y || y >= menu->y + (int32_t)menu->height)
			inside = 0;
		if (inside)
			return KEIUI_WIN32_MENU_BUTTON;
	}

	/* No tabs. */
	if (window->tab_count == 0U)
		return KEIUI_WIN32_NO_CONTROL;

	/* The tab or the button there. */
	for (index = 0U; index < window->tab_count; index++)
		places[index] = window->tabs[index].place;
	index = keiui_win32_tabs_at(places, window->tab_count, &window->tab_new_place, x, y, &part);
	if (index == KEIUI_WIN32_TAB_NONE)
		return KEIUI_WIN32_NO_CONTROL;

	/* Which of a tab, or the new tab's button. */
	if (part == KEIUI_WIN32_TAB_PART_NEW)
		return KEIUI_WIN32_TAB_NEW_BUTTON;
	if (part == KEIUI_WIN32_TAB_PART_CLOSE)
		return KEIUI_WIN32_TAB_CLOSE_BUTTON + index;
	return KEIUI_WIN32_TAB_BODY + index;
}

/* Queues a tab's input (KL_WINDOW_TAB_*, and the tab's ID; 0 for a new one). */
static void
titlebar_tab_input(
	struct kl_window *window,
	unsigned code,
	uint32_t id)
{
	struct kl_window_event *input;

	/* The input. */
	input = keiui_win32_push(window, KL_WINDOW_TAB);
	if (input == NULL)
		return;
	input->code = code;
	input->id = (int32_t)id;
}

/* Draws the menu's button: its ground under the pointer or held, and its lines. */
static void
titlebar_menu_draw(
	struct kl_window *window,
	struct kl_canvas *canvas)
{
	/* A button of the list's icon, the control's size. */
	titlebar_icon_button(window, canvas, &window->menu_place, KEIUI_WIN32_MENU_BUTTON, KL_ICON_LIST, TITLEBAR_ICON);
}

/* Draws a tab: its ground (the panel's when active, the hover's under the pointer), its title, its mark and its close button. */
static void
titlebar_tab_draw(
	struct kl_window *window,
	struct kl_canvas *canvas,
	unsigned index)
{
	const struct keiui_win32_tab *tab;
	const struct kl_theme *theme;
	kl_color ink;
	unsigned pixels;
	int baseline;
	int inset;
	int start;
	int room;
	float radius;
	float mark;

	/* A tab not shown. */
	tab = &window->tabs[index];
	if (!tab->place.shown)
		return;
	theme = kl_theme_default();
	radius = (float)keiui_win32_scale(TITLEBAR_RADIUS, window->dpi);

	/* Its ground: the panel's when active, the hover's under the pointer. */
	if ((tab->flags & KL_TAB_ACTIVE) != 0U) {
		kl_canvas_round(canvas, (float)tab->place.tab.x, (float)tab->place.tab.y, (float)tab->place.tab.width,
		    (float)tab->place.tab.height, radius, theme->panel);
	} else if (window->control_hover == KEIUI_WIN32_TAB_BODY + index) {
		kl_canvas_round(canvas, (float)tab->place.tab.x, (float)tab->place.tab.y, (float)tab->place.tab.width,
		    (float)tab->place.tab.height, radius, theme->hover);
	}

	/* The attention's mark before the title, in the accent. */
	inset = (int)keiui_win32_scale(TITLEBAR_TAB_INSET, window->dpi);
	start = tab->place.tab.x + inset;
	if ((tab->flags & KL_TAB_ATTENTION) != 0U) {
		mark = (float)keiui_win32_scale(TITLEBAR_TAB_MARK, window->dpi);
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
	if (window->text_open && room > 0) {
		pixels = titlebar_pixels(window);
		baseline = kl_text_center(pixels, tab->place.tab.y, (int)tab->place.tab.height);
		(void)kl_text_draw_fit(&window->text, canvas, start, baseline, tab->title, pixels, 0, room, ink);
	}

	/* The close button. */
	if (tab->place.close.width != 0U)
		titlebar_icon_button(window, canvas, &tab->place.close, KEIUI_WIN32_TAB_CLOSE_BUTTON + index, KL_ICON_CLOSE, TITLEBAR_TAB_ICON);
}

/* Draws the new tab's button, when there is one. */
static void
titlebar_new_draw(
	struct kl_window *window,
	struct kl_canvas *canvas)
{
	/* No button. */
	if (window->tab_new_place.width == 0U)
		return;

	/* A button of the plus. */
	titlebar_icon_button(window, canvas, &window->tab_new_place, KEIUI_WIN32_TAB_NEW_BUTTON, KL_ICON_PLUS, TITLEBAR_TAB_ICON);
}

/* Draws a button of an icon: its ground under the pointer or held, and the icon (size at 96 dpi) in its middle. */
static void
titlebar_icon_button(
	struct kl_window *window,
	struct kl_canvas *canvas,
	const struct keiui_win32_rect *place,
	unsigned part,
	enum kl_icon icon,
	uint32_t size)
{
	const struct kl_theme *theme;
	kl_color fill;
	float side;

	/* Its ground under the pointer, or held. */
	theme = kl_theme_default();
	if (window->control_pressed == part || window->control_hover == part) {
		fill = theme->hover;
		if (window->control_pressed == part)
			fill = theme->selection;
		kl_canvas_round(canvas, (float)place->x, (float)place->y, (float)place->width, (float)place->height,
		    (float)keiui_win32_scale(TITLEBAR_RADIUS, window->dpi), fill);
	}

	/* The icon in its middle. */
	side = (float)keiui_win32_scale(size, window->dpi);
	kl_icon_draw(canvas, icon, (float)place->x + ((float)place->width - side) / 2.0f, (float)place->y + ((float)place->height - side) / 2.0f,
	    side, theme->text);
}
