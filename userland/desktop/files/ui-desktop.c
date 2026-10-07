/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop's icons (files --desktop, ws094-p003 to p005,
 * plan/ws094/design.md §4): the items of ~/Desktop drawn on zdesktop's
 * desktop surface, over the wallpaper, in the cells desktop-layout.c gives
 * them, and what the pointer and the keys do to them.
 *
 * Each cell has the item's icon (a picture's thumbnail) and its name under
 * it, dark with a white halo so that it reads on the light wallpaper; a
 * selected item has a light ground behind its icon and its name on a blue
 * pill.  A click selects (Ctrl adds or takes away, Shift selects from the
 * anchor), a drag from where no item is draws a rubber band that selects
 * what it touches, a double click or Enter opens (a file with its default
 * way, WS093; a folder in a new Files window), the arrows move the
 * selection to the nearest item that way, Ctrl+A selects all and Esc
 * nothing.  A right press opens the context menu of the items under it or
 * of the empty desktop (ui-context.c; its actions, ui-desktop-actions.c).
 * A name being changed is a field in place of the name.  The file
 * manager's messages show in a pill at the bottom, and its questions (a
 * name taken by a paste) on a card in the middle, over the dimmed desktop.
 * A press held on an item drags the selection, and drops come over the
 * desktop (ui-desktop-drag.c).  The rest of the surface is clear.
 */

#include "files.h"

#include "userland/desktop/paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* A cell's icon, how far it is under the cell's top, and the name's size and baseline. */
#define DESKTOP_ICON		64
#define DESKTOP_ICON_TOP	4
#define DESKTOP_TEXT		13U
#define DESKTOP_TEXT_BASELINE	(DESKTOP_ICON + 24)

/* A two-line name's first baseline and the distance between its lines (both lines in the 104-pixel cell). */
#define DESKTOP_TEXT_BASELINE_TWO	(DESKTOP_ICON + 20)
#define DESKTOP_TEXT_LINE		15

/* The name's colours: dark text, a white halo; a selected name's pill and text. */
#define DESKTOP_TEXT_COLOR	KL_RGB(0x1e293b)
#define DESKTOP_HALO_COLOR	KL_RGBA(0xffffff, 150)
#define DESKTOP_PILL_COLOR	FM_COLOR_ACCENT
#define DESKTOP_PILL_TEXT	FM_COLOR_ACCENT_INK

/*
 * How far round the rubber band its frame is drawn again when it moves
 * (pixels: its edge's smoothing reaches past its rectangle, BUG-221).
 */
#define DESKTOP_BAND_MARGIN	2

/* A selected icon's ground, and the rubber band's fill and edge. */
#define DESKTOP_GROUND_COLOR	KL_RGBA(0xffffff, 110)
#define DESKTOP_BAND_FILL	KL_RGBA(FM_COLOR_ACCENT, 40)
#define DESKTOP_BAND_EDGE	KL_RGBA(FM_COLOR_ACCENT, 160)

/* The field of a name being changed: its height and how far it is under the icon. */
#define DESKTOP_FIELD_HEIGHT	22
#define DESKTOP_FIELD_TOP	(DESKTOP_ICON + 10)

/* The message's pill: its height, text size and distance from the bottom. */
#define DESKTOP_PILL_HEIGHT	28
#define DESKTOP_PILL_TEXT_SIZE	12U
#define DESKTOP_PILL_BOTTOM	56

/* A second click this soon after the first on the same item is a double click, in milliseconds. */
#define DESKTOP_DOUBLE_CLICK_MS	400U

/* The most items opened by one Enter. */
#define DESKTOP_OPEN_MAX	8

/* The keys the desktop answers (evdev codes). */
#define DESKTOP_KEY_ESC		1U
#define DESKTOP_KEY_ENTER	28U
#define DESKTOP_KEY_A		30U
#define DESKTOP_KEY_KPENTER	96U
#define DESKTOP_KEY_UP		103U
#define DESKTOP_KEY_LEFT	105U
#define DESKTOP_KEY_RIGHT	106U
#define DESKTOP_KEY_DOWN	108U

/* The program a folder opens in. */
#define DESKTOP_FILES		KEILAND_BINDIR "/files"

static void desktop_layout(struct fm_app *app, int width, int height);
static int desktop_over_band(const struct fm_app *app);
static void desktop_band_draw(struct fm_app *app, struct kl_canvas *canvas);
static void desktop_band_partial(struct fm_app *app, struct kl_canvas *canvas);
static void desktop_band_region(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *outer, const struct kl_rect *inner);
static void desktop_redraw_rect(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *rect);
static int desktop_partial(struct fm_app *app, struct kl_canvas *canvas);
static void desktop_painted_record(struct fm_app *app, const struct fm_entry *entry, size_t index, struct fm_desktop_painted *painted);
static void desktop_painted_keep(struct fm_app *app, struct kl_canvas *canvas);
static void desktop_clear_rect(struct kl_canvas *canvas, const struct kl_rect *rect);
static void desktop_item(struct fm_app *app, struct kl_canvas *canvas, const struct fm_entry *entry, const struct kl_rect *cell);
static void desktop_log_moved(const struct fm_desktop *desk, const char *const *names, size_t count, const struct fm_desktop_saved *known, size_t known_count);
static void desktop_name(struct fm_app *app, struct kl_canvas *canvas, const char *name, const struct kl_rect *cell, int selected);
static void desktop_name_line(struct fm_app *app, struct kl_canvas *canvas, const char *line, const struct kl_rect *cell, int baseline, int available, int selected);
static void desktop_field(struct fm_app *app, struct kl_canvas *canvas, const struct kl_rect *cell);
static void desktop_message(struct fm_app *app, struct kl_canvas *canvas);
static int desktop_renaming(const struct fm_app *app, const struct fm_entry *entry);
static uint32_t desktop_names_hash(const struct fm_tab *tab);
static void desktop_context(struct fm_app *app, const struct fm_event *event);
static void desktop_band_rect(const struct fm_desktop *desk, struct kl_rect *rect);
static void desktop_band_select(struct fm_app *app);
static long long desktop_newest_age(const struct fm_tab *tab);
static uint64_t desktop_clock(void);
static void desktop_press(struct fm_app *app, const struct fm_event *event);
static void desktop_key(struct fm_app *app, const struct fm_event *event);
static void desktop_arrow(struct fm_app *app, int dx, int dy);
static void desktop_open(struct fm_app *app, int index);
static int desktop_rects_meet(const struct kl_rect *a, const struct kl_rect *b);

/*
 * Draws the desktop: clear, with each item of the tab's folder in its cell
 * and the rubber band over them.  The layout is logged when the number of
 * items changes.  When the canvas keeps the last frame and only some cells
 * changed (a click's selection), only those cells are drawn again
 * (ws094-p009).
 */
