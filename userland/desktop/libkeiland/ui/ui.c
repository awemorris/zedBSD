/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The input of a window (ws090-p003, plan/ws090/design.md section 4):
 * which part of a frame a pointer, the wheel or a finger meant.
 *
 * Each frame records the parts that take input -- widgets, scroll
 * viewports and text views -- in the order they were drawn.  Input is
 * resolved as it arrives against the records of the frame last drawn (the
 * one on the screen), the latest record first, since what was drawn last
 * is on top.  kl_ui_end makes the frame just drawn the one input is
 * resolved against.
 *
 * The fingers go through one libkeiland gesture recognizer.  The first
 * finger of a touch decides its targets: the widget under it (for a tap)
 * and the scroll or text view under it (for a drag), fixed until the last
 * finger lifts.
 */

#include "internal.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most parts a frame records, and the most unclaimed inputs kept. */
#define UI_RECORDS		512U
#define UI_EVENTS		64U

/* The longest time between the clicks of a double click, in microseconds. */
#define UI_DOUBLE_US		400000U

/* The most gestures taken in one go. */
#define UI_GESTURES		16

/* The most keys waiting for the next frame's widgets. */
#define UI_KEYS			32U

/* The evdev code of Tab, which moves the focus. */
#define UI_KEY_TAB		15U

/* The evdev codes of the editing commands' letters (ws177-p013): Z, Y, C, X and V. */
#define UI_KEY_Z		44U
#define UI_KEY_Y		21U
#define UI_KEY_C		46U
#define UI_KEY_X		45U
#define UI_KEY_V		47U

/* The evdev code of A, whose Ctrl+A the bar's Select All sends (ws190-p002). */
#define UI_KEY_A		30U

/* The most changes a text widget's history keeps (the oldest goes first). */
#define UI_UNDO_STEPS		100U

/* The FNV-1a hash's start and prime, which stamp a text the history knows. */
#define UI_HASH_START		0xcbf29ce484222325ULL
#define UI_HASH_PRIME		0x100000001b3ULL

/*
 * How far round a widget's region a change of the lit widget may draw
 * (pixels: a lit row's ground, a ring, a shadow; BUG-226).
 */
#define UI_DAMAGE_MARGIN	8

/* What a record is. */
enum ui_kind {
	UI_KIND_HIT,
	UI_KIND_SCROLL,
	UI_KIND_TEXT,
	UI_KIND_HANDLE
};

/* What a touch's drag does. */
enum ui_drag {
	UI_DRAG_NONE,
	UI_DRAG_SCROLL,
	UI_DRAG_SELECT,
	UI_DRAG_WIDGET,
	UI_DRAG_OUTSIDE
};

/*
 * One part of a frame that takes input: a widget (its id and index), a
 * scroll's viewport or a text view (its id, its scroll and its touch).  The
 * scroll and the touch are the application's and live until the next
 * frame.
 */
struct ui_record {
	enum ui_kind kind;
	uint32_t id;
	uint32_t index;
	unsigned flags;
	struct kl_rect rect;
	struct kl_scroll *scroll;
	struct kl_text_touch *touch;

	/*
	 * A handle of the fingers' selection (UI_KIND_HANDLE, ws190-p002): the
	 * text box its touch's content is in (rect is the knob's reach), and
	 * the knob's centre, around which a finger finds it.  index is the end
	 * (KL_TEXT_HANDLE_ANCHOR or _CARET).
	 */
	struct kl_rect view;
	double centre_x;
	double centre_y;
};

/*
 * One change of a text widget's text: where it is, the bytes it took out
 * and those it put in (each allocated), and the caret and the selection's
 * other end before it.
 */
struct ui_undo_step {
	size_t at;
	char *removed;
	size_t removed_length;
	char *inserted;
	size_t inserted_length;
	size_t caret_before;
	size_t anchor_before;
};

/*
 * The history of the text widget last edited (ws177-p013): its id, the
 * changes (count of them, done of them not taken back), and the hash of
 * its text after the last one done, which tells a text the application
 * set itself (the history then no longer applies).  valid is 0 while no
 * widget has one.
 */
struct ui_undo {
	int valid;
	uint32_t id;
	uint64_t hash;
	struct ui_undo_step steps[UI_UNDO_STEPS];
	size_t count;
	size_t done;
};

/* A widget's identity: its id and index, whether there is one, and its record's flags (KEIUI_*). */
struct ui_key {
	int valid;
	uint32_t id;
	uint32_t index;
	unsigned flags;
};

/*
 * A key pressed for the widget that had the focus then (a Tab after it
 * moves the focus on), and whether that widget took it.  Since KL_VERSION
 * 38 (BUG-203) it may instead be a text an input method or the on-screen
 * keyboard sent for that widget (kind): text to put in place of the
 * selection, or bytes to delete around it.  Keys and texts wait in one
 * queue so that a text field carries them out in the order they came.
 */
struct ui_press {
	unsigned kind;
	uint32_t code;
	unsigned modifiers;
	char text[KL_WINDOW_TEXT_MAX];
	uint32_t before;
	uint32_t after;
	struct ui_key target;
	int taken;
	int from_bar;
};

/*
 * One window's input state, from kl_ui_create to kl_ui_destroy.
 *
 * shown is the frame on the screen, against which input is resolved;
 * drawing is the frame being drawn.  kl_ui_end swaps them.
 */
struct kl_ui {
	struct ui_record *shown;
	size_t shown_count;
	struct ui_record *drawing;
	size_t drawing_count;

	/* The pointer: where it is, the widget under it, the widget pressed, the last click and the one before it. */
	double pointer_x;
	double pointer_y;
	int pointer_inside;
	struct ui_key hot;
	struct ui_key active;
	struct ui_key released;
	struct ui_key clicked;
	int clicked_double;
	int clicked_touch;
	struct ui_key last_click;
	uint64_t last_click_us;

	/* The fingers: the gestures, how many are down, the touch's targets and what its drag does. */
	struct kl_gesture *gesture;
	unsigned fingers;
	struct ui_key touch_hit;
	struct ui_record touch_region;
	int touch_has_region;
	int caught;
	enum ui_drag drag;
	double down_x;
	double down_y;
	double finger_x;
	double finger_y;
	uint64_t edge_us;

	/* The inputs no part took, a ring. */
	struct kl_event events[UI_EVENTS];
	unsigned event_first;
	unsigned event_count;

	/* The widget with the keyboard's focus, and the keys waiting for the next frame's widgets. */
	struct ui_key focus;
	int focus_ring;
	struct ui_press keys[UI_KEYS];
	unsigned key_count;

	/* The time of the frame being drawn (kl_ui_begin's). */
	uint64_t now_us;

	/*
	 * The scroll a touch pad's two fingers hold (KL_VERSION 40, BUG-211):
	 * the one under the pointer when they began, until they lift.  It is
	 * the application's; it is looked for among the frame's scrolls before
	 * each use, as the application may have let it go.  NULL when none.
	 */
	struct kl_scroll *axis_scroll;

	/* The keyboard inset last acted on (ui_inset's serial, ws102-p015). */
	unsigned inset_serial;

	/*
	 * The text an input method is composing (BUG-203): the text, its
	 * cursor's byte offsets (-1 when hidden), and the widget that had the
	 * focus when it came, which shows it at its caret.  An empty text is
	 * none; the widget drawn without the focus drops it.
	 */
	char preedit[KL_WINDOW_TEXT_MAX];
	int32_t preedit_begin;
	int32_t preedit_end;
	struct ui_key preedit_target;

	/*
	 * Whether the focused widget of the frame being drawn, and of the frame
	 * shown, takes text from an input method, and its caret's rectangle
	 * (keiui_ui_text_caret); kl_ui_end moves the drawing's to the shown.
	 */
	int text_drawing;
	struct kl_rect text_drawing_caret;
	int text_shown;
	struct kl_rect text_shown_caret;

	/*
	 * The part of the window a change of the lit widget alone needs drawn
	 * again (BUG-226): the regions of the widget lit before and of the one
	 * lit now in the frame shown, with UI_DAMAGE_MARGIN round them.
	 * damage_pending until kl_ui_take_damage takes it or a frame begins;
	 * damage_whole when a change could not be placed (the whole window).
	 */
	int damage_pending;
	int damage_whole;
	struct kl_rect damage;

	/*
	 * ws177-p013: the window the input is of, with its clipboard's calls
	 * (kl_ui_window_text ties them; NULL before: the clipboard is then
	 * none), and the text widgets' history of changes.
	 */
	struct kl_window *window;
	void (*copy)(struct kl_window *window, const char *text, size_t length);
	size_t (*paste)(struct kl_window *window, char *text, size_t size);
	struct ui_undo undo;

	/*
	 * ws190-p002: the window's calls the bar asks of it (whether the
	 * clipboard has text, the keyboard's inset; NULL before
	 * kl_ui_window_text), the fingers' selection, whether the program
	 * turned it off and where its bar may stand (kl_ui_set_text_bar), the
	 * bar's command waiting for the owner (a KL_TEXT_BAR_* button, 0 for
	 * none), and the region under a handle a finger touched (its two
	 * fingers scroll it).
	 */
	int (*can_paste)(const struct kl_window *window);
	void (*keyboard_inset)(const struct kl_window *window, int *right, int *bottom);
	struct keiui_select select;
	int select_made;
	int select_off;
	int select_bounded;
	struct kl_rect select_bounds;
	unsigned bar_command;
	struct ui_record touch_below;
	int touch_has_below;
};

/*
 * The on-screen keyboard's inset a window heard last (window.c,
 * keiui_ui_inset_note; ws102-p015): a serial counted up with each, the
 * window's size, the widths covered from its right and bottom edges, and
 * the reason (KL_KEYBOARD_INSET_*), and the caret's rectangle in the
 * window the application told (kl_window_text_cursor; height 0 when it
 * has not).  Each window's input acts on a serial
 * once (kl_ui_end).  The library runs on one thread.
 */
static struct {
	unsigned serial;
	uint32_t width;
	uint32_t height;
	int right;
	int bottom;
	unsigned reason;
	int32_t caret[4];
} ui_inset;

static const struct ui_record *ui_find(const struct kl_ui *ui, double x, double y, int regions);
static void ui_damage_key(struct kl_ui *ui, const struct ui_key *key);
static int ui_scroll_shown(const struct kl_ui *ui, const struct kl_scroll *scroll);
static int ui_inside(const struct kl_rect *rect, double x, double y);
static int ui_same(const struct ui_key *key, uint32_t id, uint32_t index);
static int ui_target(const struct ui_key *target, uint32_t id, uint32_t index);
static void ui_click(struct kl_ui *ui, const struct ui_key *key, int twice, uint64_t now_us);
static struct kl_event *ui_push(struct kl_ui *ui, unsigned kind, double x, double y);
static void ui_gestures(struct kl_ui *ui, uint64_t now_us);
static void ui_gesture(struct kl_ui *ui, const struct kl_gesture_event *gesture, uint64_t now_us);
static void ui_drag_step(struct kl_ui *ui, uint64_t now_us);
static void ui_content(const struct kl_ui *ui, double x, double y, double *content_x, double *content_y);
static void ui_record(struct kl_ui *ui, enum ui_kind kind, uint32_t id, uint32_t index, unsigned flags, const struct kl_rect *rect, struct kl_scroll *scroll, struct kl_text_touch *touch);
static void ui_focus_press(struct kl_ui *ui, const struct ui_record *record, double x, double y);
static const struct ui_record *ui_focus_owner(const struct ui_key *key, const struct kl_ui *ui, double x, double y);
static int ui_focus_move(struct kl_ui *ui, int backward);
static const struct ui_record *ui_find_handle(const struct kl_ui *ui, double x, double y);
static void ui_select_overlay(struct kl_ui *ui);
static int ui_select_covered(const struct kl_ui *ui);
static void ui_select_handles(struct kl_ui *ui);
static void ui_select_bar(struct kl_ui *ui);
static void ui_select_bounds(const struct kl_ui *ui, struct kl_rect *bounds);
static void ui_select_command(struct kl_ui *ui);
static int ui_rects_meet(const struct kl_rect *first, const struct kl_rect *second);
static int ui_inset_center(struct kl_ui *ui, uint64_t now_us);
static void ui_undo_forget(struct ui_undo *undo);
static void ui_undo_drop(struct ui_undo_step *step);
static uint64_t ui_hash(const char *text, size_t length);
static int ui_word_byte(char byte);

/*
 * Makes a window's input state.
 *
 * Returns NULL when memory is short.
 */
