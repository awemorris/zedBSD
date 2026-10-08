/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Dragging the desktop's items, and the drops on the desktop (files
 * --desktop, ws094-p006, plan/ws094/design.md §4).
 *
 * A left press held on an item that moves a few pixels drags the
 * selection as the compositor's drag and drop (dnd.c), from the start: the
 * desktop is under every window, so only the compositor knows whether the
 * pointer is over the desktop or a window.  Over a Files window the drop
 * is that window's (its folder takes the items); over the desktop it comes
 * back here as a drop of the desktop's own items: on a folder item they
 * move into the folder, elsewhere they move to the cell under the pointer
 * (the pressed item there, the others keeping their places from it), and
 * the places are saved.  A drop of another window's items moves (or
 * copies) them into ~/Desktop or a folder item, the new items placed from
 * the cell of the drop.  The target is lit while a drop is over the
 * desktop; the compositor draws what is carried.
 */

#include "files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How far the pointer moves from the press before the items are dragged, in pixels. */
#define DESKTOP_DRAG_START	6

/* The target's colours: a folder item's ground and ring, and the cell's edge. */
#define DESKTOP_TARGET_GROUND	KL_RGBA(FM_COLOR_ACCENT, 60)
#define DESKTOP_TARGET_RING	KL_RGBA(FM_COLOR_ACCENT, 200)

static void drop_find(struct fm_app *app, int x, int y);
static int drop_cell_taken(const struct fm_desktop *desk, const struct fm_tab *tab, int column, int row, int moving);
static int drop_free_cell(const struct fm_desktop *desk, const struct fm_tab *tab, const unsigned char *assigned, int columns, int rows, int from, int *column, int *row);
static void drop_move_item(struct fm_app *app, unsigned char *assigned, int columns, int rows, int item, int dx, int dy);
static void drop_folder_path(struct fm_app *app, char *folder, size_t size);

/*
 * Starts following a left press on an item: it becomes a drag when the
 * pointer moves far enough before the release.
 */
void
fm_desktop_drag_press(
	struct fm_app *app,
	int index,
	int x,
	int y)
{
	struct fm_desktop *desk;

	/* The pressed item and where. */
	desk = &app->desk;
	desk->pressing = 1;
	desk->press_index = index;
	desk->press_x = x;
	desk->press_y = y;

	/* Succeeded: the press is ready for drag tracking. */
	return;
}

/*
 * Follows the pointer while a press on an item is held: far enough from
 * the press, the selection goes to the compositor's drag and drop.  Returns 1
 * while the press is the drag's.
 */
int
fm_desktop_drag_motion(
	struct fm_app *app,
	int x,
	int y)
{
	struct fm_desktop *desk;
	struct fm_tab *tab;
	int dx;
	int dy;

	/* No press held, or the drag is the compositor's already. */
	desk = &app->desk;
	if (!desk->pressing)
		return 0;
	if (desk->dragging)
		return 1;

	/* Not yet far enough from the press. */
	dx = x - desk->press_x;
	dy = y - desk->press_y;
	if (dx * dx + dy * dy <= DESKTOP_DRAG_START * DESKTOP_DRAG_START)
		return 1;

	/* The pressed item must still be one of the items. */
	tab = fm_ui_tab(app);
	if (desk->press_index < 0 || (size_t)desk->press_index >= tab->listing.count) {
		desk->pressing = 0;
		return 0;
	}

	/* An unselected pressed item becomes the selection. */
	if (tab->listing.entries[desk->press_index].selected == 0)
		fm_select_only(tab, desk->press_index);

	/*
	 * The compositor carries the selection from here (main.c starts it):
	 * drag_outside says the drag is the desktop's own, so that its drop
	 * back on the desktop is known as such, until the drag's end.
	 */
	desk->dragging = 1;
	desk->click_index = -1;
	app->drag_outside = 1;
	app->request = FM_REQUEST_DRAG_OUT;
	app->dirty = 1;
	fm_log("DESKTOP drag start name=%s x=%d y=%d", tab->listing.entries[desk->press_index].name, x, y);
	return 1;
}

/*
 * Ends a press on an item at its release, when it did not become a drag.
 */
void
fm_desktop_drag_release(
	struct fm_app *app)
{
	struct fm_desktop *desk;
	struct fm_tab *tab;

