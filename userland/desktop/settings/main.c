/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * settings: the Settings application of the desktop, in a
 * Wayland window drawn on the CPU and shown with Vulkan.
 *
 *   settings [--display=NAME] [--font=PATH] [--fallback-font=PATH]
 *            [--width=N] [--height=N] [--timeout-s=N] [--welcome] [PAGE]
 *
 * It opens on Home, or on the page PAGE names (network, about, ...).  Its
 * outcome is one line on standard error: ZSETTINGS DONE with the reason,
 * or ZSETTINGS FAILED naming what failed; ZSETTINGS READY says the first
 * frame is shown.
 *
 * Settings runs once per user (ws089-p016): a second start hands PAGE (or
 * nothing, without one) to the Settings that runs, which opens the page
 * and brings its window to the front, and ends with ZSETTINGS DONE
 * reason=handed-over.
 */

#include "window.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Files, which the Welcome opens at its end (ws164-p002). */
#define MAIN_FILES		KEILAND_BINDIR "/files"

/* The fonts used unless told otherwise (the fallback is optional). */
#define MAIN_FONT		KEILAND_DATADIR "/fonts/keiland.ttf"
#define MAIN_FALLBACK_FONT	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"

/* How many frames in a row may find the swapchain out of date before the program gives up. */
#define MAIN_STALE_LIMIT	8U

/* A frame that takes longer than this is logged, in milliseconds (a lit row's frame on the Pi 4 among them, ws210-p014). */
#define MAIN_SLOW_FRAME_MS	100U

/*
 * The longest single wait for a moment far ahead (the end of a long
 * timeout), in milliseconds; the loop works the rest out again when it
 * wakes.  With nothing due the loop waits without an end.
 */
#define MAIN_LONGEST_WAIT_MS	60000U

/* How often a flight of the touch pad's scrolling moves, at the least (milliseconds, BUG-211). */
#define MAIN_KINETIC_MS	8

/*
 * What the command line asked for.
 */
struct main_options {
	const char *display;
	const char *font;
	const char *fallback;
	unsigned page;
	unsigned page_given;
	int welcome;
	unsigned width;
	unsigned height;
	unsigned timeout;
};

/*
 * The program's parts, for the whole run.  They are file-scope because
 * the window's input queue and the app are too large for the stack.
 */
static struct se_window main_window;
static struct se_present main_present;
static struct se_app main_app;static struct kl_text main_text;

/*
 * The one copy of Settings: the socket later starts hand their page to,
 * from the start to the end of the run; NULL when Settings runs on its
 * own (no private runtime directory).
 */
static struct kl_instance *main_instance;

/* The desktop's appearance watched (ws089-p017): Settings draws in its colours (palette.c). */
static struct kl_appearance *main_appearance;

/*
 * The window's menus in the compositor, opened with the window and closed before
 * it; its service is NULL when the compositor has no System Menu.
 */
static struct se_menu main_menu;

/*
 * The window's titlebar in the compositor (its controls), opened with the window
 * and closed before it; Settings does not run without it.
 */
static struct se_titlebar main_titlebar;

/*
 * The window's glass in the compositor (its panes on the frosted glass), opened
 * with the presenter and closed before the window; without it the window
 * keeps its opaque ground.
 */
static struct se_glass main_glass;

/* The titlebar's event being carried out (too large for the stack's taste). */
static struct se_titlebar_event main_titlebar_event;

/*
 * The frame being drawn: ordinary memory the size of the swapchain, and
 * the canvas over it.  They are remade when the window changes size.
 */
static uint32_t *main_pixels;
static struct kl_canvas main_canvas;

/*
 * When the window's last new size was taken (se_clock's milliseconds) and
 * when its swapchain and canvas were made, kept from the resize until the
 * first frame at that size is shown, which logs them (BUG-306: how long a
 * display's turn leaves the window at its old size); 0 while no new size
 * waits for its frame.
 */
static uint64_t main_resize_began;
static uint64_t main_resize_made;

