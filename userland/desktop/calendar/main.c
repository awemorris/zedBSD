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
	 * frame is drawn within the part kl_ui_take_damage gives, when nothing
	 * else changed (dirty draws the whole window).
	 */
	int lit_changed;

	/* The pointer's buttons held now: while one is, every motion is drawn (a drag); otherwise only one that lights another widget (BUG-226). */
	unsigned buttons_held;
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
static int cal_resize(struct cal_window *calendar);
static void cal_draw(struct cal_window *calendar, uint64_t now_us);
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
	int status;
	int taken;
	int wait;
	int pace;

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
				calendar->dirty = 1;
				continue;
			}

			/* Another window's event is not this one's. */
			if (event.kind != KL_APP_WINDOW || event.window != calendar->window)
				continue;

			/* An action of the menu, or input. */
			if (event.input.kind == KL_WINDOW_ACTION) {
				cal_view_action(&calendar->view, event.input.code, kl_clock_us());
				cal_states(calendar);
				calendar->dirty = 1;
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
			calendar->dirty = 1;
		}

		/* The events that start now. */
		cal_remind(calendar);

		/* Something of the view moving by itself (a page turning, a cell sinking, a drag): a frame at its pace. */
		pace = cal_view_wait(&calendar->view, now);
		if (pace >= 0)
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
	int redraw;
	int taken;

	/*
	 * Any input may change the view, but a motion of the pointer only when
	 * it lights another widget or drags (BUG-226: a frame for every motion
	 * left the pointer behind).
	 */
	if (event->kind != KL_WINDOW_MOTION)
		calendar->dirty = 1;

	/* Each kind of input. */
	switch (event->kind) {
	case KL_WINDOW_MOTION:
		/* A drag draws the whole window; another lit widget only its part (BUG-226). */
		redraw = kl_ui_pointer_motion(calendar->ui, event->x, event->y);
		if (calendar->buttons_held != 0U)
			calendar->dirty = 1;
		else if (redraw)
			calendar->lit_changed = 1;
		break;
	case KL_WINDOW_LEAVE:
		(void)kl_ui_pointer_leave(calendar->ui);
		break;
	case KL_WINDOW_BUTTON:
		/* The buttons held, for the motions of a drag. */
		if (event->pressed)
			calendar->buttons_held++;
		else if (calendar->buttons_held != 0U)
			calendar->buttons_held--;

		/* The left button presses the widgets. */
		(void)kl_ui_pointer_motion(calendar->ui, event->x, event->y);
		if (event->code == KL_BUTTON_LEFT)
			(void)kl_ui_pointer_button(calendar->ui, event->pressed, event->arrival_us);
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
	calendar->dirty = 1;
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
	const struct kl_rect *present_part;
	struct kl_rect part;
	size_t count;
	int desk_only;
	int animating_before;
	int partial;
	int status;
	int error;
	int taken;

	/* Nothing changed and nothing moves: no frame. */
	if (!calendar->dirty &&
	    !calendar->moving &&
	    !calendar->animating &&
	    !calendar->lit_changed)
		return;

	/* Only the desk calendar moving (no input, no widget moving, no widget lit): its frame alone, cheaper than the whole view. */
	desk_only = 0;
	if (!calendar->dirty && !calendar->moving && !calendar->lit_changed)
		desk_only = cal_view_desk_only(&calendar->view);
	animating_before = calendar->animating;
	calendar->animating = 0;
	if (desk_only) {
		cal_view_draw_desk(&calendar->view, &calendar->style, now_us);
		status = kl_window_present(calendar->window, calendar->pixels, (size_t)calendar->width);
		if (status == EAGAIN)
			calendar->resized = 1;
		return;
	}

	/*
	 * Only another widget lit: the frame is drawn within the two widgets'
	 * part alone, the rest keeping its pixels (BUG-226; the whole window
	 * when the part cannot be told, or while the desk calendar moves too).
	 */
	partial = 0;
	if (!calendar->dirty &&
	    !calendar->moving &&
	    !animating_before &&
	    calendar->lit_changed)
		partial = kl_ui_take_damage(calendar->ui, &part);
	calendar->lit_changed = 0;

	/* The view, within the part when there is one. */
	calendar->dirty = 0;
	if (partial)
		kl_canvas_clip_push(&calendar->canvas, &part);
	kl_ui_begin(calendar->ui, now_us);
	cal_view_draw(&calendar->view, calendar->ui, &calendar->style, (int)calendar->width, (int)calendar->height, now_us);
	calendar->moving = kl_ui_end(calendar->ui, now_us);
	if (partial)
		kl_canvas_clip_pop(&calendar->canvas);

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
			calendar->dirty = 1;
		}
	}

	/* The frame shown, by the part alone when it was drawn so (BUG-221). */
	present_part = NULL;
	if (partial)
		present_part = &part;
	status = kl_window_present_part(calendar->window, calendar->pixels, (size_t)calendar->width, present_part);
	if (status == EAGAIN)
		calendar->resized = 1;

	/* The keys no widget took, and the input that met no widget. */
	for (;;) {
		taken = kl_ui_take(calendar->ui, &event);
		if (!taken)
			break;

		/* A key is the view's; it draws again. */
		if (event.kind == KL_EVENT_KEY) {
			cal_view_key(&calendar->view, event.code, event.modifiers, now_us);
			calendar->dirty = 1;
		}
	}
}

/*
 * Reports how long the loop may wait for input (ms): no time while a frame
 * is due, a frame's time while something moves, until the view's notice
 * goes, or a second.
 */
static int
cal_wait(
	const struct cal_window *calendar,
	uint64_t now_us)
{
	int wait;

	/* A frame due now. */
	if (calendar->dirty || calendar->lit_changed)
		return 0;

	/* Something moving. */
	if (calendar->moving)
		return CAL_MOVING_MS;

	/* The view's notice, or a second. */
	wait = cal_view_wait(&calendar->view, now_us);
	if (wait < 0 || wait > CAL_IDLE_MS)
		wait = CAL_IDLE_MS;

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
