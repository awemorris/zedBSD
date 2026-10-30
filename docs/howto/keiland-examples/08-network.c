/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A window that sends a line to itself over the network a hundred times and
 * measures how long each round trip takes.  Start opens a thread of its own
 * (the network thread), which opens two sockets on the loopback: one that
 * listens and one that connects to it without waiting.  The thread waits for
 * both of them at once with kl_socket_poll, then sends "ping N" from one
 * socket to the other and back, waiting for each answer the same way.  It
 * tells the application how far it got with kl_app_post.  Stop rings a
 * waker, which ends the thread's wait at once.
 */

#include <keiland/keiland.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Where the desktop's fonts are (/opt/keiland/share/fonts for a Linux build under /opt/keiland). */
#ifndef FONT_DIR
#define FONT_DIR	"/usr/share/fonts"
#endif
#define FONT		FONT_DIR "/keiland.ttf"
#define FALLBACK_FONT	FONT_DIR "/keiland-fallback.ttf"

/* The widgets' ids. */
#define ID_START	1U
#define ID_STOP		2U

/* The program's own codes of its posts: how far the round trips got, and their end. */
#define POST_PROGRESS	1U
#define POST_FINISHED	2U

/* The round trips made, how many pass between two reports, and the pause after each (milliseconds). */
#define TRIPS		100U
#define REPORT_EVERY	10U
#define PAUSE_MS	20

/* The room for one line, with its newline and NUL. */
#define LINE_SIZE	32U

/* The buttons' width and the room around them, in pixels. */
#define BUTTON_WIDTH	96
#define MARGIN		24

/*
 * One series of round trips: what the thread is given (the application and
 * the waker), what it found (how many round trips it made, the time they
 * took together, and why it ended), and the flag the window raises to stop
 * it.  The application's thread owns the record until the thread starts,
 * and again from the POST_FINISHED that hands it back; only stop and the
 * waker are shared in between.
 */
struct exchange {
	struct kl_app *app;
	struct kl_socket_waker *waker;
	unsigned done;
	uint64_t total_us;
	int error;
	atomic_int stop;
};

/* One report of how far the round trips got: made by the thread, freed by the one who takes the post. */
struct progress {
	unsigned done;
	uint64_t total_us;
};

/*
 * Everything the program keeps: its connection, window and widgets, the
 * series running (NULL when none) and its thread, what the window shows of
 * it, and what the main loop has to do next.
 */
struct network {
	struct kl_app *app;
	struct kl_window *window;
	struct kl_ui *ui;
	struct kl_text text;
	struct kl_style style;
	struct kl_canvas canvas;
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	struct exchange *exchange;
	struct kl_thread *thread;
	unsigned done;
	char status[128];
	int dirty;
	int moving;
	int resized;
	int closed;
};

static int network_resize(struct network *network);
static void network_draw(struct network *network, uint64_t now_us);
static int network_frame(struct network *network);
static void network_event(struct network *network, const struct kl_app_event *event);
static void network_input(struct network *network, const struct kl_window_event *input);
static void network_post(struct network *network, uint32_t code, void *data);
static void network_start(struct network *network);
static void network_stop(struct network *network);
static void network_finish(struct network *network);
static void network_run(void *argument);
static int network_connect(struct kl_socket **listener, struct kl_socket **client);
static int network_join(struct exchange *exchange, struct kl_socket *listener, struct kl_socket *client, struct kl_socket **server);
static int network_trips(struct exchange *exchange, struct kl_socket *client, struct kl_socket *server);
static int network_trip(struct exchange *exchange, struct kl_socket *client, struct kl_socket *server, unsigned number);
static int network_pause(struct exchange *exchange);
static int network_wait(struct exchange *exchange, struct kl_socket *socket, unsigned events);
static int network_send_line(struct exchange *exchange, struct kl_socket *socket, const char *line);
static int network_receive_line(struct exchange *exchange, struct kl_socket *socket, char *line, size_t size);
static uint64_t network_now_us(void);

