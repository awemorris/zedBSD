/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The editor of Text Editor: the document's file (opening, saving, new),
 * the frame's layout, the pointer and the keys (with the dialogs and the
 * file chooser's answer), the actions of the menus and the keys (with the
 * Find and Replace panels' requests), and time (the cursor's blinking,
 * the wheel's glide, a message fading, a selection dragged past the edge).
 * plan/ws092/design.md sections 3, 4, 6, 9 and 11.
 */

#include "textedit.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* How long the cursor shows and hides, and how long a message stays, in milliseconds. */
#define APP_BLINK_MS		530U
#define APP_MESSAGE_MS		2500U

/* How long a click may follow the one before and still count with it, and how far it may be from it. */
#define APP_CLICK_MS		400U
#define APP_CLICK_DISTANCE	4

/* How far a press in the selection moves before its text is dragged out (pixels, ws189-p003). */
#define APP_DRAG_DISTANCE	6

/* How often the view moves while it glides or a selection is dragged past the edge, in milliseconds. */
#define APP_FRAME_MS		16


/* The fewest columns a wrapped text is laid out in, and the fewest digits of the line numbers. */
#define APP_COLUMNS_MIN		8U
#define APP_DIGITS_MIN		3

/* What a drag selects by. */
#define APP_UNIT_CHARACTER	0
#define APP_UNIT_WORD		1
#define APP_UNIT_LINE		2

/* The buttons of the dialogs, as te_app_dialog_words lists them (the first is the default). */
#define APP_BUTTON_FIRST	0
#define APP_BUTTON_SECOND	1

static void app_measure(struct te_app *app);
static int app_gutter(const struct te_app *app);
static unsigned app_columns(const struct te_app *app);
static void app_fit(struct te_app *app);
static void app_replace(struct te_app *app, char *text, size_t length);
static void app_pointer(struct te_app *app, const struct te_event *event);
static void app_press(struct te_app *app, const struct te_event *event);
static void app_drag(struct te_app *app);
static void app_drag_out(struct te_app *app);
static void app_wheel(struct te_app *app, const struct te_event *event);
static void app_axis_stop(struct te_app *app, const struct te_event *event);
static size_t app_view_position(void *data, double x, double y);
static void app_view_caret(void *data, size_t position, struct kl_rect *rect);
static void app_text(struct te_app *app, const struct te_event *event);
static void app_view_word(void *data, size_t position, size_t *start, size_t *end);
static void app_key(struct te_app *app, const struct te_event *event);
static void app_request(struct te_app *app, enum te_after after);
static void app_after(struct te_app *app);
static void app_save(struct te_app *app);
static int app_save_to(struct te_app *app, const char *path);
static void app_size(struct te_app *app, int step);
static void app_dialog(struct te_app *app, enum te_dialog dialog);
static void app_recent(struct te_app *app, unsigned index);
static int app_dialog_buttons(const struct te_app *app);
static void app_choose(struct te_app *app, int saving);
static void app_chosen(struct te_app *app, const struct te_event *event);
static int app_inside(const struct te_rect *rect, int x, int y);
static void app_error_message(struct te_app *app, const char *what, const char *name, int error);

/*
 * The text's answers to the fingers' selection (libkeiland's text view
 * touch), in the text's content coordinates: the position at a point, the
 * cursor's rectangle at a position, and the word around a position.  The
 * table is constant for the program's life.
 */
static const struct kl_text_view app_text_view = {
	app_view_position,
	app_view_caret,
	app_view_word
};

/*
 * Starts the editor with an empty Untitled document, at a size, with the
 * body's and the interface's fonts.
 */
void
te_app_init(
	struct te_app *app,
	struct te_text *body,
	struct te_text *ui,
	int width,
	int height)
{
	int error;

	/* Nothing yet but the fonts and the size; line numbers and wrapping are on (J12). */
	memset(app, 0, sizeof(*app));
	app->body = body;
	app->ui = ui;
	app->width = width;
	app->height = height;
	app->pixels = TE_PIXELS_DEFAULT;
	app->line_numbers = 1;
	app->wrap = 1;
	app->focused = 1;

	/* The view's scroll (both ways; across only while lines are not wrapped) and the fingers' selection. */
	error = kl_scroll_init(&app->scroll, KL_SCROLL_X | KL_SCROLL_Y);
	if (error != 0)
		te_log("FAILED operation=scroll error=%d", error);
	kl_text_touch_init(&app->touch, &app_text_view, app);

	/* The empty document, its history and its rows. */
	error = te_buffer_init(&app->buffer, "", 0U);
	if (error != 0)
		te_log("FAILED operation=buffer error=%d", error);
	te_undo_init(&app->undo);
	te_layout_init(&app->layout);

	/* The text's measurements, and the rows. */
	app_measure(app);
	te_app_relayout(app);
	app->title_changed = 1;
	app->dirty = 1;
}

/*
 * Frees what the editor holds.
 */
void
te_app_release(
	struct te_app *app)
{
	/* The view's scroll, the rows, the history and the document. */
	kl_scroll_release(&app->scroll);
	te_layout_free(&app->layout);
	te_undo_free(&app->undo);
	te_buffer_free(&app->buffer);
}

/*
 * Opens a file in place of the document (a path that does not exist is a
 * new file of that name, made by its first save).  The caller has already
 * dealt with unsaved changes.
 *
 * Returns 0, or an errno value (the document is then as it was, and a
 * message says why).
 */
int
te_app_open(
	struct te_app *app,
	const char *path)
{
	struct te_file_info info;
	const char *name;
	char *text;
	size_t length;
	int error;

	/* The file's name, for the messages. */
	name = strrchr(path, '/');
	if (name == NULL)
		name = path;
	else
		name++;

	/* The whole file. */
	error = te_file_read(path, &text, &length, &info);
	if (error == ENOENT) {
		/* A new file of that name, empty until it is saved. */
		app_replace(app, NULL, 0U);
		snprintf(app->path, sizeof(app->path), "%s", path);
		memset(&app->file, 0, sizeof(app->file));
		te_log("OPEN path=%s bytes=0 lines=1 crlf=0 bom=0 new=1", path);
		te_app_message(app, "New file");
		return 0;
	}

	/* A file that cannot be edited says why. */
	if (error != 0) {
		app_error_message(app, "Can't open", name, error);
		return error;
	}

	/* The text in place of the document. */
	app_replace(app, text, length);
	free(text);
	snprintf(app->path, sizeof(app->path), "%s", path);
	app->file = info;
	app->opened = 1;
	te_log("OPEN path=%s bytes=%lu lines=%lu crlf=%d bom=%d", path, (unsigned long)length, (unsigned long)app->buffer.line_count, info.crlf, info.bom);

	/* Bytes that are not UTF-8 are kept, and said so. */
	if (info.invalid)
		te_app_message(app, "Some bytes aren't valid UTF-8; they are kept as they are.");

	/* Succeeded: the file is the document. */
	return 0;
}

/*
 * Starts a new, empty Untitled document.  The caller has already dealt
 * with unsaved changes.
 */
void
te_app_new(
	struct te_app *app)
{
	/* An empty text with no file. */
	app_replace(app, NULL, 0U);
	app->path[0] = '\0';
	memset(&app->file, 0, sizeof(app->file));
	te_log("NEW");
}

/*
 * Saves the document to a path (NULL: its own), and goes on with what
 * waited for the save.
 *
 * Returns 0, or an errno value (a message says why).
 */
