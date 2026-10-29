/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A window that counts the prime numbers below a limit on a thread of its
 * own (a worker), so that the window keeps answering while it counts.  The
 * worker tells the window how far it got with kl_window_post, and hands the
 * finished count back to the application with kl_app_post.  Stop asks the
 * worker to end early.
 */

#include <keiland/keiland.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

/* Where the desktop's fonts are (/opt/keiland/share/fonts for a Linux build under /opt/keiland). */
#ifndef FONT_DIR
#define FONT_DIR	"/usr/share/fonts"
#endif
#define FONT		FONT_DIR "/keiland.ttf"
#define FALLBACK_FONT	FONT_DIR "/keiland-fallback.ttf"

/* The widgets' ids. */
#define ID_START	1U
#define ID_STOP		2U

/* The program's own codes of its posts: how far a count got (to the window), and a count's end (to the application). */
#define POST_PROGRESS	1U
#define POST_FINISHED	2U

/* The numbers counted, and how many the worker looks at between two reports. */
#define LIMIT		20000000UL
#define STEP		200000UL

/* The buttons' width and the room around them, in pixels. */
#define BUTTON_WIDTH	96
#define MARGIN		24

/*
 * One count: what the worker is given (the application, the window, the
 * limit), what it found (how far it went, the primes below that), and the
 * flag the window raises to stop it.  The application's thread owns the
 * record until the worker starts, and again from the POST_FINISHED that
 * hands it back; only stop is shared in between.
 */
struct count {
	struct kl_app *app;
	struct kl_window *window;
	unsigned long limit;
	unsigned long done;
	unsigned long primes;
	atomic_int stop;
};

/* One report of how far a count got: made by the worker, freed by the one who takes the post. */
struct progress {
	unsigned long done;
	unsigned long primes;
};

/*
 * Everything the program keeps: its connection, window and widgets, the
 * count running (NULL when none) and its thread, what the window shows of
 * it, and what the main loop has to do next.
 */
struct primes {
	struct kl_app *app;
	struct kl_window *window;
	struct kl_ui *ui;
	struct kl_text text;
	struct kl_style style;
	struct kl_canvas canvas;
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	struct count *count;
	thrd_t worker;
	unsigned long done;
	unsigned long found;
	char status[128];
	int dirty;
	int moving;
	int resized;
	int closed;
};

static int primes_resize(struct primes *primes);
static void primes_draw(struct primes *primes, uint64_t now_us);
static int primes_frame(struct primes *primes);
static void primes_event(struct primes *primes, const struct kl_app_event *event);
static void primes_input(struct primes *primes, const struct kl_window_event *input);
static void primes_post(struct primes *primes, uint32_t code, void *data);
static void primes_start(struct primes *primes);
static void primes_finish(struct primes *primes);
static int primes_count(void *argument);
static int primes_is_prime(unsigned long number);

int
main(void)
{
	static struct primes primes;
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	struct kl_app_event event;
	int timeout;
	int status;
	int taken;

	/* Connects to the desktop. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "org.example.Primes";
	primes.app = kl_app_open(&app_options);
	if (primes.app == NULL) {
		fprintf(stderr, "primes: no desktop (%s)\n", strerror(errno));
		return 1;
	}

	/* Opens the window. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Primes";
	window_options.width = 440;
	window_options.height = 240;
	window_options.present = KL_PRESENT_VULKAN;
	primes.window = kl_app_window_create(primes.app, &window_options);
	if (primes.window == NULL) {
		fprintf(stderr, "primes: no window (%s)\n", strerror(errno));
		return 1;
	}

	/* Makes the widgets' record and opens the font. */
	primes.ui = kl_ui_create();
	if (primes.ui == NULL)
		return 1;
	status = kl_text_open(&primes.text, FONT, FALLBACK_FONT);
	if (status != 0) {
		fprintf(stderr, "primes: no font %s (%s)\n", FONT, strerror(status));
		return 1;
	}

	/* Gives the widgets the font. */
	primes.style.text = &primes.text;
	snprintf(primes.status, sizeof(primes.status), "Not counted yet.");

	/* Makes the first frame. */
	status = primes_resize(&primes);
	if (status != 0)
		return 1;

	/* Runs until the window is closed; the worker's posts arrive among the other events. */
	while (!primes.closed) {
		/* Waits for the desktop or a post: not at all while a frame is due, a frame's time while something moves. */
		timeout = -1;
		if (primes.moving)
			timeout = 16;
		if (primes.dirty || primes.resized)
			timeout = 0;
		status = kl_app_dispatch(primes.app, timeout);
		if (status != 0)
			break;

		/* Takes every event queued, in order. */
		for (;;) {
			taken = kl_app_take(primes.app, &event);
			if (!taken)
				break;
			primes_event(&primes, &event);
		}

		/* A new size: a new frame of that size. */
		if (primes.resized) {
			primes.resized = 0;
			status = primes_resize(&primes);
			if (status != 0)
				break;
		}

		/* Something changed or moves: draws and shows a frame. */
		if (primes.dirty || primes.moving) {
			status = primes_frame(&primes);
			if (status != 0)
				break;
		}
	}

	/* Ends a count still running before the application closes: no thread may post after it. */
	primes_finish(&primes);

	/* Frees the widgets' record, the frame and the font, and closes the window and the connection. */
	kl_ui_destroy(primes.ui);
	kl_canvas_release(&primes.canvas);
	free(primes.pixels);
	kl_text_close(&primes.text);
	kl_app_close(primes.app);

	/* Succeeded: the window was closed. */
	return 0;
}

