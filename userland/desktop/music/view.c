/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Music's view (ws120-p009; music.h): at the left the albums -- a search
 * field over "All Songs" and each album with its cover, title and artist
 * -- and at the right the album chosen: its cover, title, artist, how many
 * songs and how long, a Play button, and its songs (the number, or a mark
 * on the one playing, the title, the artist when it is not the album's,
 * the length).  A double click or a tap on a song plays it.  At the bottom
 * across the window, the bar of what plays: its cover, title and artist,
 * Previous, Play or Pause and Next, and the position with the times.
 *
 * The view asks the window for what it cannot do itself (playing, a seek)
 * as requests; the window tells it what plays.
 */

#include "music.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The albums' column: its width's share of the window and its limits, the title's band, a row's height. */
#define VIEW_SIDEBAR_SHARE	0.28
#define VIEW_SIDEBAR_MIN	220
#define VIEW_SIDEBAR_MAX	300
#define VIEW_TITLE		96
#define VIEW_ALBUM_ROW		60

/* The album's header, a song's row, the bar of what plays. */
#define VIEW_HEADER		168
#define VIEW_COVER		120
#define VIEW_SONG_ROW		40
#define VIEW_BAR		80

/* The cards on glass: the gap between them and their corner. */
#define VIEW_GAP		8
#define VIEW_CARD_RADIUS	16.0f

/* The text sizes: the title, an album's title, a name, a detail. */
#define VIEW_TEXT_TITLE		22U
#define VIEW_TEXT_ALBUM		24U
#define VIEW_TEXT_NAME		14U
#define VIEW_TEXT_SMALL		12U

/* How long a notice shows, how long the slider keeps the user's value, the least time between two seeks. */
#define VIEW_NOTICE_US		4000000U
#define VIEW_SLIDER_US		400000U
#define VIEW_SEEK_US		150000U

/* How far Left and Right go (s). */
#define VIEW_STEP		10.0

/* The widgets' ids. */
#define VIEW_ID_SEARCH		1U
#define VIEW_ID_ALBUMS		2U
#define VIEW_ID_ALBUM		3U
#define VIEW_ID_SONGS		4U
#define VIEW_ID_SONG		5U
#define VIEW_ID_PLAY_ALBUM	6U
#define VIEW_ID_PREVIOUS	7U
#define VIEW_ID_PLAY		8U
#define VIEW_ID_NEXT		9U
#define VIEW_ID_POSITION	10U

/* The ground of an opaque window, and the cover of All Songs (yamabuki like the app's icon, the UAT of 2026-10-07). */
#define VIEW_COLOR_SURFACE	kl_theme_choose(KL_RGB(0xffffff), KL_RGB(0x23272f))
#define VIEW_COLOR_SIDEBAR	kl_theme_choose(KL_RGB(0xf4f6f9), KL_RGB(0x1f232a))
#define VIEW_COLOR_TILE		KL_RGB(0xf2a900)
#define VIEW_COLOR_TILE_LIGHT	KL_RGB(0xffc933)
#define VIEW_COLOR_WHITE	KL_RGB(0xffffff)

/* Where the three parts go. */
struct view_layout {
	struct kl_rect sidebar;
	struct kl_rect content;
	struct kl_rect bar;
};

