/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Photos' window (ws157-p005; photos.h, app.h): a libkeiland application
 * with one window that shows the view (view.c), its menu (File: Import
 * Photo..., Import Folder..., Refresh, Quit; Photo: Favorite, Rotate Left,
 * Rotate Right, Add to Album..., Slideshow, Back to Photos), and the
 * view's input.  The photos are the library's in ~/Pictures/Library (its
 * database, db.c); an import (import.c) takes a photo, or the folder of a
 * photo, chosen in libkeiland's file chooser, or the path given with
 * --import before the window opens.  What the view changes (marks,
 * albums) is written to the database.  The pictures the view wants are
 * queued to the thread (thumbs.c, its thumbnails kept in
 * $XDG_CACHE_HOME/keiland/photos) and given to the view when made.
 * Ctrl+Q quits.  What happens is logged on standard error as "PHOTOS"
 * lines for the tests.
 *
 *   photos [--width=N] [--height=N] [--timeout-s=N] [--import=PATH]
 */

#include "app.h"
#include "../picture/png-write.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fonts, the window's first size, and the longest wait for input. */
#define PH_FONT			KEILAND_DATADIR "/fonts/keiland.ttf"
#define PH_FALLBACK_FONT	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"
#define PH_WIDTH		1040U
#define PH_HEIGHT		700U
#define PH_IDLE_MS		1000
#define PH_MOVING_MS		10
#define PH_WORKING_MS		15

/* The most glass panels of a frame. */
#define PH_PANELS_MAX		4U

/* The key Q, which quits with Ctrl. */
#define PH_KEY_Q		16U

/* The longest side of a photo handed to another program as a picture (ws189-p003). */
#define PH_DRAG_SIDE		2048

/* The application's id, which the file chooser's window takes too, and the thumbnails' folder under the cache folder. */
#define PH_APPLICATION		"photos"
#define PH_CACHE		"keiland/photos"

