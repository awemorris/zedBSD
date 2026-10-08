/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The on-screen keyboard (ws102-p002, plan/ws102/design.md): drawn by the
 * compositor itself, opened by a swipe from a bottom corner.
 *
 * A contact that starts in the bottom-right corner and moves towards the
 * top left opens the flick panel at the right side of the screen; one that
 * starts in the bottom-left corner and moves towards the top right opens
 * the QWERTY panel along the bottom.  The recogniser has corner.c's numbers
 * (the top-right corner's swipe to Notes): a contact that begins in a
 * corner belongs to the keyboard until it ends; it arms once it has moved
 * KEYBOARD_ARM each way within KEYBOARD_ARM_MS; it commits when it ends
 * near the diagonal, far enough along it (KEYBOARD_COMMIT) or quickly
 * enough (KEYBOARD_FLICK_DISTANCE at KEYBOARD_FLICK_SPEED).  The corners are the
 * keyboard's before Wiseview's bottom edge and the desktops' side edges
 * (shell.c asks the keyboard first), so those gestures start outside them.
 *
 * The panels are built into the screen's edges (ws102-p021, design §2.3):
 * the flick panel is the whole right column under the system bar, the
 * QWERTY and handwriting panel the whole bottom row, without a margin, an
 * outer corner radius or a shadow, a 1-pixel line on the side facing the
 * windows (as the system bar is the top edge's band).  A panel grows out
 * of its edge when it opens and goes back into it when it closes, in
 * KEYBOARD_SLIDE_MS, eased out.
 *
 * While a panel is out, the work area is the screen less the panel
 * (ws102-p007, design §2.8): kwl_keyboard_reserved gives the column or row
 * a panel takes, which the glass look's space and the desktop take away,
 * and kwl_keyboard_reserved_now the part of it out at this moment of the
 * slide, which the docked windows' place takes away (so that a docked
 * window narrows or shortens with the panel).  When the panel comes or goes,
 * every docked window is told its new size once; a floating window that
 * would reach under the panel is moved, at its size, into the work area
 * during the slide (its title bar kept at the area's top and left when it
 * is too large), and moved back when the panel goes, unless it was moved
 * or resized in the meantime.  Fullscreen windows keep their size.
 *
 * Above the flick panel's keys are its tools (ws102-p016, design §2.10):
 * a row that is always there (the application used before, delete), the
 * tools' tabs (edit now; candidates, history and emoji to come), and the
 * edit tools -- the arrows, line start and end, page up and down (keys,
 * with Shift while selecting), selecting (a toggle), select all, undo,
 * redo, copy, cut and paste (kwl_edit_action, edit.c: the window's own edit
 * operations, or their keys).  A tool the focused window cannot do now is
 * drawn faint (kwl_edit_state).  The history tab (ws102-p024) lists the
 * clipboard's history (clipboard.c, the newest first, each on one line); a
 * tap pastes an item into the focused window.
 *
 * The candidates' tab (ws166-p003) predicts words: the hiragana the flick
 * panel commits one after another are a reading (keyboard.reading); each
 * time it changes the input method is asked for the words it starts
 * (kwl_ime_predict, kl_ime_status_v1 version 2: the reading's own
 * words first, then longer ones, the user's choices before the
 * dictionary's), and the answer fills the tab, which comes up by itself
 * when a reading begins.  A word tapped replaces the reading before the
 * cursor (the text input deletes the reading's bytes and commits the
 * word, as the voice key does) and is learned (kwl_ime_learn).  When the
 * field tells its text, the reading must be what is before the cursor, or
 * nothing is replaced.  Any other key or tool, another field, a secret
 * field (a password, a PIN, hidden or sensitive text) or the panel's
 * closing ends the reading.
 *
 * The same swipe again closes the panel it opened; the other corner's
 * swipe changes panels; the panel's close key closes it, and so does a
 * drag of its title band KEYBOARD_SWIPE_CLOSE pixels towards its edge (the
 * flick panel's right, the QWERTY panel's bottom).  The login and lock
 * screens, App Home and Wiseview close it (ws102-p005).  A contact is the
 * pointer's left button or a finger, which touch.c passes through the shell
 * as the pointer (server->shell_source says which).
 *
 * The flick panel has the faces of keyboard-layout.c (ws102-p003): a press
 * on a key shows its characters around it as petals, the one the movement
 * points at lit, and the release types that character (the face key goes
 * to the next face).
 *
 * The QWERTY panel (ws102-p006) has the letters face (the digits' row,
 * three rows of letters, the space row with the arrows) and the symbols
 * face; its keys are as wide as keyboard-layout.c says, in quarter keys.
 * Shift once makes the next letter a capital (and the digits' row its
 * symbols); twice within KEYBOARD_SHIFT_LOCK_MS locks it until it is
 * pressed again.  A pressed key that types shows its character in a
 * bubble above it.  Above the digits is a thinner row of extra keys
 * (ws102-p020): Esc, Tab, Home, End, PgUp and PgDn sent by their codes, a
 * few symbols, and Ctrl and Alt, which hold for the next key only (a second
 * press lets go of them).  The work area comes in a later phase.
 *
 * The QWERTY panel's band has a button to the handwriting face (ws102-p008)
 * and back: a writing area at the left, where the pen or finger's strokes
 * are drawn as they are written, and at the right the recognizer's
 * candidates (keyboard-hand.c, a stub for now) over clear, delete, space
 * and enter.  The ink is recognized KEYBOARD_HAND_WAIT_MS after the last
 * stroke ends; a candidate tapped is sent as a character and the ink
 * cleared.  The time from a point's input to the frame that draws it is
 * logged (the lag of the line behind the finger).
 *
 * What a key types goes to the focused application (ws102-p004, design
 * §2.5) by one of two ways, without the input method's own files changing:
 * a character of the US layout (letters, digits, ASCII symbols, space,
 * enter, delete) as the key's press and release (kwl_seat_key_deliver,
 * Shift held around it when needed), which every application hears; any
 * other character (kana, full-width signs) as the commit of the focused
 * field's text input (text-input-v3, kwl_text_input_current and
 * kwl_text_input_deliver of ime.h).  A field without a text input cannot
 * take such a character, nor can one the input method is composing in;
 * the keyboard then says so in the log and sends nothing.  The voice key
 * replaces the kana it sent last by its next form (the text input deletes
 * the one before and commits the other), the case key the letter it sent
 * last (delete, then the other case).
 */

#include "desktop.h"
#include "extras.h"
#include "glass.h"
#include "ime.h"
#include "keyboard.h"
#include "menu.h"

#include <keiland/keiland.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* The corners a contact starts in: this many pixels from the bottom and from the side (kwl.h). */
#define KEYBOARD_ZONE		KWL_KEYBOARD_ZONE

/* How far a contact moves each way (inwards and up) before it is the gesture, and how soon. */
#define KEYBOARD_ARM		14
#define KEYBOARD_ARM_MS		1500U

/*
 * The diagonal: the shorter of the two movements must be at least
 * KEYBOARD_CONE_NUMERATOR / KEYBOARD_CONE_DENOMINATOR of the longer, which
 * keeps the contact within 25 degrees of the 45-degree line.
 */
#define KEYBOARD_CONE_NUMERATOR		9
#define KEYBOARD_CONE_DENOMINATOR	25

/* How far along the diagonal a letting go commits, and the distance the hint grows over. */
#define KEYBOARD_COMMIT		108.0f
#define KEYBOARD_DISTANCE	216.0f

/*
 * The hint's quarter disc (BUG-230, Notes' corner's numbers): its radius at
 * the start and its growth for each pixel along the diagonal, and the
 * radius from which the keyboard's name shows in it.
 */
#define KEYBOARD_HINT_RADIUS_MIN	24.0f
#define KEYBOARD_HINT_GROWTH		1.3f
#define KEYBOARD_HINT_LABEL_RADIUS	96.0f

/* A flick: at least this far along the diagonal, at this speed (pixels a millisecond) over the last KEYBOARD_FLICK_MS. */
#define KEYBOARD_FLICK_DISTANCE	40.0f
#define KEYBOARD_FLICK_SPEED	0.8f
#define KEYBOARD_FLICK_MS	100U

/* How many recent points of the contact are kept for its speed. */
#define KEYBOARD_SAMPLES	16U

/* The margin inside a panel's title band (around its title and buttons), and the band's height. */
#define KEYBOARD_MARGIN		12
#define KEYBOARD_BAND		36

/* The flick panel's keys: a key's side (a share of the screen's height, within limits) and the gap. */
#define KEYBOARD_KEY_DIVISOR	11
#define KEYBOARD_KEY_MIN	64
#define KEYBOARD_KEY_MAX	96
#define KEYBOARD_KEY_GAP	6
#define KEYBOARD_FLICK_COLUMNS	4
#define KEYBOARD_FLICK_ROWS	4

/* The QWERTY panel's height: a share of the screen's height (in hundredths), within limits. */
#define KEYBOARD_QWERTY_SHARE	42
#define KEYBOARD_QWERTY_MIN	260
#define KEYBOARD_QWERTY_MAX	460

/* The longest text the keyboard remembers as the last it sent (one character), and the ways it was sent. */
#define KEYBOARD_LAST		16
#define KEYBOARD_SENT_NONE	0U
#define KEYBOARD_SENT_KEY	1U
#define KEYBOARD_SENT_COMMIT	2U

/* How soon a second press of Shift locks it, in milliseconds. */
#define KEYBOARD_SHIFT_LOCK_MS	400U

/* Shift: off, for the next character only, or locked. */
#define KEYBOARD_SHIFT_OFF	0U
#define KEYBOARD_SHIFT_ONCE	1U
#define KEYBOARD_SHIFT_LOCKED	2U

/* The extra keys' row's height as a share of another row's (p020). */
#define KEYBOARD_EXTRA_SHARE	0.7f

/* Ctrl and Alt for the next key only (p020): their bits of the depressed modifiers. */
#define KEYBOARD_CTRL		4U
#define KEYBOARD_ALT		8U

/* The handwriting face: the wait after the last stroke before recognizing, the side column's width and a candidate row's height. */
#define KEYBOARD_HAND_WAIT_MS	600U
#define KEYBOARD_HAND_SIDE	300
#define KEYBOARD_HAND_NOTE	24

/* The band's button to the handwriting face and back: its width. */
#define KEYBOARD_BAND_BUTTON	84

/* The handwriting face's keys at the right: three candidates, clear, delete, space, enter. */
#define KEYBOARD_HAND_KEYS	7U
#define KEYBOARD_HAND_CLEAR	3U
#define KEYBOARD_HAND_DELETE	4U
#define KEYBOARD_HAND_SPACE	5U
#define KEYBOARD_HAND_ENTER	6U

/* A stroke's line: the dots' side and the step between them, in pixels. */
#define KEYBOARD_INK_DOT	4.0f
#define KEYBOARD_INK_STEP	2.0f

/* The opacity of the line on the side of a panel facing the windows (the system bar's line's). */
#define KEYBOARD_EDGE_ALPHA	0.18f

/* The flick panel's tools (p016): what each does. */
enum keyboard_tool_kind {
	TOOL_PREVIOUS,
	TOOL_DELETE,
	TOOL_TAB_EDIT,
	TOOL_TAB_CANDIDATES,
	TOOL_TAB_HISTORY,
	TOOL_TAB_EMOJI,
	TOOL_LEFT,
	TOOL_UP,
	TOOL_DOWN,
	TOOL_RIGHT,
	TOOL_LINE_START,
	TOOL_LINE_END,
	TOOL_PAGE_UP,
	TOOL_PAGE_DOWN,
	TOOL_SELECT,
	TOOL_SELECT_ALL,
	TOOL_UNDO,
	TOOL_REDO,
	TOOL_COPY,
	TOOL_CUT,
	TOOL_PASTE
};

/* The tools' faces under the tabs: the edit tools, the clipboard's history, the emoji (ws102-p022). */
#define KEYBOARD_FACE_EDIT	0U
#define KEYBOARD_FACE_HISTORY	1U
#define KEYBOARD_FACE_EMOJI	2U
#define KEYBOARD_FACE_CANDIDATES	3U

/*
 * The candidates' tab (ws166-p003): the longest reading kept (bytes, with
 * its NUL), the most words shown and the longest word and reading of one,
 * and the grid the words are laid out in.
 */
#define KEYBOARD_READING		96U
#define KEYBOARD_PREDICTIONS		12U
#define KEYBOARD_PREDICTION_TEXT	161U
#define KEYBOARD_CANDIDATE_COLUMNS	3U
#define KEYBOARD_CANDIDATE_ROWS		4U

/* The text-input-v3 purposes and hints of a secret field, whose readings are not kept. */
#define KEYBOARD_PURPOSE_PASSWORD	8U
#define KEYBOARD_PURPOSE_PIN		9U
#define KEYBOARD_HINT_SECRET		0xc0U

/*
 * The emoji face's grid: its columns and rows (a category's emoji fill
 * it), and the smallest cell that takes the large emoji size.
 */
#define KEYBOARD_EMOJI_COLUMNS	5U
#define KEYBOARD_EMOJI_ROWS	4U
#define KEYBOARD_EMOJI_LARGE	52

/* The history's rows: how many, and the longest text shown of an item (bytes). */
#define KEYBOARD_HISTORY_ROWS	10U
#define KEYBOARD_HISTORY_TEXT	160U

/* The tools' rows: the row always there, the tabs' row (lower), and the edit tools' rows. */
#define KEYBOARD_TOOL_ROW	44
#define KEYBOARD_TOOL_TABS	30

/* The most floating windows the work area moves at once. */
#define KEYBOARD_MOVES		32U

/* The Shift bit of the depressed modifiers (server->modifiers). */
#define KEYBOARD_SHIFT		1U

/* The close key's side, at the right of the title band. */
#define KEYBOARD_CLOSE		28

/* How long a panel takes to grow out of its edge or to go back into it (design §2.8's time). */
#define KEYBOARD_SLIDE_MS	200U

/* How far the title band is dragged towards the panel's edge to close it. */
#define KEYBOARD_SWIPE_CLOSE	80

/* A key's corner radius, and a petal's side as a share of a key's (in tenths). */
#define KEYBOARD_KEY_RADIUS	10.0f
#define KEYBOARD_PETAL_TENTHS	8

/*
 * Which panel: none, the flick panel at the right side (the bottom-right
 * corner's), or the QWERTY panel along the bottom (the bottom-left
 * corner's).
 */
enum keyboard_kind {
	PANEL_NONE,
	PANEL_FLICK,
	PANEL_QWERTY
};

/*
 * One point a contact passed through, and when (the input event's time in
 * wrapping milliseconds); the recent points give its speed at the end.
 */
struct keyboard_sample {
	int32_t x;
	int32_t y;
	uint32_t time;
};

/*
 * The contact the gesture follows, from its start in a corner to its end:
 * the corner (the panel it would open), its source, where and when it
 * started (the event's time and the compositor's clock), where it is, and
 * whether it armed or ran out of time.  Only one contact is followed.
 */
struct keyboard_contact {
	unsigned active;
	enum keyboard_kind corner;
	enum kwl_contact_source source;
	int32_t start_x;
	int32_t start_y;
	uint32_t start_time;
	uint64_t start_clock_ms;
	int32_t x;
	int32_t y;
	unsigned armed;
	unsigned expired;
	struct keyboard_sample samples[KEYBOARD_SAMPLES];
	unsigned sample_next;
	unsigned sample_count;
};

/*
 * One tool of the flick panel: its label, what it does, its row (0 the
 * row always there, 1 the tabs, 2 and on the edit tools), the first of the
 * row's columns it takes, how many, and how many columns its row has.
 */
struct keyboard_tool {
	const char *label;
	enum keyboard_tool_kind kind;
	unsigned row;
	unsigned column;
	unsigned span;
	unsigned columns;
};

/*
 * A floating window the work area moved: the window (only compared with
 * the live windows, never followed unless found among them), where it was
 * before (to go back to), where its move started and where it ends, and
 * whether it is going back.
 */
struct keyboard_move {
	struct kwl_object *surface;
	int32_t home_x;
	int32_t home_y;
	int32_t from_x;
	int32_t from_y;
	int32_t to_x;
	int32_t to_y;
	unsigned back;
};

/*
 * The keyboard's whole state: the contact of the corner's gesture, the
 * panel open (and its rectangle on the output), a press that began on the
 * panel (it is the panel's until its release), whether the corners'
 * places have been logged (once, for the tests), the flick panel's face
 * (KWL_FLICK_*), the key held (key_active: its row and column, where
 * the press began and where the pointer is now), and the last character
 * sent and how (KEYBOARD_SENT_*), which the voice and case keys change,
 * a press held on the title band (where it began), which closes the
 * panel when dragged far enough towards its edge, and the QWERTY panel's
 * face (KWL_QWERTY_*) and Shift (KEYBOARD_SHIFT_*, and when it was last
 * pressed), and Ctrl and Alt held for the next key (held, their modifier
 * bits).  On the QWERTY panel the key held is key_row and key_column
 * (the key's place in its row).  The handwriting face (hand): a stroke
 * being written, when the last stroke ended, whether the ink was
 * recognized and the answer, the key held at the right (hand_key), and a
 * point waiting to be drawn (its input time, for the lag) with the time of
 * the last frame that drew one.  slide_ms is when the panel began growing
 * out of its edge, or going back into it; leaving is the panel going back
 * (already closed for everything but its drawing, PANEL_NONE when none).
 * reserved_right and reserved_bottom are what the open panel takes from
 * the work area; moves are the floating windows the work area moved, and
 * moving says they are on their way (during the slide).
 * touch_owner is the finger (its id + 1, 0 for none) whose press the panel
 * holds when fingers come by touch.c's ROUTE_OSK (ws102-p009), and
 * touch_x, touch_y where it was last.  tool_active and tool are the flick
 * panel's tool held (its index in keyboard_tools), selecting the edit
 * tools' selection toggle (the movements go with Shift).  tools_face is
 * the face under the tabs (KEYBOARD_FACE_*), history_active and
 * history_row the history's row held.  emoji_category is the emoji face's
 * category shown, emoji_active and emoji_slot its tab or cell held (the
 * tabs first, then the cells, keyboard_emoji_rect).  The candidates' tab
 * (ws166-p003): reading is the hiragana committed since the reading began
 * (empty for none) into reading_input's field, reading_commit that
 * field's count of commits when the reading last changed (its surrounding
 * text tells the reading only when set by a later commit), predict_serial the latest
 * request to the input method, predictions and prediction_readings its
 * answer's words and their readings (prediction_count of them), and
 * candidate_active and candidate_slot the word held.
 */

/*
 * The ink written on the handwriting face, for the one compositor in this
 * process.  It is empty at start-up, filled by the pointer's or finger's
 * movement on the writing area, and cleared by the clear key, a candidate,
 * or the panel closing; only the compositor's thread touches it.
 */
static struct kwl_hand_ink keyboard_ink;

/*
 * The flick panel's tools, row by row (fixed; they live as long as the
 * program).
 */
static const struct keyboard_tool keyboard_tools[] = {
	{ "前の app", TOOL_PREVIOUS, 0U, 0U, 2U, 4U },
	{ "Del", TOOL_DELETE, 0U, 2U, 2U, 4U },
	{ "編集", TOOL_TAB_EDIT, 1U, 0U, 1U, 4U },
	{ "候補", TOOL_TAB_CANDIDATES, 1U, 1U, 1U, 4U },
	{ "履歴", TOOL_TAB_HISTORY, 1U, 2U, 1U, 4U },
	{ "絵文字", TOOL_TAB_EMOJI, 1U, 3U, 1U, 4U },
	{ "←", TOOL_LEFT, 2U, 0U, 1U, 4U },
	{ "↑", TOOL_UP, 2U, 1U, 1U, 4U },
	{ "↓", TOOL_DOWN, 2U, 2U, 1U, 4U },
	{ "→", TOOL_RIGHT, 2U, 3U, 1U, 4U },
	{ "行頭", TOOL_LINE_START, 3U, 0U, 1U, 4U },
	{ "行末", TOOL_LINE_END, 3U, 1U, 1U, 4U },
	{ "PgUp", TOOL_PAGE_UP, 3U, 2U, 1U, 4U },
	{ "PgDn", TOOL_PAGE_DOWN, 3U, 3U, 1U, 4U },
	{ "選択", TOOL_SELECT, 4U, 0U, 1U, 4U },
	{ "全選択", TOOL_SELECT_ALL, 4U, 1U, 1U, 4U },
	{ "取消", TOOL_UNDO, 4U, 2U, 1U, 4U },
	{ "やり直し", TOOL_REDO, 4U, 3U, 1U, 4U },
	{ "コピー", TOOL_COPY, 5U, 0U, 1U, 3U },
	{ "切り取り", TOOL_CUT, 5U, 1U, 1U, 3U },
	{ "貼り付け", TOOL_PASTE, 5U, 2U, 1U, 3U }
};

struct keyboard_state {
	struct keyboard_contact contact;
	enum keyboard_kind open;
	int32_t panel[4];
	unsigned pressing;
	unsigned zones_logged;
	unsigned face;
	unsigned key_active;
	unsigned key_row;
	unsigned key_column;
	int32_t key_start_x;
	int32_t key_start_y;
	int32_t key_x;
	int32_t key_y;
	char last[KEYBOARD_LAST];
	unsigned last_sent;
	unsigned band_active;
	int32_t band_x;
	int32_t band_y;
	unsigned qface;
	unsigned shift;
	uint64_t shift_ms;
	uint32_t held;
	unsigned hand;
	unsigned writing;
	uint64_t stroke_end_ms;
	unsigned recognized;
	struct kwl_hand_result result;
	unsigned hand_key_active;
	unsigned hand_key;
	unsigned lag_pending;
	uint64_t lag_input_ms;
	uint64_t lag_frame_ms;
	uint64_t slide_ms;
	enum keyboard_kind leaving;
	int32_t reserved_right;
	int32_t reserved_bottom;
	struct keyboard_move moves[KEYBOARD_MOVES];
	unsigned move_count;
	unsigned moving;
	uint32_t touch_owner;
	int32_t touch_x;
	int32_t touch_y;
	unsigned tool_active;
	unsigned tool;
	unsigned selecting;
	unsigned tools_face;
	unsigned history_active;
	unsigned history_row;
	unsigned emoji_category;
	unsigned emoji_active;
	unsigned emoji_slot;
	char reading[KEYBOARD_READING];
	struct kwl_text_input *reading_input;
	uint32_t reading_commit;
	uint32_t predict_serial;
	char predictions[KEYBOARD_PREDICTIONS][KEYBOARD_PREDICTION_TEXT];
	char prediction_readings[KEYBOARD_PREDICTIONS][KEYBOARD_PREDICTION_TEXT];
	unsigned prediction_count;
	unsigned candidate_active;
	unsigned candidate_slot;

