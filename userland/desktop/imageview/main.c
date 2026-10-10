/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Image Viewer (ws091): PNG, JPEG and GIF images in a Wayland window,
 * decoded on the CPU and drawn with Vulkan.
 *
 *   imageview [--display=NAME] [--font=PATH] [--width=N] [--height=N]
 *             [--fullscreen] [--timeout-s=N] [FILE|FOLDER]
 *
 * The file is opened from the command line (with the other images of its
 * folder to go through), or with File > Open (Ctrl+O).  The outcome is
 * one line on standard error: IMAGEVIEW DONE with the reason, or
 * IMAGEVIEW FAILED naming what failed; IMAGEVIEW READY says the first
 * frame is shown, and IMAGEVIEW SHOW each image shown.  ws090-p008: the
 * window and its input are libkeiland's (kl_window); the viewer's own
 * presenter draws the image and the canvas on its surface.
 */

#include "window.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The font used unless told otherwise. */
#define MAIN_FONT		KEILAND_DATADIR "/fonts/keiland.ttf"

/* How many frames in a row may find the swapchain out of date before the program gives up. */
#define MAIN_STALE_LIMIT	8U

/* The longest the loop sleeps when nothing is due, in milliseconds. */
#define MAIN_IDLE_MS		1000

/* The application's identity in the compositor and the recent files. */
#define MAIN_APPLICATION	"imageview"

/* The ground: clear over the glass, black when fullscreen, a light slate when the window is opaque (0xAARRGGBB). */
#define MAIN_GROUND_GLASS	0x00000000U
#define MAIN_GROUND_FULLSCREEN	0xff000000U
#define MAIN_GROUND_OPAQUE	kl_theme_choose(0xffe8ecf1U, 0xff16191fU)

/*
 * What the command line asked for.
 */
struct main_options {
	const char *display;
	const char *font;
	const char *file;
	unsigned width;
	unsigned height;
	unsigned timeout;
	int fullscreen;
};

/*
 * The program's parts, for the whole run.  They are file-scope because
 * the viewer is too large for the stack.
 *
 * The window: libkeiland's window (the Wayland connection, the surface and
 * the input queue), from the start of the run to its end.
 */
static struct iv_window main_window;

/* Whether the compositor gave a new size, or asked to close, since the loop last looked. */
static int main_resized;
static int main_closed;

/* The presenter: Vulkan's swapchain over the window, made after it and closed before it. */
static struct iv_present main_present;

/* The viewer: the images and the view, made once the swapchain's size is known. */
static struct iv_app main_app;

/* The font the canvas's words are drawn in, open for the whole run (without it the canvas has no words). */
static struct iv_text main_text;

/*
 * The window's menus in the compositor, opened with the window and closed
 * before it; absent with a compositor without them.
 */
static struct iv_menu main_menu;

/* The window's titlebar controls in the compositor, with the same life as the menus. */
static struct iv_titlebar main_titlebar;

/* The window's glass in the compositor, with the same life (absent without it, or with an opaque swapchain). */
static struct iv_glass main_glass;

/* The touch screen's gestures and scroller, made with the viewer (without them fingers do nothing). */
static struct iv_touch main_touch;

/*
 * libkeiland's file chooser while the viewer waits for it (File > Open), a
 * window of its own over the viewer's; NULL otherwise.
 */
static struct kl_file_chooser *main_chooser;

/* The font the chooser draws with: the viewer's own. */
static const char *main_font;

/* The files the chooser offers: the images the viewer reads first, or every file. */
static const struct kl_file_filter main_filters[] = {
	{ "Images", "png jpg jpeg jpe gif" },
	{ "All Files", NULL }
};

/*
 * The canvas's memory: ordinary memory the size of the swapchain, remade
 * (and the canvas with it) when the window changes size.
 */
static uint32_t *main_pixels;

/* The canvas over main_pixels, which the viewer draws its words and cards into. */
static struct iv_canvas main_canvas;

/*
 * The application (WS131 p017, libkeiland's kl_app): the connection, its
 * one queue of inputs (the window's, the actions of its menus and
 * controls), and the desktop's appearance, which the viewer draws in.
 */
static struct kl_app *main_kl;

/* The frame of an animated image last written to the presenter (it writes a new one when the viewer's serial moves on). */
static unsigned main_frame_serial;

