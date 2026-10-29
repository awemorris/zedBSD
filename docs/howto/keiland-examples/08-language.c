/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A window in the desktop's language whose catalogs are read on a thread
 * of their own (a reader), so that the window never waits for the files.
 * Each change of the desktop's language comes as a KL_APP_LANGUAGE event
 * and asks the reader for that language; only the reading of the latest
 * change is used, so that a reading that comes late never puts back a
 * language the user left.  Switch Language asks the desktop for the other
 * language: the change comes back as an event like any other.
 */

#include <keiland/keiland.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

/* Where the desktop's fonts are (/opt/keiland/share/fonts for a Linux build under /opt/keiland). */
#ifndef FONT_DIR
#define FONT_DIR	"/usr/share/fonts"
#endif
#define FONT		FONT_DIR "/keiland.ttf"
#define FALLBACK_FONT	FONT_DIR "/keiland-fallback.ttf"

/* The translations' domain: the catalogs are language.tr. */
#define DOMAIN		"language"

/* The widget's id, and the code of the reader's post. */
#define ID_SWITCH	1U
#define POST_READ	1U

/* The button's width and the room around it, in pixels. */
#define BUTTON_WIDTH	180
#define MARGIN		24

/*
 * What the application's thread and the reader share, under the lock: the
 * number of the latest change asked for (wanted) and its language, the
 * number the reader took last, and whether the reader is to end.  The
 * reader sleeps on changed while there is nothing new.
 */
struct reader {
	mtx_t lock;
	cnd_t changed;
	struct kl_app *app;
	unsigned long wanted;
	unsigned long taken;
	char language[KL_TR_NAME_MAX + 1U];
	int quit;
};

/* One reading: the change's number, its language, and the catalogs read (NULL after a failure); freed by the one who takes the post. */
struct reading {
	unsigned long generation;
	char language[KL_TR_NAME_MAX + 1U];
	struct kl_tr_catalogs *catalogs;
	int error;
};

/*
 * Everything the program keeps: its connection, window and widgets, the
 * settings it asks for a change through, the reader and its thread, the
 * number of the latest change (generation), the language shown, how many
 * readings were used and set aside, the language asked for last, and what
 * the main loop has to do next.
 */
struct language {
	struct kl_app *app;
	struct kl_window *window;
	struct kl_ui *ui;
	struct kl_text text;
	struct kl_style style;
	struct kl_canvas canvas;
	struct kl_settings *settings;
	struct reader reader;
	thrd_t thread;
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	unsigned long generation;
	char shown[KL_TR_NAME_MAX + 1U];
	unsigned long used;
	unsigned long discarded;
	int asked;
	int dirty;
	int moving;
	int resized;
	int closed;
};

static int language_resize(struct language *language);
static void language_draw(struct language *language, uint64_t now_us);
static int language_frame(struct language *language);
static void language_input(struct language *language, const struct kl_window_event *input);
static void language_changed(struct language *language, const char *name);
static void language_read(struct language *language, struct reading *reading);
static void language_switch(struct language *language);
static int language_reader(void *argument);