static int main_parse(int argc, char **argv, struct main_options *options);
static const char *main_value(const char *argument, const char *name);
static int main_number(const char *text, unsigned maximum, unsigned *value);
static int main_loop(const struct main_options *options);
static int main_frame(void);
static void main_slow_frame(uint64_t draw, uint64_t present);
static void main_resize_shown(void);
static void main_text_input(void);
static int main_canvas_make(void);
static int main_timeout(uint64_t now, uint64_t deadline);
static int main_until(uint64_t now, uint64_t moment);
static int main_shorter(int limit, int wait);
static void main_request(void);
static int main_open_files(void);
static void main_state_update(void);
static void main_about_window(void);
static void main_handed_over(void);
static void main_appearance_changed(void *data, unsigned appearance);

/*
 * Runs Settings.
 */
int
main(
	int argc,
	char **argv)
{
	struct main_options options;
	struct se_menu_state menu_state;
	struct se_titlebar_state titlebar_state;
	const char *page_word;
	VkResult result;
	int status;
	int error;

	/* The command line. */
	status = main_parse(argc, argv, &options);
	if (status != 0) {
		fprintf(stderr, "usage: settings [--display=NAME] [--font=PATH] [--fallback-font=PATH] [--width=N] [--height=N] [--timeout-s=N] [--welcome] [PAGE]\n");
		return 2;
	}

	/* One Settings: a later start hands its page to the one that runs and ends; without the socket this one runs on its own. */
	page_word = "";
	if (options.page_given)
		page_word = se_pages[options.page].word;
	if (options.welcome)
		page_word = "welcome";
	error = kl_instance_open("settings", page_word, &main_instance);
	if (error != 0) {
		se_log("INSTANCE alone errno=%d", error);
	} else if (main_instance == NULL) {
		se_log("DONE reason=handed-over page=%s", page_word);
		return 0;
	}

	/* The fonts. */
	error = kl_text_open(&main_text, options.font, options.fallback);
	if (error != 0) {
		fprintf(stderr, "ZSETTINGS FAILED operation=font path=%s error=%d\n", options.font, error);
		kl_instance_close(main_instance);
		return 1;
	}

	/* The window. */
	status = se_window_open(&main_window, options.display, options.width, options.height, "Settings", "settings");
	if (status != 0) {
		fprintf(stderr, "ZSETTINGS FAILED operation=window error=%d\n", errno);
		se_window_close(&main_window);
		kl_text_close(&main_text);
		kl_instance_close(main_instance);
		return 1;
	}

	/* The presenter. */
	result = se_present_open(&main_present, &main_window);
	if (result != VK_SUCCESS) {
		fprintf(stderr, "ZSETTINGS FAILED operation=%s result=%d\n", main_present.operation, (int)result);
		se_present_close(&main_present);
		se_window_close(&main_window);
		kl_text_close(&main_text);
		kl_instance_close(main_instance);
		return 1;
	}

	/*
	 * The pointer's motions merged, and each frame drawn once the
	 * compositor took the last one (ws210-p014 F2): the pointer crossing
	 * the panes while a frame is on its way draws its latest place once,
	 * not a frame queued for every row it crossed (2026-10-12, the Pi 4).
	 */
	error = kl_window_set_pacing(main_window.kui, KL_PACING_MOTION | KL_PACING_FRAME, 0U);
	se_log("PACING motion=1 frame=1 errno=%d", error);

	/* The interface, and what About shows of the window: the graphics device and the screen (the machine's names come from the desktop, ws188-p002). */
	main_app.now = se_clock();
	main_about_window();
	se_ui_init(&main_app, &main_text, options.page);
	if (options.welcome)
		se_welcome_start(&main_app);

	/* The desktop's system (the network and the sound follow it), and the desktop's settings. */
	se_system_open(&main_app, main_window.display);
	se_machine_open(&main_app);
	se_network_open(&main_app);
	se_look_open(&main_app, main_window.display);
	se_sound_open(&main_app);

	/* The desktop's appearance: the colours of the one told now, and a frame again when it changes (light under a compositor without it). */
	error = kl_appearance_open(main_window.display, main_appearance_changed, NULL, &main_appearance);
	if (error != 0)
		se_log("APPEARANCE none errno=%d", error);
	se_palette_set(kl_appearance_get(main_appearance));
	se_log("APPEARANCE appearance=%u", kl_appearance_get(main_appearance));

	/* Glass when the compositor can show the window see-through (the frame's ground is then left clear). */
	main_app.glass = se_glass_open(&main_glass, &main_window, &main_present);

	/* The menus; a window whose menus cannot be made goes on without them. */
	se_ui_menu_state(&main_app, &menu_state);
	error = se_menu_open(&main_menu, &main_window, &menu_state);
	if (error != 0) {
		se_log("MENU failed errno=%d", error);
		se_menu_close(&main_menu);
	}

	/* The titlebar's controls; without the compositor's titlebar Settings does not start. */
	se_ui_titlebar_state(&main_app, &titlebar_state);
	error = se_titlebar_open(&main_titlebar, &main_window, &titlebar_state);
	if (error != 0) {
		fprintf(stderr, "ZSETTINGS FAILED operation=titlebar errno=%d\n", error);
		se_titlebar_close(&main_titlebar);
		se_menu_close(&main_menu);
		se_glass_close(&main_glass);
		se_present_close(&main_present);
		se_window_close(&main_window);
		kl_text_close(&main_text);
		kl_instance_close(main_instance);
		return 1;
	}

	/* The loop, until the window closes. */
	status = main_loop(&options);

	/* Everything goes, the network, Bluetooth and the sound before the system, then the titlebar, the menus and the glass before the window they belong to. */
	se_network_close(&main_app);
	se_bluetooth_close(&main_app);
	se_storage_close(&main_app);
	se_sound_close(&main_app);
	se_system_close(&main_app);
	se_look_close(&main_app);
	se_ui_close(&main_app);
	kl_appearance_close(main_appearance);
	se_titlebar_close(&main_titlebar);
	se_menu_close(&main_menu);
	se_glass_close(&main_glass);
	kl_canvas_release(&main_canvas);
	free(main_pixels);
	se_present_close(&main_present);
	se_window_close(&main_window);
	kl_text_close(&main_text);
	kl_instance_close(main_instance);

	/* Reports how the run ended. */
	if (status != 0)
		return 1;

	/* Succeeded: the window was closed. */
	return 0;
}

