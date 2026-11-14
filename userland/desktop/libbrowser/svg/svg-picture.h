/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A picture: an SVG document fragment turned into a list of painting
 * commands, in painting order, independent of where and how large it is
 * drawn.
 *
 * Building walks the rendering tree (SVG 2 §3.2): the 'svg' root and
 * nested 'svg' elements (new viewports, §5.1 and §8.8), 'g' (§5.2), 'use'
 * (§5.5, with 'symbol' §5.4), 'switch' (§5.6: its first child whose
 * conditions hold, with no condition attribute supported holding), and the
 * shape elements (svg-shape.h).  'defs', 'symbol' outside a 'use',
 * 'clipPath', 'linearGradient', 'radialGradient', 'title', 'desc',
 * 'metadata' and elements of other namespaces are not rendered.  Text,
 * images, markers, masks, patterns, filters and foreignObject are not
 * drawn (they and their subtrees are skipped).
 *
 * A shape becomes a fill command and then a stroke command (§13.8's
 * default paint-order: fill, stroke), each when its paint is not none and
 * the element is visible.  Opacity on a group or a shape with
 * opacity < 1 becomes a group around its commands (§3.6.1).
 *
 * Coordinates: the picture's space is the root viewport's, in CSS px,
 * (0, 0) to (width, height) as given to svg_picture_build; each command
 * carries the matrix from its element's user space to the picture's
 * space (the viewBox transforms, transforms and use offsets on the way).
 */

#ifndef KEILAND_BROWSER_SVG_PICTURE_H
#define KEILAND_BROWSER_SVG_PICTURE_H

#include "svg/svg-pserver.h"
#include "vector/vec-flatten.h"

/* The deepest element nesting walked (deeper elements are not drawn). */
#define SVG_DEPTH_MAX		64

/* The most 'use' elements nested inside one another (deeper ones draw nothing, as a cycle does). */
#define SVG_USE_DEPTH_MAX	16

/* The most commands of one picture (more are dropped and the build reports ENOBUFS with the picture kept). */
#define SVG_COMMANDS_MAX	65536

/* The most clips of one picture. */
#define SVG_CLIPS_MAX		4096

/* The kinds of command. */
enum svg_command_kind {
	/* Fills the command's path with its paint. */
	SVG_COMMAND_FILL = 0,
	/* Strokes the command's path with its paint and stroke style. */
	SVG_COMMAND_STROKE = 1,
	/* Starts a group: the commands up to the matching end are composited together with the opacity. */
	SVG_COMMAND_GROUP = 2,
	/* Ends the innermost group. */
	SVG_COMMAND_GROUP_END = 3
};

/*
 * A command's paint: a straight color, or a resolved gradient (its stops
 * owned by the picture), with the opacity to apply on top (fill-opacity
 * or stroke-opacity: the paint's alpha is multiplied by it).
 */
struct svg_command_paint {
	enum svg_server_kind kind;
	uint32_t color;
	struct vec_gradient gradient;
	float opacity;
};

/*
 * One painting command.
 *
 * path is an index into the picture's paths (FILL and STROKE), matrix the
 * user-to-picture transform.  rule the fill rule (FILL).  stroke the
 * style for STROKE with lengths resolved in user units; its dashes point
 * into the picture's dash storage, and its tolerance is 0 (the drawer
 * sets it).  clip the index of the clip that applies (-1 for none: the
 * clip of the element and of its ancestors, met together, is one clip of
 * the picture, see struct svg_clip).  opacity the group opacity (GROUP).
 * A command's bbox is the tight bounding box of its path in user space,
 * which objectBoundingBox units were resolved against.
 */
struct svg_command {
	enum svg_command_kind kind;
	size_t path;
	struct vec_matrix matrix;
	enum vec_fill_rule rule;
	struct svg_command_paint paint;
	struct vec_stroke_style stroke;
	int clip;
	float opacity;
	struct vec_rect bbox;
};

/*
 * One part of a clipping path: a path (an index into the picture's paths)
 * with its matrix into the picture's space and its clip-rule (CSS Masking
 * 1 §6.2).
 */
struct svg_clip_part {
	size_t path;
	struct vec_matrix matrix;
	enum vec_fill_rule rule;
};

/*
 * One clip: the union of its parts (CSS Masking 1 §6.1: the clipping path
 * is the union of the shapes of its children), met with the clip parent
 * when parent >= 0 (a clip-path on the 'clipPath' element itself, or the
 * clip of an ancestor of the clipped element).  The parts are
 * part_count consecutive entries of the picture's parts from first_part.
 * A clip with no part clips everything away (an empty clipping path).
 */