struct kl_ui *
kl_ui_create(void)
{
	struct kl_ui *ui;
	int error;

	/* The state. */
	ui = calloc(1, sizeof(*ui));
	if (ui == NULL)
		return NULL;

	/* The records of the frame shown. */
	ui->shown = calloc(UI_RECORDS, sizeof(ui->shown[0]));
	if (ui->shown == NULL) {
		kl_ui_destroy(ui);
		return NULL;
	}

	/* The records of the frame being drawn. */
	ui->drawing = calloc(UI_RECORDS, sizeof(ui->drawing[0]));
	if (ui->drawing == NULL) {
		kl_ui_destroy(ui);
		return NULL;
	}

	/* The gestures of the fingers. */
	ui->gesture = kl_gesture_create();
	if (ui->gesture == NULL) {
		kl_ui_destroy(ui);
		return NULL;
	}

	/* The fingers' selection's scroll (ws190-p002), which the records of a frame may point at until the input goes. */
	error = kl_scroll_init(&ui->select.scroll, KL_SCROLL_X);
	if (error != 0) {
		kl_ui_destroy(ui);
		return NULL;
	}

	/* The scroll is released with the input. */
	ui->select_made = 1;

	/* Succeeded: no frame yet, so input meets no part. */
	return ui;
}

/*
 * Frees a window's input state.
 */
void
kl_ui_destroy(
	struct kl_ui *ui)
{
	/* No state, nothing to free. */
	if (ui == NULL)
		return;

	/* The fingers' selection's scroll and a text area's lines. */
	if (ui->select_made)
		kl_scroll_release(&ui->select.scroll);
	free(ui->select.layout);

	/* The text widgets' history, the gestures and the records. */
	ui_undo_forget(&ui->undo);
	if (ui->gesture != NULL)
		kl_gesture_destroy(ui->gesture);
	free(ui->shown);
	free(ui->drawing);
	free(ui);
}

/*
 * The pointer moves to a point of the window.
 */
int
kl_ui_pointer_motion(
	struct kl_ui *ui,
	double x,
	double y)
{
	const struct ui_record *record;
	struct ui_key hot;
	int same;

	/* The place; a widget held by a press (a slider's knob) follows it. */
	ui->pointer_x = x;
	ui->pointer_y = y;
	ui->pointer_inside = 1;
	if (ui->active.valid && (ui->active.flags & KEIUI_DRAGGABLE) != 0U)
		return 1;

	/* The widget under it. */
	memset(&hot, 0, sizeof(hot));
	record = ui_find(ui, x, y, 0);
	if (record != NULL && record->kind == UI_KIND_HIT) {
		hot.valid = 1;
		hot.id = record->id;
		hot.index = record->index;
	}

	/* The same widget (or none again): nothing to draw. */
	same = 0;
	if (hot.valid && ui->hot.valid)
		same = ui_same(&ui->hot, hot.id, hot.index);
	if (!hot.valid && !ui->hot.valid)
		same = 1;
	if (same)
		return 0;

	/* Succeeded: another widget is lit; the two widgets' regions are drawn again. */
	ui_damage_key(ui, &ui->hot);
	ui->hot = hot;
	ui_damage_key(ui, &ui->hot);
	return 1;
}

/*
 * The pointer leaves the window.
 */
int
kl_ui_pointer_leave(
	struct kl_ui *ui)
{
	/* Nothing lit any more. */
	ui->pointer_inside = 0;
	if (!ui->hot.valid)
		return 0;

	/* Succeeded: the lit widget goes dark, its region drawn again. */
	ui_damage_key(ui, &ui->hot);
	ui->hot.valid = 0;
	return 1;
}

/*
 * Forgets the press the pointer holds on a widget without a click
 * (KL_VERSION 70, ws189-p002): the press became a drag and drop, whose
 * release the compositor keeps.  Returns 1 when a press was held.
 */
int
kl_ui_pointer_cancel(
	struct kl_ui *ui)
{
	/* No widget held. */
	if (!ui->active.valid)
		return 0;

	/* Succeeded: the widget is let go unclicked, its region drawn again. */
	ui_damage_key(ui, &ui->active);
	ui->active.valid = 0;
	ui->released.valid = 0;
	return 1;
}

/*
 * Takes the part of the window that the changes of the lit widget since
 * the last frame (kl_ui_pointer_motion, kl_ui_pointer_leave) need drawn
 * again, when that is all that changed (KL_VERSION 62, BUG-226): the
 * application may draw its next frame within it alone (kl_canvas_clip_push
 * round the whole drawing; the rest of the frame keeps its pixels).
 * Returns 1 with the part, 0 when there is none, or when the whole window
 * is to be drawn again.
 */
int
kl_ui_take_damage(
	struct kl_ui *ui,
	struct kl_rect *rect)
{
	int pending;
	int whole;

	/* What there is, taken. */
	pending = ui->damage_pending;
	whole = ui->damage_whole;
	ui->damage_pending = 0;
	ui->damage_whole = 0;

	/* Nothing, or no part that can be told. */
	if (!pending || whole)
		return 0;

	/* Succeeded: the part. */
	*rect = ui->damage;
	return 1;
}

/*
 * The main button is pressed or released at the pointer.
 */
int
kl_ui_pointer_button(
	struct kl_ui *ui,
	int pressed,
	uint64_t now_us)
{
	const struct ui_record *record;
	struct kl_event *event;
	int same;

	/* The part under the pointer, of any kind. */
	record = ui_find(ui, ui->pointer_x, ui->pointer_y, 0);

	/* A press on a widget holds it (and one that takes the keyboard takes the focus). */
	if (pressed) {
		ui_focus_press(ui, record, ui->pointer_x, ui->pointer_y);
		if (record != NULL && record->kind == UI_KIND_HIT) {
			ui->active.valid = 1;
			ui->active.id = record->id;
			ui->active.index = record->index;
			ui->active.flags = record->flags;
			return 1;
		}

		/* Anywhere else: the application's press, naming the region under it. */
		event = ui_push(ui, KL_EVENT_PRESS, ui->pointer_x, ui->pointer_y);
		if (event != NULL && record != NULL)
			event->region = record->id;
		return 1;
	}

	/* A release on the widget pressed clicks it; a widget that follows the pointer sees it held for one frame more (the motion just before). */
	if (ui->active.valid) {
		if ((ui->active.flags & KEIUI_DRAGGABLE) != 0U)
			ui->released = ui->active;
		same = 0;
		if (record != NULL && record->kind == UI_KIND_HIT)
			same = ui_same(&ui->active, record->id, record->index);
		if (same)
			ui_click(ui, &ui->active, 0, now_us);
		ui->active.valid = 0;
		return 1;
	}

	/* A release of a press nothing took is the application's. */
	event = ui_push(ui, KL_EVENT_RELEASE, ui->pointer_x, ui->pointer_y);
	if (event != NULL && record != NULL)
		event->region = record->id;
	return 1;
}

/*
 * The wheel turns by dx, dy pixels at the pointer: the scroll under it
 * glides, else the application hears it.
 */
int
kl_ui_wheel(
	struct kl_ui *ui,
	double dx,
	double dy,
	uint64_t now_us)
{
	const struct ui_record *record;
	struct kl_event *event;

	/* The scroll or text view under the pointer. */
	record = ui_find(ui, ui->pointer_x, ui->pointer_y, 1);
	if (record != NULL && record->scroll != NULL) {
		kl_scroll_wheel(record->scroll, dx, dy, now_us);
		return 1;
	}

	/* Nothing scrolls there: the application's wheel. */
	event = ui_push(ui, KL_EVENT_WHEEL, ui->pointer_x, ui->pointer_y);
	if (event == NULL)
		return 0;
	event->dx = dx;
	event->dy = dy;
	return 1;
}

/*
 * Takes a window's scrolling input (KL_VERSION 40, BUG-211): a wheel's
 * axis event glides the scroll under the pointer as kl_ui_wheel does; a
 * touch pad's fingers hold the scroll under the pointer when they began
 * and move it at once, and their end (KL_WINDOW_AXIS_STOP) lets it fly on.
 * Scrolling with nothing to scroll is the application's wheel.  Returns 1
 * when the input was taken (or queued for the application).
 */
int
kl_ui_axis(
	struct kl_ui *ui,
	const struct kl_window_event *event)
{
	int flung;
	const struct ui_record *record;
	struct kl_scroll *scroll;
	int shown;
	int taken;

	/* The fingers' end: the scroll they held flies on, if the frame still has it. */
	if (event->kind == KL_WINDOW_AXIS_STOP) {
		scroll = ui->axis_scroll;
		ui->axis_scroll = NULL;
		if (scroll == NULL)
			return 0;
		shown = ui_scroll_shown(ui, scroll);
		flung = 0;
		if (shown)
			flung = kl_scroll_axis_stop(scroll, event->time_us);
		if (flung)
			return KL_UI_AXIS_FLUNG;
		return 1;
	}

	/* Nothing else but scrolling. */
	if (event->kind != KL_WINDOW_AXIS)
		return 0;

	/* A wheel's, or something continuous: the glide under the pointer. */
	if (event->axis_source != KL_AXIS_SOURCE_FINGER) {
		taken = kl_ui_wheel(ui, event->dx, event->dy, event->arrival_us);
		return taken;
	}

	/* The fingers: the scroll they already hold, while the frame has it, else the one under the pointer. */
	scroll = ui->axis_scroll;
	if (scroll != NULL) {
		shown = ui_scroll_shown(ui, scroll);
		if (!shown)
			scroll = NULL;
	}
	if (scroll == NULL) {
		record = ui_find(ui, ui->pointer_x, ui->pointer_y, 1);
		if (record != NULL)
			scroll = record->scroll;
	}

	/* Nothing scrolls there: the application's wheel. */
	if (scroll == NULL) {
		taken = kl_ui_wheel(ui, event->dx, event->dy, event->arrival_us);
		return taken;
	}

	/* The scroll held and moved at once. */
	ui->axis_scroll = scroll;
	kl_scroll_axis(scroll, event->dx, event->dy, KL_AXIS_SOURCE_FINGER, event->time_us);

	/* Succeeded: the fingers' scrolling is taken. */
	return 1;
}

/*
 * A finger touches: the first finger chooses the touch's targets.
 */
int
kl_ui_touch_down(
	struct kl_ui *ui,
	int32_t id,
	uint64_t time_us,
	uint64_t now_us,
	double x,
	double y)
{
	const struct ui_record *record;
	const struct ui_record *handle;
	int error;

	/* The first finger: the widget under it and the scroll or text view under it. */
	if (ui->fingers == 0U) {
		memset(&ui->touch_hit, 0, sizeof(ui->touch_hit));
		record = ui_find(ui, x, y, 0);
		if (record != NULL && record->kind == UI_KIND_HIT) {
			ui->touch_hit.valid = 1;
			ui->touch_hit.id = record->id;
			ui->touch_hit.index = record->index;
			ui->touch_hit.flags = record->flags;
		}

		/* The region (a scroll's press stops a glide or a flight). */
		ui->touch_has_region = 0;
		ui->touch_has_below = 0;
		ui->caught = 0;
		record = ui_find(ui, x, y, 1);
		handle = ui_find_handle(ui, x, y);
		if (record != NULL && handle == NULL) {
			ui->touch_region = *record;
			ui->touch_has_region = 1;
			ui->caught = kl_scroll_press(record->scroll, now_us);
		}

		/*
		 * A handle of the fingers' selection under the finger (ws190-p002)
		 * is the touch's region, its content in the text box; the region
		 * under it is kept, not pressed, for two fingers, which scroll it.
		 */
		if (handle != NULL) {
			ui->touch_hit.valid = 0;
			ui->touch_region = *handle;
			ui->touch_region.rect = handle->view;
			ui->touch_has_region = 1;
			(void)kl_scroll_press(handle->scroll, now_us);
			if (record != NULL) {
				ui->touch_below = *record;
				ui->touch_has_below = 1;
			}
		}

		/* No drag yet. */
		ui->drag = UI_DRAG_NONE;
		ui->down_x = x;
		ui->down_y = y;
	}

	/* The finger to the gestures. */
	error = kl_gesture_down(ui->gesture, id, time_us, now_us, x, y);
	if (error == 0)
		ui->fingers++;

	/* What the fingers mean so far. */
	ui_gestures(ui, now_us);
	return 1;
}

/*
 * A finger moves.
 */
int
kl_ui_touch_motion(
	struct kl_ui *ui,
	int32_t id,
	uint64_t time_us,
	uint64_t now_us,
	double x,
	double y)
{
	/* To the gestures, and what they mean. */
	(void)kl_gesture_motion(ui->gesture, id, time_us, now_us, x, y);
	ui_gestures(ui, now_us);

	/* A drag moves something each frame. */
	if (ui->drag != UI_DRAG_NONE)
		return 1;
	return 0;
}

/*
 * A finger lifts.
 */
int
kl_ui_touch_up(
	struct kl_ui *ui,
	int32_t id,
	uint64_t time_us,
	uint64_t now_us)
{
	int error;

	/* To the gestures. */
	error = kl_gesture_up(ui->gesture, id, time_us);
	if (error == 0 && ui->fingers > 0U)
		ui->fingers--;

	/* What the fingers mean now (a tap, the end of a drag). */
	ui_gestures(ui, now_us);
	return 1;
}

/*
 * The compositor took the fingers away.
 */
int
kl_ui_touch_cancel(
	struct kl_ui *ui,
	uint64_t now_us)
{
	/* The gestures end without their lift. */
	kl_gesture_cancel(ui->gesture);
	ui->fingers = 0;
	ui_gestures(ui, now_us);
	return 1;
}