int
te_app_save(
	struct te_app *app,
	const char *path)
{
	int error;

	/* The document's own path, unless another is given. */
	if (path == NULL)
		path = app->path;

	/* The text into the file. */
	error = app_save_to(app, path);
	if (error != 0)
		return error;

	/* Succeeded: what waited for the save goes on. */
	app_after(app);
	return 0;
}

/*
 * Takes a new size of the frame.
 */
void
te_app_resize(
	struct te_app *app,
	int width,
	int height)
{
	/* The size, and the rows laid out for its width. */
	app->width = width;
	app->height = height;
	te_app_relayout(app);
	app->dirty = 1;
}

/*
 * Takes one input of the window.
 */
void
te_app_event(
	struct te_app *app,
	const struct te_event *event)
{
	/* The input by its kind. */
	switch (event->type) {
	case TE_EVENT_MOTION:
	case TE_EVENT_BUTTON:
	case TE_EVENT_LEAVE:
		app_pointer(app, event);
		break;
	case TE_EVENT_AXIS:
		app_wheel(app, event);
		break;
	case TE_EVENT_AXIS_STOP:
		app_axis_stop(app, event);
		break;
	case TE_EVENT_KEY:
		app_key(app, event);
		break;
	case TE_EVENT_ACTION:
		te_app_action(app, (enum te_action)event->action);
		break;
	case TE_EVENT_FOCUS:
		/* The keyboard came or went; the cursor starts a blink either way. */
		app->focused = event->pressed;
		app->blink_start = app->now;
		app->dirty = 1;
		break;
	case TE_EVENT_CHOSEN:
		app_chosen(app, event);
		break;
	case TE_EVENT_TEXT:
	case TE_EVENT_TEXT_DELETE:
		app_text(app, event);
		break;
	}

	/* The line numbers may have grown a digit, which narrows the text. */
	app_fit(app);

	/* A selection the keys or the pointer changed is no longer the fingers' (their handles and bar go, ws190-p003). */
	if (app->touch.handles || app->touch.bar) {
		if (app->touch.anchor != app->anchor || app->touch.caret != app->cursor)
			kl_text_touch_set_selection(&app->touch, app->anchor, app->cursor);
	}
}

/*
 * Carries out an action of the menus, the context menu or the keys.
 */
void
te_app_action(
	struct te_app *app,
	enum te_action action)
{
	int passes;

	/*
	 * A dialog or the chooser waits for its answer; only Quit goes past
	 * them, and Find Next and Find Previous past the Find panel, which they
	 * go on from (BUG-248).
	 */
	passes = 0;
	if (action == TE_ACTION_QUIT) {
		passes = 1;
	} else if (app->dialog == TE_DIALOG_FIND && !app->choosing) {
		/* Only finding again goes past the Find panel. */
		if (action == TE_ACTION_FIND_NEXT || action == TE_ACTION_FIND_PREVIOUS)
			passes = 1;
	}

	/* Anything else waits for the dialog or the chooser. */
	if ((app->dialog != TE_DIALOG_NONE || app->choosing) && !passes)
		return;
	te_log("ACTION %d", (int)action);

	/* An item of File > Open Recent opens its file (ws128-p003). */
	if ((unsigned)action >= TE_ACTION_RECENT_FIRST && (unsigned)action < TE_ACTION_RECENT_FIRST + TE_RECENT_MAX) {
		app_recent(app, (unsigned)action - TE_ACTION_RECENT_FIRST);
		app->dirty = 1;
		return;
	}

	/* The action. */
	switch (action) {
	case TE_ACTION_NEW:
		app_request(app, TE_AFTER_NEW);
		break;
	case TE_ACTION_OPEN:
		app_request(app, TE_AFTER_OPEN);
		break;
	case TE_ACTION_SAVE:
		app_save(app);
		break;
	case TE_ACTION_SAVE_AS:
		app_choose(app, 1);
		break;
	case TE_ACTION_CLOSE:
	case TE_ACTION_QUIT:
		/* Quit closes a dialog first (the chooser's answer is then ignored), then asks like Close. */
		app->dialog = TE_DIALOG_NONE;
		app->choosing = 0;
		app_request(app, TE_AFTER_CLOSE);
		break;
	case TE_ACTION_UNDO:
		te_edit_undo(app);
		break;
	case TE_ACTION_REDO:
		te_edit_redo(app);
		break;
	case TE_ACTION_CUT:
		te_edit_cut(app);
		break;
	case TE_ACTION_COPY:
		te_edit_copy(app);
		break;
	case TE_ACTION_PASTE:
		te_edit_paste(app, 0);
		break;
	case TE_ACTION_SELECT_ALL:
		te_edit_select_all(app);
		break;
	case TE_ACTION_FIND:
		/* The Find panel (BUG-248: the menu bar has no find field; main.c fills its field the first frame). */
		app->panel_fresh = 1;
		app_dialog(app, TE_DIALOG_FIND);
		break;
	case TE_ACTION_FIND_NEXT:
		te_edit_find(app, 1, 0);
		break;
	case TE_ACTION_FIND_PREVIOUS:
		te_edit_find(app, 0, 0);
		break;
	case TE_ACTION_REPLACE:
		/* The Replace panel (main.c fills its fields the first frame). */
		app->panel_fresh = 1;
		app_dialog(app, TE_DIALOG_REPLACE);
		break;
	case TE_ACTION_LINE_NUMBERS:
		app->line_numbers = !app->line_numbers;
		te_app_relayout(app);
		break;
	case TE_ACTION_WORD_WRAP:
		/* The rows change: the view keeps the cursor. */
		app->wrap = !app->wrap;
		app->scroll_x = 0.0;
		te_app_relayout(app);
		te_edit_reveal(app);
		break;
	case TE_ACTION_BIGGER:
		app_size(app, 1);
		break;
	case TE_ACTION_SMALLER:
		app_size(app, -1);
		break;
	case TE_ACTION_ACTUAL_SIZE:
		app_size(app, 0);
		break;
	case TE_ACTION_ABOUT:
		app_dialog(app, TE_DIALOG_ABOUT);
		break;
	case TE_ACTION_NONE:
		break;
	}

	/* The frame shows what the action did. */
	app->dirty = 1;
}

/*
 * Moves time on: the cursor's blink, the wheel's glide, a message's end,
 * and a selection dragged past the edge.  Reports in how many milliseconds
 * something is due again (-1 for nothing).
 */
int
te_app_tick(
	struct te_app *app,
	uint64_t now)
{
	uint64_t since;
	int moving;
	int due;
	int wait;

	/* The time, and nothing due yet. */
	app->now = now;
	due = -1;

	/* A message that ran its time goes. */
	if (app->message[0] != '\0') {
		if (now >= app->message_until) {
			app->message[0] = '\0';
			app->dirty = 1;
		} else {
			due = (int)(app->message_until - now);
		}
	}

	/* The view where its scroll has it (the wheel's glide, the fingers' flight); a moving view wants frames. */
	moving = te_app_sync_scroll(app, now * 1000U);
	if (moving && (due < 0 || due > APP_FRAME_MS))
		due = APP_FRAME_MS;

	/* A selection dragged past the edge scrolls the view and follows the pointer. */
	if (app->selecting) {
		app_drag(app);
		if (due < 0 || due > APP_FRAME_MS)
			due = APP_FRAME_MS;
	}

	/* The cursor blinks while the keyboard is the window's and no dialog or chooser covers it. */
	if (app->focused && app->dialog == TE_DIALOG_NONE && !app->choosing) {
		since = now - app->blink_start;
		wait = (int)(APP_BLINK_MS - since % APP_BLINK_MS);
		if (since % APP_BLINK_MS < APP_FRAME_MS)
			app->dirty = 1;
		if (due < 0 || wait < due)
			due = wait;
	}

	/* Succeeded: when something is due next. */
	return due;
}