void
fm_desktop_draw(
	struct fm_app *app,
	struct kl_canvas *canvas)
{
	struct fm_desktop *desk;
	struct kl_rect cell;
	struct fm_tab *tab;
	size_t index;
	size_t hidden;
	uint64_t ready_at;
	long long newest_age;
	int placed;
	int cells;
	int logging;
	int partial;

	/* The places for this size and listing. */
	tab = fm_ui_tab(app);
	desk = &app->desk;
	desktop_layout(app, canvas->width, canvas->height);

	/* The frame's size, no clickable part yet (a question's card records its buttons), and libkeiland's widgets' frame (ui-widgets.c). */
	app->width = canvas->width;
	app->height = canvas->height;
	app->hit_count = 0;
	(void)fm_widgets_begin(app);

	/* The layout is logged when the number of items changed (the tests read it). */
	logging = 0;
	if (desk->logged != (int)tab->listing.count + 1)
		logging = 1;

	/* Only the changed cells, when the kept frame allows. */
	if (!logging) {
		partial = desktop_partial(app, canvas);
		if (partial) {
			fm_widgets_end(app);
			app->dirty = 0;
			return;
		}
	}

	/* Clear, so that the wallpaper shows. */
	kl_canvas_clear(canvas);

	/* Each item in its cell; an item without a cell is not shown. */
	cells = 0;
	for (index = 0; index < tab->listing.count && index < desk->place_count; index++) {
		/* The item's cell. */
		placed = fm_desktop_cell_rect(desk->places[index].column, desk->places[index].row, canvas->width, canvas->height, &cell);
		if (!placed)
			continue;

		/* The item. */
		desktop_item(app, canvas, &tab->listing.entries[index], &cell);
		cells++;

		/* Where it is, for the tests. */
		if (logging)
			fm_log("DESKTOP place name=%s column=%d row=%d x=%d y=%d", tab->listing.entries[index].name, desk->places[index].column, desk->places[index].row, cell.x, cell.y);
	}

	/* The rubber band over the items. */
	desktop_band_draw(app, canvas);

	/* The target of a drop over the desktop. */
	fm_desktop_drop_draw(app, canvas);

	/* The file manager's message, and its question over everything; the widgets' frame ends. */
	desktop_message(app, canvas);
	fm_overlay_draw(app, canvas);
	fm_widgets_end(app);

	/* The summary line, once for the layout, with its time and the age of the newest item (ws094-p008). */
	if (logging) {
		/* Count the listing entries that received no visible grid cell. */
		hidden = 0U;
		if (tab->listing.count > (size_t)cells)
			hidden = tab->listing.count - (size_t)cells;

		/* Sample the timing values without changing the ready-log prefix. */
		ready_at = desktop_clock();
		newest_age = desktop_newest_age(tab);
		fm_log("DESKTOP ready items=%lu cells=%d width=%d height=%d at_ms=%llu newest_age_ms=%lld hidden=%lu",
		       (unsigned long)tab->listing.count,
		       cells,
		       canvas->width,
		       canvas->height,
		       (unsigned long long)ready_at,
		       newest_age,
		       (unsigned long)hidden);

		/* Remember which listing produced the ready summary. */
		desk->logged = (int)tab->listing.count + 1;
	}

	/* What the cells were drawn with, for the next frame; the frame is drawn. */
	desktop_painted_keep(app, canvas);
	app->dirty = 0;

	/* Succeeded: the canvas holds the complete desktop frame. */
	return;
}

/*
 * Forgets the frame kept in the canvas: the next frame is drawn whole (a new canvas).
 */
void
fm_desktop_repaint(
	struct fm_desktop *desk)
{
	desk->painted = 0;

	/* Succeeded: the next frame must repaint the whole canvas. */
	return;
}

/*
 * Takes an input on the desktop: a button, a motion (the rubber band) or
 * a key.
 */
void
fm_desktop_event(
    struct fm_app *app,
    const struct fm_event *event)
{
	struct fm_desktop *desk;
	struct fm_tab *tab;
	int dragging;
	int first;

	/* A drag and drop over the desktop, or the end of its own (ui-desktop-drag.c). */
	desk = &app->desk;
	switch (event->type) {
	case FM_EVENT_DROP_ENTER:
	case FM_EVENT_DROP_MOTION:
	case FM_EVENT_DROP_LEAVE:
	case FM_EVENT_DROP:
	case FM_EVENT_DROP_ACTION:
	case FM_EVENT_DRAG_DONE:
		fm_desktop_drop_event(app, event);
		return;
	default:
		break;
	}

	/* A question takes the pointer and the keys until it is answered (its card is the file manager's). */
	if (app->dialog != FM_DIALOG_NONE && event->type != FM_EVENT_ACTION) {
		fm_ui_event(app, event);
		return;
	}

	/* The pointer's place, for the rubber band. */
	desk->pointer_x = event->x;
	desk->pointer_y = event->y;

	/* Each kind of input. */
	switch (event->type) {
	case FM_EVENT_BUTTON:
		/* libkeiland's widgets see the press too (the field of the name being changed places its caret there). */
		fm_widgets_input(app, event);

		/* The right button: the context menu of what it is on. */
		if (event->button == FM_BUTTON_RIGHT) {
			if (event->pressed)
				desktop_context(app, event);
			break;
		}

		/* The left button: a press selects, opens or starts a band; its release ends the band. */
		if (event->button != FM_BUTTON_LEFT)
			break;

		/* A press, the release of a press on an item, or of a band. */
		if (event->pressed) {
			desktop_press(app, event);
		} else if (desk->pressing) {
			fm_desktop_drag_release(app);
		} else if (desk->band) {
			desk->band = 0;
			app->dirty = 1;
			tab = fm_ui_tab(app);
			first = fm_select_first(tab);
			fm_log("DESKTOP band end first=%d", first);
		}

		/* Nothing more for a button. */
		break;
	case FM_EVENT_MOTION:
		/* libkeiland's widgets follow the pointer; a press held on an item may become a drag. */
		fm_widgets_input(app, event);
		dragging = fm_desktop_drag_motion(app, event->x, event->y);
		if (dragging)
			break;

		/* A band follows the pointer and selects what it touches. */
		if (desk->band) {
			desktop_band_select(app);
			app->dirty = 1;
		}

		/* Nothing more for a motion. */
		break;
	case FM_EVENT_KEY:
		/* A key press. */
		if (event->pressed)
			desktop_key(app, event);
		break;
	case FM_EVENT_ACTION:
		/* A choice of the context menu. */
		fm_desktop_action(app, event->action);
		break;
	case FM_EVENT_TEXT:
	case FM_EVENT_TEXT_DELETE:
	case FM_EVENT_PREEDIT:
		/* An input method's text, for the name being changed (rename.c). */
		(void)fm_rename_input(app, event);
		break;
	default:
		break;
	}

	/* Succeeded: the input has been dispatched. */
	return;
}