	/*
	 * The panel App Home or Wiseview put away, and the window that had
	 * the focus then (BUG-229): when they are gone and that window has the
	 * focus again, the panel comes back.  PANEL_NONE when nothing waits.
	 */
	enum keyboard_kind restore;
	struct kwl_object *restore_focus;

	/*
	 * The field the faces follow (q893): the purpose of the text input
	 * served when the faces last followed it (0, a text field's, when none
	 * is served), its kind (KWL_FIELD_*), the QWERTY face that kind opens
	 * on (the symbols' key goes back to it), and the flick face the user
	 * last had in a text field, which the next text field gets back.
	 */
	uint32_t field_purpose;
	unsigned field_kind;
	unsigned field_qface;
	unsigned flick_chosen;
};

/*
 * The keyboard of the one compositor in this process.
 *
 * It is zero (no contact, no panel) at start-up.  The compositor's single
 * thread is the only one that reads or changes it: the input handlers, the
 * clock and the drawing.
 */
static struct keyboard_state keyboard;

/*
 * The accent the user chose (ws179-p001) for the held key, and the ink on
 * it, as 0 to 1 RGBA for the panel's ground: made again at the start of
 * each drawing of the keyboard (kwl_keyboard_draw); the event loop's thread
 * alone uses them.
 */
static float keyboard_blue[4];
static float keyboard_on_blue[4];

static int keyboard_contact_begin(struct kwl_server *server, int32_t x, int32_t y, uint32_t time);
static int keyboard_contact_move(struct kwl_server *server, int32_t x, int32_t y, uint32_t time);
static int keyboard_contact_end(struct kwl_server *server, int32_t x, int32_t y, uint32_t time);
static enum keyboard_kind keyboard_corner_at(struct kwl_server *server, int32_t x, int32_t y);
static void keyboard_travel(int32_t *inwards, int32_t *upwards);
static void keyboard_sample(int32_t x, int32_t y, uint32_t time);
static float keyboard_speed(void);
static int keyboard_on_diagonal(int32_t inwards, int32_t upwards);
static void keyboard_commit(struct kwl_server *server, const char *via, float progress);
static void keyboard_open(struct kwl_server *server, enum keyboard_kind kind);
static void keyboard_put_away(struct kwl_server *server, const char *reason);
static void keyboard_restore(struct kwl_server *server, float home);
static void keyboard_place(struct kwl_server *server, enum keyboard_kind kind, int32_t *rect);
static int keyboard_panel_button(struct kwl_server *server, uint32_t button, uint32_t state);
static int keyboard_contains(const int32_t *rect, int32_t x, int32_t y);
static int keyboard_key_size(struct kwl_server *server);
static void keyboard_key_rect(struct kwl_server *server, unsigned row, unsigned column, int32_t *rect);
static int keyboard_key_at(struct kwl_server *server, int32_t x, int32_t y, unsigned *row, unsigned *column);
static unsigned keyboard_key_direction(struct kwl_server *server);
static void keyboard_key_release(struct kwl_server *server);
static void keyboard_draw_keys(struct kwl_server *server, VkCommandBuffer command);
static void keyboard_draw_key(struct kwl_server *server, VkCommandBuffer command, const int32_t *rect, const char *label, const float *ground, const float *ink, enum glass_size size);
static void keyboard_draw_petals(struct kwl_server *server, VkCommandBuffer command);
static unsigned keyboard_characters(const char *text);
static int keyboard_band_swiped(int32_t x, int32_t y);
static void keyboard_qwerty_rect(struct kwl_server *server, unsigned row, unsigned index, int32_t *rect);
static int keyboard_qwerty_at(struct kwl_server *server, int32_t x, int32_t y, unsigned *row, unsigned *index);
static const struct kwl_qwerty_key *keyboard_qwerty_key(unsigned row, unsigned index);
static void keyboard_qwerty_release(struct kwl_server *server);
static void keyboard_qwerty_shift(void);
static void keyboard_qwerty_log(struct kwl_server *server);
static void keyboard_follow_field(struct kwl_server *server);
static void keyboard_draw_qwerty(struct kwl_server *server, VkCommandBuffer command);
static void keyboard_draw_bubble(struct kwl_server *server, VkCommandBuffer command);
static void keyboard_band_button_rect(int32_t *rect);
static void keyboard_hand_area(int32_t *rect);
static void keyboard_hand_key_rect(unsigned key, int32_t *rect);
static int keyboard_hand_key_at(int32_t x, int32_t y, unsigned *key);
static void keyboard_hand_toggle(struct kwl_server *server);
static void keyboard_hand_point(struct kwl_server *server, int32_t x, int32_t y, int begin);
static void keyboard_hand_release(struct kwl_server *server);
static void keyboard_hand_key_release(struct kwl_server *server);
static void keyboard_hand_recognize(struct kwl_server *server);
static void keyboard_draw_hand(struct kwl_server *server, VkCommandBuffer command);
static void keyboard_send(struct kwl_server *server, const char *text);
static int keyboard_send_key(struct kwl_server *server, unsigned code, int shift);
static void keyboard_key_event(struct kwl_server *server, uint32_t time, unsigned code, uint32_t state);
static int keyboard_send_commit(struct kwl_server *server, const char *text, uint32_t before);
static void keyboard_voice(struct kwl_server *server);
static void keyboard_case(struct kwl_server *server);
static void keyboard_remember(const char *text, unsigned sent);
static void keyboard_reading_sent(struct kwl_server *server, const char *text, unsigned sent);
static void keyboard_reading_back(struct kwl_server *server);
static void keyboard_reading_replace(struct kwl_server *server, const char *before, const char *after);
static void keyboard_reading_end(struct kwl_server *server, const char *why);
static void keyboard_reading_predict(struct kwl_server *server);
static int keyboard_hiragana(const char *text);
static int keyboard_secret(const struct kwl_text_input *input);
static void keyboard_candidate_rect(struct kwl_server *server, unsigned slot, int32_t *rect);
static int keyboard_candidate_at(struct kwl_server *server, int32_t x, int32_t y, unsigned *slot);
static void keyboard_candidate_release(struct kwl_server *server);
static void keyboard_draw_candidates(struct kwl_server *server, VkCommandBuffer command);
static void keyboard_close_rect(int32_t *rect);
static void keyboard_draw_panel(struct kwl_server *server, VkCommandBuffer command, const int32_t *rect, float opacity);
static void keyboard_draw_hint(struct kwl_server *server, VkCommandBuffer command);
static void keyboard_draw_sliding(struct kwl_server *server, VkCommandBuffer command, int leaving);
static float keyboard_slide(void);
static void keyboard_work_area(struct kwl_server *server);
static int keyboard_is_window(const struct kwl_object *surface);
static void keyboard_fit_floating(struct kwl_server *server, struct kwl_object *surface);
static void keyboard_move_back(struct kwl_server *server);
static void keyboard_move_step(struct kwl_server *server, float t);
static int keyboard_window_live(struct kwl_server *server, const struct kwl_object *window);
static void keyboard_moves_at_end(struct kwl_server *server, int end);
static void keyboard_tool_rect(struct kwl_server *server, unsigned index, int32_t *rect);
static int keyboard_tool_at(struct kwl_server *server, int32_t x, int32_t y, unsigned *index);
static int keyboard_tool_enabled(struct kwl_server *server, unsigned index, uint32_t enabled, int state);
static void keyboard_tool_release(struct kwl_server *server);
static void keyboard_tool_move(struct kwl_server *server, unsigned code);
static void keyboard_tool_edit(struct kwl_server *server, unsigned action);
static void keyboard_draw_tools(struct kwl_server *server, VkCommandBuffer command);
static void keyboard_history_rect(struct kwl_server *server, unsigned row, int32_t *rect);
static int keyboard_history_at(struct kwl_server *server, int32_t x, int32_t y, unsigned *row);
static void keyboard_history_release(struct kwl_server *server);
static void keyboard_draw_history(struct kwl_server *server, VkCommandBuffer command);
static void keyboard_emoji_rect(struct kwl_server *server, unsigned slot, int32_t *rect);
static int keyboard_emoji_at(struct kwl_server *server, int32_t x, int32_t y, unsigned *slot);
static void keyboard_emoji_release(struct kwl_server *server);
static void keyboard_emoji_log(struct kwl_server *server);
static void keyboard_draw_emoji(struct kwl_server *server, VkCommandBuffer command);
static const char *keyboard_kind_name(enum keyboard_kind kind);
static const char *keyboard_source_name(enum kwl_contact_source source);
static int keyboard_touch_button(struct kwl_server *server, int32_t x, int32_t y, uint32_t state);

/*
 * Feeds the left pointer button (the mouse's, or a finger's passed as the
 * pointer) to the keyboard: a press in a bottom corner begins the corner's
 * gesture, a press on the open panel is the panel's, and a release ends
 * whichever began.  Returns 1 when the button is the keyboard's.
 */
int
kwl_keyboard_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	int taken;

	/* Only the left button. */
	if (button != KWL_BUTTON_LEFT)
		return 0;

	/* A release ends the corner's contact, when it is the keyboard's. */
	if (state == 0) {
		taken = keyboard_contact_end(server, server->pointer_x, server->pointer_y, server->input_time);
		if (taken)
			return 1;

		/* Or the press on the panel. */
		taken = keyboard_panel_button(server, button, state);
		return taken;
	}

	/* A press in a bottom corner begins the corner's gesture, also where an open panel reaches the corner. */
	taken = keyboard_contact_begin(server, server->pointer_x, server->pointer_y, server->input_time);
	if (taken)
		return 1;

	/* Succeeded: a press on the open panel is the panel's. */
	taken = keyboard_panel_button(server, button, state);
	return taken;
}

/*
 * Feeds the pointer's movement to the keyboard while the corner's contact
 * is followed (only the source that began it moves it).  Returns 1 when the
 * movement is the keyboard's.
 */
int
kwl_keyboard_motion(
	struct kwl_server *server)
{
	int taken;

	/* A press held on the panel keeps the pointer's movement; a key held follows it (its petals). */
	if (keyboard.pressing) {
		if (keyboard.key_active) {
			keyboard.key_x = server->pointer_x;
			keyboard.key_y = server->pointer_y;
			server->dirty = 1;
		}

		/* A stroke being written follows the pointer. */
		if (keyboard.writing)
			keyboard_hand_point(server, server->pointer_x, server->pointer_y, 0);

		/* The movement is the panel's. */
		return 1;
	}

	/* Only the contact of the source moving the pointer now. */
	if (!keyboard.contact.active || keyboard.contact.source != server->shell_source)
		return 0;

	/* Succeeded: the contact moves with the pointer. */
	taken = keyboard_contact_move(server, server->pointer_x, server->pointer_y, server->input_time);
	return taken;
}

/*
 * A finger touches the open panel (touch.c's ROUTE_OSK, ws102-p009): its
 * own press, at its place, while other fingers may be anywhere.  A finger
 * that still holds the panel's press lets it go first where it was last
 * (the key it held acts), so that fingers overlapping in time (two thumbs
 * typing) each type their key once.  Returns 1 when the panel took the
 * finger.
 */
int
kwl_keyboard_touch_down(
	struct kwl_server *server,
	uint32_t id,
	int32_t x,
	int32_t y,
	uint32_t time)
{
	int rolled;
	int taken;

	/* A press of the mouse holds the panel: the finger is not taken. */
	(void)time;
	if (keyboard.pressing && keyboard.touch_owner == 0U)
		return 0;

	/* The finger holding the press lets it go first. */
	rolled = 0;
	if (keyboard.pressing && keyboard.touch_owner != 0U) {
		rolled = 1;
		(void)keyboard_touch_button(server, keyboard.touch_x, keyboard.touch_y, 0U);
		keyboard.touch_owner = 0U;
	}

	/* The finger's press, at its place. */
	taken = keyboard_touch_button(server, x, y, 1U);
	if (taken) {
		keyboard.touch_owner = id + 1U;
		keyboard.touch_x = x;
		keyboard.touch_y = y;
	}

	/* Succeeded: said for the tests. */
	printf("KWL OSK touch down id=%u x=%d y=%d taken=%d rollover=%d\n", id, x, y, taken, rolled);
	return taken;
}

/*
 * A finger of the panel moves: the press follows it while the finger
 * holds it (a flick's petals, a stroke).  Returns 1.
 */
int
kwl_keyboard_touch_motion(
	struct kwl_server *server,
	uint32_t id,
	int32_t x,
	int32_t y,
	uint32_t time)
{
	int32_t saved_x;
	int32_t saved_y;

	/* Only the finger holding the press moves it. */
	(void)time;
	if (keyboard.touch_owner != id + 1U || !keyboard.pressing)
		return 1;

	/* The press follows the finger (the pointer's place for keyboard.c's handlers, the pointer itself stays). */
	keyboard.touch_x = x;
	keyboard.touch_y = y;
	saved_x = server->pointer_x;
	saved_y = server->pointer_y;
	server->pointer_x = x;
	server->pointer_y = y;
	(void)kwl_keyboard_motion(server);
	server->pointer_x = saved_x;
	server->pointer_y = saved_y;
	return 1;
}

/*
 * A finger of the panel lifts: the press it holds ends there (the key
 * acts); a finger whose press another finger already ended does nothing.
 * Returns 1.
 */
int
kwl_keyboard_touch_up(
	struct kwl_server *server,
	uint32_t id,
	int32_t x,
	int32_t y,
	uint32_t time)
{
	int acted;

	/* A finger that does not hold the press. */
	(void)time;
	acted = 0;
	if (keyboard.touch_owner == id + 1U && keyboard.pressing) {
		(void)keyboard_touch_button(server, x, y, 0U);
		acted = 1;
	}

	/* The press is no finger's now. */
	if (keyboard.touch_owner == id + 1U)
		keyboard.touch_owner = 0U;
	printf("KWL OSK touch up id=%u x=%d y=%d acted=%d\n", id, x, y, acted);
	return 1;
}

/*
 * A finger of the panel is given up (its screen went): the press it holds
 * ends without acting.
 */
void
kwl_keyboard_touch_cancel(
	struct kwl_server *server,
	uint32_t id)
{
	/* Only the finger holding the press. */
	if (keyboard.touch_owner != id + 1U)
		return;

	/* The press ends; a stroke being written ends as a stroke. */
	keyboard.touch_owner = 0U;
	if (keyboard.writing)
		keyboard_hand_release(server);
	keyboard.pressing = 0;
	keyboard.key_active = 0;
	keyboard.band_active = 0;
	keyboard.hand_key_active = 0;
	server->dirty = 1;
	printf("KWL OSK touch cancel id=%u\n", id);
}

/*
 * Feeds a finger's press or release to the panel as the pointer's left
 * button at the finger's place; the pointer itself stays where it is.
 * Returns 1 when the panel took it.
 */
static int
keyboard_touch_button(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	uint32_t state)
{
	int32_t saved_x;
	int32_t saved_y;
	int taken;

	/* The finger's place as the pointer's for the panel's handler. */
	saved_x = server->pointer_x;
	saved_y = server->pointer_y;
	server->pointer_x = x;
	server->pointer_y = y;
	taken = keyboard_panel_button(server, KWL_BUTTON_LEFT, state);
	server->pointer_x = saved_x;
	server->pointer_y = saved_y;
	return taken;
}

/*
 * Keeps the keyboard's time: the corners' places are logged once, a
 * contact whose release never came ends, and one that does not arm in time
 * is let go.  The panel follows a change of the screen's size.
 */
void
kwl_keyboard_tick(
	struct kwl_server *server)
{
	int32_t rect[4];
	uint64_t now;
	float slide;
	float home;
	int same;

	/* The corners' places, once, for the tests. */
	if (!keyboard.zones_logged && server->width > 0U) {
		keyboard.zones_logged = 1;
		printf("KWL OSK zone kind=flick x=%d y=%d size=%d\n", (int)server->width - KEYBOARD_ZONE, (int)server->height - KEYBOARD_ZONE, KEYBOARD_ZONE);
		printf("KWL OSK zone kind=qwerty x=0 y=%d size=%d\n", (int)server->height - KEYBOARD_ZONE, KEYBOARD_ZONE);
	}

	/* A contact whose release never came (its device went away with the button held) ends. */
	if (keyboard.contact.active && (server->buttons_down & 1U) == 0U) {
		keyboard.contact.active = 0;
		server->dirty = 1;
		printf("KWL OSK cancel reason=lost\n");
	}

	/* So does a press on the panel (not a finger's, which touch.c ends, ws102-p009). */
	if (keyboard.pressing && keyboard.touch_owner == 0U && (server->buttons_down & 1U) == 0U)
		keyboard.pressing = 0;

	/* A contact still waiting to arm times out. */
	now = kwl_milliseconds();
	if (keyboard.contact.active &&
	    !keyboard.contact.armed &&
	    !keyboard.contact.expired &&
	    now - keyboard.contact.start_clock_ms > KEYBOARD_ARM_MS) {
		keyboard.contact.expired = 1;
		printf("KWL OSK cancel reason=timeout\n");
	}

	/* The windows the work area moves follow the slide, and are left at their ends once it is over. */
	if (keyboard.moving) {
		slide = keyboard_slide();
		keyboard_move_step(server, slide);
		if (slide >= 1.0f)
			keyboard.moving = 0;
	}

	/* A panel growing out of its edge or going back into it is drawn every frame; one gone back is done with. */
	if (now - keyboard.slide_ms < KEYBOARD_SLIDE_MS) {
		server->dirty = 1;
	} else if (keyboard.leaving != PANEL_NONE) {
		keyboard.leaving = PANEL_NONE;
		server->dirty = 1;
	}

	/* The faces follow the kind of the field served (q893), open or not. */
	keyboard_follow_field(server);

	/* A panel App Home or Wiseview put away comes back when they are gone (BUG-229). */
	home = kwl_home_progress(server);
	if (keyboard.open == PANEL_NONE && keyboard.restore != PANEL_NONE)
		keyboard_restore(server, home);

	/* Nothing more without an open panel. */
	if (keyboard.open == PANEL_NONE)
		return;

	/* The login and lock screens close it; App Home and Wiseview put it away until they are gone. */
	if (server->greeter) {
		kwl_keyboard_close(server, "greeter");
		return;
	} else if (server->locked) {
		kwl_keyboard_close(server, "lock");
		return;
	} else if (home > 0.0f || server->home_to > 0.0f) {
		keyboard_put_away(server, "home");
		return;
	} else if (server->wiseview > 0.0f || server->wiseview_gesture || server->wiseview_moving) {
		keyboard_put_away(server, "wiseview");
		return;
	}

	/* The handwriting's ink is recognized a while after its last stroke. */
	if (keyboard.hand &&
	    !keyboard.writing &&
	    !keyboard.recognized &&
	    keyboard_ink.count > 0U &&
	    now - keyboard.stroke_end_ms >= KEYBOARD_HAND_WAIT_MS)
		keyboard_hand_recognize(server);

	/* The open panel's place for the screen's size now. */
	keyboard_place(server, keyboard.open, rect);
	same = memcmp(rect, keyboard.panel, sizeof(rect));
	if (same != 0) {
		memcpy(keyboard.panel, rect, sizeof(rect));
		server->dirty = 1;
		printf("KWL OSK place kind=%s x=%d y=%d width=%d height=%d\n", keyboard_kind_name(keyboard.open), rect[0], rect[1], rect[2], rect[3]);
	}
}

/*
 * Tells whether the keyboard shows something over the windows: an open
 * panel, or the hint of an armed contact.  While it does, the output is
 * composed, even over a fullscreen window.
 */
int
kwl_keyboard_showing(
	void)
{
	/* An open panel, or one going back into its edge. */
	if (keyboard.open != PANEL_NONE || keyboard.leaving != PANEL_NONE)
		return 1;

	/* An armed contact that has not timed out. */
	if (keyboard.contact.active &&
	    keyboard.contact.armed &&
	    !keyboard.contact.expired)
		return 1;

	/* Nothing shows. */
	return 0;
}

/*
 * Tells whether a point of the output is on the open panel (a press there
 * is the keyboard's, not a window's).
 */
int
kwl_keyboard_at(
	int32_t x,
	int32_t y)
{
	int inside;

	/* No panel. */
	if (keyboard.open == PANEL_NONE)
		return 0;

	/* Succeeded: whether the point is on it. */
	inside = keyboard_contains(keyboard.panel, x, y);
	return inside;
}

/*
 * Closes the open panel, saying why in the log.
 */
void
kwl_keyboard_close(
	struct kwl_server *server,
	const char *reason)
{
	/* No panel. */
	if (keyboard.open == PANEL_NONE)
		return;

	/* The panel goes, going back into its edge (drawn until it is in). */
	printf("KWL OSK close kind=%s reason=%s\n", keyboard_kind_name(keyboard.open), reason);
	keyboard.leaving = keyboard.open;
	keyboard.slide_ms = kwl_milliseconds();
	keyboard.open = PANEL_NONE;
	keyboard.tool_active = 0;
	keyboard.history_active = 0;
	keyboard.emoji_active = 0;
	keyboard.candidate_active = 0;
	keyboard.selecting = 0;
	keyboard_reading_end(server, "close");
	keyboard_work_area(server);
	keyboard.held = 0;
	keyboard.pressing = 0;
	keyboard.touch_owner = 0U;
	keyboard.key_active = 0;
	keyboard.band_active = 0;
	keyboard.writing = 0;
	keyboard.hand_key_active = 0;
	kwl_hand_clear(&keyboard_ink);
	keyboard.recognized = 0;
	memset(&keyboard.result, 0, sizeof(keyboard.result));
	server->dirty = 1;
}

