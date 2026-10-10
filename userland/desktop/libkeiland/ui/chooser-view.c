/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The file chooser's view (ws090-p006): one frame of the window, made of
 * the widgets, and what their reports and the keys they leave do to the
 * model.  The looks are the chooser's of ws092-p003 (chooser-draw.c of
 * libkeiland), which are Files': two cards on the system's frosted glass,
 * the sidebar of places on the left and the list on the right, the
 * location above the list and the name, the filter and the buttons below
 * it; without glass the cards stand on Files' pale ground.
 */

#include "chooser.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* The space around and between the cards, their corners, and the sidebar's width. */
#define VIEW_GAP		8
#define VIEW_RADIUS		16
#define VIEW_SIDEBAR_WIDTH	184

/* The space inside the content card, the location row, the list's header and the bottom bar. */
#define VIEW_PAD		12
#define VIEW_LOCATION		28
#define VIEW_HEADER		30
#define VIEW_BAR		52

/* The buttons' and fields' height, and the buttons' widths. */
#define VIEW_BUTTON		32
#define VIEW_ACCEPT_WIDTH	92
#define VIEW_CANCEL_WIDTH	84
#define VIEW_FILTER_WIDTH	136

/* The narrowest the name may be before the filter gives it its room (Save), and the room of its label. */
#define VIEW_NAME_MIN		110
#define VIEW_NAME_LABEL		48

/* A place's height in the sidebar. */
#define VIEW_PLACE		30

/* The widths of the list's Size and Modified columns, and the narrowest list that shows Modified. */
#define VIEW_SIZE_WIDTH		84
#define VIEW_MODIFIED_WIDTH	148
#define VIEW_MODIFIED_MIN	460

/* The text sizes (Files'). */
#define VIEW_TEXT_ROW		13U
#define VIEW_TEXT_HEADER	12U
#define VIEW_TEXT_LOCATION	14U

/* The warning's colours (a refusal's message). */
#define VIEW_WARNING_TEXT	KL_RGB(0xc8313a)
#define VIEW_WARNING_GROUND	kl_theme_choose(KL_RGB(0xfff1f1), KL_RGB(0x3a2326))
#define VIEW_WARNING_EDGE	KL_RGB(0xf6c9cb)

/* The item icons' colours: a folder's body and tab, a page's edge and lines. */
#define VIEW_FOLDER		KL_RGB(0x5aa2f5)
#define VIEW_FOLDER_BACK	KL_RGB(0x3d86e0)
#define VIEW_PAGE_EDGE		KL_RGB(0xc5ccd6)
#define VIEW_PAGE_LINES		KL_RGB(0xb8c1ce)

/* The evdev codes of the letters of the chooser's commands (Ctrl+H, Ctrl+L). */
#define VIEW_KEY_H		35U
#define VIEW_KEY_L		38U

/* The separator between the parts of the location, and the ellipsis put before a location cut at its start. */
#define VIEW_CRUMB		" \xe2\x80\xba "
#define VIEW_ELLIPSIS		"\xe2\x80\xa6"

/* The question's buttons: the main one first, the one that keeps the file last. */
static const char *const view_confirm_labels[] = { "Replace", "Cancel" };

/*
 * Where the window's parts are for its size.
 */
struct view_layout {
	struct kl_rect sidebar;
	struct kl_rect content;
	struct kl_rect up;
	struct kl_rect location;
	struct kl_rect header;
	struct kl_rect list;
	struct kl_rect bar;
	struct kl_rect name;
	struct kl_rect filter;
	struct kl_rect cancel;
	struct kl_rect accept;
};

static void view_layout(const struct keiui_chooser *chooser, const struct kl_style *style, int width, int height, struct view_layout *layout);
static int view_sidebar(struct keiui_chooser *chooser, struct kl_ui *ui, const struct kl_style *style, const struct view_layout *layout);
static int view_location(struct keiui_chooser *chooser, struct kl_ui *ui, const struct kl_style *style, const struct view_layout *layout);
static void view_location_text(const struct keiui_chooser *chooser, const struct kl_style *style, char *out, size_t size, int width);
static int view_list(struct keiui_chooser *chooser, struct kl_ui *ui, const struct kl_style *style, const struct view_layout *layout);
static void view_row(const struct keiui_chooser *chooser, const struct kl_style *style, const struct view_layout *layout, size_t index, const struct kl_rect *row, kl_color ink, int modified);
static void view_item_icon(const struct kl_style *style, int folder, int x, int y, int lit);
static void view_size_text(int64_t size, char *out, size_t size_out);
static void view_time_text(int64_t when, char *out, size_t size);
static int view_bar(struct keiui_chooser *chooser, struct kl_ui *ui, const struct kl_style *style, const struct view_layout *layout);
static void view_message(const struct keiui_chooser *chooser, const struct kl_style *style, const struct view_layout *layout);
static int view_confirm(struct keiui_chooser *chooser, struct kl_ui *ui, const struct kl_style *style, const struct view_layout *layout);
static int view_key(struct keiui_chooser *chooser, struct kl_ui *ui, uint32_t code, unsigned modifiers);
static void view_move(struct keiui_chooser *chooser, long delta);
static void view_set(struct kl_rect *rect, int x, int y, int width, int height);

