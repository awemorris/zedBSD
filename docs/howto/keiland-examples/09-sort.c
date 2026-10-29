/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A window that sorts a list of words with the system's sort program: it
 * starts sort as a process, writes the words to its standard input, reads
 * the sorted lines from its standard output, and shows how it ended.
 * Reverse runs sort -r.  Bad Option gives sort an option it does not know,
 * to show what it writes to its standard error and the code it ends with.
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

/* The widgets' ids. */
#define ID_SORT		1U
#define ID_REVERSE	2U
#define ID_BAD		3U

/* The lines kept of the output and their longest length (with the NUL). */
#define LINE_COUNT	10
#define LINE_BYTES	64

/* The layout, in pixels: the room around everything, a line's height, the second column, and the button's width. */
#define MARGIN		24
#define LINE_HEIGHT	20
#define COLUMN		220
#define BUTTON_WIDTH	96

/* The words sorted, in the order they are written to sort. */
static const char *const word_table[] = {
	"pear", "apple", "fig", "cherry", "banana", "date", "elderberry", "grape"
};
#define WORD_COUNT	(sizeof(word_table) / sizeof(word_table[0]))

/*
 * Everything the program keeps: its connection, window and widgets, the
 * process while it runs, the input written to it and how much of it went,
 * the lines it wrote back (and whether the rest of a long one is being
 * skipped), its last error line, how it ended, the switches, and what the
 * main loop has to do next.
 */
struct sorter {
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
	char input[512];
	size_t input_length;
	size_t written;
	char lines[LINE_COUNT][LINE_BYTES];
	size_t line_count;
	int skipping;
	char error[LINE_BYTES];
	char ended[96];
	int reverse;
	int bad;
	int dirty;
	int moving;
	int resized;
	int closed;
};

static int sorter_resize(struct sorter *sorter);
static void sorter_draw(struct sorter *sorter, uint64_t now_us);
static int sorter_frame(struct sorter *sorter);
static void sorter_input(struct sorter *sorter, const struct kl_window_event *input);
static void sorter_start(struct sorter *sorter);
static void sorter_write(struct sorter *sorter);
static void sorter_news(struct sorter *sorter, unsigned ready);
static void sorter_read(struct sorter *sorter);
static void sorter_end(struct sorter *sorter);

int
main(void)
{
	static struct sorter sorter;
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	struct kl_app_event event;
	size_t index;
	int timeout;
	int status;
	int taken;

	/* Connects to the desktop. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "org.example.Sort";
	sorter.app = kl_app_open(&app_options);
	if (sorter.app == NULL) {
		fprintf(stderr, "sort: no desktop (%s)\n", strerror(errno));
		return 1;
	}

	/* Opens the window. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Sort";
	window_options.width = 440;
	window_options.height = 300;
	window_options.present = KL_PRESENT_VULKAN;
	sorter.window = kl_app_window_create(sorter.app, &window_options);
	if (sorter.window == NULL) {
		fprintf(stderr, "sort: no window (%s)\n", strerror(errno));
		return 1;
	}

	/* Makes the widgets' record and opens the font. */
	sorter.ui = kl_ui_create();
	if (sorter.ui == NULL)
		return 1;
	status = kl_text_open(&sorter.text, FONT, FALLBACK_FONT);
	if (status != 0) {
		fprintf(stderr, "sort: no font %s (%s)\n", FONT, strerror(status));
		return 1;
	}

	/* Gives the widgets the font. */
	sorter.style.text = &sorter.text;

	/* The input sort reads: the words, one to a line. */
	for (index = 0; index < WORD_COUNT; index++) {
		status = snprintf(sorter.input + sorter.input_length, sizeof(sorter.input) - sorter.input_length, "%s\n", word_table[index]);
		sorter.input_length += (size_t)status;
	}

	/* Makes the first frame. */
	status = sorter_resize(&sorter);
	if (status != 0)
		return 1;

	/* Runs until the window is closed; the process's news arrives among the other events. */
	while (!sorter.closed) {
		/* Waits for the desktop or the process: not at all while a frame is due, a frame's time while something moves. */
		timeout = -1;
		if (sorter.moving)
			timeout = 16;
		if (sorter.dirty || sorter.resized)
			timeout = 0;
		status = kl_app_dispatch(sorter.app, timeout);
		if (status != 0)
			break;

		/* Takes every event queued, in order. */
		for (;;) {
			taken = kl_app_take(sorter.app, &event);
			if (!taken)
				break;

			/* The appearance changed: the theme's colours are new. */
			if (event.kind == KL_APP_THEME)
				sorter.dirty = 1;

			/* News of the process: output to read, room for input, its end. */
			if (event.kind == KL_APP_PROCESS && event.process == sorter.process)
				sorter_news(&sorter, event.ready);

			/* An input of the window. */
			if (event.kind == KL_APP_WINDOW && event.window == sorter.window)
				sorter_input(&sorter, &event.input);
		}

		/* A new size: a new frame of that size. */
		if (sorter.resized) {
			sorter.resized = 0;
			status = sorter_resize(&sorter);
			if (status != 0)
				break;
		}

		/* Something changed or moves: draws and shows a frame. */
		if (sorter.dirty || sorter.moving) {
			status = sorter_frame(&sorter);
			if (status != 0)
				break;
		}
	}

	/* Lets go of a process still running: the library reaps it when it ends, and it is not ended here. */
	if (sorter.process != NULL)
		kl_process_close(sorter.process);

	/* Frees the widgets' record, the frame and the font, and closes the window and the connection. */
	kl_ui_destroy(sorter.ui);
	kl_canvas_release(&sorter.canvas);
	free(sorter.pixels);
	kl_text_close(&sorter.text);
	kl_app_close(sorter.app);

	/* Succeeded: the window was closed. */
	return 0;
}

