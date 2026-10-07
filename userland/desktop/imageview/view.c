/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The view of Image Viewer (ws091): which image of the folder is shown,
 * how (its scale, where it is scrolled to, its quarter turns), and what
 * the keys, the pointer and the actions do to it.
 *
 * The image is fitted to the window unless zoomed: the largest scale that
 * shows it whole, never enlarging it.  Zoomed, it is dragged about (a drag
 * of the pointer, the arrow keys) and zoomed about the pointer (the wheel)
 * or the window's middle (the keys and the actions); fitted, a sideways
 * drag swipes to the next or the previous image.  A double click, like a
 * double tap, goes from the fit to 100 % (or twice the fit, the larger)
 * about the point clicked, and back.  The neighbours of the image shown
 * are decoded ahead while nothing else is going on.
 */

#include "imageview.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* How long a message stays, and how long the chip stays after the view changes, in milliseconds. */
#define VIEW_MESSAGE_MS		3000U
#define VIEW_CHIP_MS		2500U

/* How long the chip takes to fade at its end, in milliseconds. */
#define VIEW_CHIP_FADE_MS	300U

/* How much one step of the zoom changes the scale, and the largest scale (an image pixel as 16 window pixels). */
#define VIEW_ZOOM_STEP		1.25
#define VIEW_SCALE_MAX		16.0

/* How far below the fit two fingers or the wheel may zoom out before it springs back (a share of the fit). */
#define VIEW_SCALE_UNDER	0.5

/* How many pixels of the wheel make one step of the zoom (the compositor sends 60 a notch). */
#define VIEW_WHEEL_STEP		60.0

/* How far an arrow key moves a zoomed image, in pixels. */
#define VIEW_PAN_STEP		64.0

/* The longest two clicks may be apart, in time and in pixels, to make a double click. */
#define VIEW_DOUBLE_CLICK_MS	400U
#define VIEW_DOUBLE_CLICK_SLOP	8

/* How far the pointer moves before a press becomes a drag, in pixels. */
#define VIEW_DRAG_SLOP		6

/* How far (a share of the area's width) or how fast (pixels a millisecond) a swipe must go to change the image. */
#define VIEW_SWIPE_SHARE	0.33
#define VIEW_SWIPE_SPEED	0.8

/* The share of a swipe's movement kept past the first and the last image. */
#define VIEW_SWIPE_RESIST	3.0

/* How long the image slides into place after a swipe, and how far it comes from (a share of the area's width). */
#define VIEW_SLIDE_MS		220U
#define VIEW_SLIDE_SHARE	0.35

/* The scale from which the image is sampled to its nearest texel, so that its pixels show sharp. */
#define VIEW_NEAREST_SCALE	3.0

/* How often the view is redrawn while it moves by itself, in milliseconds. */
#define VIEW_FRAME_MS		16

/* The size of the empty window's Open button. */
#define VIEW_OPEN_WIDTH		140
#define VIEW_OPEN_HEIGHT	38

static void view_show(struct iv_app *app, struct iv_image *image, int keep_neighbours);
static void view_go(struct iv_app *app, size_t index);
static void view_step(struct iv_app *app, int direction);
static struct iv_image *view_decode(struct iv_app *app, size_t index);
static void view_free_image(struct iv_image **image);
static void view_refresh_folder(struct iv_app *app);
static void view_reset(struct iv_app *app);
static void view_turn(struct iv_app *app, int quarters);
static void view_zoom_step(struct iv_app *app, double factor, double x, double y);
static double view_ease(double progress);
static double view_left(const struct iv_app *app);
static double view_top(const struct iv_app *app);
static double view_min_scale(const struct iv_app *app);
static void view_key(struct iv_app *app, const struct iv_event *event);
static void view_pan_key(struct iv_app *app, double dx, double dy, int direction);
static int view_toggle_key(uint32_t key);
static void view_button(struct iv_app *app, const struct iv_event *event);
static void view_motion(struct iv_app *app, const struct iv_event *event);
static void view_axis(struct iv_app *app, const struct iv_event *event);
static int view_can_swipe(const struct iv_app *app);
static void open_chooser(struct iv_app *app);
static void view_slideshow_tick(struct iv_app *app, uint64_t now, int *due);

/*
 * Starts the viewer with no image, for a window of a size.
 */
void
iv_app_init(
	struct iv_app *app,
	struct iv_text *text,
	int width,
	int height)
{
	/* Nothing shown, fitted, and the whole window to lay out. */
	memset(app, 0, sizeof(*app));
	app->text = text;
	app->fit = 1;
	app->scale = 1.0;
	app->glass = 1;
	app->want_open_with = -1;
	app->now = iv_clock();
	iv_app_resize(app, width, height);
	app->dirty = 1;
	app->ui_dirty = 1;
}

/*
 * Frees the images and the folder.
 */
void
iv_app_release(
	struct iv_app *app)
{
	/* The images and the folder. */
	iv_app_close_image(app);
}

/*
 * Opens a file (or the first image of a folder) and its folder's images.
 *
 * Returns 0 when something is shown (the image, or why it cannot be), or
 * an errno value with a message.
 */
int
iv_app_open(
	struct iv_app *app,
	const char *path)
{
	struct iv_image *image;
	struct stat status;
	int is_folder;
	int error;

	/* The folder the path is in, or is. */
	error = iv_folder_read(&app->folder, path);
	if (error != 0) {
		iv_app_message(app, "Cannot read that folder.", VIEW_MESSAGE_MS);
		iv_log("OPEN failed path=%s errno=%d", path, error);
		return error;
	}

	/* Whether the path is a folder, which shows its first image. */
	is_folder = 0;
	error = stat(path, &status);
	if (error == 0)
		is_folder = S_ISDIR(status.st_mode);

	/* A folder without images says so. */
	if (is_folder && app->folder.count == 0) {
		iv_app_message(app, "There are no images in that folder.", VIEW_MESSAGE_MS);
		iv_log("OPEN empty folder=%s", path);
		return ENOENT;
	}

	/* The neighbours of whatever was shown go. */
	view_free_image(&app->previous);
	view_free_image(&app->next);

	/* The image is decoded now: a folder's first, or the file (a failure is shown with its reason). */
	if (is_folder) {
		image = view_decode(app, app->folder.index);
	} else {
		image = calloc(1, sizeof(*image));
		if (image != NULL)
			(void)iv_image_load(image, path, app->max_dimension);
	}

	/* Without memory for it, nothing changes. */
	if (image == NULL) {
		iv_app_message(app, "There is not enough memory to open that.", VIEW_MESSAGE_MS);
		return ENOMEM;
	}

	/* The image replaces whatever was shown. */
	view_show(app, image, 0);

	/* Succeeded: the image is shown (or why it cannot be). */
	return 0;
}

/*
 * Stops showing any image: the images and the folder go.
 */
void
iv_app_close_image(
	struct iv_app *app)
{
	/* Every image held. */
	view_free_image(&app->current);
	view_free_image(&app->previous);
	view_free_image(&app->next);

	/* The folder, and the view back to its start. */
	iv_folder_release(&app->folder);
	app->has_image = 0;
	app->image_serial++;
	view_reset(app);
	app->dirty = 1;
	app->ui_dirty = 1;
}

/*
 * Takes a new window size: the area images are shown in, and the fit.
 */
void
iv_app_resize(
	struct iv_app *app,
	int width,
	int height)
{
	/* The window's size, and the area in it. */
	app->window_width = width;
	app->window_height = height;
	iv_app_layout(app);
}

