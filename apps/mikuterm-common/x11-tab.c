/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * An X window shown in a tab of mikuterm (x11-tab.h): what the network
 * thread keeps of the window, and the tab's connection on the
 * application's thread, which draws the window's pixels at the X server's
 * scale and turns the user's input into X's (its place in X's pixels, the
 * pointer's buttons as X numbers them, the wheels' notches as presses of
 * buttons 4 to 7).
 */

#include "x11-tab.h"
#include "session.h"

#include "userland/desktop/xserver/win32/scale.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The colour of the tab where the window does not reach (the window is drawn at the tab's corner). */
#define TAB_BACKGROUND		0xFF202020U

/* Every pixel shown is opaque (the X pixels' top byte is not alpha). */
#define TAB_OPAQUE		0xFF000000U

/* How far the window's wheel moves for one notch (libkeiland's distance of a notch). */
#define TAB_WHEEL_NOTCH		60.0

/* The evdev codes of the pointer's buttons: left, right, middle, and the side buttons back and forward. */
#define TAB_BUTTON_LEFT		0x110U
#define TAB_BUTTON_RIGHT	0x111U
#define TAB_BUTTON_MIDDLE	0x112U
#define TAB_BUTTON_SIDE		0x113U
#define TAB_BUTTON_EXTRA	0x114U

/* X's buttons: the wheels' up, down, left and right, and the side buttons. */
#define TAB_X_WHEEL_UP		4U
#define TAB_X_WHEEL_DOWN	5U
#define TAB_X_WHEEL_LEFT	6U
#define TAB_X_WHEEL_RIGHT	7U
#define TAB_X_BACK		8U
#define TAB_X_FORWARD		9U

static size_t tab_read(struct terminal_connection *connection, unsigned char *buffer, size_t size);
static int tab_write(struct terminal_connection *connection, const unsigned char *bytes, size_t size, size_t *written);
static int tab_resize(struct terminal_connection *connection, unsigned columns, unsigned rows, unsigned width, unsigned height);
static int tab_mode(const struct terminal_connection *connection, unsigned *mode);
static unsigned tab_event(struct terminal_connection *connection, const struct kl_app_event *event);
static unsigned tab_pending(const struct terminal_connection *connection);
static void tab_close(struct terminal_connection *connection);
static int tab_draw(struct terminal_connection *connection, uint32_t *pixels, uint32_t width, uint32_t height);
static void tab_title(struct terminal_connection *connection, char *title, size_t size);
static void tab_input(struct terminal_connection *connection, const struct terminal_picture_event *event);
static void tab_refuse(struct terminal_connection *connection);
static void tab_queue(struct x11_tab *tab, unsigned kind, uint32_t first, uint32_t second, uint32_t third, const char *text);
static void tab_pointer(struct x11_tab *tab, unsigned button, int pressed, int32_t x, int32_t y);
static void tab_wheel(struct x11_tab *tab, double *rest, double distance, unsigned back, unsigned forward, int32_t x, int32_t y);
static unsigned tab_button(uint32_t code);
static void tab_clean_title(char *title, const uint8_t *bytes, size_t size);
static void tab_ring(struct x11_tab *tab);

/* What a tab of an X window draws, and how it takes the window's input. */
static const struct terminal_picture_ops tab_picture_ops = {
	tab_draw,
	tab_title,
	tab_input,
	tab_refuse
};

/* The functions of a tab of an X window. */
static const struct terminal_connection_ops tab_ops = {
	tab_read,
	tab_write,
	tab_resize,
	tab_mode,
	tab_event,
	tab_pending,
	tab_close,
	NULL,
	&tab_picture_ops
};

/*
 * Makes the tab of an X window the child opened: its size in X's pixels
 * (black until the child sends its pixels), the X server's scale, the
 * host's name and the window's title.  Both the X server's side and the
 * application's thread hold it.  Returns 0, ENOMEM, or the error of
 * making its lock.
 */
