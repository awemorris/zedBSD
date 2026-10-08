/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Photos' window, view and pictures (ws157-p003; photos.h is the
 * library's).
 *
 * The view (view.c) knows nothing of the window or the thread: it shows
 * the thumbnails and the picture the window gives it, lists the photos it
 * wants pictures of, and sets flags for what the window does for it
 * (saving the marks, reading the library again).  The thread (thumbs.c)
 * decodes the files (decode.c) and makes their pictures, one job at a
 * time, the photo shown whole before the thumbnails.
 */

#ifndef PHOTOS_APP_H
#define PHOTOS_APP_H

#include "photos.h"

/* A thumbnail's side, and the most thumbnails kept at once. */
#define PH_THUMB_SIDE		160
#define PH_THUMBS_KEPT		400U

/* The longest side of the picture of a photo shown whole. */
#define PH_VIEW_SIDE		2048

/* The most thumbnails the view asks for in a frame. */
#define PH_WANTS_MAX		48U

/* How long a photo shows in a slideshow. */
#define PH_SLIDE_US		3000000U

/* The actions of the menu, the keys and the buttons. */
#define PH_ACTION_QUIT		1U
#define PH_ACTION_REFRESH	2U
#define PH_ACTION_FAVORITE	3U
#define PH_ACTION_TURN_LEFT	4U
#define PH_ACTION_TURN_RIGHT	5U
#define PH_ACTION_SLIDESHOW	6U
#define PH_ACTION_BACK		7U
#define PH_ACTION_NEXT		8U
#define PH_ACTION_PREVIOUS	9U
#define PH_ACTION_OPEN		10U
#define PH_ACTION_IMPORT	11U	/* a photo chosen in the file chooser */
#define PH_ACTION_IMPORT_FOLDER	12U	/* the folder of a photo chosen in it */
#define PH_ACTION_ALBUM		13U	/* the card that adds the photo to an album */

/* What the view has of a photo's thumbnail. */
#define PH_THUMB_NONE		0
#define PH_THUMB_READY		1
#define PH_THUMB_FAILED		2

/*
 * A photo's thumbnail: what the view has of it, the picture (turned as
 * the photo is), the turns it was made with, and the frame it was last
 * drawn in (the oldest go first when too many are kept).
 */
struct ph_thumb {
	int state;
	struct kl_image image;
	int turns;
	uint64_t drawn;
};

/*
 * The view's state.
 *
 * What is listed: the list (PH_LIST_*) and its album, the scrolls of the
 * lists and of the grid, the photo chosen in the grid (-1 for none), and
 * the grid's columns as last drawn (for Up and Down).
 *
 * The photo shown whole (-1 for the grid), its picture and the turns it
 * was made with (none until the window gives it; failed when it cannot be
 * made), the slideshow and when it goes on.
 *
 * The thumbnails (one a photo), how many are kept and the frames drawn;
 * the photos wanted this frame.
 *
 * The card that adds a photo to an album: whether it shows, the photo it
 * adds, and the name of a new album typed in it.
 *
 * For the window: the marks or the albums changed (to save), the library
 * to read again, an import asked for (PH_ACTION_IMPORT or
 * PH_ACTION_IMPORT_FOLDER; 0 for none), the generation of the library (a
 * result of another is not taken).  The
 * notice at the bottom until a time, whether the window stands on glass,
 * and whether the program is to end.
 */
struct ph_view {
	int list;
	size_t album;
	struct kl_scroll lists_scroll;
	struct kl_scroll grid_scroll;
	long chosen;
	int columns;
	int reveal;

	long open;
	struct kl_image picture;
	int picture_turns;
	int picture_failed;
	int slideshow;
	uint64_t slide_at;

	struct ph_thumb *thumbs;
	size_t thumb_count;
	size_t kept;
	uint64_t frame;
	size_t wants[PH_WANTS_MAX];
	size_t want_count;

	int adding;
	long card_photo;
	struct kl_field album_name;

	int save;
	int refresh;
	unsigned import;
	unsigned generation;
	char notice[160];
	uint64_t notice_until;
	int glass;
	int quit;

	/*
	 * A drag of a photo out of the window (ws189-p003): a press held on a
	 * cell or on the photo shown whole arms it (the photo, where the
	 * pointer was, and the frame that saw the press held); far enough
	 * away it becomes the compositor's drag (main.c).
	 */
	int drag_armed;
	long drag_photo;
	double drag_press_x;
	double drag_press_y;
	uint64_t drag_frame;
};

/*
 * A picture the thread made: the photo, whether it is the whole picture
 * (else a thumbnail), the turns it was made with, the library's
 * generation, the error (0, or why it could not be made) and the picture.
 */
struct ph_result {
	size_t photo;
	int whole;
	int turns;
	unsigned generation;
	int error;
	struct kl_image image;
};

/* The pictures of the files (decode.c). */
int ph_decode(const char *path, struct kl_image *image);
int ph_thumbnail(const struct kl_image *picture, int side, struct kl_image *thumb);
int ph_fit(const struct kl_image *picture, int side, struct kl_image *fitted);
int ph_turn(const struct kl_image *picture, int turns, struct kl_image *turned);

/* The thread that makes them, the thumbnails kept in a cache folder (thumbs.c). */
int ph_worker_start(const char *cache);
void ph_worker_stop(void);
int ph_worker_queue(size_t photo, int whole, int turns, const char *path, const char *id, unsigned generation);
void ph_worker_drop_thumbs(void);
int ph_worker_take(struct ph_result *result);
int ph_worker_busy(void);

/* The view (view.c). */
int ph_view_init(struct ph_view *view);
void ph_view_release(struct ph_view *view);
int ph_view_reset(struct ph_view *view);
void ph_view_action(struct ph_view *view, unsigned action, uint64_t now_us);
void ph_view_key(struct ph_view *view, uint32_t key, unsigned modifiers, uint64_t now_us);
void ph_view_result(struct ph_view *view, struct ph_result *result);
void ph_view_open(struct ph_view *view, long photo, uint64_t now_us);
int ph_view_wait(const struct ph_view *view, uint64_t now_us);
int ph_view_tick(struct ph_view *view, uint64_t now_us);
void ph_view_draw(struct ph_view *view, struct kl_ui *ui, const struct kl_style *style, int width, int height, uint64_t now_us);
size_t ph_view_panels(const struct ph_view *view, int width, int height, struct kl_glass_panel *panels, size_t capacity);
void ph_view_notice(struct ph_view *view, const char *message, uint64_t now_us);
long ph_view_drag_check(struct ph_view *view, double x, double y);

#endif
