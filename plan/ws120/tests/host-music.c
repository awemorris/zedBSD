/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws120-p009: draws Music's view (userland/desktop/music/view.c) on the
 * host into pictures and drives it as the window would: every song, an
 * album chosen, its Play button, a song playing (the window's part told
 * by hand), Next, a double click on a song, Space, a search, the reason
 * nothing can play, the cards on glass, and an empty collection.  The
 * requests the view queues and the "MUSIC" lines it logs are checked.
 *
 *     host-music FONT FALLBACK PREFIX FOLDER
 *
 * FOLDER holds the collection make-m4a.py --view writes (FOLDER/Music).
 * Writes PREFIX-NAME.ppm for each picture (PREFIX-NAME.pam and .panels on
 * glass) and prints "PASS name" or "FAIL name ..." for each check; exits
 * with 1 when one failed.
 */

#include "userland/desktop/music/music.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The window's size. */
#define TEST_WIDTH		1040
#define TEST_HEIGHT		680

/* Where the view puts things at that size (view.c's layout: the albums 291 wide, the bar 80 high). */
#define TEST_ALBUM_X		150
#define TEST_ALBUM_Y		124
#define TEST_ALBUM_ROW		60
#define TEST_PLAY_X		507
#define TEST_PLAY_Y		135
#define TEST_SONG_X		600
#define TEST_SONG_Y		188
#define TEST_SONG_ROW		40
#define TEST_BAR_Y		628
#define TEST_MIDDLE		520

/* The log the view wrote, for the checks. */
static char test_log[32768];
static size_t test_log_length;

/* The checks that failed. */
static int test_failures;

/* The frame's time. */
static uint64_t test_now = 1000000000U;

int main(int argc, char **argv);
static void test_frame(struct mu_view *view, struct kl_ui *ui, const struct kl_style *style);
static void test_click(struct mu_view *view, struct kl_ui *ui, const struct kl_style *style, int x, int y, int twice);
static void test_check(const char *name, const char *expected);
static void test_request(struct mu_view *view, const char *name, unsigned action, long song);
static int test_save(const struct mu_view *view, const struct kl_canvas *canvas, const char *prefix, const char *name);

/*
 * Draws and drives the view and checks what it did.
 */
