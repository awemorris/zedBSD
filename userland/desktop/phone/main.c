/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Phone's window (WS170 p000, p003; phone.h): a libkeiland application
 * with one window that shows the view (view.c), its menu (File: Quit;
 * Conversation: Call, Send Message, Attach File...), and the view's input.
 * Ctrl+Q quits.
 *
 * The contacts and their timelines are read from ~/Documents/Phone at the
 * start (store.c).  What the view asks the window carries out: a message
 * written is kept as "sending" and sent through the compositor's phone
 * (kl_system_phone_send, WS170 p004), its state kept as the phone tells
 * it; a call is made and kept with how it ended; a new contact is kept; a
 * contact's messages are marked read when it is shown.  A message that
 * comes is kept under its sender's contact (a new one for a number not
 * known) and told as a notification.  What happens is logged on standard
 * error as "PHONE" lines for the tests, never the numbers or the words.
 *
 *   phone [--width=N] [--height=N] [--timeout-s=N]
 */

#include "phone.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* The fonts, the window's first size, and the longest wait for input. */
#define PH_FONT			KEILAND_DATADIR "/fonts/keiland.ttf"
#define PH_FALLBACK_FONT	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"
#define PH_WIDTH		980U
#define PH_HEIGHT		660U
#define PH_IDLE_MS		1000
#define PH_MOVING_MS		10

/* The most glass panels of a frame. */
#define PH_PANELS_MAX		4U

/* The key Q, which quits with Ctrl. */
#define PH_KEY_Q		16U

/* The most requests to the phone waiting for their states, and the folder of the store under the home. */
#define PH_PENDING_MAX		32U
#define PH_STORE_FOLDER		"Documents/Phone"

/*
 * The window's state: the application, the window and its input, the
 * frame (its pixels, size and canvas), the text and the style, the view,
 * whether a frame is due, the window changed size, or something moves, and
 * whether the glass was decided.
 */
struct ph_window {
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
	struct ph_view view;
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
	int glass_decided;

	/*
	 * The compositor's phone (NULL when the compositor has none), and the
	 * requests sent that wait for their states: the request's number, the
	 * contact and the item it is about (a request of 0 is a free row).
	 */
	struct kl_system *system;
	struct ph_pending {
		uint32_t request;
		long contact;
		size_t item;
	} pending[PH_PENDING_MAX];
};

