/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Image Viewer (ws091): the parts that know neither Wayland nor Vulkan.
 *
 * An image (image.c) is decoded once into premultiplied 0xAARRGGBB words
 * on the CPU (transparent parts laid over a checkerboard, so that every
 * texel is opaque), with levels of halves for showing it small, and the
 * composed frames of an animated GIF.  The presenter (present.c) keeps the
 * levels in textures and draws the image as one quad where the view
 * (view.c) places it: its scale, where it is scrolled to and its quarter
 * turns.  Over it goes a canvas of the viewer's own words and cards,
 * drawn on the CPU (draw.c) only when they change.  The images of the
 * folder (folder.c) are gone through in order.
 */

#ifndef IMAGEVIEW_IMAGEVIEW_H
#define IMAGEVIEW_IMAGEVIEW_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct truetype_face;

/* Marks a parameter a callback's signature requires but the viewer does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* The window's size when it opens. */
#define IV_WIDTH		1000U
#define IV_HEIGHT		720U

/* The longest path the viewer keeps. */
#define IV_PATH_MAX		1024

/* The longest file name the folder keeps. */
#define IV_NAME_MAX		256

/* The most levels of halves an image has (32768 pixels down to one). */
#define IV_LEVELS_MAX		16U

/* The largest image kept at its full size, in pixels (larger ones are halved until they fit). */
#define IV_IMAGE_PIXELS_MAX	(64U * 1024U * 1024U)

/* The most memory the frames of one animated GIF may take, in bytes (more: its first frame only). */
#define IV_FRAMES_BYTES_MAX	(256U * 1024U * 1024U)

/* How many applications Open With offers, and the longest name of one shown (ws128-p005). */
#define IV_OPENERS		8
#define IV_OPENER_NAME		64

/* How long a slideshow shows each image, in milliseconds (ws128-p005). */
#define IV_SLIDESHOW_MS		3000U

/* The modifiers of an input. */
#define IV_MOD_SHIFT		0x01U
#define IV_MOD_CTRL		0x02U
#define IV_MOD_ALT		0x04U
#define IV_MOD_SUPER		0x08U

/* The evdev codes of the keys the viewer answers. */
#define IV_KEY_ESCAPE		1U
#define IV_KEY_1		2U
#define IV_KEY_0		11U
#define IV_KEY_MINUS		12U
#define IV_KEY_EQUAL		13U
#define IV_KEY_BACKSPACE	14U
#define IV_KEY_TAB		15U
#define IV_KEY_Q		16U
#define IV_KEY_W		17U
#define IV_KEY_R		19U
#define IV_KEY_O		24U
#define IV_KEY_ENTER		28U
#define IV_KEY_F		33U
#define IV_KEY_SPACE		57U
#define IV_KEY_KP_MINUS		74U
#define IV_KEY_KP_PLUS		78U
#define IV_KEY_F5		63U
#define IV_KEY_F11		87U
#define IV_KEY_KP_ENTER		96U
#define IV_KEY_HOME		102U
#define IV_KEY_UP		103U
#define IV_KEY_PAGE_UP		104U
#define IV_KEY_LEFT		105U
#define IV_KEY_RIGHT		106U
#define IV_KEY_END		107U
#define IV_KEY_DOWN		108U
#define IV_KEY_PAGE_DOWN	109U
#define IV_KEY_DELETE		111U

/* The pointer's buttons (evdev BTN_LEFT, BTN_RIGHT). */
#define IV_BUTTON_LEFT		0x110U
#define IV_BUTTON_RIGHT		0x111U

/*
 * The kinds of input the window queues for the viewer.
 */
enum iv_event_type {
	IV_EVENT_MOTION = 0,
	IV_EVENT_BUTTON,
	IV_EVENT_AXIS,
	IV_EVENT_LEAVE,
	IV_EVENT_KEY,
	IV_EVENT_ACTION
};

/*
 * One input: where the pointer was, what happened, the modifiers held and
 * when (milliseconds of the monotonic clock).  Only the fields of its type
 * are meaningful; repeat says a key press is the window's repeat of a key
 * held, not a press of its own.
 */