/*
 * Starts drawing a frame at a time: the gestures found by now, a drag
 * followed, and every scroll of the frame shown moved on to the time.
 */
void
kl_ui_begin(
	struct kl_ui *ui,
	uint64_t now_us)
{
	size_t index;

	/* The frame's time; a long press is found by the clock. */
	ui->now_us = now_us;
	ui_gestures(ui, now_us);

	/* A drag moves its scroll or its selection. */
	ui_drag_step(ui, now_us);

	/* Each scroll of the frame shown, at the frame's time. */
	for (index = 0; index < ui->shown_count; index++) {
		if (ui->shown[index].scroll != NULL)
			(void)kl_scroll_step(ui->shown[index].scroll, now_us);
	}

	/* The frame being drawn records its parts from none, and no widget of it takes text yet. */
	ui->drawing_count = 0;
	ui->text_drawing = 0;

	/* The fingers' selection's field has not been drawn in it yet (ws190-p002). */
	ui->select.drawn = 0;

	/* A frame drawn covers the changes of the lit widget so far. */
	ui->damage_pending = 0;
	ui->damage_whole = 0;
}

/*
 * Records a widget of the frame being drawn and reports what the input
 * did to it (KL_HIT_* bits).
 */
unsigned
kl_ui_hit(
	struct kl_ui *ui,
	uint32_t id,
	uint32_t index,
	const struct kl_rect *rect)
{
	unsigned state;
	int same;

	/* The record, for the input until the next frame. */
	ui_record(ui, UI_KIND_HIT, id, index, 0U, rect, NULL, NULL);

	/* Lit: the pointer is over it. */
	state = 0;
	same = 0;
	if (ui->hot.valid)
		same = ui_same(&ui->hot, id, index);
	if (same)
		state |= KL_HIT_HOT;

	/* Held: a press on it has not been released (or a drag of it was let go since the last frame). */
	same = 0;
	if (ui->active.valid)
		same = ui_same(&ui->active, id, index);
	if (ui->released.valid && !same)
		same = ui_same(&ui->released, id, index);
	if (same)
		state |= KL_HIT_ACTIVE;

	/* Clicked since the last frame, once or as the second of two. */
	same = 0;
	if (ui->clicked.valid)
		same = ui_same(&ui->clicked, id, index);
	if (same) {
		state |= KL_HIT_CLICKED;
		if (ui->clicked_double)
			state |= KL_HIT_DOUBLE;
		if (ui->clicked_touch)
			state |= KL_HIT_TOUCHED;
	}

	/* Reports the state. */
	return state;
}

/*
 * Records a scroll's viewport of the frame being drawn: the wheel and a
 * finger's drag over it move the scroll.
 */
void
kl_ui_scroll_region(
	struct kl_ui *ui,
	uint32_t id,
	const struct kl_rect *rect,
	struct kl_scroll *scroll)
{
	/* The record. */
	ui_record(ui, UI_KIND_SCROLL, id, 0U, 0U, rect, scroll, NULL);
}

/*
 * Records a view of editable text of the frame being drawn: its scroll
 * takes the wheel and two fingers, its touch takes one finger.
 */
void
kl_ui_text_region(
	struct kl_ui *ui,
	uint32_t id,
	const struct kl_rect *rect,
	struct kl_scroll *scroll,
	struct kl_text_touch *touch)
{
	/* The record. */
	ui_record(ui, UI_KIND_TEXT, id, 0U, 0U, rect, scroll, touch);
}

/*
 * Ends a frame: its parts become the ones input is resolved against, and
 * a click is used up.  Returns 1 while something moves by itself (the
 * window should draw the next frame).
 */
int
kl_ui_end(
	struct kl_ui *ui,
	uint64_t now_us)
{
	struct ui_record *swap;
	struct kl_scroll *scroll;
	struct kl_event *event;
	size_t index;
	unsigned kept;
	int moving;
	int delivered;
	int held;
	int lit;
	int moved;

	/* The fingers' selection's handles and bar over the frame, or its end (ws190-p002). */
	ui_select_overlay(ui);

	/* The frame drawn is the one on the screen now. */
	swap = ui->shown;
	ui->shown = ui->drawing;
	ui->shown_count = ui->drawing_count;
	ui->drawing = swap;
	ui->drawing_count = 0;

	/* A click, and a drag let go, are seen by one frame. */
	ui->clicked.valid = 0;
	ui->released.valid = 0;
	ui->clicked_double = 0;
	ui->clicked_touch = 0;

	/* Whether the frame now shown has a focused widget that takes text, and where its caret is. */
	ui->text_shown = ui->text_drawing;
	ui->text_shown_caret = ui->text_drawing_caret;

	/*
	 * The oldest key no widget took is the application's; the keys after
	 * it wait for the next frame, so that they are carried out after it
	 * (a letter that selects an item, then Enter that opens it).
	 */
	kept = 0;
	delivered = 0;
	for (index = 0; index < ui->key_count; index++) {
		if (ui->keys[index].taken)
			continue;
		if (delivered) {
			ui->keys[kept] = ui->keys[index];
			kept++;
			continue;
		}

		/* A text no widget took has nowhere else to go (the focused widget takes no text): it is dropped. */
		if (ui->keys[index].kind != KEIUI_INPUT_KEY)
			continue;

		/* The bar's command its field did not take is not the application's key (ws190-p002). */
		if (ui->keys[index].from_bar)
			continue;

		/* The first is the application's. */
		delivered = 1;
		event = ui_push(ui, KL_EVENT_KEY, ui->pointer_x, ui->pointer_y);
		if (event == NULL)
			continue;
		event->code = ui->keys[index].code;
		event->modifiers = ui->keys[index].modifiers;
	}

	/* The keys waiting; they want another frame. */
	ui->key_count = kept;

	/* The bar's command, for its field in the next frame (after the keys already waiting). */
	ui_select_command(ui);

	/* Fingers down, a drag or keys waiting want frames. */
	moving = 0;
	if (ui->fingers > 0U || ui->drag != UI_DRAG_NONE || ui->key_count > 0U)
		moving = 1;

	/* The widget under a still pointer, in the frame drawn (a list scrolled under it); another is lit in the next frame. */
	held = 0;
	if (ui->active.valid && (ui->active.flags & KEIUI_DRAGGABLE) != 0U)
		held = 1;
	if (ui->pointer_inside && !held) {
		lit = kl_ui_pointer_motion(ui, ui->pointer_x, ui->pointer_y);
		if (lit)
			moving = 1;
	}

	/* A keyboard that came or changed keeps the text view's caret in sight (the frame just drawn has the view). */
	if (ui->inset_serial != ui_inset.serial) {
		ui->inset_serial = ui_inset.serial;
		if (ui_inset.reason != KL_KEYBOARD_INSET_NONE) {
			moved = ui_inset_center(ui, now_us);
			if (moved)
				moving = 1;
		}
	}

	/* So do scrolls that glide, fly, or whose bars fade. */
	for (index = 0; index < ui->shown_count; index++) {
		scroll = ui->shown[index].scroll;
		if (scroll == NULL)
			continue;
		if (scroll->gliding || scroll->touched)
			moving = 1;
		if (scroll->moved_us != 0U && now_us >= scroll->moved_us && now_us - scroll->moved_us < KL_SCROLL_FADE_US)
			moving = 1;
	}

	/* Reports whether another frame is wanted. */
	return moving;
}

/*
 * Takes the oldest input no part took.  Returns 1 with it in *event, 0
 * when there is none.
 */
int
kl_ui_take(
	struct kl_ui *ui,
	struct kl_event *event)
{
	/* None left. */
	if (ui->event_count == 0U)
		return 0;

	/* The oldest. */
	*event = ui->events[ui->event_first];
	ui->event_first = (ui->event_first + 1U) % UI_EVENTS;
	ui->event_count--;

	/* Succeeded: one input. */
	return 1;
}

/*
 * Gives how far the fingers of a drag no part took have moved since it
 * began, resampled for a frame at a time.  Returns ENOENT when no such
 * drag goes on.
 */
int
kl_ui_drag_offset(
	struct kl_ui *ui,
	uint64_t now_us,
	double *dx,
	double *dy)
{
	int error;

	/* Only the application's own drag. */
	if (ui->drag != UI_DRAG_OUTSIDE)
		return ENOENT;

	/* The gestures' offset. */
	error = kl_gesture_drag_offset(ui->gesture, now_us, dx, dy);
	if (error != 0)
		return error;

	/* Succeeded: the offset. */
	return 0;
}

/*
 * A key is pressed or released: Tab and Shift+Tab move the focus among the
 * widgets that take the keyboard; another press waits for the next
 * frame's focused widget (or, taken by none, becomes the application's).
 * Returns 1 when the window must draw again.
 */
int
kl_ui_key(
	struct kl_ui *ui,
	uint32_t key,
	int pressed,
	unsigned modifiers)
{
	struct kl_event *event;
	int moved;

	/* Releases do nothing. */
	if (!pressed)
		return 0;

	/* Tab moves the focus, when there is somewhere to move it. */
	if (key == UI_KEY_TAB && (modifiers & (KL_MOD_CTRL | KL_MOD_ALT)) == 0U) {
		moved = ui_focus_move(ui, (modifiers & KL_MOD_SHIFT) != 0U);
		if (moved)
			return 1;
	}

	/* Without a focused widget (and no key waiting before it) the key is the application's at once. */
	if ((!ui->focus.valid && ui->key_count == 0U) || ui->key_count == UI_KEYS) {
		event = ui_push(ui, KL_EVENT_KEY, ui->pointer_x, ui->pointer_y);
		if (event == NULL)
			return 0;
		event->code = key;
		event->modifiers = modifiers;
		return 1;
	}

	/* The key waits for the focused widget. */
	ui->keys[ui->key_count].kind = KEIUI_INPUT_KEY;
	ui->keys[ui->key_count].code = key;
	ui->keys[ui->key_count].modifiers = modifiers;
	ui->keys[ui->key_count].target = ui->focus;
	ui->keys[ui->key_count].taken = 0;
	ui->keys[ui->key_count].from_bar = 0;
	ui->key_count++;

	/* Succeeded: the next frame carries it out. */
	return 1;
}

/*
 * Gives a widget the keyboard's focus.
 */
void
kl_ui_set_focus(
	struct kl_ui *ui,
	uint32_t id,
	uint32_t index)
{
	/* The widget (the flags come when it is drawn). */
	ui->focus.valid = 1;
	ui->focus_ring = 0;
	ui->focus.id = id;
	ui->focus.index = index;
	ui->focus.flags = KEIUI_FOCUSABLE;
}

/*
 * Takes the keyboard's focus from every widget.
 */
void
kl_ui_clear_focus(
	struct kl_ui *ui)
{
	/* No widget. */
	ui->focus.valid = 0;
}

/*
 * Tells whether a widget has the keyboard's focus.
 */
int
kl_ui_has_focus(
	const struct kl_ui *ui,
	uint32_t id,
	uint32_t index)
{
	int same;

	/* No widget has it. */
	if (!ui->focus.valid)
		return 0;

	/* This one, or another. */
	same = ui_same(&ui->focus, id, index);
	return same;
}

/*
 * Reports the point of the input a widget follows: the pointer, or the
 * finger that tapped or drags a widget.
 */
void
kl_ui_pointer(
	const struct kl_ui *ui,
	double *x,
	double *y)
{
	/* The point. */
	*x = ui->pointer_x;
	*y = ui->pointer_y;
}

/*
 * Gives the widget with the focus a text an input method or the on-screen
 * keyboard sent (a KL_WINDOW_TEXT_* input of the window, KL_VERSION 38).
 *
 * A text to commit and bytes to delete wait with the keys, in the order
 * they came, for the next frame's focused widget; a widget that takes no
 * text lets them go.  The text being composed replaces the one before and
 * shows at the focused widget's caret.  Returns 1 when the window must
 * draw again.
 */
int
kl_ui_text(
	struct kl_ui *ui,
	const struct kl_window_event *event)
{
	struct ui_press *press;

	/* The text being composed replaces the one before, for the widget with the focus now. */
	if (event->kind == KL_WINDOW_TEXT_PREEDIT) {
		memcpy(ui->preedit, event->text, sizeof(ui->preedit));
		ui->preedit[sizeof(ui->preedit) - 1U] = '\0';
		ui->preedit_begin = event->begin;
		ui->preedit_end = event->end;
		ui->preedit_target = ui->focus;
		return 1;
	}

	/* Only a text to commit and bytes to delete wait for the widget; another input is not a text. */
	if (event->kind != KL_WINDOW_TEXT_COMMIT && event->kind != KL_WINDOW_TEXT_DELETE)
		return 0;

	/* Without a focused widget, or with the queue full, the text has nowhere to go. */
	if (!ui->focus.valid || ui->key_count == UI_KEYS)
		return 0;

	/* The text, or the bytes to delete, after the keys before it. */
	press = &ui->keys[ui->key_count];
	memset(press, 0, sizeof(*press));
	press->kind = KEIUI_INPUT_COMMIT;
	if (event->kind == KL_WINDOW_TEXT_DELETE)
		press->kind = KEIUI_INPUT_DELETE;
	memcpy(press->text, event->text, sizeof(press->text));
	press->text[sizeof(press->text) - 1U] = '\0';
	press->before = event->before;
	press->after = event->after;

	/*
	 * It is for the widget that has the focus now; the count makes it wait
	 * for the next frame, like a key, and keeps the window drawing.
	 */
	press->target = ui->focus;
	ui->key_count++;

	/* Succeeded: the next frame carries it out. */
	return 1;
}