	/* A drag goes on until the compositor ends it. */
	desk = &app->desk;
	if (desk->dragging)
		return;

	/* The press is over; a plain click on a selected item selects it alone now. */
	desk->pressing = 0;
	tab = fm_ui_tab(app);
	if (desk->press_alone &&
	    desk->press_index >= 0 &&
	    (size_t)desk->press_index < tab->listing.count) {
		fm_select_only(tab, desk->press_index);
		app->dirty = 1;
	}

	/* The click is done with. */
	desk->press_alone = 0;

	/* Succeeded: the click is no longer held. */
	return;
}

/*
 * Follows a drag and drop over the desktop: its enter, motion, leave and
 * drop, the action the compositor chose, and the end of the desktop's own drag.
 */
void
fm_desktop_drop_event(
	struct fm_app *app,
	const struct fm_event *event)
{
	struct fm_desktop *desk;

	/* Each event. */
	desk = &app->desk;
	app->dirty = 1;
	switch (event->type) {
	case FM_EVENT_DROP_ENTER:
		/* A drag comes over the desktop: whether it is the desktop's own, and whether it carries file names. */
		app->drop_active = 1;
		app->drop_self = 0;
		if (event->pressed != 0 && desk->dragging)
			app->drop_self = 1;
		app->drop_files = event->focused;
		app->drop_kinds = event->button;

		/* A picture or text without file names becomes a new file on the desktop (ws189-p003). */
		app->drop_content = 0;
		if (!app->drop_self &&
		    !app->drop_files &&
		    (app->drop_kinds & (KL_DROP_IMAGE | KL_DROP_TEXT)) != 0U)
			app->drop_content = 1;
		app->drag_target = FM_DRAG_NONE;
		desk->drop_item = -1;
		desk->drop_column = -1;
		desk->drop_row = -1;
		fm_log("DESKTOP drop enter self=%d files=%d content=%d x=%d y=%d", app->drop_self, app->drop_files, app->drop_content, event->x, event->y);
		drop_find(app, event->x, event->y);
		break;
	case FM_EVENT_DROP_MOTION:
		/* It moves: the target under it. */
		drop_find(app, event->x, event->y);
		break;
	case FM_EVENT_DROP_ACTION:
		/* The compositor's choice of move or copy. */
		app->drop_action = event->action;
		break;
	case FM_EVENT_DROP_LEAVE:
		/* It went over a window, or away. */
		app->drop_active = 0;
		app->drag_target = FM_DRAG_NONE;
		desk->drop_item = -1;
		desk->drop_column = -1;
		fm_log("DESKTOP drop leave");
		break;
	case FM_EVENT_DROP:
		/* Dropped on a target: the Wayland side fetches the names (or, for the desktop's own items on the desktop, moves them to cells). */
		app->drop_active = 0;
		if (app->drag_target != FM_DRAG_FOLDER) {
			fm_log("DESKTOP drop target=none");
			break;
		}

		/* The folder and the operation (a copy when the compositor chose one). */
		snprintf(app->drop_folder, sizeof(app->drop_folder), "%s", app->drag_folder);
		app->drop_operation = FM_TASK_MOVE;
		if (app->drop_action == FM_DND_COPY)
			app->drop_operation = FM_TASK_COPY;
		app->request = FM_REQUEST_DROP;

		/* A picture or text becomes a new file, a copy, with no choice to ask (ws189-p003). */
		if (app->drop_content) {
			app->drop_operation = FM_TASK_COPY;
			fm_log("DESKTOP drop content kinds=%u column=%d row=%d", app->drop_kinds, desk->drop_column, desk->drop_row);
			break;
		}

		/* The desktop's own items on the desktop itself move to cells, whatever action was chosen. */
		desk->drop_place = 0;
		if (app->drop_self && desk->drop_item < 0) {
			desk->drop_place = 1;
			fm_log("DESKTOP drop place column=%d row=%d", desk->drop_column, desk->drop_row);
			break;
		}

		/* The log line the tests read. */
		fm_log("DESKTOP drop self=%d destination=%s action=%u column=%d row=%d", app->drop_self, app->drop_folder, app->drop_action, desk->drop_column, desk->drop_row);

		/* "Ask": the choice in a context menu at the drop's place first (ui-context.c). */
		if (app->drop_action == FM_DND_ASK) {
			app->request = FM_REQUEST_DROP_ASK;
			app->context_where = FM_CONTEXT_DROP;
			app->context_x = app->drop_x;
			app->context_y = app->drop_y;
			app->drop_asking = 1;
		}

		/* The drop is the Wayland side's now. */
		break;
	case FM_EVENT_DRAG_DONE:
		/* The desktop's own drag is over (dropped somewhere, or cancelled). */
		fm_log("DESKTOP drag done dropped=%d", event->pressed);
		desk->dragging = 0;
		desk->pressing = 0;
		app->drag_outside = 0;
		break;
	default:
		break;
	}

	/* Succeeded: the drag event has been dispatched. */
	return;
}

