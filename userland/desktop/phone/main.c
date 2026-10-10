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
 * The paired phone's messages (ws197-p004b, plan/ws197/phase004/phase.md
 * section 6): this program listens (kl_system_phone_listen) and brings
 * them in page by page (kl_system_phone_sync) when it starts, when the
 * phone's messages become ready, after a drop, every five minutes while
 * the phone tells no new message, and over the last seven days once a
 * day; how far it came is kept (sync/bt-<address>.state), advanced only by
 * a synchronisation that ended well.  A message that came by itself is
 * kept and told; one of a synchronisation is kept and not told.  Texts go
 * to the paired phone as SMS (kl_system_phone_send_text); one the phone
 * may or may not have sent is kept as unknown, and one still not sent an
 * hour later, after one more synchronisation, failed.  Messages read here
 * are marked read on the phone.
 *
 *   phone [--width=N] [--height=N] [--timeout-s=N] [--peer NUMBER]
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

/* The backends in kl_phone_link (the compositor's phone.backend): none and the paired phone; and the paired phone's messages off and ready. */
#define PH_BACKEND_NONE		0U
#define PH_BACKEND_BLUETOOTH	2U
#define PH_MESSAGES_OFF		0U
#define PH_MESSAGES_READY	2U

/* What the app says when no phone is set up, and when the paired phone is not connected (BUG-287). */
#define PH_NOTICE_SET_UP	"No phone is set up: in Settings, Bluetooth, press Use as phone on your phone."
#define PH_NOTICE_NOT_LINKED	"The phone is not connected: turn its Bluetooth on and keep it near. Settings, Bluetooth shows its state."

/* The synchronisations (bits of what is wanted): over the mark, the five minutes', the last seven days'. */
#define PH_SYNC_NORMAL		1U
#define PH_SYNC_QUICK		2U
#define PH_SYNC_DEEP		4U

/* A page's items, a first synchronisation's days and limit, the overlaps (seconds) and the waits. */
#define PH_SYNC_COUNT		32U
#define PH_SYNC_FIRST_DAYS	30
#define PH_SYNC_FIRST_LIMIT	500U
#define PH_DAY_SECONDS		86400
#define PH_QUICK_OVERLAP	600
#define PH_QUICK_SECONDS	300
#define PH_DEEP_DAYS		7
#define PH_SYNC_ANSWER_US	180000000U
#define PH_SYNC_TRIES		3U
#define PH_SYNC_RETRY_SECONDS	600
#define PH_STALE_SECONDS	3600
#define PH_CHECK_SECONDS	60

/* The handles kept for marking read on the phone, and the marks waiting. */
#define PH_HANDLES_MAX		256U
#define PH_MARKS_MAX		64U

/*
 * A synchronisation of the paired phone's messages: what is wanted next
 * (PH_SYNC_* bits), the kind running (0 for none), its page's request (0
 * while none is asked) and when it was asked, when it started (UNIX
 * seconds), its since, limit and cursor, the items of the page taken,
 * whether it is the first (no mark), whether a folder's limit stopped it,
 * whether a drop came meanwhile, the pages that came short in a row and
 * when to try again, when the last one started and ended well, when the
 * next five minutes' one is due and the next look at the deep one.
 */
struct ph_sync {
	unsigned wanted;
	unsigned running;
	uint32_t request;
	uint64_t asked_us;
	int64_t started;
	int64_t since;
	unsigned limit;
	char cursor[KL_PHONE_CURSOR_MAX];
	unsigned items;
	int first;
	int capped;
	int again;
	unsigned short_pages;
	time_t retry_at;
	int64_t last_start;
	time_t last_done;
	time_t next_quick;
	time_t deep_check_at;
};