/*
 * Tells whether the focused widget of the frame shown takes text from an
 * input method (a text field), with its caret's rectangle in the window.
 *
 * The application asks for the window's text input while it does
 * (kl_window_text_input) and tells where the caret is
 * (kl_window_text_cursor), so that an input method's candidates and the
 * on-screen keyboard stay out of its way.  caret may be NULL.
 */
int
kl_ui_text_wanted(
	const struct kl_ui *ui,
	struct kl_rect *caret)
{
	/* No widget has the focus any more. */
	if (!ui->focus.valid)
		return 0;

	/* The focused widget of the frame shown takes no text. */
	if (!ui->text_shown)
		return 0;

	/* The caret, when the caller wants it. */
	if (caret != NULL)
		*caret = ui->text_shown_caret;

	/* Succeeded: the focused widget takes text. */
	return 1;
}

/*
 * Records a widget with its flags (KEIUI_*) and reports what the input did
 * to it (KL_HIT_* bits, with KL_HIT_FOCUSED).
 */
unsigned
keiui_ui_widget(
	struct kl_ui *ui,
	uint32_t id,
	uint32_t index,
	const struct kl_rect *rect,
	unsigned flags)
{
	unsigned state;
	size_t before;
	int same;

	/* The record, and what kl_ui_hit reports of it; the flags go on the record when it was made (a full frame makes none). */
	before = ui->drawing_count;
	state = kl_ui_hit(ui, id, index, rect);
	if (ui->drawing_count > before)
		ui->drawing[before].flags = flags;

	/* The focus. */
	same = 0;
	if (ui->focus.valid)
		same = ui_same(&ui->focus, id, index);
	if (same)
		state |= KL_HIT_FOCUSED;

	/* Reports the state. */
	return state;
}

/*
 * Takes the next key a widget wants, when the widget has the focus; the
 * keys it does not want stay for the application.  Returns 1 with it, 0
 * when none is left for it.
 */
int
keiui_ui_take_key(
	struct kl_ui *ui,
	uint32_t id,
	uint32_t index,
	keiui_wants_key wants,
	uint32_t *code,
	unsigned *modifiers)
{
	unsigned slot;
	int wanted;
	int same;

	/* The oldest key not taken, pressed while the widget had the focus; one it does not want stops it (the application has it first, in order). */
	for (slot = 0; slot < ui->key_count; slot++) {
		if (ui->keys[slot].taken)
			continue;
		same = ui_target(&ui->keys[slot].target, id, index);
		if (!same)
			continue;

		/* A text sent for it is not a key: only a widget that edits text takes it (keiui_ui_take_input). */
		if (ui->keys[slot].kind != KEIUI_INPUT_KEY)
			return 0;

		/* The key, when the widget wants it. */
		wanted = wants(ui->keys[slot].code, ui->keys[slot].modifiers);
		if (!wanted)
			return 0;
		ui->keys[slot].taken = 1;
		*code = ui->keys[slot].code;
		*modifiers = ui->keys[slot].modifiers;
		return 1;
	}

	/* None left. */
	return 0;
}

/*
 * Takes Enter and Space waiting for a focused widget, leaving the other
 * keys for the application.  Returns 1 when one was pressed.
 */
int
keiui_ui_take_activate(
	struct kl_ui *ui,
	uint32_t id,
	uint32_t index)
{
	unsigned slot;
	uint32_t code;
	int pressed;
	int same;

	/* Each Enter and Space not taken yet, pressed while the widget had the focus, up to another key of it (the application's first, in order). */
	pressed = 0;
	for (slot = 0; slot < ui->key_count; slot++) {
		code = ui->keys[slot].code;
		if (ui->keys[slot].taken)
			continue;
		same = ui_target(&ui->keys[slot].target, id, index);
		if (!same)
			continue;
		if (ui->keys[slot].kind != KEIUI_INPUT_KEY)
			break;
		if (code != KL_KEY_ENTER && code != KL_KEY_KPENTER && code != KL_KEY_SPACE)
			break;
		ui->keys[slot].taken = 1;
		pressed = 1;
	}

	/* Reports whether one was pressed. */
	return pressed;
}

/*
 * Tells which editing command a key with its modifiers is (ws177-p013):
 * Ctrl+Z undo, Ctrl+Shift+Z and Ctrl+Y redo, Ctrl+C copy, Ctrl+X cut,
 * Ctrl+V paste, Ctrl+Left and Ctrl+Right a word (with Shift too).
 * Returns KEIUI_EDIT_NONE for any other key.
 */
unsigned
keiui_edit_command(
	uint32_t code,
	unsigned modifiers)
{
	unsigned shift;

	/* Only keys with Control, without Alt or Super. */
	if ((modifiers & KL_MOD_CTRL) == 0U)
		return KEIUI_EDIT_NONE;
	if ((modifiers & (KL_MOD_ALT | KL_MOD_SUPER)) != 0U)
		return KEIUI_EDIT_NONE;
	shift = modifiers & KL_MOD_SHIFT;

	/* Each command's key. */
	switch (code) {
	case UI_KEY_Z:
		if (shift != 0U)
			return KEIUI_EDIT_REDO;
		return KEIUI_EDIT_UNDO;
	case UI_KEY_Y:
		return KEIUI_EDIT_REDO;
	case UI_KEY_C:
		return KEIUI_EDIT_COPY;
	case UI_KEY_X:
		return KEIUI_EDIT_CUT;
	case UI_KEY_V:
		return KEIUI_EDIT_PASTE;
	case KL_KEY_LEFT:
		return KEIUI_EDIT_WORD_LEFT;
	case KL_KEY_RIGHT:
		return KEIUI_EDIT_WORD_RIGHT;
	default:
		break;
	}

	/* Not a command. */
	return KEIUI_EDIT_NONE;
}

/*
 * Records a text widget's change in its history: the bytes the change
 * took out and put in (the text before and after, less what they share at
 * both ends), with the caret and the selection before it.  A widget other
 * than the history's, or a text the history does not end with (the
 * application set it), starts a new history; the changes taken back are
 * forgotten.  Memory running out forgets the history.
 */
void
keiui_edit_record(
	struct kl_ui *ui,
	uint32_t id,
	const char *before,
	size_t before_length,
	size_t caret_before,
	size_t anchor_before,
	const char *after,
	size_t after_length)
{
	struct ui_undo *undo;
	struct ui_undo_step *step;
	size_t prefix;
	size_t suffix;
	size_t index;
	uint64_t hash;

	/* A new history for another widget, or for a text set apart from it. */
	undo = &ui->undo;
	hash = ui_hash(before, before_length);
	if (!undo->valid || undo->id != id || undo->hash != hash) {
		ui_undo_forget(undo);
		undo->valid = 1;
		undo->id = id;
	}

	/* What the two texts share at their start, then at their end (not overlapping). */
	prefix = 0;
	while (prefix < before_length && prefix < after_length && before[prefix] == after[prefix])
		prefix++;
	suffix = 0;
	while (suffix < before_length - prefix &&
	       suffix < after_length - prefix &&
	       before[before_length - 1U - suffix] == after[after_length - 1U - suffix])
		suffix++;

	/* The same text is no change. */
	if (before_length - prefix - suffix == 0U && after_length - prefix - suffix == 0U)
		return;

	/* The changes taken back are forgotten. */
	for (index = undo->done; index < undo->count; index++)
		ui_undo_drop(&undo->steps[index]);
	undo->count = undo->done;

	/* A full history forgets its oldest change. */
	if (undo->count == UI_UNDO_STEPS) {
		ui_undo_drop(&undo->steps[0]);
		memmove(&undo->steps[0], &undo->steps[1], (UI_UNDO_STEPS - 1U) * sizeof(undo->steps[0]));
		undo->count--;
	}

	/* The change: where, and the bytes out and in (one more byte each, so that none is of size 0). */
	step = &undo->steps[undo->count];
	memset(step, 0, sizeof(*step));
	step->at = prefix;
	step->removed_length = before_length - prefix - suffix;
	step->inserted_length = after_length - prefix - suffix;
	step->caret_before = caret_before;
	step->anchor_before = anchor_before;
	step->removed = malloc(step->removed_length + 1U);
	step->inserted = malloc(step->inserted_length + 1U);
	if (step->removed == NULL || step->inserted == NULL) {
		ui_undo_drop(step);
		ui_undo_forget(undo);
		return;
	}

	/* The bytes it took out and put in. */
	memcpy(step->removed, before + prefix, step->removed_length);
	memcpy(step->inserted, after + prefix, step->inserted_length);

	/* Kept, the history now ending with the text after it. */
	undo->count++;
	undo->done = undo->count;
	undo->hash = ui_hash(after, after_length);
}

/*
 * Takes the last change done back (redo 0), or does the first one taken
 * back again (redo 1), on a widget's text of a capacity (with its NUL):
 * the caret and the selection are as before the change, or after it.
 * Returns 1 when a change was, 0 when there is none, the history is
 * another widget's, the text is not the one the history ends with, or the
 * result would not fit.
 */
int
keiui_edit_undo(
	struct kl_ui *ui,
	uint32_t id,
	int redo,
	char *text,
	size_t *length,
	size_t capacity,
	size_t *caret,
	size_t *anchor)
{
	struct ui_undo *undo;
	struct ui_undo_step *step;
	const char *out;
	const char *in;
	size_t out_length;
	size_t in_length;
	size_t new_length;
	uint64_t hash;
	int differs;

	/* The widget's history, ending with the text it has now. */
	undo = &ui->undo;
	if (!undo->valid || undo->id != id)
		return 0;
	hash = ui_hash(text, *length);
	if (hash != undo->hash)
		return 0;

	/* The change: undo puts back what it took out, redo does it again. */
	if (redo) {
		if (undo->done == undo->count)
			return 0;
		step = &undo->steps[undo->done];
		out = step->removed;
		out_length = step->removed_length;
		in = step->inserted;
		in_length = step->inserted_length;
	} else {
		if (undo->done == 0U)
			return 0;
		step = &undo->steps[undo->done - 1U];
		out = step->inserted;
		out_length = step->inserted_length;
		in = step->removed;
		in_length = step->removed_length;
	}

	/* Refuses a text the change does not fit, or that does not have the bytes it takes out. */
	if (step->at + out_length > *length)
		return 0;
	differs = memcmp(text + step->at, out, out_length);
	if (differs != 0)
		return 0;
	new_length = *length - out_length + in_length;
	if (new_length + 1U > capacity)
		return 0;

	/* The bytes after the change move, and its bytes go in. */
	memmove(text + step->at + in_length, text + step->at + out_length, *length - step->at - out_length + 1U);
	memcpy(text + step->at, in, in_length);
	*length = new_length;

	/* The caret: as before the change (undo), or after what it put in (redo). */
	if (redo) {
		*caret = step->at + in_length;
		*anchor = *caret;
		undo->done++;
	} else {
		*caret = step->caret_before;
		*anchor = step->anchor_before;
		if (*caret > new_length)
			*caret = new_length;
		if (*anchor > new_length)
			*anchor = new_length;
		undo->done--;
	}

	/* The history now ends with this text. */
	undo->hash = ui_hash(text, *length);

	/* Succeeded: the change was taken back or done again. */
	return 1;
}

/* Puts a text on the window's clipboard; without a window the text goes nowhere. */
void
keiui_edit_copy(
	struct kl_ui *ui,
	const char *text,
	size_t length)
{
	/* No window tied to the input. */
	if (ui->window == NULL || ui->copy == NULL)
		return;

	/* The window's clipboard. */
	ui->copy(ui->window, text, length);
}

/*
 * Reads the window's clipboard's text into a buffer of a size, ending it
 * with a NUL.  Returns its length (0 without a window or a text).
 */
size_t
keiui_edit_paste(
	struct kl_ui *ui,
	char *text,
	size_t size)
{
	size_t length;

	/* Nothing yet. */
	if (size == 0U)
		return 0;
	text[0] = '\0';

	/* No window tied to the input. */
	if (ui->window == NULL || ui->paste == NULL)
		return 0;

	/* The clipboard's bytes, as many as fit with the NUL. */
	length = ui->paste(ui->window, text, size - 1U);
	if (length > size - 1U)
		length = size - 1U;
	text[length] = '\0';

	/* Succeeded: the text. */
	return length;
}

/*
 * Gives the start of the word before an offset of a text (forward 0: past
 * the spaces and signs before it, then the word), or the end of the word
 * after it (forward 1).  A word is letters, digits, underscores and any
 * byte of a character beyond ASCII, so the offset given is never inside a
 * character.
 */
