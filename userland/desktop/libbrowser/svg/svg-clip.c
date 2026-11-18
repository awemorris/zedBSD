/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * SVG clipPath elements built into clips of a picture.
 */

#include "svg/svg-clip.h"
#include "svg/svg-shape.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * The most child nodes of one clipPath that a build looks at (elements and
 * text alike).  A document can reference one clipPath from thousands of
 * elements, so the work of every build has to be bounded by something other
 * than the number of builds; a clipPath with more children than this ends
 * the build with ENOBUFS.  It equals the most paths a picture holds, which
 * is also the most parts, so no clip that could be built is refused by it.
 */
#define CLIP_CHILDREN_MAX	SVG_COMMANDS_MAX

/* The number of entries the picture's arrays of clips and parts are first made to hold. */
#define CLIP_CAPACITY_FIRST	16

/*
 * The word of clipPathUnits that makes the contents' coordinates fractions
 * of the clipped element's bounding box.  Any other text (userSpaceOnUse,
 * or a value that is none of the words) means the user space of the
 * clipped element.
 *
 * Values referenced: CSS Masking 1 §6.1 (clipPathUnits: userSpaceOnUse |
 * objectBoundingBox, userSpaceOnUse when not specified).
 */
static const char clip_units_box_word[] = "objectBoundingBox";

/*
 * The state shared by every level of one svg_clip_build: the arguments, the
 * clipPaths on the way (the chain a cycle is found in) and the styles one
 * level works with.  It lives on the heap because a style is large and the
 * chain of clipPath elements can be SVG_DEPTH_MAX deep.
 */
struct clip_build {
	const struct svg_tree *tree;
	const struct svg_style *style_outer;
	const struct vec_matrix *user_to_picture;
	const struct vec_rect *bbox;
	const struct svg_context *context;
	int outer;
	struct svg_picture *picture;
	const struct svg_node *chain[SVG_DEPTH_MAX];
	struct svg_style clip_style;
	struct svg_style child_style;
	struct svg_style target_style;
};

/*
 * What one clipPath contributes before its clip exists: where its parts
 * are in the picture's parts, and the clipPath that its own clip-path
 * names (NULL when it has no usable one).
 */
struct clip_own {
	size_t first_part;
	size_t part_count;
	const struct svg_node *parent_node;
};

/*
 * One shape that takes part in a clip: the shape element (a child of the
 * clipPath, or what a child 'use' references), its name, the matrix from
 * its user space to the picture's, the context its lengths resolve
 * against and the clip-rule of its part.
 */
struct clip_target {
	const struct svg_node *node;
	const char *name;
	struct vec_matrix matrix;
	struct svg_context context;
	enum vec_fill_rule rule;
};

static int clip_level(struct clip_build *build, const struct svg_node *node, int depth, int *index);
static int clip_own_build(struct clip_build *build, const struct svg_node *node, int depth, struct clip_own *own);
static int clip_children(struct clip_build *build, const struct svg_node *node, const struct vec_matrix *base, struct clip_own *own);
static int clip_target_find(struct clip_build *build, const struct svg_node *child, const char *child_name, const struct vec_matrix *base, struct clip_target *target);
static int clip_part_add(struct clip_build *build, const struct clip_target *target, struct clip_own *own);
static int clip_clip_add(struct svg_picture *picture, const struct clip_own *own, int parent, int *index);
static int clip_parts_reserve(struct svg_picture *picture);
static int clip_clips_reserve(struct svg_picture *picture);
static int clip_base_matrix(const struct clip_build *build, const struct svg_node *node, struct vec_matrix *base);
static int clip_units_box(const struct svg_tree *tree, const struct svg_node *node);
static const struct svg_node *clip_parent_find(const struct clip_build *build, const struct svg_style *style, int depth);
static const struct svg_node *clip_use_target(const struct svg_tree *tree, const struct svg_node *use);
static void clip_transform_read(const struct svg_tree *tree, const struct svg_node *node, struct vec_matrix *matrix);
static float clip_length_read(const struct svg_tree *tree, const struct svg_node *node, const char *name, enum svg_axis axis, const struct svg_context *context);
static int clip_name_is(const char *name, const char *expected);
static int clip_is_space(char c);
static const char *clip_skip_space(const char *p);

