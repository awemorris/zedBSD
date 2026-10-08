/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Photos' view (ws157-p003; app.h): at the left the lists -- Timeline
 * (every photo), Favorites, and each album with its first photo -- and at
 * the right the photos of the list chosen, a grid of square thumbnails
 * under a heading for each month, the newest first.  A click chooses a
 * photo; a double click, a tap or Enter shows it over the whole window,
 * where Left and Right go to the photos beside it in the list, Escape goes
 * back, and buttons mark it a favourite, turn it, or play the list as a
 * slideshow.
 *
 * The view asks the window for the pictures it has not got (the photos in
 * sight without a thumbnail, the photo shown whole) by listing them; the
 * window gives the pictures the thread made (ph_view_result).  At most
 * PH_THUMBS_KEPT thumbnails are kept: the ones drawn longest ago go.
 *
 * ws157-p005: the albums are the database's (an album links photos by
 * their ids); Photo > Add to Album... shows a card over the view with the
 * albums and a field for a new one.  An import (the grid's Import button,
 * File > Import...) is asked of the window, which shows the file chooser.
 */

#include "app.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The lists' column: its width's share of the window and its limits, the title's band, the rows. */
#define VIEW_SIDEBAR_SHARE	0.24
#define VIEW_SIDEBAR_MIN	200
#define VIEW_SIDEBAR_MAX	260
#define VIEW_TITLE		64
#define VIEW_ROW		44
#define VIEW_ALBUM_ROW		52
#define VIEW_ALBUM_LABEL	36

/* The grid: its heading, a month's heading, the cells' size and gap, the margin. */
#define VIEW_HEADER		72
#define VIEW_MONTH		40
#define VIEW_CELL		150
#define VIEW_CELL_GAP		6
#define VIEW_MARGIN		20

/* A photo shown whole: the bar at the top, the line at the bottom, the buttons beside it. */
#define VIEW_TOP		56
#define VIEW_BOTTOM		36
#define VIEW_SIDE_BUTTON	22

/* The cards on glass: the gap between them and their corner. */
#define VIEW_GAP		8
#define VIEW_CARD_RADIUS	16.0f

/* The text sizes: the title, a heading, a name, a detail. */
#define VIEW_TEXT_TITLE		22U
#define VIEW_TEXT_HEADING	16U
#define VIEW_TEXT_NAME		14U
#define VIEW_TEXT_SMALL		12U

/* How long a notice shows. */
#define VIEW_NOTICE_US		3000000U

/* The keys taken without a modifier: F (favourite), R (turn; Shift+R the other way), A (Add to Album), F5 (read again). */
#define VIEW_KEY_A		30U
#define VIEW_KEY_F		33U
#define VIEW_KEY_R		19U
#define VIEW_KEY_F5		63U

/* The widgets' ids. */
#define VIEW_ID_LISTS		1U
#define VIEW_ID_LIST		2U
#define VIEW_ID_GRID		3U
#define VIEW_ID_CELL		4U
#define VIEW_ID_SLIDESHOW	5U
#define VIEW_ID_BACK		6U
#define VIEW_ID_FAVORITE	7U
#define VIEW_ID_LEFT		8U
#define VIEW_ID_RIGHT		9U
#define VIEW_ID_PLAY		10U
#define VIEW_ID_PREVIOUS	11U
#define VIEW_ID_NEXT		12U
#define VIEW_ID_IMPORT		13U
#define VIEW_ID_PICTURE		14U

/* How far a press held on a photo moves before the photo is dragged out of the window (pixels, ws189-p003). */
#define VIEW_DRAG_DISTANCE	8
#define VIEW_ID_CARD_ALBUM	1000U
#define VIEW_ID_CARD_NAME	15U
#define VIEW_ID_CARD_NEW	16U
#define VIEW_ID_CARD_CANCEL	17U

/* The card of Add to Album: its width, a row's height, and the most albums it lists. */
#define VIEW_CARD_WIDTH		360
#define VIEW_CARD_ROW		36
#define VIEW_CARD_ALBUMS	8U

/* The ground of an opaque window, of the lists, of a photo shown whole, and of a cell waiting for its picture. */
#define VIEW_COLOR_SURFACE	kl_theme_choose(KL_RGB(0xffffff), KL_RGB(0x23272f))
#define VIEW_COLOR_SIDEBAR	kl_theme_choose(KL_RGB(0xf4f6f9), KL_RGB(0x1f232a))
#define VIEW_COLOR_DARK		KL_RGB(0x101114)
#define VIEW_COLOR_WAITING	kl_theme_choose(KL_RGB(0xe6e9ee), KL_RGB(0x31363f))
#define VIEW_COLOR_WHITE	KL_RGB(0xffffff)
#define VIEW_COLOR_PALE		KL_RGB(0xb4b9c2)
#define VIEW_COLOR_HEART	KL_RGB(0xff4d6d)

/* The most photos a list shows. */
#define VIEW_LIST_MAX		PH_PHOTOS_MAX

/* Where the two parts go. */
struct view_layout {
	struct kl_rect sidebar;
	struct kl_rect content;
};

/* The months' names. */
static const char *const view_months[12] = {
	"January", "February", "March", "April", "May", "June",
	"July", "August", "September", "October", "November", "December"
};

/* The photos of the list shown (the room is large; one view, one frame at a time). */
static size_t view_shown_indices[VIEW_LIST_MAX];

