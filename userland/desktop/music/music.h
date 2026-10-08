/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Music (WS120): Keiland's music player for the songs in ~/Music.
 *
 * The songs are m4a files (AAC in MP4, plan/ws120/phase001/phase.md, the
 * 2026-10-07 user decision); their names, artists, albums and covers come
 * from the files' MP4 metadata (tags.c), and the collection groups them by
 * album (library.c).  Playing (play.c) reads the file with mediafile,
 * decodes the AAC with Video Player's add-in of libavcodec (codec.c, opened
 * with dlopen) and writes the sound to libkeiland's sound stream (audio.c).
 *
 * The view (view.c) draws a frame with libkeiland's canvas and widgets and
 * knows nothing of the window or the player -- what the user asks it
 * queues as requests the window takes -- so that the host tests draw it
 * into pictures; the window (main.c) feeds it the input.
 */

#ifndef MUSIC_MUSIC_H
#define MUSIC_MUSIC_H

#include <keiland/keiland.h>

#include <stddef.h>
#include <stdint.h>

/* The longest name, artist or album kept, with its NUL. */
#define MU_TEXT_MAX		256U

/* The largest cover kept from a file. */
#define MU_COVER_MAX		(8U * 1024U * 1024U)

/*
 * What a song's file says of it: its title, artist, album's artist and
 * album, its number on the album (0 for none), its length, its cover (the
 * bytes of a JPEG or PNG, allocated; NULL for none; has_cover says the file
 * has one even where the bytes are not kept), and whether it has a track of
 * sound and one of pictures.
 */
struct mu_tags {
	char title[MU_TEXT_MAX];
	char artist[MU_TEXT_MAX];
	char album_artist[MU_TEXT_MAX];
	char album[MU_TEXT_MAX];
	int track;
	int64_t duration_ms;
	unsigned char *cover;
	size_t cover_size;
	int has_cover;
	int has_sound;
	int has_video;
};

/*
 * One song of the collection: its file, title, artist, number, length,
 * and the album it is on (an index of the albums).  The strings are the
 * library's.
 */
struct mu_song {
	char *path;
	char *title;
	char *artist;
	int track;
	int64_t duration_ms;
	size_t album;
};

/*
 * One album: its title and artist, the file of its first song with a cover
 * (NULL for none), the cover's bytes read from it when the album is first
 * drawn (mu_library_cover_load; NULL until then), and its picture made from
 * them (its pixels NULL until then, or when it cannot be made:
 * picture_tried says it was tried).  The library owns the strings and the
 * bytes; the view makes the picture and releases it (mu_view_release,
 * mu_view_forget_pictures).
 */
struct mu_album {
	char *title;
	char *artist;
	char *cover_path;
	unsigned char *cover;
	size_t cover_size;
	struct kl_image picture;
	int picture_tried;
};

/* The actions of the menu, the keys and the buttons. */
#define MU_ACTION_PLAY		1U	/* play or pause */
#define MU_ACTION_NEXT		2U
#define MU_ACTION_PREVIOUS	3U
#define MU_ACTION_QUIT		4U
#define MU_ACTION_SONG		5U	/* a song chosen (the request's song) */
#define MU_ACTION_SEEK		6U	/* the position moved (the request's seconds) */

/* The most requests the view queues for the window between two frames. */
#define MU_REQUESTS_MAX		8U

/* Something the view asks of the window: the action, the song and the position in seconds. */
struct mu_request {
	unsigned action;
	long song;
	double seconds;
};

/* The player's state as the view shows it. */
#define MU_STOPPED		0U
#define MU_PLAYING		1U
#define MU_PAUSED		2U

/* The side of an album's picture made from its cover (pixels). */
#define MU_COVER_SIDE		160

/*
 * The view's state.
 *
 * What is shown: the album chosen (-1 for every song), the search, the
 * scrolls of the albums and of the songs, and the song chosen in the list
 * (-1 for none).
 *
 * What plays, as the window last told: the song (-1 for none), the
 * player's state (MU_*), its position and the song's length (seconds),
 * and why playing cannot be (empty for nothing).
 *
 * The position's slider: its value, which follows the position except for
 * a while after the user moved it (until then), and a seek it asked that
 * is not yet sent with when the last one was.
 *
 * The requests for the window, the notice at the bottom until a time,
 * whether the window stands on glass, and whether the program is to end.
 */
struct mu_view {
	long album;
	struct kl_field search;
	struct kl_scroll albums_scroll;
	struct kl_scroll songs_scroll;
	long chosen;

	long playing;
	unsigned state;
	double position;
	double length;
	char problem[MU_TEXT_MAX];

	double slider;
	uint64_t slider_until;
	int seek_pending;
	uint64_t seek_at;

	struct mu_request requests[MU_REQUESTS_MAX];
	size_t request_count;
	char notice[160];
	uint64_t notice_until;
	int glass;
	int quit;

	/* The songs shown, the view's own list (as long as the collection), and whether a cover's failure was told. */
	size_t *shown;
	size_t shown_room;
	int cover_told;

	/*
	 * The bar's button last pressed (its widget id, 0 for none) and the
	 * frame's time it was counted at: a double click on a button whose
	 * first click was not counted just before came within one frame, which
	 * the input keeps as one click, and is two presses (ws177-p021).
	 */
	uint32_t bar_pressed;
	uint64_t bar_pressed_us;
};

/* The tags of a file (tags.c). */
int mu_tags_read(const char *path, struct mu_tags *tags);
void mu_tags_release(struct mu_tags *tags);

/* The collection (library.c). */
void mu_library_set_cache(const char *path);
int mu_library_scan(const char *folder);
int mu_library_changed(void);
int mu_library_rescan(void);
int mu_library_add_file(const char *path, long *song);
long mu_library_find(const char *path);
int mu_library_cover_load(size_t album);
const struct mu_song *mu_songs(size_t *count);
struct mu_album *mu_albums(size_t *count);
size_t mu_library_list(long album, const char *search, size_t *indices, size_t capacity);
long mu_library_next(long song, int step);
void mu_library_release(void);

/* The picture of a cover (cover.c). */
int mu_cover_picture(const unsigned char *data, size_t size, int side, struct kl_image *image);

/* The view (view.c). */
int mu_view_init(struct mu_view *view);
void mu_view_release(struct mu_view *view);
void mu_view_forget_pictures(struct mu_view *view);
void mu_view_action(struct mu_view *view, unsigned action, uint64_t now_us);
void mu_view_key(struct mu_view *view, uint32_t key, unsigned modifiers, uint64_t now_us);
int mu_view_wait(const struct mu_view *view, uint64_t now_us);
void mu_view_draw(struct mu_view *view, struct kl_ui *ui, const struct kl_style *style, int width, int height, uint64_t now_us);
size_t mu_view_panels(const struct mu_view *view, int width, int height, struct kl_glass_panel *panels, size_t capacity);
int mu_view_take_request(struct mu_view *view, struct mu_request *request);
void mu_view_notice(struct mu_view *view, const char *message, uint64_t now_us);

/* The log for the tests (main.c, and the host tests' own). */
void mu_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
