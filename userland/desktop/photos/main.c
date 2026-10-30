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
 * view's input.  The photos are the library's in ~/Pictures/Media (its
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
#include "media-worker.h"
#include "../picture/png-write.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

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
	/* The compositor is the only entry point to the CLI-owned library. */
	struct kl_system *system;
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
	int marks_failed;

	/*
	 * The marks' update under way (0: none, ws157-p014) and the photos
	 * and albums it carries, marked changed again when it fails; and the
	 * identity a deletion waits on it for.
	 */
	uint32_t marks_request;
	char (*marks_photos)[PH_ID_SIZE];
	size_t marks_photo_count;
	char (*marks_albums)[PH_ID_SIZE];
	size_t marks_album_count;
	char remove_after[PH_ID_SIZE];

	/* Coalesces media notifications while a background import or refresh is running. */
	int media_again;
	/* A confirmation retains its identity independently of library refreshes. */
	char removing[PH_ID_SIZE];
	struct kl_file_chooser *chooser;
	unsigned chooser_for;
	int answered;
	char chosen[PH_PATH_MAX];
	int dirty;
	int resized;
	int moving;
	int glass_decided;

	/*
	 * Only the lit widget changed since the last frame (a motion or the
	 * pointer leaving, BUG-226): the next frame is drawn and shown within
	 * the parts the widgets tell (kl_ui_take_frame) alone.  buttons_held
	 * counts the buttons held, while which a motion draws the whole window.
	 */
	int lit_changed;
	unsigned buttons_held;
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
	{ 12U, 6U, KL_MENU_ITEM_NORMAL, "Back to Photos", PH_ACTION_BACK, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 13U, 6U, KL_MENU_ITEM_NORMAL, "Delete...", PH_ACTION_DELETE, KL_MENU_ROLE_NONE, 0U, 0U }
};