/*
 * The window's state: the application, the window and its input, the
 * frame (its pixels, size and canvas), the text and the style, the view,
 * the library's folder (and whether there is one), the file chooser shown
 * and what for (PH_ACTION_IMPORT or _FOLDER), whether it answered and the
 * path it chose to import (empty when cancelled), whether a frame is due, the window changed size, a
 * widget moves, and the glass was decided.
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
	char root[PH_PATH_MAX];
	int root_known;
	struct kl_file_chooser *chooser;
	unsigned chooser_for;
	int answered;
	char chosen[PH_PATH_MAX];
	int dirty;
	int resized;
	int moving;
	int glass_decided;
};

/* The window's menu. */
static const struct kl_menu_entry ph_menu[] = {
	{ 1U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "File", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 2U, 1U, KL_MENU_ITEM_NORMAL, "Import Photo...", PH_ACTION_IMPORT, KL_MENU_ROLE_NONE, KL_MENU_CTRL, 'i' },
	{ 3U, 1U, KL_MENU_ITEM_NORMAL, "Import Folder...", PH_ACTION_IMPORT_FOLDER, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 4U, 1U, KL_MENU_ITEM_NORMAL, "Refresh", PH_ACTION_REFRESH, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 5U, 1U, KL_MENU_ITEM_NORMAL, "Quit Photos", PH_ACTION_QUIT, KL_MENU_ROLE_QUIT, KL_MENU_CTRL, 'q' },
	{ 6U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Photo", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 7U, 6U, KL_MENU_ITEM_NORMAL, "Favorite", PH_ACTION_FAVORITE, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 8U, 6U, KL_MENU_ITEM_NORMAL, "Rotate Left", PH_ACTION_TURN_LEFT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 9U, 6U, KL_MENU_ITEM_NORMAL, "Rotate Right", PH_ACTION_TURN_RIGHT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 10U, 6U, KL_MENU_ITEM_NORMAL, "Add to Album...", PH_ACTION_ALBUM, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 11U, 6U, KL_MENU_ITEM_NORMAL, "Slideshow", PH_ACTION_SLIDESHOW, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 12U, 6U, KL_MENU_ITEM_NORMAL, "Back to Photos", PH_ACTION_BACK, KL_MENU_ROLE_NONE, 0U, 0U }
};

int main(int argc, char **argv);
static int ph_parse(int argc, char **argv, unsigned *width, unsigned *height, unsigned *timeout, const char **source);
static void ph_library_start(struct ph_window *photos);
static void ph_import_path(struct ph_window *photos, const char *source, int notice);
static void ph_refresh(struct ph_window *photos);
static void ph_choose(struct ph_window *photos, unsigned purpose);
static void ph_chooser_done(void *data, struct kl_file_chooser *chooser, unsigned result, const char *path, size_t filter);
static void ph_chosen(struct ph_window *photos);
static void ph_cache_folder(char *folder, size_t size);
static int ph_loop(struct ph_window *photos, unsigned timeout);
static void ph_input(struct ph_window *photos, const struct kl_window_event *event);
static void ph_drag_start(struct ph_window *photos, long photo);
static int ph_drag_picture(struct ph_window *photos, long photo, unsigned char **png, size_t *size);
static int ph_uri(const char *path, char *uri, size_t size);
static void ph_results(struct ph_window *photos);
static void ph_jobs(struct ph_window *photos);
static void ph_marks(struct ph_window *photos);
static int ph_resize(struct ph_window *photos);
static void ph_draw(struct ph_window *photos, uint64_t now_us);
static int ph_wait(const struct ph_window *photos, uint64_t now_us);

/*
 * Runs Photos.
 */
int
main(
	int argc,
	char **argv)
{
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	static struct ph_window photos;
	char cache[PH_PATH_MAX];
	const char *source;
	unsigned timeout;
	unsigned width;
	unsigned height;
	int status;
	int error;

	/* The command line. */
	status = ph_parse(argc, argv, &width, &height, &timeout, &source);
	if (status != 0) {
		fprintf(stderr, "usage: photos [--width=N] [--height=N] [--timeout-s=N] [--import=PATH]\n");
		return 2;
	}

	/* The fonts; without them the view shows no words. */
	error = kl_text_open(&photos.text, PH_FONT, PH_FALLBACK_FONT);
	if (error != 0)
		ph_log("FONT missing error=%d", error);

	/* The library, and the import asked for. */
	ph_library_start(&photos);
	if (source != NULL)
		ph_import_path(&photos, source, 0);

	/* The view's state. */
	error = ph_view_init(&photos.view);
	if (error != 0) {
		ph_log("FAILED operation=view error=%d", error);
		return 1;
	}

	/* The thread that makes the pictures, the thumbnails kept in the cache. */
	ph_cache_folder(cache, sizeof(cache));
	error = ph_worker_start(cache);
	if (error != 0) {
		ph_log("FAILED operation=thread error=%d", error);
		ph_view_release(&photos.view);
		return 1;
	}

	/* The application. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = PH_APPLICATION;
	photos.app = kl_app_open(&app_options);
	if (photos.app == NULL) {
		ph_log("FAILED operation=app error=%d", errno);
		ph_worker_stop();
		ph_view_release(&photos.view);
		return 1;
	}

	/* Its window. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Photos";
	window_options.width = width;
	window_options.height = height;
	window_options.present = KL_PRESENT_VULKAN;
	photos.window = kl_app_window_create(photos.app, &window_options);
	if (photos.window == NULL) {
		ph_log("FAILED operation=window error=%d", errno);
		kl_app_close(photos.app);
		ph_worker_stop();
		ph_view_release(&photos.view);
		return 1;
	}

	/* The input of its frames. */
	photos.ui = kl_ui_create();
	if (photos.ui == NULL) {
		ph_log("FAILED operation=ui error=%d", errno);
		kl_app_close(photos.app);
		ph_worker_stop();
		ph_view_release(&photos.view);
		return 1;
	}

	/* The menu and the style (opaque until the first frame finds whether the window can stand on glass). */
	(void)kl_window_set_menu(photos.window, ph_menu, sizeof(ph_menu) / sizeof(ph_menu[0]));
	photos.style.text = &photos.text;
	photos.style.theme = kl_theme_default();
	photos.style.glass = 0;
	photos.view.glass = 0;

	/* The loop until the window closes. */
	status = ph_loop(&photos, timeout);

	/* Everything goes. */
	kl_file_chooser_destroy(photos.chooser);
	ph_worker_stop();
	kl_ui_destroy(photos.ui);
	if (photos.canvas_made)
		kl_canvas_release(&photos.canvas);
	free(photos.pixels);
	kl_app_close(photos.app);
	ph_view_release(&photos.view);
	ph_library_release();
	kl_text_close(&photos.text);

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
	fputs("PHOTOS ", stderr);
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
	const char **source)
{
	int index;
	int same;

	/* The defaults. */
	*width = PH_WIDTH;
	*height = PH_HEIGHT;
	*timeout = 0U;
	*source = NULL;

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

		/* A file or a folder to import. */
		same = strncmp(argv[index], "--import=", 9U);
		if (same != 0 || argv[index][9] == '\0')
			return -1;
		*source = argv[index] + 9;
	}

	/* A window needs a size. */
	if (*width == 0U || *height == 0U)
		return -1;

	/* Succeeded: the command line is read. */
	return 0;
}