/*
 * Builds the clip of a clipPath element for one clipped element.
 *
 * Values referenced: CSS Masking 1 §5.1 (a clip-path that names no
 * clipPath clips nothing), §6.1 (the clipPath element: clipPathUnits,
 * the union of the children's silhouettes, display and visibility of the
 * children, the transform attribute, clip-path on the clipPath, an empty
 * clipping path clipping everything, properties inheriting from the
 * clipPath's ancestors), §6.2 (clip-rule); SVG 2 §5.5.2 (the matrix of a
 * use: transform, then translate(x, y)), CSS Transforms 1 §7.1 (a
 * transform with no inverse hides the element).
 *
 * Choices where the header is silent: the x and y of a child 'use' offset
 * the shape it references as they do for a drawn use; the shape a use
 * references takes its style from the use; a child whose matrix cannot be
 * inverted contributes nothing (as an element drawn with one is not
 * drawn); a bounding box that is not finite also gives an empty clip; a
 * style that cannot be computed (too deep) is the initial style; a
 * clipPath with more child nodes than CLIP_CHILDREN_MAX ends the build
 * with ENOBUFS.
 */
int
svg_clip_build(
	const struct svg_tree *tree,
	const struct svg_node *clip_path,
	const struct svg_style *style_outer,
	const struct vec_matrix *user_to_picture,
	const struct vec_rect *bbox,
	const struct svg_context *context,
	int outer,
	struct svg_picture *picture,
	int *index)
{
	struct clip_build *build;
	const char *name;
	int is_clip_path;
	int clip_index;
	int error;

	/* Refuses missing arguments. */
	if (tree == NULL || clip_path == NULL || user_to_picture == NULL || bbox == NULL)
		return EINVAL;
	if (context == NULL || picture == NULL || index == NULL)
		return EINVAL;
	if (tree->name == NULL || tree->attribute == NULL || tree->first_child == NULL || tree->next_sibling == NULL)
		return EINVAL;
	if (tree->find_id == NULL || tree->parent == NULL)
		return EINVAL;

	/* Refuses an element that is not a clipPath. */
	name = tree->name(tree->context, clip_path);
	is_clip_path = clip_name_is(name, "clipPath");
	if (!is_clip_path)
		return EINVAL;

	/* Allocates the state of the build. */
	build = calloc(1, sizeof(*build));
	if (build == NULL)
		return ENOMEM;

	/* Fills the state with the arguments. */
	build->tree = tree;
	build->style_outer = style_outer;
	build->user_to_picture = user_to_picture;
	build->bbox = bbox;
	build->context = context;
	build->outer = outer;
	build->picture = picture;

	/* Builds the clip, and the clips of the clipPaths its clip-path names first. */
	clip_index = -1;
	error = clip_level(build, clip_path, 0, &clip_index);
	free(build);
	if (error != 0)
		return error;

	/* Succeeded: the new clip is in the picture. */
	*index = clip_index;
	return 0;
}

/*
 * Builds the clip of one clipPath in the chain, and of the clipPath its
 * clip-path names (which becomes the parent), and reports the new clip's
 * index.  On an error nothing is left in the picture's clips and parts
 * (the paths added stay, unused).
 */
static int
clip_level(
	struct clip_build *build,
	const struct svg_node *node,
	int depth,
	int *index)
{
	struct svg_picture *picture;
	struct clip_own own;
	size_t saved_clips;
	size_t saved_parts;
	int parent;
	int error;

	/* Remembers how much the picture holds, to give it back on an error. */
	picture = build->picture;
	saved_clips = picture->clip_count;
	saved_parts = picture->part_count;

	/* Refuses another clip beyond the limit before any work is done for it. */
	if (picture->clip_count >= SVG_CLIPS_MAX)
		return ENOBUFS;

	/* Puts the clipPath on the chain, which is how a repeat of it is found. */
	build->chain[depth] = node;

	/* Builds the parts of this clipPath and finds the clipPath its clip-path names. */
	error = clip_own_build(build, node, depth, &own);
	if (error != 0) {
		picture->part_count = saved_parts;
		return error;
	}

	/* Builds the clip of the clipPath that is the parent; without one the parent is the outer clip. */
	parent = build->outer;
	if (own.parent_node != NULL) {
		error = clip_level(build, own.parent_node, depth + 1, &parent);
		if (error != 0) {
			picture->part_count = saved_parts;
			picture->clip_count = saved_clips;
			return error;
		}
	}

	/* Adds this clipPath's clip, whose parent is the one found above. */
	error = clip_clip_add(picture, &own, parent, index);
	if (error != 0) {
		picture->part_count = saved_parts;
		picture->clip_count = saved_clips;
		return error;
	}

	/* Succeeded: the clip is in the picture. */
	return 0;
}

