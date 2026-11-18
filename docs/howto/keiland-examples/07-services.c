/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A window that uses the desktop's services: it stands on the desktop's
 * glass when the compositor offers it, follows the light or dark
 * appearance, speaks the language chosen in Settings, opens a file
 * through the desktop's file chooser and puts it on the recent list,
 * copies the path to the clipboard, and posts a notification.
 */

#include <keiland/keiland.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where the desktop's fonts are (/opt/keiland/share/fonts for a Linux build under /opt/keiland). */
#ifndef FONT_DIR
#define FONT_DIR	"/usr/share/fonts"
#endif
#define FONT		FONT_DIR "/keiland.ttf"
#define FALLBACK_FONT	FONT_DIR "/keiland-fallback.ttf"

/* The application's ID, and its translations' domain (its catalogs are services.tr). */
#define APPLICATION	"org.example.Services"
#define DOMAIN		"services"

/* The widgets' ids. */
#define ID_OPEN		1U
#define ID_COPY		2U
#define ID_NOTIFY	3U

/* The layout, in pixels: the card's margin, the room inside it, and its corners (the glass panel's too). */
#define MARGIN		16
#define PADDING		24
#define CARD_RADIUS	16

/* The longest path kept. */
#define PATH_MAX_BYTES	1024

/*
 * Everything the program keeps: its connection, window and widgets, the
 * file chooser while it is open, the path chosen, how many files were
 * opened, and what the main loop has to do next.
 */
struct services {
	struct kl_app *app;
	struct kl_window *window;
	struct kl_ui *ui;
	struct kl_text text;
	struct kl_style style;
	struct kl_canvas canvas;
	struct kl_file_chooser *chooser;
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	char path[PATH_MAX_BYTES];
	unsigned long opened;
	int answered;
	int dirty;
	int moving;
	int resized;
	int closed;
};

static int services_resize(struct services *services);
static void services_draw(struct services *services, uint64_t now_us, struct kl_rect *card);
static int services_frame(struct services *services);
static void services_input(struct services *services, const struct kl_window_event *input);
static void services_open(struct services *services);
static void services_chosen(void *data, struct kl_file_chooser *chooser, unsigned result, const char *path, size_t filter);
static void services_language(struct services *services, const char *language);

/* What the file chooser calls when it has its answer. */
static const struct kl_file_chooser_listener services_chooser_listener = {
	services_chosen
};

int
main(void)
{
	static struct services services;
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	struct kl_app_event event;
	int see_through;
	int timeout;
	int status;
	int taken;

	/* Connects to the desktop. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = APPLICATION;
	services.app = kl_app_open(&app_options);
	if (services.app == NULL) {
		fprintf(stderr, "services: no desktop (%s)\n", strerror(errno));
		return 1;
	}

	/* Speaks the desktop's language: reads the catalogs of the language the application heard when it opened. */
	status = kl_tr_open(DOMAIN, kl_app_language(services.app));
	if (status != 0)
		fprintf(stderr, "services: the texts stay in English (%s)\n", strerror(status));

	/* Opens the window; the Vulkan presenter is the one that can stand on glass. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = kl_tr("Services");
	window_options.width = 480;
	window_options.height = 300;
	window_options.present = KL_PRESENT_VULKAN;
	services.window = kl_app_window_create(services.app, &window_options);
	if (services.window == NULL) {
		fprintf(stderr, "services: no window (%s)\n", strerror(errno));
		return 1;
	}

	/* Makes the widgets' record and opens the font. */
	services.ui = kl_ui_create();
	if (services.ui == NULL)
		return 1;
	status = kl_text_open(&services.text, FONT, FALLBACK_FONT);
	if (status != 0) {
		fprintf(stderr, "services: no font %s (%s)\n", FONT, strerror(status));
		return 1;
	}

	/* Gives the widgets the font. */
	services.style.text = &services.text;

	/* Makes the first frame, and learns whether the window is see-through: then it stands on glass. */
	status = services_resize(&services);
	if (status != 0)
		return 1;
	see_through = kl_window_see_through(services.window);
	services.style.glass = see_through;

	/* Runs until the window is closed. */
	while (!services.closed) {
		/* Waits for the desktop: not at all while a frame is due, a frame's time while something moves. */
		timeout = -1;
		if (services.moving)
			timeout = 16;
		if (services.dirty || services.resized || services.answered)
			timeout = 0;
		status = kl_app_dispatch(services.app, timeout);
		if (status != 0)
			break;

		/* Takes every event queued, in order. */
		for (;;) {
			taken = kl_app_take(services.app, &event);
			if (!taken)
				break;

			/* The appearance changed: kl_theme_default's colours are new, the frame is drawn with them. */
			if (event.kind == KL_APP_THEME)
				services.dirty = 1;

			/* The desktop's language changed: the event names the language it changed to. */
			if (event.kind == KL_APP_LANGUAGE)
				services_language(&services, event.language);

			/* An input of the window; none of the chooser's window comes here. */
			if (event.kind == KL_APP_WINDOW && event.window == services.window)
				services_input(&services, &event.input);
		}

		/* The chooser answered during the dispatch: it is closed now. */
		if (services.answered) {
			services.answered = 0;
			kl_file_chooser_destroy(services.chooser);
			services.chooser = NULL;
			services.dirty = 1;
		}

		/* A new size: a new frame of that size. */
		if (services.resized) {
			services.resized = 0;
			status = services_resize(&services);
			if (status != 0)
				break;
		}

		/* Something changed or moves: draws and shows a frame. */
		if (services.dirty || services.moving) {
			status = services_frame(&services);
			if (status != 0)
				break;
		}
	}

	/* Closes the chooser, the widgets' record, the frame, the font, the texts and the connection. */
	if (services.chooser != NULL)
		kl_file_chooser_destroy(services.chooser);
	kl_ui_destroy(services.ui);
	kl_canvas_release(&services.canvas);
	free(services.pixels);
	kl_text_close(&services.text);
	kl_tr_close();
	kl_app_close(services.app);

	/* Succeeded: the window was closed. */
	return 0;
}