struct iv_event {
	enum iv_event_type type;
	int x;
	int y;
	uint32_t button;
	int pressed;
	int repeat;
	int scroll;
	uint32_t key;
	uint32_t modifiers;
	uint64_t time;
	uint32_t action;
};

/*
 * The things the viewer can be asked to do, by its menus, its titlebar,
 * its context menu or its keys.
 */
enum iv_action {
	IV_ACTION_NONE = 0,
	IV_ACTION_OPEN,
	IV_ACTION_CLOSE,
	IV_ACTION_QUIT,
	IV_ACTION_FIT,
	IV_ACTION_ACTUAL,
	IV_ACTION_ZOOM_IN,
	IV_ACTION_ZOOM_OUT,
	IV_ACTION_ROTATE_RIGHT,
	IV_ACTION_ROTATE_LEFT,
	IV_ACTION_FULLSCREEN,
	IV_ACTION_PREVIOUS,
	IV_ACTION_NEXT,
	IV_ACTION_FIRST,
	IV_ACTION_LAST,
	IV_ACTION_PLAY,
	IV_ACTION_ABOUT,
	IV_ACTION_TRASH,
	IV_ACTION_SLIDESHOW
};

/*
 * The actions of Open With's applications (ws128-p005): the first one's,
 * then one more for each further application, up to IV_OPENERS.
 */
#define IV_ACTION_OPEN_WITH_FIRST	100U

/*
 * A surface to draw on: the caller's pixels (premultiplied 0xAARRGGBB),
 * the words in a row, and the size.
 */
struct iv_canvas {
	uint32_t *pixels;
	size_t stride;
	int width;
	int height;
};

/*
 * One font file: its bytes (kept for the face), the face, and the size the
 * face is set to.
 */
struct iv_text {
	void *data;
	size_t size;
	struct truetype_face *face;
	unsigned pixels;
	unsigned char *scratch;
	size_t scratch_size;
};

/*
 * One level of an image: its opaque premultiplied pixels (0xAARRGGBB, a
 * row of width words after another) and its size.  Level 0 is the image
 * itself; each further level is half the one before, rounded up.
 */
struct iv_level {
	uint32_t *pixels;
	int width;
	int height;
};

/*
 * A decoded image, or the reason it could not be decoded.
 *
 * width and height are level 0's (the image turned as its EXIF orientation
 * says, and halved when it was larger than the viewer keeps);
 * file_width and file_height are the image's own size, turned the same
 * way, for the viewer to show.  An animated GIF has frame_count frames,
 * each composed whole at level 0's size, shown for its delay; level 0's
 * pixels are then the first frame, and the image has no further levels.
 * error is 0 for an image that can be shown, otherwise an errno value with
 * reason saying why in the viewer's words.
 */
struct iv_image {
	char path[IV_PATH_MAX];
	const char *format;
	int width;
	int height;
	int file_width;
	int file_height;
	int has_alpha;
	struct iv_level levels[IV_LEVELS_MAX];
	size_t level_count;
	uint32_t **frames;
	unsigned *delays;
	size_t frame_count;
	int error;
	char reason[128];
};

/*
 * The images of one folder: the folder, its image files' names in the
 * viewer's order, the one shown, and the folder's modification time when
 * it was read (a changed folder is read again).
 */
struct iv_folder {
	char directory[IV_PATH_MAX];
	char (*names)[IV_NAME_MAX];
	size_t count;
	size_t index;
	time_t modified;
};

/*
 * Where the image is drawn in the window this frame: the window pixel of
 * each of its corners (in the image's own order: top left, top right,
 * bottom left, bottom right, before any turn), the rectangle it is clipped
 * to (the area), the level whose texture is drawn, and whether it is
 * sampled to the nearest texel (a large enlargement) rather than smoothly.
 * visible is 0 when no image is drawn.
 */
struct iv_quad {
	int visible;
	float x[4];
	float y[4];
	int clip_x;
	int clip_y;
	int clip_width;
	int clip_height;
	size_t level;
	int nearest;
};

/*
 * An animated change of the view: from one scale and scroll to another,
 * starting at a time (milliseconds), for IV_ANIMATION_MS.
 */