/*
 * Makes the frame at the window's size; nonzero when it cannot.
 */
static int
sorter_resize(
	struct sorter *sorter)
{
	uint32_t *pixels;
	int status;

	/* Sets the presenter to the window's size, which it reports. */
	status = kl_window_present_resize(sorter->window, &sorter->width, &sorter->height);
	if (status != 0)
		return -1;

	/* Allocates the frame. */
	pixels = malloc((size_t)sorter->width * sorter->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* Puts the canvas on the new pixels in place of the old ones; the widgets draw on it. */
	if (sorter->pixels != NULL)
		kl_canvas_release(&sorter->canvas);
	free(sorter->pixels);
	sorter->pixels = pixels;
	status = kl_canvas_init(&sorter->canvas, sorter->pixels, sorter->width, (int)sorter->width, (int)sorter->height);
	if (status != 0)
		return -1;
	sorter->style.canvas = &sorter->canvas;

	/* Succeeded: the new frame is drawn next. */
	sorter->dirty = 1;
	return 0;
}

/*
 * Draws one frame: the words, the lines sort wrote back, how it ended,
 * and the Sort button with its two switches.
 */
static void
sorter_draw(
	struct sorter *sorter,
	uint64_t now_us)
{
	const struct kl_theme *theme;
	struct kl_rect whole;
	struct kl_rect button;
	unsigned sort_flags;
	size_t index;
	int baseline;
	int clicked;
	int x;

	/* Starts the frame. */
	theme = kl_theme_default();
	sorter->style.theme = theme;
	kl_ui_begin(sorter->ui, now_us);

	/* Paints the ground. */
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)sorter->width;
	whole.height = (int)sorter->height;
	kl_canvas_gradient(&sorter->canvas, &whole, theme->ground_top, theme->ground_bottom);

	/* The columns' headings. */
	baseline = kl_text_center(theme->text_body, MARGIN, LINE_HEIGHT);
	(void)kl_text_draw(&sorter->text, &sorter->canvas, MARGIN, baseline, "Words", 5U, theme->text_body, 1, theme->text);
	(void)kl_text_draw(&sorter->text, &sorter->canvas, COLUMN, baseline, "Sorted", 6U, theme->text_body, 1, theme->text);

	/* The words as they are written to sort. */
	for (index = 0; index < WORD_COUNT; index++) {
		baseline = kl_text_center(theme->text_body, MARGIN + (int)(index + 1U) * LINE_HEIGHT, LINE_HEIGHT);
		(void)kl_text_draw(&sorter->text, &sorter->canvas, MARGIN, baseline, word_table[index], strlen(word_table[index]), theme->text_body, 0, theme->text_secondary);
	}

	/* The lines sort wrote back, then its error line in the danger colour. */
	for (index = 0; index < sorter->line_count; index++) {
		baseline = kl_text_center(theme->text_body, MARGIN + (int)(index + 1U) * LINE_HEIGHT, LINE_HEIGHT);
		(void)kl_text_draw(&sorter->text, &sorter->canvas, COLUMN, baseline, sorter->lines[index], strlen(sorter->lines[index]), theme->text_body, 0, theme->text);
	}