int
x11_tab_create(
	uint32_t window,
	unsigned width,
	unsigned height,
	unsigned scale,
	const char *host,
	const uint8_t *title,
	size_t title_size,
	struct x11_tab **result)
{
	struct x11_tab *tab;
	int error;

	/* The tab's memory. */
	*result = NULL;
	tab = calloc(1U, sizeof(*tab));
	if (tab == NULL)
		return ENOMEM;

	/* The window's pixels. */
	tab->pixels = calloc((size_t)width * (size_t)height, sizeof(*tab->pixels));
	if (tab->pixels == NULL) {
		free(tab);
		return ENOMEM;
	}

	/* Its lock. */
	error = kl_mutex_init(&tab->lock);
	if (error != 0) {
		free(tab->pixels);
		free(tab);
		return error;
	}

	/* What it shows, and who holds it. */
	tab->connection.ops = &tab_ops;
	tab->window = window;
	tab->scale = scale;
	(void)snprintf(tab->host, sizeof(tab->host), "%s", host);
	tab->width = width;
	tab->height = height;
	tab_clean_title(tab->title, title, title_size);
	tab->references = 2U;
	tab->news = 1;

	/* Succeeded: the tab is made. */
	*result = tab;
	return 0;
}

/*
 * Gives a tab its session (which it holds from now on: the network
 * thread's waker and the application it posts to) and the posts' number.
 * The network thread calls it before the tab reaches the application's
 * thread, which looks at what the tab has once it takes it.
 */
void
x11_tab_attach(
	struct x11_tab *tab,
	struct mikuterm_session *session,
	uint32_t number)
{
	/* The session is held as long as the tab. */
	kl_mutex_lock(&session->lock);

	session->references++;

	kl_mutex_unlock(&session->lock);

	/* The tab's session and number. */
	kl_mutex_lock(&tab->lock);

	tab->session = session;
	tab->number = number;

	kl_mutex_unlock(&tab->lock);
}

/*
 * Lets go of a tab: the X server's side or the application's thread.  The
 * last frees it and lets go of its session.
 */
void
x11_tab_release(
	struct x11_tab *tab)
{
	unsigned remaining;

	/* One holder fewer. */
	kl_mutex_lock(&tab->lock);

	tab->references--;
	remaining = tab->references;

	kl_mutex_unlock(&tab->lock);

	/* The last one frees the tab, then its session's hold. */
	if (remaining != 0U)
		return;
	kl_mutex_destroy(&tab->lock);
	free(tab->pixels);
	if (tab->session != NULL)
		session_release(tab->session);
	free(tab);
}

/* Takes the window's new title from the child. */
void
x11_tab_set_title(
	struct x11_tab *tab,
	const uint8_t *title,
	size_t size)
{
	/* The title, for the tab to take. */
	kl_mutex_lock(&tab->lock);

	tab_clean_title(tab->title, title, size);
	tab->news = 1;

	kl_mutex_unlock(&tab->lock);
}

/*
 * Takes the window's new size from the child: its pixels are kept where
 * both sizes have them, the rest black until the child sends them.
 * Returns 0, or ENOMEM (the tab is as it was).
 */
int
x11_tab_set_size(
	struct x11_tab *tab,
	unsigned width,
	unsigned height)
{
	uint32_t *pixels;
	unsigned rows;
	unsigned columns;
	unsigned row;

	/* The new pixels, made before the lock is taken. */
	pixels = calloc((size_t)width * (size_t)height, sizeof(*pixels));
	if (pixels == NULL)
		return ENOMEM;

	/* The old pixels copied over where both sizes have them, and the new size. */
	kl_mutex_lock(&tab->lock);

	rows = tab->height;
	if (rows > height)
		rows = height;
	columns = tab->width;
	if (columns > width)
		columns = width;
	for (row = 0U; row < rows; row++)
		memcpy(pixels + (size_t)row * width, tab->pixels + (size_t)row * tab->width, (size_t)columns * sizeof(*pixels));
	free(tab->pixels);
	tab->pixels = pixels;
	tab->width = width;
	tab->height = height;
	tab->news = 1;

	kl_mutex_unlock(&tab->lock);

	/* Succeeded: the window's size. */
	return 0;
}

