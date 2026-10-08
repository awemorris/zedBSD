/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Music's window (ws120-p009; music.h, play.h): a libkeiland application
 * with one window that shows the view (view.c), its menu (File: Quit;
 * Playback: Play or Pause, Next, Previous), and the view's input.  The
 * songs are ~/Music's (library.c) and the file named on the command line
 * (from Files), which plays at once.  The view's requests are carried out
 * here with the player (play.c); a song played to its end goes on to the
 * next.  Ctrl+Q quits.  What happens is logged on standard error as
 * "MUSIC" lines for the tests.
 *
 *   music [--width=N] [--height=N] [--timeout-s=N] [FILE]
 */

#include "play.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fonts, the window's first size, and the longest wait for input. */
#define MU_FONT			KEILAND_DATADIR "/fonts/keiland.ttf"
#define MU_FALLBACK_FONT	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"
#define MU_WIDTH		1040U
#define MU_HEIGHT		680U
#define MU_IDLE_MS		1000
#define MU_MOVING_MS		10

/* The most glass panels of a frame. */
#define MU_PANELS_MAX		4U

/* The key Q, which quits with Ctrl. */
#define MU_KEY_Q		16U

/* Previous goes to the start of the song instead after this far into it (s). */
#define MU_RESTART		3.0

/* The folder of the songs under the home, and how often it is looked at for a change (microseconds). */
#define MU_FOLDER		"Music"
#define MU_LOOK_US		5000000U

/*
 * The window's state: the application, the window and its input, the
 * frame (its pixels, size and canvas), the text and the style, the view,
 * the player, whether a frame is due, the window changed size, a widget
 * moves, the glass was decided, the last quarter of a second and the
 * last two seconds of the position told (for the frames and the log),
 * when the folder was last looked at for a change, a song to play at the
 * next round (-1 for none: the one after a song that could not go on), and
 * how many songs in a row could not go on (ws177-p021).
 */
struct mu_window {
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
	struct mu_view view;
	struct mu_player player;
	int dirty;
	int resized;
	int moving;
	int glass_decided;
	long quarter;
	long logged;
	uint64_t looked;
	long pending;
	size_t failed;
};