/*
 * Makes the frame at the window's size; nonzero when it cannot.
 */
static int
primes_resize(
	struct primes *primes)
{
	uint32_t *pixels;
	int status;

	/* Sets the presenter to the window's size, which it reports. */
	status = kl_window_present_resize(primes->window, &primes->width, &primes->height);
	if (status != 0)
		return -1;

	/* Allocates the frame. */
	pixels = malloc((size_t)primes->width * primes->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* Puts the canvas on the new pixels in place of the old ones; the widgets draw on it. */
	if (primes->pixels != NULL)
		kl_canvas_release(&primes->canvas);
	free(primes->pixels);
	primes->pixels = pixels;
	status = kl_canvas_init(&primes->canvas, primes->pixels, primes->width, (int)primes->width, (int)primes->height);
	if (status != 0)
		return -1;
	primes->style.canvas = &primes->canvas;

	/* Succeeded: the new frame is drawn next. */
	primes->dirty = 1;
	return 0;
}

/*
 * Draws one frame: the question, how far the count got, what it found,
 * and the Start and Stop buttons.
 */
static void
primes_draw(
	struct primes *primes,
	uint64_t now_us)
{
	const struct kl_theme *theme;
	struct kl_rect whole;
	struct kl_rect bar;
	struct kl_rect start;
	struct kl_rect stop;
	unsigned start_flags;
	unsigned stop_flags;
	char line[64];
	double fraction;
	int baseline;
	int clicked;

	/* Starts the frame. */
	theme = kl_theme_default();
	primes->style.theme = theme;
	kl_ui_begin(primes->ui, now_us);

	/* Paints the ground. */
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)primes->width;
	whole.height = (int)primes->height;
	kl_canvas_gradient(&primes->canvas, &whole, theme->ground_top, theme->ground_bottom);

	/* The question. */
	snprintf(line, sizeof(line), "Primes below %lu", (unsigned long)LIMIT);
	baseline = kl_text_center(theme->text_body, MARGIN, 24);
	(void)kl_text_draw(&primes->text, &primes->canvas, MARGIN, baseline, line, strlen(line), theme->text_body, 1, theme->text);

	/* How far the count got, as a bar, and what it found. */
	bar.x = MARGIN;
	bar.y = MARGIN + 44;
	bar.width = (int)primes->width - 2 * MARGIN;
	bar.height = 6;
	fraction = (double)primes->done / (double)LIMIT;
	kl_progress(&primes->style, &bar, fraction, now_us);
	baseline = kl_text_center(theme->text_body, bar.y + 16, 24);
	(void)kl_text_draw(&primes->text, &primes->canvas, MARGIN, baseline, primes->status, strlen(primes->status), theme->text_body, 0, theme->text_secondary);

	/* Lays out the two buttons in a row at the bottom. */
	start.x = MARGIN;
	start.y = (int)primes->height - MARGIN - theme->control_height;
	start.width = BUTTON_WIDTH;
	start.height = theme->control_height;
	stop = start;
	stop.x = start.x + BUTTON_WIDTH + 12;

