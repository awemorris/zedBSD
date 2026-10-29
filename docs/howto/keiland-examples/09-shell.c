/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A shell in a window: the program starts sh with a terminal of its own,
 * shows the lines the terminal prints, and types the command written in
 * the field into it.  It is not a terminal emulator: it keeps the text and
 * drops the terminal's control sequences.  When the shell turns the echo
 * off and reads by the line, as it does for a password, the field hides
 * what is typed.
 */

#include <keiland/keiland.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where the desktop's fonts are (/opt/keiland/share/fonts for a Linux build under /opt/keiland). */
#ifndef FONT_DIR
#define FONT_DIR	"/usr/share/fonts"
#endif
#define FONT		FONT_DIR "/keiland.ttf"
#define FALLBACK_FONT	FONT_DIR "/keiland-fallback.ttf"

/* The widget's id. */
#define ID_COMMAND	1U

/* The lines kept of what the terminal printed, and the longest (with the NUL). */
#define LINE_COUNT	64
#define LINE_BYTES	160

/* The layout, in pixels: the room around everything and a line's height. */
#define MARGIN		16
#define LINE_HEIGHT	18

/* The control characters the terminal prints: an escape, a backspace, a tab, a carriage return and a new line. */
#define ESCAPE		0x1b
#define BACKSPACE	0x08
#define TAB		0x09
#define RETURN		0x0d
#define NEWLINE		0x0a

/* Where the program is in a control sequence the terminal prints: outside one, after its escape, within its parameters. */
#define SEQUENCE_NONE		0
#define SEQUENCE_ESCAPE		1
#define SEQUENCE_PARAMETERS	2

/*
 * Everything the program keeps: its connection, window and widgets, the
 * shell while it runs, the terminal's size, the lines it printed (the
 * last one is being written) and where it is in a control sequence, the
 * command being typed into it and how much of it went, the field, whether
 * the field hides what is typed, how the shell ended, and what the main
 * loop has to do next.
 */
struct shell {
	struct kl_app *app;
	struct kl_window *window;
	struct kl_ui *ui;
	struct kl_text text;
	struct kl_style style;
	struct kl_canvas canvas;
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	struct kl_process *process;
	unsigned columns;
	unsigned rows;
	char lines[LINE_COUNT][LINE_BYTES];
	size_t line_count;
	size_t column;
	int sequence;
	char typed[KL_FIELD_MAX + 1U];
	size_t typed_length;
	size_t typed_written;
	struct kl_field command;
	int secret;
	char ended[64];
	int dirty;
	int moving;
	int resized;
	int closed;
};

static int shell_resize(struct shell *shell);
static void shell_size(struct shell *shell);
static void shell_draw(struct shell *shell, uint64_t now_us);
static int shell_frame(struct shell *shell);
static void shell_input(struct shell *shell, const struct kl_window_event *input);
static int shell_start(struct shell *shell);
static void shell_type(struct shell *shell);
static void shell_news(struct shell *shell, unsigned ready);
static void shell_print(struct shell *shell, const char *bytes, size_t size);
static void shell_new_line(struct shell *shell);