int
main(void)
{
	static struct language language;
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	struct kl_app_event event;
	struct reading *reading;
	uint32_t request;
	int timeout;
	int status;
	int japanese;
	int taken;
	int error;

	/* Connects to the desktop. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "org.example.Language";
	language.app = kl_app_open(&app_options);
	if (language.app == NULL) {
		fprintf(stderr, "language: no desktop (%s)\n", strerror(errno));
		return 1;
	}

	/* Reads the catalogs of the desktop's language now, on this thread: no window waits yet. */
	snprintf(language.shown, sizeof(language.shown), "%s", kl_app_language(language.app));
	status = kl_tr_open(DOMAIN, language.shown);
	if (status != 0)
		fprintf(stderr, "language: the texts stay in English (%s)\n", strerror(status));
	japanese = strcmp(language.shown, "ja");
	if (japanese == 0)
		language.asked = 1;

	/* The settings, only to ask the desktop for a change: the program never reads the language back from them. */
	language.settings = kl_settings_open(kl_app_display(language.app), NULL);
	if (language.settings == NULL)
		return 1;

	/* The reader: its shared record, then its thread, which sleeps until a change comes. */
	language.reader.app = language.app;
	status = mtx_init(&language.reader.lock, mtx_plain);
	if (status != thrd_success)
		return 1;
	status = cnd_init(&language.reader.changed);
	if (status != thrd_success)
		return 1;
	status = thrd_create(&language.thread, language_reader, &language.reader);
	if (status != thrd_success)
		return 1;

	/* Opens the window, titled in the language read. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = kl_tr("Language");
	window_options.width = 440;
	window_options.height = 220;
	window_options.present = KL_PRESENT_VULKAN;
	language.window = kl_app_window_create(language.app, &window_options);
	if (language.window == NULL) {
		fprintf(stderr, "language: no window (%s)\n", strerror(errno));
		return 1;
	}

	/* Makes the widgets' record and opens the font. */
	language.ui = kl_ui_create();
	if (language.ui == NULL)
		return 1;
	status = kl_text_open(&language.text, FONT, FALLBACK_FONT);
	if (status != 0) {
		fprintf(stderr, "language: no font %s (%s)\n", FONT, strerror(status));
		return 1;
	}

	/* Gives the widgets the font. */
	language.style.text = &language.text;

	/* Makes the first frame. */
	status = language_resize(&language);
	if (status != 0)
		return 1;

	/* Runs until the window is closed. */
	while (!language.closed) {
		/* Waits for the desktop or the reader: not at all while a frame is due, a frame's time while something moves. */
		timeout = -1;
		if (language.moving)
			timeout = 16;
		if (language.dirty || language.resized)
			timeout = 0;
		status = kl_app_dispatch(language.app, timeout);
		if (status != 0)
			break;

		/* Takes the answers to the program's requests for a change; a refusal is told, the rest come back as events. */
		(void)kl_settings_dispatch(language.settings);
		for (;;) {
			taken = kl_settings_take_result(language.settings, &request, &error);
			if (!taken)
				break;
			if (error != 0)
				fprintf(stderr, "language: the desktop refused the change (%s)\n", strerror(error));
		}

		/* Takes every event queued, in order. */
		for (;;) {
			taken = kl_app_take(language.app, &event);
			if (!taken)
				break;

			/* The appearance changed: the theme's colours are new. */
			if (event.kind == KL_APP_THEME)
				language.dirty = 1;

			/* The desktop's language changed: the language it changed to is in the event. */
			if (event.kind == KL_APP_LANGUAGE)
				language_changed(&language, event.language);

			/* A reading came back. */
			if (event.kind == KL_APP_POST && event.code == POST_READ)
				language_read(&language, event.data);

			/* An input of the window. */
			if (event.kind == KL_APP_WINDOW && event.window == language.window)
				language_input(&language, &event.input);
		}

		/* A new size: a new frame of that size. */
		if (language.resized) {
			language.resized = 0;
			status = language_resize(&language);
			if (status != 0)
				break;
		}

		/* Something changed or moves: draws and shows a frame. */
		if (language.dirty || language.moving) {
			status = language_frame(&language);
			if (status != 0)
				break;
		}
	}

	/* Ends the reader before the application closes: no thread may post after it. */
	mtx_lock(&language.reader.lock);
	language.reader.quit = 1;
	cnd_signal(&language.reader.changed);
	mtx_unlock(&language.reader.lock);
	(void)thrd_join(language.thread, NULL);

	/* Frees the readings the reader left: their catalogs are set aside unread. */
	for (;;) {
		status = kl_app_dispatch(language.app, 0);
		if (status != 0)
			break;
		taken = kl_app_take(language.app, &event);
		if (!taken)
			break;
		if (event.kind == KL_APP_POST && event.code == POST_READ) {
			reading = event.data;
			kl_tr_discard(reading->catalogs);
			free(reading);
		}
	}

	/* Frees the reader's record, the settings, the widgets' record, the frame, the font and the texts, and closes the connection. */
	cnd_destroy(&language.reader.changed);
	mtx_destroy(&language.reader.lock);
	kl_settings_close(language.settings);
	kl_ui_destroy(language.ui);
	kl_canvas_release(&language.canvas);
	free(language.pixels);
	kl_text_close(&language.text);
	kl_tr_close();
	kl_app_close(language.app);

	/* Succeeded: the window was closed. */
	return 0;
}