struct iv_animation {
	int running;
	uint64_t start;
	double from_scale;
	double to_scale;
	double from_x;
	double from_y;
	double to_x;
	double to_y;
};

/*
 * The viewer: the images, the folder, how the image is shown, the pointer's
 * drag, the swipe to the next image, the chip and the message, whether the
 * file chooser is asked for (chooser_open: the viewer waits for the answer
 * of libkeiland's chooser, which the window shows starting at chooser_folder,
 * ws090-p008), and what the window is asked to do.
 *
 * The view: the image is turned by rotation quarter turns clockwise and
 * drawn at scale window pixels to an image pixel (level 0's).  The turned
 * image at that scale is the content, content_width by content_height
 * pixels; the area (area_*) is the part of the window images are shown
 * in.  A content narrower than the area is centred across it; a wider one
 * is scrolled, scroll_x being how far its left edge lies left of the
 * area's (0 to content_width - area_width), and likewise scroll_y down.
 * fit says the scale follows the window (the largest that shows the
 * whole image, never above 1).  swipe is how far a swipe has moved the
 * image across (fingers or the pointer at the fit), and a slide moves it
 * from slide_from to 0 after the next image came in.
 *
 * current is the image shown; previous and next hold the neighbours once
 * decoded ahead (NULL before).  image_serial changes whenever current
 * changes, and frame_serial whenever the frame of an animated GIF shown
 * changes, for the presenter.  ui_dirty says the canvas of words and
 * cards must be drawn again; dirty that a frame must be shown.  chip_*
 * is where the frame last drew the chip (a zero width: not drawn), for
 * the glass under it.
 *
 * want_trash asks the window to move the image shown to the trash, and
 * want_open_with (-1 for none) to open it in that application of Open
 * With; the window then tells the viewer (iv_app_removed) when the image
 * went (ws128-p005).  A slideshow (slideshow) goes on to the next image
 * every IV_SLIDESHOW_MS, the next step due at slideshow_due; it made the
 * window fill the screen when slideshow_screen is set, and leaves it again
 * when it stops.
 */
struct iv_app {
	struct iv_image *current;
	struct iv_image *previous;
	struct iv_image *next;
	struct iv_folder folder;
	int has_image;
	/* The presenter requested CPU image drawing after a texture allocation failed. */
	int cpu_image;
	unsigned image_serial;
	unsigned frame_serial;
	size_t frame;
	uint64_t frame_due;
	int playing;
	int window_width;
	int window_height;
	int area_x;
	int area_y;
	int area_width;
	int area_height;
	int fullscreen;
	int want_fullscreen;
	int glass;
	unsigned rotation;
	double scale;
	int fit;
	double scroll_x;
	double scroll_y;
	struct iv_animation animation;
	int pressed;
	int dragging;
	int press_x;
	int press_y;
	int last_x;
	int last_y;
	uint64_t last_time;
	double velocity_x;
	double press_scroll_x;
	double press_scroll_y;
	uint64_t last_click;
	int last_click_x;
	int last_click_y;
	double swipe;
	int sliding;
	double slide_from;
	uint64_t slide_start;
	char message[160];
	uint64_t message_until;
	uint64_t chip_until;
	int chip_x;
	int chip_y;
	int chip_width;
	int chip_height;
	int chooser_open;
	char chooser_folder[IV_PATH_MAX];
	int want_close;
	int want_trash;
	int want_open_with;
	int slideshow;
	int slideshow_screen;
	uint64_t slideshow_due;
	int want_context;
	int context_x;
	int context_y;
	int opened;
	int max_dimension;
	uint64_t now;
	int dirty;
	int ui_dirty;
	struct iv_text *text;
	int touching;
	int zooming;
};

/*
 * A place in the image: a point of the turned image, in its pixels at
 * scale 1.  Two fingers keep one under them while they zoom.
 */
struct iv_place {
	double x;
	double y;
};

/* How long an animated change of the view takes, in milliseconds. */
#define IV_ANIMATION_MS		200U

/*
 * The margin around the images in a window (not fullscreen), and the glass
 * card's inset from the window's edge: none, so that the card lines up with
 * the floating titlebar (ws090-p021).
 */