/*
 * Shows a message at the bottom of the frame for a while.
 */
void
te_app_message(
	struct te_app *app,
	const char *message)
{
	/* The message and its end. */
	snprintf(app->message, sizeof(app->message), "%s", message);
	app->message_until = app->now + APP_MESSAGE_MS;
	app->dirty = 1;
	te_log("MESSAGE %s", message);
}

/*
 * Lays the whole text out again for the frame's width and the text's
 * size, keeping the view within the text.
 */
void
te_app_relayout(
	struct te_app *app)
{
	unsigned columns;
	int error;

	/* The columns the text's width holds. */
	columns = app_columns(app);

	/* The rows; without memory the text keeps the rows it had. */
	error = te_layout_reset(&app->layout, &app->buffer, app->wrap, columns);
	if (error != 0)
		te_log("LAYOUT failed error=%d", error);
	te_app_clamp(app);
	app->dirty = 1;
}

/*
 * Keeps the view's place within the text.
 */
void
te_app_clamp(
	struct te_app *app)
{
	struct te_rect text;
	double largest_x;
	double largest_y;

	/* Down no further than the last row at the bottom. */
	largest_y = te_app_max_scroll_y(app);
	if (app->scroll_y > largest_y)
		app->scroll_y = largest_y;
	if (app->scroll_y < 0.0)
		app->scroll_y = 0.0;

	/* Across no further than the widest line (never, with wrapping). */
	largest_x = te_app_max_scroll_x(app);
	if (app->scroll_x > largest_x)
		app->scroll_x = largest_x;
	if (app->scroll_x < 0.0)
		app->scroll_x = 0.0;

	/* The scroll's sizes: the text's viewport and as much more as it scrolls. */
	te_app_text_rect(app, &text);
	kl_scroll_set_size(&app->scroll, (double)text.width + largest_x, (double)text.height + largest_y, (double)text.width, (double)text.height);

	/* A place the editor chose (a reveal, a drag past the edge, a new file) moves the scroll there at once. */
	if (app->scroll_x != app->scroll.x || app->scroll_y != app->scroll.y)
		kl_scroll_move_to(&app->scroll, app->scroll_x, app->scroll_y, 0, app->now * 1000U);
}

/*
 * Moves the view's scroll on to a time and takes its place as the place
 * drawn.  Returns 1 while it moves by itself (the wheel's glide, the
 * fingers' flight).
 */
int
te_app_sync_scroll(
	struct te_app *app,
	uint64_t now_us)
{
	int moving;

	/* The scroll at the time. */
	moving = kl_scroll_step(&app->scroll, now_us);

	/* A new place is drawn. */
	if (app->scroll.x != app->scroll_x || app->scroll.y != app->scroll_y) {
		app->scroll_x = app->scroll.x;
		app->scroll_y = app->scroll.y;
		app->dirty = 1;
	}

	/* Reports whether it moves on. */
	return moving;
}

/*
 * Takes what the fingers did to the text (libkeiland's text view touch): the
 * selection they made, and a long press's context menu.
 */
void
te_app_touch(
	struct te_app *app)
{
	unsigned changes;

	/* What changed. */
	changes = kl_text_touch_take(&app->touch);

	/* The fingers' selection becomes the editor's. */
	if (changes != 0U)
		te_log("TOUCH changes=%u anchor=%lu caret=%lu handles=%d", changes, (unsigned long)app->touch.anchor, (unsigned long)app->touch.caret, app->touch.handles);
	if ((changes & KL_TEXT_TOUCH_SELECTION) != 0U) {
		te_edit_select(app, app->touch.anchor, app->touch.caret);
		app->dirty = 1;
	}

	/* The context menu at the finger. */
	if ((changes & KL_TEXT_TOUCH_MENU) != 0U && app->host.context_menu != NULL)
		app->host.context_menu(app->host.data, (int)app->touch.menu_x, (int)app->touch.menu_y);

	/* The handles came or went (a drag's end, a key's selection): the frame shows it. */
	if (app->touch.handles != app->handles_shown) {
		app->handles_shown = app->touch.handles;
		app->dirty = 1;
	}

	/* The bar came or went (ws190-p003): the fingers' input lays it out, the frame draws it. */
	if ((changes & KL_TEXT_TOUCH_BAR) != 0U)
		app->dirty = 1;
}

/*
 * Gives the rectangle the text is drawn in (right of the line numbers).
 */
void
te_app_text_rect(
	const struct te_app *app,
	struct te_rect *rect)
{
	struct te_rect card;
	int gutter;

	/* Inside the card, past the line numbers. */
	te_app_card(app, &card);
	gutter = app_gutter(app);
	rect->x = card.x + TE_TEXT_SIDE + gutter;
	rect->y = card.y + TE_TEXT_TOP;
	rect->width = card.x + card.width - TE_TEXT_SIDE - rect->x;
	rect->height = card.y + card.height - TE_TEXT_TOP - rect->y;

	/* Never less than a cell and a row. */
	if (rect->width < app->cell)
		rect->width = app->cell;
	if (rect->height < app->row_height)
		rect->height = app->row_height;
}

/*
 * Gives the card the window's content is drawn on.
 */
void
te_app_card(
	const struct te_app *app,
	struct te_rect *rect)
{
	/* The window, less the inset (none, see TE_CARD_INSET) on every side. */
	rect->x = TE_CARD_INSET;
	rect->y = TE_CARD_INSET;
	rect->width = app->width - 2 * TE_CARD_INSET;
	rect->height = app->height - 2 * TE_CARD_INSET;
}

/*
 * Reports how far the view may scroll across (0 with wrapping).
 */
double
te_app_max_scroll_x(
	const struct te_app *app)
{
	struct te_rect text;
	double largest;

	/* Wrapped lines never reach across. */
	if (app->wrap)
		return 0.0;

	/* The widest line and a cell for the cursor, past the text's width. */
	te_app_text_rect(app, &text);
	largest = (double)(app->layout.widest + 1U) * (double)app->cell - (double)text.width;
	if (largest < 0.0)
		largest = 0.0;

	/* Succeeded: the largest offset. */
	return largest;
}

/*
 * Reports how far the view may scroll down: the last row at the bottom.
 */
double
te_app_max_scroll_y(
	const struct te_app *app)
{
	struct te_rect text;
	double largest;

	/* The rows' height past the text's. */
	te_app_text_rect(app, &text);
	largest = (double)app->layout.total * (double)app->row_height - (double)text.height;
	if (largest < 0.0)
		largest = 0.0;

	/* Succeeded: the largest offset. */
	return largest;
}

/*
 * Reports the document's name: its file's name, or Untitled.
 */
const char *
te_app_name(
	const struct te_app *app)
{
	const char *slash;

	/* No file yet. */
	if (app->path[0] == '\0')
		return "Untitled";

	/* The part after the last slash. */
	slash = strrchr(app->path, '/');
	if (slash == NULL)
		return app->path;

	/* Succeeded: the name. */
	return slash + 1;
}

/*
 * Reports whether the document has changes that are not saved.
 */
int
te_app_modified(
	const struct te_app *app)
{
	int modified;

	/* The history knows. */
	modified = te_undo_modified(&app->undo);

	/* Succeeded: whether it has. */
	return modified;
}