/* The window's menu. */
static const struct kl_menu_entry mu_menu[] = {
	{ 1U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "File", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 2U, 1U, KL_MENU_ITEM_NORMAL, "Quit Music", MU_ACTION_QUIT, KL_MENU_ROLE_QUIT, KL_MENU_CTRL, 'q' },
	{ 3U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Playback", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 4U, 3U, KL_MENU_ITEM_NORMAL, "Play or Pause", MU_ACTION_PLAY, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 5U, 3U, KL_MENU_ITEM_NORMAL, "Next", MU_ACTION_NEXT, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 6U, 3U, KL_MENU_ITEM_NORMAL, "Previous", MU_ACTION_PREVIOUS, KL_MENU_ROLE_NONE, 0U, 0U }
};

int main(int argc, char **argv);
static int mu_parse(int argc, char **argv, unsigned *width, unsigned *height, unsigned *timeout, const char **file);
static int mu_library_start(const char *file, long *song);
static void mu_file_notice(struct mu_window *music, int error, uint64_t now_us);
static int mu_loop(struct mu_window *music, unsigned timeout);
static void mu_input(struct mu_window *music, const struct kl_window_event *event);
static void mu_requests(struct mu_window *music, uint64_t now_us);
static void mu_play_song(struct mu_window *music, long song, uint64_t now_us);
static void mu_tell(struct mu_window *music, int error, uint64_t now_us);
static void mu_gone(struct mu_window *music, long song, uint64_t now_us);
static void mu_failed(struct mu_window *music, int failure, uint64_t now_us);
static void mu_advance(struct mu_window *music, long song);
static void mu_lost(struct mu_window *music, unsigned state, uint64_t now_us);
static void mu_toggle(struct mu_window *music, uint64_t now_us);
static void mu_step(struct mu_window *music, int step, uint64_t now_us);
static void mu_follow(struct mu_window *music, uint64_t now_us);
static void mu_look(struct mu_window *music, uint64_t now_us);
static void mu_reload(struct mu_window *music, uint64_t now_us);
static char *mu_copy(const char *text);
static int mu_resize(struct mu_window *music);
static void mu_draw(struct mu_window *music, uint64_t now_us);
static int mu_wait(const struct mu_window *music, uint64_t now_us);

/*
 * Runs Music.
 */
int
main(
	int argc,
	char **argv)
{
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	static struct mu_window music;
	const char *file;
	unsigned timeout;
	unsigned width;
	unsigned height;
	long song;
	int file_error;
	int status;
	int error;

	/* The command line. */
	status = mu_parse(argc, argv, &width, &height, &timeout, &file);
	if (status != 0) {
		fprintf(stderr, "usage: music [--width=N] [--height=N] [--timeout-s=N] [FILE]\n");
		return 2;
	}

	/* The fonts; without them the view shows no words. */
	error = kl_text_open(&music.text, MU_FONT, MU_FALLBACK_FONT);
	if (error != 0)
		mu_log("FONT missing error=%d", error);

	/* The songs, and the file named. */
	file_error = mu_library_start(file, &song);

	/* The view's state. */
	error = mu_view_init(&music.view);
	if (error != 0) {
		mu_log("FAILED operation=view error=%d", error);
		return 1;
	}

	/* Nothing waits to play; a file named that cannot be played says why (ws177-p021). */
	music.pending = -1;
	if (file != NULL && file_error != 0)
		mu_file_notice(&music, file_error, kl_clock_us());

	/* The sound (none is no failure: the view says why nothing plays), and the decoding add-in. */
	error = mu_player_init(&music.player);
	mu_log("AUDIO error=%d", error);
	if (error != 0)
		(void)snprintf(music.view.problem, sizeof(music.view.problem), "No sound: the sound service is not running.");
	error = vp_codec_load();
	if (error != 0)
		(void)snprintf(music.view.problem, sizeof(music.view.problem), "Playing needs libavcodec (the libavcodec package).");

	/* The application. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "music";
	music.app = kl_app_open(&app_options);
	if (music.app == NULL) {
		mu_log("FAILED operation=app error=%d", errno);
		mu_player_release(&music.player);
		mu_view_release(&music.view);
		return 1;
	}

	/* Its window. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Music";
	window_options.width = width;
	window_options.height = height;
	window_options.present = KL_PRESENT_VULKAN;
	music.window = kl_app_window_create(music.app, &window_options);
	if (music.window == NULL) {
		mu_log("FAILED operation=window error=%d", errno);
		kl_app_close(music.app);
		mu_player_release(&music.player);
		mu_view_release(&music.view);
		return 1;
	}

	/* The input of its frames. */
	music.ui = kl_ui_create();
	if (music.ui == NULL) {
		mu_log("FAILED operation=ui error=%d", errno);
		kl_app_close(music.app);
		mu_player_release(&music.player);
		mu_view_release(&music.view);
		return 1;
	}

	/* The menu and the style (opaque until the first frame finds whether the window can stand on glass). */
	(void)kl_window_set_menu(music.window, mu_menu, sizeof(mu_menu) / sizeof(mu_menu[0]));
	music.style.text = &music.text;
	music.style.theme = kl_theme_default();
	music.style.glass = 0;
	music.view.glass = 0;
	music.quarter = -1;
	music.logged = -1;

	/* The file named plays at once. */
	if (song >= 0)
		mu_play_song(&music, song, kl_clock_us());

	/* The loop until the window closes. */
	status = mu_loop(&music, timeout);

	/* Everything goes. */
	mu_player_release(&music.player);
	kl_ui_destroy(music.ui);
	if (music.canvas_made)
		kl_canvas_release(&music.canvas);
	free(music.pixels);
	kl_app_close(music.app);
	mu_view_release(&music.view);
	mu_library_release();
	kl_text_close(&music.text);

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
mu_log(
	const char *format,
	...)
{
	va_list arguments;

	/* The line. */
	va_start(arguments, format);
	fputs("MUSIC ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

/*
 * Writes a log line of Video Player's sound and decoding, which Music
 * shares, as a Music line.
 */
void
vp_log(
	const char *format,
	...)
{
	va_list arguments;

	/* The line. */
	va_start(arguments, format);
	fputs("MUSIC ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

/*
 * Reads the command line; nonzero when it cannot be read.
 */
static int
mu_parse(
	int argc,
	char **argv,
	unsigned *width,
	unsigned *height,
	unsigned *timeout,
	const char **file)
{
	int index;
	int same;

	/* The defaults. */
	*width = MU_WIDTH;
	*height = MU_HEIGHT;
	*timeout = 0U;
	*file = NULL;

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

		/* An option not known, or a second file. */
		if (argv[index][0] == '-' || *file != NULL)
			return -1;

		/* The file. */
		*file = argv[index];
	}

	/* A window needs a size. */
	if (*width == 0U || *height == 0U)
		return -1;

	/* Succeeded: the command line is read. */
	return 0;
}

/*
 * Reads the songs of the home's Music folder, and adds the file named (-1
 * for none, or one that is not a song).  Returns 0, or why the file named
 * is not a song (an errno value: ENOTSUP for one without sound).
 */
static int
mu_library_start(
	const char *file,
	long *song)
{
	char folder[1024];
	const char *home;
	size_t count;
	int error;

	/* The home's Music folder. */
	*song = -1;
	home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		home = "/tmp";
	(void)snprintf(folder, sizeof(folder), "%s/%s", home, MU_FOLDER);
	error = mu_library_scan(folder);
	(void)mu_songs(&count);
	mu_log("LIBRARY songs=%lu error=%d", (unsigned long)count, error);

	/* The file named. */
	if (file == NULL)
		return 0;
	error = mu_library_add_file(file, song);
	mu_log("FILE song=%ld error=%d", *song, error);
	if (error != 0)
		*song = -1;

	/* Succeeded: the file's outcome. */
	return error;
}

/* Says why a file named on the command line (from Files) cannot be played. */
static void
mu_file_notice(
	struct mu_window *music,
	int error,
	uint64_t now_us)
{
	/* No sound in it, not a song's kind of file, gone, or not readable. */
	if (error == ENOTSUP)
		mu_view_notice(&music->view, "This file has no sound Music can play.", now_us);
	else if (error == EINVAL)
		mu_view_notice(&music->view, "This file is not a song Music can play.", now_us);
	else if (error == ENOENT)
		mu_view_notice(&music->view, "The file is gone.", now_us);
	else
		mu_view_notice(&music->view, "The file could not be read.", now_us);
}

/*
 * Runs the window until it closes, Quit or the timeout; nonzero when
 * something failed.
 */
static int
mu_loop(
	struct mu_window *music,
	unsigned timeout)
{
	struct kl_app_event event;
	uint64_t started;
	uint64_t now;
	int status;
	int taken;
	int wait;

	/* The first frame. */
	status = mu_resize(music);
	if (status != 0)
		return -1;
	mu_log("READY width=%u height=%u", music->width, music->height);

	/* Each round: the input, the view's requests, the player, then a frame when something changed. */
	started = kl_clock_us();
	for (;;) {
		/* Waits for the compositor, or for the time something moves. */
		now = kl_clock_us();
		wait = mu_wait(music, now);
		status = kl_app_dispatch(music->app, wait);
		if (status != 0) {
			mu_log("DONE reason=disconnected");
			return 0;
		}

		/* The window's input and the actions of its menu. */
		for (;;) {
			taken = kl_app_take(music->app, &event);
			if (!taken)
				break;

			/* The desktop's appearance changed: the theme's colours are new. */
			if (event.kind == KL_APP_THEME) {
				music->dirty = 1;
				continue;
			}

			/* Another window's event is not this one's. */
			if (event.kind != KL_APP_WINDOW || event.window != music->window)
				continue;

			/* An action of the menu, or input. */
			if (event.input.kind == KL_WINDOW_ACTION) {
				mu_view_action(&music->view, event.input.code, kl_clock_us());
				music->dirty = 1;
			} else {
				mu_input(music, &event.input);
			}
		}

		/* What the view asked, and what the player did. */
		now = kl_clock_us();
		mu_requests(music, now);
		mu_follow(music, now);
		mu_look(music, now);

		/* The end: the window closed or Quit. */
		if (music->view.quit) {
			mu_log("DONE reason=close");
			return 0;
		}

		/* The timeout, when one was given. */
		if (timeout != 0U && now - started >= (uint64_t)timeout * 1000000U) {
			mu_log("DONE reason=timeout");
			return 0;
		}

		/* A new size. */
		if (music->resized) {
			music->resized = 0;
			status = mu_resize(music);
			if (status != 0)
				return -1;
		}

		/* The view's notice gone: drawn without it. */
		if (music->view.notice[0] != '\0' && now >= music->view.notice_until) {
			music->view.notice[0] = '\0';
			music->dirty = 1;
		}

		/* A frame. */
		mu_draw(music, now);
	}
}

/*
 * Gives one input of the window to the view's widgets, or takes it as the
 * window's: Ctrl+Q, a new size, the close.
 */
static void
mu_input(
	struct mu_window *music,
	const struct kl_window_event *event)
{
	int taken;

	/* Ctrl+Q quits. */
	if (event->kind == KL_WINDOW_KEY && event->pressed && (event->modifiers & KL_MOD_CTRL) != 0U && event->code == MU_KEY_Q) {
		music->view.quit = 1;
		return;
	}

	/* The widgets' input draws again. */
	taken = kl_ui_window_input(music->ui, event);
	if (taken) {
		music->dirty = 1;
		return;
	}

	/* The window's own. */
	if (event->kind == KL_WINDOW_RESIZE)
		music->resized = 1;
	else if (event->kind == KL_WINDOW_CLOSE)
		music->view.quit = 1;
}

/* Carries out the view's requests with the player. */
static void
mu_requests(
	struct mu_window *music,
	uint64_t now_us)
{
	struct mu_request request;
	unsigned steps;
	int step;
	int taken;

	/* Each one, in order; Next and Previous in a row are one step, their sum, so a song is opened once (ws177-p021). */
	step = 0;
	steps = 0U;
	for (;;) {
		taken = mu_view_take_request(&music->view, &request);
		if (!taken)
			break;
		music->dirty = 1;

		/* Next or Previous: added to the step. */
		if (request.action == MU_ACTION_NEXT || request.action == MU_ACTION_PREVIOUS) {
			if (request.action == MU_ACTION_NEXT)
				step++;
			else
				step--;
			steps++;
			continue;
		}

		/* Another request: the step before it first. */
		if (steps != 0U) {
			mu_log("STEP step=%d requests=%u", step, steps);
			mu_step(music, step, now_us);
			step = 0;
			steps = 0U;
		}

		/* The request. */
		switch (request.action) {
		case MU_ACTION_SONG:
			music->failed = 0U;
			mu_play_song(music, request.song, now_us);
			break;
		case MU_ACTION_PLAY:
			mu_toggle(music, now_us);
			break;
		case MU_ACTION_SEEK:
			mu_player_seek(&music->player, request.seconds);
			mu_log("SEEK song=%ld to_ms=%lld", request.song, (long long)(request.seconds * 1000.0));
			break;
		default:
			break;
		}
	}

	/* The step the round ended with. */
	if (steps != 0U) {
		mu_log("STEP step=%d requests=%u", step, steps);
		mu_step(music, step, now_us);
	}
}

/* Plays a song from its start; a song that cannot be played says why. */
static void
mu_play_song(
	struct mu_window *music,
	long song,
	uint64_t now_us)
{
	const struct mu_song *songs;
	size_t count;
	int error;

	/* A song of the collection. */
	songs = mu_songs(&count);
	if (song < 0 || (size_t)song >= count)
		return;

	/* Opened and playing. */
	error = mu_player_open(&music->player, songs[song].path);
	mu_log("PLAY song=%ld error=%d problem=%d", song, error, music->player.problem);
	music->view.playing = song;
	music->view.chosen = song;
	music->view.position = 0.0;
	music->view.length = (double)songs[song].duration_ms / 1000.0;
	music->logged = -1;
	if (error == 0) {
		music->view.length = music->player.duration;
		music->view.state = MU_PLAYING;
		return;
	}

	/* Not playing; a file that went makes the collection look again and goes on (ws177-p021). */
	music->view.state = MU_STOPPED;
	if (error == ENOENT) {
		mu_gone(music, song, now_us);
		return;
	}

	/* Why. */
	mu_tell(music, error, now_us);
}

/* Says why a song cannot be played. */
static void
mu_tell(
	struct mu_window *music,
	int error,
	uint64_t now_us)
{
	/* The add-in, the sound service, or the song. */
	if (music->player.problem == VP_CODEC_MISSING || music->player.problem == VP_CODEC_VERSION)
		mu_view_notice(&music->view, "Playing needs libavcodec (the libavcodec package).", now_us);
	else if (error == ENODEV)
		mu_view_notice(&music->view, "There is no sound: the sound service is not running.", now_us);
	else
		mu_view_notice(&music->view, "This song cannot be played.", now_us);
}

/*
 * A song whose file went: the folder is looked through again (the song
 * leaves the list), and the song after it plays at the next round.
 */
static void
mu_gone(
	struct mu_window *music,
	long song,
	uint64_t now_us)
{
	const struct mu_song *songs;
	size_t count;
	char *next;
	long found;

	/* The song after it, by its file (the indexes change). */
	next = NULL;
	songs = mu_songs(&count);
	if (song >= 0 && (size_t)song + 1U < count)
		next = mu_copy(songs[song + 1].path);

	/* Told, and the collection again. */
	mu_log("GONE song=%ld", song);
	mu_reload(music, now_us);
	mu_view_notice(&music->view, "This song's file is gone.", now_us);

	/* The song after it, at the next round. */
	found = -1;
	if (next != NULL)
		found = mu_library_find(next);
	free(next);
	mu_advance(music, found);
}

/* A song that stopped while it played (MU_FAIL_*): told, and the next one plays at the next round. */
static void
mu_failed(
	struct mu_window *music,
	int failure,
	uint64_t now_us)
{
	long next;

	/* Why. */
	mu_log("FAILED song=%ld reason=%d", music->view.playing, failure);
	if (failure == MU_FAIL_DECODE)
		mu_view_notice(&music->view, "This song could not be decoded.", now_us);
	else
		mu_view_notice(&music->view, "This song's file could not be read.", now_us);

	/* Stopped, and the next one. */
	music->view.state = MU_STOPPED;
	next = mu_library_next(music->view.playing, 1);
	mu_advance(music, next);
}

/*
 * Plays a song at the next round after one that could not go on, unless
 * as many songs as the collection has failed in a row (nothing then).
 */
static void
mu_advance(
	struct mu_window *music,
	long song)
{
	size_t count;

	/* None after it. */
	if (song < 0)
		return;

	/* Every song failed in a row: no more. */
	(void)mu_songs(&count);
	music->failed++;
	if (music->failed > count) {
		mu_log("STOP failed=%lu", (unsigned long)music->failed);
		return;
	}

	/* At the next round. */
	music->pending = song;
}

/*
 * The sound's stream was lost (its service went, WS191): it is opened
 * again and the song goes on from where it was, paused when it was; with
 * no service the song stops and says so.
 */
static void
mu_lost(
	struct mu_window *music,
	unsigned state,
	uint64_t now_us)
{
	const struct mu_song *songs;
	size_t count;
	double position;
	long song;
	int error;

	/* The song and where it was. */
	songs = mu_songs(&count);
	song = music->view.playing;
	if (song < 0 || (size_t)song >= count)
		return;
	position = mu_player_position(&music->player);
	mu_log("AUDIO lost song=%ld ms=%lld", song, (long long)(position * 1000.0));

	/* Opened again (the stream with it). */
	error = mu_player_open(&music->player, songs[song].path);
	mu_log("AUDIO reopened song=%ld error=%d", song, error);
	music->dirty = 1;
	if (error != 0) {
		music->view.state = MU_STOPPED;
		mu_tell(music, error, now_us);
		return;
	}

	/* From where it was, and paused when it was. */
	if (position > 0.0)
		mu_player_seek(&music->player, position);
	if (state == MU_PAUSED)
		mu_player_pause(&music->player);
}

/* Plays or pauses the song; a song that ended plays again from its start. */
static void
mu_toggle(
	struct mu_window *music,
	uint64_t now_us)
{
	unsigned state;
	int ended;

	/* The player's state. */
	state = mu_player_state(&music->player, &ended);
	if (state == MU_PLAYING) {
		mu_player_pause(&music->player);
		mu_log("PAUSE song=%ld", music->view.playing);
		return;
	}

	/* Paused: on from there. */
	if (state == MU_PAUSED) {
		mu_player_play(&music->player);
		mu_log("RESUME song=%ld", music->view.playing);
		return;
	}

	/* Stopped: the song shown again from its start. */
	mu_play_song(music, music->view.playing, now_us);
}

/*
 * Goes to the next song or the one before; Previous goes to the start of
 * the song instead when it has played a while.  Past the last, the player
 * stops.
 */
static void
mu_step(
	struct mu_window *music,
	int step,
	uint64_t now_us)
{
	double position;
	long song;

	/* Nothing playing. */
	if (music->view.playing < 0)
		return;

	/* Previous after a while: the start again (the first Previous of a step). */
	position = mu_player_position(&music->player);
	if (step < 0 && position > MU_RESTART) {
		step++;
		if (step == 0) {
			mu_player_seek(&music->player, 0.0);
			mu_log("SEEK song=%ld to_ms=0", music->view.playing);
			return;
		}
	}

	/* The neighbour in the collection's order. */
	song = mu_library_next(music->view.playing, step);
	if (song >= 0) {
		mu_play_song(music, song, now_us);
		return;
	}

	/* Past the end (or before the start): the song ends where it is. */
	if (step > 0) {
		mu_player_close(&music->player);
		music->view.state = MU_STOPPED;
		music->view.position = 0.0;
		mu_log("STOP song=%ld", music->view.playing);
	} else {
		mu_player_seek(&music->player, 0.0);
	}
}

/*
 * Tells the view what the player does: its state and position (a frame
 * each quarter of a second while it plays), and goes on to the next song
 * when one ended.
 */
static void
mu_follow(
	struct mu_window *music,
	uint64_t now_us)
{
	unsigned state;
	double position;
	long quarter;
	long pending;
	long two;
	int failure;
	int ended;
	int lost;

	/* A song waiting to play, after one that could not go on (ws177-p021). */
	if (music->pending >= 0) {
		pending = music->pending;
		music->pending = -1;
		mu_play_song(music, pending, now_us);
	}

	/* The state, and the end of a song: the next one, or the end of the list. */
	state = mu_player_state(&music->player, &ended);
	if (ended) {
		mu_log("ENDED song=%ld", music->view.playing);
		music->failed = 0U;
		mu_step(music, 1, now_us);
		state = mu_player_state(&music->player, &ended);
	}

	/* A song that could not go on: told, and the next one. */
	failure = mu_player_failure(&music->player);
	if (failure != 0) {
		mu_failed(music, failure, now_us);
		state = mu_player_state(&music->player, &ended);
	}

	/* The sound's service went while a song plays or is paused: the stream is opened again (WS191). */
	if (music->view.playing >= 0 && state != MU_STOPPED) {
		lost = vp_audio_lost(&music->player.audio);
		if (lost) {
			mu_lost(music, state, now_us);
			state = mu_player_state(&music->player, &ended);
		}
	}

	/* A new state, drawn and told. */
	if (state != music->view.state && music->view.playing >= 0) {
		music->view.state = state;
		music->dirty = 1;
		mu_log("STATE song=%ld state=%u", music->view.playing, state);
	}

	/* The position: a frame each quarter of a second; a song that played a second is no failure. */
	if (state == MU_STOPPED)
		return;
	position = mu_player_position(&music->player);
	music->view.position = position;
	if (position >= 1.0)
		music->failed = 0U;
	quarter = (long)(position * 4.0);
	if (quarter != music->quarter) {
		music->quarter = quarter;
		music->dirty = 1;
	}

	/* The log line each two seconds. */
	two = (long)(position / 2.0);
	if (two != music->logged) {
		music->logged = two;
		mu_log("POSITION song=%ld ms=%lld", music->view.playing, (long long)(position * 1000.0));
	}
}

/* Looks at the folder every few seconds; when it changed, the collection is made again. */
static void
mu_look(
	struct mu_window *music,
	uint64_t now_us)
{
	int changed;

	/* Not yet. */
	if (now_us - music->looked < MU_LOOK_US)
		return;
	music->looked = now_us;

	/* Changed: looked through again. */
	changed = mu_library_changed();
	if (changed)
		mu_reload(music, now_us);
}

/*
 * Looks through the folder again and keeps what plays, the song chosen and
 * the album chosen (by the songs' files and the album's name); a song
 * playing that went stops.
 */
static void
mu_reload(
	struct mu_window *music,
	uint64_t now_us)
{
	const struct mu_song *songs;
	struct mu_album *albums;
	size_t song_count;
	size_t album_count;
	size_t index;
	char *playing;
	char *chosen;
	char *title;
	char *artist;
	int same;
	int error;

	/* What is shown, by name. */
	playing = NULL;
	chosen = NULL;
	title = NULL;
	artist = NULL;
	songs = mu_songs(&song_count);
	albums = mu_albums(&album_count);
	if (music->view.playing >= 0 && (size_t)music->view.playing < song_count)
		playing = mu_copy(songs[music->view.playing].path);
	if (music->view.chosen >= 0 && (size_t)music->view.chosen < song_count)
		chosen = mu_copy(songs[music->view.chosen].path);
	if (music->view.album >= 0 && (size_t)music->view.album < album_count) {
		title = mu_copy(albums[music->view.album].title);
		artist = mu_copy(albums[music->view.album].artist);
	}

	/* The collection again; the pictures go with the albums. */
	mu_view_forget_pictures(&music->view);
	error = mu_library_rescan();
	songs = mu_songs(&song_count);
	albums = mu_albums(&album_count);
	mu_log("RESCAN songs=%lu albums=%lu error=%d", (unsigned long)song_count, (unsigned long)album_count, error);

	/* The songs, found again by their files. */
	music->view.playing = -1;
	music->view.chosen = -1;
	if (playing != NULL)
		music->view.playing = mu_library_find(playing);
	if (chosen != NULL)
		music->view.chosen = mu_library_find(chosen);

	/* A song playing that went: stopped. */
	if (playing != NULL && music->view.playing < 0) {
		mu_player_close(&music->player);
		music->view.state = MU_STOPPED;
		music->view.position = 0.0;
		mu_log("GONE song=-1");
		mu_view_notice(&music->view, "This song's file is gone.", now_us);
	}

	/* The album, found again by its name and artist (every song when it went). */
	music->view.album = -1;
	for (index = 0; title != NULL && artist != NULL && index < album_count; index++) {
		same = strcmp(albums[index].title, title) == 0 && strcmp(albums[index].artist, artist) == 0;
		if (same) {
			music->view.album = (long)index;
			break;
		}
	}

	/* The names, and a frame. */
	free(playing);
	free(chosen);
	free(title);
	free(artist);
	music->dirty = 1;
}

/* Copies a string (NULL without memory). */
static char *
mu_copy(
	const char *text)
{
	size_t length;
	char *copy;

	/* The bytes with the NUL. */
	length = strlen(text) + 1U;
	copy = malloc(length);
	if (copy == NULL)
		return NULL;
	memcpy(copy, text, length);

	/* Succeeded: the copy. */
	return copy;
}

/*
 * Remakes the presenter and the canvas at the window's size; nonzero when
 * it cannot.
 */
static int
mu_resize(
	struct mu_window *music)
{
	uint32_t *pixels;
	int see_through;
	int status;

	/* The presenter at the window's size. */
	status = kl_window_present_resize(music->window, &music->width, &music->height);
	if (status != 0) {
		mu_log("FAILED operation=present error=%d", status);
		return -1;
	}

	/* The compositor's glass, when the frames are blended by their alpha (decided at the first size). */
	if (!music->glass_decided) {
		music->glass_decided = 1;
		see_through = kl_window_see_through(music->window);
		if (see_through) {
			music->style.glass = 1;
			music->view.glass = 1;
		}

		/* The log line the tests read. */
		mu_log("GLASS see_through=%d", see_through);
	}

	/* A frame's pixels of its size. */
	pixels = malloc((size_t)music->width * (size_t)music->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* The canvas on them, in place of the old one. */
	if (music->canvas_made)
		kl_canvas_release(&music->canvas);
	music->canvas_made = 0;
	free(music->pixels);
	music->pixels = pixels;
	status = kl_canvas_init(&music->canvas, music->pixels, (size_t)music->width, (int)music->width, (int)music->height);
	if (status != 0)
		return -1;

	/* Succeeded: drawn again at the new size. */
	music->canvas_made = 1;
	music->style.canvas = &music->canvas;
	music->dirty = 1;
	return 0;
}

/*
 * Draws and shows a frame when something changed or moves, then gives the
 * view the keys no widget took.
 */
static void
mu_draw(
	struct mu_window *music,
	uint64_t now_us)
{
	struct kl_glass_panel panels[MU_PANELS_MAX];
	struct kl_event event;
	size_t count;
	int status;
	int error;
	int taken;

	/* Nothing changed and nothing moves: no frame. */
	if (!music->dirty && !music->moving && !music->view.seek_pending)
		return;

	/* The view. */
	music->dirty = 0;
	kl_ui_begin(music->ui, now_us);
	mu_view_draw(&music->view, music->ui, &music->style, (int)music->width, (int)music->height, now_us);
	music->moving = kl_ui_end(music->ui, now_us);
	kl_ui_window_text(music->ui, music->window);

	/* The glass's panels for the frame; a compositor without glass leaves the window opaque from the next one. */
	if (music->view.glass) {
		count = mu_view_panels(&music->view, (int)music->width, (int)music->height, panels, MU_PANELS_MAX);
		error = kl_window_set_glass(music->window, panels, count);
		if (error != 0) {
			mu_log("GLASS failed error=%d", error);
			music->view.glass = 0;
			music->style.glass = 0;
			music->dirty = 1;
		}
	}

	/* The frame shown. */
	status = kl_window_present(music->window, music->pixels, (size_t)music->width);
	if (status == EAGAIN)
		music->resized = 1;

	/* The keys no widget took are the view's; it draws again. */
	for (;;) {
		taken = kl_ui_take(music->ui, &event);
		if (!taken)
			break;
		if (event.kind == KL_EVENT_KEY) {
			mu_view_key(&music->view, event.code, event.modifiers, now_us);
			music->dirty = 1;
		}
	}
}

/*
 * Reports how long the loop may wait for input (ms): no time while a frame
 * is due, a frame's time while a widget moves, the view's own time, or a
 * second.
 */
static int
mu_wait(
	const struct mu_window *music,
	uint64_t now_us)
{
	int wait;

	/* A frame due now, or a request waiting. */
	if (music->dirty || music->view.request_count != 0U)
		return 0;

	/* A widget moving. */
	if (music->moving)
		return MU_MOVING_MS;

	/* The view's time, or a second. */
	wait = mu_view_wait(&music->view, now_us);
	if (wait < 0 || wait > MU_IDLE_MS)
		wait = MU_IDLE_MS;
	return wait;
}