/* Reads the command line into the options; returns nonzero for a malformed one. */
static int
main_parse(
	int argc,
	char **argv,
	struct main_options *options)
{
	const struct se_page *page;
	const char *value;
	int status;
	int index;

	/* The defaults. */
	memset(options, 0, sizeof(*options));
	options->font = MAIN_FONT;
	options->fallback = MAIN_FALLBACK_FONT;
	options->page = SE_PAGE_HOME;
	options->width = SE_WIDTH;
	options->height = SE_HEIGHT;

	/* Each argument. */
	for (index = 1; index < argc; index++) {
		/* The compositor's display. */
		value = main_value(argv[index], "--display=");
		if (value != NULL) {
			options->display = value;
			continue;
		}

		/* The main font. */
		value = main_value(argv[index], "--font=");
		if (value != NULL) {
			options->font = value;
			continue;
		}

		/* The fallback font. */
		value = main_value(argv[index], "--fallback-font=");
		if (value != NULL) {
			options->fallback = value;
			continue;
		}

		/* The window's width. */
		value = main_value(argv[index], "--width=");
		if (value != NULL) {
			status = main_number(value, 8192U, &options->width);
			if (status != 0)
				return status;
			continue;
		}

		/* The window's height. */
		value = main_value(argv[index], "--height=");
		if (value != NULL) {
			status = main_number(value, 8192U, &options->height);
			if (status != 0)
				return status;
			continue;
		}

		/* How long the program runs at most (0 for ever). */
		value = main_value(argv[index], "--timeout-s=");
		if (value != NULL) {
			status = main_number(value, 86400U, &options->timeout);
			if (status != 0)
				return status;
			continue;
		}

		/* The Welcome (ws164-p002, the compositor's start at a first login). */
		status = strcmp(argv[index], "--welcome");
		if (status == 0) {
			options->welcome = 1;
			continue;
		}

		/* An unknown option refuses the command line. */
		if (argv[index][0] == '-')
			return -1;

		/* The page to open, which must be one of the pages' words. */
		page = se_page_find(argv[index]);
		if (page == NULL)
			return -1;
		options->page = page->id;
		options->page_given = 1;
	}