/*
 * Makes a new selection the primary selection (once per round of input).
 */
void
te_app_publish_primary(
	struct te_app *app)
{
	size_t start;
	size_t end;
	char *text;

	/* Only a changed, non-empty selection, and only with somewhere to publish it. */
	if (!app->primary_changed)
		return;
	app->primary_changed = 0;
	te_edit_selection(app, &start, &end);
	if (end <= start || app->host.select == NULL)
		return;
	if (end - start > TE_FILE_MAX)
		end = start + TE_FILE_MAX;

	/* The selected text, which the window keeps its own copy of. */
	text = malloc(end - start);
	if (text == NULL)
		return;
	te_buffer_copy(&app->buffer, start, end, text);
	app->host.select(app->host.data, text, end - start);
	free(text);
}

/*
 * Follows a drag of text from elsewhere over the window (ws189-p003): the
 * position a drop would insert at, shown by a caret.  Returns 1 when the
 * text takes the drop there, 0 when it does not (outside the text, under
 * a dialog, or the window's own drag).
 */
int
te_app_drop_over(
	struct te_app *app,
	int x,
	int y)
{
	struct te_rect card;
	size_t position;
	int inside;

	/* Not under a dialog or the chooser, nor the window's own drag (it does not move text yet). */
	if (app->dialog != TE_DIALOG_NONE || app->choosing || app->dragging_out) {
		te_app_drop_leave(app);
		return 0;
	}

	/* Only over the text's card. */
	te_app_card(app, &card);
	inside = app_inside(&card, x, y);
	if (!inside) {
		te_app_drop_leave(app);
		return 0;
	}

	/* The position under the pointer, drawn again when it moved. */
	position = te_edit_position_at(app, x, y);
	if (!app->drop_over || position != app->drop_position)
		app->dirty = 1;
	app->drop_over = 1;
	app->drop_position = position;

	/* Succeeded: the drop is taken there. */
	return 1;
}

/*
 * The drag of text left the window, or is taken nowhere: its caret goes.
 */
void
te_app_drop_leave(
	struct te_app *app)
{
	/* Nothing shown. */
	if (!app->drop_over)
		return;

	/* The caret goes. */
	app->drop_over = 0;
	app->dirty = 1;
}

/*
 * Inserts dropped text where the drag's caret was, as one edit, and
 * selects it.
 */
void
te_app_drop_text(
	struct te_app *app,
	const char *text,
	size_t length)
{
	size_t position;

	/* Only a drop over the text. */
	if (!app->drop_over)
		return;

	/* The caret's place is taken, and the caret goes. */
	position = app->drop_position;
	app->drop_over = 0;

	/* The text at the position, one step of undo, then selected. */
	te_edit_select(app, position, position);
	te_edit_insert_text(app, text, length, TE_MERGE_NONE);
	te_edit_select(app, position, position + length);
	app->primary_changed = 1;
	app->dirty = 1;
}

/*
 * The window's own drag of text ended (dropped or not).
 */
void
te_app_drag_done(
	struct te_app *app)
{
	/* Neither a drag nor a press waits any more. */
	app->dragging_out = 0;
	app->drag_armed = 0;
	app->selecting = 0;
}

/*
 * Reports where the drag's caret is drawn: a row tall at the drop's
 * position, in the window.
 */
void
te_app_drop_rect(
	const struct te_app *app,
	struct te_rect *rect)
{
	struct kl_rect caret;
	struct te_rect text;

	/* The position's caret in the text's content, moved by the view's place and scroll. */
	app_view_caret((void *)app, app->drop_position, &caret);
	te_app_text_rect(app, &text);
	rect->x = text.x + caret.x - (int)app->scroll_x;
	rect->y = text.y + caret.y - (int)app->scroll_y;
	rect->width = caret.width;
	rect->height = caret.height;
}

/*
 * A finger tapped (count 1) or tapped twice (count 2): the pointer's click
 * there, and for a double tap in the text, the word there selected.
 */
void
te_app_tap(
	struct te_app *app,
	int x,
	int y,
	int count)
{
	struct te_event event;
	struct te_rect text;
	size_t position;
	size_t start;
	size_t end;
	int inside;

	/* A double tap in the text selects the word there. */
	te_app_text_rect(app, &text);
	inside = app_inside(&text, x, y);
	if (count == 2 && inside && app->dialog == TE_DIALOG_NONE) {
		position = te_edit_position_at(app, x, y);
		te_edit_word(app, position, &start, &end);
		te_edit_select(app, start, end);
		return;
	}

	/* Otherwise a click: the pointer comes, presses and lets go. */
	memset(&event, 0, sizeof(event));
	event.x = x;
	event.y = y;
	event.button = TE_BUTTON_LEFT;
	event.type = TE_EVENT_MOTION;
	te_app_event(app, &event);
	event.type = TE_EVENT_BUTTON;
	event.pressed = 1;
	te_app_event(app, &event);
	event.pressed = 0;
	te_app_event(app, &event);
}

/* Measures the body's text at its size: the cell's width, the row's height and the baseline. */
static void
app_measure(
	struct te_app *app)
{
	struct te_text_line line;
	int advance;

	/* The font's line at the size, with some room between rows. */
	te_text_metrics(app->body, app->pixels, &line);
	app->ascent = line.ascent + 2;
	app->row_height = line.ascent + line.descent + 5;
	if (app->row_height < (int)app->pixels + 2)
		app->row_height = (int)app->pixels + 2;

	/* A cell is the monospaced font's advance (a guess from the size without a font). */
	advance = te_text_advance(app->body, '0', app->pixels);
	app->cell = advance;
	if (advance <= 0)
		app->cell = (int)(app->pixels * 3U / 5U);
	if (app->cell < 1)
		app->cell = 1;
}

/* Reports the width of the line numbers' column (0 without them). */
static int
app_gutter(
	const struct te_app *app)
{
	size_t lines;
	int digits;

	/* No column without line numbers. */
	if (!app->line_numbers)
		return 0;

	/* As many digits as the last line's number has, at least three. */
	digits = 1;
	for (lines = app->buffer.line_count; lines >= 10U; lines /= 10U)
		digits++;
	if (digits < APP_DIGITS_MIN)
		digits = APP_DIGITS_MIN;

	/* Succeeded: the digits and a margin. */
	return digits * app->cell + TE_GUTTER_PAD;
}

/* Reports how many columns the text's width holds. */
static unsigned
app_columns(
	const struct te_app *app)
{
	struct te_rect text;
	unsigned columns;

	/* The whole cells across the text. */
	te_app_text_rect(app, &text);
	columns = (unsigned)(text.width / app->cell);
	if (columns < APP_COLUMNS_MIN)
		columns = APP_COLUMNS_MIN;

	/* Succeeded: the columns. */
	return columns;
}

/* Lays the text out again when its width changed (the line numbers grew a digit). */
static void
app_fit(
	struct te_app *app)
{
	unsigned columns;

	/* Only wrapped text depends on the width. */
	if (!app->wrap)
		return;

	/* The same columns keep the rows. */
	columns = app_columns(app);
	if (columns == app->layout.columns)
		return;

	/* The rows for the new width, the cursor still in view. */
	te_app_relayout(app);
	te_edit_reveal(app);
}

