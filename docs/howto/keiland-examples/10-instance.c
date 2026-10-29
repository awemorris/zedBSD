/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A program that runs once per user: a second start hands its request
 * (its first argument) to the copy that runs, and ends.  The copy that
 * runs lists the requests it was handed and brings its window to the
 * front for each.  Start Another starts the program again, apart from
 * this one (a launch), with a request of its own -- as a file manager
 * starts a program on a file.
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

/* The name the copies of the program find each other by. */
#define INSTANCE_NAME	"example-requests"

/* The widget's id. */
#define ID_ANOTHER	1U

/* The requests listed. */
#define REQUEST_COUNT	8

/* The layout, in pixels: the room around everything, a line's height, and the button's width. */
#define MARGIN		24
#define LINE_HEIGHT	22
#define BUTTON_WIDTH	140

/*
 * Everything the program keeps: its connection, window and widgets, the
 * one copy it is (NULL when it runs on its own), the path it was started
 * by, the requests it was handed, how many programs it started, what the
 * last start said, and what the main loop has to do next.
 */
struct requests {
	struct kl_app *app;
	struct kl_window *window;
	struct kl_ui *ui;
	struct kl_text text;
	struct kl_style style;
	struct kl_canvas canvas;
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	struct kl_instance *instance;
	const char *program;
	char list[REQUEST_COUNT][KL_INSTANCE_REQUEST_MAX];
	size_t count;
	unsigned started;
	char status[128];
	int dirty;
	int moving;
	int resized;
	int closed;
};

static int requests_resize(struct requests *requests);
static void requests_draw(struct requests *requests, uint64_t now_us);
static int requests_frame(struct requests *requests);
static void requests_input(struct requests *requests, const struct kl_window_event *input);
static void requests_add(struct requests *requests, const char *request);
static void requests_take(struct requests *requests);
static void requests_launch(struct requests *requests);

int
main(
	int argc,
	char *argv[])
{
	static struct requests requests;
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	struct kl_app_event event;
	const char *request;
	int timeout;
	int status;
	int taken;

	/* The request of this start: the first argument, or none. */
	request = "";
	if (argc > 1)
		request = argv[1];
	requests.program = argv[0];

	/* Becomes the one copy, or hands the request to the copy that runs: before connecting to the desktop. */
	status = kl_instance_open(INSTANCE_NAME, request, &requests.instance);
	if (status != 0) {
		/* No way to find the other copies: this one runs on its own. */
		fprintf(stderr, "requests: running on its own (%s)\n", strerror(status));
		requests.instance = NULL;
	} else if (requests.instance == NULL) {
		/* Handed over: the copy that runs carries the request out. */
		return 0;
	}

	/* Connects to the desktop. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "org.example.Requests";
	requests.app = kl_app_open(&app_options);
	if (requests.app == NULL) {
		fprintf(stderr, "requests: no desktop (%s)\n", strerror(errno));
		return 1;
	}

	/* The later starts' requests come as the application's events from now on. */
	if (requests.instance != NULL) {
		status = kl_instance_bind(requests.instance, requests.app);
		if (status != 0)
			fprintf(stderr, "requests: later starts are not heard (%s)\n", strerror(status));
	}

	/* Opens the window. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Requests";
	window_options.width = 440;
	window_options.height = 300;
	window_options.present = KL_PRESENT_VULKAN;
	requests.window = kl_app_window_create(requests.app, &window_options);
	if (requests.window == NULL) {
		fprintf(stderr, "requests: no window (%s)\n", strerror(errno));
		return 1;
	}

	/* Makes the widgets' record and opens the font. */
	requests.ui = kl_ui_create();
	if (requests.ui == NULL)
		return 1;
	status = kl_text_open(&requests.text, FONT, FALLBACK_FONT);
	if (status != 0) {
		fprintf(stderr, "requests: no font %s (%s)\n", FONT, strerror(status));
		return 1;
	}

	/* Gives the widgets the font. */
	requests.style.text = &requests.text;

	/* The first request is this start's own. */
	if (request[0] != '\0')
		requests_add(&requests, request);

	/* Makes the first frame. */
	status = requests_resize(&requests);
	if (status != 0)
		return 1;

	/* Runs until the window is closed. */
	while (!requests.closed) {
		/* Waits for the desktop or a later start: not at all while a frame is due, a frame's time while something moves. */
		timeout = -1;
		if (requests.moving)
			timeout = 16;
		if (requests.dirty || requests.resized)
			timeout = 0;
		status = kl_app_dispatch(requests.app, timeout);
		if (status != 0)
			break;

		/* Takes every event queued, in order. */
		for (;;) {
			taken = kl_app_take(requests.app, &event);
			if (!taken)
				break;

			/* The appearance changed: the theme's colours are new. */
			if (event.kind == KL_APP_THEME)
				requests.dirty = 1;

			/* Later starts handed requests over. */
			if (event.kind == KL_APP_INSTANCE && event.instance == requests.instance)
				requests_take(&requests);

			/* An input of the window. */
			if (event.kind == KL_APP_WINDOW && event.window == requests.window)
				requests_input(&requests, &event.input);
		}

		/* A new size: a new frame of that size. */
		if (requests.resized) {
			requests.resized = 0;
			status = requests_resize(&requests);
			if (status != 0)
				break;
		}

		/* Something changed or moves: draws and shows a frame. */
		if (requests.dirty || requests.moving) {
			status = requests_frame(&requests);
			if (status != 0)
				break;
		}
	}

	/* Stops being the one copy: a later start runs as the first. */
	if (requests.instance != NULL)
		kl_instance_close(requests.instance);

	/* Frees the widgets' record, the frame and the font, and closes the window and the connection. */
	kl_ui_destroy(requests.ui);
	kl_canvas_release(&requests.canvas);
	free(requests.pixels);
	kl_text_close(&requests.text);
	kl_app_close(requests.app);

	/* Succeeded: the window was closed. */
	return 0;
}