/*
 * Builds the parts of one clipPath (the union of the shapes of its
 * children) and finds the clipPath its own clip-path names.
 */
static int
clip_own_build(
	struct clip_build *build,
	const struct svg_node *node,
	int depth,
	struct clip_own *own)
{
	const struct svg_tree *tree;
	struct vec_matrix base;
	int usable;
	int error;

	/* Starts with no part: the parts are the ones added from here on. */
	tree = build->tree;
	own->first_part = build->picture->part_count;
	own->part_count = 0;
	own->parent_node = NULL;

	/* Takes the clipPath's style from its ancestors (CSS Masking 1 §6.1); one that cannot be computed is the initial style. */
	error = svg_style_of(tree, node, build->style_outer, &build->clip_style);
	if (error != 0)
		svg_style_initial(&build->clip_style);

	/* Finds the clipPath that this one's clip-path names. */
	own->parent_node = clip_parent_find(build, &build->clip_style, depth);

	/* Makes the matrix of the clipPath's contents; a box with no area clips everything away (no part). */
	usable = clip_base_matrix(build, node, &base);
	if (!usable)
		return 0;

	/* Adds a part for each child that is a shape, or a use of one. */
	error = clip_children(build, node, &base, own);
	if (error != 0)
		return error;

	/* Succeeded: the parts are in the picture. */
	return 0;
}

/*
 * Adds the parts of the children of a clipPath, in tree order.
 */
static int
clip_children(
	struct clip_build *build,
	const struct svg_node *node,
	const struct vec_matrix *base,
	struct clip_own *own)
{
	const struct svg_tree *tree;
	const struct svg_node *child;
	const char *name;
	struct clip_target target;
	size_t visited;
	int found;
	int error;

	/* Looks at each child node: only shapes and uses of shapes add a part. */
	tree = build->tree;
	visited = 0;
	child = tree->first_child(tree->context, node);
	while (child != NULL) {
		visited++;
		if (visited > CLIP_CHILDREN_MAX)
			return ENOBUFS;

		/* Skips a node that is not an element of the SVG namespace. */
		name = tree->name(tree->context, child);
		if (name != NULL) {
			/* Finds the shape this child clips with, if it has one. */
			found = clip_target_find(build, child, name, base, &target);
			if (found) {
				/* Adds the shape as a part. */
				error = clip_part_add(build, &target, own);
				if (error != 0)
					return error;
			}
		}

		child = tree->next_sibling(tree->context, child);
	}

	/* Succeeded: every child that contributes has its part. */
	return 0;
}

/*
 * Finds the shape a child of a clipPath clips with and the matrix of its
 * user space (CSS Masking 1 §6.1: the shape elements, and a use that
 * references one directly).  Reports 1 and fills target, or 0 when the
 * child contributes nothing: another element, a use of anything but a
 * shape, display none, visibility hidden or collapse, a matrix with no
 * inverse.
 */
static int
clip_target_find(
	struct clip_build *build,
	const struct svg_node *child,
	const char *child_name,
	const struct vec_matrix *base,
	struct clip_target *target)
{
	const struct svg_tree *tree;
	const struct svg_style *shape_style;
	const struct svg_node *shape;
	const char *shape_name;
	struct svg_context context;
	struct vec_matrix element_matrix;
	struct vec_matrix shape_matrix;
	struct vec_matrix offset;
	struct vec_matrix inverse;
	float x;
	float y;
	int is_shape;
	int is_use;
	int error;