/*
 * Takes a part of the window's pixels from the child: its place and size,
 * and its pixels (four little-endian bytes each, 0xAARRGGBB, rows of the
 * part's width).  Returns 0, or EINVAL for a part not inside the window
 * (nothing is taken).
 */
int
x11_tab_put(
	struct x11_tab *tab,
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height,
	const uint8_t *bytes)
{
	uint32_t *line;
	const uint8_t *from;
	uint32_t row;
	uint32_t column;

	/* The part, inside the window as it is now. */
	kl_mutex_lock(&tab->lock);

	if (x > tab->width ||
	    width > tab->width - x ||
	    y > tab->height ||
	    height > tab->height - y) {
		kl_mutex_unlock(&tab->lock);
		return EINVAL;
	}

	/* Its pixels, row by row. */
	from = bytes;
	for (row = 0U; row < height; row++) {
		line = tab->pixels + (size_t)(y + row) * tab->width + x;
		for (column = 0U; column < width; column++) {
			line[column] = x11_link_get32(from);
			from += 4;
		}
	}
	tab->news = 1;

	kl_mutex_unlock(&tab->lock);

	/* Succeeded: the part is the tab's. */
	return 0;
}

/* Says that the window ended (or the X server did): the tab closes. */
void
x11_tab_end(
	struct x11_tab *tab)
{
	/* The end, for the tab to take. */
	kl_mutex_lock(&tab->lock);

	tab->ended = 1;

	kl_mutex_unlock(&tab->lock);

	/* The application's thread hears of it. */
	x11_tab_wake(tab);
}

/*
 * Tells the application's thread that the tab has news or has ended,
 * unless a post is already on its way or the tab has no session yet.
 */
void
x11_tab_wake(
	struct x11_tab *tab)
{
	struct mikuterm_session *session;
	uint32_t number;
	int error;

	/* One post at a time, of a tab with something to say. */
	kl_mutex_lock(&tab->lock);

	session = tab->session;
	number = tab->number;
	if (session == NULL || tab->posted || (!tab->news && !tab->ended)) {
		kl_mutex_unlock(&tab->lock);
		return;
	}
	tab->posted = 1;

	kl_mutex_unlock(&tab->lock);

	/* The post; a failed one lets the next try. */
	error = session_app_post(session->app, number);
	if (error != 0) {
		kl_mutex_lock(&tab->lock);

		tab->posted = 0;

		kl_mutex_unlock(&tab->lock);
	}
}

/*
 * Tells the network thread whether the tab is leaving: closed by the user
 * (1), or refused by the run (1, *refused set).  Returns 0 while it
 * stays.
 */
int
x11_tab_leaving(
	struct x11_tab *tab,
	int *refused)
{
	int leaving;

	/* The tab's end, as the application's thread left it. */
	kl_mutex_lock(&tab->lock);

	*refused = tab->refused;
	leaving = 0;
	if (tab->closed)
		leaving = 1;

	kl_mutex_unlock(&tab->lock);

	/* Reports whether it leaves. */
	return leaving;
}

/*
 * Gives the network thread the size the tab gives the window, once after
 * each change.  Returns 1 with the size, or 0 when there is none to send.
 */
int
x11_tab_take_size(
	struct x11_tab *tab,
	unsigned *width,
	unsigned *height)
{
	int wanted;

	/* The size, taken once. */
	kl_mutex_lock(&tab->lock);

	wanted = tab->wanted;
	*width = tab->wanted_width;
	*height = tab->wanted_height;
	tab->wanted = 0;

	kl_mutex_unlock(&tab->lock);

	/* Reports whether there was one. */
	return wanted;
}