	/* Start: a new count on a worker; disabled while one runs. */
	start_flags = KL_BUTTON_PRIMARY;
	if (primes->count != NULL)
		start_flags |= KL_BUTTON_DISABLED;
	clicked = kl_button(primes->ui, &primes->style, ID_START, &start, "Start", start_flags);
	if (clicked)
		primes_start(primes);

	/* Stop: asks the worker to end; its count comes back as usual, cut short. */
	stop_flags = 0U;
	if (primes->count == NULL)
		stop_flags = KL_BUTTON_DISABLED;
	clicked = kl_button(primes->ui, &primes->style, ID_STOP, &stop, "Stop", stop_flags);
	if (clicked && primes->count != NULL)
		atomic_store(&primes->count->stop, 1);

	/* Ends the frame. */
	primes->moving = kl_ui_end(primes->ui, now_us);
	kl_ui_window_text(primes->ui, primes->window);
}

/*
 * Draws and shows a frame; nonzero when the frame cannot be shown.
 */
static int
primes_frame(
	struct primes *primes)
{
	int status;

	/* Draws the frame. */
	primes->dirty = 0;
	primes_draw(primes, kl_clock_us());

	/* Hands it to the compositor. */
	status = kl_window_present(primes->window, primes->pixels, primes->width);
	if (status == EAGAIN) {
		/* The window's size changed under the frame: a new frame of the new size. */
		primes->resized = 1;
	} else if (status != 0) {
		/* Refuses to go on when the frame could not be shown. */
		return -1;
	}

	/* Succeeded: the frame is shown. */
	return 0;
}

/*
 * Acts on one event of the application: the theme, the window's input,
 * and the posts, whether they came to the window or to the application.
 */
static void
primes_event(
	struct primes *primes,
	const struct kl_app_event *event)
{
	/* The appearance changed: the theme's colours are new. */
	if (event->kind == KL_APP_THEME)
		primes->dirty = 1;

	/* A post to the window comes among its input, as an input of its own kind. */
	if (event->kind == KL_APP_WINDOW && event->window == primes->window) {
		if (event->input.kind == KL_WINDOW_POST) {
			primes_post(primes, event->input.code, event->input.data);
			return;
		}

		/* Offers any other input to the widgets. */
		primes_input(primes, &event->input);
	}

	/* A post to the application, or one to a window that closed before it came. */
	if (event->kind == KL_APP_POST)
		primes_post(primes, event->code, event->data);
}

/*
 * Gives one input of the window to the widgets, or acts on it.
 */
static void
primes_input(
	struct primes *primes,
	const struct kl_window_event *input)
{
	int taken;

	/* The window's own inputs: a new size, the close button. */
	if (input->kind == KL_WINDOW_RESIZE) {
		primes->resized = 1;
		return;
	}

	/* The close button ends the program. */
	if (input->kind == KL_WINDOW_CLOSE) {
		primes->closed = 1;
		return;
	}

	/* The rest is the widgets'. */
	taken = kl_ui_window_input(primes->ui, input);
	if (taken)
		primes->dirty = 1;
}

/*
 * Takes one of the worker's posts: a report of progress, which is shown
 * and freed, or the count handed back, which ends it.  The data of each is
 * the program's from now on.
 */
static void
primes_post(
	struct primes *primes,
	uint32_t code,
	void *data)
{
	struct progress *progress;
	struct count *count;
	const char *how;

	/* A report: shown, then freed (a worker's reports come before its count, never after). */
	if (code == POST_PROGRESS) {
		progress = data;
		primes->done = progress->done;
		primes->found = progress->primes;
		snprintf(primes->status, sizeof(primes->status), "%lu primes below %lu so far.", primes->found, primes->done);
		free(progress);
		primes->dirty = 1;
		return;
	}

	/* The count handed back: its thread is ending, so it is joined; the record is the program's again. */
	if (code == POST_FINISHED) {
		count = data;
		(void)thrd_join(primes->worker, NULL);
		primes->done = count->done;
		primes->found = count->primes;
		how = "Done";
		if (count->done < count->limit)
			how = "Stopped";
		snprintf(primes->status, sizeof(primes->status), "%s: %lu primes below %lu.", how, primes->found, primes->done);
		free(count);
		primes->count = NULL;
		primes->dirty = 1;
	}
}