/*
 * Opens the selected items (no more than DESKTOP_OPEN_MAX): each file
 * with its default way, each folder in a new Files window.
 */
void
fm_desktop_open_selected(
    struct fm_app *app)
{
	struct fm_tab *tab;
	size_t index;
	int opened;

	/* Each selected item. */
	tab = fm_ui_tab(app);
	opened = 0;
	for (index = 0; index < tab->listing.count; index++) {
		/* An item not selected, or one too many. */
		if (tab->listing.entries[index].selected == 0)
			continue;
		if (opened == DESKTOP_OPEN_MAX)
			break;

		/* It opens. */
		desktop_open(app, (int)index);
		opened++;
	}

	/* Succeeded: the bounded selection has been opened. */
	return;
}

/*
 * Finds the item whose cell is at a point of the desktop; -1 for none.
 */
int
fm_desktop_item_at(
    struct fm_app *app,
    int x,
    int y)
{
	struct fm_desktop *desk;
	struct kl_rect cell;
	size_t index;
	int placed;

	/* Each placed item's cell. */
	desk = &app->desk;
	for (index = 0; index < desk->place_count; index++) {
		/* An item without a cell. */
		placed = fm_desktop_cell_rect(desk->places[index].column, desk->places[index].row, desk->width, desk->height, &cell);
		if (!placed)
			continue;

		/* The point in its cell. */
		if (x >= cell.x &&
		    x < cell.x + cell.width &&
		    y >= cell.y &&
		    y < cell.y + cell.height)
			return (int)index;
	}

	/* No item there. */
	return -1;
}

/*
 * Tells whether something other than the rubber band is drawn over the
 * items or across cells (a kept frame with the band alone is drawn again
 * only where the band and the cells changed, BUG-221): a drag or a drop,
 * a message or an operation's progress, a question, or a name's field.
 */
static int
desktop_over_band(
	const struct fm_app *app)
{
	/* The desktop's own drag, a drop over it. */
	if (app->desk.dragging ||
	    app->drop_active)
		return 1;

	/* A question, or a name being changed. */
	if (app->dialog != FM_DIALOG_NONE || app->focus == FM_FOCUS_RENAME)
		return 1;

	/* A message while it lasts, or an operation's progress. */
	if (app->message[0] != '\0' && app->now < app->message_until)
		return 1;
	if (app->task_count > 0)
		return 1;

	/* Nothing over the items. */
	return 0;
}

/*
 * Draws again only the cells whose record changed, when the canvas holds
 * the last whole frame of the same listing and size and nothing was or is
 * over the items.  Returns 1 when the frame is done so, 0 when it must be
 * drawn whole.
 */
static int
desktop_partial(
	struct fm_app *app,
	struct kl_canvas *canvas)
{
	struct fm_desktop_painted now;
	struct fm_desktop_painted *before;
	struct fm_desktop *desk;
	struct kl_rect cell;
	struct fm_tab *tab;
	size_t index;
	int placed;
	int over;
	int differs;

	/* The kept frame, of this size and listing, with nothing but the rubber band over it now. */
	desk = &app->desk;
	tab = fm_ui_tab(app);
	over = desktop_over_band(app);
	if (!desk->painted || over)
		return 0;
	if (desk->painted_width != canvas->width || desk->painted_height != canvas->height)
		return 0;
	if (desk->painted_count != tab->listing.count || desk->place_count != tab->listing.count)
		return 0;
	if (desk->painted_modified != desk->laid_modified || desk->painted_names != desk->laid_names)
		return 0;

	/* An item moved to another cell: the whole frame (its old cell is cleared with it). */
	for (index = 0; index < tab->listing.count; index++) {
		before = &desk->painted_cells[index];
		if (before->column != desk->places[index].column || before->row != desk->places[index].row)
			return 0;
	}

	/* Each cell whose record changed: cleared and drawn again, inside it. */
	for (index = 0; index < tab->listing.count; index++) {
		desktop_painted_record(app, &tab->listing.entries[index], index, &now);
		before = &desk->painted_cells[index];
		differs = memcmp(&now, before, sizeof(now));
		if (differs == 0)
			continue;
		*before = now;

		/* An item without a cell is not shown. */
		placed = fm_desktop_cell_rect(desk->places[index].column, desk->places[index].row, canvas->width, canvas->height, &cell);
		if (!placed)
			continue;

		/* The cell again, under the band where it crosses it. */
		desktop_clear_rect(canvas, &cell);
		kl_canvas_clip_push(canvas, &cell);
		desktop_item(app, canvas, &tab->listing.entries[index], &cell);
		desktop_band_draw(app, canvas);
		kl_canvas_clip_pop(canvas);
	}

	/* The rubber band where it moved, came or went (BUG-221). */
	desktop_band_partial(app, canvas);

	/* Succeeded: the frame is the kept one with its changed cells. */
	return 1;
}

/* Draws the rubber band, while one is dragged, over what is drawn (within the clip). */
static void
desktop_band_draw(
	struct fm_app *app,
	struct kl_canvas *canvas)
{
	struct kl_rect band;

	/* No band. */
	if (!app->desk.band)
		return;

	/* Its fill and its edge. */
	desktop_band_rect(&app->desk, &band);
	kl_canvas_fill(canvas, &band, DESKTOP_BAND_FILL);
	kl_canvas_round_border(canvas, (float)band.x, (float)band.y, (float)band.width, (float)band.height, 0.0f, 1.0f, DESKTOP_BAND_EDGE);
}

/*
 * Draws the kept frame's rubber band again where it changed (BUG-221):
 * where the kept band was and where it is now, less the part inside both,
 * whose pixels stay; the frame then has the band as it is now.
 */
static void
desktop_band_partial(
	struct fm_app *app,
	struct kl_canvas *canvas)
{
	struct fm_desktop *desk;
	struct kl_rect inner;
	struct kl_rect before;
	struct kl_rect now;
	int right;
	int bottom;

	/* Neither the kept frame nor this one has a band. */
	desk = &app->desk;
	if (!desk->painted_band && !desk->band)
		return;

	/* The band now, none without one. */
	memset(&now, 0, sizeof(now));
	if (desk->band)
		desktop_band_rect(desk, &now);

	/* The part inside both bands, less the margin round their edges, which keeps its pixels (none unless both are there). */
	memset(&inner, 0, sizeof(inner));
	before = desk->painted_band_rect;
	if (desk->painted_band && desk->band) {
		inner.x = before.x;
		if (now.x > inner.x)
			inner.x = now.x;
		inner.y = before.y;
		if (now.y > inner.y)
			inner.y = now.y;
		right = before.x + before.width;
		if (now.x + now.width < right)
			right = now.x + now.width;
		bottom = before.y + before.height;
		if (now.y + now.height < bottom)
			bottom = now.y + now.height;
		inner.x += DESKTOP_BAND_MARGIN;
		inner.y += DESKTOP_BAND_MARGIN;
		inner.width = right - DESKTOP_BAND_MARGIN - inner.x;
		inner.height = bottom - DESKTOP_BAND_MARGIN - inner.y;
	}

