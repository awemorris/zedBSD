/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Calendar's window (WS155 p000, p003; calendar.h): a libkeiland
 * application with one window that shows the view (view.c), its menu
 * (File: Quit; View: Today, Previous Month, Next Month, Reduce Motion),
 * and the view's input; today is the system's day.  The events and memos
 * are read from ~/Documents/Calendar at the start (store.c), and while
 * the program runs an event with a time is told as a notification when it
 * starts.  Ctrl+Q quits.  What happens is logged on standard error as
 * "CALENDAR" lines for the tests.
 *
 *   calendar [--width=N] [--height=N] [--timeout-s=N] [--reduce-motion]
 */

#include "calendar.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* The fonts, the window's first size, and the longest wait for input. */
#define CAL_FONT			KEILAND_DATADIR "/fonts/keiland.ttf"
#define CAL_FALLBACK_FONT	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"
#define CAL_WIDTH		1280U
#define CAL_HEIGHT		800U
#define CAL_IDLE_MS		1000
#define CAL_MOVING_MS		10

/* A frame that takes longer than this is logged, in milliseconds (a lit widget's frame on the Pi 4 among them, ws210-p014). */
#define CAL_SLOW_FRAME_MS	100U

/* The most glass panels of a frame. */
#define CAL_PANELS_MAX		4U

/* The key Q, which quits with Ctrl. */
#define CAL_KEY_Q		16U

/* The folder of the store under the home. */
#define CAL_STORE_FOLDER	"Documents/Calendar"

/*
 * The window's state: the application, the window and its input, the
 * frame (its pixels, size and canvas), the text and the style, the view,
 * whether a frame is due, the window changed size, a widget moves, or the
 * view moves by itself, and whether the glass was decided.
 */
struct cal_window {
	struct kl_app *app;
	struct kl_window *window;
	struct kl_ui *ui;
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	struct kl_canvas canvas;
	int canvas_made;
	struct kl_text text;
	struct kl_style style;
	struct cal_view view;
	int dirty;

	/*
	 * Only the lit widget changed since the last frame (BUG-226): the next
	 * frame is drawn within the parts kl_ui_take_frame gives, when nothing
	 * else changed (dirty draws the whole window).
	 */
	int lit_changed;

	/* The pointer's buttons held now: while one is, a motion during a card's drag draws the whole window; otherwise only one that lights another widget draws its part (BUG-226). */
	unsigned buttons_held;

	/*
	 * A key or text came since the last press: a widget the keys lit may
	 * show its ring until a press takes the focus away, so that press draws
	 * the whole window (ws210-p014).
	 */
	int keyed;

	/* The left button was last pressed over a day's cell alone: its release there draws the cell alone (a widget pressed elsewhere is drawn again with the whole window). */
	int pressed_cell;

	/* Why the whole window is to be drawn, for the log of a slow frame (NULL: nothing told). */
	const char *why;
	int resized;
	int moving;
	int animating;
	int glass_decided;

	/* The minute of today whose starts were told last (-1 before the first look: nothing before the start is told). */
	int reminded_minute;
};

/* The window's menu. */
static const struct kl_menu_entry cal_menu[] = {
	{ 1U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "File", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 2U, 1U, KL_MENU_ITEM_NORMAL, "Quit Calendar", CAL_ACTION_QUIT, KL_MENU_ROLE_QUIT, KL_MENU_CTRL, 'q' },
	{ 3U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "View", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 4U, 3U, KL_MENU_ITEM_NORMAL, "Today", CAL_ACTION_TODAY, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 5U, 3U, KL_MENU_ITEM_NORMAL, "Previous Month", CAL_ACTION_PREVIOUS, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 6U, 3U, KL_MENU_ITEM_NORMAL, "Next Month", CAL_ACTION_NEXT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 7U, 3U, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 8U, 3U, KL_MENU_ITEM_CHECKBOX, "Reduce Motion", CAL_ACTION_MOTION, KL_MENU_ROLE_NONE, 0U, 0U }
};

int main(int argc, char **argv);
static int cal_parse(int argc, char **argv, unsigned *width, unsigned *height, unsigned *timeout, int *reduce_motion);
static void cal_today(struct cal_date *today);
static void cal_states(struct cal_window *calendar);
static int cal_loop(struct cal_window *calendar, unsigned timeout);
static void cal_input(struct cal_window *calendar, const struct kl_window_event *event);
static int cal_button_cell(struct cal_window *calendar, const struct kl_window_event *event, struct kl_rect *rect);
static void cal_dirty(struct cal_window *calendar, const char *why);
static unsigned cal_add_part(struct kl_rect *parts, unsigned count, const struct kl_rect *rect);
static int cal_resize(struct cal_window *calendar);
static void cal_draw(struct cal_window *calendar, uint64_t now_us);
static void cal_slow_frame(const struct cal_window *calendar, uint64_t started, uint64_t drawn, const struct kl_rect *parts, unsigned count, const char *why);
static int cal_wait(const struct cal_window *calendar, uint64_t now_us);
static void cal_store_start(void);
static void cal_remind(struct cal_window *calendar);

