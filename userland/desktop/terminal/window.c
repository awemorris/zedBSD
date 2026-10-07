/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Wayland window of terminal: a window of libkeiland's application
 * (WS131 p018, kl_app; ws090-p011 before), whose surface the terminal
 * draws on with its own Vulkan (KL_PRESENT_NONE).  The application waits
 * for the compositor and the shells' pseudo-terminals together
 * (kl_app_watch_fd).
 *
 * The application queues the window's input; this file takes it into what
 * the main loop reads.  A key press is turned into bytes at once (keys.c)
 * and kept until the main loop writes them to the shell, and a held key's
 * repeats (which the application makes after the compositor's events are
 * in, BUG-111) are typed the same way.  The pointer's presses, releases
 * and motions are kept for the selection (ws035-p093), the wheel as
 * notches of the view's scroll (ws035-p114), and the fingers for touch.c
 * (ws081-p011).  The menus' choices (menu.c), the tabs' (tabs.c), a drop
 * waiting, the end of a drag and the selections' changes come as inputs
 * too (clipboard.c, primary.c: the window's own clipboard and primary
 * selection).
 *
 * The window asks for the input method's text (BUG-155): what it commits
 * goes to the shell like typed keys, and what it composes is kept for the
 * renderer to show at the cursor.
 */

#include "terminal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The evdev codes of Page Up and Page Down, which with Shift scroll the view (ws035-p114). */
#define WINDOW_KEY_PAGEUP	104U
#define WINDOW_KEY_PAGEDOWN	109U

/* F11, which makes the window fullscreen or ends it when the compositor's menu did not take it (BUG-194). */
#define WINDOW_KEY_F11		87U

/* The keys the search bar answers itself (ws128-p006): Escape, Backspace, Enter and the keypad's Enter. */
#define WINDOW_KEY_ESCAPE	1U
#define WINDOW_KEY_BACKSPACE	14U
#define WINDOW_KEY_ENTER	28U
#define WINDOW_KEY_KP_ENTER	96U

/* The evdev codes of the pointer's left and middle buttons. */
#define WINDOW_BUTTON_LEFT	0x110U
#define WINDOW_BUTTON_MIDDLE	0x112U

/*
 * How far the window's wheel moves for one notch of the compositor's (its step of
 * 15 surface units, which libkeiland scales by 4): the terminal scrolls by
 * notches.
 */
#define WINDOW_WHEEL_NOTCH	60.0

static void window_watch(struct terminal_window *window, const int *others, unsigned count);
static void window_event(struct terminal_window *window, const struct kl_window_event *event);
static void window_selection(const struct kl_window_event *event);
static void window_drop_enter(const struct kl_window_event *event);
static void window_press(struct terminal_window *window, uint32_t key);
static void window_text_commit(struct terminal_window *window, const char *text);
static void window_search_key(struct terminal_window *window, uint32_t key);
static void window_search_append(struct terminal_window *window, const char *text, size_t length);
static void window_text_preedit(struct terminal_window *window, const struct kl_window_event *event);
static void window_text_delete(struct terminal_window *window, uint32_t before);
static void window_wheel(struct terminal_window *window, double dy);
static void window_pointer_event(struct terminal_window *window, unsigned kind, const struct kl_window_event *event);
static void window_pad_push(struct terminal_window *window, unsigned type, const struct kl_window_event *event);
static void window_touch_push(struct terminal_window *window, unsigned type, const struct kl_window_event *event);
static uint32_t window_modifiers(unsigned modifiers);

/*
 * Connects to the compositor and makes a toplevel window of a size.
 *
 * Returns 0 once the first configure is acknowledged, or -1 with errno set.
 */