	/* Where the kept band was, and where the band is now, less that part. */
	if (desk->painted_band)
		desktop_band_region(app, canvas, &before, &inner);
	if (desk->band)
		desktop_band_region(app, canvas, &now, &inner);

	/* The frame has the band as it is now. */
	desk->painted_band = desk->band;
	desk->painted_band_rect = now;
}

/*
 * Draws again a band's rectangle, with the margin round it, less an inner
 * rectangle (none when it has no size): the rows above and below the
 * inner one, and the columns beside it between them.
 */
static void
desktop_band_region(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *outer,
	const struct kl_rect *inner)
{
	struct kl_rect grown;
	struct kl_rect part;
	int top;
	int bottom;

	/* The rectangle with its margin. */
	grown.x = outer->x - DESKTOP_BAND_MARGIN;
	grown.y = outer->y - DESKTOP_BAND_MARGIN;
	grown.width = outer->width + 2 * DESKTOP_BAND_MARGIN;
	grown.height = outer->height + 2 * DESKTOP_BAND_MARGIN;

	/* Without an inner part, all of it. */
	if (inner->width <= 0 || inner->height <= 0) {
		desktop_redraw_rect(app, canvas, &grown);
		return;
	}

	/* The rows above the inner part. */
	top = inner->y;
	if (top < grown.y)
		top = grown.y;
	if (top > grown.y + grown.height)
		top = grown.y + grown.height;
	part.x = grown.x;
	part.y = grown.y;
	part.width = grown.width;
	part.height = top - grown.y;
	desktop_redraw_rect(app, canvas, &part);

	/* The rows below it. */
	bottom = inner->y + inner->height;
	if (bottom < top)
		bottom = top;
	if (bottom > grown.y + grown.height)
		bottom = grown.y + grown.height;
	part.y = bottom;
	part.height = grown.y + grown.height - bottom;
	desktop_redraw_rect(app, canvas, &part);

	/* The columns left and right of it, between them. */
	part.y = top;
	part.height = bottom - top;
	part.x = grown.x;
	part.width = inner->x - grown.x;
	desktop_redraw_rect(app, canvas, &part);
	part.x = inner->x + inner->width;
	part.width = grown.x + grown.width - part.x;
	desktop_redraw_rect(app, canvas, &part);
}

/*
 * Draws a rectangle of the desktop again (nothing for one without a size):
 * cleared, the items whose cells meet it and the rubber band over them,
 * within it.
 */
static void
desktop_redraw_rect(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *rect)
{
	struct fm_desktop *desk;
	struct kl_rect cell;
	struct fm_tab *tab;
	size_t index;
	int placed;
	int meets;

	/* Nothing to draw. */
	if (rect->width <= 0 || rect->height <= 0)
		return;

	/* Cleared, and drawn within it. */
	desk = &app->desk;
	tab = fm_ui_tab(app);
	desktop_clear_rect(canvas, rect);
	kl_canvas_clip_push(canvas, rect);

	/* Each item whose cell meets it. */
	for (index = 0; index < tab->listing.count && index < desk->place_count; index++) {
		placed = fm_desktop_cell_rect(desk->places[index].column, desk->places[index].row, canvas->width, canvas->height, &cell);
		if (!placed)
			continue;
		meets = desktop_rects_meet(rect, &cell);
		if (!meets)
			continue;
		desktop_item(app, canvas, &tab->listing.entries[index], &cell);
	}

	/* The band over them, and the clip ends. */
	desktop_band_draw(app, canvas);
	kl_canvas_clip_pop(canvas);
}

/* Records what an item's cell is drawn with now. */
static void
desktop_painted_record(
	struct fm_app *app,
	const struct fm_entry *entry,
	size_t index,
	struct fm_desktop_painted *painted)
{
	int kind;

	/* Zeroed first, so that records compare whole. */
	memset(painted, 0, sizeof(*painted));
	painted->column = app->desk.places[index].column;
	painted->row = app->desk.places[index].row;
	painted->selected = entry->selected;
	painted->cut = entry->cut;

	/* A picture's thumbnail, once made (fm_grid_entry_icon draws it). */
	kind = fm_thumb_kind(entry);
	if (kind != 0)
		painted->thumb = fm_thumb_get(app, entry->path, entry->modified);

	/* Succeeded: the record describes this cell. */
	return;
}

/*
 * Keeps what a whole frame's cells were drawn with, and whether the next
 * frame may draw only the changed ones (not after something was drawn over
 * the items, nor when there is no memory for the records).
 */
static void
desktop_painted_keep(
	struct fm_app *app,
	struct kl_canvas *canvas)
{
	struct fm_desktop_painted *cells;
	struct fm_desktop *desk;
	struct fm_tab *tab;
	size_t index;
	int over;

	/* Nothing kept until the records are made. */
	desk = &app->desk;
	tab = fm_ui_tab(app);
	desk->painted = 0;

	/* A frame with something but the rubber band over the items, or items without places, is not kept. */
	over = desktop_over_band(app);
	if (over || desk->place_count != tab->listing.count)
		return;

	/* Room for a record per item. */
	cells = realloc(desk->painted_cells, (tab->listing.count + 1U) * sizeof(cells[0]));
	if (cells == NULL)
		return;
	desk->painted_cells = cells;

	/* Each item's record. */
	for (index = 0; index < tab->listing.count; index++)
		desktop_painted_record(app, &tab->listing.entries[index], index, &cells[index]);

	/* The frame is kept, for this size and listing. */
	desk->painted = 1;
	desk->painted_width = canvas->width;
	desk->painted_height = canvas->height;
	desk->painted_count = tab->listing.count;
	desk->painted_modified = desk->laid_modified;
	desk->painted_names = desk->laid_names;

	/* The band the frame has, when it has one. */
	desk->painted_band = desk->band;
	if (desk->band)
		desktop_band_rect(desk, &desk->painted_band_rect);

	/* Succeeded: the records describe the retained canvas. */
	return;
}

/* Makes a rectangle of the canvas clear (transparent), within the canvas. */
static void
desktop_clear_rect(
	struct kl_canvas *canvas,
	const struct kl_rect *rect)
{
	uint32_t *row;
	int left;
	int top;
	int right;
	int bottom;
	int y;

	/* The rectangle inside the canvas. */
	left = rect->x;
	if (left < 0)
		left = 0;
	top = rect->y;
	if (top < 0)
		top = 0;
	right = rect->x + rect->width;
	if (right > canvas->width)
		right = canvas->width;
	bottom = rect->y + rect->height;
	if (bottom > canvas->height)
		bottom = canvas->height;
	if (right <= left || bottom <= top)
		return;

	/* Each row's part, zero (transparent black, premultiplied). */
	for (y = top; y < bottom; y++) {
		row = canvas->pixels + (size_t)y * canvas->stride;
		memset(row + left, 0, sizeof(row[0]) * (size_t)(right - left));
	}

	/* Succeeded: the canvas rectangle is transparent. */
	return;
}