	/* Accepts only the shape elements and the use element. */
	tree = build->tree;
	is_shape = svg_shape_is(child_name);
	is_use = clip_name_is(child_name, "use");
	if (!is_shape && !is_use)
		return 0;

	/* Makes the child's style from the clipPath's, not from the clipped element's (CSS Masking 1 §6.1). */
	svg_style_inherit(&build->child_style, &build->clip_style);
	svg_style_apply(&build->child_style, &build->clip_style, tree, child);

	/* Skips a child that display none removes from the clipping path. */
	if (build->child_style.display == SVG_DISPLAY_NONE)
		return 0;

	/* Reads the child's transform attribute. */
	clip_transform_read(tree, child, &element_matrix);

	/* Takes the shape itself, or the one the use references. */
	shape = child;
	shape_name = child_name;
	shape_style = &build->child_style;
	shape_matrix = element_matrix;
	if (is_use) {
		/* Finds the element the use references; it has to be a shape (an indirect reference contributes nothing). */
		shape = clip_use_target(tree, child);
		if (shape == NULL)
			return 0;
		shape_name = tree->name(tree->context, shape);
		is_shape = svg_shape_is(shape_name);
		if (!is_shape)
			return 0;

		/* Makes the shape's style from the use's, as a shape drawn through a use is. */
		svg_style_inherit(&build->target_style, &build->child_style);
		svg_style_apply(&build->target_style, &build->child_style, tree, shape);
		shape_style = &build->target_style;
		if (shape_style->display == SVG_DISPLAY_NONE)
			return 0;

		/* Reads the use's offset, in lengths of the use's own font size (SVG 2 §5.5.2). */
		context = *build->context;
		context.font_size = build->child_style.font_size;
		x = clip_length_read(tree, child, "x", SVG_AXIS_X, &context);
		y = clip_length_read(tree, child, "y", SVG_AXIS_Y, &context);
		error = vec_matrix_translate(&offset, x, y);
		if (error != 0)
			return 0;

		/* Makes the matrix: the shape's transform, then the use's offset, then the use's transform (SVG 2 §5.5.2). */
		clip_transform_read(tree, shape, &shape_matrix);
		vec_matrix_multiply(&shape_matrix, &shape_matrix, &offset);
		vec_matrix_multiply(&shape_matrix, &shape_matrix, &element_matrix);
	}

	/* Skips a shape that visibility hides from the clipping path. */
	if (shape_style->visibility != SVG_VISIBILITY_VISIBLE)
		return 0;

	/* Places the shape's user space in the picture. */
	vec_matrix_multiply(&shape_matrix, &shape_matrix, base);

	/* Skips a shape whose matrix has no inverse (CSS Transforms 1 §7.1: it is not rendered). */
	error = vec_matrix_invert(&inverse, &shape_matrix);
	if (error != 0)
		return 0;

	/* Fills the target: the lengths of the shape resolve against the shape's own font size. */
	target->node = shape;
	target->name = shape_name;
	target->matrix = shape_matrix;
	target->context = *build->context;
	target->context.font_size = shape_style->font_size;
	target->rule = shape_style->clip_rule;

	/* Succeeded: the shape takes part in the clip. */
	return 1;
}

/*
 * Adds the part of one shape: its path in its user space, with the
 * matrix to the picture and its clip-rule.  A shape that renders nothing
 * or whose path is invalid adds nothing.
 */
static int
clip_part_add(
	struct clip_build *build,
	const struct clip_target *target,
	struct clip_own *own)
{
	struct svg_picture *picture;
	struct svg_clip_part *part;
	struct vec_path path;
	enum svg_shape_result result;
	size_t path_index;
	int error;

	/* Builds the shape's equivalent path. */
	picture = build->picture;
	vec_path_init(&path);
	result = SVG_SHAPE_EMPTY;
	error = svg_shape_path(build->tree, target->node, target->name, &target->context, &path, &result);
	if (error != 0) {
		/* A memory error or a path beyond the limits ends the build; any other error draws nothing of the shape. */
		vec_path_release(&path);
		if (error == ENOMEM || error == ENOBUFS)
			return error;

		return 0;
	}