/*
 * Lights the target of a drop over the desktop: a folder item's ground and
 * ring, or the edge of the cell the items would go to.
 */
void
fm_desktop_drop_draw(
	struct fm_app *app,
	struct kl_canvas *canvas)
{
	struct fm_desktop *desk;
	struct kl_rect cell;
	int placed;

	/* Only while a drop with a target is over the desktop. */
	desk = &app->desk;
	if (!app->drop_active || app->drag_target != FM_DRAG_FOLDER)
		return;

	/* A folder item: its cell lit. */
	if (desk->drop_item >= 0 && (size_t)desk->drop_item < desk->place_count) {
		placed = fm_desktop_cell_rect(desk->places[desk->drop_item].column, desk->places[desk->drop_item].row, canvas->width, canvas->height, &cell);
		if (!placed)
			return;
		kl_canvas_round(canvas, (float)cell.x + 4.0f, (float)cell.y, (float)cell.width - 8.0f, (float)cell.height - 4.0f, 12.0f, DESKTOP_TARGET_GROUND);
		kl_canvas_round_border(canvas, (float)cell.x + 4.0f, (float)cell.y, (float)cell.width - 8.0f, (float)cell.height - 4.0f, 12.0f, 2.0f, DESKTOP_TARGET_RING);
		return;
	}

	/* The desktop: the cell under the drop, its edge. */
	placed = fm_desktop_cell_rect(desk->drop_column, desk->drop_row, canvas->width, canvas->height, &cell);
	if (!placed)
		return;
	kl_canvas_round_border(canvas, (float)cell.x + 4.0f, (float)cell.y, (float)cell.width - 8.0f, (float)cell.height - 4.0f, 12.0f, 2.0f, DESKTOP_TARGET_RING);

	/* Succeeded: the drop target is highlighted. */
	return;
}

/*
 * Carries out a drop of the desktop's own items on the desktop itself
 * (drop_place): the pressed item goes to the cell of the drop and the
 * other selected items keep their places from it; an item whose cell would
 * be off the grid or another item's stays.  The places are saved.  Returns
 * 1 when the drop was such a move (the caller finishes the drag and drop),
 * 0 otherwise.
 */
int
fm_desktop_drop_place(
	struct fm_app *app)
{
	struct fm_desktop *desk;
	struct fm_tab *tab;
	unsigned char *assigned;
	size_t index;
	int columns;
	int rows;
	int dx;
	int dy;

	/* Only a move of the desktop's own items to cells. */
	desk = &app->desk;
	if (!desk->drop_place)
		return 0;
	desk->drop_place = 0;

	/* A drop outside every cell, or without the pressed item, moves nothing. */
	tab = fm_ui_tab(app);
	if (desk->drop_column < 0 ||
	    desk->press_index < 0 ||
	    (size_t)desk->press_index >= desk->place_count) {
		fm_log("DESKTOP move none");
		return 1;
	}

	/* How far the pressed item moves, in cells. */
	dx = desk->drop_column - desk->places[desk->press_index].column;
	dy = desk->drop_row - desk->places[desk->press_index].row;

	/* The cells given in this move, so that two items do not take one. */
	fm_desktop_grid(desk->width, desk->height, &columns, &rows);
	assigned = calloc((size_t)columns * (size_t)rows, 1U);
	if (assigned == NULL)
		return 1;

	/* The pressed item first, then the other selected items in order. */
	drop_move_item(app, assigned, columns, rows, desk->press_index, dx, dy);
	for (index = 0; index < tab->listing.count && index < desk->place_count; index++) {
		/* The pressed item moved already; unselected items stay. */
		if ((int)index == desk->press_index)
			continue;
		if (tab->listing.entries[index].selected == 0)
			continue;
		drop_move_item(app, assigned, columns, rows, (int)index, dx, dy);
	}