/*
 * Gives what the open panel takes from the work area: the flick panel's
 * width at the right, the QWERTY panel's height at the bottom (0 without a
 * panel).  The glass look's space and the desktop's place take these away.
 */
void
kwl_keyboard_reserved(
	int32_t *right,
	int32_t *bottom)
{
	/* The panel's column or row, whole. */
	*right = keyboard.reserved_right;
	*bottom = keyboard.reserved_bottom;
}

/*
 * Gives what the panel takes from the screen at this moment of its slide:
 * a growing share while it comes out, a shrinking one while it goes back.
 * The docked windows' place takes these away, so that they narrow and
 * shorten with the panel.
 */
void
kwl_keyboard_reserved_now(
	int32_t *right,
	int32_t *bottom)
{
	enum keyboard_kind kind;
	float out;

	/* Nothing out. */
	*right = 0;
	*bottom = 0;

	/* The panel out or going back: none is nothing. */
	kind = keyboard.open;
	if (kind == PANEL_NONE)
		kind = keyboard.leaving;
	if (kind == PANEL_NONE)
		return;

	/* The share out: growing while it comes, shrinking while it goes back. */
	out = keyboard_slide();
	if (keyboard.leaving != PANEL_NONE)
		out = 1.0f - out;

	/* The flick panel's width at the right, the QWERTY panel's height at the bottom. */
	if (kind == PANEL_FLICK) {
		*right = (int32_t)(out * (float)keyboard.panel[2]);
	} else {
		*bottom = (int32_t)(out * (float)keyboard.panel[3]);
	}
}

/*
 * Draws the keyboard over everything else: the open panel, and the hint of
 * an armed contact growing from its corner.
 */
void
kwl_keyboard_draw(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	/* The held key's colours: the accent the user chose, on the panel's ground. */
	kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 1.0f, keyboard_blue);
	kwl_accent_colour(server, server->dark, KWL_ACCENT_INK, 1.0f, keyboard_on_blue);

	/* The open panel, grown out of its edge as far as its slide has come. */
	if (keyboard.open != PANEL_NONE)
		keyboard_draw_sliding(server, command, 0);

	/* A panel going back into its edge, drawn as it was (its face), further in each frame. */
	if (keyboard.leaving != PANEL_NONE) {
		keyboard.open = keyboard.leaving;
		keyboard_draw_sliding(server, command, 1);
		keyboard.open = PANEL_NONE;
	}

	/* The hint of an armed contact. */
	if (keyboard.contact.active &&
	    keyboard.contact.armed &&
	    !keyboard.contact.expired)
		keyboard_draw_hint(server, command);
}

/*
 * Starts following a contact that begins in a bottom corner.  Returns 1
 * when the contact is the keyboard's.
 */
static int
keyboard_contact_begin(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	uint32_t time)
{
	enum keyboard_kind corner;
	float home;
	int open;

	/* The login and lock screens, App Home and Wiseview have no keyboard. */
	if (server->greeter || server->locked)
		return 0;
	home = kwl_home_progress(server);
	if (home > 0.0f || server->home_to > 0.0f)
		return 0;
	if (server->wiseview > 0.0f || server->wiseview_gesture || server->wiseview_moving)
		return 0;

	/* A contact already followed keeps the gesture. */
	if (keyboard.contact.active)
		return 0;

	/* Only a contact in a bottom corner. */
	corner = keyboard_corner_at(server, x, y);
	if (corner == PANEL_NONE)
		return 0;

	/* An open menu closes on a press anywhere, the corners too, before the gesture could start. */
	open = kwl_network_is_open();
	if (open)
		return 0;
	open = kwl_menu_is_open();
	if (open)
		return 0;
	open = kwl_volume_is_open();
	if (open)
		return 0;

	/* The contact from its start; it is not armed yet. */
	memset(&keyboard.contact, 0, sizeof(keyboard.contact));
	keyboard.contact.active = 1;
	keyboard.contact.corner = corner;
	keyboard.contact.source = server->shell_source;
	keyboard.contact.start_x = x;
	keyboard.contact.start_y = y;
	keyboard.contact.start_time = time;
	keyboard.contact.start_clock_ms = kwl_milliseconds();
	keyboard.contact.x = x;
	keyboard.contact.y = y;
	keyboard_sample(x, y, time);

	/* Succeeded: the contact is the keyboard's. */
	printf("KWL OSK press corner=%s source=%s x=%d y=%d\n", keyboard_kind_name(corner), keyboard_source_name(server->shell_source), x, y);
	return 1;
}

/*
 * Follows the corner's contact to a new point: it arms once it has moved
 * far enough inwards and up in time.  Returns 1 when it is the keyboard's.
 */
static int
keyboard_contact_move(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	uint32_t time)
{
	uint32_t elapsed;
	int32_t inwards;
	int32_t upwards;

	/* A contact the keyboard does not have goes on. */
	if (!keyboard.contact.active)
		return 0;

	/* The point, for the hint and for the speed at the end. */
	keyboard.contact.x = x;
	keyboard.contact.y = y;
	keyboard_sample(x, y, time);

	/* A contact that timed out stays the keyboard's until it ends, without effect. */
	if (keyboard.contact.expired)
		return 1;

	/* An armed contact redraws the hint. */
	if (keyboard.contact.armed) {
		server->dirty = 1;
		return 1;
	}

	/* Too late to arm: the contact is let go (still the keyboard's until it ends). */
	elapsed = time - keyboard.contact.start_time;
	if (elapsed > KEYBOARD_ARM_MS) {
		keyboard.contact.expired = 1;
		printf("KWL OSK cancel reason=timeout\n");
		return 1;
	}

	/* It arms once it has moved far enough inwards and far enough up. */
	keyboard_travel(&inwards, &upwards);
	if (inwards < KEYBOARD_ARM || upwards < KEYBOARD_ARM)
		return 1;

	/* Succeeded: the contact is armed, and the hint shows from now on. */
	keyboard.contact.armed = 1;
	server->dirty = 1;
	printf("KWL OSK armed corner=%s ms=%u\n", keyboard_kind_name(keyboard.contact.corner), elapsed);
	return 1;
}

/*
 * Ends the corner's contact: it commits when it ended near the diagonal,
 * far enough along it or quickly enough, and is let go otherwise.  Returns
 * 1 when the contact was the keyboard's.
 */
static int
keyboard_contact_end(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	uint32_t time)
{
	int32_t inwards;
	int32_t upwards;
	int diagonal;
	float progress;
	float speed;

	/* A contact the keyboard does not have goes on. */
	if (!keyboard.contact.active)
		return 0;

	/* The last point, which the speed is measured to. */
	keyboard.contact.x = x;
	keyboard.contact.y = y;
	keyboard_sample(x, y, time);
	server->dirty = 1;

	/* A contact that timed out ends without effect (its cancel was logged then). */
	if (keyboard.contact.expired) {
		keyboard.contact.active = 0;
		return 1;
	}

	/* A contact that never armed ends without effect, like a tap in the corner. */
	if (!keyboard.contact.armed) {
		keyboard.contact.active = 0;
		printf("KWL OSK cancel reason=unarmed\n");
		return 1;
	}

	/* A contact that ends off the diagonal is let go. */
	keyboard_travel(&inwards, &upwards);
	diagonal = keyboard_on_diagonal(inwards, upwards);
	if (!diagonal) {
		keyboard.contact.active = 0;
		printf("KWL OSK cancel reason=direction\n");
		return 1;
	}

	/* Far enough along the diagonal commits. */
	progress = ((float)inwards + (float)upwards) * 0.5f;
	if (progress >= KEYBOARD_COMMIT) {
		keyboard_commit(server, "distance", progress);
		return 1;
	}

	/* A quick flick commits after a shorter way. */
	speed = keyboard_speed();
	if (progress >= KEYBOARD_FLICK_DISTANCE && speed >= KEYBOARD_FLICK_SPEED) {
		keyboard_commit(server, "flick", progress);
		return 1;
	}

	/* Succeeded: too short and too slow, the contact is let go. */
	keyboard.contact.active = 0;
	printf("KWL OSK cancel reason=short progress=%.0f speed=%.2f\n", (double)progress, (double)speed);
	return 1;
}

/* Finds the bottom corner a point is in: the flick panel's (right), the QWERTY panel's (left), or none. */
static enum keyboard_kind
keyboard_corner_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	/* Only the bottom strip. */
	if (y < (int32_t)server->height - KEYBOARD_ZONE)
		return PANEL_NONE;

	/* The right corner. */
	if (x >= (int32_t)server->width - KEYBOARD_ZONE)
		return PANEL_FLICK;

	/* The left corner. */
	if (x < KEYBOARD_ZONE)
		return PANEL_QWERTY;

	/* Between them. */
	return PANEL_NONE;
}

/* Works out how far the contact has moved inwards (away from its corner's side) and up. */
static void
keyboard_travel(
	int32_t *inwards,
	int32_t *upwards)
{
	/* Leftwards from the right corner, rightwards from the left one. */
	*inwards = keyboard.contact.x - keyboard.contact.start_x;
	if (keyboard.contact.corner == PANEL_FLICK)
		*inwards = keyboard.contact.start_x - keyboard.contact.x;

	/* Up from either. */
	*upwards = keyboard.contact.start_y - keyboard.contact.y;
}

/* Keeps a point of the contact among the recent ones. */
static void
keyboard_sample(
	int32_t x,
	int32_t y,
	uint32_t time)
{
	struct keyboard_sample *sample;

	/* The next place of the ring. */
	sample = &keyboard.contact.samples[keyboard.contact.sample_next];
	sample->x = x;
	sample->y = y;
	sample->time = time;
	keyboard.contact.sample_next = (keyboard.contact.sample_next + 1U) % KEYBOARD_SAMPLES;
	if (keyboard.contact.sample_count < KEYBOARD_SAMPLES)
		keyboard.contact.sample_count++;
}

/*
 * Measures the contact's speed along the diagonal over the last
 * KEYBOARD_FLICK_MS, in pixels a millisecond (0 without two points).
 */
static float
keyboard_speed(
	void)
{
	const struct keyboard_sample *last;
	const struct keyboard_sample *first;
	const struct keyboard_sample *sample;
	uint32_t elapsed;
	unsigned index;
	unsigned count;
	float inwards;
	float upwards;

	/* The newest point. */
	if (keyboard.contact.sample_count < 2U)
		return 0.0f;
	index = (keyboard.contact.sample_next + KEYBOARD_SAMPLES - 1U) % KEYBOARD_SAMPLES;
	last = &keyboard.contact.samples[index];

	/* The oldest point within the window, walking back. */
	first = last;
	for (count = 1U; count < keyboard.contact.sample_count; count++) {
		index = (index + KEYBOARD_SAMPLES - 1U) % KEYBOARD_SAMPLES;
		sample = &keyboard.contact.samples[index];
		if (last->time - sample->time > KEYBOARD_FLICK_MS)
			break;
		first = sample;
	}

	/* No time between them: no speed. */
	elapsed = last->time - first->time;
	if (elapsed == 0U)
		return 0.0f;

	/* The movement inwards and up, along the diagonal. */
	inwards = (float)(last->x - first->x);
	if (keyboard.contact.corner == PANEL_FLICK)
		inwards = -inwards;
	upwards = (float)(first->y - last->y);

	/* Succeeded: the speed. */
	return (inwards + upwards) * 0.5f / (float)elapsed;
}

/* Tells whether a movement (inwards, up) is within the cone around the diagonal. */
static int
keyboard_on_diagonal(
	int32_t inwards,
	int32_t upwards)
{
	/* Both ways must be positive. */
	if (inwards <= 0 || upwards <= 0)
		return 0;

	/* The shorter way against the longer. */
	if (inwards * KEYBOARD_CONE_DENOMINATOR < upwards * KEYBOARD_CONE_NUMERATOR)
		return 0;
	if (upwards * KEYBOARD_CONE_DENOMINATOR < inwards * KEYBOARD_CONE_NUMERATOR)
		return 0;

	/* On the diagonal. */
	return 1;
}

/*
 * Acts on a committed swipe: the corner's panel opens, or closes when it
 * is the one open (the same swipe again).
 */
static void
keyboard_commit(
	struct kwl_server *server,
	const char *via,
	float progress)
{
	enum keyboard_kind corner;

	/* The contact is over. */
	corner = keyboard.contact.corner;
	keyboard.contact.active = 0;
	printf("KWL OSK commit corner=%s via=%s progress=%.0f\n", keyboard_kind_name(corner), via, (double)progress);

	/* The same panel again closes it. */
	if (keyboard.open == corner) {
		kwl_keyboard_close(server, "gesture");
		return;
	}

	/* Otherwise the corner's panel opens (in place of the other one). */
	keyboard_open(server, corner);
}

/* Opens a panel at its place for the screen's size. */
static void
keyboard_open(
	struct kwl_server *server,
	enum keyboard_kind kind)
{
	/* A panel opened is no longer one waiting to come back. */
	keyboard.restore = PANEL_NONE;
	keyboard.restore_focus = NULL;

	/* The panel and its rectangle, growing out of its edge from now (one going back is done with). */
	keyboard.open = kind;
	keyboard.leaving = PANEL_NONE;
	keyboard.slide_ms = kwl_milliseconds();
	keyboard.pressing = 0;
	keyboard_place(server, kind, keyboard.panel);
	server->dirty = 1;

	/* The log line the tests read, and the QWERTY panel's keys' places. */
	printf("KWL OSK open kind=%s x=%d y=%d width=%d height=%d\n", keyboard_kind_name(kind), keyboard.panel[0], keyboard.panel[1], keyboard.panel[2], keyboard.panel[3]);

	/* The work area less the panel; the windows follow it (their insets told before their configures, inset.c). */
	keyboard_work_area(server);
	if (kind == PANEL_QWERTY)
		keyboard_qwerty_log(server);
}

/*
 * Puts the open panel away while App Home or Wiseview covers the windows,
 * keeping which panel it was and which window had the focus (BUG-229).
 */
static void
keyboard_put_away(
	struct kwl_server *server,
	const char *reason)
{
	enum keyboard_kind kind;

	/* The panel and the focus to come back to. */
	kind = keyboard.open;
	kwl_keyboard_close(server, reason);
	keyboard.restore = kind;
	keyboard.restore_focus = server->focus;
	printf("KWL OSK put-away kind=%s reason=%s\n", keyboard_kind_name(kind), reason);
}

/*
 * Brings back the panel App Home or Wiseview put away, once they are gone,
 * when the window that had the focus has it again (BUG-229).  The login
 * and lock screens, or another window taking the focus, give it up.
 */
static void
keyboard_restore(
	struct kwl_server *server,
	float home)
{
	enum keyboard_kind kind;
	int live;

	/* The login and lock screens end the wait. */
	if (server->greeter || server->locked) {
		printf("KWL OSK restore-dropped reason=%s\n", server->greeter ? "greeter" : "lock");
		keyboard.restore = PANEL_NONE;
		keyboard.restore_focus = NULL;
		return;
	}

	/* App Home or Wiseview still covers the windows. */
	if (home > 0.0f || server->home_to > 0.0f)
		return;
	if (server->wiseview > 0.0f || server->wiseview_gesture || server->wiseview_moving)
		return;

	/* The same window, still a window, must have the focus again. */
	kind = keyboard.restore;
	keyboard.restore = PANEL_NONE;
	live = 0;
	if (server->focus != NULL && server->focus == keyboard.restore_focus)
		live = keyboard_window_live(server, server->focus);
	keyboard.restore_focus = NULL;
	if (live == 0) {
		printf("KWL OSK restore-dropped reason=focus\n");
		return;
	}

	/* The panel comes back as it was. */
	printf("KWL OSK restore kind=%s\n", keyboard_kind_name(kind));
	keyboard_open(server, kind);
}

/*
 * Works out a panel's rectangle (x, y, width, height) for the screen's
 * size, against its edges: the flick panel the right column under the
 * system bar (as wide as its keys, a share of the height); the QWERTY
 * panel the bottom row (a share of the height).
 */
static void
keyboard_place(
	struct kwl_server *server,
	enum keyboard_kind kind,
	int32_t *rect)
{
	int32_t width;
	int32_t height;
	int32_t key;

	/* The screen. */
	width = (int32_t)server->width;
	height = (int32_t)server->height;

	/* The QWERTY panel: the whole bottom row, a share of the height. */
	if (kind == PANEL_QWERTY) {
		rect[3] = height * KEYBOARD_QWERTY_SHARE / 100;
		if (rect[3] < KEYBOARD_QWERTY_MIN)
			rect[3] = KEYBOARD_QWERTY_MIN;
		if (rect[3] > KEYBOARD_QWERTY_MAX)
			rect[3] = KEYBOARD_QWERTY_MAX;
		rect[2] = width;
		rect[0] = 0;
		rect[1] = height - rect[3];
		return;
	}

	/* The flick panel's key side. */
	key = keyboard_key_size(server);

	/* The whole right column under the system bar, as wide as the keys and their gaps. */
	rect[2] = KEYBOARD_FLICK_COLUMNS * key + (KEYBOARD_FLICK_COLUMNS + 1) * KEYBOARD_KEY_GAP;
	rect[3] = height - KWL_GLASS_BAR;
	rect[0] = width - rect[2];
	rect[1] = KWL_GLASS_BAR;
}

/*
 * Handles the left button on the open panel: a press on it is the
 * panel's (the close key's press closes it at the release); its release
 * ends it.  Returns 1 when the button was the panel's.
 */
static int
keyboard_panel_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	int32_t close[4];
	unsigned row;
	unsigned column;
	int swiped;
	int inside;
	int found;

	/* The release of a press the panel took: a key held acts, the close key closes. */
	(void)button;
	if (state == 0) {
		if (!keyboard.pressing)
			return 0;
		keyboard.pressing = 0;

		/* The title band dragged far enough towards the panel's edge closes it. */
		if (keyboard.band_active) {
			keyboard.band_active = 0;
			swiped = keyboard_band_swiped(server->pointer_x, server->pointer_y);
			if (swiped) {
				kwl_keyboard_close(server, "swipe");
				return 1;
			}
		}

		/* A stroke ends. */
		if (keyboard.writing) {
			keyboard_hand_release(server);
			return 1;
		}

		/* A tool of the flick panel acts. */
		if (keyboard.tool_active) {
			keyboard_tool_release(server);
			return 1;
		}

		/* A row of the history is pasted. */
		if (keyboard.history_active) {
			keyboard_history_release(server);
			return 1;
		}

		/* An emoji is sent, or another category is shown. */
		if (keyboard.emoji_active) {
			keyboard_emoji_release(server);
			return 1;
		}

		/* A word of the candidates' tab replaces the reading. */
		if (keyboard.candidate_active) {
			keyboard_candidate_release(server);
			return 1;
		}

		/* A key of the handwriting face acts. */
		if (keyboard.hand_key_active) {
			keyboard_hand_key_release(server);
			return 1;
		}

		/* A key held: what the release means, on either panel. */
		if (keyboard.key_active) {
			keyboard.key_x = server->pointer_x;
			keyboard.key_y = server->pointer_y;
			if (keyboard.open == PANEL_QWERTY) {
				keyboard_qwerty_release(server);
			} else {
				keyboard_key_release(server);
			}

			/* The release was the key's. */
			return 1;
		}

		/* On the QWERTY panel's band button, the handwriting face comes or goes. */
		keyboard_band_button_rect(close);
		inside = keyboard_contains(close, server->pointer_x, server->pointer_y);
		if (inside && keyboard.open == PANEL_QWERTY) {
			keyboard_hand_toggle(server);
			return 1;
		}

		/* On the close key, the panel closes. */
		keyboard_close_rect(close);
		inside = keyboard_contains(close, server->pointer_x, server->pointer_y);
		if (inside)
			kwl_keyboard_close(server, "key");
		return 1;
	}

	/* A press off the panel is not the panel's. */
	inside = kwl_keyboard_at(server->pointer_x, server->pointer_y);
	if (!inside)
		return 0;

	/* The press is the panel's until its release; one on the title band may close it by a drag. */
	keyboard.pressing = 1;
	keyboard.key_active = 0;
	keyboard.band_active = 0;
	if (server->pointer_y < keyboard.panel[1] + KEYBOARD_BAND) {
		keyboard.band_active = 1;
		keyboard.band_x = server->pointer_x;
		keyboard.band_y = server->pointer_y;
	}

	/* On the flick panel's tools, the tool is held; on the history's rows, the row. */
	if (keyboard.open == PANEL_FLICK) {
		found = keyboard_tool_at(server, server->pointer_x, server->pointer_y, &row);
		if (found) {
			keyboard.tool_active = 1;
			keyboard.tool = row;
			server->dirty = 1;
			return 1;
		}

		/* A row of the history, when it shows. */
		found = 0;
		if (keyboard.tools_face == KEYBOARD_FACE_HISTORY)
			found = keyboard_history_at(server, server->pointer_x, server->pointer_y, &row);
		if (found) {
			keyboard.history_active = 1;
			keyboard.history_row = row;
			server->dirty = 1;
			return 1;
		}

		/* A tab or an emoji of the emoji face, when it shows. */
		found = 0;
		if (keyboard.tools_face == KEYBOARD_FACE_EMOJI)
			found = keyboard_emoji_at(server, server->pointer_x, server->pointer_y, &row);
		if (found) {
			keyboard.emoji_active = 1;
			keyboard.emoji_slot = row;
			server->dirty = 1;
			return 1;
		}

		/* A word of the candidates' tab, when it shows. */
		found = 0;
		if (keyboard.tools_face == KEYBOARD_FACE_CANDIDATES)
			found = keyboard_candidate_at(server, server->pointer_x, server->pointer_y, &row);
		if (found) {
			keyboard.candidate_active = 1;
			keyboard.candidate_slot = row;
			server->dirty = 1;
			return 1;
		}
	}

	/* On a key, the key is held (the flick panel's petals, the QWERTY panel's bubble show). */
	found = 0;
	if (keyboard.open == PANEL_FLICK)
		found = keyboard_key_at(server, server->pointer_x, server->pointer_y, &row, &column);
	if (keyboard.open == PANEL_QWERTY && !keyboard.hand)
		found = keyboard_qwerty_at(server, server->pointer_x, server->pointer_y, &row, &column);

	/* On the handwriting face: a stroke begins on the writing area, a key at the right is held. */
	if (keyboard.open == PANEL_QWERTY && keyboard.hand) {
		keyboard_hand_area(close);
		inside = keyboard_contains(close, server->pointer_x, server->pointer_y);
		if (inside) {
			keyboard_hand_point(server, server->pointer_x, server->pointer_y, 1);
			printf("KWL OSK hand stroke-begin x=%d y=%d\n", server->pointer_x, server->pointer_y);
			return 1;
		}

		/* A key at the right. */
		found = keyboard_hand_key_at(server->pointer_x, server->pointer_y, &row);
		if (found) {
			keyboard.hand_key_active = 1;
			keyboard.hand_key = row;
			server->dirty = 1;
			return 1;
		}

		/* Anywhere else on the face (the gaps), nothing is held. */
		found = 0;
	}

	/* A key found on the flick or the QWERTY panel is held. */
	if (found) {
		keyboard.key_active = 1;
		keyboard.key_row = row;
		keyboard.key_column = column;
		keyboard.key_start_x = server->pointer_x;
		keyboard.key_start_y = server->pointer_y;
		keyboard.key_x = server->pointer_x;
		keyboard.key_y = server->pointer_y;
		server->dirty = 1;
	}

	/* Succeeded: the press is the panel's. */
	printf("KWL OSK panel-press x=%d y=%d key=%d\n", server->pointer_x, server->pointer_y, found);
	return 1;
}