	/* The error line, if sort wrote one. */
	if (sorter->error[0] != '\0') {
		baseline = kl_text_center(theme->text_small, MARGIN + (int)(sorter->line_count + 1U) * LINE_HEIGHT, LINE_HEIGHT);
		(void)kl_text_draw_fit(&sorter->text, &sorter->canvas, COLUMN, baseline, sorter->error, theme->text_small, 0, (int)sorter->width - COLUMN - MARGIN, theme->danger);
	}

	/* How it ended. */
	baseline = kl_text_center(theme->text_small, (int)sorter->height - MARGIN - theme->control_height - 28, LINE_HEIGHT);
	(void)kl_text_draw(&sorter->text, &sorter->canvas, MARGIN, baseline, sorter->ended, strlen(sorter->ended), theme->text_small, 0, theme->text_secondary);

	/* Sort: starts sort; disabled while it runs. */
	button.x = MARGIN;
	button.y = (int)sorter->height - MARGIN - theme->control_height;
	button.width = BUTTON_WIDTH;
	button.height = theme->control_height;
	sort_flags = KL_BUTTON_PRIMARY;
	if (sorter->process != NULL)
		sort_flags |= KL_BUTTON_DISABLED;
	clicked = kl_button(sorter->ui, &sorter->style, ID_SORT, &button, "Sort", sort_flags);
	if (clicked)
		sorter_start(sorter);

	/* The switches: Reverse, and Bad Option. */
	x = button.x + BUTTON_WIDTH + 16;
	baseline = kl_text_center(theme->text_body, button.y, theme->control_height);
	(void)kl_switch(sorter->ui, &sorter->style, ID_REVERSE, x, button.y + (theme->control_height - theme->switch_height) / 2, &sorter->reverse, 0U);
	(void)kl_text_draw(&sorter->text, &sorter->canvas, x + theme->switch_width + 8, baseline, "Reverse", 7U, theme->text_body, 0, theme->text);
	x += theme->switch_width + 96;
	(void)kl_switch(sorter->ui, &sorter->style, ID_BAD, x, button.y + (theme->control_height - theme->switch_height) / 2, &sorter->bad, 0U);
	(void)kl_text_draw(&sorter->text, &sorter->canvas, x + theme->switch_width + 8, baseline, "Bad Option", 10U, theme->text_body, 0, theme->text);

	/* Ends the frame. */
	sorter->moving = kl_ui_end(sorter->ui, now_us);
	kl_ui_window_text(sorter->ui, sorter->window);
}

/*
 * Draws and shows a frame; nonzero when the frame cannot be shown.
 */
static int
sorter_frame(
	struct sorter *sorter)
{
	int status;

	/* Draws the frame. */
	sorter->dirty = 0;
	sorter_draw(sorter, kl_clock_us());