/* A key's handle on the phone for this session of its messages (mark_read). */
struct ph_handle {
	char key[KL_PHONE_KEY_MAX];
	char handle[KL_PHONE_HANDLE_MAX];
};

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
	 * item it is about by its serial, and whether it is a text of
	 * kl_system_phone_send_text (a request of 0 is a free row).
	 */
	struct kl_system *system;
	struct ph_pending {
		uint32_t request;
		unsigned long serial;
		int text;
	} pending[PH_PENDING_MAX];

	/*
	 * The paired phone's messages (ws197-p004b): whether the compositor
	 * carries them, the link as last told (link_known 0 before), the
	 * synchronisation, the handles of the keys seen (a ring by
	 * handle_next), the handles waiting to be marked read on the phone and
	 * the request of the one under way (0 for none), when the texts not
	 * sent are next looked at, and when the synchronisation for them was
	 * asked (0 for none).
	 */
	int phone_sync;
	struct kl_phone_link link;
	int link_known;
	struct ph_sync sync;
	struct ph_handle handles[PH_HANDLES_MAX];
	unsigned handle_next;
	char marks[PH_MARKS_MAX][KL_PHONE_HANDLE_MAX];
	unsigned mark_count;
	uint32_t mark_request;
	time_t stale_check_at;
	time_t stale_sync_at;
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
static int ph_parse(int argc, char **argv, unsigned *width, unsigned *height, unsigned *timeout, const char **peer);
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
static void ph_remember(struct ph_window *phone, uint32_t request, unsigned long serial, int text);
static void ph_result(struct ph_window *phone, uint32_t request, int error);
static void ph_items(struct ph_window *phone);
static void ph_item(struct ph_window *phone, const struct kl_phone_item *item);
static void ph_link(struct ph_window *phone);
static int ph_paired(const struct ph_window *phone);
static void ph_sync_want(struct ph_window *phone, unsigned kind);
static void ph_sync_start(struct ph_window *phone);
static void ph_sync_page(struct ph_window *phone);
static void ph_sync_answer(struct ph_window *phone, int error);
static void ph_sync_end(struct ph_window *phone, int succeeded);
static void ph_sync_timers(struct ph_window *phone);
static void ph_stale_texts(struct ph_window *phone);
static void ph_handle_keep(struct ph_window *phone, const char *key, const char *handle);
static const char *ph_handle_of(const struct ph_window *phone, const char *key);
static void ph_mark_add(struct ph_window *phone, const char *handle);
static void ph_mark_next(struct ph_window *phone);
static void ph_read_conversation(struct ph_window *phone, long contact);

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
	const char *peer;
	unsigned capabilities;
	unsigned timeout;
	unsigned width;
	unsigned height;
	long contact;
	int status;
	int error;

	/* The command line. */
	status = ph_parse(argc, argv, &width, &height, &timeout, &peer);
	if (status != 0) {
		fprintf(stderr, "usage: phone [--width=N] [--height=N] [--timeout-s=N] [--peer NUMBER]\n");
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

	/* A number asked (a notification's click, ws197-p004 section 7): its conversation shown. */
	if (peer != NULL) {
		contact = ph_store_conversation(peer, NULL, 1);
		if (contact >= 0)
			ph_view_select(&phone.view, contact);
		ph_log("PEER found=%d", contact >= 0);
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

	/* The compositor's phone, and its messages of the paired phone heard (ws197-p004b). */
	phone.system = kl_app_system(phone.app);
	if (phone.system != NULL) {
		capabilities = kl_system_capabilities(phone.system);
		if ((capabilities & KL_SYSTEM_HAS_PHONE) == 0U)
			phone.system = NULL;
		if ((capabilities & KL_SYSTEM_HAS_PHONE_SYNC) != 0U && phone.system != NULL) {
			error = kl_system_phone_listen(phone.system, 1U);
			if (error == 0)
				phone.phone_sync = 1;
		}
	}

	/* The log the tests read. */
	ph_log("SYSTEM phone=%d sync=%d", phone.system != NULL, phone.phone_sync);

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
	unsigned *timeout,
	const char **peer)
{
	int index;
	int same;

	/* The defaults. */
	*width = PH_WIDTH;
	*height = PH_HEIGHT;
	*timeout = 0U;
	*peer = NULL;

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

		/* The number whose conversation is shown (the next argument). */
		same = strcmp(argv[index], "--peer");
		if (same == 0 && index + 1 < argc) {
			index++;
			*peer = argv[index];
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
	const struct kl_rect *present_part;
	struct kl_rect part;
	size_t count;
	int status;
	int error;
	int taken;
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
	 * on-screen keyboard stay out of its way; the widgets' Cut, Copy and
	 * Paste use the window's clipboard (ws190-p002: the bar of the fingers'
	 * selection too).
	 */
	kl_ui_window_text(phone->ui, phone->window);

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

/*
 * Takes the phone's items, events and the results of the requests sent
 * (the items first: a sync's result follows its items), then starts what
 * the paired phone's messages wait for.
 */
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

	/* The paired phone's messages that came. */
	ph_items(phone);

	/* Each message that came, each state, the link's changes and the drops. */
	for (;;) {
		taken = kl_system_take_phone_event(phone->system, &event);
		if (!taken)
			break;

		/* Each kind. */
		switch (event.kind) {
		case KL_PHONE_RECEIVED:
			ph_received(phone, &event);
			break;
		case KL_PHONE_STATUS:
			ph_status(phone, event.request, event.state, 0);
			break;
		case KL_PHONE_LINK_CHANGED:
			ph_link(phone);
			break;
		case KL_PHONE_DROPPED:
			ph_items(phone);
			ph_sync_want(phone, PH_SYNC_NORMAL);
			break;
		default:
			break;
		}
	}

	/* Items an event's mark told late. */
	ph_items(phone);

	/* The requests' results. */
	for (;;) {
		taken = kl_system_take_result(phone->system, &request, &error);
		if (!taken)
			break;
		ph_result(phone, request, error);
	}

	/* The synchronisation, the texts not sent and the marks waiting. */
	ph_sync_timers(phone);
	ph_stale_texts(phone);
	ph_sync_start(phone);
	ph_mark_next(phone);
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
	size_t at;
	long contact;
	int error;

	/* The request's row. */
	for (index = 0; index < PH_PENDING_MAX; index++) {
		if (phone->pending[index].request == request)
			break;
	}

	/* A request not waited for, or its item gone. */
	if (index == PH_PENDING_MAX || request == 0U)
		return;
	error = ph_store_find_serial(phone->pending[index].serial, &contact, &at);
	if (error != 0) {
		phone->pending[index].request = 0;
		return;
	}

	/* The item asked about. */
	contacts = ph_contacts(&count);
	item = &contacts[contact].items[at];

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

	/* A text delivered stays so when its sent comes late. */
	if (kept == PH_STATE_SENT && item->state == PH_STATE_DELIVERED)
		kept = PH_STATE_DELIVERED;
	(void)ph_store_set_state(contact, at, kept, detail);
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
			ph_read_conversation(phone, request.contact);
			break;
		case PH_ACTION_SAVE:
			ph_save_contact(phone);
			break;
		default:
			break;
		}
	}
}