static int main_parse(int argc, char **argv, struct main_options *options);
static const char *main_value(const char *argument, const char *name);
static int main_number(const char *text, unsigned maximum, unsigned *value);
static int main_loop(const struct main_options *options);
static int main_frame(void);
static int main_resize(void);
static void main_window_event(const struct kl_window_event *event);
static void main_event(enum iv_event_type type, const struct kl_window_event *event, struct iv_event *input);
static void main_choose(void);
static void main_chosen(void *data, struct kl_file_chooser *chooser, unsigned result, const char *path, size_t filter);
static int main_canvas_make(void);
static void main_state(struct iv_state *state);
static void main_opened(void);
static void main_share(void);
static void main_openers(void);
static void main_fullscreen(void);
static void main_appearance_changed(void);
static int main_image(void);

/*
 * Runs Image Viewer.
 */
int
main(
	int argc,
	char **argv)
{
	struct main_options options;
	struct kl_app_options app_options;
	struct kl_window_options window_options;
	struct iv_state state;
	VkResult result;
	int status;
	int error;
	int glass;

	/* The command line. */
	status = main_parse(argc, argv, &options);
	if (status != 0) {
		fprintf(stderr, "usage: imageview [--display=NAME] [--font=PATH] [--width=N] [--height=N] [--fullscreen] [--timeout-s=N] [FILE|FOLDER]\n");
		return 2;
	}

	/* The font; without it the viewer shows the images and no words of its own. */
	error = iv_text_open(&main_text, options.font);
	if (error != 0)
		iv_log("FONT missing path=%s error=%d", options.font, error);
	main_font = options.font;

	/* The application: the connection to the compositor. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.display = options.display;
	app_options.application = MAIN_APPLICATION;
	main_kl = kl_app_open(&app_options);
	if (main_kl == NULL) {
		fprintf(stderr, "IMAGEVIEW FAILED operation=app error=%d\n", errno);
		iv_text_close(&main_text);
		return 1;
	}

	/* The window, whose surface the viewer's own presenter draws on. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Image Viewer";
	window_options.width = options.width;
	window_options.height = options.height;
	window_options.present = KL_PRESENT_NONE;
	main_window.kui = kl_app_window_create(main_kl, &window_options);
	if (main_window.kui == NULL) {
		fprintf(stderr, "IMAGEVIEW FAILED operation=window error=%d\n", errno);
		kl_app_close(main_kl);
		iv_text_close(&main_text);
		return 1;
	}

	/* The presenter. */
	result = iv_present_open(&main_present, &main_window);
	if (result != VK_SUCCESS) {
		fprintf(stderr, "IMAGEVIEW FAILED operation=%s result=%d\n", main_present.operation, (int)result);
		iv_present_close(&main_present);
		kl_window_close(main_window.kui);
		kl_app_close(main_kl);
		iv_text_close(&main_text);
		return 1;
	}

	/* The viewer at the swapchain's size, knowing how large a texture may be and whether the window is glass. */
	iv_app_init(&main_app, &main_text, (int)main_present.extent.width, (int)main_present.extent.height);
	main_app.max_dimension = main_present.max_dimension;
	glass = iv_glass_open(&main_glass, &main_window, &main_present, &main_app);
	main_app.glass = glass;

	/* The file given on the command line. */
	if (options.file != NULL)
		(void)iv_app_open(&main_app, options.file);

	/* The touch screen; without memory for it the fingers do nothing. */
	error = iv_touch_open(&main_touch);
	if (error != 0)
		iv_log("TOUCH failed errno=%d", error);

	/* The menus and the titlebar; a window without them goes on with its keys. */
	main_state(&state);
	error = iv_menu_open(&main_menu, &main_window, &state);
	if (error != 0) {
		iv_log("MENU failed errno=%d", error);
		iv_menu_close(&main_menu);
	}

	/* The titlebar's controls. */
	error = iv_titlebar_open(&main_titlebar, &main_window, &state);
	if (error != 0) {
		iv_log("TITLEBAR failed errno=%d", error);
		iv_titlebar_close(&main_titlebar);
	}

	/* Fullscreen from the start, when asked. */
	if (options.fullscreen)
		kl_window_set_fullscreen(main_window.kui, 1);

	/* The loop, until the window closes. */
	status = main_loop(&options);

	/* Everything goes, the chooser, the titlebar, the menus, the glass and the viewer before the window they belong to. */
	kl_file_chooser_destroy(main_chooser);
	main_chooser = NULL;
	iv_titlebar_close(&main_titlebar);
	iv_menu_close(&main_menu);
	iv_glass_close(&main_glass);
	iv_touch_close(&main_touch);
	iv_app_release(&main_app);
	free(main_pixels);
	iv_present_close(&main_present);
	kl_window_close(main_window.kui);
	kl_app_close(main_kl);
	iv_text_close(&main_text);

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
	const char *value;
	int status;
	int index;
	int match;

	/* The defaults. */
	memset(options, 0, sizeof(*options));
	options->font = MAIN_FONT;
	options->width = IV_WIDTH;
	options->height = IV_HEIGHT;

	/* Each argument. */
	for (index = 1; index < argc; index++) {
		/* The compositor's display. */
		value = main_value(argv[index], "--display=");
		if (value != NULL) {
			options->display = value;
			continue;
		}

		/* The font of the viewer's own words. */
		value = main_value(argv[index], "--font=");
		if (value != NULL) {
			options->font = value;
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

		/* Fullscreen from the start. */
		match = strcmp(argv[index], "--fullscreen");
		if (match == 0) {
			options->fullscreen = 1;
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

		/* An unknown option refuses the command line. */
		if (argv[index][0] == '-')
			return -1;

		/* A second file refuses the command line. */
		if (options->file != NULL)
			return -1;

		/* The file to open. */
		options->file = argv[index];
	}

	/* A window has some size. */
	if (options->width < 320U || options->height < 240U)
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
	struct kl_app_event app_event;
	struct iv_state state;
	uint64_t started;
	uint64_t now;
	int prefetched;
	int taken;
	int status;
	int timeout;
	int due;

	/* The first frame's canvas. */
	status = main_canvas_make();
	if (status != 0) {
		fprintf(stderr, "IMAGEVIEW FAILED operation=canvas\n");
		return -1;
	}

	/* Open With's slots for no image yet, the image given on the command line, and the first frame. */
	main_openers();
	main_opened();
	status = main_frame();
	if (status != 0)
		return -1;

	/* Logs that the first frame is shown, which the tests wait for. */
	iv_log("READY width=%u height=%u glass=%d images=%lu",
	       main_present.extent.width,
	       main_present.extent.height,
	       main_app.glass,
	       (unsigned long)main_app.folder.count);

	/* Each round: input, time, and a frame when something changed. */
	started = iv_clock();
	for (;;) {
		/* Waits no longer than the viewer's next animation step, frame or fade. */
		now = iv_clock();
		timeout = MAIN_IDLE_MS;
		due = iv_app_tick(&main_app, now);
		if (due >= 0 && due < timeout)
			timeout = due;

		/* Nor than the fingers' next step (a fling, a long press). */
		due = iv_touch_tick(&main_touch, &main_app, kl_clock_us());
		if (due >= 0 && due < timeout)
			timeout = due;

		/* A frame already waiting to be drawn does not wait at all. */
		if (main_app.dirty)
			timeout = 0;

		/* With time to spare, a neighbouring image is decoded ahead, one a round. */
		if (timeout > 0) {
			prefetched = iv_app_prefetch(&main_app);
			if (prefetched)
				timeout = 0;
		}

		/* Waits (a key held repeats within, after its release if that came); a lost connection ends the run. */
		status = kl_app_dispatch(main_kl, timeout);
		if (status != 0) {
			iv_log("DONE reason=disconnected");
			return 0;
		}
		now = iv_clock();
		main_app.now = now;

		/* Every input queued, in the order it came (the menus' and the titlebar's choices and the fingers among them), and the desktop's appearance. */
		for (;;) {
			taken = kl_app_take(main_kl, &app_event);
			if (taken == 0)
				break;
			if (app_event.kind == KL_APP_THEME)
				main_appearance_changed();
			if (app_event.kind == KL_APP_WINDOW && app_event.window == main_window.kui)
				main_window_event(&app_event.input);
		}

		/* The chooser the viewer asked for (File > Open), or one it no longer waits for closed. */
		main_choose();

		/* The image to the trash, or to an application of Open With, when asked (ws128-p005). */
		main_share();

		/* An image opened: its title and the recent files; the full screen asked for or given. */
		main_opened();
		main_fullscreen();

		/* Time passes for the viewer and the fingers; the menus and the titlebar show its state. */
		(void)iv_app_tick(&main_app, now);
		(void)iv_touch_tick(&main_touch, &main_app, kl_clock_us());
		main_state(&state);
		iv_menu_refresh(&main_menu, &state);
		iv_titlebar_refresh(&main_titlebar, &state);

		/* A context menu asked for (a right press or a long press). */
		if (main_app.want_context) {
			main_app.want_context = 0;
			iv_menu_context(&main_menu, &state, main_app.context_x, main_app.context_y);
		}

		/* The close button, Quit, or Close on an empty window end the run. */
		if (main_closed != 0 || main_app.want_close != 0) {
			iv_log("DONE reason=close");
			return 0;
		}

		/* So does the timeout, when one was given. */
		if (options->timeout != 0U && now - started >= (uint64_t)options->timeout * 1000U) {
			iv_log("DONE reason=timeout");
			return 0;
		}

		/* A new size: a new swapchain and canvas, and a frame. */
		if (main_resized != 0) {
			main_resized = 0;
			status = main_resize();
			if (status != 0)
				return -1;
		}

		/* A frame when something changed. */
		if (main_app.dirty != 0) {
			status = main_frame();
			if (status != 0)
				return -1;
		}
	}
}

/* Draws and shows a frame, remaking the swapchain when it is out of date; nonzero when it cannot be shown. */
static int
main_frame(
	void)
{
	struct iv_quad quad;
	uint32_t ground;
	VkResult result;
	unsigned stale;
	int canvas_changed;
	int status;

	/* Tries until the frame is shown, remaking a stale swapchain a few times. */
	for (stale = 0; stale < MAIN_STALE_LIMIT; stale++) {
		/* The image's textures, when the image changed. */
		status = main_image();
		if (status != 0)
			return -1;

		/* The canvas, when its words or cards changed; the glass's panels follow it. */
		canvas_changed = 0;
		if (main_app.ui_dirty || main_present.cpu_image) {
			iv_draw(&main_app, &main_canvas);
			canvas_changed = 1;
		}

		/* The glass's panels follow the canvas. */
		iv_glass_update(&main_glass, &main_app);

		/* The ground under everything. */
		ground = MAIN_GROUND_OPAQUE;
		if (main_app.glass)
			ground = MAIN_GROUND_GLASS;
		if (main_app.fullscreen)
			ground = MAIN_GROUND_FULLSCREEN;

		/* The frame: the image where the view places it, the canvas over it. */
		iv_app_quad(&main_app, &quad);
		result = iv_present_frame(&main_present, main_pixels, (size_t)main_present.extent.width, canvas_changed, &quad, ground);
		if (result == VK_SUCCESS) {
			main_app.dirty = 0;
			return 0;
		}

		/* Anything but a stale swapchain is a failure. */
		if (result != VK_ERROR_OUT_OF_DATE_KHR) {
			fprintf(stderr, "IMAGEVIEW FAILED operation=%s result=%d\n", main_present.operation, (int)result);
			return -1;
		}

		/* A stale swapchain is remade at the window's size, with a canvas to match. */
		status = main_resize();
		if (status != 0)
			return -1;
	}

	/* The swapchain stayed out of date. */
	fprintf(stderr, "IMAGEVIEW FAILED operation=stale-swapchain\n");
	return -1;
}

/* Remakes the swapchain at the window's size, with a canvas to match, and lays the viewer out in it; nonzero when it cannot. */
static int
main_resize(void)
{
	uint32_t width;
	uint32_t height;
	VkResult result;
	int status;

	/* The swapchain at the size the compositor gave. */
	kl_window_size(main_window.kui, &width, &height);
	result = iv_present_resize(&main_present, width, height);
	if (result != VK_SUCCESS) {
		fprintf(stderr, "IMAGEVIEW FAILED operation=%s result=%d\n", main_present.operation, (int)result);
		return -1;
	}

	/* A canvas of the swapchain's size. */
	status = main_canvas_make();
	if (status != 0)
		return -1;

	/* The viewer lays itself out at the swapchain's size. */
	iv_app_resize(&main_app, (int)main_present.extent.width, (int)main_present.extent.height);

	/* Succeeded: frames are drawn at the new size. */
	return 0;
}

/*
 * Takes one input of the window: the pointer, the wheel, the keys and the
 * menus' and the titlebar's actions become the viewer's inputs, the
 * fingers go to the touch screen, and a new size or a close request is
 * noted for the loop.
 */
static void
main_window_event(
	const struct kl_window_event *event)
{
	struct iv_event input;

	/* What it is. */
	switch (event->kind) {
	case KL_WINDOW_MOTION:
		main_event(IV_EVENT_MOTION, event, &input);
		iv_app_event(&main_app, &input);
		break;
	case KL_WINDOW_LEAVE:
		main_event(IV_EVENT_LEAVE, event, &input);
		iv_app_event(&main_app, &input);
		break;
	case KL_WINDOW_BUTTON:
		/* The button and whether it went down. */
		main_event(IV_EVENT_BUTTON, event, &input);
		input.button = event->code;
		input.pressed = event->pressed;
		iv_app_event(&main_app, &input);
		break;
	case KL_WINDOW_AXIS:
		/* Only the vertical wheel zooms or moves the image. */
		if (event->dy == 0.0)
			break;
		main_event(IV_EVENT_AXIS, event, &input);
		input.scroll = (int)event->dy;
		iv_app_event(&main_app, &input);
		break;
	case KL_WINDOW_KEY:
		/* The key, whether it went down, and whether it is a held key's repeat. */
		main_event(IV_EVENT_KEY, event, &input);
		input.key = event->code;
		input.pressed = event->pressed;
		input.repeat = event->repeated;
		iv_app_event(&main_app, &input);
		break;
	case KL_WINDOW_ACTION:
		/* An item of the menus or a control of the titlebar, in its place among the keys. */
		iv_log("ACTION id=%d action=%u", (int)event->id, (unsigned)event->code);
		main_event(IV_EVENT_ACTION, event, &input);
		input.action = event->code;
		iv_app_event(&main_app, &input);
		break;
	case KL_WINDOW_TOUCH_DOWN:
	case KL_WINDOW_TOUCH_MOTION:
	case KL_WINDOW_TOUCH_UP:
	case KL_WINDOW_TOUCH_CANCEL:
		iv_touch_event(&main_touch, &main_app, event);
		break;
	case KL_WINDOW_RESIZE:
		main_resized = 1;
		break;
	case KL_WINDOW_CLOSE:
		main_closed = 1;
		break;
	default:
		break;
	}
}

/* Fills a viewer's input of a kind with what every input carries: the pointer's place, the modifiers held and the time it was read. */
static void
main_event(
	enum iv_event_type type,
	const struct kl_window_event *event,
	struct iv_event *input)
{
	/* The modifiers are the same bits as the viewer's; the time is in milliseconds of the same clock. */
	memset(input, 0, sizeof(*input));
	input->type = type;
	input->x = (int)event->x;
	input->y = (int)event->y;
	input->modifiers = event->modifiers;
	input->time = event->arrival_us / 1000U;
}

/*
 * Gives the presenter the image shown (when it changed) and the frame of
 * an animated one (when it moved on); nonzero when the textures could not
 * be made.
 */
static int
main_image(
	void)
{
	const struct iv_image *image;
	VkResult result;

	/* The image shown, if any. */
	image = NULL;
	if (main_app.has_image)
		image = main_app.current;

	/* The image's levels, once for each image shown. */
	result = iv_present_set_image(&main_present, image, main_app.image_serial);
	if (result != VK_SUCCESS) {
		fprintf(stderr, "IMAGEVIEW FAILED operation=%s result=%d\n", main_present.operation, (int)result);
		return -1;
	}

	/* CPU sampling keeps original pixels while uploading only the window-sized canvas. */
	main_app.cpu_image = main_present.cpu_image;

	/* An animated image's frame, when it moved on. */
	if (image != NULL &&
	    image->frame_count > 1U &&
	    main_frame_serial != main_app.frame_serial) {
		iv_present_set_frame(&main_present, image->frames[main_app.frame]);
		main_frame_serial = main_app.frame_serial;
	}

	/* Succeeded: the presenter has the image. */
	return 0;
}

/* Makes the canvas's memory at the swapchain's size; nonzero when memory runs out. */
static int
main_canvas_make(void)
{
	uint32_t *pixels;
	size_t count;

	/* The canvas's memory. */
	count = (size_t)main_present.extent.width * (size_t)main_present.extent.height;
	pixels = malloc(count * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* The new memory replaces the old. */
	free(main_pixels);
	main_pixels = pixels;

	/* The canvas over it, to be drawn anew. */
	main_canvas.pixels = main_pixels;
	main_canvas.stride = main_present.extent.width;
	main_canvas.width = (int)main_present.extent.width;
	main_canvas.height = (int)main_present.extent.height;
	main_app.dirty = 1;
	main_app.ui_dirty = 1;

	/* Succeeded: frames can be drawn. */
	return 0;
}

/*
 * Shows libkeiland's file chooser when the viewer asks for it, at the folder
 * it names, and closes one it no longer waits for.
 */
static void
main_choose(void)
{
	struct kl_file_chooser_options options;
	static const struct kl_file_chooser_listener listener = {
		main_chosen
	};

	/* A chooser the viewer no longer waits for goes. */
	if (!main_app.chooser_open) {
		if (main_chooser != NULL) {
			kl_file_chooser_destroy(main_chooser);
			main_chooser = NULL;
		}

		/* Nothing to show. */
		return;
	}

	/* One already shown answers in its time. */
	if (main_chooser != NULL)
		return;

	/* Open, at the viewer's folder, with the images shown first, in the viewer's font. */
	memset(&options, 0, sizeof(options));
	options.mode = KL_FILE_CHOOSER_OPEN;
	options.application = MAIN_APPLICATION;
	options.folder = main_app.chooser_folder;
	options.filters = main_filters;
	options.filter_count = sizeof(main_filters) / sizeof(main_filters[0]);
	options.filter = 0;
	options.font = main_font;

	/* The chooser's window over the viewer's; without it the viewer stops waiting. */
	main_chooser = kl_file_chooser_open(kl_app_display(main_kl), kl_window_toplevel(main_window.kui), &options, &listener, &main_app);
	if (main_chooser == NULL) {
		iv_log("CHOOSER failed errno=%d", errno);
		iv_app_message(&main_app, "The file chooser could not be shown.", 4000U);
		iv_app_chosen(&main_app, NULL);
	}
}

/* The chooser answered: the path (empty when cancelled) goes to the viewer, and the chooser goes. */
static void
main_chosen(
	void *data,
	struct kl_file_chooser *chooser,
	unsigned result,
	const char *path,
	size_t filter)
{
	/* The answer, cancelled unless a file was chosen. */
	(void)filter;
	if (result != KL_FILE_CHOOSER_CHOSEN)
		path = NULL;
	iv_app_chosen(data, path);

	/* The chooser is spent. */
	kl_file_chooser_destroy(chooser);
	if (chooser == main_chooser)
		main_chooser = NULL;
}

/* Gathers what the menus and the titlebar show. */
static void
main_state(
	struct iv_state *state)
{
	/* A clean state, so that states compare by their bytes. */
	memset(state, 0, sizeof(*state));

	/* What the viewer shows, where it is in the folder, and how. */
	state->has_image = main_app.has_image;
	state->index = main_app.folder.index;
	state->count = main_app.folder.count;
	state->fit = main_app.fit;
	state->playing = main_app.playing;
	state->fullscreen = main_app.fullscreen;
	state->slideshow = main_app.slideshow;

	/* Only an image that was decoded can be zoomed and turned. */
	if (main_app.has_image &&
	    main_app.current != NULL &&
	    main_app.current->error == 0)
		state->can_show = 1;

	/* Only an animated image plays. */
	if (main_app.has_image &&
	    main_app.current != NULL &&
	    main_app.current->frame_count > 1U)
		state->animated = 1;
}

/* After an image opened: the window's title names it, and it joins the recent files. */
static void
main_opened(void)
{
	char resolved[PATH_MAX];
	char title[IV_PATH_MAX + 32];
	const char *name;
	char *absolute;
	int error;

	/* Only once for each image shown. */
	if (!main_app.opened)
		return;

	/* The opening is being answered now; Open With follows the image (ws128-p005). */
	main_app.opened = 0;
	main_openers();

	/* Without an image, the application's name. */
	if (!main_app.has_image || main_app.current == NULL) {
		kl_window_set_title(main_window.kui, "Image Viewer");
		return;
	}

	/* The file's name: the part of the path after its last slash. */
	name = strrchr(main_app.current->path, '/');
	if (name == NULL)
		name = main_app.current->path;
	else
		name++;

	/* The title: the file's name and the application's. */
	snprintf(title, sizeof(title), "%s \xe2\x80\x94 Image Viewer", name);
	kl_window_set_title(main_window.kui, title);

	/* The recent files, by the absolute path. */
	absolute = realpath(main_app.current->path, resolved);
	if (absolute == NULL)
		return;

	/* The image joins the recent files; a failure only goes to the log. */
	error = kl_recent_add(resolved, MAIN_APPLICATION);
	if (error != 0)
		iv_log("RECENT failed errno=%d", error);
}

/*
 * Carries out what the viewer asked of the files (ws128-p005): the image
 * shown into the trash (the viewer then goes on to the next image), or
 * the image opened in an application of Open With.
 */
static void
main_share(void)
{
	char path[IV_PATH_MAX];
	char trashed[2 * IV_PATH_MAX + 32];
	int index;
	int error;

	/* The image to the trash. */
	if (main_app.want_trash) {
		main_app.want_trash = 0;
		if (main_app.has_image && main_app.current != NULL) {
			/* The path is kept: the image goes once it is in the trash. */
			snprintf(path, sizeof(path), "%s", main_app.current->path);
			error = iv_share_trash(path, trashed, sizeof(trashed));
			if (error != 0)
				snprintf(trashed, sizeof(trashed), "-");
			iv_log("TRASH path=%s trashed=%s errno=%d", path, trashed, error);

			/* Gone: the next image; not gone: why. */
			if (error == 0) {
				iv_app_removed(&main_app);
				iv_app_message(&main_app, "Moved to the Trash", 1500U);
			} else {
				iv_app_message(&main_app, "The image could not be moved to the Trash.", 3000U);
			}
		}
	}

	/* The image to an application of Open With. */
	if (main_app.want_open_with >= 0) {
		index = main_app.want_open_with;
		main_app.want_open_with = -1;
		if (main_app.has_image && main_app.current != NULL) {
			error = iv_share_open_with(main_app.current->path, main_app.current->format, index);
			iv_log("OPEN-WITH index=%d path=%s errno=%d", index, main_app.current->path, error);
			if (error != 0)
				iv_app_message(&main_app, "The application could not be started.", 3000U);
		}
	}
}

/* Gives File > Open With the applications for the image shown (none without one). */
static void
main_openers(void)
{
	char names[IV_OPENERS][IV_OPENER_NAME];
	int count;

	/* The applications Files offers for the image's type. */
	count = 0;
	if (main_app.has_image && main_app.current != NULL)
		count = iv_share_openers(main_app.current->path, main_app.current->format, names, IV_OPENERS);

	/* The menu shows them. */
	iv_menu_openers(&main_menu, names, count);
}

/*
 * Asks the compositor for the full screen (or out of it) when the viewer
 * wants it, and lays the viewer out as the compositor made the window.
 */
static void
main_fullscreen(void)
{
	int fullscreen;
	int wanted;

	/* A request: the opposite of how the window is now. */
	if (main_app.want_fullscreen) {
		/* The full screen for a window that is not in it, and out of it for one that is. */
		wanted = 1;
		fullscreen = kl_window_fullscreen(main_window.kui);
		if (fullscreen)
			wanted = 0;

		/* The request goes to the compositor, which answers with a configure. */
		main_app.want_fullscreen = 0;
		kl_window_set_fullscreen(main_window.kui, wanted);
		iv_log("FULLSCREEN request=%d", wanted);
	}

	/* The compositor's answer: the layout (and the glass) follow it. */
	fullscreen = kl_window_fullscreen(main_window.kui);
	if (main_app.fullscreen != fullscreen) {
		main_app.fullscreen = fullscreen;
		iv_app_layout(&main_app);
		iv_log("FULLSCREEN state=%d", main_app.fullscreen);
	}
}

/* Takes the desktop's new appearance: the viewer is drawn again in its colours. */
static void
main_appearance_changed(void)
{
	unsigned appearance;

	/* A new frame, with the canvas's cards and words drawn again in the new colours. */
	appearance = kl_appearance_get(NULL);
	main_app.dirty = 1;
	main_app.ui_dirty = 1;
	iv_log("APPEARANCE appearance=%u", appearance);
}
