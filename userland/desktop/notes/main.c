/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Notes, the handwritten notebook (plan/ws079/design-input-notes.md
 * section 5).
 *
 *   notes [--fullscreen] [--width=W] [--height=H] [--timeout-s=S] [FILE.pdf]
 *
 * One process shows one notebook of A4 pages.  The pointer (and, once the
 * compositor offers the tablet protocol, a pen) draws strokes with the
 * pen, a translucent highlighter, or erases whole strokes; Undo and Redo
 * take changes back and make them again; pages are added and turned.
 *
 * The notebook is saved as a PDF (save.c) when asked (Ctrl+S, the toolbar,
 * the menu), on its own NOTES_AUTOSAVE_IDLE_MS (5 seconds) after the last
 * change once no stroke is being drawn, and when Notes closes.  Every
 * change is also written to a journal before it is shown (journal.c), so
 * a crash or a killed process loses nothing: the next start recovers the
 * notebook from the journal -- the journal of FILE when one is given, or
 * the most recent journal when none is.  A new notebook is saved as
 * ~/Documents/Notes/note-YYYYMMDD-HHMMSS.pdf.
 *
 * Ctrl+N adds a page after the current one (one process is one notebook,
 * so a new page is what "new" makes).  FILE is opened from the edit data
 * its PDF carries (save.c).  Another program's PDF -- or a notebook whose
 * pages another program changed -- is written on: its pages are drawn under
 * the strokes (libpdf draws them into the background picture) and each
 * save adds the strokes to the file as a new revision, leaving its own
 * bytes as they were.  An encrypted or signed PDF is left as it is and a
 * new notebook starts.  Ctrl+O opens another PDF with libkeiland's file
 * chooser (the notebook shown is saved first), and Ctrl+Shift+S saves the
 * notebook as another file, which Notes goes on writing (ws128-p002).
 *
 * ws081-p013: the fingers do not write.  One finger scrolls a page zoomed
 * past the window (with inertia), two fingers zoom, a double tap zooms in
 * or back to the whole page, and a tap on the toolbar presses its button;
 * a palm near the pen is left alone (touch.c).  ws081-p015: while the
 * toolbar's Finger is on, one finger writes as the pointer does and two
 * fingers scroll and zoom.
 *
 * ws175-p008: the Select tool edits the PDF's images and graphics (plan/
 * ws175/phase001/design.md section 7): a press chooses the object under it
 * (framed in blue, a handle on each corner), a drag moves it, a handle's
 * drag sizes it (its proportions kept; Shift sizes the sides apart), the
 * arrow keys move it by a point (Shift: ten) and Delete deletes it; the
 * toolbar's Image inserts an image file, Replace puts one in the chosen
 * image's place and Reset puts a page's object back as the page has it.
 * Each is one change to undo.  A page with edits is drawn by its editor
 * (edit.c), again every MAIN_DRAG_PREVIEW_MS while a drag moves its
 * object.
 *
 * ws175-p008 with ws079-p017: the Text tool puts words on the page and
 * changes the PDF's lines of text.  A press on a line opens its words in
 * the text box (box.c: libkeiland's field, which takes an input method's
 * text) under it, and the page shows the words as they are typed; a press
 * elsewhere opens a box of several lines there for new words (a drag
 * across sets the width they wrap at), in the toolbar's font, size and
 * pen colour.  Enter in a line's box, Esc, or a press outside the box
 * closes it keeping the words as one change -- once the page's editor
 * takes them: words no font writes keep the box open with the reason.
 * With the Select tool a line of text is chosen, moved and sized like an
 * image, and a double click (or Edit, or Enter) opens its words in the
 * box; its font and size are the toolbar's Font and A- / A+.
 *
 * --timeout-s ends Notes after that many seconds as if it were closed (the
 * tests use it to bound a run).  The lines starting with "NOTES" on the
 * standard output are what the tests read.
 */

#include "app.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The window's size when the compositor leaves it to Notes. */
#define MAIN_WIDTH		1024U
#define MAIN_HEIGHT		768U

/* The font the toolbar's labels are drawn with. */
#define MAIN_FONT		KEILAND_DATADIR "/fonts/keiland.ttf"

/* The longest path Notes keeps. */
#define MAIN_PATH_MAX		4096U

/* The app_id the file chooser's window gets, so that compositor shows it as Notes'. */
#define MAIN_APPLICATION	"notes"

/* The eraser's radius, in points. */
#define MAIN_ERASER_RADIUS	10.0f

/* How close two samples may be before the second is dropped, in pixels. */
#define MAIN_SAMPLE_STEP	0.25f

/* How long a status stays on the toolbar, in milliseconds. */
#define MAIN_STATUS_MS		4000U

/* Notes' own settings (ws177-p011), and the key that remembers the clean copy's notice was given. */
#define MAIN_SETTINGS_APP	"notes"
#define MAIN_CLEAN_TOLD_KEY	"notes.clean-copy-told"

/* How long what Notes found when it opened a PDF (written on, refused) stays, in milliseconds. */
#define MAIN_NOTICE_MS		10000U

/* The evdev codes of the keys Notes handles itself. */
#define MAIN_KEY_ESC		1U
#define MAIN_KEY_W		17U
#define MAIN_KEY_E		18U
#define MAIN_KEY_T		20U
#define MAIN_KEY_Y		21U
#define MAIN_KEY_O		24U
#define MAIN_KEY_P		25U
#define MAIN_KEY_S		31U
#define MAIN_KEY_Z		44U
#define MAIN_KEY_N		49U
#define MAIN_KEY_M		50U
#define MAIN_KEY_F11		87U
#define MAIN_KEY_PAGE_UP	104U
#define MAIN_KEY_PAGE_DOWN	109U
#define MAIN_KEY_BACKSPACE	14U
#define MAIN_KEY_UP		103U
#define MAIN_KEY_LEFT		105U
#define MAIN_KEY_RIGHT		106U
#define MAIN_KEY_DOWN		108U
#define MAIN_KEY_DELETE		111U
#define MAIN_KEY_ENTER		28U

/* The points of a circle the pen's mark is drawn with. */
#define MAIN_CIRCLE_POINTS	40U

/*
 * The desk around the page: a soft wash like Kei's blurred landscape --
 * pale sky at the top, a light haze a little below the middle, pale leaf
 * green at the bottom; in the dark appearance the same wash at night,
 * ws089-p017; the page stays white paper) -- and the page's slate shadow: its
 * colour, its darkest alpha, how many pixels it fades over and how far it
 * drops below the page.
 */
#define MAIN_DESK_TOP		kl_theme_choose(0xd9e6f5ffU, 0x1b2230ffU)
#define MAIN_DESK_HAZE		kl_theme_choose(0xeef3f6ffU, 0x20252dffU)
#define MAIN_DESK_BOTTOM	kl_theme_choose(0xdfecd6ffU, 0x1a2219ffU)
#define MAIN_DESK_HAZE_SHARE	0.58f
#define MAIN_SHADOW_COLOR	0x1f3a6600U
#define MAIN_SHADOW_ALPHA	44
#define MAIN_SHADOW_RINGS	12
#define MAIN_SHADOW_DROP	3.0f

/* The colours of the pen's mark: its white halo, the slate of the eraser's ring and its fill. */
#define MAIN_MARK_HALO		0xffffffd8U
#define MAIN_MARK_RING		0x33415599U
#define MAIN_MARK_FILL		0x3341551cU

/* What the current contact is doing. */
#define MAIN_CONTACT_NONE	0U
#define MAIN_CONTACT_TOOLBAR	1U
#define MAIN_CONTACT_DRAW	2U
#define MAIN_CONTACT_ERASE	3U
#define MAIN_CONTACT_SELECT	4U
#define MAIN_CONTACT_TEXT	5U
#define MAIN_CONTACT_BOX	6U

/*
 * The Select tool (ws175-p008): no object chosen; what a drag does (no
 * drag, the object moved, sized by a corner's handle); the handles' size
 * and how close a press must be to one, in pixels; the colour of the
 * chosen object's frame (Kei's blue); how often a drag draws the page
 * again with the object where the drag has it, in milliseconds; the
 * smallest side an object is sized to and how many times the page's size
 * it may reach; how far the arrow keys move it (Shift: ten times), in
 * points.
 */
#define MAIN_NONE		((size_t)-1)
#define MAIN_DRAG_NONE		0U
#define MAIN_DRAG_MOVE		1U
#define MAIN_DRAG_RESIZE	2U
#define MAIN_HANDLE_SIZE	8.0f
#define MAIN_HANDLE_REACH	12.0f
#define MAIN_SELECT_COLOR	(((kl_theme_default()->accent & 0xffffffU) << 8) | 0xffU)
#define MAIN_DRAG_PREVIEW_MS	100U

/* The most extra frames the text box draws at once to take keys that wait for it (one key a frame). */
#define MAIN_BOX_CATCH_UP	32U

#define MAIN_SIDE_MIN		4.0
#define MAIN_SIDE_TIMES_PAGE	4.0
#define MAIN_NUDGE		1.0

/*
 * ws175-p008 (with ws079-p017): what the text box edits -- a line of the
 * page's own, an inserted text, new words -- and the box's sizes in pixels
 * (a line's height; the words' box at least as wide, its height; the gap
 * between an object and the box under it; the least drag that sets the
 * width new words wrap at).  A double click is two presses within
 * MAIN_DOUBLE_MS.  The text's size goes by MAIN_SIZE_STEP points between
 * MAIN_SIZE_MIN and MAIN_SIZE_MAX, from MAIN_SIZE_DEFAULT.
 */
#define MAIN_BOX_LINE		0U
#define MAIN_BOX_INSERTED	1U
#define MAIN_BOX_NEW		2U
#define MAIN_BOX_LINE_HEIGHT	36
#define MAIN_BOX_WIDTH_MIN	240
#define MAIN_BOX_AREA_HEIGHT	120
#define MAIN_BOX_GAP		8
#define MAIN_BOX_DRAG_MIN	8.0f
#define MAIN_DOUBLE_MS		400U
#define MAIN_SIZE_STEP		0.5f
#define MAIN_SIZE_MIN		6.0f
#define MAIN_SIZE_MAX		144.0f
#define MAIN_SIZE_DEFAULT	12.0f

/* What the file chooser is shown for: a PDF (Open, Save As), an image to insert, an image for the chosen object, a clean copy (ws175-p009). */
#define MAIN_CHOOSE_DOCUMENT	0U
#define MAIN_CHOOSE_INSERT	1U
#define MAIN_CHOOSE_REPLACE	2U
#define MAIN_CHOOSE_CLEAN	3U

/*
 * Everything Notes holds for the one notebook it shows.
 */
struct notes_app {
	/* The window, its drawing, the toolbar, the frame being built and the page frame (below). */
	struct notes_window window;
	struct notes_renderer renderer;
	struct notes_ui ui;
	struct notes_frame frame;
	struct notes_frame page_frame;
	struct notes_view view;

	/*
	 * The fingers (touch.c): the gestures, the zoom and the scroll; when the
	 * page's place is due again (milliseconds, -1: not), the page it was
	 * last placed for (another page starts at its top), the place last
	 * logged for the tests, and whether the last frame was drawn while two
	 * fingers zoomed.
	 */
	struct notes_touch touch;
	int touch_due;
	const struct notes_page *touch_page;
	struct notes_view logged_view;
	int drawn_zooming;

	/*
	 * Writing with a finger (ws081-p015): whether it is on (the toolbar's
	 * Finger), and whether the contact under way is a writing finger's (its
	 * later events belong to it only while it is; the pointer or the pen
	 * taking the contact over clears it).
	 */
	int finger_write;
	int finger_contact;

	/*
	 * The page's picture (render.c) as the last frame left it: the page it
	 * shows, the renderer's serial of the picture, the document's reshaped
	 * count and the scale when it was drawn, and how many of the page's
	 * strokes it holds, from the bottom.  A frame adds the strokes put on
	 * top since, or draws the page again from the start when anything else
	 * changed.
	 */
	const struct notes_page *picture_page;
	unsigned long picture_serial;
	uint64_t picture_reshaped;
	float picture_scale;
	size_t picture_strokes;

	/*
	 * The background of a page of the PDF the notebook writes on: the
	 * drawing libpdf made of one page of the base (NULL: none yet), which
	 * page it is, and whether drawing it failed; and the page, the scale and
	 * the size the renderer's background picture was last drawn for (it is
	 * drawn again when any of them changes).
	 */
	struct pdf_display_list *background_list;
	size_t background_source;
	int background_failed;
	const struct notes_page *background_page;
	float background_scale;
	uint32_t background_width;
	uint32_t background_height;

	/*
	 * ws175-p008: the background's drawing is of the page with its edits
	 * (the page's editor) and of a drag under way: the page and the look
	 * (app_look) it was made for; and the look the page's picture was drawn
	 * with (it is drawn again from the start when the look changed).
	 */
	const struct notes_page *background_list_page;
	uint64_t background_look;
	uint64_t picture_look;

	/*
	 * The Select tool (ws175-p008): the object chosen on the page shown (its
	 * index among the page's editor's objects; MAIN_NONE: none), its kind
	 * and whether it was inserted or has an edit (for the toolbar); the drag
	 * under way (MAIN_DRAG_*), the corner whose handle it holds, where it
	 * started (page points), the object's corners then, and the map of the
	 * page's shown space it makes so far; the drags' previews drawn (they
	 * count into the look), when the last was, and whether the map changed
	 * since.
	 */
	size_t selected;
	enum pdf_edit_kind selected_kind;
	int selected_inserted;
	int selected_edited;
	unsigned drag;
	unsigned drag_corner;
	float drag_x;
	float drag_y;
	double drag_quad[8];
	double drag_map[6];
	uint64_t drag_previews;
	uint64_t drag_previewed_at;
	int drag_moved;

	/*
	 * ws175-p008: a text chosen (a line of the page's own or an inserted
	 * one): whether its words cannot be changed, its font (enum
	 * pdf_edit_font) and its size in points; and the last press of the
	 * Select tool (when, on which object), which a second one soon after on
	 * the same line of text makes a double click.
	 */
	int selected_fixed;
	unsigned selected_font;
	float selected_size;
	uint32_t select_press_ms;
	size_t select_press_object;

	/*
	 * The Text tool (ws175-p008 with ws079-p017): the font and the size new
	 * words take (the colour is the pen's), and the press under way that
	 * puts them (its place in the window; whether it is one).  The text box
	 * and what it edits (MAIN_BOX_*): the object's index in the page's
	 * editor, its state when the box opened (its words not kept), where new
	 * words go (page points) and the width they wrap at (0: not wrapped),
	 * and the font (and the one it opened with) and the size they are
	 * written in.  While the box edits a line of the page's own, the page
	 * shows the words typed (a preview in the page's editor): whether it
	 * does, the previews drawn (they count into the look), when the last
	 * was, and whether the words changed since.
	 */
	unsigned text_font;
	float text_size;
	int text_pressing;
	float text_press_x;
	float text_press_y;
	struct notes_box box;
	unsigned box_kind;
	size_t box_object;
	struct notes_edit box_edit;
	float box_x;
	float box_y;
	float box_width;
	double box_quad[8];
	int box_height;
	unsigned box_font;
	unsigned box_initial_font;
	float box_size;
	int box_previewing;
	uint64_t text_previews;
	uint64_t box_previewed_at;
	int box_changed;

	/* What the file chooser is shown for (MAIN_CHOOSE_*), and what it was shown for when it answered. */
	unsigned chooser_purpose;
	unsigned chosen_purpose;

	/* The pen over the window: whether it is there, its place (surface pixels) and its source (NOTES_SOURCE_*). */
	int hover;
	float hover_x;
	float hover_y;
	unsigned hover_source;

	/* The notebook, its file and the name shown in the title. */
	struct notes_document document;
	char path[MAIN_PATH_MAX];
	const char *name;

	/* The clean copy saved last this session (empty: none), which File > Open Clean Copy opens (ws177-p011). */
	char clean_path[MAIN_PATH_MAX];

	/*
	 * The page shown, the tool (its NOTES_ACTION_*), the colour's and the
	 * width's index, and whether the eraser (the tool, and a pen's eraser
	 * end) cuts parts of strokes rather than removing whole ones.
	 */
	size_t page;
	unsigned tool;
	unsigned color;
	unsigned width;
	int erase_parts;

	/* The contact under way, the stroke it draws (off the page until it ends), and when it started. */
	unsigned contact;
	struct notes_stroke *live;
	uint32_t live_start;

	/* When the notebook last changed (monotonic milliseconds). */
	uint64_t changed_at;

	/* The status on the toolbar, and until when (0: none). */
	char status[160];
	uint64_t status_until;

	/* Whether the frame and the toolbar need drawing again, and the width and the signature of the buttons logged last. */
	int redraw;
	int toolbar_dirty;
	uint32_t buttons_logged;
	uint64_t buttons_signature;
	int drawn;

	/* The desktop's appearance watched (ws089-p017): the desk and the toolbar follow it; NULL without it. */
	struct kl_appearance *appearance;

	/* When Notes ends by itself (0: never), and whether it is ending. */
	uint64_t deadline;
	int quit;

	/*
	 * File > Open and Save As (ws128-p002): libkeiland's file chooser while it
	 * is shown (NULL otherwise) and its mode, and the path it answered with,
	 * kept until the main loop carries it out (ready says it waits; an empty
	 * path is a cancel).
	 */
	struct kl_file_chooser *chooser;
	unsigned chooser_mode;
	char chosen[MAIN_PATH_MAX];
	unsigned chosen_mode;
	int chosen_ready;

	/*
	 * The frames drawn since the last NOTES FRAMES line: how many, and the
	 * sum and the longest of the time spent building their geometry and
	 * drawing them (microseconds).  The line goes out when a contact ends,
	 * so it tells what drawing a stroke costs a frame.
	 */
	unsigned long frame_count;
	uint64_t frame_build_us;
	uint64_t frame_draw_us;
	uint64_t frame_longest_us;
};

static int app_start_document(struct notes_app *app, const char *file);
static const char *app_opened_name(unsigned opened);
static int app_background(struct notes_app *app, const struct notes_page *page, float scale);
static int app_new_path(char *path, size_t size);
static int app_make_folders(const char *path);
static void app_set_title(struct notes_app *app);
static void app_status(struct notes_app *app, const char *text);
static void app_state(const struct notes_app *app, struct notes_ui_state *state);
static void app_action(struct notes_app *app, uint32_t action);
static void app_key(struct notes_app *app, const struct notes_key *key);
static void app_input(struct notes_app *app, const struct notes_input *input);
static void app_touch(struct notes_app *app);
static void app_finger(struct notes_app *app);
static void app_abort_contact(struct notes_app *app);
static void app_hover(struct notes_app *app, const struct notes_input *input);
static void app_sample(struct notes_app *app, const struct notes_input *input);
static void app_end_contact(struct notes_app *app, const struct notes_input *input);
static void app_changed(struct notes_app *app);
static int app_save(struct notes_app *app, const char *reason);
static void app_draw(struct notes_app *app);
static struct notes_frame *app_page_frame(struct notes_app *app, const struct notes_page *page, float scale, int *clear);
static void app_desk(struct notes_app *app, const struct notes_view *view, float width, float height);
static void app_mark(struct notes_app *app, const struct notes_view *view, int over_toolbar);
static void app_circle(struct notes_frame *frame, float cx, float cy, float radius, uint32_t color);
static void app_ring(struct notes_frame *frame, float cx, float cy, float radius, float thickness, uint32_t color);
static void app_frame_report(struct notes_app *app);
static int app_timeout(const struct notes_app *app, uint64_t now);
static uint64_t app_unix_ms(void);
static uint64_t app_microseconds(void);
static void app_choose(struct notes_app *app, unsigned mode, unsigned purpose);
static void app_chooser_done(void *data, struct kl_file_chooser *chooser, unsigned result, const char *path, size_t filter);
static void app_chosen(struct notes_app *app);
static void app_open_file(struct notes_app *app, const char *path);
static void app_save_as(struct notes_app *app, const char *path);
static void app_save_clean(struct notes_app *app, const char *path);
static int app_clean_told(void);
static void app_place_page(struct notes_app *app);
static void app_appearance_changed(void *data, unsigned appearance);
static const char *app_tool_name(unsigned tool);
static uint64_t app_look(const struct notes_app *app);
static void app_count_edits(const struct notes_app *app, size_t *edits, size_t *pages);
static void app_select_press(struct notes_app *app, float x, float y, uint32_t time_ms);
static void app_select_motion(struct notes_app *app, float x, float y);
static void app_select_release(struct notes_app *app);
static void app_select(struct notes_app *app, size_t index, int logged);
static void app_deselect(struct notes_app *app);
static void app_edit_object(const struct notes_undo *entry, struct notes_edit *which);
static void app_reselect(struct notes_app *app, const struct notes_edit *which);
static int app_selected_state(struct notes_app *app, struct notes_edit *state);
static void app_drag_preview(struct notes_app *app, struct pdf_page_editor *editor);
static void app_apply_map(struct notes_app *app, const double map[6]);
static void app_delete_object(struct notes_app *app);
static void app_reset_object(struct notes_app *app);
static void app_put_image(struct notes_app *app, const char *path, unsigned purpose);
static void app_selection_draw(struct notes_app *app, const struct notes_view *view);
static void app_map_multiply(const double left[6], const double right[6], double product[6]);
static void app_map_point(const double map[6], double x, double y, double *mapped_x, double *mapped_y);
static void app_text_press(struct notes_app *app, float x, float y);
static void app_text_release(struct notes_app *app, const struct notes_input *input);
static void app_box_edit(struct notes_app *app, size_t index);
static void app_box_new(struct notes_app *app, float x, float y, float width);
static void app_box_start(struct notes_app *app, unsigned shape, const struct kl_rect *rect, const char *text);
static void app_box_place(struct notes_app *app, const double quad[8], int width, int height, struct kl_rect *rect);
static void app_box_rect(struct notes_app *app, struct kl_rect *rect);
static int app_box_follow(struct notes_app *app);
static void app_box_frame(struct notes_app *app);
static int app_box_commit(struct notes_app *app);
static int app_box_try(struct notes_app *app, const struct notes_edit *state, unsigned *result);
static void app_box_close(struct notes_app *app);
static void app_box_preview(struct notes_app *app, struct pdf_page_editor *editor);
static int app_box_keeps(uint32_t action);
static void app_text_font(struct notes_app *app);
static void app_text_size(struct notes_app *app, float step);
static unsigned app_next_font(unsigned font, int original);
static const char *app_font_name(unsigned font);
static const char *app_font_label(unsigned font);
static unsigned app_color_index(uint32_t color);
static size_t app_characters(const char *text);
static uint64_t app_buttons_signature(const struct notes_ui *ui);

