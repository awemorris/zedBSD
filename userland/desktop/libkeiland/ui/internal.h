/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the library's files share among themselves and do not export
 * (exports.map lets only the kl_ calls of <keiland/keiland.h> out).
 */

#ifndef KEIUI_INTERNAL_H
#define KEIUI_INTERNAL_H

#include <keiland/keiland.h>

struct wl_display;
struct wl_registry;
struct wl_event_queue;
struct wl_interface;

/*
 * A search for one global of a display (globals.c, WS131 p015), for
 * libkeiland's objects: the global's name (0 when the compositor has none)
 * and version, the registry it is bound from -- an application's, when
 * the display is one (no search then), otherwise a registry of the
 * search's own on a queue of its own -- and the search's objects.
 */
struct keiui_global_search {
	uint32_t name;
	uint32_t version;
	struct wl_registry *registry;
	struct wl_event_queue *queue;
	struct wl_display *wrapper;
	struct wl_registry *own;
	const char *interface;
};

/* The theme handed out made the appearance's with an accent's colours, one written so for the tests, and the light or the dark theme itself (theme.c, ws089-p017, ws179-p001). */
void keiui_theme_set(unsigned appearance, unsigned accent);
void keiui_theme_with(unsigned appearance, unsigned accent, struct kl_theme *theme);
const struct kl_theme *keiui_theme_of(unsigned appearance);

/* Finds a global (0 or ENOMEM), binds it on the default queue (NULL when it cannot), and ends the search. */
int keiui_global_find(struct keiui_global_search *search, struct wl_display *display, const char *interface);
void *keiui_global_bind(struct keiui_global_search *search, const struct wl_interface *type, uint32_t version);
void keiui_global_end(struct keiui_global_search *search);

/* The registry of a display's application, with its global of an interface (*name 0 when none); NULL when the display is no application's (app.c). */
struct wl_registry *keiui_app_global(struct wl_display *display, const char *interface, uint32_t *name, uint32_t *version);

/*
 * What a widget's record says of it (ui.c): it takes the keyboard, it
 * takes a drag (a slider), it shuts out the widgets drawn before it from
 * Tab (a dialog).  A press on a record that does not take the keyboard
 * gives it to the record of the same id under it that does (a list's row:
 * the list).
 */
#define KEIUI_FOCUSABLE		1U
#define KEIUI_DRAGGABLE		2U
#define KEIUI_MODAL		4U

/*
 * ws190-p002: a press on a widget that keeps the focus where it is (the
 * bar's buttons: a press there does not take the keyboard from the field
 * whose selection it acts on), and a widget a finger's drag that starts on
 * it does nothing for (neither a selection nor a scroll begins).
 */
#define KEIUI_KEEP_FOCUS	8U
#define KEIUI_NO_DRAG		16U

/* The ids of the library's own records (ws190-p002): the bar of a field's selection and its handles; programs use none from 0xfffffff0 up. */
#define KEIUI_TEXT_BAR_ID	0xfffffffdU
#define KEIUI_TEXT_HANDLE_ID	0xfffffffcU

/* Records a widget with its flags and reports what the input did to it (ui.c, KL_HIT_* bits). */
unsigned keiui_ui_widget(struct kl_ui *ui, uint32_t id, uint32_t index, const struct kl_rect *rect, unsigned flags);

/* Tells whether a widget takes a key (its code and the modifiers held). */
typedef int (*keiui_wants_key)(uint32_t code, unsigned modifiers);

/* The index that stands for any of an id's widgets when keys are taken (a dialog's, a list's). */
#define KEIUI_ANY		0xfffffffeU

/* Takes the next key a widget wants among those pressed while it had the focus (ui.c); 1 with it, 0 when none is left for it (the others stay for the application). */
int keiui_ui_take_key(struct kl_ui *ui, uint32_t id, uint32_t index, keiui_wants_key wants, uint32_t *code, unsigned *modifiers);

/* What one input taken by a widget that edits text is (keiui_ui_take_input): none, a key, a text to commit, or bytes to delete around the caret. */
#define KEIUI_INPUT_NONE	0U
#define KEIUI_INPUT_KEY		1U
#define KEIUI_INPUT_COMMIT	2U
#define KEIUI_INPUT_DELETE	3U

/*
 * One input a widget that edits text takes (BUG-203): its kind, a key's
 * code and modifiers, the text an input method or the on-screen keyboard
 * commits, and the bytes to delete before and after the caret.
 */
struct keiui_input {
	unsigned kind;
	uint32_t code;
	unsigned modifiers;
	char text[KL_WINDOW_TEXT_MAX];
	uint32_t before;
	uint32_t after;
	/* ws190-p002: a command of the bar of the fingers' selection (Ctrl+X, C, V or A the bar sent), not the keyboard's. */
	int from_bar;
};

