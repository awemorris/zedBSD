/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of Text Editor that know neither Wayland nor Vulkan: the
 * document (a gap buffer with a table of line starts), its undo history,
 * reading and saving files, the rows the text is laid out in, editing,
 * finding, and drawing the frame on the CPU.  The host tests build these
 * alone (plan/ws092/design.md section 2).
 */

#ifndef TEXTEDIT_TEXTEDIT_H
#define TEXTEDIT_TEXTEDIT_H

#include <keiland/keiland.h>

#include "userland/desktop/paths.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

struct truetype_face;

/* The window's size until the compositor gives one. */
#define TE_WIDTH		900U
#define TE_HEIGHT		680U

/* The longest path kept, and the longest text searched for. */
#define TE_PATH_MAX		1024
#define TE_FIND_MAX		256

/* The largest file opened, and the largest the text may grow to by editing. */
#define TE_FILE_MAX		(16UL * 1024UL * 1024UL)
#define TE_TEXT_MAX		(64UL * 1024UL * 1024UL)

/* The modifiers held with an input. */
#define TE_MOD_SHIFT		0x01U
#define TE_MOD_CTRL		0x02U
#define TE_MOD_ALT		0x04U
#define TE_MOD_SUPER		0x08U

/* The evdev codes of the keys the editor knows by name. */
#define TE_KEY_ESCAPE		1U
#define TE_KEY_0		11U
#define TE_KEY_MINUS		12U
#define TE_KEY_EQUAL		13U
#define TE_KEY_BACKSPACE	14U
#define TE_KEY_TAB		15U
#define TE_KEY_Q		16U
#define TE_KEY_W		17U
#define TE_KEY_Y		21U
#define TE_KEY_O		24U
#define TE_KEY_ENTER		28U
#define TE_KEY_A		30U
#define TE_KEY_S		31U
#define TE_KEY_D		32U
#define TE_KEY_F		33U
#define TE_KEY_G		34U
#define TE_KEY_H		35U
#define TE_KEY_Z		44U
#define TE_KEY_X		45U
#define TE_KEY_C		46U
#define TE_KEY_V		47U
#define TE_KEY_N		49U
#define TE_KEY_SPACE		57U
#define TE_KEY_F3		61U
#define TE_KEY_KP_ENTER		96U
#define TE_KEY_HOME		102U
#define TE_KEY_UP		103U
#define TE_KEY_PAGE_UP		104U
#define TE_KEY_LEFT		105U
#define TE_KEY_RIGHT		106U
#define TE_KEY_END		107U
#define TE_KEY_DOWN		108U
#define TE_KEY_PAGE_DOWN	109U
#define TE_KEY_INSERT		110U
#define TE_KEY_DELETE		111U

/* The pointer's buttons, as evdev codes. */
#define TE_BUTTON_LEFT		0x110U
#define TE_BUTTON_RIGHT		0x111U
#define TE_BUTTON_MIDDLE	0x112U

/*
 * The kinds of input the window queues for the editor: the pointer, the
 * wheel, a key, a menu's action, the keyboard's focus coming or going, the
 * file chooser's answer, and an input method's text.
 */
enum te_event_type {
	TE_EVENT_MOTION = 0,
	TE_EVENT_BUTTON,
	TE_EVENT_AXIS,
	TE_EVENT_LEAVE,
	TE_EVENT_KEY,
	TE_EVENT_ACTION,
	TE_EVENT_FOCUS,
	TE_EVENT_CHOSEN,
	TE_EVENT_TEXT,
	TE_EVENT_TEXT_DELETE,
	TE_EVENT_AXIS_STOP
};

/*
 * One input: its kind, the pointer's place, the button and whether it was
 * pressed, the wheel's distance in pixels (down and right are positive;
 * for a touch pad's fingers also unrounded, with the source and the
 * compositor's time on the monotonic clock, ws090-p019),
 * the key, the modifiers, the time in milliseconds, the action, whether
 * the focus came (pressed), and an input method's text or the path the
 * file chooser chose (empty when it was cancelled).
 */
struct te_event {
	enum te_event_type type;
	int x;
	int y;
	uint32_t button;
	int pressed;
	int scroll;
	int scroll_x;
	double axis_dx;
	double axis_dy;
	unsigned axis_source;
	uint64_t axis_us;
	uint32_t key;
	uint32_t modifiers;
	uint64_t time;
	uint32_t action;
	char text[TE_PATH_MAX];
};

/*
 * What the menus, the keys and the context menu ask for.
 */