/* Reads the library's database (none when there is no home: nothing is kept). */
static void
ph_library_start(
	struct ph_window *photos)
{
	size_t count;
	size_t albums;
	int error;

	/* The library's folder and its database. */
	error = ph_library_root(photos->root, sizeof(photos->root));
	photos->root_known = error == 0;
	if (photos->root_known)
		error = ph_db_load(photos->root);
	(void)ph_photos(&count);
	(void)ph_albums(&albums);
	ph_log("LIBRARY root=%s photos=%lu albums=%lu error=%d", photos->root, (unsigned long)count, (unsigned long)albums, error);
}

/*
 * Imports a file or a folder into the library and writes the database;
 * with notice, the view starts again on the library and says what came
 * of it.
 */
static void
ph_import_path(
	struct ph_window *photos,
	const char *source,
	int notice)
{
	struct ph_import_result result;
	char words[160];
	int error;
	int saved;

	/* Into the library, written. */
	if (!photos->root_known)
		return;
	error = ph_import(photos->root, source, &result);
	saved = ph_db_save(photos->root);
	ph_log("IMPORT done source=%s imported=%u duplicates=%u failed=%u error=%d save=%d", source, result.imported, result.duplicates, result.failed,
	    error, saved);
	if (!notice)
		return;

	/* The view on the new order, and what came of it. */
	ph_worker_drop_thumbs();
	(void)ph_view_reset(&photos->view);
	if (error != 0)
		(void)snprintf(words, sizeof(words), "Nothing could be imported.");
	else if (result.duplicates > 0U)
		(void)snprintf(words, sizeof(words), "Imported %u photos (%u already in the library).", result.imported, result.duplicates);
	else
		(void)snprintf(words, sizeof(words), "Imported %u photos.", result.imported);
	ph_view_notice(&photos->view, words, kl_clock_us());
	photos->dirty = 1;
}

/* Reads the library again, and starts the view on it. */
static void
ph_refresh(
	struct ph_window *photos)
{
	int error;

	/* The library again. */
	ph_worker_drop_thumbs();
	ph_library_release();
	ph_library_start(photos);

	/* The view from the timeline's top. */
	error = ph_view_reset(&photos->view);
	ph_log("REFRESH error=%d", error);
	ph_view_notice(&photos->view, "The library was read again.", kl_clock_us());
	photos->dirty = 1;
}

/*
 * Shows libkeiland's file chooser for an import: a photo, or a photo in
 * the folder to import (PH_ACTION_IMPORT or _FOLDER), the pictures shown
 * first.  One shown already answers in its time.
 */