/*
 * Keeps the message written as sending and sends it: to the paired phone
 * as SMS (ws197-p004b), else on the channel the contact's last message
 * went by (RCS for a first).
 */
static void
ph_send(
	struct ph_window *phone,
	long contact)
{
	const struct ph_contact *contacts;
	enum ph_channel channel;
	unsigned long serial;
	uint32_t request;
	size_t count;
	size_t item;
	size_t index;
	int paired;
	int error;

	/* The contact. */
	contacts = ph_contacts(&count);
	if (contact < 0 || (size_t)contact >= count || phone->view.message.length == 0U)
		return;

	/* No phone set up: nothing kept or sent, and where to set one up (the text stays to send later, BUG-287). */
	if (phone->system != NULL && phone->phone_sync && phone->link_known && phone->link.backend == PH_BACKEND_NONE) {
		ph_view_notice(&phone->view, PH_NOTICE_SET_UP, kl_clock_us());
		return;
	}

	/* The paired phone not connected: nothing kept or sent (the text stays). */
	paired = ph_paired(phone);
	if (paired && phone->link.messages != PH_MESSAGES_READY) {
		ph_view_notice(&phone->view, PH_NOTICE_NOT_LINKED, kl_clock_us());
		return;
	}

	/* The paired phone that takes no text: nothing kept or sent. */
	if (paired && !phone->link.can_send) {
		ph_view_notice(&phone->view, "The phone does not take texts to send.", kl_clock_us());
		return;
	}

	/* Its channel: SMS through the paired phone, else the last message's. */
	channel = PH_RCS;
	for (index = contacts[contact].item_count; index > 0U; index--) {
		if (contacts[contact].items[index - 1U].kind == PH_CALL)
			continue;
		if (contacts[contact].items[index - 1U].channel <= PH_RCS)
			channel = contacts[contact].items[index - 1U].channel;
		break;
	}

	/* The paired phone's texts are SMS. */
	if (paired)
		channel = PH_SMS;

	/* Kept as sending, the timeline at its end. */
	error = ph_store_add_item(contact, PH_TEXT, channel, 1, time(NULL), PH_STATE_SENDING, phone->view.message.text, NULL, &item);
	if (error != 0) {
		ph_view_notice(&phone->view, "The message could not be kept.", kl_clock_us());
		return;
	}

	/* Its name for this run. */
	serial = contacts[contact].items[item].serial;

	/* The new item shown. */
	phone->view.to_end = 1;

	/* Sent through the compositor's phone (the paired phone's texts of up to 8192 bytes); without one it failed. */
	request = 0;
	error = ENOTSUP;
	if (paired) {
		error = kl_system_phone_send_text(phone->system, KL_PHONE_SMS, contacts[contact].number, phone->view.message.text, phone->view.message.length,
		    &request);
	} else if (phone->system != NULL) {
		error = kl_system_phone_send(phone->system, (unsigned)channel, contacts[contact].number, phone->view.message.text, &request);
	}

	/* The log the tests read: lengths only. */
	ph_log("SEND contact=%ld channel=%u length=%zu paired=%d error=%d", contact, (unsigned)channel, phone->view.message.length, paired, error);
	kl_field_set(&phone->view.message, "");
	if (error != 0) {
		(void)ph_store_set_state(contact, item, PH_STATE_FAILED, NULL);
		if (error == EINVAL)
			ph_view_notice(&phone->view, "The number or the message cannot be sent.", kl_clock_us());
		else
			ph_view_notice(&phone->view, PH_NOTICE_SET_UP, kl_clock_us());
		return;
	}

	/* Its states to come. */
	ph_remember(phone, request, serial, paired);
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
	ph_remember(phone, request, contacts[contact].items[item].serial, 0);
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
	unsigned long serial,
	int text)
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
	phone->pending[index].serial = serial;
	phone->pending[index].text = text;
}

