/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The run of terminal, whichever program it is: a window whose tabs each
 * show the other end of a connection (connection.h), drawn by the display
 * the program was linked with (display.h).  It shows what the other end
 * writes and sends it the keys typed.  The window follows the size the
 * window system gives it, and each connection is told the grid's new
 * size.  The run ends when the last tab's connection ends or the window is
 * closed.
 *
 * Each tab of the window (tabs.c, ws035-p086: the titlebar's tabs) is a
 * connection of its own with its own grid; the keys go to the active
 * tab's, and every connection is read so none of them waits.  A tab whose
 * connection ends closes; the last one ends the run.
 *
 * Its menus (menu.c) are drawn by the window system; what they choose is
 * carried out here: a new window (the program's), closing, the selection
 * and the clipboard (the window system's, shared with other clients,
 * clipboard.c), the font's size, fullscreen, keys for the other end,
 * clearing and resetting the screen, and a line about the terminal.
 *
 * Every outcome is one line on standard output (ZTERM ...), which the
 * tests read; the program says how the run ended before it is torn down.
 */

#include "run.h"
#include "connection.h"
#include "display.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How many redraws in a row may find the drawing out of date before the terminal gives up. */
#define RUN_STALE_LIMIT	8U

/* The most text the clipboard holds: every cell as four UTF-8 bytes, and a line break per row. */
#define RUN_CLIPBOARD_MAX	(TERMINAL_MAX_COLUMNS * TERMINAL_MAX_ROWS * 4U + TERMINAL_MAX_ROWS)

/* What a bracketed paste is sent between (CSI ? 2004 h). */
#define RUN_PASTE_START		"\033[200~"
#define RUN_PASTE_END		"\033[201~"

/* The most bytes a paste sends: the clipboard's text and the brackets around it. */
#define RUN_PASTE_MAX		(RUN_CLIPBOARD_MAX + 16U)

/*
 * How the run is going: the active tab's connection, and what the error
 * line names when something fails.
 */
struct run_state {
	/* The active tab's connection (NULL when none runs). */
	struct terminal_connection *connection;

	/*
	 * The grid's size, as last told to the connections, the font's size in
	 * pixels at 96 dots an inch (the size the menus show and the settings
	 * keep) and the screen's scale in 96ths (the font is drawn at pixels
	 * times scale over 96).
	 */
	unsigned columns;
	unsigned rows;
	unsigned pixels;
	unsigned scale;

	/*
	 * Whether Ambiguous-width characters are wide (View > Treat
	 * Ambiguous-Width Characters as Wide, ws128-p009): every tab's grid
	 * follows it, and the settings file keeps it for the next run.
	 */
	int ambiguous_wide;

	/*
	 * The font's size the settings file keeps (0: none yet; View > Zoom and
	 * Text Size keep theirs) and the colour theme (View > Theme), ws128-p006.
	 */
	unsigned kept_pixels;
	unsigned theme;

	/* The size the display draws at, in pixels. */
	uint32_t width;
	uint32_t height;

	/* What failed last, the display's result of it, and why the run ended normally. */
	const char *operation;
	int result;
	const char *reason;
};

/*
 * The terminal's parts, for the whole run.  They are file-scope because the
 * grid alone is too large for the stack.  The display is made with the
 * window and released at the run's end (NULL until it is made).
 */
static struct terminal_font run_font;
static struct terminal_window run_window;
static struct terminal_display *run_display;

/*
 * The touch screen (touch.c, ws081-p011): the fingers' gestures and
 * scroller, made with the window (without them fingers do nothing).
 */
static struct terminal_touch run_touch;

/* In how many milliseconds the fingers want the next round (-1: none). */
static int run_touch_due = -1;

/*
 * One tab: its grid (allocated, too large for the stack), its connection
 * (the other end: a shell on a terminal of its own, or a remote one), its
 * ID in the titlebar and its title.
 */
struct run_tab {
	struct terminal_screen *screen;
	struct terminal_connection *connection;
	uint32_t id;
	char title[TERMINAL_TAB_TITLE];
};

/*
 * The tabs, in their order, how many there are, the active one (whose grid
 * is drawn and whose connection takes the keys) and the ID the next tab
 * gets.  run_screen is the active tab's grid; run->connection is the
 * active tab's connection.  They live as long as the terminal.
 */
static struct run_tab run_tabs[TERMINAL_TABS];
static unsigned run_tab_count;
static unsigned run_active;
static uint32_t run_tab_next = 1U;
static struct terminal_screen *run_screen;

/*
 * The clipboard's text and its length: the terminal's copy (Edit > Copy),
 * which its data source sends to other clients while it is the selection,
 * or another client's selection received for a paste; the text being
 * pasted into the shell (Edit > Paste) with how much of it was written,
 * between the brackets when the program asked for them; and the primary
 * selection or a drop received before it is pasted.  They live as long as
 * the terminal.
 */
static char run_clipboard[RUN_CLIPBOARD_MAX];
static size_t run_clipboard_length;
static char run_paste[RUN_PASTE_MAX];
static char run_received[RUN_CLIPBOARD_MAX];

/*
 * The pointer's selection (ws035-p093): whether the left button is held
 * for a selection, or was pressed inside the range (a move then drags the
 * text out), where and with which serial, the press before (for double and
 * triple clicks: its time, its cell and how many clicks it made), and the
 * cell a drag of the range started at.  A cell is a column and a line
 * (screen.c numbers lines so they follow the text into the scrollback,
 * ws035-p114).
 */
static int run_selecting;
static int run_drag_armed;
static int32_t run_press_x;
static int32_t run_press_y;
static uint32_t run_press_serial;
static uint32_t run_click_time;
static unsigned run_click_column;
static unsigned long run_click_line;
static unsigned run_clicks;
static unsigned run_anchor_column;
static unsigned long run_anchor_line;

/*
 * Whether the last click was a touch pad's tap: the compositor gives a
 * tap's press and release at once, with the same time (a button pressed
 * by a hand is held for some milliseconds).  A press soon after a tap may
 * be a double tap or a tap-and-drag, which the compositor gives as the
 * tap's click and then a held press (BUG-276); run_tap_drag says the press
 * held now came that way, so that its first move off the cell makes it the
 * drag of characters the user meant, not a double click's drag by words.
 */
static int run_last_tap;
static int run_tap_drag;

/*
 * A tap on the selected range (BUG-276, the user: "with a range selected,
 * the first tap of a double tap takes the selection away"): the click of a
 * touch pad's tap does not clear the range at once but at run_tap_clear_at
 * (0: nothing waits), so that the tap-and-drag that may follow drags the
 * range out as a pressed pad does.  run_tap_second says the press held
 * now came within that time on the range: moved, it drags the range out;
 * let go, it was a double tap and selects the word.
 */
static uint64_t run_tap_clear_at;
static int run_tap_second;

/*
 * The unit a held selection grows by (ws035-p111): 1 a cell (a click), 2 a
 * word (a double click), 3 a line (a triple click); the word or line the
 * double or triple click chose, which a drag keeps selected while it adds
 * whole words or lines on the side the pointer goes; and whether the drag
 * changed the range since the press.
 */
static unsigned run_unit;
static unsigned long run_unit_from[2];
static unsigned long run_unit_to[2];
static int run_unit_moved;

/*
 * The edge scrolling of a held selection (ws035-p114): where the pointer
 * last was while the button selects, which way the view scrolls because
 * the pointer is past the grid's top (1, back into the scrollback) or
 * bottom (-1, toward the live screen) or 0 while it is on the grid, and
 * when the next step is due, in milliseconds.
 */
static int32_t run_pointer_x;
static int32_t run_pointer_y;
static int run_edge;
static uint64_t run_edge_at;

/* How close in time a click makes a double or triple click, and how far a press moves before it drags, in milliseconds and pixels. */
#define RUN_CLICK_MS		400U
#define RUN_DRAG_DISTANCE	6

/* How long a tap's click on the range waits for a tap-and-drag before it clears the range: the compositor's tap-drag time (touchpad.c TAP_DRAG_MS) and a margin. */
#define RUN_TAP_DRAG_MS	350U

/* How often a selection held past the grid's edge scrolls, in milliseconds, and the most lines one step scrolls (ws035-p114). */
#define RUN_EDGE_MS		60U
#define RUN_EDGE_LINES		8U

/* How many lines one notch of the wheel scrolls the view (ws035-p114). */
#define RUN_WHEEL_LINES	3

/* The window's title as last set: the active tab's (OSC 0 or 2), or the program's (ws035-p091). */
static char run_window_title[TERMINAL_TITLE];
static size_t run_paste_length;

/* The text selected with the pointer, which the primary selection sends (primary.c keeps a pointer to it). */
static char run_primary[RUN_CLIPBOARD_MAX];
static size_t run_paste_written;

/*
 * Whether the active connection reads a secret (ws095-p006): its echo off
 * on a whole line, as a password prompt has it.  The window's text input is off
 * meanwhile, so the input method composes nothing and the keys go as they are.
 */
static int run_secret;

static int run_start(const struct terminal_run_options *options, struct run_state *run);
static int run_loop(const struct terminal_run_options *options, struct run_state *run);
static int run_resize(const struct terminal_run_options *options, struct run_state *run);
static void run_grid(unsigned width, unsigned height, unsigned *columns, unsigned *rows);
static void run_tell_size(const struct run_state *run, struct terminal_connection *connection);
static int run_read_shell(struct terminal_connection *connection, struct terminal_screen *screen, unsigned ready);
static int run_write_shell(struct terminal_connection *connection);
static int run_menu_actions(const struct terminal_run_options *options, struct run_state *run);
static void run_menu_state(const struct run_state *run, struct terminal_menu_state *state);
static void run_new_window(const struct terminal_run_options *options, const struct run_state *run);
static int run_zoom(const struct terminal_run_options *options, struct run_state *run, unsigned pixels);
static void run_ambiguous_wide(const struct terminal_run_options *options, struct run_state *run);
static int run_save_settings(const struct run_state *run);
static void run_theme(const struct terminal_run_options *options, struct run_state *run, unsigned theme);
static void run_find(int step);
static void run_search(const struct terminal_run_options *options);
static void run_copy(void);
static void run_start_paste(void);
static void run_drop_paste(void);
static void run_paste_begin(const char *text, size_t length, int lines);
static int run_tab_new(const struct terminal_run_options *options, struct run_state *run);
static void run_tab_switch(struct run_state *run, unsigned index);
static int run_tab_close(const struct terminal_run_options *options, struct run_state *run, unsigned index);
static int run_tab_find(uint32_t id);
static int run_tab_requests(const struct terminal_run_options *options, struct run_state *run);
static void run_tabs_show(const struct terminal_run_options *options);
static void run_title_copy(char *to, size_t size, const char *from);
static void run_pointer(void);
static void run_pointer_press(const struct terminal_pointer_event *event);
static void run_primary_paste(void);
static void run_pointer_motion(const struct terminal_pointer_event *event);
static void run_pointer_release(const struct terminal_pointer_event *event);
static void run_tap_expire(uint64_t now);
static void run_range_clear(void);
static void run_select_word(unsigned column, unsigned long line);
static void run_cell(int32_t x, int32_t y, unsigned *column, unsigned long *line);
static void run_range(unsigned from_column, unsigned long from_line, unsigned to_column, unsigned long to_line);
static int run_word_character(unsigned column, unsigned long line);
static void run_unit_bounds(unsigned unit, unsigned column, unsigned long line, unsigned *from, unsigned *to);
static void run_unit_extend(unsigned column, unsigned long line);
static void run_selected_log(const char *how);
static void run_select_to_pointer(void);
static void run_edge_scroll(uint64_t now);
static void run_scroll(void);
static void run_view_log(const char *how);
static void run_touch_round(void);
static void run_text_cursor(void);
static void run_secret_follow(struct terminal_connection *connection);
static unsigned run_font_pixels(const struct run_state *run, unsigned pixels);
static void run_touch_queue(unsigned kind, const struct terminal_touch_pointer *made);

/*
 * Runs the terminal: opens the font, the window, the display and the first
 * tab, runs until the last tab or the window closes, tells the program how
 * the run ended (options->ended, before anything is torn down: a tab's
 * close is logged after it) and releases everything.
 *
 * Returns 0 for a normal end, or -1 when something failed; result says
 * which and why either way.
 */
int
terminal_run(
	const struct terminal_run_options *options,
	struct terminal_run_result *result)
{
	struct run_state run;
	int status;
	int error;

	/* Nothing has failed yet; the screen's scale is the options' (96ths, 96 by default). */
	memset(&run, 0, sizeof(run));
	run.connection = NULL;
	run.operation = "none";
	run.result = 0;
	run.reason = "closed";
	run.scale = options->scale;
	if (run.scale == 0U)
		run.scale = 96U;

	/* The font, the window, the drawing and the first tab; then the terminal runs until it ends. */
	status = run_start(options, &run);
	if (status == 0)
		status = run_loop(options, &run);

	/* How the run ended, for the program to say before the teardown. */
	error = errno;
	memset(result, 0, sizeof(*result));
	result->failed = 0;
	if (status != 0)
		result->failed = 1;
	result->operation = run.operation;
	result->result = run.result;
	result->error = error;
	result->reason = run.reason;
	if (options->ended != NULL)
		options->ended(options->data, result);

	/* Each tab's connection, if it still runs, is hung up on. */
	while (run_tab_count > 0U)
		(void)run_tab_close(options, &run, run_tab_count - 1U);

	/* The drawing before the window it draws into, then the font. */
	terminal_display_close(run_display);
	run_display = NULL;
	terminal_touch_close(&run_touch);
	terminal_window_close(&run_window);
	terminal_font_close(&run_font);

	/* Reports a failed run. */
	if (status != 0)
		return -1;

	/* Succeeded: the run ended normally. */
	return 0;
}

/* Opens the font, the window and the drawing, sizes the grid and opens the first tab; returns nonzero on failure. */
static int
run_start(
	const struct terminal_run_options *options,
	struct run_state *run)
{
	struct terminal_menu_state state;
	struct terminal_settings settings;
	unsigned pixels;
	int status;

	/* The settings kept from the last run (the defaults when there are none). */
	terminal_settings_load(&settings);
	run->ambiguous_wide = settings.ambiguous_wide;
	run->kept_pixels = settings.font_size;
	run->theme = settings.theme;
	printf("ZTERM SETTINGS run=%s ambiguous_wide=%d font_size=%u theme=%u\n", options->token, run->ambiguous_wide, settings.font_size, settings.theme);

	/* The font's size: the one given on the command line, else the one kept, else the default (ws128-p006). */
	pixels = options->pixels;
	if (!options->pixels_given && settings.font_size != 0U)
		pixels = settings.font_size;

	/* The font, at the screen's scale, which sets the cell's size. */
	run->operation = "terminal_font_open";
	status = terminal_font_open(&run_font, options->font, options->companion, run_font_pixels(run, pixels));
	if (status != 0) {
		errno = status;
		return -1;
	}

	/* The fallback font for the characters the font has not got; without it they show the font's missing glyph. */
	status = ENOENT;
	if (options->fallback != NULL)
		status = terminal_font_fallback(&run_font, options->fallback);
	if (status != 0)
		printf("ZTERM FONT fallback=none error=%d\n", status);

	/* The window, sized for the grid asked for. */
	run->operation = "terminal_window_open";
	status = terminal_window_open(&run_window,
				      options->display,
				      options->application,
				      options->title,
				      options->present,
				      options->columns * run_font.cell_width + 2U * TERMINAL_PADDING,
				      options->rows * run_font.cell_height + 2U * TERMINAL_PADDING);
	if (status != 0)
		return -1;

	/* The fingers; without memory for them they do nothing. */
	status = terminal_touch_open(&run_touch);
	if (status != 0)
		printf("ZTERM TOUCH none error=%d\n", status);

	/* The drawing, at the size the compositor gave. */
	status = terminal_display_open(&run_display, &run_window, &run_font);
	run->operation = terminal_display_operation(run_display, &run->result);
	if (status != 0)
		return -1;

	/* The grid that fits the window, and the theme it is drawn in. */
	run->pixels = pixels;
	run_window.theme = run->theme;
	terminal_display_size(run_display, &run->width, &run->height);
	run_grid(run->width, run->height, &run->columns, &run->rows);

	/* The menus, which the compositor draws (none from a compositor without the System Menu). */
	run_menu_state(run, &state);
	run->operation = "terminal_menu_open";
	status = terminal_menu_open(&run_window, &state);
	if (status != 0)
		return -1;

	/* The first tab: its connection at that size. */
	run->operation = options->connector->operation;
	status = run_tab_new(options, run);
	if (status != 0)
		return -1;

	/* Succeeded: the terminal is up. */
	printf("ZTERM START run=%s columns=%u rows=%u cell=%ux%u window=%ux%u\n",
	       options->token, run->columns, run->rows, run_font.cell_width, run_font.cell_height,
	       run->width, run->height);
	fflush(stdout);
	return 0;
}

/* Runs until the last tab's connection ends or the window is closed; returns nonzero on a failure. */
static int
run_loop(
	const struct terminal_run_options *options,
	struct run_state *run)
{
	struct terminal_menu_state state;
	uint64_t started;
	uint64_t now;
	unsigned stale;
	unsigned index;
	struct terminal_connection *connections[TERMINAL_TABS];
	unsigned ready[TERMINAL_TABS];
	unsigned pending[TERMINAL_TABS];
	int status;
	int timeout;
	int wait;
	int asked;
	int error;

	/* One round per event: wait, read the shell, send the keys, redraw what changed. */
	started = terminal_clock();
	stale = 0U;
	for (;;) {
		/* Waits for the compositor or the shell (or less, until a held key repeats: the application's). */
		timeout = 1000;
		now = terminal_clock();

		/* The fingers' next round, when it comes first (ws081-p011). */
		if (run_touch_due >= 0 && run_touch_due < timeout)
			timeout = run_touch_due;

		/* A tap's click on the range clears it when no tap-and-drag came in time (BUG-276). */
		if (run_tap_clear_at != 0U) {
			if (run_tap_clear_at <= now)
				timeout = 0;
			else if ((uint64_t)timeout > run_tap_clear_at - now)
				timeout = (int)(run_tap_clear_at - now);
		}

		/* A selection held past the grid's edge scrolls on its own time, even while the pointer rests (ws035-p114). */
		if (run_selecting && run_edge != 0) {
			if (run_edge_at <= now)
				timeout = 0;
			else if ((uint64_t)timeout > run_edge_at - now)
				timeout = (int)(run_edge_at - now);
		}

		/*
		 * A change waiting for the compositor to take the last frame is
		 * drawn when it answers (which wakes the wait), or at the latest
		 * when the pace gives up waiting (pace.h).
		 */
		if (run_screen->changed) {
			wait = terminal_window_frame_wait(&run_window, now);
			if (wait < timeout)
				timeout = wait;
		}

		/* A connection that still has something now (whatever events come) is not waited for. */
		for (index = 0; index < run_tab_count; index++) {
			connections[index] = run_tabs[index].connection;
			pending[index] = connections[index]->ops->pending(connections[index]);
			if (pending[index] != 0U)
				timeout = 0;
		}

		/* The keys are read in the modes the active tab's program asked for (DECCKM, DECKPAM). */
		run_window.key_modes = 0U;
		if (run_screen->cursor_keys_application)
			run_window.key_modes |= TERMINAL_KEYS_CURSOR;
		if (run_screen->keypad_application)
			run_window.key_modes |= TERMINAL_KEYS_KEYPAD;

		/* Runs the window system's events and learns what the connections told. */
		run->operation = "terminal_window_dispatch";
		status = terminal_window_dispatch(&run_window, connections, run_tab_count, timeout, ready);
		if (status != 0)
			return -1;

		/* What a connection still had counts as told. */
		for (index = 0; index < run_tab_count; index++)
			ready[index] |= pending[index];

		/* The compositor closing the window ends the terminal. */
		if (run_window.closed) {
			run->reason = "closed";
			return 0;
		}

		/* The menus' choices are carried out; Close Window (or closing the last tab) ends the terminal. */
		status = run_menu_actions(options, run);
		if (status < 0)
			return -1;
		if (status > 0) {
			run->reason = "menu-close";
			return 0;
		}

		/* What the titlebar asked of the tabs; closing the last one ends the terminal. */
		status = run_tab_requests(options, run);
		if (status < 0)
			return -1;
		if (status > 0) {
			run->reason = "tabs-closed";
			return 0;
		}

		/* What each connection wrote goes on its grid, from the last tab so a closed one leaves the others' places; a connection's end closes its tab. */
		for (index = run_tab_count; index > 0U; index--) {
			/* A connection that told nothing to read and has not ended. */
			if (index - 1U >= TERMINAL_TABS || (ready[index - 1U] & (TERMINAL_READY_OUTPUT | TERMINAL_READY_EXIT)) == 0U)
				continue;

			/* Its bytes on its grid; its end closes its tab, and the last tab's ends the terminal. */
			status = run_read_shell(run_tabs[index - 1U].connection, run_tabs[index - 1U].screen, ready[index - 1U]);
			if (status != 0) {
				status = run_tab_close(options, run, index - 1U);
				if (status != 0) {
					run->reason = "shell-exited";
					return 0;
				}
			}
		}

		/* A drop on the window is pasted into the active shell (clipboard.c). */
		if (run_window.drop_pending)
			run_drop_paste();

		/* The fingers scroll the view, and their taps and long presses become the pointer's presses (ws081-p011). */
		run_touch_round();

		/* The pointer selects, or drags the selected text out (ws035-p093); a tap's wait on the range ends in time (BUG-276). */
		run_pointer();
		run_tap_expire(terminal_clock());

		/* The search bar's text and steps find their matches (ws128-p006). */
		run_search(options);

		/* The wheel and Shift+Page Up or Down scroll the view, and a selection past the edge scrolls it (ws035-p114). */
		run_scroll();
		run_edge_scroll(terminal_clock());

		/* A key typed ends the selection (Edit > Select All, or the pointer's range). */
		if (run_window.input_length != 0U && (run_screen->selected || run_screen->range)) {
			run_screen->selected = 0;
			run_screen->range = 0;
			run_screen->changed = 1;
		}

		/* A key typed shows the live screen again, where the shell answers it (ws035-p114), on whole lines. */
		if (run_window.input_length != 0U &&
		    (run_screen->view != 0U ||
		     run_screen->view_offset != 0)) {
			run_screen->view = 0U;
			run_screen->view_offset = 0;
			run_screen->changed = 1;
			run_view_log("key");
		}

		/* The keys typed and the text pasted go to the shell. */
		status = run_write_shell(run->connection);
		if (status != 0) {
			run->reason = "shell-exited";
			return 0;
		}

		/* A password prompt in the active connection turns the input method off until it is answered. */
		run_secret_follow(run->connection);

		/* The deadline, when one was given, ends the terminal. */
		now = terminal_clock();
		if (options->timeout != 0U && now - started >= (uint64_t)options->timeout * 1000U) {
			run->reason = "timeout";
			return 0;
		}

		/* A new size remakes the swapchain and the grid, and tells the shell. */
		if (run_window.resized) {
			status = run_resize(options, run);
			if (status != 0)
				return -1;
		}

		/* The menus show the terminal's state, and the titlebar its tabs (only a change is sent). */
		run_menu_state(run, &state);
		terminal_menu_refresh(&run_window, &state);
		run_tabs_show(options);

		/* The input method's text being composed changed: drawn again at the cursor (BUG-155). */
		if (run_window.preedit_changed) {
			run_window.preedit_changed = 0;
			run_screen->changed = 1;
		}

		/* Nothing changed: nothing to draw. */
		if (!run_screen->changed)
			continue;

		/*
		 * The compositor has not taken the last frame yet: the change
		 * waits for it, and whatever else changes meanwhile (a selection
		 * dragged on, the shell's output) is drawn in the same frame, as
		 * it is then, instead of a frame for each (pace.h).
		 */
		wait = terminal_window_frame_wait(&run_window, terminal_clock());
		if (wait != 0)
			continue;

		/* Draws the grid with the frame callback asked for it; a drawing out of date is remade and drawn again next round. */
		asked = terminal_window_frame_ask(&run_window, terminal_clock());
		status = terminal_display_draw(run_display, run_screen, &run_font, &run_window);
		run->operation = terminal_display_operation(run_display, &run->result);
		error = 0;
		if (status != 0)
			error = EIO;
		terminal_window_frame_shown(&run_window, asked, error);
		if (status == EAGAIN) {
			stale++;
			if (stale > RUN_STALE_LIMIT)
				return -1;
			run_window.resized = 1;
			continue;
		}

		/* Any other failure ends the terminal. */
		if (status != 0)
			return -1;

		/* The grid on the window is up to date. */
		stale = 0U;
		run_screen->changed = 0;

		/* The input method's candidates stay next to the cursor as drawn (only a change is sent). */
		run_text_cursor();
	}
}

/* Remakes the swapchain at the window's new size, fits the grid to it and tells the shell; returns nonzero on failure. */
static int
run_resize(
	const struct terminal_run_options *options,
	struct run_state *run)
{
	unsigned index;
	int status;

	/* The drawing at the new size. */
	run_window.resized = 0;
	status = terminal_display_resize(run_display, run_window.width, run_window.height);
	run->operation = terminal_display_operation(run_display, &run->result);
	if (status != 0)
		return -1;

	/* The grid that fits it, for every tab, told to each connection. */
	terminal_display_size(run_display, &run->width, &run->height);
	run_grid(run->width, run->height, &run->columns, &run->rows);
	for (index = 0; index < run_tab_count; index++) {
		terminal_screen_resize(run_tabs[index].screen, run->columns, run->rows);
		run_tell_size(run, run_tabs[index].connection);
	}

	/* The active grid is drawn again. */
	run_screen->changed = 1;

	/* Succeeded: the next frame is drawn at the new size. */
	printf("ZTERM RESIZE run=%s columns=%u rows=%u window=%ux%u\n", options->token, run->columns, run->rows, run->width, run->height);
	fflush(stdout);
	return 0;
}



/* Works out how many cells fit in the window, inside the padding. */
static void
run_grid(
	unsigned width,
	unsigned height,
	unsigned *columns,
	unsigned *rows)
{
	/* The window less the padding on both sides, in whole cells, at least one of each. */
	*columns = 1U;
	*rows = 1U;
	if (width > 2U * TERMINAL_PADDING + run_font.cell_width)
		*columns = (width - 2U * TERMINAL_PADDING) / run_font.cell_width;
	if (height > 2U * TERMINAL_PADDING + run_font.cell_height)
		*rows = (height - 2U * TERMINAL_PADDING) / run_font.cell_height;
	if (*columns > TERMINAL_MAX_COLUMNS)
		*columns = TERMINAL_MAX_COLUMNS;
	if (*rows > TERMINAL_MAX_ROWS)
		*rows = TERMINAL_MAX_ROWS;
}

/* Tells a connection the grid's size (a shell gets SIGWINCH), in cells and in pixels. */
static void
run_tell_size(
	const struct run_state *run,
	struct terminal_connection *connection)
{
	unsigned width;
	unsigned height;
	int error;

	/* The grid's cells and the pixels they cover. */
	width = run->columns * run_font.cell_width;
	height = run->rows * run_font.cell_height;
	error = connection->ops->resize(connection, run->columns, run->rows, width, height);
	(void)error;
}

/*
 * Reads what a shell wrote and puts it on its grid; returns nonzero once
 * the shell has ended (what it wrote before its end is read first).
 */
static int
run_read_shell(
	struct terminal_connection *connection,
	struct terminal_screen *screen,
	unsigned ready)
{
	unsigned char buffer[8192];
	size_t count;
	int rounds;

	/* Reads bursts until nothing is left, at most a few before the grid is drawn again. */
	for (rounds = 0; rounds < 16; rounds++) {
		count = connection->ops->read(connection, buffer, sizeof(buffer));
		if (count == 0U)
			break;
		terminal_perf_step("read", (unsigned)count);
		terminal_screen_write(screen, buffer, count);
	}

	/* The shell ended and all it wrote is on the grid: it is gone. */
	if ((ready & TERMINAL_READY_EXIT) != 0U && count == 0U)
		return 1;

	/* More may follow (its end told again with what is left), read on the next round. */
	return 0;
}

/*
 * Turns the window's text input off while the active shell reads a secret
 * (ws095-p006): sudo, su, ssh and passwd turn the echo off and keep the
 * whole line, so the keys then reach them as they are, with no text
 * composed.  A full-screen program that turns the echo off with the line
 * (an editor, ICANON off) keeps the input method.
 */
static void
run_secret_follow(
	struct terminal_connection *connection)
{
	unsigned mode;
	int secret;
	int error;

	/* The active connection's modes; one that cannot tell them leaves the text input as it is. */
	if (connection == NULL)
		return;
	error = connection->ops->mode(connection, &mode);
	if (error != 0)
		return;

	/* A secret: no echo, on a whole line. */
	secret = 0;
	if ((mode & TERMINAL_MODE_ECHO) == 0U && (mode & TERMINAL_MODE_LINES) != 0U)
		secret = 1;
	if (secret == run_secret)
		return;

	/* The text input off for the secret, on again after it. */
	run_secret = secret;
	if (secret)
		kl_window_text_input(run_window.kui, 0);
	else
		kl_window_text_input(run_window.kui, 1);
	printf("ZTERM IME secret=%d\n", secret);
	fflush(stdout);
}

/*
 * Writes the keys typed to the shell; returns nonzero once the shell has
 * gone.  What its input has no room for stays for a later round (the
 * shell tells when there is room).
 */
static int
run_write_shell(
	struct terminal_connection *connection)
{
	size_t count;
	int error;

	/* Text being pasted goes first, as much as the terminal takes. */
	if (run_paste_written < run_paste_length) {
		error = connection->ops->write(connection, (const unsigned char *)run_paste + run_paste_written, run_paste_length - run_paste_written, &count);
		if (error != 0)
			return 1;

		/* What was written is not written again. */
		run_paste_written += count;
	}

	/* Nothing typed. */
	if (run_window.input_length == 0U)
		return 0;

	/* The bytes typed, as many as the input takes. */
	error = connection->ops->write(connection, run_window.input, run_window.input_length, &count);
	if (error != 0)
		return 1;

	/* What was written leaves the buffer. */
	terminal_perf_step("write", (unsigned)count);
	memmove(run_window.input, run_window.input + count, run_window.input_length - count);
	run_window.input_length -= count;

	/* Succeeded: the keys went to the shell. */
	return 0;
}

/*
 * Carries out the actions the menus chose.  Returns 1 when one closes the
 * terminal, -1 when one failed, 0 otherwise.
 */
static int
run_menu_actions(
	const struct terminal_run_options *options,
	struct run_state *run)
{
	uint32_t action;
	int status;

	/* Each action in the order it was chosen. */
	for (;;) {
		action = terminal_menu_take(&run_window);
		if (action == TERMINAL_ACTION_NONE)
			break;

		/* What the action does. */
		status = 0;
		switch (action) {
		case TERMINAL_ACTION_NEW_WINDOW:
			/* Another terminal, in a window of its own. */
			run_new_window(options, run);
			break;
		case TERMINAL_ACTION_CLOSE:
			/* The terminal ends. */
			return 1;
		case TERMINAL_ACTION_NEW_TAB:
			/* Another shell, in a tab of its own. */
			status = run_tab_new(options, run);
			break;
		case TERMINAL_ACTION_CLOSE_TAB:
			/* The active tab closes; the last one's end ends the terminal. */
			status = run_tab_close(options, run, run_active);
			if (status != 0)
				return 1;
			break;
		case TERMINAL_ACTION_COPY:
			/* The selected text into the terminal's clipboard. */
			run_copy();
			break;
		case TERMINAL_ACTION_PASTE:
			/* The clipboard's text to the shell, as if typed. */
			run_start_paste();
			break;
		case TERMINAL_ACTION_SELECT_ALL:
			/* The whole screen is selected until a key is typed. */
			run_screen->selected = 1;
			run_screen->changed = 1;
			break;
		case TERMINAL_ACTION_ZOOM_IN:
			/* The text a step larger, or one of the fixed sizes below. */
			status = run_zoom(options, run, run->pixels + TERMINAL_PIXELS_STEP);
			break;
		case TERMINAL_ACTION_ZOOM_OUT:
			status = run_zoom(options, run, run->pixels - TERMINAL_PIXELS_STEP);
			break;
		case TERMINAL_ACTION_ZOOM_NORMAL:
			status = run_zoom(options, run, options->pixels);
			break;
		case TERMINAL_ACTION_SIZE_SMALL:
			status = run_zoom(options, run, TERMINAL_PIXELS_SMALL);
			break;
		case TERMINAL_ACTION_SIZE_MEDIUM:
			status = run_zoom(options, run, TERMINAL_PIXELS_MEDIUM);
			break;
		case TERMINAL_ACTION_SIZE_LARGE:
			status = run_zoom(options, run, TERMINAL_PIXELS_LARGE);
			break;
		case TERMINAL_ACTION_SIZE_HUGE:
			status = run_zoom(options, run, TERMINAL_PIXELS_HUGE);
			break;
		case TERMINAL_ACTION_FULLSCREEN:
			/* The compositor's configure says whether it happened. */
			if (run_window.fullscreen)
				terminal_window_set_fullscreen(&run_window, 0);
			else
				terminal_window_set_fullscreen(&run_window, 1);
			break;
		case TERMINAL_ACTION_INTERRUPT:
			/* ^C, as if typed. */
			terminal_window_type(&run_window, "\003", 1U);
			break;
		case TERMINAL_ACTION_END_OF_FILE:
			/* ^D, as if typed. */
			terminal_window_type(&run_window, "\004", 1U);
			break;
		case TERMINAL_ACTION_CLEAR:
			/* The cursor home and the screen erased. */
			terminal_screen_write(run_screen, (const unsigned char *)"\033[H\033[2J", 7U);
			break;
		case TERMINAL_ACTION_RESET:
			/* The screen as it started, at its size, with the width setting kept. */
			terminal_screen_init(run_screen, run->columns, run->rows);
			terminal_screen_set_ambiguous_wide(run_screen, run->ambiguous_wide);
			break;
		case TERMINAL_ACTION_AMBIGUOUS_WIDE:
			/* Ambiguous-width characters written from now on take the other width. */
			run_ambiguous_wide(options, run);
			break;
		case TERMINAL_ACTION_FIND:
			/* The search bar opens with the text looked for last (ws128-p006). */
			run_find(0);
			break;
		case TERMINAL_ACTION_FIND_NEXT:
			/* The next older match. */
			run_find(1);
			break;
		case TERMINAL_ACTION_FIND_PREVIOUS:
			/* The next newer match. */
			run_find(-1);
			break;
		case TERMINAL_ACTION_THEME_DARK:
			run_theme(options, run, TERMINAL_THEME_DARK);
			break;
		case TERMINAL_ACTION_THEME_LIGHT:
			run_theme(options, run, TERMINAL_THEME_LIGHT);
			break;
		case TERMINAL_ACTION_THEME_CONTRAST:
			run_theme(options, run, TERMINAL_THEME_CONTRAST);
			break;
		case TERMINAL_ACTION_ABOUT:
			/* A line about the terminal on the screen (there are no dialogs). */
			if (options->about != NULL)
				terminal_screen_write(run_screen, (const unsigned char *)options->about, strlen(options->about));
			break;
		default:
			/* An action this terminal does not have. */
			break;
		}

		/* A failed action ends the terminal. */
		if (status != 0)
			return -1;

		/* The log line the tests read. */
		printf("ZTERM ACTION run=%s action=%u\n", options->token, action);
		fflush(stdout);
	}

	/* Succeeded: every action was carried out. */
	return 0;
}

/* Describes the terminal's state for the menus. */
static void
run_menu_state(
	const struct run_state *run,
	struct terminal_menu_state *state)
{
	/* Disables menu actions until their initial state is supplied. */
	memset(state, 0, sizeof(*state));

	/* The first menu exists before the first tab has a screen to select from. */
	if (run_screen != NULL) {
		/* A selected cell or range enables actions on the terminal's text. */
		if (run_screen->selected || run_screen->range)
			state->selection = 1;
	}

	/* Clipboard, type size, fullscreen and the width setting do not depend on a tab. */
	state->clipboard = terminal_clipboard_has_text(&run_window);
	state->pixels = run->pixels;
	state->fullscreen = run_window.fullscreen;
	state->ambiguous_wide = run->ambiguous_wide;
	state->theme = run->theme;
	state->can_find_again = 0;
	if (run_window.search_length != 0U)
		state->can_find_again = 1;
}

/*
 * Opens a new window the program's way (another terminal, apart from this
 * one: it keeps nothing of this terminal's); a program without new
 * windows does nothing.
 */
static void
run_new_window(
	const struct terminal_run_options *options,
	const struct run_state *run)
{
	/* Nothing of the run is handed over. */
	(void)run;

	/* The program's own way, from this window. */
	if (options->new_window == NULL)
		return;
	options->new_window(options->data, run_window.app, run_window.kui);
}

/*
 * Draws the text at another size (within TERMINAL_PIXELS_MIN and _MAX):
 * the grid that fits the window is made again and told to the shell.
 * Returns 0, or -1 when the font cannot be drawn at all any more.
 */
static int
run_zoom(
	const struct terminal_run_options *options,
	struct run_state *run,
	unsigned pixels)
{
	unsigned index;
	int error;

	/* The size stays within the bounds. */
	if (pixels < TERMINAL_PIXELS_MIN)
		pixels = TERMINAL_PIXELS_MIN;
	if (pixels > TERMINAL_PIXELS_MAX)
		pixels = TERMINAL_PIXELS_MAX;

	/* An unchanged size does nothing. */
	if (pixels == run->pixels)
		return 0;

	/* The font at the new size (at the screen's scale), once no frame reads the atlas; a size it cannot give keeps the old one. */
	(void)terminal_display_idle(run_display);
	error = terminal_font_resize(&run_font, run_font_pixels(run, pixels));
	if (error == ENOMEM) {
		run->operation = "terminal_font_resize";
		errno = error;
		return -1;
	}

	/* A size the font cannot be measured at is not taken. */
	if (error != 0)
		return 0;

	/* The size the menus show and Zoom In and Out step from, kept for the next run (ws128-p006). */
	run->pixels = pixels;
	run->kept_pixels = pixels;
	error = run_save_settings(run);
	if (error != 0)
		printf("ZTERM SETTINGS save-failed error=%d\n", error);

	/* The grid that fits the window at the new size, for every tab, told to each shell. */
	run_grid(run->width, run->height, &run->columns, &run->rows);
	for (index = 0; index < run_tab_count; index++) {
		terminal_screen_resize(run_tabs[index].screen, run->columns, run->rows);
		run_tell_size(run, run_tabs[index].connection);
	}

	/* The active grid is drawn again. */
	run_screen->changed = 1;

	/* Succeeded: the next frame draws at the new size. */
	printf("ZTERM ZOOM run=%s pixels=%u columns=%u rows=%u\n", options->token, pixels, run->columns, run->rows);
	fflush(stdout);
	return 0;
}

/*
 * Turns View > Treat Ambiguous-Width Characters as Wide over: every tab's
 * grid gives the next Ambiguous characters the other width (what is on
 * the screens stays as it is), and the settings file keeps the choice for
 * the next run.
 */
static void
run_ambiguous_wide(
	const struct terminal_run_options *options,
	struct run_state *run)
{
	unsigned index;
	int error;

	/* The other setting; the menu shows it checked or not on the next refresh. */
	if (run->ambiguous_wide) {
		run->ambiguous_wide = 0;
	} else {
		run->ambiguous_wide = 1;
	}

	/* Every tab's grid follows at once. */
	for (index = 0U; index < run_tab_count; index++)
		terminal_screen_set_ambiguous_wide(run_tabs[index].screen, run->ambiguous_wide);

	/* The settings file keeps it; a failed write leaves this run changed and is reported. */
	error = run_save_settings(run);

	/* The log line the tests read. */
	printf("ZTERM AMBIGUOUS run=%s wide=%d saved=%d\n", options->token, run->ambiguous_wide, error);
	fflush(stdout);
}

/* Keeps the run's settings for the next run (ws128-p009, ws128-p006); returns 0 or an errno value. */
static int
run_save_settings(
	const struct run_state *run)
{
	struct terminal_settings settings;
	int error;

	/* The width setting, the font's size kept (0: none) and the theme. */
	memset(&settings, 0, sizeof(settings));
	settings.ambiguous_wide = run->ambiguous_wide;
	settings.font_size = run->kept_pixels;
	settings.theme = run->theme;

	/* The settings file keeps them. */
	error = terminal_settings_save(&settings);
	if (error != 0)
		return error;

	/* Succeeded: the next run reads them. */
	return 0;
}

/*
 * Draws the window in a colour theme (View > Theme, ws128-p006), kept for
 * the next run.
 */
static void
run_theme(
	const struct terminal_run_options *options,
	struct run_state *run,
	unsigned theme)
{
	int error;

	/* The theme the window is drawn in from the next frame. */
	run->theme = theme;
	run_window.theme = theme;
	run_screen->changed = 1;

	/* The settings file keeps it; a failed write leaves this run changed and is reported. */
	error = run_save_settings(run);

	/* The log line the tests read. */
	printf("ZTERM THEME run=%s theme=%u saved=%d\n", options->token, theme, error);
	fflush(stdout);
}

/*
 * Opens the search bar (Edit > Find, step 0), or takes a step to the
 * next older (1) or newer (-1) match of the text looked for last; a step
 * without a text opens the bar instead (ws128-p006).
 */
static void
run_find(
	int step)
{
	/* The bar opens, showing the text looked for last; the main loop finds it again. */
	if (step == 0 || run_window.search_length == 0U) {
		run_window.search_open = 1;
		run_window.search_edited = 1;
		return;
	}

	/* The step, taken with the bar open. */
	run_window.search_open = 1;
	run_window.search_step = step;
}

/*
 * Finds the search bar's text (ws128-p006): a text that changed is looked
 * for back from the bottom of the window, a step goes on from the match
 * shown; a match found is brought into the window and marked.
 */
static void
run_search(
	const struct terminal_run_options *options)
{
	uint32_t query[TERMINAL_SEARCH_LENGTH];
	unsigned long from_line;
	unsigned long line;
	unsigned from_column;
	unsigned column;
	unsigned cells;
	size_t count;
	int direction;
	int edited;
	int found;

	/* Only a changed text or a step asks for a search. */
	if (!run_window.search_edited && run_window.search_step == 0)
		return;

	/* A closed bar marks nothing; the window is drawn without it. */
	if (!run_window.search_open) {
		run_window.search_edited = 0;
		run_window.search_step = 0;
		run_window.search_found = 0;
		run_screen->changed = 1;
		printf("ZTERM SEARCH run=%s closed\n", options->token);
		fflush(stdout);
		return;
	}

	/* The text's characters. */
	count = terminal_search_decode(run_window.search_query, run_window.search_length, query, TERMINAL_SEARCH_LENGTH);

	/* A changed text from below the window's last row back; a step from the match shown, its way. */
	direction = 1;
	from_line = run_screen->scrolled - run_screen->view + run_screen->rows;
	from_column = 0U;
	edited = run_window.search_edited;
	if (!edited && run_window.search_found) {
		direction = run_window.search_step;
		from_line = run_window.search_line;
		from_column = run_window.search_column;
	}

	/* The change and the step are taken. */
	run_window.search_edited = 0;
	run_window.search_step = 0;

	/* The match; none keeps the view where it is. */
	found = terminal_search_find(run_screen, query, count, from_line, from_column, direction, &line, &column, &cells);
	if (found) {
		run_window.search_found = 1;
		run_window.search_line = line;
		run_window.search_column = column;
		run_window.search_cells = cells;
		terminal_search_show(run_screen, line);
	} else if (edited) {
		/* A changed text that is nowhere has no match to show; a step past the last keeps the one shown. */
		run_window.search_found = 0;
	}

	/* The bar and the marks are drawn again. */
	run_screen->changed = 1;

	/* The log line the tests read. */
	printf("ZTERM SEARCH run=%s query=%s found=%d line=%lu column=%u cells=%u view=%u\n",
	       options->token,
	       run_window.search_query,
	       found,
	       run_window.search_line,
	       run_window.search_column,
	       run_window.search_cells,
	       run_screen->view);
	fflush(stdout);
}

/* Copies the selected text to the clipboard (Edit > Copy: the pointer's range, or the whole screen selected). */
static void
run_copy(void)
{
	/* Nothing is copied without a selection. */
	if (!run_screen->selected && !run_screen->range)
		return;

	/* The text, as it is on the screen now. */
	run_clipboard_length = terminal_screen_text(run_screen, run_clipboard, sizeof(run_clipboard));
	printf("ZTERM COPY bytes=%lu\n", (unsigned long)run_clipboard_length);
	fflush(stdout);

	/* It is the selection other clients paste (clipboard.c). */
	terminal_clipboard_set(&run_window, run_clipboard, run_clipboard_length);
}

/*
 * Starts pasting the clipboard into the shell (Edit > Paste): line breaks
 * become carriage returns, as the Enter key sends them.
 */
static void
run_start_paste(void)
{
	int own;

	/* Another client's selection is received first (the terminal's own copy is already here). */
	own = terminal_clipboard_own(&run_window);
	if (!own)
		run_clipboard_length = terminal_clipboard_receive(&run_window, run_clipboard, sizeof(run_clipboard));

	/* The clipboard's text, line breaks turned into what Enter sends; the main loop writes it as the shell takes it. */
	run_paste_begin(run_clipboard, run_clipboard_length, 1);
	printf("ZTERM PASTE bytes=%lu\n", (unsigned long)run_clipboard_length);
	fflush(stdout);
}

/*
 * Pastes the primary selection into the shell (a middle click, ws035-p100):
 * line breaks become carriage returns, as the Enter key sends them.
 */
static void
run_primary_paste(void)
{
	size_t length;

	/* A paste still being written is not interrupted. */
	if (run_paste_written < run_paste_length)
		return;

	/* The primary selection's text (the terminal's own, or another client's). */
	length = terminal_primary_receive(&run_window, run_received, sizeof(run_received));

	/* Line breaks are what Enter sends; the main loop writes it as the shell takes it. */
	run_paste_begin(run_received, length, 1);
	printf("ZTERM PASTE primary bytes=%lu\n", (unsigned long)length);
	fflush(stdout);
}

/*
 * Opens a new tab with a connection of its own, at the grid's size, and
 * makes it the active one.  Returns 0, or -1 with errno set when the
 * connection cannot be opened (a full window has no new tab and returns 0).
 */
static int
run_tab_new(
	const struct terminal_run_options *options,
	struct run_state *run)
{
	const struct terminal_connector *connector;
	struct terminal_connection *connection;
	struct run_tab *tab;
	struct terminal_screen *screen;
	unsigned width;
	unsigned height;
	int error;

	/* A full window has no new tab. */
	if (run_tab_count == TERMINAL_TABS)
		return 0;

	/* The tab's grid. */
	screen = malloc(sizeof(*screen));
	if (screen == NULL) {
		errno = ENOMEM;
		return -1;
	}

	/* Empty, at the grid's size, with the window's width setting. */
	terminal_screen_init(screen, run->columns, run->rows);
	terminal_screen_set_ambiguous_wide(screen, run->ambiguous_wide);

	/* Its connection, at the grid's size, which writes the tab's first title. */
	connector = options->connector;
	tab = &run_tabs[run_tab_count];
	width = run->columns * run_font.cell_width;
	height = run->rows * run_font.cell_height;
	error = connector->open(connector->data,
				run_window.app,
				run_tab_next,
				run->columns,
				run->rows,
				width,
				height,
				tab->title,
				sizeof(tab->title),
				&connection);
	if (error != 0) {
		free(screen);
		errno = error;
		return -1;
	}

	/* The tab, at the end, with its ID. */
	tab->screen = screen;
	tab->connection = connection;
	tab->id = run_tab_next;
	run_tab_next++;
	run_tab_count++;

	/* Succeeded: it is the active one. */
	run_tab_switch(run, run_tab_count - 1U);
	printf("ZTERM TAB new run=%s id=%u count=%u\n", options->token, tab->id, run_tab_count);
	fflush(stdout);
	return 0;
}

/* Makes a tab the active one: its grid is drawn and its shell takes the keys. */
static void
run_tab_switch(
	struct run_state *run,
	unsigned index)
{
	/* Only a tab that is there. */
	if (index >= run_tab_count)
		return;

	/* The active tab's grid and shell. */
	run_active = index;
	run_screen = run_tabs[index].screen;
	run->connection = run_tabs[index].connection;

	/* The keys not yet sent belonged to the tab before; its grid is drawn, without the other tab's search. */
	run_window.input_length = 0;
	run_window.search_open = 0;
	run_window.search_found = 0;
	run_screen->changed = 1;
	printf("ZTERM TAB active id=%u\n", run_tabs[index].id);
	fflush(stdout);
}

/*
 * Closes a tab: its connection is hung up on (a shell's process is reaped
 * by the library), its grid freed, and a neighbour becomes active when it
 * was.  Returns 1 when it was the last
 * tab (the terminal ends), 0 otherwise.
 */
static int
run_tab_close(
	const struct terminal_run_options *options,
	struct run_state *run,
	unsigned index)
{
	struct run_tab *tab;
	uint32_t id;

	/* Only a tab that is there. */
	if (index >= run_tab_count)
		return run_tab_count == 0U;

	/* Its connection, hung up on and closed. */
	tab = &run_tabs[index];
	id = tab->id;
	tab->connection->ops->close(tab->connection);
	tab->connection = NULL;

	/* Its grid. */
	free(tab->screen);

	/* The tabs after it move up. */
	run_tab_count--;
	memmove(&run_tabs[index], &run_tabs[index + 1U], (run_tab_count - index) * sizeof(run_tabs[0]));
	printf("ZTERM TAB closed run=%s id=%u count=%u\n", options->token, id, run_tab_count);
	fflush(stdout);

	/* The last tab: the terminal ends (no connection is active). */
	if (run_tab_count == 0U) {
		run_screen = NULL;
		run->connection = NULL;
		return 1;
	}

	/* The tab now at its place (or the last one) is active when it was; otherwise the active one may have moved up. */
	if (index == run_active) {
		if (index >= run_tab_count)
			index = run_tab_count - 1U;
		run_tab_switch(run, index);
	} else if (index < run_active) {
		run_tab_switch(run, run_active - 1U);
	}

	/* Succeeded: tabs remain. */
	return 0;
}

/* Finds a tab by its ID; -1 when there is none. */
static int
run_tab_find(
	uint32_t id)
{
	unsigned index;

	/* Each tab. */
	for (index = 0; index < run_tab_count; index++) {
		/* The tab with the ID. */
		if (run_tabs[index].id == id)
			return (int)index;
	}

	/* None. */
	return -1;
}

/*
 * Carries out what the titlebar asked of the tabs: a new tab, one chosen,
 * one closed.  Returns 1 when the last tab closed, -1 when a shell could
 * not be started, 0 otherwise.
 */
static int
run_tab_requests(
	const struct terminal_run_options *options,
	struct run_state *run)
{
	struct terminal_tab_request request;
	int taken;
	int found;
	int status;

	/* Each request in the order it came. */
	for (;;) {
		taken = terminal_tabs_take(&run_window, &request);
		if (taken == 0)
			break;

		/* A new tab. */
		status = 0;
		if (request.kind == TERMINAL_TAB_NEW)
			status = run_tab_new(options, run);
		if (status != 0)
			return -1;

		/* A tab chosen, or closed, that is still there. */
		found = run_tab_find(request.id);
		if (found < 0)
			continue;
		if (request.kind == TERMINAL_TAB_ACTIVATE)
			run_tab_switch(run, (unsigned)found);
		if (request.kind == TERMINAL_TAB_CLOSE) {
			status = run_tab_close(options, run, (unsigned)found);
			if (status != 0)
				return 1;
		}
	}

	/* Succeeded: the tabs are as asked. */
	return 0;
}

/*
 * Shows the tabs in the titlebar (tabs.c sends only a change): each tab's
 * title is the one its shell set (OSC 0 or 2), else "Shell N".  The
 * window's title follows the active tab's.
 */
static void
run_tabs_show(
	const struct terminal_run_options *options)
{
	struct terminal_tab_view views[TERMINAL_TABS];
	const char *title;
	unsigned index;
	int same;

	/* Each tab's ID and title. */
	for (index = 0; index < run_tab_count; index++) {
		views[index].id = run_tabs[index].id;
		title = run_tabs[index].title;
		if (run_tabs[index].screen->title[0] != '\0')
			title = run_tabs[index].screen->title;
		run_title_copy(views[index].title, sizeof(views[index].title), title);
	}

	/* With the active one. */
	if (run_tab_count == 0U)
		return;
	terminal_tabs_show(&run_window, views, run_tab_count, run_tabs[run_active].id);

	/* The window's title: the active tab's, or the application's name; only a change is sent. */
	title = options->title;
	if (run_tabs[run_active].screen->title[0] != '\0')
		title = run_tabs[run_active].screen->title;
	same = strcmp(title, run_window_title);
	if (same == 0)
		return;
	run_title_copy(run_window_title, sizeof(run_window_title), title);
	kl_window_set_title(run_window.kui, run_window_title);
	printf("ZTERM TITLE tab=%u title=%s\n", run_tabs[run_active].id, run_window_title);
	fflush(stdout);
}

/* Copies a title as far as it fits, never cutting a UTF-8 character in two. */
static void
run_title_copy(
	char *to,
	size_t size,
	const char *from)
{
	size_t length;

	/* The bytes that fit, less a character's leading part the cut would leave. */
	length = strlen(from);
	if (length >= size) {
		length = size - 1U;
		while (length > 0U && ((unsigned char)from[length] & 0xc0U) == 0x80U)
			length--;
	}

	/* Succeeded: the copy, ended. */
	memcpy(to, from, length);
	to[length] = '\0';
}

/* Pastes what was dropped on the window into the active shell, as a paste is. */
static void
run_drop_paste(void)
{
	size_t length;

	/* The dropped text (file names as quoted words). */
	length = terminal_clipboard_drop(&run_window, run_received, sizeof(run_received));

	/* As it is; the main loop writes it as the shell takes it. */
	run_paste_begin(run_received, length, 0);
}

/*
 * Starts writing a paste to the active connection: the text with its line
 * breaks turned into what Enter sends (lines nonzero) or as it is, between
 * ESC [ 200 ~ and ESC [ 201 ~ when the program asked for bracketed paste.
 * A bracketed paste leaves out the text's ESC bytes, so the text cannot end
 * the bracket before its end.
 */
static void
run_paste_begin(
	const char *text,
	size_t length,
	int lines)
{
	size_t index;
	size_t at;
	int bracketed;

	/* The opening bracket, when the program asked for it. */
	at = 0U;
	bracketed = 0;
	if (run_screen != NULL && run_screen->bracketed_paste)
		bracketed = 1;
	if (bracketed) {
		memcpy(run_paste, RUN_PASTE_START, sizeof(RUN_PASTE_START) - 1U);
		at = sizeof(RUN_PASTE_START) - 1U;
	}

	/* The text, a byte at a time (the buffer holds the largest text and both brackets). */
	for (index = 0U; index < length; index++) {
		/* An ESC in a bracketed paste is left out. */
		if (bracketed && text[index] == '\033')
			continue;

		/* A line break is what Enter sends; every other byte is itself. */
		if (lines && text[index] == '\n') {
			run_paste[at] = '\r';
		} else {
			run_paste[at] = text[index];
		}
		at++;
	}

	/* The closing bracket. */
	if (bracketed) {
		memcpy(run_paste + at, RUN_PASTE_END, sizeof(RUN_PASTE_END) - 1U);
		at += sizeof(RUN_PASTE_END) - 1U;
	}

	/* The main loop writes it from its start. */
	run_paste_length = at;
	run_paste_written = 0;
}

/* Takes the pointer's events: the left button selects, or drags the selected text out (ws035-p093). */
static void
run_pointer(void)
{
	const struct terminal_pointer_event *event;
	unsigned index;

	/* Each event, oldest first. */
	for (index = 0U; index < run_window.pointer_event_count; index++) {
		event = &run_window.pointer_events[index];
		if (event->kind == TERMINAL_POINTER_MIDDLE)
			run_primary_paste();
		else if (event->kind == TERMINAL_POINTER_PRESS)
			run_pointer_press(event);
		else if (event->kind == TERMINAL_POINTER_MOTION)
			run_pointer_motion(event);
		else
			run_pointer_release(event);
	}

	/* Succeeded: all taken. */
	run_window.pointer_event_count = 0U;
}

/*
 * A press of the left button: inside the range it may start a drag; with
 * Shift it extends the selection to the cell; otherwise one click starts a
 * selection, two select a word, three a line, and a drag after two or
 * three clicks goes on by whole words or lines.
 */
static void
run_pointer_press(
	const struct terminal_pointer_event *event)
{
	unsigned column;
	unsigned long line;
	unsigned from;
	unsigned to;
	int again;
	int inside;
	int extend;

	/* The cell, and whether the press repeats the last click there. */
	run_cell(event->x, event->y, &column, &line);
	again = 0;
	if (run_clicks > 0U &&
	    event->time - run_click_time <= RUN_CLICK_MS &&
	    column == run_click_column &&
	    line == run_click_line)
		again = 1;
	run_click_time = event->time;
	run_click_column = column;
	run_click_line = line;

	/* The pointer is on the grid where it pressed: no edge scrolls yet (ws035-p114). */
	run_pointer_x = event->x;
	run_pointer_y = event->y;
	run_edge = 0;

	/*
	 * Shift and a click extend the selection there from where the last
	 * one started (any earlier press set that place; ws035-p111).
	 */
	extend = 0;
	if ((event->modifiers & TERMINAL_MODIFIER_SHIFT) != 0U && run_clicks > 0U)
		extend = 1;
	if (extend) {
		run_clicks = 1U;
		run_unit = 1U;
		run_unit_moved = 1;
		run_selecting = 1;
		run_range(run_anchor_column, run_anchor_line, column, line);
		return;
	}

	/*
	 * A press while a tap's click on the range waits (BUG-276): on the
	 * range it may drag it out (a tap-and-drag) or end a double tap;
	 * elsewhere the wait ends and the range goes as the tap meant.
	 */
	inside = terminal_screen_in_range(run_screen, column, line);
	if (run_tap_clear_at != 0U) {
		run_tap_clear_at = 0U;
		if (inside) {
			run_drag_armed = 1;
			run_tap_second = 1;
			run_press_x = event->x;
			run_press_y = event->y;
			run_press_serial = event->serial;
			run_clicks = 1U;
			return;
		}

		/* Elsewhere: the range goes. */
		run_range_clear();
	}

	/* A press inside the range, not a repeated click, may drag it out. */
	if (inside && !again) {
		run_drag_armed = 1;
		run_press_x = event->x;
		run_press_y = event->y;
		run_press_serial = event->serial;
		run_clicks = 1U;
		return;
	}

	/* A second press soon after a tap may be a tap-and-drag: decided by its first move (BUG-276). */
	run_tap_drag = 0;
	if (again && run_clicks == 1U && run_last_tap)
		run_tap_drag = 1;

	/* How many clicks, up to three (a fourth starts again at one). */
	if (again)
		run_clicks = run_clicks % 3U + 1U;
	else
		run_clicks = 1U;
	run_screen->selected = 0;
	run_screen->changed = 1;

	/* The held button selects by the clicks' unit until it is released. */
	run_selecting = 1;
	run_unit = run_clicks;
	run_unit_moved = 0;

	/* One: the selection starts here and follows the pointer. */
	if (run_clicks == 1U) {
		run_screen->range = 0;
		run_anchor_column = column;
		run_anchor_line = line;
		return;
	}

	/* Two or three: the word or the line under the pointer is the range, and a drag grows from it. */
	run_unit_bounds(run_unit, column, line, &from, &to);
	run_unit_from[0] = from;
	run_unit_from[1] = line;
	run_unit_to[0] = to;
	run_unit_to[1] = line;
	run_anchor_column = from;
	run_anchor_line = line;
	run_range(from, line, to, line);

	/* Said, and the primary selection. */
	if (run_clicks == 2U)
		run_selected_log("word");
	else
		run_selected_log("line");
}

/*
 * Gives the first and the last column of the unit a cell is in: the cell
 * itself (1), its word, or the one character that is not a word's (2), or
 * its whole line (3).
 */
static void
run_unit_bounds(
	unsigned unit,
	unsigned column,
	unsigned long line,
	unsigned *from,
	unsigned *to)
{
	int word;

	/* A cell is its own unit. */
	*from = column;
	*to = column;
	if (unit <= 1U)
		return;

	/* A line runs across the grid. */
	if (unit >= 3U) {
		*from = 0U;
		*to = run_screen->columns - 1U;
		return;
	}

	/* A word: its letters to the left. */
	word = run_word_character(column, line);
	while (word && *from > 0U) {
		word = run_word_character(*from - 1U, line);
		if (word)
			(*from)--;
	}

	/* And to its right. */
	word = run_word_character(column, line);
	while (word && *to + 1U < run_screen->columns) {
		word = run_word_character(*to + 1U, line);
		if (word)
			(*to)++;
	}
}

/*
 * Grows a word or line selection to the unit under the pointer: the range
 * keeps the word or line first chosen and runs to the far end of the
 * pointer's unit on whichever side it is.
 */
static void
run_unit_extend(
	unsigned column,
	unsigned long line)
{
	unsigned from;
	unsigned to;
	int before;

	/* The unit under the pointer. */
	run_unit_bounds(run_unit, column, line, &from, &to);

	/* The pointer's unit before the first chosen one runs the range backward, otherwise forward. */
	before = 0;
	if (line < run_unit_from[1])
		before = 1;
	else if (line == run_unit_from[1] && from < run_unit_from[0])
		before = 1;
	if (before) {
		run_range(from, line, run_unit_to[0], run_unit_to[1]);
	} else {
		run_range(run_unit_from[0], run_unit_from[1], to, line);
	}

	/* The range changed since the press. */
	run_unit_moved = 1;
}

/* A motion: it drags the range out once it moves far enough from a press inside it, or extends a selection. */
static void
run_pointer_motion(
	const struct terminal_pointer_event *event)
{
	char text[4096];
	size_t length;
	unsigned column;
	unsigned long line;
	int32_t dx;
	int32_t dy;
	int32_t top;
	int32_t bottom;
	int edge;

	/* A press inside the range, moved far enough: the range's text is dragged out. */
	if (run_drag_armed) {
		dx = event->x - run_press_x;
		dy = event->y - run_press_y;
		if (dx > -RUN_DRAG_DISTANCE && dx < RUN_DRAG_DISTANCE && dy > -RUN_DRAG_DISTANCE && dy < RUN_DRAG_DISTANCE)
			return;
		run_drag_armed = 0;
		length = terminal_screen_text(run_screen, text, sizeof(text));
		terminal_clipboard_drag(&run_window, text, length, run_press_serial);
		return;
	}

	/* A selection follows the pointer from where it started. */
	if (!run_selecting)
		return;
	run_pointer_x = event->x;
	run_pointer_y = event->y;

	/*
	 * A tap-and-drag that leaves its cell is a drag of characters from the
	 * tapped cell, not a double click's drag by words (BUG-276: a touch
	 * pad's tap then a finger moved, where a hand would press the pad).
	 */
	if (run_tap_drag) {
		run_cell(event->x, event->y, &column, &line);
		if (column != run_click_column || line != run_click_line) {
			run_tap_drag = 0;
			run_clicks = 1U;
			run_unit = 1U;
			run_anchor_column = run_click_column;
			run_anchor_line = run_click_line;
			run_screen->range = 0;
			run_screen->changed = 1;
		}
	}

	/*
	 * Past the grid's top the view scrolls back into the scrollback, past
	 * its bottom toward the live screen: a step at once, then one every
	 * RUN_EDGE_MS while the pointer stays there (ws035-p114).
	 */
	top = (int32_t)TERMINAL_PADDING;
	bottom = top + (int32_t)(run_screen->rows * run_font.cell_height);
	edge = 0;
	if (event->y < top)
		edge = 1;
	else if (event->y >= bottom)
		edge = -1;
	if (edge != 0 && edge != run_edge)
		run_edge_at = terminal_clock();
	run_edge = edge;

	/* The selection runs to the pointer's cell. */
	run_select_to_pointer();
}

/*
 * Extends the held selection to the cell nearest the pointer's last place:
 * by words or lines after a double or triple click (ws035-p111), else a
 * cell at a time.
 */
static void
run_select_to_pointer(void)
{
	unsigned column;
	unsigned long line;

	/* The cell, on the lines the view shows now. */
	run_cell(run_pointer_x, run_pointer_y, &column, &line);

	/* After a double or triple click, by whole words or lines. */
	if (run_unit > 1U) {
		run_unit_extend(column, line);
		return;
	}

	/* A cell at a time; the pressed cell alone selects nothing yet. */
	if (column == run_anchor_column && line == run_anchor_line && !run_screen->range)
		return;
	run_range(run_anchor_column, run_anchor_line, column, line);
}

/*
 * Scrolls the view a step while a held selection's pointer is past the
 * grid's top or bottom, and runs the selection onto the line the edge
 * then shows (ws035-p114).  The farther past the edge, the more lines a
 * step scrolls.
 */
static void
run_edge_scroll(
	uint64_t now)
{
	int32_t beyond;
	unsigned lines;
	int moved;

	/* Only a held selection with the pointer past an edge, once its step is due. */
	if (!run_selecting || run_edge == 0)
		return;
	if (now < run_edge_at)
		return;

	/* How far past the edge, in pixels (at least one). */
	if (run_edge > 0) {
		beyond = (int32_t)TERMINAL_PADDING - run_pointer_y;
	} else {
		beyond = run_pointer_y - (int32_t)TERMINAL_PADDING - (int32_t)(run_screen->rows * run_font.cell_height) + 1;
	}

	/* A pointer that has just come back counts as one pixel past. */
	if (beyond < 1)
		beyond = 1;

	/* A line for each cell's height past the edge, up to a limit. */
	lines = 1U + (unsigned)(beyond - 1) / run_font.cell_height;
	if (lines > RUN_EDGE_LINES)
		lines = RUN_EDGE_LINES;

	/* The step, and when the next is due; at the scrollback's end or on the live screen nothing moves. */
	run_edge_at = now + RUN_EDGE_MS;
	moved = terminal_screen_scroll_view(run_screen, run_edge * (int)lines);
	if (!moved)
		return;

	/* The selection reaches the line now at the edge. */
	run_select_to_pointer();
	run_view_log("edge");
}

/* Scrolls the view as the wheel's notches and Shift+Page Up or Down asked (ws035-p114). */
static void
run_scroll(void)
{
	unsigned page;
	int lines;
	int moved;

	/* Nothing asked, nothing to scroll. */
	if (run_window.scroll_notches == 0 && run_window.scroll_pages == 0)
		return;

	/* A few lines a notch; a page is the grid's rows less one, which stays in sight. */
	page = run_screen->rows - 1U;
	if (page == 0U)
		page = 1U;
	lines = run_window.scroll_notches * RUN_WHEEL_LINES + run_window.scroll_pages * (int)page;
	run_window.scroll_notches = 0;
	run_window.scroll_pages = 0;

	/* The view moves within the scrollback. */
	moved = terminal_screen_scroll_view(run_screen, lines);
	if (!moved)
		return;

	/* A held selection runs onto the lines now under the pointer. */
	if (run_selecting)
		run_select_to_pointer();

	/* Said, for the tests. */
	run_view_log("scroll");
}

/* A release: a press inside the range that did not drag clears it (a click); a selection ends. */
static void
run_pointer_release(
	const struct terminal_pointer_event *event)
{
	/* The edge stops scrolling with the button's release (ws035-p114). */
	run_edge = 0;

	/* A release at the press's own time ends a touch pad's tap (BUG-276); a held press is a hand's. */
	run_last_tap = 0;
	if (event->time == run_click_time)
		run_last_tap = 1;
	run_tap_drag = 0;

	/*
	 * A click inside the range clears it, and a Shift click later extends
	 * from there.  A touch pad's tap waits first for the tap-and-drag that
	 * may follow (BUG-276); the second tap of a double tap on the range
	 * selects the word, as a double click does.
	 */
	if (run_drag_armed) {
		run_drag_armed = 0;
		if (run_tap_second) {
			run_tap_second = 0;
			run_select_word(run_click_column, run_click_line);
			return;
		}

		/* A tap's click waits for its drag. */
		if (run_last_tap) {
			run_tap_clear_at = terminal_clock() + RUN_TAP_DRAG_MS;
			return;
		}

		/* A hand's click clears it now. */
		run_range_clear();
		return;
	}

	/* A selection made by the pointer's move. */
	if (!run_selecting)
		return;
	run_selecting = 0;

	/* A word or line selection says so only when the drag grew it (the press said the first one). */
	if (run_unit == 2U) {
		if (run_unit_moved)
			run_selected_log("words");
		return;
	}

	/* The same for lines. */
	if (run_unit == 3U) {
		if (run_unit_moved)
			run_selected_log("lines");
		return;
	}

	/* A cell selection, dragged or extended with Shift. */
	if (run_screen->range)
		run_selected_log("drag");
}

/* Clears the range a tap on it left waiting, once no tap-and-drag came in time (BUG-276). */
static void
run_tap_expire(
	uint64_t now)
{
	/* Nothing waits, or not yet. */
	if (run_tap_clear_at == 0U || now < run_tap_clear_at)
		return;

	/* Succeeded: the tap's click clears the range as a click does. */
	run_tap_clear_at = 0U;
	run_range_clear();
}

/* Clears the range as a click inside it does: a Shift click later extends from the clicked cell. */
static void
run_range_clear(void)
{
	/* No range; the anchor at the click. */
	run_screen->range = 0;
	run_screen->changed = 1;
	run_anchor_column = run_click_column;
	run_anchor_line = run_click_line;
}

/* Selects the word under a cell, as a double click does (the second tap of a double tap on the range, BUG-276). */
static void
run_select_word(
	unsigned column,
	unsigned long line)
{
	unsigned from;
	unsigned to;

	/* The word's bounds, the range on them, a drag after it by words. */
	run_clicks = 2U;
	run_unit = 2U;
	run_unit_moved = 0;
	run_unit_bounds(run_unit, column, line, &from, &to);
	run_unit_from[0] = from;
	run_unit_from[1] = line;
	run_unit_to[0] = to;
	run_unit_to[1] = line;
	run_anchor_column = from;
	run_anchor_line = line;
	run_range(from, line, to, line);

	/* Said, and the primary selection. */
	run_selected_log("word");
}

/*
 * Gives the cell under a point of the surface, the nearest one for a point
 * outside the grid: its column, and the line the view shows on its row.
 */
static void
run_cell(
	int32_t x,
	int32_t y,
	unsigned *column,
	unsigned long *line)
{
	unsigned row;

	/* From the grid's padded top left, with the text moved by the view's offset within a line (ws081-p011), a cell's size at a time. */
	x -= (int32_t)TERMINAL_PADDING;
	y -= (int32_t)TERMINAL_PADDING + run_screen->view_offset;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	*column = (unsigned)x / run_font.cell_width;
	row = (unsigned)y / run_font.cell_height;

	/* The nearest cell inside the grid. */
	if (*column >= run_screen->columns)
		*column = run_screen->columns - 1U;
	if (row >= run_screen->rows)
		row = run_screen->rows - 1U;

	/* Succeeded: the line on that row, in the scrollback when the view is back (ws035-p114). */
	*line = terminal_screen_view_line(run_screen, row);
}

/* Selects the cells from one to another, in whichever order they come. */
static void
run_range(
	unsigned from_column,
	unsigned long from_line,
	unsigned to_column,
	unsigned long to_line)
{
	unsigned long first[2];
	unsigned long last[2];
	int later;

	/* The first in reading order first. */
	later = 0;
	if (from_line > to_line)
		later = 1;
	else if (from_line == to_line && from_column > to_column)
		later = 1;
	if (later) {
		first[0] = to_column;
		first[1] = to_line;
		last[0] = from_column;
		last[1] = from_line;
	} else {
		first[0] = from_column;
		first[1] = from_line;
		last[0] = to_column;
		last[1] = to_line;
	}

	/* The same range as shown (the pointer moved within a cell): nothing to draw anew. */
	if (run_screen->range &&
	    run_screen->range_from[0] == first[0] &&
	    run_screen->range_from[1] == first[1] &&
	    run_screen->range_to[0] == last[0] &&
	    run_screen->range_to[1] == last[1])
		return;

	/* Succeeded: drawn anew. */
	run_screen->range_from[0] = first[0];
	run_screen->range_from[1] = first[1];
	run_screen->range_to[0] = last[0];
	run_screen->range_to[1] = last[1];
	run_screen->range = 1;
	run_screen->changed = 1;
}

/* Tells whether a cell's character belongs to a word (not a space nor one of the characters that part words in a shell). */
static int
run_word_character(
	unsigned column,
	unsigned long line)
{
	const struct terminal_cell *cell;
	uint32_t codepoint;
	const char *found;

	/* A line no longer kept has no words (ws035-p114). */
	cell = terminal_screen_line_cell(run_screen, column, line);
	if (cell == NULL)
		return 0;

	/* A blank or a space parts words. */
	codepoint = cell->codepoint;
	if (codepoint == 0U || codepoint == ' ' || codepoint == '\t')
		return 0;

	/* Any other character but the shell's punctuation is a word's. */
	if (codepoint >= 0x80U)
		return 1;
	found = strchr("\"'`()[]{}<>|;&,", (int)codepoint);
	if (found != NULL)
		return 0;

	/* Succeeded: a word's. */
	return 1;
}

/* Logs what the pointer selected: how, where (column, line) and how many bytes of text. */
static void
run_selected_log(
	const char *how)
{
	size_t length;

	/* The range's text, whole (as long as the buffer holds), into what the primary selection sends. */
	length = terminal_screen_text(run_screen, run_primary, sizeof(run_primary));
	printf("ZTERM SELECT how=%s from=%lu,%lu to=%lu,%lu bytes=%lu\n", how, run_screen->range_from[0], run_screen->range_from[1], run_screen->range_to[0], run_screen->range_to[1], (unsigned long)length);
	fflush(stdout);

	/* The selected text is the primary selection (ws035-p100). */
	terminal_primary_set(&run_window, run_primary, length);
}

/*
 * Logs where the view is: how it moved, how many lines back from the live
 * screen it is, how many lines the scrollback keeps and how many have
 * ever scrolled off (ws035-p114).
 */
static void
run_view_log(
	const char *how)
{
	/* One line, for the tests. */
	printf("ZTERM VIEW how=%s back=%u history=%u scrolled=%lu\n", how, run_screen->view, run_screen->history_count, run_screen->scrolled);
	fflush(stdout);
}

/*
 * Runs the fingers for one round (ws081-p011): gives them the screen's
 * view, takes their events, sets the view they moved to, and queues the
 * pointer's presses, releases and motions they made for run_pointer.  A
 * long press's hold (ws081-p014) is a press held on the selection (which a
 * drag then takes out), elsewhere a double click held (the word under the
 * finger, grown by words).
 */
static void
run_touch_round(void)
{
	struct terminal_touch_pointer made;
	unsigned kinds[3];
	unsigned kind_count;
	unsigned kind;
	unsigned index;
	unsigned column;
	unsigned long line;
	unsigned view;
	int offset;
	int moved;
	int taken;
	int inside;

	/* The screen's view, and the fingers' events. */
	terminal_touch_layout(&run_touch, run_screen, run_font.cell_height, run_screen->history_count,
			      run_screen->rows * run_font.cell_height, run_screen->view, run_screen->view_offset);
	for (index = 0U; index < run_window.touch_count; index++)
		terminal_touch_event(&run_touch, &run_window.touches[index]);
	run_window.touch_count = 0U;

	/* Time moves on for them; a new view is shown. */
	run_touch_due = terminal_touch_tick(&run_touch, terminal_touch_clock());
	moved = terminal_touch_view(&run_touch, &view, &offset);
	if (moved) {
		run_screen->view = view;
		run_screen->view_offset = offset;
		run_screen->changed = 1;
	}

	/* The pointer's events they made, after the pointer's own. */
	for (;;) {
		taken = terminal_touch_take_pointer(&run_touch, &made);
		if (!taken)
			break;

		/* A hold is a press on the selection, or a double click held elsewhere; the rest are what they say. */
		kinds[0] = made.kind;
		kind_count = 1U;
		if (made.kind == TERMINAL_TOUCH_HOLD) {
			run_cell(made.x, made.y, &column, &line);
			inside = terminal_screen_in_range(run_screen, column, line);
			kinds[0] = TERMINAL_POINTER_PRESS;
			if (!inside) {
				kinds[1] = TERMINAL_POINTER_RELEASE;
				kinds[2] = TERMINAL_POINTER_PRESS;
				kind_count = 3U;
			}

			/* Records which held selection supplied the pointer press. */
			printf("ZTERM TOUCH hold on-selection=%d\n", inside);
			fflush(stdout);
		}

		/* Each, queued (a full queue drops it). */
		for (kind = 0U; kind < kind_count; kind++)
			run_touch_queue(kinds[kind], &made);
	}
}

/* Queues a pointer event the fingers made, at their place and time (a full queue drops it). */
static void
run_touch_queue(
	unsigned kind,
	const struct terminal_touch_pointer *made)
{
	struct terminal_pointer_event *event;

	/* Room for it. */
	if (run_window.pointer_event_count >= TERMINAL_POINTER_EVENTS)
		return;

	/* The event, after the others. */
	event = &run_window.pointer_events[run_window.pointer_event_count];
	run_window.pointer_event_count++;
	event->kind = kind;
	event->x = made->x;
	event->y = made->y;
	event->time = made->time;
	event->serial = made->serial;
	event->modifiers = run_window.modifiers;
}

/*
 * Tells the input method where the cursor's cell is on the window (surface
 * pixels), or the search bar's caret while it is open, so that its
 * candidate list opens beside the text being composed (BUG-155,
 * ws090-p022); libkeiland sends only a change.
 */
static void
run_text_cursor(void)
{
	unsigned column;
	int x;
	int y;

	/* The cursor's cell, on the rows the view shows (a view scrolled back moves it down). */
	x = (int)TERMINAL_PADDING + (int)(run_screen->cursor_column * run_font.cell_width);
	y = (int)TERMINAL_PADDING + (int)((run_screen->cursor_row + run_screen->view) * run_font.cell_height) + run_screen->view_offset;

	/* While the search bar is open: its caret's cell on the last row, where the composed text shows (ws090-p022). */
	if (run_window.search_open && run_screen->rows != 0U) {
		column = terminal_search_bar_column(run_window.search_query, run_window.search_length, run_screen->ambiguous_wide);
		x = (int)TERMINAL_PADDING + (int)(column * run_font.cell_width);
		y = (int)TERMINAL_PADDING + (int)((run_screen->rows - 1U) * run_font.cell_height);
	}

	/* The cell's rectangle. */
	kl_window_text_cursor(run_window.kui, x, y, (int)run_font.cell_width, (int)run_font.cell_height);
}

/*
 * Gives the pixels a font size is drawn at on the screen: the size, at 96
 * dots an inch, times the screen's scale in 96ths (the same size at the
 * scale of 96).
 */
static unsigned
run_font_pixels(
	const struct run_state *run,
	unsigned pixels)
{
	unsigned drawn;

	/* The size at the screen's scale, rounded to the nearest pixel. */
	drawn = (pixels * run->scale + 48U) / 96U;

	/* Reports the size drawn at. */
	return drawn;
}