/*
 * Works out the items' places for the desktop's size and listing when
 * either changed (the saved places read once): the saved places first,
 * then the places the items were shown at, so that a new item takes a free
 * cell and the others stay.
 */
static void
desktop_layout(
	struct fm_app *app,
	int width,
	int height)
{
	struct fm_desktop_place *places;
	struct fm_desktop_saved *known;
	struct fm_desktop *desk;
	struct fm_tab *tab;
	const char **names;
	char path[FM_PATH_MAX];
	size_t known_count;
	size_t index;
	uint32_t names_hash;
	int columns;
	int rows;
	int error;

	/* The saved places, once. */
	desk = &app->desk;
	tab = fm_ui_tab(app);
	if (!desk->loaded) {
		desk->loaded = 1;
		error = fm_desktop_layout_path(path, sizeof(path));
		if (error == 0)
			error = fm_desktop_layout_read(path, &desk->saved, &desk->saved_count);
		fm_log("DESKTOP layout saved=%lu error=%d", (unsigned long)desk->saved_count, error);
	}

	/* The listing's names in order, as a hash (a rename changes it without changing the count). */
	names_hash = desktop_names_hash(tab);

	/* An unchanged size and listing keep their places. */
	if (desk->width == width &&
	    desk->height == height &&
	    desk->laid_count == tab->listing.count &&
	    desk->laid_modified == tab->listing.modified &&
	    desk->laid_names == names_hash &&
	    desk->laid_error == tab->listing.error &&
	    desk->place_count == tab->listing.count)
		return;

	/* Room for the places and the names. */
	places = realloc(desk->places, (tab->listing.count + 1U) * sizeof(places[0]));
	if (places == NULL)
		return;
	desk->places = places;
	names = malloc((tab->listing.count + 1U) * sizeof(names[0]));
	if (names == NULL)
		return;

	/* Collect all listing names, including entries without a grid cell. */
	for (index = 0; index < tab->listing.count; index++)
		names[index] = tab->listing.entries[index].name;

	/* An unreadable directory must retain the user's saved places. */
	if (tab->listing.error == 0)
		fm_desktop_layout_prune(desk, names, tab->listing.count);

	/* The places known: the saved ones, then those shown. */
	known_count = desk->saved_count + desk->shown_count;
	known = malloc((known_count + 1U) * sizeof(known[0]));
	if (known == NULL) {
		free(names);
		return;
	}

	/* Copied in that order (the first place of a name is the one kept). */
	if (desk->saved_count != 0U)
		memcpy(known, desk->saved, desk->saved_count * sizeof(known[0]));
	if (desk->shown_count != 0U)
		memcpy(known + desk->saved_count, desk->shown, desk->shown_count * sizeof(known[0]));

	/* Place the complete listing using its retained saved places. */
	fm_desktop_arrange(names, tab->listing.count, known, known_count, width, height, desk->places);

	/* A new size is logged with its grid, and each item that could not keep its place (ws094-p010). */
	if (desk->width != width || desk->height != height) {
		fm_desktop_grid(width, height, &columns, &rows);
		fm_log("DESKTOP grid width=%d height=%d columns=%d rows=%d", width, height, columns, rows);
	}

	/* Report changed placements against the saved and previously shown places. */
	desktop_log_moved(desk, names, tab->listing.count, known, known_count);
	free(known);

	/* The places shown now, remembered for the next layout. */
	desk->place_count = tab->listing.count;
	error = fm_desktop_remember(desk, names, tab->listing.count);
	if (error != 0)
		fm_log("DESKTOP remember error=%d", error);
	free(names);

	/* The places are for this size and listing. */
	desk->width = width;
	desk->height = height;
	desk->laid_count = tab->listing.count;
	desk->laid_modified = tab->listing.modified;
	desk->laid_names = names_hash;
	desk->laid_error = tab->listing.error;
	desk->logged = 0;

	/* Succeeded: the cached places match this listing and size. */
	return;
}

/*
 * Logs each item a layout put elsewhere than its known place (saved, or
 * shown before; the first of its name counts): a place outside a smaller
 * desktop's grid, or taken, gives the item a free cell (column -1: none).
 */
static void
desktop_log_moved(
	const struct fm_desktop *desk,
	const char *const *names,
	size_t count,
	const struct fm_desktop_saved *known,
	size_t known_count)
{
	size_t index;
	size_t other;
	int same;

	/* Each item with a known place. */
	for (index = 0; index < count; index++) {
		/* Finds the first saved or previously shown place of this name. */
		for (other = 0; other < known_count; other++) {
			same = strcmp(known[other].name, names[index]);
			if (same == 0)
				break;
		}

		/* A new item has no earlier place to compare against. */
		if (other == known_count)
			continue;

		/* Where it was and where it is now, when they differ. */
		if (known[other].column == desk->places[index].column && known[other].row == desk->places[index].row)
			continue;
		fm_log("DESKTOP moved name=%s from=%d,%d to=%d,%d saved=%d",
		       names[index],
		       known[other].column,
		       known[other].row,
		       desk->places[index].column,
		       desk->places[index].row,
		       other < desk->saved_count);
	}

	/* Succeeded: all changed known placements have been logged. */
	return;
}

/* Draws one item in its cell: the ground of a selected one, its icon, centred at the top, and its name under it. */
static void
desktop_item(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct fm_entry *entry,
	const struct kl_rect *cell)
{
	float left;
	float top;
	int renaming;

	/* The icon's place, centred across the cell, a little under its top. */
	left = (float)cell->x + (float)(cell->width - DESKTOP_ICON) / 2.0f;
	top = (float)cell->y + (float)DESKTOP_ICON_TOP;

	/* A selected item's light ground behind its icon. */
	if (entry->selected)
		kl_canvas_round(canvas, left - 6.0f, top - 4.0f, (float)DESKTOP_ICON + 12.0f, (float)DESKTOP_ICON + 8.0f, 10.0f, DESKTOP_GROUND_COLOR);

	/* The icon, faded when the item is cut or dragged. */
	fm_grid_entry_icon(app, canvas, entry, left, top, (float)DESKTOP_ICON);
	if (entry->cut != 0 ||
	    (app->desk.dragging &&
	     entry->selected != 0))
		kl_canvas_round(canvas, left, top, (float)DESKTOP_ICON, (float)DESKTOP_ICON, 8.0f, KL_RGBA(0xffffff, 150));

	/* The name under it, or the field of the name being changed. */
	renaming = desktop_renaming(app, entry);
	if (renaming) {
		desktop_field(app, canvas, cell);
	} else {
		desktop_name(app, canvas, entry->name, cell, entry->selected);
	}

	/* Succeeded: the item and its name are drawn. */
	return;
}