int
main(
	int argc,
	char **argv)
{
	const struct mu_song *songs;
	struct kl_canvas canvas;
	struct kl_style style;
	struct kl_text text;
	struct mu_view view;
	struct kl_ui *ui;
	struct mu_request request;
	uint32_t *pixels;
	char folder[1024];
	size_t count;
	int taken;
	int error;

	/* The arguments. */
	if (argc != 5) {
		fprintf(stderr, "usage: host-music FONT FALLBACK PREFIX FOLDER\n");
		return 2;
	}

	/* The fonts. */
	kl_text_companions("userland/desktop/fonts/Mahora-Bold.ttf", "userland/desktop/fonts/JetBrainsMono-Regular.ttf");
	error = kl_text_open(&text, argv[1], argv[2]);
	if (error != 0) {
		fprintf(stderr, "host-music: fonts error=%d\n", error);
		return 2;
	}

	/* The frame and its canvas. */
	pixels = calloc((size_t)TEST_WIDTH * TEST_HEIGHT, sizeof(pixels[0]));
	if (pixels == NULL)
		return 2;
	error = kl_canvas_init(&canvas, pixels, TEST_WIDTH, TEST_WIDTH, TEST_HEIGHT);
	if (error != 0)
		return 2;

	/* The input and the style. */
	ui = kl_ui_create();
	if (ui == NULL)
		return 2;
	style.canvas = &canvas;
	style.text = &text;
	style.theme = kl_theme_default();
	style.glass = 0;

	/* The collection. */
	(void)snprintf(folder, sizeof(folder), "%s/Music", argv[4]);
	mu_library_set_cache(NULL);
	error = mu_library_scan(folder);
	songs = mu_songs(&count);
	if (error != 0 || count != 14U) {
		printf("FAIL library error=%d songs=%lu (14 expected)\n", error, (unsigned long)count);
		return 1;
	}
	error = mu_view_init(&view);
	if (error != 0)
		return 2;

	/* Every song at the start; the covers are made as they are drawn. */
	test_frame(&view, ui, &style);
	test_check("covers", "COVER album=1 error=0\nCOVER album=2 error=0\nCOVER album=3 error=0");
	(void)test_save(&view, &canvas, argv[3], "all");

	/* The albums are Demos, Peaks, Sunrise, Tides: Sunrise (the third) chosen. */
	test_click(&view, ui, &style, TEST_ALBUM_X, TEST_ALBUM_Y + 3 * TEST_ALBUM_ROW, 0);
	test_check("album", "ALBUM index=2");
	test_frame(&view, ui, &style);
	(void)test_save(&view, &canvas, argv[3], "album");

	/* Play: the album's first song. */
	test_click(&view, ui, &style, TEST_PLAY_X, TEST_PLAY_Y, 0);
	test_check("play-album", "REQUEST action=5 song=5");
	test_request(&view, "play-album-request", MU_ACTION_SONG, 5);

	/* The window plays it: a minute and a quarter in. */
	view.playing = 5;
	view.state = MU_PLAYING;
	view.position = 75.0;
	view.length = (double)songs[5].duration_ms / 1000.0;
	test_frame(&view, ui, &style);
	(void)test_save(&view, &canvas, argv[3], "playing");

	/* Next, in the bar. */
	test_click(&view, ui, &style, TEST_MIDDLE + 52, TEST_BAR_Y, 0);
	test_request(&view, "next", MU_ACTION_NEXT, 5);

	/* The play button pauses. */
	test_click(&view, ui, &style, TEST_MIDDLE, TEST_BAR_Y, 0);
	test_request(&view, "pause", MU_ACTION_PLAY, 5);

	/* A double click on the third song plays it; a single click chooses only. */
	test_click(&view, ui, &style, TEST_SONG_X, TEST_SONG_Y + 2 * TEST_SONG_ROW, 0);
	taken = mu_view_take_request(&view, &request);
	if (taken) {
		printf("FAIL single-click a request action=%u\n", request.action);
		test_failures++;
	} else {
		printf("PASS single-click\n");
	}
	test_click(&view, ui, &style, TEST_SONG_X, TEST_SONG_Y + 2 * TEST_SONG_ROW, 1);
	test_request(&view, "double-click", MU_ACTION_SONG, 7);

	/* Space with nothing focused plays or pauses. */
	(void)kl_ui_key(ui, KL_KEY_SPACE, 1, 0U);
	test_frame(&view, ui, &style);
	test_request(&view, "space", MU_ACTION_PLAY, 5);

	/* The search field focused takes Space; Escape, then Enter, leave it and Space plays or pauses again (ws177-p021). */
	{
		struct mu_request stray;

		test_click(&view, ui, &style, 100, 68, 0);
		(void)kl_ui_key(ui, KL_KEY_SPACE, 1, 0U);
		test_frame(&view, ui, &style);
		if (mu_view_take_request(&view, &stray)) {
			printf("FAIL search-space a request action=%u\n", stray.action);
			test_failures++;
		} else {
			printf("PASS search-space\n");
		}
		(void)kl_ui_key(ui, KL_KEY_ESC, 1, 0U);
		test_frame(&view, ui, &style);
		(void)kl_ui_key(ui, KL_KEY_SPACE, 1, 0U);
		test_frame(&view, ui, &style);
		test_request(&view, "search-escape", MU_ACTION_PLAY, 5);
		test_click(&view, ui, &style, 100, 68, 0);
		(void)kl_ui_key(ui, KL_KEY_ENTER, 1, 0U);
		test_frame(&view, ui, &style);
		(void)mu_view_take_request(&view, &stray);
		(void)kl_ui_key(ui, KL_KEY_SPACE, 1, 0U);
		test_frame(&view, ui, &style);
		test_request(&view, "search-enter", MU_ACTION_PLAY, 5);
		kl_field_set(&view.search, "");
	}

	/* Paused, on glass. */
	view.state = MU_PAUSED;
	view.glass = 1;
	style.glass = 1;
	test_frame(&view, ui, &style);
	(void)test_save(&view, &canvas, argv[3], "glass");
	view.glass = 0;
	style.glass = 0;

	/* A search over every song. */
	test_click(&view, ui, &style, TEST_ALBUM_X, TEST_ALBUM_Y, 0);
	test_check("all-songs", "ALBUM index=-1");
	kl_field_set(&view.search, "ben");
	test_frame(&view, ui, &style);
	(void)test_save(&view, &canvas, argv[3], "search");
	kl_field_set(&view.search, "");

	/* Nothing can play. */
	view.playing = -1;
	view.state = MU_STOPPED;
	(void)snprintf(view.problem, sizeof(view.problem), "Playing needs libavcodec (the libavcodec package).");
	test_frame(&view, ui, &style);
	(void)test_save(&view, &canvas, argv[3], "no-codec");
	view.problem[0] = '\0';

	/* An empty collection. */
	mu_view_release(&view);
	mu_library_release();
	error = mu_view_init(&view);
	if (error != 0)
		return 2;
	test_frame(&view, ui, &style);
	(void)test_save(&view, &canvas, argv[3], "empty");

	/* The end. */
	mu_view_release(&view);
	kl_ui_destroy(ui);
	kl_canvas_release(&canvas);
	free(pixels);
	kl_text_close(&text);
	if (test_failures != 0) {
		printf("host-music: FAIL %d\n", test_failures);
		return 1;
	}
	printf("host-music: PASS\n");
	return 0;
}