int
main(void)
{
	static struct shell shell;
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	struct kl_app_event event;
	int timeout;
	int status;
	int taken;

	/* Connects to the desktop. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "org.example.Shell";
	shell.app = kl_app_open(&app_options);
	if (shell.app == NULL) {
		fprintf(stderr, "shell: no desktop (%s)\n", strerror(errno));
		return 1;
	}

	/* Opens the window. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Shell";
	window_options.width = 560;
	window_options.height = 360;
	window_options.present = KL_PRESENT_VULKAN;
	shell.window = kl_app_window_create(shell.app, &window_options);
	if (shell.window == NULL) {
		fprintf(stderr, "shell: no window (%s)\n", strerror(errno));
		return 1;
	}

	/* Makes the widgets' record, opens the font, and gives the field the keyboard. */
	shell.ui = kl_ui_create();
	if (shell.ui == NULL)
		return 1;
	status = kl_text_open(&shell.text, FONT, FALLBACK_FONT);
	if (status != 0) {
		fprintf(stderr, "shell: no font %s (%s)\n", FONT, strerror(status));
		return 1;
	}

	/* Gives the widgets the font. */
	shell.style.text = &shell.text;
	kl_ui_set_focus(shell.ui, ID_COMMAND, 0U);
	shell.line_count = 1;

	/* Makes the first frame, which tells the terminal's size. */
	status = shell_resize(&shell);
	if (status != 0)
		return 1;

	/* Starts the shell in its terminal. */
	status = shell_start(&shell);
	if (status != 0)
		return 1;

	/* Runs until the window is closed; the shell's news arrives among the other events. */
	while (!shell.closed) {
		/* Waits for the desktop or the shell: not at all while a frame is due, a frame's time while something moves. */
		timeout = -1;
		if (shell.moving)
			timeout = 16;
		if (shell.dirty || shell.resized)
			timeout = 0;
		status = kl_app_dispatch(shell.app, timeout);
		if (status != 0)
			break;

		/* Takes every event queued, in order. */
		for (;;) {
			taken = kl_app_take(shell.app, &event);
			if (!taken)
				break;

			/* The appearance changed: the theme's colours are new. */
			if (event.kind == KL_APP_THEME)
				shell.dirty = 1;

			/* News of the shell: what its terminal printed, room for what is typed, its end. */
			if (event.kind == KL_APP_PROCESS && event.process == shell.process)
				shell_news(&shell, event.ready);

			/* An input of the window. */
			if (event.kind == KL_APP_WINDOW && event.window == shell.window)
				shell_input(&shell, &event.input);
		}

		/* A new size: a new frame of that size, and the terminal told. */
		if (shell.resized) {
			shell.resized = 0;
			status = shell_resize(&shell);
			if (status != 0)
				break;
		}

		/* Something changed or moves: draws and shows a frame. */
		if (shell.dirty || shell.moving) {
			status = shell_frame(&shell);
			if (status != 0)
				break;
		}
	}

	/* Hangs up the shell's terminal, as closing a terminal does, and lets go of it. */
	if (shell.process != NULL) {
		(void)kl_process_kill(shell.process, KL_PROCESS_HANGUP);
		kl_process_close(shell.process);
	}

	/* Frees the widgets' record, the frame and the font, and closes the window and the connection. */
	kl_ui_destroy(shell.ui);
	kl_canvas_release(&shell.canvas);
	free(shell.pixels);
	kl_text_close(&shell.text);
	kl_app_close(shell.app);

	/* Succeeded: the window was closed. */
	return 0;
}

/*
 * Makes the frame at the window's size, and tells the terminal its new
 * size; nonzero when the frame cannot be made.
 */
static int
shell_resize(
	struct shell *shell)
{
	uint32_t *pixels;
	int status;

	/* Sets the presenter to the window's size, which it reports. */
	status = kl_window_present_resize(shell->window, &shell->width, &shell->height);
	if (status != 0)
		return -1;

	/* Allocates the frame. */
	pixels = malloc((size_t)shell->width * shell->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* Puts the canvas on the new pixels in place of the old ones; the widgets draw on it. */
	if (shell->pixels != NULL)
		kl_canvas_release(&shell->canvas);
	free(shell->pixels);
	shell->pixels = pixels;
	status = kl_canvas_init(&shell->canvas, shell->pixels, shell->width, (int)shell->width, (int)shell->height);
	if (status != 0)
		return -1;
	shell->style.canvas = &shell->canvas;

	/* The terminal's size follows the window's. */
	shell_size(shell);

	/* Succeeded: the new frame is drawn next. */
	shell->dirty = 1;
	return 0;
}

/*
 * Works out how many characters and lines the window shows, and tells the
 * shell's terminal when it runs (programs such as ls fit their output to
 * it).
 */
static void
shell_size(
	struct shell *shell)
{
	const struct kl_theme *theme;
	int character;
	int area;
	int status;