/*
 * Draws an item's name centred under its icon, in one or two lines
 * (fm_desktop_label: a long name broken, a longer one with its middle left
 * out): white on a blue pill when selected, otherwise dark over a soft
 * white halo (the text drawn around it a pixel each way).  A selected
 * name is shown the same way, not whole: the cell is drawn again alone
 * (ws094-p009), so nothing is drawn outside it.
 */
static void
desktop_name(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const char *name,
	const struct kl_rect *cell,
	int selected)
{
	char first[FM_DESKTOP_LABEL_MAX];
	char second[FM_DESKTOP_LABEL_MAX];
	int available;
	int widest;
	int width;
	int pill_left;
	int pill_right;
	int pill_top;
	int baseline;
	int lines;

	/* The lines, no wider than the cell. */
	available = cell->width - 8;
	fm_desktop_label(app->text, name, available, DESKTOP_TEXT, first, second);
	lines = 1;
	if (second[0] != '\0')
		lines = 2;

	/* The first baseline: under the icon, a little higher with two lines so both stay in the cell. */
	baseline = cell->y + DESKTOP_TEXT_BASELINE;
	if (lines == 2)
		baseline = cell->y + DESKTOP_TEXT_BASELINE_TWO;

	/* A selected name: its pill behind both lines, kept in the cell. */
	if (selected) {
		widest = kl_text_width(app->text, first, strlen(first), DESKTOP_TEXT, 0);
		width = kl_text_width(app->text, second, strlen(second), DESKTOP_TEXT, 0);
		if (width > widest)
			widest = width;
		if (widest > available)
			widest = available;
		pill_left = cell->x + (cell->width - widest) / 2 - 5;
		if (pill_left < cell->x)
			pill_left = cell->x;
		pill_right = cell->x + (cell->width + widest) / 2 + 5;
		if (pill_right > cell->x + cell->width)
			pill_right = cell->x + cell->width;
		pill_top = baseline - 13;
		kl_canvas_round(canvas, (float)pill_left, (float)pill_top, (float)(pill_right - pill_left), (float)(18 + (lines - 1) * DESKTOP_TEXT_LINE), 9.0f, DESKTOP_PILL_COLOR);
	}

	/* Each line, centred. */
	desktop_name_line(app, canvas, first, cell, baseline, available, selected);
	if (lines == 2)
		desktop_name_line(app, canvas, second, cell, baseline + DESKTOP_TEXT_LINE, available, selected);

	/* Succeeded: the fitted name is drawn under its icon. */
	return;
}

/* Draws one line of a name centred across a cell: white on the pill when selected, otherwise dark over its halo. */
static void
desktop_name_line(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const char *line,
	const struct kl_rect *cell,
	int baseline,
	int available,
	int selected)
{
	int width;
	int left;
	int dx;
	int dy;

	/* Centred across the cell. */
	width = kl_text_width(app->text, line, strlen(line), DESKTOP_TEXT, 0);
	if (width > available)
		width = available;
	left = cell->x + (cell->width - width) / 2;

	/* On the pill. */
	if (selected) {
		(void)kl_text_draw_fit(app->text, canvas, left, baseline, line, DESKTOP_TEXT, 0, available, DESKTOP_PILL_TEXT);
		return;
	}

	/* The halo: the text a pixel off in each direction. */
	for (dy = -1; dy <= 1; dy++) {
		/* Draws the neighboring halo offsets along this row. */
		for (dx = -1; dx <= 1; dx++) {
			/* The centre is the text itself, drawn last. */
			if (dx == 0 && dy == 0)
				continue;
			(void)kl_text_draw_fit(app->text, canvas, left + dx, baseline + dy, line, DESKTOP_TEXT, 0, available, DESKTOP_HALO_COLOR);
		}
	}

	/* The text over it. */
	(void)kl_text_draw_fit(app->text, canvas, left, baseline, line, DESKTOP_TEXT, 0, available, DESKTOP_TEXT_COLOR);

	/* Succeeded: the name is drawn over its halo. */
	return;
}

/* Draws the field of the name being changed under an item's icon: white, with the accent's edge, as wide as the name (up to two cells). */
static void
desktop_field(
	struct fm_app *app,
	struct kl_canvas *canvas,
	const struct kl_rect *cell)
{
	struct kl_rect field;
	int width;

	/* As wide as the name and a little room, at least the cell and at most two cells. */
	width = kl_text_width(app->text, app->rename.text, app->rename.length, DESKTOP_TEXT, 0) + 32;
	if (width < cell->width - 4)
		width = cell->width - 4;
	if (width > 2 * cell->width)
		width = 2 * cell->width;

	/* Centred under the icon, kept on the desktop. */
	field.x = cell->x + (cell->width - width) / 2;
	if (field.x + width > canvas->width - 4)
		field.x = canvas->width - 4 - width;
	field.y = cell->y + DESKTOP_FIELD_TOP;
	field.width = width;
	field.height = DESKTOP_FIELD_HEIGHT;

	/* libkeiland's field there (rename.c). */
	fm_rename_draw(app, canvas, &field);

	/* Succeeded: the rename field is drawn in place. */
	return;
}

/* Draws the file manager's message, or else a running operation's progress, in a dark pill at the bottom of the desktop. */
static void
desktop_message(
	struct fm_app *app,
	struct kl_canvas *canvas)
{
	struct kl_style style;
	char text[256];

	/* A message while it lasts, or the first operation's progress. */
	text[0] = '\0';
	if (app->message[0] != '\0' && app->now < app->message_until)
		snprintf(text, sizeof(text), "%s", app->message);
	if (text[0] == '\0' && app->task_count > 0)
		fm_task_text(app->tasks[0], text, sizeof(text));

	/* Nothing to say. */
	if (text[0] == '\0')
		return;

	/* libkeiland's chip, centred at the bottom (ws090-p023). */
	fm_style(app, canvas, &style);
	kl_chip(&style, canvas->width / 2, canvas->height - DESKTOP_PILL_BOTTOM + DESKTOP_PILL_HEIGHT, text);

	/* Succeeded: the desktop message is visible. */
	return;
}

/* Hashes the names of the tab's listing in their order (FNV-1a, with a zero byte after each name). */
static uint32_t
desktop_names_hash(
	const struct fm_tab *tab)
{
	const unsigned char *name;
	uint32_t hash;
	size_t index;

	/* Each name's bytes and its end, in order. */
	hash = 2166136261U;
	for (index = 0; index < tab->listing.count; index++) {
		/* The name's bytes. */
		for (name = (const unsigned char *)tab->listing.entries[index].name;
		     *name != '\0';
		     name++) {
			hash ^= (uint32_t)*name;
			hash *= 16777619U;
		}

		/* The end of the name, so that "ab","c" and "a","bc" differ. */
		hash *= 16777619U;
	}

	/* Succeeded: reports the listing's ordered-name hash. */
	return hash;
}

