/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The layers' pictures (ws211-p002, plan/ws211/phase001/phase.md, design
 * version 4): App Home's content and the desktop layer, each drawn into an
 * image of the output's size, so that App Home's opening and closing show
 * them as two pictures instead of drawing every shape of both in each frame
 * (on the Raspberry Pi 4 a frame of both is 460 to 900 draws).
 *
 * Both images are made with the output, in its format, so that the glass
 * look's pipelines draw into them as into the output; a device that cannot
 * make both keeps neither (KWL LAYER unavailable) and the compositor draws
 * as before.
 *
 * The desktop's picture is drawn in the first frame of each opening and
 * closing, in the frame's own commands before its pass (in-band): that
 * frame draws the desktop once, as every frame did before, and the frames
 * after it show the picture.  It shows the windows as they were then (the
 * change the user accepted on 2026-10-12); a window mapped later is drawn
 * over it, and a window in it that an animation starts to move takes the
 * frames back to drawing everything.
 *
 * Home's picture holds its content without the stage and without the clock
 * (drawn over it each frame), on transparent black, as Home opens (its
 * page's first application selected, no icon lit); its key
 * (kwl_home_picture_key) says what it shows, and is made again only when
 * something it shows may have changed (home_picture_stale).  A stale
 * picture is drawn again in-band in an animation's frame, or, while Home is
 * closed, as a frame of its own that presents nothing (the bake of its slot, compose.c)
 * when there has been no input and no window commit for LAYER_QUIET_MS, at
 * most twice a second.
 *
 * A glyph a picture uses that the atlas evicted before the picture's
 * drawing completed (a text measured outside a frame) may have reached the
 * picture: the atlas counts such evictions (its guard), and the picture is
 * drawn again (review-3 A1, review-4 H2).  A
 * drawing that completes after the output was opened again is not taken
 * (the generation, review-3 A3).
 *
 * A window docking or coming back (shell.c's dock animation, ws211-p007,
 * the 2026-10-12 user's request) is drawn the same way: the first frame of
 * its animation draws the desktop's picture without it and the window's
 * own picture (its body, unshadowed, where it starts), and the frames show
 * the desktop's picture and the window's scaled to where it is, instead of
 * every window and every glass panel.  Its client is told its new size only
 * when the animation ends (shell.c), so that it does not draw meanwhile.
 */

#include "compose.h"
#include "glass.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* How long the compositor has been without input and without a window commit before Home's picture is drawn on its own. */
#define LAYER_QUIET_MS		500U

/* How long since the last frame completed before it is. */
#define LAYER_FRAME_QUIET_MS	100U

/* The fewest milliseconds between the first and the third drawing on its own: two a second at most. */
#define LAYER_TURN_MS		1000U

/* The generations of the outputs' pictures (one each time an output opens). */
static uint64_t layers_generations;

static VkResult layers_pass(struct kwl_compose *compose);
static VkResult layers_image(struct kwl_compose *compose, struct kwl_layer_image *target);
static void layers_image_destroy(struct kwl_compose *compose, struct kwl_layer_image *target);
static void layers_destroy(struct kwl_compose *compose);
static void layers_record_pass(struct kwl_server *server, struct kwl_list *command, unsigned which, struct kwl_object **windows, unsigned count);
static int layers_home_current(struct kwl_server *server);
static int layers_animating(struct kwl_server *server);
static void layers_log_use(struct kwl_layers *layers);
static const char *layers_name(unsigned which);

/*
 * Makes the layers' pictures for the output just opened: the pass and every
 * image, or those of the output before when they are of its size (kept by
 * the output's close; their memory is slow to get on some devices), else
 * those go first.  Without all of them the pictures are unavailable for
 * the output's life.  Nothing drawn before is taken as drawn.
 */
void
kwl_layers_open(
	struct kwl_server *server)
{
	struct kwl_layer_image images[KWL_LAYERS];
	struct kwl_compose *compose;
	struct kwl_layers *layers;
	VkRenderPass pass;
	VkResult result;
	unsigned index;
	unsigned kept;

	/* The pictures kept, when of the size drawn at and wanted; others go. */
	compose = server->compose;
	layers = &compose->layers;
	kept = 0U;
	if (layers->ready && !server->layer_cache_off &&
	    layers->width == compose->target_width && layers->height == compose->target_height) {
		pass = layers->pass;
		memcpy(images, layers->images, sizeof(images));
		kept = 1U;
	} else {
		kwl_layers_close(compose);
	}

	/* Nothing is drawn yet; the size is the output's, the generation a new one. */
	memset(layers, 0, sizeof(*layers));
	layers->width = compose->target_width;
	layers->height = compose->target_height;
	layers_generations++;
	layers->generation = layers_generations;
	server->home_picture_stale = 1U;

	/* None when the compositor was asked to draw everything each frame (--no-layer-cache, the reference). */
	if (server->layer_cache_off) {
		layers->failed = 1U;
		printf("KWL LAYER unavailable reason=off\n");
		return;
	}

	/* The pictures kept are ready again. */
	if (kept) {
		layers->pass = pass;
		memcpy(layers->images, images, sizeof(images));
		layers->ready = 1U;
		printf("KWL LAYER ready width=%u height=%u images=%u kept=1\n", layers->width, layers->height, KWL_LAYERS);
		return;
	}

	/* The pass, then each image. */
	result = layers_pass(compose);
	for (index = 0U; index < KWL_LAYERS && result == VK_SUCCESS; index++)
		result = layers_image(compose, &layers->images[index]);

	/* All or none. */
	if (result != VK_SUCCESS) {
		layers_destroy(compose);
		layers->failed = 1U;
		printf("KWL LAYER unavailable result=%d\n", (int)result);
		return;
	}

	/* Succeeded: the pictures can be drawn. */
	layers->ready = 1U;
	printf("KWL LAYER ready width=%u height=%u images=%u\n", layers->width, layers->height, KWL_LAYERS);
}

/*
 * Destroys the layers' pictures.
 *
 * Nothing in flight draws or reads them; a drawing still to complete is
 * not taken.
 */
void
kwl_layers_close(
	struct kwl_compose *compose)
{
	/* Whatever was made. */
	layers_destroy(compose);
	memset(&compose->layers, 0, sizeof(compose->layers));
}

/*
 * Draws Home's stale picture as a frame of its own that presents nothing,
 * when App Home is closed and the compositor has been quiet (the event
 * loop calls it when it has nothing to draw).  An animation that ended
 * says how its frames were drawn first.  Returns 1 when the drawing went
 * in flight, 0 when none was due, and an errno value's negation when it
 * could not be submitted.
 */
int
kwl_layers_tick(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	struct kwl_layers *layers;
	VkResult result;
	uint64_t now;
	uint64_t key;
	int ready;
	int home_due;
	int maps_due;
	int busy;
	uint64_t quiet;

	/* Only with the pictures, the output, no frame in flight, in the glass look, with nothing else on the screen. */
	compose = server->compose;
	if (compose == NULL || !compose->output_open || compose->output_lost || compose->in_flight)
		return 0;

	/* Nor a task: the picture's drawing writes host images' staging, which a move to its layout may still be reading. */
	busy = kwl_compose_tasks_busy(compose);
	if (busy)
		return 0;
	layers = &compose->layers;
	if (!layers->ready)
		return 0;
	if (!server->home_moving)
		layers_log_use(layers);
	if (!server->glass || server->greeter || server->locked || server->screen_off)
		return 0;

	/* Only with Home closed and still. */
	if (kwl_home_progress(server) > 0.0f || server->home_moving || server->home_dragging)
		return 0;

	/*
	 * Only when what Home shows may have changed, or when a glass map or
	 * picture waits (those App Home's and Wiseview's first frames would
	 * draw are asked for now, glass-map.c).
	 */
	home_due = 1;
	if (!server->home_picture_stale && layers_home_current(server))
		home_due = 0;
	if (!home_due) {
		kwl_glass_prewarm(server);
		maps_due = kwl_glass_maps_wanted(server);
		if (!maps_due)
			return 0;
	}

	/*
	 * Only in the quiet: no input and no window commit for a while, no
	 * frame just done, two drawings a second at most.  Before the quiet the
	 * loop comes back when it begins (a frame wanted draws first).
	 */
	now = kwl_milliseconds();
	if (server->dirty || server->damaged)
		return 0;
	quiet = server->input_ms + LAYER_QUIET_MS;
	if (server->commit_ms + LAYER_QUIET_MS > quiet)
		quiet = server->commit_ms + LAYER_QUIET_MS;
	if (server->frame_done_ms + LAYER_FRAME_QUIET_MS > quiet)
		quiet = server->frame_done_ms + LAYER_FRAME_QUIET_MS;
	if (layers->bake_times[0] != 0U && layers->bake_times[0] + LAYER_TURN_MS > quiet)
		quiet = layers->bake_times[0] + LAYER_TURN_MS;
	if (now < quiet) {
		kwl_due(server, quiet);
		return 0;
	}

	/* The key made again: a picture that shows it already is kept, and then only the maps are drawn. */
	if (home_due && layers_home_current(server))
		home_due = 0;
	if (!home_due) {
		maps_due = kwl_glass_maps_wanted(server);
		if (!maps_due)
			return 0;
	}

	/* The render thread is told a picture may be drawn; the walk waits for its ready (ws213-p004). */
	ready = kwl_compose_want_layer(server);
	if (!ready)
		return 0;
	key = kwl_home_picture_key(server);

	/* Drawn into the list (the glass maps waiting first) and given as a frame that presents nothing. */
	kwl_compose_list_begin(compose);
	kwl_glass_maps_inband(server, &compose->list);
	if (home_due)
		layers_record_pass(server, &compose->list, KWL_LAYER_HOME, NULL, 0U);
	result = kwl_compose_submit_quiet(server);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=layer result=%d\n", (int)result);
		return -EIO;
	}

	/* In flight: the picture is taken when it completes, if no glyph it uses was evicted meanwhile. */
	layers->bake_times[0] = layers->bake_times[1];
	layers->bake_times[1] = now;
	if (!home_due)
		return 1;
	layers->keys[KWL_LAYER_HOME] = key;
	printf("KWL LAYER baked what=home inband=0 draws=%u us=%llu at_us=%llu\n", layers->bake_draws, (unsigned long long)layers->bake_us, (unsigned long long)kwl_microseconds());
	return 1;
}

/*
 * Draws, in the frame being recorded and before its pass, the pictures an
 * animation of App Home needs (review-3, Q1's choice).  What the animation
 * shows is decided once, at its first frame (review-4 H1): the desktop's
 * picture is drawn then, with the windows that frame draws; Home's picture
 * then too when it does not show what is there, except when Home opens, as
 * its content is not seen in the first frame: it is drawn in the second
 * (review-4 H3).  Nothing is drawn again later in the animation; a running
 * mark that changes meanwhile shows at the end.  The frame's hold covers
 * the windows' images the desktop's picture samples.
 */
void
kwl_layers_inband(
	struct kwl_server *server,
	struct kwl_list *command,
	struct kwl_object **windows,
	unsigned count)
{
	struct kwl_layers *layers;
	uint64_t key;

	/* Only Home's own animation, with the pictures. */
	layers = &server->compose->layers;
	if (!layers->ready || !layers_animating(server))
		return;

	/* The second frame of an opening draws the Home picture it lacked. */
	if (layers->decided_anim == server->home_start_ms) {
		if (layers->using && layers->home_later) {
			key = kwl_home_picture_key(server);
			layers_record_pass(server, command, KWL_LAYER_HOME, NULL, 0U);
			layers->keys[KWL_LAYER_HOME] = key;
			layers->home_later = 0U;
			printf("KWL LAYER baked what=home inband=1 draws=%u us=%llu at_us=%llu\n", layers->bake_draws, (unsigned long long)layers->bake_us, (unsigned long long)kwl_microseconds());
		}
		return;
	}

	/* The first frame decides: the pictures are used. */
	layers->decided_anim = server->home_start_ms;
	layers->using = 1U;
	layers->home_later = 0U;

	/* The desktop's picture, with the windows the frame draws. */
	layers_record_pass(server, command, KWL_LAYER_DESKTOP, windows, count);
	printf("KWL LAYER baked what=desktop inband=1 draws=%u windows=%u us=%llu at_us=%llu\n", layers->bake_draws, layers->window_count, (unsigned long long)layers->bake_us, (unsigned long long)kwl_microseconds());

	/* Home's picture when it does not show what is there: now when Home closes, in the next frame when it opens. */
	if (layers_home_current(server))
		return;
	if (server->home_to > server->home_from) {
		layers->home_later = 1U;
		return;
	}
	key = kwl_home_picture_key(server);
	layers_record_pass(server, command, KWL_LAYER_HOME, NULL, 0U);
	layers->keys[KWL_LAYER_HOME] = key;
	printf("KWL LAYER baked what=home inband=1 draws=%u us=%llu at_us=%llu\n", layers->bake_draws, (unsigned long long)layers->bake_us, (unsigned long long)kwl_microseconds());
}

/*
 * Tells whether this frame of App Home's animation shows the pictures
 * (shell.c): Home opening or closing on its own, not followed by a finger,
 * without a search or a page turning, with the pictures its first frame
 * decided on.  A window of the desktop's picture that an animation starts
 * to move, or a picture dropped, takes the rest of the animation back to
 * drawing everything (review-4 M3).  The frames are counted for the log
 * (KWL LAYER use).
 */
int
kwl_layers_showing(
	struct kwl_server *server)
{
	struct kwl_layers *layers;
	int pictured;
	int shown;

	/* Only Home's own animation, with the pictures decided for it. */
	layers = &server->compose->layers;
	shown = 0;
	if (layers->ready && layers_animating(server) && layers->decided_anim == server->home_start_ms && layers->using)
		shown = 1;

	/* Both pictures drawn (Home's may come in the second frame), and no window of the picture moving. */
	if (shown && (!layers->valid[KWL_LAYER_DESKTOP] || (!layers->valid[KWL_LAYER_HOME] && !layers->home_later)))
		shown = 0;
	if (shown && server->anim != NULL) {
		pictured = kwl_layers_pictured(server, server->anim);
		if (pictured)
			shown = 0;
	}

	/* Once back to drawing everything, the animation stays so. */
	if (!shown && layers->decided_anim == server->home_start_ms)
		layers->using = 0U;

	/* The frames of an animation, counted. */
	if (layers_animating(server)) {
		if (shown)
			layers->use_pictures++;
		else
			layers->use_drawn++;
	}

	/* Succeeded: whether the frame shows the pictures. */
	return shown;
}

/* Tells whether this frame shows the pictures without Home's (an opening's first frame, where its content is not yet seen). */
int
kwl_layers_home_hidden(
	struct kwl_server *server)
{
	/* Home's picture comes in the next frame. */
	return server->compose->layers.home_later != 0U;
}

/*
 * Notes a window the desktop's picture being drawn draws (shell.c's
 * draw_desktop_layer): the windows mapped since, and those it left out,
 * are drawn over it.  By its map order, which no later mapping reuses
 * (review-4 M3).
 */
void
kwl_layers_note(
	struct kwl_server *server,
	const struct kwl_object *surface)
{
	struct kwl_layers *layers;

	/* Only while the picture is drawn, while there is room. */
	layers = &server->compose->layers;
	if (!layers->noting || layers->window_count >= KWL_FRAME_WINDOWS)
		return;
	layers->window_orders[layers->window_count] = surface->map_order;
	layers->window_count++;
}

/* Tells whether a window is in the desktop's picture (the windows mapped since, or left out, are drawn over it). */
int
kwl_layers_pictured(
	struct kwl_server *server,
	const struct kwl_object *surface)
{
	struct kwl_layers *layers;
	unsigned index;

	/* By its map order. */
	layers = &server->compose->layers;
	for (index = 0U; index < layers->window_count; index++) {
		if (layers->window_orders[index] == surface->map_order)
			return 1;
	}

	/* Not in it. */
	return 0;
}

/* Gives a picture's descriptor set, to sample it (KWL_LAYER_HOME or KWL_LAYER_DESKTOP). */
VkDescriptorSet
kwl_layers_set(
	struct kwl_server *server,
	unsigned which)
{
	/* The picture's set. */
	return server->compose->layers.images[which].set;
}

/*
 * Gives the box (left, top, right, bottom, in the picture's pixels) of
 * what Home's picture holds; outside it the picture is clear.  Returns 0
 * when it is not known (the whole picture is then drawn).
 */
int
kwl_layers_home_bounds(
	struct kwl_server *server,
	float *bounds)
{
	struct kwl_layers *layers;

	/* Known once the picture was drawn with it. */
	layers = &server->compose->layers;
	if (!layers->home_bounded)
		return 0;

	/* Succeeded: the box. */
	bounds[0] = layers->home_bounds[0];
	bounds[1] = layers->home_bounds[1];
	bounds[2] = layers->home_bounds[2];
	bounds[3] = layers->home_bounds[3];
	return 1;
}

/*
 * Draws, in the frame being recorded and before its pass, the pictures the
 * animation of a docking or returning window shows (ws211-p007): decided
 * once, at its first frame, when shell.c says its frames may show them.
 * The desktop's picture is drawn without the window, and the window's
 * picture with its body where it starts.  The frame's hold covers the
 * windows' images both sample; the window's picture shows them as they were
 * at the start for the rest of the animation.
 */
void
kwl_layers_anim_inband(
	struct kwl_server *server,
	struct kwl_list *command,
	struct kwl_object **windows,
	unsigned count)
{
	struct kwl_layers *layers;
	uint64_t desktop_us;
	unsigned desktop_draws;
	int allowed;

	/* Only a window's animation, with the pictures of the output's present size. */
	layers = &server->compose->layers;
	if (!layers->ready || server->anim == NULL)
		return;
	if (layers->width != server->width || layers->height != server->height)
		return;

	/* Decided once, at its first frame; not used unless drawn now. */
	if (layers->anim_decided == server->anim_serial)
		return;
	layers->anim_decided = server->anim_serial;
	layers->anim_using = 0U;

	/* Only an animation whose frames may show them (shell.c). */
	allowed = kwl_glass_anim_picturable(server, windows, count);
	if (!allowed) {
		printf("KWL LAYER anim pictures=0 surface=%u\n", server->anim->id);
		return;
	}

	/* The desktop without the window, then the window where it starts. */
	layers_record_pass(server, command, KWL_LAYER_DESKTOP, windows, count);
	desktop_draws = layers->bake_draws;
	desktop_us = layers->bake_us;
	layers_record_pass(server, command, KWL_LAYER_WINDOW, NULL, 0U);
	memcpy(layers->anim_rect, server->anim_from, sizeof(layers->anim_rect));
	layers->anim_using = 1U;
	printf("KWL LAYER baked what=anim inband=1 surface=%u desktop_draws=%u windows=%u window_draws=%u us=%llu at_us=%llu\n",
	       server->anim->id,
	       desktop_draws,
	       layers->window_count,
	       layers->bake_draws,
	       (unsigned long long)(desktop_us + layers->bake_us),
	       (unsigned long long)kwl_microseconds());
}

/*
 * Tells whether this frame of a window's dock animation shows the pictures
 * its first frame drew (shell.c): both still drawn, and the frame still
 * one that may show them (Home opening, a slide, a drag and the like take
 * the rest of the animation back to drawing everything).  The frames are
 * counted for the log (KWL LAYER use).
 */
int
kwl_layers_anim_showing(
	struct kwl_server *server,
	struct kwl_object **windows,
	unsigned count)
{
	struct kwl_layers *layers;
	int allowed;
	int decided;
	int shown;

	/* Only the animation its first frame decided on, with the pictures. */
	layers = &server->compose->layers;
	decided = 0;
	if (layers->ready &&
	    server->anim != NULL &&
	    layers->anim_decided == server->anim_serial)
		decided = 1;
	shown = 0;
	if (decided && layers->anim_using)
		shown = 1;

	/* Both pictures still drawn, and the frame still one that may show them. */
	if (shown &&
	    (!layers->valid[KWL_LAYER_DESKTOP] ||
	     !layers->valid[KWL_LAYER_WINDOW]))
		shown = 0;
	if (shown) {
		allowed = kwl_glass_anim_picturable(server, windows, count);
		if (!allowed)
			shown = 0;
	}

	/* Once back to drawing everything, the animation stays so; its frames are counted. */
	if (decided && !shown)
		layers->anim_using = 0U;
	if (decided && shown)
		layers->use_pictures++;
	else if (decided)
		layers->use_drawn++;

	/* Succeeded: whether the frame shows the pictures. */
	return shown;
}

/*
 * Gives the window's picture of a dock animation to sample, with the part
 * of it its body was drawn in (uv: left, top, right, bottom, from 0 to 1).
 */
VkDescriptorSet
kwl_layers_anim_picture(
	struct kwl_server *server,
	float uv[4])
{
	struct kwl_layers *layers;

	/* The rectangle the body was drawn in, as a part of the picture. */
	layers = &server->compose->layers;
	uv[0] = (float)layers->anim_rect[0] / (float)layers->width;
	uv[1] = (float)layers->anim_rect[1] / (float)layers->height;
	uv[2] = (float)(layers->anim_rect[0] + layers->anim_rect[2]) / (float)layers->width;
	uv[3] = (float)(layers->anim_rect[1] + layers->anim_rect[3]) / (float)layers->height;

	/* Succeeded: the window's picture. */
	return layers->images[KWL_LAYER_WINDOW].set;
}

/*
 * Ends a submission that drew pictures (a frame, or one that presents
 * nothing): they are taken unless the output was opened again since, or a
 * glyph one of them uses was evicted from the atlas while it waited
 * (review-3 A1, A3; review-4 H2).
 */
void
kwl_layers_complete(
	struct kwl_server *server,
	uint64_t generation)
{
	struct kwl_layers *layers;
	uint32_t evictions;
	unsigned which;

	/* Nothing of another output's pictures. */
	layers = &server->compose->layers;
	if (!layers->ready || generation != layers->generation)
		return;
	if (layers->inflight == 0U)
		return;

	/* Each picture this submission drew, taken or drawn again later; the atlas no longer guards their glyphs. */
	evictions = glass_glyph_guard_end(server);
	for (which = 0U; which < KWL_LAYERS; which++) {
		if ((layers->inflight & (1U << which)) == 0U)
			continue;
		if (evictions == 0U)
			continue;
		layers->valid[which] = 0U;
		server->home_picture_stale = 1U;
		printf("KWL LAYER dropped what=%s reason=eviction evictions=%u\n", layers_name(which), evictions);
	}
	layers->inflight = 0U;
}

/* Tells whether App Home's own animation runs: opening or closing, not followed by a finger, without a search or a page turning. */
static int
layers_animating(
	struct kwl_server *server)
{
	/* Moving on its own, its first frame recorded, its pages still. */
	if (!server->home_moving || server->home_dragging || server->home_start_pending || server->home_page_moving)
		return 0;

	/* No search typed or being composed. */
	if (server->home_query_length != 0U || server->home_preedit[0] != '\0')
		return 0;

	/* Succeeded. */
	return 1;
}

/*
 * Tells whether Home's picture shows what Home would show: drawn, from the
 * wallpaper and the tiles as they are, at the page, the accent and the
 * output's size it was drawn at (compared each time, review-4 M2), and with
 * its key made again when something else it shows may have changed.
 */
static int
layers_home_current(
	struct kwl_server *server)
{
	struct kwl_layers *layers;
	uint64_t key;

	/* Drawn, from the wallpaper, the tiles, the page, the accent and the size as they are. */
	layers = &server->compose->layers;
	if (!layers->valid[KWL_LAYER_HOME])
		return 0;
	if (layers->home_wallpaper != glass_wallpaper_version(server) ||
	    layers->home_tiles != glass_tiles_ready(server) ||
	    layers->home_page != server->home_page ||
	    layers->home_accent != server->accent ||
	    layers->home_width != server->width ||
	    layers->home_height != server->height)
		return 0;

	/* Nothing else it shows changed since its key was made. */
	if (!server->home_picture_stale)
		return 1;

	/* The key made again. */
	server->home_picture_stale = 0U;
	key = kwl_home_picture_key(server);
	return key == layers->keys[KWL_LAYER_HOME];
}

/* Logs how the frames of the animation that ended were drawn, once (KWL LAYER use). */
static void
layers_log_use(
	struct kwl_layers *layers)
{
	/* Nothing counted since the last. */
	if (layers->use_pictures == 0U && layers->use_drawn == 0U)
		return;
	printf("KWL LAYER use pictures=%u drawn=%u\n", layers->use_pictures, layers->use_drawn);
	layers->use_pictures = 0U;
	layers->use_drawn = 0U;
}

/*
 * Writes a picture into the display list, outside any pass: the layers'
 * pass on its image, cleared to transparent black, and Home's content drawn
 * as it opens, without placing it, in its own colours, with no icon lit and
 * without the clock; or the desktop layer as Home has it, in the
 * appearance's colours.  Every state the glass look's drawing reads is set
 * for the pass and given back (review-2 N5).  The submission that carries
 * it takes the picture when it completes (kwl_layers_complete).
 */
static void
layers_record_pass(
	struct kwl_server *server,
	struct kwl_list *command,
	unsigned which,
	struct kwl_object **windows,
	unsigned count)
{
	struct kwl_compose *compose;
	struct kwl_layers *layers;
	struct kwl_draw_counts counts;
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	uint64_t started;
	VkViewport viewport;
	VkRect2D scissor;
	VkRect2D scissor_saved;
	VkFramebuffer framebuffer_saved;
	VkDescriptorSet backdrop_saved;
	VkDeviceSize offset;
	unsigned recording_saved;
	unsigned keep_saved;
	unsigned section_saved;
	unsigned backdrop_recording_saved;
	unsigned layer_saved;
	int32_t view_x_saved;
	int32_t view_y_saved;
	uint32_t view_width_saved;
	uint32_t view_height_saved;

	/* The pass on the picture's image, cleared to transparent black; the atlas guards the glyphs it uses until it completes. */
	compose = server->compose;
	layers = &compose->layers;
	started = kwl_microseconds();
	glass_glyph_guard_begin(server);
	memset(&clear, 0, sizeof(clear));
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = layers->pass;
	pass.framebuffer = layers->images[which].framebuffer;
	pass.renderArea.extent.width = layers->width;
	pass.renderArea.extent.height = layers->height;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	kwl_list_begin_render_pass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
	kwl_compose_bind_forget(server->compose);

	/* The pass's own viewport, scissor and quad corners. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)layers->width;
	viewport.height = (float)layers->height;
	viewport.maxDepth = 1.0f;
	kwl_list_set_viewport(command, 0U, 1U, &viewport);
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = layers->width;
	scissor.extent.height = layers->height;
	kwl_list_set_scissor(command, 0U, 1U, &scissor);
	offset = 0U;
	kwl_list_bind_vertex_buffers(command, 0U, 1U, &compose->corners, &offset);

	/* The state the drawing reads, kept to be given back. */
	scissor_saved = compose->scissor_now;
	framebuffer_saved = compose->framebuffer_now;
	backdrop_saved = compose->backdrop_set;
	backdrop_recording_saved = compose->backdrop_recording;
	recording_saved = server->hover_recording;
	keep_saved = server->keep_colours;
	section_saved = server->draw_section;
	layer_saved = server->layer_on;
	view_x_saved = server->view_x;
	view_y_saved = server->view_y;
	view_width_saved = server->view_width;
	view_height_saved = server->view_height;
	counts = server->draw_counts;

	/* The whole image, unplaced, without the backdrop or hover tests; Home in its own colours. */
	compose->scissor_now = scissor;
	compose->framebuffer_now = VK_NULL_HANDLE;
	compose->backdrop_set = VK_NULL_HANDLE;
	compose->backdrop_recording = 0U;
	server->hover_recording = 0U;
	server->keep_colours = which == KWL_LAYER_HOME ? 1U : 0U;
	server->draw_section = which == KWL_LAYER_HOME ? KWL_SECTION_HOME : KWL_SECTION_LAYER;
	server->layer_on = 0U;
	server->view_x = 0;
	server->view_y = 0;
	server->view_width = 0U;
	server->view_height = 0U;
	memset(&server->draw_counts, 0, sizeof(server->draw_counts));

	/* Home's content as it opens, no icon lit, no clock, and the box of it; the desktop layer, noting the windows it draws; or the animated window. */
	if (which == KWL_LAYER_HOME) {
		kwl_compose_bounds_begin(compose);
		kwl_home_draw_content(server, command, 1.0f, 1U);
		kwl_batch_flush(server, command);
		layers->home_bounded = (unsigned)kwl_compose_bounds_end(compose, layers->home_bounds);
	} else if (which == KWL_LAYER_DESKTOP) {
		layers->window_count = 0U;
		layers->noting = 1U;
		kwl_glass_draw_desktop_picture(server, command, windows, count);
		layers->noting = 0U;
	} else {
		kwl_glass_draw_anim_picture(server, command);
	}
	layers->bake_draws = server->draw_counts.shapes + server->draw_counts.quads;

	/* The state given back. */
	compose->scissor_now = scissor_saved;
	compose->framebuffer_now = framebuffer_saved;
	compose->backdrop_set = backdrop_saved;
	compose->backdrop_recording = backdrop_recording_saved;
	server->hover_recording = recording_saved;
	server->keep_colours = keep_saved;
	server->draw_section = section_saved;
	server->layer_on = layer_saved;
	server->view_x = view_x_saved;
	server->view_y = view_y_saved;
	server->view_width = view_width_saved;
	server->view_height = view_height_saved;
	server->draw_counts = counts;

	/* The pass ends with the image ready to be sampled, its joined quads drawn. */
	kwl_batch_flush(server, command);
	kwl_list_end_render_pass(command);

	/*
	 * The picture is drawn as far as the frames after it in the queue are
	 * concerned (the pass's dependency); it is taken when the submission
	 * completes without an eviction (kwl_layers_complete).
	 */
	layers->valid[which] = 1U;
	layers->inflight |= 1U << which;
	if (which == KWL_LAYER_HOME) {
		layers->home_wallpaper = glass_wallpaper_version(server);
		layers->home_tiles = glass_tiles_ready(server);
		layers->home_page = server->home_page;
		layers->home_accent = server->accent;
		layers->home_width = server->width;
		layers->home_height = server->height;
	}
	layers->bake_us = kwl_microseconds() - started;
}

/*
 * Makes the layers' pass: one attachment of the output's format, cleared,
 * drawn and left to be sampled; written after the last reads of the image,
 * and read after the writes.
 */
static VkResult
layers_pass(
	struct kwl_compose *compose)
{
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkSubpassDependency dependencies[2];
	VkRenderPassCreateInfo pass;

	/* One colour attachment of the output's format. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = compose->format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	memset(&reference, 0, sizeof(reference));
	reference.attachment = 0U;
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &reference;

	/* Written after the last reads (and writes) of the image. */
	memset(dependencies, 0, sizeof(dependencies));
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0U;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	/* Read after the writes. */
	dependencies[1].srcSubpass = 0U;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	/* The pass. */
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	pass.attachmentCount = 1U;
	pass.pAttachments = &attachment;
	pass.subpassCount = 1U;
	pass.pSubpasses = &subpass;
	pass.dependencyCount = 2U;
	pass.pDependencies = dependencies;
	return vkCreateRenderPass(compose->device, &pass, NULL, &compose->layers.pass);
}

/* Makes one picture: an image of the output's size and format, its memory, view, framebuffer and linearly sampled set. */
static VkResult
layers_image(
	struct kwl_compose *compose,
	struct kwl_layer_image *target)
{
	VkPhysicalDeviceMemoryProperties memory;
	VkMemoryRequirements requirements;
	VkImageCreateInfo image;
	VkMemoryAllocateInfo allocate;
	VkImageViewCreateInfo view;
	VkFramebufferCreateInfo framebuffer;
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	uint32_t index;
	VkResult result;

	/* The image: drawn into and sampled. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = compose->format;
	image.extent.width = compose->layers.width;
	image.extent.height = compose->layers.height;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	result = vkCreateImage(compose->device, &image, NULL, &target->image);
	if (result != VK_SUCCESS)
		return result;

	/* Device memory for it: the device's own type first, else any it takes. */
	vkGetImageMemoryRequirements(compose->device, target->image, &requirements);
	vkGetPhysicalDeviceMemoryProperties(compose->physical, &memory);
	for (index = 0; index < memory.memoryTypeCount; index++) {
		if ((requirements.memoryTypeBits & (1U << index)) != 0U &&
		    (memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U)
			break;
	}
	if (index == memory.memoryTypeCount) {
		for (index = 0; index < memory.memoryTypeCount; index++) {
			if ((requirements.memoryTypeBits & (1U << index)) != 0U)
				break;
		}
	}
	if (index == memory.memoryTypeCount)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* The memory, bound. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = index;
	result = vkAllocateMemory(compose->device, &allocate, NULL, &target->memory);
	if (result != VK_SUCCESS)
		return result;
	result = vkBindImageMemory(compose->device, target->image, target->memory, 0U);
	if (result != VK_SUCCESS)
		return result;

	/* Its view. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = target->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = compose->format;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	result = vkCreateImageView(compose->device, &view, NULL, &target->view);
	if (result != VK_SUCCESS)
		return result;

	/* The framebuffer of the layers' pass. */
	memset(&framebuffer, 0, sizeof(framebuffer));
	framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer.renderPass = compose->layers.pass;
	framebuffer.attachmentCount = 1U;
	framebuffer.pAttachments = &target->view;
	framebuffer.width = compose->layers.width;
	framebuffer.height = compose->layers.height;
	framebuffer.layers = 1U;
	result = vkCreateFramebuffer(compose->device, &framebuffer, NULL, &target->framebuffer);
	if (result != VK_SUCCESS)
		return result;

	/* The descriptor set, sampled linearly (the pictures are drawn smaller and larger). */
	result = kwl_compose_set_get(compose, &target->set);
	if (result != VK_SUCCESS)
		return result;
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = compose->linear_sampler;
	image_info.imageView = target->view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = target->set;
	write.dstBinding = 0U;
	write.descriptorCount = 1U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(compose->device, 1U, &write, 0U, NULL);

	/* Succeeded. */
	return VK_SUCCESS;
}

/* Destroys what was made of one picture. */
static void
layers_image_destroy(
	struct kwl_compose *compose,
	struct kwl_layer_image *target)
{
	/* In the reverse order of making. */
	if (target->set != VK_NULL_HANDLE)
		kwl_compose_set_put(compose, target->set);
	if (target->framebuffer != VK_NULL_HANDLE)
		vkDestroyFramebuffer(compose->device, target->framebuffer, NULL);
	if (target->view != VK_NULL_HANDLE)
		vkDestroyImageView(compose->device, target->view, NULL);
	if (target->image != VK_NULL_HANDLE)
		vkDestroyImage(compose->device, target->image, NULL);
	if (target->memory != VK_NULL_HANDLE)
		vkFreeMemory(compose->device, target->memory, NULL);
	memset(target, 0, sizeof(*target));
}

/* Destroys both pictures and the pass (the device is idle). */
static void
layers_destroy(
	struct kwl_compose *compose)
{
	unsigned index;

	/* Each picture, then the pass. */
	for (index = 0U; index < KWL_LAYERS; index++)
		layers_image_destroy(compose, &compose->layers.images[index]);
	if (compose->layers.pass != VK_NULL_HANDLE)
		vkDestroyRenderPass(compose->device, compose->layers.pass, NULL);
	compose->layers.pass = VK_NULL_HANDLE;
	compose->layers.ready = 0U;
}

/* Names a picture for the log. */
static const char *
layers_name(
	unsigned which)
{
	/* Home's, the desktop's, or the animated window's. */
	if (which == KWL_LAYER_HOME)
		return "home";
	if (which == KWL_LAYER_DESKTOP)
		return "desktop";

	/* Succeeded: the window's. */
	return "window";
}