int
main(void)
{
	static struct network network;
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	struct kl_app_event event;
	int timeout;
	int status;
	int taken;

	/* Connects to the desktop. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "org.example.Network";
	network.app = kl_app_open(&app_options);
	if (network.app == NULL) {
		fprintf(stderr, "network: no desktop (%s)\n", strerror(errno));
		return 1;
	}

	/* Opens the window. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Network";
	window_options.width = 440;
	window_options.height = 240;
	window_options.present = KL_PRESENT_VULKAN;
	network.window = kl_app_window_create(network.app, &window_options);
	if (network.window == NULL) {
		fprintf(stderr, "network: no window (%s)\n", strerror(errno));
		return 1;
	}

	/* Makes the widgets' record and opens the font. */
	network.ui = kl_ui_create();
	if (network.ui == NULL)
		return 1;
	status = kl_text_open(&network.text, FONT, FALLBACK_FONT);
	if (status != 0) {
		fprintf(stderr, "network: no font %s (%s)\n", FONT, strerror(status));
		return 1;
	}

	/* Gives the widgets the font. */
	network.style.text = &network.text;
	snprintf(network.status, sizeof(network.status), "No round trips yet.");

	/* Makes the first frame. */
	status = network_resize(&network);
	if (status != 0)
		return 1;

	/* Runs until the window is closed; the thread's posts arrive among the other events. */
	while (!network.closed) {
		/* Waits for the desktop or a post: not at all while a frame is due, a frame's time while something moves. */
		timeout = -1;
		if (network.moving)
			timeout = 16;
		if (network.dirty || network.resized)
			timeout = 0;
		status = kl_app_dispatch(network.app, timeout);
		if (status != 0)
			break;

		/* Takes every event queued, in order. */
		for (;;) {
			taken = kl_app_take(network.app, &event);
			if (!taken)
				break;
			network_event(&network, &event);
		}

		/* A new size: a new frame of that size. */
		if (network.resized) {
			network.resized = 0;
			status = network_resize(&network);
			if (status != 0)
				break;
		}

		/* Something changed or moves: draws and shows a frame. */
		if (network.dirty || network.moving) {
			status = network_frame(&network);
			if (status != 0)
				break;
		}
	}

	/* Ends the round trips still running before the application closes: no thread may post after it. */
	network_finish(&network);

	/* Frees the widgets' record, the frame and the font, and closes the window and the connection. */
	kl_ui_destroy(network.ui);
	kl_canvas_release(&network.canvas);
	free(network.pixels);
	kl_text_close(&network.text);
	kl_app_close(network.app);

	/* Succeeded: the window was closed. */
	return 0;
}

/*
 * Makes the frame at the window's size; nonzero when it cannot.
 */
static int
network_resize(
	struct network *network)
{
	uint32_t *pixels;
	int status;

	/* Sets the presenter to the window's size, which it reports. */
	status = kl_window_present_resize(network->window, &network->width, &network->height);
	if (status != 0)
		return -1;

	/* Allocates the frame. */
	pixels = malloc((size_t)network->width * network->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* Puts the canvas on the new pixels in place of the old ones; the widgets draw on it. */
	if (network->pixels != NULL)
		kl_canvas_release(&network->canvas);
	free(network->pixels);
	network->pixels = pixels;
	status = kl_canvas_init(&network->canvas, network->pixels, network->width, (int)network->width, (int)network->height);
	if (status != 0)
		return -1;
	network->style.canvas = &network->canvas;

	/* Succeeded: the new frame is drawn next. */
	network->dirty = 1;
	return 0;
}

/*
 * Draws one frame: the question, how far the round trips got, what they
 * found, and the Start and Stop buttons.
 */
static void
network_draw(
	struct network *network,
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
	network->style.theme = theme;
	kl_ui_begin(network->ui, now_us);

	/* Paints the ground. */
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)network->width;
	whole.height = (int)network->height;
	kl_canvas_gradient(&network->canvas, &whole, theme->ground_top, theme->ground_bottom);