	/* A window has some size. */
	if (options->width < 480U || options->height < 360U)
		return -1;

	/* Succeeded: the options are read. */
	return 0;
}

/* Returns what follows an option's name in an argument, or NULL when the argument is another option. */
static const char *
main_value(
	const char *argument,
	const char *name)
{
	size_t length;
	int match;

	/* The name must start the argument. */
	length = strlen(name);
	match = strncmp(argument, name, length);
	if (match != 0)
		return NULL;

	/* Reports the value after it. */
	return argument + length;
}

/* Reads a decimal number no larger than a maximum; nonzero for a malformed one. */
static int
main_number(
	const char *text,
	unsigned maximum,
	unsigned *value)
{
	unsigned long number;
	char *end;

	/* The digits, all of them. */
	errno = 0;
	number = strtoul(text, &end, 10);
	if (errno != 0 ||
	    end == text ||
	    *end != '\0' ||
	    number > maximum)
		return -1;

	/* Succeeded: the number. */
	*value = (unsigned)number;
	return 0;
}

/* Runs the window until it closes (or the timeout passes); returns nonzero when something failed. */
static int
main_loop(
	const struct main_options *options)
{
	struct se_event event;
	uint64_t started;
	uint64_t deadline;
	uint64_t now;
	int inputs;
	int taken;
	int status;
	int pace;
	int timeout;
	int error;

	/* The first frame's canvas. */
	status = main_canvas_make();
	if (status != 0) {
		fprintf(stderr, "ZSETTINGS FAILED operation=canvas\n");
		return -1;
	}

	/* The first frame. */
	status = main_frame();
	if (status != 0)
		return -1;

	/* A later start of Settings arrives as the application's event. */
	if (main_instance != NULL) {
		error = kl_instance_bind(main_instance, main_window.app);
		if (error != 0)
			se_log("INSTANCE bind error=%d", error);
	}

	/* The log line the tests wait for. */
	se_log("READY width=%u height=%u glass=%d page=%s", main_present.extent.width, main_present.extent.height, main_app.glass, se_pages[main_app.page].word);

	/* The moment the run ends when a timeout was given (0: none). */
	started = se_clock();
	deadline = 0U;
	if (options->timeout != 0U)
		deadline = started + (uint64_t)options->timeout * 1000U;

	/* Each round: input, time, and a frame when something changed. */
	for (;;) {
		/* Waits for the compositor, or until something is due. */
		now = se_clock();
		timeout = main_timeout(now, deadline);
		status = se_window_dispatch(&main_window, timeout);
		if (status != 0) {
			se_log("DONE reason=disconnected");
			return 0;
		}

		/* Every input queued (the menus' choices and a held key's repeats among them). */
		now = se_clock();
		inputs = 0;
		for (;;) {
			taken = se_window_take(&main_window, &event);
			if (taken == 0)
				break;
			se_ui_event(&main_app, &event);
			inputs++;
		}

		/* What was done with the titlebar, oldest first, at the time now. */
		main_app.now = now;
		for (;;) {
			taken = se_titlebar_take(&main_titlebar, &main_titlebar_event);
			if (taken == 0)
				break;
			se_ui_titlebar(&main_app, &main_titlebar_event);
			inputs++;
		}

		/* What the window was asked to do: minimizing, zooming, closing. */
		main_request();

		/* The pages later starts handed over, and their window brought to the front. */
		if (main_window.handed_over) {
			main_window.handed_over = 0;
			main_handed_over();
		}

		/* Time passes for the interface (the minute About shows), and the system reports (the network and the sound follow it). */
		se_ui_tick(&main_app, now);
		se_system_poll(&main_app);
		se_machine_poll(&main_app, now);
		se_network_poll(&main_app, now);
		se_look_poll(&main_app, now);
		se_storage_poll(&main_app, now);
		se_sharing_poll(&main_app);
		se_printers_poll(&main_app);
		se_bluetooth_poll(&main_app);
		se_display_poll(&main_app);
		se_sound_poll(&main_app, now);
		if (main_app.dirty != 0)
			inputs++;

		/* The titlebar and the menus show the state after input. */
		if (inputs != 0)
			main_state_update();

		/* The close button ends the run. */
		if (main_window.closed != 0) {
			se_log("DONE reason=close");
			return 0;
		}

		/* So does the timeout, when one was given. */
		if (options->timeout != 0U && now - started >= (uint64_t)options->timeout * 1000U) {
			se_log("DONE reason=timeout");
			return 0;
		}

		/* A new size: a new swapchain and canvas, and a frame. */
		if (main_window.resized != 0) {
			main_window.resized = 0;
			main_resize_began = se_clock();
			status = se_present_resize(&main_present, main_window.width, main_window.height);
			if (status != VK_SUCCESS) {
				fprintf(stderr, "ZSETTINGS FAILED operation=%s result=%d\n", main_present.operation, status);
				return -1;
			}

			/* The canvas to match. */
			status = main_canvas_make();
			if (status != 0)
				return -1;
			main_resize_made = se_clock();
			main_app.dirty = 1;
		}

		/* Nothing changed: no frame. */
		if (main_app.dirty == 0 &&
		    main_app.hover_moved == 0 &&
		    main_app.hover_pending == 0)
			continue;

		/*
		 * A frame once the compositor took the last one (ws210-p014 F2.2):
		 * until then nothing is consumed, so what changes meanwhile is
		 * drawn as one frame in its latest state.
		 */
		pace = kl_window_frame_wait(main_window.kui, kl_clock_us());
		if (pace != 0)
			continue;

		/* The region under the pointer: what changed since the last frame drawn (ws210-p014). */
		se_ui_hover_settle(&main_app);

		/* A frame when something changed, or the lit region alone (BUG-226). */
		if (main_app.dirty != 0 || main_app.hover_pending != 0) {
			status = main_frame();
			if (status != 0)
				return -1;

			/* The first frame at a new size says how long it took. */
			if (main_resize_began != 0U)
				main_resize_shown();
		}
	}
}