/*
 * Draws one frame of the chooser at a size and a time and carries out the
 * input since the last one; reports 1 when another frame is wanted (the
 * model changed, or the list or a finger moves).
 */
int
keiui_chooser_frame(
	struct keiui_chooser *chooser,
	struct kl_ui *ui,
	const struct kl_style *style,
	int width,
	int height,
	uint64_t now_us)
{
	struct view_layout layout;
	struct kl_event event;
	struct kl_rect whole;
	uint32_t index;
	int changed;
	int moving;
	int taken;

	/* The frame's time, and the parts' places. */
	chooser->now_us = now_us;
	kl_ui_begin(ui, now_us);
	view_layout(chooser, style, width, height, &layout);

	/* The widget the model wants to have the keyboard (the list by its own record). */
	if (chooser->want_focus != 0U) {
		index = 0U;
		if (chooser->want_focus == KEIUI_CHOOSER_ID_LIST)
			index = KEIUI_CHOOSER_LIST_SELF;
		kl_ui_set_focus(ui, chooser->want_focus, index);
		chooser->want_focus = 0U;
	}

	/* The ground: clear over glass (the cards' veils), Files' pale ground otherwise, and the two cards. */
	whole.x = 0;
	whole.y = 0;
	whole.width = width;
	whole.height = height;
	if (style->glass)
		kl_canvas_clear(style->canvas);
	else
		kl_canvas_gradient(style->canvas, &whole, style->theme->ground_top, style->theme->ground_bottom);
	kl_panel(style, &layout.sidebar, 1);
	kl_panel(style, &layout.content, 0);

	/* The sidebar, the location, the list, the bar and a refusal's message. */
	changed = view_sidebar(chooser, ui, style, &layout);
	changed |= view_location(chooser, ui, style, &layout);
	changed |= view_list(chooser, ui, style, &layout);
	changed |= view_bar(chooser, ui, style, &layout);
	view_message(chooser, style, &layout);

	/* The question over them all. */
	if (chooser->confirm)
		changed |= view_confirm(chooser, ui, style, &layout);

	/* The frame is drawn; the keys no widget took are the chooser's commands. */
	moving = kl_ui_end(ui, now_us);
	for (;;) {
		taken = kl_ui_take(ui, &event);
		if (!taken)
			break;
		if (event.kind == KL_EVENT_KEY)
			changed |= view_key(chooser, ui, event.code, event.modifiers);
	}

	/* Another frame when the model changed or something moves. */
	if (changed || moving)
		return 1;
	return 0;
}

/*
 * Writes the glass panels under the two cards for a size; returns how
 * many (2, or 0 without room for them).
 */
size_t
keiui_chooser_panels(
	int width,
	int height,
	struct kl_rect *panels,
	size_t capacity)
{
	/* Room for both. */
	if (capacity < 2U)
		return 0;

	/* The sidebar's card and the content's. */
	view_set(&panels[0], VIEW_GAP, VIEW_GAP, VIEW_SIDEBAR_WIDTH, height - 2 * VIEW_GAP);
	view_set(&panels[1], 2 * VIEW_GAP + VIEW_SIDEBAR_WIDTH, VIEW_GAP, width - 3 * VIEW_GAP - VIEW_SIDEBAR_WIDTH, height - 2 * VIEW_GAP);

	/* Succeeded: two panels. */
	return 2;
}

/* Lays the window's parts out for its size. */
static void
view_layout(
	const struct keiui_chooser *chooser,
	const struct kl_style *style,
	int width,
	int height,
	struct view_layout *layout)
{
	struct kl_rect cards[2];
	int accept_width;
	int middle;
	int right;
	int left;

	/* The two cards, side by side. */
	(void)style;
	(void)keiui_chooser_panels(width, height, cards, 2U);
	layout->sidebar = cards[0];
	layout->content = cards[1];

	/* The row of the button to the folder above and the location. */
	view_set(&layout->up, layout->content.x + VIEW_PAD, layout->content.y + 10, VIEW_LOCATION, VIEW_LOCATION);
	view_set(&layout->location, layout->up.x + VIEW_LOCATION + 8, layout->up.y, layout->content.width - 2 * VIEW_PAD - VIEW_LOCATION - 8, VIEW_LOCATION);

	/* The list's header, the bottom bar, and the list between them. */
	view_set(&layout->header, layout->content.x + VIEW_PAD, layout->up.y + VIEW_LOCATION + 8, layout->content.width - 2 * VIEW_PAD, VIEW_HEADER);
	view_set(&layout->bar, layout->header.x, layout->content.y + layout->content.height - VIEW_BAR, layout->header.width, VIEW_BAR);
	view_set(&layout->list, layout->header.x, layout->header.y + VIEW_HEADER, layout->header.width, layout->bar.y - layout->header.y - VIEW_HEADER - 4);