/*
 * Makes the frame at the window's size; nonzero when it cannot.
 */
static int
requests_resize(
	struct requests *requests)
{
	uint32_t *pixels;
	int status;

	/* Sets the presenter to the window's size, which it reports. */
	status = kl_window_present_resize(requests->window, &requests->width, &requests->height);
	if (status != 0)
		return -1;

	/* Allocates the frame. */
	pixels = malloc((size_t)requests->width * requests->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* Puts the canvas on the new pixels in place of the old ones; the widgets draw on it. */
	if (requests->pixels != NULL)
		kl_canvas_release(&requests->canvas);
	free(requests->pixels);
	requests->pixels = pixels;
	status = kl_canvas_init(&requests->canvas, requests->pixels, requests->width, (int)requests->width, (int)requests->height);
	if (status != 0)
		return -1;
	requests->style.canvas = &requests->canvas;

	/* Succeeded: the new frame is drawn next. */
	requests->dirty = 1;
	return 0;
}

/*
 * Draws one frame: the requests handed over, what the last start said,
 * and Start Another.
 */
static void
requests_draw(
	struct requests *requests,
	uint64_t now_us)
{
	const struct kl_theme *theme;
	struct kl_rect whole;
	struct kl_rect button;
	const char *heading;
	size_t index;
	int baseline;
	int clicked;

	/* Starts the frame. */
	theme = kl_theme_default();
	requests->style.theme = theme;
	kl_ui_begin(requests->ui, now_us);

	/* Paints the ground. */
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)requests->width;
	whole.height = (int)requests->height;
	kl_canvas_gradient(&requests->canvas, &whole, theme->ground_top, theme->ground_bottom);

	/* The heading: whether this is the one copy. */
	heading = "Requests handed to this copy";
	if (requests->instance == NULL)
		heading = "Running on its own: no requests come";
	baseline = kl_text_center(theme->text_body, MARGIN, LINE_HEIGHT);
	(void)kl_text_draw(&requests->text, &requests->canvas, MARGIN, baseline, heading, strlen(heading), theme->text_body, 1, theme->text);

	/* The requests, oldest first. */
	for (index = 0; index < requests->count; index++) {
		baseline = kl_text_center(theme->text_body, MARGIN + (int)(index + 1U) * LINE_HEIGHT, LINE_HEIGHT);
		(void)kl_text_draw_fit(&requests->text, &requests->canvas, MARGIN, baseline, requests->list[index], theme->text_body, 0, (int)requests->width - 2 * MARGIN, theme->text_secondary);
	}

	/* What the last start said. */
	baseline = kl_text_center(theme->text_small, (int)requests->height - MARGIN - theme->control_height - LINE_HEIGHT - 4, LINE_HEIGHT);
	(void)kl_text_draw(&requests->text, &requests->canvas, MARGIN, baseline, requests->status, strlen(requests->status), theme->text_small, 0, theme->text_secondary);