/*
 * Lays the area out in the window (the whole window when it is
 * fullscreen, otherwise inside the glass card with a margin), and keeps
 * the image fitted or in bounds.
 */
void
iv_app_layout(
	struct iv_app *app)
{
	int margin;

	/* The margin around the images: none when fullscreen. */
	margin = IV_MARGIN;
	if (app->fullscreen)
		margin = 0;

	/* The area, at least a pixel each way. */
	app->area_x = margin;
	app->area_y = margin;
	app->area_width = app->window_width - 2 * margin;
	app->area_height = app->window_height - 2 * margin;
	if (app->area_width < 1)
		app->area_width = 1;
	if (app->area_height < 1)
		app->area_height = 1;

	/* A fitted image follows the area; any other stays in bounds. */
	if (app->fit)
		app->scale = iv_app_fit_scale(app);
	iv_app_clamp(app);
	app->dirty = 1;
	app->ui_dirty = 1;
}

/*
 * Reports the scale that fits the whole turned image into the area,
 * never above 1 (1 without an image).
 */
double
iv_app_fit_scale(
	const struct iv_app *app)
{
	double width;
	double height;
	double across;
	double down;
	double scale;

	/* Without an image to show, the scale does not matter. */
	if (!app->has_image ||
	    app->current == NULL ||
	    app->current->width <= 0)
		return 1.0;

	/* The turned image's size. */
	width = (double)app->current->width;
	height = (double)app->current->height;
	if ((app->rotation & 1U) != 0) {
		width = (double)app->current->height;
		height = (double)app->current->width;
	}

	/* The smaller of the two ratios, and never an enlargement. */
	across = (double)app->area_width / width;
	down = (double)app->area_height / height;
	scale = across;
	if (down < scale)
		scale = down;
	if (scale > 1.0)
		scale = 1.0;

	/* Reports the fit. */
	return scale;
}

/*
 * Reports the width of the turned image at the scale, in window pixels.
 */
double
iv_app_content_width(
	const struct iv_app *app)
{
	int width;

	/* Nothing to show is no width. */
	if (!app->has_image || app->current == NULL)
		return 0.0;

	/* The turned image's width. */
	width = app->current->width;
	if ((app->rotation & 1U) != 0)
		width = app->current->height;

	/* Reports it at the scale. */
	return (double)width * app->scale;
}

/*
 * Reports the height of the turned image at the scale, in window pixels.
 */
double
iv_app_content_height(
	const struct iv_app *app)
{
	int height;

	/* Nothing to show is no height. */
	if (!app->has_image || app->current == NULL)
		return 0.0;

	/* The turned image's height. */
	height = app->current->height;
	if ((app->rotation & 1U) != 0)
		height = app->current->width;

	/* Reports it at the scale. */
	return (double)height * app->scale;
}

/*
 * Keeps the scroll within the image: 0 across a content narrower than
 * the area (it is centred), otherwise between 0 and how much wider it is.
 */
void
iv_app_clamp(
	struct iv_app *app)
{
	double largest_x;
	double largest_y;

	/* How far each way the content can be scrolled. */
	largest_x = iv_app_content_width(app) - (double)app->area_width;
	largest_y = iv_app_content_height(app) - (double)app->area_height;
	if (largest_x < 0.0)
		largest_x = 0.0;
	if (largest_y < 0.0)
		largest_y = 0.0;

	/* Across. */
	if (app->scroll_x > largest_x)
		app->scroll_x = largest_x;
	if (app->scroll_x < 0.0)
		app->scroll_x = 0.0;

	/* And down. */
	if (app->scroll_y > largest_y)
		app->scroll_y = largest_y;
	if (app->scroll_y < 0.0)
		app->scroll_y = 0.0;
}

/*
 * Sets the scale (within half the fit and VIEW_SCALE_MAX); the image is
 * fitted again only by the fit itself.  The scroll is not moved: the
 * caller keeps a place under a point with iv_app_show_place().
 */
void
iv_app_zoom_to(
	struct iv_app *app,
	double scale)
{
	double smallest;

	/* Within the bounds of the zoom. */
	smallest = view_min_scale(app);
	if (scale < smallest)
		scale = smallest;
	if (scale > VIEW_SCALE_MAX)
		scale = VIEW_SCALE_MAX;

	/* The scale set by hand is no longer the fit. */
	app->scale = scale;
	app->fit = 0;
	app->animation.running = 0;
	app->dirty = 1;
	iv_app_show_chip(app);
}

/*
 * Finds the place of the turned image under a window point.
 */
void
iv_app_place_at(
	const struct iv_app *app,
	double x,
	double y,
	struct iv_place *place)
{
	/* The point's distance from the content's corner, in image pixels. */
	place->x = (x - view_left(app)) / app->scale;
	place->y = (y - view_top(app)) / app->scale;
}

/*
 * Scrolls so that a place of the turned image lies under a window point
 * (as nearly as the bounds allow).
 */
void
iv_app_show_place(
	struct iv_app *app,
	const struct iv_place *place,
	double x,
	double y)
{
	/* The scroll that puts the place at the point. */
	app->scroll_x = place->x * app->scale - (x - (double)app->area_x);
	app->scroll_y = place->y * app->scale - (y - (double)app->area_y);
	iv_app_clamp(app);
	app->dirty = 1;
}

/*
 * Zooms to a scale keeping the place under a window point where it is,
 * at once or animated.
 */
void
iv_app_zoom_at(
	struct iv_app *app,
	double scale,
	double x,
	double y,
	int animate)
{
	struct iv_place place;
	double from_scale;
	double from_x;
	double from_y;

	/* Nothing to zoom without an image to show. */
	if (!app->has_image ||
	    app->current == NULL ||
	    app->current->error != 0)
		return;

	/* The place under the point, and where the view is now. */
	iv_app_place_at(app, x, y, &place);
	from_scale = app->scale;
	from_x = app->scroll_x;
	from_y = app->scroll_y;

	/* The view at the new scale with the place under the point. */
	iv_app_zoom_to(app, scale);
	iv_app_show_place(app, &place, x, y);

	/* Animated: from where the view was to where it is now. */
	if (animate) {
		app->animation.running = 1;
		app->animation.start = app->now;
		app->animation.from_scale = from_scale;
		app->animation.to_scale = app->scale;
		app->animation.from_x = from_x;
		app->animation.from_y = from_y;
		app->animation.to_x = app->scroll_x;
		app->animation.to_y = app->scroll_y;
		app->scale = from_scale;
		app->scroll_x = from_x;
		app->scroll_y = from_y;
	}

	/* The log line the tests read. */
	iv_log("ZOOM scale=%.4f x=%.1f y=%.1f", scale, app->scroll_x, app->scroll_y);
}

/*
 * Springs a zoom that went below the fit (two fingers, the wheel) back to
 * the fit.
 */
void
iv_app_settle(
	struct iv_app *app)
{
	double fit;

	/* At or above the fit nothing moves. */
	fit = iv_app_fit_scale(app);
	if (app->scale >= fit)
		return;

	/* Back to the fit, animated. */
	iv_app_action(app, IV_ACTION_FIT);
}

/*
 * Ends a swipe (the fingers or the pointer let go at a velocity, pixels a
 * millisecond, across): far or fast enough, and when may_turn allows it,
 * the next or the previous image comes in; otherwise the image slides
 * back.
 */