enum te_action {
	TE_ACTION_NONE = 0,
	TE_ACTION_NEW,
	TE_ACTION_OPEN,
	TE_ACTION_SAVE,
	TE_ACTION_SAVE_AS,
	TE_ACTION_CLOSE,
	TE_ACTION_QUIT,
	TE_ACTION_UNDO,
	TE_ACTION_REDO,
	TE_ACTION_CUT,
	TE_ACTION_COPY,
	TE_ACTION_PASTE,
	TE_ACTION_SELECT_ALL,
	TE_ACTION_FIND,
	TE_ACTION_FIND_NEXT,
	TE_ACTION_FIND_PREVIOUS,
	TE_ACTION_REPLACE,
	TE_ACTION_LINE_NUMBERS,
	TE_ACTION_WORD_WRAP,
	TE_ACTION_BIGGER,
	TE_ACTION_SMALLER,
	TE_ACTION_ACTUAL_SIZE,
	TE_ACTION_ABOUT
};

/*
 * File > Open Recent (ws128-p003): the actions of its items, the first
 * file's and the ones after it, and how many files it shows.
 */
#define TE_ACTION_RECENT_FIRST	100U
#define TE_RECENT_MAX		10U

/*
 * A frame being drawn: premultiplied 0xAARRGGBB words, stride words a row,
 * and the rectangle drawing is clipped to (the whole canvas unless
 * te_canvas_clip narrows it).
 */
struct te_canvas {
	uint32_t *pixels;
	size_t stride;
	int width;
	int height;
	int clip_x;
	int clip_y;
	int clip_width;
	int clip_height;
};

/*
 * One glyph drawn at a size: its cache key (0 for an empty slot), its
 * coverage bitmap (width bytes a row; NULL for a blank glyph) or its
 * colours (premultiplied 0xAARRGGBB, a colour emoji, ws102-p019), where it
 * sits from the pen and the baseline, and how far it moves the pen.
 */
struct te_glyph {
	uint32_t key;
	int width;
	int height;
	int left;
	int top;
	int advance;
	uint8_t *bitmap;
	uint32_t *pixels;
};

/* One font file: its bytes (kept for the face), the face, and the size set last. */
struct te_text_face {
	void *data;
	size_t size;
	struct truetype_face *face;
	unsigned pixels;
};

/*
 * How many faces a text has: the main font, a fallback, and the colour emoji
 * font (opened the first time a character neither of the others has is
 * drawn, ws102-p019).
 */
#define TE_TEXT_FACES		3
#define TE_TEXT_EMOJI		KEILAND_DATADIR "/fonts/keiland-emoji.ttf"

/*
 * The fonts of one kind of text (the body's monospaced font, or the
 * interface's) and every glyph drawn so far.  One lives for the whole run;
 * the cache is emptied when it fills up.
 */
struct te_text {
	struct te_text_face faces[TE_TEXT_FACES];
	int face_count;
	int emoji_tried;
	struct te_glyph *cache;
	unsigned cache_size;
	unsigned cache_used;
	uint8_t *scratch;
	size_t scratch_size;
};

/* The vertical measurements of a font at a size, in pixels. */
struct te_text_line {
	int ascent;
	int descent;
	int height;
};

/*
 * The document's text: UTF-8 bytes in a gap buffer, and where each line
 * starts.
 *
 * Positions are logical byte offsets (the gap left out).  lines[i] is where
 * line i starts; lines[0] is 0 and line_count is at least 1.  The buffer
 * owns both arrays.
 */
struct te_buffer {
	char *data;
	size_t capacity;
	size_t gap_start;
	size_t gap_end;
	size_t *lines;
	size_t line_count;
	size_t line_capacity;
};

/* The kinds of change the undo history records. */
#define TE_UNDO_INSERT		0
#define TE_UNDO_DELETE		1

/* How a change may join the one before it (typing, Backspace, Delete). */
#define TE_MERGE_NONE		0
#define TE_MERGE_TYPING		1
#define TE_MERGE_BACKSPACE	2
#define TE_MERGE_FORWARD	3

/*
 * One change: what it did (inserted or deleted text at a position), the
 * cursor and the selection's anchor before and after it, the group of
 * changes undone together, and how it may be joined.  The step owns its
 * text.
 */
struct te_undo_step {
	int kind;
	size_t position;
	char *text;
	size_t length;
	size_t cursor_before;
	size_t anchor_before;
	size_t cursor_after;
	size_t anchor_after;
	unsigned group;
	int merge;
	uint64_t time;
};