	/* Start Another: the program again, apart from this one. */
	button.x = MARGIN;
	button.y = (int)requests->height - MARGIN - theme->control_height;
	button.width = BUTTON_WIDTH;
	button.height = theme->control_height;
	clicked = kl_button(requests->ui, &requests->style, ID_ANOTHER, &button, "Start Another", KL_BUTTON_PRIMARY);
	if (clicked)
		requests_launch(requests);

	/* Ends the frame. */
	requests->moving = kl_ui_end(requests->ui, now_us);
	kl_ui_window_text(requests->ui, requests->window);
}

/*
 * Draws and shows a frame; nonzero when the frame cannot be shown.
 */
static int
requests_frame(
	struct requests *requests)
{
	int status;

	/* Draws the frame. */
	requests->dirty = 0;
	requests_draw(requests, kl_clock_us());

	/* Hands it to the compositor. */
	status = kl_window_present(requests->window, requests->pixels, requests->width);
	if (status == EAGAIN) {
		/* The window's size changed under the frame: a new frame of the new size. */
		requests->resized = 1;
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
requests_input(
	struct requests *requests,
	const struct kl_window_event *input)
{
	int taken;

	/* The window's own inputs: a new size, the close button. */
	if (input->kind == KL_WINDOW_RESIZE) {
		requests->resized = 1;
		return;
	}

	/* The close button ends the program. */
	if (input->kind == KL_WINDOW_CLOSE) {
		requests->closed = 1;
		return;
	}

	/* The rest is the widgets'. */
	taken = kl_ui_window_input(requests->ui, input);
	if (taken)
		requests->dirty = 1;
}

/*
 * Puts a request on the list, dropping the oldest when the list is full.
 */
static void
requests_add(
	struct requests *requests,
	const char *request)
{
	/* A full list: the oldest goes. */
	if (requests->count == REQUEST_COUNT) {
		memmove(requests->list[0], requests->list[1], sizeof(requests->list[0]) * (REQUEST_COUNT - 1U));
		requests->count--;
	}

	/* The request at the end. */
	snprintf(requests->list[requests->count], sizeof(requests->list[0]), "%s", request);
	requests->count++;
	requests->dirty = 1;
}

/*
 * Takes every request the later starts handed over, and brings the window
 * to the front with each one's token: the user started the program to see
 * it.
 */
static void
requests_take(
	struct requests *requests)
{
	char request[KL_INSTANCE_REQUEST_MAX];
	char token[KL_ACTIVATION_TOKEN_MAX];
	int taken;
	int status;

	/* Takes them until none is left; one event may stand for several. */
	for (;;) {
		taken = kl_instance_take(requests->instance, request, sizeof(request), token, sizeof(token));
		if (!taken)
			break;

		/* An empty request (a start without an argument) is listed as such. */
		if (request[0] == '\0')
			snprintf(request, sizeof(request), "(started again)");
		requests_add(requests, request);

		/* To the front, when the start brought a token and the compositor lets it. */
		if (token[0] != '\0') {
			status = kl_activate(kl_app_display(requests->app), kl_window_surface(requests->window), token);
			if (status != 0)
				fprintf(stderr, "requests: the window stays where it is (%s)\n", strerror(status));
		}
	}
}

/*
 * Starts the program again, apart from this one, with a request of its
 * own.  The new copy finds this one and hands the request over, so it
 * comes back here as a KL_APP_INSTANCE event.
 */
static void
requests_launch(
	struct requests *requests)
{
	struct kl_launch_options options;
	const char *argv[3];
	char request[64];
	int status;

	/* The command: this program, by the path it was started with, and the next request's text. */
	requests->started++;
	snprintf(request, sizeof(request), "Request %u from Start Another", requests->started);
	argv[0] = requests->program;
	argv[1] = request;
	argv[2] = NULL;

	/* Starts it; from the window, so that the compositor may let the new copy's window come to the front. */
	memset(&options, 0, sizeof(options));
	options.argv = argv;
	options.from = requests->window;
	status = kl_launch(requests->app, &options);
	if (status != 0) {
		snprintf(requests->status, sizeof(requests->status), "Could not start %s (%s).", requests->program, strerror(status));
	} else {
		snprintf(requests->status, sizeof(requests->status), "Started: %s.", request);
	}

	/* Draws the new status. */
	requests->dirty = 1;
}