void
iv_app_swipe_end(
	struct iv_app *app,
	double velocity,
	int may_turn)
{
	double distance;
	double speed;
	int direction;
	int fast;
	int far;

	/* Whether the image went far enough, or fast enough. */
	direction = 0;
	distance = fabs(app->swipe);
	far = 0;
	if (distance > (double)app->area_width * VIEW_SWIPE_SHARE)
		far = 1;
	fast = 0;
	speed = fabs(velocity);
	if (speed > VIEW_SWIPE_SPEED)
		fast = 1;

	/* Which way it went: the swipe's side, or the fling's when it went fast but not far. */
	if (may_turn &&
	    (far || fast)) {
		/* Leftward goes on to the next image, rightward back to the previous one. */
		if (app->swipe < 0.0 ||
		    (!far && velocity < 0.0))
			direction = 1;
		else
			direction = -1;
	}

	/* No image that way: the swipe slides back. */
	if (direction > 0 && app->folder.index + 1 >= app->folder.count)
		direction = 0;
	if (direction < 0 && app->folder.index == 0)
		direction = 0;

	/* Sliding back from where the swipe left it. */
	if (direction == 0) {
		app->sliding = 1;
		app->slide_from = app->swipe;
		app->slide_start = app->now;
		app->dirty = 1;
		iv_log("SWIPE back from=%.0f", app->swipe);
		return;
	}

	/* The next image comes in from the side the swipe went away from. */
	view_step(app, direction);
	app->sliding = 1;
	app->slide_from = (double)direction * (double)app->area_width * VIEW_SLIDE_SHARE;
	app->slide_start = app->now;
	app->swipe = app->slide_from;
	app->dirty = 1;
	iv_log("SWIPE turn direction=%d", direction);
}

/*
 * Works out where the image is drawn this frame (none without an image
 * that can be shown): its corners in the window, turned, and the level of
 * halves drawn.
 */
void
iv_app_quad(
	const struct iv_app *app,
	struct iv_quad *quad)
{
	const struct iv_image *image;
	double left;
	double top;
	double right;
	double bottom;
	double halves;
	size_t level;

	/* Nothing drawn without an image that can be shown. */
	memset(quad, 0, sizeof(*quad));
	image = app->current;
	if (!app->has_image ||
	    image == NULL ||
	    image->error != 0 ||
	    image->level_count == 0)
		return;

	/* The content's rectangle, moved by a swipe. */
	left = view_left(app) + app->swipe;
	top = view_top(app);
	right = left + iv_app_content_width(app);
	bottom = top + iv_app_content_height(app);

	/* The image's corners (top left, top right, bottom left, bottom right) as its turn places them. */
	switch (app->rotation & 3U) {
	case 1:
		quad->x[0] = (float)right;
		quad->y[0] = (float)top;
		quad->x[1] = (float)right;
		quad->y[1] = (float)bottom;
		quad->x[2] = (float)left;
		quad->y[2] = (float)top;
		quad->x[3] = (float)left;
		quad->y[3] = (float)bottom;
		break;
	case 2:
		quad->x[0] = (float)right;
		quad->y[0] = (float)bottom;
		quad->x[1] = (float)left;
		quad->y[1] = (float)bottom;
		quad->x[2] = (float)right;
		quad->y[2] = (float)top;
		quad->x[3] = (float)left;
		quad->y[3] = (float)top;
		break;
	case 3:
		quad->x[0] = (float)left;
		quad->y[0] = (float)bottom;
		quad->x[1] = (float)left;
		quad->y[1] = (float)top;
		quad->x[2] = (float)right;
		quad->y[2] = (float)bottom;
		quad->x[3] = (float)right;
		quad->y[3] = (float)top;
		break;
	default:
		quad->x[0] = (float)left;
		quad->y[0] = (float)top;
		quad->x[1] = (float)right;
		quad->y[1] = (float)top;
		quad->x[2] = (float)left;
		quad->y[2] = (float)bottom;
		quad->x[3] = (float)right;
		quad->y[3] = (float)bottom;
		break;
	}

	/* The smallest level still at least as large as it is shown. */
	level = 0;
	if (app->scale < 1.0) {
		halves = floor(log2(1.0 / app->scale));
		if (halves > 0.0)
			level = (size_t)halves;
	}

	/* No smaller level than the last. */
	if (level >= image->level_count)
		level = image->level_count - 1U;

	/* The quad, clipped to the area, sampled to the nearest texel when it is much enlarged. */
	quad->visible = 1;
	quad->clip_x = app->area_x;
	quad->clip_y = app->area_y;
	quad->clip_width = app->area_width;
	quad->clip_height = app->area_height;
	quad->level = level;
	quad->nearest = 0;
	if (app->scale >= VIEW_NEAREST_SCALE)
		quad->nearest = 1;
}

/*
 * Shows a message for a while (0: until replaced).
 */
void
iv_app_message(
	struct iv_app *app,
	const char *message,
	uint64_t duration)
{
	/* Keeps the text and when it goes. */
	snprintf(app->message, sizeof(app->message), "%s", message);
	app->message_until = 0;
	if (duration != 0)
		app->message_until = app->now + duration;
	app->dirty = 1;
	app->ui_dirty = 1;
	iv_log("MESSAGE %s", message);
}

/*
 * Shows the chip (the image's name, place, size and zoom) for a while.
 */
void
iv_app_show_chip(
	struct iv_app *app)
{
	/* Only for an image being shown. */
	if (!app->has_image)
		return;

	/* The chip stays VIEW_CHIP_MS from now. */
	app->chip_until = app->now + VIEW_CHIP_MS;
	app->dirty = 1;
	app->ui_dirty = 1;
}

/*
 * Reports how opaque the chip is now (0 when it is not shown, 1 until it
 * starts to fade).
 */
double
iv_app_chip_opacity(
	const struct iv_app *app)
{
	uint64_t left;

	/* Not shown, or gone. */
	if (app->chip_until == 0 || app->now >= app->chip_until)
		return 0.0;

	/* Fading in its last moments. */
	left = app->chip_until - app->now;
	if (left < VIEW_CHIP_FADE_MS)
		return (double)left / (double)VIEW_CHIP_FADE_MS;

	/* Fully shown. */
	return 1.0;
}

/*
 * Places the empty window's Open button.
 */
void
iv_app_open_button(
	const struct iv_app *app,
	int *x,
	int *y,
	int *width,
	int *height)
{
	/* Under the mark and the words, in the middle. */
	*width = VIEW_OPEN_WIDTH;
	*height = VIEW_OPEN_HEIGHT;
	*x = (app->window_width - VIEW_OPEN_WIDTH) / 2;
	*y = app->window_height / 2 + 64;
}

/*
 * Takes the file chooser's answer: the file chosen (or the first image of
 * a folder) is shown with its folder's images; NULL or an empty path says
 * the chooser was cancelled.
 */
void
iv_app_chosen(
	struct iv_app *app,
	const char *path)
{
	/* The viewer no longer waits for the chooser. */
	app->chooser_open = 0;

	/* Cancelled: what is shown stays. */
	if (path == NULL || path[0] == '\0') {
		iv_log("CHOOSER cancelled");
		return;
	}

	/* A file chosen is shown. */
	iv_log("CHOOSER chose path=%s", path);
	(void)iv_app_open(app, path);
	app->dirty = 1;
	app->ui_dirty = 1;
}

/*
 * Goes on after the image shown left its folder (moved to the trash,
 * ws128-p005): the folder is read again and the image now in its place
 * is shown (the one before it at the end); a folder with no image left
 * shows nothing.
 */
void
iv_app_removed(
	struct iv_app *app)
{
	char directory[IV_PATH_MAX];
	size_t index;
	int error;

