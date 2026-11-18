/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The glass maps: the colour of glass at every output pixel, worked out
 * beforehand, and pictures of the whole output that do not change.
 *
 * Flat glass (a window's panels, its blurred desktop) samples the blurred
 * wallpaper at its own place on the output and whitens or darkens it by its
 * colour; nothing else of the shape changes the colour of its inside.  So
 * for one colour the inside's colour depends on the output pixel alone,
 * and the panel shader that works it out (its inside variant, over the
 * same blurred wallpaper) can write it for every pixel of an image of the
 * output's size once.  The inside of such glass is then drawn by the map
 * variant (KWL_PANEL_INSIDE_MAP), which reads the map at the pixel itself
 * (nearest sampling, at the texel's centre): the same bytes, without the
 * per-pixel arithmetic.  Glass with a sheen from the top depends on its
 * box too; only glass over the whole output (Wiseview's, App Home's stage)
 * is mapped so, its box being the output's.
 *
 * A glass's map is made once two frames asked for it (a shape seen once,
 * or one whose key moves, is not worth a pass over the whole output, nor
 * the map of a lasting one), drawn before the next frame's own pass
 * (kwl_glass_maps_inband), and used from then on; meanwhile the glass is
 * drawn as before.  A map is drawn again
 * when the blurred wallpaper changes, and all go with the output's size.
 * Plain flat glass in the light appearance (no colour, not lifted) is the
 * blurred wallpaper itself, read by the same variant with its own linear
 * sampling, and needs no map.
 *
 * A whole-output picture (App Home's stage: its glass and its light) is
 * drawn the same way by the function its user gives, the frame after the
 * first that asked; the frames then draw it as one opaque image.
 *
 * A soft shadow around a box depends only on the pixel's place from the
 * box, its size, corner radius, softness and colour: its picture (as far
 * as the shadow reaches) is drawn once, on transparent black, and a
 * shadow at a whole pixel's place is then drawn as that picture, blended,
 * instead of working out the distance to the box at every pixel.  The
 * picture holds the shadow's colour in the output's bytes, so the blend
 * may differ from the shadow worked out in the frame by a step of a byte.
 */

#include "compose.h"
#include "glass.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* What a map holds: nothing, a colour waiting to be drawn, or that colour drawn. */
#define GLASS_MAP_FREE		0U
#define GLASS_MAP_WANTED	1U
#define GLASS_MAP_READY		2U

/* The most maps drawn before one frame's pass (each is a pass over the whole output), and the most shadows. */
#define GLASS_MAP_DRAWS		1U
#define GLASS_SHADOW_DRAWS	2U

static int map_key_same(const struct kwl_glass_map *map, const float *key);
static void map_key_of(const float *constants, float *key);
static struct kwl_glass_map *map_find(struct kwl_glass_map *maps, unsigned count, const float *key);
static struct kwl_glass_map *map_claim(struct kwl_server *server, struct kwl_glass_map *maps, unsigned count, const float *key);
static int map_take(struct kwl_server *server, struct kwl_glass_map *maps, unsigned count, const float *key, kwl_glass_picture_draw draw, VkDescriptorSet *map_set);
static int map_current(struct kwl_server *server, const struct kwl_glass_map *map);
static int map_candidate(struct kwl_server *server, const float *key);
static void maps_draw_wanted(struct kwl_server *server, struct kwl_list *command, struct kwl_glass_map *maps, unsigned count, unsigned most);
static VkResult maps_open(struct kwl_server *server);
static VkResult map_pass_create(struct kwl_compose *compose, VkAttachmentLoadOp load, VkRenderPass *created);
static void map_image_size(struct kwl_server *server, const struct kwl_glass_map *map, uint32_t *width, uint32_t *height);
static VkResult map_image_create(struct kwl_compose *compose, struct kwl_glass_map *map);
static void map_image_destroy(struct kwl_compose *compose, struct kwl_glass_map *map);
static void map_draw(struct kwl_server *server, struct kwl_list *command, struct kwl_glass_map *map, const struct kwl_import *blurred);
static void map_draw_glass(struct kwl_server *server, struct kwl_list *command, const struct kwl_glass_map *map, const struct kwl_import *blurred);
static void map_draw_picture(struct kwl_server *server, struct kwl_list *command, const struct kwl_glass_map *map);
static void map_draw_shadow(struct kwl_server *server, struct kwl_list *command, const struct kwl_glass_map *map, const struct kwl_import *blurred);

/*
 * Gives the descriptor set the inside of a glass shape (its constants,
 * the shape's own set) is drawn from with the map variant, when it can be:
 * glass on the blurred wallpaper, on the output's whole plane, flat or over
 * the whole output.  A colour without a map yet asks for one and gets none
 * this time.  Returns 1 with *map_set, or 0 (the inside is drawn as before).
 */
int
kwl_glass_map_set(
	struct kwl_server *server,
	const float *constants,
	VkDescriptorSet set,
	VkDescriptorSet *map_set)
{
	const struct kwl_import *blurred;
	struct kwl_compose *compose;
	float key[KWL_GLASS_MAP_KEY];
	int taken;

	/* Not possible on this device. */
	compose = server->compose;
	if (compose->glass_maps.failed)
		return 0;

	/* Glass on the blurred wallpaper (not on a window's backdrop). */
	blurred = glass_blurred(server);
	if (blurred == NULL ||
	    blurred->set == VK_NULL_HANDLE ||
	    set != blurred->set)
		return 0;

	/* On the output's whole plane, at its size (not a head's part of it). */
	if (server->view_width != 0U)
		return 0;
	if (constants[20] != (float)server->width || constants[21] != (float)server->height)
		return 0;

	/* A sheen from the top depends on the box: only a box that is the whole output. */
	if (constants[6] != 0.0f &&
	    (constants[8] != 0.0f ||
	     constants[9] != 0.0f ||
	     constants[10] != (float)server->width ||
	     constants[11] != (float)server->height))
		return 0;

	/*
	 * Plain flat glass in the light appearance (no colour, not lifted, not
	 * darkened) is the blurred wallpaper as it is: the shader mixes in
	 * nothing, so its own image serves.
	 */
	if (constants[15] == 0.0f &&
	    constants[4] == 0.0f &&
	    constants[5] == 0.0f &&
	    constants[6] == 0.0f) {
		*map_set = set;
		return 1;
	}

	/* The map of the colour, drawn over the blurred wallpaper as it is now. */
	map_key_of(constants, key);
	taken = map_take(server, compose->glass_maps.maps, KWL_GLASS_MAPS, key, NULL, map_set);
	if (!taken)
		return 0;

	/* Succeeded: the inside reads the map. */
	return 1;
}

/*
 * Gives the descriptor set of a whole-output picture that does not change
 * while the blurred wallpaper, the appearance and the output's size stay
 * (picture, KWL_GLASS_PICTURE_*), drawn by draw; a picture not drawn yet
 * is drawn before the next frame's pass.  Returns 1 with *picture_set, or 0 (the
 * caller draws it itself this time).
 */
int
kwl_glass_picture_set(
	struct kwl_server *server,
	unsigned picture,
	kwl_glass_picture_draw draw,
	VkDescriptorSet *picture_set)
{
	const struct kwl_import *blurred;
	float key[KWL_GLASS_MAP_KEY];
	int taken;

	/* Not possible on this device. */
	if (server->compose->glass_maps.failed)
		return 0;

	/* Nothing to draw from before the glass look opens. */
	blurred = glass_blurred(server);
	if (blurred == NULL || blurred->set == VK_NULL_HANDLE)
		return 0;

	/* The picture's key: its number, and the appearance it was drawn in. */
	memset(key, 0, sizeof(key));
	if (server->dark != 0)
		key[0] = 1.0f;
	key[9] = (float)picture;
	taken = map_take(server, server->compose->glass_maps.maps, KWL_GLASS_MAPS, key, draw, picture_set);
	if (!taken)
		return 0;

	/* Succeeded: the frame shows the picture. */
	return 1;
}

/*
 * Gives the picture of a soft shadow around a box (x, y, width, height on
 * the output, at whole pixels) of a corner radius, a softness (a whole
 * number of pixels) and a colour (straight, with its opacity): *origin is
 * where the picture's top-left corner goes on the output, *size its width
 * and height.  A shadow without a picture yet asks for one.  Returns 1 with
 * the picture's set, or 0 (the shadow is worked out as before).
 */
int
kwl_glass_shadow_set(
	struct kwl_server *server,
	const float *box,
	float radius,
	float soft,
	const float *colour,
	float *origin,
	float *size,
	VkDescriptorSet *shadow_set)
{
	struct kwl_glass_map *map;
	float key[KWL_GLASS_MAP_KEY];
	float whole[3];
	float reach;
	int taken;

	/* Not possible on this device. */
	if (server->compose->glass_maps.failed)
		return 0;

	/* A box at whole pixels, of a whole softness: the picture's pixels are the output's. */
	whole[0] = floorf(box[0]);
	whole[1] = floorf(box[1]);
	whole[2] = floorf(soft);
	if (box[0] != whole[0] || box[1] != whole[1] || soft != whole[2])
		return 0;
	if (box[2] <= 0.0f || box[3] <= 0.0f || soft < 1.0f)
		return 0;

	/* The shadow's key: its colour, the box's size, the radius and the softness. */
	memset(key, 0, sizeof(key));
	memcpy(key, colour, 4U * sizeof(float));
	key[4] = box[2];
	key[5] = box[3];
	key[6] = radius;
	key[7] = soft;
	key[9] = (float)KWL_GLASS_PICTURE_SHADOW;
	taken = map_take(server, server->compose->glass_maps.shadows, KWL_SHADOW_MAPS, key, NULL, shadow_set);
	if (!taken)
		return 0;

	/* As far as the shadow reaches around the box (the trimmed quad, glass.c), and a pixel for the rounding. */
	map = map_find(server->compose->glass_maps.shadows, KWL_SHADOW_MAPS, key);
	reach = soft + 1.0f;
	origin[0] = box[0] - reach;
	origin[1] = box[1] - reach;
	size[0] = (float)map->image_width;
	size[1] = (float)map->image_height;

	/* Succeeded: the shadow is the picture. */
	return 1;
}

/*
 * Draws the maps the last frames asked for, before the frame's pass (no
 * frame is in flight): at most GLASS_MAP_DRAWS glass maps or pictures and
 * GLASS_SHADOW_DRAWS shadows a frame.
 */
void
kwl_glass_maps_inband(
	struct kwl_server *server,
	struct kwl_list *command)
{
	const struct kwl_import *blurred;
	struct kwl_compose *compose;
	struct kwl_glass_maps *maps;

	/* Not possible on this device. */
	compose = server->compose;
	maps = &compose->glass_maps;
	if (maps->failed)
		return;

	/* Nothing to draw from before the glass look opens. */
	blurred = glass_blurred(server);
	if (blurred == NULL || blurred->set == VK_NULL_HANDLE)
		return;

	/*
	 * The maps of another size go first (the output turned or changed its
	 * mode), once no frame in flight may read them.
	 */
	if (maps->pass != VK_NULL_HANDLE && (maps->width != server->width || maps->height != server->height)) {
		if (compose->in_flight != 0U)
			return;
		kwl_glass_maps_destroy(compose);
	}

	/* The maps wanted, up to the frame's share, then the shadows. */
	maps_draw_wanted(server, command, maps->maps, KWL_GLASS_MAPS, GLASS_MAP_DRAWS);
	if (maps->failed)
		return;
	maps_draw_wanted(server, command, maps->shadows, KWL_SHADOW_MAPS, GLASS_SHADOW_DRAWS);
}

/*
 * Tells whether a map or a picture waits to be drawn before the next pass
 * (a quiet bake may draw it, layer-cache.c).  Returns 1 when one does.
 */
int
kwl_glass_maps_wanted(
	struct kwl_server *server)
{
	struct kwl_glass_maps *maps;
	unsigned index;

	/* Not possible on this device. */
	maps = &server->compose->glass_maps;
	if (maps->failed)
		return 0;

	/* Any map or shadow wanted. */
	for (index = 0U; index < KWL_GLASS_MAPS; index++) {
		if (maps->maps[index].state == GLASS_MAP_WANTED)
			return 1;
	}
	for (index = 0U; index < KWL_SHADOW_MAPS; index++) {
		if (maps->shadows[index].state == GLASS_MAP_WANTED)
			return 1;
	}

	/* None. */
	return 0;
}

/* Releases the maps' images and pass (no frame that reads them is in flight). */
void
kwl_glass_maps_destroy(
	struct kwl_compose *compose)
{
	struct kwl_glass_maps *maps;
	unsigned failed;
	unsigned index;

	/* Each map's image, and each shadow's. */
	maps = &compose->glass_maps;
	for (index = 0U; index < KWL_GLASS_MAPS; index++)
		map_image_destroy(compose, &maps->maps[index]);
	for (index = 0U; index < KWL_SHADOW_MAPS; index++)
		map_image_destroy(compose, &maps->shadows[index]);

	/* The passes the maps are drawn with. */
	if (maps->pass != VK_NULL_HANDLE)
		vkDestroyRenderPass(compose->device, maps->pass, NULL);
	if (maps->clear_pass != VK_NULL_HANDLE)
		vkDestroyRenderPass(compose->device, maps->clear_pass, NULL);

	/* Nothing is left; a device that could not make them stays without. */
	failed = maps->failed;
	memset(maps, 0, sizeof(*maps));
	maps->failed = failed;
}

/* Tells whether a map holds the colour of a key. */
static int
map_key_same(
	const struct kwl_glass_map *map,
	const float *key)
{
	unsigned index;

	/* A map that holds nothing holds no colour. */
	if (map->state == GLASS_MAP_FREE)
		return 0;

	/* Every value of the key. */
	for (index = 0U; index < KWL_GLASS_MAP_KEY; index++) {
		if (map->key[index] != key[index])
			return 0;
	}

	/* The same colour. */
	return 1;
}

/*
 * Gives the key of a glass shape's colour: its colour and opacity, whether
 * it is lifted (white) and darkened (dark glass), and its sheen with the
 * one over its box's height (the box is the whole output then); 0 for a
 * picture.
 */
static void
map_key_of(
	const float *constants,
	float *key)
{
	/* The colour, then the two choices glass_shape_glass_constants made. */
	key[0] = constants[12];
	key[1] = constants[13];
	key[2] = constants[14];
	key[3] = constants[15];
	key[4] = constants[4];
	key[5] = constants[5];

	/* The sheen and how it fades down the box (nothing on flat glass, whose box does not matter). */
	key[6] = constants[6];
	key[7] = 0.0f;
	if (constants[6] != 0.0f)
		key[7] = constants[7];
	key[8] = 0.0f;

	/* Glass, not a picture. */
	key[9] = 0.0f;
}

/*
 * Gives a map's set when it is drawn over the blurred wallpaper as it is
 * now and at the output's size, noting the frame's use; otherwise asks for
 * it (a new key takes a map, which draw draws for a picture).  Returns 1
 * with *map_set, or 0.
 */
static int
map_take(
	struct kwl_server *server,
	struct kwl_glass_map *maps,
	unsigned count,
	const float *key,
	kwl_glass_picture_draw draw,
	VkDescriptorSet *map_set)
{
	struct kwl_glass_map *map;
	int current;
	int again;

	/* The map of the key, current. */
	map = map_find(maps, count, key);
	current = 0;
	if (map != NULL)
		current = map_current(server, map);
	if (current) {
		map->used = server->frame;
		*map_set = map->set;
		return 1;
	}

	/* None yet: a key an earlier frame asked for too (a picture's at once) takes a map. */
	if (map == NULL) {
		if (draw == NULL) {
			again = map_candidate(server, key);
			if (!again)
				return 0;
		}
		map = map_claim(server, maps, count, key);
		if (map == NULL)
			return 0;
		map->draw = draw;
	}

	/* Stale or not drawn yet: the next frame draws it. */
	map->state = GLASS_MAP_WANTED;
	map->used = server->frame;

	/* Not drawn this time. */
	return 0;
}

/*
 * Tells whether a key without a map was asked for by an earlier frame
 * (it is then forgotten, as it takes a map); otherwise remembers that this
 * frame asked for it.  Returns 1 for a key asked for again.
 */
static int
map_candidate(
	struct kwl_server *server,
	const float *key)
{
	struct kwl_glass_maps *maps;
	unsigned index;
	int same;

	/* Each key remembered. */
	maps = &server->compose->glass_maps;
	for (index = 0U; index < KWL_GLASS_CANDIDATES; index++) {
		if (maps->candidate_frames[index] == 0U)
			continue;
		same = memcmp(maps->candidates[index], key, sizeof(maps->candidates[index]));
		if (same != 0)
			continue;

		/* Asked for already in this frame: still once. */
		if (maps->candidate_frames[index] == server->frame + 1U)
			return 0;

		/* Asked for again: forgotten, as it takes a map. */
		maps->candidate_frames[index] = 0U;
		return 1;
	}

	/* Remembered as asked for by this frame (the frame being drawn). */
	index = maps->candidate_next;
	memcpy(maps->candidates[index], key, sizeof(maps->candidates[index]));
	maps->candidate_frames[index] = server->frame + 1U;
	maps->candidate_next = (index + 1U) % KWL_GLASS_CANDIDATES;

	/* Asked for once. */
	return 0;
}

/*
 * Tells whether a map is drawn and still shows what it would show now: a
 * glass's map or a whole-output picture drawn over the blurred wallpaper as
 * it is and at the output's size; a shadow's picture depends on neither.
 */
static int
map_current(
	struct kwl_server *server,
	const struct kwl_glass_map *map)
{
	const struct kwl_import *blurred;

	/* Drawn. */
	if (map->state != GLASS_MAP_READY)
		return 0;

	/* A shadow's picture is what its key says. */
	if (map->key[9] == (float)KWL_GLASS_PICTURE_SHADOW)
		return 1;

	/* Over the blurred wallpaper as it is now, at the output's size. */
	blurred = glass_blurred(server);
	if (map->source != blurred->generation || map->version != blurred->version)
		return 0;
	if (map->width != server->width || map->height != server->height)
		return 0;

	/* Current. */
	return 1;
}

/* Finds the map of a key among count maps, or NULL. */
static struct kwl_glass_map *
map_find(
	struct kwl_glass_map *maps,
	unsigned count,
	const float *key)
{
	struct kwl_glass_map *map;
	unsigned index;
	int same;

	/* Each map. */
	for (index = 0U; index < count; index++) {
		map = &maps[index];
		same = map_key_same(map, key);
		if (same)
			return map;
	}

	/* None holds it. */
	return NULL;
}

/*
 * Gives a map for a new key: a free one, else the one used longest ago
 * that the last two frames did not use (so that more colours than maps do
 * not draw a map again every frame).  NULL when none can be given.
 */
static struct kwl_glass_map *
map_claim(
	struct kwl_server *server,
	struct kwl_glass_map *maps,
	unsigned count,
	const float *key)
{
	struct kwl_glass_map *oldest;
	struct kwl_glass_map *map;
	unsigned index;

	/* A free map first, else the oldest one not in use. */
	oldest = NULL;
	for (index = 0U; index < count; index++) {
		map = &maps[index];
		if (map->state == GLASS_MAP_FREE) {
			oldest = map;
			break;
		}
		if (map->used + 1U >= server->frame)
			continue;
		if (oldest == NULL || map->used < oldest->used)
			oldest = map;
	}

	/* Every map is in use. */
	if (oldest == NULL)
		return NULL;

	/* The map takes the key; its image is drawn again. */
	memcpy(oldest->key, key, sizeof(oldest->key));
	oldest->state = GLASS_MAP_WANTED;

	/* Succeeded: the map for the key. */
	return oldest;
}

/* Makes the maps' pass, at the output's size. */
static VkResult
maps_open(
	struct kwl_server *server)
{
	struct kwl_glass_maps *maps;
	VkResult result;

	/* The size the maps are drawn at. */
	maps = &server->compose->glass_maps;
	maps->width = server->width;
	maps->height = server->height;

	/* The pass that writes every pixel, and the one that clears first (a shadow's picture). */
	result = map_pass_create(server->compose, VK_ATTACHMENT_LOAD_OP_DONT_CARE, &maps->pass);
	if (result != VK_SUCCESS)
		return result;
	result = map_pass_create(server->compose, VK_ATTACHMENT_LOAD_OP_CLEAR, &maps->clear_pass);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: maps can be drawn. */
	return VK_SUCCESS;
}

/*
 * Draws the wanted maps among count, at most most of them, each image
 * made first at the size it needs (the output's, or a shadow's own); a
 * failure leaves the maps failed.
 */
static void
maps_draw_wanted(
	struct kwl_server *server,
	struct kwl_list *command,
	struct kwl_glass_map *maps,
	unsigned count,
	unsigned most)
{
	const struct kwl_import *blurred;
	struct kwl_compose *compose;
	struct kwl_glass_map *map;
	uint32_t width;
	uint32_t height;
	VkResult result;
	unsigned drawn;
	unsigned index;

	/* Each map wanted, up to the share. */
	compose = server->compose;
	blurred = glass_blurred(server);
	drawn = 0U;
	for (index = 0U; index < count && drawn < most; index++) {
		map = &maps[index];
		if (map->state != GLASS_MAP_WANTED)
			continue;

		/* The passes, made the first time (a failure is remembered). */
		if (compose->glass_maps.pass == VK_NULL_HANDLE) {
			result = maps_open(server);
			if (result != VK_SUCCESS) {
				printf("KWL GLASS map failed result=%d\n", (int)result);
				kwl_glass_maps_destroy(compose);
				compose->glass_maps.failed = 1U;
				return;
			}
		}

		/*
		 * Its image at the size it needs, made again when another size is
		 * needed, once no frame in flight may read the old one.
		 */
		map_image_size(server, map, &width, &height);
		if (map->image != VK_NULL_HANDLE && (map->image_width != width || map->image_height != height)) {
			if (compose->in_flight != 0U)
				continue;
			map_image_destroy(compose, map);
		}
		if (map->image == VK_NULL_HANDLE) {
			map->image_width = width;
			map->image_height = height;
			result = map_image_create(compose, map);
			if (result != VK_SUCCESS) {
				printf("KWL GLASS map failed result=%d\n", (int)result);
				kwl_glass_maps_destroy(compose);
				compose->glass_maps.failed = 1U;
				return;
			}
		}

		/* Drawn, ready for the shapes drawn after it. */
		map_draw(server, command, map, blurred);
		map->source = blurred->generation;
		map->version = blurred->version;
		map->width = server->width;
		map->height = server->height;
		map->state = GLASS_MAP_READY;
		drawn++;
		printf("KWL GLASS map drawn slot=%u picture=%.0f key=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f size=%ux%u\n",
		       index, (double)map->key[9], (double)map->key[0], (double)map->key[1], (double)map->key[2], (double)map->key[3],
		       (double)map->key[4], (double)map->key[5], (double)map->key[6], (double)map->key[7], map->image_width, map->image_height);
	}

}

/* Gives the size of a map's image: a shadow's reach around its box, Wiseview's ground twice the output's width, else the output's. */
static void
map_image_size(
	struct kwl_server *server,
	const struct kwl_glass_map *map,
	uint32_t *width,
	uint32_t *height)
{
	float reach;

	/* The output's size, for a glass's map and a whole-output picture; two outputs side by side for Wiseview's ground. */
	*width = server->width;
	*height = server->height;
	if (map->key[9] == (float)KWL_GLASS_PICTURE_WISEVIEW_GROUND)
		*width = 2U * server->width;
	if (map->key[9] != (float)KWL_GLASS_PICTURE_SHADOW)
		return;

	/* The box (its size up to whole pixels) and the shadow's reach (its softness and a pixel) on each side. */
	reach = map->key[7] + 1.0f;
	*width = (uint32_t)ceilf(map->key[4] + 2.0f * reach);
	*height = (uint32_t)ceilf(map->key[5] + 2.0f * reach);
}

/*
 * Makes a pass of the maps: every pixel written (load DONT_CARE) or the
 * image cleared first (CLEAR), and left to be sampled; it waits for the
 * reads of the map's last use, and its writes are seen by the reads after
 * it.
 */
static VkResult
map_pass_create(
	struct kwl_compose *compose,
	VkAttachmentLoadOp load,
	VkRenderPass *created)
{
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkSubpassDependency dependencies[2];
	VkRenderPassCreateInfo pass;
	VkResult result;

	/* One colour attachment of the output's format, so that a map's bytes are the frame's. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = compose->format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = load;
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

	/* Written after the last reads of the map, and after the copy of the blurred wallpaper. */
	memset(dependencies, 0, sizeof(dependencies));
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0U;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

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
	result = vkCreateRenderPass(compose->device, &pass, NULL, created);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded. */
	return VK_SUCCESS;
}

/* Makes a map's image: device memory, a view, a framebuffer of the maps' pass, and a set sampled at the nearest texel. */
static VkResult
map_image_create(
	struct kwl_compose *compose,
	struct kwl_glass_map *map)
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

	/* The image: drawn into and sampled, of the map's size and the output's format. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = compose->format;
	image.extent.width = map->image_width;
	image.extent.height = map->image_height;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	result = vkCreateImage(compose->device, &image, NULL, &map->image);
	if (result != VK_SUCCESS)
		return result;

	/* Device memory for it (the device's own first). */
	vkGetImageMemoryRequirements(compose->device, map->image, &requirements);
	vkGetPhysicalDeviceMemoryProperties(compose->physical, &memory);
	for (index = 0; index < memory.memoryTypeCount; index++) {
		if ((requirements.memoryTypeBits & (1U << index)) != 0U &&
		    (memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U)
			break;
	}

	/* Otherwise any type the image takes. */
	if (index == memory.memoryTypeCount) {
		for (index = 0; index < memory.memoryTypeCount; index++) {
			if ((requirements.memoryTypeBits & (1U << index)) != 0U)
				break;
		}
	}

	/* No memory the image takes. */
	if (index == memory.memoryTypeCount)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* The memory, bound. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = index;
	result = vkAllocateMemory(compose->device, &allocate, NULL, &map->memory);
	if (result != VK_SUCCESS)
		return result;
	result = vkBindImageMemory(compose->device, map->image, map->memory, 0U);
	if (result != VK_SUCCESS)
		return result;

	/* Its view. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = map->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = compose->format;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	result = vkCreateImageView(compose->device, &view, NULL, &map->view);
	if (result != VK_SUCCESS)
		return result;

	/* The framebuffer of the maps' pass. */
	memset(&framebuffer, 0, sizeof(framebuffer));
	framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer.renderPass = compose->glass_maps.pass;
	framebuffer.attachmentCount = 1U;
	framebuffer.pAttachments = &map->view;
	framebuffer.width = map->image_width;
	framebuffer.height = map->image_height;
	framebuffer.layers = 1U;
	result = vkCreateFramebuffer(compose->device, &framebuffer, NULL, &map->framebuffer);
	if (result != VK_SUCCESS)
		return result;

	/* The descriptor set, sampled at the nearest texel: a pixel reads its own. */
	result = kwl_compose_set_get(compose, &map->set);
	if (result != VK_SUCCESS)
		return result;
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = compose->sampler;
	image_info.imageView = map->view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = map->set;
	write.dstBinding = 0U;
	write.descriptorCount = 1U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(compose->device, 1U, &write, 0U, NULL);

	/* Succeeded: the map can be drawn. */
	return VK_SUCCESS;
}

/* Releases a map's image, view, framebuffer, memory and set; its key and state stay. */
static void
map_image_destroy(
	struct kwl_compose *compose,
	struct kwl_glass_map *map)
{
	/* Whatever was made. */
	if (map->framebuffer != VK_NULL_HANDLE)
		vkDestroyFramebuffer(compose->device, map->framebuffer, NULL);
	if (map->view != VK_NULL_HANDLE)
		vkDestroyImageView(compose->device, map->view, NULL);
	if (map->image != VK_NULL_HANDLE)
		vkDestroyImage(compose->device, map->image, NULL);
	if (map->memory != VK_NULL_HANDLE)
		vkFreeMemory(compose->device, map->memory, NULL);
	if (map->set != VK_NULL_HANDLE)
		kwl_compose_set_put(compose, map->set);

	/* No image. */
	map->framebuffer = VK_NULL_HANDLE;
	map->view = VK_NULL_HANDLE;
	map->image = VK_NULL_HANDLE;
	map->memory = VK_NULL_HANDLE;
	map->set = VK_NULL_HANDLE;
	map->image_width = 0U;
	map->image_height = 0U;
}

/* Draws a map: its pass on the map's image, with the pass's own viewport, scissor and quad corners, then a glass's colour or a picture. */
static void
map_draw(
	struct kwl_server *server,
	struct kwl_list *command,
	struct kwl_glass_map *map,
	const struct kwl_import *blurred)
{
	struct kwl_compose *compose;
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkViewport viewport;
	VkRect2D scissor;
	VkDeviceSize offset;
	unsigned shadow;

	/* The pass on the map's image; a shadow's picture on transparent black. */
	compose = server->compose;
	shadow = 0U;
	if (map->key[9] == (float)KWL_GLASS_PICTURE_SHADOW)
		shadow = 1U;
	memset(&clear, 0, sizeof(clear));
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = compose->glass_maps.pass;
	if (shadow) {
		pass.renderPass = compose->glass_maps.clear_pass;
		pass.clearValueCount = 1U;
		pass.pClearValues = &clear;
	}
	pass.framebuffer = map->framebuffer;
	pass.renderArea.extent.width = map->image_width;
	pass.renderArea.extent.height = map->image_height;
	kwl_list_begin_render_pass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
	kwl_compose_bind_forget(compose);

	/* The pass's own viewport, scissor and quad corners. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)map->image_width;
	viewport.height = (float)map->image_height;
	viewport.maxDepth = 1.0f;
	kwl_list_set_viewport(command, 0U, 1U, &viewport);
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = map->image_width;
	scissor.extent.height = map->image_height;
	kwl_list_set_scissor(command, 0U, 1U, &scissor);
	offset = 0U;
	kwl_list_bind_vertex_buffers(command, 0U, 1U, &compose->corners, &offset);

	/* A picture by its own function, a shadow or a glass's colour by one quad. */
	if (map->draw != NULL) {
		map_draw_picture(server, command, map);
	} else if (shadow) {
		map_draw_shadow(server, command, map, blurred);
	} else {
		map_draw_glass(server, command, map, blurred);
	}

	/* The pass ends with the map ready to be sampled. */
	kwl_list_end_render_pass(command);
	kwl_compose_bind_forget(compose);
}

/*
 * Draws a glass's map: one quad over the whole image with the opaque inside
 * variant of the glass the frame draws (light or dark), its colour, its
 * sheen down the whole output, at full opacity, over the blurred wallpaper.
 */
static void
map_draw_glass(
	struct kwl_server *server,
	struct kwl_list *command,
	const struct kwl_glass_map *map,
	const struct kwl_import *blurred)
{
	struct kwl_compose *compose;
	float constants[KWL_PANEL_CONSTANTS];
	VkPipeline pipeline;

	/* The whole image; the box is the image too (it places the sheen). */
	compose = server->compose;
	memset(constants, 0, sizeof(constants));
	constants[0] = -1.0f;
	constants[1] = -1.0f;
	constants[2] = 1.0f;
	constants[3] = 1.0f;
	constants[4] = map->key[4];
	constants[5] = map->key[5];
	constants[6] = map->key[6];
	constants[7] = map->key[7];
	constants[10] = (float)map->image_width;
	constants[11] = (float)map->image_height;
	memcpy(&constants[12], map->key, 4U * sizeof(float));
	constants[17] = (float)MODE_GLASS;
	constants[20] = (float)map->image_width;
	constants[21] = (float)map->image_height;
	constants[23] = 1.0f;

	/* The opaque inside of the glass the frame would draw: the dark glass's darkens. */
	pipeline = compose->panel_opaque_pipelines[KWL_PANEL_INSIDE_GLASS];
	if (map->key[5] != 0.0f) {
		constants[17] = -1.0f;
		pipeline = compose->panel_opaque_pipelines[KWL_PANEL_INSIDE_DARK_GLASS];
	}

	/* One quad. */
	kwl_compose_bind(server, command, pipeline, compose->panel_layout, blurred->set);
	kwl_list_push_constants(command, compose->panel_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0U, sizeof(constants), constants);
	kwl_list_draw(command, KWL_QUAD_VERTICES, 1U, 0U, 0U);
}

/*
 * Draws a picture's map by its function, as on the output's whole plane:
 * no damage, backdrop, layer, head, covering windows or hover tests; the
 * frame's draw counts and the state the drawing reads are given back.
 */
static void
map_draw_picture(
	struct kwl_server *server,
	struct kwl_list *command,
	const struct kwl_glass_map *map)
{
	struct kwl_compose *compose;
	struct kwl_draw_counts counts;
	VkDescriptorSet backdrop_saved;
	VkFramebuffer framebuffer_saved;
	VkRect2D scissor_saved;
	unsigned backdrop_recording_saved;
	unsigned recording_saved;
	unsigned keep_saved;
	unsigned cover_count_saved;
	unsigned layer_saved;
	int cover_level_saved;
	int32_t view_x_saved;
	int32_t view_y_saved;
	uint32_t view_width_saved;
	uint32_t view_height_saved;

	/* The state the drawing reads, kept to be given back. */
	compose = server->compose;
	scissor_saved = compose->scissor_now;
	framebuffer_saved = compose->framebuffer_now;
	backdrop_saved = compose->backdrop_set;
	backdrop_recording_saved = compose->backdrop_recording;
	recording_saved = server->hover_recording;
	keep_saved = server->keep_colours;
	cover_count_saved = server->cover_count;
	cover_level_saved = server->cover_level;
	layer_saved = server->layer_on;
	view_x_saved = server->view_x;
	view_y_saved = server->view_y;
	view_width_saved = server->view_width;
	view_height_saved = server->view_height;
	counts = server->draw_counts;

	/* The whole image, unplaced. */
	compose->scissor_now.offset.x = 0;
	compose->scissor_now.offset.y = 0;
	compose->scissor_now.extent.width = map->image_width;
	compose->scissor_now.extent.height = map->image_height;
	compose->framebuffer_now = VK_NULL_HANDLE;
	compose->backdrop_set = VK_NULL_HANDLE;
	compose->backdrop_recording = 0U;
	server->hover_recording = 0U;
	server->cover_count = 0U;
	server->cover_level = -1;
	server->layer_on = 0U;
	server->view_x = 0;
	server->view_y = 0;
	server->view_width = 0U;
	server->view_height = 0U;

	/* The picture, its joined quads drawn. */
	map->draw(server, command);
	kwl_batch_flush(server, command);

	/* The state given back. */
	compose->scissor_now = scissor_saved;
	compose->framebuffer_now = framebuffer_saved;
	compose->backdrop_set = backdrop_saved;
	compose->backdrop_recording = backdrop_recording_saved;
	server->hover_recording = recording_saved;
	server->keep_colours = keep_saved;
	server->cover_count = cover_count_saved;
	server->cover_level = cover_level_saved;
	server->layer_on = layer_saved;
	server->view_x = view_x_saved;
	server->view_y = view_y_saved;
	server->view_width = view_width_saved;
	server->view_height = view_height_saved;
	server->draw_counts = counts;
}

/*
 * Draws a shadow's picture: one quad over the whole image with the
 * shadow's own variant, blended over transparent black, its box where the
 * shadow's reach leaves it (a pixel more than the softness from each
 * side), at full opacity.
 */
static void
map_draw_shadow(
	struct kwl_server *server,
	struct kwl_list *command,
	const struct kwl_glass_map *map,
	const struct kwl_import *blurred)
{
	struct kwl_compose *compose;
	float constants[KWL_PANEL_CONSTANTS];
	float reach;

	/* The whole image; the box inside it by the reach. */
	compose = server->compose;
	reach = map->key[7] + 1.0f;
	memset(constants, 0, sizeof(constants));
	constants[0] = -1.0f;
	constants[1] = -1.0f;
	constants[2] = 1.0f;
	constants[3] = 1.0f;
	constants[6] = 1.0f;
	constants[7] = 1.0f;
	constants[8] = reach;
	constants[9] = reach;
	constants[10] = map->key[4];
	constants[11] = map->key[5];
	memcpy(&constants[12], map->key, 4U * sizeof(float));
	constants[16] = map->key[6];
	constants[17] = (float)MODE_SHADOW;
	constants[18] = map->key[7];
	constants[20] = (float)map->image_width;
	constants[21] = (float)map->image_height;
	constants[23] = 1.0f;

	/* One quad (the shadow samples nothing; the blurred wallpaper is bound as any set). */
	kwl_compose_bind(server, command, compose->panel_pipelines[1], compose->panel_layout, blurred->set);
	kwl_list_push_constants(command, compose->panel_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0U, sizeof(constants), constants);
	kwl_list_draw(command, KWL_QUAD_VERTICES, 1U, 0U, 0U);
}