/*
 * Starts a count on a worker thread.
 */
static void
primes_start(
	struct primes *primes)
{
	struct count *count;
	int status;

	/* The count's record, handed to the worker. */
	count = calloc(1, sizeof(*count));
	if (count == NULL) {
		snprintf(primes->status, sizeof(primes->status), "No memory to count.");
		return;
	}

	/* Gives the thread what it works with. */
	count->app = primes->app;
	count->window = primes->window;
	count->limit = LIMIT;
	atomic_init(&count->stop, 0);

	/* The worker: from here until POST_FINISHED, the record is its own but for stop. */
	status = thrd_create(&primes->worker, primes_count, count);
	if (status != thrd_success) {
		free(count);
		snprintf(primes->status, sizeof(primes->status), "No thread to count on.");
		return;
	}

	/* Remembers the running count. */
	primes->count = count;
	primes->done = 0;
	primes->found = 0;
	snprintf(primes->status, sizeof(primes->status), "Counting...");
}

/*
 * Ends a count still running when the program ends: asks the worker to
 * stop, waits for its thread, and takes the posts it left, whose data
 * would be lost with the application otherwise.
 */
static void
primes_finish(
	struct primes *primes)
{
	struct kl_app_event event;
	int status;
	int taken;

	/* Nothing runs. */
	if (primes->count == NULL)
		return;

	/* Stops the worker; it posts its count as it ends, and taking that post joins its thread. */
	atomic_store(&primes->count->stop, 1);

	/* Takes the posts left, without waiting, until its count came back (the last of them). */
	while (primes->count != NULL) {
		status = kl_app_dispatch(primes->app, 0);
		if (status != 0)
			break;
		taken = kl_app_take(primes->app, &event);
		if (!taken)
			break;
		if (event.kind == KL_APP_WINDOW && event.input.kind == KL_WINDOW_POST) {
			primes_post(primes, event.input.code, event.input.data);
		} else if (event.kind == KL_APP_POST) {
			primes_post(primes, event.code, event.data);
		}
	}
}

/*
 * The worker: counts the primes below the limit, reports every STEP
 * numbers, and hands the count back when it is done or asked to stop.  It
 * calls nothing of libkeiland but the two posts, which any thread may.
 */
static int
primes_count(
	void *argument)
{
	struct count *count;
	struct progress *progress;
	unsigned long number;
	int stop;
	int prime;
	int status;

	/* Looks at every number below the limit, one at a time. */
	count = argument;
	for (number = 2; number < count->limit; number++) {
		/* Every STEP numbers: ends when asked, or tells the window how far it got. */
		if (number % STEP == 0) {
			stop = atomic_load(&count->stop);
			if (stop)
				break;
			progress = malloc(sizeof(*progress));
			if (progress != NULL) {
				progress->done = number;
				progress->primes = count->primes;
				status = kl_window_post(count->window, POST_PROGRESS, progress);
				if (status != 0)
					free(progress);
			}
		}

		/* Counts the number when it is a prime. */
		prime = primes_is_prime(number);
		if (prime)
			count->primes++;
	}

	/* Notes how far the count went. */
	count->done = number;

	/* Hands the count back; it fails only without memory, and then the window shows the count as running. */
	status = kl_app_post(count->app, POST_FINISHED, count);
	if (status != 0)
		fprintf(stderr, "primes: the count was lost (%s)\n", strerror(status));

	/* The thread's end: the application's thread joins it. */
	return 0;
}

/*
 * Reports whether a number is a prime, by trying its odd divisors up to
 * its square root.
 */
static int
primes_is_prime(
	unsigned long number)
{
	unsigned long divisor;

	/* 0 and 1 are not primes, 2 is, and no other even number is. */
	if (number < 2UL)
		return 0;
	if (number == 2UL)
		return 1;
	if (number % 2UL == 0)
		return 0;

	/* Tries the odd divisors up to the square root. */
	for (divisor = 3UL; divisor * divisor <= number; divisor += 2UL) {
		if (number % divisor == 0)
			return 0;
	}

	/* No divisor: a prime. */
	return 1;
}