size_t
keiui_edit_word(
	const char *text,
	size_t length,
	size_t at,
	int forward)
{
	int word;

	/* Forward: past what is not a word, then past the word. */
	if (forward) {
		while (at < length) {
			word = ui_word_byte(text[at]);
			if (word)
				break;
			at++;
		}
		while (at < length) {
			word = ui_word_byte(text[at]);
			if (!word)
				break;
			at++;
		}

		/* The end of the word. */
		return at;
	}

	/* Backward: before what is not a word, then before the word. */
	while (at > 0U) {
		word = ui_word_byte(text[at - 1U]);
		if (word)
			break;
		at--;
	}
	while (at > 0U) {
		word = ui_word_byte(text[at - 1U]);
		if (!word)
			break;
		at--;
	}

	/* The start of the word. */
	return at;
}

/* Ties a window's input to its window and its clipboard's calls (kl_ui_window_text, text-input.c). */
void
keiui_ui_set_window(
	struct kl_ui *ui,
	struct kl_window *window,
	void (*copy)(struct kl_window *, const char *, size_t),
	size_t (*paste)(struct kl_window *, char *, size_t))
{
	/* The window and its calls. */
	ui->window = window;
	ui->copy = copy;
	ui->paste = paste;
}

/*
 * Reports the widget with the focus.  Returns 1 with its id and index, 0
 * when no widget has it.
 */
int
keiui_ui_focused(
	const struct kl_ui *ui,
	uint32_t *id,
	uint32_t *index)
{
	/* None. */
	if (!ui->focus.valid)
		return 0;

	/* Succeeded: the widget. */
	*id = ui->focus.id;
	*index = ui->focus.index;
	return 1;
}

/*
 * Tells whether the focus's ring shows: the keyboard moved the focus (Tab),
 * not a click, a tap or the application.
 */
int
keiui_ui_focus_ring(
	const struct kl_ui *ui)
{
	/* Moved by Tab and still there. */
	if (!ui->focus.valid)
		return 0;
	return ui->focus_ring;
}

/*
 * Takes the next input a widget that edits text wants, in the order they
 * came while it had the focus: a key it wants, a text to commit, or bytes
 * to delete.  Returns 1 with it, 0 when none is left for it (a key it does
 * not want stops it: the application has it first, in order).
 */
int
keiui_ui_take_input(
	struct kl_ui *ui,
	uint32_t id,
	uint32_t index,
	keiui_wants_key wants,
	struct keiui_input *input)
{
	struct ui_press *press;
	unsigned slot;
	int wanted;
	int same;

	/* Nothing taken yet. */
	input->kind = KEIUI_INPUT_NONE;
	input->from_bar = 0;

	/* The oldest input not taken, sent while the widget had the focus. */
	for (slot = 0; slot < ui->key_count; slot++) {
		press = &ui->keys[slot];
		if (press->taken)
			continue;
		same = ui_target(&press->target, id, index);
		if (!same)
			continue;

		/* A key it does not want stops it. */
		if (press->kind == KEIUI_INPUT_KEY) {
			wanted = wants(press->code, press->modifiers);
			if (!wanted)
				return 0;
		}

		/* The input, taken. */
		press->taken = 1;
		input->kind = press->kind;
		input->code = press->code;
		input->modifiers = press->modifiers;
		memcpy(input->text, press->text, sizeof(input->text));
		input->before = press->before;
		input->after = press->after;
		input->from_bar = press->from_bar;

		/* Succeeded: one input for the widget. */
		return 1;
	}

	/* None left. */
	return 0;
}

/*
 * Reports the text an input method is composing for a widget, with its
 * cursor's byte offsets (-1 when hidden), or NULL when none is composed
 * for it.  A widget drawn without the focus drops what was being composed
 * for it, so that it does not come back with the focus.
 */
const char *
keiui_ui_preedit(
	struct kl_ui *ui,
	uint32_t id,
	uint32_t index,
	int focused,
	int32_t *begin,
	int32_t *end)
{
	int same;

	/* Nothing is being composed. */
	if (ui->preedit[0] == '\0')
		return NULL;

	/* It is composed for another widget, or for none. */
	same = ui_target(&ui->preedit_target, id, index);
	if (!same)
		return NULL;

	/* The widget has lost the focus: the composition goes. */
	if (!focused) {
		ui->preedit[0] = '\0';
		return NULL;
	}

	/* Succeeded: the text and its cursor. */
	*begin = ui->preedit_begin;
	*end = ui->preedit_end;
	return ui->preedit;
}

/*
 * Notes that the focused widget being drawn takes text from an input
 * method, with its caret's rectangle in the window (kl_ui_text_wanted
 * reports it once the frame is shown).
 */
void
keiui_ui_text_caret(
	struct kl_ui *ui,
	const struct kl_rect *caret)
{
	/* The frame being drawn has a widget that takes text, with its caret here. */
	ui->text_drawing = 1;
	ui->text_drawing_caret = *caret;
}

/*
 * Reports the time of the frame being drawn (the time kl_ui_begin was
 * given), which the widgets glide and fade by.
 */
uint64_t
keiui_ui_now(
	const struct kl_ui *ui)
{
	/* The frame's time. */
	return ui->now_us;
}

/*
 * Gives a window's input's fingers' selection (ws190-p002), which the
 * text widgets put themselves in and draw it from.
 */
struct keiui_select *
keiui_ui_select(
	struct kl_ui *ui)
{
	/* Succeeded: the input's own. */
	return &ui->select;
}

/*
 * Tells whether a widget is in the fingers' selection's mode.
 */
int
keiui_ui_select_owned(
	struct kl_ui *ui,
	uint32_t id,
	uint32_t index)
{
	/* No mode. */
	if (!ui->select.active)
		return 0;

	/* Another widget's. */
	if (ui->select.id != id || ui->select.index != index)
		return 0;

	/* Succeeded: the widget's. */
	return 1;
}

/*
 * Puts a widget in the fingers' selection's mode: its id and index, what
 * it is (KEIUI_SELECT_*), its view's answers (given the selection as their
 * data) and its address, which only tells it from another widget.
 */
void
keiui_ui_select_begin(
	struct kl_ui *ui,
	uint32_t id,
	uint32_t index,
	int kind,
	const struct kl_text_view *view,
	const void *widget,
	const struct keiui_bar_calls *bar_calls)
{
	struct keiui_select *select;

	/* The owner and its view, a new touch, and the bar's calls. */
	select = &ui->select;
	select->active = 1;
	select->id = id;
	select->index = index;
	select->kind = kind;
	select->widget = widget;
	select->bar_calls = bar_calls;
	kl_text_touch_init(&select->touch, view, select);

	/* A field's content scrolls across, a text area's down. */
	select->scroll.axes = KL_SCROLL_X;
	if (kind == KEIUI_SELECT_AREA)
		select->scroll.axes = KL_SCROLL_Y;
}

/*
 * Takes the fingers' selection's mode away: its handles and bar go, and a
 * finger dragging one of its ends lets go (the widget keeps its selection).
 */
void
keiui_ui_select_end(
	struct kl_ui *ui)
{
	struct keiui_select *select;

	/* No mode. */
	select = &ui->select;
	if (!select->active)
		return;

	/* No owner, no handles, no bar. */
	select->active = 0;
	select->touch.handles = 0;
	select->touch.bar = 0;
	select->touch.selecting = 0;
	ui->bar_command = 0;

	/* A drag of one of its ends is over. */
	if (ui->drag == UI_DRAG_SELECT && ui->touch_region.touch == &select->touch)
		ui->drag = UI_DRAG_NONE;
}

/*
 * Notes that the widget in the fingers' selection was drawn in the frame
 * being drawn: its rectangle, its text box (window coordinates), and the
 * style it was drawn with, copied with the canvas's clip of the moment.
 */
void
keiui_ui_select_drawn(
	struct kl_ui *ui,
	const struct kl_rect *rect,
	const struct kl_rect *box,
	const struct kl_style *style)
{
	struct keiui_select *select;
	size_t index;

	/* Where it is and how it is drawn. */
	select = &ui->select;
	select->rect = *rect;
	select->box = *box;
	select->style = *style;
	select->clip = style->canvas->clip;
	select->drawn = 1;

	/* Its record in the frame being drawn, the last of its id and index. */
	select->order = ui->drawing_count;
	for (index = ui->drawing_count; index > 0U; index--) {
		if (ui->drawing[index - 1U].kind != UI_KIND_HIT)
			continue;
		if (ui->drawing[index - 1U].id != select->id || ui->drawing[index - 1U].index != select->index)
			continue;
		select->order = index - 1U;
		break;
	}
}

/*
 * Tells whether a double tap of a finger may put a field in the fingers'
 * selection (the program did not turn it off).
 */
int
keiui_ui_select_enabled(
	const struct kl_ui *ui)
{
	/* Turned off by the program. */
	if (ui->select_off)
		return 0;

	/* Succeeded: it may. */
	return 1;
}

/*
 * Ties the window's calls the bar asks of it (kl_ui_window_text): whether
 * the clipboard has text to paste, and the on-screen keyboard's inset.
 */
void
keiui_ui_set_window_extras(
	struct kl_ui *ui,
	int (*can_paste)(const struct kl_window *),
	void (*keyboard_inset)(const struct kl_window *, int *, int *))
{
	/* The calls. */
	ui->can_paste = can_paste;
	ui->keyboard_inset = keyboard_inset;
}

/*
 * Turns the fingers' selection of the window's fields on (the default) or
 * off, and gives where its bar may stand (window coordinates; NULL: the
 * whole canvas the fields are drawn on).  KL_VERSION 74.
 */
void
kl_ui_set_text_bar(
	struct kl_ui *ui,
	const struct kl_rect *bounds,
	int enabled)
{
	/* On or off; off ends a selection under way. */
	ui->select_off = 0;
	if (!enabled) {
		ui->select_off = 1;
		keiui_ui_select_end(ui);
	}

	/* The bar's bounds, or the canvas. */
	ui->select_bounded = 0;
	if (bounds != NULL) {
		ui->select_bounded = 1;
		ui->select_bounds = *bounds;
	}
}

/*
 * Gives the part two rectangles share.  Returns 1 with it, 0 when they
 * share no area (result is then untouched).
 */
int
keiui_rect_intersect(
	const struct kl_rect *first,
	const struct kl_rect *second,
	struct kl_rect *result)
{
	int left;
	int top;
	int right;
	int bottom;

	/* The later left and top edges. */
	left = first->x;
	if (second->x > left)
		left = second->x;
	top = first->y;
	if (second->y > top)
		top = second->y;

	/* The earlier right and bottom edges. */
	right = first->x + first->width;
	if (second->x + second->width < right)
		right = second->x + second->width;
	bottom = first->y + first->height;
	if (second->y + second->height < bottom)
		bottom = second->y + second->height;

	/* No area shared. */
	if (right <= left || bottom <= top)
		return 0;

	/* Succeeded: the shared part. */
	result->x = left;
	result->y = top;
	result->width = right - left;
	result->height = bottom - top;
	return 1;
}

/* Finds the latest record of the frame shown under a point: any kind, or (regions) only a scroll or a text view. */
static const struct ui_record *
ui_find(
	const struct kl_ui *ui,
	double x,
	double y,
	int regions)
{
	const struct ui_record *record;
	size_t index;
	int inside;

	/* From the last drawn (on top) down; a handle is the fingers' alone (ui_find_handle). */
	for (index = ui->shown_count; index > 0U; index--) {
		record = &ui->shown[index - 1U];
		if (regions && record->kind == UI_KIND_HIT)
			continue;
		if (record->kind == UI_KIND_HANDLE)
			continue;
		inside = ui_inside(&record->rect, x, y);
		if (inside)
			return record;
	}

	/* Nothing there. */
	return NULL;
}

/*
 * Adds the region of a widget of the frame shown, with UI_DAMAGE_MARGIN
 * round it, to the part a change of the lit widget needs drawn again
 * (BUG-226); a widget the frame shown has no region for asks for the whole
 * window.  No widget adds nothing.
 */
static void
ui_damage_key(
	struct kl_ui *ui,
	const struct ui_key *key)
{
	const struct ui_record *record;
	struct kl_rect grown;
	size_t index;
	int first;
	int same;
	int right;
	int bottom;

	/* No widget. */
	if (!key->valid)
		return;

	/* Its region in the frame shown. */
	record = NULL;
	for (index = 0; index < ui->shown_count; index++) {
		if (ui->shown[index].kind != UI_KIND_HIT)
			continue;
		same = ui_same(key, ui->shown[index].id, ui->shown[index].index);
		if (same) {
			record = &ui->shown[index];
			break;
		}
	}

	/* Something to draw again from now; a widget without a region is the whole window. */
	first = 0;
	if (!ui->damage_pending)
		first = 1;
	ui->damage_pending = 1;
	if (record == NULL) {
		ui->damage_whole = 1;
		return;
	}

	/* The whole window already: nothing to add. */
	if (ui->damage_whole)
		return;

	/* The region and its margin. */
	grown.x = record->rect.x - UI_DAMAGE_MARGIN;
	grown.y = record->rect.y - UI_DAMAGE_MARGIN;
	grown.width = record->rect.width + 2 * UI_DAMAGE_MARGIN;
	grown.height = record->rect.height + 2 * UI_DAMAGE_MARGIN;

	/* The first region is the part. */
	if (first) {
		ui->damage = grown;
		return;
	}

	/* Another widens it to hold both. */
	right = ui->damage.x + ui->damage.width;
	if (grown.x + grown.width > right)
		right = grown.x + grown.width;
	bottom = ui->damage.y + ui->damage.height;
	if (grown.y + grown.height > bottom)
		bottom = grown.y + grown.height;
	if (grown.x < ui->damage.x)
		ui->damage.x = grown.x;
	if (grown.y < ui->damage.y)
		ui->damage.y = grown.y;
	ui->damage.width = right - ui->damage.x;
	ui->damage.height = bottom - ui->damage.y;
}