/*
 * Makes the frame at the window's size; nonzero when it cannot.
 */
static int
language_resize(
	struct language *language)
{
	uint32_t *pixels;
	int status;

	/* Sets the presenter to the window's size, which it reports. */
	status = kl_window_present_resize(language->window, &language->width, &language->height);
	if (status != 0)
		return -1;

	/* Allocates the frame. */
	pixels = malloc((size_t)language->width * language->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* Puts the canvas on the new pixels in place of the old ones; the widgets draw on it. */
	if (language->pixels != NULL)
		kl_canvas_release(&language->canvas);
	free(language->pixels);
	language->pixels = pixels;
	status = kl_canvas_init(&language->canvas, language->pixels, language->width, (int)language->width, (int)language->height);
	if (status != 0)
		return -1;
	language->style.canvas = &language->canvas;

	/* Succeeded: the new frame is drawn next. */
	language->dirty = 1;
	return 0;
}

/*
 * Draws one frame: a greeting, the language shown and what became of the
 * readings, and Switch Language.  Every text goes through kl_tr, so the
 * frame after a reading was used is all in the new language.
 */
static void
language_draw(
	struct language *language,
	uint64_t now_us)
{
	const struct kl_theme *theme;
	struct kl_rect whole;
	struct kl_rect button;
	char used[32];
	char discarded[32];
	char line[256];
	int baseline;
	int clicked;

	/* Starts the frame. */
	theme = kl_theme_default();
	language->style.theme = theme;
	kl_ui_begin(language->ui, now_us);

	/* Paints the ground. */
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)language->width;
	whole.height = (int)language->height;
	kl_canvas_gradient(&language->canvas, &whole, theme->ground_top, theme->ground_bottom);

	/* The greeting, and the language it is in. */
	baseline = kl_text_center(theme->text_title, MARGIN, 40);
	(void)kl_text_draw(&language->text, &language->canvas, MARGIN, baseline, kl_tr("Hello"), strlen(kl_tr("Hello")), theme->text_title, 1, theme->text);
	(void)kl_tr_format(line, sizeof(line), kl_tr("This window speaks the desktop's language: {1}."), language->shown, (const char *)NULL);
	baseline = kl_text_center(theme->text_body, MARGIN + 48, 24);
	(void)kl_text_draw(&language->text, &language->canvas, MARGIN, baseline, line, strlen(line), theme->text_body, 0, theme->text);

	/* What became of the readings: the latest change's was used, the older ones that came late were set aside. */
	snprintf(used, sizeof(used), "%lu", language->used);
	snprintf(discarded, sizeof(discarded), "%lu", language->discarded);
	(void)kl_tr_format(line, sizeof(line), kl_tr("Catalogs used: {1}. Set aside: {2}."), used, discarded, (const char *)NULL);
	baseline = kl_text_center(theme->text_body, MARGIN + 76, 24);
	(void)kl_text_draw(&language->text, &language->canvas, MARGIN, baseline, line, strlen(line), theme->text_body, 0, theme->text_secondary);

	/* Switch Language: asks the desktop for the other language. */
	button.x = MARGIN;
	button.y = (int)language->height - MARGIN - theme->control_height;
	button.width = BUTTON_WIDTH;
	button.height = theme->control_height;
	clicked = kl_button(language->ui, &language->style, ID_SWITCH, &button, kl_tr("Switch Language"), KL_BUTTON_PRIMARY);
	if (clicked)
		language_switch(language);

	/* Ends the frame. */
	language->moving = kl_ui_end(language->ui, now_us);
	kl_ui_window_text(language->ui, language->window);
}

/*
 * Draws and shows a frame; nonzero when the frame cannot be shown.
 */
static int
language_frame(
	struct language *language)
{
	int status;

	/* Draws the frame. */
	language->dirty = 0;
	language_draw(language, kl_clock_us());