/*
 * Runs Notes.
 */
int
main(
	int argc,
	char **argv)
{
	static struct notes_app app;
	const char *file;
	unsigned long value;
	uint64_t now;
	uint32_t width;
	uint32_t height;
	unsigned box_keys;
	unsigned box_texts;
	unsigned index;
	int fullscreen;
	int is_fullscreen;
	int is_width;
	int is_height;
	int is_timeout;
	int timeout;
	int status;
	int error;
	int arg;

	/* The options and the file. */
	file = NULL;
	fullscreen = 0;
	width = MAIN_WIDTH;
	height = MAIN_HEIGHT;
	for (arg = 1; arg < argc; arg++) {
		/* Which option the argument is, when it is one. */
		is_fullscreen = strcmp(argv[arg], "--fullscreen");
		is_width = strncmp(argv[arg], "--width=", 8U);
		is_height = strncmp(argv[arg], "--height=", 9U);
		is_timeout = strncmp(argv[arg], "--timeout-s=", 12U);

		/* Each option takes its value; anything else is the file. */
		if (is_fullscreen == 0) {
			fullscreen = 1;
		} else if (is_width == 0) {
			value = strtoul(argv[arg] + 8, NULL, 10);
			if (value >= 320U && value <= 8192U)
				width = (uint32_t)value;
		} else if (is_height == 0) {
			value = strtoul(argv[arg] + 9, NULL, 10);
			if (value >= 240U && value <= 8192U)
				height = (uint32_t)value;
		} else if (is_timeout == 0) {
			value = strtoul(argv[arg] + 12, NULL, 10);
			if (value != 0U)
				app.deadline = notes_clock() + (uint64_t)value * 1000U;
		} else if (argv[arg][0] == '-') {
			fprintf(stderr, "usage: notes [--fullscreen] [--width=W] [--height=H] [--timeout-s=S] [FILE.pdf]\n");
			return 2;
		} else {
			file = argv[arg];
		}
	}

	/* The notebook: recovered from a journal, or new. */
	app.tool = NOTES_ACTION_PEN;
	app.selected = MAIN_NONE;
	app.select_press_object = MAIN_NONE;
	app.width = 1U;
	app.text_font = PDF_EDIT_FONT_SANS;
	app.text_size = MAIN_SIZE_DEFAULT;
	error = app_start_document(&app, file);
	if (error != 0) {
		fprintf(stderr, "notes: cannot start a notebook: %s\n", strerror(error));
		return 1;
	}

	/* The window. */
	status = notes_window_open(&app.window, width, height, fullscreen);
	if (status != 0) {
		fprintf(stderr, "notes: cannot open a window: %s\n", strerror(errno));
		return 1;
	}

	/* Its title names the file. */
	app_set_title(&app);

	/* The desktop's appearance: the desk and the toolbar follow it (light under a compositor without it). */
	error = kl_appearance_open(app.window.display, app_appearance_changed, &app, &app.appearance);
	if (error != 0)
		printf("NOTES APPEARANCE none error=%d\n", error);

	/* The menus; without the System Menu the keys still work. */
	error = notes_menu_open(&app.window);
	if (error != 0)
		printf("NOTES MENU none error=%d\n", error);

	/* The drawing. */
	error = (int)notes_renderer_open(&app.renderer, &app.window);
	if (error != (int)VK_SUCCESS) {
		fprintf(stderr, "notes: %s failed (%d)\n", app.renderer.operation, error);
		notes_renderer_close(&app.renderer);
		notes_window_close(&app.window);
		return 1;
	}

	/* The toolbar's font; the buttons work without it. */
	error = notes_ui_open(&app.ui, MAIN_FONT);
	if (error != 0)
		printf("NOTES FONT none error=%d\n", error);

	/* The fingers; without memory for them they do nothing. */
	app.touch_due = -1;
	error = notes_touch_open(&app.touch);
	if (error != 0)
		printf("NOTES TOUCH none error=%d\n", error);

	/* The page's first place, before any input needs it. */
	app_place_page(&app);

	/* The tests' first line. */
	printf("NOTES START width=%u height=%u fullscreen=%d pages=%lu strokes=%lu path=%s\n",
	       app.window.width, app.window.height, app.window.fullscreen,
	       (unsigned long)app.document.page_count, (unsigned long)notes_document_stroke_total(&app.document), app.path);
	fflush(stdout);
	app.redraw = 1;
	app.toolbar_dirty = 1;

	/* The main loop: wait, take what arrived, save when due, draw when needed. */
	while (!app.quit) {
		/* Waits for the compositor until the next thing that is due. */
		now = notes_clock();
		timeout = app_timeout(&app, now);
		status = notes_window_dispatch(&app.window, timeout);
		if (status != 0) {
			printf("NOTES DISCONNECTED\n");
			break;
		}

		/* A new size remakes the swapchain and the toolbar. */
		if (app.window.resized) {
			app.window.resized = 0;
			error = (int)notes_renderer_resize(&app.renderer, app.window.width, app.window.height);
			if (error != (int)VK_SUCCESS) {
				fprintf(stderr, "notes: %s failed (%d)\n", app.renderer.operation, error);
				break;
			}

			/* Everything is drawn again at the new size (the text box on an overlay of the size). */
			app.redraw = 1;
			app.toolbar_dirty = 1;
			if (app.box.open)
				app_box_frame(&app);
		}

		/*
		 * The text box's inputs (the keys and an input method's text
		 * counted for the tests' line), then its frame, which takes them
		 * (ws175-p008); keys still waiting after a frame take the next
		 * frames without new input (T1-324: the last letters and Esc of
		 * a quick typing waited for the next event).
		 */
		if (app.window.box_count != 0U || (app.box.open && app.box.again)) {
			box_keys = 0;
			box_texts = 0;
			for (index = 0; index < app.window.box_count; index++) {
				notes_box_input(&app.box, &app.window.box_events[index]);
				if (app.window.box_events[index].kind == KL_WINDOW_KEY && app.window.box_events[index].pressed)
					box_keys++;
				if (app.window.box_events[index].kind == KL_WINDOW_TEXT_COMMIT || app.window.box_events[index].kind == KL_WINDOW_TEXT_PREEDIT)
					box_texts++;
			}

			/* Taken; logged when the keys or the text came. */
			app.window.box_count = 0;
			if (box_keys != 0U || box_texts != 0U) {
				printf("NOTES TEXT box input keys=%u texts=%u open=%d\n", box_keys, box_texts, app.box.open);
				fflush(stdout);
			}

			/*
			 * The frame, and more while keys typed faster than the widget
			 * takes them wait (one a frame), so that the box catches up
			 * before the window is drawn (T1-331: the screenshot showed
			 * "ZE" of "ZEBRA" while the rest waited for later frames).
			 */
			if (app.box.open)
				app_box_frame(&app);
			for (index = 0; index < MAIN_BOX_CATCH_UP && app.box.open && app.box.again; index++)
				app_box_frame(&app);
		}

		/* The menus' choices. */
		for (index = 0; index < app.window.action_count; index++)
			app_action(&app, app.window.actions[index]);
		app.window.action_count = 0;

		/* The keys. */
		for (index = 0; index < app.window.key_count; index++)
			app_key(&app, &app.window.keys[index]);
		app.window.key_count = 0;

		/* The pointer's and the pen's events. */
		for (index = 0; index < app.window.input_count; index++)
			app_input(&app, &app.window.inputs[index]);
		app.window.input_count = 0;

		/* What the file chooser answered (ws128-p002). */
		if (app.chosen_ready)
			app_chosen(&app);

		/* The fingers' events, and where they put the page. */
		for (index = 0; index < app.window.touch_count; index++)
			notes_touch_event(&app.touch, &app.window.touches[index]);
		app.window.touch_count = 0;
		app_touch(&app);

		/* The autosave, once the notebook has been still long enough and nothing is being drawn. */
		now = notes_clock();
		if (app.document.dirty &&
		    app.contact == MAIN_CONTACT_NONE &&
		    now >= app.changed_at + NOTES_AUTOSAVE_IDLE_MS)
			(void)app_save(&app, "autosave");

		/* The words typed in the text box are shown on the page once the preview's time comes. */
		if (app.box_changed && now >= app.box_previewed_at + MAIN_DRAG_PREVIEW_MS)
			app.redraw = 1;

		/* A status whose time is up goes. */
		if (app.status_until != 0U && now >= app.status_until) {
			app.status[0] = '\0';
			app.status_until = 0;
			app.toolbar_dirty = 1;
			app.redraw = 1;
		}

		/* The compositor's close, or the end of the run. */
		if (app.window.closed)
			app.quit = 1;
		if (app.deadline != 0U && now >= app.deadline)
			app.quit = 1;

		/* A new frame when something changed. */
		if (app.redraw && !app.quit)
			app_draw(&app);
	}

	/* A stroke still being drawn is kept, and so are the words of the text box when the editor takes them. */
	if (app.live != NULL)
		app_end_contact(&app, NULL);
	if (app.box.open) {
		status = app_box_commit(&app);
		if (!status)
			app_box_close(&app);
	}

	/* The notebook saved when it changed. */
	if (app.document.dirty)
		(void)app_save(&app, "close");

	/* The tests' last line. */
	printf("NOTES EXIT pages=%lu strokes=%lu dirty=%d\n", (unsigned long)app.document.page_count,
	       (unsigned long)notes_document_stroke_total(&app.document), app.document.dirty);
	fflush(stdout);

	/* Everything goes; the journal stays only when the last save failed. */
	kl_file_chooser_destroy(app.chooser);
	app.chooser = NULL;
	notes_touch_close(&app.touch);
	notes_box_free(&app.box);
	notes_ui_close(&app.ui);
	notes_frame_free(&app.frame);
	notes_frame_free(&app.page_frame);
	pdf_display_list_destroy(app.background_list);
	notes_renderer_close(&app.renderer);
	kl_appearance_close(app.appearance);
	notes_window_close(&app.window);
	notes_journal_destroy(app.document.journal);
	app.document.journal = NULL;
	notes_document_free(&app.document);

	/* Succeeded: Notes ran to its end. */
	return 0;
}

/*
 * Starts the notebook: the given file's journal, the most recent journal
 * when no file is given, or a new notebook.
 */
static int
app_start_document(
	struct notes_app *app,
	const char *file)
{
	char journal_path[MAIN_PATH_MAX];
	char recovered_path[MAIN_PATH_MAX];
	char folder[MAIN_PATH_MAX];
	struct stat status;
	const char *slash;
	const char *cwd;
	size_t edited_pages;
	size_t records;
	size_t edits;
	unsigned opened;
	const char *kind;
	int written;
	int exists;
	int found;
	int error;

	/* The file's absolute path, when a file is given. */
	app->path[0] = '\0';
	if (file != NULL) {
		if (file[0] == '/') {
			written = snprintf(app->path, sizeof(app->path), "%s", file);
		} else {
			/* A relative path is under the current folder. */
			cwd = getcwd(folder, sizeof(folder));
			if (cwd == NULL)
				return errno;
			written = snprintf(app->path, sizeof(app->path), "%s/%s", folder, file);
		}

		/* A path that did not fit is refused. */
		if (written < 0 || (size_t)written >= sizeof(app->path))
			return ENAMETOOLONG;
	}

	/* The journal to recover: the file's, when it is there, or the most recent one. */
	found = 0;
	if (app->path[0] != '\0') {
		error = notes_journal_path(app->path, journal_path, sizeof(journal_path));
		if (error == 0) {
			exists = stat(journal_path, &status);
			if (exists == 0)
				found = 1;
		}
	} else {
		error = notes_journal_newest(journal_path, sizeof(journal_path));
		if (error == 0)
			found = 1;
	}

	/* A journal rebuilds the notebook as it was when Notes last ran; one written on a PDF gets that PDF back from the file. */
	if (found) {
		error = notes_journal_recover(journal_path, &app->document, recovered_path, sizeof(recovered_path), &records);
		if (error == 0) {
			error = notes_attach_base(recovered_path, &app->document);
			if (error != 0) {
				printf("NOTES RECOVER base error=%d path=%s\n", error, recovered_path);
				notes_document_free(&app->document);
			}
		}

		/*
		 * The edits of the PDF's objects must still apply (ws175-p007,
		 * design.md [N8]); when one does not, the journal is set aside, not
		 * replayed into a document that would lose it at the next save.
		 */
		if (error == 0) {
			error = notes_document_check_edits(&app->document);
			if (error != 0) {
				printf("NOTES RECOVER failed reason=key journal=%s\n", journal_path);
				(void)notes_journal_set_aside(journal_path);
				notes_document_free(&app->document);
			}
		}

		/* The recovered notebook, or the failure that leaves the file to be opened instead. */
		if (error == 0) {
			memcpy(app->path, recovered_path, strlen(recovered_path) + 1U);
			printf("NOTES RECOVER records=%lu pages=%lu strokes=%lu path=%s\n", (unsigned long)records,
			       (unsigned long)app->document.page_count, (unsigned long)notes_document_stroke_total(&app->document), app->path);
			(void)snprintf(app->status, sizeof(app->status), "Recovered %lu strokes", (unsigned long)notes_document_stroke_total(&app->document));
			app->status_until = notes_clock() + MAIN_STATUS_MS;
		} else {
			printf("NOTES RECOVER failed error=%d journal=%s\n", error, journal_path);
			found = 0;
		}
	}

	/* Whether the file is there, when no journal was recovered. */
	exists = -1;
	if (!found && app->path[0] != '\0')
		exists = stat(app->path, &status);

	/* A file that is there but has no journal is opened: a notebook from its edit data, another PDF as the background. */
	if (exists == 0) {
		error = notes_open_pdf(app->path, &app->document, &opened);
		if (error == 0) {
			found = 1;
			kind = app_opened_name(opened);
			printf("NOTES OPEN pages=%lu strokes=%lu kind=%s path=%s\n", (unsigned long)app->document.page_count,
			       (unsigned long)notes_document_stroke_total(&app->document), kind, app->path);
			app_count_edits(app, &edits, &edited_pages);
			printf("NOTES EDITS opened edits=%lu edited_pages=%lu rebased=%d\n", (unsigned long)edits, (unsigned long)edited_pages,
			       opened == NOTES_OPENED_REBASED);

			/* Writing on another program's PDF is said once. */
			if (opened == NOTES_OPENED_FOREIGN)
				(void)snprintf(app->status, sizeof(app->status), "Writing on the PDF; it stays as it was under your ink");
			else if (opened == NOTES_OPENED_CHANGED)
				(void)snprintf(app->status, sizeof(app->status), "Pages changed by another program are now background");
			else if (opened == NOTES_OPENED_REBASED)
				(void)snprintf(app->status, sizeof(app->status), "The edits no longer match this PDF; it opened as it looks");
			if (opened == NOTES_OPENED_FOREIGN || opened == NOTES_OPENED_CHANGED || opened == NOTES_OPENED_REBASED)
				app->status_until = notes_clock() + MAIN_NOTICE_MS;
		} else {
			/* A file Notes cannot write on is left as it is, and a new notebook starts. */
			printf("NOTES OPEN failed error=%d path=%s\n", error, app->path);
			if (error == EACCES)
				(void)snprintf(app->status, sizeof(app->status), "The PDF is encrypted; Notes cannot write on it. Started a new note");
			else if (error == EPERM)
				(void)snprintf(app->status, sizeof(app->status), "The PDF is signed; Notes does not write on it. Started a new note");
			else if (error == ENOTSUP)
				(void)snprintf(app->status, sizeof(app->status), "The PDF uses features Notes cannot read yet. Started a new note");
			else
				(void)snprintf(app->status, sizeof(app->status), "Cannot open the PDF; started a new note");
			app->status_until = notes_clock() + MAIN_NOTICE_MS;
			app->path[0] = '\0';
		}
	}

	/* Otherwise a new notebook, at the given path or a new one. */
	if (!found) {
		error = notes_document_init(&app->document, app_unix_ms());
		if (error != 0)
			return error;
		if (app->path[0] == '\0') {
			error = app_new_path(app->path, sizeof(app->path));
			if (error != 0)
				return error;
		}
	}

	/* The journal every change goes to. */
	app->document.journal = notes_journal_create(app->path);
	if (app->document.journal == NULL)
		printf("NOTES JOURNAL none error=%d\n", errno);

	/* The name shown in the title: the file's last part. */
	slash = strrchr(app->path, '/');
	app->name = app->path;
	if (slash != NULL)
		app->name = slash + 1;

	/* Succeeded: the notebook is ready. */
	app->changed_at = notes_clock();
	return 0;
}

/* Names what notes_open_pdf() found, for the tests' line. */
static const char *
app_opened_name(
	unsigned opened)
{
	/* Each kind by its word. */
	switch (opened) {
	case NOTES_OPENED_ANNOTATED:
		return "annotated";
	case NOTES_OPENED_FOREIGN:
		return "foreign";
	case NOTES_OPENED_CHANGED:
		return "changed";
	case NOTES_OPENED_REBASED:
		return "rebased";
	default:
		break;
	}

	/* A notebook Notes saved. */
	return "notes";
}

/*
 * Draws the background picture of a page of the PDF the notebook writes on
 * at a scale, into the renderer's background image at the page picture's
 * size, and tells whether it is there to show (0 when the page cannot be
 * drawn).
 *
 * libpdf interprets the base's page once into a display list, which is kept
 * while the page is shown, and rasterizes it on the CPU over white; the
 * picture is drawn again only for another page, scale or size.
 *
 * ws175-p008: a page with edits of its objects (or one whose object a drag
 * moves) is drawn by its editor (edit.c), a page of Notes' own too (its
 * inserted images); the drawing is made again when the look (app_look)
 * changed.
 */
static int
app_background(
	struct notes_app *app,
	const struct notes_page *page,
	float scale)
{
	struct pdf_display_list *list;
	struct pdf_page_editor *editor;
	unsigned char *target;
	uint32_t *pixels;
	uint64_t look;
	uint64_t started;
	uint64_t rendered;
	uint64_t rasterized;
	uint32_t width;
	uint32_t height;
	size_t pitch;
	size_t count;
	size_t index;
	size_t row;
	int dragged;
	int edited;
	int error;

	/* Nothing to draw without the PDF, or (a page of Notes' own) without edits, a drag or the text box's words. */
	dragged = 0;
	if (app->drag != MAIN_DRAG_NONE && app->drag_previews > 0U)
		dragged = 1;
	if (app->box_previewing)
		dragged = 1;
	edited = 0;
	if (page->edit_count > 0U || dragged)
		edited = 1;
	if (!edited && (app->document.base == NULL || page->origin != NOTES_ORIGIN_OVER))
		return 0;
	look = app_look(app);

	/* The picture's size, the page picture's. */
	width = app->renderer.page_width;
	height = app->renderer.page_height;
	if (width == 0U || height == 0U)
		return 0;

	/* A picture drawn for the page at the scale, the size and the look stands. */
	if (app->background_page == page &&
	    app->background_look == look &&
	    app->background_scale == scale &&
	    app->background_width == width &&
	    app->background_height == height &&
	    app->renderer.background_width == width &&
	    app->renderer.background_height == height)
		return 1;

	/* The page as a display list -- the base's page, or the page's editor's --, made once while the page and its look stay. */
	started = app_microseconds();
	if (app->background_list == NULL || app->background_list_page != page || app->background_source != page->source ||
	    app->background_look != look) {
		pdf_display_list_destroy(app->background_list);
		app->background_list = NULL;
		app->background_failed = 0;
		app->background_source = page->source;
		app->background_list_page = page;
		app->background_look = look;
		if (edited) {
			error = notes_page_editor(&app->document, app->page, &editor);
			if (error == 0 && app->drag != MAIN_DRAG_NONE && app->drag_previews > 0U)
				app_drag_preview(app, editor);
			if (error == 0 && app->box_previewing)
				app_box_preview(app, editor);
			if (error == 0)
				error = pdf_page_editor_render(editor, (size_t)-1, &list);
		} else {
			error = pdf_page_render(app->document.base, page->source, &list);
		}

		/* A page that cannot be drawn stays white. */
		if (error != 0) {
			printf("NOTES BACKGROUND failed source=%lu error=%d\n", (unsigned long)page->source, error);
			fflush(stdout);
			app->background_failed = 1;
			return 0;
		}

		/* The drawing is kept while the page is shown. */
		app->background_list = list;
	}

	/* A page that could not be drawn stays white. */
	if (app->background_failed)
		return 0;
	rendered = app_microseconds();

	/* The picture on the CPU: white, then the page over it. */
	count = (size_t)width * (size_t)height;
	pixels = malloc(count * sizeof(*pixels));
	if (pixels == NULL)
		return 0;
	for (index = 0; index < count; index++)
		pixels[index] = 0xffffffffU;
	error = pdf_display_list_rasterize(app->background_list, pixels, width, width, height, (double)scale, 0.0, 0.0);
	if (error != 0) {
		printf("NOTES BACKGROUND raster failed source=%lu error=%d\n", (unsigned long)page->source, error);
		free(pixels);
		return 0;
	}

	/* When the picture was drawn, for the log. */
	rasterized = app_microseconds();

	/* The renderer's image of the size, which the picture's rows are copied into. */
	error = (int)notes_renderer_background(&app->renderer, width, height, &target, &pitch);
	if (error != (int)VK_SUCCESS || target == NULL) {
		printf("NOTES BACKGROUND image failed error=%d\n", error);
		free(pixels);
		return 0;
	}

	/* Copies each row into the image, whose rows may be longer. */
	for (row = 0; row < height; row++)
		memcpy(target + row * pitch, pixels + row * width, (size_t)width * sizeof(*pixels));
	free(pixels);

	/* The picture stands for the page at the scale (logged for the tests). */
	app->background_page = page;
	app->background_scale = scale;
	app->background_width = width;
	app->background_height = height;
	printf("NOTES BACKGROUND source=%lu items=%lu flags=%u size=%ux%u render_us=%lu raster_us=%lu\n", (unsigned long)page->source,
	       (unsigned long)app->background_list->count, app->background_list->flags, width, height,
	       (unsigned long)(rendered - started), (unsigned long)(rasterized - rendered));
	fflush(stdout);

	/* Succeeded: the background is in the image. */
	return 1;
}