static void
ph_choose(
	struct ph_window *photos,
	unsigned purpose)
{
	static const struct kl_file_filter filters[] = {
		{ "Pictures", "jpg jpeg jpe png gif" },
		{ "All files", NULL }
	};
	static const struct kl_file_chooser_listener listener = {
		ph_chooser_done
	};
	struct kl_file_chooser_options options;
	char folder[PH_PATH_MAX];
	const char *home;

	/* One at a time. */
	if (photos->chooser != NULL)
		return;
	photos->chooser_for = purpose;

	/* From the home's Pictures folder. */
	home = getenv("HOME");
	folder[0] = '\0';
	if (home != NULL)
		(void)snprintf(folder, sizeof(folder), "%s/Pictures", home);
	memset(&options, 0, sizeof(options));
	options.mode = KL_FILE_CHOOSER_OPEN;
	options.title = "Import Photo";
	if (purpose == PH_ACTION_IMPORT_FOLDER)
		options.title = "Import the Folder of a Photo";
	options.application = PH_APPLICATION;
	options.folder = folder;
	options.filters = filters;
	options.filter_count = sizeof(filters) / sizeof(filters[0]);
	options.font = PH_FONT;
	photos->chooser = kl_file_chooser_open(kl_app_display(photos->app), kl_window_toplevel(photos->window), &options, &listener, photos);
	ph_log("CHOOSER open for=%u ok=%d", purpose, photos->chooser != NULL);
	if (photos->chooser == NULL)
		ph_view_notice(&photos->view, "The file chooser could not be shown.", kl_clock_us());
}

/* Takes the file chooser's answer: the path to import, later in the loop (not from inside the chooser's own call). */
static void
ph_chooser_done(
	void *data,
	struct kl_file_chooser *chooser,
	unsigned result,
	const char *path,
	size_t filter)
{
	struct ph_window *photos;
	char *slash;

	/* A path chosen: the photo, or its folder. */
	(void)chooser;
	(void)filter;
	photos = data;
	photos->chosen[0] = '\0';
	if (result == KL_FILE_CHOOSER_CHOSEN && path != NULL)
		(void)snprintf(photos->chosen, sizeof(photos->chosen), "%s", path);
	if (photos->chooser_for == PH_ACTION_IMPORT_FOLDER) {
		slash = strrchr(photos->chosen, '/');
		if (slash != NULL && slash != photos->chosen)
			*slash = '\0';
	}

	/* Taken by the loop; the log line the tests read. */
	photos->answered = 1;
	ph_log("CHOOSER done result=%u path=%s", result, photos->chosen);
}

/* Imports what the file chooser chose, when it has answered. */
static void
ph_chosen(
	struct ph_window *photos)
{
	char source[PH_PATH_MAX];

	/* The chooser goes once it has answered (its answer is kept). */
	if (!photos->answered)
		return;
	photos->answered = 0;
	kl_file_chooser_destroy(photos->chooser);
	photos->chooser = NULL;
	photos->dirty = 1;

	/* The import, unless it was cancelled. */
	if (photos->chosen[0] == '\0')
		return;
	(void)snprintf(source, sizeof(source), "%s", photos->chosen);
	photos->chosen[0] = '\0';
	ph_import_path(photos, source, 1);
}

/* Writes the thumbnails' cache folder: $XDG_CACHE_HOME, else ~/.cache, then keiland/photos (empty when neither is known). */
static void
ph_cache_folder(
	char *folder,
	size_t size)
{
	const char *cache;
	const char *home;

	/* $XDG_CACHE_HOME, else ~/.cache. */
	folder[0] = '\0';
	cache = getenv("XDG_CACHE_HOME");
	home = getenv("HOME");
	if (cache != NULL && cache[0] == '/')
		(void)snprintf(folder, size, "%s/%s", cache, PH_CACHE);
	else if (home != NULL && home[0] == '/')
		(void)snprintf(folder, size, "%s/.cache/%s", home, PH_CACHE);
}

/*
 * Runs the window until it closes, Quit or the timeout; nonzero when
 * something failed.
 */