struct svg_clip {
	size_t first_part;
	size_t part_count;
	int parent;
};

/*
 * A built picture: its size, the commands, and what they point into, all
 * owned by the picture and freed by svg_picture_release.  The paths are
 * kept as struct vec_path (each the equivalent path of one element, built
 * once even when several commands use it).  The stops of every gradient
 * of the commands are in stops; dashes in dashes.
 */
struct svg_picture {
	float width;
	float height;
	struct svg_command *commands;
	size_t command_count;
	size_t command_capacity;
	struct vec_path *paths;
	size_t path_count;
	size_t path_capacity;
	struct svg_clip *clips;
	size_t clip_count;
	size_t clip_capacity;
	struct svg_clip_part *parts;
	size_t part_count;
	size_t part_capacity;
	struct vec_stop *stops;
	size_t stop_count;
	size_t stop_capacity;
	float *dashes;
	size_t dash_count;
	size_t dash_capacity;
};

/* Makes an empty picture. */
void svg_picture_init(struct svg_picture *picture);

/* Frees everything a picture owns and leaves it empty. */
void svg_picture_release(struct svg_picture *picture);

/*
 * Adds a path to a picture (taking its arrays: path is left empty) and
 * reports its index: the step the builder and the clip builder share.
 * Errors: ENOBUFS beyond SVG_COMMANDS_MAX paths; ENOMEM (path is then
 * released).
 */
int svg_picture_add_path(struct svg_picture *picture, struct vec_path *path, size_t *index);

/*
 * Builds the picture of an 'svg' element drawn into a viewport of width
 * by height CSS px.
 *
 * The root's viewBox and preserveAspectRatio (svg_view_box_transform)
 * place its content in (0, 0, width, height); its x, y, width and height
 * attributes are not used for the root (the caller sized the viewport
 * from them).  A nested 'svg' establishes a viewport at its x, y, width
 * and height (100% for an absent size) and clips to it only when its
 * overflow would (§8.8; this builder does not clip nested viewports: a
 * known gap).  The root's style starts from svg_style_initial with
 * font_size the given font size.
 *
 * Per element: its style (svg_style_inherit from the parent, then
 * svg_style_apply); display none skips it and its subtree; its transform
 * attribute (svg_parse_transform; an invalid one is ignored) applied
 * before its children's; its clip-path through svg_clip_build (an id that
 * names no 'clipPath' clips nothing, CSS Masking 1 §5.1, and so does any
 * failure of svg_clip_build but ENOMEM, which ends the build); the clip
 * passes to its descendants as their outer clip (it is built before
 * them; for a container, 'g', 'use' or 'svg', the bounding box it is given
 * is the container's, §8.10: the union of the boxes of its rendered
 * shapes, each box's corners mapped into the container's user space,
 * found by a walk of its subtree before its commands are built); its
 * opacity
 * < 1 a group.  For a shape: its path (svg_shape_path), its bounding box
 * (vec_path_bounds), its fill paint resolved (a color, currentColor from
 * the style's color, or an url to a gradient through svg_server_resolve,
 * the fallback or none when the url names no gradient, §13.2), the same
 * for its stroke with the stroke width and dashes resolved
 * (svg_length_resolve; percentages of the normalized diagonal; a negative
 * width or dash is invalid: no stroke, or no dashing), stroke-dashoffset,
 * and the commands added.  'use' (§5.5): the referenced element (href,
 * #id only) drawn as a child of the 'use' with translate(x, y) after the
 * use's transform, a referenced 'symbol' or 'svg' establishing a viewport
 * of the use's width and height (100% when absent) with its viewBox; a
 * reference to an ancestor of the use, or deeper than SVG_USE_DEPTH_MAX
 * uses, draws nothing (§5.5.1's circular references).
 *
 * Input: tree; root an element named "svg"; width and height finite and
 * > 0; font_size the root's font size in px (16 when unknown); picture
 * initialized (its old contents are released first).
 * Output: the picture.
 * Errors: EINVAL for a root that is not an 'svg' or a size out of range
 * (the picture is then empty); ENOBUFS when SVG_COMMANDS_MAX or
 * SVG_CLIPS_MAX was reached (the picture holds the commands up to there
 * and is drawable); ENOMEM (the picture is then empty).
 */
int svg_picture_build(const struct svg_tree *tree, const struct svg_node *root, float width, float height, float font_size, struct svg_picture *picture);

#endif
