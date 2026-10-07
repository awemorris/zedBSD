/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PDF Viewer (ws079-p006): the parts that know neither Wayland nor Vulkan.
 *
 * The viewer draws each frame on the CPU into a canvas of premultiplied
 * 0xAARRGGBB words, which the presenter (present.c) shows with Vulkan.
 * Pages are interpreted by libpdf into display lists and rasterized by
 * libpdf's CPU rasterizer at the zoom in force; the rasters are kept in a
 * cache and copied into the frame.  The view (view.c) lays the pages out
 * in one of two modes: a vertical continuous scroll, or one page at a time
 * turned by a sideways drag (a swipe) or the keys.  ws079-p015: a sidebar
 * of page thumbnails on the left, and a card that asks for the password
 * of an encrypted document.  ws081-p012: the touch screen (touch.c) scrolls
 * with inertia, zooms with two fingers and swipes pages.
 */

#ifndef PDFVIEWER_VIEWER_H
#define PDFVIEWER_VIEWER_H

#include <stddef.h>
#include <stdint.h>

#include <pdf.h>

struct truetype_face;

/* The window's size when it opens. */
#define PV_WIDTH		1000U
#define PV_HEIGHT		760U

/* The longest path the viewer keeps. */
#define PV_PATH_MAX		1024

/* The longest password the password card takes, in bytes (revisions 5 and 6 of PDF encryption count 127). */
#define PV_PASSWORD_MAX		127

/* The modifiers of an input. */
#define PV_MOD_SHIFT		0x01U
#define PV_MOD_CTRL		0x02U
#define PV_MOD_ALT		0x04U
#define PV_MOD_SUPER		0x08U

/* The evdev codes of the keys the viewer answers. */
#define PV_KEY_ESCAPE		1U
#define PV_KEY_0		11U
#define PV_KEY_MINUS		12U
#define PV_KEY_EQUAL		13U
#define PV_KEY_BACKSPACE	14U
#define PV_KEY_TAB		15U
#define PV_KEY_Q		16U
#define PV_KEY_W		17U
#define PV_KEY_E		18U
#define PV_KEY_O		24U
#define PV_KEY_F		33U
#define PV_KEY_C		46U
#define PV_KEY_ENTER		28U
#define PV_KEY_SPACE		57U
#define PV_KEY_F3		61U
#define PV_KEY_F9		67U
#define PV_KEY_KP_MINUS		74U
#define PV_KEY_KP_PLUS		78U
#define PV_KEY_KP_ENTER		96U
#define PV_KEY_HOME		102U
#define PV_KEY_UP		103U
#define PV_KEY_PAGE_UP		104U
#define PV_KEY_LEFT		105U
#define PV_KEY_RIGHT		106U
#define PV_KEY_END		107U
#define PV_KEY_DOWN		108U
#define PV_KEY_PAGE_DOWN	109U

/* The pointer's left button (evdev BTN_LEFT). */
#define PV_BUTTON_LEFT		0x110U

/*
 * The kinds of input the window queues for the viewer.
 */
enum pv_event_type {
	PV_EVENT_MOTION = 0,
	PV_EVENT_BUTTON,
	PV_EVENT_AXIS,
	PV_EVENT_LEAVE,
	PV_EVENT_KEY,
	PV_EVENT_ACTION
};

/*
 * One input: where the pointer was, what happened, the modifiers held and
 * when (milliseconds of the monotonic clock), and for a key whether it is a
 * repeat of a key held (BUG-111).  Only the fields of its type are
 * meaningful.
 */
struct pv_event {
	enum pv_event_type type;
	int x;
	int y;
	uint32_t button;
	int pressed;
	int scroll;
	uint32_t key;
	uint32_t modifiers;
	uint64_t time;
	uint32_t action;
	int repeat;
};

/*
 * The things the viewer can be asked to do, by its menus, its titlebar or
 * its keys.
 */