/* Tells whether a point is in a rectangle (x, y, width, height). */
static int
keyboard_contains(
	const int32_t *rect,
	int32_t x,
	int32_t y)
{
	/* Left or right of it. */
	if (x < rect[0] || x >= rect[0] + rect[2])
		return 0;

	/* Above or below it. */
	if (y < rect[1] || y >= rect[1] + rect[3])
		return 0;

	/* In it. */
	return 1;
}

/* Works out the close key's rectangle, at the right of the open panel's title band. */
static void
keyboard_close_rect(
	int32_t *rect)
{
	/* A square in the band, a margin from the panel's right edge. */
	rect[2] = KEYBOARD_CLOSE;
	rect[3] = KEYBOARD_CLOSE;
	rect[0] = keyboard.panel[0] + keyboard.panel[2] - KEYBOARD_MARGIN - KEYBOARD_CLOSE;
	rect[1] = keyboard.panel[1] + (KEYBOARD_BAND - KEYBOARD_CLOSE) / 2 + 4;
}

/*
 * Draws a panel: its glass, square against the screen's edges, the line
 * on its side facing the windows, the title at the left of the band and
 * the close key at its right.
 */
static void
keyboard_draw_panel(
	struct kwl_server *server,
	VkCommandBuffer command,
	const int32_t *rect,
	float opacity)
{
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float key[4] = { 0.12f, 0.16f, 0.24f, 0.08f };
	struct glass_shape shape;
	static const float key_ground[4] = { 0.86f, 0.89f, 0.93f, 0.95f };
	float edge_line[4] = { 0.12f, 0.16f, 0.24f, 0.0f };
	int32_t close[4];
	int32_t button[4];
	int32_t advance;
	const char *title;
	const char *label;

	/* The glass, square against the screen's edges (the band of an edge, as the system bar). */
	glass_shape_init(&shape, (float)rect[0], (float)rect[1], (float)rect[2], (float)rect[3]);
	shape.mode = MODE_GLASS;
	shape.radius = 0.0f;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.86f;
	shape.opacity = opacity;
	glass_shape_draw(server, command, &shape);

	/* The line on the side facing the windows: a column's (the flick panel's) left, a row's (the QWERTY panel's) top. */
	edge_line[3] = KEYBOARD_EDGE_ALPHA * opacity;
	if (rect[2] < rect[3]) {
		glass_draw_solid(server, command, (float)rect[0], (float)rect[1], 1.0f, (float)rect[3], 0.0f, edge_line);
	} else {
		glass_draw_solid(server, command, (float)rect[0], (float)rect[1], (float)rect[2], 1.0f, 0.0f, edge_line);
	}

	/* The hint draws the glass alone. */
	if (opacity < 1.0f)
		return;

	/* The title at the left of the band: the flick panel's face, or the keyboard. */
	title = "かな";
	if (keyboard.face == KWL_FLICK_ALPHA)
		title = "ABC";
	if (keyboard.face == KWL_FLICK_NUMBER)
		title = "123";
	if (keyboard.open == PANEL_QWERTY)
		title = "ABC";
	if (keyboard.open == PANEL_QWERTY && keyboard.qface == KWL_QWERTY_SYMBOLS)
		title = "?123";
	if (keyboard.open == PANEL_QWERTY && keyboard.qface == KWL_QWERTY_EMAIL)
		title = "ABC @";
	if (keyboard.open == PANEL_QWERTY && keyboard.qface == KWL_QWERTY_URL)
		title = "ABC /";
	if (keyboard.open == PANEL_QWERTY && keyboard.qface == KWL_QWERTY_NUMBER)
		title = "123";
	if (keyboard.open == PANEL_QWERTY && keyboard.hand)
		title = "手書き";
	glass_draw_text(server, command, SIZE_TITLE, rect[0] + KEYBOARD_MARGIN + 4, rect[1] + KEYBOARD_BAND - 6, title, rect[2] - 3 * KEYBOARD_MARGIN - KEYBOARD_CLOSE, dark);

	/* The panel's keys: the QWERTY panel's with the held key's bubble, the flick panel's with its petals. */
	if (keyboard.open == PANEL_QWERTY && keyboard.hand) {
		keyboard_draw_hand(server, command);
	} else if (keyboard.open == PANEL_QWERTY) {
		keyboard_draw_qwerty(server, command);
		keyboard_draw_bubble(server, command);
	} else if (keyboard.open == PANEL_FLICK) {
		keyboard_draw_tools(server, command);
		if (keyboard.tools_face == KEYBOARD_FACE_HISTORY)
			keyboard_draw_history(server, command);
		if (keyboard.tools_face == KEYBOARD_FACE_EMOJI)
			keyboard_draw_emoji(server, command);
		if (keyboard.tools_face == KEYBOARD_FACE_CANDIDATES)
			keyboard_draw_candidates(server, command);
		keyboard_draw_keys(server, command);
		keyboard_draw_petals(server, command);
	}

	/* The close key: a pale round key with the multiplication sign. */
	keyboard_close_rect(close);
	glass_draw_solid(server, command, (float)close[0], (float)close[1], (float)close[2], (float)close[3], (float)close[2] / 2.0f, key);
	/* The QWERTY panel's band button: to the handwriting face, or back to the keys. */
	if (keyboard.open == PANEL_QWERTY) {
		keyboard_band_button_rect(button);
		label = "手書き";
		if (keyboard.hand)
			label = "ABC";
		keyboard_draw_key(server, command, button, label, key_ground, dark, SIZE_TITLE);
	}

	/* The close key's sign. */
	advance = glass_glyph_advance(server, SIZE_SIGN, GLASS_CLOSE_GLYPH);
	glass_draw_glyph(server, command, SIZE_SIGN, GLASS_CLOSE_GLYPH, close[0] + (close[2] - advance) / 2, close[1] + close[3] / 2 + 7, dark);
}

/*
 * Draws the hint of an armed contact as Notes' corner draws its own
 * (BUG-230, corner.c): a glass quarter disc from the bottom corner out to
 * the contact, its rim blue once letting go would open the panel, and the
 * keyboard's name along the diagonal inside it once there is room.
 */
static void
keyboard_draw_hint(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float ink[4] = { 0.10f, 0.16f, 0.30f, 1.0f };
	struct glass_shape shape;
	const char *label;
	float color[4];
	float corner_x;
	float corner_y;
	float progress;
	float radius;
	float along;
	int32_t inwards;
	int32_t upwards;
	int32_t width;
	int32_t text_x;
	int diagonal;
	int ready;

	/* How far along the diagonal, and whether letting go now would open the panel. */
	keyboard_travel(&inwards, &upwards);
	progress = ((float)inwards + (float)upwards) * 0.5f;
	if (progress < 0.0f)
		progress = 0.0f;
	if (progress > KEYBOARD_DISTANCE)
		progress = KEYBOARD_DISTANCE;
	diagonal = keyboard_on_diagonal(inwards, upwards);
	ready = 0;
	if (diagonal && progress >= KEYBOARD_COMMIT)
		ready = 1;

	/* The disc reaches the contact: a little more than its way along the diagonal from the corner. */
	radius = KEYBOARD_HINT_RADIUS_MIN + progress * KEYBOARD_HINT_GROWTH;

	/* The corner it grows from: the flick panel's bottom right, the QWERTY panel's bottom left. */
	corner_x = 0.0f;
	if (keyboard.contact.corner == PANEL_FLICK)
		corner_x = (float)server->width;
	corner_y = (float)server->height;

	/* A soft shadow under the turned-up corner. */
	glass_shape_init(&shape, corner_x - radius, corner_y - radius, radius * 2.0f, radius * 2.0f);
	shape.quad[0] -= 40.0f;
	shape.quad[1] -= 40.0f;
	shape.quad[2] += 80.0f;
	shape.quad[3] += 80.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = radius;
	shape.soft = 18.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.26f;
	glass_shape_draw(server, command, &shape);

	/* The glass disc about the corner, white, greyer while off the diagonal. */
	glass_shape_init(&shape, corner_x - radius, corner_y - radius, radius * 2.0f, radius * 2.0f);
	shape.mode = MODE_GLASS;
	shape.radius = radius;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.78f;
	if (!diagonal) {
		shape.color[0] = 0.86f;
		shape.color[1] = 0.88f;
		shape.color[2] = 0.92f;
		shape.color[3] = 0.60f;
	}

	/* The disc's edge, drawn. */
	shape.edge = 0.85f;
	glass_shape_draw(server, command, &shape);

	/* Its rim, blue once letting go would open the panel. */
	glass_shape_init(&shape, corner_x - radius, corner_y - radius, radius * 2.0f, radius * 2.0f);
	shape.mode = MODE_RING;
	shape.radius = radius;
	shape.soft = 2.0f;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.70f;
	if (ready) {
		shape.color[0] = 0.25f;
		shape.color[1] = 0.52f;
		shape.color[2] = 0.95f;
		shape.color[3] = 0.95f;
	}
	glass_shape_draw(server, command, &shape);

	/* The keyboard's name, along the diagonal inside the disc, fading in once there is room for it. */
	if (radius < KEYBOARD_HINT_LABEL_RADIUS)
		return;
	memcpy(color, ink, sizeof(color));
	color[3] = (radius - KEYBOARD_HINT_LABEL_RADIUS) / 40.0f;
	if (color[3] > 1.0f)
		color[3] = 1.0f;
	label = kl_tr("Keyboard");
	width = glass_text_width(server, SIZE_TITLE, label);
	along = radius * 0.42f;
	text_x = (int32_t)along - width / 2;
	if (keyboard.contact.corner == PANEL_FLICK)
		text_x = (int32_t)(corner_x - along) - width / 2;
	glass_draw_text(server, command, SIZE_TITLE, text_x, (int32_t)(corner_y - along) + 6, label, width + 1, color);
}

/*
 * Draws the open panel (or, leaving, the one going back) moved towards
 * its edge by what is left of its slide: the flick panel to the right, the
 * QWERTY panel down.  The panel's rectangle is moved for the drawing and
 * put back after it, so that its keys are drawn where the panel is.
 */
static void
keyboard_draw_sliding(
	struct kwl_server *server,
	VkCommandBuffer command,
	int leaving)
{
	int32_t kept[4];
	uint64_t elapsed;
	float t;
	float out;

	/* How far the slide has come, eased out (0 at its start, 1 at its end). */
	elapsed = kwl_milliseconds() - keyboard.slide_ms;
	t = 1.0f;
	if (elapsed < KEYBOARD_SLIDE_MS)
		t = (float)elapsed / (float)KEYBOARD_SLIDE_MS;
	t = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);

	/* How much of the panel is still (or already) in its edge. */
	out = 1.0f - t;
	if (leaving)
		out = t;

	/* The panel moved by that much of its size towards its edge, drawn, and put back. */
	memcpy(kept, keyboard.panel, sizeof(kept));
	if (keyboard.open == PANEL_FLICK)
		keyboard.panel[0] += (int32_t)(out * (float)keyboard.panel[2]);
	else
		keyboard.panel[1] += (int32_t)(out * (float)keyboard.panel[3]);
	keyboard_draw_panel(server, command, keyboard.panel, 1.0f);
	memcpy(keyboard.panel, kept, sizeof(kept));
}

/* Works out the flick panel's key side for the screen's height (a share of it, within limits). */
static int
keyboard_key_size(
	struct kwl_server *server)
{
	int key;

	/* A share of the height. */
	key = (int)server->height / KEYBOARD_KEY_DIVISOR;
	if (key < KEYBOARD_KEY_MIN)
		key = KEYBOARD_KEY_MIN;
	if (key > KEYBOARD_KEY_MAX)
		key = KEYBOARD_KEY_MAX;

	/* The side. */
	return key;
}

/* Works out a key's rectangle on the open flick panel: the keys fill the bottom of its column. */
static void
keyboard_key_rect(
	struct kwl_server *server,
	unsigned row,
	unsigned column,
	int32_t *rect)
{
	int key;

	/* The key's side, and its place in the grid at the bottom of the column (the tools' area above it, §2.10). */
	key = keyboard_key_size(server);
	rect[0] = keyboard.panel[0] + KEYBOARD_KEY_GAP + (int32_t)column * (key + KEYBOARD_KEY_GAP);
	rect[1] = keyboard.panel[1] + keyboard.panel[3] - (int32_t)(KWL_FLICK_ROWS - row) * (key + KEYBOARD_KEY_GAP);
	rect[2] = key;
	rect[3] = key;
}

/* Finds the flick panel's key at a point.  Returns 1 with its row and column, or 0 (a gap, the band). */
static int
keyboard_key_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	unsigned *row,
	unsigned *column)
{
	int32_t rect[4];
	unsigned r;
	unsigned c;
	int inside;

	/* Each key of the grid. */
	for (r = 0; r < KWL_FLICK_ROWS; r++) {
		for (c = 0; c < KWL_FLICK_COLUMNS; c++) {
			/* The point on this key. */
			keyboard_key_rect(server, r, c, rect);
			inside = keyboard_contains(rect, x, y);
			if (!inside)
				continue;

			/* Succeeded: the key. */
			*row = r;
			*column = c;
			return 1;
		}
	}

	/* No key there. */
	return 0;
}

/* Works out the direction the held key's press has moved (the flick), from where it began. */
static unsigned
keyboard_key_direction(
	struct kwl_server *server)
{
	unsigned direction;
	int dx;
	int dy;
	int key;

	/* The movement from the press, and the key's side. */
	dx = keyboard.key_x - keyboard.key_start_x;
	dy = keyboard.key_y - keyboard.key_start_y;
	key = keyboard_key_size(server);

	/* The flick's direction (keyboard-layout.c). */
	direction = kwl_flick_direction(dx, dy, key);
	return direction;
}

/*
 * Acts on the release of the held key: the face key goes to the next
 * face, delete sends the delete key, the voice and case keys change the
 * last character, and any other key sends its character in the flick's
 * direction (none where the direction has none).
 */
static void
keyboard_key_release(
	struct kwl_server *server)
{
	const struct kwl_flick_key *key;
	const char *text;
	const char *shown;
	unsigned direction;
	int newline;
	int sent;

	/* The key and the flick's direction. */
	keyboard.key_active = 0;
	server->dirty = 1;
	key = kwl_flick_key(keyboard.face, keyboard.key_row, keyboard.key_column);
	if (key == NULL)
		return;
	direction = keyboard_key_direction(server);

	/* The character in that direction (a newline is logged as \n), for the log line the tests read. */
	text = kwl_flick_text(key, direction);
	shown = text;
	if (shown == NULL)
		shown = "";
	newline = strcmp(shown, "\n");
	if (newline == 0)
		shown = "\\n";
	printf("KWL OSK key face=%s row=%u column=%u dir=%s action=%u text=%s\n", kwl_flick_face_name(keyboard.face), keyboard.key_row, keyboard.key_column, kwl_flick_direction_name(direction), key->action, shown);

	/* What the key does. */
	switch (key->action) {
	case KWL_FLICK_FACE:
		/* The next face. */
		keyboard.face = kwl_flick_face_next(keyboard.face);
		printf("KWL OSK face name=%s\n", kwl_flick_face_name(keyboard.face));
		break;
	case KWL_FLICK_BACKSPACE:
		/* The delete key; the last character is gone, from the reading too. */
		sent = keyboard_send_key(server, KWL_FLICK_KEY_BACKSPACE, 0);
		if (!sent)
			break;
		keyboard_remember("", KEYBOARD_SENT_NONE);
		keyboard_reading_back(server);
		break;
	case KWL_FLICK_VOICE:
		keyboard_voice(server);
		break;
	case KWL_FLICK_CASE:
		keyboard_case(server);
		break;
	default:
		/* A character, when the direction has one. */
		if (text != NULL)
			keyboard_send(server, text);
		break;
	}
}

/* Draws the flick panel's keys: character keys pale white, the fixed keys a little darker, the held one blue. */
static void
keyboard_draw_keys(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 0.92f };
	static const float grey[4] = { 0.86f, 0.89f, 0.93f, 0.95f };
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	const struct kwl_flick_key *key;
	const float *ground;
	const float *ink;
	enum glass_size size;
	int32_t rect[4];
	unsigned characters;
	unsigned row;
	unsigned column;

	/* Each key of the face. */
	for (row = 0; row < KWL_FLICK_ROWS; row++) {
		for (column = 0; column < KWL_FLICK_COLUMNS; column++) {
			/* The key, its place, its colours: the held one blue, the acting ones grey. */
			key = kwl_flick_key(keyboard.face, row, column);
			keyboard_key_rect(server, row, column, rect);
			ground = white;
			ink = dark;
			if (key->action != KWL_FLICK_TYPE)
				ground = grey;
			if (keyboard.key_active && keyboard.key_row == row && keyboard.key_column == column) {
				ground = keyboard_blue;
				ink = keyboard_on_blue;
			}

			/* A single character large, a longer label a size smaller (the same for all of them). */
			size = SIZE_SEARCH;
			characters = keyboard_characters(key->label);
			if (characters > 1U)
				size = SIZE_SIGN;
			keyboard_draw_key(server, command, rect, key->label, ground, ink, size);
		}
	}
}

/* Draws one key: its rounded ground and its label in the middle. */
static void
keyboard_draw_key(
	struct kwl_server *server,
	VkCommandBuffer command,
	const int32_t *rect,
	const char *label,
	const float *ground,
	const float *ink,
	enum glass_size size)
{
	int32_t width;
	int32_t baseline;
	unsigned kept;

	/* A key in the accent is drawn in its colours as they are (the dark appearance's mapping would turn its ink over). */
	kept = server->keep_colours;
	if (ground == keyboard_blue)
		kept = kwl_accent_as_is(server);

	/* The ground. */
	glass_draw_solid(server, command, (float)rect[0], (float)rect[1], (float)rect[2], (float)rect[3], KEYBOARD_KEY_RADIUS, ground);

	/* The label, centred (the baseline a little under the middle). */
	width = glass_text_width(server, size, label);
	baseline = rect[1] + rect[3] / 2 + 8;
	if (size == SIZE_SIGN)
		baseline = rect[1] + rect[3] / 2 + 7;
	glass_draw_text(server, command, size, rect[0] + (rect[2] - width) / 2, baseline, label, rect[2], ink);
	kwl_accent_done(server, kept);
}

/*
 * Draws the held key's petals: its characters to the left, above, to the
 * right and below it, the one the flick points at blue (the key itself is
 * blue while the press has not moved far enough).
 */