int
terminal_window_open(
	struct terminal_window *window,
	const char *display,
	uint32_t width,
	uint32_t height)
{
	struct kl_window_options options;
	struct kl_app_options app_options;
	struct kl_app_event event;
	int taken;

	/* Nothing held yet. */
	memset(window, 0, sizeof(*window));

	/* The application: the connection to the compositor. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.display = display;
	app_options.application = "terminal";
	window->app = kl_app_open(&app_options);
	if (window->app == NULL)
		return -1;

	/* Its window, the size asked for until the compositor gives one; the terminal draws on it itself. */
	memset(&options, 0, sizeof(options));
	options.title = "Terminal";
	options.width = width;
	options.height = height;
	options.present = KL_PRESENT_NONE;
	window->kui = kl_app_window_create(window->app, &options);
	if (window->kui == NULL)
		return -1;

	/* The keyboard's editing buttons come as the compositor's keys for a terminal (Ctrl+Shift+C and V), not as Ctrl+C. */
	kl_window_edit_by_keys(window->kui);

	/* The window's size and full screen as the first configure left them. */
	kl_window_size(window->kui, &window->width, &window->height);
	window->fullscreen = kl_window_fullscreen(window->kui);

	/*
	 * The input method's text, asked for for good: the whole window is
	 * where the shell's text is typed (BUG-155).  Nothing is composed yet.
	 */
	window->preedit[0] = '\0';
	window->preedit_begin = -1;
	window->preedit_end = -1;
	kl_window_text_input(window->kui, 1);

	/* Text and file names dropped on the window are pasted into the shell (ws035-p088). */
	(void)kl_window_accept_drops(window->kui, KL_DROP_TEXT | KL_DROP_URIS);

	/* What the window's making left queued (its size is already known). */
	for (;;) {
		taken = kl_app_take(window->app, &event);
		if (taken == 0)
			break;
		if (event.kind == KL_APP_WINDOW)
			window_event(window, &event.input);
	}
	window->resized = 0;

	/* Succeeded: the window can be drawn into. */
	return 0;
}

/*
 * Waits for the compositor or another descriptor (the shells') and takes
 * what happened: the window's input, and which descriptors are ready.
 *
 * `timeout` is in milliseconds (-1 waits for ever; a held key's repeat or
 * input already queued waits less); ready[i] tells whether others[i] has
 * something to read or has hung up.  Returns 0, or -1 when the connection
 * is broken.
 */
int
terminal_window_dispatch(
	struct terminal_window *window,
	const int *others,
	unsigned count,
	int timeout,
	int *ready)
{
	struct kl_app_event event;
	unsigned index;
	int status;
	int taken;

	/* The descriptors watched are the shells' now; none is ready yet. */
	window_watch(window, others, count);
	for (index = 0; index < count; index++)
		ready[index] = 0;

	/* The compositor and the shells, together (a held key repeats within, after its release if that came). */
	status = kl_app_dispatch(window->app, timeout);
	if (status != 0)
		return -1;

	/* What happened, in its order: the window's input, and the shells that wrote or hung up. */
	for (;;) {
		taken = kl_app_take(window->app, &event);
		if (taken == 0)
			break;

		/* The window's input. */
		if (event.kind == KL_APP_WINDOW && event.window == window->kui) {
			window_event(window, &event.input);
			continue;
		}

		/* A shell's descriptor. */
		if (event.kind != KL_APP_FD)
			continue;
		for (index = 0; index < count; index++) {
			if (others[index] == event.fd)
				ready[index] = 1;
		}
	}

	/* Whether the compositor made the window fullscreen (the View menu shows it). */
	window->fullscreen = kl_window_fullscreen(window->kui);

	/* Succeeded: the events so far have run. */
	return 0;
}

/*
 * Destroys the window's objects and disconnects.
 */
void
terminal_window_close(
	struct terminal_window *window)
{
	/* The menus, before the window they are shown on. */
	if (window->kui != NULL)
		terminal_menu_close(window);

	/* The window, then the application and its connection. */
	if (window->kui != NULL)
		kl_window_close(window->kui);
	if (window->app != NULL)
		kl_app_close(window->app);
	memset(window, 0, sizeof(*window));
}