	/* The marks are done with; the next frame lays the items out from the saved places. */
	free(assigned);
	app->dirty = 1;

	/* Succeeded: the items moved to their cells. */
	return 1;
}

/*
 * Places the items another window dropped into ~/Desktop, by their names:
 * from the cell of the drop down its column and on to the left, in the
 * free cells.  The places are saved, so the items appear there when the
 * operation has made them.
 */
void
fm_desktop_dropped(
	struct fm_app *app,
	char *const *paths,
	size_t count)
{
	struct fm_desktop *desk;
	struct fm_tab *tab;
	unsigned char *assigned;
	char folder[FM_PATH_MAX];
	const char *name;
	size_t index;
	int columns;
	int rows;
	int from;
	int column;
	int row;
	int found;
	int same;
	int error;

	/* Only a drop into the desktop's own folder on a cell (a folder item takes its items in). */
	desk = &app->desk;
	if (desk->drop_item >= 0 || desk->drop_column < 0)
		return;
	drop_folder_path(app, folder, sizeof(folder));
	same = strcmp(folder, app->drop_folder);
	if (same != 0)
		return;

	/* The grid, the items, and the cells given to the dropped items. */
	tab = fm_ui_tab(app);
	fm_desktop_grid(desk->width, desk->height, &columns, &rows);
	assigned = calloc((size_t)columns * (size_t)rows, 1U);
	if (assigned == NULL)
		return;

	/* Each dropped item's name, in the next free cell from the drop's. */
	from = desk->drop_column * rows + desk->drop_row;
	for (index = 0; index < count; index++) {
		/* The name, after the last slash. */
		name = strrchr(paths[index], '/');
		if (name == NULL)
			name = paths[index];
		else
			name++;

		/* The next free cell; none left places the rest where the layout puts them. */
		found = drop_free_cell(desk, tab, assigned, columns, rows, from, &column, &row);
		if (!found)
			break;

		/* The place, kept for when the item appears. */
		assigned[column * rows + row] = 1U;
		from = column * rows + row + 1;
		error = fm_desktop_layout_set(desk, name, column, row);
		fm_log("DESKTOP dropped name=%s column=%d row=%d error=%d", name, column, row, error);
	}

	/* The marks are done with. */
	free(assigned);

	/* Succeeded: the assigned dropped-item places have been reported. */
	return;
}

/*
 * Finds the target under a drop over the desktop: a folder item (not one
 * being dragged), else the desktop's folder at the cell under it.  A drag
 * of another window without file names has no target.  A changed target
 * is answered to the compositor.
 */
static void
drop_find(
	struct fm_app *app,
	int x,
	int y)
{
	struct fm_desktop *desk;
	struct fm_tab *tab;
	unsigned previous;
	int previous_item;
	int index;
	int in_cell;

	/* The target before, to tell a change; the drop's place for a context menu. */
	desk = &app->desk;
	tab = fm_ui_tab(app);
	previous = app->drag_target;
	previous_item = desk->drop_item;
	app->drop_x = x;
	app->drop_y = y;

	/* A drag without file names, a picture or text has no target here. */
	if (!app->drop_self && !app->drop_files && !app->drop_content) {
		app->drag_target = FM_DRAG_NONE;
		desk->drop_item = -1;
		desk->drop_column = -1;
		if (previous != app->drag_target)
			app->drop_answer = 1;
		return;
	}

	/* The cell under it, for a move or the dropped items' places. */
	in_cell = fm_desktop_cell_at(x, y, desk->width, desk->height, &desk->drop_column, &desk->drop_row);
	if (!in_cell) {
		desk->drop_column = -1;
		desk->drop_row = -1;
	}

	/* A folder item under it, not one of the desktop's own dragged items (a picture or text goes to the desktop's folder alone). */
	desk->drop_item = -1;
	index = -1;
	if (!app->drop_content)
		index = fm_desktop_item_at(app, x, y);
	if (index >= 0 &&
	    (size_t)index < tab->listing.count &&
	    tab->listing.entries[index].folder != 0) {
		if (!app->drop_self || tab->listing.entries[index].selected == 0)
			desk->drop_item = index;
	}

	/* The target: the folder item, or the desktop's folder. */
	app->drag_target = FM_DRAG_FOLDER;
	if (desk->drop_item >= 0) {
		snprintf(app->drag_folder, sizeof(app->drag_folder), "%s", tab->listing.entries[desk->drop_item].path);
	} else {
		drop_folder_path(app, app->drag_folder, sizeof(app->drag_folder));
	}

	/* A changed target is answered, and logged for the tests. */
	if (previous != app->drag_target || previous_item != desk->drop_item) {
		app->drop_answer = 1;
		fm_log("DESKTOP drop target=%s", app->drag_folder);
	}

	/* Succeeded: the compositor will hear any changed target. */
	return;
}