static void
keyboard_draw_petals(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	struct glass_shape shape;
	const struct kwl_flick_key *key;
	const char *text;
	unsigned direction;
	unsigned side;
	int32_t rect[4];
	int32_t petal[4];
	int32_t size;

	/* Only a held character key. */
	if (!keyboard.key_active)
		return;
	key = kwl_flick_key(keyboard.face, keyboard.key_row, keyboard.key_column);
	if (key == NULL || key->action != KWL_FLICK_TYPE)
		return;

	/* The key, the petals' side and the direction pointed at. */
	keyboard_key_rect(server, keyboard.key_row, keyboard.key_column, rect);
	size = rect[2] * KEYBOARD_PETAL_TENTHS / 10;
	direction = keyboard_key_direction(server);

	/* Each side's petal that has a character. */
	for (side = KWL_FLICK_LEFT; side < KWL_FLICK_DIRECTIONS; side++) {
		/* A side without a character has no petal. */
		text = kwl_flick_text(key, side);
		if (text == NULL)
			continue;

		/* The petal next to the key on that side, centred on it. */
		petal[2] = size;
		petal[3] = size;
		petal[0] = rect[0] + (rect[2] - size) / 2;
		petal[1] = rect[1] + (rect[3] - size) / 2;
		if (side == KWL_FLICK_LEFT)
			petal[0] = rect[0] - size - 2;
		else if (side == KWL_FLICK_RIGHT)
			petal[0] = rect[0] + rect[2] + 2;
		else if (side == KWL_FLICK_UP)
			petal[1] = rect[1] - size - 2;
		else
			petal[1] = rect[1] + rect[3] + 2;

		/* A soft shadow under it, so it stands out from the keys it covers. */
		glass_shape_init(&shape, (float)petal[0], (float)petal[1] + 3.0f, (float)petal[2], (float)petal[3]);
		shape.quad[0] -= 16.0f;
		shape.quad[1] -= 16.0f;
		shape.quad[2] += 32.0f;
		shape.quad[3] += 32.0f;
		shape.mode = MODE_SHADOW;
		shape.radius = KEYBOARD_KEY_RADIUS;
		shape.soft = 8.0f;
		shape.color[0] = 0.10f;
		shape.color[1] = 0.18f;
		shape.color[2] = 0.35f;
		shape.color[3] = 0.30f;
		glass_shape_draw(server, command, &shape);

		/* Blue when the flick points at it. */
		if (side == direction) {
			keyboard_draw_key(server, command, petal, text, keyboard_blue, keyboard_on_blue, SIZE_SEARCH);
		} else {
			keyboard_draw_key(server, command, petal, text, white, dark, SIZE_SEARCH);
		}
	}
}

/*
 * Sends a character to the focused application: as the key of the US
 * layout that types it, else as the commit of the focused field's text
 * input.  The character is remembered for the voice and case keys.
 */
static void
keyboard_send(
	struct kwl_server *server,
	const char *text)
{
	unsigned code;
	int shift;
	int found;
	int sent;

	/* A character of the US layout: its key. */
	found = kwl_flick_us_key(text, &code, &shift);
	if (found) {
		sent = keyboard_send_key(server, code, shift);
		if (sent)
			keyboard_remember(text, KEYBOARD_SENT_KEY);
		keyboard_reading_sent(server, text, KEYBOARD_SENT_KEY);
		return;
	}

	/* Any other: the text input's commit. */
	sent = keyboard_send_commit(server, text, 0);
	if (sent)
		keyboard_remember(text, KEYBOARD_SENT_COMMIT);

	/* The reading grows by a hiragana committed, and ends at anything else. */
	if (sent) {
		keyboard_reading_sent(server, text, KEYBOARD_SENT_COMMIT);
		return;
	}

	/* A character the field did not take ends it too. */
	keyboard_reading_end(server, "refused");
}

/*
 * Sends a key's press and release to the focused application, Shift held
 * around them when asked (the physical modifiers are restored after).
 * Returns 1 when there was an application to hear it.
 */
static int
keyboard_send_key(
	struct kwl_server *server,
	unsigned code,
	int shift)
{
	uint32_t modifiers;
	uint32_t wanted;
	uint32_t used;
	uint32_t time;

	/* Without a focused application the key reaches nobody. */
	if (server->focus == NULL) {
		printf("KWL OSK refused reason=no-focus code=%u\n", code);
		return 0;
	}

	/* Shift when the key needs it, and Ctrl and Alt held for this key (they are used up). */
	modifiers = server->modifiers;
	wanted = modifiers | keyboard.held;
	if (shift)
		wanted |= KEYBOARD_SHIFT;
	used = keyboard.held;
	keyboard.held = 0;
	if (wanted != modifiers) {
		server->modifiers = wanted;
		kwl_seat_modifiers(server);
	}

	/* The press and the release, at the compositor's time. */
	time = (uint32_t)kwl_milliseconds();
	keyboard_key_event(server, time, code, 1U);
	keyboard_key_event(server, time, code, 0U);

	/* The modifiers as they were. */
	if (server->modifiers != modifiers) {
		server->modifiers = modifiers;
		kwl_seat_modifiers(server);
	}

	/* Succeeded: the key was sent. */
	printf("KWL OSK send via=key code=%u shift=%d held=%u\n", code, shift, used);
	return 1;
}

/*
 * Gives one press or release of a key the panel typed to where a key of
 * the keyboard goes.  The QWERTY panel's keys go through the input method
 * as the keyboard's do (BUG-231): while it serves the field and is not in
 * direct input, "a" becomes the preedit "あ" and Space converts.  The
 * flick panel's keys, which type their own kana, and a key the input
 * method does not take reach the focused application.
 */
static void
keyboard_key_event(
	struct kwl_server *server,
	uint32_t time,
	unsigned code,
	uint32_t state)
{
	int taken;

	/* The QWERTY panel's key: the input method's first (a release goes where its press went). */
	if (keyboard.open == PANEL_QWERTY) {
		taken = kwl_ime_key_early(server, time, code, state);
		if (taken)
			return;
		taken = kwl_ime_key_grab(server, time, code, state, 0);
		if (taken) {
			printf("KWL OSK send via=ime code=%u\n", code);
			return;
		}
	}

	/* The focused application hears it. */
	kwl_seat_key_deliver(server, time, code, state);
}

/*
 * Commits a text to the focused field's text input, deleting some bytes
 * before the cursor first (the voice key's replacement).  A field without
 * a text input, or one the input method is composing in, takes nothing.
 * Returns 1 when the text was committed.
 */
static int
keyboard_send_commit(
	struct kwl_server *server,
	const char *text,
	uint32_t before)
{
	struct kwl_text_input *input;

	/* The focused field's text input. */
	input = kwl_text_input_current(server);
	if (input == NULL) {
		printf("KWL OSK refused reason=no-text-input text=%s\n", text);
		return 0;
	}

	/* A composition of the input method is left alone (its preedit would be lost). */
	if (server->ime != NULL && server->ime->composing) {
		printf("KWL OSK refused reason=composing text=%s\n", text);
		return 0;
	}

	/* The deletion and the commit, applied together. */
	kwl_text_input_deliver(input, NULL, 0, 0, text, before, 0U);

	/* Succeeded: the field has the text. */
	printf("KWL OSK send via=commit text=%s before=%u\n", text, before);
	return 1;
}

/* Replaces the last kana sent (committed) by its next voiced, half-voiced or small form. */
static void
keyboard_voice(
	struct kwl_server *server)
{
	char next[KEYBOARD_LAST];
	int found;
	int sent;

	/* Only a kana that was committed. */
	if (keyboard.last_sent != KEYBOARD_SENT_COMMIT) {
		printf("KWL OSK voice none\n");
		return;
	}

	/* Its next form, if it has one. */
	found = kwl_flick_voice(keyboard.last, next, sizeof(next));
	if (!found) {
		printf("KWL OSK voice none\n");
		return;
	}

	/* The kana before the cursor replaced by it, in the reading too. */
	sent = keyboard_send_commit(server, next, (uint32_t)strlen(keyboard.last));
	if (sent) {
		keyboard_reading_replace(server, keyboard.last, next);
		keyboard_remember(next, KEYBOARD_SENT_COMMIT);
	}
}

/* Replaces the last letter sent (as a key) by its other case: the delete key, then the letter's key. */
static void
keyboard_case(
	struct kwl_server *server)
{
	char next[KEYBOARD_LAST];
	unsigned code;
	int shift;
	int found;
	int sent;

	/* Only a letter that was sent as a key. */
	if (keyboard.last_sent != KEYBOARD_SENT_KEY) {
		printf("KWL OSK case none\n");
		return;
	}

	/* Its other case, if it is a letter. */
	found = kwl_flick_case(keyboard.last, next, sizeof(next));
	if (!found) {
		printf("KWL OSK case none\n");
		return;
	}

	/* The letter before the cursor deleted, and the other one typed. */
	sent = keyboard_send_key(server, KWL_FLICK_KEY_BACKSPACE, 0);
	if (!sent)
		return;
	found = kwl_flick_us_key(next, &code, &shift);
	if (!found)
		return;
	sent = keyboard_send_key(server, code, shift);
	if (sent)
		keyboard_remember(next, KEYBOARD_SENT_KEY);
}

/* Remembers the last character sent and how, for the voice and case keys. */
static void
keyboard_remember(
	const char *text,
	unsigned sent)
{
	/* The character (cut to the room) and the way. */
	(void)snprintf(keyboard.last, sizeof(keyboard.last), "%s", text);
	keyboard.last_sent = sent;
}

/*
 * Tells whether the title band's drag ended far enough towards the
 * panel's edge: the flick panel's right, the QWERTY panel's bottom.
 */
static int
keyboard_band_swiped(
	int32_t x,
	int32_t y)
{
	/* The flick panel: to the right. */
	if (keyboard.open == PANEL_FLICK && x - keyboard.band_x >= KEYBOARD_SWIPE_CLOSE)
		return 1;

	/* The QWERTY panel: down. */
	if (keyboard.open == PANEL_QWERTY && y - keyboard.band_y >= KEYBOARD_SWIPE_CLOSE)
		return 1;

	/* Not far enough. */
	return 0;
}

/*
 * Works out a QWERTY key's rectangle: the panel's width is
 * KWL_QWERTY_ROW_UNITS quarter keys, a narrower row is centred, the rows
 * share the height under the title band (with room for the extra keys'
 * row above them).
 */
static void
keyboard_qwerty_rect(
	struct kwl_server *server,
	unsigned row,
	unsigned index,
	int32_t *rect)
{
	const struct kwl_qwerty_key *keys;
	unsigned count;
	unsigned units;
	unsigned before;
	unsigned rows;
	unsigned key;
	float unit;
	int32_t height;
	int32_t extra;
	int32_t left;

	/* The row's keys: the whole row's width in quarters, and the quarters before the key. */
	(void)server;
	keys = kwl_qwerty_row(keyboard.qface, row, &count);
	units = 0;
	before = 0;
	for (key = 0; key < count; key++) {
		/* The quarters before this key. */
		if (key == index)
			before = units;
		units += keys[key].width;
	}

	/* A quarter's width across the panel, and the row's left end (a narrower row centred). */
	unit = (float)(keyboard.panel[2] - KEYBOARD_KEY_GAP) / (float)KWL_QWERTY_ROW_UNITS;
	left = keyboard.panel[0] + KEYBOARD_KEY_GAP + (int32_t)((float)(KWL_QWERTY_ROW_UNITS - units) * unit / 2.0f);

	/* The rows' height under the band: the extra keys' row a share of the others'. */
	rows = KWL_QWERTY_ROWS;
	height = (int32_t)((float)(keyboard.panel[3] - KEYBOARD_BAND - (int32_t)(rows + 1U) * KEYBOARD_KEY_GAP) / ((float)(rows - 1U) + KEYBOARD_EXTRA_SHARE));
	extra = (int32_t)((float)height * KEYBOARD_EXTRA_SHARE);

	/* The key: the extra row on top, the others under it. */
	rect[0] = left + (int32_t)((float)before * unit);
	rect[1] = keyboard.panel[1] + KEYBOARD_BAND + KEYBOARD_KEY_GAP;
	rect[2] = (int32_t)((float)keys[index].width * unit) - KEYBOARD_KEY_GAP;
	rect[3] = extra;
	if (row != KWL_QWERTY_EXTRA_ROW) {
		rect[1] += extra + KEYBOARD_KEY_GAP + (int32_t)(row - 1U) * (height + KEYBOARD_KEY_GAP);
		rect[3] = height;
	}
}

/* Finds the QWERTY key at a point.  Returns 1 with its row and its place in the row, or 0. */
static int
keyboard_qwerty_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	unsigned *row,
	unsigned *index)
{
	int32_t rect[4];
	unsigned count;
	unsigned r;
	unsigned i;
	int inside;

	/* Each key of each row. */
	for (r = 0; r < KWL_QWERTY_ROWS; r++) {
		(void)kwl_qwerty_row(keyboard.qface, r, &count);
		for (i = 0; i < count; i++) {
			/* The point on this key. */
			keyboard_qwerty_rect(server, r, i, rect);
			inside = keyboard_contains(rect, x, y);
			if (!inside)
				continue;

			/* Succeeded: the key. */
			*row = r;
			*index = i;
			return 1;
		}
	}

	/* No key there. */
	return 0;
}

/* Returns a key of the QWERTY panel's face; NULL outside its row. */
static const struct kwl_qwerty_key *
keyboard_qwerty_key(
	unsigned row,
	unsigned index)
{
	const struct kwl_qwerty_key *keys;
	unsigned count;

	/* The row, and the key in it. */
	keys = kwl_qwerty_row(keyboard.qface, row, &count);
	if (keys == NULL || index >= count)
		return NULL;

	/* The key. */
	return &keys[index];
}

/*
 * Acts on the release of the held QWERTY key: a character (its Shift form
 * while Shift is on, which a single Shift gives to one character only),
 * delete, an arrow, Shift, or the other face.
 */
static void
keyboard_qwerty_release(
	struct kwl_server *server)
{
	const struct kwl_qwerty_key *key;
	const char *text;
	int sent;

	/* The key. */
	keyboard.key_active = 0;
	server->dirty = 1;
	key = keyboard_qwerty_key(keyboard.key_row, keyboard.key_column);
	if (key == NULL)
		return;

	/* The log line the tests read, with the time (the rate of typing). */
	printf("KWL OSK qkey face=%s row=%u index=%u label=%s shift=%u ms=%llu\n", kwl_qwerty_face_name(keyboard.qface), keyboard.key_row, keyboard.key_column, key->label, keyboard.shift, (unsigned long long)kwl_milliseconds());

	/* What the key does. */
	switch (key->action) {
	case KWL_FLICK_SHIFT:
		keyboard_qwerty_shift();
		break;
	case KWL_FLICK_FACE:
		/* The next face for the field (the symbols, or back from them); Shift goes. */
		keyboard.qface = kwl_qwerty_face_next(keyboard.qface, keyboard.field_qface);
		keyboard.shift = KEYBOARD_SHIFT_OFF;
		printf("KWL OSK qface name=%s\n", kwl_qwerty_face_name(keyboard.qface));
		keyboard_qwerty_log(server);
		break;
	case KWL_FLICK_BACKSPACE:
		/* The delete key; the last character is gone. */
		sent = keyboard_send_key(server, KWL_FLICK_KEY_BACKSPACE, 0);
		if (sent)
			keyboard_remember("", KEYBOARD_SENT_NONE);
		break;
	case KWL_FLICK_ARROW:
		(void)keyboard_send_key(server, key->code, 0);
		break;
	case KWL_FLICK_CTRL:
		/* Ctrl for the next key, or no more. */
		keyboard.held ^= KEYBOARD_CTRL;
		printf("KWL OSK held=%u\n", keyboard.held);
		break;
	case KWL_FLICK_ALT:
		/* Alt for the next key, or no more. */
		keyboard.held ^= KEYBOARD_ALT;
		printf("KWL OSK held=%u\n", keyboard.held);
		break;
	default:
		/* A character: its Shift form while Shift is on (a single Shift is used up). */
		text = key->text;
		if (keyboard.shift != KEYBOARD_SHIFT_OFF)
			text = key->shifted;
		if (keyboard.shift == KEYBOARD_SHIFT_ONCE)
			keyboard.shift = KEYBOARD_SHIFT_OFF;
		if (text != NULL)
			keyboard_send(server, text);
		break;
	}
}

/* Presses Shift: off to once; once to locked when pressed again soon, else off; locked to off. */
static void
keyboard_qwerty_shift(
	void)
{
	uint64_t now;

	/* The time of this press. */
	now = kwl_milliseconds();

	/* The next state. */
	if (keyboard.shift == KEYBOARD_SHIFT_OFF) {
		keyboard.shift = KEYBOARD_SHIFT_ONCE;
	} else if (keyboard.shift == KEYBOARD_SHIFT_ONCE && now - keyboard.shift_ms <= KEYBOARD_SHIFT_LOCK_MS) {
		keyboard.shift = KEYBOARD_SHIFT_LOCKED;
	} else {
		keyboard.shift = KEYBOARD_SHIFT_OFF;
	}

	/* When it was pressed, and the log line the tests read. */
	keyboard.shift_ms = now;
	printf("KWL OSK shift state=%u\n", keyboard.shift);
}

/* Logs the QWERTY panel's keys' places for its face (the tests find the keys by them). */
static void
keyboard_qwerty_log(
	struct kwl_server *server)
{
	const struct kwl_qwerty_key *keys;
	int32_t rect[4];
	unsigned count;
	unsigned row;
	unsigned index;

	/* Each key of each row. */
	for (row = 0; row < KWL_QWERTY_ROWS; row++) {
		keys = kwl_qwerty_row(keyboard.qface, row, &count);
		for (index = 0; index < count; index++) {
			keyboard_qwerty_rect(server, row, index, rect);
			printf("KWL OSK qrect face=%s row=%u index=%u x=%d y=%d width=%d height=%d label=%s\n", kwl_qwerty_face_name(keyboard.qface), row, index, rect[0], rect[1], rect[2], rect[3], keys[index].label);
		}
	}
}

/*
 * Follows the kind of the field the text input serves (q893): a field of
 * digits gets the digits' faces, an email or a web address the letters
 * with its sign, and a text field the faces the user chose; nothing served
 * counts as a text field.  A key held keeps the faces until it is let go.
 */
static void
keyboard_follow_field(
	struct kwl_server *server)
{
	struct kwl_text_input *input;
	uint32_t purpose;
	unsigned kind;

	/* A key held is looked up again on its face when it is let go. */
	if (keyboard.key_active)
		return;

	/* The served field's purpose (none is a text field's). */
	input = kwl_text_input_current(server);
	purpose = 0;
	if (input != NULL)
		purpose = input->purpose;

	/* Nothing to follow while the purpose stays. */
	if (purpose == keyboard.field_purpose)
		return;
	keyboard.field_purpose = purpose;

	/* Another purpose of the same kind keeps the faces. */
	kind = kwl_field_kind(purpose);
	if (kind == keyboard.field_kind)
		return;

	/* The flick face the user had in a text field is kept for the next one. */
	if (keyboard.field_kind == KWL_FIELD_TEXT)
		keyboard.flick_chosen = keyboard.face;
	keyboard.field_kind = kind;

	/* The faces for the kind; Shift goes. */
	keyboard.face = kwl_field_flick_face(kind, keyboard.flick_chosen);
	keyboard.field_qface = kwl_field_qwerty_face(kind);
	keyboard.qface = keyboard.field_qface;
	keyboard.shift = KEYBOARD_SHIFT_OFF;
	server->dirty = 1;

	/* The log line the tests read, and the QWERTY keys' new places while that panel shows. */
	printf("KWL OSK field purpose=%u kind=%s face=%s qface=%s\n", (unsigned)purpose, kwl_field_kind_name(kind),
	    kwl_flick_face_name(keyboard.face), kwl_qwerty_face_name(keyboard.qface));
	if (keyboard.open == PANEL_QWERTY)
		keyboard_qwerty_log(server);
}

/* Draws the QWERTY panel's keys: typing keys white, acting keys grey, Shift blue while on, the held key blue. */
static void
keyboard_draw_qwerty(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 0.92f };
	static const float grey[4] = { 0.86f, 0.89f, 0.93f, 0.95f };
	static const float pale[4] = { 0.78f, 0.86f, 0.99f, 1.0f };
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	const struct kwl_qwerty_key *keys;
	const float *ground;
	const float *ink;
	const char *label;
	enum glass_size size;
	int32_t rect[4];
	unsigned characters;
	unsigned count;
	unsigned row;
	unsigned index;

	/* Each key of each row. */
	for (row = 0; row < KWL_QWERTY_ROWS; row++) {
		keys = kwl_qwerty_row(keyboard.qface, row, &count);
		for (index = 0; index < count; index++) {
			/* The key's place, colours and label (the Shift form while Shift is on). */
			keyboard_qwerty_rect(server, row, index, rect);
			ground = white;
			ink = dark;
			label = keys[index].label;
			if (keyboard.shift != KEYBOARD_SHIFT_OFF)
				label = keys[index].shifted_label;
			if (keys[index].action != KWL_FLICK_TYPE)
				ground = grey;
			if (keys[index].action == KWL_FLICK_SHIFT && keyboard.shift == KEYBOARD_SHIFT_ONCE)
				ground = pale;
			if (keys[index].action == KWL_FLICK_CTRL && (keyboard.held & KEYBOARD_CTRL) != 0U)
				ground = pale;
			if (keys[index].action == KWL_FLICK_ALT && (keyboard.held & KEYBOARD_ALT) != 0U)
				ground = pale;
			if (keys[index].action == KWL_FLICK_SHIFT && keyboard.shift == KEYBOARD_SHIFT_LOCKED) {
				ground = keyboard_blue;
				ink = keyboard_on_blue;
			}

			/* The held key is blue. */
			if (keyboard.key_active && keyboard.key_row == row && keyboard.key_column == index) {
				ground = keyboard_blue;
				ink = keyboard_on_blue;
			}

			/* A single character large, a word a size smaller. */
			size = SIZE_SEARCH;
			characters = keyboard_characters(label);
			if (characters > 1U)
				size = SIZE_SIGN;
			keyboard_draw_key(server, command, rect, label, ground, ink, size);
		}
	}
}

/* Draws the bubble over the held QWERTY key that types: its character, larger, above it. */
static void
keyboard_draw_bubble(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	const struct kwl_qwerty_key *key;
	struct glass_shape shape;
	const char *label;
	int32_t rect[4];
	int32_t bubble[4];

	/* Only a held key that types a character. */
	if (!keyboard.key_active)
		return;
	key = keyboard_qwerty_key(keyboard.key_row, keyboard.key_column);
	if (key == NULL || key->action != KWL_FLICK_TYPE)
		return;

	/* Above the key, a little wider, as high as it. */
	keyboard_qwerty_rect(server, keyboard.key_row, keyboard.key_column, rect);
	bubble[2] = rect[2] + 16;
	bubble[3] = rect[3] + 12;
	bubble[0] = rect[0] - 8;
	bubble[1] = rect[1] - bubble[3] - 4;

	/* Its shadow, and the bubble with the character. */
	glass_shape_init(&shape, (float)bubble[0], (float)bubble[1] + 3.0f, (float)bubble[2], (float)bubble[3]);
	shape.quad[0] -= 16.0f;
	shape.quad[1] -= 16.0f;
	shape.quad[2] += 32.0f;
	shape.quad[3] += 32.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = KEYBOARD_KEY_RADIUS;
	shape.soft = 8.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.30f;
	glass_shape_draw(server, command, &shape);
	label = key->label;
	if (keyboard.shift != KEYBOARD_SHIFT_OFF)
		label = key->shifted_label;
	keyboard_draw_key(server, command, bubble, label, white, dark, SIZE_SEARCH);
}