	/* Adds nothing for a shape that renders nothing. */
	if (result != SVG_SHAPE_DRAWN) {
		vec_path_release(&path);
		return 0;
	}

	/* Makes room for the part first, so a failure after the path is taken leaves nothing half done. */
	error = clip_parts_reserve(picture);
	if (error != 0) {
		vec_path_release(&path);
		return error;
	}

	/* Gives the path to the picture (this takes its arrays, and frees them on an error). */
	error = svg_picture_add_path(picture, &path, &path_index);
	if (error != 0) {
		/* An invalid path draws nothing of the shape; the limits and memory end the build. */
		if (error == EINVAL)
			return 0;

		return error;
	}

	/* Writes the part: the path, the matrix of its user space and its clip-rule. */
	part = &picture->parts[picture->part_count];
	part->path = path_index;
	part->matrix = target->matrix;
	part->rule = target->rule;
	picture->part_count++;
	own->part_count++;

	/* Succeeded: the shape is a part of the clip. */
	return 0;
}

/*
 * Adds a clip made of the parts of own, with its parent, and reports its
 * index.
 */
static int
clip_clip_add(
	struct svg_picture *picture,
	const struct clip_own *own,
	int parent,
	int *index)
{
	struct svg_clip *clip;
	int error;

	/* Refuses a clip beyond the limit. */
	if (picture->clip_count >= SVG_CLIPS_MAX)
		return ENOBUFS;

	/* Makes room for one more clip. */
	error = clip_clips_reserve(picture);
	if (error != 0)
		return error;

	/* Writes the clip: where its parts are and the clip it is met with. */
	clip = &picture->clips[picture->clip_count];
	clip->first_part = own->first_part;
	clip->part_count = own->part_count;
	clip->parent = parent;

	/* The new clip's index is its place in the picture's clips. */
	*index = (int)picture->clip_count;
	picture->clip_count++;

	/* Succeeded: the picture holds the clip. */
	return 0;
}

/*
 * Makes room in the picture's parts for one more.  Errors: ENOBUFS beyond
 * SVG_COMMANDS_MAX parts (a part has a path of its own, which is limited
 * to the same number); ENOMEM.
 */
static int
clip_parts_reserve(
	struct svg_picture *picture)
{
	struct svg_clip_part *grown;
	size_t capacity;

	/* Refuses a part beyond the limit. */
	if (picture->part_count >= SVG_COMMANDS_MAX)
		return ENOBUFS;

	/* Keeps the array when it has room. */
	if (picture->part_count < picture->part_capacity)
		return 0;

	/* Doubles the capacity (it stays far below the largest size_t). */
	capacity = picture->part_capacity * 2;
	if (capacity < CLIP_CAPACITY_FIRST)
		capacity = CLIP_CAPACITY_FIRST;
	grown = realloc(picture->parts, capacity * sizeof(*grown));
	if (grown == NULL)
		return ENOMEM;
	picture->parts = grown;
	picture->part_capacity = capacity;

	/* Succeeded: the array holds one more part. */
	return 0;
}

/*
 * Makes room in the picture's clips for one more.  Errors: ENOBUFS beyond
 * SVG_CLIPS_MAX clips; ENOMEM.
 */
static int
clip_clips_reserve(
	struct svg_picture *picture)
{
	struct svg_clip *grown;
	size_t capacity;

	/* Refuses a clip beyond the limit. */
	if (picture->clip_count >= SVG_CLIPS_MAX)
		return ENOBUFS;

	/* Keeps the array when it has room. */
	if (picture->clip_count < picture->clip_capacity)
		return 0;

	/* Doubles the capacity (it stays far below the largest size_t). */
	capacity = picture->clip_capacity * 2;
	if (capacity < CLIP_CAPACITY_FIRST)
		capacity = CLIP_CAPACITY_FIRST;
	grown = realloc(picture->clips, capacity * sizeof(*grown));
	if (grown == NULL)
		return ENOMEM;
	picture->clips = grown;
	picture->clip_capacity = capacity;

	/* Succeeded: the array holds one more clip. */
	return 0;
}