/* Puts a text (NULL for none) in place of the document, with a new history and the view at its start. */
static void
app_replace(
	struct te_app *app,
	char *text,
	size_t length)
{
	struct te_buffer buffer;
	int error;

	/* The new document; without memory the old one stays. */
	if (text == NULL)
		text = "";
	error = te_buffer_init(&buffer, text, length);
	if (error != 0) {
		te_app_message(app, "Not enough memory to open this file.");
		return;
	}

	/* It replaces the old one, with an empty history. */
	te_buffer_free(&app->buffer);
	app->buffer = buffer;
	te_undo_free(&app->undo);
	te_undo_init(&app->undo);

	/* The cursor and the view at the start, the rows laid out. */
	app->cursor = 0;
	app->anchor = 0;
	app->goal_valid = 0;
	app->scroll_x = 0.0;
	app->scroll_y = 0.0;
	te_app_relayout(app);
	app->title_changed = 1;
	app->dirty = 1;
}

/* Takes the pointer's input: over a dialog, or in the text. */
static void
app_pointer(
	struct te_app *app,
	const struct te_event *event)
{
	/* Where the pointer is. */
	app->pointer_x = event->x;
	app->pointer_y = event->y;

	/* A dialog takes the pointer (libkeiland's, through main.c). */
	if (app->dialog != TE_DIALOG_NONE)
		return;

	/* The pointer left: a drag in progress ends. */
	if (event->type == TE_EVENT_LEAVE)
		return;

	/* A drag follows the pointer; a press in the selection that moves far enough drags its text out. */
	if (event->type == TE_EVENT_MOTION) {
		if (app->drag_armed)
			app_drag_out(app);
		if (app->selecting)
			app_drag(app);
		return;
	}

	/* A press in the selection let go without moving places the cursor there, as a click does. */
	if (!event->pressed && app->drag_armed && event->button == TE_BUTTON_LEFT) {
		app->drag_armed = 0;
		te_edit_select(app, app->drag_position, app->drag_position);
		app->dirty = 1;
		return;
	}

	/* A release ends a drag; its selection is offered as the primary one. */
	if (!event->pressed) {
		if (app->selecting && event->button == TE_BUTTON_LEFT) {
			app->selecting = 0;
			app->primary_changed = 1;
		}

		/* A release does nothing more. */
		return;
	}

	/* A press. */
	app_press(app, event);
}

/*
 * A press in the text: the left button places the cursor (Shift extends
 * the selection; twice selects a word, three times a line) and starts a
 * drag; the middle one pastes the primary selection there; the right one
 * opens the context menu.
 */
static void
app_press(
	struct te_app *app,
	const struct te_event *event)
{
	struct te_rect card;
	size_t position;
	size_t start;
	size_t end;
	int distance_x;
	int distance_y;
	int near;
	int inside;

	/* Only in the card. */
	te_app_card(app, &card);
	inside = app_inside(&card, event->x, event->y);
	if (!inside)
		return;
	position = te_edit_position_at(app, event->x, event->y);

	/* The middle button pastes the primary selection where it presses. */
	if (event->button == TE_BUTTON_MIDDLE) {
		te_edit_select(app, position, position);
		te_edit_paste(app, 1);
		return;
	}

	/* The right button: the context menu (over the selection, or with the cursor moved there). */
	if (event->button == TE_BUTTON_RIGHT) {
		te_edit_selection(app, &start, &end);
		if (position < start || position > end || start == end)
			te_edit_select(app, position, position);
		if (app->host.context_menu != NULL)
			app->host.context_menu(app->host.data, event->x, event->y);
		return;
	}

	/* Only the left button selects. */
	if (event->button != TE_BUTTON_LEFT)
		return;

	/* A click soon after one near it counts with it (up to three). */
	distance_x = abs(event->x - app->click_x);
	distance_y = abs(event->y - app->click_y);
	near = 0;
	if (distance_x <= APP_CLICK_DISTANCE && distance_y <= APP_CLICK_DISTANCE)
		near = 1;
	if (near && app->now - app->click_time <= APP_CLICK_MS && app->click_count < 3)
		app->click_count++;
	else
		app->click_count = 1;
	app->click_time = app->now;
	app->click_x = event->x;
	app->click_y = event->y;

	/* One click in the selection may become a drag of its text (decided by the moves, ws189-p003). */
	te_edit_selection(app, &start, &end);
	if (app->click_count == 1 &&
	    (event->modifiers & TE_MOD_SHIFT) == 0U &&
	    start < end &&
	    position >= start &&
	    position < end &&
	    app->host.drag_text != NULL) {
		app->drag_armed = 1;
		app->drag_press_x = event->x;
		app->drag_press_y = event->y;
		app->drag_position = position;
		return;
	}

	/* One click places the cursor (Shift extends the selection); two a word; three a line. */
	app->select_unit = APP_UNIT_CHARACTER;
	start = position;
	end = position;
	if (app->click_count == 2) {
		app->select_unit = APP_UNIT_WORD;
		te_edit_word(app, position, &start, &end);
	} else if (app->click_count == 3) {
		app->select_unit = APP_UNIT_LINE;
		te_edit_line(app, position, &start, &end);
	}

	/* The selection, and what a drag keeps selected. */
	if (app->click_count == 1 && (event->modifiers & TE_MOD_SHIFT) != 0U) {
		te_edit_select(app, app->anchor, position);
		app->select_start = app->anchor;
		app->select_end = app->anchor;
	} else {
		te_edit_select(app, start, end);
		app->select_start = start;
		app->select_end = end;
	}

	/* The pointer's moves drag the selection until the release. */
	app->selecting = 1;
}

/* Drags the selection's text out of the window once a press in it has moved far enough (ws189-p003). */
static void
app_drag_out(
	struct te_app *app)
{
	size_t start;
	size_t end;
	char *text;
	int distance_x;
	int distance_y;
	int error;

	/* Not far enough yet. */
	distance_x = abs(app->pointer_x - app->drag_press_x);
	distance_y = abs(app->pointer_y - app->drag_press_y);
	if (distance_x <= APP_DRAG_DISTANCE && distance_y <= APP_DRAG_DISTANCE)
		return;

	/* The press is spent, whatever comes of the drag. */
	app->drag_armed = 0;
	te_edit_selection(app, &start, &end);
	if (end <= start)
		return;

	/* A selection past what a file holds is cut short. */
	if (end - start > TE_FILE_MAX)
		end = start + TE_FILE_MAX;

	/* The selected text, which the window keeps its own copy of. */
	text = malloc(end - start);
	if (text == NULL)
		return;
	te_buffer_copy(&app->buffer, start, end, text);

	/* The drag; while it goes on, the window does not take its own text. */
	error = app->host.drag_text(app->host.data, text, end - start);
	free(text);
	if (error == 0)
		app->dragging_out = 1;
}

/* Extends the selection being dragged to the pointer, scrolling when it is past the text's edge. */
static void
app_drag(
	struct te_app *app)
{
	struct te_rect text;
	size_t position;
	size_t start;
	size_t end;
	double step;

	/* Past the top or the bottom, the view scrolls by a share of the distance. */
	te_app_text_rect(app, &text);
	step = 0.0;
	if (app->pointer_y < text.y)
		step = (double)(app->pointer_y - text.y) / 3.0;
	if (app->pointer_y > text.y + text.height)
		step = (double)(app->pointer_y - text.y - text.height) / 3.0;
	if (step != 0.0) {
		app->scroll_y += step;
		te_app_clamp(app);
		app->dirty = 1;
	}

	/* The place under the pointer, grown to the word or the line the drag selects by. */
	position = te_edit_position_at(app, app->pointer_x, app->pointer_y);
	start = position;
	end = position;
	if (app->select_unit == APP_UNIT_WORD)
		te_edit_word(app, position, &start, &end);
	if (app->select_unit == APP_UNIT_LINE)
		te_edit_line(app, position, &start, &end);