/*
 * Keeps a log line of the view, for the checks.
 */
void
mu_log(
	const char *format,
	...)
{
	va_list arguments;
	int length;

	/* The line, after the others. */
	va_start(arguments, format);
	length = vsnprintf(test_log + test_log_length, sizeof(test_log) - test_log_length - 1U, format, arguments);
	va_end(arguments);
	if (length < 0 || (size_t)length >= sizeof(test_log) - test_log_length - 1U)
		return;

	/* The line's end. */
	test_log_length += (size_t)length;
	test_log[test_log_length] = '\n';
	test_log_length++;
	test_log[test_log_length] = '\0';
}

/*
 * Draws one frame of the view as the window does, and gives the keys no
 * widget took to the view.
 */
static void
test_frame(
	struct mu_view *view,
	struct kl_ui *ui,
	const struct kl_style *style)
{
	struct kl_event event;
	int taken;

	/* The frame. */
	kl_ui_begin(ui, test_now);
	mu_view_draw(view, ui, style, TEST_WIDTH, TEST_HEIGHT, test_now);
	(void)kl_ui_end(ui, test_now);

	/* The keys no widget took. */
	for (;;) {
		taken = kl_ui_take(ui, &event);
		if (!taken)
			break;
		if (event.kind == KL_EVENT_KEY)
			mu_view_key(view, event.code, event.modifiers, test_now);
	}
}

/*
 * Clicks at a point (twice, soon after each other, for a double click): the
 * pointer goes there, presses and releases, and a frame takes the click.
 */
static void
test_click(
	struct mu_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	int x,
	int y,
	int twice)
{
	/* A second after the last, so that the first is a single click. */
	test_now += 1000000U;
	(void)kl_ui_pointer_motion(ui, (double)x, (double)y);
	(void)kl_ui_pointer_button(ui, 1, test_now);
	(void)kl_ui_pointer_button(ui, 0, test_now + 50000U);
	test_frame(view, ui, style);
	if (!twice)
		return;

	/* The second, soon after. */
	test_now += 150000U;
	(void)kl_ui_pointer_button(ui, 1, test_now);
	(void)kl_ui_pointer_button(ui, 0, test_now + 50000U);
	test_frame(view, ui, style);
}

