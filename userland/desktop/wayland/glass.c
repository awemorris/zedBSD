/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The glass look of window mode (a proof of the look and feel):
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
 * companions (Mahora Bold, and the monospaced fallback for the
 * signs Mahora lacks); without a font the look is drawn without text.  Any other character is
 * rendered when it is first drawn, from that font or else from the fallback
 * font (server->fallback_font_path, for Japanese), into a cell of the
 * atlas's cache, the least recently drawn cell making room; text is UTF-8.
 * A character neither font has is looked for in the colour emoji font
 * (GLASS_EMOJI_FONT, opened on the first such character),
 * whose colour glyph goes into the cell as it is and is drawn as an image.
 * The titlebar's icons (icons.c) are rendered into the atlas once, at two
 * sizes, and so are the seven layers of the mark (artwork/mark.c),
 * which the login and lock screens draw.  The applications'
 * tiles (icons.c: the banded rounded square with the picture
 * cut out) are rendered in colour into an image of their own at each size
 * the compositor draws them (App Home, Alt+Tab, the system bar, the title
 * bars' marks) and drawn as images, so that what is behind a tile shows
 * through its picture.
 */

#include "glass.h"
#include "glyph-worker.h"
#include "lane.h"
#include "../artwork/mark.h"
#include "../picture/color-glyph.h"
#include "../picture/wallpaper.h"

#include <truetype/truetype.h>

#include "userland/desktop/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
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
 * five sizes, and at the sixth (App Home's clock) only the
 * space, the digits and the colon: the glyphs up to GLASS_CLOCK_LAST.
 */
#define GLASS_GLYPHS		98U
#define GLASS_SIZES		6U
#define GLASS_CLOCK_LAST	(':' - 32U)
#define GLASS_ATLAS_WIDTH	1024U
#define GLASS_ATLAS_HEIGHT	1024U
#define GLASS_FILE_MAX		(16U * 1024U * 1024U)

/* The fonts kept open: the first font, the fallback and the colour emoji font. */
#define GLASS_FACES		3U
#define GLASS_EMOJI_FONT	KEILAND_DATADIR "/fonts/keiland-emoji.ttf"

/* The icons' two sizes in pixels, and how many there are. */
#define GLASS_ICON_SIZES	2U

/*
 * The applications' tiles: the sizes kept, in pixels a side
 * (a window's mark by its title and on Wiseview's tiles, the system bar's
 * applications, Alt+Tab, App Home), how many, the image they are kept in
 * and the empty pixels between them.  Each size takes rows of its own, as
 * many as its tiles need side by side; the image is tall enough for
 * those of every picture with room to spare (tiles_create fails, and
 * no tile is drawn anywhere, when they do not fit).
 */
#define GLASS_TILE_SIZES	4U
#define GLASS_TILE_WIDTH	1024U
#define GLASS_TILE_HEIGHT	512U
#define GLASS_TILE_GAP		2U

/*
 * How far inside an application's tile, a part of its side, the blurred
 * scene its picture shows is drawn: past the tile's smoothed
 * edge, so that none of it shows round the tile, and short of its picture
 * (about a sixth of the side in).
 */
#define GLASS_TILE_SCENE_INSET	0.08f

/* The bands an application tile's reflection on App Home's floor fades out in. */
#define GLASS_REFLECTION_SLICES	8U

/* The largest side, in output pixels, of a quad joined to others into one draw (batch.c). */
#define GLASS_BATCH_SIDE	128.0f

/* The smallest inside of a shape drawn apart from its edges, in pixels (smaller ones are not worth the draws). */
#define GLASS_SPLIT_AREA	4096.0f

/*
 * How far within its box less its corner radius the inside of a glass
 * shape drawn apart lies (ws270-p007): three pixels, where the distance to
 * the edge is -3 or less, so the shader neither smooths the edge nor
 * lights it (its bright edge is the band from -2 to 0).
 */
#define GLASS_SPLIT_GLASS_INSET	3.0f

/*
 * The least area of a shape cut around the opaque insides of the windows
 * over it (ws270-p007): smaller ones (a glyph, a button) are drawn whole,
 * unless wholly hidden, as their pieces would cost more draws than pixels.
 */
#define GLASS_COVER_AREA	(96.0f * 96.0f)

/*
 * The large digits of the lock screen's clock, in an image of
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

/* The mark's layers in the atlas, in pixels a side. */
#define GLASS_MARK_PIXELS	128U

/*
 * The mark's layers again at the system bar's launcher size, in pixels a
 * side: drawn one to one, they stay crisp where the large ones
 * would be shrunk five times.  They sit in the free column right of the
 * large ones, GLASS_MARK_SMALL_ACROSS a row.
 */
#define GLASS_MARK_SMALL_PIXELS	26U
#define GLASS_MARK_SMALL_ACROSS	4U

/* The largest glyph rendered, in pixels a side. */
#define GLASS_BITMAP		64U

/*
 * What a glyph worker's job is for: a cell of the atlas's cache (a glyph's
 * coverage or a colour glyph's pixels), one of the lock screen clock's
 * large digits, or the bytes of the fallback or the emoji font.
 */
#define GLASS_JOB_CELL		0U
#define GLASS_JOB_LARGE		1U
#define GLASS_JOB_COLOR		2U
#define GLASS_JOB_FALLBACK	3U
#define GLASS_JOB_EMOJI		4U

/* The blurred wallpaper is this many times smaller than the output. */
#define GLASS_BLUR_SCALE	4U

/*
 * How long a drawing the blur thread made waits, in milliseconds, for a
 * pass where no frame in flight copies a host image's staging, so that it
 * is written into the images there are: past it new images are made
 * (which is slower than writing, but waits for nothing).
 */
#define GLASS_BLUR_HOLD_MS	200U
#define GLASS_BLUR_RADIUS	6U
#define GLASS_BLUR_PASSES	3U

/*
 * A picture decoded for the wallpaper: its pixels
 * (three bytes each, red, green and blue, the rows packed) and its size in
 * pixels.  data is NULL for the landscape drawn here.
 */
struct wallpaper_picture {
	unsigned char *data;
	uint32_t width;
	uint32_t height;
};

/*
 * The wallpaper's file read and decoded ahead on a thread of its own,
 * so that the disk and the decoding work while the
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
 * its own so that the event loop
 * never waits on the disk: the path ("-" and landscape set for the
 * landscape), the picture or the error once the thread is
 * done, and whether it runs or has not been joined yet.  lock guards done;
 * only the event loop starts it (kwl_glass_wallpaper_begin,
 * kwl_glass_landscape, kwl_glass_resize) and joins it
 * (kwl_glass_blur_take, kwl_glass_close).
 *
 * The picture is drawn at the output's size and blurred on the blur
 * thread (struct glass_blur_job); answer says the settings' request waits
 * for it, resize that it is the output's new size's (only logged), and
 * started_ms when it was asked for.
 */
struct glass_loader {
	pthread_t thread;
	pthread_mutex_t lock;
	char path[256];
	unsigned landscape;
	unsigned answer;
	unsigned resize;
	uint64_t started_ms;
	struct wallpaper_picture picture;
	int error;
	int done;
	int started;
};

/*
 * A wallpaper's drawing at an output's size and its frosted glass's blur,
 * on the blur thread (glass_blur, the event loop does not draw or blur
 * pictures): the decoded picture (the job's; data NULL for the landscape),
 * the size, the rows of both packed at their widths (made by the thread),
 * the error, and what the event loop does with it (answer, resize,
 * started_ms and path as in struct glass_loader).  The picture is kept,
 * so that a result of an output's old size is drawn again at the new one.
 */
struct glass_blur_job {
	struct kwl_lane_job lane;
	struct wallpaper_picture picture;
	uint32_t width;
	uint32_t height;
	uint32_t *rows;
	uint32_t *blurred;
	int error;
	unsigned answer;
	unsigned resize;
	uint64_t started_ms;
	char path[256];
};

/*
 * The settings' request for a wallpaper (one at a time): waiting from its
 * begin to its result, done with the result's error until
 * kwl_glass_wallpaper_poll tells it.
 */
struct glass_answer {
	unsigned waiting;
	unsigned done;
	int error;
};

/*
 * One glyph as the look draws it: its place in its image, its box and
 * advance, whether it is a colour glyph drawn as an image, and whether its
 * coverage is still being rendered by the glyph worker: a
 * pending glyph is measured with its advance but not drawn, and its cell
 * gets the coverage at the start of a later walk.
 */
struct glass_glyph {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	int32_t left;
	int32_t top;
	int32_t advance;
	uint32_t color;
	uint32_t pending;
};

/*
 * One cell of the atlas's cache: the character and size whose glyph it
 * holds (a codepoint of 0 is a free cell), the glyph's place and metrics,
 * when it was last drawn, and the number of its last assignment (a glyph
 * worker's result for an earlier one is not written).
 */
struct glass_cached {
	uint32_t codepoint;
	unsigned size;
	struct glass_glyph glyph;
	uint64_t used;
	uint64_t generation;
};

/*
 * The look's images and glyphs: the wallpaper and its blur, the atlas with
 * the ASCII glyphs at each size, the icons at their sizes, the
 * applications' tiles in their own image (tiles_ready once they are drawn
 * there), the mark's
 * layers large and at the launcher's size, and the cache of
 * other characters; the fonts stay open (with their files' bytes) to
 * render the cache's glyphs.  cache_top is the atlas row the cache starts
 * at, cache_count the cells that fit under it, clock the count of cached
 * glyphs drawn (the cells' use is ordered by it).  The large digits of the
 * lock screen's clock have their image, made when the clock
 * first asks, and are drawn again only when it asks for another size
 * (large_pixels, 0 while none is drawn).
 *
 * The glyph worker renders the cache's glyphs and the large
 * digits away from the event loop once it runs (worker_running): it opens
 * its own faces over the bytes kept here (font_size their lengths), a
 * cell's assignments are numbered by cell_generation, and the large
 * digits asked for are large_wanted at large_generation, of which
 * large_expected glyphs are being rendered and large_received are written
 * (large_failed is a size the worker could not draw, not asked again).
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
	/*
	 * The fallback and the emoji fonts being read by the glyph worker
	 * (1 from the ask, read at the worker's start, until the bytes are
	 * taken): a character the open fonts lack waits for them, drawn as
	 * nothing and measured as nothing until then.
	 */
	unsigned fallback_loading;
	unsigned emoji_loading;
	struct glass_cached cache[GLASS_CELLS];
	uint32_t cache_top;
	/*
	 * The guard of the glyphs a layer's picture in flight uses (layer-cache.c,
	 * review-4 H2): a cell used at guard_clock or later holds one, and
	 * guarded_evictions counts those given to another glyph before the
	 * picture completed, and the glyphs the picture could not draw because
	 * they were still being rendered (0: no guard).
	 */
	uint64_t guard_clock;
	uint32_t guarded_evictions;
	unsigned cache_count;
	uint64_t clock;
	struct kwl_import large;
	struct glass_glyph large_glyphs[GLASS_LARGE_GLYPHS];
	unsigned large_pixels;
	size_t font_size[GLASS_FACES];
	struct kwl_glyph_worker worker;
	unsigned worker_running;
	uint64_t cell_generation;
	unsigned large_wanted;
	uint64_t large_generation;
	unsigned large_expected;
	unsigned large_received;
	unsigned large_failed;
};

static const unsigned glass_pixels[GLASS_SIZES] = { 14U, 15U, 20U, 36U, 24U, 64U };

/* The icons' sizes in pixels. */
static const unsigned glass_icon_pixels[GLASS_ICON_SIZES] = { 16U, 20U };

/* The applications' tiles' sizes in pixels, smallest first. */
static const unsigned glass_tile_pixels[GLASS_TILE_SIZES] = { 20U, 26U, 48U, 72U };

static int glass_shape_split(const struct kwl_server *server, const float *constants, const float *quad, const float *box, float radius, VkRect2D *pieces, unsigned *count, unsigned *inside, int *opaque);
static void glass_split_rect(float x, float y, float width, float height, VkRect2D *rect);
static void glass_shape_draw_split(struct kwl_server *server, struct kwl_list *command, const float *constants, VkDescriptorSet set, VkPipeline pipeline, const VkRect2D *pieces, unsigned count, unsigned inside, int opaque);
static void glass_shape_draw_pieces(struct kwl_server *server, struct kwl_list *command, const struct glass_shape *shape, const float (*pieces)[4], unsigned count);
static void glass_shape_piece(const struct glass_shape *shape, const float *part, struct glass_shape *piece);
static int glass_shape_cover(struct kwl_server *server, struct kwl_list *command, const struct glass_shape *shape);
static void glass_shadow_trim(struct glass_shape *shape);
static int glass_shadow_picture(struct kwl_server *server, const struct glass_shape *shadow, struct glass_shape *picture);
static int glass_ring_hollow(struct kwl_server *server, struct kwl_list *command, const struct glass_shape *shape);
static unsigned glass_count_kind_of(const float *constants);
static int wallpaper_create(struct kwl_server *server, struct kwl_glass *glass, uint32_t width, uint32_t height);
static int wallpaper_target(struct kwl_server *server, struct kwl_glass *glass, uint32_t width, uint32_t height, unsigned *renewed);
static int wallpaper_renew(struct kwl_server *server, struct kwl_glass *glass, uint32_t width, uint32_t height);
static int glass_image_renew(struct kwl_server *server, struct kwl_import *place, uint32_t width, uint32_t height, VkSampler sampler);
static int wallpaper_fill(struct kwl_glass *glass, const char *path, uint32_t width, uint32_t height);
static int wallpaper_draw(struct kwl_glass *glass, struct wallpaper_picture *given, uint32_t width, uint32_t height);
static void wallpaper_pixel(uint32_t x, uint32_t y, uint32_t width, uint32_t height, float *rgb);
static float ridge(float x, float base, float amplitude, float phase);
static int blur_create(struct kwl_server *server, struct kwl_glass *glass, uint32_t width, uint32_t height);
static void wallpaper_row(const struct wallpaper_picture *picture, const uint32_t *columns, uint32_t source_y, uint32_t *line, uint32_t width);
static void landscape_row(uint32_t y, uint32_t width, uint32_t height, uint32_t *line);
static void blur_add(const uint32_t *line, uint32_t *sums, uint32_t small_width);
static void blur_average(uint32_t *sums, float *small, uint32_t small_width);
static void blur_fill(float *small, float *scratch, uint32_t width, uint32_t height, unsigned char *blurred, size_t blurred_pitch);
static int wallpaper_render(const struct wallpaper_picture *picture, uint32_t width, uint32_t height, unsigned char *rows, size_t pitch, unsigned char *blurred, size_t blurred_pitch);
static int loader_start(struct kwl_server *server, const char *path, unsigned answer, unsigned resize);
static void blur_submit(struct kwl_server *server, struct wallpaper_picture *picture, const char *path, unsigned answer, unsigned resize, uint64_t started_ms);
static void blur_run(struct kwl_lane_job *lane);
static void blur_done(void *context, struct kwl_lane_job *lane);
static void blur_drop(struct kwl_lane_job *lane);
static void blur_apply(struct kwl_server *server, struct glass_blur_job *job);
static void blur_copy(struct kwl_glass *glass, const struct glass_blur_job *job);
static void blur_free(struct glass_blur_job *job);
static void blur_answer(unsigned answer, int error);
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
static int glass_add_face(struct kwl_glass *glass, void *data, size_t size);
static int glass_ask_font(struct kwl_glass *glass, const char *path, unsigned kind);
static void glass_take_font(struct kwl_glass *glass, struct kwl_glyph_job *job);
static int glass_ask_color(struct kwl_glass *glass, unsigned glyph, enum glass_size size, unsigned slot, const struct kl_color_image *metrics);
static void glass_take_color(struct kwl_glass *glass, const struct kwl_glyph_job *job);
static void glass_worker_start(struct kwl_server *server, struct kwl_glass *glass);
static void glass_worker_font(struct kwl_glass *glass, unsigned slot);
static void glass_take_cell(struct kwl_glass *glass, const struct kwl_glyph_job *job);
static void glass_take_large(struct kwl_glass *glass, const struct kwl_glyph_job *job);
static int large_ask(struct kwl_glass *glass, unsigned pixels);
static int glass_ask_cell(struct kwl_glass *glass, unsigned font, unsigned glyph, enum glass_size size, unsigned slot, const struct truetype_glyph *metrics);
static const struct glass_glyph *glass_glyph_of(struct kwl_glass *glass, enum glass_size size, uint32_t codepoint);
static const struct glass_glyph *glass_cache_glyph(struct kwl_glass *glass, enum glass_size size, uint32_t codepoint);
static const struct glass_glyph *glass_cache_color(struct kwl_glass *glass, enum glass_size size, uint32_t codepoint, unsigned id, unsigned slot);
static uint32_t glass_utf8_next(const char **text);
static void glass_draw_glyph_at(struct kwl_server *server, struct kwl_list *command, const struct glass_glyph *glyph, int32_t x, int32_t baseline, const float *color);
static void *file_read(const char *path, size_t *size);
static int wallpaper_load(const char *path, struct wallpaper_picture *picture);
static int wallpaper_read(const char *path, struct wallpaper_picture *picture);
static void *loader_run(void *argument);
static void loader_join(void);
static void *prefetch_run(void *argument);
static int prefetch_take(const char *path, struct wallpaper_picture *picture);
static void prefetch_drop(void);
static void glass_dark_color(const float *color, float *dark);
static void glass_shape_glass_constants(float *constants);

/* The wallpaper read ahead, when kwl_glass_prefetch started it (only the main thread starts and takes it). */
static struct glass_prefetch glass_prefetch;

/*
 * The wallpaper chosen during the session being read (only the event loop
 * starts and joins it); its lock is made with the first one.
 */
static struct glass_loader glass_loader;

/* The blur thread's operations, the thread (once the look opened it) and the settings' request. */
static const struct kwl_lane_ops blur_ops = {
	blur_run,
	blur_done,
	blur_drop
};
static struct kwl_lane glass_blur;
static unsigned glass_blur_running;

/* A drawing waiting for a pass where the images may be written (GLASS_BLUR_HOLD_MS), and since when. */
static struct glass_blur_job *glass_blur_held;
static uint64_t glass_blur_held_ms;
static struct glass_answer glass_answer;

/* Whether glass_loader's lock has been made (once, by the first begin). */
static int glass_loader_ready;

/*
 * Starts reading and decoding the wallpaper's file on a thread,
 * before the Vulkan device is made; the look takes the picture
 * when it draws the wallpaper.  Without a thread the look reads the file
 * itself.
 */
void
kwl_glass_prefetch(
	struct kwl_server *server)
{
	sigset_t signals_all;
	sigset_t signals_before;
	int error;

	/* Only the glass look with a picture reads one. */
	if (!server->glass || server->wallpaper_path == NULL)
		return;

	/* The thread; without it the file is read when the wallpaper is drawn. */
	glass_prefetch.path = server->wallpaper_path;
	/* The thread blocks every signal: the stops and a child's end go to the event loop's thread. */
	(void)sigfillset(&signals_all);
	(void)pthread_sigmask(SIG_SETMASK, &signals_all, &signals_before);
	error = pthread_create(&glass_prefetch.thread, NULL, prefetch_run, NULL);
	(void)pthread_sigmask(SIG_SETMASK, &signals_before, NULL);
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
	error = wallpaper_create(server, glass, server->width, server->height);
	if (error != 0)
		return error;
	printf("KWL STARTUP step=wallpaper ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));
	printf("KWL GLASS size width=%u height=%u by=open\n", server->width, server->height);

	/* The glyphs; the look is drawn without text when the font cannot be read. */
	started = kwl_milliseconds();
	error = atlas_create(server, glass);
	if (error != 0)
		printf("KWL GLASS no text: font=%s errno=%d\n", server->font_path, error);

	/* The glyph worker renders the glyphs drawn from now on; without it the event loop renders them. */
	if (glass->text)
		glass_worker_start(server, glass);

	/* The blur thread draws and blurs the wallpapers chosen from now on; without it the event loop does. */
	if (!glass_blur_running) {
		error = kwl_lane_start(&glass_blur, "blur", 1U, server->wake[1]);
		if (error != 0)
			printf("KWL GLASS blur unavailable errno=%d the wallpaper is drawn in the event loop\n", error);
		else
			glass_blur_running = 1U;
	}
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

	/* A wallpaper still being read is waited for, and its picture let go; the blur thread ends, its jobs let go. */
	loader_join();
	free(glass_loader.picture.data);
	glass_loader.picture.data = NULL;
	if (glass_blur_running) {
		kwl_lane_stop(&glass_blur);
		glass_blur_running = 0U;
	}

	/* Lets go of a blur held for a pass. */
	if (glass_blur_held != NULL) {
		blur_free(glass_blur_held);
		glass_blur_held = NULL;
	}

	/* Forgets the answer the look kept. */
	memset(&glass_answer, 0, sizeof(glass_answer));

	/* A picture read ahead that the look never took is let go too. */
	prefetch_drop();

	/* Nothing was made. */
	if (server->compose == NULL || server->compose->glass == NULL)
		return;

	/* The glyph worker stops before the font bytes its faces read go. */
	glass = server->compose->glass;
	if (glass->worker_running) {
		kwl_glyph_worker_stop(&glass->worker);
		glass->worker_running = 0U;
	}

	/* The fonts kept open for the cache. */
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
 * Draws the landscape into the look again (the desktop's
 * preferences going back to no picture).  The blur thread draws it like a
 * picture, kwl_glass_blur_take shows it and kwl_glass_wallpaper_poll tells
 * it; without that thread the event loop draws it.
 *
 * It is drawn into the images, or into new ones of the same size while a
 * frame in flight copies a staging (the old ones go once the frames given
 * before have completed), so nothing waits.  The whole output is drawn
 * again.  Returns 0 or an errno value (the wallpaper shown stays).
 */
int
kwl_glass_landscape(
	struct kwl_server *server)
{
	struct wallpaper_picture picture;
	uint64_t started;
	unsigned renewed;
	int error;

	/* Without the look there is no wallpaper. */
	if (server->compose == NULL || server->compose->glass == NULL)
		return ENODEV;

	/*
	 * The blur thread draws it (nothing is read), and
	 * kwl_glass_wallpaper_poll tells it shown; without the thread the
	 * loader's thread hands it on when it is free, and while it reads a
	 * picture the event loop draws the landscape itself, as before.
	 */
	memset(&picture, 0, sizeof(picture));
	if (glass_blur_running) {
		glass_answer.waiting = 1U;
		blur_submit(server, &picture, "-", 1U, 0U, kwl_milliseconds());
		return 0;
	}
	if (!glass_loader.started) {
		error = loader_start(server, NULL, 1U, 0U);
		if (error == 0) {
			glass_answer.waiting = 1U;
			return 0;
		}
	}

	/* The images to draw into: the same, or new ones while a frame in flight copies a staging. */
	started = kwl_milliseconds();
	error = wallpaper_target(server, server->compose->glass, server->width, server->height, &renewed);
	if (error != 0)
		return error;

	/* Draws it into the wallpaper's image and its blurred copy, at the output's size. */
	error = wallpaper_draw(server->compose->glass, &picture, server->width, server->height);
	if (error != 0)
		return error;

	/* Everything on the output stands on it. */
	server->dirty = 1;

	/* The log names the landscape ("-") and how long it took. */
	printf("KWL GLASS wallpaper path=- ms=%llu images=%s\n", (unsigned long long)(kwl_milliseconds() - started), renewed ? "new" : "same");

	/* Succeeded: the landscape is shown from the next frame. */
	return 0;
}

/*
 * Makes the look's output-sized images again for an output of another size:
 * the wallpaper and its blurred copy, with the picture the
 * settings hold now.  The old ones go once the frames given before have
 * completed, with no wait.  Returns 0 or an errno value (the look is then
 * drawn without its wallpaper until the next change).
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

	/*
	 * With the blur thread the picture is read again and drawn there, and
	 * the old images are shown (stretched) until it is (a picture being
	 * read already is drawn at the new size when it comes).
	 */
	if (glass_blur_running) {
		if (glass_loader.started)
			return 0;
		error = loader_start(server, server->wallpaper_path, 0U, 1U);
		if (error == 0)
			return 0;
		printf("KWL GLASS resize loader errno=%d the picture is drawn in the event loop\n", error);
	}

	/* The images of the old size go once the frames given before have completed. */
	started = kwl_milliseconds();
	kwl_host_image_release(server->compose, &glass->wallpaper);
	kwl_host_image_release(server->compose, &glass->blurred);

	/* The images of the new size, with the picture in them. */
	error = wallpaper_create(server, glass, server->width, server->height);
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
 * own: the event loop goes on, the blur thread draws it, and
 * kwl_glass_wallpaper_poll tells it shown.  Returns 0, EBUSY while another
 * is being read or drawn, EINVAL for a path that is not an ordinary file (a FIFO, a device,
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
	if (glass_loader.started || glass_answer.waiting)
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

	/* The thread that reads and decodes the picture; the blur thread draws it. */
	error = loader_start(server, path, 1U, 0U);
	if (error != 0)
		return error;
	glass_answer.waiting = 1U;

	/* Succeeded: the picture is being read. */
	return 0;
}

/*
 * Tells the result of the settings' request for a wallpaper once the blur
 * thread's drawing of it is shown.  Returns 0 while there is none or it is
 * still being read or drawn, or 1 with *error 0 (the picture is shown) or
 * the errno value of reading or drawing it (the wallpaper shown stays).
 */
int
kwl_glass_wallpaper_poll(
	struct kwl_server *server,
	int *error)
{
	/* Nothing told yet. */
	(void)server;
	if (!glass_answer.done)
		return 0;

	/* Told once; another request may come from now on. */
	*error = glass_answer.error;
	memset(&glass_answer, 0, sizeof(glass_answer));
	return 1;
}

/*
 * Takes the wallpaper's work done on its threads, once a pass of the event
 * loop: a picture the loader read goes to the blur thread (or, without it,
 * is drawn here), and a drawing the blur thread made is shown.
 */
void
kwl_glass_blur_take(
	struct kwl_server *server)
{
	struct wallpaper_picture picture;
	struct glass_blur_job *job;
	uint64_t now;
	int writable;
	int done;

	/* A picture read (or not) by the loader. */
	if (glass_loader.started) {
		(void)pthread_mutex_lock(&glass_loader.lock);

		done = glass_loader.done;

		(void)pthread_mutex_unlock(&glass_loader.lock);

		/* Joined (it is done), and the picture is the event loop's. */
		if (done) {
			(void)pthread_join(glass_loader.thread, NULL);
			glass_loader.started = 0;
			picture = glass_loader.picture;
			memset(&glass_loader.picture, 0, sizeof(glass_loader.picture));
			if (glass_loader.error != 0) {
				printf("KWL GLASS no wallpaper: path=%s errno=%d\n", glass_loader.path, glass_loader.error);
				blur_answer(glass_loader.answer, glass_loader.error);
			} else {
				blur_submit(server, &picture, glass_loader.path, glass_loader.answer, glass_loader.resize, glass_loader.started_ms);
			}
		}

		/* The loader tells its end with nothing but the flag: looked at soon while it reads. */
		if (!done)
			kwl_due_soon(server);
	}

	/* The drawings made. */
	if (glass_blur_running)
		(void)kwl_lane_take(&glass_blur, server);

	/* One that waited for the images: now that they may be written, or new ones once it waited long enough. */
	job = glass_blur_held;
	if (job == NULL)
		return;
	writable = kwl_compose_host_writable(server);
	if (!writable) {
		now = kwl_milliseconds();
		if (now - glass_blur_held_ms < GLASS_BLUR_HOLD_MS) {
			kwl_due(server, glass_blur_held_ms + GLASS_BLUR_HOLD_MS);
			return;
		}
	}

	/* Shows the held drawing and lets it go. */
	glass_blur_held = NULL;
	blur_apply(server, job);
	blur_free(job);
}

/*
 * Gives the blur thread a picture to draw at the output's size (taken:
 * data NULL afterwards), for the request given; without the thread or the
 * memory of its job it is drawn here as before.
 */
static void
blur_submit(
	struct kwl_server *server,
	struct wallpaper_picture *picture,
	const char *path,
	unsigned answer,
	unsigned resize,
	uint64_t started_ms)
{
	struct glass_blur_job *job;
	struct glass_blur_job inline_job;

	/* The job, the picture its own. */
	job = NULL;
	if (glass_blur_running)
		job = calloc(1, sizeof(*job));
	if (job == NULL) {
		memset(&inline_job, 0, sizeof(inline_job));
		job = &inline_job;
	}

	/* The job takes the picture, the size and the answer. */
	job->picture = *picture;
	memset(picture, 0, sizeof(*picture));
	job->width = server->width;
	job->height = server->height;
	job->answer = answer;
	job->resize = resize;
	job->started_ms = started_ms;
	(void)snprintf(job->path, sizeof(job->path), "%s", path);

	/* On the thread. */
	if (job != &inline_job) {
		kwl_lane_submit(&glass_blur, &job->lane, &blur_ops, 0U);
		return;
	}

	/* Here, as before the thread. */
	blur_run(&job->lane);
	blur_apply(server, job);
	free(job->picture.data);
	free(job->rows);
	free(job->blurred);
}

/* Draws a job's picture at its size and blurs its frosted glass, on the blur thread. */
static void
blur_run(
	struct kwl_lane_job *lane)
{
	struct glass_blur_job *job;
	uint32_t small_width;
	uint32_t small_height;
	int drawn;

	/* The rows of the wallpaper and of the frosted glass, packed at their widths. */
	job = (struct glass_blur_job *)lane;
	small_width = job->width / GLASS_BLUR_SCALE;
	small_height = job->height / GLASS_BLUR_SCALE;
	job->rows = malloc((size_t)job->width * job->height * sizeof(*job->rows) + 1U);
	job->blurred = malloc((size_t)small_width * small_height * sizeof(*job->blurred) + 1U);
	job->error = 0;
	if (job->rows == NULL || job->blurred == NULL) {
		job->error = ENOMEM;
		return;
	}

	/* The picture drawn into them. */
	drawn = wallpaper_render(
	    &job->picture,
	    job->width,
	    job->height,
	    (unsigned char *)job->rows,
	    (size_t)job->width * sizeof(*job->rows),
	    (unsigned char *)job->blurred,
	    (size_t)small_width * sizeof(*job->blurred));
	if (drawn != 0)
		job->error = drawn;
}

/* Shows a drawing the blur thread made, on the event loop's thread, and lets its job go. */
static void
blur_done(
	void *context,
	struct kwl_lane_job *lane)
{
	struct kwl_server *server;
	struct glass_blur_job *job;
	int hold;
	int writable;

	/* A drawing of an output's old size is made again at the new one, from the same picture. */
	server = context;
	job = (struct glass_blur_job *)lane;
	if (job->error == 0 && (job->width != server->width || job->height != server->height)) {
		free(job->rows);
		free(job->blurred);
		job->rows = NULL;
		job->blurred = NULL;
		job->width = server->width;
		job->height = server->height;
		kwl_lane_submit(&glass_blur, &job->lane, &blur_ops, 0U);
		return;
	}

	/*
	 * Held for a pass where the images it fits may be written (instead of
	 * new ones made); one held already is shown first, as it is older.
	 */
	hold = 0;
	if (job->error == 0 &&
	    server->compose != NULL &&
	    server->compose->glass != NULL &&
	    server->compose->glass->wallpaper.width == job->width &&
	    server->compose->glass->wallpaper.height == job->height) {
		writable = kwl_compose_host_writable(server);
		if (!writable)
			hold = 1;
	}

	/* Shows the one held already first, then holds this one. */
	if (hold) {
		if (glass_blur_held != NULL) {
			blur_apply(server, glass_blur_held);
			blur_free(glass_blur_held);
		}

		/* Holds the job for that pass. */
		glass_blur_held = job;
		glass_blur_held_ms = kwl_milliseconds();
		return;
	}

	/* Shown, then let go. */
	blur_apply(server, job);
	blur_free(job);
}

/* Lets go of a job never run (the look's end). */
static void
blur_drop(
	struct kwl_lane_job *lane)
{
	/* Its picture, its rows and itself. */
	blur_free((struct glass_blur_job *)lane);
}

/*
 * Puts a drawing into the look's images (the same, or new ones of its size,
 * wallpaper_target), draws the output again and answers its request.
 */
static void
blur_apply(
	struct kwl_server *server,
	struct glass_blur_job *job)
{
	struct kwl_glass *glass;
	unsigned renewed;
	int error;

	/* The look went meanwhile. */
	error = job->error;
	if (server->compose == NULL || server->compose->glass == NULL)
		error = ENODEV;
	glass = NULL;
	if (error == 0)
		glass = server->compose->glass;

	/* The images to write into, then the rows. */
	renewed = 0U;
	if (error == 0)
		error = wallpaper_target(server, glass, job->width, job->height, &renewed);
	if (error == 0) {
		blur_copy(glass, job);
		server->dirty = 1;
	}

	/* The log names the picture ("-" for the landscape), or the new size, and how long since it was asked for. */
	if (error != 0)
		printf("KWL GLASS no wallpaper: path=%s errno=%d\n", job->path, error);
	else if (job->resize)
		printf("KWL GLASS resize width=%u height=%u ms=%llu images=%s\n", job->width, job->height, (unsigned long long)(kwl_milliseconds() - job->started_ms), renewed ? "new" : "same");
	else
		printf("KWL GLASS wallpaper path=%s ms=%llu images=%s\n", job->path, (unsigned long long)(kwl_milliseconds() - job->started_ms), renewed ? "new" : "same");

	/* The settings' request is answered. */
	blur_answer(job->answer, error);
}

/* Frees a blur job, its picture and its rows. */
static void
blur_free(
	struct glass_blur_job *job)
{
	/* Each part, then the job. */
	free(job->picture.data);
	free(job->rows);
	free(job->blurred);
	free(job);
}

/* Keeps the result of the settings' request, when the work was one (kwl_glass_wallpaper_poll tells it). */
static void
blur_answer(
	unsigned answer,
	int error)
{
	/* Not the settings' request. */
	if (!answer)
		return;

	/* Told at the next poll. */
	glass_answer.done = 1U;
	glass_answer.error = error;
}

/*
 * Makes the wallpaper's image and its blurred copy at a size (the output's
 * logical one: the caller says which), and draws them.
 */
static int
wallpaper_create(
	struct kwl_server *server,
	struct kwl_glass *glass,
	uint32_t width,
	uint32_t height)
{
	uint64_t started;
	VkResult result;
	int error;

	/* The output-sized image. */
	started = kwl_milliseconds();
	result = kwl_host_image_create(server->compose, width, height, server->compose->sampler, &glass->wallpaper);
	if (result != VK_SUCCESS)
		return EIO;

	/* The small image of the frosted glass. */
	error = blur_create(server, glass, width, height);
	if (error != 0)
		return error;

	/* How long the two images took; the rest of the step is the picture's reading and drawing. */
	printf("KWL STARTUP step=wallpaper-images ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

	/* The picture in both. */
	error = wallpaper_fill(glass, server->wallpaper_path, width, height);
	if (error != 0)
		return error;

	/* Succeeded: the wallpaper is drawn. */
	return 0;
}

/*
 * Readies the wallpaper's images for a new picture of a size written by
 * the caller, with no wait: the images themselves when they have the size
 * and no frame in flight copies a host image's staging (the frames in
 * flight sample the images the copies fill, and the next copy comes after
 * them in the queue), otherwise new ones (wallpaper_renew; *renewed says
 * which).  Returns 0, or EIO with the old images kept.
 */
static int
wallpaper_target(
	struct kwl_server *server,
	struct kwl_glass *glass,
	uint32_t width,
	uint32_t height,
	unsigned *renewed)
{
	int writable;
	int error;

	/* Their stagings may be written now. */
	*renewed = 0U;
	writable = kwl_compose_host_writable(server);
	if (writable && glass->wallpaper.width == width && glass->wallpaper.height == height && glass->blurred.image != VK_NULL_HANDLE)
		return 0;

	/* New ones, the old ones let go after the frames given before. */
	error = wallpaper_renew(server, glass, width, height);

	/* Reports why the images could not be renewed. */
	if (error != 0)
		return error;

	/* Succeeded: new images stand. */
	*renewed = 1U;
	return 0;
}

/*
 * Gives the wallpaper and its frosted glass new images of a size for a
 * new picture (written by the caller), the old ones let go once the
 * frames given before have completed.  Returns 0, or EIO with the old
 * images kept.
 */
static int
wallpaper_renew(
	struct kwl_server *server,
	struct kwl_glass *glass,
	uint32_t width,
	uint32_t height)
{
	struct kwl_import blurred;
	VkResult result;
	int error;

	/* The frosted glass's first, aside, so that a failure of either keeps both old ones. */
	memset(&blurred, 0, sizeof(blurred));
	result = kwl_host_image_create(server->compose, width / GLASS_BLUR_SCALE, height / GLASS_BLUR_SCALE, server->compose->linear_sampler, &blurred);
	if (result != VK_SUCCESS) {
		kwl_host_image_release(server->compose, &blurred);
		return EIO;
	}

	/* Renews the wallpaper image at the display's size. */
	error = glass_image_renew(server, &glass->wallpaper, width, height, server->compose->sampler);
	if (error != 0) {
		kwl_host_image_release(server->compose, &blurred);
		return error;
	}

	/* The new frosted glass takes the old one's place, which goes after the frames given before. */
	blurred.version = glass->blurred.version + 1U;
	blurred.draw = glass->blurred.draw;
	kwl_host_image_release(server->compose, &glass->blurred);
	kwl_host_image_move(server->compose, &blurred, &glass->blurred);
	return 0;
}

/*
 * Puts a new host image of a size in place of the one there (none, or one
 * frames given before may still sample): the old one is let go after
 * them (kwl_compose_bury), the new one keeps its way of drawing and a
 * later version (the pictures that show it are told apart).  Its staging
 * is the caller's to write.  Returns 0, or EIO with the old image kept.
 */
static int
glass_image_renew(
	struct kwl_server *server,
	struct kwl_import *place,
	uint32_t width,
	uint32_t height,
	VkSampler sampler)
{
	struct kwl_import fresh;
	VkResult result;

	/* The new image, aside. */
	memset(&fresh, 0, sizeof(fresh));
	result = kwl_host_image_create(server->compose, width, height, sampler, &fresh);
	if (result != VK_SUCCESS) {
		kwl_host_image_release(server->compose, &fresh);
		return EIO;
	}

	/* It follows the old one, which goes once nothing given uses it; then it takes the place. */
	fresh.version = place->version + 1U;
	fresh.draw = place->draw;
	if (place->image != VK_NULL_HANDLE || place->staging != VK_NULL_HANDLE)
		kwl_host_image_release(server->compose, place);
	kwl_host_image_move(server->compose, &fresh, place);
	return 0;
}

/*
 * Draws the start's picture (a PNG or a JPEG; or the landscape
 * drawn here when path is NULL or cannot be read) into the wallpaper's image
 * and its blurred copy, which are mapped, the output's size, and kept for
 * the look's life.
 *
 * The picture is made one output row at a time: each row is
 * packed, copied into the image (which is only written, never read), and
 * added to the frosted glass's block averages, so no output-sized copy of
 * the picture is kept.  A wallpaper chosen later is read on a thread
 * (kwl_glass_wallpaper_begin) and drawn by wallpaper_draw.
 */
static int
wallpaper_fill(
	struct kwl_glass *glass,
	const char *path,
	uint32_t width,
	uint32_t height)
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
	error = wallpaper_draw(glass, &picture, width, height);
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
	struct kwl_glass *glass,
	struct wallpaper_picture *given,
	uint32_t width,
	uint32_t height)
{
	struct wallpaper_picture picture;
	uint64_t started;
	int error;

	/* The picture is this function's from now on. */
	picture = *given;
	given->data = NULL;

	/* The picture into both images at their size; then its bytes go. */
	started = kwl_milliseconds();
	error = wallpaper_render(&picture, width, height, glass->wallpaper.map, glass->wallpaper.row_pitch, glass->blurred.map, glass->blurred.row_pitch);
	free(picture.data);
	if (error != 0)
		return error;

	/* The next frame copies both into their sampled images. */
	kwl_host_image_written(&glass->wallpaper);
	kwl_host_image_written(&glass->blurred);
	printf("KWL STARTUP step=wallpaper-picture ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

	/* Succeeded: both images hold the picture. */
	return 0;
}

/*
 * Draws a picture (data NULL: the landscape) at an output's size into the
 * wallpaper's rows and the frosted glass's (the output's size divided by
 * GLASS_BLUR_SCALE), each row at its pitch in bytes.  The picture is made
 * one output row at a time: each row is packed, copied out,
 * and added to the frosted glass's block averages, so no output-sized copy
 * of the picture is kept.  It touches nothing but what it is given, so the
 * wallpaper's thread runs it too.  Returns 0 or ENOMEM.
 */
static int
wallpaper_render(
	const struct wallpaper_picture *picture,
	uint32_t width,
	uint32_t height,
	unsigned char *rows,
	size_t pitch,
	unsigned char *blurred,
	size_t blurred_pitch)
{
	uint32_t *columns;
	uint32_t *line;
	uint32_t *sums;
	float *small;
	float *scratch;
	uint32_t small_width;
	uint32_t small_height;
	uint32_t x;
	uint32_t y;
	uint32_t source_y;
	uint32_t made_y;

	/* The frosted glass's size. */
	small_width = width / GLASS_BLUR_SCALE;
	small_height = height / GLASS_BLUR_SCALE;

	/* One output row, each output column's source column, a block row's sums, the small image and its passes' copy. */
	line = malloc((size_t)width * sizeof(*line));
	columns = malloc((size_t)width * sizeof(*columns));
	sums = calloc((size_t)small_width * 3U + 1U, sizeof(*sums));
	small = calloc((size_t)small_width * small_height * 3U + 1U, sizeof(*small));
	scratch = calloc((size_t)small_width * small_height * 3U + 1U, sizeof(*scratch));
	if (line == NULL || columns == NULL || sums == NULL || small == NULL || scratch == NULL) {
		free(line);
		free(columns);
		free(sums);
		free(small);
		free(scratch);
		return ENOMEM;
	}

	/* Each output column takes the source column it falls on (the nearest pixel), as a byte offset. */
	if (picture->data != NULL) {
		for (x = 0; x < width; x++)
			columns[x] = (uint32_t)((uint64_t)x * picture->width / width) * 3U;
	}

	/*
	 * Each output row: made (a row that falls on the same source row as
	 * the one above is that row again), copied out, and added to the block
	 * of the frosted glass it lies in.
	 */
	made_y = UINT32_MAX;
	for (y = 0; y < height; y++) {
		if (picture->data != NULL) {
			/* The picture's row, unless the line already holds it. */
			source_y = (uint32_t)((uint64_t)y * picture->height / height);
			if (source_y != made_y)
				wallpaper_row(picture, columns, source_y, line, width);
			made_y = source_y;
		} else {
			/* The landscape's row. */
			landscape_row(y, width, height, line);
		}

		/* Into the wallpaper's row. */
		memcpy(rows + (size_t)y * pitch, line, (size_t)width * sizeof(*line));

		/* Rows below the last whole block are not in the frosted glass. */
		if (y >= small_height * GLASS_BLUR_SCALE)
			continue;

		/* Into its block; a block's last row makes that row of the small image. */
		blur_add(line, sums, small_width);
		if (y % GLASS_BLUR_SCALE == GLASS_BLUR_SCALE - 1U)
			blur_average(sums, &small[(size_t)(y / GLASS_BLUR_SCALE) * small_width * 3U], small_width);
	}

	/* The frosted glass from the block averages. */
	blur_fill(small, scratch, small_width, small_height, blurred, blurred_pitch);

	/* The working rows are no longer needed. */
	free(line);
	free(columns);
	free(sums);
	free(small);
	free(scratch);

	/* Succeeded: both hold the picture. */
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
 * Colors one pixel of the landscape in the desktop's look: a bright sky that pales towards the
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
	struct kwl_glass *glass,
	uint32_t width,
	uint32_t height)
{
	VkResult result;

	/* The wallpaper's size divided by GLASS_BLUR_SCALE. */
	result = kwl_host_image_create(server->compose, width / GLASS_BLUR_SCALE, height / GLASS_BLUR_SCALE, server->compose->linear_sampler, &glass->blurred);
	if (result != VK_SUCCESS)
		return EIO;

	/* Succeeded: the image is there. */
	return 0;
}

/*
 * Draws the frosted glass from its small image (the wallpaper averaged down
 * by GLASS_BLUR_SCALE, blur_average): blurred by repeated box passes (close
 * to a Gaussian) in place, with scratch as the passes' copy, then packed
 * into its rows at their pitch.
 */
static void
blur_fill(
	float *small,
	float *scratch,
	uint32_t width,
	uint32_t height,
	unsigned char *blurred,
	size_t blurred_pitch)
{
	uint32_t *row;
	uint32_t x;
	uint32_t y;
	unsigned pass;

	/* Box passes across and down. */
	for (pass = 0; pass < GLASS_BLUR_PASSES; pass++) {
		blur_pass(small, scratch, width, height, 1);
		blur_pass(small, scratch, width, height, 0);
	}

	/* Into the rows. */
	for (y = 0; y < height; y++) {
		row = (uint32_t *)(blurred + (size_t)y * blurred_pitch);
		for (x = 0; x < width; x++)
			row[x] = pack_pixel(&small[((size_t)y * width + x) * 3U]);
	}
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
	uint64_t started;
	VkResult result;
	int error;

	/* The first font, kept open for the cache's glyphs. */
	started = kwl_milliseconds();
	error = glass_open_face(glass, server->font_path);
	if (error != 0)
		return error;

	/* Its companions: Mahora Bold, and the monospaced fallback for the signs it lacks. */
	(void)truetype_open_companions(glass->faces[0], KEILAND_FONT_BOLD, KEILAND_FONT_FALLBACK_MONO);
	printf("KWL STARTUP step=glyphs-fonts ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

	/*
	 * The fallback font is read by the glyph worker from its start, the
	 * emoji font too (glass_worker_start), or without the worker on the
	 * first character the first font lacks (glass_cache_glyph): reading
	 * their megabytes here would delay the compositor's start.
	 */
	glass->fallback_path = server->fallback_font_path;
	glass->fallback_tried = 0;

	/* The atlas image, transparent where nothing is drawn. */
	started = kwl_milliseconds();
	result = kwl_host_image_create(server->compose, GLASS_ATLAS_WIDTH, GLASS_ATLAS_HEIGHT, server->compose->sampler, &glass->atlas);
	if (result != VK_SUCCESS)
		return EIO;
	printf("KWL STARTUP step=glyphs-image ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

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

				/* The next frame copies the new glyph into the sampled atlas. */
				kwl_host_image_written(&glass->atlas);
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

	/* The mark's layers in the row after them. */
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

	/* Its size, bounded by GLASS_FILE_MAX (the buffer is as large as the file). */
	error = fstat(descriptor, &status);
	if (error != 0) {
		close(descriptor);
		return NULL;
	}

	/* Only an ordinary file is a picture: a FIFO or a device would hold the reader. */
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
 * Reads a picture's file and decodes it (a PNG or a JPEG;
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

/* Reads and decodes the wallpaper chosen (the landscape has nothing to read) on the loader's thread; the blur thread draws it. */
static void *
loader_run(
	void *argument)
{
	struct wallpaper_picture picture;
	int error;

	/* The thread needs nothing passed: glass_loader names the file. */
	(void)argument;

	/* Reads the file and decodes the picture; the landscape is a picture without pixels. */
	memset(&picture, 0, sizeof(picture));
	error = 0;
	if (!glass_loader.landscape)
		error = wallpaper_read(glass_loader.path, &picture);

	/* Publishes the result; done tells the event loop that the thread can be joined. */
	(void)pthread_mutex_lock(&glass_loader.lock);

	glass_loader.picture = picture;
	glass_loader.error = error;
	glass_loader.done = 1;

	(void)pthread_mutex_unlock(&glass_loader.lock);

	/* Succeeded: the thread ends. */
	return NULL;
}

/*
 * Starts the wallpaper's thread for a picture's path, or for the landscape
 * (path NULL), at the output's size now.  Returns 0, or the errno value of
 * making its lock or its thread.
 */
static int
loader_start(
	struct kwl_server *server,
	const char *path,
	unsigned answer,
	unsigned resize)
{
	sigset_t signals_all;
	sigset_t signals_before;
	int error;

	/* Makes the lock with the first picture. */
	if (!glass_loader_ready) {
		error = pthread_mutex_init(&glass_loader.lock, NULL);
		if (error != 0)
			return error;

		/* Later starts reuse the lock made here. */
		glass_loader_ready = 1;
	}

	/* Gives the thread the path ("-" for the landscape), the size and an empty result to fill. */
	glass_loader.landscape = 0U;
	if (path == NULL) {
		glass_loader.landscape = 1U;
		path = "-";
	}

	/* The path, what its result is for, and nothing read yet. */
	(void)server;
	(void)snprintf(glass_loader.path, sizeof(glass_loader.path), "%s", path);
	glass_loader.answer = answer;
	glass_loader.resize = resize;
	glass_loader.started_ms = kwl_milliseconds();
	memset(&glass_loader.picture, 0, sizeof(glass_loader.picture));
	glass_loader.error = 0;
	glass_loader.done = 0;

	/* Starts the thread that reads and decodes the picture. */
	/* The thread blocks every signal: the stops and a child's end go to the event loop's thread. */
	(void)sigfillset(&signals_all);
	(void)pthread_sigmask(SIG_SETMASK, &signals_all, &signals_before);
	error = pthread_create(&glass_loader.thread, NULL, loader_run, NULL);
	(void)pthread_sigmask(SIG_SETMASK, &signals_before, NULL);
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

/* Copies the rows the blur thread drew into the look's images (of the job's size), which the next frame copies again. */
static void
blur_copy(
	struct kwl_glass *glass,
	const struct glass_blur_job *job)
{
	uint32_t small_width;
	uint32_t small_height;
	uint32_t y;

	/* The frosted glass's size. */
	small_width = job->width / GLASS_BLUR_SCALE;
	small_height = job->height / GLASS_BLUR_SCALE;

	/* The wallpaper's rows, at the image's pitch. */
	for (y = 0; y < job->height; y++)
		memcpy((unsigned char *)glass->wallpaper.map + (size_t)y * glass->wallpaper.row_pitch, job->rows + (size_t)y * job->width, (size_t)job->width * sizeof(uint32_t));

	/* The frosted glass's rows. */
	for (y = 0; y < small_height; y++)
		memcpy((unsigned char *)glass->blurred.map + (size_t)y * glass->blurred.row_pitch, job->blurred + (size_t)y * small_width, (size_t)small_width * sizeof(uint32_t));

	/* The next frame copies both into their sampled images. */
	kwl_host_image_written(&glass->wallpaper);
	kwl_host_image_written(&glass->blurred);
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
	struct kwl_list *command,
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
	VkPipeline pipeline;
	VkRect2D pieces[1U + KWL_CUT_PIECES];
	struct glass_shape trimmed;
	struct glass_shape picture;
	uint64_t pixels;
	unsigned count;
	unsigned inside;
	unsigned kind;
	int joined;
	int visible;
	int covered;
	int opaque;
	int split;
	int pictured;
	int hollow;

	/* A shape of a layer faded out wholly is not drawn (the desktop behind App Home). */
	if (server->layer_on && server->layer_opacity <= 0.0f)
		return;

	/*
	 * A shadow's quad reaches past where it fades out wholly: it is drawn
	 * only as far as it shows (ws270-p007), unless a layer scales it.
	 */
	if (shape->mode == MODE_SHADOW && !server->layer_on) {
		trimmed = *shape;
		glass_shadow_trim(&trimmed);
		shape = &trimmed;

		/* Drawn from the shadow's picture when it has one (glass-map.c). */
		pictured = glass_shadow_picture(server, &trimmed, &picture);
		if (pictured)
			shape = &picture;
	}

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

	/* On a head, its part of the plane: the quad and the box in its own pixels. */
	if (server->view_width != 0U) {
		quad[0] -= (float)server->view_x;
		quad[1] -= (float)server->view_y;
		box[0] -= (float)server->view_x;
		box[1] -= (float)server->view_y;
		width = (float)server->view_width;
		height = (float)server->view_height;
	}

	/* Completely clipped quads cannot affect a pixel: outside the scissor, or between the damage's rectangles. */
	visible = kwl_compose_clip_visible(compose, quad[0], quad[1], quad[2], quad[3]);
	if (!visible)
		return;

	/*
	 * Under the opaque insides of windows drawn later it is not seen
	 * (ws270-p007): it is left out under them, drawn as the pieces of its
	 * quad around them, or not at all.
	 */
	covered = glass_shape_cover(server, command, shape);
	if (covered)
		return;

	/* An outline is drawn around its empty middle, where it covers nothing. */
	hollow = glass_ring_hollow(server, command, shape);
	if (hollow)
		return;

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
	 * In the dark appearance the glass, the solid colours, the
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

	/* The glass's values that are the same over the whole shape, worked out once here. */
	if (shape->mode == MODE_GLASS)
		glass_shape_glass_constants(constants);

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

	/* Known modes omit unused branches; other values keep the original shader. */
	pipeline = compose->panel_pipeline;
	if (shape->mode == MODE_GLASS && constants[17] < 0.0f)
		pipeline = compose->panel_pipelines[KWL_PANEL_DARK_GLASS];
	else if (shape->mode == MODE_GLASS)
		pipeline = compose->panel_pipelines[0];
	else if (shape->mode == MODE_SHADOW)
		pipeline = compose->panel_pipelines[1];
	else if (shape->mode == MODE_IMAGE)
		pipeline = compose->panel_pipelines[2];
	else if (shape->mode == MODE_SOLID)
		pipeline = compose->panel_pipelines[3];
	else if (shape->mode == MODE_RING)
		pipeline = compose->panel_pipelines[4];
	else if (shape->mode == MODE_TEXT)
		pipeline = compose->panel_pipelines[5];
	else if (shape->mode == MODE_BLUR)
		pipeline = compose->panel_pipelines[6];

	/*
	 * App Home's floor lines (small solid quads) are joined into one draw
	 * (batch.c, the floors first); anything else draws
	 * the quads waiting first, then itself alone.
	 */
	if (server->batch_floors &&
	    shape->mode == MODE_SOLID &&
	    constants[17] == (float)MODE_SOLID &&
	    quad[2] <= GLASS_BATCH_SIDE &&
	    quad[3] <= GLASS_BATCH_SIDE) {
		joined = kwl_batch_add(server, command, constants, set);
		if (joined)
			return;
	}

	/*
	 * Glyphs that follow one another are joined into one draw (batch.c,
	 * ws270-p007): a line of text was a draw a glyph, and on the PowerVR
	 * every 64 draws of a pass cost another load of the whole pass.
	 */
	if (constants[17] == (float)MODE_TEXT) {
		joined = kwl_batch_add_text(server, command, constants, set);
		if (joined) {
			pixels = kwl_compose_count_pixels(server, quad[0], quad[1], quad[2], quad[3], 1);
			kwl_compose_count_kind(server, KWL_COUNT_TEXT, pixels);
			return;
		}
	}
	kwl_batch_flush(server, command);

	/* A glass, image or solid shape large enough is drawn as its inside and its edges. */
	split = glass_shape_split(server, constants, quad, box, radius, pieces, &count, &inside, &opaque);
	if (split) {
		glass_shape_draw_split(server, command, constants, set, pipeline, pieces, count, inside, opaque);
		return;
	}

	/* The chosen pipeline keeps the same constants, image and triangle pair. */
	kwl_compose_count_draw(server, pipeline, set, 1U);
	pixels = kwl_compose_count_pixels(server, quad[0], quad[1], quad[2], quad[3], 1);
	kind = glass_count_kind_of(constants);
	kwl_compose_count_kind(server, kind, pixels);
	kwl_compose_bind(server, command, pipeline, compose->panel_layout, set);
	kwl_list_push_constants(command, compose->panel_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0U, sizeof(constants), constants);
	kwl_compose_clip_draw(server, command, quad[0], quad[1], quad[2], quad[3], KWL_QUAD_VERTICES);
	kwl_compose_count_commands(server, 1U, 1U);
}

/*
 * Tells whether a shape about to be drawn is drawn in parts: a glass
 * (light or dark), shadow, image or solid shape whose inside is at least
 * GLASS_SPLIT_AREA pixels.  The inside is the box less its corners and
 * smoothed edge, where the shader covers every pixel fully, for glass
 * less its bright edge too (GLASS_SPLIT_GLASS_INSET), for a shadow where
 * it is at its full alpha; there the shader's
 * inside variant (KWL_PANEL_INSIDE) gives the same colour without working
 * out the distance to the edge.  The inside, in whole pixels, is
 * pieces[0]; the quad's rest around it follows, *count rectangles in all;
 * *inside is the inside's variant (KWL_PANEL_INSIDE_*).  *opaque says the
 * inside is opaque (at full opacity, glass, an image whose pixels are all
 * opaque or a solid colour of full alpha): it then needs no blending, the
 * blend of a colour of alpha 1 being that colour, and the GPU can drop
 * what is drawn under it.
 */
static int
glass_shape_split(
	const struct kwl_server *server,
	const float *constants,
	const float *quad,
	const float *box,
	float radius,
	VkRect2D *pieces,
	unsigned *count,
	unsigned *inside,
	int *opaque)
{
	float cuts[KWL_CUT_PIECES][4];
	float covered[4];
	float hole[1][4];
	float inset;
	float left;
	float top;
	float right;
	float bottom;
	unsigned found;
	unsigned index;

	/* Only the glass, image and solid modes as themselves, in the frame's own pass. */
	if (server->compose->backdrop_recording != 0U)
		return 0;
	if (constants[17] == (float)MODE_IMAGE) {
		*inside = KWL_PANEL_INSIDE_IMAGE;
		*opaque = constants[19] >= 1.0f;
	} else if (constants[17] == (float)MODE_SOLID) {
		*inside = KWL_PANEL_INSIDE_SOLID;
		*opaque = constants[15] >= 1.0f;
	} else if (constants[17] == (float)MODE_GLASS) {
		*inside = KWL_PANEL_INSIDE_GLASS;
		*opaque = 1;
	} else if (constants[17] < 0.0f) {
		*inside = KWL_PANEL_INSIDE_DARK_GLASS;
		*opaque = 1;
	} else if (constants[17] == (float)MODE_SHADOW) {
		*inside = KWL_PANEL_INSIDE_SHADOW;
		*opaque = 0;
	} else {
		return 0;
	}

	/* A shape faded as a whole is seen through everywhere; an opaque inside is written with its opacity as 1. */
	if (constants[23] != 1.0f)
		*opaque = 0;
	if (quad[2] <= 0.0f || quad[3] <= 0.0f)
		return 0;

	/* The inside in whole pixels, within the quad (every pixel centre there is covered, and off a glass edge). */
	glass_opaque_inside(box, radius, covered);
	if (*inside == KWL_PANEL_INSIDE_GLASS || *inside == KWL_PANEL_INSIDE_DARK_GLASS) {
		covered[0] += GLASS_SPLIT_GLASS_INSET - 1.0f;
		covered[1] += GLASS_SPLIT_GLASS_INSET - 1.0f;
		covered[2] -= 2.0f * (GLASS_SPLIT_GLASS_INSET - 1.0f);
		covered[3] -= 2.0f * (GLASS_SPLIT_GLASS_INSET - 1.0f);
	}

	/*
	 * A shadow is at its full alpha where the distance is at most half its
	 * softness below 0 (the shader's smoothstep from there): the box less
	 * the larger of its radius and that half, and a pixel more.
	 */
	if (*inside == KWL_PANEL_INSIDE_SHADOW) {
		inset = constants[18] > 1.0f ? constants[18] : 1.0f;
		inset *= 0.5f;
		if (inset < radius)
			inset = radius;
		inset += 1.0f;
		covered[0] = box[0] + inset;
		covered[1] = box[1] + inset;
		covered[2] = box[2] - 2.0f * inset;
		covered[3] = box[3] - 2.0f * inset;
	}
	left = covered[0] > quad[0] ? covered[0] : quad[0];
	top = covered[1] > quad[1] ? covered[1] : quad[1];
	right = covered[0] + covered[2] < quad[0] + quad[2] ? covered[0] + covered[2] : quad[0] + quad[2];
	bottom = covered[1] + covered[3] < quad[1] + quad[3] ? covered[1] + covered[3] : quad[1] + quad[3];
	left = ceilf(left);
	top = ceilf(top);
	right = floorf(right);
	bottom = floorf(bottom);
	if (right <= left || bottom <= top || (right - left) * (bottom - top) < GLASS_SPLIT_AREA)
		return 0;
	glass_split_rect(left, top, right - left, bottom - top, &pieces[0]);

	/* The quad's rest, whose outer edges are taken outward (nothing is drawn outside the quad). */
	hole[0][0] = left;
	hole[0][1] = top;
	hole[0][2] = right - left;
	hole[0][3] = bottom - top;
	found = kwl_damage_rects_cut(quad, (const float (*)[4])hole, 1U, cuts);
	for (index = 0U; index < found; index++)
		glass_split_rect(cuts[index][0], cuts[index][1], cuts[index][2], cuts[index][3], &pieces[1U + index]);
	*count = 1U + found;
	return 1;
}

/* A rectangle of the pass's pixels around a part of a quad: its edges taken outward to whole pixels. */
static void
glass_split_rect(
	float x,
	float y,
	float width,
	float height,
	VkRect2D *rect)
{
	float left;
	float top;
	float right;
	float bottom;

	/* Whole pixels, never below 0 (the pass begins there). */
	left = floorf(x);
	top = floorf(y);
	right = ceilf(x + width);
	bottom = ceilf(y + height);
	if (left < 0.0f)
		left = 0.0f;
	if (top < 0.0f)
		top = 0.0f;
	if (right < left)
		right = left;
	if (bottom < top)
		bottom = top;
	rect->offset.x = (int32_t)left;
	rect->offset.y = (int32_t)top;
	rect->extent.width = (uint32_t)(right - left);
	rect->extent.height = (uint32_t)(bottom - top);
}

/* Draws a shape split by glass_shape_split: its inside first (without blending when opaque), then its edges blended. */
static void
glass_shape_draw_split(
	struct kwl_server *server,
	struct kwl_list *command,
	const float *constants,
	VkDescriptorSet set,
	VkPipeline pipeline,
	const VkRect2D *pieces,
	unsigned count,
	unsigned inside,
	int opaque)
{
	struct kwl_compose *compose;
	VkDescriptorSet inside_set;
	VkPipeline middle;
	uint64_t pixels;
	unsigned index;
	unsigned kind;
	int mapped;

	/*
	 * The inside of flat glass on the blurred wallpaper reads its colour
	 * from the colour's map when there is one (glass-map.c).
	 */
	compose = server->compose;
	inside_set = set;
	if (inside == KWL_PANEL_INSIDE_GLASS || inside == KWL_PANEL_INSIDE_DARK_GLASS) {
		mapped = kwl_glass_map_set(server, constants, set, &inside_set);
		if (mapped)
			inside = KWL_PANEL_INSIDE_MAP;
	}

	/* The inside, counted by its shape's kind (KWL MODES). */
	kind = glass_count_kind_of(constants);
	middle = compose->panel_inside_pipelines[inside];
	if (opaque)
		middle = compose->panel_opaque_pipelines[inside];
	kwl_compose_count_draw(server, middle, inside_set, 1U);
	pixels = kwl_compose_count_pixels(server, (float)pieces[0].offset.x, (float)pieces[0].offset.y, (float)pieces[0].extent.width, (float)pieces[0].extent.height, !opaque);
	kwl_compose_count_kind(server, kind, pixels);
	kwl_compose_bind(server, command, middle, compose->panel_layout, inside_set);
	kwl_list_push_constants(command, compose->panel_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0U, KWL_PANEL_CONSTANTS * sizeof(float), constants);
	kwl_compose_clip_draw_rect(server, command, &pieces[0], KWL_QUAD_VERTICES);

	/* The edges around it, blended over what is under them; the constants stay. */
	kwl_compose_bind(server, command, pipeline, compose->panel_layout, set);
	for (index = 1U; index < count; index++) {
		kwl_compose_count_draw(server, pipeline, set, 1U);
		pixels = kwl_compose_count_pixels(server, (float)pieces[index].offset.x, (float)pieces[index].offset.y, (float)pieces[index].extent.width, (float)pieces[index].extent.height, 1);
		kwl_compose_count_kind(server, kind, pixels);
		kwl_compose_clip_draw_rect(server, command, &pieces[index], KWL_QUAD_VERTICES);
	}
	kwl_compose_count_commands(server, count, count);
}

/*
 * Records one shape except where opaque shapes drawn after it hide it:
 * holes are the insides of those shapes (glass_opaque_inside), and the
 * shape is drawn as the pieces of its quad around them (kwl_damage_rects_cut),
 * each with the part of the image it covers.  The GPU then shades nothing
 * that is drawn over anyway.  In a layer seen through (the desktop going
 * back behind App Home) the shapes over it are not opaque, so it is drawn
 * whole.
 */
void
glass_shape_draw_except(
	struct kwl_server *server,
	struct kwl_list *command,
	const struct glass_shape *shape,
	const float (*holes)[4],
	unsigned hole_count)
{
	float pieces[KWL_CUT_PIECES][4];
	unsigned count;

	/* No hole: the whole shape. */
	if (hole_count == 0U) {
		glass_shape_draw(server, command, shape);
		return;
	}

	/* The shapes over it are seen through in a fading layer: the whole shape. */
	if (server->layer_on && server->layer_opacity < 1.0f) {
		glass_shape_draw(server, command, shape);
		return;
	}

	/* The quad less the holes worth cutting; an empty quad has nothing to cut. */
	if (shape->quad[2] <= 0.0f || shape->quad[3] <= 0.0f)
		return;
	count = kwl_damage_rects_cut(shape->quad, holes, hole_count, pieces);

	/* Each piece is the shape over a part of its quad. */
	glass_shape_draw_pieces(server, command, shape, (const float (*)[4])pieces, count);
}

/*
 * Gives the part of a shape inside a rectangle (x, y, width, height on the
 * output) as a shape of its own: its quad cut to the rectangle, showing
 * that part of the image; the box and the rest stay the shape's.  Returns
 * 1, or 0 when nothing of the quad is inside.
 */
int
glass_shape_crop(
	const struct glass_shape *shape,
	const float *rect,
	struct glass_shape *piece)
{
	float cut[1][4];
	float left;
	float top;
	float right;
	float bottom;

	/* The quad's edges cut to the rectangle's. */
	left = shape->quad[0];
	top = shape->quad[1];
	right = shape->quad[0] + shape->quad[2];
	bottom = shape->quad[1] + shape->quad[3];
	if (left < rect[0])
		left = rect[0];
	if (top < rect[1])
		top = rect[1];
	if (right > rect[0] + rect[2])
		right = rect[0] + rect[2];
	if (bottom > rect[1] + rect[3])
		bottom = rect[1] + rect[3];

	/* Nothing of the quad inside. */
	if (right <= left || bottom <= top)
		return 0;

	/* The shape over that part of its quad. */
	cut[0][0] = left;
	cut[0][1] = top;
	cut[0][2] = right - left;
	cut[0][3] = bottom - top;
	glass_shape_piece(shape, cut[0], piece);

	/* Succeeded: the part inside. */
	return 1;
}

/*
 * Works out the hole an opaque rounded rectangle (box: x, y, width, height)
 * makes in the shapes under it: the box less its corner radius and one more
 * pixel on every side, where the shader covers every pixel fully (cover 1).
 */
void
glass_opaque_inside(
	const float *box,
	float radius,
	float *hole)
{
	float inset;

	/* The box shrunk by the radius and the pixel of its smoothed edge. */
	inset = radius + 1.0f;
	hole[0] = box[0] + inset;
	hole[1] = box[1] + inset;
	hole[2] = box[2] - 2.0f * inset;
	hole[3] = box[3] - 2.0f * inset;
}

/* Draws a rounded rectangle in a solid color. */
void
glass_draw_solid(
	struct kwl_server *server,
	struct kwl_list *command,
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
	struct kwl_list *command,
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
 * Draws a line of UTF-8 text centred on x (a button's word, a board's
 * line), cut in the middle where it is wider than limit.  The middle cut's
 * x is the line's left edge; a caller that gave it the centre drew the
 * word from the centre on, half its width to the right.
 */
void
glass_draw_text_centred(
	struct kwl_server *server,
	struct kwl_list *command,
	enum glass_size size,
	int32_t centre,
	int32_t baseline,
	const char *text,
	int32_t limit,
	const float *color)
{
	int32_t width;

	/* The line's width, at most the room. */
	width = glass_text_width(server, size, text);
	if (width > limit)
		width = limit;

	/* Succeeded: drawn from half its width left of the centre. */
	glass_draw_text_middle(server, command, size, centre - width / 2, baseline, text, limit, color);
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
	struct kwl_list *command,
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
	struct kwl_list *command,
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
 * clock) at a size in pixels, unless they are at that size
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
	int writable;
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

	/* A size the glyph worker is rendering, or could not render. */
	if (glass->large_wanted == pixels)
		return EAGAIN;
	if (glass->large_failed == pixels)
		return EINVAL;

	/*
	 * The image the first time; later the event loop draws into it again
	 * when no frame in flight copies a host image's staging, otherwise into
	 * a new one, the frames in flight going on with the old one, let go
	 * after them (the glyph worker's digits are written at the start of a
	 * walk, when no frame in flight copies one either).
	 */
	if (glass->large.image == VK_NULL_HANDLE) {
		result = kwl_host_image_create(server->compose, GLASS_LARGE_WIDTH, GLASS_LARGE_HEIGHT, server->compose->sampler, &glass->large);
		if (result != VK_SUCCESS)
			return EIO;
	} else if (!glass->worker_running) {
		writable = kwl_compose_host_writable(server);
		if (!writable) {
			error = glass_image_renew(server, &glass->large, GLASS_LARGE_WIDTH, GLASS_LARGE_HEIGHT, server->compose->sampler);
			if (error != 0)
				return error;
		}
	}

	/*
	 * With the glyph worker the digits are rendered there and written at
	 * the start of a later walk; the caller draws an atlas
	 * size until then.
	 */
	if (glass->worker_running) {
		error = large_ask(glass, pixels);
		if (error != 0 && error != EAGAIN) {
			printf("KWL GLASS large pixels=%u errno=%d\n", pixels, error);
			glass->large_failed = pixels;
			glass->large_wanted = 0U;
		}

		/* EAGAIN while the worker renders them, or why they cannot be. */
		return error;
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
	struct kwl_list *command,
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
	struct kwl_list *command,
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
 * floor reflects it (App Home's stage): the tile's lower part,
 * height pixels of it, mirrored, its opacity from opacity at the floor
 * fading to nothing downwards (in GLASS_REFLECTION_SLICES bands).
 */
void
glass_draw_app_tile_reflection(
	struct kwl_server *server,
	struct kwl_list *command,
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
 * scene under the glass the tile is on, unwhitened, with
 * GLASS_HOLE_WALLPAPER the desktop's wallpaper itself, sharp, as if the
 * picture were a real hole through the bar or App Home (the
 * 2026-10-06 user decision).  lighten
 * (0..1) whitens the tile itself, as a lit button.
 */
void
glass_draw_app_tile(
	struct kwl_server *server,
	struct kwl_list *command,
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
 * Draws the mark in a square of a size in pixels at (x, y): its seven
 * layers from the atlas, each in the colour of the look asked for, the
 * whole as opaque as asked (0..1).  A mark about the launcher's size uses
 * the layers rendered at that size.
 */
void
glass_draw_mark(
	struct kwl_server *server,
	struct kwl_list *command,
	int32_t x,
	int32_t y,
	unsigned pixels,
	enum glass_mark_look look,
	float opacity)
{
	/*
	 * The bar pale, its shade deeper, the leaf clearer and its shade the
	 * deep blue of the splash, the overlap deeper still, then the white
	 * light along the edges and the sheen.  The panes are
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
	 * The same layers for the system bar (the 2026-09-28 user
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
 * output (heads.c); NULL without the glass look or before the
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

/* Gives the blurred wallpaper the glass samples (its set, generation and version), or NULL before the glass look opens. */
const struct kwl_import *
glass_blurred(
	struct kwl_server *server)
{
	/* No glass look yet. */
	if (server->compose->glass == NULL)
		return NULL;

	/* The quarter-size image. */
	return &server->compose->glass->blurred;
}

/* Gives the version of the wallpaper's image (it grows with each new picture), for the keys of pictures that show it. */
uint32_t
glass_wallpaper_version(
	struct kwl_server *server)
{
	/* The full-size image's version. */
	return server->compose->glass->wallpaper.version;
}

/*
 * Guards the glyphs a layer's picture being recorded uses (layer-cache.c,
 * review-4 H2): every glyph looked up from now on is used later than the
 * guard's start, and its eviction before glass_glyph_guard_end is counted.
 * A guard already up (another picture of the same submission) stays.
 */
void
glass_glyph_guard_begin(
	struct kwl_server *server)
{
	struct kwl_glass *glass;

	/* The first picture of a submission starts the guard. */
	glass = server->compose->glass;
	if (glass->guard_clock == 0U)
		glass->guard_clock = glass->clock + 1U;
}

/* Ends the guard when the pictures completed; gives how many guarded glyphs were evicted meanwhile. */
uint32_t
glass_glyph_guard_end(
	struct kwl_server *server)
{
	struct kwl_glass *glass;
	uint32_t evictions;

	/* The count, and no guard. */
	glass = server->compose->glass;
	evictions = glass->guarded_evictions;
	glass->guard_clock = 0U;
	glass->guarded_evictions = 0U;
	return evictions;
}

/*
 * Redraws the output when the glyph worker has rendered glyphs:
 * the next walk writes them into their cells and draws the text that
 * waited for them.  The event loop calls it every pass.
 */
void
kwl_glass_glyphs_poll(
	struct kwl_server *server)
{
	struct kwl_glass *glass;
	int ready;

	/* Without the look or its worker nothing is rendered elsewhere. */
	if (server->compose == NULL || server->compose->glass == NULL)
		return;
	glass = server->compose->glass;
	if (!glass->worker_running)
		return;

	/* Whether rendered glyphs wait to be taken. */
	ready = kwl_glyph_worker_ready(&glass->worker);
	if (!ready)
		return;

	/* The whole output again, for the text drawn without them. */
	server->dirty = 1;
}

/*
 * Writes the glyphs the glyph worker rendered into their cells and the
 * large digits' image, in the order they were asked for, so that a cell
 * given twice keeps the later glyph.  It runs at the start of
 * a walk, the only time the look's images are written while the frames
 * read them.
 */
void
kwl_glass_glyphs_take(
	struct kwl_compose *compose)
{
	struct kwl_glass *glass;
	struct kwl_glyph_job *job;
	struct kwl_glyph_job *next;

	/* Without the look or its worker there is nothing to take. */
	glass = compose->glass;
	if (glass == NULL || !glass->worker_running)
		return;

	/* Each rendered glyph, oldest first, into what asked for it. */
	job = kwl_glyph_worker_take(&glass->worker);
	while (job != NULL) {
		next = job->next;
		if (job->kind == GLASS_JOB_CELL) {
			glass_take_cell(glass, job);
		} else if (job->kind == GLASS_JOB_COLOR) {
			glass_take_color(glass, job);
		} else if (job->kind == GLASS_JOB_FALLBACK || job->kind == GLASS_JOB_EMOJI) {
			glass_take_font(glass, job);
		} else {
			glass_take_large(glass, job);
		}

		/* The job is done with. */
		kwl_glyph_job_free(job);
		job = next;
	}
}

/* Tells whether the applications' tiles are drawn (1) or not yet (0). */
uint32_t
glass_tiles_ready(
	struct kwl_server *server)
{
	/* The tiles' image is drawn once. */
	return server->compose->glass->tiles_ready != 0U ? 1U : 0U;
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

	/* Succeeded: the tiles are drawn, and the next frame copies them into the sampled image. */
	kwl_host_image_written(&glass->tiles);
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

	/* The next frame copies the new glyph into the sampled image. */
	kwl_host_image_written(&glass->large);
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
 * Renders the mark's layers into the atlas side by side from a row on,
 * the small ones at the launcher's size in the column right of them,
 * and moves the row past them; returns 0, or ENOSPC when the
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

	/* The next frame copies the new coverage into the sampled atlas. */
	kwl_host_image_written(&glass->atlas);
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

	/* The font in it, kept. */
	error = glass_add_face(glass, data, size);
	if (error != 0)
		return error;

	/* Succeeded: the font can render glyphs. */
	return 0;
}

/*
 * Opens a font over a file's bytes and keeps both for the cache (the bytes
 * are the look's from then on, freed when it could not be opened); returns
 * 0, ENOENT without room, or EINVAL.
 */
static int
glass_add_face(
	struct kwl_glass *glass,
	void *data,
	size_t size)
{
	struct truetype_face *face;
	int error;

	/* No room for another font. */
	if (glass->face_count == GLASS_FACES) {
		free(data);
		return ENOENT;
	}

	/* The font in the bytes. */
	error = truetype_open(data, size, 0U, &face);
	if (error != 0) {
		free(data);
		return EINVAL;
	}

	/* Kept until the look closes. */
	glass->faces[glass->face_count] = face;
	glass->font_data[glass->face_count] = data;
	glass->font_size[glass->face_count] = size;
	glass->face_count++;

	/* The glyph worker opens its own face of it when a glyph first needs it. */
	glass_worker_font(glass, glass->face_count - 1U);

	/* Succeeded: the font can render glyphs. */
	return 0;
}

/*
 * Asks the glyph worker for a font file's bytes (the fallback or the emoji
 * font); returns 0, or ENOMEM with nothing asked.
 */
static int
glass_ask_font(
	struct kwl_glass *glass,
	const char *path,
	unsigned kind)
{
	struct kwl_glyph_job *job;

	/* The job, the worker's from its submission. */
	job = calloc(1, sizeof(*job));
	if (job == NULL)
		return ENOMEM;

	/* The file to read, kept by the server (or a constant) until the job is taken. */
	job->task = KWL_GLYPH_TASK_READ;
	job->path = path;
	job->kind = kind;
	kwl_glyph_worker_submit(&glass->worker, job);

	/* Succeeded: the bytes come back as a job taken. */
	return 0;
}

/*
 * Opens the fallback or the emoji font over the bytes the glyph worker
 * read (the job's bytes become the look's), or tells why it could not be;
 * the characters that waited for it are measured and drawn from the next
 * walk on.
 */
static void
glass_take_font(
	struct kwl_glass *glass,
	struct kwl_glyph_job *job)
{
	const char *path;
	int error;

	/* The font the bytes are of, read no longer. */
	if (job->kind == GLASS_JOB_EMOJI) {
		path = GLASS_EMOJI_FONT;
		glass->emoji_loading = 0U;
	} else {
		path = glass->fallback_path;
		glass->fallback_loading = 0U;
	}

	/* The face over the bytes (which are the look's from now on), or the reason there is none. */
	error = job->error;
	if (error == 0) {
		error = glass_add_face(glass, job->data, job->size);
		job->data = NULL;
	}

	/* The emoji font's face is asked apart from the others. */
	if (job->kind == GLASS_JOB_EMOJI) {
		if (error == 0)
			glass->emoji_face = (int)glass->face_count - 1;
		printf("KWL GLASS emoji font: path=%s errno=%d\n", path, error);
		return;
	}

	/* The fallback font's line, as when it was opened on the first character it had. */
	if (error != 0) {
		printf("KWL GLASS no fallback font: path=%s errno=%d\n", path, error);
	} else {
		printf("KWL GLASS fallback font: path=%s faces=%u\n", path, glass->face_count);
	}
}

/*
 * Starts the glyph worker and gives it the fonts open now;
 * the main thread's wake pipe tells the event loop of rendered glyphs.
 * Without it the cache's glyphs are rendered by the event loop as before.
 */
static void
glass_worker_start(
	struct kwl_server *server,
	struct kwl_glass *glass)
{
	unsigned slot;
	int error;

	/* The thread. */
	error = kwl_glyph_worker_start(&glass->worker, server->wake[1]);
	if (error != 0) {
		printf("KWL GLASS worker unavailable errno=%d\n", error);
		return;
	}

	/* The cache asks it from now on. */
	glass->worker_running = 1U;

	/* The fonts opened so far; a later one is given when it opens. */
	for (slot = 0U; slot < glass->face_count; slot++)
		glass_worker_font(glass, slot);
	printf("KWL GLASS worker started faces=%u\n", glass->face_count);

	/* The fallback font's file read ahead (one that cannot be asked is read on its first character, as without the worker). */
	if (glass->fallback_tried == 0U) {
		error = glass_ask_font(glass, glass->fallback_path, GLASS_JOB_FALLBACK);
		if (error == 0) {
			glass->fallback_tried = 1U;
			glass->fallback_loading = 1U;
		}
	}

	/* The emoji font's file likewise, after the fallback's. */
	if (glass->emoji_tried == 0U) {
		error = glass_ask_font(glass, GLASS_EMOJI_FONT, GLASS_JOB_EMOJI);
		if (error == 0) {
			glass->emoji_tried = 1U;
			glass->emoji_face = -1;
			glass->emoji_loading = 1U;
		}
	}
}

/* Gives the glyph worker a font the look opened: its bytes, and the first font's companions. */
static void
glass_worker_font(
	struct kwl_glass *glass,
	unsigned slot)
{
	struct kwl_glyph_font font;

	/* Without the worker the event loop renders on its own faces. */
	if (!glass->worker_running)
		return;

	/* The bytes, kept unchanged until the look closes (after the worker stops). */
	memset(&font, 0, sizeof(font));
	font.data = glass->font_data[slot];
	font.size = glass->font_size[slot];

	/* The first font with the companions atlas_create opened it with. */
	if (slot == 0U) {
		font.bold_path = KEILAND_FONT_BOLD;
		font.next_path = KEILAND_FONT_FALLBACK_MONO;
	}

	/* The worker reads it under its lock. */
	kwl_glyph_worker_font(&glass->worker, slot, &font);
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
	unsigned pending;
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

	/*
	 * No font open has it while the fallback or the emoji font is still
	 * being read: nothing now, and the character is looked up again once
	 * the font came (a layer's picture recorded without it is counted with
	 * the guard's evictions, so that it is drawn again too).
	 */
	if (id == 0U && (glass->fallback_loading || glass->emoji_loading)) {
		if (glass->guard_clock != 0U)
			glass->guarded_evictions++;
		return NULL;
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

	/* No font has it yet: the colour emoji font, opened once on the first such character. */
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
			if (glass->guard_clock != 0U && glass->cache[oldest].codepoint != 0U && glass->cache[oldest].used >= glass->guard_clock)
				glass->guarded_evictions++;
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

	/* The cell's place in the atlas; a glyph a picture in flight uses that it held is counted. */
	cell = &glass->cache[oldest];
	if (glass->guard_clock != 0U && cell->codepoint != 0U && cell->used >= glass->guard_clock)
		glass->guarded_evictions++;
	per_row = GLASS_ATLAS_WIDTH / GLASS_CELL;
	cell->glyph.x = (oldest % per_row) * GLASS_CELL;
	cell->glyph.y = glass->cache_top + (oldest / per_row) * GLASS_CELL;

	/*
	 * The cell's new assignment: a glyph worker's result for an earlier one
	 * is not written into it.
	 */
	glass->cell_generation++;
	cell->generation = glass->cell_generation;

	/*
	 * A glyph with a box is rendered by the glyph worker when it runs: the
	 * cell waits for its coverage, written at the start of a later walk.
	 * Otherwise (no worker, or the job could not be made)
	 * the event loop renders it now.
	 */
	pending = 0U;
	if (glass->worker_running && metrics.width != 0U && metrics.height != 0U) {
		error = glass_ask_cell(glass, which, id, size, oldest, &metrics);
		if (error == 0)
			pending = 1U;
	}

	/* The glyph's coverage, into the cell, when the event loop renders it. */
	if (!pending) {
		memset(bitmap, 0, sizeof(bitmap));
		if (metrics.width != 0U && metrics.height != 0U) {
			error = truetype_render_glyph(face, id, &metrics, bitmap, metrics.width, sizeof(bitmap));
			if (error != 0)
				return NULL;
		}

		/* Into the cell. */
		atlas_put(glass, bitmap, cell->glyph.x, cell->glyph.y, metrics.width, metrics.height);
	}

	/* The cell now holds this character's glyph, or waits for it. */
	cell->codepoint = codepoint;
	cell->size = (unsigned)size;
	cell->glyph.color = 0;
	cell->glyph.pending = pending;
	cell->glyph.width = metrics.width;
	cell->glyph.height = metrics.height;
	cell->glyph.left = metrics.left;
	cell->glyph.top = metrics.top;
	cell->glyph.advance = metrics.advance;
	glass->clock++;
	cell->used = glass->clock;
	printf("KWL GLASS glyph codepoint=U+%04X size=%u face=%u cell=%u pending=%u\n", codepoint, (unsigned)size, which, oldest, pending);

	/* Succeeded: the glyph. */
	return &cell->glyph;
}

/*
 * Draws a character's colour glyph of the emoji font into a cell of the
 * cache (its premultiplied colours as they are), for glass_draw_glyph_at
 * to draw as an image; NULL when it cannot be drawn or is
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

	/*
	 * The glyph worker decodes it when it runs: the box measured here from
	 * the PNG's header, the cell waiting for its pixels, written at the
	 * start of a later walk.
	 */
	if (glass->worker_running) {
		error = kl_color_glyph_metrics(glass->faces[glass->emoji_face], id, glass_pixels[size], &image);
		if (error != 0)
			return NULL;
		if (image.width > (int)GLASS_CELL || image.height > (int)GLASS_CELL)
			return NULL;

		/* The cell's place in the atlas, a new assignment (a worker's result for an earlier one is not written). */
		cell = &glass->cache[slot];
		per_row = GLASS_ATLAS_WIDTH / GLASS_CELL;
		cell->glyph.x = (slot % per_row) * GLASS_CELL;
		cell->glyph.y = glass->cache_top + (slot / per_row) * GLASS_CELL;
		glass->cell_generation++;
		cell->generation = glass->cell_generation;

		/* Asked, or decoded here when the job could not be made. */
		error = glass_ask_color(glass, id, size, slot, &image);
		if (error == 0) {
			cell->glyph.pending = 1U;
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
			printf("KWL GLASS glyph codepoint=U+%04X size=%u face=emoji cell=%u color=1 pending=1\n", codepoint, (unsigned)size, slot);
			return &cell->glyph;
		}
	}

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

	/* The colours are in the atlas now, and the next frame copies them into the sampled atlas. */
	free(image.pixels);
	kwl_host_image_written(&glass->atlas);

	/* Succeeded: the cell holds the character's colour glyph, a new assignment (a worker's result for an earlier one is not written). */
	glass->cell_generation++;
	cell->generation = glass->cell_generation;
	cell->glyph.pending = 0U;
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

/*
 * Asks the glyph worker for a cell's glyph: the font, the
 * glyph and the size, the box measured here and the cell's assignment.
 * Returns 0, or ENOMEM with nothing asked (the caller renders it itself).
 */
static int
glass_ask_cell(
	struct kwl_glass *glass,
	unsigned font,
	unsigned glyph,
	enum glass_size size,
	unsigned slot,
	const struct truetype_glyph *metrics)
{
	struct kwl_glyph_job *job;

	/* The job, the worker's from its submission. */
	job = calloc(1, sizeof(*job));
	if (job == NULL)
		return ENOMEM;

	/* What to render, and the cell's assignment it is for. */
	job->font = font;
	job->pixels = glass_pixels[size];
	job->glyph = glyph;
	job->width = metrics->width;
	job->height = metrics->height;
	job->kind = GLASS_JOB_CELL;
	job->slot = slot;
	job->generation = glass->cache[slot].generation;

	/* After every glyph asked for before it. */
	kwl_glyph_worker_submit(&glass->worker, job);

	/* Succeeded: the cell waits for the worker. */
	return 0;
}

/*
 * Asks the glyph worker for a cell's colour glyph: the glyph and the size
 * on the emoji font, the box measured here and the cell's assignment.
 * Returns 0, or ENOMEM with nothing asked (the caller decodes it itself).
 */
static int
glass_ask_color(
	struct kwl_glass *glass,
	unsigned glyph,
	enum glass_size size,
	unsigned slot,
	const struct kl_color_image *metrics)
{
	struct kwl_glyph_job *job;

	/* The job, the worker's from its submission. */
	job = calloc(1, sizeof(*job));
	if (job == NULL)
		return ENOMEM;

	/* What to decode, and the cell's assignment it is for. */
	job->task = KWL_GLYPH_TASK_COLOR;
	job->font = (unsigned)glass->emoji_face;
	job->pixels = glass_pixels[size];
	job->glyph = glyph;
	job->width = (uint32_t)metrics->width;
	job->height = (uint32_t)metrics->height;
	job->kind = GLASS_JOB_COLOR;
	job->slot = slot;
	job->generation = glass->cache[slot].generation;

	/* After every job asked for before it. */
	kwl_glyph_worker_submit(&glass->worker, job);

	/* Succeeded: the cell waits for the worker. */
	return 0;
}

/*
 * Writes a decoded colour glyph's pixels into its cell, unless the cell was
 * given to another glyph since it was asked for; one the worker could not
 * decode leaves the cell drawing nothing.
 */
static void
glass_take_color(
	struct kwl_glass *glass,
	const struct kwl_glyph_job *job)
{
	struct glass_cached *cell;
	uint32_t *row;
	int line;

	/* The cell holds another glyph now, or no longer waits. */
	cell = &glass->cache[job->slot];
	if (cell->generation != job->generation || !cell->glyph.pending)
		return;

	/* A glyph that could not be decoded is drawn as nothing. */
	if (job->error != 0) {
		cell->glyph.pending = 0U;
		cell->glyph.width = 0U;
		cell->glyph.height = 0U;
		printf("KWL GLASS glyph codepoint=U+%04X cell=%u color=1 errno=%d\n", cell->codepoint, job->slot, job->error);
		return;
	}

	/* The colours into the cell row by row; the walk's copy takes the atlas. */
	for (line = 0; line < job->image.height; line++) {
		row = (uint32_t *)((unsigned char *)glass->atlas.map + (size_t)(cell->glyph.y + (uint32_t)line) * glass->atlas.row_pitch);
		memcpy(row + cell->glyph.x, job->image.pixels + (size_t)line * (size_t)job->image.width, (size_t)job->image.width * sizeof(uint32_t));
	}

	/* Tells the atlas it was written, and marks the glyph ready. */
	kwl_host_image_written(&glass->atlas);
	cell->glyph.pending = 0U;
}

/*
 * Writes a rendered glyph into its cell, unless the cell was given to
 * another glyph since it was asked for; a glyph the worker could not
 * render leaves the cell drawing nothing.
 */
static void
glass_take_cell(
	struct kwl_glass *glass,
	const struct kwl_glyph_job *job)
{
	struct glass_cached *cell;

	/* The cell holds another glyph now, or no longer waits. */
	cell = &glass->cache[job->slot];
	if (cell->generation != job->generation || !cell->glyph.pending)
		return;

	/* A glyph that could not be rendered is drawn as nothing. */
	if (job->error != 0) {
		cell->glyph.pending = 0U;
		cell->glyph.width = 0U;
		cell->glyph.height = 0U;
		printf("KWL GLASS glyph codepoint=U+%04X cell=%u errno=%d\n", cell->codepoint, job->slot, job->error);
		return;
	}

	/* The coverage into the cell; the walk's copy takes the atlas. */
	atlas_put(glass, job->bitmap, cell->glyph.x, cell->glyph.y, job->metrics.width, job->metrics.height);
	cell->glyph.pending = 0U;
}

/*
 * Asks the glyph worker for the large digits at a size: their
 * boxes are measured here and placed side by side as large_fill places
 * them, and every glyph with a box is asked for.  Returns EAGAIN while
 * they are being rendered (the caller draws an atlas size meanwhile), or
 * the errno value of measuring them.
 */
static int
large_ask(
	struct kwl_glass *glass,
	unsigned pixels)
{
	struct truetype_glyph metrics;
	struct truetype_face *face;
	struct glass_glyph *glyph;
	struct kwl_glyph_job *job;
	uint32_t codepoint;
	uint32_t pen_x;
	unsigned index;
	unsigned id;
	int error;

	/* None usable until all are written; a request of another size makes the earlier one's results stale. */
	glass->large_pixels = 0U;
	glass->large_generation++;
	glass->large_wanted = pixels;
	glass->large_expected = 0U;
	glass->large_received = 0U;
	memset(glass->large_glyphs, 0, sizeof(glass->large_glyphs));

	/* The first font at the size (the cache sets its own size again). */
	face = glass->faces[0];
	error = truetype_set_pixel_size(face, pixels);
	if (error != 0)
		return EINVAL;

	/* Each glyph's box and place from the left, as large_fill makes them. */
	pen_x = 0;
	for (index = 0; index < GLASS_LARGE_GLYPHS; index++) {
		/* Its character: a digit, or the colon last. */
		codepoint = (uint32_t)'0' + index;
		if (index == GLASS_LARGE_COLON)
			codepoint = ':';
		id = truetype_glyph_index(face, codepoint);

		/* Its box, which the bitmap and the image must hold. */
		error = truetype_glyph_metrics(face, id, &metrics);
		if (error != 0)
			return error;
		if (metrics.width > GLASS_LARGE_SIDE || metrics.height > GLASS_LARGE_SIDE)
			return ENOSPC;
		if (metrics.height > GLASS_LARGE_HEIGHT || pen_x + metrics.width > GLASS_LARGE_WIDTH)
			return ENOSPC;

		/* Its place and metrics, drawn once all are written. */
		glyph = &glass->large_glyphs[index];
		glyph->x = pen_x;
		glyph->y = 0;
		glyph->width = metrics.width;
		glyph->height = metrics.height;
		glyph->left = metrics.left;
		glyph->top = metrics.top;
		glyph->advance = metrics.advance;
		pen_x += metrics.width + 1U;

		/* A glyph without a box needs nothing rendered. */
		if (metrics.width == 0U || metrics.height == 0U)
			continue;

		/* The job, the worker's from its submission. */
		job = calloc(1, sizeof(*job));
		if (job == NULL)
			return ENOMEM;
		job->font = 0U;
		job->pixels = pixels;
		job->glyph = id;
		job->width = metrics.width;
		job->height = metrics.height;
		job->kind = GLASS_JOB_LARGE;
		job->slot = index;
		job->generation = glass->large_generation;
		kwl_glyph_worker_submit(&glass->worker, job);
		glass->large_expected++;
	}

	/* Digits without a box are drawn at once (as nothing). */
	if (glass->large_expected == 0U) {
		glass->large_pixels = pixels;
		glass->large_wanted = 0U;
		return 0;
	}

	/* The digits are being rendered. */
	return EAGAIN;
}

/*
 * Writes a rendered large digit into the large image; the first of a size
 * clears the image, and the last makes the digits usable at their size.
 * A digit the worker could not render fails the size, which is not asked
 * for again.
 */
static void
glass_take_large(
	struct kwl_glass *glass,
	const struct kwl_glyph_job *job)
{
	const struct glass_glyph *glyph;

	/* A request of another size since, or one that failed. */
	if (job->generation != glass->large_generation || glass->large_wanted == 0U)
		return;

	/* A digit that could not be rendered fails the size. */
	if (job->error != 0) {
		printf("KWL GLASS large pixels=%u errno=%d\n", glass->large_wanted, job->error);
		glass->large_failed = glass->large_wanted;
		glass->large_wanted = 0U;
		return;
	}

	/* The first digit of the size starts from a transparent image. */
	if (glass->large_received == 0U)
		memset(glass->large.map, 0, glass->large.row_pitch * GLASS_LARGE_HEIGHT);

	/* The digit at its place. */
	glyph = &glass->large_glyphs[job->slot];
	large_put(glass, job->bitmap, glyph->x, job->metrics.width, job->metrics.height);
	glass->large_received++;

	/* The last digit makes them usable. */
	if (glass->large_received < glass->large_expected)
		return;
	glass->large_pixels = glass->large_wanted;
	glass->large_wanted = 0U;
	printf("KWL GLASS large pixels=%u\n", glass->large_pixels);
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
	struct kwl_list *command,
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

	/*
	 * A glyph the worker is still rendering is not drawn this time:
	 * the output is drawn again when it is rendered, and a
	 * layer's picture recorded without it is counted with the guard's
	 * evictions, so that it is drawn again too.
	 */
	if (glyph->pending) {
		if (glass->guard_clock != 0U)
			glass->guarded_evictions++;
		return;
	}

	/* The glyph's pixels: coverage in the colour, or a colour glyph as an image. */
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
 * Works out a glass shape's values that are the same over the whole shape,
 * from its constants as pushed (its final colour and mode),
 * into the texture slot the glass does not sample by: whether it is white
 * (step(0.9, the least of its colour's channels)), whether it is dark glass
 * (step(mode, -0.5)), its sheen from the top (0.05 less as it is flatter,
 * none at shape.z 1) and one over its box's height (at least 1).  The
 * shader once worked each of them out for every pixel.
 */
static void
glass_shape_glass_constants(
	float *constants)
{
	float least;
	float flat;
	float height;

	/* Glass whose colour is white in every channel is lifted over a dark wallpaper. */
	least = constants[12];
	if (constants[13] < least)
		least = constants[13];
	if (constants[14] < least)
		least = constants[14];
	constants[4] = 0.0f;
	if (least >= 0.9f)
		constants[4] = 1.0f;

	/* Dark glass (mode -1) is darkened over a bright wallpaper. */
	constants[5] = 0.0f;
	if (constants[17] <= -0.5f)
		constants[5] = 1.0f;

	/* The sheen from the top, less on flatter glass (shape.z clamped to 0 to 1). */
	flat = constants[18];
	if (flat < 0.0f)
		flat = 0.0f;
	if (flat > 1.0f)
		flat = 1.0f;
	constants[6] = 0.05f * (1.0f - flat);

	/* How far down the box a pixel is, as a share of its height. */
	height = constants[11];
	if (height < 1.0f)
		height = 1.0f;
	constants[7] = 1.0f / height;
}

/*
 * Gives a colour of the light appearance its colour in the dark one:
 * a colour of little saturation (white, the greys, black and
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

/* Gives the kind a shape's draw is counted as (KWL MODES): its mode, the dark glass apart. */
static unsigned
glass_count_kind_of(
	const float *constants)
{
	float mode;

	/* The dark glass is mode -1. */
	mode = constants[17];
	if (mode < 0.0f)
		return KWL_COUNT_DARK_GLASS;

	/* The other modes are whole numbers from 0 to 6. */
	if (mode > 6.5f)
		return KWL_COUNT_KINDS;

	/* Succeeded: the mode itself. */
	return (unsigned)(mode + 0.5f);
}

/* Records a shape over pieces of its quad (x, y, width, height), each showing its part of the shape's image. */
static void
glass_shape_draw_pieces(
	struct kwl_server *server,
	struct kwl_list *command,
	const struct glass_shape *shape,
	const float (*pieces)[4],
	unsigned count)
{
	struct glass_shape piece;
	unsigned index;

	/* Each piece is the shape over a part of its quad, showing that part of the image. */
	for (index = 0U; index < count; index++) {
		glass_shape_piece(shape, pieces[index], &piece);
		glass_shape_draw(server, command, &piece);
	}
}

/* Makes the shape over a part (x, y, width, height) of its quad, showing that part of its image. */
static void
glass_shape_piece(
	const struct glass_shape *shape,
	const float *part,
	struct glass_shape *piece)
{
	float across;
	float down;

	/* The shape, its quad the part, its image's corners moved in step. */
	across = shape->uv[2] - shape->uv[0];
	down = shape->uv[3] - shape->uv[1];
	*piece = *shape;
	memcpy(piece->quad, part, sizeof(piece->quad));
	piece->uv[0] = shape->uv[0] + (part[0] - shape->quad[0]) / shape->quad[2] * across;
	piece->uv[1] = shape->uv[1] + (part[1] - shape->quad[1]) / shape->quad[3] * down;
	piece->uv[2] = shape->uv[0] + (part[0] + part[2] - shape->quad[0]) / shape->quad[2] * across;
	piece->uv[3] = shape->uv[1] + (part[1] + part[3] - shape->quad[1]) / shape->quad[3] * down;
}

/*
 * Draws a shape around the opaque insides of the windows drawn after it
 * (server->cover_holes of a level above server->cover_level, ws270-p007),
 * where it is never seen: a shape wholly inside one is not drawn, a large
 * one they cut into is drawn as the pieces of its quad around them.
 * Returns 1 when that was done (nothing more is drawn of the shape), 0 when
 * the shape is to be drawn as it is.  Only shapes in their places on the
 * output are covered: not while a layer moves them, on a head, or in the
 * backdrop's small passes.
 */
static int
glass_shape_cover(
	struct kwl_server *server,
	struct kwl_list *command,
	const struct glass_shape *shape)
{
	float holes[KWL_COVER_HOLES][4];
	float pieces[KWL_COVER_PIECES][4];
	unsigned hole_count;
	unsigned count;
	unsigned index;

	/* Nothing covers it, or its pieces are being drawn. */
	if (server->cover_count == 0U || server->cover_cutting)
		return 0;

	/* Only in its own place in the output's pass: not moved by a layer, not on a head, not in the backdrop. */
	if (server->layer_on)
		return 0;
	if (server->view_width != 0U)
		return 0;
	if (server->compose->backdrop_recording != 0U)
		return 0;

	/* The holes of the windows above it; one that holds all of the quad hides it. */
	hole_count = 0U;
	for (index = 0U; index < server->cover_count; index++) {
		if (server->cover_levels[index] <= server->cover_level)
			continue;
		if (server->cover_holes[index][0] <= shape->quad[0] &&
		    server->cover_holes[index][1] <= shape->quad[1] &&
		    server->cover_holes[index][0] + server->cover_holes[index][2] >= shape->quad[0] + shape->quad[2] &&
		    server->cover_holes[index][1] + server->cover_holes[index][3] >= shape->quad[1] + shape->quad[3])
			return 1;
		memcpy(holes[hole_count], server->cover_holes[index], sizeof(holes[0]));
		hole_count++;
	}

	/* No hole above it, or a shape too small to be worth cutting: drawn as it is. */
	if (hole_count == 0U || shape->quad[2] * shape->quad[3] < GLASS_COVER_AREA)
		return 0;

	/* The quad less the holes; uncut, it is drawn as it is. */
	count = kwl_damage_rects_subtract(shape->quad, (const float (*)[4])holes, hole_count, pieces);
	if (count == 1U && memcmp(pieces[0], shape->quad, sizeof(pieces[0])) == 0)
		return 0;

	/* Its pieces, drawn without being cut again. */
	server->cover_cutting = 1U;
	glass_shape_draw_pieces(server, command, shape, (const float (*)[4])pieces, count);
	server->cover_cutting = 0U;

	/* Succeeded: drawn around the holes. */
	return 1;
}

/*
 * Cuts a shadow's quad to where it shows: panel.frag's shadow is fully
 * transparent where a pixel is its softness (at least 1) or farther from
 * the box (smoothstep reaches 1 there), so the quad is cut to the box grown
 * by the softness and a pixel more.  The picture is the same.
 */
static void
glass_shadow_trim(
	struct glass_shape *shape)
{
	float reach;
	float left;
	float top;
	float right;
	float bottom;

	/* The box grown by the softness, and a pixel for the rounding. */
	reach = shape->soft;
	if (reach < 1.0f)
		reach = 1.0f;
	reach += 1.0f;
	left = shape->box[0] - reach;
	top = shape->box[1] - reach;
	right = shape->box[0] + shape->box[2] + reach;
	bottom = shape->box[1] + shape->box[3] + reach;

	/* The quad cut to it on each side where it reaches farther. */
	if (left < shape->quad[0])
		left = shape->quad[0];
	if (top < shape->quad[1])
		top = shape->quad[1];
	if (right > shape->quad[0] + shape->quad[2])
		right = shape->quad[0] + shape->quad[2];
	if (bottom > shape->quad[1] + shape->quad[3])
		bottom = shape->quad[1] + shape->quad[3];

	/* Nothing left: an empty quad, which is not drawn. */
	if (right < left)
		right = left;
	if (bottom < top)
		bottom = top;
	shape->quad[0] = left;
	shape->quad[1] = top;
	shape->quad[2] = right - left;
	shape->quad[3] = bottom - top;
}

/*
 * Gives a soft shadow (trimmed to its reach, in its place on the output)
 * as the image of its picture (glass-map.c) over the same quad, when it has
 * one: the box is the quad grown by a pixel (every pixel of the quad fully
 * covered, none rounded), and the quad's corners show the picture's pixels
 * under them.  Not in a backdrop's small passes or on a head.  Returns 1
 * with *picture, or 0 (the shadow is drawn as itself).
 */
static int
glass_shadow_picture(
	struct kwl_server *server,
	const struct glass_shape *shadow,
	struct glass_shape *picture)
{
	VkDescriptorSet set;
	float origin[2];
	float size[2];
	int pictured;

	/* On the output's own plane, in the frame's own pass, with something to draw. */
	if (server->view_width != 0U || server->compose->backdrop_recording != 0U)
		return 0;
	if (shadow->quad[2] <= 0.0f || shadow->quad[3] <= 0.0f)
		return 0;

	/* The shadow's picture, when it is drawn. */
	pictured = kwl_glass_shadow_set(server, shadow->box, shadow->radius, shadow->soft, shadow->color, origin, size, &set);
	if (!pictured)
		return 0;

	/* The picture's part under the quad, as an image of premultiplied colour. */
	*picture = *shadow;
	picture->mode = MODE_IMAGE;
	picture->set = set;
	picture->radius = 0.0f;
	picture->soft = 0.0f;
	picture->opaque = 0.0f;
	picture->edge = 0.0f;
	picture->box[0] = shadow->quad[0] - 1.0f;
	picture->box[1] = shadow->quad[1] - 1.0f;
	picture->box[2] = shadow->quad[2] + 2.0f;
	picture->box[3] = shadow->quad[3] + 2.0f;
	picture->uv[0] = (shadow->quad[0] - origin[0]) / size[0];
	picture->uv[1] = (shadow->quad[1] - origin[1]) / size[1];
	picture->uv[2] = (shadow->quad[0] + shadow->quad[2] - origin[0]) / size[0];
	picture->uv[3] = (shadow->quad[1] + shadow->quad[3] - origin[1]) / size[1];

	/* Succeeded: the shadow is drawn as its picture. */
	return 1;
}

/*
 * Draws an outline (MODE_RING) around its middle, where the shader's two
 * coverages are both whole and cancel (the distance at most the thickness
 * and a pixel less: the box less its radius, the thickness and a pixel),
 * when that middle is large: as the pieces of its quad around it.  Only in
 * its place on the output's own plane.  Returns 1 when it was drawn so, 0
 * when it is to be drawn whole.
 */
static int
glass_ring_hollow(
	struct kwl_server *server,
	struct kwl_list *command,
	const struct glass_shape *shape)
{
	float hole[1][4];
	float left;
	float top;
	float right;
	float bottom;

	/* An outline in its place on the output's plane. */
	if (shape->mode != MODE_RING || server->layer_on || server->view_width != 0U)
		return 0;

	/* Its empty middle. */
	glass_opaque_inside(shape->box, shape->radius + shape->soft, hole[0]);

	/* Worth cutting only where it lies within the quad over a large area. */
	left = hole[0][0] > shape->quad[0] ? hole[0][0] : shape->quad[0];
	top = hole[0][1] > shape->quad[1] ? hole[0][1] : shape->quad[1];
	right = hole[0][0] + hole[0][2] < shape->quad[0] + shape->quad[2] ? hole[0][0] + hole[0][2] : shape->quad[0] + shape->quad[2];
	bottom = hole[0][1] + hole[0][3] < shape->quad[1] + shape->quad[3] ? hole[0][1] + hole[0][3] : shape->quad[1] + shape->quad[3];
	if (right <= left || bottom <= top || (right - left) * (bottom - top) < GLASS_SPLIT_AREA)
		return 0;

	/* Succeeded: the quad's pieces around the middle (each lies off it, so is drawn whole). */
	glass_shape_draw_except(server, command, shape, (const float (*)[4])hole, 1U);
	return 1;
}

/*
 * Draws two whole-output pictures side by side into an image twice the
 * output's width (the pass's viewport), left and right, each a texel for a
 * pixel: the picture glass_draw_crossfade fades between.
 */
void
glass_draw_halves(
	struct kwl_server *server,
	struct kwl_list *command,
	VkDescriptorSet left,
	VkDescriptorSet right)
{
	struct kwl_compose *compose;
	float constants[KWL_PANEL_CONSTANTS];
	VkPipeline pipeline;

	/* An opaque image over each half; its box is the whole image, so every pixel is covered. */
	compose = server->compose;
	memset(constants, 0, sizeof(constants));
	constants[0] = -1.0f;
	constants[1] = -1.0f;
	constants[2] = 0.0f;
	constants[3] = 1.0f;
	constants[6] = 1.0f;
	constants[7] = 1.0f;
	constants[10] = 2.0f * (float)server->width;
	constants[11] = (float)server->height;
	constants[15] = 1.0f;
	constants[17] = MODE_IMAGE;
	constants[19] = 1.0f;
	constants[20] = 2.0f * (float)server->width;
	constants[21] = (float)server->height;
	constants[23] = 1.0f;
	pipeline = compose->panel_opaque_pipelines[KWL_PANEL_INSIDE_IMAGE];

	/* The left half. */
	kwl_compose_bind(server, command, pipeline, compose->panel_layout, left);
	kwl_list_push_constants(command, compose->panel_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0U, sizeof(constants), constants);
	kwl_list_draw(command, KWL_QUAD_VERTICES, 1U, 0U, 0U);

	/* The right half. */
	constants[0] = 0.0f;
	constants[2] = 1.0f;
	kwl_compose_bind(server, command, pipeline, compose->panel_layout, right);
	kwl_list_push_constants(command, compose->panel_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0U, sizeof(constants), constants);
	kwl_list_draw(command, KWL_QUAD_VERTICES, 1U, 0U, 0U);
	kwl_compose_count_draw(server, pipeline, right, 2U);
}

/*
 * Draws over the whole output, opaque, the left half of a picture of two
 * (glass_draw_halves) with its right half faded in over it by mix: the
 * same as the left drawn and the right blended over it at that opacity,
 * in one draw.
 */
void
glass_draw_crossfade(
	struct kwl_server *server,
	struct kwl_list *command,
	VkDescriptorSet set,
	float mix)
{
	struct kwl_compose *compose;
	float constants[KWL_PANEL_CONSTANTS];
	float width;
	float height;
	VkPipeline pipeline;
	uint64_t pixels;
	int visible;

	/* Not seen where the frame's damage leaves nothing. */
	compose = server->compose;
	width = (float)server->width;
	height = (float)server->height;
	visible = kwl_compose_clip_visible(compose, 0.0f, 0.0f, width, height);
	if (!visible)
		return;

	/* The whole output, sampling the left half; the right half is half the image along. */
	memset(constants, 0, sizeof(constants));
	constants[0] = -1.0f;
	constants[1] = -1.0f;
	constants[2] = 1.0f;
	constants[3] = 1.0f;
	constants[6] = 0.5f;
	constants[7] = 1.0f;
	constants[10] = width;
	constants[11] = height;
	constants[15] = mix;
	constants[17] = MODE_IMAGE;
	constants[19] = 1.0f;
	constants[20] = width;
	constants[21] = height;
	constants[23] = 1.0f;
	pipeline = compose->panel_opaque_pipelines[KWL_PANEL_INSIDE_CROSSFADE];

	/* One opaque quad, counted as an image (KWL MODES). */
	kwl_compose_count_draw(server, pipeline, set, 1U);
	pixels = kwl_compose_count_pixels(server, 0.0f, 0.0f, width, height, 0);
	kwl_compose_count_kind(server, glass_count_kind_of(constants), pixels);
	kwl_compose_bind(server, command, pipeline, compose->panel_layout, set);
	kwl_list_push_constants(command, compose->panel_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0U, sizeof(constants), constants);
	kwl_compose_clip_draw(server, command, 0.0f, 0.0f, width, height, KWL_QUAD_VERTICES);
	kwl_compose_count_commands(server, 1U, 1U);
}