	/* A character's width (the font is not of one width: an average one) and the lines above the field. */
	theme = kl_theme_default();
	character = kl_text_width(&shell->text, "0", 1U, theme->text_small, 0);
	if (character < 1)
		character = 1;
	area = (int)shell->height - 2 * MARGIN - theme->control_height - LINE_HEIGHT - 8;
	if (area < LINE_HEIGHT)
		area = LINE_HEIGHT;
	shell->columns = (unsigned)(((int)shell->width - 2 * MARGIN) / character);
	shell->rows = (unsigned)(area / LINE_HEIGHT);

	/* Tells the terminal, which tells the programs in it. */
	if (shell->process != NULL) {
		status = kl_process_resize(shell->process, shell->columns, shell->rows);
		if (status != 0)
			fprintf(stderr, "shell: the terminal keeps its size (%s)\n", strerror(status));
	}
}

/*
 * Draws one frame: the last lines the terminal printed, what the field
 * does, and the field.
 */
static void
shell_draw(
	struct shell *shell,
	uint64_t now_us)
{
	const struct kl_theme *theme;
	struct kl_rect whole;
	struct kl_rect field;
	const char *note;
	size_t first;
	size_t index;
	unsigned changed;
	int baseline;
	int top;

	/* Starts the frame. */
	theme = kl_theme_default();
	shell->style.theme = theme;
	kl_ui_begin(shell->ui, now_us);

	/* Paints the ground. */
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)shell->width;
	whole.height = (int)shell->height;
	kl_canvas_gradient(&shell->canvas, &whole, theme->ground_top, theme->ground_bottom);

	/* The last lines that fit, the one being written at the bottom. */
	first = 0;
	if (shell->line_count > shell->rows)
		first = shell->line_count - shell->rows;
	top = MARGIN;
	for (index = first; index < shell->line_count; index++) {
		baseline = kl_text_center(theme->text_small, top, LINE_HEIGHT);
		(void)kl_text_draw_fit(&shell->text, &shell->canvas, MARGIN, baseline, shell->lines[index], theme->text_small, 0, (int)shell->width - 2 * MARGIN, theme->text);
		top += LINE_HEIGHT;
	}

	/* What the field does now: types a command, types unseen, or nothing once the shell ended. */
	note = "Type a command and press Enter.";
	if (shell->secret)
		note = "The shell turned the echo off: what you type is hidden.";
	if (shell->process == NULL)
		note = shell->ended;
	field.x = MARGIN;
	field.y = (int)shell->height - MARGIN - theme->control_height;
	field.width = (int)shell->width - 2 * MARGIN;
	field.height = theme->control_height;
	baseline = kl_text_center(theme->text_small, field.y - LINE_HEIGHT - 4, LINE_HEIGHT);
	(void)kl_text_draw(&shell->text, &shell->canvas, MARGIN, baseline, note, strlen(note), theme->text_small, 0, theme->text_secondary);

	/* The field: Enter types its line into the terminal. */
	shell->command.secret = shell->secret;
	changed = kl_field(shell->ui, &shell->style, ID_COMMAND, &field, &shell->command, "Command");
	if ((changed & KL_FIELD_SUBMITTED) != 0U)
		shell_type(shell);

	/* Ends the frame. */
	shell->moving = kl_ui_end(shell->ui, now_us);
	kl_ui_window_text(shell->ui, shell->window);
}

/*
 * Draws and shows a frame; nonzero when the frame cannot be shown.
 */
static int
shell_frame(
	struct shell *shell)
{
	int status;

	/* Draws the frame. */
	shell->dirty = 0;
	shell_draw(shell, kl_clock_us());