/*
 * Runs Calendar.
 */
int
main(
	int argc,
	char **argv)
{
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	static struct cal_window calendar;
	struct cal_date today;
	unsigned timeout;
	unsigned width;
	unsigned height;
	int reduce_motion;
	int status;
	int error;

	/* The command line. */
	status = cal_parse(argc, argv, &width, &height, &timeout, &reduce_motion);
	if (status != 0) {
		fprintf(stderr, "usage: calendar [--width=N] [--height=N] [--timeout-s=N] [--reduce-motion]\n");
		return 2;
	}

	/* The fonts; without them the view shows no words. */
	error = kl_text_open(&calendar.text, CAL_FONT, CAL_FALLBACK_FONT);
	if (error != 0)
		cal_log("FONT missing error=%d", error);

	/* The events and memos kept, then the view's state for today, its motion as asked. */
	cal_store_start();
	calendar.reminded_minute = -1;
	cal_today(&today);
	error = cal_view_init(&calendar.view, &today, kl_clock_us());
	if (error != 0) {
		cal_log("FAILED operation=view error=%d", error);
		return 1;
	}

	/* The motion, reduced when asked. */
	calendar.view.reduce_motion = reduce_motion;

	/* The application. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "calendar";
	calendar.app = kl_app_open(&app_options);
	if (calendar.app == NULL) {
		cal_log("FAILED operation=app error=%d", errno);
		cal_view_release(&calendar.view);
		return 1;
	}

	/* Its window. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Calendar";
	window_options.width = width;
	window_options.height = height;
	window_options.present = KL_PRESENT_VULKAN;
	calendar.window = kl_app_window_create(calendar.app, &window_options);
	if (calendar.window == NULL) {
		cal_log("FAILED operation=window error=%d", errno);
		kl_app_close(calendar.app);
		cal_view_release(&calendar.view);
		return 1;
	}

	/*
	 * The pointer's motions merged, and each frame drawn once the
	 * compositor took the last one (ws210-p014 F2): the pointer crossing
	 * the panes while a frame is on its way draws its latest place once,
	 * not a frame queued for every widget it crossed (2026-10-12, the Pi 4:
	 * a lit cell shown 300 ms late, after the pointer had gone).
	 */
	error = kl_window_set_pacing(calendar.window, KL_PACING_MOTION | KL_PACING_FRAME, 0U);
	cal_log("PACING motion=1 frame=1 errno=%d", error);

	/* The input of its frames. */
	calendar.ui = kl_ui_create();
	if (calendar.ui == NULL) {
		cal_log("FAILED operation=ui error=%d", errno);
		kl_app_close(calendar.app);
		cal_view_release(&calendar.view);
		return 1;
	}

	/* The menu and the style (opaque until the first frame finds whether the window can stand on glass). */
	(void)kl_window_set_menu(calendar.window, cal_menu, sizeof(cal_menu) / sizeof(cal_menu[0]));
	cal_states(&calendar);
	calendar.style.text = &calendar.text;
	calendar.style.theme = kl_theme_default();
	calendar.style.glass = 0;
	calendar.view.glass = 0;

	/* The loop until the window closes. */
	status = cal_loop(&calendar, timeout);

	/* Everything goes. */
	kl_ui_destroy(calendar.ui);
	if (calendar.canvas_made)
		kl_canvas_release(&calendar.canvas);
	free(calendar.pixels);
	kl_app_close(calendar.app);
	cal_view_release(&calendar.view);
	kl_text_close(&calendar.text);
	cal_store_close();

	/* Reports how the loop ended. */
	if (status != 0)
		return 1;

	/* Succeeded: the window closed. */
	return 0;
}

/*
 * Writes a log line for the tests on standard error.
 */