static void view_layout(const struct mu_view *view, int width, int height, struct view_layout *layout);
static void view_request(struct mu_view *view, unsigned action, long song, double seconds);
static void view_sidebar(struct mu_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_album_row(struct mu_view *view, const struct kl_style *style, long album, const struct kl_rect *row, int selected, uint64_t now_us);
static void view_content(struct mu_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static void view_header(struct mu_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, const size_t *indices, size_t count, uint64_t now_us);
static void view_song_row(const struct mu_view *view, const struct kl_style *style, size_t song, size_t place, const struct kl_rect *row, unsigned hit);
static void view_bar(struct mu_view *view, struct kl_ui *ui, const struct kl_style *style, const struct kl_rect *area, uint64_t now_us);
static int view_round_button(struct kl_ui *ui, const struct kl_style *style, uint32_t id, int cx, int cy, int radius, int filled);
static void view_glyph_play(struct kl_canvas *canvas, float cx, float cy, float size, kl_color color);
static void view_glyph_pause(struct kl_canvas *canvas, float cx, float cy, float size, kl_color color);
static void view_glyph_skip(struct kl_canvas *canvas, float cx, float cy, float size, int forward, kl_color color);
static void view_cover(struct mu_view *view, const struct kl_style *style, long album, int x, int y, int side, float radius, uint64_t now_us);
static void view_time(double seconds, char *text, size_t size);
static size_t view_shown(struct mu_view *view, size_t **indices);

/*
 * Makes the view's state: every song shown, nothing chosen or playing.
 */
int
mu_view_init(
	struct mu_view *view)
{
	int error;

	/* Nothing yet. */
	memset(view, 0, sizeof(*view));
	view->album = -1;
	view->chosen = -1;
	view->playing = -1;

	/* The albums' scroll, down only. */
	error = kl_scroll_init(&view->albums_scroll, KL_SCROLL_Y);
	if (error != 0)
		return error;

	/* The songs' scroll, down only. */
	error = kl_scroll_init(&view->songs_scroll, KL_SCROLL_Y);
	if (error != 0) {
		kl_scroll_release(&view->albums_scroll);
		return error;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Frees what the view's state holds, and the albums' pictures it made.
 */
void
mu_view_release(
	struct mu_view *view)
{
	struct mu_album *albums;
	size_t count;
	size_t index;

	/* The pictures of the covers. */
	albums = mu_albums(&count);
	for (index = 0; index < count; index++) {
		kl_image_release(&albums[index].picture);
		albums[index].picture_tried = 0;
	}

	/* The scrolls and the list. */
	kl_scroll_release(&view->songs_scroll);
	kl_scroll_release(&view->albums_scroll);
	free(view->shown);
	view->shown = NULL;
	view->shown_room = 0;
}

/*
 * Releases the albums' pictures the view made, before the collection is
 * looked through again (the albums go with it).
 */
void
mu_view_forget_pictures(
	struct mu_view *view)
{
	struct mu_album *albums;
	size_t count;
	size_t index;

	/* The pictures of the covers. */
	(void)view;
	albums = mu_albums(&count);
	for (index = 0; index < count; index++) {
		kl_image_release(&albums[index].picture);
		albums[index].picture_tried = 0;
	}
}

/*
 * Carries out an action of the menu, a key or a button: the window plays,
 * pauses and moves through the songs; Quit ends the program.
 */
void
mu_view_action(
	struct mu_view *view,
	unsigned action,
	uint64_t now_us)
{
	size_t *indices;
	size_t count;

	/* Each action. */
	(void)now_us;
	switch (action) {
	case MU_ACTION_PLAY:
		/* Nothing playing yet: the song chosen, or the first shown. */
		if (view->playing < 0) {
			count = view_shown(view, &indices);
			if (view->chosen >= 0)
				view_request(view, MU_ACTION_SONG, view->chosen, 0.0);
			else if (count > 0U)
				view_request(view, MU_ACTION_SONG, (long)indices[0], 0.0);
			break;
		}

		/* Playing or paused: the other. */
		view_request(view, MU_ACTION_PLAY, view->playing, 0.0);
		break;
	case MU_ACTION_NEXT:
	case MU_ACTION_PREVIOUS:
		view_request(view, action, view->playing, 0.0);
		break;
	case MU_ACTION_QUIT:
		view->quit = 1;
		break;
	default:
		break;
	}

	/* The log line the tests read. */
	mu_log("REQUEST action=%u song=%ld", action, view->playing);
}

/*
 * Takes a key no widget took: Space plays or pauses, Left and Right go
 * back and forward, Up and Down move through the songs shown, Enter plays
 * the one chosen.
 */
void
mu_view_key(
	struct mu_view *view,
	uint32_t key,
	unsigned modifiers,
	uint64_t now_us)
{
	size_t *indices;
	size_t count;
	size_t at;
	size_t index;

	/* The keys with a modifier are the menu's. */
	if ((modifiers & (KL_MOD_CTRL | KL_MOD_ALT | KL_MOD_SUPER)) != 0U)
		return;

	/* Space: play or pause. */
	if (key == KL_KEY_SPACE) {
		mu_view_action(view, MU_ACTION_PLAY, now_us);
		return;
	}

	/* Left and Right: a step back or forward in the song playing. */
	if ((key == KL_KEY_LEFT || key == KL_KEY_RIGHT) && view->playing >= 0) {
		if (key == KL_KEY_LEFT)
			view_request(view, MU_ACTION_SEEK, view->playing, view->position - VIEW_STEP);
		else
			view_request(view, MU_ACTION_SEEK, view->playing, view->position + VIEW_STEP);
		return;
	}

	/* Enter: the song chosen plays. */
	if ((key == KL_KEY_ENTER || key == KL_KEY_KPENTER) && view->chosen >= 0) {
		view_request(view, MU_ACTION_SONG, view->chosen, 0.0);
		return;
	}

	/* Up and Down alone move, through the songs shown. */
	count = view_shown(view, &indices);
	if ((key != KL_KEY_UP && key != KL_KEY_DOWN) || count == 0U)
		return;

	/* Where the song chosen is among them (before the first when it is not). */
	at = count;
	for (index = 0; index < count; index++) {
		if ((long)indices[index] == view->chosen)
			at = index;
	}

	/* One up or down, within the list. */
	if (at == count)
		at = 0;
	else if (key == KL_KEY_UP && at > 0U)
		at--;
	else if (key == KL_KEY_DOWN && at + 1U < count)
		at++;
	view->chosen = (long)indices[at];
}

/*
 * Reports how long the window may wait for input (ms) before the next
 * frame is due by itself: a while to move the position while a song
 * plays, until the notice goes, or -1 for no time.
 */
int
mu_view_wait(
	const struct mu_view *view,
	uint64_t now_us)
{
	uint64_t left;
	int wait;

	/* Nothing moves by itself. */
	wait = -1;

	/* A song playing: its position moves. */
	if (view->state == MU_PLAYING)
		wait = 250;

	/* A notice: until it goes. */
	if (view->notice[0] != '\0' && now_us < view->notice_until) {
		left = (view->notice_until - now_us) / 1000U + 1U;
		if (wait < 0 || left < (uint64_t)wait)
			wait = (int)left;
	}

	/* A seek to send. */
	if (view->seek_pending && (wait < 0 || wait > 50))
		wait = 50;
	return wait;
}

/*
 * Draws a frame of the view in a window of a size, between the caller's
 * kl_ui_begin and kl_ui_end, and takes what the input did to its widgets.
 */
void
mu_view_draw(
	struct mu_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	int width,
	int height,
	uint64_t now_us)
{
	struct view_layout layout;
	struct kl_rect whole;

	/* The ground: clear on glass (the desktop shows between the cards), else the surface. */
	whole.x = 0;
	whole.y = 0;
	whole.width = width;
	whole.height = height;
	if (view->glass)
		kl_canvas_clear(style->canvas);
	else
		kl_canvas_fill(style->canvas, &whole, VIEW_COLOR_SURFACE);

	/* The three parts; on glass each stands on a card of its own. */
	view_layout(view, width, height, &layout);
	if (view->glass) {
		kl_canvas_round(style->canvas, (float)layout.sidebar.x, (float)layout.sidebar.y, (float)layout.sidebar.width,
		    (float)layout.sidebar.height, VIEW_CARD_RADIUS, style->theme->glass_sidebar);
		kl_canvas_round(style->canvas, (float)layout.content.x, (float)layout.content.y, (float)layout.content.width,
		    (float)layout.content.height, VIEW_CARD_RADIUS, style->theme->glass_content);
		kl_canvas_round(style->canvas, (float)layout.bar.x, (float)layout.bar.y, (float)layout.bar.width,
		    (float)layout.bar.height, VIEW_CARD_RADIUS, style->theme->glass_content);
	}

	/* The albums, the songs, the bar. */
	view_sidebar(view, ui, style, &layout.sidebar, now_us);
	view_content(view, ui, style, &layout.content, now_us);
	view_bar(view, ui, style, &layout.bar, now_us);

	/* The notice over the bottom of the songs while it shows. */
	if (view->notice[0] != '\0' && now_us < view->notice_until)
		kl_chip(style, layout.content.x + layout.content.width / 2, layout.content.y + layout.content.height - 16, view->notice);
}

/*
 * Lists the parts of the view that stand on the compositor's glass (the three
 * cards) for a window of a size, into up to capacity panels; returns how
 * many there are.
 */
size_t
mu_view_panels(
	const struct mu_view *view,
	int width,
	int height,
	struct kl_glass_panel *panels,
	size_t capacity)
{
	struct view_layout layout;
	struct kl_rect cards[3];
	size_t count;
	size_t index;

	/* The cards, as the frame draws them. */
	view_layout(view, width, height, &layout);
	cards[0] = layout.sidebar;
	cards[1] = layout.content;
	cards[2] = layout.bar;
	count = 0;
	for (index = 0; index < 3U && count < capacity; index++) {
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
 * Takes the oldest request the view queued: 1 with it, 0 when none waits.
 */
int
mu_view_take_request(
	struct mu_view *view,
	struct mu_request *request)
{
	size_t index;

	/* None. */
	if (view->request_count == 0U)
		return 0;

	/* The first, the rest moved up. */
	*request = view->requests[0];
	for (index = 1; index < view->request_count; index++)
		view->requests[index - 1U] = view->requests[index];
	view->request_count--;
	return 1;
}

/*
 * Shows a notice for a while (its words copied).
 */
void
mu_view_notice(
	struct mu_view *view,
	const char *message,
	uint64_t now_us)
{
	/* The words and until when. */
	(void)snprintf(view->notice, sizeof(view->notice), "%s", message);
	view->notice_until = now_us + VIEW_NOTICE_US;
}

/*
 * Lays out the albums at the left and the songs at the right over the bar
 * across the bottom; on glass, as cards with a gap between.
 */
static void
view_layout(
	const struct mu_view *view,
	int width,
	int height,
	struct view_layout *layout)
{
	int gap;
	int share;
	int top;

	/* The gap: only cards on glass have one. */
	gap = 0;
	if (view->glass)
		gap = VIEW_GAP;

	/* The bar across the bottom. */
	layout->bar.x = 0;
	layout->bar.y = height - VIEW_BAR;
	layout->bar.width = width;
	layout->bar.height = VIEW_BAR;

	/* The albums at the left, a share of the window within limits, over the bar. */
	share = (int)((double)width * VIEW_SIDEBAR_SHARE);
	if (share < VIEW_SIDEBAR_MIN)
		share = VIEW_SIDEBAR_MIN;
	else if (share > VIEW_SIDEBAR_MAX)
		share = VIEW_SIDEBAR_MAX;
	top = height - VIEW_BAR - gap;
	layout->sidebar.x = 0;
	layout->sidebar.y = 0;
	layout->sidebar.width = share;
	layout->sidebar.height = top;

	/* The songs the rest. */
	layout->content.x = share + gap;
	layout->content.y = 0;
	layout->content.width = width - share - gap;
	layout->content.height = top;
}

/* Queues a request for the window (a full queue drops it: the user asks again). */
static void
view_request(
	struct mu_view *view,
	unsigned action,
	long song,
	double seconds)
{
	/* No room. */
	if (view->request_count == MU_REQUESTS_MAX)
		return;

	/* At the end. */
	view->requests[view->request_count].action = action;
	view->requests[view->request_count].song = song;
	view->requests[view->request_count].seconds = seconds;
	view->request_count++;
}

/*
 * Draws the albums' column: the title, the search field, All Songs and
 * the albums.
 */
static void
view_sidebar(
	struct mu_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	struct kl_rect field;
	struct kl_rect list;
	struct kl_rect edge;
	struct kl_rect row;
	unsigned changes;
	unsigned hit;
	size_t albums;
	size_t index;
	long album;

	/* The column's ground and its edge (on glass, its card is drawn already). */
	if (!view->glass) {
		kl_canvas_fill(style->canvas, area, VIEW_COLOR_SIDEBAR);
		edge = *area;
		edge.x = area->x + area->width - 1;
		edge.width = 1;
		kl_canvas_fill(style->canvas, &edge, style->theme->separator);
	}

	/* The title. */
	(void)kl_text_draw(style->text, style->canvas, area->x + 20, area->y + 38, "Music", strlen("Music"), VIEW_TEXT_TITLE, 1, style->theme->text);

	/* The search field; what it holds filters the songs. */
	field.x = area->x + 16;
	field.y = area->y + 52;
	field.width = area->width - 32;
	field.height = 32;
	changes = kl_field(ui, style, VIEW_ID_SEARCH, &field, &view->search, "Search");
	if ((changes & KL_FIELD_CHANGED) != 0U)
		kl_scroll_move_to(&view->songs_scroll, 0.0, 0.0, 0, now_us);

	/* Enter or Escape leaves the field: Space plays or pauses again (ws177-p021). */
	if ((changes & (KL_FIELD_SUBMITTED | KL_FIELD_CANCELLED)) != 0U)
		kl_ui_clear_focus(ui);

	/* The list's viewport, which scrolls: All Songs, then each album. */
	(void)mu_albums(&albums);
	list.x = area->x;
	list.y = area->y + VIEW_TITLE;
	list.width = area->width - 1;
	list.height = area->height - VIEW_TITLE;
	kl_scroll_set_size(&view->albums_scroll, (double)list.width, (double)(albums + 1U) * VIEW_ALBUM_ROW + 8.0, (double)list.width, (double)list.height);
	kl_ui_scroll_region(ui, VIEW_ID_ALBUMS, &list, &view->albums_scroll);
	kl_canvas_clip_push(style->canvas, &list);

	/* Each row: a click shows the album (-1 for All Songs). */
	for (index = 0; index <= albums; index++) {
		/* The row, inset; one out of the viewport is not drawn. */
		row.x = list.x + 8;
		row.y = list.y + (int)index * VIEW_ALBUM_ROW - (int)view->albums_scroll.y;
		row.width = list.width - 16;
		row.height = VIEW_ALBUM_ROW - 4;
		if (row.y + row.height < list.y || row.y > list.y + list.height)
			continue;

		/* Its input. */
		album = (long)index - 1;
		hit = kl_ui_hit(ui, VIEW_ID_ALBUM, (uint32_t)index, &row);
		if ((hit & KL_HIT_CLICKED) != 0U && view->album != album) {
			view->album = album;
			view->chosen = -1;
			kl_scroll_move_to(&view->songs_scroll, 0.0, 0.0, 0, now_us);
			mu_log("ALBUM index=%ld", album);
		}

		/* The ground under the pointer, and the row. */
		if ((hit & KL_HIT_HOT) != 0U)
			kl_canvas_round(style->canvas, (float)row.x, (float)row.y, (float)row.width, (float)row.height, 10.0f, style->theme->hover);
		view_album_row(view, style, album, &row, view->album == album, now_us);
	}

	/* The rows' clip goes, and the bar shows while the list moves. */
	kl_canvas_clip_pop(style->canvas);
	(void)kl_scroll_draw_bars(&view->albums_scroll, style->canvas, &list, style->theme, now_us);
}

/* Draws one row of the albums: the cover, the title and the artist (All Songs for -1). */
static void
view_album_row(
	struct mu_view *view,
	const struct kl_style *style,
	long album,
	const struct kl_rect *row,
	int selected,
	uint64_t now_us)
{
	struct mu_album *albums;
	const char *title;
	const char *artist;
	char count_text[32];
	size_t count;
	size_t songs;
	int side;

	/* The selection under the chosen row. */
	if (selected)
		kl_canvas_round(style->canvas, (float)row->x, (float)row->y, (float)row->width, (float)row->height, 10.0f, style->theme->selection);

	/* The cover. */
	side = row->height - 12;
	view_cover(view, style, album, row->x + 6, row->y + 6, side, 6.0f, now_us);

	/* All Songs: the count of songs under it. */
	albums = mu_albums(&count);
	if (album < 0) {
		(void)mu_songs(&songs);
		(void)snprintf(count_text, sizeof(count_text), "%lu songs", (unsigned long)songs);
		title = "All Songs";
		artist = count_text;
	} else {
		/* An album: its title and artist. */
		title = albums[album].title;
		artist = albums[album].artist;
	}

	/* The words. */
	(void)kl_text_draw_fit(style->text, style->canvas, row->x + side + 18, row->y + 24, title, VIEW_TEXT_NAME, 1, row->width - side - 26, style->theme->text);
	(void)kl_text_draw_fit(style->text, style->canvas, row->x + side + 18, row->y + 43, artist, VIEW_TEXT_SMALL, 0, row->width - side - 26, style->theme->text_secondary);
}

/* Draws the songs' side: the album's header and its songs. */
static void
view_content(
	struct mu_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	size_t *indices;
	struct kl_rect header;
	struct kl_rect list;
	struct kl_rect row;
	unsigned hit;
	size_t count;
	size_t total;
	size_t index;

	/* The songs shown. */
	count = view_shown(view, &indices);
	(void)mu_songs(&total);

	/* The header. */
	header = *area;
	header.height = VIEW_HEADER;
	view_header(view, ui, style, &header, indices, count, now_us);

	/* No songs at all. */
	if (total == 0U) {
		(void)kl_text_draw(style->text, style->canvas, area->x + 28, area->y + VIEW_HEADER + 40, "Put m4a files in the Music folder to see them here.",
		    strlen("Put m4a files in the Music folder to see them here."), VIEW_TEXT_NAME, 0, style->theme->text_faint);
		return;
	}

	/* None that match. */
	if (count == 0U) {
		(void)kl_text_draw(style->text, style->canvas, area->x + 28, area->y + VIEW_HEADER + 40, "No songs match.", strlen("No songs match."), VIEW_TEXT_NAME, 0,
		    style->theme->text_faint);
		return;
	}

	/* The songs' viewport, which scrolls. */
	list.x = area->x;
	list.y = area->y + VIEW_HEADER;
	list.width = area->width;
	list.height = area->height - VIEW_HEADER;
	kl_scroll_set_size(&view->songs_scroll, (double)list.width, (double)count * VIEW_SONG_ROW + 16.0, (double)list.width, (double)list.height);
	kl_ui_scroll_region(ui, VIEW_ID_SONGS, &list, &view->songs_scroll);
	kl_canvas_clip_push(style->canvas, &list);

	/* Each row: a click chooses the song, a double click or a tap plays it. */
	for (index = 0; index < count; index++) {
		/* The row, inset; one out of the viewport is not drawn. */
		row.x = list.x + 16;
		row.y = list.y + (int)index * VIEW_SONG_ROW - (int)view->songs_scroll.y;
		row.width = list.width - 32;
		row.height = VIEW_SONG_ROW - 2;
		if (row.y + row.height < list.y || row.y > list.y + list.height)
			continue;

		/* Its input. */
		hit = kl_ui_hit(ui, VIEW_ID_SONG, (uint32_t)indices[index], &row);
		if ((hit & KL_HIT_CLICKED) != 0U)
			view->chosen = (long)indices[index];
		if ((hit & KL_HIT_CLICKED) != 0U && (hit & (KL_HIT_DOUBLE | KL_HIT_TOUCHED)) != 0U) {
			view_request(view, MU_ACTION_SONG, (long)indices[index], 0.0);
			mu_log("REQUEST action=%u song=%ld", MU_ACTION_SONG, (long)indices[index]);
		}

		/* The row. */
		view_song_row(view, style, indices[index], index, &row, hit);
	}

	/* The rows' clip goes, and the bar shows while the list moves. */
	kl_canvas_clip_pop(style->canvas);
	(void)kl_scroll_draw_bars(&view->songs_scroll, style->canvas, &list, style->theme, now_us);
}

/*
 * Draws the header of the songs: the album's cover (a tile for All Songs
 * or a search), its title, artist, how many songs and how long, and Play.
 */
static void
view_header(
	struct mu_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	const size_t *indices,
	size_t count,
	uint64_t now_us)
{
	const struct mu_song *songs;
	struct mu_album *albums;
	struct kl_rect button;
	const char *title;
	const char *artist;
	char summary[64];
	int64_t length_ms;
	size_t total;
	size_t index;
	unsigned flags;
	long minutes;
	int clicked;
	int left;

	/* The cover. */
	(void)now_us;
	view_cover(view, style, view->album, area->x + 24, area->y + 24, VIEW_COVER, 10.0f, now_us);

	/* The title and the artist: the album's, or All Songs (the search's words when there are some). */
	albums = mu_albums(&total);
	title = "All Songs";
	artist = "";
	if (view->album >= 0 && (size_t)view->album < total) {
		title = albums[view->album].title;
		artist = albums[view->album].artist;
	} else if (view->search.length > 0U) {
		title = "Search";
		artist = view->search.text;
	}

	/* How many songs, and how long in all. */
	songs = mu_songs(&total);
	length_ms = 0;
	for (index = 0; index < count; index++)
		length_ms += songs[indices[index]].duration_ms;
	minutes = (long)((length_ms + 59999) / 60000);
	if (count == 1U)
		(void)snprintf(summary, sizeof(summary), "1 song, %ld min", minutes);
	else
		(void)snprintf(summary, sizeof(summary), "%lu songs, %ld min", (unsigned long)count, minutes);

	/* The words beside the cover. */
	left = area->x + 24 + VIEW_COVER + 24;
	(void)kl_text_draw_fit(style->text, style->canvas, left, area->y + 62, title, VIEW_TEXT_ALBUM, 1, area->x + area->width - left - 24, style->theme->text);
	(void)kl_text_draw_fit(style->text, style->canvas, left, area->y + 88, artist, VIEW_TEXT_NAME + 2U, 0, area->x + area->width - left - 24, style->theme->accent_text);
	(void)kl_text_draw(style->text, style->canvas, left, area->y + 110, summary, strlen(summary), VIEW_TEXT_SMALL, 0, style->theme->text_secondary);

	/* Play: the first song shown. */
	button.x = left;
	button.y = area->y + 120;
	button.width = 96;
	button.height = 30;
	flags = KL_BUTTON_PRIMARY;
	if (count == 0U)
		flags |= KL_BUTTON_DISABLED;
	clicked = kl_button(ui, style, VIEW_ID_PLAY_ALBUM, &button, "Play", flags);
	if (clicked && count > 0U) {
		/* The keyboard is not left on the button: Space then plays or pauses, not starts again. */
		kl_ui_clear_focus(ui);
		view_request(view, MU_ACTION_SONG, (long)indices[0], 0.0);
		mu_log("REQUEST action=%u song=%ld", MU_ACTION_SONG, (long)indices[0]);
	}
}

/*
 * Draws one song's row: the number (or a mark on the one playing), the
 * title, the artist when the album's is not its own or every song is
 * shown, and the length.
 */
static void
view_song_row(
	const struct mu_view *view,
	const struct kl_style *style,
	size_t song,
	size_t place,
	const struct kl_rect *row,
	unsigned hit)
{
	const struct mu_song *songs;
	struct mu_album *albums;
	struct kl_rect line;
	char number[24];
	char length[16];
	kl_color ink;
	size_t count;
	int playing;
	int same;
	int width;
	int title_width;
	int baseline;

	/* The ground: chosen, or under the pointer; a line under the others. */
	songs = mu_songs(&count);
	albums = mu_albums(&count);
	if ((long)song == view->chosen)
		kl_canvas_round(style->canvas, (float)row->x, (float)row->y, (float)row->width, (float)row->height, 8.0f, style->theme->selection);
	else if ((hit & KL_HIT_HOT) != 0U)
		kl_canvas_round(style->canvas, (float)row->x, (float)row->y, (float)row->width, (float)row->height, 8.0f, style->theme->hover);
	line.x = row->x + 44;
	line.y = row->y + row->height;
	line.width = row->width - 44;
	line.height = 1;
	kl_canvas_fill(style->canvas, &line, style->theme->separator);

	/* The number, or the mark of the song playing. */
	playing = (long)song == view->playing;
	baseline = row->y + 25;
	ink = style->theme->text;
	if (playing) {
		ink = style->theme->accent_text;
		if (view->state == MU_PLAYING)
			view_glyph_play(style->canvas, (float)row->x + 18.0f, (float)row->y + (float)row->height / 2.0f, 12.0f, ink);
		else
			view_glyph_pause(style->canvas, (float)row->x + 18.0f, (float)row->y + (float)row->height / 2.0f, 12.0f, ink);
	} else {
		/* The number on the album, or the place in the list. */
		if (songs[song].track > 0 && view->album >= 0)
			(void)snprintf(number, sizeof(number), "%d", songs[song].track);
		else
			(void)snprintf(number, sizeof(number), "%lu", (unsigned long)(place + 1U));
		width = kl_text_width(style->text, number, strlen(number), VIEW_TEXT_SMALL, 0);
		(void)kl_text_draw(style->text, style->canvas, row->x + 26 - width, baseline, number, strlen(number), VIEW_TEXT_SMALL, 0, style->theme->text_secondary);
	}

	/* The length at the right. */
	view_time((double)songs[song].duration_ms / 1000.0, length, sizeof(length));
	width = kl_text_width(style->text, length, strlen(length), VIEW_TEXT_SMALL, 0);
	(void)kl_text_draw(style->text, style->canvas, row->x + row->width - 12 - width, baseline, length, strlen(length), VIEW_TEXT_SMALL, 0, style->theme->text_secondary);

	/* The title, and the artist after it when it is not the album's or every song is shown. */
	title_width = row->width - 44 - width - 24;
	same = strcmp(songs[song].artist, albums[songs[song].album].artist);
	if (view->album < 0 || same != 0) {
		title_width = title_width * 3 / 5;
		(void)kl_text_draw_fit(style->text, style->canvas, row->x + 44 + title_width + 16, baseline, songs[song].artist, VIEW_TEXT_NAME, 0,
		    row->width - 44 - title_width - 16 - width - 24, style->theme->text_secondary);
	}

	/* The title. */
	(void)kl_text_draw_fit(style->text, style->canvas, row->x + 44, baseline, songs[song].title, VIEW_TEXT_NAME, playing, title_width, ink);
}

/*
 * Draws the bar of what plays: the cover, the title and the artist, the
 * three buttons, and the position with the times either side.
 */
static void
view_bar(
	struct mu_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	const struct kl_rect *area,
	uint64_t now_us)
{
	const struct mu_song *songs;
	struct kl_rect edge;
	struct kl_rect track;
	char elapsed[16];
	char remaining[24];
	size_t count;
	double length;
	int clicked;
	int changed;
	int middle;
	int width;
	int words;
	int left;

	/* The bar's ground and its edge (on glass, its card is drawn already). */
	if (!view->glass) {
		kl_canvas_fill(style->canvas, area, VIEW_COLOR_SIDEBAR);
		edge = *area;
		edge.height = 1;
		kl_canvas_fill(style->canvas, &edge, style->theme->separator);
	}

	/* The three buttons in the middle, over the position. */
	middle = area->x + area->width / 2;
	clicked = view_round_button(ui, style, VIEW_ID_PREVIOUS, middle - 52, area->y + 28, 16, 0);
	view_glyph_skip(style->canvas, (float)(middle - 52), (float)area->y + 28.0f, 14.0f, 0, style->theme->text);
	if (clicked)
		mu_view_action(view, MU_ACTION_PREVIOUS, now_us);
	clicked = view_round_button(ui, style, VIEW_ID_PLAY, middle, area->y + 28, 20, 1);
	if (view->state == MU_PLAYING)
		view_glyph_pause(style->canvas, (float)middle, (float)area->y + 28.0f, 16.0f, style->theme->accent_ink);
	else
		view_glyph_play(style->canvas, (float)middle + 2.0f, (float)area->y + 28.0f, 16.0f, style->theme->accent_ink);
	if (clicked)
		mu_view_action(view, MU_ACTION_PLAY, now_us);
	clicked = view_round_button(ui, style, VIEW_ID_NEXT, middle + 52, area->y + 28, 16, 0);
	view_glyph_skip(style->canvas, (float)(middle + 52), (float)area->y + 28.0f, 14.0f, 1, style->theme->text);
	if (clicked)
		mu_view_action(view, MU_ACTION_NEXT, now_us);

	/* The position follows the song, unless the user just moved it. */
	length = view->length;
	if (length <= 0.0)
		length = 1.0;
	if (now_us >= view->slider_until)
		view->slider = view->position;
	width = area->width / 3;
	if (width > 420)
		width = 420;
	track.x = middle - width / 2;
	track.y = area->y + 52;
	track.width = width;
	track.height = 20;
	if (view->playing >= 0)
		changed = kl_slider(ui, style, VIEW_ID_POSITION, &track, 0.0, length, 0.0, &view->slider);
	else
		changed = kl_slider_flags(ui, style, VIEW_ID_POSITION, &track, 0.0, 1.0, 0.0, &view->slider, KL_BUTTON_DISABLED);

	/* Moved: a seek, sent at most every so often (the last one when the moving stops). */
	if (changed && view->playing >= 0) {
		view->slider_until = now_us + VIEW_SLIDER_US;
		view->seek_pending = 1;
	}

	/* The seek waiting, when its time has come. */
	if (view->seek_pending && now_us - view->seek_at >= VIEW_SEEK_US) {
		view->seek_pending = 0;
		view->seek_at = now_us;
		view_request(view, MU_ACTION_SEEK, view->playing, view->slider);
	}

	/* The times either side. */
	view_time(view->slider, elapsed, sizeof(elapsed));
	remaining[0] = '-';
	view_time(view->length - view->slider, remaining + 1, sizeof(remaining) - 1U);
	words = kl_text_width(style->text, elapsed, strlen(elapsed), VIEW_TEXT_SMALL, 0);
	(void)kl_text_draw(style->text, style->canvas, track.x - 10 - words, track.y + 15, elapsed, strlen(elapsed), VIEW_TEXT_SMALL, 0, style->theme->text_secondary);
	(void)kl_text_draw(style->text, style->canvas, track.x + track.width + 10, track.y + 15, remaining, strlen(remaining), VIEW_TEXT_SMALL, 0, style->theme->text_secondary);

	/* What plays at the left: its cover, its title and artist; or why nothing can. */
	songs = mu_songs(&count);
	left = area->x + 16;
	words = track.x - 60 - left - 64;
	if (view->problem[0] != '\0') {
		(void)kl_text_draw_fit(style->text, style->canvas, left, area->y + 46, view->problem, VIEW_TEXT_SMALL, 0, track.x - 70 - left, style->theme->danger);
		return;
	}

	/* Nothing playing. */
	if (view->playing < 0 || (size_t)view->playing >= count) {
		(void)kl_text_draw_fit(style->text, style->canvas, left, area->y + 46, "Not Playing", VIEW_TEXT_NAME, 0, track.x - 70 - left, style->theme->text_faint);
		return;
	}

	/* The song. */
	view_cover(view, style, (long)songs[view->playing].album, left, area->y + 12, 56, 6.0f, now_us);
	(void)kl_text_draw_fit(style->text, style->canvas, left + 68, area->y + 36, songs[view->playing].title, VIEW_TEXT_NAME, 1, words, style->theme->text);
	(void)kl_text_draw_fit(style->text, style->canvas, left + 68, area->y + 56, songs[view->playing].artist, VIEW_TEXT_SMALL, 0, words, style->theme->text_secondary);
}

/* Draws a round button (filled in the accent, or a ground under the pointer); reports a click. */
static int
view_round_button(
	struct kl_ui *ui,
	const struct kl_style *style,
	uint32_t id,
	int cx,
	int cy,
	int radius,
	int filled)
{
	struct kl_rect rect;
	unsigned hit;

	/* Its input. */
	rect.x = cx - radius;
	rect.y = cy - radius;
	rect.width = radius * 2;
	rect.height = radius * 2;
	hit = kl_ui_hit(ui, id, 0U, &rect);

	/* Its ground. */
	if (filled)
		kl_canvas_circle(style->canvas, (float)cx, (float)cy, (float)radius, style->theme->accent);
	else if ((hit & KL_HIT_HOT) != 0U)
		kl_canvas_circle(style->canvas, (float)cx, (float)cy, (float)radius, style->theme->hover);

	/* Clicked. */
	return (hit & KL_HIT_CLICKED) != 0U;
}

/* Draws the play triangle of a size centred on a point. */
static void
view_glyph_play(
	struct kl_canvas *canvas,
	float cx,
	float cy,
	float size,
	kl_color color)
{
	float points[6];

	/* Pointing right. */
	points[0] = cx - size * 0.4f;
	points[1] = cy - size * 0.5f;
	points[2] = cx + size * 0.5f;
	points[3] = cy;
	points[4] = cx - size * 0.4f;
	points[5] = cy + size * 0.5f;
	kl_canvas_polygon(canvas, points, 3, color);
}

/* Draws the two bars of pause of a size centred on a point. */
static void
view_glyph_pause(
	struct kl_canvas *canvas,
	float cx,
	float cy,
	float size,
	kl_color color)
{
	/* Two rounded bars. */
	kl_canvas_round(canvas, cx - size * 0.4f, cy - size * 0.5f, size * 0.3f, size, size * 0.08f, color);
	kl_canvas_round(canvas, cx + size * 0.1f, cy - size * 0.5f, size * 0.3f, size, size * 0.08f, color);
}

/* Draws next (forward) or previous: two triangles and a bar, of a size centred on a point. */
static void
view_glyph_skip(
	struct kl_canvas *canvas,
	float cx,
	float cy,
	float size,
	int forward,
	kl_color color)
{
	float points[6];
	float side;
	float half;

	/* The direction: 1 forward, -1 back. */
	side = -1.0f;
	if (forward)
		side = 1.0f;
	half = size * 0.5f;

	/* The first triangle. */
	points[0] = cx - side * half;
	points[1] = cy - half;
	points[2] = cx;
	points[3] = cy;
	points[4] = cx - side * half;
	points[5] = cy + half;
	kl_canvas_polygon(canvas, points, 3, color);

	/* The second, against the bar. */
	points[0] = cx;
	points[1] = cy - half;
	points[2] = cx + side * half;
	points[3] = cy;
	points[4] = cx;
	points[5] = cy + half;
	kl_canvas_polygon(canvas, points, 3, color);

	/* The bar at the end. */
	if (forward)
		kl_canvas_round(canvas, cx + half, cy - half, size * 0.12f, size, 1.0f, color);
	else
		kl_canvas_round(canvas, cx - half - size * 0.12f, cy - half, size * 0.12f, size, 1.0f, color);
}

/*
 * Draws an album's cover in a square (its picture, made from its file's
 * bytes the first time it shows), or a tile with a note for an album
 * without one, one whose cover could not be made (told once) and All Songs
 * (-1).
 */
static void
view_cover(
	struct mu_view *view,
	const struct kl_style *style,
	long album,
	int x,
	int y,
	int side,
	float radius,
	uint64_t now_us)
{
	struct mu_album *albums;
	size_t count;
	int error;

	/* The album's picture, made once: its bytes from the file, then the picture. */
	albums = mu_albums(&count);
	if (album >= 0 && (size_t)album < count && !albums[album].picture_tried && (albums[album].cover != NULL || albums[album].cover_path != NULL)) {
		albums[album].picture_tried = 1;
		error = mu_library_cover_load((size_t)album);
		if (error == 0)
			error = mu_cover_picture(albums[album].cover, albums[album].cover_size, MU_COVER_SIDE, &albums[album].picture);
		mu_log("COVER album=%ld error=%d", album, error);

		/* A cover that could not be made: the note's tile, told the first time. */
		if (error != 0 && !view->cover_told) {
			view->cover_told = 1;
			mu_view_notice(view, "A cover could not be shown.", now_us);
		}
	}

	/* The picture. */
	if (album >= 0 && (size_t)album < count && albums[album].picture.pixels != NULL) {
		kl_canvas_image(style->canvas, &albums[album].picture, (float)x, (float)y, (float)side, (float)side, radius, 1.0f);
		return;
	}

	/* A tile with the note: yamabuki for All Songs, grey for an album without a cover. */
	if (album < 0)
		kl_canvas_round_gradient(style->canvas, (float)x, (float)y, (float)side, (float)side, radius, VIEW_COLOR_TILE_LIGHT, VIEW_COLOR_TILE);
	else
		kl_canvas_round_gradient(style->canvas, (float)x, (float)y, (float)side, (float)side, radius, KL_RGB(0xb9c0cb), KL_RGB(0x8a93a1));
	kl_icon_draw(style->canvas, KL_ICON_MUSIC, (float)x + (float)side * 0.2f, (float)y + (float)side * 0.2f, (float)side * 0.6f, VIEW_COLOR_WHITE);
}

/* Writes a time as minutes and seconds (hours, minutes and seconds from an hour on). */
static void
view_time(
	double seconds,
	char *text,
	size_t size)
{
	long whole;

	/* Whole seconds, none below zero. */
	whole = (long)seconds;
	if (whole < 0)
		whole = 0;

	/* From an hour on. */
	if (whole >= 3600) {
		(void)snprintf(text, size, "%ld:%02ld:%02ld", whole / 3600, whole / 60 % 60, whole % 60);
		return;
	}

	/* Under an hour. */
	(void)snprintf(text, size, "%ld:%02ld", whole / 60, whole % 60);
}

/*
 * Lists the songs shown (those of the album chosen that match the search)
 * in the view's own list, as long as the collection; reports how many (0
 * without memory for the list).
 */
static size_t
view_shown(
	struct mu_view *view,
	size_t **indices)
{
	size_t *grown;
	size_t total;
	size_t count;

	/* The list as long as the collection. */
	(void)mu_songs(&total);
	if (view->shown_room < total || view->shown == NULL) {
		grown = realloc(view->shown, (total + 1U) * sizeof(*grown));
		if (grown == NULL) {
			*indices = view->shown;
			return 0;
		}

		/* The longer list. */
		view->shown = grown;
		view->shown_room = total + 1U;
	}

	/* The collection's list. */
	*indices = view->shown;
	count = mu_library_list(view->album, view->search.text, view->shown, view->shown_room);
	return count;
}