/* Draws and shows a frame, remaking the swapchain when it is out of date; nonzero when it cannot be shown. */
static int
main_frame(void)
{
	VkResult result;
	uint64_t started;
	uint64_t drawn;
	uint64_t shown;
	unsigned stale;
	int status;

	/* Tries until the frame is shown, remaking a stale swapchain a few times. */
	for (stale = 0; stale < MAIN_STALE_LIMIT; stale++) {
		/* The frame on the CPU. */
		started = se_clock();
		se_ui_draw(&main_app, &main_canvas);
		drawn = se_clock();

		/* The frame's glass panels, sent to take effect with it. */
		se_glass_refresh(&main_glass, &main_app);

		/* Shown in the window, by the parts it changed when it was drawn within the lit regions alone (BUG-226). */
		result = se_present_frame(&main_present, main_pixels, (size_t)main_present.extent.width, main_app.frame_parts, main_app.frame_part_count);
		shown = se_clock();

		/* A slow frame is logged (a diagnostic: where the time of a frame goes, and how much of the window it drew). */
		if (shown - started > MAIN_SLOW_FRAME_MS)
			main_slow_frame(drawn - started, shown - drawn);

		/* Succeeded: the frame is shown (the retries below are for a stale swapchain only); the text input follows it. */
		if (result == VK_SUCCESS) {
			main_text_input();
			return 0;
		}

		/* Anything but a stale swapchain is a failure. */
		if (result != VK_ERROR_OUT_OF_DATE_KHR) {
			fprintf(stderr, "ZSETTINGS FAILED operation=%s result=%d\n", main_present.operation, (int)result);
			return -1;
		}

		/* A stale swapchain is remade at the window's size, with a canvas to match. */
		result = se_present_resize(&main_present, main_window.width, main_window.height);
		if (result != VK_SUCCESS) {
			fprintf(stderr, "ZSETTINGS FAILED operation=%s result=%d\n", main_present.operation, (int)result);
			return -1;
		}

		/* The canvas to match. */
		status = main_canvas_make();
		if (status != 0)
			return -1;
	}

	/* The swapchain stayed out of date. */
	fprintf(stderr, "ZSETTINGS FAILED operation=stale-swapchain\n");
	return -1;
}