	/* The selection from what the press selected to the pointer, either way. */
	if (start < app->select_start) {
		app->anchor = app->select_end;
		app->cursor = start;
	} else {
		app->anchor = app->select_start;
		app->cursor = end;
		if (end < app->select_end)
			app->cursor = app->select_end;
	}

	/* The cursor shows where the drag is. */
	app->goal_valid = 0;
	app->blink_start = app->now;
	app->dirty = 1;
}

/* The wheel: Control changes the text's size, and otherwise the view glides. */
static void
app_wheel(
	struct te_app *app,
	const struct te_event *event)
{
	/* A dialog keeps the view still. */
	if (app->dialog != TE_DIALOG_NONE)
		return;

	/* Control and the wheel change the text's size. */
	if ((event->modifiers & TE_MOD_CTRL) != 0U) {
		if (event->scroll < 0)
			app_size(app, 1);
		if (event->scroll > 0)
			app_size(app, -1);
		return;
	}

	/* A touch pad's fingers move the view with them, and it flies on when they lift (libkeiland's scroller, ws090-p019). */
	if (event->axis_source == KL_AXIS_SOURCE_FINGER) {
		kl_scroll_axis(&app->scroll, event->axis_dx, event->axis_dy, KL_AXIS_SOURCE_FINGER, event->axis_us);
		app->dirty = 1;
		return;
	}

	/* Otherwise the view glides to the new place, within the text (libkeiland's scroll). */
	kl_scroll_wheel(&app->scroll, (double)event->scroll_x, (double)event->scroll, app->now * 1000U);
	app->dirty = 1;
}

/* The touch pad's fingers lift: the view flies on at their velocity (libkeiland's scroller, ws090-p019). */
static void
app_axis_stop(
	struct te_app *app,
	const struct te_event *event)
{
	int flung;

	/* The view the fingers held, thrown. */
	flung = kl_scroll_axis_stop(&app->scroll, event->axis_us);
	app->dirty = 1;
	if (flung)
		te_log("KINETIC fling source=finger");
}

/* Takes a key: a dialog's, F3, or the text's. */
static void
app_key(
	struct te_app *app,
	const struct te_event *event)
{
	int shift;

	/* Only presses. */
	if (!event->pressed)
		return;

	/* A dialog takes the keys (libkeiland's, through main.c). */
	if (app->dialog != TE_DIALOG_NONE)
		return;

	/* F3 finds again (Shift: backward). */
	if (event->key == TE_KEY_F3) {
		shift = 0;
		if ((event->modifiers & TE_MOD_SHIFT) != 0U)
			shift = 1;
		te_edit_find(app, !shift, 0);
		return;
	}

	/* Anything else is the text's. */
	(void)te_edit_key(app, event);
	app->blink_start = app->now;
	app->dirty = 1;
}

/*
 * Asks for something that replaces the document or closes the window: with
 * unsaved changes a dialog asks first; otherwise it happens now.
 */
static void
app_request(
	struct te_app *app,
	enum te_after after)
{
	int modified;

	/* What waits. */
	app->after = after;

	/* Unsaved changes are asked about first. */
	modified = te_app_modified(app);
	if (modified) {
		app_dialog(app, TE_DIALOG_UNSAVED);
		return;
	}

	/* Nothing to lose: it happens. */
	app_after(app);
}

/* Does what waited for the unsaved changes to be saved or dropped. */
static void
app_after(
	struct te_app *app)
{
	enum te_after after;

	/* What waited, once. */
	after = app->after;
	app->after = TE_AFTER_NOTHING;

	/* Carries it out. */
	switch (after) {
	case TE_AFTER_CLOSE:
		app->want_close = 1;
		break;
	case TE_AFTER_NEW:
		te_app_new(app);
		break;
	case TE_AFTER_OPEN:
		app_choose(app, 0);
		break;
	case TE_AFTER_OPEN_PATH:
		/* A file of Open Recent. */
		(void)te_app_open(app, app->open_path);
		break;
	case TE_AFTER_NOTHING:
		break;
	}
}

/* File > Save: to the document's file, asking first when the file changed on the disk; without a file, Save As. */
static void
app_save(
	struct te_app *app)
{
	int changed;

	/* Untitled: Save As. */
	if (app->path[0] == '\0') {
		app_choose(app, 1);
		return;
	}

	/* A file changed on the disk since it was read: overwrite it? */
	changed = te_file_changed(app->path, &app->file);
	if (changed) {
		app_dialog(app, TE_DIALOG_CHANGED);
		return;
	}

	/* The save. */
	(void)te_app_save(app, NULL);
}

/* Writes the document to a path, which becomes its file; returns 0 or an errno value. */
static int
app_save_to(
	struct te_app *app,
	const char *path)
{
	char resolved[TE_PATH_MAX];
	const char *name;
	int same;
	int error;

	/* A new path is a new file (its line ends and mark stay the document's). */
	snprintf(resolved, sizeof(resolved), "%s", path);
	same = strcmp(resolved, app->path);
	if (same != 0) {
		app->file.exists = 0;
		app->file.mode = 0;
	}

	/* The text into the file. */
	error = te_file_write(resolved, &app->buffer, &app->file);
	if (error != 0) {
		name = strrchr(resolved, '/');
		if (name == NULL)
			name = resolved;
		else
			name++;
		app_error_message(app, "Can't save", name, error);
		app->after = TE_AFTER_NOTHING;
		return error;
	}

	/* The file is the document's now, and saved. */
	snprintf(app->path, sizeof(app->path), "%s", resolved);
	te_undo_mark_saved(&app->undo);
	app->opened = 1;
	app->title_changed = 1;
	te_log("SAVE path=%s bytes=%lu", app->path, (unsigned long)te_buffer_length(&app->buffer));
	te_app_message(app, "Saved");

	/* Succeeded: saved. */
	return 0;
}

/* Changes the body's text size by a step (0: back to the default), keeping the cursor in view. */
static void
app_size(
	struct te_app *app,
	int step)
{
	unsigned pixels;

	/* The new size, within its bounds. */
	pixels = TE_PIXELS_DEFAULT;
	if (step > 0)
		pixels = app->pixels + 1U;
	if (step < 0)
		pixels = app->pixels - 1U;
	if (pixels < TE_PIXELS_MIN)
		pixels = TE_PIXELS_MIN;
	if (pixels > TE_PIXELS_MAX)
		pixels = TE_PIXELS_MAX;

	/* The rows at the new size. */
	app->pixels = pixels;
	app_measure(app);
	te_app_relayout(app);
	te_edit_reveal(app);
}

/* Shows a dialog, its default button under the keyboard. */
static void
app_dialog(
	struct te_app *app,
	enum te_dialog dialog)
{
	/* The dialog. */
	app->dialog = dialog;
	app->selecting = 0;
	app->dirty = 1;
	te_log("DIALOG %d", (int)dialog);
}

/* Opens a file of File > Open Recent, after unsaved changes are dealt with; one no longer there says so. */
static void
app_recent(
	struct te_app *app,
	unsigned index)
{
	char message[TE_PATH_MAX + 48];

	/* An item past the list does nothing. */
	if (index >= app->recent_count)
		return;

	/* A file that is gone is not opened. */
	if (!app->recent_present[index]) {
		snprintf(message, sizeof(message), "\"%s\" is no longer there.", app->recent[index]);
		te_app_message(app, message);
		return;
	}

	/* The file waits for the unsaved changes, as File > Open does. */
	snprintf(app->open_path, sizeof(app->open_path), "%s", app->recent[index]);
	te_log("RECENT open index=%u path=%s", index, app->open_path);
	app_request(app, TE_AFTER_OPEN_PATH);
}