/*
 * Takes a request's result: the synchronisation's page, a mark on the
 * phone, a text of the paired phone (one that may or may not have gone is
 * unknown, ws197-p004 section 3.5), or a request of version 16 (a refusal
 * fails it).
 */
static void
ph_result(
	struct ph_window *phone,
	uint32_t request,
	int error)
{
	enum ph_state kept;
	size_t index;
	size_t at;
	long contact;
	int found;

	/* The synchronisation's page. */
	if (request != 0U && request == phone->sync.request) {
		ph_sync_answer(phone, error);
		return;
	}

	/* A mark on the phone: the next one goes (a stale or unconnected one waits for the next synchronisation's handle). */
	if (request != 0U && request == phone->mark_request) {
		phone->mark_request = 0U;
		ph_log("MARK error=%d", error);
		return;
	}

	/* A text of the paired phone: in the phone's outbox, or not sent, or not known. */
	for (index = 0; index < PH_PENDING_MAX; index++) {
		if (phone->pending[index].request == request && phone->pending[index].text)
			break;
	}

	/* A text of the paired phone answered. */
	if (index < PH_PENDING_MAX && request != 0U) {
		ph_log("RESULT request=%u error=%d text=1", request, error);
		if (error == 0)
			return;

		/* Lost on the way: it may have gone; else refused. */
		kept = PH_STATE_FAILED;
		if (error == ECONNRESET || error == ETIMEDOUT || error == ENOBUFS)
			kept = PH_STATE_UNKNOWN;
		found = ph_store_find_serial(phone->pending[index].serial, &contact, &at);
		if (found == 0)
			(void)ph_store_set_state(contact, at, kept, NULL);
		phone->pending[index].request = 0U;
		phone->dirty = 1;
		return;
	}

	/* A request of version 16 refused (no backend): failed. */
	if (error == 0)
		return;
	ph_log("RESULT request=%u error=%d", request, error);
	ph_status(phone, request, 0U, 1);
	if (error == ENODEV || (phone->link_known && phone->link.backend == PH_BACKEND_NONE))
		ph_view_notice(&phone->view, PH_NOTICE_SET_UP, kl_clock_us());
}

/* Takes every item of the paired phone that waits. */
static void
ph_items(
	struct ph_window *phone)
{
	struct kl_phone_item item;
	int taken;

	/* No messages of the paired phone. */
	if (!phone->phone_sync)
		return;

	/* Each item, until none waits. */
	for (;;) {
		taken = kl_system_take_phone_item(phone->system, &item, sizeof(item));
		if (!taken)
			break;
		ph_item(phone, &item);
	}
}

/*
 * Keeps one item of the paired phone (ws197-p004 section 6.3): counted for
 * its sync's page, its handle kept, marked read on the phone when it was
 * read here, and told when it came by itself, new and not read.
 */
static void
ph_item(
	struct ph_window *phone,
	const struct kl_phone_item *item)
{
	struct ph_phone_message message;
	const struct ph_contact *contacts;
	const struct ph_item *kept;
	char title[160];
	char body[160];
	char lock_text[128];
	size_t count;
	size_t at;
	long contact;
	int merge;
	int error;

	/* Counted for the page asked. */
	if (item->request != 0U && item->request == phone->sync.request)
		phone->sync.items++;

	/* The message as the store takes it. */
	memset(&message, 0, sizeof(message));
	message.address = "-";
	if (phone->link_known && phone->link.address[0] != '\0')
		message.address = phone->link.address;
	message.key = item->key;
	if (item->partial)
		message.key = "-";
	message.date = (time_t)item->time;
	message.peer = item->peer;
	message.name = item->name;
	message.text = item->text;

	/* Its direction, whether the phone read it and whether its words were cut. */
	if (item->direction == 1U)
		message.outgoing = 1;
	if (item->read != 0U)
		message.read = 1;
	if (item->truncated != 0U)
		message.truncated = 1;

	/* Kept. */
	error = ph_store_phone_message(&message, &contact, &at, &merge);
	ph_log("ITEM request=%u dir=%u length=%lu merge=%d error=%d", item->request, item->direction, (unsigned long)item->length, merge, error);
	if (error != 0)
		return;
	phone->dirty = 1;

	/* Its handle on the phone, for marking it read. */
	if (!item->partial)
		ph_handle_keep(phone, item->key, item->handle);

	/* Read here and not on the phone: marked there. */
	contacts = ph_contacts(&count);
	kept = &contacts[contact].items[at];
	if (merge == PH_MERGE_KNOWN && !message.outgoing && !message.read && kept->state == PH_STATE_READ && !item->partial)
		ph_mark_add(phone, item->handle);

	/* Only a new message that came by itself, not read, is told and shown. */
	if (merge != PH_MERGE_NEW || item->request != 0U || message.outgoing || message.read)
		return;

	/* Shown now: read at once, the timeline at its end. */
	if (contact == phone->view.selected && !phone->view.adding) {
		ph_read_conversation(phone, contact);
		phone->view.to_end = 1;
	}

	/* Told: the other side's name and the first line; the lock screen shows the name alone (ws197-p004c). */
	(void)snprintf(title, sizeof(title), "Message from %s", contacts[contact].name);
	(void)snprintf(body, sizeof(body), "%.*s", (int)strcspn(item->text, "\n"), item->text);
	(void)snprintf(lock_text, sizeof(lock_text), "New message from %s", contacts[contact].name);
	(void)kl_app_notify_lock(phone->app, title, body, lock_text);
}