/* Logs a slow frame: its drawing's and its showing's times, and the parts it drew (none: the whole window) with their area in pixels. */
static void
main_slow_frame(
	uint64_t draw,
	uint64_t present)
{
	const struct kl_rect *part;
	unsigned long area;
	unsigned index;

	/* The pixels drawn: the parts', or the whole window's. */
	area = (unsigned long)main_present.extent.width * main_present.extent.height;
	if (main_app.frame_part_count != 0U)
		area = 0UL;
	for (index = 0; index < main_app.frame_part_count; index++) {
		part = &main_app.frame_parts[index];
		area += (unsigned long)part->width * (unsigned long)part->height;
	}

	/* One line. */
	se_log("SLOW-FRAME draw=%lu present=%lu copy=%u acquire=%u queue=%u wait=%u parts=%u area=%lu", (unsigned long)draw, (unsigned long)present, main_present.copy_ms, main_present.acquire_ms, main_present.present_ms, main_present.wait_ms, main_app.frame_part_count, area);
}

/*
 * Logs the first frame shown at the window's new size (BUG-306, a display's
 * turn): the size, how long the swapchain and the canvas took to make, how
 * long until the frame was shown, and when the new size was taken (at_ms,
 * the monotonic clock's milliseconds the compositor's lines use too).
 */
static void
main_resize_shown(void)
{
	uint64_t now;

	/* The line, from the moment the new size was taken. */
	now = se_clock();
	se_log("RESIZE width=%u height=%u swapchain_ms=%llu shown_ms=%llu at_ms=%llu", main_present.extent.width, main_present.extent.height,
	    (unsigned long long)(main_resize_made - main_resize_began), (unsigned long long)(now - main_resize_began),
	    (unsigned long long)main_resize_began);

	/* No new size waits for its frame any more. */
	main_resize_began = 0U;
	main_resize_made = 0U;
}

/*
 * Asks for the window's text input while a text field that takes an input
 * method has the keyboard, at its caret (widgets.c, ws090-p007).
 */
static void
main_text_input(void)
{
	struct kl_ui *fields;

	/* No fields' input: off. */
	fields = se_fields_ui();
	if (fields == NULL) {
		kl_window_text_input(main_window.kui, 0);
		return;
	}

	/* libkeiland's answer: on while a field that takes an input method has the keyboard, at its caret. */
	kl_ui_window_text(fields, main_window.kui);
}

/* Makes the frame's memory and canvas at the swapchain's size; nonzero when memory runs out. */
static int
main_canvas_make(void)
{
	size_t count;
	int error;

	/* The old canvas and memory go. */
	kl_canvas_release(&main_canvas);
	free(main_pixels);

	/* Memory for the swapchain's size. */
	count = (size_t)main_present.extent.width * (size_t)main_present.extent.height;
	main_pixels = calloc(count, sizeof(uint32_t));
	if (main_pixels == NULL)
		return -1;

	/* The canvas over it. */
	error = kl_canvas_init(&main_canvas, main_pixels, (size_t)main_present.extent.width, (int)main_present.extent.width, (int)main_present.extent.height);
	if (error != 0)
		return -1;

	/* Succeeded: frames can be drawn. */
	return 0;
}

/*
 * Reports how long the loop may sleep: not at all while a frame is due,
 * else until the first moment something is due (the end of the run among
 * them), and for ever (-1) when nothing is.  The compositor's events (the
 * desktop's reports among them) and a later start of Settings wake the
 * wait whenever they come, and a held key's repeat shortens it within the
 * application's library.
 */