	/* Hands it to the compositor. */
	status = kl_window_present(shell->window, shell->pixels, shell->width);
	if (status == EAGAIN) {
		/* The window's size changed under the frame: a new frame of the new size. */
		shell->resized = 1;
	} else if (status != 0) {
		/* Refuses to go on when the frame could not be shown. */
		return -1;
	}

	/* Succeeded: the frame is shown. */
	return 0;
}

/*
 * Gives one input of the window to the widgets, or acts on it.
 */
static void
shell_input(
	struct shell *shell,
	const struct kl_window_event *input)
{
	int taken;

	/* The window's own inputs: a new size, the close button. */
	if (input->kind == KL_WINDOW_RESIZE) {
		shell->resized = 1;
		return;
	}

	/* The close button ends the program. */
	if (input->kind == KL_WINDOW_CLOSE) {
		shell->closed = 1;
		return;
	}

	/* The rest is the widgets'. */
	taken = kl_ui_window_input(shell->ui, input);
	if (taken)
		shell->dirty = 1;
}

/*
 * Starts sh in a terminal of its own, of the size the window shows; nonzero
 * when it cannot.
 */
static int
shell_start(
	struct shell *shell)
{
	static const char *const argv[] = { "/bin/sh", NULL };
	static const char *const environment[] = { "TERM=dumb", "PS1=$ ", NULL };
	struct kl_process_options options;
	int status;

	/*
	 * A terminal in place of the three streams: the shell reads what is
	 * typed and prints to it, and it tells the shell it is interactive.  A
	 * dumb terminal asks programs for no control sequences.
	 */
	memset(&options, 0, sizeof(options));
	options.argv = argv;
	options.environment = environment;
	options.streams = KL_PROCESS_TERMINAL;
	options.columns = shell->columns;
	options.rows = shell->rows;
	status = kl_process_start(shell->app, &options, &shell->process);
	if (status != 0) {
		fprintf(stderr, "shell: no shell (%s)\n", strerror(status));
		return -1;
	}

	/* Succeeded: the shell's prompt comes as its first news. */
	return 0;
}

/*
 * Types the field's line into the terminal, with the Enter that ends it,
 * and empties the field.  What the terminal has no room for now is typed
 * when it tells there is room again.
 */
static void
shell_type(
	struct shell *shell)
{
	size_t written;
	int status;

	/* A new line, once the last one went whole; nothing after the shell ended. */
	if (shell->process == NULL)
		return;
	if (shell->typed_written == shell->typed_length) {
		shell->typed_length = (size_t)snprintf(shell->typed, sizeof(shell->typed), "%s\n", shell->command.text);
		shell->typed_written = 0;
		kl_field_set(&shell->command, "");
	}

	/* Types what is left of it; fewer bytes may be taken than given. */
	status = kl_process_write(shell->process, shell->typed + shell->typed_written, shell->typed_length - shell->typed_written, &written);
	if (status != 0)
		return;
	shell->typed_written += written;
}

/*
 * Acts on the shell's news: what its terminal printed, room for what is
 * typed, its end.
 */