	/* Hands it to the compositor. */
	status = kl_window_present(sorter->window, sorter->pixels, sorter->width);
	if (status == EAGAIN) {
		/* The window's size changed under the frame: a new frame of the new size. */
		sorter->resized = 1;
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
sorter_input(
	struct sorter *sorter,
	const struct kl_window_event *input)
{
	int taken;

	/* The window's own inputs: a new size, the close button. */
	if (input->kind == KL_WINDOW_RESIZE) {
		sorter->resized = 1;
		return;
	}

	/* The close button ends the program. */
	if (input->kind == KL_WINDOW_CLOSE) {
		sorter->closed = 1;
		return;
	}

	/* The rest is the widgets'. */
	taken = kl_ui_window_input(sorter->ui, input);
	if (taken)
		sorter->dirty = 1;
}

/*
 * Starts sort with its input and output (and its error stream) the
 * program's, and writes it the words.
 */
static void
sorter_start(
	struct sorter *sorter)
{
	struct kl_process_options options;
	const char *argv[3];
	int status;

	/* The command: sort (looked for in PATH), with -r for Reverse or an option it refuses for Bad Option. */
	argv[0] = "sort";
	argv[1] = NULL;
	argv[2] = NULL;
	if (sorter->reverse)
		argv[1] = "-r";
	if (sorter->bad)
		argv[1] = "--no-such-option";

	/* The streams the program uses: it writes the input, and reads the output and the errors. */
	memset(&options, 0, sizeof(options));
	options.argv = argv;
	options.streams = KL_PROCESS_INPUT | KL_PROCESS_OUTPUT | KL_PROCESS_ERROR;
	options.data = sorter;
	status = kl_process_start(sorter->app, &options, &sorter->process);
	if (status != 0) {
		snprintf(sorter->ended, sizeof(sorter->ended), "sort could not start (%s).", strerror(status));
		sorter->process = NULL;
		sorter->dirty = 1;
		return;
	}

	/* A new run: nothing read back yet. */
	sorter->line_count = 0;
	sorter->skipping = 0;
	sorter->error[0] = '\0';
	snprintf(sorter->ended, sizeof(sorter->ended), "sort is running.");
	sorter->written = 0;
	sorter->dirty = 1;

	/* Writes it the words. */
	sorter_write(sorter);
}

/*
 * Writes the rest of the input, and closes it once all is written: sort
 * sorts when its input ends.  What does not fit now is written when the
 * process tells there is room again.
 */
static void
sorter_write(
	struct sorter *sorter)
{
	size_t written;
	int status;

	/* Writes what is left; fewer bytes may be taken than given. */
	status = kl_process_write(sorter->process, sorter->input + sorter->written, sorter->input_length - sorter->written, &written);
	if (status != 0) {
		/* sort ended before it read all (EPIPE): its end tells why. */
		return;
	}

	/* Counts the bytes sort took. */
	sorter->written += written;

	/* All written: closes the input, which sort reads as the end of the words. */
	if (sorter->written == sorter->input_length)
		(void)kl_process_close_input(sorter->process);
}

/*
 * Acts on one piece of news of the process: room for the input, output
 * and errors to read, its end.  An event may tell several; the output it
 * tells is read before its end.
 */
static void
sorter_news(
	struct sorter *sorter,
	unsigned ready)
{
	/* Room again for the input: writes on. */
	if ((ready & KL_PROCESS_READY_INPUT) != 0U)
		sorter_write(sorter);

	/* Lines on its output or its error stream. */
	if ((ready & (KL_PROCESS_READY_OUTPUT | KL_PROCESS_READY_ERROR)) != 0U)
		sorter_read(sorter);

	/* Its end, after the output read. */
	if ((ready & KL_PROCESS_READY_EXIT) != 0U)
		sorter_end(sorter);
}

/*
 * Reads every whole line waiting on the output and the error stream.  A
 * line too long for a line of the window is kept cut, and the rest of it
 * is skipped.
 */
static void
sorter_read(
	struct sorter *sorter)
{
	char line[LINE_BYTES];
	int found;

	/* Takes the output's lines until no whole line waits. */
	for (;;) {
		found = kl_process_read_line(sorter->process, KL_PROCESS_OUTPUT, line, sizeof(line));
		if (found == 0)
			break;

		/* The rest of a long line, which ends with this piece: skipped. */
		if (sorter->skipping) {
			if (found == KL_PROCESS_LINE)
				sorter->skipping = 0;
			continue;
		}

		/* A line, or the start of a long one (its rest is skipped). */
		if (found == KL_PROCESS_LINE_CUT)
			sorter->skipping = 1;
		if (sorter->line_count < LINE_COUNT) {
			snprintf(sorter->lines[sorter->line_count], LINE_BYTES, "%s", line);
			sorter->line_count++;
		}

		/* Draws the lines again. */
		sorter->dirty = 1;
	}

	/* Keeps the last line of the error stream (a long one cut). */
	for (;;) {
		found = kl_process_read_line(sorter->process, KL_PROCESS_ERROR, line, sizeof(line));
		if (found == 0)
			break;
		snprintf(sorter->error, sizeof(sorter->error), "%s", line);
		sorter->dirty = 1;
	}
}

/*
 * Tells how the process ended, and lets go of it.
 */
static void
sorter_end(
	struct sorter *sorter)
{
	unsigned how;
	int code;

	/* With an exit code (0 when it sorted), or by a signal. */
	how = kl_process_exit(sorter->process, &code);
	if (how == KL_PROCESS_EXITED) {
		snprintf(sorter->ended, sizeof(sorter->ended), "sort ended with the code %d.", code);
	} else if (how == KL_PROCESS_SIGNALED) {
		snprintf(sorter->ended, sizeof(sorter->ended), "sort was ended by the signal %d.", code);
	}

	/* Done with it: no more of its news comes. */
	kl_process_close(sorter->process);
	sorter->process = NULL;
	sorter->dirty = 1;
}