	/* Hands it to the compositor. */
	status = kl_window_present(language->window, language->pixels, language->width);
	if (status == EAGAIN) {
		/* The window's size changed under the frame: a new frame of the new size. */
		language->resized = 1;
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
language_input(
	struct language *language,
	const struct kl_window_event *input)
{
	int taken;

	/* The window's own inputs: a new size, the close button. */
	if (input->kind == KL_WINDOW_RESIZE) {
		language->resized = 1;
		return;
	}

	/* The close button ends the program. */
	if (input->kind == KL_WINDOW_CLOSE) {
		language->closed = 1;
		return;
	}

	/* The rest is the widgets'. */
	taken = kl_ui_window_input(language->ui, input);
	if (taken)
		language->dirty = 1;
}

/*
 * Takes a change of the desktop's language: gives it the next number and
 * asks the reader for its catalogs.  The texts stay as they are until the
 * reading comes back.
 */
static void
language_changed(
	struct language *language,
	const char *name)
{
	/* The change's number: the reading that carries it back is the one to use. */
	language->generation++;

	/* Asks the reader for it, waking it if it sleeps; a change it has not taken yet is replaced. */
	mtx_lock(&language->reader.lock);
	language->reader.wanted = language->generation;
	snprintf(language->reader.language, sizeof(language->reader.language), "%s", name);
	cnd_signal(&language->reader.changed);
	mtx_unlock(&language->reader.lock);
}

/*
 * Takes a reading that came back: uses it when it carries the latest
 * change's number, sets it aside otherwise.  It is freed either way.
 */
static void
language_read(
	struct language *language,
	struct reading *reading)
{
	/* A reading of a change the desktop has since left: its catalogs go unused. */
	if (reading->generation != language->generation) {
		kl_tr_discard(reading->catalogs);
		free(reading);
		language->discarded++;
		language->dirty = 1;
		return;
	}

	/* The latest change's: its catalogs are the texts from now on (English when they could not be read). */
	if (reading->error != 0) {
		fprintf(stderr, "language: no catalogs for %s (%s)\n", reading->language, strerror(reading->error));
		kl_tr_close();
	} else {
		kl_tr_use(reading->catalogs);
	}

	/* Shows the language the catalogs were read for. */
	snprintf(language->shown, sizeof(language->shown), "%s", reading->language);
	free(reading);
	language->used++;

	/* The title and every text of the next frame in the new language. */
	kl_window_set_title(language->window, kl_tr("Language"));
	language->dirty = 1;
}

/*
 * Asks the desktop for the other language.  The program does not change
 * its own texts here: the change comes back as KL_APP_LANGUAGE, as one
 * made in Settings does.
 */
static void
language_switch(
	struct language *language)
{
	int status;

	/* The other of English (0) and Japanese (1), the values of the desktop's ui.language. */
	language->asked = !language->asked;
	status = kl_settings_set_int(language->settings, "ui.language", language->asked, NULL);
	if (status != 0)
		fprintf(stderr, "language: cannot ask for a change (%s)\n", strerror(status));
}

/*
 * The reader: sleeps until a change it has not taken comes, reads that
 * language's catalogs, and posts the reading to the application.  It calls
 * nothing of libkeiland but kl_tr_read, which any thread may, and the post.
 */
static int
language_reader(
	void *argument)
{
	struct reader *reader;
	struct reading *reading;
	int status;

	/* Serves the changes one at a time until it is told to end. */
	reader = argument;
	for (;;) {
		/* The reading of the latest change not taken yet, or the end. */
		reading = calloc(1, sizeof(*reading));
		if (reading == NULL)
			return 0;
		mtx_lock(&reader->lock);
		while (!reader->quit && reader->wanted == reader->taken)
			cnd_wait(&reader->changed, &reader->lock);
		if (reader->quit) {
			mtx_unlock(&reader->lock);
			free(reading);
			return 0;
		}

		/* Notes which change this reading answers. */
		reader->taken = reader->wanted;
		reading->generation = reader->wanted;
		snprintf(reading->language, sizeof(reading->language), "%s", reader->language);
		mtx_unlock(&reader->lock);

		/* Reads the catalogs, away from the windows' thread. */
		reading->error = kl_tr_read(DOMAIN, reading->language, &reading->catalogs);

		/* Hands the reading to the application's thread; without memory for the post it is lost. */
		status = kl_app_post(reader->app, POST_READ, reading);
		if (status != 0) {
			kl_tr_discard(reading->catalogs);
			free(reading);
		}
	}
}