/*
 * The undo history: the steps in order, how many are done (the rest can
 * be redone), the next group number, the bytes the steps hold, and the
 * number of steps done when the document was last saved (saved_valid is 0
 * once that point has been dropped or redone away).
 */
struct te_undo {
	struct te_undo_step *steps;
	size_t count;
	size_t capacity;
	size_t done;
	unsigned next_group;
	size_t bytes;
	size_t saved;
	int saved_valid;
};

/*
 * How the text is laid out in rows.
 *
 * wrap says long lines are broken at columns cells; tab is the distance
 * between tab stops in cells.  rows[i] is how many rows line i takes and
 * first_row[i] the row it starts on (both have lines entries); total is
 * the number of rows.  widest is the widest line in cells (for scrolling
 * across when lines are not wrapped; it only grows until the next reset).
 * breaks is scratch memory for one line's row starts.
 */
struct te_layout {
	int wrap;
	unsigned columns;
	unsigned tab;
	size_t *rows;
	size_t *first_row;
	size_t lines;
	size_t capacity;
	size_t total;
	size_t widest;
	size_t *breaks;
	size_t break_capacity;
};

/*
 * What a file was when it was read, so that it is saved the same way:
 * whether its lines ended in CR LF, whether it began with a byte order
 * mark, whether some bytes were not UTF-8, whether it existed, and its
 * mode, modification time and size.
 */
struct te_file_info {
	int crlf;
	int bom;
	int invalid;
	int exists;
	mode_t mode;
	time_t mtime;
	off_t size;
};

/* The dialogs a frame may show over the text. */
enum te_dialog {
	TE_DIALOG_NONE = 0,
	TE_DIALOG_UNSAVED,
	TE_DIALOG_CHANGED,
	TE_DIALOG_ABOUT,
	TE_DIALOG_REPLACE,
	TE_DIALOG_FIND
};

/* What waits for the unsaved changes to be saved or dropped. */
enum te_after {
	TE_AFTER_NOTHING = 0,
	TE_AFTER_CLOSE,
	TE_AFTER_NEW,
	TE_AFTER_OPEN,
	TE_AFTER_OPEN_PATH
};

/*
 * What the editor asks of the window: copy text to the clipboard, paste
 * the clipboard's text, make text the primary selection, paste the primary
 * selection, open the context menu at a place, and open the file chooser (to open a file, or to save as a
 * name in a folder; the path chosen comes back as a TE_EVENT_CHOSEN).  Any
 * member may be NULL (the host tests leave them so).
 */
struct te_host {
	void *data;
	void (*copy)(void *data, const char *text, size_t length);
	size_t (*paste)(void *data, char *text, size_t size);
	void (*select)(void *data, const char *text, size_t length);
	size_t (*paste_primary)(void *data, char *text, size_t size);
	void (*context_menu)(void *data, int x, int y);
	int (*choose)(void *data, int saving, const char *folder, const char *name);
	/* Starts a drag of text out of the window (ws189-p003); 0 when it started. */
	int (*drag_text)(void *data, const char *text, size_t length);
};

/* A rectangle of the frame. */
struct te_rect {
	int x;
	int y;
	int width;
	int height;
};

/*
 * The editor: the document and its file, the cursor and the selection,
 * the view (font size, rows, scrolling), the pointer's selecting, the find
 * text, the message, the dialog and the chooser, and what the main loop
 * should do next.  One lives for the whole run.
 */
struct te_app {
	/* The document, its history and its rows. */
	struct te_buffer buffer;
	struct te_undo undo;
	struct te_layout layout;

	/* The fonts, and the window's services. */
	struct te_text *body;
	struct te_text *ui;
	struct te_host host;

	/* The file: its path (empty for Untitled) and how it was read. */
	char path[TE_PATH_MAX];
	struct te_file_info file;

	/* The cursor, the selection's other end, and the cell column up and down keep. */
	size_t cursor;
	size_t anchor;
	size_t goal;
	int goal_valid;

	/* The frame's size, and the text's size and cell. */
	int width;
	int height;
	unsigned pixels;
	int cell;
	int row_height;
	int ascent;

	/* View > Line Numbers and View > Word Wrap. */
	int line_numbers;
	int wrap;

	/*
	 * Where the view is scrolled to (pixels): the libkeiland scroll that
	 * moves it (the wheel's glide, the fingers' drag and flight), and the
	 * place drawn, copied from the scroll by te_app_sync_scroll.
	 */
	struct kl_scroll scroll;
	double scroll_x;
	double scroll_y;