/*
 * Makes the frame at the window's size; nonzero when it cannot.
 */
static int
services_resize(
	struct services *services)
{
	uint32_t *pixels;
	int status;

	/* Sets the presenter to the window's size, which it reports. */
	status = kl_window_present_resize(services->window, &services->width, &services->height);
	if (status != 0)
		return -1;

	/* Allocates the frame. */
	pixels = malloc((size_t)services->width * services->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* Puts the canvas on the new pixels in place of the old ones; the widgets draw on it. */
	if (services->pixels != NULL)
		kl_canvas_release(&services->canvas);
	free(services->pixels);
	services->pixels = pixels;
	status = kl_canvas_init(&services->canvas, services->pixels, services->width, (int)services->width, (int)services->height);
	if (status != 0)
		return -1;
	services->style.canvas = &services->canvas;

	/* Succeeded: the new frame is drawn next. */
	services->dirty = 1;
	return 0;
}

/*
 * Draws one frame: one card (on glass, or on the theme's ground), what
 * the desktop told the program, and the three buttons.  Reports the card's
 * rectangle, which is the glass panel.
 */
static void
services_draw(
	struct services *services,
	uint64_t now_us,
	struct kl_rect *card)
{
	const struct kl_theme *theme;
	const char *appearance;
	unsigned shown;
	struct kl_rect whole;
	struct kl_rect button;
	unsigned open_flags;
	unsigned copy_flags;
	char line[PATH_MAX_BYTES + 64];
	char count[32];
	int top;
	int clicked;

	/* Starts the frame with the theme of the appearance shown now. */
	theme = kl_theme_default();
	services->style.theme = theme;
	kl_ui_begin(services->ui, now_us);

	/* The ground: cleared to nothing on glass (the compositor draws the glass), the theme's otherwise. */
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)services->width;
	whole.height = (int)services->height;
	if (services->style.glass) {
		kl_canvas_clear(&services->canvas);
	} else {
		kl_canvas_gradient(&services->canvas, &whole, theme->ground_top, theme->ground_bottom);
	}

	/* The card: its veil on glass, a white or dark panel otherwise. */
	card->x = MARGIN;
	card->y = MARGIN;
	card->width = (int)services->width - 2 * MARGIN;
	card->height = (int)services->height - 2 * MARGIN;
	kl_panel(&services->style, card, 0);

	/* What the desktop shows now: light or dark. */
	shown = kl_appearance_get(NULL);
	appearance = kl_tr("Light");
	if (shown == KL_APPEARANCE_DARK)
		appearance = kl_tr("Dark");
	(void)kl_tr_format(line, sizeof(line), kl_tr("Appearance: {1}"), appearance, (const char *)NULL);
	top = card->y + PADDING;
	(void)kl_text_draw(&services->text, &services->canvas, card->x + PADDING, top + 16, line, strlen(line), theme->text_body, 0, theme->text);

	/* The file chosen last, and how many were opened (the plural the language needs). */
	snprintf(count, sizeof(count), "%lu", services->opened);
	(void)kl_tr_format(line, sizeof(line), kl_trn("{1} file opened", "{1} files opened", services->opened), count, (const char *)NULL);
	(void)kl_text_draw(&services->text, &services->canvas, card->x + PADDING, top + 48, line, strlen(line), theme->text_body, 0, theme->text_secondary);
	if (services->path[0] != '\0')
		(void)kl_text_draw_fit(&services->text, &services->canvas, card->x + PADDING, top + 76, services->path, theme->text_small, 0, card->width - 2 * PADDING, theme->text_secondary);

	/* Open: the desktop's file chooser; disabled while it is open. */
	button.x = card->x + PADDING;
	button.y = card->y + card->height - PADDING - theme->control_height;
	button.height = theme->control_height;
	button.width = kl_button_width(&services->style, kl_tr("Open..."));
	open_flags = KL_BUTTON_PRIMARY;
	if (services->chooser != NULL)
		open_flags |= KL_BUTTON_DISABLED;
	clicked = kl_button(services->ui, &services->style, ID_OPEN, &button, kl_tr("Open..."), open_flags);
	if (clicked)
		services_open(services);