static int
ph_loop(
	struct ph_window *photos,
	unsigned timeout)
{
	struct kl_app_event event;
	uint64_t started;
	uint64_t now;
	int status;
	int taken;
	int moved;
	int wait;

	/* The first frame. */
	status = ph_resize(photos);
	if (status != 0)
		return -1;
	ph_log("READY width=%u height=%u", photos->width, photos->height);

	/* Each round: the input, the pictures made, then a frame when something changed. */
	started = kl_clock_us();
	for (;;) {
		/* Waits for the compositor, or for the time something moves. */
		now = kl_clock_us();
		wait = ph_wait(photos, now);
		status = kl_app_dispatch(photos->app, wait);
		if (status != 0) {
			ph_log("DONE reason=disconnected");
			return 0;
		}

		/* The window's input and the actions of its menu. */
		for (;;) {
			taken = kl_app_take(photos->app, &event);
			if (!taken)
				break;

			/* The desktop's appearance changed: the theme's colours are new. */
			if (event.kind == KL_APP_THEME) {
				photos->dirty = 1;
				continue;
			}

			/* Another window's event is not this one's. */
			if (event.kind != KL_APP_WINDOW || event.window != photos->window)
				continue;

			/* An action of the menu, or input. */
			if (event.input.kind == KL_WINDOW_ACTION) {
				ph_view_action(&photos->view, event.input.code, kl_clock_us());
				photos->dirty = 1;
			} else {
				ph_input(photos, &event.input);
			}
		}

		/* The pictures made, the slideshow, the marks to keep, the library to read again. */
		now = kl_clock_us();
		ph_results(photos);
		moved = ph_view_tick(&photos->view, now);
		if (moved)
			photos->dirty = 1;
		ph_marks(photos);
		if (photos->view.import != 0U) {
			ph_choose(photos, photos->view.import);
			photos->view.import = 0;
		}

		/* The import the chooser answered. */
		ph_chosen(photos);
		if (photos->view.refresh) {
			photos->view.refresh = 0;
			ph_refresh(photos);
		}

		/* The end: the window closed or Quit. */
		if (photos->view.quit) {
			ph_log("DONE reason=close");
			return 0;
		}

		/* The timeout, when one was given. */
		if (timeout != 0U && now - started >= (uint64_t)timeout * 1000000U) {
			ph_log("DONE reason=timeout");
			return 0;
		}

		/* A new size. */
		if (photos->resized) {
			photos->resized = 0;
			status = ph_resize(photos);
			if (status != 0)
				return -1;
		}

		/* The view's notice gone: drawn without it. */
		if (photos->view.notice[0] != '\0' && now >= photos->view.notice_until) {
			photos->view.notice[0] = '\0';
			photos->dirty = 1;
		}

		/* A frame, and the pictures it wants. */
		ph_draw(photos, now);
	}
}

/*
 * Gives one input of the window to the view's widgets, or takes it as the
 * window's: Ctrl+Q, a new size, the close.
 */
static void
ph_input(
	struct ph_window *photos,
	const struct kl_window_event *event)
{
	long photo;
	int taken;

	/* Ctrl+Q quits. */
	if (event->kind == KL_WINDOW_KEY && event->pressed && (event->modifiers & KL_MOD_CTRL) != 0U && event->code == PH_KEY_Q) {
		photos->view.quit = 1;
		return;
	}

	/* A press on a photo held and moved far enough drags the photo out of the window (ws189-p003); a release spends it. */
	if (event->kind == KL_WINDOW_MOTION) {
		photo = ph_view_drag_check(&photos->view, event->x, event->y);
		if (photo >= 0) {
			ph_drag_start(photos, photo);
			photos->dirty = 1;
			return;
		}
	}

	/* A release spends the press. */
	if (event->kind == KL_WINDOW_BUTTON && !event->pressed)
		photos->view.drag_armed = 0;

	/* The widgets' input draws again. */
	taken = kl_ui_window_input(photos->ui, event);
	if (taken) {
		photos->dirty = 1;
		return;
	}

	/* The window's own. */
	if (event->kind == KL_WINDOW_RESIZE)
		photos->resized = 1;
	else if (event->kind == KL_WINDOW_CLOSE)
		photos->view.quit = 1;
}

/*
 * Drags a photo out of the window (ws189-p003): its file (a file:// URI)
 * and its picture (a PNG, at most PH_DRAG_SIDE on its longer side, turned
 * as shown), as a copy, with its thumbnail under the pointer.  The drag
 * starts first, while the button is held; the picture is made after and
 * filled in before the next dispatch.
 */