/*
 * Takes the link's new state: the send button grey when the paired phone
 * takes no text, why it does not work told, and a synchronisation when its
 * messages become ready.
 */
static void
ph_link(
	struct ph_window *phone)
{
	struct kl_phone_link link;
	unsigned was;
	int permission;
	int no_mas;
	int error;
	int same;

	/* The state. */
	error = kl_system_phone_link(phone->system, &link, sizeof(link));
	if (error != 0)
		return;
	was = PH_MESSAGES_OFF;
	if (phone->link_known)
		was = phone->link.messages;
	same = strcmp(link.why, phone->link.why);
	phone->link = link;
	phone->link_known = 1;
	ph_log("LINK backend=%u messages=%u send=%u notify=%u owner=%u why=%s", link.backend, link.messages, link.can_send, link.notify, link.owner, link.why);

	/* The send button: grey only for a connected phone that takes no texts (one not connected is told why on Send, BUG-287). */
	phone->view.cannot_send = 0;
	if (link.backend == PH_BACKEND_BLUETOOTH && link.messages == PH_MESSAGES_READY && !link.can_send)
		phone->view.cannot_send = 1;
	phone->dirty = 1;

	/* Why the phone's messages do not work, once for each change. */
	permission = strcmp(link.why, "permission");
	no_mas = strcmp(link.why, "no-mas");
	if (same != 0 && link.backend == PH_BACKEND_BLUETOOTH) {
		if (permission == 0)
			ph_view_notice(&phone->view, "Allow access to the messages on the phone.", kl_clock_us());
		else if (no_mas == 0)
			ph_view_notice(&phone->view, "The phone does not share its messages.", kl_clock_us());
	}

	/* Ready now: a synchronisation (and at once the five minutes' and the deep one looked at). */
	if (link.messages == PH_MESSAGES_READY && was != PH_MESSAGES_READY && link.backend != 0U) {
		ph_sync_want(phone, PH_SYNC_NORMAL);
		phone->sync.next_quick = time(NULL) + PH_QUICK_SECONDS;
		phone->sync.deep_check_at = 0;
	}
}

/* Tells whether the texts go to the paired phone: the compositor carries its messages and the backend is it. */
static int
ph_paired(
	const struct ph_window *phone)
{
	/* The messages carried, and the link of the paired phone. */
	if (phone->system == NULL || !phone->phone_sync || !phone->link_known)
		return 0;
	if (phone->link.backend != PH_BACKEND_BLUETOOTH)
		return 0;

	/* Succeeded: paired. */
	return 1;
}

/* Asks a synchronisation (PH_SYNC_*): after the one running, which a drop runs again. */
static void
ph_sync_want(
	struct ph_window *phone,
	unsigned kind)
{
	/* A drop during one makes it run again. */
	if (phone->sync.running != 0U && kind == PH_SYNC_NORMAL)
		phone->sync.again = 1;

	/* Wanted. */
	phone->sync.wanted |= kind;
}

/*
 * Starts the synchronisation wanted when none runs and the phone's
 * messages are ready (section 6.2): the first since thirty days ago, at
 * most five hundred a folder; the next since the mark; the five minutes'
 * since ten minutes before the last start; the deep one over the last
 * seven days.
 */
static void
ph_sync_start(
	struct ph_window *phone)
{
	struct ph_sync *sync;
	struct ph_sync_marks marks;
	int64_t since;
	time_t now;
	int error;