	/* The fingers' selection in the text (libkeiland's text view touch: one finger selects, two scroll), and whether its handles are drawn. */
	struct kl_text_touch touch;
	int handles_shown;

	/* The keyboard's focus and the cursor's blinking. */
	int focused;
	uint64_t blink_start;

	/* The pointer: where it is, a selection being dragged (by characters, words or lines), and clicks counted. */
	int pointer_x;
	int pointer_y;
	int selecting;
	int select_unit;
	size_t select_start;
	size_t select_end;
	uint64_t click_time;
	int click_count;
	int click_x;
	int click_y;

	/*
	 * Drag and drop with other windows (ws189-p003): a left press in the
	 * selection waits to become a drag of its text (drag_armed, where it
	 * pressed and the text position there); dragging_out while that drag
	 * goes on; and a drag of text from elsewhere over the text, with the
	 * position a drop would insert at (drop_over).
	 */
	int drag_armed;
	int drag_press_x;
	int drag_press_y;
	size_t drag_position;
	int dragging_out;
	int drop_over;
	size_t drop_position;

	/* The find text, and whether the last search wrapped. */
	char find[TE_FIND_MAX];
	size_t find_length;

	/*
	 * Edit > Find and Edit > Replace (BUG-248 and ws128-p003, the dialogs
	 * TE_DIALOG_FIND and TE_DIALOG_REPLACE drawn by main.c): the
	 * replacement last used, and whether the panel was just opened (main.c
	 * then fills its fields and gives one the keyboard).
	 */
	char replace_with[TE_FIND_MAX];
	int panel_fresh;

	/*
	 * File > Open Recent (ws128-p003): the files the editor used, newest
	 * first (main.c reads them from libkeiland's recent list), whether each
	 * is still there (one that is not is shown greyed), and the file chosen
	 * that waits for unsaved changes to be dealt with (TE_AFTER_OPEN_PATH).
	 * recent_stamp is the list's stamp when it was read (kl_recent_stamp,
	 * ws177-p008): the window's getting the keyboard reads it again when
	 * another program changed it.
	 */
	char recent[TE_RECENT_MAX][TE_PATH_MAX];
	int recent_present[TE_RECENT_MAX];
	size_t recent_count;
	uint64_t recent_stamp;
	char open_path[TE_PATH_MAX];

	/* The message shown at the bottom, until when. */
	char message[160];
	uint64_t message_until;

	/* The dialog shown (drawn and answered by libkeiland's kl_dialog in main.c), and what waits for it. */
	enum te_dialog dialog;
	enum te_after after;

	/* Whether the file chooser is open, and for Save As. */
	int choosing;
	int choosing_save;

	/* What the main loop does next: close, show a new title, log and remember a file opened, draw. */
	int want_close;
	int title_changed;
	int opened;
	int primary_changed;
	int dirty;
	uint64_t now;

	/* Whether the window is glass (the frame leaves the desktop showing around the card). */
	int glass;

	/* The selection's length in characters, counted for the status, and what it was counted for. */
	size_t counted;
	size_t counted_cursor;
	size_t counted_anchor;
	size_t counted_length;
	int counted_valid;

	/*
	 * Text being composed by an input method (WS095, the text input's
	 * preedit), drawn in the body at the cursor in the body's own cells and
	 * size, the text after the cursor moved on by it.  The begin and the
	 * end are the preedit's cursor in bytes: the segment being converted
	 * when they differ, the caret when they are equal, none when negative.
	 */
	char preedit[KL_WINDOW_TEXT_MAX];
	int32_t preedit_begin;
	int32_t preedit_end;
};

/* The canvas (canvas.c). */
void te_canvas_clip(struct te_canvas *canvas, int x, int y, int width, int height);
void te_canvas_unclip(struct te_canvas *canvas);
void te_canvas_fill(struct te_canvas *canvas, int x, int y, int width, int height, uint32_t color);
void te_canvas_blend(struct te_canvas *canvas, int x, int y, int width, int height, uint32_t color);
void te_canvas_round(struct te_canvas *canvas, int x, int y, int width, int height, int radius, uint32_t color);
void te_canvas_mask(struct te_canvas *canvas, int x, int y, const unsigned char *mask, int width, int height, size_t stride, uint32_t color);
void te_canvas_pixels(struct te_canvas *canvas, int x, int y, const uint32_t *pixels, int width, int height);

