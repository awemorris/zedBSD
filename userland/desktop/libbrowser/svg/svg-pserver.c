/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * SVG gradient elements resolved into vector gradients.
 */

#include "svg/svg-pserver.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The largest alpha of a straight 0xAARRGGBB color. */
#define PSERVER_ALPHA_MAX	255.0f

/*
 * The elements a gradient takes its attributes and stops from: the
 * element itself and the templates its href names in turn, nearest
 * first.  The chain is no longer than SVG_TEMPLATE_DEPTH and holds no
 * element twice.
 */
struct pserver_chain {
	const struct svg_node *nodes[SVG_TEMPLATE_DEPTH];
	size_t count;
};

/*
 * The words of gradientUnits, in the order of pserver_units_box below.
 *
 * Values referenced: SVG 2 §14.2.2.1 and §14.2.3.1 (gradientUnits:
 * userSpaceOnUse | objectBoundingBox).
 */
static const char *const pserver_units_words[] = {
	"userSpaceOnUse",
	"objectBoundingBox"
};

/* Whether each word of pserver_units_words means that lengths are fractions of the bounding box. */
static const int pserver_units_box[] = {
	0,
	1
};

/*
 * The words of spreadMethod, in the order of pserver_spread_values below.
 *
 * Values referenced: SVG 2 §14.2.2.1 and §14.2.3.1 (spreadMethod:
 * pad | reflect | repeat).
 */
static const char *const pserver_spread_words[] = {
	"pad",
	"reflect",
	"repeat"
};

/* The spread method each word of pserver_spread_words stands for. */
static const enum vec_spread pserver_spread_values[] = {
	VEC_SPREAD_PAD,
	VEC_SPREAD_REFLECT,
	VEC_SPREAD_REPEAT
};

/* Forward declarations of the static functions. */
static int pserver_is_space(char c);
static const char *pserver_skip_space(const char *p);
static int pserver_name_equal(const struct svg_tree *tree, const struct svg_node *node, const char *name);
static int pserver_is_gradient(const struct svg_tree *tree, const struct svg_node *node);
static int pserver_chain_contains(const struct pserver_chain *chain, const struct svg_node *node);
static const struct svg_node *pserver_template(const struct svg_tree *tree, const struct svg_node *node);
static void pserver_chain_build(const struct svg_tree *tree, const struct svg_node *element, struct pserver_chain *chain);
static int pserver_find_length(const struct svg_tree *tree, const struct pserver_chain *chain, const char *kind, const char *name, int nonnegative, struct svg_length *length);
static int pserver_word_index(const char *text, const char *const *words, size_t word_count);
static int pserver_find_word(const struct svg_tree *tree, const struct pserver_chain *chain, const char *name, const char *const *words, size_t word_count);
static void pserver_find_transform(const struct svg_tree *tree, const struct pserver_chain *chain, struct vec_matrix *matrix);
static float pserver_length_value(const struct svg_length *length, enum svg_axis axis, int box_units, const struct svg_context *context);
static int pserver_value_usable(float value);
static const struct svg_node *pserver_stops_source(const struct svg_tree *tree, const struct pserver_chain *chain);
static size_t pserver_stop_count(const struct svg_tree *tree, const struct svg_node *source);
static uint32_t pserver_stop_color(const struct svg_style *style);
static float pserver_stop_offset(const struct svg_tree *tree, const struct svg_node *stop);
static int pserver_read_stops(const struct svg_tree *tree, const struct svg_node *source, const struct svg_style *outer, struct vec_stop **stops, size_t *count);
static int pserver_linear_geometry(const struct svg_tree *tree, const struct pserver_chain *chain, int box_units, const struct svg_context *context, struct vec_gradient *gradient);
static int pserver_radial_geometry(const struct svg_tree *tree, const struct pserver_chain *chain, int box_units, const struct svg_context *context, struct vec_gradient *gradient);
static int pserver_to_gradient(const struct svg_tree *tree, const struct pserver_chain *chain, int box_units, const struct vec_rect *bbox, struct vec_matrix *to_gradient);