/*
 * Gives the network thread the user's oldest input for the window.
 * Returns 1 with it, or 0 when none waits.
 */
int
x11_tab_take_input(
	struct x11_tab *tab,
	struct x11_tab_input *input)
{
	/* The oldest one, out of the queue. */
	kl_mutex_lock(&tab->lock);

	if (tab->input_count == 0U) {
		kl_mutex_unlock(&tab->lock);
		return 0;
	}
	*input = tab->inputs[0];
	tab->input_count--;
	memmove(&tab->inputs[0], &tab->inputs[1], tab->input_count * sizeof(tab->inputs[0]));

	kl_mutex_unlock(&tab->lock);

	/* Succeeded: one input. */
	return 1;
}

/* Reads nothing (the picture is drawn), but takes the news so they are told once. */
static size_t
tab_read(
	struct terminal_connection *connection,
	unsigned char *buffer,
	size_t size)
{
	struct x11_tab *tab;

	(void)buffer;
	(void)size;

	/* The tab the connection is. */
	tab = (struct x11_tab *)connection;

	/* The news are taken. */
	kl_mutex_lock(&tab->lock);

	tab->news = 0;

	kl_mutex_unlock(&tab->lock);

	/* Reports no bytes. */
	return 0U;
}

/* Takes the bytes typed and sends them nowhere (the keys go to the window as keys). */
static int
tab_write(
	struct terminal_connection *connection,
	const unsigned char *bytes,
	size_t size,
	size_t *written)
{
	(void)connection;
	(void)bytes;

	/* Succeeded: all taken. */
	*written = size;
	return 0;
}

/*
 * Takes the tab's size in pixels: the window is asked to be that size in
 * X's pixels (the X server's scale divided out), which the child sends
 * the X client.
 */
static int
tab_resize(
	struct terminal_connection *connection,
	unsigned columns,
	unsigned rows,
	unsigned width,
	unsigned height)
{
	struct x11_tab *tab;
	int32_t x_width;
	int32_t x_height;
	int changed;

	(void)columns;
	(void)rows;

	/* The size in X's pixels, at least one and at most a tab's longest side. */
	tab = (struct x11_tab *)connection;
	x_width = x11_win32_scale_to_x((int32_t)width, tab->scale);
	x_height = x11_win32_scale_to_x((int32_t)height, tab->scale);
	if (x_width < 1)
		x_width = 1;
	if (x_height < 1)
		x_height = 1;
	if (x_width > (int32_t)X11_LINK_TAB_SIDE_MAX)
		x_width = (int32_t)X11_LINK_TAB_SIDE_MAX;
	if (x_height > (int32_t)X11_LINK_TAB_SIDE_MAX)
		x_height = (int32_t)X11_LINK_TAB_SIDE_MAX;

	/* The size for the network thread, when it is another. */
	kl_mutex_lock(&tab->lock);

	changed = 0;
	if (tab->wanted_width != (unsigned)x_width || tab->wanted_height != (unsigned)x_height) {
		tab->wanted_width = (unsigned)x_width;
		tab->wanted_height = (unsigned)x_height;
		tab->wanted = 1;
		changed = 1;
	}

	kl_mutex_unlock(&tab->lock);

	/* The network thread sends it. */
	if (changed)
		tab_ring(tab);

	/* Succeeded. */
	return 0;
}

/* Cannot tell the input's modes: the keys go to the window as keys. */
static int
tab_mode(
	const struct terminal_connection *connection,
	unsigned *mode)
{
	(void)connection;
	(void)mode;

	/* Reports that it cannot tell. */
	return ENOTSUP;
}

/*
 * Tells whether an application's event is this tab's post: news to show
 * (TERMINAL_READY_OUTPUT) and the window's end, 0 when the event is not
 * its.
 */