static void view_layout(const struct ph_view *view, int width, int height, struct view_layout *layout);
static size_t view_shown(const struct ph_view *view, size_t **indices);
static long view_target(const struct ph_view *view);
static void view_favorite(struct ph_view *view, uint64_t now_us);
static void view_turn(struct ph_view *view, int step, uint64_t now_us);
static void view_turn_image(struct kl_image *image, int step);
static void view_step(struct ph_view *view, int step, int wrap, uint64_t now_us);
static void view_slideshow(struct ph_view *view, uint64_t now_us);
static void view_back(struct ph_view *view);
static void view_choose(struct ph_view *view, int list, size_t album, uint64_t now_us);
static void view_sidebar(struct ph_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_list_row(struct ph_view *view, const struct kl_style *style, size_t row, const struct kl_rect *rect, unsigned hit);
static void view_grid(struct ph_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static int view_grid_columns(int width, int *cell);
static int view_grid_place(const size_t *indices, size_t count, int columns, int cell, size_t place, int *y);
static int view_month(ph_time when);
static void view_cell(struct ph_view *view, const struct kl_style *style, size_t photo, const struct kl_rect *rect, unsigned hit);
static void view_thumb(struct ph_view *view, const struct kl_style *style, size_t photo, int x, int y, int side, float radius);
static void view_whole(struct ph_view *view, struct kl_ui *ui, const struct kl_style *style, int width, int height, uint64_t now_us);
static int view_side_button(struct kl_ui *ui, const struct kl_style *style, uint32_t id, int cx, int cy, enum kl_icon icon);
static void view_heart(struct kl_canvas *canvas, float cx, float cy, float size, kl_color color);
static void view_date(ph_time when, char *text, size_t size);
static void view_want(struct ph_view *view, size_t photo);
static void view_keep(struct ph_view *view);
static void view_drop_thumbs(struct ph_view *view);
static void view_card(struct ph_view *view, struct kl_ui *ui, const struct kl_style *style, int width, int height, uint64_t now_us);
static void view_add_to(struct ph_view *view, size_t album, uint64_t now_us);
static void view_drag_arm(struct ph_view *view, struct kl_ui *ui, long photo);

/*
 * Makes the view's state: the timeline shown, nothing chosen or open, a
 * thumbnail's place for each photo of the library.  Returns 0 or an errno
 * value.
 */
int
ph_view_init(
	struct ph_view *view)
{
	int error;

	/* Nothing yet. */
	memset(view, 0, sizeof(*view));
	view->list = PH_LIST_TIMELINE;
	view->chosen = -1;
	view->open = -1;

	/* The lists' scroll, down only. */
	error = kl_scroll_init(&view->lists_scroll, KL_SCROLL_Y);
	if (error != 0)
		return error;

	/* The grid's scroll, down only. */
	error = kl_scroll_init(&view->grid_scroll, KL_SCROLL_Y);
	if (error != 0) {
		kl_scroll_release(&view->lists_scroll);
		return error;
	}

	/* The thumbnails' places. */
	error = ph_view_reset(view);
	if (error != 0) {
		kl_scroll_release(&view->grid_scroll);
		kl_scroll_release(&view->lists_scroll);
		return error;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Frees what the view's state holds.
 */
void
ph_view_release(
	struct ph_view *view)
{
	/* The pictures. */
	view_drop_thumbs(view);
	free(view->thumbs);
	view->thumbs = NULL;
	view->thumb_count = 0;
	kl_image_release(&view->picture);

	/* The scrolls. */
	kl_scroll_release(&view->grid_scroll);
	kl_scroll_release(&view->lists_scroll);
}

/*
 * Starts the view again on the library as it is now (after it was read
 * again): the pictures go, the timeline shows, nothing is chosen or open,
 * and a result made for the library before is not taken.  Returns 0 or
 * ENOMEM.
 */
int
ph_view_reset(
	struct ph_view *view)
{
	size_t count;

	/* The pictures of the library before. */
	view_drop_thumbs(view);
	free(view->thumbs);
	view->thumbs = NULL;
	view->thumb_count = 0;
	kl_image_release(&view->picture);

	/* A place for each photo now. */
	(void)ph_photos(&count);
	if (count > 0U) {
		view->thumbs = calloc(count, sizeof(view->thumbs[0]));
		if (view->thumbs == NULL)
			return ENOMEM;
	}

	/* Their count. */
	view->thumb_count = count;

	/* The timeline, from its top. */
	view->list = PH_LIST_TIMELINE;
	view->album = 0;
	view->chosen = -1;
	view->open = -1;
	view->picture_failed = 0;
	view->slideshow = 0;
	view->want_count = 0;
	view->generation++;
	kl_scroll_move_to(&view->grid_scroll, 0.0, 0.0, 0, 0);
	return 0;
}

/*
 * Carries out an action of the menu, a key or a button on the photo shown
 * whole, else the photo chosen.
 */
void
ph_view_action(
	struct ph_view *view,
	unsigned action,
	uint64_t now_us)
{
	/* Each action. */
	switch (action) {
	case PH_ACTION_QUIT:
		view->quit = 1;
		break;
	case PH_ACTION_REFRESH:
		view->refresh = 1;
		break;
	case PH_ACTION_FAVORITE:
		view_favorite(view, now_us);
		break;
	case PH_ACTION_TURN_LEFT:
		view_turn(view, 3, now_us);
		break;
	case PH_ACTION_TURN_RIGHT:
		view_turn(view, 1, now_us);
		break;
	case PH_ACTION_SLIDESHOW:
		view_slideshow(view, now_us);
		break;
	case PH_ACTION_BACK:
		view_back(view);
		break;
	case PH_ACTION_NEXT:
		view_step(view, 1, 0, now_us);
		break;
	case PH_ACTION_PREVIOUS:
		view_step(view, -1, 0, now_us);
		break;
	case PH_ACTION_OPEN:
		if (view->chosen >= 0)
			ph_view_open(view, view->chosen, now_us);
		break;
	case PH_ACTION_IMPORT:
	case PH_ACTION_IMPORT_FOLDER:
		view->import = action;
		ph_log("REQUEST import=%u", action);
		break;
	case PH_ACTION_ALBUM:
		view->card_photo = view_target(view);
		view->adding = view->card_photo >= 0;
		kl_field_set(&view->album_name, "");
		if (view->adding)
			ph_log("CARD album photo=%ld", view->card_photo);
		break;
	default:
		break;
	}
}

/*
 * Takes a key no widget took.  In the grid: the arrows move the choice,
 * Enter shows it whole.  Shown whole: Left and Right go to the photos
 * beside it, Escape goes back.  Both: Space plays the slideshow, F marks
 * a favourite, R turns right (Shift+R left), A adds to an album, F5 reads
 * the library again.  While the album card shows, Escape closes it.
 */
void
ph_view_key(
	struct ph_view *view,
	uint32_t key,
	unsigned modifiers,
	uint64_t now_us)
{
	size_t *indices;
	size_t count;
	long step;

	/* The keys with Ctrl, Alt or Super are the menu's. */
	if ((modifiers & (KL_MOD_CTRL | KL_MOD_ALT | KL_MOD_SUPER)) != 0U)
		return;

	/* The album card takes the keys: Escape closes it. */
	if (view->adding) {
		if (key == KL_KEY_ESC)
			view->adding = 0;
		return;
	}

	/* The keys of both. */
	switch (key) {
	case KL_KEY_SPACE:
		ph_view_action(view, PH_ACTION_SLIDESHOW, now_us);
		return;
	case VIEW_KEY_F:
		ph_view_action(view, PH_ACTION_FAVORITE, now_us);
		return;
	case VIEW_KEY_R:
		if ((modifiers & KL_MOD_SHIFT) != 0U)
			ph_view_action(view, PH_ACTION_TURN_LEFT, now_us);
		else
			ph_view_action(view, PH_ACTION_TURN_RIGHT, now_us);
		return;
	case VIEW_KEY_F5:
		ph_view_action(view, PH_ACTION_REFRESH, now_us);
		return;
	case VIEW_KEY_A:
		ph_view_action(view, PH_ACTION_ALBUM, now_us);
		return;
	default:
		break;
	}

	/* Shown whole. */
	if (view->open >= 0) {
		if (key == KL_KEY_LEFT)
			ph_view_action(view, PH_ACTION_PREVIOUS, now_us);
		else if (key == KL_KEY_RIGHT)
			ph_view_action(view, PH_ACTION_NEXT, now_us);
		else if (key == KL_KEY_ESC || key == KL_KEY_BACKSPACE)
			ph_view_action(view, PH_ACTION_BACK, now_us);
		return;
	}

	/* The grid: Enter shows the photo chosen. */
	if (key == KL_KEY_ENTER || key == KL_KEY_KPENTER) {
		ph_view_action(view, PH_ACTION_OPEN, now_us);
		return;
	}

	/* The arrows move the choice, a row being the columns drawn. */
	count = view_shown(view, &indices);
	if (count == 0U)
		return;
	step = 0;
	if (key == KL_KEY_LEFT)
		step = -1;
	else if (key == KL_KEY_RIGHT)
		step = 1;
	else if (key == KL_KEY_UP)
		step = -(long)view->columns;
	else if (key == KL_KEY_DOWN)
		step = (long)view->columns;
	if (step == 0)
		return;
	view_step(view, (int)step, 0, now_us);
}

/*
 * Takes a picture the thread made: a thumbnail (some kept ones go when
 * too many are), or the picture of the photo shown whole.  A result of a
 * library read before, or of a photo no longer shown, is let go; one made
 * before the photo was turned is turned now.  The result's picture is the
 * view's or freed.
 */
void
ph_view_result(
	struct ph_view *view,
	struct ph_result *result)
{
	struct ph_photo *photos;
	struct ph_thumb *thumb;
	size_t count;
	int step;

	/* A result of this library. */
	photos = ph_photos(&count);
	if (result->generation != view->generation || result->photo >= count || result->photo >= view->thumb_count) {
		kl_image_release(&result->image);
		return;
	}

	/* Turned as the photo is now. */
	step = (photos[result->photo].turns - result->turns) & 3;
	if (result->error == 0 && step != 0)
		view_turn_image(&result->image, step);

	/* The picture of the photo shown whole. */
	if (result->whole) {
		if ((long)result->photo != view->open || view->picture.pixels != NULL) {
			kl_image_release(&result->image);
			return;
		}

		/* Taken. */
		view->picture = result->image;
		view->picture_turns = photos[result->photo].turns;
		view->picture_failed = result->error != 0;
		ph_log("PICTURE photo=%lu error=%d width=%d height=%d", (unsigned long)result->photo, result->error, result->image.width,
		    result->image.height);
		return;
	}

	/* A thumbnail. */
	thumb = &view->thumbs[result->photo];
	if (thumb->state != PH_THUMB_NONE) {
		kl_image_release(&result->image);
		return;
	}

	/* One that cannot be made is not asked for again. */
	if (result->error != 0) {
		thumb->state = PH_THUMB_FAILED;
		ph_log("THUMB photo=%lu error=%d", (unsigned long)result->photo, result->error);
		return;
	}

	/* Kept, the oldest going when there are too many. */
	view_keep(view);
	thumb->image = result->image;
	thumb->turns = photos[result->photo].turns;
	thumb->state = PH_THUMB_READY;
	thumb->drawn = view->frame;
	view->kept++;
}

/*
 * Shows a photo whole (its picture asked of the window by the next
 * frame).
 */
void
ph_view_open(
	struct ph_view *view,
	long photo,
	uint64_t now_us)
{
	struct ph_photo *photos;
	size_t count;

	/* A photo of the library. */
	photos = ph_photos(&count);
	if (photo < 0 || (size_t)photo >= count)
		return;

	/* Its picture comes from the thread. */
	kl_image_release(&view->picture);
	view->picture_failed = 0;
	view->open = photo;
	view->chosen = photo;
	if (view->slideshow)
		view->slide_at = now_us + PH_SLIDE_US;
	ph_log("OPEN photo=%ld name=%s turns=%d favorite=%d", photo, photos[photo].name, photos[photo].turns, photos[photo].favorite);
}

/*
 * Reports how long the window may wait (ms) before the view needs a frame
 * by itself: the slideshow's next photo, the notice going, or -1.
 */
int
ph_view_wait(
	const struct ph_view *view,
	uint64_t now_us)
{
	uint64_t left;
	int wait;

	/* Nothing moves by itself. */
	wait = -1;

	/* The slideshow. */
	if (view->slideshow && view->open >= 0) {
		left = 0;
		if (view->slide_at > now_us)
			left = (view->slide_at - now_us) / 1000U + 1U;
		wait = (int)left;
	}

	/* A notice: until it goes. */
	if (view->notice[0] != '\0' && now_us < view->notice_until) {
		left = (view->notice_until - now_us) / 1000U + 1U;
		if (wait < 0 || left < (uint64_t)wait)
			wait = (int)left;
	}

	/* The time. */
	return wait;
}

/*
 * Moves the slideshow on when its time has come (after the last photo,
 * the first again).  Returns 1 when it moved (a frame is due).
 */
int
ph_view_tick(
	struct ph_view *view,
	uint64_t now_us)
{
	/* The slideshow's time. */
	if (!view->slideshow || view->open < 0 || now_us < view->slide_at)
		return 0;
	view_step(view, 1, 1, now_us);
	view->slide_at = now_us + PH_SLIDE_US;
	ph_log("SLIDE photo=%ld", view->open);
	return 1;
}

/*
 * Draws a frame of the view in a window of a size, between the caller's
 * kl_ui_begin and kl_ui_end, takes what the input did to its widgets, and
 * lists the pictures it wants.
 */
void
ph_view_draw(
	struct ph_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	int width,
	int height,
	uint64_t now_us)
{
	struct view_layout layout;
	struct kl_rect whole;

	/* A new frame: nothing wanted yet. */
	view->frame++;
	view->want_count = 0;

	/* A photo shown whole has the window. */
	if (view->open >= 0) {
		view_whole(view, ui, style, width, height, now_us);
		if (view->adding)
			view_card(view, ui, style, width, height, now_us);
		if (view->notice[0] != '\0' && now_us < view->notice_until)
			kl_chip(style, width / 2, height - VIEW_BOTTOM - 12, view->notice);
		return;
	}

	/* The ground: clear on glass (the desktop shows between the cards), else the surface. */
	whole.x = 0;
	whole.y = 0;
	whole.width = width;
	whole.height = height;
	if (view->glass)
		kl_canvas_clear(style->canvas);
	else
		kl_canvas_fill(style->canvas, &whole, VIEW_COLOR_SURFACE);

	/* The two parts; on glass each stands on a card of its own. */
	view_layout(view, width, height, &layout);
	if (view->glass) {
		kl_canvas_round(style->canvas, (float)layout.sidebar.x, (float)layout.sidebar.y, (float)layout.sidebar.width,
		    (float)layout.sidebar.height, VIEW_CARD_RADIUS, style->theme->glass_sidebar);
		kl_canvas_round(style->canvas, (float)layout.content.x, (float)layout.content.y, (float)layout.content.width,
		    (float)layout.content.height, VIEW_CARD_RADIUS, style->theme->glass_content);
	}

	/* The lists and the grid. */
	view_sidebar(view, ui, style, &layout.sidebar, now_us);
	view_grid(view, ui, style, &layout.content, now_us);

	/* The album card over it, and the notice over the bottom of the grid while it shows. */
	if (view->adding)
		view_card(view, ui, style, width, height, now_us);
	if (view->notice[0] != '\0' && now_us < view->notice_until)
		kl_chip(style, layout.content.x + layout.content.width / 2, layout.content.y + layout.content.height - 16, view->notice);
}

/*
 * Lists the parts of the view that stand on the compositor's glass (the two
 * cards; none while a photo is shown whole) into up to capacity panels;
 * returns how many there are.
 */
size_t
ph_view_panels(
	const struct ph_view *view,
	int width,
	int height,
	struct kl_glass_panel *panels,
	size_t capacity)
{
	struct view_layout layout;
	struct kl_rect cards[2];
	size_t count;
	size_t index;

	/* A photo shown whole is opaque. */
	if (view->open >= 0)
		return 0;

	/* The cards, as the frame draws them. */
	view_layout(view, width, height, &layout);
	cards[0] = layout.sidebar;
	cards[1] = layout.content;
	count = 0;
	for (index = 0; index < 2U && count < capacity; index++) {
		memset(&panels[count], 0, sizeof(panels[count]));
		panels[count].x = cards[index].x;
		panels[count].y = cards[index].y;
		panels[count].width = cards[index].width;
		panels[count].height = cards[index].height;
		panels[count].radius = (int32_t)VIEW_CARD_RADIUS;
		panels[count].kind = KL_GLASS_CARD;
		count++;
	}

	/* The panels listed. */
	return count;
}

/*
 * Shows a notice for a while (its words copied).
 */
void
ph_view_notice(
	struct ph_view *view,
	const char *message,
	uint64_t now_us)
{
	/* The words and until when. */
	(void)snprintf(view->notice, sizeof(view->notice), "%s", message);
	view->notice_until = now_us + VIEW_NOTICE_US;
}

/*
 * Tells whether the pointer moved far enough from a press held on a photo
 * for the photo to be dragged out of the window (ws189-p003): the photo,
 * once (the press is spent), or -1.
 */
long
ph_view_drag_check(
	struct ph_view *view,
	double x,
	double y)
{
	double dx;
	double dy;

	/* A press held in the last frame drawn. */
	if (!view->drag_armed || view->drag_frame != view->frame)
		return -1;

	/* Far enough. */
	dx = x - view->drag_press_x;
	dy = y - view->drag_press_y;
	if (dx * dx + dy * dy < (double)(VIEW_DRAG_DISTANCE * VIEW_DRAG_DISTANCE))
		return -1;

	/* Succeeded: the photo, the press spent. */
	view->drag_armed = 0;
	return view->drag_photo;
}

/* Lays out the lists at the left and the grid at the right; on glass, as cards with a gap between. */
static void
view_layout(
	const struct ph_view *view,
	int width,
	int height,
	struct view_layout *layout)
{
	int gap;
	int share;

	/* The gap: only cards on glass have one. */
	gap = 0;
	if (view->glass)
		gap = VIEW_GAP;

	/* The lists at the left, a share of the window within limits. */
	share = (int)((double)width * VIEW_SIDEBAR_SHARE);
	if (share < VIEW_SIDEBAR_MIN)
		share = VIEW_SIDEBAR_MIN;
	else if (share > VIEW_SIDEBAR_MAX)
		share = VIEW_SIDEBAR_MAX;
	layout->sidebar.x = 0;
	layout->sidebar.y = 0;
	layout->sidebar.width = share;
	layout->sidebar.height = height;

	/* The grid the rest. */
	layout->content.x = share + gap;
	layout->content.y = 0;
	layout->content.width = width - share - gap;
	layout->content.height = height;
}

/* Lists the photos of the list shown; reports how many. */
static size_t
view_shown(
	const struct ph_view *view,
	size_t **indices)
{
	/* The library's list. */
	*indices = view_shown_indices;
	return ph_library_list(view->list, view->album, view_shown_indices, VIEW_LIST_MAX);
}

/* The photo an action is on: the one shown whole, else the one chosen (-1 for none). */
static long
view_target(
	const struct ph_view *view)
{
	/* Shown whole first. */
	if (view->open >= 0)
		return view->open;
	return view->chosen;
}

/* Marks the photo a favourite, or not any more. */
static void
view_favorite(
	struct ph_view *view,
	uint64_t now_us)
{
	struct ph_photo *photos;
	size_t count;
	long photo;

	/* The photo. */
	photo = view_target(view);
	photos = ph_photos(&count);
	if (photo < 0 || (size_t)photo >= count)
		return;

	/* The other way, saved by the window (changed names the month the save writes). */
	photos[photo].favorite = !photos[photo].favorite;
	photos[photo].changed = 1;
	view->save = 1;
	ph_log("FAVORITE photo=%ld on=%d", photo, photos[photo].favorite);
	if (photos[photo].favorite)
		ph_view_notice(view, "Added to Favorites", now_us);
	else
		ph_view_notice(view, "Removed from Favorites", now_us);
}

/* Turns the photo by quarter turns clockwise (3 is a quarter to the left); its pictures are turned too. */
static void
view_turn(
	struct ph_view *view,
	int step,
	uint64_t now_us)
{
	struct ph_photo *photos;
	struct ph_thumb *thumb;
	size_t count;
	long photo;

	/* The photo. */
	(void)now_us;
	photo = view_target(view);
	photos = ph_photos(&count);
	if (photo < 0 || (size_t)photo >= count)
		return;

	/* Its turns, saved by the window in its month's line (the picture's file is not written). */
	photos[photo].turns = (photos[photo].turns + step) & 3;
	photos[photo].changed = 1;
	view->save = 1;
	ph_log("TURN photo=%ld turns=%d", photo, photos[photo].turns);

	/* Its thumbnail. */
	if ((size_t)photo < view->thumb_count) {
		thumb = &view->thumbs[photo];
		if (thumb->state == PH_THUMB_READY) {
			view_turn_image(&thumb->image, step);
			thumb->turns = photos[photo].turns;
		}
	}

	/* Its picture when it is shown whole. */
	if (photo == view->open && view->picture.pixels != NULL) {
		view_turn_image(&view->picture, step);
		view->picture_turns = photos[photo].turns;
	}
}

/* Turns a picture in place by quarter turns (left as it is when there is no room). */
static void
view_turn_image(
	struct kl_image *image,
	int step)
{
	struct kl_image turned;
	int error;

	/* A new picture in place of the old. */
	error = ph_turn(image, step, &turned);
	if (error != 0)
		return;
	kl_image_release(image);
	*image = turned;
}

/*
 * Goes to a photo beside the one shown whole, or moves the choice in the
 * grid, by a step in the list shown; wrapping goes round from the end.
 */
static void
view_step(
	struct ph_view *view,
	int step,
	int wrap,
	uint64_t now_us)
{
	size_t *indices;
	size_t count;
	size_t index;
	long current;
	long place;

	/* The list, and where the photo is in it (before the first when it is not). */
	count = view_shown(view, &indices);
	if (count == 0U)
		return;
	current = view_target(view);
	place = -1;
	for (index = 0; index < count; index++) {
		if ((long)indices[index] == current)
			place = (long)index;
	}

	/* The step, within the list or round it. */
	if (place < 0) {
		place = 0;
	} else {
		place += step;
		if (wrap && place >= (long)count)
			place = 0;
		if (place < 0)
			place = 0;
		if (place >= (long)count)
			place = (long)count - 1;
	}

	/* Shown whole, or chosen and brought into sight. */
	if (view->open >= 0) {
		if ((long)indices[place] != view->open)
			ph_view_open(view, (long)indices[place], now_us);
		return;
	}

	/* Chosen. */
	view->chosen = (long)indices[place];
	view->reveal = 1;
}

/* Starts the slideshow from the photo shown or chosen (the first of the list without one), or stops it. */
static void
view_slideshow(
	struct ph_view *view,
	uint64_t now_us)
{
	size_t *indices;
	size_t count;

	/* Playing: stops. */
	if (view->slideshow) {
		view->slideshow = 0;
		ph_log("SLIDESHOW on=0");
		return;
	}

	/* The photo it starts from. */
	count = view_shown(view, &indices);
	if (view->open < 0) {
		if (view->chosen >= 0)
			ph_view_open(view, view->chosen, now_us);
		else if (count > 0U)
			ph_view_open(view, (long)indices[0], now_us);
	}

	/* None to show. */
	if (view->open < 0)
		return;

	/* Each photo for a while. */
	view->slideshow = 1;
	view->slide_at = now_us + PH_SLIDE_US;
	ph_log("SLIDESHOW on=1 photo=%ld", view->open);
}

/* Goes back to the grid from the photo shown whole. */
static void
view_back(
	struct ph_view *view)
{
	/* The grid, the photo chosen there. */
	if (view->open < 0)
		return;
	ph_log("BACK photo=%ld", view->open);
	view->chosen = view->open;
	view->open = -1;
	view->slideshow = 0;
	view->reveal = 1;
	kl_image_release(&view->picture);
	view->picture_failed = 0;
}

/* Shows a list (an album's for PH_LIST_ALBUM) from its top. */
static void
view_choose(
	struct ph_view *view,
	int list,
	size_t album,
	uint64_t now_us)
{
	/* The same list. */
	if (view->list == list && (list != PH_LIST_ALBUM || view->album == album))
		return;

	/* Another, from the top, nothing chosen. */
	view->list = list;
	view->album = album;
	view->chosen = -1;
	kl_scroll_move_to(&view->grid_scroll, 0.0, 0.0, 0, now_us);
	ph_log("LIST list=%d album=%lu", list, (unsigned long)album);
}

/*
 * Draws the lists' column: the title, Timeline, Favorites, and the albums
 * under their label.
 */
static void
view_sidebar(
	struct ph_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	struct kl_rect list;
	struct kl_rect edge;
	struct kl_rect rect;
	unsigned hit;
	size_t albums;
	size_t rows;
	size_t row;
	int y;
	int height;

	/* The column's ground and its edge (on glass, its card is drawn already). */
	if (!view->glass) {
		kl_canvas_fill(style->canvas, area, VIEW_COLOR_SIDEBAR);
		edge = *area;
		edge.x = area->x + area->width - 1;
		edge.width = 1;
		kl_canvas_fill(style->canvas, &edge, style->theme->separator);
	}

	/* The title. */
	(void)kl_text_draw(style->text, style->canvas, area->x + 20, area->y + 40, "Photos", strlen("Photos"), VIEW_TEXT_TITLE, 1, style->theme->text);

	/* The viewport, which scrolls: the two lists, the label, the albums. */
	(void)ph_albums(&albums);
	rows = albums + 2U;
	list.x = area->x;
	list.y = area->y + VIEW_TITLE;
	list.width = area->width - 1;
	list.height = area->height - VIEW_TITLE;
	height = 2 * VIEW_ROW + VIEW_ALBUM_LABEL + (int)albums * VIEW_ALBUM_ROW + 8;
	kl_scroll_set_size(&view->lists_scroll, (double)list.width, (double)height, (double)list.width, (double)list.height);
	kl_ui_scroll_region(ui, VIEW_ID_LISTS, &list, &view->lists_scroll);
	kl_canvas_clip_push(style->canvas, &list);

	/* Each row. */
	y = list.y - (int)view->lists_scroll.y;
	for (row = 0; row < rows; row++) {
		/* Its place: the two lists, then the label over the albums. */
		if (row == 2U) {
			(void)kl_text_draw(style->text, style->canvas, list.x + 20, y + 26, "Albums", strlen("Albums"), VIEW_TEXT_SMALL, 1,
			    style->theme->text_secondary);
			y += VIEW_ALBUM_LABEL;
		}

		/* The row. */
		rect.x = list.x + 8;
		rect.y = y;
		rect.width = list.width - 16;
		rect.height = VIEW_ROW - 4;
		if (row >= 2U)
			rect.height = VIEW_ALBUM_ROW - 4;
		y += rect.height + 4;
		if (rect.y + rect.height < list.y || rect.y > list.y + list.height)
			continue;

		/* Its input: a click shows the list. */
		hit = kl_ui_hit(ui, VIEW_ID_LIST, (uint32_t)row, &rect);
		if ((hit & KL_HIT_CLICKED) != 0U) {
			if (row == 0U)
				view_choose(view, PH_LIST_TIMELINE, 0, now_us);
			else if (row == 1U)
				view_choose(view, PH_LIST_FAVORITES, 0, now_us);
			else
				view_choose(view, PH_LIST_ALBUM, row - 2U, now_us);
		}

		/* The row. */
		view_list_row(view, style, row, &rect, hit);
	}

	/* The rows' clip goes, and the bar shows while the list moves. */
	kl_canvas_clip_pop(style->canvas);
	(void)kl_scroll_draw_bars(&view->lists_scroll, style->canvas, &list, style->theme, now_us);
}

/* Draws one row of the lists: Timeline and Favorites with their marks, an album with its first photo. */
static void
view_list_row(
	struct ph_view *view,
	const struct kl_style *style,
	size_t row,
	const struct kl_rect *rect,
	unsigned hit)
{
	const struct ph_album *albums;
	size_t indices[1];
	size_t album_count;
	size_t count;
	char detail[32];
	const char *name;
	int selected;
	int side;
	int left;

	/* Chosen, or under the pointer. */
	albums = ph_albums(&album_count);
	selected = (row == 0U && view->list == PH_LIST_TIMELINE) || (row == 1U && view->list == PH_LIST_FAVORITES) ||
	    (row >= 2U && view->list == PH_LIST_ALBUM && view->album == row - 2U);
	if (selected)
		kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, 10.0f, style->theme->selection);
	else if ((hit & KL_HIT_HOT) != 0U)
		kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, 10.0f, style->theme->hover);

	/* Timeline: the pictures' mark and how many photos. */
	left = rect->x + 44;
	if (row == 0U) {
		kl_icon_draw(style->canvas, KL_ICON_PICTURES, (float)rect->x + 10.0f, (float)rect->y + 9.0f, 22.0f, style->theme->accent);
		(void)ph_photos(&count);
		(void)kl_text_draw_fit(style->text, style->canvas, left, rect->y + 26, "Timeline", VIEW_TEXT_NAME, 1, rect->width - 100, style->theme->text);
		(void)snprintf(detail, sizeof(detail), "%lu", (unsigned long)count);
		(void)kl_text_draw(style->text, style->canvas, rect->x + rect->width - 50, rect->y + 26, detail, strlen(detail), VIEW_TEXT_SMALL, 0,
		    style->theme->text_secondary);
		return;
	}

	/* Favorites: a heart and how many. */
	if (row == 1U) {
		view_heart(style->canvas, (float)rect->x + 21.0f, (float)rect->y + 20.0f, 18.0f, VIEW_COLOR_HEART);
		count = ph_library_list(PH_LIST_FAVORITES, 0, view_shown_indices, VIEW_LIST_MAX);
		(void)kl_text_draw_fit(style->text, style->canvas, left, rect->y + 26, "Favorites", VIEW_TEXT_NAME, 1, rect->width - 100, style->theme->text);
		(void)snprintf(detail, sizeof(detail), "%lu", (unsigned long)count);
		(void)kl_text_draw(style->text, style->canvas, rect->x + rect->width - 50, rect->y + 26, detail, strlen(detail), VIEW_TEXT_SMALL, 0,
		    style->theme->text_secondary);
		return;
	}

	/* An album: its first photo, its name and how many. */
	if (row - 2U >= album_count)
		return;
	side = rect->height - 8;
	count = ph_library_list(PH_LIST_ALBUM, row - 2U, indices, 1);
	if (count > 0U)
		view_thumb(view, style, indices[0], rect->x + 6, rect->y + 4, side, 6.0f);
	else
		kl_canvas_round(style->canvas, (float)rect->x + 6.0f, (float)rect->y + 4.0f, (float)side, (float)side, 6.0f, VIEW_COLOR_WAITING);
	name = albums[row - 2U].name;
	(void)snprintf(detail, sizeof(detail), "%lu photos", (unsigned long)albums[row - 2U].count);
	if (albums[row - 2U].count == 1U)
		(void)snprintf(detail, sizeof(detail), "1 photo");
	(void)kl_text_draw_fit(style->text, style->canvas, rect->x + side + 16, rect->y + 21, name, VIEW_TEXT_NAME, 1, rect->width - side - 24, style->theme->text);
	(void)kl_text_draw_fit(style->text, style->canvas, rect->x + side + 16, rect->y + 39, detail, VIEW_TEXT_SMALL, 0, rect->width - side - 24,
	    style->theme->text_secondary);
}

/*
 * Draws the grid's side: the heading (the list's name, how many, the
 * slideshow), then the photos under their months.
 */
static void
view_grid(
	struct ph_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	const struct ph_album *albums;
	struct ph_photo *photos;
	struct kl_rect button;
	struct kl_rect list;
	struct kl_rect cell_rect;
	size_t *indices;
	size_t album_count;
	size_t photo_count;
	size_t count;
	size_t index;
	char summary[48];
	char month_text[32];
	const char *title;
	const char *empty;
	unsigned flags;
	unsigned hit;
	int columns;
	int cell;
	int month;
	int last_month;
	int column;
	int y;
	int total;
	int place_y;
	int year;
	int month_number;
	int day;
	int clicked;
	int found;

	/* The list and its name. */
	count = view_shown(view, &indices);
	photos = ph_photos(&photo_count);
	albums = ph_albums(&album_count);
	title = "Timeline";
	if (view->list == PH_LIST_FAVORITES)
		title = "Favorites";
	else if (view->list == PH_LIST_ALBUM && view->album < album_count)
		title = albums[view->album].name;
	if (count == 1U)
		(void)snprintf(summary, sizeof(summary), "1 photo");
	else
		(void)snprintf(summary, sizeof(summary), "%lu photos", (unsigned long)count);

	/* The heading. */
	(void)kl_text_draw_fit(style->text, style->canvas, area->x + VIEW_MARGIN, area->y + 36, title, VIEW_TEXT_TITLE, 1, area->width - 200, style->theme->text);
	(void)kl_text_draw(style->text, style->canvas, area->x + VIEW_MARGIN, area->y + 56, summary, strlen(summary), VIEW_TEXT_SMALL, 0,
	    style->theme->text_secondary);
	button.width = kl_button_width(style, "Slideshow");
	button.height = 30;
	button.x = area->x + area->width - VIEW_MARGIN - button.width;
	button.y = area->y + 22;
	flags = KL_BUTTON_PRIMARY;
	if (count == 0U)
		flags |= KL_BUTTON_DISABLED;
	clicked = kl_button(ui, style, VIEW_ID_SLIDESHOW, &button, "Slideshow", flags);
	if (clicked && count > 0U) {
		kl_ui_clear_focus(ui);
		ph_view_action(view, PH_ACTION_SLIDESHOW, now_us);
	}

	/* Import, beside it: the folder of a photo chosen. */
	button.width = kl_button_width(style, "Import");
	button.x -= button.width + 8;
	clicked = kl_button(ui, style, VIEW_ID_IMPORT, &button, "Import", 0U);
	if (clicked) {
		kl_ui_clear_focus(ui);
		ph_view_action(view, PH_ACTION_IMPORT_FOLDER, now_us);
	}

	/* Nothing to show. */
	if (count == 0U) {
		empty = "Import photos with Import (a folder) or File > Import Photo...";
		if (view->list == PH_LIST_FAVORITES)
			empty = "Mark a photo a favorite (F) to see it here.";
		else if (view->list == PH_LIST_ALBUM)
			empty = "This album has no photos.";
		(void)kl_text_draw(style->text, style->canvas, area->x + VIEW_MARGIN, area->y + VIEW_HEADER + 40, empty, strlen(empty), VIEW_TEXT_NAME, 0,
		    style->theme->text_faint);
		return;
	}

	/* The viewport, which scrolls, and the cells' size. */
	list.x = area->x;
	list.y = area->y + VIEW_HEADER;
	list.width = area->width;
	list.height = area->height - VIEW_HEADER;
	columns = view_grid_columns(list.width, &cell);
	view->columns = columns;
	total = view_grid_place(indices, count, columns, cell, count, &place_y);

	/* The photo chosen brought into sight. */
	if (view->reveal) {
		view->reveal = 0;
		found = 0;
		for (index = 0; index < count && !found; index++) {
			if ((long)indices[index] != view->chosen)
				continue;
			found = 1;
			(void)view_grid_place(indices, count, columns, cell, index, &place_y);
			if ((double)place_y < view->grid_scroll.y)
				kl_scroll_move_to(&view->grid_scroll, 0.0, (double)place_y - VIEW_MONTH, 0, now_us);
			else if ((double)(place_y + cell) > view->grid_scroll.y + (double)list.height)
				kl_scroll_move_to(&view->grid_scroll, 0.0, (double)(place_y + cell + VIEW_CELL_GAP - list.height), 0, now_us);
		}
	}

	/* The content and its viewport. */
	kl_scroll_set_size(&view->grid_scroll, (double)list.width, (double)total, (double)list.width, (double)list.height);
	kl_ui_scroll_region(ui, VIEW_ID_GRID, &list, &view->grid_scroll);
	kl_canvas_clip_push(style->canvas, &list);

	/* Each photo, under its month's heading. */
	y = list.y + 4 - (int)view->grid_scroll.y;
	last_month = -1;
	column = 0;
	for (index = 0; index < count; index++) {
		/* A new month: a new row under its heading. */
		month = view_month(photos[indices[index]].taken);
		if (month != last_month) {
			if (column != 0)
				y += cell + VIEW_CELL_GAP;
			column = 0;
			last_month = month;
			if (y + VIEW_MONTH >= list.y && y <= list.y + list.height) {
				ph_time_split(photos[indices[index]].taken, &year, &month_number, &day);
				(void)snprintf(month_text, sizeof(month_text), "%s %d", view_months[month_number - 1], year);
				(void)kl_text_draw(style->text, style->canvas, list.x + VIEW_MARGIN, y + 28, month_text, strlen(month_text), VIEW_TEXT_HEADING, 1,
				    style->theme->text);
			}

			/* Below the heading. */
			y += VIEW_MONTH;
		}

		/* The cell; one out of the viewport is not drawn. */
		cell_rect.x = list.x + VIEW_MARGIN + column * (cell + VIEW_CELL_GAP);
		cell_rect.y = y;
		cell_rect.width = cell;
		cell_rect.height = cell;
		column++;
		if (column == columns) {
			column = 0;
			y += cell + VIEW_CELL_GAP;
		}

		/* Out of sight. */
		if (cell_rect.y + cell < list.y || cell_rect.y > list.y + list.height)
			continue;

		/* Its input: a click chooses, a double click or a tap shows it whole, a press held may become a drag out (ws189-p003). */
		hit = kl_ui_hit(ui, VIEW_ID_CELL, (uint32_t)indices[index], &cell_rect);
		if ((hit & KL_HIT_ACTIVE) != 0U)
			view_drag_arm(view, ui, (long)indices[index]);
		if ((hit & KL_HIT_CLICKED) != 0U)
			view->chosen = (long)indices[index];
		if ((hit & KL_HIT_CLICKED) != 0U && (hit & (KL_HIT_DOUBLE | KL_HIT_TOUCHED)) != 0U)
			ph_view_open(view, (long)indices[index], now_us);

		/* The cell. */
		view_cell(view, style, indices[index], &cell_rect, hit);
	}

	/* The cells' clip goes, and the bar shows while the grid moves. */
	kl_canvas_clip_pop(style->canvas);
	(void)kl_scroll_draw_bars(&view->grid_scroll, style->canvas, &list, style->theme, now_us);
}

/* Reports how many columns fit a width and the cells' side. */
static int
view_grid_columns(
	int width,
	int *cell)
{
	int inner;
	int columns;

	/* The cells near their size, filling the width. */
	inner = width - 2 * VIEW_MARGIN;
	columns = (inner + VIEW_CELL_GAP) / (VIEW_CELL + VIEW_CELL_GAP);
	if (columns < 1)
		columns = 1;
	*cell = (inner - (columns - 1) * VIEW_CELL_GAP) / columns;
	if (*cell < 16)
		*cell = 16;
	return columns;
}

/*
 * Lays out the grid as it is drawn: the top of the cell of a place in the
 * list (the content's top 0; any place past the end gives nothing), and
 * returns the content's height.
 */
static int
view_grid_place(
	const size_t *indices,
	size_t count,
	int columns,
	int cell,
	size_t place,
	int *y)
{
	struct ph_photo *photos;
	size_t photo_count;
	size_t index;
	int month;
	int last_month;
	int column;
	int at;

	/* Each photo, as view_grid places it. */
	photos = ph_photos(&photo_count);
	at = 4;
	last_month = -1;
	column = 0;
	*y = 0;
	for (index = 0; index < count; index++) {
		month = view_month(photos[indices[index]].taken);
		if (month != last_month) {
			if (column != 0)
				at += cell + VIEW_CELL_GAP;
			column = 0;
			last_month = month;
			at += VIEW_MONTH;
		}

		/* The place asked for. */
		if (index == place)
			*y = at;
		column++;
		if (column == columns) {
			column = 0;
			at += cell + VIEW_CELL_GAP;
		}
	}

	/* The height, the last row's and a margin. */
	if (column != 0)
		at += cell + VIEW_CELL_GAP;
	return at + VIEW_MARGIN;
}

/* The month of a time as one number (the year's months counted). */
static int
view_month(
	ph_time when)
{
	int year;
	int month;
	int day;

	/* Split. */
	ph_time_split(when, &year, &month, &day);
	return year * 12 + month - 1;
}

/* Draws a cell: its thumbnail, the choice around it and a heart on a favourite. */
static void
view_cell(
	struct ph_view *view,
	const struct kl_style *style,
	size_t photo,
	const struct kl_rect *rect,
	unsigned hit)
{
	struct ph_photo *photos;
	size_t count;

	/* The thumbnail. */
	view_thumb(view, style, photo, rect->x, rect->y, rect->width, 4.0f);

	/* Under the pointer: lighter. */
	if ((hit & KL_HIT_HOT) != 0U)
		kl_canvas_round(style->canvas, (float)rect->x, (float)rect->y, (float)rect->width, (float)rect->height, 4.0f, KL_RGBA(0xffffff, 0x30));

	/* A favourite: a heart in its corner. */
	photos = ph_photos(&count);
	if (photo < count && photos[photo].favorite) {
		view_heart(style->canvas, (float)rect->x + 15.0f, (float)(rect->y + rect->height) - 14.0f, 18.0f, VIEW_COLOR_WHITE);
		view_heart(style->canvas, (float)rect->x + 15.0f, (float)(rect->y + rect->height) - 14.0f, 14.0f, VIEW_COLOR_HEART);
	}

	/* Chosen: a ring of the accent. */
	if ((long)photo == view->chosen) {
		kl_canvas_round_border(style->canvas, (float)rect->x - 2.0f, (float)rect->y - 2.0f, (float)rect->width + 4.0f, (float)rect->height + 4.0f, 6.0f, 3.0f,
		    style->theme->accent);
	}
}

/* Draws a photo's thumbnail in a square (a grey square while it is made, asked for). */
static void
view_thumb(
	struct ph_view *view,
	const struct kl_style *style,
	size_t photo,
	int x,
	int y,
	int side,
	float radius)
{
	struct ph_thumb *thumb;

	/* A photo the view has a place for. */
	if (photo >= view->thumb_count)
		return;
	thumb = &view->thumbs[photo];
	thumb->drawn = view->frame;

	/* The picture. */
	if (thumb->state == PH_THUMB_READY) {
		kl_canvas_image(style->canvas, &thumb->image, (float)x, (float)y, (float)side, (float)side, radius, 1.0f);
		return;
	}

	/* Waiting, or a picture that cannot be made. */
	kl_canvas_round(style->canvas, (float)x, (float)y, (float)side, (float)side, radius, VIEW_COLOR_WAITING);
	if (thumb->state == PH_THUMB_FAILED) {
		kl_icon_draw(style->canvas, KL_ICON_PICTURE, (float)x + (float)side * 0.3f, (float)y + (float)side * 0.3f, (float)side * 0.4f,
		    style->theme->text_faint);
		return;
	}

	/* Asked for. */
	view_want(view, photo);
}

/*
 * Draws the photo shown whole: the bar at the top (Back, its name and
 * date, the slideshow, the turns, the favourite), the picture fitted to
 * the window, the buttons to the photos beside it, and where it is in the
 * list.
 */
static void
view_whole(
	struct ph_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	int width,
	int height,
	uint64_t now_us)
{
	struct ph_photo *photos;
	struct ph_thumb *thumb;
	const struct kl_image *image;
	struct kl_rect whole;
	struct kl_rect button;
	struct kl_rect picture_rect;
	unsigned hit;
	size_t *indices;
	size_t count;
	size_t shown;
	size_t index;
	char date[48];
	char where[48];
	const char *label;
	long photo;
	double scale;
	double across;
	double down;
	int area_width;
	int area_height;
	int draw_width;
	int draw_height;
	int clicked;
	int right;
	int place;

	/* The ground. */
	whole.x = 0;
	whole.y = 0;
	whole.width = width;
	whole.height = height;
	kl_canvas_fill(style->canvas, &whole, VIEW_COLOR_DARK);
	photos = ph_photos(&count);
	photo = view->open;
	if (photo < 0 || (size_t)photo >= count)
		return;

	/* The picture: the whole one, else its thumbnail for now. */
	image = NULL;
	if (view->picture.pixels != NULL) {
		image = &view->picture;
	} else if ((size_t)photo < view->thumb_count) {
		thumb = &view->thumbs[photo];
		thumb->drawn = view->frame;
		if (thumb->state == PH_THUMB_READY)
			image = &thumb->image;
	}

	/* Fitted to the space between the bars. */
	area_width = width - 2 * (VIEW_SIDE_BUTTON * 2 + 24);
	area_height = height - VIEW_TOP - VIEW_BOTTOM;
	if (image != NULL && area_width > 0 && area_height > 0) {
		across = (double)area_width / (double)image->width;
		down = (double)area_height / (double)image->height;
		scale = across;
		if (down < scale)
			scale = down;
		if (image == &view->picture && scale > 1.0 && view->picture.width < PH_VIEW_SIDE && view->picture.height < PH_VIEW_SIDE)
			scale = 1.0;
		draw_width = (int)((double)image->width * scale);
		draw_height = (int)((double)image->height * scale);
		kl_canvas_image(style->canvas, image, (float)((width - draw_width) / 2), (float)(VIEW_TOP + (area_height - draw_height) / 2), (float)draw_width,
		    (float)draw_height, 0.0f, 1.0f);

		/* A press held on the picture may become a drag of the photo out of the window (ws189-p003). */
		picture_rect.x = (width - draw_width) / 2;
		picture_rect.y = VIEW_TOP + (area_height - draw_height) / 2;
		picture_rect.width = draw_width;
		picture_rect.height = draw_height;
		hit = kl_ui_hit(ui, VIEW_ID_PICTURE, (uint32_t)photo, &picture_rect);
		if ((hit & KL_HIT_ACTIVE) != 0U)
			view_drag_arm(view, ui, photo);
	} else if (view->picture_failed) {
		(void)kl_text_draw(style->text, style->canvas, width / 2 - 120, height / 2, "This photo cannot be shown.", strlen("This photo cannot be shown."),
		    VIEW_TEXT_NAME, 0, VIEW_COLOR_PALE);
	}

	/* The picture asked for. */
	if (view->picture.pixels == NULL && !view->picture_failed)
		view_want(view, (size_t)photo);

	/* Back. */
	button.x = 12;
	button.y = 13;
	button.width = kl_button_width(style, "Back");
	button.height = 30;
	clicked = kl_button(ui, style, VIEW_ID_BACK, &button, "Back", 0U);
	if (clicked) {
		kl_ui_clear_focus(ui);
		view_back(view);
		return;
	}

	/* Its name and date. */
	view_date(photos[photo].taken, date, sizeof(date));
	(void)kl_text_draw_fit(style->text, style->canvas, button.x + button.width + 16, 26, photos[photo].name, VIEW_TEXT_NAME, 1, width / 3,
	    VIEW_COLOR_WHITE);
	(void)kl_text_draw(style->text, style->canvas, button.x + button.width + 16, 44, date, strlen(date), VIEW_TEXT_SMALL, 0, VIEW_COLOR_PALE);

	/* At the right: the favourite, the turns, the slideshow. */
	right = width - 12;
	label = "Favorite";
	if (photos[photo].favorite)
		label = "Unfavorite";
	button.width = kl_button_width(style, label);
	right -= button.width;
	button.x = right;
	clicked = kl_button(ui, style, VIEW_ID_FAVORITE, &button, label, 0U);
	if (clicked) {
		kl_ui_clear_focus(ui);
		view_favorite(view, now_us);
	}

	/* Rotate Right. */
	button.width = kl_button_width(style, "Rotate Right");
	right -= button.width + 8;
	button.x = right;
	clicked = kl_button(ui, style, VIEW_ID_RIGHT, &button, "Rotate Right", 0U);
	if (clicked) {
		kl_ui_clear_focus(ui);
		view_turn(view, 1, now_us);
	}

	/* Rotate Left. */
	button.width = kl_button_width(style, "Rotate Left");
	right -= button.width + 8;
	button.x = right;
	clicked = kl_button(ui, style, VIEW_ID_LEFT, &button, "Rotate Left", 0U);
	if (clicked) {
		kl_ui_clear_focus(ui);
		view_turn(view, 3, now_us);
	}

	/* The slideshow. */
	label = "Slideshow";
	if (view->slideshow)
		label = "Stop";
	button.width = kl_button_width(style, label);
	right -= button.width + 8;
	button.x = right;
	clicked = kl_button(ui, style, VIEW_ID_PLAY, &button, label, KL_BUTTON_PRIMARY);
	if (clicked) {
		kl_ui_clear_focus(ui);
		view_slideshow(view, now_us);
	}

	/* The photos beside it. */
	clicked = view_side_button(ui, style, VIEW_ID_PREVIOUS, VIEW_SIDE_BUTTON + 12, VIEW_TOP + area_height / 2, KL_ICON_BACK);
	if (clicked)
		view_step(view, -1, 0, now_us);
	clicked = view_side_button(ui, style, VIEW_ID_NEXT, width - VIEW_SIDE_BUTTON - 12, VIEW_TOP + area_height / 2, KL_ICON_FORWARD);
	if (clicked)
		view_step(view, 1, 0, now_us);

	/* Where it is in the list. */
	shown = view_shown(view, &indices);
	place = 0;
	for (index = 0; index < shown; index++) {
		if ((long)indices[index] == view->open)
			place = (int)index + 1;
	}

	/* The words. */
	if (place > 0)
		(void)snprintf(where, sizeof(where), "%d of %lu", place, (unsigned long)shown);
	else
		(void)snprintf(where, sizeof(where), "%s", photos[photo].name);
	draw_width = kl_text_width(style->text, where, strlen(where), VIEW_TEXT_SMALL, 0);
	(void)kl_text_draw(style->text, style->canvas, (width - draw_width) / 2, height - 14, where, strlen(where), VIEW_TEXT_SMALL, 0, VIEW_COLOR_PALE);
}

/* Draws a round button with an icon at its centre; nonzero when it was clicked. */
static int
view_side_button(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	int cx,
	int cy,
	enum kl_icon icon)
{
	struct kl_rect rect;
	unsigned hit;
	kl_color ground;

	/* Its square and input. */
	rect.x = cx - VIEW_SIDE_BUTTON;
	rect.y = cy - VIEW_SIDE_BUTTON;
	rect.width = VIEW_SIDE_BUTTON * 2;
	rect.height = VIEW_SIDE_BUTTON * 2;
	hit = kl_ui_hit(ui, id, 0, &rect);

	/* The circle, lighter under the pointer, and the icon. */
	ground = KL_RGBA(0xffffff, 0x22);
	if ((hit & KL_HIT_HOT) != 0U)
		ground = KL_RGBA(0xffffff, 0x44);
	kl_canvas_circle(style->canvas, (float)cx, (float)cy, (float)VIEW_SIDE_BUTTON, ground);
	kl_icon_draw(style->canvas, icon, (float)cx - 10.0f, (float)cy - 10.0f, 20.0f, VIEW_COLOR_WHITE);
	return (hit & KL_HIT_CLICKED) != 0U;
}

/* Draws a heart of a size centred on a point: two circles over a point. */
static void
view_heart(
	struct kl_canvas *canvas,
	float cx,
	float cy,
	float size,
	kl_color color)
{
	float points[6];
	float radius;

	/* The two lobes. */
	radius = size * 0.27f;
	kl_canvas_circle(canvas, cx - radius * 0.9f, cy - size * 0.12f, radius, color);
	kl_canvas_circle(canvas, cx + radius * 0.9f, cy - size * 0.12f, radius, color);

	/* The point below. */
	points[0] = cx - radius * 1.85f;
	points[1] = cy - size * 0.02f;
	points[2] = cx + radius * 1.85f;
	points[3] = cy - size * 0.02f;
	points[4] = cx;
	points[5] = cy + size * 0.42f;
	kl_canvas_polygon(canvas, points, 3, color);
}

/* Writes a date as its month's name, day and year with the time. */
static void
view_date(
	ph_time when,
	char *text,
	size_t size)
{
	long seconds;
	int year;
	int month;
	int day;

	/* The day and the time of day. */
	ph_time_split(when, &year, &month, &day);
	seconds = (long)(when % 86400);
	if (seconds < 0)
		seconds += 86400;
	(void)snprintf(text, size, "%s %d, %d  %02ld:%02ld", view_months[month - 1], day, year, seconds / 3600, seconds / 60 % 60);
}

/* Lists a photo whose picture the view wants (once a frame, as many as there is room for). */
static void
view_want(
	struct ph_view *view,
	size_t photo)
{
	size_t index;

	/* Already listed. */
	for (index = 0; index < view->want_count; index++) {
		if (view->wants[index] == photo)
			return;
	}

	/* Listed. */
	if (view->want_count < PH_WANTS_MAX) {
		view->wants[view->want_count] = photo;
		view->want_count++;
	}
}

/* Makes room for a thumbnail: when too many are kept, the one drawn longest ago (not this frame) goes. */
static void
view_keep(
	struct ph_view *view)
{
	struct ph_thumb *thumb;
	size_t oldest;
	size_t index;
	uint64_t drawn;

	/* Room left. */
	if (view->kept < PH_THUMBS_KEPT)
		return;

	/* The one drawn longest ago. */
	oldest = view->thumb_count;
	drawn = view->frame;
	for (index = 0; index < view->thumb_count; index++) {
		thumb = &view->thumbs[index];
		if (thumb->state == PH_THUMB_READY && thumb->drawn < drawn) {
			drawn = thumb->drawn;
			oldest = index;
		}
	}

	/* It goes (made again when it is drawn again). */
	if (oldest == view->thumb_count)
		return;
	kl_image_release(&view->thumbs[oldest].image);
	view->thumbs[oldest].state = PH_THUMB_NONE;
	view->kept--;
}

/* Frees the thumbnails kept. */
static void
view_drop_thumbs(
	struct ph_view *view)
{
	size_t index;

	/* Each one. */
	for (index = 0; index < view->thumb_count; index++) {
		kl_image_release(&view->thumbs[index].image);
		view->thumbs[index].state = PH_THUMB_NONE;
	}

	/* None kept. */
	view->kept = 0;
}

/*
 * Draws the card of Add to Album over the view: the albums (a click adds
 * the photo to one), a field for a new album's name with New Album (or
 * Enter), and Cancel.
 */
static void
view_card(
	struct ph_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	int width,
	int height,
	uint64_t now_us)
{
	struct ph_album *albums;
	struct kl_rect whole;
	struct kl_rect card;
	struct kl_rect row;
	char label[PH_NAME_MAX + 32];
	char shown_id[PH_ID_SIZE];
	size_t count;
	size_t shown;
	size_t index;
	size_t album;
	unsigned changes;
	int clicked;
	int error;
	int same;

	/* The view dimmed, the card in its middle. */
	albums = ph_albums(&count);
	shown = count;
	if (shown > VIEW_CARD_ALBUMS)
		shown = VIEW_CARD_ALBUMS;
	whole.x = 0;
	whole.y = 0;
	whole.width = width;
	whole.height = height;
	kl_canvas_fill(style->canvas, &whole, KL_RGBA(0x000000, 0x60));
	card.width = VIEW_CARD_WIDTH;
	card.height = 160 + (int)shown * VIEW_CARD_ROW;
	card.x = (width - card.width) / 2;
	card.y = (height - card.height) / 2;
	kl_canvas_shadow(style->canvas, (float)card.x, (float)card.y, (float)card.width, (float)card.height, 14.0f, 18.0f, KL_RGBA(0x000000, 0x50));
	kl_canvas_round(style->canvas, (float)card.x, (float)card.y, (float)card.width, (float)card.height, 14.0f, VIEW_COLOR_SURFACE);
	(void)kl_text_draw(style->text, style->canvas, card.x + 20, card.y + 34, "Add to Album", strlen("Add to Album"), VIEW_TEXT_HEADING, 1, style->theme->text);

	/* Each album (the first ones), a click adds the photo to it. */
	for (index = 0; index < shown; index++) {
		row.x = card.x + 16;
		row.y = card.y + 48 + (int)index * VIEW_CARD_ROW;
		row.width = card.width - 32;
		row.height = VIEW_CARD_ROW - 4;
		(void)snprintf(label, sizeof(label), "%s (%lu)", albums[index].name, (unsigned long)albums[index].count);
		clicked = kl_button(ui, style, VIEW_ID_CARD_ALBUM + (uint32_t)index, &row, label, 0U);
		if (clicked) {
			view_add_to(view, index, now_us);
			return;
		}
	}

	/* A new album's name, and New Album (Enter does the same). */
	row.x = card.x + 16;
	row.y = card.y + 56 + (int)shown * VIEW_CARD_ROW;
	row.width = card.width - 32 - 120;
	row.height = 30;
	changes = kl_field(ui, style, VIEW_ID_CARD_NAME, &row, &view->album_name, "New album name");
	row.x += row.width + 8;
	row.width = 112;
	clicked = kl_button(ui, style, VIEW_ID_CARD_NEW, &row, "New Album", KL_BUTTON_PRIMARY);
	if ((clicked || (changes & KL_FIELD_SUBMITTED) != 0U) && view->album_name.length > 0U) {
		/* The album shown keeps its place in the list (the albums are sorted again). */
		shown_id[0] = '\0';
		if (view->list == PH_LIST_ALBUM && view->album < count)
			(void)snprintf(shown_id, sizeof(shown_id), "%s", albums[view->album].id);
		error = ph_album_create(view->album_name.text, &album);
		albums = ph_albums(&count);
		for (index = 0; index < count && shown_id[0] != '\0'; index++) {
			same = strcmp(albums[index].id, shown_id);
			if (same == 0)
				view->album = index;
		}

		/* The album shown kept, and the photo added to the new one. */
		ph_log("ALBUM create name=%s error=%d", view->album_name.text, error);
		if (error == 0)
			view_add_to(view, album, now_us);
		else
			ph_view_notice(view, "That name cannot be an album's.", now_us);
		return;
	}

	/* Cancel. */
	row.x = card.x + card.width - 16 - 96;
	row.y = card.y + card.height - 40;
	row.width = 96;
	row.height = 28;
	clicked = kl_button(ui, style, VIEW_ID_CARD_CANCEL, &row, "Cancel", 0U);
	if (clicked)
		view->adding = 0;
}

/* Adds the photo shown whole or chosen to an album, the card closed and the database to be written. */
static void
view_add_to(
	struct ph_view *view,
	size_t album,
	uint64_t now_us)
{
	struct ph_photo *photos;
	struct ph_album *albums;
	char words[PH_NAME_MAX + 32];
	size_t count;
	size_t album_count;
	long photo;
	int error;

	/* The photo the card was shown for, into the album. */
	view->adding = 0;
	photo = view->card_photo;
	photos = ph_photos(&count);
	albums = ph_albums(&album_count);
	if (photo < 0 || (size_t)photo >= count || album >= album_count)
		return;
	error = ph_album_add(album, photos[photo].id);
	ph_log("ALBUM add album=%s photo=%ld error=%d", albums[album].name, photo, error);
	if (error != 0)
		return;

	/* Written, and said. */
	view->save = 1;
	(void)snprintf(words, sizeof(words), "Added to %s", albums[album].name);
	ph_view_notice(view, words, now_us);
}

/* Notes a press held on a photo: where the pointer was when it was first seen held (a drag starts from there). */
static void
view_drag_arm(
	struct ph_view *view,
	struct kl_ui *ui,
	long photo)
{
	double x;
	double y;

	/* The same press seen again: only its frame. */
	if (view->drag_armed && view->drag_photo == photo && view->drag_frame + 1U >= view->frame) {
		view->drag_frame = view->frame;
		return;
	}

	/* A new press. */
	kl_ui_pointer(ui, &x, &y);
	view->drag_armed = 1;
	view->drag_photo = photo;
	view->drag_press_x = x;
	view->drag_press_y = y;
	view->drag_frame = view->frame;
}