	/* The buttons from the right of the bar, in the middle of its height. */
	middle = layout->bar.y + (VIEW_BAR - VIEW_BUTTON) / 2 + 2;
	right = layout->bar.x + layout->bar.width;
	accept_width = VIEW_ACCEPT_WIDTH;
	if (chooser->mode == KL_FILE_CHOOSER_FOLDER)
		accept_width = 136;
	view_set(&layout->accept,
		 right - accept_width,
		 middle,
		 accept_width,
		 VIEW_BUTTON);
	view_set(&layout->cancel, layout->accept.x - 8 - VIEW_CANCEL_WIDTH, middle, VIEW_CANCEL_WIDTH, VIEW_BUTTON);

	/* Open: the filter on the left, no name. */
	left = layout->bar.x;
	if (chooser->mode != KL_FILE_CHOOSER_SAVE) {
		view_set(&layout->filter, left, middle, VIEW_FILTER_WIDTH, VIEW_BUTTON);
		if (chooser->filter_count < 2U)
			view_set(&layout->filter, 0, 0, 0, 0);
		view_set(&layout->name, 0, 0, 0, 0);
		return;
	}

	/* Save: the filter left of the buttons when there is room, and the name filling the rest after its label. */
	right = layout->cancel.x - 12;
	view_set(&layout->filter, 0, 0, 0, 0);
	if (chooser->filter_count > 1U && right - left - VIEW_NAME_LABEL - VIEW_FILTER_WIDTH - 10 >= VIEW_NAME_MIN) {
		view_set(&layout->filter, right - VIEW_FILTER_WIDTH, middle, VIEW_FILTER_WIDTH, VIEW_BUTTON);
		right = layout->filter.x - 10;
	}

	/* The name takes the rest. */
	view_set(&layout->name, left + VIEW_NAME_LABEL, middle, right - left - VIEW_NAME_LABEL, VIEW_BUTTON);
}

/* Draws the sidebar's places and shows the one clicked; 1 when the model changed. */
static int
view_sidebar(
	struct keiui_chooser *chooser,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct view_layout *layout)
{
	struct kl_rect row;
	size_t index;
	int current;
	int pressed;
	int changed;
	int top;

	/* The section's title. */
	top = kl_sidebar_section(style, layout->sidebar.x + 8, layout->sidebar.y + 8, layout->sidebar.width - 16, "Places");

	/* Each place, lit when it is shown; a click shows it. */
	changed = 0;
	kl_canvas_clip_push(style->canvas, &layout->sidebar);
	for (index = 0; index < chooser->place_count; index++) {
		view_set(&row, layout->sidebar.x + 8, top + (int)index * VIEW_PLACE, layout->sidebar.width - 16, VIEW_PLACE);
		current = keiui_chooser_is_place(chooser, index);
		pressed = kl_sidebar_item(ui, style, KEIUI_CHOOSER_ID_PLACES, (uint32_t)index, &row, chooser->places[index].icon, chooser->places[index].label, current);
		if (pressed && !chooser->confirm) {
			keiui_chooser_go_place(chooser, index);
			changed = 1;
		}
	}

	/* Drawing reaches the whole window again. */
	kl_canvas_clip_pop(style->canvas);
	return changed;
}

/* Draws the button to the folder above and the location (or the path's field); 1 when the model changed. */
static int
view_location(
	struct keiui_chooser *chooser,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct view_layout *layout)
{
	const struct kl_theme *theme;
	char shown[KEIUI_CHOOSER_PATH_MAX];
	kl_color ink;
	unsigned changes;
	unsigned state;
	float middle_x;
	float middle_y;
	int enabled;
	int changed;

	/* The button: a round ground under the pointer, and an arrow, pale when there is nothing above. */
	theme = style->theme;
	changed = 0;
	state = kl_ui_hit(ui, KEIUI_CHOOSER_ID_UP, 0U, &layout->up);
	enabled = keiui_chooser_can_go_up(chooser);
	ink = theme->icon;
	if (!enabled)
		ink = theme->text_faint;
	middle_x = (float)layout->up.x + (float)layout->up.width * 0.5f;
	middle_y = (float)layout->up.y + (float)layout->up.height * 0.5f;
	if (enabled && (state & KL_HIT_HOT) != 0U)
		kl_canvas_circle(style->canvas, middle_x, middle_y, (float)layout->up.width * 0.5f, theme->hover);
	kl_canvas_line(style->canvas, middle_x, middle_y - 6.0f, middle_x, middle_y + 6.0f, 1.8f, ink);
	kl_canvas_line(style->canvas, middle_x - 5.0f, middle_y - 1.0f, middle_x, middle_y - 6.0f, 1.8f, ink);
	kl_canvas_line(style->canvas, middle_x + 5.0f, middle_y - 1.0f, middle_x, middle_y - 6.0f, 1.8f, ink);

	/* A click goes to the folder above. */
	if (enabled && (state & KL_HIT_CLICKED) != 0U && !chooser->confirm) {
		chooser->message[0] = '\0';
		keiui_chooser_go_up(chooser);
		changed = 1;
	}