	/* The question. */
	snprintf(line, sizeof(line), "%u round trips over the loopback", TRIPS);
	baseline = kl_text_center(theme->text_body, MARGIN, 24);
	(void)kl_text_draw(&network->text, &network->canvas, MARGIN, baseline, line, strlen(line), theme->text_body, 1, theme->text);

	/* How far the round trips got, as a bar, and what they found. */
	bar.x = MARGIN;
	bar.y = MARGIN + 44;
	bar.width = (int)network->width - 2 * MARGIN;
	bar.height = 6;
	fraction = (double)network->done / (double)TRIPS;
	kl_progress(&network->style, &bar, fraction, now_us);
	baseline = kl_text_center(theme->text_body, bar.y + 16, 24);
	(void)kl_text_draw(&network->text, &network->canvas, MARGIN, baseline, network->status, strlen(network->status), theme->text_body, 0, theme->text_secondary);

	/* Lays out the two buttons in a row at the bottom. */
	start.x = MARGIN;
	start.y = (int)network->height - MARGIN - theme->control_height;
	start.width = BUTTON_WIDTH;
	start.height = theme->control_height;
	stop = start;
	stop.x = start.x + BUTTON_WIDTH + 12;

	/* Start: new round trips on a network thread; disabled while some run. */
	start_flags = KL_BUTTON_PRIMARY;
	if (network->exchange != NULL)
		start_flags |= KL_BUTTON_DISABLED;
	clicked = kl_button(network->ui, &network->style, ID_START, &start, "Start", start_flags);
	if (clicked)
		network_start(network);

	/* Stop: asks the thread to end; its series comes back as usual, cut short. */
	stop_flags = 0U;
	if (network->exchange == NULL)
		stop_flags = KL_BUTTON_DISABLED;
	clicked = kl_button(network->ui, &network->style, ID_STOP, &stop, "Stop", stop_flags);
	if (clicked && network->exchange != NULL)
		network_stop(network);

	/* Ends the frame. */
	network->moving = kl_ui_end(network->ui, now_us);
	kl_ui_window_text(network->ui, network->window);
}


/*
 * Draws and shows a frame; nonzero when the frame cannot be shown.
 */
static int
network_frame(
	struct network *network)
{
	int status;

	/* Draws the frame. */
	network->dirty = 0;
	network_draw(network, kl_clock_us());