void
cal_log(
	const char *format,
	...)
{
	va_list arguments;

	/* The line. */
	va_start(arguments, format);
	fputs("CALENDAR ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

/*
 * Reads the command line; nonzero when it cannot be read.
 */
static int
cal_parse(
	int argc,
	char **argv,
	unsigned *width,
	unsigned *height,
	unsigned *timeout,
	int *reduce_motion)
{
	int index;
	int same;

	/* The defaults. */
	*width = CAL_WIDTH;
	*height = CAL_HEIGHT;
	*timeout = 0U;
	*reduce_motion = 0;

	/* Each argument. */
	for (index = 1; index < argc; index++) {
		/* The width. */
		same = strncmp(argv[index], "--width=", 8U);
		if (same == 0) {
			*width = (unsigned)strtoul(argv[index] + 8, NULL, 10);
			continue;
		}

		/* The height. */
		same = strncmp(argv[index], "--height=", 9U);
		if (same == 0) {
			*height = (unsigned)strtoul(argv[index] + 9, NULL, 10);
			continue;
		}

		/* The timeout. */
		same = strncmp(argv[index], "--timeout-s=", 12U);
		if (same == 0) {
			*timeout = (unsigned)strtoul(argv[index] + 12, NULL, 10);
			continue;
		}

		/* The motion reduced. */
		same = strcmp(argv[index], "--reduce-motion");
		if (same == 0) {
			*reduce_motion = 1;
			continue;
		}

		/* An argument not known. */
		return -1;
	}

	/* A window needs a size. */
	if (*width == 0U || *height == 0U)
		return -1;

	/* Succeeded: the command line is read. */
	return 0;
}

/*
 * Runs the window until it closes, Quit or the timeout; nonzero when
 * something failed.
 */
static int
cal_loop(
	struct cal_window *calendar,
	unsigned timeout)
{
	struct kl_app_event event;
	uint64_t started;
	uint64_t now;
	int animating;
	int status;
	int taken;
	int wait;

	/* The first frame. */
	status = cal_resize(calendar);
	if (status != 0)
		return -1;
	cal_log("READY width=%u height=%u", calendar->width, calendar->height);

	/* Each round: the input, then a frame when something changed. */
	started = kl_clock_us();
	for (;;) {
		/* Waits for the compositor, or for the time something moves. */
		now = kl_clock_us();
		wait = cal_wait(calendar, now);
		status = kl_app_dispatch(calendar->app, wait);
		if (status != 0) {
			cal_log("DONE reason=disconnected");
			return 0;
		}

		/* The window's input and the actions of its menu. */
		for (;;) {
			taken = kl_app_take(calendar->app, &event);
			if (!taken)
				break;

			/* The desktop's appearance changed: the theme's colours are new, the window is drawn again (ws089-p017). */
			if (event.kind == KL_APP_THEME) {
				cal_dirty(calendar, "theme");
				continue;
			}

			/* Another window's event is not this one's. */
			if (event.kind != KL_APP_WINDOW || event.window != calendar->window)
				continue;

			/* An action of the menu, or input. */
			if (event.input.kind == KL_WINDOW_ACTION) {
				cal_view_action(&calendar->view, event.input.code, kl_clock_us());
				cal_states(calendar);
				cal_dirty(calendar, "action");
			} else {
				cal_input(calendar, &event.input);
			}
		}

		/* The end: the window closed or Quit. */
		now = kl_clock_us();
		if (calendar->view.quit) {
			cal_log("DONE reason=close");
			return 0;
		}

		/* The timeout, when one was given. */
		if (timeout != 0U && now - started >= (uint64_t)timeout * 1000000U) {
			cal_log("DONE reason=timeout");
			return 0;
		}

		/* A new size. */
		if (calendar->resized) {
			calendar->resized = 0;
			status = cal_resize(calendar);
			if (status != 0)
				return -1;
		}

		/* The view's notice gone: drawn without it. */
		if (calendar->view.notice[0] != '\0' && now >= calendar->view.notice_until) {
			calendar->view.notice[0] = '\0';
			cal_dirty(calendar, "notice");
		}

		/* The events that start now. */
		cal_remind(calendar);

		/* Something of the view moving by itself (a page turning, a cell sinking, a drag): a frame at its pace; a notice waiting to go is no motion. */
		animating = cal_view_animating(&calendar->view);
		if (animating)
			calendar->animating = 1;

		/* A frame. */
		cal_draw(calendar, now);
	}
}

/*
 * Gives one input of the window to the view's widgets, or takes it as a
 * key of the window.
 */
static void
cal_input(
	struct cal_window *calendar,
	const struct kl_window_event *event)
{
	struct kl_rect cell;
	int on_cell;
	int redraw;
	int taken;

	/*
	 * Any input may change the view, but a motion of the pointer only when
	 * it lights another widget or drags, and the pointer leaving only the
	 * widget it lit (BUG-226: a frame for every motion left the pointer
	 * behind), and the left button over a day of the months only that
	 * day's cell (ws210-p014).
	 */
	if (event->kind != KL_WINDOW_MOTION &&
	    event->kind != KL_WINDOW_LEAVE &&
	    event->kind != KL_WINDOW_BUTTON)
		cal_dirty(calendar, "input");

	/* Each kind of input. */
	switch (event->kind) {
	case KL_WINDOW_MOTION:
		/*
		 * A drag of a card draws the whole window; another lit widget only
		 * its part (BUG-226), held button or not; a button held still over
		 * the same widget draws nothing.
		 */
		redraw = kl_ui_pointer_motion(calendar->ui, event->x, event->y);
		if (calendar->buttons_held != 0U && calendar->view.dragging >= 0)
			cal_dirty(calendar, "drag");
		else if (redraw)
			calendar->lit_changed = 1;
		break;
	case KL_WINDOW_LEAVE:
		/* The lit widget goes dark: its part alone, or the whole window while a button is held. */
		redraw = kl_ui_pointer_leave(calendar->ui);
		if (calendar->buttons_held != 0U)
			cal_dirty(calendar, "leave");
		else if (redraw)
			calendar->lit_changed = 1;
		break;
	case KL_WINDOW_BUTTON:
		/* The buttons held, for the motions of a drag. */
		if (event->pressed)
			calendar->buttons_held++;
		else if (calendar->buttons_held != 0U)
			calendar->buttons_held--;

		/* Whether it is the left button over a day's cell alone, found before a press takes the focus away. */
		on_cell = cal_button_cell(calendar, event, &cell);
		if (event->code == KL_BUTTON_LEFT && event->pressed) {
			calendar->pressed_cell = on_cell;
		} else if (event->code == KL_BUTTON_LEFT && !calendar->pressed_cell) {
			/* The release of a press made elsewhere: what it held is drawn again. */
			on_cell = 0;
		}
		if (event->pressed)
			calendar->keyed = 0;

		/* The left button presses the widgets. */
		redraw = kl_ui_pointer_motion(calendar->ui, event->x, event->y);
		if (event->code == KL_BUTTON_LEFT)
			(void)kl_ui_pointer_button(calendar->ui, event->pressed, event->arrival_us);

		/*
		 * Over a day's cell, a press changes nothing drawn and a release
		 * (a click, which may choose the day) draws the cell alone; the
		 * view draws again what choosing the day changes in the same
		 * frame (ws210-p014, 2026-10-12: the whole window drawn for the
		 * press, the click and the frame after it made a click show on
		 * the Pi 4 some 800 ms late).  Any other button, the whole window.
		 */
		if (!on_cell) {
			cal_dirty(calendar, "button");
		} else if (!event->pressed) {
			kl_ui_damage(calendar->ui, &cell);
			calendar->lit_changed = 1;
		} else if (redraw) {
			calendar->lit_changed = 1;
		}
		break;
	case KL_WINDOW_AXIS:
	case KL_WINDOW_AXIS_STOP:
		/* The wheel glides; a touch pad's fingers hold the content, and it flies on when they lift (BUG-211). */
		taken = kl_ui_axis(calendar->ui, event);
		if (taken == KL_UI_AXIS_FLUNG)
			cal_log("KINETIC fling source=finger");
		break;
	case KL_WINDOW_TOUCH_DOWN:
		(void)kl_ui_touch_down(calendar->ui, event->id, event->time_us, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_MOTION:
		(void)kl_ui_touch_motion(calendar->ui, event->id, event->time_us, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_UP:
		(void)kl_ui_touch_up(calendar->ui, event->id, event->time_us, event->arrival_us);
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		(void)kl_ui_touch_cancel(calendar->ui, event->arrival_us);
		break;
	case KL_WINDOW_KEY:
		/* Ctrl+Q quits; the other keys go to the widgets, and those no widget takes to the view. */
		calendar->keyed = 1;
		if (event->pressed &&
		    (event->modifiers & KL_MOD_CTRL) != 0U &&
		    event->code == CAL_KEY_Q)
			calendar->view.quit = 1;
		else
			(void)kl_ui_key(calendar->ui, event->code, event->pressed, event->modifiers);
		break;
	case KL_WINDOW_TEXT_COMMIT:
	case KL_WINDOW_TEXT_PREEDIT:
	case KL_WINDOW_TEXT_DELETE:
		/* Text from an input method or the on-screen keyboard, for the field with the keyboard (BUG-203, BUG-204). */
		calendar->keyed = 1;
		(void)kl_ui_text(calendar->ui, event);
		break;
	case KL_WINDOW_RESIZE:
		calendar->resized = 1;
		break;
	case KL_WINDOW_CLOSE:
		calendar->view.quit = 1;
		break;
	default:
		break;
	}
}

/*
 * Finds whether a button's press or release is the left button's over a
 * day's cell of the last frame, which changes nothing else (no card or
 * memo dragged, no field with the keyboard, no widget the keys lit): 1
 * with the cell's part, 0 otherwise.
 */
static int
cal_button_cell(
	struct cal_window *calendar,
	const struct kl_window_event *event,
	struct kl_rect *rect)
{
	int wanted;
	int found;

	/* Only the left button, with nothing dragged. */
	if (event->code != KL_BUTTON_LEFT)
		return 0;
	if (calendar->view.dragging >= 0)
		return 0;

	/* A field with the keyboard, or a widget the keys may have lit, loses the focus to a press. */
	wanted = kl_ui_text_wanted(calendar->ui, NULL);
	if (wanted)
		return 0;
	if (calendar->keyed)
		return 0;

	/* The cell under the pointer. */
	found = cal_view_cell_at(&calendar->view, event->x, event->y, rect);
	return found;
}

/*
 * Asks for the whole window in the next frame, and tells why for the log
 * of a slow frame (the first reason since the last frame).
 */
static void
cal_dirty(
	struct cal_window *calendar,
	const char *why)
{
	/* The first reason is kept. */
	if (calendar->why == NULL)
		calendar->why = why;

	/* The whole window. */
	calendar->dirty = 1;
}

/*
 * Adds a part to a frame's parts (KL_FRAME_PARTS_MAX at most; beyond them
 * merged into the last one) and returns how many there are.
 */
static unsigned
cal_add_part(
	struct kl_rect *parts,
	unsigned count,
	const struct kl_rect *rect)
{
	struct kl_rect *last;
	int right;
	int bottom;

	/* A part of its own, while there is room. */
	if (count < KL_FRAME_PARTS_MAX) {
		parts[count] = *rect;
		return count + 1U;
	}

	/* Otherwise the last part grows to hold it. */
	last = &parts[count - 1U];
	right = last->x + last->width;
	if (rect->x + rect->width > right)
		right = rect->x + rect->width;
	bottom = last->y + last->height;
	if (rect->y + rect->height > bottom)
		bottom = rect->y + rect->height;
	if (rect->x < last->x)
		last->x = rect->x;
	if (rect->y < last->y)
		last->y = rect->y;
	last->width = right - last->x;
	last->height = bottom - last->y;
	return count;
}

/*
 * Remakes the presenter and the canvas at the window's size; nonzero when
 * it cannot.
 */
static int
cal_resize(
	struct cal_window *calendar)
{
	uint32_t *pixels;
	int see_through;
	int status;

	/* The presenter at the window's size. */
	status = kl_window_present_resize(calendar->window, &calendar->width, &calendar->height);
	if (status != 0) {
		cal_log("FAILED operation=present error=%d", status);
		return -1;
	}

	/* The compositor's glass, when the frames are blended by their alpha (decided at the first size). */
	if (!calendar->glass_decided) {
		calendar->glass_decided = 1;
		see_through = kl_window_see_through(calendar->window);
		if (see_through) {
			calendar->style.glass = 1;
			calendar->view.glass = 1;
		}

		/* The log line the tests read. */
		cal_log("GLASS see_through=%d", see_through);
	}

	/* A frame's pixels of its size. */
	pixels = malloc((size_t)calendar->width * (size_t)calendar->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* The canvas on them, in place of the old one. */
	if (calendar->canvas_made)
		kl_canvas_release(&calendar->canvas);
	calendar->canvas_made = 0;
	free(calendar->pixels);
	calendar->pixels = pixels;
	status = kl_canvas_init(&calendar->canvas, calendar->pixels, (size_t)calendar->width, (int)calendar->width, (int)calendar->height);
	if (status != 0)
		return -1;

	/* Succeeded: drawn again at the new size. */
	calendar->canvas_made = 1;
	calendar->style.canvas = &calendar->canvas;
	cal_dirty(calendar, "resize");
	return 0;
}

/*
 * Draws and shows a frame when something changed or moves, then takes the
 * keys no widget took.
 */
static void
cal_draw(
	struct cal_window *calendar,
	uint64_t now_us)
{
	struct kl_glass_panel panels[CAL_PANELS_MAX];
	struct kl_event event;
	struct kl_rect parts[KL_FRAME_PARTS_MAX];
	struct kl_rect redraw[CAL_REDRAW_MAX];
	const char *why;
	uint64_t started;
	uint64_t drawn;
	unsigned part_count;
	unsigned index;
	size_t redraw_count;
	size_t count;
	int desk_only;
	int animating_before;
	int partial;
	int need;
	int pace;
	int status;
	int error;
	int taken;

	/* Nothing changed and nothing moves: no frame. */
	if (!calendar->dirty &&
	    !calendar->moving &&
	    !calendar->animating &&
	    !calendar->lit_changed)
		return;

	/*
	 * A frame once the compositor took the last one (ws210-p014 F2.2):
	 * until then nothing is consumed, so what changes meanwhile is drawn
	 * as one frame in its latest state.
	 */
	pace = kl_window_frame_wait(calendar->window, now_us);
	if (pace != 0)
		return;

	/* Only the desk calendar moving (no input, no widget moving): its frame alone, cheaper than the whole view. */
	started = kl_clock_us();
	desk_only = 0;
	if (!calendar->dirty && !calendar->moving && calendar->animating)
		desk_only = cal_view_desk_only(&calendar->view);
	animating_before = calendar->animating;
	calendar->animating = 0;
	if (desk_only && !calendar->lit_changed) {
		/* Drawn and shown within the desk calendar's area alone (ws210-p014: the whole window was shown for each of its frames). */
		cal_view_draw_desk(&calendar->view, &calendar->style, now_us);
		drawn = kl_clock_us();
		status = kl_window_present_part(calendar->window, calendar->pixels, (size_t)calendar->width, &calendar->view.desk_area);
		if (status == EAGAIN)
			calendar->resized = 1;
		cal_slow_frame(calendar, started, drawn, &calendar->view.desk_area, 1U, "desk");
		return;
	}

	/*
	 * Only another widget lit: the frame is drawn within the parts the
	 * widgets tell alone, the rest keeping its pixels (BUG-226,
	 * ws210-p014: the widget lit when the last frame was drawn and the one
	 * lit now; none when the pointer came back; the whole window when the
	 * parts cannot be told).  While the desk calendar's page turns, its
	 * area is one more part; while anything else moves, the whole window
	 * is drawn.
	 */
	partial = 0;
	if (!calendar->dirty &&
	    !calendar->moving &&
	    calendar->lit_changed) {
		if (!animating_before)
			partial = 1;
		if (desk_only)
			partial = 1;
	}
	if (partial && desk_only)
		kl_ui_damage(calendar->ui, &calendar->view.desk_area);
	part_count = 0;
	if (partial) {
		need = kl_ui_take_frame(calendar->ui, parts, KL_FRAME_PARTS_MAX, &part_count);
		if (need == KL_FRAME_NONE) {
			calendar->lit_changed = 0;
			return;
		}
		if (need == KL_FRAME_WHOLE)
			part_count = 0;
	}
	calendar->lit_changed = 0;

	/* Why the frame is drawn, for the log of a slow one: in parts, or the whole window for its first reason. */
	why = "parts";
	if (part_count == 0U) {
		why = calendar->why;
		if (why == NULL) {
			/* No input asked for it: something moving, the view's motion, or a lit widget without a part. */
			if (calendar->moving)
				why = "moving";
			else if (animating_before)
				why = "animating";
			else
				why = "lit";
		}
	}
	calendar->why = NULL;

	/* The view, all of it. */
	calendar->dirty = 0;
	if (part_count == 0U) {
		kl_ui_begin(calendar->ui, now_us);
		cal_view_draw(&calendar->view, calendar->ui, &calendar->style, (int)calendar->width, (int)calendar->height, now_us);
		calendar->moving = kl_ui_end(calendar->ui, now_us);
	}

	/* Or within each part in turn, the rest keeping the last frame's pixels. */
	for (index = 0; index < part_count; index++) {
		kl_canvas_clip_push(&calendar->canvas, &parts[index]);
		kl_ui_begin(calendar->ui, now_us);
		cal_view_draw(&calendar->view, calendar->ui, &calendar->style, (int)calendar->width, (int)calendar->height, now_us);
		calendar->moving = kl_ui_end(calendar->ui, now_us);
		kl_canvas_clip_pop(&calendar->canvas);
	}

	/*
	 * A click that chose another day: what it changed of what was drawn
	 * before it (the old day's cell, the card of the day chosen) drawn
	 * again now within those parts, which a frame of parts shows too
	 * (ws210-p014).
	 */
	redraw_count = calendar->view.redraw_count;
	memcpy(redraw, calendar->view.redraw, redraw_count * sizeof(redraw[0]));
	calendar->view.redraw_count = 0;
	for (index = 0; index < (unsigned)redraw_count; index++) {
		kl_canvas_clip_push(&calendar->canvas, &redraw[index]);
		kl_ui_begin(calendar->ui, now_us);
		cal_view_draw(&calendar->view, calendar->ui, &calendar->style, (int)calendar->width, (int)calendar->height, now_us);
		calendar->moving = kl_ui_end(calendar->ui, now_us);
		kl_canvas_clip_pop(&calendar->canvas);
		if (part_count != 0U)
			part_count = cal_add_part(parts, part_count, &redraw[index]);
	}

	/* A click that changed something else of what was drawn before it: the whole view again, in the next frame. */
	if (calendar->view.again) {
		calendar->view.again = 0;
		cal_dirty(calendar, "again");
	}

	/*
	 * The text input is asked for while a field has the keyboard, and told
	 * where its caret is, so that an input method's candidates and the
	 * on-screen keyboard stay out of its way; the widgets' Cut, Copy and
	 * Paste use the window's clipboard (ws190-p002: the bar of the fingers'
	 * selection too).
	 */
	kl_ui_window_text(calendar->ui, calendar->window);

	/* The glass's panels for the frame; a compositor without glass leaves the window opaque from the next one. */
	if (calendar->view.glass) {
		count = cal_view_panels(&calendar->view, (int)calendar->width, (int)calendar->height, panels, CAL_PANELS_MAX);
		error = kl_window_set_glass(calendar->window, panels, count);
		if (error != 0) {
			cal_log("GLASS failed error=%d", error);
			calendar->view.glass = 0;
			calendar->style.glass = 0;
			cal_dirty(calendar, "glass");
		}
	}

	/* The frame shown, by its parts alone when it was drawn so (BUG-221). */
	drawn = kl_clock_us();
	status = kl_window_present_parts(calendar->window, calendar->pixels, (size_t)calendar->width, parts, part_count);
	if (status == EAGAIN)
		calendar->resized = 1;
	if (redraw_count != 0U && part_count != 0U)
		why = "day";
	cal_slow_frame(calendar, started, drawn, parts, part_count, why);

	/* The frame of a click that chose another day, logged with its times however long it took (the Pi 4's measure of ws210-p014). */
	if (redraw_count != 0U)
		cal_log("DAY-FRAME draw=%lu present=%lu parts=%u", (unsigned long)((drawn - started) / 1000U), (unsigned long)((kl_clock_us() - drawn) / 1000U), part_count);

	/* The keys no widget took, and the input that met no widget. */
	for (;;) {
		taken = kl_ui_take(calendar->ui, &event);
		if (!taken)
			break;

		/* A key is the view's; it draws again. */
		if (event.kind == KL_EVENT_KEY) {
			cal_view_key(&calendar->view, event.code, event.modifiers, now_us);
			cal_dirty(calendar, "key");
		}
	}
}

/*
 * Logs a frame that took long (a diagnostic of the Pi 4, ws210-p014): its
 * drawing's and its showing's times, the presenter's parts of them, the
 * parts it drew (none: the whole window) with their area in pixels, and why
 * it was drawn (parts, the desk calendar, a day chosen, or the first reason
 * for the whole window: input, button, key, again, moving, animating...).
 */
static void
cal_slow_frame(
	const struct cal_window *calendar,
	uint64_t started,
	uint64_t drawn,
	const struct kl_rect *parts,
	unsigned count,
	const char *why)
{
	struct kl_present_times times;
	unsigned long area;
	uint64_t shown;
	unsigned index;

	/* A frame that did not take long. */
	shown = kl_clock_us();
	if (shown - started <= (uint64_t)CAL_SLOW_FRAME_MS * 1000U)
		return;

	/* The pixels drawn: the parts', or the whole window's. */
	area = (unsigned long)calendar->width * calendar->height;
	if (count != 0U)
		area = 0UL;
	for (index = 0; index < count; index++)
		area += (unsigned long)parts[index].width * (unsigned long)parts[index].height;

	/* One line. */
	kl_window_present_times(calendar->window, &times);
	cal_log("SLOW-FRAME draw=%lu present=%lu copy=%u acquire=%u queue=%u wait=%u parts=%u area=%lu why=%s", (unsigned long)((drawn - started) / 1000U), (unsigned long)((shown - drawn) / 1000U), times.copy_ms, times.acquire_ms, times.present_ms, times.wait_ms, count, area, why);
}

/*
 * Reports how long the loop may wait for input (ms): no time while a frame
 * is due, a frame's time while something moves, until the compositor took
 * the last frame (ws210-p014 F2.6: its answer wakes the wait), until the
 * view's notice goes, or a second.
 */
static int
cal_wait(
	const struct cal_window *calendar,
	uint64_t now_us)
{
	int animating;
	int pace;
	int view;
	int wait;

	/* A frame wanted: whether it may be drawn now, or how long the compositor's answer is waited for at most (-1: until it comes). */
	animating = cal_view_animating(&calendar->view);
	pace = -1;
	if (calendar->dirty ||
	    calendar->lit_changed ||
	    calendar->moving ||
	    animating)
		pace = kl_window_frame_wait(calendar->window, now_us);

	/* A frame due now. */
	if (pace == 0 && (calendar->dirty || calendar->lit_changed))
		return 0;

	/* Something moving: a frame at its pace. */
	if (pace == 0 && calendar->moving)
		return CAL_MOVING_MS;

	/* The view's own time (its motion's next frame only when a frame may be drawn, the notice's end), or a second. */
	wait = CAL_IDLE_MS;
	view = -1;
	if (pace == 0 || !animating)
		view = cal_view_wait(&calendar->view, now_us);
	if (view >= 0 && view < wait)
		wait = view;

	/* The compositor's answer, waited for at most so long. */
	if (pace > 0 && pace < wait)
		wait = pace;

	/* The time to wait. */
	return wait;
}

/*
 * Finds today in the system's local time.
 */
static void
cal_today(
	struct cal_date *today)
{
	struct tm local;
	time_t now;

	/* The time now, in the local time zone. */
	now = time(NULL);
	memset(&local, 0, sizeof(local));
	(void)localtime_r(&now, &local);

	/* Its day. */
	today->year = local.tm_year + 1900;
	today->month = local.tm_mon + 1;
	today->day = local.tm_mday;
}

/*
 * Shows the menu's check of Reduce Motion as the view has it.
 */
static void
cal_states(
	struct cal_window *calendar)
{
	unsigned state;

	/* Checked while the motion is reduced. */
	state = 0;
	if (calendar->view.reduce_motion)
		state = KL_ACTION_CHECKED;
	(void)kl_window_set_action_state(calendar->window, CAL_ACTION_MOTION, state);
}

/* Opens the store under the home's Documents (an empty one when it cannot). */
static void
cal_store_start(void)
{
	char root[1024];
	const char *home;
	int error;

	/* The home's Documents and Calendar in it. */
	home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		home = "/tmp";
	(void)snprintf(root, sizeof(root), "%s/Documents", home);
	(void)mkdir(root, 0700);
	(void)snprintf(root, sizeof(root), "%s/%s", home, CAL_STORE_FOLDER);

	/* Everything kept there. */
	error = cal_store_open(root);
	cal_log("STORE error=%d", error);
}

/*
 * Tells each event of today with a time that has started since the last
 * look, as a notification (its title, and when it starts).
 */
static void
cal_remind(
	struct cal_window *calendar)
{
	const struct cal_item *items;
	struct tm local;
	char line[64];
	size_t count;
	size_t index;
	time_t now;
	int minute;

	/* The minute of today now. */
	now = time(NULL);
	(void)localtime_r(&now, &local);
	minute = local.tm_hour * 60 + local.tm_min;

	/* The first look, or a new day: only what starts from now on is told. */
	if (calendar->reminded_minute < 0 || minute < calendar->reminded_minute) {
		calendar->reminded_minute = minute;
		return;
	}

	/* The same minute: told already. */
	if (minute == calendar->reminded_minute)
		return;

	/* Each event of today starting after the last look and by now. */
	items = cal_items(&count);
	for (index = 0; index < count; index++) {
		if (items[index].memo || items[index].all_day)
			continue;
		if (items[index].date.year != local.tm_year + 1900 || items[index].date.month != local.tm_mon + 1 || items[index].date.day != local.tm_mday)
			continue;
		if (items[index].start <= calendar->reminded_minute || items[index].start > minute)
			continue;

		/* Told. */
		(void)snprintf(line, sizeof(line), "Starts at %02d:%02d (%s)", items[index].start / 60, items[index].start % 60, cal_list_name(items[index].list));
		(void)kl_app_notify(calendar->app, items[index].title, line);
		cal_log("REMIND index=%lu start=%d", (unsigned long)index, items[index].start);
	}

	/* Looked up to now. */
	calendar->reminded_minute = minute;
}