/*
 * Makes an empty server: no paint, no stops.
 */
void
svg_server_init(
	struct svg_server *server)
{
	/* Refuses a missing server. */
	if (server == NULL)
		return;

	/* Clears every field: kind NONE, no color, no gradient, no stops. */
	memset(server, 0, sizeof(*server));
	server->kind = SVG_SERVER_NONE;
	server->stops = NULL;
}

/*
 * Frees the stops of a server and leaves it empty.
 */
void
svg_server_release(
	struct svg_server *server)
{
	/* Refuses a missing server. */
	if (server == NULL)
		return;

	/* Frees the stops the gradient points at. */
	free(server->stops);

	/* Leaves the server empty. */
	svg_server_init(server);
}

/*
 * Resolves a gradient element for a shape.
 *
 * Values referenced: SVG 2 §14.1.1 (templates by href), §14.2.2.1 and
 * §14.2.2.2 (linearGradient attributes and defaults, equal points), §14.2.3.1
 * and §14.2.3.2 (radialGradient attributes and defaults, fx, fy, fr),
 * §14.2.4 (stops: offset, stop-color, stop-opacity, the order fixed up, no
 * stop, one stop), §8.11 (object bounding box units, the box mapping, a box
 * without width or height).  An attribute whose value is invalid for its
 * type (an unknown word, a malformed length or transform, a negative r or
 * fr) counts as not specified on that element, so the next template's
 * value, or the initial one, is taken.  A length of another unit than a
 * percentage or a number in objectBoundingBox units is taken in user
 * units as the fraction.  A geometry value that is not finite or beyond
 * VEC_COORD_MAX paints nothing.  More stops than VEC_GRADIENT_STOPS_MAX
 * keep the first ones.
 */
int
svg_server_resolve(
	const struct svg_tree *tree,
	const struct svg_node *element,
	const struct svg_style *outer,
	const struct vec_rect *bbox,
	const struct svg_context *context,
	struct svg_server *server)
{
	struct pserver_chain chain;
	struct vec_gradient gradient;
	struct vec_stop *stops;
	const struct svg_node *source;
	struct vec_matrix to_gradient;
	size_t count;
	uint32_t color;
	int box_units;
	int word;
	int is_linear;
	int is_radial;
	int usable;
	int error;

	/* Releases the old contents of the server. */
	if (server == NULL)
		return EINVAL;
	svg_server_release(server);

	/* Refuses missing arguments. */
	if (tree == NULL || element == NULL || context == NULL)
		return EINVAL;

	/* Refuses an element that is not a gradient element. */
	is_linear = pserver_name_equal(tree, element, "linearGradient");
	is_radial = pserver_name_equal(tree, element, "radialGradient");
	if (!is_linear && !is_radial)
		return EINVAL;

	/* Follows the templates of the element. */
	pserver_chain_build(tree, element, &chain);

	/* Takes gradientUnits: objectBoundingBox unless a word of the chain says otherwise. */
	box_units = 1;
	word = pserver_find_word(tree, &chain, "gradientUnits", pserver_units_words, 2);
	if (word >= 0)
		box_units = pserver_units_box[word];

	/* Paints nothing when the units need a box that has no width or height. */
	if (box_units) {
		if (bbox == NULL)
			return 0;
		if (!vec_finite(bbox->left) || !vec_finite(bbox->top) || !vec_finite(bbox->right) || !vec_finite(bbox->bottom))
			return 0;
		if (!(bbox->right > bbox->left) || !(bbox->bottom > bbox->top))
			return 0;
	}

	/* Makes the matrix from the shape's user space to gradient space; paints nothing when there is none. */
	error = pserver_to_gradient(tree, &chain, box_units, bbox, &to_gradient);
	if (error != 0)
		return 0;

	/* Finds the stops: the element's own, or the first template's that has any; none paints nothing. */
	source = pserver_stops_source(tree, &chain);
	if (source == NULL)
		return 0;