	/* The path's field while a path is typed: Enter goes there, Esc closes it. */
	if (chooser->typing_path) {
		changes = kl_field(ui, style, KEIUI_CHOOSER_ID_PATH, &layout->location, &chooser->path, NULL);
		if ((changes & KL_FIELD_CHANGED) != 0U)
			chooser->message[0] = '\0';
		if ((changes & KL_FIELD_SUBMITTED) != 0U) {
			keiui_chooser_accept_path(chooser);
			changed = 1;
		}

		/* Esc closes it. */
		if ((changes & KL_FIELD_CANCELLED) != 0U) {
			keiui_chooser_close_path(chooser);
			changed = 1;
		}

		/* Typing changed the field. */
		return changed | (int)(changes != 0U);
	}

	/* The location's parts, lit under the pointer; a click types a path. */
	state = kl_ui_hit(ui, KEIUI_CHOOSER_ID_LOCATION, 0U, &layout->location);
	if ((state & KL_HIT_HOT) != 0U)
		kl_canvas_round(style->canvas, (float)layout->location.x, (float)layout->location.y, (float)layout->location.width, (float)layout->location.height, 8.0f, theme->hover);
	view_location_text(chooser, style, shown, sizeof(shown), layout->location.width - 20);
	(void)kl_text_draw(style->text, style->canvas, layout->location.x + 10, kl_text_center(VIEW_TEXT_LOCATION, layout->location.y, layout->location.height), shown, strlen(shown), VIEW_TEXT_LOCATION, 1, theme->text);
	if ((state & KL_HIT_CLICKED) != 0U && !chooser->confirm) {
		chooser->message[0] = '\0';
		keiui_chooser_open_path(chooser);
		changed = 1;
	}

	/* Reports whether the model changed. */
	return changed;
}

/*
 * Writes the location as its parts from Home or Computer ("Home ›
 * Documents"), dropping the first parts behind an ellipsis when it is
 * wider than width.
 */
static void
view_location_text(
	const struct keiui_chooser *chooser,
	const struct kl_style *style,
	char *out,
	size_t size,
	int width)
{
	char parts[KEIUI_CHOOSER_PATH_MAX];
	const char *home;
	const char *rest;
	const char *start;
	const char *crumb;
	size_t length;
	int inside;
	int same;
	int fits;

	/* Recent is its own name. */
	if (chooser->recent) {
		snprintf(out, size, "Recent");
		return;
	}

	/* The folder from Home when it is in the home folder, from Computer otherwise. */
	home = keiui_chooser_home();
	rest = chooser->folder;
	snprintf(parts, sizeof(parts), "Computer");
	inside = 0;
	length = 0;
	if (home[0] == '/' && home[1] != '\0') {
		length = strlen(home);
		same = strncmp(chooser->folder, home, length);
		if (same == 0 && (chooser->folder[length] == '/' || chooser->folder[length] == '\0'))
			inside = 1;
	}

	/* A folder in the home folder starts from Home. */
	if (inside) {
		snprintf(parts, sizeof(parts), "Home");
		rest = chooser->folder + length;
	}

	/* Each part after it, joined by the separator. */
	while (*rest == '/')
		rest++;
	while (*rest != '\0') {
		length = strcspn(rest, "/");
		snprintf(parts + strlen(parts), sizeof(parts) - strlen(parts), "%s%.*s", VIEW_CRUMB, (int)length, rest);
		rest += length;
		while (*rest == '/')
			rest++;
	}

	/* The whole location when it fits. */
	start = parts;
	fits = kl_text_width(style->text, start, strlen(start), VIEW_TEXT_LOCATION, 1);
	if (fits <= width) {
		snprintf(out, size, "%s", parts);
		return;
	}

	/* Else the last parts that fit behind the ellipsis (at least the last one). */
	for (;;) {
		crumb = strstr(start, VIEW_CRUMB);
		if (crumb == NULL)
			break;
		start = crumb + strlen(VIEW_CRUMB);
		snprintf(out, size, "%s%s%s", VIEW_ELLIPSIS, VIEW_CRUMB, start);
		fits = kl_text_width(style->text, out, strlen(out), VIEW_TEXT_LOCATION, 1);
		if (fits <= width)
			return;
	}

	/* Only the last part, cut to the width. */
	(void)kl_text_fit(style->text, start, VIEW_TEXT_LOCATION, 1, width, out, size);
}

/* Draws the list's header and its rows (or why it has none), and carries out what the list reports; 1 when the model changed. */
static int
view_list(
	struct keiui_chooser *chooser,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct view_layout *layout)
{
	const struct kl_theme *theme;
	const struct kl_rect *header;
	char line[KEIUI_CHOOSER_MESSAGE_MAX];
	struct kl_rect row;
	kl_color ink;
	unsigned changes;
	size_t first;
	size_t last;
	size_t index;
	long selected;
	int modified;
	int baseline;
	int width;

