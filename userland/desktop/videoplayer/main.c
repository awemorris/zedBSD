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

/*
 * After the end of the file, a heard position that stands still this long
 * (us) is the end of the sound (BUG-300); and how often the clock is logged
 * while playing (us).
 */
#define VP_END_STALL_US		500000U
#define VP_CLOCK_LOG_US		1000000U

/* The pictures replaced while a frame on the GPU may still draw them, at most. */
#define VP_RETIRED		4U

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
	struct app_frame *picture;
	double picture_time;
	struct app_scaler *scaler;

	/*
	 * What the frames drawn since the last DRAW line cost (ws236 p008):
	 * how many, the time fitting the picture into the frame took, and the
	 * time the rest (the bar and the present) took, in microseconds.
	 */
	unsigned draws;
	uint64_t draw_picture_us;
	uint64_t draw_rest_us;

	/* What the window says instead of a picture when a file could not be played ("" for nothing). */
	char notice[160];
	unsigned shown;
	int need_picture;

	/*
	 * The watch on the sound after the end of the file: the position heard
	 * when it last moved, and when (us).  A position that stands still after
	 * the end is the end (BUG-300).  It starts again whenever the player is
	 * not playing at the end.
	 */
	int end_watching;
	uint64_t end_heard;
	uint64_t end_heard_us;

	/* When the clock was last logged (us, 0 for never). */
	uint64_t clock_logged_us;

	/* The bar: shown until a time; the position while dragged and the last seek it asked. */
	uint64_t bar_until;
	double position;
	uint64_t seek_at;

	/* The last press on the picture (when and where), which a second one soon after makes a double click. */
	uint64_t click_us;
	int32_t click_x;
	int32_t click_y;

	/*
	 * The presenter (VP_GPU_PRESENT): whether it is open, when the overlay
	 * (the bar, a notice, a picture the CPU scaled) was last drawn, and
	 * whether the last frame showed it.  picture_serial is the newest frame
	 * that drew the picture shown (0 for none); a picture replaced while a
	 * frame that drew it may be on the GPU waits in retired with that
	 * frame's serial until the presenter reports it done.  draw_gpu counts
	 * the frames drawn since the last DRAW line that showed a picture the
	 * GPU holds.  next_due is the time of the next picture (-1 for none),
	 * which the loop waits for.
	 */
	struct vp_present present;
	int present_open;
	uint64_t overlay_us;
	int overlay_shown;
	uint64_t picture_serial;
	struct app_frame *retired[VP_RETIRED];
	uint64_t retired_serial[VP_RETIRED];
	unsigned retired_count;
	unsigned draw_gpu;
	double next_due;

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

