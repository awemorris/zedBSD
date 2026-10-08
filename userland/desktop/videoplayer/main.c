/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Video Player's window (WS122 p002; videoplayer.h): a libkeiland
 * application with one window, its menu (File: Open, Close, Quit; Playback:
 * Play or Pause, Stop, Back and Forward 10 s; View: Full Screen) and, over
 * the bottom of the picture, a bar of controls -- play or pause, the time,
 * and the position to drag -- shown while paused and for a few seconds
 * after the pointer moves.  The picture is fitted to the window (black
 * bands around it) by libswscale straight into the frame.
 *
 * Keys: Space plays or pauses, Left and Right go 10 s back and forward,
 * F the full screen, Esc leaves it, Ctrl+O opens, Ctrl+W closes, Ctrl+Q
 * quits.  What happens is logged on standard error as "VIDEOPLAYER" lines
 * for the tests.
 *
 *   videoplayer [--width=N] [--height=N] [--timeout-s=N] [FILE]
 */

#include "videoplayer.h"

#include <keiland/keiland.h>


#include "userland/desktop/paths.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fonts, the window's size until the compositor gives one, and the longest wait while nothing moves (ms). */
#define VP_FONT			KEILAND_DATADIR "/fonts/keiland.ttf"
#define VP_FALLBACK_FONT	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"
#define VP_WIDTH		960U
#define VP_HEIGHT		600U
#define VP_IDLE_MS		1000

/* The bar of controls: its height, how long it stays after the pointer moves (us), and its widgets' ids. */
#define VP_BAR_HEIGHT		56
#define VP_BAR_US		3000000U

/* In full screen the bar shows for less after the pointer moved (ws122-p005a, us). */
#define VP_BAR_FULL_US		2000000U

/* A double click on the picture toggles full screen: the second press this soon (us) and this near (pixels). */
#define VP_DOUBLE_US		400000U
#define VP_DOUBLE_NEAR		8
#define VP_ID_PLAY		1U
#define VP_ID_POSITION		2U

/* How far Left and Right go (s), and the least time between two seeks while the position is dragged (us). */
#define VP_STEP			10.0
#define VP_SEEK_US		150000U

/* The evdev codes of the keys. */
#define VP_KEY_ESC		1U
#define VP_KEY_Q		16U
#define VP_KEY_W		17U
#define VP_KEY_O		24U
#define VP_KEY_ENTER		28U
#define VP_KEY_F		33U
#define VP_KEY_F11		87U
#define VP_KEY_SPACE		57U
#define VP_KEY_LEFT		105U
#define VP_KEY_RIGHT		106U

/* The actions of the menu. */
#define VP_ACTION_OPEN		1U
#define VP_ACTION_CLOSE		2U
#define VP_ACTION_QUIT		3U
#define VP_ACTION_PLAY		4U
#define VP_ACTION_STOP		5U
#define VP_ACTION_BACK		6U
#define VP_ACTION_FORWARD	7U
#define VP_ACTION_FULLSCREEN	8U

/* The player: the application, its window and frame, the controls, the media and what is shown. */
struct vp_player {
	/* The application, the window and its input. */
	struct kl_app *app;
	struct kl_window *window;
	struct kl_ui *ui;
	struct kl_file_chooser *chooser;

	/* The frame: its pixels and size, the canvas, the text and the style. */
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	uint64_t resized_us;
	uint32_t presented_width;
	uint32_t presented_height;
	int present_error;
	struct kl_canvas canvas;
	int canvas_made;
	struct kl_text text;
	struct kl_style style;

	/* The sound and the media. */
	struct vp_audio audio;
	struct vp_media media;

	/* The picture shown, its time, the converter that fits it, and how many were shown. */
	struct media_frame *picture;
	double picture_time;
	struct media_scaler *scaler;

	/* What the window says instead of a picture when a file could not be played ("" for nothing). */
	char notice[160];
	unsigned shown;
	int need_picture;

	/* The bar: shown until a time; the position while dragged and the last seek it asked. */
	uint64_t bar_until;
	double position;
	uint64_t seek_at;

	/* The last press on the picture (when and where), which a second one soon after makes a double click. */
	uint64_t click_us;
	int32_t click_x;
	int32_t click_y;

	/* A frame is due, the window changed size, the run ends. */
	int dirty;
	int resized;
	int quit;
};