/* Tells whether an item's name is the one being changed. */
static int
desktop_renaming(
	const struct fm_app *app,
	const struct fm_entry *entry)
{
	int differs;

	/* No name is being changed. */
	if (app->focus != FM_FOCUS_RENAME)
		return 0;

	/* The item is the one whose path the change keeps. */
	differs = strcmp(entry->path, app->rename_path);
	if (differs != 0)
		return 0;

	/* Succeeded: this item owns the active rename field. */
	return 1;
}

/*
 * Handles a right press: an item under it is selected (the selection
 * stays when the item is part of it) and the items' context menu opens;
 * where no item is, the selection goes and the empty desktop's menu
 * opens.  A name being changed ends first, keeping what was typed.
 */
static void
desktop_context(
	struct fm_app *app,
	const struct fm_event *event)
{
	struct fm_tab *tab;
	int index;

	/* The name being changed ends, and the items are placed again for the listing it left. */
	if (app->focus == FM_FOCUS_RENAME) {
		fm_desktop_rename_end(app, 1);
		desktop_layout(app, app->desk.width, app->desk.height);
	}

	/* The press's place, which the menu opens at, and the item there. */
	tab = fm_ui_tab(app);
	app->context_x = event->x;
	app->context_y = event->y;
	app->context_place = -1;
	app->dirty = 1;
	index = fm_desktop_item_at(app, event->x, event->y);

	/* An item: selected unless it already is, and the items' menu. */
	if (index >= 0 && (size_t)index < tab->listing.count) {
		if (tab->listing.entries[index].selected == 0)
			fm_select_only(tab, index);
		app->context_where = FM_CONTEXT_ITEMS;
		app->request = FM_REQUEST_CONTEXT;
		fm_log("DESKTOP context name=%s", tab->listing.entries[index].name);
		return;
	}

	/* The empty desktop: nothing stays selected, and its menu. */
	fm_select_none(tab);
	app->context_where = FM_CONTEXT_EMPTY;
	app->request = FM_REQUEST_CONTEXT;
	fm_log("DESKTOP context empty x=%d y=%d", event->x, event->y);

	/* Succeeded: the empty desktop menu is requested. */
	return;
}

/* Works out the rubber band's rectangle, from where it started to the pointer. */
static void
desktop_band_rect(
	const struct fm_desktop *desk,
	struct kl_rect *rect)
{
	/* The left edge and the width, whichever way the pointer went. */
	rect->x = desk->band_x;
	rect->width = desk->pointer_x - desk->band_x;
	if (rect->width < 0) {
		rect->x = desk->pointer_x;
		rect->width = -rect->width;
	}

	/* The top edge and the height. */
	rect->y = desk->band_y;
	rect->height = desk->pointer_y - desk->band_y;
	if (rect->height < 0) {
		rect->y = desk->pointer_y;
		rect->height = -rect->height;
	}

	/* Succeeded: the band rectangle covers either pointer direction. */
	return;
}

/* Selects the items whose cells the rubber band touches, and only those. */
static void
desktop_band_select(
	struct fm_app *app)
{
	struct fm_desktop *desk;
	struct kl_rect band;
	struct kl_rect cell;
	struct fm_tab *tab;
	size_t index;
	int placed;
	int meets;

	/* The band. */
	desk = &app->desk;
	tab = fm_ui_tab(app);
	desktop_band_rect(desk, &band);

	/* Each item's mark: whether its cell meets the band. */
	for (index = 0; index < tab->listing.count && index < desk->place_count; index++) {
		/* An item without a cell is not selected. */
		tab->listing.entries[index].selected = 0;
		placed = fm_desktop_cell_rect(desk->places[index].column, desk->places[index].row, desk->width, desk->height, &cell);
		if (!placed)
			continue;

		/* Its cell and the band. */
		meets = desktop_rects_meet(&band, &cell);
		if (meets)
			tab->listing.entries[index].selected = 1;
	}

	/* Succeeded: selection matches the touched cells. */
	return;
}

/* Handles a left press: a double click opens, a click selects, a press where no item is starts a rubber band. */
static void
desktop_press(
	struct fm_app *app,
	const struct fm_event *event)
{
	struct fm_desktop *desk;
	struct fm_tab *tab;
	int renaming;
	int index;

	/* The item under the press. */
	desk = &app->desk;
	tab = fm_ui_tab(app);
	index = fm_desktop_item_at(app, event->x, event->y);
	app->dirty = 1;

	/* A press on the field or the item whose name is being changed keeps the change; elsewhere it ends, keeping what was typed. */
	if (app->focus == FM_FOCUS_RENAME) {
		renaming = fm_rename_hit(app, event->x, event->y);
		if (renaming)
			return;
		if (index >= 0)
			renaming = desktop_renaming(app, &tab->listing.entries[index]);
		if (renaming)
			return;

		/* The change ends, and the items are placed again for the listing it left. */
		fm_desktop_rename_end(app, 1);
		desktop_layout(app, desk->width, desk->height);
		index = fm_desktop_item_at(app, event->x, event->y);
	}

	/* No item: the selection goes (Ctrl keeps it) and a rubber band starts. */
	if (index < 0) {
		if ((event->modifiers & FM_MOD_CTRL) == 0U)
			fm_select_none(tab);
		desk->band = 1;
		desk->band_x = event->x;
		desk->band_y = event->y;
		desk->click_index = -1;
		fm_log("DESKTOP band start x=%d y=%d", event->x, event->y);
		return;
	}

	/* A second press on the same item soon after the first opens the selection. */
	if (index == desk->click_index && event->time - desk->click_ms <= DESKTOP_DOUBLE_CLICK_MS) {
		desk->click_index = -1;
		fm_select_only(tab, index);
		fm_log("DESKTOP open name=%s via=double-click", tab->listing.entries[index].name);
		fm_desktop_open_selected(app);
		return;
	}

	/* A first click, remembered for a double click. */
	desk->click_index = index;
	desk->click_ms = event->time;

	/*
	 * Ctrl adds or takes away, Shift selects from the anchor, a plain click
	 * selects the item alone; on an item already selected the selection
	 * stays until the release (it may be dragged), which then selects the
	 * item alone (press_alone).
	 */
	desk->press_alone = 0;
	if ((event->modifiers & FM_MOD_CTRL) != 0U) {
		fm_select_toggle(tab, index);
	} else if ((event->modifiers & FM_MOD_SHIFT) != 0U) {
		fm_select_range(tab, tab->anchor, index);
	} else if (tab->listing.entries[index].selected != 0) {
		desk->press_alone = 1;
	} else {
		fm_select_only(tab, index);
	}

	/* The log line the tests read, and the time main.c measures the selection's frame from (ws094-p008). */
	fm_log("DESKTOP select name=%s selected=%d", tab->listing.entries[index].name, tab->listing.entries[index].selected);
	desk->select_ms = desktop_clock();

	/* A press held on a selected item may become a drag of the selection. */
	if (tab->listing.entries[index].selected != 0)
		fm_desktop_drag_press(app, index, event->x, event->y);

	/* Succeeded: the selected item may become a drag. */
	return;
}