static void
shell_news(
	struct shell *shell,
	unsigned ready)
{
	char bytes[4096];
	unsigned mode;
	size_t size;
	unsigned how;
	int status;
	int code;

	/* Room again in the terminal: types the rest of the line. */
	if ((ready & KL_PROCESS_READY_INPUT) != 0U)
		shell_type(shell);

	/* What the terminal printed: all that waits, as bytes, not as lines (a prompt has no line's end). */
	if ((ready & KL_PROCESS_READY_OUTPUT) != 0U) {
		for (;;) {
			size = kl_process_read(shell->process, KL_PROCESS_OUTPUT, bytes, sizeof(bytes));
			if (size == 0)
				break;
			shell_print(shell, bytes, size);
		}

		/* The terminal's modes after what it printed: no echo but by the line is a password asked for. */
		shell->secret = 0;
		status = kl_process_terminal_mode(shell->process, &mode);
		if (status == 0 && (mode & KL_PROCESS_TERMINAL_ECHO) == 0U) {
			if ((mode & KL_PROCESS_TERMINAL_LINES) != 0U)
				shell->secret = 1;
		}

		/* Draws the screen again. */
		shell->dirty = 1;
	}

	/* Its end: told, and the program is done with it. */
	if ((ready & KL_PROCESS_READY_EXIT) != 0U) {
		how = kl_process_exit(shell->process, &code);
		if (how == KL_PROCESS_EXITED) {
			snprintf(shell->ended, sizeof(shell->ended), "The shell ended with the code %d.", code);
		} else if (how == KL_PROCESS_SIGNALED) {
			snprintf(shell->ended, sizeof(shell->ended), "The shell was ended by the signal %d.", code);
		}

		/* Closes the process that ended. */
		kl_process_close(shell->process);
		shell->process = NULL;
		shell->secret = 0;
		shell->dirty = 1;
	}
}

/*
 * Puts what the terminal printed on the lines: the text as it is, a new
 * line at its end, a backspace back one place, a tab to the next eighth
 * place; carriage returns, other control characters and the control
 * sequences are dropped.
 */
static void
shell_print(
	struct shell *shell,
	const char *bytes,
	size_t size)
{
	char *line;
	size_t index;
	unsigned char byte;

	/* Takes the bytes one at a time. */
	for (index = 0; index < size; index++) {
		byte = (unsigned char)bytes[index];
		line = shell->lines[shell->line_count - 1U];

		/* Within a control sequence: an escape and one character, or an escape, '[' and its parameters up to a final letter. */
		if (shell->sequence == SEQUENCE_ESCAPE) {
			shell->sequence = SEQUENCE_NONE;
			if (byte == '[')
				shell->sequence = SEQUENCE_PARAMETERS;
			continue;
		}

		/* Inside the parameters, every byte is dropped up to the final one. */
		if (shell->sequence == SEQUENCE_PARAMETERS) {
			if (byte >= 0x40 && byte <= 0x7e)
				shell->sequence = SEQUENCE_NONE;
			continue;
		}

		/* The control characters this program follows; the others are dropped. */
		if (byte == ESCAPE) {
			shell->sequence = SEQUENCE_ESCAPE;
			continue;
		}

		/* A new line starts a new row. */
		if (byte == NEWLINE) {
			shell_new_line(shell);
			continue;
		}

		/* A backspace takes the last character back. */
		if (byte == BACKSPACE) {
			if (shell->column > 0) {
				shell->column--;
				line[shell->column] = '\0';
			}

			continue;
		}

		/* A tab fills up to the next stop. */
		if (byte == TAB) {
			while (shell->column % 8U != 7U && shell->column < LINE_BYTES - 2U) {
				line[shell->column] = ' ';
				shell->column++;
			}

			/* A tab then counts as a space. */
			byte = ' ';
		}

		/* Any other control character is dropped. */
		if (byte < 0x20 || byte == 0x7f)
			continue;

		/* A character's byte at the end of the line (a line longer than kept is cut). */
		if (shell->column < LINE_BYTES - 1U) {
			line[shell->column] = (char)byte;
			shell->column++;
			line[shell->column] = '\0';
		}
	}
}

/*
 * Ends the line being written and starts the next, dropping the oldest
 * line when all are used.
 */
static void
shell_new_line(
	struct shell *shell)
{
	/* All lines used: the oldest goes. */
	if (shell->line_count == LINE_COUNT) {
		memmove(shell->lines[0], shell->lines[1], sizeof(shell->lines[0]) * (LINE_COUNT - 1U));
		shell->line_count--;
	}

	/* The next line, empty. */
	shell->lines[shell->line_count][0] = '\0';
	shell->line_count++;
	shell->column = 0;
}
