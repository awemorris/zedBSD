/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * SVG document fragments built into pictures of painting commands.
 */

#include "svg/svg-picture.h"

#include "svg/svg-clip.h"
#include "svg/svg-shape.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The font size (px) of the root when the page gives none usable. */
#define PICTURE_ROOT_FONT_SIZE_DEFAULT	16.0f

/* The tolerance of the flattening that measures a path for pathLength (SVG 2 §9.6.1): the drawing tolerance over 16. */
#define PICTURE_MEASURE_TOLERANCE	(VEC_TOLERANCE_DEFAULT / 16.0f)

/* The capacity a picture's array starts with. */
#define PICTURE_ARRAY_INITIAL		16UL

/* The entries of the cache that remembers where a gradient's stops were stored. */
#define PICTURE_STOPS_CACHE		64UL

/* The miter limit of the initial style, used when a style's value is not a number. */
#define PICTURE_MITER_LIMIT_DEFAULT	4.0f

/* The entries of the cache that remembers the bounding box of a container. */
#define PICTURE_BBOX_CACHE		64UL

/* What the walk makes of an element. */
enum picture_kind {
	/* Not drawn, and neither is its subtree. */
	PICTURE_KIND_SKIP = 0,
	/* A container that is drawn through its children ('g', 'a'). */
	PICTURE_KIND_CONTAINER = 1,
	/* A shape element. */
	PICTURE_KIND_SHAPE = 2,
	/* A nested 'svg': a container that makes a new viewport. */
	PICTURE_KIND_SVG = 3,
	/* A 'symbol' that is the referenced element of a 'use': a container that makes a new viewport. */
	PICTURE_KIND_SYMBOL = 4,
	/* A 'use': a container whose only child is the element it references. */
	PICTURE_KIND_USE = 5,
	/* A 'switch': a container that draws only the first of its children whose conditions hold. */
	PICTURE_KIND_SWITCH = 6
};

/*
 * What a 'use' gives the element it references: its width and height,
 * when it has valid ones, which replace those of a referenced 'svg' or
 * 'symbol' (SVG 2 §5.5.2).  It lives for the walk of that one element.
 */
struct picture_instance {
	int has_width;
	float width;
	int has_height;
	float height;
};

/*
 * Where the content of a container is: the matrices and the viewport its
 * children are drawn in, and for a 'use' the element it references.
 *
 * own is the container's own user space (the one its clip-path is in: the
 * matrix of its transform, and for a 'use' with its x and y translation
 * too); inner maps the children's space into own (the identity, except for
 * a viewport, whose x, y and viewBox it holds); content is inner then own,
 * the matrix the children are placed with.  viewport_width and
 * viewport_height are what percentages in the children resolve against.
 * instance is what a 'use' passes to its target.
 */
struct picture_frame {
	struct vec_matrix own;
	struct vec_matrix inner;
	struct vec_matrix content;
	float viewport_width;
	float viewport_height;
	const struct svg_node *target;
	struct picture_instance instance;
};

/*
 * The box a walk collects instead of drawing: the union of the boxes of
 * the shapes it meets, and whether it met any.
 */
struct picture_box {
	struct vec_rect rect;
	int found;
};

/*
 * The bounding box found for one container, with what the box depends on:
 * a container met again in the same situation (typically the content of
 * a 'use' that is used many times) is not walked again.  An entry is in
 * use when its node is not NULL.
 */
struct picture_bbox_entry {
	const struct svg_node *node;
	int depth;
	size_t use_count;
	float viewport_width;
	float viewport_height;
	float font_size;
	struct picture_instance instance;
	struct vec_rect box;
};

/*
 * Where the stops of one gradient element were stored in the picture's
 * stops.  The stops of an element do not depend on the shape it paints
 * (only its geometry does), so a gradient used by many shapes is stored
 * once instead of once per command.
 */
struct picture_stops_entry {
	const struct svg_node *node;
	size_t first;
	size_t count;
};

/*
 * The state of one build: the tree and the picture it fills, the styles
 * of the elements on the path from the root to the one being walked, the
 * sticky error and the counters of the limits.  It lives for one call of
 * svg_picture_build.
 *
 * styles[0] is the style the root inherits (the outer style, or the
 * initial one); styles[d] is the style of the element at depth d, the root
 * being at depth 1.  error is 0, ENOBUFS (a limit was reached: the walk
 * stops and the picture keeps what it has) or ENOMEM (the picture is
 * dropped); ENOMEM is never replaced by ENOBUFS.  open_groups is the
 * number of groups started and not yet ended: each holds back one command
 * of the limit for its GROUP_END, so that a group can always be closed.
 * points counts the points of the paths added to the picture.  uses holds
 * the 'use' elements being expanded, outermost first, use_count of them:
 * a reference back to one of them or to what holds it is circular.
 * bbox_cache remembers the bounding boxes of containers.
 */
struct picture_build {
	const struct svg_tree *tree;
	const struct svg_style *outer;
	struct svg_picture *picture;
	float root_font_size;
	float viewport_width;
	float viewport_height;
	struct svg_style *styles;
	unsigned long visits;
	size_t points;
	size_t open_groups;
	int error;
	const struct svg_node *uses[SVG_USE_DEPTH_MAX];
	size_t use_count;
	struct picture_stops_entry stops_cache[PICTURE_STOPS_CACHE];
	struct picture_bbox_entry bbox_cache[PICTURE_BBOX_CACHE];
};

/* A resolved paint of one shape, with where its stops were stored. */
struct picture_paint_result {
	int drawn;
	struct svg_command_paint paint;
	size_t stop_first;
};

/*
 * Everything one shape adds to the picture, decided before anything is
 * added, so that a shape is added whole or not at all.
 */
struct picture_shape_job {
	struct vec_rect bbox;
	struct vec_matrix matrix;
	int clip;
	float opacity;
	enum vec_fill_rule rule;
	struct picture_paint_result fill;
	struct picture_paint_result stroke_paint;
	int stroke_drawn;
	struct vec_stroke_style stroke;
	float dashes[VEC_DASHES_MAX];
	size_t dash_count;
};

/* Forward declarations of the static functions. */
static void *picture_grow(void *array, size_t *capacity, size_t needed, size_t element_size);
static void picture_fail(struct picture_build *build, int error);
static int picture_name_is(const char *name, const char *expected);
static int picture_visit(struct picture_build *build);
static int picture_room(struct picture_build *build, size_t needed);
static int picture_command_add(struct picture_build *build, const struct svg_command *command);
static int picture_group_open(struct picture_build *build, float opacity, size_t *index);
static void picture_group_close(struct picture_build *build, size_t index);
static int picture_dashes_store(struct picture_build *build, const float *dashes, size_t count, size_t *first);
static int picture_stops_store(struct picture_build *build, const struct svg_node *gradient, const struct vec_stop *stops, size_t count, size_t *first);
static enum picture_kind picture_classify(const char *name, const struct picture_instance *instance);
static int picture_matrix_usable(const struct vec_matrix *matrix);
static void picture_context(const struct picture_build *build, int depth, struct svg_context *context);
static void picture_style(struct picture_build *build, const struct svg_node *node, int depth);
static int picture_is_space(char c);
static int picture_language_holds(const char *text);
static int picture_conditions_hold(const struct picture_build *build, const struct svg_node *node);
static float picture_length_or(const struct svg_tree *tree, const struct svg_node *node, const char *attribute, enum svg_axis axis, const struct svg_context *context, float fallback);
static int picture_size_attribute(const struct svg_tree *tree, const struct svg_node *node, const char *attribute, enum svg_axis axis, const struct svg_context *context, float *size);
static int picture_enter(struct picture_build *build, const struct svg_node *node, int depth, const struct vec_matrix *parent, const struct picture_instance *instance, enum picture_kind *kind, const char **name, struct vec_matrix *matrix);
static int picture_use_circular(struct picture_build *build, const struct svg_node *node, const struct svg_node *target);
static const struct svg_node *picture_use_target(struct picture_build *build, const struct svg_node *node);
static int picture_frame_use(struct picture_build *build, const struct svg_node *node, int depth, const struct vec_matrix *entered, struct picture_frame *frame);
static int picture_frame_viewport(struct picture_build *build, const struct svg_node *node, int depth, const struct picture_instance *instance, struct picture_frame *frame);
static int picture_frame(struct picture_build *build, const struct svg_node *node, enum picture_kind kind, int depth, const struct vec_matrix *entered, const struct picture_instance *instance, struct picture_frame *frame);
static const struct svg_node *picture_switch_child(struct picture_build *build, const struct svg_node *node);
static void picture_walk(struct picture_build *build, const struct svg_node *node, int depth, const struct vec_matrix *parent, int clip, struct picture_box *box, const struct picture_instance *instance);
static void picture_children(struct picture_build *build, const struct svg_node *node, enum picture_kind kind, int depth, const struct vec_matrix *parent, const struct picture_frame *frame, int clip, struct picture_box *box);
static void picture_container(struct picture_build *build, const struct svg_node *node, enum picture_kind kind, int depth, const struct vec_matrix *matrix, int outer_clip, struct picture_box *box, const struct picture_instance *instance);
static void picture_root(struct picture_build *build, const struct svg_node *root, float width, float height);
static void picture_include_box(struct vec_rect *rect, const struct vec_matrix *matrix, const struct vec_rect *box);
static void picture_bbox_shape(struct picture_build *build, const struct svg_node *node, const char *name, int depth, const struct vec_matrix *matrix, struct picture_box *box);
static int picture_instance_same(const struct picture_instance *first, const struct picture_instance *second);
static int picture_bbox_key_same(const struct picture_bbox_entry *entry, const struct picture_bbox_entry *key);
static void picture_container_bbox(struct picture_build *build, const struct svg_node *node, enum picture_kind kind, int depth, const struct picture_instance *instance, struct vec_rect *bbox);
static int picture_clip(struct picture_build *build, const struct svg_node *node, int depth, const struct vec_matrix *matrix, const struct vec_rect *shape_box, enum picture_kind kind, const struct picture_instance *instance, int outer_clip);
static uint32_t picture_fallback_color(const struct svg_paint *paint, const struct svg_style *style, int *drawn);
static void picture_paint_resolve(struct picture_build *build, const struct svg_paint *paint, float opacity, const struct svg_style *style, const struct vec_rect *bbox, const struct svg_context *context, struct picture_paint_result *result);
static int picture_measure(struct picture_build *build, const struct vec_path *path, float *length);
static void picture_dash_setup(struct picture_build *build, const struct svg_node *node, const struct vec_path *path, const struct svg_style *style, const struct svg_context *context, struct picture_shape_job *job);
static void picture_stroke_setup(struct picture_build *build, const struct svg_node *node, const struct vec_path *path, const struct svg_style *style, const struct svg_context *context, float stroke_width, struct picture_shape_job *job);
static void picture_shape_emit(struct picture_build *build, struct vec_path *path, struct picture_shape_job *job);
static void picture_shape(struct picture_build *build, const struct svg_node *node, const char *name, int depth, const struct vec_matrix *matrix, int outer_clip);
static void picture_finish(struct picture_build *build);