enum pv_action {
	PV_ACTION_NONE = 0,
	PV_ACTION_OPEN,
	PV_ACTION_CLOSE,
	PV_ACTION_QUIT,
	PV_ACTION_ANNOTATE,
	PV_ACTION_PRINT,
	PV_ACTION_MODE_SCROLL,
	PV_ACTION_MODE_PAGE,
	PV_ACTION_FIT_WIDTH,
	PV_ACTION_FIT_PAGE,
	PV_ACTION_ZOOM_IN,
	PV_ACTION_ZOOM_OUT,
	PV_ACTION_ZOOM_RESET,
	PV_ACTION_PREVIOUS,
	PV_ACTION_NEXT,
	PV_ACTION_FIRST,
	PV_ACTION_LAST,
	PV_ACTION_THUMBNAILS,
	PV_ACTION_FIND,
	PV_ACTION_FIND_NEXT,
	PV_ACTION_FIND_PREVIOUS,
	PV_ACTION_COPY
};

/*
 * How the pages are laid out.
 */
enum pv_mode {
	PV_MODE_SCROLL = 0,
	PV_MODE_PAGE
};

/*
 * How the zoom is chosen: to fit the width, to fit the page, or a scale
 * the user set.
 */
enum pv_fit {
	PV_FIT_WIDTH = 0,
	PV_FIT_PAGE,
	PV_FIT_CUSTOM
};

/*
 * A surface to draw on: the caller's pixels (premultiplied 0xAARRGGBB),
 * the words in a row, and the size.
 */
struct pv_canvas {
	uint32_t *pixels;
	size_t stride;
	int width;
	int height;
};

/*
 * One font file: its bytes (kept for the face), the face, and the size the
 * face is set to.
 */
struct pv_text {
	void *data;
	size_t size;
	struct truetype_face *face;
	unsigned pixels;
	unsigned char *scratch;
	size_t scratch_size;
};

/*
 * One page of the open document: its shown size in points, its display
 * list once interpreted (NULL before), its raster at one scale once
 * drawn, and its thumbnail once drawn for the sidebar (NULL before).  used
 * orders the rasters for the cache's eviction.  ws128-p004: its words as
 * libpdf reads them (NULL: none), once read (text_read).
 */
struct pv_page {
	double width;
	double height;
	struct pdf_display_list *list;
	int list_error;
	uint32_t *raster;
	int raster_width;
	int raster_height;
	double raster_scale;
	uint64_t used;
	uint32_t *thumbnail;
	int thumbnail_width;
	int thumbnail_height;
	struct pdf_page_text *text;
	int text_read;
};

/*
 * The open document: its path, libpdf's document, its pages, the bytes the
 * page rasters and the thumbnails take, and the flags of the pages drawn
 * so far.
 */
struct pv_document {
	char path[PV_PATH_MAX];
	struct pdf_document *document;
	struct pv_page *pages;
	size_t count;
	double widest;
	double tallest;
	size_t raster_bytes;
	size_t thumbnail_bytes;
	uint64_t clock;
	unsigned flags;
};

/*
 * The viewer: the document, how it is laid out and where the view is, the
 * pointer's drag or swipe, the page turn in progress, the message shown,
 * whether the file chooser is asked for, and what the window is asked to do.
 *
 * scroll_y is the top of the view in the laid-out document, in pixels (the
 * scroll mode's whole column of pages, or the page mode's one page);
 * scroll_x is the left of the view when the pages are wider than it.
 * swipe is how far the page of the page mode is dragged sideways, and a
 * turn moves it from turn_from to turn_to between turn_start and
 * turn_start + PV_TURN_MS.  dirty says the frame must be drawn again.
 * shown_flags gathers the display-list flags (PDF_DISPLAY_*) of the pages
 * the last frame drew, which decide whether the frame says that some
 * content could not be shown; notice_shown is whether it last said so.
 *
 * window_width is the window's width; width is the part the pages are
 * laid out in, the window less the sidebar while it is shown (thumbnails
 * asks for it, and a document is open).  thumbnail_scroll is the top of the
 * sidebar's view of its column of thumbnails, in pixels, and
 * thumbnail_followed the page it last scrolled into view; a press in the
 * sidebar (thumbnail_pressed) chooses a page unless it drags the column.
 *
 * asking_password says the password card is shown for password_path, an
 * encrypted document the empty password did not open; password holds what
 * has been typed (never logged, cleared after each try), and
 * password_wrong says the last try was refused.
 *
 * ws081-p012: touching says fingers are on the pages or the content glides
 * after them (touch.c), and zooming that two fingers are changing the zoom:
 * while zooming, the frame shows the pages' rasters stretched to the new
 * scale instead of drawing them again, until the fingers stop.
 *
 * BUG-259: resizing says the window's size changed at resized_at (the
 * view's clock) and has not stayed still long enough since; the frame
 * shows the pages' rasters stretched to the new scale, as while zooming,
 * and pv_app_tick draws them again once it settles.
 *
 * ws090-p008: choosing says the viewer waits for the answer of libkeiland's
 * file chooser, which the window shows starting at chooser_folder
 * (pv_app_chosen takes the answer).  keyboard_right and keyboard_bottom are how much of the
 * window the on-screen keyboard covers from its right and its bottom edge
 * (0 without it); the password card stays in the part it leaves.
 *
 * ws128-p004 (find.c): the words Find looks for, the place found (its
 * page, first character and length; find_found: one is shown), and whether
 * the titlebar's find field is asked to take the keyboard (main.c); the
 * selection (a drag under way, whether it moved, whether there is one, its
 * page and its two ends, characters of the page's text); and the words
 * copied that main.c puts on the clipboard (copy_text, malloc'd, NULL when
 * none waits).
 */