	/* Hands it to the compositor. */
	status = kl_window_present(network->window, network->pixels, network->width);
	if (status == EAGAIN) {
		/* The window's size changed under the frame: a new frame of the new size. */
		network->resized = 1;
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
network_event(
	struct network *network,
	const struct kl_app_event *event)
{
	/* The appearance changed: the theme's colours are new. */
	if (event->kind == KL_APP_THEME)
		network->dirty = 1;

	/* A post to the window comes among its input, as an input of its own kind. */
	if (event->kind == KL_APP_WINDOW && event->window == network->window) {
		if (event->input.kind == KL_WINDOW_POST) {
			network_post(network, event->input.code, event->input.data);
			return;
		}

		/* Offers any other input to the widgets. */
		network_input(network, &event->input);
	}

	/* A post to the application, or one to a window that closed before it came. */
	if (event->kind == KL_APP_POST)
		network_post(network, event->code, event->data);
}

/*
 * Gives one input of the window to the widgets, or acts on it.
 */
static void
network_input(
	struct network *network,
	const struct kl_window_event *input)
{
	int taken;

	/* The window's own inputs: a new size, the close button. */
	if (input->kind == KL_WINDOW_RESIZE) {
		network->resized = 1;
		return;
	}

	/* The close button ends the program. */
	if (input->kind == KL_WINDOW_CLOSE) {
		network->closed = 1;
		return;
	}

	/* The rest is the widgets'. */
	taken = kl_ui_window_input(network->ui, input);
	if (taken)
		network->dirty = 1;
}

/*
 * Takes one of the thread's posts: a report of progress, which is shown
 * and freed, or the series handed back, which ends it.  The data of each is
 * the program's from now on.
 */
static void
network_post(
	struct network *network,
	uint32_t code,
	void *data)
{
	struct progress *progress;
	struct exchange *exchange;

	/* A report: shown, then freed (the thread's reports come before its series, never after). */
	if (code == POST_PROGRESS) {
		progress = data;
		network->done = progress->done;
		snprintf(network->status, sizeof(network->status), "%u of %u round trips, %lu us each so far.", progress->done, TRIPS, (unsigned long)(progress->total_us / progress->done));
		free(progress);
		network->dirty = 1;
		return;
	}

	/* The series handed back: its thread is ending, so it is joined, and only then is its waker closed. */
	if (code == POST_FINISHED) {
		exchange = data;
		(void)kl_thread_join(network->thread);
		network->thread = NULL;
		kl_socket_waker_close(exchange->waker);
		network->done = exchange->done;
		if (exchange->error == 0) {
			snprintf(network->status, sizeof(network->status), "Done: %u round trips, %lu us each.", exchange->done, (unsigned long)(exchange->total_us / exchange->done));
		} else if (exchange->error == ECANCELED) {
			snprintf(network->status, sizeof(network->status), "Stopped after %u round trips.", exchange->done);
		} else {
			snprintf(network->status, sizeof(network->status), "Failed after %u round trips (%s).", exchange->done, strerror(exchange->error));
		}
		free(exchange);
		network->exchange = NULL;
		network->dirty = 1;
	}
}

/*
 * Starts the round trips on a network thread.
 */
static void
network_start(
	struct network *network)
{
	struct kl_thread_options options;
	struct exchange *exchange;
	int status;

	/* The series' record, handed to the thread. */
	exchange = calloc(1, sizeof(*exchange));
	if (exchange == NULL) {
		snprintf(network->status, sizeof(network->status), "No memory for the round trips.");
		return;
	}

	/* Gives the thread what it works with. */
	exchange->app = network->app;
	atomic_init(&exchange->stop, 0);

	/* The waker that ends the thread's waits; it is closed after the thread is joined. */
	status = kl_socket_waker_open(&exchange->waker);
	if (status != 0) {
		free(exchange);
		snprintf(network->status, sizeof(network->status), "No waker (%s).", strerror(status));
		return;
	}

	/* The network thread: from here until POST_FINISHED, the record is its own but for stop. */
	memset(&options, 0, sizeof(options));
	options.name = "network";
	options.run = network_run;
	options.argument = exchange;
	options.priority = KL_THREAD_PRIORITY_NORMAL;
	status = kl_thread_start(&options, &network->thread);
	if (status != 0) {
		kl_socket_waker_close(exchange->waker);
		free(exchange);
		snprintf(network->status, sizeof(network->status), "No thread for the network.");
		return;
	}

	/* Remembers the running series. */
	network->exchange = exchange;
	network->done = 0;
	snprintf(network->status, sizeof(network->status), "Connecting...");
}

/*
 * Asks the thread to end: raises its flag, then rings its waker, so that a
 * wait in progress returns and the thread looks at the flag.
 */
static void
network_stop(
	struct network *network)
{
	/* The flag first: the thread looks at it after its wait returns. */
	atomic_store(&network->exchange->stop, 1);

	/* Ends the thread's wait, or the next one it makes. */
	kl_socket_waker_ring(network->exchange->waker);
}

/*
 * Ends round trips still running when the program ends: asks the thread to
 * stop, and takes the posts it left, the last of which joins its thread.
 */
static void
network_finish(
	struct network *network)
{
	struct kl_app_event event;
	int status;
	int taken;

	/* Nothing runs. */
	if (network->exchange == NULL)
		return;

	/* Stops the thread; it posts its series as it ends, and taking that post joins it. */
	network_stop(network);

	/* Takes the posts, waiting for them, until the series came back (the last of them). */
	while (network->exchange != NULL) {
		status = kl_app_dispatch(network->app, -1);
		if (status != 0)
			break;
		for (;;) {
			taken = kl_app_take(network->app, &event);
			if (!taken)
				break;
			if (event.kind == KL_APP_WINDOW && event.input.kind == KL_WINDOW_POST) {
				network_post(network, event.input.code, event.input.data);
			} else if (event.kind == KL_APP_POST) {
				network_post(network, event.code, event.data);
			}
		}
	}
}

/*
 * The network thread: makes the round trips over two sockets of the
 * loopback, reports every REPORT_EVERY of them, and hands the series back
 * when it is done, failed or asked to stop.  It calls nothing of the
 * application but the posts, which any thread may; it waits for the
 * network only here, never on the application's thread.
 */
static void
network_run(
	void *argument)
{
	struct exchange *exchange;
	struct kl_socket *listener;
	struct kl_socket *client;
	struct kl_socket *server;
	int status;

	/* Starts with no socket; each is closed below if it was opened. */
	exchange = argument;
	listener = NULL;
	client = NULL;
	server = NULL;

	/* Opens the listening socket and the connecting one, and waits for them to meet. */
	status = network_connect(&listener, &client);
	if (status == 0)
		status = network_join(exchange, listener, client, &server);

	/* Makes the round trips over the two ends of the connection. */
	if (status == 0)
		status = network_trips(exchange, client, server);

	/* Closes the sockets: this thread is the only one that used them. */
	if (server != NULL)
		kl_socket_close(server);
	if (client != NULL)
		kl_socket_close(client);
	if (listener != NULL)
		kl_socket_close(listener);

	/* Notes why the series ended (0: all the round trips were made). */
	exchange->error = status;

	/* Hands the series back; it fails only without memory, and then the window shows the series as running. */
	status = kl_app_post(exchange->app, POST_FINISHED, exchange);
	if (status != 0)
		fprintf(stderr, "network: the round trips were lost (%s)\n", strerror(status));

	/* The thread's end: the application's thread joins it. */
}

/*
 * Opens a socket that listens on the loopback, at a port the system
 * chooses, and one that connects to it without waiting; zero when both are
 * open.  The connect has not ended yet: network_join waits for it.
 */
static int
network_connect(
	struct kl_socket **listener,
	struct kl_socket **client)
{
	struct kl_address address;
	int status;

	/* The listening socket: a port of the loopback, which the system chooses (0). */
	kl_address_loopback(KL_ADDRESS_IPV4, 0, &address);
	status = kl_socket_open(KL_ADDRESS_IPV4, KL_SOCKET_STREAM, KL_SOCKET_NONBLOCKING, listener);
	if (status != 0)
		return status;
	status = kl_socket_bind(*listener, &address);
	if (status != 0)
		return status;
	status = kl_socket_listen(*listener, 1);
	if (status != 0)
		return status;

	/* Asks which port it got: that is where the other socket connects. */
	status = kl_socket_local_address(*listener, &address);
	if (status != 0)
		return status;

	/* The connecting socket: its connect returns EINPROGRESS and ends later, or ends at once on the loopback. */
	status = kl_socket_open(KL_ADDRESS_IPV4, KL_SOCKET_STREAM, KL_SOCKET_NONBLOCKING, client);
	if (status != 0)
		return status;
	status = kl_socket_connect(*client, &address);
	if (status != 0 && status != EINPROGRESS)
		return status;

	/* Succeeded: both sockets are open, the connection may still be on its way. */
	return 0;
}

/*
 * Waits until the listening socket holds the connection (to accept it) and
 * the connecting one has ended its connect, both at once, and ends early
 * when asked to stop; zero when the connection is made, with the accepted
 * socket in *server.
 */
static int
network_join(
	struct exchange *exchange,
	struct kl_socket *listener,
	struct kl_socket *client,
	struct kl_socket **server)
{
	struct kl_socket_wait waits[3];
	size_t count;
	size_t ready;
	size_t index;
	int connected;
	int stop;
	int status;

	/* Not connected yet; the connect may have ended at once, but the loop asks all the same. */
	connected = 0;

	/* Waits for the connection on both of its ends, and for the waker, until the two sockets are in use. */
	while (*server == NULL || !connected) {
		/* Lists what is still waited for: the connection to accept, the connect to end, and the waker. */
		memset(waits, 0, sizeof(waits));
		count = 0;
		if (*server == NULL) {
			waits[count].socket = listener;
			waits[count].events = KL_SOCKET_READABLE;
			count++;
		}
		if (!connected) {
			waits[count].socket = client;
			waits[count].events = KL_SOCKET_WRITABLE;
			count++;
		}
		waits[count].waker = exchange->waker;
		waits[count].events = KL_SOCKET_READABLE;
		count++;

		/* Waits for any of them. */
		status = kl_socket_poll(waits, count, KL_SOCKET_FOREVER, &ready);
		if (status != 0)
			return status;

		/* Looks at the stop flag only now that the wait returned: the ring came after the flag was raised. */
		stop = atomic_load(&exchange->stop);
		if (stop)
			return ECANCELED;

		/* Takes what became ready: the connection is accepted, the connect's end is read. */
		for (index = 0; index < count; index++) {
			if (waits[index].ready == 0)
				continue;
			if (waits[index].socket == listener) {
				status = kl_socket_accept(listener, KL_SOCKET_NONBLOCKING, server, NULL);
				if (status != 0 && status != EAGAIN)
					return status;
			} else if (waits[index].socket == client) {
				status = kl_socket_connect_result(client);
				if (status != 0)
					return status;
				connected = 1;
			}
		}
	}

	/* Succeeded: the client is connected and the server socket is its other end. */
	return 0;
}

/*
 * Makes the round trips one after the other, with a pause after each, and
 * reports every REPORT_EVERY of them to the application; zero when all are
 * made.
 */
static int
network_trips(
	struct exchange *exchange,
	struct kl_socket *client,
	struct kl_socket *server)
{
	struct progress *progress;
	unsigned number;
	int status;

	/* Makes the round trips, numbered from 1. */
	for (number = 1U; number <= TRIPS; number++) {
		status = network_trip(exchange, client, server, number);
		if (status != 0)
			return status;

		/* Every REPORT_EVERY round trips: tells the window how far it got. */
		if (number % REPORT_EVERY == 0) {
			progress = malloc(sizeof(*progress));
			if (progress != NULL) {
				progress->done = exchange->done;
				progress->total_us = exchange->total_us;
				status = kl_app_post(exchange->app, POST_PROGRESS, progress);
				if (status != 0)
					free(progress);
			}
		}

		/* The pause, which a Stop ends at once. */
		status = network_pause(exchange);
		if (status != 0)
			return status;
	}

	/* Succeeded: every round trip was made. */
	return 0;
}

/*
 * Makes one round trip: sends "ping N" from the client, the server socket
 * receives it and sends the same line back, and the client receives it;
 * zero when the line came back as it went, with the round trip counted.
 */
static int
network_trip(
	struct exchange *exchange,
	struct kl_socket *client,
	struct kl_socket *server,
	unsigned number)
{
	char line[LINE_SIZE];
	char heard[LINE_SIZE];
	char answer[LINE_SIZE];
	uint64_t started_us;
	uint64_t elapsed_us;
	int status;

	/* The line, and the time it is sent at (the monotonic clock). */
	snprintf(line, sizeof(line), "ping %u\n", number);
	started_us = network_now_us();

	/* The client sends it. */
	status = network_send_line(exchange, client, line);
	if (status != 0)
		return status;

	/* The server receives it and sends the same line back. */
	status = network_receive_line(exchange, server, heard, sizeof(heard));
	if (status != 0)
		return status;
	status = network_send_line(exchange, server, heard);
	if (status != 0)
		return status;

	/* The client receives the answer. */
	status = network_receive_line(exchange, client, answer, sizeof(answer));
	if (status != 0)
		return status;

	/* The answer must be the line that went. */
	status = strcmp(line, answer);
	if (status != 0)
		return EPROTO;

	/* Counts the round trip and the time it took. */
	elapsed_us = network_now_us() - started_us;
	exchange->done++;
	exchange->total_us += elapsed_us;

	/* Succeeded: the line came back. */
	return 0;
}

/*
 * Pauses between two round trips by waiting for the waker alone, for
 * PAUSE_MS: a Stop ends the pause at once.  Zero when the pause passed, or
 * ECANCELED when Stop was asked.
 */
static int
network_pause(
	struct exchange *exchange)
{
	struct kl_socket_wait wait;
	size_t ready;
	int stop;
	int status;

	/* Waits for the waker only, with a time limit. */
	memset(&wait, 0, sizeof(wait));
	wait.waker = exchange->waker;
	wait.events = KL_SOCKET_READABLE;
	status = kl_socket_poll(&wait, 1, PAUSE_MS, &ready);
	if (status != 0)
		return status;

	/* Looks at the stop flag after the wait returned, whether it timed out or was rung. */
	stop = atomic_load(&exchange->stop);
	if (stop)
		return ECANCELED;

	/* Succeeded: the pause passed. */
	return 0;
}

/*
 * Waits until a socket is ready for what events say, or until the waker
 * rings; zero when the socket is ready, ECANCELED when Stop was asked.
 */
static int
network_wait(
	struct exchange *exchange,
	struct kl_socket *socket,
	unsigned events)
{
	struct kl_socket_wait waits[2];
	size_t ready;
	int stop;
	int status;

	/* The socket, and the waker that Stop rings. */
	memset(waits, 0, sizeof(waits));
	waits[0].socket = socket;
	waits[0].events = events;
	waits[1].waker = exchange->waker;
	waits[1].events = KL_SOCKET_READABLE;
	status = kl_socket_poll(waits, 2, KL_SOCKET_FOREVER, &ready);
	if (status != 0)
		return status;

	/* Looks at the stop flag after the wait returned. */
	stop = atomic_load(&exchange->stop);
	if (stop)
		return ECANCELED;

	/* Succeeded: the socket is ready, or failed, which its next call reports. */
	return 0;
}

/*
 * Sends a whole line on a socket that does not block: what the socket
 * takes goes at once, the rest after a wait for room.
 */
static int
network_send_line(
	struct exchange *exchange,
	struct kl_socket *socket,
	const char *line)
{
	size_t length;
	size_t total;
	size_t sent;
	int status;

	/* Sends until every byte of the line has gone. */
	length = strlen(line);
	total = 0;
	while (total < length) {
		status = kl_socket_send(socket, line + total, length - total, &sent);
		if (status == EAGAIN) {
			/* No room now: waits for it. */
			status = network_wait(exchange, socket, KL_SOCKET_WRITABLE);
			if (status != 0)
				return status;
			continue;
		}
		if (status != 0)
			return status;
		total += sent;
	}

	/* Succeeded: the line was sent. */
	return 0;
}

/*
 * Receives a line, up to its newline, on a socket that does not block,
 * waiting for the bytes first; zero when the line is whole in the buffer.
 */
static int
network_receive_line(
	struct exchange *exchange,
	struct kl_socket *socket,
	char *line,
	size_t size)
{
	size_t length;
	size_t received;
	int status;

	/* Receives until the newline came. */
	length = 0;
	for (;;) {
		/* Waits for bytes (or the stream's end), or for Stop. */
		status = network_wait(exchange, socket, KL_SOCKET_READABLE);
		if (status != 0)
			return status;

		/* Takes what is there; EAGAIN means the wait ended without bytes, so it waits again. */
		status = kl_socket_receive(socket, line + length, size - 1U - length, &received);
		if (status == EAGAIN)
			continue;
		if (status != 0)
			return status;

		/* Zero bytes are the stream's end: the peer closed before the line was whole. */
		if (received == 0)
			return ECONNRESET;
		length += received;
		if (line[length - 1U] == '\n')
			break;

		/* A line that fills the buffer without a newline is not one of ours. */
		if (length == size - 1U)
			return EMSGSIZE;
	}

	/* Succeeded: the line, ended for the string functions. */
	line[length] = '\0';
	return 0;
}

/*
 * Reports the monotonic clock in microseconds.
 */
static uint64_t
network_now_us(void)
{
	struct timespec now;

	/* The monotonic clock, which a change of the time of day does not move. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Reports it in microseconds. */
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}