	/* Starts the gradient with the model every gradient element has. */
	memset(&gradient, 0, sizeof(gradient));
	gradient.premultiplied = 0;
	gradient.to_gradient = to_gradient;
	word = pserver_find_word(tree, &chain, "spreadMethod", pserver_spread_words, 3);
	gradient.spread = VEC_SPREAD_PAD;
	if (word >= 0)
		gradient.spread = pserver_spread_values[word];

	/* Takes the geometry of the element's kind; a geometry that cannot be used paints nothing. */
	if (is_linear)
		usable = pserver_linear_geometry(tree, &chain, box_units, context, &gradient);
	else
		usable = pserver_radial_geometry(tree, &chain, box_units, context, &gradient);
	if (!usable)
		return 0;

	/* Reads the stops with their colors. */
	count = 0;
	stops = NULL;
	error = pserver_read_stops(tree, source, outer, &stops, &count);
	if (error != 0)
		return error;

	/* Clamps the offsets and puts them in order. */
	error = vec_gradient_fixup_svg(stops, count);
	if (error != 0) {
		free(stops);
		return error;
	}

	/*
	 * Gives one color where there is nothing to interpolate: one stop,
	 * or a linear gradient whose two points are the same (its last stop).
	 */
	color = stops[count - 1].color;
	if (count == 1 || (is_linear && gradient.start.x == gradient.end.x && gradient.start.y == gradient.end.y)) {
		free(stops);
		server->kind = SVG_SERVER_COLOR;
		server->color = color;

		/* Succeeded: the server paints one color. */
		return 0;
	}

	/* Hands the stops to the server together with the gradient. */
	gradient.stops = stops;
	gradient.count = count;
	server->gradient = gradient;
	server->stops = stops;
	server->kind = SVG_SERVER_GRADIENT;

	/* Succeeded: the server holds a gradient and owns its stops. */
	return 0;
}