	/* The columns' titles (Modified only when the list is wide enough) and the line under them. */
	theme = style->theme;
	header = &layout->header;
	modified = 0;
	if (layout->list.width >= VIEW_MODIFIED_MIN)
		modified = 1;
	baseline = kl_text_center(VIEW_TEXT_HEADER, header->y, header->height);
	(void)kl_text_draw(style->text, style->canvas, header->x + 34, baseline, "Name", 4U, VIEW_TEXT_HEADER, 0, theme->text_secondary);
	if (modified) {
		(void)kl_text_draw(style->text, style->canvas, header->x + header->width - VIEW_MODIFIED_WIDTH + 4, baseline, "Modified", 8U, VIEW_TEXT_HEADER, 0, theme->text_secondary);
		(void)kl_text_draw(style->text, style->canvas, header->x + header->width - VIEW_MODIFIED_WIDTH - VIEW_SIZE_WIDTH + 4, baseline, "Size", 4U, VIEW_TEXT_HEADER, 0, theme->text_secondary);
	} else {
		(void)kl_text_draw(style->text, style->canvas, header->x + header->width - VIEW_SIZE_WIDTH + 4, baseline, "Size", 4U, VIEW_TEXT_HEADER, 0, theme->text_secondary);
	}

	/* The line under the titles. */
	view_set(&row, header->x, header->y + header->height - 1, header->width, 1);
	kl_canvas_fill(style->canvas, &row, theme->separator);

	/* The rows that show. */
	changes = kl_list_begin(ui, style, KEIUI_CHOOSER_ID_LIST, &layout->list, &chooser->list, chooser->count, &first, &last);
	for (index = first; index < last; index++) {
		changes |= kl_list_row(ui, style, KEIUI_CHOOSER_ID_LIST, &layout->list, &chooser->list, index, &row, &ink);
		view_row(chooser, style, layout, index, &row, ink, modified);
	}

	/* The list ends. */
	kl_list_end(ui, style, &layout->list, &chooser->list);

	/* An empty list says why. */
	if (chooser->count == 0U) {
		if (chooser->list_error != 0)
			snprintf(line, sizeof(line), "Can't open this folder: %s", strerror(chooser->list_error));
		else if (chooser->recent)
			snprintf(line, sizeof(line), "No recent files");
		else
			snprintf(line, sizeof(line), "This folder is empty");
		width = kl_text_width(style->text, line, strlen(line), VIEW_TEXT_ROW, 0);
		(void)kl_text_draw(style->text, style->canvas, layout->list.x + (layout->list.width - width) / 2, kl_text_center(VIEW_TEXT_ROW, layout->list.y, layout->list.height / 2), line, strlen(line), VIEW_TEXT_ROW, 0, theme->text_secondary);
	}

	/* Nothing reported, or a question asked: nothing to do. */
	if (changes == 0U || chooser->confirm)
		return 0;
	chooser->message[0] = '\0';
	selected = chooser->list.selected;

	/* A double click, a double tap or Enter activates; a finger's tap on a folder goes into it at once. */
	if ((changes & KL_LIST_ACTIVATED) != 0U) {
		keiui_chooser_activate(chooser, selected);
		return 1;
	}

	/* A finger's tap on a folder goes into it at once. */
	if ((changes & KL_LIST_TOUCHED) != 0U && selected >= 0L && (size_t)selected < chooser->count && chooser->entries[selected].folder) {
		keiui_chooser_activate(chooser, selected);
		return 1;
	}

	/* A selection (Save's name follows a file). */
	if ((changes & KL_LIST_SELECTED) != 0U)
		keiui_chooser_select(chooser, selected);
	return 1;
}

/* Draws one row's content over the ground the list gave it: its icon, name, size and time. */
static void
view_row(
	const struct keiui_chooser *chooser,
	const struct kl_style *style,
	const struct view_layout *layout,
	size_t index,
	const struct kl_rect *row,
	kl_color ink,
	int modified)
{
	const struct keiui_chooser_entry *entry;
	char cell[64];
	kl_color faint;
	int baseline;
	int name_width;
	int width;
	int lit;
	int right;

	/* The ink of the row: the list's (the accent's ink on the accent), the quiet one a little fainter. */
	entry = &chooser->entries[index];
	faint = style->theme->text_secondary;
	lit = 0;
	if (ink != style->theme->text) {
		faint = KL_RGBA(ink, 210);
		lit = 1;
	}

	/* The icon and the name. */
	baseline = kl_text_center(VIEW_TEXT_ROW, row->y, row->height);
	view_item_icon(style, entry->folder, row->x + 6, row->y + 3, lit);
	name_width = layout->list.width - 34 - VIEW_SIZE_WIDTH - 8;
	if (modified)
		name_width -= VIEW_MODIFIED_WIDTH;
	(void)kl_text_draw_fit(style->text, style->canvas, row->x + 34, baseline, entry->name, VIEW_TEXT_ROW, 0, name_width - 8, ink);

	/* The size, to the right of its column ("--" for a folder). */
	if (entry->folder)
		snprintf(cell, sizeof(cell), "--");
	else
		view_size_text(entry->size, cell, sizeof(cell));
	width = kl_text_width(style->text, cell, strlen(cell), VIEW_TEXT_ROW, 0);
	right = layout->list.x + layout->list.width - 16;
	if (modified)
		right -= VIEW_MODIFIED_WIDTH;
	(void)kl_text_draw(style->text, style->canvas, right - width, baseline, cell, strlen(cell), VIEW_TEXT_ROW, 0, faint);

	/* The time it changed. */
	if (modified) {
		view_time_text(entry->modified, cell, sizeof(cell));
		(void)kl_text_draw_fit(style->text, style->canvas, layout->list.x + layout->list.width - VIEW_MODIFIED_WIDTH + 4, baseline, cell, VIEW_TEXT_ROW, 0, VIEW_MODIFIED_WIDTH - 16, faint);
	}
}