/* Works out the QWERTY panel's band button's rectangle, left of the close key. */
static void
keyboard_band_button_rect(
	int32_t *rect)
{
	int32_t close[4];

	/* As high as the close key, KEYBOARD_BAND_BUTTON wide, a margin to its left. */
	keyboard_close_rect(close);
	rect[2] = KEYBOARD_BAND_BUTTON;
	rect[3] = close[3];
	rect[0] = close[0] - KEYBOARD_MARGIN - rect[2];
	rect[1] = close[1];
}

/* Works out the handwriting face's writing area: the panel under its band, but the side column. */
static void
keyboard_hand_area(
	int32_t *rect)
{
	/* From the left, under the band, to the side column. */
	rect[0] = keyboard.panel[0] + KEYBOARD_KEY_GAP;
	rect[1] = keyboard.panel[1] + KEYBOARD_BAND + KEYBOARD_KEY_GAP;
	rect[2] = keyboard.panel[2] - 3 * KEYBOARD_KEY_GAP - KEYBOARD_HAND_SIDE;
	rect[3] = keyboard.panel[3] - KEYBOARD_BAND - 2 * KEYBOARD_KEY_GAP;
}

/*
 * Works out a key of the handwriting face's side column: the note's line
 * on top, then a row of the three candidates, a row of clear and delete, a
 * row of space and enter.
 */
static void
keyboard_hand_key_rect(
	unsigned key,
	int32_t *rect)
{
	int32_t area[4];
	int32_t left;
	int32_t top;
	int32_t height;
	int32_t width;

	/* The side column, and its rows' height under the note. */
	keyboard_hand_area(area);
	left = area[0] + area[2] + KEYBOARD_KEY_GAP;
	top = area[1] + KEYBOARD_HAND_NOTE + KEYBOARD_KEY_GAP;
	height = (area[3] - KEYBOARD_HAND_NOTE - 3 * KEYBOARD_KEY_GAP) / 3;

	/* The candidates: a third of the column each. */
	if (key < KEYBOARD_HAND_CLEAR) {
		width = (KEYBOARD_HAND_SIDE - 2 * KEYBOARD_KEY_GAP) / 3;
		rect[0] = left + (int32_t)key * (width + KEYBOARD_KEY_GAP);
		rect[1] = top;
		rect[2] = width;
		rect[3] = height;
		return;
	}

	/* The other keys: half the column each, two rows. */
	width = (KEYBOARD_HAND_SIDE - KEYBOARD_KEY_GAP) / 2;
	rect[0] = left + (int32_t)((key - KEYBOARD_HAND_CLEAR) % 2U) * (width + KEYBOARD_KEY_GAP);
	rect[1] = top + (int32_t)(1U + (key - KEYBOARD_HAND_CLEAR) / 2U) * (height + KEYBOARD_KEY_GAP);
	rect[2] = width;
	rect[3] = height;
}

/* Finds the handwriting face's key at a point.  Returns 1 with the key, or 0. */
static int
keyboard_hand_key_at(
	int32_t x,
	int32_t y,
	unsigned *key)
{
	int32_t rect[4];
	unsigned index;
	int inside;

	/* Each key of the side column. */
	for (index = 0; index < KEYBOARD_HAND_KEYS; index++) {
		/* The point on this key. */
		keyboard_hand_key_rect(index, rect);
		inside = keyboard_contains(rect, x, y);
		if (!inside)
			continue;

		/* Succeeded: the key. */
		*key = index;
		return 1;
	}

	/* No key there. */
	return 0;
}

/* Changes the QWERTY panel to the handwriting face or back; the ink and its answer go. */
static void
keyboard_hand_toggle(
	struct kwl_server *server)
{
	int32_t area[4];

	/* The other face, with no ink. */
	keyboard.hand = !keyboard.hand;
	keyboard.writing = 0;
	keyboard.recognized = 0;
	kwl_hand_clear(&keyboard_ink);
	memset(&keyboard.result, 0, sizeof(keyboard.result));
	server->dirty = 1;

	/* The log line the tests read, with the writing area's place; the templates are read ahead meanwhile. */
	if (keyboard.hand) {
		kwl_hand_preload(&keyboard.result);
		keyboard_hand_area(area);
		printf("KWL OSK hand on area=%d,%d,%d,%d\n", area[0], area[1], area[2], area[3]);
		return;
	}

	/* Back to the keys. */
	printf("KWL OSK hand off\n");
}

/*
 * Adds a point of the pointer to the ink (begin: the first of a stroke),
 * kept within the writing area; the point waits for the frame that draws
 * it, whose lag is logged.
 */
static void
keyboard_hand_point(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	int begin)
{
	int32_t area[4];
	int kept;

	/* The point, kept within the writing area. */
	keyboard_hand_area(area);
	if (x < area[0])
		x = area[0];
	if (x >= area[0] + area[2])
		x = area[0] + area[2] - 1;
	if (y < area[1])
		y = area[1];
	if (y >= area[1] + area[3])
		y = area[1] + area[3] - 1;

	/* The first point of a stroke, or the next one. */
	if (begin) {
		kept = kwl_hand_begin(&keyboard_ink, x, y);
		keyboard.writing = 1;
		keyboard.recognized = 0;
	} else {
		kept = kwl_hand_add(&keyboard_ink, x, y);
	}

	/* A point not kept (no move, no room) draws nothing new. */
	if (!kept)
		return;

	/* A kept point is drawn in the next frame; its input time is kept for the lag (the oldest waiting). */
	if (!keyboard.lag_pending) {
		keyboard.lag_pending = 1;
		keyboard.lag_input_ms = kwl_milliseconds();
	}

	/* The frame. */
	server->dirty = 1;
}

/* Ends the stroke being written; the ink is recognized after a wait. */
static void
keyboard_hand_release(
	struct kwl_server *server)
{
	/* The stroke is over, and when. */
	keyboard.writing = 0;
	keyboard.stroke_end_ms = kwl_milliseconds();
	server->dirty = 1;
	printf("KWL OSK hand stroke-end strokes=%u points=%u\n", keyboard_ink.count, kwl_hand_points(&keyboard_ink));
}

/*
 * Acts on the release of a key of the handwriting face: a candidate is
 * sent and the ink cleared; clear clears it; delete, space and enter are
 * sent as keys.
 */
static void
keyboard_hand_key_release(
	struct kwl_server *server)
{
	unsigned key;
	int sent;

	/* The key. */
	keyboard.hand_key_active = 0;
	key = keyboard.hand_key;
	server->dirty = 1;
	printf("KWL OSK hand key=%u\n", key);

	/* A candidate there is: sent, and the ink and the answer go. */
	if (key < KEYBOARD_HAND_CLEAR) {
		if (key >= keyboard.result.count)
			return;
		keyboard_send(server, keyboard.result.candidates[key]);
		kwl_hand_clear(&keyboard_ink);
		keyboard.recognized = 0;
		memset(&keyboard.result, 0, sizeof(keyboard.result));
		return;
	}

	/* The other keys. */
	switch (key) {
	case KEYBOARD_HAND_CLEAR:
		/* The ink and the answer go. */
		kwl_hand_clear(&keyboard_ink);
		keyboard.recognized = 0;
		memset(&keyboard.result, 0, sizeof(keyboard.result));
		printf("KWL OSK hand clear\n");
		break;
	case KEYBOARD_HAND_DELETE:
		sent = keyboard_send_key(server, KWL_FLICK_KEY_BACKSPACE, 0);
		if (sent)
			keyboard_remember("", KEYBOARD_SENT_NONE);
		break;
	case KEYBOARD_HAND_SPACE:
		keyboard_send(server, " ");
		break;
	default:
		keyboard_send(server, "\n");
		break;
	}
}

/* Recognizes the ink (keyboard-hand.c) and shows the candidates. */
static void
keyboard_hand_recognize(
	struct kwl_server *server)
{
	const char *first;
	int32_t bounds[4];
	int32_t area[4];

	/* The answer, once for this ink, by its shape, size and place on the writing area. */
	keyboard_hand_area(area);
	kwl_hand_recognize_on(&keyboard_ink, area[1], area[3], &keyboard.result);
	keyboard.recognized = 1;
	server->dirty = 1;

	/* The log line the tests read: the ink measured, and the answer (its first candidate). */
	kwl_hand_bounds(&keyboard_ink, bounds);
	first = "";
	if (keyboard.result.count > 0U)
		first = keyboard.result.candidates[0];
	printf("KWL OSK hand recognize strokes=%u points=%u box=%d,%d,%d,%d candidates=%u first=%s note=%s\n", keyboard_ink.count, kwl_hand_points(&keyboard_ink), bounds[0], bounds[1], bounds[2], bounds[3], keyboard.result.count, first, keyboard.result.note);
}

/*
 * Draws the handwriting face: the writing area with a faint cross in its
 * middle, the ink's strokes as rows of round dots, the note, the
 * candidates and the keys.  A frame that draws a waiting point logs its
 * lag behind the input.
 */
static void
keyboard_draw_hand(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float paper[4] = { 1.0f, 1.0f, 1.0f, 0.80f };
	static const float guide[4] = { 0.12f, 0.16f, 0.24f, 0.10f };
	static const float ink[4] = { 0.10f, 0.20f, 0.45f, 1.0f };
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 0.92f };
	static const float grey[4] = { 0.86f, 0.89f, 0.93f, 0.95f };
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float soft[4] = { 0.34f, 0.38f, 0.46f, 1.0f };
	static const char *const labels[KEYBOARD_HAND_KEYS] = { "", "", "", "消す", "Del", "space", "Enter" };
	const struct kwl_hand_stroke *stroke;
	const float *ground;
	const float *text_ink;
	const char *label;
	int32_t area[4];
	int32_t rect[4];
	uint64_t now;
	unsigned index;
	unsigned point;
	float dx;
	float dy;
	float length;
	float step;
	float x;
	float y;

	/* The writing area and its cross. */
	keyboard_hand_area(area);
	glass_draw_solid(server, command, (float)area[0], (float)area[1], (float)area[2], (float)area[3], KEYBOARD_KEY_RADIUS, paper);
	glass_draw_solid(server, command, (float)(area[0] + area[2] / 2), (float)area[1] + 8.0f, 1.0f, (float)area[3] - 16.0f, 0.0f, guide);
	glass_draw_solid(server, command, (float)area[0] + 8.0f, (float)(area[1] + area[3] / 2), (float)area[2] - 16.0f, 1.0f, 0.0f, guide);

	/* Each stroke: dots along each of its segments. */
	for (index = 0; index < keyboard_ink.count; index++) {
		stroke = &keyboard_ink.strokes[index];
		for (point = 0; point < stroke->count; point++) {
			/* The first point is a dot of its own. */
			x = (float)stroke->points[point].x;
			y = (float)stroke->points[point].y;
			if (point == 0U) {
				glass_draw_solid(server, command, x - KEYBOARD_INK_DOT / 2.0f, y - KEYBOARD_INK_DOT / 2.0f, KEYBOARD_INK_DOT, KEYBOARD_INK_DOT, KEYBOARD_INK_DOT / 2.0f, ink);
				continue;
			}

			/* The segment from the point before: a dot every KEYBOARD_INK_STEP. */
			dx = x - (float)stroke->points[point - 1U].x;
			dy = y - (float)stroke->points[point - 1U].y;
			length = sqrtf(dx * dx + dy * dy);
			for (step = KEYBOARD_INK_STEP; step < length + KEYBOARD_INK_STEP; step += KEYBOARD_INK_STEP) {
				if (step > length)
					step = length;
				glass_draw_solid(server, command, x - dx + dx * step / length - KEYBOARD_INK_DOT / 2.0f, y - dy + dy * step / length - KEYBOARD_INK_DOT / 2.0f, KEYBOARD_INK_DOT, KEYBOARD_INK_DOT, KEYBOARD_INK_DOT / 2.0f, ink);
			}
		}
	}

	/* The note over the candidates. */
	keyboard_hand_key_rect(0U, rect);
	glass_draw_text(server, command, SIZE_BAR, rect[0], rect[1] - KEYBOARD_KEY_GAP - 6, keyboard.result.note, KEYBOARD_HAND_SIDE, soft);

	/* The keys: the candidates (empty without an answer), clear, delete, space, enter; the held one blue. */
	for (index = 0; index < KEYBOARD_HAND_KEYS; index++) {
		keyboard_hand_key_rect(index, rect);
		label = labels[index];
		ground = grey;
		text_ink = dark;
		if (index < KEYBOARD_HAND_CLEAR) {
			ground = white;
			label = "";
			if (index < keyboard.result.count)
				label = keyboard.result.candidates[index];
		}

		/* The held key is blue. */
		if (keyboard.hand_key_active && keyboard.hand_key == index) {
			ground = keyboard_blue;
			text_ink = keyboard_on_blue;
		}

		/* The key. */
		keyboard_draw_key(server, command, rect, label, ground, text_ink, SIZE_SIGN);
	}

	/* A frame that draws a waiting point: its lag behind the input, and the time since the last such frame. */
	if (!keyboard.lag_pending)
		return;
	now = kwl_milliseconds();
	printf("KWL OSK hand frame lag_ms=%llu gap_ms=%llu points=%u\n", (unsigned long long)(now - keyboard.lag_input_ms), (unsigned long long)(now - keyboard.lag_frame_ms), kwl_hand_points(&keyboard_ink));
	keyboard.lag_pending = 0;
	keyboard.lag_frame_ms = now;
}

/* Tells how far the panel's slide has come, eased out: 0 at its start, 1 at its end and after. */
static float
keyboard_slide(void)
{
	uint64_t elapsed;
	float t;

	/* The time since the slide began, a share of its length. */
	elapsed = kwl_milliseconds() - keyboard.slide_ms;
	if (elapsed >= KEYBOARD_SLIDE_MS)
		return 1.0f;
	t = (float)elapsed / (float)KEYBOARD_SLIDE_MS;

	/* Eased out: quick at first, slow at the end. */
	return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

/*
 * Works out the work area for the panel open now (or none), and has the
 * windows follow it: each docked window told its new size, each floating
 * window that would reach under the panel moved into the area, and those
 * moved before moved back when the panel is gone.
 */
static void
keyboard_work_area(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	int32_t right;
	int32_t bottom;
	int window;

	/* What the panel takes: its column or row, or nothing. */
	right = 0;
	bottom = 0;
	if (keyboard.open == PANEL_FLICK)
		right = keyboard.panel[2];
	if (keyboard.open == PANEL_QWERTY)
		bottom = keyboard.panel[3];

	/* An unchanged area moves nothing. */
	if (right == keyboard.reserved_right && bottom == keyboard.reserved_bottom)
		return;
	keyboard.reserved_right = right;
	keyboard.reserved_bottom = bottom;
	keyboard.moving = 1;
	printf("KWL OSK work-area right=%d bottom=%d\n", right, bottom);

	/* Without a panel, the windows moved before go back. */
	if (right == 0 && bottom == 0)
		keyboard_move_back(server);

	/* Each window's end: a docked one's new size, a floating one's place in the area when it reaches under the panel. */
	for (client = server->clients; client != NULL; client = client->next) {
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only windows; a fullscreen one keeps its size (the panel is over it). */
			window = keyboard_is_window(surface);
			if (!window || surface->fullscreen)
				continue;

			/* A docked window: the docked size (KWL_GLASS_DOCK_PAD in from every side) less the panel (or whole again). */
			if (surface->maximized) {
				surface->window_width = server->width - (uint32_t)right - 2U * KWL_GLASS_DOCK_PAD;
				surface->window_height = server->height - KWL_GLASS_DOCK_TOP - (uint32_t)bottom - KWL_GLASS_DOCK_PAD;
				continue;
			}

			/* A floating window, moved in when it must be (not when the panel went). */
			if (right != 0 || bottom != 0)
				keyboard_fit_floating(server, surface);
		}
	}

	/*
	 * The windows with an inset are told what the panel covers of them at
	 * their ends (inset.c), before their configures: the moved windows are
	 * put at their ends for it, and back where their moves start after.
	 */
	keyboard_moves_at_end(server, 1);
	if (right != 0 || bottom != 0) {
		kwl_keyboard_inset_notify(server, keyboard.panel);
	} else {
		kwl_keyboard_inset_notify(server, NULL);
	}

	/* The moved windows back where their moves start. */
	keyboard_moves_at_end(server, 0);

	/* Each docked window told its new size, once. */
	for (client = server->clients; client != NULL; client = client->next) {
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only docked windows (not fullscreen). */
			window = keyboard_is_window(surface);
			if (!window || surface->fullscreen || !surface->maximized)
				continue;

			/* The configure, and the log line the tests read. */
			(void)kwl_window_send_configure(surface);
			printf("KWL OSK work docked surface=%u width=%u height=%u\n", surface->id, surface->window_width, surface->window_height);
		}
	}
}

/*
 * Puts the moved windows at the ends of their moves (end), or back where
 * their moves start (the slide then moves them), for what the insets are
 * told.
 */
static void
keyboard_moves_at_end(
	struct kwl_server *server,
	int end)
{
	struct keyboard_move *move;
	unsigned index;
	int found;

	/* Each move's window (still one) at one of its places. */
	for (index = 0; index < keyboard.move_count; index++) {
		move = &keyboard.moves[index];
		found = keyboard_window_live(server, move->surface);
		if (!found)
			continue;
		if (end) {
			move->surface->x = move->to_x;
			move->surface->y = move->to_y;
		} else {
			move->surface->x = move->from_x;
			move->surface->y = move->from_y;
		}
	}
}

/*
 * Tells whether a window remembered by its address is still one of the
 * live windows (the address is only compared, never followed, until it is
 * found among them).
 */
static int
keyboard_window_live(
	struct kwl_server *server,
	const struct kwl_object *window)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	int alive;

	/* Each client's objects. */
	for (client = server->clients; client != NULL; client = client->next) {
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Another object. */
			if (surface != window)
				continue;

			/* The object; it must still be a window. */
			alive = keyboard_is_window(surface);
			return alive;
		}
	}

	/* Not among them. */
	return 0;
}

/* Tells whether an object is a window (a mapped toplevel's surface, not the desktop's). */
static int
keyboard_is_window(
	const struct kwl_object *surface)
{
	int desktop;

	/* Only a live, mapped surface with a toplevel. */
	if (surface->kind != KWL_SURFACE || surface->dead || !surface->mapped)
		return 0;
	if (surface->role == NULL || surface->role->top == NULL)
		return 0;

	/* The desktop's icons are not a window. */
	desktop = kwl_desktop_is(surface);
	if (desktop)
		return 0;

	/* A window. */
	return 1;
}

/*
 * Moves a floating window into the work area when it reaches under the
 * panel: its right and bottom edges inside the area, but its title bar
 * never above the area's top nor its left edge left of the area (a window
 * too large overhangs under the panel).  The move follows the slide.
 */
static void
keyboard_fit_floating(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	struct keyboard_move *move;
	uint32_t image_width;
	uint32_t image_height;
	unsigned index;
	int32_t right_edge;
	int32_t bottom_edge;
	int32_t width;
	int32_t height;
	int32_t x;
	int32_t y;

	/* The window's size (its image's), and the area's right and bottom edges for a body. */
	if (surface->current == NULL)
		return;
	kwl_surface_size(surface, &image_width, &image_height);
	width = (int32_t)image_width;
	height = (int32_t)image_height;
	if (width <= 0 || height <= 0)
		return;
	right_edge = (int32_t)server->width - KWL_GLASS_MARGIN - keyboard.reserved_right;
	bottom_edge = (int32_t)server->height - KWL_GLASS_MARGIN - keyboard.reserved_bottom;

	/*
	 * A window the work area moved before (another panel was out), and
	 * not moved by the user since, is placed from where it was at first,
	 * so that closing brings it back there (ws102-p016, p007's limit).
	 */
	move = NULL;
	for (index = 0; index < keyboard.move_count; index++) {
		if (keyboard.moves[index].surface == surface)
			move = &keyboard.moves[index];
	}

	/* A window the user moved since is placed afresh (its old move forgotten). */
	if (move != NULL && (surface->x != move->to_x || surface->y != move->to_y)) {
		move->surface = NULL;
		move = NULL;
	}

	/* The place inside them, not above nor left of the area, from its first place. */
	x = surface->x;
	y = surface->y;
	if (move != NULL) {
		x = move->home_x;
		y = move->home_y;
	}

	/* Its right and bottom edges inside the area, never above nor left of it. */
	if (x + width > right_edge)
		x = right_edge - width;
	if (y + height > bottom_edge)
		y = bottom_edge - height;
	if (x < KWL_GLASS_MARGIN)
		x = KWL_GLASS_MARGIN;
	if (y < KWL_GLASS_TOP)
		y = KWL_GLASS_TOP;

	/* A window moved before goes on from where it is to the new place (back to its first one if that fits now). */
	if (move != NULL) {
		move->from_x = surface->x;
		move->from_y = surface->y;
		move->to_x = x;
		move->to_y = y;
		move->back = 0;
		if (x == move->home_x && y == move->home_y)
			move->back = 1;
		printf("KWL OSK work moved surface=%u from=%d,%d to=%d,%d\n", surface->id, surface->x, surface->y, x, y);
		return;
	}

	/* A window already inside stays. */
	if (x == surface->x && y == surface->y)
		return;

	/* No room to remember another move: it stays. */
	if (keyboard.move_count >= KEYBOARD_MOVES)
		return;

	/* The move, from where it is to the place inside, remembered to go back. */
	move = &keyboard.moves[keyboard.move_count];
	keyboard.move_count++;
	move->surface = surface;
	move->home_x = surface->x;
	move->home_y = surface->y;
	move->from_x = surface->x;
	move->from_y = surface->y;
	move->to_x = x;
	move->to_y = y;
	move->back = 0;
	printf("KWL OSK work moved surface=%u from=%d,%d to=%d,%d\n", surface->id, surface->x, surface->y, x, y);
}