/*
 * Carries out a dialog's button (the first is the default, the last
 * cancels).
 */
void
te_app_dialog_choose(
	struct te_app *app,
	int button)
{
	enum te_dialog dialog;

	/* The dialog closes. */
	dialog = app->dialog;
	app->dialog = TE_DIALOG_NONE;
	app->dirty = 1;
	te_log("DIALOG choose=%d", button);

	/* What the button means in that dialog. */
	switch (dialog) {
	case TE_DIALOG_UNSAVED:
		/* Save (as, for Untitled), Don't Save, or Cancel. */
		if (button == APP_BUTTON_FIRST) {
			if (app->path[0] == '\0')
				app_choose(app, 1);
			else
				(void)te_app_save(app, NULL);
		} else if (button == APP_BUTTON_SECOND) {
			app_after(app);
		} else {
			app->after = TE_AFTER_NOTHING;
		}

		/* The unsaved changes are dealt with. */
		break;
	case TE_DIALOG_CHANGED:
		/* Overwrite the file changed on the disk, or keep it. */
		if (button == APP_BUTTON_FIRST)
			(void)te_app_save(app, NULL);
		else
			app->after = TE_AFTER_NOTHING;
		break;
	case TE_DIALOG_ABOUT:
	case TE_DIALOG_REPLACE:
	case TE_DIALOG_FIND:
	case TE_DIALOG_NONE:
		break;
	}
}

/* Reports how many buttons the shown dialog has. */
static int
app_dialog_buttons(
	const struct te_app *app)
{
	/* Unsaved changes: Save, Don't Save, Cancel. */
	if (app->dialog == TE_DIALOG_UNSAVED)
		return 3;

	/* About: OK. */
	if (app->dialog == TE_DIALOG_ABOUT)
		return 1;

	/* The others: the action and Cancel. */
	return 2;
}

/*
 * Carries out the Replace panel's Replace (all 0) or Replace All (1)
 * (ws128-p003): the find text becomes the editor's (as the Find panel's
 * sets it), the replacement is kept for the next time, and a message says
 * what was done.
 */
void
te_app_replace(
	struct te_app *app,
	const char *find,
	const char *with,
	int all)
{
	char message[TE_FIND_MAX + 64];
	size_t with_length;
	size_t count;
	int replaced;

	/* The find text, as long as the editor keeps, and the replacement. */
	snprintf(app->find, sizeof(app->find), "%s", find);
	app->find_length = strlen(app->find);
	snprintf(app->replace_with, sizeof(app->replace_with), "%s", with);
	with_length = strlen(app->replace_with);

	/* Nothing to find: the panel says so. */
	if (app->find_length == 0U) {
		te_app_message(app, "Type the text to find.");
		return;
	}

	/* Replace All: every place, as one undo group. */
	if (all) {
		count = te_edit_replace_all(app, app->replace_with, with_length);
		te_log("REPLACE all count=%lu", (unsigned long)count);
		if (count == 0U) {
			snprintf(message, sizeof(message), "No matches for \"%s\"", app->find);
		} else if (count == 1U) {
			snprintf(message, sizeof(message), "Replaced 1 place");
		} else {
			snprintf(message, sizeof(message), "Replaced %lu places", (unsigned long)count);
		}

		/* The message, at the bottom of the card. */
		te_app_message(app, message);
		return;
	}

	/* Replace: the place found, then the next is selected. */
	replaced = te_edit_replace(app, app->replace_with, with_length);
	te_log("REPLACE one replaced=%d", replaced);
}

/*
 * Closes the Replace panel; the place found stays selected.
 */
void
te_app_replace_close(
	struct te_app *app)
{
	/* The panel goes, and the text has the keyboard again. */
	app->dialog = TE_DIALOG_NONE;
	app->dirty = 1;
	te_log("REPLACE closed");
}

/*
 * Takes the Find panel's text as it is typed (BUG-248): kept for Find Next
 * (cut at a character's start when it is too long), and found from where
 * the selection starts; an empty text only clears the marks.
 */
void
te_app_find_text(
	struct te_app *app,
	const char *text)
{
	size_t length;

	/* As much of the text as the editor keeps, ending at a character's start. */
	length = strlen(text);
	if (length >= sizeof(app->find)) {
		length = sizeof(app->find) - 1U;
		while (length > 0U && ((unsigned char)text[length] & 0xc0U) == 0x80U)
			length--;
	}

	/* The text copied. */
	memcpy(app->find, text, length);
	app->find[length] = '\0';
	app->find_length = length;

	/* Found as it is typed. */
	if (app->find_length != 0U)
		te_edit_find(app, 1, 1);
	app->dirty = 1;
}

/*
 * Closes the Find panel; the place found stays selected.
 */
void
te_app_find_close(
	struct te_app *app)
{
	/* The panel goes, and the text has the keyboard again. */
	app->dialog = TE_DIALOG_NONE;
	app->dirty = 1;
	te_log("FIND closed");
}

/*
 * Gives the shown dialog's title (written into title), its words, its
 * buttons' labels (the first the default, the last the one that cancels)
 * and how many there are.
 */
void
te_app_dialog_words(
	const struct te_app *app,
	char *title,
	size_t size,
	const char **words,
	const char *const **labels,
	int *count)
{
	static const char *const unsaved[] = { "Save", "Don't Save", "Cancel" };
	static const char *const changed[] = { "Overwrite", "Cancel" };
	static const char *const about[] = { "OK" };
	const char *name;

	/* The dialog's words and buttons. */
	name = te_app_name(app);
	*count = app_dialog_buttons(app);
	switch (app->dialog) {
	case TE_DIALOG_UNSAVED:
		snprintf(title, size, "Save changes to \"%s\"?", name);
		*words = "Your changes will be lost if you don't save them.";
		*labels = unsaved;
		break;
	case TE_DIALOG_CHANGED:
		snprintf(title, size, "\"%s\" changed on disk.", name);
		*words = "Overwrite it with the text here?";
		*labels = changed;
		break;
	default:
		snprintf(title, size, "Text Editor");
		*words = "A simple editor of plain text for Kei.";
		*labels = about;
		break;
	}
}

/*
 * Opens the file chooser (the window's) to open a file or to save as a
 * name, at the document's folder (or the home folder); its answer comes
 * back as a TE_EVENT_CHOSEN.
 */
static void
app_choose(
	struct te_app *app,
	int saving)
{
	char folder[TE_PATH_MAX];
	const char *home;
	const char *name;
	char *slash;
	int error;

	/* Without a chooser there is nothing to choose with. */
	if (app->host.choose == NULL) {
		te_app_message(app, "No file chooser is available.");
		app->after = TE_AFTER_NOTHING;
		return;
	}

	/* The document's folder, or home, or here. */
	snprintf(folder, sizeof(folder), "%s", app->path);
	slash = strrchr(folder, '/');
	if (slash == folder)
		folder[1] = '\0';
	else if (slash != NULL)
		*slash = '\0';
	if (slash == NULL) {
		home = getenv("HOME");
		if (home == NULL || home[0] == '\0')
			home = ".";
		snprintf(folder, sizeof(folder), "%s", home);
	}

	/* The name Save As starts with: the document's, or a new one. */
	name = te_app_name(app);
	if (app->path[0] == '\0')
		name = "Untitled.txt";

	/* The chooser; while it is open the editor waits for its answer. */
	error = app->host.choose(app->host.data, saving, folder, name);
	if (error != 0) {
		app_error_message(app, "Can't show the files of", folder, error);
		app->after = TE_AFTER_NOTHING;
		return;
	}

	/* The editor waits for the answer. */
	app->choosing = 1;
	app->choosing_save = saving;
	app->selecting = 0;
	te_log("CHOOSER saving=%d folder=%s", saving, folder);
}