/* Draws an item's small icon in a 22-pixel square: a blue folder, or a white page (its lines in the accent on a lit row). */
static void
view_item_icon(
	const struct kl_style *style,
	int folder,
	int x,
	int y,
	int lit)
{
	struct kl_rect line;
	kl_color lines;

	/* A folder: its tab behind, its body in front, a light edge on top. */
	if (folder) {
		kl_canvas_round(style->canvas, (float)(x + 2), (float)(y + 3), 9.0f, 6.0f, 2.0f, VIEW_FOLDER_BACK);
		kl_canvas_round(style->canvas, (float)(x + 2), (float)(y + 5), 19.0f, 14.0f, 3.0f, VIEW_FOLDER);
		view_set(&line, x + 3, y + 6, 17, 1);
		kl_canvas_fill(style->canvas, &line, KL_RGBA(0xffffff, 90));
		return;
	}

	/* A page with a thin edge and three lines of text. */
	lines = VIEW_PAGE_LINES;
	if (lit)
		lines = style->theme->accent;
	kl_canvas_round(style->canvas, (float)(x + 4), (float)(y + 1), 15.0f, 20.0f, 3.0f, kl_theme_choose(KL_RGB(0xffffff), KL_RGB(0x2c313b)));
	kl_canvas_round_border(style->canvas, (float)(x + 4), (float)(y + 1), 15.0f, 20.0f, 3.0f, 1.0f, VIEW_PAGE_EDGE);
	view_set(&line, x + 7, y + 7, 9, 1);
	kl_canvas_fill(style->canvas, &line, lines);
	line.y = y + 10;
	kl_canvas_fill(style->canvas, &line, lines);
	view_set(&line, x + 7, y + 13, 6, 1);
	kl_canvas_fill(style->canvas, &line, lines);
}

/* Writes a file's size as people read it (Files' units: bytes, KB, MB, GB of 1000). */
static void
view_size_text(
	int64_t size,
	char *out,
	size_t size_out)
{
	/* Bytes, then the larger units with one decimal. */
	if (size < 1000) {
		snprintf(out, size_out, "%lld bytes", (long long)size);
	} else if (size < 1000000) {
		snprintf(out, size_out, "%.1f KB", (double)size / 1000.0);
	} else if (size < 1000000000) {
		snprintf(out, size_out, "%.1f MB", (double)size / 1000000.0);
	} else {
		snprintf(out, size_out, "%.1f GB", (double)size / 1000000000.0);
	}
}

/* Writes when an item changed: the date and the time. */
static void
view_time_text(
	int64_t when,
	char *out,
	size_t size)
{
	struct tm *local;
	time_t seconds;

	/* The local time, or nothing when it cannot be told. */
	seconds = (time_t)when;
	local = localtime(&seconds);
	if (local == NULL) {
		snprintf(out, size, "--");
		return;
	}

	/* Written as a date and a time of day. */
	snprintf(out, size, "%04d-%02d-%02d %02d:%02d", local->tm_year + 1900, local->tm_mon + 1, local->tm_mday, local->tm_hour, local->tm_min);
}

/* Draws the bottom bar: the name (Save), the filter and the two buttons; 1 when the model changed. */
static int
view_bar(
	struct keiui_chooser *chooser,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct view_layout *layout)
{
	const struct kl_rect *filter;
	struct kl_rect line;
	const char *accept;
	unsigned changes;
	unsigned state;
	unsigned flags;
	float middle_x;
	float middle_y;
	int pressed;
	int changed;
	int can;

	/* The line above the bar. */
	view_set(&line, layout->bar.x, layout->bar.y, layout->bar.width, 1);
	kl_canvas_fill(style->canvas, &line, style->theme->separator);

	/* Save: the name's label and field; Enter saves, Esc cancels. */
	changed = 0;
	if (chooser->mode == KL_FILE_CHOOSER_SAVE) {
		(void)kl_text_draw(style->text, style->canvas, layout->bar.x + 2, kl_text_center(VIEW_TEXT_ROW, layout->name.y, layout->name.height), "Name", 4U, VIEW_TEXT_ROW, 0, style->theme->text_secondary);
		changes = kl_field(ui, style, KEIUI_CHOOSER_ID_NAME, &layout->name, &chooser->name, "Name");
		if (changes != 0U) {
			chooser->message[0] = '\0';
			changed = 1;
		}

		/* Enter saves. */
		if ((changes & KL_FIELD_SUBMITTED) != 0U && !chooser->confirm)
			keiui_chooser_accept(chooser);

		/* Esc cancels. */
		if ((changes & KL_FIELD_CANCELLED) != 0U && !chooser->confirm)
			keiui_chooser_cancel(chooser);
	}