/* The window's menu. */
static const struct kl_menu_entry vp_menu[] = {
	{ 1U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "File", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 2U, 1U, KL_MENU_ITEM_NORMAL, "Open...", VP_ACTION_OPEN, KL_MENU_ROLE_OPEN, KL_MENU_CTRL, 'o' },
	{ 3U, 1U, KL_MENU_ITEM_NORMAL, "Close", VP_ACTION_CLOSE, KL_MENU_ROLE_CLOSE, KL_MENU_CTRL, 'w' },
	{ 4U, 1U, KL_MENU_ITEM_NORMAL, "Quit Video Player", VP_ACTION_QUIT, KL_MENU_ROLE_QUIT, KL_MENU_CTRL, 'q' },
	{ 5U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Playback", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 6U, 5U, KL_MENU_ITEM_NORMAL, "Play or Pause", VP_ACTION_PLAY, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 7U, 5U, KL_MENU_ITEM_NORMAL, "Stop", VP_ACTION_STOP, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 8U, 5U, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 9U, 5U, KL_MENU_ITEM_NORMAL, "Back 10 Seconds", VP_ACTION_BACK, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 10U, 5U, KL_MENU_ITEM_NORMAL, "Forward 10 Seconds", VP_ACTION_FORWARD, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 11U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "View", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 12U, 11U, KL_MENU_ITEM_CHECKBOX, "Full Screen", VP_ACTION_FULLSCREEN, KL_MENU_ROLE_FULLSCREEN, 0U, 0U }
};

/* The files the chooser offers. */
static const struct kl_file_filter vp_filters[] = {
	{ "Videos", "mp4 m4v mkv webm mov avi mpg mpeg ts ogv" },
	{ "All Files", NULL }
};

static int vp_parse(int argc, char **argv, unsigned *width, unsigned *height, unsigned *timeout, const char **file);
static void vp_media_log(void *context, const char *line);
static int vp_loop(struct vp_player *player, unsigned timeout);
static void vp_input(struct vp_player *player, const struct kl_window_event *event);
static void vp_action(struct vp_player *player, uint32_t action);
static void vp_open(struct vp_player *player, const char *path);
static void vp_toggle(struct vp_player *player);
static void vp_states(struct vp_player *player);
static void vp_choose(struct vp_player *player);
static void vp_chosen(void *data, struct kl_file_chooser *chooser, unsigned result, const char *path, size_t filter);
static int vp_resize(struct vp_player *player);
static void vp_draw(struct vp_player *player, uint64_t now_us);
static void vp_draw_picture(struct vp_player *player);
static void vp_notice(struct vp_player *player, int error, int problem);
static void vp_draw_bar(struct vp_player *player, uint64_t now_us);
static void vp_time_text(double seconds, char *text, size_t size);
static uint64_t vp_bar_time(struct vp_player *player);
static int vp_double_click(struct vp_player *player, const struct kl_window_event *event, uint64_t now);
static int vp_wait(struct vp_player *player, uint64_t now_us);

/* The chooser tells its answer here. */
static const struct kl_file_chooser_listener vp_chooser_listener = {
	vp_chosen
};

/*
 * Runs the player.
 */
int
main(
	int argc,
	char **argv)
{
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	static struct vp_player player;
	const char *file;
	unsigned timeout;
	unsigned width;
	unsigned height;
	int status;
	int error;

	/* libmedia's log lines (the reader's and the decoders') among the player's. */
	media_set_log(vp_media_log, NULL);

	/* The command line. */
	status = vp_parse(argc, argv, &width, &height, &timeout, &file);
	if (status != 0) {
		fprintf(stderr, "usage: videoplayer [--width=N] [--height=N] [--timeout-s=N] [FILE]\n");
		return 2;
	}

	/* The fonts; without them the controls show no words. */
	error = kl_text_open(&player.text, VP_FONT, VP_FALLBACK_FONT);
	if (error != 0)
		vp_log("FONT missing error=%d", error);

	/* The application and its window. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "videoplayer";
	player.app = kl_app_open(&app_options);
	if (player.app == NULL) {
		vp_log("FAILED operation=app error=%d", errno);
		return 1;
	}

	/* Its window, and the input of its frames. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Video Player";
	window_options.width = width;
	window_options.height = height;
	window_options.present = KL_PRESENT_VULKAN;
	player.window = kl_app_window_create(player.app, &window_options);
	player.ui = kl_ui_create();
	if (player.window == NULL || player.ui == NULL) {
		vp_log("FAILED operation=window error=%d", errno);
		kl_app_close(player.app);
		return 1;
	}

	/* The window shows a video: in full screen the compositor may show it without composing (game mode, ws122-p005b). */
	(void)kl_window_set_content_type(player.window, KL_CONTENT_VIDEO);

	/* The menu, the style, the sound (none is no failure) and the media. */
	(void)kl_window_set_menu(player.window, vp_menu, sizeof(vp_menu) / sizeof(vp_menu[0]));
	player.style.text = &player.text;
	player.style.theme = kl_theme_default();
	player.style.glass = 0;
	error = vp_audio_open(&player.audio);
	vp_log("AUDIO error=%d", error);
	vp_media_init(&player.media, &player.audio);
	vp_states(&player);

	/* The file named, and the loop until the window closes. */
	if (file != NULL)
		vp_open(&player, file);
	status = vp_loop(&player, timeout);

	/* Everything goes. */
	vp_media_close(&player.media);
	vp_audio_close(&player.audio);
	media_frame_free(&player.picture);
	media_scaler_free(player.scaler);
	kl_file_chooser_destroy(player.chooser);
	kl_ui_destroy(player.ui);
	if (player.canvas_made)
		kl_canvas_release(&player.canvas);
	free(player.pixels);
	kl_app_close(player.app);
	kl_text_close(&player.text);
	if (status != 0)
		return 1;
	return 0;
}

/*
 * Writes a log line for the tests on standard error.
 */
void
vp_log(
	const char *format,
	...)
{
	va_list arguments;

	/* The line. */
	va_start(arguments, format);
	fputs("VIDEOPLAYER ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

/* Writes one of libmedia's log lines as a Video Player line. */
static void
vp_media_log(
	void *context,
	const char *line)
{
	(void)context;

	/* The line, after the player's word. */
	fprintf(stderr, "VIDEOPLAYER %s\n", line);
}

/* Reads the command line; nonzero when it cannot be read. */
static int
vp_parse(
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
	*width = VP_WIDTH;
	*height = VP_HEIGHT;
	*timeout = 0U;
	*file = NULL;

	/* Each argument. */
	for (index = 1; index < argc; index++) {
		/* The size and the timeout. */
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

		/* An option not known, or the file. */
		if (argv[index][0] == '-')
			return -1;
		*file = argv[index];
	}

	/* A window needs a size. */
	if (*width == 0U || *height == 0U)
		return -1;
	return 0;
}

/* Runs the window until it closes, Quit or the timeout; nonzero when something failed. */
static int
vp_loop(
	struct vp_player *player,
	unsigned timeout)
{
	struct kl_app_event event;
	uint64_t started;
	uint64_t now;
	int status;
	int taken;
	int wait;

	/* The first frame. */
	status = vp_resize(player);
	if (status != 0)
		return -1;
	vp_log("READY width=%u height=%u", player->width, player->height);

	/* Each round: the input, then a frame when something changed or a picture is due. */
	started = kl_clock_us();
	for (;;) {
		/* Waits for the compositor or for the next picture's time. */
		now = kl_clock_us();
		wait = vp_wait(player, now);
		status = kl_app_dispatch(player->app, wait);
		if (status != 0) {
			vp_log("DONE reason=disconnected");
			return 0;
		}

		/* The window's input and the actions of its menu. */
		for (;;) {
			taken = kl_app_take(player->app, &event);
			if (!taken)
				break;

			/* The desktop's appearance changed: the theme's colours are new, the window is drawn again (ws089-p017). */
			if (event.kind == KL_APP_THEME) {
				player->dirty = 1;
				continue;
			}

			/* Another window's event is not this one's. */
			if (event.kind != KL_APP_WINDOW || event.window != player->window)
				continue;
			if (event.input.kind == KL_WINDOW_ACTION)
				vp_action(player, event.input.code);
			else
				vp_input(player, &event.input);
		}

		/* The end: the window closed, Quit, or the timeout. */
		now = kl_clock_us();
		if (player->quit) {
			vp_log("DONE reason=close shown=%u", player->shown);
			return 0;
		}

		/* The timeout, when one was given. */
		if (timeout != 0U && now - started >= (uint64_t)timeout * 1000000U) {
			vp_log("DONE reason=timeout shown=%u", player->shown);
			return 0;
		}

		/* A new size. */
		if (player->resized) {
			player->resized = 0;
			status = vp_resize(player);
			if (status != 0)
				return -1;
		}

		/* A frame: the picture due, the bar. */
		vp_draw(player, now);
	}
}

/* Gives one input of the window to the controls, or takes it as a key of the player. */
static void
vp_input(
	struct vp_player *player,
	const struct kl_window_event *event)
{
	uint64_t now;
	int doubled;
	int ctrl;
	int alt;
	int full;

	/* Any input may change the bar; the pointer brings it back for a while (less in full screen). */
	now = kl_clock_us();
	full = kl_window_fullscreen(player->window);
	player->dirty = 1;
	switch (event->kind) {
	case KL_WINDOW_MOTION:
		player->bar_until = now + vp_bar_time(player);
		(void)kl_ui_pointer_motion(player->ui, event->x, event->y);
		break;
	case KL_WINDOW_LEAVE:
		(void)kl_ui_pointer_leave(player->ui);
		break;
	case KL_WINDOW_BUTTON:
		player->bar_until = now + vp_bar_time(player);
		(void)kl_ui_pointer_motion(player->ui, event->x, event->y);
		if (event->code == KL_BUTTON_LEFT)
			(void)kl_ui_pointer_button(player->ui, event->pressed, event->arrival_us);

		/* A double click on the picture toggles full screen (ws122-p005a). */
		doubled = vp_double_click(player, event, now);
		if (doubled) {
			vp_log("FULLSCREEN toggle via=double-click");
			vp_action(player, VP_ACTION_FULLSCREEN);
		}

		/* The button is done. */
		break;
	case KL_WINDOW_TOUCH_DOWN:
		player->bar_until = now + vp_bar_time(player);
		(void)kl_ui_touch_down(player->ui, event->id, event->time_us, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_MOTION:
		(void)kl_ui_touch_motion(player->ui, event->id, event->time_us, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_UP:
		(void)kl_ui_touch_up(player->ui, event->id, event->time_us, event->arrival_us);
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		(void)kl_ui_touch_cancel(player->ui, event->arrival_us);
		break;
	case KL_WINDOW_KEY:
		/* Only presses are the player's keys. */
		if (!event->pressed)
			break;
		ctrl = (event->modifiers & KL_MOD_CTRL) != 0U;
		alt = (event->modifiers & KL_MOD_ALT) != 0U;
		if (event->code == VP_KEY_F11) {
			/* F11 and Alt+Enter toggle full screen, as F does (ws122-p005a, the 2026-10-06 user request). */
			vp_log("FULLSCREEN toggle via=f11");
			vp_action(player, VP_ACTION_FULLSCREEN);
		} else if (alt && event->code == VP_KEY_ENTER) {
			vp_log("FULLSCREEN toggle via=alt-enter");
			vp_action(player, VP_ACTION_FULLSCREEN);
		} else if (ctrl && event->code == VP_KEY_O)
			vp_action(player, VP_ACTION_OPEN);
		else if (ctrl && event->code == VP_KEY_W)
			vp_action(player, VP_ACTION_CLOSE);
		else if (ctrl && event->code == VP_KEY_Q)
			vp_action(player, VP_ACTION_QUIT);
		else if (event->code == VP_KEY_SPACE)
			vp_action(player, VP_ACTION_PLAY);
		else if (event->code == VP_KEY_LEFT)
			vp_action(player, VP_ACTION_BACK);
		else if (event->code == VP_KEY_RIGHT)
			vp_action(player, VP_ACTION_FORWARD);
		else if (event->code == VP_KEY_F)
			vp_action(player, VP_ACTION_FULLSCREEN);
		else if (event->code == VP_KEY_ESC && full)
			vp_action(player, VP_ACTION_FULLSCREEN);
		break;
	case KL_WINDOW_RESIZE:
		player->resized = 1;
		break;
	case KL_WINDOW_CLOSE:
		player->quit = 1;
		break;
	default:
		break;
	}
}

/* Carries out an action of the menu, a key or a control. */
static void
vp_action(
	struct vp_player *player,
	uint32_t action)
{
	double clock;
	int full;

	/* The log line the tests read. */
	vp_log("ACTION action=%u", (unsigned)action);
	player->dirty = 1;
	player->bar_until = kl_clock_us() + vp_bar_time(player);
	switch (action) {
	case VP_ACTION_OPEN:
		vp_choose(player);
		break;
	case VP_ACTION_CLOSE:
		vp_media_close(&player->media);
		media_frame_free(&player->picture);
		player->notice[0] = '\0';
		vp_log("CLOSE");
		break;
	case VP_ACTION_QUIT:
		player->quit = 1;
		break;
	case VP_ACTION_PLAY:
		vp_toggle(player);
		break;
	case VP_ACTION_STOP:
		/* Paused at the start. */
		vp_media_pause(&player->media);
		vp_media_seek(&player->media, 0.0);
		player->need_picture = 1;
		vp_log("STOP");
		break;
	case VP_ACTION_BACK:
	case VP_ACTION_FORWARD:
		/* 10 s from where the clock is. */
		clock = vp_media_clock(&player->media);
		if (action == VP_ACTION_BACK)
			clock -= VP_STEP;
		else
			clock += VP_STEP;
		vp_media_seek(&player->media, clock);
		player->need_picture = 1;
		vp_log("SEEK to_ms=%lld", (long long)(clock * 1000.0));
		break;
	case VP_ACTION_FULLSCREEN:
		full = kl_window_fullscreen(player->window);
		kl_window_set_fullscreen(player->window, !full);
		vp_log("FULLSCREEN on=%d", !full);
		break;
	default:
		break;
	}

	/* The menu follows. */
	vp_states(player);
}

/* Opens a file, paused at its start and then playing. */
static void
vp_open(
	struct vp_player *player,
	const char *path)
{
	int error;

	/* The media; the picture shown before goes. */
	media_frame_free(&player->picture);
	player->notice[0] = '\0';
	error = vp_media_open(&player->media, path);
	vp_log("OPENED path=%s error=%d codec=%d", path, error, player->media.codec_problem);
	if (error != 0) {
		vp_notice(player, error, player->media.codec_problem);
		return;
	}

	/* The first picture as soon as it is decoded, and playing. */
	player->need_picture = 1;
	player->shown = 0;
	vp_media_play(&player->media);
	vp_log("PLAY shown=0 time_ms=0");
	vp_states(player);
}

/* Plays or pauses. */
static void
vp_toggle(
	struct vp_player *player)
{
	unsigned state;

	/* What it does now. */
	(void)pthread_mutex_lock(&player->media.lock);
	state = player->media.state;
	(void)pthread_mutex_unlock(&player->media.lock);

	/* The other. */
	if (state == VP_PLAYING) {
		vp_media_pause(&player->media);
		vp_log("PAUSE shown=%u time_ms=%lld", player->shown, (long long)(vp_media_clock(&player->media) * 1000.0));
	} else if (state != VP_EMPTY) {
		vp_log("PLAY shown=%u time_ms=%lld", player->shown, (long long)(vp_media_clock(&player->media) * 1000.0));
		vp_media_play(&player->media);
	}
}

/* Tells the menu what can be done now. */
static void
vp_states(
	struct vp_player *player)
{
	unsigned disabled;
	unsigned state;

	/* What is open. */
	(void)pthread_mutex_lock(&player->media.lock);
	state = player->media.state;
	(void)pthread_mutex_unlock(&player->media.lock);

	/* The playback's actions only with something open. */
	disabled = 0;
	if (state == VP_EMPTY)
		disabled = KL_ACTION_DISABLED;
	(void)kl_window_set_action_state(player->window, VP_ACTION_CLOSE, disabled);
	(void)kl_window_set_action_state(player->window, VP_ACTION_PLAY, disabled);
	(void)kl_window_set_action_state(player->window, VP_ACTION_STOP, disabled);
	(void)kl_window_set_action_state(player->window, VP_ACTION_BACK, disabled);
	(void)kl_window_set_action_state(player->window, VP_ACTION_FORWARD, disabled);
}

/* Opens the file chooser over the window (one at a time). */
static void
vp_choose(
	struct vp_player *player)
{
	struct kl_file_chooser_options options;

	/* One chooser at a time. */
	if (player->chooser != NULL)
		return;

	/* Open a video. */
	memset(&options, 0, sizeof(options));
	options.mode = KL_FILE_CHOOSER_OPEN;
	options.title = "Open Video";
	options.application = "videoplayer";
	options.filters = vp_filters;
	options.filter_count = sizeof(vp_filters) / sizeof(vp_filters[0]);
	player->chooser = kl_file_chooser_open(kl_app_display(player->app), kl_window_toplevel(player->window), &options, &vp_chooser_listener, player);
	if (player->chooser == NULL)
		vp_log("CHOOSER failed error=%d", errno);
}

/* The chooser's answer: the file chosen is opened. */
static void
vp_chosen(
	void *data,
	struct kl_file_chooser *chooser,
	unsigned result,
	const char *path,
	size_t filter)
{
	struct vp_player *player;

	/* The chooser goes. */
	(void)filter;
	player = data;
	kl_file_chooser_destroy(chooser);
	player->chooser = NULL;

	/* The file, when one was chosen. */
	if (result == KL_FILE_CHOOSER_CHOSEN)
		vp_open(player, path);
	player->dirty = 1;
}

/* Remakes the presenter and the canvas at the window's size; nonzero when it cannot. */
static int
vp_resize(
	struct vp_player *player)
{
	uint32_t *pixels;
	int status;

	/* The presenter at the window's size. */
	status = kl_window_present_resize(player->window, &player->width, &player->height);
	if (status != 0) {
		vp_log("FAILED operation=present error=%d", status);
		return -1;
	}

	/* A canvas of its size. */
	pixels = malloc((size_t)player->width * (size_t)player->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;
	if (player->canvas_made)
		kl_canvas_release(&player->canvas);
	player->canvas_made = 0;
	free(player->pixels);
	player->pixels = pixels;
	status = kl_canvas_init(&player->canvas, player->pixels, (size_t)player->width, (int)player->width, (int)player->height);
	if (status != 0)
		return -1;

	/* Succeeded: drawn again at the new size, from now. */
	player->resized_us = kl_clock_us();
	player->canvas_made = 1;
	player->style.canvas = &player->canvas;
	player->dirty = 1;
	return 0;
}

/* Draws and shows a frame when the picture changed, the bar moves or something else changed. */
static void
vp_draw(
	struct vp_player *player,
	uint64_t now_us)
{
	struct media_frame *picture;
	uint64_t shown_us;
	uint64_t after_ms;
	double clock;
	double time;
	double next;
	int status;

	/* The picture whose time has come (the first one at once after an open or a seek). */
	clock = vp_media_clock(&player->media);
	picture = vp_media_take(&player->media, clock, &time, &next);
	if (picture == NULL && player->need_picture && next >= 0.0)
		picture = vp_media_take(&player->media, next, &time, &next);
	if (picture != NULL) {
		media_frame_free(&player->picture);
		player->picture = picture;
		player->picture_time = time;
		player->need_picture = 0;
		player->shown++;
		player->dirty = 1;
		if (player->shown == 1U || player->shown % 100U == 0U)
			vp_log("FRAMES shown=%u time_ms=%lld", player->shown, (long long)(time * 1000.0));
	}

	/* The end of the file: the last picture shown and nothing left. */
	(void)pthread_mutex_lock(&player->media.lock);
	if (player->media.state == VP_PLAYING && player->media.eof && player->media.picture_count == 0U && clock >= player->picture_time) {
		player->media.state = VP_ENDED;
		vp_log("ENDED shown=%u", player->shown);
		player->dirty = 1;
	}

	/* The media is the thread's again. */
	(void)pthread_mutex_unlock(&player->media.lock);

	/* Nothing changed: no frame. */
	if (!player->dirty && now_us > player->bar_until + VP_BAR_US)
		return;
	player->dirty = 0;

	/* The picture, the bar, and the frame shown. */
	kl_ui_begin(player->ui, now_us);
	vp_draw_picture(player);
	vp_draw_bar(player, now_us);
	(void)kl_ui_end(player->ui, now_us);
	status = kl_window_present(player->window, player->pixels, (size_t)player->width);
	if (status == EAGAIN)
		player->resized = 1;

	/* A frame that could not be shown is logged once for each new error (it is drawn again when something changes). */
	if (status != 0 && status != EAGAIN && status != player->present_error)
		vp_log("FAILED operation=frame error=%d", status);
	if (status != EAGAIN)
		player->present_error = status;

	/* The first frame shown at a new size, and how long after the size came (ws122-p005a, T1-235; the loop's time is from before the resize, T1-237). */
	if (status == 0 && (player->width != player->presented_width || player->height != player->presented_height)) {
		player->presented_width = player->width;
		player->presented_height = player->height;
		shown_us = kl_clock_us();
		after_ms = 0U;
		if (shown_us > player->resized_us)
			after_ms = (shown_us - player->resized_us) / 1000U;
		vp_log("PRESENTED width=%u height=%u after_ms=%llu", player->width, player->height, (unsigned long long)after_ms);
	}
}

/* Draws the picture fitted to the window, black around it (or black alone). */
static void
vp_draw_picture(
	struct vp_player *player)
{
	struct kl_rect whole;
	struct kl_text_line line;
	struct media_frame *picture;
	double aspect;
	int picture_width;
	int picture_height;
	int text_width;
	int width;
	int height;
	int x;
	int y;
	int status;

	/* Black. */
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)player->width;
	whole.height = (int)player->height;
	kl_canvas_fill(&player->canvas, &whole, KL_RGB(0x000000));
	picture = player->picture;

	/* Without a picture, what the window has to say, in the middle. */
	if (picture == NULL) {
		if (player->notice[0] == '\0')
			return;
		kl_text_metrics(&player->text, 16U, &line);
		text_width = kl_text_width(&player->text, player->notice, strlen(player->notice), 16U, 0);
		(void)kl_text_draw(&player->text, &player->canvas, ((int)player->width - text_width) / 2, (int)player->height / 2 + line.ascent / 2,
		    player->notice, strlen(player->notice), 16U, 0, KL_RGB(0xe0e0e0));
		return;
	}

	/* The picture's shape (square samples: the add-in reads no aspect field), fitted to the window. */
	media_frame_size(picture, &picture_width, &picture_height);
	if (picture_width <= 0 || picture_height <= 0)
		return;
	aspect = (double)picture_width / (double)picture_height;
	width = (int)player->width;
	height = (int)((double)width / aspect);
	if (height > (int)player->height) {
		height = (int)player->height;
		width = (int)((double)height * aspect);
	}

	/* Too small to show, and where it goes. */
	if (width < 2 || height < 2)
		return;
	x = ((int)player->width - width) / 2;
	y = ((int)player->height - height) / 2;

	/* Scaled straight into the frame (BGRA is the canvas's 0xAARRGGBB, opaque); the scaler is remade when the sizes change. */
	status = media_frame_scale(picture, &player->scaler, player->pixels + (size_t)y * player->width + (size_t)x,
	    player->width * sizeof(uint32_t), width, height);
	if (status != 0)
		vp_log("SCALE failed width=%d height=%d", picture_width, picture_height);
}

/* Says in the window why a file could not be played. */
static void
vp_notice(
	struct vp_player *player,
	int error,
	int problem)
{
	/* The add-in's problem, or the file's. */
	switch (problem) {
	case MEDIA_PROBLEM_MISSING:
		(void)snprintf(player->notice, sizeof(player->notice), "Playing video needs FFmpeg's libavcodec, which is not installed.");
		break;
	case MEDIA_PROBLEM_VERSION:
		(void)snprintf(player->notice, sizeof(player->notice), "This version of libavcodec is not supported.");
		break;
	case MEDIA_PROBLEM_FORMAT:
		(void)snprintf(player->notice, sizeof(player->notice), "The video's format is not supported.");
		break;
	default:
		(void)snprintf(player->notice, sizeof(player->notice), "The file could not be opened (error %d).", error);
		break;
	}

	/* The notice is drawn with the next frame. */
	vp_log("NOTICE problem=%d text=%s", problem, player->notice);
	player->dirty = 1;
}

/* Draws the bar of controls while it shows: play or pause, the times, the position. */
static void
vp_draw_bar(
	struct vp_player *player,
	uint64_t now_us)
{
	struct kl_text_line line;
	struct kl_rect bar;
	struct kl_rect button;
	struct kl_rect slider;
	char now_text[32];
	char length_text[32];
	char times[80];
	const char *label;
	double clock;
	double duration;
	unsigned state;
	int pressed;
	int changed;
	int width;

	/* Shown while paused or ended, and for a while after the pointer moved. */
	(void)pthread_mutex_lock(&player->media.lock);
	state = player->media.state;
	duration = player->media.duration;
	(void)pthread_mutex_unlock(&player->media.lock);
	if (state == VP_PLAYING && now_us > player->bar_until)
		return;

	/* The bar, dark over the picture. */
	bar.x = 0;
	bar.y = (int)player->height - VP_BAR_HEIGHT;
	bar.width = (int)player->width;
	bar.height = VP_BAR_HEIGHT;
	kl_canvas_fill(&player->canvas, &bar, KL_RGBA(0x000000, 160));

	/* Play or pause. */
	button.x = 12;
	button.y = bar.y + 12;
	button.width = 84;
	button.height = 32;
	label = "Play";
	if (state == VP_PLAYING)
		label = "Pause";
	pressed = kl_button(player->ui, &player->style, VP_ID_PLAY, &button, label, 0U);
	if (pressed)
		vp_toggle(player);

	/* The time now and the length. */
	clock = vp_media_clock(&player->media);
	vp_time_text(clock, now_text, sizeof(now_text));
	vp_time_text(duration, length_text, sizeof(length_text));
	(void)snprintf(times, sizeof(times), "%s / %s", now_text, length_text);
	kl_text_metrics(&player->text, 14U, &line);
	width = kl_text_width(&player->text, times, strlen(times), 14U, 0);
	(void)kl_text_draw(&player->text, &player->canvas, (int)player->width - width - 16, bar.y + (VP_BAR_HEIGHT + line.ascent - line.descent) / 2,
	    times, strlen(times), 14U, 0, KL_RGB(0xffffff));

	/* The position: it follows the clock, and a drag seeks (not more often than VP_SEEK_US). */
	slider.x = button.x + button.width + 16;
	slider.y = bar.y + 12;
	slider.width = (int)player->width - slider.x - width - 32;
	slider.height = 32;
	if (slider.width < 40 || duration <= 0.0)
		return;
	player->position = clock;
	changed = kl_slider(player->ui, &player->style, VP_ID_POSITION, &slider, 0.0, duration, 0.0, &player->position);
	if (changed && now_us - player->seek_at >= VP_SEEK_US) {
		player->seek_at = now_us;
		vp_media_seek(&player->media, player->position);
		player->need_picture = 1;
		vp_log("SEEK to_ms=%lld via=slider", (long long)(player->position * 1000.0));
	}
}

/* Writes a time as m:ss or h:mm:ss (-:-- when not known). */
static void
vp_time_text(
	double seconds,
	char *text,
	size_t size)
{
	long whole;
	int unknown;

	/* Not known. */
	unknown = isnan(seconds);
	if (seconds < 0.0 || unknown) {
		(void)snprintf(text, size, "-:--");
		return;
	}

	/* Hours when there are. */
	whole = (long)seconds;
	if (whole >= 3600L)
		(void)snprintf(text, size, "%ld:%02ld:%02ld", whole / 3600L, (whole / 60L) % 60L, whole % 60L);
	else
		(void)snprintf(text, size, "%ld:%02ld", whole / 60L, whole % 60L);
}

/* How long the loop may wait (ms): until the next picture while playing, briefly while the bar shows. */
static int
vp_wait(
	struct vp_player *player,
	uint64_t now_us)
{
	unsigned state;
	int wait;

	/* Something to draw now. */
	if (player->dirty)
		return 0;

	/* While playing, the pictures' pace (a frame's time at most). */
	(void)pthread_mutex_lock(&player->media.lock);
	state = player->media.state;
	(void)pthread_mutex_unlock(&player->media.lock);
	wait = VP_IDLE_MS;
	if (state == VP_PLAYING || player->need_picture)
		wait = 10;

	/* While the bar shows after the pointer moved, its time updates. */
	if (now_us < player->bar_until + VP_BAR_US && wait > 100)
		wait = 100;
	return wait;
}

/* Gives how long the bar stays after the pointer moved: less in full screen (ws122-p005a). */
static uint64_t
vp_bar_time(
	struct vp_player *player)
{
	int full;

	/* Full screen: the picture is uncovered sooner. */
	full = kl_window_fullscreen(player->window);
	if (full)
		return VP_BAR_FULL_US;

	/* A window. */
	return VP_BAR_US;
}

/*
 * Tells whether a button event is the second press of a double click on
 * the picture (not on the bar while it shows): soon after the first and
 * near it.  Remembers a first press.  Returns 1 for the second.
 */
static int
vp_double_click(
	struct vp_player *player,
	const struct kl_window_event *event,
	uint64_t now)
{
	int32_t dx;
	int32_t dy;
	int on_bar;

	/* Only the left button's press. */
	if (event->code != KL_BUTTON_LEFT || !event->pressed)
		return 0;

	/* Not on the bar while it shows (its controls take the press). */
	on_bar = 0;
	if (event->y >= (int32_t)player->height - VP_BAR_HEIGHT && now <= player->bar_until)
		on_bar = 1;
	if (on_bar) {
		player->click_us = 0U;
		return 0;
	}

	/* The second press soon after and near the first: a double click, after which a new one begins. */
	dx = (int32_t)event->x - player->click_x;
	dy = (int32_t)event->y - player->click_y;
	if (player->click_us != 0U &&
	    now - player->click_us <= VP_DOUBLE_US &&
	    dx * dx + dy * dy <= VP_DOUBLE_NEAR * VP_DOUBLE_NEAR) {
		player->click_us = 0U;
		return 1;
	}

	/* A first press. */
	player->click_us = now;
	player->click_x = (int32_t)event->x;
	player->click_y = (int32_t)event->y;
	return 0;
}