/*
 * Makes the matrix from the clipPath's contents to the picture: the
 * contents' own transform, inside the bounding box mapping when
 * clipPathUnits is objectBoundingBox, inside the clipped element's matrix
 * (CSS Masking 1 §6.1).  Reports 0 when objectBoundingBox meets a box with
 * no width or no height (or one that is not finite), which clips
 * everything away; else 1.
 */
static int
clip_base_matrix(
	const struct clip_build *build,
	const struct svg_node *node,
	struct vec_matrix *base)
{
	const struct vec_rect *bbox;
	struct vec_matrix transform;
	struct vec_matrix box_map;
	float width;
	float height;
	int box_units;
	int finite_left;
	int finite_top;
	int finite_right;
	int finite_bottom;
	int finite_width;
	int finite_height;

	/* Takes the clipPath's own transform attribute. */
	clip_transform_read(build->tree, node, &transform);

	/* Tells which coordinate system the contents are in. */
	box_units = clip_units_box(build->tree, node);
	if (box_units) {
		/* Refuses a box that is not finite. */
		bbox = build->bbox;
		finite_left = vec_finite(bbox->left);
		finite_top = vec_finite(bbox->top);
		finite_right = vec_finite(bbox->right);
		finite_bottom = vec_finite(bbox->bottom);
		if (!finite_left || !finite_top || !finite_right || !finite_bottom)
			return 0;

		/* Refuses a box with no width or no height (an empty clip). */
		width = bbox->right - bbox->left;
		height = bbox->bottom - bbox->top;
		finite_width = vec_finite(width);
		finite_height = vec_finite(height);
		if (!finite_width || !finite_height)
			return 0;
		if (!(width > 0.0f) || !(height > 0.0f))
			return 0;

		/* The contents' (0, 0) to (1, 1) is the box: translate(left, top) scale(width, height). */
		box_map.a = width;
		box_map.b = 0.0f;
		box_map.c = 0.0f;
		box_map.d = height;
		box_map.e = bbox->left;
		box_map.f = bbox->top;

		/* The transform applies inside the box mapping. */
		vec_matrix_multiply(base, &transform, &box_map);
	} else {
		/* The contents are in the clipped element's user space. */
		*base = transform;
	}

	/* Places the clipped element's user space in the picture. */
	vec_matrix_multiply(base, base, build->user_to_picture);

	/* Succeeded: the contents have a matrix. */
	return 1;
}

/*
 * Tells whether the clipPath's clipPathUnits is objectBoundingBox (the
 * text without the white space around it equals the word).
 */
static int
clip_units_box(
	const struct svg_tree *tree,
	const struct svg_node *node)
{
	const char *text;
	const char *start;
	const char *end;
	size_t length;
	size_t word_length;
	int different;
	int space;

	/* Reads the attribute. */
	text = tree->attribute(tree->context, node, "clipPathUnits");
	if (text == NULL)
		return 0;

	/* Takes the text without the white space around it. */
	start = clip_skip_space(text);
	end = start + strlen(start);
	space = 1;
	while (space && end > start) {
		space = clip_is_space(end[-1]);
		if (space)
			end--;
	}
	length = (size_t)(end - start);

	/* Compares it with the word. */
	word_length = strlen(clip_units_box_word);
	if (length != word_length)
		return 0;
	different = strncmp(start, clip_units_box_word, length);
	if (different != 0)
		return 0;

	/* Succeeded: the text is the word. */
	return 1;
}

/*
 * Finds the clipPath that a clipPath's clip-path (in its style) names, to
 * be the parent of its clip.  Reports NULL for no clip-path, an id that
 * names no clipPath (CSS Masking 1 §5.1: no clipping), one already on the
 * chain (a cycle) and a chain that has reached SVG_DEPTH_MAX.
 */