/*
 * Turns the moves back: each window the work area moved goes back to
 * where it was, unless it was moved or resized since (it is then left
 * where it is, and forgotten).
 */
static void
keyboard_move_back(
	struct kwl_server *server)
{
	struct keyboard_move *move;
	unsigned index;
	unsigned kept;
	int found;

	/* Each move. */
	kept = 0;
	for (index = 0; index < keyboard.move_count; index++) {
		/* The window must still be one (a closed window is forgotten). */
		move = &keyboard.moves[index];
		found = keyboard_window_live(server, move->surface);
		if (!found)
			continue;

		/* A window moved by the user since (not where the move left it) stays where it is. */
		if (move->surface->x != move->to_x || move->surface->y != move->to_y || move->surface->maximized) {
			printf("KWL OSK work kept surface=%u\n", move->surface->id);
			continue;
		}

		/* Back where it was, following the slide. */
		move->from_x = move->to_x;
		move->from_y = move->to_y;
		move->to_x = move->home_x;
		move->to_y = move->home_y;
		move->back = 1;
		keyboard.moves[kept] = *move;
		kept++;
		printf("KWL OSK work back surface=%u to=%d,%d\n", move->surface->id, move->home_x, move->home_y);
	}

	/* The moves still to be drawn. */
	keyboard.move_count = kept;
}

/*
 * Places the moved windows a share of the way along their moves (t, the
 * slide's).  At the end, the moves back are forgotten; the moves in stay
 * remembered for going back.
 */
static void
keyboard_move_step(
	struct kwl_server *server,
	float t)
{
	struct keyboard_move *move;
	unsigned index;
	unsigned kept;
	int found;

	/* Each move, its window between its two places (its end past the slide); a window gone is forgotten. */
	kept = 0;
	for (index = 0; index < keyboard.move_count; index++) {
		move = &keyboard.moves[index];
		found = keyboard_window_live(server, move->surface);
		if (!found)
			continue;
		move->surface->x = move->from_x + (int32_t)((float)(move->to_x - move->from_x) * t);
		move->surface->y = move->from_y + (int32_t)((float)(move->to_y - move->from_y) * t);
		server->dirty = 1;

		/* A move back that is over is forgotten; one in stays for going back. */
		if (t >= 1.0f && move->back)
			continue;
		if (t >= 1.0f) {
			move->from_x = move->to_x;
			move->from_y = move->to_y;
		}

		/* The move is kept. */
		keyboard.moves[kept] = *move;
		kept++;
	}

	/* The moves left. */
	keyboard.move_count = kept;
}

/*
 * Works out a tool's rectangle: the tools fill the flick panel's column
 * under its title band, down to its keys; a row has as many columns as its
 * tools say, the tabs' row is lower than the others.
 */
static void
keyboard_tool_rect(
	struct kwl_server *server,
	unsigned index,
	int32_t *rect)
{
	const struct keyboard_tool *tool;
	int32_t width;
	int32_t column;
	int32_t top;

	/* The tool, and the column's width for its row's columns. */
	(void)server;
	tool = &keyboard_tools[index];
	width = (keyboard.panel[2] - (int32_t)(tool->columns + 1U) * KEYBOARD_KEY_GAP) / (int32_t)tool->columns;
	column = width + KEYBOARD_KEY_GAP;

	/* Its row's top: the row always there, the tabs, then the edit tools' rows. */
	top = keyboard.panel[1] + KEYBOARD_BAND + KEYBOARD_KEY_GAP;
	if (tool->row >= 1U)
		top += KEYBOARD_TOOL_ROW + KEYBOARD_KEY_GAP;
	if (tool->row >= 2U)
		top += KEYBOARD_TOOL_TABS + KEYBOARD_KEY_GAP + (int32_t)(tool->row - 2U) * (KEYBOARD_TOOL_ROW + KEYBOARD_KEY_GAP);

	/* The rectangle, over its columns. */
	rect[0] = keyboard.panel[0] + KEYBOARD_KEY_GAP + (int32_t)tool->column * column;
	rect[1] = top;
	rect[2] = (int32_t)tool->span * column - KEYBOARD_KEY_GAP;
	rect[3] = KEYBOARD_TOOL_ROW;
	if (tool->row == 1U)
		rect[3] = KEYBOARD_TOOL_TABS;
}

/* Finds the flick panel's tool at a point.  Returns 1 with its index, or 0. */
static int
keyboard_tool_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	unsigned *index)
{
	int32_t rect[4];
	unsigned tool;
	int inside;

	/* Each tool (the edit tools only on their face). */
	for (tool = 0; tool < sizeof(keyboard_tools) / sizeof(keyboard_tools[0]); tool++) {
		/* A tool of another face is not there. */
		if (keyboard_tools[tool].row >= 2U && keyboard.tools_face != KEYBOARD_FACE_EDIT)
			continue;

		/* The point on this tool. */
		keyboard_tool_rect(server, tool, rect);
		inside = keyboard_contains(rect, x, y);
		if (!inside)
			continue;

		/* Succeeded: the tool. */
		*index = tool;
		return 1;
	}

	/* No tool there. */
	return 0;
}

/*
 * Tells whether a tool can do something now: a tab always; an edit
 * operation only when the focused window can do it
 * (enabled, the bits kwl_edit_state gave; state is its answer, -1 without
 * a focused window).
 */
static int
keyboard_tool_enabled(
	struct kwl_server *server,
	unsigned index,
	uint32_t enabled,
	int state)
{
	unsigned action;

	/* The tabs can always be chosen. */
	(void)server;
	switch (keyboard_tools[index].kind) {
	case TOOL_TAB_CANDIDATES:
	case TOOL_TAB_HISTORY:
	case TOOL_TAB_EMOJI:
		return 1;
	case TOOL_UNDO:
		action = KWL_EDIT_UNDO;
		break;
	case TOOL_REDO:
		action = KWL_EDIT_REDO;
		break;
	case TOOL_COPY:
		action = KWL_EDIT_COPY;
		break;
	case TOOL_CUT:
		action = KWL_EDIT_CUT;
		break;
	case TOOL_PASTE:
		action = KWL_EDIT_PASTE;
		break;
	case TOOL_SELECT_ALL:
		action = KWL_EDIT_SELECT_ALL;
		break;
	default:
		return 1;
	}

	/* Without a focused window no operation can be done. */
	if (state < 0)
		return 0;

	/* The operation, when the window can do it now. */
	if ((enabled & (1U << action)) != 0U)
		return 1;
	return 0;
}

/*
 * Acts on the release of the held tool: the application before comes up,
 * delete is sent, the movements are sent as keys (with Shift while
 * selecting), selecting turns on or off, the edit operations go to the
 * focused window (copy and cut end the selecting).
 */
static void
keyboard_tool_release(
	struct kwl_server *server)
{
	const struct keyboard_tool *tool;
	int error;
	int sent;

	/* The tool. */
	keyboard.tool_active = 0;
	server->dirty = 1;
	tool = &keyboard_tools[keyboard.tool];
	printf("KWL OSK tool label=%s\n", tool->label);

	/* A tool that is not a tab or delete ends the reading (the cursor or the text may move). */
	if (tool->row != 1U && tool->kind != TOOL_DELETE)
		keyboard_reading_end(server, "tool");

	/* What it does. */
	switch (tool->kind) {
	case TOOL_PREVIOUS:
		error = kwl_focus_previous(server);
		printf("KWL OSK tool previous error=%d\n", error);
		break;
	case TOOL_DELETE:
		sent = keyboard_send_key(server, KWL_FLICK_KEY_BACKSPACE, 0);
		if (sent)
			keyboard_reading_back(server);
		break;
	case TOOL_LEFT:
		keyboard_tool_move(server, KWL_KEY_LEFT);
		break;
	case TOOL_UP:
		keyboard_tool_move(server, KWL_KEY_UP);
		break;
	case TOOL_DOWN:
		keyboard_tool_move(server, KWL_KEY_DOWN);
		break;
	case TOOL_RIGHT:
		keyboard_tool_move(server, KWL_KEY_RIGHT);
		break;
	case TOOL_LINE_START:
		keyboard_tool_move(server, KWL_KEY_HOME);
		break;
	case TOOL_LINE_END:
		keyboard_tool_move(server, KWL_KEY_END);
		break;
	case TOOL_PAGE_UP:
		keyboard_tool_move(server, KWL_KEY_PAGE_UP);
		break;
	case TOOL_PAGE_DOWN:
		keyboard_tool_move(server, KWL_KEY_PAGE_DOWN);
		break;
	case TOOL_SELECT:
		/* Selecting on or off: the movements go with Shift while it is on. */
		keyboard.selecting = !keyboard.selecting;
		printf("KWL OSK tool selecting=%u\n", keyboard.selecting);
		break;
	case TOOL_SELECT_ALL:
		keyboard_tool_edit(server, KWL_EDIT_SELECT_ALL);
		break;
	case TOOL_UNDO:
		keyboard_tool_edit(server, KWL_EDIT_UNDO);
		break;
	case TOOL_REDO:
		keyboard_tool_edit(server, KWL_EDIT_REDO);
		break;
	case TOOL_COPY:
		keyboard.selecting = 0;
		keyboard_tool_edit(server, KWL_EDIT_COPY);
		break;
	case TOOL_CUT:
		keyboard.selecting = 0;
		keyboard_tool_edit(server, KWL_EDIT_CUT);
		break;
	case TOOL_PASTE:
		keyboard_tool_edit(server, KWL_EDIT_PASTE);
		break;
	case TOOL_TAB_EDIT:
		keyboard.tools_face = KEYBOARD_FACE_EDIT;
		printf("KWL OSK tool face=edit\n");
		break;
	case TOOL_TAB_HISTORY:
		keyboard.tools_face = KEYBOARD_FACE_HISTORY;
		printf("KWL OSK tool face=history items=%u\n", kwl_clipboard_history_count(server));
		break;
	case TOOL_TAB_EMOJI:
		/* The emoji face, at the category shown last; the log gives the tests its places. */
		keyboard.tools_face = KEYBOARD_FACE_EMOJI;
		printf("KWL OSK tool face=emoji category=%u\n", keyboard.emoji_category);
		keyboard_emoji_log(server);
		break;
	case TOOL_TAB_CANDIDATES:
		/* The candidates' face (ws166-p003). */
		keyboard.tools_face = KEYBOARD_FACE_CANDIDATES;
		printf("KWL OSK tool face=candidates count=%u\n", keyboard.prediction_count);
		break;
	default:
		break;
	}
}

/* Sends a movement's key to the focused window, with Shift while selecting (the selection grows). */
static void
keyboard_tool_move(
	struct kwl_server *server,
	unsigned code)
{
	int shift;

	/* Shift while selecting. */
	shift = 0;
	if (keyboard.selecting)
		shift = 1;

	/* The key. */
	(void)keyboard_send_key(server, code, shift);
}

/* Sends an edit operation to the focused window (edit.c: its own operation, or its key). */
static void
keyboard_tool_edit(
	struct kwl_server *server,
	unsigned action)
{
	int error;

	/* The operation, and the log line the tests read. */
	error = kwl_edit_action(server, action);
	printf("KWL OSK tool edit action=%u error=%d\n", action, error);
}

/*
 * Draws the flick panel's tools over its keys' column: the row always
 * there, the tabs (edit lit), the edit tools; selecting lit while on, a
 * tool the focused window cannot do now faint, the held tool blue.
 */
static void
keyboard_draw_tools(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 0.92f };
	static const float grey[4] = { 0.86f, 0.89f, 0.93f, 0.95f };
	static const float pale[4] = { 0.78f, 0.86f, 0.99f, 1.0f };
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float faint[4] = { 0.12f, 0.16f, 0.24f, 0.30f };
	const struct keyboard_tool *tool;
	const float *ground;
	const float *ink;
	int32_t rect[4];
	uint32_t enabled;
	unsigned index;
	int state;
	int usable;

	/* What the focused window can do now. */
	enabled = 0;
	state = kwl_edit_state(server, &enabled);

	/* Each tool. */
	for (index = 0; index < sizeof(keyboard_tools) / sizeof(keyboard_tools[0]); index++) {
		/* Its place and colours: the row always there grey, the edit tab and selecting while on pale, faint when it cannot. */
		tool = &keyboard_tools[index];
		keyboard_tool_rect(server, index, rect);
		ground = white;
		ink = dark;
		if (tool->row == 0U)
			ground = grey;
		if (tool->row >= 2U && keyboard.tools_face != KEYBOARD_FACE_EDIT)
			continue;
		if (tool->kind == TOOL_TAB_EDIT && keyboard.tools_face == KEYBOARD_FACE_EDIT)
			ground = pale;
		if (tool->kind == TOOL_TAB_HISTORY && keyboard.tools_face == KEYBOARD_FACE_HISTORY)
			ground = pale;
		if (tool->kind == TOOL_TAB_EMOJI && keyboard.tools_face == KEYBOARD_FACE_EMOJI)
			ground = pale;
		if (tool->kind == TOOL_TAB_CANDIDATES && keyboard.tools_face == KEYBOARD_FACE_CANDIDATES)
			ground = pale;
		if (tool->kind == TOOL_SELECT && keyboard.selecting)
			ground = pale;
		usable = keyboard_tool_enabled(server, index, enabled, state);
		if (!usable)
			ink = faint;

		/* The held tool is blue. */
		if (keyboard.tool_active && keyboard.tool == index) {
			ground = keyboard_blue;
			ink = keyboard_on_blue;
		}

		/* The tool. */
		keyboard_draw_key(server, command, rect, tool->label, ground, ink, SIZE_TITLE);
	}
}

/*
 * Works out a row of the history's list: the rows share the flick panel's
 * column between the tabs and the keys.
 */
static void
keyboard_history_rect(
	struct kwl_server *server,
	unsigned row,
	int32_t *rect)
{
	int32_t top;
	int32_t bottom;
	int32_t height;
	int key;

	/* From under the tabs to over the keys. */
	key = keyboard_key_size(server);
	top = keyboard.panel[1] + KEYBOARD_BAND + 2 * KEYBOARD_KEY_GAP + KEYBOARD_TOOL_ROW + KEYBOARD_KEY_GAP + KEYBOARD_TOOL_TABS + KEYBOARD_KEY_GAP;
	bottom = keyboard.panel[1] + keyboard.panel[3] - (int32_t)KWL_FLICK_ROWS * (key + KEYBOARD_KEY_GAP) - KEYBOARD_KEY_GAP;
	height = (bottom - top - (int32_t)(KEYBOARD_HISTORY_ROWS - 1U) * KEYBOARD_KEY_GAP) / (int32_t)KEYBOARD_HISTORY_ROWS;

	/* The row. */
	rect[0] = keyboard.panel[0] + KEYBOARD_KEY_GAP;
	rect[1] = top + (int32_t)row * (height + KEYBOARD_KEY_GAP);
	rect[2] = keyboard.panel[2] - 2 * KEYBOARD_KEY_GAP;
	rect[3] = height;
}

/* Finds the history's row at a point (only rows with an item).  Returns 1 with the row, or 0. */
static int
keyboard_history_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	unsigned *row)
{
	int32_t rect[4];
	unsigned count;
	unsigned index;
	int inside;

	/* Each row with an item. */
	count = kwl_clipboard_history_count(server);
	for (index = 0; index < count && index < KEYBOARD_HISTORY_ROWS; index++) {
		/* The point on this row. */
		keyboard_history_rect(server, index, rect);
		inside = keyboard_contains(rect, x, y);
		if (!inside)
			continue;

		/* Succeeded: the row. */
		*row = index;
		return 1;
	}

	/* No item there. */
	return 0;
}

/* Pastes the held row's item into the focused window (clipboard.c). */
static void
keyboard_history_release(
	struct kwl_server *server)
{
	int error;

	/* The item, pasted, and the log line the tests read; a reading ends. */
	keyboard.history_active = 0;
	server->dirty = 1;
	keyboard_reading_end(server, "paste");
	error = kwl_clipboard_history_paste(server, keyboard.history_row);
	printf("KWL OSK history paste index=%u error=%d\n", keyboard.history_row, error);
}

/*
 * Draws the clipboard's history under the tabs: each item on a row, the
 * newest first, its text on one line (line breaks and tabs shown as
 * spaces, cut in the middle when too long); the held row blue; a note when
 * the history is empty.
 */
static void
keyboard_draw_history(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 0.92f };
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float soft[4] = { 0.34f, 0.38f, 0.46f, 1.0f };
	char line[KEYBOARD_HISTORY_TEXT + 1U];
	const float *ground;
	const float *ink;
	const char *text;
	int32_t rect[4];
	size_t length;
	size_t byte;
	unsigned count;
	unsigned index;

	/* An empty history: a note in the first row's place. */
	count = kwl_clipboard_history_count(server);
	if (count == 0U) {
		keyboard_history_rect(server, 0U, rect);
		glass_draw_text(server, command, SIZE_BAR, rect[0] + 8, rect[1] + rect[3] / 2 + 5, "履歴はまだありません", rect[2] - 16, soft);
		return;
	}

	/* Each item, the newest first. */
	for (index = 0; index < count && index < KEYBOARD_HISTORY_ROWS; index++) {
		/* The item's text, on one line (cut to the room of the copy). */
		text = kwl_clipboard_history_get(server, index, &length);
		if (text == NULL)
			continue;
		if (length > KEYBOARD_HISTORY_TEXT)
			length = KEYBOARD_HISTORY_TEXT;
		memcpy(line, text, length);
		line[length] = '\0';
		for (byte = 0; byte < length; byte++) {
			if (line[byte] == '\n' || line[byte] == '\r' || line[byte] == '\t')
				line[byte] = ' ';
		}

		/* The row: blue while held. */
		keyboard_history_rect(server, index, rect);
		ground = white;
		ink = dark;
		if (keyboard.history_active && keyboard.history_row == index) {
			ground = keyboard_blue;
			ink = keyboard_on_blue;
		}

		/* Its ground and its text, cut in the middle to the row. */
		glass_draw_solid(server, command, (float)rect[0], (float)rect[1], (float)rect[2], (float)rect[3], 6.0f, ground);
		glass_draw_text_middle(server, command, SIZE_TITLE, rect[0] + 8, rect[1] + rect[3] / 2 + 5, line, rect[2] - 16, ink);
	}
}

/*
 * Works out a place of the emoji face, which shares the flick panel's
 * column between the tabs and the keys with the history: the category
 * tabs on a row at the top (slots 0 to KWL_EMOJI_CATEGORIES - 1), the
 * emoji's grid under them (the following slots, row by row).
 */
static void
keyboard_emoji_rect(
	struct kwl_server *server,
	unsigned slot,
	int32_t *rect)
{
	int32_t top;
	int32_t bottom;
	int32_t width;
	int32_t tab_width;
	int32_t grid_top;
	int32_t cell_width;
	int32_t cell_height;
	unsigned cell;
	int key;

	/* From under the tools' tabs to over the keys, as the history's rows. */
	key = keyboard_key_size(server);
	top = keyboard.panel[1] + KEYBOARD_BAND + 2 * KEYBOARD_KEY_GAP + KEYBOARD_TOOL_ROW + KEYBOARD_KEY_GAP + KEYBOARD_TOOL_TABS + KEYBOARD_KEY_GAP;
	bottom = keyboard.panel[1] + keyboard.panel[3] - (int32_t)KWL_FLICK_ROWS * (key + KEYBOARD_KEY_GAP) - KEYBOARD_KEY_GAP;
	width = keyboard.panel[2] - 2 * KEYBOARD_KEY_GAP;

	/* A category's tab: an equal share of the top row. */
	if (slot < KWL_EMOJI_CATEGORIES) {
		tab_width = (width - (int32_t)(KWL_EMOJI_CATEGORIES - 1U) * KEYBOARD_KEY_GAP) / (int32_t)KWL_EMOJI_CATEGORIES;
		rect[0] = keyboard.panel[0] + KEYBOARD_KEY_GAP + (int32_t)slot * (tab_width + KEYBOARD_KEY_GAP);
		rect[1] = top;
		rect[2] = tab_width;
		rect[3] = KEYBOARD_TOOL_TABS;
		return;
	}

	/* An emoji's cell: its column and row in the grid under the tabs. */
	cell = slot - KWL_EMOJI_CATEGORIES;
	grid_top = top + KEYBOARD_TOOL_TABS + KEYBOARD_KEY_GAP;
	cell_width = (width - (int32_t)(KEYBOARD_EMOJI_COLUMNS - 1U) * KEYBOARD_KEY_GAP) / (int32_t)KEYBOARD_EMOJI_COLUMNS;
	cell_height = (bottom - grid_top - (int32_t)(KEYBOARD_EMOJI_ROWS - 1U) * KEYBOARD_KEY_GAP) / (int32_t)KEYBOARD_EMOJI_ROWS;
	rect[0] = keyboard.panel[0] + KEYBOARD_KEY_GAP + (int32_t)(cell % KEYBOARD_EMOJI_COLUMNS) * (cell_width + KEYBOARD_KEY_GAP);
	rect[1] = grid_top + (int32_t)(cell / KEYBOARD_EMOJI_COLUMNS) * (cell_height + KEYBOARD_KEY_GAP);
	rect[2] = cell_width;
	rect[3] = cell_height;
}