static int vp_parse(int argc, char **argv, unsigned *width, unsigned *height, unsigned *timeout, int *fullscreen, const char **file);
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
static int vp_played_out(struct vp_player *player, double clock, uint64_t now_us);
static void vp_log_clock(struct vp_player *player, double clock, int played_out, uint64_t now_us);
static void vp_draw_picture(struct vp_player *player);
static int vp_fit(struct vp_player *player, struct app_frame *picture, int *x, int *y, int *width, int *height);
static void vp_draw_gpu(struct vp_player *player, uint64_t now_us, int overlay_dirty);
static int vp_bar_visible(struct vp_player *player, uint64_t now_us);
static void vp_retire_picture(struct vp_player *player);
static void vp_reap(struct vp_player *player, int all);
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
	VkResult result;
	int fullscreen;
	int status;
	int error;

	/* libmedia's log lines (the reader's and the decoders') among the player's; no picture is due yet. */
	media_set_log(vp_media_log, NULL);
	player.next_due = -1.0;

	/* The command line. */
	status = vp_parse(argc, argv, &width, &height, &timeout, &fullscreen, &file);
	if (status != 0) {
		fprintf(stderr, "usage: videoplayer [--width=N] [--height=N] [--timeout-s=N] [--fullscreen] [FILE]\n");
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
	if (VP_GPU_PRESENT)
		window_options.present = KL_PRESENT_NONE;
	player.window = kl_app_window_create(player.app, &window_options);
	player.ui = kl_ui_create();
	if (player.window == NULL || player.ui == NULL) {
		vp_log("FAILED operation=window error=%d", errno);
		kl_app_close(player.app);
		return 1;
	}

	/* The window shows a video: in full screen the compositor may show it without composing (game mode, ws122-p005b). */
	(void)kl_window_set_content_type(player.window, KL_CONTENT_VIDEO);

	/*
	 * The player's own presenter draws the pictures the GPU holds
	 * (ws236-p011); the decoders are asked for them from the start.
	 */
	if (VP_GPU_PRESENT) {
		result = vp_present_open(&player.present, player.window);
		if (result != VK_SUCCESS) {
			vp_log("FAILED operation=present step=%s result=%d", player.present.operation, (int)result);
			vp_present_close(&player.present);
			kl_ui_destroy(player.ui);
			kl_app_close(player.app);
			return 1;
		}

		/* Open; the decoders give pictures the GPU holds when the presenter can import them. */
		player.present_open = 1;
		if (player.present.imports_supported)
			media_decoder_gpu_pictures(VP_GPU_HOLD);
		vp_log("PRESENT own=1 gpu_pictures=%d hold=%u", player.present.imports_supported, VP_GPU_HOLD);
	}

	/* The menu, the style, the sound (none is no failure) and the media. */
	(void)kl_window_set_menu(player.window, vp_menu, sizeof(vp_menu) / sizeof(vp_menu[0]));
	player.style.text = &player.text;
	player.style.theme = kl_theme_default();
	player.style.glass = 0;
	error = vp_audio_open(&player.audio);
	vp_log("AUDIO error=%d", error);
	vp_media_init(&player.media, &player.audio);
	vp_states(&player);

	/* Full screen from the start when asked: the compositor may then show the pictures without composing them. */
	if (fullscreen) {
		kl_window_set_fullscreen(player.window, 1);
		vp_log("FULLSCREEN on=1 via=option");
		vp_states(&player);
	}

	/* The file named, and the loop until the window closes. */
	if (file != NULL)
		vp_open(&player, file);
	status = vp_loop(&player, timeout);

	/* Record the final presentation state before closing the reader and its audio clock. */
	vp_log("FRAMES shown=%u time_ms=%lld late=%u", player.shown, (long long)(vp_media_clock(&player.media) * 1000.0), player.media.late);

	/* Everything goes. */
	vp_media_close(&player.media);
	vp_audio_close(&player.audio);
	if (player.present_open)
		(void)vp_present_idle(&player.present);
	vp_reap(&player, 1);
	app_frame_free(&player.picture);
	app_scaler_free(player.scaler);
	if (player.present_open)
		vp_present_close(&player.present);
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
	int *fullscreen,
	const char **file)
{
	int index;
	int same;

	/* The defaults. */
	*width = VP_WIDTH;
	*height = VP_HEIGHT;
	*timeout = 0U;
	*fullscreen = 0;
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

		/* Full screen from the start. */
		same = strcmp(argv[index], "--fullscreen");
		if (same == 0) {
			*fullscreen = 1;
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
		vp_retire_picture(player);
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
	vp_retire_picture(player);
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
	kl_mutex_lock(&player->media.lock);
	state = player->media.state;
	kl_mutex_unlock(&player->media.lock);

	/* The other. */
	if (state == VP_PLAYING) {
		vp_media_pause(&player->media);
		vp_log("PAUSE shown=%u time_ms=%lld late=%u", player->shown, (long long)(vp_media_clock(&player->media) * 1000.0), player->media.late);
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
	kl_mutex_lock(&player->media.lock);
	state = player->media.state;
	kl_mutex_unlock(&player->media.lock);

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
	VkResult result;
	int status;

	/* The presenter at the window's size. */
	status = kl_window_present_resize(player->window, &player->width, &player->height);
	if (status != 0) {
		vp_log("FAILED operation=present error=%d", status);
		return -1;
	}

	/* The player's own presenter takes the size too; the swapchain's may differ. */
	if (player->present_open) {
		result = vp_present_resize(&player->present, player->width, player->height);
		if (result != VK_SUCCESS) {
			vp_log("FAILED operation=present step=%s result=%d", player->present.operation, (int)result);
			return -1;
		}

		/* The frame is drawn at the swapchain's size. */
		player->width = player->present.extent.width;
		player->height = player->present.extent.height;
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
	struct app_frame *picture;
	uint64_t shown_us;
	uint64_t started_us;
	uint64_t drawn_us;
	uint64_t after_ms;
	double clock;
	double due;
	double time;
	double next;
	int status;
	int failure;
	int played_out;
	int overlay_dirty;

	/* A decoder failure is a user-visible playback state, rather than an apparent end of the file. */
	kl_mutex_lock(&player->media.lock);
	failure = player->media.failure;
	player->media.failure = 0;
	kl_mutex_unlock(&player->media.lock);
	if (failure != 0) {
		(void)snprintf(player->notice, sizeof(player->notice), "Playback stopped because the media could not be decoded.");
		player->dirty = 1;
	}

	/* The clock, and whether the sound has played out at the end of the file. */
	clock = vp_media_clock(&player->media);
	played_out = vp_played_out(player, clock, now_us);

	/* The clock's reckoning, once a second while playing (BUG-300). */
	vp_log_clock(player, clock, played_out, now_us);

	/*
	 * The picture whose time has come (the first one at once after an open
	 * or a seek).  Once the sound has played out, every picture left is due:
	 * the clock stops short of the end with the position heard (BUG-300).
	 */
	due = clock;
	if (played_out)
		due = HUGE_VAL;

	/* What changed before the picture (input, the state): the overlay is drawn again for it. */
	overlay_dirty = player->dirty;
	picture = vp_media_take(&player->media, due, &time, &next);
	if (picture == NULL && player->need_picture && next >= 0.0)
		picture = vp_media_take(&player->media, next, &time, &next);
	player->next_due = next;
	if (picture != NULL) {
		vp_retire_picture(player);
		player->picture = picture;
		player->picture_time = time;
		player->need_picture = 0;
		player->shown++;
		player->dirty = 1;
		if (player->shown == 1U || player->shown % 100U == 0U)
			vp_log("FRAMES shown=%u time_ms=%lld late=%u", player->shown, (long long)(time * 1000.0), player->media.late);
	}

	/* The end of the file, published once the sound has played out and the last picture is taken. */
	kl_mutex_lock(&player->media.lock);

	if (
		played_out &&
		player->media.state == VP_PLAYING &&
		player->media.picture_count == 0U) {
		/* The clock stays at the end (BUG-300), as the slider and the time show it while ended. */
		player->media.clock_time = clock;
		if (player->media.duration > clock)
			player->media.clock_time = player->media.duration;
		player->media.state = VP_ENDED;
		vp_log("FRAMES shown=%u time_ms=%lld late=%u", player->shown, (long long)(clock * 1000.0), player->media.late);
		vp_log("ENDED shown=%u", player->shown);
		player->dirty = 1;
	}

	kl_mutex_unlock(&player->media.lock);

	/* Nothing changed: no frame. */
	if (!player->dirty && now_us > player->bar_until + VP_BAR_US)
		return;
	player->dirty = 0;

	/* The player's own presenter draws the frame. */
	if (player->present_open) {
		vp_draw_gpu(player, now_us, overlay_dirty);
		return;
	}

	/* The picture, the bar, and the frame shown, each timed for the DRAW line. */
	started_us = kl_clock_us();
	kl_ui_begin(player->ui, now_us);
	vp_draw_picture(player);
	drawn_us = kl_clock_us();
	vp_draw_bar(player, now_us);
	(void)kl_ui_end(player->ui, now_us);
	status = kl_window_present(player->window, player->pixels, (size_t)player->width);
	player->draws++;
	player->draw_picture_us += drawn_us - started_us;
	player->draw_rest_us += kl_clock_us() - drawn_us;
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

/*
 * Tells whether the sound has played out at the end of the file while playing (BUG-300):
 * the device side has taken all the sound written but the last frame at most, or the
 * position heard has stood still for a while; without sound, the clock is at the length.
 */
static int
vp_played_out(
	struct vp_player *player,
	double clock,
	uint64_t now_us)
{
	uint64_t read_frames;
	uint64_t written_frames;
	uint64_t heard_frames;
	double duration;
	unsigned state;
	int has_audio;
	int seeking;
	int eof;

	/* The media's state, and whether the reader has come to the end of the file. */
	kl_mutex_lock(&player->media.lock);

	state = player->media.state;
	eof = player->media.eof;
	seeking = player->media.seek_wanted;
	has_audio = player->media.has_audio;
	duration = player->media.duration;

	kl_mutex_unlock(&player->media.lock);

	/* Not playing at the end, or a seek on its way: not played out, and the watch on the sound starts again. */
	if (state != VP_PLAYING || !eof || seeking) {
		player->end_watching = 0;
		return 0;
	}

	/* Without sound, the end comes with the clock at the length (or at once without one). */
	if (!has_audio) {
		if (duration <= 0.0)
			return 1;
		if (clock >= duration)
			return 1;
		return 0;
	}

	/*
	 * The device side has taken all the sound written.  The rate converter
	 * keeps a last frame waiting for a next one to blend towards, which never
	 * comes, so one frame short is the end as well.
	 */
	read_frames = vp_audio_read_position(&player->audio);
	written_frames = vp_audio_write_position(&player->audio);
	if (read_frames + 1U >= written_frames)
		return 1;

	/* The position heard: one that moved starts the watch again from now. */
	heard_frames = vp_audio_clock_position(&player->audio);
	if (!player->end_watching || heard_frames != player->end_heard) {
		player->end_watching = 1;
		player->end_heard = heard_frames;
		player->end_heard_us = now_us;
		return 0;
	}

	/* A position heard that has stood still since the end of the file is the end of the sound. */
	if (now_us - player->end_heard_us >= VP_END_STALL_US)
		return 1;

	/* Succeeded: the sound is still playing out. */
	return 0;
}

/* Logs the clock, the sound's counts and the pictures waiting, once a second while playing (BUG-300). */
static void
vp_log_clock(
	struct vp_player *player,
	double clock,
	int played_out,
	uint64_t now_us)
{
	uint64_t heard_frames;
	uint64_t written_frames;
	uint64_t read_frames;
	unsigned pictures;
	unsigned late;
	unsigned state;
	int eof;

	/* Not a second since the last line. */
	if (player->clock_logged_us != 0U && now_us - player->clock_logged_us < VP_CLOCK_LOG_US)
		return;

	/* The media's state and the pictures decoded ahead. */
	kl_mutex_lock(&player->media.lock);

	state = player->media.state;
	eof = player->media.eof;
	pictures = player->media.picture_count;
	late = player->media.late;

	kl_mutex_unlock(&player->media.lock);

	/* Only while playing. */
	if (state != VP_PLAYING)
		return;

	/* The sound's counts: the position heard, the frames written, and the frames the device side took. */
	heard_frames = vp_audio_clock_position(&player->audio);
	written_frames = vp_audio_write_position(&player->audio);
	read_frames = vp_audio_read_position(&player->audio);

	/* The line, and when it was written. */
	vp_log("CLOCK time_ms=%lld heard=%llu written=%llu consumed=%llu pictures=%u late=%u eof=%d out=%d state=%u",
	    (long long)(clock * 1000.0),
	    (unsigned long long)heard_frames,
	    (unsigned long long)written_frames,
	    (unsigned long long)read_frames,
	    pictures,
	    late,
	    eof,
	    played_out,
	    state);
	player->clock_logged_us = now_us;

	/* What the frames drawn since the last line cost, on average. */
	if (player->draws != 0U) {
		vp_log("DRAW frames=%u picture_us=%llu rest_us=%llu gpu=%u acquire_us=%llu submit_us=%llu wait_us=%llu",
		    player->draws,
		    (unsigned long long)(player->draw_picture_us / player->draws),
		    (unsigned long long)(player->draw_rest_us / player->draws),
		    player->draw_gpu,
		    (unsigned long long)(player->present.acquire_us / player->draws),
		    (unsigned long long)(player->present.submit_us / player->draws),
		    (unsigned long long)(player->present.wait_us / player->draws));
	}

	/* The next line counts from here. */
	player->draws = 0U;
	player->draw_picture_us = 0U;
	player->draw_rest_us = 0U;
	player->draw_gpu = 0U;
	player->present.acquire_us = 0U;
	player->present.submit_us = 0U;
	player->present.wait_us = 0U;
}

/* Draws the picture fitted to the window, black around it (or black alone). */
static void
vp_draw_picture(
	struct vp_player *player)
{
	struct kl_rect whole;
	struct kl_text_line line;
	struct app_frame *picture;
	int picture_width;
	int picture_height;
	int fitted;
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

	/* The picture fitted to the window; too small to show, nothing. */
	fitted = vp_fit(player, picture, &x, &y, &width, &height);
	if (!fitted)
		return;
	app_frame_size(picture, &picture_width, &picture_height);

	/* Scaled straight into the frame (BGRA is the canvas's 0xAARRGGBB, opaque); the scaler is remade when the sizes change. */
	status = app_frame_scale(picture, &player->scaler, player->pixels + (size_t)y * player->width + (size_t)x,
	    player->width * sizeof(uint32_t), width, height);
	if (status != 0)
		vp_log("SCALE failed width=%d height=%d", picture_width, picture_height);
}

/*
 * Fits a picture to the window by its shape (and its samples' aspect):
 * its rectangle in window pixels.  Returns 0 when it is too small to show.
 */
static int
vp_fit(
	struct vp_player *player,
	struct app_frame *picture,
	int *x,
	int *y,
	int *width,
	int *height)
{
	double aspect;
	int picture_width;
	int picture_height;
	int sar_num;
	int sar_den;

	/* The picture's shape. */
	app_frame_size(picture, &picture_width, &picture_height);
	if (picture_width <= 0 || picture_height <= 0)
		return 0;
	app_frame_aspect(picture, &sar_num, &sar_den);
	aspect = (double)picture_width * sar_num / ((double)picture_height * sar_den);

	/* As wide as the window, unless that is too tall. */
	*width = (int)player->width;
	*height = (int)((double)*width / aspect);
	if (*height > (int)player->height) {
		*height = (int)player->height;
		*width = (int)((double)*height * aspect);
	}

	/* Too small to show. */
	if (*width < 2 || *height < 2)
		return 0;

	/* Succeeded: in the middle of the window. */
	*x = ((int)player->width - *width) / 2;
	*y = ((int)player->height - *height) / 2;
	return 1;
}

/*
 * Draws a frame with the player's own presenter: the picture the GPU holds
 * as a quad (converted and scaled by the GPU), and the overlay over it --
 * the bar, a notice, or a picture the CPU scaled -- drawn by the CPU only
 * when it changed: on input or a change of state (overlay_dirty), for each
 * picture the CPU scales, and four times a second while the bar shows (its
 * time and position move).  An overlay with nothing in it is not drawn.
 */
static void
vp_draw_gpu(
	struct vp_player *player,
	uint64_t now_us,
	int overlay_dirty)
{
	struct vp_present_quad quad;
	const struct vp_present_quad *shown;
	uint64_t submitted;
	uint64_t started_us;
	uint64_t drawn_us;
	uint64_t shown_us;
	uint64_t after_ms;
	VkResult result;
	int gpu;
	int bar;
	int fitted;
	int overlay;
	int changed;
	int status;

	/* The pictures the frames done drew go back to the decoder. */
	started_us = kl_clock_us();
	vp_reap(player, 0);

	/* The picture the GPU holds, where it fits. */
	gpu = 0;
	shown = NULL;
	if (player->picture != NULL) {
		status = app_frame_gpu(player->picture, &quad.picture);
		if (status == 0) {
			gpu = 1;
			fitted = vp_fit(player, player->picture, &quad.x, &quad.y, &quad.width, &quad.height);
			if (fitted)
				shown = &quad;
		}
	}

	/*
	 * The overlay shows the bar, a picture the CPU scaled, or the notice
	 * where there is no picture; a picture the GPU holds without the bar
	 * needs none.
	 */
	bar = vp_bar_visible(player, now_us);
	overlay = bar;
	if (!gpu && player->picture != NULL)
		overlay = 1;
	if (player->picture == NULL && player->notice[0] != '\0')
		overlay = 1;

	/* Drawn again when it changed, for a picture the CPU scales, for a newly shown overlay, and while the bar moves. */
	changed = 0;
	if (overlay) {
		if (overlay_dirty || !gpu || !player->overlay_shown)
			changed = 1;
		if (bar && now_us - player->overlay_us >= 250000U)
			changed = 1;
	}

	/* The overlay: clear around the bar over a picture the GPU holds, else the picture or the notice on black. */
	if (changed) {
		kl_ui_begin(player->ui, now_us);
		if (gpu) {
			memset(player->pixels, 0, (size_t)player->width * player->height * sizeof(player->pixels[0]));
		} else {
			vp_draw_picture(player);
		}

		/* The bar over it, while it shows. */
		vp_draw_bar(player, now_us);
		(void)kl_ui_end(player->ui, now_us);
		player->overlay_us = now_us;
	}

	/* Whether this frame shows the overlay, for the next one's choice. */
	player->overlay_shown = overlay;

	/* The frame, waited for: the picture shown before may be freed after it. */
	drawn_us = kl_clock_us();
	submitted = player->present.serial;
	result = vp_present_frame(&player->present, player->pixels, (size_t)player->width, overlay, changed, shown);

	/* A frame that reached the GPU with the picture keeps the picture until it is done, even if its present failed. */
	if (shown != NULL && player->present.serial != submitted) {
		player->picture_serial = player->present.serial;
		player->draw_gpu++;
	}

	/* The frame's cost, for the DRAW line. */
	player->draws++;
	player->draw_picture_us += drawn_us - started_us;
	player->draw_rest_us += kl_clock_us() - drawn_us;

	/* An out-of-date swapchain is made again at the window's size; another failure is logged once for each new result. */
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
		player->resized = 1;
	} else if (result != VK_SUCCESS && (int)result != player->present_error) {
		vp_log("FAILED operation=frame step=%s result=%d", player->present.operation, (int)result);
		player->present_error = (int)result;
	}

	/* The first frame shown at a new size, and how long after the size came. */
	if (result == VK_SUCCESS && (player->width != player->presented_width || player->height != player->presented_height)) {
		player->presented_width = player->width;
		player->presented_height = player->height;
		shown_us = kl_clock_us();
		after_ms = 0U;
		if (shown_us > player->resized_us)
			after_ms = (shown_us - player->resized_us) / 1000U;
		vp_log("PRESENTED width=%u height=%u after_ms=%llu picture=%d gpu=%d", player->width, player->height, (unsigned long long)after_ms, player->picture != NULL, gpu);
	}
}

/*
 * Lets the picture shown go: at once when no frame on the GPU may still
 * draw it, else into the retired list until its frame is done.
 */
static void
vp_retire_picture(
	struct vp_player *player)
{
	uint64_t done;

	/* No picture. */
	if (player->picture == NULL)
		return;

	/* Drawn by no frame still on the GPU: it goes now. */
	done = 0U;
	if (player->present_open)
		done = vp_present_done(&player->present);
	if (!player->present_open || player->picture_serial <= done) {
		app_frame_free(&player->picture);
		player->picture_serial = 0U;
		return;
	}

	/* A full list waits for every frame first. */
	if (player->retired_count == VP_RETIRED) {
		(void)vp_present_idle(&player->present);
		vp_reap(player, 0);
	}

	/* It waits for its frame. */
	player->retired[player->retired_count] = player->picture;
	player->retired_serial[player->retired_count] = player->picture_serial;
	player->retired_count++;
	player->picture = NULL;
	player->picture_serial = 0U;
}

/* Frees the retired pictures whose frames are done, or all of them (the presenter idle). */
static void
vp_reap(
	struct vp_player *player,
	int all)
{
	uint64_t done;
	unsigned index;
	unsigned kept;

	/* The newest frame done. */
	done = 0U;
	if (player->present_open)
		done = vp_present_done(&player->present);

	/* Each retired picture, freed when its frame is done; the others move up. */
	kept = 0U;
	for (index = 0U; index < player->retired_count; index++) {
		if (all || player->retired_serial[index] <= done) {
			app_frame_free(&player->retired[index]);
			continue;
		}

		/* Still drawn by a frame on the GPU. */
		player->retired[kept] = player->retired[index];
		player->retired_serial[kept] = player->retired_serial[index];
		kept++;
	}

	/* The pictures still waiting. */
	player->retired_count = kept;
}

/* Tells whether the bar of controls shows: while paused or ended, and for a while after the pointer moved. */
static int
vp_bar_visible(
	struct vp_player *player,
	uint64_t now_us)
{
	unsigned state;

	/* The media's state. */
	kl_mutex_lock(&player->media.lock);
	state = player->media.state;
	kl_mutex_unlock(&player->media.lock);

	/* Hidden while playing once its time is over. */
	if (state == VP_PLAYING && now_us > player->bar_until)
		return 0;

	/* Succeeded: it shows. */
	return 1;
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
	case MEDIA_PROBLEM_DEVICE:
		(void)snprintf(player->notice, sizeof(player->notice), "Video decoding is unavailable on this system.");
		break;
	case MEDIA_PROBLEM_PROFILE:
		(void)snprintf(player->notice, sizeof(player->notice), "This media format is not supported.");
		break;
	case MEDIA_PROBLEM_BUSY:
		(void)snprintf(player->notice, sizeof(player->notice), "The video decoder is busy. Try again after closing another video.");
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
	kl_mutex_lock(&player->media.lock);
	state = player->media.state;
	duration = player->media.duration;
	kl_mutex_unlock(&player->media.lock);
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
	double until;
	int wait;

	/* Something to draw now. */
	if (player->dirty)
		return 0;

	/* While playing, the pictures' pace (a frame's time at most). */
	kl_mutex_lock(&player->media.lock);
	state = player->media.state;
	kl_mutex_unlock(&player->media.lock);
	wait = VP_IDLE_MS;
	if (state == VP_PLAYING || player->need_picture)
		wait = 10;

	/* With the presenter, playing with a picture waiting: until its time, no longer. */
	if (player->present_open && state == VP_PLAYING && player->next_due >= 0.0) {
		until = (player->next_due - vp_media_clock(&player->media)) * 1000.0;
		if (until < 0.0)
			until = 0.0;
		if (until < (double)wait)
			wait = (int)until;
	}

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