	/* Copy Path: the path as text on the clipboard; disabled before a file was chosen. */
	button.x += button.width + 12;
	button.width = kl_button_width(&services->style, kl_tr("Copy Path"));
	copy_flags = 0U;
	if (services->path[0] == '\0')
		copy_flags = KL_BUTTON_DISABLED;
	clicked = kl_button(services->ui, &services->style, ID_COPY, &button, kl_tr("Copy Path"), copy_flags);
	if (clicked)
		kl_window_copy(services->window, services->path, strlen(services->path));

	/* Notify: a notification under the application's name, saying how many files were opened. */
	button.x += button.width + 12;
	button.width = kl_button_width(&services->style, kl_tr("Notify"));
	clicked = kl_button(services->ui, &services->style, ID_NOTIFY, &button, kl_tr("Notify"), 0U);
	if (clicked)
		(void)kl_app_notify(services->app, kl_tr("Services"), line);

	/* Ends the frame. */
	services->moving = kl_ui_end(services->ui, now_us);
	kl_ui_window_text(services->ui, services->window);
}

/*
 * Draws and shows a frame, with its glass panel; nonzero when the frame
 * cannot be shown.
 */
static int
services_frame(
	struct services *services)
{
	struct kl_glass_panel panel;
	struct kl_rect card;
	int status;

	/* Draws the frame. */
	services->dirty = 0;
	services_draw(services, kl_clock_us(), &card);

	/* On glass: the card is the panel the compositor puts the glass under, from this frame on. */
	if (services->style.glass) {
		panel.x = card.x;
		panel.y = card.y;
		panel.width = card.width;
		panel.height = card.height;
		panel.radius = CARD_RADIUS;
		panel.kind = KL_GLASS_CARD;
		status = kl_window_set_glass(services->window, &panel, 1U);
		if (status != 0) {
			/* No glass after all: the next frame is opaque. */
			services->style.glass = 0;
			services->dirty = 1;
		}
	}

	/* Hands it to the compositor. */
	status = kl_window_present(services->window, services->pixels, services->width);
	if (status == EAGAIN) {
		/* The window's size changed under the frame: a new frame of the new size. */
		services->resized = 1;
	} else if (status != 0) {
		/* Refuses to go on when the frame could not be shown. */
		return -1;
	}

	/* Succeeded: the frame is shown. */
	return 0;
}

/*
 * Gives one input of the window to the widgets, or acts on it.
 */
static void
services_input(
	struct services *services,
	const struct kl_window_event *input)
{
	int taken;

	/* The window's own inputs: a new size, the close button. */
	if (input->kind == KL_WINDOW_RESIZE) {
		services->resized = 1;
		return;
	}

	/* The close button ends the program. */
	if (input->kind == KL_WINDOW_CLOSE) {
		services->closed = 1;
		return;
	}

	/* The rest is the widgets'. */
	taken = kl_ui_window_input(services->ui, input);
	if (taken)
		services->dirty = 1;
}

/*
 * Opens the desktop's file chooser over the window.  The program keeps
 * running; the answer comes once, through services_chosen, during a
 * dispatch.
 */
static void
services_open(
	struct services *services)
{
	struct kl_file_chooser_options options;

	/* One chooser at a time. */
	if (services->chooser != NULL)
		return;

	/* An Open window in the program's name, in its fonts. */
	memset(&options, 0, sizeof(options));
	options.mode = KL_FILE_CHOOSER_OPEN;
	options.title = kl_tr("Open");
	options.application = APPLICATION;
	options.font = FONT;
	options.fallback_font = FALLBACK_FONT;
	services->chooser = kl_file_chooser_open(kl_app_display(services->app), kl_window_toplevel(services->window), &options, &services_chooser_listener, services);
	if (services->chooser == NULL)
		fprintf(stderr, "services: no file chooser (%s)\n", strerror(errno));
}

/*
 * Takes the file chooser's answer: the path chosen, which goes on the
 * desktop's recent list.  The chooser is destroyed by the main loop.
 */
static void
services_chosen(
	void *data,
	struct kl_file_chooser *chooser,
	unsigned result,
	const char *path,
	size_t filter)
{
	struct services *services;


	/* The chooser and the filter are not needed here. */
	(void)chooser;
	(void)filter;

	/* Keeps the path, and tells the desktop the file was opened. */
	services = data;
	if (result == KL_FILE_CHOOSER_CHOSEN) {
		snprintf(services->path, sizeof(services->path), "%s", path);
		services->opened++;
		(void)kl_recent_add(path, APPLICATION);
	}

	/* The main loop closes the chooser. */
	services->answered = 1;
}

/*
 * Speaks the language the desktop changed to: reads its catalogs, so that
 * kl_tr gives its texts from now on, and draws the window again.
 */
static void
services_language(
	struct services *services,
	const char *language)
{
	int status;

	/* The new language's catalogs (English after a failure). */
	status = kl_tr_open(DOMAIN, language);
	if (status != 0)
		fprintf(stderr, "services: the texts are in English (%s)\n", strerror(status));

	/* The title and every text of the next frame in the new language. */
	kl_window_set_title(services->window, kl_tr("Services"));
	services->dirty = 1;
}