/* Handles a key: Enter opens, the arrows move, Ctrl+A selects all, Esc nothing. */
static void
desktop_key(
	struct fm_app *app,
	const struct fm_event *event)
{
	struct fm_tab *tab;
	int handled;

	/* The name being changed takes the keys: Enter renames, Esc gives up. */
	tab = fm_ui_tab(app);
	app->dirty = 1;
	if (app->focus == FM_FOCUS_RENAME) {
		(void)fm_rename_input(app, event);
		return;
	}

	/* The keys of the file operations (Delete, F2, the clipboard, undo, a new folder). */
	handled = fm_desktop_operation_key(app, event);
	if (handled)
		return;

	/* Each other key. */
	switch (event->key) {
	case DESKTOP_KEY_ENTER:
	case DESKTOP_KEY_KPENTER:
		/* The selection opens. */
		fm_log("DESKTOP open via=enter");
		fm_desktop_open_selected(app);
		break;
	case DESKTOP_KEY_A:
		/* Ctrl+A selects all. */
		if ((event->modifiers & FM_MOD_CTRL) != 0U)
			fm_select_all(tab);
		break;
	case DESKTOP_KEY_ESC:
		/* Esc selects nothing. */
		fm_select_none(tab);
		break;
	case DESKTOP_KEY_UP:
		desktop_arrow(app, 0, -1);
		break;
	case DESKTOP_KEY_DOWN:
		desktop_arrow(app, 0, 1);
		break;
	case DESKTOP_KEY_LEFT:
		desktop_arrow(app, -1, 0);
		break;
	case DESKTOP_KEY_RIGHT:
		desktop_arrow(app, 1, 0);
		break;
	default:
		break;
	}

	/* Succeeded: the key has been dispatched. */
	return;
}

/*
 * Moves the selection to the nearest item in a direction (dx, dy: -1, 0 or
 * 1 on the screen) from the cursor; without a cursor, the first item.
 */
static void
desktop_arrow(
	struct fm_app *app,
	int dx,
	int dy)
{
	struct fm_desktop *desk;
	struct kl_rect from;
	struct kl_rect cell;
	struct fm_tab *tab;
	size_t index;
	long distance;
	long best_distance;
	int best;
	int ahead_x;
	int ahead_y;
	int placed;

	/* The cursor's cell; without one the first item is selected. */
	desk = &app->desk;
	tab = fm_ui_tab(app);
	if (tab->cursor < 0 || (size_t)tab->cursor >= desk->place_count) {
		fm_select_only(tab, 0);
		return;
	}

	/* The cursor's cell must be on the desktop. */
	placed = fm_desktop_cell_rect(desk->places[tab->cursor].column, desk->places[tab->cursor].row, desk->width, desk->height, &from);
	if (!placed)
		return;

	/* The nearest item whose cell is that way. */
	best = -1;
	best_distance = 0;
	for (index = 0; index < desk->place_count; index++) {
		/* An item without a cell. */
		placed = fm_desktop_cell_rect(desk->places[index].column, desk->places[index].row, desk->width, desk->height, &cell);
		if (!placed)
			continue;

		/* How far it is along the direction; it must be ahead. */
		ahead_x = (cell.x - from.x) * dx;
		ahead_y = (cell.y - from.y) * dy;
		if (ahead_x + ahead_y <= 0)
			continue;

		/* The distance, the sideways part counting double. */
		distance = (long)(ahead_x + ahead_y) + 2L * (long)abs((cell.x - from.x) * dy + (cell.y - from.y) * dx);
		if (best < 0 || distance < best_distance) {
			best = (int)index;
			best_distance = distance;
		}
	}

	/* The item that way becomes the selection. */
	if (best >= 0) {
		fm_select_only(tab, best);
		fm_log("DESKTOP select name=%s selected=1 via=arrow", tab->listing.entries[best].name);
	}

	/* Succeeded: any nearest item has been selected. */
	return;
}

/* Opens one item: a folder in a new Files window, a file with its default way (WS093). */
static void
desktop_open(
	struct fm_app *app,
	int index)
{
	char *arguments[3];
	struct fm_entry *entry;
	struct fm_tab *tab;
	int error;

	/* The item. */
	tab = fm_ui_tab(app);
	entry = &tab->listing.entries[index];

	/* A file opens with its default way. */
	if (entry->folder == 0) {
		fm_open_entry(app, index, 0);
		return;
	}

	/* A folder opens in a new Files window. */
	arguments[0] = DESKTOP_FILES;
	arguments[1] = entry->path;
	arguments[2] = NULL;
	error = fm_apps_spawn(arguments);
	fm_log("DESKTOP open-folder path=%s error=%d", entry->path, error);

	/* Succeeded: the folder launch has been reported. */
	return;
}

/* Tells whether two rectangles overlap. */
static int
desktop_rects_meet(
	const struct kl_rect *a,
	const struct kl_rect *b)
{
	/* Apart across. */
	if (a->x + a->width <= b->x || b->x + b->width <= a->x)
		return 0;

	/* Apart down. */
	if (a->y + a->height <= b->y || b->y + b->height <= a->y)
		return 0;

	/* Succeeded: the rectangles overlap. */
	return 1;
}

/* Reports how long ago (milliseconds) the newest item of the listing was changed, by the real clock (-1 without items). */
static long long
desktop_newest_age(
	const struct fm_tab *tab)
{
	struct timespec now;
	struct stat status;
	long long newest;
	long long changed;
	size_t index;
	int error;

	/* The newest change among the items. */
	newest = -1;
	for (index = 0; index < tab->listing.count; index++) {
		error = stat(tab->listing.entries[index].path, &status);
		if (error != 0)
			continue;
		changed = (long long)status.st_mtim.tv_sec * 1000LL + (long long)(status.st_mtim.tv_nsec / 1000000L);
		if (changed > newest)
			newest = changed;
	}

	/* No item. */
	if (newest < 0)
		return -1;

	/* Its age now. */
	error = clock_gettime(CLOCK_REALTIME, &now);
	if (error != 0)
		return -1;

	/* Succeeded: reports the newest item's age in milliseconds. */
	return (long long)now.tv_sec * 1000LL + (long long)(now.tv_nsec / 1000000L) - newest;
}

/* Reports the monotonic clock in milliseconds (main.c's fm_clock, which the frames are timed by). */
static uint64_t
desktop_clock(void)
{
	struct timespec now;
	int error;

	/* The monotonic clock. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0U;

	/* Succeeded: reports the monotonic sample in milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