	/* The filter as a pill with its label and a small chevron, when there is a choice and room: a click shows the next. */
	if (layout->filter.width > 0) {
		filter = &layout->filter;
		state = kl_ui_hit(ui, KEIUI_CHOOSER_ID_FILTER, 0U, filter);
		kl_canvas_round(style->canvas, (float)filter->x, (float)filter->y, (float)filter->width, (float)filter->height, (float)filter->height * 0.5f, style->theme->hover);
		if ((state & KL_HIT_HOT) != 0U)
			kl_canvas_round(style->canvas, (float)filter->x, (float)filter->y, (float)filter->width, (float)filter->height, (float)filter->height * 0.5f, style->theme->hover);
		(void)kl_text_draw_fit(style->text, style->canvas, filter->x + 14, kl_text_center(VIEW_TEXT_ROW, filter->y, filter->height), chooser->filters[chooser->filter].label, VIEW_TEXT_ROW, 0, filter->width - 40, style->theme->text);
		middle_x = (float)(filter->x + filter->width - 18);
		middle_y = (float)filter->y + (float)filter->height * 0.5f;
		kl_canvas_line(style->canvas, middle_x - 4.0f, middle_y - 2.0f, middle_x, middle_y + 2.0f, 1.5f, style->theme->text_secondary);
		kl_canvas_line(style->canvas, middle_x + 4.0f, middle_y - 2.0f, middle_x, middle_y + 2.0f, 1.5f, style->theme->text_secondary);
		if ((state & KL_HIT_CLICKED) != 0U && !chooser->confirm) {
			chooser->message[0] = '\0';
			keiui_chooser_next_filter(chooser);
			changed = 1;
		}
	}

	/* Cancel. */
	pressed = kl_button(ui, style, KEIUI_CHOOSER_ID_CANCEL, &layout->cancel, "Cancel", 0U);
	if (pressed && !chooser->confirm) {
		keiui_chooser_cancel(chooser);
		changed = 1;
	}

	/* The main button, faded when it cannot be pressed. */
	accept = "Open";
	if (chooser->mode == KL_FILE_CHOOSER_SAVE)
		accept = "Save";

	/* Folder mode chooses a directory without navigating through the accept button. */
	if (chooser->mode == KL_FILE_CHOOSER_FOLDER)
		accept = "Select Folder";
	flags = KL_BUTTON_PRIMARY;
	can = keiui_chooser_can_accept(chooser);
	if (!can)
		flags |= KL_BUTTON_DISABLED;
	pressed = kl_button(ui, style, KEIUI_CHOOSER_ID_ACCEPT, &layout->accept, accept, flags);
	if (pressed && !chooser->confirm) {
		chooser->message[0] = '\0';
		keiui_chooser_accept(chooser);
		changed = 1;
	}

	/* Reports whether the model changed. */
	return changed;
}

/* Draws a refusal's message over the bottom of the list: a pale red chip as wide as its words. */
static void
view_message(
	const struct keiui_chooser *chooser,
	const struct kl_style *style,
	const struct view_layout *layout)
{
	struct kl_rect chip;
	int width;

	/* No message, nothing. */
	if (chooser->message[0] == '\0')
		return;

	/* The chip at the list's bottom. */
	width = kl_text_width(style->text, chooser->message, strlen(chooser->message), VIEW_TEXT_ROW, 0) + 28;
	if (width > layout->list.width)
		width = layout->list.width;
	view_set(&chip, layout->list.x + (layout->list.width - width) / 2, layout->list.y + layout->list.height - 38, width, 32);
	kl_canvas_round(style->canvas, (float)chip.x, (float)chip.y, (float)chip.width, (float)chip.height, 10.0f, VIEW_WARNING_GROUND);
	kl_canvas_round_border(style->canvas, (float)chip.x, (float)chip.y, (float)chip.width, (float)chip.height, 10.0f, 1.0f, VIEW_WARNING_EDGE);
	(void)kl_text_draw_fit(style->text, style->canvas, chip.x + 14, kl_text_center(VIEW_TEXT_ROW, chip.y, chip.height), chooser->message, VIEW_TEXT_ROW, 0, chip.width - 28, VIEW_WARNING_TEXT);
}

/* Draws the question before a file is replaced over the content, and carries out its answer; 1 when answered. */
static int
view_confirm(
	struct keiui_chooser *chooser,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct view_layout *layout)
{
	char where[KEIUI_CHOOSER_PATH_MAX];
	char title[KEIUI_CHOOSER_PATH_MAX + 32];
	char body[KEIUI_CHOOSER_PATH_MAX + 96];
	const char *name;
	const char *folder;
	char *slash;
	int answer;