/*
 * Adds bytes to what the shell reads next, as if typed (the Session menu's
 * interrupt and end of file); what does not fit is dropped.
 */
void
terminal_window_type(
	struct terminal_window *window,
	const char *bytes,
	size_t length)
{
	/* Only what fits in the buffer. */
	if (length > sizeof(window->input) - window->input_length)
		length = sizeof(window->input) - window->input_length;

	/* The bytes after those typed before. */
	memcpy(window->input + window->input_length, bytes, length);
	window->input_length += length;
}

/*
 * Asks the compositor to make the window fullscreen, or to end it; the
 * configure that follows says what it did.
 */
void
terminal_window_set_fullscreen(
	struct terminal_window *window,
	int fullscreen)
{
	/* On the default output, or back to a window. */
	kl_window_set_fullscreen(window->kui, fullscreen);
}

/*
 * Returns a monotonic time in milliseconds (0 when the clock cannot be read).
 */
uint64_t
terminal_clock(void)
{
	struct timespec now;
	int status;

	/* The monotonic clock. */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return 0U;

	/* Reports it in milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/*
 * Watches the shells' descriptors for reading, as many as there are now:
 * one no longer among them stops being watched.
 */
static void
window_watch(
	struct terminal_window *window,
	const int *others,
	unsigned count)
{
	unsigned kept;
	unsigned index;
	unsigned other;
	int found;
	int error;

	/* The ones watched that are gone stop being watched. */
	kept = 0;
	for (index = 0; index < window->watched_count; index++) {
		found = 0;
		for (other = 0; other < count; other++) {
			if (others[other] == window->watched[index])
				found = 1;
		}
		if (!found) {
			(void)kl_app_watch_fd(window->app, window->watched[index], 0U);
			continue;
		}
		window->watched[kept] = window->watched[index];
		kept++;
	}
	window->watched_count = kept;

	/* The new ones are watched. */
	for (other = 0; other < count && window->watched_count < TERMINAL_TABS; other++) {
		found = 0;
		for (index = 0; index < window->watched_count; index++) {
			if (window->watched[index] == others[other])
				found = 1;
		}
		if (found)
			continue;
		error = kl_app_watch_fd(window->app, others[other], KL_APP_FD_READ);
		if (error != 0)
			continue;
		window->watched[window->watched_count] = others[other];
		window->watched_count++;
	}
}

/* Turns one input of the window into the terminal's. */
static void
window_event(
	struct terminal_window *window,
	const struct kl_window_event *event)
{
	/* Every input carries the modifiers held. */
	window->modifiers = window_modifiers(event->modifiers);

	/* What it is. */
	switch (event->kind) {
	case KL_WINDOW_MOTION:
		/* The pointer's place, and a motion for the main loop. */
		window->pointer_x = (int32_t)event->x;
		window->pointer_y = (int32_t)event->y;
		window_pointer_event(window, TERMINAL_POINTER_MOTION, event);
		break;
	case KL_WINDOW_BUTTON:
		/* The middle button's press pastes the primary selection (ws035-p100). */
		if (event->code == WINDOW_BUTTON_MIDDLE) {
			if (event->pressed)
				window_pointer_event(window, TERMINAL_POINTER_MIDDLE, event);
			break;
		}

		/* Otherwise only the left button, pressed or released. */
		if (event->code != WINDOW_BUTTON_LEFT)
			break;
		if (event->pressed) {
			window_pointer_event(window, TERMINAL_POINTER_PRESS, event);
		} else {
			window_pointer_event(window, TERMINAL_POINTER_RELEASE, event);
		}

		break;
	case KL_WINDOW_AXIS:
		/* A touch pad's fingers scroll as fingers do, with libkeiland's scroller (ws090-p019); a wheel by notches. */
		if (event->axis_source == KL_AXIS_SOURCE_FINGER) {
			window_pad_push(window, TERMINAL_TOUCH_PAD, event);
		} else {
			window_wheel(window, event->dy);
		}
		break;
	case KL_WINDOW_AXIS_STOP:
		window_pad_push(window, TERMINAL_TOUCH_PAD_STOP, event);
		break;
	case KL_WINDOW_KEY:
		/* A press (or a held key's repeat) types; a release does nothing. */
		if (event->pressed)
			window_press(window, event->code);
		break;
	case KL_WINDOW_TEXT_COMMIT:
		window_text_commit(window, event->text);
		break;
	case KL_WINDOW_TEXT_PREEDIT:
		window_text_preedit(window, event);
		break;
	case KL_WINDOW_TEXT_DELETE:
		/* Only bytes before the cursor can be taken back, as Backspace; the terminal has no text after it. */
		window_text_delete(window, event->before);
		break;
	case KL_WINDOW_TOUCH_DOWN:
		window_touch_push(window, TERMINAL_TOUCH_DOWN, event);
		break;
	case KL_WINDOW_TOUCH_MOTION:
		window_touch_push(window, TERMINAL_TOUCH_MOTION, event);
		break;
	case KL_WINDOW_TOUCH_UP:
		window_touch_push(window, TERMINAL_TOUCH_UP, event);
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		window_touch_push(window, TERMINAL_TOUCH_CANCEL, event);
		break;
	case KL_WINDOW_RESIZE:
		/* The size the compositor gave, drawn at from the next frame. */
		kl_window_size(window->kui, &window->width, &window->height);
		window->resized = 1;
		break;
	case KL_WINDOW_CLOSE:
		/* The main loop ends the terminal. */
		window->closed = 1;
		break;
	case KL_WINDOW_ACTION:
		/* A menu's item, for the main loop (menu.c). */
		terminal_menu_chosen(window, event);
		break;
	case KL_WINDOW_TAB:
		/* A tab chosen, closed or new, for the main loop (tabs.c). */
		terminal_tabs_input(window, event);
		break;
	case KL_WINDOW_DROP_ENTER:
		window_drop_enter(event);
		break;
	case KL_WINDOW_DROP:
		/* Dropped: the main loop pastes it (clipboard.c). */
		window->drop_pending = 1;
		break;
	case KL_WINDOW_DRAG_DONE:
		/* The terminal's drag of text ended. */
		printf("ZTERM DRAG done dropped=%u\n", event->code);
		fflush(stdout);
		break;
	case KL_WINDOW_SELECTION:
		window_selection(event);
		break;
	default:
		break;
	}
}

/* Tells the tests a drag the window takes came over it, and what it has. */
static void
window_drop_enter(
	const struct kl_window_event *event)
{
	int uris;
	int text;

	/* The file names and the text it has. */
	uris = 0;
	if ((event->code & KL_DROP_URIS) != 0U)
		uris = 1;
	text = 0;
	if ((event->code & KL_DROP_TEXT) != 0U)
		text = 1;

	/* The log line the tests read. */
	printf("ZTERM DROP enter uris=%d text=%d\n", uris, text);
	fflush(stdout);
}

/* Tells the tests a selection's change: the clipboard's, or the primary selection's offer. */
static void
window_selection(
	const struct kl_window_event *event)
{
	/* The primary selection. */
	if (event->code == KL_SELECTION_PRIMARY) {
		printf("ZTERM PRIMARY offer text=%d\n", event->pressed);
		fflush(stdout);
		return;
	}

	/* The clipboard. */
	printf("ZTERM CLIPBOARD selection text=%d\n", event->pressed);
	fflush(stdout);
}

/* Adds a key's bytes to what the shell reads next. */
static void
window_press(
	struct terminal_window *window,
	uint32_t key)
{
	size_t length;
	int shift;
	int on;

	/* While the search bar is open the keys edit what it looks for (ws128-p006). */
	if (window->search_open) {
		window_search_key(window, key);
		return;
	}

	/* Shift with Page Up scrolls the view a page back instead of typing (ws035-p114). */
	shift = 0;
	if ((window->modifiers & TERMINAL_MODIFIER_SHIFT) != 0U)
		shift = 1;
	if (shift && key == WINDOW_KEY_PAGEUP) {
		window->scroll_pages++;
		return;
	}

	/* Shift with Page Down scrolls a page toward the live screen. */
	if (shift && key == WINDOW_KEY_PAGEDOWN) {
		window->scroll_pages--;
		return;
	}

	/*
	 * F11 alone is the Fullscreen item's key (BUG-194): it reaches the
	 * window only when the compositor's menus did not choose the item (a
	 * desktop without them), and does the same here instead of reaching
	 * the shell.
	 */
	if (key == WINDOW_KEY_F11 && window->modifiers == 0U) {
		on = 1;
		if (window->fullscreen)
			on = 0;
		printf("ZTERM FULLSCREEN key on=%d\n", on);
		terminal_window_set_fullscreen(window, on);
		return;
	}

	/* The bytes, if they fit in what is left of the buffer. */
	length = terminal_key_bytes(key, window->modifiers, window->input + window->input_length, sizeof(window->input) - window->input_length);
	window->input_length += length;

	/* A key typed after a commit stands between it and the cursor: a later deletion cannot take it back. */
	if (length != 0U)
		window->last_commit[0] = '\0';
}

/*
 * Adds the input method's committed text to what the shell reads next, as
 * if typed; text that does not fit whole in what is left of the buffer is
 * dropped whole, so no character is cut.
 */
static void
window_text_commit(
	struct terminal_window *window,
	const char *text)
{
	size_t length;

	/* The text's bytes; while the search bar is open it is what the bar looks for (ws128-p006). */
	length = strlen(text);
	if (window->search_open) {
		window_search_append(window, text, length);
		return;
	}

	/* Whether they fit after those typed before. */
	if (length > sizeof(window->input) - window->input_length) {
		printf("ZTERM IME commit dropped bytes=%u\n", (unsigned)length);
		fflush(stdout);
		return;
	}

	/* The bytes after those typed before; the main loop writes them to the shell. */
	memcpy(window->input + window->input_length, text, length);
	window->input_length += length;

	/* Kept for a deletion that replaces it. */
	(void)snprintf(window->last_commit, sizeof(window->last_commit), "%s", text);

	/* The log line the tests read. */
	printf("ZTERM IME commit bytes=%u text=%s\n", (unsigned)length, text);
	fflush(stdout);
}

/*
 * Answers a key while the search bar is open (ws128-p006): Escape closes
 * it, Enter finds the next older match (Shift+Enter the next newer),
 * Backspace takes back the last character, and a key that types a
 * printable character adds it.  Nothing goes to the shell.
 */
static void
window_search_key(
	struct terminal_window *window,
	uint32_t key)
{
	unsigned char bytes[16];
	size_t length;
	int shift;

	/* Whether Shift is held, which turns Enter the other way. */
	shift = 0;
	if ((window->modifiers & TERMINAL_MODIFIER_SHIFT) != 0U)
		shift = 1;

	/* Each key the bar answers itself. */
	switch (key) {
	case WINDOW_KEY_ESCAPE:
		/* The bar closes; the main loop draws the window without it. */
		window->search_open = 0;
		window->search_edited = 1;
		return;
	case WINDOW_KEY_ENTER:
	case WINDOW_KEY_KP_ENTER:
		/* The next match back through the scrollback, or forward with Shift. */
		window->search_step = 1;
		if (shift)
			window->search_step = -1;
		return;
	case WINDOW_KEY_BACKSPACE:
		/* The last character goes (its continuation bytes, then its first). */
		while (window->search_length > 0U &&
		       ((unsigned char)window->search_query[window->search_length - 1U] & 0xc0U) == 0x80U)
			window->search_length--;
		if (window->search_length > 0U)
			window->search_length--;
		window->search_query[window->search_length] = '\0';
		window->search_edited = 1;
		return;
	default:
		break;
	}

	/* A key that types one printable character of ASCII adds it; any other does nothing. */
	length = terminal_key_bytes(key, window->modifiers & TERMINAL_MODIFIER_SHIFT, bytes, sizeof(bytes));
	if (length != 1U ||
	    bytes[0] < 0x20U ||
	    bytes[0] > 0x7eU)
		return;

	/* The character joins the text. */
	window_search_append(window, (const char *)bytes, 1U);
}

/* Adds text (UTF-8) to what the search bar looks for, whole or not at all. */
static void
window_search_append(
	struct terminal_window *window,
	const char *text,
	size_t length)
{
	/* Text that does not fit whole is left out, so no character is cut. */
	if (window->search_length + length >= sizeof(window->search_query))
		return;

	/* The text after what is there; the main loop looks again. */
	memcpy(window->search_query + window->search_length, text, length);
	window->search_length += length;
	window->search_query[window->search_length] = '\0';
	window->search_edited = 1;
}

/*
 * Takes back bytes before the cursor that the input method or the
 * on-screen keyboard committed last, typing one Backspace for each
 * character among them.  A deletion reaching further back than the last
 * commit is not the terminal's to know (the shell owns the line) and is
 * ignored.
 */
static void
window_text_delete(
	struct terminal_window *window,
	uint32_t before)
{
	size_t length;
	size_t at;
	unsigned characters;
	unsigned index;

	/* Nothing to delete before the cursor. */
	if (before == 0U)
		return;

	/* A deletion longer than the last commit is ignored. */
	length = strlen(window->last_commit);
	if ((size_t)before > length) {
		printf("ZTERM IME delete ignored bytes=%u\n", (unsigned)before);
		fflush(stdout);
		return;
	}

	/* The characters in the last commit's tail: every byte that is not a UTF-8 continuation starts one. */
	characters = 0U;
	for (at = length - (size_t)before; at < length; at++) {
		if (((unsigned char)window->last_commit[at] & 0xc0U) != 0x80U)
			characters++;
	}

	/* One Backspace (DEL, as the key types it) for each, while the buffer has room. */
	for (index = 0U; index < characters; index++) {
		if (window->input_length >= sizeof(window->input))
			break;
		window->input[window->input_length] = 0x7fU;
		window->input_length++;
	}

	/* The tail is gone from the last commit too. */
	window->last_commit[length - (size_t)before] = '\0';

	/* The log line the tests read. */
	printf("ZTERM IME delete bytes=%u characters=%u\n", (unsigned)before, characters);
	fflush(stdout);
}

/* Keeps the input method's text being composed (empty when it goes) for the renderer to show at the cursor. */
static void
window_text_preedit(
	struct terminal_window *window,
	const struct kl_window_event *event)
{
	/* The text and its segment; the main loop draws them. */
	(void)snprintf(window->preedit, sizeof(window->preedit), "%s", event->text);
	window->preedit_begin = event->begin;
	window->preedit_end = event->end;
	window->preedit_changed = 1;

	/* The log line the tests read. */
	printf("ZTERM IME preedit=%s begin=%d end=%d\n", window->preedit, (int)event->begin, (int)event->end);
	fflush(stdout);
}

/*
 * The wheel turns: each notch toward the user (down) goes forward toward
 * the live screen, away from the user back into the scrollback
 * (ws035-p114); a distance short of a notch waits for the rest.
 */
static void
window_wheel(
	struct terminal_window *window,
	double dy)
{
	int notches;

	/* The distance so far, in notches. */
	window->wheel += dy / WINDOW_WHEEL_NOTCH;
	notches = (int)window->wheel;

	/* Whole notches are kept for the main loop, back as positive. */
	window->scroll_notches -= notches;
	window->wheel -= (double)notches;
}

/* Adds a pointer event at the pointer's place for the main loop (a motion after a motion replaces it; a full queue drops it). */
static void
window_pointer_event(
	struct terminal_window *window,
	unsigned kind,
	const struct kl_window_event *event)
{
	struct terminal_pointer_event *kept;
	uint32_t time;

	/* The compositor's time, in milliseconds of the monotonic clock (the low 32 bits). */
	time = (uint32_t)(event->time_us / 1000U);

	/* A motion after a motion replaces it. */
	if (kind == TERMINAL_POINTER_MOTION && window->pointer_event_count > 0U) {
		kept = &window->pointer_events[window->pointer_event_count - 1U];
		if (kept->kind == TERMINAL_POINTER_MOTION) {
			kept->x = window->pointer_x;
			kept->y = window->pointer_y;
			kept->time = time;
			return;
		}
	}

	/* Room for it. */
	if (window->pointer_event_count >= TERMINAL_POINTER_EVENTS)
		return;

	/* Succeeded: kept, a press with its serial. */
	kept = &window->pointer_events[window->pointer_event_count++];
	kept->kind = kind;
	kept->x = window->pointer_x;
	kept->y = window->pointer_y;
	kept->time = time;
	kept->serial = 0U;
	if (kind != TERMINAL_POINTER_MOTION)
		kept->serial = event->serial;
	kept->modifiers = window->modifiers;
}

/* Queues a touch input for touch.c; a full queue drops it. */
static void
window_touch_push(
	struct terminal_window *window,
	unsigned type,
	const struct kl_window_event *event)
{
	struct terminal_touch_event *kept;

	/* A full queue drops the input (the fingers are far ahead of the program). */
	if (window->touch_count >= TERMINAL_TOUCH_EVENTS)
		return;

	/* The input, after the ones before it: its time as the compositor's milliseconds, when it was read, a down's serial. */
	kept = &window->touches[window->touch_count];
	window->touch_count++;
	memset(kept, 0, sizeof(*kept));
	kept->type = type;
	kept->id = event->id;
	kept->x = (float)event->x;
	kept->y = (float)event->y;
	kept->time = (uint32_t)(event->time_us / 1000U);
	kept->arrival = event->arrival_us;
	if (type == TERMINAL_TOUCH_DOWN)
		kept->serial = event->serial;
}

/*
 * Queues a touch pad's scrolling among the touch inputs (ws090-p019), in
 * their order: the move (as a wheel scrolls) at the compositor's time, or
 * the fingers' lift.
 */
static void
window_pad_push(
	struct terminal_window *window,
	unsigned type,
	const struct kl_window_event *event)
{
	struct terminal_touch_event *kept;

	/* A full queue drops the input. */
	if (window->touch_count >= TERMINAL_TOUCH_EVENTS)
		return;

	/* The input, after the ones before it. */
	kept = &window->touches[window->touch_count];
	window->touch_count++;
	memset(kept, 0, sizeof(*kept));
	kept->type = type;
	kept->x = (float)event->dx;
	kept->y = (float)event->dy;
	kept->time = (uint32_t)(event->time_us / 1000U);
	kept->arrival = event->arrival_us;
}

/* Turns libkeiland's modifier bits into wl_keyboard's, which keys.c and the selection read. */
static uint32_t
window_modifiers(
	unsigned modifiers)
{
	uint32_t bits;

	/* Shift, Control and Alt. */
	bits = 0U;
	if ((modifiers & KL_MOD_SHIFT) != 0U)
		bits |= TERMINAL_MODIFIER_SHIFT;
	if ((modifiers & KL_MOD_CTRL) != 0U)
		bits |= TERMINAL_MODIFIER_CONTROL;
	if ((modifiers & KL_MOD_ALT) != 0U)
		bits |= TERMINAL_MODIFIER_ALT;

	/* Reports them. */
	return bits;
}