/* Takes the next key a widget wants, or text sent for it, in the order they came while it had the focus (ui.c); 1 with it, 0 when none is left for it. */
int keiui_ui_take_input(struct kl_ui *ui, uint32_t id, uint32_t index, keiui_wants_key wants, struct keiui_input *input);

/*
 * The editing a text widget (a field, a text area) asks of its window's
 * input (ui.c, ws177-p013): the commands of the keys with Control --
 * undo (Ctrl+Z) and redo (Ctrl+Shift+Z, Ctrl+Y), copy, cut and paste
 * (Ctrl+C, Ctrl+X, Ctrl+V) and a word left or right (Ctrl+Left,
 * Ctrl+Right, with Shift the selection) -- and the history of the widget's
 * text undo walks, one change a key or a commit.
 */
#define KEIUI_EDIT_NONE		0U
#define KEIUI_EDIT_UNDO		1U
#define KEIUI_EDIT_REDO		2U
#define KEIUI_EDIT_COPY		3U
#define KEIUI_EDIT_CUT		4U
#define KEIUI_EDIT_PASTE	5U
#define KEIUI_EDIT_WORD_LEFT	6U
#define KEIUI_EDIT_WORD_RIGHT	7U

/* Tells which editing command a key with its modifiers is (KEIUI_EDIT_*). */
unsigned keiui_edit_command(uint32_t code, unsigned modifiers);

/* Records a widget's change of its text (before and after) in the history, with the caret and the selection before it. */
void keiui_edit_record(struct kl_ui *ui, uint32_t id, const char *before, size_t before_length, size_t caret_before, size_t anchor_before, const char *after, size_t after_length);

/* Takes a change back (redo 0) or does it again (redo 1) on a widget's text; 1 when one was, 0 when none (or the text is no longer the history's). */
int keiui_edit_undo(struct kl_ui *ui, uint32_t id, int redo, char *text, size_t *length, size_t capacity, size_t *caret, size_t *anchor);

/* Puts text on the window's clipboard (nothing without a window). */
void keiui_edit_copy(struct kl_ui *ui, const char *text, size_t length);

/* Reads the window's clipboard's text, NUL-terminated; its length (0 without a window or a text). */
size_t keiui_edit_paste(struct kl_ui *ui, char *text, size_t size);

/* Gives the start of the word before an offset (forward 0), or the end of the word after it (forward 1). */
size_t keiui_edit_word(const char *text, size_t length, size_t at, int forward);

/* Ties a window's input to its window for the clipboard (text-input.c, which links the window's calls). */
void keiui_ui_set_window(struct kl_ui *ui, struct kl_window *window, void (*copy)(struct kl_window *, const char *, size_t), size_t (*paste)(struct kl_window *, char *, size_t));

/* Reports the text being composed for a widget, or NULL; a widget drawn without the focus drops it (ui.c). */
const char *keiui_ui_preedit(struct kl_ui *ui, uint32_t id, uint32_t index, int focused, int32_t *begin, int32_t *end);

/* Notes that the focused widget being drawn takes text from an input method, with its caret's rectangle in the window (ui.c). */
void keiui_ui_text_caret(struct kl_ui *ui, const struct kl_rect *caret);

/* Takes Enter and Space pressed while a widget had the focus (ui.c): 1 when one was pressed (the other keys stay for the application). */
int keiui_ui_take_activate(struct kl_ui *ui, uint32_t id, uint32_t index);

/* Reports the widget with the focus (ui.c): 1 with its id and index, 0 when none has it. */
int keiui_ui_focused(const struct kl_ui *ui, uint32_t *id, uint32_t *index);

/* Tells whether the focus's ring shows (ui.c): the keyboard moved the focus, not a click, a tap or the application. */
int keiui_ui_focus_ring(const struct kl_ui *ui);

/*
 * Notes the on-screen keyboard's inset a window heard (ui.c, from
 * window.c; ws102-p015): the window's size, the covered widths from its
 * right and bottom edges, the reason, and the caret's rectangle in the
 * window as the application last told it (kl_window_text_cursor; height 0
 * when it has not).  The next kl_ui_end of each window's input keeps its
 * text view's caret in sight (the library runs on one thread).
 */
void keiui_ui_inset_note(uint32_t width, uint32_t height, int right, int bottom, unsigned reason, const int32_t *caret);

/* Begins a finger's drag of a named end's handle (text-touch.c; KL_TEXT_HANDLE_ANCHOR or _CARET), at a point of the view's content. */
void keiui_text_touch_hold(struct kl_text_touch *touch, int end, double x, double y);