/* The text (text.c). */
int te_text_open(struct te_text *text, const char *primary, const char *fallback);
void te_text_close(struct te_text *text);
void te_text_metrics(struct te_text *text, unsigned pixels, struct te_text_line *line);
int te_text_center(unsigned pixels, int top, int height);
int te_text_width(struct te_text *text, const char *string, size_t length, unsigned pixels, int bold);
int te_text_draw(struct te_text *text, struct te_canvas *canvas, int x, int baseline, const char *string, size_t length, unsigned pixels, int bold, uint32_t color);
int te_text_draw_fit(struct te_text *text, struct te_canvas *canvas, int x, int baseline, const char *string, unsigned pixels, int bold, int width, uint32_t color);
size_t te_text_fit(struct te_text *text, const char *string, unsigned pixels, int bold, int width, char *out, size_t size);
int te_text_advance(struct te_text *text, uint32_t codepoint, unsigned pixels);
void te_text_draw_char(struct te_text *text, struct te_canvas *canvas, int x, int baseline, uint32_t codepoint, unsigned pixels, uint32_t color);
uint32_t te_utf8_next(const char *string, size_t length, size_t *index);

/* The document (buffer.c). */
int te_buffer_init(struct te_buffer *buffer, const char *text, size_t length);
void te_buffer_free(struct te_buffer *buffer);
size_t te_buffer_length(const struct te_buffer *buffer);
int te_buffer_insert(struct te_buffer *buffer, size_t position, const char *bytes, size_t count);
void te_buffer_delete(struct te_buffer *buffer, size_t start, size_t end);
unsigned char te_buffer_byte(const struct te_buffer *buffer, size_t position);
void te_buffer_copy(const struct te_buffer *buffer, size_t start, size_t end, char *out);
void te_buffer_segments(const struct te_buffer *buffer, const char **first, size_t *first_length, const char **second, size_t *second_length);
size_t te_buffer_line_of(const struct te_buffer *buffer, size_t position);
size_t te_buffer_line_start(const struct te_buffer *buffer, size_t line);
size_t te_buffer_line_end(const struct te_buffer *buffer, size_t line);
uint32_t te_buffer_char(const struct te_buffer *buffer, size_t position, size_t *next);
size_t te_buffer_next_char(const struct te_buffer *buffer, size_t position);
size_t te_buffer_prev_char(const struct te_buffer *buffer, size_t position);

/* The undo history (undo.c). */
void te_undo_init(struct te_undo *undo);
void te_undo_free(struct te_undo *undo);
unsigned te_undo_group(struct te_undo *undo);
int te_undo_record(struct te_undo *undo, const struct te_undo_step *step);
int te_undo_undo_range(struct te_undo *undo, size_t *first, size_t *last);
int te_undo_redo_range(struct te_undo *undo, size_t *first, size_t *last);
const struct te_undo_step *te_undo_step(const struct te_undo *undo, size_t index);
int te_undo_can_undo(const struct te_undo *undo);
int te_undo_can_redo(const struct te_undo *undo);
void te_undo_mark_saved(struct te_undo *undo);
int te_undo_modified(const struct te_undo *undo);

/* Files (file.c). */
int te_file_read(const char *path, char **text, size_t *length, struct te_file_info *info);
int te_file_write(const char *path, const struct te_buffer *buffer, struct te_file_info *info);
int te_file_changed(const char *path, const struct te_file_info *info);

/* The rows (layout.c). */
void te_layout_init(struct te_layout *layout);
void te_layout_free(struct te_layout *layout);
int te_layout_reset(struct te_layout *layout, const struct te_buffer *buffer, int wrap, unsigned columns);
int te_layout_update(struct te_layout *layout, const struct te_buffer *buffer, size_t first, size_t old_count, size_t new_count);
unsigned te_layout_cells(uint32_t codepoint, unsigned column, unsigned tab);
size_t te_layout_breaks(struct te_layout *layout, const struct te_buffer *buffer, size_t line, const size_t **breaks);
size_t te_layout_line_of_row(const struct te_layout *layout, size_t row);
void te_layout_row_range(struct te_layout *layout, const struct te_buffer *buffer, size_t row, size_t *start, size_t *end);
void te_layout_place(struct te_layout *layout, const struct te_buffer *buffer, size_t position, size_t *row, size_t *column);
size_t te_layout_position(struct te_layout *layout, const struct te_buffer *buffer, size_t row, size_t column);
size_t te_layout_columns_between(const struct te_layout *layout, const struct te_buffer *buffer, size_t start, size_t end);