	/* The place the image had, and its folder. */
	index = app->folder.index;
	snprintf(directory, sizeof(directory), "%s", app->folder.directory);

	/* The neighbours decoded ahead may be the wrong ones now. */
	view_free_image(&app->previous);
	view_free_image(&app->next);

	/* The folder as it is now. */
	error = iv_folder_read(&app->folder, directory);
	if (error != 0 || app->folder.count == 0) {
		iv_log("REMOVED left=0");
		iv_app_slideshow(app, 0);
		iv_app_close_image(app);
		return;
	}

	/* The image now in its place, or the last one when it was the last. */
	if (index >= app->folder.count)
		index = app->folder.count - 1U;
	iv_log("REMOVED left=%lu index=%lu", (unsigned long)app->folder.count, (unsigned long)index);

	/* That image is decoded and shown (no neighbour is held, so view_go decodes it). */
	view_go(app, index);
}

/*
 * Starts (on) or stops a slideshow (ws128-p005): the images of the folder
 * one after another, each for IV_SLIDESHOW_MS, around from the last to the
 * first, on the full screen.  A slideshow that made the window fill the
 * screen leaves it when it stops.
 */
void
iv_app_slideshow(
	struct iv_app *app,
	int on)
{
	/* A stop, of a slideshow that runs. */
	if (!on) {
		if (!app->slideshow)
			return;

		/* The full screen the slideshow made goes with it. */
		app->slideshow = 0;
		if (app->slideshow_screen && app->fullscreen)
			app->want_fullscreen = 1;
		app->slideshow_screen = 0;

		/* Says so, for the user and the tests. */
		iv_app_message(app, "Slideshow stopped", 1200U);
		iv_log("SLIDESHOW stop");
		app->dirty = 1;
		return;
	}

	/* A slideshow needs an image to start from. */
	if (!app->has_image || app->folder.count == 0) {
		iv_app_message(app, "There are no images for a slideshow.", VIEW_MESSAGE_MS);
		return;
	}

	/* It starts, on the full screen, the next image due after a while. */
	app->slideshow = 1;
	app->slideshow_due = app->now + IV_SLIDESHOW_MS;
	app->slideshow_screen = 0;
	if (!app->fullscreen) {
		app->want_fullscreen = 1;
		app->slideshow_screen = 1;
	}

	/* Says so, for the user and the tests. */
	iv_app_message(app, "Slideshow \xe2\x80\x94 Esc stops", 1500U);
	iv_log("SLIDESHOW start count=%lu", (unsigned long)app->folder.count);
	app->dirty = 1;
}

/*
 * Takes one input of the window.
 */
void
iv_app_event(
	struct iv_app *app,
	const struct iv_event *event)
{
	/* Each kind of input to its handler. */
	switch (event->type) {
	case IV_EVENT_KEY:
		view_key(app, event);
		break;
	case IV_EVENT_BUTTON:
		view_button(app, event);
		break;
	case IV_EVENT_MOTION:
		view_motion(app, event);
		break;
	case IV_EVENT_AXIS:
		view_axis(app, event);
		break;
	case IV_EVENT_ACTION:
		iv_app_action(app, (enum iv_action)event->action);
		break;
	case IV_EVENT_LEAVE:
		app->pressed = 0;
		app->dragging = 0;
		break;
	}
}

/*
 * Does what an action (of a menu, the titlebar, the context menu or a key)
 * asks.
 */
void
iv_app_action(
	struct iv_app *app,
	enum iv_action action)
{
	double middle_x;
	double middle_y;
	double fit;

	/* The area's middle, about which the zoom's steps go. */
	middle_x = (double)app->area_x + (double)app->area_width / 2.0;
	middle_y = (double)app->area_y + (double)app->area_height / 2.0;
	iv_log("ACTION %d", (int)action);

	/* An application of Open With is the window's to start on the image shown (ws128-p005). */
	if ((unsigned)action >= IV_ACTION_OPEN_WITH_FIRST &&
	    (unsigned)action < IV_ACTION_OPEN_WITH_FIRST + IV_OPENERS) {
		if (app->has_image)
			app->want_open_with = (int)((unsigned)action - IV_ACTION_OPEN_WITH_FIRST);
		app->dirty = 1;
		return;
	}

	/* Each action. */
	switch (action) {
	case IV_ACTION_OPEN:
		open_chooser(app);
		break;
	case IV_ACTION_CLOSE:
		/* Close shows nothing; on an empty window it closes the window. */
		if (app->has_image) {
			iv_app_close_image(app);
		} else {
			app->want_close = 1;
		}

		break;
	case IV_ACTION_QUIT:
		app->want_close = 1;
		break;
	case IV_ACTION_FIT:
		/* Fitted again, animated from where the view is. */
		if (!app->has_image)
			break;
		fit = iv_app_fit_scale(app);
		iv_app_zoom_at(app, fit, middle_x, middle_y, 1);
		app->animation.to_x = 0.0;
		app->animation.to_y = 0.0;
		app->fit = 1;
		break;
	case IV_ACTION_ACTUAL:
		iv_app_zoom_at(app, 1.0, middle_x, middle_y, 1);
		break;
	case IV_ACTION_ZOOM_IN:
		view_zoom_step(app, VIEW_ZOOM_STEP, middle_x, middle_y);
		break;
	case IV_ACTION_ZOOM_OUT:
		view_zoom_step(app, 1.0 / VIEW_ZOOM_STEP, middle_x, middle_y);
		break;
	case IV_ACTION_ROTATE_RIGHT:
		view_turn(app, 1);
		break;
	case IV_ACTION_ROTATE_LEFT:
		view_turn(app, 3);
		break;
	case IV_ACTION_FULLSCREEN:
		app->want_fullscreen = 1;
		break;
	case IV_ACTION_PREVIOUS:
		view_step(app, -1);
		break;
	case IV_ACTION_NEXT:
		view_step(app, 1);
		break;
	case IV_ACTION_FIRST:
		/* The first image of the folder, when it has any. */
		if (app->folder.count != 0)
			view_go(app, 0);
		break;
	case IV_ACTION_LAST:
		/* The last image of the folder, when it has any. */
		if (app->folder.count != 0)
			view_go(app, app->folder.count - 1U);
		break;
	case IV_ACTION_PLAY:
		/* Only an animated image plays and pauses. */
		if (app->has_image &&
		    app->current != NULL &&
		    app->current->frame_count > 1U) {
			/* Playing pauses and pausing plays, saying which. */
			if (app->playing) {
				app->playing = 0;
				iv_app_message(app, "Paused", 1200U);
			} else {
				app->playing = 1;
				iv_app_message(app, "Playing", 1200U);
			}

			/* The frame shown next is due a delay from now. */
			app->frame_due = app->now + app->current->delays[app->frame];
		}

		break;
	case IV_ACTION_ABOUT:
		iv_app_message(app, "Image Viewer \xe2\x80\x94 Kei", VIEW_MESSAGE_MS);
		break;
	case IV_ACTION_TRASH:
		/* The window moves the image shown to the trash, then tells the viewer (ws128-p005). */
		if (app->has_image && app->current != NULL)
			app->want_trash = 1;
		break;
	case IV_ACTION_SLIDESHOW:
		/* A slideshow starts, or stops (ws128-p005). */
		if (app->slideshow) {
			iv_app_slideshow(app, 0);
		} else {
			iv_app_slideshow(app, 1);
		}

		break;
	case IV_ACTION_NONE:
		break;
	}

	/* Every action asks for a frame. */
	app->dirty = 1;
}