/* Tells whether a point is in a rectangle. */
static int
ui_inside(
	const struct kl_rect *rect,
	double x,
	double y)
{
	/* Left of it or above it. */
	if (x < (double)rect->x || y < (double)rect->y)
		return 0;

	/* Right of it or below it. */
	if (x >= (double)(rect->x + rect->width) || y >= (double)(rect->y + rect->height))
		return 0;

	/* Inside. */
	return 1;
}

/* Tells whether a key was pressed for a widget (KEIUI_ANY: any of the id's). */
static int
ui_target(
	const struct ui_key *target,
	uint32_t id,
	uint32_t index)
{
	int same;

	/* Pressed while no widget had the focus. */
	if (!target->valid)
		return 0;

	/* Any of the id's widgets. */
	if (index == KEIUI_ANY) {
		if (target->id == id)
			return 1;
		return 0;
	}

	/* This widget. */
	same = ui_same(target, id, index);
	return same;
}

/* Tells whether a key names a widget. */
static int
ui_same(
	const struct ui_key *key,
	uint32_t id,
	uint32_t index)
{
	/* The id. */
	if (key->id != id)
		return 0;

	/* And the index. */
	if (key->index != index)
		return 0;
	return 1;
}

/* Clicks a widget (twice: the second of a double click or tap). */
static void
ui_click(
	struct kl_ui *ui,
	const struct ui_key *key,
	int twice,
	uint64_t now_us)
{
	int same;

	/* A second click on the same widget soon after is a double click. */
	same = 0;
	if (!twice && ui->last_click.valid)
		same = ui_same(&ui->last_click, key->id, key->index);
	if (same && now_us - ui->last_click_us <= UI_DOUBLE_US)
		twice = 1;

	/* The click, seen by the next frame's widget. */
	ui->clicked = *key;
	ui->clicked.valid = 1;
	ui->clicked_double = twice;
	ui->clicked_touch = 0;

	/* A double click is used up; a single one may start one. */
	ui->last_click = *key;
	ui->last_click.valid = !twice;
	ui->last_click_us = now_us;
}

/* Keeps an input no part took (the oldest goes when the ring is full). */
static struct kl_event *
ui_push(
	struct kl_ui *ui,
	unsigned kind,
	double x,
	double y)
{
	struct kl_event *event;
	unsigned slot;

	/* A full ring drops its oldest. */
	if (ui->event_count == UI_EVENTS) {
		ui->event_first = (ui->event_first + 1U) % UI_EVENTS;
		ui->event_count--;
	}

	/* The slot after the newest. */
	slot = (ui->event_first + ui->event_count) % UI_EVENTS;
	ui->event_count++;
	event = &ui->events[slot];
	memset(event, 0, sizeof(*event));
	event->kind = kind;
	event->x = x;
	event->y = y;
	event->fingers = ui->fingers;

	/* Reports the input for its details. */
	return event;
}

/*
 * Takes the gestures found by now.  The second tap of a double tap comes
 * as a tap followed by a double tap; the pair is carried out as the double
 * tap alone.
 */
static void
ui_gestures(
	struct kl_ui *ui,
	uint64_t now_us)
{
	struct kl_gesture_event gestures[UI_GESTURES];
	struct kl_scroll *scroll;
	int count;
	int index;
	int taken;

	/* The gestures found by now. */
	count = 0;
	while (count < UI_GESTURES) {
		taken = kl_gesture_next(ui->gesture, now_us, &gestures[count]);
		if (taken == 0)
			break;
		count++;
	}

	/* Each in turn; a tap the next double tap stands for is skipped. */
	for (index = 0; index < count; index++) {
		if (gestures[index].kind == KL_GESTURE_TAP && index + 1 < count && gestures[index + 1].kind == KL_GESTURE_DOUBLE_TAP)
			continue;
		ui_gesture(ui, &gestures[index], now_us);
	}

	/* A region pressed at the touch and not dragged (a tap, a long press) is let go when the last finger lifts. */
	if (ui->fingers != 0U || ui->drag != UI_DRAG_NONE || !ui->touch_has_region)
		return;
	scroll = ui->touch_region.scroll;
	if (scroll->touched && !scroll->released)
		kl_scroll_fling(scroll, 0.0, 0.0, now_us);
}

/* Carries out one gesture on the touch's targets. */
static void
ui_gesture(
	struct kl_ui *ui,
	const struct kl_gesture_event *gesture,
	uint64_t now_us)
{
	const struct ui_record *owner;
	struct kl_scroll *scroll;
	struct kl_event *event;
	double x;
	double y;
	int twice;
	int outside;

	/* The region's scroll, if any. */
	scroll = NULL;
	if (ui->touch_has_region)
		scroll = ui->touch_region.scroll;

	/* What the gesture means. */
	switch (gesture->kind) {
	case KL_GESTURE_TAP:
	case KL_GESTURE_DOUBLE_TAP:
		/* A tap that caught flying content only stops it. */
		twice = 0;
		if (gesture->kind == KL_GESTURE_DOUBLE_TAP)
			twice = 1;
		if (ui->caught)
			break;

		/* A tap on a handle of the fingers' selection keeps it as it is (ws190-p002). */
		if (ui->touch_has_region && ui->touch_region.kind == UI_KIND_HANDLE)
			break;

		/* A tap off the selection's field and its bar ends the fingers' selection (the field keeps it). */
		if (ui->select.active) {
			outside = 1;
			if (ui->touch_hit.valid && ui->touch_hit.id == ui->select.id && ui->touch_hit.index == ui->select.index)
				outside = 0;
			if (ui->touch_hit.valid && ui->touch_hit.id == KEIUI_TEXT_BAR_ID)
				outside = 0;
			if (outside)
				keiui_ui_select_end(ui);
		}

		/* A widget under it is clicked (at the tap, which takes the focus to it); else a text view puts its caret; else the application's. */
		if (ui->touch_hit.valid) {
			ui->pointer_x = gesture->x;
			ui->pointer_y = gesture->y;
			owner = ui_focus_owner(&ui->touch_hit, ui, gesture->x, gesture->y);
			if (owner != NULL) {
				ui->focus.valid = 1;
				ui->focus_ring = 0;
				ui->focus.id = owner->id;
				ui->focus.index = owner->index;
				ui->focus.flags = owner->flags;
			}

			/* And clicked. */
			ui_click(ui, &ui->touch_hit, twice, now_us);
			ui->clicked_touch = 1;
		} else if (ui->touch_has_region && ui->touch_region.kind == UI_KIND_TEXT) {
			ui_content(ui, gesture->x, gesture->y, &x, &y);
			kl_text_touch_tap(ui->touch_region.touch, x, y, twice);
		} else if (twice) {
			(void)ui_push(ui, KL_EVENT_DOUBLE_TAP, gesture->x, gesture->y);
		} else {
			(void)ui_push(ui, KL_EVENT_TAP, gesture->x, gesture->y);
		}

		/* The tap is carried out. */
		break;
	case KL_GESTURE_LONG_PRESS:
		/* A long press on a handle of the fingers' selection is nothing (ws190-p002). */
		if (ui->touch_has_region && ui->touch_region.kind == UI_KIND_HANDLE)
			break;

		/* A text view asks for its context menu; elsewhere the application hears it, with the region. */
		if (ui->touch_has_region && ui->touch_region.kind == UI_KIND_TEXT && !ui->touch_hit.valid) {
			kl_text_touch_long_press(ui->touch_region.touch, gesture->x, gesture->y);
			break;
		}

		/* The application's long press, naming the widget or else the region under it. */
		event = ui_push(ui, KL_EVENT_LONG_PRESS, gesture->x, gesture->y);
		if (event != NULL && ui->touch_has_region)
			event->region = ui->touch_region.id;
		if (event != NULL && ui->touch_hit.valid)
			event->region = ui->touch_hit.id;
		break;
	case KL_GESTURE_DRAG_BEGIN:
		/* One finger in a text view selects; otherwise a region scrolls; with no region the drag is the application's. */
		ui->finger_x = gesture->x;
		ui->finger_y = gesture->y;
		ui->edge_us = now_us;
		if (ui->touch_has_region && ui->touch_region.kind == UI_KIND_HANDLE && gesture->fingers == 1U) {
			/* One finger on a handle of the fingers' selection drags that end (ws190-p002). */
			ui->drag = UI_DRAG_SELECT;
			ui_content(ui, gesture->x, gesture->y, &x, &y);
			keiui_text_touch_hold(ui->touch_region.touch, (int)ui->touch_region.index, x, y);
		} else if (ui->touch_has_region && ui->touch_region.kind == UI_KIND_HANDLE) {
			/* More fingers there scroll the region under the handle, if any. */
			ui->touch_region = ui->touch_below;
			ui->touch_has_region = ui->touch_has_below;
			ui->drag = UI_DRAG_NONE;
			if (ui->touch_has_region) {
				(void)kl_scroll_press(ui->touch_region.scroll, now_us);
				ui->drag = UI_DRAG_SCROLL;
			}
		} else if (ui->touch_hit.valid && (ui->touch_hit.flags & KEIUI_NO_DRAG) != 0U) {
			/* A drag that starts on the bar's buttons does nothing (ws190-p002). */
			ui->drag = UI_DRAG_NONE;
		} else if (ui->touch_hit.valid && (ui->touch_hit.flags & KEIUI_DRAGGABLE) != 0U) {
			ui->drag = UI_DRAG_WIDGET;
			ui->active = ui->touch_hit;
			ui->pointer_x = gesture->x;
			ui->pointer_y = gesture->y;
		} else if (ui->touch_has_region && ui->touch_region.kind == UI_KIND_TEXT && gesture->fingers == 1U) {
			ui->drag = UI_DRAG_SELECT;
			ui_content(ui, gesture->x, gesture->y, &x, &y);
			kl_text_touch_drag_begin(ui->touch_region.touch, x, y);
		} else if (ui->touch_has_region) {
			ui->drag = UI_DRAG_SCROLL;
		} else {
			ui->drag = UI_DRAG_OUTSIDE;
			event = ui_push(ui, KL_EVENT_DRAG_BEGIN, gesture->x, gesture->y);
			if (event != NULL)
				event->fingers = gesture->fingers;
		}

		/* The drag has begun. */
		break;
	case KL_GESTURE_DRAG_END:
		/* The scroll flies on; a selection keeps its handles; the application hears the end of its drag. */
		if (ui->drag == UI_DRAG_SCROLL && scroll != NULL)
			kl_scroll_fling(scroll, gesture->vx, gesture->vy, now_us);
		if (ui->drag == UI_DRAG_SELECT) {
			/*
			 * The selection ends where the finger lifted.  A frame follows the
			 * finger's resampled place, which lags it, and a quick stroke lifts
			 * before any frame has followed it to its end (ws190-p003, T1-445).
			 */
			ui->finger_x = gesture->x;
			ui->finger_y = gesture->y;
			ui_content(ui, gesture->x, gesture->y, &x, &y);
			kl_text_touch_drag(ui->touch_region.touch, x, y);

			/* The finger lets the selection go: it keeps its handles and bar, and the view stops. */
			kl_text_touch_drag_end(ui->touch_region.touch);
			kl_scroll_fling(scroll, 0.0, 0.0, now_us);
		}

		/* The application's drag ends with the fingers' velocity. */
		if (ui->drag == UI_DRAG_OUTSIDE) {
			event = ui_push(ui, KL_EVENT_DRAG_END, gesture->x, gesture->y);
			if (event != NULL) {
				event->dx = gesture->vx;
				event->dy = gesture->vy;
			}
		}

		/* A widget's drag lets it go (seen held for one frame more). */
		if (ui->drag == UI_DRAG_WIDGET) {
			ui->released = ui->active;
			ui->active.valid = 0;
		}

		/* No drag any more. */
		ui->drag = UI_DRAG_NONE;
		break;
	case KL_GESTURE_CANCEL:
		/* Taken away: the scroll springs back, a selection ends where it is. */
		if (scroll != NULL)
			kl_scroll_cancel(scroll, now_us);
		if (ui->drag == UI_DRAG_SELECT)
			kl_text_touch_drag_end(ui->touch_region.touch);
		if (ui->drag == UI_DRAG_WIDGET)
			ui->active.valid = 0;
		ui->drag = UI_DRAG_NONE;
		break;
	default:
		break;
	}
}