static int
main_timeout(
	uint64_t now,
	uint64_t deadline)
{
	int interface;
	int network;
	int sound;
	int look;
	int storage;
	int machine;
	int bluetooth;
	int dialog;
	int display;
	int limit;
	int pace;

	/*
	 * A frame the last one asked for (a scroll it corrected), or the lit
	 * region's, is drawn at once, or once the compositor took the last
	 * one (ws210-p014 F2: its answer wakes the wait; a positive pace is
	 * how long it is waited for at most).
	 */
	pace = -1;
	if (main_app.dirty != 0 ||
	    main_app.hover_moved != 0 ||
	    main_app.hover_pending != 0) {
		pace = kl_window_frame_wait(main_window.kui, kl_clock_us());
		if (pace == 0)
			return 0;
	}

	/* A touch pad's scrolling that flies on moves every frame (BUG-211). */
	if (main_app.kinetic.flying != 0)
		return MAIN_KINETIC_MS;

	/* Nothing due yet: the wait has no end, except the end of the run when a timeout was given, and the frame's pace. */
	limit = -1;
	if (deadline != 0U)
		limit = main_until(now, deadline);
	limit = main_shorter(limit, pace);

	/* The minute About shows. */
	interface = se_ui_wait(&main_app, now);
	limit = main_shorter(limit, interface);

	/* While the network wants polls. */
	network = se_network_wait(&main_app);
	limit = main_shorter(limit, network);

	/* While the sound holds something back, or its page follows audiod. */
	sound = se_sound_wait(&main_app);
	limit = main_shorter(limit, sound);

	/* While the Wallpaper page's small copies are being read, so each tile fills soon after its copy. */
	look = se_look_wait(&main_app);
	limit = main_shorter(limit, look);

	/* While a folder or the Trash is counted or emptied (ws089-p023). */
	storage = se_storage_wait(&main_app);
	limit = main_shorter(limit, storage);

	/* While a reading of the computer is due or waited for (ws188-p002). */
	machine = se_machine_wait(&main_app);
	limit = main_shorter(limit, machine);

	/* While the Bluetooth page's scan is to be asked again (ws143-p006). */
	bluetooth = se_bluetooth_wait(&main_app);
	limit = main_shorter(limit, bluetooth);

	/* While the popup's ring turns, or until it has been left alone too long (ws199-p001). */
	dialog = se_dialog_wait(&main_app);
	limit = main_shorter(limit, dialog);

	/* While a trial of a display's resolution counts its seconds down. */
	display = se_display_wait(&main_app);
	limit = main_shorter(limit, display);

	/* Reports the wait, -1 for one without an end. */
	return limit;
}

/* Reports the milliseconds from now until a moment, at most MAIN_LONGEST_WAIT_MS; 0 for a moment already passed. */
static int
main_until(
	uint64_t now,
	uint64_t moment)
{
	uint64_t rest;

	/* A moment passed is due at once. */
	if (moment <= now)
		return 0;

	/* A long wait is cut short; the loop asks again when it wakes. */
	rest = moment - now;
	if (rest > MAIN_LONGEST_WAIT_MS)
		rest = MAIN_LONGEST_WAIT_MS;

	/* Reports the wait. */
	return (int)rest;
}

/* Reports the shorter of two waits in milliseconds, where -1 is a wait without an end. */
static int
main_shorter(
	int limit,
	int wait)
{
	/* A wait without an end shortens nothing. */
	if (wait < 0)
		return limit;

	/* Any wait shortens one without an end. */
	if (limit < 0)
		return wait;

	/* Reports the shorter one. */
	if (wait < limit)
		return wait;
	return limit;
}

/* Starts Files, which opens at Today (the Welcome's end, ws164-p002); returns 0, or the error of a start that failed (logged). */
static int
main_open_files(void)
{
	struct kl_launch_options options;
	const char *arguments[2];
	int error;

	/* Files, on its own (its window is the compositor's to place). */
	arguments[0] = MAIN_FILES;
	arguments[1] = NULL;
	memset(&options, 0, sizeof(options));
	options.argv = arguments;
	error = kl_launch(main_window.app, &options);
	se_log("WELCOME files error=%d", error);
	if (error != 0)
		return error;

	/* Succeeded: Files runs. */
	return 0;
}