/*
 * Moves time on: the zoom's animation, the slide after a swipe, the frames
 * of an animated image, and the chip and the message going.
 *
 * Returns how many milliseconds until something is due (-1 for nothing).
 */
int
iv_app_tick(
	struct iv_app *app,
	uint64_t now)
{
	struct iv_image *image;
	uint64_t elapsed;
	double progress;
	double eased;
	int due;
	int wait;

	/* The time, and nothing due yet. */
	app->now = now;
	due = -1;

	/* The zoom's animation: the view between its ends by the eased time. */
	if (app->animation.running) {
		elapsed = now - app->animation.start;
		progress = (double)elapsed / (double)IV_ANIMATION_MS;
		if (progress >= 1.0) {
			progress = 1.0;
			app->animation.running = 0;
		}

		/* The view between the animation's ends. */
		eased = view_ease(progress);
		app->scale = app->animation.from_scale + (app->animation.to_scale - app->animation.from_scale) * eased;
		app->scroll_x = app->animation.from_x + (app->animation.to_x - app->animation.from_x) * eased;
		app->scroll_y = app->animation.from_y + (app->animation.to_y - app->animation.from_y) * eased;
		iv_app_clamp(app);
		app->dirty = 1;
		due = VIEW_FRAME_MS;
	}

	/* The slide after a swipe: the image eases into place. */
	if (app->sliding) {
		elapsed = now - app->slide_start;
		progress = (double)elapsed / (double)VIEW_SLIDE_MS;
		if (progress >= 1.0) {
			progress = 1.0;
			app->sliding = 0;
			iv_log("SLIDE done");
		}

		/* The swipe's remaining distance, eased. */
		app->swipe = app->slide_from * (1.0 - view_ease(progress));
		app->dirty = 1;
		due = VIEW_FRAME_MS;
	}

	/* An animated image's next frame, when it is due. */
	image = app->current;
	if (app->has_image &&
	    image != NULL &&
	    image->frame_count > 1U &&
	    app->playing) {
		if (now >= app->frame_due) {
			app->frame = (app->frame + 1U) % image->frame_count;
			app->frame_serial++;
			app->frame_due = now + image->delays[app->frame];
			app->dirty = 1;
		}

		/* Waits no longer than the next frame. */
		wait = (int)(app->frame_due - now);
		if (due < 0 || wait < due)
			due = wait;
	}

	/* A slideshow goes on to the next image when it is due (ws128-p005). */
	view_slideshow_tick(app, now, &due);

	/* The chip fades and goes. */
	if (app->chip_until != 0) {
		if (now >= app->chip_until) {
			app->chip_until = 0;
			app->ui_dirty = 1;
			app->dirty = 1;
		} else {
			/* Each step of its fade is drawn, then its end. */
			wait = (int)(app->chip_until - now);
			if (wait <= (int)VIEW_CHIP_FADE_MS) {
				wait = VIEW_FRAME_MS;
				app->ui_dirty = 1;
				app->dirty = 1;
			} else {
				wait -= (int)VIEW_CHIP_FADE_MS;
			}

			/* Waits no longer than the chip's next step. */
			if (due < 0 || wait < due)
				due = wait;
		}
	}

	/* The message goes when its time is up. */
	if (app->message_until != 0) {
		if (now >= app->message_until) {
			app->message[0] = '\0';
			app->message_until = 0;
			app->ui_dirty = 1;
			app->dirty = 1;
		} else {
			/* Waits no longer than the message stays. */
			wait = (int)(app->message_until - now);
			if (due < 0 || wait < due)
				due = wait;
		}
	}

	/* Reports when something is due next. */
	return due;
}

/*
 * Decodes one neighbour of the image shown ahead (the next one first),
 * when nothing moves.  Returns 1 when it decoded one, 0 when nothing was
 * left to do.
 */
int
iv_app_prefetch(
	struct iv_app *app)
{
	/* Nothing ahead while the view moves, or without a folder. */
	if (!app->has_image ||
	    app->touching ||
	    app->pressed ||
	    app->animation.running ||
	    app->sliding ||
	    app->folder.count == 0)
		return 0;

	/* The next image, when there is one. */
	if (app->next == NULL && app->folder.index + 1 < app->folder.count) {
		app->next = view_decode(app, app->folder.index + 1U);
		return 1;
	}

	/* The previous image, when there is one. */
	if (app->previous == NULL && app->folder.index > 0) {
		app->previous = view_decode(app, app->folder.index - 1U);
		return 1;
	}

	/* Both neighbours are held. */
	return 0;
}

/*
 * Toggles between the fit and a closer look (100 %, or twice the fit when
 * that is larger) about a window point.
 */
void
iv_app_toggle_zoom(
	struct iv_app *app,
	double x,
	double y)
{
	double fit;
	double closer;

	/* Nothing to zoom without an image to show. */
	if (!app->has_image ||
	    app->current == NULL ||
	    app->current->error != 0)
		return;

	/* Zoomed: back to the fit. */
	if (!app->fit) {
		iv_app_action(app, IV_ACTION_FIT);
		return;
	}

	/* Fitted: closer, about the point. */
	fit = iv_app_fit_scale(app);
	closer = 2.0 * fit;
	if (closer < 1.0)
		closer = 1.0;
	iv_app_zoom_at(app, closer, x, y, 1);
}

/*
 * Writes a log line on standard error: IMAGEVIEW and the message.  The
 * tests wait for these lines.
 */
void
iv_log(
	const char *format,
	...)
{
	va_list arguments;

	/* The prefix, the message and the end of the line, at once. */
	fputs("IMAGEVIEW ", stderr);
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
	fflush(stderr);
}

/*
 * Reports a monotonic time in milliseconds (0 when the clock cannot be read).
 */