static void
ph_drag_start(
	struct ph_window *photos,
	long photo)
{
	struct kl_drag_data data[2];
	struct kl_drag_icon icon;
	struct ph_photo *list;
	struct ph_thumb *thumb;
	const struct kl_image *picture;
	unsigned char *png;
	char uri[PH_PATH_MAX * 3 + 16];
	size_t count;
	size_t uri_length;
	size_t png_size;
	int error;

	/* The photo, and its file as a URI with its line's end. */
	list = ph_photos(&count);
	if (photo < 0 || (size_t)photo >= count)
		return;
	error = ph_uri(list[photo].path, uri, sizeof(uri));
	if (error != 0) {
		ph_log("DND failed photo=%ld errno=%d", photo, error);
		return;
	}

	/* The line's length. */
	uri_length = strlen(uri);

	/* The two types; the picture's data comes after the drag starts. */
	data[0].type = "text/uri-list";
	data[0].data = uri;
	data[0].length = uri_length;
	data[1].type = "image/png";
	data[1].data = NULL;
	data[1].length = 0;

	/* Its thumbnail under the pointer (the photo shown whole, else the grid's thumbnail), held at its middle. */
	picture = NULL;
	if (photo == photos->view.open && photos->view.picture.pixels != NULL)
		picture = &photos->view.picture;
	thumb = NULL;
	if ((size_t)photo < photos->view.thumb_count)
		thumb = &photos->view.thumbs[photo];
	if (picture == NULL && thumb != NULL && thumb->state == PH_THUMB_READY)
		picture = &thumb->image;
	memset(&icon, 0, sizeof(icon));
	if (picture != NULL && picture->stride == (size_t)picture->width) {
		icon.pixels = picture->pixels;
		icon.width = picture->width;
		icon.height = picture->height;
		icon.hot_x = picture->width / 2;
		icon.hot_y = picture->height / 2;
	}

	/* The drag, from the press, as a copy; the press is the drag's now. */
	if (icon.pixels != NULL) {
		error = kl_window_start_drag_icon(photos->window, data, 2U, KL_DND_COPY, kl_window_press_serial(photos->window), &icon);
	} else {
		error = kl_window_start_drag(photos->window, data, 2U, KL_DND_COPY, kl_window_press_serial(photos->window));
	}

	/* The press is the drag's now, whether it started or not. */
	(void)kl_ui_pointer_cancel(photos->ui);
	if (error != 0) {
		ph_log("DND failed photo=%ld errno=%d", photo, error);
		return;
	}

	/* The log line the tests read. */
	ph_log("DND start photo=%ld path=%s", photo, list[photo].path);

	/* The picture, made now and filled in. */
	png = NULL;
	png_size = 0;
	error = ph_drag_picture(photos, photo, &png, &png_size);
	if (error == 0)
		error = kl_window_drag_fill(photos->window, "image/png", png, png_size);
	free(png);

	/* Succeeded or not, the drag goes on (a picture not made leaves the file alone). */
	ph_log("DND picture photo=%ld bytes=%lu errno=%d", photo, (unsigned long)png_size, error);
}

/*
 * Makes the PNG of a photo for a drag: the picture shown whole when it is
 * the one, else its file decoded, fitted to PH_DRAG_SIDE and turned as the
 * library says.  Returns 0 with the PNG (the caller frees it), or an errno
 * value.
 */