/*
 * Makes an empty picture.
 */
void
svg_picture_init(
	struct svg_picture *picture)
{
	/* Refuses a missing picture. */
	if (picture == NULL)
		return;

	/* Clears every field: no size, no command, no memory. */
	memset(picture, 0, sizeof(*picture));
}

/*
 * Frees everything a picture owns and leaves it empty.
 */
void
svg_picture_release(
	struct svg_picture *picture)
{
	size_t index;

	/* Refuses a missing picture. */
	if (picture == NULL)
		return;

	/* Frees the arrays of each path the picture owns. */
	for (index = 0;
	     index < picture->path_count;
	     index++)
		vec_path_release(&picture->paths[index]);

	/* Frees the picture's arrays. */
	free(picture->commands);
	free(picture->paths);
	free(picture->clips);
	free(picture->parts);
	free(picture->stops);
	free(picture->dashes);

	/* Leaves the picture empty. */
	memset(picture, 0, sizeof(*picture));
}

/*
 * Adds a path to a picture, taking its arrays.
 *
 * Whatever the outcome, the path is empty afterwards: on success the
 * picture owns what it held, on an error it is freed here, so a caller
 * has nothing more to release.
 */
int
svg_picture_add_path(
	struct svg_picture *picture,
	struct vec_path *path,
	size_t *index)
{
	void *grown;
	int path_error;

	/* Refuses missing arguments (nothing can be released without them). */
	if (picture == NULL || path == NULL || index == NULL)
		return EINVAL;

	/* Refuses a path that met an error while it was built. */
	path_error = vec_path_error(path);
	if (path_error != 0) {
		vec_path_release(path);
		return EINVAL;
	}

	/* Refuses a path beyond the limit of the picture's paths. */
	if (picture->path_count >= SVG_COMMANDS_MAX) {
		vec_path_release(path);
		return ENOBUFS;
	}

	/* Makes room for one more path. */
	grown = picture_grow(picture->paths, &picture->path_capacity, picture->path_count + 1, sizeof(*picture->paths));
	if (grown == NULL) {
		vec_path_release(path);
		return ENOMEM;
	}
	picture->paths = grown;

	/* Moves the path's arrays into the picture and leaves the caller's path empty. */
	picture->paths[picture->path_count] = *path;
	*index = picture->path_count;
	picture->path_count++;
	vec_path_init(path);

	/* Succeeded: the picture owns the path at *index. */
	return 0;
}

/*
 * Builds the picture of an 'svg' element drawn into a viewport.
 *
 * Values referenced: SVG 2 §3.2 (the rendering tree), §5.1 and §5.2
 * (svg and g), §5.4 (symbol), §5.5 (use, §5.5.1 circular references,
 * §5.5.2 the translation of x and y and the size of a referenced
 * viewport), §5.6 (switch and the conditional processing attributes),
 * §8.2 (the viewBox transform), §8.6 (a zero viewBox), §8.8 (a new
 * viewport), §8.10 (the bounding box of a container), §9.6.1
 * (pathLength), §13.2 (paint and its fallback), §13.8 (fill, then
 * stroke), §3.6.1 (opacity on a group), §3.2.3 (display and
 * visibility), CSS Transforms 1 §7.1 (a non-invertible matrix hides the
 * element and its content).
 *
 * Choices where the header is silent: the 'a' element is drawn as a
 * container like 'g' (SVG 2 §5.2's list of container elements); the
 * root's transform attribute is applied outside its viewBox transform
 * (SVG 2 §8.2); an element with a matrix that cannot be inverted draws
 * nothing; elements below SVG_DEPTH_MAX are skipped without an error.
 * A nested 'svg' applies its transform outside its x, y and viewBox
 * placement, like the root; a negative width or height of an 'svg',
 * 'symbol' or 'use' is ignored as if absent, and a zero one draws
 * nothing; a 'symbol' has no transform attribute and its refX and refY
 * are not used.  A 'use' is circular when its target is the 'use', an
 * ancestor of it, or an ancestor of a 'use' being expanded (not only
 * when it is nested too deeply).  The conditional processing attributes
 * hold for every element, not only the children of a 'switch': an
 * element they exclude draws nothing, but can still be referenced.  The
 * box of a container is kept for a container met again with the same
 * viewport, font size, nesting and size from a 'use'.
 */
int
svg_picture_build(
	const struct svg_tree *tree,
	const struct svg_node *root,
	const struct svg_style *outer,
	float width,
	float height,
	float root_font_size,
	struct svg_picture *picture)
{
	struct picture_build *build;
	const char *name;
	int finite_width;
	int finite_height;
	int finite_font;
	int is_svg;
	int error;

	/* Refuses a missing picture (it cannot be emptied without its pointer). */
	if (picture == NULL)
		return EINVAL;

	/* Releases the old contents first, so every failure below leaves the picture empty. */
	svg_picture_release(picture);

	/* Refuses a missing tree, root or table of functions. */
	if (tree == NULL || root == NULL)
		return EINVAL;
	if (tree->name == NULL || tree->attribute == NULL || tree->first_child == NULL || tree->next_sibling == NULL)
		return EINVAL;

	/* Refuses a root that is not an 'svg' element. */
	name = tree->name(tree->context, root);
	is_svg = picture_name_is(name, "svg");
	if (!is_svg)
		return EINVAL;

	/* Refuses a viewport that is not finite, positive and within the coordinate range. */
	finite_width = vec_finite(width);
	finite_height = vec_finite(height);
	if (!finite_width || !finite_height)
		return EINVAL;
	if (width <= 0.0f || height <= 0.0f)
		return EINVAL;
	if (width > VEC_COORD_MAX || height > VEC_COORD_MAX)
		return EINVAL;

	/* Allocates the state of the build. */
	build = calloc(1, sizeof(*build));
	if (build == NULL)
		return ENOMEM;

	/* Allocates the styles of the chain from the root: one per depth and the base one. */
	build->styles = calloc((size_t)SVG_DEPTH_MAX + 1, sizeof(*build->styles));
	if (build->styles == NULL) {
		free(build);
		return ENOMEM;
	}

	/* Fills the state: the tree, the picture, the root's font size and the viewport. */
	build->tree = tree;
	build->outer = outer;
	build->picture = picture;
	build->root_font_size = root_font_size;
	finite_font = vec_finite(root_font_size);
	if (!finite_font || root_font_size <= 0.0f)
		build->root_font_size = PICTURE_ROOT_FONT_SIZE_DEFAULT;
	build->viewport_width = width;
	build->viewport_height = height;
	picture->width = width;
	picture->height = height;

	/* Makes the style the root inherits: the page's outer style, or the initial one. */
	if (outer != NULL)
		build->styles[0] = *outer;
	else
		svg_style_initial(&build->styles[0]);

	/* Walks the root and everything it draws. */
	picture_root(build, root, width, height);

	/* Points the commands into the picture's arrays, unless the picture is to be dropped. */
	error = build->error;
	if (error == 0 || error == ENOBUFS)
		picture_finish(build);

	/* Gives the state back. */
	free(build->styles);
	free(build);

	/* Drops a picture that ran out of memory: it must be empty. */
	if (error == ENOMEM) {
		svg_picture_release(picture);
		return ENOMEM;
	}

	/* Reports a limit that was reached (the picture holds what was built before it). */
	if (error != 0)
		return error;

	/* Succeeded: the picture holds the commands. */
	return 0;
}

/*
 * Grows an array so that it holds at least needed elements, doubling its
 * capacity.  Returns the new array (the old one is then invalid), or the
 * old one when it already has room, or NULL when memory or the size
 * arithmetic fails (the old array and *capacity are then unchanged).
 */
static void *
picture_grow(
	void *array,
	size_t *capacity,
	size_t needed,
	size_t element_size)
{
	void *grown;
	size_t new_capacity;

	/* Keeps the array when it already has room. */
	if (needed <= *capacity)
		return array;

	/* Starts from the current capacity, or the initial one for an empty array. */
	new_capacity = *capacity;
	if (new_capacity == 0)
		new_capacity = PICTURE_ARRAY_INITIAL;

	/* Doubles the capacity until it holds what is needed, without overflow. */
	while (new_capacity < needed) {
		if (new_capacity > SIZE_MAX / 2)
			return NULL;
		new_capacity *= 2;
	}

	/* Refuses a byte size that overflows. */
	if (new_capacity > SIZE_MAX / element_size)
		return NULL;

	/* Reallocates the array to the new capacity. */
	grown = realloc(array, new_capacity * element_size);
	if (grown == NULL)
		return NULL;
	*capacity = new_capacity;

	/* Succeeded: the array holds the new capacity. */
	return grown;
}

/*
 * Records the first error of a build; ENOMEM replaces ENOBUFS (it drops
 * the picture), and nothing replaces ENOMEM.
 */
static void
picture_fail(
	struct picture_build *build,
	int error)
{
	/* Keeps ENOMEM whatever comes after it. */
	if (build->error == ENOMEM)
		return;

	/* Records the error when it is the first, or when it is ENOMEM. */
	if (build->error == 0 || error == ENOMEM)
		build->error = error;
}

/*
 * Tells whether an element name (NULL for a node that is not an SVG
 * element) is exactly the expected one.
 */
