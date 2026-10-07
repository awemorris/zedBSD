/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * terminal: a VT100 terminal in a Wayland window, drawn with Vulkan.
 *
 * It runs a shell (or a command) on a pseudo-terminal, shows what it writes
 * and sends it the keys typed.  The window follows the size the compositor
 * gives it, and the shell is told the grid's new size.  The terminal ends
 * when the shell exits or the window is closed.
 *
 * Each tab of the window (tabs.c, ws035-p086: the titlebar's tabs) is a
 * shell of its own with its own grid; the keys go to the active tab's
 * shell, and every shell is read so none of them waits.  A tab whose shell
 * exits closes; the last one ends the terminal.
 *
 * Its menus (menu.c) are drawn by the compositor; what they choose is carried out
 * here: a new window (another terminal), closing, the selection and the
 * clipboard (the compositor's, shared with other clients, clipboard.c), the
 * font's size, fullscreen, keys for the shell, clearing and
 * resetting the screen, and a line about the terminal.
 *
 * Every outcome is one line on standard output: ZTERM DONE on a normal end
 * (with the reason), ZTERM FAILED naming what failed otherwise.
 */

#include "terminal.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/*
 * openpty and forkpty: <libutil.h> on FreeBSD, <pty.h> elsewhere (the
 * terminal's own pseudo-terminals, an application's OS dependency outside
 * the desktop's boundary, WS131 D14).
 */
#if defined(__FreeBSD__)
#include <libutil.h>
#else
#include <pty.h>
#endif

/* The font the terminal uses unless told otherwise, and its size in pixels. */
#define MAIN_FONT		KEILAND_DATADIR "/fonts/keiland-mono.ttf"
#define MAIN_FONT_PIXELS	16U

/* The font that draws what the monospaced font has no glyph for (CJK), unless told otherwise. */
#define MAIN_FALLBACK_FONT	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"

/* The grid the window opens with. */
#define MAIN_COLUMNS		90U
#define MAIN_ROWS		28U

/* The shell run when no command is given. */
#define MAIN_SHELL		"/bin/sh"

/* How many redraws in a row may find the swapchain out of date before the terminal gives up. */
#define MAIN_STALE_LIMIT	8U

/* The program started for a new window when the terminal was not run by a path. */
#define MAIN_PROGRAM		KEILAND_BINDIR "/terminal"

/* The most text the clipboard holds: every cell as four UTF-8 bytes, and a line break per row. */
#define MAIN_CLIPBOARD_MAX	(TERMINAL_MAX_COLUMNS * TERMINAL_MAX_ROWS * 4U + TERMINAL_MAX_ROWS)

/* What Help > About Terminal writes on the screen. */
#define MAIN_ABOUT		"\r\n\033[1mTerminal\033[0m: a VT100 terminal for Kei, drawn with Vulkan.\r\n" \
				"Its menus are drawn by the System Menu (xdg_toplevel_menu_v1).\r\n"

/*
 * What the command line asked for.
 */
struct main_options {
	const char *program;
	const char *display;
	const char *font;
	const char *fallback;
	const char *command;
	const char *token;
	unsigned pixels;
	int pixels_given;
	unsigned columns;
	unsigned rows;
	unsigned timeout;
};

/*
 * How the run is going: the shell, and what the error line names when
 * something fails.
 */
struct main_run {
	/* The shell's process and its pseudo-terminal (-1 when not started). */
	pid_t child;
	int master;

	/* The grid's size, as last told to the shell, and the font's size in pixels. */
	unsigned columns;
	unsigned rows;
	unsigned pixels;

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

	/* What failed last, the Vulkan result of it, and why the run ended normally. */
	const char *operation;
	VkResult result;
	const char *reason;
};

/*
 * The terminal's parts, for the whole run.  They are file-scope because the
 * grid alone is too large for the stack.
 */
static struct terminal_font main_font;
static struct terminal_window main_window;
static struct terminal_renderer main_renderer;

/*
 * The touch screen (touch.c, ws081-p011): the fingers' gestures and
 * scroller, made with the window (without them fingers do nothing).
 */
static struct terminal_touch main_touch;

/* In how many milliseconds the fingers want the next round (-1: none). */
static int main_touch_due = -1;

/*
 * One tab: its grid (allocated, too large for the stack), its shell's
 * process and pseudo-terminal, its ID in the titlebar and its title.
 */
struct main_tab {
	struct terminal_screen *screen;
	pid_t child;
	int master;
	uint32_t id;
	char title[TERMINAL_TAB_TITLE];
};

/*
 * The tabs, in their order, how many there are, the active one (whose grid
 * is drawn and whose shell takes the keys) and the ID the next tab gets.
 * main_screen is the active tab's grid; run->child and run->master are the
 * active tab's shell.  They live as long as the terminal.
 */
static struct main_tab main_tabs[TERMINAL_TABS];
static unsigned main_tab_count;
static unsigned main_active;
static uint32_t main_tab_next = 1U;
static struct terminal_screen *main_screen;

/*
 * The clipboard's text and its length: the terminal's copy (Edit > Copy),
 * which its data source sends to other clients while it is the selection,
 * or another client's selection received for a paste; and the text being
 * pasted into the shell (Edit > Paste) with how much of it was written.
 * They live as long as the terminal.
 */
static char main_clipboard[MAIN_CLIPBOARD_MAX];
static size_t main_clipboard_length;
static char main_paste[MAIN_CLIPBOARD_MAX];

/*
 * The pointer's selection (ws035-p093): whether the left button is held
 * for a selection, or was pressed inside the range (a move then drags the
 * text out), where and with which serial, the press before (for double and
 * triple clicks: its time, its cell and how many clicks it made), and the
 * cell a drag of the range started at.  A cell is a column and a line
 * (screen.c numbers lines so they follow the text into the scrollback,
 * ws035-p114).
 */
static int main_selecting;
static int main_drag_armed;
static int32_t main_press_x;
static int32_t main_press_y;
static uint32_t main_press_serial;
static uint32_t main_click_time;
static unsigned main_click_column;
static unsigned long main_click_line;
static unsigned main_clicks;
static unsigned main_anchor_column;
static unsigned long main_anchor_line;

/*
 * The unit a held selection grows by (ws035-p111): 1 a cell (a click), 2 a
 * word (a double click), 3 a line (a triple click); the word or line the
 * double or triple click chose, which a drag keeps selected while it adds
 * whole words or lines on the side the pointer goes; and whether the drag
 * changed the range since the press.
 */
static unsigned main_unit;
static unsigned long main_unit_from[2];
static unsigned long main_unit_to[2];
static int main_unit_moved;

/*
 * The edge scrolling of a held selection (ws035-p114): where the pointer
 * last was while the button selects, which way the view scrolls because
 * the pointer is past the grid's top (1, back into the scrollback) or
 * bottom (-1, toward the live screen) or 0 while it is on the grid, and
 * when the next step is due, in milliseconds.
 */
static int32_t main_pointer_x;
static int32_t main_pointer_y;
static int main_edge;
static uint64_t main_edge_at;

/* How close in time a click makes a double or triple click, and how far a press moves before it drags, in milliseconds and pixels. */
#define MAIN_CLICK_MS		400U
#define MAIN_DRAG_DISTANCE	6

/* How often a selection held past the grid's edge scrolls, in milliseconds, and the most lines one step scrolls (ws035-p114). */
#define MAIN_EDGE_MS		60U
#define MAIN_EDGE_LINES		8U

/* How many lines one notch of the wheel scrolls the view (ws035-p114). */
#define MAIN_WHEEL_LINES	3

/* The window's title as last set: the active tab's (OSC 0 or 2), or "Terminal" (ws035-p091). */
static char main_window_title[TERMINAL_TITLE];
static size_t main_paste_length;

/* The text selected with the pointer, which the primary selection sends (primary.c keeps a pointer to it). */
static char main_primary[MAIN_CLIPBOARD_MAX];
static size_t main_paste_written;

/*
 * Whether the active shell reads a secret (ws095-p006): its echo off on a
 * whole line, as a password prompt has it.  The window's text input is off
 * meanwhile, so the input method composes nothing and the keys go as they are.
 */
static int main_secret;

static int main_parse(int argc, char **argv, struct main_options *options);
static const char *main_value(const char *argument, const char *name);
static int main_number(const char *text, unsigned maximum, unsigned *value);
static int main_start(const struct main_options *options, struct main_run *run);
static int main_loop(const struct main_options *options, struct main_run *run);
static int main_resize(const struct main_options *options, struct main_run *run);
static pid_t main_spawn(const struct main_options *options, int *master);
static void main_grid(unsigned width, unsigned height, unsigned *columns, unsigned *rows);
static void main_tell_size(int master, unsigned columns, unsigned rows);
static int main_read_shell(int master, struct terminal_screen *screen);
static int main_write_shell(int master);
static int main_menu_actions(const struct main_options *options, struct main_run *run);
static void main_menu_state(const struct main_run *run, struct terminal_menu_state *state);
static void main_new_window(const struct main_options *options, const struct main_run *run);
static int main_zoom(const struct main_options *options, struct main_run *run, unsigned pixels);
static void main_ambiguous_wide(const struct main_options *options, struct main_run *run);
static int main_save_settings(const struct main_run *run);
static void main_theme(const struct main_options *options, struct main_run *run, unsigned theme);
static void main_find(int step);
static void main_search(const struct main_options *options);
static void main_copy(void);
static void main_start_paste(void);
static void main_drop_paste(void);
static int main_tab_new(const struct main_options *options, struct main_run *run);
static void main_tab_switch(struct main_run *run, unsigned index);
static int main_tab_close(const struct main_options *options, struct main_run *run, unsigned index);
static int main_tab_find(uint32_t id);
static int main_tab_requests(const struct main_options *options, struct main_run *run);
static void main_tabs_show(void);
static void main_title_copy(char *to, size_t size, const char *from);
static void main_pointer(void);
static void main_pointer_press(const struct terminal_pointer_event *event);
static void main_primary_paste(void);
static void main_pointer_motion(const struct terminal_pointer_event *event);
static void main_pointer_release(void);
static void main_cell(int32_t x, int32_t y, unsigned *column, unsigned long *line);
static void main_range(unsigned from_column, unsigned long from_line, unsigned to_column, unsigned long to_line);
static int main_word_character(unsigned column, unsigned long line);
static void main_unit_bounds(unsigned unit, unsigned column, unsigned long line, unsigned *from, unsigned *to);
static void main_unit_extend(unsigned column, unsigned long line);
static void main_selected_log(const char *how);
static void main_select_to_pointer(void);
static void main_edge_scroll(uint64_t now);
static void main_scroll(void);
static void main_view_log(const char *how);
static void main_touch_round(void);
static void main_text_cursor(void);
static void main_secret_follow(int master);
static void main_touch_queue(unsigned kind, const struct terminal_touch_pointer *made);

/*
 * Runs the terminal.
 */
int
main(
	int argc,
	char **argv)
{
	struct main_options options;
	struct main_run run;
	int status;
	int exit_status;

	/* The command line. */
	status = main_parse(argc, argv, &options);
	if (status != 0) {
		fprintf(stderr, "usage: terminal [--display=NAME] [--font=PATH] [--fallback-font=PATH] [--font-size=PIXELS] [--columns=N] [--rows=N] [--command=COMMAND] [--token=NAME] [--timeout-s=N]\n");
		return 2;
	}

	/* A write to a shell that has gone must not kill the terminal. */
	(void)signal(SIGPIPE, SIG_IGN);
	memset(&run, 0, sizeof(run));
	run.child = -1;
	run.master = -1;
	run.operation = "none";
	run.result = VK_SUCCESS;
	run.reason = "closed";

	/* The font, the window, the drawing and the shell; then the terminal runs until it ends. */
	status = main_start(&options, &run);
	if (status == 0)
		status = main_loop(&options, &run);

	/* One line says how the run ended. */
	exit_status = 0;
	if (status == 0) {
		printf("ZTERM DONE run=%s reason=%s\n", options.token, run.reason);
	} else {
		printf("ZTERM FAILED run=%s operation=%s result=%d errno=%d\n", options.token, run.operation, (int)run.result, errno);
		exit_status = 1;
	}

	/* The line reaches whoever reads the log before the terminal goes. */
	fflush(stdout);

	/* Each tab's shell, if it still runs, is hung up on and reaped. */
	while (main_tab_count > 0U)
		(void)main_tab_close(&options, &run, main_tab_count - 1U);

	/* The drawing before the window it draws into, then the font. */
	terminal_renderer_close(&main_renderer);
	terminal_touch_close(&main_touch);
	terminal_window_close(&main_window);
	terminal_font_close(&main_font);

	/* Reports how the terminal ended. */
	return exit_status;
}

/* Reads the command line into the options; returns nonzero for a malformed one. */
static int
main_parse(
	int argc,
	char **argv,
	struct main_options *options)
{
	const char *value;
	const char *slash;
	int index;
	int status;

	/* The defaults. */
	memset(options, 0, sizeof(*options));
	options->program = MAIN_PROGRAM;
	options->display = NULL;
	options->font = MAIN_FONT;
	options->fallback = MAIN_FALLBACK_FONT;
	options->command = NULL;
	options->token = "term";
	options->pixels = MAIN_FONT_PIXELS;
	options->columns = MAIN_COLUMNS;
	options->rows = MAIN_ROWS;
	options->timeout = 0U;

	/* A program run by a path is run by the same path for a new window (a bare name is looked up as MAIN_PROGRAM). */
	slash = NULL;
	if (argc > 0 && argv[0] != NULL)
		slash = strchr(argv[0], '/');

	/* The path it was run by. */
	if (slash != NULL)
		options->program = argv[0];

	/* Each option in turn; the first name that matches takes it. */
	for (index = 1; index < argc; index++) {
		/* The compositor's socket. */
		value = main_value(argv[index], "--display=");
		if (value != NULL) {
			options->display = value;
			continue;
		}

		/* The font file. */
		value = main_value(argv[index], "--font=");
		if (value != NULL) {
			options->font = value;
			continue;
		}

		/* The fallback font file. */
		value = main_value(argv[index], "--fallback-font=");
		if (value != NULL) {
			options->fallback = value;
			continue;
		}

		/* The font's size in pixels. */
		value = main_value(argv[index], "--font-size=");
		if (value != NULL) {
			status = main_number(value, 96U, &options->pixels);
			if (status != 0)
				return -1;
			options->pixels_given = 1;
			continue;
		}

		/* The grid the window opens with. */
		value = main_value(argv[index], "--columns=");
		if (value != NULL) {
			status = main_number(value, TERMINAL_MAX_COLUMNS, &options->columns);
			if (status != 0)
				return -1;
			continue;
		}

		/* The rows the window opens with. */
		value = main_value(argv[index], "--rows=");
		if (value != NULL) {
			status = main_number(value, TERMINAL_MAX_ROWS, &options->rows);
			if (status != 0)
				return -1;
			continue;
		}

		/* A command run by the shell in place of an interactive shell. */
		value = main_value(argv[index], "--command=");
		if (value != NULL) {
			options->command = value;
			continue;
		}

		/* The name of the run in the log lines. */
		value = main_value(argv[index], "--token=");
		if (value != NULL) {
			options->token = value;
			continue;
		}

		/* A deadline in seconds, 0 for none. */
		value = main_value(argv[index], "--timeout-s=");
		if (value != NULL) {
			status = main_number(value, 86400U, &options->timeout);
			if (status != 0)
				return -1;
			continue;
		}

		/* An unknown option refuses the command line. */
		return -1;
	}

	/* A font too small, or an empty grid, is refused. */
	if (options->pixels < 6U || options->columns == 0U || options->rows == 0U)
		return -1;

	/* Succeeded: the options. */
	return 0;
}

/* Returns what follows an option's name in an argument, or NULL when the argument is another option. */
static const char *
main_value(
	const char *argument,
	const char *name)
{
	size_t length;
	int differs;

	/* The argument must start with the name, equals sign included. */
	length = strlen(name);
	differs = strncmp(argument, name, length);
	if (differs != 0)
		return NULL;

	/* Reports the value after the name. */
	return argument + length;
}

/* Opens the font, the window and the drawing, sizes the grid and starts the shell; returns nonzero on failure. */
static int
main_start(
	const struct main_options *options,
	struct main_run *run)
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

	/* The font, which sets the cell's size. */
	run->operation = "terminal_font_open";
	status = terminal_font_open(&main_font, options->font, pixels);
	if (status != 0) {
		errno = status;
		return -1;
	}

	/* The fallback font for the characters the font has not got; without it they show the font's missing glyph. */
	status = terminal_font_fallback(&main_font, options->fallback);
	if (status != 0)
		printf("ZTERM FONT fallback=none error=%d\n", status);

	/* The window, sized for the grid asked for. */
	run->operation = "terminal_window_open";
	status = terminal_window_open(&main_window, options->display,
				      options->columns * main_font.cell_width + 2U * TERMINAL_PADDING,
				      options->rows * main_font.cell_height + 2U * TERMINAL_PADDING);
	if (status != 0)
		return -1;

	/* The fingers; without memory for them they do nothing. */
	status = terminal_touch_open(&main_touch);
	if (status != 0)
		printf("ZTERM TOUCH none error=%d\n", status);

	/* The drawing, at the size the compositor gave. */
	run->result = terminal_renderer_open(&main_renderer, &main_window, &main_font);
	run->operation = main_renderer.operation;
	if (run->result != VK_SUCCESS)
		return -1;

	/* The grid that fits the window, and the theme it is drawn in. */
	run->pixels = pixels;
	main_window.theme = run->theme;
	main_grid(main_renderer.extent.width, main_renderer.extent.height, &run->columns, &run->rows);

	/* The menus, which the compositor draws (none from a compositor without the System Menu). */
	main_menu_state(run, &state);
	run->operation = "terminal_menu_open";
	status = terminal_menu_open(&main_window, &state);
	if (status != 0)
		return -1;

	/* The first tab: a shell on a pseudo-terminal of that size. */
	run->operation = "forkpty";
	status = main_tab_new(options, run);
	if (status != 0)
		return -1;

	/* Succeeded: the terminal is up. */
	printf("ZTERM START run=%s columns=%u rows=%u cell=%ux%u window=%ux%u\n",
	       options->token, run->columns, run->rows, main_font.cell_width, main_font.cell_height,
	       main_renderer.extent.width, main_renderer.extent.height);
	fflush(stdout);
	return 0;
}

/* Runs until the shell ends or the window is closed; returns nonzero on a failure. */
static int
main_loop(
	const struct main_options *options,
	struct main_run *run)
{
	struct terminal_menu_state state;
	uint64_t started;
	uint64_t now;
	unsigned stale;
	unsigned index;
	int masters[TERMINAL_TABS];
	int ready[TERMINAL_TABS];
	int status;
	int timeout;

	/* One round per event: wait, read the shell, send the keys, redraw what changed. */
	started = terminal_clock();
	stale = 0U;
	for (;;) {
		/* Waits for the compositor or the shell (or less, until a held key repeats: the application's). */
		timeout = 1000;
		now = terminal_clock();

		/* The fingers' next round, when it comes first (ws081-p011). */
		if (main_touch_due >= 0 && main_touch_due < timeout)
			timeout = main_touch_due;

		/* A selection held past the grid's edge scrolls on its own time, even while the pointer rests (ws035-p114). */
		if (main_selecting && main_edge != 0) {
			if (main_edge_at <= now)
				timeout = 0;
			else if ((uint64_t)timeout > main_edge_at - now)
				timeout = (int)(main_edge_at - now);
		}

		/* Runs the compositor's events and learns which shells wrote. */
		for (index = 0; index < main_tab_count; index++)
			masters[index] = main_tabs[index].master;
		run->operation = "terminal_window_dispatch";
		status = terminal_window_dispatch(&main_window, masters, main_tab_count, timeout, ready);
		if (status != 0)
			return -1;

		/* The compositor closing the window ends the terminal. */
		if (main_window.closed) {
			run->reason = "closed";
			return 0;
		}

		/* The menus' choices are carried out; Close Window (or closing the last tab) ends the terminal. */
		status = main_menu_actions(options, run);
		if (status < 0)
			return -1;
		if (status > 0) {
			run->reason = "menu-close";
			return 0;
		}

		/* What the titlebar asked of the tabs; closing the last one ends the terminal. */
		status = main_tab_requests(options, run);
		if (status < 0)
			return -1;
		if (status > 0) {
			run->reason = "tabs-closed";
			return 0;
		}

		/* What each shell wrote goes on its grid, from the last tab so a closed one leaves the others' places; a shell's end closes its tab. */
		for (index = main_tab_count; index > 0U; index--) {
			/* A shell that did not write. */
			if (index - 1U >= TERMINAL_TABS || !ready[index - 1U])
				continue;

			/* Its bytes on its grid; its end closes its tab, and the last tab's ends the terminal. */
			status = main_read_shell(main_tabs[index - 1U].master, main_tabs[index - 1U].screen);
			if (status != 0) {
				status = main_tab_close(options, run, index - 1U);
				if (status != 0) {
					run->reason = "shell-exited";
					return 0;
				}
			}
		}

		/* A drop on the window is pasted into the active shell (clipboard.c). */
		if (main_window.drop_pending)
			main_drop_paste();

		/* The fingers scroll the view, and their taps and long presses become the pointer's presses (ws081-p011). */
		main_touch_round();

		/* The pointer selects, or drags the selected text out (ws035-p093). */
		main_pointer();

		/* The search bar's text and steps find their matches (ws128-p006). */
		main_search(options);

		/* The wheel and Shift+Page Up or Down scroll the view, and a selection past the edge scrolls it (ws035-p114). */
		main_scroll();
		main_edge_scroll(terminal_clock());

		/* A key typed ends the selection (Edit > Select All, or the pointer's range). */
		if (main_window.input_length != 0U && (main_screen->selected || main_screen->range)) {
			main_screen->selected = 0;
			main_screen->range = 0;
			main_screen->changed = 1;
		}

		/* A key typed shows the live screen again, where the shell answers it (ws035-p114), on whole lines. */
		if (main_window.input_length != 0U &&
		    (main_screen->view != 0U ||
		     main_screen->view_offset != 0)) {
			main_screen->view = 0U;
			main_screen->view_offset = 0;
			main_screen->changed = 1;
			main_view_log("key");
		}

		/* The keys typed and the text pasted go to the shell. */
		status = main_write_shell(run->master);
		if (status != 0) {
			run->reason = "shell-exited";
			return 0;
		}

		/* A password prompt in the active shell turns the input method off until it is answered. */
		main_secret_follow(run->master);

		/* The deadline, when one was given, ends the terminal. */
		now = terminal_clock();
		if (options->timeout != 0U && now - started >= (uint64_t)options->timeout * 1000U) {
			run->reason = "timeout";
			return 0;
		}

		/* A new size remakes the swapchain and the grid, and tells the shell. */
		if (main_window.resized) {
			status = main_resize(options, run);
			if (status != 0)
				return -1;
		}

		/* The menus show the terminal's state, and the titlebar its tabs (only a change is sent). */
		main_menu_state(run, &state);
		terminal_menu_refresh(&main_window, &state);
		main_tabs_show();

		/* The input method's text being composed changed: drawn again at the cursor (BUG-155). */
		if (main_window.preedit_changed) {
			main_window.preedit_changed = 0;
			main_screen->changed = 1;
		}

		/* Nothing changed: nothing to draw. */
		if (!main_screen->changed)
			continue;

		/* Draws the grid; a swapchain out of date is remade and drawn again next round. */
		run->result = terminal_renderer_draw(&main_renderer, main_screen, &main_font, &main_window);
		run->operation = main_renderer.operation;
		if (run->result == VK_ERROR_OUT_OF_DATE_KHR) {
			stale++;
			if (stale > MAIN_STALE_LIMIT)
				return -1;
			main_window.resized = 1;
			continue;
		}

		/* Any other failure ends the terminal. */
		if (run->result != VK_SUCCESS)
			return -1;

		/* The grid on the window is up to date. */
		stale = 0U;
		main_screen->changed = 0;

		/* The input method's candidates stay next to the cursor as drawn (only a change is sent). */
		main_text_cursor();
	}
}

/* Remakes the swapchain at the window's new size, fits the grid to it and tells the shell; returns nonzero on failure. */
static int
main_resize(
	const struct main_options *options,
	struct main_run *run)
{
	unsigned index;

	/* The swapchain at the new size. */
	main_window.resized = 0;
	run->result = terminal_renderer_resize(&main_renderer, main_window.width, main_window.height);
	run->operation = main_renderer.operation;
	if (run->result != VK_SUCCESS)
		return -1;

	/* The grid that fits it, for every tab, told to each shell. */
	main_grid(main_renderer.extent.width, main_renderer.extent.height, &run->columns, &run->rows);
	for (index = 0; index < main_tab_count; index++) {
		terminal_screen_resize(main_tabs[index].screen, run->columns, run->rows);
		main_tell_size(main_tabs[index].master, run->columns, run->rows);
	}

	/* The active grid is drawn again. */
	main_screen->changed = 1;

	/* Succeeded: the next frame is drawn at the new size. */
	printf("ZTERM RESIZE run=%s columns=%u rows=%u window=%ux%u\n", options->token, run->columns, run->rows, main_renderer.extent.width, main_renderer.extent.height);
	fflush(stdout);
	return 0;
}

/* Reads a decimal number no larger than a maximum; returns nonzero when it is not one. */
static int
main_number(
	const char *text,
	unsigned maximum,
	unsigned *value)
{
	unsigned long parsed;
	char *end;

	/* A number with nothing after it. */
	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0')
		return -1;

	/* Too large a value is refused. */
	if (parsed > maximum)
		return -1;

	/* Succeeded: the value. */
	*value = (unsigned)parsed;
	return 0;
}

/* Starts the shell (or the command) on a new pseudo-terminal; returns its process, or -1. */
static pid_t
main_spawn(
	const struct main_options *options,
	int *master)
{
	char *arguments[4];
	pid_t child;
	int flags;
	int status;

	/* The pseudo-terminal and the child whose controlling terminal it is. */
	child = forkpty(master, NULL, NULL, NULL);
	if (child < 0)
		return -1;

	/* The child becomes the shell, with the terminal's type in its environment. */
	if (child == 0) {
		(void)setenv("TERM", "xterm", 1);
		arguments[0] = MAIN_SHELL;
		arguments[1] = NULL;
		if (options->command != NULL) {
			arguments[1] = "-c";
			arguments[2] = (char *)options->command;
			arguments[3] = NULL;
		}

		/* Only a failed exec comes back. */
		(void)execv(MAIN_SHELL, arguments);
		_exit(127);
	}

	/* The terminal reads the shell without blocking, a burst at a time. */
	flags = fcntl(*master, F_GETFL);
	if (flags >= 0) {
		status = fcntl(*master, F_SETFL, flags | O_NONBLOCK);
		(void)status;
	}

	/* Succeeded: the shell runs. */
	return child;
}

/* Works out how many cells fit in the window, inside the padding. */
static void
main_grid(
	unsigned width,
	unsigned height,
	unsigned *columns,
	unsigned *rows)
{
	/* The window less the padding on both sides, in whole cells, at least one of each. */
	*columns = 1U;
	*rows = 1U;
	if (width > 2U * TERMINAL_PADDING + main_font.cell_width)
		*columns = (width - 2U * TERMINAL_PADDING) / main_font.cell_width;
	if (height > 2U * TERMINAL_PADDING + main_font.cell_height)
		*rows = (height - 2U * TERMINAL_PADDING) / main_font.cell_height;
	if (*columns > TERMINAL_MAX_COLUMNS)
		*columns = TERMINAL_MAX_COLUMNS;
	if (*rows > TERMINAL_MAX_ROWS)
		*rows = TERMINAL_MAX_ROWS;
}

/* Tells the shell the grid's size (it gets SIGWINCH). */
static void
main_tell_size(
	int master,
	unsigned columns,
	unsigned rows)
{
	struct winsize size;
	int status;

	/* The size in cells; the pixels are not given. */
	memset(&size, 0, sizeof(size));
	size.ws_col = (unsigned short)columns;
	size.ws_row = (unsigned short)rows;
	status = ioctl(master, TIOCSWINSZ, &size);
	(void)status;
}

/* Reads what a shell wrote and puts it on its grid; returns nonzero once the shell has gone. */
static int
main_read_shell(
	int master,
	struct terminal_screen *screen)
{
	unsigned char buffer[8192];
	ssize_t count;
	int rounds;

	/* Reads bursts until nothing is left, at most a few before the grid is drawn again. */
	for (rounds = 0; rounds < 16; rounds++) {
		count = read(master, buffer, sizeof(buffer));
		if (count > 0) {
			terminal_screen_write(screen, buffer, (size_t)count);
			continue;
		}

		/* Nothing more for now. */
		if (count < 0 && (errno == EAGAIN || errno == EINTR))
			return 0;

		/* End of file, or EIO once the last user of the terminal has gone: the shell is gone. */
		return 1;
	}

	/* Succeeded: more may follow, read on the next round. */
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
main_secret_follow(
	int master)
{
	struct termios modes;
	int secret;
	int error;

	/* The active shell's modes; a master that cannot tell them leaves the text input as it is. */
	if (master < 0)
		return;
	error = tcgetattr(master, &modes);
	if (error != 0)
		return;

	/* A secret: no echo, on a whole line. */
	secret = 0;
	if ((modes.c_lflag & ECHO) == 0 && (modes.c_lflag & ICANON) != 0)
		secret = 1;
	if (secret == main_secret)
		return;

	/* The text input off for the secret, on again after it. */
	main_secret = secret;
	if (secret)
		kl_window_text_input(main_window.kui, 0);
	else
		kl_window_text_input(main_window.kui, 1);
	printf("ZTERM IME secret=%d\n", secret);
	fflush(stdout);
}

/* Writes the keys typed to the shell; returns nonzero once the shell has gone. */
static int
main_write_shell(
	int master)
{
	ssize_t count;

	/* Text being pasted goes first, as much as the terminal takes. */
	if (main_paste_written < main_paste_length) {
		count = write(master, main_paste + main_paste_written, main_paste_length - main_paste_written);
		if (count < 0 &&
		    errno != EAGAIN &&
		    errno != EINTR)
			return 1;

		/* What was written is not written again; a full terminal takes the rest later. */
		if (count > 0)
			main_paste_written += (size_t)count;
	}

	/* Nothing typed. */
	if (main_window.input_length == 0U)
		return 0;

	/* The bytes typed, all at once; a full terminal keeps the rest for the next round. */
	count = write(master, main_window.input, main_window.input_length);
	if (count < 0) {
		/* A full pseudo-terminal is tried again later; any other failure means the shell is gone. */
		if (errno == EAGAIN || errno == EINTR)
			return 0;
		return 1;
	}

	/* What was written leaves the buffer. */
	memmove(main_window.input, main_window.input + count, main_window.input_length - (size_t)count);
	main_window.input_length -= (size_t)count;

	/* Succeeded: the keys went to the shell. */
	return 0;
}

/*
 * Carries out the actions the menus chose.  Returns 1 when one closes the
 * terminal, -1 when one failed, 0 otherwise.
 */
static int
main_menu_actions(
	const struct main_options *options,
	struct main_run *run)
{
	uint32_t action;
	int status;

	/* Each action in the order it was chosen. */
	for (;;) {
		action = terminal_menu_take(&main_window);
		if (action == TERMINAL_ACTION_NONE)
			break;

		/* What the action does. */
		status = 0;
		switch (action) {
		case TERMINAL_ACTION_NEW_WINDOW:
			/* Another terminal, in a window of its own. */
			main_new_window(options, run);
			break;
		case TERMINAL_ACTION_CLOSE:
			/* The terminal ends. */
			return 1;
		case TERMINAL_ACTION_NEW_TAB:
			/* Another shell, in a tab of its own. */
			status = main_tab_new(options, run);
			break;
		case TERMINAL_ACTION_CLOSE_TAB:
			/* The active tab closes; the last one's end ends the terminal. */
			status = main_tab_close(options, run, main_active);
			if (status != 0)
				return 1;
			break;
		case TERMINAL_ACTION_COPY:
			/* The selected text into the terminal's clipboard. */
			main_copy();
			break;
		case TERMINAL_ACTION_PASTE:
			/* The clipboard's text to the shell, as if typed. */
			main_start_paste();
			break;
		case TERMINAL_ACTION_SELECT_ALL:
			/* The whole screen is selected until a key is typed. */
			main_screen->selected = 1;
			main_screen->changed = 1;
			break;
		case TERMINAL_ACTION_ZOOM_IN:
			/* The text a step larger, or one of the fixed sizes below. */
			status = main_zoom(options, run, run->pixels + TERMINAL_PIXELS_STEP);
			break;
		case TERMINAL_ACTION_ZOOM_OUT:
			status = main_zoom(options, run, run->pixels - TERMINAL_PIXELS_STEP);
			break;
		case TERMINAL_ACTION_ZOOM_NORMAL:
			status = main_zoom(options, run, options->pixels);
			break;
		case TERMINAL_ACTION_SIZE_SMALL:
			status = main_zoom(options, run, TERMINAL_PIXELS_SMALL);
			break;
		case TERMINAL_ACTION_SIZE_MEDIUM:
			status = main_zoom(options, run, TERMINAL_PIXELS_MEDIUM);
			break;
		case TERMINAL_ACTION_SIZE_LARGE:
			status = main_zoom(options, run, TERMINAL_PIXELS_LARGE);
			break;
		case TERMINAL_ACTION_SIZE_HUGE:
			status = main_zoom(options, run, TERMINAL_PIXELS_HUGE);
			break;
		case TERMINAL_ACTION_FULLSCREEN:
			/* The compositor's configure says whether it happened. */
			if (main_window.fullscreen)
				terminal_window_set_fullscreen(&main_window, 0);
			else
				terminal_window_set_fullscreen(&main_window, 1);
			break;
		case TERMINAL_ACTION_INTERRUPT:
			/* ^C, as if typed. */
			terminal_window_type(&main_window, "\003", 1U);
			break;
		case TERMINAL_ACTION_END_OF_FILE:
			/* ^D, as if typed. */
			terminal_window_type(&main_window, "\004", 1U);
			break;
		case TERMINAL_ACTION_CLEAR:
			/* The cursor home and the screen erased. */
			terminal_screen_write(main_screen, (const unsigned char *)"\033[H\033[2J", 7U);
			break;
		case TERMINAL_ACTION_RESET:
			/* The screen as it started, at its size, with the width setting kept. */
			terminal_screen_init(main_screen, run->columns, run->rows);
			terminal_screen_set_ambiguous_wide(main_screen, run->ambiguous_wide);
			break;
		case TERMINAL_ACTION_AMBIGUOUS_WIDE:
			/* Ambiguous-width characters written from now on take the other width. */
			main_ambiguous_wide(options, run);
			break;
		case TERMINAL_ACTION_FIND:
			/* The search bar opens with the text looked for last (ws128-p006). */
			main_find(0);
			break;
		case TERMINAL_ACTION_FIND_NEXT:
			/* The next older match. */
			main_find(1);
			break;
		case TERMINAL_ACTION_FIND_PREVIOUS:
			/* The next newer match. */
			main_find(-1);
			break;
		case TERMINAL_ACTION_THEME_DARK:
			main_theme(options, run, TERMINAL_THEME_DARK);
			break;
		case TERMINAL_ACTION_THEME_LIGHT:
			main_theme(options, run, TERMINAL_THEME_LIGHT);
			break;
		case TERMINAL_ACTION_THEME_CONTRAST:
			main_theme(options, run, TERMINAL_THEME_CONTRAST);
			break;
		case TERMINAL_ACTION_ABOUT:
			/* A line about the terminal on the screen (there are no dialogs). */
			terminal_screen_write(main_screen, (const unsigned char *)MAIN_ABOUT, sizeof(MAIN_ABOUT) - 1U);
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
main_menu_state(
	const struct main_run *run,
	struct terminal_menu_state *state)
{
	/* Disables menu actions until their initial state is supplied. */
	memset(state, 0, sizeof(*state));

	/* The first menu exists before the first tab has a screen to select from. */
	if (main_screen != NULL) {
		/* A selected cell or range enables actions on the terminal's text. */
		if (main_screen->selected || main_screen->range)
			state->selection = 1;
	}

	/* Clipboard, type size, fullscreen and the width setting do not depend on a tab. */
	state->clipboard = terminal_clipboard_has_text(&main_window);
	state->pixels = run->pixels;
	state->fullscreen = main_window.fullscreen;
	state->ambiguous_wide = run->ambiguous_wide;
	state->theme = run->theme;
	state->can_find_again = 0;
	if (main_window.search_length != 0U)
		state->can_find_again = 1;
}

/*
 * Starts another terminal for a new window, the way this one was run.  It
 * is started through a child that ends at once, so that it is nobody's
 * child to wait for.
 */
static void
main_new_window(
	const struct main_options *options,
	const struct main_run *run)
{
	char token[64];
	char display[128];
	char *arguments[4];
	unsigned index;
	pid_t child;
	pid_t grandchild;
	int status;

	/* The child, which starts the terminal and ends. */
	child = fork();
	if (child < 0)
		return;
	if (child == 0) {
		/* The terminal's process; the child that made it ends (as does a child whose fork failed). */
		grandchild = fork();
		if (grandchild != 0)
			_exit(0);

		/* It keeps nothing of this terminal's: not the shells' pseudo-terminals. */
		(void)run;
		for (index = 0; index < main_tab_count; index++)
			(void)close(main_tabs[index].master);

		/* The same program, its log lines named after this run's. */
		(void)snprintf(token, sizeof(token), "--token=%s-new", options->token);
		arguments[0] = (char *)options->program;
		arguments[1] = token;
		arguments[2] = NULL;

		/* The same display, when this one was given one. */
		if (options->display != NULL) {
			(void)snprintf(display, sizeof(display), "--display=%s", options->display);
			arguments[2] = display;
			arguments[3] = NULL;
		}

		/* Only a failed exec comes back. */
		(void)execv(options->program, arguments);
		_exit(127);
	}

	/* The child ends at once. */
	(void)waitpid(child, &status, 0);
}

/*
 * Draws the text at another size (within TERMINAL_PIXELS_MIN and _MAX):
 * the grid that fits the window is made again and told to the shell.
 * Returns 0, or -1 when the font cannot be drawn at all any more.
 */
static int
main_zoom(
	const struct main_options *options,
	struct main_run *run,
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

	/* The font at the new size; a size it cannot give keeps the old one. */
	error = terminal_font_resize(&main_font, pixels);
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
	error = main_save_settings(run);
	if (error != 0)
		printf("ZTERM SETTINGS save-failed error=%d\n", error);

	/* The grid that fits the window at the new size, for every tab, told to each shell. */
	main_grid(main_renderer.extent.width, main_renderer.extent.height, &run->columns, &run->rows);
	for (index = 0; index < main_tab_count; index++) {
		terminal_screen_resize(main_tabs[index].screen, run->columns, run->rows);
		main_tell_size(main_tabs[index].master, run->columns, run->rows);
	}

	/* The active grid is drawn again. */
	main_screen->changed = 1;

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
main_ambiguous_wide(
	const struct main_options *options,
	struct main_run *run)
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
	for (index = 0U; index < main_tab_count; index++)
		terminal_screen_set_ambiguous_wide(main_tabs[index].screen, run->ambiguous_wide);

	/* The settings file keeps it; a failed write leaves this run changed and is reported. */
	error = main_save_settings(run);

	/* The log line the tests read. */
	printf("ZTERM AMBIGUOUS run=%s wide=%d saved=%d\n", options->token, run->ambiguous_wide, error);
	fflush(stdout);
}

/* Keeps the run's settings for the next run (ws128-p009, ws128-p006); returns 0 or an errno value. */
static int
main_save_settings(
	const struct main_run *run)
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
main_theme(
	const struct main_options *options,
	struct main_run *run,
	unsigned theme)
{
	int error;

	/* The theme the window is drawn in from the next frame. */
	run->theme = theme;
	main_window.theme = theme;
	main_screen->changed = 1;

	/* The settings file keeps it; a failed write leaves this run changed and is reported. */
	error = main_save_settings(run);

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
main_find(
	int step)
{
	/* The bar opens, showing the text looked for last; the main loop finds it again. */
	if (step == 0 || main_window.search_length == 0U) {
		main_window.search_open = 1;
		main_window.search_edited = 1;
		return;
	}

	/* The step, taken with the bar open. */
	main_window.search_open = 1;
	main_window.search_step = step;
}

/*
 * Finds the search bar's text (ws128-p006): a text that changed is looked
 * for back from the bottom of the window, a step goes on from the match
 * shown; a match found is brought into the window and marked.
 */
static void
main_search(
	const struct main_options *options)
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
	if (!main_window.search_edited && main_window.search_step == 0)
		return;

	/* A closed bar marks nothing; the window is drawn without it. */
	if (!main_window.search_open) {
		main_window.search_edited = 0;
		main_window.search_step = 0;
		main_window.search_found = 0;
		main_screen->changed = 1;
		printf("ZTERM SEARCH run=%s closed\n", options->token);
		fflush(stdout);
		return;
	}

	/* The text's characters. */
	count = terminal_search_decode(main_window.search_query, main_window.search_length, query, TERMINAL_SEARCH_LENGTH);

	/* A changed text from below the window's last row back; a step from the match shown, its way. */
	direction = 1;
	from_line = main_screen->scrolled - main_screen->view + main_screen->rows;
	from_column = 0U;
	edited = main_window.search_edited;
	if (!edited && main_window.search_found) {
		direction = main_window.search_step;
		from_line = main_window.search_line;
		from_column = main_window.search_column;
	}

	/* The change and the step are taken. */
	main_window.search_edited = 0;
	main_window.search_step = 0;

	/* The match; none keeps the view where it is. */
	found = terminal_search_find(main_screen, query, count, from_line, from_column, direction, &line, &column, &cells);
	if (found) {
		main_window.search_found = 1;
		main_window.search_line = line;
		main_window.search_column = column;
		main_window.search_cells = cells;
		terminal_search_show(main_screen, line);
	} else if (edited) {
		/* A changed text that is nowhere has no match to show; a step past the last keeps the one shown. */
		main_window.search_found = 0;
	}

	/* The bar and the marks are drawn again. */
	main_screen->changed = 1;

	/* The log line the tests read. */
	printf("ZTERM SEARCH run=%s query=%s found=%d line=%lu column=%u cells=%u view=%u\n",
	       options->token,
	       main_window.search_query,
	       found,
	       main_window.search_line,
	       main_window.search_column,
	       main_window.search_cells,
	       main_screen->view);
	fflush(stdout);
}

/* Copies the selected text to the clipboard (Edit > Copy: the pointer's range, or the whole screen selected). */
static void
main_copy(void)
{
	/* Nothing is copied without a selection. */
	if (!main_screen->selected && !main_screen->range)
		return;

	/* The text, as it is on the screen now. */
	main_clipboard_length = terminal_screen_text(main_screen, main_clipboard, sizeof(main_clipboard));
	printf("ZTERM COPY bytes=%lu\n", (unsigned long)main_clipboard_length);
	fflush(stdout);

	/* It is the selection other clients paste (clipboard.c). */
	terminal_clipboard_set(&main_window, main_clipboard, main_clipboard_length);
}

/*
 * Starts pasting the clipboard into the shell (Edit > Paste): line breaks
 * become carriage returns, as the Enter key sends them.
 */
static void
main_start_paste(void)
{
	size_t index;
	int own;

	/* Another client's selection is received first (the terminal's own copy is already here). */
	own = terminal_clipboard_own(&main_window);
	if (!own)
		main_clipboard_length = terminal_clipboard_receive(&main_window, main_clipboard, sizeof(main_clipboard));

	/* The clipboard's text, line breaks turned into what Enter sends. */
	for (index = 0; index < main_clipboard_length; index++) {
		/* A line break is what Enter sends; every other byte is itself. */
		if (main_clipboard[index] == '\n')
			main_paste[index] = '\r';
		else
			main_paste[index] = main_clipboard[index];
	}

	/* The main loop writes it as the shell takes it. */
	main_paste_length = main_clipboard_length;
	main_paste_written = 0;
	printf("ZTERM PASTE bytes=%lu\n", (unsigned long)main_paste_length);
	fflush(stdout);
}

/*
 * Pastes the primary selection into the shell (a middle click, ws035-p100):
 * line breaks become carriage returns, as the Enter key sends them.
 */
static void
main_primary_paste(void)
{
	size_t length;
	size_t index;

	/* A paste still being written is not interrupted. */
	if (main_paste_written < main_paste_length)
		return;

	/* The primary selection's text (the terminal's own, or another client's). */
	length = terminal_primary_receive(&main_window, main_paste, sizeof(main_paste));

	/* Line breaks are what Enter sends. */
	for (index = 0; index < length; index++) {
		if (main_paste[index] == '\n')
			main_paste[index] = '\r';
	}

	/* The main loop writes it as the shell takes it. */
	main_paste_length = length;
	main_paste_written = 0;
	printf("ZTERM PASTE primary bytes=%lu\n", (unsigned long)length);
	fflush(stdout);
}

/*
 * Opens a new tab with a shell of its own, at the grid's size, and makes it
 * the active one.  Returns 0, or -1 when the shell cannot be started (a
 * full window has no new tab and returns 0).
 */
static int
main_tab_new(
	const struct main_options *options,
	struct main_run *run)
{
	struct main_tab *tab;
	struct terminal_screen *screen;
	pid_t child;
	int master;

	/* A full window has no new tab. */
	if (main_tab_count == TERMINAL_TABS)
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

	/* Its shell, told the grid's size. */
	child = main_spawn(options, &master);
	if (child < 0) {
		free(screen);
		return -1;
	}

	/* The shell learns the size. */
	main_tell_size(master, run->columns, run->rows);

	/* The tab, at the end, with its ID and title. */
	tab = &main_tabs[main_tab_count];
	tab->screen = screen;
	tab->child = child;
	tab->master = master;
	tab->id = main_tab_next;
	main_tab_next++;
	(void)snprintf(tab->title, sizeof(tab->title), "Shell %u", tab->id);
	main_tab_count++;

	/* Succeeded: it is the active one. */
	main_tab_switch(run, main_tab_count - 1U);
	printf("ZTERM TAB new run=%s id=%u count=%u\n", options->token, tab->id, main_tab_count);
	fflush(stdout);
	return 0;
}

/* Makes a tab the active one: its grid is drawn and its shell takes the keys. */
static void
main_tab_switch(
	struct main_run *run,
	unsigned index)
{
	/* Only a tab that is there. */
	if (index >= main_tab_count)
		return;

	/* The active tab's grid and shell. */
	main_active = index;
	main_screen = main_tabs[index].screen;
	run->child = main_tabs[index].child;
	run->master = main_tabs[index].master;

	/* The keys not yet sent belonged to the tab before; its grid is drawn, without the other tab's search. */
	main_window.input_length = 0;
	main_window.search_open = 0;
	main_window.search_found = 0;
	main_screen->changed = 1;
	printf("ZTERM TAB active id=%u\n", main_tabs[index].id);
	fflush(stdout);
}

/*
 * Closes a tab: its shell is hung up on and reaped, its grid freed, and a
 * neighbour becomes active when it was.  Returns 1 when it was the last
 * tab (the terminal ends), 0 otherwise.
 */
static int
main_tab_close(
	const struct main_options *options,
	struct main_run *run,
	unsigned index)
{
	struct main_tab *tab;
	uint32_t id;
	int status;

	/* Only a tab that is there. */
	if (index >= main_tab_count)
		return main_tab_count == 0U;

	/* Its shell, hung up on and reaped, and its grid. */
	tab = &main_tabs[index];
	id = tab->id;
	(void)close(tab->master);
	if (tab->child > 0) {
		(void)kill(tab->child, SIGHUP);
		(void)waitpid(tab->child, &status, 0);
	}

	/* Its grid. */
	free(tab->screen);

	/* The tabs after it move up. */
	main_tab_count--;
	memmove(&main_tabs[index], &main_tabs[index + 1U], (main_tab_count - index) * sizeof(main_tabs[0]));
	printf("ZTERM TAB closed run=%s id=%u count=%u\n", options->token, id, main_tab_count);
	fflush(stdout);

	/* The last tab: the terminal ends (no shell is active). */
	if (main_tab_count == 0U) {
		main_screen = NULL;
		run->child = -1;
		run->master = -1;
		return 1;
	}

	/* The tab now at its place (or the last one) is active when it was; otherwise the active one may have moved up. */
	if (index == main_active) {
		if (index >= main_tab_count)
			index = main_tab_count - 1U;
		main_tab_switch(run, index);
	} else if (index < main_active) {
		main_tab_switch(run, main_active - 1U);
	}

	/* Succeeded: tabs remain. */
	return 0;
}

/* Finds a tab by its ID; -1 when there is none. */
static int
main_tab_find(
	uint32_t id)
{
	unsigned index;

	/* Each tab. */
	for (index = 0; index < main_tab_count; index++) {
		/* The tab with the ID. */
		if (main_tabs[index].id == id)
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
main_tab_requests(
	const struct main_options *options,
	struct main_run *run)
{
	struct terminal_tab_request request;
	int taken;
	int found;
	int status;

	/* Each request in the order it came. */
	for (;;) {
		taken = terminal_tabs_take(&main_window, &request);
		if (taken == 0)
			break;

		/* A new tab. */
		status = 0;
		if (request.kind == TERMINAL_TAB_NEW)
			status = main_tab_new(options, run);
		if (status != 0)
			return -1;

		/* A tab chosen, or closed, that is still there. */
		found = main_tab_find(request.id);
		if (found < 0)
			continue;
		if (request.kind == TERMINAL_TAB_ACTIVATE)
			main_tab_switch(run, (unsigned)found);
		if (request.kind == TERMINAL_TAB_CLOSE) {
			status = main_tab_close(options, run, (unsigned)found);
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
main_tabs_show(void)
{
	struct terminal_tab_view views[TERMINAL_TABS];
	const char *title;
	unsigned index;
	int same;

	/* Each tab's ID and title. */
	for (index = 0; index < main_tab_count; index++) {
		views[index].id = main_tabs[index].id;
		title = main_tabs[index].title;
		if (main_tabs[index].screen->title[0] != '\0')
			title = main_tabs[index].screen->title;
		main_title_copy(views[index].title, sizeof(views[index].title), title);
	}

	/* With the active one. */
	if (main_tab_count == 0U)
		return;
	terminal_tabs_show(&main_window, views, main_tab_count, main_tabs[main_active].id);

	/* The window's title: the active tab's, or the application's name; only a change is sent. */
	title = "Terminal";
	if (main_tabs[main_active].screen->title[0] != '\0')
		title = main_tabs[main_active].screen->title;
	same = strcmp(title, main_window_title);
	if (same == 0)
		return;
	main_title_copy(main_window_title, sizeof(main_window_title), title);
	kl_window_set_title(main_window.kui, main_window_title);
	printf("ZTERM TITLE tab=%u title=%s\n", main_tabs[main_active].id, main_window_title);
	fflush(stdout);
}

/* Copies a title as far as it fits, never cutting a UTF-8 character in two. */
static void
main_title_copy(
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
main_drop_paste(void)
{
	size_t length;

	/* The dropped text (file names as quoted words). */
	length = terminal_clipboard_drop(&main_window, main_paste, sizeof(main_paste));

	/* The main loop writes it as the shell takes it. */
	main_paste_length = length;
	main_paste_written = 0;
}

/* Takes the pointer's events: the left button selects, or drags the selected text out (ws035-p093). */
static void
main_pointer(void)
{
	const struct terminal_pointer_event *event;
	unsigned index;

	/* Each event, oldest first. */
	for (index = 0U; index < main_window.pointer_event_count; index++) {
		event = &main_window.pointer_events[index];
		if (event->kind == TERMINAL_POINTER_MIDDLE)
			main_primary_paste();
		else if (event->kind == TERMINAL_POINTER_PRESS)
			main_pointer_press(event);
		else if (event->kind == TERMINAL_POINTER_MOTION)
			main_pointer_motion(event);
		else
			main_pointer_release();
	}

	/* Succeeded: all taken. */
	main_window.pointer_event_count = 0U;
}

/*
 * A press of the left button: inside the range it may start a drag; with
 * Shift it extends the selection to the cell; otherwise one click starts a
 * selection, two select a word, three a line, and a drag after two or
 * three clicks goes on by whole words or lines.
 */
static void
main_pointer_press(
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
	main_cell(event->x, event->y, &column, &line);
	again = 0;
	if (main_clicks > 0U &&
	    event->time - main_click_time <= MAIN_CLICK_MS &&
	    column == main_click_column &&
	    line == main_click_line)
		again = 1;
	main_click_time = event->time;
	main_click_column = column;
	main_click_line = line;

	/* The pointer is on the grid where it pressed: no edge scrolls yet (ws035-p114). */
	main_pointer_x = event->x;
	main_pointer_y = event->y;
	main_edge = 0;

	/*
	 * Shift and a click extend the selection there from where the last
	 * one started (any earlier press set that place; ws035-p111).
	 */
	extend = 0;
	if ((event->modifiers & TERMINAL_MODIFIER_SHIFT) != 0U && main_clicks > 0U)
		extend = 1;
	if (extend) {
		main_clicks = 1U;
		main_unit = 1U;
		main_unit_moved = 1;
		main_selecting = 1;
		main_range(main_anchor_column, main_anchor_line, column, line);
		return;
	}

	/* A press inside the range, not a repeated click, may drag it out. */
	inside = terminal_screen_in_range(main_screen, column, line);
	if (inside && !again) {
		main_drag_armed = 1;
		main_press_x = event->x;
		main_press_y = event->y;
		main_press_serial = event->serial;
		main_clicks = 1U;
		return;
	}

	/* How many clicks, up to three (a fourth starts again at one). */
	if (again)
		main_clicks = main_clicks % 3U + 1U;
	else
		main_clicks = 1U;
	main_screen->selected = 0;
	main_screen->changed = 1;

	/* The held button selects by the clicks' unit until it is released. */
	main_selecting = 1;
	main_unit = main_clicks;
	main_unit_moved = 0;

	/* One: the selection starts here and follows the pointer. */
	if (main_clicks == 1U) {
		main_screen->range = 0;
		main_anchor_column = column;
		main_anchor_line = line;
		return;
	}

	/* Two or three: the word or the line under the pointer is the range, and a drag grows from it. */
	main_unit_bounds(main_unit, column, line, &from, &to);
	main_unit_from[0] = from;
	main_unit_from[1] = line;
	main_unit_to[0] = to;
	main_unit_to[1] = line;
	main_anchor_column = from;
	main_anchor_line = line;
	main_range(from, line, to, line);

	/* Said, and the primary selection. */
	if (main_clicks == 2U)
		main_selected_log("word");
	else
		main_selected_log("line");
}

/*
 * Gives the first and the last column of the unit a cell is in: the cell
 * itself (1), its word, or the one character that is not a word's (2), or
 * its whole line (3).
 */
static void
main_unit_bounds(
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
		*to = main_screen->columns - 1U;
		return;
	}

	/* A word: its letters to the left. */
	word = main_word_character(column, line);
	while (word && *from > 0U) {
		word = main_word_character(*from - 1U, line);
		if (word)
			(*from)--;
	}

	/* And to its right. */
	word = main_word_character(column, line);
	while (word && *to + 1U < main_screen->columns) {
		word = main_word_character(*to + 1U, line);
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
main_unit_extend(
	unsigned column,
	unsigned long line)
{
	unsigned from;
	unsigned to;
	int before;

	/* The unit under the pointer. */
	main_unit_bounds(main_unit, column, line, &from, &to);

	/* The pointer's unit before the first chosen one runs the range backward, otherwise forward. */
	before = 0;
	if (line < main_unit_from[1])
		before = 1;
	else if (line == main_unit_from[1] && from < main_unit_from[0])
		before = 1;
	if (before) {
		main_range(from, line, main_unit_to[0], main_unit_to[1]);
	} else {
		main_range(main_unit_from[0], main_unit_from[1], to, line);
	}

	/* The range changed since the press. */
	main_unit_moved = 1;
}

/* A motion: it drags the range out once it moves far enough from a press inside it, or extends a selection. */
static void
main_pointer_motion(
	const struct terminal_pointer_event *event)
{
	char text[4096];
	size_t length;
	int32_t dx;
	int32_t dy;
	int32_t top;
	int32_t bottom;
	int edge;

	/* A press inside the range, moved far enough: the range's text is dragged out. */
	if (main_drag_armed) {
		dx = event->x - main_press_x;
		dy = event->y - main_press_y;
		if (dx > -MAIN_DRAG_DISTANCE && dx < MAIN_DRAG_DISTANCE && dy > -MAIN_DRAG_DISTANCE && dy < MAIN_DRAG_DISTANCE)
			return;
		main_drag_armed = 0;
		length = terminal_screen_text(main_screen, text, sizeof(text));
		terminal_clipboard_drag(&main_window, text, length, main_press_serial);
		return;
	}

	/* A selection follows the pointer from where it started. */
	if (!main_selecting)
		return;
	main_pointer_x = event->x;
	main_pointer_y = event->y;

	/*
	 * Past the grid's top the view scrolls back into the scrollback, past
	 * its bottom toward the live screen: a step at once, then one every
	 * MAIN_EDGE_MS while the pointer stays there (ws035-p114).
	 */
	top = (int32_t)TERMINAL_PADDING;
	bottom = top + (int32_t)(main_screen->rows * main_font.cell_height);
	edge = 0;
	if (event->y < top)
		edge = 1;
	else if (event->y >= bottom)
		edge = -1;
	if (edge != 0 && edge != main_edge)
		main_edge_at = terminal_clock();
	main_edge = edge;

	/* The selection runs to the pointer's cell. */
	main_select_to_pointer();
}

/*
 * Extends the held selection to the cell nearest the pointer's last place:
 * by words or lines after a double or triple click (ws035-p111), else a
 * cell at a time.
 */
static void
main_select_to_pointer(void)
{
	unsigned column;
	unsigned long line;

	/* The cell, on the lines the view shows now. */
	main_cell(main_pointer_x, main_pointer_y, &column, &line);

	/* After a double or triple click, by whole words or lines. */
	if (main_unit > 1U) {
		main_unit_extend(column, line);
		return;
	}

	/* A cell at a time; the pressed cell alone selects nothing yet. */
	if (column == main_anchor_column && line == main_anchor_line && !main_screen->range)
		return;
	main_range(main_anchor_column, main_anchor_line, column, line);
}

/*
 * Scrolls the view a step while a held selection's pointer is past the
 * grid's top or bottom, and runs the selection onto the line the edge
 * then shows (ws035-p114).  The farther past the edge, the more lines a
 * step scrolls.
 */
static void
main_edge_scroll(
	uint64_t now)
{
	int32_t beyond;
	unsigned lines;
	int moved;

	/* Only a held selection with the pointer past an edge, once its step is due. */
	if (!main_selecting || main_edge == 0)
		return;
	if (now < main_edge_at)
		return;

	/* How far past the edge, in pixels (at least one). */
	if (main_edge > 0) {
		beyond = (int32_t)TERMINAL_PADDING - main_pointer_y;
	} else {
		beyond = main_pointer_y - (int32_t)TERMINAL_PADDING - (int32_t)(main_screen->rows * main_font.cell_height) + 1;
	}

	/* A pointer that has just come back counts as one pixel past. */
	if (beyond < 1)
		beyond = 1;

	/* A line for each cell's height past the edge, up to a limit. */
	lines = 1U + (unsigned)(beyond - 1) / main_font.cell_height;
	if (lines > MAIN_EDGE_LINES)
		lines = MAIN_EDGE_LINES;

	/* The step, and when the next is due; at the scrollback's end or on the live screen nothing moves. */
	main_edge_at = now + MAIN_EDGE_MS;
	moved = terminal_screen_scroll_view(main_screen, main_edge * (int)lines);
	if (!moved)
		return;

	/* The selection reaches the line now at the edge. */
	main_select_to_pointer();
	main_view_log("edge");
}

/* Scrolls the view as the wheel's notches and Shift+Page Up or Down asked (ws035-p114). */
static void
main_scroll(void)
{
	unsigned page;
	int lines;
	int moved;

	/* Nothing asked, nothing to scroll. */
	if (main_window.scroll_notches == 0 && main_window.scroll_pages == 0)
		return;

	/* A few lines a notch; a page is the grid's rows less one, which stays in sight. */
	page = main_screen->rows - 1U;
	if (page == 0U)
		page = 1U;
	lines = main_window.scroll_notches * MAIN_WHEEL_LINES + main_window.scroll_pages * (int)page;
	main_window.scroll_notches = 0;
	main_window.scroll_pages = 0;

	/* The view moves within the scrollback. */
	moved = terminal_screen_scroll_view(main_screen, lines);
	if (!moved)
		return;

	/* A held selection runs onto the lines now under the pointer. */
	if (main_selecting)
		main_select_to_pointer();

	/* Said, for the tests. */
	main_view_log("scroll");
}

/* A release: a press inside the range that did not drag clears it (a click); a selection ends. */
static void
main_pointer_release(void)
{
	/* The edge stops scrolling with the button's release (ws035-p114). */
	main_edge = 0;

	/* A click inside the range clears it, and a Shift click later extends from there. */
	if (main_drag_armed) {
		main_drag_armed = 0;
		main_screen->range = 0;
		main_screen->changed = 1;
		main_anchor_column = main_click_column;
		main_anchor_line = main_click_line;
		return;
	}

	/* A selection made by the pointer's move. */
	if (!main_selecting)
		return;
	main_selecting = 0;

	/* A word or line selection says so only when the drag grew it (the press said the first one). */
	if (main_unit == 2U) {
		if (main_unit_moved)
			main_selected_log("words");
		return;
	}

	/* The same for lines. */
	if (main_unit == 3U) {
		if (main_unit_moved)
			main_selected_log("lines");
		return;
	}

	/* A cell selection, dragged or extended with Shift. */
	if (main_screen->range)
		main_selected_log("drag");
}

/*
 * Gives the cell under a point of the surface, the nearest one for a point
 * outside the grid: its column, and the line the view shows on its row.
 */
static void
main_cell(
	int32_t x,
	int32_t y,
	unsigned *column,
	unsigned long *line)
{
	unsigned row;

	/* From the grid's padded top left, with the text moved by the view's offset within a line (ws081-p011), a cell's size at a time. */
	x -= (int32_t)TERMINAL_PADDING;
	y -= (int32_t)TERMINAL_PADDING + main_screen->view_offset;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	*column = (unsigned)x / main_font.cell_width;
	row = (unsigned)y / main_font.cell_height;

	/* The nearest cell inside the grid. */
	if (*column >= main_screen->columns)
		*column = main_screen->columns - 1U;
	if (row >= main_screen->rows)
		row = main_screen->rows - 1U;

	/* Succeeded: the line on that row, in the scrollback when the view is back (ws035-p114). */
	*line = terminal_screen_view_line(main_screen, row);
}

/* Selects the cells from one to another, in whichever order they come. */
static void
main_range(
	unsigned from_column,
	unsigned long from_line,
	unsigned to_column,
	unsigned long to_line)
{
	int later;

	/* The first in reading order first. */
	later = 0;
	if (from_line > to_line)
		later = 1;
	else if (from_line == to_line && from_column > to_column)
		later = 1;
	if (later) {
		main_screen->range_from[0] = to_column;
		main_screen->range_from[1] = to_line;
		main_screen->range_to[0] = from_column;
		main_screen->range_to[1] = from_line;
	} else {
		main_screen->range_from[0] = from_column;
		main_screen->range_from[1] = from_line;
		main_screen->range_to[0] = to_column;
		main_screen->range_to[1] = to_line;
	}

	/* Succeeded: drawn anew. */
	main_screen->range = 1;
	main_screen->changed = 1;
}

/* Tells whether a cell's character belongs to a word (not a space nor one of the characters that part words in a shell). */
static int
main_word_character(
	unsigned column,
	unsigned long line)
{
	const struct terminal_cell *cell;
	uint32_t codepoint;
	const char *found;

	/* A line no longer kept has no words (ws035-p114). */
	cell = terminal_screen_line_cell(main_screen, column, line);
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
main_selected_log(
	const char *how)
{
	size_t length;

	/* The range's text, whole (as long as the buffer holds), into what the primary selection sends. */
	length = terminal_screen_text(main_screen, main_primary, sizeof(main_primary));
	printf("ZTERM SELECT how=%s from=%lu,%lu to=%lu,%lu bytes=%lu\n", how, main_screen->range_from[0], main_screen->range_from[1], main_screen->range_to[0], main_screen->range_to[1], (unsigned long)length);
	fflush(stdout);

	/* The selected text is the primary selection (ws035-p100). */
	terminal_primary_set(&main_window, main_primary, length);
}

/*
 * Logs where the view is: how it moved, how many lines back from the live
 * screen it is, how many lines the scrollback keeps and how many have
 * ever scrolled off (ws035-p114).
 */
static void
main_view_log(
	const char *how)
{
	/* One line, for the tests. */
	printf("ZTERM VIEW how=%s back=%u history=%u scrolled=%lu\n", how, main_screen->view, main_screen->history_count, main_screen->scrolled);
	fflush(stdout);
}

/*
 * Runs the fingers for one round (ws081-p011): gives them the screen's
 * view, takes their events, sets the view they moved to, and queues the
 * pointer's presses, releases and motions they made for main_pointer.  A
 * long press's hold (ws081-p014) is a press held on the selection (which a
 * drag then takes out), elsewhere a double click held (the word under the
 * finger, grown by words).
 */
static void
main_touch_round(void)
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
	terminal_touch_layout(&main_touch, main_screen, main_font.cell_height, main_screen->history_count,
			      main_screen->rows * main_font.cell_height, main_screen->view, main_screen->view_offset);
	for (index = 0U; index < main_window.touch_count; index++)
		terminal_touch_event(&main_touch, &main_window.touches[index]);
	main_window.touch_count = 0U;

	/* Time moves on for them; a new view is shown. */
	main_touch_due = terminal_touch_tick(&main_touch, terminal_touch_clock());
	moved = terminal_touch_view(&main_touch, &view, &offset);
	if (moved) {
		main_screen->view = view;
		main_screen->view_offset = offset;
		main_screen->changed = 1;
	}

	/* The pointer's events they made, after the pointer's own. */
	for (;;) {
		taken = terminal_touch_take_pointer(&main_touch, &made);
		if (!taken)
			break;

		/* A hold is a press on the selection, or a double click held elsewhere; the rest are what they say. */
		kinds[0] = made.kind;
		kind_count = 1U;
		if (made.kind == TERMINAL_TOUCH_HOLD) {
			main_cell(made.x, made.y, &column, &line);
			inside = terminal_screen_in_range(main_screen, column, line);
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
			main_touch_queue(kinds[kind], &made);
	}
}

/* Queues a pointer event the fingers made, at their place and time (a full queue drops it). */
static void
main_touch_queue(
	unsigned kind,
	const struct terminal_touch_pointer *made)
{
	struct terminal_pointer_event *event;

	/* Room for it. */
	if (main_window.pointer_event_count >= TERMINAL_POINTER_EVENTS)
		return;

	/* The event, after the others. */
	event = &main_window.pointer_events[main_window.pointer_event_count];
	main_window.pointer_event_count++;
	event->kind = kind;
	event->x = made->x;
	event->y = made->y;
	event->time = made->time;
	event->serial = made->serial;
	event->modifiers = main_window.modifiers;
}

/*
 * Tells the input method where the cursor's cell is on the window (surface
 * pixels), or the search bar's caret while it is open, so that its
 * candidate list opens beside the text being composed (BUG-155,
 * ws090-p022); libkeiland sends only a change.
 */
static void
main_text_cursor(void)
{
	unsigned column;
	int x;
	int y;

	/* The cursor's cell, on the rows the view shows (a view scrolled back moves it down). */
	x = (int)TERMINAL_PADDING + (int)(main_screen->cursor_column * main_font.cell_width);
	y = (int)TERMINAL_PADDING + (int)((main_screen->cursor_row + main_screen->view) * main_font.cell_height) + main_screen->view_offset;

	/* While the search bar is open: its caret's cell on the last row, where the composed text shows (ws090-p022). */
	if (main_window.search_open && main_screen->rows != 0U) {
		column = terminal_search_bar_column(main_window.search_query, main_window.search_length, main_screen->ambiguous_wide);
		x = (int)TERMINAL_PADDING + (int)(column * main_font.cell_width);
		y = (int)TERMINAL_PADDING + (int)((main_screen->rows - 1U) * main_font.cell_height);
	}

	/* The cell's rectangle. */
	kl_window_text_cursor(main_window.kui, x, y, (int)main_font.cell_width, (int)main_font.cell_height);
}