/* Finding (find.c). */
int te_find(const struct te_buffer *buffer, const char *needle, size_t length, size_t from, int forward, size_t *start, int *wrapped);
int te_find_at(const struct te_buffer *buffer, const char *needle, size_t length, size_t position);

/* Editing (edit.c). */
int te_edit_insert_text(struct te_app *app, const char *text, size_t length, int merge);
int te_edit_delete(struct te_app *app, size_t start, size_t end, int merge);
int te_edit_key(struct te_app *app, const struct te_event *event);
void te_edit_selection(const struct te_app *app, size_t *start, size_t *end);
void te_edit_select(struct te_app *app, size_t anchor, size_t cursor);
void te_edit_copy(struct te_app *app);
void te_edit_cut(struct te_app *app);
void te_edit_paste(struct te_app *app, int primary);
void te_edit_select_all(struct te_app *app);
void te_edit_undo(struct te_app *app);
void te_edit_redo(struct te_app *app);
void te_edit_word(const struct te_app *app, size_t position, size_t *start, size_t *end);
void te_edit_line(const struct te_app *app, size_t position, size_t *start, size_t *end);
size_t te_edit_position_at(struct te_app *app, int x, int y);
void te_edit_reveal(struct te_app *app);
void te_edit_find(struct te_app *app, int forward, int from_selection);
int te_edit_replace(struct te_app *app, const char *with, size_t with_length);
size_t te_edit_replace_all(struct te_app *app, const char *with, size_t with_length);

/* The editor (app.c). */
void te_app_init(struct te_app *app, struct te_text *body, struct te_text *ui, int width, int height);
void te_app_release(struct te_app *app);
int te_app_open(struct te_app *app, const char *path);
void te_app_new(struct te_app *app);
int te_app_save(struct te_app *app, const char *path);
void te_app_resize(struct te_app *app, int width, int height);
void te_app_event(struct te_app *app, const struct te_event *event);
void te_app_action(struct te_app *app, enum te_action action);
int te_app_tick(struct te_app *app, uint64_t now);
void te_app_message(struct te_app *app, const char *message);
void te_app_relayout(struct te_app *app);
void te_app_clamp(struct te_app *app);
int te_app_sync_scroll(struct te_app *app, uint64_t now_us);
void te_app_touch(struct te_app *app);
void te_app_text_rect(const struct te_app *app, struct te_rect *rect);
void te_app_caret_rect(const struct te_app *app, struct te_rect *rect);
unsigned te_app_preedit_cells(const struct te_app *app, unsigned column, size_t bytes);
void te_app_card(const struct te_app *app, struct te_rect *rect);
double te_app_max_scroll_x(const struct te_app *app);
double te_app_max_scroll_y(const struct te_app *app);
const char *te_app_name(const struct te_app *app);
int te_app_modified(const struct te_app *app);
void te_app_publish_primary(struct te_app *app);
void te_app_tap(struct te_app *app, int x, int y, int count);
int te_app_drop_over(struct te_app *app, int x, int y);
void te_app_drop_leave(struct te_app *app);
void te_app_drop_text(struct te_app *app, const char *text, size_t length);
void te_app_drag_done(struct te_app *app);
void te_app_drop_rect(const struct te_app *app, struct te_rect *rect);
void te_app_dialog_choose(struct te_app *app, int button);
void te_app_dialog_words(const struct te_app *app, char *title, size_t size, const char **words, const char *const **labels, int *count);
void te_app_replace(struct te_app *app, const char *find, const char *with, int all);
void te_app_replace_close(struct te_app *app);
void te_app_find_text(struct te_app *app, const char *text);
void te_app_find_close(struct te_app *app);

/* The frame (draw.c). */
void te_draw(struct te_app *app, struct te_canvas *canvas);

/* The log and the clock (main.c, or the host tests). */
void te_log(const char *format, ...);
uint64_t te_clock(void);

/*
 * The layout of the frame, in pixels: the card reaches the window's edges
 * (no inset), so that it lines up with the floating titlebar and the gap
 * above it is the compositor's alone (ws090-p021).
 */
#define TE_CARD_INSET		0
#define TE_CARD_RADIUS		18
#define TE_TEXT_SIDE		16
#define TE_TEXT_TOP		12
#define TE_GUTTER_PAD		12

/* The text's sizes: the body's default, smallest and largest, and the interface's. */
#define TE_PIXELS_DEFAULT	15U
#define TE_PIXELS_MIN		10U
#define TE_PIXELS_MAX		32U
#define TE_UI_PIXELS		13U

#endif