/* Reports the time of the frame being drawn (ui.c, the time kl_ui_begin was given). */
uint64_t keiui_ui_now(const struct kl_ui *ui);

/* Draws a button that is one of several under an id (widgets.c; a dialog's) and reports 1 when it was pressed. */
int keiui_button(struct kl_ui *ui, const struct kl_style *style, uint32_t id, uint32_t index, const struct kl_rect *rect, const char *label, unsigned flags);

/* The line pictures, from KL_ICON_TILES on (icons-line.c). */
void keiui_icon_line_draw(struct kl_canvas *canvas, enum kl_icon icon, float x, float y, float size, kl_color color);

/*
 * ws190-p002 (plan/ws190/phase001/phase.md section 2.3): the fingers'
 * selection of a field or a text area, one a window's input (kl_ui keeps
 * it from kl_ui_create to kl_ui_destroy).  A double tap of a finger puts
 * the focused field in it; kl_ui records the handles and the bar over the
 * field in kl_ui_end and carries the fingers on them out.
 *
 * The view's answers come from the field's own copy kept here (field or
 * area, and for a text area its lines in layout), never from the
 * application's memory between frames: the field is drawn each frame,
 * compares its text with the copy (another text set by the application
 * ends the mode) and copies itself again.  scroll is the content's scroll
 * the handles' drag moves near the view's edges; it is made with the input
 * and lives as long, as the records of the frame shown point at it.
 */
#define KEIUI_SELECT_FIELD	1
#define KEIUI_SELECT_AREA	2

/*
 * The bar's calls (text-bar.c), given to kl_ui by the widget that begins
 * the fingers' selection, so that ui.c does not link the bar's drawing and
 * text (a window's input without fields does not need them).
 */
struct keiui_bar_calls {
	unsigned (*buttons)(unsigned facts);
	int (*layout)(struct kl_text_bar *bar, struct kl_text *text, unsigned buttons, const struct kl_rect *selection, const struct kl_rect *visible, const struct kl_rect *bounds);
	unsigned (*hit)(struct kl_ui *ui, uint32_t id, const struct kl_text_bar *bar, unsigned *held);
	void (*draw)(const struct kl_text_bar *bar, const struct kl_style *style, unsigned held);
};

/* The bar's calls of text-bar.c. */
extern const struct keiui_bar_calls keiui_text_bar_calls;

struct keiui_select {
	int active;
	uint32_t id;
	uint32_t index;
	const void *widget;
	int kind;
	struct kl_text_touch touch;
	const struct keiui_bar_calls *bar_calls;
	struct kl_scroll scroll;
	struct kl_rect rect;
	struct kl_rect box;
	struct kl_rect clip;
	struct kl_style style;
	struct kl_field field;
	struct kl_text_area area;
	void *layout;
	int drawn;
	size_t order;
};

/* Gives a window's input's fingers' selection (ui.c). */
struct keiui_select *keiui_ui_select(struct kl_ui *ui);

/* Tells whether the fingers' selection is a widget's (ui.c): 1 while the widget is in the mode. */
int keiui_ui_select_owned(struct kl_ui *ui, uint32_t id, uint32_t index);

/* Puts a widget in the fingers' selection (ui.c): its id and index, its kind and its view's answers; the widget then gives its copy each frame (keiui_ui_select_drawn). */
void keiui_ui_select_begin(struct kl_ui *ui, uint32_t id, uint32_t index, int kind, const struct kl_text_view *view, const void *widget, const struct keiui_bar_calls *bar_calls);

/* Takes the fingers' selection's mode away (ui.c; the widget keeps its selection). */
void keiui_ui_select_end(struct kl_ui *ui);

/* Notes that the widget in the mode was drawn in the frame being drawn, at its rectangle and text box, with its style (ui.c). */
void keiui_ui_select_drawn(struct kl_ui *ui, const struct kl_rect *rect, const struct kl_rect *box, const struct kl_style *style);

/* Tells whether the fingers' selection may begin in this window's input (kl_ui_set_text_bar). */
int keiui_ui_select_enabled(const struct kl_ui *ui);

/* Ties the window's calls the bar asks of it (text-input.c): whether the clipboard has text, and the on-screen keyboard's inset. */
void keiui_ui_set_window_extras(struct kl_ui *ui, int (*can_paste)(const struct kl_window *), void (*keyboard_inset)(const struct kl_window *, int *, int *));

/* Gives the part two rectangles share (ui.c); 1 with it, 0 when they share no area. */
int keiui_rect_intersect(const struct kl_rect *first, const struct kl_rect *second, struct kl_rect *result);

/* The word around a position of a text (text-select.c, the fingers' word: section 2.4); start = end off a word. */
void keiui_select_word(const char *text, size_t length, size_t position, size_t *start, size_t *end);

#endif