	/* One at a time, when wanted, the phone's messages ready, and not waiting to try again. */
	sync = &phone->sync;
	now = time(NULL);
	if (!phone->phone_sync || sync->running != 0U || sync->wanted == 0U)
		return;
	if (!phone->link_known || phone->link.messages != PH_MESSAGES_READY || phone->link.backend == 0U)
		return;
	if (sync->retry_at != 0 && now < sync->retry_at)
		return;
	sync->retry_at = 0;

	/* The mark (none: the first). */
	memset(&marks, 0, sizeof(marks));
	error = ENOENT;
	if (phone->link.address[0] != '\0')
		error = ph_store_sync_load(phone->link.address, &marks);
	since = marks.messages_since;

	/* The kind: over the mark first, then the deep one, then the five minutes'. */
	memset(sync->cursor, 0, sizeof(sync->cursor));
	sync->first = 0;
	sync->capped = 0;
	sync->again = 0;
	sync->limit = 0U;
	if ((sync->wanted & PH_SYNC_NORMAL) != 0U || error != 0) {
		sync->running = PH_SYNC_NORMAL;
		sync->since = since;
		if (error != 0) {
			sync->first = 1;
			sync->since = (int64_t)now - (int64_t)PH_SYNC_FIRST_DAYS * PH_DAY_SECONDS;
			sync->limit = PH_SYNC_FIRST_LIMIT;
		}

		/* The kinds it covers are done. */
		sync->wanted &= ~(PH_SYNC_NORMAL | PH_SYNC_QUICK);
	} else if ((sync->wanted & PH_SYNC_DEEP) != 0U) {
		sync->running = PH_SYNC_DEEP;
		sync->since = (int64_t)now - (int64_t)PH_DEEP_DAYS * PH_DAY_SECONDS;
		sync->wanted &= ~PH_SYNC_DEEP;
	} else {
		sync->running = PH_SYNC_QUICK;
		sync->since = since;
		if (sync->last_start != 0)
			sync->since = sync->last_start - PH_QUICK_OVERLAP;
		sync->wanted &= ~PH_SYNC_QUICK;
	}

	/* Not before the epoch. */
	if (sync->since < 0)
		sync->since = 0;

	/* Its first page. */
	sync->started = (int64_t)now;
	sync->last_start = sync->started;
	ph_log("SYNC start kind=%u first=%d limit=%u", sync->running, sync->first, sync->limit);
	ph_sync_page(phone);
}

/* Asks the synchronisation's next page (busy: asked again at the next round). */
static void
ph_sync_page(
	struct ph_window *phone)
{
	struct ph_sync *sync;
	uint32_t request;
	int error;

	/* The page from the cursor. */
	sync = &phone->sync;
	error = kl_system_phone_sync(phone->system, KL_PHONE_MESSAGES, sync->since, sync->limit, sync->cursor, PH_SYNC_COUNT, &request);
	if (error == EBUSY) {
		sync->request = 0U;
		return;
	}

	/* Another failure ends it. */
	if (error != 0) {
		ph_log("SYNC page error=%d", error);
		ph_sync_end(phone, 0);
		return;
	}

	/* Asked: its items are counted until its answer. */
	sync->request = request;
	sync->asked_us = kl_clock_us();
	sync->items = 0U;
}

/*
 * Takes the answer of the synchronisation's page: the next page while
 * more follow, the end when none does; a page whose items did not all
 * come, a stale cursor or a failure ends it without moving the mark.
 */
static void
ph_sync_answer(
	struct ph_window *phone,
	int error)
{
	char cursor[KL_PHONE_CURSOR_MAX];
	struct ph_sync *sync;
	unsigned skipped;
	unsigned capped;
	unsigned kind;
	unsigned count;
	unsigned more;
	int found;

	/* The page answered. */
	sync = &phone->sync;
	found = ENOENT;
	if (error == 0)
		found = kl_system_phone_page_end(phone->system, sync->request, cursor, sizeof(cursor), &more, &count, &skipped, &capped);
	sync->request = 0U;

	/* A stale cursor: the same synchronisation again from the start. */
	if (error == ESTALE) {
		kind = sync->running;
		ph_sync_end(phone, 0);
		ph_sync_want(phone, kind);
		return;
	}

	/* A failure: the mark stays. */
	if (error != 0 || found != 0) {
		ph_log("SYNC failed error=%d", error);
		ph_sync_end(phone, 0);
		return;
	}

	/* Items lost on the way: again, three times in a row, then after ten minutes. */
	if (count != sync->items) {
		ph_log("SYNC short count=%u items=%u", count, sync->items);
		sync->short_pages++;
		kind = sync->running;
		ph_sync_end(phone, 0);
		ph_sync_want(phone, kind);
		if (sync->short_pages >= PH_SYNC_TRIES) {
			sync->short_pages = 0U;
			sync->retry_at = time(NULL) + PH_SYNC_RETRY_SECONDS;
		}

		/* Tried again later. */
		return;
	}

	/* The next page. */
	if (capped)
		sync->capped = 1;
	if (more) {
		(void)snprintf(sync->cursor, sizeof(sync->cursor), "%s", cursor);
		ph_sync_page(phone);
		return;
	}

	/* Succeeded: the end. */
	sync->short_pages = 0U;
	ph_sync_end(phone, 1);
}