static int
ph_drag_picture(
	struct ph_window *photos,
	long photo,
	unsigned char **png,
	size_t *size)
{
	struct ph_photo *list;
	struct kl_image decoded;
	struct kl_image fitted;
	struct kl_image turned;
	const struct kl_image *source;
	size_t count;
	int error;

	/* The picture shown whole is ready (fitted and turned already). */
	*png = NULL;
	*size = 0;
	if (photo == photos->view.open && photos->view.picture.pixels != NULL) {
		source = &photos->view.picture;
		error = kl_picture_png(source->pixels, source->width, source->height, source->stride, png, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* Otherwise the file, decoded. */
	list = ph_photos(&count);
	memset(&decoded, 0, sizeof(decoded));
	error = ph_decode(list[photo].path, &decoded);
	if (error != 0)
		return error;

	/* Fitted. */
	memset(&fitted, 0, sizeof(fitted));
	error = ph_fit(&decoded, PH_DRAG_SIDE, &fitted);
	kl_image_release(&decoded);
	if (error != 0)
		return error;

	/* Turned as the library says. */
	memset(&turned, 0, sizeof(turned));
	error = ph_turn(&fitted, list[photo].turns, &turned);
	kl_image_release(&fitted);
	if (error != 0)
		return error;

	/* The PNG. */
	error = kl_picture_png(turned.pixels, turned.width, turned.height, turned.stride, png, size);
	kl_image_release(&turned);
	if (error != 0)
		return error;

	/* Succeeded: the PNG. */
	return 0;
}

/*
 * Writes a path as a "text/uri-list" line: file:// and the path, the
 * bytes outside the unreserved set and "/" as %XX, then CRLF.  Returns 0,
 * or ENAMETOOLONG when it does not fit.
 */
static int
ph_uri(
	const char *path,
	char *uri,
	size_t size)
{
	static const char hex[] = "0123456789ABCDEF";
	const unsigned char *byte;
	size_t used;
	int plain;

	/* The scheme. */
	if (size < 16U)
		return ENAMETOOLONG;
	memcpy(uri, "file://", 7U);
	used = 7U;

	/* Each byte, as it is or as %XX. */
	for (byte = (const unsigned char *)path; *byte != '\0'; byte++) {
		/* Room for the longest form, the line's end and the NUL. */
		if (used + 3U + 3U > size)
			return ENAMETOOLONG;

		/* Letters, digits, "-._~" and "/" stay as they are. */
		plain = 0;
		if ((*byte >= 'a' && *byte <= 'z') || (*byte >= 'A' && *byte <= 'Z') || (*byte >= '0' && *byte <= '9'))
			plain = 1;
		if (*byte == '-' || *byte == '.' || *byte == '_' || *byte == '~' || *byte == '/')
			plain = 1;
		if (plain) {
			uri[used++] = (char)*byte;
			continue;
		}

		/* Any other byte as %XX. */
		uri[used++] = '%';
		uri[used++] = hex[*byte >> 4];
		uri[used++] = hex[*byte & 0x0fU];
	}

	/* Succeeded: the line ended. */
	uri[used++] = '\r';
	uri[used++] = '\n';
	uri[used] = '\0';
	return 0;
}

/* Gives the view the pictures the thread made. */
static void
ph_results(
	struct ph_window *photos)
{
	struct ph_result result;
	int taken;

	/* Each one; a frame shows them. */
	for (;;) {
		taken = ph_worker_take(&result);
		if (!taken)
			break;
		ph_view_result(&photos->view, &result);
		photos->dirty = 1;
	}
}

/*
 * Queues the pictures the view wanted in the frame drawn: the photo shown
 * whole, or the thumbnails in sight (those queued before and no longer in
 * sight are dropped).
 */
static void
ph_jobs(
	struct ph_window *photos)
{
	struct ph_photo *list;
	struct ph_view *view;
	size_t count;
	size_t index;
	size_t photo;
	int whole;

	/* The thumbnails wanted before go; the frame's wants come in their place. */
	view = &photos->view;
	list = ph_photos(&count);
	ph_worker_drop_thumbs();
	for (index = 0; index < view->want_count; index++) {
		photo = view->wants[index];
		if (photo >= count)
			continue;
		whole = (long)photo == view->open;
		(void)ph_worker_queue(photo, whole, list[photo].turns, list[photo].path, list[photo].id, view->generation);
	}
}

/* Writes the database when the view changed the marks or the albums. */
static void
ph_marks(
	struct ph_window *photos)
{
	int error;

	/* Nothing changed, or nowhere to keep them. */
	if (!photos->view.save)
		return;
	photos->view.save = 0;
	if (!photos->root_known)
		return;

	/* Written: the months and the albums that changed. */
	error = ph_db_save(photos->root);
	ph_log("SAVE error=%d", error);
	if (error != 0)
		ph_view_notice(&photos->view, "The library could not be saved.", kl_clock_us());
}

/*
 * Remakes the presenter and the canvas at the window's size; nonzero when
 * it cannot.
 */
static int
ph_resize(
	struct ph_window *photos)
{
	uint32_t *pixels;
	int see_through;
	int status;

	/* The presenter at the window's size. */
	status = kl_window_present_resize(photos->window, &photos->width, &photos->height);
	if (status != 0) {
		ph_log("FAILED operation=present error=%d", status);
		return -1;
	}

	/* The compositor's glass, when the frames are blended by their alpha (decided at the first size). */
	if (!photos->glass_decided) {
		photos->glass_decided = 1;
		see_through = kl_window_see_through(photos->window);
		if (see_through) {
			photos->style.glass = 1;
			photos->view.glass = 1;
		}

		/* The log line the tests read. */
		ph_log("GLASS see_through=%d", see_through);
	}

	/* A frame's pixels of its size. */
	pixels = malloc((size_t)photos->width * (size_t)photos->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* The canvas on them, in place of the old one. */
	if (photos->canvas_made)
		kl_canvas_release(&photos->canvas);
	photos->canvas_made = 0;
	free(photos->pixels);
	photos->pixels = pixels;
	status = kl_canvas_init(&photos->canvas, photos->pixels, (size_t)photos->width, (int)photos->width, (int)photos->height);
	if (status != 0)
		return -1;

	/* Succeeded: drawn again at the new size. */
	photos->canvas_made = 1;
	photos->style.canvas = &photos->canvas;
	photos->dirty = 1;
	return 0;
}

/*
 * Draws and shows a frame when something changed or moves, queues the
 * pictures it wants, then gives the view the keys no widget took.
 */
static void
ph_draw(
	struct ph_window *photos,
	uint64_t now_us)
{
	struct kl_glass_panel panels[PH_PANELS_MAX];
	struct kl_event event;
	size_t count;
	size_t album;
	long open;
	long chosen;
	int status;
	int error;
	int taken;
	int list;
	int adding;

	/* Nothing changed and nothing moves: no frame. */
	if (!photos->dirty && !photos->moving)
		return;

	/* The view, and the pictures it wants; what its widgets changed while it drew is drawn in the next frame. */
	photos->dirty = 0;
	open = photos->view.open;
	chosen = photos->view.chosen;
	list = photos->view.list;
	album = photos->view.album;
	adding = photos->view.adding;
	kl_ui_begin(photos->ui, now_us);
	ph_view_draw(&photos->view, photos->ui, &photos->style, (int)photos->width, (int)photos->height, now_us);
	if (open != photos->view.open || chosen != photos->view.chosen || list != photos->view.list || album != photos->view.album ||
	    adding != photos->view.adding)
		photos->dirty = 1;
	photos->moving = kl_ui_end(photos->ui, now_us);
	kl_ui_window_text(photos->ui, photos->window);
	ph_jobs(photos);

	/* The glass's panels for the frame; a compositor without glass leaves the window opaque from the next one. */
	if (photos->view.glass) {
		count = ph_view_panels(&photos->view, (int)photos->width, (int)photos->height, panels, PH_PANELS_MAX);
		error = kl_window_set_glass(photos->window, panels, count);
		if (error != 0) {
			ph_log("GLASS failed error=%d", error);
			photos->view.glass = 0;
			photos->style.glass = 0;
			photos->dirty = 1;
		}
	}

	/* The frame shown. */
	status = kl_window_present(photos->window, photos->pixels, (size_t)photos->width);
	if (status == EAGAIN)
		photos->resized = 1;

	/* The keys no widget took are the view's; it draws again. */
	for (;;) {
		taken = kl_ui_take(photos->ui, &event);
		if (!taken)
			break;
		if (event.kind == KL_EVENT_KEY) {
			ph_view_key(&photos->view, event.code, event.modifiers, now_us);
			photos->dirty = 1;
		}
	}
}

/*
 * Reports how long the loop may wait for input (ms): no time while a frame
 * is due, a frame's time while a widget moves, a little while the thread
 * makes pictures, the view's own time, or a second.
 */
static int
ph_wait(
	const struct ph_window *photos,
	uint64_t now_us)
{
	int wait;
	int busy;

	/* A frame due now. */
	if (photos->dirty)
		return 0;

	/* A widget moving. */
	if (photos->moving)
		return PH_MOVING_MS;

	/* The thread at work: its results are looked for soon. */
	busy = ph_worker_busy();
	if (busy)
		return PH_WORKING_MS;

	/* The view's time, or a second. */
	wait = ph_view_wait(&photos->view, now_us);
	if (wait < 0 || wait > PH_IDLE_MS)
		wait = PH_IDLE_MS;
	return wait;
}