int main(int argc, char **argv);
static int ph_parse(int argc, char **argv, unsigned *width, unsigned *height, unsigned *timeout, const char **source);
static void ph_library_start(struct ph_window *photos);
static void ph_media_results(struct ph_window *photos);
static void ph_import_path(struct ph_window *photos, const char *source, int notice);
static void ph_delete_dialog(struct ph_window *photos, uint64_t now_us);
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
static int ph_marks_record(struct ph_window *photos);
static void ph_marks_forget(struct ph_window *photos);
static void ph_marks_taken(struct ph_window *photos);
static void ph_marks_answer(struct ph_window *photos, int error);
static void ph_marks_finish(struct ph_window *photos);
static void ph_remove_start(struct ph_window *photos, const char *id, uint64_t now_us);
static int ph_resize(struct ph_window *photos);
static void ph_draw(struct ph_window *photos, uint64_t now_us);
static void ph_draw_view(struct ph_window *photos, uint64_t now_us);
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

	/* Obtains metadata through the compositor before creating the initial view. */
	photos.system = kl_system_open(kl_app_display(photos.app));
	error = ph_media_worker_start();
	if (error != 0)
		ph_log("FAILED operation=media-thread error=%d", error);
	if (source != NULL) {
		ph_import_path(&photos, source, 0);
	} else {
		ph_library_start(&photos);
	}

	/* The initial empty view remains usable until metadata arrives. */
	(void)ph_view_reset(&photos.view);
	if (photos.system != NULL)
		(void)kl_system_media_watch(photos.system, 1U);

	/* Its window. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Photos";
	window_options.width = width;
	window_options.height = height;
	window_options.present = KL_PRESENT_VULKAN;
	photos.window = kl_app_window_create(photos.app, &window_options);
	if (photos.window == NULL) {
		ph_log("FAILED operation=window error=%d", errno);
		ph_media_worker_stop();
		kl_system_close(photos.system);
		kl_app_close(photos.app);
		ph_worker_stop();
		ph_view_release(&photos.view);
		return 1;
	}

	/*
	 * The pointer's motions merged, and each frame drawn once the
	 * compositor took the last one (ws210-p014 F2): the pointer crossing
	 * the view while a frame is on its way draws its latest place once,
	 * not a frame queued for every widget it crossed (2026-10-12, the Pi 4).
	 */
	error = kl_window_set_pacing(photos.window, KL_PACING_MOTION | KL_PACING_FRAME, 0U);
	ph_log("PACING motion=1 frame=1 errno=%d", error);

	/* The input of its frames. */
	photos.ui = kl_ui_create();
	if (photos.ui == NULL) {
		ph_log("FAILED operation=ui error=%d", errno);
		ph_media_worker_stop();
		kl_system_close(photos.system);
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
	ph_media_worker_stop();

	/* Commit edits deferred during import (waiting for an update under way) before releasing the UI-owned library. */
	ph_marks_finish(&photos);

	/* Thumbnail jobs no longer refer to the library after this join. */
	ph_worker_stop();
	kl_ui_destroy(photos.ui);
	if (photos.canvas_made)
		kl_canvas_release(&photos.canvas);
	free(photos.pixels);
	kl_system_close(photos.system);
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

/* Schedules a snapshot without disturbing the currently browsed library. */
static void
ph_library_start(
	struct ph_window *photos)
{
	int error;

	/* A running import will be followed by one coalesced refresh if notifications arrive. */
	error = ph_media_worker_queue(NULL, 0);
	if (error == EBUSY) {
		photos->media_again = 1;
	} else if (error != 0) {
		ph_view_notice(&photos->view, "The media library is unavailable.", kl_clock_us());
		photos->dirty = 1;
	}
}

/* Enqueues a file or folder import while leaving the current view usable. */
static void
ph_import_path(
	struct ph_window *photos,
	const char *source,
	int notice)
{
	int error;

	/* Copying, hashing, metadata IO and synchronous API waits all belong to the worker. */
	error = ph_media_worker_queue(source, notice);
	if (error == EBUSY) {
		ph_view_notice(&photos->view,
			       "A media operation is already running. Try "
			       "again when it finishes.",
			       kl_clock_us());
	} else if (error != 0) {
		ph_view_notice(&photos->view,
			       "The media import could not be started.",
			       kl_clock_us());
	} else if (notice) {
		ph_view_notice(&photos->view,
			       "Importing media in the background...",
			       kl_clock_us());
	}

	/* A notice is drawn without waiting for the compositor's helper. */
	photos->dirty = 1;
}

/* Requests a fresh snapshot after preserving local metadata changes. */
static void
ph_refresh(
	struct ph_window *photos)
{
	/* A failed or unanswered metadata update must not be discarded: the refresh follows its answer. */
	if (photos->marks_failed) {
		if (photos->marks_request == 0U)
			photos->view.save = 1;
		ph_marks(photos);
		if (photos->marks_failed) {
			photos->media_again = 1;
			return;
		}
	}

	/* The view and its thumbnail cache remain usable while the request runs. */
	ph_library_start(photos);
}

/* Installs completed metadata only on the UI thread, after import IO has finished. */
static void
ph_media_results(
	struct ph_window *photos)
{
	struct ph_media_result result;
	struct ph_view_keep keep;
	FILE *snapshot;
	int taken;
	int error;
	int view_error;

	/* Taking a result never waits on the compositor or the import job. */
	taken = ph_media_worker_take(&result);
	if (!taken)
		return;

	/* Unsaved marks are retained; a later snapshot will include their committed values. */
	if (photos->view.save || photos->marks_failed) {
		if (photos->marks_failed && photos->marks_request == 0U)
			photos->view.save = 1;
		ph_marks(photos);
		if (result.descriptor >= 0)
			close(result.descriptor);
		if (photos->marks_failed) {
			photos->media_again = 1;
			return;
		}

		/* Replace the stale snapshot with one including the saved metadata changes. */
		ph_library_start(photos);
		return;
	}

	/* A failed import can still have a valid snapshot containing partial successes. */
	error = result.error;
	if (result.descriptor >= 0) {
		snapshot = fdopen(result.descriptor, "r");
		if (snapshot == NULL) {
			error = errno;
			close(result.descriptor);
		} else {
			/* The view keeps its place, its photo shown whole and its slideshow across the reread. */
			ph_view_remember(&photos->view, &keep);
			ph_worker_drop_thumbs();
			ph_library_release();
			photos->root_known = 0;
			error = ph_snapshot_read(
			    snapshot, photos->root, sizeof(photos->root));
			fclose(snapshot);
			if (error == 0)
				photos->root_known = 1;
			else
				ph_library_release();

			/* A failed snapshot also invalidates the old view's indices and jobs. */
			view_error = ph_view_reset(&photos->view);
			if (view_error != 0) {
				ph_library_release();
				photos->root_known = 0;
				(void)ph_view_reset(&photos->view);
				if (error == 0)
					error = view_error;
			}

			/* What of the noted view is still in the library. */
			ph_view_restore(&photos->view, &keep);
		}
	}

	/* Completion gives a useful status while the metadata and view remain UI-owned. */
	if (error != 0 || result.error != 0) {
		ph_view_notice(&photos->view,
			       "The media operation could not be completed.",
			       kl_clock_us());
	} else if (result.removed) {
		ph_view_notice(&photos->view, "Media deleted from the library.", kl_clock_us());
	} else if (result.imported && result.notice) {
		ph_view_notice(&photos->view,
			       "Media imported into the library.",
			       kl_clock_us());
	}

	/* Completion invalidates the visible metadata, without changing it from the worker. */
	photos->dirty = 1;
	ph_log("MEDIA done imported=%d error=%d snapshot=%d",
	       result.imported,
	       result.error,
	       error);

	/* External additions during the job are included by a single follow-up request. */
	if (photos->media_again) {
		photos->media_again = 0;
		ph_library_start(photos);
	}
}

/*
 * Shows libkeiland's file chooser for an import: a photo, or
 * the folder to import (PH_ACTION_IMPORT or _FOLDER), the pictures shown
 * first.  One shown already answers in its time.
 */
static void
ph_choose(
	struct ph_window *photos,
	unsigned purpose)
{
	static const struct kl_file_filter filters[] = {
	    {"Photos and Videos", "jpg jpeg jpe png gif mp4 mov m4v 3gp webm mkv avi"},
	    {"All files", NULL}};
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
	if (purpose == PH_ACTION_IMPORT_FOLDER) {
		options.mode = KL_FILE_CHOOSER_FOLDER;
		options.title = "Import Folder";
	}

	/* The chooser belongs to this Photos window. */
	options.application = PH_APPLICATION;
	options.folder = folder;
	options.filters = filters;
	options.filter_count = sizeof(filters) / sizeof(filters[0]);
	/* Folder selection does not offer file extension filters. */
	if (purpose == PH_ACTION_IMPORT_FOLDER) {
		options.filters = NULL;
		options.filter_count = 0U;
	}

	/* Use the same fonts as the application for either selection mode. */
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

	/* A path chosen: the photo or the directory itself. */
	(void)chooser;
	(void)filter;
	photos = data;
	photos->chosen[0] = '\0';
	if (result == KL_FILE_CHOOSER_CHOSEN && path != NULL) {
		(void)snprintf(
		    photos->chosen, sizeof(photos->chosen), "%s", path);
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
	unsigned changed;
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

		/* Preserves the chosen identity before a completed refresh replaces the view. */
		if (photos->view.removing[0] != '\0') {
			memcpy(photos->removing, photos->view.removing, PH_ID_SIZE);
			photos->view.removing[0] = '\0';
			photos->dirty = 1;
		}

		/* Installs ready snapshots without blocking the next input dispatch. */
		ph_media_results(photos);

		/* A direct CLI import wakes every watching Photos instance. */
		changed = 0U;
		if (photos->system != NULL)
			(void)kl_system_dispatch(photos->system, &changed);
		if ((changed & KL_SYSTEM_CHANGED_MEDIA_DONE) != 0U)
			ph_marks_taken(photos);
		if ((changed & KL_SYSTEM_CHANGED_MEDIA) != 0U) {
			ph_marks(photos);
			ph_refresh(photos);
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
				/* A pending confirmation cannot be retargeted by menu actions. */
				if (photos->removing[0] == '\0' || event.input.code == PH_ACTION_QUIT)
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

	/*
	 * A motion of the pointer draws again only when it lights another
	 * widget or moves a held one (as Music, 2026-10-12): the lit widgets'
	 * part alone, or the whole window while a button is held.
	 */
	if (event->kind == KL_WINDOW_MOTION) {
		taken = kl_ui_pointer_motion(photos->ui, event->x, event->y);
		if (taken && photos->buttons_held != 0U)
			photos->dirty = 1;
		else if (taken)
			photos->lit_changed = 1;
		return;
	}

	/* The pointer leaving darkens the lit widget: its part alone is drawn again. */
	if (event->kind == KL_WINDOW_LEAVE) {
		taken = kl_ui_pointer_leave(photos->ui);
		if (taken)
			photos->lit_changed = 1;
		return;
	}

	/* The buttons held, for the motions while one is. */
	if (event->kind == KL_WINDOW_BUTTON) {
		if (event->pressed)
			photos->buttons_held++;
		else if (photos->buttons_held != 0U)
			photos->buttons_held--;
	}

	/* The widgets' other input draws again. */
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

	/* Only a photo of the library. */
	list = ph_photos(&count);
	if (photo < 0 || (size_t)photo >= count)
		return;

	/* Its file as a URI with its line's end. */
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
 * the one, else its file's picture fitted to PH_DRAG_SIDE (made by
 * keiland-preview in its sandbox) and turned as the
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

	/* Otherwise the file's picture, fitted. */
	list = ph_photos(&count);
	error = ph_preview(list[photo].path, PH_DRAG_SIDE, 0, &fitted);
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
		if (*byte >= 'a' && *byte <= 'z')
			plain = 1;
		if (*byte >= 'A' && *byte <= 'Z')
			plain = 1;
		if (*byte >= '0' && *byte <= '9')
			plain = 1;
		if (*byte == '-' ||
		    *byte == '.' ||
		    *byte == '_' ||
		    *byte == '~' ||
		    *byte == '/')
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

/*
 * Starts the update of the marks and albums changed (ws157-p014: without
 * waiting; its answer comes to ph_marks_answer).  One at a time: edits
 * made meanwhile wait for its answer.
 */
static void
ph_marks(
	struct ph_window *photos)
{
	FILE *changes;
	int error;
	int closed;

	/* One update under way at a time. */
	if (photos->marks_request != 0U)
		return;

	/* Metadata edits remain local while import IO owns the helper; the UI stays usable. */
	error = ph_media_worker_busy();
	if (error)
		return;

	/* A read-only frame produces no CLI mutation. */
	if (!photos->view.save)
		return;
	photos->view.save = 0;
	photos->marks_failed = 1;
	changes = tmpfile();
	if (changes == NULL) {
		ph_view_notice(&photos->view, "The media changes could not be prepared.", kl_clock_us());
		return;
	}

	/* Sends only dirty id-based marks and albums, preserving concurrent imports. */
	error = ph_snapshot_changes(changes);
	if (error == 0) {
		closed = fflush(changes);
		if (closed != 0)
			error = EIO;
	}

	/* Rewinds the metadata update before borrowing its descriptor for the request. */
	if (error == 0) {
		closed = fseek(changes, 0L, SEEK_SET);
		if (closed != 0)
			error = EIO;
	}

	/* What it carries, then the request (its descriptor is copied as it is sent). */
	if (error == 0)
		error = ph_marks_record(photos);
	if (error == 0) {
		error = kl_system_media_apply_begin(photos->system, fileno(changes), &photos->marks_request);
		if (error != 0)
			ph_marks_forget(photos);
	}

	/* Keeps dirty rows after a failed update so an explicit retry can preserve them. */
	fclose(changes);
	if (error != 0) {
		ph_view_notice(&photos->view, "The media changes could not be saved.", kl_clock_us());
		return;
	}
}

/*
 * Keeps the identities of the photos and albums changed (the update
 * carries them) and clears their marks: edits made while it is under way
 * mark them again.  Returns 0 or ENOMEM.
 */
static int
ph_marks_record(
	struct ph_window *photos)
{
	struct ph_photo *list;
	struct ph_album *albums;
	size_t count;
	size_t album_count;
	size_t index;

	/* Room for every photo and album at most. */
	list = ph_photos(&count);
	albums = ph_albums(&album_count);
	photos->marks_photos = calloc(count + 1U, sizeof(photos->marks_photos[0]));
	if (photos->marks_photos == NULL)
		return ENOMEM;
	photos->marks_albums = calloc(album_count + 1U, sizeof(photos->marks_albums[0]));
	if (photos->marks_albums == NULL) {
		free(photos->marks_photos);
		photos->marks_photos = NULL;
		return ENOMEM;
	}

	/* The changed photos, their marks cleared. */
	photos->marks_photo_count = 0U;
	for (index = 0U; index < count; index++) {
		if (!list[index].changed)
			continue;
		memcpy(photos->marks_photos[photos->marks_photo_count], list[index].id, PH_ID_SIZE);
		photos->marks_photo_count++;
		list[index].changed = 0;
	}

	/* The changed albums, their marks cleared. */
	photos->marks_album_count = 0U;
	for (index = 0U; index < album_count; index++) {
		if (!albums[index].changed)
			continue;
		memcpy(photos->marks_albums[photos->marks_album_count], albums[index].id, PH_ID_SIZE);
		photos->marks_album_count++;
		albums[index].changed = 0;
	}

	/* Succeeded: recorded. */
	return 0;
}

/* Marks again what an update carried (it failed, or was not sent), and forgets it. */
static void
ph_marks_forget(
	struct ph_window *photos)
{
	struct ph_photo *list;
	struct ph_album *albums;
	size_t count;
	size_t album_count;
	size_t index;
	size_t kept;
	int same;

	/* Each photo carried, still in the library. */
	list = ph_photos(&count);
	for (kept = 0U; kept < photos->marks_photo_count; kept++) {
		for (index = 0U; index < count; index++) {
			same = memcmp(list[index].id, photos->marks_photos[kept], PH_ID_SIZE);
			if (same == 0)
				list[index].changed = 1;
		}
	}

	/* Each album carried, still there. */
	albums = ph_albums(&album_count);
	for (kept = 0U; kept < photos->marks_album_count; kept++) {
		for (index = 0U; index < album_count; index++) {
			same = memcmp(albums[index].id, photos->marks_albums[kept], PH_ID_SIZE);
			if (same == 0)
				albums[index].changed = 1;
		}
	}

	/* The record goes. */
	free(photos->marks_photos);
	free(photos->marks_albums);
	photos->marks_photos = NULL;
	photos->marks_albums = NULL;
	photos->marks_photo_count = 0U;
	photos->marks_album_count = 0U;
}

/* Takes the media library's answers: the marks' update's is handled, any other dropped. */
static void
ph_marks_taken(
	struct ph_window *photos)
{
	uint32_t request;
	int descriptor;
	int error;
	int taken;

	/* Each answer. */
	for (;;) {
		taken = kl_system_media_take(photos->system, &request, &error, &descriptor);
		if (!taken)
			break;
		if (descriptor >= 0)
			close(descriptor);
		if (request == photos->marks_request && request != 0U)
			ph_marks_answer(photos, error);
	}
}

/*
 * Handles the marks' update's answer: a failure marks its rows again
 * (saved at the next change or refresh); a success lets the work that
 * waited for it go on (a deletion, a refresh).
 */
static void
ph_marks_answer(
	struct ph_window *photos,
	int error)
{
	char id[PH_ID_SIZE];

	/* No update under way any more. */
	photos->marks_request = 0U;
	photos->dirty = 1;

	/* A failure keeps the marks for the next try. */
	if (error != 0) {
		ph_marks_forget(photos);
		photos->marks_failed = 1;
		photos->remove_after[0] = '\0';
		ph_view_notice(&photos->view, "The media changes could not be saved.", kl_clock_us());
		return;
	}

	/* Saved: the record goes (without marking anything again). */
	photos->marks_photo_count = 0U;
	photos->marks_album_count = 0U;
	ph_marks_forget(photos);
	photos->marks_failed = 0;

	/* A deletion that waited for it. */
	if (photos->remove_after[0] != '\0') {
		memcpy(id, photos->remove_after, PH_ID_SIZE);
		photos->remove_after[0] = '\0';
		ph_remove_start(photos, id, kl_clock_us());
	}

	/* A refresh that waited for it. */
	if (photos->media_again) {
		photos->media_again = 0;
		ph_library_start(photos);
	}
}

/*
 * Before the window goes: waits (a few seconds at most) for an update
 * under way, then saves what is still changed, waiting for it.
 */
static void
ph_marks_finish(
	struct ph_window *photos)
{
	FILE *changes;
	unsigned tries;
	unsigned changed;
	int descriptor;
	int error;
	int status;

	/* An update under way: its answer, dispatched by hand. */
	for (tries = 0U; photos->marks_request != 0U && tries < 500U; tries++) {
		status = wl_display_roundtrip(kl_app_display(photos->app));
		if (status < 0)
			break;
		changed = 0U;
		(void)kl_system_dispatch(photos->system, &changed);
		ph_marks_taken(photos);
	}

	/* Nothing left to save. */
	if (photos->system == NULL)
		return;
	if (photos->marks_failed && photos->marks_request == 0U)
		photos->view.save = 1;
	if (!photos->view.save || photos->marks_request != 0U)
		return;

	/* The rest, written down and rewound. */
	changes = tmpfile();
	if (changes == NULL)
		return;
	error = ph_snapshot_changes(changes);
	if (error == 0) {
		status = fflush(changes);
		if (status != 0)
			error = EIO;
	}

	/* Rewound for the request. */
	if (error == 0) {
		status = fseek(changes, 0L, SEEK_SET);
		if (status != 0)
			error = EIO;
	}

	/* Saved, waiting for it (the window is closing). */
	if (error == 0) {
		error = kl_system_media_apply(photos->system, fileno(changes), &descriptor);
		if (error == 0)
			close(descriptor);
	}

	/* The file goes. */
	fclose(changes);
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
	struct kl_rect parts[KL_FRAME_PARTS_MAX];
	struct kl_event event;
	unsigned part_count;
	unsigned index;
	size_t count;
	int status;
	int error;
	int taken;
	int need;
	int pace;

	/* Nothing changed and nothing moves: no frame. */
	if (!photos->dirty && !photos->moving && !photos->lit_changed)
		return;

	/*
	 * A frame once the compositor took the last one (ws210-p014 F2.2):
	 * until then nothing is consumed, so what changes meanwhile is drawn
	 * as one frame in its latest state.
	 */
	pace = kl_window_frame_wait(photos->window, now_us);
	if (pace != 0)
		return;

	/*
	 * Only another widget lit: the frame is drawn and shown within the
	 * parts the widgets tell alone (BUG-226, ws210-p014: the widget lit
	 * when the last frame was drawn and the one lit now; none when the
	 * pointer came back); a whole window for each hover was a CPU frame on
	 * the Pi 4 (2026-10-12).
	 */
	part_count = 0;
	if (!photos->dirty && !photos->moving && photos->lit_changed) {
		need = kl_ui_take_frame(photos->ui, parts, KL_FRAME_PARTS_MAX, &part_count);
		if (need == KL_FRAME_NONE) {
			photos->lit_changed = 0;
			return;
		}
		if (need == KL_FRAME_WHOLE)
			part_count = 0;
	}
	photos->lit_changed = 0;

	/* The view, all of it; what its widgets changed while it drew is drawn in the next frame. */
	photos->dirty = 0;
	if (part_count == 0U)
		ph_draw_view(photos, now_us);

	/* Or within each part in turn, the rest keeping the last frame's pixels. */
	for (index = 0; index < part_count; index++) {
		kl_canvas_clip_push(&photos->canvas, &parts[index]);
		ph_draw_view(photos, now_us);
		kl_canvas_clip_pop(&photos->canvas);
	}
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

	/* The frame shown, by its parts alone when it was drawn so. */
	status = kl_window_present_parts(photos->window, photos->pixels, (size_t)photos->width, parts, part_count);
	if (status == EAGAIN)
		photos->resized = 1;

	/* The keys no widget took are the view's; it draws again. */
	for (;;) {
		taken = kl_ui_take(photos->ui, &event);
		if (!taken)
			break;
		if (event.kind == KL_EVENT_KEY) {
			if (photos->removing[0] == '\0')
				ph_view_key(&photos->view, event.code, event.modifiers, now_us);
			photos->dirty = 1;
		}
	}
}

/*
 * Draws the view and the dialog over it in one frame of the widgets, and
 * the pictures the view wants; a change of what the view shows asks for
 * another frame of the whole window.
 */
static void
ph_draw_view(
	struct ph_window *photos,
	uint64_t now_us)
{
	size_t album;
	long open;
	long chosen;
	int list;
	int adding;

	/* What the view shows before it draws. */
	open = photos->view.open;
	chosen = photos->view.chosen;
	list = photos->view.list;
	album = photos->view.album;
	adding = photos->view.adding;

	/* The view; what it changed while it drew is drawn in the next frame. */
	kl_ui_begin(photos->ui, now_us);
	ph_view_draw(&photos->view, photos->ui, &photos->style, (int)photos->width, (int)photos->height, now_us);
	if (open != photos->view.open || chosen != photos->view.chosen || list != photos->view.list || album != photos->view.album ||
	    adding != photos->view.adding)
		photos->dirty = 1;

	/* The dialog consumes input after the view, using the common modal widget. */
	if (photos->removing[0] != '\0')
		ph_delete_dialog(photos, now_us);
	photos->moving = kl_ui_end(photos->ui, now_us);
}

/* Confirms one selected identity before admitting a background deletion job. */
static void
ph_delete_dialog(
	struct ph_window *photos,
	uint64_t now_us)
{
	static const char *const labels[] = {"Delete", "Cancel"};
	struct kl_rect area;
	int answer;

	/* The confirmation covers both the managed file and its library entry. */
	area.x = 0;
	area.y = 0;
	area.width = (int)photos->width;
	area.height = (int)photos->height;
	answer = kl_dialog(photos->ui, &photos->style, 100U, &area, "Delete media?", "Permanently delete this item from the library? The original import source is kept.", labels, 2);
	if (answer < 0)
		return;

	/* Cancel leaves the database and the pending worker untouched. */
	if (answer != 0) {
		photos->removing[0] = '\0';
		photos->dirty = 1;
		return;
	}

	/* Preserve local metadata changes before the worker requests the latest database: an update under way is waited for. */
	ph_marks(photos);
	if (photos->marks_request != 0U) {
		memcpy(photos->remove_after, photos->removing, PH_ID_SIZE);
		photos->removing[0] = '\0';
		photos->dirty = 1;
		return;
	}

	/* The deletion, unless the marks could not be saved. */
	if (photos->marks_failed) {
		photos->removing[0] = '\0';
		photos->dirty = 1;
		ph_view_notice(&photos->view, "The media deletion could not be started.", now_us);
		return;
	}

	/* The deletion now. */
	ph_remove_start(photos, photos->removing, now_us);
	photos->removing[0] = '\0';
	photos->dirty = 1;
}

/* Starts the worker's deletion of one identity and says so. */
static void
ph_remove_start(
	struct ph_window *photos,
	const char *id,
	uint64_t now_us)
{
	int error;

	/* The worker's job. */
	error = ph_media_worker_remove(id);
	if (error == EBUSY) {
		ph_view_notice(&photos->view, "A media operation is already running. Try again when it finishes.", now_us);
		return;
	}

	/* A refusal cannot be presented as a completed deletion. */
	if (error != 0) {
		ph_view_notice(&photos->view, "The media deletion could not be started.", now_us);
		return;
	}

	/* Succeeded: the UI remains usable while the worker waits for the CLI. */
	ph_view_notice(&photos->view, "Deleting media in the background...", now_us);
}

/*
 * Reports how long the loop may wait for input (ms): no time while a frame
 * is due, a frame's time while a widget moves, until the compositor took
 * the last frame (ws210-p014 F2.6: its answer wakes the wait), a little
 * while the thread makes pictures, the view's own time, or a second.
 */
static int
ph_wait(
	const struct ph_window *photos,
	uint64_t now_us)
{
	int pace;
	int wait;
	int busy;

	/* A frame wanted: whether it may be drawn now, or how long the compositor's answer is waited for at most (-1: until it comes). */
	pace = -1;
	if (photos->dirty || photos->lit_changed || photos->moving)
		pace = kl_window_frame_wait(photos->window, now_us);

	/* A frame due now. */
	if (pace == 0 && photos->dirty)
		return 0;
	if (pace == 0 && photos->lit_changed)
		return 0;

	/* A widget moving. */
	if (pace == 0 && photos->moving)
		return PH_MOVING_MS;

	/* The thread at work: its results are looked for soon. */
	busy = ph_media_worker_busy();
	if (!busy)
		busy = ph_worker_busy();
	if (busy)
		return PH_WORKING_MS;

	/* The view's time, or a second. */
	wait = ph_view_wait(&photos->view, now_us);
	if (wait < 0 || wait > PH_IDLE_MS)
		wait = PH_IDLE_MS;

	/* The compositor's answer, waited for at most so long. */
	if (pace > 0 && pace < wait)
		wait = pace;
	return wait;
}