struct pv_app {
	struct pv_document document;
	int has_document;
	enum pv_mode mode;
	enum pv_fit fit;
	double zoom;
	int window_width;
	int width;
	int height;
	double scroll_x;
	double scroll_y;
	size_t page;
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
	double swipe;
	int turning;
	double turn_from;
	double turn_to;
	int turn_direction;
	uint64_t turn_start;
	double wheel;
	char message[256];
	uint64_t message_until;
	uint64_t indicator_until;
	int choosing;
	char chooser_folder[PV_PATH_MAX];
	int want_close;
	int want_annotate;
	int want_print;
	int opened;
	uint64_t now;
	int dirty;
	uint64_t turn_at;
	unsigned shown_flags;
	int notice_shown;
	int thumbnails;
	double thumbnail_scroll;
	size_t thumbnail_followed;
	int thumbnail_pressed;
	int thumbnail_dragging;
	int thumbnail_press_y;
	double thumbnail_press_scroll;
	int asking_password;
	char password_path[PV_PATH_MAX];
	char password[PV_PASSWORD_MAX + 1];
	size_t password_length;
	int password_wrong;
	struct pv_text *text;
	int touching;
	int zooming;
	int resizing;
	uint64_t resized_at;
	int keyboard_right;
	int keyboard_bottom;
	char find_query[256];
	int find_found;
	size_t find_page;
	size_t find_from;
	size_t find_length;
	int want_find_focus;
	int selecting;
	int select_moved;
	int has_selection;
	size_t select_page;
	size_t select_anchor;
	size_t select_caret;
	char *copy_text;
	size_t copy_length;
};

/*
 * A place in the document: a page, and a point on it in points from its
 * top left corner.  Two fingers keep one under them while they zoom.
 */
struct pv_place {
	size_t page;
	double x;
	double y;
};

/* The document and its page cache (document.c). */
int pv_document_open(struct pv_document *document, const char *path, const char *password);
void pv_document_close(struct pv_document *document);
int pv_document_raster(struct pv_document *document, size_t index, double scale, const struct pv_page **page);
void pv_document_trim(struct pv_document *document, size_t keep_first, size_t keep_last);
int pv_document_thumbnail(struct pv_document *document, size_t index, const struct pv_page **page);
void pv_document_trim_thumbnails(struct pv_document *document, size_t keep_first, size_t keep_last);