static unsigned
tab_event(
	struct terminal_connection *connection,
	const struct kl_app_event *event)
{
	struct x11_tab *tab;
	unsigned ready;

	/* Only this tab's post. */
	tab = (struct x11_tab *)connection;
	if (event->kind != KL_APP_POST || event->code != SESSION_POST)
		return 0U;
	if ((uintptr_t)event->data != (uintptr_t)tab->number)
		return 0U;

	/* The post is taken: the network thread may post again. */
	kl_mutex_lock(&tab->lock);

	tab->posted = 0;
	ready = 0U;
	if (tab->news)
		ready |= TERMINAL_READY_OUTPUT;
	if (tab->ended)
		ready |= TERMINAL_READY_EXIT;

	kl_mutex_unlock(&tab->lock);

	/* Reports what the tab has. */
	return ready;
}

/* Tells what the tab has now, whatever events came: news to show, the window's end. */
static unsigned
tab_pending(
	const struct terminal_connection *connection)
{
	struct x11_tab *tab;
	unsigned ready;

	/* The tab the connection is. */
	tab = (struct x11_tab *)connection;

	/* The news and the end. */
	kl_mutex_lock(&tab->lock);

	ready = 0U;
	if (tab->news)
		ready |= TERMINAL_READY_OUTPUT;
	if (tab->ended)
		ready |= TERMINAL_READY_EXIT;

	kl_mutex_unlock(&tab->lock);

	/* Reports what is there. */
	return ready;
}

/*
 * Closes the tab: the network thread asks the window to close (as its
 * window manager's close: the X client may ask the user first) and the
 * application's thread lets go.  It returns at once.
 */
static void
tab_close(
	struct terminal_connection *connection)
{
	struct x11_tab *tab;

	/* The tab the connection is. */
	tab = (struct x11_tab *)connection;

	/* The close, for the network thread. */
	kl_mutex_lock(&tab->lock);

	tab->closed = 1;

	kl_mutex_unlock(&tab->lock);

	/* The network thread wakes to it, then the tab lets go. */
	tab_ring(tab);
	x11_tab_release(tab);
}

/*
 * Draws the window into the tab's pixels: at the X server's scale (whole
 * scales pixel by pixel, the others filtered), from the tab's corner,
 * the rest of the tab in the background's colour.  Returns 0.
 */
static int
tab_draw(
	struct terminal_connection *connection,
	uint32_t *pixels,
	uint32_t width,
	uint32_t height)
{
	struct x11_tab *tab;
	struct x11_win32_box box;
	unsigned scaled_width;
	unsigned scaled_height;
	size_t count;
	size_t index;
	int32_t x;
	int32_t y;
	int whole;

	/* The background everywhere first. */
	tab = (struct x11_tab *)connection;
	count = (size_t)width * (size_t)height;
	for (index = 0U; index < count; index++)
		pixels[index] = TAB_BACKGROUND;

	/* The window at the scale, as far as the tab reaches. */
	whole = x11_win32_scale_is_whole(tab->scale);

	kl_mutex_lock(&tab->lock);

	scaled_width = x11_win32_scale_size(tab->width, tab->scale);
	scaled_height = x11_win32_scale_size(tab->height, tab->scale);
	box.x0 = 0;
	box.y0 = 0;
	box.x1 = (int32_t)scaled_width;
	if (box.x1 > (int32_t)width)
		box.x1 = (int32_t)width;
	box.y1 = (int32_t)scaled_height;
	if (box.y1 > (int32_t)height)
		box.y1 = (int32_t)height;
	x11_win32_scale_pixels(tab->pixels, tab->width, tab->width, tab->height, pixels, width, scaled_width, scaled_height, whole, &box);

	kl_mutex_unlock(&tab->lock);

	/* The window's pixels shown opaque. */
	for (y = box.y0; y < box.y1; y++) {
		for (x = box.x0; x < box.x1; x++)
			pixels[(size_t)y * width + (size_t)x] |= TAB_OPAQUE;
	}

	/* Succeeded: the tab is drawn. */
	return 0;
}