static const struct svg_node *
clip_parent_find(
	const struct clip_build *build,
	const struct svg_style *style,
	int depth)
{
	const struct svg_tree *tree;
	const struct svg_node *found;
	const char *name;
	int is_clip_path;
	int level;

	/* Keeps the outer clip for a clipPath with no clip-path. */
	if (!style->has_clip_path)
		return NULL;

	/* Stops the chain at its limit. */
	if (depth + 1 >= SVG_DEPTH_MAX)
		return NULL;

	/* Finds the element the clip-path names. */
	tree = build->tree;
	found = tree->find_id(tree->context, style->clip_path);
	if (found == NULL)
		return NULL;

	/* Accepts only a clipPath. */
	name = tree->name(tree->context, found);
	is_clip_path = clip_name_is(name, "clipPath");
	if (!is_clip_path)
		return NULL;

	/* Refuses a clipPath that is already on the chain: its clip-path is a cycle. */
	for (level = 0;
	     level <= depth;
	     level++) {
		if (build->chain[level] == found)
			return NULL;
	}

	/* Succeeded: the clipPath becomes the parent. */
	return found;
}

/*
 * Finds the element a use references (href, a "#id" that is the whole of
 * the attribute).  Reports NULL for no usable reference.
 */
static const struct svg_node *
clip_use_target(
	const struct svg_tree *tree,
	const struct svg_node *use)
{
	const struct svg_node *target;
	const char *text;
	const char *cursor;
	char id[SVG_ID_MAX + 1];
	int error;

	/* Reads the href attribute. */
	text = tree->attribute(tree->context, use, "href");
	if (text == NULL)
		return NULL;

	/* Reads the reference, which must be a local "#id" and the whole of the attribute (white space around it is allowed). */
	cursor = clip_skip_space(text);
	error = svg_url_read(&cursor, 1, id);
	if (error != 0)
		return NULL;
	cursor = clip_skip_space(cursor);
	if (*cursor != '\0')
		return NULL;

	/* Finds the element the reference names. */
	target = tree->find_id(tree->context, id);

	/* Succeeded: the element, or NULL when the id names none. */
	return target;
}

/*
 * Reads an element's transform attribute into a matrix: the identity when
 * it has none or an invalid one (an invalid attribute is ignored).
 */
static void
clip_transform_read(
	const struct svg_tree *tree,
	const struct svg_node *node,
	struct vec_matrix *matrix)
{
	const char *text;
	int error;

	/* Starts from the identity, which is what an absent or invalid attribute leaves. */
	vec_matrix_identity(matrix);
	text = tree->attribute(tree->context, node, "transform");
	if (text == NULL)
		return;

	/* Parses the attribute; a failure leaves the matrix as it was. */
	error = svg_parse_transform(text, matrix);
	if (error != 0)
		vec_matrix_identity(matrix);
}

/*
 * Reads an element's length attribute resolved to user units against the
 * context: 0 when it has none or an invalid one.
 */
static float
clip_length_read(
	const struct svg_tree *tree,
	const struct svg_node *node,
	const char *name,
	enum svg_axis axis,
	const struct svg_context *context)
{
	struct svg_length length;
	const char *text;
	float value;
	int error;

	/* Reads the attribute. */
	text = tree->attribute(tree->context, node, name);
	if (text == NULL)
		return 0.0f;

	/* Parses it; an invalid length is as if absent. */
	error = svg_parse_length(text, &length);
	if (error != 0)
		return 0.0f;

	/* Resolves it against the context. */
	value = svg_length_resolve(&length, axis, context);

	/* Succeeded: the length in user units. */
	return value;
}

/* Tells whether an element name (NULL for a node that is not an SVG element) is exactly the expected one. */
static int
clip_name_is(
	const char *name,
	const char *expected)
{
	int different;

	/* Refuses a node that has no name. */
	if (name == NULL)
		return 0;

	/* Compares the two names. */
	different = strcmp(name, expected);
	if (different != 0)
		return 0;

	/* Succeeded: the names are the same. */
	return 1;
}

/* Tells whether a character is one of the white space characters SVG allows in a value. */
static int
clip_is_space(
	char c)
{
	/* Space, tab, line feed, carriage return and form feed are white space. */
	if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')
		return 1;

	/* Succeeded: anything else is not white space. */
	return 0;
}

/* Skips the white space at a position, stopping at the first other character or the NUL. */
static const char *
clip_skip_space(
	const char *p)
{
	int space;

	/* Moves past every white space character. */
	space = clip_is_space(*p);
	while (space) {
		p++;
		space = clip_is_space(*p);
	}

	/* Succeeded: p is at a character that is not white space. */
	return p;
}
