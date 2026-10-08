/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The glass look of window mode (ws035-p059, a proof of the look and feel):
 * its images and the shapes it draws.  The windows, title bars and system
 * bar that use them are in shell.c.
 *
 * The wallpaper is a picture given with --wallpaper, or a pale blue
 * landscape the CPU draws once.  A quarter-size, blurred copy of it is the
 * frosted glass: a glass panel samples it at its own place on the output and
 * whitens it (like Windows' Mica, only the wallpaper shows through; windows
 * under a panel do not).
 *
 * Text is drawn from a glyph atlas made with libtruetype from the font at
 * server->font_path (printable ASCII and the multiplication sign) and its
 * companions (ws090-p020: Mahora Bold, and the monospaced fallback for the
 * signs Mahora lacks); without a font the look is drawn without text.  Any other character (WS070 p009) is
 * rendered when it is first drawn, from that font or else from the fallback
 * font (server->fallback_font_path, for Japanese), into a cell of the
 * atlas's cache, the least recently drawn cell making room; text is UTF-8.
 * A character neither font has is looked for in the colour emoji font
 * (GLASS_EMOJI_FONT, opened on the first such character, ws102-p019),
 * whose colour glyph goes into the cell as it is and is drawn as an image.
 * The titlebar's icons (icons.c) are rendered into the atlas once, at two
 * sizes, and so are the seven layers of the Kei mark (artwork/mark.c,
 * ws035-p108), which the login and lock screens draw.  The applications'
 * tiles (icons.c, ws128-p012: the banded rounded square with the picture
 * cut out) are rendered in colour into an image of their own at each size
 * the compositor draws them (App Home, Alt+Tab, the system bar, the title
 * bars' marks) and drawn as images, so that what is behind a tile shows
 * through its picture.
 */

#include "glass.h"
#include "../artwork/mark.h"
#include "../picture/color-glyph.h"
#include "../picture/wallpaper.h"

#include <truetype/truetype.h>

#include "userland/desktop/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * The atlas holds printable ASCII, the multiplication sign (the close
 * button), the check mark and the right angle quote (the System Menu) at
 * five sizes, and at the sixth (App Home's clock, ws181-p006) only the
 * space, the digits and the colon: the glyphs up to GLASS_CLOCK_LAST.
 */
#define GLASS_GLYPHS		98U
#define GLASS_SIZES		6U
#define GLASS_CLOCK_LAST	(':' - 32U)
#define GLASS_ATLAS_WIDTH	1024U
#define GLASS_ATLAS_HEIGHT	1024U
#define GLASS_FILE_MAX		(16U * 1024U * 1024U)

/* The fonts kept open: the first font, the fallback and the colour emoji font (ws102-p019). */
#define GLASS_FACES		3U
#define GLASS_EMOJI_FONT	KEILAND_DATADIR "/fonts/keiland-emoji.ttf"

/* The icons' two sizes in pixels, and how many there are. */
#define GLASS_ICON_SIZES	2U

/*
 * The applications' tiles (ws128-p012): the sizes kept, in pixels a side
 * (a window's mark by its title and on Wiseview's tiles, the system bar's
 * applications, Alt+Tab, App Home), how many, the image they are kept in
 * and the empty pixels between them.
 */
#define GLASS_TILE_SIZES	4U
#define GLASS_TILE_WIDTH	1024U
#define GLASS_TILE_HEIGHT	256U
#define GLASS_TILE_GAP		2U

/*
 * How far inside an application's tile, a part of its side, the blurred
 * scene its picture shows is drawn (BUG-237): past the tile's smoothed
 * edge, so that none of it shows round the tile, and short of its picture
 * (about a sixth of the side in).
 */
#define GLASS_TILE_SCENE_INSET	0.08f

/* The bands an application tile's reflection on App Home's floor fades out in (ws099-p035b). */
#define GLASS_REFLECTION_SLICES	8U

/*
 * The large digits of the lock screen's clock (ws187-p001), in an image of
 * their own at the one size the clock asks for: the image's size, its
 * glyphs (the ten digits, then the colon), and the largest size and glyph
 * box (pixels) they are drawn at.
 */
#define GLASS_LARGE_WIDTH	2048U
#define GLASS_LARGE_HEIGHT	256U
#define GLASS_LARGE_GLYPHS	11U
#define GLASS_LARGE_COLON	10U
#define GLASS_LARGE_PIXELS_MOST	240U
#define GLASS_LARGE_SIDE	256U

/* A cached glyph's cell in the atlas, in pixels a side, and the most cells there are. */
#define GLASS_CELL		48U
#define GLASS_CELLS		512U

/* The Kei mark's layers in the atlas, in pixels a side. */
#define GLASS_MARK_PIXELS	128U

/*
 * The Kei mark's layers again at the system bar's launcher size, in pixels a
 * side (ws035-p117): drawn one to one, they stay crisp where the large ones
 * would be shrunk five times.  They sit in the free column right of the
 * large ones, GLASS_MARK_SMALL_ACROSS a row.
 */
#define GLASS_MARK_SMALL_PIXELS	26U
#define GLASS_MARK_SMALL_ACROSS	4U

/* The largest glyph rendered, in pixels a side. */
#define GLASS_BITMAP		64U

/* The blurred wallpaper is this many times smaller than the output. */
#define GLASS_BLUR_SCALE	4U
#define GLASS_BLUR_RADIUS	6U
#define GLASS_BLUR_PASSES	3U

/*
 * A picture decoded for the wallpaper (ws035-p133, ws138-p001): its pixels
 * (three bytes each, red, green and blue, the rows packed) and its size in
 * pixels.  data is NULL for the landscape drawn here.
 */
struct wallpaper_picture {
	unsigned char *data;
	uint32_t width;
	uint32_t height;
};

/*
 * The wallpaper's file read and decoded ahead on a thread of its own
 * (ws035-p133, ws138-p001), so that the disk and the decoding work while the
 * Vulkan device is made: the path, the picture (data NULL when it could not
 * be read or decoded), and whether the thread runs or has not been joined
 * yet.  Only the main thread starts and joins it; the thread writes the
 * picture before it ends, and pthread_join makes it visible to the main
 * thread.
 */
struct glass_prefetch {
	pthread_t thread;
	const char *path;
	struct wallpaper_picture picture;
	int started;
};

/*
 * A wallpaper chosen during the session, read and decoded on a thread of
 * its own (WS135, plan/ws135/design.md section 4.4) so that the event loop
 * never waits on the disk: the path, the picture or the error once the
 * thread is done, and whether it runs or has not been joined yet.  lock
 * guards done; only the event loop starts it (kwl_glass_wallpaper_begin)
 * and joins it (kwl_glass_wallpaper_poll, kwl_glass_close).
 */
struct glass_loader {
	pthread_t thread;
	pthread_mutex_t lock;
	char path[256];
	struct wallpaper_picture picture;
	int error;
	int done;
	int started;
};

struct glass_glyph {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	int32_t left;
	int32_t top;
	int32_t advance;
	uint32_t color;
};

/*
 * One cell of the atlas's cache: the character and size whose glyph it
 * holds (a codepoint of 0 is a free cell), the glyph's place and metrics,
 * and when it was last drawn.
 */
struct glass_cached {
	uint32_t codepoint;
	unsigned size;
	struct glass_glyph glyph;
	uint64_t used;
};

/*
 * The look's images and glyphs: the wallpaper and its blur, the atlas with
 * the ASCII glyphs at each size, the icons at their sizes, the
 * applications' tiles in their own image (tiles_ready once they are drawn
 * there), the Kei mark's
 * layers large and at the launcher's size (ws035-p117), and the cache of
 * other characters; the fonts stay open (with their files' bytes) to
 * render the cache's glyphs.  cache_top is the atlas row the cache starts
 * at, cache_count the cells that fit under it, clock the count of cached
 * glyphs drawn (the cells' use is ordered by it).  The large digits of the
 * lock screen's clock (ws187-p001) have their image, made when the clock
 * first asks, and are drawn again only when it asks for another size
 * (large_pixels, 0 while none is drawn).
 */
struct kwl_glass {
	struct kwl_import wallpaper;
	struct kwl_import blurred;
	struct kwl_import atlas;
	struct glass_glyph glyphs[GLASS_SIZES][GLASS_GLYPHS];
	struct glass_glyph icons[GLASS_ICON_SIZES][GLASS_ICON_COUNT];
	struct kwl_import tiles;
	struct glass_glyph app_tiles[GLASS_TILE_SIZES][GLASS_ICON_APPS];
	unsigned tiles_ready;
	struct glass_glyph mark[KL_MARK_LAYERS];
	struct glass_glyph mark_small[KL_MARK_LAYERS];
	unsigned text;
	struct truetype_face *faces[GLASS_FACES];
	void *font_data[GLASS_FACES];
	unsigned face_count;
	const char *fallback_path;
	unsigned fallback_tried;
	unsigned emoji_tried;
	int emoji_face;
	struct glass_cached cache[GLASS_CELLS];
	uint32_t cache_top;
	unsigned cache_count;
	uint64_t clock;
	struct kwl_import large;
	struct glass_glyph large_glyphs[GLASS_LARGE_GLYPHS];
	unsigned large_pixels;
};

static const unsigned glass_pixels[GLASS_SIZES] = { 14U, 15U, 20U, 36U, 24U, 64U };

/* The icons' sizes in pixels. */
static const unsigned glass_icon_pixels[GLASS_ICON_SIZES] = { 16U, 20U };

/* The applications' tiles' sizes in pixels, smallest first. */
static const unsigned glass_tile_pixels[GLASS_TILE_SIZES] = { 20U, 26U, 48U, 72U };

static int wallpaper_create(struct kwl_server *server, struct kwl_glass *glass);
static int wallpaper_fill(struct kwl_server *server, struct kwl_glass *glass, const char *path);
static int wallpaper_draw(struct kwl_server *server, struct kwl_glass *glass, struct wallpaper_picture *given);
static void wallpaper_pixel(uint32_t x, uint32_t y, uint32_t width, uint32_t height, float *rgb);
static float ridge(float x, float base, float amplitude, float phase);
static int blur_create(struct kwl_server *server, struct kwl_glass *glass);
static void wallpaper_row(const struct wallpaper_picture *picture, const uint32_t *columns, uint32_t source_y, uint32_t *line, uint32_t width);
static void landscape_row(uint32_t y, uint32_t width, uint32_t height, uint32_t *line);
static void blur_add(const uint32_t *line, uint32_t *sums, uint32_t small_width);
static void blur_average(uint32_t *sums, float *small, uint32_t small_width);
static int blur_fill(struct kwl_server *server, struct kwl_glass *glass, float *small);
static void blur_pass(float *pixels, float *scratch, uint32_t width, uint32_t height, int horizontal);
static uint32_t pack_pixel(const float *rgb);
static int atlas_create(struct kwl_server *server, struct kwl_glass *glass);
static int atlas_fill(struct kwl_glass *glass, struct truetype_face *face);
static int atlas_icons(struct kwl_glass *glass, uint32_t *pen_y);
static int atlas_mark(struct kwl_glass *glass, uint32_t *pen_y);
static int tiles_create(struct kwl_server *server, struct kwl_glass *glass);
static int large_fill(struct kwl_glass *glass, struct truetype_face *face, unsigned pixels);
static void large_put(struct kwl_glass *glass, const uint8_t *bitmap, uint32_t pen_x, uint32_t width, uint32_t height);
static const struct glass_glyph *large_glyph_of(struct kwl_glass *glass, uint32_t character);
static void atlas_put(struct kwl_glass *glass, const uint8_t *bitmap, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
static int glass_open_face(struct kwl_glass *glass, const char *path);
static const struct glass_glyph *glass_glyph_of(struct kwl_glass *glass, enum glass_size size, uint32_t codepoint);
static const struct glass_glyph *glass_cache_glyph(struct kwl_glass *glass, enum glass_size size, uint32_t codepoint);
static const struct glass_glyph *glass_cache_color(struct kwl_glass *glass, enum glass_size size, uint32_t codepoint, unsigned id, unsigned slot);
static uint32_t glass_utf8_next(const char **text);
static void glass_draw_glyph_at(struct kwl_server *server, VkCommandBuffer command, const struct glass_glyph *glyph, int32_t x, int32_t baseline, const float *color);
static void *file_read(const char *path, size_t *size);
static int wallpaper_load(const char *path, struct wallpaper_picture *picture);
static int wallpaper_read(const char *path, struct wallpaper_picture *picture);
static void *loader_run(void *argument);
static void loader_join(void);
static void *prefetch_run(void *argument);
static int prefetch_take(const char *path, struct wallpaper_picture *picture);
static void prefetch_drop(void);
static void glass_dark_color(const float *color, float *dark);

/* The wallpaper read ahead, when kwl_glass_prefetch started it (only the main thread starts and takes it). */
static struct glass_prefetch glass_prefetch;

/*
 * The wallpaper chosen during the session being read (only the event loop
 * starts and joins it); its lock is made with the first one.
 */
static struct glass_loader glass_loader;

/* Whether glass_loader's lock has been made (once, by the first begin). */
static int glass_loader_ready;

/*
 * Starts reading and decoding the wallpaper's file on a thread (ws035-p133,
 * ws138-p001), before the Vulkan device is made; the look takes the picture
 * when it draws the wallpaper.  Without a thread the look reads the file
 * itself.
 */
void
kwl_glass_prefetch(
	struct kwl_server *server)
{
	int error;

	/* Only the glass look with a picture reads one. */
	if (!server->glass || server->wallpaper_path == NULL)
		return;

	/* The thread; without it the file is read when the wallpaper is drawn. */
	glass_prefetch.path = server->wallpaper_path;
	error = pthread_create(&glass_prefetch.thread, NULL, prefetch_run, NULL);
	if (error != 0) {
		printf("KWL GLASS prefetch unavailable errno=%d\n", error);
		return;
	}

	/* The thread runs until the look takes its picture. */
	glass_prefetch.started = 1;
}

/*
 * Makes the wallpaper, its blurred copy and the glyph atlas.
 */
int
kwl_glass_open(
	struct kwl_server *server)
{
	struct kwl_glass *glass;
	uint64_t started;
	int error;

	/* The look's state lives as long as window mode's device. */
	glass = calloc(1, sizeof(*glass));
	if (glass == NULL)
		return ENOMEM;
	server->compose->glass = glass;

	/* The wallpaper and the frosted glass made from it. */
	started = kwl_milliseconds();
	error = wallpaper_create(server, glass);
	if (error != 0)
		return error;
	printf("KWL STARTUP step=wallpaper ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

	/* The glyphs; the look is drawn without text when the font cannot be read. */
	started = kwl_milliseconds();
	error = atlas_create(server, glass);
	if (error != 0)
		printf("KWL GLASS no text: font=%s errno=%d\n", server->font_path, error);
	printf("KWL STARTUP step=glyphs ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

	/* The applications' tiles; without them the marks are drawn without pictures. */
	started = kwl_milliseconds();
	error = tiles_create(server, glass);
	if (error != 0)
		printf("KWL GLASS no tiles: errno=%d\n", error);
	printf("KWL STARTUP step=tiles ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

	/* Succeeded. */
	printf("KWL GLASS ready text=%u\n", glass->text);
	return 0;
}

/*
 * Releases the look's images.
 */
void
kwl_glass_close(
	struct kwl_server *server)
{
	struct kwl_glass *glass;
	unsigned face;

	/* A wallpaper still being read is waited for, and its picture let go. */
	loader_join();
	free(glass_loader.picture.data);
	glass_loader.picture.data = NULL;

	/* A picture read ahead that the look never took is let go too. */
	prefetch_drop();

	/* Nothing was made. */
	if (server->compose == NULL || server->compose->glass == NULL)
		return;

	/* The fonts kept open for the cache. */
	glass = server->compose->glass;
	for (face = 0; face < glass->face_count; face++) {
		truetype_close(glass->faces[face]);
		free(glass->font_data[face]);
	}

	/* The images, then the record. */
	kwl_host_image_release(server->compose, &glass->wallpaper);
	kwl_host_image_release(server->compose, &glass->blurred);
	kwl_host_image_release(server->compose, &glass->atlas);
	kwl_host_image_release(server->compose, &glass->tiles);
	kwl_host_image_release(server->compose, &glass->large);
	free(glass);
	server->compose->glass = NULL;
}

/*
 * Draws the landscape into the look again (ws089-p007, the desktop's
 * preferences going back to no picture).  No file is read, so the event
 * loop does it at once; a picture is read on a thread instead
 * (kwl_glass_wallpaper_begin, ws138-p001 U7).
 *
 * The images keep their size and their descriptors, so only their pixels
 * change; the device finishes what it is drawing from them first.  The
 * whole output is drawn again.  Returns 0 or an errno value.
 */
int
kwl_glass_landscape(
	struct kwl_server *server)
{
	struct wallpaper_picture picture;
	uint64_t started;
	int error;

	/* Without the look there is no wallpaper. */
	if (server->compose == NULL || server->compose->glass == NULL)
		return ENODEV;

	/* The frames in flight read the images; they end first. */
	started = kwl_milliseconds();
	(void)vkDeviceWaitIdle(server->compose->device);

	/* The landscape is a picture without pixels. */
	memset(&picture, 0, sizeof(picture));

	/* Draws it into the wallpaper's image and its blurred copy. */
	error = wallpaper_draw(server, server->compose->glass, &picture);
	if (error != 0)
		return error;

	/* Everything on the output stands on it. */
	server->dirty = 1;

	/* The log names the landscape ("-") and how long it took. */
	printf("KWL GLASS wallpaper path=- ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

	/* Succeeded: the landscape is shown from the next frame. */
	return 0;
}

/*
 * Makes the look's output-sized images again for an output of another size
 * (ws113-p004a): the wallpaper and its blurred copy, with the picture the
 * settings hold now.  The device finishes what it is drawing from them
 * first.  Returns 0 or an errno value (the look is then drawn without its
 * wallpaper until the next change).
 */
int
kwl_glass_resize(
	struct kwl_server *server)
{
	struct kwl_glass *glass;
	uint64_t started;
	int error;

	/* Without the look there is no wallpaper. */
	if (server->compose == NULL || server->compose->glass == NULL)
		return 0;
	glass = server->compose->glass;

	/* The frames in flight read the images; they end first. */
	started = kwl_milliseconds();
	(void)vkDeviceWaitIdle(server->compose->device);

	/* The images of the old size go. */
	kwl_host_image_release(server->compose, &glass->wallpaper);
	kwl_host_image_release(server->compose, &glass->blurred);

	/* The images of the new size, with the picture in them. */
	error = wallpaper_create(server, glass);
	if (error != 0) {
		printf("KWL GLASS resize errno=%d\n", error);
		return error;
	}

	/* Everything stands on them. */
	server->dirty = 1;
	printf("KWL GLASS resize width=%u height=%u ms=%llu\n", server->width, server->height, (unsigned long long)(kwl_milliseconds() - started));

	/* Succeeded: the look fits the output. */
	return 0;
}

/*
 * Starts reading a wallpaper chosen during the session on a thread of its
 * own (WS135): the event loop goes on, and kwl_glass_wallpaper_poll shows
 * the picture once it is read.  Returns 0, EBUSY while another is being
 * read, EINVAL for a path that is not an ordinary file (a FIFO, a device,
 * a folder) or too long, ENODEV without the look, or the errno value of
 * looking at the file or of the thread.
 */
int
kwl_glass_wallpaper_begin(
	struct kwl_server *server,
	const char *path)
{
	struct stat status;
	size_t length;
	int descriptor;
	int regular;
	int error;

	/* Without the look there is no wallpaper. */
	if (server->compose == NULL || server->compose->glass == NULL)
		return ENODEV;

	/* One picture at a time. */
	if (glass_loader.started)
		return EBUSY;

	/* A path that fits. */
	length = strlen(path);
	if (length >= sizeof(glass_loader.path))
		return EINVAL;

	/* Opens the path without waiting, so that a FIFO or a device is refused now. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (descriptor < 0)
		return errno;

	/* Looks at what the path names, then lets the descriptor go. */
	error = fstat(descriptor, &status);
	(void)close(descriptor);
	if (error != 0)
		return EINVAL;

	/* Refuses anything but an ordinary file. */
	regular = S_ISREG(status.st_mode);
	if (!regular)
		return EINVAL;

	/* Makes the lock with the first picture. */
	if (!glass_loader_ready) {
		error = pthread_mutex_init(&glass_loader.lock, NULL);
		if (error != 0)
			return error;

		/* Later begins reuse the lock made here. */
		glass_loader_ready = 1;
	}

	/* Gives the thread the path and an empty result to fill. */
	memcpy(glass_loader.path, path, length + 1U);
	memset(&glass_loader.picture, 0, sizeof(glass_loader.picture));
	glass_loader.error = 0;
	glass_loader.done = 0;

	/* Starts the thread that reads and decodes the picture. */
	error = pthread_create(&glass_loader.thread, NULL, loader_run, NULL);
	if (error != 0)
		return error;

	/*
	 * started tells poll and close that a thread runs or waits to be
	 * joined, and refuses another begin until then.
	 */
	glass_loader.started = 1;

	/* Succeeded: the picture is being read. */
	return 0;
}

/*
 * Shows a wallpaper kwl_glass_wallpaper_begin read, once its thread is
 * done.  Returns 0 while there is none or it is still being read, or 1
 * with *error 0 (the picture is shown) or the errno value of reading or
 * drawing it (the wallpaper shown stays).
 */
int
kwl_glass_wallpaper_poll(
	struct kwl_server *server,
	int *error)
{
	struct wallpaper_picture picture;
	uint64_t started;
	int done;

	/* Nothing is being read. */
	if (!glass_loader.started)
		return 0;

	/* Samples whether the thread is done. */
	(void)pthread_mutex_lock(&glass_loader.lock);

	done = glass_loader.done;

	(void)pthread_mutex_unlock(&glass_loader.lock);

	/* Still reading. */
	if (!done)
		return 0;

	/*
	 * Joins the finished thread; clearing started lets the next begin in,
	 * and the picture is the event loop's now.
	 */
	(void)pthread_join(glass_loader.thread, NULL);
	glass_loader.started = 0;
	picture = glass_loader.picture;
	memset(&glass_loader.picture, 0, sizeof(glass_loader.picture));

	/* A picture that could not be read is not shown. */
	*error = glass_loader.error;
	if (*error != 0) {
		printf("KWL GLASS no wallpaper: path=%s errno=%d\n", glass_loader.path, *error);
		return 1;
	}

	/* The look went meanwhile. */
	if (server->compose == NULL || server->compose->glass == NULL) {
		free(picture.data);
		*error = ENODEV;
		return 1;
	}

	/* The frames in flight read the images; they end first, then the picture is drawn. */
	started = kwl_milliseconds();
	(void)vkDeviceWaitIdle(server->compose->device);
	*error = wallpaper_draw(server, server->compose->glass, &picture);
	if (*error != 0)
		return 1;

	/* Redraws the output, since everything on it stands on the wallpaper. */
	server->dirty = 1;

	/* Logs how long the drawing took, for the tests. */
	printf("KWL GLASS wallpaper path=%s ms=%llu\n", glass_loader.path, (unsigned long long)(kwl_milliseconds() - started));

	/* Succeeded: the new wallpaper is shown from the next frame. */
	return 1;
}

/* Makes the wallpaper's image and its blurred copy, and draws them. */
static int
wallpaper_create(
	struct kwl_server *server,
	struct kwl_glass *glass)
{
	VkResult result;
	int error;

	/* The output-sized image. */
	result = kwl_host_image_create(server->compose, server->width, server->height, server->compose->sampler, &glass->wallpaper);
	if (result != VK_SUCCESS)
		return EIO;

	/* The small image of the frosted glass. */
	error = blur_create(server, glass);
	if (error != 0)
		return error;

	/* The picture in both. */
	error = wallpaper_fill(server, glass, server->wallpaper_path);
	if (error != 0)
		return error;

	/* Succeeded: the wallpaper is drawn. */
	return 0;
}

/*
 * Draws the start's picture (a PNG or a JPEG, ws138-p001; or the landscape
 * drawn here when path is NULL or cannot be read) into the wallpaper's image
 * and its blurred copy, which are mapped, the output's size, and kept for
 * the look's life.
 *
 * The picture is made one output row at a time (ws035-p133): each row is
 * packed, copied into the image (which is only written, never read), and
 * added to the frosted glass's block averages, so no output-sized copy of
 * the picture is kept.  A wallpaper chosen later is read on a thread
 * (kwl_glass_wallpaper_begin) and drawn by wallpaper_draw.
 */
static int
wallpaper_fill(
	struct kwl_server *server,
	struct kwl_glass *glass,
	const char *path)
{
	struct wallpaper_picture picture;
	int error;

	/* The picture given, when it can be read; otherwise the landscape drawn here. */
	memset(&picture, 0, sizeof(picture));
	if (path != NULL) {
		error = wallpaper_load(path, &picture);
		if (error != 0)
			printf("KWL GLASS no wallpaper: path=%s errno=%d\n", path, error);
	}

	/* The picture into both images. */
	error = wallpaper_draw(server, glass, &picture);
	if (error != 0)
		return error;

	/* Succeeded: both images hold the picture. */
	return 0;
}

/*
 * Draws a picture read already (data NULL: the landscape) into the
 * wallpaper's image and its blurred copy, one output row at a time, and
 * lets the picture's bytes go.
 */
static int
wallpaper_draw(
	struct kwl_server *server,
	struct kwl_glass *glass,
	struct wallpaper_picture *given)
{
	struct wallpaper_picture picture;
	uint32_t *columns;
	uint32_t *line;
	uint32_t *sums;
	uint32_t *row;
	float *small;
	uint32_t width;
	uint32_t height;
	uint32_t small_width;
	uint32_t small_height;
	uint32_t x;
	uint32_t y;
	uint32_t source_y;
	uint32_t made_y;
	uint64_t started;
	int error;

	/* The picture is this function's from now on. */
	picture = *given;
	given->data = NULL;

	/* The sizes: the output's, and the frosted glass's. */
	started = kwl_milliseconds();
	width = server->width;
	height = server->height;
	small_width = width / GLASS_BLUR_SCALE;
	small_height = height / GLASS_BLUR_SCALE;

	/* One output row, each output column's source column, a block row's sums, and the small image. */
	line = malloc((size_t)width * sizeof(*line));
	columns = malloc((size_t)width * sizeof(*columns));
	sums = calloc((size_t)small_width * 3U, sizeof(*sums));
	small = calloc((size_t)small_width * small_height * 3U, sizeof(*small));
	if (line == NULL || columns == NULL || sums == NULL || small == NULL) {
		free(line);
		free(columns);
		free(sums);
		free(small);
		free(picture.data);
		return ENOMEM;
	}

	/* Each output column takes the source column it falls on (the nearest pixel), as a byte offset. */
	if (picture.data != NULL) {
		for (x = 0; x < width; x++)
			columns[x] = (uint32_t)((uint64_t)x * picture.width / width) * 3U;
	}

	/*
	 * Each output row: made (a row that falls on the same source row as
	 * the one above is that row again), copied into the image, and added to
	 * the block of the frosted glass it lies in.
	 */
	made_y = UINT32_MAX;
	for (y = 0; y < height; y++) {
		if (picture.data != NULL) {
			/* The picture's row, unless the line already holds it. */
			source_y = (uint32_t)((uint64_t)y * picture.height / height);
			if (source_y != made_y)
				wallpaper_row(&picture, columns, source_y, line, width);
			made_y = source_y;
		} else {
			/* The landscape's row. */
			landscape_row(y, width, height, line);
		}

		/* Into the image. */
		row = (uint32_t *)((unsigned char *)glass->wallpaper.map + (size_t)y * glass->wallpaper.row_pitch);
		memcpy(row, line, (size_t)width * sizeof(*line));

		/* Rows below the last whole block are not in the frosted glass. */
		if (y >= small_height * GLASS_BLUR_SCALE)
			continue;

		/* Into its block; a block's last row makes that row of the small image. */
		blur_add(line, sums, small_width);
		if (y % GLASS_BLUR_SCALE == GLASS_BLUR_SCALE - 1U)
			blur_average(sums, &small[(size_t)(y / GLASS_BLUR_SCALE) * small_width * 3U], small_width);
	}

	/* The rows are made; the picture and the working rows are no longer needed. */
	free(line);
	free(columns);
	free(sums);
	free(picture.data);
	printf("KWL STARTUP step=wallpaper-picture ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

	/* The frosted glass. */
	error = blur_fill(server, glass, small);
	free(small);
	if (error != 0)
		return error;

	/* Succeeded: both images hold the picture. */
	return 0;
}

/* Packs one output row from a source row of the picture, each column from its source column. */
static void
wallpaper_row(
	const struct wallpaper_picture *picture,
	const uint32_t *columns,
	uint32_t source_y,
	uint32_t *line,
	uint32_t width)
{
	const unsigned char *source;
	const unsigned char *pixel;
	uint32_t x;

	/* The source row's first byte. */
	source = picture->data + (size_t)source_y * picture->width * 3U;

	/* Each pixel as opaque BGRA (A, R, G, B from the top byte), as pack_pixel would make it. */
	for (x = 0; x < width; x++) {
		pixel = source + columns[x];
		line[x] = 0xff000000U | ((uint32_t)pixel[0] << 16) | ((uint32_t)pixel[1] << 8) | (uint32_t)pixel[2];
	}
}

/* Packs one output row of the landscape drawn here. */
static void
landscape_row(
	uint32_t y,
	uint32_t width,
	uint32_t height,
	uint32_t *line)
{
	float rgb[3];
	uint32_t x;

	/* Each pixel colored, then packed. */
	for (x = 0; x < width; x++) {
		wallpaper_pixel(x, y, width, height, rgb);
		line[x] = pack_pixel(rgb);
	}
}

/*
 * Colors one pixel of the landscape in the Kei look (ws035-p108,
 * plan/ws035/kei-identity-design.md): a bright sky that pales towards the
 * horizon with a soft sun, three ranges of misty hills (pale blue far away,
 * young green near), a still lake that reflects them, soft green leaves at
 * the top left and along the foot with a few blurred white flowers, and the
 * whole a little whitened, as if seen through haze.
 */
static void
wallpaper_pixel(
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height,
	float *rgb)
{
	static const float ranges[3][5] = {
		/* base, amplitude, phase, and the color's green and blue (red is below). */
		{ 0.40f, 0.08f, 0.3f, 0.86f, 0.95f },
		{ 0.47f, 0.06f, 2.1f, 0.81f, 0.86f },
		{ 0.54f, 0.04f, 4.7f, 0.80f, 0.68f }
	};
	static const float reds[3] = { 0.78f, 0.68f, 0.64f };

	/* The blurred white flowers along the foot: where (fractions of the output) and how large. */
	static const float flowers[6][3] = {
		{ 0.06f, 0.92f, 0.030f },
		{ 0.15f, 0.97f, 0.025f },
		{ 0.24f, 0.90f, 0.020f },
		{ 0.33f, 0.98f, 0.022f },
		{ 0.80f, 0.95f, 0.024f },
		{ 0.93f, 0.90f, 0.028f }
	};
	float u;
	float v;
	float mirror;
	float top;
	float mist;
	float sun;
	float lake;
	float leaves;
	float bloom;
	float dx;
	float dy;
	unsigned index;

	/* The place, as fractions of the output. */
	u = (float)x / (float)width;
	v = (float)y / (float)height;
	lake = 0.62f;

	/* Under the lake's edge the landscape is seen in the water. */
	mirror = v;
	if (v > lake)
		mirror = 2.0f * lake - v;

	/* The sky: light blue above, nearly white at the horizon, a glow to the upper right. */
	rgb[0] = 0.76f + 0.22f * mirror;
	rgb[1] = 0.87f + 0.11f * mirror;
	rgb[2] = 0.98f + 0.01f * mirror;
	sun = expf(-((u - 0.78f) * (u - 0.78f) * 18.0f + (mirror - 0.16f) * (mirror - 0.16f) * 30.0f));
	rgb[0] += 0.12f * sun;
	rgb[1] += 0.09f * sun;
	rgb[2] += 0.03f * sun;

	/* Each range in front of the ones behind it, misty towards its foot. */
	for (index = 0; index < 3U; index++) {
		top = ridge(u, ranges[index][0], ranges[index][1], ranges[index][2]);
		if (mirror < top)
			continue;
		mist = (mirror - top) / 0.14f;
		if (mist > 1.0f)
			mist = 1.0f;
		rgb[0] = reds[index] + (0.93f - reds[index]) * mist * 0.55f;
		rgb[1] = ranges[index][3] + (0.96f - ranges[index][3]) * mist * 0.55f;
		rgb[2] = ranges[index][4] + (0.99f - ranges[index][4]) * mist * 0.55f;
	}

	/* The water is a little bluer than what it reflects, deeper towards the bottom. */
	if (v > lake) {
		rgb[0] = rgb[0] * 0.94f - (v - lake) * 0.10f;
		rgb[1] = rgb[1] * 0.97f - (v - lake) * 0.04f;
		rgb[2] = rgb[2] * 1.00f - (v - lake) * 0.02f;
		rgb[0] += 0.004f * sinf((float)y * 0.9f + u * 3.0f);
		rgb[1] += 0.004f * sinf((float)y * 0.9f + u * 3.0f);
	}

	/* Leaves: out of focus at the top left, and a meadow rising at both lower corners. */
	dx = u - 0.02f;
	dy = v - 0.04f;
	leaves = 0.75f * expf(-(dx * dx * 14.0f + dy * dy * 22.0f));
	top = 0.84f - 0.10f * (2.0f * fabsf(u - 0.5f));
	if (v > top) {
		mist = (v - top) / 0.16f;
		if (mist > 1.0f)
			mist = 1.0f;
		leaves += 0.85f * mist;
	}

	/* The leaves' colour over the scene, as much as they cover. */
	if (leaves > 1.0f)
		leaves = 1.0f;
	rgb[0] += (0.66f - rgb[0]) * leaves;
	rgb[1] += (0.82f - rgb[1]) * leaves;
	rgb[2] += (0.58f - rgb[2]) * leaves;

	/* The white flowers, blurred, among the leaves along the foot. */
	for (index = 0; index < 6U; index++) {
		dx = (u - flowers[index][0]) * (float)width / (float)height;
		dy = v - flowers[index][1];
		bloom = 0.85f * expf(-(dx * dx + dy * dy) / (flowers[index][2] * flowers[index][2]));
		rgb[0] += (0.98f - rgb[0]) * bloom;
		rgb[1] += (0.99f - rgb[1]) * bloom;
		rgb[2] += (0.97f - rgb[2]) * bloom;
	}

	/* The haze over everything: a fifth of the way to white. */
	for (index = 0; index < 3U; index++)
		rgb[index] += (1.0f - rgb[index]) * 0.20f;

	/* Each channel within 0..1. */
	for (index = 0; index < 3U; index++) {
		if (rgb[index] > 1.0f)
			rgb[index] = 1.0f;
		if (rgb[index] < 0.0f)
			rgb[index] = 0.0f;
	}
}

/* The height (as a fraction of the output) of a mountain range's ridge at x. */
static float
ridge(
	float x,
	float base,
	float amplitude,
	float phase)
{
	float wave;
	float peak;

	/* A few waves of falling length make saddles, and a sharp term the peaks. */
	wave = 0.55f * sinf(x * 5.1f + phase);
	wave += 0.30f * sinf(x * 11.3f + phase * 1.7f);
	wave += 0.15f * sinf(x * 23.9f + phase * 2.3f);
	peak = 1.0f - fabsf(sinf(x * 4.3f + phase * 0.7f));
	wave += 0.60f * peak * peak * peak;

	/* Higher waves are lower on the output. */
	return base - amplitude * wave;
}

/* Makes the frosted glass's small image, sampled linearly (blur_fill draws it). */
static int
blur_create(
	struct kwl_server *server,
	struct kwl_glass *glass)
{
	VkResult result;

	/* The output's size divided by GLASS_BLUR_SCALE. */
	result = kwl_host_image_create(server->compose, server->width / GLASS_BLUR_SCALE, server->height / GLASS_BLUR_SCALE, server->compose->linear_sampler, &glass->blurred);
	if (result != VK_SUCCESS)
		return EIO;

	/* Succeeded: the image is there. */
	return 0;
}

/*
 * Draws the frosted glass from its small image (the wallpaper averaged down
 * by GLASS_BLUR_SCALE, blur_average): blurred by repeated box passes (close
 * to a Gaussian) in place, then packed into the image.
 */
static int
blur_fill(
	struct kwl_server *server,
	struct kwl_glass *glass,
	float *small)
{
	uint32_t *row;
	float *scratch;
	uint32_t width;
	uint32_t height;
	uint32_t x;
	uint32_t y;
	unsigned pass;

	/* The small image's size. */
	width = server->width / GLASS_BLUR_SCALE;
	height = server->height / GLASS_BLUR_SCALE;

	/* The passes' working copy. */
	scratch = calloc((size_t)width * height * 3U, sizeof(float));
	if (scratch == NULL)
		return ENOMEM;

	/* Box passes across and down. */
	for (pass = 0; pass < GLASS_BLUR_PASSES; pass++) {
		blur_pass(small, scratch, width, height, 1);
		blur_pass(small, scratch, width, height, 0);
	}

	/* Into the image. */
	for (y = 0; y < height; y++) {
		row = (uint32_t *)((unsigned char *)glass->blurred.map + y * glass->blurred.row_pitch);
		for (x = 0; x < width; x++)
			row[x] = pack_pixel(&small[((size_t)y * width + x) * 3U]);
	}

	/* Succeeded. */
	free(scratch);
	return 0;
}

/* Adds a packed output row to the sums of the blocks it crosses (three channels a block, red first). */
static void
blur_add(
	const uint32_t *line,
	uint32_t *sums,
	uint32_t small_width)
{
	uint32_t pixel;
	uint32_t x;
	uint32_t dx;

	/* Each block's GLASS_BLUR_SCALE pixels of this row. */
	for (x = 0; x < small_width; x++) {
		for (dx = 0; dx < GLASS_BLUR_SCALE; dx++) {
			pixel = line[x * GLASS_BLUR_SCALE + dx];
			sums[x * 3U] += (pixel >> 16) & 0xffU;
			sums[x * 3U + 1U] += (pixel >> 8) & 0xffU;
			sums[x * 3U + 2U] += pixel & 0xffU;
		}
	}
}

/* Makes one row of the small image from a block row's sums (0..1 each), and clears the sums for the next. */
static void
blur_average(
	uint32_t *sums,
	float *small,
	uint32_t small_width)
{
	uint32_t index;

	/* Each channel of each block: the average of its GLASS_BLUR_SCALE squared pixels. */
	for (index = 0; index < small_width * 3U; index++) {
		small[index] = (float)sums[index] / (255.0f * (float)(GLASS_BLUR_SCALE * GLASS_BLUR_SCALE));
		sums[index] = 0;
	}
}

/* Averages each pixel with its GLASS_BLUR_RADIUS neighbours on each side, across or down (clamped at the edges). */
static void
blur_pass(
	float *pixels,
	float *scratch,
	uint32_t width,
	uint32_t height,
	int horizontal)
{
	uint32_t x;
	uint32_t y;
	uint32_t channel;
	int32_t offset;
	int32_t sx;
	int32_t sy;
	float sum;

	/* Each output pixel from the input. */
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			for (channel = 0; channel < 3U; channel++) {
				sum = 0.0f;
				for (offset = -(int32_t)GLASS_BLUR_RADIUS; offset <= (int32_t)GLASS_BLUR_RADIUS; offset++) {
					/* The neighbour, clamped to the image. */
					sx = (int32_t)x;
					sy = (int32_t)y;
					if (horizontal)
						sx += offset;
					else
						sy += offset;
					if (sx < 0)
						sx = 0;
					if (sy < 0)
						sy = 0;
					if (sx >= (int32_t)width)
						sx = (int32_t)width - 1;
					if (sy >= (int32_t)height)
						sy = (int32_t)height - 1;
					sum += pixels[((size_t)sy * width + (size_t)sx) * 3U + channel];
				}

				/* The average of the window. */
				scratch[((size_t)y * width + x) * 3U + channel] = sum / (float)(2U * GLASS_BLUR_RADIUS + 1U);
			}
		}
	}

	/* The result replaces the input. */
	memcpy(pixels, scratch, (size_t)width * height * 3U * sizeof(float));
}

/* Packs a color (0..1 each) as an opaque BGRA pixel. */
static uint32_t
pack_pixel(
	const float *rgb)
{
	uint32_t red;
	uint32_t green;
	uint32_t blue;

	/* Each channel rounded to eight bits. */
	red = (uint32_t)(rgb[0] * 255.0f + 0.5f);
	green = (uint32_t)(rgb[1] * 255.0f + 0.5f);
	blue = (uint32_t)(rgb[2] * 255.0f + 0.5f);

	/* A, R, G, B from the top byte (B, G, R, A in memory). */
	return 0xff000000U | (red << 16) | (green << 8) | blue;
}

/*
 * Makes the glyph atlas from the font.  Returns 0 with text available, or an
 * error with the look to be drawn without text.
 */
static int
atlas_create(
	struct kwl_server *server,
	struct kwl_glass *glass)
{
	VkResult result;
	int error;

	/* The first font, kept open for the cache's glyphs. */
	error = glass_open_face(glass, server->font_path);
	if (error != 0)
		return error;

	/* Its companions: Mahora Bold, and the monospaced fallback for the signs it lacks (ws090-p020). */
	(void)truetype_open_companions(glass->faces[0], KEILAND_FONT_BOLD, KEILAND_FONT_FALLBACK_MONO);

	/*
	 * The fallback font is opened on the first character the first font
	 * lacks (glass_cache_glyph): reading its megabytes here would delay
	 * the compositor's start.
	 */
	glass->fallback_path = server->fallback_font_path;
	glass->fallback_tried = 0;

	/* The atlas image, transparent where nothing is drawn. */
	result = kwl_host_image_create(server->compose, GLASS_ATLAS_WIDTH, GLASS_ATLAS_HEIGHT, server->compose->sampler, &glass->atlas);
	if (result != VK_SUCCESS)
		return EIO;

	/* The glyphs at each size, the icons and the cache's place after them. */
	error = atlas_fill(glass, glass->faces[0]);
	if (error != 0)
		return error;

	/* Succeeded. */
	glass->text = 1;
	return 0;
}

/* Renders every glyph at every size into the atlas, row by row. */
static int
atlas_fill(
	struct kwl_glass *glass,
	struct truetype_face *face)
{
	static uint8_t bitmap[64U * 64U];
	struct truetype_glyph metrics;
	struct glass_glyph *glyph;
	uint32_t *row;
	uint32_t codepoint;
	uint32_t pen_x;
	uint32_t pen_y;
	uint32_t line;
	uint32_t x;
	uint32_t y;
	uint32_t value;
	unsigned size;
	unsigned index;
	unsigned id;
	uint64_t started;
	int error;

	/* The pen starts at the top left; each row is as tall as its tallest glyph. */
	started = kwl_milliseconds();
	memset(glass->atlas.map, 0, glass->atlas.row_pitch * GLASS_ATLAS_HEIGHT);
	pen_x = 0;
	pen_y = 0;
	line = 0;
	for (size = 0; size < GLASS_SIZES; size++) {
		error = truetype_set_pixel_size(face, glass_pixels[size]);
		if (error != 0)
			return EINVAL;

		/* Each glyph of this size; the clock's size has the digits and the colon only. */
		for (index = 0; index < GLASS_GLYPHS; index++) {
			if (size == SIZE_CLOCK && index > GLASS_CLOCK_LAST)
				break;
			codepoint = 32U + index;
			if (index == GLASS_CLOSE_GLYPH)
				codepoint = 0xd7U;
			if (index == GLASS_CHECK_GLYPH)
				codepoint = 0x2713U;
			if (index == GLASS_ARROW_GLYPH)
				codepoint = 0x203aU;
			id = truetype_glyph_index(face, codepoint);

			/* A menu sign the font lacks stays empty rather than showing the missing-glyph box. */
			if (id == 0U && index > GLASS_CLOSE_GLYPH)
				continue;
			error = truetype_glyph_metrics(face, id, &metrics);
			if (error != 0 || metrics.width > 64U || metrics.height > 64U)
				continue;

			/* A full row moves the pen down. */
			if (pen_x + metrics.width + 1U > GLASS_ATLAS_WIDTH) {
				pen_x = 0;
				pen_y += line + 1U;
				line = 0;
			}

			/* The atlas must hold it. */
			if (pen_y + metrics.height > GLASS_ATLAS_HEIGHT)
				return ENOSPC;

			/* The glyph's place and metrics. */
			glyph = &glass->glyphs[size][index];
			glyph->x = pen_x;
			glyph->y = pen_y;
			glyph->width = metrics.width;
			glyph->height = metrics.height;
			glyph->left = metrics.left;
			glyph->top = metrics.top;
			glyph->advance = metrics.advance;

			/* Its coverage as premultiplied white. */
			if (metrics.width != 0U && metrics.height != 0U) {
				memset(bitmap, 0, sizeof(bitmap));
				error = truetype_render_glyph(face, id, &metrics, bitmap, metrics.width, sizeof(bitmap));
				if (error != 0)
					continue;
				for (y = 0; y < metrics.height; y++) {
					row = (uint32_t *)((unsigned char *)glass->atlas.map + (pen_y + y) * glass->atlas.row_pitch);
					for (x = 0; x < metrics.width; x++) {
						value = bitmap[y * metrics.width + x];
						row[pen_x + x] = (value << 24) | (value << 16) | (value << 8) | value;
					}
				}
			}

			/* The pen moves past it. */
			pen_x += metrics.width + 1U;
			if (metrics.height > line)
				line = metrics.height;
		}
	}

	/* The icons in the rows after the glyphs. */
	printf("KWL STARTUP step=glyphs-text ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));
	started = kwl_milliseconds();
	pen_y += line + 1U;
	error = atlas_icons(glass, &pen_y);
	if (error != 0)
		return error;
	printf("KWL STARTUP step=glyphs-icons ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));
	started = kwl_milliseconds();

	/* The Kei mark's layers in the row after them. */
	error = atlas_mark(glass, &pen_y);
	if (error != 0)
		return error;
	printf("KWL STARTUP step=glyphs-mark ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

	/* The cache's cells in the rest of the atlas. */
	glass->cache_top = pen_y;
	glass->cache_count = (GLASS_ATLAS_WIDTH / GLASS_CELL) * ((GLASS_ATLAS_HEIGHT - pen_y) / GLASS_CELL);
	if (glass->cache_count > GLASS_CELLS)
		glass->cache_count = GLASS_CELLS;
	printf("KWL GLASS atlas cache-top=%u cells=%u faces=%u\n", glass->cache_top, glass->cache_count, glass->face_count);

	/* Succeeded. */
	return 0;
}

/* Reads a whole file of up to GLASS_FILE_MAX bytes; NULL with errno set when it cannot. */
static void *
file_read(
	const char *path,
	size_t *size)
{
	struct stat status;
	unsigned char *data;
	ssize_t count;
	size_t capacity;
	size_t length;
	int descriptor;
	int regular;
	int error;

	/* The file; opening a FIFO or a device does not wait for a writer. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (descriptor < 0)
		return NULL;

	/* Its size, bounded by GLASS_FILE_MAX (ws035-p133: the buffer is as large as the file). */
	error = fstat(descriptor, &status);
	if (error != 0) {
		close(descriptor);
		return NULL;
	}

	/* Only an ordinary file is a picture: a FIFO or a device would hold the reader (WS135). */
	regular = S_ISREG(status.st_mode);
	if (!regular) {
		close(descriptor);
		errno = EINVAL;
		return NULL;
	}

	/* A file larger than the bound is read to the bound. */
	capacity = GLASS_FILE_MAX;
	if (status.st_size >= 0 && (uint64_t)status.st_size < GLASS_FILE_MAX)
		capacity = (size_t)status.st_size;
	if (capacity == 0U) {
		close(descriptor);
		errno = EINVAL;
		return NULL;
	}

	/* Its bytes. */
	data = malloc(capacity);
	if (data == NULL) {
		close(descriptor);
		errno = ENOMEM;
		return NULL;
	}

	/* Read to the end or the bound: normally one read. */
	length = 0;
	for (;;) {
		count = read(descriptor, data + length, capacity - length);
		if (count <= 0)
			break;
		length += (size_t)count;
		if (length == capacity)
			break;
	}

	/* The file is no longer needed. */
	close(descriptor);

	/* An empty or unreadable file has nothing to use. */
	if (length == 0) {
		free(data);
		errno = EINVAL;
		return NULL;
	}

	/* Succeeded. */
	*size = length;
	return data;
}

/*
 * Takes the wallpaper's picture for the start: the one the prefetch decoded
 * when it read this path, otherwise read and decoded now.  Returns 0, or an
 * errno value with nothing kept.
 */
static int
wallpaper_load(
	const char *path,
	struct wallpaper_picture *picture)
{
	int taken;
	int error;

	/* The picture decoded ahead, when the prefetch decoded this path. */
	taken = prefetch_take(path, picture);
	if (taken)
		return 0;

	/* Otherwise the file now (and the reason when it cannot be shown). */
	error = wallpaper_read(path, picture);
	if (error != 0)
		return error;

	/* Succeeded: the picture is kept for its rows. */
	return 0;
}

/*
 * Reads a picture's file and decodes it (a PNG or a JPEG, ws138-p001;
 * transparency over black) into three bytes a pixel.  Returns 0, or an errno
 * value with nothing kept: the file's, EINVAL for bytes that are not such a
 * picture, EFBIG for one too large.  Safe on any thread.
 */
static int
wallpaper_read(
	const char *path,
	struct wallpaper_picture *picture)
{
	struct kl_wallpaper_image image;
	unsigned char *data;
	size_t size;
	int error;

	/* The file's bytes. */
	data = file_read(path, &size);
	if (data == NULL)
		return errno;

	/* The pixels; the file's bytes are no longer needed either way. */
	error = kl_wallpaper_decode(data, size, &image);
	free(data);
	if (error != 0)
		return error;

	/* Succeeded: the picture is kept for its rows. */
	picture->data = image.rgb;
	picture->width = image.width;
	picture->height = image.height;
	return 0;
}

/* Reads and decodes the wallpaper glass_loader names, away from the event loop. */
static void *
loader_run(
	void *argument)
{
	struct wallpaper_picture picture;
	int error;

	/* The thread needs nothing passed: glass_loader names the file. */
	(void)argument;

	/* Reads the file and decodes the picture. */
	memset(&picture, 0, sizeof(picture));
	error = wallpaper_read(glass_loader.path, &picture);

	/* Publishes the result; done tells poll that the thread can be joined. */
	(void)pthread_mutex_lock(&glass_loader.lock);

	glass_loader.picture = picture;
	glass_loader.error = error;
	glass_loader.done = 1;

	(void)pthread_mutex_unlock(&glass_loader.lock);

	/* Succeeded: the thread ends. */
	return NULL;
}

/* Waits for a wallpaper still being read (at the look's end). */
static void
loader_join(
	void)
{
	/* Nothing is being read. */
	if (!glass_loader.started)
		return;

	/*
	 * Waits for the thread, which ends on its own since an ordinary file is
	 * read to its end; clearing started records that nothing runs now.
	 */
	(void)pthread_join(glass_loader.thread, NULL);
	glass_loader.started = 0;
}

/* Reads and decodes the wallpaper's file on the prefetch's thread; the result is taken after the join. */
static void *
prefetch_run(
	void *argument)
{
	(void)argument;

	/* The picture, or none (the look then reads the file again and reports why). */
	memset(&glass_prefetch.picture, 0, sizeof(glass_prefetch.picture));
	(void)wallpaper_read(glass_prefetch.path, &glass_prefetch.picture);

	/* Succeeded: the thread ends; its result waits for the join. */
	return NULL;
}

/*
 * Takes the picture the prefetch decoded, when it read this path: waits for
 * its thread first.  Returns 1 with the picture, or 0 (and frees what it
 * decoded for another path) when the caller must read the file itself.
 */
static int
prefetch_take(
	const char *path,
	struct wallpaper_picture *picture)
{
	struct wallpaper_picture decoded;
	int same;

	/* No thread was started, or it was taken already. */
	if (!glass_prefetch.started)
		return 0;

	/*
	 * The thread's end makes its result visible here; clearing started
	 * tells prefetch_drop that nothing is left to wait for.
	 */
	(void)pthread_join(glass_prefetch.thread, NULL);
	glass_prefetch.started = 0;
	decoded = glass_prefetch.picture;
	memset(&glass_prefetch.picture, 0, sizeof(glass_prefetch.picture));

	/* A picture of another path (the preferences chose one since) is not this one. */
	same = strcmp(path, glass_prefetch.path);
	if (same != 0) {
		free(decoded.data);
		return 0;
	}

	/* A picture that could not be read is read again by the caller, which reports why. */
	if (decoded.data == NULL)
		return 0;

	/* Succeeded: the caller owns the picture. */
	*picture = decoded;
	return 1;
}

/* Waits for the prefetch's thread when it was never taken, and lets its picture go. */
static void
prefetch_drop(
	void)
{
	/* No thread was started, or it was taken already. */
	if (!glass_prefetch.started)
		return;

	/* The thread's end makes its picture visible here; nobody takes it. */
	(void)pthread_join(glass_prefetch.thread, NULL);
	glass_prefetch.started = 0;
	free(glass_prefetch.picture.data);
	memset(&glass_prefetch.picture, 0, sizeof(glass_prefetch.picture));
}

/* Starts a shape over a box: the quad is the box, with no image and no color. */
void
glass_shape_init(
	struct glass_shape *shape,
	float x,
	float y,
	float width,
	float height)
{
	/* The quad and the box are the same until the caller widens the quad. */
	memset(shape, 0, sizeof(*shape));
	shape->quad[0] = x;
	shape->quad[1] = y;
	shape->quad[2] = width;
	shape->quad[3] = height;
	shape->box[0] = x;
	shape->box[1] = y;
	shape->box[2] = width;
	shape->box[3] = height;

	/* The whole image, not faded. */
	shape->uv[2] = 1.0f;
	shape->uv[3] = 1.0f;
	shape->opacity = 1.0f;
}

/* Records one shape: its constants for both shader stages and the strip. */
void
glass_shape_draw(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct glass_shape *shape)
{
	struct kwl_compose *compose;
	float constants[KWL_PANEL_CONSTANTS];
	float quad[4];
	float box[4];
	float radius;
	float opacity;
	float width;
	float height;
	VkDescriptorSet set;

	/* A shape of a layer faded out wholly is not drawn (the desktop behind App Home, ws181-p008). */
	if (server->layer_on && server->layer_opacity <= 0.0f)
		return;

	/*
	 * The quad and the box, moved and scaled with the layer (the desktop
	 * going back into the distance behind App Home, Home's content coming
	 * forward, the desktops sliding): x' = layer x + x * scale.
	 */
	compose = server->compose;
	width = (float)server->width;
	height = (float)server->height;
	memcpy(quad, shape->quad, sizeof(quad));
	memcpy(box, shape->box, sizeof(box));
	radius = shape->radius;
	opacity = shape->opacity;
	if (server->layer_on) {
		quad[0] = server->layer_x + quad[0] * server->layer_scale;
		quad[1] = server->layer_y + quad[1] * server->layer_scale;
		quad[2] = quad[2] * server->layer_scale;
		quad[3] = quad[3] * server->layer_scale;
		box[0] = server->layer_x + box[0] * server->layer_scale;
		box[1] = server->layer_y + box[1] * server->layer_scale;
		box[2] = box[2] * server->layer_scale;
		box[3] = box[3] * server->layer_scale;
		radius = radius * server->layer_scale;
		opacity = opacity * server->layer_opacity;
	}

	/* On a head, its part of the plane: the quad and the box in its own pixels (ws113-p007). */
	if (server->view_width != 0U) {
		quad[0] -= (float)server->view_x;
		quad[1] -= (float)server->view_y;
		box[0] -= (float)server->view_x;
		box[1] -= (float)server->view_y;
		width = (float)server->view_width;
		height = (float)server->view_height;
	}

	/* The quad in normalized device coordinates. */
	constants[0] = 2.0f * quad[0] / width - 1.0f;
	constants[1] = 2.0f * quad[1] / height - 1.0f;
	constants[2] = 2.0f * (quad[0] + quad[2]) / width - 1.0f;
	constants[3] = 2.0f * (quad[1] + quad[3]) / height - 1.0f;

	/* The part of the image, the box, the color. */
	memcpy(&constants[4], shape->uv, sizeof(shape->uv));
	memcpy(&constants[8], box, sizeof(box));
	memcpy(&constants[12], shape->color, sizeof(shape->color));

	/* The shape and the output. */
	constants[16] = radius;
	constants[17] = shape->mode;
	constants[18] = shape->soft;
	constants[19] = shape->opaque;
	constants[20] = width;
	constants[21] = height;
	constants[22] = shape->edge;
	constants[23] = opacity;

	/*
	 * In the dark appearance (ws089-p017) the glass, the solid colours, the
	 * outlines and the text take their dark colours, and the glass is the
	 * shader's dark glass (mode -1); shadows, images and the blur's steps
	 * are left as they are, and so is a shape drawn light.
	 */
	if (server->dark != 0 &&
	    shape->light == 0U &&
	    server->keep_colours == 0U) {
		/* The colour of a shape that has one. */
		if (shape->mode == MODE_GLASS || shape->mode == MODE_SOLID || shape->mode == MODE_RING || shape->mode == MODE_TEXT)
			glass_dark_color(shape->color, &constants[12]);

		/* The glass is dark glass. */
		if (shape->mode == MODE_GLASS)
			constants[17] = -1.0f;
	}

	/* Glass asked to be dark is the dark glass in either appearance, its colour as given (the system bar). */
	if (shape->dark_glass != 0U && shape->mode == MODE_GLASS)
		constants[17] = -1.0f;

	/*
	 * A shape without an image of its own is given the blurred scene under
	 * the window being drawn when there is one (backdrop.c), else the
	 * blurred wallpaper (only the glass samples it).
	 */
	set = shape->set;
	if (set == VK_NULL_HANDLE)
		set = compose->backdrop_set;
	if (set == VK_NULL_HANDLE)
		set = compose->glass->blurred.set;

	/* The pipeline, the image and the strip. */
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, compose->panel_pipeline);
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, compose->panel_layout, 0U, 1U, &set, 0U, NULL);
	vkCmdPushConstants(command, compose->panel_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0U, sizeof(constants), constants);
	vkCmdDraw(command, KWL_QUAD_VERTICES, 1U, 0U, 0U);
}

/* Draws a rounded rectangle in a solid color. */
void
glass_draw_solid(
	struct kwl_server *server,
	VkCommandBuffer command,
	float x,
	float y,
	float width,
	float height,
	float radius,
	const float *color)
{
	struct glass_shape shape;

	/* One solid shape. */
	glass_shape_init(&shape, x, y, width, height);
	shape.mode = MODE_SOLID;
	shape.radius = radius;
	memcpy(shape.color, color, sizeof(shape.color));
	glass_shape_draw(server, command, &shape);
}

/* The width of a line of UTF-8 text in pixels (0 without text). */
int32_t
glass_text_width(
	struct kwl_server *server,
	enum glass_size size,
	const char *text)
{
	const struct glass_glyph *glyph;
	struct kwl_glass *glass;
	uint32_t codepoint;
	int32_t width;

	/* Nothing without glyphs. */
	glass = server->compose->glass;
	width = 0;
	if (!glass->text)
		return 0;

	/* The sum of the advances of the characters there are glyphs for. */
	while (*text != '\0') {
		codepoint = glass_utf8_next(&text);
		glyph = glass_glyph_of(glass, size, codepoint);
		if (glyph != NULL)
			width += glyph->advance;
	}

	/* Succeeded. */
	return width;
}

/*
 * Draws a line of UTF-8 text from x on a baseline, cut short (with an
 * ellipsis of dots) where it would pass x + limit.
 */
void
glass_draw_text(
	struct kwl_server *server,
	VkCommandBuffer command,
	enum glass_size size,
	int32_t x,
	int32_t baseline,
	const char *text,
	int32_t limit,
	const float *color)
{
	const struct glass_glyph *glyph;
	struct kwl_glass *glass;
	uint32_t codepoint;
	int32_t start;
	int32_t width;
	int32_t dots;
	unsigned index;

	/* Nothing without glyphs. */
	glass = server->compose->glass;
	if (!glass->text)
		return;

	/* Room is kept for the ellipsis when the text is too long. */
	dots = 3 * glass->glyphs[size]['.' - 32].advance;
	width = glass_text_width(server, size, text);
	if (width <= limit)
		dots = 0;

	/* Each character there is a glyph for, while there is room. */
	start = x;
	while (*text != '\0') {
		codepoint = glass_utf8_next(&text);
		glyph = glass_glyph_of(glass, size, codepoint);
		if (glyph == NULL)
			continue;

		/* The text stops where the ellipsis must go. */
		if (dots != 0 && x + glyph->advance > start + limit - dots)
			break;

		/* The glyph, and the pen after it. */
		glass_draw_glyph_at(server, command, glyph, x, baseline, color);
		x += glyph->advance;
	}

	/* The ellipsis. */
	if (dots != 0) {
		for (index = 0; index < 3U; index++) {
			glass_draw_glyph(server, command, size, '.' - 32, x, baseline, color);
			x += glass->glyphs[size]['.' - 32].advance;
		}
	}
}

/*
 * Draws a line of UTF-8 text from x on a baseline, cut in the middle (with
 * an ellipsis of dots between its start and its end) where it would pass
 * x + limit.  Names that differ only at their end (Document 1, Document 2)
 * stay told apart.
 */
void
glass_draw_text_middle(
	struct kwl_server *server,
	VkCommandBuffer command,
	enum glass_size size,
	int32_t x,
	int32_t baseline,
	const char *text,
	int32_t limit,
	const float *color)
{
	const struct glass_glyph *glyph;
	struct kwl_glass *glass;
	const char *tail;
	const char *next;
	uint32_t codepoint;
	int32_t width;
	int32_t dots;
	int32_t room;
	int32_t before;
	int32_t tail_width;
	int32_t head_end;
	unsigned index;

	/* Nothing without glyphs. */
	glass = server->compose->glass;
	if (!glass->text)
		return;

	/* A text that fits is drawn whole. */
	width = glass_text_width(server, size, text);
	if (width <= limit) {
		glass_draw_text(server, command, size, x, baseline, text, limit, color);
		return;
	}

	/* The room besides the ellipsis; without any, the ordinary cut. */
	dots = 3 * glass->glyphs[size]['.' - 32].advance;
	room = limit - dots;
	if (room <= 0) {
		glass_draw_text(server, command, size, x, baseline, text, limit, color);
		return;
	}

	/* The end kept: the last characters within half the room (before is the width ahead of it). */
	before = 0;
	next = text;
	while (*next != '\0') {
		/* The rest from here fits in half the room. */
		if (width - before <= room / 2)
			break;

		/* Otherwise this character is ahead of the end. */
		codepoint = glass_utf8_next(&next);
		glyph = glass_glyph_of(glass, size, codepoint);
		if (glyph != NULL)
			before += glyph->advance;
	}

	/* The end starts there. */
	tail = next;

	/* An end that starts with a dot starts after it (the ellipsis would run into it: "REA....md"). */
	if (*tail == '.') {
		codepoint = glass_utf8_next(&next);
		glyph = glass_glyph_of(glass, size, codepoint);
		if (glyph != NULL)
			before += glyph->advance;
		tail = next;
	}

	/* The start, while it fits before the ellipsis and the end. */
	tail_width = width - before;
	head_end = x + room - tail_width;
	next = text;
	while (next < tail) {
		/* The next character there is a glyph for. */
		codepoint = glass_utf8_next(&next);
		glyph = glass_glyph_of(glass, size, codepoint);
		if (glyph == NULL)
			continue;

		/* The start stops where the ellipsis must go. */
		if (x + glyph->advance > head_end)
			break;

		/* The glyph, and the pen after it. */
		glass_draw_glyph_at(server, command, glyph, x, baseline, color);
		x += glyph->advance;
	}

	/* The ellipsis. */
	for (index = 0; index < 3U; index++) {
		glass_draw_glyph(server, command, size, '.' - 32, x, baseline, color);
		x += glass->glyphs[size]['.' - 32].advance;
	}

	/* The end, each character there is a glyph for. */
	next = tail;
	while (*next != '\0') {
		/* The next character's glyph. */
		codepoint = glass_utf8_next(&next);
		glyph = glass_glyph_of(glass, size, codepoint);
		if (glyph == NULL)
			continue;

		/* The glyph, and the pen after it. */
		glass_draw_glyph_at(server, command, glyph, x, baseline, color);
		x += glyph->advance;
	}
}

/* Draws one glyph of the atlas with its origin at x on the baseline. */
void
glass_draw_glyph(
	struct kwl_server *server,
	VkCommandBuffer command,
	enum glass_size size,
	unsigned index,
	int32_t x,
	int32_t baseline,
	const float *color)
{
	struct kwl_glass *glass;

	/* The atlas's glyph, drawn where it goes. */
	glass = server->compose->glass;
	glass_draw_glyph_at(server, command, &glass->glyphs[size][index], x, baseline, color);
}

/* The advance of one glyph of the atlas (0 without text). */
int32_t
glass_glyph_advance(
	struct kwl_server *server,
	enum glass_size size,
	unsigned index)
{
	struct kwl_glass *glass;

	/* Nothing without glyphs. */
	glass = server->compose->glass;
	if (!glass->text)
		return 0;

	/* Succeeded. */
	return glass->glyphs[size][index].advance;
}

/*
 * Makes the large digits (the digits and the colon of the lock screen's
 * clock, ws187-p001) at a size in pixels, unless they are at that size
 * already.  Returns 0, or an errno value: ENODEV without the look's text,
 * EINVAL for a size past GLASS_LARGE_PIXELS_MOST (the caller then draws an
 * atlas size instead).
 */
int
glass_large_prepare(
	struct kwl_server *server,
	unsigned pixels)
{
	struct kwl_glass *glass;
	VkResult result;
	int error;

	/* Nothing without the look's text. */
	glass = server->compose->glass;
	if (glass == NULL || !glass->text)
		return ENODEV;

	/* A size the image has no room for is refused. */
	if (pixels == 0U || pixels > GLASS_LARGE_PIXELS_MOST)
		return EINVAL;

	/* The digits are at that size already (the usual frame). */
	if (glass->large_pixels == pixels)
		return 0;

	/* The image the first time; later the frames in flight that read it end before it is drawn again. */
	if (glass->large.image == VK_NULL_HANDLE) {
		result = kwl_host_image_create(server->compose, GLASS_LARGE_WIDTH, GLASS_LARGE_HEIGHT, server->compose->sampler, &glass->large);
		if (result != VK_SUCCESS)
			return EIO;
	} else {
		(void)vkDeviceWaitIdle(server->compose->device);
	}

	/* The digits drawn at the size; none is usable until they all are. */
	glass->large_pixels = 0U;
	error = large_fill(glass, glass->faces[0], pixels);
	if (error != 0) {
		printf("KWL GLASS large pixels=%u errno=%d\n", pixels, error);
		return error;
	}

	/* Succeeded: the digits are at the size. */
	glass->large_pixels = pixels;
	printf("KWL GLASS large pixels=%u\n", pixels);
	return 0;
}

/*
 * Measures a line of the large digits in pixels.
 *
 * It is 0 while none are made, and other characters count nothing.
 */
int32_t
glass_large_text_width(
	struct kwl_server *server,
	const char *text)
{
	const struct glass_glyph *glyph;
	struct kwl_glass *glass;
	int32_t width;

	/* Nothing while the digits are not made. */
	glass = server->compose->glass;
	width = 0;
	if (glass == NULL || glass->large_pixels == 0U)
		return 0;

	/* The sum of the advances of the characters there are large glyphs for. */
	while (*text != '\0') {
		glyph = large_glyph_of(glass, (uint32_t)(unsigned char)*text);
		text++;
		if (glyph != NULL)
			width += glyph->advance;
	}

	/* Succeeded. */
	return width;
}

/*
 * Draws a line of the large digits from x on a baseline.
 *
 * Other characters are passed over.
 */
void
glass_draw_large_text(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t x,
	int32_t baseline,
	const char *text,
	const float *color)
{
	const struct glass_glyph *glyph;
	struct glass_shape shape;
	struct kwl_glass *glass;

	/* Nothing while the digits are not made. */
	glass = server->compose->glass;
	if (glass == NULL || glass->large_pixels == 0U)
		return;

	/* Each character there is a large glyph for, the pen moving past it. */
	while (*text != '\0') {
		glyph = large_glyph_of(glass, (uint32_t)(unsigned char)*text);
		text++;
		if (glyph == NULL)
			continue;

		/* Its coverage in the colour, one to one with the image. */
		if (glyph->width != 0U && glyph->height != 0U) {
			glass_shape_init(&shape, (float)(x + glyph->left), (float)(baseline - glyph->top), (float)glyph->width, (float)glyph->height);
			shape.mode = MODE_TEXT;
			shape.uv[0] = (float)glyph->x / (float)GLASS_LARGE_WIDTH;
			shape.uv[1] = (float)glyph->y / (float)GLASS_LARGE_HEIGHT;
			shape.uv[2] = (float)(glyph->x + glyph->width) / (float)GLASS_LARGE_WIDTH;
			shape.uv[3] = (float)(glyph->y + glyph->height) / (float)GLASS_LARGE_HEIGHT;
			memcpy(shape.color, color, sizeof(shape.color));
			shape.set = glass->large.set;
			glass_shape_draw(server, command, &shape);
		}

		/* The pen after it. */
		x += glyph->advance;
	}
}

/*
 * Draws a titlebar icon (GLASS_ICON_* before GLASS_ICON_FIRST_APP) in a
 * square of a size in pixels at (x, y), from the atlas's icon of that size
 * or of the nearest one scaled.  The applications' pictures are drawn on
 * their tiles by glass_draw_app_tile.
 */
void
glass_draw_icon(
	struct kwl_server *server,
	VkCommandBuffer command,
	unsigned icon,
	int32_t x,
	int32_t y,
	unsigned pixels,
	const float *color)
{
	struct glass_shape shape;
	const struct glass_glyph *glyph;
	struct kwl_glass *glass;
	unsigned size;

	/* Nothing without the atlas, or for an icon the atlas does not have. */
	glass = server->compose->glass;
	if (!glass->text || icon >= GLASS_ICON_FIRST_APP)
		return;

	/* The larger size for anything above the smaller. */
	size = 0;
	if (pixels > glass_icon_pixels[0])
		size = 1;
	glyph = &glass->icons[size][icon];

	/* The icon's cell of the atlas, over the square. */
	glass_shape_init(&shape, (float)x, (float)y, (float)pixels, (float)pixels);
	shape.mode = MODE_TEXT;
	shape.uv[0] = (float)glyph->x / (float)GLASS_ATLAS_WIDTH;
	shape.uv[1] = (float)glyph->y / (float)GLASS_ATLAS_HEIGHT;
	shape.uv[2] = (float)(glyph->x + glyph->width) / (float)GLASS_ATLAS_WIDTH;
	shape.uv[3] = (float)(glyph->y + glyph->height) / (float)GLASS_ATLAS_HEIGHT;
	memcpy(shape.color, color, sizeof(shape.color));
	shape.set = glass->atlas.set;
	glass_shape_draw(server, command, &shape);
}

/*
 * Draws an application's tile upside down under a floor at y, as a glossy
 * floor reflects it (App Home's stage, ws099-p035b): the tile's lower part,
 * height pixels of it, mirrored, its opacity from opacity at the floor
 * fading to nothing downwards (in GLASS_REFLECTION_SLICES bands).
 */
void
glass_draw_app_tile_reflection(
	struct kwl_server *server,
	VkCommandBuffer command,
	unsigned icon,
	float x,
	float y,
	float pixels,
	float height,
	float opacity)
{
	struct glass_shape shape;
	const struct glass_glyph *glyph;
	struct kwl_glass *glass;
	unsigned size;
	unsigned slice;
	float top;
	float bottom;
	float source_top;
	float source_bottom;
	float glyph_top;
	float glyph_height;

	/* Nothing without the tiles, for an icon without one, or for nothing to show. */
	glass = server->compose->glass;
	if (!glass->tiles_ready)
		return;
	if (icon < GLASS_ICON_FIRST_APP || icon >= GLASS_ICON_COUNT)
		return;
	if (height <= 0.0f || opacity <= 0.0f)
		return;

	/* The smallest size kept that is not smaller than the square, else the largest. */
	for (size = 0; size + 1U < GLASS_TILE_SIZES; size++) {
		if ((float)glass_tile_pixels[size] >= pixels)
			break;
	}

	/* That size's tile, and its rows in the tiles' image. */
	glyph = &glass->app_tiles[size][icon - GLASS_ICON_FIRST_APP];
	glyph_top = (float)glyph->y;
	glyph_height = (float)glyph->height;

	/* Each band down from the floor: the tile's rows up from its bottom, fainter further down. */
	for (slice = 0U; slice < GLASS_REFLECTION_SLICES; slice++) {
		top = y + height * (float)slice / (float)GLASS_REFLECTION_SLICES;
		bottom = y + height * (float)(slice + 1U) / (float)GLASS_REFLECTION_SLICES;
		source_top = glyph_top + glyph_height - glyph_height * (top - y) / pixels;
		source_bottom = glyph_top + glyph_height - glyph_height * (bottom - y) / pixels;
		glass_shape_init(&shape, x, top, pixels, bottom - top);
		shape.mode = MODE_IMAGE;
		shape.uv[0] = (float)glyph->x / (float)GLASS_TILE_WIDTH;
		shape.uv[1] = source_top / (float)GLASS_TILE_HEIGHT;
		shape.uv[2] = (float)(glyph->x + glyph->width) / (float)GLASS_TILE_WIDTH;
		shape.uv[3] = source_bottom / (float)GLASS_TILE_HEIGHT;
		shape.opacity = opacity * (1.0f - ((float)slice + 0.5f) / (float)GLASS_REFLECTION_SLICES);
		shape.set = glass->tiles.set;
		glass_shape_draw(server, command, &shape);
	}
}

/*
 * Draws an application's tile (an icon from GLASS_ICON_FIRST_APP) in a
 * square of a size in pixels at (x, y): the tile kept at that size, or the
 * smallest kept larger (the largest when none is) scaled, as opaque as
 * asked (0..1).  Its picture is cut out: with GLASS_HOLE_GROUND what was
 * drawn under the tile shows through it, with GLASS_HOLE_SCENE the blurred
 * scene under the glass the tile is on, unwhitened (BUG-237), with
 * GLASS_HOLE_WALLPAPER the desktop's wallpaper itself, sharp, as if the
 * picture were a real hole through the bar or App Home (ws099-p034b, the
 * 2026-10-06 user decision).  lighten
 * (0..1) whitens the tile itself, as a lit button.
 */
void
glass_draw_app_tile(
	struct kwl_server *server,
	VkCommandBuffer command,
	unsigned icon,
	float x,
	float y,
	float pixels,
	float opacity,
	float lighten,
	enum glass_hole hole)
{
	struct glass_shape shape;
	const struct glass_glyph *glyph;
	struct kwl_glass *glass;
	unsigned size;
	float inset;

	/* Nothing without the tiles, or for an icon without one. */
	glass = server->compose->glass;
	if (!glass->tiles_ready)
		return;
	if (icon < GLASS_ICON_FIRST_APP || icon >= GLASS_ICON_COUNT)
		return;

	/* The smallest size kept that is not smaller than the square, else the largest. */
	for (size = 0; size + 1U < GLASS_TILE_SIZES; size++) {
		if ((float)glass_tile_pixels[size] >= pixels)
			break;
	}

	/* That size's tile of the icon. */
	glyph = &glass->app_tiles[size][icon - GLASS_ICON_FIRST_APP];

	/*
	 * On light glass the picture is a window onto the scene the glass
	 * frosts: the blurred scene under the tile as it is (glass of no
	 * colour, flat, without an edge, light in the dark appearance too),
	 * drawn inside the tile, where only the cut-out picture leaves it seen.
	 */
	if (hole == GLASS_HOLE_SCENE) {
		inset = pixels * GLASS_TILE_SCENE_INSET;
		glass_shape_init(&shape, x + inset, y + inset, pixels - 2.0f * inset, pixels - 2.0f * inset);
		shape.mode = MODE_GLASS;
		shape.radius = pixels * GLASS_ICON_TILE_RADIUS - inset;
		shape.soft = 1.0f;
		shape.opacity = opacity;
		shape.light = 1U;
		glass_shape_draw(server, command, &shape);
	}

	/*
	 * Through to the wallpaper: the part of the wallpaper under the tile
	 * (the wallpaper is drawn over the whole output), inside the tile,
	 * where only the cut-out picture leaves it seen.
	 */
	if (hole == GLASS_HOLE_WALLPAPER) {
		inset = pixels * GLASS_TILE_SCENE_INSET;
		glass_shape_init(&shape, x + inset, y + inset, pixels - 2.0f * inset, pixels - 2.0f * inset);
		shape.mode = MODE_IMAGE;
		shape.opaque = 1.0f;
		shape.radius = pixels * GLASS_ICON_TILE_RADIUS - inset;
		shape.uv[0] = (x + inset) / (float)server->width;
		shape.uv[1] = (y + inset) / (float)server->height;
		shape.uv[2] = (x + pixels - inset) / (float)server->width;
		shape.uv[3] = (y + pixels - inset) / (float)server->height;
		shape.opacity = opacity;
		shape.set = glass_wallpaper_set(server);
		glass_shape_draw(server, command, &shape);
	}

	/* The tile's pixels over the square, premultiplied, as an image. */
	glass_shape_init(&shape, x, y, pixels, pixels);
	shape.mode = MODE_IMAGE;
	shape.uv[0] = (float)glyph->x / (float)GLASS_TILE_WIDTH;
	shape.uv[1] = (float)glyph->y / (float)GLASS_TILE_HEIGHT;
	shape.uv[2] = (float)(glyph->x + glyph->width) / (float)GLASS_TILE_WIDTH;
	shape.uv[3] = (float)(glyph->y + glyph->height) / (float)GLASS_TILE_HEIGHT;
	shape.opacity = opacity;
	shape.set = glass->tiles.set;
	glass_shape_draw(server, command, &shape);

	/* Unlit, that is all. */
	if (lighten <= 0.0f)
		return;

	/*
	 * Lit: white over the tile, as much as the tile covers (its alpha read
	 * as a glyph's coverage), so the picture's hole stays clear; white in
	 * the dark appearance too.
	 */
	shape.mode = MODE_TEXT;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = lighten;
	shape.light = 1U;
	glass_shape_draw(server, command, &shape);
}

/*
 * Draws the Kei mark in a square of a size in pixels at (x, y): its seven
 * layers from the atlas, each in the colour of the look asked for, the
 * whole as opaque as asked (0..1).  A mark about the launcher's size uses
 * the layers rendered at that size (ws035-p117).
 */
void
glass_draw_mark(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t x,
	int32_t y,
	unsigned pixels,
	enum glass_mark_look look,
	float opacity)
{
	/*
	 * The bar pale, its shade deeper, the leaf clearer and its shade the
	 * deep blue of the splash, the overlap deeper still, then the white
	 * light along the edges and the sheen (ws035-p109).  The panes are
	 * translucent, so the picture behind shows through as on the splash.
	 */
	static const float splash_colours[KL_MARK_LAYERS][4] = {
		{ 0.663f, 0.765f, 0.965f, 0.69f },
		{ 0.498f, 0.635f, 0.941f, 0.35f },
		{ 0.639f, 0.847f, 0.980f, 0.67f },
		{ 0.227f, 0.525f, 0.961f, 0.67f },
		{ 0.184f, 0.486f, 0.953f, 0.78f },
		{ 1.0f, 1.0f, 1.0f, 0.67f },
		{ 1.0f, 1.0f, 1.0f, 0.24f }
	};

	/*
	 * The same layers for the system bar (ws035-p118, the 2026-09-28 user
	 * decision): every pane a slightly deeper blue and nearly opaque, and
	 * the white light and sheen fainter, so that the mark stands out on
	 * the bar's light glass instead of fading into it.
	 */
	static const float bar_colours[KL_MARK_LAYERS][4] = {
		{ 0.455f, 0.600f, 0.925f, 0.92f },
		{ 0.290f, 0.451f, 0.878f, 0.55f },
		{ 0.400f, 0.690f, 0.945f, 0.90f },
		{ 0.145f, 0.408f, 0.890f, 0.90f },
		{ 0.106f, 0.349f, 0.839f, 0.96f },
		{ 1.0f, 1.0f, 1.0f, 0.50f },
		{ 1.0f, 1.0f, 1.0f, 0.18f }
	};
	const float (*colours)[4];
	struct glass_shape shape;
	const struct glass_glyph *layers;
	const struct glass_glyph *glyph;
	struct kwl_glass *glass;
	unsigned layer;

	/* Nothing without the atlas. */
	glass = server->compose->glass;
	if (!glass->text)
		return;

	/* The colours of the look asked for. */
	colours = splash_colours;
	if (look == GLASS_MARK_BAR)
		colours = bar_colours;

	/* The layers rendered nearest the size: the small ones up to half again their size. */
	layers = glass->mark;
	if (pixels <= GLASS_MARK_SMALL_PIXELS * 3U / 2U)
		layers = glass->mark_small;

	/* Each layer's cell of the atlas over the square, in order. */
	for (layer = 0; layer < KL_MARK_LAYERS; layer++) {
		glyph = &layers[layer];
		glass_shape_init(&shape, (float)x, (float)y, (float)pixels, (float)pixels);
		shape.mode = MODE_TEXT;
		shape.uv[0] = (float)glyph->x / (float)GLASS_ATLAS_WIDTH;
		shape.uv[1] = (float)glyph->y / (float)GLASS_ATLAS_HEIGHT;
		shape.uv[2] = (float)(glyph->x + glyph->width) / (float)GLASS_ATLAS_WIDTH;
		shape.uv[3] = (float)(glyph->y + glyph->height) / (float)GLASS_ATLAS_HEIGHT;
		memcpy(shape.color, colours[layer], sizeof(shape.color));
		shape.color[3] *= opacity;
		shape.set = glass->atlas.set;
		glass_shape_draw(server, command, &shape);
	}
}

/*
 * Gives the wallpaper at the desktop's size, for a display beside the
 * output (heads.c, ws113-p004b); NULL without the glass look or before the
 * wallpaper is made.
 */
const struct kwl_import *
kwl_glass_wallpaper(
	struct kwl_server *server)
{
	struct kwl_glass *glass;

	/* The look's own wallpaper. */
	if (server->compose == NULL || server->compose->glass == NULL)
		return NULL;
	glass = server->compose->glass;
	if (glass->wallpaper.image == VK_NULL_HANDLE)
		return NULL;

	/* Succeeded: the image. */
	return &glass->wallpaper;
}

/* The descriptor set of the wallpaper, for pictures of it (the desktops). */
VkDescriptorSet
glass_wallpaper_set(
	struct kwl_server *server)
{
	/* The full-size image. */
	return server->compose->glass->wallpaper.set;
}

/*
 * Renders every titlebar icon at each of its sizes into the atlas from a
 * row on, and moves the row past them; returns 0, or ENOSPC when the atlas
 * is full.
 */
static int
atlas_icons(
	struct kwl_glass *glass,
	uint32_t *pen_y)
{
	static uint8_t bitmap[GLASS_BITMAP * GLASS_BITMAP];
	struct glass_glyph *glyph;
	uint32_t pen_x;
	unsigned pixels;
	unsigned tallest;
	unsigned size;
	unsigned icon;

	/* The titlebar's icons side by side, as tall as the largest size. */
	pen_x = 0;
	tallest = glass_icon_pixels[GLASS_ICON_SIZES - 1U];
	for (size = 0; size < GLASS_ICON_SIZES; size++) {
		pixels = glass_icon_pixels[size];
		for (icon = 0; icon < GLASS_ICON_FIRST_APP; icon++) {
			/* A full row moves the pen down. */
			if (pen_x + pixels + 1U > GLASS_ATLAS_WIDTH) {
				pen_x = 0;
				*pen_y += tallest + 1U;
			}

			/* The atlas must hold it. */
			if (*pen_y + pixels > GLASS_ATLAS_HEIGHT)
				return ENOSPC;

			/* The icon's coverage, into the atlas. */
			kwl_icon_raster(icon, pixels, bitmap, pixels);
			atlas_put(glass, bitmap, pen_x, *pen_y, pixels, pixels);

			/* Its place, square, drawn from its top left. */
			glyph = &glass->icons[size][icon];
			glyph->x = pen_x;
			glyph->y = *pen_y;
			glyph->width = pixels;
			glyph->height = pixels;
			glyph->left = 0;
			glyph->top = 0;
			glyph->advance = (int32_t)pixels;

			/* The pen moves past it. */
			pen_x += pixels + 1U;
		}
	}

	/* The row after the icons. */
	*pen_y += tallest + 1U;

	/* Succeeded: the icons are in the atlas. */
	return 0;
}

/*
 * Makes the image of the applications' tiles and draws every tile at each
 * size into it, a row or more a size; returns 0 with the tiles ready, EIO
 * when the image cannot be made, or ENOSPC when they do not fit.
 */
static int
tiles_create(
	struct kwl_server *server,
	struct kwl_glass *glass)
{
	struct glass_glyph *glyph;
	VkResult result;
	uint32_t *place;
	uint32_t pen_x;
	uint32_t pen_y;
	unsigned pixels;
	unsigned size;
	unsigned icon;

	/* The image, transparent where no tile is, sampled smoothly for the sizes between those kept. */
	result = kwl_host_image_create(server->compose, GLASS_TILE_WIDTH, GLASS_TILE_HEIGHT, server->compose->linear_sampler, &glass->tiles);
	if (result != VK_SUCCESS)
		return EIO;
	memset(glass->tiles.map, 0, glass->tiles.row_pitch * GLASS_TILE_HEIGHT);

	/* Each size's tiles side by side from a row of their own, gaps between them so that smooth sampling keeps to one tile. */
	pen_y = 0;
	for (size = 0; size < GLASS_TILE_SIZES; size++) {
		pixels = glass_tile_pixels[size];
		pen_x = 0;
		for (icon = 0; icon < GLASS_ICON_APPS; icon++) {
			/* A full row moves the pen down. */
			if (pen_x + pixels + GLASS_TILE_GAP > GLASS_TILE_WIDTH) {
				pen_x = 0;
				pen_y += pixels + GLASS_TILE_GAP;
			}

			/* The image must hold it. */
			if (pen_y + pixels + GLASS_TILE_GAP > GLASS_TILE_HEIGHT)
				return ENOSPC;

			/* The tile, drawn straight into the image. */
			place = (uint32_t *)((unsigned char *)glass->tiles.map + (size_t)pen_y * glass->tiles.row_pitch) + pen_x;
			kwl_icon_tile(GLASS_ICON_FIRST_APP + icon, pixels, place, glass->tiles.row_pitch / sizeof(uint32_t));

			/* Its place, square. */
			glyph = &glass->app_tiles[size][icon];
			glyph->x = pen_x;
			glyph->y = pen_y;
			glyph->width = pixels;
			glyph->height = pixels;
			glyph->advance = (int32_t)pixels;

			/* The pen moves past it. */
			pen_x += pixels + GLASS_TILE_GAP;
		}

		/* The next size from the row after. */
		pen_y += pixels + GLASS_TILE_GAP;
	}

	/* Succeeded: the tiles are drawn. */
	glass->tiles_ready = 1U;
	return 0;
}

/*
 * Draws the large digits and the colon at a size into their image, side by
 * side along its top, a column apart so that sampling keeps to one glyph;
 * returns 0, ENOSPC when a glyph does not fit, or the font's errno value.
 */
static int
large_fill(
	struct kwl_glass *glass,
	struct truetype_face *face,
	unsigned pixels)
{
	/* One glyph's coverage while it is rendered; static to keep its 64 KiB off the stack. */
	static uint8_t bitmap[GLASS_LARGE_SIDE * GLASS_LARGE_SIDE];
	struct truetype_glyph metrics;
	struct glass_glyph *glyph;
	uint32_t codepoint;
	uint32_t pen_x;
	unsigned index;
	unsigned id;
	int error;

	/* The image transparent, no glyph known, and the face at the size (the cache sets its own size again). */
	memset(glass->large.map, 0, glass->large.row_pitch * GLASS_LARGE_HEIGHT);
	memset(glass->large_glyphs, 0, sizeof(glass->large_glyphs));
	error = truetype_set_pixel_size(face, pixels);
	if (error != 0)
		return EINVAL;

	/* Each glyph from the left. */
	pen_x = 0;
	for (index = 0; index < GLASS_LARGE_GLYPHS; index++) {
		/* Its character: a digit, or the colon last. */
		codepoint = (uint32_t)'0' + index;
		if (index == GLASS_LARGE_COLON)
			codepoint = ':';
		id = truetype_glyph_index(face, codepoint);

		/* Its box. */
		error = truetype_glyph_metrics(face, id, &metrics);
		if (error != 0)
			return error;

		/* The bitmap must hold it. */
		if (metrics.width > GLASS_LARGE_SIDE || metrics.height > GLASS_LARGE_SIDE)
			return ENOSPC;

		/* The image must hold it. */
		if (metrics.height > GLASS_LARGE_HEIGHT || pen_x + metrics.width > GLASS_LARGE_WIDTH)
			return ENOSPC;

		/* Its place and metrics. */
		glyph = &glass->large_glyphs[index];
		glyph->x = pen_x;
		glyph->y = 0;
		glyph->width = metrics.width;
		glyph->height = metrics.height;
		glyph->left = metrics.left;
		glyph->top = metrics.top;
		glyph->advance = metrics.advance;

		/* Renders a glyph that has a box. */
		if (metrics.width != 0U && metrics.height != 0U) {
			error = truetype_render_glyph(face, id, &metrics, bitmap, metrics.width, sizeof(bitmap));
			if (error != 0)
				return error;

			/* Stores its coverage as premultiplied white. */
			large_put(glass, bitmap, pen_x, metrics.width, metrics.height);
		}

		/* The pen moves past it and a clear column. */
		pen_x += metrics.width + 1U;
	}

	/* Succeeded: every glyph is drawn. */
	return 0;
}

/* Stores a glyph's coverage as premultiplied white at a column of the large image. */
static void
large_put(
	struct kwl_glass *glass,
	const uint8_t *bitmap,
	uint32_t pen_x,
	uint32_t width,
	uint32_t height)
{
	uint32_t *row;
	uint32_t x;
	uint32_t y;
	uint32_t value;

	/* Copies each row of the bitmap. */
	for (y = 0; y < height; y++) {
		/* Finds the image's row. */
		row = (uint32_t *)((unsigned char *)glass->large.map + (size_t)y * glass->large.row_pitch);

		/* Spreads each coverage value over the four channels. */
		for (x = 0; x < width; x++) {
			value = bitmap[y * width + x];
			row[pen_x + x] = (value << 24) | (value << 16) | (value << 8) | value;
		}
	}
}

/* Gives the large glyph of a character: a digit or the colon (NULL for any other). */
static const struct glass_glyph *
large_glyph_of(
	struct kwl_glass *glass,
	uint32_t character)
{
	/* The colon, after the digits. */
	if (character == ':')
		return &glass->large_glyphs[GLASS_LARGE_COLON];

	/* Anything but a digit has none. */
	if (character < '0' || character > '9')
		return NULL;

	/* Succeeded: the digit's glyph. */
	return &glass->large_glyphs[character - '0'];
}

/*
 * Renders the Kei mark's layers into the atlas side by side from a row on,
 * the small ones at the launcher's size in the column right of them
 * (ws035-p117), and moves the row past them; returns 0, or ENOSPC when the
 * atlas is full.
 */
static int
atlas_mark(
	struct kwl_glass *glass,
	uint32_t *pen_y)
{
	static uint8_t bitmap[GLASS_MARK_PIXELS * GLASS_MARK_PIXELS];
	struct glass_glyph *glyph;
	unsigned layer;
	uint32_t pen_x;
	uint32_t column_x;

	/* The atlas must hold the row. */
	if (*pen_y + GLASS_MARK_PIXELS > GLASS_ATLAS_HEIGHT)
		return ENOSPC;

	/* Each layer's coverage, into the atlas, and its place. */
	pen_x = 0;
	for (layer = 0; layer < KL_MARK_LAYERS; layer++) {
		kl_mark_raster(layer, GLASS_MARK_PIXELS, bitmap, GLASS_MARK_PIXELS);
		atlas_put(glass, bitmap, pen_x, *pen_y, GLASS_MARK_PIXELS, GLASS_MARK_PIXELS);
		glyph = &glass->mark[layer];
		glyph->x = pen_x;
		glyph->y = *pen_y;
		glyph->width = GLASS_MARK_PIXELS;
		glyph->height = GLASS_MARK_PIXELS;
		glyph->left = 0;
		glyph->top = 0;
		glyph->advance = (int32_t)GLASS_MARK_PIXELS;

		/* The pen moves past it. */
		pen_x += GLASS_MARK_PIXELS + 1U;
	}

	/* The small layers must fit the column left of the row's end. */
	column_x = pen_x;
	if (column_x + GLASS_MARK_SMALL_ACROSS * (GLASS_MARK_SMALL_PIXELS + 1U) > GLASS_ATLAS_WIDTH)
		return ENOSPC;

	/* Each small layer's coverage, into its cell of the column, and its place. */
	for (layer = 0; layer < KL_MARK_LAYERS; layer++) {
		kl_mark_raster(layer, GLASS_MARK_SMALL_PIXELS, bitmap, GLASS_MARK_SMALL_PIXELS);
		glyph = &glass->mark_small[layer];
		glyph->x = column_x + (layer % GLASS_MARK_SMALL_ACROSS) * (GLASS_MARK_SMALL_PIXELS + 1U);
		glyph->y = *pen_y + (layer / GLASS_MARK_SMALL_ACROSS) * (GLASS_MARK_SMALL_PIXELS + 1U);
		glyph->width = GLASS_MARK_SMALL_PIXELS;
		glyph->height = GLASS_MARK_SMALL_PIXELS;
		glyph->left = 0;
		glyph->top = 0;
		glyph->advance = (int32_t)GLASS_MARK_SMALL_PIXELS;
		atlas_put(glass, bitmap, glyph->x, glyph->y, GLASS_MARK_SMALL_PIXELS, GLASS_MARK_SMALL_PIXELS);
	}

	/* The row after the mark. */
	*pen_y += GLASS_MARK_PIXELS + 1U;

	/* Succeeded: the mark is in the atlas. */
	return 0;
}

/* Writes a bitmap of coverage into the atlas at a place, as premultiplied white. */
static void
atlas_put(
	struct kwl_glass *glass,
	const uint8_t *bitmap,
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height)
{
	uint32_t *row;
	uint32_t value;
	uint32_t column;
	uint32_t line;

	/* Each row of the bitmap, onto its row of the atlas. */
	for (line = 0; line < height; line++) {
		row = (uint32_t *)((unsigned char *)glass->atlas.map + (size_t)(y + line) * glass->atlas.row_pitch);
		for (column = 0; column < width; column++) {
			value = bitmap[line * width + column];
			row[x + column] = (value << 24) | (value << 16) | (value << 8) | value;
		}
	}
}

/*
 * Opens a font and keeps it (and its file's bytes) for the cache; returns
 * 0, ENOENT for no path, or the reason it could not be read.
 */
static int
glass_open_face(
	struct kwl_glass *glass,
	const char *path)
{
	struct truetype_face *face;
	void *data;
	size_t size;
	int error;

	/* No path, or no room for another font. */
	if (path == NULL || glass->face_count == GLASS_FACES)
		return ENOENT;

	/* The file. */
	data = file_read(path, &size);
	if (data == NULL)
		return errno;

	/* The font in it. */
	error = truetype_open(data, size, 0U, &face);
	if (error != 0) {
		free(data);
		return EINVAL;
	}

	/* Kept until the look closes. */
	glass->faces[glass->face_count] = face;
	glass->font_data[glass->face_count] = data;
	glass->face_count++;

	/* Succeeded: the font can render glyphs. */
	return 0;
}

/* Finds the glyph of a character at a size: the atlas's for ASCII, the cache's for any other; NULL for none. */
static const struct glass_glyph *
glass_glyph_of(
	struct kwl_glass *glass,
	enum glass_size size,
	uint32_t codepoint)
{
	const struct glass_glyph *glyph;

	/* Control characters have none. */
	if (codepoint < 32U || codepoint == 127U)
		return NULL;

	/* Printable ASCII is in the atlas. */
	if (codepoint < 127U)
		return &glass->glyphs[size][codepoint - 32U];

	/* Any other character is in the cache, rendered on its first use. */
	glyph = glass_cache_glyph(glass, size, codepoint);
	return glyph;
}

/*
 * Finds a character's glyph in the cache, rendering it into the least
 * recently drawn cell when it is not there; NULL when it cannot be.
 */
static const struct glass_glyph *
glass_cache_glyph(
	struct kwl_glass *glass,
	enum glass_size size,
	uint32_t codepoint)
{
	static uint8_t bitmap[GLASS_BITMAP * GLASS_BITMAP];
	const struct glass_glyph *glyph;
	struct truetype_glyph metrics;
	struct truetype_face *face;
	struct glass_cached *cell;
	unsigned per_row;
	unsigned oldest;
	unsigned index;
	unsigned which;
	unsigned id;
	int error;

	/* No cache without room or fonts. */
	if (glass->cache_count == 0U || glass->face_count == 0U)
		return NULL;

	/* The cell that holds it already, else the least recently drawn (a free one first). */
	oldest = 0;
	for (index = 0; index < glass->cache_count; index++) {
		cell = &glass->cache[index];
		if (cell->codepoint == codepoint && cell->size == (unsigned)size) {
			glass->clock++;
			cell->used = glass->clock;
			return &cell->glyph;
		}

		/* Otherwise the oldest so far. */
		if (cell->used < glass->cache[oldest].used)
			oldest = index;
	}

	/*
	 * The first font that has the character, else the first font's
	 * missing-glyph box.  The colour emoji font, once open, is among the
	 * faces but is asked below instead: its glyphs are colour images with
	 * no outlines (T1-175: every emoji after the first came out empty).
	 */
	face = glass->faces[0];
	id = 0;
	for (which = 0; which < glass->face_count; which++) {
		if (glass->emoji_tried != 0U && (int)which == glass->emoji_face)
			continue;
		id = truetype_glyph_index(glass->faces[which], codepoint);
		if (id != 0U) {
			face = glass->faces[which];
			break;
		}
	}

	/* No font open has it: the fallback font, opened once on the first such character. */
	if (id == 0U && glass->fallback_tried == 0U) {
		glass->fallback_tried = 1;
		error = glass_open_face(glass, glass->fallback_path);
		if (error != 0) {
			printf("KWL GLASS no fallback font: path=%s errno=%d\n", glass->fallback_path, error);
		} else {
			printf("KWL GLASS fallback font: path=%s faces=%u\n", glass->fallback_path, glass->face_count);
			which = glass->face_count - 1U;
			id = truetype_glyph_index(glass->faces[which], codepoint);
			if (id != 0U)
				face = glass->faces[which];
		}
	}

	/* No font has it yet: the colour emoji font, opened once on the first such character (ws102-p019). */
	if (id == 0U && glass->emoji_tried == 0U) {
		glass->emoji_tried = 1;
		glass->emoji_face = -1;
		error = glass_open_face(glass, GLASS_EMOJI_FONT);
		if (error == 0)
			glass->emoji_face = (int)glass->face_count - 1;
		printf("KWL GLASS emoji font: path=%s errno=%d\n", GLASS_EMOJI_FONT, error);
	}

	/* The emoji font's colour glyph, when it has the character. */
	if (id == 0U && glass->emoji_tried != 0U && glass->emoji_face >= 0) {
		id = truetype_glyph_index(glass->faces[glass->emoji_face], codepoint);
		if (id != 0U) {
			glyph = glass_cache_color(glass, size, codepoint, id, oldest);
			return glyph;
		}
	}

	/* No font has it: the first font's box. */
	if (id == 0U)
		which = 0;

	/* Its metrics at the size, which must fit a cell. */
	error = truetype_set_pixel_size(face, glass_pixels[size]);
	if (error != 0)
		return NULL;
	error = truetype_glyph_metrics(face, id, &metrics);
	if (error != 0)
		return NULL;
	if (metrics.width > GLASS_CELL || metrics.height > GLASS_CELL)
		return NULL;

	/* The cell's place in the atlas. */
	cell = &glass->cache[oldest];
	per_row = GLASS_ATLAS_WIDTH / GLASS_CELL;
	cell->glyph.x = (oldest % per_row) * GLASS_CELL;
	cell->glyph.y = glass->cache_top + (oldest / per_row) * GLASS_CELL;

	/* The glyph's coverage, into the cell. */
	memset(bitmap, 0, sizeof(bitmap));
	if (metrics.width != 0U && metrics.height != 0U) {
		error = truetype_render_glyph(face, id, &metrics, bitmap, metrics.width, sizeof(bitmap));
		if (error != 0)
			return NULL;
	}

	/* Into the cell. */
	atlas_put(glass, bitmap, cell->glyph.x, cell->glyph.y, metrics.width, metrics.height);

	/* The cell now holds this character's glyph. */
	cell->codepoint = codepoint;
	cell->size = (unsigned)size;
	cell->glyph.color = 0;
	cell->glyph.width = metrics.width;
	cell->glyph.height = metrics.height;
	cell->glyph.left = metrics.left;
	cell->glyph.top = metrics.top;
	cell->glyph.advance = metrics.advance;
	glass->clock++;
	cell->used = glass->clock;
	printf("KWL GLASS glyph codepoint=U+%04X size=%u face=%u cell=%u\n", codepoint, (unsigned)size, which, oldest);

	/* Succeeded: the glyph. */
	return &cell->glyph;
}

/*
 * Draws a character's colour glyph of the emoji font into a cell of the
 * cache (its premultiplied colours as they are), for glass_draw_glyph_at
 * to draw as an image (ws102-p019); NULL when it cannot be drawn or is
 * larger than a cell.
 */
static const struct glass_glyph *
glass_cache_color(
	struct kwl_glass *glass,
	enum glass_size size,
	uint32_t codepoint,
	unsigned id,
	unsigned slot)
{
	struct kl_color_image image;
	struct glass_cached *cell;
	uint32_t *row;
	unsigned per_row;
	int error;
	int line;

	/* The glyph's colours at the size, which must fit a cell. */
	error = kl_color_glyph(glass->faces[glass->emoji_face], id, glass_pixels[size], &image);
	if (error != 0)
		return NULL;
	if (image.width > (int)GLASS_CELL || image.height > (int)GLASS_CELL) {
		free(image.pixels);
		return NULL;
	}

	/* The cell's place in the atlas, and the colours into it row by row. */
	cell = &glass->cache[slot];
	per_row = GLASS_ATLAS_WIDTH / GLASS_CELL;
	cell->glyph.x = (slot % per_row) * GLASS_CELL;
	cell->glyph.y = glass->cache_top + (slot / per_row) * GLASS_CELL;
	for (line = 0; line < image.height; line++) {
		row = (uint32_t *)((unsigned char *)glass->atlas.map + (size_t)(cell->glyph.y + (uint32_t)line) * glass->atlas.row_pitch);
		memcpy(row + cell->glyph.x, image.pixels + (size_t)line * (size_t)image.width, (size_t)image.width * sizeof(uint32_t));
	}

	/* The colours are in the atlas now. */
	free(image.pixels);

	/* Succeeded: the cell holds the character's colour glyph. */
	cell->codepoint = codepoint;
	cell->size = (unsigned)size;
	cell->glyph.width = (uint32_t)image.width;
	cell->glyph.height = (uint32_t)image.height;
	cell->glyph.left = image.left;
	cell->glyph.top = image.top;
	cell->glyph.advance = image.advance;
	cell->glyph.color = 1;
	glass->clock++;
	cell->used = glass->clock;
	printf("KWL GLASS glyph codepoint=U+%04X size=%u face=emoji cell=%u color=1\n", codepoint, (unsigned)size, slot);
	return &cell->glyph;
}

/* Reads one UTF-8 character and moves past it; a malformed byte reads as U+FFFD and is passed alone. */
static uint32_t
glass_utf8_next(
	const char **text)
{
	const unsigned char *bytes;
	uint32_t codepoint;
	unsigned count;
	unsigned index;

	/* The first byte says how many follow. */
	bytes = (const unsigned char *)*text;
	if (bytes[0] < 0x80U) {
		*text += 1;
		return bytes[0];
	}

	/* A lead byte of two, three or four. */
	if ((bytes[0] & 0xe0U) == 0xc0U) {
		codepoint = bytes[0] & 0x1fU;
		count = 1;
	} else if ((bytes[0] & 0xf0U) == 0xe0U) {
		codepoint = bytes[0] & 0x0fU;
		count = 2;
	} else if ((bytes[0] & 0xf8U) == 0xf0U) {
		codepoint = bytes[0] & 0x07U;
		count = 3;
	} else {
		*text += 1;
		return 0xfffdU;
	}

	/* Each continuation byte adds six bits; a missing one makes the lead byte malformed. */
	for (index = 1; index <= count; index++) {
		if ((bytes[index] & 0xc0U) != 0x80U) {
			*text += 1;
			return 0xfffdU;
		}

		/* The byte's bits. */
		codepoint = (codepoint << 6) | (bytes[index] & 0x3fU);
	}

	/* Succeeded: the character, and the text after it. */
	*text += 1U + count;
	return codepoint;
}

/* Draws one glyph with its origin at x on the baseline, one to one with the atlas. */
static void
glass_draw_glyph_at(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct glass_glyph *glyph,
	int32_t x,
	int32_t baseline,
	const float *color)
{
	struct glass_shape shape;
	struct kwl_glass *glass;

	/* A space draws nothing. */
	glass = server->compose->glass;
	if (glyph->width == 0U || glyph->height == 0U)
		return;

	/* The glyph's pixels: coverage in the colour, or a colour glyph as an image (ws102-p019). */
	glass_shape_init(&shape, (float)(x + glyph->left), (float)(baseline - glyph->top), (float)glyph->width, (float)glyph->height);
	shape.mode = MODE_TEXT;
	if (glyph->color)
		shape.mode = MODE_IMAGE;
	shape.uv[0] = (float)glyph->x / (float)GLASS_ATLAS_WIDTH;
	shape.uv[1] = (float)glyph->y / (float)GLASS_ATLAS_HEIGHT;
	shape.uv[2] = (float)(glyph->x + glyph->width) / (float)GLASS_ATLAS_WIDTH;
	shape.uv[3] = (float)(glyph->y + glyph->height) / (float)GLASS_ATLAS_HEIGHT;
	memcpy(shape.color, color, sizeof(shape.color));
	shape.set = glass->atlas.set;
	glass_shape_draw(server, command, &shape);
}

/*
 * Gives a colour of the light appearance its colour in the dark one
 * (ws089-p017): a colour of little saturation (white, the greys, black and
 * the faintly tinted ones) has its lightness turned over with its tint
 * kept, so that white glass becomes dark glass and dark text light text;
 * a saturated colour (the accent, the warnings) stays as it is.  The
 * opacity is kept.
 */
static void
glass_dark_color(
	const float *color,
	float *dark)
{
	float most;
	float least;
	float shift;
	unsigned index;

	/* The colour's greatest and least channel. */
	most = color[0];
	least = color[0];
	for (index = 1U; index < 3U; index++) {
		if (color[index] > most)
			most = color[index];
		if (color[index] < least)
			least = color[index];
	}

	/* A saturated colour stays as it is. */
	dark[3] = color[3];
	if (most - least >= GLASS_DARK_SATURATION) {
		dark[0] = color[0];
		dark[1] = color[1];
		dark[2] = color[2];
		return;
	}

	/* The lightness (the middle of the greatest and the least) turned over, each channel moved by the same. */
	shift = 1.0f - (most + least);
	for (index = 0U; index < 3U; index++) {
		dark[index] = color[index] + shift;
		if (dark[index] < 0.0f)
			dark[index] = 0.0f;
		if (dark[index] > 1.0f)
			dark[index] = 1.0f;
	}
}