/* The window's menu. */
static const struct kl_menu_entry ph_menu[] = {
	{ 1U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "File", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 2U, 1U, KL_MENU_ITEM_NORMAL, "Quit Phone", PH_ACTION_QUIT, KL_MENU_ROLE_QUIT, KL_MENU_CTRL, 'q' },
	{ 3U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Conversation", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 4U, 3U, KL_MENU_ITEM_NORMAL, "Call", PH_ACTION_CALL, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 5U, 3U, KL_MENU_ITEM_NORMAL, "Send Message", PH_ACTION_SEND, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 6U, 3U, KL_MENU_ITEM_NORMAL, "Attach File...", PH_ACTION_ATTACH, KL_MENU_ROLE_NONE, 0U, 0U }
};

int main(int argc, char **argv);
static int ph_parse(int argc, char **argv, unsigned *width, unsigned *height, unsigned *timeout);
static int ph_loop(struct ph_window *phone, unsigned timeout);
static void ph_input(struct ph_window *phone, const struct kl_window_event *event);
static int ph_resize(struct ph_window *phone);
static void ph_draw(struct ph_window *phone, uint64_t now_us);
static int ph_wait(const struct ph_window *phone, uint64_t now_us);
static void ph_store_start(void);
static void ph_phone_round(struct ph_window *phone);
static void ph_received(struct ph_window *phone, const struct kl_phone_event *event);
static void ph_status(struct ph_window *phone, uint32_t request, unsigned state, int failed);
static void ph_requests(struct ph_window *phone);
static void ph_send(struct ph_window *phone, long contact);
static void ph_call(struct ph_window *phone, long contact);
static void ph_save_contact(struct ph_window *phone);
static void ph_remember(struct ph_window *phone, uint32_t request, long contact, size_t item);

/*
 * Runs Phone.
 */
int
main(
	int argc,
	char **argv)
{
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	static struct ph_window phone;
	unsigned capabilities;
	unsigned timeout;
	unsigned width;
	unsigned height;
	int status;
	int error;

	/* The command line. */
	status = ph_parse(argc, argv, &width, &height, &timeout);
	if (status != 0) {
		fprintf(stderr, "usage: phone [--width=N] [--height=N] [--timeout-s=N]\n");
		return 2;
	}

	/* The fonts; without them the view shows no words. */
	error = kl_text_open(&phone.text, PH_FONT, PH_FALLBACK_FONT);
	if (error != 0)
		ph_log("FONT missing error=%d", error);

	/* The contacts and their timelines, then the view's state (the latest contact shown). */
	ph_store_start();
	error = ph_view_init(&phone.view);
	if (error != 0) {
		ph_log("FAILED operation=view error=%d", error);
		return 1;
	}

	/* The application. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "phone";
	phone.app = kl_app_open(&app_options);
	if (phone.app == NULL) {
		ph_log("FAILED operation=app error=%d", errno);
		ph_view_release(&phone.view);
		return 1;
	}

	/* Its window, and the input of its frames. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Phone";
	window_options.width = width;
	window_options.height = height;
	window_options.present = KL_PRESENT_VULKAN;
	phone.window = kl_app_window_create(phone.app, &window_options);
	phone.ui = kl_ui_create();
	if (phone.window == NULL || phone.ui == NULL) {
		ph_log("FAILED operation=window error=%d", errno);
		kl_ui_destroy(phone.ui);
		kl_app_close(phone.app);
		ph_view_release(&phone.view);
		return 1;
	}

	/* The menu and the style (opaque until the first frame finds whether the window can stand on glass). */
	(void)kl_window_set_menu(phone.window, ph_menu, sizeof(ph_menu) / sizeof(ph_menu[0]));
	phone.style.text = &phone.text;
	phone.style.theme = kl_theme_default();
	phone.style.glass = 0;
	phone.view.glass = 0;

	/* The compositor's phone. */
	phone.system = kl_app_system(phone.app);
	if (phone.system != NULL) {
		capabilities = kl_system_capabilities(phone.system);
		if ((capabilities & KL_SYSTEM_HAS_PHONE) == 0U)
			phone.system = NULL;
	}

	/* The log the tests read. */
	ph_log("SYSTEM phone=%d", phone.system != NULL);

	/* The loop until the window closes. */
	status = ph_loop(&phone, timeout);

	/* Everything goes. */
	kl_ui_destroy(phone.ui);
	if (phone.canvas_made)
		kl_canvas_release(&phone.canvas);
	free(phone.pixels);
	kl_app_close(phone.app);
	ph_view_release(&phone.view);
	kl_text_close(&phone.text);
	ph_store_close();

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
ph_log(
	const char *format,
	...)
{
	va_list arguments;

	/* The line. */
	va_start(arguments, format);
	fputs("PHONE ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

/*
 * Reads the command line; nonzero when it cannot be read.
 */
static int
ph_parse(
	int argc,
	char **argv,
	unsigned *width,
	unsigned *height,
	unsigned *timeout)
{
	int index;
	int same;

	/* The defaults. */
	*width = PH_WIDTH;
	*height = PH_HEIGHT;
	*timeout = 0U;

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
ph_loop(
	struct ph_window *phone,
	unsigned timeout)
{
	struct kl_app_event event;
	uint64_t started;
	uint64_t now;
	int status;
	int taken;
	int wait;

	/* The first frame. */
	status = ph_resize(phone);
	if (status != 0)
		return -1;
	ph_log("READY width=%u height=%u", phone->width, phone->height);

	/* Each round: the input, then a frame when something changed. */
	started = kl_clock_us();
	for (;;) {
		/* Waits for the compositor, or for the time something moves. */
		now = kl_clock_us();
		wait = ph_wait(phone, now);
		status = kl_app_dispatch(phone->app, wait);
		if (status != 0) {
			ph_log("DONE reason=disconnected");
			return 0;
		}

		/* The window's input and the actions of its menu. */
		for (;;) {
			taken = kl_app_take(phone->app, &event);
			if (!taken)
				break;

			/* The desktop's appearance changed: the theme's colours are new, the window is drawn again (ws089-p017). */
			if (event.kind == KL_APP_THEME) {
				phone->dirty = 1;
				continue;
			}

			/* Another window's event is not this one's. */
			if (event.kind != KL_APP_WINDOW || event.window != phone->window)
				continue;

			/* An action of the menu, or input. */
			if (event.input.kind == KL_WINDOW_ACTION) {
				ph_view_action(&phone->view, event.input.code, kl_clock_us());
				phone->dirty = 1;
			} else {
				ph_input(phone, &event.input);
			}
		}

		/* The phone's events, and what the view asked. */
		ph_phone_round(phone);
		ph_requests(phone);

		/* The end: the window closed or Quit. */
		now = kl_clock_us();
		if (phone->view.quit) {
			ph_log("DONE reason=close");
			return 0;
		}

		/* The timeout, when one was given. */
		if (timeout != 0U && now - started >= (uint64_t)timeout * 1000000U) {
			ph_log("DONE reason=timeout");
			return 0;
		}

		/* A new size. */
		if (phone->resized) {
			phone->resized = 0;
			status = ph_resize(phone);
			if (status != 0)
				return -1;
		}

		/* The view's notice gone: drawn without it. */
		if (phone->view.notice != NULL && now >= phone->view.notice_until) {
			phone->view.notice = NULL;
			phone->dirty = 1;
		}

		/* A frame, then what its buttons asked. */
		ph_draw(phone, now);
		ph_requests(phone);
	}
}

/*
 * Gives one input of the window to the view's widgets, or takes it as a
 * key of the window.
 */
static void
ph_input(
	struct ph_window *phone,
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
		phone->dirty = 1;

	/* Each kind of input. */
	switch (event->kind) {
	case KL_WINDOW_MOTION:
		/* A drag draws the whole window; another lit widget only its part (BUG-226). */
		redraw = kl_ui_pointer_motion(phone->ui, event->x, event->y);
		if (phone->buttons_held != 0U)
			phone->dirty = 1;
		else if (redraw)
			phone->lit_changed = 1;
		break;
	case KL_WINDOW_LEAVE:
		(void)kl_ui_pointer_leave(phone->ui);
		break;
	case KL_WINDOW_BUTTON:
		/* The buttons held, for the motions of a drag. */
		if (event->pressed)
			phone->buttons_held++;
		else if (phone->buttons_held != 0U)
			phone->buttons_held--;

		/* The left button presses the widgets. */
		(void)kl_ui_pointer_motion(phone->ui, event->x, event->y);
		if (event->code == KL_BUTTON_LEFT)
			(void)kl_ui_pointer_button(phone->ui, event->pressed, event->arrival_us);
		break;
	case KL_WINDOW_AXIS:
	case KL_WINDOW_AXIS_STOP:
		/* The wheel glides; a touch pad's fingers hold the content, and it flies on when they lift (BUG-211). */
		taken = kl_ui_axis(phone->ui, event);
		if (taken == KL_UI_AXIS_FLUNG)
			ph_log("KINETIC fling source=finger");
		break;
	case KL_WINDOW_TOUCH_DOWN:
		(void)kl_ui_touch_down(phone->ui, event->id, event->time_us, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_MOTION:
		(void)kl_ui_touch_motion(phone->ui, event->id, event->time_us, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_UP:
		(void)kl_ui_touch_up(phone->ui, event->id, event->time_us, event->arrival_us);
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		(void)kl_ui_touch_cancel(phone->ui, event->arrival_us);
		break;
	case KL_WINDOW_KEY:
		/* Ctrl+Q quits; the other keys go to the widgets, and those no widget takes to the view. */
		if (event->pressed &&
		    (event->modifiers & KL_MOD_CTRL) != 0U &&
		    event->code == PH_KEY_Q)
			phone->view.quit = 1;
		else
			(void)kl_ui_key(phone->ui, event->code, event->pressed, event->modifiers);
		break;
	case KL_WINDOW_TEXT_COMMIT:
	case KL_WINDOW_TEXT_PREEDIT:
	case KL_WINDOW_TEXT_DELETE:
		/* Text from an input method or the on-screen keyboard, for the field with the keyboard (BUG-203, BUG-204). */
		(void)kl_ui_text(phone->ui, event);
		break;
	case KL_WINDOW_RESIZE:
		phone->resized = 1;
		break;
	case KL_WINDOW_CLOSE:
		phone->view.quit = 1;
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
ph_resize(
	struct ph_window *phone)
{
	uint32_t *pixels;
	int see_through;
	int status;

	/* The presenter at the window's size. */
	status = kl_window_present_resize(phone->window, &phone->width, &phone->height);
	if (status != 0) {
		ph_log("FAILED operation=present error=%d", status);
		return -1;
	}

	/* The compositor's glass, when the frames are blended by their alpha (decided at the first size). */
	if (!phone->glass_decided) {
		phone->glass_decided = 1;
		see_through = kl_window_see_through(phone->window);
		if (see_through) {
			phone->style.glass = 1;
			phone->view.glass = 1;
		}

		/* The log line the tests read. */
		ph_log("GLASS see_through=%d", see_through);
	}

	/* A frame's pixels of its size. */
	pixels = malloc((size_t)phone->width * (size_t)phone->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* The canvas on them, in place of the old one. */
	if (phone->canvas_made)
		kl_canvas_release(&phone->canvas);
	phone->canvas_made = 0;
	free(phone->pixels);
	phone->pixels = pixels;
	status = kl_canvas_init(&phone->canvas, phone->pixels, (size_t)phone->width, (int)phone->width, (int)phone->height);
	if (status != 0)
		return -1;

	/* Succeeded: drawn again at the new size. */
	phone->canvas_made = 1;
	phone->style.canvas = &phone->canvas;
	phone->dirty = 1;
	return 0;
}

/*
 * Draws and shows a frame when something changed or moves, then takes the
 * keys no widget took.
 */
static void
ph_draw(
	struct ph_window *phone,
	uint64_t now_us)
{
	struct kl_glass_panel panels[PH_PANELS_MAX];
	struct kl_event event;
	struct kl_rect caret;
	const struct kl_rect *present_part;
	struct kl_rect part;
	size_t count;
	int status;
	int error;
	int taken;
	int wanted;
	int partial;

	/* Nothing changed and nothing moves: no frame. */
	if (!phone->dirty && !phone->moving && !phone->lit_changed)
		return;

	/*
	 * Only another widget lit: the frame is drawn within the two widgets'
	 * part alone, the rest keeping its pixels (BUG-226; the whole window
	 * when the part cannot be told).
	 */
	partial = 0;
	if (!phone->dirty && !phone->moving && phone->lit_changed)
		partial = kl_ui_take_damage(phone->ui, &part);
	phone->lit_changed = 0;

	/* The view, within the part when there is one. */
	phone->dirty = 0;
	if (partial)
		kl_canvas_clip_push(&phone->canvas, &part);
	kl_ui_begin(phone->ui, now_us);
	ph_view_draw(&phone->view, phone->ui, &phone->style, (int)phone->width, (int)phone->height, now_us);
	phone->moving = kl_ui_end(phone->ui, now_us);
	if (partial)
		kl_canvas_clip_pop(&phone->canvas);

	/*
	 * The text input is asked for while a field has the keyboard, and told
	 * where its caret is, so that an input method's candidates and the
	 * on-screen keyboard stay out of its way.
	 */
	wanted = kl_ui_text_wanted(phone->ui, &caret);
	kl_window_text_input(phone->window, wanted);
	if (wanted)
		kl_window_text_cursor(phone->window, caret.x, caret.y, caret.width, caret.height);

	/* The glass's panels for the frame; a compositor without glass leaves the window opaque from the next one. */
	if (phone->view.glass) {
		count = ph_view_panels(&phone->view, (int)phone->width, (int)phone->height, panels, PH_PANELS_MAX);
		error = kl_window_set_glass(phone->window, panels, count);
		if (error != 0) {
			ph_log("GLASS failed error=%d", error);
			phone->view.glass = 0;
			phone->style.glass = 0;
			phone->dirty = 1;
		}
	}

	/* The frame shown, by the part alone when it was drawn so (BUG-221). */
	present_part = NULL;
	if (partial)
		present_part = &part;
	status = kl_window_present_part(phone->window, phone->pixels, (size_t)phone->width, present_part);
	if (status == EAGAIN)
		phone->resized = 1;

	/* The keys no widget took, and the input that met no widget. */
	for (;;) {
		taken = kl_ui_take(phone->ui, &event);
		if (!taken)
			break;

		/* A key is the view's; it draws again. */
		if (event.kind == KL_EVENT_KEY) {
			ph_view_key(&phone->view, event.code, event.modifiers, now_us);
			phone->dirty = 1;
		}
	}
}

/*
 * Reports how long the loop may wait for input (ms): no time while a frame
 * is due, a frame's time while something moves, until the view's notice
 * goes, or a second.
 */
static int
ph_wait(
	const struct ph_window *phone,
	uint64_t now_us)
{
	int wait;

	/* A frame due now (or its lit part), or requests to carry out. */
	if (phone->dirty || phone->lit_changed)
		return 0;
	if (phone->view.request_count != 0U)
		return 0;

	/* Something moving. */
	if (phone->moving)
		return PH_MOVING_MS;

	/* The view's notice, or a second. */
	wait = ph_view_wait(&phone->view, now_us);
	if (wait < 0 || wait > PH_IDLE_MS)
		wait = PH_IDLE_MS;

	/* The time to wait. */
	return wait;
}

/* Opens the store under the home's Documents (an empty one when it cannot). */
static void
ph_store_start(void)
{
	char root[1024];
	const char *home;
	int error;

	/* The home's Documents and Phone in it. */
	home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		home = "/tmp";
	(void)snprintf(root, sizeof(root), "%s/Documents", home);
	(void)mkdir(root, 0700);
	(void)snprintf(root, sizeof(root), "%s/%s", home, PH_STORE_FOLDER);

	/* Everything kept there. */
	error = ph_store_open(root);
	ph_log("STORE error=%d", error);
}

/* Takes the phone's events and the results of the requests sent. */
static void
ph_phone_round(
	struct ph_window *phone)
{
	struct kl_phone_event event;
	unsigned changed;
	uint32_t request;
	int taken;
	int error;
	int status;

	/* No phone. */
	if (phone->system == NULL)
		return;

	/* The events read by the application's dispatch. */
	status = kl_system_dispatch(phone->system, &changed);
	if (status != 0) {
		phone->system = NULL;
		return;
	}

	/* The requests refused (no backend): failed. */
	for (;;) {
		taken = kl_system_take_result(phone->system, &request, &error);
		if (!taken)
			break;
		if (error == 0)
			continue;
		ph_log("RESULT request=%u error=%d", request, error);
		ph_status(phone, request, 0U, 1);
		if (error == ENODEV)
			ph_view_notice(&phone->view, "No phone backend: choose one in the desktop's settings (phone.backend).", kl_clock_us());
	}

	/* Each message that came, and each state. */
	for (;;) {
		taken = kl_system_take_phone_event(phone->system, &event);
		if (!taken)
			break;
		if (event.kind == KL_PHONE_RECEIVED)
			ph_received(phone, &event);
		else if (event.kind == KL_PHONE_STATUS)
			ph_status(phone, event.request, event.state, 0);
	}
}

/* Keeps a message that came under its sender's contact (a new one for a number not known), and tells it. */
static void
ph_received(
	struct ph_window *phone,
	const struct kl_phone_event *event)
{
	const struct ph_contact *contacts;
	char title[128];
	size_t count;
	size_t item;
	long contact;
	int error;

	/* The sender's contact. */
	contact = ph_store_find_number(event->from);
	if (contact < 0) {
		error = ph_store_add_contact("", event->from, &contact);
		if (error != 0) {
			ph_log("RECEIVED contact-failed error=%d", error);
			return;
		}
	}

	/* The message, unread. */
	error = ph_store_add_item(contact, PH_TEXT, (enum ph_channel)event->channel, 0, (time_t)event->time, PH_STATE_UNREAD, event->text, NULL, &item);
	ph_log("RECEIVED contact=%ld channel=%u length=%lu error=%d", contact, event->channel, (unsigned long)strlen(event->text), error);
	if (error != 0)
		return;
	phone->dirty = 1;

	/* Shown now: read at once, the timeline at its end. */
	if (contact == phone->view.selected && !phone->view.adding) {
		(void)ph_store_mark_read(contact);
		phone->view.to_end = 1;
	}

	/* Told. */
	contacts = ph_contacts(&count);
	(void)snprintf(title, sizeof(title), "Message from %s", contacts[contact].name);
	(void)kl_app_notify(phone->app, title, event->text);
}

/* Keeps a request's state on its item (failed: the request was refused or the backend could not). */
static void
ph_status(
	struct ph_window *phone,
	uint32_t request,
	unsigned state,
	int failed)
{
	const struct ph_contact *contacts;
	const struct ph_item *item;
	enum ph_state kept;
	const char *detail;
	size_t count;
	size_t index;

	/* The request's row. */
	for (index = 0; index < PH_PENDING_MAX; index++) {
		if (phone->pending[index].request == request)
			break;
	}

	/* A request not waited for. */
	if (index == PH_PENDING_MAX || request == 0U)
		return;
	contacts = ph_contacts(&count);
	item = &contacts[phone->pending[index].contact].items[phone->pending[index].item];

	/* The state as the store keeps it. */
	detail = NULL;
	kept = PH_STATE_FAILED;
	if (!failed && state == KL_PHONE_SENT)
		kept = PH_STATE_SENT;
	else if (!failed && state == KL_PHONE_DELIVERED)
		kept = PH_STATE_DELIVERED;
	else if (!failed && state == KL_PHONE_ANSWERED)
		kept = PH_STATE_ANSWERED;
	else if (!failed && state == KL_PHONE_NO_ANSWER)
		kept = PH_STATE_NO_ANSWER;
	if (item->kind == PH_CALL && kept == PH_STATE_ANSWERED)
		detail = "0:00";
	(void)ph_store_set_state(phone->pending[index].contact, phone->pending[index].item, kept, detail);
	ph_log("STATUS request=%u state=%u failed=%d", request, state, failed);
	phone->dirty = 1;

	/* A request that ended frees its row (a message is done once delivered or failed, a call once it ended). */
	if (kept != PH_STATE_SENT)
		phone->pending[index].request = 0;
}

/* Carries out what the view asked. */
static void
ph_requests(
	struct ph_window *phone)
{
	struct ph_request request;
	int taken;

	/* Each request. */
	for (;;) {
		taken = ph_view_take_request(&phone->view, &request);
		if (!taken)
			break;
		phone->dirty = 1;

		/* Each kind. */
		switch (request.action) {
		case PH_ACTION_SEND:
			ph_send(phone, request.contact);
			break;
		case PH_ACTION_CALL:
			ph_call(phone, request.contact);
			break;
		case PH_ACTION_READ:
			(void)ph_store_mark_read(request.contact);
			break;
		case PH_ACTION_SAVE:
			ph_save_contact(phone);
			break;
		default:
			break;
		}
	}
}

/* Keeps the message written as sending and sends it on the channel the contact's last message went by (RCS for a first). */
static void
ph_send(
	struct ph_window *phone,
	long contact)
{
	const struct ph_contact *contacts;
	enum ph_channel channel;
	uint32_t request;
	size_t count;
	size_t item;
	size_t index;
	int error;

	/* The contact and its channel. */
	contacts = ph_contacts(&count);
	if (contact < 0 || (size_t)contact >= count || phone->view.message.length == 0U)
		return;
	channel = PH_RCS;
	for (index = contacts[contact].item_count; index > 0U; index--) {
		if (contacts[contact].items[index - 1U].kind == PH_CALL)
			continue;
		if (contacts[contact].items[index - 1U].channel <= PH_RCS)
			channel = contacts[contact].items[index - 1U].channel;
		break;
	}

	/* Kept as sending, the timeline at its end. */
	error = ph_store_add_item(contact, PH_TEXT, channel, 1, time(NULL), PH_STATE_SENDING, phone->view.message.text, NULL, &item);
	if (error != 0) {
		ph_view_notice(&phone->view, "The message could not be kept.", kl_clock_us());
		return;
	}

	/* The new item shown. */
	phone->view.to_end = 1;

	/* Sent through the compositor's phone; without one it failed. */
	request = 0;
	error = ENOTSUP;
	if (phone->system != NULL)
		error = kl_system_phone_send(phone->system, (unsigned)channel, contacts[contact].number, phone->view.message.text, &request);
	ph_log("SEND contact=%ld channel=%u length=%zu error=%d", contact, (unsigned)channel, phone->view.message.length, error);
	kl_field_set(&phone->view.message, "");
	if (error != 0) {
		(void)ph_store_set_state(contact, item, PH_STATE_FAILED, NULL);
		ph_view_notice(&phone->view, "No phone: the compositor has none.", kl_clock_us());
		return;
	}

	/* Its states to come. */
	ph_remember(phone, request, contact, item);
}

/* Calls the contact on the line and keeps the call with how it ends. */
static void
ph_call(
	struct ph_window *phone,
	long contact)
{
	const struct ph_contact *contacts;
	uint32_t request;
	size_t count;
	size_t item;
	int error;

	/* The contact. */
	contacts = ph_contacts(&count);
	if (contact < 0 || (size_t)contact >= count)
		return;

	/* Kept as a call going out, not answered until the phone says otherwise. */
	error = ph_store_add_item(contact, PH_CALL, PH_LINE, 1, time(NULL), PH_STATE_NONE, NULL, NULL, &item);
	if (error != 0)
		return;
	phone->view.to_end = 1;

	/* Made through the compositor's phone; without one it failed. */
	request = 0;
	error = ENOTSUP;
	if (phone->system != NULL)
		error = kl_system_phone_call(phone->system, KL_PHONE_LINE, contacts[contact].number, &request);
	ph_log("CALL contact=%ld error=%d", contact, error);
	if (error != 0) {
		(void)ph_store_set_state(contact, item, PH_STATE_FAILED, NULL);
		ph_view_notice(&phone->view, "No phone: the compositor has none.", kl_clock_us());
		return;
	}

	/* Its end to come. */
	ph_remember(phone, request, contact, item);
}

/* Keeps the new contact of the view's form and shows it. */
static void
ph_save_contact(
	struct ph_window *phone)
{
	long index;
	int error;

	/* Kept. */
	error = ph_store_add_contact(phone->view.new_name.text, phone->view.new_number.text, &index);
	ph_log("CONTACT added error=%d", error);
	if (error != 0) {
		ph_view_notice(&phone->view, "The contact could not be kept.", kl_clock_us());
		return;
	}

	/* Shown. */
	phone->view.adding = 0;
	ph_view_select(&phone->view, index);
}

/* Remembers a request that waits for its states (the oldest row is reused when all are taken). */
static void
ph_remember(
	struct ph_window *phone,
	uint32_t request,
	long contact,
	size_t item)
{
	size_t index;

	/* A free row, else the first. */
	for (index = 0; index < PH_PENDING_MAX; index++) {
		if (phone->pending[index].request == 0U)
			break;
	}

	/* All taken: the first is reused. */
	if (index == PH_PENDING_MAX)
		index = 0;

	/* The row. */
	phone->pending[index].request = request;
	phone->pending[index].contact = contact;
	phone->pending[index].item = item;
}