/* Follows a drag at a frame's time: the scroll with the fingers, or the selection with its finger and the edges. */
static void
ui_drag_step(
	struct kl_ui *ui,
	uint64_t now_us)
{
	struct kl_scroll *scroll;
	double seconds;
	double dx;
	double dy;
	double vx;
	double vy;
	double x;
	double y;
	int edge;
	int error;

	/* Only a drag of a region or a widget. */
	if (ui->drag != UI_DRAG_SCROLL && ui->drag != UI_DRAG_SELECT && ui->drag != UI_DRAG_WIDGET)
		return;
	scroll = ui->touch_region.scroll;

	/* The fingers' movement for the frame. */
	error = kl_gesture_drag_offset(ui->gesture, now_us, &dx, &dy);
	if (error != 0)
		return;

	/* A widget's drag moves the point it follows (a slider's knob). */
	if (ui->drag == UI_DRAG_WIDGET) {
		ui->pointer_x = ui->down_x + dx;
		ui->pointer_y = ui->down_y + dy;
		return;
	}

	/* A scroll follows the fingers. */
	if (ui->drag == UI_DRAG_SCROLL) {
		kl_scroll_drag(scroll, dx, dy);
		return;
	}

	/* A selection follows its finger. */
	ui->finger_x = ui->down_x + dx;
	ui->finger_y = ui->down_y + dy;
	ui_content(ui, ui->finger_x, ui->finger_y, &x, &y);
	kl_text_touch_drag(ui->touch_region.touch, x, y);

	/* Near an edge the content scrolls by itself, at a speed for the time since the last frame. */
	edge = kl_text_touch_edge(ui->touch_region.touch, scroll, &vx, &vy);
	seconds = 0.0;
	if (now_us > ui->edge_us)
		seconds = (double)(now_us - ui->edge_us) / 1000000.0;
	ui->edge_us = now_us;
	if (!edge || seconds <= 0.0)
		return;

	/* The content moves, and the selection follows the finger over the content that moved under it. */
	kl_scroll_move_to(scroll, scroll->x + vx * seconds, scroll->y + vy * seconds, 0, now_us);
	(void)kl_scroll_press(scroll, now_us);
	ui_content(ui, ui->finger_x, ui->finger_y, &x, &y);
	kl_text_touch_drag(ui->touch_region.touch, x, y);
}

/* Turns a point of the window into the touch's text view's content coordinates. */
static void
ui_content(
	const struct kl_ui *ui,
	double x,
	double y,
	double *content_x,
	double *content_y)
{
	/* The viewport's corner and the scroll undone. */
	*content_x = x - (double)ui->touch_region.rect.x + ui->touch_region.scroll->x;
	*content_y = y - (double)ui->touch_region.rect.y + ui->touch_region.scroll->y;
}

/* Adds a record to the frame being drawn (none past the most a frame keeps). */
static void
ui_record(
	struct kl_ui *ui,
	enum ui_kind kind,
	uint32_t id,
	uint32_t index,
	unsigned flags,
	const struct kl_rect *rect,
	struct kl_scroll *scroll,
	struct kl_text_touch *touch)
{
	struct ui_record *record;

	/* A frame with too many parts records no more. */
	if (ui->drawing_count >= UI_RECORDS)
		return;

	/* The next record. */
	record = &ui->drawing[ui->drawing_count];
	ui->drawing_count++;
	record->kind = kind;
	record->id = id;
	record->index = index;
	record->flags = flags;
	record->rect = *rect;
	record->scroll = scroll;
	record->touch = touch;
}

/* A press lands on a record (or on nothing): the widget that takes the keyboard there takes the focus, anything else takes it away. */
static void
ui_focus_press(
	struct kl_ui *ui,
	const struct ui_record *record,
	double x,
	double y)
{
	const struct ui_record *owner;
	struct ui_key key;

	/* A widget that keeps the focus where it is (the bar's buttons, ws190-p002). */
	if (record != NULL && record->kind == UI_KIND_HIT && (record->flags & KEIUI_KEEP_FOCUS) != 0U)
		return;

	/* The widget pressed, or the one of the same id under it that takes the keyboard. */
	owner = NULL;
	if (record != NULL && record->kind == UI_KIND_HIT) {
		key.valid = 1;
		key.id = record->id;
		key.index = record->index;
		key.flags = record->flags;
		owner = ui_focus_owner(&key, ui, x, y);
	}

	/* It takes the focus. */
	if (owner != NULL) {
		ui->focus.valid = 1;
		ui->focus_ring = 0;
		ui->focus.id = owner->id;
		ui->focus.index = owner->index;
		ui->focus.flags = owner->flags;
		return;
	}

	/* Anything else: no widget keeps the keyboard. */
	ui->focus.valid = 0;
}

/* Finds the record that takes the keyboard for a widget pressed at a point: itself, or the one of its id under the point that does; NULL for none. */
static const struct ui_record *
ui_focus_owner(
	const struct ui_key *key,
	const struct kl_ui *ui,
	double x,
	double y)
{
	const struct ui_record *record;
	size_t index;
	int inside;

	/* From the last drawn (on top) down: the widget itself or one of its id under the point. */
	for (index = ui->shown_count; index > 0U; index--) {
		record = &ui->shown[index - 1U];
		if (record->kind != UI_KIND_HIT || record->id != key->id)
			continue;
		if ((record->flags & KEIUI_FOCUSABLE) == 0U)
			continue;
		if (record->index == key->index)
			return record;
		inside = ui_inside(&record->rect, x, y);
		if (inside && (key->flags & KEIUI_FOCUSABLE) == 0U)
			return record;
	}

	/* None. */
	return NULL;
}

/* Moves the focus to the next (or the previous) widget that takes the keyboard, in the order drawn from the last dialog on; 1 when it moved. */
static int
ui_focus_move(
	struct kl_ui *ui,
	int backward)
{
	const struct ui_record *record;
	size_t count;
	size_t index;
	size_t step;
	size_t start;
	size_t low;
	size_t place;
	int same;

	/* A dialog shuts out what was drawn before it. */
	low = 0;
	for (index = 0; index < ui->shown_count; index++) {
		record = &ui->shown[index];
		if (record->kind == UI_KIND_HIT && (record->flags & KEIUI_MODAL) != 0U)
			low = index;
	}

	/* Where the focus is among those records (none: before the first). */
	count = ui->shown_count - low;
	start = count;
	for (index = 0; index < count && ui->focus.valid; index++) {
		record = &ui->shown[low + index];
		same = ui_same(&ui->focus, record->id, record->index);
		if (same && record->kind == UI_KIND_HIT)
			start = index;
	}

	/* The next record that takes the keyboard, round the ends. */
	for (step = 1; step <= count; step++) {
		if (backward)
			place = (start + (count + 1U) * 2U - step) % (count + 1U);
		else
			place = (start + step) % (count + 1U);
		if (place == count)
			continue;
		record = &ui->shown[low + place];
		if (record->kind != UI_KIND_HIT || (record->flags & KEIUI_FOCUSABLE) == 0U)
			continue;
		ui->focus.valid = 1;
		ui->focus_ring = 1;
		ui->focus.id = record->id;
		ui->focus.index = record->index;
		ui->focus.flags = record->flags;
		return 1;
	}

	/* Nothing takes the keyboard. */
	return 0;
}

/*
 * Notes the on-screen keyboard's inset a window heard (window.c): each
 * window's input acts on it in its next kl_ui_end.
 */
void
keiui_ui_inset_note(
	uint32_t width,
	uint32_t height,
	int right,
	int bottom,
	unsigned reason,
	const int32_t *caret)
{
	/* A new serial, and what came with it. */
	ui_inset.serial++;
	ui_inset.width = width;
	ui_inset.height = height;
	ui_inset.right = right;
	ui_inset.bottom = bottom;
	ui_inset.reason = reason;
	memcpy(ui_inset.caret, caret, sizeof(ui_inset.caret));
}

/*
 * Moves the scroll of the frame's text view (the focused one, else the
 * last recorded) so that the caret's line is in the middle of the part of
 * the view the keyboard leaves, as far as the scroll goes.  Returns 1 when
 * the scroll moved.
 */
static int
ui_inset_center(
	struct kl_ui *ui,
	uint64_t now_us)
{
	const struct ui_record *record;
	struct kl_text_touch *touch;
	struct kl_scroll *scroll;
	struct kl_rect caret;
	size_t index;
	double largest;
	double middle;
	double target;
	int visible;
	int bottom;

	/* The text view: the one with the focus, else the last recorded. */
	record = NULL;
	for (index = 0; index < ui->shown_count; index++) {
		if (ui->shown[index].kind != UI_KIND_TEXT)
			continue;
		if (record != NULL && ui->focus.valid && record->id == ui->focus.id)
			continue;
		record = &ui->shown[index];
	}

	/* None, or one without a scroll or a view to ask for the caret. */
	if (record == NULL || record->scroll == NULL || record->touch == NULL)
		return 0;
	touch = record->touch;
	scroll = record->scroll;
	if (touch->view == NULL || touch->view->caret_rect == NULL)
		return 0;

	/* A view that does not scroll down stays. */
	largest = scroll->content_height - scroll->viewport_height;
	if ((scroll->axes & KL_SCROLL_Y) == 0U || largest <= 0.0)
		return 0;

	/* The part of the view the keyboard leaves: from its top down to the keyboard's top edge. */
	bottom = record->rect.y + record->rect.height;
	if (ui_inset.bottom > 0 && (int)ui_inset.height - ui_inset.bottom < bottom)
		bottom = (int)ui_inset.height - ui_inset.bottom;
	visible = bottom - record->rect.y;
	if (visible <= 0)
		return 0;

	/*
	 * The caret's line in the middle of that part, not past the text's
	 * ends: from the caret the application told in the window (drawn at the
	 * scroll now), else from the view at the fingers' caret (content
	 * coordinates).
	 */
	if (ui_inset.caret[3] > 0) {
		middle = (double)ui_inset.caret[1] + (double)ui_inset.caret[3] / 2.0 - (double)record->rect.y + scroll->y;
	} else {
		memset(&caret, 0, sizeof(caret));
		touch->view->caret_rect(touch->data, touch->caret, &caret);
		middle = (double)caret.y + (double)caret.height / 2.0;
	}
	target = middle - (double)visible / 2.0;
	if (target > largest)
		target = largest;
	if (target < 0.0)
		target = 0.0;

	/* Already there. */
	if (target == scroll->y)
		return 0;

	/* Succeeded: the scroll goes there at once. */
	kl_scroll_move_to(scroll, scroll->x, target, 0, now_us);
	return 1;
}

/* Tells whether a scroll is one of the frame on the screen's (an application's scroll may have gone since). */
static int
ui_scroll_shown(
	const struct kl_ui *ui,
	const struct kl_scroll *scroll)
{
	size_t index;

	/* Each record of the frame shown. */
	for (index = 0; index < ui->shown_count; index++) {
		if (ui->shown[index].scroll == scroll)
			return 1;
	}

	/* Not among them. */
	return 0;
}

/* Forgets a text widget's history: each change's bytes, and the widget. */
static void
ui_undo_forget(
	struct ui_undo *undo)
{
	size_t index;

	/* Each change kept. */
	for (index = 0; index < undo->count; index++)
		ui_undo_drop(&undo->steps[index]);

	/* No history. */
	undo->count = 0;
	undo->done = 0;
	undo->valid = 0;
	undo->hash = 0;
}

/* Frees a change's bytes. */
static void
ui_undo_drop(
	struct ui_undo_step *step)
{
	/* The bytes out and in. */
	free(step->removed);
	free(step->inserted);
	step->removed = NULL;
	step->inserted = NULL;
}

/* Gives the FNV-1a hash of a text, which tells the history's text from another. */
static uint64_t
ui_hash(
	const char *text,
	size_t length)
{
	uint64_t hash;
	size_t index;

	/* Each byte, then the length. */
	hash = UI_HASH_START;
	for (index = 0; index < length; index++) {
		hash ^= (unsigned char)text[index];
		hash *= UI_HASH_PRIME;
	}

	/* The length, so that texts of zeros differ. */
	hash ^= (uint64_t)length;
	hash *= UI_HASH_PRIME;

	/* The hash. */
	return hash;
}

/* Tells whether a byte is of a word: a letter, a digit, an underscore, or a byte of a character beyond ASCII. */
static int
ui_word_byte(
	char byte)
{
	unsigned char value;

	/* Beyond ASCII. */
	value = (unsigned char)byte;
	if (value >= 0x80U)
		return 1;

	/* A letter or a digit. */
	if (value >= 'a' && value <= 'z')
		return 1;
	if (value >= 'A' && value <= 'Z')
		return 1;
	if (value >= '0' && value <= '9')
		return 1;

	/* The underscore. */
	if (value == '_')
		return 1;

	/* Not a word's. */
	return 0;
}

/*
 * Finds the handle of the fingers' selection a finger at a point touches:
 * within the reach of its knob's centre, under no bar's button.  NULL for
 * none.
 */
