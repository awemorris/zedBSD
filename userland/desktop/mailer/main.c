/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Mail's window (WS169 p000, p004; mailer.h): a libkeiland application
 * with one window that shows the view (view.c), its menu (File: New
 * Message, Get Mail, Add Account, Quit; Message: Send, Reply, Reply All,
 * Forward, Archive, Delete), and the view's input.  Ctrl+N writes a new
 * message, Ctrl+Q quits.
 *
 * The accounts are read at the start; the thread of sync.c gets their mail
 * (a new account's right after its sign-in) and waits for new mail, and the window carries out what the view asks
 * (send, get mail, archive, delete, read, a new account).  A new message
 * is told as a notification, and its sign-in code to the compositor
 * (kl_system_mail_arrived, ws169-p002), which tells the readers the user
 * allows (the browser, with the desktop's mail.codes.browser).  What
 * happens is logged on standard error as "MAIL" lines for the tests, the
 * words of a message never.
 *
 * ws177-p015: a message of the trash is deleted for good; Edit Account
 * changes or removes an account (its new settings tried first; the thread
 * is started again with the accounts as they are then); a server whose
 * certificate does not verify is named with its fingerprint, and the
 * user may trust it (kept with the account); the dates' words follow
 * the day; and a question over the window takes the input while it is
 * asked.
 *
 *   mailer [--width=N] [--height=N] [--timeout-s=N]
 */

#include "mailer.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The fonts, the window's first size, and the longest wait for input. */
#define ML_FONT			KEILAND_DATADIR "/fonts/keiland.ttf"
#define ML_FALLBACK_FONT	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"
#define ML_WIDTH		1180U
#define ML_HEIGHT		740U
#define ML_IDLE_MS		1000
#define ML_MOVING_MS		10

/* The most glass panels of a frame. */
#define ML_PANELS_MAX		4U

/* The key Q, which quits with Ctrl. */
#define ML_KEY_Q		16U

/* The desktop's setting that lets the browser hear the sign-in codes (ws169-p002). */
#define ML_CODES_SETTING	"mail.codes.browser"

/*
 * The window's state: the application, the window and its input, the
 * frame (its pixels, size and canvas), the text and the style, the view,
 * whether a frame is due, the window changed size, or something moves, and
 * whether the glass was decided.
 */
struct ml_window {
	struct kl_app *app;
	struct kl_window *window;
	struct kl_ui *ui;

	/* The input of the question asked over the window (ws177-p015): it takes the pointer and the keys while one is asked. */
	struct kl_ui *dialog_ui;
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	struct kl_canvas canvas;
	int canvas_made;
	struct kl_text text;
	struct kl_style style;
	struct ml_view view;
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
	 * The servers: the thread (NULL when it could not start), the folder
	 * the accounts are kept in, the account a Sign In tried (taken when it
	 * works), and the desktop's settings (NULL without them).
	 */
	struct ml_sync *sync;
	char folder[ML_PATH_MAX];
	struct ml_account_config pending;
	struct kl_settings *settings;

	/*
	 * The certificate the user is asked to trust (ws177-p015): the account
	 * (-1 for the form's), whether it is the SMTP server's, and its
	 * fingerprint (empty while nothing is asked); and the last one the
	 * user did not trust, which is not asked again in the run.
	 */
	int trust_account;
	int trust_smtp;
	char trust_fingerprint[ML_PIN_MAX];
	char declined[ML_PIN_MAX];
};

/* The window's menu. */
static const struct kl_menu_entry ml_menu[] = {
	{ 1U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "File", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 2U, 1U, KL_MENU_ITEM_NORMAL, "New Message", ML_ACTION_NEW, KL_MENU_ROLE_NONE, KL_MENU_CTRL, 'n' },
	{ 3U, 1U, KL_MENU_ITEM_NORMAL, "Get Mail", ML_ACTION_GET, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 14U, 1U, KL_MENU_ITEM_NORMAL, "Add Account", ML_ACTION_ADD_ACCOUNT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 15U, 1U, KL_MENU_ITEM_NORMAL, "Edit Account", ML_ACTION_EDIT_ACCOUNT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 4U, 1U, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 5U, 1U, KL_MENU_ITEM_NORMAL, "Quit Mail", ML_ACTION_QUIT, KL_MENU_ROLE_QUIT, KL_MENU_CTRL, 'q' },
	{ 6U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Message", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 7U, 6U, KL_MENU_ITEM_NORMAL, "Send", ML_ACTION_SEND, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 8U, 6U, KL_MENU_ITEM_NORMAL, "Reply", ML_ACTION_REPLY, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 9U, 6U, KL_MENU_ITEM_NORMAL, "Reply All", ML_ACTION_REPLY_ALL, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 10U, 6U, KL_MENU_ITEM_NORMAL, "Forward", ML_ACTION_FORWARD, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 11U, 6U, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 12U, 6U, KL_MENU_ITEM_NORMAL, "Archive", ML_ACTION_ARCHIVE, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 13U, 6U, KL_MENU_ITEM_NORMAL, "Delete", ML_ACTION_DELETE, KL_MENU_ROLE_NONE, 0U, 0U }
};

int main(int argc, char **argv);
static int ml_parse(int argc, char **argv, unsigned *width, unsigned *height, unsigned *timeout);
static int ml_loop(struct ml_window *mailer, unsigned timeout);
static void ml_input(struct ml_window *mailer, const struct kl_window_event *event);
static int ml_resize(struct ml_window *mailer);
static void ml_draw(struct ml_window *mailer, uint64_t now_us);
static int ml_wait(const struct ml_window *mailer, uint64_t now_us);
static void ml_servers_start(struct ml_window *mailer);
static void ml_results(struct ml_window *mailer);
static void ml_result_message(struct ml_window *mailer, const struct ml_result *result);
static void ml_requests(struct ml_window *mailer);
static void ml_request_send(struct ml_window *mailer);
static void ml_request_move(struct ml_window *mailer, long index, enum ml_folder to_folder);
static void ml_request_sign_in(struct ml_window *mailer);
static void ml_refresh_all(struct ml_window *mailer);
static void ml_status_time(struct ml_window *mailer, const char *what);
static void ml_codes_changed(void *data, const char *key, const char *value, unsigned flags);
static void ml_sync_begin(struct ml_window *mailer);
static void ml_servers_restart(struct ml_window *mailer);
static int ml_form_config(struct ml_window *mailer, struct ml_account_config *config);
static void ml_request_check(struct ml_window *mailer);
static void ml_edited(struct ml_window *mailer, int index);
static void ml_remove_account(struct ml_window *mailer, int index);
static void ml_ask_trust(struct ml_window *mailer, const struct ml_result *result);
static void ml_trust(struct ml_window *mailer, int trusted);
static void ml_draw_question(struct ml_window *mailer, uint64_t now_us);

/*
 * Runs Mail.
 */
int
main(
	int argc,
	char **argv)
{
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	static struct ml_window mailer;
	unsigned timeout;
	unsigned width;
	unsigned height;
	int status;
	int error;

	/* The command line. */
	status = ml_parse(argc, argv, &width, &height, &timeout);
	if (status != 0) {
		fprintf(stderr, "usage: mailer [--width=N] [--height=N] [--timeout-s=N]\n");
		return 2;
	}

	/* The fonts; without them the view shows no words. */
	error = kl_text_open(&mailer.text, ML_FONT, ML_FALLBACK_FONT);
	if (error != 0)
		ml_log("FONT missing error=%d", error);

	/* The view's state. */
	error = ml_view_init(&mailer.view);
	if (error != 0) {
		ml_log("FAILED operation=view error=%d", error);
		return 1;
	}

	/* The application. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "mailer";
	mailer.app = kl_app_open(&app_options);
	if (mailer.app == NULL) {
		ml_log("FAILED operation=app error=%d", errno);
		ml_view_release(&mailer.view);
		return 1;
	}

	/* Its window. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Mail";
	window_options.width = width;
	window_options.height = height;
	window_options.present = KL_PRESENT_VULKAN;
	mailer.window = kl_app_window_create(mailer.app, &window_options);
	if (mailer.window == NULL) {
		ml_log("FAILED operation=window error=%d", errno);
		kl_app_close(mailer.app);
		ml_view_release(&mailer.view);
		return 1;
	}

	/* The input of its frames. */
	mailer.ui = kl_ui_create();
	if (mailer.ui == NULL) {
		ml_log("FAILED operation=ui error=%d", errno);
		kl_app_close(mailer.app);
		ml_view_release(&mailer.view);
		return 1;
	}

	/* The input of its questions. */
	mailer.dialog_ui = kl_ui_create();
	if (mailer.dialog_ui == NULL) {
		ml_log("FAILED operation=ui error=%d", errno);
		kl_ui_destroy(mailer.ui);
		kl_app_close(mailer.app);
		ml_view_release(&mailer.view);
		return 1;
	}

	/* The menu and the style (opaque until the first frame finds whether the window can stand on glass). */
	(void)kl_window_set_menu(mailer.window, ml_menu, sizeof(ml_menu) / sizeof(ml_menu[0]));
	mailer.style.text = &mailer.text;
	mailer.style.theme = kl_theme_default();
	mailer.style.glass = 0;
	mailer.view.glass = 0;

	/* The accounts, the desktop's settings and the thread that gets the mail. */
	ml_servers_start(&mailer);

	/* The loop until the window closes. */
	status = ml_loop(&mailer, timeout);

	/* Everything goes: the thread first (it ends its sessions), then the window. */
	ml_sync_stop(mailer.sync);
	if (mailer.settings != NULL)
		kl_settings_close(mailer.settings);
	kl_ui_destroy(mailer.dialog_ui);
	kl_ui_destroy(mailer.ui);
	if (mailer.canvas_made)
		kl_canvas_release(&mailer.canvas);
	free(mailer.pixels);
	kl_app_close(mailer.app);
	ml_view_release(&mailer.view);
	kl_text_close(&mailer.text);
	ml_store_release();

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
ml_log(
	const char *format,
	...)
{
	va_list arguments;

	/* The line. */
	va_start(arguments, format);
	fputs("MAIL ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

/*
 * Reads the command line; nonzero when it cannot be read.
 */
static int
ml_parse(
	int argc,
	char **argv,
	unsigned *width,
	unsigned *height,
	unsigned *timeout)
{
	int index;
	int same;

	/* The defaults. */
	*width = ML_WIDTH;
	*height = ML_HEIGHT;
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
ml_loop(
	struct ml_window *mailer,
	unsigned timeout)
{
	struct kl_app_event event;
	uint64_t started;
	uint64_t now;
	int redated;
	int status;
	int taken;
	int wait;

	/* The first frame. */
	status = ml_resize(mailer);
	if (status != 0)
		return -1;
	ml_log("READY width=%u height=%u", mailer->width, mailer->height);

	/* Each round: the input, then a frame when something changed. */
	started = kl_clock_us();
	for (;;) {
		/* Waits for the compositor, or for the time something moves. */
		now = kl_clock_us();
		wait = ml_wait(mailer, now);
		status = kl_app_dispatch(mailer->app, wait);
		if (status != 0) {
			ml_log("DONE reason=disconnected");
			return 0;
		}

		/* The window's input and the actions of its menu. */
		for (;;) {
			taken = kl_app_take(mailer->app, &event);
			if (!taken)
				break;

			/* The desktop's appearance changed: the theme's colours are new, the window is drawn again (ws089-p017). */
			if (event.kind == KL_APP_THEME) {
				mailer->dirty = 1;
				continue;
			}

			/* The thread's results. */
			if (event.kind == KL_APP_FD) {
				ml_results(mailer);
				continue;
			}

			/* Another window's event is not this one's. */
			if (event.kind != KL_APP_WINDOW || event.window != mailer->window)
				continue;

			/* An action of the menu, or input. */
			if (event.input.kind == KL_WINDOW_ACTION) {
				ml_view_action(&mailer->view, event.input.code, kl_clock_us());
				mailer->dirty = 1;
			} else {
				ml_input(mailer, &event.input);
			}
		}

		/* The desktop's settings: a change of the sign-in codes' setting comes to ml_codes_changed. */
		if (mailer->settings != NULL)
			(void)kl_settings_dispatch(mailer->settings);

		/* What the view asked of the servers. */
		ml_requests(mailer);

		/* The end: the window closed or Quit. */
		now = kl_clock_us();
		if (mailer->view.quit) {
			ml_log("DONE reason=close");
			return 0;
		}

		/* The timeout, when one was given. */
		if (timeout != 0U && now - started >= (uint64_t)timeout * 1000000U) {
			ml_log("DONE reason=timeout");
			return 0;
		}

		/* A new size. */
		if (mailer->resized) {
			mailer->resized = 0;
			status = ml_resize(mailer);
			if (status != 0)
				return -1;
		}

		/* Another day: the dates' words from today ("Yesterday" for what was today). */
		redated = ml_store_redate(time(NULL));
		if (redated)
			mailer->dirty = 1;

		/* The view's notice gone: drawn without it. */
		if (mailer->view.notice[0] != '\0' && now >= mailer->view.notice_until) {
			mailer->view.notice[0] = '\0';
			mailer->dirty = 1;
		}

		/* A frame. */
		ml_draw(mailer, now);

		/* What the frame's buttons asked. */
		ml_requests(mailer);
	}
}

/*
 * Gives one input of the window to the view's widgets, or takes it as a
 * key of the window.
 */
static void
ml_input(
	struct ml_window *mailer,
	const struct kl_window_event *event)
{
	struct kl_ui *ui;
	int redraw;
	int taken;

	/*
	 * Any input may change the view, but a motion of the pointer only when
	 * it lights another widget or drags (BUG-226: a frame for every motion
	 * left the pointer behind).
	 */
	if (event->kind != KL_WINDOW_MOTION)
		mailer->dirty = 1;

	/* A question asked takes the input, and every input draws it again (ws177-p015). */
	ui = mailer->ui;
	if (mailer->view.question != ML_QUESTION_NONE) {
		ui = mailer->dialog_ui;
		mailer->dirty = 1;
	}

	/* Each kind of input. */
	switch (event->kind) {
	case KL_WINDOW_MOTION:
		/* A drag draws the whole window; another lit widget only its part (BUG-226). */
		redraw = kl_ui_pointer_motion(ui, event->x, event->y);
		if (mailer->buttons_held != 0U)
			mailer->dirty = 1;
		else if (redraw)
			mailer->lit_changed = 1;
		break;
	case KL_WINDOW_LEAVE:
		(void)kl_ui_pointer_leave(ui);
		break;
	case KL_WINDOW_BUTTON:
		/* The buttons held, for the motions of a drag. */
		if (event->pressed)
			mailer->buttons_held++;
		else if (mailer->buttons_held != 0U)
			mailer->buttons_held--;

		/* The left button presses the widgets. */
		(void)kl_ui_pointer_motion(ui, event->x, event->y);
		if (event->code == KL_BUTTON_LEFT)
			(void)kl_ui_pointer_button(ui, event->pressed, event->arrival_us);
		break;
	case KL_WINDOW_AXIS:
	case KL_WINDOW_AXIS_STOP:
		/* The wheel glides; a touch pad's fingers hold the content, and it flies on when they lift (BUG-211). */
		taken = kl_ui_axis(ui, event);
		if (taken == KL_UI_AXIS_FLUNG)
			ml_log("KINETIC fling source=finger");
		break;
	case KL_WINDOW_TOUCH_DOWN:
		(void)kl_ui_touch_down(ui, event->id, event->time_us, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_MOTION:
		(void)kl_ui_touch_motion(ui, event->id, event->time_us, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_UP:
		(void)kl_ui_touch_up(ui, event->id, event->time_us, event->arrival_us);
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		(void)kl_ui_touch_cancel(ui, event->arrival_us);
		break;
	case KL_WINDOW_KEY:
		/* Ctrl+Q quits; the other keys go to the widgets, and those no widget takes to the view. */
		if (event->pressed &&
		    (event->modifiers & KL_MOD_CTRL) != 0U &&
		    event->code == ML_KEY_Q)
			mailer->view.quit = 1;
		else
			(void)kl_ui_key(ui, event->code, event->pressed, event->modifiers);
		break;
	case KL_WINDOW_TEXT_COMMIT:
	case KL_WINDOW_TEXT_PREEDIT:
	case KL_WINDOW_TEXT_DELETE:
		/* Text from an input method or the on-screen keyboard, for the field with the keyboard (BUG-203, BUG-204). */
		(void)kl_ui_text(ui, event);
		break;
	case KL_WINDOW_RESIZE:
		mailer->resized = 1;
		break;
	case KL_WINDOW_CLOSE:
		mailer->view.quit = 1;
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
ml_resize(
	struct ml_window *mailer)
{
	uint32_t *pixels;
	int see_through;
	int status;

	/* The presenter at the window's size. */
	status = kl_window_present_resize(mailer->window, &mailer->width, &mailer->height);
	if (status != 0) {
		ml_log("FAILED operation=present error=%d", status);
		return -1;
	}

	/* The compositor's glass, when the frames are blended by their alpha (decided at the first size). */
	if (!mailer->glass_decided) {
		mailer->glass_decided = 1;
		see_through = kl_window_see_through(mailer->window);
		if (see_through) {
			mailer->style.glass = 1;
			mailer->view.glass = 1;
		}

		/* The log line the tests read. */
		ml_log("GLASS see_through=%d", see_through);
	}

	/* A frame's pixels of its size. */
	pixels = malloc((size_t)mailer->width * (size_t)mailer->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* The canvas on them, in place of the old one. */
	if (mailer->canvas_made)
		kl_canvas_release(&mailer->canvas);
	mailer->canvas_made = 0;
	free(mailer->pixels);
	mailer->pixels = pixels;
	status = kl_canvas_init(&mailer->canvas, mailer->pixels, (size_t)mailer->width, (int)mailer->width, (int)mailer->height);
	if (status != 0)
		return -1;

	/* Succeeded: drawn again at the new size. */
	mailer->canvas_made = 1;
	mailer->style.canvas = &mailer->canvas;
	mailer->dirty = 1;
	return 0;
}

/*
 * Draws and shows a frame when something changed or moves, then takes the
 * keys no widget took.
 */
static void
ml_draw(
	struct ml_window *mailer,
	uint64_t now_us)
{
	struct kl_glass_panel panels[ML_PANELS_MAX];
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
	if (!mailer->dirty && !mailer->moving && !mailer->lit_changed)
		return;

	/*
	 * Only another widget lit: the frame is drawn within the two widgets'
	 * part alone, the rest keeping its pixels (BUG-226; the whole window
	 * when the part cannot be told).
	 */
	partial = 0;
	if (!mailer->dirty && !mailer->moving && mailer->lit_changed && mailer->view.question == ML_QUESTION_NONE)
		partial = kl_ui_take_damage(mailer->ui, &part);
	mailer->lit_changed = 0;

	/* The view, within the part when there is one. */
	mailer->dirty = 0;
	if (partial)
		kl_canvas_clip_push(&mailer->canvas, &part);
	kl_ui_begin(mailer->ui, now_us);
	ml_view_draw(&mailer->view, mailer->ui, &mailer->style, (int)mailer->width, (int)mailer->height, now_us);
	mailer->moving = kl_ui_end(mailer->ui, now_us);
	if (partial)
		kl_canvas_clip_pop(&mailer->canvas);

	/* The question over it. */
	ml_draw_question(mailer, now_us);

	/*
	 * The text input is asked for while a field has the keyboard, and told
	 * where its caret is, so that an input method's candidates and the
	 * on-screen keyboard stay out of its way.
	 */
	wanted = kl_ui_text_wanted(mailer->ui, &caret);
	kl_window_text_input(mailer->window, wanted);
	if (wanted)
		kl_window_text_cursor(mailer->window, caret.x, caret.y, caret.width, caret.height);

	/* The glass's panels for the frame; a compositor without glass leaves the window opaque from the next one. */
	if (mailer->view.glass) {
		count = ml_view_panels(&mailer->view, (int)mailer->width, (int)mailer->height, panels, ML_PANELS_MAX);
		error = kl_window_set_glass(mailer->window, panels, count);
		if (error != 0) {
			ml_log("GLASS failed error=%d", error);
			mailer->view.glass = 0;
			mailer->style.glass = 0;
			mailer->dirty = 1;
		}
	}

	/* The frame shown, by the part alone when it was drawn so (BUG-221). */
	present_part = NULL;
	if (partial)
		present_part = &part;
	status = kl_window_present_part(mailer->window, mailer->pixels, (size_t)mailer->width, present_part);
	if (status == EAGAIN)
		mailer->resized = 1;

	/* The keys no widget took, and the input that met no widget. */
	for (;;) {
		taken = kl_ui_take(mailer->ui, &event);
		if (!taken)
			break;

		/* A key is the view's; it draws again. */
		if (event.kind == KL_EVENT_KEY) {
			ml_view_key(&mailer->view, event.code, event.modifiers, now_us);
			mailer->dirty = 1;
		}
	}
}

/*
 * Reports how long the loop may wait for input (ms): no time while a frame
 * is due, a frame's time while something moves, until the view's notice
 * goes, or a second.
 */
static int
ml_wait(
	const struct ml_window *mailer,
	uint64_t now_us)
{
	int wait;

	/* A frame due now (or its lit part), or requests to carry out. */
	if (mailer->dirty || mailer->lit_changed)
		return 0;
	if (mailer->view.request_count != 0U)
		return 0;

	/* Something moving. */
	if (mailer->moving)
		return ML_MOVING_MS;

	/* The view's notice, or a second. */
	wait = ml_view_wait(&mailer->view, now_us);
	if (wait < 0 || wait > ML_IDLE_MS)
		wait = ML_IDLE_MS;

	/* The time to wait. */
	return wait;
}

/*
 * Reads the accounts, opens the desktop's settings and starts the thread
 * that gets the mail, then asks it for every account's mail.  Without an
 * account the view shows the form of a new one.
 */
static void
ml_servers_start(
	struct ml_window *mailer)
{
	struct ml_account_config accounts[ML_ACCOUNTS_MAX];
	size_t count;
	size_t index;
	int error;

	/* The accounts on the disk. */
	count = 0;
	error = ml_config_folder(mailer->folder, sizeof(mailer->folder));
	if (error == 0)
		error = ml_accounts_load(mailer->folder, accounts, ML_ACCOUNTS_MAX, &count);
	if (error != 0)
		ml_log("ACCOUNTS failed error=%d", error);
	for (index = 0; index < count; index++)
		(void)ml_store_add_account(&accounts[index]);
	ml_log("ACCOUNTS count=%zu", count);
	if (count == 0U)
		mailer->view.adding = 1;

	/* The desktop's settings: whether the browser may fill in sign-in codes, and its changes. */
	mailer->settings = kl_settings_open(kl_app_display(mailer->app), NULL);
	if (mailer->settings != NULL) {
		mailer->view.codes_allowed = kl_settings_get_int(mailer->settings, ML_CODES_SETTING, 0);
		(void)kl_settings_watch(mailer->settings, ML_CODES_SETTING, ml_codes_changed, mailer, NULL);
	}

	/* The thread, which gets every account's mail first by itself. */
	mailer->trust_account = -1;
	ml_sync_begin(mailer);
}

/* Starts the thread with the accounts of the store and watches its results. */
static void
ml_sync_begin(
	struct ml_window *mailer)
{
	const struct ml_account_config *accounts;
	size_t count;
	int error;

	/* The thread. */
	accounts = ml_accounts(&count);
	error = ml_sync_start(accounts, count, &mailer->sync);
	if (error != 0) {
		ml_log("SYNC failed error=%d", error);
		(void)snprintf(mailer->view.status, sizeof(mailer->view.status), "Mail cannot reach the servers (%d).", error);
		mailer->sync = NULL;
		return;
	}

	/* Its results' descriptor watched by the application's loop. */
	error = kl_app_watch_fd(mailer->app, ml_sync_fd(mailer->sync), KL_APP_FD_READ);
	if (error != 0)
		ml_log("SYNC watch error=%d", error);

	/* The thread gets every account's mail first by itself. */
	if (count != 0U)
		(void)snprintf(mailer->view.status, sizeof(mailer->view.status), "Getting mail...");
}

/*
 * Starts the thread again with the accounts as they are now (an account
 * changed, removed or trusted): the old one ends its sessions, and the
 * new one gets every account's mail (the store keeps what it has).
 */
static void
ml_servers_restart(
	struct ml_window *mailer)
{
	/* The old thread, its descriptor no longer watched. */
	if (mailer->sync != NULL) {
		(void)kl_app_watch_fd(mailer->app, ml_sync_fd(mailer->sync), 0U);
		ml_sync_stop(mailer->sync);
		mailer->sync = NULL;
	}

	/* The new one. */
	ml_sync_begin(mailer);
	ml_log("SYNC restarted");
}

/* Takes the thread's results into the store and the view. */
static void
ml_results(
	struct ml_window *mailer)
{
	struct ml_account_config config;
	struct ml_result result;
	size_t count;
	int taken;
	int index;
	int error;

	/* Each result. */
	for (;;) {
		taken = ml_sync_take(mailer->sync, &result);
		if (!taken)
			break;
		mailer->dirty = 1;

		/* Each kind. */
		switch (result.kind) {
		case ML_RESULT_MESSAGE:
			ml_result_message(mailer, &result);
			break;
		case ML_RESULT_REFRESHED:
			ml_log("REFRESHED account=%d", result.account);
			ml_status_time(mailer, "Updated");
			break;
		case ML_RESULT_SENT:
			ml_log("SENT account=%d", result.account);
			mailer->view.composing = 0;
			ml_view_notice(&mailer->view, "Message sent.", kl_clock_us());
			break;
		case ML_RESULT_SIGNED_IN:
			/* The account that worked: kept, shown, and its mail got. */
			config = mailer->pending;
			index = ml_store_add_account(&config);
			ml_log("SIGNED-IN account=%d", index);
			(void)ml_accounts(&count);
			if (index >= 0) {
				error = ml_accounts_save(mailer->folder, ml_accounts(&count), count);
				if (error != 0)
					ml_log("ACCOUNTS save-failed error=%d", error);
				mailer->view.account = index;
				mailer->view.folder = ML_INBOX;
				mailer->view.selected = -1;
				mailer->view.adding = 0;
				(void)snprintf(mailer->view.status, sizeof(mailer->view.status), "Getting mail...");
			}

			/* The tried account is not needed after (its password goes). */
			memset(&mailer->pending, 0, sizeof(mailer->pending));
			break;
		case ML_RESULT_CHECKED:
			/* An account's new settings work: taken. */
			ml_edited(mailer, result.account);
			break;
		case ML_RESULT_FAILED:
			/* What failed, in the status and as a notice; a certificate the user may trust is asked about. */
			ml_log("FAILED account=%d error=%d", result.account, result.error);
			(void)snprintf(mailer->view.status, sizeof(mailer->view.status), "%s", result.text);
			ml_view_notice(&mailer->view, result.text, kl_clock_us());
			if (result.error == ML_ERROR_UNTRUSTED)
				ml_ask_trust(mailer, &result);
			break;
		default:
			break;
		}

		/* What the result held. */
		ml_sync_release(&result);
	}
}

/*
 * Keeps a message the thread got; a new arrival is told as a
 * notification, and its sign-in code to the compositor.
 */
static void
ml_result_message(
	struct ml_window *mailer,
	const struct ml_result *result)
{
	struct kl_mail_arrival arrival;
	struct kl_system *system;
	const struct ml_account_config *accounts;
	char title[ML_TEXT_MAX + 16U];
	size_t count;
	long found;
	int error;

	/* One kept already (a fetch after a move gives its folder again). */
	found = ml_store_find(result->account, result->folder, result->uid);
	if (found >= 0)
		return;

	/* Kept. */
	error = ml_store_add_parsed(result->account, result->folder, result->uid, result->flags, &result->parsed, time(NULL));
	if (error != 0) {
		ml_log("STORE failed error=%d", error);
		return;
	}

	/* The log the tests read (no words of the message). */
	ml_log("MESSAGE account=%d folder=%s uid=%lu arrived=%d code=%d", result->account, ml_folder_name(result->folder), (unsigned long)result->uid, result->arrived, result->parsed.code[0] != '\0');

	/* Only a new unread arrival is told. */
	if (!result->arrived || (result->flags & ML_UNREAD) == 0U)
		return;

	/* A notification. */
	(void)snprintf(title, sizeof(title), "New mail from %s", result->parsed.from_name);
	(void)kl_app_notify(mailer->app, title, result->parsed.subject);

	/* Its sign-in code to the compositor, for the readers the user allows. */
	if (result->parsed.code[0] == '\0')
		return;
	system = kl_app_system(mailer->app);
	if (system == NULL)
		return;
	accounts = ml_accounts(&count);
	memset(&arrival, 0, sizeof(arrival));
	arrival.account = "";
	if ((size_t)result->account < count)
		arrival.account = accounts[result->account].name;
	arrival.from = result->parsed.from_name;
	arrival.subject = result->parsed.subject;
	arrival.code = result->parsed.code;
	error = kl_system_mail_arrived(system, &arrival, NULL);
	ml_log("ARRIVED told error=%d", error);
	(void)kl_system_dispatch(system, NULL);
}

/* Carries out what the view asked. */
static void
ml_requests(
	struct ml_window *mailer)
{
	struct ml_request request;
	struct ml_message *message;
	struct ml_job job;
	int taken;

	/* Each request. */
	for (;;) {
		taken = ml_view_take_request(&mailer->view, &request);
		if (!taken)
			break;
		mailer->dirty = 1;

		/* The sign-in codes' switch needs no server. */
		if (request.action == ML_ACTION_CODES) {
			if (mailer->settings != NULL)
				(void)kl_settings_set_int(mailer->settings, ML_CODES_SETTING, mailer->view.codes_allowed, NULL);
			continue;
		}

		/* An account removed (the thread is started again without it). */
		if (request.action == ML_ACTION_REMOVE_ACCOUNT) {
			ml_remove_account(mailer, (int)request.message);
			continue;
		}

		/* The answer about a certificate. */
		if (request.action == ML_ACTION_TRUST) {
			ml_trust(mailer, (int)request.message);
			continue;
		}

		/* Without the thread nothing reaches a server. */
		if (mailer->sync == NULL) {
			ml_view_notice(&mailer->view, "Mail cannot reach the servers.", kl_clock_us());
			continue;
		}

		/* Each kind. */
		switch (request.action) {
		case ML_ACTION_SEND:
			ml_request_send(mailer);
			break;
		case ML_ACTION_GET:
			ml_refresh_all(mailer);
			break;
		case ML_ACTION_ARCHIVE:
			ml_request_move(mailer, request.message, ML_ARCHIVE);
			break;
		case ML_ACTION_DELETE:
			ml_request_move(mailer, request.message, ML_TRASH);
			break;
		case ML_ACTION_SEEN:
			/* Marked read on the server. */
			message = ml_store_at(request.message);
			if (message == NULL || message->uid == 0U)
				break;
			memset(&job, 0, sizeof(job));
			job.kind = ML_JOB_SEEN;
			job.account = message->account;
			job.folder = message->folder;
			job.uid = message->uid;
			(void)ml_sync_queue(mailer->sync, &job);
			break;
		case ML_ACTION_SIGN_IN:
			/* A new account signs in; an account edited has its new settings tried. */
			if (mailer->view.editing >= 0) {
				ml_request_check(mailer);
				break;
			}

			/* A new account. */
			ml_request_sign_in(mailer);
			break;
		default:
			break;
		}
	}
}

/* Writes the message of the view and asks the thread to send it from the account shown. */
static void
ml_request_send(
	struct ml_window *mailer)
{
	const struct ml_account_config *accounts;
	struct ml_job job;
	size_t count;
	int error;

	/* An account to send from, and someone to send to. */
	accounts = ml_accounts(&count);
	if ((size_t)mailer->view.account >= count) {
		ml_view_notice(&mailer->view, "Add an account to send mail.", kl_clock_us());
		return;
	}

	/* Someone to send to. */
	if (mailer->view.to.length == 0U) {
		ml_view_notice(&mailer->view, "Write whom the message is to.", kl_clock_us());
		return;
	}

	/* The message and its receivers. */
	memset(&job, 0, sizeof(job));
	job.kind = ML_JOB_SEND;
	job.account = mailer->view.account;
	error = ml_compose(&accounts[mailer->view.account], mailer->view.to.text, mailer->view.cc.text, mailer->view.subject.text,
	    mailer->view.body.text, mailer->view.reply_id, time(NULL), &job.raw, &job.length);
	if (error != 0) {
		ml_view_notice(&mailer->view, "The message could not be written.", kl_clock_us());
		return;
	}

	/* The receivers: To and Cc. */
	(void)snprintf(job.receivers, sizeof(job.receivers), "%s,%s", mailer->view.to.text, mailer->view.cc.text);

	/* To the thread (it frees the bytes). */
	error = ml_sync_queue(mailer->sync, &job);
	if (error != 0) {
		free(job.raw);
		return;
	}

	/* The status while it is sent. */
	(void)snprintf(mailer->view.status, sizeof(mailer->view.status), "Sending...");
	ml_log("SEND queued bytes=%zu", job.length);
}

/*
 * Moves a message to the archive or the trash: hidden at once, moved on
 * the server.  A message of the trash deleted is deleted for good
 * (ws177-p015).
 */
static void
ml_request_move(
	struct ml_window *mailer,
	long index,
	enum ml_folder to_folder)
{
	struct ml_message *message;
	struct ml_job job;

	/* The message. */
	message = ml_store_at(index);
	if (message == NULL)
		return;

	/* Deleted in the trash: for good, on the server too. */
	if (message->folder == ML_TRASH && to_folder == ML_TRASH) {
		if (message->uid != 0U) {
			memset(&job, 0, sizeof(job));
			job.kind = ML_JOB_DELETE;
			job.account = message->account;
			job.folder = message->folder;
			job.uid = message->uid;
			(void)ml_sync_queue(mailer->sync, &job);
		}

		/* Hidden here. */
		message->folder = ML_FOLDERS;
		ml_log("DELETE message=%ld", index);
		ml_view_notice(&mailer->view, "The message is deleted for good.", kl_clock_us());
		return;
	}

	/* Already in that folder. */
	if (message->folder == to_folder)
		return;

	/* On the server (the copy comes back as the folder's new message). */
	if (message->uid != 0U) {
		memset(&job, 0, sizeof(job));
		job.kind = ML_JOB_MOVE;
		job.account = message->account;
		job.folder = message->folder;
		job.uid = message->uid;
		job.to_folder = to_folder;
		(void)ml_sync_queue(mailer->sync, &job);
	}

	/* Hidden here. */
	message->folder = ML_FOLDERS;
	ml_log("MOVE message=%ld to=%s", index, ml_folder_name(to_folder));
}

/* Asks the thread to try the form's new account; it is kept when it works. */
static void
ml_request_sign_in(
	struct ml_window *mailer)
{
	struct ml_account_config config;
	struct ml_job job;
	int error;

	/* The form's account. */
	error = ml_form_config(mailer, &config);
	if (error != 0)
		return;

	/* Tried by the thread; kept when it works. */
	memset(&job, 0, sizeof(job));
	job.kind = ML_JOB_SIGN_IN;
	job.account = -1;
	job.config = config;
	mailer->pending = config;
	error = ml_sync_queue(mailer->sync, &job);
	memset(&config, 0, sizeof(config));
	memset(&job, 0, sizeof(job));
	if (error != 0)
		return;
	(void)snprintf(mailer->view.status, sizeof(mailer->view.status), "Signing in...");
	ml_log("SIGN-IN queued");
}

/* Asks the thread to try the form's new settings of the account edited; they are taken when they work. */
static void
ml_request_check(
	struct ml_window *mailer)
{
	const struct ml_account_config *accounts;
	struct ml_account_config config;
	struct ml_job job;
	size_t count;
	int same;
	int error;

	/* The account edited. */
	accounts = ml_accounts(&count);
	if (mailer->view.editing < 0 || (size_t)mailer->view.editing >= count)
		return;

	/* The form's settings. */
	error = ml_form_config(mailer, &config);
	if (error != 0)
		return;

	/* A server that stays the same keeps the certificate the user trusted. */
	same = strcmp(config.imap.host, accounts[mailer->view.editing].imap.host);
	if (same == 0 && config.imap.port == accounts[mailer->view.editing].imap.port)
		(void)snprintf(config.imap.pin, sizeof(config.imap.pin), "%s", accounts[mailer->view.editing].imap.pin);
	same = strcmp(config.smtp.host, accounts[mailer->view.editing].smtp.host);
	if (same == 0 && config.smtp.port == accounts[mailer->view.editing].smtp.port)
		(void)snprintf(config.smtp.pin, sizeof(config.smtp.pin), "%s", accounts[mailer->view.editing].smtp.pin);

	/* Tried by the thread on a session of its own. */
	memset(&job, 0, sizeof(job));
	job.kind = ML_JOB_CHECK;
	job.account = mailer->view.editing;
	job.config = config;
	mailer->pending = config;
	error = ml_sync_queue(mailer->sync, &job);
	memset(&config, 0, sizeof(config));
	memset(&job, 0, sizeof(job));
	if (error != 0)
		return;
	(void)snprintf(mailer->view.status, sizeof(mailer->view.status), "Checking the account...");
	ml_log("CHECK queued account=%d", mailer->view.editing);
}

/*
 * Reads the form into an account: the servers by the address's domain when
 * they are not written.  Returns 0, or -1 with the status saying what is
 * missing.
 */
static int
ml_form_config(
	struct ml_window *mailer,
	struct ml_account_config *config)
{
	const char *domain;
	char server[ML_TEXT_MAX + 8U];
	int error;

	/* An address with a domain, and a password. */
	domain = strchr(mailer->view.setup_address.text, '@');
	if (domain == NULL || domain[1] == '\0' || mailer->view.setup_password.length == 0U) {
		(void)snprintf(mailer->view.status, sizeof(mailer->view.status), "Write the email address and its password.");
		return -1;
	}

	/* The domain after the at sign. */
	domain++;

	/* The account. */
	memset(config, 0, sizeof(*config));
	(void)snprintf(config->name, sizeof(config->name), "%s", mailer->view.setup_name.text);
	if (config->name[0] == '\0')
		(void)snprintf(config->name, sizeof(config->name), "%s", mailer->view.setup_address.text);
	(void)snprintf(config->address, sizeof(config->address), "%s", mailer->view.setup_address.text);
	(void)snprintf(config->user, sizeof(config->user), "%s", mailer->view.setup_address.text);
	(void)snprintf(config->password, sizeof(config->password), "%s", mailer->view.setup_password.text);

	/* The IMAP server, imap.<domain> when it is not written. */
	(void)snprintf(server, sizeof(server), "%s", mailer->view.setup_imap.text);
	if (server[0] == '\0')
		(void)snprintf(server, sizeof(server), "imap.%s", domain);
	error = ml_server_parse(server, 993U, &config->imap);

	/* The SMTP server, smtp.<domain> when it is not written. */
	(void)snprintf(server, sizeof(server), "%s", mailer->view.setup_smtp.text);
	if (server[0] == '\0')
		(void)snprintf(server, sizeof(server), "smtp.%s", domain);
	if (error == 0)
		error = ml_server_parse(server, 465U, &config->smtp);
	if (error != 0) {
		(void)snprintf(mailer->view.status, sizeof(mailer->view.status), "A server is not written right.");
		return -1;
	}

	/* Succeeded: the form's account is read. */
	return 0;
}

/*
 * Takes the new settings of an account edited that work: kept in place of
 * the old ones, its messages got again from its servers (the thread is
 * started again), and the password of an address it no longer has
 * forgotten.
 */
static void
ml_edited(
	struct ml_window *mailer,
	int index)
{
	struct ml_account_config old;
	const struct ml_account_config *accounts;
	size_t count;
	int same;
	int error;

	/* The account as it was. */
	accounts = ml_accounts(&count);
	if (index < 0 || (size_t)index >= count)
		return;
	old = accounts[index];

	/* Its new settings, its old messages gone. */
	(void)ml_store_set_account(index, &mailer->pending);
	ml_store_drop_messages(index);
	memset(&mailer->pending, 0, sizeof(mailer->pending));

	/* Kept on the disk; a changed address's password forgotten. */
	accounts = ml_accounts(&count);
	error = ml_accounts_save(mailer->folder, accounts, count);
	if (error != 0)
		ml_log("ACCOUNTS save-failed error=%d", error);
	same = strcmp(old.address, accounts[index].address);
	if (same != 0)
		(void)ml_secret_save(mailer->folder, old.address, "");
	memset(&old, 0, sizeof(old));

	/* The view: the form closed and emptied (the password goes from it), the account's inbox. */
	ml_view_action(&mailer->view, ML_ACTION_CANCEL, kl_clock_us());
	mailer->view.account = index;
	mailer->view.folder = ML_INBOX;
	mailer->view.selected = -1;
	ml_log("EDITED account=%d", index);
	ml_view_notice(&mailer->view, "The account is changed.", kl_clock_us());

	/* Its mail got again. */
	ml_servers_restart(mailer);
}

/*
 * Removes an account: from the store with its messages, from the disk
 * with its password, and from the thread (started again without it).
 */
static void
ml_remove_account(
	struct ml_window *mailer,
	int index)
{
	char address[ML_TEXT_MAX];
	const struct ml_account_config *accounts;
	size_t count;
	int error;

	/* The account. */
	accounts = ml_accounts(&count);
	if (index < 0 || (size_t)index >= count)
		return;
	(void)snprintf(address, sizeof(address), "%s", accounts[index].address);

	/* Gone from the store, with its messages. */
	error = ml_store_remove_account(index);
	if (error != 0)
		return;

	/* Gone from the disk, and its password forgotten. */
	accounts = ml_accounts(&count);
	error = ml_accounts_save(mailer->folder, accounts, count);
	if (error != 0)
		ml_log("ACCOUNTS save-failed error=%d", error);
	(void)ml_secret_save(mailer->folder, address, "");

	/* The view: the form closed and emptied, the first account's inbox, or the form of a new one when none is left. */
	ml_view_action(&mailer->view, ML_ACTION_CANCEL, kl_clock_us());
	mailer->view.account = 0;
	mailer->view.folder = ML_INBOX;
	mailer->view.selected = -1;
	if (count == 0U)
		mailer->view.adding = 1;
	ml_log("REMOVED account=%d count=%zu", index, count);
	ml_view_notice(&mailer->view, "The account is removed.", kl_clock_us());

	/* The thread without it. */
	ml_servers_restart(mailer);
}

/*
 * Asks the user whether to trust a server's certificate that does not
 * verify, naming the server and the certificate's fingerprint (one the
 * user did not trust in this run is not asked about again).
 */
static void
ml_ask_trust(
	struct ml_window *mailer,
	const struct ml_result *result)
{
	char grouped[ML_PIN_MAX + ML_PIN_MAX / 2U];
	char body[ML_TEXT_MAX * 2U];
	char title[ML_TEXT_MAX + 32U];
	size_t index;
	size_t at;
	int same;

	/* Nothing to trust without the certificate's fingerprint. */
	if (result->fingerprint[0] == '\0')
		return;

	/* Not trusted before in this run. */
	same = strcmp(result->fingerprint, mailer->declined);
	if (same == 0)
		return;

	/* What would be trusted. */
	mailer->trust_account = result->account;
	mailer->trust_smtp = result->smtp;
	(void)snprintf(mailer->trust_fingerprint, sizeof(mailer->trust_fingerprint), "%s", result->fingerprint);

	/* The fingerprint in pairs, as certificates' viewers show it. */
	at = 0;
	for (index = 0; result->fingerprint[index] != '\0' && at + 3U < sizeof(grouped); index++) {
		if (index != 0U && index % 2U == 0U) {
			grouped[at] = ':';
			at++;
		}

		/* The digit. */
		grouped[at] = result->fingerprint[index];
		at++;
	}

	/* Its end. */
	grouped[at] = '\0';

	/* The question. */
	(void)snprintf(title, sizeof(title), "Trust the certificate of %.200s?", result->host);
	(void)snprintf(body, sizeof(body),
	    "Mail cannot verify it: %.200s Trust it only for a server you know, such as your own. SHA-256 %s",
	    result->text, grouped);
	ml_view_ask(&mailer->view, ML_QUESTION_TRUST, title, body);
}

/*
 * Carries out the answer about a certificate: trusted, it is kept with
 * the account (the form's account is tried again with it; an account's
 * is kept on the disk and the thread is started again); not trusted, it
 * is not asked about again in the run.
 */
static void
ml_trust(
	struct ml_window *mailer,
	int trusted)
{
	struct ml_account_config config;
	const struct ml_account_config *accounts;
	struct ml_server *server;
	struct ml_job job;
	size_t count;
	int error;

	/* Not trusted. */
	if (!trusted) {
		(void)snprintf(mailer->declined, sizeof(mailer->declined), "%s", mailer->trust_fingerprint);
		mailer->trust_fingerprint[0] = '\0';
		ml_log("TRUST trusted=0");
		return;
	}

	/* The form's account (new or edited): tried again with the certificate. */
	if (mailer->trust_account < 0) {
		(void)snprintf(mailer->pending.imap.pin, sizeof(mailer->pending.imap.pin), "%s", mailer->trust_fingerprint);
		memset(&job, 0, sizeof(job));
		job.kind = ML_JOB_SIGN_IN;
		job.account = -1;
		if (mailer->view.editing >= 0) {
			job.kind = ML_JOB_CHECK;
			job.account = mailer->view.editing;
		}

		/* The account with the certificate, tried again. */
		job.config = mailer->pending;
		error = 0;
		if (mailer->sync != NULL)
			error = ml_sync_queue(mailer->sync, &job);
		memset(&job, 0, sizeof(job));
		mailer->trust_fingerprint[0] = '\0';
		ml_log("TRUST trusted=1 account=-1 error=%d", error);
		return;
	}

	/* An account's server. */
	accounts = ml_accounts(&count);
	if ((size_t)mailer->trust_account >= count)
		return;
	config = accounts[mailer->trust_account];
	server = &config.imap;
	if (mailer->trust_smtp)
		server = &config.smtp;
	(void)snprintf(server->pin, sizeof(server->pin), "%s", mailer->trust_fingerprint);

	/* Kept, on the disk too. */
	(void)ml_store_set_account(mailer->trust_account, &config);
	memset(&config, 0, sizeof(config));
	accounts = ml_accounts(&count);
	error = ml_accounts_save(mailer->folder, accounts, count);
	if (error != 0)
		ml_log("ACCOUNTS save-failed error=%d", error);
	ml_log("TRUST trusted=1 account=%d smtp=%d", mailer->trust_account, mailer->trust_smtp);
	mailer->trust_fingerprint[0] = '\0';

	/* A message that could not be sent is sent again by the user. */
	if (mailer->trust_smtp)
		ml_view_notice(&mailer->view, "The certificate is trusted. Send the message again.", kl_clock_us());

	/* The thread with it. */
	ml_servers_restart(mailer);
}

/* Draws the question asked over the window, on the question's own input (ws177-p015). */
static void
ml_draw_question(
	struct ml_window *mailer,
	uint64_t now_us)
{
	struct kl_event event;
	int moving;
	int taken;

	/* No question. */
	if (mailer->view.question == ML_QUESTION_NONE)
		return;

	/* The dialog. */
	kl_ui_begin(mailer->dialog_ui, now_us);
	ml_view_question(&mailer->view, mailer->dialog_ui, &mailer->style, (int)mailer->width, (int)mailer->height, now_us);
	moving = kl_ui_end(mailer->dialog_ui, now_us);
	if (moving)
		mailer->moving = 1;

	/* What it did not take is nothing. */
	for (;;) {
		taken = kl_ui_take(mailer->dialog_ui, &event);
		if (!taken)
			break;
	}

	/* An answer: the window without the question is drawn next. */
	if (mailer->view.question == ML_QUESTION_NONE)
		mailer->dirty = 1;
}

/* Asks the thread for every account's mail. */
static void
ml_refresh_all(
	struct ml_window *mailer)
{
	struct ml_job job;
	size_t count;
	size_t index;

	/* Each account. */
	(void)ml_accounts(&count);
	for (index = 0; index < count; index++) {
		memset(&job, 0, sizeof(job));
		job.kind = ML_JOB_REFRESH;
		job.account = (int)index;
		(void)ml_sync_queue(mailer->sync, &job);
	}

	/* The status while it works. */
	if (count != 0U)
		(void)snprintf(mailer->view.status, sizeof(mailer->view.status), "Getting mail...");
}

/* Sets the status to a word and the time now ("Updated 09:41"). */
static void
ml_status_time(
	struct ml_window *mailer,
	const char *what)
{
	struct tm parts;
	time_t now;

	/* The local time. */
	now = time(NULL);
	(void)localtime_r(&now, &parts);
	(void)snprintf(mailer->view.status, sizeof(mailer->view.status), "%s %02d:%02d", what, parts.tm_hour, parts.tm_min);
}

/* Follows a change of the sign-in codes' setting (made here or in another program). */
static void
ml_codes_changed(
	void *data,
	const char *key,
	const char *value,
	unsigned flags)
{
	struct ml_window *mailer;

	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(flags);

	/* The switch shows the value. */
	mailer = data;
	mailer->view.codes_allowed = atoi(value);
	mailer->dirty = 1;
}