static int
picture_name_is(
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

/*
 * Counts one element visit against SVG_VISITS_MAX.  Returns 1 when the
 * walk may go on, 0 when it must stop (an error was recorded before, or
 * this visit passed the limit, which records ENOBUFS).
 */
static int
picture_visit(
	struct picture_build *build)
{
	/* Stops once an error was recorded: the walk unwinds without visiting more. */
	if (build->error != 0)
		return 0;

	/* Counts the visit. */
	build->visits++;

	/* Stops at the limit of visits that bounds the work whatever the document. */
	if (build->visits > SVG_VISITS_MAX) {
		picture_fail(build, ENOBUFS);
		return 0;
	}

	/* Succeeded: the walk may enter the element. */
	return 1;
}

/*
 * Tells whether needed more commands fit: the commands in the picture,
 * the GROUP_END every open group still owes and the new ones together stay
 * within SVG_COMMANDS_MAX.  Records ENOBUFS and returns 0 when they do not.
 */
static int
picture_room(
	struct picture_build *build,
	size_t needed)
{
	size_t used;

	/* Counts the commands present and the ends owed by the open groups. */
	used = build->picture->command_count + build->open_groups;

	/* Refuses when the new commands would pass the limit. */
	if (used > SVG_COMMANDS_MAX || needed > SVG_COMMANDS_MAX - used) {
		picture_fail(build, ENOBUFS);
		return 0;
	}

	/* Succeeded: the commands fit. */
	return 1;
}

/*
 * Appends one command to the picture (the caller made sure it fits the
 * limit with picture_room).  Records ENOMEM and returns 0 when memory is
 * short, else returns 1.
 */
static int
picture_command_add(
	struct picture_build *build,
	const struct svg_command *command)
{
	struct svg_picture *picture;
	void *grown;

	/* Makes room for one more command. */
	picture = build->picture;
	grown = picture_grow(picture->commands, &picture->command_capacity, picture->command_count + 1, sizeof(*picture->commands));
	if (grown == NULL) {
		picture_fail(build, ENOMEM);
		return 0;
	}
	picture->commands = grown;

	/* Stores the command. */
	picture->commands[picture->command_count] = *command;
	picture->command_count++;

	/* Succeeded: the command is the last of the picture. */
	return 1;
}

/*
 * Starts a group of the given opacity.  The caller checked that two
 * commands fit (the group and its end); *index is the group command's
 * index, which picture_group_close takes.  Returns 1 when the group was
 * started, 0 on a memory error.
 */
static int
picture_group_open(
	struct picture_build *build,
	float opacity,
	size_t *index)
{
	struct svg_command command;
	int added;

	/* Writes the group command: no path, no paint, no clip (the commands inside carry theirs). */
	memset(&command, 0, sizeof(command));
	command.kind = SVG_COMMAND_GROUP;
	vec_matrix_identity(&command.matrix);
	command.clip = -1;
	command.opacity = opacity;

	/* Appends it and remembers where it is. */
	*index = build->picture->command_count;
	added = picture_command_add(build, &command);
	if (!added)
		return 0;

	/* Holds back one command of the limit for the group's end. */
	build->open_groups++;

	/* Succeeded: the group is open. */
	return 1;
}

/*
 * Ends the group started at index.  A group with nothing inside draws
 * nothing, so its command is taken back instead of ending it; otherwise
 * GROUP_END is appended, which always fits (the limit held it back).
 */
static void
picture_group_close(
	struct picture_build *build,
	size_t index)
{
	struct svg_command command;

	/* Gives the held-back command back to the limit. */
	build->open_groups--;

	/* Takes back a group with no command inside. */
	if (build->picture->command_count == index + 1) {
		build->picture->command_count = index;
		return;
	}

	/* Writes the end command. */
	memset(&command, 0, sizeof(command));
	command.kind = SVG_COMMAND_GROUP_END;
	vec_matrix_identity(&command.matrix);
	command.clip = -1;
	command.opacity = 1.0f;

	/* Appends it (a memory error is recorded by the append). */
	picture_command_add(build, &command);
}

/*
 * Appends dash lengths to the picture's dashes; *first is the index of
 * the first.  Returns 1, or 0 on a memory error (recorded).
 */
static int
picture_dashes_store(
	struct picture_build *build,
	const float *dashes,
	size_t count,
	size_t *first)
{
	struct svg_picture *picture;
	void *grown;

	/* Stores nothing for no dashes. */
	picture = build->picture;
	*first = picture->dash_count;
	if (count == 0)
		return 1;

	/* Makes room for the dashes. */
	if (count > SIZE_MAX - picture->dash_count) {
		picture_fail(build, ENOMEM);
		return 0;
	}
	grown = picture_grow(picture->dashes, &picture->dash_capacity, picture->dash_count + count, sizeof(*picture->dashes));
	if (grown == NULL) {
		picture_fail(build, ENOMEM);
		return 0;
	}
	picture->dashes = grown;

	/* Copies the dash lengths. */
	memcpy(picture->dashes + picture->dash_count, dashes, count * sizeof(*dashes));
	picture->dash_count += count;

	/* Succeeded: the dashes are in the picture from *first. */
	return 1;
}

/*
 * Appends the stops of a gradient to the picture's stops, unless the same
 * stops of the same gradient element are already there (the cache), and
 * reports where they are in *first.  Returns 1, or 0 on a memory error
 * (recorded).
 */
static int
picture_stops_store(
	struct picture_build *build,
	const struct svg_node *gradient,
	const struct vec_stop *stops,
	size_t count,
	size_t *first)
{
	struct svg_picture *picture;
	struct picture_stops_entry *entry;
	void *grown;
	size_t slot;
	int same;

	/* Finds the cache entry of the element by its address. */
	picture = build->picture;
	slot = ((size_t)(uintptr_t)gradient >> 4) % PICTURE_STOPS_CACHE;
	entry = &build->stops_cache[slot];

	/* Reuses the stored stops when the same element's stops are identical. */
	if (entry->node == gradient && entry->count == count && entry->first + count <= picture->stop_count) {
		same = memcmp(picture->stops + entry->first, stops, count * sizeof(*stops)) == 0;
		if (same) {
			*first = entry->first;
			return 1;
		}
	}

	/* Makes room for the stops. */
	if (count > SIZE_MAX - picture->stop_count) {
		picture_fail(build, ENOMEM);
		return 0;
	}
	grown = picture_grow(picture->stops, &picture->stop_capacity, picture->stop_count + count, sizeof(*picture->stops));
	if (grown == NULL) {
		picture_fail(build, ENOMEM);
		return 0;
	}
	picture->stops = grown;

	/* Copies the stops and remembers where they are. */
	memcpy(picture->stops + picture->stop_count, stops, count * sizeof(*stops));
	*first = picture->stop_count;
	entry->node = gradient;
	entry->first = picture->stop_count;
	entry->count = count;
	picture->stop_count += count;

	/* Succeeded: the stops are in the picture from *first. */
	return 1;
}

/*
 * Tells what the walk does with an element of the given name: shapes are
 * drawn, 'g' and 'a' are containers (SVG 2 §5.2), a nested 'svg', a
 * 'use' and a 'switch' are containers of their own kind (§5.1, §5.5,
 * §5.6), a 'symbol' is one only as the referenced element of a 'use'
 * (instance not NULL; anywhere else it is a definition, §5.4), and
 * everything else is not drawn with its subtree ('defs', 'clipPath',
 * gradients, text, images ...).
 */
static enum picture_kind
picture_classify(
	const char *name,
	const struct picture_instance *instance)
{
	int is_shape;
	int is_group;
	int is_link;
	int is_svg;
	int is_use;
	int is_switch;
	int is_symbol;

	/* Takes a group as a container. */
	is_group = picture_name_is(name, "g");
	if (is_group)
		return PICTURE_KIND_CONTAINER;

	/* Takes a link as a container (it renders as a group). */
	is_link = picture_name_is(name, "a");
	if (is_link)
		return PICTURE_KIND_CONTAINER;

	/* Takes a shape element as a shape. */
	is_shape = svg_shape_is(name);
	if (is_shape)
		return PICTURE_KIND_SHAPE;

	/* Takes a nested 'svg' as a container with a viewport. */
	is_svg = picture_name_is(name, "svg");
	if (is_svg)
		return PICTURE_KIND_SVG;

	/* Takes a 'use' as a container of the element it references. */
	is_use = picture_name_is(name, "use");
	if (is_use)
		return PICTURE_KIND_USE;

	/* Takes a 'switch' as a container of the child it chooses. */
	is_switch = picture_name_is(name, "switch");
	if (is_switch)
		return PICTURE_KIND_SWITCH;

	/* Takes a 'symbol' as a container only when a 'use' references it. */
	is_symbol = picture_name_is(name, "symbol");
	if (is_symbol && instance != NULL)
		return PICTURE_KIND_SYMBOL;

	/* Reports an element that is not drawn. */
	return PICTURE_KIND_SKIP;
}

/*
 * Tells whether a matrix can carry drawing: its entries are within the
 * coordinate range and it can be inverted (determinant not zero), as CSS
 * Transforms 1 §7.1 hides what a non-invertible matrix maps.
 */
static int
picture_matrix_usable(
	const struct vec_matrix *matrix)
{
	float determinant;
	int valid;
	int finite;

	/* Refuses entries beyond the range. */
	valid = vec_matrix_valid(matrix);
	if (!valid)
		return 0;

	/* Refuses a matrix that flattens the plane (determinant 0 or not finite). */
	determinant = matrix->a * matrix->d - matrix->b * matrix->c;
	finite = vec_finite(determinant);
	if (!finite || determinant == 0.0f)
		return 0;

	/* Succeeded: the matrix maps the plane onto itself. */
	return 1;
}

/*
 * Makes the context lengths of the element at a depth resolve against:
 * the viewport, its font size and the root's.
 */
static void
picture_context(
	const struct picture_build *build,
	int depth,
	struct svg_context *context)
{
	/* Fills the four numbers a length needs. */
	context->viewport_width = build->viewport_width;
	context->viewport_height = build->viewport_height;
	context->font_size = build->styles[depth].font_size;
	context->root_font_size = build->root_font_size;
}

/*
 * Computes the style of the element at a depth from the one above it: the
 * inherited properties, then the element's attributes (SVG 2 §6.6).
 */
static void
picture_style(
	struct picture_build *build,
	const struct svg_node *node,
	int depth)
{
	struct svg_style *style;
	const struct svg_style *parent;

	/* Starts from the parent's inherited properties. */
	style = &build->styles[depth];
	parent = &build->styles[depth - 1];
	svg_style_inherit(style, parent);

	/* Applies the presentation attributes and the style attribute. */
	svg_style_apply(style, parent, build->tree, node);
}

/*
 * Tells whether a character is one of the white space characters the
 * attribute lists here are separated by (HTML's ASCII white space).
 */
static int
picture_is_space(
	char c)
{
	/* Takes a space, tab, line feed, form feed or carriage return as white space. */
	if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r')
		return 1;

	/* Reports anything else as not white space. */
	return 0;
}

/*
 * Tells whether a systemLanguage value holds for the language this
 * renderer assumes, English (SVG 2 §5.6.5): one of its comma-separated
 * tags, trimmed of white space, is "en" or starts with "en-" (ASCII case
 * does not matter).  Empty tags do not count, so an empty value does not
 * hold.
 */
static int
picture_language_holds(
	const char *text)
{
	const char *cursor;
	const char *start;
	const char *end;
	size_t length;
	char first;
	char second;
	int space;
	int done;

	/* Looks at each comma-separated tag in turn. */
	cursor = text;
	done = 0;
	while (!done) {
		/* Finds the end of the tag: the next comma or the end of the text. */
		start = cursor;
		while (*cursor != '\0' && *cursor != ',')
			cursor++;
		end = cursor;

		/* Trims the white space before the tag. */
		while (start < end) {
			space = picture_is_space(*start);
			if (!space)
				break;
			start++;
		}

		/* Trims the white space after the tag. */
		while (end > start) {
			space = picture_is_space(end[-1]);
			if (!space)
				break;
			end--;
		}
		length = (size_t)(end - start);

		/* Takes a tag that is "en", or "en" followed by a hyphen and more. */
		if (length >= 2) {
			first = start[0];
			second = start[1];
			if (first == 'E')
				first = 'e';
			if (second == 'N')
				second = 'n';
			if (first == 'e' && second == 'n') {
				if (length == 2)
					return 1;
				if (start[2] == '-')
					return 1;
			}
		}

		/* Goes on after the comma, or ends at the end of the text. */
		if (*cursor == '\0')
			done = 1;
		else
			cursor++;
	}

	/* No tag matched. */
	return 0;
}

/*
 * Tells whether the conditional processing attributes of an element hold
 * (SVG 2 §5.6.1): requiredExtensions holds only when absent, as no
 * extension is supported and an empty value is false; systemLanguage
 * holds when absent or when it names English; requiredFeatures is not
 * looked at (SVG 2 dropped it).  An element for which they do not hold is
 * treated as display none.
 */
static int
picture_conditions_hold(
	const struct picture_build *build,
	const struct svg_node *node)
{
	const struct svg_tree *tree;
	const char *text;
	int holds;

	/* Fails an element that requires extensions: none is supported. */
	tree = build->tree;
	text = tree->attribute(tree->context, node, "requiredExtensions");
	if (text != NULL)
		return 0;

	/* Holds for an element with no systemLanguage. */
	text = tree->attribute(tree->context, node, "systemLanguage");
	if (text == NULL)
		return 1;

	/* Tests the languages the element lists against English. */
	holds = picture_language_holds(text);

	/* Reports whether the element's languages include English. */
	return holds;
}

/*
 * Reads a length attribute of an element and resolves it to user units
 * against a context.  Reports the fallback when the attribute is absent
 * or is not a length.
 */
static float
picture_length_or(
	const struct svg_tree *tree,
	const struct svg_node *node,
	const char *attribute,
	enum svg_axis axis,
	const struct svg_context *context,
	float fallback)
{
	struct svg_length length;
	const char *text;
	float value;
	int error;

	/* Takes the fallback for an attribute that is not there. */
	text = tree->attribute(tree->context, node, attribute);
	if (text == NULL)
		return fallback;

	/* Takes the fallback for a value that is not a length. */
	error = svg_parse_length(text, &length);
	if (error != 0)
		return fallback;

	/* Resolves the length against the viewport and the font. */
	value = svg_length_resolve(&length, axis, context);

	/* Reports the length in user units. */
	return value;
}

/*
 * Reads a width or height of an element: reports 1 with *size when the
 * attribute is a length that is not negative (a negative size is invalid
 * and ignored, SVG 2 §5.5), 0 when it is absent, invalid or negative.
 */
static int
picture_size_attribute(
	const struct svg_tree *tree,
	const struct svg_node *node,
	const char *attribute,
	enum svg_axis axis,
	const struct svg_context *context,
	float *size)
{
	struct svg_length length;
	const char *text;
	float value;
	int error;

	/* Reports a size that is not there. */
	text = tree->attribute(tree->context, node, attribute);
	if (text == NULL)
		return 0;

	/* Reports a value that is not a length. */
	error = svg_parse_length(text, &length);
	if (error != 0)
		return 0;

	/* Resolves the length and refuses a negative one. */
	value = svg_length_resolve(&length, axis, context);
	if (value < 0.0f)
		return 0;

	/* Succeeded: the attribute gives the size. */
	*size = value;
	return 1;
}

/*
 * Enters an element on the way down (both walks do): counts the visit,
 * refuses what is deeper than SVG_DEPTH_MAX or not drawn or excluded by
 * conditional processing, computes the element's style (at styles[depth])
 * and refuses display none (not for a 'symbol' a 'use' instantiates:
 * its display is always inline, SVG 2 §5.4.2), then composes its
 * transform attribute (a 'symbol' has none) onto the parent's matrix.
 *
 * instance is not NULL for the element a 'use' references.  Returns 1
 * when the element is to be processed, with its kind, name and matrix
 * (user space to the walk's space) set; 0 when it and its subtree are
 * skipped (or the walk must stop, build->error set).
 */
static int
picture_enter(
	struct picture_build *build,
	const struct svg_node *node,
	int depth,
	const struct vec_matrix *parent,
	const struct picture_instance *instance,
	enum picture_kind *kind,
	const char **name,
	struct vec_matrix *matrix)
{
	const struct svg_tree *tree;
	const char *text;
	struct vec_matrix local;
	int counted;
	int holds;
	int usable;
	int error;

	/* Counts the visit against the limit. */
	counted = picture_visit(build);
	if (!counted)
		return 0;

	/* Skips what lies deeper than the limit of nesting. */
	if (depth > SVG_DEPTH_MAX)
		return 0;

	/* Skips a node that is not an element of the SVG namespace (text, other namespaces). */
	tree = build->tree;
	*name = tree->name(tree->context, node);
	if (*name == NULL)
		return 0;

	/* Skips an element that is not drawn, with its subtree. */
	*kind = picture_classify(*name, instance);
	if (*kind == PICTURE_KIND_SKIP)
		return 0;

	/* Skips an element that conditional processing excludes (SVG 2 §5.6.1). */
	holds = picture_conditions_hold(build, node);
	if (!holds)
		return 0;

	/* Computes the element's style. */
	picture_style(build, node, depth);

	/* Skips display none: the element and its subtree are not in the rendering tree (SVG 2 §3.2.3), but an instantiated symbol is always shown. */
	if (*kind != PICTURE_KIND_SYMBOL && build->styles[depth].display == SVG_DISPLAY_NONE)
		return 0;

	/* Reads the transform attribute (a symbol has none); an invalid one is ignored. */
	vec_matrix_identity(&local);
	if (*kind != PICTURE_KIND_SYMBOL) {
		text = tree->attribute(tree->context, node, "transform");
		if (text != NULL) {
			error = svg_parse_transform(text, &local);
			if (error != 0)
				vec_matrix_identity(&local);
		}
	}

	/* Composes it: the element's own transform applies first, then the parent's. */
	vec_matrix_multiply(matrix, &local, parent);

	/* Skips an element whose matrix cannot carry drawing. */
	usable = picture_matrix_usable(matrix);
	if (!usable)
		return 0;

	/* Succeeded: the element is entered. */
	return 1;
}

/*
 * Tells whether a 'use' that would draw target is circular (SVG 2 §5.5.1):
 * the target is the 'use' itself or an ancestor of it, or of one of the
 * 'use' elements being expanded (the target would then contain its own
 * instance).  Every ancestor looked at counts as a visit, so that a deep
 * or malformed tree cannot make the check endless; when the visits run
 * out the check reports a circle and the build has stopped.
 */
static int
picture_use_circular(
	struct picture_build *build,
	const struct svg_node *node,
	const struct svg_node *target)
{
	const struct svg_tree *tree;
	const struct svg_node *host;
	const struct svg_node *ancestor;
	size_t index;
	int counted;

	/* Looks at the use and at each use being expanded, outermost last. */
	tree = build->tree;
	for (index = 0;
	     index <= build->use_count;
	     index++) {
		host = node;
		if (index > 0)
			host = build->uses[index - 1];

		/* Looks at the host and each of its ancestors for the target. */
		for (ancestor = host;
		     ancestor != NULL;
		     ancestor = tree->parent(tree->context, ancestor)) {
			counted = picture_visit(build);
			if (!counted)
				return 1;
			if (ancestor == target)
				return 1;
		}
	}

	/* The target is not in the chain: it can be drawn. */
	return 0;
}

/*
 * Finds the element a 'use' references, or NULL when the 'use' draws
 * nothing: nested too deeply (SVG_USE_DEPTH_MAX), no href, an href that
 * is not "#id" (another document is not loaded), an id that names no
 * element or an element that is not in the SVG namespace (SVG 2 §5.5:
 * an invalid reference is not rendered), or a circular reference.
 */
static const struct svg_node *
picture_use_target(
	struct picture_build *build,
	const struct svg_node *node)
{
	const struct svg_tree *tree;
	const struct svg_node *target;
	const char *text;
	const char *cursor;
	const char *name;
	char id[SVG_ID_MAX + 1];
	int circular;
	int space;
	int error;

	/* Refuses a nesting of uses beyond the limit, which draws nothing as a cycle does. */
	if (build->use_count >= SVG_USE_DEPTH_MAX)
		return NULL;

	/* Refuses a tree that cannot find an element by id or an element's parent. */
	tree = build->tree;
	if (tree->find_id == NULL || tree->parent == NULL)
		return NULL;

	/* Reads href (the tree gives xlink:href when there is no href). */
	text = tree->attribute(tree->context, node, "href");
	if (text == NULL)
		return NULL;

	/* Reads the reference: only an id in this document is supported. */
	cursor = text;
	error = svg_url_read(&cursor, 1, id);
	if (error != 0)
		return NULL;

	/* Refuses anything but white space after the id. */
	space = picture_is_space(*cursor);
	while (space) {
		cursor++;
		space = picture_is_space(*cursor);
	}
	if (*cursor != '\0')
		return NULL;

	/* Finds the element the id names. */
	target = tree->find_id(tree->context, id);
	if (target == NULL)
		return NULL;

	/* Refuses an element that is not in the SVG namespace. */
	name = tree->name(tree->context, target);
	if (name == NULL)
		return NULL;

	/* Refuses a reference to the use itself or to an ancestor of it. */
	circular = picture_use_circular(build, node, target);
	if (circular)
		return NULL;

	/* Succeeded: the use draws this element. */
	return target;
}

/*
 * Places the content of a 'use': the matrix is the use's transform then
 * translate(x, y) (SVG 2 §5.5.2: appended on the right of the use's
 * transform list, so a point is moved first), which is the use's own
 * space and the one its clip-path is in; the referenced element is its
 * only child, and the use's width and height go to it.  Returns 1, or 0
 * when the use draws nothing.
 */
static int
picture_frame_use(
	struct picture_build *build,
	const struct svg_node *node,
	int depth,
	const struct vec_matrix *entered,
	struct picture_frame *frame)
{
	const struct svg_tree *tree;
	struct svg_context context;
	struct vec_matrix offset;
	float x;
	float y;
	float size;
	int has_size;
	int usable;
	int error;

	/* Finds the element the use references. */
	frame->target = picture_use_target(build, node);
	if (frame->target == NULL)
		return 0;

	/* Resolves x and y against the viewport the use is in. */
	tree = build->tree;
	picture_context(build, depth, &context);
	x = picture_length_or(tree, node, "x", SVG_AXIS_X, &context, 0.0f);
	y = picture_length_or(tree, node, "y", SVG_AXIS_Y, &context, 0.0f);

	/* Makes the use's own space: the translation applies to a point first, then the use's matrix. */
	error = vec_matrix_translate(&offset, x, y);
	if (error != 0)
		return 0;
	vec_matrix_multiply(&frame->own, &offset, entered);
	usable = picture_matrix_usable(&frame->own);
	if (!usable)
		return 0;

	/* The referenced element is drawn in that space. */
	frame->content = frame->own;

	/* Takes the width, when the use has a valid one, for a referenced 'svg' or 'symbol'. */
	has_size = picture_size_attribute(tree, node, "width", SVG_AXIS_X, &context, &size);
	if (has_size) {
		frame->instance.has_width = 1;
		frame->instance.width = size;
	}

	/* Takes the height likewise. */
	has_size = picture_size_attribute(tree, node, "height", SVG_AXIS_Y, &context, &size);
	if (has_size) {
		frame->instance.has_height = 1;
		frame->instance.height = size;
	}

	/* Succeeded: the use has a place for its referenced element. */
	return 1;
}

/*
 * Places the content of a nested 'svg' or a 'symbol' a 'use' instantiates,
 * whose own space (frame->own) is already set (SVG 2 §5.1, §5.4, §8.2, §8.8): a viewport at x, y of width by height
 * (100% of the viewport it is in when a size is absent, and a width or
 * height of the 'use' replaces the element's own, §5.5.2), with the
 * viewBox and preserveAspectRatio fitting its content to it.  The content
 * then resolves percentages against the viewBox's size, or the viewport's
 * without a viewBox.  An invalid viewBox is ignored; a zero width or
 * height of the viewport or of the viewBox draws nothing (§8.6).  Returns
 * 1, or 0 when nothing is drawn.
 */
static int
picture_frame_viewport(
	struct picture_build *build,
	const struct svg_node *node,
	int depth,
	const struct picture_instance *instance,
	struct picture_frame *frame)
{
	const struct svg_tree *tree;
	struct svg_context context;
	struct vec_rect view_box;
	struct svg_aspect aspect;
	const char *text;
	float x;
	float y;
	float width;
	float height;
	int has_size;
	int has_view_box;
	int usable;
	int error;

	/* Resolves x and y against the viewport the element is in. */
	tree = build->tree;
	picture_context(build, depth, &context);
	x = picture_length_or(tree, node, "x", SVG_AXIS_X, &context, 0.0f);
	y = picture_length_or(tree, node, "y", SVG_AXIS_Y, &context, 0.0f);

	/* Takes the width: the use's, else the element's own, else all of the viewport. */
	width = build->viewport_width;
	if (instance != NULL && instance->has_width) {
		width = instance->width;
	} else {
		has_size = picture_size_attribute(tree, node, "width", SVG_AXIS_X, &context, &width);
		if (!has_size)
			width = build->viewport_width;
	}

	/* Takes the height the same way. */
	height = build->viewport_height;
	if (instance != NULL && instance->has_height) {
		height = instance->height;
	} else {
		has_size = picture_size_attribute(tree, node, "height", SVG_AXIS_Y, &context, &height);
		if (!has_size)
			height = build->viewport_height;
	}

	/* Draws nothing for a viewport with no width or height (SVG 2 §5.5). */
	if (width <= 0.0f || height <= 0.0f)
		return 0;

	/* Reads the viewBox; an invalid one counts as absent (§8.6). */
	has_view_box = 0;
	text = tree->attribute(tree->context, node, "viewBox");
	if (text != NULL) {
		error = svg_parse_view_box(text, &view_box);
		if (error == 0)
			has_view_box = 1;
	}

	/* Makes the matrix of the content into the viewport, and the size percentages resolve against. */
	frame->viewport_width = width;
	frame->viewport_height = height;
	if (has_view_box) {
		/* Draws nothing for a viewBox without width or height (§8.6). */
		if (view_box.right - view_box.left <= 0.0f || view_box.bottom - view_box.top <= 0.0f)
			return 0;

		/* Reads preserveAspectRatio; an invalid one leaves the default xMidYMid meet. */
		aspect.align = SVG_ALIGN_XMID_YMID;
		aspect.slice = 0;
		text = tree->attribute(tree->context, node, "preserveAspectRatio");
		if (text != NULL)
			svg_parse_aspect(text, &aspect);

		/* Fits the viewBox to the viewport at x, y. */
		error = svg_view_box_transform(&view_box, &aspect, x, y, width, height, &frame->inner);
		if (error != 0)
			return 0;
		frame->viewport_width = view_box.right - view_box.left;
		frame->viewport_height = view_box.bottom - view_box.top;
	} else {
		/* Without a viewBox the content's origin is the viewport's corner. */
		error = vec_matrix_translate(&frame->inner, x, y);
		if (error != 0)
			return 0;
	}

	/* Places the content: its own matrix first, then the element's. */
	vec_matrix_multiply(&frame->content, &frame->inner, &frame->own);
	usable = picture_matrix_usable(&frame->content);
	if (!usable)
		return 0;

	/* Succeeded: the content has a viewport. */
	return 1;
}

/*
 * Places the content of a container that was entered with the matrix of
 * its transform: a 'use' and the elements with a viewport have their own
 * (above), the others draw their children in their own space.  Returns 1,
 * or 0 when the container draws nothing.
 */
static int
picture_frame(
	struct picture_build *build,
	const struct svg_node *node,
	enum picture_kind kind,
	int depth,
	const struct vec_matrix *entered,
	const struct picture_instance *instance,
	struct picture_frame *frame)
{
	int placed;

	/* Starts with the children in the container's own space and its viewport. */
	frame->own = *entered;
	vec_matrix_identity(&frame->inner);
	frame->content = *entered;
	frame->viewport_width = build->viewport_width;
	frame->viewport_height = build->viewport_height;
	frame->target = NULL;
	memset(&frame->instance, 0, sizeof(frame->instance));

	/* Places the referenced element of a 'use'. */
	if (kind == PICTURE_KIND_USE) {
		placed = picture_frame_use(build, node, depth, entered, frame);
		return placed;
	}

	/* Places the content of a viewport. */
	if (kind == PICTURE_KIND_SVG || kind == PICTURE_KIND_SYMBOL) {
		placed = picture_frame_viewport(build, node, depth, instance, frame);
		return placed;
	}

	/* Succeeded: the other containers draw in their own space. */
	return 1;
}

/*
 * Finds the child a 'switch' draws (SVG 2 §5.6.3): the first of its child
 * elements in the SVG namespace whose conditional processing attributes
 * hold; NULL when none does or the visits ran out.  Each child looked at
 * counts as a visit.  The child chosen need not be something this
 * builder draws (text, for instance), and then nothing is drawn.
 */
static const struct svg_node *
picture_switch_child(
	struct picture_build *build,
	const struct svg_node *node)
{
	const struct svg_tree *tree;
	const struct svg_node *child;
	const char *name;
	int counted;
	int holds;

	/* Looks at each child in order for the first one whose conditions hold. */
	tree = build->tree;
	for (child = tree->first_child(tree->context, node);
	     child != NULL;
	     child = tree->next_sibling(tree->context, child)) {
		counted = picture_visit(build);
		if (!counted)
			return NULL;

		/* Passes over a node that is not an element of the SVG namespace. */
		name = tree->name(tree->context, child);
		if (name == NULL)
			continue;

		/* Chooses the first element whose conditional processing attributes hold. */
		holds = picture_conditions_hold(build, child);
		if (holds)
			return child;
	}

	/* No child is chosen. */
	return NULL;
}

/*
 * Walks one element below a container (or a bounding box walk of it): a
 * shape is drawn or its box collected, a container draws its children.
 * box is NULL to draw, else the box to collect; instance is not NULL for
 * the element a 'use' references.
 */
static void
picture_walk(
	struct picture_build *build,
	const struct svg_node *node,
	int depth,
	const struct vec_matrix *parent,
	int clip,
	struct picture_box *box,
	const struct picture_instance *instance)
{
	enum picture_kind kind;
	const char *name;
	struct vec_matrix matrix;
	int entered;

	/* Enters the element, or skips it. */
	entered = picture_enter(build, node, depth, parent, instance, &kind, &name, &matrix);
	if (!entered)
		return;

	/* Draws a shape, or adds its box. */
	if (kind == PICTURE_KIND_SHAPE) {
		if (box == NULL)
			picture_shape(build, node, name, depth, &matrix, clip);
		else
			picture_bbox_shape(build, node, name, depth, &matrix, box);
		return;
	}

	/* Draws a container through its children. */
	picture_container(build, node, kind, depth, &matrix, clip, box, instance);
}

/*
 * Walks what a container contains, with its children placed by parent: all
 * its children, or for a 'use' the element it references, or for a
 * 'switch' the child it chooses.  The viewport the frame gives is the one
 * percentages resolve against below, and a 'use' is on the list of those
 * being expanded, until the walk returns.
 */
static void
picture_children(
	struct picture_build *build,
	const struct svg_node *node,
	enum picture_kind kind,
	int depth,
	const struct vec_matrix *parent,
	const struct picture_frame *frame,
	int clip,
	struct picture_box *box)
{
	const struct svg_tree *tree;
	const struct svg_node *child;
	float saved_width;
	float saved_height;

	/* Resolves the children's lengths against the container's viewport. */
	saved_width = build->viewport_width;
	saved_height = build->viewport_height;
	build->viewport_width = frame->viewport_width;
	build->viewport_height = frame->viewport_height;

	/* Walks what the container contains. */
	tree = build->tree;
	if (kind == PICTURE_KIND_USE) {
		/* Draws the referenced element, with the use marked as being expanded so that a reference back to it is refused. */
		build->uses[build->use_count] = node;
		build->use_count++;
		picture_walk(build, frame->target, depth + 1, parent, clip, box, &frame->instance);
		build->use_count--;
	} else if (kind == PICTURE_KIND_SWITCH) {
		/* Draws the one child the switch chooses. */
		child = picture_switch_child(build, node);
		if (child != NULL)
			picture_walk(build, child, depth + 1, parent, clip, box, NULL);
	} else {
		/* Draws each child in order. */
		for (child = tree->first_child(tree->context, node);
		     child != NULL;
		     child = tree->next_sibling(tree->context, child)) {
			picture_walk(build, child, depth + 1, parent, clip, box, NULL);
			if (build->error != 0)
				break;
		}
	}

	/* Gives the viewport of the container's own level back. */
	build->viewport_width = saved_width;
	build->viewport_height = saved_height;
}

/*
 * Draws a container that was entered (the root, 'g', 'a', a nested 'svg',
 * a 'use', a 'switch', an instantiated 'symbol'): places its content,
 * builds its clip, opens a group when its opacity is below 1 (SVG 2
 * §3.6.1) and walks its children, which draw into the group with the clip
 * as their outer clip.  With a box to collect (a bounding box walk) it
 * only walks.
 */
static void
picture_container(
	struct picture_build *build,
	const struct svg_node *node,
	enum picture_kind kind,
	int depth,
	const struct vec_matrix *matrix,
	int outer_clip,
	struct picture_box *box,
	const struct picture_instance *instance)
{
	struct picture_frame frame;
	size_t group_index;
	int clip;
	int opened;
	int placed;
	int fits;

	/* Places the content; a container that draws nothing ends here. */
	placed = picture_frame(build, node, kind, depth, matrix, instance, &frame);
	if (!placed)
		return;

	/* Collects the boxes of the shapes only, without clips and groups. */
	if (box != NULL) {
		picture_children(build, node, kind, depth, &frame.content, &frame, outer_clip, box);
		return;
	}

	/* Builds the clip that applies to the container and so to its children. */
	clip = picture_clip(build, node, depth, &frame.own, NULL, kind, instance, outer_clip);
	if (build->error != 0)
		return;

	/* Starts a group when the container is not opaque: its two commands must fit. */
	opened = 0;
	group_index = 0;
	if (build->styles[depth].opacity < 1.0f) {
		fits = picture_room(build, 2);
		if (!fits)
			return;
		opened = picture_group_open(build, build->styles[depth].opacity, &group_index);
		if (!opened)
			return;
	}

	/* Draws what the container contains. */
	picture_children(build, node, kind, depth, &frame.content, &frame, clip, NULL);

	/* Ends the group. */
	if (opened)
		picture_group_close(build, group_index);
}

/*
 * Draws the root 'svg': its style, its viewBox and preserveAspectRatio
 * placed in the viewport (SVG 2 §8.2), its transform attribute applied
 * outside that (§8.2: as if the element had a parent with the transform),
 * then its content like a container's.  A viewBox with a zero width or
 * height disables rendering (§8.6).
 */
static void
picture_root(
	struct picture_build *build,
	const struct svg_node *root,
	float width,
	float height)
{
	const struct svg_tree *tree;
	const char *text;
	struct vec_rect view_box;
	struct svg_aspect aspect;
	struct vec_matrix box_matrix;
	struct vec_matrix transform;
	struct vec_matrix matrix;
	int has_view_box;
	int counted;
	int usable;
	int error;

	/* Counts the root as a visit. */
	counted = picture_visit(build);
	if (!counted)
		return;

	/* Computes the root's style: it inherits from the outer style. */
	tree = build->tree;
	picture_style(build, root, 1);

	/* Draws nothing for display none. */
	if (build->styles[1].display == SVG_DISPLAY_NONE)
		return;

	/* Reads the viewBox; an invalid one counts as absent (§8.6). */
	has_view_box = 0;
	text = tree->attribute(tree->context, root, "viewBox");
	if (text != NULL) {
		error = svg_parse_view_box(text, &view_box);
		if (error == 0)
			has_view_box = 1;
	}

	/* Computes the transform of the viewBox into the viewport. */
	vec_matrix_identity(&box_matrix);
	if (has_view_box) {
		/* Draws nothing for a viewBox without width or height (§8.6). */
		if (view_box.right - view_box.left <= 0.0f || view_box.bottom - view_box.top <= 0.0f)
			return;

		/* Reads preserveAspectRatio; an invalid one leaves the default xMidYMid meet. */
		aspect.align = SVG_ALIGN_XMID_YMID;
		aspect.slice = 0;
		text = tree->attribute(tree->context, root, "preserveAspectRatio");
		if (text != NULL)
			svg_parse_aspect(text, &aspect);

		/* Places the viewBox in the viewport (0, 0, width, height). */
		error = svg_view_box_transform(&view_box, &aspect, 0.0f, 0.0f, width, height, &box_matrix);
		if (error != 0)
			return;

		/* Lengths of the content resolve against the viewBox's size: it is the viewport in user units. */
		build->viewport_width = view_box.right - view_box.left;
		build->viewport_height = view_box.bottom - view_box.top;
	}

	/* Reads the transform attribute of the root; an invalid one is ignored. */
	vec_matrix_identity(&transform);
	text = tree->attribute(tree->context, root, "transform");
	if (text != NULL) {
		error = svg_parse_transform(text, &transform);
		if (error != 0)
			vec_matrix_identity(&transform);
	}

	/* Composes them: the viewBox transform first, then the root's transform. */
	vec_matrix_multiply(&matrix, &box_matrix, &transform);
	usable = picture_matrix_usable(&matrix);
	if (!usable)
		return;

	/* Draws the root's content. */
	picture_container(build, root, PICTURE_KIND_CONTAINER, 1, &matrix, -1, NULL, NULL);
}

/*
 * Includes the four corners of a box, mapped by a matrix, in a rectangle
 * (SVG 2 §8.10: the box of a shape in the container's user space is the
 * rectangle holding its box's corners).
 */
static void
picture_include_box(
	struct vec_rect *rect,
	const struct vec_matrix *matrix,
	const struct vec_rect *box)
{
	struct vec_point corner;
	struct vec_point mapped;

	/* Maps and includes the top left corner. */
	corner.x = box->left;
	corner.y = box->top;
	mapped = vec_matrix_apply(matrix, corner);
	vec_rect_include(rect, mapped);

	/* Maps and includes the top right corner. */
	corner.x = box->right;
	corner.y = box->top;
	mapped = vec_matrix_apply(matrix, corner);
	vec_rect_include(rect, mapped);

	/* Maps and includes the bottom right corner. */
	corner.x = box->right;
	corner.y = box->bottom;
	mapped = vec_matrix_apply(matrix, corner);
	vec_rect_include(rect, mapped);

	/* Maps and includes the bottom left corner. */
	corner.x = box->left;
	corner.y = box->bottom;
	mapped = vec_matrix_apply(matrix, corner);
	vec_rect_include(rect, mapped);
}

/*
 * Adds one shape's box to the box a walk collects: the shape's tight box
 * (its path's bounds) mapped by the matrix from the shape to the
 * container.  A shape whose path cannot be made adds nothing.
 */
static void
picture_bbox_shape(
	struct picture_build *build,
	const struct svg_node *node,
	const char *name,
	int depth,
	const struct vec_matrix *matrix,
	struct picture_box *box)
{
	struct svg_context context;
	struct vec_path path;
	struct vec_path_view view;
	struct vec_rect bounds;
	enum svg_shape_result result;
	int error;

	/* Builds the shape's path against its own context. */
	picture_context(build, depth, &context);
	vec_path_init(&path);
	result = SVG_SHAPE_EMPTY;
	error = svg_shape_path(build->tree, node, name, &context, &path, &result);
	if (error != 0) {
		/* A memory error ends the build; a path beyond the limits adds nothing. */
		if (error == ENOMEM)
			picture_fail(build, ENOMEM);
		vec_path_release(&path);
		return;
	}

	/* Adds nothing for a shape that renders nothing. */
	if (result != SVG_SHAPE_DRAWN) {
		vec_path_release(&path);
		return;
	}

	/* Takes the path's tight box. */
	vec_path_view_of(&path, &view);
	error = vec_path_bounds(&view, &bounds);
	if (error == 0) {
		picture_include_box(&box->rect, matrix, &bounds);
		box->found = 1;
	}

	/* Gives the path back. */
	vec_path_release(&path);
}

/*
 * Tells whether two uses' width and height overrides are the same.
 */
static int
picture_instance_same(
	const struct picture_instance *first,
	const struct picture_instance *second)
{
	/* Differs when one has a width and the other not, or the widths differ. */
	if (first->has_width != second->has_width)
		return 0;
	if (first->has_width && first->width != second->width)
		return 0;

	/* Differs likewise in the height. */
	if (first->has_height != second->has_height)
		return 0;
	if (first->has_height && first->height != second->height)
		return 0;

	/* The same overrides. */
	return 1;
}

/*
 * Tells whether a remembered bounding box was found in the situation a
 * new one would be: the same container, at the same depth of nesting and
 * of uses, in a viewport of the same size, with the same font size and the
 * same size from a 'use' (these decide the lengths below it).
 */
static int
picture_bbox_key_same(
	const struct picture_bbox_entry *entry,
	const struct picture_bbox_entry *key)
{
	int same;

	/* Differs when the container, its depth or its uses' depth differ. */
	if (entry->node != key->node)
		return 0;
	if (entry->depth != key->depth)
		return 0;
	if (entry->use_count != key->use_count)
		return 0;

	/* Differs when the viewport or the font size the lengths resolve against differ. */
	if (entry->viewport_width != key->viewport_width)
		return 0;
	if (entry->viewport_height != key->viewport_height)
		return 0;
	if (entry->font_size != key->font_size)
		return 0;

	/* Differs when the size a use gives differs. */
	same = picture_instance_same(&entry->instance, &key->instance);

	/* Reports whether the two situations are the same. */
	return same;
}

/*
 * Finds the bounding box of a container that was entered at a depth, in
 * its own user space (SVG 2 §8.10): the union of the boxes of the shapes
 * it draws, each box's corners mapped into the container's space.  A
 * container that draws no shape has the box (0, 0, 0, 0).  The box of
 * a container is found once and kept for the build: a container met again
 * in the same situation (the content of a 'use' used many times) is not
 * walked again.
 */
static void
picture_container_bbox(
	struct picture_build *build,
	const struct svg_node *node,
	enum picture_kind kind,
	int depth,
	const struct picture_instance *instance,
	struct vec_rect *bbox)
{
	struct picture_bbox_entry key;
	struct picture_bbox_entry *entry;
	struct picture_frame frame;
	struct picture_box box;
	struct vec_matrix identity;
	size_t slot;
	int same;
	int placed;

	/* Describes the situation the box depends on. */
	memset(&key, 0, sizeof(key));
	key.node = node;
	key.depth = depth;
	key.use_count = build->use_count;
	key.viewport_width = build->viewport_width;
	key.viewport_height = build->viewport_height;
	key.font_size = build->styles[depth].font_size;
	if (instance != NULL)
		key.instance = *instance;

	/* Reuses the box found before for the same container in the same situation. */
	slot = ((size_t)(uintptr_t)node >> 4) % PICTURE_BBOX_CACHE;
	entry = &build->bbox_cache[slot];
	same = picture_bbox_key_same(entry, &key);
	if (same) {
		*bbox = entry->box;
		return;
	}

	/* Starts from the empty box, with the container's content placed in its own space. */
	vec_rect_empty(&box.rect);
	box.found = 0;
	vec_matrix_identity(&identity);
	placed = picture_frame(build, node, kind, depth, &identity, instance, &frame);

	/* Adds the boxes of what the container draws, mapped into its own space. */
	if (placed)
		picture_children(build, node, kind, depth, &frame.inner, &frame, -1, &box);

	/* Uses the zero box when no shape was found. */
	if (!box.found) {
		box.rect.left = 0.0f;
		box.rect.top = 0.0f;
		box.rect.right = 0.0f;
		box.rect.bottom = 0.0f;
	}
	*bbox = box.rect;

	/* Remembers the box, unless the walk was cut short and the box is not whole. */
	if (build->error == 0) {
		key.box = box.rect;
		*entry = key;
	}
}

/*
 * Builds the clip of an element's clip-path (CSS Masking 1 §5.1) and
 * returns the clip index its commands and descendants carry: the new one,
 * or outer_clip when the element has none, names no 'clipPath', or
 * svg_clip_build fails other than by ENOMEM or ENOBUFS (the element is
 * then drawn without it).  ENOMEM and ENOBUFS are recorded in the build.
 *
 * shape_box is the shape's own box, or NULL for a container (of the given
 * kind, and with the size a 'use' gave it, if any), whose box is found by
 * walking its content (only now, and only when a clip needs it).  matrix
 * is the element's own user space.
 */
static int
picture_clip(
	struct picture_build *build,
	const struct svg_node *node,
	int depth,
	const struct vec_matrix *matrix,
	const struct vec_rect *shape_box,
	enum picture_kind kind,
	const struct picture_instance *instance,
	int outer_clip)
{
	const struct svg_tree *tree;
	const struct svg_style *style;
	const struct svg_node *clip_path;
	const char *name;
	struct svg_context context;
	struct vec_rect bbox;
	struct svg_picture *picture;
	size_t paths_before;
	size_t index;
	int is_clip_path;
	int clip_index;
	int error;

	/* Keeps the outer clip for an element with no clip-path. */
	style = &build->styles[depth];
	if (!style->has_clip_path)
		return outer_clip;

	/* Finds the element the clip-path names; anything but a 'clipPath' clips nothing. */
	tree = build->tree;
	clip_path = tree->find_id(tree->context, style->clip_path);
	if (clip_path == NULL)
		return outer_clip;
	name = tree->name(tree->context, clip_path);
	is_clip_path = picture_name_is(name, "clipPath");
	if (!is_clip_path)
		return outer_clip;

	/* Takes the element's box: the shape's own, or the container's found by a walk. */
	if (shape_box != NULL) {
		bbox = *shape_box;
	} else {
		picture_container_bbox(build, node, kind, depth, instance, &bbox);
		if (build->error != 0)
			return outer_clip;
	}

	/* Builds the clip against the element's matrix, box and context. */
	picture = build->picture;
	paths_before = picture->path_count;
	picture_context(build, depth, &context);
	clip_index = -1;
	error = svg_clip_build(tree, clip_path, build->outer, matrix, &bbox, &context, outer_clip, picture, &clip_index);

	/* Counts the points of the paths the clip builder added, kept even when it failed. */
	for (index = paths_before;
	     index < picture->path_count;
	     index++)
		build->points += picture->paths[index].point_count;
	if (build->points > SVG_POINTS_MAX) {
		picture_fail(build, ENOBUFS);
		return outer_clip;
	}

	/* Ends the build when memory ran out or a limit of the clips was reached. */
	if (error == ENOMEM || error == ENOBUFS) {
		picture_fail(build, error);
		return outer_clip;
	}

	/* Draws without a clip for any other failure (CSS Masking 1 §5.1: a clip that cannot be made clips nothing). */
	if (error != 0)
		return outer_clip;

	/* Succeeded: the new clip covers the element and its descendants. */
	return clip_index;
}

/*
 * Resolves the fallback of a paint whose url names no usable paint server
 * (SVG 2 §13.2): a color, currentColor, or none (also when no fallback was
 * given).  Sets *drawn and returns the color.
 */
static uint32_t
picture_fallback_color(
	const struct svg_paint *paint,
	const struct svg_style *style,
	int *drawn)
{
	/* Takes the fallback color. */
	if (paint->fallback_kind == SVG_PAINT_COLOR) {
		*drawn = 1;
		return paint->fallback_color;
	}

	/* Takes the color property for a currentColor fallback. */
	if (paint->fallback_kind == SVG_PAINT_CURRENT_COLOR) {
		*drawn = 1;
		return style->color;
	}

	/* Paints nothing for a fallback of none or no fallback. */
	*drawn = 0;
	return 0;
}

/*
 * Resolves one paint of a shape (fill or stroke) into a command paint:
 * none draws nothing; a color and currentColor give a color; an url to a
 * gradient gives its color or gradient (the stops stored in the picture),
 * or nothing for a gradient without stops or a box without size (SVG 2
 * §14.2.4.3 and §8.11); an url that names no gradient takes the fallback
 * (§13.2).  opacity is fill-opacity or stroke-opacity.
 */
static void
picture_paint_resolve(
	struct picture_build *build,
	const struct svg_paint *paint,
	float opacity,
	const struct svg_style *style,
	const struct vec_rect *bbox,
	const struct svg_context *context,
	struct picture_paint_result *result)
{
	const struct svg_tree *tree;
	const struct svg_node *element;
	const char *name;
	struct svg_server server;
	int is_gradient;
	int is_linear;
	int is_radial;
	int resolved;
	int stored;
	int error;

	/* Starts with nothing to draw. */
	memset(result, 0, sizeof(*result));
	result->paint.opacity = opacity;

	/* Paints nothing for none. */
	if (paint->kind == SVG_PAINT_NONE)
		return;

	/* Takes a straight color. */
	if (paint->kind == SVG_PAINT_COLOR) {
		result->drawn = 1;
		result->paint.kind = SVG_SERVER_COLOR;
		result->paint.color = paint->color;
		return;
	}

	/* Takes the color property for currentColor (SVG 2 §13.3). */
	if (paint->kind == SVG_PAINT_CURRENT_COLOR) {
		result->drawn = 1;
		result->paint.kind = SVG_SERVER_COLOR;
		result->paint.color = style->color;
		return;
	}

	/* Finds the element the url names, and whether it is a gradient. */
	tree = build->tree;
	resolved = 0;
	is_gradient = 0;
	element = tree->find_id(tree->context, paint->id);
	if (element != NULL) {
		name = tree->name(tree->context, element);
		is_linear = picture_name_is(name, "linearGradient");
		is_radial = picture_name_is(name, "radialGradient");
		if (is_linear || is_radial)
			is_gradient = 1;
	}

	/* Resolves the gradient for the shape's box. */
	if (is_gradient) {
		svg_server_init(&server);
		error = svg_server_resolve(tree, element, build->outer, bbox, context, &server);
		if (error == ENOMEM) {
			picture_fail(build, ENOMEM);
			svg_server_release(&server);
			return;
		}
		if (error == 0)
			resolved = 1;

		/* Takes a single color the server gave. */
		if (resolved && server.kind == SVG_SERVER_COLOR) {
			result->drawn = 1;
			result->paint.kind = SVG_SERVER_COLOR;
			result->paint.color = server.color;
		}

		/* Takes a gradient: its stops go to the picture, its pointer is set when the build ends. */
		if (resolved && server.kind == SVG_SERVER_GRADIENT) {
			stored = picture_stops_store(build, element, server.stops, server.gradient.count, &result->stop_first);
			if (stored) {
				result->drawn = 1;
				result->paint.kind = SVG_SERVER_GRADIENT;
				result->paint.gradient = server.gradient;
				result->paint.gradient.stops = NULL;
			}
		}
		svg_server_release(&server);
	}

	/* Takes the fallback when the url names no usable paint server. */
	if (!resolved) {
		result->paint.color = picture_fallback_color(paint, style, &result->drawn);
		if (result->drawn)
			result->paint.kind = SVG_SERVER_COLOR;
	}
}

/*
 * Measures the length of a flattened path: the sum of vec_contour_length
 * over its contours, flattened in user space with the measuring tolerance
 * (SVG 2 §9.6.1).  Returns 1 with *length, or 0 when the path cannot be
 * measured (a memory error is recorded).
 */
static int
picture_measure(
	struct picture_build *build,
	const struct vec_path *path,
	float *length)
{
	struct vec_path_view view;
	struct vec_lines lines;
	size_t contour;
	float total;
	int finite;
	int error;

	/* Flattens the path in user space. */
	vec_path_view_of(path, &view);
	vec_lines_init(&lines);
	error = vec_flatten(&view, NULL, PICTURE_MEASURE_TOLERANCE, &lines);
	if (error != 0) {
		if (error == ENOMEM)
			picture_fail(build, ENOMEM);
		vec_lines_release(&lines);
		return 0;
	}

	/* Adds up the lengths of the contours. */
	total = 0.0f;
	for (contour = 0;
	     contour < lines.contour_count;
	     contour++)
		total += vec_contour_length(&lines, contour);
	vec_lines_release(&lines);

	/* Refuses a length that is not finite. */
	finite = vec_finite(total);
	if (!finite)
		return 0;

	/* Succeeded: the path is that long. */
	*length = total;
	return 1;
}

/*
 * Resolves the dash pattern of a stroke into the job (SVG 2 §13.5.7,
 * lengths against the context): the lengths, the offset, and the scale of
 * pathLength (§9.6.1).  A pattern of zero total length is no pattern; with
 * a pathLength of 0, a pattern that starts with a positive length is one
 * solid line and one that starts with 0 draws nothing.
 */
static void
picture_dash_setup(
	struct picture_build *build,
	const struct svg_node *node,
	const struct vec_path *path,
	const struct svg_style *style,
	const struct svg_context *context,
	struct picture_shape_job *job)
{
	const struct svg_tree *tree;
	const char *text;
	float total;
	float value;
	float path_length;
	float length;
	float scale;
	size_t index;
	int infinite;
	int measured;
	int finite;
	int error;

	/* Starts with no dashes, the scale 1 and no offset. */
	job->dash_count = 0;
	job->stroke.dash_scale = 1.0f;
	job->stroke.dash_offset = 0.0f;
	if (style->dash_count == 0)
		return;

	/* Resolves each dash length; a negative one makes the list unusable. */
	total = 0.0f;
	for (index = 0;
	     index < style->dash_count;
	     index++) {
		value = svg_length_resolve(&style->dashes[index], SVG_AXIS_OTHER, context);
		if (value < 0.0f)
			return;
		job->dashes[index] = value;
		total += value;
	}

	/* Takes no pattern for lengths that add up to nothing or overflow. */
	finite = vec_finite(total);
	if (!finite || total <= 0.0f)
		return;
	job->dash_count = style->dash_count;

	/* Resolves the offset. */
	job->stroke.dash_offset = svg_length_resolve(&style->dash_offset, SVG_AXIS_OTHER, context);

	/* Reads pathLength; a missing, invalid or negative value is ignored (§9.6.1). */
	tree = build->tree;
	text = tree->attribute(tree->context, node, "pathLength");
	if (text == NULL)
		return;
	error = svg_parse_number(text, &path_length);
	if (error != 0 || path_length < 0.0f)
		return;

	/* Scales by the measured length over pathLength; a pathLength of 0 is an infinite scale. */
	infinite = 0;
	scale = 1.0f;
	if (path_length == 0.0f) {
		infinite = 1;
	} else {
		length = 0.0f;
		measured = picture_measure(build, path, &length);
		if (!measured || length <= 0.0f)
			return;
		scale = length / path_length;
		finite = vec_finite(scale);
		if (!finite)
			infinite = 1;
		else if (scale <= 0.0f)
			return;
	}

	/* Keeps a finite scale. */
	if (!infinite) {
		job->stroke.dash_scale = scale;
		return;
	}

	/* With an infinite scale a pattern starting with a positive length is one solid line. */
	if (job->dashes[0] > 0.0f) {
		job->dash_count = 0;
		return;
	}

	/* With an infinite scale a pattern starting with 0 draws nothing. */
	job->stroke_drawn = 0;
}

/*
 * Fills the job's stroke style from the element's style: the width, cap,
 * join, miter limit (a value under 1 is taken as 1) and the dash pattern.
 * The tolerance stays 0 for the drawer to set.
 */
static void
picture_stroke_setup(
	struct picture_build *build,
	const struct svg_node *node,
	const struct vec_path *path,
	const struct svg_style *style,
	const struct svg_context *context,
	float stroke_width,
	struct picture_shape_job *job)
{
	float miter_limit;
	int finite;

	/* Takes the width, cap and join. */
	memset(&job->stroke, 0, sizeof(job->stroke));
	job->stroke.width = stroke_width;
	job->stroke.cap = style->stroke_linecap;
	job->stroke.join = style->stroke_linejoin;

	/* Takes the miter limit; a value below 1 (or not a number) is raised to what the stroker accepts. */
	miter_limit = style->stroke_miterlimit;
	finite = vec_finite(miter_limit);
	if (!finite)
		miter_limit = PICTURE_MITER_LIMIT_DEFAULT;
	if (miter_limit < 1.0f)
		miter_limit = 1.0f;
	job->stroke.miter_limit = miter_limit;

	/* Resolves the dash pattern, its offset and the pathLength scale. */
	picture_dash_setup(build, node, path, style, context, job);
	job->stroke.dash_count = job->dash_count;
}

/*
 * Adds a decided shape to the picture: its path, the group of its
 * opacity, the fill command and then the stroke command (SVG 2 §13.8's
 * default paint order).  Takes the path (it is empty afterwards).
 */
static void
picture_shape_emit(
	struct picture_build *build,
	struct vec_path *path,
	struct picture_shape_job *job)
{
	struct svg_command command;
	size_t path_index;
	size_t group_index;
	size_t needed;
	int grouped;
	int opened;
	int fits;
	int stored;
	int error;

	/* Counts the commands the shape adds, with its group's two when it is not opaque. */
	grouped = job->opacity < 1.0f;
	needed = 0;
	if (job->fill.drawn)
		needed++;
	if (job->stroke_drawn)
		needed++;
	if (grouped)
		needed += 2;
	fits = picture_room(build, needed);
	if (!fits)
		return;

	/* Adds the path to the picture (it takes the path's arrays). */
	error = svg_picture_add_path(build->picture, path, &path_index);
	if (error == ENOMEM) {
		picture_fail(build, ENOMEM);
		return;
	}
	if (error != 0) {
		picture_fail(build, ENOBUFS);
		return;
	}

	/* Starts the group of the shape's opacity. */
	opened = 0;
	group_index = 0;
	if (grouped) {
		opened = picture_group_open(build, job->opacity, &group_index);
		if (!opened)
			return;
	}

	/* Adds the fill command. */
	if (job->fill.drawn) {
		memset(&command, 0, sizeof(command));
		command.kind = SVG_COMMAND_FILL;
		command.path = path_index;
		command.matrix = job->matrix;
		command.rule = job->rule;
		command.paint = job->fill.paint;
		command.stop_first = job->fill.stop_first;
		command.clip = job->clip;
		command.opacity = 1.0f;
		command.bbox = job->bbox;
		picture_command_add(build, &command);
	}

	/* Adds the stroke command, with its dash lengths stored in the picture. */
	if (job->stroke_drawn && build->error == 0) {
		memset(&command, 0, sizeof(command));
		command.kind = SVG_COMMAND_STROKE;
		command.path = path_index;
		command.matrix = job->matrix;
		command.rule = job->rule;
		command.paint = job->stroke_paint.paint;
		command.stop_first = job->stroke_paint.stop_first;
		command.stroke = job->stroke;
		command.clip = job->clip;
		command.opacity = 1.0f;
		command.bbox = job->bbox;
		stored = picture_dashes_store(build, job->dashes, job->dash_count, &command.dash_first);
		if (stored)
			picture_command_add(build, &command);
	}

	/* Ends the group. */
	if (opened)
		picture_group_close(build, group_index);
}

/*
 * Draws one shape element: decides what it paints (fill and stroke, each
 * resolved against its box), builds its clip and adds it.  A hidden
 * shape, one with no paint, or one that renders nothing adds nothing.
 */
static void
picture_shape(
	struct picture_build *build,
	const struct svg_node *node,
	const char *name,
	int depth,
	const struct vec_matrix *matrix,
	int outer_clip)
{
	const struct svg_style *style;
	struct svg_context context;
	struct picture_shape_job job;
	struct vec_path path;
	struct vec_path_view view;
	enum svg_shape_result result;
	float stroke_width;
	int stroke_wanted;
	int finite;
	int error;

	/* Draws nothing for a hidden shape (SVG 2 §3.2.3: visibility hidden and collapse do not paint). */
	style = &build->styles[depth];
	if (style->visibility != SVG_VISIBILITY_VISIBLE)
		return;

	/* Finds out whether the stroke can paint: a paint, and a positive width resolved against the context. */
	picture_context(build, depth, &context);
	stroke_width = svg_length_resolve(&style->stroke_width, SVG_AXIS_OTHER, &context);
	finite = vec_finite(stroke_width);
	stroke_wanted = 0;
	if (style->stroke.kind != SVG_PAINT_NONE && finite && stroke_width > 0.0f)
		stroke_wanted = 1;

	/* Draws nothing for a shape with neither a fill nor a stroke to paint. */
	if (style->fill.kind == SVG_PAINT_NONE && !stroke_wanted)
		return;

	/* Builds the shape's path in its user space. */
	memset(&job, 0, sizeof(job));
	vec_path_init(&path);
	result = SVG_SHAPE_EMPTY;
	error = svg_shape_path(build->tree, node, name, &context, &path, &result);
	if (error != 0) {
		/* A memory error ends the build, a path beyond the limits ends it with ENOBUFS, anything else draws nothing. */
		if (error == ENOMEM)
			picture_fail(build, ENOMEM);
		else if (error == ENOBUFS)
			picture_fail(build, ENOBUFS);
		goto done;
	}

	/* Draws nothing for a shape that renders nothing. */
	if (result != SVG_SHAPE_DRAWN)
		goto done;

	/* Takes the shape's tight box, which objectBoundingBox units resolve against. */
	vec_path_view_of(&path, &view);
	error = vec_path_bounds(&view, &job.bbox);
	if (error != 0)
		goto done;

	/* Refuses a shape that would pass the limit of points of the picture. */
	if (build->points > SVG_POINTS_MAX || path.point_count > SVG_POINTS_MAX - build->points) {
		picture_fail(build, ENOBUFS);
		goto done;
	}

	/* Resolves the fill. */
	picture_paint_resolve(build, &style->fill, style->fill_opacity, style, &job.bbox, &context, &job.fill);
	if (build->error != 0)
		goto done;

	/* Resolves the stroke's paint. */
	if (stroke_wanted) {
		picture_paint_resolve(build, &style->stroke, style->stroke_opacity, style, &job.bbox, &context, &job.stroke_paint);
		job.stroke_drawn = job.stroke_paint.drawn;
	}
	if (build->error != 0)
		goto done;

	/* Resolves the stroke's style, which may find that nothing is left to stroke. */
	if (job.stroke_drawn)
		picture_stroke_setup(build, node, &path, style, &context, stroke_width, &job);
	if (build->error != 0)
		goto done;

	/* Draws nothing when neither paint is left. */
	if (!job.fill.drawn && !job.stroke_drawn)
		goto done;

	/* Builds the clip of the shape. */
	job.clip = picture_clip(build, node, depth, matrix, &job.bbox, PICTURE_KIND_SHAPE, NULL, outer_clip);
	if (build->error != 0)
		goto done;

	/* Adds the shape (this takes the path). */
	job.matrix = *matrix;
	job.opacity = style->opacity;
	job.rule = style->fill_rule;
	build->points += path.point_count;
	picture_shape_emit(build, &path, &job);

done:
	/* Gives back the path when it was not taken. */
	vec_path_release(&path);
}

/*
 * Sets the pointers of the commands that point into the picture's arrays
 * (dashes and gradient stops) from their indices, now that no array grows
 * any more.
 */
static void
picture_finish(
	struct picture_build *build)
{
	struct svg_picture *picture;
	struct svg_command *command;
	size_t index;

	/* Points each stroke's dashes and each gradient's stops into the picture. */
	picture = build->picture;
	for (index = 0;
	     index < picture->command_count;
	     index++) {
		command = &picture->commands[index];
		if (command->kind != SVG_COMMAND_FILL && command->kind != SVG_COMMAND_STROKE)
			continue;
		if (command->kind == SVG_COMMAND_STROKE && command->stroke.dash_count > 0)
			command->stroke.dashes = picture->dashes + command->dash_first;
		if (command->paint.kind == SVG_SERVER_GRADIENT)
			command->paint.gradient.stops = picture->stops + command->stop_first;
	}
}