/* Gives the tab's title: the host's name in brackets (a remote window cannot pass for one of this computer), then the window's. */
static void
tab_title(
	struct terminal_connection *connection,
	char *title,
	size_t size)
{
	struct x11_tab *tab;

	/* The tab the connection is. */
	tab = (struct x11_tab *)connection;

	/* The title as the window has it now. */
	kl_mutex_lock(&tab->lock);

	(void)snprintf(title, size, "[%s] %s", tab->host, tab->title);

	kl_mutex_unlock(&tab->lock);
}

/*
 * Takes one of the window's inputs for the X window: a key (a held key's
 * repeat is another press), text, the pointer's motion and buttons in X's
 * pixels, the wheels as notches, the focus, the pointer's leaving.
 */
static void
tab_input(
	struct terminal_connection *connection,
	const struct terminal_picture_event *event)
{
	struct x11_tab *tab;
	int32_t x;
	int32_t y;
	unsigned button;
	uint32_t pressed;

	/* The pointer's place on the window in X's pixels. */
	tab = (struct x11_tab *)connection;
	x = x11_win32_scale_to_x(event->x, tab->scale);
	y = x11_win32_scale_to_x(event->y, tab->scale);
	pressed = 0U;
	if (event->pressed)
		pressed = 1U;

	/* What it is. */
	switch (event->kind) {
	case TERMINAL_PICTURE_KEY:
		tab_queue(tab, X11_LINK_INPUT_KEY, event->code, pressed, 0U, NULL);
		break;
	case TERMINAL_PICTURE_TEXT:
		tab_queue(tab, X11_LINK_INPUT_TEXT, 0U, 0U, 0U, event->text);
		break;
	case TERMINAL_PICTURE_MOTION:
		tab_pointer(tab, 0U, 0, x, y);
		break;
	case TERMINAL_PICTURE_BUTTON:
		/* A button X has a number for. */
		button = tab_button(event->code);
		if (button != 0U)
			tab_pointer(tab, button, event->pressed, x, y);
		break;
	case TERMINAL_PICTURE_AXIS:
		/* Each wheel by whole notches; a positive distance goes down or right. */
		tab_wheel(tab, &tab->wheel_y, event->dy, TAB_X_WHEEL_UP, TAB_X_WHEEL_DOWN, x, y);
		tab_wheel(tab, &tab->wheel_x, event->dx, TAB_X_WHEEL_LEFT, TAB_X_WHEEL_RIGHT, x, y);
		break;
	case TERMINAL_PICTURE_FOCUS:
		tab_queue(tab, X11_LINK_INPUT_FOCUS, pressed, 0U, 0U, NULL);
		break;
	case TERMINAL_PICTURE_LEAVE:
		tab_queue(tab, X11_LINK_INPUT_LEAVE, 0U, 0U, 0U, NULL);
		break;
	case TERMINAL_PICTURE_TOUCH_DOWN:
	case TERMINAL_PICTURE_TOUCH_MOVE:
	case TERMINAL_PICTURE_TOUCH_UP:
	case TERMINAL_PICTURE_TOUCH_CANCEL: {
		struct x11_touch_action actions[8];
		unsigned touch_kind;
		unsigned count;
		unsigned i;
		int32_t ax;
		int32_t ay;

		touch_kind = X11_TOUCH_DOWN + (event->kind - TERMINAL_PICTURE_TOUCH_DOWN);
		count = x11_touch_take(&tab->touch, touch_kind, event->id, event->x, event->y, kl_clock_us(), actions, 8U);
		for (i = 0U; i < count; i++) {
			ax = x11_win32_scale_to_x(actions[i].x, tab->scale);
			ay = x11_win32_scale_to_x(actions[i].y, tab->scale);
			if (actions[i].kind == X11_TOUCH_MOTION) {
				tab_pointer(tab, 0U, 0, ax, ay);
			} else if (actions[i].kind == X11_TOUCH_BUTTON) {
				tab_pointer(tab, actions[i].button, actions[i].pressed, ax, ay);
			} else if (actions[i].kind == X11_TOUCH_WHEEL) {
				tab_wheel(tab, &tab->wheel_y, (double)actions[i].notches * TAB_WHEEL_NOTCH,
					  TAB_X_WHEEL_UP, TAB_X_WHEEL_DOWN, ax, ay);
			} else if (actions[i].kind == X11_TOUCH_PAN) {
				tab_queue(tab, X11_LINK_INPUT_PAN, 0U, (uint32_t)actions[i].dx, (uint32_t)actions[i].dy, NULL);
			} else if (actions[i].kind == X11_TOUCH_PINCH) {
				uint32_t ratio_q16;

				ratio_q16 = (uint32_t)(actions[i].scale_ratio * 65536.0 + 0.5);
				tab_queue(tab, X11_LINK_INPUT_PINCH, ratio_q16, (uint32_t)actions[i].x, (uint32_t)actions[i].y, NULL);
			}
		}
		break;
	}
	default:
		/* An input the window does not take. */
		return;
	}

	/* The network thread sends it. */
	tab_ring(tab);
}