	/* The name and the folder of the file. */
	snprintf(where, sizeof(where), "%s", chooser->confirm_path);
	slash = strrchr(where, '/');
	name = where;
	folder = "/";
	if (slash != NULL) {
		*slash = '\0';
		name = slash + 1;
		if (where[0] != '\0') {
			folder = strrchr(where, '/');
			if (folder == NULL || folder[1] == '\0')
				folder = where;
			else
				folder++;
		}
	}

	/* The question and why it is asked, over the content. */
	snprintf(title, sizeof(title), "Replace \"%s\"?", name);
	snprintf(body, sizeof(body), "A file with that name already exists in \"%s\". Replacing it overwrites its contents.", folder);
	answer = kl_dialog(ui, style, KEIUI_CHOOSER_ID_CONFIRM, &layout->content, title, body, view_confirm_labels, 2);

	/* Replace answers; Cancel keeps the file. */
	if (answer == 0) {
		keiui_chooser_replace(chooser);
		return 1;
	}

	/* Cancel, Esc. */
	if (answer == 1) {
		keiui_chooser_keep(chooser);
		return 1;
	}

	/* Not answered yet. */
	return 0;
}

/* Carries out a key no widget took: the chooser's commands; 1 when the model changed. */
static int
view_key(
	struct keiui_chooser *chooser,
	struct kl_ui *ui,
	uint32_t code,
	unsigned modifiers)
{
	uint32_t character;
	unsigned control;
	unsigned alt;

	/* An answered chooser, or one asking its question, takes nothing more. */
	if (chooser->answered || chooser->confirm)
		return 0;
	control = modifiers & KL_MOD_CTRL;
	alt = modifiers & KL_MOD_ALT;
	chooser->message[0] = '\0';

	/* The keys that mean the same wherever the keyboard is. */
	switch (code) {
	case KL_KEY_ESC:
		/* Esc closes the path's field first, then cancels. */
		if (chooser->typing_path) {
			keiui_chooser_close_path(chooser);
			return 1;
		}

		/* Otherwise the chooser ends without a path. */
		keiui_chooser_cancel(chooser);
		return 1;
	case KL_KEY_ENTER:
	case KL_KEY_KPENTER:
		keiui_chooser_accept(chooser);
		return 1;
	case KL_KEY_BACKSPACE:
		keiui_chooser_go_up(chooser);
		return 1;
	case KL_KEY_UP:
		/* Alt+Up goes to the folder above; Up alone selects the item above. */
		if (alt != 0U) {
			keiui_chooser_go_up(chooser);
			return 1;
		}

		/* Up alone: the item above. */
		view_move(chooser, -1L);
		break;
	case KL_KEY_DOWN:
		view_move(chooser, 1L);
		break;
	case KL_KEY_HOME:
		view_move(chooser, -(long)chooser->count);
		break;
	case KL_KEY_END:
		view_move(chooser, (long)chooser->count);
		break;
	default:
		break;
	}

	/* A key of the list moved the selection, and the list has the keyboard. */
	if (code == KL_KEY_UP || code == KL_KEY_DOWN || code == KL_KEY_HOME || code == KL_KEY_END) {
		kl_ui_set_focus(ui, KEIUI_CHOOSER_ID_LIST, KEIUI_CHOOSER_LIST_SELF);
		return 1;
	}

	/* Control's commands: the hidden items and the path's field. */
	if (control != 0U) {
		if (code == VIEW_KEY_H) {
			keiui_chooser_toggle_hidden(chooser);
			return 1;
		}

		/* Ctrl+L types a path. */
		if (code == VIEW_KEY_L) {
			keiui_chooser_open_path(chooser);
			return 1;
		}

		/* Another command with Control is not the chooser's. */
		return 0;
	}

	/* A character selects the next item whose name starts with it. */
	character = kl_key_character(code, modifiers);
	if (character != 0U && alt == 0U) {
		keiui_chooser_type_select(chooser, character);
		kl_ui_set_focus(ui, KEIUI_CHOOSER_ID_LIST, KEIUI_CHOOSER_LIST_SELF);
		return 1;
	}

	/* Anything else changes nothing. */
	return 0;
}

/* Moves the selection by some items, within the list (from before the first going down, after the last going up). */
static void
view_move(
	struct keiui_chooser *chooser,
	long delta)
{
	long index;

	/* An empty list has nothing to select. */
	if (chooser->count == 0U)
		return;

	/* From the item selected, or from an end. */
	index = chooser->list.selected;
	if (index < 0L && delta > 0L)
		index = -1L;
	if (index < 0L && delta < 0L)
		index = (long)chooser->count;
	index += delta;

	/* Within the list. */
	if (index < 0L)
		index = 0L;
	if (index >= (long)chooser->count)
		index = (long)chooser->count - 1L;
	keiui_chooser_select(chooser, index);
}

/* Sets a rectangle. */
static void
view_set(
	struct kl_rect *rect,
	int x,
	int y,
	int width,
	int height)
{
	/* Its corner and size. */
	rect->x = x;
	rect->y = y;
	rect->width = width;
	rect->height = height;
}