/* Writes the path of a new notebook: ~/Documents/Notes/note-YYYYMMDD-HHMMSS.pdf. */
static int
app_new_path(
	char *path,
	size_t size)
{
	struct tm local;
	struct tm *converted;
	const char *home;
	time_t now;
	int written;

	/* The home directory, or /tmp without one. */
	home = getenv("HOME");
	if (home == NULL || home[0] != '/')
		home = "/tmp";

	/* The time the notebook was made names it (the epoch when the local time cannot be told). */
	now = time(NULL);
	converted = localtime_r(&now, &local);
	if (converted == NULL)
		memset(&local, 0, sizeof(local));
	written = snprintf(path, size, "%s/Documents/Notes/note-%04d%02d%02d-%02d%02d%02d.pdf", home,
			   local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
	if (written < 0 || (size_t)written >= size)
		return ENAMETOOLONG;

	/* Succeeded: the path (its folder is made at the first save). */
	return 0;
}

/* Makes the folders of a file's path that are missing. */
static int
app_make_folders(
	const char *path)
{
	char folder[MAIN_PATH_MAX];
	char *slash;
	size_t length;
	int status;

	/* The path, to cut at each slash. */
	length = strlen(path);
	if (length >= sizeof(folder))
		return ENAMETOOLONG;
	memcpy(folder, path, length + 1U);

	/* Each folder from the root down to the file's; one that exists is passed. */
	for (slash = folder + 1; *slash != '\0'; slash++) {
		if (*slash != '/')
			continue;
		*slash = '\0';
		status = mkdir(folder, 0755);
		*slash = '/';
		if (status != 0 && errno != EEXIST)
			return errno;
	}

	/* Succeeded: the file's folder is there. */
	return 0;
}

/* Sets the window's title: the file's name, a dash and "Notes", as PDF Viewer names its window (ws035-p122). */
static void
app_set_title(
	struct notes_app *app)
{
	char title[MAIN_PATH_MAX];

	/* The title with the file's name. */
	(void)snprintf(title, sizeof(title), "%s \xe2\x80\x94 Notes", app->name);
	notes_window_set_title(&app->window, title);
}

/* Shows a status on the toolbar for a while. */
static void
app_status(
	struct notes_app *app,
	const char *text)
{
	/* The text and its time. */
	(void)snprintf(app->status, sizeof(app->status), "%s", text);
	app->status_until = notes_clock() + MAIN_STATUS_MS;
	app->toolbar_dirty = 1;
	app->redraw = 1;
}

/* Gathers what the toolbar and the menus show. */
static void
app_state(
	const struct notes_app *app,
	struct notes_ui_state *state)
{
	const struct notes_page *page;

	/* The tool, colour, width, pages, history, changes and fullscreen. */
	memset(state, 0, sizeof(*state));
	state->tool = app->tool;
	state->color = app->color;
	state->width = app->width;
	state->page = app->page;
	state->page_count = app->document.page_count;
	state->dirty = app->document.dirty;
	state->erase_parts = app->erase_parts;

	/* Undo while a change stands, redo while one was taken back. */
	if (app->document.undo_done > 0U)
		state->can_undo = 1;
	if (app->document.undo_done < app->document.undo_count)
		state->can_redo = 1;

	/* The window's state and the status line. */
	state->fullscreen = app->window.fullscreen;
	state->finger_write = app->finger_write;
	state->status = app->status;

	/* The Select tool's: an image inserted on a page drawn over the PDF, or on one of Notes' own; the chosen object's (ws175-p008). */
	page = app->document.pages[app->page];
	if (page->origin != NOTES_ORIGIN_OVER || app->document.base != NULL)
		state->can_insert = 1;
	if (app->selected != MAIN_NONE) {
		state->selected = 1;
		if (app->selected_kind == PDF_EDIT_IMAGE)
			state->can_replace = 1;
		if (app->selected_edited && !app->selected_inserted)
			state->can_reset = 1;
	}

	/* A clean copy saved this session can be opened. */
	if (app->clean_path[0] != '\0')
		state->can_open_clean = 1;

	/* A line of text chosen: its words in the box unless they cannot change, its font and size. */
	if (app->selected != MAIN_NONE && app->selected_kind == PDF_EDIT_TEXT) {
		state->text_selected = 1;
		state->can_edit_text = !app->selected_fixed;
		state->font_label = app_font_label(app->selected_font);
		state->text_size = app->selected_size;
	}

	/* The Text tool's font and size: the box's while it is open (a line's size is its own), or the next words'. */
	if (app->tool == NOTES_ACTION_TEXT) {
		state->font_label = app_font_label(app->text_font);
		state->text_size = app->text_size;
		if (app->box.open) {
			state->font_label = app_font_label(app->box_font);
			state->text_size = app->box_size;
		}
	}
}

/* Carries out an action of the toolbar, a menu or a key. */
static void
app_action(
	struct notes_app *app,
	uint32_t action)
{
	struct notes_edit which;
	size_t page;
	int closed;
	int keeps;
	int edit;
	int error;

	/*
	 * The text box open (ws175-p008): an undo or a redo is the box's own,
	 * one change a key (ws177-p013: the menu's Ctrl+Z and Ctrl+Shift+Z go to
	 * its widget's history); another action than the box's own (its font,
	 * size and colour, the window's) closes it first, keeping its words --
	 * unless the editor refuses them, when the box stays and the action is
	 * not carried out (Close ends Notes all the same).
	 */
	if (app->box.open && action == NOTES_ACTION_UNDO) {
		notes_box_key(&app->box, MAIN_KEY_Z, KL_MOD_CTRL);
		app_box_frame(app);
		return;
	}

	/* A redo does the box's change taken back again. */
	if (app->box.open && action == NOTES_ACTION_REDO) {
		notes_box_key(&app->box, MAIN_KEY_Z, KL_MOD_CTRL | KL_MOD_SHIFT);
		app_box_frame(app);
		return;
	}

	/* Other actions close the box first. */
	keeps = app_box_keeps(action);
	if (app->box.open && !keeps) {
		closed = app_box_commit(app);
		if (!closed && action != NOTES_ACTION_CLOSE)
			return;
		if (!closed)
			app_box_close(app);
	}

	/* A colour or a width chooses by its index. */
	if (action >= NOTES_ACTION_COLOR && action < NOTES_ACTION_COLOR + NOTES_COLORS) {
		app->color = action - NOTES_ACTION_COLOR;
		if (app->tool == NOTES_ACTION_ERASER)
			app->tool = NOTES_ACTION_PEN;
		app->toolbar_dirty = 1;
		app->redraw = 1;
		return;
	}

	/* A width, by its index. */
	if (action >= NOTES_ACTION_WIDTH && action < NOTES_ACTION_WIDTH + NOTES_WIDTHS) {
		app->width = action - NOTES_ACTION_WIDTH;
		app->toolbar_dirty = 1;
		app->redraw = 1;
		return;
	}

	/* The other actions. */
	switch (action) {
	case NOTES_ACTION_PEN:
	case NOTES_ACTION_HIGHLIGHTER:
	case NOTES_ACTION_SELECT:
	case NOTES_ACTION_TEXT:
		/* The tool (another than Select lets the chosen object go). */
		app->tool = action;
		if (action != NOTES_ACTION_SELECT)
			app_deselect(app);
		printf("NOTES TOOL %u name=%s\n", action, app_tool_name(action));
		break;
	case NOTES_ACTION_ERASER:
		/* The eraser; chosen again, it switches between whole strokes and parts (design-input-notes.md section 5.2). */
		if (app->tool == NOTES_ACTION_ERASER)
			app->erase_parts = !app->erase_parts;
		app->tool = action;
		app_deselect(app);
		printf("NOTES TOOL %u parts=%d name=eraser\n", action, app->erase_parts);
		break;
	case NOTES_ACTION_INSERT_IMAGE:
		/* An image file to insert on the page, chosen in the file chooser (ws175-p008). */
		app_choose(app, KL_FILE_CHOOSER_OPEN, MAIN_CHOOSE_INSERT);
		break;
	case NOTES_ACTION_REPLACE_IMAGE:
		/* An image file for the chosen object. */
		if (app->selected == MAIN_NONE)
			break;
		app_choose(app, KL_FILE_CHOOSER_OPEN, MAIN_CHOOSE_REPLACE);
		break;
	case NOTES_ACTION_DELETE_OBJECT:
		/* The chosen object deleted. */
		app_delete_object(app);
		break;
	case NOTES_ACTION_RESET_OBJECT:
		/* The chosen object as the page has it. */
		app_reset_object(app);
		break;
	case NOTES_ACTION_EDIT_TEXT:
		/* The chosen line's words in the text box (ws175-p008). */
		if (app->selected != MAIN_NONE && app->selected_kind == PDF_EDIT_TEXT)
			app_box_edit(app, app->selected);
		break;
	case NOTES_ACTION_FONT:
		/* The next font of the box, the chosen text or the Text tool's. */
		app_text_font(app);
		break;
	case NOTES_ACTION_SIZE_DOWN:
		/* Smaller. */
		app_text_size(app, -MAIN_SIZE_STEP);
		break;
	case NOTES_ACTION_SIZE_UP:
		/* Larger. */
		app_text_size(app, MAIN_SIZE_STEP);
		break;
	case NOTES_ACTION_UNDO:
		/* The last change is taken back, and its page shown (the chosen object let go). */
		edit = 0;
		if (app->document.undo_done > 0U && app->document.undo[app->document.undo_done - 1U].kind == NOTES_UNDO_EDIT_OBJECT) {
			edit = 1;
			app_edit_object(&app->document.undo[app->document.undo_done - 1U], &which);
		}

		/* The chosen object let go (chosen again below). */
		app_deselect(app);
		error = notes_document_undo(&app->document, &page);
		if (error != 0)
			break;
		if (page >= app->document.page_count)
			page = app->document.page_count - 1U;
		app->page = page;
		printf("NOTES UNDO page=%lu strokes=%lu pages=%lu\n", (unsigned long)app->page,
		       (unsigned long)app->document.pages[app->page]->stroke_count, (unsigned long)app->document.page_count);
		if (edit) {
			printf("NOTES EDIT undo page=%lu edits=%lu\n", (unsigned long)app->page, (unsigned long)app->document.pages[app->page]->edit_count);
			app_reselect(app, &which);
		}

		/* The notebook changed. */
		app_changed(app);
		break;
	case NOTES_ACTION_REDO:
		/* The last change taken back is made again, and its page shown (the chosen object let go). */
		edit = 0;
		if (app->document.undo_done < app->document.undo_count && app->document.undo[app->document.undo_done].kind == NOTES_UNDO_EDIT_OBJECT) {
			edit = 1;
			app_edit_object(&app->document.undo[app->document.undo_done], &which);
		}

		/* The chosen object let go (chosen again below). */
		app_deselect(app);
		error = notes_document_redo(&app->document, &page);
		if (error != 0)
			break;
		if (page >= app->document.page_count)
			page = app->document.page_count - 1U;
		app->page = page;
		printf("NOTES REDO page=%lu strokes=%lu pages=%lu\n", (unsigned long)app->page,
		       (unsigned long)app->document.pages[app->page]->stroke_count, (unsigned long)app->document.page_count);
		if (edit) {
			printf("NOTES EDIT redo page=%lu edits=%lu\n", (unsigned long)app->page, (unsigned long)app->document.pages[app->page]->edit_count);
			app_reselect(app, &which);
		}

		/* The notebook changed. */
		app_changed(app);
		break;
	case NOTES_ACTION_PREVIOUS_PAGE:
		/* The page before, when there is one (the chosen object let go). */
		app_deselect(app);
		if (app->page > 0U)
			app->page--;
		printf("NOTES PAGE current=%lu count=%lu\n", (unsigned long)app->page, (unsigned long)app->document.page_count);
		break;
	case NOTES_ACTION_NEXT_PAGE:
		/* The page after, when there is one (the chosen object let go). */
		app_deselect(app);
		if (app->page + 1U < app->document.page_count)
			app->page++;
		printf("NOTES PAGE current=%lu count=%lu\n", (unsigned long)app->page, (unsigned long)app->document.page_count);
		break;
	case NOTES_ACTION_NEW_PAGE:
		/* A blank page after the current one, shown. */
		app_deselect(app);
		error = notes_document_add_page(&app->document, app->page + 1U);
		if (error != 0) {
			app_status(app, "Could not add a page");
			break;
		}

		/* The new page is shown. */
		app->page++;
		printf("NOTES PAGE current=%lu count=%lu new\n", (unsigned long)app->page, (unsigned long)app->document.page_count);
		app_changed(app);
		break;
	case NOTES_ACTION_SAVE:
		/* Saved now, even when nothing changed. */
		(void)app_save(app, "request");
		break;
	case NOTES_ACTION_OPEN:
		/* Another PDF, chosen in libkeiland's file chooser (ws128-p002). */
		app_choose(app, KL_FILE_CHOOSER_OPEN, MAIN_CHOOSE_DOCUMENT);
		break;
	case NOTES_ACTION_SAVE_AS:
		/* The notebook as another file, chosen in the file chooser (ws128-p002). */
		app_choose(app, KL_FILE_CHOOSER_SAVE, MAIN_CHOOSE_DOCUMENT);
		break;
	case NOTES_ACTION_SAVE_CLEAN:
		/* A clean copy as another file, chosen in the file chooser (ws175-p009). */
		app_choose(app, KL_FILE_CHOOSER_SAVE, MAIN_CHOOSE_CLEAN);
		break;
	case NOTES_ACTION_OPEN_CLEAN:
		/* The clean copy saved last, in place of the notebook (ws177-p011). */
		if (app->clean_path[0] != '\0')
			app_open_file(app, app->clean_path);
		break;
	case NOTES_ACTION_CLOSE:
		/* The main loop saves and ends. */
		app->quit = 1;
		break;
	case NOTES_ACTION_FULLSCREEN:
		/* Fullscreen on or off; the configure that follows redraws. */
		notes_window_set_fullscreen(&app->window, !app->window.fullscreen);
		break;
	case NOTES_ACTION_LEAVE_FULLSCREEN:
		/* Back to a window, when fullscreen. */
		if (app->window.fullscreen)
			notes_window_set_fullscreen(&app->window, 0);
		break;
	case NOTES_ACTION_FINGER:
		/* One finger writes, or scrolls again (a line under way is kept); the status says which. */
		app->finger_write = !app->finger_write;
		notes_touch_write_mode(&app->touch, app->finger_write, notes_touch_clock());
		app_finger(app);
		printf("NOTES FINGER write=%d\n", app->finger_write);
		if (app->finger_write) {
			app_status(app, "One finger writes, two fingers scroll");
		} else {
			app_status(app, "Fingers scroll and zoom");
		}

		/* Nothing else. */
		break;
	default:
		break;
	}

	/* The toolbar and the page show the result. */
	fflush(stdout);
	app->toolbar_dirty = 1;
	app->redraw = 1;
}

/* Turns a key the menus did not take into an action. */
static void
app_key(
	struct notes_app *app,
	const struct notes_key *key)
{
	double map[6];
	double step;
	int control;
	int shift;

	/* The modifiers that choose a shortcut. */
	control = 0;
	if ((key->modifiers & NOTES_MODIFIER_CONTROL) != 0U)
		control = 1;
	shift = 0;
	if ((key->modifiers & NOTES_MODIFIER_SHIFT) != 0U)
		shift = 1;

	/* The standard shortcuts with Control. */
	if (control) {
		switch (key->key) {
		case MAIN_KEY_S:
			if (shift)
				app_action(app, NOTES_ACTION_SAVE_AS);
			else
				app_action(app, NOTES_ACTION_SAVE);
			break;
		case MAIN_KEY_Z:
			if (shift)
				app_action(app, NOTES_ACTION_REDO);
			else
				app_action(app, NOTES_ACTION_UNDO);
			break;
		case MAIN_KEY_Y:
			app_action(app, NOTES_ACTION_REDO);
			break;
		case MAIN_KEY_N:
			app_action(app, NOTES_ACTION_NEW_PAGE);
			break;
		case MAIN_KEY_O:
			app_action(app, NOTES_ACTION_OPEN);
			break;
		case MAIN_KEY_W:
			app_action(app, NOTES_ACTION_CLOSE);
			break;
		default:
			break;
		}

		/* A key with Control is no tool key. */
		return;
	}

	/* The keys without Control. */
	switch (key->key) {
	case MAIN_KEY_PAGE_UP:
		app_action(app, NOTES_ACTION_PREVIOUS_PAGE);
		break;
	case MAIN_KEY_PAGE_DOWN:
		app_action(app, NOTES_ACTION_NEXT_PAGE);
		break;
	case MAIN_KEY_P:
		app_action(app, NOTES_ACTION_PEN);
		break;
	case MAIN_KEY_M:
		app_action(app, NOTES_ACTION_HIGHLIGHTER);
		break;
	case MAIN_KEY_E:
		app_action(app, NOTES_ACTION_ERASER);
		break;
	case MAIN_KEY_T:
		app_action(app, NOTES_ACTION_TEXT);
		break;
	case MAIN_KEY_ENTER:
		/* The chosen line's words in the text box (ws175-p008). */
		if (app->selected != MAIN_NONE && app->selected_kind == PDF_EDIT_TEXT && !app->selected_fixed)
			app_action(app, NOTES_ACTION_EDIT_TEXT);
		break;
	case MAIN_KEY_F11:
		app_action(app, NOTES_ACTION_FULLSCREEN);
		break;
	case MAIN_KEY_ESC:
		/* The chosen object let go, or fullscreen left. */
		if (app->selected != MAIN_NONE) {
			app_deselect(app);
			break;
		}

		/* Otherwise a window again. */
		app_action(app, NOTES_ACTION_LEAVE_FULLSCREEN);
		break;
	case MAIN_KEY_DELETE:
	case MAIN_KEY_BACKSPACE:
		/* The chosen object deleted (ws175-p008). */
		if (app->selected != MAIN_NONE)
			app_action(app, NOTES_ACTION_DELETE_OBJECT);
		break;
	case MAIN_KEY_LEFT:
	case MAIN_KEY_RIGHT:
	case MAIN_KEY_UP:
	case MAIN_KEY_DOWN:
		/* The chosen object moved by a point (Shift: ten). */
		if (app->selected == MAIN_NONE || app->contact != MAIN_CONTACT_NONE)
			break;
		step = MAIN_NUDGE;
		if (shift)
			step = MAIN_NUDGE * 10.0;
		map[0] = 1.0;
		map[1] = 0.0;
		map[2] = 0.0;
		map[3] = 1.0;
		map[4] = 0.0;
		map[5] = 0.0;
		if (key->key == MAIN_KEY_LEFT)
			map[4] = -step;
		if (key->key == MAIN_KEY_RIGHT)
			map[4] = step;
		if (key->key == MAIN_KEY_UP)
			map[5] = -step;
		if (key->key == MAIN_KEY_DOWN)
			map[5] = step;
		app_apply_map(app, map);
		printf("NOTES EDIT move page=%lu object=%lu dx=%.1f dy=%.1f\n", (unsigned long)app->page, (unsigned long)app->selected, map[4], map[5]);
		fflush(stdout);
		break;
	default:
		break;
	}
}

/* Takes one input event: a press on the toolbar, a stroke, or an eraser drag. */
static void
app_input(
	struct notes_app *app,
	const struct notes_input *input)
{
	struct notes_stroke *stroke;
	uint32_t action;
	unsigned tool;
	uint32_t id;
	uint32_t color;
	float width;
	int inside;
	int closed;
	int near;

	/*
	 * The pen near the window or gone tells a palm from a finger
	 * (ws081-p013).  A line a palm was writing is taken back now, before
	 * the pen's own contact starts (ws081-p015).
	 */
	if (input->source != NOTES_SOURCE_POINTER) {
		near = 1;
		if (input->kind == NOTES_INPUT_LEAVE)
			near = 0;
		notes_touch_pen(&app->touch, near, notes_touch_clock());
		app_finger(app);
	}

	/* The pen's mark follows the pen, over the window or in contact. */
	app_hover(app, input);
	if (input->kind == NOTES_INPUT_HOVER || input->kind == NOTES_INPUT_LEAVE)
		return;

	/* A motion or an end belongs to the contact under way. */
	if (input->kind == NOTES_INPUT_MOTION) {
		if (app->contact == MAIN_CONTACT_DRAW || app->contact == MAIN_CONTACT_ERASE)
			app_sample(app, input);
		if (app->contact == MAIN_CONTACT_SELECT)
			app_select_motion(app, input->x, input->y);
		return;
	}

	/* An end of contact ends it. */
	if (input->kind == NOTES_INPUT_UP) {
		app_end_contact(app, input);
		return;
	}

	/* A contact that starts while one is under way (a lost release) ends the old one first. */
	if (app->contact != MAIN_CONTACT_NONE)
		app_end_contact(app, NULL);

	/* The frame times count from the contact's start. */
	app->frame_count = 0;
	app->frame_build_us = 0;
	app->frame_draw_us = 0;
	app->frame_longest_us = 0;

	/* A press on the toolbar is its button's. */
	if (input->y < (float)NOTES_TOOLBAR_HEIGHT) {
		app->contact = MAIN_CONTACT_TOOLBAR;
		action = notes_ui_hit(&app->ui, input->x, input->y);
		if (action != NOTES_ACTION_NONE)
			app_action(app, action);
		return;
	}

	/*
	 * The text box open (ws175-p008): a press on it is the box's (its widget
	 * has it); a press outside it closes it first, keeping its words, and
	 * goes on as a press of the tool -- unless the editor refused the words,
	 * when the box stays with the keyboard.
	 */
	if (app->box.open) {
		inside = notes_box_hit(&app->box, input->x, input->y);
		if (inside) {
			app->contact = MAIN_CONTACT_BOX;
			return;
		}

		/* Outside: closed first. */
		closed = app_box_commit(app);
		if (!closed) {
			app->contact = MAIN_CONTACT_BOX;
			return;
		}
	}

	/* The Select tool chooses, moves and sizes the page's objects (ws175-p008). */
	if (app->tool == NOTES_ACTION_SELECT && input->source != NOTES_SOURCE_ERASER) {
		app->contact = MAIN_CONTACT_SELECT;
		app_select_press(app, input->x, input->y, input->time_ms);
		return;
	}

	/* The Text tool opens the text box: on a line of text, or for new words where it is pressed (ws175-p008). */
	if (app->tool == NOTES_ACTION_TEXT && input->source != NOTES_SOURCE_ERASER) {
		app->contact = MAIN_CONTACT_TEXT;
		app_text_press(app, input->x, input->y);
		return;
	}

	/* The eraser tool, or a pen's eraser end, erases. */
	if (app->tool == NOTES_ACTION_ERASER || input->source == NOTES_SOURCE_ERASER) {
		app->contact = MAIN_CONTACT_ERASE;
		notes_document_erase_begin(&app->document);
		app_sample(app, input);
		return;
	}

	/* Otherwise a stroke starts, in the tool's colour and width. */
	tool = NOTES_TOOL_PEN;
	if (app->tool == NOTES_ACTION_HIGHLIGHTER)
		tool = NOTES_TOOL_HIGHLIGHTER;
	color = notes_ui_color(tool, app->color);
	width = notes_ui_width(tool, app->width);
	id = app->document.next_id;
	stroke = notes_stroke_create(id, tool, color, width, app_unix_ms());
	if (stroke == NULL)
		return;

	/* The stroke's number is taken, and its first sample. */
	app->document.next_id++;
	app->live = stroke;
	app->live_start = input->time_ms;
	app->contact = MAIN_CONTACT_DRAW;
	app_sample(app, input);
}

/*
 * Follows the pen for its mark: a pen over the window or in contact is
 * where its last event was; one that left, and the pointer (which has its
 * own cursor), have no mark.
 */
static void
app_hover(
	struct notes_app *app,
	const struct notes_input *input)
{
	/* The pointer and a pen that left show no mark; the frame shows it gone (logged for the tests). */
	if (input->kind == NOTES_INPUT_LEAVE || input->source == NOTES_SOURCE_POINTER) {
		if (app->hover) {
			printf("NOTES HOVER gone\n");
			fflush(stdout);
			app->redraw = 1;
		}

		/* No mark from now on. */
		app->hover = 0;
		return;
	}

	/* A pen that comes over the window is logged for the tests. */
	if (!app->hover || app->hover_source != input->source) {
		printf("NOTES HOVER source=%u x=%d y=%d\n", input->source, (int)input->x, (int)input->y);
		fflush(stdout);
	}

	/* The pen is here now; the frame shows its mark here. */
	app->hover = 1;
	app->hover_x = input->x;
	app->hover_y = input->y;
	app->hover_source = input->source;
	app->redraw = 1;
}

/*
 * Moves time on for the fingers: the toolbar's taps press its buttons, and
 * a page the fingers moved (or that glides, or is zoomed) is drawn again.
 */
static void
app_touch(
	struct notes_app *app)
{
	uint32_t action;
	float x;
	float y;
	float scale;
	int toolbar;
	int taken;

	/* A writing finger's line. */
	app_finger(app);

	/* Each tap on the toolbar presses the button under it; one on the page outside an open text box closes it, keeping its words (ws177-p012). */
	for (;;) {
		taken = notes_touch_take_tap(&app->touch, &x, &y, &toolbar);
		if (!taken)
			break;

		/* A tap on the page: the box's own taps never come here. */
		if (!toolbar) {
			if (app->box.open)
				(void)app_box_commit(app);
			continue;
		}

		/* The toolbar's button. */
		action = notes_ui_hit(&app->ui, x, y);
		if (action != NOTES_ACTION_NONE)
			app_action(app, action);
	}

	/* The fingers' time moves on. */
	app->touch_due = notes_touch_tick(&app->touch, notes_touch_clock());

	/*
	 * A place other than the one last drawn (by the tick, or by a gesture of
	 * the events before it, such as a double tap), or the start or end of a
	 * zoom (the page is drawn at its new scale), is drawn.
	 */
	notes_touch_view(&app->touch, &x, &y, &scale);
	if (x != app->view.x ||
	    y != app->view.y ||
	    scale != app->view.scale ||
	    app->touch.zooming != app->drawn_zooming)
		app->redraw = 1;
}

/* Adds a sample to the stroke being drawn, or erases at it. */
static void
app_sample(
	struct notes_app *app,
	const struct notes_input *input)
{
	struct notes_point point;
	struct notes_point *last;
	size_t removed;
	float pressure;
	float dx;
	float dy;
	float step;
	int error;

	/* The place on the page, in points. */
	memset(&point, 0, sizeof(point));
	point.x = (input->x - app->view.x) / app->view.scale;
	point.y = (input->y - app->view.y) / app->view.scale;

	/* An eraser of parts cuts the ink under its circle out of the strokes. */
	if (app->contact == MAIN_CONTACT_ERASE && app->erase_parts) {
		error = notes_document_erase_parts_at(&app->document, app->page, point.x, point.y, MAIN_ERASER_RADIUS, &removed);
		if (error == 0 && removed != 0U) {
			printf("NOTES ERASE page=%lu cut=%lu strokes=%lu\n", (unsigned long)app->page, (unsigned long)removed,
			       (unsigned long)app->document.pages[app->page]->stroke_count);
			fflush(stdout);
			app_changed(app);
		}

		/* An eraser draws no stroke. */
		return;
	}

	/* An eraser of strokes removes the strokes its circle touches. */
	if (app->contact == MAIN_CONTACT_ERASE) {
		error = notes_document_erase_at(&app->document, app->page, point.x, point.y, MAIN_ERASER_RADIUS, &removed);
		if (error == 0 && removed != 0U) {
			printf("NOTES ERASE page=%lu removed=%lu strokes=%lu\n", (unsigned long)app->page, (unsigned long)removed,
			       (unsigned long)app->document.pages[app->page]->stroke_count);
			fflush(stdout);
			app_changed(app);
		}

		/* An eraser draws no stroke. */
		return;
	}

	/* A sample too close to the last one adds nothing. */
	if (app->live->point_count != 0U) {
		last = &app->live->points[app->live->point_count - 1U];
		dx = (point.x - last->x) * app->view.scale;
		dy = (point.y - last->y) * app->view.scale;
		step = dx * dx + dy * dy;
		if (step < MAIN_SAMPLE_STEP * MAIN_SAMPLE_STEP)
			return;
	}

	/* The pressure, the tilt (in 1/100 degree) and the time since the stroke began. */
	pressure = input->pressure;
	if (!(pressure >= 0.0f))
		pressure = 0.0f;
	if (pressure > 1.0f)
		pressure = 1.0f;
	point.pressure = (uint16_t)(pressure * (float)NOTES_PRESSURE_MAX + 0.5f);
	point.tilt_x = (int16_t)(input->tilt_x * 100.0f);
	point.tilt_y = (int16_t)(input->tilt_y * 100.0f);
	point.time_ms = input->time_ms - app->live_start;
	if (input->source != NOTES_SOURCE_POINTER)
		app->live->has_tilt = 1;

	/* Succeeded or not, the stroke is drawn again with what it has. */
	(void)notes_stroke_append(app->live, &point);
	app->redraw = 1;
}

/* Ends the contact under way: a stroke goes on the page, an eraser drag becomes one change. */
static void
app_end_contact(
	struct notes_app *app,
	const struct notes_input *input)
{
	struct notes_stroke *stroke;
	unsigned lowest;
	unsigned highest;
	size_t index;
	int error;

	/* The last sample of a stroke or an eraser drag. */
	if (input != NULL &&
	    (app->contact == MAIN_CONTACT_DRAW ||
	     app->contact == MAIN_CONTACT_ERASE))
		app_sample(app, input);

	/* A finished stroke goes on top of the page. */
	if (app->contact == MAIN_CONTACT_DRAW && app->live != NULL) {
		stroke = app->live;
		app->live = NULL;
		error = notes_document_add_stroke(&app->document, app->page, stroke);
		if (error != 0) {
			notes_stroke_free(stroke);
			app_status(app, "Could not keep the stroke");
		} else {
			/* The range of the stroke's pressure, for the tests' line. */
			lowest = NOTES_PRESSURE_MAX;
			highest = 0;
			for (index = 0; index < stroke->point_count; index++) {
				if (stroke->points[index].pressure < lowest)
					lowest = stroke->points[index].pressure;
				if (stroke->points[index].pressure > highest)
					highest = stroke->points[index].pressure;
			}

			/* The tests' line. */
			printf("NOTES STROKE page=%lu id=%u tool=%u points=%lu strokes=%lu pressure=%u..%u tilt=%d\n", (unsigned long)app->page,
			       stroke->id, stroke->tool, (unsigned long)stroke->point_count, (unsigned long)app->document.pages[app->page]->stroke_count,
			       lowest, highest, stroke->has_tilt);
			fflush(stdout);
			app_changed(app);
		}
	}

	/* An eraser drag's removals are one change from now on. */
	if (app->contact == MAIN_CONTACT_ERASE)
		notes_document_erase_end(&app->document);

	/* A drag of the Select tool is one change. */
	if (app->contact == MAIN_CONTACT_SELECT) {
		if (input != NULL)
			app_select_motion(app, input->x, input->y);
		app_select_release(app);
	}

	/* A press of the Text tool opens the box for new words (ws175-p008). */
	if (app->contact == MAIN_CONTACT_TEXT)
		app_text_release(app, input);

	/* The frames the contact took, for the tests' line. */
	app_frame_report(app);

	/* No contact is under way, and no finger's. */
	app->contact = MAIN_CONTACT_NONE;
	app->finger_contact = 0;
	app->redraw = 1;
}

/*
 * Carries out a writing finger's events (ws081-p015): its line is drawn
 * (or erased along) as the pointer's would be, with the pointer's fixed
 * pressure, and a line taken back is dropped.  Once the pointer or the pen
 * takes the contact over, the finger's later events are left alone.
 */
static void
app_finger(
	struct notes_app *app)
{
	struct notes_touch_write write;
	struct notes_input input;
	int taken;

	/* Each event in turn. */
	for (;;) {
		taken = notes_touch_take_write(&app->touch, &write);
		if (!taken)
			break;

		/* The pointer's input at the finger's place and time. */
		memset(&input, 0, sizeof(input));
		input.source = NOTES_SOURCE_POINTER;
		input.x = write.x;
		input.y = write.y;
		input.pressure = NOTES_POINTER_PRESSURE;
		input.time_ms = write.time_ms;

		/* What the event does to the finger's contact. */
		switch (write.kind) {
		case NOTES_TOUCH_WRITE_BEGIN:
			/* A contact starts as the pointer's press starts one; the finger owns it while it draws or erases. */
			input.kind = NOTES_INPUT_DOWN;
			app_input(app, &input);
			app->finger_contact = 0;
			if (app->contact == MAIN_CONTACT_DRAW ||
			    app->contact == MAIN_CONTACT_ERASE)
				app->finger_contact = 1;
			break;
		case NOTES_TOUCH_WRITE_MOTION:
			/* A point of the finger's line. */
			input.kind = NOTES_INPUT_MOTION;
			if (app->finger_contact)
				app_input(app, &input);
			break;
		case NOTES_TOUCH_WRITE_END:
			/* The line ends and goes on the page. */
			input.kind = NOTES_INPUT_UP;
			if (app->finger_contact)
				app_input(app, &input);
			break;
		case NOTES_TOUCH_WRITE_ABORT:
			/* The line is taken back. */
			if (app->finger_contact)
				app_abort_contact(app);
			break;
		default:
			break;
		}
	}
}

/*
 * Takes back the contact under way without keeping it: a stroke being
 * drawn is dropped (its number is not used again), and an eraser drag's
 * removals so far stay as one change.
 */
static void
app_abort_contact(
	struct notes_app *app)
{
	/* The stroke never reaches the page. */
	if (app->contact == MAIN_CONTACT_DRAW && app->live != NULL) {
		notes_stroke_free(app->live);
		app->live = NULL;
		printf("NOTES ABORT stroke page=%lu strokes=%lu\n", (unsigned long)app->page,
		       (unsigned long)app->document.pages[app->page]->stroke_count);
		fflush(stdout);
	}

	/* An eraser drag's removals are one change from now on. */
	if (app->contact == MAIN_CONTACT_ERASE)
		notes_document_erase_end(&app->document);

	/* A press of the Text tool opens nothing. */
	if (app->contact == MAIN_CONTACT_TEXT)
		app->text_pressing = 0;

	/* A drag of the Select tool is dropped: the object stays where it was, the page's editor is made again. */
	if (app->contact == MAIN_CONTACT_SELECT && app->drag != MAIN_DRAG_NONE) {
		app->drag = MAIN_DRAG_NONE;
		notes_page_close_editor(app->document.pages[app->page]);
		app->drag_previews++;
	}

	/* No contact is under way, and no finger's. */
	app->contact = MAIN_CONTACT_NONE;
	app->finger_contact = 0;
	app->redraw = 1;
}

/* Notes a change of the notebook: the autosave waits from now, and the toolbar shows it. */
static void
app_changed(
	struct notes_app *app)
{
	/* The time of the change. */
	app->changed_at = notes_clock();
	app->toolbar_dirty = 1;
	app->redraw = 1;
}

/*
 * Saves the notebook as its PDF, and removes the journal the save makes
 * unnecessary.  Returns 0, or an errno value.
 */
static int
app_save(
	struct notes_app *app,
	const char *reason)
{
	char text[160];
	size_t edited_pages;
	size_t edits;
	size_t bytes;
	int error;

	/* The file's folder, then the file. */
	bytes = 0;
	error = app_make_folders(app->path);
	if (error == 0)
		error = notes_save_pdf(&app->document, app->path, &bytes);

	/* A failure is shown and logged, and the journal keeps the changes. */
	if (error != 0) {
		printf("NOTES SAVE failed reason=%s error=%d path=%s\n", reason, error, app->path);
		fflush(stdout);
		(void)snprintf(text, sizeof(text), "Could not save: %s", strerror(error));
		app_status(app, text);
		return error;
	}

	/* The journal is no longer needed; the next change starts another. */
	if (app->document.journal != NULL)
		(void)notes_journal_discard(app->document.journal);

	/* The file is among the recent ones. */
	(void)kl_recent_add(app->path, "notes");

	/* The tests' line and the status. */
	app_count_edits(app, &edits, &edited_pages);
	printf("NOTES SAVE reason=%s pages=%lu strokes=%lu edits=%lu edited_pages=%lu bytes=%lu path=%s\n", reason, (unsigned long)app->document.page_count,
	       (unsigned long)notes_document_stroke_total(&app->document), (unsigned long)edits, (unsigned long)edited_pages, (unsigned long)bytes, app->path);
	fflush(stdout);
	app_status(app, "Saved");

	/* Succeeded: the file is the notebook as it stands. */
	return 0;
}

/*
 * Builds and draws a frame: the desk, the page's picture with the strokes
 * added since the last frame, the stroke being drawn, the pen's mark and
 * the toolbar.
 */
static void
app_draw(
	struct notes_app *app)
{
	struct notes_ui_state state;
	struct notes_view view;
	struct notes_page *page;
	struct notes_frame *page_frame;
	unsigned char *pixels;
	const char *mode;
	uint64_t now;
	uint64_t signature;
	uint64_t started;
	uint64_t built;
	uint64_t finished;
	uint32_t picture_width;
	uint32_t picture_height;
	size_t pitch;
	size_t index;
	float stretch;
	int stretched;
	int page_clear;
	int moved;
	int error;

	/* When the frame starts, for the frame times. */
	started = app_microseconds();

	/* A drag of the Select tool shows the page with the object where it is now, every MAIN_DRAG_PREVIEW_MS (ws175-p008). */
	now = notes_clock();
	if (app->drag != MAIN_DRAG_NONE && app->drag_moved && now - app->drag_previewed_at >= MAIN_DRAG_PREVIEW_MS) {
		app->drag_previews++;
		app->drag_previewed_at = now;
		app->drag_moved = 0;
	}

	/* So do the words typed in the text box on a line of the page. */
	if (app->box_changed && now - app->box_previewed_at >= MAIN_DRAG_PREVIEW_MS) {
		app->box_changed = 0;
		app->box_previewed_at = now;
		if (app->box.open && app->box_kind == MAIN_BOX_LINE) {
			app->box_previewing = 1;
			app->text_previews++;
		}
	}

	/* The whole page's place, and the place the fingers' zoom and scroll give it (another page starts at its top). */
	page = app->document.pages[app->page];
	notes_view_layout(&view, app->renderer.extent.width, app->renderer.extent.height, page->width, page->height);
	notes_touch_layout(&app->touch, app->renderer.extent.width, app->renderer.extent.height, (float)NOTES_TOOLBAR_HEIGHT, NOTES_PAGE_MARGIN,
			   page->width, page->height, view.scale);
	if (page != app->touch_page) {
		app->touch_page = page;
		notes_touch_top(&app->touch);
	}

	/* The place the frame is drawn at. */
	notes_touch_view(&app->touch, &view.x, &view.y, &view.scale);

	/* A new place (and the first) is logged for the tests, once the fingers let the page rest. */
	if (!app->touch.moving &&
	    !app->touch.pinching &&
	    (!app->drawn ||
	     view.x != app->logged_view.x ||
	     view.y != app->logged_view.y ||
	     view.scale != app->logged_view.scale)) {
		printf("NOTES LAYOUT window=%ux%u page=%d,%d,%d,%d scale=%.4f\n", app->renderer.extent.width, app->renderer.extent.height,
		       (int)view.x, (int)view.y, (int)(page->width * view.scale), (int)(page->height * view.scale), (double)view.scale);
		fflush(stdout);
		app->logged_view = view;
	}

	/* The place the input is measured against, and whether it is drawn while zooming. */
	app->view = view;
	app->drawn_zooming = app->touch.zooming;

	/* The toolbar, drawn again when its state changed; the menus show the same state. */
	if (app->toolbar_dirty) {
		app_state(app, &state);
		notes_renderer_toolbar(&app->renderer, &pixels, &pitch);
		notes_ui_draw(&app->ui, pixels, pitch, app->renderer.extent.width, NOTES_TOOLBAR_IMAGE_HEIGHT, &state);
		notes_menu_refresh(&app->window, &state);
		app->toolbar_dirty = 0;

		/* The buttons' places, logged for the tests once per width and again when they change (another tool's, ws175-p010). */
		signature = app_buttons_signature(&app->ui);
		if (app->buttons_logged != app->renderer.extent.width || app->buttons_signature != signature) {
			app->buttons_logged = app->renderer.extent.width;
			app->buttons_signature = signature;
			printf("NOTES BUTTONS");
			for (index = 0; index < app->ui.button_count; index++) {
				printf(" %u:%d,%d,%d,%d", app->ui.buttons[index].action, app->ui.buttons[index].x, app->ui.buttons[index].y,
				       app->ui.buttons[index].width, app->ui.buttons[index].height);
			}

			/* The line ends. */
			printf("\n");
			fflush(stdout);
		}
	}

	/*
	 * While two fingers zoom, a picture of the page as it stands (at another
	 * scale) is stretched to the new one rather than drawn again every frame
	 * (ws081-p013); the page is drawn at its scale when they stop.
	 */
	stretch = 1.0f;
	stretched = 0;
	if (app->touch.zooming &&
	    app->picture_page == page &&
	    app->picture_serial == app->renderer.page_serial &&
	    app->picture_reshaped == app->document.reshaped &&
	    app->picture_strokes == page->stroke_count &&
	    app->picture_scale > 0.0f) {
		stretched = 1;
		stretch = view.scale / app->picture_scale;
	}

	/* The page's picture, at the page's size in whole pixels. */
	page_clear = 0;
	page_frame = NULL;
	if (!stretched) {
		picture_width = (uint32_t)ceil((double)(page->width * view.scale));
		picture_height = (uint32_t)ceil((double)(page->height * view.scale));
		error = (int)notes_renderer_page(&app->renderer, picture_width, picture_height);
		if (error != (int)VK_SUCCESS) {
			fprintf(stderr, "notes: %s failed (%d)\n", app->renderer.operation, error);
			app->quit = 1;
			return;
		}

		/* What the picture lacks, when anything: the whole page, or the strokes put on top since. */
		page_frame = app_page_frame(app, page, view.scale, &page_clear);
	}

	/* The desk and the page's picture on it. */
	notes_frame_begin(&app->frame);
	app_desk(app, &view, page->width * view.scale, page->height * view.scale);
	notes_frame_texture(&app->frame, NOTES_TEXTURE_PAGE, view.x, view.y,
			    (float)app->renderer.page_width * stretch, (float)app->renderer.page_height * stretch);

	/* The stroke being drawn, on top, clipped to the page. */
	notes_frame_clip(&app->frame, 1, view.x, view.y, page->width * view.scale, page->height * view.scale);
	if (app->live != NULL && app->live->point_count != 0U) {
		error = notes_stroke_outline(app->live);
		if (error == 0)
			notes_frame_polygon(&app->frame, app->live->outline, app->live->outline_count, &view, app->live->color);
	}

	/* The chosen object's frame and handles (ws175-p008), then the pen's mark on the page. */
	app_selection_draw(app, &view);
	app_mark(app, &view, 0);

	/* The text box moved with the page shown (ws177-p012): its frame drawn again where it is now. */
	moved = app_box_follow(app);
	if (moved)
		app_box_frame(app);

	/* The text box over the page (ws175-p008), on the overlay of the size it was drawn at. */
	if (app->box.open && app->renderer.overlay != VK_NULL_HANDLE) {
		notes_frame_clip(&app->frame, 0, 0.0f, 0.0f, 0.0f, 0.0f);
		notes_frame_texture(&app->frame, NOTES_TEXTURE_OVERLAY, 0.0f, 0.0f, (float)app->renderer.overlay_width, (float)app->renderer.overlay_height);
	}

	/* The toolbar across the top, and the pen's mark over it. */
	notes_frame_clip(&app->frame, 0, 0.0f, 0.0f, 0.0f, 0.0f);
	notes_frame_texture(&app->frame, NOTES_TEXTURE_TOOLBAR, 0.0f, 0.0f, (float)app->renderer.extent.width, (float)NOTES_TOOLBAR_IMAGE_HEIGHT);
	app_mark(app, &view, 1);

	/* A frame that ran out of memory is not drawn. */
	if (app->frame.error != 0 ||
	    (page_frame != NULL &&
	     page_frame->error != 0)) {
		printf("NOTES DRAW out of memory\n");
		return;
	}

	/* Draws it, after the time the geometry took; a swapchain out of date is remade and drawn next time. */
	built = app_microseconds();
	error = (int)notes_renderer_draw(&app->renderer, &app->frame, page_frame, page_clear);
	if (error == (int)VK_ERROR_OUT_OF_DATE_KHR) {
		app->window.resized = 1;
		return;
	}

	/* Any other failure ends Notes. */
	if (error != (int)VK_SUCCESS) {
		fprintf(stderr, "notes: %s failed (%d)\n", app->renderer.operation, error);
		app->quit = 1;
		return;
	}

	/* The picture holds the page as it stands now: drawn anew, or added to (logged for the tests). */
	if (page_frame != NULL) {
		mode = "add";
		if (page_clear)
			mode = "full";
		printf("NOTES PICTURE %s strokes=%lu..%lu\n", mode, (unsigned long)app->picture_strokes, (unsigned long)page->stroke_count);
		fflush(stdout);
		app->picture_page = page;
		app->picture_serial = app->renderer.page_serial;
		app->picture_reshaped = app->document.reshaped;
		app->picture_scale = view.scale;
		app->picture_strokes = page->stroke_count;
		app->picture_look = app_look(app);
	}

	/* The frame's times join the ones the next NOTES FRAMES line reports. */
	finished = app_microseconds();
	app->frame_count++;
	app->frame_build_us += built - started;
	app->frame_draw_us += finished - built;
	if (finished - started > app->frame_longest_us)
		app->frame_longest_us = finished - started;

	/* Succeeded: the frame is shown (the first is logged for the tests). */
	if (!app->drawn) {
		printf("NOTES FRAME first draws=%lu\n", (unsigned long)app->frame.draw_count);
		fflush(stdout);
	}

	/* Nothing waits to be drawn. */
	app->drawn = 1;
	app->redraw = 0;
}

/*
 * Builds the page frame the page's picture lacks, and tells whether it
 * starts from a cleared picture: the whole page when the picture shows
 * another page, was made again, is of another scale, or lost strokes;
 * only the strokes put on top since when that is all that changed.
 * Returns NULL when the picture shows the page as it stands.
 */
static struct notes_frame *
app_page_frame(
	struct notes_app *app,
	const struct notes_page *page,
	float scale,
	int *clear)
{
	struct notes_stroke *stroke;
	struct notes_view origin;
	uint64_t look;
	size_t first;
	size_t index;
	int drawn;
	int error;

	/* The picture must be drawn from the start when anything but strokes on top changed (ws175-p008: the objects' look too). */
	look = app_look(app);
	*clear = 0;
	if (app->picture_page != page) {
		*clear = 1;
	} else if (app->picture_serial != app->renderer.page_serial) {
		*clear = 1;
	} else if (app->picture_reshaped != app->document.reshaped) {
		*clear = 1;
	} else if (app->picture_scale != scale) {
		*clear = 1;
	} else if (page->stroke_count < app->picture_strokes) {
		*clear = 1;
	} else if (app->picture_look != look) {
		*clear = 1;
	}

	/* A picture that has every stroke needs nothing. */
	if (!*clear && page->stroke_count == app->picture_strokes)
		return NULL;

	/* The picture's own place: the page at its top left, at the frame's scale. */
	origin.x = 0.0f;
	origin.y = 0.0f;
	origin.scale = scale;

	/*
	 * A cleared picture starts with the white page, and on a page of the
	 * PDF the notebook writes on, with that page drawn over it; one added to
	 * starts at its first missing stroke.
	 */
	notes_frame_begin(&app->page_frame);
	first = app->picture_strokes;
	if (*clear) {
		first = 0;
		app->picture_strokes = 0;
		notes_frame_rect(&app->page_frame, 0.0f, 0.0f, page->width * scale, page->height * scale, 0xffffffffU);
		drawn = 0;
		if (page->origin == NOTES_ORIGIN_OVER || page->edit_count > 0U || app->drag != MAIN_DRAG_NONE)
			drawn = app_background(app, page, scale);
		if (drawn) {
			notes_frame_texture(&app->page_frame, NOTES_TEXTURE_BACKGROUND, 0.0f, 0.0f,
					    (float)app->renderer.background_width, (float)app->renderer.background_height);
		}
	}

	/* The strokes, bottom first, clipped to the page. */
	notes_frame_clip(&app->page_frame, 1, 0.0f, 0.0f, page->width * scale, page->height * scale);
	for (index = first; index < page->stroke_count; index++) {
		stroke = page->strokes[index];
		error = notes_stroke_outline(stroke);
		if (error != 0)
			continue;
		notes_frame_polygon(&app->page_frame, stroke->outline, stroke->outline_count, &origin, stroke->color);
	}

	/* Reports the page frame. */
	return &app->page_frame;
}

/* Adds the desk: the soft wash over the window, and the page's shadow. */
static void
app_desk(
	struct notes_app *app,
	const struct notes_view *view,
	float width,
	float height)
{
	float window_width;
	float window_height;
	float haze;
	float left;
	float top;
	float share;
	uint32_t color;
	int ring;

	/* The wash: from sky at the top to the haze, and from the haze to leaf green at the bottom. */
	window_width = (float)app->renderer.extent.width;
	window_height = (float)app->renderer.extent.height;
	haze = (float)floor((double)(window_height * MAIN_DESK_HAZE_SHARE));
	notes_frame_gradient(&app->frame, 0.0f, 0.0f, window_width, haze, MAIN_DESK_TOP, MAIN_DESK_HAZE);
	notes_frame_gradient(&app->frame, 0.0f, haze, window_width, window_height - haze, MAIN_DESK_HAZE, MAIN_DESK_BOTTOM);

	/*
	 * The shadow, dropped a little below the page: rings a pixel wide round
	 * it, fading outwards.  Each ring is four thin strips, so only the
	 * shadow's own pixels are drawn.
	 */
	for (ring = 0; ring < MAIN_SHADOW_RINGS; ring++) {
		share = 1.0f - (float)ring / (float)MAIN_SHADOW_RINGS;
		color = MAIN_SHADOW_COLOR | (uint32_t)(share * share * (float)MAIN_SHADOW_ALPHA);
		left = view->x - (float)ring - 1.0f;
		top = view->y + MAIN_SHADOW_DROP - (float)ring - 1.0f;

		/* The ring's top and bottom strips, then its sides between them. */
		notes_frame_rect(&app->frame, left, top, width + 2.0f * (float)ring + 2.0f, 1.0f, color);
		notes_frame_rect(&app->frame, left, top + height + 2.0f * (float)ring + 1.0f, width + 2.0f * (float)ring + 2.0f, 1.0f, color);
		notes_frame_rect(&app->frame, left, top + 1.0f, 1.0f, height + 2.0f * (float)ring, color);
		notes_frame_rect(&app->frame, left + width + 2.0f * (float)ring + 1.0f, top + 1.0f, 1.0f, height + 2.0f * (float)ring, color);
	}
}

/*
 * Adds the pen's mark: where the pen is while it is over the window and
 * the compositor shows no cursor for it.  On the page it shows the tool --
 * a dot of the pen's or the highlighter's colour and width, or the eraser's
 * ring -- and over the toolbar (over_toolbar) a small slate dot.  A pen
 * drawing a stroke has no mark: the ink shows where it is.
 */
static void
app_mark(
	struct notes_app *app,
	const struct notes_view *view,
	int over_toolbar)
{
	unsigned tool;
	uint32_t color;
	float radius;
	int on_toolbar;

	/* No pen over the window, or a pen drawing. */
	if (!app->hover)
		return;
	if (app->contact == MAIN_CONTACT_DRAW)
		return;

	/* The mark is drawn with the page's part of the frame, or the toolbar's. */
	on_toolbar = 0;
	if (app->hover_y < (float)NOTES_TOOLBAR_HEIGHT)
		on_toolbar = 1;
	if (on_toolbar != over_toolbar)
		return;

	/* Over the toolbar, a small dot that points at the buttons. */
	if (on_toolbar) {
		app_circle(&app->frame, app->hover_x, app->hover_y, 5.5f, MAIN_MARK_HALO);
		app_circle(&app->frame, app->hover_x, app->hover_y, 4.0f, MAIN_MARK_RING | 0xffU);
		return;
	}

	/* The Select tool has no mark on the page. */
	if (app->tool == NOTES_ACTION_SELECT && app->hover_source != NOTES_SOURCE_ERASER)
		return;

	/* The eraser, or a pen's eraser end: its ring, the size it erases. */
	if (app->tool == NOTES_ACTION_ERASER || app->hover_source == NOTES_SOURCE_ERASER) {
		radius = MAIN_ERASER_RADIUS * view->scale;
		app_circle(&app->frame, app->hover_x, app->hover_y, radius, MAIN_MARK_FILL);
		app_ring(&app->frame, app->hover_x, app->hover_y, radius, 1.5f, MAIN_MARK_RING);
		return;
	}

	/* The pen or the highlighter: a dot of its colour and about its width, on a white halo. */
	tool = NOTES_TOOL_PEN;
	if (app->tool == NOTES_ACTION_HIGHLIGHTER)
		tool = NOTES_TOOL_HIGHLIGHTER;
	color = notes_ui_color(tool, app->color);
	radius = notes_ui_width(tool, app->width) * view->scale * 0.5f;
	if (radius < 2.0f)
		radius = 2.0f;
	app_circle(&app->frame, app->hover_x, app->hover_y, radius + 1.5f, MAIN_MARK_HALO);
	app_circle(&app->frame, app->hover_x, app->hover_y, radius, color);
}

/* Adds a filled circle in window pixels, in a colour (0xRRGGBBAA). */
static void
app_circle(
	struct notes_frame *frame,
	float cx,
	float cy,
	float radius,
	uint32_t color)
{
	struct pdf_point points[MAIN_CIRCLE_POINTS];
	struct notes_view pixels;
	double angle;
	unsigned index;

	/* The circle's corners, counter-clockwise. */
	for (index = 0; index < MAIN_CIRCLE_POINTS; index++) {
		angle = 2.0 * 3.14159265358979 * (double)index / (double)MAIN_CIRCLE_POINTS;
		points[index].x = (double)cx + (double)radius * cos(angle);
		points[index].y = (double)cy + (double)radius * sin(angle);
	}

	/* The polygon, placed in pixels as they are. */
	pixels.x = 0.0f;
	pixels.y = 0.0f;
	pixels.scale = 1.0f;
	notes_frame_polygon(frame, points, MAIN_CIRCLE_POINTS, &pixels, color);
}

/*
 * Adds a ring in window pixels, in a colour (0xRRGGBBAA): one polygon that
 * goes round the outer circle and back round the inner one, which the
 * nonzero rule fills between them.
 */
static void
app_ring(
	struct notes_frame *frame,
	float cx,
	float cy,
	float radius,
	float thickness,
	uint32_t color)
{
	struct pdf_point points[2U * MAIN_CIRCLE_POINTS + 2U];
	struct notes_view pixels;
	double angle;
	double inner;
	unsigned index;
	unsigned count;

	/* The outer circle, closed back to its first corner. */
	count = 0;
	for (index = 0; index <= MAIN_CIRCLE_POINTS; index++) {
		angle = 2.0 * 3.14159265358979 * (double)index / (double)MAIN_CIRCLE_POINTS;
		points[count].x = (double)cx + (double)radius * cos(angle);
		points[count].y = (double)cy + (double)radius * sin(angle);
		count++;
	}

	/* The inner circle the other way round, back to the start. */
	inner = (double)radius - (double)thickness;
	for (index = 0; index <= MAIN_CIRCLE_POINTS; index++) {
		angle = -2.0 * 3.14159265358979 * (double)index / (double)MAIN_CIRCLE_POINTS;
		points[count].x = (double)cx + inner * cos(angle);
		points[count].y = (double)cy + inner * sin(angle);
		count++;
	}

	/* The polygon, placed in pixels as they are. */
	pixels.x = 0.0f;
	pixels.y = 0.0f;
	pixels.scale = 1.0f;
	notes_frame_polygon(frame, points, count, &pixels, color);
}

/*
 * Logs the frames drawn since the last report -- their count, the average
 * time their geometry and their drawing took, and the longest frame -- and
 * starts counting again.
 */
static void
app_frame_report(
	struct notes_app *app)
{
	unsigned long build_average;
	unsigned long draw_average;

	/* A contact that drew no frame has nothing to report. */
	if (app->frame_count == 0U)
		return;

	/* The averages, in microseconds. */
	build_average = (unsigned long)(app->frame_build_us / app->frame_count);
	draw_average = (unsigned long)(app->frame_draw_us / app->frame_count);

	/* The tests' line. */
	printf("NOTES FRAMES count=%lu strokes=%lu build_us=%lu draw_us=%lu frame_us=%lu longest_us=%lu\n", app->frame_count,
	       (unsigned long)app->document.pages[app->page]->stroke_count, build_average, draw_average,
	       build_average + draw_average, (unsigned long)app->frame_longest_us);
	fflush(stdout);

	/* The next report counts from here. */
	app->frame_count = 0;
	app->frame_build_us = 0;
	app->frame_draw_us = 0;
	app->frame_longest_us = 0;
}

/* Tells how long the main loop may wait, in milliseconds (-1: until the compositor speaks). */
static int
app_timeout(
	const struct notes_app *app,
	uint64_t now)
{
	uint64_t due;
	uint64_t wait;

	/* The next thing due: the autosave, the status's end, the end of the run. */
	due = 0;
	if (app->document.dirty && app->contact == MAIN_CONTACT_NONE)
		due = app->changed_at + NOTES_AUTOSAVE_IDLE_MS;
	if (app->status_until != 0U &&
	    (due == 0U ||
	     app->status_until < due))
		due = app->status_until;
	if (app->deadline != 0U &&
	    (due == 0U ||
	     app->deadline < due))
		due = app->deadline;

	/* The text box's words on the page, once typing let the preview's time pass (ws175-p008). */
	if (app->box_changed &&
	    (due == 0U ||
	     app->box_previewed_at + MAIN_DRAG_PREVIEW_MS < due))
		due = app->box_previewed_at + MAIN_DRAG_PREVIEW_MS;

	/* A frame waiting to be drawn is due now. */
	if (app->redraw)
		return 0;

	/* The fingers' next tick, when it comes before anything else. */
	if (app->touch_due >= 0 &&
	    (due == 0U ||
	     now + (uint64_t)app->touch_due < due))
		return app->touch_due;

	/* Nothing is due: wait for the compositor. */
	if (due == 0U)
		return -1;

	/* Something past due runs now. */
	if (due <= now)
		return 0;

	/* Reports the wait, within an int. */
	wait = due - now;
	if (wait > 1000000U)
		wait = 1000000U;
	return (int)wait;
}

/* Returns the time of day in UNIX milliseconds. */
static uint64_t
app_unix_ms(void)
{
	struct timespec now;
	int status;

	/* The real-time clock. */
	status = clock_gettime(CLOCK_REALTIME, &now);
	if (status != 0)
		return 0U;

	/* Reports it in milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Returns the monotonic time in microseconds. */
static uint64_t
app_microseconds(void)
{
	struct timespec now;
	int status;

	/* The monotonic clock. */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return 0U;

	/* Reports it in microseconds. */
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

/*
 * Shows libkeiland's file chooser for File > Open (KL_FILE_CHOOSER_OPEN) or
 * Save As (KL_FILE_CHOOSER_SAVE), at the notebook's folder, the PDFs
 * shown first (ws128-p002).  One already shown answers in its time.
 * ws175-p008: for an image to insert or put in the chosen object's place
 * (purpose), the images shown first.
 */
static void
app_choose(
	struct notes_app *app,
	unsigned mode,
	unsigned purpose)
{
	static const struct kl_file_filter filters[] = {
		{ "PDF documents", "pdf" },
		{ "All files", NULL }
	};
	static const struct kl_file_filter image_filters[] = {
		{ "Images", "png jpg jpeg jpe" },
		{ "All files", NULL }
	};
	static const struct kl_file_chooser_listener listener = {
		app_chooser_done
	};
	struct kl_file_chooser_options options;
	char folder[MAIN_PATH_MAX];
	char clean_name[MAIN_PATH_MAX];
	const char *word;
	char *slash;
	size_t length;
	int has_extension;

	/* One chooser at a time; this one's purpose (MAIN_CHOOSE_*). */
	if (app->chooser != NULL)
		return;
	app->chooser_purpose = purpose;

	/* The notebook's folder (the home folder when the path has none). */
	(void)snprintf(folder, sizeof(folder), "%s", app->path);
	slash = strrchr(folder, '/');
	if (slash != NULL && slash != folder)
		*slash = '\0';

	/* Open shows the PDFs; Save As starts with the notebook's name. */
	memset(&options, 0, sizeof(options));
	options.mode = mode;
	options.application = MAIN_APPLICATION;
	options.folder = folder;
	options.filters = filters;
	options.filter_count = sizeof(filters) / sizeof(filters[0]);
	options.filter = 0;
	options.font = MAIN_FONT;
	if (mode == KL_FILE_CHOOSER_SAVE) {
		options.title = "Save As";
		options.name = app->name;
	}

	/* A clean copy starts with the notebook's name and "-clean" (ws175-p009). */
	if (app->chooser_purpose == MAIN_CHOOSE_CLEAN) {
		/* The name without its .pdf, then the suffix. */
		(void)snprintf(clean_name, sizeof(clean_name), "%s", app->name);
		length = strlen(clean_name);
		has_extension = 0;
		if (length > 4U)
			has_extension = strcmp(clean_name + length - 4U, ".pdf") == 0;
		if (has_extension)
			clean_name[length - 4U] = '\0';
		length = strlen(clean_name);
		(void)snprintf(clean_name + length, sizeof(clean_name) - length, "-clean.pdf");
		options.title = "Save Clean Copy";
		options.name = clean_name;
	}

	/* An image's chooser shows the images (ws175-p008). */
	if (app->chooser_purpose == MAIN_CHOOSE_INSERT || app->chooser_purpose == MAIN_CHOOSE_REPLACE) {
		options.filters = image_filters;
		options.filter_count = sizeof(image_filters) / sizeof(image_filters[0]);
		options.title = "Insert Image";
		if (app->chooser_purpose == MAIN_CHOOSE_REPLACE)
			options.title = "Replace Image";
	}

	/* The chooser's window over Notes'; without it the status says why. */
	app->chooser = kl_file_chooser_open(app->window.display, app->window.toplevel, &options, &listener, app);
	if (app->chooser == NULL) {
		printf("NOTES CHOOSER failed errno=%d\n", errno);
		app_status(app, "The file chooser could not be shown");
		return;
	}

	/* The tests' line. */
	app->chooser_mode = mode;
	word = "open";
	if (mode == KL_FILE_CHOOSER_SAVE)
		word = "save";
	if (app->chooser_purpose == MAIN_CHOOSE_INSERT)
		word = "insert";
	if (app->chooser_purpose == MAIN_CHOOSE_REPLACE)
		word = "replace";
	if (app->chooser_purpose == MAIN_CHOOSE_CLEAN)
		word = "clean";
	printf("NOTES CHOOSER open mode=%s folder=%s\n", word, folder);
}

/* The chooser answered: the path (empty when cancelled) waits for the main loop, and the chooser goes. */
static void
app_chooser_done(
	void *data,
	struct kl_file_chooser *chooser,
	unsigned result,
	const char *path,
	size_t filter)
{
	struct notes_app *app;

	/* The answer, kept for the main loop (which carries it out after the dispatch). */
	(void)filter;
	app = data;
	app->chosen[0] = '\0';
	if (result == KL_FILE_CHOOSER_CHOSEN && path != NULL)
		(void)snprintf(app->chosen, sizeof(app->chosen), "%s", path);
	app->chosen_mode = app->chooser_mode;
	app->chosen_purpose = app->chooser_purpose;
	app->chosen_ready = 1;

	/* The chooser is spent. */
	kl_file_chooser_destroy(chooser);
	if (chooser == app->chooser)
		app->chooser = NULL;
}

/* Carries out what the chooser answered: nothing for a cancel, else the file opened or saved as. */
static void
app_chosen(
	struct notes_app *app)
{
	char path[MAIN_PATH_MAX];

	/* Once. */
	app->chosen_ready = 0;
	(void)snprintf(path, sizeof(path), "%s", app->chosen);

	/* Cancelled: the notebook stays as it is. */
	if (path[0] == '\0') {
		printf("NOTES CHOOSER cancelled\n");
		fflush(stdout);
		return;
	}

	/* A clean copy written there (ws175-p009). */
	if (app->chosen_purpose == MAIN_CHOOSE_CLEAN) {
		app_save_clean(app, path);
		return;
	}

	/* An image inserted or put in the chosen object's place (ws175-p008). */
	if (app->chosen_purpose != MAIN_CHOOSE_DOCUMENT) {
		app_put_image(app, path, app->chosen_purpose);
		return;
	}

	/* Save As, or Open. */
	if (app->chosen_mode == KL_FILE_CHOOSER_SAVE) {
		app_save_as(app, path);
	} else {
		app_open_file(app, path);
	}
}

/*
 * Opens another PDF in place of the notebook shown (File > Open,
 * ws128-p002): the notebook is saved first (a stroke being drawn is kept),
 * then the new one starts as at Notes' start -- its journal recovered,
 * its edit data read, or another program's PDF written on.
 */
static void
app_open_file(
	struct notes_app *app,
	const char *path)
{
	int same;
	int error;

	/* The notebook shown is opened already. */
	same = strcmp(path, app->path);
	if (same == 0) {
		app_status(app, "That notebook is open");
		return;
	}

	/* The notebook shown is kept: a stroke being drawn ends, and changes are saved. */
	if (app->live != NULL)
		app_end_contact(app, NULL);
	if (app->document.dirty) {
		error = app_save(app, "open");
		if (error != 0)
			return;
	}

	/* The notebook shown goes, with the pictures drawn of it. */
	notes_journal_destroy(app->document.journal);
	app->document.journal = NULL;
	notes_document_free(&app->document);
	memset(&app->document, 0, sizeof(app->document));
	pdf_display_list_destroy(app->background_list);
	app->background_list = NULL;
	app->background_failed = 0;
	app->background_page = NULL;
	app->background_list_page = NULL;
	app->picture_page = NULL;
	app->touch_page = NULL;
	app->selected = MAIN_NONE;
	app->drag = MAIN_DRAG_NONE;
	app->page = 0;
	app->status[0] = '\0';
	app->status_until = 0;

	/* The new one; a notebook that cannot start at all leaves a new one at a new path. */
	error = app_start_document(app, path);
	if (error != 0) {
		printf("NOTES OPEN failed error=%d path=%s\n", error, path);
		error = app_start_document(app, NULL);
		if (error != 0) {
			fprintf(stderr, "notes: cannot start a notebook: %s\n", strerror(error));
			app->quit = 1;
			return;
		}
	}

	/* The new notebook shown from its first page, named in the title, and among the recent files. */
	app_place_page(app);
	app_set_title(app);
	(void)kl_recent_add(app->path, MAIN_APPLICATION);
	if (app->status[0] == '\0')
		app_status(app, "Opened");
	printf("NOTES OPENED pages=%lu strokes=%lu path=%s\n", (unsigned long)app->document.page_count,
	       (unsigned long)notes_document_stroke_total(&app->document), app->path);
	fflush(stdout);
	app->toolbar_dirty = 1;
	app->redraw = 1;
}

/*
 * Saves the notebook as another file and goes on writing that one (File >
 * Save As, ws128-p002): the journal follows the new path, and the file the
 * notebook was saved as before stays as it was saved.
 */
static void
app_save_as(
	struct notes_app *app,
	const char *path)
{
	const char *slash;
	int same;

	/* The same file is a plain save. */
	same = strcmp(path, app->path);
	if (same == 0) {
		(void)app_save(app, "request");
		return;
	}

	/* The old path's journal goes (a save follows at once, so nothing is lost). */
	if (app->document.journal != NULL) {
		(void)notes_journal_discard(app->document.journal);
		notes_journal_destroy(app->document.journal);
		app->document.journal = NULL;
	}

	/* The new path, its name and its journal. */
	(void)snprintf(app->path, sizeof(app->path), "%s", path);
	slash = strrchr(app->path, '/');
	app->name = app->path;
	if (slash != NULL)
		app->name = slash + 1;
	app->document.journal = notes_journal_create(app->path);
	if (app->document.journal == NULL)
		printf("NOTES JOURNAL none error=%d\n", errno);

	/* The title, and the notebook saved there now. */
	app_set_title(app);
	(void)app_save(app, "save-as");
	app->toolbar_dirty = 1;
	app->redraw = 1;
}

/*
 * Writes a clean copy of the notebook to another file (File > Save Clean
 * Copy, ws175-p009): the notebook is saved first, then its PDF is written
 * whole at the path without the earlier revisions, the resources no page
 * uses and the edit data.  The notebook stays at its own path; the copy
 * opens as another program's PDF.
 */
static void
app_save_clean(
	struct notes_app *app,
	const char *path)
{
	struct stat status;
	size_t bytes;
	size_t objects;
	size_t dropped;
	int same;
	int found;
	int told;
	int error;

	/* The notebook's own file cannot be its copy. */
	same = strcmp(path, app->path);
	if (same == 0) {
		app_status(app, "Choose another file for the clean copy");
		return;
	}

	/* The notebook saved, when it has changes or no file yet (a stroke being drawn ends first). */
	if (app->live != NULL)
		app_end_contact(app, NULL);
	found = stat(app->path, &status);
	if (app->document.dirty || found != 0) {
		error = app_save(app, "clean-copy");
		if (error != 0)
			return;
	}

	/* The copy; a failure is shown and logged. */
	error = notes_save_clean_copy(app->path, path, &bytes, &objects, &dropped);
	if (error != 0) {
		printf("NOTES CLEAN-COPY failed error=%d path=%s\n", error, path);
		fflush(stdout);
		app_status(app, "Could not save the clean copy");
		return;
	}

	/* The copy is among the recent files, and File > Open Clean Copy opens it (ws177-p011). */
	(void)kl_recent_add(path, MAIN_APPLICATION);
	(void)snprintf(app->clean_path, sizeof(app->clean_path), "%s", path);

	/* The first clean copy ever tells, once, that the notebook itself still keeps what was removed (design.md D1). */
	told = app_clean_told();
	if (!told) {
		app_status(app, "Clean copy saved. This notebook keeps removed items in its history");
		app->status_until = notes_clock() + 2U * MAIN_STATUS_MS;
	} else {
		app_status(app, "Saved a clean copy. File > Open Clean Copy opens it");
	}

	/* The tests' line. */
	printf("NOTES CLEAN-COPY bytes=%lu objects=%lu dropped=%lu told=%d path=%s\n", (unsigned long)bytes, (unsigned long)objects, (unsigned long)dropped, told, path);
	fflush(stdout);
}

/*
 * Tells whether Notes told already, after an earlier clean copy, that the
 * notebook keeps removed items in its history; the first time it says no
 * and remembers that it told (notes.clean-copy-told in Notes' own
 * settings).  Without the settings it tells every time.
 */
static int
app_clean_told(void)
{
	struct kl_settings *settings;
	int told;

	/* Notes' own settings, which need no display. */
	settings = kl_settings_open(NULL, MAIN_SETTINGS_APP);
	if (settings == NULL)
		return 0;

	/* Told before, or told now and remembered. */
	told = kl_settings_get_int(settings, MAIN_CLEAN_TOLD_KEY, 0);
	if (!told)
		(void)kl_settings_set_int(settings, MAIN_CLEAN_TOLD_KEY, 1, NULL);
	kl_settings_close(settings);

	/* Succeeded: whether it was told before this time. */
	return told;
}

/* Places the notebook's first page in the window, for the pointer and the fingers (at the start, and after Open). */
static void
app_place_page(
	struct notes_app *app)
{
	/* The page's place, and the fingers' with it. */
	notes_view_layout(&app->view, app->renderer.extent.width, app->renderer.extent.height,
			  app->document.pages[0]->width, app->document.pages[0]->height);
	notes_touch_layout(&app->touch, app->renderer.extent.width, app->renderer.extent.height, (float)NOTES_TOOLBAR_HEIGHT, NOTES_PAGE_MARGIN,
			   app->document.pages[0]->width, app->document.pages[0]->height, app->view.scale);
}

/* Takes the desktop's new appearance: the desk and the toolbar are drawn again in it. */
static void
app_appearance_changed(
	void *data,
	unsigned appearance)
{
	struct notes_app *app;

	/* A new frame and toolbar. */
	app = data;
	app->redraw = 1;
	app->toolbar_dirty = 1;
	printf("NOTES APPEARANCE appearance=%u\n", appearance);
	fflush(stdout);
}

/* Names a tool for the tests' line (design.md [L9]). */
static const char *
app_tool_name(
	unsigned tool)
{
	/* Each tool by its word. */
	switch (tool) {
	case NOTES_ACTION_HIGHLIGHTER:
		return "highlighter";
	case NOTES_ACTION_ERASER:
		return "eraser";
	case NOTES_ACTION_SELECT:
		return "select";
	case NOTES_ACTION_TEXT:
		return "text";
	default:
		break;
	}

	/* The pen. */
	return "pen";
}

/*
 * Gives the look of the pages' objects (ws175-p008): it grows when an edit
 * changes and when a drag's or the text box's preview is drawn, which is
 * when the page's background must be drawn again.
 */
static uint64_t
app_look(
	const struct notes_app *app)
{
	/* The edits' changes, the drags' previews and the text box's. */
	return app->document.edit_serial + app->drag_previews + app->text_previews;
}

/* Counts the notebook's edits and the pages that have any, for the tests' lines. */
static void
app_count_edits(
	const struct notes_app *app,
	size_t *edits,
	size_t *pages)
{
	size_t index;

	/* Each page's edits. */
	*edits = 0;
	*pages = 0;
	for (index = 0; index < app->document.page_count; index++) {
		*edits += app->document.pages[index]->edit_count;
		if (app->document.pages[index]->edit_count > 0U)
			(*pages)++;
	}
}

/*
 * Starts a contact of the Select tool at a point of the window
 * (ws175-p008, design.md section 7.1): on a handle of the chosen object it
 * sizes it, on an image, a graphic or a line of text of the page it
 * chooses it and moves it, elsewhere it lets the chosen one go.  A second
 * press on the chosen line of text within MAIN_DOUBLE_MS of the first (at
 * the compositor's time) opens its words in the text box.
 */
static void
app_select_press(
	struct notes_app *app,
	float x,
	float y,
	uint32_t time_ms)
{
	struct pdf_page_editor *editor;
	struct pdf_edit_object object;
	float page_x;
	float page_y;
	float corner_x;
	float corner_y;
	size_t index;
	unsigned corner;
	unsigned status;
	int error;

	/* The point on the page, and the page's editor. */
	page_x = (x - app->view.x) / app->view.scale;
	page_y = (y - app->view.y) / app->view.scale;
	app->drag = MAIN_DRAG_NONE;
	error = notes_page_editor(&app->document, app->page, &editor);
	if (error != 0) {
		printf("NOTES EDIT page failed page=%lu error=%d\n", (unsigned long)app->page, error);
		fflush(stdout);
		app_status(app, "The objects of this page cannot be edited");
		app_deselect(app);
		return;
	}

	/* A page with a stream that cannot be read cannot be edited (design.md [M5]). */
	status = pdf_page_editor_status(editor);
	if ((status & PDF_EDIT_PAGE_READ_ONLY) != 0U) {
		app_status(app, "This page cannot be edited: part of it cannot be read");
		app_deselect(app);
		return;
	}

	/* A handle of the chosen object sizes it, from the opposite corner. */
	memset(&object, 0, sizeof(object));
	object.size = sizeof(object);
	if (app->selected != MAIN_NONE) {
		error = pdf_page_editor_object(editor, app->selected, &object);
		for (corner = 0; error == 0 && corner < 4U; corner++) {
			corner_x = app->view.x + (float)object.quad[corner * 2U] * app->view.scale - x;
			corner_y = app->view.y + (float)object.quad[corner * 2U + 1U] * app->view.scale - y;
			if (corner_x > MAIN_HANDLE_REACH || corner_x < -MAIN_HANDLE_REACH || corner_y > MAIN_HANDLE_REACH || corner_y < -MAIN_HANDLE_REACH)
				continue;
			app->drag = MAIN_DRAG_RESIZE;
			app->drag_corner = corner;
			break;
		}
	}

	/* Otherwise the object under the point: an image, a graphic or a line of text. */
	if (app->drag == MAIN_DRAG_NONE) {
		error = pdf_page_editor_hit(editor, (double)page_x, (double)page_y, &index);
		if (error == 0)
			error = pdf_page_editor_object(editor, index, &object);
		if (error != 0) {
			app->select_press_object = MAIN_NONE;
			app_deselect(app);
			return;
		}

		/* A double click on the chosen line of text: its words in the box. */
		if (object.kind == PDF_EDIT_TEXT && index == app->selected && index == app->select_press_object &&
		    time_ms - app->select_press_ms < MAIN_DOUBLE_MS) {
			app->select_press_object = MAIN_NONE;
			app_box_edit(app, index);
			return;
		}

		/* The press, for a double click. */
		app->select_press_object = index;
		app->select_press_ms = time_ms;

		/* Chosen (logged when it is another), and moved by the drag. */
		if (index != app->selected)
			app_select(app, index, 1);
		app->drag = MAIN_DRAG_MOVE;
	}

	/* The drag starts here, from the object's corners, moving nothing yet. */
	app->drag_x = page_x;
	app->drag_y = page_y;
	memcpy(app->drag_quad, object.quad, sizeof(app->drag_quad));
	app->drag_map[0] = 1.0;
	app->drag_map[1] = 0.0;
	app->drag_map[2] = 0.0;
	app->drag_map[3] = 1.0;
	app->drag_map[4] = 0.0;
	app->drag_map[5] = 0.0;
	app->drag_moved = 0;
	app->drag_previewed_at = 0;
	app->redraw = 1;
}

/*
 * Follows a drag of the Select tool: the object moved by the point's way
 * from where the drag started, or sized from the corner opposite the
 * handle (its proportions kept; Shift sizes each side apart), within
 * MAIN_SIDE_MIN points and MAIN_SIDE_TIMES_PAGE times the page.
 */
static void
app_select_motion(
	struct notes_app *app,
	float x,
	float y)
{
	const struct notes_page *page;
	double anchor_x;
	double anchor_y;
	double from_x;
	double from_y;
	double to_x;
	double to_y;
	double across;
	double down;
	double lowest;
	double highest;
	double scale_x;
	double scale_y;
	double page_x;
	double page_y;
	unsigned opposite;

	/* A drag under way, at the point on the page. */
	if (app->drag == MAIN_DRAG_NONE)
		return;
	page_x = (double)((x - app->view.x) / app->view.scale);
	page_y = (double)((y - app->view.y) / app->view.scale);

	/* Moved: by the point's way. */
	if (app->drag == MAIN_DRAG_MOVE) {
		app->drag_map[0] = 1.0;
		app->drag_map[3] = 1.0;
		app->drag_map[4] = page_x - (double)app->drag_x;
		app->drag_map[5] = page_y - (double)app->drag_y;
		app->drag_moved = 1;
		app->redraw = 1;
		return;
	}

	/* Sized: from the opposite corner, as far as the handle's corner went. */
	opposite = (app->drag_corner + 2U) % 4U;
	anchor_x = app->drag_quad[opposite * 2U];
	anchor_y = app->drag_quad[opposite * 2U + 1U];
	from_x = app->drag_quad[app->drag_corner * 2U] - anchor_x;
	from_y = app->drag_quad[app->drag_corner * 2U + 1U] - anchor_y;
	to_x = page_x - anchor_x;
	to_y = page_y - anchor_y;
	scale_x = 1.0;
	scale_y = 1.0;
	if ((app->window.modifiers & NOTES_MODIFIER_SHIFT) != 0U && app->selected_kind != PDF_EDIT_TEXT) {
		if (from_x > 1e-6 || from_x < -1e-6)
			scale_x = to_x / from_x;
		if (from_y > 1e-6 || from_y < -1e-6)
			scale_y = to_y / from_y;
	} else if (from_x * from_x + from_y * from_y > 1e-12) {
		scale_x = (to_x * from_x + to_y * from_y) / (from_x * from_x + from_y * from_y);
		scale_y = scale_x;
	}

	/* Within the smallest and the largest sides: the object's sides now, the page's. */
	page = app->document.pages[app->page];
	across = hypot(app->drag_quad[2] - app->drag_quad[0], app->drag_quad[3] - app->drag_quad[1]);
	down = hypot(app->drag_quad[6] - app->drag_quad[0], app->drag_quad[7] - app->drag_quad[1]);
	lowest = MAIN_SIDE_MIN / fmin(fmax(across, 1e-6), fmax(down, 1e-6));
	highest = MAIN_SIDE_TIMES_PAGE * fmax((double)page->width, (double)page->height) / fmax(fmax(across, down), 1e-6);
	scale_x = fmin(fmax(scale_x, lowest), highest);
	scale_y = fmin(fmax(scale_y, lowest), highest);

	/* The map: the anchor stays, the rest scales about it. */
	app->drag_map[0] = scale_x;
	app->drag_map[1] = 0.0;
	app->drag_map[2] = 0.0;
	app->drag_map[3] = scale_y;
	app->drag_map[4] = anchor_x - scale_x * anchor_x;
	app->drag_map[5] = anchor_y - scale_y * anchor_y;
	app->drag_moved = 1;
	app->redraw = 1;
}

/* Ends a drag of the Select tool: the map it made becomes one change (logged), or a press without a drag changes nothing. */
static void
app_select_release(
	struct notes_app *app)
{
	double map[6];
	unsigned drag;
	int moved;

	/* A drag under way. */
	drag = app->drag;
	if (drag == MAIN_DRAG_NONE)
		return;
	app->drag = MAIN_DRAG_NONE;
	memcpy(map, app->drag_map, sizeof(map));

	/* Whether it moved the object at all. */
	moved = fabs(map[0] - 1.0) > 1e-6 || fabs(map[3] - 1.0) > 1e-6 || fabs(map[4]) > 1e-6 || fabs(map[5]) > 1e-6;
	if (!moved) {
		/* A preview drawn goes with the page's editor, made again. */
		if (app->drag_previews > 0U) {
			notes_page_close_editor(app->document.pages[app->page]);
			app->drag_previews++;
		}

		/* Nothing changed. */
		app->redraw = 1;
		return;
	}

	/* The change, and the tests' line. */
	app_apply_map(app, map);
	if (drag == MAIN_DRAG_MOVE)
		printf("NOTES EDIT move page=%lu object=%lu dx=%.1f dy=%.1f\n", (unsigned long)app->page, (unsigned long)app->selected, map[4], map[5]);
	else
		printf("NOTES EDIT resize page=%lu object=%lu sx=%.3f sy=%.3f\n", (unsigned long)app->page, (unsigned long)app->selected, map[0], map[3]);
	fflush(stdout);
}

/*
 * Chooses an object of the page shown by its index in the page's editor
 * (logged for the tests when asked), and notes what the toolbar shows of
 * it: its kind, whether it was inserted, whether it has an edit.
 */
static void
app_select(
	struct notes_app *app,
	size_t index,
	int logged)
{
	struct pdf_page_editor *editor;
	struct pdf_edit_object object;
	struct notes_edit state;
	const char *kind;
	int error;

	/* The object and its state. */
	memset(&object, 0, sizeof(object));
	object.size = sizeof(object);
	error = notes_page_editor(&app->document, app->page, &editor);
	if (error == 0)
		error = pdf_page_editor_object(editor, index, &object);
	if (error == 0)
		error = notes_page_object(&app->document, app->page, index, &state);
	if (error != 0) {
		app_deselect(app);
		return;
	}

	/* Chosen. */
	app->selected = index;
	app->selected_kind = object.kind;
	app->selected_inserted = (state.flags & NOTES_EDIT_INSERTED) != 0U;
	app->selected_edited = state.flags != 0U;
	app->toolbar_dirty = 1;
	app->redraw = 1;

	/* A line of text's: whether its words can change, its font and its size (an inserted text's own, or the line's). */
	app->selected_fixed = (object.flags & (PDF_EDIT_OBJECT_TEXT_FIXED | PDF_EDIT_OBJECT_INVISIBLE)) != 0U;
	app->selected_font = PDF_EDIT_FONT_ORIGINAL;
	if ((state.flags & NOTES_EDIT_TEXT) != 0U)
		app->selected_font = state.font;
	app->selected_size = (float)object.font_size;
	if (app->selected_inserted)
		app->selected_size = state.text_size;
	if (!logged)
		return;

	/* The tests' line (a text's first words too), and the clip that hides what moves out of it. */
	kind = "graphic";
	if (object.kind == PDF_EDIT_IMAGE)
		kind = "image";
	if (object.kind == PDF_EDIT_TEXT)
		kind = "text";
	printf("NOTES EDIT select page=%lu object=%lu kind=%s inserted=%d clipped=%d", (unsigned long)app->page, (unsigned long)index, kind,
	       app->selected_inserted, (object.flags & PDF_EDIT_OBJECT_CLIPPED) != 0U);
	if (object.kind == PDF_EDIT_TEXT && object.text != NULL)
		printf(" fixed=%d text=\"%.40s\"", app->selected_fixed, object.text);
	printf("\n");
	fflush(stdout);
	if ((object.flags & PDF_EDIT_OBJECT_CLIPPED) != 0U)
		app_status(app, "Clipped: what moves out of the clip stays hidden");
}

/* Lets the chosen object go. */
static void
app_deselect(
	struct notes_app *app)
{
	/* None chosen, no drag. */
	if (app->selected == MAIN_NONE && app->drag == MAIN_DRAG_NONE)
		return;
	app->selected = MAIN_NONE;
	app->drag = MAIN_DRAG_NONE;
	app->toolbar_dirty = 1;
	app->redraw = 1;
}

/* Gives the chosen object's state (its edit, or its key as the page has it).  Returns 0 or an errno value. */
static int
app_selected_state(
	struct notes_app *app,
	struct notes_edit *state)
{
	/* None chosen. */
	if (app->selected == MAIN_NONE)
		return ENOENT;

	/* The state by its index. */
	return notes_page_object(&app->document, app->page, app->selected, state);
}

/*
 * Puts the chosen object where the drag has it in the page's editor, for
 * the preview only (the drag's end makes the change; a drop makes the
 * editor again): an inserted image's placement is the drag's map, a page's
 * object's is its placement so far followed by the map.
 */
static void
app_drag_preview(
	struct notes_app *app,
	struct pdf_page_editor *editor)
{
	struct notes_edit state;
	double placement[6];
	size_t item;
	int error;

	/* The object's state. */
	error = app_selected_state(app, &state);
	if (error != 0)
		return;

	/* Its placement so far (a page's own placed object's), then the drag's map. */
	placement[0] = 1.0;
	placement[1] = 0.0;
	placement[2] = 0.0;
	placement[3] = 1.0;
	placement[4] = 0.0;
	placement[5] = 0.0;
	if ((state.flags & NOTES_EDIT_INSERTED) == 0U && (state.flags & NOTES_EDIT_PLACED) != 0U) {
		for (item = 0; item < 6U; item++)
			placement[item] = (double)state.transform[item];
	}

	/* The drag's map after it. */
	app_map_multiply(placement, app->drag_map, placement);

	/* In the editor (a refusal leaves it where it was). */
	(void)pdf_page_editor_place(editor, app->selected, placement);
}

/*
 * Moves or sizes the chosen object by a map of the page's shown space, as
 * one change: an inserted image's placement followed by the map, a page's
 * object placed (its placement so far followed by the map).
 */
static void
app_apply_map(
	struct notes_app *app,
	const double map[6])
{
	struct notes_edit state;
	double transform[6];
	size_t item;
	int error;

	/* The object's state. */
	error = app_selected_state(app, &state);
	if (error != 0)
		return;

	/* Its transform so far (the identity for an object not placed yet), followed by the map. */
	for (item = 0; item < 6U; item++)
		transform[item] = (double)state.transform[item];
	if ((state.flags & (NOTES_EDIT_INSERTED | NOTES_EDIT_PLACED)) == 0U) {
		transform[0] = 1.0;
		transform[1] = 0.0;
		transform[2] = 0.0;
		transform[3] = 1.0;
		transform[4] = 0.0;
		transform[5] = 0.0;
	}

	/* The map after it, back on the edit's numbers. */
	app_map_multiply(transform, map, transform);
	for (item = 0; item < 6U; item++)
		state.transform[item] = (float)transform[item];
	if ((state.flags & NOTES_EDIT_INSERTED) == 0U)
		state.flags |= NOTES_EDIT_PLACED;

	/* The change; the object stays chosen. */
	error = notes_document_edit_object(&app->document, app->page, &state);
	if (error != 0) {
		printf("NOTES EDIT failed page=%lu object=%lu error=%d\n", (unsigned long)app->page, (unsigned long)app->selected, error);
		app_status(app, "Could not move the object");
		return;
	}

	/* Succeeded: the object, still chosen, where the map put it. */
	app_select(app, app->selected, 0);
	app_changed(app);
}

/* Deletes the chosen object, as one change: an inserted image taken off, a page's object deleted. */
static void
app_delete_object(
	struct notes_app *app)
{
	struct notes_edit state;
	size_t object;
	int error;

	/* The object's state. */
	error = app_selected_state(app, &state);
	if (error != 0)
		return;

	/* Taken off, or deleted. */
	if ((state.flags & NOTES_EDIT_INSERTED) != 0U) {
		error = notes_document_reset_object(&app->document, app->page, &state);
	} else {
		state.flags = NOTES_EDIT_DELETED;
		state.image = NULL;
		error = notes_document_edit_object(&app->document, app->page, &state);
	}

	/* A failure is shown; otherwise the object is gone and let go. */
	if (error != 0) {
		app_status(app, "Could not delete the object");
		return;
	}

	/* Gone (the tests' line). */
	object = app->selected;
	app_deselect(app);
	printf("NOTES EDIT delete page=%lu object=%lu\n", (unsigned long)app->page, (unsigned long)object);
	fflush(stdout);
	app_changed(app);
}

/* Puts the chosen object of the page back as the page has it (Reset, design.md [L10]), as one change. */
static void
app_reset_object(
	struct notes_app *app)
{
	struct notes_edit state;
	int error;

	/* A page's own object with an edit. */
	if (app->selected == MAIN_NONE || app->selected_inserted || !app->selected_edited)
		return;
	error = app_selected_state(app, &state);
	if (error == 0)
		error = notes_document_reset_object(&app->document, app->page, &state);
	if (error != 0) {
		app_status(app, "Could not reset the object");
		return;
	}

	/* The tests' line; the object stays chosen. */
	printf("NOTES EDIT reset page=%lu object=%lu\n", (unsigned long)app->page, (unsigned long)app->selected);
	fflush(stdout);
	app_select(app, app->selected, 0);
	app_changed(app);
}

/*
 * Puts an image file on the page shown (ws175-p008): in the chosen object's
 * place, or inserted at the middle of the page's part in the window, at
 * most half the page's width and height and its own size at 72 dpi
 * (design.md section 5.3), then chosen with the Select tool.
 */
static void
app_put_image(
	struct notes_app *app,
	const char *path,
	unsigned purpose)
{
	struct pdf_page_editor *editor;
	struct notes_image *image;
	struct notes_page *page;
	struct notes_edit state;
	double shown_width;
	double shown_height;
	double left;
	double top;
	double right;
	double bottom;
	double scale;
	double width;
	double height;
	unsigned status;
	int error;

	/* The image. */
	error = notes_picture_load(&app->document, path, &image);
	if (error != 0) {
		printf("NOTES EDIT image failed error=%d path=%s\n", error, path);
		fflush(stdout);
		if (error == ENOTSUP)
			app_status(app, "This image cannot be put in a PDF (JPEG or PNG; not CMYK)");
		else if (error == E2BIG)
			app_status(app, "The image is too large");
		else
			app_status(app, "Could not read the image");
		return;
	}

	/* In the chosen object's place. */
	if (purpose == MAIN_CHOOSE_REPLACE) {
		error = app_selected_state(app, &state);
		state.flags |= NOTES_EDIT_IMAGE;
		state.image = image;
		if (error == 0)
			error = notes_document_edit_object(&app->document, app->page, &state);
		if (error == 0) {
			printf("NOTES EDIT replace page=%lu object=%lu image=%lux%lu\n", (unsigned long)app->page, (unsigned long)app->selected,
			       (unsigned long)image->width, (unsigned long)image->height);
			app_select(app, app->selected, 0);
		}
	} else {
		/* Inserted: a page that can be edited. */
		error = notes_page_editor(&app->document, app->page, &editor);
		status = 0U;
		if (error == 0)
			status = pdf_page_editor_status(editor);
		if ((status & PDF_EDIT_PAGE_READ_ONLY) != 0U)
			error = EPERM;

		/* The page's part in the window. */
		page = app->document.pages[app->page];
		left = fmax(0.0, (double)(-app->view.x / app->view.scale));
		top = fmax(0.0, (double)(((float)NOTES_TOOLBAR_HEIGHT - app->view.y) / app->view.scale));
		right = fmin((double)page->width, (double)(((float)app->renderer.extent.width - app->view.x) / app->view.scale));
		bottom = fmin((double)page->height, (double)(((float)app->renderer.extent.height - app->view.y) / app->view.scale));
		if (right <= left || bottom <= top) {
			left = 0.0;
			top = 0.0;
			right = (double)page->width;
			bottom = (double)page->height;
		}

		/* The image's size as shown (a quarter turn swaps its sides), at most half the page's and its own. */
		shown_width = (double)image->width;
		shown_height = (double)image->height;
		if (image->orientation >= 5) {
			shown_width = (double)image->height;
			shown_height = (double)image->width;
		}

		/* At most half the page's sides and its own size. */
		scale = fmin(1.0, fmin((double)page->width / 2.0 / shown_width, (double)page->height / 2.0 / shown_height));
		width = shown_width * scale;
		height = shown_height * scale;

		/* Its unit square onto the middle (its top left where (0, 1) goes). */
		memset(&state, 0, sizeof(state));
		state.flags = NOTES_EDIT_INSERTED | NOTES_EDIT_IMAGE;
		state.id = app->document.next_id;
		state.image = image;
		state.transform[0] = (float)width;
		state.transform[3] = (float)-height;
		state.transform[4] = (float)((left + right) / 2.0 - width / 2.0);
		state.transform[5] = (float)((top + bottom) / 2.0 + height / 2.0);
		if (error == 0) {
			app->document.next_id++;
			error = notes_document_edit_object(&app->document, app->page, &state);
		}

		/* Chosen with the Select tool, the last of the page's objects. */
		if (error == 0)
			error = notes_page_editor(&app->document, app->page, &editor);
		if (error == 0) {
			app->tool = NOTES_ACTION_SELECT;
			app_select(app, pdf_page_editor_count(editor) - 1U, 0);
			printf("NOTES EDIT insert page=%lu kind=image object=%lu image=%lux%lu\n", (unsigned long)app->page, (unsigned long)app->selected,
			       (unsigned long)image->width, (unsigned long)image->height);
		}
	}

	/* The caller's hold on the image goes (the edit holds its own); a failure is shown. */
	notes_image_release(image);
	fflush(stdout);
	if (error != 0) {
		printf("NOTES EDIT image failed error=%d path=%s\n", error, path);
		fflush(stdout);
		app_status(app, "Could not put the image on this page");
		return;
	}

	/* Succeeded: the image is on the page. */
	app_changed(app);
}

/*
 * Draws the chosen object's frame -- its corners where a drag has them --
 * and a handle on each corner, in Kei's blue.
 */
static void
app_selection_draw(
	struct notes_app *app,
	const struct notes_view *view)
{
	struct pdf_page_editor *editor;
	struct pdf_edit_object object;
	struct pdf_point edge[4];
	double quad[8];
	double along_x;
	double along_y;
	double length;
	double thick;
	float x;
	float y;
	unsigned corner;
	unsigned next;
	int error;

	/* The Select tool's chosen object. */
	if (app->tool != NOTES_ACTION_SELECT || app->selected == MAIN_NONE)
		return;

	/* Its corners: where the drag has them, or the editor's. */
	if (app->drag != MAIN_DRAG_NONE) {
		for (corner = 0; corner < 4U; corner++)
			app_map_point(app->drag_map, app->drag_quad[corner * 2U], app->drag_quad[corner * 2U + 1U], &quad[corner * 2U], &quad[corner * 2U + 1U]);
	} else {
		memset(&object, 0, sizeof(object));
		object.size = sizeof(object);
		error = notes_page_editor(&app->document, app->page, &editor);
		if (error == 0)
			error = pdf_page_editor_object(editor, app->selected, &object);
		if (error != 0)
			return;
		memcpy(quad, object.quad, sizeof(quad));
	}

	/* Each side, a band two pixels wide. */
	thick = 1.0 / (double)view->scale;
	for (corner = 0; corner < 4U; corner++) {
		next = (corner + 1U) % 4U;
		along_x = quad[next * 2U] - quad[corner * 2U];
		along_y = quad[next * 2U + 1U] - quad[corner * 2U + 1U];
		length = hypot(along_x, along_y);
		if (length < 1e-9)
			continue;
		edge[0].x = quad[corner * 2U] - along_y / length * thick;
		edge[0].y = quad[corner * 2U + 1U] + along_x / length * thick;
		edge[1].x = quad[next * 2U] - along_y / length * thick;
		edge[1].y = quad[next * 2U + 1U] + along_x / length * thick;
		edge[2].x = quad[next * 2U] + along_y / length * thick;
		edge[2].y = quad[next * 2U + 1U] - along_x / length * thick;
		edge[3].x = quad[corner * 2U] + along_y / length * thick;
		edge[3].y = quad[corner * 2U + 1U] - along_x / length * thick;
		notes_frame_polygon(&app->frame, edge, 4U, view, MAIN_SELECT_COLOR);
	}

	/* A handle on each corner: white with a blue square in it. */
	for (corner = 0; corner < 4U; corner++) {
		x = view->x + (float)quad[corner * 2U] * view->scale;
		y = view->y + (float)quad[corner * 2U + 1U] * view->scale;
		notes_frame_rect(&app->frame, x - MAIN_HANDLE_SIZE / 2.0f - 1.0f, y - MAIN_HANDLE_SIZE / 2.0f - 1.0f, MAIN_HANDLE_SIZE + 2.0f,
				 MAIN_HANDLE_SIZE + 2.0f, 0xffffffffU);
		notes_frame_rect(&app->frame, x - MAIN_HANDLE_SIZE / 2.0f, y - MAIN_HANDLE_SIZE / 2.0f, MAIN_HANDLE_SIZE, MAIN_HANDLE_SIZE, MAIN_SELECT_COLOR);
	}
}

/* Multiplies two maps of the plane (a point goes through left, then right); product may be either. */
static void
app_map_multiply(
	const double left[6],
	const double right[6],
	double product[6])
{
	double result[6];

	/* The product of the 3 by 3 matrices whose third column is 0, 0, 1. */
	result[0] = left[0] * right[0] + left[1] * right[2];
	result[1] = left[0] * right[1] + left[1] * right[3];
	result[2] = left[2] * right[0] + left[3] * right[2];
	result[3] = left[2] * right[1] + left[3] * right[3];
	result[4] = left[4] * right[0] + left[5] * right[2] + right[4];
	result[5] = left[4] * right[1] + left[5] * right[3] + right[5];
	memcpy(product, result, sizeof(result));
}

/* Maps a point of the plane. */
static void
app_map_point(
	const double map[6],
	double x,
	double y,
	double *mapped_x,
	double *mapped_y)
{
	/* The point times the map. */
	*mapped_x = x * map[0] + y * map[2] + map[4];
	*mapped_y = x * map[1] + y * map[3] + map[5];
}

/* Gives the object an undo entry of an edit names (its state after or before, by value; the image not held). */
static void
app_edit_object(
	const struct notes_undo *entry,
	struct notes_edit *which)
{
	/* The state after, or before when there is none after. */
	memset(which, 0, sizeof(*which));
	if (entry->edit_after != NULL)
		*which = *entry->edit_after;
	else if (entry->edit_before != NULL)
		*which = *entry->edit_before;
}

/*
 * Chooses again, after an undo or a redo of an edit, the object it changed
 * when the page shown has it and it is not deleted (ws175-p008: its handles
 * stay for the next drag).
 */
static void
app_reselect(
	struct notes_app *app,
	const struct notes_edit *which)
{
	struct pdf_page_editor *editor;
	struct pdf_edit_object object;
	size_t index;
	int error;

	/* With the Select tool, the object on the page shown, not deleted. */
	if (app->tool != NOTES_ACTION_SELECT)
		return;
	error = notes_page_object_index(&app->document, app->page, which, &index);
	if (error == 0)
		error = notes_page_editor(&app->document, app->page, &editor);
	memset(&object, 0, sizeof(object));
	object.size = sizeof(object);
	if (error == 0)
		error = pdf_page_editor_object(editor, index, &object);
	if (error != 0 || (object.flags & PDF_EDIT_OBJECT_DELETED) != 0U)
		return;

	/* Chosen again. */
	app_select(app, index, 0);
}

/*
 * Starts a press of the Text tool at a point of the window (ws175-p008,
 * design.md section 7.1): on a line of text it opens the line's words in
 * the text box; elsewhere on a page that can be edited the press is kept,
 * and its release opens the box for new words there.
 */
static void
app_text_press(
	struct notes_app *app,
	float x,
	float y)
{
	struct pdf_page_editor *editor;
	struct pdf_edit_object object;
	const struct notes_page *page;
	float page_x;
	float page_y;
	size_t index;
	unsigned status;
	int error;

	/* A point on the page. */
	app->text_pressing = 0;
	page = app->document.pages[app->page];
	page_x = (x - app->view.x) / app->view.scale;
	page_y = (y - app->view.y) / app->view.scale;
	if (page_x < 0.0f || page_y < 0.0f || page_x >= page->width || page_y >= page->height)
		return;

	/* The page's editor, of a page that can be edited. */
	error = notes_page_editor(&app->document, app->page, &editor);
	if (error != 0) {
		printf("NOTES EDIT page failed page=%lu error=%d\n", (unsigned long)app->page, error);
		fflush(stdout);
		app_status(app, "The objects of this page cannot be edited");
		return;
	}

	/* A page with a stream that cannot be read cannot be edited (design.md [M5]). */
	status = pdf_page_editor_status(editor);
	if ((status & PDF_EDIT_PAGE_READ_ONLY) != 0U) {
		app_status(app, "This page cannot be edited: part of it cannot be read");
		return;
	}

	/* A line of text under the point: its words in the box. */
	memset(&object, 0, sizeof(object));
	object.size = sizeof(object);
	error = pdf_page_editor_hit(editor, (double)page_x, (double)page_y, &index);
	if (error == 0)
		error = pdf_page_editor_object(editor, index, &object);
	if (error == 0 && object.kind == PDF_EDIT_TEXT) {
		app_box_edit(app, index);
		return;
	}

	/* Elsewhere: new words, where the press started, once it ends. */
	app->text_pressing = 1;
	app->text_press_x = x;
	app->text_press_y = y;
}

/*
 * Ends a press of the Text tool: the box for new words opens where it
 * started, wrapping them at the width it was dragged across (a press
 * without a drag: not wrapped).
 */
static void
app_text_release(
	struct notes_app *app,
	const struct notes_input *input)
{
	float across;
	float width;

	/* A press that opens new words. */
	if (!app->text_pressing)
		return;
	app->text_pressing = 0;

	/* The width dragged across, in points, when it is a drag. */
	width = 0.0f;
	if (input != NULL) {
		across = input->x - app->text_press_x;
		if (across >= MAIN_BOX_DRAG_MIN)
			width = across / app->view.scale;
	}

	/* The box at the press's place on the page. */
	app_box_new(app, (app->text_press_x - app->view.x) / app->view.scale, (app->text_press_y - app->view.y) / app->view.scale, width);
}

/*
 * Opens the text box on a line of text of the page shown, by its index in
 * the page's editor: a line of the page's own in a one-line box under it
 * (the page shows the words as they are typed), an inserted text in a box
 * of several lines under it, in its font, size and colour.  A line whose
 * words cannot change is not opened (the status says why).
 */
static void
app_box_edit(
	struct notes_app *app,
	size_t index)
{
	struct pdf_page_editor *editor;
	struct pdf_edit_object object;
	struct notes_edit state;
	struct kl_rect rect;
	const char *text;
	unsigned shape;
	size_t length;
	int error;

	/* The object and its state. */
	memset(&object, 0, sizeof(object));
	object.size = sizeof(object);
	error = notes_page_editor(&app->document, app->page, &editor);
	if (error == 0)
		error = pdf_page_editor_object(editor, index, &object);
	if (error == 0)
		error = notes_page_object(&app->document, app->page, index, &state);
	if (error != 0 || object.kind != PDF_EDIT_TEXT)
		return;

	/* Words that cannot change (their font's characters are not known, or they are invisible). */
	if ((object.flags & (PDF_EDIT_OBJECT_TEXT_FIXED | PDF_EDIT_OBJECT_INVISIBLE)) != 0U) {
		app_status(app, "The words of this line cannot be changed (they can be moved or deleted)");
		return;
	}

	/* What the box edits: the object, its state without its words, its font. */
	app->box_object = index;
	app->box_edit = state;
	app->box_edit.text = NULL;
	app->box_edit.image = NULL;
	app->box_font = PDF_EDIT_FONT_ORIGINAL;
	if ((state.flags & NOTES_EDIT_TEXT) != 0U)
		app->box_font = state.font;
	app->box_initial_font = app->box_font;
	app->box_size = 0.0f;

	/* Its place, kept in page points: the box follows the object as the page moves. */
	memcpy(app->box_quad, object.quad, sizeof(app->box_quad));

	/* An inserted text: its words, size and colour, in a box of several lines. */
	if ((state.flags & NOTES_EDIT_INSERTED) != 0U) {
		app->box_kind = MAIN_BOX_INSERTED;
		app->box_size = state.text_size;
		app->color = app_color_index(state.color);
		app->box_height = MAIN_BOX_AREA_HEIGHT;
		app_box_rect(app, &rect);
		app_box_start(app, NOTES_BOX_AREA, &rect, state.text);
		return;
	}

	/* A line of the page's own: its words as the page has them now, in one line. */
	app->box_kind = MAIN_BOX_LINE;
	app->box_height = MAIN_BOX_LINE_HEIGHT;
	shape = NOTES_BOX_LINE;
	text = "";
	if (object.text != NULL)
		text = object.text;

	/* A line longer than a field holds is edited in a box of several lines, its line ends made spaces when kept (ws177-p012). */
	length = strlen(text);
	if (length + 1U >= KL_FIELD_MAX) {
		app->box_height = MAIN_BOX_AREA_HEIGHT;
		shape = NOTES_BOX_AREA;
	}

	/* Opened under the line. */
	app_box_rect(app, &rect);
	app_box_start(app, shape, &rect, text);
}

/*
 * Opens the text box for new words at a point of the page shown (points),
 * wrapped at a width (points; 0: not wrapped), in the Text tool's font and
 * size and the pen's colour: a box of several lines whose top left is the
 * words' place.
 */
static void
app_box_new(
	struct notes_app *app,
	float x,
	float y,
	float width)
{
	struct kl_rect rect;

	/* What the box edits: new words there. */
	app->box_kind = MAIN_BOX_NEW;
	app->box_object = MAIN_NONE;
	memset(&app->box_edit, 0, sizeof(app->box_edit));
	app->box_x = x;
	app->box_y = y;
	app->box_width = width;
	app->box_font = app->text_font;
	app->box_initial_font = app->box_font;
	app->box_size = app->text_size;

	/* The box from the point, as wide as the words wrap, inside the window. */
	app->box_height = MAIN_BOX_AREA_HEIGHT;
	app_box_rect(app, &rect);

	/* Opened empty. */
	app_box_start(app, NOTES_BOX_AREA, &rect, "");
}

/*
 * Places the box of a size under an object's corners (page points), or
 * above them when there is no room under them, inside the window and
 * below the toolbar.
 */
static void
app_box_place(
	struct notes_app *app,
	const double quad[8],
	int width,
	int height,
	struct kl_rect *rect)
{
	double left;
	double top;
	double bottom;
	unsigned corner;

	/* The object's bounds in the window. */
	left = quad[0];
	top = quad[1];
	bottom = quad[1];
	for (corner = 1; corner < 4U; corner++) {
		left = fmin(left, quad[corner * 2U]);
		top = fmin(top, quad[corner * 2U + 1U]);
		bottom = fmax(bottom, quad[corner * 2U + 1U]);
	}

	/* In the window's pixels. */
	left = (double)app->view.x + left * (double)app->view.scale;
	top = (double)app->view.y + top * (double)app->view.scale;
	bottom = (double)app->view.y + bottom * (double)app->view.scale;

	/* Under the object, or above it when the window ends first. */
	rect->x = (int)left;
	rect->y = (int)bottom + MAIN_BOX_GAP;
	rect->width = width;
	rect->height = height;
	if (rect->y + height > (int)app->renderer.extent.height - MAIN_BOX_GAP)
		rect->y = (int)top - MAIN_BOX_GAP - height;

	/* Inside the window, below the toolbar. */
	if (rect->x + width > (int)app->renderer.extent.width - MAIN_BOX_GAP)
		rect->x = (int)app->renderer.extent.width - MAIN_BOX_GAP - width;
	if (rect->x < MAIN_BOX_GAP)
		rect->x = MAIN_BOX_GAP;
	if (rect->y < (int)NOTES_TOOLBAR_HEIGHT + MAIN_BOX_GAP)
		rect->y = (int)NOTES_TOOLBAR_HEIGHT + MAIN_BOX_GAP;
}

/*
 * Works out the box's rectangle in the window as the page is shown now:
 * under (or above) the object edited, as wide as it is across (at least
 * the box's least), or for new words from their place, as wide as they
 * wrap; inside the window and below the toolbar.
 */
static void
app_box_rect(
	struct notes_app *app,
	struct kl_rect *rect)
{
	double left;
	double right;
	int pixels;

	/* An object's box: its width the object's across, in the window's pixels now. */
	if (app->box_kind != MAIN_BOX_NEW) {
		left = fmin(fmin(app->box_quad[0], app->box_quad[2]), fmin(app->box_quad[4], app->box_quad[6]));
		right = fmax(fmax(app->box_quad[0], app->box_quad[2]), fmax(app->box_quad[4], app->box_quad[6]));
		pixels = (int)((right - left) * (double)app->view.scale) + 24;
		if (pixels < MAIN_BOX_WIDTH_MIN)
			pixels = MAIN_BOX_WIDTH_MIN;
		app_box_place(app, app->box_quad, pixels, app->box_height, rect);
		return;
	}

	/* New words: from their place, as wide as they wrap. */
	pixels = (int)(app->box_width * app->view.scale);
	if (pixels < MAIN_BOX_WIDTH_MIN)
		pixels = MAIN_BOX_WIDTH_MIN;
	rect->x = (int)(app->view.x + app->box_x * app->view.scale);
	rect->y = (int)(app->view.y + app->box_y * app->view.scale);
	rect->width = pixels;
	rect->height = app->box_height;

	/* Inside the window, below the toolbar. */
	if (rect->x + rect->width > (int)app->renderer.extent.width - MAIN_BOX_GAP)
		rect->x = (int)app->renderer.extent.width - MAIN_BOX_GAP - rect->width;
	if (rect->y + rect->height > (int)app->renderer.extent.height - MAIN_BOX_GAP)
		rect->y = (int)app->renderer.extent.height - MAIN_BOX_GAP - rect->height;
	if (rect->x < MAIN_BOX_GAP)
		rect->x = MAIN_BOX_GAP;
	if (rect->y < (int)NOTES_TOOLBAR_HEIGHT + MAIN_BOX_GAP)
		rect->y = (int)NOTES_TOOLBAR_HEIGHT + MAIN_BOX_GAP;
}

/*
 * Moves the open box to where the page shown now puts it (ws177-p012: the
 * page zoomed or scrolled, by a finger or the keys, or the window sized).
 * Returns 1 when it moved (its frame is then drawn again), 0 otherwise.
 */
static int
app_box_follow(
	struct notes_app *app)
{
	struct kl_rect rect;

	/* Only an open box. */
	if (!app->box.open)
		return 0;

	/* Its place now; the same place is no move. */
	app_box_rect(app, &rect);
	if (rect.x == app->box.rect.x &&
	    rect.y == app->box.rect.y &&
	    rect.width == app->box.rect.width &&
	    rect.height == app->box.rect.height)
		return 0;

	/* The box and the window's fingers take the new place. */
	app->box.rect = rect;
	notes_window_box_rect(&app->window, &rect);

	/* The tests' line. */
	printf("NOTES TEXT box moved rect=%d,%d,%d,%d\n", rect.x, rect.y, rect.width, rect.height);
	fflush(stdout);

	/* Succeeded: moved. */
	return 1;
}

/*
 * Opens the box of a shape in a rectangle with words in it: the window's
 * keys and an input method's text go to it, and it is drawn (logged for
 * the tests).
 */
static void
app_box_start(
	struct notes_app *app,
	unsigned shape,
	const struct kl_rect *rect,
	const char *text)
{
	static const char *const kinds[] = { "line", "inserted", "new" };
	int error;

	/* The box, with the keyboard. */
	error = notes_box_open(&app->box, shape, rect, text);
	if (error != 0) {
		printf("NOTES TEXT box failed error=%d\n", error);
		fflush(stdout);
		app_status(app, "The text box cannot be shown: its font is missing");
		return;
	}

	/* The window's input goes to it (a finger down on its rectangle too); the page shows nothing typed yet. */
	notes_window_box(&app->window, 1);
	notes_window_box_rect(&app->window, rect);
	app->box_previewing = 0;
	app->box_changed = 0;
	app->box_previewed_at = 0;

	/* The tests' line. */
	printf("NOTES TEXT box open kind=%s page=%lu object=%ld font=%s rect=%d,%d,%d,%d\n", kinds[app->box_kind], (unsigned long)app->page,
	       (long)app->box_object, app_font_name(app->box_font), rect->x, rect->y, rect->width, rect->height);
	fflush(stdout);

	/* Drawn, which also asks for the input method. */
	app->toolbar_dirty = 1;
	app_box_frame(app);
}

/*
 * Draws the box's frame, which takes the input given to it, and carries
 * out what its widget reported: changed words are shown on the page (a
 * line's, after the preview's time), Enter in a line's box and Esc close
 * it, keeping its words.
 */
static void
app_box_frame(
	struct notes_app *app)
{
	unsigned reported;
	int error;

	/* The box where the page shown puts it now. */
	(void)app_box_follow(app);

	/* The frame. */
	error = notes_box_draw(&app->box, &app->renderer, app->window.kui, app_microseconds());
	if (error != 0) {
		printf("NOTES TEXT box failed error=%d\n", error);
		fflush(stdout);
		app_status(app, "The text box cannot be drawn");
		app_box_close(app);
		return;
	}

	/* Shown with the next frame; what the widget reported is logged for the tests (the words' bytes). */
	app->redraw = 1;
	reported = notes_box_take(&app->box);
	if (reported != 0U) {
		printf("NOTES TEXT box reported=%u bytes=%lu\n", reported, (unsigned long)strlen(notes_box_text(&app->box)));
		fflush(stdout);
	}

	/* Changed words: a line's shown on the page soon. */
	if ((reported & KL_FIELD_CHANGED) != 0U && app->box_kind == MAIN_BOX_LINE)
		app->box_changed = 1;

	/* Done: the words kept (or the box stays when the editor refuses them). */
	if ((reported & (KL_FIELD_SUBMITTED | KL_FIELD_CANCELLED)) != 0U)
		(void)app_box_commit(app);
}

/*
 * Closes the box keeping its words, as one change, once the page's editor
 * takes them (ws175-p004: the model holds no words the PDF cannot be
 * written with): a line's new words and font, an inserted text's words,
 * font, size and colour, or new words put on the page (and chosen with
 * the Select tool's frame when it is that tool).  Words emptied delete
 * the line or take the inserted text off; words unchanged change nothing.
 * Returns 1 when the box closed, 0 when the editor refused the words (the
 * status says why; the box stays with the keyboard).
 */
static int
app_box_commit(
	struct notes_app *app)
{
	static const char *const kinds[] = { "line", "inserted", "new" };
	static char text[KL_TEXT_AREA_MAX];
	struct pdf_page_editor *editor;
	struct notes_edit state;
	uint32_t color;
	unsigned result;
	size_t object;
	size_t index;
	int unchanged;
	int same;
	int error;

	/* The words (a copy the state names), and whether they are the ones the box opened with. */
	if (!app->box.open)
		return 1;
	(void)snprintf(text, sizeof(text), "%s", notes_box_text(&app->box));

	/* A line's words are one line: the line ends a long line's box of several lines took are spaces (ws177-p012). */
	if (app->box_kind == MAIN_BOX_LINE) {
		for (index = 0; text[index] != '\0'; index++) {
			if (text[index] == '\n' || text[index] == '\r')
				text[index] = ' ';
		}
	}

	/* Whether they are the words the box opened with. */
	same = strcmp(text, notes_box_initial(&app->box));
	state = app->box_edit;
	state.text = text;

	/* Nothing changed -- no new words, or the words, font, size and colour as they were: closed as it was. */
	unchanged = 0;
	if (app->box_kind == MAIN_BOX_NEW && text[0] == '\0')
		unchanged = 1;
	if (app->box_kind == MAIN_BOX_LINE && same == 0 && app->box_font == app->box_initial_font)
		unchanged = 1;
	color = notes_ui_color(NOTES_TOOL_PEN, app->color);
	if (app->box_kind == MAIN_BOX_INSERTED && same == 0 && app->box_font == app->box_initial_font && app->box_size == app->box_edit.text_size &&
	    color == app->box_edit.color)
		unchanged = 1;
	if (unchanged) {
		app_box_close(app);
		return 1;
	}

	/* Emptied: the line deleted, the inserted text taken off. */
	if (text[0] == '\0') {
		state.text = NULL;
		if (app->box_kind == MAIN_BOX_INSERTED) {
			error = notes_document_reset_object(&app->document, app->page, &state);
		} else {
			state.flags = NOTES_EDIT_DELETED;
			error = notes_document_edit_object(&app->document, app->page, &state);
		}

		/* Closed either way; a failure is shown. */
		object = app->box_object;
		app_box_close(app);
		if (error != 0) {
			app_status(app, "Could not delete the words");
			return 1;
		}

		/* Gone (the tests' line). */
		if (app->selected == object)
			app_deselect(app);
		printf("NOTES EDIT delete page=%lu object=%lu\n", (unsigned long)app->page, (unsigned long)object);
		fflush(stdout);
		app_changed(app);
		return 1;
	}

	/* The state: the words in the font, an inserted text's size and colour too. */
	state.flags |= NOTES_EDIT_TEXT;
	state.font = app->box_font;
	if (app->box_kind != MAIN_BOX_LINE) {
		state.text_size = app->box_size;
		state.color = color;
	}

	/* New words: inserted, their box's top left at the place, wrapped at the width. */
	if (app->box_kind == MAIN_BOX_NEW) {
		state.flags = NOTES_EDIT_INSERTED | NOTES_EDIT_TEXT;
		state.id = app->document.next_id;
		state.box_width = app->box_width;
		state.transform[0] = 1.0f;
		state.transform[3] = 1.0f;
		state.transform[4] = app->box_x;
		state.transform[5] = app->box_y;
	}

	/* The editor asked first: words it cannot write stay in the box. */
	error = app_box_try(app, &state, &result);
	if (error != 0) {
		notes_box_focus(&app->box);
		app_box_frame(app);
		return 0;
	}

	/* The change (a new object takes the document's next number). */
	if (app->box_kind == MAIN_BOX_NEW)
		app->document.next_id++;
	error = notes_document_edit_object(&app->document, app->page, &state);
	if (error != 0) {
		printf("NOTES EDIT failed page=%lu object=%ld error=%d\n", (unsigned long)app->page, (long)app->box_object, error);
		fflush(stdout);
		app_status(app, "Could not keep the words");
		app_box_close(app);
		return 1;
	}

	/* The object's index now (new words are the page's last). */
	object = app->box_object;
	if (app->box_kind == MAIN_BOX_NEW) {
		error = notes_page_editor(&app->document, app->page, &editor);
		if (error == 0)
			object = pdf_page_editor_count(editor) - 1U;
	}

	/* The box closed. */
	app_box_close(app);

	/* The tests' line: a line's whether a replacement font writes it, an inserted text's size and colour (always the font asked for). */
	printf("NOTES EDIT text page=%lu object=%lu kind=%s chars=%lu font=%s", (unsigned long)app->page, (unsigned long)object,
	       kinds[app->box_kind], (unsigned long)app_characters(text), app_font_name(state.font));
	if (app->box_kind == MAIN_BOX_LINE)
		printf(" fallback=%d", (result & PDF_EDIT_TEXT_REPLACED) != 0U);
	else
		printf(" size=%g color=%08x", (double)state.text_size, (unsigned)state.color);
	printf("\n");
	fflush(stdout);

	/* A line its own font lacks characters for is written in a replacement: the status says so. */
	if (app->box_kind == MAIN_BOX_LINE && state.font == PDF_EDIT_FONT_ORIGINAL && (result & PDF_EDIT_TEXT_REPLACED) != 0U)
		app_status(app, "The line's font lacks some of these characters; it now uses Sans");

	/* With the Select tool, the words stay chosen. */
	if (app->tool == NOTES_ACTION_SELECT && object != MAIN_NONE)
		app_select(app, object, 0);
	app_changed(app);
	return 1;
}

/*
 * Asks the page's editor whether it writes a state's words (notes_page_try_edit):
 * a font missing, characters no font has, or another refusal is shown and
 * logged.  Returns 0 when it does, or an errno value.
 */
static int
app_box_try(
	struct notes_app *app,
	const struct notes_edit *state,
	unsigned *result)
{
	int error;

	/* The editor's try. */
	error = notes_page_try_edit(&app->document, app->page, state, result);
	app->text_previews++;
	if (error == 0 && (*result & PDF_EDIT_TEXT_MISSING) != 0U)
		error = EILSEQ;

	/* Taken. */
	if (error == 0)
		return 0;

	/* Refused: why (the tests' line and the status). */
	printf("NOTES EDIT text refused page=%lu object=%ld error=%d result=%u\n", (unsigned long)app->page, (long)app->box_object, error, *result);
	fflush(stdout);
	if (error == ENOTSUP)
		app_status(app, "No font for these words is installed");
	else if (error == EILSEQ)
		app_status(app, "No font has some of these characters; change them to keep the words");
	else
		app_status(app, "These words cannot be written on this page");
	return error;
}

/*
 * Closes the box without keeping anything more: the window's input is
 * Notes' again, and a preview of a line's words goes (the page's editor
 * is made again from the edits).
 */
static void
app_box_close(
	struct notes_app *app)
{
	/* The box and the window's input. */
	if (!app->box.open)
		return;
	notes_box_close(&app->box);
	notes_window_box(&app->window, 0);
	app->box_changed = 0;

	/* The preview goes. */
	if (app->box_previewing) {
		notes_page_close_editor(app->document.pages[app->page]);
		app->box_previewing = 0;
		app->text_previews++;
	}

	/* Shown without the box (the tests' line). */
	printf("NOTES TEXT box close\n");
	fflush(stdout);
	app->toolbar_dirty = 1;
	app->redraw = 1;
}

/*
 * Puts the words typed in the box on their line in the page's editor, for
 * the preview only (closing the box makes the editor again): a line of the
 * page's own, its words not empty, in the box's font; a refusal leaves the
 * line as it was.
 */
static void
app_box_preview(
	struct notes_app *app,
	struct pdf_page_editor *editor)
{
	struct pdf_edit_text words;
	const char *text;
	unsigned result;

	/* A line's box with words. */
	if (!app->box.open || app->box_kind != MAIN_BOX_LINE)
		return;
	text = notes_box_text(&app->box);
	if (text[0] == '\0')
		return;

	/* The words in the font. */
	memset(&words, 0, sizeof(words));
	words.size = sizeof(words);
	words.utf8 = text;
	words.font = (enum pdf_edit_font)app->box_font;
	(void)pdf_page_editor_set_text(editor, app->box_object, &words, &result);
}

/* Tells whether an action leaves the text box open: its font, its size, the colours, the window's own. */
static int
app_box_keeps(
	uint32_t action)
{
	/* The colours. */
	if (action >= NOTES_ACTION_COLOR && action < NOTES_ACTION_COLOR + NOTES_COLORS)
		return 1;

	/* The box's and the window's. */
	switch (action) {
	case NOTES_ACTION_FONT:
	case NOTES_ACTION_SIZE_DOWN:
	case NOTES_ACTION_SIZE_UP:
	case NOTES_ACTION_FULLSCREEN:
	case NOTES_ACTION_LEAVE_FULLSCREEN:
	case NOTES_ACTION_FINGER:
		return 1;
	default:
		break;
	}

	/* Any other closes it. */
	return 0;
}

/*
 * Chooses the next font (ws175-p008): the open box's (a line's may go back
 * to its own), the chosen text's as one change once the page's editor
 * takes its words in it, or the Text tool's for the next words.
 */
static void
app_text_font(
	struct notes_app *app)
{
	struct pdf_page_editor *editor;
	struct pdf_edit_object object;
	struct notes_edit state;
	unsigned result;
	char *words;
	int error;

	/* The open box's: shown on a line at once. */
	if (app->box.open) {
		app->box_font = app_next_font(app->box_font, app->box_kind == MAIN_BOX_LINE);
		if (app->box_kind == MAIN_BOX_LINE)
			app->box_changed = 1;
		app->toolbar_dirty = 1;
		return;
	}

	/* The Text tool's, for the next words. */
	if (app->tool != NOTES_ACTION_SELECT || app->selected == MAIN_NONE || app->selected_kind != PDF_EDIT_TEXT) {
		app->text_font = app_next_font(app->text_font, 0);
		app->toolbar_dirty = 1;
		return;
	}

	/* The chosen text's words that can change, as they are now. */
	if (app->selected_fixed) {
		app_status(app, "The words of this line cannot be changed (they can be moved or deleted)");
		return;
	}

	/* Its object and its state, and a copy of its words. */
	memset(&object, 0, sizeof(object));
	object.size = sizeof(object);
	error = notes_page_editor(&app->document, app->page, &editor);
	if (error == 0)
		error = pdf_page_editor_object(editor, app->selected, &object);
	if (error == 0)
		error = app_selected_state(app, &state);
	if (error != 0 || object.text == NULL)
		return;
	words = strdup(object.text);
	if (words == NULL)
		return;

	/* In the next font (an inserted text's words are its own already). */
	if ((state.flags & NOTES_EDIT_INSERTED) == 0U)
		state.text = words;
	state.flags |= NOTES_EDIT_TEXT;
	state.font = app_next_font(app->selected_font, (state.flags & NOTES_EDIT_INSERTED) == 0U);
	app->box_object = app->selected;
	error = app_box_try(app, &state, &result);
	if (error == 0)
		error = notes_document_edit_object(&app->document, app->page, &state);
	free(words);
	if (error != 0)
		return;

	/* The tests' line; the text stays chosen. */
	printf("NOTES EDIT font page=%lu object=%lu font=%s", (unsigned long)app->page, (unsigned long)app->selected, app_font_name(state.font));
	if ((state.flags & NOTES_EDIT_INSERTED) == 0U)
		printf(" fallback=%d", (result & PDF_EDIT_TEXT_REPLACED) != 0U);
	printf("\n");
	fflush(stdout);
	app_select(app, app->selected, 0);
	app_changed(app);
}

/*
 * Makes the text's size smaller or larger by a step (ws175-p008), within
 * MAIN_SIZE_MIN and MAIN_SIZE_MAX: the open box's (new words', an inserted
 * text's), the chosen text's as one change (an inserted text's size, a
 * line sized about its top left), or the Text tool's for the next words.
 */
static void
app_text_size(
	struct notes_app *app,
	float step)
{
	struct pdf_page_editor *editor;
	struct pdf_edit_object object;
	struct notes_edit state;
	double map[6];
	double scale;
	float base;
	float size;
	unsigned result;
	int error;

	/* The open box's (a line keeps its own size: its handles size it). */
	if (app->box.open) {
		if (app->box_kind != MAIN_BOX_LINE)
			app->box_size = fminf(fmaxf(app->box_size + step, MAIN_SIZE_MIN), MAIN_SIZE_MAX);
		app->toolbar_dirty = 1;
		return;
	}

	/* The Text tool's, for the next words. */
	if (app->tool != NOTES_ACTION_SELECT || app->selected == MAIN_NONE || app->selected_kind != PDF_EDIT_TEXT) {
		app->text_size = fminf(fmaxf(app->text_size + step, MAIN_SIZE_MIN), MAIN_SIZE_MAX);
		app->toolbar_dirty = 1;
		return;
	}

	/* The chosen text, its size now and after the step. */
	base = app->selected_size;
	if (base <= 0.0f)
		base = MAIN_SIZE_DEFAULT;
	size = fminf(fmaxf(base + step, MAIN_SIZE_MIN), MAIN_SIZE_MAX);
	error = app_selected_state(app, &state);
	if (error != 0)
		return;

	/* An inserted text: its size, once the page's editor takes its words at it. */
	if ((state.flags & NOTES_EDIT_INSERTED) != 0U) {
		state.text_size = size;
		app->box_object = app->selected;
		error = app_box_try(app, &state, &result);
		if (error == 0)
			error = notes_document_edit_object(&app->document, app->page, &state);
		if (error != 0)
			return;
		printf("NOTES EDIT size page=%lu object=%lu size=%g\n", (unsigned long)app->page, (unsigned long)app->selected, (double)size);
		fflush(stdout);
		app_select(app, app->selected, 0);
		app_changed(app);
		return;
	}

	/* A line of the page's own: sized about its top left. */
	memset(&object, 0, sizeof(object));
	object.size = sizeof(object);
	error = notes_page_editor(&app->document, app->page, &editor);
	if (error == 0)
		error = pdf_page_editor_object(editor, app->selected, &object);
	if (error != 0)
		return;
	scale = (double)size / (double)base;
	map[0] = scale;
	map[1] = 0.0;
	map[2] = 0.0;
	map[3] = scale;
	map[4] = object.quad[0] - scale * object.quad[0];
	map[5] = object.quad[1] - scale * object.quad[1];
	app_apply_map(app, map);
	printf("NOTES EDIT resize page=%lu object=%lu sx=%.3f sy=%.3f\n", (unsigned long)app->page, (unsigned long)app->selected, map[0], map[3]);
	fflush(stdout);
}

/* Gives the font after one (ws175-p008): Original (a line's own, when it may), Sans, Mono, Japanese, and round again. */
static unsigned
app_next_font(
	unsigned font,
	int original)
{
	/* Each to the next. */
	switch (font) {
	case PDF_EDIT_FONT_ORIGINAL:
		return PDF_EDIT_FONT_SANS;
	case PDF_EDIT_FONT_SANS:
		return PDF_EDIT_FONT_MONO;
	case PDF_EDIT_FONT_MONO:
		return PDF_EDIT_FONT_CJK;
	default:
		break;
	}

	/* After the last, the first. */
	if (original)
		return PDF_EDIT_FONT_ORIGINAL;
	return PDF_EDIT_FONT_SANS;
}

/* Names a font for the tests' lines. */
static const char *
app_font_name(
	unsigned font)
{
	/* Each by its word. */
	switch (font) {
	case PDF_EDIT_FONT_SANS:
		return "sans";
	case PDF_EDIT_FONT_MONO:
		return "mono";
	case PDF_EDIT_FONT_CJK:
		return "cjk";
	default:
		break;
	}

	/* The line's own. */
	return "original";
}

/* Names a font for the toolbar. */
static const char *
app_font_label(
	unsigned font)
{
	/* Each by its name. */
	switch (font) {
	case PDF_EDIT_FONT_SANS:
		return "Sans";
	case PDF_EDIT_FONT_MONO:
		return "Mono";
	case PDF_EDIT_FONT_CJK:
		return "Japanese";
	default:
		break;
	}

	/* The line's own. */
	return "Original";
}

/* Gives the index of the pen's colour a text is written in (the first when it is none of them). */
static unsigned
app_color_index(
	uint32_t color)
{
	uint32_t pen;
	unsigned index;

	/* The pen's colour that is the same. */
	for (index = 0; index < NOTES_COLORS; index++) {
		pen = notes_ui_color(NOTES_TOOL_PEN, index);
		if (pen == color)
			return index;
	}

	/* None of them. */
	return 0;
}

/* Counts the characters of UTF-8 words (the bytes that start one). */
static size_t
app_characters(
	const char *text)
{
	size_t count;
	size_t at;

	/* Each byte that is not a continuation. */
	count = 0;
	for (at = 0; text[at] != '\0'; at++) {
		if (((unsigned char)text[at] & 0xc0U) != 0x80U)
			count++;
	}

	/* Reports them. */
	return count;
}

/* Gives a signature of the toolbar's buttons as laid out (their actions and places; FNV-1a), which tells when they changed. */
static uint64_t
app_buttons_signature(
	const struct notes_ui *ui)
{
	const struct notes_button *button;
	uint64_t hash;
	uint32_t values[5];
	unsigned index;
	unsigned value;

	/* Each button's action and rectangle, in order. */
	hash = 14695981039346656037ULL;
	for (index = 0; index < ui->button_count; index++) {
		button = &ui->buttons[index];
		values[0] = button->action;
		values[1] = (uint32_t)button->x;
		values[2] = (uint32_t)button->y;
		values[3] = (uint32_t)button->width;
		values[4] = (uint32_t)button->height;
		for (value = 0; value < 5U; value++) {
			hash ^= values[value];
			hash *= 1099511628211ULL;
		}
	}

	/* Reports it. */
	return hash;
}