/* Says that the run had no room for the tab: the child shows the window as a window of its own (the close follows). */
static void
tab_refuse(
	struct terminal_connection *connection)
{
	struct x11_tab *tab;

	/* The tab the connection is. */
	tab = (struct x11_tab *)connection;

	/* The refusal, for the network thread with the close. */
	kl_mutex_lock(&tab->lock);

	tab->refused = 1;

	kl_mutex_unlock(&tab->lock);
}

/*
 * Queues one input for the network thread: a move takes the place of a
 * move queued just before it, and a full queue drops the input.
 */
static void
tab_queue(
	struct x11_tab *tab,
	unsigned kind,
	uint32_t first,
	uint32_t second,
	uint32_t third,
	const char *text)
{
	struct x11_tab_input *input;
	unsigned count;

	/* After the inputs before it. */
	kl_mutex_lock(&tab->lock);

	count = tab->input_count;

	/* A move after a move: only the newest place counts. */
	if (kind == X11_LINK_INPUT_POINTER &&
	    first == 0U &&
	    count != 0U &&
	    tab->inputs[count - 1U].kind == X11_LINK_INPUT_POINTER &&
	    tab->inputs[count - 1U].numbers[0] == 0U) {
		tab->inputs[count - 1U].numbers[1] = second;
		tab->inputs[count - 1U].numbers[2] = third;
		kl_mutex_unlock(&tab->lock);
		return;
	}

	/* A full queue drops it. */
	if (count >= X11_TAB_INPUTS) {
		kl_mutex_unlock(&tab->lock);
		return;
	}

	/* The input. */
	input = &tab->inputs[count];
	memset(input, 0, sizeof(*input));
	input->kind = kind;
	input->numbers[0] = first;
	input->numbers[1] = second;
	input->numbers[2] = third;
	if (text != NULL)
		(void)snprintf(input->text, sizeof(input->text), "%s", text);
	tab->input_count = count + 1U;

	kl_mutex_unlock(&tab->lock);
}

/* Queues the pointer's input: a move (button 0) or a button pressed or released, at a place in X's pixels. */
static void
tab_pointer(
	struct x11_tab *tab,
	unsigned button,
	int pressed,
	int32_t x,
	int32_t y)
{
	uint32_t first;

	/* The button, and whether it went down. */
	first = button;
	if (pressed)
		first |= X11_LINK_INPUT_PRESSED;

	/* The input, the place as signed numbers. */
	tab_queue(tab, X11_LINK_INPUT_POINTER, first, (uint32_t)x, (uint32_t)y, NULL);
}