/* Finds the emoji face's tab or emoji at a point (only cells with an emoji).  Returns 1 with its slot, or 0. */
static int
keyboard_emoji_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	unsigned *slot)
{
	int32_t rect[4];
	unsigned count;
	unsigned index;
	int inside;

	/* The tabs, then the cells of the category shown. */
	count = KWL_EMOJI_CATEGORIES + kwl_emoji_count(keyboard.emoji_category);
	for (index = 0; index < count; index++) {
		/* The point on this place. */
		keyboard_emoji_rect(server, index, rect);
		inside = keyboard_contains(rect, x, y);
		if (!inside)
			continue;

		/* Succeeded: the place. */
		*slot = index;
		return 1;
	}

	/* Nothing there. */
	return 0;
}

/*
 * Acts on the release of the held place of the emoji face: a tab shows
 * its category, an emoji is committed to the focused field's text input
 * (a window without one takes nothing, keyboard_send_commit says why).
 */
static void
keyboard_emoji_release(
	struct kwl_server *server)
{
	const char *text;
	int sent;

	/* The place is let go. */
	keyboard.emoji_active = 0;
	server->dirty = 1;

	/* A tab: its category, and its places for the tests. */
	if (keyboard.emoji_slot < KWL_EMOJI_CATEGORIES) {
		keyboard.emoji_category = keyboard.emoji_slot;
		printf("KWL OSK emoji category=%u\n", keyboard.emoji_category);
		keyboard_emoji_log(server);
		return;
	}

	/* The emoji of the cell. */
	text = kwl_emoji(keyboard.emoji_category, keyboard.emoji_slot - KWL_EMOJI_CATEGORIES);
	if (text == NULL)
		return;

	/* Committed whole (no deletion before it), and the log line the tests read; a reading ends. */
	sent = keyboard_send_commit(server, text, 0U);
	printf("KWL OSK emoji commit sent=%d text=%s\n", sent, text);
	keyboard_reading_end(server, "emoji");

	/*
	 * The voice key replaces the last kana sent by deleting its bytes; after
	 * an emoji there is no kana before the cursor to replace.
	 */
	if (sent)
		keyboard.last_sent = KEYBOARD_SENT_NONE;
}

/* Logs the emoji face's places (its tabs and the cells of the category shown) for the tests. */
static void
keyboard_emoji_log(
	struct kwl_server *server)
{
	int32_t rect[4];
	unsigned count;
	unsigned slot;

	/* The tabs. */
	for (slot = 0; slot < KWL_EMOJI_CATEGORIES; slot++) {
		keyboard_emoji_rect(server, slot, rect);
		printf("KWL OSK etab category=%u x=%d y=%d width=%d height=%d\n", slot, (int)rect[0], (int)rect[1], (int)rect[2], (int)rect[3]);
	}

	/* The cells of the category shown. */
	count = kwl_emoji_count(keyboard.emoji_category);
	for (slot = 0; slot < count; slot++) {
		keyboard_emoji_rect(server, KWL_EMOJI_CATEGORIES + slot, rect);
		printf("KWL OSK erect category=%u index=%u x=%d y=%d width=%d height=%d\n", keyboard.emoji_category, slot, (int)rect[0], (int)rect[1], (int)rect[2], (int)rect[3]);
	}
}

/*
 * Draws the emoji face under the tools' tabs: the category tabs (the one
 * shown pale), and the category's emoji in colour on white cells; the held
 * place blue.
 */
static void
keyboard_draw_emoji(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 0.92f };
	static const float pale[4] = { 0.78f, 0.86f, 0.99f, 1.0f };
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	const float *ground;
	const float *ink;
	const char *text;
	enum glass_size size;
	int32_t rect[4];
	unsigned count;
	unsigned slot;

	/* The tabs: the category shown pale, the held one blue. */
	for (slot = 0; slot < KWL_EMOJI_CATEGORIES; slot++) {
		keyboard_emoji_rect(server, slot, rect);
		ground = white;
		ink = dark;
		if (slot == keyboard.emoji_category)
			ground = pale;
		if (keyboard.emoji_active && keyboard.emoji_slot == slot) {
			ground = keyboard_blue;
			ink = keyboard_on_blue;
		}

		/* The tab with its category's name. */
		keyboard_draw_key(server, command, rect, kwl_emoji_category_name(slot), ground, ink, SIZE_TITLE);
	}

	/* The emoji large when the cells have the room, a size smaller otherwise. */
	keyboard_emoji_rect(server, KWL_EMOJI_CATEGORIES, rect);
	size = SIZE_SEARCH;
	if (rect[3] >= KEYBOARD_EMOJI_LARGE)
		size = SIZE_ICON;

	/* Each emoji of the category on its cell, the held one blue. */
	count = kwl_emoji_count(keyboard.emoji_category);
	for (slot = 0; slot < count; slot++) {
		text = kwl_emoji(keyboard.emoji_category, slot);
		keyboard_emoji_rect(server, KWL_EMOJI_CATEGORIES + slot, rect);
		ground = white;
		if (keyboard.emoji_active && keyboard.emoji_slot == KWL_EMOJI_CATEGORIES + slot)
			ground = keyboard_blue;
		keyboard_draw_key(server, command, rect, text, ground, dark, size);
	}
}

/* Counts the characters of a UTF-8 text (the bytes that do not continue one). */
static unsigned
keyboard_characters(
	const char *text)
{
	const unsigned char *byte;
	unsigned count;

	/* Each byte that starts a character. */
	count = 0;
	for (byte = (const unsigned char *)text;
	     *byte != '\0';
	     byte++) {
		if ((*byte & 0xc0U) != 0x80U)
			count++;
	}

	/* The characters. */
	return count;
}

/* Names a panel (or a corner) for the log. */
static const char *
keyboard_kind_name(
	enum keyboard_kind kind)
{
	/* Each kind. */
	switch (kind) {
	case PANEL_FLICK:
		return "flick";
	case PANEL_QWERTY:
		return "qwerty";
	default:
		break;
	}

	/* None. */
	return "none";
}

/* Names a contact's source for the log. */
static const char *
keyboard_source_name(
	enum kwl_contact_source source)
{
	/* Each source. */
	switch (source) {
	case KWL_CONTACT_TOUCH:
		return "touch";
	case KWL_CONTACT_PEN:
		return "pen";
	default:
		break;
	}

	/* The pointer. */
	return "pointer";
}

/*
 * Takes the predictions the input method gave for a reading
 * (kl_ime_status_v1.predictions, ws166-p003): the words of the
 * latest request fill the candidates' tab, "WORD\tREADING" a line; an
 * answer to an older request is dropped.
 */
void
kwl_keyboard_predictions(
	struct kwl_server *server,
	uint32_t serial,
	const char *list)
{
	const char *line;
	const char *tab;
	const char *end;
	const char *first;
	int32_t rect[4];
	unsigned index;
	size_t word;
	size_t reading;

	/* Only the answer to the latest reading. */
	if (serial != keyboard.predict_serial || keyboard.reading[0] == '\0') {
		printf("KWL OSK predictions stale serial=%u\n", serial);
		return;
	}

	/* Each line that fits, while there is room. */
	keyboard.prediction_count = 0;
	line = list;
	while (*line != '\0' && keyboard.prediction_count < KEYBOARD_PREDICTIONS) {
		/* The line's end and its tab. */
		end = strchr(line, '\n');
		if (end == NULL)
			end = line + strlen(line);
		tab = memchr(line, '\t', (size_t)(end - line));

		/* A line with a word, a tab and a reading that fit is kept. */
		if (tab != NULL) {
			word = (size_t)(tab - line);
			reading = (size_t)(end - tab - 1);
			if (word > 0U && word < KEYBOARD_PREDICTION_TEXT && reading < KEYBOARD_PREDICTION_TEXT) {
				memcpy(keyboard.predictions[keyboard.prediction_count], line, word);
				keyboard.predictions[keyboard.prediction_count][word] = '\0';
				memcpy(keyboard.prediction_readings[keyboard.prediction_count], tab + 1, reading);
				keyboard.prediction_readings[keyboard.prediction_count][reading] = '\0';
				keyboard.prediction_count++;
			}
		}

		/* The next line. */
		if (*end == '\0')
			break;
		line = end + 1;
	}

	/* Shown, and the log line the tests read. */
	keyboard.candidate_active = 0;
	server->dirty = 1;
	first = "-";
	if (keyboard.prediction_count != 0U)
		first = keyboard.predictions[0];
	printf("KWL OSK predictions serial=%u reading=%s count=%u first=%s\n", serial, keyboard.reading, keyboard.prediction_count, first);

	/* The words' places, for the tests to tap them. */
	for (index = 0; index < keyboard.prediction_count && index < KEYBOARD_CANDIDATE_COLUMNS * KEYBOARD_CANDIDATE_ROWS; index++) {
		keyboard_candidate_rect(server, index, rect);
		printf("KWL OSK crect slot=%u x=%d y=%d width=%d height=%d\n", index, (int)rect[0], (int)rect[1], (int)rect[2], (int)rect[3]);
	}
}

/*
 * Follows the reading after a character was sent (ws166-p003): a hiragana
 * committed into the field the reading is in, or begins one in, grows it;
 * anything else ends it.
 */
static void
keyboard_reading_sent(
	struct kwl_server *server,
	const char *text,
	unsigned sent)
{
	struct kwl_text_input *input;
	size_t length;
	size_t added;
	int kana;
	int secret;

	/* A key, or a character that is not hiragana, ends the reading. */
	kana = keyboard_hiragana(text);
	if (sent != KEYBOARD_SENT_COMMIT || !kana) {
		keyboard_reading_end(server, "other");
		return;
	}

	/* A secret field keeps no reading. */
	input = kwl_text_input_current(server);
	secret = keyboard_secret(input);
	if (input == NULL || secret) {
		keyboard_reading_end(server, "secret");
		return;
	}

	/* Another field begins a new reading. */
	if (input != keyboard.reading_input) {
		keyboard_reading_end(server, "field");
		keyboard.reading_input = input;
	}

	/* The hiragana added, when it fits (a reading too long ends). */
	length = strlen(keyboard.reading);
	added = strlen(text);
	if (length + added >= sizeof(keyboard.reading)) {
		keyboard_reading_end(server, "long");
		return;
	}

	/* Added. */
	memcpy(keyboard.reading + length, text, added + 1U);

	/* A reading that begins brings the candidates' tab up; the words are asked for. */
	if (length == 0U)
		keyboard.tools_face = KEYBOARD_FACE_CANDIDATES;
	keyboard_reading_predict(server);
}

/* Takes the last character off the reading (the delete key), asking again for what is left. */
static void
keyboard_reading_back(
	struct kwl_server *server)
{
	size_t length;

	/* No reading. */
	length = strlen(keyboard.reading);
	if (length == 0U)
		return;

	/* Back over the last character's continuation bytes, then its first. */
	while (length > 0U && ((unsigned char)keyboard.reading[length - 1U] & 0xc0U) == 0x80U)
		length--;
	if (length > 0U)
		length--;
	keyboard.reading[length] = '\0';

	/* The words for what is left (none for nothing). */
	if (length == 0U) {
		keyboard_reading_end(server, "empty");
		return;
	}

	/* What is left is asked for. */
	keyboard_reading_predict(server);
}

/* Replaces the reading's last character (the voice key's change), asking again. */
static void
keyboard_reading_replace(
	struct kwl_server *server,
	const char *before,
	const char *after)
{
	size_t length;
	size_t old;
	size_t added;
	int differs;

	/* Only a reading that ends with the character changed. */
	length = strlen(keyboard.reading);
	old = strlen(before);
	added = strlen(after);
	if (length < old) {
		keyboard_reading_end(server, "voice");
		return;
	}

	/* The end is the character changed, and the new one fits. */
	differs = memcmp(keyboard.reading + length - old, before, old);
	if (differs != 0 || length - old + added >= sizeof(keyboard.reading)) {
		keyboard_reading_end(server, "voice");
		return;
	}

	/* The character replaced, and the words asked for again. */
	memcpy(keyboard.reading + length - old, after, added + 1U);
	keyboard_reading_predict(server);
}

/* Ends the reading: no reading, no words (the request in flight is dropped when it answers). */
static void
keyboard_reading_end(
	struct kwl_server *server,
	const char *why)
{
	/* Nothing to end. */
	if (keyboard.reading[0] == '\0' && keyboard.prediction_count == 0U)
		return;

	/* The reading and its words go. */
	printf("KWL OSK reading end reason=%s reading=%s\n", why, keyboard.reading);
	keyboard.reading[0] = '\0';
	keyboard.reading_input = NULL;
	keyboard.prediction_count = 0;
	keyboard.candidate_active = 0;
	keyboard.predict_serial++;
	server->dirty = 1;
}

/* Asks the input method for the reading's words; the old words stay until they come. */
static void
keyboard_reading_predict(
	struct kwl_server *server)
{
	struct kwl_text_input *input;
	int error;

	/* The field's text known so far is older than this reading (the field read only while it is current). */
	input = kwl_text_input_current(server);
	if (input != NULL && input == keyboard.reading_input)
		keyboard.reading_commit = input->commits;

	/* A new request: an answer to an older one is dropped. */
	keyboard.predict_serial++;
	error = kwl_ime_predict(server, keyboard.predict_serial, keyboard.reading);
	printf("KWL OSK reading=%s serial=%u error=%d\n", keyboard.reading, keyboard.predict_serial, error);

	/* Without an input method that predicts there are no words. */
	if (error != 0)
		keyboard.prediction_count = 0;
	server->dirty = 1;
}

/* Tells whether a text is hiragana alone (with the long vowel mark), the characters a reading has. */
static int
keyboard_hiragana(
	const char *text)
{
	const unsigned char *byte;
	uint32_t code;

	/* Nothing is not a reading. */
	byte = (const unsigned char *)text;
	if (*byte == 0U)
		return 0;

	/* Each character: three bytes of UTF-8 in U+3041 to U+3096, or U+30FC. */
	while (*byte != 0U) {
		if ((byte[0] & 0xf0U) != 0xe0U || (byte[1] & 0xc0U) != 0x80U || (byte[2] & 0xc0U) != 0x80U)
			return 0;
		code = ((uint32_t)(byte[0] & 0x0fU) << 12) | ((uint32_t)(byte[1] & 0x3fU) << 6) | (uint32_t)(byte[2] & 0x3fU);
		if ((code < 0x3041U || code > 0x3096U) && code != 0x30fcU)
			return 0;
		byte += 3;
	}

	/* Succeeded: hiragana. */
	return 1;
}

/* Tells whether a field holds a secret: a password, a PIN, hidden or sensitive text. */
static int
keyboard_secret(
	const struct kwl_text_input *input)
{
	/* No field, no secret. */
	if (input == NULL)
		return 0;

	/* The purpose or the hints say so. */
	if (input->purpose == KEYBOARD_PURPOSE_PASSWORD || input->purpose == KEYBOARD_PURPOSE_PIN)
		return 1;
	if ((input->hint & KEYBOARD_HINT_SECRET) != 0U)
		return 1;

	/* Not a secret. */
	return 0;
}

/*
 * Works out a cell of the candidates' face, which shares the flick
 * panel's column between the tabs and the keys with the history: the
 * words in a grid, row by row.
 */
static void
keyboard_candidate_rect(
	struct kwl_server *server,
	unsigned slot,
	int32_t *rect)
{
	int32_t top;
	int32_t bottom;
	int32_t width;
	int32_t cell_width;
	int32_t cell_height;
	int key;

	/* From under the tools' tabs to over the keys, as the history's rows. */
	key = keyboard_key_size(server);
	top = keyboard.panel[1] + KEYBOARD_BAND + 2 * KEYBOARD_KEY_GAP + KEYBOARD_TOOL_ROW + KEYBOARD_KEY_GAP + KEYBOARD_TOOL_TABS + KEYBOARD_KEY_GAP;
	bottom = keyboard.panel[1] + keyboard.panel[3] - (int32_t)KWL_FLICK_ROWS * (key + KEYBOARD_KEY_GAP) - KEYBOARD_KEY_GAP;
	width = keyboard.panel[2] - 2 * KEYBOARD_KEY_GAP;

	/* The cell's column and row. */
	cell_width = (width - (int32_t)(KEYBOARD_CANDIDATE_COLUMNS - 1U) * KEYBOARD_KEY_GAP) / (int32_t)KEYBOARD_CANDIDATE_COLUMNS;
	cell_height = (bottom - top - (int32_t)(KEYBOARD_CANDIDATE_ROWS - 1U) * KEYBOARD_KEY_GAP) / (int32_t)KEYBOARD_CANDIDATE_ROWS;
	rect[0] = keyboard.panel[0] + KEYBOARD_KEY_GAP + (int32_t)(slot % KEYBOARD_CANDIDATE_COLUMNS) * (cell_width + KEYBOARD_KEY_GAP);
	rect[1] = top + (int32_t)(slot / KEYBOARD_CANDIDATE_COLUMNS) * (cell_height + KEYBOARD_KEY_GAP);
	rect[2] = cell_width;
	rect[3] = cell_height;
}

/* Finds the word of the candidates' face at a point.  Returns 1 with its slot, or 0. */
static int
keyboard_candidate_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y,
	unsigned *slot)
{
	int32_t rect[4];
	unsigned index;
	int inside;

	/* Each word shown. */
	for (index = 0; index < keyboard.prediction_count && index < KEYBOARD_CANDIDATE_COLUMNS * KEYBOARD_CANDIDATE_ROWS; index++) {
		/* The point on this word. */
		keyboard_candidate_rect(server, index, rect);
		inside = keyboard_contains(rect, x, y);
		if (!inside)
			continue;

		/* Succeeded: the word. */
		*slot = index;
		return 1;
	}

	/* No word there. */
	return 0;
}

/*
 * Replaces the reading before the cursor by the word held, and has the
 * input method learn it (ws166-p003).  The field must still be the
 * reading's, and when it has told its text since the reading last changed,
 * the reading must be what is before the cursor; otherwise nothing is
 * replaced and the reading ends.  A field that tells no text, or has not
 * told it since (most applications never do, and ime-probe tells it once
 * at the enable), is trusted to hold the reading the keyboard itself
 * committed.
 */
static void
keyboard_candidate_release(
	struct kwl_server *server)
{
	struct kwl_text_input *input;
	char word[KEYBOARD_PREDICTION_TEXT];
	char reading[KEYBOARD_PREDICTION_TEXT];
	size_t length;
	size_t text_length;
	int differs;
	int sent;

	/* The word is let go. */
	keyboard.candidate_active = 0;
	server->dirty = 1;
	if (keyboard.candidate_slot >= keyboard.prediction_count || keyboard.reading[0] == '\0')
		return;

	/* The field the reading was typed into. */
	input = kwl_text_input_current(server);
	if (input == NULL || input != keyboard.reading_input) {
		printf("KWL OSK candidate refused reason=field\n");
		keyboard_reading_end(server, "field");
		return;
	}

	/* When the field told its text after the reading changed, the reading is what is before the cursor. */
	length = strlen(keyboard.reading);
	if (input->text != NULL && input->cursor >= 0 && input->text_commit > keyboard.reading_commit) {
		differs = 1;
		text_length = strlen(input->text);
		if ((size_t)input->cursor >= length && (size_t)input->cursor <= text_length)
			differs = memcmp(input->text + (size_t)input->cursor - length, keyboard.reading, length);
		if (differs != 0) {
			printf("KWL OSK candidate refused reason=surrounding\n");
			keyboard_reading_end(server, "surrounding");
			return;
		}
	}

	/* The word and its reading, kept before the reading ends. */
	(void)snprintf(word, sizeof(word), "%s", keyboard.predictions[keyboard.candidate_slot]);
	(void)snprintf(reading, sizeof(reading), "%s", keyboard.prediction_readings[keyboard.candidate_slot]);

	/* The reading's bytes deleted and the word committed together. */
	sent = keyboard_send_commit(server, word, (uint32_t)length);
	printf("KWL OSK candidate commit sent=%d slot=%u word=%s reading=%s\n", sent, keyboard.candidate_slot, word, reading);
	keyboard_reading_end(server, "chosen");
	if (!sent)
		return;

	/* Learned, and nothing for the voice key to change. */
	kwl_ime_learn(server, reading, word);
	keyboard_remember("", KEYBOARD_SENT_NONE);
}

/*
 * Draws the candidates' face under the tabs: the words in a grid, the
 * held one blue; a note when there is no reading or no word.
 */
static void
keyboard_draw_candidates(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 0.92f };
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float soft[4] = { 0.34f, 0.38f, 0.46f, 1.0f };
	const float *ground;
	const float *ink;
	const char *note;
	int32_t rect[4];
	unsigned index;

	/* No word: a note in the first cell's row. */
	if (keyboard.prediction_count == 0U) {
		keyboard_candidate_rect(server, 0U, rect);
		note = "かなを入力すると候補が出ます";
		if (keyboard.reading[0] != '\0')
			note = "候補はありません";
		glass_draw_text(server, command, SIZE_BAR, rect[0] + 8, rect[1] + rect[3] / 2 + 5, note, keyboard.panel[2] - 4 * KEYBOARD_KEY_GAP, soft);
		return;
	}

	/* Each word, the held one blue. */
	for (index = 0; index < keyboard.prediction_count && index < KEYBOARD_CANDIDATE_COLUMNS * KEYBOARD_CANDIDATE_ROWS; index++) {
		/* The cell's colours. */
		keyboard_candidate_rect(server, index, rect);
		ground = white;
		ink = dark;
		if (keyboard.candidate_active && keyboard.candidate_slot == index) {
			ground = keyboard_blue;
			ink = keyboard_on_blue;
		}

		/* Its ground and its word, cut in the middle to the cell. */
		glass_draw_solid(server, command, (float)rect[0], (float)rect[1], (float)rect[2], (float)rect[3], 6.0f, ground);
		glass_draw_text_middle(server, command, SIZE_TITLE, rect[0] + 6, rect[1] + rect[3] / 2 + 5, keyboard.predictions[index], rect[2] - 12, ink);
	}
}