/* Tells whether a character is one of the white space characters SVG allows in a value. */
static int
pserver_is_space(
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
pserver_skip_space(
	const char *p)
{
	int space;

	/* Moves past every white space character. */
	space = pserver_is_space(*p);
	while (space) {
		p++;
		space = pserver_is_space(*p);
	}

	/* Succeeded: p is at a character that is not white space. */
	return p;
}

/* Tells whether a node is an element of the SVG namespace with a given local name. */
static int
pserver_name_equal(
	const struct svg_tree *tree,
	const struct svg_node *node,
	const char *name)
{
	const char *node_name;
	int different;

	/* Takes the node's name, which is NULL for anything but an SVG element. */
	node_name = tree->name(tree->context, node);
	if (node_name == NULL)
		return 0;

	/* Compares the names. */
	different = strcmp(node_name, name);
	if (different != 0)
		return 0;

	/* Succeeded: the names are the same. */
	return 1;
}

/* Tells whether a node is a linearGradient or a radialGradient element. */
static int
pserver_is_gradient(
	const struct svg_tree *tree,
	const struct svg_node *node)
{
	int linear;
	int radial;

	/* Tells which of the two it is, if any. */
	linear = pserver_name_equal(tree, node, "linearGradient");
	if (linear)
		return 1;
	radial = pserver_name_equal(tree, node, "radialGradient");
	if (radial)
		return 1;

	/* Succeeded: it is neither. */
	return 0;
}

/* Tells whether a chain already holds a node. */
static int
pserver_chain_contains(
	const struct pserver_chain *chain,
	const struct svg_node *node)
{
	size_t index;

	/* Looks for the node among the chain's elements. */
	for (index = 0;
	     index < chain->count;
	     index++) {
		if (chain->nodes[index] == node)
			return 1;
	}

	/* Succeeded: the node is not in the chain. */
	return 0;
}

/*
 * Finds the template an element's href names: an element of the same
 * document that is a linearGradient or a radialGradient (SVG 2 §14.2.2.1
 * and §14.2.3.1).  Reports NULL when there is no href, it is not a plain
 * "#id" reference, or it names nothing that can be a template.
 */
static const struct svg_node *
pserver_template(
	const struct svg_tree *tree,
	const struct svg_node *node)
{
	const struct svg_node *target;
	const char *text;
	const char *cursor;
	char id[SVG_ID_MAX + 1];
	int error;
	int gradient;

	/* Reads the href attribute. */
	text = tree->attribute(tree->context, node, "href");
	if (text == NULL)
		return NULL;

	/* Reads the reference, which must be a local "#id" and the whole of the attribute. */
	cursor = text;
	error = svg_url_read(&cursor, 1, id);
	if (error != 0)
		return NULL;
	cursor = pserver_skip_space(cursor);
	if (*cursor != '\0')
		return NULL;

	/* Finds the element the reference names. */
	target = tree->find_id(tree->context, id);
	if (target == NULL)
		return NULL;

	/* Accepts only a gradient element as a template. */
	gradient = pserver_is_gradient(tree, target);
	if (!gradient)
		return NULL;

	/* Succeeded: target is the template. */
	return target;
}

/*
 * Follows the href templates from an element: the element first, then each
 * template, ending at a missing or invalid reference, at an element the
 * chain already holds (a cycle), or when the chain has SVG_TEMPLATE_DEPTH
 * elements (SVG 2 §14.1.1).
 */
static void
pserver_chain_build(
	const struct svg_tree *tree,
	const struct svg_node *element,
	struct pserver_chain *chain)
{
	const struct svg_node *current;
	const struct svg_node *next;
	int repeated;

	/* Starts the chain with the element itself. */
	chain->nodes[0] = element;
	chain->count = 1;
	current = element;

	/* Adds the template of the last element until there is none or it would repeat. */
	while (chain->count < SVG_TEMPLATE_DEPTH) {
		next = pserver_template(tree, current);
		if (next == NULL)
			break;
		repeated = pserver_chain_contains(chain, next);
		if (repeated)
			break;
		chain->nodes[chain->count] = next;
		chain->count++;
		current = next;
	}
}

/*
 * Finds a length attribute along the chain: the first element (of the
 * given kind of gradient when kind is not NULL) whose attribute is a valid
 * length (not negative when nonnegative is set).  Reports 1 and the
 * length, or 0 when no element of the chain has one.
 */
static int
pserver_find_length(
	const struct svg_tree *tree,
	const struct pserver_chain *chain,
	const char *kind,
	const char *name,
	int nonnegative,
	struct svg_length *length)
{
	struct svg_length parsed;
	const char *text;
	size_t index;
	int of_kind;
	int error;

	/* Looks at each element of the chain, nearest first. */
	for (index = 0;
	     index < chain->count;
	     index++) {
		/* Skips an element of the other kind of gradient. */
		if (kind != NULL) {
			of_kind = pserver_name_equal(tree, chain->nodes[index], kind);
			if (!of_kind)
				continue;
		}

		/* Skips an element that does not have the attribute. */
		text = tree->attribute(tree->context, chain->nodes[index], name);
		if (text == NULL)
			continue;

		/* Skips an invalid length, which counts as not specified. */
		error = svg_parse_length(text, &parsed);
		if (error != 0)
			continue;
		if (nonnegative && parsed.value < 0.0f)
			continue;

		/* Succeeded: this element's length is the attribute's value. */
		*length = parsed;
		return 1;
	}

	/* Succeeded: no element specifies the attribute. */
	return 0;
}

/*
 * Finds which of a list of words a text is: it, after and before white
 * space, equals the word exactly.  Reports the index, or -1.
 */
static int
pserver_word_index(
	const char *text,
	const char *const *words,
	size_t word_count)
{
	const char *start;
	const char *end;
	size_t length;
	size_t index;
	int different;

	/* Takes the text without the white space around it. */
	start = pserver_skip_space(text);
	end = start + strlen(start);
	while (end > start && pserver_is_space(end[-1]))
		end--;
	length = (size_t)(end - start);

	/* Compares it with each word. */
	for (index = 0;
	     index < word_count;
	     index++) {
		if (strlen(words[index]) != length)
			continue;
		different = strncmp(start, words[index], length);
		if (different == 0)
			return (int)index;
	}

	/* Succeeded: the text is none of the words. */
	return -1;
}

/*
 * Finds an enumerated attribute along the chain: the first element whose
 * attribute is one of the words.  Reports the word's index, or -1 when no
 * element of the chain has one.
 */
static int
pserver_find_word(
	const struct svg_tree *tree,
	const struct pserver_chain *chain,
	const char *name,
	const char *const *words,
	size_t word_count)
{
	const char *text;
	size_t index;
	int word;

	/* Looks at each element of the chain, nearest first. */
	for (index = 0;
	     index < chain->count;
	     index++) {
		text = tree->attribute(tree->context, chain->nodes[index], name);
		if (text == NULL)
			continue;

		/* Skips a text that is none of the words, which counts as not specified. */
		word = pserver_word_index(text, words, word_count);
		if (word < 0)
			continue;

		/* Succeeded: this element's word is the attribute's value. */
		return word;
	}

	/* Succeeded: no element specifies the attribute. */
	return -1;
}

/*
 * Finds gradientTransform along the chain: the first element whose
 * attribute is a valid transform list.  The identity when none is.
 */
static void
pserver_find_transform(
	const struct svg_tree *tree,
	const struct pserver_chain *chain,
	struct vec_matrix *matrix)
{
	struct vec_matrix parsed;
	const char *text;
	size_t index;
	int error;

	/* Starts from the identity, the initial value. */
	vec_matrix_identity(matrix);

	/* Looks at each element of the chain, nearest first. */
	for (index = 0;
	     index < chain->count;
	     index++) {
		text = tree->attribute(tree->context, chain->nodes[index], "gradientTransform");
		if (text == NULL)
			continue;

		/* Skips an invalid list, which counts as not specified. */
		vec_matrix_identity(&parsed);
		error = svg_parse_transform(text, &parsed);
		if (error != 0)
			continue;

		/* Takes this element's transform. */
		*matrix = parsed;
		return;
	}
}

/*
 * Resolves a geometry length to a coordinate of gradient space: with
 * bounding box units a percentage over 100 and any other length in user
 * units as it is (the fraction), otherwise in user units with a
 * percentage of the viewport (SVG 2 §8.11, §14.2.2.1, §14.2.3.1).
 */
static float
pserver_length_value(
	const struct svg_length *length,
	enum svg_axis axis,
	int box_units,
	const struct svg_context *context)
{
	float value;

	/* Takes a percentage of the box as a fraction. */
	if (box_units && length->unit == SVG_UNIT_PERCENT) {
		value = length->value / 100.0f;
		if (!vec_finite(value))
			return 0.0f;
		return value;
	}

	/* Resolves any other length in user units. */
	value = svg_length_resolve(length, axis, context);

	/* Succeeded: value is the coordinate. */
	return value;
}

/* Tells whether a geometry value is a number a gradient can use: finite and within the coordinate range. */
static int
pserver_value_usable(
	float value)
{
	/* Refuses a NaN or an infinity. */
	if (!vec_finite(value))
		return 0;

	/* Refuses a value beyond the coordinate range. */
	if (value > VEC_COORD_MAX || value < -VEC_COORD_MAX)
		return 0;

	/* Succeeded: the value is usable. */
	return 1;
}

/*
 * Finds the element the stops come from: the first of the chain that has
 * a stop child (SVG 2 §14.2.4.3 and §14.1.1).  NULL when none has.
 */
static const struct svg_node *
pserver_stops_source(
	const struct svg_tree *tree,
	const struct pserver_chain *chain)
{
	size_t index;
	size_t count;

	/* Looks at each element of the chain, nearest first. */
	for (index = 0;
	     index < chain->count;
	     index++) {
		count = pserver_stop_count(tree, chain->nodes[index]);
		if (count > 0)
			return chain->nodes[index];
	}

	/* Succeeded: no element of the chain has a stop. */
	return NULL;
}

/* Counts the stop children of an element, up to VEC_GRADIENT_STOPS_MAX. */
static size_t
pserver_stop_count(
	const struct svg_tree *tree,
	const struct svg_node *source)
{
	const struct svg_node *child;
	size_t count;
	int is_stop;

	/* Counts the children that are stop elements. */
	count = 0;
	child = tree->first_child(tree->context, source);
	while (child != NULL && count < VEC_GRADIENT_STOPS_MAX) {
		is_stop = pserver_name_equal(tree, child, "stop");
		if (is_stop)
			count++;
		child = tree->next_sibling(tree->context, child);
	}

	/* Succeeded: count stops. */
	return count;
}

/*
 * Makes a stop's straight color from its style: stop-color (currentColor
 * taking the color property) with its alpha multiplied by stop-opacity
 * (SVG 2 §14.2.4.2).
 */
static uint32_t
pserver_stop_color(
	const struct svg_style *style)
{
	uint32_t color;
	uint32_t alpha_byte;
	float alpha;
	float opacity;

	/* Takes the color, resolving currentColor. */
	color = style->stop_color;
	if (style->stop_color_current)
		color = style->color;

	/* Multiplies the color's alpha by the stop's opacity, rounding to the nearest. */
	opacity = style->stop_opacity;
	if (!(opacity > 0.0f))
		opacity = 0.0f;
	if (opacity > 1.0f)
		opacity = 1.0f;
	alpha = (float)(color >> 24) * opacity;
	if (alpha > PSERVER_ALPHA_MAX)
		alpha = PSERVER_ALPHA_MAX;
	alpha_byte = (uint32_t)(alpha + 0.5f);

	/* Succeeded: the color with its new alpha. */
	return (color & 0x00ffffffU) | (alpha_byte << 24);
}

/*
 * Reads a stop's offset: a number or a percentage (SVG 2 §14.2.4.1); an
 * invalid or missing one is the initial value 0.
 */
static float
pserver_stop_offset(
	const struct svg_tree *tree,
	const struct svg_node *stop)
{
	struct svg_length length;
	const char *text;
	float offset;
	int error;

	/* Takes the attribute, 0 when it is missing. */
	text = tree->attribute(tree->context, stop, "offset");
	if (text == NULL)
		return 0.0f;

	/* Takes 0 for a malformed value. */
	error = svg_parse_length(text, &length);
	if (error != 0)
		return 0.0f;

	/* Takes a number as it is and a percentage over 100; any other unit is not an offset. */
	offset = 0.0f;
	if (length.unit == SVG_UNIT_NUMBER)
		offset = length.value;
	else if (length.unit == SVG_UNIT_PERCENT)
		offset = length.value / 100.0f;
	if (!vec_finite(offset))
		return 0.0f;

	/* Succeeded: the offset before its clamping. */
	return offset;
}

/*
 * Reads the stop children of the source element into a new array: each
 * stop's offset, and its color from the style the stop gets inheriting from
 * the gradient element (SVG 2 §14.2.4.2), which is computed from that
 * element's own ancestors.  Reports the array and its count, which is at
 * least 1 here.
 */
static int
pserver_read_stops(
	const struct svg_tree *tree,
	const struct svg_node *source,
	const struct svg_style *outer,
	struct vec_stop **stops,
	size_t *count)
{
	struct svg_style gradient_style;
	struct svg_style stop_style;
	struct vec_stop *array;
	const struct svg_node *child;
	size_t total;
	size_t index;
	int is_stop;
	int error;

	/* Allocates the array for the stops. */
	total = pserver_stop_count(tree, source);
	array = malloc(total * sizeof(*array));
	if (array == NULL)
		return ENOMEM;

	/* Computes the gradient element's style; one that cannot be computed is the initial style. */
	error = svg_style_of(tree, source, outer, &gradient_style);
	if (error != 0)
		svg_style_initial(&gradient_style);

	/* Reads each stop child. */
	index = 0;
	child = tree->first_child(tree->context, source);
	while (child != NULL && index < total) {
		is_stop = pserver_name_equal(tree, child, "stop");
		if (is_stop) {
			svg_style_inherit(&stop_style, &gradient_style);
			svg_style_apply(&stop_style, &gradient_style, tree, child);
			array[index].offset = pserver_stop_offset(tree, child);
			array[index].color = pserver_stop_color(&stop_style);
			array[index].has_offset = 1;
			array[index].is_hint = 0;
			array[index].has_hint = 0;
			array[index].hint = 0.0f;
			index++;
		}
		child = tree->next_sibling(tree->context, child);
	}

	/* Publishes the array. */
	*stops = array;
	*count = index;

	/* Succeeded: the stops are read. */
	return 0;
}

/*
 * Takes the geometry of a linear gradient: x1, y1, x2, y2 (SVG 2
 * §14.2.2.1), from the linearGradient elements of the chain.  Reports 1,
 * or 0 when a value cannot be used.
 */
static int
pserver_linear_geometry(
	const struct svg_tree *tree,
	const struct pserver_chain *chain,
	int box_units,
	const struct svg_context *context,
	struct vec_gradient *gradient)
{
	struct svg_length length;
	float x1;
	float y1;
	float x2;
	float y2;
	int found;

	/* Takes x1, whose initial value is 0%. */
	found = pserver_find_length(tree, chain, "linearGradient", "x1", 0, &length);
	if (!found) {
		length.value = 0.0f;
		length.unit = SVG_UNIT_PERCENT;
	}
	x1 = pserver_length_value(&length, SVG_AXIS_X, box_units, context);

	/* Takes y1, whose initial value is 0%. */
	found = pserver_find_length(tree, chain, "linearGradient", "y1", 0, &length);
	if (!found) {
		length.value = 0.0f;
		length.unit = SVG_UNIT_PERCENT;
	}
	y1 = pserver_length_value(&length, SVG_AXIS_Y, box_units, context);

	/* Takes x2, whose initial value is 100%. */
	found = pserver_find_length(tree, chain, "linearGradient", "x2", 0, &length);
	if (!found) {
		length.value = 100.0f;
		length.unit = SVG_UNIT_PERCENT;
	}
	x2 = pserver_length_value(&length, SVG_AXIS_X, box_units, context);

	/* Takes y2, whose initial value is 0%. */
	found = pserver_find_length(tree, chain, "linearGradient", "y2", 0, &length);
	if (!found) {
		length.value = 0.0f;
		length.unit = SVG_UNIT_PERCENT;
	}
	y2 = pserver_length_value(&length, SVG_AXIS_Y, box_units, context);

	/* Refuses a point that cannot be used. */
	if (!pserver_value_usable(x1) || !pserver_value_usable(y1) || !pserver_value_usable(x2) || !pserver_value_usable(y2))
		return 0;

	/* Publishes the gradient line. */
	gradient->kind = VEC_GRADIENT_LINEAR;
	gradient->start.x = x1;
	gradient->start.y = y1;
	gradient->end.x = x2;
	gradient->end.y = y2;

	/* Succeeded: the geometry is set. */
	return 1;
}

/*
 * Takes the geometry of a radial gradient: the end circle cx, cy, r and
 * the focal circle fx, fy, fr (SVG 2 §14.2.3.1), from the radialGradient
 * elements of the chain; fx and fy are cx and cy where no element has them.
 * Reports 1, or 0 when a value cannot be used.
 */
static int
pserver_radial_geometry(
	const struct svg_tree *tree,
	const struct pserver_chain *chain,
	int box_units,
	const struct svg_context *context,
	struct vec_gradient *gradient)
{
	struct svg_length length;
	float cx;
	float cy;
	float r;
	float fx;
	float fy;
	float fr;
	int found;

	/* Takes cx, whose initial value is 50%. */
	found = pserver_find_length(tree, chain, "radialGradient", "cx", 0, &length);
	if (!found) {
		length.value = 50.0f;
		length.unit = SVG_UNIT_PERCENT;
	}
	cx = pserver_length_value(&length, SVG_AXIS_X, box_units, context);

	/* Takes cy, whose initial value is 50%. */
	found = pserver_find_length(tree, chain, "radialGradient", "cy", 0, &length);
	if (!found) {
		length.value = 50.0f;
		length.unit = SVG_UNIT_PERCENT;
	}
	cy = pserver_length_value(&length, SVG_AXIS_Y, box_units, context);

	/* Takes r, whose initial value is 50%; a negative one is invalid. */
	found = pserver_find_length(tree, chain, "radialGradient", "r", 1, &length);
	if (!found) {
		length.value = 50.0f;
		length.unit = SVG_UNIT_PERCENT;
	}
	r = pserver_length_value(&length, SVG_AXIS_OTHER, box_units, context);

	/* Takes fx, which is cx when no element has it. */
	fx = cx;
	found = pserver_find_length(tree, chain, "radialGradient", "fx", 0, &length);
	if (found)
		fx = pserver_length_value(&length, SVG_AXIS_X, box_units, context);

	/* Takes fy, which is cy when no element has it. */
	fy = cy;
	found = pserver_find_length(tree, chain, "radialGradient", "fy", 0, &length);
	if (found)
		fy = pserver_length_value(&length, SVG_AXIS_Y, box_units, context);

	/* Takes fr, whose initial value is 0%; a negative one is invalid. */
	found = pserver_find_length(tree, chain, "radialGradient", "fr", 1, &length);
	if (!found) {
		length.value = 0.0f;
		length.unit = SVG_UNIT_PERCENT;
	}
	fr = pserver_length_value(&length, SVG_AXIS_OTHER, box_units, context);

	/* Refuses a circle that cannot be used. */
	if (!pserver_value_usable(cx) || !pserver_value_usable(cy) || !pserver_value_usable(r))
		return 0;
	if (!pserver_value_usable(fx) || !pserver_value_usable(fy) || !pserver_value_usable(fr))
		return 0;

	/* Publishes the two circles. */
	gradient->kind = VEC_GRADIENT_RADIAL;
	gradient->center.x = cx;
	gradient->center.y = cy;
	gradient->radius = r;
	gradient->focal.x = fx;
	gradient->focal.y = fy;
	gradient->focal_radius = fr;

	/* Succeeded: the geometry is set. */
	return 1;
}

/*
 * Makes the matrix from the shape's user space to gradient space: the
 * inverse of the chain user space <- bounding box mapping <-
 * gradientTransform <- gradient space (SVG 2 §8.11, §14.2.2.1).  With
 * bounding box units the box maps by translate(left, top) scale(width,
 * height); with user space units the box plays no part.  Reports EINVAL
 * when the matrix cannot be made or inverted.
 */
static int
pserver_to_gradient(
	const struct svg_tree *tree,
	const struct pserver_chain *chain,
	int box_units,
	const struct vec_rect *bbox,
	struct vec_matrix *to_gradient)
{
	struct vec_matrix transform;
	struct vec_matrix mapping;
	struct vec_matrix to_user;
	int valid;
	int error;

	/* Takes gradientTransform. */
	pserver_find_transform(tree, chain, &transform);

	/* Composes the gradient space's way into user space: gradientTransform, then the box mapping. */
	to_user = transform;
	if (box_units) {
		mapping.a = bbox->right - bbox->left;
		mapping.b = 0.0f;
		mapping.c = 0.0f;
		mapping.d = bbox->bottom - bbox->top;
		mapping.e = bbox->left;
		mapping.f = bbox->top;
		vec_matrix_multiply(&to_user, &transform, &mapping);
	}

	/* Refuses a matrix with numbers that cannot be used. */
	valid = vec_matrix_valid(&to_user);
	if (!valid)
		return EINVAL;

	/* Inverts it; a matrix without an inverse paints nothing. */
	error = vec_matrix_invert(to_gradient, &to_user);
	if (error != 0)
		return EINVAL;

	/* Succeeded: to_gradient maps user space into gradient space. */
	return 0;
}