/*
 * Tells whether a cell is taken by an item: one not selected when moving
 * the selection (moving), or any.
 */
static int
drop_cell_taken(
	const struct fm_desktop *desk,
	const struct fm_tab *tab,
	int column,
	int row,
	int moving)
{
	size_t index;

	/* Each placed item. */
	for (index = 0; index < desk->place_count && index < tab->listing.count; index++) {
		/* Another cell. */
		if (desk->places[index].column != column || desk->places[index].row != row)
			continue;

		/* A selected item leaves its cell when the selection moves. */
		if (moving && tab->listing.entries[index].selected != 0)
			continue;

		/* The cell is that item's. */
		return 1;
	}

	/* No item has it. */
	return 0;
}

/*
 * Finds the first free cell from a cell's number on (the number counts
 * down a column, then the next to the left): free of the items and of the
 * cells already given.  Returns 1 with its column and row, or 0.
 */
static int
drop_free_cell(
	const struct fm_desktop *desk,
	const struct fm_tab *tab,
	const unsigned char *assigned,
	int columns,
	int rows,
	int from,
	int *column,
	int *row)
{
	int number;
	int taken;

	/* Each cell from the one given. */
	for (number = from; number < columns * rows; number++) {
		/* A cell given already. */
		if (assigned[number] != 0U)
			continue;

		/* A cell an item has. */
		taken = drop_cell_taken(desk, tab, number / rows, number % rows, 0);
		if (taken)
			continue;

		/* The free cell. */
		*column = number / rows;
		*row = number % rows;
		return 1;
	}

	/* Every cell from there is taken. */
	return 0;
}

/*
 * Moves one dragged item by a number of cells, when its new cell is in
 * the grid, not another (unselected) item's and not given in this move;
 * otherwise it stays.  The new place is saved.
 */
static void
drop_move_item(
	struct fm_app *app,
	unsigned char *assigned,
	int columns,
	int rows,
	int item,
	int dx,
	int dy)
{
	struct fm_desktop *desk;
	struct fm_tab *tab;
	const char *name;
	int column;
	int row;
	int taken;
	int error;

	/* The item's new cell. */
	desk = &app->desk;
	tab = fm_ui_tab(app);
	name = tab->listing.entries[item].name;
	column = desk->places[item].column + dx;
	row = desk->places[item].row + dy;

	/* Off the grid: it stays. */
	if (column < 0 ||
	    column >= columns ||
	    row < 0 ||
	    row >= rows) {
		fm_log("DESKTOP move name=%s kept reason=grid", name);
		return;
	}

	/* Another item's, or given already: it stays. */
	taken = drop_cell_taken(desk, tab, column, row, 1);
	if (taken || assigned[column * rows + row] != 0U) {
		fm_log("DESKTOP move name=%s kept reason=taken", name);
		return;
	}

	/* The cell is the item's, and kept. */
	assigned[column * rows + row] = 1U;
	error = fm_desktop_layout_set(desk, name, column, row);
	fm_log("DESKTOP move name=%s column=%d row=%d error=%d", name, column, row, error);

	/* Succeeded: the saved move has been reported. */
	return;
}

/* Finds the desktop's folder (the tab's place). */
static void
drop_folder_path(
	struct fm_app *app,
	char *folder,
	size_t size)
{
	struct fm_tab *tab;

	/* The place the tab shows now. */
	tab = fm_ui_tab(app);
	snprintf(folder, size, "%s", tab->history[tab->history_index].location.path);

	/* Succeeded: the caller has the target folder path. */
	return;
}
