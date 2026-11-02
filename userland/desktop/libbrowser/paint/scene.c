/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A scene (paint/paint.h): a display list taken from the page that built
 * it, made to point at nothing the page may change or free afterwards, so
 * that a drawing thread can draw it while the page goes on.
 *
 * The list's own arena (its glyphs, its gradients, its filters) comes with
 * it, and the documents of its frames stay held by it.  What its items
 * point at outside it is held or copied: each image the page keeps becomes
 * a copy of the bitmap's record in the list's arena, over the same pixels,
 * which the scene holds (img_hold_retain); and the font files of the text
 * system it was built with are held face by face, so that the drawing
 * thread opens faces of its own on the same bytes.
 */

#include "paint/paint.h"
#include "base/base.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The fewest slots of the table that tells the images of a list apart while a scene is made. */
#define SCENE_IMAGES_MIN	16U

/*
 * One image of the list while its scene is made: the bitmap the page keeps
 * (the key) and the scene's copy of its record.
 */
struct scene_image {
	const struct img_bitmap *bitmap;
	struct img_bitmap *copy;
};

static int scene_images(struct paint_scene *scene);
static struct scene_image *scene_slot(struct scene_image *table, size_t capacity, const struct img_bitmap *bitmap);

/*
 * Makes a scene of a list: the list's contents are taken (it is left empty
 * and ready for the next one), its images are held and copied, and the
 * font files of the text system are held.  Reports 0, or ENOMEM (the list
 * is then released).
 */
int
paint_scene_make(
	struct paint_list *list,
	const struct text_system *text,
	struct paint_scene **out)
{
	struct paint_scene *scene;
	int face;
	int error;

	/* The scene, with the list's contents. */
	*out = NULL;
	scene = calloc(1, sizeof(*scene));
	if (scene == NULL) {
		paint_release(list);
		paint_list_init(list);
		return ENOMEM;
	}
	scene->refs = 1U;
	scene->list = *list;
	paint_list_init(list);
	wb_vector_init(&scene->holds, sizeof(struct img_hold *));

	/* The images its items draw, held and copied. */
	error = scene_images(scene);
	if (error != 0) {
		paint_scene_release(scene);
		return error;
	}

	/* The font files the glyphs are drawn from, face by face. */
	for (face = 0; face < TEXT_FACES; face++)
		scene->faces[face] = text_face_bytes(text, face);

	/* Succeeded: the scene points at nothing the page changes. */
	*out = scene;
	return 0;
}

/* Holds a scene once more, from any thread. */
void
paint_scene_retain(
	struct paint_scene *scene)
{
	/* One hold more. */
	__atomic_fetch_add(&scene->refs, 1U, __ATOMIC_RELAXED);
}

/* Lets go of a hold on a scene, from any thread (NULL is nothing); the last frees it. */
void
paint_scene_release(
	struct paint_scene *scene)
{
	struct img_hold **hold;
	size_t index;
	unsigned refs;
	int face;

	/* Nothing held, or held by others still. */
	if (scene == NULL)
		return;
	refs = __atomic_sub_fetch(&scene->refs, 1U, __ATOMIC_ACQ_REL);
	if (refs != 0U)
		return;

	/* The pixels of its images. */
	for (index = 0; index < scene->holds.count; index++) {
		hold = wb_vector_at(&scene->holds, index);
		img_hold_release(*hold);
	}
	wb_vector_release(&scene->holds);

	/* The font files. */
	for (face = 0; face < TEXT_FACES; face++)
		text_bytes_release(scene->faces[face]);

	/* The list, its frames' documents with it. */
	paint_release(&scene->list);
	free(scene);
}

/*
 * Makes the scene's items point at copies of the records of the images
 * the page keeps (in the list's arena), whose pixels it holds; an image
 * of an arena (a gradient's) already lives as long as the list.
 */
static int
scene_images(
	struct paint_scene *scene)
{
	struct scene_image *table;
	struct scene_image *slot;
	struct paint_item *item;
	struct img_hold *hold;
	struct img_bitmap *copy;
	size_t capacity;
	size_t index;
	int error;

	/* The table, at least twice as large as the items, a power of two. */
	capacity = SCENE_IMAGES_MIN;
	while (capacity < scene->list.items.count * 2U)
		capacity *= 2U;
	table = calloc(capacity, sizeof(*table));
	if (table == NULL)
		return ENOMEM;

	/* Each image item, its bitmap copied once. */
	error = 0;
	for (index = 0; index < scene->list.items.count; index++) {
		item = wb_vector_at(&scene->list.items, index);
		if (item->image == NULL || item->image->hold == NULL)
			continue;

		/* One copied before. */
		slot = scene_slot(table, capacity, item->image);
		if (slot->bitmap != NULL) {
			item->image = slot->copy;
			continue;
		}

		/* The record in the arena, without the hold, which the scene keeps. */
		copy = wb_arena_alloc(&scene->list.arena, sizeof(*copy));
		if (copy == NULL) {
			error = ENOMEM;
			break;
		}
		hold = img_hold_retain(item->image);
		error = wb_vector_push(&scene->holds, &hold);
		if (error != 0) {
			img_hold_release(hold);
			break;
		}
		*copy = *item->image;
		copy->hold = NULL;
		slot->bitmap = item->image;
		slot->copy = copy;
		item->image = copy;
	}

	/* The table goes. */
	free(table);
	if (error != 0)
		return error;

	/* Succeeded: every image is the scene's. */
	return 0;
}

/* Finds a bitmap's slot in the table (open addressing): its own, or the empty one it goes in. */
static struct scene_image *
scene_slot(
	struct scene_image *table,
	size_t capacity,
	const struct img_bitmap *bitmap)
{
	size_t slot;

	/* From the pointer's hash, on until it or an empty slot. */
	slot = (size_t)(((uint64_t)(uintptr_t)bitmap * 0x9e3779b97f4a7c15ULL) >> 20) & (capacity - 1U);
	while (table[slot].bitmap != NULL && table[slot].bitmap != bitmap)
		slot = (slot + 1U) & (capacity - 1U);

	/* Succeeded: the slot. */
	return &table[slot];
}