/*
 * Checks that the view logged a line, and empties the log.
 */
static void
test_check(
	const char *name,
	const char *expected)
{
	const char *found;

	/* The line among those logged. */
	found = strstr(test_log, expected);
	if (found == NULL) {
		printf("FAIL %s expected \"%s\" in:\n%s", name, expected, test_log);
		test_failures++;
	} else {
		printf("PASS %s\n", name);
	}

	/* The next check reads only what comes after. */
	test_log_length = 0;
	test_log[0] = '\0';
}

/* Checks the view's next request (and that it is the last). */
static void
test_request(
	struct mu_view *view,
	const char *name,
	unsigned action,
	long song)
{
	struct mu_request request;
	int taken;

	/* The request. */
	taken = mu_view_take_request(view, &request);
	if (!taken || request.action != action || request.song != song) {
		printf("FAIL %s expected action=%u song=%ld, got taken=%d action=%u song=%ld\n", name, action, song, taken,
		    request.action, request.song);
		test_failures++;
		return;
	}

	/* No other. */
	taken = mu_view_take_request(view, &request);
	if (taken) {
		printf("FAIL %s another request action=%u\n", name, request.action);
		test_failures++;
		return;
	}
	printf("PASS %s\n", name);
	test_log_length = 0;
	test_log[0] = '\0';
}

/*
 * Writes a canvas as PREFIX-NAME.ppm, or on glass as PREFIX-NAME.pam with
 * its alpha and PREFIX-NAME.panels; nonzero when it cannot.
 */
static int
test_save(
	const struct mu_view *view,
	const struct kl_canvas *canvas,
	const char *prefix,
	const char *name)
{
	struct kl_glass_panel panels[4];
	const char *extension;
	char path[512];
	FILE *file;
	uint32_t pixel;
	size_t count;
	size_t index;
	int x;
	int y;

	/* The picture's file: PAM with its alpha on glass, else PPM. */
	extension = "ppm";
	if (view->glass)
		extension = "pam";
	(void)snprintf(path, sizeof(path), "%s-%s.%s", prefix, name, extension);
	file = fopen(path, "wb");
	if (file == NULL)
		return -1;

	/* The header. */
	if (view->glass)
		fprintf(file, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", canvas->width, canvas->height);
	else
		fprintf(file, "P6\n%d %d\n255\n", canvas->width, canvas->height);

	/* Each pixel's red, green and blue, and on glass its alpha. */
	for (y = 0; y < canvas->height; y++) {
		for (x = 0; x < canvas->width; x++) {
			pixel = canvas->pixels[(size_t)y * canvas->stride + (size_t)x];
			fputc((int)((pixel >> 16) & 0xffU), file);
			fputc((int)((pixel >> 8) & 0xffU), file);
			fputc((int)(pixel & 0xffU), file);
			if (view->glass)
				fputc((int)((pixel >> 24) & 0xffU), file);
		}
	}
	fclose(file);

	/* On glass, the panels beside it. */
	if (!view->glass)
		return 0;
	(void)snprintf(path, sizeof(path), "%s-%s.panels", prefix, name);
	file = fopen(path, "w");
	if (file == NULL)
		return -1;
	count = mu_view_panels(view, canvas->width, canvas->height, panels, 4);
	for (index = 0; index < count; index++)
		fprintf(file, "%d %d %d %d %d\n", panels[index].x, panels[index].y, panels[index].width, panels[index].height, panels[index].radius);
	fclose(file);
	return 0;
}

/* The appearance libkeiland's theme asks for: the light one (the host test has no compositor to ask). */
unsigned
kl_appearance_get(
	const struct kl_appearance *appearance)
{
	/* The light appearance. */
	(void)appearance;
	return KL_APPEARANCE_LIGHT;
}