/* The view: layout, navigation and input (view.c). */
void pv_app_init(struct pv_app *app, struct pv_text *text, int width, int height);
void pv_app_release(struct pv_app *app);
int pv_app_open(struct pv_app *app, const char *path);
void pv_app_close_document(struct pv_app *app);
void pv_app_resize(struct pv_app *app, int width, int height);
void pv_app_event(struct pv_app *app, const struct pv_event *event);
void pv_app_action(struct pv_app *app, enum pv_action action);
int pv_app_tick(struct pv_app *app, uint64_t now);
int pv_app_prefetch(struct pv_app *app);
size_t pv_app_current_page(const struct pv_app *app);
double pv_app_scale(const struct pv_app *app, size_t index);
double pv_app_neighbour_distance(const struct pv_app *app, size_t neighbour);
double pv_app_page_top(const struct pv_app *app, size_t index);
double pv_app_content_height(const struct pv_app *app);
double pv_app_content_width(const struct pv_app *app);
void pv_app_message(struct pv_app *app, const char *message, uint64_t duration);
void pv_app_chosen(struct pv_app *app, const char *path);
int pv_app_sidebar_width(const struct pv_app *app);
void pv_app_clamp(struct pv_app *app);
void pv_app_zoom_to(struct pv_app *app, double scale);
double pv_app_page_left(const struct pv_app *app, size_t index);
void pv_app_place_at(const struct pv_app *app, double x, double y, struct pv_place *place);
void pv_app_show_place(struct pv_app *app, const struct pv_place *place, double x, double y);
void pv_app_swipe_end(struct pv_app *app, double velocity, int may_turn);
void pv_app_go_to(struct pv_app *app, size_t index);

/* Find and the selection (find.c, ws128-p004). */
void pv_find_text(struct pv_app *app, const char *query);
void pv_find_next(struct pv_app *app, int direction);
int pv_select_button(struct pv_app *app, const struct pv_event *event);
int pv_select_motion(struct pv_app *app, const struct pv_event *event);
void pv_select_copy(struct pv_app *app);
void pv_find_clear(struct pv_app *app);
void pv_find_draw(struct pv_app *app, struct pv_canvas *canvas, size_t index, int x, int y, double scale);
void pv_thumbnail_range(const struct pv_app *app, size_t *first, size_t *last);
void pv_thumbnail_place(const struct pv_app *app, size_t index, int *x, int *y, int *width, int *height);
void pv_password_layout(const struct pv_app *app, int *x, int *y, int *width, int *height);

/* The sizes of the layout, which the view and the frame share. */
#define PV_MARGIN		16
#define PV_GAP			16

/*
 * The sidebar of thumbnails: its width, the box a thumbnail fits in, the
 * height of one page's slot (the box, its number and the space around),
 * and the space above the first slot.  The sidebar is shown only in a
 * window wide enough to leave PV_SIDEBAR_ROOM for the pages.
 */
#define PV_SIDEBAR_WIDTH	168
#define PV_SIDEBAR_ROOM		240
#define PV_THUMBNAIL_WIDTH	112
#define PV_THUMBNAIL_HEIGHT	146
#define PV_THUMBNAIL_SLOT	186
#define PV_THUMBNAIL_TOP	10

/* The password card's size, and its buttons' size and inset from its corner. */
#define PV_PASSWORD_WIDTH	420
#define PV_PASSWORD_HEIGHT	214
#define PV_PASSWORD_BUTTON_WIDTH	96
#define PV_PASSWORD_BUTTON_HEIGHT	34
#define PV_PASSWORD_BUTTON_INSET	20

/* The frame (draw.c). */
void pv_draw(struct pv_app *app, struct pv_canvas *canvas);
void pv_draw_set_dark(int dark);
void pv_draw_set_accent(uint32_t accent, uint32_t ink);
uint32_t pv_draw_accent(void);

/* The canvas (canvas.c). */
void pv_canvas_fill(struct pv_canvas *canvas, int x, int y, int width, int height, uint32_t color);
void pv_canvas_blend(struct pv_canvas *canvas, int x, int y, int width, int height, uint32_t color);
void pv_canvas_round(struct pv_canvas *canvas, int x, int y, int width, int height, int radius, uint32_t color);
void pv_canvas_copy(struct pv_canvas *canvas, int x, int y, const uint32_t *pixels, int width, int height);
void pv_canvas_stretch(struct pv_canvas *canvas, int x, int y, int width, int height, const uint32_t *pixels, int source_width, int source_height);
void pv_canvas_mask(struct pv_canvas *canvas, int x, int y, const unsigned char *mask, int width, int height, uint32_t color);

/* The text (text.c). */
int pv_text_open(struct pv_text *text, const char *path);
void pv_text_close(struct pv_text *text);
int pv_text_width(struct pv_text *text, const char *string, unsigned pixels);
void pv_text_draw(struct pv_text *text, struct pv_canvas *canvas, int x, int baseline, const char *string, unsigned pixels, uint32_t color);

/* The log (view.c). */
void pv_log(const char *format, ...);
uint64_t pv_clock(void);

#endif