static const struct ui_record *
ui_find_handle(
	const struct kl_ui *ui,
	double x,
	double y)
{
	const struct ui_record *record;
	size_t index;
	double dx;
	double dy;
	double reach;
	int inside;

	/* From the last drawn (on top) down: a widget over the point (the bar) hides the handles under it. */
	reach = (double)KL_TEXT_HANDLE_REACH / 2.0;
	for (index = ui->shown_count; index > 0U; index--) {
		record = &ui->shown[index - 1U];
		inside = ui_inside(&record->rect, x, y);
		if (!inside)
			continue;
		if (record->kind == UI_KIND_HIT)
			return NULL;
		if (record->kind != UI_KIND_HANDLE)
			continue;

		/* Within the knob's reach. */
		dx = x - record->centre_x;
		dy = y - record->centre_y;
		if (dx * dx + dy * dy <= reach * reach)
			return record;
	}

	/* No handle there. */
	return NULL;
}

/*
 * Ends a frame's fingers' selection (kl_ui_end, before the frame is
 * shown): the mode ends when its field was not drawn or lost the focus;
 * otherwise its handles and bar are recorded over everything and drawn,
 * unless something drawn after the field covers it.
 */
static void
ui_select_overlay(
	struct kl_ui *ui)
{
	struct keiui_select *select;
	int focused;
	int covered;

	/* No mode. */
	select = &ui->select;
	if (!select->active)
		return;

	/* A field not drawn in the frame (gone, another page) ends the mode. */
	if (!select->drawn) {
		keiui_ui_select_end(ui);
		return;
	}

	/* So does a field that lost the focus (a tap or Tab elsewhere, the program's). */
	focused = 0;
	if (ui->focus.valid)
		focused = ui_same(&ui->focus, select->id, select->index);
	if (!focused) {
		keiui_ui_select_end(ui);
		return;
	}

	/* A dialog or a widget drawn over the field: no handles and no bar this frame. */
	covered = ui_select_covered(ui);
	if (covered)
		return;

	/* The handles, then the bar over them. */
	ui_select_handles(ui);
	ui_select_bar(ui);
}

/* Tells whether a record after the selection's field covers it: a dialog, or a widget over the field. */
static int
ui_select_covered(
	const struct kl_ui *ui)
{
	const struct ui_record *record;
	size_t index;
	int meet;

	/* Each record after the field's. */
	for (index = ui->select.order + 1U; index < ui->drawing_count; index++) {
		record = &ui->drawing[index];
		if (record->kind != UI_KIND_HIT)
			continue;

		/* A dialog shuts the field out. */
		if ((record->flags & KEIUI_MODAL) != 0U)
			return 1;

		/* A widget over the field. */
		meet = ui_rects_meet(&record->rect, &ui->select.rect);
		if (meet)
			return 1;
	}

	/* Nothing covers it. */
	return 0;
}

/*
 * Records and draws the handles of the selection's ends: each knob's
 * reach, within the field's clip and its text box widened by a knob, for
 * a finger to drag that end.
 */
static void
ui_select_handles(
	struct kl_ui *ui)
{
	struct keiui_select *select;
	struct ui_record *record;
	struct kl_rect place;
	struct kl_rect grown;
	struct kl_rect visible;
	struct kl_rect reach;
	struct kl_rect kept;
	size_t ends[2];
	double origin_x;
	double origin_y;
	int sides[2];
	int index;
	int meet;

	/* Handles only for a selection, not a caret. */
	select = &ui->select;
	if (!select->touch.handles || select->touch.anchor == select->touch.caret)
		return;

	/* Where the handles may show: the field's clip and its box widened by a knob to the sides and below. */
	grown.x = select->box.x - KL_TEXT_HANDLE;
	grown.y = select->box.y;
	grown.width = select->box.width + 2 * KL_TEXT_HANDLE;
	grown.height = select->box.height + KL_TEXT_HANDLE;
	meet = keiui_rect_intersect(&grown, &select->clip, &visible);
	if (!meet)
		return;

	/* Each end's knob, recorded with its reach. */
	origin_x = (double)select->box.x - select->scroll.x;
	origin_y = (double)select->box.y - select->scroll.y;
	ends[0] = select->touch.anchor;
	ends[1] = select->touch.caret;
	sides[0] = KL_TEXT_HANDLE_ANCHOR;
	sides[1] = KL_TEXT_HANDLE_CARET;
	for (index = 0; index < 2; index++) {
		select->touch.view->caret_rect(select->touch.data, ends[index], &place);

		/* The reach around the knob's centre, within where the handles show. */
		reach.x = (int)(origin_x + (double)place.x) - KL_TEXT_HANDLE_REACH / 2;
		reach.y = (int)(origin_y + (double)(place.y + place.height)) + KL_TEXT_HANDLE / 2 - KL_TEXT_HANDLE_REACH / 2;
		reach.width = KL_TEXT_HANDLE_REACH;
		reach.height = KL_TEXT_HANDLE_REACH;
		meet = keiui_rect_intersect(&reach, &visible, &kept);
		if (!meet)
			continue;

		/* The record, with the text box and the knob's centre. */
		ui_record(ui, UI_KIND_HANDLE, KEIUI_TEXT_HANDLE_ID, (uint32_t)sides[index], 0U, &kept, &select->scroll, &select->touch);
		if (ui->drawing_count == 0U)
			continue;
		record = &ui->drawing[ui->drawing_count - 1U];
		if (record->kind != UI_KIND_HANDLE)
			continue;
		record->view = select->box;
		record->centre_x = origin_x + (double)place.x;
		record->centre_y = origin_y + (double)(place.y + place.height) + (double)KL_TEXT_HANDLE / 2.0;
	}

	/* The knobs, drawn where the handles may show. */
	kl_canvas_clip_push(select->style.canvas, &visible);
	kl_text_touch_draw_handles(&select->touch, select->style.canvas, origin_x, origin_y, select->style.theme);
	kl_canvas_clip_pop(select->style.canvas);
}

/*
 * Records and draws the bar of the selection, while its touch shows it
 * and no finger drags the selection or the page; the button pressed since
 * the last frame waits for kl_ui_end to send it to the field.
 */
static void
ui_select_bar(
	struct kl_ui *ui)
{
	struct keiui_select *select;
	struct kl_text_bar bar;
	struct kl_rect first;
	struct kl_rect second;
	struct kl_rect selection;
	struct kl_rect visible;
	struct kl_rect bounds;
	unsigned facts;
	unsigned buttons;
	unsigned pressed;
	unsigned held;
	size_t length;
	size_t start;
	size_t end;
	double origin_x;
	double origin_y;
	int bottom;
	int shown;
	int secret;
	int can_paste;

	/* Only while the touch shows the bar and nothing is dragged, with the bar's calls. */
	select = &ui->select;
	if (!select->touch.bar || select->touch.selecting || select->bar_calls == NULL)
		return;
	if (ui->drag == UI_DRAG_SCROLL || ui->drag == UI_DRAG_SELECT)
		return;

	/* The text's length and whether it is a secret field's. */
	length = select->area.length;
	secret = 0;
	if (select->kind == KEIUI_SELECT_FIELD) {
		length = select->field.length;
		secret = select->field.secret;
	}

	/* The selection's ends in order. */
	start = select->touch.anchor;
	end = select->touch.caret;
	if (start > end) {
		start = select->touch.caret;
		end = select->touch.anchor;
	}

	/* What the text is, for the buttons. */
	facts = 0U;
	if (start != end)
		facts |= KL_TEXT_BAR_SELECTED;
	if (start == 0U && end == length && length != 0U)
		facts |= KL_TEXT_BAR_WHOLE;
	if (length == 0U)
		facts |= KL_TEXT_BAR_EMPTY;
	if (secret)
		facts |= KL_TEXT_BAR_SECRET;
	if (ui->window != NULL && ui->copy != NULL && ui->paste != NULL)
		facts |= KL_TEXT_BAR_CLIPBOARD;
	can_paste = 0;
	if (ui->window != NULL && ui->can_paste != NULL)
		can_paste = ui->can_paste(ui->window);
	if (can_paste)
		facts |= KL_TEXT_BAR_CAN_PASTE;
	buttons = select->bar_calls->buttons(facts);

	/* The selection in the window: one line's ends, or the lines across the text box. */
	origin_x = (double)select->box.x - select->scroll.x;
	origin_y = (double)select->box.y - select->scroll.y;
	select->touch.view->caret_rect(select->touch.data, start, &first);
	select->touch.view->caret_rect(select->touch.data, end, &second);
	selection.x = (int)(origin_x + (double)first.x);
	selection.y = (int)(origin_y + (double)first.y);
	selection.width = second.x - first.x;
	selection.height = first.height;
	if (second.y != first.y) {
		bottom = second.y + second.height;
		selection.x = select->box.x;
		selection.width = select->box.width;
		selection.height = bottom - first.y;
	}

	/* The part of the field that shows its text: its box within the clip it was drawn in. */
	shown = keiui_rect_intersect(&select->clip, &select->box, &visible);
	if (!shown)
		return;

	/* Laid out within the bounds, by the part of the selection the field shows. */
	ui_select_bounds(ui, &bounds);
	shown = select->bar_calls->layout(&bar, select->style.text, buttons, &selection, &visible, &bounds);
	if (!shown)
		return;

	/* Recorded over everything, drawn, and the button pressed kept for the field. */
	pressed = select->bar_calls->hit(ui, KEIUI_TEXT_BAR_ID, &bar, &held);
	select->bar_calls->draw(&bar, &select->style, held);
	if (pressed != 0U)
		ui->bar_command = pressed;
}

/*
 * Gives where the bar may stand: the canvas the field is drawn on, or the
 * program's bounds within it, less what the on-screen keyboard covers at
 * the right and the bottom.
 */
static void
ui_select_bounds(
	const struct kl_ui *ui,
	struct kl_rect *bounds)
{
	const struct kl_canvas *canvas;
	struct kl_rect whole;
	int right;
	int bottom;
	int meet;

	/* The canvas, or the program's bounds within it. */
	canvas = ui->select.style.canvas;
	whole.x = 0;
	whole.y = 0;
	whole.width = canvas->width;
	whole.height = canvas->height;
	*bounds = whole;
	if (ui->select_bounded) {
		meet = keiui_rect_intersect(&whole, &ui->select_bounds, bounds);
		if (!meet)
			memset(bounds, 0, sizeof(*bounds));
	}

	/* The keyboard's inset: the window's own, else the last one heard for a window of the canvas's size. */
	right = 0;
	bottom = 0;
	if (ui->window != NULL && ui->keyboard_inset != NULL) {
		ui->keyboard_inset(ui->window, &right, &bottom);
	} else if (ui_inset.reason != KL_KEYBOARD_INSET_NONE && (int)ui_inset.width == canvas->width && (int)ui_inset.height == canvas->height) {
		right = ui_inset.right;
		bottom = ui_inset.bottom;
	}

	/* The covered widths taken off the canvas's right and bottom. */
	if (right > 0 && bounds->x + bounds->width > canvas->width - right)
		bounds->width = canvas->width - right - bounds->x;
	if (bottom > 0 && bounds->y + bounds->height > canvas->height - bottom)
		bounds->height = canvas->height - bottom - bounds->y;
	if (bounds->width < 0)
		bounds->width = 0;
	if (bounds->height < 0)
		bounds->height = 0;
}

/*
 * Sends the bar's button pressed to the selection's field as the key it
 * stands for (Ctrl+X, Ctrl+C, Ctrl+V, Ctrl+A), after the keys waiting, so
 * that the field's own editing carries it out in the next frame; a full
 * queue drops it.
 */
static void
ui_select_command(
	struct kl_ui *ui)
{
	struct ui_press *press;
	uint32_t code;

	/* No command. */
	if (ui->bar_command == 0U)
		return;

	/* The key of the command. */
	code = UI_KEY_A;
	if (ui->bar_command == KL_TEXT_BAR_CUT)
		code = UI_KEY_X;
	else if (ui->bar_command == KL_TEXT_BAR_COPY)
		code = UI_KEY_C;
	else if (ui->bar_command == KL_TEXT_BAR_PASTE)
		code = UI_KEY_V;
	ui->bar_command = 0;

	/* No room, or no field to take it. */
	if (ui->key_count == UI_KEYS || !ui->select.active)
		return;

	/* The key with Control, for the field, marked as the bar's. */
	press = &ui->keys[ui->key_count];
	memset(press, 0, sizeof(*press));
	press->kind = KEIUI_INPUT_KEY;
	press->code = code;
	press->modifiers = KL_MOD_CTRL;
	press->target.valid = 1;
	press->target.id = ui->select.id;
	press->target.index = ui->select.index;
	press->target.flags = KEIUI_FOCUSABLE;
	press->from_bar = 1;
	ui->key_count++;
}

/* Tells whether two rectangles share any part. */
static int
ui_rects_meet(
	const struct kl_rect *first,
	const struct kl_rect *second)
{
	struct kl_rect shared;
	int meet;

	/* Their shared part. */
	meet = keiui_rect_intersect(first, second, &shared);

	/* Reports whether there is one. */
	return meet;
}