/* Carries out what the window was asked to do by an action, once. */
static void
main_request(void)
{
	unsigned request;
	int error;

	/* The request, taken. */
	request = main_app.request;
	main_app.request = SE_REQUEST_NONE;

	/* Each request. */
	switch (request) {
	case SE_REQUEST_MINIMIZE:
		se_window_minimize(&main_window);
		break;
	case SE_REQUEST_ZOOM:
		se_window_zoom(&main_window);
		break;
	case SE_REQUEST_CLOSE:
		/*
		 * The Welcome's last step opens Files (its Today) as the window goes
		 * (ws164-p002); one that cannot be started keeps the window, which
		 * says so (ws177-p007).
		 */
		if (main_app.request_files) {
			main_app.request_files = 0;
			error = main_open_files();
			if (error != 0) {
				se_welcome_files_failed(&main_app, error);
				break;
			}
		}

		/* The window goes. */
		main_window.closed = 1;
		break;
	default:
		break;
	}
}

/* Tells the titlebar and the menus the window's state (each sends only what changed). */
static void
main_state_update(void)
{
	struct se_titlebar_state titlebar;
	struct se_menu_state menu;

	/* The titlebar. */
	se_ui_titlebar_state(&main_app, &titlebar);
	se_titlebar_refresh(&main_titlebar, &titlebar);

	/* The menus. */
	se_ui_menu_state(&main_app, &menu);
	se_menu_refresh(&main_menu, &menu);
}

/* Puts what the window learned into About: the graphics device's name and the screen's mode. */
static void
main_about_window(void)
{
	unsigned hertz;

	/* The graphics device, as Vulkan names it. */
	(void)snprintf(main_app.about.graphics, sizeof(main_app.about.graphics), "%s", main_present.device_name);

	/* The screen's mode, when the compositor told it (the refresh in millihertz, shown in whole hertz). */
	if (main_window.output_width <= 0 || main_window.output_height <= 0)
		return;
	hertz = (unsigned)((main_window.output_refresh + 500) / 1000);
	if (hertz > 0U) {
		(void)snprintf(main_app.about.display, sizeof(main_app.about.display), "%d x %d, %u Hz", (int)main_window.output_width, (int)main_window.output_height, hertz);
	} else {
		(void)snprintf(main_app.about.display, sizeof(main_app.about.display), "%d x %d", (int)main_window.output_width, (int)main_window.output_height);
	}
}

/* Opens the pages later starts of Settings handed over, and brings the window to the front with their tokens. */
static void
main_handed_over(void)
{
	char request[KL_INSTANCE_REQUEST_MAX];
	char token[KL_ACTIVATION_TOKEN_MAX];
	const struct se_page *page;
	int activated;
	int welcome;
	int known;
	int taken;

	/* Each start that handed its page over. */
	for (;;) {
		taken = kl_instance_take(main_instance, request, sizeof(request), token, sizeof(token));
		if (taken == 0)
			break;

		/* Its page, when it named one (a start without a page only brings the window). */
		page = NULL;
		known = 0;
		if (request[0] != '\0')
			page = se_page_find(request);
		if (page != NULL) {
			se_ui_go(&main_app, page->id);
			known = 1;
		}

		/* The Welcome asked of the one that runs (ws164-p002). */
		welcome = strcmp(request, "welcome");
		if (welcome == 0) {
			se_welcome_start(&main_app);
			known = 1;
		}

		/* The window to the front, when the start had a token (the compositor decides). */
		activated = -1;
		if (token[0] != '\0')
			activated = kl_activate(main_window.display, main_window.surface, token);
		main_app.dirty = 1;
		se_log("INSTANCE request page=%s known=%d activate=%d", request, known, activated);
	}
}

/* Takes the desktop's new appearance: Settings' colours become its, and the frame is drawn again. */
static void
main_appearance_changed(
	void *data,
	unsigned appearance)
{
	/* The colours, and a new frame. */
	(void)data;
	se_palette_set(appearance);
	main_app.dirty = 1;
	se_log("APPEARANCE appearance=%u", appearance);
}