uint64_t
iv_clock(void)
{
	struct timespec now;
	int status;

	/* The monotonic clock. */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return 0U;

	/* Reports it in milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/*
 * Makes an image the one shown: the view fitted and upright, the frames
 * from the first, the chip shown.  keep_neighbours says the previous and
 * the next already hold the right images.
 */
static void
view_show(
	struct iv_app *app,
	struct iv_image *image,
	int keep_neighbours)
{
	/* The neighbours go, unless they are already the right ones. */
	if (!keep_neighbours) {
		view_free_image(&app->previous);
		view_free_image(&app->next);
	}

	/* The image replaces the one shown. */
	view_free_image(&app->current);
	app->current = image;
	app->has_image = 1;
	app->image_serial++;
	app->opened = 1;

	/* The view starts fitted and upright, and an animated image plays. */
	view_reset(app);
	app->frame = 0;
	app->playing = 0;
	if (image->frame_count > 1U) {
		app->playing = 1;
		app->frame_due = app->now + image->delays[0];
	}

	/* The chip says what is shown. */
	iv_app_show_chip(app);
	app->dirty = 1;
	app->ui_dirty = 1;

	/* Logs the image shown, which the tests wait for. */
	iv_log("SHOW path=%s index=%lu count=%lu width=%d height=%d frames=%lu error=%d",
	       image->path,
	       (unsigned long)app->folder.index,
	       (unsigned long)app->folder.count,
	       image->file_width,
	       image->file_height,
	       (unsigned long)image->frame_count,
	       image->error);
}

/* Shows one image of the folder by its place, using a neighbour already decoded when it is the one. */
static void
view_go(
	struct iv_app *app,
	size_t index)
{
	struct iv_image *image;

	/* Nowhere to go without the folder. */
	if (index >= app->folder.count)
		return;

	/* The next or the previous image, already decoded, keeps its neighbour. */
	if (index == app->folder.index + 1U && app->next != NULL) {
		view_free_image(&app->previous);
		app->previous = app->current;
		app->current = NULL;
		image = app->next;
		app->next = NULL;
		app->folder.index = index;
		view_show(app, image, 1);
		return;
	}

	/* The same going back. */
	if (index + 1U == app->folder.index && app->previous != NULL) {
		view_free_image(&app->next);
		app->next = app->current;
		app->current = NULL;
		image = app->previous;
		app->previous = NULL;
		app->folder.index = index;
		view_show(app, image, 1);
		return;
	}

	/* Another image is decoded now; the neighbours are decoded again later. */
	image = view_decode(app, index);
	if (image == NULL) {
		iv_app_message(app, "There is not enough memory to open that.", VIEW_MESSAGE_MS);
		return;
	}

	/* Succeeded: the image is shown. */
	app->folder.index = index;
	view_show(app, image, 0);
}

/* Goes to the next (1) or the previous (-1) image, saying so at either end. */
static void
view_step(
	struct iv_app *app,
	int direction)
{
	/* Only with images to go through. */
	if (!app->has_image || app->folder.count == 0)
		return;

	/* The folder may have changed since it was read. */
	view_refresh_folder(app);

	/* The ends are not passed. */
	if (direction > 0 && app->folder.index + 1U >= app->folder.count) {
		iv_app_message(app, "Last image", 1200U);
		return;
	}

	/* Nor the first image. */
	if (direction < 0 && app->folder.index == 0) {
		iv_app_message(app, "First image", 1200U);
		return;
	}

	/* The image that way. */
	if (direction > 0)
		view_go(app, app->folder.index + 1U);
	else
		view_go(app, app->folder.index - 1U);
}

/* Decodes the image at a place of the folder; NULL when memory runs out (an image that fails to decode still comes back, with its reason). */
static struct iv_image *
view_decode(
	struct iv_app *app,
	size_t index)
{
	char path[IV_PATH_MAX];
	struct iv_image *image;
	int error;

	/* The image's path. */
	error = iv_folder_path(&app->folder, index, path, sizeof(path));
	if (error != 0)
		return NULL;

	/* Room for the image. */
	image = calloc(1, sizeof(*image));
	if (image == NULL)
		return NULL;

	/* The image, decoded (or with the reason it could not be, which it carries). */
	(void)iv_image_load(image, path, app->max_dimension);

	/* Reports the image. */
	return image;
}

/* Frees an image held and forgets it. */
static void
view_free_image(
	struct iv_image **image)
{
	/* Nothing held. */
	if (*image == NULL)
		return;

	/* Its pixels, and the image. */
	iv_image_free(*image);
	free(*image);
	*image = NULL;
}

/*
 * Reads the folder again when it changed since it was read (an image
 * added or taken away), keeping the image shown where it now is.
 */
static void
view_refresh_folder(
	struct iv_app *app)
{
	struct stat status;
	int error;

	/* Unchanged folders are not read again. */
	error = stat(app->folder.directory, &status);
	if (error != 0 || status.st_mtime == app->folder.modified)
		return;

	/* Read again around the image shown; the neighbours may no longer be. */
	error = iv_folder_read(&app->folder, app->current->path);
	if (error != 0)
		return;

	/* The neighbours are found again from the new list. */
	view_free_image(&app->previous);
	view_free_image(&app->next);
	iv_log("FOLDER reread count=%lu index=%lu", (unsigned long)app->folder.count, (unsigned long)app->folder.index);
}

/* Puts the view back to its start: upright, fitted, nothing moving. */
static void
view_reset(
	struct iv_app *app)
{
	/* Upright and fitted, not scrolled, swiped or animated. */
	app->rotation = 0;
	app->fit = 1;
	app->scale = iv_app_fit_scale(app);
	app->scroll_x = 0.0;
	app->scroll_y = 0.0;
	app->swipe = 0.0;
	app->sliding = 0;
	app->animation.running = 0;
	app->pressed = 0;
	app->dragging = 0;
}

/* Turns the image by quarter turns clockwise, keeping the fit, or the place in the middle of the area. */
static void
view_turn(
	struct iv_app *app,
	int quarters)
{
	struct iv_place place;
	struct iv_place turned;
	double middle_x;
	double middle_y;
	double height;
	int step;

	/* Nothing to turn without an image to show. */
	if (!app->has_image ||
	    app->current == NULL ||
	    app->current->error != 0)
		return;

	/* The place in the middle, before the turn. */
	middle_x = (double)app->area_x + (double)app->area_width / 2.0;
	middle_y = (double)app->area_y + (double)app->area_height / 2.0;
	iv_app_place_at(app, middle_x, middle_y, &place);

	/* Each quarter turns the place with the image (the turned image's width is the height before). */
	for (step = 0; step < quarters; step++) {
		/* The height of the image as it is turned now. */
		height = (double)app->current->height;
		if ((app->rotation & 1U) != 0)
			height = (double)app->current->width;

		/* The place a quarter turn clockwise on. */
		turned.x = height - place.y;
		turned.y = place.x;
		place = turned;
		app->rotation = (app->rotation + 1U) & 3U;
	}

	/* A fitted image is fitted again; another keeps the place in the middle. */
	if (app->fit) {
		app->scale = iv_app_fit_scale(app);
		app->scroll_x = 0.0;
		app->scroll_y = 0.0;
	} else {
		iv_app_show_place(app, &place, middle_x, middle_y);
	}

	/* Within the image's bounds, with the chip saying so. */
	iv_app_clamp(app);
	iv_app_show_chip(app);
	app->dirty = 1;
	iv_log("TURN rotation=%u", app->rotation * 90U);
}

/* Zooms by a factor about a window point, animated. */
static void
view_zoom_step(
	struct iv_app *app,
	double factor,
	double x,
	double y)
{
	double target;
	double fit;

	/* From where an animation is headed, when one runs. */
	target = app->scale;
	if (app->animation.running)
		target = app->animation.to_scale;
	target *= factor;

	/* Out past the fit is the fit. */
	fit = iv_app_fit_scale(app);
	if (factor < 1.0 && target < fit) {
		iv_app_action(app, IV_ACTION_FIT);
		return;
	}

	/* The zoom, animated. */
	iv_app_zoom_at(app, target, x, y, 1);
}

/* Eases a progress (0 to 1) out: fast at first, slowing to its end. */
static double
view_ease(
	double progress)
{
	double rest;

	/* A cubic ease-out. */
	rest = 1.0 - progress;

	/* Reports the eased progress. */
	return 1.0 - rest * rest * rest;
}

/* Reports the window x of the content's left edge (centred when narrower than the area). */
static double
view_left(
	const struct iv_app *app)
{
	double width;

	/* A narrower content is centred across the area. */
	width = iv_app_content_width(app);
	if (width <= (double)app->area_width)
		return (double)app->area_x + ((double)app->area_width - width) / 2.0;

	/* A wider one is scrolled. */
	return (double)app->area_x - app->scroll_x;
}

/* Reports the window y of the content's top edge (centred when lower than the area). */
static double
view_top(
	const struct iv_app *app)
{
	double height;

	/* A lower content is centred down the area. */
	height = iv_app_content_height(app);
	if (height <= (double)app->area_height)
		return (double)app->area_y + ((double)app->area_height - height) / 2.0;

	/* A higher one is scrolled. */
	return (double)app->area_y - app->scroll_y;
}

/* Reports the smallest scale the zoom goes to (a share of the fit, which it springs back to). */
static double
view_min_scale(
	const struct iv_app *app)
{
	double fit;

	/* The fit, which the zoom springs back to when it is let go below it. */
	fit = iv_app_fit_scale(app);

	/* Reports the share of the fit. */
	return fit * VIEW_SCALE_UNDER;
}

/* Handles a key of the viewer's. */
static void
view_key(
	struct iv_app *app,
	const struct iv_event *event)
{
	int control;
	int shift;
	int toggle;

	/* Only presses (and repeats) do anything; each is logged for the tests. */
	if (!event->pressed)
		return;

	/* Logs the key for the tests. */
	iv_log("KEY key=%u modifiers=%u repeat=%d time=%llu", event->key, event->modifiers, event->repeat, (unsigned long long)event->time);

	/*
	 * A key held repeats only where more of the same makes sense (moving,
	 * zooming, going through the images); a toggle does not, since a late
	 * release (the compositor busy changing to the full screen) would
	 * toggle it back.
	 */
	toggle = view_toggle_key(event->key);
	if (event->repeat && toggle)
		return;

	/* Whether Ctrl is held, which the file's keys need. */
	control = 0;
	if ((event->modifiers & IV_MOD_CTRL) != 0)
		control = 1;

	/* Whether Shift is held, which turns the other way. */
	shift = 0;
	if ((event->modifiers & IV_MOD_SHIFT) != 0)
		shift = 1;

	/* Each key. */
	switch (event->key) {
	case IV_KEY_O:
		/* Ctrl+O opens the chooser. */
		if (control)
			iv_app_action(app, IV_ACTION_OPEN);
		break;
	case IV_KEY_W:
		/* Ctrl+W closes the image. */
		if (control)
			iv_app_action(app, IV_ACTION_CLOSE);
		break;
	case IV_KEY_Q:
		/* Ctrl+Q ends the viewer. */
		if (control)
			iv_app_action(app, IV_ACTION_QUIT);
		break;
	case IV_KEY_EQUAL:
	case IV_KEY_KP_PLUS:
		iv_app_action(app, IV_ACTION_ZOOM_IN);
		break;
	case IV_KEY_MINUS:
	case IV_KEY_KP_MINUS:
		iv_app_action(app, IV_ACTION_ZOOM_OUT);
		break;
	case IV_KEY_0:
		iv_app_action(app, IV_ACTION_FIT);
		break;
	case IV_KEY_1:
		iv_app_action(app, IV_ACTION_ACTUAL);
		break;
	case IV_KEY_R:
		/* Shift+R turns the other way. */
		if (shift)
			iv_app_action(app, IV_ACTION_ROTATE_LEFT);
		else
			iv_app_action(app, IV_ACTION_ROTATE_RIGHT);

		break;
	case IV_KEY_F:
	case IV_KEY_F11:
		iv_app_action(app, IV_ACTION_FULLSCREEN);
		break;
	case IV_KEY_ESCAPE:
		/* Esc stops a slideshow (which leaves the full screen it made), else leaves the full screen. */
		if (app->slideshow) {
			iv_app_slideshow(app, 0);
		} else if (app->fullscreen) {
			iv_app_action(app, IV_ACTION_FULLSCREEN);
		}

		break;
	case IV_KEY_DELETE:
		/* Delete moves the image to the trash (ws128-p005). */
		iv_app_action(app, IV_ACTION_TRASH);
		break;
	case IV_KEY_F5:
		/* F5 starts or stops a slideshow (ws128-p005). */
		iv_app_action(app, IV_ACTION_SLIDESHOW);
		break;
	case IV_KEY_SPACE:
		/* Space plays an animated image, and goes on otherwise. */
		if (app->has_image &&
		    app->current != NULL &&
		    app->current->frame_count > 1U)
			iv_app_action(app, IV_ACTION_PLAY);
		else
			iv_app_action(app, IV_ACTION_NEXT);
		break;
	case IV_KEY_PAGE_DOWN:
		iv_app_action(app, IV_ACTION_NEXT);
		break;
	case IV_KEY_PAGE_UP:
	case IV_KEY_BACKSPACE:
		iv_app_action(app, IV_ACTION_PREVIOUS);
		break;
	case IV_KEY_HOME:
		iv_app_action(app, IV_ACTION_FIRST);
		break;
	case IV_KEY_END:
		iv_app_action(app, IV_ACTION_LAST);
		break;
	case IV_KEY_LEFT:
		view_pan_key(app, -VIEW_PAN_STEP, 0.0, -1);
		break;
	case IV_KEY_RIGHT:
		view_pan_key(app, VIEW_PAN_STEP, 0.0, 1);
		break;
	case IV_KEY_UP:
		view_pan_key(app, 0.0, -VIEW_PAN_STEP, 0);
		break;
	case IV_KEY_DOWN:
		view_pan_key(app, 0.0, VIEW_PAN_STEP, 0);
		break;
	default:
		break;
	}
}

/* Tells whether a key toggles something (the full screen, the fit, a turn, the chooser...), so that it never repeats. */
static int
view_toggle_key(
	uint32_t key)
{
	/* The keys whose repeat would undo or overdo what they did. */
	switch (key) {
	case IV_KEY_ESCAPE:
	case IV_KEY_F:
	case IV_KEY_F5:
	case IV_KEY_F11:
	case IV_KEY_DELETE:
	case IV_KEY_R:
	case IV_KEY_0:
	case IV_KEY_1:
	case IV_KEY_O:
	case IV_KEY_W:
	case IV_KEY_Q:
	case IV_KEY_SPACE:
	case IV_KEY_ENTER:
	case IV_KEY_KP_ENTER:
	case IV_KEY_HOME:
	case IV_KEY_END:
		return 1;
	default:
		break;
	}

	/* The rest may repeat. */
	return 0;
}

/*
 * Moves a zoomed image by an arrow key; a key across at the edge (or on
 * an image that fits across) goes to the image that way instead.
 */
static void
view_pan_key(
	struct iv_app *app,
	double dx,
	double dy,
	int direction)
{
	double before_x;
	double before_y;

	/* Nothing to move without an image. */
	if (!app->has_image)
		return;

	/* Moves the view, within the image. */
	before_x = app->scroll_x;
	before_y = app->scroll_y;
	app->scroll_x += dx;
	app->scroll_y += dy;
	iv_app_clamp(app);
	app->dirty = 1;

	/* A key across that moved nothing goes to the next or the previous image. */
	if (direction != 0 &&
	    app->scroll_x == before_x &&
	    app->scroll_y == before_y)
		view_step(app, direction);
}

/* Handles a button: the Open button's click, a drag or a swipe, a double click, the context menu. */
static void
view_button(
	struct iv_app *app,
	const struct iv_event *event)
{
	double velocity;
	int x;
	int y;
	int width;
	int height;
	int distance_x;
	int distance_y;
	int swiping;

	/* The right button asks for the context menu where it was pressed. */
	if (event->button == IV_BUTTON_RIGHT) {
		if (event->pressed) {
			app->want_context = 1;
			app->context_x = event->x;
			app->context_y = event->y;
		}

		/* The right button does nothing else. */
		return;
	}

	/* Only the left button does anything else. */
	if (event->button != IV_BUTTON_LEFT)
		return;

	/* A press on the empty window's Open button opens the chooser. */
	if (!app->has_image) {
		iv_app_open_button(app, &x, &y, &width, &height);
		if (event->pressed &&
		    event->x >= x &&
		    event->x < x + width &&
		    event->y >= y &&
		    event->y < y + height)
			iv_app_action(app, IV_ACTION_OPEN);
		return;
	}

	/* A press starts following the pointer; a second one soon after in the same place is a double click. */
	if (event->pressed) {
		/* How far the press is from the last click; soon after and close by, it is a double click. */
		distance_x = abs(event->x - app->last_click_x);
		distance_y = abs(event->y - app->last_click_y);
		if (event->time - app->last_click <= VIEW_DOUBLE_CLICK_MS &&
		    distance_x <= VIEW_DOUBLE_CLICK_SLOP &&
		    distance_y <= VIEW_DOUBLE_CLICK_SLOP) {
			app->last_click = 0;
			iv_app_toggle_zoom(app, (double)event->x, (double)event->y);
			return;
		}

		/* A first click, remembered for a double click, starts following the pointer. */
		app->last_click = event->time;
		app->last_click_x = event->x;
		app->last_click_y = event->y;
		app->pressed = 1;
		app->dragging = 0;
		app->press_x = event->x;
		app->press_y = event->y;
		app->last_x = event->x;
		app->last_y = event->y;
		app->last_time = event->time;
		app->velocity_x = 0.0;
		app->press_scroll_x = app->scroll_x;
		app->press_scroll_y = app->scroll_y;
		app->sliding = 0;
		app->animation.running = 0;
		return;
	}

	/* Only a release of a press the viewer followed ends anything. */
	if (!app->pressed)
		return;

	/* A release ends the drag; a swipe goes to the next image or slides back. */
	app->pressed = 0;
	swiping = view_can_swipe(app);
	if (app->dragging && swiping) {
		/* A pointer that stopped before it let go flings nothing. */
		velocity = app->velocity_x;
		if (event->time - app->last_time > 100U)
			velocity = 0.0;
		iv_app_swipe_end(app, velocity, 1);
	}

	/* The pointer is followed no more. */
	app->dragging = 0;
}

/* Handles the pointer's motion: a drag moves a zoomed image, or swipes a fitted one; any motion shows the chip. */
static void
view_motion(
	struct iv_app *app,
	const struct iv_event *event)
{
	double dx;
	double dy;
	uint64_t elapsed;
	int moved_x;
	int moved_y;
	int swiping;

	/* The pointer moving over an image shows its chip. */
	if (app->has_image &&
	    !app->pressed)
		iv_app_show_chip(app);

	/* Nothing more without a press. */
	if (!app->pressed)
		return;

	/* A press becomes a drag once the pointer has gone a little way. */
	moved_x = abs(event->x - app->press_x);
	moved_y = abs(event->y - app->press_y);
	if (!app->dragging &&
	    moved_x < VIEW_DRAG_SLOP &&
	    moved_y < VIEW_DRAG_SLOP)
		return;

	/* The press is a drag from now until the release. */
	app->dragging = 1;

	/* The drag's velocity across, pixels a millisecond. */
	elapsed = event->time - app->last_time;
	if (elapsed > 0U)
		app->velocity_x = (double)(event->x - app->last_x) / (double)elapsed;
	app->last_x = event->x;
	app->last_y = event->y;
	app->last_time = event->time;

	/* A fitted image swipes across, resisting past the first and the last image. */
	dx = (double)(event->x - app->press_x);
	dy = (double)(event->y - app->press_y);
	swiping = view_can_swipe(app);
	if (swiping) {
		/* The image follows the pointer, a third as far past either end. */
		app->swipe = dx;
		if (app->folder.index == 0 && app->swipe > 0.0)
			app->swipe /= VIEW_SWIPE_RESIST;
		if (app->folder.index + 1U >= app->folder.count && app->swipe < 0.0)
			app->swipe /= VIEW_SWIPE_RESIST;
		app->dirty = 1;
		return;
	}

	/* A zoomed image follows the pointer. */
	app->scroll_x = app->press_scroll_x - dx;
	app->scroll_y = app->press_scroll_y - dy;
	iv_app_clamp(app);
	app->dirty = 1;
}

/* Handles the wheel: a zoom about the pointer (one step a notch). */
static void
view_axis(
	struct iv_app *app,
	const struct iv_event *event)
{
	double factor;
	double target;
	double fit;

	/* Nothing to zoom without an image to show, or without a turn of the wheel. */
	if (!app->has_image ||
	    app->current == NULL ||
	    app->current->error != 0 ||
	    event->scroll == 0)
		return;

	/* Down zooms out, up zooms in, a step a notch; at the fit's bottom the zoom stops. */
	factor = pow(VIEW_ZOOM_STEP, -(double)event->scroll / VIEW_WHEEL_STEP);
	target = app->scale * factor;
	fit = iv_app_fit_scale(app);
	if (factor < 1.0 && target < fit) {
		/* A zoomed image goes back to the fit; a fitted one stays. */
		if (!app->fit)
			iv_app_action(app, IV_ACTION_FIT);
		return;
	}

	/* The zoom about the pointer, at once (the wheel's steps come quickly). */
	iv_app_zoom_at(app, target, (double)event->x, (double)event->y, 0);
}

/* Tells whether a drag swipes: a fitted image of a folder with more than one image. */
static int
view_can_swipe(
	const struct iv_app *app)
{
	/* Only fitted, with somewhere to go. */
	if (!app->fit || app->folder.count < 2U)
		return 0;

	/* A swipe changes the image. */
	return 1;
}

/*
 * Asks for the file chooser (libkeiland's, which the window shows: main.c) at
 * the shown image's folder, or the home folder.
 */
static void
open_chooser(
	struct iv_app *app)
{
	char folder[IV_PATH_MAX];
	const char *home;

	/* The shown image's folder, or the home folder, or the root. */
	folder[0] = '\0';
	if (app->has_image && app->folder.directory[0] != '\0')
		snprintf(folder, sizeof(folder), "%s", app->folder.directory);

	/* Without an image, the home folder, or the root without a home. */
	if (folder[0] == '\0') {
		home = getenv("HOME");
		if (home == NULL || home[0] == '\0')
			home = "/";
		snprintf(folder, sizeof(folder), "%s", home);
	}

	/* The viewer waits for the chooser's answer (iv_app_chosen); the chooser starts in that folder. */
	snprintf(app->chooser_folder, sizeof(app->chooser_folder), "%s", folder);
	app->chooser_open = 1;
	iv_log("CHOOSER open folder=%s", app->chooser_folder);
}

/*
 * Moves a slideshow on: when the next image is due, the folder's next one
 * (the first after the last) is shown; due becomes the time to wait.
 */
static void
view_slideshow_tick(
	struct iv_app *app,
	uint64_t now,
	int *due)
{
	size_t index;
	int wait;

	/* Only while a slideshow runs on an image. */
	if (!app->slideshow || !app->has_image)
		return;

	/* The next image when its time has come. */
	if (now >= app->slideshow_due) {
		/* The folder may have changed; the image after the one shown, around at the end. */
		view_refresh_folder(app);
		index = app->folder.index + 1U;
		if (index >= app->folder.count)
			index = 0;

		/* A folder of one image keeps showing it. */
		if (app->folder.count > 1U && index != app->folder.index) {
			iv_log("SLIDESHOW next index=%lu", (unsigned long)index);
			view_go(app, index);
		}

		/* The step after this one. */
		app->slideshow_due = now + IV_SLIDESHOW_MS;
	}

	/* Waits no longer than the next step. */
	wait = (int)(app->slideshow_due - now);
	if (*due < 0 || wait < *due)
		*due = wait;
}