/*
 * The file chooser answered: a path to open or to save to (the chooser has
 * already asked before replacing a file), or nothing when it was
 * cancelled.
 */
static void
app_chosen(
	struct te_app *app,
	const struct te_event *event)
{
	int saving;

	/* An answer nobody waits for (Quit came meanwhile) is dropped. */
	if (!app->choosing)
		return;
	saving = app->choosing_save;
	app->choosing = 0;
	app->dirty = 1;

	/* Cancelled: nothing waits any more. */
	if (event->text[0] == '\0') {
		app->after = TE_AFTER_NOTHING;
		return;
	}

	/* Save As saves to the path, and goes on with what waited. */
	if (saving) {
		(void)te_app_save(app, event->text);
		return;
	}

	/* Open opens it. */
	(void)te_app_open(app, event->text);
}

/* Tells whether a point is in a rectangle. */
static int
app_inside(
	const struct te_rect *rect,
	int x,
	int y)
{
	/* Left of it or above it. */
	if (x < rect->x || y < rect->y)
		return 0;

	/* Right of it or below it. */
	if (x >= rect->x + rect->width || y >= rect->y + rect->height)
		return 0;

	/* Inside. */
	return 1;
}

/* Shows why a file could not be opened, saved or listed. */
static void
app_error_message(
	struct te_app *app,
	const char *what,
	const char *name,
	int error)
{
	char message[160];

	/* The reasons said in words of their own, and the rest by the system's. */
	switch (error) {
	case EFBIG:
		snprintf(message, sizeof(message), "\"%s\" is too large to edit.", name);
		break;
	case EILSEQ:
		snprintf(message, sizeof(message), "\"%s\" isn't a plain text file.", name);
		break;
	case EISDIR:
		snprintf(message, sizeof(message), "\"%s\" is a folder.", name);
		break;
	default:
		snprintf(message, sizeof(message), "%s \"%s\": %s", what, name, strerror(error));
		break;
	}

	/* Shown. */
	te_app_message(app, message);
}

/* The fingers' answer: the position nearest a point of the text's content. */
static size_t
app_view_position(
	void *data,
	double x,
	double y)
{
	struct te_app *app;
	struct te_rect text;
	size_t position;

	/* The point in the window, where the editor finds positions. */
	app = data;
	te_app_text_rect(app, &text);
	position = te_edit_position_at(app, (int)(x - app->scroll_x) + text.x, (int)(y - app->scroll_y) + text.y);

	/* Reports the position. */
	return position;
}

/* The fingers' answer: the cursor's rectangle at a position, in the text's content. */
static void
app_view_caret(
	void *data,
	size_t position,
	struct kl_rect *rect)
{
	struct te_app *app;
	size_t row;
	size_t column;

	/* The row and the cell of the position. */
	app = data;
	te_layout_place(&app->layout, &app->buffer, position, &row, &column);

	/* The cursor's bar there, a row tall. */
	rect->x = (int)column * app->cell;
	rect->y = (int)row * app->row_height;
	rect->width = 2;
	rect->height = app->row_height;
}

/* The fingers' answer: the word around a position. */
static void
app_view_word(
	void *data,
	size_t position,
	size_t *start,
	size_t *end)
{
	/* The editor's word (a run of one kind of character). */
	te_edit_word(data, position, start, end);
}

/*
 * Reports the caret's rectangle in the window (a row tall), where an input
 * method's candidates and the text being composed are shown.
 */
void
te_app_caret_rect(
	const struct te_app *app,
	struct te_rect *rect)
{
	struct kl_rect caret;
	struct te_rect text;
	size_t row;
	size_t column;
	size_t offset;
	unsigned cells;

	/* The caret in the text's content, then moved by the view's place and scroll. */
	app_view_caret((void *)app, app->cursor, &caret);
	te_app_text_rect(app, &text);
	rect->x = text.x + caret.x - (int)app->scroll_x;
	rect->y = text.y + caret.y - (int)app->scroll_y;
	rect->width = caret.width;
	rect->height = caret.height;

	/* While text is composed, the place the input method's candidates go under: its segment or its caret. */
	if (app->preedit[0] == '\0')
		return;

	/* The cursor's row and column, and the composed text's length. */
	te_layout_place((struct te_layout *)&app->layout, &app->buffer, app->cursor, &row, &column);
	offset = strlen(app->preedit);
	if (app->preedit_begin >= 0 && (size_t)app->preedit_begin < offset)
		offset = (size_t)app->preedit_begin;
	cells = te_app_preedit_cells(app, (unsigned)column, offset);
	rect->x += (int)cells * app->cell;
}

/*
 * Counts the cells the composed text's first bytes take in the body when it
 * starts at a column (wide characters two, tabs to the next stop, as the
 * body's own characters).
 */
unsigned
te_app_preedit_cells(
	const struct te_app *app,
	unsigned column,
	size_t bytes)
{
	uint32_t codepoint;
	size_t length;
	size_t index;
	unsigned cells;

	/* Each character before the byte asked about. */
	length = strlen(app->preedit);
	if (bytes > length)
		bytes = length;
	cells = 0;
	index = 0;
	while (index < bytes) {
		codepoint = te_utf8_next(app->preedit, length, &index);
		cells += te_layout_cells(codepoint, column + cells, app->layout.tab);
	}

	/* The cells they take. */
	return cells;
}

/*
 * Takes text from the window's text input (an input method, the on-screen
 * keyboard, ws090-p013): bytes around the caret deleted, or text inserted
 * in place of the selection, as typing is.
 */
static void
app_text(
	struct te_app *app,
	const struct te_event *event)
{
	size_t length;
	size_t start;
	size_t end;
	size_t total;
	int error;

	/* Not while a dialog or the chooser asks. */
	if (app->dialog != TE_DIALOG_NONE || app->choosing)
		return;

	/* Bytes before and after the caret: selected, then deleted. */
	if (event->type == TE_EVENT_TEXT_DELETE) {
		total = te_buffer_length(&app->buffer);
		start = 0;
		if (app->cursor > (size_t)event->key)
			start = app->cursor - (size_t)event->key;
		end = app->cursor + (size_t)event->button;
		if (end > total)
			end = total;
		te_edit_select(app, start, end);
		(void)te_edit_insert_text(app, "", 0U, TE_MERGE_NONE);
		te_edit_reveal(app);
		app->dirty = 1;
		te_log("TEXT delete before=%u after=%u", (unsigned)event->key, (unsigned)event->button);
		return;
	}

	/* The text, in place of the selection. */
	length = strlen(event->text);
	if (length == 0U)
		return;
	error = te_edit_insert_text(app, event->text, length, TE_MERGE_NONE);
	if (error != 0)
		te_app_message(app, "Not enough memory.");
	te_edit_reveal(app);
	app->dirty = 1;
	te_log("TEXT commit bytes=%lu text=%s", (unsigned long)length, event->text);
}