/*
 * Ends the synchronisation: one that ended well moves the mark (a day
 * before it started; the deep one its own time), tells a first one cut by
 * the limit, and runs once more after a drop that came meanwhile.
 */
static void
ph_sync_end(
	struct ph_window *phone,
	int succeeded)
{
	struct ph_sync *sync;
	struct ph_sync_marks marks;
	unsigned kind;
	int again;
	int error;

	/* Not running any more. */
	sync = &phone->sync;
	kind = sync->running;
	again = sync->again;
	sync->running = 0U;
	sync->request = 0U;
	sync->again = 0;
	ph_log("SYNC end kind=%u succeeded=%d capped=%d", kind, succeeded, sync->capped);
	if (!succeeded)
		return;

	/* The mark of the phone (its other marks as they were). */
	if (phone->link.address[0] != '\0') {
		(void)ph_store_sync_load(phone->link.address, &marks);
		if (kind == PH_SYNC_DEEP)
			marks.deep_at = (int64_t)time(NULL);
		else
			marks.messages_since = sync->started - PH_DAY_SECONDS;
		error = ph_store_sync_save(phone->link.address, &marks);
		if (error != 0)
			ph_log("SYNC save error=%d", error);
	}

	/* Ended well: the texts not sent are looked at again. */
	sync->last_done = time(NULL);
	phone->stale_check_at = 0;

	/* A first one cut by the limit. */
	if (sync->first && sync->capped)
		ph_view_notice(&phone->view, "Some older messages were not brought in.", kl_clock_us());

	/* A drop meanwhile: once more. */
	if (again)
		ph_sync_want(phone, PH_SYNC_NORMAL);
}

/*
 * Looks after the synchronisation's times: a page not answered in time
 * fails it, the five minutes' one while the phone tells no new message,
 * and the deep one once a day.
 */
static void
ph_sync_timers(
	struct ph_window *phone)
{
	struct ph_sync *sync;
	struct ph_sync_marks marks;
	uint64_t now_us;
	time_t now;
	int error;

	/* A page not answered in time. */
	sync = &phone->sync;
	now_us = kl_clock_us();
	if (sync->request != 0U && now_us - sync->asked_us >= PH_SYNC_ANSWER_US) {
		ph_log("SYNC timeout");
		ph_sync_end(phone, 0);
	}

	/* A page busy at the library: asked again. */
	if (sync->running != 0U && sync->request == 0U)
		ph_sync_page(phone);

	/* Only while the phone's messages are ready. */
	if (!phone->phone_sync || !phone->link_known || phone->link.messages != PH_MESSAGES_READY)
		return;
	now = time(NULL);

	/* No new message told: every five minutes. */
	if (!phone->link.notify && now >= sync->next_quick) {
		sync->next_quick = now + PH_QUICK_SECONDS;
		ph_sync_want(phone, PH_SYNC_QUICK);
	}

	/* The deep one when the last is a day old (looked at every hour). */
	if (now < sync->deep_check_at || phone->link.address[0] == '\0')
		return;
	sync->deep_check_at = now + PH_STALE_SECONDS;
	error = ph_store_sync_load(phone->link.address, &marks);
	if (error == 0 && (int64_t)now - marks.deep_at >= PH_DAY_SECONDS)
		ph_sync_want(phone, PH_SYNC_DEEP);
}

/*
 * Looks at the texts to the paired phone still sending or unknown an hour
 * on (ws197-p004 section 6.3): a synchronisation first, and those still
 * not matched after it failed.
 */
static void
ph_stale_texts(
	struct ph_window *phone)
{
	const struct ph_contact *contacts;
	const struct ph_item *item;
	unsigned stale;
	size_t count;
	size_t index;
	size_t at;
	time_t now;
	int failing;
	int paired;

	/* Once a minute, for the paired phone. */
	now = time(NULL);
	if (now < phone->stale_check_at)
		return;
	phone->stale_check_at = now + PH_CHECK_SECONDS;
	paired = ph_paired(phone);
	if (!paired)
		return;

	/* After a synchronisation asked for them that ended well, they failed. */
	failing = 0;
	if (phone->stale_sync_at != 0 && phone->sync.last_done >= phone->stale_sync_at)
		failing = 1;