/*
 * Turns a wheel's distance into clicks of its buttons (back for a
 * negative distance, forward for a positive one): each whole notch a
 * press and a release, the rest kept for the next turn.
 */
static void
tab_wheel(
	struct x11_tab *tab,
	double *rest,
	double distance,
	unsigned back,
	unsigned forward,
	int32_t x,
	int32_t y)
{
	/* The distance with what the last turn left. */
	*rest += distance;

	/* Each whole notch forward. */
	while (*rest >= TAB_WHEEL_NOTCH) {
		tab_pointer(tab, forward, 1, x, y);
		tab_pointer(tab, forward, 0, x, y);
		*rest -= TAB_WHEEL_NOTCH;
	}

	/* Each whole notch back. */
	while (*rest <= -TAB_WHEEL_NOTCH) {
		tab_pointer(tab, back, 1, x, y);
		tab_pointer(tab, back, 0, x, y);
		*rest += TAB_WHEEL_NOTCH;
	}
}

/* Gives X's number of a pointer's button by its evdev code (0: a button X has none for). */
static unsigned
tab_button(
	uint32_t code)
{
	/* X numbers the left button 1, the middle 2, the right 3, the side buttons 8 and 9. */
	switch (code) {
	case TAB_BUTTON_LEFT:
		return 1U;
	case TAB_BUTTON_MIDDLE:
		return 2U;
	case TAB_BUTTON_RIGHT:
		return 3U;
	case TAB_BUTTON_SIDE:
		return TAB_X_BACK;
	case TAB_BUTTON_EXTRA:
		return TAB_X_FORWARD;
	default:
		break;
	}

	/* None. */
	return 0U;
}

/*
 * Copies a title the child sent (at most X11_LINK_TAB_TITLE_MAX bytes,
 * the link's check) as text that can be shown: UTF-8 kept, a byte that
 * is not part of a whole character a '?', a control character a space.
 */
static void
tab_clean_title(
	char *title,
	const uint8_t *bytes,
	size_t size)
{
	size_t offset;
	size_t written;
	size_t length;
	size_t index;
	uint8_t first;

	/* Each character in turn. */
	offset = 0U;
	written = 0U;
	while (offset < size && written < X11_LINK_TAB_TITLE_MAX) {
		first = bytes[offset];

		/* A single byte: a control character is a space. */
		if (first < 0x80U) {
			title[written] = (char)first;
			if (first < 0x20U || first == 0x7FU)
				title[written] = ' ';
			written++;
			offset++;
			continue;
		}

		/* The length a leading byte says (0: not a leading byte). */
		length = 0U;
		if (first >= 0xC2U && first <= 0xDFU)
			length = 2U;
		else if (first >= 0xE0U && first <= 0xEFU)
			length = 3U;
		else if (first >= 0xF0U && first <= 0xF4U)
			length = 4U;

		/* A whole character of continuation bytes, which fits. */
		if (length != 0U && (length > size - offset || written + length > X11_LINK_TAB_TITLE_MAX))
			length = 0U;
		for (index = 1U; index < length; index++) {
			if ((bytes[offset + index] & 0xC0U) != 0x80U) {
				length = 0U;
				break;
			}
		}

		/* A byte that starts no whole character. */
		if (length == 0U) {
			title[written] = '?';
			written++;
			offset++;
			continue;
		}

		/* The character as it is. */
		memcpy(title + written, bytes + offset, length);
		written += length;
		offset += length;
	}

	/* Ended. */
	title[written] = '\0';
}

/* Wakes the network thread of the tab's session, which sends what the tab has for the child. */
static void
tab_ring(
	struct x11_tab *tab)
{
	/* The session is the tab's from before the application's thread had the tab. */
	kl_socket_waker_ring(tab->session->waker);
}