#define IV_MARGIN		24
#define IV_CARD_INSET		0
#define IV_CARD_RADIUS		18

/* The images (image.c). */
int iv_image_load(struct iv_image *image, const char *path, int max_dimension);
void iv_image_free(struct iv_image *image);
int iv_image_orientation(const unsigned char *data, size_t size);
int iv_image_is_name(const char *name);

/* The folder (folder.c). */
int iv_folder_read(struct iv_folder *folder, const char *path);
void iv_folder_release(struct iv_folder *folder);
int iv_folder_path(const struct iv_folder *folder, size_t index, char *path, size_t size);
int iv_folder_compare(const char *left, const char *right);

/* The view: layout, navigation and input (view.c). */
void iv_app_init(struct iv_app *app, struct iv_text *text, int width, int height);
void iv_app_release(struct iv_app *app);
int iv_app_open(struct iv_app *app, const char *path);
void iv_app_close_image(struct iv_app *app);
void iv_app_resize(struct iv_app *app, int width, int height);
void iv_app_event(struct iv_app *app, const struct iv_event *event);
void iv_app_action(struct iv_app *app, enum iv_action action);
int iv_app_tick(struct iv_app *app, uint64_t now);
int iv_app_prefetch(struct iv_app *app);
double iv_app_fit_scale(const struct iv_app *app);
double iv_app_content_width(const struct iv_app *app);
double iv_app_content_height(const struct iv_app *app);
void iv_app_clamp(struct iv_app *app);
void iv_app_zoom_to(struct iv_app *app, double scale);
void iv_app_place_at(const struct iv_app *app, double x, double y, struct iv_place *place);
void iv_app_show_place(struct iv_app *app, const struct iv_place *place, double x, double y);
void iv_app_zoom_at(struct iv_app *app, double scale, double x, double y, int animate);
void iv_app_settle(struct iv_app *app);
void iv_app_toggle_zoom(struct iv_app *app, double x, double y);
void iv_app_swipe_end(struct iv_app *app, double velocity, int may_turn);
void iv_app_quad(const struct iv_app *app, struct iv_quad *quad);
void iv_app_message(struct iv_app *app, const char *message, uint64_t duration);
void iv_app_show_chip(struct iv_app *app);
double iv_app_chip_opacity(const struct iv_app *app);
void iv_app_open_button(const struct iv_app *app, int *x, int *y, int *width, int *height);
void iv_app_layout(struct iv_app *app);
void iv_app_chosen(struct iv_app *app, const char *path);
void iv_app_removed(struct iv_app *app);
void iv_app_slideshow(struct iv_app *app, int on);

/* What the viewer shares with Files: the trash and Open With (share.c). */
int iv_share_trash(const char *path, char *trashed, size_t size);
int iv_share_openers(const char *path, const char *format, char names[][IV_OPENER_NAME], int capacity);
int iv_share_open_with(const char *path, const char *format, int index);

/* The canvas of words and cards (draw.c). */
void iv_draw(struct iv_app *app, struct iv_canvas *canvas);

/* The canvas (canvas.c). */
void iv_canvas_image(struct iv_canvas *canvas, const struct iv_level *image, const struct iv_quad *quad);
void iv_canvas_fill(struct iv_canvas *canvas, int x, int y, int width, int height, uint32_t color);
void iv_canvas_blend(struct iv_canvas *canvas, int x, int y, int width, int height, uint32_t color);
void iv_canvas_round(struct iv_canvas *canvas, int x, int y, int width, int height, int radius, uint32_t color);
void iv_canvas_mask(struct iv_canvas *canvas, int x, int y, const unsigned char *mask, int width, int height, uint32_t color);

/* The text (text.c). */
int iv_text_open(struct iv_text *text, const char *path);
void iv_text_close(struct iv_text *text);
int iv_text_width(struct iv_text *text, const char *string, unsigned pixels);
void iv_text_draw(struct iv_text *text, struct iv_canvas *canvas, int x, int baseline, const char *string, unsigned pixels, uint32_t color);

/* The log and the clock (view.c). */
void iv_log(const char *format, ...);
uint64_t iv_clock(void);

#endif