	/* Each text sending or unknown an hour on. */
	stale = 0U;
	contacts = ph_contacts(&count);
	for (index = 0U; index < count; index++) {
		for (at = 0U; at < contacts[index].item_count; at++) {
			item = &contacts[index].items[at];
			if (item->kind != PH_TEXT || !item->outgoing)
				continue;
			if (item->state != PH_STATE_SENDING && item->state != PH_STATE_UNKNOWN)
				continue;
			if (now - item->date < PH_STALE_SECONDS)
				continue;
			stale++;
			if (failing)
				(void)ph_store_set_state((long)index, at, PH_STATE_FAILED, NULL);
		}
	}

	/* None, or failed now. */
	if (stale == 0U || failing) {
		if (failing && stale != 0U) {
			ph_log("STALE failed=%u", stale);
			phone->dirty = 1;
		}

		/* Nothing waits for a synchronisation. */
		phone->stale_sync_at = 0;
		return;
	}

	/* A synchronisation for them first. */
	if (phone->stale_sync_at == 0) {
		phone->stale_sync_at = now;
		ph_sync_want(phone, PH_SYNC_NORMAL);
	}
}

/* Keeps a key's handle on the phone (the oldest goes when all are taken). */
static void
ph_handle_keep(
	struct ph_window *phone,
	const char *key,
	const char *handle)
{
	unsigned index;
	int same;

	/* The key's own, made new. */
	for (index = 0U; index < PH_HANDLES_MAX; index++) {
		same = strcmp(phone->handles[index].key, key);
		if (same == 0) {
			(void)snprintf(phone->handles[index].handle, sizeof(phone->handles[index].handle), "%s", handle);
			return;
		}
	}

	/* Over the oldest. */
	index = phone->handle_next;
	phone->handle_next = (phone->handle_next + 1U) % PH_HANDLES_MAX;
	(void)snprintf(phone->handles[index].key, sizeof(phone->handles[index].key), "%s", key);
	(void)snprintf(phone->handles[index].handle, sizeof(phone->handles[index].handle), "%s", handle);
}

/* Finds a key's handle on the phone (NULL when none was seen). */
static const char *
ph_handle_of(
	const struct ph_window *phone,
	const char *key)
{
	unsigned index;
	int same;

	/* Each kept. */
	for (index = 0U; index < PH_HANDLES_MAX; index++) {
		if (phone->handles[index].key[0] == '\0')
			continue;
		same = strcmp(phone->handles[index].key, key);
		if (same == 0)
			return phone->handles[index].handle;
	}

	/* None. */
	return NULL;
}

/* Queues a handle to be marked read on the phone (none when the queue is full: the next synchronisation tells it again). */
static void
ph_mark_add(
	struct ph_window *phone,
	const char *handle)
{
	/* Room. */
	if (phone->mark_count == PH_MARKS_MAX || handle[0] == '\0')
		return;

	/* After the others. */
	(void)snprintf(phone->marks[phone->mark_count], sizeof(phone->marks[0]), "%s", handle);
	phone->mark_count++;
}

/* Asks the next mark on the phone once the last is answered. */
static void
ph_mark_next(
	struct ph_window *phone)
{
	uint32_t request;
	int paired;
	int error;

	/* Each mark in turn, while the paired phone carries them. */
	paired = ph_paired(phone);
	while (paired && phone->mark_request == 0U && phone->mark_count > 0U) {
		error = kl_system_phone_mark_read(phone->system, phone->marks[0], &request);
		memmove(phone->marks[0], phone->marks[1], (phone->mark_count - 1U) * sizeof(phone->marks[0]));
		phone->mark_count--;
		if (error == 0)
			phone->mark_request = request;
	}
}

/*
 * Marks a conversation read here and its messages of the paired phone
 * read there (one at a time; those without a handle yet are marked when a
 * synchronisation brings it).
 */
static void
ph_read_conversation(
	struct ph_window *phone,
	long contact)
{
	const struct ph_contact *contacts;
	const struct ph_item *item;
	const char *handle;
	const char *key;
	size_t count;
	size_t index;

	/* A contact that is there. */
	contacts = ph_contacts(&count);
	if (contact < 0 || (size_t)contact >= count)
		return;

	/* Each unread message of the phone with a handle seen. */
	for (index = 0U; index < contacts[contact].item_count; index++) {
		item = &contacts[contact].items[index];
		if (item->state != PH_STATE_UNREAD || item->source == NULL)
			continue;
		key = strrchr(item->source, ':');
		if (key == NULL)
			continue;
		handle = ph_handle_of(phone, key + 1);
		if (handle != NULL)
			ph_mark_add(phone, handle);
	}

	/* Read here. */
	(void)ph_store_mark_read(contact);
}
